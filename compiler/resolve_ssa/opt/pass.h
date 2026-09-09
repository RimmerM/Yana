#pragma once

#include "opt.h"
#include "analysis.h"
#include "../build.h"

namespace ssa {

/*
 * What the passes share: the state one body is optimized against, and the handful of questions
 * about types and instructions that every one of them asks.
 *
 * What is deliberately *not* here is the IR surgery. Every rewrite in this directory goes through
 * `opt.ir()` - see `resolve_ssa/build.h` - because each of them is a two-sided edit: an instruction
 * names its operands and every value names its users, an edge is written down in two places, and a
 * pass that updates one side without the other leaves IR that prints correctly and walks wrongly.
 */
struct OptContext {
    Context& context;
    mir::Program& program;
    Converted& converted;

    // The two regions, unpacked once. Declarations survive the release of MIR and are what every
    // type question goes through; the SSA region is where the bodies are.
    DeclBase decl;
    SsaBase base;

    // The body being optimized, and the declaration it belongs to. Both, because half the questions
    // at this tier are about the signature rather than about the graph.
    mir::Function* function = nullptr;
    Body* body = nullptr;

    /*
     * The buffers the passes work in, which belong to the stage rather than to one body.
     *
     * There is one `OptContext` per program and one call of each pass per body per round, so a set
     * built inside a pass is built a few thousand times over a compilation and holds a handful of
     * bits each time. `sets` hands out the per-pass ones by scope - see `ScratchSet`.
     *
     * The analyses below it are not scratch: they are cached, and held here rather than in the pass
     * that asked because the point of them is that the *next* pass gets the same answer. See
     * `AnalysisStamp`, and `dominatorsOf` and its neighbours, which are the only way to read one.
     */
    IndexSetPool sets;

    Dominators dominators;
    Loops loops;
    Contained contained;
    ConstantContents constantContents;

    AnalysisStamp dominanceStamp;
    AnalysisStamp loopStamp;
    AnalysisStamp containedStamp;
    AnalysisStamp constantStamp;

    // Set by any rewrite, through the editor. The driver runs the passes to a fixed point over one
    // body, because folding exposes identities and identities leave values unread.
    bool changed = false;

    /*
     * The one way this directory edits SSA - see `resolve_ssa/build.h`.
     *
     * Built per rewrite rather than held, because an editor is bound to one body and this context
     * outlives every body it visits: five references and a flag, so a pass asking for one in a loop
     * costs nothing worth arranging around.
     *
     * No version counter is handed over, and that is `IrVersion`'s note: the counters live on the
     * body, so an editor cannot be constructed without them and a stale cache cannot be arranged by
     * forgetting an argument.
     */
    Editor ir() { return Editor(context, program, converted.arena, *function, *body, &changed); }
};

/*
 * The shared predicates.
 *
 * Three of the questions a pass asks are already answered on the instruction: `isPure`,
 * `mayConsume` and `isTerminator` are `inst.h`'s readers over `inst.def`'s trait column, they take
 * a kind rather than a context, and they are in scope here through that header. Wrapping them would
 * be a second spelling of a table that already has one.
 *
 * The two below are the type questions, and they are wrapped because the alternative is every pass
 * remembering which region a type is answered against. `hasTeardown` is whether a value of this
 * type owes a teardown call, and `isLinear` is whether an owned one is *followed* at all - see
 * `resolve_mir/type.cpp`, where the two are kept apart and the third question beside them
 * (`isTriviallyCopyable`) is the one neither answers.
 */
inline bool hasTeardown(OptContext& opt, DeclPtr<mir::Type> type) {
    return mir::hasTeardown(opt.decl, type);
}

inline bool isLinear(OptContext& opt, DeclPtr<mir::Type> type) {
    return mir::isLinear(opt.decl, type);
}

/*
 * The same two questions asked of a whole **occurrence**, which is what an allocation and a drop
 * carry - `InstAlloc::held`, `InstDrop::held`.
 *
 * Not the same answer as the type's, and the difference is the one the occurrence exists for: a
 * borrowed occurrence of a type that owes a teardown owes nothing, because what is at the place
 * belongs to somebody else. `mir::ownershipOfOccurrence` is where that is decided, and asking it
 * through the type alone is how a pass comes to remove a slot whose contents the frame does own.
 */
inline bool hasTeardownAt(OptContext& opt, const Occurrence& held) {
    return mir::hasTeardownAt(opt.decl, held);
}

inline bool isLinearAt(OptContext& opt, const Occurrence& held) {
    return mir::isLinearAt(opt.decl, held);
}

/*
 * An integer type this stage is willing to compute in, and the two things it needs to know.
 *
 * `bits` is the type's **declared** width, which is the width a result is put back into. There is no
 * register width beside it and that is the difference from the shipping stage's `IntFacts`: the
 * normal form at this tier is the declared one - `mir::reducedToWidth` states it for a constant and
 * `resolve_ssa/lower/width.cpp` for a value in a register - so how wide the register holding it
 * turns out to be is a question below this one and nothing here has to ask it.
 *
 * See `foldableInt` for which types have one, and for the two that deliberately do not.
 */
struct IntFacts {
    U8 bits = 0;
    bool isSigned = false;
};

/*
 * Whether a type is one the folder may compute in, and at what width.
 *
 * Three answers rather than a type test, because the three kinds that reach the arithmetic are not
 * one kind:
 *
 *  - an **integer**, at its own declared width and signedness;
 *  - a **`@bits(n)` refinement**, at the refinement's own `n` - see the note in `fold.cpp`, which is
 *    where the decision and its reason are written down, and where it parts company with the
 *    shipping stage's folder;
 *  - **`Bool`**, one unsigned bit. It is a declared enum in this stage rather than a type kind, and
 *    the three connectives are `Bitwise(Bool)`'s members, so the folder has to have a width for it.
 *
 * Nothing for a **target-answered** width - `Size`, `USize`, `CodeUnit` - because there is no single
 * width to compute one at. `foldableIntWidths` below is what a caller that is willing to do the work
 * asks instead; this stays the reader for everything that wants one number, and every pass outside
 * the folder is one of those.
 */
Maybe<IntFacts> foldableInt(OptContext& opt, DeclPtr<mir::Type> type);

/*
 * The two widths a type may be computed at, for the one caller willing to compute at both.
 *
 * A type that states its own width answers it twice, and that is what makes the rule one rule rather
 * than two. A **target-answered** one - `Size`, `USize`, `CodeUnit` - answers the width the language
 * guarantees and the widest it may turn out to be; `Folder::foldAtWidths` runs the whole fold at
 * each and writes the answer down only where the two agree, which is the only form in which a
 * number folded here is a number on every target.
 *
 * **Agreement rather than range.** The obvious weaker rule - compute at the guaranteed width and
 * check that the answer is one the type holds - is what `resolve_mir/fold.cpp` had, and it
 * miscompiled: `not (0 :: USize)` is `0xffffffff` at thirty-two bits, which a `USize` holds
 * everywhere, and is `0xffffffffffffffff` on the target this compiler emits for. `neg` of an
 * unsigned, a `sub` below zero and a `shl` that carries past the guaranteed width are the same hole.
 * What agreement costs is those cases' fold and nothing else: a value that came from source already
 * holds at the guaranteed width (`mir::integerHolds`), and every operation that stays inside it
 * answers the same number at both.
 */
struct IntWidths {
    IntFacts narrow;
    U8 wide = 0;

    bool differ() const { return wide != narrow.bits; }
};

Maybe<IntWidths> foldableIntWidths(OptContext& opt, DeclPtr<mir::Type> type);

// The float width, 32 or 64, or nothing for anything else. No `IntFacts` equivalent, because a float
// has none of the questions an integer has: no refinement, no register wider than the type, and the
// two widths are the two the hardware has.
Maybe<U8> foldableFloat(OptContext& opt, DeclPtr<mir::Type> type);

// A value read back at its own width: sign-extended to 64 bits where the type is signed, masked
// where it is not. The form every fold here computes in and the form a folded constant is stored in,
// and the same rule `mir::reducedToWidth` applies to a literal.
U64 narrowToWidth(U64 value, const IntFacts& facts);

// The constant one value is, at its own type's width, or nothing where it is not one.
Maybe<U64> constantValueOf(OptContext& opt, Value* value);

/*
 * The constant a value *is*, or null - the whole `mir::Constant` rather than the number.
 *
 * What `constantValueOf` cannot answer about: whether two values hold the same constant, which
 * matters because this IR does not intern them. Exported for `cfg.cpp`, where a block argument every
 * edge answers with one number is the shape two rewrites make and one spends.
 */
const mir::Constant* constantOf(OptContext& opt, Value* value);

// The constant one value is, read as a double. An `F32` widens to one exactly, so this is the value
// at its own width either way.
Maybe<F64> constantFloatOf(OptContext& opt, Value* value);

/*
 * Whether a floating value is one this stage may write down - every one except the three there is no
 * literal for: a NaN, and the two infinities.
 *
 * Written as arithmetic rather than with `<cmath>`, which nothing else in this stage needs: a NaN is
 * the only value that differs from itself, and subtracting an infinity from itself is the only other
 * way to reach one.
 */
inline bool isFoldableFloat(F64 value) { return value == value && value - value == 0.0; }

/*
 * A fresh constant, placed immediately in front of the instruction it is being folded into - see
 * `Editor::setInsertBefore`, which exists for this.
 *
 * In front rather than anywhere, because this is real SSA and a constant defined below the
 * instruction reading it is what `checkUse` reports. `makeConstant` takes the value at the type's own
 * width and turns it into whichever kind of constant that type holds - an integer, or the
 * constructor index of an enum, which is what a `Bool` is here.
 */
Value* makeConstant(OptContext& opt, Inst& at, DeclPtr<mir::Type> type, U64 value);

// The same, for a constant that is not a number - a `map` arm's string, float or record.
Value* makeConstantValue(OptContext& opt, Inst& at, const mir::Constant& constant);
Value* makeFloatConstant(OptContext& opt, Inst& at, DeclPtr<mir::Type> type, F64 value);

/*
 * What one instruction comes to, or null where it comes to nothing new - `opt/fold.cpp`.
 *
 * A *value*, never a rewrite in place: everything here answers with a constant it made, an operand
 * that was already standing there, or - in the one reassociation rule - the result of an instruction
 * it made in front of this one, and the caller is what points the readers at it and takes the
 * instruction out. That is what lets `simplify` drive this from a worklist: the only thing that
 * changes is which values exist. It is why the `bitsUpTo`-becomes-`and` rewrite the shipping stage's
 * folder performs is not here - that one *replaces* an instruction at a different kind, which is the
 * shape `simplify.cpp`'s own rewrites have, and `optimizeLowerFunction` makes it one tier down.
 */
Value* foldInst(OptContext& opt, Inst& inst);

/*
 * When two instructions are the same, and when one of them may be taken out - `opt/cse.cpp`, which
 * is the one statement of both.
 *
 * Here rather than inside a pass because three of them ask: `simplify`'s block-local CSE, `cse`'s
 * walk of the dominator tree, and `range`, which folds a comparison and has the same ownership
 * question to answer first. Two spellings of "these two instructions are the same" is one of them
 * learning about a new kind and the other not, which is the failure `sameExtra` was written to stop
 * one tier down.
 *
 *  - `isRemovable` is the ownership rule: a `kSsaPure` kind, no tracked operand and no consuming
 *    use, and results that are `Trivial` or `Address`. `simplify.cpp`'s file comment is where each
 *    of the three clauses is argued.
 *  - `isUnifiable` is whether one value may stand for another - the conventions *agree* rather than
 *    merely both being untracked, because replacing an address by a scalar is not a substitution
 *    however the linear rule reads it.
 *  - `sameComputation` compares operands as SSA values with their use kinds, and the results on
 *    their whole occurrence and convention - an equality rather than a congruence, so two
 *    instructions with equal-but-distinct operands are left for the round after the operands are
 *    unified.
 *  - `materializedAtUse` is the exemption: a computation over nothing but constants, and an
 *    instruction with no operands at all, are spelled where they are read rather than computed, so
 *    a second reader is what stops that.
 */
bool isRemovable(OptContext& opt, Inst& inst);

/*
 * The same rule for an instruction **nothing reads**, which admits a `Load` as well - `opt/cse.cpp`,
 * where the ownership half is shared with `isRemovable` rather than restated.
 *
 * One caller, `dischargeChecks`, and it is the one that runs where `forwardMemory` and
 * `removeDeadStorage` do not: the `self.length` read a discharged bounds test leaves standing is a
 * memory access whose value nothing wants, and the purity column declines it for a question - may a
 * pass recompute this, or move it past a store - that deleting an unread read does not ask.
 */
bool isRemovableIfUnread(OptContext& opt, Inst& inst);

// And the weakest of the three, for an instruction nothing reads - `cse.cpp`, where the two clauses
// it drops are argued. `simplify`'s dead-value rewrite is the one reader.
bool isDiscardableIfUnread(OptContext& opt, Inst& inst);

bool isUnifiable(Value& from, Value& to);
bool sameComputation(OptContext& opt, Inst& a, Inst& b);
bool materializedAtUse(OptContext& opt, Inst& inst);

// The one result an instruction has, or null where it has none or several. Shared for the same
// reason as the four above: every rewrite that stands one value in for another opens with it.
Value* soleResult(OptContext& opt, Inst& inst);

/*
 * The passes.
 *
 * Each takes the context and nothing else, sets `opt.changed` through the editor when it rewrites,
 * and is followed by a verifier checkpoint that names it - see `runPass` in `opt.cpp`. A pass is
 * declared here **when it is built**, so that this header never carries a name for code that does
 * not exist: phase 4 adds `simplify`, phase 5 `cleanCfg` and `threadBranches`, phase 6
 * `forwardMemory` and `removeDeadStorage`, phase 7 `convertSelects`, phase 8 `deadLoops` - and not
 * `range`, which phase 8 measured and did not keep and phase 11 measured again on promoted IR and
 * did not keep either, see README.md's two notes - phase 9 `inline` and `reach`, phase 10 `promote`,
 * and phase 11 `cse` and `hoist`. README.md's section B is the list and the order.
 *
 * Phase 9 added the last two, `inlineCalls` and `markProgramReachable`, and they are the two that
 * are **program-wide**: they run once, in front of the per-function loop, because what one leaves
 * behind is work for the seven below it and what the other answers is a question about the program
 * rather than about a body.
 */

/*
 * A local held in values rather than in storage, with block arguments where the writes meet -
 * `opt/promote.cpp`.
 *
 * First in the list, because everything below it is written against values and this is what turns a
 * local into one. Phase 10 is why it is at this tier at all rather than left to
 * `promoteStackSlots`: four of the passes below measured their own answers away against storage the
 * lower tier removes too late for any of them to see it.
 */
void promote(OptContext& opt);

// Constants, algebraic identities, the `Bool` bitwise rewrite, block-local pure CSE and dead-value
// removal, as one worklist over uses - `opt/simplify.cpp`, and README.md's section B for why the
// five are one pass.
void simplify(OptContext& opt);

// The graph: a branch on a constant, a block nothing reaches, and a block boundary with nothing on
// either side of it, as one worklist over blocks - `opt/cfg.cpp`. The three are one pass because
// each of them produces the shape the other two consume.
void cleanCfg(OptContext& opt);

/*
 * A block argument every incoming edge answers with the same value - or with the argument itself -
 * replaced by that value and taken out, and one nothing reads taken out outright. `opt/cfg.cpp`.
 *
 * Not a pass in `optimizeBody`'s list: it is called by the two rewrites that make the shape, from
 * inside their own runs. `promote` at the end of its construction, over the arguments it placed
 * where a slot's writes met; `cleanCfg` at the top of each of its sweeps, over a header whose back
 * edge has just gone. Everything the *conversion* placed - a `for`'s bound and step carried round
 * the header - is what neither of them used to reach, which is phase 14 and the note in `cfg.cpp`.
 */
void removeTrivialArguments(OptContext& opt);

// A join whose branch an incoming edge has already decided, sent straight past it - `opt/cfg.cpp`,
// beside `cleanCfg` and not inside it: this one *creates* edges, so it cannot run in a walk that is
// holding a block cursor.
void threadBranches(OptContext& opt);

/*
 * A condition a branch decided, handed on again from inside the arm it decided - `opt/cfg.cpp`.
 *
 * Beside `threadBranches` because it makes the shape that pass and `removeTrivialArguments` spend:
 * a block argument every edge answers with the same constant. It is not the branch-implied *range*
 * facts README.md measured twice and did not keep - this relates a value to itself rather than two
 * values to each other, and is one comparison with no lattice.
 */
void decideBranchConditions(OptContext& opt);

// The third of `cleanCfg`'s three rewrites on its own - the blocks nothing reaches, and the edges
// and values they owned. Exported for `dischargeChecks`, which runs where `cleanCfg` does not and
// strands a block that holds a call; one implementation reached two ways, rather than a second
// reachability walk to keep in agreement with this one.
void removeUnreachableBlocks(OptContext& opt);

// The edge after a call that does not come back, cut - `mir::Summary::diverges`, and the abort arm
// of every grafted bounds check. In `opt/cfg.cpp` beside the cleanup it hands its work to, and run
// immediately in front of `cleanCfg` for the same reason.
void endNonReturningBlocks(OptContext& opt);

// A read answered by a write above it, a second read answered by the first, and a write nothing
// reads - one forward scan per block, carrying a table from address to contents. `opt/memory.cpp`,
// and README.md's section B for why it is a scan rather than a worklist.
void forwardMemory(OptContext& opt);

// An allocation nothing reads, with the writes into it - `opt/memory.cpp`, beside the forwarder
// because it is what makes an allocation's reads go away in the first place.
void removeDeadStorage(OptContext& opt);

/*
 * A run of slots the frame can keep - `opt/place.cpp`, and the one placement this tier decides.
 *
 * A `RunSlots` whose bytes never leave the frame is marked `placed`, and the descriptor built over
 * it stops claiming the allocator's. It is here rather than in MIR because the frame that keeps the
 * answer is not always the frame the request is written in: `String.ofCapacity` and `growArray` ask
 * for storage their *caller* keeps, so nothing can be asked until the inliner has moved it. Such a
 * request also arrives as a `runSlots` call, which `normalize` makes into the instruction first.
 */
void placeRuns(OptContext& opt);

// If-conversion: a branch whose arms only compute a value becomes one `select` per value the join
// declares - `opt/select.cpp`. Both arms are evaluated afterwards, which is the whole of what it has
// to be sure of, and the linear rule is a precondition rather than a check.
void convertSelects(OptContext& opt);

// A loop whose body has no effect and whose exit does not depend on anything it computes, decided
// false at its header - `opt/loop.cpp`. It deletes nothing: `cleanCfg`, immediately below it in the
// round, is what removes the body a constant test has stranded.
void deadLoops(OptContext& opt);

/*
 * The second of two identical computations answered by the first, over the whole dominator tree -
 * `opt/cse.cpp`, and the loads with it.
 *
 * A pass of its own rather than a wider `simplify`: what a worklist over uses reaches for free is
 * the pair standing side by side in one block, and reaching the pair the *program* was written with
 * costs a dominator tree and a scope stack. The two share one statement of "these two instructions
 * are the same" - the predicates above.
 */
void cse(OptContext& opt);

/*
 * A computation whose answer cannot change between iterations, moved to the block in front of the
 * loop - `opt/loop.cpp`, beside `deadLoops` because both are questions about `Loops`.
 *
 * After `cse`, which is the ordering that matters: a value the two arms of a loop body spelled twice
 * is one value before this asks whether it is invariant, so it leaves the loop once instead of not
 * at all.
 */
void hoist(OptContext& opt);

/*
 * A record parameter that becomes one parameter per field - `opt/arg.cpp`.
 *
 * Program-wide and run once, in front of the inliner, for the reason that file's comment gives: it
 * is a calling convention rather than an optimization, so a signature this stage is going to rewrite
 * is rewritten once in the callee rather than once per copy the inliner makes of it.
 */
void flattenArguments(OptContext& opt);

/*
 * A call to a small callee replaced by a copy of its body - `opt/inline.cpp`.
 *
 * Program-wide and run once, before the per-function loop, for the reason README.md's section B
 * gives: what a copy buys is not the call it removes but that the seven passes below it can then
 * see through it. It runs its own rounds, and `markProgramReachable` at the top of each.
 */
void inlineCalls(OptContext& opt);

/*
 * Which functions the program can still arrive at, with `reachable` cleared on the ones it cannot -
 * `opt/reach.cpp`.
 *
 * The inliner's, and it is called from there rather than from the driver: a stale reachable set is
 * safe for *skipping* work and unsafe for a threshold, and the threshold is the sole-call-site rule
 * one round further down.
 */
void markProgramReachable(OptContext& opt);

/*
 * And the same question about module-level globals, asked **once, below everything else** -
 * `opt/reach.cpp`, where the two walks are separated.
 *
 * A global stops being reachable when the folder answers its last read, and the folder is a
 * per-function pass, so this is the one answer in the directory that cannot be computed until the
 * whole program has been optimized. Program-wide, and it writes `mir::Global::reachable`, which is
 * what `Lowering::declareGlobal` reads.
 */
void markReachableGlobals(OptContext& opt);

/*
 * The library's `checkFailed` branches removed, for `-no-checks` - `opt/check.cpp`.
 *
 * Program-wide like the two above, and the one pass that runs **above** the `optimizeIr` gate: what
 * the switch buys has to be free rather than cheap, and an ordinary pass would leave an unoptimized
 * build computing every bounds test and then keeping it. It answers whether anything went, because
 * on that path the driver runs nothing else and owes a `verifyProgram` only if this rewrote
 * something.
 */
bool dischargeChecks(OptContext& opt);

/*
 * The verifier checkpoint the driver puts behind every pass, named - `opt.cpp`.
 *
 * Here rather than only there because the inliner rewrites a body outside the per-function loop and
 * owes the same attribution: a pass is the smallest thing that can be blamed. Compiled out under a
 * release build, which is where the whole of its cost is.
 */
#if defined(_DEBUG) || defined(DEBUG)
void checkpoint(OptContext& opt, StringView where);
#else
inline void checkpoint(OptContext&, StringView) {}
#endif

// The driver's own fixed point over one body. Here rather than only in `opt.cpp` because the
// inliner needs it: what a call site is judged against is the callee as it will be *emitted*,
// and answering that is running the rounds on it - sizing a body in the form the front end wrote
// is sizing a body nothing emits.
void optimizeBody(OptContext& opt);

} // namespace ssa
