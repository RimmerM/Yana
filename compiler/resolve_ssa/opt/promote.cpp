#include "pass.h"

/*
 * Storage held in a value instead - README.md's section A row for `promotePlaces`, brought back to
 * this tier from the lower one.
 *
 * ## Why it is here and not left below
 *
 * The port delegated the cross-block half of this to `promoteStackSlots`, on the argument that a
 * local nothing borrows costs nothing by the time it is machine code. That is true of the emitted
 * bytes and false of everything above them, and phases 4 to 9 measured it four times over. `range`
 * was built, fired **zero** times and was removed, because both of its rewrites need one SSA value
 * read in two blocks and no named value at this tier was one - 627 zero tests asked, not one of them
 * with a second test of the same value anywhere in the body. `convertSelects` fired 99 times instead
 * of 151 until it grew an exception for a `load`, because every short-circuit arm holds the load of a
 * parameter's slot. The inliner's `sizeOf` counts a store and a load per parameter before a body does
 * anything, so a budget of twenty is spent on traffic that is not emitted. And `threadBranches`
 * misses `let ok = ...` in front of `if ok`, because the binding puts a store and a load in the join
 * and a join is threadable only if it holds its branch and nothing else.
 *
 * It also contradicts the plan for the second backend. `Analysis-IR-JS.md` Part 6.1 has JSIR consume
 * OSSA directly and never see the lower IR, so SROA, place forwarding, CSE and loop simplification
 * have to be *here* or that backend does without them - and a promotion that lives one tier down is
 * one tier below where half the consumers are.
 *
 * So promotion is an SSA-tier pass and it runs **first** in the per-function list, in front of
 * `simplify`, because everything below it is written against values and this is what turns storage
 * into one. What does not change is `.ssa.expect`: that is the conversion's output and the ownership
 * golden, and `resolve_ssa/README.md`'s ruling about not making it depend on an optimization applies
 * with more force to the optimizer than it did to the conversion.
 *
 * ## What is eligible
 *
 * An `Alloc` in `StorageClass::Frame` whose held occurrence owes neither a teardown nor a disposal,
 * that `Contained` calls contained, and **every one of whose uses is a whole-allocation `Load`, a
 * whole-allocation `Store`, or a `Drop` that lowers to nothing**. A `FieldAddress`, an
 * `ElementAddress`, a `Copy`, an `Atomic`, a call argument or a store of the address as a value each
 * decline it, and each of them is a use `Contained` has already refused - so what this walk adds over
 * that analysis is the two conditions below.
 *
 * **Every load's result and every store's value are `Trivial` and agree on their whole occurrence.**
 * The convention half is the linear rule: what replaces the loads is one value per definition, and
 * duplicating an owner or a borrow is not a substitution - `Editor::replaceAllUses` refuses both. The
 * occurrence half is `Forwarder::isForwardable`'s rule and has the same reason: a value's occurrence
 * is what `lower/manifest.cpp` manifests it from, and a block argument declares exactly one. Whether
 * two occurrences happen to manifest alike is the **target's** answer and `optimizeProgram` runs once
 * per program, which is the refusal `fold.cpp` makes about a width and `memory.cpp` about a sum's
 * fields.
 *
 * That is what keeps a **borrowed** parameter's slot in storage. `fn f(n: Int)` stores an
 * `I32 [shared call] guaranteed` into a slot its loads read back as a plain `I32 trivial`, so the
 * pair is a convention change and an occurrence change at once - phase 6 declined the same
 * substitution for the same reason and its note says so. A `->` parameter of a trivial type is an
 * ordinary owned occurrence at both ends and promotes with every other local, and so does every
 * parameter slot of an **inlined** body, where what the store installs is the caller's own value.
 *
 * A **borrow of the allocation** is refused by the same clause and it is worth stating on its own,
 * because `Contained` admits one: there is no `Borrow` instruction at this tier, so `&x` is a `load`
 * whose result is `Guaranteed` and whose loan names the allocation, and containment recurses into it
 * rather than declining it. Here the load's result is not `Trivial`, so the allocation is not
 * promoted - which is the only answer available, since the loan names storage the promotion would
 * remove and a loan with no lender is a loan whose endpoints are never found.
 *
 * Three smaller answers, each of which had to be decided rather than assumed:
 *
 *  - **narrow `@bits(n)` and `Bool` need no case.** A value at this tier is held in its declared
 *    width's normal form - `resolve_ssa/lower/width.cpp` states it and `fold.cpp` computes in it - so
 *    the value a store writes and the value a load reads back are the same normal form and the
 *    rewrite is a renaming. The occurrence rule above is what says so without asking a width at all.
 *  - **a vector, a float and a pointer held as a value all promote**, because each of them arrives
 *    `Trivial`: `conventionFor` sends a trivially-copyable owned occurrence there, and a raw pointer
 *    is one. What does not promote is a slot holding an **`Address`** - the address of another
 *    allocation, a global or a field - and the reason is `allocationRoot`: it walks a chain of
 *    projections back to a defining `Alloc`, a block argument has no definer, so promoting one would
 *    trade a load for an alias root `opt/memory.cpp` can never bound again. `Contained` refuses most
 *    of these already, since a store of an address *as a value* is an escape of that address.
 *  - **an allocation with no reads is left alone.** `removeDeadStorage` takes it two passes later
 *    with the writes, and doing it here would place block arguments for a value nothing reads only to
 *    take them back.
 *
 * ## The construction
 *
 * The lower tier's, cited rather than reinvented - `lower/lower_promote.cpp`, whose file comment is
 * the argument for it. A forward availability dataflow says where each slot is written on every path
 * in; the blocks are then walked in **reverse postorder**, so what a block carries in is already
 * known from its predecessors, and a block argument is placed only where they disagree or where one
 * of them has not been visited - a back edge, so a loop header. That is the same set of arguments the
 * iterated dominance frontier would place, reached without computing one, and it is what the note
 * over there records after building every phi and deleting all but a handful of them cost fourteen
 * seconds on one body.
 *
 * Forwarding a predecessor's value is sound because it dominates: every path here runs through some
 * predecessor, each of those is dominated by the definition, so this block is too - which is why the
 * agreement has to be over **all** the incoming edges rather than the ones already walked.
 *
 * **A read of a slot nothing has written on every path into it declines the slot**, exactly as it
 * does one tier down, rather than being answered with a zero. A register may not hold what was never
 * put in it, and this tier has a second reason the lower one does not: the value would have to be
 * given an occurrence and a convention out of nothing, and a constant of the slot's type is not
 * available for a record or a vector. Nothing in the corpus reaches it - the conversion writes a
 * local before it reads one - and a slot that did would be one the *next* round promotes if
 * `cleanCfg` removed the path.
 *
 * ## What this pass declines outright
 *
 * **A body with an unreachable block is not promoted at all.** `verifyBody` checks such a block
 * structurally, and dominance is one of the things it checks - nothing dominates a block with no
 * position in the order - so an argument placed there, or an operand handed over an edge that leaves
 * one, is reported at this pass's own checkpoint. `cleanCfg` deletes them in the same round and the
 * next round promotes what this one declined, so the cost is a round rather than an opportunity. The
 * alternative was reasoning about paths control never takes, which is reasoning about nothing.
 *
 * ## Ownership
 *
 * Every value this pass moves is `Trivial`, at both ends of every substitution, so the linear rule is
 * untouched: nothing gains a consumption obligation, nothing loses one, and no borrow changes which
 * loan it is measured against. `Editor::replaceAllUses` asserts both halves at every site, which is
 * what makes that a statement rather than a claim.
 */

namespace ssa {

namespace {

constexpr U32 kNoSlot = ~U32(0);

/*
 * One allocation this pass decided it can hold in values.
 *
 * `occurrence` is what every load and every store of it agreed on, and is what the block arguments
 * declare. `column` is which candidate this was while the survey ran, and therefore which bit of
 * each block's availability row answers for it - the row is kept per block rather than per slot, so
 * dropping a candidate does not renumber it.
 */
struct Slot {
    Value* allocation = nullptr;
    Inst* alloc = nullptr;
    Occurrence occurrence;
    U32 column = 0;

    // Per block, indexed by `BlockId`: what the slot holds on the way in, and on the way out.
    Array<Value*> entry;
    Array<Value*> exit;
};

// Where this pass placed a block argument, and which slot it carries. A list rather than a walk of
// `Slot::entry`, because that array names an argument only where one was needed - a block whose
// predecessors agree carries their value in directly.
//
// The argument itself used to be here as well, for the removal `promote` carried; that is
// `removeTrivialArguments` now and finds its own, so what is left is the two numbers `fill` reads.
struct Placed {
    U32 block = 0;
    U32 slot = 0;
};

struct Promoter {
    OptContext& opt;

    /*
     * Read once at the top and held, on `analysis.h`'s discipline.
     *
     * Holding it is sound here for a stronger reason than it is in `memory.cpp`: this pass reads
     * containment only while it is *choosing* slots, before it has edited anything, and every edit
     * afterwards removes a use.
     */
    const Contained& contained;
    const Dominators& dominators;

    Array<Slot> slots;
    Array<Placed> placed;

    // Which slot an allocation is, by `ValueId`, and `kNoSlot` for every value that is not one.
    Array<U32> slotOf;

    // Where each slot is written on every path in, one row per block over the *candidate*
    // numbering - see `Slot::column`.
    IndexSetList available;

    // What each slot holds at the point the block walk has reached, one entry per slot.
    Array<Value*> current;

    U32 blockCount = 0;

    Value* addressOf(Use used) {
        return used.isSet() ? opt.base[used.value()] : nullptr;
    }

    // A value the collection never saw is not a slot - which includes every block argument this
    // pass has itself placed, since `slotOf` is sized before any of them exists.
    U32 slotAt(Value* value) {
        if(!value || value->id.index >= slotOf.size()) return kNoSlot;
        return slotOf[value->id.index];
    }

    // The slot an instruction reads and the slot it writes, asked separately because a `Drop` is
    // neither and every other kind is one or the other or nothing.
    U32 readSlot(Inst& inst) {
        if(inst.kind != Inst::Load) return kNoSlot;
        return slotAt(addressOf(((InstLoad&)inst).address));
    }

    U32 writtenSlot(Inst& inst) {
        if(inst.kind != Inst::Store) return kNoSlot;
        return slotAt(addressOf(((InstStore&)inst).address));
    }

    /*
     * Whether one allocation's whole use list is loads, stores and teardown-free drops of the whole
     * of it, agreeing on one `Trivial` occurrence - the eligibility rule, and the file comment is
     * the argument for each clause.
     */
    bool eligible(Value& allocation, Occurrence& agreed) {
        auto& alloc = (InstAlloc&)*opt.base[allocation.definer];

        // Frame storage only, and the held occurrence owing nothing - `removeDeadStorage`'s two
        // questions, asked here for the same reason: what this ends with is deleting the request for
        // the storage, which is a complete answer exactly where the frame supplied it and where
        // nothing was owed when it went away.
        if(alloc.storage != mir::StorageClass::Frame) return false;
        if(hasTeardownAt(opt, alloc.held) || isLinearAt(opt, alloc.held)) return false;
        if(!contained[allocation]) return false;

        auto typed = false;
        auto read = false;

        auto agree = [&](Value& value) {
            if(value.convention != Convention::Trivial) return false;

            if(!typed) {
                agreed = value.occurrence;
                typed = true;
                return true;
            }

            return agreed == value.occurrence;
        };

        for(auto userPointer: allocation.uses(opt.base)) {
            auto& user = *opt.base[userPointer];
            auto names = [&](Use used) { return addressOf(used) == &allocation; };

            switch(user.kind) {
                case Inst::Load: {
                    if(!names(((InstLoad&)user).address)) return false;
                    if(user.results.size() != 1) return false;
                    if(!agree(*opt.base[user.results.get(opt.base, 0)])) return false;

                    read = true;
                    break;
                }

                case Inst::Store: {
                    auto& store = (InstStore&)user;

                    // Through it and not *of* it: a store of the allocation's own address is the
                    // address escaping into storage, which `Contained` has already refused and which
                    // this states again because the two operands are one use list entry each.
                    if(!names(store.address) || names(store.value)) return false;
                    if(!agree(*opt.base[store.value.value()])) return false;
                    break;
                }

                case Inst::Drop: {
                    auto& drop = (InstDrop&)user;
                    if(!names(drop.address)) return false;

                    // Erased with the allocation, which is sound exactly where the teardown emits no
                    // instruction - `Lowering::emitTeardown` returns immediately for an occurrence
                    // that owes neither half, and `isLinearAt` beside it is the disposal an author
                    // may still have written.
                    if(hasTeardownAt(opt, drop.held) || isLinearAt(opt, drop.held)) return false;
                    break;
                }

                default:
                    return false;
            }

            // A use in a block control cannot reach is what the file comment declines the whole body
            // for; this is the same refusal read off one allocation, in case a body ever holds one
            // that this pass was not given the chance to decline.
            if(!user.block || !dominators.isReachable(opt.base[user.block]->index.index)) {
                return false;
            }
        }

        // No reads, no promotion: an allocation nothing reads is `removeDeadStorage`'s, and placing
        // arguments for a value nothing names would be work only to take it back.
        return typed && read;
    }

    void collect() {
        slotOf.clear();
        for(Size i = 0; i < opt.body->values.size(); i++) slotOf.push(kNoSlot);

        for(auto valuePointer: opt.body->values.contents(opt.base)) {
            auto value = opt.base[valuePointer];
            if(!value->definer) continue;

            auto definer = opt.base[value->definer];
            if(definer->kind != Inst::Alloc || !definer->block) continue;

            Occurrence agreed;
            if(!eligible(*value, agreed)) continue;

            Slot slot;
            slot.allocation = value;
            slot.alloc = definer;
            slot.occurrence = agreed;
            slot.column = U32(slots.size());

            slotOf[value->id.index] = U32(slots.size());
            slots.push(::move(slot));
        }
    }

    /*
     * Where every slot is known to hold something, and which slots survive - `lower_promote.cpp`'s
     * `surveySlots`, and the transpose is the point there and here: one row of bits per block rather
     * than a walk of the function per slot.
     *
     * `in[b]` is the AND over the predecessors' `out`, `out[b]` is `in[b]` plus what the block
     * writes, and the fixpoint is solved from the **optimistic** end - every block starts written and
     * the rounds take away what is not. Starting from the pessimistic end is wrong rather than
     * conservative: an AND over predecessors cannot become true inside a loop unless it was already
     * true when the latch was first looked at, so "false everywhere" is a fixpoint of every cycle,
     * and what that costs is every local live across a nested one.
     *
     * The fixpoint walks the reverse postorder, and every `incoming` entry counts in the AND with no
     * question asked about whether control reaches the predecessor. That is exact rather than
     * conservative here for one reason: `run` has already declined a body holding a block nothing
     * reaches, so the order is every live block and every predecessor is one of them. Without that
     * refusal the AND would have to leave an unreachable predecessor out - availability is a
     * statement about the paths from the entry, and there is no path through such a block - and an
     * unreachable *cycle* would then stay optimistically full with nothing to take it back.
     */
    void survey(IndexSet& usable) {
        auto count = slots.size();

        usable.reset(count);
        usable.fill();

        IndexSetList stores;
        IndexSetList out;
        stores.reset(blockCount, count);
        available.reset(blockCount, count);
        out.reset(blockCount, count);

        for(U32 i = 0; i < blockCount; i++) {
            auto block = opt.body->block(opt.base, BlockId(i));
            if(block->deleted) continue;

            for(auto pointer: block->instructions(opt.base)) {
                auto written = writtenSlot(*opt.base[pointer]);
                if(written != kNoSlot) stores[i].set(written, true);
            }
        }

        for(U32 i = 0; i < blockCount; i++) {
            available[i].fill();
            out[i].fill();
        }

        IndexSet incoming;
        incoming.reset(count);

        for(auto changed = true; changed;) {
            changed = false;

            for(auto index: dominators.order) {
                auto block = opt.body->block(opt.base, BlockId(index));

                auto edges = Size(0);
                for(auto pointer: block->incoming(opt.base)) {
                    auto from = opt.base[pointer]->index.index;

                    if(!edges) incoming.copyFrom(out[from]);
                    else incoming.intersectWith(out[from]);

                    edges++;
                }

                if(!edges) incoming.reset(count);

                if(!available[index].equals(incoming)) {
                    available[index].copyFrom(incoming);
                    changed = true;
                }

                incoming.unionWith(stores[index]);

                if(!out[index].equals(incoming)) {
                    out[index].copyFrom(incoming);
                    changed = true;
                }
            }
        }

        // And the one thing that can still disqualify a slot: a read of storage nothing has written
        // on every path into it. Per slot out of the same walk, since "written by here" is a bit in
        // a row like every other.
        IndexSet written;

        for(auto index: dominators.order) {
            auto block = opt.body->block(opt.base, BlockId(index));
            written.copyFrom(available[index]);

            for(auto pointer: block->instructions(opt.base)) {
                auto& inst = *opt.base[pointer];

                auto read = readSlot(inst);
                if(read != kNoSlot && !written[read]) usable.set(read, false);

                auto writes = writtenSlot(inst);
                if(writes != kNoSlot) written.set(writes, true);
            }
        }
    }

    // The candidates the survey kept, renumbered, with each one's availability column carried so
    // that the rows built above still answer for it.
    bool keep(const IndexSet& usable) {
        Array<Slot> kept;

        for(Size i = 0; i < slots.size(); i++) {
            if(!usable[i]) continue;
            kept.push(::move(slots[i]));
        }

        slots = ::move(kept);
        if(slots.isEmpty()) return false;

        for(Size i = 0; i < slotOf.size(); i++) slotOf[i] = kNoSlot;
        for(Size i = 0; i < slots.size(); i++) {
            slotOf[slots[i].allocation->id.index] = U32(i);

            for(U32 b = 0; b < blockCount; b++) {
                slots[i].entry.push(nullptr);
                slots[i].exit.push(nullptr);
            }
        }

        return true;
    }

    /*
     * One block: the arguments it needs on entry, then the accesses taken out of it.
     *
     * The rewrite is a walk over a snapshot of the instruction list, because every erase below
     * removes an entry from the list being walked - the same shape `Forwarder::run` takes and for the
     * same reason.
     */
    void enter(Editor& editor, Block& block) {
        auto index = block.index.index;

        for(Size i = 0; i < slots.size(); i++) {
            auto& slot = slots[i];
            if(!available[index][slot.column]) { current[i] = nullptr; continue; }

            Value* only = nullptr;
            auto agreed = true;

            for(auto pointer: block.incoming(opt.base)) {
                auto value = slot.exit[opt.base[pointer]->index.index];
                if(!value) { agreed = false; break; }

                if(!only) only = value;
                else if(only != value) { agreed = false; break; }
            }

            /*
             * A predecessor whose exit is not yet known is a back edge - so a loop header - and an
             * unvisited predecessor reads here as a disagreement, which is what places the argument.
             * Availability is the statement that every predecessor leaves something here, so `only`
             * being null means the walk has not been to any of them.
             */
            if(agreed && only) {
                slot.entry[index] = only;
                current[i] = only;
                continue;
            }

            auto argument = editor.declareBlockArgument(block, slot.occurrence, Convention::Trivial);
            slot.entry[index] = argument;
            current[i] = argument;
            placed.push(Placed { index, U32(i) });
        }

        SmallArray<SsaPtr<Inst>, 32> instructions;
        for(auto pointer: block.instructions(opt.base)) instructions.push(pointer);

        for(auto pointer: instructions) {
            auto& inst = *opt.base[pointer];
            if(!inst.block) continue;

            if(inst.kind == Inst::Store) {
                auto which = writtenSlot(inst);
                if(which == kNoSlot) continue;

                current[which] = opt.base[((InstStore&)inst).value.value()];
                editor.eraseInst(inst);
                continue;
            }

            if(inst.kind == Inst::Load) {
                auto which = readSlot(inst);
                if(which == kNoSlot) continue;

                // Guaranteed by the survey: a load of a slot that holds nothing is exactly what
                // disqualified the slot, so reaching one here is that walk having disagreed with
                // this one.
                assertTrue(current[which] != nullptr);

                editor.replaceAllUses(*opt.base[inst.results.get(opt.base, 0)], *current[which]);
                editor.eraseInst(inst);
                continue;
            }

            if(inst.kind == Inst::Drop) {
                auto which = slotAt(addressOf(((InstDrop&)inst).address));
                if(which == kNoSlot) continue;

                editor.eraseInst(inst);
                continue;
            }
        }

        for(Size i = 0; i < slots.size(); i++) slots[i].exit[index] = current[i];
    }

    /*
     * The operand each placed argument is handed, once every block has said what it leaves.
     *
     * Block by block and in declaration order, because the operands are *appended*: an edge into a
     * block that took two arguments owes them in the order the block declares them. One append per
     * **edge** rather than per predecessor - `branch %c, X, X` is two edges into one block and each
     * owes its own operand - and every edge out of one predecessor is handed the same value, since
     * what a slot holds on the way out is a property of the block rather than of the arm.
     */
    void fill(Editor& editor) {
        for(U32 index = 0; index < blockCount; index++) {
            auto block = opt.body->block(opt.base, BlockId(index));
            if(block->deleted) continue;

            SmallArray<U32, 8> here;
            for(auto& entry: placed) {
                if(entry.block == index) here.push(entry.slot);
            }

            if(!here.size()) continue;

            for(Size i = 0; i < block->predecessorCount(); i++) {
                auto from = opt.base[block->predecessorAt(opt.base, i)];

                auto seen = false;
                for(Size k = 0; k < i; k++) {
                    if(opt.base[block->predecessorAt(opt.base, k)] == from) seen = true;
                }

                if(seen) continue;

                auto end = from->terminator(opt.base);
                assertTrue(end != nullptr);

                Size arm = 0;
                SmallArray<Size, 4> arms;
                forEachEdge(*end, [&](Edge& edge) {
                    if(edge.target && opt.base[edge.target] == block) arms.push(arm);
                    arm++;
                });

                for(auto at: arms) {
                    for(auto which: here) {
                        auto value = slots[which].exit[from->index.index];
                        assertTrue(value != nullptr);

                        editor.addEdgeArgument(*end, at, editor.use(*value, UseKind::Inspect));
                    }
                }
            }
        }
    }

    void run() {
        blockCount = U32(opt.body->blocks.size());

        /*
         * A body with a block control cannot reach is declined whole - see the file comment. The
         * order holds the reachable blocks and nothing else, so this is one comparison against the
         * blocks that are still in the table.
         */
        auto live = Size(0);
        for(auto pointer: opt.body->blocks.contents(opt.base)) {
            if(!opt.base[pointer]->deleted) live++;
        }

        if(live != dominators.order.size()) return;

        collect();
        if(slots.isEmpty()) return;

        IndexSet usable;
        survey(usable);
        if(!keep(usable)) return;

        current.clear();
        for(Size i = 0; i < slots.size(); i++) current.push(nullptr);

        auto editor = opt.ir();
        for(auto index: dominators.order) enter(editor, *opt.body->block(opt.base, BlockId(index)));

        // Nothing may still be reading storage that is about to go. Asserted here rather than left
        // to `eraseInst`, because this is where the answer names which allocation was missed.
        for(auto& slot: slots) assertTrue(slot.allocation->useCount() == 0);

        fill(editor);

        /*
         * And the arguments that say nothing, taken straight back out - `opt/cfg.cpp`, where the
         * rule and its three refusals are written down.
         *
         * This pass carried its own copy of that until phase 14, over the arguments *it* had placed
         * and deliberately nothing else. What the general form adds here is the ones the conversion
         * placed, which is what a `for`'s bound and step are: this is the first point in the round
         * at which anything looks at them, and every pass below reads a header without them.
         *
         * It is called with the allocations still standing, exactly where the narrow version was.
         * The `assertTrue` above is what makes that safe to say twice: nothing reads a slot any
         * more, so no substitution below can be one this pass has not already accounted for.
         */
        removeTrivialArguments(opt);

        for(auto& slot: slots) editor.eraseInst(*slot.alloc);
    }
};

} // namespace

void promote(OptContext& opt) {
    /*
     * The dominator tree is read once and held across every rewrite below, which is the discipline
     * `analysis.h` states - and here it is more than a convention: the reverse postorder is the
     * order the construction walks in, and re-asking after a block argument was added would answer
     * about a body this pass is half way through editing. Nothing it edits changes the graph.
     */
    Promoter promoter { opt, containedOf(opt), dominatorsOf(opt) };
    promoter.run();
}

} // namespace ssa
