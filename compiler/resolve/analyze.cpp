#include "analyze.h"
#include "expr.h"
#include "generic.h"
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

/*
 * Which storage one value may refer to.
 *
 * Ownership is stated over places, but everything a *caller* needs to know is stated over values:
 * whether the pointer this function returned points into its own frame, whether the borrow it was
 * handed ended up somewhere that outlives the call. So each value carries the set of roots it may
 * refer to, as an ordinary forward fixpoint over the SSA graph.
 *
 * The set is over locals rather than over "argument or not", because the two questions the result
 * answers are different: a summary asks which *arguments* a value is rooted in, and storage-class
 * selection asks which *allocations* have to outlive the frame. A local backed by an Arg answers
 * the first, every local answers the second, and one set covers both.
 */
struct Provenance {
    LocalSet locals;
    bool global = false;

    // Storage this analysis cannot name: the result of an opaque call, or anything reached through
    // a raw pointer whose own origin was already unknown. Conservative in the one direction that
    // matters - it can only make storage live longer than it had to.
    bool unknown = false;
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
    Size valueCount = 0;

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

    /*
     * The flow facts, all keyed the same way: values by their id, roots by their local index.
     *
     * `contents` is what makes this more than a walk of the operand graph. A value written into a
     * place is reachable through that place's root afterwards, so an array's buffer is reachable
     * through the array, and returning the array is what makes the buffer outlive the frame. It is
     * field-insensitive - `x.a` and `x.b` contribute to one set - which is precise enough for the
     * question and avoids a second projection model inside the analysis.
     */
    Array<Provenance> values;
    Array<Provenance> contents;

    /*
     * Whether each root's storage has to stay valid after this frame returns.
     *
     * Two arrays for one question, because the two consumers ask it differently. `outlives` starts
     * with every parameter's slot set, since a parameter names the caller's storage and that
     * already survives - which is what makes "written into an argument" an escape with no rule of
     * its own. `escaped` records only what this pass *proved* escapes, which is what a summary
     * reports as a retained argument and what storage-class selection reads.
     */
    LocalSet outlives;
    LocalSet escaped;

    // What each root's representation has to be able to do, per Design.md's owner mutation demand.
    Array<ReprRequirements> demand;

    // Which roots this frame has to hand storage back for. Not simply "is heap-placed": storage
    // that escaped is heap-placed *because* something else owns it now.
    LocalSet releasesStorage;

    // Reported diagnostics, but also the switch that decides whether this run is one of the
    // fixpoint's silent rounds or the final one that rewrites the body.
    bool reporting = true;
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
 * Every diagnostic this file produces goes through here, because the passes run more than once.
 *
 * Summaries are a fixpoint: a function is analyzed as many times as it takes for what its callees
 * say about themselves to stop changing, and only the last of those rounds is the one whose
 * diagnostics are the program's. Reporting from the silent rounds would say the same thing three
 * times; not recording `ok` in them would let a round that failed still be treated as a result.
 */
template<class... Args>
static void report(Analysis& analysis, StringView text, LocationId source, Args&&... args) {
    analysis.ok = false;
    if(analysis.reporting) {
        analysis.context.diagnostics.error(text, source, forward<Args>(args)...);
    }
}

static void note(Analysis& analysis, StringView text, LocationId source) {
    if(analysis.reporting) {
        analysis.context.diagnostics.message(Diagnostics::MessageLevel, text, source);
    }
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
 * Provenance, containment, and what has to outlive the frame.
 *
 * Three facts computed together because each needs the one before it. A value's provenance is the
 * set of roots it may refer to; a root's contents are the provenance of everything written into
 * it; and a root outlives the frame when something handed it, or something it contains, to code
 * that runs after this function returns.
 *
 * All three are "may" analyses climbing from empty, so a round that has not seen a callee's summary
 * yet is optimistic rather than wrong - the fixpoint above only ever adds.
 */

static Provenance emptyProvenance(Size count) {
    return Provenance { emptySet(count), false, false };
}

static bool joinProvenance(Provenance& target, const Provenance& source) {
    auto changed = false;

    for(Size i = 0; i < source.locals.size() && i < target.locals.size(); i++) {
        if(source.locals[i] && !target.locals[i]) {
            target.locals[i] = 1;
            changed = true;
        }
    }

    if(source.global && !target.global) { target.global = true; changed = true; }
    if(source.unknown && !target.unknown) { target.unknown = true; changed = true; }
    return changed;
}

static Provenance& provenanceOf(Analysis& analysis, ModulePtr<Value> value) {
    static Provenance none;
    if(!value) return none;

    auto id = analysis.local[value]->id;
    return id < analysis.values.size() ? analysis.values[id] : none;
}

// Whether a value is the kind of thing that can refer to storage at all. A scalar computed into a
// register refers to nothing, and saying so keeps arithmetic out of the fixpoint entirely.
static bool refersToStorage(Analysis& analysis, TypePtr type) {
    return isMemoryType(analysis.global, type) || isPointer(analysis.global, type) ||
           isBorrow(analysis.global, type);
}

// The roots a place names. A projection stays inside the storage its root names, so the path is
// not walked - which is the same reason liveness is tracked per local.
static Provenance placeProvenance(Analysis& analysis, const Place& place) {
    auto result = emptyProvenance(analysis.localCount);

    switch(place.root) {
        case PlaceRoot::Local:
            if(place.local < analysis.localCount) result.locals[place.local] = 1;
            else result.unknown = true;
            break;

        case PlaceRoot::Global:
            result.global = true;
            break;

        case PlaceRoot::Pointer:
            // The place is the memory the pointer names, so its roots are the pointer's own.
            joinProvenance(result, provenanceOf(analysis, place.pointer));
            break;
    }

    return result;
}

// What reading out of a place produces: everything anything ever wrote into the roots it names.
static Provenance contentsOfPlace(Analysis& analysis, const Place& place) {
    auto roots = placeProvenance(analysis, place);
    auto result = emptyProvenance(analysis.localCount);

    for(Size i = 0; i < analysis.localCount; i++) {
        if(roots.locals[i]) joinProvenance(result, analysis.contents[i]);
    }

    if(roots.global || roots.unknown) result.unknown = true;
    return result;
}

// What a value contributes when it is written somewhere. An aggregate is copied byte for byte, so
// what lands in the destination is what the source contained rather than the source itself.
static Provenance transferredProvenance(Analysis& analysis, ModulePtr<Value> value) {
    if(!value) return emptyProvenance(analysis.localCount);

    auto result = emptyProvenance(analysis.localCount);
    auto type = analysis.local[value]->type;

    if(isMemoryType(analysis.global, type)) {
        auto roots = provenanceOf(analysis, value);
        for(Size i = 0; i < analysis.localCount; i++) {
            if(roots.locals[i]) joinProvenance(result, analysis.contents[i]);
        }

        if(roots.global || roots.unknown) result.unknown = true;
    } else if(refersToStorage(analysis, type)) {
        joinProvenance(result, provenanceOf(analysis, value));
    }

    return result;
}

// The summary of a called function, or nothing when the callee is not one this pass can see.
static FunctionSummary* summaryOf(Analysis& analysis, ModulePtr<Function> callee) {
    if(!callee) return nullptr;

    auto& summary = analysis.local[callee]->summary;
    return summary.ready && !summary.opaque ? &summary : nullptr;
}

// What a call's result may refer to, composed from the callee's declared return-root group. A
// borrow coming out of a call is related to every member of that group at once, which is
// Design.md's deliberate conservatism: the callee may have returned any of them.
static Provenance callResultProvenance(Analysis& analysis, ModulePtr<Function> callee,
                                       ModuleList<ModulePtr<Value>, false>& args, TypePtr type) {
    auto result = emptyProvenance(analysis.localCount);
    if(!refersToStorage(analysis, type)) return result;

    auto summary = summaryOf(analysis, callee);
    if(!summary) {
        result.unknown = true;
        return result;
    }

    if(summary->resultBound == StorageBound::Arguments) {
        U16 index = 0;
        for(auto arg: args.contents(analysis.local)) {
            if(summary->declaredRoots & (U64(1) << min(U16(63), index))) {
                joinProvenance(result, provenanceOf(analysis, arg));
            }

            index++;
        }
    } else if(summary->resultBound != StorageBound::Frame) {
        result.unknown = true;
    }

    return result;
}

// One round of the value fixpoint. Returns whether anything was added.
static bool flowRound(Analysis& analysis) {
    auto changed = false;
    auto local = analysis.local;

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto pointer = analysis.order[i];
        auto& instruction = *local[pointer];
        auto id = instruction.id;
        if(id >= analysis.values.size()) continue;

        auto produced = emptyProvenance(analysis.localCount);

        // A value that owns a slot refers to that slot and to nothing else, whatever produced it -
        // an allocation, a copy, a call whose aggregate result landed in one.
        auto backing = backingLocal(analysis, (ModulePtr<Value>)pointer);
        if(backing != maxLimit<U32>) {
            produced.locals[backing] = 1;
        } else {
            switch(instruction.kind) {
                case Value::LoadPlace: {
                    auto& read = (InstLoadPlace&)instruction;

                    // An aggregate is addressed rather than loaded, so the value *is* the place.
                    // A scalar or a pointer read out of storage is whatever was written there.
                    if(isMemoryType(analysis.global, instruction.type)) {
                        joinProvenance(produced, placeProvenance(analysis, read.place));
                    } else if(refersToStorage(analysis, instruction.type)) {
                        joinProvenance(produced, contentsOfPlace(analysis, read.place));
                    }

                    break;
                }

                case Value::Borrow:
                    joinProvenance(produced, placeProvenance(analysis, ((InstBorrow&)instruction).place));
                    break;

                case Value::Address:
                    joinProvenance(produced, placeProvenance(analysis, ((InstAddress&)instruction).place));
                    break;

                case Value::Move:
                    joinProvenance(produced, placeProvenance(analysis, ((InstMove&)instruction).place));
                    break;

                case Value::Cast:
                    // A cast of a pointer is the same address written differently, and asInt/asPtr
                    // are casts. Losing the root here is exactly how an escape would go unnoticed.
                    joinProvenance(produced, provenanceOf(analysis, ((InstUnary&)instruction).from));
                    break;

                case Value::Add:
                case Value::Sub:
                    // Pointer arithmetic stays inside whatever the pointer named.
                    if(refersToStorage(analysis, instruction.type)) {
                        joinProvenance(produced, provenanceOf(analysis, ((InstBinary&)instruction).lhs));
                        joinProvenance(produced, provenanceOf(analysis, ((InstBinary&)instruction).rhs));
                    }

                    break;

                case Value::Call: {
                    auto& call = (InstCall&)instruction;
                    joinProvenance(produced, callResultProvenance(analysis, call.callee, call.args, instruction.type));
                    break;
                }

                case Value::GenCall:
                    /*
                     * No summary to read - the instance is decided per specialization - so the
                     * conservative reading of what a reference result may point at is: anything
                     * this call was handed.
                     *
                     * Deliberately without `unknown`, which would be the safer answer anywhere
                     * else. An InstGenCall never survives specialization, and every specialization
                     * is checked in full with the real instructions in place; so what this decides
                     * is only how much a *generic* body is allowed to say about itself, and the
                     * soundness of any concrete program is settled elsewhere. Saying `unknown` here
                     * would instead make every generic accessor unable to declare its own roots.
                     */
                    if(refersToStorage(analysis, instruction.type)) {
                        for(auto arg: ((InstGenCall&)instruction).args.contents(local)) {
                            joinProvenance(produced, provenanceOf(analysis, arg));
                        }
                    }

                    break;

                case Value::Native:
                    // copyMemory and setMemory produce nothing, and a syscall produces an integer.
                    // Neither hands back an address this analysis could have named.
                    if(refersToStorage(analysis, instruction.type)) produced.unknown = true;
                    break;

                case Value::Phi:
                    for(auto input: ((InstPhi&)instruction).inputs.contents(local)) {
                        joinProvenance(produced, provenanceOf(analysis, input.value));
                    }

                    break;

                default:
                    break;
            }
        }

        changed = joinProvenance(analysis.values[id], produced) || changed;

        // Writing into a place makes what was written reachable through that place's root.
        if(instruction.kind == Value::Init || instruction.kind == Value::Assign) {
            auto& write = (InstInit&)instruction;
            auto roots = placeProvenance(analysis, write.place);
            auto stored = transferredProvenance(analysis, write.value);

            for(Size l = 0; l < analysis.localCount; l++) {
                if(roots.locals[l]) changed = joinProvenance(analysis.contents[l], stored) || changed;
            }
        }
    }

    return changed;
}

static void computeProvenance(Analysis& analysis) {
    analysis.valueCount = analysis.function.valueCounter;

    for(Size i = 0; i < analysis.valueCount; i++) {
        analysis.values.push(emptyProvenance(analysis.localCount));
    }

    for(Size i = 0; i < analysis.localCount; i++) {
        analysis.contents.push(emptyProvenance(analysis.localCount));
    }

    /*
     * What is inside a parameter is rooted in that parameter.
     *
     * Nothing in this frame wrote it, so without this a pointer loaded out of an argument would
     * come from nowhere and an accessor could not say what its result was rooted in. "Reachable
     * through" is exactly the relation Design.md's rule is stated over - "every borrow escaping
     * through the result must be transitively rooted in a `return` parameter" - so a parameter's
     * contents starting as the parameter itself is that rule's base case.
     */
    for(Size l = 0; l < analysis.localCount; l++) {
        auto slot = analysis.function.localAt(analysis.local, U32(l));
        if(slot.value && analysis.local[slot.value]->kind == Value::Arg) analysis.contents[l].locals[l] = 1;
    }

    // Bounded rather than unbounded: each round can only add, and the lattice is finite, so this
    // settles - the bound is a guard against a rule added later that is not monotone, not a
    // shortcut. Loops need one round per level of the value graph they close over.
    for(Size round = 0; round < analysis.instructionCount + 2; round++) {
        if(!flowRound(analysis)) break;
    }
}

/*
 * What has to outlive the frame.
 *
 * Seeded from the four instructions that can hand storage to something running after this function
 * returns, then closed over containment: if a root outlives the frame, so must everything reachable
 * through it, or the array survives and its buffer does not.
 */
static bool markEscaped(Analysis& analysis, const Provenance& roots) {
    auto changed = false;

    for(Size l = 0; l < analysis.localCount; l++) {
        if(roots.locals[l] && !analysis.escaped[l]) {
            analysis.escaped[l] = 1;
            analysis.outlives[l] = 1;
            changed = true;
        }
    }

    return changed;
}

// One round of seeds. Separate from the closure below only so that both can be repeated together:
// a store into a root that a later instruction turns out to hand away is an escape too, and one
// pass in instruction order would miss it.
static bool escapeRound(Analysis& analysis) {
    auto changed = false;

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];

        switch(instruction.kind) {
            case Value::Ret: {
                auto value = ((InstRet&)instruction).value;
                if(!value) break;

                /*
                 * An aggregate result is copied into storage the caller passed in, so the slot it
                 * came out of stays behind and only what it *contained* leaves. A borrow or a
                 * pointer is the address itself, so the root leaves with it.
                 *
                 * A root that is a *parameter's* slot is deliberately not marked. Handing a borrow
                 * of an argument back is not an escape, it is the return-root mechanism, and the
                 * caller already bounds that storage - the summary says so through its declared
                 * group rather than through this bit. Marking it here would make every accessor's
                 * argument look like something that had to outlive its caller, and every value
                 * anyone ever borrowed from would land on the heap.
                 */
                auto leaving = transferredProvenance(analysis, value);

                for(Size l = 0; l < analysis.localCount; l++) {
                    auto slot = analysis.function.localAt(analysis.local, U32(l));
                    if(slot.value && analysis.local[slot.value]->kind == Value::Arg) leaving.locals[l] = 0;
                }

                changed = markEscaped(analysis, leaving) || changed;
                break;
            }

            case Value::Init:
            case Value::Assign: {
                auto& write = (InstInit&)instruction;
                auto roots = placeProvenance(analysis, write.place);

                auto escaping = roots.global || roots.unknown;
                for(Size l = 0; l < analysis.localCount && !escaping; l++) {
                    escaping = roots.locals[l] && analysis.outlives[l];
                }

                if(escaping) {
                    changed = markEscaped(analysis, transferredProvenance(analysis, write.value)) || changed;
                }

                break;
            }

            case Value::Call: {
                auto& call = (InstCall&)instruction;
                auto summary = summaryOf(analysis, call.callee);
                U16 index = 0;

                for(auto arg: call.args.contents(analysis.local)) {
                    auto retained = !summary || index >= summary->args.size() ||
                                    summary->args.get(analysis.local, index).retained;

                    if(retained) {
                        changed = markEscaped(analysis, transferredProvenance(analysis, arg)) || changed;
                    }

                    index++;
                }

                break;
            }

            case Value::GenCall:
                // No summary to consult, so everything handed over is assumed kept.
                for(auto arg: ((InstGenCall&)instruction).args.contents(analysis.local)) {
                    changed = markEscaped(analysis, transferredProvenance(analysis, arg)) || changed;
                }

                break;

            default:
                break;
        }
    }

    // Containment closure. A root that outlives the frame drags everything written into it along,
    // and that relation is what connects an array's own storage to its buffer's.
    for(Size l = 0; l < analysis.localCount; l++) {
        if(!analysis.outlives[l]) continue;

        for(Size m = 0; m < analysis.localCount; m++) {
            // A root contains itself - that is how a parameter's contents are rooted in the
            // parameter - and that says nothing about escaping.
            if(m == l) continue;

            if(analysis.contents[l].locals[m] && !analysis.escaped[m]) {
                analysis.escaped[m] = 1;
                analysis.outlives[m] = 1;
                changed = true;
            }
        }
    }

    return changed;
}

static void computeOutliving(Analysis& analysis) {
    analysis.outlives = emptySet(analysis.localCount);
    analysis.escaped = emptySet(analysis.localCount);

    // A parameter's storage is the caller's and already outlives this frame. It is set in
    // `outlives` and not in `escaped`, because nothing here proved anything about it.
    for(Size l = 0; l < analysis.localCount; l++) {
        auto slot = analysis.function.localAt(analysis.local, U32(l));
        if(slot.value && analysis.local[slot.value]->kind == Value::Arg) analysis.outlives[l] = 1;
    }

    for(Size round = 0; round <= analysis.localCount + 1; round++) {
        if(!escapeRound(analysis)) break;
    }
}

/*
 * Owner mutation demand (Design.md's "Binding mutability and owner mutation demand").
 *
 * Deliberately keyed on the root rather than on the binding that named it, and deliberately not
 * raised by initialization: filling storage that held nothing is what every owned value's first
 * instruction does, so counting it would make every root writable and the analysis would answer
 * the same thing everywhere. Overwriting a live value is the operation that needs writable storage,
 * which is the whole reason Init and Assign are two instructions.
 */
static void raiseDemand(Analysis& analysis, const Provenance& roots, const ReprRequirements& what) {
    for(Size l = 0; l < analysis.localCount; l++) {
        if(roots.locals[l]) analysis.demand[l].raise(what);
    }
}

static void computeDemand(Analysis& analysis) {
    for(Size l = 0; l < analysis.localCount; l++) analysis.demand.push(ReprRequirements());

    auto writable = ReprRequirements { MutationDemand::Writable, false, false };
    auto unknown = ReprRequirements { MutationDemand::Unknown, false, false };

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];

        switch(instruction.kind) {
            case Value::Assign: {
                auto& write = (InstInit&)instruction;
                auto roots = placeProvenance(analysis, write.place);
                raiseDemand(analysis, roots, writable);

                /*
                 * Replacing indirect storage this owner holds is what a regrow is.
                 *
                 * There is no `resize` operation in the language to key this on, so the structural
                 * definition is the one above: an assignment - not an initialization - of a pointer
                 * into a projection of a root replaced storage that root was holding. That is
                 * exactly what an array's grow does and what nothing else does, and it is the fact
                 * that keeps a growable array's buffer off the frame.
                 */
                if(write.place.projections.isNotEmpty() &&
                   isPointer(analysis.global, analysis.local[write.value]->type)) {
                    raiseDemand(analysis, roots, ReprRequirements { MutationDemand::ReadOnly, false, true });
                }

                break;
            }

            case Value::Borrow:
                if(((InstBorrow&)instruction).mut) {
                    raiseDemand(analysis, placeProvenance(analysis, ((InstBorrow&)instruction).place), writable);
                }

                break;

            case Value::Address:
                // Design.md's Pointers section: the memory a raw pointer names is always mutable,
                // so handing one out is both a write capability and a demand for storage to exist.
                raiseDemand(analysis, placeProvenance(analysis, ((InstAddress&)instruction).place),
                            ReprRequirements { MutationDemand::Writable, true, false });
                break;

            case Value::Call: {
                auto& call = (InstCall&)instruction;
                auto summary = summaryOf(analysis, call.callee);
                U16 index = 0;

                for(auto arg: call.args.contents(analysis.local)) {
                    auto roots = provenanceOf(analysis, arg);

                    if(!summary || index >= summary->args.size()) {
                        if(refersToStorage(analysis, analysis.local[arg]->type)) {
                            raiseDemand(analysis, roots, unknown);
                        }
                    } else {
                        raiseDemand(analysis, roots, summary->args.get(analysis.local, index).requirements);
                    }

                    index++;
                }

                break;
            }

            case Value::GenCall:
                for(auto arg: ((InstGenCall&)instruction).args.contents(analysis.local)) {
                    if(refersToStorage(analysis, analysis.local[arg]->type)) {
                        raiseDemand(analysis, provenanceOf(analysis, arg), unknown);
                    }
                }

                break;

            case Value::Native:
                // Native's block operations write through whatever they were given, and there is
                // no signature here to say which of the two arguments that was.
                for(auto arg: ((InstNative&)instruction).args.contents(analysis.local)) {
                    if(refersToStorage(analysis, analysis.local[arg]->type)) {
                        raiseDemand(analysis, provenanceOf(analysis, arg),
                                    ReprRequirements { MutationDemand::Writable, true, false });
                    }
                }

                break;

            default:
                break;
        }
    }
}

/*
 * The summary: what a caller may know without looking at this body.
 *
 * Derived from the flow facts above rather than computed separately, which is the point of having
 * built them: "is this argument retained" is "did anything derived from its slot escape", and
 * "where is the result rooted" is the provenance of what every `ret` handed back.
 */

// The slot one parameter's storage is named by, or none for a scalar passed in a register - which
// has no storage in this frame for anything to be rooted in.
static U32 argLocal(Analysis& analysis, ModulePtr<Arg> arg) {
    for(U32 l = 0; l < analysis.localCount; l++) {
        if(analysis.function.localAt(analysis.local, l).value == (ModulePtr<Value>)arg) return l;
    }

    return maxLimit<U32>;
}

static U64 rootBit(U16 index) {
    return index < 64 ? U64(1) << index : 0;
}

// Rebuilds the summary from the current round's facts, reporting whether anything moved. The
// fixpoint above runs until every function in the program answers no.
static bool deriveSummary(Analysis& analysis) {
    auto& function = analysis.function;
    auto& summary = function.summary;
    auto changed = !summary.ready;

    // Sized once and then updated in place: the fixpoint visits a function many times and the
    // module arena never gives anything back, so pushing per round would be a leak per round.
    while(summary.args.size() < function.args.size()) {
        summary.args.push(analysis.module.arena, ArgSummary());
    }

    U16 index = 0;
    U64 declared = 0;

    for(auto argPointer: function.args.contents(analysis.local)) {
        auto arg = analysis.local[argPointer];
        auto slot = argLocal(analysis, argPointer);

        ArgSummary updated;
        updated.returnRoot = arg->returnRoot;

        if(slot != maxLimit<U32>) {
            updated.requirements = analysis.demand[slot];
            updated.retained = analysis.escaped[slot];
        }

        // A `&` parameter is a declaration that the caller's storage must be writable, whatever
        // this body turns out to do with it. The signature is the contract, not the body.
        if(arg->isMutableBorrow()) updated.requirements.mutation = MutationDemand::Writable;

        if(arg->returnRoot) declared |= rootBit(index);

        if(!(summary.args.get(analysis.local, index) == updated)) {
            summary.args.set(analysis.local, index, updated);
            changed = true;
        }

        index++;
    }

    // What every return path handed back, unioned. Provenance composition through a call already
    // happened when the call's own result got its provenance, so a function returning another
    // selector's result arrives here with that callee's roots already mapped through the operands.
    auto returned = emptyProvenance(analysis.localCount);
    auto returnsValue = false;

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];
        if(instruction.kind != Value::Ret) continue;

        auto value = ((InstRet&)instruction).value;
        if(!value) continue;

        returnsValue = true;
        joinProvenance(returned, transferredProvenance(analysis, value));
    }

    U64 actual = 0;
    auto invalid = returned.global || returned.unknown;

    for(Size l = 0; l < analysis.localCount; l++) {
        if(!returned.locals[l]) continue;

        auto slot = analysis.function.localAt(analysis.local, U32(l));
        auto arg = slot.value && analysis.local[slot.value]->kind == Value::Arg
            ? (Arg*)analysis.local[slot.value] : nullptr;

        // A borrow rooted in a sunk parameter is as invalid as one rooted in a local: the callee
        // owns what it was given, so there is no caller-side root left to keep it alive.
        if(arg && arg->convention != ast::BindType::Sink) actual |= rootBit(arg->index);
        else invalid = true;
    }

    auto bound = StorageBound::Frame;
    if(invalid) bound = StorageBound::Escapes;
    else if(actual) bound = StorageBound::Arguments;

    auto borrowed = isBorrow(analysis.global, function.returnType);
    auto mutableResult = borrowed &&
        ((BorrowType*)analysis.global[function.returnType])->mut;

    // Everything reaching here is about the *result*, so a function that returns nothing keeps the
    // frame-bounded answer rather than inheriting a root from a path that returned no value.
    if(!returnsValue) {
        actual = 0;
        invalid = false;
        bound = StorageBound::Frame;
    }

    if(summary.declaredRoots != declared || summary.actualRoots != actual ||
       summary.invalidRoot != invalid || summary.resultBound != bound ||
       summary.returnsBorrow != borrowed || summary.mutableResult != mutableResult) {
        summary.declaredRoots = declared;
        summary.actualRoots = actual;
        summary.invalidRoot = invalid;
        summary.resultBound = bound;
        summary.returnsBorrow = borrowed;
        summary.mutableResult = mutableResult;
        changed = true;
    }

    summary.ready = true;
    return changed;
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

/*
 * How far a borrow's extent reaches.
 *
 * To the last instruction that consumes the borrow value - and then, if one of those was a call
 * that may hand it back, to the last use of what the call produced. That second clause is the
 * whole of Design.md's "the caller conservatively keeps every member borrowed until the last use
 * of all result borrows", and it is why the loan on `objects` does not end at the call to
 * `getMutableEntry` but at the last use of the entry it returned.
 *
 * Transitive, because a caller may hand the result on again: a chain of selectors keeps the
 * original root borrowed for the whole chain. The `seen` list is what makes a value used by two
 * calls, or a loop, terminate instead of walking the graph forever.
 */
static U32 lastUseOf(Analysis& analysis, ModulePtr<Inst> pointer) {
    auto found = analysis.indexOf.get(U32(pointer));
    auto last = found ? found.unwrap() : 0;

    Array<ModulePtr<Value>> pending;
    Array<ModulePtr<Value>> seen;
    pending.push((ModulePtr<Value>)pointer);

    while(pending.size()) {
        auto value = pending.pop().unwrap();

        auto visited = false;
        for(auto& entry: seen) visited = visited || entry == value;
        if(visited) continue;
        seen.push(value);

        for(auto user: analysis.local[value]->uses.contents(analysis.local)) {
            auto index = analysis.indexOf.get(U32(user));
            if(index && index.unwrap() > last) last = index.unwrap();

            auto& instruction = *analysis.local[user];
            if(instruction.kind != Value::Call) continue;

            auto& call = (InstCall&)instruction;
            auto summary = summaryOf(analysis, call.callee);
            if(!summary) continue;

            U16 position = 0;
            for(auto arg: call.args.contents(analysis.local)) {
                if(arg == value && (summary->declaredRoots & rootBit(position))) {
                    pending.push((ModulePtr<Value>)user);
                }

                position++;
            }
        }
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

            report(analysis,
                   borrow.mut
                       ? "this use conflicts with a mutable borrow of the same storage, which is exclusive while it is live"_v
                       : "this write conflicts with an immutable borrow of the same storage that is still live"_v,
                   instruction.source);

            note(analysis, "the borrow it conflicts with is here"_v, borrowed.source);
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
                report(analysis, "cannot move a part of a value out of it - move the whole value instead"_v,
                       instruction.source);
                continue;
            }

            auto root = rootLocal(analysis, moved.place);
            if(root != maxLimit<U32> && !analysis.tracked[root].owned) {
                report(analysis, "cannot take ownership of borrowed storage - a `&` binding never owns what it refers to"_v,
                       instruction.source);
                continue;
            }
        }

        for(auto use: effects.uses) {
            if(states[use] == OwnState::Owned || states[use] == OwnState::Uninitialized) continue;

            auto name = analysis.tracked[use].name;
            auto moved = states[use] == OwnState::Moved;

            if(name) {
                report(analysis,
                       moved ? "%@ has been moved out of and cannot be used again"_v
                             : "%@ may have been moved out of on some paths reaching here"_v,
                       instruction.source, analysis.context.findName(name));
            } else {
                report(analysis,
                       moved ? "this value has been moved out of and cannot be used again"_v
                             : "this value may have been moved out of on some paths reaching here"_v,
                       instruction.source);
            }
        }
    }
}

/*
 * The return-root check (Design.md's "Borrows in return position").
 *
 * The declaration is the contract and the body is what has to fit it, so this compares two things
 * the summary already holds: the group the signature declared, and the roots resolving every return
 * path actually found. Nothing here looks at a callee's body - a call's result arrived with the
 * callee's declared group already mapped through the operands, which is what makes provenance
 * compose transitively through a helper without inspecting one.
 */
static void checkReturnRoots(Analysis& analysis) {
    auto& function = analysis.function;
    auto& summary = function.summary;
    if(!summary.returnsBorrow) return;

    auto source = function.source;

    // A borrow rooted in a local, a global, or a sunk parameter has no caller-side root that could
    // keep it alive, which is a different mistake from being rooted in the wrong argument.
    if(summary.invalidRoot) {
        report(analysis,
               "a borrow returned from this function is rooted in storage the caller does not own - it must come from an argument marked `return`"_v,
               source);
    }

    auto undeclared = summary.actualRoots & ~summary.declaredRoots;
    if(!undeclared) return;

    U16 index = 0;
    for(auto argPointer: function.args.contents(analysis.local)) {
        auto arg = analysis.local[argPointer];

        if(undeclared & rootBit(index)) {
            report(analysis,
                   "a borrow returned from this function is rooted in %@, which the signature did not mark `return`"_v,
                   arg->source, analysis.context.findName(arg->name));
        }

        index++;
    }
}

/*
 * Storage-class selection (Implementation-IR.md part 5, Implementation-Regions.md part 4).
 *
 * Cheapest first, which with regions deliberately left out of this milestone is two options: the
 * frame, unless this pass proved the storage has to outlive it. `mayResize` is *not* one of the
 * reasons - an owner whose buffer may be replaced starts on the frame and migrates when it actually
 * grows, which is the whole point of tracking the demand rather than assuming it.
 *
 * Only an allocation has a storage class to choose. A call result or a copy occupies storage the
 * instruction that produced it creates, and if one of those escapes it escapes as a raw pointer,
 * which the language already says nothing about - see the note at the end of this file.
 */
static void selectStorage(Analysis& analysis, OwnershipResult& result) {
    analysis.releasesStorage = emptySet(analysis.localCount);

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];
        if(instruction.kind != Value::Alloc) continue;

        auto& allocation = (InstAlloc&)instruction;
        if(allocation.local >= analysis.localCount) continue;

        auto escapes = analysis.escaped[allocation.local] != 0;
        auto storage = escapes ? StorageClass::Heap : StorageClass::Stack;

        // `@heap` on the binding overrides the analysis in the one direction that is always safe:
        // Design.md's "for a large allocation that's freed well before the region closes".
        auto slot = analysis.function.localAt(analysis.local, allocation.local);
        if(slot.storage == StorageClass::Heap) storage = StorageClass::Heap;

        /*
         * Who releases it.
         *
         * Storage that escaped was handed to something that outlives this frame, and that something
         * owns it now: an array's buffer is on the heap precisely because the array it belongs to
         * left, and the array's own `Drop` is what frees it. Releasing it here as well would free it
         * twice. A `@heap` binding is the other case - it went to the heap because it was asked to,
         * and it still lives and dies in this frame.
         */
        allocation.releasedHere = !escapes;
        allocation.storage = storage;

        // The flag the program reads at run time, where something asked for one.
        if(allocation.storageFlag && analysis.local[allocation.storageFlag]->kind == Value::ConstInt) {
            ((ConstInt*)analysis.local[allocation.storageFlag])->value = storage == StorageClass::Heap;
        }

        // Heap storage this frame owns has to be handed back at the end of the value's life, which
        // is a reason to drop a local whose type has no drop of its own.
        if(storage == StorageClass::Heap && allocation.releasedHere) {
            analysis.releasesStorage[allocation.local] = 1;
            analysis.tracked[allocation.local].droppable = true;
            if(allocation.local < result.locals.size()) result.locals[allocation.local].droppable = true;
        }

        if(storage == StorageClass::Heap && analysis.module.program.allocateHeap) {
            analysis.local[analysis.module.program.allocateHeap]->used = true;
        }

        analysis.function.locals.set(analysis.local, allocation.local,
                                     Local { slot.type, slot.name, slot.value, slot.convention,
                                             storage, slot.borrowed });

        if(allocation.local < result.locals.size()) result.locals[allocation.local].storage = storage;
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
        if(!implementation) return nullptr;

        /*
         * A parametric instance - `instance Drop(Array(a))` - has one implementation written over
         * its own variables, and what runs is the specialization for the types the head matched.
         * The same step emitInstanceCall takes for an ordinary call, taken here because a drop has
         * no call site in the source to have taken it at.
         */
        if((*module.arena)[implementation]->gen) {
            implementation = instantiateFunction(module, implementation, toBuffer(match.args), source);
            if(!implementation) return nullptr;
        }

        (*module.arena)[implementation]->used = true;
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
            /*
             * Each constructor carries a different payload, so the glue reads the discriminant and
             * drops the members of whichever one is present.
             *
             * Built as a chain of tests rather than as a jump table, because that is what the IR
             * has: `je` is its only conditional, and a record with a dozen constructors is not the
             * case worth a second control-flow construct for. A constructor whose payload has
             * nothing to release is skipped entirely, so the chain is as long as the number of
             * constructors that own something rather than the number that exist.
             */
            auto exit = resolver.addBlock();

            for(auto constructor: record->constructors.contents(global)) {
                auto content = constructor.content;
                if(!content || !needsDrop(module, content)) continue;

                auto discriminant = resolver.load(
                    resolver.project(base, ProjectionKind::Discriminant, 0), source);

                auto index = resolver.makeInt(source, module.scalar.int_, constructor.index);
                auto matches = resolver.emit<InstCmp>(source, 0, module.scalar.bool_,
                                                      discriminant, index, CompareOp::Eq);

                auto drops = resolver.addBlock();
                auto next = resolver.addBlock();
                resolver.terminate(resolver.emit<InstJe>(source, 0, module.scalar.unit,
                                                         resolver.ref(matches), drops, next));

                resolver.current = drops;
                dropMembers(resolver, module, resolver.project(base, ProjectionKind::Downcast,
                                                               U16(constructor.index)), content, source);
                resolver.terminate(resolver.emit<InstJmp>(source, 0, module.scalar.unit, exit));

                resolver.current = next;
            }

            resolver.terminate(resolver.emit<InstJmp>(source, 0, module.scalar.unit, exit));
            resolver.current = exit;
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
                        report(analysis, "this assignment overwrites a value that was moved out of on only some paths - conditional drops need drop flags, which are not implemented yet"_v,
                               analysis.local[analysis.order[i]]->source);
                    } else if(before == OwnState::Owned) {
                        blockDrops[b].push(PendingDrop { U32(l), U32(i) });
                    }
                }

                auto ownedAfter = defines || (before == OwnState::Owned && !moves);
                auto maybeAfter = !defines && !moves && before == OwnState::Maybe;

                if((liveBefore || defines) && !liveAfter && (ownedAfter || maybeAfter)) {
                    if(maybeAfter) {
                        report(analysis, "this value was moved out of on only some paths reaching its last use - conditional drops need drop flags, which are not implemented yet"_v,
                               analysis.local[analysis.order[i]]->source);
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
                    report(analysis, "this value is owned on only some paths reaching this branch - conditional drops need drop flags, which are not implemented yet"_v,
                           analysis.local[block->terminator]->source);
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

    // Heap storage this frame owns has to be handed back whether or not the type it holds has
    // anything of its own to run - which is the whole difference between the two halves of a
    // derived drop, "drop each member" and "release this owner's own storage".
    auto releases = localIndex < analysis.releasesStorage.size() && analysis.releasesStorage[localIndex];
    if(!implementation && !releases) return nullptr;

    auto drop = createInst<InstDrop>(analysis.module, analysis.function, block, source, 0,
                                     analysis.module.scalar.unit, Place::inLocal(localIndex),
                                     ownershipOf(analysis.module, slot.type).drop);

    drop->implementation = implementation;
    drop->releaseStorage = releases;

    if(releases && analysis.module.program.freeHeap) {
        analysis.local[analysis.module.program.freeHeap]->used = true;
    }

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

/*
 * One function, once.
 *
 * `reporting` is what separates the fixpoint's silent rounds from the one round whose diagnostics
 * are the program's, and `rewrite` whether this run is allowed to insert drops and choose storage.
 * Everything before those two switches is the same work either way: the facts do not depend on
 * which round computed them, only on the summaries that were available when they did.
 */
static bool analyzeFunction(Module& module, Function& function, OwnershipResult& result,
                            bool reporting, bool rewrite, bool* summaryChanged) {
    Analysis analysis(module, function);
    analysis.localCount = function.localCount();
    analysis.reporting = reporting;

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

    // The interprocedural half, in the order each part needs the one before it: which storage every
    // value refers to, what has to outlive the frame, what each root's representation must do, and
    // finally what all of that says to a caller.
    computeProvenance(analysis);
    computeOutliving(analysis);
    computeDemand(analysis);

    auto changed = deriveSummary(analysis);
    if(summaryChanged) *summaryChanged = changed;

    // A silent round exists to move the summary and nothing else. Checking here as well would be
    // harmless but wasted, and rewriting would apply a decision the fixpoint has not settled yet.
    if(!rewrite) return true;

    checkMoves(analysis);
    checkBorrows(analysis);
    checkReturnRoots(analysis);

    result.locals = analysis.tracked;
    for(Size l = 0; l < analysis.localCount; l++) {
        result.locals[l].requirements = analysis.demand[l];
        result.locals[l].escapes = analysis.escaped[l] != 0;
    }

    selectStorage(analysis, result);
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

bool runOwnership(Module& module, Function& function, OwnershipResult& result) {
    return analyzeFunction(module, function, result, true, true, nullptr);
}

// Which functions the passes run over. A signature has no body, an intrinsic is generated at each
// call site rather than being one function, and a generic body is checked but never given drops -
// what reaches the backend is its specializations, and those are ordinary functions that get their
// own drops here. Checking the generic body anyway is what puts a use-after-move diagnostic on the
// function that has the bug instead of once per instantiation.
static bool ownershipApplies(Function& function) {
    return !function.signature && !function.intrinsic && function.blocks.isNotEmpty();
}

/*
 * The whole program, in two phases.
 *
 * A summary is a statement about a function that its callers read, so a caller cannot be analyzed
 * before its callees - and with recursion there is no order in which that is true. The answer is
 * the ordinary one: run the analysis silently until no summary moves, then run it once more for
 * real. Every fact involved is a "may" fact climbing from empty, so the silent rounds are
 * optimistic and each round only adds; the last round therefore sees every summary at its final
 * value, and what it reports is what the program means.
 *
 * The cost is that the intraprocedural work is repeated per round. That is deliberate over keeping
 * every function's Analysis alive at once: the rounds are bounded by the depth of the call graph,
 * and a compiler that holds the liveness of every function of a program in memory to save a few
 * of them is trading the wrong resource.
 */
static Size summaryRound(Program& program, bool& changed) {
    auto base = *program.arena;
    Size analyzed = 0;

    for(auto module: program.modules) {
        for(Size i = 0; i < module->functionOrder.size(); i++) {
            auto function = base[module->functionOrder.get(base, i)];
            if(!ownershipApplies(*function)) continue;

            OwnershipResult discarded;
            auto moved = false;
            analyzeFunction(*module, *function, discarded, false, false, &moved);

            changed = changed || moved;
            analyzed++;
        }
    }

    return analyzed;
}

bool runProgramOwnership(Program& program) {
    auto base = *program.arena;
    auto success = true;

    if(!program.ownership) program.ownership = Ptr<OwnershipResults>(new OwnershipResults());

    // A signature has no body to summarize, so it says nothing rather than saying the optimistic
    // thing: a class method's implementation is chosen per instance, and a caller that assumed one
    // did not mutate its argument would be assuming it of every instance there will ever be.
    for(auto module: program.modules) {
        for(auto pointer: module->functionOrder.contents(base)) {
            auto function = base[pointer];
            if(ownershipApplies(*function)) continue;

            function->summary.opaque = true;
            function->summary.ready = true;
        }
    }

    /*
     * Rounds until nothing moves.
     *
     * The first round sees callees that have not been visited yet and reads the conservative answer
     * for them; every round after that sees real summaries, so what the iteration is doing is
     * relaxing an over-approximation rather than climbing a lattice. That is why the bound is a
     * count of functions rather than an argument about monotonicity: it settles in a handful of
     * rounds for any call graph a program has, and the cap is what stops a rule added later from
     * turning a failure to settle into a hang.
     */
    auto changed = true;
    for(Size round = 0; changed; round++) {
        changed = false;
        auto count = summaryRound(program, changed);
        if(round > count + 1) break;
    }

    for(auto module: program.modules) {
        // Specializations and drop glue are appended while this runs, so the list is walked by
        // index - the same reason resolveModuleBodies does.
        for(Size i = 0; i < module->functionOrder.size(); i++) {
            auto pointer = module->functionOrder.get(base, i);
            auto function = base[pointer];
            if(!ownershipApplies(*function)) continue;

            OwnershipResult result;
            auto ok = analyzeFunction(*module, *function, result, true, true, nullptr);
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

// The three demand bits, in one place so that a local and an argument print alike.
static void printRequirements(Net::Writer& writer, const ReprRequirements& requirements) {
    switch(requirements.mutation) {
        case MutationDemand::ReadOnly: writer.writeString(" readonly"_v); break;
        case MutationDemand::Writable: writer.writeString(" writable"_v); break;
        case MutationDemand::Unknown: writer.writeString(" unknown"_v); break;
    }

    if(requirements.needsStableAddress) writer.writeString(" addressed"_v);
    if(requirements.mayResize) writer.writeString(" resizable"_v);
}

/*
 * The summary, printed first because it is what the rest of the program was analyzed against.
 *
 * A caller's diagnostics and a caller's storage decisions both follow from these lines, so a fixture
 * that asserts them is asserting the interface every call site was checked against rather than one
 * body's internals.
 */
static void printSummary(Net::Writer& writer, Context& context, ModuleBase base, Function& function) {
    auto& summary = function.summary;
    U16 index = 0;

    for(auto argPointer: function.args.contents(base)) {
        auto arg = base[argPointer];
        writer.writeString("  arg "_v);

        if(arg->name) writer.writeString(context.findName(arg->name));
        else writeIndex(writer, index);

        if(index < summary.args.size()) {
            auto entry = summary.args.get(base, index);
            printRequirements(writer, entry.requirements);

            if(entry.returnRoot) writer.writeString(" return"_v);
            if(entry.retained) writer.writeString(" retained"_v);
        }

        writer.writeByte('\n');
        index++;
    }

    switch(summary.resultBound) {
        case StorageBound::Frame: break;
        case StorageBound::Arguments: writer.writeString("  result arguments\n"_v); break;
        case StorageBound::Region: writer.writeString("  result region\n"_v); break;
        case StorageBound::Escapes: writer.writeString("  result escapes\n"_v); break;
    }
}

static void printFunctionOwnership(Net::Writer& writer, Context& context, Program& program,
                                   Function& function, OwnershipResult& result) {
    writer.writeString("fn "_v);
    writer.writeString(context.findName(function.name));
    writer.writeString(" {\n"_v);

    printSummary(writer, context, *program.arena, function);

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
        printRequirements(writer, tracked.requirements);

        // Only the allocations have a storage class to report, and only the non-default one is
        // worth a word: everything is frame-placed unless something proved it could not be.
        if(tracked.escapes) writer.writeString(" escapes"_v);
        if(tracked.storage == StorageClass::Heap) writer.writeString(" heap"_v);

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
 * than it had to, or gives a value more storage than it needed. Nothing here can make it accept a
 * program it should reject, which is the property worth preserving while the rest is filled in.
 *
 * **Drop flags.** A value moved out of on only some paths reaching its last use needs a runtime bit
 * saying whether the slot still owns anything. The bit, the block split around the conditional
 * drop, and InstDrop::flag are all designed for; what is here reports instead of emitting them.
 * This is the largest single item and the one an ordinary program hits first - `if c: consume(x)`
 * is enough.
 *
 * **Partial moves.** Moving one field out of an aggregate leaves the slot half-owned. checkMoves()
 * rejects it, because representing it means a drop flag per field and a drop that runs over a
 * subset of members - the same machinery drop flags need, one level further in.
 *
 * **Two-phase borrows.** `f(&x, g(x))` evaluates `g(x)` while the borrow of `x` for the first
 * argument is already live, which is rejected here and accepted by Rust through a reservation
 * phase. The resolver happens to evaluate arguments before creating the borrow, so the common
 * shapes do not hit it, but the rule is not stated anywhere and should be.
 *
 * **Per-field granularity for liveness, ownership and demand.** All three are tracked per local, so
 * borrowing `x.a` keeps all of `x` alive and writing `x.a` makes all of `x` writable. Conflict
 * *detection* is per place and does distinguish `x.a` from `x.b`; it is only the extent and the
 * demand that are coarse. Containment in the provenance analysis is field-insensitive for the same
 * reason and with the same effect.
 *
 * **Demand does not follow a move.** Design.md says an ownership root keeps its demand across a
 * move, and here a `->` binding starts a new local with a demand of its own. What that costs is
 * precision in one direction only - a value moved and then mutated leaves its source classified
 * read-only, and the source's storage was already dead by then.
 *
 * **Places rooted in a raw pointer are not checked against each other.** placesOverlap() answers no
 * for two of them, so `*p` and `*q` never conflict however they were derived. That is what `%T`
 * means and what makes Native's `borrow` the deliberate seam it is - a collection written over raw
 * storage is trusted about aliasing inside itself, and owes its callers a `return` marker that
 * makes the outside checkable.
 *
 * **Regions.** The storage decision is between the frame and the heap; StorageClass::Region is
 * reserved and never selected. Implementation-Regions.md part 4 is the third case in this
 * decision rather than a new pass, which is why it was left out rather than approximated.
 *
 * **Repr variants beyond "in memory or not".** There is no packing and no niche yet, so the only
 * two representations that differ are storage and no storage - which is what resolve/lower.cpp's
 * scalarization spends the demand result on. A read-only variant that differs in *layout* needs
 * Implementation-Repr.md's work first, and a materialize/thaw conversion at the boundaries where
 * an unspecialized ABI requires the canonical one.
 *
 * **Interprocedural summaries are recomputed per round.** The fixpoint re-runs the whole
 * intraprocedural analysis for every function on every round rather than keeping each function's
 * facts alive. The rounds are bounded by the depth of the call graph, and the alternative trades
 * the wrong resource - see runProgramOwnership.
 *
 * **The checked reference rungs.** `Ref` and `RegionPtr` classify conservatively in ownershipOf()
 * and are not constructible yet, so nothing exercises them.
 * ---------------------------------------------------------------------------------------------
 */
