#include "edit.h"
#include "builder.h"

IrEditor::IrEditor(Module& module, Function& function, bool* changed):
    module(module), function(function), base(*module.arena), changed(changed) {}

void IrEditor::recordUse(ModulePtr<Value> value, ModulePtr<Inst> user) {
    if(!value) return;
    base[value]->useList.push(module.arena, user);
}

void IrEditor::addUse(ModulePtr<Value> value, Inst* user) {
    recordUse(value, user - base);
}

// A place is used through whatever it is rooted in, so that the value the storage came from - an
// alloc, an argument, or the pointer an address was computed into - sees every read and write of
// any part of it. A global has no value to attribute the use to; its uses are recorded on the
// global itself when the instruction is built.
void IrEditor::addPlaceUse(const Place& place, Inst* user) {
    if(place.root == PlaceRoot::Pointer || place.root == PlaceRoot::Borrow) {
        addUse(place.pointer, user);
    } else if(place.root == PlaceRoot::Local) {
        auto owner = base[user->block] ? base[base[user->block]->function] : nullptr;

        if(owner && place.local < owner->localCount()) {
            addUse(owner->localAt(base, place.local).value, user);
        }
    }

    // An index projection is an ordinary operand of the access it appears in.
    auto projections = place.projections;
    for(auto projection: projections.contents(base)) {
        if(projection.value) addUse(projection.value, user);
    }
}

/*
 * Recording one instruction as a user of everything it names.
 *
 * Written as a switch rather than as a walk of `eachOperand`, and that is deliberate: the two are
 * independent statements of the same list, and `verifyFunction` compares them. A recording path
 * built out of the walking path would make that check compare a list with itself.
 *
 * Every kind is here, phis and terminators included. They used to be handled inside `Block::add`
 * instead, on the grounds that both are *block* structure - a phi joins the incoming edges and a
 * terminator creates them - which is true of the edges and was never true of the operands: a phi's
 * alternatives and a branch's condition are reads like any other, and `setTerminator` needs to
 * record them for an instruction that is not being appended to anything.
 */
void IrEditor::recordUses(Inst* inst) {
    // The storage half, which is the same list for every pass that walks places - see
    // instructionPlaces. What is left below is the operands, which are per instruction.
    eachPlace(*inst, [&](const Place& place) { addPlaceUse(place, inst); });

    switch(inst->kind) {
        case Value::Phi:
            for(auto input: ((InstPhi*)inst)->inputs.contents(base)) addUse(input.value, inst);
            break;
        case Value::Je:
            addUse(((InstJe*)inst)->cond, inst);
            break;
        case Value::Ret:
            addUse(((InstRet*)inst)->value, inst);
            break;
        // A run's length is an operand of the allocation - see InstAlloc::extent. Null for the
        // allocation of one object, which is every other one.
        case Value::Alloc:
            addUse(((InstAlloc*)inst)->extent, inst);
            break;
        case Value::Init:
        case Value::Assign:
            addUse(((InstInit*)inst)->value, inst);
            break;
        case Value::Exchange:
            addUse(((InstExchange*)inst)->value, inst);
            break;
        case Value::Native:
            for(auto arg: ((InstNative*)inst)->args.contents(base)) addUse(arg, inst);
            break;
        case Value::Aggregate: {
            auto aggregate = (InstAggregate*)inst;
            eachAggregateComponent(base, *aggregate,
                                   [&](const AggregateComponent& component, Size) {
                addUse(component.value, inst);
                addUse(component.step.value, inst);
            });
            break;
        }
        case Value::Cast:
        case Value::Neg:
        case Value::Not:
            addUse(((InstUnary*)inst)->from, inst);
            break;
        // The table the slot is read out of. A TypeMetric has no operand at all - it names a type -
        // which is why the two are not one case despite being the same kind of question.
        case Value::TableSlot:
            addUse(((InstTableSlot*)inst)->table, inst);
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
        case Value::Cmp: {
            auto binary = (InstBinary*)inst;
            addUse(binary->lhs, inst);
            addUse(binary->rhs, inst);
            break;
        }
        case Value::Select: {
            auto select = (InstSelect*)inst;
            addUse(select->cond, inst);
            addUse(select->whenTrue, inst);
            addUse(select->whenFalse, inst);
            break;
        }
        case Value::Call:
            for(auto arg: ((InstCall*)inst)->args.contents(base)) addUse(arg, inst);
            break;
        case Value::CallDyn: {
            auto call = (InstCallDyn*)inst;
            addUse(call->callable, inst);
            addUse(call->address, inst);
            for(auto arg: call->args.contents(base)) addUse(arg, inst);
            break;
        }
        case Value::GenCall:
            for(auto arg: ((InstGenCall*)inst)->args.contents(base)) addUse(arg, inst);
            break;
        default:
            break;
    }
}

void IrEditor::dropUse(ModulePtr<Value> value, ModulePtr<Inst> user) {
    if(!value) return;

    auto& uses = base[value]->useList;
    for(Size i = 0; i < uses.size(); i++) {
        if(uses.get(base, i) == user) {
            uses.remove(base, i);
            return;
        }
    }
}

void IrEditor::dropUses(ModulePtr<Inst> instruction) {
    auto& value = *base[instruction];

    eachOperand(base, value, [&](ModulePtr<Value> operand) { dropUse(operand, instruction); });

    // The storage a place is rooted in, which `eachOperand` deliberately does not yield - see
    // eachPlaceRootValue. Missing it leaves the Alloc believing in a reader that is no longer in any
    // block, which is invisible until a pass asks the Alloc who reads it.
    eachPlaceRootValue(base, function, value, [&](ModulePtr<Value> storage) {
        dropUse(storage, instruction);
    });
}

void IrEditor::replaceValue(ModulePtr<Value> from, ModulePtr<Value> to) {
    if(from == to) return;

    // Null would mean pointing a reader at nothing, which every caller is supposed to have already
    // decided against. Asserted rather than tolerated because the result is otherwise a use of
    // whatever sits at offset zero of the arena.
    assertTrue(to != nullptr);

    auto& uses = base[from]->useList;
    while(uses.size()) {
        auto userPointer = uses.get(base, uses.size() - 1);
        uses.remove(base, uses.size() - 1);

        // Every matching operand at once, and one use entry per visit: an instruction reading the
        // value twice is in the list twice, so the two counts stay equal either way round.
        mapOperands(base, *base[userPointer], [&](ModulePtr<Value> operand) {
            return operand == from ? to : operand;
        });

        base[to]->useList.push(module.arena, userPointer);
    }

    markChanged();
}

void IrEditor::forgetLocalValue(ModulePtr<Value> value) {
    for(U32 local = 0; local < function.localCount(); local++) {
        if(function.localAt(base, local).value != value) continue;

        function.setLocalValue(base, local, nullptr);
    }
}

void IrEditor::setLocalValue(U32 index, ModulePtr<Value> value) {
    function.setLocalValue(base, index, value);
}

void IrEditor::repointLocalValue(ModulePtr<Value> from, ModulePtr<Value> to) {
    for(U32 local = U32(function.localCount()); local-- > 0;) {
        if(function.localAt(base, local).value != from) continue;

        function.setLocalValue(base, local, to);
    }
}

// The arms this terminator names, by ordinal - see instructionSuccessorSlots, which is where the
// list they come from is declared. Every slot is filled, so a caller may copy the whole array into
// `outgoingBlocks` without asking how many there were.
Size IrEditor::successorsOf(const Value& terminator, ModulePtr<Block>* target) {
    ModulePtr<Block>* slots[kMaxSuccessors];
    auto count = instructionSuccessorSlots(const_cast<Value&>(terminator), slots);

    for(Size i = 0; i < kMaxSuccessors; i++) target[i] = i < count ? *slots[i] : nullptr;
    return count;
}

void IrEditor::recordEdges(Inst* terminator, ModulePtr<Block> from) {
    ModulePtr<Block> successors[kMaxSuccessors];
    auto count = successorsOf(*terminator, successors);
    auto block = base[from];

    for(Size i = 0; i < kMaxSuccessors; i++) block->outgoingBlocks[i] = successors[i];

    for(Size i = 0; i < count; i++) {
        if(successors[i]) base[successors[i]]->incomingList.push(module.arena, from);
    }
}

Inst* IrEditor::append(Block& block, Inst* inst) {
    auto pointer = (ModulePtr<Inst>)(inst - base);
    auto blockPointer = (ModulePtr<Block>)(&block - base);

    assertTrue(inst->block == blockPointer);

    if(inst->kind == Value::Phi) {
        block.phiList.push(module.arena, (InstPhi*)inst - base);
    } else if(isTerminator(*inst)) {
        assertTrue(block.terminatorInst == nullptr);
        block.terminatorInst = pointer;
        recordEdges(inst, blockPointer);
    } else {
        block.instructionList.push(module.arena, pointer);
    }

    recordUses(inst);
    return inst;
}

void IrEditor::reorder(Block& block, Buffer<ModulePtr<Inst>> order) {
    /*
     * A permutation, which is the whole of what this promises - and the reason it is the one
     * structural edit with no bookkeeping attached to it. Equal length alone does not say so: a
     * caller that lists one instruction twice and omits another satisfies it, and what that leaves
     * is an instruction in no block whose uses are all still recorded.
     *
     * So the count of each entry is compared on both sides. Quadratic, and confined to assertion
     * builds for that reason: a block of a hundred instructions is ten thousand comparisons against
     * a rewrite that is otherwise two pointer walks. The corpus's blocks are nothing like that
     * large, but the bound is on the program rather than on anything here.
     */
#if defined(_DEBUG) || defined(DEBUG)
    assertTrue(order.size() == block.instructionCount());

    for(auto instruction: order) {
        Size listed = 0;
        for(auto other: order) listed += other == instruction;

        Size present = 0;
        for(Size i = 0; i < block.instructionCount(); i++) {
            present += block.instructionAt(base, i) == instruction;
        }

        assertTrue(listed == present);
    }
#endif

    block.instructionList.clear();
    for(auto instruction: order) block.instructionList.push(module.arena, instruction);
}

void IrEditor::insert(Block& block, Size index, InstList& instructions) {
    /*
     * Appended and then reordered, rather than written into the list at the position: appending is
     * what records every use, and a list a pass filled in by hand would be one more place for the
     * two directions of the IR to disagree.
     */
    auto existing = block.instructionCount();
    for(auto instruction: instructions) append(block, instruction);

    SmallArray<ModulePtr<Inst>, 48> ordered;
    for(Size i = 0; i < existing; i++) {
        if(i == index) {
            for(auto j = existing; j < block.instructionCount(); j++) {
                ordered.push(block.instructionAt(base, j));
            }
        }

        ordered.push(block.instructionAt(base, i));
    }

    // An index at the end of the list, which nothing above would have reached.
    if(index >= existing) {
        for(auto j = existing; j < block.instructionCount(); j++) {
            ordered.push(block.instructionAt(base, j));
        }
    }

    reorder(block, Buffer<ModulePtr<Inst>>(ordered.pointer(), ordered.size()));
    markChanged();
}

void IrEditor::removeInstruction(ModulePtr<Inst> instruction) {
    auto value = base[instruction];

    dropUses(instruction);

    auto block = base[value->block];
    for(Size i = 0; i < block->instructionCount(); i++) {
        if(block->instructionAt(base, i) == instruction) {
            block->instructionList.remove(base, i);
            break;
        }
    }

    // And the slots this value was the whole contents of, which stop existing with it. A caller that
    // means the storage to survive in another value says so with `repointLocalValue` first - by the
    // time this returns there is nothing left to point at.
    forgetLocalValue((ModulePtr<Value>)instruction);
}

void IrEditor::eraseInstruction(ModulePtr<Inst> instruction) {
    assertTrue(base[instruction]->useCount() == 0);

    removeInstruction(instruction);
    markChanged();
}

void IrEditor::erasePhi(ModulePtr<InstPhi> pointer) {
    auto phi = base[pointer];
    auto instruction = (ModulePtr<Inst>)pointer;

    dropUses(instruction);

    auto block = base[phi->block];
    for(Size i = 0; i < block->phiCount(); i++) {
        if(block->phiAt(base, i) != pointer) continue;

        block->phiList.remove(base, i);
        break;
    }

    forgetLocalValue((ModulePtr<Value>)pointer);
}

void IrEditor::moveInstruction(ModulePtr<Inst> instruction, Block& target) {
    auto value = base[instruction];
    auto source = base[value->block];
    auto targetPointer = (ModulePtr<Block>)(&target - base);

    for(Size i = 0; i < source->instructionCount(); i++) {
        if(source->instructionAt(base, i) != instruction) continue;

        source->instructionList.remove(base, i);
        break;
    }

    target.instructionList.push(module.arena, instruction);
    value->block = targetPointer;

    markChanged();
}

void IrEditor::moveInstructions(Block& source, Block& target) {
    auto targetPointer = (ModulePtr<Block>)(&target - base);

    for(auto instruction: source.instructions(base)) {
        base[instruction]->block = targetPointer;
        target.instructionList.push(module.arena, instruction);
    }

    source.instructionList.clear();
}

void IrEditor::addPhiInput(ModulePtr<InstPhi> pointer, PhiInput input) {
    base[pointer]->inputs.push(module.arena, input);
    recordUse(input.value, (ModulePtr<Inst>)pointer);
}

void IrEditor::removePhiInput(ModulePtr<InstPhi> pointer, Size index) {
    auto phi = base[pointer];

    dropUse(phi->inputs.get(base, index).value, (ModulePtr<Inst>)pointer);
    phi->inputs.remove(base, index);
}

void IrEditor::removeEdge(ModulePtr<Block> into, ModulePtr<Block> from) {
    auto block = base[into];

    for(Size i = 0; i < block->predecessorCount(); i++) {
        if(block->predecessorAt(base, i) != from) continue;

        block->incomingList.remove(base, i);
        break;
    }

    for(auto phiPointer: block->phis(base)) {
        auto phi = base[phiPointer];

        for(Size i = 0; i < phi->inputs.size(); i++) {
            if(phi->inputs.get(base, i).block != from) continue;

            removePhiInput(phiPointer, i);
            break;
        }
    }
}

void IrEditor::retargetEdge(Block& target, ModulePtr<Block> from, ModulePtr<Block> to) {
    for(Size i = 0; i < target.predecessorCount(); i++) {
        if(target.predecessorAt(base, i) != from) continue;

        target.incomingList.set(base, i, to);
    }

    for(auto phiPointer: target.phis(base)) {
        auto phi = base[phiPointer];

        for(Size i = 0; i < phi->inputs.size(); i++) {
            auto input = phi->inputs.get(base, i);
            if(input.block != from) continue;

            input.block = to;
            phi->inputs.set(base, i, input);
        }
    }
}

void IrEditor::retargetEdgeOnce(Block& target, ModulePtr<Block> from, ModulePtr<Block> to) {
    for(Size i = 0; i < target.predecessorCount(); i++) {
        if(target.predecessorAt(base, i) != from) continue;

        target.incomingList.set(base, i, to);
        break;
    }

    for(auto phiPointer: target.phis(base)) {
        auto phi = base[phiPointer];

        for(Size i = 0; i < phi->inputs.size(); i++) {
            auto input = phi->inputs.get(base, i);
            if(input.block != from) continue;

            input.block = to;
            phi->inputs.set(base, i, input);
            break;
        }
    }
}

/*
 * A terminator handed from one block to another, edges and all.
 *
 * Shared by the two operations that move one: a split gives it to the continuation, a merge gives it
 * to the block doing the absorbing. Both used to say this themselves, in eleven lines each that had
 * to stay identical - and the half worth sharing is the last one, where every successor is told the
 * edges now leave from somewhere else.
 */
void IrEditor::transferTerminator(Block& from, Block& to) {
    auto fromPointer = (ModulePtr<Block>)(&from - base);
    auto toPointer = (ModulePtr<Block>)(&to - base);

    to.terminatorInst = from.terminatorInst;
    if(from.terminatorInst) base[from.terminatorInst]->block = toPointer;

    for(Size i = 0; i < kMaxSuccessors; i++) {
        to.outgoingBlocks[i] = from.outgoingBlocks[i];
        from.outgoingBlocks[i] = nullptr;
    }

    from.terminatorInst = nullptr;

    // Every match rather than one per arm, which is right here and wrong in `splitEdge`: what has
    // changed is the block the edges leave from, so both of a doubled arm's records move together.
    for(auto successor: to.outgoingBlocks) {
        if(!successor) continue;

        retargetEdge(*base[successor], fromPointer, toPointer);
    }
}

void IrEditor::setTerminator(Block& block, Inst* inst) {
    assertTrue(isTerminator(*inst));

    auto blockPointer = (ModulePtr<Block>)(&block - base);

    ModulePtr<Block> before[kMaxSuccessors];
    ModulePtr<Block> after[kMaxSuccessors];

    for(Size i = 0; i < kMaxSuccessors; i++) before[i] = block.outgoingBlocks[i];
    successorsOf(*inst, after);

    if(block.terminatorInst) dropUses(block.terminatorInst);

    /*
     * The multiset difference - see the header. An edge that is in both sets keeps everything that
     * was recorded about it, which is what makes a fold of `je %c, then, else` to `jmp then` leave
     * `then`'s phi alternatives alone.
     */
    bool kept[kMaxSuccessors] = {};
    bool fresh[kMaxSuccessors];
    for(Size i = 0; i < kMaxSuccessors; i++) fresh[i] = true;

    for(Size i = 0; i < kMaxSuccessors; i++) {
        if(!before[i]) continue;

        for(Size j = 0; j < kMaxSuccessors; j++) {
            if(!after[j] || !fresh[j] || after[j] != before[i]) continue;

            kept[i] = true;
            fresh[j] = false;
            break;
        }
    }

    for(Size i = 0; i < kMaxSuccessors; i++) {
        if(before[i] && !kept[i]) removeEdge(before[i], blockPointer);
    }

    block.terminatorInst = (ModulePtr<Inst>)(inst - base);
    for(Size i = 0; i < kMaxSuccessors; i++) block.outgoingBlocks[i] = after[i];
    inst->block = blockPointer;

    for(Size j = 0; j < kMaxSuccessors; j++) {
        if(after[j] && fresh[j]) base[after[j]]->incomingList.push(module.arena, blockPointer);
    }

    recordUses(inst);
    markChanged();
}

void IrEditor::clearTerminator(Block& block) {
    auto blockPointer = (ModulePtr<Block>)(&block - base);

    if(block.terminatorInst) dropUses(block.terminatorInst);

    for(Size i = 0; i < kMaxSuccessors; i++) {
        if(block.outgoingBlocks[i]) removeEdge(block.outgoingBlocks[i], blockPointer);
        block.outgoingBlocks[i] = nullptr;
    }

    block.terminatorInst = nullptr;
}

Size IrEditor::redirectSuccessor(Block& from, ModulePtr<Block> oldTarget, ModulePtr<Block> newTarget) {
    if(!from.terminatorInst) return 0;

    auto fromPointer = (ModulePtr<Block>)(&from - base);
    auto terminator = base[from.terminatorInst];
    Size redirected = 0;

    // Per arm rather than per distinct successor: with `je %c, X, X` both arms lead to `X` and both
    // are edges, so both move and the count says two.
    ModulePtr<Block>* slots[kMaxSuccessors];
    auto count = instructionSuccessorSlots(*terminator, slots);

    for(Size i = 0; i < count; i++) {
        if(*slots[i] != oldTarget) continue;

        *slots[i] = newTarget;
        redirected++;
    }

    if(!redirected) return 0;

    for(auto& outgoing: from.outgoingBlocks) {
        if(outgoing == oldTarget) outgoing = newTarget;
    }

    // One edge record moved per arm, since two arms at one block are two edges.
    for(Size i = 0; i < redirected; i++) {
        removeEdge(oldTarget, fromPointer);
        base[newTarget]->incomingList.push(module.arena, fromPointer);
    }

    return redirected;
}

void IrEditor::clearPredecessors(Block& block) {
    // By removal rather than by a clear, because an embedded list has no such operation - see
    // SmallList, where an entry is present exactly when its high bit is set.
    while(block.incomingList.size()) block.incomingList.remove(base, block.incomingList.size() - 1);
}

Block* IrEditor::splitBlock(Block& block, Size index) {
    auto pointer = (ModulePtr<Block>)(&block - base);

    auto continuation = function.addBlock(module, block.name);
    auto continuationPointer = (ModulePtr<Block>)(continuation - base);
    continuation->source = block.source;

    // Inline: the tail of one block, held only for the length of this call.
    SmallArray<ModulePtr<Inst>, 16> moved;
    for(Size i = index + 1; i < block.instructionCount(); i++) {
        moved.push(block.instructionAt(base, i));
    }

    for(Size i = block.instructionCount(); i-- > index + 1;) {
        block.instructionList.remove(base, i);
    }

    for(auto instruction: moved) {
        base[instruction]->block = continuationPointer;
        continuation->instructionList.push(module.arena, instruction);
    }

    transferTerminator(block, *continuation);
    return continuation;
}

Block* IrEditor::splitEdge(Block& from, Size successor) {
    // Before the indexing rather than after it. A block has exactly kMaxSuccessors slots, so an
    // ordinal from anywhere else reads past them - and an out-of-bounds read of an arena is a
    // plausible-looking block pointer rather than a crash.
    assertTrue(successor < kMaxSuccessors);

    auto fromPointer = (ModulePtr<Block>)(&from - base);
    auto to = from.outgoingBlocks[successor];
    assertTrue(to != nullptr);

    auto split = function.addBlock(module);
    auto splitPointer = (ModulePtr<Block>)(split - base);
    split->index = U16(function.blocks.size() - 1);
    split->source = from.terminatorInst ? base[from.terminatorInst]->source : from.source;

    // The arm named, and only that one: with `je %c, X, X` the other still leads to `X` and has a
    // record of its own to keep. Which is what the ordinal buys - the slot is the arm.
    if(auto terminator = from.terminatorInst ? base[from.terminatorInst] : nullptr) {
        ModulePtr<Block>* slots[kMaxSuccessors];
        if(successor < instructionSuccessorSlots(*terminator, slots)) *slots[successor] = splitPointer;
    }

    from.outgoingBlocks[successor] = splitPointer;

    /*
     * One record in the successor, repointed - a *retarget* rather than a removal and an addition,
     * because the phi alternative that came over this edge still comes over it, one block later.
     * `retargetEdgeOnce` is what keeps the count right where two arms lead to one block: each split
     * takes one entry, and the arm that has not been split yet keeps the other.
     */
    retargetEdgeOnce(*base[to], fromPointer, splitPointer);
    split->incomingList.push(module.arena, fromPointer);

    // The edge out of the split, from the jump itself rather than written into slot zero by hand -
    // `to`'s incoming record was moved above rather than added, which is why this is not `append`.
    auto jump = createInst<InstJmp>(module, function, *split, split->source, StringId(), module.scalar.unit, to);
    split->terminatorInst = (ModulePtr<Inst>)((Inst*)jump - base);
    successorsOf(*jump, split->outgoingBlocks);

    return split;
}

void IrEditor::spliceInto(Block& into, Block& block) {
    auto pointer = (ModulePtr<Block>)(&block - base);
    auto intoPointer = (ModulePtr<Block>)(&into - base);

    moveInstructions(block, into);

    // The jump that was here simply stops being anything: a terminator has no operands beyond a
    // condition, and the one being replaced is the jump into the block being absorbed.
    if(into.terminatorInst) dropUses(into.terminatorInst);

    transferTerminator(block, into);
    clearPredecessors(block);
    markChanged();
}

void IrEditor::discardBlock(Block& block) {
    for(auto phi: block.phis(base)) {
        dropUses((ModulePtr<Inst>)phi);
        forgetLocalValue((ModulePtr<Value>)phi);
    }

    for(auto instruction: block.instructions(base)) {
        dropUses(instruction);
        forgetLocalValue((ModulePtr<Value>)instruction);
    }

    // And the edges it owned, which is what takes this block out of every surviving successor's
    // predecessor list and out of one alternative of each of that successor's phis. Doing it here
    // rather than by sweeping the survivors is what makes the counts right: an edge is removed once
    // per edge, and a block with both arms at one successor owned two of them.
    clearTerminator(block);
}

/*
 * Every value of one function, in no particular order: the parameters, the phis, the instructions
 * and each block's terminator.
 *
 * Constants are not among them and cannot be - one belongs to no block and is reached only through
 * whatever names it - so a caller that needs those too finds them by walking operands.
 */
template<class F>
static void eachFunctionValue(ModuleBase base, Function& function, F&& f) {
    for(auto arg: function.args.contents(base)) f((ModulePtr<Value>)arg);

    for(auto blockPointer: function.blocks.contents(base)) {
        auto block = base[blockPointer];

        for(auto phi: block->phis(base)) f((ModulePtr<Value>)phi);
        for(auto instruction: block->instructions(base)) f((ModulePtr<Value>)instruction);
        if(block->terminator()) f((ModulePtr<Value>)block->terminator());
    }
}

// Two passes, because a list may only be cleared before anything is pushed into it.
void IrEditor::rebuildUses() {
    auto forget = [&](ModulePtr<Value> value) {
        if(value) base[value]->useList.clear();
    };

    eachFunctionValue(base, function, [&](ModulePtr<Value> value) {
        forget(value);

        // Constants and arguments reached only from here, which is why this clears operands as well
        // as definitions rather than only the latter.
        eachOperand(base, *base[value], forget);
        eachPlaceRootValue(base, function, *base[value], forget);
    });

    eachFunctionValue(base, function, [&](ModulePtr<Value> value) {
        auto user = (ModulePtr<Inst>)value;

        eachOperand(base, *base[value], [&](ModulePtr<Value> operand) { recordUse(operand, user); });
        eachPlaceRootValue(base, function, *base[value], [&](ModulePtr<Value> storage) {
            recordUse(storage, user);
        });
    });
}

void IrEditor::setBlockOrder(Buffer<ModulePtr<Block>> order) {
    function.blocks.clear();

    U16 index = 0;
    for(auto pointer: order) {
        function.blocks.push(module.arena, pointer);
        base[pointer]->index = index++;
    }
}
