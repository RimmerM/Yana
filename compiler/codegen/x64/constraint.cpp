#include "gen.h"
#include "x64_util.h"

/*
 * Calling conventions, and the instruction shapes derived from them and from the selected forms.
 *
 * A CallConvention describes a *contract* between two functions, and is used from both sides: a call
 * site places its arguments from it, and the function it lands in finds them in the same places,
 * because both ask classifyArgs rather than deciding for themselves. It is the one part of an
 * instruction's register behaviour a machine form cannot state, because where an argument goes
 * depends on how many of each bank came before it.
 *
 * A convention is stated as tables of registers rather than as code, so adding one is data. The
 * two axes that turned out to matter are how a convention runs out of registers (per bank, as SysV
 * does, or by argument position, as Win64 does) and what it reserves below its stack arguments -
 * both are fields rather than special cases.
 *
 * Everything else an instruction does to the register file - which operands it forces into
 * particular registers, what it clobbers, which result it writes over which operand, which operand
 * may stay in a frame slot - comes from its selected MachineForm; see machine.cpp. shapeOf at the
 * bottom of this file is where the two meet.
 */

// Appends general registers to an argument or result table, in the order the convention hands them
// out. Taken by reference to the array so that the tables below state a register list once and
// nothing has to repeat its length.
template<Size count>
static void addGenRegs(CallConvention::BankRegs& table, const IntRegister (&regs)[count]) {
    for(Size i = 0; i < count; i++) table.add(gpr(regs[i]));
}

// Appends xmm0..xmm(count-1). Every convention that passes vector values uses them in order from
// zero, so there is nothing to state beyond how many.
static void addXmmRegs(CallConvention::BankRegs& table, Size count) {
    for(Size i = 0; i < count; i++) table.add(vectorReg(i));
}

template<Size count>
static void addGenClobber(RegSet& set, const IntRegister (&regs)[count]) {
    for(Size i = 0; i < count; i++) set.add(gpr(regs[i]));
}

static void addXmmClobber(RegSet& set, Size first, Size count) {
    for(Size i = 0; i < count; i++) set.add(vectorReg(first + i));
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
// as a caller reading back a register the callee had quietly stopped preserving. `preservesArgs`
// exempts the argument half of that check and only it - a result register is written by definition,
// whoever the callee is.
static void finish(CallConvention& convention) {
    auto& target = targetRegisters();

    // Each table belongs to the bank it is indexed by, and names only registers a value of that
    // bank could have been given in the first place - a convention that passed an argument in rsp,
    // or in a vector register the bank does not have, would be describing a machine that does not
    // exist. Checked here because the tables are written by hand.
    auto checkTable = [&](RegisterBankId bank, const CallConvention::BankRegs& table, bool clobbered) {
        for(Size i = 0; i < table.count; i++) {
            assertTrue(table.regs[i].bank == bank); // a convention table holds a register of another bank
            assertTrue(target.bank(bank).allocatable.has(table.regs[i])); // ... that is not allocatable
            assertTrue(!clobbered || convention.clobber.has(table.regs[i])); // argument or result register is not clobbered
        }
    };

    for(Size bank = 0; bank < kRegisterBankCount; bank++) {
        checkTable(RegisterBankId(bank), convention.args[bank], !convention.preservesArgs);
        checkTable(RegisterBankId(bank), convention.results[bank], true);
    }

    // A convention that clobbered rbp would be saying that a call destroys its caller's frame
    // pointer, which is not something the caller can defend against: the frame pointer is not a
    // value the allocator placed and there is nothing to copy out of the way.
    assertTrue(!convention.clobber.has(framePointerReg())); // rbp is preserved by every convention

    convention.calleeSaved = convention.clobber.complement(allocatableRegs());
    convention.defined = true;
}

Constraints::Constraints() {
    /*
     * System V AMD64.
     */
    {
        auto& sysv = convention[(Size)LowerCallType::Sysv];
        addGenRegs(sysv.args[BankGpr], kSysvArgs);
        addXmmRegs(sysv.args[BankVector], 8);
        addGenRegs(sysv.results[BankGpr], kSysvResults);
        addXmmRegs(sysv.results[BankVector], 2);

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
        addGenRegs(win.args[BankGpr], kWin64Args);
        addXmmRegs(win.args[BankVector], 4);
        addGenRegs(win.results[BankGpr], kWin64Results);
        addXmmRegs(win.results[BankVector], 1);

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
        addGenRegs(complex.args[BankGpr], kComplexArgs);
        addXmmRegs(complex.args[BankVector], 16);
        addGenRegs(complex.results[BankGpr], kComplexResults);
        addXmmRegs(complex.results[BankVector], 16);

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
        addGenRegs(simple.args[BankGpr], kSimpleArgs);
        addXmmRegs(simple.args[BankVector], 3);
        addGenRegs(simple.results[BankGpr], kSimpleResults);
        addXmmRegs(simple.results[BankVector], 1);

        addGenClobber(simple.clobber, kSimpleArgs);
        addGenClobber(simple.clobber, kSimpleResults);
        addXmmClobber(simple.clobber, 0, 3);
        finish(simple);
    }

    {
        auto& clobber = convention[(Size)LowerCallType::Clobber];
        addGenRegs(clobber.args[BankGpr], kComplexArgs);
        addXmmRegs(clobber.args[BankVector], 16);
        addGenRegs(clobber.results[BankGpr], kComplexResults);
        addXmmRegs(clobber.results[BankVector], 16);

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
     *
     * And what it leaves alone is everything but three registers. rax carries the result, and the
     * `syscall` instruction itself destroys rcx and r11 - it puts the return address in one and the
     * flags in the other - so those three are the whole clobber set. The argument registers are
     * *not* in it, which is what separates this convention from every other one here: the kernel
     * reads them and hands them back, where a compiled callee owns the register it was passed in.
     *
     * It matters wherever a check that ends the program sits inside a loop. A bounds check's failure
     * arm is an `exit` syscall, every value the loop carries is live across it, and treating the
     * syscall as destroying rdi through r9 left them nine registers to dodge - which is more than
     * the six callee-saved ones they could then have, so the rest of the loop's values went to the
     * frame. What each syscall's own arguments cost is unchanged: the copies placing them write
     * those registers where the call stands, and writtenRegisters takes them from the shape.
     */
    {
        auto& syscall = convention[(Size)LowerCallType::Syscall];
        addGenRegs(syscall.args[BankGpr], kSyscallArgs);
        addGenRegs(syscall.results[BankGpr], kSyscallResults);

        syscall.preservesArgs = true;
        addGenClobber(syscall.clobber, kSyscallResults);
        syscall.clobber.add(gpr(IntRegister::rcx));
        syscall.clobber.add(gpr(IntRegister::r11));
        finish(syscall);
    }
}

const Constraints& targetConstraints() {
    static Constraints constraints;
    return constraints;
}

const CallConvention& Constraints::getConvention(LowerCallType type) const {
    assertTrue(type <= LowerCallType::LastType);
    return convention[(Size)type];
}

U32 argAreaBytes(const CallConvention& convention, const ArgLocationList& args) {
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

void shapeOf(LowerBase base, const MachineFunction& machine, const Constraints& constraints,
             LowerFunction& fun, LowerInst* inst, InstShape& shape) {
    shape.clear();

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

        return;
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
        return;
    }

    // Everything else takes its shape from the form selection chose for it. An operand beyond the
    // form's own list is unconstrained, which is what most operands of most instructions are.
    auto& form = machine.formOf(inst);
    shape.clobber = form.clobbers;

    auto constraintFor = [&](const Array<MachineOperandConstraint>& list, Size i) {
        if(i >= list.size()) return ArgLocation {};
        if(list[i].kind != OperandConstraintKind::FixedRegister) return ArgLocation {};
        return ArgLocation::inRegister(list[i].fixedReg);
    };

    for(Size i = 0; i < used.size(); i++) {
        // An operand the encoding swallowed occupies nothing, whatever the form says about the
        // position it would otherwise have held.
        auto value = base[used[i]];
        shape.uses.push(isImplicit(value) ? ArgLocation {} : constraintFor(form.uses, i));
    }

    for(Size i = 0; i < created.size(); i++) {
        shape.creates.push(isImplicit(&created[i]) ? ArgLocation {} : constraintFor(form.defs, i));
    }
}

/*
 * Memory operands. See the block comment on DirectMemoryChoice in gen.h.
 *
 * Which operand an encoding could take from memory is the form's answer. What is added here is the
 * half that depends on the value in it rather than on the instruction: an operand the encoding
 * already swallowed has no location at all, and a slot is exactly as wide as the value in it, so an
 * access at any other width would take a neighbouring value with it.
 */

static bool operandFitsMemoryForm(LowerBase base, const MachineForm& form, LowerInst* inst, I32 index) {
    auto used = inst->used();
    assertTrue(Size(index) < used.size());

    auto value = base[used[index]];
    if(isImplicit(value)) return false;

    return stackSlotClassFor(value->type) == stackSlotClassFor(operationType(base, form, inst));
}

DirectMemoryChoice directMemoryOperands(LowerBase base, const MachineFunction& machine, LowerInst* inst) {
    auto& form = machine.formOf(inst);
    DirectMemoryChoice out;

    auto applicable = [&](I32 index) {
        return index != kNoMemoryOperand && operandFitsMemoryForm(base, form, inst, index);
    };

    if(applicable(form.memoryUse())) out.read = form.memoryUse();

    // A read/write operand is written back through the same r/m field it is read from, so the result
    // has to be a value of its own rather than something the encoding swallowed.
    auto hasResult = inst->createdCount > 0 && !isImplicit(&inst->created()[0]);
    if(hasResult && applicable(form.memoryDef())) out.readWrite = form.memoryDef();

    return out;
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
