#include "transform_internal.h"

/*
 * The shape of one instruction.
 *
 * Nothing here changes what a function computes. Each of these decides how one instruction is
 * *written*: whether its constant operand is embedded in the encoding or materialized into a
 * register, whether its callee needs an address at all, whether its cast extends anything, and
 * whether the comparison above it can stay in the flags rather than becoming a register.
 *
 * They are peepholes in the strict sense - each looks at one instruction and the values around it -
 * and the sweep that runs them is `selectMachineInstructions` in transform.cpp, which is also where
 * the order between the two halves is argued for.
 */

// Whether running this instruction can change the flags register.
//
// Answered from the form selection would give it, which is the same function the final selection
// pass calls - so the two cannot drift apart. A peephole can change which form an instruction takes:
// an immediate that becomes embedded turns a register form into an immediate one. What it usually
// cannot change is whether the form writes the flags, which validateMachineForms checks for every
// opcode that does not explicitly declare its forms to differ.
//
// The six that do declare it are why this has one caller and where that caller is matters. Three of
// them are conservative until they settle - an immediate is `xor r, r` until it is embedded - but a
// constant-sourced `cast` or `bitcast` is not: it *gains* the flags effect when its constant is
// embedded. So this is only ever asked from the second sweep of selectMachineInstructions, by which
// point every form decision a peephole makes has been made. See MachineOpcodeDesc::flagsSelective.
inline bool modifiesFlags(LowerBase base, LowerInst* inst) {
    return writesFlags(machineTarget().form(selectForm(base, inst)).flagsEffect);
}

// Whether embedding this constant is worth doing at all, which is a size question rather than a
// legality one - fitsImmediate answers legality, per form, in canEmbedImm below.
//
// A one-byte immediate is always cheaper embedded. A four-byte one is cheaper only while it is read
// a couple of times; past that, one materialization into a register beats repeating four bytes at
// every use.
static bool isEmbeddableImm(LowerImm* imm) {
    // Floats can never be embedded.
    if(!isIntLike(imm->result.type)) return false;

    if(fitsImmediate(ImmediateWidth::Imm8, imm->i)) return true;
    return fitsImmediate(ImmediateWidth::Imm32, imm->i) && imm->result.uses.size() <= 2;
}

// Checks if this specific instruction can embed the provided embeddable immediate operand.
//
// Which operands can swallow which constant is the form table's answer - every operand position the
// value occupies has to have a form that accepts an immediate *of this width* there. A value read
// twice by one instruction, only one of whose positions takes an immediate, is not embeddable at
// all: embedding it would leave the other position with no location to read.
static bool canEmbedImm(LowerBase base, LowerInst* inst, LowerValue* op) {
    // A cast produces a value of its own, so the constant has to be materializable at the target
    // type - which for now means an integer one.
    auto kind = inst->kind;
    if(kind == LowerInst::Cast || kind == LowerInst::Bitcast) {
        if(!isIntLike(((LowerInstUnary*)inst)->result.type)) return false;
    }

    /*
     * The scalar of a splat this machine builds out of nothing - §5.7. `pxor r, r` and `pcmpeqd r, r`
     * take it as `folded()`, so there is nothing to encode and nothing to place; left occupying a
     * location it is a `mov r15d, 0` in front of a `pxor` that does not read it.
     *
     * Answered here rather than through `opcodeCanEmbedImmediate` for the reason the block count is:
     * that function is asked about an opcode and a position, and which of `OpVBroadcast`'s forms
     * applies is a question about this instruction's own operand.
     */
    if(splatIsMachineConstant(base, inst)) return true;

    auto opcode = opcodeFor(base, inst);
    auto value = immValue(op);
    auto used = inst->used();
    bool found = false;


    for(Size i = 0; i < used.size(); i++) {
        if(base[used[i]] != op) continue;


        if(!opcodeCanEmbedImmediate(opcode, i, value)) return false;
        found = true;
    }

    return found;
}

/*
 * Whether every use of this constant is a splat the machine builds out of nothing - §5.7.
 *
 * For those, being taken out of allocation is not an optimization: `selectForm` answers `FormVZero`
 * or `FormVOnes` for the *pattern*, and both take the scalar as `folded()`, so a scalar that keeps a
 * location is the selection verifier's "folds an operand that still needs a location". It is
 * therefore asked ahead of `isEmbeddableImm`, whose two rules are both about cost and both wrong
 * here - a float can never be embedded, and `zero() :: Vec(Float)` is a float; and a constant read
 * more than twice is cheaper in a register, which a splat's scalar never is because there is no
 * register.
 */
bool onlyFeedsMachineSplats(LowerBase base, LowerImm* imm) {
    auto uses = imm->result.uses.contents(base);
    if(uses.size() == 0) return false;

    for(auto use: uses) {
        if(!splatIsMachineConstant(base, base[use])) return false;
    }

    return true;
}

// Tries to embed this immediate into any instructions that use it.
bool tryEmbedImm(LowerBase base, LowerImm* imm) {
    if(!isEmbeddableImm(imm) && !onlyFeedsMachineSplats(base, imm)) return false;

    for(auto use: imm->result.uses.contents(base)) {
        if(!canEmbedImm(base, base[use], &imm->result)) return false;
    }

    imm->result.flags |= LowerValue::Implicit;
    return true;
}

// A call to a statically known function is encoded as a direct rel32 call, which never reads the
// target address out of a register. Materializing it costs a `lea` that nothing reads, and - worse -
// a register that has to survive the call's clobber set, of which there are only a handful. Mark the
// address implicit unless something other than a direct callee position actually needs it.
bool tryElideDirectCallee(LowerBase base, LowerInstFun* fun) {
    for(auto offset: fun->result.uses.contents(base)) {
        auto use = base[offset];
        if(use->kind != LowerInst::Call) return false;
        if(((LowerInstCall*)use)->getCallType() == LowerCallType::Syscall) return false;

        // used()[0] is the callee; anywhere else it is an ordinary argument and needs a register.
        auto used = use->used();
        if(base[used[0]] != &fun->result) return false;

        for(Size i = 1; i < used.size(); i++) {
            if(base[used[i]] == &fun->result) return false;
        }
    }

    fun->result.flags |= LowerValue::Implicit;
    return true;
}

/*
 * A global read or written through its own address, which needs no register either.
 *
 * `[rip + g]` is an addressing mode, so an access whose address *is* a global carries the symbol in
 * its own displacement and never loads the address at all. Materializing it costs a seven-byte `lea`
 * in front of every access, a register to hold the answer - which in this backend's conventions is
 * usually a callee-saved one, so it costs a push and a pop as well - and, because the address is
 * rematerializable rather than kept live, the same `lea` again at the next access. `allocateHeap`
 * carried five of them for four globals.
 *
 * The condition is that every use is *the* address operand of its instruction. Which operand that is
 * comes from the opcode rather than the selected form, for the reason opcodeAddressOperand states:
 * selection has not run yet. A use anywhere else - the value of a store, a call argument, an operand
 * of arithmetic - is the address wanted as a number, and a number lives in a register.
 *
 * The one shape this deliberately does not reach is a global folded into an addressing mode with a
 * base or an index: `[rip + disp]` has neither field, so a global that reached an `X86Address` is
 * already committed to a register and its use is not an address operand here. That is the same
 * answer for the same reason, arrived at without a case.
 */
bool tryFoldGlobalAddress(LowerBase base, LowerInstGlobal* global) {
    for(auto offset: global->result.uses.contents(base)) {
        auto use = base[offset];
        auto operand = opcodeAddressOperand(opcodeFor(base, use));
        if(operand < 0) return false;

        auto used = use->used();
        for(Size i = 0; i < used.size(); i++) {
            if(base[used[i]] == &global->result && I32(i) != operand) return false;
        }

        if(base[used[operand]] != &global->result) return false;
    }

    global->result.flags |= LowerValue::Implicit;
    return true;
}

/*
 * Casts that extend nothing.
 *
 * An unsigned cast between an integer and a wider one is a move at the narrower of the two widths,
 * because a 32-bit `mov` clears the upper half of its destination rather than preserving it - so one
 * encoding both truncates a 64-bit source and zero-extends a 32-bit one (see FormCastMov). The
 * allocator then usually gives the result the register its source is vacating, and the whole cast
 * becomes `mov eax, eax`: a real instruction, and one that is doing nothing whenever the upper half
 * it clears was already clear.
 *
 * It nearly always was. Every AMD64 instruction with a 32-bit destination clears bits 32-63 of it,
 * so a value some other 32-bit operation produced arrives already extended and the cast has nothing
 * left to do. The two questions below are that: which definitions carry the guarantee, and which
 * casts are asking for exactly it.
 */

// How far isZeroExtended will follow a 64-bit operation into its operands. The kinds that need it -
// masking and shifting a value down - come in short chains a container's capacity arithmetic is the
// shape of (`(header >> 30) & 0x3fffffff`), and a budget rather than a visited set is what keeps a
// question asked once per cast from walking a whole dataflow graph.
static const U32 kExtendedDepth = 4;

// Whether the register holding `value` is known to have its upper 32 bits clear.
//
// A property of the defining *instruction* rather than of the value's type, in both directions. An
// Int32 that arrived in an argument register or came back from a call is a 32-bit value whose upper
// half nobody promised anything about - neither the System V convention nor this backend's own
// clears it - while an Int64 produced by an unsigned widening is a 64-bit value that provably is
// clear, since the move that widened it is what cleared it.
//
// Conservative by construction: a kind not named here is answered "no", which costs a peephole and
// never an answer. Spilling does not disturb it either way, since a slot is exactly as wide as the
// value in it and the reload fills the same bits the definition did.
static bool isZeroExtended(LowerBase base, LowerValue* value, U32 depth = kExtendedDepth) {
    auto inst = value->inst();
    auto type = value->type;

    // A 32-bit result is the whole of the common case and needs nothing looked up: every one of the
    // operations below is emitted at its result's own width and writes the whole destination
    // register, so a 32-bit one of them clears the rest of it.
    auto narrow = type == LowerType::Int32;

    switch(inst->kind) {
        // A constant is known exactly, whatever it is materialized by.
        case LowerInst::Imm:
            return isIntLike(type) && ((LowerImm*)inst)->i <= 0xffffffffull;

        /*
         * A copy, which is as clear as its source and no clearer.
         *
         * This answered `narrow` unconditionally and was **wrong**, on the same ground the refusal
         * of `Set` in `readsLowHalfOnly` below stands on: `mov r32, r32` would do the clearing
         * itself, but the allocator coalesces a copy whose source dies into no instruction at all,
         * and the register the copy "wrote" is then the source's with the source's upper half still
         * in it. `set %arg; cast to Long` was two empty encodings and a 64-bit read of a register
         * the System V convention says nothing about above bit 31 - see `keepThroughSet` and
         * `keepSetOfArgument` in test/x64/UpperHalf.lower.
         *
         * Following the source rather than refusing outright, because the copy is transparent in
         * both directions: a `set` of something already clear is clear whether or not the move
         * survives, which is the case the corpus actually has.
         */
        case LowerInst::Set: {
            if(depth == 0) return false;
            return isZeroExtended(base, base[((LowerInstUnary*)inst)->from], depth - 1);
        }

        case LowerInst::Neg:   case LowerInst::Not:
        case LowerInst::Add:   case LowerInst::Sub:
        case LowerInst::Mul:   case LowerInst::IMul:
        case LowerInst::Div:   case LowerInst::IDiv:
        case LowerInst::Rem:   case LowerInst::IRem:
        case LowerInst::MulHi: case LowerInst::IMulHi:
        case LowerInst::Shl:   case LowerInst::Sar:

        // The rotations and the byte reversal, which write their destination exactly as the shifts
        // above do - `rol r32`, `rorx r32, r/m32, imm8`, `bswap r32`. They were absent rather than
        // excluded: each was added to the IR after this list was written, which is the drift a list
        // of kinds collects and the reason inst.def's columns exist.
        case LowerInst::Rol:   case LowerInst::Ror:
        case LowerInst::Bswap:

        // `crc32 r32, r/m32`, whose result is a checksum and whose destination is the accumulator
        // written whole. The 64-bit form is the one that would not qualify, and `narrow` is what
        // separates them.
        case LowerInst::Crc32:

        /*
         * The conditional move, and the one entry here that is not "the instruction wrote the
         * register" but "the instruction wrote the register **either way**".
         *
         * `cmovcc r32, r/m32` zero-extends its destination in 64-bit mode whether or not the
         * condition held - the destination is architecturally always written at a 32-bit operand
         * size, which is exactly why it cannot be used to preserve an upper half. So a select whose
         * arms are Int32 leaves a clear register on both paths, and the tie that put the first arm
         * there beforehand does not enter into it.
         */
        case LowerInst::Select:

        // A lane taken out of a vector, which crosses banks as `movd`/`pextrb`/`pextrw`/`pextrd`
        // into a general register - all four write the 32-bit destination and none has a form that
        // preserves anything above it.
        case LowerInst::VecLane:

        /*
         * And the four x86 kinds a transform above this one produced, each of them a rewrite of
         * something this list already answered for.
         *
         * That is what makes them worth naming rather than a completeness exercise: `selectBitOps`
         * turns `not a; and n, b` into `x86_andnot` and `x, x - 1` into `x86_lowbit_clear`, and
         * `selectByteSwapAccesses` turns a load and a `bswap` into `x86_movbe_load` - so a value
         * that *was* clear here stopped being clear the moment the rewrite that made it cheaper ran.
         * `andn r32`, `blsr r32`, `movsx r32, r/m8` and `movbe r32, m32` all write a 32-bit
         * destination.
         */
        case LowerInst::X86AndNot:
        case LowerInst::X86LowBit:
        case LowerInst::X86Sext:
        case LowerInst::X86MovbeLoad:
            return narrow;

        // Masking cannot set a bit its operands do not have between them, so one clear operand is
        // enough however wide the operation is. This is the rule that reaches a container's
        // capacity: `and rax, 0x3fffffff` at Long is as extended as a 32-bit operation's result.
        case LowerInst::And: {
            if(narrow) return true;
            if(depth == 0) return false;

            auto binary = (LowerInstBinary*)inst;
            return isZeroExtended(base, base[binary->lhs], depth - 1)
                || isZeroExtended(base, base[binary->rhs], depth - 1);
        }

        // Where a bitwise operation can, so both sides have to be clear.
        case LowerInst::Or: case LowerInst::Xor: {
            if(narrow) return true;
            if(depth == 0) return false;

            auto binary = (LowerInstBinary*)inst;
            return isZeroExtended(base, base[binary->lhs], depth - 1)
                && isZeroExtended(base, base[binary->rhs], depth - 1);
        }

        // A logical shift only moves bits down: a clear operand stays clear, and a constant count of
        // 32 or more leaves nothing above bit 31 whatever went in. The arithmetic shift above has
        // neither property, since it fills from the sign bit.
        case LowerInst::Shr: {
            if(narrow) return true;

            auto binary = (LowerInstBinary*)inst;
            auto count = base[binary->rhs];
            if(count->inst()->kind == LowerInst::Imm && immValue(count) >= 32) return true;

            return depth > 0 && isZeroExtended(base, base[binary->lhs], depth - 1);
        }

        /*
         * A phi, which is as clear as every value that reaches it - the same shape `Or` and `Xor`
         * have, and for a reason that is about the allocator rather than about an encoding.
         *
         * A phi is no instruction. It is resolved into a copy on each incoming edge, and
         * `resolvePhis` gives every one of those copies the class of the *phi's own type* - so an
         * Int32 phi is `mov r32, r32`, `mov r32, [slot]`, `mov [slot], r32` or `xchg r32, r32`
         * between two 32-bit registers, and each of those clears what it does not write. A slot is
         * packed to the value's width (`stackSlotClassFor`), so a spill and reload of one is 32 bits
         * in both directions.
         *
         * And where the allocator emits no copy at all, the register the phi "wrote" is the
         * source's - which is exactly the hole `Set` above fell into, and is why this is the
         * sources' answer rather than a claim of its own.
         *
         * The recursion terminates on the budget: a loop's phi reaches itself, and the answer at
         * depth zero is the refusal. In practice it rarely gets there - the value on a back edge is
         * usually an `add` or an `and`, which answers from its own type without consulting the phi.
         */
        case LowerInst::Phi: {
            if(depth == 0) return false;

            for(auto used: inst->used()) {
                if(!isZeroExtended(base, base[used], depth - 1)) return false;
            }

            return true;
        }

        // A comparison materialized into a register is a zero-extension by construction - `setcc`
        // into a byte the sequence either zeroed first or `movzx`-es afterwards - and its value is 0
        // or 1 regardless. One folded into the flags has no register at all to answer for.
        case LowerInst::Cmp:
            return !isImplicit(value);

        // The movemask, which is the one reduction still standing this late - every other kind was
        // expanded into a tree by `lowerVectorReductions`. `pmovmskb r32` writes a 32-bit register
        // and clears the rest of it, and it has no wider form to be confused with.
        case LowerInst::VecReduce:
            return narrow;

        /*
         * The intrinsics whose answer is a bit *count* rather than a value: `popcnt` and the bit
         * scans all answer at most 64, so every bit above the low byte is clear whatever width the
         * instruction ran at. A cast of one of these is a name for the register rather than a `mov`.
         *
         * **`Bsr` is deliberately not among them, and `Cttz` is only because of what it promises.**
         * `bsf` and `bsr` leave the destination *unwritten* for a zero operand, so a 32-bit one does
         * not clear the upper half of its register and what is above the answer is whatever was
         * there before. `Cttz`'s contract is that its operand is never zero (see the row in
         * intrinsic.cpp, and every emitter of it), so the case cannot arise; `Bsr` is emitted by
         * `expandBitScans` with an operand that may well be zero, and the select downstream is what
         * discards the answer rather than anything about the register.
         */
        case LowerInst::Intrinsic: {
            if(!isIntLike(type)) return false;

            auto which = ((LowerInstIntrinsic*)inst)->getIntrinsic();
            return which == LowerIntrinsic::Popcnt
                || which == LowerIntrinsic::Cttz
                || which == LowerIntrinsic::CttzWidth
                || which == LowerIntrinsic::ClzWidth;
        }

        // Anything loaded at four bytes or fewer lands in a register the load itself filled: the
        // narrow forms extend into the result's own width, and a four-byte one is `mov r32` unless
        // it is the signed widening `movsxd`. A sign extension is the one that carries a bit up.
        case LowerInst::Load: {
            auto load = (LowerInstLoad*)inst;
            if(!isIntLike(type) || load->getWidth() > 4) return false;
            return !load->isSigned() || !is64Bit(type);
        }

        // The same load, under the same rule. An acquire or relaxed load of four bytes or fewer is
        // an ordinary `mov` on this architecture and a seq_cst one is too - the ordering is the
        // fences around it rather than the width it writes - so nothing about being atomic changes
        // which bits of the destination the instruction fills.
        case LowerInst::AtomicLoad: {
            auto load = (LowerInstAtomicLoad*)inst;
            if(!isIntLike(type) || load->getWidth() > 4) return false;
            return !load->isSigned() || !is64Bit(type);
        }

        // And a cast, which is the case that makes two of these in a row collapse into one. The
        // marking below does not enter into it: a cast with a 32-bit end moves at 32 bits, and one
        // this peephole marked instead inherits a source it has already required to be clear - so
        // the answer is the same either way, and this question stays independent of the order the
        // casts are visited in.
        case LowerInst::Cast: {
            auto cast = (LowerInstCast*)inst;
            auto source = base[cast->from];
            auto from = source->type;
            if(!isIntLike(from) || !isIntLike(type)) return false;

            if(isImm(source)) return immValue(source) <= 0xffffffffull;
            if(!is64Bit(from) && is64Bit(type) && cast->isSignedSource() && cast->isSignedResult()) {
                return false;
            }

            return !is64Bit(from) || !is64Bit(type);
        }

        default:
            return false;
    }
}

/*
 * Whether bit 31 of a 32-bit value is known clear - that is, whether it is non-negative read as a
 * signed `Int32`.
 *
 * The companion to the question above, and the two answer different halves of one register: that one
 * is about bits 32-63, this one about bit 31. Together they say a 32-bit definition already holds its
 * own sign extension, which is what makes a *signed* widening of it emit nothing.
 *
 * Only the kinds whose result cannot reach bit 31 at all, so this needs no range arithmetic: masking
 * against a constant that does not have it, shifting down by a constant, a comparison, a load too
 * narrow to reach it, and a widening from something narrower. `Bits.crc` in the corpus is the shape
 * it was written for - `sext (and %x, 255)`, whose 255 says the answer is eight bits wide and whose
 * `movslq` was therefore a copy.
 *
 * An `add`, a `mul` and a shift *up* are deliberately absent even where their operands qualify: each
 * of them can carry into bit 31, and the whole value of this question is that it never has to reason
 * about how far.
 */
static bool isNonNegative32(LowerBase base, LowerValue* value) {
    return value->type == LowerType::Int32 &&
           (knownZeroBits(base, value) & (U64(1) << 31));
}

/*
 * Whether one instruction reads `value` at 32 bits and nothing wider.
 *
 * The other half of the truncation question, and it is asked forwards where `isZeroExtended` is
 * asked backwards. That one says the bits a truncating `mov` would clear are already clear; this one
 * says nobody looks at them - and a truncation nobody looks at past is as removable as one that has
 * nothing to do. It is the analysis §9.4.1 named and did not build: `stringLiteral`'s surviving
 * `mov eax, eax` is followed by 32-bit `and`s alone, and no fact about where the value came from
 * will ever make that one go.
 *
 * A whitelist, because the safe answer is "no" and the shapes that are safe are few:
 *
 *  - **an operation every one of whose operands and results is a 32-bit integer**, which is the whole
 *    of the arithmetic. `OperationWidth` is derived from those types, so such an instruction encodes
 *    at 32 bits and reads exactly 32 - and, being a 32-bit destination, hands on a register that is
 *    clear again. The *whole* instruction has to be narrow rather than only this operand: `add ptr,
 *    int` and `and ptr, int` are both spelled in this IR and both run at 64 bits.
 *  - **a store of four bytes or fewer**, which writes the low half and reads no more of it. Asked of
 *    the stored value only - a store's address operand is a pointer and would fail the rule above.
 *  - **a splat**, which crosses into the vector bank through `movd r32` and takes four bytes with it.
 *
 * And the refusals that matter, none of which is merely conservative:
 *
 *  - **another `Cast`, and a `Set`.** Not because either reads too much - `movsxd`, the truncating
 *    `mov` and `mov r32, r32` all read 32 bits - but because both are copies, and `isZeroExtended`
 *    answers for a copy by its *type*: a cast with a 32-bit end moves at 32 bits and therefore
 *    clears, and so does a 32-bit `Set`. Neither claim survives this marking. The cast may already
 *    have been marked on the strength of the answer, and a `Set` the allocator coalesces away is not
 *    a `mov` at all - the register it "wrote" is the source's, with whatever this peephole just
 *    licensed leaving in the top of it. Declining here keeps that answer independent of this
 *    marking, which is the property §9.4 established and the one thing here that would silently
 *    produce a wrong register.
 *
 *    This is also the line the additions below stand on the right side of. Every kind added to the
 *    whitelist emits a real instruction that writes its whole 32-bit destination, and none of them
 *    is a copy the allocator can make disappear.
 *  - **a call, a return, a phi and a branch.** The first two are the ABI question - no convention
 *    here promises anything about the upper half of a 32-bit argument or result, so this backend
 *    must not start depending on one - and a stack-passed argument becomes an `X86PushArg` that
 *    stores eight bytes, which is a use that does not exist yet when this is asked. A phi is a copy
 *    the allocator places and a value read where this cannot see the reader.
 *  - **an address.** An index register in a SIB byte is read at 64 bits whatever the value's type
 *    says, which is the one place a 32-bit value is silently used as a wide one.
 */
static bool readsLowHalfOnly(LowerBase base, LowerInst* user, LowerValue* value) {
    auto narrow = [](LowerType type) { return isIntLike(type) && !is64Bit(type); };

    switch(user->kind) {
        case LowerInst::Store: {
            auto store = (LowerInstStore*)user;
            return store->value == value - base && store->getWidth() <= 4;
        }

        // The two stores a transform above this one folded a store into, held to the same rule and
        // asked of the same operand: `add [mem], r32` and `movbe [mem], r32` read the register at
        // the width the access states and no more. Absent rather than excluded - `selectStoreUpdates`
        // and `selectByteSwapAccesses` both run above `selectMachineInstructions`, so a store this
        // answered for stopped being answered for as soon as it was rewritten.
        case LowerInst::X86StoreOp: {
            auto store = (LowerInstX86StoreOp*)user;
            return store->value == value - base && store->getWidth() <= 4;
        }

        case LowerInst::X86MovbeStore: {
            auto store = (LowerInstX86MovbeStore*)user;
            return store->value == value - base && store->getWidth() <= 4;
        }

        // The scalar crosses banks as `movd r32, xmm`, which is four bytes and no more.
        case LowerInst::VecSplat:
            return true;

        case LowerInst::Neg: case LowerInst::Not:
        case LowerInst::Add: case LowerInst::Sub:
        case LowerInst::Mul: case LowerInst::IMul:
        case LowerInst::Div: case LowerInst::IDiv:
        case LowerInst::Rem: case LowerInst::IRem:
        case LowerInst::MulHi: case LowerInst::IMulHi:
        case LowerInst::Shl: case LowerInst::Shr: case LowerInst::Sar:
        case LowerInst::And: case LowerInst::Or: case LowerInst::Xor:
        case LowerInst::Cmp: case LowerInst::Select:

        // The rest of the arithmetic, which was missing for the reason the list above `isZeroExtended`
        // gives: each of these was added to the IR after this whitelist was written. The type test
        // below is what actually admits them, and it is the same test the others pass.
        case LowerInst::Rol: case LowerInst::Ror:
        case LowerInst::Bswap: case LowerInst::Crc32:

        // And the three rewrites, for the same reason as the two stores above: `andn r32`,
        // `blsr r32` and `movsx r32, r/m8` read 32 bits of a 32-bit operand, and the pair each of
        // them replaced was already answered for here.
        case LowerInst::X86AndNot: case LowerInst::X86LowBit: case LowerInst::X86Sext: {
            for(auto& created: user->created()) if(!narrow(created.type)) return false;
            for(auto used: user->used()) if(!narrow(base[used]->type)) return false;
            return true;
        }

        default:
            return false;
    }
}

// Whether no instruction anywhere reads bits 32-63 of this value, so that a truncation into it may
// leave whatever was there. A value with no users at all answers false: there is nothing to gain and
// the shape is a dead instruction rather than a live one this is about.
static bool upperHalfUnread(LowerBase base, LowerValue* value) {
    if(value->type != LowerType::Int32) return false;
    if(value->uses.size() == 0) return false;

    for(auto userPtr: value->uses.contents(base)) {
        if(!readsLowHalfOnly(base, base[userPtr], value)) return false;
    }

    return true;
}

// Marks a cast whose move would change no bit of its source, so that selection gives it the form
// that emits nothing when the allocator has put the two in one register (FormCastCopy).
//
// Two shapes qualify. A cast between two 64-bit types moves at 64 bits and is a plain copy already,
// whatever either end calls itself - a refinement widening to the type it refines is the one that
// reaches here. And a cast with a 32-bit end is the zero-extending truncating move, which is a copy
// exactly when the bits it would clear are clear.
//
// The signed widening is not one of them: `movsxd` is a different instruction with something real to
// do. Nor is a constant source, which makes the cast a materialization rather than a move.
bool trySkipCastExtend(LowerBase base, LowerInstCast* cast) {
    auto source = base[cast->from];
    auto from = source->type;
    auto to = cast->result.type;

    if(!isIntLike(from) || !isIntLike(to)) return false;
    if(isImm(source)) return false;

    if(is64Bit(from) && is64Bit(to)) {
        cast->setSkipsExtend(true);
        return true;
    }

    /*
     * The signed widening, which is `movsxd` and has something real to do - unless the bit it would
     * copy upwards is a zero, in which case it is copying the same zeros the unsigned widening
     * writes and the two are one instruction. `isNonNegative32` is that question; the general one
     * below then asks whether even that move is needed.
     */
    if(!is64Bit(from) && is64Bit(to) && cast->isSignedSource() && cast->isSignedResult() &&
       !isNonNegative32(base, source)) {
        return false;
    }

    /*
     * Either end of the question: the bits are already clear, or nobody looks at them.
     *
     * The second is asked of the cast's *result* and the first of its source, and they are
     * independent - a truncation of a function argument can never satisfy the first (no convention
     * promises the upper half of one) and satisfies the second whenever the value stays 32-bit
     * arithmetic, which is what `n = truncate(length)` at the top of every container loop is.
     */
    if(!isZeroExtended(base, source) && !upperHalfUnread(base, &cast->result)) return false;

    cast->setSkipsExtend(true);
    return true;
}

// Tries to swap operands to the provided instruction in order to make it easier to perform further optimizations.
// This needs to be done before register allocation,
// since swapping and then embedding may reduce the number of registers needed.
bool trySwapOperands(LowerBase base, LowerInst* inst) {
    if(!isBinary(inst)) return false;

    auto binary = (LowerInstBinary*)inst;
    if(!isIntLike(binary->result.type)) return false;

    // For register and memory operands, both directions can be encoded, so it is pointless to swap.
    // Because of that, we only check if immediates can swapped.
    if(base[binary->lhs]->inst()->kind != LowerInst::Imm) return false;

    // Only swap for operations that are safe.
    auto kind = binary->kind;
    if(!(kind == LowerInst::Add || kind == LowerInst::Mul || kind == LowerInst::IMul ||
       kind == LowerInst::And || kind == LowerInst::Or || kind == LowerInst::Xor)) return false;

    // Swap lhs with rhs to ensure the immediate is on the right side.
    ::swap(binary->lhs, binary->rhs);
    return true;
}

/*
 * The two floating-point comparisons AMD64 can only answer the other way round.
 *
 * UCOMISS/UCOMISD leave their answer in the flags an *unsigned* integer comparison uses, and set CF,
 * ZF and PF together when either operand is a NaN. That makes `ja` and `jae` - the codes `gt` and
 * `ge` take - read correctly without anything further: an unordered comparison has CF set, so both
 * are false, which is what an ordered comparison of a NaN has to be.
 *
 * `jb` and `jbe` are the same codes read the other way, so both are *true* for a NaN, and there is
 * no condition code that is ordered-below. What there is instead is the identity: `a < b` is `b > a`
 * for every pair the comparison is defined on, and false for every pair it is not. So the operands
 * are exchanged and the comparison rewritten, here, once - rather than in the encoder, where the
 * folding that carries a comparison into a branch or a select would each have to know about it.
 *
 * Equality is not fixable this way and is handled where the flags are read - see tryMergeCompare and
 * genFloatFlagsToReg.
 */
bool orderFloatCompare(LowerBase base, LowerInst* inst) {
    if(inst->kind != LowerInst::Cmp) return false;

    auto cmp = (LowerInstCmp*)inst;
    if(!isFloat(base[cmp->lhs]->type)) return false;

    switch(cmp->getCmp()) {
        case LowerCmp::lt: cmp->setCmp(LowerCmp::gt); break;
        case LowerCmp::le: cmp->setCmp(LowerCmp::ge); break;
        default: return false;
    }

    ::swap(cmp->lhs, cmp->rhs);
    return true;
}

/*
 * The same exchange for a packed comparison, in the other direction and for a different reason.
 *
 * A packed comparison answers a mask rather than the flags, so there is no NaN asymmetry to work
 * around: `cmpps` states its relation in a predicate byte and every one of the eight is exact. What
 * is missing is only that four of them have no predicate of their own - `a > b` is written `b < a`,
 * with the predicate for "less" - so `gt` and `ge` are turned into `lt` and `le` here rather than in
 * the encoder, which is the opposite of the direction the scalar rule above moves them.
 *
 * An integer comparison has a narrower reason and the same shape. The machine has `pcmpeqd` and
 * `pcmpgtd` and nothing else, so `ilt` is `igt` with the operands the other way round; `ige`, `ile`
 * and `neq` would need the mask inverted afterwards, and selection refuses them.
 */
bool orderPackedCompare(LowerBase base, LowerInst* inst) {
    if(inst->kind != LowerInst::Cmp) return false;

    auto cmp = (LowerInstCmp*)inst;
    if(!isVectorLike(base[cmp->lhs]->type)) return false;

    // Which of the four they are is `packedCompareRelation`'s answer rather than a second switch
    // here: `checkVectorSupported` reads the same function from the other side of this pass, and a
    // relation the two disagreed about is one refused before it could be exchanged.
    auto exchanged = packedCompareRelation(cmp->getCmp());
    if(exchanged == cmp->getCmp()) return false;

    cmp->setCmp(exchanged);
    ::swap(cmp->lhs, cmp->rhs);
    return true;
}

// Whether this comparison's result is of a shape that could be left in the flags at all, before
// anything is asked about what stands between it and the things that read it.
static bool canCarryInFlags(LowerBase base, LowerInstCmp* cmp) {
    /*
     * A floating-point equality is not a condition code.
     *
     * UCOMISS answers "equal" in ZF and "unordered" in PF, and both are set at once by a NaN - so
     * ordered equality is `ZF and not PF` and inequality is `not ZF or PF`. Neither is one `setcc`
     * and neither is one `jcc`.
     *
     * **Into a register that is fatal and into a branch it is not**, and that is the distinction
     * this used to miss by refusing both. A value has to be a single number, so the register form
     * is `setcc`, a short jump over a correction, and the correction - which is what
     * genFloatFlagsToReg emits and the only way to write it. A *branch* has two of everything
     * already: it has two arms, and the parity case simply names one of them. `jp F ; je T` is the
     * whole of it, and it reads the comparison's own flags where the register form re-derived them
     * through `setcc ; jnp ; xor ; test`.
     *
     * So the refusal is now about where the answer is going. A `Select` still takes the register
     * route - a conditional *move* has one condition code and no second arm to hang the parity case
     * on - and a branch carries the flags like any other comparison, with emitBranch spending the
     * one extra jump. Eleven bytes down to four, at every `x == y` on a float in the language.
     *
     * The ordering comparisons were never affected - canonicalizeOperands has already put them all
     * in the `gt`/`ge` form, where CF alone is the answer and a NaN makes it false.
     */
    if(isFloat(base[cmp->lhs]->type)) {
        auto kind = cmp->getCmp();

        if(kind == LowerCmp::eq || kind == LowerCmp::neq) {
            for(auto offset: cmp->result.uses.contents(base)) {
                if(base[offset]->kind != LowerInst::Je) return false;
            }
        }
    }

    auto result = &cmp->result - base;

    for(auto offset: cmp->result.uses.contents(base)) {
        auto use = base[offset];

        // If the result is used as an actual value, it needs to written to a register.
        if(use->kind != LowerInst::Je && use->kind != LowerInst::Select) return false;

        /*
         * And a select reads three operands where a branch reads one, so being *a* use of it is not
         * being the condition of it. `a && b` if-converts to `select %c, %b, %c` - the comparison is
         * the condition and the value the false arm produces - and folding that into the flags would
         * leave the arm with no register to be read out of, which is what the selection verifier
         * reports as "folds an operand that still needs a location".
         */
        if(use->kind == LowerInst::Select) {
            auto& select = *(LowerInstSelect*)use;
            if(select.lhs == result || select.rhs == result) return false;
        }
    }

    return true;
}

/*
 * The window between a comparison and the things that read it.
 *
 * A comparison whose flags are still intact where they are read needs no register at all: the branch
 * or the select reads ZF and CF directly, which is what tryMergeCompare below arranges. Anything in
 * between that writes the flags takes that away, and the comparison becomes a `setcc`, a
 * zero-extension and a `test` against the result - six instructions where two would do.
 *
 * The clobber is usually not something that had to be there. `select %c, %v, 0` materializes its
 * zero as `xor r, r` - three bytes where `mov r, 0` is five, which is the whole reason that form
 * exists - and lowering puts it where it is read, which is inside the window. Nothing about that
 * instruction depends on the comparison, so it can simply be computed first.
 *
 * Hoisting whatever can be moved is therefore part of the fold rather than a pass of its own: the
 * motion is only worth performing where it makes the merge happen, and only the merge knows that.
 * §2 of test/bench/findings.md measures what it is worth - 1.85x on `conditionalSum` and 1.34x on
 * `countBytes`, which is where a three-byte size peephole was costing a factor of two.
 */

// Moves the instruction at `from` in `block`'s list up to `to`, shifting what it passes down one.
//
// Only the list order changes: the instruction keeps its block and every value it reads keeps its
// use, so nothing outside the list has to be told. Positions within the block do move, which is why
// the caller has to carry the shift back into its own walk.
static void moveInstTo(LowerBase base, LowerBlock* block, Size from, Size to) {
    assertTrue(to <= from); // this only ever lifts an instruction earlier
    auto inst = block->instructions.get(base, from);

    for(Size i = from; i > to; i--) {
        block->instructions.set(base, i, block->instructions.get(base, i - 1));
    }

    block->instructions.set(base, to, inst);
}

// How far a comparison's flags have to survive, as an index one past the last instruction that could
// clobber them - so the terminator, which is not in the list, is the list's own size.
//
// Nothing() when a use sits somewhere this cannot answer for: another block, which the walk does not
// follow. TODO: follow paths between blocks from the definition to the use.
static Maybe<Size> flagsWindowEnd(LowerBase base, LowerInstCmp* cmp, Size index) {
    auto block = base[cmp->block];
    auto list = block->instructions.contents(base);
    auto terminator = base[block->terminator];

    // The empty window, which is what a comparison read by the instruction directly below it has.
    Size end = index + 1;

    for(auto offset: cmp->result.uses.contents(base)) {
        auto use = base[offset];
        if(use->block != cmp->block) return Nothing();

        if(use == terminator) {
            if(list.size() > end) end = list.size();
            continue;
        }

        Size at = index + 1;
        for(; at < list.size(); at++) {
            if(base[list[at]] == use) break;
        }

        assertTrue(at < list.size()); // a use in the comparison's own block, but not below it
        if(at > end) end = at;
    }

    return Just(end);
}

/*
 * Whether this instruction can be computed before the comparison rather than after it.
 *
 * Only a value computed in registers from registers qualifies: it reads and writes no memory, so
 * moving it above the loads and stores that share the window changes nothing, and it cannot fault,
 * so it cannot change what has run when something else does. `kLowerPure` with no memory bit is that
 * statement, which rules out a call and a load; `kLowerDivides` rules out the divisions, which are
 * far too expensive for their position to be what a window costs.
 *
 * A comparison is left out deliberately, and it is the one exclusion that is a policy rather than a
 * fact: lifting one above another only exchanges which of the two windows the clobber sits in, and
 * the one it moves into is the one already being fixed.
 *
 * `kLowerUsesFlags` is the other refusal worth naming. A select whose comparison a peephole already
 * folded into it *reads* the flags where it stands, so moving it above a comparison moves it out of
 * the window it was reading - which is the exact failure this pass exists to avoid, arrived at from
 * the other side.
 */
static bool canHoistOverCompare(LowerInst* inst) {
    if(inst->kind == LowerInst::Cmp) return false;
    if(!isPure(inst) || mayFault(inst)) return false;

    return !hasLowerTrait(inst, kLowerReads | kLowerWrites | kLowerOrdered | kLowerDivides | kLowerUsesFlags);
}

// Empties the window of everything that writes the flags, by lifting each such instruction above the
// comparison. Answers false, having moved nothing, when even one of them has to stay - a partial
// move buys nothing, since a single clobber left behind costs the same as all of them.
//
// An instruction that goes up takes whatever it reads from inside the window with it, which is what
// the backwards walk is for: an operand is always defined above its reader, so walking up from the
// use means everything that has to travel is already known by the time its definition is reached.
// The window's own `add r, 1` is the ordinary case - the immediate it reads is an instruction of its
// own, sitting between the comparison and the add, and writing no flags at all.
//
// Anything that has to travel and cannot - a load, whose position is not free to change - ends the
// attempt, and so does anything reading the comparison's own result, which is never a condition.
static bool clearFlagsWindow(LowerBase base, LowerInstCmp* cmp, Size index, Size end, Size& hoisted) {
    auto block = base[cmp->block];
    auto list = block->instructions.contents(base);

    // The values the instructions being lifted read, and where those instructions are - collected in
    // descending order of position, since that is the direction the walk goes.
    SmallArray<LowerValue*, 16> needed;
    SmallArray<Size, 8> lift;

    for(Size i = end; i-- > index + 1;) {
        auto inst = base[list[i]];
        bool lifts = modifiesFlags(base, inst);

        if(!lifts) {
            for(auto& value: inst->created()) {
                if(needed.containsValue(&value)) { lifts = true; break; }
            }
        }

        if(!lifts) continue;
        if(!canHoistOverCompare(inst)) return false;

        for(auto use: inst->used()) needed.push(base[use]);
        lift.push(i);
    }

    if(needed.containsValue(&cmp->result)) return false;

    // Lifted in ascending order, so that they arrive above the comparison in the order they had.
    // Each move shifts only what lies between the comparison and the instruction being lifted, so
    // the positions collected above stay correct for the ones still to come.
    for(Size i = 0; i < lift.size(); i++) moveInstTo(base, block, lift[lift.size() - i - 1], index + i);

    hoisted = lift.size();
    return true;
}

/*
 * §3.5.2.1 A comparison that is materialized *and* branched on.
 *
 * The fold above is all or nothing: it needs every use of the comparison to be one that can read the
 * flags, and it needs them all in the comparison's own block. A comparison read by anything else -
 * a store, a call argument, a phi in a successor - therefore keeps its register, and the branch that
 * reads it as well goes back to `test r, r; jcc`, re-deriving from a register the flags it is
 * standing next to. `setcc` and the `movzx` behind it write no flags, so those flags are still the
 * comparison's where the branch reads them.
 *
 * That is the third `Jcc` form: a branch on the flags whose condition operand is still an ordinary
 * register operand, because the value it names is genuinely live. It emits the `jcc` and not the
 * `test`, which is the two bytes, and nothing about the materialization changes.
 *
 * What it needs is exactly the window the fold above needs, measured to the terminator rather than
 * to the last use - the other uses read a register and do not care what the flags hold. Nothing is
 * hoisted for it: lifting an instruction out of the window is worth doing for six instructions and
 * not for two, and a window this cannot have is one the fold above already tried to clear.
 */
static bool tryBranchOnLiveCompare(LowerBase base, LowerInstCmp* cmp, Size index) {
    // The same shapes the fold above refuses, and for the same reason: a float equality is not a
    // condition code, and its materialization corrects the NaN case with an instruction that writes
    // the flags (genFloatFlagsToReg) - so there is nothing here for a branch to read.
    if(isFloat(base[cmp->lhs]->type)) {
        auto kind = cmp->getCmp();
        if(kind == LowerCmp::eq || kind == LowerCmp::neq) return false;
    }

    auto block = base[cmp->block];
    auto branch = base[block->terminator];
    if(branch->kind != LowerInst::Je) return false;

    auto je = (LowerInstJe*)branch;
    if(je->getEmbeddedCmp()) return false;   // already reading the flags
    if(base[je->cond] != &cmp->result) return false;

    // Everything between the comparison and the terminator has to leave the flags alone. The
    // materialization itself is not in the list and does not: `setcc` writes a byte and `movzx`
    // extends it, and the `xor` that clears the register ahead of the comparison is emitted in front
    // of it rather than behind it (see genSetCc).
    auto list = block->instructions.contents(base);
    for(Size i = index + 1; i < list.size(); i++) {
        if(modifiesFlags(base, base[list[i]])) return false;
    }

    je->setEmbeddedCmp(Just(cmp->getCmp()));
    return true;
}

/*
 * §3.5.2.2 The comparison the instruction above it already performed.
 *
 * `while i != 0: i = i - 1` compiles to `dec` and then `test`, where LLVM spends only the `dec`:
 * every group-1 operation sets ZF from its own result, so a comparison of that result against zero
 * is asking a question that is standing there answered. The fold above carries a comparison to where
 * it is read; this one removes it outright.
 *
 * Three things decide it, and the third is the one that has to be asked here rather than earlier.
 *
 *  - **the comparison is against zero, and its code is one the flags left behind can answer.**
 *    `== 0` and `!= 0` read ZF alone and every form below answers them. A *signed* comparison reads
 *    SF against OF as well, and an addition that overflowed sets OF to say something about the
 *    operation rather than about the result - `sub a, b; jl` is `a < b` and not `a - b < 0`. The
 *    logical operations clear OF outright, so after one of those `jl` is the sign bit of the result
 *    and the whole signed family is answered too. That is the second table, and it is one field
 *    rather than a table: see `MachineForm::signInFlags`.
 *  - **the left-hand side is produced in this block by a form that leaves its result in ZF.**
 *    `MachineForm::resultInFlags` is that claim, and it is much narrower than writing the flags at
 *    all: `imul`, `mul` and the divisions leave ZF undefined, and a shift by a count of zero leaves
 *    the flags of whatever ran before it.
 *  - **nothing between the two writes the flags.** Asked *after* the merge rather than before it,
 *    because `clearFlagsWindow` lifts instructions out of the window below the comparison and puts
 *    them directly above it - which is inside this window. A comparison whose window had to be
 *    cleared has usually lost this, and that is the right answer rather than a missed one.
 *
 * The instructions in between are also the reason this is not a peephole on the pair: the definition
 * and the comparison are adjacent in the shapes that matter, but a `mov` or a load between them
 * costs nothing and is common, so what is walked is the stretch rather than the one slot above.
 */
// Whether a comparison against zero is one the flags a form leaves behind can answer. `eq`/`neq` are
// ZF and need only the coarser claim; the four signed codes read ZF, SF and OF, which is the whole of
// what a logical operation defines and none of what an arithmetic one leaves meaning the same thing.
static bool answeredByFlags(LowerCmp kind, const MachineForm& form) {
    if(kind == LowerCmp::eq || kind == LowerCmp::neq) return form.resultInFlags;

    auto signed_ = kind == LowerCmp::ilt || kind == LowerCmp::ile ||
                   kind == LowerCmp::igt || kind == LowerCmp::ige;
    return signed_ && form.signInFlags;
}

static void tryElideCompare(LowerBase base, LowerInstCmp* cmp, Size index) {
    auto kind = cmp->getCmp();

    // The embedded constant zero, and nothing else. A zero that is still in a register is one the
    // `xor` materializing it wrote the flags for, so the window below would refuse it anyway - and
    // FormCmpNone declares the immediate operand FormCmpImm declares, so this is also what keeps the
    // two forms interchangeable at the point the choice is made.
    if(!isImplicit(base[cmp->rhs])) return;

    auto rhs = base[cmp->rhs]->inst();
    if(rhs->kind != LowerInst::Imm || ((LowerImm*)rhs)->i != 0) return;

    auto lhs = base[cmp->lhs];
    auto definition = lhs->inst();
    if(definition->block != cmp->block) return;

    // A float compared against a zero *bit pattern* is not this: `ucomisd` is what answers it, and
    // no float operation writes the integer flags at all.
    if(!isIntLike(lhs->type)) return;
    if(!answeredByFlags(kind, machineTarget().form(selectForm(base, definition)))) return;

    // Backwards from the comparison to the definition, which is the direction that stops soonest:
    // the definition is usually the instruction directly above, and a clobber usually is too.
    auto block = base[cmp->block];
    auto list = block->instructions.contents(base);

    for(auto i = index; i-- > 0;) {
        auto inst = base[list[i]];
        if(inst == definition) {
            cmp->setFlagsLive();
            return;
        }

        if(modifiesFlags(base, inst)) return;
    }
}

/*
 * §3.5.2.2 And the same finding where there is no comparison at all to elide.
 *
 * `if !growHeap(n) then ..` reaches this backend as `%c = xor %r, 1` and a branch on `%c` - the
 * negation is arithmetic, and the branch tests a register because that is what a branch on a value
 * does. `FormJccReg` then emits `test %eax, %eax` in front of the `jcc`, re-deriving from a register
 * the answer the `xor` above it left in ZF.
 *
 * The fold is the same one, and it needs no form of its own: a branch on a value is a branch on
 * `value != 0`, so saying so is `setEmbeddedCmp(neq)`, and `FormJccLive` is already the form for a
 * branch that reads the flags while its condition stays an ordinary operand. The condition keeps its
 * register and keeps every other use it has; what goes away is the two bytes that recomputed it.
 *
 * The conditions are `tryElideCompare`'s, minus the one about the comparison kind - there is no
 * comparison, and `!= 0` is the only thing a branch on a value ever asks.
 */
void tryElideBranchTest(LowerBase base, LowerBlock* block) {
    auto terminator = base[block->terminator];
    if(terminator->kind != LowerInst::Je) return;

    auto je = (LowerInstJe*)terminator;
    if(je->getEmbeddedCmp()) return;   // already reading the flags

    auto condition = base[je->cond];
    auto definition = condition->inst();
    if(definition->block != block - base) return;
    if(!isIntLike(condition->type)) return;
    if(!machineTarget().form(selectForm(base, definition)).resultInFlags) return;

    // From the end of the block back to the definition. The window is measured to the terminator
    // because that is what reads the flags, and the terminator is not in the list.
    auto list = block->instructions.contents(base);

    for(auto i = list.size(); i-- > 0;) {
        auto inst = base[list[i]];
        if(inst == definition) {
            je->setEmbeddedCmp(Just(LowerCmp::neq));
            return;
        }

        if(modifiesFlags(base, inst)) return;
    }
}

// Carries a comparison into the branches and selects that read it, so that its answer stays in the
// flags rather than being materialized. Returns how many instructions were lifted above it to make
// that possible, which is how far its own position in the block moved down.
Size tryMergeCompare(LowerBase base, LowerInstCmp* cmp, Size index) {
    /*
     * A packed comparison is not a comparison in the sense any of this is about.
     *
     * Its answer is a mask in a vector register rather than a condition in the flags - it does not
     * write the flags at all - so there is nothing here for it to be carried into. The step that
     * makes it wrong rather than merely pointless is the first one: a comparison nothing reads is
     * marked implicit because a flags-only comparison needs no destination, and a packed one *has* a
     * destination, which its encoding writes over its own first operand. Taking its location away
     * leaves an instruction that clobbers a live value.
     */
    if(isVectorLike(base[cmp->lhs]->type)) return 0;

    auto& uses = cmp->result.uses;

    if(uses.size() == 0) {
        cmp->result.flags |= LowerValue::Implicit;
        return 0;
    }

    if(!canCarryInFlags(base, cmp)) {
        tryBranchOnLiveCompare(base, cmp, index);
        return 0;
    }

    auto end = flagsWindowEnd(base, cmp, index);

    if(end.isNothing()) {
        tryBranchOnLiveCompare(base, cmp, index);
        return 0;
    }

    // Nothing to fall back to here: this is the one refusal that means a flag writer stands between
    // the comparison and something that reads it, which is exactly what the form above cannot have.
    Size hoisted = 0;
    if(!clearFlagsWindow(base, cmp, index, end.unwrap(), hoisted)) return 0;

    // The only uses are instructions that can use flags directly, and nothing writes them in
    // between any more, so the result can stay as flags.
    cmp->result.flags |= LowerValue::Implicit;

    // And then whether the comparison needs to happen at all, which is only worth asking of one
    // that got this far: a comparison still materializing a value emits a `setcc` the flags this
    // would remove are read by, so there would be nothing to elide. The position it is asked at is
    // the one it now has - `clearFlagsWindow` moved it down by `hoisted`.
    tryElideCompare(base, cmp, index + hoisted);

    for(auto offset: uses.contents(base)) {
        auto use = base[offset];

        if(use->kind == LowerInst::Je) {
            ((LowerInstJe*)use)->setEmbeddedCmp(Just(cmp->getCmp()));
        } else if(use->kind == LowerInst::Select) {
            ((LowerInstSelect*)use)->setEmbeddedCmp(Just(cmp->getCmp()));
        }
    }

    return hoisted;
}

/*
 * The sign of a narrow value put back, in one instruction instead of two.
 *
 * `truncateToWidth` in resolve/lower_type.cpp is where every narrow signed value is re-signed - a
 * lane read, a field read, the result of arithmetic at an `I8` or an `I16` - and what it writes is
 * `x << k >> k` at the distance that puts the narrow sign bit in the register's. That is the right
 * portable form: LLVM folds the pair into a `sext` for itself and `(x << 16) >> 16` is the idiom a
 * JS engine recognizes, so two of the three backends want exactly what they are handed.
 *
 * On x86 the pair costs about twice what the instruction does, in three separate ways:
 *
 *  - **Two uops and six bytes** where `movsx` is one and three (four at a 64-bit result, both ways).
 *  - **A copy, whenever the source has another reader.** Both shifts are `tiedDef` two-address
 *    forms, so the value has to be in the register the result will occupy; `movsx` reads its operand
 *    out of a separate field and leaves it alone.
 *  - **The flags.** Both shifts declare `FlagsEffect::Def`; `movsx` writes nothing. A sign-extension
 *    standing between a comparison and the branch that reads it forced the comparison to be redone,
 *    which is the one of the three that costs more than it looks.
 *
 * A spilled operand keeps working and gets better: `emitRegRm` puts an r/m operand left in the frame
 * into a memory ModRM, and `movsx r, byte [slot]` is the same opcode - so a source that did not get
 * a register becomes a sign-extending load rather than a reload and two shifts.
 *
 * ## What is matched
 *
 * The shift pair, at equal constant distances, over a scalar integer in a 4- or 8-byte register, at
 * a distance that leaves a whole number of bytes: 1, 2 or 4. Four is `movsxd` and exists only as a
 * 32-to-64 encoding, so it is taken only where the result is 64 bits wide.
 *
 * **The left shift must have exactly one reader.** With another reader it has to stay, and what the
 * rewrite would buy is then only the flags and the tie - at the cost of keeping the shift's operand
 * live across it as well, which is a register in whatever loop this is in. That is not a trade worth
 * making blind, and the shape `truncateToWidth` writes always has the one reader.
 *
 * A constant source is left alone: `x << k >> k` over an immediate is a fold rather than an
 * instruction, and one of the peepholes below still does it.
 */
void selectSignExtends(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // The shifts and the immediates this leaves behind, cleared after the walk rather than
        // during it: the left shift stands immediately *above* the right one being rewritten, and
        // removing it there would renumber the instructions this loop is indexing. The distance is
        // one `Imm` both shifts name, so it dies only once the second of them has gone.
        InstChain dead;

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Sar) continue;

            auto sar = (LowerInstBinary*)inst;
            auto type = sar->result.type;

            // A scalar integer in a general register. A packed `sar` is a different instruction
            // entirely and `isInt` answers false for one by construction - see LowerType.
            if(!isInt(type)) continue;

            auto registerBytes = type.byteWidth();
            if(registerBytes != 4 && registerBytes != 8) continue;

            // The kind directly rather than `isImm`, which additionally asks whether the constant
            // has been *embedded* into its reader's encoding - a mark `selectMachineInstructions`
            // makes below this pass. Every immediate here is still an instruction of its own.
            auto count = base[sar->rhs];
            if(count->inst()->kind != LowerInst::Imm) continue;

            auto shift = base[sar->lhs];
            auto shl = shift->inst();
            if(shl->kind != LowerInst::Shl) continue;
            if(shift->uses.size() != 1) continue;

            auto up = (LowerInstBinary*)shl;
            if(up->result.type != type) continue;

            auto upCount = base[up->rhs];
            if(upCount->inst()->kind != LowerInst::Imm) continue;
            if(immValue(upCount) != immValue(count)) continue;

            auto source = base[up->lhs];
            if(source->inst()->kind == LowerInst::Imm) continue;

            // The distance is register-relative - see `signShift` in resolve/lower_type.cpp - so
            // what is left below it is the width being extended from.
            auto distance = immValue(count);
            if(distance == 0 || distance >= registerBytes * 8) continue;

            auto sourceBits = registerBytes * 8 - distance;
            if(sourceBits % 8 != 0) continue;

            auto sourceBytes = sourceBits / 8;
            if(sourceBytes != 1 && sourceBytes != 2) {
                // `movsxd` and nothing else, so only where it widens into a 64-bit register.
                if(sourceBytes != 4 || registerBytes != 8) continue;
            }

            auto sext = new (fun.arena) LowerInstX86Sext(
                sar->result.name, type, source - base, U8(sourceBytes)
            );

            insertInstAt(base, block, i, sext);
            replaceAllUses(base, &sar->result, &sext->result);
            removeInst(base, sar);

            // The shift, and the distance both shifts named. `removeDeadChain` takes each only once
            // its own use list is empty, so a distance that some third instruction also reads - or
            // the one `Imm` that served both shifts, which is the shape `truncateToWidth` writes -
            // is left exactly where it is.
            dead.push(shl);
            dead.push(count->inst());
            dead.push(upCount->inst());
        }

        removeDeadChain(base, dead);
    }
}

/*
 * §3.5.6 The BMI pair-replacements.
 *
 * Four shapes this machine has one instruction for above the baseline and two below it, and one
 * rewrite that exists only to reach a fifth:
 *
 *   ~a & b        andn a, b       BMI1
 *   x & (x - 1)   blsr x          BMI1   the lowest set bit cleared
 *   x & -x        blsi x          BMI1   the lowest set bit alone
 *   x ^ (x - 1)   blsmsk x        BMI1   the mask up to and including the lowest set bit
 *   rol x, k      ror x, w - k    BMI2   so that the immediate rotation reaches `rorx`
 *
 * ## Why these are peepholes and not IR
 *
 * Every one of the four is arithmetic the *library* already writes, in the form the library should
 * write it in. `alignUp` is `(v + a - 1) & ~(a - 1)`, `isPowerOf2` is `v != 0 && (v & (v - 1)) == 0`,
 * and a loop over the set bits of a word clears the lowest one by subtracting one and masking. None
 * of those wants a spelling of its own: each is correct, portable, and folded by LLVM's own
 * selection for a target that has the instruction. So the recognition belongs where the instruction
 * does, which is here.
 *
 * That is the argument `LowerInst::Bswap` declined, and the difference is worth stating because it
 * looks like the same one. A byte reversal written as a shift tree is a *tree* - four shifts, three
 * masks and three `or`s - and the optimizer above may reassociate it, so a peephole over it stops
 * finding what it looks for. These four are two instructions each. There is nothing to reassociate:
 * `x - 1` is a subtraction of a constant, which every fold in this compiler leaves where it is, and
 * `~a` is one instruction with one operand.
 *
 * ## What each one buys
 *
 * The same three things `selectSignExtends` buys, in the same order:
 *
 *  - **One instruction instead of two**, and one uop instead of two.
 *  - **No copy.** `not r/m`, `dec r/m` and `neg r/m` are two-address, so each pair needs the operand
 *    it reads twice copied into the register the result will occupy. The replacement reads its
 *    operand out of a field of its own and leaves it alone - which is the whole of why a loop over
 *    the set bits of a word stops needing a scratch register.
 *  - **The flags.** The pair writes them twice, over two windows; the replacement writes them once.
 *
 * ## Where the pass sits
 *
 * **Below `selectStoreUpdates`**, and that is the constraint. `mask &= ~bit` is a load, an `and` and
 * a store to one place, which that pass turns into `and [m], r` - two instructions including the
 * complement, against three for an `andn`, which has no memory-*destination* form to be folded into.
 * Running above it would take the `and` out from under it and lose the better of the two.
 *
 * **Above `selectMemorySources`**, which is the other half and is what the memory twins registered
 * beside these forms are for. A load feeding one of the four still folds into the instruction that
 * replaced the pair - and for the three lowest-bit operations it folds where it could not before:
 * the pair read its subject twice, so no load feeding it had a single reader, and the replacement
 * reads it once.
 *
 * Nothing has been folded into an address by the time this runs, so the `isMem` guard below is a
 * cheap statement of what these forms require rather than a case that arises. It is written anyway,
 * because what a pass requires of its operands should not have to be re-derived from where it sits.
 */

// The all-ones pattern at a scalar word's own width, which is what a complement written as a `xor`
// carries and what a decrement written as an addition of minus one carries. Local rather than
// lower_fold.cpp's `maskOf`, which is file-private there.
static U64 allOnesFor(LowerType type) {
    return type.byteWidth() >= 8 ? maxLimit<U64> : (U64(1) << (type.byteWidth() * 8)) - 1;
}

/*
 * The value an instruction complements, or nothing if it complements none.
 *
 * `not x` is what the language's `not` lowers to; `x ^ -1` is the same function and is what a fold
 * that met an all-ones mask can leave behind, so both are read.
 *
 * ## And `x ^ 1`, which is a complement only in a context
 *
 * A `Bool`'s complement is **not** a complement of its word. `Bitwise(Bool).not` is `emitLogicalNot`
 * and emits `xor c, 1`, because complementing the storage of a one-bit value gives something that is
 * not a `Bool` - see the ruling in resolve/core.cpp. So `!a && b` arrives here as
 * `and (xor a, 1), b`, and the `andn` this file exists to reach was never offered it: `x ^ 1` is not
 * `~x`, and the two differ in every bit above the lowest.
 *
 * They differ in bits the `and` may already be discarding, which is what makes the rewrite available
 * and is why `under` is a parameter. `andn a, u` is `~a & u`; the instruction being replaced computes
 * `(a ^ 1) & u`. Bit 0 agrees whatever `a` is - both spellings flip it - and every bit above it is
 * zero in `a ^ 1` and one in `~a`, so the two are equal exactly where `u` has those bits clear.
 * **That is `isBooleanValued(u)` and nothing about `a` at all**, which is stronger than it looks:
 * the value being complemented may be any word, and it is the operand it is read against that
 * licenses the rewrite.
 *
 * `under` is null for a caller that is not `and`ing the result with anything, which declines the
 * `x ^ 1` spelling rather than guessing at a context.
 */
static LowerValue* complementedOperand(LowerBase base, LowerValue* value, LowerValue* under = nullptr) {
    auto inst = value->inst();

    if(inst->kind == LowerInst::Not) return base[((LowerInstUnary*)inst)->from];

    if(inst->kind == LowerInst::Xor) {
        auto binary = (LowerInstBinary*)inst;
        auto rhs = base[binary->rhs];

        if(rhs->inst()->kind != LowerInst::Imm) return nullptr;

        // At the operation's own width: `x ^ 0xffffffff` complements an `i32` and does something
        // else to an `i64`.
        auto constant = immValue(rhs);
        if(constant == allOnesFor(binary->result.type)) return base[binary->lhs];
        if(constant == 1 && under && isBooleanValued(base, under)) return base[binary->lhs];

        return nullptr;
    }

    return nullptr;
}

// Whether `value` is `subject - 1`, written either way round: the lowering emits a subtraction of
// one, and a fold that normalized the sign can leave an addition of minus one.
static bool isDecrementOf(LowerBase base, LowerValue* value, LowerValue* subject) {
    auto inst = value->inst();
    if(inst->kind != LowerInst::Sub && inst->kind != LowerInst::Add) return false;

    auto binary = (LowerInstBinary*)inst;
    if(base[binary->lhs] != subject) return false;

    auto rhs = base[binary->rhs];
    if(rhs->inst()->kind != LowerInst::Imm) return false;

    auto allOnes = allOnesFor(binary->result.type);
    return immValue(rhs) == (inst->kind == LowerInst::Sub ? U64(1) : allOnes);
}

// Whether `value` is `-subject`, as the negation the lowering writes or as the subtraction from zero
// a source program spells it with.
static bool isNegationOf(LowerBase base, LowerValue* value, LowerValue* subject) {
    auto inst = value->inst();

    if(inst->kind == LowerInst::Neg) return base[((LowerInstUnary*)inst)->from] == subject;

    if(inst->kind == LowerInst::Sub) {
        auto binary = (LowerInstBinary*)inst;
        if(base[binary->rhs] != subject) return false;

        auto lhs = base[binary->lhs];
        return lhs->inst()->kind == LowerInst::Imm && immValue(lhs) == 0;
    }

    return false;
}

// A scalar integer in a general register, which is every width these forms have. A packed `and` is a
// different opcode and `isInt` answers false for one by construction.
static bool isScalarWord(LowerType type) {
    if(!isInt(type)) return false;

    auto bytes = type.byteWidth();
    return bytes == 4 || bytes == 8;
}

/*
 * One binary instruction replaced by the single instruction it equals.
 *
 * `spent` is the operand's own definition, which is now unread - the decrement, the negation or the
 * complement. It is pushed rather than removed here for `selectSignExtends`' reason: it stands
 * *above* the instruction being rewritten, and taking it out at this point would renumber the list
 * the caller is indexing. `removeDeadChain` takes it once the walk is over and only if nothing else
 * came to read it.
 */
static void replaceBinaryWith(LowerBase base, LowerBlock* block, Size at, LowerInstBinary* pair,
                              LowerInstSingle* replacement, LowerValue* spent, InstChain& dead)
{
    insertInstAt(base, block, at, replacement);
    replaceAllUses(base, &pair->result, &replacement->result);
    removeInst(base, pair);
    dead.push(spent->inst());
}

void selectBitOps(Context&, LowerBase base, LowerFunction& fun) {
    auto features = targetFeatures();
    auto bmi1 = (kFeatureBmi1 & ~features) == 0;

    // A left rotation is rewritten only in order to reach `rorx`, so BMI2 is the whole of the reason
    // to do it: without the alternative the two rotations are the same two bytes at the same form.
    auto bmi2 = (kFeatureBmi2 & ~features) == 0;
    if(!bmi1 && !bmi2) return;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];
        InstChain dead;

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];

            /*
             * The left rotation, which is not a pair-replacement at all: it is one instruction
             * rewritten into the one instruction it equals, so that the form selection has a `rorx`
             * to reach. `rol x, k` and `ror x, w - k` are the same function at every k, the count
             * being taken modulo the width - see LowerInst::Rol, where that is the ruling.
             *
             * A distance of zero is left alone. It is the identity, `w - 0` is `w` which is again
             * the identity, and rewriting one into the other would trade a rotation that folds away
             * for a `rorx` that is six bytes.
             */
            if(bmi2 && inst->kind == LowerInst::Rol) {
                auto rol = (LowerInstBinary*)inst;
                if(!isScalarWord(rol->result.type)) continue;

                auto count = base[rol->rhs];
                if(count->inst()->kind != LowerInst::Imm) continue;

                auto width = U64(rol->result.type.byteWidth()) * 8;
                auto distance = immValue(count) % width;
                if(distance == 0) continue;

                auto imm = new (fun.arena) LowerImm(StringId(), rol->result.type, width - distance);
                insertInstAt(base, block, i, imm);

                auto ror = new (fun.arena) LowerInstBinary(
                    rol->result.name, rol->result.type, rol->lhs, &imm->result - base, LowerInst::Ror
                );

                // Past the new immediate, which the insertion above put where the rotation was.
                replaceBinaryWith(base, block, i + 1, rol, ror, count, dead);
                i++;
                continue;
            }

            if(!bmi1) continue;
            if(inst->kind != LowerInst::And && inst->kind != LowerInst::Xor) continue;

            auto binary = (LowerInstBinary*)inst;
            if(!isScalarWord(binary->result.type)) continue;

            auto lhs = base[binary->lhs];
            auto rhs = base[binary->rhs];
            if(isMem(lhs) || isMem(rhs)) continue;

            /*
             * The three lowest-bit operations, each of which combines a value with something derived
             * from that same value. Both positions are tried: these operations are commutative and
             * `canonicalizeOperands` only moves *immediates* to the right, so neither side is
             * canonical.
             *
             * The derived operand must have exactly one reader, on `selectSignExtends`' terms - with
             * a second reader it has to stay, and the rewrite would then buy one instruction at the
             * cost of holding one more value live across it.
             */
            auto replaced = false;

            for(auto attempt = 0; attempt < 2 && !replaced; attempt++) {
                auto subject = attempt == 0 ? lhs : rhs;
                auto derived = attempt == 0 ? rhs : lhs;
                if(derived->uses.size() != 1) continue;

                auto which = LowerX86LowBit::Clear;

                if(binary->kind == LowerInst::Xor) {
                    if(!isDecrementOf(base, derived, subject)) continue;
                    which = LowerX86LowBit::Mask;
                } else if(isDecrementOf(base, derived, subject)) {
                    which = LowerX86LowBit::Clear;
                } else if(isNegationOf(base, derived, subject)) {
                    which = LowerX86LowBit::Isolate;
                } else {
                    continue;
                }

                replaceBinaryWith(base, block, i, binary, new (fun.arena) LowerInstX86LowBit(
                    binary->result.name, binary->result.type, subject - base, which
                ), derived, dead);

                replaced = true;
            }

            // `~a & b`, which only an `and` has and which the loop above cannot have matched: a
            // complement is not one of the three shapes it looks for.
            for(auto attempt = 0; attempt < 2 && !replaced && binary->kind == LowerInst::And; attempt++) {
                auto negated = attempt == 0 ? lhs : rhs;
                auto other = attempt == 0 ? rhs : lhs;
                if(negated->uses.size() != 1) continue;

                auto source = complementedOperand(base, negated, other);
                if(!source) continue;

                replaceBinaryWith(base, block, i, binary, new (fun.arena) LowerInstX86AndNot(
                    binary->result.name, binary->result.type, source - base, other - base
                ), negated, dead);

                replaced = true;
            }
        }

        removeDeadChain(base, dead);
    }
}
