#include "analyze.h"
#include "expr.h"
#include "name.h"

/*
 * What every pass works over, gathered once.
 *
 * The numbering is the spine: each instruction of the function gets one index - blocks in the order
 * they were built, phis before instructions before the terminator - and liveness, ownership state
 * and drop points are all stated in those indices. That is the same arrangement
 * lower/lower_analyze.cpp uses at the lower level, deliberately, so the two dumps read against each
 * other.
 *
 * Sets are a byte per local rather than a bit. A function has a handful of locals, the sets are
 * copied constantly by the fixpoint, and a byte array copies correctly by value while a packed one
 * would need care every time - which is the wrong thing to spend care on here.
 */

namespace {

using LocalSet = Array<U8>;

struct BlockRange {
    U32 first = 0;
    U32 end = 0;
};

// What one instruction does to the locals it touches. `defs` and `uses` drive liveness, `moves`
// drives the ownership lattice, and `assigns` marks the writes that overwrite a live value - the
// one place a drop happens before an instruction rather than after one.
struct Effects {
    // Writes that replace the whole slot, so nothing above them can still be reaching its old
    // contents. These end a live range going backwards.
    Array<U32> defs;

    // Writes that make a slot owned without replacing all of it - one field of an aggregate being
    // constructed. They are `uses` for liveness, because the rest of the slot survives them, and
    // this list only records the ownership half.
    Array<U32> inits;

    Array<U32> uses;
    Array<U32> moves;
    bool assigns = false;
};

// One drop the pass decided to insert. `before` is a linear index: the drop goes immediately
// before that instruction, which is always a real position because a terminator never defines or
// last-uses a local itself.
struct PendingDrop {
    U32 local = 0;
    U32 before = 0;
};

// One drop that belongs on a CFG edge rather than inside a block - the branch case, where a value
// is live down one arm and dead down the other.
struct EdgeDrop {
    U32 local = 0;
    Size fromBlock = 0;
    Size toBlock = 0;
};

// One borrow, with the extent over which it holds. Exclusivity is a question about two of these
// overlapping in both extent and place.
struct LiveBorrow {
    ModulePtr<Inst> instruction;
    U32 from = 0;
    U32 to = 0;
    bool mut = false;
};

struct Analysis {
    Analysis(Module& module, Function& function):
        module(module), context(module.context), global(*module.types), local(*module.arena),
        function(function) {}

    Module& module;
    Context& context;
    GlobalBase global;
    ModuleBase local;
    Function& function;

    Size localCount = 0;
    Size instructionCount = 0;

    Array<ModulePtr<Inst>> order;
    Array<BlockRange> blockRanges;
    Array<Effects> effects;
    Array<TrackedLocal> tracked;

    // Where each instruction sits in the numbering, so that a value's use list can be turned into
    // an extent without rescanning.
    HashMap<U32, U32> indexOf;

    Array<LocalSet> liveIn;
    Array<LocalSet> liveOut;

    // Ownership state before each instruction, one row per instruction index.
    Array<Array<OwnState>> stateBefore;

    bool ok = true;

    Block* blockAt(Size index) { return local[function.blocks.get(local, index)]; }
    Size blockCount() { return function.blocks.size(); }
};

// Defined further down, next to the overlap test it exists for.
static bool touchedPlace(Value& instruction, Place& target);

static LocalSet emptySet(Size count) {
    LocalSet set;
    for(Size i = 0; i < count; i++) set.push(0);
    return set;
}

/*
 * Numbering and effects.
 */

static void numberFunction(Analysis& analysis) {
    for(Size i = 0; i < analysis.blockCount(); i++) {
        auto block = analysis.blockAt(i);
        BlockRange range;
        range.first = U32(analysis.order.size());

        for(auto phi: block->phis.contents(analysis.local)) analysis.order.push((ModulePtr<Inst>)phi);
        for(auto instruction: block->instructions.contents(analysis.local)) analysis.order.push(instruction);
        if(block->terminator) analysis.order.push(block->terminator);

        range.end = U32(analysis.order.size());
        analysis.blockRanges.push(range);
    }

    analysis.instructionCount = analysis.order.size();
    for(Size i = 0; i < analysis.instructionCount; i++) {
        *analysis.indexOf.add(U32(analysis.order[i])).value = U32(i);
    }
}

// The local a place is rooted in, or none. A global outlives every function and a raw pointer's
// target is outside the ownership model by definition, so neither contributes.
static U32 rootLocal(Analysis& analysis, const Place& place) {
    if(place.root != PlaceRoot::Local) return maxLimit<U32>;
    return place.local < analysis.localCount ? place.local : maxLimit<U32>;
}

static void useRoot(Analysis& analysis, Effects& effects, const Place& place) {
    auto root = rootLocal(analysis, place);
    if(root != maxLimit<U32>) effects.uses.push(root);
}

// The local a value is the contents of, or none. An aggregate that lives in storage is named by
// the value that produced it - a call result, a copy, an allocation - and Function::locals records
// that pairing, which is what lets an SSA operand be recognized as an owned slot.
static U32 backingLocal(Analysis& analysis, ModulePtr<Value> value) {
    if(!value) return maxLimit<U32>;

    for(U32 i = 0; i < analysis.localCount; i++) {
        if(analysis.function.localAt(analysis.local, i).value == value) return i;
    }

    return maxLimit<U32>;
}

// Reading a value that is the contents of a slot is a use of that slot. Aggregates travel through
// the IR as the value that produced them rather than as a load, so without this an owned record
// passed to a call would look dead at the point it was created.
static void useValue(Analysis& analysis, Effects& effects, ModulePtr<Value> value) {
    auto root = backingLocal(analysis, value);
    if(root != maxLimit<U32>) effects.uses.push(root);
}

/*
 * Ownership leaving this frame through a value.
 *
 * Writing an owned aggregate into another place, returning it, or merging it into a phi all hand
 * its contents to something else. The slot it came out of must not be dropped afterwards, or the
 * same storage is released twice - which for `Pair {left: makeBuffer(32), ...}` would be a double
 * free of the buffer the field now owns.
 *
 * Only droppable types transfer. For everything else the write is a copy of bytes nobody is
 * responsible for, and saying it moved would make the source unusable for no reason.
 */
static void transferFrom(Analysis& analysis, Effects& effects, ModulePtr<Value> value) {
    auto root = backingLocal(analysis, value);
    if(root == maxLimit<U32>) return;

    effects.uses.push(root);

    auto type = analysis.function.localAt(analysis.local, root).type;
    if(ownershipOf(analysis.module, type).drop != DropKind::None) effects.moves.push(root);
}

static void computeEffects(Analysis& analysis) {
    auto local = analysis.local;

    for(auto pointer: analysis.order) {
        auto& instruction = *local[pointer];
        Effects effects;

        // A value that owns storage of its own defines the slot recording it. That covers the
        // aggregate results - a call's, a copy's - which are created already filled rather than
        // allocated and then written into.
        auto produced = backingLocal(analysis, (ModulePtr<Value>)pointer);
        if(produced != maxLimit<U32> && instruction.kind != Value::Arg) {
            // An allocation ends the slot's live range going backwards - nothing above it can be
            // reaching contents that did not exist - without making it owned, since it puts
            // nothing in the storage it creates. That is exactly the split between the two lists.
            effects.defs.push(produced);
            if(instruction.kind != Value::Alloc) effects.inits.push(produced);
        }

        switch(instruction.kind) {
            case Value::Alloc:
                // Allocating creates storage and puts nothing in it. What makes a slot owned is
                // the Init that follows, which is why `let x = e` is two instructions.
                break;

            case Value::LoadPlace:
                useRoot(analysis, effects, ((InstLoadPlace&)instruction).place);
                break;

            case Value::Init:
            case Value::Assign: {
                auto& write = (InstInit&)instruction;
                auto root = rootLocal(analysis, write.place);

                if(root != maxLimit<U32>) {
                    auto whole = write.place.projections.isEmpty();

                    if(whole) {
                        effects.defs.push(root);
                        effects.assigns = instruction.kind == Value::Assign;
                    } else {
                        // A field write leaves the rest of the slot alone, so it reads as a use.
                        effects.uses.push(root);
                    }

                    // Filling a field is still what makes a constructed aggregate owned: there is
                    // no single instruction that initializes one as a whole.
                    if(whole || instruction.kind == Value::Init) effects.inits.push(root);
                }

                transferFrom(analysis, effects, write.value);
                break;
            }

            case Value::Borrow:
                useRoot(analysis, effects, ((InstBorrow&)instruction).place);
                break;

            case Value::Move: {
                auto& moved = (InstMove&)instruction;
                useRoot(analysis, effects, moved.place);

                auto root = rootLocal(analysis, moved.place);
                if(root != maxLimit<U32>) effects.moves.push(root);
                break;
            }

            case Value::Copy:
                useRoot(analysis, effects, ((InstCopy&)instruction).place);
                break;

            case Value::Drop:
                useRoot(analysis, effects, ((InstDrop&)instruction).place);
                break;

            case Value::Address:
                useRoot(analysis, effects, ((InstAddress&)instruction).place);
                break;

            case Value::Cast:
            case Value::Neg:
            case Value::Not:
                useValue(analysis, effects, ((InstUnary&)instruction).from);
                break;

            case Value::Add:
            case Value::Sub:
            case Value::Mul:
            case Value::Div:
            case Value::Rem:
            case Value::Shl:
            case Value::Shr:
            case Value::Sar:
            case Value::And:
            case Value::Or:
            case Value::Xor:
            case Value::Cmp:
                useValue(analysis, effects, ((InstBinary&)instruction).lhs);
                useValue(analysis, effects, ((InstBinary&)instruction).rhs);
                break;

            case Value::Native:
                for(auto arg: ((InstNative&)instruction).args.contents(local)) {
                    useValue(analysis, effects, arg);
                }

                break;

            case Value::Call:
                // A default-convention argument is a borrow, so passing one keeps the caller's
                // slot alive for the call without handing it over. A `->` argument was already
                // turned into an InstMove before it got here, and that is where the handover is.
                for(auto arg: ((InstCall&)instruction).args.contents(local)) {
                    useValue(analysis, effects, arg);
                }

                break;

            case Value::GenCall:
                for(auto arg: ((InstGenCall&)instruction).args.contents(local)) {
                    useValue(analysis, effects, arg);
                }

                break;

            case Value::Je:
                useValue(analysis, effects, ((InstJe&)instruction).cond);
                break;

            case Value::Ret:
                // Returning hands ownership to the caller. Without this the value would be dropped
                // on the way out and the caller handed released storage.
                transferFrom(analysis, effects, ((InstRet&)instruction).value);
                break;

            default:
                break;
        }

        analysis.effects.push(::move(effects));
    }
}

/*
 * A value that refers into a slot keeps that slot alive for as long as the value is.
 *
 * An aggregate is never loaded into a register: `load %pair.left` produces the *address* of the
 * field, which is to say a borrow of it. So the slot is used wherever that value is used, not only
 * where the load was written - without this, `firstByte(pair.left)` would drop the pair between
 * taking the address of its field and reading through it.
 *
 * One level deep, which is what the resolver produces: placeFor() recovers a place from a load
 * rather than loading again, so chains of these do not arise. The address an `addressOf` hands out
 * is a different matter - a raw pointer can be stored anywhere and outlive any extent this could
 * compute - and is unchecked by construction, which is what `%T` means.
 */
static void extendBorrowUses(Analysis& analysis) {
    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto pointer = analysis.order[i];
        auto& instruction = *analysis.local[pointer];

        Place place;
        auto borrows = instruction.kind == Value::Borrow || instruction.kind == Value::Address ||
                       (instruction.kind == Value::LoadPlace &&
                        isMemoryType(analysis.global, instruction.type));

        if(!borrows || !touchedPlace(instruction, place)) continue;

        auto root = rootLocal(analysis, place);
        if(root == maxLimit<U32>) continue;

        for(auto user: instruction.uses.contents(analysis.local)) {
            auto index = analysis.indexOf.get(U32(user));
            if(index) analysis.effects[index.unwrap()].uses.push(root);
        }
    }
}

/*
 * A phi's operands are used on the edges into it, not at the phi.
 *
 * Attributing them to the join block instead is the classic way to get a false use-after-move: at
 * the join, every arm's slot has been merged with the arms that never wrote it, so all of them read
 * as "owned on some paths". Attributing each operand to the end of the predecessor it comes from is
 * both what actually happens and what makes the state at the join say nothing at all about slots
 * that belong to one arm.
 */
static void attributePhiEdges(Analysis& analysis) {
    for(Size b = 0; b < analysis.blockCount(); b++) {
        auto block = analysis.blockAt(b);

        for(auto phiPointer: block->phis.contents(analysis.local)) {
            auto& phi = *analysis.local[phiPointer];

            for(auto input: phi.inputs.contents(analysis.local)) {
                auto from = analysis.local[input.block];
                if(!from->terminator) continue;

                auto index = analysis.indexOf.get(U32(from->terminator));
                if(!index) continue;

                transferFrom(analysis, analysis.effects[index.unwrap()], input.value);
            }
        }
    }
}

/*
 * Liveness - the ordinary backward fixpoint.
 *
 * A local is live at a point when some path from there reaches a use of it before the next write
 * of the whole slot. Blocks are walked in reverse per round, which settles in one round for
 * straight-line code and in as many rounds as the loop nest is deep for anything else.
 */

static void applyBackward(Analysis& analysis, Size first, Size end, LocalSet& live) {
    for(Size i = end; i > first; i--) {
        auto& effects = analysis.effects[i - 1];

        // Defs before uses: an instruction that both writes and reads a slot leaves it live above.
        for(auto def: effects.defs) live[def] = 0;
        for(auto use: effects.uses) live[use] = 1;
    }
}

static void computeLiveness(Analysis& analysis) {
    auto count = analysis.localCount;
    auto blocks = analysis.blockCount();

    for(Size i = 0; i < blocks; i++) {
        analysis.liveIn.push(emptySet(count));
        analysis.liveOut.push(emptySet(count));
    }

    auto changed = true;
    while(changed) {
        changed = false;

        for(Size i = blocks; i > 0; i--) {
            auto index = i - 1;
            auto block = analysis.blockAt(index);
            auto live = emptySet(count);

            for(auto successor: block->outgoing) {
                if(!successor) continue;

                auto& successorIn = analysis.liveIn[analysis.local[successor]->index];
                for(Size l = 0; l < count; l++) live[l] |= successorIn[l];
            }

            for(Size l = 0; l < count; l++) {
                if(live[l] != analysis.liveOut[index][l]) changed = true;
            }

            analysis.liveOut[index] = live;

            auto range = analysis.blockRanges[index];
            applyBackward(analysis, range.first, range.end, live);

            for(Size l = 0; l < count; l++) {
                if(live[l] != analysis.liveIn[index][l]) changed = true;
            }

            analysis.liveIn[index] = live;
        }
    }
}

/*
 * Ownership state - the forward companion.
 *
 * Uninitialized and Moved both mean "owns nothing" and are kept apart only so that a use of one
 * reads as a different mistake from a use of the other. Maybe is the join of anything with
 * anything else, and is the state a drop flag exists to resolve at run time.
 */

static OwnState joinState(OwnState a, OwnState b) {
    if(a == b) return a;

    // Two ways of owning nothing join to the one that produces the better diagnostic.
    auto emptyA = a == OwnState::Uninitialized || a == OwnState::Moved;
    auto emptyB = b == OwnState::Uninitialized || b == OwnState::Moved;
    if(emptyA && emptyB) return OwnState::Moved;

    return OwnState::Maybe;
}

// Everything one instruction does to the ownership state, in place.
static void transferState(Analysis& analysis, Size index, Array<OwnState>& states) {
    auto& effects = analysis.effects[index];

    // Moves before inits: `x = consume(x)` moves out of the slot and then fills it again, and the
    // state that survives is the second of the two.
    for(auto moved: effects.moves) states[moved] = OwnState::Moved;
    for(auto init: effects.inits) states[init] = OwnState::Owned;

    auto& instruction = *analysis.local[analysis.order[index]];
    if(instruction.kind == Value::Drop) {
        auto root = rootLocal(analysis, ((InstDrop&)instruction).place);
        if(root != maxLimit<U32>) states[root] = OwnState::Moved;
    }
}

static void computeOwnership(Analysis& analysis) {
    auto count = analysis.localCount;
    auto blocks = analysis.blockCount();

    Array<Array<OwnState>> entry;
    Array<U8> reached;

    for(Size i = 0; i < blocks; i++) {
        Array<OwnState> states;
        for(Size l = 0; l < count; l++) states.push(OwnState::Uninitialized);
        entry.push(::move(states));
        reached.push(0);
    }

    // A parameter's slot arrives already holding the caller's value. It is not owned here - see
    // TrackedLocal::owned - but it is initialized, and saying so is what keeps a read of a
    // parameter from reading as a use of storage something moved out of.
    for(Size l = 0; l < count; l++) {
        auto slot = analysis.function.localAt(analysis.local, U32(l));
        if(slot.value && analysis.local[slot.value]->kind == Value::Arg) entry[0][l] = OwnState::Owned;
    }

    reached[0] = 1;
    Array<Size> worklist;
    worklist.push(0);

    while(worklist.size()) {
        auto index = worklist.pop().unwrap();
        auto block = analysis.blockAt(index);
        auto range = analysis.blockRanges[index];
        auto states = entry[index];

        for(Size i = range.first; i < range.end; i++) {
            analysis.stateBefore[i] = states;
            transferState(analysis, i, states);
        }

        for(auto successor: block->outgoing) {
            if(!successor) continue;

            auto successorIndex = analysis.local[successor]->index;
            auto updated = false;

            // An unreached successor takes this state outright. Joining into it instead would meet
            // every owned local with the all-Uninitialized bottom and turn the lot into Maybe,
            // which is the classic way to get a dataflow analysis that answers "it depends" to
            // every question.
            if(!reached[successorIndex]) {
                entry[successorIndex] = states;
                reached[successorIndex] = 1;
                updated = true;
            } else {
                for(Size l = 0; l < count; l++) {
                    auto joined = joinState(entry[successorIndex][l], states[l]);
                    if(joined == entry[successorIndex][l]) continue;

                    entry[successorIndex][l] = joined;
                    updated = true;
                }
            }

            if(updated) worklist.push(successorIndex);
        }
    }
}

/*
 * Do two places name storage that can overlap?
 *
 * Same root, and one projection path a prefix of the other. `x.a` and `x.b` are disjoint, `x` and
 * `x.a` are not, and that precision is what makes a `&` parameter usable at all - `moveBy` borrows
 * `p.x` and `p.y` in turn and neither is a conflict with the other.
 *
 * Two deliberate conservatisms. A step through a Deref or an Index leads somewhere this analysis
 * cannot name, so from there on the two are assumed to overlap. And places rooted in raw pointers
 * never conflict with anything, including each other: `%T` carries no aliasing information by
 * construction, and reporting on it would be inventing a rule the language says it does not have.
 */
static bool placesOverlap(ModuleBase base, Place lhs, Place rhs) {
    if(lhs.root != rhs.root) return false;
    if(lhs.root == PlaceRoot::Pointer) return false;
    if(lhs.root == PlaceRoot::Local && lhs.local != rhs.local) return false;
    if(lhs.root == PlaceRoot::Global && lhs.global != rhs.global) return false;

    auto left = lhs.projections;
    auto right = rhs.projections;
    auto leftContents = left.contents(base);
    auto rightContents = right.contents(base);

    auto leftIterator = leftContents.begin();
    auto rightIterator = rightContents.begin();

    for(Size i = 0; i < min(left.size(), right.size()); i++) {
        auto a = *leftIterator;
        auto b = *rightIterator;
        ++leftIterator;
        ++rightIterator;

        if(a.kind == ProjectionKind::Deref || a.kind == ProjectionKind::Index) return true;
        if(b.kind == ProjectionKind::Deref || b.kind == ProjectionKind::Index) return true;
        if(a.kind != b.kind || a.index != b.index) return false;
    }

    // One path ran out, so it is a prefix of the other and names storage containing it.
    return true;
}

// The place an instruction touches, for conflict reporting. Only the instructions that name
// storage have one; everything else answers no and is not a borrow conflict by construction.
static bool touchedPlace(Value& instruction, Place& target) {
    switch(instruction.kind) {
        case Value::LoadPlace: target = ((InstLoadPlace&)instruction).place; return true;
        case Value::Init:
        case Value::Assign: target = ((InstInit&)instruction).place; return true;
        case Value::Borrow: target = ((InstBorrow&)instruction).place; return true;
        case Value::Move: target = ((InstMove&)instruction).place; return true;
        case Value::Copy: target = ((InstCopy&)instruction).place; return true;
        case Value::Address: target = ((InstAddress&)instruction).place; return true;
        default: return false;
    }
}

/*
 * The borrow checker.
 *
 * Three rules, and each is one of the three questions Design.md's memory model asks. What is
 * deliberately *not* here is recorded at the end of this file.
 */

// How far a borrow's extent reaches: to the last instruction that consumes the borrow value. A
// borrow's only consumer today is a call argument, so this is usually one instruction later - but
// stating it as an extent rather than as a point is what makes it generalize when a borrow can be
// held in a binding.
static U32 lastUseOf(Analysis& analysis, ModulePtr<Inst> pointer) {
    auto found = analysis.indexOf.get(U32(pointer));
    auto last = found ? found.unwrap() : 0;

    auto uses = analysis.local[pointer]->uses;
    for(auto user: uses.contents(analysis.local)) {
        auto index = analysis.indexOf.get(U32(user));
        if(index && index.unwrap() > last) last = index.unwrap();
    }

    return last;
}

static void checkBorrows(Analysis& analysis) {
    Array<LiveBorrow> borrows;

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto pointer = analysis.order[i];
        auto& instruction = *analysis.local[pointer];
        if(instruction.kind != Value::Borrow) continue;

        borrows.push(LiveBorrow {
            pointer, U32(i), lastUseOf(analysis, pointer), ((InstBorrow&)instruction).mut,
        });
    }

    for(auto& borrow: borrows) {
        auto& borrowed = (InstBorrow&)*analysis.local[borrow.instruction];

        for(Size i = borrow.from + 1; i <= borrow.to; i++) {
            auto other = analysis.order[i];
            auto& instruction = *analysis.local[other];

            Place place;
            if(!touchedPlace(instruction, place)) continue;
            if(!placesOverlap(analysis.local, borrowed.place, place)) continue;

            // The instructions that consume the borrow reach the storage *through* it, which is
            // the whole point of handing one out rather than a conflict with it.
            auto consumed = false;
            auto uses = analysis.local[borrow.instruction]->uses;
            for(auto user: uses.contents(analysis.local)) {
                if(user == other) consumed = true;
            }

            if(consumed) continue;

            auto otherBorrow = instruction.kind == Value::Borrow;
            auto otherMutable = otherBorrow && ((InstBorrow&)instruction).mut;

            // Two immutable borrows of one place are exactly what borrows are for.
            if(!borrow.mut && otherBorrow && !otherMutable) continue;

            // Reading through a live immutable borrow is fine; it is the mutable one that is
            // exclusive. A write is a conflict with either.
            auto writes = instruction.kind == Value::Assign || instruction.kind == Value::Init ||
                          instruction.kind == Value::Move || otherMutable ||
                          instruction.kind == Value::Address;

            if(!borrow.mut && !writes) continue;

            analysis.context.diagnostics.error(
                borrow.mut
                    ? "this use conflicts with a mutable borrow of the same storage, which is exclusive while it is live"_v
                    : "this write conflicts with an immutable borrow of the same storage that is still live"_v,
                instruction.source);

            analysis.context.diagnostics.message(Diagnostics::MessageLevel,
                                                 "the borrow it conflicts with is here"_v, borrowed.source);
            analysis.ok = false;
        }
    }
}

/*
 * Use after move, and the moves that cannot be represented at all.
 */
static void checkMoves(Analysis& analysis) {
    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];
        auto& states = analysis.stateBefore[i];
        auto& effects = analysis.effects[i];

        if(instruction.kind == Value::Move) {
            auto& moved = (InstMove&)instruction;

            // A partial move would leave the slot half-owned, and every later drop of it would
            // have to know which half. That is a drop flag per field and a drop that runs over a
            // subset of members - real work, deferred deliberately rather than approximated.
            if(moved.place.projections.isNotEmpty()) {
                analysis.context.diagnostics.error("cannot move a part of a value out of it - move the whole value instead"_v,
                                                   instruction.source);
                analysis.ok = false;
                continue;
            }

            auto root = rootLocal(analysis, moved.place);
            if(root != maxLimit<U32> && !analysis.tracked[root].owned) {
                analysis.context.diagnostics.error("cannot take ownership of borrowed storage - a `&` binding never owns what it refers to"_v,
                                                   instruction.source);
                analysis.ok = false;
                continue;
            }
        }

        for(auto use: effects.uses) {
            if(states[use] == OwnState::Owned || states[use] == OwnState::Uninitialized) continue;

            auto name = analysis.tracked[use].name;
            auto moved = states[use] == OwnState::Moved;

            if(name) {
                analysis.context.diagnostics.error(
                    moved ? "%@ has been moved out of and cannot be used again"_v
                          : "%@ may have been moved out of on some paths reaching here"_v,
                    instruction.source, analysis.context.findName(name));
            } else {
                analysis.context.diagnostics.error(
                    moved ? "this value has been moved out of and cannot be used again"_v
                          : "this value may have been moved out of on some paths reaching here"_v,
                    instruction.source);
            }

            analysis.ok = false;
        }
    }
}

/*
 * Derived drop glue.
 *
 * "Drop each member, then release this type's own storage" written out as a function, so that it
 * can be printed, called recursively and lowered like anything else. It takes a raw pointer to what
 * it is dropping, which is what an InstDrop hands it.
 *
 * Interned per type on the Program: a record with two fields of one type generates one of these,
 * and a type reachable from itself terminates because the entry is added before the body is built.
 */
static ModulePtr<Function> dropGlueFor(Module& module, TypePtr type, LocationId source);

// The name a glue function is printed and linked under. It is not addressable in source; what it
// needs is to be unique and to say what it drops.
static StringId dropGlueName(Module& module, TypePtr type) {
    StringBuilder text;
    text << "drop$";
    describeType(module.context, *module.types, type, text);
    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

// The implementation one member's drop runs, or null when that member has nothing to release.
static ModulePtr<Function> dropImplementation(Module& module, TypePtr type, LocationId source) {
    auto ownership = ownershipOf(module, type);

    if(ownership.drop == DropKind::Authored) {
        TypePtr args[] = { type };
        auto match = matchInstance(module, module.coreClasses.drop, toBuffer(args));
        if(!match) return nullptr;

        auto instance = (*module.arena)[match.instance];
        if(instance->functions.isEmpty()) return nullptr;

        auto implementation = instance->functions.get(*module.arena, 0);
        if(implementation) (*module.arena)[implementation]->used = true;
        return implementation;
    }

    if(ownership.drop == DropKind::Derived) return dropGlueFor(module, type, source);
    return nullptr;
}

// Emits one InstDrop for each member of `content` that has something to release, projected off
// `base`. Shared by the tuple case and by a record constructor's payload.
static void dropMembers(ExprResolver& resolver, Module& module, Place base, TypePtr content,
                        LocationId source) {
    auto global = *module.types;
    if(!content || global[content]->kind != Type::Tup) return;

    auto tuple = (TupType*)global[content];
    U16 index = 0;

    for(auto field: tuple->fields.contents(global)) {
        auto implementation = dropImplementation(module, field.type, source);
        auto kind = ownershipOf(module, field.type).drop;

        if(implementation) {
            auto place = resolver.project(base, ProjectionKind::Field, index);
            auto drop = resolver.emit<InstDrop>(source, 0, module.scalar.unit, place, kind);
            drop->implementation = implementation;
        }

        index++;
    }
}

/*
 * Built in the module that asked for it, not in Core.
 *
 * The glue has to resolve `instance Drop(Buffer)` for each of its members, and instance lookup is
 * relative to the module doing the looking - so building it in Core would find nothing an ordinary
 * program declared and silently produce empty glue. Interning is still program-wide, which relies
 * on instance coherence: two modules that can both see a type agree on what dropping it means, and
 * the language already requires that.
 */
static ModulePtr<Function> dropGlueFor(Module& module, TypePtr type, LocationId source) {
    auto& program = module.program;
    if(auto found = program.dropGlue.get(U32(type))) return found.unwrap();

    // addAnonymousFunction already registers it in the module's function order, which is what puts
    // it in front of printing and lowering.
    auto function = addAnonymousFunction(module, dropGlueName(module, type), source);
    auto pointer = function - *module.arena;

    // Registered before the body is built, so a type reachable from itself finds the entry rather
    // than generating glue forever.
    *program.dropGlue.add(U32(type)).value = pointer;

    function->returnType = module.scalar.unit;
    function->used = true;

    auto valueName = module.context.addQualifiedName("value", 5, 1);
    auto arg = function->addArg(module, valueName, resolvePointerType(module, type), source);

    ExprResolver resolver(module.context, module, *function);
    auto base = Place::atPointer((ModulePtr<Value>)(arg - *module.arena));
    auto global = *module.types;

    if(global[type]->kind == Type::Tup) {
        dropMembers(resolver, module, base, type, source);
    } else if(global[type]->kind == Type::Record) {
        auto record = (RecordType*)global[type];

        if(record->layout == RecordType::Single) {
            auto content = record->constructors.get(global, 0).content;
            dropMembers(resolver, module, resolver.project(base, ProjectionKind::Downcast, 0), content, source);
        } else if(record->layout == RecordType::Multi) {
            // Each constructor carries a different payload, so the glue has to read the
            // discriminant and drop the members of whichever one is there. That is a switch this
            // pass does not build yet - see the restrictions at the end of this file.
            module.context.diagnostics.error("a derived drop for a multi-constructor record is not generated yet - write an `instance Drop` for %@"_v,
                                             source, describeType(module.context, global, type));
        }
    }

    resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, nullptr));
    return pointer;
}

/*
 * Drop placement.
 */

// Where in a block's instruction list a linear index sits. A drop that would land on the
// terminator goes at the end of the list instead, which is the same position - the terminator is
// held apart from the list rather than in it.
static Size positionInBlock(Analysis& analysis, Size blockIndex, U32 index) {
    auto block = analysis.blockAt(blockIndex);
    auto range = analysis.blockRanges[blockIndex];
    auto phis = block->phis.size();

    if(index <= range.first + phis) return 0;

    auto position = Size(index - range.first - phis);
    return min(position, block->instructions.size());
}

static void placeDrops(Analysis& analysis, Array<Array<PendingDrop>>& blockDrops, Array<EdgeDrop>& edgeDrops) {
    auto count = analysis.localCount;

    for(Size b = 0; b < analysis.blockCount(); b++) {
        auto block = analysis.blockAt(b);
        auto range = analysis.blockRanges[b];
        if(range.end == range.first) continue;

        // Liveness at each point inside the block, derived by replaying the backward walk. `after`
        // is the state over the gap following each instruction, which is where a drop goes.
        Array<LocalSet> after;
        auto live = analysis.liveOut[b];

        for(Size i = range.end; i > range.first; i--) {
            after.push(live);

            auto& effects = analysis.effects[i - 1];
            for(auto def: effects.defs) live[def] = 0;
            for(auto use: effects.uses) live[use] = 1;
        }

        for(Size l = 0; l < count; l++) {
            if(!analysis.tracked[l].owned || !analysis.tracked[l].droppable) continue;

            auto liveBefore = analysis.liveIn[b][l];

            for(Size i = range.first; i < range.end; i++) {
                auto& effects = analysis.effects[i];
                auto liveAfter = after[range.end - 1 - i][l];

                auto defines = false;
                for(auto init: effects.inits) defines = defines || init == l;

                auto moves = false;
                for(auto move: effects.moves) moves = moves || move == l;

                auto before = analysis.stateBefore[i][l];

                // Overwriting a live value releases the old one first. That is the entire reason
                // Init and Assign are two instructions rather than one.
                if(defines && effects.assigns && before != OwnState::Uninitialized) {
                    if(before == OwnState::Maybe) {
                        analysis.context.diagnostics.error("this assignment overwrites a value that was moved out of on only some paths - conditional drops need drop flags, which are not implemented yet"_v,
                                                           analysis.local[analysis.order[i]]->source);
                        analysis.ok = false;
                    } else if(before == OwnState::Owned) {
                        blockDrops[b].push(PendingDrop { U32(l), U32(i) });
                    }
                }

                auto ownedAfter = defines || (before == OwnState::Owned && !moves);
                auto maybeAfter = !defines && !moves && before == OwnState::Maybe;

                if((liveBefore || defines) && !liveAfter && (ownedAfter || maybeAfter)) {
                    if(maybeAfter) {
                        analysis.context.diagnostics.error("this value was moved out of on only some paths reaching its last use - conditional drops need drop flags, which are not implemented yet"_v,
                                                           analysis.local[analysis.order[i]]->source);
                        analysis.ok = false;
                    } else {
                        blockDrops[b].push(PendingDrop { U32(l), U32(i + 1) });
                    }
                }

                liveBefore = liveAfter;
            }
        }

        // The branch case: live down one arm and dead down the other. liveOut is the union over
        // successors, so this can only arise where a block has more than one - which is why there
        // is no corresponding "drop at the end of the block" case.
        for(auto successor: block->outgoing) {
            if(!successor) continue;

            auto successorIndex = analysis.local[successor]->index;

            for(Size l = 0; l < count; l++) {
                if(!analysis.tracked[l].owned || !analysis.tracked[l].droppable) continue;
                if(!analysis.liveOut[b][l] || analysis.liveIn[successorIndex][l]) continue;

                auto state = range.end > range.first
                    ? analysis.stateBefore[range.end - 1][l] : OwnState::Uninitialized;

                // The terminator itself never changes ownership, so the state before it is the
                // state on the edge.
                if(state == OwnState::Maybe) {
                    analysis.context.diagnostics.error("this value is owned on only some paths reaching this branch - conditional drops need drop flags, which are not implemented yet"_v,
                                                       analysis.local[block->terminator]->source);
                    analysis.ok = false;
                } else if(state == OwnState::Owned) {
                    edgeDrops.push(EdgeDrop { U32(l), b, successorIndex });
                }
            }
        }
    }
}

/*
 * Rewriting the body.
 */

static InstDrop* makeDrop(Analysis& analysis, Block& block, U32 localIndex, LocationId source) {
    auto slot = analysis.function.localAt(analysis.local, localIndex);
    auto implementation = dropImplementation(analysis.module, slot.type, source);
    if(!implementation) return nullptr;

    auto drop = createInst<InstDrop>(analysis.module, analysis.function, block, source, 0,
                                     analysis.module.scalar.unit, Place::inLocal(localIndex),
                                     ownershipOf(analysis.module, slot.type).drop);

    drop->implementation = implementation;
    return drop;
}

static void insertBlockDrops(Analysis& analysis, Array<Array<PendingDrop>>& blockDrops) {
    for(Size b = 0; b < analysis.blockCount(); b++) {
        if(blockDrops[b].isEmpty()) continue;

        auto block = analysis.blockAt(b);
        Array<ModulePtr<Inst>> existing;
        for(auto instruction: block->instructions.contents(analysis.local)) existing.push(instruction);

        // Positions are computed against the original numbering, so they are resolved before
        // anything is inserted and applied in one pass afterwards.
        Array<Size> positions;
        Array<InstDrop*> instructions;

        for(auto& pending: blockDrops[b]) {
            auto position = positionInBlock(analysis, b, pending.before);
            auto source = analysis.local[analysis.order[min(Size(pending.before), analysis.instructionCount - 1)]]->source;
            auto drop = makeDrop(analysis, *block, pending.local, source);
            if(!drop) continue;

            positions.push(position);
            instructions.push(drop);
        }

        block->instructions.clear();
        for(Size i = 0; i <= existing.size(); i++) {
            for(Size d = 0; d < positions.size(); d++) {
                if(positions[d] != i) continue;
                block->instructions.push(analysis.module.arena, (ModulePtr<Inst>)(instructions[d] - analysis.local));
            }

            if(i < existing.size()) block->instructions.push(analysis.module.arena, existing[i]);
        }
    }
}

/*
 * Splitting an edge to carry its drops.
 *
 * The alternative would be to put the drop at the top of the successor, which is only correct when
 * every path into it agreed - and the case this exists for is precisely the one where they do not.
 * Everything that names the old edge has to be redirected: the branch, both block graphs, and any
 * phi in the successor that reads a value from this predecessor.
 */
static void splitEdge(Analysis& analysis, Size fromIndex, Size toIndex, Array<U32>& locals) {
    auto& module = analysis.module;
    auto base = analysis.local;
    auto from = analysis.blockAt(fromIndex);
    auto to = analysis.blockAt(toIndex);

    auto fromPointer = analysis.function.blocks.get(base, fromIndex);
    auto toPointer = analysis.function.blocks.get(base, toIndex);

    auto split = analysis.function.addBlock(module);
    auto splitPointer = split - base;
    split->index = U16(analysis.function.blocks.size() - 1);
    split->source = base[from->terminator]->source;

    for(auto localIndex: locals) {
        auto drop = makeDrop(analysis, *split, localIndex, split->source);
        if(drop) split->instructions.push(module.arena, (ModulePtr<Inst>)(drop - base));
    }

    auto jump = createInst<InstJmp>(module, analysis.function, *split, split->source, 0,
                                    module.scalar.unit, toPointer);
    split->terminator = (ModulePtr<Inst>)(jump - base);
    split->outgoing[0] = toPointer;

    // The branch now leaves through the split block instead.
    auto terminator = base[from->terminator];
    if(terminator->kind == Value::Je) {
        auto& branch = (InstJe&)*terminator;
        if(branch.thenBlock == toPointer) branch.thenBlock = splitPointer;
        else if(branch.elseBlock == toPointer) branch.elseBlock = splitPointer;
    } else if(terminator->kind == Value::Jmp) {
        ((InstJmp&)*terminator).target = splitPointer;
    }

    for(auto& outgoing: from->outgoing) {
        if(outgoing == toPointer) outgoing = splitPointer;
    }

    for(Size i = 0; i < to->incoming.size(); i++) {
        if(to->incoming.get(base, i) == fromPointer) to->incoming.set(base, i, splitPointer);
    }

    split->incoming.push(module.arena, fromPointer);

    for(auto phiPointer: to->phis.contents(base)) {
        auto& phi = *base[phiPointer];
        for(Size i = 0; i < phi.inputs.size(); i++) {
            auto input = phi.inputs.get(base, i);
            if(input.block != fromPointer) continue;

            input.block = splitPointer;
            phi.inputs.set(base, i, input);
        }
    }
}

static void insertEdgeDrops(Analysis& analysis, Array<EdgeDrop>& edgeDrops) {
    // Grouped per edge, so one split block carries every drop that edge owes rather than one per.
    while(edgeDrops.size()) {
        auto first = edgeDrops[0];
        Array<U32> locals;
        Array<EdgeDrop> remaining;

        for(auto& drop: edgeDrops) {
            if(drop.fromBlock == first.fromBlock && drop.toBlock == first.toBlock) locals.push(drop.local);
            else remaining.push(drop);
        }

        splitEdge(analysis, first.fromBlock, first.toBlock, locals);
        edgeDrops = ::move(remaining);
    }
}

/*
 * Live ranges, for the printed form.
 *
 * A local is occupied at a point when it holds something reachable there: either the backward
 * liveness says a use is still ahead, or this is the instruction that gave it a value. Coalescing
 * the runs of occupied indices is what turns a per-point answer into the ranges-with-holes shape
 * the header describes.
 */
static void buildRanges(Analysis& analysis, OwnershipResult& result) {
    auto count = analysis.localCount;

    for(Size l = 0; l < count; l++) {
        Array<U8> occupied;
        for(Size i = 0; i < analysis.instructionCount; i++) occupied.push(0);

        for(Size b = 0; b < analysis.blockCount(); b++) {
            auto range = analysis.blockRanges[b];
            if(range.end == range.first) continue;

            // Replay the backward walk to recover liveness at each point inside the block, which
            // the fixpoint only kept at the two ends.
            Array<U8> before;
            auto live = analysis.liveOut[b];

            for(Size i = range.end; i > range.first; i--) {
                auto& effects = analysis.effects[i - 1];
                for(auto def: effects.defs) live[def] = 0;
                for(auto use: effects.uses) live[use] = 1;
                before.push(live[l]);
            }

            for(Size i = range.first; i < range.end; i++) {
                auto liveBefore = before[range.end - 1 - i];
                auto defines = false;
                for(auto def: analysis.effects[i].defs) defines = defines || def == l;
                for(auto init: analysis.effects[i].inits) defines = defines || init == l;

                occupied[i] = liveBefore || defines;
            }
        }

        result.rangeOffsets.push(U32(result.ranges.size()));
        auto emitted = 0u;
        auto open = maxLimit<U32>;

        for(Size i = 0; i <= analysis.instructionCount; i++) {
            auto live = i < analysis.instructionCount && occupied[i];

            if(live && open == maxLimit<U32>) {
                open = U32(i);
            } else if(!live && open != maxLimit<U32>) {
                result.ranges.push(LiveRange { open, U32(i) });
                open = maxLimit<U32>;
                emitted++;
            }
        }

        result.rangeCounts.push(emitted);
    }
}

} // namespace

/*
 * The entry points.
 */

bool runOwnership(Module& module, Function& function, OwnershipResult& result) {
    Analysis analysis(module, function);
    analysis.localCount = function.localCount();

    if(function.blocks.isEmpty()) return true;

    numberFunction(analysis);
    computeEffects(analysis);
    extendBorrowUses(analysis);
    attributePhiEdges(analysis);

    for(Size l = 0; l < analysis.localCount; l++) {
        auto slot = function.localAt(analysis.local, U32(l));
        auto ownership = ownershipOf(module, slot.type);

        /*
         * Which slots this frame is responsible for releasing.
         *
         * A borrowed parameter - the default convention, or `&` - refers to storage the caller
         * owns and keeps; dropping it here would release something the caller still holds. A `->`
         * parameter is the opposite: the caller handed ownership over and recorded the handover as
         * an InstMove, so this frame is the one that owes it a drop.
         *
         * The exception is a function that *is* the drop, or the sink. `Drop::drop` receives the
         * value in order to release it, and dropping its own parameter at the end would call
         * itself forever; `Sink::sink` empties its source into the destination, so what is left is
         * not something to release either. Both are the two places in the language where a `->`
         * parameter's disposal is the body's own business.
         */
        auto parameter = slot.value && analysis.local[slot.value]->kind == Value::Arg;
        auto disposer = function.instanceOf == module.coreClasses.drop ||
                        function.instanceOf == module.coreClasses.sink;

        auto owned = parameter
            ? (slot.convention == ast::BindType::Sink && !disposer)
            : !slot.borrowed;

        analysis.tracked.push(TrackedLocal {
            slot.type, slot.name, owned, ownership.drop != DropKind::None,
        });
    }

    for(Size i = 0; i < analysis.instructionCount; i++) analysis.stateBefore.push(Array<OwnState>());

    computeLiveness(analysis);
    computeOwnership(analysis);

    checkMoves(analysis);
    checkBorrows(analysis);

    result.locals = analysis.tracked;
    buildRanges(analysis, result);

    // Nothing is rewritten once something has been reported: the IR the diagnostics were derived
    // from is the one worth printing, and inserting drops into a body already known to be wrong
    // produces a second round of diagnostics about the first round's mistakes.
    if(!analysis.ok) return false;

    // A generic body is checked and then left alone. Its type variables classify conservatively -
    // Design.md requires an unconstrained parameter to be treated as owning something - so drops
    // derived here would be drops of a type nothing knows the shape of. What reaches the backend is
    // this function's specializations, and each of those is an ordinary function that gets its own.
    if(function.gen) return true;

    Array<Array<PendingDrop>> blockDrops;
    for(Size b = 0; b < analysis.blockCount(); b++) blockDrops.push(Array<PendingDrop>());

    Array<EdgeDrop> edgeDrops;
    placeDrops(analysis, blockDrops, edgeDrops);

    if(!analysis.ok) return false;

    insertBlockDrops(analysis, blockDrops);
    insertEdgeDrops(analysis, edgeDrops);
    return true;
}

// Which functions the passes run over. A signature has no body, an intrinsic is generated at each
// call site rather than being one function, and a generic body is checked but never given drops -
// what reaches the backend is its specializations, and those are ordinary functions that get their
// own drops here. Checking the generic body anyway is what puts a use-after-move diagnostic on the
// function that has the bug instead of once per instantiation.
static bool ownershipApplies(Function& function) {
    return !function.signature && !function.intrinsic && function.blocks.isNotEmpty();
}

bool runProgramOwnership(Program& program) {
    auto base = *program.arena;
    auto success = true;

    if(!program.ownership) program.ownership = Ptr<OwnershipResults>(new OwnershipResults());

    for(auto module: program.modules) {
        // Specializations and drop glue are appended while this runs, so the list is walked by
        // index - the same reason resolveModuleBodies does.
        for(Size i = 0; i < module->functionOrder.size(); i++) {
            auto pointer = module->functionOrder.get(base, i);
            auto function = base[pointer];
            if(!ownershipApplies(*function)) continue;

            OwnershipResult result;
            auto ok = runOwnership(*module, *function, result);
            success = success && ok;

            // add() hands back uninitialized storage, so the result is constructed into it rather
            // than assigned - assigning would run the destructor of whatever the slot happened to
            // contain, which for a struct of Arrays means freeing garbage pointers.
            if(ok && !function->gen) {
                new (program.ownership->functions.add(U32(pointer)).value) OwnershipResult(::move(result));
            }
        }
    }

    return success;
}

/*
 * Printing.
 *
 * The point of this file having a dump of its own is that liveness is the pass everything else
 * believes: a wrong range shows up as a drop in the wrong place, or as no drop at all, and neither
 * is obvious from the IR it produced. Printing the ranges makes the belief checkable.
 */

static void writeIndex(Net::Writer& writer, U32 value) {
    writer.writeBytes(32, [&](Byte* buffer) {
        return show(value, (char*)buffer, 32);
    });
}

static void printFunctionOwnership(Net::Writer& writer, Context& context, Program& program,
                                   Function& function, OwnershipResult& result) {
    writer.writeString("fn "_v);
    writer.writeString(context.findName(function.name));
    writer.writeString(" {\n"_v);

    for(Size l = 0; l < result.locals.size(); l++) {
        auto& tracked = result.locals[l];
        writer.writeString("  %"_v);

        if(tracked.name) writer.writeString(context.findName(tracked.name));
        else {
            writer.writeString("local"_v);
            writeIndex(writer, U32(l));
        }

        writer.writeString(": "_v);
        writer.writeString(stringView(describeType(context, *program.types, tracked.type)));

        if(!tracked.owned) writer.writeString(" borrowed"_v);
        if(tracked.droppable) writer.writeString(" droppable"_v);

        writer.writeString(" live"_v);
        auto ranges = result.rangesOf(l);

        if(!ranges.length) writer.writeString(" never"_v);

        for(Size i = 0; i < ranges.length; i++) {
            writer.writeString(" ["_v);
            writeIndex(writer, ranges[i].from);
            writer.writeString(", "_v);
            writeIndex(writer, ranges[i].to);
            writer.writeByte(')');
        }

        writer.writeByte('\n');
    }

    writer.writeString("}\n"_v);
}

void printOwnership(Net::Writer& writer, Context& context, Program& program) {
    auto base = *program.arena;
    Size index = 0;

    if(!program.ownership) return;

    for(auto module: program.modules) {
        for(auto pointer: module->functionOrder.contents(base)) {
            auto found = program.ownership->functions.get(U32(pointer));
            if(!found) continue;

            auto function = base[pointer];
            if(!module->root && !function->used) continue;
            if(function->signature) continue;

            if(index++) writer.writeByte('\n');
            printFunctionOwnership(writer, context, program, *function, found.unwrap());
        }
    }
}

/*
 * ---------------------------------------------------------------------------------------------
 * What this pass does not do yet.
 *
 * Everything below is a deliberate omission rather than an oversight, and each is conservative in
 * the same direction: the analysis either rejects a program it could have accepted, or drops later
 * than it had to. Nothing here can make it accept a program it should reject, which is the
 * property worth preserving while the rest is filled in.
 *
 * **Drop flags.** A value moved out of on only some paths reaching its last use needs a runtime bit
 * saying whether the slot still owns anything. The bit, the block split around the conditional
 * drop, and InstDrop::flag are all designed for; what is here reports instead of emitting them.
 * This is the largest single item and the one an ordinary program hits first - `if c: consume(x)`
 * is enough.
 *
 * **Partial moves.** Moving one field out of an aggregate leaves the slot half-owned. checkMove()
 * rejects it, because representing it means a drop flag per field and a drop that runs over a
 * subset of members - the same machinery drop flags need, one level further in.
 *
 * **Multi-constructor derived drops.** dropGlueFor() handles a tuple and a single-constructor
 * record. A multi-constructor one needs the glue to read the discriminant and drop the members of
 * whichever constructor is present, which is a switch this pass does not build; it reports and asks
 * for an authored `instance Drop`.
 *
 * **Loans that survive a call.** A borrow's extent ends at the last use of the borrow value, so a
 * callee that retains what it was given is not modelled. That needs the function summaries of
 * Milestone 6, and until then a call returning a borrow is rejected rather than mis-tracked - see
 * the return-root marker's own diagnostic in resolveSignature.
 *
 * **Two-phase borrows.** `f(&x, g(x))` evaluates `g(x)` while the borrow of `x` for the first
 * argument is already live, which is rejected here and accepted by Rust through a reservation
 * phase. The resolver happens to evaluate arguments before creating the borrow, so the common
 * shapes do not hit it, but the rule is not stated anywhere and should be.
 *
 * **Per-field granularity for liveness and ownership.** Both are tracked per local, so borrowing
 * `x.a` keeps all of `x` alive. Conflict *detection* is per place and does distinguish `x.a` from
 * `x.b`; it is only the extent that is coarse.
 *
 * **Address escapes.** `addressOf` produces a raw pointer that can be stored anywhere, and no
 * extent computed here bounds it. This is unchecked by construction - it is what `%T` means - but
 * it does mean a program can defeat the analysis through `Native` without being told.
 *
 * **The checked reference rungs.** `Ref` and `RegionPtr` classify conservatively in ownershipOf()
 * and are not constructible yet, so nothing exercises them.
 * ---------------------------------------------------------------------------------------------
 */
