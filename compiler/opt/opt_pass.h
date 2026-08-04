#pragma once

#include "opt.h"
#include "../resolve/builder.h"
#include "../resolve/place.h"

/*
 * What the passes share: the state one function is optimized against, and the handful of IR
 * operations that are the same in all of them.
 *
 * Every rewrite here is a use-list rewrite. The resolve IR keeps both directions - an instruction
 * names its operands and every value names its users - and a pass that updates one without the
 * other leaves an IR that prints correctly and walks wrongly, which is the failure mode worth
 * spending a header on preventing.
 */

/*
 * The dominator tree of one function - see opt_flow.cpp, which computes all three of these.
 *
 * `dominators[i][j]` is whether block `j` dominates block `i`, which includes `i` itself.
 * `preorder` is a visit order in which a block precedes everything it dominates, and is what a pass
 * that *moves* an instruction needs: laying candidates out in that order lays a definition out
 * before the use that depends on it, with no dependency walk of its own.
 */
struct Dominance {
    // One row per block, holding the blocks that dominate it. A list rather than an array of
    // arrays so that the rows survive the function they were computed for - see computeDominance,
    // which both of its callers reach once per round.
    IndexSetList dominators;

    Array<U32> immediate;

    // The dominator tree's edges, one row per block. An ArrayList rather than an array of arrays
    // for the reason that container documents: a row is emptied rather than destroyed between
    // functions, and a row index no earlier function reached starts inline instead of allocating.
    // A block dominates a handful of others immediately, so eight covers the ordinary one.
    ArrayList<U32, 8> children;

    Array<U32> preorder;
    Array<ModulePtr<Block>> blocks;

    static constexpr U32 kNone = maxLimit<U32>;
};

struct OptContext {
    Context& context;
    Program& program;
    GlobalBase global;
    ModuleBase local;
    ReprTable& repr;

    Module* module = nullptr;
    Function* function = nullptr;

    /*
     * The buffers the passes work in, which belong to the stage rather than to one function.
     *
     * There is one OptContext per program and one call of each pass per function per round, so a
     * set built inside a pass is built a few thousand times over a compilation and holds a handful
     * of bits each time. `sets` hands out the per-pass ones by scope - see ScratchSet - and the
     * dominator tree is named because two passes ask for it and neither wants the other's.
     */
    IndexSetPool sets;
    Dominance dominance;

    // Set by any rewrite. The driver runs the passes to a fixed point over one function, because
    // folding exposes identities and identities expose more folding.
    bool changed = false;
};

/*
 * The operands of one instruction, in the order `Block::add` records uses in.
 *
 * `f` is handed each operand and answers what it should become, which is the one shape that serves
 * a field and a list element alike - a `ModuleList` element is reached through `get`/`set` and
 * there is no reference to hand out. Returning the operand unchanged is the read-only use.
 *
 * This has to name exactly what `Block::add` names. An operand it misses is one a replacement walks
 * past, leaving a use of a value that is no longer defined; an operand it invents is a use count
 * that never balances.
 */
template<class F>
void mapOperands(ModuleBase base, Value& instruction, F&& f) {
    auto place = [&](Place& p) {
        if(p.root == PlaceRoot::Pointer || p.root == PlaceRoot::Borrow) p.pointer = f(p.pointer);

        for(Size i = 0; i < p.projections.size(); i++) {
            auto projection = p.projections.get(base, i);
            if(!projection.value) continue;

            projection.value = f(projection.value);
            p.projections.set(base, i, projection);
        }
    };

    auto list = [&](ModuleList<ModulePtr<Value>, false>& values) {
        for(Size i = 0; i < values.size(); i++) values.set(base, i, f(values.get(base, i)));
    };

    Place* places[kMaxPlaces];
    auto placeCount = instructionPlaceSlots(instruction, places);
    for(Size i = 0; i < placeCount; i++) place(*places[i]);

    switch(instruction.kind) {
        /*
         * How many slots a run holds - InstAlloc::extent, which every pass here had been blind to.
         *
         * It is an operand in every sense that matters: `Block::add` records it as a use, and a
         * rewrite that renumbers values has to renumber it. Leaving it out of this walk meant the
         * dead-value pass saw the instruction computing it with no users and deleted it, and the
         * allocation was then left naming a value no block defined - which lowering reports as
         * "resolve value was used before it was lowered".
         *
         * The reason nothing caught it is that every run until now got its extent from an array
         * literal, where the count is a `ConstInt`. A constant belongs to no block and is
         * materialized per function on demand, so it cannot be deleted and needs no remapping - the
         * hole was real from the day `extent` was added and unreachable until something passed a
         * *computed* count. `newStringOfCapacity` is the first thing that does.
         *
         * `storageFlag` is deliberately not here for exactly that reason: it is always the constant
         * the escape analysis patched, so it is never in a block and never at risk. Adding it would
         * be describing a use that does not exist.
         */
        case Value::Alloc: {
            auto& allocation = (InstAlloc&)instruction;
            if(allocation.extent) allocation.extent = f(allocation.extent);
            break;
        }
        case Value::Init:
        case Value::Assign: {
            auto& init = (InstInit&)instruction;
            init.value = f(init.value);
            break;
        }
        case Value::Exchange: {
            auto& exchange = (InstExchange&)instruction;
            exchange.value = f(exchange.value);
            break;
        }
        case Value::Native:
            list(((InstNative&)instruction).args);
            break;
        case Value::Cast:
        case Value::Neg:
        case Value::Not: {
            auto& unary = (InstUnary&)instruction;
            unary.from = f(unary.from);
            break;
        }
        case Value::Add: case Value::Sub: case Value::Mul: case Value::Div: case Value::Rem:
        case Value::Shl: case Value::Shr: case Value::Sar:
        case Value::And: case Value::Or: case Value::Xor: case Value::Cmp: {
            auto& binary = (InstBinary&)instruction;
            binary.lhs = f(binary.lhs);
            binary.rhs = f(binary.rhs);
            break;
        }
        case Value::Select: {
            auto& select = (InstSelect&)instruction;
            select.cond = f(select.cond);
            select.whenTrue = f(select.whenTrue);
            select.whenFalse = f(select.whenFalse);
            break;
        }
        case Value::Call:
            list(((InstCall&)instruction).args);
            break;
        case Value::CallDyn: {
            auto& call = (InstCallDyn&)instruction;
            call.callable = f(call.callable);
            call.address = f(call.address);
            list(call.args);
            break;
        }
        case Value::GenCall:
            list(((InstGenCall&)instruction).args);
            break;
        case Value::Je: {
            auto& branch = (InstJe&)instruction;
            branch.cond = f(branch.cond);
            break;
        }
        case Value::Ret: {
            auto& ret = (InstRet&)instruction;
            ret.value = f(ret.value);
            break;
        }
        case Value::Phi: {
            auto& phi = (InstPhi&)instruction;
            for(Size i = 0; i < phi.inputs.size(); i++) {
                auto input = phi.inputs.get(base, i);
                input.value = f(input.value);
                phi.inputs.set(base, i, input);
            }
            break;
        }
        default:
            break;
    }
}

template<class F>
inline void eachOperand(ModuleBase base, Value& instruction, F&& f) {
    mapOperands(base, instruction, [&](ModulePtr<Value> operand) {
        if(operand) f(operand);
        return operand;
    });
}

/*
 * The storage roots one instruction names, as the values whose use lists record them.
 *
 * A place rooted in a *local* is a use of the `Alloc` that gave the local its storage - see
 * `addPlaceUse` in resolve/block.cpp, which is what makes "every access to this local" answerable by
 * walking one use list. That use has no operand slot holding it: the root is a local index, and the
 * Alloc is reached through the function's local table.
 *
 * Which is why this is separate from `mapOperands` rather than part of it. A *rewrite* must not
 * touch the root - pointing it somewhere else is not something a place can express - while a *use
 * count* must, or an erased instruction leaves a reader the Alloc still believes in. Erasing the
 * redundant store in opt_place.cpp is exactly that case.
 */
template<class F>
inline void eachRootValue(OptContext& opt, Value& instruction, F&& f) {
    eachPlace(instruction, [&](const Place& place) {
        if(place.root != PlaceRoot::Local) return;
        if(place.local >= opt.function->localCount()) return;

        if(auto storage = opt.function->localAt(opt.local, place.local).value) f(storage);
    });
}

/*
 * Whether this value is one the optimizer may compute again, or not compute at all.
 *
 * The list is short on purpose, and every kind left out of it is left out for a reason rather than
 * from caution: the ownership instructions are the decisions the analyses already took, the calls
 * do whatever their callee does, and `LoadPlace` reads storage that something else may be writing -
 * which is a question about aliasing rather than about the instruction, and is what the place
 * forwarding pass exists to answer.
 */
inline bool isPureValue(const Value& value) {
    switch(value.kind) {
        case Value::ConstInt: case Value::ConstFloat: case Value::ConstDouble:
        case Value::ConstString:
        case Value::Cast: case Value::Neg: case Value::Not:
        case Value::Add: case Value::Sub: case Value::Mul: case Value::Div: case Value::Rem:
        case Value::Shl: case Value::Shr: case Value::Sar:
        case Value::And: case Value::Or: case Value::Xor: case Value::Cmp:
        // Pure because both of its operands are: a select is only ever built out of values that may
        // be computed unconditionally, so there is nothing in one that recomputing could repeat and
        // nothing that not computing it could skip. See convertSelects, which is where that holds.
        case Value::Select:
        case Value::Symbol: case Value::TypeMetric:
            return true;
        default:
            return false;
    }
}

// Removing one entry from a value's use list. One rather than all: an instruction naming the same
// value twice appears twice, and the list has to keep saying so.
void dropUse(OptContext& opt, ModulePtr<Value> value, ModulePtr<Inst> user);

// Pointing every reader of one value at another, use lists and operands together.
void replaceValue(OptContext& opt, ModulePtr<Value> from, ModulePtr<Value> to);

// Taking an instruction out of circulation: it stops counting as a user of everything it read, and
// is dropped from its block. Only ever called on a pure instruction nothing reads.
void eraseInstruction(OptContext& opt, ModulePtr<Inst> instruction);

/*
 * An integer type the optimizer is willing to compute in, and the two numbers it needs.
 *
 * `bits` is the type's own width and the width a result is wrapped back to. `registerBits` is the
 * width the arithmetic *happens* at, which is not the same number for a type narrower than the
 * register holding it - and the gap between them is exactly what `truncateToWidth` in
 * resolve/lower.cpp reproduces on native and `coerce` in codegen/js/type.cpp on JS.
 */
struct IntFacts {
    U16 bits = 0;
    U16 registerBits = 0;
    bool isSigned = false;

    bool fillsRegister() const { return bits == registerBits; }
};

Maybe<IntFacts> foldableInt(OptContext& opt, TypePtr type);

/*
 * Whether a type's whole representation is a constructor index - a sum whose every constructor
 * carries nothing, which `layoutRecord` in repr/repr.cpp lays out as a discriminant and no payload.
 *
 * Deliberately not an entry in `foldableInt`: an index has no width to read it at, and no arithmetic
 * is typed at one, so what that function would have to answer about it does not exist. What *is*
 * true of it is enough for two folds - `constantValueOf` reads a constant of one, and `foldCompare`
 * decides an ordering between two - and both of them are below.
 *
 * `Bool` is one of these by construction and is deliberately excluded, because it *does* have a
 * width: it is an operand of `xor`, and `foldableInt` has its own entry for it.
 */
bool isConstructorIndex(OptContext& opt, TypePtr type);

// A value read back at its own width: sign-extended to 64 bits for a signed type, zero-extended for
// an unsigned one. This is the form every fold computes in and the form a folded constant is stored
// in - see makeConstant.
U64 narrowToWidth(U64 value, const IntFacts& facts);

// The constant one operand is, or nothing where it is not one. Answered at the operand's own type,
// so a caller never has to re-normalize what it got.
Maybe<U64> constantValueOf(OptContext& opt, ModulePtr<Value> value);

// A fresh integer constant of one type, belonging to the block the instruction it replaces did.
ModulePtr<Value> makeConstant(OptContext& opt, Value& at, TypePtr type, U64 value);

/*
 * The floating counterparts, which need no equivalent of `IntFacts` because a float has none of the
 * questions an integer has: there is no refinement, no register wider than the type, and the two
 * widths are the two the hardware has. The width is the whole of what there is to know.
 */
Maybe<FloatType::Width> foldableFloat(OptContext& opt, TypePtr type);

// The constant one operand is, read as a double. An `F32` widens to one exactly, so this is the
// value at its own width either way and a caller never loses anything by being handed the wider one.
Maybe<F64> constantFloatOf(OptContext& opt, ModulePtr<Value> value);

// A fresh floating constant of one type, rounded to that type's own width on the way in - which for
// `Float` is the same rounding `Math.fround` performs on JS and a `cvtsd2ss` performs natively.
ModulePtr<Value> makeFloatConstant(OptContext& opt, Value& at, TypePtr type, F64 value);

/*
 * Whether a floating value is one both targets spell the same way, which is every one except the
 * three the emitters have no literal for: a NaN, and the two infinities.
 *
 * Written as arithmetic rather than with `<cmath>` so that it needs no more of the host than the
 * rest of this stage does - a NaN is the only value that differs from itself, and subtracting an
 * infinity from itself is the only other way to reach one.
 */
inline bool isFoldableFloat(F64 value) { return value == value && value - value == 0.0; }

// Putting freshly built instructions into a block at one position, uses and all. They are added in
// the order given and end up in that order in front of whatever was at `index`.
/*
 * A short run of instructions being built before it is spliced into a block: what one packed store
 * expands into, what materializing an argument takes, what an inlined body emits per instruction.
 *
 * Eight inline. These are expansions of a single instruction, so the count is decided by the widest
 * expansion in the pass rather than by anything about the program being compiled.
 */
using InstList = SmallArray<Inst*, 8>;

void insertInstructions(OptContext& opt, Block& block, Size index, InstList& instructions);

/*
 * The place a value came out of, or nothing where it came out of no storage this function can name.
 *
 * The same two answers ExprResolver::findPlace gives, for the same reason: a value loaded out of a
 * place is addressed through that place again rather than through a copy, and every other value of
 * a memory type is some local's storage - an allocation, a call's result, an exchanged temporary.
 *
 * Two passes ask it and they ask it of the same thing - a memory-typed argument at a call site.
 * opt_arg.cpp needs somewhere to project a field out of; opt_inline.cpp needs the root a callee's
 * own places should be rebuilt against.
 */
Maybe<Place> storageOf(OptContext& opt, ModulePtr<Value> value);

/*
 * The fields of an aggregate a pass is willing to take apart, and how a place names one.
 *
 * `constructor` is the `Downcast` a record's field path begins with - `%p@Point.x` is a downcast to
 * `Point` followed by field `x` - and is absent for a bare tuple. Reproducing that exactly is the
 * point of computing it in one place rather than at each use: a path a pass invents has to be one
 * the backends already know how to walk.
 */
struct Fields {
    TypePtr content = nullptr;
    Maybe<U16> constructor;
    Size count = 0;

    bool exists() const { return content != nullptr; }
};

// The single-constructor shape of one type, or nothing where it has no such shape - a sum, an enum,
// a scalar, an array.
Fields fieldsOf(OptContext& opt, TypePtr type);

TypePtr fieldType(OptContext& opt, const Fields& fields, Size index);
StringId fieldName(OptContext& opt, const Fields& fields, Size index);

// One field of an aggregate, as a place: the given root and path, then the constructor's downcast
// where there is one, then the field.
Place fieldPlace(OptContext& opt, Place base, const Fields& fields, U16 index);

// Fills `result` for the current function, reusing everything it already holds. Handed the
// caller's structure rather than returning one because the caller's is `opt.dominance`, which
// outlives every function.
void computeDominance(OptContext& opt, Dominance& result);

/*
 * One natural loop: the header every iteration passes through, the blocks that reach the back edge,
 * and the single block outside that leads into it.
 *
 * `preheader` is `kNone` where there is no such block, and a loop with none is one nothing may be
 * hoisted out of - there is nowhere to put it that runs exactly once per entry.
 */
struct Loop {
    U32 header = 0;
    U32 preheader = kNone;
    IndexSet contains;

    // Inline: a loop in an ordinary function is a handful of blocks, and this list is rebuilt from
    // `contains` every round of every pass that asks for the loops.
    SmallArray<U32, 16> blocks;

    Loop() = default;
    Loop(Loop&&) = default;

    /*
     * Written out because the loop list is *sorted* - innermost first - and a sort assigns.
     *
     * Neither member can take the default: an IndexSet is move-only, and a SmallArray deletes
     * assignment because the inherited one would append rather than replace. So the two are said
     * here by their own names, which is also the only place the copy is: `contains` hands its
     * storage over, and `blocks` is copied into whatever storage the destination already had.
     */
    Loop& operator = (Loop&& other) {
        if(this == &other) return *this;

        header = other.header;
        preheader = other.preheader;
        contains = ::move(other.contains);
        replaceContents(blocks, other.blocks);
        return *this;
    }

    static constexpr U32 kNone = maxLimit<U32>;
};

// Innermost first, so a value hoisted out of an inner loop lands where the next round can hoist it
// out of the one containing it.
void computeLoops(OptContext& opt, Dominance& dominance, Array<Loop>& loops);

// The blocks control can actually get to, indexed by block. A pass that reasons optimistically about
// a cycle needs this, and so does one that deletes what nothing reaches.
void computeReachable(OptContext& opt, IndexSet& reachable);

// Per local, whether a callee could reach its storage: indexed by local, and false for anything the
// function handed an address of.
void computeContainment(OptContext& opt, IndexSet& contained);

// Whether this place is storage inside a local a callee cannot reach - a contained root, and a path
// that stays inside the allocation rather than leaving through a pointer, an element or a witness.
bool staysInFrame(OptContext& opt, const IndexSet& contained, const Place& place);

// Recomputing every use list from the instructions that exist - see opt.cpp, which says why it is
// necessary rather than tidy. Any pass that rewrites a function it did not arrive at through the
// driver has to do this first.
void rebuildUses(OptContext& opt);

// Every function the program can reach other than by naming it in a `Call` or a `GenCall` - see
// opt_arg.cpp. A call site cannot be rewritten on behalf of one of these, because there is no
// declaration at the site to read the rule from.
void addressTaken(OptContext& opt, HashMap<U32, bool>& taken);

/*
 * The three questions about storage, answered in opt_place.cpp - see the file comment there, which
 * is where the reasoning behind each of them lives.
 *
 * `pathsMayOverlap` compares two projection paths and takes a prefix relation as overlap in both
 * directions, so writing `x` kills `x.a` and writing `x.a` kills a read of `x`. `placesMayAlias`
 * adds the roots to that, and declines to say anything at all about a pointer or a borrow.
 * `samePlace` is the stricter question a *forward* needs, as against the one a kill needs: the same
 * storage rather than possibly the same.
 */
bool pathsMayOverlap(OptContext& opt, const Place& a, const Place& b);
bool placesMayAlias(OptContext& opt, const Place& a, const Place& b);
bool samePlace(OptContext& opt, const Place& a, const Place& b);

// Whether a value of this type is one whose *contents* a load answers with, rather than storage the
// load merely names. Forwarding one of the second kind would replace a place with a value that is
// not the same thing.
bool holdsLoadableValue(OptContext& opt, TypePtr type);

/*
 * A successor's record of where one edge arrived from, pointed at a different block: its predecessor
 * entry and every phi alternative that named the old one. Both halves, because a phi is a value the
 * *predecessors* produce and an alternative left naming a block the edge no longer leaves from is an
 * input no backend can find a copy for.
 *
 * Every match rather than the first, since a `je` with both arms at one block leaves two of each.
 */
void retargetEdge(OptContext& opt, Block* target, ModulePtr<Block> from, ModulePtr<Block> to);

/*
 * A block whose one predecessor ends by jumping straight to it, folded back into that predecessor -
 * see opt_branch.cpp, which is where the rule and its guards live.
 *
 * Exposed because two passes produce the shape and neither can clean up after the other. Inlining
 * makes it by cutting a caller's block in two around a call it then grafts a body into; folding
 * makes it by deleting an arm and leaving the join with one way in. Answers whether anything moved.
 */
bool mergeBlocks(OptContext& opt);

/*
 * The blocks nothing reaches, dropped along with their edges into the ones that survive - see
 * opt_branch.cpp. Answers whether anything went, since the block list is renumbered when it does.
 *
 * Exposed for the same reason `mergeBlocks` is: it is the cleanup any CFG rewrite here owes, and
 * there are now two of them - a folded branch strands an arm, and an if-conversion splices out both.
 */
bool removeUnreachableBlocks(OptContext& opt);

void foldFunction(OptContext& opt);
void foldBranches(OptContext& opt);

/*
 * If-conversion: a branch whose arms only produce a value becomes a `select` - see opt_select.cpp.
 *
 * A CFG rewrite like `foldBranches`, and after it for the same reason that one is after the folder:
 * most of the diamonds worth converting are made rather than written, and an arm a constant
 * condition already deleted is not one to convert.
 */
void convertSelects(OptContext& opt);
void collapseBorrows(OptContext& opt);
void forwardPlaces(OptContext& opt);
void promotePlaces(OptContext& opt);
void hoistLoopValues(OptContext& opt);
void eliminateDeadLoops(OptContext& opt);

/*
 * The driver's own fixed point over one function, exposed because the inliner needs it too.
 *
 * What a call site is judged against is the callee as it will be *emitted*, and answering that is
 * running the passes on it - see `settle` in opt_inline.cpp, and Implementation-Containers.md §13.2
 * for the case that made a chosen few of them not enough. Expects `rebuildUses` to have been called
 * for the function it is about to work on, which is the one thing it does not do itself.
 */
void optimizeRounds(OptContext& opt);
void scalarizeLocals(OptContext& opt);
void eliminateCommonValues(OptContext& opt);
void eliminateDeadValues(OptContext& opt);

// The repr-lower step: packed field access becomes arithmetic over a storage unit. Answers whether
// it rewrote anything, which is what decides whether the passes above are worth running again.
bool expandPacking(OptContext& opt);

/*
 * The calling convention step: a record parameter becomes one parameter per field.
 *
 * Program-wide and run once, before any function is optimized, because it rewrites signatures and
 * every caller of them - see opt_arg.cpp for why that cannot be done a function at a time.
 */
void flattenArguments(OptContext& opt);

/*
 * The inlining step: a call to a straight-line callee becomes a copy of its body.
 *
 * Program-wide and run once alongside `flattenArguments`, and before any function is optimized,
 * because what it is for is giving the passes below something to see - see opt_inline.cpp.
 */
void inlineCalls(OptContext& opt);

/*
 * Ownership spent into ordinary operations - Implementation-Simplification.md §14.
 *
 * Program-wide and before everything else here, because what it removes is the constraint the rest
 * of this stage is written under: `clonableKind` refuses a body containing an ownership instruction,
 * so the shapes most worth inlining were the ones that could not be. Its postcondition is per
 * function - *a non-generic body contains no `Drop`* - and that is the form every consumer needs.
 */
void dischargeOwnership(OptContext& opt);
