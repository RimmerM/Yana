#pragma once

#include <stdlib.h>
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

    // Whether an argument register survives the call. It does not under any convention whose callee
    // is compiled code - the callee owns the register the moment it is entered, and `finish` checks
    // that every argument register is in the clobber set for exactly that reason. The Linux syscall
    // convention is the one exception: the kernel gives back everything but rax, rcx and r11, so a
    // value in rsi is still there after a `write`. What the call's *own* arguments cost is unchanged
    // either way - the copies that place them write those registers where the call stands, and
    // writtenRegisters adds them to the site's mask from the shape.
    bool preservesArgs = false;

    // Set once the convention has tables to work from. A function or call using one that does not
    // has to be rejected: with empty tables every argument would silently be classified onto the
    // stack, which is a working compile of the wrong program.
    bool defined = false;
};

/*
 * Where the arguments of one call live.
 *
 * Inline, because one of these is built per call instruction by four passes here and a call with
 * more than eight arguments is not what programs look like - see compiler/util/README.md. Nothing
 * holds the address of an entry across a push: every reader indexes the finished list.
 */
using ArgLocationList = SmallArray<ArgLocation, 8>;

// Assigns every argument of a call its location, by walking the argument list in order and handing
// out registers of each class until the convention runs out. Both sides go through this - the caller
// to place its operands, the callee to find where its arguments arrived, the verifier to check both.
//
// `typeOf` is asked for the type of argument `i`, so a caller can classify straight out of whatever
// buffer it already has without building a type list first.
template<class F>
void classifyArgs(const CallConvention& convention, Size count, F&& typeOf, ArgLocationList& out) {
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
void classifyResults(const CallConvention& convention, Size count, F&& typeOf, ArgLocationList& out) {
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
U32 argAreaBytes(const CallConvention& convention, const ArgLocationList& args);


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
    ArgLocationList uses;
    ArgLocationList creates;

    // Emptied rather than rebuilt. Every caller asks for this per instruction inside a walk of the
    // function - four of the passes here do, some of them more than once per instruction - so one
    // shape held across the walk is the difference between two allocations per instruction and none.
    void clear() {
        uses.clear();
        creates.clear();
        clobber = RegSet {};
        convention = nullptr;
        isReturn = false;
        isCall = false;
    }

    // Registers the instruction writes behind its operands' backs.
    RegSet clobber;

    // The convention a call, a syscall or a return follows, for the callers that need more of it
    // than the operand locations: the argument area's size and alignment, and the preserved set.
    const CallConvention* convention = nullptr;

    // A return's operands are constrained like its convention's *results* rather than its
    // arguments, and nothing is live once the function has returned - so a return neither clobbers
    // anything nor has anything left to protect.
    bool isReturn = false;

    /*
     * A call or a syscall, which is the one shape whose clobbers land *after* its operands have been
     * read rather than before them.
     *
     * Every other instruction's `clobber` is what its expansion writes behind its operands' backs -
     * `xor rdx, rdx` in front of a division, r11 as scratch inside an unrolled copy - so an operand
     * read straight out of its own register has to keep away from all of it. A call's clobber set is
     * the callee's, and the callee does not run until the target has been read and the arguments
     * placed. So the only registers a call's own operands have to dodge are the fixed ones its
     * parallel copy writes, and `computeAvoidSets` is where the difference is spent.
     */
    bool isCall = false;
};

// Fills `into`, which the caller owns and reuses - see InstShape::clear.
void shapeOf(LowerBase base, const MachineFunction& machine, const Constraints& constraints,
             LowerFunction& fun, LowerInst* inst, InstShape& into);

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

// The registers this instruction's own operands and results are pinned to, which is the part of
// `writtenRegisters` that stands *in front of* the instruction: the parallel copy places them
// whatever else the expansion then does. For a call it is the whole of what its target operand has
// to keep away from - see InstShape::isCall.
RegSet fixedRegisters(const InstShape& shape);

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

// Whether this backend can move a value of the class at all - between two registers, and between a
// register and a frame slot. False for the classes whose every transfer is VEX- or EVEX-encoded,
// which is the 256- and 512-bit vector classes and both mask classes: `classForType` will hand one
// out for a wide vector type, and this is what turns that into a stated refusal at the boundary
// rather than an assertion inside the encoder. See kClassMoves.
bool classHasMoves(RegisterClassId regClass);

/*
 * Where the instruction records live: a bump allocator over chunks that are kept rather than freed.
 *
 * Not `Arena` from context.h, for two reasons that both matter at this size. Its chunk is four
 * megabytes, which is three orders of magnitude more than a module's records, and it hands back
 * whatever offset the last allocation ended on - fine for the AST nodes it was written for, whose
 * sizes are all multiples of the word, and not for a run of `n` operands. This rounds every
 * allocation up so the next one is aligned, and `reset` rewinds instead of freeing, so the second
 * function of a module writes into the chunk the first one grew.
 */
struct RecordArena {
    RecordArena() = default;
    RecordArena(const RecordArena&) = delete;
    RecordArena& operator = (const RecordArena&) = delete;
    ~RecordArena();

    void* alloc(Size bytes);

    // Hands every chunk back to the next writer. Whatever was allocated before is still in memory
    // and is about to be overwritten, so this invalidates every record handed out since the last
    // one - see RegScratch::resetRecords.
    void reset() {
        chunk = 0;
        used = 0;
    }

private:
    // Large enough that a function of any ordinary size takes one chunk, small enough that holding
    // one per allocation is nothing. A request larger than this gets a chunk of its own.
    static constexpr Size kChunkBytes = 64 * 1024;

    Array<Byte*> chunks;
    Array<Size> sizes;
    Size chunk = 0; // the chunk being filled
    Size used = 0;  // bytes of it spent
};

/*
 * The instruction records are runs in the arena, named by `SmallBuffer`.
 *
 * Tritium already has the type this wants: a pointer and a length, with the storage owned by
 * somebody else - which is exactly what a run in the arena is. SmallBuffer is the packed form,
 * holding both in one word by putting the length in the top sixteen bits of the pointer, and an
 * instruction's operand list is a handful of entries, so the four lists of a record cost four words
 * between them instead of eight.
 *
 * As four separate arrays this was four allocations per instruction, each a header and a rounded-up
 * buffer around three operands. Now it is a bump of the arena pointer, and an InstRegs is something
 * that can be copied and moved without touching memory at all.
 *
 * The storage is the scratch's, not the record's, so a record is valid for as long as the scratch
 * that produced it is - see RegScratch.
 */

// Copies `from` into `arena` as a run. The one place a record's list is created: everything that
// builds one grows an ordinary array first and commits it here, since a run in an arena cannot grow.
template<class T>
SmallBuffer<T> commitSlice(RecordArena& arena, const Array<T>& from) {
    if(from.isEmpty()) return SmallBuffer<T> {};

    auto items = (T*)arena.alloc(from.size() * sizeof(T));
    for(Size i = 0; i < from.size(); i++) new (items + i) T(from[i]);

    return SmallBuffer<T> { items, from.size() };
}

// Resolved locations - physical registers, or frame slots where the encoding has a memory form -
// for a single instruction. `uses`/`creates` are parallel to that instruction's `used()`/`created()`
// buffers, in the same order, and name where the encoder will find (or put) each operand *at this
// instruction*, which is not necessarily where the value lives the rest of the time.
struct InstRegs {
    SmallBuffer<ResolvedOperand> uses;
    SmallBuffer<ResolvedOperand> creates;

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
    SmallBuffer<RegMove> moves;

    // Moves emitted immediately after the instruction, carrying a result out of the fixed register
    // the encoding had to write it to and into its home.
    SmallBuffer<RegMove> postMoves;
};

// Register assignments for every instruction in one block, in the order:
// block->instructions (in order), followed by exactly one entry for block->terminator.
struct BlockRegs {
    /*
     * Copies emitted at the block's entry, before its first instruction's own `moves`.
     *
     * The one insertion point a block did not have. It carries the half of edge resolution that
     * cannot go in the predecessor: where a predecessor branches, a copy at its end would run on the
     * way to *both* successors, so the copy goes at the start of the successor instead - which is
     * sound exactly when the successor has one predecessor. See collectEdgeTransitions.
     *
     * It runs *before* the first instruction's `moves`, and that ordering is the definition rather
     * than an accident: both sets stand at the block's entry point, and this one is what establishes
     * the location `locationAt(beforeInst(firstIndex))` names, which the operand copies behind it
     * then read from. The entry block's argument copies have the same relationship to the phi
     * transfers behind them.
     *
     * Empty for almost every block, and a block whose only content is one of these is no longer a
     * block that emits nothing - see `emitsNothing` in gen.cpp.
     */
    SmallBuffer<RegMove> entryMoves;

    /*
     * Copies emitted at the block's exit: after the terminator's own operand copies and immediately
     * in front of the terminator itself.
     *
     * The mirror of `entryMoves`, and built by exactly one thing - `hoistCommonEntryMoves`, which
     * moves a copy that stood at the head of *both* arms of a conditional branch back into the block
     * they branch from. Emitted where they are because that is the one point on both paths: after
     * this block's own copies, so nothing they establish is disturbed, and before the branch, which
     * is where the two paths part.
     *
     * Empty for every block that does not end in a conditional branch, and for nearly all of those.
     */
    SmallBuffer<RegMove> exitMoves;

    // Inline for a block of up to sixteen instructions, which most of them are: this is built once
    // per block of every function, and an InstRegs is four words now that its lists are runs in the
    // arena - see commitSlice.
    SmallArray<InstRegs, 24> insts;
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

    void clear() {
        slots.clear();
        references.reset();
        hasDynamicAlloca = false;
        argAreaSize = 0;
        callAlignment = 8;
    }
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

        // mov/movss/movsd r, [rip + global] - the *contents* of an immutable global rather than its
        // address, which is the one load that reproduces. `mut` clear is a promise that nothing
        // writes the storage, so the value does not depend on where in the program this lands. It
        // is what a pooled float constant is placed as: recreating it costs the same eight bytes
        // the definition did, where a frame home would cost a store and a reload of the same width.
        ConstantLoad,

        /*
         * pxor/vpxor r, r, r - a vector of zeroes, which every one of these machines makes out of
         * nothing at all.
         *
         * The cheapest recipe in the list and the one worth the most, because of what a *vector*
         * spill costs rather than what the instruction costs. A 16- or 32-byte value in the frame
         * raises the frame's own alignment past what the calling convention promises, so a function
         * holding one across a call pays a realigning prologue - a frame pointer held for the whole
         * function, `and $-32,%rsp`, and the `leave` that undoes it - for a value that is three
         * bytes to recreate. `sumVectors` in test/bench/programs is the shape: a zero accumulator
         * built before a call to `elements` and read after it.
         *
         * Not written as an `Immediate` of zero, which it resembles: that one is `mov r, imm` into a
         * general register, and this one is a self-exclusive-or in the vector bank. Two kinds
         * because two encoders.
         */
        VectorZero,
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
 * A web has one *home* - the location it keeps wherever nothing says otherwise - and a list of
 * *segments*, each of which is an exception to it over a stretch of program points. Most webs have
 * no segments at all. A web that was *split* has one per stretch that would otherwise have destroyed
 * it, or per stretch it borrows a register over - see the boundary invariant below.
 *
 * The home is a field rather than the first segment's location. It was positional once, which forced
 * every producer to build an alternating home/exception list and made "the location everywhere else"
 * something to be reconstructed rather than something stated.
 */

/*
 * Which of the three kinds of exception a segment is. What distinguishes them is not where they are
 * but *what entering and leaving one costs*, and how far one may reach:
 *
 *   Window  (§5.8) a web that has a register steps out of it into the frame across what would
 *                  destroy it. Entering stores and leaving reloads, and both are real: the register
 *                  is where the value was. Lies wholly inside one block.
 *   Cached  (§5.9) a web that has none steps into one over a cluster of its reads. Entering loads
 *                  and leaving costs nothing at all - the home never stopped holding the value, so
 *                  the copy back would write what is already there. Lies wholly inside one block.
 *   Region  (§5.10) the same idea over a set of *whole blocks*: a homeless web borrows a register for
 *                  a loop, and the copies that establish it stand on the CFG edges into the region
 *                  rather than between two instructions. Leaving is free on the same terms Cached is.
 *   RegionMoved    the same, for a web something *writes* inside the region. The home no longer holds
 *                  the value while the register does, so every edge out of the region stores it back
 *                  - which is Window's arithmetic at a region's scale.
 *
 * The two in-block kinds may not touch a block boundary; the two region kinds cover only whole ones.
 * Nothing may *define* a member of the web inside a Cached or a Region segment, which is what makes
 * the value at the far end the same value that went in - and RegionMoved is exactly the kind that
 * lifts that restriction by paying for it. `verifyPlacement` checks all of that.
 */
enum class SegmentKind : U8 {
    Window,
    Cached,
    Region,
    RegionMoved,
};

struct AllocationSegment {
    // Program points, half-open - see beforeInst/afterInst in lower.h.
    U32 from = 0;
    U32 to = 0;

    MachineLocation location;
    SegmentKind kind = SegmentKind::Window;

    bool covers(U32 point) const { return point >= from && point < to; }

    // Whether leaving this segment costs anything. The home holds the value throughout the two kinds
    // that answer true, so there is nothing to carry back out of them - see the kinds above, and
    // `collectTransitions` and `collectEdgeTransitions` in legalize.cpp, which are the two readers.
    bool leavesFree() const {
        return kind == SegmentKind::Cached || kind == SegmentKind::Region;
    }

    // Whether this segment is one of the two that live inside a single block. The distinction decides
    // which of the two transition walks owns its boundaries: an in-block segment's are copies between
    // two instructions, a region's are copies on CFG edges.
    bool inBlock() const {
        return kind == SegmentKind::Window || kind == SegmentKind::Cached;
    }

    // Whether a member of the web may be written inside it. The two that answer false are the two
    // whose whole soundness is that the home never stopped holding the value.
    bool allowsDefinition() const {
        return kind == SegmentKind::Window || kind == SegmentKind::RegionMoved;
    }
};

/*
 * Where one web lives: a home, and the exceptions to it.
 *
 * **The boundary invariant.** A web's location at a block's entry point and at its exit point is
 * whatever its segments say there, and that answer is single-valued *per block* - which is what makes
 * it something a CFG edge can be asked about. Two rules keep it so:
 *
 *   - an in-block segment (Window, Cached) never covers a block's entry or exit point, so a web is
 *     in its home at both ends of every block it merely passes through;
 *   - a Region segment covers only *whole* block spans, so a block is either entirely inside one or
 *     entirely outside it.
 *
 * What that rules out is a segment running from inside one block to inside another, which reads as
 * one location over a run of the *layout* - and the layout is not the CFG. The block in between may
 * be reached from elsewhere, and the block it ends in may be reached from a block where the web is
 * in its home. `verifyPlacement` checks the shape of every segment against the block spans, because
 * it is the assumption every consumer of a placement makes and the one whose violation would be
 * silent.
 *
 * Where a boundary becomes a copy follows from which kind of segment it bounds. An in-block boundary
 * at point `p` is a copy attached to instruction `(p - 1) / 2` - to its `moves` when p is odd and to
 * its `postMoves` when p is even, which are the two slots either side of the instruction and so the
 * two program points a boundary can fall between; `collectTransitions` in legalize.cpp is what places
 * it. A region's boundary is a copy on a CFG edge, and `collectEdgeTransitions` is what places that.
 */
struct WebAllocation {
    // Where the web is wherever no segment says otherwise. Invalid for a web that was never placed,
    // which is what `Placer::placed` asks.
    //
    // A field rather than the first segment's location, which is what it used to be. Positional made
    // every producer build an alternating home/exception list to keep a home segment first and last,
    // and made "the location everywhere else" something to be reconstructed rather than stated.
    MachineLocation home;

    // The exceptions to it, sorted and disjoint. Empty for almost every web; a split one has one per
    // stretch it spends somewhere else.
    //
    // Two inline: there is one of these per value in the function, and a web that is split at all is
    // usually split around one or two things.
    SmallArray<AllocationSegment, 2> segments;

    // What a copy of this web is made of. On the web rather than derived from a member's type for
    // the same reason RegMove carries one: a bank does not say which instruction a transfer takes,
    // and it is what lets a split transition be read off a placement without the IR beside it.
    RegisterClassId regClass = ClassGpr64;

    // The location this web occupies at `point`: whichever segment covers it, and the home otherwise.
    //
    // A point the web is not live at is answered like any other, which is what a *result* asks for:
    // it is resolved at its instruction's `before` point but does not exist until the `after` one. A
    // point inside a hole answers the home, and unambiguously - nothing carries a value across a
    // hole, so there is nothing there for a segment to be an exception to.
    MachineLocation locationAt(U32 point) const {
        for(auto& segment: segments) {
            if(segment.covers(point)) return segment.location;
        }

        return home;
    }

    // The segment covering `point`, or null where the home is what covers it. What the two transition
    // walks ask when they need the segment's *kind* and not only its location - whether leaving it
    // costs anything.
    const AllocationSegment* segmentAt(U32 point) const {
        for(auto& segment: segments) {
            if(segment.covers(point)) return &segment;
        }

        return nullptr;
    }

    /*
     * The copy this web needs on an edge, given where the predecessor exits and where the successor
     * is entered. False where it needs none.
     *
     * One statement of the rule, because three places ask it and a disagreement between any two of
     * them is silent: the resolver that emits the copy, the verifier that checks the edge has
     * somewhere to put one, and the placement pass that has to refuse a region whose edges have not.
     *
     * Leaving a segment costs nothing where `leavesFree` says so - the home never stopped holding the
     * value - but only where the home is where the value is going. A region handing over to another
     * register still has to hand it over.
     */
    bool edgeTransfer(U32 exitPoint, U32 entryPoint, MachineLocation& from, MachineLocation& to) const {
        from = locationAt(exitPoint);
        to = locationAt(entryPoint);
        if(from == to) return false;

        auto leaving = segmentAt(exitPoint);
        if(to == home && leaving && leaving->leavesFree()) return false;

        return true;
    }

    bool isSplit() const { return segments.isNotEmpty(); }

    // Empties the web without giving up the storage its segment list grew into - see
    // Placement::clear. A web starts unplaced, which is what an invalid home means.
    void clear() {
        home = MachineLocation::invalid();
        segments.clear();
        regClass = ClassGpr64;
    }
};

struct Placement {
    // Which web each value belongs to, indexed by the dense LiveId buildLiveness assigns, and the
    // webs themselves. A web is named by the LiveId of its representative, so the two are indexed
    // alike and a value's location is one lookup away.
    //
    // The webs are a PooledList rather than an Array because a placement is written into rather than
    // returned: `clear` empties each web's segment list instead of destroying it, so placing a
    // second function - or the same function a second time, which is what a displacement costs -
    // reuses the storage the first one grew. See allocateRegisters.
    Array<LiveId> webOf;
    PooledList<WebAllocation> webs;

    /*
     * Which values a copy has proved hold the same number, as one label per value: two values are
     * one number exactly when their labels agree.
     *
     * A web is normally a set of quantities that never coexist, which is what lets one location
     * serve all of them. Copy coalescing widens that: the two ends of a `mov` may be live at once
     * and still share a register, because what is in it is right for both. Nothing in placement
     * needs to tell the two kinds apart - a web is a web - but the *verifier* does, since "two
     * overlapping values in one location" is otherwise exactly the mistake it exists to catch.
     *
     * See buildWebs, which fills it before the phi merges can widen a web past what a copy proved.
     */
    Array<LiveId> copyClassOf;

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

    // Set when legalizing this placement could need scratch registers, which asks for the reserve to
    // be measured and, if it grew, for one more placement pass with it held back. See
    // allocateRegisters. It is a *may*, not a *will*: measuring a placement that turns out to need
    // nothing costs one pass and holds nothing back, where guessing low leaves an instruction with
    // nowhere to bring a value it cannot read where it is.
    //
    // Two things set it, and both are properties of the placement rather than of the function.
    //
    // A web with no register at all: a value that is not in one has to be brought into a scratch
    // register at each instruction that touches it.
    //
    // And two or more webs in a class with no exchange instruction. A parallel copy whose sources
    // and destinations permute cyclically has to break the cycle, and where the class *has* an
    // exchange - the general registers - that costs no register at all. The vector file has none, so
    // the break has to park one end somewhere, and a phi swap is exactly the shape that produces
    // one. Nothing here knows whether a cycle will actually occur; that is what the measurement is.
    bool requiresLegalizationTemps = false;

    /*
     * Webs this pass would rather have displaced than left the web that asked for their register
     * homeless.
     *
     * A request from placement to placement, and it names *a register* rather than asking for the
     * web to be left with none. That is the whole of what the asking web needed: this one register,
     * free over this interval. What the displaced web then does is its own search again, against a
     * register file that has since changed - so a web displaced out of r13 in one pass can take r14
     * in the next if nothing else wanted it, where being left homeless outright meant the frame
     * whatever else was free.
     *
     * It is also what bounds the loop. A register only ever *enters* a web's displaced set, so each
     * request either narrows one web's choices by one register or is a repeat and is dropped.
     */
    struct DisplacementRequest {
        LiveId web = LiveId(0);
        PhysicalReg reg;
    };

    Array<DisplacementRequest> displacementRequests;

    Size valueCount() const { return webOf.size(); }

    // Whether a chain of copies proved these two values hold the same number - see `copyClassOf`.
    bool sameNumber(LiveId a, LiveId b) const {
        return a < copyClassOf.size() && b < copyClassOf.size() && copyClassOf[a] == copyClassOf[b];
    }

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

    // The location this value's web keeps everywhere its segments do not say otherwise - see
    // WebAllocation. *Not* the location at a block boundary, which a region segment may differ at:
    // ask `locationOf` at the boundary point for that.
    MachineLocation homeOf(LiveId id) const {
        return id < webOf.size() ? webs[webOf[id]].home : MachineLocation::invalid();
    }

    // Empties the placement for the next function, keeping every buffer it grew into. `webs` is
    // sized by its owner rather than here, since the value count is the first thing placement knows
    // and emptying a list it is about to resize would be work done twice.
    void clear() {
        webOf.clear();
        copyClassOf.clear();
        frame.clear();
        remats.clear();
        incomingArgs.clear();
        displacementRequests.clear();
        writtenPhysical = RegSet();
        requiresLegalizationTemps = false;
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

    void clear() {
        blocks.reset();
        writtenPhysical = RegSet();
    }
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

    // How often each block runs relative to the entry - see FunctionFrequencyInfo. Built once by
    // allocateRegisters, which needs it for every spill cost it weighs, and carried here so that
    // emission can weigh a jump the same way (§7.2) rather than walking the CFG a second time.
    FunctionFrequencyInfo frequency;

    // Empties the whole allocation for the next function, keeping every buffer - see
    // allocateRegisters.
    void clear() {
        placement.clear();
        legalized.clear();
        usedCalleeSaved = RegSet();
        framePointer = false;
        temporaries = TemporaryReserve();
    }
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
 * register held for the whole function. checkFrameSupported reports it as the function enters the
 * backend, in every build, rather than leaving it to an assertion a release build removes.
 */
/*
 * The bytes a whole vector register occupies when it is preserved.
 *
 * A callee-saved vector register has to be given back entire whatever this function put in it - the
 * caller may have been holding a packed value there and nothing in the IR represents that - so
 * preservation is the *bank's* width rather than the class of whatever occupied it.
 *
 * Which makes it a function of the target rather than a constant. On a target where a value can
 * occupy a ymm, the caller may be holding thirty-two bytes in one and giving back sixteen is silent
 * corruption of a value nothing in this function ever named - the exact failure the paragraph above
 * describes, one register width up. `widestVectorClass` is the same answer said as a class, and the
 * save is emitted with that class's move so the two cannot disagree about the width.
 */
inline U32 vectorSaveSize() {
    return (targetFeatures() & kFeatureAvx2) ? 32u : 16u;
}

inline RegisterClassId widestVectorClass() {
    return (targetFeatures() & kFeatureAvx2) ? ClassYmm256 : ClassXmm128;
}

/*
 * Whether an instruction touching a register of this class has to carry a vector prefix.
 *
 * The rule the whole backend keeps once the target has AVX: **nothing that writes a vector register
 * is encoded as a legacy SSE instruction.** A legacy write leaves the upper half of its register
 * alone, and executing one while any upper half is dirty is what costs - a save and a restore on the
 * parts §5.4 was written for, a merging uop and a false dependency on everything since. VEX-encoded
 * writes zero those bits, so a function that is VEX throughout can never be in the state that pays.
 *
 * Asked of the three narrow classes only. The wide ones have no legacy spelling to choose against,
 * and a general register has no upper half for anything to be dirty in.
 *
 * The form table answers the same question through `MachineForm::alternative` - see the VEX tier in
 * the machine_forms_*.cpp files. This is for the bytes that no form describes: the copies, the spills and the reloads
 * `kClassMoves` writes, and the expansions a pseudo's own emitter writes.
 */
inline bool vectorClassNeedsVex(RegisterClassId regClass) {
    if(regClass != ClassFloat32 && regClass != ClassFloat64 && regClass != ClassXmm128) return false;
    return (targetFeatures() & kFeatureAvx) != 0;
}

// The same question where there is no class to ask it of, which is every pseudo that expands into
// packed instructions: those are all ClassXmm128 at the narrow width and ClassYmm256 at the wide one,
// and the wide one is VEX whatever this answers.
inline bool packedNeedsVex() {
    return (targetFeatures() & kFeatureAvx) != 0;
}

struct FrameLayout {
    // Callee-saved general registers the prologue pushes, in ascending register order.
    RegSet savedRegs;

    // Callee-saved vector registers, which cannot be pushed - there is no PUSH for one - and take a
    // region of the frame instead, kVectorSaveSize bytes each in ascending register order starting
    // at `vectorSaveOffset` from `vectorSaveBase`. Empty under every convention that treats the
    // whole vector file as caller-saved, which is most of them.
    RegSet savedVectors;
    I32 vectorSaveOffset = 0;
    PhysicalReg vectorSaveBase;

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
    bool isEmpty() const {
        return savedRegs.isEmpty() && savedVectors.isEmpty() && !framePointer && fixedSize == 0;
    }

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

// Whether this function may have to align rsp itself, because something in it could need a stronger
// boundary than its own entry convention promises. Answered from the IR for the same reason the
// frame-pointer question is - realigning requires a frame pointer, so both have to be settled before
// the allocator is told which registers it may hand out - and it is a *may* because a spill slot
// wider than a word demands the alignment only if the allocator actually creates one. The exact
// answer belongs to computeFrameLayout, which has the slots in front of it; see FrameLayout.
bool functionMayRealignStack(LowerBase base, LowerFunction& fun, const Constraints& constraints);

// Whether this backend can build a frame for this function at all, reported if it cannot. Answered
// from the IR before any backend decision has been taken, and unconditionally rather than as an
// assertion - the one unsupported combination is a dynamic alloca in a function that also has to
// realign the stack. Returns false having reported; see checkFrameSupported in frame.cpp.
bool checkFrameSupported(Context& ctx, LowerBase base, LowerFunction& fun, const Constraints& constraints);

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
    // The patched value is `symbolOffset - (siteOffset + 4 + trailing)`, i.e. relative to the end of
    // the instruction (matching how the CPU computes RIP-relative offsets).
    U32 siteOffset;

    /*
     * Bytes of this instruction that follow the displacement field.
     *
     * Zero for every site but one, because a rel32 and a `lea` both end at the field - which is why
     * this did not exist while a RIP-relative address was only ever materialized. A memory access
     * that carries an immediate does not: `mov dword [rip + g], 7` writes the constant after the
     * displacement, and the processor measures the displacement from the *end of the instruction*,
     * so those four bytes have to be subtracted here or the store lands four bytes past the global.
     */
    U8 trailing = 0;

    /*
     * Data sites only, and ignored for a code site.
     *
     * Set for a compiler-built table's slot, which holds `target - anchorOffset` in four bytes and
     * is therefore final the moment both are placed - so resolveRelocations writes it and it never
     * reaches applyDataRelocations. Clear for a pointer inside a source constant, which is absolute
     * and target-width and cannot be written until the image is mapped.
     *
     * Note that a table slot is measured from the image anchor, not from the byte after the field -
     * unlike the rel32 above, which matches how the CPU computes a RIP-relative displacement.
     * Nothing is executing here; the reader adds the anchor's address back, so that is what it must
     * be measured from. See repr/table.h.
     */
    bool anchorRelative = false;

    // Resolution target: exactly one of these is set.
    // `function` is used for calls/address-loads that target a (possibly not-yet-emitted)
    // function elsewhere in the module; `global` is used for address-loads of module-level data
    // (see AsmModule::addGlobal). A jump within a function is not one of these - its target is
    // known the moment the function is finished, and relaxBranches resolves it there.
    LowerFunction* function = nullptr;
    LowerGlobal* global = nullptr;
};

/*
 * A jump within a function, whose displacement may turn out to be one byte or four.
 *
 * Recorded rather than resolved as it is written, because which of the two it is cannot be answered
 * until the whole function has been emitted - and answering it shortens the function, which changes
 * the answer for every other branch in it. `relaxBranches` in gen.cpp settles all of them at once
 * and then writes the bytes; see §7.1 of the README.
 *
 * The instruction is written in its *long* form and this says how to write the short one, so the
 * buffer holds a legal function at every point and the relaxation is a rewrite rather than a
 * back-patch of something incomplete.
 */
struct AsmBranch {
    U32 start;    // the instruction's first byte - the 0x0f escape, for a conditional
    U32 site;     // the rel32 field, which is `start + 1` for a jmp and `start + 2` for a jcc

    // The block this branch lands on, or null for the function's shared epilogue (§7.2). Null is a
    // real target rather than a missing one: the epilogue is not a block, and a branch aimed at a
    // return that emits nothing else is aimed at it directly.
    LowerBlock* block;

    U8 shortOpcode;   // 0xeb, or 0x70 + the condition code the long form encodes as 0x80 + cc
    bool isShort;     // what relaxation decided
};

/*
 * §7.3 Room reserved in front of a loop, whose size relaxation decides.
 *
 * A loop head that lands across an instruction-fetch boundary is measured at up to 40% slower than
 * the same bytes one boundary over (§55.2 of test/bench/findings.md), and which side of a boundary
 * it falls on is otherwise decided by the total size of everything emitted before it. `llc` aligns
 * substantially every loop head it emits and this backend used to align none, so a single reading of
 * the two compared one's placement policy with the other's luck.
 *
 * The bytes cannot be counted when the block is emitted, for the same reason a branch's length
 * cannot: every branch below the loop is still going to shrink, and shrinking one moves the loop.
 * So the maximum is reserved here and `relaxBranches` decides how much of it to keep, at the point
 * where the layout is settled and the loop's extent is known - which is also what lets the decision
 * be made on the loop's *size*, since by then there is one.
 *
 * The reserved bytes are written as single-byte nops and rewritten as one multi-byte nop by the
 * compaction, so what survives is one instruction rather than a run of them. Only a fallthrough into
 * the loop executes it, and it does so once.
 */
struct AsmPad {
    U32 start;          // the first reserved byte
    U32 end;            // one past the last, which is the block's first byte
    LowerBlock* block;  // the loop header this precedes
    U32 keep = 0;       // what relaxation decided to keep
};

/*
 * An absolute address inside emitted constant data - LowerDataRelocation, after placement.
 *
 * Kept apart from AsmRelocation because it is a different kind of fixup: that one is a rel32 in an
 * instruction, patched once every symbol's offset is known, and this one is a full 64-bit address
 * that stays unknown until the module is mapped. Both offsets are recorded here; the addition
 * happens in applyDataRelocations, which is the last thing to touch the buffer.
 */
struct AsmDataRelocation {
    // Where in the buffer the 8-byte address goes.
    U32 siteOffset;

    // The buffer offset of what it points at.
    U32 targetOffset;
};

struct AsmModule {
    explicit AsmModule(Size initialSize = 4096): buffer(initialSize) {}

    Net::BufferWriter buffer;
    Array<AsmBlock> blocks;
    Array<AsmRelocation> relocations;
    Array<AsmDataRelocation> dataRelocations;

    // Data relocations whose target offset is not known yet, because the global or function they
    // name had not been emitted when the data holding them was. Drained by resolveRelocations.
    Array<AsmRelocation> pendingData;

    HashMap<LowerBlock*, U32> blockOffsets;
    HashMap<LowerFunction*, U32> functionOffsets;
    HashMap<LowerGlobal*, U32> globalOffsets;

    /*
     * §5.4 Whether any function in this module can leave the upper half of a `ymm` dirty.
     *
     * A question about the module and not about the function being emitted, which is the whole
     * point of it: the upper halves a foreign boundary hands over are dirty because of whatever ran
     * before it, and what ran before it is usually not the function the boundary is in. Cached here
     * because emission is per function and the answer is not - see moduleDirtiesUpperHalves.
     */
    enum class UpperHalves: U8 { Unknown, Clean, Dirty };
    UpperHalves upperHalves = UpperHalves::Unknown;

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

    /*
     * Opens a function, having first laid down its prefix data where the only place it may go is.
     *
     * A function with prefix data is preceded by exactly its bytes: whoever reads them computes
     * their address by subtracting their size from the entry point, so a byte of padding in between
     * would point the reader at the wrong word. The padding that *is* allowed goes before them, and
     * is what keeps the prefix - and so the entry point - on a sensible boundary after whatever the
     * previous function's last instruction left behind.
     *
     * Every entry point is put on a sixteen-byte boundary, which is what every other x86-64 toolchain
     * does and what this did not: functions used to be packed end to end, so a function's address -
     * and with it the alignment of every loop inside it - was decided by the total size of everything
     * emitted before it. That is a cost twice over. It is a *measurement* hazard, and the one
     * test/bench/README.md's caution about padding is about: a change that shrinks one function moves
     * every function below it, and a hot loop that lands across a 32-byte boundary differently is
     * worth up to 40% on its own, so an improvement and a regression are indistinguishable. And it is
     * a cost in itself, since which side of a boundary an entry point falls on is then arbitrary
     * rather than chosen. The padding is the same trapping byte the process entry uses.
     */
    void startFunction(LowerBase base, LowerFunction* fun) {
        // The prefix is what the padding is measured back from, since the entry point is what has to
        // land on the boundary and the prefix is glued to the front of it.
        auto prefix = fun->prefix ? base[fun->prefix]->initialContents.size() : 0;
        auto mask = U64(functionAlignment()) - 1;
        while((buffer.offset() + prefix) & mask) buffer.writeByte(0xcc);

        // The displacement, after the boundary rather than before it, so that every entry point
        // lands at the same offset past its own multiple. See functionDisplacement.
        for(U32 i = 0; i < functionDisplacement(); i++) buffer.writeByte(0xcc);

        if(fun->prefix) emitData(base, base[fun->prefix]);
        functionOffsets.add(fun, U32(buffer.offset()));
    }

    /*
     * The boundary above, and the one control that makes a timing comparison mean anything.
     *
     * Sixteen is the answer, and is what every other x86-64 toolchain uses. `YANA_FUNC_ALIGN` raises
     * it, and exists for exactly one purpose: **a measurement**. Padded to a boundary wider than any
     * function in the image, every function lands at a fixed multiple of it whatever the ones in
     * front of it are - so a change that alters one function's size cannot move another, and the
     * difference between two runs is the change rather than where the change pushed everything else.
     *
     * That is not a small correction. test/bench/README.md's caution about it was written before
     * this existed and understates it: on the corpus, a fold that removed two bounds checks from
     * `direct` in Pipeline.yana and touched no other byte of the program measured **+28 ms** on the
     * whole program and **-1 ms** on `direct` itself, purely because the 45 bytes it saved moved every
     * function below it. Rank a change with `YANA_FUNC_ALIGN=256` and read the ordinary build for
     * what actually ships.
     */
    static U32 functionAlignment() {
        auto set = getenv("YANA_FUNC_ALIGN");
        if(!set) return 16;

        auto value = U32(atoi(set));
        return value >= 16 && (value & (value - 1)) == 0 ? value : 16;
    }

    /*
     * The second half of that control, and the one that reaches *inside* a function.
     *
     * `YANA_FUNC_ALIGN` pins where a function starts; it does not change where a loop inside it sits
     * relative to the 32- and 64-byte lines the front end fetches in, since the loop's offset from
     * the entry point is fixed by the code. `YANA_FUNC_PAD` displaces every entry point by the same
     * number of bytes past its boundary, so a sweep of it moves every loop head in the image through
     * the line together while leaving each function's internal layout byte-identical.
     *
     * It exists because this backend emits no loop-head alignment at all and llc emits one on
     * substantially every loop, so a single reading of the corpus compares our alignment luck with
     * their alignment policy. Swept, the best reading is what the code is worth; the spread is what
     * the policy is worth. A short loop that straddles a 64-byte line measures up to 40% slower than
     * the same bytes one boundary over - see findings.md §55.
     *
     * Measurement only, like the boundary above: shipping builds set neither.
     */
    static U32 functionDisplacement() {
        auto set = getenv("YANA_FUNC_PAD");
        if(!set) return 0;

        auto value = U32(atoi(set));
        return value < functionAlignment() ? value : 0;
    }

    // Appends a global's data to the buffer and records the offset its address-loads resolve to.
    // Globals are emitted into the same flat buffer as code (this is not an object-file writer -
    // there are no sections), so callers should emit all functions first and all globals after,
    // keeping executable and non-executable bytes from interleaving. Offsets are 16-byte aligned
    // so that a global is never split across a cache line by whatever preceded it.
    void addGlobal(LowerBase base, LowerGlobal* global) {
        while(buffer.offset() & 15) buffer.writeByte(0);
        emitData(base, global);
    }

    // One global's bytes at the current offset, wherever that is. Shared by the module's data and by
    // prefix data, which differ in where they are placed and in nothing else.
    void emitData(LowerBase base, LowerGlobal* global) {
        auto start = U32(buffer.offset());
        globalOffsets.add(global, start);
        buffer.writeBytes(global->initialContents.data(), global->initialContents.size());

        // The sites are recorded against the emitted copy rather than against the source bytes, and
        // their targets are looked up in resolveRelocations - the global this one points at may not
        // have been emitted yet.
        for(auto relocation: global->relocations.contents(base)) {
            pendingData.push(AsmRelocation {
                .siteOffset = start + relocation.offset,
                .anchorRelative = relocation.anchorRelative,
                .function = relocation.function ? base[relocation.function] : nullptr,
                .global = relocation.global ? base[relocation.global] : nullptr,
            });
        }
    }

    /*
     * Writes the real addresses into the data relocation sites, given where the module was placed.
     *
     * Separate from resolveRelocations because it needs something that one does not: the address the
     * buffer ended up at. Whoever maps the module calls this once, afterwards; a linker would emit
     * the same list as dynamic relocations and let the loader do it.
     */
    void applyDataRelocations(Byte* loadBase) {
        // Through a writer over the same bytes rather than a copy of a host U64: a relocation site
        // is a target-endian word exactly like every immediate the emitter wrote above it, and the
        // one place that wrote one by copying host bytes was the one place it was not stated.
        //
        // Sized at the buffer's capacity rather than at its length so that nothing here can reach
        // the resize path, which would move the bytes into an allocation of the writer's own and
        // leave the patched addresses in memory nobody maps.
        Net::BufferWriter site(buffer.buffer, Size(buffer.max - buffer.buffer));

        for(auto& relocation: dataRelocations) {
            site.offset(relocation.siteOffset);
            site.writeLong<LittleEndian>(U64(loadBase) + U64(relocation.targetOffset));
        }
    }

    // Records a placeholder relocation at the rel32 field about to be written at the buffer's
    // current offset, then writes a placeholder 0 in its place. Call resolveRelocations() once
    // all functions referenced by any relocation have been emitted. `trailing` is how many bytes of
    // the instruction still follow the field - see AsmRelocation.
    void addRelocation(LowerFunction* target, U8 trailing = 0) {
        relocations.push(AsmRelocation {
            .siteOffset = U32(buffer.offset()), .trailing = trailing, .function = target,
        });
        buffer.writeInt<LittleEndian>(0);
    }

    void addRelocation(LowerGlobal* target, U8 trailing = 0) {
        relocations.push(AsmRelocation {
            .siteOffset = U32(buffer.offset()), .trailing = trailing, .global = target,
        });
        buffer.writeInt<LittleEndian>(0);
    }

    /*
     * Patches every recorded relocation with the now-known offset of its target.
     * Must be called after every block/function referenced by any relocation has been emitted.
     *
     * `anchor` is the global every table slot is measured from - see repr/table.h. Null is allowed
     * only for a module that built no table, since a slot cannot be written without it.
     */
    void resolveRelocations(LowerGlobal* anchor);
};

// Represents an address calculation (base + index * scale) + displacement.
// Used with two different instruction kinds:
//  - X86Address: purely embedded into whatever instruction uses it (Load/Store) - never
//    materialized into a register of its own, so its result is always Implicit.
//  - X86Lea: materializes the computed address into a real register (LEA), e.g. for pointer
//    arithmetic that doesn't immediately feed a Load/Store.
struct LowerInstX86Address: LowerInstSingle {
    LowerInstX86Address(LowerInst::Kind kind, StringId name, LowerPtr<LowerValue> base, LowerPtr<LowerValue> index, U8 scale, U32 displacement):
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

    /*
     * `[rip + g]` instead of a computed address: a global named in the encoding rather than a
     * pointer held in a register.
     *
     * A field rather than an operand because that is exactly what it is not - the form has nothing
     * to place, and a `Global` instruction feeding this would be a value the allocator had to find a
     * register for. Set only where base and index are both absent, since the rip-relative form has
     * neither field.
     *
     * What it buys is one instruction where there were two: a pooled constant read once becomes
     * `addsd xmm, [rip + k]` rather than a load into a register and an add of it.
     */
    LowerPtr<LowerGlobal> symbol = nullptr;

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

/*
 * The working storage the two passes share, owned by whoever is allocating a series of functions.
 *
 * Everything in here is per-function state that gets thrown away, and every one of those buffers is
 * O(values) or O(instructions) in size: one list of conflicting webs per value, one list of
 * occupants per register, one operand record per instruction. Allocating them per function meant a
 * few thousand mallocs to compile a handful of small modules, all of them handing back a buffer the
 * next function immediately asked for again.
 *
 * So the caller holds one of these across the whole module and hands it to each call. The pass
 * structures are held by pointer because their contents are private to place.cpp and legalize.cpp;
 * they are created on first use and released by the destructor.
 */
struct PlacementScratch;
struct LegalizeScratch;

void destroyPlacementScratch(PlacementScratch* scratch);
void destroyLegalizeScratch(LegalizeScratch* scratch);

struct RegScratch {
    RegScratch() = default;
    RegScratch(const RegScratch&) = delete;
    RegScratch& operator = (const RegScratch&) = delete;

    ~RegScratch() {
        destroyPlacementScratch(placement);
        destroyLegalizeScratch(legalize);
    }

    PlacementScratch* placement = nullptr;
    LegalizeScratch* legalize = nullptr;

    /*
     * Where the instruction records live - see commitSlice.
     *
     * This one is *not* emptied between functions, because unlike everything else here it is not
     * scratch: a FunctionRegs points into it, so an allocation stays readable for as long as this
     * scratch does. That is what lets a caller hold on to the records of every function in a module
     * - which the codegen trace does, since it prints them only once the whole module is emitted.
     *
     * A caller that consumes each function before allocating the next may call `resetRecords`
     * between them and keep the arena at one function's worth. It invalidates every record already
     * handed out, which is why it is the caller's to call and not this file's.
     */
    RecordArena records;

    void resetRecords() { records.reset(); }

    // The registers each web has already been displaced out of, one entry per value - see
    // Placement::DisplacementRequest. Held here rather than in the placer because it is the one
    // piece of state that survives *between* passes over a function.
    Array<RegSet> displacedFrom;
};

// One complete placement of a function, written into `out` - which is emptied first, so a placement
// that has already been used for another function hands this one its buffers.
//
// `framePointer` and `temporaries` are what is held back from every web - rbp when the frame is
// addressed through it, and the scratch registers legalization is going to need - and
// `displacedFrom` names, per web, the registers a previous pass asked it to keep out of. `frequency` is what
// every decision that trades one part of the function against another is weighed by; it depends on
// the CFG alone, so one is computed per allocation rather than per pass.
void computePlacement(LowerBase base, LowerFunction& fun, Liveness& live, const MachineFunction& machine,
    const Constraints& constraints, const FunctionFrequencyInfo& frequency, const LoopInfo& loops, bool framePointer,
    const TemporaryReserve& temporaries, const Array<RegSet>& displacedFrom, RegScratch& scratch,
    Placement& out);

// Resolves every instruction against a completed placement, handing out scratch registers from
// `temporaries` - which has to be one measureTemporaryReserve produced for this same placement.
void legalizeFunction(LowerBase base, LowerFunction& fun, Liveness& live, const MachineFunction& machine,
    const Constraints& constraints, const Placement& placement, const TemporaryReserve& temporaries,
    RegScratch& scratch, LegalizedFunction& out);

// How many scratch registers legalizing this placement will need, by bank and by pool. Answered by
// legalizing it and recording what was asked for, rather than by a second rule that mirrors the
// first: the two would be a pair of answers to one question, and the one that is wrong is the one
// that leaves an instruction with nowhere to bring a spilled operand.
TemporaryReserve measureTemporaryReserve(LowerBase base, LowerFunction& fun, Liveness& live, const MachineFunction& machine,
    const Constraints& constraints, const Placement& placement, const TemporaryReserve& pool,
    RegScratch& scratch);

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

// Allocates `fun` into `result`, which is emptied first: a FunctionRegs that has already described
// another function hands this one every buffer it grew, which is what makes allocating a module
// cost the largest function's storage rather than the sum of them all.
void allocateRegisters(Context& ctx, LowerBase base, LowerFunction& fun, const MachineFunction& machine,
    RegScratch& scratch, FunctionRegs& result);

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
// `inst` is null for the two sequences that belong to the *function* rather than to any one
// instruction, and `regs` is empty for both: the prologue, which is reported first because it is
// emitted first, and the shared epilogue of §7.2, which is reported last for the same reason. A
// function that duplicates its epilogue reports only the prologue.
//
// Offsets are final. They are reported once branch relaxation (§7.1) has rewritten the function, so
// a range here is where the instruction actually ended up rather than where it was first written.
using InstEmitCallback = void (*)(void* ctx, LowerInst* inst, const InstRegs& regs, U32 startOffset, U32 endOffset);

void genFunction(Context& context, LowerBase base, AsmModule& to, LowerFunction& fun, const MachineFunction& machine, FunctionRegs& regs, InstEmitCallback onInst = nullptr, void* onInstCtx = nullptr);
