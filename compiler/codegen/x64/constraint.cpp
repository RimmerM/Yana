#include "gen.h"
#include "x64_util.h"

static void clearConstraints(ClassConstraints& c) {
    for(Size i = 0; i < kMaxRegInputs; i++) {
        c.args[i] = kInvalidReg;
        c.results[i] = kInvalidReg;
    }
}

InstConstraints::InstConstraints() {
    for(auto& c: constraints) clearConstraints(c);
}

Constraints::Constraints() {
    mul.constraints[GenReg].args[0] = (Byte)IntRegister::rax;
    mul.constraints[GenReg].results[0] = (Byte)IntRegister::rax;
    mul.clobber |= U64(1) << (Byte)IntRegister::rdx;

    div.constraints[GenReg].args[0] = (Byte)IntRegister::rax;
    div.constraints[GenReg].results[0] = (Byte)IntRegister::rax;
    div.clobber |= U64(1) << (Byte)IntRegister::rdx;

    // Swapped results compared to div. rdx is listed as clobbered even though it is also where the
    // remainder is produced: the division writes it regardless, so anything else living there has
    // to be out of the way first, and a result register is not by itself a signal to move an
    // unrelated occupant (unlike an argument register, which the allocator has to clear anyway).
    rem.constraints[GenReg].args[0] = (Byte)IntRegister::rax;
    rem.constraints[GenReg].results[0] = (Byte)IntRegister::rdx;
    rem.clobber |= U64(1) << (Byte)IntRegister::rax;
    rem.clobber |= U64(1) << (Byte)IntRegister::rdx;

    shift.constraints[GenReg].args[1] = (Byte)IntRegister::rcx;

    movsb.constraints[GenReg].args[0] = (Byte)IntRegister::rdi;
    movsb.constraints[GenReg].args[1] = (Byte)IntRegister::rsi;
    movsb.constraints[GenReg].args[2] = (Byte)IntRegister::rcx;
    // `rep movsb` consumes its own operands as it runs: rdi and rsi are left pointing past the
    // copied region and rcx is counted down to zero. They must be listed as clobbered so that a
    // value still live afterwards (e.g. a destination pointer reused by a following instruction)
    // is copied somewhere safe first, instead of being read back advanced.
    movsb.clobber |= U64(1) << (Byte)IntRegister::rdi;
    movsb.clobber |= U64(1) << (Byte)IntRegister::rsi;
    movsb.clobber |= U64(1) << (Byte)IntRegister::rcx;

    // The unrolled-mov encoding used for an immediate byte count reads its operands from wherever
    // they already are and leaves them alone, so it constrains nothing - it only needs r11 free as
    // a scratch register (see genCopy in gen.cpp). genSetPattern's unrolled form needs no scratch
    // at all, so stosbImm constrains and clobbers nothing.
    movsbImm.clobber |= U64(1) << (Byte)IntRegister::r11;

    // Positional, matching LowerInstSetPattern's used() order (to, count, pattern) - not the
    // rdi/rax/rcx order `rep stosb` itself reads them in.
    stosb.constraints[GenReg].args[0] = (Byte)IntRegister::rdi;
    stosb.constraints[GenReg].args[1] = (Byte)IntRegister::rcx;
    stosb.constraints[GenReg].args[2] = (Byte)IntRegister::rax;
    // As with `rep movsb` above: rdi is advanced past the filled region and rcx is counted down
    // to zero. rax (the pattern) is only read, so it survives intact.
    stosb.clobber |= U64(1) << (Byte)IntRegister::rdi;
    stosb.clobber |= U64(1) << (Byte)IntRegister::rcx;

    // TODO: Support other calling conventions (Sysv, Win64, Simple, Clobber) - only the custom
    // Yana "Complex" convention and the Linux syscall ABI are implemented for now.
    auto& complex = call[(Size)LowerCallType::Complex].constraints[GenReg];

    complex.args[0] = (Byte)IntRegister::rdi;
    complex.args[1] = (Byte)IntRegister::rsi;
    complex.args[2] = (Byte)IntRegister::rdx;
    complex.args[3] = (Byte)IntRegister::rcx;
    complex.args[4] = (Byte)IntRegister::r8;
    complex.args[5] = (Byte)IntRegister::r9;
    complex.args[6] = (Byte)IntRegister::r10;
    complex.args[7] = (Byte)IntRegister::r11;
    complex.results[0] = (Byte)IntRegister::rax;
    complex.results[1] = (Byte)IntRegister::rdx;
    complex.results[2] = (Byte)IntRegister::rcx;
    complex.results[3] = (Byte)IntRegister::r8;
    complex.results[4] = (Byte)IntRegister::r9;
    complex.results[5] = (Byte)IntRegister::r10;
    complex.results[6] = (Byte)IntRegister::r11;
    complex.results[7] = (Byte)IntRegister::rdi;
    complex.results[8] = (Byte)IntRegister::rsi;

    auto& complexInst = call[(Size)LowerCallType::Complex];
    for(Size i = 0; i < 8; i++) {
        complexInst.clobber |= U64(1) << complex.args[i];
    }
    for(Size i = 0; i < 9; i++) {
        complexInst.clobber |= U64(1) << complex.results[i];
    }

    // TODO: Linux only, support different platforms in the future.
    auto& syscall = call[(Size)LowerCallType::Syscall].constraints[GenReg];
    syscall.args[0] = (Byte)IntRegister::rax;
    syscall.args[1] = (Byte)IntRegister::rdi;
    syscall.args[2] = (Byte)IntRegister::rsi;
    syscall.args[3] = (Byte)IntRegister::rdx;
    syscall.args[4] = (Byte)IntRegister::r10;
    syscall.args[5] = (Byte)IntRegister::r8;
    syscall.args[6] = (Byte)IntRegister::r9;
    syscall.results[0] = (Byte)IntRegister::rax;

    auto& syscallInst = call[(Size)LowerCallType::Syscall];
    syscallInst.clobber |= U64(1) << (Byte)IntRegister::rcx;
    syscallInst.clobber |= U64(1) << (Byte)IntRegister::r11;
}

const InstConstraints* Constraints::getConstraints(LowerBase base, LowerInst* inst) const {
    auto kind = inst->kind;

    if(kind == LowerInst::Mul && isIntLike(((LowerInstBinary*)inst)->result.type)) {
        return &mul;
    } else if((kind == LowerInst::Div || kind == LowerInst::IDiv) && isIntLike(((LowerInstBinary*)inst)->result.type)) {
        // Signed (IDiv) and unsigned (Div) division share the same rax/rdx register protocol -
        // genIDiv/genDiv only differ in the opcode's reg-field extension, not in which registers
        // are read/written.
        return &div;
    } else if((kind == LowerInst::Rem || kind == LowerInst::IRem) && isIntLike(((LowerInstBinary*)inst)->result.type)) {
        return &rem;
    } else if((kind == LowerInst::Shr || kind == LowerInst::Shl || kind == LowerInst::Sar) && isIntLike(((LowerInstBinary*)inst)->result.type)) {
        return &shift;
    } else if(kind == LowerInst::Call) {
        auto type = ((LowerInstCall*)inst)->getCallType();
        assertTrue(type <= LowerCallType::LastType);
        return &call[(Size)type];
    } else if(kind == LowerInst::Copy) {
        // Encoding chosen once by transformFunction; genCopy reads the same flag.
        return ((LowerInstCopy*)inst)->isUnrolled() ? &movsbImm : &movsb;
    } else if(kind == LowerInst::SetPattern) {
        return ((LowerInstSetPattern*)inst)->isUnrolled() ? &stosbImm : &stosb;
    } else {
        return nullptr;
    }
}

const InstConstraints& Constraints::getCall(LowerCallType type) const {
    assertTrue(type <= LowerCallType::LastType);
    return call[(Size)type];
}
