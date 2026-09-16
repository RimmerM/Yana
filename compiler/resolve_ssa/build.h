#pragma once

#include "inst.h"
#include "../resolve_mir/specialize.h"

namespace ssa {

/*
 * **The one place an occurrence becomes a convention** - §7.2's four rungs, in the order they are
 * asked:
 *
 *  - a position that is not owned is `Guaranteed`: the value is somebody else's and this body only
 *    reads it;
 *  - otherwise **linear** is `Owned`, which is "consumed exactly once" - `mir::isLinearAt` and not
 *    `mir::hasTeardown`, because a linear token with nothing to run has no teardown and is still
 *    the value the obligation exists for;
 *  - otherwise **not trivially copyable** is `Affine`: nothing to run, so it may be abandoned, and
 *    a second owner is the author's `Copy` rather than the bytes;
 *  - otherwise `Trivial`.
 *
 * A free function rather than a member of the conversion, because it is asked in two tiers: the
 * walk that converts MIR asks it of every position, and `Editor::addConst` asks it of a constant
 * built by a pass that has no conversion in hand. A second copy of these four rungs is how the two
 * would come to disagree about the one type the obligation is for.
 */
Convention conventionFor(DeclBase decl, const mir::Occurrence& occurrence);

/*
 * The one way SSA is built and rewritten - the gatekeeper the current `IrEditor` is, kept for the
 * tier where it belongs (§8.5).
 *
 * MIR is append-only and needs no editor; SSA is the optimizer's IR and does. Instruction order,
 * block arguments, CFG edges and use lists are four things that have to change together, and one
 * API is what keeps them from disagreeing - which is the same argument the existing editor makes,
 * and the reason it is one of the pieces this rewrite keeps rather than reconsiders.
 *
 * `Program` is the MIR stage's, because declarations are shared: a function here names an
 * `mir::Function`, never another body's instruction.
 */
struct Editor {
    // The building constructor: it *makes* a body, with its entry block and its root region, and
    // hands the function to it.
    Editor(Context& context, mir::Program& program, SsaArena& arena, mir::Function& function);

    /*
     * The attaching constructor, for a pass that rewrites a body somebody else built.
     *
     * There is no insertion point until `setBlock` says so - the creating form starts at the entry
     * because that is where a conversion begins, and a pass that appended wherever the last edit
     * happened to leave the cursor would be a pass whose output depends on the order of its own
     * rewrites. The region *is* set, to the entry's, because `addBlock` needs one and a block made
     * without a region is a block the verifier reports.
     *
     * `changed` is the fixed point's flag and is null everywhere else: only the destructive half
     * sets it, since appending to a body being built is not a reason to run the rounds again. What
     * is *not* a constructor argument is the version counter - it lives on the body, and `IrVersion`
     * is where the reason is written.
     */
    Editor(Context& context, mir::Program& program, SsaArena& arena, mir::Function& function,
           Body& body, bool* changed = nullptr);

    Context& context;
    mir::Program& program;
    SsaArena& arena;
    mir::Function& function;
    Body& body;
    bool* changed = nullptr;

    SsaBase base() { return *arena; }
    DeclBase declBase() { return *program.decl; }

    // A block in the region the editor is currently building into - see `enterRegion`. Almost every
    // block is in the body's own region, which is what makes the no-argument form the common one.
    Block* addBlock(StringId name);

    /*
     * A nested continuation region, and where it ends - `Continuation`.
     *
     * `enterRegion` opens one inside whatever is current and makes it current; `leaveRegion` returns
     * to the parent. A pair rather than a per-block argument because a region is a *span* of blocks -
     * the cleanup arm of a conditional drop is however many blocks that arm takes - and passing the
     * region at each `addBlock` is how one of them ends up in the wrong one.
     */
    ContinuationId enterRegion(Continuation::Kind kind, Continuation::Multiplicity multiplicity);
    void leaveRegion();

    // Which region a block made from here on belongs to. Read by the conversion, whose blocks all
    // exist before any of them is filled: an arm opened while one is being emitted has to nest in
    // *that* block's region and not in whichever one the previous block left current.
    void setRegion(ContinuationId region) { currentRegion = region; }

    // Where a region rejoins its parent - `Continuation::destination`. Separate from `leaveRegion`
    // because the block it names does not exist yet while the region is open: a cleanup arm's
    // successor is made after the arm, in the enclosing region, which is the whole point of it.
    void rejoinRegion(ContinuationId region, Block& destination);

    // Occurrences, not types: a merge of owned values and a merge of shared windows are the same
    // shape and different obligations, and the successor is where the obligation is declared.
    Value* addBlockArgument(Block& block, const Occurrence& occurrence, Convention convention);

    void setBlock(Block& block);
    Block* currentBlock() { return current; }

    /*
     * Where the next instruction goes: immediately in front of `before`, rather than at the end of
     * the current block.
     *
     * A pass needs this and a conversion does not, which is why it arrives with the destructive half
     * rather than beside `setBlock`. Every block a pass sees already has a terminator, so `append`
     * refuses all of them - and what a fold has to do is put the constant it made *above* the
     * instruction that reads it, since a value defined below its use is the first thing `checkUse`
     * reports.
     *
     * The cursor advances behind each instruction placed through it, so two of them added in a row
     * arrive in the order they were written and both still stand above `before`. It is cleared by
     * `setBlock`, which is the appending form and is what a builder wants.
     */
    void setInsertBefore(Inst& before);

    /*
     * Instructions. Each returns the instruction; `resultOf` is how a caller names what it defined,
     * and asserts that it defined one - a raw index would let a caller name a result that is not
     * there.
     */

    Inst* addConst(LocationId source, const mir::Constant& constant);
    Inst* addGlobalAddress(LocationId source, DeclPtr<mir::Global> global);
    Inst* addGenValue(LocationId source, DeclPtr<mir::Type> variable, DeclPtr<mir::Type> type);

    // One measurement of `of`, as a value of `as` - see `InstMetric`.
    Inst* addMetric(LocationId source, mir::TypeMetric metric, DeclPtr<mir::Type> of,
                    DeclPtr<mir::Type> as);
    /*
     * An operation the machine has and this IR does not - see `InstNative`.
     *
     * `hasResult` rather than a unit result, exactly as `addCall` takes one: `setMemory` answers
     * `{}` and a value of no bits is not a value this tier names.
     */
    Inst* addNative(LocationId source, mir::NativeOp op, const Use* arguments, Size count,
                    const Occurrence& result, Convention convention, bool hasResult);

    /*
     * One atomic operation - the location first, then the shape `mir::AtomicForm` states.
     *
     * `hasResult` for the same reason `addNative` takes one: three of the rows answer `{}`, and a
     * result nothing can name is a value this tier would leave dead behind every store and fence.
     */
    Inst* addAtomic(LocationId source, mir::AtomicOp op, mir::AtomicOrder order,
                    mir::AtomicOrder failure, bool weak, const Use* arguments, Size count,
                    const Occurrence& result, Convention convention, bool hasResult);

    // Whether the compare-exchange named by `cas` stored - a projection of an instruction that has
    // already run, so it takes the `Bool` the library declared as its result type.
    Inst* addAtomicOk(LocationId source, Use cas, const Occurrence& result);

    /*
     * The four vector operations - the MIR nodes carried down, with the result occurrence stated
     * because three of the four answer something their operand does not decide: a splat's lane
     * count, a shuffle's whole shape and a `firstset`'s integer.
     *
     * `Convention::Trivial` throughout and not a parameter: a vector duplicates freely and releases
     * nothing, which is `mir::ownershipOf`'s constant for the kind rather than anything a site here
     * could decide differently.
     */
    // `a * b + c` at most once rounded - `mir::NodeFma` carried down.
    Inst* addFma(LocationId source, Use lhs, Use rhs, Use addend, const Occurrence& result);

    // A branch-free choice - `mir::NodeSelect` carried down, with the result stated because this
    // tier states every result rather than deriving one.
    Inst* addSelect(LocationId source, Use condition, Use whenTrue, Use whenFalse,
                    const Occurrence& result, Convention convention);

    // A `match` over constants - `mir::NodeMap` carried down, with the entries copied into the SSA
    // region for the reason `InstMap` gives.
    Inst* addMap(LocationId source, Use selector, const mir::MapEntry* entries, Size count,
                 const mir::Constant& fallback, bool closed, const Occurrence& result,
                 Convention convention);

    Inst* addVecSplat(LocationId source, Use from, const Occurrence& result);
    Inst* addVecLane(LocationId source, Use from, U16 lane, const Occurrence& result);
    Inst* addVecWithLane(LocationId source, Use from, U16 lane, Use value,
                         const Occurrence& result);
    Inst* addVecShuffle(LocationId source, Use left, Use right, const U8* pattern, Size count,
                        const Occurrence& result);
    Inst* addVecReduce(LocationId source, Use from, mir::ReduceOp reduce,
                       const Occurrence& result);

    Inst* addAlloc(LocationId source, const Occurrence& held, StorageClass storage);

    // Storage for `capacity` elements, whose placement `opt/place.cpp` decides - `InstRunSlots`.
    Inst* addRunSlots(LocationId source, DeclPtr<mir::Type> element, U64 capacity);

    // The release that answers one of those - `InstFree`. No result: what it returns is memory.
    Inst* addFree(LocationId source, Use address);
    // `shape` is the *subject's* interior structure - see `InstFieldAddress::shape`, where the
    // reason it cannot be read off `base` is written out. Null for every record that owns its
    // fields.
    Inst* addFieldAddress(LocationId source, Use base, DeclPtr<mir::Type> record, U16 field,
                          DeclPtr<mir::OwnershipSchema> shape = nullptr);

    // The address of one element - `xs[i]`. `container` is the fixed array or window it is an
    // element of, and what the result points at is that container's element type.
    Inst* addElementAddress(LocationId source, Use base, Use index, DeclPtr<mir::Type> container);

    // The same, where the subject is a type parameter: the offset is the property slot's and `held`
    // is what the requirement promised is at it.
    Inst* addPropertyAddress(LocationId source, Use base, U16 property, DeclPtr<mir::Type> held);
    // `overread` is `InstLoad::overread` and defaults off - one MIR read in the language sets it,
    // and it travels from there to `LowerInstLoad::isOverread` unchanged.
    Inst* addLoad(LocationId source, Use address, const Occurrence& result, Convention convention,
                  bool overread = false);
    Inst* addCopy(LocationId source, Use address, const Occurrence& result, Convention convention);
    Inst* addStore(LocationId source, Use address, Use value);

    // A record from one value per field, in field order. The occurrence is the record's, and the
    // convention is `conventionFor`'s answer for it - the same function every other result goes
    // through, so an aggregate of a type with a teardown is an owner without this deciding so.
    Inst* addAggregate(LocationId source, U16 constructor, const Use* components, Size count,
                       const Occurrence& result, Convention convention);

    // The end of the life of what is at `address`. No result: what dies was in storage, and this is
    // the instruction that says so - see `InstDrop`.
    Inst* addDrop(LocationId source, Use address, const Occurrence& held,
                  bool boxesVisible = true);

    // The result convention is the caller's, not this call's: a comparison answers a `Bool` and an
    // addition answers its operand type, and which of those it is was settled in MIR. Passing it in
    // keeps `conventionFor` the one place an occurrence becomes a convention.
    Inst* addBinary(LocationId source, BinaryOp op, Use lhs, Use rhs,
                    const Occurrence& result, Convention convention);
    Inst* addUnary(LocationId source, UnaryOp op, Use value,
                   const Occurrence& result, Convention convention);

    // A function value - G7. The result is the function type, address-only, so what comes out is
    // storage the lowering fills with the pair; `environment` is null where the value captured
    // nothing.
    Inst* addFunValue(LocationId source, DeclPtr<mir::Function> callee, const Use* environment,
                      const Occurrence& result, Convention convention);

    /*
     * A call, direct or through a value. `hasResult` is false where the callee answers `{}`, and
     * that is a decision rather than a shortcut: a `{}` has no bytes and therefore no register and
     * no lower value, so a result value for one would be a value nothing below this can map. Making
     * it *absent* means an operand that tried to name it fails loudly at conversion instead of
     * silently lowering to nothing.
     *
     * The whole `mir::CallSite` rather than a callee, because "which function does this reach" has
     * one answer with four shapes and `callTarget` already produced it. A parameter list that took
     * the target alone would be one a caller could satisfy while dropping the half that says the
     * target is a slot. `target` is the fourth shape's other half - the value the call goes through,
     * read only where `site.through` says so - which is what keeps one entry point for a call rather
     * than two that have to agree about the argument list, the result and the convention.
     */
    Inst* addCall(LocationId source, const mir::CallSite& site, Use target, const Use* arguments,
                  Size count, const Occurrence& result, Convention convention, bool hasResult);

    Inst* addReturn(LocationId source, const Use* values, Size count);
    Inst* addJump(LocationId source, Block& target, const Use* arguments, Size count);
    // A two-way branch, with what both arms receive. One argument list for both edges, which is
    // `mir::Builder::addBranch`'s rule and is stated there: a value live before a branch is live on
    // both sides of it.
    Inst* addBranch(LocationId source, Use condition, Block& whenTrue, Block& whenFalse,
                    const Use* arguments, Size count);
    Inst* addUnreachable(LocationId source);

    /*
     * The same four terminators, built and **not placed** - what `replaceTerminator` takes.
     *
     * Detached because a terminator that replaces another cannot be appended first: the block
     * already has one, and `append` refuses to put an instruction behind a terminator. Nothing is
     * recorded for one of these until it is placed, so a caller that builds one and drops it costs
     * an allocation in a region that is released whole.
     */
    Inst* makeReturn(LocationId source, const Use* values, Size count);
    Inst* makeJump(LocationId source, Block& target, const Use* arguments, Size count);
    Inst* makeBranch(LocationId source, Use condition, Block& whenTrue, Block& whenFalse,
                     const Use* arguments, Size count);

    /*
     * The same, with **each edge's** operands stated separately.
     *
     * The one-list form above is the *builder's*: a conversion emits a branch whose two arms receive
     * the same values, which is `mir::Builder::addBranch`'s rule that a value live before a branch is
     * live on both sides of it. What it cannot express is a branch whose two targets declare
     * different arguments, and `opt/promote.cpp` makes those - a block argument is placed where one
     * slot's writes meet, and the two arms of a branch meet different sets of them.
     *
     * So a *pass* reproducing a branch that already exists takes this form and hands each edge back
     * what it was carrying. The one-list form is this one called with the same list twice, so there
     * is one place that builds a branch and the difference between the two is a parameter.
     */
    Inst* makeBranchEdges(LocationId source, Use condition,
                          Block& whenTrue, const Use* trueArguments, Size trueCount,
                          Block& whenFalse, const Use* falseArguments, Size falseCount);
    /*
     * A multi-way branch, made and appended - `mir::NodeSwitch` carried down.
     *
     * `values` and `targets` are parallel and ascending, and `arguments` is supplied to every edge
     * for `makeBranch`'s reason: a value live before a multi-way branch is live on every arm of it.
     */
    Inst* makeSwitch(LocationId source, Use selector, const I64* values, Block* const* targets,
                     Size count, Block& fallback, const Use* arguments, Size argumentCount);
    Inst* addSwitch(LocationId source, Use selector, const I64* values, Block* const* targets,
                    Size count, Block& fallback, const Use* arguments, Size argumentCount);

    /*
     * And the per-edge form, which is to `makeSwitch` exactly what `makeBranchEdges` is to
     * `makeBranch` - and is needed for the same reason it is.
     *
     * A `switch` is *built* with one list shared by every arm, because that is what a value live
     * before it means. `promote` then places a block argument where a slot's writes meet and pushes
     * the operand onto **the arms that reach that block**, so a standing `switch` may carry a
     * different list on every edge - which is what a pass copying one has to preserve. Copying the
     * fallback's list onto the cases instead was a wrong answer twice over: the counts disagree and
     * the builder says so, and where they happen to agree the case edge would silently hand its
     * target the fallback's value.
     *
     * `counts` is one length per case and `arguments` their lists laid end to end, in case order.
     * The fallback's is passed separately, because it is the one edge that is not a case.
     */
    Inst* makeSwitchEdges(LocationId source, Use selector, const I64* values, Block* const* targets,
                          const Size* counts, const Use* arguments, Size count, Block& fallback,
                          const Use* fallbackArguments, Size fallbackCount);

    Inst* makeUnreachable(LocationId source);

    Value* resultOf(Inst& inst, Size index = 0);
    Use use(Value& value, UseKind kind);

    /*
     * Fills the predecessor lists from the edges. Derived, so it is computed once the forward
     * direction is complete rather than maintained twice while it is being built.
     *
     * Which is the one thing that separates the building half above from the destructive half
     * below: `addJump` and `addBranch` create an edge and record **no** predecessor entry, because
     * a conversion emits every block before it seals any of them. A *pass* has no such point, so a
     * pass gives a block its terminator through `replaceTerminator`, which keeps both records of
     * every edge it moves. `verifyBody` compares the two, so getting it wrong is reported rather
     * than carried.
     */
    void seal();

    /*
     * The loan on one set of storage, made once - `Loan`.
     *
     * Interned by the storage set and the capability together, so that two borrows rooted in the
     * same places are one loan and end at one point. An empty set is the caller's loan, which is the
     * whole of what a borrowed parameter depends on; there is one of those per capability and it
     * never ends inside this body.
     */
    Loan* loanOn(LocationId source, const SsaPtr<Value>* storage, Size count, Access capability);

    /*
     * What a borrowed value depends on, once the caller knows - `Value::loan`.
     *
     * Separate from creating the value because the two answers come from different places: the
     * convention is the occurrence's and is known where the instruction is built, and the lender is
     * the *origin environment's* and is known only to the conversion, which has the MIR body's loan
     * table in hand. A value keeps the caller's loan until this says otherwise, which is the
     * permissive answer and the right default for one this body cannot root.
     */
    void setDependency(Value& value, Loan& loan);

    /*
     * ## The destructive half
     *
     * Everything above builds; everything below takes something out or moves it, and each of them
     * bumps `Body::version` and sets `changed`. What they have in common is that none of them is a
     * policy: whether a block *should* be merged or a branch *may* be folded is a pass's question
     * and lives in `opt/`, and what is here is the mechanical consequence of having decided.
     */

    /*
     * One instruction taken out of circulation: it stops counting as a user of everything it read,
     * the edges it owned stop counting in their targets' `incoming`, and it is dropped from its
     * block's list.
     *
     * **The table entry is left standing.** `Body::insts` is a `RegionArray` indexed by `InstId` and
     * so is every side table over instructions, so an erase is an unlink and never a compaction -
     * the same rule `Block::deleted` follows one level up.
     *
     * Refuses an instruction whose results still have readers, which is the assertion every caller
     * that removes a *live* value has already satisfied by pointing its readers somewhere with
     * `replaceAllUses`. A caller that has not is the bug this catches.
     *
     * A loan whose storage set names one of the results has that entry removed. A set that empties
     * becomes the caller's loan - one that never ends inside this body - and that is sound here for
     * exactly the reason above: the result has no readers, so no live borrow depends on it.
     */
    void eraseInst(Inst& inst);

    /*
     * Every reader of one value pointed at another - the operand slots and the use-list entries
     * moved together, and **each use's `UseKind` preserved**: a consuming use of the old value is a
     * consuming use of the new one, and silently demoting one is how an owner comes to be consumed
     * twice.
     *
     * Two conventions are asserted rather than checked, because a caller that wants either of them
     * is a caller that has already decided something the linear rule can see:
     *
     *  - `isTracked` must agree. Replacing an owner by a trivial value drops a consumption
     *    obligation from every path that had one, and replacing a trivial value by an owner invents
     *    one; neither is a substitution, and both read as a correct rewrite afterwards.
     *  - a **borrow is replaced only by a borrow, on the same loan or on a loan over fewer lenders**.
     *    The loan is what the extent rule is checked against, so a borrow rerouted onto a different
     *    body-local loan is one whose uses are now measured against endpoints in a different place -
     *    and a use that was inside the old extent may be outside the new one with nothing in the IR
     *    changed to say so. A loan ends when *any* of its lenders is released, so one over a subset
     *    lasts at least as long: every use inside the wider extent is inside the narrower loan's,
     *    and the direction is the whole rule - a superset shortens the extent and is refused.
     *    `lendsWithin` is that test, and the caller's loan (an empty storage set, and the default
     *    `addResult` gives every borrow it makes) is its degenerate case rather than a special one.
     *    What needed the general form is the inliner: `rootLender` re-roots a callee's loan exactly,
     *    so `fn firstOf(x: 'Int, y: 'Int) -> 'Int = x` comes back rooted in the first argument's
     *    lender where the *call's* result was on the group both arguments published.
     *
     * Loans whose storage set named `from` are repointed at `to`, and that half is not optional: a
     * loan is found by its lender, so one still naming a value nothing defines is a loan whose
     * endpoints are never reached and a borrow of storage that has been handed back then verifies.
     */
    void replaceAllUses(Value& from, Value& to);

    /*
     * The same substitution narrowed to **one reader** - every slot a single instruction names
     * `from` through, exchanged for `to`, and every other reader left alone.
     *
     * The two conventions above are asserted here too and for the same reasons: what makes a
     * substitution sound is a property of the pair, not of how many users it is applied to. What
     * this drops is only the sweep, so a caller that wants the whole use list still writes
     * `replaceAllUses` and gets the loan repointing with it - a single-reader rewrite has no
     * lender to move, because `from` still exists and its other readers still hold it.
     *
     * `opt/cfg.cpp`'s `decideBranchConditions` is the caller: a value one arm of a branch has
     * decided is that constant in the blocks that arm alone reaches and is still itself everywhere
     * else, so "every use in these blocks" is the rewrite and "every use" is not.
     */
    void replaceUsesIn(Inst& user, Value& from, Value& to);

    /*
     * One instruction moved, by unlink and relink and **never by a remove followed by an add**.
     *
     * The operands already name the instruction, so re-adding registers every use a second time -
     * and the symptom is quiet, because an inflated use count reads as "more than one reader" and
     * every single-reader rewrite then declines. That is the lower IR's `addInst` bug, which cost a
     * day and one instruction per container `push` across thirty fixtures.
     *
     * `position` is the index the instruction ends up at in `target`, counted after it has left
     * where it was. A terminator is refused: moving one changes the CFG, and `replaceTerminator`
     * and `splitBlock` are the two operations that do.
     */
    void moveInst(Inst& inst, Block& target, Size position);

    /*
     * One block cut in two at `position`: the instructions from that point on, the terminator
     * included, move to a fresh block, whose successors are told their edges now leave from it.
     *
     * The new block is in **the same continuation region**, which is the rule for every block a pass
     * makes: a split is one block written as two and the region is a statement about what those
     * blocks are the arm of. It does not claim the region's `entry` - control still enters at the
     * first half, which is the block that kept the identity.
     *
     * The first half is left with **no terminator**, which is the one state a block may be in that
     * the verifier reports: the caller decides what leaves it, and a `jump` to the answer is the
     * ordinary one.
     */
    Block* splitBlock(Block& block, Size position);

    /*
     * A block taken out of the body - see `Block::deleted`, where the reason the entry stays in the
     * table is written.
     *
     * Asserts it has no incoming edges and is not the body's entry, then erases its instructions
     * from the last to the first. Last first is what makes the erases legal: the terminator goes
     * before anything it named, and inside a block a definition always precedes its readers. What is
     * *not* covered is a value defined here and read from another block, which `eraseInst`'s own
     * assertion catches - a caller deleting a block whose values something still reads has skipped a
     * step rather than found a special case.
     */
    void deleteBlock(Block& block);

    /*
     * Where one arm of a terminator goes, changed: the edge's target, one `incoming` entry moved
     * from the old block to the new.
     *
     * By **ordinal**, not by target. `branch %c, X, X` is legal, so "the edge from here to X" names
     * two of them, and the old stage's version that took a target changed one arm of the branch,
     * both edge records and every matching predecessor entry - three different answers to one
     * question. Told which arm, this moves exactly one of each.
     *
     * The new target must declare the arguments the edge already supplies. An edge whose list does
     * not match its target is IR the verifier rejects, so it is refused where it is made rather than
     * reported two passes later; a redirect to a block of a different shape is a *different* edge,
     * and `replaceTerminator` is what builds one.
     */
    void redirectEdge(Inst& terminator, Size arm, Block& to);

    /*
     * A block argument declared on a block that is **already built**, and one operand appended to
     * one edge - the two halves of adding an argument to a body somebody else wrote.
     *
     * Two operations rather than one, and that is not a convenience: the construction that needs
     * them cannot supply the operands when it declares the argument, because a loop header's
     * incoming value is a value the header itself defines. So between the declaration and the last
     * fill the body is in a state `verifyBody` rejects - an edge supplying fewer operands than its
     * target declares - and closing that window is the caller's obligation rather than an assertion
     * this can make. `opt/promote.cpp` is the one caller and the only one that should be.
     *
     * The building form above stays as it was and keeps refusing a filled block. What it refuses is
     * a *builder* naming a block it has already emitted into, which is a mistake with no legitimate
     * shape; a pass rewriting a finished body is the legitimate shape, and it says so by name.
     *
     * `addEdgeArgument` takes the arm by **ordinal**, for `redirectEdge`'s reason: `branch %c, X, X`
     * is two edges into one block and each owes its own operand.
     */
    Value* declareBlockArgument(Block& block, const Occurrence& occurrence, Convention convention);
    void addEdgeArgument(Inst& terminator, Size arm, Use value);

    /*
     * One operand an edge already carries, exchanged for another - the arm by ordinal and the
     * position within its list.
     *
     * Separate from `replaceTerminator`, which keeps the arguments of an edge present on both sides
     * *over whatever the caller supplied*: that rule is what makes folding a branch into a jump safe
     * without knowing what the target was being handed, and this is the edit that means the
     * opposite. `opt/select.cpp`, placing the operands of an edge it has just built, is the caller.
     */
    void setEdgeArgument(Inst& terminator, Size arm, Size position, Use value);

    /*
     * One block argument removed, with the operand it was fed by dropped from every incoming edge.
     *
     * Every edge, not every predecessor: a predecessor whose two arms both land here supplies the
     * argument twice and owes two removals, which is the same duplicate-arm rule `incoming` follows.
     *
     * The argument must have no readers. A caller that has pointed them somewhere with
     * `replaceAllUses` has already satisfied that, and one that has not is removing a parameter the
     * block still uses.
     */
    void removeBlockArgument(Block& block, Size index);

    /*
     * A block's terminator replaced, and with it the edges the old one owned.
     *
     * The two successor lists are compared **as multisets** rather than swapped wholesale, and that
     * is the whole of what makes this usable. An edge present in both is left exactly as it was -
     * its `incoming` entry stays and the arguments it carried are copied onto the new edge, over
     * whatever the caller supplied. Fold `branch %c, then, else` into `jump then` and the edge into
     * `then` is one that was already there: removing and re-adding it would take the arguments
     * `then` was being handed and put none back, and there is nothing at the fold site that knows
     * what they were. Only surplus old edges lose their `incoming` entry and only surplus new ones
     * gain one.
     *
     * `branch %c, X, X` needs no case of its own and gets none: there were two edges into `X`, the
     * match is greedy by ordinal, and folding leaves one of each record.
     *
     * The terminator is one of the `make*` forms above - built and not placed, because the block
     * still has the one being replaced. A block with no terminator is allowed to have this called on
     * it, which is how a split's first half is finished.
     */
    Inst* replaceTerminator(Block& block, Inst* terminator);

private:
    Block* current = nullptr;
    ContinuationId currentRegion;

    // Where `append` puts what it is given: at the end of `current` where this is `kAppend`, and at
    // this index otherwise - see `setInsertBefore`.
    static constexpr Size kAppend = ~Size(0);
    Size cursor = kAppend;

    Inst* append(Inst* inst);
    Value* addResult(Inst& inst, const Occurrence& occurrence, Convention convention);

    // The value and the entry in `Block::arguments`, without the question of whether this block may
    // still be given one - which is what the two public forms above differ on and nothing else.
    Value* makeBlockArgument(Block& block, const Occurrence& occurrence, Convention convention);
    void recordUse(Inst& inst, Use used);

    // The half-update primitives, private for the reason the old stage's are: with them in reach,
    // "drop the old use, repoint the operand, record the new one" is three statements a pass can get
    // two-thirds of.
    void dropUse(Value& value, Inst& user);
    void dropUses(Inst& inst);
    void unlinkInst(Inst& inst);
    void insertInst(Block& block, Size position, Inst& inst);

    // One `incoming` entry naming `from` taken out of `target`, and one put in. One rather than all,
    // because the list holds an entry per edge - see `Block::incoming`.
    void dropIncoming(Block& target, Block& from);
    void addIncoming(Block& target, Block& from);

    // One value taken out of every loan's storage set, and one exchanged for another - the half of
    // `eraseInst` and `replaceAllUses` that is about the lender rather than about an operand.
    void forgetStorage(SsaPtr<Value> storage);
    void repointStorage(SsaPtr<Value> from, SsaPtr<Value> to);

    void markChanged() { if(changed) *changed = true; }

    // The two halves of `IrVersion`. A CFG edit is a value edit as well - see the type, where the
    // reason the coarser of the two answers is the right one is written down.
    void markValues() { body.version.values++; }
    void markBlocks() { body.version.blocks++; body.version.values++; }
};

} // namespace ssa
