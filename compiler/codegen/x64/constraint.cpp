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

    // The integer half of the System V AMD64 ABI. Vector arguments and results are not described
    // here: InstConstraints::clobber is a general-register mask, so there is no way to say that a
    // SysV call destroys xmm0-xmm15 (B9), and a float argument would be silently mishandled rather
    // than rejected. Integer and pointer arguments beyond the sixth go on the stack, which the
    // caller side does not implement yet either - see assignArgs.
    auto& sysv = call[(Size)LowerCallType::Sysv].constraints[GenReg];

    sysv.args[0] = (Byte)IntRegister::rdi;
    sysv.args[1] = (Byte)IntRegister::rsi;
    sysv.args[2] = (Byte)IntRegister::rdx;
    sysv.args[3] = (Byte)IntRegister::rcx;
    sysv.args[4] = (Byte)IntRegister::r8;
    sysv.args[5] = (Byte)IntRegister::r9;
    sysv.results[0] = (Byte)IntRegister::rax;
    sysv.results[1] = (Byte)IntRegister::rdx;

    // Stated in full rather than derived from the argument and result registers: r10 and r11 are
    // caller-saved without appearing in either, and a callee is entitled to use them.
    auto& sysvInst = call[(Size)LowerCallType::Sysv];
    sysvInst.clobber =
        (U64(1) << (Byte)IntRegister::rax) | (U64(1) << (Byte)IntRegister::rcx) |
        (U64(1) << (Byte)IntRegister::rdx) | (U64(1) << (Byte)IntRegister::rsi) |
        (U64(1) << (Byte)IntRegister::rdi) | (U64(1) << (Byte)IntRegister::r8) |
        (U64(1) << (Byte)IntRegister::r9)  | (U64(1) << (Byte)IntRegister::r10) |
        (U64(1) << (Byte)IntRegister::r11);

    // TODO: Support the remaining calling conventions (Win64, Simple, Clobber).
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

    // What a function compiled with each convention has to give back, and what it may assume about
    // rsp where it makes a call. Only the two described conventions have an entry: the others have
    // no argument tables either (see the TODO above), so a function using one cannot be compiled at
    // all yet, and a preserved set for it would be a guess.
    //
    // A syscall is never a function's own convention - the kernel is the callee - so its entry
    // stays empty.
    auto preserved =
        (U64(1) << (Byte)IntRegister::rbx) |
        (U64(1) << (Byte)IntRegister::r12) |
        (U64(1) << (Byte)IntRegister::r13) |
        (U64(1) << (Byte)IntRegister::r14) |
        (U64(1) << (Byte)IntRegister::r15);

    auto& complexConvention = convention[(Size)LowerCallType::Complex];
    complexConvention.calleeSaved = preserved;

    // The compiler is on both sides of its own convention and nothing it generates yet needs a
    // 16-byte-aligned stack, so this one costs no padding. It has to become 16 as soon as a value
    // is spilled to an aligned vector slot.
    complexConvention.stackAlignment = 8;

    auto& sysvConvention = convention[(Size)LowerCallType::Sysv];
    sysvConvention.calleeSaved = preserved;

    // Required by the ABI, and not negotiable: a SysV callee is entitled to use aligned vector
    // stores against its own frame, which are only aligned if rsp was.
    sysvConvention.stackAlignment = 16;

    // The two halves of a convention describe one contract from opposite sides: a register a call
    // leaves alone is exactly one its callee has to give back. Stating both and checking them
    // against each other catches a convention that gains an argument or result register in one
    // table and not the other - which would otherwise show up as a caller reading back a register
    // the callee had quietly stopped preserving.
    assertTrue(complexConvention.calleeSaved == (~complexInst.clobber & kAllocatableRegs));
    assertTrue(sysvConvention.calleeSaved == (~sysvInst.clobber & kAllocatableRegs));
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

const CallConvention& Constraints::getConvention(LowerCallType type) const {
    assertTrue(type <= LowerCallType::LastType);
    return convention[(Size)type];
}

/*
 * Instruction constraint queries. See the block comment on InstShape in gen.h.
 */

InstShape shapeOf(LowerBase base, const Constraints& constraints, LowerFunction& fun, LowerInst* inst) {
    InstShape shape;

    if(inst->kind == LowerInst::Ret) {
        shape.c = &constraints.getCall(fun.callType);
        shape.isReturn = true;
        return shape;
    }

    shape.c = constraints.getConstraints(base, inst);

    if(inst->kind == LowerInst::Call && ((LowerInstCall*)inst)->getCallType() != LowerCallType::Syscall) {
        shape.argStart = 1;
    }

    return shape;
}

RegId wantForUse(LowerBase base, LowerInst* inst, const InstShape& shape, Size i) {
    if(!shape.c || i < shape.argStart) return kInvalidReg;

    auto used = inst->used();
    auto value = base[used[i]];
    if(isImplicit(value)) return kInvalidReg;

    auto cls = classForType(value->type);
    Size classIndex = 0;

    for(Size j = shape.argStart; j < i; j++) {
        auto other = base[used[j]];
        if(isImplicit(other)) continue;
        if(classForType(other->type) == cls) classIndex++;
    }

    if(classIndex >= kMaxRegInputs) return kInvalidReg;

    auto& table = shape.c->constraints[cls];
    return shape.isReturn ? table.results[classIndex] : table.args[classIndex];
}

RegId wantForResult(LowerInst* inst, const InstShape& shape, Size i) {
    if(!shape.c || shape.isReturn) return kInvalidReg;

    auto created = inst->created();
    if(isImplicit(&created[i])) return kInvalidReg;

    auto cls = classForType(created[i].type);
    Size classIndex = 0;

    for(Size j = 0; j < i; j++) {
        if(isImplicit(&created[j])) continue;
        if(classForType(created[j].type) == cls) classIndex++;
    }

    if(classIndex >= kMaxRegInputs) return kInvalidReg;
    return shape.c->constraints[cls].results[classIndex];
}

U64 writtenRegisters(LowerBase base, LowerInst* inst, const InstShape& shape) {
    if(!shape.c) return 0;

    U64 mask = shape.isReturn ? 0 : shape.c->clobber;

    auto used = inst->used();
    for(Size i = 0; i < used.size(); i++) {
        mask |= regBit(wantForUse(base, inst, shape, i));
    }

    auto created = inst->created();
    for(Size i = 0; i < created.size(); i++) {
        mask |= regBit(wantForResult(inst, shape, i));
    }

    return mask;
}
