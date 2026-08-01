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

bool placesMayAlias(OptContext& opt, const Place& a, const Place& b) {
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
    if(first.root == PlaceRoot::Pointer || first.root == PlaceRoot::Borrow) {
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
};

struct Forwarder {
    OptContext& opt;
    SmallArray<Known, 16> known;

    // Per local, whether a callee could reach its storage - see `computeContainment`. Indexed by
    // local, and empty until one function has been walked.
    IndexSet contained;

    // Per local, whether nothing in this function ever computed its address - see `pointerSafe`,
    // which is the only thing that reads it.
    IndexSet unaddressed;

    void forget() { known.clear(); }

    // Everything an instruction this pass cannot see through may have written. Not everything: a
    // local whose address was never handed out is storage no callee has a way to name, and keeping
    // it is what lets a record built out of parameters survive the call in front of the read of it.
    void forgetExposed() {
        for(Size i = known.size(); i-- > 0;) {
            if(!staysInFrame(opt, contained, known[i].place)) known.remove(i);
        }
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

        for(Size i = known.size(); i-- > 0;) {
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

            default:
                return !isPureValue(instruction);
        }
    }

    bool forwardable(TypePtr type) { return holdsLoadableValue(opt, type); }

    ModulePtr<Value> knownValue(Place& place) {
        for(Size i = known.size(); i-- > 0;) {
            if(samePlace(opt, known[i].place, place)) return known[i].value;
        }

        return nullptr;
    }

    // One entry per piece of storage, so that "what is known about this place" and "which store put
    // it there" are one answer rather than the most recent of several.
    void remember(Place& place, ModulePtr<Value> value, ModulePtr<Inst> pending) {
        for(Size i = known.size(); i-- > 0;) {
            if(samePlace(opt, known[i].place, place)) known.remove(i);
        }

        known.push(Known { place, value, pending });
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
        auto remaining = opt.local[loaded]->uses.size();
        if(!remaining) return false;

        for(Size i = index + 1; i < block.instructions.size(); i++) {
            auto pointer = block.instructions.get(opt.local, i);
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
     */
    bool eliminateOverwritten(Place& place) {
        for(Size i = known.size(); i-- > 0;) {
            if(!samePlace(opt, known[i].place, place) || !known[i].pending) continue;

            auto pending = known[i].pending;
            known[i].pending = nullptr;
            eraseInstruction(opt, pending);
            return true;
        }

        return false;
    }

    void run(Block& block) {
        forget();

        for(Size i = 0; i < block.instructions.size(); i++) {
            auto pointer = block.instructions.get(opt.local, i);
            auto instruction = opt.local[pointer];

            switch(instruction->kind) {
                case Value::LoadPlace: {
                    auto& load = (InstLoadPlace&)*instruction;

                    // A load that is answered from a value already in hand is not a read of
                    // anything: it stops being an access at all, so the store it would have read
                    // stays removable. That is the case every read-modify-write chain is made of.
                    if(forwardable(load.type)) {
                        if(auto value = knownValue(load.place)) {
                            replaceValue(opt, (ModulePtr<Value>)pointer, value);
                            break;
                        }
                    }

                    // And the memory case, which is answered with the other name for the storage
                    // rather than with its contents - see `sharedStorage`.
                    if(!forwardable(load.type)) {
                        auto value = sharedStorage(block, i, (ModulePtr<Value>)pointer, load.place,
                                                   load.type);
                        if(value) {
                            replaceValue(opt, (ModulePtr<Value>)pointer, value);
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

                    if(forwardable(type) && instruction->uses.isEmpty()) {
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
                            eraseInstruction(opt, pointer);
                            i--;
                            break;
                        }
                    }

                    // The store it replaces came out of the block in front of this one, so the walk
                    // has to step back over the gap it left.
                    if(forwardable(type) && eliminateOverwritten(store.place)) i--;

                    forgetAliasing(store.place);
                    if(forwardable(type)) remember(store.place, store.value, pointer);
                    publishNiche(store.place, *instruction);

                    /*
                     * And the aggregate case, which is a *read* of the source as well as a write of
                     * the destination.
                     *
                     * The read half is not optional. A whole-value write reads every byte of what it
                     * copies, and nothing above records that - so a store into the source that
                     * nothing else read was removable by `eliminateOverwritten` even though this
                     * copy had just read it. Marking it here is what makes the entries inherited
                     * below rest on stores that are still there.
                     */
                    if(type && isMemoryType(opt.global, type)) {
                        if(auto source = storageOf(opt, store.value)) {
                            markRead(source.unwrap());

                            if(opt.local[store.value]->kind != Value::Move &&
                               source.unwrap().root == PlaceRoot::Local &&
                               source.unwrap().local < unaddressed.size() &&
                               unaddressed[source.unwrap().local]) {
                                inheritCopy(store.place, source.unwrap());

                                // And the whole of it, which `inheritCopy` deliberately skips: the
                                // fact is not what the storage contains but that two names now
                                // reach the same contents, which is what `sharedStorage` reads.
                                remember(store.place, store.value, nullptr);

                                for(auto& stored: known) {
                                    if(!samePlace(opt, stored.place, store.place)) continue;

                                    stored.alias = source.unwrap();
                                    stored.aliased = true;
                                }
                            }
                        }
                    }

                    break;
                }
                default:
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
            for(auto user: opt.local[slot.value]->uses.contents(opt.local)) {
                switch(opt.local[user]->kind) {
                    case Value::Init: case Value::Assign:
                    case Value::LoadPlace: case Value::Copy:
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
