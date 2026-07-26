#pragma once

#include "Net/Buffer.h"
#include "Net/Stream.h"
#include "../../lower/lower_inst.h"
#include "target.h"
#include "machine.h"

static constexpr Size kMaxRegInputs = 16;

// Where one argument or result of a call is passed.
//
// This is the single place the register-versus-stack decision is made. The caller writes a stack
// argument into its outgoing area at `stackOffset`, the callee reads it from its incoming area at
// the same offset, and both get the answer from classifyArgs rather than deciding for themselves -
// which is what stops the two sides from disagreeing about where an argument went.
struct ArgLocation {
    enum Kind: U8 {
        None,     // unconstrained: the operand stays wherever the allocator put it
        Register, // a fixed register
        Stack,    // the argument area, at `stackOffset` bytes from its base
    };

    Kind kind = None;
    PhysicalReg reg;
    U32 stackOffset = 0;

    static ArgLocation inRegister(PhysicalReg reg) { return ArgLocation { Register, reg, 0 }; }
    static ArgLocation onStack(U32 offset) { return ArgLocation { Stack, PhysicalReg {}, offset }; }
};

// A calling convention, stated once and used from both sides.
//
// A machine form describes what one *instruction* does to the register file. This describes what a
// *call* does where it appears and what a function compiled with the convention owes the caller it
// returns to - the same contract seen from opposite ends, which is why constraint.cpp states both
// halves and checks them against each other rather than deriving either.
struct CallConvention {
    // The registers arguments and results are assigned to, per bank, in the order the convention
    // hands them out. An argument that runs past the end of its bank's list is passed in the
    // argument area instead; a result that does would have to be returned through memory, which
    // needs a hidden pointer argument the lowering does not produce, so classifyResults rejects it.
    struct BankRegs {
        PhysicalReg regs[kMaxRegInputs];
        Size count = 0;

        void add(PhysicalReg reg) {
            assertTrue(count < kMaxRegInputs);
            regs[count++] = reg;
        }
    };

    BankRegs args[kRegisterBankCount];
    BankRegs results[kRegisterBankCount];

    // What a call of this convention destroys, and what a function compiled with it has to give
    // back. rsp is preserved too, but it is reserved rather than handed out, so keeping it valid is
    // the frame code's business rather than the prologue's. rbp is in neither position: no
    // convention may clobber it (a caller may be holding a frame pointer there), so it is preserved
    // by every one of them and saved by the prologue of any function that takes it as a register.
    RegSet clobber;
    RegSet calleeSaved;

    // What rsp must be a multiple of at the point a call of this convention is executed, before the
    // call pushes its return address. A convention the compiler is on both sides of can leave this
    // at 8; an external one generally cannot, because its callees are entitled to assume the
    // alignment when they spill a vector register.
    U32 stackAlignment = 8;

    // Bytes the caller reserves below the first stack argument, for a convention that requires the
    // callee to have somewhere to spill its register arguments - Win64's shadow space. Zero
    // everywhere else.
    U32 shadowSpace = 0;

    // Win64 assigns argument registers by position rather than per bank: a float in argument
    // position 2 takes xmm2 and leaves r8 unused, so a callee can find any argument without knowing
    // the types of the ones before it. SysV and the compiler's own conventions count each bank
    // independently, filling rdi..r9 with integers however many floats came first.
    bool positionalArgs = false;

    // Set once the convention has tables to work from. A function or call using one that does not
    // has to be rejected: with empty tables every argument would silently be classified onto the
    // stack, which is a working compile of the wrong program.
    bool defined = false;
};

// Assigns every argument of a call its location, by walking the argument list in order and handing
// out registers of each class until the convention runs out. Both sides go through this - the caller
// to place its operands, the callee to find where its arguments arrived, the verifier to check both.
//
// `typeOf` is asked for the type of argument `i`, so a caller can classify straight out of whatever
// buffer it already has without building a type list first.
template<class F>
void classifyArgs(const CallConvention& convention, Size count, F&& typeOf, Array<ArgLocation>& out) {
    assertTrue(convention.defined); // a call using an undescribed convention

    Size taken[kRegisterBankCount] = {};
    auto stack = convention.shadowSpace;

    for(Size i = 0; i < count; i++) {
        auto bank = bankForType(typeOf(i));
        auto& table = convention.args[bank];

        // A positional convention indexes the table by argument position, so an argument of one
        // bank consumes the slot of every bank; a per-bank one keeps an independent counter each.
        auto index = convention.positionalArgs ? i : taken[bank];

        if(index < table.count) {
            out.push(ArgLocation::inRegister(table.regs[index]));
            taken[bank]++;
        } else {
            // Every stack argument occupies one 8-byte slot, in declaration order and lowest first,
            // which is what the callee's incoming offsets assume.
            out.push(ArgLocation::onStack(stack));
            stack += 8;
        }
    }
}

// The same for a call's results, which no described convention passes on the stack.
template<class F>
void classifyResults(const CallConvention& convention, Size count, F&& typeOf, Array<ArgLocation>& out) {
    assertTrue(convention.defined); // a call using an undescribed convention

    Size taken[kRegisterBankCount] = {};

    for(Size i = 0; i < count; i++) {
        auto bank = bankForType(typeOf(i));
        auto& table = convention.results[bank];
        auto index = taken[bank]++;

        assertTrue(index < table.count); // more results than the calling convention can return
        out.push(ArgLocation::inRegister(table.regs[index]));
    }
}

// Bytes of argument area a call needs: enough for the highest stack argument, plus any shadow space
// the convention asks for even when every argument fitted in a register, rounded up so that opening
// the area cannot knock rsp off the boundary the callee is entitled to expect.
U32 argAreaBytes(const CallConvention& convention, const Array<ArgLocation>& args);

// Byte count above which a Copy/SetPattern with a compile-time size stops being straight-lined into
// plain moves and takes the rep-prefixed string instruction instead. Chosen once, in
// transformFunction (see selectBlockOpEncoding), and recorded on the instruction.
static constexpr U64 kMaxUnrolledMemOp = 32;

// The calling conventions, which are the one part of an instruction's register behaviour that a
// machine form cannot state for itself: where a call's arguments go depends on how many of each bank
// came before them, which a fixed operand list cannot say. Everything else - fixed registers, ties,
// clobbers, memory alternatives, flags - is in the selected MachineForm.
struct Constraints {
    Constraints();
    const CallConvention& getConvention(LowerCallType type) const;

private:
    CallConvention convention[(Size)LowerCallType::LastType + 1];
};

// The conventions, built once. They are constant and the same for every function, and each of the
// three passes that reads them used to construct its own copy.
const Constraints& targetConstraints();

/*
 * Instruction shapes.
 *
 * Where each operand and result of one instruction has to be is worked out once, into an InstShape,
 * and then read back by index. Two sources feed it, and neither is consulted anywhere else: the
 * selected machine form, for an ordinary instruction whose encoding forces particular registers, and
 * the calling convention, for a call, a syscall or a return, whose operand placement depends on how
 * many arguments of each bank came before.
 *
 * Entry N is operand N, which the sources it comes from do not guarantee for themselves: a form
 * states only the operands it has something to say about, and a convention skips the operands that
 * are not arguments at all. Every caller used to re-derive that mapping with its own copy of the
 * rule, which is how the allocator and the verifier could disagree about it.
 */

struct InstShape {
    // Parallel to the instruction's own used()/created() buffers, so operand N is entry N.
    Array<ArgLocation> uses;
    Array<ArgLocation> creates;

    // Registers the instruction writes behind its operands' backs.
    RegSet clobber;

    // The convention a call, a syscall or a return follows, for the callers that need more of it
    // than the operand locations: the argument area's size and alignment, and the preserved set.
    const CallConvention* convention = nullptr;

    // A return's operands are constrained like its convention's *results* rather than its
    // arguments, and nothing is live once the function has returned - so a return neither clobbers
    // anything nor has anything left to protect.
    bool isReturn = false;
};

InstShape shapeOf(LowerBase base, const MachineFunction& machine, const Constraints& constraints, LowerFunction& fun, LowerInst* inst);

// The fixed register operand `i` has to be in when the instruction executes, if any. A stack-passed
// argument has no register and answers an invalid location here, so a caller that needs to tell the
// two apart reads shape.uses[i] instead.
inline MachineLocation wantForUse(const InstShape& shape, Size i) {
    auto& location = shape.uses[i];
    return location.kind == ArgLocation::Register ? MachineLocation::physical(location.reg) : MachineLocation::invalid();
}

// The fixed register result `i` is produced in, if any.
inline MachineLocation wantForResult(const InstShape& shape, Size i) {
    auto& location = shape.creates[i];
    return location.kind == ArgLocation::Register ? MachineLocation::physical(location.reg) : MachineLocation::invalid();
}

// Every register this instruction writes behind the operands' backs: the ones it clobbers, plus the
// ones the parallel copy in front of it writes to satisfy fixed-register constraints. A value that
// has to survive the instruction, and an operand that isn't itself placed by that parallel copy,
// both have to stay out of these.
RegSet writtenRegisters(const InstShape& shape);

/*
 * Memory operands.
 *
 * A value that lives in the frame normally has to be brought into a register before anything can
 * read it. Most x86 ALU instructions have a form that reads one operand straight out of memory
 * instead, which removes the reload entirely - `add rax, [slot]` in place of a load and an add.
 *
 * Which operand that is, if any, is the *selected form's* answer (`memoryUse`/`memoryDef` on
 * MachineForm). What is added here is the half the form cannot state, because it depends on the value
 * rather than on the instruction: an operand the encoding already swallowed has no location at all,
 * and a slot is exactly as wide as the value in it, so an access at any other width would take a
 * neighbouring value with it.
 *
 * That is the whole of this: **allocation-dependent applicability of form data**, not a second table
 * of instruction properties. One call answers both roles at once so that placement's costing,
 * legalization and the verifier consume one result rather than asking twice and having to remember
 * that the two are mutually exclusive.
 */

// The operand index for a role no form of this instruction offers.
static constexpr I32 kNoMemoryOperand = -1;

// Which of `inst`'s operands may stay in a frame slot, by role, as indices into its used() buffer.
//
// At most one of each, and at most one *overall* at any given instruction: a general memory operand
// occupies the r/m field and there is one of those, which validateMachineForms checks per form and
// which is why the two roles are answered together rather than composed by the caller.
//
//   `read`      the operand an encoding can take from memory outright - `add rax, [slot]` in place of
//               a reload and an add. Applicable on its own.
//
//   `readWrite` the operand a destructive encoding reads *and writes* through the same r/m field -
//               `add [slot], rcx` rather than `add rax, [slot]` - so it is always operand zero where
//               it answers at all. Not applicable on its own: the operand and the result also have to
//               occupy the same slot, which only the allocator can say. `inPlaceAt` is that question.
struct DirectMemoryChoice {
    I32 read = kNoMemoryOperand;
    I32 readWrite = kNoMemoryOperand;

    bool hasRead() const { return read != kNoMemoryOperand; }
    bool hasReadWrite() const { return readWrite != kNoMemoryOperand; }
};

DirectMemoryChoice directMemoryOperands(LowerBase base, const MachineFunction& machine, LowerInst* inst);

// Whether the read/write role is actually taken: the form offers it, and the operand and the result
// turn out to be in the same slot - which is the half only the allocator can answer, and the reason
// `readWrite` alone is not applicability. Two things produce it. The operand's life ends at the point
// the result's begins, so first-fit hands them one slot whenever it can; and phi-web coalescing makes a
// loop-carried accumulator and the value computed for the next iteration literally one web.
//
// Asked once by legalization of the homes placement gave them, and again by the verifier of the
// locations legalization resolved them to. Placement's costing asks it before either exists, and so
// asks of the webs instead - see isInPlace in place.cpp.
inline bool takesInPlace(const DirectMemoryChoice& choice, MachineLocation operand, MachineLocation result) {
    return choice.hasReadWrite() && operand.isStack() && operand == result;
}

/*
 * Addresses.
 *
 * One address representation, and one encoder for it. Every memory reference this backend emits - a
 * folded X86Address, a pointer sitting in a register, an outgoing argument store, a RIP-relative
 * global - is resolved into one of these by legalization and written out by the shared encoder in
 * gen.cpp. Nothing else writes a ModRM byte for an address.
 *
 * That matters because the special cases are not obvious and are all silent when wrong: rsp and r12
 * can only be a base through a SIB byte, rbp and r13 have no displacement-free encoding, a missing
 * base is a SIB form of its own, and REX.B/REX.X extend the base and index independently.
 *
 * A frame slot is the one memory reference not described here: its address is not known until frame
 * layout has run, so it stays a location and the encoder builds the address from the layout.
 */

// A complete AMD64 memory reference: `[base + index*scale + displacement]`, `[rip + displacement]`,
// or any legal subset of that. Registers here are physical general-purpose register numbers -
// allocation and legalization are both over by the time an address reaches emission.
struct MachineAddress {
    bool hasBase = false;
    bool hasIndex = false;

    // `[rip + disp32]`, whose displacement is only known once every function and global has been
    // emitted, so it is written as a relocation rather than as bytes.
    bool ripRelative = false;

    U8 base = 0;
    U8 index = 0;
    U8 scale = 1; // 1, 2, 4 or 8 - the only scalings the SIB byte encodes
    I32 displacement = 0;

    // Set instead of `displacement` when the address names something whose offset is not known yet.
    // Exactly one of the two may be set, and only on a RIP-relative address.
    LowerFunction* relocFunction = nullptr;
    LowerGlobal* relocGlobal = nullptr;

    // `[reg]` - a pointer the allocator left in a register.
    static MachineAddress atRegister(U8 base) {
        return MachineAddress { .hasBase = true, .base = base };
    }

    // `[reg + displacement]` - a frame object, or a fixed offset inside one.
    static MachineAddress atOffset(U8 base, I32 displacement) {
        return MachineAddress { .hasBase = true, .base = base, .displacement = displacement };
    }

    // `[rip + symbol]`, resolved by AsmModule::resolveRelocations once everything has been emitted.
    static MachineAddress atSymbol(LowerFunction* function, LowerGlobal* global) {
        return MachineAddress { .ripRelative = true, .relocFunction = function, .relocGlobal = global };
    }
};

/*
 * Legalized instructions.
 *
 * What legalization decided each instruction does with the placement it was given: where every
 * operand is read from, where every result is written, and the copies that bridge the difference
 * between those and where the values live the rest of the time.
 *
 * `LowerValue` intentionally has no `.reg` field - the result is a whole-function mapping that the
 * encoder consumes positionally, and threading it through the IR would put target-specific state on
 * a target-independent structure. Instead there is one `InstRegs` record per instruction, which the
 * encoder in gen.cpp consumes in lockstep with its own instruction walk.
 *
 * The operands here are resolved: a physical register, a frame slot the selected form has a memory
 * alternative for, the value of an immediate the encoding carries, or nothing at all for one the
 * encoding swallowed. The encoder reads these and the selected form and nothing else - it never
 * looks at the instruction to work out what shape an operand has, because that question was
 * answered by selection and by placement, each exactly once.
 */

// One operand of one instruction, as emission sees it.
struct ResolvedOperand {
    // Where the operand is at this instruction: a physical register, a frame slot, or a recipe.
    // Invalid for an operand that occupies no location - an immediate the encoding carries, an
    // address folded into a ModRM byte, a comparison consumed as flags.
    MachineLocation at;

    // Which register class this operand is read or written *as*, which a location does not say: a
    // location names the physical register, and the class is what turns it into the view an encoder
    // writes - `eax` as against `rax`, `xmm3` as against `zmm3`. Meaningless for an operand that
    // occupies no location.
    //
    // It is here rather than derived at emission because deriving it means reading the operand's
    // type back out of the IR, which is the one thing emission is not supposed to do - and because
    // the index alone is the same number for every bank, so a class from the wrong one is silent.
    RegisterClassId regClass = ClassGpr64;

    // The value an immediate operand carries. Resolved here rather than read out of the IR by the
    // encoder, which is what keeps "is this operand an immediate" a question the selected form
    // already answered.
    U64 immediate = 0;
    bool isImmediate = false;

    static ResolvedOperand none() { return ResolvedOperand {}; }

    static ResolvedOperand location(MachineLocation at, RegisterClassId regClass) {
        return ResolvedOperand { at, regClass };
    }

    static ResolvedOperand constant(U64 value) {
        return ResolvedOperand { MachineLocation::invalid(), ClassGpr64, value, true };
    }

    bool isValid() const { return at.isValid(); }
    bool isPhysical() const { return at.isPhysical(); }
    bool isStack() const { return at.isStack(); }
    bool isRemat() const { return at.isRemat(); }

    // The register this operand names, at the width the encoder writes it.
    RegisterView view() const {
        return targetRegisters().viewOf(regClass, at.physicalReg());
    }

    // Deliberately no equality: two operands are compared through `at`, because "the same place" is
    // the only sense in which two of them are ever the same thing.
};

// One step of a location permutation: a copy between two locations, of a value of one class.
//
// The class is what says which instruction the copy is - a bank alone does not, since two classes
// over one register file need not move at the same width, and a class narrower than its register
// need not preserve what the rest of it held. `swap` marks the entry as an exchange rather than a
// copy: sequencing a parallel copy whose sources and destinations overlap cyclically needs one, and
// where the class has an exchange instruction (GPR `xchg`) it costs no scratch register at all.
struct RegMove {
    MachineLocation from;
    MachineLocation to;
    RegisterClassId regClass = ClassGpr64;
    bool swap = false;
};

// Whether a cycle in a parallel copy of this class can be broken with an exchange instruction, or
// has to go through a scratch register. Answered from the same move table emission writes the bytes
// from (gen.cpp), so the sequencer cannot ask for an exchange no encoder has.
bool classHasExchange(RegisterClassId regClass);

// Resolved locations - physical registers, or frame slots where the encoding has a memory form -
// for a single instruction. `uses`/`creates` are parallel to that instruction's `used()`/`created()`
// buffers, in the same order, and name where the encoder will find (or put) each operand *at this
// instruction*, which is not necessarily where the value lives the rest of the time.
struct InstRegs {
    Array<ResolvedOperand> uses;
    Array<ResolvedOperand> creates;

    // The one memory address this instruction references, for the forms whose encoding has an
    // address field. At most one: a ModRM byte addresses one thing, and a frame slot - the other
    // kind of memory operand - is named by a location instead, since its address is not known until
    // frame layout has run.
    MachineAddress address;
    bool hasAddress = false;

    // Moves emitted immediately before the instruction: they bring operands from their home
    // registers into the places this instruction requires (fixed-register constraints, or the
    // destination of a destructive two-address encoding), and carry values into a successor's phi
    // registers at a terminator. Already sequenced - emit them in order.
    Array<RegMove> moves;

    // Moves emitted immediately after the instruction, carrying a result out of the fixed register
    // the encoding had to write it to and into its home.
    Array<RegMove> postMoves;
};

// Register assignments for every instruction in one block, in the order:
// block->instructions (in order), followed by exactly one entry for block->terminator.
struct BlockRegs {
    Array<InstRegs> insts;
};

/*
 * Frame objects.
 *
 * Anything the function keeps on the stack is an *abstract* slot while the allocator is running:
 * the allocator decides that a value needs stack space, and frame layout decides afterwards where
 * that space is, because the answer depends on things the allocator does not know yet - how many
 * registers ended up needing saving, whether a frame pointer is required, how much alignment the
 * calls in the function demand.
 *
 * A slot is named by a `MachineLocation` of kind `StackSlot` whose index is its `StackSlotId`, so a value
 * living on the stack and a value living in a register are the same kind of thing everywhere the
 * allocator handles locations - and neither can be mistaken for the other, since the kind says which
 * it is. `StackSlotId`, `StackSlotClass` and `stackSlotClassFor` are in target.h with the rest of
 * the location model.
 */

static constexpr StackSlotId kInvalidSlot = 0xffff;

// What a slot is for. Frame layout puts each kind in its own region, because they answer to
// different rules: incoming arguments live in the *caller's* frame above the return address and
// cannot be moved, locals have to keep their addresses for as long as the function runs, and spill
// slots are the only ones that may be shared between values whose lives do not overlap.
enum class StackSlotKind: U8 {
    Spill,
    Local,       // a fixed-size alloca
    IncomingArg, // an argument the caller left on the stack
};

struct StackSlot {
    StackSlotKind kind = StackSlotKind::Spill;
    StackSlotClass slotClass = StackSlotClass::Slot64;
    U32 size = 8;
    U32 alignment = 8;

    // For an IncomingArg, its byte offset within the argument area, which is what fixes its
    // address: the caller wrote it there and the convention decided where there is. Unused for the
    // kinds this frame places itself.
    U32 argOffset = 0;
};

// A reference to a frame object from an instruction, before frame layout has run. The addend allows
// a reference into the middle of a slot - an element of a local array, the second half of a value
// spilled as two words.
struct FrameReference {
    StackSlotId slot = kInvalidSlot;
    I32 addend = 0;
};

// Everything the function puts on the stack, collected while registers are being allocated and
// consumed by frame layout. Nothing here has an address yet.
struct FrameObjects {
    Array<StackSlot> slots;

    // Frame objects individual instructions refer to: an alloca's local, and later a spilled
    // operand's slot. Keyed by instruction because the reference belongs to the instruction rather
    // than to any value it produces.
    HashMap<LowerInst*, FrameReference> references;

    // Set by an alloca whose size is not known until the function runs. Such a function has to move
    // rsp at runtime, so every fixed frame object needs an address that survives that - which means
    // a frame pointer, whatever the frame-pointer mode asks for.
    bool hasDynamicAlloca = false;

    // Bytes of outgoing argument area: enough for the call in this function that passes the most on
    // the stack, and zero for a function whose calls all fit in registers.
    //
    // The area is reserved once by the prologue rather than opened and closed around each call, and
    // it is always the lowest part of the frame - a callee looks for its stack arguments at the
    // stack pointer, so nothing may sit between them and it. Reserving it once is what keeps rsp
    // still for the whole body, which in turn is what lets a frame be addressed through rsp at all.
    U32 argAreaSize = 0;

    // The largest alignment any call in this function requires of rsp. The frame is padded so that
    // the prologue leaves rsp on that boundary.
    U32 callAlignment = 8;

    StackSlotId add(StackSlot slot) {
        slots.push(slot);
        return StackSlotId(slots.size() - 1);
    }

    bool isEmpty() const { return slots.isEmpty(); }
};

/*
 * Rematerialization.
 *
 * A value cheap enough to recompute does not need to be kept anywhere. Instead of a register or a
 * frame slot, its web is given a *recipe*: the one instruction that recreates it, which is emitted
 * afresh into a scratch register at each instruction that reads it. The definition itself then emits
 * nothing at all, and the value occupies no location between its uses - which is the point, since
 * the values this applies to are exactly the ones whose live ranges are long and whose contents
 * never change.
 *
 * A recipe has to be reproducible at every point the value is live: side-effect free, independent of
 * anything the program can write, and legal wherever it lands. All four kinds below are constants in
 * that sense - an immediate, the address of a global or a function, and the address of a fixed frame
 * object, which is a constant offset from a base register the frame keeps valid for the whole
 * function.
 *
 * A recipe is named by a MachineLocation of kind Rematerializable whose index is its position in
 * FunctionRegs::remats, so a rematerializable value, a value in a slot and a value in a register are
 * the same kind of thing everywhere a location is handled.
 */
struct Remat {
    enum Kind: U8 {
        Immediate,       // mov r, imm
        GlobalAddress,   // lea r, [rip + global]
        FunctionAddress, // lea r, [rip + function]
        FrameAddress,    // lea r, [base + slot]
    };

    Kind kind = Immediate;
    LowerType type = LowerType::Int64;

    U64 imm = 0;                        // Immediate
    LowerGlobal* global = nullptr;      // GlobalAddress
    LowerFunction* function = nullptr;  // FunctionAddress
    FrameReference frame;               // FrameAddress
};

/*
 * Placement.
 *
 * Where each value lives between the instructions that touch it, which is the allocation proper.
 * The per-instruction InstRegs above are what the encoder needs in order to emit it, and say where
 * an operand sits *at one instruction*, which is not always the same place.
 *
 * Placement is a pass of its own (place.cpp) and runs to completion over the whole function before
 * any instruction record exists. That is what lets it think again about a web it has already
 * placed: nothing has been published that would have to be rebuilt, so a displacement is a decision
 * inside placement rather than a reason to start the function over.
 *
 * It is over *webs* rather than over values. A phi and the values that feed it are one quantity
 * under several SSA names, and giving all of them one location makes the copy between them an
 * identity that is never emitted. `webOf` says which web a value belongs to; the web holds the
 * location.
 *
 * A web's location is a list of *segments* - a location and the stretch of program points over
 * which it holds. Every web has exactly one segment today, covering the whole of its life, which is
 * what makes the result independent of block layout: a value is in the same place on every path
 * that reaches a given instruction. The list is what persistent splitting adds to, and it is here
 * now so that splitting extends the allocation result rather than replacing it.
 */

struct AllocationSegment {
    // Program points, half-open - see beforeInst/afterInst in lower.h.
    U32 from = 0;
    U32 to = 0;

    MachineLocation location;
};

struct WebAllocation {
    // Sorted and disjoint, and together covering every point the web is live at. Empty for a web
    // that never needed a location at all.
    Array<AllocationSegment> segments;

    // The location this web occupies at `point`. The point is not consulted while a web has a
    // single segment - it is in the signature because that is the question callers should be
    // asking, and the one that gets a different answer once a web's life is split.
    MachineLocation locationAt(U32 point) const {
        assertTrue(segments.size() <= 1); // a split web, which nothing produces yet
        return segments.isEmpty() ? MachineLocation::invalid() : segments[0].location;
    }
};

struct Placement {
    // Which web each value belongs to, indexed by the dense LiveId buildLiveness assigns, and the
    // webs themselves. A web is named by the LiveId of its representative, so the two are indexed
    // alike and a value's location is one lookup away.
    Array<LiveId> webOf;
    Array<WebAllocation> webs;

    // Everything the function needs stack space for - see FrameObjects.
    FrameObjects frame;

    // The recipes for the webs that live nowhere - see Remat. A location of kind Rematerializable
    // indexes this, and every one of them belongs to exactly one web.
    Array<Remat> remats;

    // Where each of the function's arguments arrives, in argument order: the register the
    // convention delivered it in, or the incoming frame object the caller left it in. Invalid for
    // an argument the encoding swallowed. Recorded here because the frame object is placement's to
    // create, and legalization needs to name the same one when it emits the entry copies.
    Array<MachineLocation> incomingArgs;

    // Every register placement decided the function writes: the ones handed out to webs, and the
    // ones instructions clobber or are forced to write behind a value's back. Legalization adds the
    // scratch registers it hands out, and the two together are what the prologue has to save.
    RegSet writtenPhysical;

    // Set when a web ended up with no register at all. A value that is not in one has to be brought
    // into a scratch register at each instruction that touches it, and those are reserved for the
    // whole function rather than found after the fact - so this asks for the reserve to be measured
    // and, if it grew, for one more placement pass with it held back. See allocateRegisters.
    bool requiresLegalizationTemps = false;

    // Webs this pass would rather have displaced than left the web that asked for their register
    // homeless. Both are requests from placement to placement - a displaced web chooses a recipe or
    // a slot for itself on the next pass, which is why this asks for it to be left *homeless* rather
    // than spilled. Applied to the next placement pass; see `assign` in place.cpp.
    Array<LiveId> displacementRequests;

    Size valueCount() const { return webOf.size(); }

    // The location holding `id` at program point `point`, invalid for a value that never needed
    // one.
    MachineLocation locationOf(LiveId id, U32 point) const {
        return id < webOf.size() ? webs[webOf[id]].locationAt(point) : MachineLocation::invalid();
    }

    MachineLocation locationOf(LowerValue* v, U32 point) const {
        auto id = v->liveId();
        assertTrue(id != kNullLive); // every non-implicit value is numbered by buildLiveness
        return locationOf(id, point);
    }
};

// The instruction records legalization produced: one InstRegs per instruction and terminator of
// every block, in emission order. See legalize.cpp.
struct LegalizedFunction {
    HashMap<LowerBlock*, BlockRegs> blocks;

    // The scratch registers legalization actually handed out. Placement does not know which of them
    // an instruction will need - that is the question legalization answers - so the two halves of
    // "what does this function write" are added together once both have run.
    RegSet writtenPhysical;
};

// The whole allocation of one function: where every value lives, and what each instruction does
// with that. Produced by allocateRegisters() and consumed by genFunction().
struct FunctionRegs {
    // Where every value lives - see Placement.
    Placement placement;

    // What each instruction reads and writes, given that - see LegalizedFunction.
    LegalizedFunction legalized;

    // Callee-saved registers this function writes, and therefore has to save on entry and restore
    // before every return. Empty for a function that stayed inside its convention's clobber set,
    // which is the common case for a leaf function.
    RegSet usedCalleeSaved;

    // Whether this function establishes rbp as a frame pointer, decided from the IR before
    // allocation ran (functionNeedsFramePointer) and carried here so that frame layout uses the
    // same answer the allocator did. False means rbp was allocatable and may hold a value; the two
    // must never disagree, since the frame is addressed through rbp exactly when this is set.
    bool framePointer = false;

    // The scratch registers this function held back, which is what legalization handed out from - see
    // TemporaryReserve. Carried here because it is part of the allocation: the registers in it are
    // ones no web was offered, and a reader of the result that assumed a fixed set would disagree
    // with the pass that chose it.
    TemporaryReserve temporaries;
};

/*
 * Frame layout.
 *
 * Runs after allocation, once everything the frame has to hold is known, and turns the abstract
 * slots above into concrete displacements from a base register. This is the only place that knows
 * what the stack looks like; the encoders ask it for an address and emit one.
 *
 * With a frame pointer the layout is
 *
 *     [rbp + 16 + n]   incoming stack argument at offset n
 *     [rbp + 8]        return address
 *     [rbp]            caller's rbp
 *     [rbp - 8k]       saved callee-saved registers
 *     [rbp - ...]      locals and spill slots
 *     [rsp + n]        outgoing argument area                <- rsp after the prologue
 *
 * and without one the same objects hang off rsp instead, which works because rsp then stays put for
 * the whole body - the argument area is reserved once by the prologue rather than opened around
 * each call, and a function that moves rsp any other way (a dynamic alloca) has a frame pointer.
 *
 * The outgoing area is the one thing always addressed through rsp rather than through `base`: a
 * callee finds its stack arguments at the stack pointer, so the area has to stay at the bottom even
 * in a function whose rsp moves. A dynamic alloca therefore re-establishes it below the memory it
 * allocated (see genAlloca).
 *
 * A function that needs rsp on a stronger boundary than its own entry convention promises - one that
 * calls SysV from a convention aligned to 8, or that allocates an over-aligned local - cannot get
 * there by padding: padding preserves an offset from an entry that was never aligned in the first
 * place. It has to *realign*:
 *
 *     [rbp + 16 + n]   incoming stack argument at offset n
 *     [rbp + 8]        return address
 *     [rbp]            caller's rbp
 *     [rbp - 8k]       saved callee-saved registers
 *                      <- and rsp, -alignment: aligned here, by an amount only known at run time
 *     [rsp + ...]      locals and spill slots
 *     [rsp + n]        outgoing argument area                <- rsp after the prologue
 *
 * The realignment splits the frame in two, and the two halves are addressed through different
 * registers - which is what `slotBase` is for. Everything below the mask hangs off the now-aligned
 * rsp, so a local is on its own boundary because the region it sits in is; the incoming arguments are
 * above it and keep their fixed distance from rbp, since nothing can be said about the distance from
 * rsp to them any more. The epilogue recovers rsp from rbp, which it already does whenever there is a
 * frame pointer - so realigning requires one, exactly as a dynamic alloca does.
 *
 * A dynamic alloca and a realignment are the one combination not supported: the alloca moves rsp out
 * from under the locals the realignment put there, and keeping them reachable would take a third base
 * register held for the whole function. computeFrameLayout rejects it rather than emitting a frame
 * whose locals move.
 */
struct FrameLayout {
    // Callee-saved registers the prologue pushes, in ascending register order.
    RegSet savedRegs;

    // Set when rbp is established as the base for fixed frame objects. Costs a push, a move and a
    // register; see FramePointerMode for when it is worth it.
    bool framePointer = false;

    // The register the frame as a whole is measured from: rbp when there is a frame pointer, rsp when
    // there is not. A realigning frame measures its locals from rsp instead - see slotBase.
    PhysicalReg base;

    // Bytes the prologue subtracts from rsp: the outgoing argument area, the locals and spill
    // slots, and any padding needed to leave rsp on the boundary the calls in this function require.
    U32 fixedSize = 0;

    // Bytes of that reserved for outgoing arguments, at the very bottom. An outgoing argument at
    // convention offset n is at [rsp + n], and a dynamic allocation has to leave this much below
    // itself so that the next call still finds it there.
    U32 argAreaSize = 0;

    // Set when the prologue has to align rsp itself rather than inherit an alignment from its caller,
    // because something in the body needs a stronger boundary than the entry convention promises -
    // see the picture above. Requires a frame pointer, since the distance from rsp back to the
    // incoming arguments is then only known at run time.
    bool realignsStack = false;

    // The boundary rsp is kept on: what a realignment masks to, and what a dynamic allocation rounds
    // its size up to so that moving rsp at run time preserves it.
    U32 dynamicAlignment = 8;

    // Displacement from `slotBase[i]` for each slot, indexed by StackSlotId.
    Array<I32> slotOffset;

    // The register each slot's displacement is measured from. One per slot rather than one per frame,
    // because a realigning frame has two: its locals hang off the aligned rsp and its incoming
    // arguments keep their distance from rbp, and the mask between them is exactly what makes the
    // distance from one to the other unknown until run time.
    Array<PhysicalReg> slotBase;

    // Whether the function needs any prologue at all.
    bool isEmpty() const { return savedRegs.isEmpty() && !framePointer && fixedSize == 0; }

    I32 offsetOf(FrameReference ref) const {
        assertTrue(ref.slot < slotOffset.size());
        return slotOffset[ref.slot] + ref.addend;
    }

    PhysicalReg baseOf(FrameReference ref) const {
        assertTrue(ref.slot < slotBase.size());
        return slotBase[ref.slot];
    }
};

FrameLayout computeFrameLayout(Context& ctx, LowerBase base, LowerFunction& fun, const Constraints& constraints, const FunctionRegs& regs);

// Whether this function establishes a frame pointer, which decides whether rbp is a register the
// allocator may hand out. Answered from the IR and the settings alone, so that it can be asked
// before allocation starts and its answer given to both the allocator and frame layout - see the
// comment at the top of frame.cpp.
bool functionNeedsFramePointer(Context& ctx, LowerBase base, LowerFunction& fun);

// Whether this function has to align rsp itself, because something in it needs a stronger boundary
// than its own entry convention promises. Answered from the IR for the same reason the frame-pointer
// question is: realigning requires a frame pointer, so the two have to be decided together and before
// the allocator is told which registers it may hand out.
bool functionRealignsStack(LowerBase base, LowerFunction& fun, const Constraints& constraints);

// Checks that the offsets a layout produced describe a frame its objects fit in, and that no two of
// them land on the same bytes. Both failures corrupt memory rather than producing a visibly wrong
// register, so neither shows up in a golden; genFunction runs this in debug builds.
bool verifyFrameLayout(Context& ctx, LowerFunction& fun, const FrameObjects& objects, const FrameLayout& layout);

struct AsmBlock {
    LowerBlock* block;
    U32 startOffset;
    U32 endOffset;
};

// A reference to a not-yet-known code offset (a block start or a function entry point) that
// needs to be patched into the instruction stream once all code has been emitted.
// Used for jump/call targets and RIP-relative global/function address loads.
struct AsmRelocation {
    // Offset in the buffer of the 4-byte rel32 field to patch.
    // The patched value is `symbolOffset - (siteOffset + 4)`, i.e. relative to the byte
    // immediately following the field (matching how the CPU computes RIP-relative offsets).
    U32 siteOffset;

    // Resolution target: exactly one of these is set.
    // `function` is used for calls/address-loads that target a (possibly not-yet-emitted)
    // function elsewhere in the module; `block` is used for intra-function jumps; `global` is
    // used for address-loads of module-level data (see AsmModule::addGlobal).
    LowerFunction* function = nullptr;
    LowerBlock* block = nullptr;
    LowerGlobal* global = nullptr;
};

struct AsmModule {
    explicit AsmModule(Size initialSize = 4096): buffer(initialSize) {}

    Net::BufferWriter buffer;
    Array<AsmBlock> blocks;
    Array<AsmRelocation> relocations;
    HashMap<LowerBlock*, U32> blockOffsets;
    HashMap<LowerFunction*, U32> functionOffsets;
    HashMap<LowerGlobal*, U32> globalOffsets;

    void startBlock(LowerBlock* block) {
        auto b = blocks.push(AsmBlock {
            .block = block,
            .startOffset = U32(buffer.offset()),
            .endOffset = 0,
        });

        blockOffsets.add(block, U32(b - blocks.begin()));
    }

    void endBlock(LowerBlock* block) {
        auto b = blockOffsets.getValue(block);
        assertTrue(b.isJust());

        blocks[b.unwrap()].endOffset = U32(buffer.offset());
    }

    void startFunction(LowerFunction* fun) {
        functionOffsets.add(fun, U32(buffer.offset()));
    }

    // Appends a global's data to the buffer and records the offset its address-loads resolve to.
    // Globals are emitted into the same flat buffer as code (this is not an object-file writer -
    // there are no sections), so callers should emit all functions first and all globals after,
    // keeping executable and non-executable bytes from interleaving. Offsets are 16-byte aligned
    // so that a global is never split across a cache line by whatever preceded it.
    void addGlobal(LowerGlobal* global) {
        while(buffer.offset() & 15) buffer.writeByte(0);

        globalOffsets.add(global, U32(buffer.offset()));
        buffer.writeBytes(global->initialContents.data(), global->initialContents.size());
    }

    // Records a placeholder relocation at the rel32 field about to be written at the buffer's
    // current offset, then writes a placeholder 0 in its place. Call resolveRelocations() once
    // all functions referenced by any relocation have been emitted.
    void addRelocation(LowerBlock* target) {
        relocations.push(AsmRelocation { .siteOffset = U32(buffer.offset()), .block = target });
        buffer.writeInt<LittleEndian>(0);
    }

    void addRelocation(LowerFunction* target) {
        relocations.push(AsmRelocation { .siteOffset = U32(buffer.offset()), .function = target });
        buffer.writeInt<LittleEndian>(0);
    }

    void addRelocation(LowerGlobal* target) {
        relocations.push(AsmRelocation { .siteOffset = U32(buffer.offset()), .global = target });
        buffer.writeInt<LittleEndian>(0);
    }

    // Patches every recorded relocation with the now-known offset of its target.
    // Must be called after every block/function referenced by any relocation has been emitted.
    void resolveRelocations();
};

// Represents an address calculation (base + index * scale) + displacement.
// Used with two different instruction kinds:
//  - X86Address: purely embedded into whatever instruction uses it (Load/Store) - never
//    materialized into a register of its own, so its result is always Implicit.
//  - X86Lea: materializes the computed address into a real register (LEA), e.g. for pointer
//    arithmetic that doesn't immediately feed a Load/Store.
struct LowerInstX86Address: LowerInstSingle {
    LowerInstX86Address(LowerInst::Kind kind, U32 name, LowerPtr<LowerValue> base, LowerPtr<LowerValue> index, U8 scale, U32 displacement):
        LowerInstSingle(kind, name, LowerType::Pointer),
        first(base ? base : index), second(base && index ? index : nullptr),
        displacement(displacement), scale(scale),
        hasBase(base != nullptr), hasIndex(index != nullptr)
    {
        assertTrue(kind == LowerInst::X86Address || kind == LowerInst::X86Lea);

        usedCount = U8((hasBase ? 1 : 0) + (hasIndex ? 1 : 0));

        if(kind == LowerInst::X86Address) {
            result.flags |= LowerValue::Implicit;
        }
    }

    // The operand slots, named by position rather than by role. used() is one contiguous buffer, so
    // an address with no base - the no-base SIB form, `[index*scale + disp32]` - holds its index in
    // the first slot: a hole where the absent base would have been is a null operand that every
    // consumer walking used() would dereference. Read them through base() and index() below.
    LowerPtr<LowerValue> first, second;

    U32 displacement;
    U8 scale;
    bool hasBase;
    bool hasIndex;

    LowerPtr<LowerValue> base() const { return hasBase ? first : nullptr; }
    LowerPtr<LowerValue> index() const { return hasIndex ? (hasBase ? second : first) : nullptr; }
};

// Runs the target transform pipeline over `fun` in place - see the pipeline table at the bottom of
// transform.cpp for the passes and the order. `ctx` is only used to name the function in the
// between-pass invariant checks, which run in debug builds.
void transformFunction(Context& ctx, LowerBase base, LowerFunction& fun, MachineFunction& machine);

/*
 * The allocation pipeline.
 *
 * `allocateRegisters` (register.cpp) is the driver: it runs placement until it stops asking for
 * another pass, and legalizes the result once.
 *
 *   computePlacement   where every web lives, and nothing else. Runs over the whole function
 *                      without constructing a single instruction record, so a web it wants back can
 *                      simply be placed again.
 *   legalizeFunction   what each instruction does with that: which location every operand is read
 *                      from, where every result is written, and the copies that bridge the two.
 *
 * The split is the point. Placement answers "where does this value persist", legalization answers
 * "where must it be at this instruction", and neither answers the other's question.
 */

// One complete placement of a function. `framePointer` and `temporaries` are what is held back from
// every web - rbp when the frame is addressed through it, and the scratch registers legalization is
// going to need - and `forcedHomeless` names the webs a previous pass asked to be left homeless.
Placement computePlacement(LowerBase base, LowerFunction& fun, Liveness& live, const MachineFunction& machine,
    const Constraints& constraints, bool framePointer, const TemporaryReserve& temporaries,
    const Array<bool>& forcedHomeless);

// Resolves every instruction against a completed placement, handing out scratch registers from
// `temporaries` - which has to be one measureTemporaryReserve produced for this same placement.
LegalizedFunction legalizeFunction(LowerBase base, LowerFunction& fun, const MachineFunction& machine,
    const Constraints& constraints, const Placement& placement, const TemporaryReserve& temporaries);

// How many scratch registers legalizing this placement will need, by bank and by pool. Answered by
// legalizing it and recording what was asked for, rather than by a second rule that mirrors the
// first: the two would be a pair of answers to one question, and the one that is wrong is the one
// that leaves an instruction with nowhere to bring a spilled operand.
TemporaryReserve measureTemporaryReserve(LowerBase base, LowerFunction& fun, const MachineFunction& machine,
    const Constraints& constraints, const Placement& placement);

// Where the encoder reads one operand, which is the question legalization exists to answer. It is
// declared here because placement asks it too: a destructive result must not be placed in a
// register one of its instruction's other operands is still to be read from, and where those are
// read from is this same rule.
//
// The answer is a location, unless the operand has to be brought into a scratch register first - in
// which case the caller says which one, since legalization is handing them out per instruction and
// placement is only asking where a sibling operand will land.
struct UseSite {
    MachineLocation at;             // where the operand is read, if it is read where it lives
    bool needsTemp = false;         // otherwise it has to be brought into a scratch register
    RegisterBankId tempBank = BankGpr;
};

UseSite useSiteOf(LowerBase base, const MachineFunction& machine, const Placement& placement,
    LowerInst* inst, const InstShape& shape, Size i, U32 index, MachineLocation destructiveReg, bool memoryDest);

FunctionRegs allocateRegisters(Context& ctx, LowerBase base, LowerFunction& fun, const MachineFunction& machine);

// Checks the selected forms against the function they were selected for: that every instruction has
// one, that it belongs to the opcode the instruction was selected into, that it describes no more
// operands than the instruction has, that an operand it calls an immediate or folds away is one, and
// that the target has the features its encoding needs. Run at the end of transformFunction in debug
// builds, which is the boundary it checks.
bool verifySelection(Context& ctx, LowerBase base, LowerFunction& fun, const MachineFunction& machine);

// Checks a placement on its own terms, before any instruction has been resolved against it: that
// every live web has a location, that no two values whose lives overlap were given the same one,
// that each location is one a value of that type may occupy, and that nothing was placed in a
// register something writes while it is live. These are the mistakes that produce a wrong location
// rather than a wrong instruction, and catching them here names the web rather than the eventual
// read. computePlacement's caller runs it in debug builds.
bool verifyPlacement(Context& ctx, LowerBase base, LowerFunction& fun, Liveness& live,
    const MachineFunction& machine, const Constraints& constraints, const Placement& placement, bool framePointer);

// Checks that an allocation actually delivers every value to every instruction that reads it, by
// simulating the register and stack contents the emitted code will produce and comparing them
// against what each instruction expects to find. Returns false and logs the first disagreement per
// function; allocateRegisters runs it on its own result in debug builds.
//
// It knows nothing about how the allocation was arrived at - only about FunctionRegs, the liveness
// sets and the selected machine forms - so it stays a valid check as the allocator gains live intervals
// with holes, phi webs, stack homes and split locations.
bool verifyAllocation(Context& ctx, LowerBase base, LowerFunction& fun, Liveness& live, const MachineFunction& machine, const Constraints& constraints, const FunctionRegs& regs);

// Called (if non-null) once for every instruction/terminator emitted, with the byte range it
// occupies in `to.buffer` - used by test harnesses to build an annotated disassembly listing
// without genFunction itself needing to know anything about how that listing is formatted.
//
// `inst` is null for the function prologue, which belongs to the function rather than to any one
// instruction; `regs` is empty in that case.
using InstEmitCallback = void (*)(void* ctx, LowerInst* inst, const InstRegs& regs, U32 startOffset, U32 endOffset);

void genFunction(Context& context, LowerBase base, AsmModule& to, LowerFunction& fun, const MachineFunction& machine, FunctionRegs& regs, InstEmitCallback onInst = nullptr, void* onInstCtx = nullptr);
