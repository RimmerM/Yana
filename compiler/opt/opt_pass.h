#pragma once

#include "opt.h"
#include "../resolve/builder.h"
#include "../resolve/place.h"
#include "../resolve/verify.h"
#include "../resolve/analyze.h"

/*
 * What the passes share: the state one function is optimized against, and the questions about types
 * and storage that are the same in all of them.
 *
 * What is *not* here any more is the IR surgery. Every rewrite in this directory goes through
 * `opt.ir()` - see resolve/edit.h - because each of them is a two-sided edit: an instruction names
 * its operands and every value names its users, an edge is written down in three places, and a pass
 * that updates one side without the other leaves an IR that prints correctly and walks wrongly.
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

/*
 * When one cached analysis was computed, and from what.
 *
 * `OptContext` holds the three answers every pass here asks for - the dominator tree, the loops and
 * the storage a callee can reach - and each of them used to be recomputed by each pass that wanted
 * it. That is between two and five walks of a function per round of a fixed point that runs the
 * round up to eight times, for an answer that in most of those rounds nothing has invalidated: the
 * later rounds of the loop exist to discover that there is nothing left to do.
 *
 * So each is stamped with the function it was read from and the `IrVersion` the IR was at, and is
 * handed back rather than recomputed while both still hold. `overValues` is which structure the
 * analysis is over - see IrVersion. A dominator tree and the loops derived from it are statements
 * about the block graph alone, so folding an operand or deleting an instruction leaves them
 * standing; containment is a statement about instructions and use lists, so it does not.
 *
 * The function is compared as well as the version, because `opt.function` is assigned in eight
 * places - the inliner settles a callee in the middle of describing a call site, and gives the
 * caller back afterwards - and a stamp naming another function is not an answer about this one
 * whatever the IR has done since.
 */
struct AnalysisStamp {
    Function* function = nullptr;

    // 64 bits for the reason the counters are: a stamp is only wrong if a counter wraps exactly
    // onto the value it holds, and a width no compilation can exhaust is cheaper than the argument
    // that no compilation can exhaust a narrower one.
    U64 values = 0;
    U64 blocks = 0;

    bool holds(Function* current, const IrVersion& now, bool overValues) const {
        if(function != current || blocks != now.blocks) return false;
        return !overValues || values == now.values;
    }

    void take(Function* current, const IrVersion& now) {
        function = current;
        values = now.values;
        blocks = now.blocks;
    }
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
     * of bits each time. `sets` hands out the per-pass ones by scope - see ScratchSet.
     *
     * The four below it are not scratch: they are the cached analyses, held here rather than in the
     * pass that asked because the point of them is that the *next* pass gets the same answer. See
     * AnalysisStamp, and `dominanceOf` and its three neighbours, which are the only way to read one.
     */
    IndexSetPool sets;

    Dominance dominance;
    Array<Loop> loops;
    IndexSet contained;
    IndexSet reachable;

    AnalysisStamp dominanceStamp;
    AnalysisStamp loopStamp;
    AnalysisStamp containedStamp;
    AnalysisStamp reachableStamp;

    // Set by any rewrite. The driver runs the passes to a fixed point over one function, because
    // folding exposes identities and identities expose more folding.
    bool changed = false;

    // What has been written, for the four stamps above - see IrVersion. Not the same question as
    // `changed`: a rewrite the fixed point has no reason to run another round over is still one a
    // cached answer has every reason to notice.
    IrVersion version;

    /*
     * The one way this directory edits the IR - see resolve/edit.h.
     *
     * Built per call rather than held, because an editor is bound to one function and this context
     * outlives every function it visits: three references and a flag, so a pass asking for one in a
     * loop costs nothing worth arranging around.
     */
    IrEditor ir() { return IrEditor(*module, *function, &changed, &version); }
};

// The storage roots one instruction names - see eachPlaceRootValue in resolve/module.h, which is
// where it lives now that the verifier asks the same question outside this stage.
template<class F>
inline void eachRootValue(OptContext& opt, Value& instruction, F&& f) {
    eachPlaceRootValue(opt.local, *opt.function, instruction, forward<F>(f));
}

/*
 * The locals whose storage an instruction is *handed*, as opposed to the ones it names a place in.
 *
 * `eachPlace` is the other half and neither covers the other: a call has no places at all, and the
 * whole-aggregate operand is how a record reaches one. Every caller of this is also a caller of
 * that, for the reason `computeContainment` admits the argument case at all - the exposure ends with
 * the call, so whoever is tracking storage across it has to forget exactly here instead.
 *
 * Answered off the `Alloc` rather than off the type, because that is what a local's storage *is* as a
 * value: opt_arg.cpp says the same when it substitutes one for a retired parameter.
 */
template<class F>
inline void eachHandedLocal(OptContext& opt, Value& instruction, F&& f) {
    eachOperand(opt.local, instruction, [&](ModulePtr<Value> operand) {
        auto& value = *opt.local[operand];
        if(value.kind != Value::Alloc) return;

        auto local = ((InstAlloc&)value).local;
        if(local != maxLimit<U32>) f(local);
    });
}

/*
 * The locals whose *address* an instruction is handed, which is the second way storage reaches a
 * callee - `push(out, 0)` passes a `borrow_mut` of a local rather than the record itself.
 *
 * Kept apart from `eachHandedLocal` above, and the split is a ruling rather than a tidiness. The two
 * are the two halves of what `computeContainment` admits, and the readers of containment divide on
 * exactly this line:
 *
 *  - **A by-value argument is a binding the callee cannot assign**, so the storage behind it is read
 *    while the call runs and not written. That is what lets `promotePlaces` forward a field across
 *    `score(f)` and `eliminateCommonValues` keep a load over it, which is the whole of what
 *    Contain.Call.yana and Default.yana assert. `forwardPlaces` and `scanEffects` forget anyway,
 *    which costs those two passes nothing they were getting.
 *  - **An address is one the callee may write through**, and every reader has to end its facts at
 *    the instruction that received it. There is no summary flag for "wrote through this argument"
 *    and no reason to want one: the call is a point, and forgetting at a point is exact.
 *
 * The whole local rather than the borrowed sub-place, because that is what every caller does with
 * the answer - a `Place::inLocal` covers the path the borrow was taken of and every path beside it.
 * Mutability is not asked either: `&` is the mutable form in this IR and `InstBorrow::mut` is the
 * exclusivity question rather than the writability one.
 */
template<class F>
inline void eachAddressedLocal(OptContext& opt, Value& instruction, F&& f) {
    eachOperand(opt.local, instruction, [&](ModulePtr<Value> operand) {
        auto& value = *opt.local[operand];
        if(value.kind != Value::Borrow) return;

        auto& place = ((InstBorrow&)value).place;
        if(place.root == PlaceRoot::Local && place.local < opt.function->localCount()) {
            f(place.local);
        }
    });
}

/*
 * Whether a call is one of the checks the compiler inserted - see Program::checkCondition.
 *
 * Recognized by the callee rather than by its name, for the reason the pointer is recorded on the
 * program at all: nothing in any program writes this call, and the stages that emit one have to be
 * able to point at the same function without agreeing on a spelling.
 *
 * Two passes ask, and they ask for opposite halves of the same fact - that the callee reads a flag
 * and touches nothing else. `clobbers` uses it to keep the storage it is tracking across one, and
 * `isDischargedCheck` uses it to remove the call outright once the flag has folded to `false`.
 */
inline bool isCheckCall(OptContext& opt, ModulePtr<Function> callee) {
    return callee && callee == opt.program.checkCondition;
}

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

/*
 * The place a value came out of, or nothing where it came out of no storage this function can name.
 *
 * The same two answers ExprResolver::findPlace gives, for the same reason: a value loaded out of a
 * place is addressed through that place again rather than through a copy, and every other value of
 * a memory type is some local's storage - an allocation, a call's result, an exchanged temporary.
 *
 * Two passes ask it and they ask it of the same thing - a memory-typed argument at a call site.
 * opt_arg.cpp needs somewhere to project a field out of; opt_inline.cpp needs the root a callee's
 * own places should be rebuilt against. Both of them ask it through `argumentStorage` below rather
 * than directly, and the note there is why.
 */
Maybe<Place> storageOf(OptContext& opt, ModulePtr<Value> value);

/*
 * The same question asked of a *call argument*, which is the only form either caller has.
 *
 * A `return` parameter's argument is the exception `storageOf` cannot answer, and it is why this
 * exists. The marker makes the loan outlive the call, so `borrowArgument` hands over an explicit
 * `InstBorrow` rather than the loaded value - and that borrow has a slot of its own holding an
 * *address*. `storageOf` finds that slot, which is a correct answer to the question it asks and the
 * wrong storage for either caller: it is one pointer wide and has no fields at all. Reading
 * `Flat.items` and `Flat.length` out of it produced two loads at offset zero, so a slice's length
 * arrived as its own base address.
 *
 * What the fields are in is the place the borrow *names*, which the instruction carries.
 */
Maybe<Place> argumentStorage(OptContext& opt, ModulePtr<Value> value);

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

// Innermost first, so a value hoisted out of an inner loop lands where the next round can hoist it
// out of the one containing it.
void computeLoops(OptContext& opt, Dominance& dominance, Array<Loop>& loops);

// The blocks control can actually get to, indexed by block. A pass that reasons optimistically about
// a cycle needs this, and so does one that deletes what nothing reaches.
void computeReachable(OptContext& opt, IndexSet& reachable);

// Per local, whether a callee could reach its storage: indexed by local, and false for anything the
// function handed an address of.
void computeContainment(OptContext& opt, IndexSet& contained);

/*
 * The four above, asked of the stage rather than computed - and this is the form every pass should
 * use. See AnalysisStamp: the answer is computed on the first ask and handed back on every ask after
 * it until something writes the structure it is stated over.
 *
 * A pass reads one of these *once*, at the top, and holds the reference across its own rewrites -
 * which is what the four already did with their private copies and is the same staleness they always
 * had. What is new is only that asking again after a rewrite gets a fresh answer rather than the
 * same one, so a pass that wants the stale reading has to keep holding its reference rather than
 * re-asking. None does.
 */
Dominance& dominanceOf(OptContext& opt);
Array<Loop>& loopsOf(OptContext& opt);
const IndexSet& containmentOf(OptContext& opt);
const IndexSet& reachableOf(OptContext& opt);

// Whether this place is storage inside a local a callee cannot reach - a contained root, and a path
// that stays inside the allocation rather than leaving through a pointer, an element or a witness.
bool staysInFrame(OptContext& opt, const IndexSet& contained, const Place& place);

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
 * Whether an instruction may write storage a pass is tracking, or hand out a way to - opt_place.cpp,
 * where the rule and the reason its default is `true` both live.
 *
 * Read by both passes that carry a fact about storage across an instruction, and the two ask it at
 * different reach: `forwardPlaces` of the instruction in front of the one it is looking at,
 * `eliminateCommonValues` of every block that could run between two loads of one place. The places
 * an instruction *names* are the caller's question - a store is not one of these, and what it
 * invalidates is `placesMayAlias` rather than everything.
 */
bool writesUnknownStorage(OptContext& opt, Value& instruction);

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

/*
 * A boolean phi used only by a branch, threaded back onto the edges that supplied it - see
 * opt_branch.cpp. Answers whether a join went away.
 */
bool threadBooleanBranches(OptContext& opt);

/*
 * The blocks a call that does not come back ends - see opt_branch.cpp, and §10 item 2 of
 * test/bench/findings.md for what the edge it removes was costing. Answers whether anything changed.
 */
bool endNonReturningBlocks(OptContext& opt);

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

/*
 * What a branch proves about the arm below it - see opt_range.cpp, which is the whole of the range
 * reasoning in this directory and is deliberately one fact rather than a lattice.
 */
void narrowCheckedIndexes(OptContext& opt);

// The zero tests a dominating branch already decided - the second half of opt_range.cpp. Folds the
// comparison and leaves the branch to `foldBranches` on the next round.
void foldProvenZeroTests(OptContext& opt);
void eliminateDeadLoops(OptContext& opt);

/*
 * The driver's own fixed point over one function, exposed because the inliner needs it too.
 *
 * What a call site is judged against is the callee as it will be *emitted*, and answering that is
 * running the passes on it - see `settle` in opt_inline.cpp, and Implementation-Containers.md §13.2
 * for the case that made a chosen few of them not enough.
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

/*
 * The teardown of a function value every reaching lambda leaves empty, deleted rather than searched
 * for at run time - see opt_closure.cpp.
 *
 * Called by `dischargeDrop` before the generic expansion, and answers whether it took the drop.
 * Separate from that function because what it is is an analysis: what it discharges to is nothing,
 * and everything worth reading is the proof that it is.
 */
bool devirtualizeClosureDrop(OptContext& opt, Block& block, Size index, InstDrop& drop);

/*
 * Which lifted lambdas still need the header emitted in front of them - see opt_closure.cpp.
 *
 * Program-wide and before `dischargeOwnership`, because what it reads is the drops as the ownership
 * passes left them: a closure whose drop `closureTeardown` already resolved to its environment is
 * exactly the one whose header nothing goes through. Clears `Function::closureHeaderRead`, which
 * both backends consult before emitting the table.
 */
void markClosureHeaders(OptContext& opt);
