#include "opt_pass.h"
#include "../resolve/expr.h"

/*
 * Read/write combining over places.
 *
 * Three rewrites, all of them straight-line within one block:
 *
 *  - a load of a place something just wrote becomes the value it wrote;
 *  - a second load of a place nothing has written since becomes the first load;
 *  - a store of exactly the value a place is already known to hold goes away;
 *  - a store nothing reads before the same place is written again goes away.
 *
 * The last two are the ones the read-modify-write chains need. A packed field's write is `read the word,
 * replace the field's bits, write it back`, and a record built out of literals is that chain once per
 * field over a word that started at a constant - so forwarding the read makes the arithmetic
 * foldable, folding it makes the write-back a store of what is already there, and the third rule is
 * what actually removes it. None of them is a peephole on its own.
 *
 * ## Why this is a place pass rather than a memory pass
 *
 * Because at this altitude the answer to "can these two accesses touch the same storage" is
 * structural. A `Place` is a local plus a path of projections, `x.a` and `x.b` are different fields
 * of the same local by construction, and the borrow checker has already proved that two live `&`s of
 * overlapping paths do not coexist. Below the fork the same question is pointer disambiguation over
 * `add ptr, imm`, and on the JS tree it is reasoning about property names on values anything may
 * alias. This is the one stage where the ownership model pays for itself as an optimization.
 *
 * Two consequences worth stating, because they are why the packing expansion has to run *above* this
 * rather than below it:
 *
 *  - co-packed fields do not alias here. `h.version` and `h.length` share a machine word and are two
 *    different projections, so a write of one does not kill a read of the other - which is exactly
 *    Design.md's rule that the two are independent, restated as an optimization. Below the fork they
 *    are the same word and the information is gone.
 *  - a *whole-record* store still kills every field of it, because a shorter path is a prefix of the
 *    longer one and `mayAlias` says so.
 *
 * ## What is deliberately not done
 *
 * Nothing crosses a block boundary. That needs an availability dataflow and a way to place the
 * result, which is opt_promote.cpp - and the chains *this* pass exists for are straight-line, so the
 * two are worth keeping apart: this one asks nothing about the shape of the function and runs over
 * anything, while that one needs a local a callee cannot reach before it may say a word.
 *
 * Nothing crosses anything that may write storage this pass cannot see either - see `clobbers`,
 * whose default is to forget everything.
 *
 * The three questions about *storage* - do these two places overlap, are they the same one, is this
 * a value a load answers with - are asked by both passes and answered here, because the reasoning
 * behind each of them is the reasoning above.
 */

/*
 * Whether a sum's tag and its payload are two pieces of storage - the one case where two projections
 * of *different* kinds at the same position separate rather than overlap.
 *
 * `Word` is the layout where they do: `layoutRecord` in repr/repr.cpp places a discriminant word in
 * front of the payload and the payload at `payloadOffset` after it, so a write of one cannot reach
 * the other. That is what makes `init %m.discriminant, 1` survive `init %m@Just, 40` - which the
 * `Just(40)` in every constructor call is, and until this was here the discriminant was forgotten by
 * the payload write immediately below it and every read of it stayed a load.
 *
 * `Niche` is the case this must *not* say yes to, and the reason the question is asked of the target
 * rather than of the path: a niche-folded discriminant is a pattern of the payload's own storage,
 * so the two really are one place and the payload write really is what publishes the tag. See
 * `publishNiche`, which is that same fact read the other way round.
 *
 * `Bits` is excluded as well, though `layoutRecord` does place the tag past the widest payload. What
 * is not established is that it stays there through the IR: opt_pack.cpp rewrites a packed field
 * into a read-modify-write of the *word*, and a word-sized write reaches everything in it. A bit-
 * tagged sum whose payload is co-packed with the tag would be that write, so this declines rather
 * than resting on a rule the pass below it could break.
 */
static bool separateTagAndPayload(OptContext& opt, const Place& place, Size index,
                                  ProjectionKind left, ProjectionKind right) {
    auto pair = (left == ProjectionKind::Discriminant && right == ProjectionKind::Downcast) ||
                (left == ProjectionKind::Downcast && right == ProjectionKind::Discriminant);
    if(!pair || !opt.function) return false;

    auto owner = placeType(*opt.module, *opt.function, place, index);
    if(!owner) return false;

    return opt.repr.of(owner).discriminant == DiscriminantKind::Word;
}

bool pathsMayOverlap(OptContext& opt, const Place& first, const Place& second) {
    // `get` is a read that the list spells as a mutation, which is why every walk of a projection
    // path in this directory casts the constness off rather than taking a copy of the path.
    auto& a = const_cast<Place&>(first).projections;
    auto& b = const_cast<Place&>(second).projections;
    auto count = min(a.size(), b.size());

    for(Size i = 0; i < count; i++) {
        auto left = a.get(opt.local, i);
        auto right = b.get(opt.local, i);

        // Everything in front of this position was the same projection on both paths, so either
        // place answers for the type this one is taken of.
        if(left.kind != right.kind) {
            return !separateTagAndPayload(opt, first, i, left.kind, right.kind);
        }

        switch(left.kind) {
            case ProjectionKind::Field:
            case ProjectionKind::Property:
                // Two different fields of one aggregate are two different pieces of storage. That is
                // true here even where the target co-packs them into one word, which is the whole
                // reason this pass runs before the packing is expanded.
                if(left.index != right.index) return false;
                break;
            case ProjectionKind::Discriminant:
                break;
            case ProjectionKind::Unit:
                /*
                 * The word two co-packed fields became. Reached only where both paths agreed on the
                 * field in front of it, which is the whole reason opt_pack.cpp canonicalizes that
                 * index: `h.version` and `h.length` are two fields and one word, and it is the
                 * *field* indices above that the rule below is entitled to separate.
                 *
                 * Two different unit widths at one position would be two readings of one place,
                 * which nothing produces - so this compares nothing and lets the walk continue.
                 */
                break;
            case ProjectionKind::Downcast:
                /*
                 * Two constructors of one sum overlap: their payloads share the storage, and on both
                 * targets deliberately so. Only one is live at a time, but "which one" is a fact
                 * about the discriminant rather than about the path, so this declines to separate
                 * them.
                 */
                if(left.index != right.index) return true;
                break;
            case ProjectionKind::Index: {
                // Two constant indices are two elements; anything else may be the same one.
                auto leftIndex = constantValueOf(opt, left.value);
                auto rightIndex = constantValueOf(opt, right.value);
                if(!leftIndex || !rightIndex) return true;
                if(leftIndex.unwrap() != rightIndex.unwrap()) return false;
                break;
            }
            case ProjectionKind::Deref:
                // Through a pointer, and this pass knows nothing about what it points at.
                return true;
        }
    }

    return true;
}

/*
 * A pointer value as a base and a constant byte displacement from it.
 *
 * `p + i` on a `%T` is `add p, i * strideof T`, so an element address is this shape and nothing
 * else: one root, and arithmetic that either folds to a number or does not. The walk stops at the
 * first operand that is not a constant and answers with whatever it stopped on, which makes the
 * base an SSA value rather than an allocation - and value identity is the whole of what the two
 * comparisons below need, since two names for one address are one value after CSE.
 *
 * This is the disambiguation the file header says belongs *below* the fork. It is here because the
 * shape it needs arrived: `strideof` used to be an unfoldable question at this altitude, so an
 * element address was opaque arithmetic and there was nothing to compare - see `foldMetric` in
 * opt_fold.cpp, which is what made a stride a number one stage before either backend.
 */
struct AddressTerm {
    ModulePtr<Value> base = nullptr;
    I64 offset = 0;
};

static AddressTerm addressTerm(OptContext& opt, ModulePtr<Value> pointer) {
    AddressTerm term { pointer, 0 };
    if(!pointer) return term;

    // A bound rather than a termination proof - operands in SSA cannot cycle, so this ends on its
    // own. What the cap stops is a long chain costing more to walk than the fact is worth.
    for(Size step = 0; step < 8; step++) {
        auto& value = *opt.local[term.base];
        if(value.kind != Value::Add && value.kind != Value::Sub) break;

        // The result has to be the address, not an integer that later becomes one: a cast in
        // between is a reinterpretation this pass has no rule for.
        if(!isPointer(opt.global, value.type)) break;

        auto& binary = (InstBinary&)value;

        if(auto rhs = constantValueOf(opt, binary.rhs)) {
            term.offset += value.kind == Value::Add ? I64(rhs.unwrap()) : -I64(rhs.unwrap());
            term.base = binary.lhs;
            continue;
        }

        // `k + p`, which only addition has - a constant minus a pointer is not an address.
        if(value.kind != Value::Add) break;

        auto lhs = constantValueOf(opt, binary.lhs);
        if(!lhs) break;

        term.offset += I64(lhs.unwrap());
        term.base = binary.rhs;
    }

    return term;
}

// Whether two pointer values are the same address, which is either the same value or the same
// displacement from one.
static bool sameAddress(OptContext& opt, ModulePtr<Value> first, ModulePtr<Value> second) {
    if(first && first == second) return true;

    auto left = addressTerm(opt, first);
    auto right = addressTerm(opt, second);

    return left.base && left.base == right.base && left.offset == right.offset;
}

/*
 * Whether a place rooted at a pointer stays inside the one value that pointer names.
 *
 * The displacement comparison below measures an access as the pointee type's own extent, and that
 * is only the whole of what the place reaches while every projection lands *within* the pointee. A
 * field, a downcast, a discriminant and a packed word all do. An `Index` does not - it is how a
 * host element is a place (`hostElement` in resolve/host.cpp), and `[p][3]` is three elements past
 * the extent this would have measured - and a `Deref` leaves the value entirely.
 *
 * Those two are not lost: they are compared by `pathsMayOverlap`, which already separates two
 * constant indices, and reaching it is what the equal-displacement case below does.
 */
static bool insidePointee(OptContext& opt, const Place& place) {
    auto& projections = const_cast<Place&>(place).projections;

    for(Size i = 0; i < projections.size(); i++) {
        switch(projections.get(opt.local, i).kind) {
            case ProjectionKind::Field:
            case ProjectionKind::Property:
            case ProjectionKind::Downcast:
            case ProjectionKind::Discriminant:
            case ProjectionKind::Unit:
                break;
            default:
                return false;
        }
    }

    return true;
}

// How many bytes an access through this pointer touches, or zero where that is not a number this
// target has - a generic pointee is measured out of a descriptor and an opaque one is not measured.
static U32 accessExtent(OptContext& opt, ModulePtr<Value> pointer) {
    if(!pointer) return 0;

    auto type = pointeeType(opt.global, opt.local[pointer]->type);
    if(!type || isGeneric(opt.global, type)) return 0;

    auto& repr = opt.repr.of(type);
    return repr.opaque ? 0 : repr.size;
}

/*
 * Whether two address bases are storage that cannot be one piece.
 *
 * Deliberately not provenance, which this pass declines: each case below is a *structural*
 * guarantee that two instructions made two things, and each is a fact some other rule in this file
 * already rests on.
 *
 *  - the address of two different **locals**, which is `placesMayAlias`'s own rule for a `Local`
 *    root reached one indirection later. Two slots are two pieces of storage, and taking the
 *    address of each does not make them one;
 *  - two different **allocations**, which `clobbers` already calls "storage that has just come into
 *    existence with nothing in it" - so the second cannot be the first;
 *  - two different **host array literals**, which is that same statement on the managed target: a
 *    `[…]` evaluates to an array nothing else holds.
 *
 * Only ever between two of a kind. An `Alloc` and an `Address` may perfectly well be the same
 * storage - the address of the slot the allocation filled is exactly that - and mixing them is the
 * one way this could be read as saying more than it does.
 */
static bool basesSeparate(OptContext& opt, ModulePtr<Value> first, ModulePtr<Value> second) {
    if(!first || !second || first == second) return false;

    auto& left = *opt.local[first];
    auto& right = *opt.local[second];
    if(left.kind != right.kind) return false;

    switch(left.kind) {
        case Value::Address: {
            auto& a = ((InstAddress&)left).place;
            auto& b = ((InstAddress&)right).place;

            return a.root == PlaceRoot::Local && b.root == PlaceRoot::Local && a.local != b.local;
        }
        case Value::Alloc:
            return true;
        case Value::Native:
            return ((InstNative&)left).op == NativeOp::HostArray &&
                   ((InstNative&)right).op == NativeOp::HostArray;
        default:
            return false;
    }
}

/*
 * Two pointer-rooted places that provably do not touch each other.
 *
 * Either two roots that are not one piece of storage, or one root and two byte ranges that do not
 * meet. The second is the array literal's whole question: `[10, 20, 30]` writes three elements
 * through one buffer pointer at three displacements, and without it each write forgets the two in
 * front of it and no element is ever known.
 *
 * Two *unrelated* pointers are still not separated. Which of those name the same storage is
 * provenance rather than shape - the question this pass has always declined, and the one
 * opt_promote.cpp asks of a local it has already proved contained.
 */
static bool addressesSeparate(OptContext& opt, const Place& a, const Place& b) {
    auto left = addressTerm(opt, a.pointer);
    auto right = addressTerm(opt, b.pointer);

    // Independent of the displacements and of the paths: no offset from one of these reaches the
    // other, so neither place has to stay inside its pointee for this to hold.
    if(basesSeparate(opt, left.base, right.base)) return true;

    if(!insidePointee(opt, a) || !insidePointee(opt, b)) return false;
    if(!left.base || left.base != right.base || left.offset == right.offset) return false;

    auto leftExtent = accessExtent(opt, a.pointer);
    auto rightExtent = accessExtent(opt, b.pointer);
    if(!leftExtent || !rightExtent) return false;

    return left.offset + I64(leftExtent) <= right.offset ||
           right.offset + I64(rightExtent) <= left.offset;
}

bool placesMayAlias(OptContext& opt, const Place& a, const Place& b) {
    /*
     * Two raw pointers, which are separable exactly as far as arithmetic over one base goes - see
     * `addressesSeparate`. Where they are the same address the paths decide, which is what makes
     * two elements of one host array two pieces of storage: there the displacement is zero on both
     * sides and the element is an `Index` projection.
     */
    if(a.root == PlaceRoot::Pointer && b.root == PlaceRoot::Pointer) {
        if(addressesSeparate(opt, a, b)) return false;
        if(sameAddress(opt, a.pointer, b.pointer)) return pathsMayOverlap(opt, a, b);

        return true;
    }

    // A raw pointer may name anything at all, and a borrow may name anything the borrow checker let
    // it be taken of - which is a question about provenance rather than about the place, and one
    // this pass does not ask. opt_promote.cpp does ask it, of a local it has already proved contained.
    if(a.root == PlaceRoot::Pointer || b.root == PlaceRoot::Pointer) return true;
    if(a.root == PlaceRoot::Borrow || b.root == PlaceRoot::Borrow) return true;

    if(a.root != b.root) return false;
    if(a.root == PlaceRoot::Local && a.local != b.local) return false;
    if(a.root == PlaceRoot::Global && a.global != b.global) return false;

    return pathsMayOverlap(opt, a, b);
}

bool samePlace(OptContext& opt, const Place& first, const Place& second) {
    if(first.root != second.root) return false;
    if(first.root == PlaceRoot::Local && first.local != second.local) return false;
    if(first.root == PlaceRoot::Global && first.global != second.global) return false;
    if(first.root == PlaceRoot::Pointer) {
        // Two computations of one address, which is the same place however many times the program
        // spelled it. `xs[0] = 10` and the read of `xs[0]` below it are two `add base, 0`s, and
        // before this they were two pointers this pass had no way to call equal.
        if(!sameAddress(opt, first.pointer, second.pointer)) return false;
    } else if(first.root == PlaceRoot::Borrow) {
        if(first.pointer != second.pointer) return false;
    }

    auto& a = const_cast<Place&>(first).projections;
    auto& b = const_cast<Place&>(second).projections;
    if(a.size() != b.size()) return false;

    for(Size i = 0; i < a.size(); i++) {
        auto left = a.get(opt.local, i);
        auto right = b.get(opt.local, i);

        if(left.kind != right.kind || left.index != right.index) return false;
        if(!left.value && !right.value) continue;

        if(left.value == right.value) continue;

        auto leftIndex = constantValueOf(opt, left.value);
        auto rightIndex = constantValueOf(opt, right.value);
        if(!leftIndex || !rightIndex || leftIndex.unwrap() != rightIndex.unwrap()) return false;
    }

    return true;
}

bool holdsLoadableValue(OptContext& opt, TypePtr type) {
    return type && !isUnit(opt.global, type) && !isMemoryType(opt.global, type);
}

namespace {

// What one place is known to hold. A store and a load establish the same fact and are not kept
// apart: "this place holds %v" is what both a later load and a later store of %v rest on.
struct Known {
    Place place;
    ModulePtr<Value> value = nullptr;

    /*
     * The store that put it there, while nothing has read it back.
     *
     * Null once something has, and null for a fact a *load* established - that is storage which
     * already held the value, and there is no write to remove. Overwriting a place whose entry still
     * names a store is what makes that store dead: nothing between the two could have seen it, or
     * this would have been cleared.
     */
    ModulePtr<Inst> pending = nullptr;

    /*
     * Where the fact came from, for one inherited across an aggregate copy - see `inheritCopy`.
     *
     * The two places hold the same thing because a copy put it in both, so a write to *either* ends
     * that and this is the half `place` does not cover. It matters on a managed target rather than
     * natively, and there for a structural reason: `storeInto` in codegen/js writes an aggregate by
     * assigning the reference, so the destination and the source are one object and a write through
     * the source's name is a write to the destination's storage.
     */
    Place alias;
    bool aliased = false;

    /*
     * Which element of an `InstAggregate` this is, where it is one.
     *
     * `pending` holds the aggregate for these rather than a store, because that is the instruction a
     * later write is folded *into* - see `foldIntoAggregate`. The two are told apart by this being
     * set, and they share `pending` on purpose: what makes a store removable and what makes an
     * element rewritable are the same fact, that nothing has read the slot since, and `markRead`
     * clears it for both without knowing there are two.
     */
    U32 slot = maxLimit<U32>;
};

/*
 * How many elements a host array is known to have.
 *
 * The managed target's counterpart to a run's `length` field, and it needs an entry of its own for
 * the reason it is not one: `Array(a)` *is* the host array there, its length is a property of the
 * object rather than storage the program wrote, and the read of it is a `NativeOp::HostField`
 * instead of a load of a place. So `known` has nothing to key it on, while the fact itself is the
 * ordinary one - a literal writes its elements and their count is how many it wrote.
 *
 * `length` is what the writes this walk has seen add up to, which is the whole of what JavaScript
 * says: an array starts empty and `a[k] = v` leaves it exactly `k + 1` long where it was shorter,
 * whatever was or was not written in between. Anything that could make it longer without this walk
 * seeing the index ends the entry rather than adjusting it.
 *
 * What it buys is the bounds check on a literal. `[7, 8, 9][0]` compares `0` against a length no
 * pass could answer, so the check stayed, and a call the place pass cannot see through - which the
 * check was, before `clobbers` learned this one - is what stopped the elements folding behind it.
 */
struct HostArrayLength {
    ModulePtr<Value> array;
    U64 length;
};

struct Forwarder {
    OptContext& opt;
    SmallArray<Known, 16> known;

    // The host arrays created in this block, and how long each is known to be - see
    // HostArrayLength. Always empty on a native target, where nothing emits a `hostarray` at all.
    SmallArray<HostArrayLength, 4> arrays;

    // Per local, whether a callee could reach its storage - see `computeContainment`. Indexed by
    // local, and empty until one function has been walked.
    IndexSet contained;

    // Per local, whether nothing in this function ever computed its address - see `pointerSafe`,
    // which is the only thing that reads it.
    IndexSet unaddressed;

    void forget() {
        known.clear();
        arrays.clear();
    }

    // Everything an instruction this pass cannot see through may have written. Not everything: a
    // local whose address was never handed out is storage no callee has a way to name, and keeping
    // it is what lets a record built out of parameters survive the call in front of the read of it.
    void forgetExposed() {
        for(Size i = known.size(); i-- > 0;) {
            if(!staysInFrame(opt, contained, known[i].place)) known.remove(i);
        }

        // And every length, with no such exemption: a host array is reached through a value rather
        // than through a local, so `contained` has nothing to say about one and anything handed the
        // value may have pushed onto it.
        arrays.clear();
    }

    /*
     * Whether a *raw pointer* could be pointing into this place.
     *
     * `placesMayAlias` declines to say anything at all about a pointer root, so a write through one
     * forgets the whole table - which is right for storage the program computed an address for and
     * wrong for storage it did not. An address is computed by exactly two instructions, `Address` and
     * `Borrow`, so a local whose allocation is used by neither has no pointer anywhere in the
     * function that could name it.
     *
     * That is `unaddressed` rather than `contained`, and the difference is the clause that matters
     * here: containment also refuses a local used as an *operand*, which being copied out of is. A
     * copy reads the bytes and computes no address, so it says nothing about pointers - and the run
     * behind every array literal is a local whose whole purpose is to be copied out of.
     *
     * It is what makes an array literal survive its own elements. `[10, 20, 30]` writes the run's
     * fields, then the three elements through a raw pointer into the buffer, and only then assembles
     * the array - so without this the placement tag is forgotten before anything is built out of it,
     * and Implementation-Containers.md §13.2's fold never sees a constant.
     *
     * A *borrow* root is deliberately not covered. On a managed target an aggregate copy assigns the
     * reference, so a borrow of something the local was copied *into* reaches the local's storage
     * without ever having been a borrow of it - and the list above does not see that. Raw pointers
     * have no such route, which is why the two roots part company here.
     */
    bool pointerSafe(const Place& place) {
        if(place.root != PlaceRoot::Local) return false;
        if(place.local >= unaddressed.size() || !unaddressed[place.local]) return false;

        auto& projections = const_cast<Place&>(place).projections;
        for(Size i = 0; i < projections.size(); i++) {
            switch(projections.get(opt.local, i).kind) {
                case ProjectionKind::Field:
                case ProjectionKind::Downcast:
                case ProjectionKind::Discriminant:
                case ProjectionKind::Unit:
                    break;
                default:
                    return false;
            }
        }

        return true;
    }

    // A write through a place, and everything it may have landed on.
    void forgetAliasing(Place& place) {
        auto viaPointer = place.root == PlaceRoot::Pointer;

        /*
         * The same rule read from the other end: a write to a local no pointer in this function can
         * name cannot reach anything a pointer *does* name.
         *
         * `pointerSafe` says a raw pointer has no route to this storage, and that is one statement
         * about two directions. Only one of them was being used, and the missing one costs exactly
         * what the used one buys: `[10, 20, 30]` writes its elements through the buffer pointer and
         * then writes the array's own `length`, and without this that last ordinary field write
         * forgets all three elements one instruction before anything reads them.
         */
        auto safeTarget = !viaPointer && pointerSafe(place);

        for(Size i = known.size(); i-- > 0;) {
            if(safeTarget && known[i].place.root == PlaceRoot::Pointer &&
               (!known[i].aliased || known[i].alias.root == PlaceRoot::Pointer)) {
                continue;
            }

            if(viaPointer && pointerSafe(known[i].place) &&
               (!known[i].aliased || pointerSafe(known[i].alias))) {
                continue;
            }

            if(placesMayAlias(opt, known[i].place, place)) {
                known.remove(i);
            } else if(known[i].aliased && placesMayAlias(opt, known[i].alias, place)) {
                known.remove(i);
            }
        }
    }

    /*
     * Whether an instruction may write storage this pass is tracking, or hand out a way to.
     *
     * The default is yes. An instruction kind this does not recognize is one whose effect on storage
     * has not been checked, and forgetting too much costs an optimization while forgetting too
     * little is a miscompile - which is the same asymmetry `borrowStaysHere` in codegen/js/gen.cpp
     * resolves the same way.
     */
    bool clobbers(Value& instruction) {
        switch(instruction.kind) {
            // Pure computation, and storage that has just come into existence with nothing in it.
            case Value::Alloc:
                return false;

            // Reads. `Copy` reads its place and writes storage of its own that nothing else names
            // yet, and `Borrow` of an immutable place cannot be written through.
            case Value::LoadPlace:
            case Value::Copy:
                return false;

            case Value::Borrow:
                return ((InstBorrow&)instruction).mut;

            // Handled by the caller, which knows which place they name.
            case Value::Init:
            case Value::Assign:
                return false;

            /*
             * The check the compiler inserts at a subscript - see Program::checkCondition.
             *
             * Its one argument is a `Bool` passed by value, so there is no storage of the caller's
             * it has a way to name: it reads the flag and either returns having done nothing or
             * does not return at all. A pass that forgot the table here would be undoing the shape
             * §15 asked for - the check is a *call* precisely so that the block it is emitted into
             * stays one block, and forgetting at it costs exactly what splitting it would have.
             *
             * That is a statement about this callee and no other. Every other call does whatever
             * its callee does, which this pass has never had a way to ask.
             */
            case Value::Call:
                return !isCheckCall(opt, ((InstCall&)instruction).callee);

            /*
             * The two host operations that write nothing. Everything else a `Native` can be - a
             * method call on a host value, an allocation, a copy between two addresses - is left to
             * the default, which is that it may have written anything.
             */
            case Value::Native:
                switch(((InstNative&)instruction).op) {
                    /*
                     * A host property read - `xs.length`, which is the whole of NativeOp::HostField
                     * (see `HostMember`: both intrinsics that produce one read a length). It
                     * computes no address and writes nothing.
                     *
                     * Not the same claim as `isPureValue`, which still declines it: what a property
                     * answers changes when something writes the value it belongs to, exactly as a
                     * load's answer does. So it may not be recomputed anywhere - only relied on not
                     * to be a writer.
                     */
                    case NativeOp::HostField:
                    // And an empty array, which is storage that has just come into existence with
                    // nothing in it - the managed target's `Alloc`, and no more of a writer.
                    case NativeOp::HostArray:
                        return false;
                    default:
                        return true;
                }

            default:
                return !isPureValue(instruction);
        }
    }

    bool forwardable(TypePtr type) { return holdsLoadableValue(opt, type); }

    /*
     * The same storage, however the two places spell it - `samePlace` widened by `sameElement`.
     *
     * Every question in this pass that asks "is this the place I have a fact about" goes through
     * here, and the four are the same question: what a load may be answered with, which entry a
     * store replaces, which store an overwrite kills, and which element a literal write folds into.
     * The *aliasing* question is not one of them and is deliberately left alone - `placesMayAlias`
     * stays conservative, so a read it cannot separate still clears the fact either way.
     */
    bool sameStorage(const Place& first, const Place& second) {
        return samePlace(opt, first, second) || sameElement(first, second);
    }

    ModulePtr<Value> knownValue(Place& place) {
        for(Size i = known.size(); i-- > 0;) {
            if(sameStorage(known[i].place, place)) return known[i].value;
        }

        return nullptr;
    }

    // One entry per piece of storage, so that "what is known about this place" and "which store put
    // it there" are one answer rather than the most recent of several.
    void remember(Place& place, ModulePtr<Value> value, ModulePtr<Inst> pending,
                  U32 slot = maxLimit<U32>) {
        for(Size i = known.size(); i-- > 0;) {
            if(sameStorage(known[i].place, place)) known.remove(i);
        }

        known.push(Known { place, value, pending, Place(), false, slot });
    }

    /*
     * Whether a value is available at an instruction, which for this pass is a question about one
     * block: a value defined in another one dominates every use here, or the IR would not verify.
     * A constant belongs to no block at all and is materialized per function, so it is available
     * everywhere and falls out of the same test.
     */
    bool definedBefore(ModulePtr<Inst> at, ModulePtr<Value> value) {
        auto& definition = *opt.local[value];

        // Available everywhere: a constant belongs to no block and is materialized per function,
        // and a parameter arrives ahead of the first instruction.
        if(definition.kind == Value::Arg || isConstant(definition)) return true;

        /*
         * Everything else has to be defined in this block, and that is stricter than domination on
         * purpose. The usual rule - "a definition dominates every use, so a value from another
         * block is available here" - is about uses that *exist*. This creates a new one, at a
         * position above the old one, and a definition in a block this one dominates reaches
         * neither. Getting that wrong emitted `var v5 = [1, v, 3]` above `var v = ...`, which is a
         * hole in the array rather than a diagnostic.
         *
         * A value in a genuinely dominating block would be legal to use and is declined here: that
         * needs `opt.dominance`, which this pass does not compute, for a case the literals in front
         * of it do not reach.
         */
        auto block = opt.local[at]->block;
        if(definition.block != block) return false;

        // A phi is defined at the top of its block, so it precedes every instruction in it.
        if(definition.kind == Value::Phi) return true;

        for(auto pointer: opt.local[block]->instructions(opt.local)) {
            if(pointer == at) return false;
            if((ModulePtr<Value>)pointer == value) return true;
        }

        return false;
    }

    /*
     * Whether two places name the same element, where one of them may be spelled as an `Index`
     * projection and the other as arithmetic on the base.
     *
     * A sharper question than `samePlace`, asked here rather than there on purpose. The two
     * spellings are both in the program and neither is going away: a literal names its elements by
     * index, because that is the form a target with no addresses can also expand, while `xs[i]`
     * goes through `get`/`getMut` in Collections and comes out as `store(items + i, x)`. Teaching
     * the general equality test to see through both would widen a primitive every rewrite in this
     * file rests on; teaching *one fold* to is a claim that can only make this fold fire.
     *
     * Sound on the same terms `sameAddress` is - one base and one displacement, with the index
     * spent into the displacement at the pointee's own stride - plus the extents matching, since
     * one address is two places when two different widths are read through it.
     */
    bool sameElement(const Place& first, const Place& second) {
        if(first.root != PlaceRoot::Pointer || second.root != PlaceRoot::Pointer) return false;
        if(accessExtent(opt, first.pointer) == 0) return false;
        if(accessExtent(opt, first.pointer) != accessExtent(opt, second.pointer)) return false;

        // Each side reduces to a base, a byte displacement, and whatever path is left over. Only an
        // element is being matched here, so anything left over is a different place.
        auto reduce = [&](const Place& place, AddressTerm& term) {
            term = addressTerm(opt, place.pointer);

            auto& projections = const_cast<Place&>(place).projections;
            if(projections.isEmpty()) return true;
            if(projections.size() != 1) return false;

            auto step = projections.get(opt.local, 0);
            if(step.kind != ProjectionKind::Index) return false;

            auto index = constantValueOf(opt, step.value);
            if(!index) return false;

            // Stride rather than size: elements are spaced by what the next one starts at, and for
            // a type whose size is not a multiple of its alignment those are different numbers.
            auto element = pointeeType(opt.global, opt.local[place.pointer]->type);
            if(!element) return false;

            term.offset += I64(index.unwrap()) * I64(opt.repr.of(element).stride);
            return true;
        };

        AddressTerm left;
        AddressTerm right;
        if(!reduce(first, left) || !reduce(second, right)) return false;

        return left.base && left.base == right.base && left.offset == right.offset;
    }

    /*
     * A write into an element of a literal, folded back into the literal.
     *
     * `let &xs = [1, 2, 3, 4]` followed by `xs[1] = 20` is one array built once, and this is what
     * says so: the element becomes `20` and the store goes away. It replaces what
     * `eliminateOverwritten` used to do to the same program by deleting the literal's *own* element
     * instead - which removed no store at all, since the later write still ran, and on a managed
     * target left the array to be built with a gap and patched. A guard there declined that; this
     * is the rewrite it was standing in for, and deleting it is what retired the guard.
     *
     * Three conditions, and each is a way the fold would change what the program does:
     *
     *  - **nothing has read the slot since**, which is the entry still naming its aggregate.
     *    `markRead` and every clobber clear it, so a load, a borrow, or a call in between all stop
     *    this - and so does the `InstDrop` the drop pass emits in front of an assignment that
     *    replaces an owned value, which is what keeps a teardown from being skipped;
     *  - **the value is available where the literal is**, since the *store* moves earlier even
     *    though nothing else does. A value this block computes after the literal cannot go into it;
     *  - **the element owes no teardown**, which `forwardable` almost gives - it excludes memory
     *    types - and `needsTeardown` finishes for a scalar with a `Drop` instance of its own.
     */
    bool foldIntoAggregate(Place& place, ModulePtr<Value> value) {
        for(Size i = known.size(); i-- > 0;) {
            auto& entry = known[i];
            if(entry.slot == maxLimit<U32> || !entry.pending) continue;
            if(!sameElement(entry.place, place)) continue;

            auto instruction = opt.local[entry.pending];
            if(instruction->kind != Value::Aggregate) return false;
            if(!definedBefore(entry.pending, value)) return false;

            auto& aggregate = (InstAggregate&)*instruction;
            auto component = aggregate.components.get(opt.local, entry.slot);
            auto previous = component.value;
            if(previous == value) return false;
            if(needsTeardown(*opt.module, opt.local[previous]->type)) return false;

            opt.ir().rewriteOperands(entry.pending, [&](Value&) {
                component.value = value;
                aggregate.components.set(opt.local, entry.slot, component);
            });

            entry.value = value;
            opt.changed = true;
            return true;
        }

        return false;
    }

    // Which element a write names, where that is a number this pass can put down - see
    // `noteHostWrite`, which is the only thing that asks and the reason a non-constant index is an
    // answer of its own rather than a zero.
    Maybe<U64> writtenIndex(const Place& place) {
        auto& projections = const_cast<Place&>(place).projections;
        if(projections.isEmpty()) return Nothing();

        auto first = projections.get(opt.local, 0);
        if(first.kind != ProjectionKind::Index) return Nothing();

        return constantValueOf(opt, first.value);
    }

    /*
     * A write through a place, read for what it does to a host array's length.
     *
     * Only a write *into* an array can change one. A local holding the reference is not the array -
     * assigning one somewhere else copies the reference and leaves the object exactly as long as it
     * was - so the roots that matter are the two that name the array itself.
     *
     * Two ways to lose the fact, and they are the same statement about what this walk can see:
     *
     *  - an **index it cannot put a number on**, which may be past the end. That is how a JavaScript
     *    array grows, and it is what `push` compiles to here - `xs[xs.length] = v`;
     *  - a write it **cannot separate from the array**, which includes every write through a borrow.
     *    Two names for one array are one array, and `basesSeparate` is the only thing entitled to
     *    say two host arrays are not.
     */
    void noteHostWrite(const Place& place) {
        if(arrays.isEmpty()) return;
        if(place.root != PlaceRoot::Pointer && place.root != PlaceRoot::Borrow) return;

        auto index = writtenIndex(place);

        for(Size i = arrays.size(); i-- > 0;) {
            if(basesSeparate(opt, place.pointer, arrays[i].array)) continue;

            if(place.pointer != arrays[i].array || !index) {
                arrays.remove(i);
                continue;
            }

            auto reached = index.unwrap() + 1;
            if(reached > arrays[i].length) arrays[i].length = reached;
        }
    }

    /*
     * The two host operations this pass has an opinion about, which are the two halves of one fact.
     *
     * A `hostarray` is an array that has just come into existence with nothing in it - the same
     * statement `clobbers` makes about an `Alloc`, and the starting point every element write above
     * counts up from. A `HostField` is `xs.length` and nothing else, so where the array is one this
     * block built the read has an answer and stops being a read.
     *
     * The constant belongs to the block the read is in, which is what `makeConstant` places it in.
     * What removes the `hostarray` afterwards is the ordinary path: nothing reads it, and the
     * literal it was built from goes with it.
     */
    void noteHostOp(ModulePtr<Inst> pointer, InstNative& instruction) {
        if(instruction.op == NativeOp::HostArray) {
            arrays.push(HostArrayLength { (ModulePtr<Value>)pointer, 0 });
            return;
        }

        if(instruction.op != NativeOp::HostField || instruction.args.size() != 1) return;

        auto array = instruction.args.get(opt.local, 0);
        for(auto& entry: arrays) {
            if(entry.array != array) continue;

            auto constant = makeConstant(opt, instruction, instruction.type, entry.length);
            opt.ir().replaceValue((ModulePtr<Value>)pointer, constant);
            return;
        }
    }

    /*
     * Where `place`'s path continues past `prefix`, or nothing where it does not begin with it.
     *
     * `samePlace`'s comparison stopped at the shorter of the two, which is the one question a
     * *prefix* asks - so this is that function with its length test moved rather than a second
     * opinion about when two projections are the same one.
     */
    Maybe<Size> pathBeyond(const Place& prefix, const Place& place) {
        if(prefix.root != place.root) return Nothing();
        if(prefix.root == PlaceRoot::Local && prefix.local != place.local) return Nothing();
        if(prefix.root == PlaceRoot::Global && prefix.global != place.global) return Nothing();
        if(prefix.root == PlaceRoot::Pointer || prefix.root == PlaceRoot::Borrow) {
            if(prefix.pointer != place.pointer) return Nothing();
        }

        auto& a = const_cast<Place&>(prefix).projections;
        auto& b = const_cast<Place&>(place).projections;
        if(a.size() > b.size()) return Nothing();

        for(Size i = 0; i < a.size(); i++) {
            auto left = a.get(opt.local, i);
            auto right = b.get(opt.local, i);

            if(left.kind != right.kind || left.index != right.index) return Nothing();
            if(left.value == right.value) continue;

            auto leftIndex = constantValueOf(opt, left.value);
            auto rightIndex = constantValueOf(opt, right.value);
            if(!leftIndex || !rightIndex || leftIndex.unwrap() != rightIndex.unwrap()) return Nothing();
        }

        return Just(a.size());
    }

    // One place with another's path hung off the end of it.
    Place extend(const Place& base, const Place& from, Size beyond) {
        Place result;
        result.root = base.root;
        result.local = base.local;
        result.global = base.global;
        result.pointer = base.pointer;

        auto& head = const_cast<Place&>(base).projections;
        for(Size i = 0; i < head.size(); i++) {
            result.projections.push(opt.program.arena, head.get(opt.local, i));
        }

        auto& tail = const_cast<Place&>(from).projections;
        for(Size i = beyond; i < tail.size(); i++) {
            result.projections.push(opt.program.arena, tail.get(opt.local, i));
        }

        return result;
    }

    /*
     * What a whole-aggregate write copies along with the bytes.
     *
     * `let &xs = [1, 2, 3]` builds the run in one temporary, the array in a second, and copies the
     * second into `xs` - so the placement tag escape analysis patched into the run's own allocation
     * is two whole-value writes away from every read of `xs.run.capacity`, and none of the rules
     * above connects them. The write is not forwardable (its value is storage rather than something a
     * load answers with) and splitting it into one write per field is opt_scalar.cpp's rule, which
     * declines here twice over - the type owes a teardown, and the field is packed.
     *
     * So neither side of the copy is rewritten and the *facts* cross it instead: everything known
     * about a place inside the source is, immediately afterwards, equally true of the same path
     * inside the destination. Implementation-Containers.md §13.2's third step is what wanted it, and
     * a frame-placed array's teardown folding to nothing at all is what it buys.
     *
     * Three conditions, and each rules out one way of the copy not being a copy:
     *
     *  - **the value is not a `Move`.** Which of Design-Memory §4.1's two relocations a write
     *    performs is decided by the value rather than by the type - see `relocate` in lower.cpp - and
     *    only a move runs a `Sink`. Everything else is a block copy natively and a reference
     *    assignment on JS, and the facts survive both;
     *  - **the source is a place**, since otherwise there is nothing for the paths to be relative to;
     *  - **nothing took the source local's address**, which is what lets the inherited entries
     *    outlive a call. `unaddressed` is the same list `pointerSafe` reads, for a second
     *    consequence of it: a callee is given storage by being passed it, being passed it is a
     *    `Call` operand, and a `Call` operand is not one of the four kinds. So whatever else holds
     *    this value holds it *through* one of these copies - and the destination's own containment,
     *    which `forgetExposed` checks, is exactly the question of who those are.
     *
     * The entries carry the source place as `alias`, so that a later write through *it* ends them
     * too. That is the half `place` cannot cover on a target where the two names are one object.
     */
    void inheritCopy(Place& destination, const Place& source) {
        // Over a snapshot, because `remember` pushes onto the list this is reading.
        SmallArray<Known, 8> inherited;

        for(auto& entry: known) {
            auto beyond = pathBeyond(source, entry.place);
            if(!beyond) continue;

            // The whole of the source, which is the value being copied rather than something inside
            // it - and a memory type is never a value a load answers with, so there is nothing to
            // inherit and `remember` would be recording the storage as its own contents.
            if(beyond.unwrap() == entry.place.projections.size()) continue;

            inherited.push(Known {
                extend(destination, entry.place, beyond.unwrap()), entry.value, nullptr,
                entry.place, true
            });
        }

        for(auto& entry: inherited) remember(entry.place, entry.value, nullptr);

        // The alias is set afterwards rather than through `remember`, which takes only what a store
        // establishes. Matched by place, since that is what `remember` just keyed the entry on.
        for(auto& entry: inherited) {
            for(auto& stored: known) {
                if(!samePlace(opt, stored.place, entry.place)) continue;

                stored.alias = entry.alias;
                stored.aliased = true;
            }
        }
    }

    /*
     * A write of a whole value, which is a *read* of the source as well as a write of the destination.
     *
     * The read half is not optional. A whole-value write reads every byte of what it copies, and
     * nothing else records that - so a store into the source that nothing else read was removable by
     * `eliminateOverwritten` even though this copy had just read it. Marking it here is what makes
     * the entries `inheritCopy` sets up rest on stores that are still there.
     *
     * Shared with the aggregate case, which is where a construction's whole-value components arrive:
     * `Boxed {tag: 7, held: someMaybe}` hands over a sum this frame just built, and without this the
     * reader of `held` was no longer handed the source and the record it was built to be taken apart
     * into stopped disappearing. Both callers reach it with one component's place and one value,
     * which is all it ever needed.
     */
    void noteCopy(Place& destination, ModulePtr<Value> value) {
        auto type = opt.local[value]->type;
        if(!type || !isMemoryType(opt.global, type)) return;

        auto source = storageOf(opt, value);
        if(!source) return;

        markRead(source.unwrap());

        if(opt.local[value]->kind == Value::Move) return;
        if(source.unwrap().root != PlaceRoot::Local) return;
        if(source.unwrap().local >= unaddressed.size() || !unaddressed[source.unwrap().local]) return;

        inheritCopy(destination, source.unwrap());

        // And the whole of it, which `inheritCopy` deliberately skips: the fact is not what the
        // storage contains but that two names now reach the same contents, which is what
        // `sharedStorage` reads.
        remember(destination, value, nullptr);

        for(auto& stored: known) {
            if(!samePlace(opt, stored.place, destination)) continue;

            stored.alias = source.unwrap();
            stored.aliased = true;
        }
    }

    /*
     * The discriminant a payload write establishes, where the two are one piece of storage.
     *
     * `separateTagAndPayload` is the layout question and this is the same answer read from the other
     * end. Under `Niche` the tag is not stored: one constructor keeps the payload untouched and every
     * other is a pattern outside the payload type's valid range - so writing that constructor's
     * payload *is* how the tag comes to say so, and the value written cannot fail to mean it because
     * every inhabitant of the payload type is inside the range the patterns were taken from outside
     * of.
     *
     * Which makes forgetting the discriminant here exactly half an answer. The write does end
     * whatever the tag was known to be, and `forgetAliasing` is right to say so; what it cannot say
     * is what the tag became, and without that a niche-folded sum built out of literals is a
     * constructor call followed by a load of a discriminant nobody has to read. `Maybe(&T)` is a
     * plain nullable pointer on every target and `Maybe(Int)` is one on JS, so this is the ordinary
     * case there rather than a corner of it.
     *
     * Only a write of the *whole* payload, which is what the path ending in the downcast means. A
     * write one level further in - a field of the payload - leaves storage this pass has no rule
     * about, since the value that decides the constructor is then only partly written.
     */
    void publishNiche(Place& place, Value& at) {
        auto& projections = place.projections;
        if(projections.isEmpty()) return;

        auto last = projections.get(opt.local, projections.size() - 1);
        if(last.kind != ProjectionKind::Downcast || !opt.function) return;

        auto owner = placeType(*opt.module, *opt.function, place, projections.size() - 1);
        if(!owner) return;

        auto& repr = opt.repr.of(owner);
        if(!repr.isNicheFolded() || repr.encoding.payloadConstructor != last.index) return;

        Place tag;
        tag.root = place.root;
        tag.local = place.local;
        tag.global = place.global;
        tag.pointer = place.pointer;

        for(Size i = 0; i + 1 < projections.size(); i++) {
            tag.projections.push(opt.program.arena, projections.get(opt.local, i));
        }

        tag.projections.push(opt.program.arena, Projection { ProjectionKind::Discriminant, 0 });

        // A fact rather than a store: nothing wrote the tag, so there is no pending instruction for
        // a later overwrite of it to remove.
        remember(tag, makeConstant(opt, at, opt.program.scalar.int_, last.index), nullptr);
    }

    /*
     * Whether a value of a memory type may be read out of one of two places holding it rather than
     * the other.
     *
     * A load of a memory type does not answer with contents - there is no register that would hold
     * them - so what it produces is the storage itself, and `forwardable` declines it for that
     * reason. But two places that a copy has just made equal are two answers to the same question,
     * and where the storage is only *read* it does not matter which of them a reader is handed.
     *
     * That is what removes a record built to be taken apart. `Labelled {label: 2, contents: Just(40)}`
     * inlined into a caller that wants `contents` is a two-field aggregate written, one field read
     * back and the rest discarded - which on a managed target is a whole object allocated for one
     * property. Handing the reader the storage the field was copied *from* leaves the aggregate with
     * nothing but writes, and `eliminateDeadLocal` takes it from there.
     *
     * The conditions are what keep "equal here" from being read as "equal there":
     *
     *  - **no teardown.** With one, which storage a reader was handed stops being invisible: a
     *    `Move` out of it, a `Drop` of it, a `->` parameter consuming it are all decisions about
     *    *that* storage, and this would be quietly moving them to the other one. The same gate
     *    `eliminateDeadLocal` uses, for the same reason;
     *  - **every use is a read**, checked against the instruction rather than inferred from the
     *    type, since the list above is exactly the one that would be wrong;
     *  - **every use is in this block, and nothing between here and the last of them writes either
     *    place.** This is the condition the rest of the pass does not need: a *value* forwarded out
     *    of a place is the contents, and stays the answer however the place changes afterwards,
     *    while this hands over the place - so it has to hold until the last reader has read it, not
     *    just until here. Natively the two are separate storage and a write to one diverges from the
     *    other; on a managed target `storeInto` made them one object, and there they cannot.
     */
    bool readOnlyUse(Value& user, ModulePtr<Value> loaded) {
        switch(user.kind) {
            case Value::Call:
            case Value::CallDyn:
            case Value::GenCall:
            case Value::Ret:
                return true;

            // A copy of it into somewhere else, which reads it - but not a write *through* it, which
            // would be this value as the destination's root rather than as the source.
            case Value::Init:
            case Value::Assign:
                return ((InstInit&)user).value == loaded;

            default:
                return false;
        }
    }

    bool sharedStorageSurvives(Block& block, Size index, ModulePtr<Value> loaded,
                               const Place& place, const Known& entry) {
        auto remaining = opt.local[loaded]->useCount();
        if(!remaining) return false;

        for(Size i = index + 1; i < block.instructionCount(); i++) {
            auto pointer = block.instructionAt(opt.local, i);
            auto instruction = opt.local[pointer];

            bool uses = false;
            eachOperand(opt.local, *instruction, [&](ModulePtr<Value> operand) {
                if(operand == loaded) uses = true;
            });

            if(uses) {
                if(!readOnlyUse(*instruction, loaded)) return false;
                if(--remaining == 0) return true;
            }

            if(instruction->kind == Value::Init || instruction->kind == Value::Assign) {
                auto& store = (InstInit&)*instruction;
                if(placesMayAlias(opt, store.place, const_cast<Place&>(place))) return false;
                if(placesMayAlias(opt, store.place, const_cast<Known&>(entry).alias)) return false;
                continue;
            }

            if(clobbers(*instruction)) return false;
        }

        // A use in another block, which this walk cannot say anything about.
        return false;
    }

    ModulePtr<Value> sharedStorage(Block& block, Size index, ModulePtr<Value> loaded, Place& place,
                                   TypePtr type) {
        if(!type || !isMemoryType(opt.global, type)) return nullptr;
        if(needsTeardown(*opt.module, type)) return nullptr;

        for(Size i = known.size(); i-- > 0;) {
            auto& entry = known[i];
            if(!entry.aliased || !entry.value || !samePlace(opt, entry.place, place)) continue;
            if(opt.local[entry.value]->type != type) continue;
            if(!sharedStorageSurvives(block, index, loaded, place, entry)) continue;

            return entry.value;
        }

        return nullptr;
    }

    // Everything this access may have seen, which is no longer a write nobody read. Reads are marked
    // rather than forgotten: the *value* is still known, and it is only the store that stops being
    // removable.
    void markRead(Place& place) {
        for(auto& entry: known) {
            if(placesMayAlias(opt, entry.place, place)) entry.pending = nullptr;
        }
    }

    void markRead(Value& instruction) {
        eachPlace(instruction, [&](const Place& place) { markRead(const_cast<Place&>(place)); });
    }

    /*
     * The store that is about to be overwritten, if nothing has read it.
     *
     * Within one block and one place, which is what makes it sound without an availability analysis:
     * the entry survives only while nothing aliasing it was read and nothing this pass cannot see
     * ran, and the overwrite is total because `samePlace` compares the whole path - including a
     * unit projection's width, so half a word is never mistaken for all of it.
     *
     * A component of an aggregate is not one of these, and the entry for one says so by naming an
     * instruction this declines to erase. It used to be - a literal was `n` stores and this removed
     * the ones a later write killed, which on a managed target left the array to be built with a gap
     * and patched, so a guard here declined it. `InstAggregate` retired that argument and replaced it
     * with a simpler one: the instruction writes the *other* components too, so erasing it for the
     * sake of one of them deletes stores that are still live. Folding the later write into the
     * aggregate is `foldIntoAggregate` above, and where that declines there is nothing here to
     * remove.
     *
     * The entry still carries the aggregate rather than a null, because `foldIntoAggregate` is how
     * it finds the instruction to rewrite. `Settings {flags: ..., count: 0}` followed by
     * `s.count = s.count + 1` is the shape: the fold declines - the sum is computed after the
     * literal - and this then deleted the whole construction, taking the unrelated `flags` with it.
     */
    bool eliminateOverwritten(Place& place) {
        for(Size i = known.size(); i-- > 0;) {
            if(!sameStorage(known[i].place, place) || !known[i].pending) continue;
            if(opt.local[known[i].pending]->kind == Value::Aggregate) continue;

            auto pending = known[i].pending;
            known[i].pending = nullptr;
            opt.ir().eraseInstruction(pending);
            return true;
        }

        return false;
    }

    void run(Block& block) {
        forget();

        for(Size i = 0; i < block.instructionCount(); i++) {
            auto pointer = block.instructionAt(opt.local, i);
            auto instruction = opt.local[pointer];

            switch(instruction->kind) {
                case Value::LoadPlace: {
                    auto& load = (InstLoadPlace&)*instruction;

                    // A load that is answered from a value already in hand is not a read of
                    // anything: it stops being an access at all, so the store it would have read
                    // stays removable. That is the case every read-modify-write chain is made of.
                    if(forwardable(load.type)) {
                        if(auto value = knownValue(load.place)) {
                            opt.ir().replaceValue((ModulePtr<Value>)pointer, value);
                            break;
                        }
                    }

                    // And the memory case, which is answered with the other name for the storage
                    // rather than with its contents - see `sharedStorage`.
                    if(!forwardable(load.type)) {
                        auto value = sharedStorage(block, i, (ModulePtr<Value>)pointer, load.place,
                                                   load.type);
                        if(value) {
                            opt.ir().replaceValue((ModulePtr<Value>)pointer, value);
                            break;
                        }
                    }

                    markRead(load.place);
                    if(forwardable(load.type)) remember(load.place, (ModulePtr<Value>)pointer, nullptr);
                    break;
                }
                case Value::Init:
                case Value::Assign: {
                    auto& store = (InstInit&)*instruction;
                    auto type = opt.local[store.value]->type;

                    if(forwardable(type) && instruction->useCount() == 0) {
                        /*
                         * A write of what is already there.
                         *
                         * Sound because an assignment is only a write by the time this runs -
                         * whatever the old value's teardown owed has already been emitted as its own
                         * `InstDrop` by the drop pass, so there is nothing in an `Assign` but the
                         * store (see the Init/Assign case in resolve/lower.cpp). And the value's own
                         * ownership is untouched, because the place is being left holding the value
                         * it was already holding.
                         */
                        if(knownValue(store.place) == store.value) {
                            opt.ir().eraseInstruction(pointer);
                            i--;
                            break;
                        }
                    }

                    /*
                     * An element of a literal, written back into the literal - and the store is
                     * this instruction rather than an earlier one, so what goes away is this.
                     * Ahead of the dead-store rule because the two answer the same shape and only
                     * one of them is right for it: there is no separate store to erase.
                     */
                    if(forwardable(type) && foldIntoAggregate(store.place, store.value)) {
                        opt.ir().eraseInstruction(pointer);
                        i--;
                        break;
                    }

                    // The store it replaces came out of the block in front of this one, so the walk
                    // has to step back over the gap it left.
                    if(forwardable(type) && eliminateOverwritten(store.place)) i--;

                    forgetAliasing(store.place);
                    noteHostWrite(store.place);
                    if(forwardable(type)) remember(store.place, store.value, pointer);
                    publishNiche(store.place, *instruction);

                    noteCopy(store.place, store.value);
                    break;
                }
                /*
                 * A duplicate, whose destination is storage of its own.
                 *
                 * `clobbers` already says what this is - "reads its place and writes storage of its
                 * own that nothing else names yet" - and this is the consequence of the second half.
                 * The bytes at the copy's own local are the bytes at the source, so everything known
                 * about a place inside the source is, immediately afterwards, equally true of the
                 * same path inside the destination. Exactly `noteCopy`'s rule with the destination
                 * arrived at differently: there a whole value is *written into* a place, and here
                 * the instruction's own result is where it lands.
                 *
                 * It is the shape a `->` argument reaches an inlined callee in. `matching(Just(u), u)`
                 * in Unit.yana is `init %m.discriminant, 1`, a copy of `%m` for the argument, and -
                 * once the body is inlined - the match's tag test reading the *copy*. Without this
                 * the constant stops at the copy, so `foldFunction` never sees `cmp_eq 1, 1` and
                 * `foldBranches` never sees a branch it can take: both backends emitted a diamond
                 * whose two arms are the same and whose condition is a tautology.
                 *
                 * **A bitwise duplicate only.** An authored `Copy` runs a user function to build the
                 * result, and what that function put there is not what the source holds - see
                 * InstCopy::copy, which is null for exactly the case this may claim.
                 */
                case Value::Copy: {
                    auto& duplicate = (InstCopy&)*instruction;
                    markRead(duplicate.place);

                    if(duplicate.copy || duplicate.local == maxLimit<U32>) break;

                    // The same two guards `noteCopy` applies to a source, for the same reasons: the
                    // paths have to be relative to something, and nothing may hold another way in.
                    if(duplicate.place.root != PlaceRoot::Local) break;
                    if(duplicate.place.local >= unaddressed.size()) break;
                    if(!unaddressed[duplicate.place.local]) break;

                    auto destination = Place::inLocal(duplicate.local);
                    forgetAliasing(destination);
                    inheritCopy(destination, duplicate.place);
                    break;
                }
                /*
                 * Every element of a literal, on the same terms as the stores it replaced.
                 *
                 * The three facts each element carried are all still true and all still wanted:
                 * the write forgets what aliased that slot, it is what a host array's length counts
                 * (`Array(a)` is the host array here, so `[1, 2, 3].length` is *three writes* rather
                 * than a field), and the value is known at the slot afterwards so a constant
                 * subscript folds. Without this an array literal became opaque - `ConstIndex`
                 * stopped folding and every bounds check on a literal stopped discharging.
                 *
                 * The entry names this instruction as its pending store, which `eliminateOverwritten`
                 * declines to erase for the reason stated there - a component is not separately
                 * removable. It is carried anyway because `foldIntoAggregate` is the rewrite that
                 * answers an overwritten component, and finding the instruction is what it needs.
                 *
                 * `noteCopy` is the fourth fact, and it belongs to the field form: a component that
                 * is a whole value read out of a local makes the two names equal, exactly as the
                 * `Init` it replaced did.
                 */
                case Value::Aggregate: {
                    auto& aggregate = (InstAggregate&)*instruction;

                    eachWrittenComponent(opt.local, opt.module->arena, aggregate,
                                         [&](Place element, ModulePtr<Value> value, Size at) {
                        forgetAliasing(element);
                        noteHostWrite(element);
                        if(forwardable(opt.local[value]->type)) {
                            remember(element, value, pointer, U32(at));
                        }

                        noteCopy(element, value);

                        // And the tag a niche-folded payload publishes, which only the sum form
                        // reaches: a component's step is a `Downcast` exactly when the payload is
                        // being written whole, and that write *is* how the discriminant comes to say
                        // so. Without it a `Just(x)` built as one instruction lost the fact, and
                        // every `?.` over one went back to testing the payload against null.
                        publishNiche(element, *instruction);
                    });

                    break;
                }
                default:
                    // Ahead of the clobber test rather than in a case of its own, because the two
                    // are not alternatives: `hostarray` is a host operation *and* an instruction
                    // the rules below still have to be applied to. See `noteHostOp`.
                    if(instruction->kind == Value::Native) {
                        noteHostOp(pointer, (InstNative&)*instruction);
                    }

                    if(clobbers(*instruction)) {
                        forgetExposed();

                        /*
                         * And the places the instruction names itself, which `forgetExposed` is
                         * exactly the wrong rule for: a `Move` out of a contained local, or a
                         * `Swap` of two of them, writes storage a callee could not have reached and
                         * this pass still has to forget. Conservative on a read - a place slot here
                         * is not known to be written - and it costs nothing, since an instruction
                         * that got this far is one the pass declined to model anyway.
                         */
                        eachPlace(*instruction, [&](const Place& place) {
                            forgetAliasing(const_cast<Place&>(place));
                        });

                        /*
                         * And the storage it was *handed*, which `forgetExposed` is now deliberately
                         * the wrong rule for either.
                         *
                         * `computeContainment` admits an unretained call argument, so a record
                         * passed to `==` stays contained and its facts survive every other call in
                         * the function - which is the whole point. What it does not survive is this
                         * call, because the callee holds the storage for as long as it runs and may
                         * write it. Forgetting here is what pays for admitting it there: exposure
                         * that ends is exposure the pass has to end somewhere.
                         */
                        eachHandedLocal(opt, *instruction, [&](U32 local) {
                            auto place = Place::inLocal(local);
                            forgetAliasing(place);
                        });
                    } else {
                        markRead(*instruction);
                    }
                    break;
            }
        }
    }
};

}

/*
 * Per local, whether anything in this function computed its address.
 *
 * `Address` and `Borrow` are the two instructions that do, so this is every other use being one of
 * the four that read or write the storage in place. Deliberately *not* `computeContainment`, which
 * asks a stronger question for a different consumer: it also refuses a local used as an *operand*,
 * and being copied out of is one. A copy reads the bytes and computes no address.
 *
 * Two passes read it and the second is not about pointers at all - see `pointerSafe` and
 * `inheritCopy`, which take two different consequences of the same list.
 */
static void computeUnaddressed(OptContext& opt, IndexSet& unaddressed) {
    unaddressed.reset(opt.function->localCount());

    for(U32 i = 0; i < opt.function->localCount(); i++) {
        auto slot = opt.function->localAt(opt.local, i);
        auto ok = slot.value && opt.local[slot.value]->kind == Value::Alloc &&
                  !slot.borrowed && !slot.closureEnv;

        if(ok) {
            for(auto user: opt.local[slot.value]->uses(opt.local)) {
                switch(opt.local[user]->kind) {
                    case Value::Init: case Value::Assign:
                    case Value::LoadPlace: case Value::Copy:
                    // The stores a construction is, said once - see InstAggregate. It names a place
                    // and hands out no address, which is the whole question here.
                    case Value::Aggregate:
                        break;
                    default:
                        ok = false;
                }

                if(!ok) break;
            }
        }

        unaddressed.set(i, ok);
    }
}

void forwardPlaces(OptContext& opt) {
    Forwarder forwarder { opt };
    computeContainment(opt, forwarder.contained);
    computeUnaddressed(opt, forwarder.unaddressed);

    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        forwarder.run(*opt.local[blockPointer]);
    }
}
