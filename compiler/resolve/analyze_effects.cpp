#include "analyze_pass.h"

/*
 * The numbering, and what each instruction does to the locals it touches.
 *
 * Nothing here is a decision. It is the spine every other pass indexes by and the per-instruction
 * summary all of them read, which is why it is one file: an instruction added to the IR is added to
 * exactly one switch, and a pass that would have needed a case of its own is a pass that is asking
 * a question this file does not answer.
 */

// A set with a slot per local, all clear. Every pass allocates one of these, which is why it lives
// with the numbering rather than with whichever pass happened to want it first.
LocalSet emptySet(Size count) {
    LocalSet set;
    for(Size i = 0; i < count; i++) set.push(0);
    return set;
}

void numberFunction(Analysis& analysis) {
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
U32 rootLocal(Analysis& analysis, const Place& place) {
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
U32 backingLocal(Analysis& analysis, ModulePtr<Value> value) {
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
    if(needsTeardown(analysis.module, type)) effects.moves.push(root);
}

static void deriveEffects(Analysis& analysis) {
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
                        if(instruction.kind == Value::Assign) effects.overwrites.push(root);
                    } else {
                        // A field write leaves the rest of the slot alone, so it reads as a use.
                        // That covers the liveness half of a field *assignment* as well - what such
                        // a write replaces is one field, and the drop for it is stated over that
                        // field's place rather than over the slot. See placeOverwriteDrops.
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

            /*
             * Both places are read and both are written, and the state of neither changes. So both
             * are uses and neither is a def, a move or an init - the whole of what makes these two
             * the operations that need no lattice.
             *
             * A use is not nothing, though: it is what keeps the old contents live up to here, which
             * is what stops the last-use rule dropping a value that this instruction is about to
             * hand somewhere else. It is also what makes reading a moved-out slot through one of
             * these the use-after-move it is.
             */
            case Value::Swap: {
                auto& swap = (InstSwap&)instruction;
                useRoot(analysis, effects, swap.a);
                useRoot(analysis, effects, swap.b);
                break;
            }

            case Value::Exchange: {
                auto& exchange = (InstExchange&)instruction;
                useRoot(analysis, effects, exchange.place);

                // The incoming value goes into the place, so whatever owed a drop for it is now the
                // place's business - the same handover an Init of the same value would be.
                transferFrom(analysis, effects, exchange.value);
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

            case Value::CallDyn: {
                // The callee's code and environment are read out of the function value, which is
                // what keeps the value itself alive across the call it is used by.
                auto& call = (InstCallDyn&)instruction;
                useValue(analysis, effects, call.callable);
                useValue(analysis, effects, call.address);

                for(auto arg: call.args.contents(local)) useValue(analysis, effects, arg);
                break;
            }

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

        if(!borrows || !firstPlace(instruction, place)) continue;

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
 * The three together, which is the only order they are ever wanted in: what each instruction does,
 * then the two corrections that make the answer the one the CFG has rather than the one the operand
 * list has.
 */
void computeEffects(Analysis& analysis) {
    deriveEffects(analysis);
    extendBorrowUses(analysis);
    attributePhiEdges(analysis);
}
