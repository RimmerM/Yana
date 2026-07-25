#include "gen.h"
#include "x64_util.h"

/*
 * Fixed-register tables and calling conventions.
 *
 * Two different things live here, and the split matters. An InstConstraints describes one
 * *instruction* whose encoding forces its operands into particular registers - a division's
 * rax/rdx protocol, a shift's count in rcx. A CallConvention describes a *contract* between two
 * functions, and is used from both sides: a call site places its arguments from it, and the
 * function it lands in finds them in the same places, because both ask classifyArgs rather than
 * deciding for themselves.
 *
 * A convention is stated as tables of registers rather than as code, so adding one is data. The
 * two axes that turned out to matter are how a convention runs out of registers (per class, as SysV
 * does, or by argument position, as Win64 does) and what it reserves below its stack arguments -
 * both are fields rather than special cases.
 */

static void clearConstraints(ClassConstraints& c) {
    for(Size i = 0; i < kMaxRegInputs; i++) {
        c.args[i] = kInvalidReg;
        c.results[i] = kInvalidReg;
    }
}

InstConstraints::InstConstraints() {
    for(auto& c: constraints) clearConstraints(c);
}

static RegId gen(IntRegister reg) {
    return makeRegId(GenReg, U16(reg));
}

static RegId xmm(Size index) {
    return makeRegId(XmmReg, U16(index));
}

// Appends general registers to an argument or result table, in the order the convention hands them
// out. Taken by reference to the array so that the tables below state a register list once and
// nothing has to repeat its length.
template<Size count>
static void addGenRegs(CallConvention::ClassRegs& table, const IntRegister (&regs)[count]) {
    for(Size i = 0; i < count; i++) table.add(gen(regs[i]));
}

// Appends xmm0..xmm(count-1). Every convention that passes vector values uses them in order from
// zero, so there is nothing to state beyond how many.
static void addXmmRegs(CallConvention::ClassRegs& table, Size count) {
    for(Size i = 0; i < count; i++) table.add(xmm(i));
}

template<Size count>
static void addGenClobber(RegSet& set, const IntRegister (&regs)[count]) {
    for(Size i = 0; i < count; i++) set.add(gen(regs[i]));
}

static void addXmmClobber(RegSet& set, Size first, Size count) {
    for(Size i = 0; i < count; i++) set.add(xmm(first + i));
}

/*
 * The register lists each convention hands out, at file scope so that a convention built from
 * another's lists (Clobber, which passes arguments exactly as Complex does and simply keeps
 * nothing) states them once.
 */

static const IntRegister kSysvArgs[] = {
    IntRegister::rdi, IntRegister::rsi, IntRegister::rdx,
    IntRegister::rcx, IntRegister::r8, IntRegister::r9,
};

static const IntRegister kSysvResults[] = { IntRegister::rax, IntRegister::rdx };

// Stated in full rather than derived from the argument and result registers: r10 and r11 are
// caller-saved without appearing in either, and a callee is entitled to use them.
static const IntRegister kSysvClobber[] = {
    IntRegister::rax, IntRegister::rcx, IntRegister::rdx, IntRegister::rsi,
    IntRegister::rdi, IntRegister::r8, IntRegister::r9, IntRegister::r10,
    IntRegister::r11,
};

static const IntRegister kWin64Args[] = {
    IntRegister::rcx, IntRegister::rdx, IntRegister::r8, IntRegister::r9,
};

static const IntRegister kWin64Results[] = { IntRegister::rax };

static const IntRegister kWin64Clobber[] = {
    IntRegister::rax, IntRegister::rcx, IntRegister::rdx,
    IntRegister::r8, IntRegister::r9, IntRegister::r10, IntRegister::r11,
};

static const IntRegister kComplexArgs[] = {
    IntRegister::rdi, IntRegister::rsi, IntRegister::rdx, IntRegister::rcx,
    IntRegister::r8, IntRegister::r9, IntRegister::r10, IntRegister::r11,
};

static const IntRegister kComplexResults[] = {
    IntRegister::rax, IntRegister::rdx, IntRegister::rcx, IntRegister::r8,
    IntRegister::r9, IntRegister::r10, IntRegister::r11, IntRegister::rdi,
    IntRegister::rsi,
};

static const IntRegister kSimpleArgs[] = {
    IntRegister::rdi, IntRegister::rsi, IntRegister::rdx,
};

static const IntRegister kSimpleResults[] = { IntRegister::rax };

// TODO: Linux only, support different platforms in the future.
static const IntRegister kSyscallArgs[] = {
    IntRegister::rax, IntRegister::rdi, IntRegister::rsi,
    IntRegister::rdx, IntRegister::r10, IntRegister::r8, IntRegister::r9,
};

static const IntRegister kSyscallResults[] = { IntRegister::rax };

// Finishes a convention once its tables are stated: the registers it has to give back are the
// allocatable ones its calls leave alone, which is the same contract seen from the other side.
//
// The clobber set is the one part that cannot be derived - r10 and r11 are caller-saved under SysV
// without appearing in any argument or result table, and a callee is entitled to use them - so it
// is stated in full and checked here for the mistake that actually happens: a convention that gains
// an argument or result register without gaining the clobber that goes with it, which would show up
// as a caller reading back a register the callee had quietly stopped preserving.
static void finish(CallConvention& convention) {
    for(auto& table: convention.args) {
        for(Size i = 0; i < table.count; i++) {
            assertTrue(convention.clobber.has(table.regs[i])); // argument register is not clobbered
        }
    }

    for(auto& table: convention.results) {
        for(Size i = 0; i < table.count; i++) {
            assertTrue(convention.clobber.has(table.regs[i])); // result register is not clobbered
        }
    }

    // A convention that clobbered rbp would be saying that a call destroys its caller's frame
    // pointer, which is not something the caller can defend against: the frame pointer is not a
    // value the allocator placed and there is nothing to copy out of the way.
    assertTrue(!convention.clobber.has(framePointerReg())); // rbp is preserved by every convention

    convention.calleeSaved = convention.clobber.complement(allocatableRegs());
    convention.defined = true;
}

Constraints::Constraints() {
    mul.constraints[GenReg].args[0] = gen(IntRegister::rax);
    mul.constraints[GenReg].results[0] = gen(IntRegister::rax);
    mul.clobber.add(gen(IntRegister::rdx));

    div.constraints[GenReg].args[0] = gen(IntRegister::rax);
    div.constraints[GenReg].results[0] = gen(IntRegister::rax);
    div.clobber.add(gen(IntRegister::rdx));

    // Swapped results compared to div. rdx is listed as clobbered even though it is also where the
    // remainder is produced: the division writes it regardless, so anything else living there has
    // to be out of the way first, and a result register is not by itself a signal to move an
    // unrelated occupant (unlike an argument register, which the allocator has to clear anyway).
    rem.constraints[GenReg].args[0] = gen(IntRegister::rax);
    rem.constraints[GenReg].results[0] = gen(IntRegister::rdx);
    rem.clobber.add(gen(IntRegister::rax));
    rem.clobber.add(gen(IntRegister::rdx));

    shift.constraints[GenReg].args[1] = gen(IntRegister::rcx);

    movsb.constraints[GenReg].args[0] = gen(IntRegister::rdi);
    movsb.constraints[GenReg].args[1] = gen(IntRegister::rsi);
    movsb.constraints[GenReg].args[2] = gen(IntRegister::rcx);
    // `rep movsb` consumes its own operands as it runs: rdi and rsi are left pointing past the
    // copied region and rcx is counted down to zero. They must be listed as clobbered so that a
    // value still live afterwards (e.g. a destination pointer reused by a following instruction)
    // is copied somewhere safe first, instead of being read back advanced.
    movsb.clobber.add(gen(IntRegister::rdi));
    movsb.clobber.add(gen(IntRegister::rsi));
    movsb.clobber.add(gen(IntRegister::rcx));

    // The unrolled-mov encoding used for an immediate byte count reads its operands from wherever
    // they already are and leaves them alone, so it constrains nothing - it only needs r11 free as
    // a scratch register (see genCopy in gen.cpp). genSetPattern's unrolled form needs no scratch
    // at all, so stosbImm constrains and clobbers nothing.
    movsbImm.clobber.add(gen(IntRegister::r11));

    // Positional, matching LowerInstSetPattern's used() order (to, count, pattern) - not the
    // rdi/rax/rcx order `rep stosb` itself reads them in.
    stosb.constraints[GenReg].args[0] = gen(IntRegister::rdi);
    stosb.constraints[GenReg].args[1] = gen(IntRegister::rcx);
    stosb.constraints[GenReg].args[2] = gen(IntRegister::rax);
    // As with `rep movsb` above: rdi is advanced past the filled region and rcx is counted down
    // to zero. rax (the pattern) is only read, so it survives intact.
    stosb.clobber.add(gen(IntRegister::rdi));
    stosb.clobber.add(gen(IntRegister::rcx));

    /*
     * System V AMD64.
     */
    {
        auto& sysv = convention[(Size)LowerCallType::Sysv];
        addGenRegs(sysv.args[GenReg], kSysvArgs);
        addXmmRegs(sysv.args[XmmReg], 8);
        addGenRegs(sysv.results[GenReg], kSysvResults);
        addXmmRegs(sysv.results[XmmReg], 2);

        addGenClobber(sysv.clobber, kSysvClobber);
        addXmmClobber(sysv.clobber, 0, 16); // every vector register is caller-saved

        // Required by the ABI, and not negotiable: a SysV callee is entitled to use aligned vector
        // stores against its own frame, which are only aligned if rsp was.
        sysv.stackAlignment = 16;
        finish(sysv);
    }

    /*
     * Win64.
     */
    {
        auto& win = convention[(Size)LowerCallType::Win64];
        addGenRegs(win.args[GenReg], kWin64Args);
        addXmmRegs(win.args[XmmReg], 4);
        addGenRegs(win.results[GenReg], kWin64Results);
        addXmmRegs(win.results[XmmReg], 1);

        addGenClobber(win.clobber, kWin64Clobber);
        addXmmClobber(win.clobber, 0, 6); // xmm6-15 are callee-saved here, unlike SysV

        // The two things that make Win64 differ in kind rather than in register names, and the
        // reason both are fields on CallConvention rather than code: an argument takes the slot of
        // its *position*, so a float in position 2 uses xmm2 and leaves r8 alone, and the caller
        // leaves 32 bytes below the stack arguments for the callee to spill its register ones into.
        win.positionalArgs = true;
        win.shadowSpace = 32;
        win.stackAlignment = 16;
        finish(win);
    }

    /*
     * The compiler's own conventions.
     *
     * Complex gives the callee as much as possible, on the grounds that both sides are compiled
     * here and a call is not the barrier an external one is. Simple is the opposite: it keeps almost
     * everything for the caller, so calling a small helper costs the caller nothing. Clobber passes
     * arguments exactly as Complex does but keeps nothing at all.
     */
    {
        auto& complex = convention[(Size)LowerCallType::Complex];
        addGenRegs(complex.args[GenReg], kComplexArgs);
        addXmmRegs(complex.args[XmmReg], 16);
        addGenRegs(complex.results[GenReg], kComplexResults);
        addXmmRegs(complex.results[XmmReg], 16);

        addGenClobber(complex.clobber, kComplexArgs);
        addGenClobber(complex.clobber, kComplexResults);
        addXmmClobber(complex.clobber, 0, 16);

        // The compiler is on both sides of this one and nothing it generates yet needs a
        // 16-byte-aligned stack, so it costs no padding. It has to become 16 as soon as a value is
        // spilled to an aligned vector slot.
        complex.stackAlignment = 8;
        finish(complex);
    }

    {
        auto& simple = convention[(Size)LowerCallType::Simple];
        addGenRegs(simple.args[GenReg], kSimpleArgs);
        addXmmRegs(simple.args[XmmReg], 3);
        addGenRegs(simple.results[GenReg], kSimpleResults);
        addXmmRegs(simple.results[XmmReg], 1);

        addGenClobber(simple.clobber, kSimpleArgs);
        addGenClobber(simple.clobber, kSimpleResults);
        addXmmClobber(simple.clobber, 0, 3);
        finish(simple);
    }

    {
        auto& clobber = convention[(Size)LowerCallType::Clobber];
        addGenRegs(clobber.args[GenReg], kComplexArgs);
        addXmmRegs(clobber.args[XmmReg], 16);
        addGenRegs(clobber.results[GenReg], kComplexResults);
        addXmmRegs(clobber.results[XmmReg], 16);

        // Everything the allocator could have handed out is gone across this call, so nothing can
        // be kept in a register over it - except rbp, which no convention may clobber. The caller
        // may be holding a frame pointer there and nothing in the IR represents that for the
        // allocator to move out of the way, so rbp stays preserved even by the convention whose
        // whole point is that it preserves nothing.
        clobber.clobber = allocatableRegs();
        clobber.clobber.remove(framePointerReg());
        finish(clobber);
    }

    /*
     * Linux syscalls. Never a function's own convention - the kernel is the callee - so its
     * preserved set is only ever read as "what a syscall leaves alone".
     */
    {
        auto& syscall = convention[(Size)LowerCallType::Syscall];
        addGenRegs(syscall.args[GenReg], kSyscallArgs);
        addGenRegs(syscall.results[GenReg], kSyscallResults);

        addGenClobber(syscall.clobber, kSyscallArgs);
        addGenClobber(syscall.clobber, kSyscallResults);
        syscall.clobber.add(gen(IntRegister::rcx));
        syscall.clobber.add(gen(IntRegister::r11));
        finish(syscall);
    }
}

const Constraints& targetConstraints() {
    static Constraints constraints;
    return constraints;
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
    } else if(kind == LowerInst::Copy) {
        // Encoding chosen once by transformFunction; genCopy reads the same flag.
        return ((LowerInstCopy*)inst)->isUnrolled() ? &movsbImm : &movsb;
    } else if(kind == LowerInst::SetPattern) {
        return ((LowerInstSetPattern*)inst)->isUnrolled() ? &stosbImm : &stosb;
    } else {
        return nullptr;
    }
}

const CallConvention& Constraints::getConvention(LowerCallType type) const {
    assertTrue(type <= LowerCallType::LastType);
    return convention[(Size)type];
}

U32 argAreaBytes(const CallConvention& convention, const Array<ArgLocation>& args) {
    auto bytes = convention.shadowSpace;

    for(auto& location: args) {
        if(location.kind == ArgLocation::Stack && location.stackOffset + 8 > bytes) {
            bytes = location.stackOffset + 8;
        }
    }

    if(bytes == 0) return 0;

    auto alignment = convention.stackAlignment;
    return (bytes + alignment - 1) & ~(alignment - 1);
}

/*
 * Instruction shapes. See the block comment on InstShape in gen.h.
 */

InstShape shapeOf(LowerBase base, const Constraints& constraints, LowerFunction& fun, LowerInst* inst) {
    InstShape shape;

    auto used = inst->used();
    auto created = inst->created();

    if(inst->kind == LowerInst::Ret) {
        // A return's operands are the function's results, so they are placed by the result half of
        // its own convention rather than by any table of its own.
        shape.convention = &constraints.getConvention(fun.callType);
        shape.isReturn = true;

        classifyResults(*shape.convention, used.size(), [&](Size i) {
            return base[used[i]]->type;
        }, shape.uses);

        return shape;
    }

    if(inst->kind == LowerInst::Call) {
        auto callType = ((LowerInstCall*)inst)->getCallType();
        shape.convention = &constraints.getConvention(callType);

        // A syscall has no callee to resolve: its used()[0] is the syscall number, which the
        // convention places like any other argument. Every other call names its target first, and
        // that operand is not an argument at all.
        Size argStart = callType == LowerCallType::Syscall ? 0 : 1;
        for(Size i = 0; i < argStart; i++) shape.uses.push(ArgLocation {});

        classifyArgs(*shape.convention, used.size() - argStart, [&](Size i) {
            return base[used[i + argStart]]->type;
        }, shape.uses);

        classifyResults(*shape.convention, created.size(), [&](Size i) {
            return created[i].type;
        }, shape.creates);

        shape.clobber = shape.convention->clobber;
        return shape;
    }

    auto c = constraints.getConstraints(base, inst);

    if(c) shape.clobber = c->clobber;

    Size taken[kPhysRegClassCount] = {};
    for(Size i = 0; i < used.size(); i++) {
        auto value = base[used[i]];

        if(!c || isImplicit(value)) {
            shape.uses.push(ArgLocation {});
            continue;
        }

        auto cls = classForType(value->type);
        auto index = taken[cls]++;
        auto reg = index < kMaxRegInputs ? c->constraints[cls].args[index] : kInvalidReg;

        shape.uses.push(reg == kInvalidReg ? ArgLocation {} : ArgLocation::inRegister(reg));
    }

    Size producing[kPhysRegClassCount] = {};
    for(Size i = 0; i < created.size(); i++) {
        if(!c || isImplicit(&created[i])) {
            shape.creates.push(ArgLocation {});
            continue;
        }

        auto cls = classForType(created[i].type);
        auto index = producing[cls]++;
        auto reg = index < kMaxRegInputs ? c->constraints[cls].results[index] : kInvalidReg;

        shape.creates.push(reg == kInvalidReg ? ArgLocation {} : ArgLocation::inRegister(reg));
    }

    return shape;
}

/*
 * Memory operands. See the block comment on memoryUseOperand in gen.h.
 */

// The width the encoding works at, which is not always the operand's own. A comparison produces an
// Int32 whatever it compared, and a pointer arithmetic instruction works at 64 bits however wide the
// offset was declared.
static LowerType operationType(LowerBase base, LowerInst* inst) {
    if(inst->kind == LowerInst::Cmp) return base[((LowerInstCmp*)inst)->lhs]->type;

    assertTrue(inst->createdCount > 0);
    return inst->created()[0].type;
}

// Which operand the *encoding* can take from memory, before anything is asked about the value in it.
//
// Every entry here is the r/m operand of a two-operand form whose other operand is the register in
// the ModRM.reg field: `add r, r/m` rather than `add r/m, r`. The destination is always the register
// one, so the memory operand is never also a result - a read-modify-write form would need the
// operand and the result to share a slot, which only happens once webs are coalesced across an
// instruction.
//
// Casts are absent deliberately: their source and result widths differ by definition, and the width
// rule below is exactly what a cast breaks. A spilled cast source is still reloaded.
static I32 memoryFormOperand(LowerBase base, LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Set:
            // MOV r, r/m.
            return 0;

        case LowerInst::Add: case LowerInst::Sub:
        case LowerInst::And: case LowerInst::Or: case LowerInst::Xor:
        case LowerInst::Cmp:
            // The reg-destination direction of each group-1 opcode.
            return 1;

        case LowerInst::IMul:
            // IMUL r, r/m is the two-operand form; the three-operand one takes an immediate there
            // instead, and genIMul chooses between them the same way.
            return isImm(base[((LowerInstBinary*)inst)->rhs]) ? kNoMemoryOperand : 1;

        case LowerInst::Mul: case LowerInst::Div: case LowerInst::IDiv:
        case LowerInst::Rem: case LowerInst::IRem:
            // The group-3 forms take their second operand as r/m; the first is forced into rax by
            // InstConstraints and is loaded into it directly whether it comes from a register or
            // from the frame.
            return 1;

        default:
            return kNoMemoryOperand;
    }
}

I32 memoryUseOperand(LowerBase base, LowerInst* inst) {
    auto index = memoryFormOperand(base, inst);
    if(index == kNoMemoryOperand) return kNoMemoryOperand;

    auto used = inst->used();
    assertTrue(Size(index) < used.size());

    // An operand that was folded into the encoding has no location of any kind, in memory or
    // otherwise - an embedded immediate is already part of the instruction.
    auto value = base[used[index]];
    if(isImplicit(value)) return kNoMemoryOperand;

    // A slot is exactly as wide as the value in it and slots are packed by width, so an access of
    // any other width would read or write a neighbouring value along with this one.
    if(stackSlotClassFor(value->type) != stackSlotClassFor(operationType(base, inst))) {
        return kNoMemoryOperand;
    }

    return index;
}

RegSet writtenRegisters(const InstShape& shape) {
    // A return ends the function: nothing is live afterwards, so there is nothing for its clobbers
    // to protect and no reason to keep anything out of the registers it writes.
    if(shape.isReturn) return RegSet {};

    auto set = shape.clobber;

    for(auto& location: shape.uses) {
        if(location.kind == ArgLocation::Register) set.add(location.reg);
    }

    for(auto& location: shape.creates) {
        if(location.kind == ArgLocation::Register) set.add(location.reg);
    }

    return set;
}
