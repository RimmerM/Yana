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
 * **Every load's result and every store's value are `Trivial` and agree on their type and their
 * root's contract.** The convention half is the linear rule: what replaces the loads is one value
 * per definition, and duplicating an owner or a borrow is not a substitution -
 * `Editor::replaceAllUses` refuses both. The occurrence half is `Forwarder::isForwardable`'s rule and
 * has the same reason: a value's occurrence is what `lower/manifest.cpp` manifests it from, and a
 * block argument declares exactly one. Whether two occurrences happen to manifest alike is the
 * **target's** answer and `optimizeProgram` runs once per program, which is the refusal `fold.cpp`
 * makes about a width and `memory.cpp` about a sum's fields.
 *
 * The interior **shape** is the one part of the occurrence excused, and `agree` is where the
 * argument for that is.
 *
 * That is what keeps a **borrowed** parameter's slot in storage. `fn f(n: Int)` stores an
 * `I32 [shared call] guaranteed` into a slot its loads read back as a plain `I32 trivial`, so the
 * pair is a convention change and an occurrence change at once - phase 6 declined the same
 * substitution for the same reason and its note says so. A `->` parameter of a trivial type is an
 * ordinary owned occurrence at both ends and promotes with every other local, and so does every
 * parameter slot of an **inlined** body, where what the store installs is the caller's own value.
 *
 * A **borrow of the allocation** is a second *name* for it rather than a use of what is in it, and
 * since J37 the walk follows one. There is no `Borrow` instruction at this tier, so `&x` is a `load`
 * whose result is `Guaranteed` and whose loan names the allocation, and `represent` restates such a
 * name at another occurrence without moving a byte - which is exactly the pair `allocationRoot`
 * already walks and `Containment` already recurses into. What the walk here adds is that the *whole*
 * of what those names are used for must again be loads, stores and drops of the whole of it: then
 * the borrow, the restatements and the storage all go together, and the loan that named the
 * allocation goes with the instruction that made it rather than being left without a lender.
 *
 * **A name handed over an edge is one of these too, and since J50 the walk follows that** - the
 * block argument at the other end is the same storage under a third name, and it is what a borrow
 * that crosses a join is spelled as here. It cost the whole allocation rather than the edge:
 * `pick` in `test/resolve/LiveAfterLoop.yana` is inlined into a join, so `main`'s counter was
 * borrowed into a block argument, `Contained` read that as an escape, and the loop below it then
 * ran out of memory - `mov (%rax),%rdx; inc %rdx; mov %rdx,(%rax)` where the shipping stage has
 * `inc %rax`. `opt/analysis.cpp`'s arm is the containment half; `argumentsAgree` is this one.
 *
 * **This is the shape a grafted continuation leaves and it is most of what this pass is worth.** An
 * `iter fn`'s `for` keeps its outcome signal, its accumulator and its result in slots the
 * continuation was handed a borrow of; the inliner puts the body back but leaves the borrow, so
 * before this every one of those stayed in storage - the vector `indexOf` re-loaded and re-stored its
 * flag, its running position and its `Maybe(Size)` on every iteration, which is also what kept
 * `lower_licm` from hoisting a loop-invariant load out of a loop that "writes storage" and
 * `threadBranches` from seeing a join that holds its branch and nothing else.
 *
 * It is refused where any of those names is used for anything else - a `FieldAddress`, a call
 * argument, a `Copy` - which is where the loan really does describe storage somebody else reads.
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

    // The values that give this storage a second name - a `Guaranteed` load, the `represent`s over
    // it, and the block arguments those are handed to. In no order: `run` takes each of them away
    // at the point nothing reads it any more. Empty for the ordinary slot, which is most of them.
    Array<Value*> names;

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

    // Every *name* for one slot's storage answers as that slot, which is what makes the accesses
    // through a borrow of it the same accesses - see `scan`.
    void name(Value& value, U32 slot) {
        if(value.id.index < slotOf.size()) slotOf[value.id.index] = slot;
    }

    // The slot an instruction reads and the slot it writes, asked separately because a `Drop` is
    // neither and every other kind is one or the other or nothing.
    U32 readSlot(Inst& inst) {
        if(inst.kind != Inst::Load) return kNoSlot;
        if(((InstLoad&)inst).isVolatile) return kNoSlot;

        // A borrow of the slot is a second *name* for it and not a read of what is in it - `scan`
        // followed it and `run` erases it with the storage, so neither the survey nor the rewrite
        // may treat it as an access. The `represent`s over it are not `Load`s and never arrive here.
        if(inst.results.size() == 1
           && opt.base[inst.results.get(opt.base, 0)]->convention == Convention::Guaranteed) {
            return kNoSlot;
        }

        return slotAt(addressOf(((InstLoad&)inst).address));
    }

    U32 writtenSlot(Inst& inst) {
        if(inst.kind != Inst::Store) return kNoSlot;
        return slotAt(addressOf(((InstStore&)inst).address));
    }

    /*
     * What one candidate has been found out about while its names are walked.
     *
     * A structure rather than four reference parameters, because the walk is recursive: a borrow of
     * the allocation is a second name whose uses have to answer the same questions, and threading
     * four out-parameters through that reads as four independent facts when it is one verdict.
     */
    struct Survey {
        Occurrence agreed;
        bool typed = false;
        bool read = false;

        // Every name for this storage, the allocation itself first. Both the worklist the scan runs
        // over while it grows and, minus that first entry, what the slot keeps.
        Array<Value*> names;
    };

    /*
     * The type and the root's contract, and **not** the interior shape.
     *
     * The shape says which positions inside the value somebody else owns, which is an ownership
     * statement and not a manifestation - so excusing it here would be exactly the "they happen to
     * manifest alike" shortcut the file comment refuses, were it not for the clause above it: every
     * load and every store here is `Convention::Trivial`, and a trivially copied value owes no
     * teardown at either end. A shape carries no obligation across a pair like that; the loans it
     * described were decided in `resolve_mir` and nothing at this tier reads it.
     *
     * The case is a **grafted parameter slot whose argument knows more than the callee asked for**:
     * `bitcast(addressOf(global))` is a `%CodeUnit` whose pointee names the global's loan,
     * `stringLiteral(bytes: %CodeUnit)` declared no origin, so the store installs a shape the load
     * reads back without. Leaving the slot in storage cost two instructions per string literal -
     * enough to move an inlining decision in `resolve/NicheOwned.yana`, which is how it was noticed.
     *
     * What the promoted slot then declares is the shape they agreed on, or none where they did not:
     * an empty shape is "nothing inside is anyone else's", which is what a slot whose ends disagree
     * can honestly say and what its own declaration already said.
     */
    bool agree(Survey& survey, Value& value) {
        if(value.convention != Convention::Trivial) return false;

        if(!survey.typed) {
            survey.agreed = value.occurrence;
            survey.typed = true;
            return true;
        }

        if(survey.agreed.type != value.occurrence.type) return false;
        if(survey.agreed.contract != value.occurrence.contract) return false;

        if(survey.agreed.shape != value.occurrence.shape) survey.agreed.shape = nullptr;
        return true;
    }

    /*
     * How many second names one allocation may have.
     *
     * A work cap and not what makes the walk terminate - `survey.names` is a set, so every name is
     * queued once and the walk ends whether or not this is ever reached. What it bounds is a body
     * where a borrow is restated into a great many places, which would otherwise be a use-list scan
     * per name for a slot the rewrite is unlikely to want.
     */
    static constexpr Size kMaxNames = 32;

    static bool isName(Survey& survey, Value* value) {
        for(auto name: survey.names) {
            if(name == value) return true;
        }

        return false;
    }

    // A second name found below another, queued once. A linear scan rather than a bit set: an
    // allocation has two or three names and the cap above is what the worst case is measured
    // against.
    static bool note(Survey& survey, Value& value) {
        if(isName(survey, &value)) return true;
        if(survey.names.size() >= kMaxNames) return false;

        survey.names.push(&value);
        return true;
    }

    /*
     * Whether the whole use list of one **name** for this storage is loads, stores and teardown-free
     * drops of the whole of it - the eligibility rule, one name at a time. Every second name it
     * finds is queued rather than descended into, and `scan` is the walk over that queue. The file
     * comment is the argument for each clause.
     */
    bool scanUses(Survey& survey, Value& named) {
        for(auto userPointer: named.uses(opt.base)) {
            auto& user = *opt.base[userPointer];
            if(isVolatileMemory(user)) return false;
            auto names = [&](Use used) { return addressOf(used) == &named; };

            switch(user.kind) {
                case Inst::Load: {
                    if(!names(((InstLoad&)user).address)) return false;
                    if(user.results.size() != 1) return false;

                    auto result = opt.base[user.results.get(opt.base, 0)];

                    /*
                     * A `Guaranteed` result is a **borrow**: a second named for this storage rather
                     * than a read of what is in it, which is how `&x` is spelled at this tier. It is
                     * followed rather than declined, and what follows it is the same question.
                     */
                    if(result->convention == Convention::Guaranteed) {
                        if(!note(survey, *result)) return false;
                        break;
                    }

                    if(!agree(survey, *result)) return false;

                    survey.read = true;
                    break;
                }

                case Inst::Store: {
                    auto& store = (InstStore&)user;

                    // Through it and not *of* it: a store of the allocation's own address is the
                    // address escaping into storage, which `Contained` has already refused and which
                    // this states again because the two operands are one use list entry each.
                    if(!names(store.address) || names(store.value)) return false;
                    if(!agree(survey, *opt.base[store.value.value()])) return false;
                    break;
                }

                /*
                 * `represent` - the same storage restated at another occurrence, which moves no byte
                 * and is what the inliner leaves where a continuation's handover crossed a
                 * signature. A second named exactly as the borrow above is.
                 */
                case Inst::Unary: {
                    auto& unary = (InstUnary&)user;
                    if(unary.op != mir::UnaryOp::Represent || !names(unary.value)) return false;
                    if(user.results.size() != 1) return false;

                    if(!note(survey, *opt.base[user.results.get(opt.base, 0)])) return false;
                    break;
                }

                /*
                 * A name **handed over an edge** - J50, and the counterpart of `Containment`'s own
                 * arm for it. The block argument at the other end is a second name for this storage
                 * exactly as a `represent` is, and it is what a borrow that crosses a join or a loop
                 * is spelled as here.
                 *
                 * Two conditions, and `argumentsAgree` below is where the second one is checked.
                 * The argument has to be a **borrow**, because a name for storage is what this
                 * follows and an operand of any other convention fills an argument that is a value;
                 * and every edge into that block has to fill it with a name of *this* storage, or
                 * the argument is this slot on some paths and something else on the rest.
                 *
                 * Every position is asked on every edge, because a terminator may hand the same
                 * value to two arms and `branch %c, X, X` is two edges into one block.
                 */
                case Inst::Jump:
                case Inst::Branch:
                case Inst::Switch: {
                    auto held = true;

                    forEachEdge(opt.base, user, [&](Edge& edge) {
                        if(!held) return;
                        if(!edge.target) { held = false; return; }

                        auto target = opt.base[edge.target];
                        Size position = 0;

                        for(auto used: edge.arguments.contents(opt.base)) {
                            auto at = position++;
                            if(!used.isSet() || opt.base[used.value()] != &named) continue;

                            if(at >= target->argumentCount()) { held = false; return; }

                            auto argument = opt.base[target->argumentAt(opt.base, at)];
                            if(argument->convention != Convention::Guaranteed) {
                                held = false;
                                return;
                            }

                            if(!note(survey, *argument)) { held = false; return; }
                        }
                    });

                    if(!held) return false;
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

        return true;
    }

    /*
     * Every name that is a **block argument**, checked against the edges that fill it.
     *
     * Two things are asked, and each is a way the argument would not be this storage:
     *
     *  - **every edge into its block fills it with a name of this allocation.** The scan reaches an
     *    argument from one edge; the others may hand over a borrow of something else entirely, and a
     *    name that is this slot on one path is not a name for it. A block with no way in is the
     *    entry block, whose arguments are the function's parameters - refused here without a case of
     *    its own, since a parameter is filled by no edge at all.
     *  - **the value each edge hands over is an instruction**, never another block argument. That is
     *    a restriction rather than a rule about storage, and `run` is where it is spent: a name is
     *    taken away when nothing reads it, and the reader of a block argument's operand is the edge,
     *    so a pair of arguments filling each other round a loop would be two names neither of which
     *    can ever go first. What it costs is a borrow carried across a back edge *as an argument*,
     *    which the corpus does not hold - a loop that borrows re-takes the borrow each turn.
     */
    bool argumentsAgree(Survey& survey) {
        for(auto name: survey.names) {
            if(name->definer) continue;

            // A value that is neither defined by an instruction nor declared by a block is one this
            // pass has no account of, and there is no argument list to check it against.
            if(!name->block) return false;

            auto block = opt.base[name->block];
            auto index = Size(0);
            auto found = false;

            for(Size i = 0; i < block->argumentCount(); i++) {
                if(opt.base[block->argumentAt(opt.base, i)] != name) continue;

                index = i;
                found = true;
                break;
            }

            if(!found || !block->predecessorCount()) return false;

            auto filled = true;

            for(Size i = 0; i < block->predecessorCount(); i++) {
                auto end = opt.base[block->predecessorAt(opt.base, i)]->terminator(opt.base);
                if(!end) return false;

                forEachEdge(opt.base, *end, [&](Edge& edge) {
                    if(!filled || !edge.target || opt.base[edge.target] != block) return;
                    if(index >= edge.arguments.size()) { filled = false; return; }

                    auto used = edge.arguments.get(opt.base, index);
                    if(!used.isSet()) { filled = false; return; }

                    auto supplied = opt.base[used.value()];
                    if(!supplied->definer || !isName(survey, supplied)) filled = false;
                });
            }

            if(!filled) return false;
        }

        return true;
    }

    // The walk: the allocation, then every name found below one, until the list stops growing.
    bool scan(Survey& survey, Value& allocation) {
        survey.names.push(&allocation);

        for(Size at = 0; at < survey.names.size(); at++) {
            if(!scanUses(survey, *survey.names[at])) return false;
        }

        return argumentsAgree(survey);
    }

    bool eligible(Value& allocation, Survey& survey) {
        auto& alloc = (InstAlloc&)*opt.base[allocation.definer];

        // Frame storage only, and the held occurrence owing nothing - `removeDeadStorage`'s two
        // questions, asked here for the same reason: what this ends with is deleting the request for
        // the storage, which is a complete answer exactly where the frame supplied it and where
        // nothing was owed when it went away.
        if(alloc.storage != mir::StorageClass::Frame) return false;
        if(hasTeardownAt(opt, alloc.held) || isLinearAt(opt, alloc.held)) return false;
        if(!contained[allocation]) return false;

        if(!scan(survey, allocation)) return false;

        // No reads, no promotion: an allocation nothing reads is `removeDeadStorage`'s, and placing
        // arguments for a value nothing names would be work only to take it back.
        return survey.typed && survey.read;
    }

    void collect() {
        slotOf.clear();
        for(Size i = 0; i < opt.body->values.size(); i++) slotOf.push(kNoSlot);

        for(auto valuePointer: opt.body->values.contents(opt.base)) {
            auto value = opt.base[valuePointer];
            if(!value->definer) continue;

            auto definer = opt.base[value->definer];
            if(definer->kind != Inst::Alloc || !definer->block) continue;

            Survey survey;
            if(!eligible(*value, survey)) continue;

            Slot slot;
            slot.allocation = value;
            slot.alloc = definer;
            slot.occurrence = survey.agreed;
            slot.column = U32(slots.size());

            // Every name but the allocation, which is `survey.names`' first entry and is `slot.
            // allocation` already.
            for(Size i = 1; i < survey.names.size(); i++) slot.names.push(survey.names[i]);

            name(*value, U32(slots.size()));
            for(auto held: slot.names) name(*held, U32(slots.size()));

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
            name(*slots[i].allocation, U32(i));
            for(auto held: slots[i].names) name(*held, U32(i));

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
                forEachEdge(opt.base, *end, [&](Edge& edge) {
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

    // One block argument taken back out, found by identity: `removeBlockArgument` takes the operand
    // filling it off every edge in, which is what makes the sweep above terminate.
    void removeArgument(Editor& editor, Value& argument) {
        auto block = opt.base[argument.block];

        for(Size i = 0; i < block->argumentCount(); i++) {
            if(opt.base[block->argumentAt(opt.base, i)] != &argument) continue;

            editor.removeBlockArgument(*block, i);
            return;
        }

        assertTrue(false && "a name that is a block argument of no block");
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

        /*
         * The second names go first, and then nothing may still be reading storage that is about to
         * go. Each of them is taken away at the point **nothing reads it**, which is a sweep to a
         * fixpoint rather than the discovery order reversed: since J50 a name can be a block
         * argument, whose reader is the edge that fills it and whose removal is therefore what takes
         * that reader away, so the order is no longer the order they were found in.
         *
         * The loan a borrow minted goes with the instruction that made it, as before.
         */
        for(auto& slot: slots) {
            for(auto changed = true; changed;) {
                changed = false;

                for(Size i = slot.names.size(); i-- > 0;) {
                    auto held = slot.names[i];
                    if(held->useCount()) continue;

                    if(held->definer) editor.eraseInst(*opt.base[held->definer]);
                    else removeArgument(editor, *held);

                    slot.names.remove(i);
                    changed = true;
                }
            }

            // There is always one that can go next, and `argumentsAgree` is why: a block argument
            // is filled only from names that are instructions, so "is read by" over one slot's
            // names is a forest and never a cycle.
            assertTrue(slot.names.isEmpty());
        }

        // Asserted here rather than left to `eraseInst`, because this is where the answer names
        // which allocation was missed.
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
