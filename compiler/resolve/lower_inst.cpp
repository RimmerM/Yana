/*
 * The instruction dispatch, and the operands every arm of it reads.
 *
 * `lowerInstruction` decides which of the three files below owns a kind and hands it the
 * instruction; `lowerTerminator` is the other half, and is here rather than in one of them because
 * a terminator makes an edge instead of a value and shares nothing with the three.
 */

#include "lower_internal.h"

LowerPtr<LowerValue> mappedValue(LowerContext& lower, ModulePtr<Value> pointer);

// Constants belong to no block in the resolve IR, so each one is materialized once per function
// in the entry block the first time something asks for it.
static LowerPtr<LowerValue> mapConstant(LowerContext& lower, ModulePtr<Value> pointer) {
    auto& value = *lower.local[pointer];
    LowerInst* instruction;
    auto type = lowerType(lower.global, value.type);

    switch(value.kind) {
        case Value::ConstInt:
            instruction = new (lower.to.arena) LowerImm(value.name, type, ((ConstInt&)value).value);
            break;
        case Value::ConstFloat:
            instruction = new (lower.to.arena) LowerImm(value.name, type, F64(((ConstFloat&)value).value));
            break;
        case Value::ConstDouble:
            instruction = new (lower.to.arena) LowerImm(value.name, type, ((ConstDouble&)value).value);
            break;
        default:
            assertTrue("expected constant" == nullptr);
            return nullptr;
    }

    instruction->source = value.source;
    lower.constantBlock->addInst(lower.lower, instruction);

    auto result = instruction->created().ptr - lower.lower;
    lower.values.add(pointer, result);
    return result;
}

LowerPtr<LowerValue> mappedValue(LowerContext& lower, ModulePtr<Value> pointer) {
    if(!pointer) return nullptr;
    if(auto found = lower.values.get(pointer)) return found.unwrap();

    auto& value = *lower.local[pointer];
    if(isConstant(value)) return mapConstant(lower, pointer);

    /*
     * How wide a concrete type is, which is a constant that only this stage knows.
     *
     * Materialized here rather than where the instruction sits, on the same terms as a literal: it
     * has no effect, no position, and one value however many times it is asked for. Doing it lazily
     * is what keeps the scaling fold above from leaving an `imm 1` behind every time it removes the
     * only use of a stride. A *generic* type's metric is a real load out of a descriptor and is not
     * this case; it is mapped where the instruction is.
     */
    if(value.kind == Value::TypeMetric) {
        auto& metric = (InstTypeMetric&)value;
        auto number = lower.repr.metric(metric.of, metric.metric);

        auto result = immediate(lower, number, lowerType(lower.global, value.type));
        lower.values.add(pointer, result);
        return result;
    }

    assertTrue("resolve value was used before it was lowered" == nullptr);
    return nullptr;
}

LowerCmp lowerCmp(LowerContext& lower, InstCmp& compare) {
    auto signedOperands = signedOperand(lower.global, lower.local[compare.lhs]->type);

    switch(compare.cmp) {
        case CompareOp::Eq: return LowerCmp::eq;
        case CompareOp::Ne: return LowerCmp::neq;
        case CompareOp::Gt: return signedOperands ? LowerCmp::igt : LowerCmp::gt;
        case CompareOp::Ge: return signedOperands ? LowerCmp::ige : LowerCmp::ge;
        case CompareOp::Lt: return signedOperands ? LowerCmp::ilt : LowerCmp::lt;
        case CompareOp::Le: return signedOperands ? LowerCmp::ile : LowerCmp::le;
    }

    return LowerCmp::eq;
}

void mapResult(LowerContext& lower, ModulePtr<Value> from, LowerInst* instruction) {
    auto& value = *lower.local[from];
    instruction->source = value.source;

    if(!isUnit(lower.global, value.type)) {
        assertTrue(instruction->createdCount == 1);
        lower.values.add(from, instruction->created().ptr - lower.lower);
    }
}

LowerInst::Kind binaryKind(LowerContext& lower, InstBinary& binary) {
    auto floating = isFloat(lower.global, binary.type);

    // Which of the two multiply/divide/remainder instructions an integer operation becomes is the
    // type's own signedness: an unsigned type's arithmetic is the unsigned one, which is the
    // whole of what makes Native's U8..U64 different from the I-family at the machine level.
    auto signed_ = signedOperand(lower.global, binary.type);

    switch(binary.kind) {
        case Value::Add: return LowerInst::Add;
        case Value::Sub: return LowerInst::Sub;
        case Value::Mul: return floating ? LowerInst::Mul : (signed_ ? LowerInst::IMul : LowerInst::Mul);
        case Value::Div: return floating ? LowerInst::Div : (signed_ ? LowerInst::IDiv : LowerInst::Div);
        case Value::Rem: return signed_ ? LowerInst::IRem : LowerInst::Rem;
        case Value::Shl: return LowerInst::Shl;
        case Value::Shr: return LowerInst::Shr;
        case Value::Sar: return LowerInst::Sar;
        case Value::And: return LowerInst::And;
        case Value::Or: return LowerInst::Or;
        case Value::Xor: return LowerInst::Xor;
        default:
            assertTrue("expected binary instruction" == nullptr);
            return LowerInst::Add;
    }
}

/*
 * Which file lowers a kind.
 *
 * One switch with no default, so a kind added to inst.def is a compile error here rather than an
 * assertion something reaches - which is the whole reason the group is a function instead of three
 * sets of case labels sitting beside the arms that already name them. The kinds that never arrive
 * here are named too: an argument and a phi are created by the block walk, a constant is
 * materialized per function on first use, and a terminator ends its block, so listing them is what
 * makes their absence below a statement rather than a hole.
 */
enum class InstGroup {
    Storage,     // lower_mem.cpp
    Compute,     // lower_calc.cpp
    Call,        // lower_call.cpp
    Terminator,  // lowerTerminator, below
    Elsewhere,   // not reached through an instruction walk at all
};

static InstGroup instGroup(Value::Kind kind) {
    switch(kind) {
        case Value::Arg:
        case Value::Phi:
        case Value::ConstInt:
        case Value::ConstFloat:
        case Value::ConstDouble:
        case Value::ConstString:
            return InstGroup::Elsewhere;

        case Value::Alloc:
        case Value::LoadPlace:
        case Value::Init:
        case Value::Assign:
        case Value::Aggregate:
        case Value::Borrow:
        case Value::Move:
        case Value::Swap:
        case Value::Exchange:
        case Value::Copy:
        case Value::Drop:
        case Value::Address:
            return InstGroup::Storage;

        case Value::TypeMetric:
        case Value::TableSlot:
        case Value::Native:
        case Value::Cast:
        case Value::Bitcast:
        case Value::Neg:
        case Value::Not:
        case Value::Sqrt:
        case Value::Fma:
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
        case Value::Select:
        case Value::VecSplat:
        case Value::VecLane:
        case Value::VecWithLane:
        case Value::VecShuffle:
        case Value::VecReduce:
        case Value::Symbol:
            return InstGroup::Compute;

        case Value::Call:
        case Value::CallDyn:
        case Value::GenCall:
            return InstGroup::Call;

        case Value::Je:
        case Value::Jmp:
        case Value::Ret:
        case Value::Unreachable:
            return InstGroup::Terminator;
    }

    assertTrue("instruction kind with no lowering" == nullptr);
    return InstGroup::Elsewhere;
}

void lowerInstruction(LowerContext& lower, LowerBlock& block, ModulePtr<Inst> pointer) {
    auto& instruction = *lower.local[pointer];
    auto instValue = (ModulePtr<Value>)pointer;
    auto function = lower.local[lower.local[instruction.block]->function];
    LowerInst* result = nullptr;

    switch(instGroup(instruction.kind)) {
        case InstGroup::Storage:
            result = lowerStorageInst(lower, block, instruction, instValue, function);
            break;
        case InstGroup::Compute:
            result = lowerComputeInst(lower, block, instruction, instValue, function);
            break;
        case InstGroup::Call:
            result = lowerCallInst(lower, block, instruction, instValue, function);
            break;
        case InstGroup::Terminator:
        case InstGroup::Elsewhere:
            assertTrue("unexpected non-control resolve instruction" == nullptr);
            return;
    }

    // A null result is the arm saying it mapped what it produced itself, or that there was nothing
    // to map - a store, a drop, and every value that is an address the place walk already computed.
    if(result) mapResult(lower, instValue, result);
}


void lowerTerminator(LowerContext& lower, LowerBlock& block, ModulePtr<Inst> pointer) {
    auto& instruction = *lower.local[pointer];
    LowerInst* result = nullptr;
    switch(instruction.kind) {
        case Value::Je: {
            auto& branch = (InstJe&)instruction;
            result = je(lower.lower, lower.to, block,
                        lower.lower[mappedValue(lower, branch.cond)],
                        lower.lower[lower.blocks.getValue(branch.thenBlock).unwrap()],
                        lower.lower[lower.blocks.getValue(branch.elseBlock).unwrap()]);
            break;
        }
        case Value::Jmp: {
            auto& jump = (InstJmp&)instruction;
            result = jmp(lower.lower, lower.to, block,
                         lower.lower[lower.blocks.getValue(jump.target).unwrap()]);
            break;
        }
        case Value::Ret: {
            auto& returnInst = (InstRet&)instruction;
            auto functionPointer = lower.local[instruction.block]->function;
            auto function = lower.local[functionPointer];
            auto memoryResult = isMemoryType(lower.global, function->returnType);

            /*
             * A unit result carries nothing back, whatever the resolve IR named.
             *
             * A body that returns unit *concretely* is resolved with no operand at all, but one
             * that returns a type variable is not: `fn identity(value: a) -> a` returns its
             * argument, and the specialization at `a = {}` is a `ret` naming a value nothing below
             * ever emitted - a unit value is not a value here. Read off the type rather than off
             * the operand, so that both spellings of "returns nothing" reach the same instruction.
             */
            auto returned = isUnit(lower.global, function->returnType) ? ModulePtr<Value>(nullptr)
                                                                       : returnInst.value;

            if(memoryResult && returned) {
                // The other place bytes are written into storage that did not hold them: the
                // caller's hidden result slot. A returned move relocates into it by whatever rule
                // its type relocates by, exactly as an initialization does.
                auto target = lower.returnPlaces.getValue(functionPointer).unwrap();
                auto source = mappedValue(lower, returned);
                auto copyInst = relocate(lower, block, target, returned, source, function->returnType);
                copyInst->source = instruction.source;
            }

            auto count = returned && !memoryResult ? 1 : 0;
            auto storage = lower.to.arena.alloc(sizeof(LowerInstRet) + sizeof(LowerPtr<LowerValue>) * count);
            auto returnLower = new (storage) LowerInstRet;
            returnLower->usedCount = count;

            if(count) returnLower->used()[0] = mappedValue(lower, returned);
            result = block.addInst(lower.lower, returnLower);
            break;
        }

        /*
         * A block control never leaves, said as itself.
         *
         * It used to be a `ret` of zeros of the function's own result types - a lie contained to a
         * block nothing arrives at the end of, and one `validateRet` held to the signature like any
         * other return. What that cost is an epilogue and a `c3` per abort arm, which was the whole
         * of §11.2's +323 bytes; the lower IR now has the terminator instead, and the x64 form for
         * it encodes nothing at all.
         */
        case Value::Unreachable:
            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstUnreachable());
            break;

        default:
            assertTrue("expected resolve terminator" == nullptr);
            return;
    }

    result->source = instruction.source;
}
