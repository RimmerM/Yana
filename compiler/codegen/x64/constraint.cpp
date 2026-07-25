#include "gen.h"
#include "x64_util.h"

InstConstraints::InstConstraints() {
    set(intArgs, kMaxRegInputs, kInvalidReg);
    set(floatArgs, kMaxRegInputs, kInvalidReg);
    set(intResults, kMaxRegInputs, kInvalidReg);
    set(floatResults, kMaxRegInputs, kInvalidReg);
}

Constraints::Constraints() {
    mul.intArgs[0] = (Byte)IntRegister::rax;
    mul.intResults[0] = (Byte)IntRegister::rax;
    mul.clobber |= U64(1) << (Byte)IntRegister::rdx;

    div.intArgs[0] = (Byte)IntRegister::rax;
    div.intResults[0] = (Byte)IntRegister::rax;
    div.clobber |= U64(1) << (Byte)IntRegister::rdx;

    // Swapped results compared to div.
    rem.intArgs[0] = (Byte)IntRegister::rax;
    rem.intResults[0] = (Byte)IntRegister::rdx;
    rem.clobber |= U64(1) << (Byte)IntRegister::rax;

    shift.intArgs[1] = (Byte)IntRegister::rcx;

    movsb.intArgs[0] = (Byte)IntRegister::rdi;
    movsb.intArgs[1] = (Byte)IntRegister::rsi;
    movsb.intArgs[2] = (Byte)IntRegister::rcx;

    stosb.intArgs[0] = (Byte)IntRegister::rdi;
    stosb.intArgs[1] = (Byte)IntRegister::rax;
    stosb.intArgs[2] = (Byte)IntRegister::rcx;

    // TODO: Support other calling conventions.
    auto& complex = call[(Size)LowerCallType::Complex];

    complex.intArgs[0] = (Byte)IntRegister::rdi;
    complex.intArgs[1] = (Byte)IntRegister::rsi;
    complex.intArgs[2] = (Byte)IntRegister::rdx;
    complex.intArgs[3] = (Byte)IntRegister::rcx;
    complex.intArgs[4] = (Byte)IntRegister::r8;
    complex.intArgs[5] = (Byte)IntRegister::r9;
    complex.intArgs[6] = (Byte)IntRegister::r10;
    complex.intArgs[7] = (Byte)IntRegister::r11;
    complex.intResults[0] = (Byte)IntRegister::rax;
    complex.intResults[1] = (Byte)IntRegister::rdx;
    complex.intResults[2] = (Byte)IntRegister::rcx;
    complex.intResults[3] = (Byte)IntRegister::r8;
    complex.intResults[4] = (Byte)IntRegister::r9;
    complex.intResults[5] = (Byte)IntRegister::r10;
    complex.intResults[6] = (Byte)IntRegister::r11;
    complex.intResults[7] = (Byte)IntRegister::rdi;
    complex.intResults[8] = (Byte)IntRegister::rsi;

    for(Size i = 0; i < CallConstraints::kMaxIntArgs; i++) {
        complex.set.clobber |= U64(1) << complex.set.intArgs[i];
        complex.set.clobber |= U64(1) << complex.set.intResults[i];

        complex.get[i].intResults[0] = complex.set.intArgs[i];
    }

    for(Size i = 0; i < CallConstraints::kMaxFloatArgs; i++) {
        complex.set.floatArgs[i] = (Byte)XmmRegister::xmm0 + i;
        complex.set.floatResults[i] = (Byte)XmmRegister::xmm0 + i;
        complex.set.clobber |= U64(1) << ((Byte)XmmRegister::xmm0 + i);

        complex.get[i].floatResults[0] = complex.set.floatArgs[i];
    }

    // TODO: Linux only, support different platforms in the future.
    call[(Size)LowerCallType::Syscall].set.intArgs[0] = (Byte)IntRegister::rax;
    call[(Size)LowerCallType::Syscall].set.intArgs[1] = (Byte)IntRegister::rdi;
    call[(Size)LowerCallType::Syscall].set.intArgs[2] = (Byte)IntRegister::rsi;
    call[(Size)LowerCallType::Syscall].set.intArgs[3] = (Byte)IntRegister::rdx;
    call[(Size)LowerCallType::Syscall].set.intArgs[4] = (Byte)IntRegister::r10;
    call[(Size)LowerCallType::Syscall].set.intArgs[5] = (Byte)IntRegister::r8;
    call[(Size)LowerCallType::Syscall].set.intArgs[6] = (Byte)IntRegister::r9;
    call[(Size)LowerCallType::Syscall].set.intResults[0] = (Byte)IntRegister::rax;
    call[(Size)LowerCallType::Syscall].set.clobber |= U64(1) << (Byte)IntRegister::rcx;
    call[(Size)LowerCallType::Syscall].set.clobber |= U64(1) << (Byte)IntRegister::r11;
}

const InstConstraints* Constraints::getConstraints(OffsetBase base, LowerInst* inst) const {
    auto kind = inst->kind;

    if(kind == LowerInst::Arg) {
        auto a = (LowerArg*)inst;
        auto index = a->getIndex();
        auto type = (Size)base[base[a->block]->fun]->callType;

        if(index >= kMaxRegInputs) {
            return nullptr;
        } else {
            return &call[type].get[index];
        }
    } else if(kind == LowerInst::Mul && isIntLike(((LowerInstBinary*)inst)->result.type)) {
        return &mul;
    } else if(kind == LowerInst::Div && isIntLike(((LowerInstBinary*)inst)->result.type)) {
        return &div;
    } else if(kind == LowerInst::Rem && isIntLike(((LowerInstBinary*)inst)->result.type)) {
        return &rem;
    } else if((kind == LowerInst::Shr || kind == LowerInst::Shl || kind == LowerInst::Sar) && isIntLike(((LowerInstBinary*)inst)->result.type)) {
        return &shift;
    } else if(kind == LowerInst::Call) {
        auto type = ((LowerInstCall*)inst)->getCallType();
        assertTrue(type <= LowerCallType::LastType);
        return &call[(Size)type];
    } else if(kind == LowerInst::Copy) {
        return &movsb;
    } else if(kind == LowerInst::SetPattern) {
        return &stosb;
    } else {
        return nullptr;
    }
}

const InstConstraints& Constraints::getCall(LowerCallType type) const {
    assertTrue(type <= LowerCallType::LastType);
    return call[(Size)type];
}
