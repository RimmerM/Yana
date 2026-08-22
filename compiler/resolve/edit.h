#pragma once

#include "module.h"

/*
 * The one place the resolve IR is edited - Analysis-Status.md's second structural improvement.
 *
 * The IR states most of what it knows twice. An instruction names its operands and every value names
 * its readers; a block lists its instructions and every instruction names its block; an edge is
 * written down in the predecessor's `outgoing`, the successor's `incoming` and one alternative per
 * phi in the successor; and a local slot names the value its storage came from while that value
 * names the slot back. Every one of those pairings is a thing a rewrite can leave half-updated, and
 * every one of them fails the same way: the IR prints correctly and walks wrongly, so the symptom is
 * a crash inside a backend or a wrong number rather than a diagnostic naming the pass.
 *
 * `verifyFunction` (verify.h) is the half of the answer that *detects* that. This is the half that
 * prevents it. Six things move together here and nowhere else:
 *
 *  - block contents, and the `Value::block` back edge;
 *  - def-use lists, in both directions;
 *  - CFG edges, in all three of the places one is recorded;
 *  - phi inputs, whose block half is an edge and whose value half is a use;
 *  - the `Local::value` / `Value::slot` pairing;
 *  - the storage a place is *rooted* in, which is a use with no operand slot holding it.
 *
 * `Block`'s lists are private and this is the only friend, and so are `Value::uses` and
 * `Function::setLocalValue`. A reader still reads them - through the accessors on those types, which
 * hand out contents rather than the list.
 *
 * ## How much that actually guarantees
 *
 * Precisely this: **the records cannot be written without going through here.** A pass cannot push a
 * use, drop one, splice an instruction into a block or move an edge on its own, and the three-step
 * form of a half-update - drop the old use, repoint the field, record the new one - is not writable
 * either, because the pieces it is made of are private and `rewriteOperands` is the whole of what is
 * offered instead.
 *
 * It is *not* a proof that an instruction cannot change what it names. Operand fields are public -
 * `InstBinary::lhs`, `InstJe::cond` - so a pass can still assign one and say nothing, and one
 * deliberately does: `commute` in opt_fold.cpp swaps a binary's two operands, which changes nothing
 * about the multiset and therefore owes no bookkeeping at all. Making that unwritable would mean
 * privatizing the operand of every instruction kind, which is a large change against a small risk,
 * and it would put the honest case above behind an accessor too.
 *
 * So the boundary is: every rewrite that *changes a use* goes through this type, and nothing here
 * can be half-done. What stands behind the rest is `verifyFunction`, which recomputes both
 * directions and compares - and which is why that check is worth its cost rather than redundant.
 *
 * ## What is deliberately not here
 *
 * Policy. Whether a block *should* be merged, whether a branch *may* be folded, which instruction is
 * worth hoisting: those are the passes' own and they live in compiler/opt. What is here is the
 * mechanical consequence of having decided - `spliceInto` performs a merge and refuses to judge one.
 *
 * And `rebuildUses`, which is now what its name says: a repair, reached by the verifier's own tests
 * and by nothing in normal operation. Every pass maintains its lists as it goes.
 */

// A short run of instructions built before it is spliced into a block: what one packed store expands
// into, what materializing an argument takes, what an inlined body emits per instruction. Eight
// inline, since these are expansions of a single instruction - the count is decided by the widest
// expansion in a pass rather than by anything about the program being compiled.
using InstList = SmallArray<Inst*, 8>;

/*
 * How many times each of the IR's two structures has been written, for whoever is caching an answer
 * derived from one of them.
 *
 * An analysis over a function is a function of the IR, so it stops being the answer exactly when the
 * IR it was read from changes - and "changed" is not one question. A dominator tree is a statement
 * about the blocks and the edges between them, and no amount of rewriting *inside* a block can make
 * it wrong; the storage a callee can reach is a statement about instructions and use lists, and no
 * amount of moving blocks around can. The optimizer's fixed point runs a dozen passes over a
 * function up to eight times and most of the later rounds change nothing at all, so the two facts
 * above are the difference between computing each analysis once and computing it thirty times.
 *
 * A counter rather than a flag, because there is no point at which a reader may clear one: two
 * analyses over the same structure are cached independently and would each clear the other's notice.
 * A cache holds the counter it was built at and compares.
 *
 * A CFG edit bumps both, deliberately. Every operation below that moves an edge also moves the phi
 * alternative arriving over it or the terminator owning it, both of which are uses - and the two
 * that arguably do not, `setBlockOrder` and `redirectSuccessor`, are rare enough that distinguishing
 * them would buy nothing but a second rule to get wrong.
 */
struct IrVersion {
    // Instructions, their operands, the use lists, and the `Local::value` / `Value::slot` pairing.
    U64 values = 1;

    // The block set, the block order, and the edges between blocks.
    U64 blocks = 1;
};

struct IrEditor {
    /*
     * One function's worth of editing.
     *
     * `changed` is the optimizer's fixed-point flag and is null everywhere else: the resolver builds
     * an IR rather than rewriting one, so there is nothing there for a round to be repeated over.
     * Only the operations that are a *rewrite* set it, which is the same set of them that set it
     * before this type existed.
     *
     * `version` is null in the same places and is *not* the same question - see IrVersion. Every
     * operation here that writes anything bumps it, including the ones that deliberately leave
     * `changed` alone: appending an instruction is not on its own a reason to run the fixed point
     * again, and it is absolutely a reason to stop believing a cached use-list analysis.
     */
    IrEditor(Module& module, Function& function, bool* changed = nullptr,
             IrVersion* version = nullptr);

    Module& module;
    Function& function;
    ModuleBase base;
    bool* changed;
    IrVersion* version;

    // ---- uses -------------------------------------------------------------------------------

    /*
     * One entry of one use list, which is what everything below is made of.
     *
     * Exposed because there is one caller that has to record a use the ordinary path could not. A
     * specialization's local table is filled in *after* its body, since the value each slot holds is
     * produced by an instruction that has to be cloned first - so every place rooted in a local was
     * added while the slot still held nothing, and `recordUses` had no value to attribute the use
     * to. See cloneBody, which pays exactly the slots that were empty at the time and no others.
     *
     * A use recorded twice is as wrong as one not recorded at all - `dropUse` removes one entry per
     * naming - so a caller reaching for this is one that knows precisely which entries are missing.
     */
    void recordUse(ModulePtr<Value> value, ModulePtr<Inst> user);

    // Pointing every reader of one value at another, use lists and operands together.
    void replaceValue(ModulePtr<Value> from, ModulePtr<Value> to);

    /*
     * One instruction's operands changed, with the use lists moved to match - the transactional form
     * of the three-step dance a peephole would otherwise write by hand.
     *
     * `f` is handed the instruction and may do anything to what it names: repoint one field, swap
     * two, rewrite a whole argument list to a different length. Everything it read stops counting
     * before `f` runs and everything it reads counts afterwards, so there is no state in between in
     * which one direction is stated and the other is not - which is the shape of half-update this
     * type exists to make unwritable. The pieces it is built out of - `dropUse`, `dropUses`,
     * `recordUses` - are private for exactly that reason: with them in reach, "drop the old use,
     * repoint the field, record the new one" is three statements a pass can get two-thirds of.
     *
     * It costs a walk of every operand rather than the one that changed. Every caller is a peephole
     * over one instruction, so that is a handful of pointers against a rule that cannot be got wrong.
     */
    template<class F>
    void rewriteOperands(ModulePtr<Inst> user, F&& f) {
        dropUses(user);
        f(*base[user]);
        recordUses(base[user]);
        markValues();
    }

    /*
     * The one-field spelling, which is what most callers want: `replaceOperand(pointer,
     * instruction.lhs, folded)`.
     *
     * The reference is into the instruction itself and stays valid across the drop - nothing here
     * moves the arena.
     */
    void replaceOperand(ModulePtr<Inst> user, ModulePtr<Value>& operand, ModulePtr<Value> value) {
        rewriteOperands(user, [&](Value&) { operand = value; });
    }

    // ---- local slots ------------------------------------------------------------------------

    /*
     * Every local slot one value was the whole contents of, emptied.
     *
     * `Local::value` is the other half of `Value::slot` - see Function::setLocalValue - and a slot
     * left naming an instruction that is no longer in any block is storage whose provenance every
     * later pass reads and gets a wrong answer from: `eachPlaceRootValue` attributes a place's use
     * to it, `storageOf` hands it back as the root to project from, and `lowerProgram` asks it who
     * reads the storage and is told "nobody", which is how a slot with readers came to look like one
     * that could stay in registers.
     *
     * By scan rather than through `Value::slot`, and that is the whole reason this is a function.
     * `Value::slot` holds *one* answer while several slots may name one value - the inliner points
     * every slot that named a call at the value that replaced it, deliberately, since a class
     * default reached through an instance ends up with two of them. Clearing only the one the value
     * names back leaves the others behind.
     */
    void forgetLocalValue(ModulePtr<Value> value);

    /*
     * The same slots, refilled from another value rather than emptied - what a pass that *replaced*
     * an instruction and then took it out of its block owes instead. The storage did not stop
     * existing; it is now named by whatever the readers were pointed at.
     *
     * Lowest last, so that the `Value::slot` back edge names the lowest slot of the several that may
     * hold one value. That is the answer `findPlace` and `backingLocal` give.
     *
     * **Except where `to` already fills a slot of its own**, which is the case a collapsing phi
     * produces and the one this may not answer by writing a second slot. See `mergeIntoLocal`.
     */
    void repointLocalValue(ModulePtr<Value> from, ModulePtr<Value> to);

    /*
     * Two locals that turned out to name one piece of storage, made one.
     *
     * Every place rooted in `from` is re-rooted in `to` and `from`'s slot is emptied, so that what
     * is left is one local for one allocation. That is an invariant most of `compiler/opt` reasons
     * from without saying so: opt_promote.cpp's proof is "the only places that can overlap one of
     * this local's fields are other places rooted in the same local", opt_scalar.cpp removes a
     * local whose own place is written and never read, and both are false the moment two locals
     * name one `Alloc`.
     *
     * A collapsing phi is what makes that happen. `let m = if c then Just(x) else Nothing` gives
     * the binding a slot whose value is a phi of the two arms' allocations; folding the condition
     * leaves one arm, and repointing `m`'s slot at that arm's `Alloc` - which already has a slot -
     * is a second name for it. `promotePlaces` then read `m`'s discriminant, found nothing writing
     * *that* local, and answered with the zero an unwritten local holds: every such `if` came out
     * as the wrong constructor.
     *
     * The use lists need no repair. A place rooted in a local is recorded as a use of that local's
     * storage value - see `addPlaceUse` - and both slots hold the same value here, so re-rooting
     * moves a use from one name of it to another.
     */
    void mergeIntoLocal(U32 from, U32 to);

    /*
     * One slot pointed at the value that fills it - the pairing's two fields, `Local::value` and
     * `Value::slot`, written together.
     *
     * `forgetLocalValue` and `repointLocalValue` above are the *searching* forms, for a caller that
     * knows a value and not which slots hold it. This is the direct one, and the four callers are
     * the two halves of specialization, the inliner splicing a result, and opt_arg giving a
     * flattened parameter storage of its own.
     */
    void setLocalValue(U32 index, ModulePtr<Value> value);

    // ---- instructions -----------------------------------------------------------------------

    /*
     * One instruction appended to a block, uses and edges recorded.
     *
     * The one entry point for putting anything into a block, and it takes all three kinds: a phi
     * joins the incoming edges, a terminator creates the outgoing ones, and everything else goes on
     * the end of the instruction list. A block that already has a terminator is asserted against -
     * see `setTerminator`, which is what replacing one means.
     */
    Inst* append(Block& block, Inst* inst);

    /*
     * The same, at a position rather than at the end. Appended and then reordered, because appending
     * is what records the uses and a list written by hand would be one more place for the two
     * directions to disagree. They land in the order given, in front of whatever was at `index`.
     */
    void insert(Block& block, Size index, InstList& instructions);

    // A permutation of one block's instruction list. Nothing about a use, an edge or a slot changes
    // - which is what makes reordering the one structural edit with no bookkeeping attached to it.
    void reorder(Block& block, Buffer<ModulePtr<Inst>> order);

    /*
     * Taking an instruction out of circulation: it stops counting as a user of everything it read,
     * every slot it was the whole contents of is emptied, and it is dropped from its block.
     *
     * `eraseInstruction` is the same thing for an instruction nothing reads, which it asserts -
     * every caller that removes a *live* instruction has pointed its readers somewhere first, and
     * one that has not is the bug this catches.
     */
    void removeInstruction(ModulePtr<Inst> instruction);
    void eraseInstruction(ModulePtr<Inst> instruction);

    // A phi removed from its block, and from the use lists of everything it read. Separate because a
    // phi is not in the instruction list - it is in the phi list, which is what makes "before
    // everything else in the block" a property of the IR rather than of the order things were added.
    void erasePhi(ModulePtr<InstPhi> phi);

    /*
     * One instruction moved to the end of another block's list, which is where the hoisting pass
     * puts an invariant value; and the whole of one block's list, which is what a merge and an
     * if-conversion move. A use list records who reads a value rather than where from, so nothing
     * about one changes here - what does is the `Value::block` back edge.
     */
    void moveInstruction(ModulePtr<Inst> instruction, Block& target);
    void moveInstructions(Block& source, Block& target);

    // ---- phis -------------------------------------------------------------------------------

    // One alternative added to a phi, which is a use of the value it names. The block half has to be
    // a predecessor already; this does not create the edge.
    void addPhiInput(ModulePtr<InstPhi> phi, PhiInput input);

    // One alternative removed, by position, dropping the use with it.
    void removePhiInput(ModulePtr<InstPhi> phi, Size index);

    // ---- control flow -----------------------------------------------------------------------

    /*
     * A block's terminator replaced, and with it the edges the old one owned.
     *
     * The two successor sets are compared as multisets rather than swapped wholesale, and that is
     * the whole of what makes this usable: an edge in both is *left alone*, so the phi alternatives
     * arriving over it survive. Fold `je %c, then, else` to `jmp then` and the edge into `then` is
     * one that was already there - removing and re-adding it would take every alternative in `then`
     * with it and put none back. Only the surplus old edges are removed, with their alternatives,
     * and only the surplus new ones are created.
     *
     * A `je` with both arms at one block is not a special case: there were two edges into it, and
     * folding leaves one.
     */
    void setTerminator(Block& block, Inst* terminator);

    // A block left with no terminator and no outgoing edges - what one half of a split has until the
    // other half is built, and what a merged-away block ends as.
    void clearTerminator(Block& block);

    // One edge from `from` into `into`, removed: the predecessor entry and the one phi alternative
    // that arrived over it. One of each rather than all, because two edges between the same pair of
    // blocks are two entries and removing one is what folding one of them means.
    void removeEdge(ModulePtr<Block> into, ModulePtr<Block> from);

    /*
     * A successor's record of where one edge arrived from, pointed at a different block: its
     * predecessor entry and every phi alternative that named the old one. Both halves, because a phi
     * is a value the *predecessors* produce and an alternative left naming a block the edge no
     * longer leaves from is an input no backend can find a copy for.
     *
     * Every match rather than the first, since a `je` with both arms at one block leaves two of each.
     */
    void retargetEdge(Block& target, ModulePtr<Block> from, ModulePtr<Block> to);

    /*
     * Where a terminator jumps, changed without rebuilding it: the branch's own field, the
     * predecessor's `outgoing`, and one edge record moved per arm that named `oldTarget`.
     *
     * Answers how many arms moved, which is nothing a caller wants to know and everything a caller
     * that *loops* needs: a walk that empties a predecessor list one redirect at a time terminates
     * only if each round removes an entry, and this returning zero is exactly the case where none
     * did. That happens when the entry is stale - `oldTarget` names `from` as a predecessor and
     * `from` no longer branches there - which is a broken IR rather than a shape, and the honest
     * response to it is to stop rather than to spin.
     */
    Size redirectSuccessor(Block& from, ModulePtr<Block> oldTarget, ModulePtr<Block> newTarget);

    // Every incoming edge forgotten, without touching the predecessors - which is only correct for a
    // block whose predecessors have gone or have been pointed elsewhere.
    void clearPredecessors(Block& block);

    /*
     * One block cut in two after `index`: the instructions behind that point, the terminator, and
     * with the terminator every edge the block owned, all move to a fresh block. Answers the new one.
     *
     * The two halves of an edge move together. A successor records where an edge came from twice -
     * once in its predecessor list and once per phi alternative - and both would otherwise name a
     * block the edge no longer leaves from.
     */
    Block* splitBlock(Block& block, Size index);

    /*
     * A fresh block inserted on one edge, which is where a drop that only some paths owe goes: the
     * alternative is the top of the successor, and that is only correct where every path agreed.
     *
     * Answers the new block, with the edge already rerouted through it and its own jump in place.
     *
     * The edge is named by *ordinal* rather than by its successor, and that is the difference
     * between a total operation and one with a hole in it: `je %c, X, X` is legal, and "the edge
     * from here to X" then names two of them. Told which arm, this moves exactly one predecessor
     * entry and one alternative per phi, so splitting each arm in turn leaves two split blocks and
     * three representations that still agree. Told a successor, it could only guess - and the
     * version that guessed changed one arm of the branch, both entries of `outgoing`, and every
     * matching predecessor entry, which is three different answers to one question.
     */
    Block* splitEdge(Block& from, Size successor);

    /*
     * And the spelling that used to be the signature, deleted rather than absent.
     *
     * `ModulePtr` converts to `U32` implicitly, so `splitEdge(block, someSuccessorPointer)` compiles
     * clean and passes an arena offset as an ordinal. That is not a hypothetical - it is what the
     * one caller did after this changed shape, and the build said nothing. Naming the overload is
     * what turns it into an error at the call site that has to be read.
     */
    Block* splitEdge(Block& from, ModulePtr<Block> to) = delete;

    /*
     * One block folded into another that jumps to it - the inverse of a split, and the mechanical
     * half of a merge. The instructions move, the terminator and its edges move, and the successors
     * are told the edges now leave from `into`.
     *
     * Refuses nothing: whether the shape is one that may be merged is `mergeBlocks`' question, and
     * the guards for it are there.
     */
    void spliceInto(Block& into, Block& block);

    /*
     * A block that is about to stop existing, taken out of the function's bookkeeping.
     *
     * Every instruction, phi and terminator in it stops being a user of what it read, every slot one
     * of them filled is emptied, and every edge it owned is removed from the successor that recorded
     * it - predecessor entry and phi alternative together. What is *not* touched is its own contents:
     * the caller is about to drop the block from `Function::blocks`, and a list nothing can reach
     * costs nothing.
     */
    void discardBlock(Block& block);

    // The function's block list, rewritten. A block's index is its position in this list - which
    // every walk in opt_flow.cpp assumes - so writing the list is what renumbers.
    void setBlockOrder(Buffer<ModulePtr<Block>> order);

    // ---- repair -----------------------------------------------------------------------------

    /*
     * Every use list recomputed from the instructions that exist.
     *
     * Nothing in the pipeline calls it, and that is the point: everything above maintains both
     * directions as it goes, so there is no pass whose output needs repairing. It is kept because it
     * is the only *independent* statement of what a use list should hold - neutering one of the
     * operations above and comparing what this computes against what the passes kept is how
     * `verifyFunction`'s def-use check is shown to bite - and because a caller that arrived at a
     * function some other way has nowhere else to turn.
     */
    void rebuildUses();

private:
    // Removing one entry from a value's use list. One rather than all: an instruction naming the
    // same value twice appears twice, and the list has to keep saying so.
    void dropUse(ModulePtr<Value> value, ModulePtr<Inst> user);

    /*
     * One instruction recorded as a user of everything it names, and the inverse.
     *
     * `recordUses` is a switch of its own rather than a walk of `eachOperand`, and that is the whole
     * reason `verifyFunction`'s def-use check is a check: two independent statements of one list,
     * compared. `dropUses` is the walking side, so an operand the switch invents or misses is a use
     * count that never balances - which is exactly what the verifier reports.
     */
    void recordUses(Inst* inst);
    void dropUses(ModulePtr<Inst> inst);

    // One edge's record in a successor, repointed - the first matching predecessor entry and the
    // first matching alternative of each phi, rather than every match. That is what makes an edge
    // splittable one arm at a time; `retargetEdge` above is the whole-block form, where every edge
    // leaving one block now leaves another and moving all of them is the point.
    void retargetEdgeOnce(Block& target, ModulePtr<Block> from, ModulePtr<Block> to);

    // A terminator and the edges it owns, handed from one block to another - what a split gives its
    // continuation and what a merge gives the block absorbing it. The successors are told the edges
    // now leave from `to`, which is the half that is easy to write and easy to forget.
    void transferTerminator(Block& from, Block& to);

    void addUse(ModulePtr<Value> value, Inst* user);
    void addPlaceUse(const Place& place, Inst* user);
    void recordEdges(Inst* terminator, ModulePtr<Block> from);
    // `target` needs room for kMaxSuccessors, and every slot is written - the arms this terminator
    // has, and null for the rest, which is exactly what a block's own outgoing slots hold.
    static Size successorsOf(const Value& terminator, ModulePtr<Block>* target);

    void markChanged() { if(changed) *changed = true; }

    // The two halves of IrVersion. A CFG edit is a value edit as well - see the type, where the
    // reason the coarser of the two answers is the right one is written down.
    void markValues() { if(version) version->values++; }
    void markBlocks() { if(version) { version->blocks++; version->values++; } }
};
