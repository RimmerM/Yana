#include "gen.h"
#include "x64_util.h"

struct CallConv {
    ArrayF<U8, 16> registerIntArgs;
    ArrayF<U8, 16> registerIntReturns;
    ArrayF<U8, 16> registerFloatArgs;
    ArrayF<U8, 16> registerFloatReturns;
};

inline U8 makeMod(U8 mode, U8 lhs, U8 rhs) {
    return (mode << 6) | ((rhs & 7) << 3) | (lhs & 7);
}

inline U8 makeRex(bool is64, U8 lhs, U8 rhs, U8 index) {
    return 0b01000000 | (is64 ? 0b1000 : 0) | ((rhs & 8) >> 1) | ((index & 8) >> 2) | ((lhs & 8) >> 3);
}

static void genReg(AsmModule& to, LowerType type, U8 acc, U8 opCode, U8 ext, U8 prefix = 0) {
    if(is64Bit(type) || needsRex(acc)) {
        to.buffer.writeByte(makeRex(is64Bit(type), acc, ext, 0));
    }

    if(prefix) to.buffer.writeByte(prefix);
    to.buffer.writeByte(opCode);
    to.buffer.writeByte(makeMod(3, acc, ext));
}

static void genReg(AsmModule& to, LowerInstUnary& i, U8 opCode, U8 ext, U8 prefix = 0) {
    genReg(to, i.result.type, i.from->reg, opCode, ext, prefix);
}

static void genReg(AsmModule& to, LowerInstBinary& i, U8 opCode, U8 ext, U8 prefix = 0) {
    genReg(to, i.result.type, i.lhs->reg, opCode, ext, prefix);
}

static void genRegEax(AsmModule& to, LowerInstBinary& i, U8 opCode, U8 ext) {
    assertTrue(i.result.reg == (U8)IntRegister::rax || i.result.reg == (U8)IntRegister::rdx);
    assertTrue(isReg(i.lhs) && (i.lhs->reg == (U8)IntRegister::rax || i.lhs->reg == (U8)IntRegister::rdx));
    assertTrue(isReg(i.rhs));

    if(is64Bit(i.lhs->type) || needsRex(&i)) {
        to.buffer.writeByte(makeRex(is64Bit(i.lhs->type), i.rhs->reg, 0, 0));
    }

    to.buffer.writeByte(opCode);
    to.buffer.writeByte(makeMod(3, i.rhs->reg, ext));
}

static void genRegReg(AsmModule& to, LowerType type, U8 acc, U8 operand, U8 opCode, U8 prefix = 0) {
    if(is64Bit(type) || needsRex(acc) || needsRex(operand)) {
        to.buffer.writeByte(makeRex(is64Bit(type), acc, operand, 0));
    }

    if(prefix) to.buffer.writeByte(prefix);
    to.buffer.writeByte(opCode);
    to.buffer.writeByte(makeMod(3, acc, operand));
}

static void genRegReg(AsmModule& to, LowerInstUnary& i, U8 opCode, U8 prefix = 0) {
    genRegReg(to, i.result.type, i.result.reg, i.from->reg, opCode, prefix);
}

static void genRegReg(AsmModule& to, LowerInstBinary& i, U8 opCode, U8 prefix = 0) {
    genRegReg(to, i.result.type, i.lhs->reg, i.rhs->reg, opCode, prefix);
}

static void genRegImm(AsmModule& to, LowerInstBinary& i, U8 opCode8, U8 opCode32, U8 ext) {
    if(is64Bit(i.lhs->type) || needsRex(&i)) {
        to.buffer.writeByte(makeRex(is64Bit(i.lhs->type), i.lhs->reg, ext, 0));
    }

    if(auto imm8 = encodeImm8(i.rhs)) {
        to.buffer.writeByte(opCode8);
        to.buffer.writeByte(makeMod(3, i.lhs->reg, ext));
        to.buffer.writeByte(imm8.unwrap());
    } else if(auto imm32 = encodeImm32(i.rhs)) {
        to.buffer.writeByte(opCode32);
        to.buffer.writeByte(makeMod(3, i.lhs->reg, ext));
        to.buffer.writeInt<LittleEndian>(imm32.unwrap());
    } else {
        assertTrue("invalid immediate value" == nullptr);
    }
}

static void genIncReg(AsmModule& to, LowerInstBinary& i, bool sub) {
    if(is64Bit(i.lhs->type) || needsRex(&i)) {
        to.buffer.writeByte(makeRex(is64Bit(i.lhs->type), i.lhs->reg, 0, 0));
    }

    to.buffer.writeByte(0xff);
    to.buffer.writeByte(makeMod(3, i.lhs->reg, sub ? 1 : 0));
}

static void genZeroReg(AsmModule& to, U8 reg, LowerType type) {
    if(is64Bit(type) || needsRex(reg)) {
        to.buffer.writeByte(makeRex(is64Bit(type), reg, reg, 0));
    }

    to.buffer.writeByte(0x31);
    to.buffer.writeByte(makeMod(3, reg, reg));
}

static void genMovImm(AsmModule& to, LowerImm& i) {
    // No need to generate anything for implicit immediates.
    if(i.implicit) return;

    auto imm = i.i;
    auto is64 = is64Bit(i.result.type);

    // Special case for zeroing registers.
    // TODO: This is unsafe when combined with flag forwarding between instructions;
    //  only enable when we can check if it is safe in this position.
    //if(imm == 0) return genZeroReg(to, i.result.reg, i.result.type);

    // We need to set a full immediate value when <32 bits are used,
    // or else the upper part of the register would retain its old value.
    if((!is64 || (imm & 0xffffffff80000000) == 0xffffffff80000000 || (imm & 0x7fffffff) == imm) && (i.result.reg & 8) == 0) {
        // 32 bit
        to.buffer.writeByte(0xb8 + (i.result.reg & 7));
        to.buffer.writeInt<LittleEndian>(imm);
    } else {
        // 64 bit
        to.buffer.writeByte(makeRex(is64, i.result.reg, 0, 0));
        to.buffer.writeByte(0xb8 + (i.result.reg & 7));

        if(is64) {
            to.buffer.writeLong<LittleEndian>(imm);
        } else {
            to.buffer.writeInt<LittleEndian>(imm);
        }
    }
}

static void genMovReg(AsmModule& to, LowerInstUnary& i) {
    genRegReg(to, i, 0x8b);
}

static void genMovRegS(AsmModule& to, LowerInstUnary& i) {
    genRegReg(to, i, 0x63);
}

static void genCommonBinary(AsmModule& to, LowerInstBinary& i, U8 regreg, U8 ext) {
    if(isReg(i.lhs) && isReg(i.rhs)) {
        genRegReg(to, i, regreg);
    } else if(isReg(i.lhs) && isImm(i.rhs)) {
        genRegImm(to, i, 0x83, 0x81, ext);
    } else {
        assertTrue("unsupported operands to binary instruction" == nullptr);
    }
}

static void genNop(AsmModule& to, LowerInst& i) {
    to.buffer.writeByte(0x90);
}

static void genAdd(AsmModule& to, LowerInstBinary& i) {
    if(isReg(i.lhs) && isImm(i.rhs)) {
        if(((LowerImm*)i.rhs)->i == 1) {
            return genIncReg(to, i, false);
        } else if(((LowerImm*)i.rhs)->i == (U64)I64(-1)) {
            return genIncReg(to, i, true);
        }
    }

    genCommonBinary(to, i, 0x3, 0);
}

static void genSub(AsmModule& to, LowerInstBinary& i) {
    if(isReg(i.lhs) && isImm(i.rhs)) {
        if(((LowerImm*)i.rhs)->i == 1) {
            genIncReg(to, i, true);
        } else if(((LowerImm*)i.rhs)->i == (U64)I64(-1)) {
            genIncReg(to, i, false);
        }
    }

    genCommonBinary(to, i, 0x29, 5);
}

static void genOr(AsmModule& to, LowerInstBinary& i) {
    genCommonBinary(to, i, 0xb, 1);
}

static void genXor(AsmModule& to, LowerInstBinary& i) {
    genCommonBinary(to, i, 0x33, 6);
}

static void genAnd(AsmModule& to, LowerInstBinary& i) {
    genCommonBinary(to, i, 0x23, 4);
}

static void genShift(AsmModule& to, LowerInstBinary& i, U8 onceOp, U8 immOp, U8 regOp, U8 ext) {
    if(isReg(i.lhs) && isReg(i.rhs)) {
        assertTrue(i.rhs->reg == (U8)IntRegister::rcx);
        genReg(to, i, regOp, ext);
    } else if(isReg(i.lhs) && isImm(i.rhs)) {
        auto imm = ((LowerImm*)i.rhs)->i;
        assertTrue(imm <= 0x7f);

        if(imm == 1) {
            genReg(to, i, onceOp, ext);
        } else {
            genRegImm(to, i, immOp, immOp, ext);
        }
    } else {
        assertTrue("unsupported operands to shift instruction" == nullptr);
    }
}

static void genShift(AsmModule& to, LowerInstBinary& i, U8 ext) {
    genShift(to, i, 0xd1, 0xc1, 0xd3, ext);
}

static void genIMul(AsmModule& to, LowerInstBinary& i) {
    assertTrue(isInt(i.lhs->type));

    if(isReg(i.lhs) && isReg(i.rhs)) {
        genRegReg(to, i, 0xaf, 0x0f);
    } else if(isReg(i.lhs) && isImm(i.rhs)) {
        genRegImm(to, i, 0x6b, 0x69, i.lhs->reg);
    } else {
        assertTrue("unsupported operands to mul instruction" == nullptr);
    }
}

static void genMul(AsmModule& to, LowerInstBinary& i) {
    genRegEax(to, i, 0xf7, 4);
}

static void genCqo(AsmModule& to, LowerType type) {
    if(is64Bit(type)) {
        to.buffer.writeByte(makeRex(true, 0, 0, 0));
    }

    to.buffer.writeByte(0x99);
}

static void genDiv(AsmModule& to, LowerInstBinary& i) {
    // div uses both rax and rdx, so we need to zero rdx first.
    // Since the division overwrites the flags anyway, it is safe to use xor here.
    assertTrue(isInt(i.lhs->type));
    genZeroReg(to, (U8)IntRegister::rdx, i.lhs->type);
    genRegEax(to, i, 0xf7, 6);
}

static void genIDiv(AsmModule& to, LowerInstBinary& i) {
    // idiv uses both rax and rdx, so we need to sign-extend rax into rdx first.
    assertTrue(isInt(i.lhs->type));
    genCqo(to, i.lhs->type);
    genRegEax(to, i, 0xf7, 7);
}

static void genNeg(AsmModule& to, LowerInstUnary& i) {
    genReg(to, i, 0xf7, 3);
}

static void genNot(AsmModule& to, LowerInstUnary& i) {
    assertTrue(isInt(i.from->type));
    genReg(to, i, 0xf7, 2);
}

static void genCopy(AsmModule& to, LowerInstCopy& i) {
    if(isImm(i.count)) {
        // TODO: generate set of mov instructions.
    } else {
        assertTrue(i.to->reg == (U8)IntRegister::rdi && i.from->reg == (U8)IntRegister::rsi && i.count->reg == (U8)IntRegister::rcx);
        to.buffer.writeByte(0xf3);
        to.buffer.writeByte(0xa4);
    }
}

static void genSetPattern(AsmModule& to, LowerInstSetPattern& i) {
    if(isImm(i.count)) {
        // TODO: generate set of mov instructions.
    } else {
        assertTrue(i.to->reg == (U8)IntRegister::rdi && i.pattern->reg == (U8)IntRegister::rax && i.count->reg == (U8)IntRegister::rcx);
        to.buffer.writeByte(0xf3);
        to.buffer.writeByte(0xaa);
    }
}

static void genBswap(AsmModule& to, LowerInstUnary& i) {
    assertTrue(isReg(i.from));
    assertTrue(i.from->reg == i.result.reg);

    auto is64 = is64Bit(i.result.type);
    if(is64 || needsRex(i.result.reg)) {
        // 64 bit
        to.buffer.writeByte(makeRex(is64, i.result.reg, 0, 0));
        to.buffer.writeByte(0x0f);
        to.buffer.writeByte(0xc8 + (i.result.reg & 7));
    } else {
        // 32 bit
        to.buffer.writeByte(0x0f);
        to.buffer.writeByte(0xc8 + (i.result.reg & 7));
    }
}

static void genRet(AsmModule& to, LowerInstRet& i) {
    // Register allocation should already have generated moves to set the correct registers here,
    // so all we have to do is return.
    to.buffer.writeByte(0xc3);
}

static void genPush(AsmModule& to, LowerInstUnary& i) {
    if(isReg(i.from)) {
        if(needsRex(i.from->reg)) {
            to.buffer.writeByte(makeRex(false, i.from->reg, 0, 0));
        }

        to.buffer.writeByte(0x50 + (i.result.reg & 7));
    } else if(isImm(i.from)) {
        if(auto imm8 = encodeImm8(i.from)) {
            to.buffer.writeByte(0x6a);
            to.buffer.writeByte(imm8.unwrap());
        } else if(auto imm32 = encodeImm32(i.from)) {
            to.buffer.writeByte(0x68);
            to.buffer.writeInt<LittleEndian>(imm32.unwrap());
        } else {
            assertTrue("invalid immediate value" == nullptr);
        }
    } else {
        assertTrue("unsupported operand to push instruction" == nullptr);
    }
}

static void genPop(AsmModule& to, LowerInstSingle& i) {
    if(needsRex(i.result.reg)) {
        to.buffer.writeByte(makeRex(false, i.result.reg, 0, 0));
    }

    to.buffer.writeByte(0x58 + (i.result.reg & 7));
}

static void genCmpToFlags(AsmModule& to, LowerInstCmp& i) {
    genCommonBinary(to, i, 0x3b, 7);
}

static U8 getCompareOp(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::eq:
            return 0x94;
        case LowerCmp::neq:
            return 0x95;
        case LowerCmp::gt:
            return 0x97;
        case LowerCmp::ge:
            return 0x93;
        case LowerCmp::lt:
            return 0x92;
        case LowerCmp::le:
            return 0x96;
        case LowerCmp::igt:
            return 0x9f;
        case LowerCmp::ige:
            return 0x9d;
        case LowerCmp::ilt:
            return 0x9c;
        case LowerCmp::ile:
            return 0x9e;
    }

    assertTrue(false);
    return 0;
}

static void genFlagsToReg(AsmModule& to, U8 reg, LowerCmp cmp) {
    if(needsRex(reg)) {
        to.buffer.writeByte(makeRex(false, reg, 0, 0));
    }

    to.buffer.writeByte(0x0f);
    to.buffer.writeByte(getCompareOp(cmp));
}

static void genCmp(AsmModule& to, LowerInstCmp& i) {
    auto type = i.lhs->type;

    if(isInt(type) || isPtr(type)) {
        genCmpToFlags(to, i);
        if(!i.implicit) genFlagsToReg(to, i.result.reg, i.cmp);
    } else if(isFloat(type)) {
        // TODO
    } else {
        assertTrue("invalid comparison operands" == nullptr);
    }
}

static void genTestReg(AsmModule& to, LowerType type, U8 lhs, U8 rhs) {
    genRegReg(to, type, lhs, rhs, 0x85);
}

static U8 getSelectOp(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::eq:
            return 0x44;
        case LowerCmp::neq:
            return 0x45;
        case LowerCmp::gt:
            return 0x47;
        case LowerCmp::ge:
            return 0x43;
        case LowerCmp::lt:
            return 0x42;
        case LowerCmp::le:
            return 0x46;
        case LowerCmp::igt:
            return 0x4f;
        case LowerCmp::ige:
            return 0x4d;
        case LowerCmp::ilt:
            return 0x4c;
        case LowerCmp::ile:
            return 0x4e;
    }

    assertTrue(false);
    return 0;
}

static void genSelect(AsmModule& to, LowerInstSelect& i) {
    LowerCmp cmp;

    if(i.embeddedCmp) {
        // Flags are already set correctly.
        cmp = i.embeddedCmp.unwrap();
    } else if(isReg(i.cmp)) {
        genTestReg(to, LowerType::Int32, i.cmp->reg, i.cmp->reg);
        cmp = LowerCmp::eq;
    } else {
        assertTrue("unsupported operands to select instruction" == nullptr);
        return;
    }

    if(!isReg(i.lhs) || !isReg(i.rhs)) {
        assertTrue("unsupported select op" == nullptr);
    }

    assertTrue(i.lhs->reg = i.result.reg);
    genRegReg(to, i.lhs->type, i.lhs->reg, i.rhs->reg, getSelectOp(cmp), 0x0f);
}

static void genSwap(AsmModule& to, LowerInstSwap& i) {
    assertTrue(isReg(i.lhs) && isReg(i.rhs));
    genRegReg(to, i.lhs->type, i.lhs->reg, i.rhs->reg, 0x87);
}

struct AddressMode {
    U32 displacement;
    U8 mod;
    U8 rm;
    U8 index;
    Maybe<U8> sib;
};

static AddressMode genAddress(LowerValue* source) {
    if(isReg(source)) {
        if(needsRex(source->reg)) {
            return {

            };
        } else {
            return {
                .rm = source->reg,
            };
        }
    } else if(source->inst->kind == LowerInst::X86Address) {
        auto a = (LowerInstX86Address*)source->inst;
        return {
            .displacement = a->displacement,
        };
    } else {
        assertTrue("unsupported memory address" == nullptr);
        return AddressMode {};
    }
}

static void genLoad(AsmModule& to, LowerInstLoad& i) {
    auto a = genAddress(i.from);

    if(i.width >= 8 || needsRex(i.result.reg) || needsRex(a.mod)) {
        to.buffer.writeByte(makeRex(i.width >= 8, i.result.reg, a.rm, a.index));
    }

    to.buffer.writeByte(0x8b);
    to.buffer.writeByte(makeMod(a.mod, i.result.reg, a.rm));
    if(a.sib) to.buffer.writeByte(a.sib.unwrap());
    if(a.displacement > 0) to.buffer.writeInt<LittleEndian>(a.displacement);
}

static void genCast(AsmModule& to, LowerInstCast& i) {
    auto source = i.from->type;
    auto target = i.result.type;

    if(isIntLike(source) && isIntLike(target)) {
        if(i.signedResult && i.signedSource) {
            genMovRegS(to, i);
        } else {
            // This implements truncation by zeroing the upper part of the register if needed.
            // Because of that, we need to perform the operation even if the source and target register are the same.
            genMovReg(to, i);
        }
    } else if(isIntLike(source) && isFloat(target)) {
        // TODO
    } else if(isFloat(source) && isIntLike(target)) {
        // TODO
    } else {
        assertTrue("invalid cast operands" == nullptr);
    }
}

static void genBitcast(AsmModule& to, LowerInstUnary& i) {
    auto source = i.from->type;
    auto target = i.result.type;

    if(isIntLike(source) && isIntLike(target)) {
        // If the result is in a different register, move it.
        // Otherwise, there is nothing to do.
        if(i.from->reg != i.result.reg) {
            genMovReg(to, i);
        }
    } else if(isIntLike(source) && isFloat(target)) {
        // TODO
    } else if(isFloat(source) && isIntLike(target)) {
        // TODO
    } else {
        assertTrue("invalid cast operands" == nullptr);
    }
}

static void genInst(AsmModule& to, LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Arg:
            // No code generation needed.
            break;
        case LowerInst::Global:
            // TODO: Load address to global.
            break;
        case LowerInst::Fun:
            // TODO: Load address to function.
            break;
        case LowerInst::Imm:
            genMovImm(to, *(LowerImm*)inst);
            break;

        case LowerInst::Nop:
            genNop(to, *inst);
            break;
        case LowerInst::Cast:
            genCast(to, *(LowerInstCast*)inst);
            break;
        case LowerInst::Bitcast:
            genBitcast(to, *(LowerInstUnary*)inst);
            break;
        case LowerInst::Set:
            genMovReg(to, *(LowerInstUnary*)inst);
            break;
        case LowerInst::Neg:
            genNeg(to, *(LowerInstUnary*)inst);
            break;
        case LowerInst::Not:
            genNot(to, *(LowerInstUnary*)inst);
            break;

        case LowerInst::Add:
            genAdd(to, *(LowerInstBinary*)inst);
            break;
        case LowerInst::Sub:
            genSub(to, *(LowerInstBinary*)inst);
            break;
        case LowerInst::Mul:
            genMul(to, *(LowerInstBinary*)inst);
            break;
        case LowerInst::IMul:
            genIMul(to, *(LowerInstBinary*)inst);
            break;
        case LowerInst::Div:
            genDiv(to, *(LowerInstBinary*)inst);
            break;
        case LowerInst::IDiv:
            genIDiv(to, *(LowerInstBinary*)inst);
            break;
        case LowerInst::Rem:
            genDiv(to, *(LowerInstBinary*)inst);
            break;
        case LowerInst::IRem:
            genIDiv(to, *(LowerInstBinary*)inst);
            break;

        case LowerInst::Shl:
            genShift(to, *(LowerInstBinary*)inst, 4);
            break;
        case LowerInst::Shr:
            genShift(to, *(LowerInstBinary*)inst, 5);
            break;
        case LowerInst::Sar:
            genShift(to, *(LowerInstBinary*)inst, 7);
            break;
        case LowerInst::And:
            genAnd(to, *(LowerInstBinary*)inst);
            break;
        case LowerInst::Or:
            genOr(to, *(LowerInstBinary*)inst);
            break;
        case LowerInst::Xor:
            genXor(to, *(LowerInstBinary*)inst);
            break;

        case LowerInst::Cmp:
            genCmp(to, *(LowerInstCmp*)inst);
            break;
        case LowerInst::Select:
            genSelect(to, *(LowerInstSelect*)inst);
            break;
        case LowerInst::Alloca:
            // No code generation needed, this is done in the function prologue.
            break;
        case LowerInst::Load:
            genLoad(to, *(LowerInstLoad*)inst);
            break;
        case LowerInst::Store:
        case LowerInst::Copy:
            genCopy(to, *(LowerInstCopy*)inst);
            break;
        case LowerInst::SetPattern:
            genSetPattern(to, *(LowerInstSetPattern*)inst);
            break;

        case LowerInst::CallStatic:
        case LowerInst::CallDyn:
        case LowerInst::CallSys:

        case LowerInst::Je:
        case LowerInst::Jmp:
        case LowerInst::Ret:
            genRet(to, *(LowerInstRet*)inst);
            break;
        case LowerInst::Phi:
            // No code generation needed, this is done before jumping out of incoming blocks.
            break;

        case LowerInst::X86Address:
            // Embedded into any uses.
            break;
        case LowerInst::X86Lea:
            // TODO;
            break;
        case LowerInst::X86Bswap:
            genBswap(to, *(LowerInstUnary*)inst);
            break;
        case LowerInst::X86Push:
            genPush(to, *(LowerInstUnary*)inst);
            break;
        case LowerInst::X86Pop:
            genPop(to, *(LowerInstSingle*)inst);
            break;
    }
}

static void genControl(AsmModule& to, LowerInst* inst, LowerBlock* next) {
    // Statically detect if new instruction were added that aren't being handled here.
    static_assert(LowerInst::LastTerminator - LowerInst::FirstTerminator == 2, "missing code generation for terminating instructions.");
    assertTrue(isTerminator(inst));

    switch(inst->kind) {
        case LowerInst::Je:
        case LowerInst::Jmp:
        case LowerInst::Ret:
            genRet(to, *(LowerInstRet*)inst);
            break;
    }
}

void genFunction(Context& context, AsmModule& to, LowerFunction& fun) {
    auto blocks = fun.blocks.contents();

    for(Size i = 0; i < blocks.size(); i++) {
        auto b = blocks[i];
        to.startBlock(b);

        // Generate common instructions in the block.
        // We don't have to take care of phi instructions, since this is done by register allocation.
        for(auto inst: b->instructions.contents()) {
            genInst(to, inst);
        }

        // Keep track of the block that will be positioned immediately after this one,
        // which allows us to remove some unconditional jumps.
        auto next = i + 1 >= blocks.size() ? nullptr : blocks[i + 1];
        genControl(to, b->terminator, next);

        to.endBlock(b);
    }
}
