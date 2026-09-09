#pragma once

#include "../dominance.h"
#include "../../util/container.h"

namespace ssa {

struct OptContext;

/*
 * The whole-function answers a pass reads once at the top and holds, and the stamp that decides
 * whether asking again recomputes one.
 *
 * `Dominators` itself is not here - it is `resolve_ssa/dominance.h`, beside the verifier, because
 * the verifier and the lowering need it in builds this directory is switched off in. What is here
 * is the *caching* of it, and the analyses only a pass wants.
 */

/*
 * When one cached analysis was computed, and from what - the shipping stage's `AnalysisStamp`,
 * unchanged, because the argument for it is unchanged.
 *
 * Each answer is stamped with the function it was read from and the `IrVersion` the body was at,
 * and is handed back rather than recomputed while both still hold. The fixed point runs its passes
 * over a body several times and most of the later rounds change nothing - that is what the later
 * rounds are *for*, discovering there is nothing left to do - so the difference is between one
 * dominator tree per function and one per pass per round.
 *
 * `overValues` is which structure the answer is a statement about - see `IrVersion`. A dominator
 * tree and the loops derived from it are statements about the block graph alone, so folding an
 * operand or erasing an instruction leaves them standing; an answer about allocations and their use
 * lists does not survive either.
 *
 * The function is compared as well as the version, because `opt.function` is assigned per body and
 * the inliner will settle a callee in the middle of describing a call site: a stamp naming another
 * function is not an answer about this one whatever the IR has done since.
 */
struct AnalysisStamp {
    mir::Function* function = nullptr;

    // 64 bits for the reason the counters are: a stamp is only wrong if a counter wraps exactly
    // onto the value it holds, and a width no compilation can exhaust is cheaper than the argument
    // that no compilation can exhaust a narrower one.
    U64 values = 0;
    U64 blocks = 0;

    bool holds(mir::Function* current, const IrVersion& now, bool overValues) const {
        if(function != current || blocks != now.blocks) return false;
        return !overValues || values == now.values;
    }

    void take(mir::Function* current, const IrVersion& now) {
        function = current;
        values = now.values;
        blocks = now.blocks;
    }
};

/*
 * One natural loop: the header every iteration passes through, the blocks that reach the back edge,
 * and the single block outside that leads into it.
 *
 * `preheader` is `kNone` where there is no such block, and a loop with none is one nothing may be
 * hoisted out of - there is nowhere to put it that runs exactly once per entry.
 *
 * The block set is an `IndexSet` and there is no second list of the same blocks beside it, which is
 * where this differs from the shipping stage's `Loop`. That list was rebuilt from the set every
 * round of every pass that asked for the loops, and a walk over the set answers the same question -
 * `forEach` visits the members lowest first. What the set is also the measure of is nesting: an
 * inner loop's blocks are a subset of its container's, so the sort below is by `popCount`.
 */
struct Loop {
    static constexpr U32 kNone = ~U32(0);

    U32 header = 0;
    U32 preheader = kNone;
    IndexSet contains;
};

/*
 * A body's natural loops, innermost first - so a value hoisted out of an inner loop lands where the
 * next round can hoist it out of the one containing it.
 *
 * One loop per header rather than one per back edge: two edges back to the same block are one loop
 * with two ways round, which is what a header *is*. A back edge is an edge whose target dominates
 * its source, and the dominator tree is therefore the input rather than something recomputed here.
 */
struct Loops {
    Array<Loop> all;

    Size size() const { return all.size(); }
    Loop& operator [] (Size index) { return all[index]; }
    const Loop& operator [] (Size index) const { return all[index]; }

    auto begin() { return all.begin(); }
    auto end() { return all.end(); }

    void build(SsaBase base, Body& body, const Dominators& dominators);
};

/*
 * Which blocks control can get to, as a reader of its own.
 *
 * It holds no array, and that is the decision: `Dominators` already answers it - a block absent
 * from the reverse postorder has no position - so a second copy would be a second thing to
 * invalidate and a second chance for the two to disagree. What this buys over asking the tree
 * directly is that a pass which wants only reachability says only that.
 */
struct Reachable {
    const Dominators* dominators = nullptr;

    bool operator [] (U32 block) const { return dominators->isReachable(block); }
    bool operator [] (BlockId block) const { return dominators->isReachable(block.index); }
};

/*
 * The cached forms, and the only way a pass should ask - see `AnalysisStamp`.
 *
 * Each is the same two lines: hand back what is there if the stamp still holds, otherwise recompute
 * and stamp it. The buffers belong to the stage, so a recompute reuses whatever the last function
 * left allocated and only the answer is rebuilt.
 *
 * A pass reads one of these *once*, at the top, and holds the reference across its own rewrites -
 * which is the same staleness a private copy would have had. What the cache adds is that asking
 * again after a rewrite gets a fresh answer, so a pass that wants the earlier reading has to keep
 * holding its reference rather than re-asking.
 */
Dominators& dominatorsOf(OptContext& opt);
Loops& loopsOf(OptContext& opt);
Reachable reachableOf(OptContext& opt);

/*
 * Per `Alloc`, whether every use of its address keeps that address inside this frame.
 *
 * The shipping stage's `computeContainment`, over an `Alloc`'s `Value::uses` instead of a local's
 * use list - and the same argument for why the use list answers it at all: every instruction that
 * reads or writes the storage names the allocation, so an allocation whose users are all such
 * instructions has had its address handed to nothing. It was not stored anywhere, not returned and
 * not captured, because each of those is an instruction and each would be in the list.
 *
 * What is admitted is a `Load`, a `Store` **through** it, a `Copy`, a `Drop`, an `Atomic` whose
 * location it is, and a `FieldAddress`/`ElementAddress` whose own result is contained - which is the
 * recursion, and is what makes `x.a.b` a projection rather than an escape. A `Load` recurses too, and
 * that is the half a use-list scan does not see: there is no `Borrow` instruction at this tier, so
 * `&x` handed to a callee is a `load` of the slot whose result is `Guaranteed` and whose loan names
 * the allocation - a second name for the storage, admitted only while it does not escape either. The
 * other three conventions are contents rather than a way back: `Trivial` is the value, `Owned` is the
 * value taken out, and `Address` is a pointer that was *in* the slot. Everything else declines:
 * an `Aggregate` component, a `Store` of the address *as a value*, a `Native` operand, a `Return`,
 * an edge operand, a `FunValue` environment. There is no default that admits an unrecognized kind,
 * because forgetting too much costs an optimization and forgetting too little is a miscompile.
 *
 * **A call argument is admitted at a position the callee does not `escape`**, which is the shipping
 * stage's `callKeepsStorage` over an inferred fact instead of a declared one. It was `retains` and
 * it was the wrong row - that one is a reach set over *values*, so a callee writing a pointer back
 * into the storage it was lent read as keeping the address; the arm in `Containment::admits` is
 * where that argument is written out. It was an outright escape until J25 filled the row, for the
 * honest reason that `Unknown` reads back as `ArgumentSet::all()` and admitting an argument on that
 * answer is admitting it on no answer at all; that is still what an unfilled row does.
 *
 * What the admission leaves behind is the other half - what the callee wrote through the address
 * while it had it - and that is deliberately **not** here. `mayWriteStorage` asks it, of the call
 * itself, through the callee's `mutates` row. Two rows read in two places on purpose: retention is
 * about the *address* and mutation is about the *storage*, and one predicate answering both would be
 * a containment that quietly means "and never written", which is not what any reader asks it.
 *
 * It is the one analysis here that is a statement about **values** rather than about blocks, so it
 * is the one whose stamp passes `overValues`: folding an operand or erasing an instruction can move
 * an allocation from escaping to contained, and a cache that survived that would be answering about
 * a use list that no longer exists.
 */
struct Contained {
    // Indexed by `ValueId`, and set only for a value an `Alloc` defines. Everything else reads
    // false, which is the answer a question about an allocation has for a value that is not one.
    IndexSet inside;

    bool operator [] (const Value& value) const { return inside[value.id.index]; }
    bool operator [] (const Value* value) const { return value && inside[value->id.index]; }

    void build(OptContext& opt);
};

Contained& containedOf(OptContext& opt);

/*
 * Per `Alloc`, the constant its storage holds - for the whole of the body, not for a block.
 *
 * A **string literal** is what this exists for. `Resolver::textValue` builds the value the library's
 * `stringLiteral` describes, so a literal arrives as `const String {...}` stored into a frame slot,
 * and every question a program asks of a literal is then a read of that slot: its length, the
 * pointer to its bytes, and the `ownsHeap` flag its teardown tests. Block-local forwarding loses all
 * three at the first call, because a literal is always lent to something - `==`, `length`, a
 * formatter - on the way to the teardown that reads its flag. This is the same fact asked of the
 * **function** instead, and it rests on nothing being able to write the storage:
 *
 *  - the allocation is `Contained`, so no callee was handed a way back to it that it kept;
 *  - exactly one instruction writes it, a whole-slot `Store` of a value an `InstConst` defines;
 *  - and every other instruction of the body is asked `mayWriteStorage` and says no - which is one
 *    question rather than an allowlist of readers, so a kind nobody has looked at yet is a barrier
 *    rather than an omission.
 *
 * **A `Drop` of the slot is admitted**, and it is the one exception. A teardown is handed the
 * address and `mayWriteStorage` therefore says it may write - correctly - but a drop is the end of
 * the value's life: nothing may read the storage after it without something re-filling it first,
 * and a re-fill is a second write this rule has already excluded. So whatever a teardown writes
 * cannot be read, and admitting it is what lets the flag its own body tests be answered by the
 * constant that was put there. That is the shipping stage's `constantLocalContents` ruling, arriving
 * with the containment and the mutation rows inferred rather than declared.
 *
 * A statement about **values**, so its stamp passes `overValues` for `Contained`'s reason.
 */
struct ConstantContents {
    // Indexed by `ValueId`. Unset for every value that is not an allocation holding a constant,
    // which is nearly all of them - the same shape `Contained` uses, with a value instead of a bit.
    Array<mir::Constant> held;

    mir::Constant operator [] (const Value& value) const {
        return value.id.index < held.size() ? held[value.id.index] : mir::Constant();
    }

    void build(OptContext& opt, const Contained& contained);
};

ConstantContents& constantContentsOf(OptContext& opt);

/*
 * What a read of this address answers, where that is settled before the program runs - and unset
 * where it is not.
 *
 * Two roots and one walk. An **immutable global** holds its own initializer, which is what makes a
 * read out of a placed constant a number; an `Alloc` in `ConstantContents` holds what was stored
 * into it. Between the root and the address are the field and element projections, indexed through
 * `mir::constantField` so that "which value of the list is field `n`" has one answer at both tiers.
 *
 * A `Guaranteed` load is walked through, because at this tier that is how a borrow of a slot is
 * spelled and it names the same storage. A `represent` is **not**: it hands back the same bytes at a
 * different type, and a constant indexed past one would be read at the wrong shape.
 */
Maybe<mir::Constant> constantAt(OptContext& opt, const ConstantContents& contents, Value* address);

/*
 * The allocation an address is rooted in, or null where this body cannot name one.
 *
 * The walk up the `FieldAddress`/`ElementAddress` chain, which is what makes "these two addresses
 * are in different allocations" answerable at all. Null is the permissive answer and covers every
 * root that is not a frame allocation: a global's address, a pointer loaded out of storage, a
 * borrowed parameter arriving as a block argument. Two addresses one of which has no root may name
 * the same bytes, and the aliasing rules in `opt/memory.cpp` read it that way.
 */
Value* allocationRoot(OptContext& opt, Value* address);

/*
 * One step up an address chain, or null where the chain ends.
 *
 * The three ways an address names storage another value already names - a field or element
 * projection, a `Guaranteed` load (which at this tier is how a borrow is spelled and is no
 * instruction in the lowering), and `represent`. Exported because `cse` compares two chains rather
 * than only walking one, and a second copy of the list would be a second answer to keep in step.
 */
Value* addressStep(OptContext& opt, Value* address);

/*
 * Whether one instruction may write an allocation's storage, or storage reachable from it.
 *
 * The question `forwardMemory` asks of everything between a write and a read, and the one place the
 * answer is written down. Three kinds of answer meet in it:
 *
 *  - a **structural** one, where the instruction names the storage: a `Store` through an address
 *    rooted here, an `Atomic` on one, a `Drop` of one - a teardown receives an address and may write
 *    through it - and a `Load` whose result is **tracked**, which is the handover and leaves the
 *    slot empty, which is a write however it is spelled;
 *  - an **opaque** one, where the instruction is a `Call`, a `Native` or any kind this does not
 *    recognize. `Contained` is what proves such an instruction disjoint from this allocation: a
 *    callee cannot write storage whose address it was never given. A call is now the case that
 *    needs both facts - containment admits an argument the callee does not retain, so "contained"
 *    means "no call kept it" rather than "no call saw it", and the positions *this* call was handed
 *    it at are asked against the callee's `mutates` row. An unfilled row is `ArgumentSet::all()`,
 *    which is the barrier this was before either fact existed;
 *  - and **no**, for everything pure, for an `Alloc` (storage that has just come into existence) and
 *    for a `Copy`, which reads its address and writes storage of its own that nothing else names.
 *
 * `Contained` is a **parameter** rather than something this asks for, and that is the reading
 * discipline above made unavoidable: a pass holds one answer across its own rewrites, and a question
 * asked per instruction per entry that re-stamped the cache would rebuild the whole analysis at
 * every erase.
 *
 * What makes holding it sound is narrower than "the pass only erases", and the difference is worth
 * stating because it is a condition on the *caller* rather than a property of this function. An
 * erase can only take an allocation out of escaping, never put one in - but `forwardMemory` also
 * calls `replaceAllUses`, which **adds** uses, and a use added to an allocation's list is exactly
 * how a contained one would come to escape. The reading survives because of what that substitution
 * is: both sides are `Trivial` (`Forwarder::isForwardable`), so it never retargets a use onto an
 * address, and an allocation's value is an `Address`. `forwardMemory` asserts that where it
 * substitutes. A pass that ever forwarded an address would have to re-ask rather than hold.
 */
bool mayWriteStorage(OptContext& opt, const Contained& contained, Inst& inst, Value& allocation);


} // namespace ssa
