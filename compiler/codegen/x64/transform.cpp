#include "gen.h"
#include "x64_util.h"
#include "../../lower/lower_fold.h"

// For `setOperand`, which is how a rewritten operand keeps the use lists agreeing with it.
#include "../../lower/lower_builder.h"

/*
 * A short list of instructions one rewrite is about: the chain a constant vector is defined by, the
 * instructions a fold left with no readers, the readers of a value being retargeted.
 *
 * One name rather than `Array<LowerInst*>` spelled at each of them, because every one of these has
 * the same lifetime - one instruction, or one block's walk - and the same shape: a splat and a
 * handful of lane writes, a comparison and the two constants under it. Inline for that reason and
 * for the one compiler/util/README.md gives: several of these are built *per instruction*, so an
 * ordinary array is one allocation per instruction of the function whether or not the fold applies.
 */
using InstChain = SmallArray<LowerInst*, 8>;

/*
 * The two readers of one, stated here because five passes above their definitions call them.
 *
 * Both live beside the constant pooling they were written for, far below: the bytes a
 * `vsplat`/`vwithlane` chain comes to, and the sweep that takes such a chain back out once nothing
 * reads it. They used to be declared twice, once above each group of callers, which is two places to
 * keep agreeing with one definition.
 */
static bool constantVectorBytes(LowerBase base, LowerValue* value, U8* bytes, Size size,
                                InstChain& chain);
static void removeDeadChain(LowerBase base, InstChain& chain);

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

// Whether a block operation with this byte count takes the unrolled encoding rather than the
// rep-prefixed string instruction. The unrolled form is only viable for a compile-time count small
// enough to be worth straight-lining; everything else takes `rep movsb`/`rep stosb`, which needs its
// operands in fixed registers - the count in rcx among them.
//
// Stated here rather than beside selectBlockOpEncoding because two things ask it and the order they
// are asked in must not matter: the peephole below, which reaches the count's `Imm` *before* the
// operation that reads it, and the encoding choice itself. Both derive the answer from the constant
// rather than from the flag, so neither can be told something the other will contradict.
//
// It says nothing about whether the count ends up in a register, which is a different question with
// a different answer - the constant may be shared with an instruction that needs one. That is what
// the pair of unrolled forms in machine.cpp is for.
static bool isUnrolledCount(LowerBase base, LowerPtr<LowerValue> count) {
    auto value = base[count];
    if(value->inst()->kind != LowerInst::Imm) return false;

    return ((LowerImm*)value->inst())->i <= kMaxUnrolledMemOp;
}

// Which operand of an instruction is a block operation's byte count, or -1 where it is not one. The
// two kinds declare their operands in different orders - `copy to, from, count` against
// `setpattern to, count, pattern` - and both encoders read the count out of the IR.
static Size blockOpCountOperand(LowerInst* inst) {
    if(inst->kind == LowerInst::Copy) return 2;
    if(inst->kind == LowerInst::SetPattern) return 1;
    return Size(-1);
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

    auto countOperand = blockOpCountOperand(inst);

    for(Size i = 0; i < used.size(); i++) {
        if(base[used[i]] != op) continue;

        /*
         * The byte count of a block operation that will be unrolled, which is the one operand no
         * form table entry can answer for. It is not carried in the encoding's bytes - there is no
         * immediate field, since the count is what decides how many `mov`s are written rather than
         * what any one of them says - so it is `folded()` rather than an `Immediate`, and
         * opcodeCanEmbedImmediate looks only at immediates.
         *
         * Asked of the count rather than of the opcode because the forms of one opcode disagree
         * about it and the disagreement is the point: `rep movsb` reads the same operand out of rcx.
         *
         * Answering "yes" here is a request rather than a decision - what settles it is whether
         * every *other* use of the constant agrees, since Implicit is set on the value. selectForm
         * reads the flag back and picks the unrolled form that matches, so a count this could not
         * take out of allocation is one that stays in a register at an encoding that ignores it.
         */
        if(Size(i) == countOperand) {
            if(!isUnrolledCount(base, used[i])) return false;
            found = true;
            continue;
        }

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
static bool onlyFeedsMachineSplats(LowerBase base, LowerImm* imm) {
    auto uses = imm->result.uses.contents(base);
    if(uses.size() == 0) return false;

    for(auto use: uses) {
        if(!splatIsMachineConstant(base, base[use])) return false;
    }

    return true;
}

// Tries to embed this immediate into any instructions that use it.
static bool tryEmbedImm(LowerBase base, LowerImm* imm) {
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
static bool tryElideDirectCallee(LowerBase base, LowerInstFun* fun) {
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
static bool tryFoldGlobalAddress(LowerBase base, LowerInstGlobal* global) {
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

        case LowerInst::Set:
        case LowerInst::Neg:   case LowerInst::Not:
        case LowerInst::Add:   case LowerInst::Sub:
        case LowerInst::Mul:   case LowerInst::IMul:
        case LowerInst::Div:   case LowerInst::IDiv:
        case LowerInst::Rem:   case LowerInst::IRem:
        case LowerInst::MulHi: case LowerInst::IMulHi:
        case LowerInst::Shl:   case LowerInst::Sar:
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
         * The intrinsics whose answer is a bit *count* rather than a value: `popcnt` and the two bit
         * scans all answer at most 64, so every bit above the low byte is clear whatever width the
         * instruction ran at. A cast of one of these is a name for the register rather than a `mov`.
         */
        case LowerInst::Intrinsic: {
            if(!isIntLike(type)) return false;

            auto which = ((LowerInstIntrinsic*)inst)->getIntrinsic();
            return which == LowerIntrinsic::Popcnt
                || which == LowerIntrinsic::Cttz
                || which == LowerIntrinsic::CttzWidth;
        }

        // Anything loaded at four bytes or fewer lands in a register the load itself filled: the
        // narrow forms extend into the result's own width, and a four-byte one is `mov r32` unless
        // it is the signed widening `movsxd`. A sign extension is the one that carries a bit up.
        case LowerInst::Load: {
            auto load = (LowerInstLoad*)inst;
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
 *  - **another `Cast`.** Not because the instruction reads too much - `movsxd` and the truncating
 *    `mov` both read 32 bits - but because `isZeroExtended` answers for a `Cast` by its *types*, on
 *    the grounds that a cast with a 32-bit end moves at 32 bits and therefore clears. Marking this
 *    one makes that untrue of it, and the second cast may already have been marked on the strength
 *    of it. Declining here keeps that answer independent of this marking, which is the property
 *    §9.4 established and the one thing here that would silently produce a wrong register.
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
        case LowerInst::Cmp: case LowerInst::Select: {
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
static bool trySkipCastExtend(LowerBase base, LowerInstCast* cast) {
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
static bool trySwapOperands(LowerBase base, LowerInst* inst) {
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
static bool orderFloatCompare(LowerBase base, LowerInst* inst) {
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
static bool orderPackedCompare(LowerBase base, LowerInst* inst) {
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

// Whether this instruction can be computed before the comparison rather than after it.
//
// Only a value computed in registers from registers qualifies: it reads and writes no memory, so
// moving it above the loads and stores that share the window changes nothing, and it cannot fault,
// so it cannot change what has run when something else does. That rules out a call, whose flag
// clobber is not movable at all, and the divisions, which fault and are far too expensive for their
// position to be what a window costs.
//
// A comparison is left out deliberately. Lifting one above another only exchanges which of the two
// windows the clobber sits in, and the one it moves into is the one already being fixed.
static bool canHoistOverCompare(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Imm:
        case LowerInst::Set:
        case LowerInst::Cast:
        case LowerInst::Bitcast:
        case LowerInst::Neg:
        case LowerInst::Not:
        case LowerInst::Add:
        case LowerInst::Sub:
        case LowerInst::Mul:
        case LowerInst::IMul:
        case LowerInst::MulHi:
        case LowerInst::IMulHi:
        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar:
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
            return true;
        default:
            return false;
    }
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
static void tryElideBranchTest(LowerBase base, LowerBlock* block) {
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
static Size tryMergeCompare(LowerBase base, LowerInstCmp* cmp, Size index) {
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

// Records, once, which of the two encodings a Copy/SetPattern will take, so that the register
// constraints (the selected form) and the encoder (genCopy/genSetPattern) read one field instead of
// each re-deriving the choice and risking disagreement. See isUnrolledCount above for the choice
// itself, which the immediate peephole has to agree with.
static void selectBlockOpEncoding(LowerBase base, LowerInst* inst) {
    if(inst->kind == LowerInst::Copy) {
        auto copy = (LowerInstCopy*)inst;
        copy->setUnrolled(isUnrolledCount(base, copy->count));
    } else if(inst->kind == LowerInst::SetPattern) {
        auto set = (LowerInstSetPattern*)inst;
        set->setUnrolled(isUnrolledCount(base, set->count));
    }
}

// Inserts an empty block on the edge from `pred` (its `outgoing[edge]`) to `succ`, so that the
// moves that feed `succ`'s phis have a block of their own to live in.
//
// Phi moves are emitted at the end of the predecessor, which is only sound if control reaching that
// point is guaranteed to continue into the phi's block. When the predecessor ends in a conditional
// branch, it is not: the moves would run on the way to *both* successors, writing phi registers on
// a path where they hold something else. Splitting gives the edge a block whose only successor is
// `succ`, which restores that guarantee.
static void splitEdge(LowerBase base, LowerFunction& fun, LowerBlock* pred, Size edge) {
    auto& arena = fun.arena;
    auto succ = base[pred->outgoing[edge]];
    auto predOffset = pred - base;

    auto split = new (arena) LowerBlock(pred->fun, StringId(), BlockIndex(fun.blocks.size()));
    fun.blocks.push(arena, split - base);

    // Wired up by hand rather than through addInst, which would append the split block to `succ`'s
    // incoming list instead of replacing the predecessor entry that the phis still refer to.
    auto jmp = (LowerInst*)new (arena) LowerInstJmp(succ - base);
    jmp->block = split - base;
    split->terminator = jmp - base;
    split->outgoing[0] = succ - base;
    split->incoming.push(arena, predOffset);

    auto je = (LowerInstJe*)base[pred->terminator];
    assertTrue(je->kind == LowerInst::Je);
    if(edge == 0) je->then = split - base;
    else je->otherwise = split - base;
    pred->outgoing[edge] = split - base;

    for(Size i = 0; i < succ->incoming.size(); i++) {
        if(succ->incoming.get(base, i) == predOffset) {
            succ->incoming.set(base, i, split - base);
            break;
        }
    }

    for(auto p: succ->phis.contents(base)) {
        auto sources = base[p]->sources();
        for(Size i = 0; i < sources.size(); i++) {
            if(sources.ptr[i] == predOffset) sources.ptr[i] = split - base;
        }
    }
}

/*
 * Which edges are split, and why it is every critical one rather than only the ones a phi transfer
 * needs.
 *
 * A phi transfer needs an insertion point on its edge, and so does a *location change* - a web that
 * is in a register inside a loop and in its home outside it has to be carried across every edge of
 * the boundary (§5.10 of place.cpp). The two are the same requirement, and a critical edge - a
 * branching predecessor into a joining successor - is exactly the shape that has no such point:
 * a copy at the end of the predecessor runs on the way to both successors, and one at the head of the
 * successor runs on the way in from all of them.
 *
 * Splitting only the phi edges left the second half unserved, and the measurement is what says how
 * much: **193 of `Matrix`'s 257 region candidates were refused for want of an insertion point**, which
 * is three quarters of everything that survived every other test. A loop's exit edge is critical
 * almost by construction - the block after a loop joins the path that ran it with the path that
 * skipped it.
 *
 * What it costs is a block per critical edge, and the answer to that is already here: §3.2.3 emits
 * nothing for a block whose whole content is a jump, so an edge nothing lands on costs no byte and no
 * label. What is left is that the *layout* sees the extra blocks, which is measured rather than
 * assumed - see §49 of test/bench/findings.md.
 */
static void splitPhiEdges(LowerBase base, LowerFunction& fun) {
    // Snapshotted because splitting appends to the block list, and a freshly created split block
    // has a single successor and so can never itself need splitting.
    SmallArray<LowerPtr<LowerBlock>, 64> original;
    for(auto b: fun.blocks.contents(base)) original.push(b);

    for(auto offset: original) {
        auto pred = base[offset];

        // Only a block with two successors can reach a successor on a path it might not take.
        if(!pred->outgoing[0] || !pred->outgoing[1]) continue;

        for(Size edge = 0; edge < 2; edge++) {
            auto succ = base[pred->outgoing[edge]];

            // A successor with one predecessor already has an insertion point of its own: the head of
            // the block, which only this edge reaches. Splitting there would add a jump for nothing.
            // Both arms reaching one block counts as two predecessors, which `incoming` records twice.
            if(succ->phis.isEmpty() && succ->incoming.size() < 2) continue;

            splitEdge(base, fun, pred, edge);
        }
    }
}

/*
 * Outgoing stack arguments.
 *
 * A call whose convention runs out of argument registers passes the rest in the argument area, and
 * each of those becomes an explicit store ahead of the call.
 *
 * The store exists to break the argument's lifetime. Left as an ordinary operand of the call, a
 * stack argument would have to sit in a register from wherever it was computed all the way to the
 * call, competing for registers with every other argument being computed in between - which is
 * precisely where a call with more arguments than registers is under the most pressure. Storing it
 * early ends its live range at the store, and memory holds it from there on.
 *
 * That is also why the store has to be an instruction rather than a move hung off the call: liveness
 * runs over instructions, so a store it cannot see shortens nothing.
 *
 * Which arguments these are is the convention's answer and never the author's, so the caller writes
 * into exactly the offsets the callee reads back from.
 */

// Inserts `inst` into `block`'s instruction list at `at`, shifting what follows up one. The list has
// no insert of its own, and the linear shift costs less than adding one would: this runs once per
// stack argument, over a list every pass already walks end to end.
static void insertInstAt(LowerBase base, LowerBlock* block, Size at, LowerInst* inst) {
    auto& arena = base[block->fun]->arena;

    inst->block = block - base;
    for(auto use: inst->used()) base[use]->uses.push(arena, inst - base);

    block->instructions.push(arena, inst - base);

    for(auto i = block->instructions.size() - 1; i > at; i--) {
        block->instructions.set(base, i, block->instructions.get(base, i - 1));
    }

    block->instructions.set(base, at, inst - base);
}

// Takes an instruction nothing reads any more out of its block, and with it the uses it contributed.
// Dropping those is what makes the next instruction of a folded address chain dead in turn, so the
// whole chain comes out by removing its instructions in order.
static void removeInst(LowerBase base, LowerInst* inst) {
    for(auto offset: inst->used()) {
        auto v = base[offset];
        auto uses = v->uses.contents(base);

        for(Size i = 0; i < uses.size(); i++) {
            if(base[uses[i]] == inst) { v->uses.remove(base, i); break; }
        }
    }

    auto block = base[inst->block];
    auto list = block->instructions.contents(base);

    for(Size i = 0; i < list.size(); i++) {
        if(base[list[i]] == inst) {
            block->instructions.remove(base, i);
            return;
        }
    }

    assertTrue("removing an instruction that is not in its own block" == nullptr);
}

// Moves `user`'s use of `from` over to `to`. Both use lists have to reflect it: they are how every
// later pass finds who consumes a value, and a stale entry would keep a dead value looking live.
static void replaceUse(LowerBase base, LowerValue* from, LowerInst* user, LowerValue* to) {
    auto uses = from->uses.contents(base);

    for(Size i = 0; i < uses.size(); i++) {
        if(base[uses[i]] == user) {
            from->uses.remove(base, i);
            break;
        }
    }

    to->uses.push(base[base[user->block]->fun]->arena, user - base);
}

// The same for every reader at once, which is what replacing a value with an equivalent one takes.
// The user list is snapshotted because moving a use rewrites the very list it is read from, and a
// user that reads the value twice appears twice and moves both of its entries across.
static void replaceAllUses(LowerBase base, LowerValue* from, LowerValue* to) {
    InstChain users;
    for(auto u: from->uses.contents(base)) users.push(base[u]);

    for(auto user: users) {
        replaceUse(base, from, user, to);

        auto used = user->used();
        for(Size i = 0; i < used.size(); i++) {
            if(base[used[i]] == from) used[i] = to - base;
        }
    }
}

// Where the store for an argument can go, as an index into its block's instruction list. As early as
// possible, since shortening the live range is the whole point: just after whichever comes last of
// the value's own definition and the preceding call, and never later than the call it feeds.
//
// The preceding call matters because the argument area is shared between the calls of a function -
// it is reserved once, sized for the largest - so a store hoisted above an earlier call would
// overwrite an argument that call has not read yet.
static Size stackArgPosition(LowerBase base, LowerBlock* block, LowerValue* value, Size callIndex) {
    Size position = 0;
    auto instructions = block->instructions.contents(base);

    for(Size i = 0; i < callIndex; i++) {
        auto inst = base[instructions[i]];

        if(inst->kind == LowerInst::Call) position = i + 1;

        for(auto& created: inst->created()) {
            if(&created == value) position = i + 1;
        }
    }

    return position;
}

static void insertStackArgs(LowerBase base, LowerFunction& fun, const Constraints& constraints) {
    auto& arena = fun.arena;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // Indexed rather than buffered, because inserting a store rewrites the list underneath.
        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Call) continue;

            auto callType = ((LowerInstCall*)inst)->getCallType();
            auto& convention = constraints.getConvention(callType);
            auto used = inst->used();

            // A syscall's first operand is its number, which the convention places like any other
            // argument; every other call names its target there, and that is not an argument.
            Size argStart = callType == LowerCallType::Syscall ? 0 : 1;

            ArgLocationList locations;
            classifyArgs(convention, used.size() - argStart, [&](Size a) {
                return base[used[a + argStart]]->type;
            }, locations);

            for(Size a = 0; a < locations.size(); a++) {
                if(locations[a].kind != ArgLocation::Stack) continue;

                auto operand = used[a + argStart];
                auto value = base[operand];

                auto push = new (arena) LowerInstX86PushArg(operand, locations[a].stackOffset, value->type);
                insertInstAt(base, block, stackArgPosition(base, block, value, i), push);

                // The call names the store's result from here on, so it still lists every argument
                // in order while the value itself is dead from the store onwards.
                replaceUse(base, value, inst, &push->result);
                used[a + argStart] = &push->result - base;

                i++; // the call has moved up one
            }
        }
    }
}

/*
 * Unsigned conversions.
 *
 * AMD64 converts between the two register banks in one instruction only where the integer side is
 * *signed*: `cvtsi2sd` reads an i64 and `cvttsd2si` writes one, and neither has an unsigned form
 * before AVX-512. So an unsigned conversion is a sequence rather than an instruction, which is
 * exactly why it is expanded here instead of being given a machine form - a form describes one
 * encoding with its operands, and there is no encoding to describe.
 *
 * Expanding into ordinary IR rather than into a pseudo is what keeps it cheap. Every instruction
 * below is one the backend already allocates, folds and encodes: each comparison is folded into the
 * select that reads it, each constant is embedded or materialized by the same peephole as any other,
 * and the register pressure is priced by the same costing. A pseudo would have needed scratch
 * registers nothing reserves, clobbers of its own, and an encoder that reproduced half of this file.
 *
 * The two 32-bit cases are exact rather than approximate, and for the same reason in both
 * directions: every u32 fits in an i64, so widening the *other* side and converting signed is
 * correct with nothing to correct afterwards.
 */

// The sequence replacing one conversion, built in front of it so that every value it produces is
// available wherever the conversion's own result was.
//
// Each step is a statement of its own rather than an argument to the next, because emitting appends
// to a list: nesting the calls would leave the order they run in up to the compiler's choice of
// argument evaluation order, and the wrong choice is a use before its definition.
struct Expansion {
    LowerBase base;
    LowerFunction& fun;
    LowerBlock* block;

    // Where the next instruction goes, which is the conversion's own position until the first one
    // has been inserted and pushed it down.
    Size at;

    LowerValue* emit(LowerInstSingle* inst) {
        insertInstAt(base, block, at++, inst);
        return &inst->result;
    }

    LowerValue* integer(LowerType type, U64 value) {
        return emit(new (fun.arena) LowerImm(StringId(), type, value));
    }

    LowerValue* floating(LowerType type, F64 value) {
        return emit(new (fun.arena) LowerImm(StringId(), type, value));
    }

    LowerValue* binary(LowerInst::Kind kind, LowerType type, LowerValue* lhs, LowerValue* rhs, StringId name = StringId()) {
        return emit(new (fun.arena) LowerInstBinary(name, type, lhs - base, rhs - base, kind));
    }

    LowerValue* convert(LowerType type, LowerValue* from, bool signedSource, bool signedResult, StringId name = StringId()) {
        return emit(new (fun.arena) LowerInstCast(name, type, from - base, signedSource, signedResult));
    }

    // The same bits read as another type of the same width - between two vectors it is the register
    // itself and emits nothing wherever the allocator lands both ends in one place.
    LowerValue* reinterpret(LowerType type, LowerValue* from, StringId name = StringId()) {
        return emit(new (fun.arena) LowerInstUnary(LowerInst::Bitcast, name, type, from - base));
    }

    LowerValue* withLane(LowerType type, LowerValue* vector, U8 lane, LowerValue* value, StringId name = StringId()) {
        return emit(new (fun.arena) LowerInstVecLane(name, type, vector - base, lane, value - base));
    }

    // One lane read back out, which answers in the lane's scalar form and in no other type - so the
    // type is derived here rather than passed, the way the text parser derives it.
    LowerValue* lane(LowerValue* vector, U8 index, StringId name = StringId()) {
        return emit(new (fun.arena) LowerInstVecLane(name, scalarFormOf(vector->type), vector - base, index));
    }

    LowerValue* splat(LowerType type, LowerValue* scalar, StringId name = StringId()) {
        return emit(new (fun.arena) LowerInstVecSplat(name, type, scalar - base));
    }

    /*
     * A one-in-one-out intrinsic, which is the only shape anything here needs.
     *
     * `LowerInstIntrinsic` is not a `LowerInstSingle` - its results live past the instruction rather
     * than in it, because an intrinsic may answer none or several - so this cannot go through
     * `emit`, and builds the allocation the way `handleIntrinsic` in lower_resolve.cpp does.
     */
    LowerValue* intrinsic(LowerIntrinsic which, LowerType type, LowerValue* operand, StringId name = StringId()) {
        auto inst = (LowerInstIntrinsic*)fun.arena.alloc(
            sizeof(LowerInstIntrinsic) + sizeof(LowerValue) + sizeof(LowerPtr<LowerValue>));

        new (inst) LowerInstIntrinsic(which, 1, 1);
        inst->used().ptr[0] = operand - base;
        new (inst->created().ptr) LowerValue(inst, type, name);

        insertInstAt(base, block, at++, (LowerInst*)inst);
        return inst->created().ptr;
    }

    // The same with two operands, which `bzhi` is the only one of. Written out rather than made
    // variadic because the allocation size is what differs and two is where it stops.
    LowerValue* intrinsic2(LowerIntrinsic which, LowerType type, LowerValue* first,
                           LowerValue* second, StringId name = StringId()) {
        auto inst = (LowerInstIntrinsic*)fun.arena.alloc(
            sizeof(LowerInstIntrinsic) + sizeof(LowerValue) + 2 * sizeof(LowerPtr<LowerValue>));

        new (inst) LowerInstIntrinsic(which, 1, 2);
        inst->used().ptr[0] = first - base;
        inst->used().ptr[1] = second - base;
        new (inst->created().ptr) LowerValue(inst, type, name);

        insertInstAt(base, block, at++, (LowerInst*)inst);
        return inst->created().ptr;
    }

    /*
     * Lanes rearranged, with the pattern written by a callback rather than handed over as a buffer.
     *
     * The pattern lives in the instruction's own allocation - past the used values, the way a phi's
     * source blocks do - so it cannot be filled in before the instruction exists, and a caller that
     * built one somewhere else would be copying it in anyway.
     */
    template<class F>
    LowerValue* shuffle(LowerType type, LowerValue* left, LowerValue* right, F&& entry, StringId name = StringId()) {
        auto inst = (LowerInstVecShuffle*)fun.arena.alloc(
            sizeof(LowerInstVecShuffle) + LowerInstVecShuffle::patternBytes(type));
        new (inst) LowerInstVecShuffle(name, type, left - base, right - base);

        auto pattern = inst->pattern();
        for(Size i = 0; i < pattern.length; i++) pattern[i] = entry(i);

        return emit((LowerInstSingle*)inst);
    }

    LowerValue* compare(LowerCmp cmp, LowerValue* lhs, LowerValue* rhs) {
        return emit(new (fun.arena) LowerInstCmp(StringId(), lhs - base, rhs - base, cmp));
    }

    // `select` yields its first value when the condition holds, which is the order the machine form
    // and the encoder both read it in.
    LowerValue* select(LowerType type, LowerValue* condition, LowerValue* whenTrue, LowerValue* whenFalse, StringId name = StringId()) {
        return emit(new (fun.arena) LowerInstSelect(name, whenTrue - base, whenFalse - base, condition - base, type));
    }
};

// An unsigned integer widened into a float.
static LowerValue* expandUnsignedToFloat(Expansion& e, LowerValue* value, LowerType to, StringId name) {
    if(value->type == LowerType::Int32) {
        auto wide = e.convert(LowerType::Int64, value, false, false);
        return e.convert(to, wide, true, false, name);
    }

    // Only the top bit makes the signed conversion wrong, so a value that has it set is halved until
    // it does not and the result doubled back. Halving with `(x >> 1) | (x & 1)` rather than with a
    // plain shift keeps the bit that would have been shifted out where the rounding can still see
    // it: dropping it would round a value that should have gone up to even instead.
    //
    // Both halves are computed and one is selected rather than branched over. Neither can trap - a
    // conversion of a negative i64 is simply a negative float, which this path then discards - so
    // the arm that is wrong on a given input costs an instruction rather than correctness.
    auto one = e.integer(LowerType::Int64, 1);
    auto half = e.binary(LowerInst::Shr, LowerType::Int64, value, one);
    auto odd = e.binary(LowerInst::And, LowerType::Int64, value, one);
    auto rounded = e.binary(LowerInst::Or, LowerType::Int64, half, odd);
    auto halved = e.convert(to, rounded, true, false);
    auto doubled = e.binary(LowerInst::Add, to, halved, halved);
    auto direct = e.convert(to, value, true, false);

    // Signed-less-than-zero is exactly "the top bit is set", which is the one case the direct
    // conversion gets wrong. The comparison is emitted immediately in front of the select so that
    // the folding leaves it in the flags rather than materializing it.
    auto zero = e.integer(LowerType::Int64, 0);
    auto negative = e.compare(LowerCmp::ilt, value, zero);
    return e.select(to, negative, doubled, direct, name);
}

/*
 * A float truncated into a *signed* integer, saturating - see `saturationRange` for the ruling.
 *
 * Two comparisons and two selects, and the reason it is only two of each is that `cvttsd2si` has
 * already answered one of the three cases. Its result for a NaN, for +infinity and for anything
 * outside the range is the integer indefinite value - which *is* the type's minimum, and therefore
 * *is* the saturated answer for everything that overflows downwards. So what is left to fix is the
 * top end and NaN, and each is one comparison the hardware reads directly:
 *
 *  - `x >= 2^(n-1)` is an ordered comparison, so a NaN answers false and cannot be caught here. The
 *    bound is the power of two rather than the type's maximum because the maximum is not a double at
 *    sixty-four bits, and a comparison against something near it is a comparison against the wrong
 *    number.
 *  - `cmp_uno` is the NaN test, and it exists because no pair of ordered comparisons can be one: a
 *    NaN and a value below the range both answer false to `x >= lo`, and the two want different
 *    results. On this machine it is the parity flag alone, which is why it is a comparison of its
 *    own rather than the `x != x` it replaced - that needed ZF as well and the two `setcc`s and a
 *    combine `genFloatFlagsToReg` emits for a float equality.
 */
static LowerValue* expandFloatToSigned(Expansion& e, LowerValue* value, LowerType to, StringId name) {
    auto bits = to == LowerType::Int32 ? 32 : 64;
    auto limit = bits == 32 ? 2147483648.0 : 9223372036854775808.0;
    auto highest = bits == 32 ? U64(0x7FFFFFFF) : U64(0x7FFFFFFFFFFFFFFF);

    auto direct = e.convert(to, value, false, true);

    auto zero = e.integer(to, 0);
    auto isNaN = e.compare(LowerCmp::uno, value, value);
    auto ordered = e.select(to, isNaN, zero, direct);

    auto bound = e.floating(value->type, limit);
    auto maximum = e.integer(to, highest);
    auto isBig = e.compare(LowerCmp::ge, value, bound);

    return e.select(to, isBig, maximum, ordered, name);
}

/*
 * And into an unsigned integer, which saturates on the same terms and gets no help from the
 * hardware at either end: `cvttsd2si` is a signed conversion, so its answer for a negative input is
 * a negative number rather than the zero this has to produce.
 *
 * Both ends are therefore explicit. `x >= 0` is ordered, so it is false for a NaN as well as for a
 * negative - and both of those want zero, which is why one comparison covers the two cases that
 * needed two for the signed form.
 */
static LowerValue* expandFloatToUnsigned(Expansion& e, LowerValue* value, LowerType to, StringId name) {
    auto zeroFloat = e.floating(value->type, 0.0);
    auto atLeastZero = e.compare(LowerCmp::ge, value, zeroFloat);

    if(to == LowerType::Int32) {
        // Every value of a `U32` converts through a signed 64-bit conversion exactly, so the
        // in-range arm is what it always was and only the two ends are new.
        auto wide = e.convert(LowerType::Int64, value, false, true);
        auto narrowed = e.convert(to, wide, false, false);

        auto zero = e.integer(to, 0);
        auto low = e.select(to, atLeastZero, narrowed, zero);

        auto bound = e.floating(value->type, 4294967296.0);
        auto maximum = e.integer(to, 0xFFFFFFFF);
        auto isBig = e.compare(LowerCmp::ge, value, bound);

        return e.select(to, isBig, maximum, low, name);
    }

    // Everything below 2^63 converts signed exactly. Everything above it is brought into range by
    // subtracting 2^63 - which is exact, both operands being of the same magnitude - and the bit
    // that removes is put back into the integer afterwards.
    //
    // As above, both arms are computed and one selected. The comparison has the select as its only
    // reader and sits directly in front of it, so it stays in the flags.
    auto limit = e.floating(value->type, 9223372036854775808.0);
    auto reduced = e.binary(LowerInst::Sub, value->type, value, limit);
    auto big = e.convert(LowerType::Int64, reduced, false, true);
    auto sign = e.integer(LowerType::Int64, 0x8000000000000000);
    auto flipped = e.binary(LowerInst::Xor, LowerType::Int64, big, sign);
    auto small = e.convert(LowerType::Int64, value, false, true);
    auto isBig = e.compare(LowerCmp::ge, value, limit);
    auto inRange = e.select(LowerType::Int64, isBig, flipped, small);

    auto zero = e.integer(LowerType::Int64, 0);
    auto low = e.select(LowerType::Int64, atLeastZero, inRange, zero);

    auto ceiling = e.floating(value->type, 18446744073709551616.0);
    auto maximum = e.integer(LowerType::Int64, 0xFFFFFFFFFFFFFFFF);
    auto isHuge = e.compare(LowerCmp::ge, value, ceiling);

    return e.select(LowerType::Int64, isHuge, maximum, low, name);
}

static void expandBankConversions(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // Indexed rather than buffered, and advanced by hand rather than by the loop, because both
        // things this does to an instruction move the ones after it: an expansion inserts in front
        // of the conversion and every removal closes the gap it leaves.
        for(Size i = 0; i < block->instructions.size(); ) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Cast) { i++; continue; }

            auto cast = (LowerInstCast*)inst;
            auto value = base[cast->from];
            auto to = cast->result.type;

            // A conversion is unsigned when the *integer* side is, which is whichever of the two
            // flags the cast carries depending on the direction it goes in. A cast that crosses no
            // bank boundary is not a conversion at all.
            auto toFloat = isFloat(to) && !isFloat(value->type);
            auto fromFloat = isFloat(value->type) && !isFloat(to);

            // **Every** float-to-integer conversion is expanded, not only the unsigned ones, because
            // saturating is what one means and `cvttsd2si` alone does not saturate - see
            // `saturationRange`. The other direction still only needs the unsigned case, which is
            // the one the machine has no instruction for.
            auto expandable = fromFloat || (toFloat && !cast->isSignedSource());

            if(!expandable) { i++; continue; }

            // A conversion nothing reads is removed rather than expanded. It is dead either way -
            // every instruction the expansion produces is side-effect free - but expanding it first
            // would make ten dead instructions out of one, and no later pass would take them out.
            // Whatever followed has moved into this position, so the walk stays where it is.
            if(cast->result.uses.isEmpty()) {
                removeInst(base, cast);
                continue;
            }

            Expansion e { base, fun, block, i };
            auto replacement = toFloat
                ? expandUnsignedToFloat(e, value, to, cast->result.name)
                : cast->isSignedResult() ? expandFloatToSigned(e, value, to, cast->result.name)
                                         : expandFloatToUnsigned(e, value, to, cast->result.name);

            replaceAllUses(base, &cast->result, replacement);
            removeInst(base, cast);

            // Past the whole expansion. The insertions left it occupying the positions the
            // conversion's own used to begin at, and removing the conversion from the end of it
            // closed the gap - so `at` is where the walk carries on. Nothing in what was produced is
            // an unsigned conversion, so there is nothing there to come back for.
            i = e.at;
        }
    }
}

/*
 * A packed shift whose count is a splat, written as the scalar count the machine's form takes.
 *
 * `class (Num(a)) Integral(a)` declares `shl(lhs: a, rhs: a)`, so over a vector *both* operands are
 * vectors and `v `shr` 7` reaches this backend as a shift by `vsplat(7)`. The form table has only
 * the immediate rows - `pslld xmm, imm8` and its siblings - and the selection asks whether the right
 * operand is a scalar `Imm`, so every shift a program could actually write was refused for want of
 * an instruction that was standing right there.
 *
 * **The splat is the whole of the difference, and unwrapping it is the whole of the fix.** Every
 * lane of the count holds the same scalar by construction, which is exactly what one shared count
 * means, so the rewrite is exact rather than a narrowing of what was asked for.
 *
 * **Every splat is unwrapped, not only a constant one**, and the argument is the same for both: a
 * splat's lanes all hold the scalar, and both machine forms want that scalar. ~~A splat of a runtime
 * value would want the machine's other form, which this backend does not have~~ - it has it now
 * (`FormVShlReg` and its two siblings), and what that form takes is a scalar in a general register,
 * so this pass is what puts it in reach.
 *
 * Handing either form the splat *unchanged* would be wrong rather than slow, which is worth keeping
 * written down: `pslld` reads the whole low **quadword** as one count, so a 32-bit lane splat of 7
 * arrives as 0x0000000700000007 and shifts every lane out. The unwrapping is what makes the count a
 * number rather than a bit pattern; the `movd` in the register form's expansion is what keeps it
 * one.
 *
 * **Above `poolVectorConstants`**, which is the whole of where this may sit: a constant splat is a
 * `.rodata` load by the time that pass has run, and a load is not a count this can read. It is the
 * same ordering constraint every pass reading a constant chain has, and for the same reason.
 */
static void unwrapVectorShiftCounts(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];

            if(inst->kind != LowerInst::Shl && inst->kind != LowerInst::Shr && inst->kind != LowerInst::Sar) {
                continue;
            }

            auto shift = (LowerInstBinary*)inst;
            if(!isVectorLike(shift->result.type)) continue;

            // A count that is already a scalar is the spelling the machine wants and the one the
            // `.lower` fixtures are written in - there is nothing here for it.
            auto count = base[shift->rhs];
            if(count->inst()->kind != LowerInst::VecSplat) continue;

            auto source = base[((LowerInstVecSplat*)count->inst())->from];
            setOperand(base, fun.arena, inst, shift->rhs, source);

            /*
             * **The orphaned splat has to go, and not only because it is dead code.** While it
             * stands it is a second reader of the constant, and `canEmbedImm` will not embed a
             * constant something else needs in a register - so the count would be materialized with
             * a `mov` and the shift would encode a zero where its immediate should be. Measured:
             * `mov $0x7,%eax ; movd ; pshufd ; psrld $0x0`.
             *
             * The splat sits above the shift, so removing it moves the walk back one - and its own
             * source may be dead in turn, which is what the second removal is for. Nothing deeper
             * than that: a constant is the end of the chain, and a runtime count is a value some
             * other instruction computed and this pass has no business following.
             */
            // The cursor moves back only for a removal from *this* block: either of the two may have
            // been built in another one - a constant hoisted to the entry block is the ordinary case
            // - and there the walk is unaffected.
            auto removeAndTrack = [&](LowerInst* dead) {
                if(base[dead->block] == block) i--;
                removeInst(base, dead);
            };

            if(!count->uses.isEmpty()) continue;

            removeAndTrack(count->inst());

            // The scalar goes only if it is a constant with nothing left reading it. A runtime count
            // is somebody else's instruction and removing it here would be a dead-code pass, which
            // this is not - and it still has a reader anyway, the shift this just gave it to.
            if(source->uses.isEmpty() && source->inst()->kind == LowerInst::Imm) {
                removeAndTrack(source->inst());
            }
        }
    }
}

/*
 * The fused multiply-add, where the target has no instruction that fuses.
 *
 * `a * b + c` at two roundings rather than one, which is not an approximation of what was asked for
 * but the other thing the language permits: Design-Vector §3.3 makes `fma` a *permission* to fuse
 * rather than a promise, precisely so that a target without FMA3 can spend it as the two operations
 * it always meant. A program that must not fuse writes `a * b + c` itself and gets two roundings
 * everywhere; a program that writes `fma` is saying it does not care.
 *
 * Expanded into IR rather than into a pseudo, on `expandUnsignedConversions`' argument: the multiply
 * and the add are two instructions this backend already allocates, folds and costs.
 */
static void expandFusedMultiplyAdd(Context&, LowerBase base, LowerFunction& fun) {
    if(targetFeatures() & kFeatureFma3) return;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); ) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Fma) { i++; continue; }

            auto fma = (LowerInstFma*)inst;
            auto type = fma->result.type;

            Expansion e { base, fun, block, i };
            auto product = e.binary(LowerInst::Mul, type, base[fma->a], base[fma->b]);
            auto sum = e.binary(LowerInst::Add, type, product, base[fma->c], fma->result.name);

            replaceAllUses(base, &fma->result, sum);
            removeInst(base, fma);

            i = e.at;
        }
    }
}

/*
 * Unsigned packed comparisons, which no x86 has and every x86 can do.
 *
 * `pcmpgt` reads its lanes as signed and there is no unsigned twin at any feature level. What there
 * is instead is the identity that makes one: `a <u b` exactly when `(a ^ 0x80000000) <s (b ^
 * 0x80000000)`, because flipping the top bit maps the unsigned order onto the signed one. So an
 * unsigned relation is the signed one over both operands biased, which is two exclusive-ors and a
 * broadcast the folder hoists out of any loop it is invariant in.
 *
 * ~~A 32-bit lane only.~~ Every lane width the signed relations have: the bias is a constant splat,
 * which is pooled before it is anything, and a narrow one has a broadcast of its own now. A 64-bit
 * lane still has no `pcmpgtq` to bias *into* before SSE4.2, and `unsupportedVectorReason` states
 * that bound from the other side.
 *
 * This is what `firstSet` reaches, and reaching it is not obvious: the lane indices it compares are
 * small non-negative numbers whose signed and unsigned orders agree, and the *type* is what decides
 * which comparison the IR asks for. So the sequence below is exact where it is also unnecessary,
 * which is the ordinary case for it.
 *
 * ## The two non-strict relations take a shorter route
 *
 * `a <=u b` is `minu(a, b) == a` and `a >=u b` is `maxu(a, b) == a` - two instructions, no constant
 * and no mask inverted. What they replace is the worst case of the bias: `ile` is one of the three
 * relations the machine has only the *complement* of (`packedCompareIsInverted`), so an unsigned
 * `le` was two exclusive-ors and then `pcmpgt ; pcmpeqd ; pxor` through a scratch register - five
 * instructions where this is two.
 *
 * The two strict ones keep the bias, and that is a measurement rather than an omission: `a <u b`
 * through a minimum is `maxu(a, b) == a` complemented, which is the same inversion pseudo again and
 * comes to four, where the bias is two exclusive-ors and a `pcmpgt` whose constant is hoisted out of
 * any loop it stands in.
 */
static void biasUnsignedPackedCompares(Context&, LowerBase base, LowerFunction& fun) {
    auto signedForm = [](LowerCmp cmp) {
        switch(cmp) {
            case LowerCmp::lt: return LowerCmp::ilt;
            case LowerCmp::le: return LowerCmp::ile;
            case LowerCmp::gt: return LowerCmp::igt;
            case LowerCmp::ge: return LowerCmp::ige;
            default:           return cmp;
        }
    };

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); ) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Cmp) { i++; continue; }

            auto cmp = (LowerInstCmp*)inst;
            auto type = base[cmp->lhs]->type;
            auto relation = signedForm(cmp->getCmp());

            if(relation == cmp->getCmp() || !isIntVector(type)) { i++; continue; }

            /*
             * The two non-strict relations, as a minimum or a maximum against one of the operands -
             * see the note above on why the strict pair is not done this way.
             *
             * The comparison is rewritten in place rather than replaced: it keeps its result, its
             * readers and its position, and what changes is which values it reads and which relation
             * it states. `a` stands in both the minimum and the equality, which is what makes this
             * two instructions rather than two and a copy.
             */
            auto written = cmp->getCmp();

            if((written == LowerCmp::le || written == LowerCmp::ge) && packedMinMaxSupported(type)) {
                Expansion pick { base, fun, block, i };
                auto lhs = base[cmp->lhs];
                auto rhs = base[cmp->rhs];

                auto minMax = pick.emit(new (fun.arena) LowerInstX86MinMax(
                    StringId(), type, lhs - base, rhs - base,
                    written == LowerCmp::le ? LowerMinMax::Min : LowerMinMax::Max
                ));

                setOperand(base, fun.arena, inst, cmp->lhs, minMax);
                setOperand(base, fun.arena, inst, cmp->rhs, lhs);
                cmp->setCmp(LowerCmp::eq);

                i = pick.at + 1;
                continue;
            }

            Expansion e { base, fun, block, i };

            // The top bit of every lane. Built once here and left to CSE and LICM, which is the
            // whole argument for expanding into IR: two comparisons in one block share this, and one
            // inside a loop leaves it outside.
            auto bit = e.integer(scalarFormOf(type), U64(1) << (laneBytes(type.lane) * 8 - 1));
            auto bias = e.splat(type, bit);
            auto lhs = e.binary(LowerInst::Xor, type, base[cmp->lhs], bias);
            auto rhs = e.binary(LowerInst::Xor, type, base[cmp->rhs], bias);

            setOperand(base, fun.arena, inst, cmp->lhs, lhs);
            setOperand(base, fun.arena, inst, cmp->rhs, rhs);
            cmp->setCmp(relation);

            // Past the four instructions the expansion added and past the comparison itself, which
            // is now a signed one and has nothing here to come back for.
            i = e.at + 1;
        }
    }
}

/*
 * Every lane of a vector combined into one scalar.
 *
 * A tree of shuffles and pairwise operations, log2(lanes) deep, expanded into IR - which is what
 * §5.3 of Implementation-Vector.md asks for and for the reason it gives: every instruction this
 * produces is then allocated, folded and costed by machinery that already exists, where a pseudo
 * would have needed a scratch vector register for each level of the tree.
 *
 * **The tree is a butterfly and not a compaction**, which is the one thing here that is a decision
 * rather than a transcription. The order a floating-point reduction combines in is a stated language
 * property (Design-Vector §4.5) - `(a0+a1) + (a2+a3)` for four lanes - so the shape has to be the
 * adjacent-pair tree the other two backends build and not the "fold the upper half down" idiom,
 * which for four lanes gives `(a0+a2) + (a1+a3)` and a different answer. Written as a compaction
 * that would have been two shuffles per level (the even lanes, then the odd ones); written as a
 * butterfly - lane `j` paired with lane `j ^ s`, doubling `s` - it is *one*, because every lane
 * holds the combination of its own group at every level and lane zero holds the one that is wanted.
 *
 * What comes out of the bottom is a lane extract, which is the same instruction the tree is built on
 * top of and needs nothing of its own.
 */

// One level of the tree: the two vectors combined the way this reduction says. `min` and `max` are a
// comparison and a select rather than an instruction, which is the same shape `emitMinMax` gives
// them in the library - the machine has `minps` and this backend has no form for it yet.
static LowerValue* reduceStep(Expansion& e, LowerReduce reduce, LowerType type,
                              LowerValue* lhs, LowerValue* rhs)
{
    auto compareAndSelect = [&](LowerCmp cmp) {
        // The comparison answers a mask of the operands' shape, which is the one thing about a
        // vector comparison that is not the scalar instruction unchanged.
        auto mask = e.emit(new (e.fun.arena) LowerInstCmp(
            StringId(), lhs - e.base, rhs - e.base, cmp, maskType(type.lane, type.lanes())));

        return e.select(type, mask, lhs, rhs);
    };

    switch(reduce) {
        case LowerReduce::Add: return e.binary(LowerInst::Add, type, lhs, rhs);
        case LowerReduce::Mul: return e.binary(LowerInst::Mul, type, lhs, rhs);
        case LowerReduce::And: return e.binary(LowerInst::And, type, lhs, rhs);
        case LowerReduce::Or:  return e.binary(LowerInst::Or, type, lhs, rhs);

        // `min` keeps the left operand where it compares less, and `max` where it compares greater -
        // so a NaN in either position follows the comparison, which answers false and yields the
        // right-hand side. That is what `minps` does with its operands in this order, and what the
        // library's `min` already promises.
        case LowerReduce::Min:  return compareAndSelect(LowerCmp::lt);
        case LowerReduce::IMin: return compareAndSelect(LowerCmp::ilt);
        case LowerReduce::Max:  return compareAndSelect(LowerCmp::gt);
        default:                return compareAndSelect(LowerCmp::igt);
    }
}

/*
 * A reduction of a **mask**, which is four questions rather than one arithmetic operation.
 *
 * `and` and `or` combine the lanes as they stand and answer a truth value: a mask lane is all-ones
 * or all-zeros here, so lane zero of the combined mask is `-1` or `0`, and `& 1` is the whole of
 * turning that into the `Bool` the result type asks for.
 *
 * `add` is the odd one and is `count`: how many lanes are set. Summing the mask itself would answer
 * the negative of it - every set lane is `-1` - and, worse, would be an `add` over two masks, which
 * is a thing the lower IR does not admit at all (a mask holds truth values). So the mask is turned
 * into a vector of ones and zeros with a select first, and what is summed is that.
 *
 * `first` is `firstSet` and has no tree at all: it is a lane *index*, and the only reason it is a
 * reduction kind rather than the `select(mask, iota, splat(lanes))` chain it used to be written as
 * is that every machine answers it in one step and no two answer it alike.
 *
 * `bits` is the movemask the three below reach it through, and the three things a caller may hand in
 * are about it: one already placed for a mask that several of them read, whether what comes back is
 * already a 0/1 rather than the -1/0 a mask lane holds, and whether those bits are the complement of
 * the mask this reduction was written about (§45.3).
 */
static LowerValue* expandMaskBitsReduce(Expansion& e, LowerReduce reduce, LowerValue* source,
                                        LowerValue* bits, bool& truth, bool complemented);

/*
 * A reduction of an 8- or 16-bit lane, which is the butterfly above finished in a general register.
 *
 * The tree needs a shuffle that pairs lane `i` with lane `i ^ stride`, and this machine's only
 * integer shuffle moves 32-bit lanes - `pshufd`. So the levels split in two by where the partner is:
 *
 * - **A partner a whole 32-bit lane away or further** is that same `pshufd`, applied to the register
 *   read as `i32` and read back as itself. Both bitcasts are the register and emit nothing; the
 *   *combining* step stays at the narrow lane, which is where the lane width has to be honoured.
 * - **A partner inside one 32-bit lane** has no shuffle at all, so the last one or two levels are
 *   done after the value has crossed to a general register - which it has to do anyway, since what
 *   a reduction answers is a scalar.
 *
 * The order is free here in a way it is not for a float: Design-Vector §4.5 fixes the *pairing* order
 * because floating-point addition is not associative, and there is no floating-point lane narrower
 * than four bytes. Every operation this reaches is associative and commutative over the integers, so
 * doing the wide levels first and the narrow ones last answers the same number.
 *
 * The scalar finish is two shapes rather than one. `add`, `and`, `or` and `mul` combine the whole
 * word against itself shifted down - the low lane of the result is exact whatever the lanes above it
 * hold, because a carry only ever travels upward - and the answer is the low lane truncated. `min`
 * and `max` cannot borrow that: they need each sub-lane as a value of its own, so the word is cut
 * into its two or four pieces and the pieces are compared at the lane's own width and signedness.
 */
static LowerValue* reduceScalarStep(Expansion& e, LowerReduce reduce, LowerType type,
                                    LowerValue* lhs, LowerValue* rhs) {
    auto compareAndSelect = [&](LowerCmp cmp) {
        auto flag = e.compare(cmp, lhs, rhs);
        return e.select(type, flag, lhs, rhs);
    };

    switch(reduce) {
        case LowerReduce::Add: return e.binary(LowerInst::Add, type, lhs, rhs);
        case LowerReduce::Mul: return e.binary(LowerInst::Mul, type, lhs, rhs);
        case LowerReduce::And: return e.binary(LowerInst::And, type, lhs, rhs);
        case LowerReduce::Or:  return e.binary(LowerInst::Or, type, lhs, rhs);
        case LowerReduce::Min:  return compareAndSelect(LowerCmp::lt);
        case LowerReduce::IMin: return compareAndSelect(LowerCmp::ilt);
        case LowerReduce::Max:  return compareAndSelect(LowerCmp::gt);
        default:                return compareAndSelect(LowerCmp::igt);
    }
}

static LowerValue* expandNarrowReduce(Expansion& e, LowerReduce reduce, LowerValue* value) {
    auto type = value->type;
    auto width = laneBytes(type.lane);
    auto bits = width * 8;
    auto perWord = 4 / width;
    auto lanes = type.lanes();

    // The same register read as 32-bit lanes, which is what the shuffle below is expressed in. Both
    // bitcasts are the register itself and emit nothing.
    auto wide = LowerType(LowerLane::Int32, U8(type.laneShift - (perWord == 4 ? 2 : 1)), false);

    for(U32 stride = perWord; stride < lanes; stride *= 2) {
        auto step = stride / perWord;
        auto asWide = e.reinterpret(wide, value);
        auto partnerWide = e.shuffle(wide, asWide, asWide, [&](Size j) { return U8(j ^ step); });
        auto partner = e.reinterpret(type, partnerWide);

        value = reduceStep(e, reduce, type, value, partner);
    }

    // Lane zero of the 32-bit view, which now holds every sub-lane's own answer - `movd`, and the
    // one lane extract this backend has at every feature level.
    auto word = e.lane(e.reinterpret(wide, value), 0);
    auto scalar = LowerType::Int32;

    /*
     * What a narrow lane is *in* a general register: the low `bits` and nothing said about the rest.
     * That is this backend's existing convention for a lane extract - `movd` of a byte vector hands
     * over four bytes and the reader uses one - so the two shapes below differ in exactly whether
     * they can live with it.
     */
    if(reduce == LowerReduce::Add || reduce == LowerReduce::Mul ||
       reduce == LowerReduce::And || reduce == LowerReduce::Or) {
        // A carry only ever travels upward, so the low lane of the whole word combined against
        // itself shifted down is the combination of every lane - exactly, and whatever the lanes
        // above it end up holding.
        for(U32 shift = 16; shift >= bits; shift /= 2) {
            auto moved = e.binary(LowerInst::Shr, scalar, word, e.integer(scalar, shift));
            word = reduceScalarStep(e, reduce, scalar, word, moved);
        }

        /*
         * And the lanes above the answer, cleared - which is not the redundant step it looks like.
         *
         * A reduction is the one operation here whose *result* is read as a whole register by
         * something that never knew there were lanes: `count(mask)` answers an `Int`, and an `Int`
         * is thirty-two bits of value rather than eight bits of value and a convention. So the
         * partial sums the shifts leave above the low lane have to go, and `count(m) == 5` compared
         * them until they did.
         */
        return e.binary(LowerInst::And, scalar, word, e.integer(scalar, (U64(1) << bits) - 1));
    }

    /*
     * `min` and `max` cannot: a comparison reads the whole register, so each sub-lane has to be a
     * value of its own first. An unsigned lane is masked and a signed one is shifted up and back
     * down, which is the sign extension the lane's own width asks for - and the comparison is then
     * the ordinary 32-bit one, at the signedness the reduction named.
     */
    auto signedLane = reduce == LowerReduce::IMin || reduce == LowerReduce::IMax;

    auto piece = [&](U32 index) {
        if(signedLane) {
            auto up = e.binary(LowerInst::Shl, scalar, word, e.integer(scalar, 32 - bits - index * bits));
            return e.binary(LowerInst::Sar, scalar, up, e.integer(scalar, 32 - bits));
        }

        auto down = index == 0 ? word
                               : e.binary(LowerInst::Shr, scalar, word, e.integer(scalar, index * bits));

        return e.binary(LowerInst::And, scalar, down, e.integer(scalar, (U64(1) << bits) - 1));
    };

    auto best = piece(0);
    for(U32 i = 1; i < perWord; i++) best = reduceScalarStep(e, reduce, scalar, best, piece(i));

    return best;
}

/*
 * The tree over one vector, answering the scalar it reduces to.
 *
 * A scalar rather than the vector whose lane zero holds it, because the narrow route below has no
 * such vector: its last levels happen after the value has crossed to a general register. The wide
 * route ends in the lane extract it always did, which is one instruction either way.
 */
static LowerValue* reduceTree(Expansion& e, LowerReduce reduce, LowerValue* value, LowerType type) {
    if(laneBytes(type.lane) < 4) return expandNarrowReduce(e, reduce, value);

    auto lanes = type.lanes();

    // The butterfly. At stride `s` every lane is paired with the lane `s` above or below it, which
    // after `log2(lanes)` doublings leaves lane zero holding the whole tree - and holding it in the
    // adjacent-pair order, since a lane's partner at each level is the other half of its own group.
    for(U32 stride = 1; stride < lanes; stride *= 2) {
        auto partner = e.shuffle(type, value, value, [&](Size i) { return U8(i ^ stride); });
        value = reduceStep(e, reduce, type, value, partner);
    }

    return e.lane(value, 0);
}

static LowerValue* expandReduce(Expansion& e, LowerReduce reduce, LowerValue* source,
                                LowerValue* bits, bool& truth, bool complemented = false) {
    auto type = source->type;

    if(type.isMask()) return expandMaskBitsReduce(e, reduce, source, bits, truth, complemented);
    return reduceTree(e, reduce, source, type);
}

/*
 * A mask read through `pmovmskb` - §34.2 of test/bench/findings.md.
 *
 * One instruction turns the whole mask into an integer with a bit per *byte*, and all four mask
 * consumers are then ordinary scalar arithmetic on it. What it replaces is a reduction tree: `any`
 * was three `pshufd`/`por` levels, a lane extract and six general-register instructions, `count`
 * was that plus a select against two splats it had to build first, and `firstSet` was a blend per
 * level over a vector of lane indices - about forty instructions where a bit scan is one.
 *
 * **A bit per byte, not per lane.** A mask lane is all-ones or all-zeros by construction, so a
 * four-byte lane contributes four identical bits: `any` and `all` do not care, `count` divides and
 * `firstSet` shifts. The full pattern is `1 << bytes` minus one - sixteen bits at 128 and thirty-two
 * at 256 - and it is computed from the type rather than written down, because the two tiers share
 * this code.
 *
 * `count` needs a population count, which is an instruction only where the target claims it. Without
 * it the tree below is still the shorter of the two - a SWAR population count is a dozen general
 * -register instructions - so the fallback is the code this replaced rather than a second expansion.
 */

/*
 * How many bits of the movemask one lane contributes.
 *
 * One, wherever the machine has an instruction that says so. `movmskps` and `movmskpd` read the sign
 * bit of each 32- or 64-bit element - which for a mask is the lane, a mask lane being all-ones or
 * all-zeros - and hand back exactly the bitmap every consumer below wants. `pmovmskb` reads a bit
 * per *byte*, so a 16-bit lane contributes two and every consumer of one pays a shift to divide them
 * back out; there is no `movmskw` and that is why the shift survives at one lane width.
 *
 * An 8-bit lane takes `pmovmskb` as well and has no shift either way, a bit per byte already being a
 * bit per lane there.
 *
 * **This and the form selection in machine.cpp are one decision made twice**, which is the hazard
 * worth naming: choosing `movmskps` there and leaving the shift here would divide a bitmap that was
 * never multiplied. See selectPackedForm's `VecReduce` arm.
 */
static U64 maskBitsPerLane(LowerType type) {
    auto width = laneBytes(type.lane);
    return width == 4 || width == 8 ? 1 : width;
}

// The same as a shift: a bit index is a lane index shifted left by this, which is what `count`
// divides out and `firstSet` undoes. Zero at three of the four lane widths.
static U64 maskBitShift(LowerType type) {
    U64 shift = 0;
    for(auto bits = maskBitsPerLane(type); bits > 1; bits /= 2) shift++;

    return shift;
}

// And how many bits of the movemask are the mask's at all - which is what "nothing is set above the
// lanes" means, and what the sentinel `firstSet` uses sits at.
static Size maskBitCount(LowerType type) {
    return Size(type.lanes() * maskBitsPerLane(type));
}

// The movemask itself. One instruction, and the one instruction every consumer below starts from -
// which is why `lowerVectorReductions` may place it once for a mask several of them read.
static LowerValue* emitMaskBits(Expansion& e, LowerValue* source) {
    return e.emit(new (e.fun.arena) LowerInstVecReduce(StringId(), LowerType::Int32, source - e.base,
                                                       LowerReduce::Bits));
}

/*
 * `firstSet` off the movemask: the lowest set bit of it, shifted back into a lane index.
 *
 * **Two sequences, and which one is chosen is decided by whether the movemask fills its word.**
 * Both answer the same three things - the lowest set lane, the lane count where none is set, and
 * nothing undefined in between - and they answer the third differently because the machine does.
 *
 * *Where the bits leave room above them*, which is a 128-bit register's sixteen, the sentinel does
 * two jobs and is one instruction: setting the bit one past the last lane byte makes "nothing is
 * set" answer the lane count - the scan finds the sentinel and the shift turns it into `lanes` - and
 * it is also what keeps the operand non-zero, which `Cttz` needs because `bsf` leaves its
 * destination undefined at zero.
 *
 * *Where they fill it*, which is a `ymm`'s thirty-two bytes, there is no bit to set: bit 32 is not a
 * bit an `i32` has. `tzcnt` answers the operand's width for a zero operand and the width is 32,
 * which is the byte count, which the shift turns into the lane count - so the sentinel is not
 * replaced by something wider, it is *not needed*, and the sequence is one instruction shorter than
 * the one that has it rather than longer.
 *
 * **The second is the one that generalizes and the first is the one that is portable**, which is why
 * both are here. A 512-bit mask is a `k` register with a bit per lane rather than per byte, so
 * sixty-four byte lanes fill sixty-four bits with nothing above them, and the sentinel has no width
 * to live at however wide the arithmetic is made; `tzcnt` at 64 answers 64, which is the lane count
 * again. The sentinel survives because it needs no feature, and BMI1 is claimed only from AVX2 -
 * which is exactly the level at which a movemask first fills its word.
 */
static LowerValue* expandMaskFirstSet(Expansion& e, LowerValue* bits, LowerType type) {
    auto scalar = LowerType::Int32;
    auto width = maskBitCount(type);
    auto shift = maskBitShift(type);

    /*
     * A movemask that fills its word comes from a register wider than 128 bits, and a vector that
     * wide needs AVX2 - `unsupportedVectorReason` refuses one without it - so the feature the scan
     * needs is implied by the shape that needs the scan. Asserted rather than branched on, because a
     * fallback here would be a path no target description can reach: see x64FeaturesFor, which is
     * where the two are tied together.
     *
     * One bit per lane is what narrowed this case rather than widening it: only a `pmovmskb` of a
     * `ymm` fills the word now, which is an 8- or a 16-bit lane at 256 bits. A 32-bit lane's eight
     * bits leave twenty-four above them and take the sentinel, which needs no feature at all.
     */
    assertTrue(width <= 32); // a wider register's mask is a `k` bit per lane, and is not this yet

    if(width == 32) {
        assertTrue(targetFeatures() & kFeatureBmi1);

        auto first = e.intrinsic(LowerIntrinsic::CttzWidth, scalar, bits);
        return shift ? e.binary(LowerInst::Shr, scalar, first, e.integer(scalar, shift)) : first;
    }

    auto marked = e.binary(LowerInst::Or, scalar, bits, e.integer(scalar, U64(1) << width));
    auto first = e.intrinsic(LowerIntrinsic::Cttz, scalar, marked);

    return shift ? e.binary(LowerInst::Shr, scalar, first, e.integer(scalar, shift)) : first;
}

/*
 * §45.3 A mask read complemented, and the three reductions that need no complement at all.
 *
 * `complemented` says that `bits` is the movemask of the *opposite* of the mask this reduction was
 * written about - see `foldComplementedCompare` below, which is what arranges that. The bitmap of a
 * complemented mask is the bitmap exclusive-ored with the pattern, so one instruction would answer
 * it; three of the four consumers do not need even that:
 *
 *     all(!m)   = none(m)        the bitmap against zero
 *     any(!m)   = !all(m)        the bitmap against the full pattern
 *     count(!m) = lanes - count(m)
 *
 * The first two are the *same* comparison this emits either way with the constant swapped, so they
 * cost nothing whatsoever. The population is written as the exclusive-or rather than as a
 * subtraction from the lane count, which is the same instruction count and one fewer live value -
 * and `firstSet` has no identity of its own and takes the exclusive-or as well.
 */
static LowerValue* expandMaskBitsReduce(Expansion& e, LowerReduce reduce, LowerValue* source,
                                        LowerValue* bits, bool& truth, bool complemented = false) {
    auto type = source->type;
    auto scalar = LowerType::Int32;
    auto width = maskBitCount(type);
    auto full = width < 32 ? (U64(1) << width) - 1 : ~U64(0);

    if(!bits) bits = emitMaskBits(e, source);

    // The two that read the bits themselves rather than a property of them, which is where the
    // complement has to become an instruction. Only the mask's own bits are flipped: nothing above
    // them is the mask's, and `firstSet` reads a set bit there as a lane.
    if(complemented && (reduce == LowerReduce::FirstSet || reduce == LowerReduce::Add)) {
        bits = e.binary(LowerInst::Xor, scalar, bits, e.integer(scalar, full));
    }

    if(reduce == LowerReduce::FirstSet) return expandMaskFirstSet(e, bits, type);

    if(reduce == LowerReduce::Add) {
        auto counted = e.intrinsic(LowerIntrinsic::Popcnt, scalar, bits);

        // Every lane contributed `maskBitsPerLane` equal bits, so the population is the lane count
        // times that - and it is a power of two, so the division is a shift. Three of the four lane
        // widths contribute one bit and need no shift at all; the 16-bit lane is the one that does.
        auto shift = maskBitShift(type);

        return shift ? e.binary(LowerInst::Shr, scalar, counted, e.integer(scalar, shift)) : counted;
    }

    /*
     * `any` is "not zero" and `all` is "every byte set", and what is answered is the *comparison* -
     * so a branch on it reads the flags the comparison left standing and spends nothing at all.
     *
     * It used to be a select of one and zero, which the caller then narrowed with an `& 1` because
     * the tree this replaced handed back the -1 a mask lane holds. Both are gone: a comparison
     * already answers 0 or 1, so the narrowing is a no-op, and materializing it is `genSetCc`'s job
     * on the paths that genuinely want a value. `if any(hits)` was `xor ; test ; mov ; cmove ; and ;
     * jne` and is now `test ; jne` - four instructions per iteration of every search loop.
     */
    /*
     * The pattern is signed at the width the comparison happens at, which is the difference between
     * a three-byte instruction and nine. A mask filling a whole 256-bit register wants every bit of
     * the `i32` set, and that written as `0xffffffff` is a constant no immediate carries - so it was
     * materialized into a register, and in a leaf function that register cost a callee-saved push
     * and pop as well. The same number as an `i32` is -1, which is an `imm8`.
     */
    /*
     * `any` is a comparison against zero and `all` one against the full pattern; complemented, each
     * keeps its relation and takes the *other's* constant. That is the identity above written out -
     * `any(!m)` is "not every bit set" and `all(!m)` is "no bit set" - and it is why these two cost
     * nothing at all: the instruction emitted is the same instruction with a different immediate.
     */
    auto isAny = reduce == LowerReduce::Or;
    auto wanted = isAny != complemented ? U64(0) : full;

    truth = true;
    return e.compare(isAny ? LowerCmp::neq : LowerCmp::eq, bits, e.integer(scalar, wanted));
}

/*
 * §37 One movemask for every consumer of a mask.
 *
 * `if any(hits) then return Just(at + firstSet(hits))` is two consumers of one mask, in two blocks,
 * and expanded one at a time it is two `pmovmskb`s of the same register - the second on the path
 * where the first has already answered. The instruction is the same instruction; what stops the
 * expansion from saying so is that it runs after the tier where common subexpressions are removed,
 * so nothing below it will notice.
 *
 * So the movemask is placed *once*, immediately below the instruction that defines the mask. That
 * position needs no dominance computation to justify: a definition dominates every use of what it
 * defines, and an instruction directly under it dominates exactly what it does - so a movemask there
 * is readable from every consumer of the mask, in this block and in every block below.
 *
 * **Only where there is more than one consumer**, which is the whole of what keeps it from being a
 * hoist. A single reader has its movemask expanded where it stands, as before; moving that one up to
 * the definition would lengthen a general-register live range across whatever lies between, and buy
 * nothing. A mask defined by a phi or by an argument has no position "below the definition" in an
 * instruction list at all, and takes the same path.
 */

// The mask a reduction reads, where that reduction is one that goes through the movemask - which
// is every mask reduction, and nothing else.
static LowerValue* maskBitsSource(LowerBase base, LowerInst* inst) {
    if(inst->kind != LowerInst::VecReduce) return nullptr;

    auto reduce = (LowerInstVecReduce*)inst;
    auto source = base[reduce->from];

    if(!source->type.isMask()) return nullptr;
    if(reduce->getReduce() == LowerReduce::Bits) return nullptr;   // the movemask itself

    return source;
}

/*
 * How many reductions read this mask through the movemask, and whether *every* reader of it is one
 * that a live-lane range can serve.
 *
 * The count is what decides whether a movemask is placed once or expanded where it stands. The
 * second answer is what §41.3's fusion needs and is stricter in two ways, because that rewrite
 * removes the `and` rather than reading it:
 *
 *   - a use that is not a mask reduction at all keeps the `and` alive, so the range would be a
 *     `bzhi` placed *beside* the vector comparison rather than instead of it, and
 *   - `all` cannot be served by ranged bits at any price. It asks whether every lane holds, which
 *     is the movemask against a full pattern - and the whole of what the range does is clear the
 *     bits above the live lanes, so the pattern would have to become the range.
 */
static Size maskBitsReaders(LowerBase base, LowerValue* source, bool& rangeable) {
    Size readers = 0;
    rangeable = true;

    for(auto use: source->uses.contents(base)) {
        auto inst = base[use];

        if(maskBitsSource(base, inst) != source) {
            rangeable = false;
            continue;
        }

        if(((LowerInstVecReduce*)inst)->getReduce() == LowerReduce::And) rangeable = false;
        readers++;
    }

    return readers;
}

/*
 * The point immediately below the instruction that defines this value - which dominates exactly what
 * the definition does, and is where anything shared between the definition's readers goes.
 *
 * A **phi** takes the top of its own block, which is the same statement one line up: a phi is not in
 * the instruction list and has no slot below it, but every value it defines is live from the head of
 * its block, so position zero dominates exactly what the phi does. That is the shape a search whose
 * mask comes from two arms has, and without it such a mask pays a movemask per consumer.
 *
 * An **argument** is declined, and that is a judgement rather than a limitation. The only position
 * that dominates every use of one is the top of the entry block, which may be an arbitrary distance
 * - a call, a loop - from the consumers that would read it, and a mask handed in as a parameter is
 * not a shape this language's own vector code produces. It keeps a movemask per consumer, which is
 * what every consumer had before any of this.
 */
static bool positionBelowDefinition(LowerBase base, LowerValue* value, LowerBlock*& block, Size& at) {
    auto definition = value->inst();

    block = base[definition->block];
    at = 0;

    if(isPhi(definition)) return true;

    auto list = block->instructions.contents(base);
    while(at < list.size() && base[list[at]] != definition) at++;
    if(at == list.size()) return false; // an argument: see above

    at++;
    return true;
}

// One movemask below the mask's definition, for a mask more than one consumer reads. Where that
// position is, and which definitions have one at all, is `positionBelowDefinition` above.
static LowerValue* placeSharedMaskBits(LowerBase base, LowerFunction& fun, LowerValue* source) {
    LowerBlock* block = nullptr;
    Size at = 0;

    if(!positionBelowDefinition(base, source, block, at)) return nullptr;

    Expansion e { base, fun, block, at };
    return emitMaskBits(e, source);
}

/*
 * §52 A mask two arms join, read as the bits its two arms already computed.
 *
 * `if any(hits) then return at + firstSet(hits)` written in a loop *and* in the tail after it is two
 * such tests branching into one block, and the merge of the two arms is a phi of the two masks:
 *
 *   b_loop: %h1 = cmp ... ; %b1 = movmsk %h1 ; test %b1 ; jne b_hit
 *   b_tail: %h2 = and ... ; %b2 = movmsk %h2 ; test %b2 ; jne b_hit
 *   b_hit:  %h  = phi [b_loop, %h1], [b_tail, %h2]
 *           %b  = movmsk %h                        <- a third movemask of a mask already measured
 *           bsf (%b | sentinel)
 *
 * §37 places one movemask per mask and that is exactly what it did here: three masks, three
 * movemasks. What it cannot see is that the third is a *join* of the other two - and a movemask
 * distributes over one, because it is a function of its operand alone. So the phi moves into the
 * scalar domain: `%b = phi [b_loop, %b1], [b_tail, %b2]`, and the vector phi dies with it.
 *
 * **Each alternative's bits are reused rather than computed**, which is what makes this free rather
 * than a trade. An arm that branches here on `any` has measured its own mask to do it, so the bits
 * this joins are the ones the guard already holds - `existingMaskBits` finds that movemask, and a
 * placement below the definition is the fallback for an arm whose own reduction has not been reached
 * yet. Either way no movemask is added; one is removed, and the phi that replaces it holds a general
 * register where the old one held a vector across the join.
 *
 * The bits of a *complemented* mask (§45.3) are the complement of the bits asked about, so the
 * alternatives have to agree about that: the flag is carried out to the caller, which records it for
 * the joined value exactly as it would for a shared one. Disagreement is a refusal rather than an
 * exclusive-or per arm, since nothing produces one - a complement comes from the shape of the
 * comparison, and the arms of a search are the same shape.
 */

// A movemask of this mask that every reader of the mask can see. Only one in the definition's own
// block is accepted: the definition dominates wherever the mask is live, so a movemask beside it
// does too, and one *elsewhere* would need the dominator tree to justify.
static LowerValue* existingMaskBits(LowerBase base, LowerValue* mask) {
    for(auto use: mask->uses.contents(base)) {
        auto inst = base[use];
        if(inst->kind != LowerInst::VecReduce) continue;
        if(((LowerInstVecReduce*)inst)->getReduce() != LowerReduce::Bits) continue;
        if(base[inst->block] != base[mask->inst()->block]) continue;

        return &((LowerInstVecReduce*)inst)->result;
    }

    return nullptr;
}

// Whether any reduction reads this mask through the movemask - which is what says that a movemask
// placed for it now is work an expansion below was going to do anyway.
static bool hasMaskBitsReader(LowerBase base, LowerValue* mask) {
    for(auto use: mask->uses.contents(base)) {
        if(maskBitsSource(base, base[use]) == mask) return true;
    }

    return false;
}

/*
 * The mask sources a function's reductions have settled, and how each was settled.
 *
 * A function has a handful of these at most - one per mask a search or a tally reads - so the lookup
 * is a walk rather than a map. `bits` is the movemask placed once where every consumer can see it,
 * where one was placed at all; `complemented` says the comparison under it was rewritten to the
 * relation the machine has (§45.3), so the bits every consumer reads are the opposite of the ones it
 * asked about. An entry may carry the second without the first.
 */
struct SharedMaskBits {
    LowerValue* source;
    LowerValue* bits;
    bool complemented;
};

using SharedMaskList = SmallArray<SharedMaskBits, 8>;

static LowerValue* placeJoinedMaskBits(LowerBase base, LowerFunction& fun, LowerValue* source,
                                       SharedMaskList& shared, bool& complemented)
{
    auto phi = (LowerInstPhi*)source->inst();
    auto block = base[phi->block];
    auto count = Size(phi->usedCount);

    // The bits each alternative contributes, null where one is still to be placed. Every alternative
    // is settled before *any* movemask is placed, because a refusal after the first would leave the
    // arm's own bits somewhere the arm did not choose.
    SmallArray<LowerValue*, 8> alternatives;
    auto complementedAll = false;

    for(Size i = 0; i < count; i++) {
        auto incoming = base[phi->used()[i]];
        if(!incoming->type.isMask()) return nullptr;

        LowerValue* bits = nullptr;
        auto flipped = false;

        for(auto& entry: shared) {
            if(entry.source != incoming) continue;

            bits = entry.bits;
            flipped = entry.complemented;
            break;
        }

        if(!bits) bits = existingMaskBits(base, incoming);

        if(!bits) {
            LowerBlock* at = nullptr;
            Size index = 0;

            // An alternative no reduction reads is one whose movemask would be new work, and the
            // join saves exactly one - so paying for it per arm is a trade rather than a saving. One
            // with nowhere to put a movemask is an argument, which `positionBelowDefinition` refuses
            // for reasons of its own.
            if(!hasMaskBitsReader(base, incoming)) return nullptr;
            if(!positionBelowDefinition(base, incoming, at, index)) return nullptr;
        }

        if(i && flipped != complementedAll) return nullptr;

        complementedAll = flipped;
        alternatives.push(bits);
    }

    for(Size i = 0; i < count; i++) {
        if(alternatives[i]) continue;

        auto incoming = base[phi->used()[i]];
        alternatives[i] = placeSharedMaskBits(base, fun, incoming);

        // Recorded, so that the arm's own reduction reads this movemask rather than emitting a
        // second one where it stands - which is what makes the placement free.
        shared.push(SharedMaskBits { incoming, alternatives[i], false });
    }

    auto joined = makePhi(fun.arena, LowerType::Int32, U32(count));
    auto used = joined->used();
    auto sources = joined->sources();

    for(Size i = 0; i < count; i++) {
        used[i] = alternatives[i] - base;
        sources[i] = phi->sources()[i];
    }

    block->addInst(base, joined);

    complemented = complementedAll;
    return &joined->result;
}

/*
 * `none`, which arrives as `any` and a negation and leaves as one comparison.
 *
 * The library writes `none(m)` as `any(m)` exclusive-ored with one - the negation of a `Bool` is
 * arithmetic, and above this tier there is nothing to negate but the value. What that costs once the
 * reduction has become a comparison is the whole materialization the comparison was meant to avoid:
 * `test ; setne ; xor $1 ; jne` where the answer wanted is `test ; je`.
 *
 * So the comparison is inverted instead, which is exact - it answers 0 or 1 by construction, and the
 * negation of a bit is the other relation. Asked here rather than as a peephole over every `xor`,
 * because here the comparison's uses are known to be the reduction's: it was created three lines ago
 * with the uses the reduction had and nothing else, which is what makes rewriting it rather than
 * copying it sound.
 */
static void foldNegatedTruth(LowerBase base, LowerValue* value) {
    if(value->uses.size() != 1) return;

    auto use = base[value->uses.get(base, 0)];
    if(use->kind != LowerInst::Xor) return;

    auto negation = (LowerInstBinary*)use;
    auto lhs = base[negation->lhs];
    auto other = lhs == value ? base[negation->rhs] : lhs;

    /*
     * The constant read as an instruction rather than through `isImm`, which asks a different
     * question: that one means "already embedded into its reader", and the embedding happens in
     * `selectMachineInstructions` several passes below this. Here every constant is still an
     * instruction of its own, and the one this looks for is the `1` a negation is written as.
     */
    if(other == value || other->inst()->kind != LowerInst::Imm) return;
    if(((LowerImm*)other->inst())->i != 1) return;

    // The two the mask expansion produces, and the two a truth value can be compared with. Written
    // out rather than negated generically because a signed relation has no business here at all.
    auto cmp = (LowerInstCmp*)value->inst();
    auto kind = cmp->getCmp();

    if(kind == LowerCmp::neq) cmp->setCmp(LowerCmp::eq);
    else if(kind == LowerCmp::eq) cmp->setCmp(LowerCmp::neq);
    else return;

    replaceAllUses(base, &negation->result, value);
    removeInst(base, negation);
}

/*
 * §41.3 The live-lane range of a masked tail, taken out of the vector bank.
 *
 * Every bulk operation in `resolve/core.cpp` is written so that the last chunk contributes only the
 * lanes that are really there:
 *
 *     count(m .& maskUpTo(live))
 *
 * and `maskUpTo(n)` is `iota .< splat(n)`. Written out, that is a general register moved into a
 * vector one, a broadcast, a comparison against `iota` - which is a 32-byte constant held in
 * `.rodata` and in a register for the whole function, plus the bias constant an *unsigned* lane
 * comparison needs and the two exclusive-ors that apply it - and then an `and` per consumer. Eight
 * instructions and three pooled constants to say "only the first `n` lanes count".
 *
 * Every one of those consumers goes through a movemask (§37), and a bit range of a general register
 * is one instruction: `bzhi dst, bits, n` keeps the low `n` bits and clears the rest. So the range
 * stops being a vector at all. What it takes away is not the `and` - it is `iota`, its bias, the two
 * registers holding them across the loop and the `.rodata` they sat in.
 *
 * ## The index, and the one thing the machine will not do
 *
 * `bzhi` reads its count from the *low byte* of its operand and clears nothing when that byte is at
 * or above the register width. That is the right answer for a count larger than the lane count -
 * every lane is live - and it is the wrong answer twice over otherwise: a *negative* count reads as
 * 255 and would answer "all lanes" where `iota <s n` answers none, and a count of 256 reads as zero
 * and would answer "no lanes" where the truth is all of them.
 *
 * So the count has to be known to be a small non-negative number before this is worth anything, and
 * `laneRangeIndex` below is where that is established rather than assumed. Two proofs, and between
 * them they cover what the library writes:
 *
 *   - **the high bits are known clear**, which `knownZeroBits` answers directly. A byte lane's count
 *     arrives as `n .& 255` because that is the lane's own width, so every string search and count
 *     is this case and pays nothing at all.
 *   - **the block is guarded by the comparison the count is a subtraction of**. `live = n - i` in a
 *     block entered only when `i <s n` cannot be negative, which is exactly the shape a chunked
 *     loop's tail has. The upper end is then one unsigned `min` against the lane count - three
 *     instructions in a block that runs once per call, against six per call in the vector bank.
 *
 * A count neither proof reaches is left as the vector comparison it was written as.
 *
 * ## §45.1 The range is one scalar, however many consumers read it
 *
 * `occurrencesVectors` has one consumer of the masked result and `indexOfVectors` has two - `any` in
 * the tail block and `firstSet` on the arm below it, both of `and(v .== sought, maskUpTo(live))`.
 * The second was refused outright while the fusion insisted on a single reader, and the whole vector
 * bank stayed for it: `iota` and its bias in `.rodata` and in two registers, a broadcast, a compare
 * and an `and`, so that two reductions of one mask could read the vector the range was written as.
 *
 * They do not read the vector. Both read the *movemask* of it, which §37 already places once below
 * the mask's definition - and a range over that placed movemask is one more instruction in the same
 * position, shared on exactly the same terms:
 *
 *     %bits = vpmovmskb %hits      one movemask of the data mask alone
 *     %live = bzhi %bits, %n       the range, applied once
 *
 * So the sharing is not a second mechanism. `placeFusedRangeBits` puts the pair where
 * `placeSharedMaskBits` puts the movemask - immediately below the `and`, which dominates every
 * consumer of it because the `and` does - and every consumer then reads `%live` as if it had been
 * the movemask, because for every consumer other than `all` that is what it is.
 *
 * What that leaves is a use count rather than a use: the `and` dies once the last reduction reading
 * it has been rewritten, which may be in a block below the one the range was placed in. That is why
 * the dead chain here is swept once for the whole function rather than once per block.
 */

struct LaneRange {
    LowerValue* mask = nullptr;    // the data mask the range is applied to
    LowerValue* count = nullptr;   // how many lanes are live, as a scalar
    LowerInst* combine = nullptr;  // the `and` of the two
    LowerInst* compare = nullptr;  // `iota REL splat(count)`
    LowerInst* splat = nullptr;    // the count moved into the vector bank, which is what stops
    bool ordered = false;          // the relation is signed, so a negative count means no lanes
    InstChain chain;       // the `iota` constant's own chain
};

// Whether these bytes are `0, 1, 2, ...` read at the lane width - `iota`, and the only constant this
// recognizes. Read little-endian per lane, which is what `constantVectorBytes` wrote.
static bool bytesAreIota(const U8* bytes, LowerType type) {
    auto width = laneBytes(type.lane);

    for(Size lane = 0; lane < type.lanes(); lane++) {
        U64 value = 0;
        copyMem(bytes + lane * width, &value, width);
        if(value != lane) return false;
    }

    return true;
}

/*
 * `and(m, iota REL splat(n))`, taken apart - or nothing.
 *
 * The relation is read rather than assumed: `iota .< splat(n)` is what the library writes, and the
 * lane type decides whether that is the signed or the unsigned comparison. Both are recognized and
 * which one it was is carried out, because it is what says whether a negative count is a question at
 * all - an unsigned lane has no negative counts to worry about.
 *
 * A **constant** count is declined and left to `foldConstantMasks`, which answers it exactly: the
 * full chunks of the same loop go through this identical line with `n` equal to the lane count, and
 * a mask that is all-ones should disappear rather than become a `bzhi` of a literal.
 *
 * *Who* reads the masked result is not asked here - `maskBitsReaders` is, and its `rangeable` is the
 * whole of the condition: every reader a reduction that goes through the movemask, and none of them
 * `all`. One reader and several are both served, and the difference is only where the `bzhi` goes.
 */
static bool matchLaneRangeMask(LowerBase base, LowerValue* source, LaneRange& into) {
    auto combine = source->inst();
    if(combine->kind != LowerInst::And || !source->type.isMask()) return false;

    auto binary = (LowerInstBinary*)combine;

    for(Size side = 0; side < 2; side++) {
        auto range = base[side ? binary->lhs : binary->rhs];
        auto mask = base[side ? binary->rhs : binary->lhs];
        auto compare = range->inst();

        if(compare->kind != LowerInst::Cmp || range->uses.size() != 1) continue;

        auto cmp = (LowerInstCmp*)compare;
        auto relation = cmp->getCmp();
        if(relation != LowerCmp::lt && relation != LowerCmp::ilt) continue;

        auto constant = base[cmp->lhs];
        auto splat = base[cmp->rhs];
        auto type = constant->type;

        if(!isIntVector(type) || splat->inst()->kind != LowerInst::VecSplat) continue;

        auto count = base[((LowerInstVecSplat*)splat->inst())->from];
        if(count->inst()->kind == LowerInst::Imm) continue; // see above: a fold, not a range

        auto size = Size(type.byteWidth());
        if(size > kMaxVectorBytes) continue;

        U8 bytes[kMaxVectorBytes] = {};

        // Collected straight into the result rather than into a list of its own and copied across.
        // Emptied on the way in because this loop has two sides and the failing one has to leave
        // nothing behind; what `into` holds is only read when this returns true.
        into.chain.clear();

        if(!constantVectorBytes(base, constant, bytes, size, into.chain)) continue;
        if(!bytesAreIota(bytes, type)) continue;

        into.mask = mask;
        into.count = count;
        into.combine = combine;
        into.compare = compare;
        into.splat = splat->inst();
        into.ordered = relation == LowerCmp::ilt;

        return true;
    }

    return false;
}

/*
 * Whether this value cannot be negative where the block below it runs.
 *
 * Two answers, and the second is the one that exists for this. `knownZeroBits` is the general one
 * and covers everything the front end masked on the way in; the guard is the shape a chunked tail
 * has, and nothing weaker reaches it - `live = n - i` is a subtraction of two values neither of
 * which is bounded on its own.
 *
 * The guard is read *locally*: the block has one predecessor, and that predecessor branches here on
 * exactly the comparison the subtraction is of. No dominator tree, and no reasoning about paths -
 * one predecessor is what makes "the branch was taken" true of every entry to this block.
 */
static bool isNonNegativeIn(LowerBase base, LowerBlock* block, LowerValue* value) {
    if(knownZeroBits(base, value) & (U64(1) << 31)) return true;

    auto inst = value->inst();
    if(inst->kind != LowerInst::Sub) return false;

    auto subtraction = (LowerInstBinary*)inst;
    if(block->incoming.size() != 1) return false;

    auto from = base[block->incoming.get(base, 0)];
    auto terminator = base[from->terminator];
    if(terminator->kind != LowerInst::Je) return false;

    auto branch = (LowerInstJe*)terminator;
    if(base[branch->then] != block) return false; // the arm where the comparison held

    auto condition = base[branch->cond]->inst();
    if(condition->kind != LowerInst::Cmp) return false;

    auto cmp = (LowerInstCmp*)condition;
    auto relation = cmp->getCmp();

    // `a - b` is not negative where `b < a` or `b <= a` held, at either signedness - the unsigned
    // pair as well, since a difference the unsigned relation makes non-negative is one the signed
    // reading of `Int` agrees about for every value below 2^31.
    auto ordered = relation == LowerCmp::ilt || relation == LowerCmp::ile;
    if(!ordered && relation != LowerCmp::lt && relation != LowerCmp::le) return false;

    return base[cmp->lhs] == base[subtraction->rhs] && base[cmp->rhs] == base[subtraction->lhs];
}

/*
 * The count as a `bzhi` index, or nothing where it cannot be made into one safely.
 *
 * Three things have to hold of what is handed to the instruction, and each is either proved or paid
 * for: it is not negative (proved, or the range is declined), it is not above 255 (an unsigned `min`
 * against the lane count, unless the bits above the byte are already known clear), and it is the
 * count *scaled* by whatever a lane is worth in the movemask - which after §41.5 is one bit at three
 * of the four lane widths and needs no scaling at all.
 */
static LowerValue* laneRangeIndex(Expansion& e, const LaneRange& range, LowerType type) {
    auto scalar = LowerType::Int32;
    auto count = range.count;
    auto shift = maskBitShift(type);
    auto lanes = U64(type.lanes());

    // An unsigned relation has no negative counts to rule out; a signed one has, and a count this
    // cannot place above zero would answer "every lane" where the comparison answers "none".
    if(range.ordered && !isNonNegativeIn(e.base, e.block, count)) return nullptr;

    /*
     * Whether the scaled count is the whole of its own low byte, which is what makes the machine's
     * saturation at the register width the right answer and the `min` below unnecessary.
     *
     * Asked as "every bit from here up is known zero", the position being what the scaling leaves
     * room for: a count that has to be shifted left by one may reach 127 rather than 255. A byte
     * lane's count arrives as `n .& 255` and clears the question outright.
     */
    auto known = knownZeroBits(e.base, count);
    auto highBits = U64(0xffffffff) & ~((U64(1) << (8 - shift)) - 1);

    if((known & highBits) != highBits) {
        // `min(count, lanes)` unsigned, which is a compare and a conditional move. Correct at both
        // ends: a count above the lane count means every lane is live and `lanes` says exactly that,
        // and the comparison is unsigned because by here the count is known not to be negative.
        auto limit = e.integer(scalar, lanes);
        auto within = e.compare(LowerCmp::lt, count, limit);
        count = e.select(scalar, within, count, limit);
    }

    return shift ? e.binary(LowerInst::Shl, scalar, count, e.integer(scalar, shift)) : count;
}

/*
 * The range-limited movemask, placed once below the `and` it replaces - §45.1 above.
 *
 * Two instructions and whatever the index costs, in a position that dominates every reader of the
 * `and` for the reason `placeSharedMaskBits` gives about the movemask alone. The count is available
 * there without asking: it is read by the splat, which is read by the comparison, which is read by
 * the `and` this sits under, so its definition dominates this point through three edges of the same
 * chain.
 *
 * A refusal costs nothing to back out of. `laneRangeIndex` declines before it emits - the proof it
 * cannot make is the first thing it asks - so a null here has left the block exactly as it was.
 */
static LowerValue* placeFusedRangeBits(LowerBase base, LowerFunction& fun, LowerValue* source,
                                       const LaneRange& range) {
    LowerBlock* block = nullptr;
    Size at = 0;

    if(!positionBelowDefinition(base, source, block, at)) return nullptr;

    Expansion e { base, fun, block, at };
    auto index = laneRangeIndex(e, range, source->type);
    if(!index) return nullptr;

    return e.intrinsic2(LowerIntrinsic::Bzhi, LowerType::Int32, emitMaskBits(e, range.mask), index);
}

// What a fused range leaves behind: the `and`, the comparison that built it, the splat under that
// and the `iota` chain under the comparison. Each goes only once its own use list has emptied, which
// is `removeDeadChain`'s rule and what keeps an `iota` two masked tails share exactly where it is.
/*
 * §45.3 A comparison the machine has only the complement of, complemented after the movemask.
 *
 * Three of the six relations a signed lane can be compared with are not instructions: `neq` is
 * `pcmpeq` inverted, and `ile` and `ige` are `pcmpgt` inverted. Inverting a *vector* is an all-ones
 * register and an exclusive-or against it, so each of the three costs two extra vector instructions
 * and a register held for the constant - `VecLanewise.comparisons` has three copies of
 *
 *     pcmpgtd  xmm2, xmm1
 *     pcmpeqd  xmm15, xmm15
 *     pxor     xmm2, xmm15
 *     movmskps eax, xmm2
 *
 * in one 182-byte function. But a mask whose every reader is a reduction is never *looked at* as a
 * vector: what each of them reads is the movemask, and the complement of a bitmap is a scalar
 * operation on a value that is already in a general register.
 *
 * So the comparison is rewritten to the relation the machine has, and its readers are told the bits
 * they are given are the opposite ones. `expandMaskBitsReduce` is where that is paid for, and for
 * three of the four consumers it is not paid at all - `all` and `any` become each other with the
 * constant swapped, and `count` is the lane count less the count. Only `firstSet` and the population
 * need the exclusive-or, and one scalar instruction against two vector ones is still the trade.
 *
 * **Every reader has to be a reduction**, which is the whole of the condition. A mask that is also
 * selected with, stored, or combined with another mask is one whose lanes are the value, and
 * rewriting the comparison under it would be answering a different question. `Bits` reductions are
 * not in the set either, which is what makes this safe to ask only at the first consumer: a movemask
 * placed for a mask is a reader of it, so a mask that already has one is a mask this declines.
 */
static bool foldComplementedCompare(LowerBase base, LowerValue* source) {
    auto inst = source->inst();
    if(inst->kind != LowerInst::Cmp) return false;

    auto cmp = (LowerInstCmp*)inst;
    auto type = base[cmp->lhs]->type;
    if(!isIntVector(type)) return false;

    // The machine's own question, asked of the relation as it will be selected - and asked twice,
    // because what makes this worth doing is that the *negation* is an instruction where this is
    // not. `neq`, `ile` and `ige` are the three that answer yes to the first and no to the second.
    auto relation = cmp->getCmp();
    auto negated = negatedCmp(relation);

    if(!packedCompareIsInverted(type, packedCompareRelation(relation))) return false;
    if(packedCompareIsInverted(type, packedCompareRelation(negated))) return false;

    /*
     * Nothing but reductions, each of which goes through the movemask - and at most one of them a
     * reduction the complement costs an instruction.
     *
     * The vector complement is paid *once* however many readers there are: the mask is inverted
     * where it is built. So a scalar one has to be paid once too, and `count` and `firstSet` each
     * pay their own - two of them is one instruction traded for two, which the vector register the
     * all-ones constant occupies does not make up for. `any` and `all` are free at any number, being
     * the same comparison against the other constant.
     */
    Size complements = 0;

    for(auto use: source->uses.contents(base)) {
        auto inst = base[use];
        if(maskBitsSource(base, inst) != source) return false;

        auto kind = ((LowerInstVecReduce*)inst)->getReduce();
        if(kind == LowerReduce::FirstSet || kind == LowerReduce::Add) complements++;
    }

    if(complements > 1) return false;

    cmp->setCmp(negated);
    return true;
}

// A phi this file emptied, taken back out - defined with the rotation's own phi helpers below, and
// read here because a joined mask leaves one behind.
static bool dropUnusedPhi(LowerBase base, LowerBlock* block, LowerInstPhi*& phi);

static void pushFusedRange(InstChain& dead, const LaneRange& range) {
    dead.push(range.combine);
    dead.push(range.compare);
    dead.push(range.splat);
    for(auto link: range.chain) dead.push(link);
}

static void lowerVectorReductions(Context&, LowerBase base, LowerFunction& fun) {
    // What this walk has settled, per mask - see SharedMaskBits, which the joined placement below
    // reads and writes as well.
    SharedMaskList shared;

    // The mask phis a joined placement emptied, swept once the walk is done: each loses its last
    // reader when the reduction that read it is expanded, and a phi nothing reads is still a live
    // range the allocator would carry a vector register through the join for.
    SmallArray<LowerInstPhi*, 4> joined;

    /*
     * What a fused lane range leaves behind: the `and`, the comparison that built the range, the
     * splat under it and the `iota` chain under that.
     *
     * Cleared once the *function* has been walked rather than once per block, which is §45.1's one
     * structural consequence. Each of these stands above the reduction being expanded, so removing
     * one during the walk would renumber what the loop indexes - and the `and` of a mask two blocks
     * read is still live when the first of them is done, so a per-block sweep would find it in use
     * and leave the whole vector bank standing.
     */
    InstChain dead;

    /*
     * The function is walked twice, and what the second pass is for is the *joins* - a reduction
     * whose mask is a phi, which §52's placement answers out of the bits its alternatives already
     * hold. Those bits are what an arm's own guard computed, and an arm below this one in the block
     * order has not been expanded when the join is reached: taken in one pass, the join would find
     * nothing to reuse in half of its predecessors and would place a movemask that the arm was about
     * to place for itself.
     *
     * Nothing else about the two passes differs, and there is no third: a *chain* of joins - a mask
     * phi whose own alternative is one - settles in whichever order the second pass reaches them,
     * and both orders are right. One that has already been joined is in `shared` and its bits are
     * reused; one that has not is measured where it stands, which is the movemask its own reduction
     * was going to place anyway.
     */
    for(Size pass = 0; pass < 2; pass++)
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // Indexed and advanced by hand, like the passes above: the expansion is inserted in front of
        // the reduction and moves everything after it.
        for(Size i = 0; i < block->instructions.size(); ) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::VecReduce) { i++; continue; }

            auto reduce = (LowerInstVecReduce*)inst;
            auto source = base[reduce->from];

            // The movemask is this backend's own instruction and is what every expansion below is
            // written in terms of, so one standing here is finished rather than pending. It is here
            // because the walk placed it: a mask defined in this block by an instruction above this
            // one is where a shared movemask goes, and expanding it again would not terminate.
            if(reduce->getReduce() == LowerReduce::Bits) { i++; continue; }

            // Which pass this reduction belongs to - see above.
            auto join = source->type.isMask() && isPhi(source->inst());
            if(join != (pass == 1)) { i++; continue; }

            /*
             * The bits this reduction reads, and the three ways there are to one.
             *
             * A value already in `shared` is a movemask - ranged or plain - placed below this mask's
             * definition by an earlier consumer, and is the whole answer: what a consumer wants of
             * the bits is the same thing whichever of the two it is reading.
             *
             * Otherwise the live-lane range is asked first, because it decides what the movemask is
             * taken *of* - see LaneRange above. `all` is the one consumer it cannot serve, and it
             * disqualifies the mask rather than itself: the comparison against a full pattern is
             * exactly what the range clears the top of, so a mask any reader asks `all` of keeps the
             * vector form for all of them. `maskBitsReaders` is where both halves of that are
             * counted.
             *
             * The placement may go *into this block above this instruction*, so the position the
             * expansion starts at is re-read afterwards rather than carried across the call.
             *
             * The complement (§45.3) is the last of the three and is asked only where neither of the
             * others applied - a ranged mask is an `and` rather than a comparison, and a mask an
             * earlier consumer has settled is settled including which way round its bits are. It is
             * *recorded* even where no movemask was shared, since the rewrite it makes is to the
             * comparison itself: a later consumer looking at it would find the relation already
             * flipped and read the bits the wrong way round.
             */
            SharedMaskBits* settled = nullptr;
            for(auto& entry: shared) {
                if(entry.source == source) { settled = &entry; break; }
            }

            auto bits = settled ? settled->bits : nullptr;
            auto complemented = settled ? settled->complemented : false;

            LaneRange range;
            auto fused = false; // this reduction applies the range itself, where it stands

            /*
             * A mask a phi joins, answered out of its alternatives' own bits - §52. Taken before the
             * three below and instead of them: a phi is not a comparison, so neither the lane range
             * nor the complement has anything to read, and the movemask this places is a *phi* of
             * placements rather than one of its own.
             */
            if(!settled && join && maskBitsSource(base, inst)) {
                bits = placeJoinedMaskBits(base, fun, source, shared, complemented);

                if(bits) {
                    shared.push(SharedMaskBits { source, bits, complemented });

                    // Once per phi however many reductions read it: the second would be a sweep of
                    // an instruction the first had already taken out of its block.
                    if(!joined.containsValue((LowerInstPhi*)source->inst())) {
                        joined.push((LowerInstPhi*)source->inst());
                    }
                }
            }

            if(!bits && !settled && maskBitsSource(base, inst)) {
                auto rangeable = false;
                auto readers = maskBitsReaders(base, source, rangeable);

                auto ranged = rangeable && (targetFeatures() & kFeatureBmi2)
                    && matchLaneRangeMask(base, source, range);

                if(ranged && readers > 1) {
                    // One movemask and one `bzhi` for every consumer of the masked result, which is
                    // §45.1. A refusal falls through to the plain shared movemask below.
                    bits = placeFusedRangeBits(base, fun, source, range);
                    if(bits) {
                        shared.push(SharedMaskBits { source, bits, false });
                        pushFusedRange(dead, range);
                    }
                } else if(ranged) {
                    // The single reader, which is this one: no position dominates it more usefully
                    // than the one it occupies, and hoisting the movemask to the definition would
                    // lengthen a general-register live range across whatever lies between.
                    fused = true;
                }

                if(!bits && !fused) {
                    complemented = foldComplementedCompare(base, source);
                    if(readers > 1) bits = placeSharedMaskBits(base, fun, source);
                    if(bits || complemented) shared.push(SharedMaskBits { source, bits, complemented });
                }
            }

            auto list = block->instructions.contents(base);
            Size at = i;
            while(at < list.size() && base[list[at]] != inst) at++;
            assertTrue(at < list.size()); // it was at `i`, or one below it if a movemask was placed

            Expansion e { base, fun, block, at };
            auto truth = false;

            /*
             * The movemask of the data mask alone, and the range applied to its bits. The count is
             * asked for last because it is the half that can refuse - `laneRangeIndex` declines a
             * count it cannot place inside the byte the instruction reads - and a refusal here falls
             * back to expanding the `and` as it stands, which is what every consumer did before.
             */
            if(fused) {
                if(auto index = laneRangeIndex(e, range, source->type)) {
                    bits = e.intrinsic2(LowerIntrinsic::Bzhi, LowerType::Int32,
                                        emitMaskBits(e, range.mask), index);

                    pushFusedRange(dead, range);
                } else {
                    // Nothing was emitted for the range, so nothing has to be taken back: the two
                    // producers `laneRangeIndex` may leave standing are its own `min`, and it emits
                    // that only on the path that then answers.
                }
            }

            auto scalar = expandReduce(e, reduce->getReduce(), source, bits, truth, complemented);

            /*
             * A mask's `and` and `or` answer a `Bool`, and what the extract handed back is `-1` or
             * `0` - every bit of a set lane, which is what a mask lane holds. `& 1` is the narrowing
             * to a truth value, and it is exact rather than approximate precisely because the lane
             * has no other two values it could have held.
             *
             * `truth` is the expansion saying it has already answered one: the movemask route ends
             * in a comparison, which is a 0 or a 1 by construction and is worth far more than the
             * narrowing - a branch reads it out of the flags.
             */
            if(source->type.isMask() && !truth && reduce->getReduce() != LowerReduce::Add
               && reduce->getReduce() != LowerReduce::FirstSet) {
                scalar = e.binary(LowerInst::And, scalar->type, scalar, e.integer(scalar->type, 1));
            }

            replaceAllUses(base, &reduce->result, scalar);
            removeInst(base, reduce);

            // And `none`, which is this comparison with a negation on top of it - asked after the
            // uses have moved across, since what it reads is the comparison's users.
            if(truth) foldNegatedTruth(base, scalar);

            // Past the whole expansion. Removing the reduction from the end of it closed the gap the
            // insertions opened, and nothing in what was produced is a reduction.
            i = e.at;
        }
    }

    // Each is removed only once its own use list is empty, which is what keeps a constant two ranges
    // share exactly where it is - and `iota` in a function with two masked tails is exactly that
    // constant.
    removeDeadChain(base, dead);

    // And the vector phis the joins replaced, each of which is empty exactly when every reduction
    // that read it has been expanded - which is now. One that still has a reader stays: a mask a
    // select or a store also reads is a value the join shared rather than removed.
    for(auto phi: joined) {
        auto block = base[phi->block];
        dropUnusedPhi(base, block, phi);
    }
}

/*
 * A comparison and the select that reads it, recognized as one minimum or maximum.
 *
 * `min` and `max` have no instruction in the portable IR - `emitMinMax` in resolve/simd.cpp writes
 * them as `select(a < b, a, b)`, which is what a target without a packed minimum needs anyway and
 * what LLVM's own selection folds back. x86 has the instruction at every lane width but the
 * quadword, so this is where the pair becomes one: three instructions (a compare, a blend, and the
 * mask register between them) down to `vpmaxsd`, and the operand it reads may then come out of
 * memory, which a blend's could not.
 *
 * ## The two shapes, and why one of them exchanges the operands
 *
 * `select(a REL b, a, b)` is the shape the library and the reduction tree both build, and it maps
 * straight across: a `lt` keeps the left operand where it is smaller, which is a minimum with the
 * operands in that order. The mirror `select(a REL b, b, a)` is the same operation with the
 * comparison read the other way round - `a < b ? b : a` is `max(b, a)` - so it is recognized as the
 * opposite kind with the operands exchanged rather than declined.
 *
 * **The order survives the exchange, and that is the whole of what makes this exact at a float
 * lane.** `minps a, b` answers `b` whenever the comparison is false, which is what a NaN in either
 * operand produces and what `-0.0` against `+0.0` produces; so it is `select(a < b, a, b)` bit for
 * bit, and it is *not* `select(b > a, ...)` with the operands left where they were.
 *
 * ## What is declined
 *
 * A non-strict relation at a float lane. `select(a <= b, a, b)` and `minps a, b` differ at the pair
 * `(+0.0, -0.0)` - the comparison holds, so the select answers `+0.0` where the instruction answers
 * `-0.0` - and nothing in the language says a program may not have written it. An integer lane has
 * no such pair and takes `le` and `lt` alike.
 *
 * A quadword integer lane, which has no `pminsq` before AVX-512 (see the form table), and a mask the
 * select reads that anything else reads too: the comparison would then stay and this would be an
 * instruction added rather than two replaced.
 */

// The same comparison with its operands exchanged: `a < b` is `b > a`. Equality and the unordered
// tests are their own mirrors and are not relations this recognizes anyway.
static LowerCmp mirroredCmp(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::lt:  return LowerCmp::gt;
        case LowerCmp::le:  return LowerCmp::ge;
        case LowerCmp::gt:  return LowerCmp::lt;
        case LowerCmp::ge:  return LowerCmp::le;
        case LowerCmp::ilt: return LowerCmp::igt;
        case LowerCmp::ile: return LowerCmp::ige;
        case LowerCmp::igt: return LowerCmp::ilt;
        case LowerCmp::ige: return LowerCmp::ile;
        default:            return cmp;
    }
}

// Which minimum or maximum `select(a REL b, a, b)` is, or nothing where the relation is not an
// ordering this machine has an instruction for at this lane kind.
static Maybe<LowerMinMax> minMaxForRelation(LowerCmp cmp, bool isFloat) {
    switch(cmp) {
        case LowerCmp::lt:  return Just(LowerMinMax::Min);
        case LowerCmp::gt:  return Just(LowerMinMax::Max);

        // The signed pair, which a float lane can never state - `signedOperand` answers a lane's
        // signedness and a float lane is neither - and the non-strict pair, which is exact for an
        // integer lane and not for a float one. See the note above.
        case LowerCmp::ilt: return isFloat ? Nothing() : Just(LowerMinMax::IMin);
        case LowerCmp::igt: return isFloat ? Nothing() : Just(LowerMinMax::IMax);
        case LowerCmp::le:  return isFloat ? Nothing() : Just(LowerMinMax::Min);
        case LowerCmp::ge:  return isFloat ? Nothing() : Just(LowerMinMax::Max);
        case LowerCmp::ile: return isFloat ? Nothing() : Just(LowerMinMax::IMin);
        case LowerCmp::ige: return isFloat ? Nothing() : Just(LowerMinMax::IMax);
        default:            return Nothing();
    }
}

// Answers the minimum or maximum this select performs, with `lhs` and `rhs` set to the operands in
// the order the machine reads them - or nothing where this select is not one.
static Maybe<LowerMinMax> matchPackedMinMax(LowerBase base, LowerInstSelect* select,
                                            LowerValue*& lhs, LowerValue*& rhs) {
    auto type = select->result.type;
    if(!packedMinMaxSupported(type)) return Nothing();

    auto condition = base[select->cmp];
    if(condition->inst()->kind != LowerInst::Cmp) return Nothing();

    // The comparison has to die with the select, or this replaces two instructions with two and
    // leaves the mask being computed for one reader that no longer wants it.
    if(condition->uses.size() != 1) return Nothing();

    auto cmp = (LowerInstCmp*)condition->inst();
    auto a = base[cmp->lhs];
    auto b = base[cmp->rhs];
    // `lhs` is the value taken where the mask is set and `rhs` the other, which is the order both
    // the machine form and the encoder read a select in.
    auto whenTrue = base[select->lhs];
    auto whenFalse = base[select->rhs];
    auto relation = cmp->getCmp();

    if(whenTrue == a && whenFalse == b) {
        lhs = a;
        rhs = b;
    } else if(whenTrue == b && whenFalse == a) {
        // The mirror: `a < b ? b : a` is `max(b, a)`, which is this relation read from the other
        // side with the operands in the order the select already names them.
        lhs = b;
        rhs = a;
        relation = mirroredCmp(relation);
    } else {
        return Nothing();
    }

    return minMaxForRelation(relation, isFloatVector(type));
}

static void selectPackedMinMax(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // The comparisons this leaves with no readers, cleared after the walk rather than during it:
        // one of them stands immediately *above* the select being rewritten, and removing it there
        // would renumber the instructions this loop is indexing.
        InstChain dead;

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Select) continue;

            auto select = (LowerInstSelect*)inst;
            LowerValue* lhs = nullptr;
            LowerValue* rhs = nullptr;

            auto kind = matchPackedMinMax(base, select, lhs, rhs);
            if(!kind) continue;

            auto comparison = base[select->cmp]->inst();
            auto minMax = new (fun.arena) LowerInstX86MinMax(
                select->result.name, select->result.type, lhs - base, rhs - base, kind.unwrap()
            );

            insertInstAt(base, block, i, minMax);
            replaceAllUses(base, &select->result, &minMax->result);
            removeInst(base, select);

            // The comparison, whose one use has just gone. It has to go too: there is no dead-code
            // elimination below this point, so an instruction nothing reads is one that gets emitted.
            dead.push(comparison);
        }

        removeDeadChain(base, dead);
    }
}

/*
 * Addressing modes.
 *
 * x86 computes `base + index*{1,2,4,8} + disp32` as part of a memory access and charges nothing for
 * it. The lowering has no notion of that - it produces the arithmetic as ordinary instructions - so
 * the shape is recognized here and collapsed into the `X86Address` the encoder already knows how to
 * embed into a load or a store.
 *
 * An X86Address emits no code and occupies no register of its own. It is placed immediately in front
 * of the access that reads it, and genLoad/genStore fold it into their ModRM byte. The adjacency is
 * required rather than incidental: the address's base and index are live *into* the access, so
 * anything that came between the two could overwrite them.
 *
 * One thing does come between them, and it was a live miscompile - the access's own operand copies.
 * Legalization hands scratch registers out per instruction, so an operand of the access could be
 * given the register the base had just been materialized into, and `xs[0] = 1` became a store
 * through the number 1. `foldedAddressRegs` in legalize.cpp is the rule that keeps this true, and
 * `checkFoldedAddress` in verify.cpp is the assertion that says so in a debug build - it was missing
 * because the rest of that file checks *operands*, and a folded address is not one.
 *
 * A chain is only taken apart when every instruction in it exists solely to compute this address.
 * Folding half of one would leave the arithmetic behind *and* repeat it inside the address, so the
 * test is "every use is an address operand" at the top of the chain and "this is the only use"
 * further in. The top may legitimately have several users - a pointer read and then written, an
 * array element used twice - and each of them gets an address instruction of its own.
 */

// `base + index*scale + displacement`, with either register absent: x86 encodes a bare displacement,
// a base alone, an index alone (the no-base SIB form) and both together.
struct AddressPattern {
    LowerValue* base = nullptr;
    LowerValue* index = nullptr;
    U8 scale = 1;
    I64 displacement = 0;
};

// Whether `user` reads `v` as the address of a memory access and nowhere else. *Which* operand that
// is comes from the opcode rather than from a list of instruction kinds here - a load, a store and a
// cache-control intrinsic all name one - and an instruction whose opcode names none reads no address
// at all.
//
// An X86Address can only occupy that one position, so `store %p, %p` reads the same value once as an
// address and once as a value, and rewriting only the first would leave the second pointing at an
// instruction about to be removed.
static bool isAddressOperand(LowerBase base, LowerInst* user, LowerValue* v) {
    auto index = opcodeAddressOperand(opcodeFor(base, user));
    if(index < 0) return false;

    auto used = user->used();
    if(base[used[index]] != v) return false;

    for(Size i = 0; i < used.size(); i++) {
        if(I32(i) != index && base[used[i]] == v) return false;
    }

    return true;
}

static bool isOnlyUsedAsAddress(LowerBase base, LowerValue* v) {
    if(v->uses.isEmpty()) return false;

    for(auto u: v->uses.contents(base)) {
        if(!isAddressOperand(base, base[u], v)) return false;
    }

    return true;
}

// Whether `inst` is the one and only thing that reads `v`, and so whether folding `v` away leaves
// nothing behind. This is the test at every level of the chain below the top one.
static bool isOnlyUse(LowerBase base, LowerValue* v, LowerInst* inst) {
    return v->uses.size() == 1 && base[v->uses.get(base, 0)] == inst;
}

// The signed displacement `v` contributes, if it is an immediate small enough to be one. x86 sign-
// extends an address displacement from 32 bits, so the range it can hold is exactly a four-byte
// immediate's - and whether the immediate was made implicit is irrelevant, since the value is read
// here rather than encoded from a register.
static Maybe<I64> addressDisplacement(LowerValue* v) {
    if(v->inst()->kind != LowerInst::Imm) return Nothing();

    auto imm = immValue(v);
    if(!fitsImmediate(ImmediateWidth::Imm32, imm)) return Nothing();

    return Just(I64(I32(U32(imm))));
}

// Whether every use of `v` is address arithmetic this fold is going to take apart, so that `v` is
// dead once the last of them has been rewritten even though no single one of them is its only use.
//
// This is what lets one shift serve as the scaled index of several addresses. The rule further in is
// "this is the only use", because folding a computation something else still reads would perform it
// twice; but a shift whose *every* reader is an address performs it zero times once they have all
// been rewritten, and the readers need not be the same instruction for that to hold.
//
// Deliberately narrow, since the cost of being wrong is a live range extended for nothing: each user
// has to be a pointer `add` - the one shape the peel below absorbs an index into - reading `v` once,
// and its result has to be an address and nothing else. Anything longer, and the chain above it might
// stop for a reason of its own and leave `v` materialized after all.
static bool isOnlyUsedAsScaledIndex(LowerBase base, LowerValue* v) {
    for(auto u: v->uses.contents(base)) {
        auto user = base[u];
        if(user->kind != LowerInst::Add) return false;

        auto binary = (LowerInstBinary*)user;
        auto lhs = base[binary->lhs];
        auto rhs = base[binary->rhs];

        // `add %o, %o` reads it in both positions, and an address has one index.
        if(lhs == rhs) return false;
        if(!isPtr(binary->result.type)) return false;
        if(!isOnlyUsedAsAddress(base, &binary->result)) return false;
    }

    return !v->uses.isEmpty();
}

// Matches `v` against `index * {1,2,4,8}`, the only scaling the SIB byte can encode. `exclusive` says
// whether this fold is what makes `v` dead - false when it is shared between several addresses, in
// which case the last of them to be folded is the one that removes it.
//
// Only a 64-bit multiply qualifies. A 32-bit `shl %i, 2` wraps at 32 bits and the address unit does
// not, so folding one would change what an index near the top of its range produces. A plain
// unscaled index is not subject to that: it reaches the address in the same register the 64-bit add
// would have read it from, whatever its declared width.
static bool matchScaled(LowerBase base, LowerValue* v, LowerInst* user, LowerValue*& index, U8& scale, bool& exclusive) {
    if(!is64Bit(v->type)) return false;

    auto inst = v->inst();
    if(!isBinary(inst)) return false;

    auto binary = (LowerInstBinary*)inst;
    auto factorValue = base[binary->rhs];
    if(factorValue->inst()->kind != LowerInst::Imm) return false;

    auto imm = ((LowerImm*)factorValue->inst())->i;
    U64 factor;

    if(inst->kind == LowerInst::Shl) {
        if(imm > 3) return false;
        factor = U64(1) << imm;
    } else if(inst->kind == LowerInst::Mul || inst->kind == LowerInst::IMul) {
        factor = imm;
        if(factor != 1 && factor != 2 && factor != 4 && factor != 8) return false;
    } else {
        return false;
    }

    auto source = base[binary->lhs];
    if(isImplicit(source)) return false;

    // Last, since it is the only test here that walks a list: the shape has to be one the SIB byte
    // can hold before it is worth asking who else reads it.
    auto onlyUse = isOnlyUse(base, v, user);
    if(!onlyUse && !isOnlyUsedAsScaledIndex(base, v)) return false;

    index = source;
    scale = U8(factor);
    exclusive = onlyUse;
    return true;
}

// Peels `base + index*scale + displacement` off `address`, stopping as soon as what is left is not
// exclusively this address's own arithmetic. `folded` collects the instructions that become dead, in
// the order they can be removed: an outer add before the shift it absorbed, so that each is already
// unused by the time it goes.
//
// The caller decides what the peeled pattern becomes. An address every user reads as an address
// becomes an X86Address folded into each of them; anything else becomes an X86Lea that computes it
// into a register - see foldLeas.
static bool peelAddress(LowerBase base, LowerValue* address, AddressPattern& out, SmallArray<LowerInst*, 8>& folded) {
    out.base = address;

    // The folded instruction that reads whatever `out.base` ended up being, which is what the
    // index-only step below needs in order to prove that nothing else reads it. Null while nothing
    // has been peeled at all, since then the base is the address itself and its readers are the
    // caller's business.
    LowerInst* baseUser = nullptr;

    // Loop invariant: everything reading `out.base` is about to be rewritten to read the address
    // instead, so the instruction computing it can be removed.
    for(;;) {
        auto v = out.base;
        auto inst = v->inst();

        // Pointer arithmetic only. A 32-bit add wraps where the address unit does not, and the
        // lowering states the width in the result type of the operation itself.
        if(!isPtr(v->type)) break;
        if(inst->kind != LowerInst::Add && inst->kind != LowerInst::Sub) break;

        auto binary = (LowerInstBinary*)inst;
        auto lhs = base[binary->lhs];
        auto rhs = base[binary->rhs];

        // Decided in full before anything is committed, so that a step that turns out not to fit
        // leaves the pattern as the previous one left it.
        LowerValue* next = nullptr;
        LowerValue* index = out.index;
        U8 scale = out.scale;
        auto displacement = out.displacement;
        LowerInst* scaled = nullptr;

        if(auto d = addressDisplacement(rhs)) {
            displacement += inst->kind == LowerInst::Sub ? -d.unwrap() : d.unwrap();
            if(displacement >= I64(minLimit<I32>) && displacement <= I64(maxLimit<I32>)) next = lhs;
        } else if(inst->kind == LowerInst::Add && !out.index) {
            // Add is commutative and the immediate peephole has already run, so either side may be
            // the one carrying the index.
            //
            // A shift shared between several addresses is taken apart by each of them but removed
            // only by the last, so `scaled` stays null for all but that one - the value is still
            // read, and the instruction has to stay until it is not.
            bool exclusive = false;

            if(matchScaled(base, rhs, inst, index, scale, exclusive)) {
                if(exclusive) scaled = rhs->inst();
                next = lhs;
            } else if(matchScaled(base, lhs, inst, index, scale, exclusive)) {
                if(exclusive) scaled = lhs->inst();
                next = rhs;
            } else if(!isImplicit(rhs)) {
                index = rhs;
                scale = 1;
                next = lhs;
            }
        }

        // The base has to reach the address in a register of its own; an operand that was folded
        // into some other instruction's encoding has none.
        if(!next || isImplicit(next)) break;

        out.index = index;
        out.scale = scale;
        out.displacement = displacement;
        out.base = next;
        baseUser = inst;

        folded.push(inst);
        if(scaled) folded.push(scaled);

        // Anything else reading what is left stops the chain here: that value stays materialized,
        // and folding further would compute it twice rather than once.
        if(!isOnlyUse(base, next, inst)) break;
    }

    // `[index*scale + disp32]` with no base at all is a legal SIB form, and it is what a scaled index
    // with nothing left to add it to becomes: what the loop above stopped on is the multiply, which
    // the addressing unit does for free but which would otherwise stay an instruction whose result
    // the address reads as an unscaled base. This is the shape an absolute address indexed at run
    // time takes - the offset is the displacement, and there is no pointer to add it to.
    //
    // Only worth it for a real scaling. At scale 1 the index register is the register the base would
    // have been, so nothing is saved - and `[reg]` would become a SIB byte plus a four-byte
    // displacement for the privilege.
    if(!out.index && baseUser) {
        auto candidate = out.base;
        auto user = baseUser;
        LowerInst* bitcast = nullptr;

        // A bitcast is what the lowering has to write to use a computed integer as an address, and
        // between two 64-bit classes it computes nothing: the value and its cast are the same bits in
        // the same register. So the scaled index behind one is still a scaled index, and taking it as
        // the address's index removes the cast along with the multiply.
        //
        // Only looked through here, and not for a base: a base reaches the access in a register
        // either way, so seeing through the cast would change which register that is for no gain.
        if(candidate->inst()->kind == LowerInst::Bitcast && isOnlyUse(base, candidate, user)) {
            auto source = base[((LowerInstUnary*)candidate->inst())->from];

            if(is64Bit(source->type) && is64Bit(candidate->type)) {
                bitcast = candidate->inst();
                user = bitcast;
                candidate = source;
            }
        }

        LowerValue* index = nullptr;
        U8 scale = 1;
        bool exclusive = false;

        // matchScaled proves what this needs: a 64-bit multiply or shift by an encodable factor, read
        // by nothing but the instruction that is about to be folded away - or by nothing but other
        // addresses, in which case it stays until the last of them has been rewritten.
        if(matchScaled(base, candidate, user, index, scale, exclusive) && scale != 1) {
            // Outermost first, so that each is already unused by the time it goes.
            if(bitcast) folded.push(bitcast);
            if(exclusive) folded.push(candidate->inst());

            out.index = index;
            out.scale = scale;
            out.base = nullptr;
        }
    }

    return folded.isNotEmpty();
}

static bool matchAddress(LowerBase base, LowerValue* address, AddressPattern& out, SmallArray<LowerInst*, 8>& folded) {
    if(!isOnlyUsedAsAddress(base, address)) return false;

    return peelAddress(base, address, out, folded);
}

// Where `inst` sits in its own block's instruction list.
static Size indexOfInst(LowerBase base, LowerBlock* block, LowerInst* inst) {
    auto list = block->instructions.contents(base);

    for(Size i = 0; i < list.size(); i++) {
        if(base[list[i]] == inst) return i;
    }

    assertTrue("instruction is not in its own block" == nullptr);
    return 0;
}

static void foldAddresses(LowerBase base, LowerFunction& fun) {
    auto& arena = fun.arena;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];

            // Every instruction that references memory, which is the ones whose opcode names an
            // address operand - not a list of kinds. The operand is already an X86Address when an
            // earlier access on the same chain folded it for every user at once.
            auto operand = opcodeAddressOperand(opcodeFor(base, inst));
            if(operand < 0) continue;

            auto address = base[inst->used()[operand]];
            if(isMem(address)) continue;

            AddressPattern pattern;
            SmallArray<LowerInst*, 8> folded;
            if(!matchAddress(base, address, pattern, folded)) continue;

            // Snapshotted: the loop below rewrites the very list it is reading.
            SmallArray<LowerInst*, 8> users;
            for(auto u: address->uses.contents(base)) users.push(base[u]);

            for(auto user: users) {
                auto computed = new (arena) LowerInstX86Address(
                    LowerInst::X86Address, StringId(),
                    pattern.base ? pattern.base - base : nullptr,
                    pattern.index ? pattern.index - base : nullptr,
                    pattern.scale, U32(I32(pattern.displacement))
                );

                auto userBlock = base[user->block];
                insertInstAt(base, userBlock, indexOfInst(base, userBlock, user), computed);

                // Each user's own address operand, which matchAddress already established every one
                // of them has - the users of one folded chain need not all be the same instruction.
                replaceUse(base, address, user, &computed->result);
                user->used()[opcodeAddressOperand(opcodeFor(base, user))] = &computed->result - base;
            }

            for(auto dead: folded) removeInst(base, dead);

            // Both the insertions and the removals moved things around underneath the walk, so the
            // position to carry on from is wherever this access ended up.
            i = indexOfInst(base, block, inst);
        }
    }
}

/*
 * `lea`.
 *
 * The fold above only fires for an address computation every user reads *as an address*, because
 * that is the case where the arithmetic disappears entirely. An address that has to end up in a
 * register - pointer arithmetic passed to a call, an element pointer written to memory, a base kept
 * across a branch - still gets the same addressing unit, just with the answer materialized: that is
 * what `lea` is.
 *
 * `lea` is worth reaching for in exactly two shapes, and neither is "every pointer add". It computes
 * `base + index*{1,2,4,8} + disp` in one instruction where the lowering emitted two or three, and it
 * writes its result somewhere other than its operands, where `add` overwrites the first of them and
 * so needs a copy in front of it whenever that operand is still read afterwards. Where neither
 * applies, `add` is one instruction of the same length and is left alone.
 */

// Whether replacing this chain with an `lea` costs fewer instructions than leaving it alone.
//
// The base's use list still counts the instruction about to be folded away, so "used more than once"
// is what "read somewhere else as well, and therefore copied before an `add` could overwrite it"
// looks like from here.
static bool isLeaProfitable(const AddressPattern& pattern, const SmallArray<LowerInst*, 8>& folded) {
    if(folded.size() > 1) return true;

    // An index-only address folded the multiply that produced it, so there is nothing left for an
    // `add` to have been - and no base whose use count could say anything either way.
    if(!pattern.base) return true;

    return pattern.base->uses.size() > 1;
}

static void foldLeas(LowerBase base, LowerFunction& fun) {
    auto& arena = fun.arena;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // Backwards, so that the top of a chain is reached before the arithmetic feeding it. The
        // other way round, `p + i*4` would become an `lea` of its own and leave the `+ 24` above it
        // behind as a second instruction, where taking the outer add first absorbs both.
        Size i = block->instructions.size();

        while(i > 0) {
            i--;

            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Add && inst->kind != LowerInst::Sub) continue;

            // Pointer arithmetic only, for the reason the fold above gives: the address unit works
            // at 64 bits and does not wrap where a narrower operation does.
            auto& result = ((LowerInstBinary*)inst)->result;
            if(!isPtr(result.type) || isImplicit(&result) || result.uses.isEmpty()) continue;

            AddressPattern pattern;
            SmallArray<LowerInst*, 8> folded;
            if(!peelAddress(base, &result, pattern, folded)) continue;
            if(!isLeaProfitable(pattern, folded)) continue;

            auto lea = new (arena) LowerInstX86Address(
                LowerInst::X86Lea, result.name,
                pattern.base ? pattern.base - base : nullptr,
                pattern.index ? pattern.index - base : nullptr,
                pattern.scale, U32(I32(pattern.displacement))
            );

            // In front of the instruction it replaces, which is where the value was already
            // available to everything that reads it.
            insertInstAt(base, block, i, lea);

            replaceAllUses(base, &result, &lea->result);
            for(auto dead: folded) removeInst(base, dead);

            // Both the insertion and the removals moved things around underneath the walk, so the
            // position to carry on from is wherever the new instruction ended up. Everything the
            // fold consumed was at or before it, and the `lea` itself is not a candidate.
            i = indexOfInst(base, block, lea);
        }
    }
}

/*
 * Folding a load into the instruction that consumes it.
 *
 * Most of the AMD64 ALU reads one operand straight out of memory, and §5.5 already takes that for a
 * *frame slot*. What it does not take is a load the program wrote: `mov rax, [rdi]` followed by
 * `add rcx, rax` is two instructions where `add rcx, [rdi]` is one, at the same length in bytes.
 * The form that reads it there is the memory-source twin (MachineForm::memorySource); this is what
 * moves an instruction onto one. §5 of test/bench/findings.md is the measurement.
 *
 * What the fold rewrites is one operand. The consumer stops reading the load's result and reads the
 * *address* instead, and the load is removed - so what reaches allocation is an instruction shaped
 * exactly like a load: an `address()` operand holding an X86Address placed immediately above it.
 * Nothing in placement, legalization, emission or the verifiers learns a case for it; each asks the
 * selected form which operand is an address and gets an answer it already knew what to do with.
 *
 * That the operand holds an X86Address is also the whole record of the fold. It is the one value
 * that can only ever be an address, so selection reads the decision back off the operand list rather
 * than off a flag that would have to be kept in step with it - which is why a load whose pointer
 * arrived in a register is given one here as well. `[reg]` is an addressing mode like any other and
 * costs nothing to say.
 *
 * Three things bound it, and each of the three is load-bearing:
 *
 *  - **Nothing between the load and its reader may write memory.** Where the two are adjacent that is
 *    free, and it is all this asked for at first. Lifting it is §3.1.2's sink: the load is moved down
 *    to its reader rather than the reader moved up, so what has to hold is the same thing
 *    `foldStoreUpdates` asks of the same stretch - `mayWriteMemory` over every instruction between,
 *    with a call not on the whitelist. The load's *address* travels with it, since an X86Address has
 *    to stand immediately in front of whatever dereferences it, and that is what needs the address to
 *    have no other reader: one left behind would be reading a value defined below it.
 *
 *    The distance is bounded (kMaxLoadSinkDistance) because the search walks up from the reader, so
 *    an unbounded one would make the pass quadratic in a block that folds nothing.
 *  - **The encoding reads exactly the bytes the load read.** A narrow load extends into its result,
 *    which an operand of an ALU instruction has no room to do, and an access at any other width
 *    would read a neighbouring value. This is the rule directMemoryOperands applies to a frame slot,
 *    asked of an address instead.
 *  - **Nothing may be copied into a fixed register in front of the instruction.** The address's own
 *    base and index are live *into* the consumer - they belong to the X86Address one above it - so a
 *    copy emitted in front could overwrite one. The destructive copy is covered, by
 *    collectTieConflicts in place.cpp; a fixed-register operand is not, which is what keeps the
 *    group-3 `mul` and `div` shapes out of this.
 */

/*
 * Whether running this instruction can write memory.
 *
 * A whitelist of the kinds that provably cannot, on the terms `touchesMemory` in lower_forward.cpp
 * states: a kind added later is answered "yes" and costs a rewrite rather than correctness. It is
 * the *write* half of that question rather than the whole of it, which is what lets a load stand
 * between the two accesses being fused - and one always does, `b[k]` being read in the same
 * expression.
 */
static bool mayWriteMemory(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Arg:
        case LowerInst::Global:
        case LowerInst::Fun:
        case LowerInst::Imm:
        case LowerInst::Nop:
        case LowerInst::Select:
        case LowerInst::Alloca:
        case LowerInst::Phi:
        case LowerInst::VecSplat:
        case LowerInst::VecLane:
        case LowerInst::VecWithLane:
        case LowerInst::VecShuffle:
        case LowerInst::VecReduce:
        case LowerInst::Load:
        case LowerInst::X86Address:
        case LowerInst::X86Lea:
        case LowerInst::X86MinMax:
        case LowerInst::X86MaskAnd:
        case LowerInst::X86Permute:

        // `Fma` is neither unary nor binary, so the fallback would answer *yes* for it - which is
        // the rule this is written to and is wrong here, an FMA being arithmetic like any other.
        case LowerInst::Fma:
            return false;
        default:
            return !isUnary(inst) && !isBinary(inst) && !isCast(inst);
    }
}

/*
 * How far above its reader a load may stand and still be folded into it.
 *
 * The search walks up from the reader, so this is what keeps the pass linear in the block rather
 * than quadratic over one that folds nothing. Sixteen is well past what the shapes this catches
 * need: what separates a load from its reader is the *other* operand's computation, which for an
 * array element is an index cast, a shift and an add.
 */
static constexpr Size kMaxLoadSinkDistance = 16;

// Whether exchanging this operation's operands leaves it computing the same thing. The same set
// trySwapOperands uses, and restricted to the integer bank for the same reason: a float addition is
// commutative in value but not in which NaN payload the machine propagates.
static bool isCommutativeInt(LowerInst* inst) {
    /*
     * The packed minimum and maximum, at an integer lane and not at a float one.
     *
     * `min(a, b)` and `min(b, a)` hold the same lanes for integers, so the operand the load feeds
     * may be moved into the position the encoding dereferences - which is the whole of what turns
     * `vmovdqu ; vpmaxsd` into one instruction. At a float lane the order is *the answer* for a NaN
     * and for a pair of zeros of opposite sign (see LowerInst::X86MinMax), so exchanging it there
     * would be a different operation wearing the same name.
     */
    if(inst->kind == LowerInst::X86MinMax) return isIntVector(((LowerInstX86MinMax*)inst)->result.type);

    /*
     * A masked vector, where the mask is the arm that is *kept*.
     *
     * That one is `pand`/`andps`, which is commutative for the reason the bitwise three below are:
     * what it does is to bits. So the operand a load feeds may be moved into the position the
     * encoding dereferences, which is what turns `vmovups (%rdx),%ymm3 ; vandps` into one
     * instruction in a masked loop. The *complemented* one is `pandn`, which computes `~lhs & rhs`
     * and means two different things read the two ways round.
     */
    if(inst->kind == LowerInst::X86MaskAnd) return !((LowerInstX86MaskAnd*)inst)->isComplemented();

    /*
     * An equality, at every type it can be asked about.
     *
     * `a == b` is `b == a` whatever the operands are - the one relation for which that is true of a
     * float and of a NaN as well, both orders answering false - so the load may be exchanged into
     * the memory-capable position here too. That is what the AVX2 string and integer loops needed:
     * `vpcmpeqb` reads its second operand out of memory quite happily, and the comparison arrived
     * with the load on the *left*, so a separate `vmovdqu` was emitted in front of every one.
     *
     * `neq` rides along for the same reason. A packed one is the equality inverted rather than an
     * instruction of its own, and what is inverted is a mask that does not care which side is which.
     */
    if(inst->kind == LowerInst::Cmp) {
        auto relation = ((LowerInstCmp*)inst)->getCmp();
        return relation == LowerCmp::eq || relation == LowerCmp::neq;
    }

    if(!isBinary(inst)) return false;
    auto type = ((LowerInstBinary*)inst)->result.type;

    switch(inst->kind) {
        /*
         * The bitwise three, at every type whose bits they are - a vector and a mask included, and
         * a *float* vector included, which is the one that matters here: an absolute value is an
         * `and` against a sign mask over `f32x8`, and with the operands as written the mask stands
         * where the encoding's address goes. Exchanging is what puts the value being measured there
         * instead, so the loop's own load folds and the loop-invariant mask keeps its register.
         *
         * A float `and` is commutative in the way that matters here, and in a way a float `add` is
         * not: what these do is to bits, so there is no rounding and no NaN payload to be taken from
         * one side rather than the other.
         */
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
            return isIntLike(type) || isVectorLike(type);

        // The arithmetic, at integer lanes alone. A float add and a float multiply are exchangeable
        // in value, and this backend does not exchange them: `addps` takes the payload of a NaN from
        // its destination, so which operand is which is visible in a way it is not for the three
        // above.
        case LowerInst::Add:
        case LowerInst::Mul:
        case LowerInst::IMul:
            return isIntLike(type) || isIntVector(type);

        default:
            return false;
    }
}

// Whether this form requires an operand in a particular register, which is the copy a folded address
// cannot survive - see the third bound above.
static bool hasFixedOperands(const MachineForm& form) {
    for(auto& constraint: form.uses) {
        if(constraint.kind == OperandConstraintKind::FixedRegister) return true;
    }

    return false;
}

/*
 * Folds the load feeding operand `at` of the instruction at `index` into it. Answers where that
 * instruction ended up, or Nothing where it was left alone - in which case nothing has been changed
 * at all: the operand exchange a commutative operation may need is made at the end, with every
 * question already answered, so that a fold which does not happen leaves no trace of having been
 * considered. That is what lets the caller try one operand and then the other.
 */
static Maybe<Size> tryFoldLoadOperand(LowerBase base, LowerFunction& fun, LowerBlock* block,
    Size index, const MachineForm& twin, Size memory, Size at)
{
    auto inst = base[block->instructions.get(base, index)];
    auto used = inst->used();
    auto value = base[used[at]];
    auto load = (LowerInstLoad*)value->inst();

    /*
     * Where the load stands, and whether the stretch between it and here leaves memory alone.
     *
     * One walk answers both: upwards from the reader to the load, refusing at the first instruction
     * that could change what the load would read. A load in another block, or further up than the
     * bound, is one this declines to look for.
     */
    if(base[load->block] != block) return Nothing();

    auto loadAt = index;
    for(Size steps = 0; ; steps++) {
        if(loadAt == 0 || steps > kMaxLoadSinkDistance) return Nothing();

        loadAt--;
        auto above = base[block->instructions.get(base, loadAt)];
        if(above == (LowerInst*)load) break;
        if(mayWriteMemory(above)) return Nothing();
    }

    // Whether this is a *sink* rather than the adjacent fold §3.1.2 started as, which is the one
    // thing below that has to be decided rather than assumed: a sunk load takes its address down
    // with it, and an address with another reader may not travel.
    auto sunk = loadAt != index - 1;

    // Which operand holds it has to be the one the encoding can dereference, or an operand a
    // commutative operation can exchange into it - which is the shape `arr[i] + sum` arrives in.
    auto exchange = at != memory;
    if(exchange && !(isCommutativeInt(inst) && used.size() == 2 && at < used.size())) return Nothing();

    /*
     * An operand the encoding was carrying as an immediate has nowhere to go.
     *
     * The memory twin names a register in the field the constant occupied, so the fold displaces it
     * into one - and materializing a constant is exactly the instruction the fold removed, at more
     * bytes. `%v = load %p ; add %v, 1` is `mov (%rdx),%edx ; inc %edx` and folds to
     * `mov $1,%ecx ; add (%rdx),%ecx`: the same two instructions, three bytes longer, and the
     * constant now holds a register across them.
     *
     * Unreachable while the load had to be adjacent - the `imm` that defines the constant stands
     * between the two - so this is a rule the sink needs and the adjacent fold never met.
     *
     * Asked of the twin rather than of the operand, because a great many twins carry both at once:
     * `cmp [m], $0` is one instruction and folding a load into it displaces nothing. Only a twin with
     * no immediate field at all has nowhere to put one. The operand's *kind* rather than `isImm`,
     * which reads a flag no pass has set this early - embedding an immediate is a decision
     * `selectMachineInstructions` makes below here.
     */
    if(twin.immediateWidth() == ImmediateWidth::None) {
        for(Size i = 0; i < used.size(); i++) {
            if(i != at && base[used[i]]->inst()->kind == LowerInst::Imm) return Nothing();
        }
    }

    // The bytes the encoding reads are the bytes the load read, unextended.
    if(load->getWidth() != accessWidthOf(value->type)) return Nothing();
    if(stackSlotClassFor(value->type) != stackSlotClassFor(operationType(base, twin, inst))) return Nothing();

    auto address = base[load->from];

    /*
     * A pooled constant, which becomes the *whole* address rather than something to build one from.
     *
     * This is the case the rip-relative form of `LowerInstX86Address` exists for. It is checked
     * before the two below because the answers differ: the address of a pooled constant is neither
     * a folded `X86Address` sitting two instructions up nor a pointer in a register, and left to
     * the general path the global would be committed to a register with a `lea` in front of it -
     * strictly worse than the load being folded.
     *
     * Any global nothing writes, not only a pooled constant: `mut` clear is a promise, derived from
     * `Global::isWritten` for a real program and written as `mut @g` in a `.lower` fixture. A global
     * that is written is left to the general path, where nothing is assumed about it.
     */
    auto pooledSymbol = LowerPtr<LowerGlobal>(nullptr);
    if(address->inst()->kind == LowerInst::Global) {
        auto target = ((LowerInstGlobal*)address->inst())->target;
        if(base[target]->mut) return Nothing();

        // The load is about to be the only reader gone; anything else reading the address still
        // needs it in a register, and this fold would leave that reader without a definition.
        if(address->uses.size() != 1) return Nothing();

        pooledSymbol = target;
    } else if(isMem(address)) {
        // Where the address fold put it: immediately in front of the load it serves. Checked rather
        // than assumed, an address anywhere else being one whose registers the instructions in
        // between could have written.
        if(loadAt == 0 || base[block->instructions.get(base, loadAt - 1)] != address->inst()) return Nothing();

        // A sunk load takes its address with it, so an address something else reads may not go: the
        // reader left behind would be reading a value defined below it. Where the two are adjacent
        // the address does not move and nothing is asked of its other readers.
        if(sunk && address->uses.size() != 1) return Nothing();
    } else if(isImplicit(address)) {
        // A pointer the encoding swallowed has no register for an address to be built around.
        return Nothing();
    }

    /*
     * Committed from here: everything below changes the function.
     */

    // Through the operand list rather than through `LowerInstBinary`'s two fields, because the kinds
    // this exchanges are no longer all binary: `X86MinMax` has its own struct, and what "exchange the
    // operands" means is the same for every two-operand instruction - the first two used values,
    // which is what the encoder and the form both read positionally.
    if(exchange) {
        auto operands = inst->used();
        auto first = operands[0];

        operands[0] = operands[1];
        operands[1] = first;
    }

    if(pooledSymbol) {
        auto producer = address->inst();
        auto computed = new (fun.arena) LowerInstX86Address(
            LowerInst::X86Address, StringId(), nullptr, nullptr, 1, 0
        );

        computed->symbol = pooledSymbol;

        replaceUse(base, value, inst, &computed->result);
        inst->used()[memory] = &computed->result - base;

        // The load first, because it is the address's last reader and the `global` that produced it
        // has nothing left to produce once it is gone - the symbol is in the encoding now. Both are
        // removed here rather than left to a dead-value sweep, because there is none between this
        // pass and allocation.
        removeInst(base, load);
        removeInst(base, producer);

        // Both removals were above the consumer and the address goes back immediately in front of
        // it, so where everything ended up is asked rather than counted: the `global` need not have
        // been adjacent to the load it fed.
        auto here = indexOfInst(base, block, inst);
        insertInstAt(base, block, here, computed);

        return Just(here + 1);
    }

    // The load goes first, wherever it stood: it is what the address was in front of, and taking it
    // out is what lets the address end up in front of the consumer instead.
    auto computedHere = !isMem(address);

    if(computedHere) {
        // A pointer that reached the load in a register becomes `[reg]`, so that the operand says
        // what it is without a flag beside it.
        address = &(new (fun.arena) LowerInstX86Address(
            LowerInst::X86Address, StringId(), address - base, nullptr, 1, 0
        ))->result;
    }

    replaceUse(base, value, inst, address);
    inst->used()[memory] = address - base;
    removeInst(base, load);

    // And the address is put immediately in front of the consumer. For an adjacent fold that is
    // where it already is - the load having been between them - so only a sink moves anything.
    if(computedHere) {
        insertInstAt(base, block, indexOfInst(base, block, inst), address->inst());
    } else if(sunk) {
        auto producer = address->inst();
        removeInst(base, producer);
        insertInstAt(base, block, indexOfInst(base, block, inst), producer);
    }

    return Just(indexOfInst(base, block, inst));
}

/*
 * Folds a load into the instruction at `index`, whichever of its operands one feeds.
 *
 * The operand the encoding dereferences is offered first and every other one after it, because the
 * two answers are not interchangeable: reaching another operand needs an exchange, which only a
 * commutative operation has, and the load feeding that one may be somewhere this cannot fold from at
 * all. `abs(v[i]) `is where that matters - the `and` reads the element and a mask loaded outside the
 * loop, the mask is in the r/m position, and stopping at the first operand that *looks* like a
 * candidate refused the whole fold and left the element's load in the loop.
 */
static Maybe<Size> tryFoldLoad(LowerBase base, LowerFunction& fun, LowerBlock* block, Size index) {
    if(index == 0) return Nothing();

    auto inst = base[block->instructions.get(base, index)];
    auto& form = machineTarget().form(selectForm(base, inst));

    // Nothing to fold into: either a form with no memory-capable operand, or one already on its
    // twin - a folded operand is one the form reads as an address, and there is one r/m field.
    if(!form.memorySource) return Nothing();

    auto& twin = machineTarget().form(form.memorySource);
    if(hasFixedOperands(twin)) return Nothing();

    auto memory = Size(form.memoryUse());
    auto used = inst->used();

    // A load with one reader, and that reader is this instruction. `add %v, %v` reads it in both
    // positions and only one of them can be the address, which a use count of one already excludes.
    auto candidate = [&](Size i) {
        auto operand = base[used[i]];
        if(operand->inst()->kind != LowerInst::Load) return false;

        return !isImplicit(operand) && operand->uses.size() == 1;
    };

    if(memory < used.size() && candidate(memory)) {
        if(auto folded = tryFoldLoadOperand(base, fun, block, index, twin, memory, memory)) return folded;
    }

    for(Size i = 0; i < used.size(); i++) {
        if(i == memory || !candidate(i)) continue;

        if(auto folded = tryFoldLoadOperand(base, fun, block, index, twin, memory, i)) return folded;
    }

    return Nothing();
}

static void foldLoads(LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            // A fold removes the load above the instruction and may insert an address in its place,
            // so the instruction just examined does not stay where the walk left it. Nothing above
            // it changed shape, so carrying on from wherever it ended up skips nothing.
            if(auto folded = tryFoldLoad(base, fun, block, i)) i = folded.unwrap();
        }
    }
}

/*
 * §45.2 An accumulating write, folded into one memory-destination instruction.
 *
 * `out[i] = out[i] + x` is a load, an operation and a store, and on this machine it is one
 * instruction: the group-1 ALU writes its result back through the very r/m field it read its operand
 * from. What that removes from `Matrix`'s inner loop is two decoded instructions and, more to the
 * point, the *register* the loaded value occupied for the length of the operation - in the innermost
 * loop of every program that accumulates into an array.
 *
 *     mov  r11d, [out + i*4]           imul edx, left
 *     imul r15d, [b + i*4]      =>     add  [out + i*4], edx
 *     add  r11d, r15d
 *     mov  [out + i*4], r11d
 *
 * The rewrite is a *sink*: the load moves down to where the store is and the two become one access.
 * Four things have to hold for that, and each is checked rather than assumed.
 *
 *  - **One address, and it is the same value.** The load and the store name the same SSA pointer,
 *    which is what makes "the same location" a fact rather than an alias question. Two computations
 *    of one address would be that question, and the tier above has already unified them where they
 *    are unifiable.
 *  - **The load feeds the operation and the operation feeds the store, and nothing else reads
 *    either.** Otherwise the load has to be performed anyway and this adds an access rather than
 *    removing one.
 *  - **Nothing between them writes memory.** The load is being moved across whatever lies between
 *    it and the store, so anything that could change what it would read is a refusal.
 *    `mayWriteMemory` is the whitelist, and a call is not on it.
 *  - **The location is the left-hand side of a subtraction.** `[m] - v` is what the machine
 *    performs; `v - [m]` is a different number. The four commutative ones are folded from either
 *    side.
 *
 * The load may sit in the block *above* the store's, which is not an extension of the rule but the
 * shape a bounds-checked subscript has: `out[i]` is checked, and the check is a branch, so the load
 * and the store the program wrote as one line are on two sides of an edge. The condition there is
 * that the store's block has exactly one way in - then every path that reaches the store has just
 * run the tail of the load's block, and the two spans that have to be clean are that tail and the
 * head of the store's own block.
 */

// The five operations the machine can perform through its r/m field, which is the whole of what the
// form table describes - see OpStoreAdd and the block beside it in machine.cpp.
static bool isStoreUpdateOp(LowerInst* inst) {
    // Asked of the form table rather than restated here: which operations have an in-place memory
    // form is one fact, and it is the same one the opcode and the form selection read. See
    // StoreUpdateOp.
    return storeUpdateOpFor(inst->kind) != nullptr;
}

// Whether every instruction in `[from, to)` of this block leaves memory as it found it. The
// terminator is not one of them and is not asked about: a branch writes nothing, and the two spans
// this is asked about are a block's tail and another's head.
static bool spanCannotWrite(LowerBase base, LowerBlock* block, Size from, Size to) {
    auto list = block->instructions.contents(base);

    for(Size i = from; i < to && i < list.size(); i++) {
        if(mayWriteMemory(base[list[i]])) return false;
    }

    return true;
}

// Whether nothing between the load and the store can change what the load would read. The load is
// either in the store's own block, or in the one block the store's block is entered from - see the
// header above for why that second case is the ordinary one rather than the exotic one.
static bool nothingWritesBetween(LowerBase base, LowerBlock* block, Size store, LowerInst* load) {
    auto from = base[load->block];

    if(from == block) {
        auto at = indexOfInst(base, block, load);
        return at < store && spanCannotWrite(base, block, at + 1, store);
    }

    if(block->incoming.size() != 1) return false;
    if(base[block->incoming.get(base, 0)] != from) return false;

    auto at = indexOfInst(base, from, load);
    return spanCannotWrite(base, from, at + 1, from->instructions.size())
        && spanCannotWrite(base, block, 0, store);
}

/*
 * ## What it costs, which is why a loop does not get one
 *
 * Measured, `add [m], r` is *slower* than the three instructions it replaces - on this machine, in
 * every loop it was put in:
 *
 * | loop | split | in place |
 * | --- | --- | --- |
 * | `out[i] += src[i]` through a handle (as the IR writes it) | 3.43 ms | 4.14 ms |
 * | the same with the base already in a register (as LLVM writes it) | 2.68 ms | 3.72 ms |
 * | `out[i] += 1`, where the fold also removes the register | 2.70 ms | 2.95 ms |
 *
 * Those are hand-written assembly loops differing in nothing else (test/bench/findings.md §45.2),
 * and `programs/Matrix.yana` agrees to within a point: 153.9 ms against 166.0 ms, the only program
 * in the corpus the fold reaches. Fewer instructions and more time. The read-modify-write is one
 * instruction whose store cannot leave the store buffer until its own load has returned, and a loop
 * that stores every iteration is limited by exactly that.
 *
 * So the fold is applied where a store is **not in a loop**, which is where the trade it makes is
 * the one it looked like: three decoded instructions and a register become one instruction in code
 * that runs once. That is most of what it reaches anyway - a teardown, a field updated on a path, a
 * counter bumped outside a loop - and it is the whole of the size win with none of the cost.
 */

// Folds the store at `index`, the operation feeding it and the load feeding that into one in-place
// update. Answers where the update ended up, or Nothing - in which case nothing has been changed at
// all, every question being asked before the first rewrite.
static Maybe<Size> tryFoldStoreUpdate(LowerBase base, LowerFunction& fun, LowerBlock* block, Size index) {
    auto inst = base[block->instructions.get(base, index)];
    if(inst->kind != LowerInst::Store) return Nothing();

    auto store = (LowerInstStore*)inst;
    auto stored = base[store->value];
    auto op = stored->inst();

    // The operation, read where the store reads it: in this block, with this store as its one
    // reader. Anything else and the operation stands anyway, and this adds an access to it.
    if(!isStoreUpdateOp(op) || isImplicit(stored)) return Nothing();
    if(stored->uses.size() != 1 || base[op->block] != block) return Nothing();

    auto binary = (LowerInstBinary*)op;
    if(!isIntLike(binary->result.type)) return Nothing();

    auto lhs = base[binary->lhs];
    auto rhs = base[binary->rhs];

    // Which side the location is. A subtraction has one answer and the other four have two, and
    // `lhs == rhs` is `x + x` - one value read twice, which is not an update of anything.
    if(lhs == rhs) return Nothing();

    auto from = lhs;
    auto with = rhs;

    if(lhs->inst()->kind != LowerInst::Load) {
        if(op->kind == LowerInst::Sub) return Nothing();

        from = rhs;
        with = lhs;
    }

    if(from->inst()->kind != LowerInst::Load || isImplicit(from)) return Nothing();
    if(from->uses.size() != 1) return Nothing();

    auto load = (LowerInstLoad*)from->inst();

    // The same location, and the same bytes of it. An overread is a load of more than it says and
    // has no in-place operation to be: what would be written back is the width, and what was read
    // is not.
    if(base[load->from] != base[store->to]) return Nothing();
    if(load->getWidth() != store->getWidth() || load->isOverread()) return Nothing();

    /*
     * A constant right-hand side at a width the immediate forms do not cover.
     *
     * The two of them are the group-1 `imm8`/`imm32` pair and sit at four bytes and eight, which is
     * where that pair is; the byte and word updates are register-only. `canEmbedImm` asks the
     * *opcode* whether a constant can be embedded and would be answered yes by those two forms, so a
     * narrow update of a constant would have its operand taken out of allocation and then selected
     * into a form with nowhere to read it. Refused here rather than answered there, since the
     * question that pass asks has one answer per opcode.
     */
    if(with->inst()->kind == LowerInst::Imm && store->getWidth() < 4) return Nothing();

    if(!nothingWritesBetween(base, block, index, load)) return Nothing();

    /*
     * Committed: everything below changes the function.
     */
    auto update = new (fun.arena) LowerInstX86StoreOp(
        store->to, with - base, store->getWidth(), op->kind
    );

    insertInstAt(base, block, index, update);

    // In this order: the store is the operation's only reader and the operation is the load's, so
    // each is dead only once the one below it has gone. There is no dead-value sweep between here
    // and allocation - an instruction nothing reads is an instruction that gets emitted.
    removeInst(base, store);
    removeInst(base, op);
    removeInst(base, load);

    return Just(indexOfInst(base, block, update));
}

static void foldStoreUpdates(LowerBase base, LowerFunction& fun) {
    // Which blocks are in a loop, which is the whole of what decides where this fires - see the
    // table above. Built once for the function; nothing here creates or renumbers a block, so the
    // indexes it is read by stay the ones it was built from.
    auto loops = fun.buildLoops(base);

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];
        if(loops.depth[block->index] > 0) continue;

        for(Size i = 0; i < block->instructions.size(); i++) {
            // The fold removes two instructions that may both be above the store, so where the
            // update ended up is asked rather than counted. Everything it consumed was at or before
            // that point, and the update itself is not a candidate.
            if(auto folded = tryFoldStoreUpdate(base, fun, block, i)) i = folded.unwrap();
        }
    }
}

/*
 * Loop rotation.
 *
 * A loop written with its test at the top costs two branches an iteration: the conditional one that
 * decides whether to run the body, and the unconditional one that goes back for the next iteration.
 * Only one of the two can ever be a fallthrough, whatever order the blocks are put in - which is why
 * this is a transform rather than a further refinement of §3.2.
 *
 *   head:  cmp rax,rsi        =>    pre:   cmp rax,rsi
 *          jge exit                        jge exit
 *          ...body...               body:  ...body...
 *          jmp head                 head:  cmp rax,rsi
 *   exit:                                  jl  body
 *                                   exit:
 *
 * What moves is not the test but the *entry*: the preheader stops jumping into the header and asks
 * the header's own question instead, so the header is left reachable only from the latch and becomes
 * the bottom of the loop. The body is then the block the loop is entered through, the header is the
 * block it is left from, and the two branches an iteration used to pay are one.
 *
 * The test is therefore evaluated in two places, and both of them run exactly when the single copy
 * used to: the preheader's copy is the first iteration's test, and the header's copy is every later
 * one. Nothing is speculated and nothing runs an extra time, which is why a load in the header is as
 * duplicable as a compare - the limit below is about code size and nothing else.
 *
 * ## What it costs in bytes
 *
 * Nothing, to within an instruction. The preheader's `jmp` and the latch's `jmp` both disappear into
 * fallthroughs, and what replaces them is the duplicated test - a compare and a conditional branch
 * against two five-byte jumps.
 *
 * ## SSA, and why the phis move
 *
 * A header phi names a value per predecessor, and after rotation the header has only the latch left.
 * The merge it was performing has moved to the body, which is now what both the preheader and the
 * header lead into, so each header phi becomes one there - and, where the loop's result is read
 * afterwards, one in the exit block as well, since the exit is now reached from the preheader too.
 * Both take the same pair: what the preheader hands over, and what the rotated header holds.
 *
 * The one that is easy to get wrong is what a *header* instruction reads. The rotated header runs
 * after the body, so a phi it used to read has already been advanced by the latch: the value it
 * wants is the phi's latch alternative, and a header phi appearing in that alternative is in turn
 * the body's phi. `%i` in the test becomes `%i2`, which is exactly the induction variable the
 * iteration just finished computing.
 *
 * ## What is declined
 *
 * The shape has to be the ordinary one, and every requirement below exists because the repair above
 * is stated in terms of it - one preheader ending in an unconditional jump, one latch, and a header
 * whose two successors are one block inside the loop and one outside, each reached from nowhere else.
 * The header must also be the only block the loop leaves through, or a value it defines could be
 * read on a path the exit block does not dominate and there would be nowhere to put the phi.
 *
 * A header instruction read anywhere but the header is declined for the same reason in miniature.
 * The repair exists - it is the same pair of phis - but the shape it would serve is a header doing
 * work rather than a header asking a question, and duplicating that work is a different trade.
 */

// The largest header this will duplicate. The shape it is for is a comparison and its operands, and
// a header past this size is one where the duplication is the dominant cost rather than a rounding
// error against the two jumps it removes.
static constexpr Size kMaxRotatedHeader = 4;

// Whether the header's copy of this instruction may also be made in the preheader.
//
// Every kind here computes one value from its operands and reads nothing that the block it moves
// into cannot supply. A store, a call or a `copy` is excluded because duplicating it duplicates an
// effect - even though it would run the same number of times, the second copy is code that has to
// be kept in step with the first - and an `alloca` because a second one is a second allocation.
static bool isRotatableHeaderInst(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Imm:
        case LowerInst::Global:
        case LowerInst::Fun:
        case LowerInst::Set:
        case LowerInst::Cast:
        case LowerInst::Bitcast:
        case LowerInst::Neg:
        case LowerInst::Not:
        case LowerInst::Add:
        case LowerInst::Sub:
        case LowerInst::Mul:
        case LowerInst::IMul:
        case LowerInst::MulHi:
        case LowerInst::IMulHi:
        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar:
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
        case LowerInst::Cmp:
        case LowerInst::Select:
        case LowerInst::Load:
            return true;
        default:
            return false;
    }
}

// How much storage one of the kinds above occupies. Each of them is a fixed-shape allocation - the
// created value and the operand pointers are members rather than a trailing array - so the copy
// below can be a flat one, and every field an instruction carries comes across without this having
// to know which fields those are.
static Size rotatedInstSize(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Imm:    return sizeof(LowerImm);
        case LowerInst::Global: return sizeof(LowerInstGlobal);
        case LowerInst::Fun:    return sizeof(LowerInstFun);
        case LowerInst::Cast:   return sizeof(LowerInstCast);
        case LowerInst::Cmp:    return sizeof(LowerInstCmp);
        case LowerInst::Select: return sizeof(LowerInstSelect);
        case LowerInst::Load:   return sizeof(LowerInstLoad);
        default:                return isUnary(inst) ? sizeof(LowerInstUnary) : sizeof(LowerInstBinary);
    }
}

// A second copy of one header instruction, detached: it belongs to no block and nothing reads it
// yet, and its operands still name what the original's did until the caller remaps them.
//
// The result value is rebuilt in place rather than patched, which is what clears the use list the
// copy inherited - a list whose entries name the *original's* readers - and drops the source name
// with it, so the two copies do not both claim to be `%i`.
static LowerInst* cloneHeaderInst(Region<LowerRegion>& arena, LowerInst* inst) {
    auto size = rotatedInstSize(inst);
    auto clone = (LowerInst*)arena.alloc(size);
    copyMem(inst, clone, size);

    clone->block = nullptr;
    clone->liveId = kNullLive;

    auto created = clone->created();
    for(Size i = 0; i < created.size(); i++) {
        auto flags = created[i].flags;
        new (&created[i]) LowerValue(clone, created[i].type, StringId());
        created[i].flags = flags;
    }

    return clone;
}

// Which alternative of `phi` the edge from `from` carries.
static Size phiSourceIndex(LowerBase base, LowerInstPhi* phi, LowerBlock* from) {
    auto sources = phi->sources();

    for(Size i = 0; i < sources.size(); i++) {
        if(base[sources[i]] == from) return i;
    }

    assertTrue("a phi has no alternative for one of its own predecessors" == nullptr);
    return 0;
}

// Everything an instruction reads stops counting it as a reader. The instruction itself is dropped
// from wherever it is listed by the caller - see removeInst, which is this plus that.
static void detachOperands(LowerBase base, LowerInst* inst) {
    for(auto offset: inst->used()) {
        auto v = base[offset];
        auto uses = v->uses.contents(base);

        for(Size i = 0; i < uses.size(); i++) {
            if(base[uses[i]] == inst) { v->uses.remove(base, i); break; }
        }
    }
}

// Points one operand of `user` at a different value, both use lists included.
static void retargetOperand(LowerBase base, LowerInst* user, Size slot, LowerValue* to) {
    auto from = base[user->used()[slot]];
    if(from == to) return;

    replaceUse(base, from, user, to);
    user->used()[slot] = to - base;
}

// Replaces the phi at `index` with one that takes an alternative from one more predecessor.
//
// A phi's alternatives are allocated with it, so gaining an edge means a new instruction rather than
// a longer list - and the result value moves with it, which is why every reader has to be pointed at
// the replacement. Only the exit and body blocks need this, and only for the preheader's new edge.
static void growPhi(LowerBase base, LowerFunction& fun, LowerBlock* block, Size index,
                    LowerBlock* extraSource, LowerValue* extraValue)
{
    auto& arena = fun.arena;
    auto old = base[block->phis.get(base, index)];
    auto count = Size(old->usedCount);

    auto phi = makePhi(arena, old->result.type, U32(count + 1));
    phi->result.name = old->result.name;
    phi->source = old->source;
    phi->block = block - base;

    auto used = phi->used();
    auto sources = phi->sources();
    auto oldUsed = old->used();
    auto oldSources = old->sources();

    for(Size i = 0; i < count; i++) {
        used[i] = oldUsed[i];
        sources[i] = oldSources[i];
    }

    used[count] = extraValue - base;
    sources[count] = extraSource - base;

    detachOperands(base, (LowerInst*)old);
    for(auto u: used) base[u]->uses.push(arena, (LowerInst*)phi - base);

    block->phis.set(base, index, phi - base);
    replaceAllUses(base, &old->result, &phi->result);
}

// A phi this pass built that nothing turned out to read. Taken back out rather than left behind,
// since a value with no readers is still a live range the allocator would carry through the loop.
static bool dropUnusedPhi(LowerBase base, LowerBlock* block, LowerInstPhi*& phi) {
    if(!phi || phi->result.uses.size()) return false;

    for(Size i = 0; i < block->phis.size(); i++) {
        if(base[block->phis.get(base, i)] == phi) {
            block->phis.remove(base, i);
            break;
        }
    }

    detachOperands(base, (LowerInst*)phi);
    phi = nullptr;
    return true;
}

// One loop in the shape the rotation is stated in - see the comment above for what each block has to
// be, and rotatableLoop for what is checked.
struct RotatableLoop {
    LowerBlock* header;
    LowerBlock* pre;    // the one predecessor outside the loop, ending in an unconditional jump
    LowerBlock* latch;  // the one predecessor inside it
    LowerBlock* body;   // the header's successor inside the loop, which the rotation makes the entry
    LowerBlock* exit;   // the header's successor outside it
};

// One header phi and the three values it becomes: what the preheader hands over, what the rotated
// header holds, and the merge of the two in each of the blocks that now sees both.
struct RotatedPhi {
    LowerInstPhi* header;
    LowerValue* pre;
    LowerValue* hdr;
    LowerInstPhi* body;
    LowerInstPhi* exit;
};

/*
 * A block a loop leaves to that carries nothing onwards - the one second exit the rotation can take.
 *
 * The gate below asks for a single exit because of where a header phi's readers are sent: one outside
 * the loop is pointed at the merge built in the *exit* block, which is only its value where the exit
 * dominates it. A second way out reaches code the exit does not dominate, and there is nothing to
 * point such a reader at.
 *
 * Unless the block it reaches is dominated by the loop's own body, in which case there is: the merge
 * built there. That is what this asks for, in the two conditions it is the conjunction of - every way
 * in comes from inside the loop, and there is no way onwards - because together they say the block is
 * dominated by the body after the rotation as well as before it, and that nothing beyond it can be
 * looking at a loop value at all.
 *
 * Two shapes in ordinary code are exactly this. A bounds check's abort arm is one that reads nothing
 * (§10 item 2 of test/bench/findings.md gave every one of them a `ret`, which is what made every
 * checked loop multi-exit); an early `return` out of a `while` is one that reads the induction
 * variable, and `Float.escape` in the corpus is a loop that pays an unconditional jump per iteration
 * for having one.
 *
 * **Or the block reads nothing the loop defines**, which is the same conclusion reached without the
 * first condition. What the two together establish is where a *reader* of a header phi can be
 * pointed; a block with no such reader has nothing to point anywhere, and having no successors it
 * cannot hand one on either. So the predecessors stop mattering, and that is not a corner: it is the
 * abort arm again, once `mergeIdenticalExits` has made the program's copies of it one block. Sharing
 * one exit between two nested loops gives the inner one's arm a predecessor from the outer, and
 * without this every one of Matrix's three loops stopped rotating and its innermost paid a jump per
 * iteration for it.
 */
static bool readsLoopValue(LowerBase base, const LoopInfo& loops, U32 headerIndex, LowerInst* inst) {
    for(auto used: inst->used()) {
        auto from = base[used]->inst()->block;
        if(from && loops.contains(headerIndex, base[from]->index)) return true;
    }

    return false;
}

/*
 * §48.1 The same conclusion for a second exit that does carry on.
 *
 * `terminalExit` above asks for two things at once - every way in comes from the loop, and there is
 * no way onwards - and only the first of them is the dominance argument. The second is there because
 * a *successor* of such a block is a reader the first condition says nothing about.
 *
 * So the first condition is taken to its own fixpoint instead, and the same question is asked of the
 * other side as well, because after the rotation there are two merges to point a reader at and each
 * is only that reader's value where it dominates it:
 *
 *   the body's merge   in a block every path into which passes through the loop
 *   the exit's merge   in a block every path into which passes through the header's exit arm
 *
 * Both are the same walk over one seed set, which is what this is. A block joins when it has
 * predecessors and every one of them is in the seeds or already joined - "every path here passes
 * through the seeds" - and since the rotation makes the body the block the loop is entered through,
 * a block the loop is the only way into is one the body dominates.
 *
 * **A reader in neither set is what refuses the loop, and `Iter.firstOverOrCount` is why.** Its two
 * early exits converge on a block that also falls into the block the header's exit arm reaches, so
 * the count is read where *neither* merge dominates - and rotating it anyway produced a function that
 * read a register the guard path had never written. That is the check `rotatableLoop` makes below,
 * and it is over the phis' readers rather than over the exits, because a reader is what needs a
 * value and an exit is only how one gets there.
 *
 * `indexOfVectors` in the SIMD corpus is the shape this is for: a search loop whose found-arm reads
 * the accumulated index and then jumps to the function's common return, which is one block onwards
 * and so one block too far for `terminalExit`.
 */
static void collectReachedOnlyFrom(LowerBase base, LowerFunction& fun, const IndexSet& seeds,
                                   const IndexSet& excluded, IndexSet& into)
{
    into.reset(fun.blocks.size());

    auto changed = true;
    while(changed) {
        changed = false;

        for(auto o: fun.blocks.contents(base)) {
            auto block = base[o];
            auto index = Size(block->index);

            if(into[index] || seeds[index] || excluded[index]) continue;

            // The entry block has no predecessors, so the rule below would admit it vacuously.
            if(block->incoming.isEmpty()) continue;

            auto only = true;
            for(auto p: block->incoming.contents(base)) {
                auto pred = Size(base[p]->index);
                if(seeds[pred] || into[pred]) continue;

                only = false;
                break;
            }

            if(!only) continue;

            into.set(index, true);
            changed = true;
        }
    }
}

/*
 * Where each of the rotation's three answers is the reader's value, as three sets of blocks.
 *
 * Built per candidate loop and reused across them, since every one of them is a walk over the
 * function's blocks and a loop that is refused has already paid for it.
 */
struct RotationRegions {
    IndexSet inLoop;    // the loop's own blocks
    IndexSet bodySide;  // outside it, and the loop is the only way in
    IndexSet exitSide;  // the header's exit arm, and everything it is the only way into

    // What each walk is given as its seeds and its exclusions; named so that the two calls read as
    // two questions rather than as one function taking four sets.
    IndexSet seeds;
    IndexSet excluded;
};

// Which of the three a read of a header phi arriving from `from` takes, or none at all - which is a
// reader neither merge dominates, and the reason a loop is refused.
enum class RotatedRead: U8 { Header, Body, Exit, Nowhere };

static RotatedRead rotatedRead(const RotationRegions& regions, LowerBlock* header, LowerBlock* from) {
    auto index = Size(from->index);

    if(from == header) return RotatedRead::Header;
    if(regions.inLoop[index] || regions.bodySide[index]) return RotatedRead::Body;
    if(regions.exitSide[index]) return RotatedRead::Exit;

    return RotatedRead::Nowhere;
}

static void buildRotationRegions(LowerBase base, LowerFunction& fun, const LoopInfo& loops,
                                 U32 headerIndex, LowerBlock* exit, RotationRegions& regions)
{
    auto count = fun.blocks.size();
    auto exitIndex = Size(exit->index);

    regions.inLoop.reset(count);
    for(auto o: fun.blocks.contents(base)) {
        auto block = base[o];
        if(loops.contains(headerIndex, block->index)) regions.inLoop.set(Size(block->index), true);
    }

    // The body's side. The header's own exit arm is held out of it, and that is the whole of the care
    // needed: every one of its predecessors is in the loop too, so it would join on the rule above -
    // and it is precisely the block the rotation gives a *new* predecessor to, the preheader's guard,
    // whose purpose is to carry the zero-iteration answer.
    regions.excluded.reset(count);
    regions.excluded.set(exitIndex, true);
    collectReachedOnlyFrom(base, fun, regions.inLoop, regions.excluded, regions.bodySide);

    // And the exit's, which is that arm and everything it is in turn the only way into. Excluding
    // what the body's side already claimed is not a tie-break: a block in both would be one the loop
    // and the exit are each the only way into, which is a block with no way in at all.
    regions.seeds.reset(count);
    regions.seeds.set(exitIndex, true);

    regions.excluded.reset(count);
    regions.excluded.unionWith(regions.inLoop);
    regions.excluded.unionWith(regions.bodySide);
    collectReachedOnlyFrom(base, fun, regions.seeds, regions.excluded, regions.exitSide);
    regions.exitSide.set(exitIndex, true);
}

static bool terminalExit(LowerBase base, const LoopInfo& loops, U32 headerIndex, LowerBlock* block) {
    if(block->outgoing[0] || block->outgoing[1]) return false;

    auto entered = true;
    for(auto p: block->incoming.contents(base)) {
        if(!loops.contains(headerIndex, base[p]->index)) { entered = false; break; }
    }

    if(entered) return true;

    // A phi is a reader on an *edge* rather than in a block, so one here is refused outright rather
    // than asked about - the alternative it takes from a loop predecessor is a loop value read on a
    // path this has just decided not to reason about.
    if(block->phis.size()) return false;

    for(auto i: block->instructions.contents(base)) {
        if(readsLoopValue(base, loops, headerIndex, base[i])) return false;
    }

    return !readsLoopValue(base, loops, headerIndex, base[block->terminator]);
}

static Maybe<RotatableLoop> rotatableLoop(LowerBase base, LowerFunction& fun, const LoopInfo& loops,
                                          LowerBlock* header, RotationRegions& regions)
{
    if(base[header->terminator]->kind != LowerInst::Je) return Nothing();

    auto index = header->index;
    auto first = base[header->outgoing[0]];
    auto second = base[header->outgoing[1]];

    // Exactly one arm may leave. A header that branches within the loop is not the block the loop is
    // left from, and one whose arms both leave is not a loop this pass can read.
    auto firstStays = loops.contains(index, first->index);
    if(firstStays == loops.contains(index, second->index)) return Nothing();

    RotatableLoop loop {};
    loop.header = header;
    loop.body = firstStays ? first : second;
    loop.exit = firstStays ? second : first;

    if(loop.body == header) return Nothing();
    if(header->incoming.size() != 2) return Nothing();

    auto a = base[header->incoming.get(base, 0)];
    auto b = base[header->incoming.get(base, 1)];

    auto aStays = loops.contains(index, a->index);
    if(aStays == loops.contains(index, b->index)) return Nothing();

    loop.latch = aStays ? a : b;
    loop.pre = aStays ? b : a;

    /*
     * The preheader has to be a block whose whole purpose is to enter the loop, since its jump is
     * what becomes the guard; and the two blocks that gain the preheader as a predecessor have to
     * have had only the header, or a phi in either would need alternatives this cannot supply.
     *
     * **Counted, the three are one refusal** - §44 of test/bench/findings.md. Over the 233
     * `test/resolve` programs 149 loops rotate, and of the three conditions here the first two refuse
     * *nothing at all*: no loop in the corpus or the suite has a preheader that is not a plain jump,
     * and none has a body reached by more than one edge. The third refuses 37, and 5 over the
     * benchmark corpus.
     *
     * **And generalizing the third was built and measured out.** Twenty of those 37 are the case that
     * can be answered - every extra edge into the exit leaves the *loop*, so the value a header phi
     * had on it is the iteration's own and the merge can take the body's phi for it - and one of the
     * five is. Built, it is +19 bytes over the 184 executables for no measurable time, which is the
     * verdict §29 reached for three other relaxations of this same pass: the rotation trades a jump
     * per iteration for a copy of the header test in the preheader, and a loop that reaches this
     * condition is one where that trade has already stopped paying. The other 17 cannot be answered
     * at all - an edge into the exit from *outside* the loop means the header does not dominate it,
     * and there is no value for the merge to carry.
     */
    if(base[loop.pre->terminator]->kind != LowerInst::Jmp) return Nothing();
    if(loop.body->incoming.size() != 1) return Nothing();
    if(loop.exit->incoming.size() != 1) return Nothing();

    /*
     * §48.1 Every reader of a header phi has to have one of the two merges dominating it.
     *
     * This used to be asked of the *exits* - the header had to be the only way out, or every other
     * way out had to go nowhere - which is a sufficient condition for the real one and refuses a
     * search loop whose found-arm carries on into the function's common return. The real one is
     * asked here instead, of the readers, because a reader is what needs a value and an exit is only
     * how one gets there.
     *
     * A reader in neither region is a read the rotation has nothing to point at, and the loop is
     * refused. `Iter.firstOverOrCount` is the shape: two early exits converging on a block that the
     * header's exit arm also falls into, so the count is read where neither merge dominates.
     */
    buildRotationRegions(base, fun, loops, index, loop.exit, regions);

    for(auto p: header->phis.contents(base)) {
        auto phi = base[p];

        for(auto u: phi->result.uses.contents(base)) {
            auto user = base[u];
            auto used = user->used();

            for(Size slot = 0; slot < used.size(); slot++) {
                if(base[used[slot]] != &phi->result) continue;

                // For a phi the read happens on the edge it names rather than in the block it sits
                // in, which is what lets a merge below the loop take the body's answer on one
                // alternative and the exit's on another.
                auto from = user->kind == LowerInst::Phi
                    ? base[((LowerInstPhi*)user)->sources()[slot]]
                    : base[user->block];

                if(rotatedRead(regions, header, from) == RotatedRead::Nowhere) return Nothing();
            }
        }
    }

    if(header->instructions.size() > kMaxRotatedHeader) return Nothing();

    for(auto i: header->instructions.contents(base)) {
        auto inst = base[i];
        if(!isRotatableHeaderInst(inst)) return Nothing();

        // Read only where it is computed. A phi reader is refused whatever block it sits in, since
        // what it reads the value on is an edge - including the latch edge, which leaves the header
        // by a route the block of the reader does not show.
        for(auto u: inst->created().ptr->uses.contents(base)) {
            auto user = base[u];
            if(user->kind == LowerInst::Phi || base[user->block] != header) return Nothing();
        }
    }

    return Just(loop);
}

static void rotateLoop(LowerBase base, LowerFunction& fun, const LoopInfo& loops,
                       const RotatableLoop& loop, const RotationRegions& regions)
{
    auto& arena = fun.arena;
    auto header = loop.header;
    auto pre = loop.pre;
    auto body = loop.body;
    auto exit = loop.exit;
    auto headerIndex = header->index;

    SmallArray<RotatedPhi, 8> phis;
    for(auto p: header->phis.contents(base)) {
        auto phi = base[p];
        phis.push(RotatedPhi { phi, base[phi->used()[phiSourceIndex(base, phi, pre)]], nullptr, nullptr, nullptr });
    }

    // What each value the header defines is called at the end of the preheader: a phi is whatever it
    // takes from that edge, and an instruction is the copy made below. Everything else is itself,
    // since a value the header could read already reached the preheader to get there.
    SmallArray<LowerValue*, kMaxRotatedHeader> originals;
    SmallArray<LowerValue*, kMaxRotatedHeader> clones;

    auto inPre = [&](LowerValue* v) -> LowerValue* {
        for(auto& r: phis) if(&r.header->result == v) return r.pre;
        for(Size i = 0; i < originals.size(); i++) if(originals[i] == v) return clones[i];
        return v;
    };

    for(auto i: header->instructions.contents(base)) {
        auto inst = base[i];
        auto clone = cloneHeaderInst(arena, inst);

        auto used = clone->used();
        for(Size k = 0; k < used.size(); k++) used[k] = inPre(base[used[k]]) - base;

        pre->addInst(base, clone);
        originals.push(inst->created().ptr);
        clones.push(clone->created().ptr);
    }

    auto je = (LowerInstJe*)base[header->terminator];
    auto guardCond = inPre(base[je->cond]);

    // The preheader stops entering the loop and starts deciding. Unwired by hand because addInst
    // records an edge rather than replacing one, and refuses a successor that already names it.
    pre->terminator = nullptr;
    pre->outgoing[0] = nullptr;
    pre->outgoing[1] = nullptr;

    for(Size i = 0; i < header->incoming.size(); i++) {
        if(base[header->incoming.get(base, i)] == pre) { header->incoming.remove(base, i); break; }
    }

    auto guard = new (arena) LowerInstJe(guardCond - base, je->then, je->otherwise);
    guard->likelihood[0] = je->likelihood[0];
    guard->likelihood[1] = je->likelihood[1];
    guard->source = je->source;
    pre->addInst(base, guard);

    // Whatever the two blocks already merged, they now merge one more edge of. The alternative the
    // preheader brings is what its own copy of the header computed, which is what the first
    // iteration - or the zero-iteration case, in the exit block - would have arrived with.
    for(auto block: { body, exit }) {
        for(Size i = 0; i < block->phis.size(); i++) {
            auto phi = base[block->phis.get(base, i)];
            auto slot = phiSourceIndex(base, phi, header);
            growPhi(base, fun, block, i, pre, inPre(base[phi->used()[slot]]));
        }
    }

    // The merges the header's own phis become. Built before they are filled in, because what the
    // rotated header holds is stated in terms of them: a phi advanced by the latch reads, at the
    // point the latch now sits, the body's phi rather than the header's.
    for(auto& r: phis) {
        r.body = makePhi(arena, r.header->result.type, 2);
        r.exit = makePhi(arena, r.header->result.type, 2);
    }

    auto inBody = [&](LowerValue* v) -> LowerValue* {
        for(auto& r: phis) if(&r.header->result == v) return &r.body->result;
        return v;
    };

    for(auto& r: phis) {
        r.hdr = inBody(base[r.header->used()[phiSourceIndex(base, r.header, loop.latch)]]);
    }

    for(auto& r: phis) {
        for(auto phi: { r.body, r.exit }) {
            auto used = phi->used();
            auto sources = phi->sources();

            used[0] = r.pre - base;
            sources[0] = pre - base;
            used[1] = r.hdr - base;
            sources[1] = header - base;
        }

        body->addInst(base, r.body);
        exit->addInst(base, r.exit);
    }

    /*
     * Everything that still names a header phi, pointed at whichever of the three answers its own
     * position asks for. What decides is where the read happens, which for a phi is the edge it
     * reads on and not the block it sits in:
     *
     *   in the header      the value the latch just produced, which is what the rotated header sees
     *   elsewhere in loop  the body's phi, which is now what the loop is entered through
     *   outside the loop   the exit's phi, which merges the guard's answer with the last iteration's
     *
     * A second exit reads on an edge that leaves the loop and is nevertheless the *body's* answer:
     * every way into such a block is from inside the loop, so the body dominates it, and the exit's
     * merge - which is what the guard's zero-iteration answer arrives through - is a value it was
     * never reached by. See `terminalExit` for the block that goes nowhere and
     * `collectLoopOnlyBlocks` for the one that carries on into code the loop is still the only way
     * into.
     */
    // One list for the walk, emptied per phi: the readers are snapshotted because the loop below
    // retargets them, and a list per phi is an allocation per phi - see InstChain.
    InstChain users;

    for(auto& r: phis) {
        auto value = &r.header->result;

        users.clear();
        for(auto u: value->uses.contents(base)) users.push(base[u]);

        for(auto user: users) {
            auto used = user->used();

            for(Size slot = 0; slot < used.size(); slot++) {
                if(base[used[slot]] != value) continue;

                auto from = user->kind == LowerInst::Phi
                    ? base[((LowerInstPhi*)user)->sources()[slot]]
                    : base[user->block];

                auto to = &r.exit->result;
                switch(rotatedRead(regions, header, from)) {
                    case RotatedRead::Header: to = r.hdr;             break;
                    case RotatedRead::Body:   to = &r.body->result;   break;
                    case RotatedRead::Exit:   break;

                    // Refused by `rotatableLoop`, which is what makes this unreachable rather than
                    // a case with an answer.
                    case RotatedRead::Nowhere:
                        assertTrue("a header phi is read where neither merge reaches" == nullptr);
                        break;
                }

                retargetOperand(base, user, slot, to);
            }
        }
    }

    for(auto& r: phis) {
        assertTrue(r.header->result.uses.size() == 0);
        detachOperands(base, (LowerInst*)r.header);
    }

    while(header->phis.size()) header->phis.remove(base, header->phis.size() - 1);

    // A loop-carried value that turns out to be read only inside the loop needs no exit merge, and
    // one only the header itself advances needs no body merge. Which of them are unread is not
    // known until the rewriting above has run, and dropping one can leave another unread in turn.
    bool dropped = true;
    while(dropped) {
        dropped = false;

        for(auto& r: phis) {
            dropped |= dropUnusedPhi(base, body, r.body);
            dropped |= dropUnusedPhi(base, exit, r.exit);
        }
    }
}

/*
 * §30.4 A preheader made rather than found.
 *
 * The rotation puts the header's copy of its own test at the end of the preheader, and there is one
 * block in a function that may not receive an instruction: the implicit entry. `runLegalizer` emits
 * the argument-home copies at index 0 and asserts nothing else is there, and `buildRanges` gives an
 * argument its range from outside every block - so an instruction placed there is a read in a block
 * that neither defines the argument nor has it live-in, which is a range that does not exist.
 *
 * A loop whose header the entry block enters directly is therefore unrotatable for a reason that has
 * nothing to do with the loop, and the answer is to stop finding the preheader and make one: an empty
 * block on the edge, which is the shape every other loop the lowering produces already has. It costs
 * nothing where the rotation then declines the loop anyway - `computeBypass` skips a block with no
 * instructions, no moves and an unconditional jump, and this is one until the rotation fills it.
 *
 * The rule is exact rather than a guess about which successor is a header. The entry block has one
 * successor and every block is reached through it, so a second predecessor of that successor is a
 * block the successor reaches: a back edge, and the successor a loop header.
 */
static LowerBlock* insertJumpPreheader(LowerBase base, LowerFunction& fun, LowerBlock* pred) {
    auto& arena = fun.arena;
    auto succ = base[pred->outgoing[0]];
    auto predOffset = pred - base;

    auto split = new (arena) LowerBlock(pred->fun, StringId(), BlockIndex(fun.blocks.size()));
    fun.blocks.push(arena, split - base);

    // Wired up by hand for the reason splitEdge gives: addInst would append the new block to `succ`'s
    // incoming list rather than replacing the entry the phis still name.
    auto jmp = (LowerInst*)new (arena) LowerInstJmp(succ - base);
    jmp->block = split - base;
    split->terminator = jmp - base;
    split->outgoing[0] = succ - base;
    split->incoming.push(arena, predOffset);

    auto old = (LowerInstJmp*)base[pred->terminator];
    assertTrue(old->kind == LowerInst::Jmp);
    old->then = split - base;
    pred->outgoing[0] = split - base;

    for(Size i = 0; i < succ->incoming.size(); i++) {
        if(succ->incoming.get(base, i) == predOffset) {
            succ->incoming.set(base, i, split - base);
            break;
        }
    }

    for(auto p: succ->phis.contents(base)) {
        auto sources = base[p]->sources();
        for(Size i = 0; i < sources.size(); i++) {
            if(sources.ptr[i] == predOffset) sources.ptr[i] = split - base;
        }
    }

    return split;
}

// Ahead of the loop analysis rather than inside it, because a block created afterwards is one the
// LoopInfo the rotation reads is not indexed for.
static void insertEntryPreheader(LowerBase base, LowerFunction& fun) {
    auto entry = base[fun.blocks.get(base, 0)];
    if(!entry->terminator || base[entry->terminator]->kind != LowerInst::Jmp) return;
    if(base[entry->outgoing[0]]->incoming.size() < 2) return;

    insertJumpPreheader(base, fun, entry);
}

/*
 * §32.3 The rotated header, folded into the latch it now sits behind.
 *
 * The rotation leaves the test in a block of its own, and that block has one predecessor - the latch
 * - and no phis, the header's merges having become the body's and the exit's. So the two are already
 * one straight line of instructions with a block boundary drawn across it, and the boundary costs a
 * real instruction: **the backend treats the flags as dead at every block edge** (`computeFlagsRead`,
 * and `flagsWindowEnd` which refuses a reader outside the comparison's own block), so a counter
 * decremented in the latch and tested in the header is `dec ; test ; jcc` where the decrement has
 * already answered in SF and ZF.
 *
 * Merging them is the ordinary single-predecessor fold and needs no new claim about flags at all:
 * `tryElideCompare` then finds the definition in its own block and elides the comparison the way it
 * already does everywhere else. That is why this is a CFG transform rather than a widening of the
 * flags window - the window's block-locality is what `emitAsLea` depends on (§31.3: a `lea` may
 * replace an `add` exactly because nothing across the edge can be reading the flags it drops), and
 * carrying flags over an edge would make that peephole silently wrong.
 *
 * ## What it costs
 *
 * For a one-block loop the latch *is* the body, so the merged block becomes its own successor and
 * its phis acquire a self-edge. That is a shape the rest of the pipeline already handles -
 * `normalizePhiEdges` splits an edge whose predecessor has two successors and whose successor has
 * phis, which is exactly this one - and it is why this runs where it does: before that pass, and
 * before anything that reasons about block indices.
 *
 * ## Two refusals
 *
 * The entry block never receives instructions - `runLegalizer` asserts it holds none, its terminator
 * being at index zero is what lets the argument copies be emitted ahead of the function - so a
 * header whose predecessor is the entry is left alone. And the predecessor has to end in a plain
 * `Jmp`: a conditional branch to this block means the other arm exists, which contradicts the single
 * incoming edge, and asking rather than assuming keeps the two facts from having to agree.
 */
static bool foldIntoPredecessor(LowerBase base, LowerFunction& fun, LowerBlock* block) {
    if(block->phis.size() != 0 || block->incoming.size() != 1 || !block->terminator) return false;

    auto pred = base[block->incoming.get(base, 0)];
    if(pred == block || pred == base[fun.blocks.get(base, 0)]) return false;

    auto jump = base[pred->terminator];
    if(jump->kind != LowerInst::Jmp || base[((LowerInstJmp*)jump)->then] != block) return false;

    auto& arena = fun.arena;

    // The jump is dropped rather than detached: a `Jmp` reads no value, so no use list mentions it,
    // and the edge it recorded is what the instructions below take over.
    pred->terminator = nullptr;
    pred->outgoing[0] = nullptr;
    pred->outgoing[1] = nullptr;

    // Moved rather than re-added: `addInst` would register every operand a second time, and these
    // instructions are already recorded as readers of what they read. Only the block changes.
    for(auto offset: block->instructions.contents(base)) {
        base[offset]->block = pred - base;
        pred->instructions.push(arena, offset);
    }

    pred->terminator = block->terminator;
    base[block->terminator]->block = pred - base;
    pred->outgoing[0] = block->outgoing[0];
    pred->outgoing[1] = block->outgoing[1];

    // Every successor now arrives from the predecessor, including the predecessor itself where the
    // loop was one block: a self-edge here is a phi alternative that names its own block, which is
    // what a merged latch and header is.
    for(auto successor: pred->outgoing) {
        if(!successor) continue;

        auto to = base[successor];
        for(Size i = 0; i < to->incoming.size(); i++) {
            if(to->incoming.get(base, i) == block - base) to->incoming.set(base, i, pred - base);
        }

        for(auto p: to->phis.contents(base)) {
            auto sources = base[p]->sources();
            for(Size i = 0; i < sources.size(); i++) {
                if(sources.ptr[i] == block - base) sources.ptr[i] = pred - base;
            }
        }
    }

    while(block->instructions.size()) block->instructions.remove(base, block->instructions.size() - 1);
    while(block->incoming.size()) block->incoming.remove(base, block->incoming.size() - 1);
    block->terminator = nullptr;
    block->outgoing[0] = nullptr;
    block->outgoing[1] = nullptr;

    for(Size i = 0; i < fun.blocks.size(); i++) {
        if(fun.blocks.get(base, i) != block - base) continue;

        fun.blocks.remove(base, i);
        break;
    }

    // Renumbering is not optional: `index` is a position in this list and half the analyses index
    // arrays by it.
    for(Size i = 0; i < fun.blocks.size(); i++) base[fun.blocks.get(base, i)]->index = BlockIndex(i);
    return true;
}

static void rotateFunctionLoops(LowerBase base, LowerFunction& fun) {
    insertEntryPreheader(base, fun);

    auto loops = fun.buildLoops(base);

    // Snapshotted, because rotating one loop is what stops its header from being one. Which blocks a
    // loop *contains* is what everything below asks, and that is what rotation leaves alone: no
    // block is created or renumbered, and the body it moves the entry to was already a member.
    SmallArray<LowerPtr<LowerBlock>, 16> headers;
    for(auto o: fun.blocks.contents(base)) {
        if(loops.isHeader(base[o]->index)) headers.push(o);
    }

    // One set of regions for the function, filled per candidate: each is a walk over every block,
    // and a loop that is refused has already paid for it - see RotationRegions.
    RotationRegions regions;

    SmallArray<LowerPtr<LowerBlock>, 16> rotated;
    for(auto o: headers) {
        if(auto loop = rotatableLoop(base, fun, loops, base[o], regions)) {
            rotateLoop(base, fun, loops, loop.unwrap(), regions);
            rotated.push(o);
        }
    }

    // Behind every rotation rather than after each one: folding removes a block and renumbers, and
    // the `loops` above is indexed by the numbering it was built with.
    for(auto o: rotated) foldIntoPredecessor(base, fun, base[o]);
}

/*
 * Block order.
 *
 * The list is rewritten into reverse postorder, so that a block is - wherever the CFG allows it -
 * visited after the predecessors that define the values live on entry to it. Both consumers depend
 * on that: buildRanges numbers instructions in block-list order, and its ranges are only tight when
 * that order follows the control flow; genFunction emits in the same order, so reverse postorder
 * also turns more branches into fallthrough. Keeping one order for both is what lets the allocator
 * work in linear indices and the encoder walk in lockstep with it.
 *
 * *Which* reverse postorder is a further choice, though, and taking the successors as declared is
 * the wrong one around a loop. A header ending in `je body, exit` explores the body first, so the
 * body is finished - and pushed onto the postorder - before the exit, and comes out *after* it once
 * the postorder is reversed: the exit block lands between the header and the body it leaves. Every
 * iteration then pays a taken branch into the body and a jump back, and every interval spanning the
 * loop is split into two ranges around the intruding block.
 *
 * Exploring the successor that *leaves* the loop first fixes both, since it is finished first and so
 * reversed last.
 *
 * That generalizes, and is what the choice is actually made on: **explore the less likely successor
 * first**, so that the likely one is reversed into the position immediately after the branch and
 * becomes the fallthrough. A loop exit is only the case of it the CFG can derive on its own - the
 * edge leaving a loop is taken once where the edge staying in it is taken every iteration but the
 * last - and a branch the IR says is unlikely gets the same treatment for the same reason. Where the
 * two edges are equally likely there is nothing to prefer, and the loop depth decides: the deeper
 * successor goes last, so that a block entering a loop is followed by the loop rather than by
 * whatever comes after it. `edgeWeightsOf` is where those probabilities come from, and it is the
 * same one the block frequencies are computed from - so layout and cost cannot disagree about which
 * arm of a branch is the common one.
 */

static void traverseOrdered(LowerBase base, const LoopInfo& loops, LowerBlock* b, BlockList& out) {
    b->marker = 1;

    auto first = b->outgoing[0] ? base[b->outgoing[0]] : nullptr;
    auto second = b->outgoing[1] ? base[b->outgoing[1]] : nullptr;

    if(first && second) {
        auto weights = edgeWeightsOf(base, loops, b);

        auto swapThem = weights.weight[0] != weights.weight[1]
            ? weights.weight[0] > weights.weight[1]
            : loops.depth[first->index] > loops.depth[second->index];

        if(swapThem) ::swap(first, second);
    }

    if(first && !first->marker) traverseOrdered(base, loops, first, out);
    if(second && !second->marker) traverseOrdered(base, loops, second, out);

    out.push(b->index);
}

static void orderBlocks(LowerBase base, LowerFunction& fun) {
    auto blockList = fun.blocks.contents(base);
    auto entry = base[fun.blocks.get(base, 0)];

    // Also leaves each block's loop depth on it, which is where the ordering below reads it back
    // from after the renumbering has invalidated the index-keyed result.
    auto loops = fun.buildLoops(base);

    for(auto o: blockList) base[o]->marker = 0;

    BlockList postorder(blockList.size());
    traverseOrdered(base, loops, entry, postorder);

    // A block that the entry point cannot reach has no place in the ordering, and nothing
    // downstream is prepared to allocate registers for one.
    assertTrue(postorder.size() == fun.blocks.size());

    // Inline on the same terms as BlockList, which this is a permutation of.
    SmallArray<LowerPtr<LowerBlock>, 64> ordered;
    for(Size i = postorder.size(); i > 0; i--) {
        ordered.push(fun.blocks.get(base, postorder[i - 1]));
    }

    for(Size i = 0; i < ordered.size(); i++) {
        auto b = base[ordered[i]];

        fun.blocks.set(base, i, ordered[i]);
        b->index = BlockIndex(i);
    }
}

/*
 * The transform pipeline.
 *
 * The passes below used to be one function with the order expressed as the sequence of statements in
 * it, and the reasons for that order as comments between them. They are named passes now, with the
 * order stated once in kTransformPipeline and each pass's contract stated next to the pass.
 *
 * The order is not arbitrary and each step of it is load-bearing:
 *
 *   rotateLoops                changes the CFG, and so goes before every pass that reasons about a
 *                              position within it - and before liveness is ever built, since it both
 *                              creates and removes phis
 *   canonicalizeOperands       puts immediates where the later passes expect to find them, so that
 *                              nothing downstream has to check both sides of a commutative operation
 *   selectAddressesAndLeas     removes address arithmetic *before* liveness, which is the only point
 *                              at which removing it actually shortens an interval - and before the
 *                              immediate peephole, so that an immediate the fold leaves with no uses
 *                              is made implicit rather than materialized into a register nothing reads
 *   selectMemorySources        folds a load into the instruction that consumes it, which needs the
 *                              address above it to be an X86Address already
 *   selectMachineInstructions  chooses the shape of each instruction: which immediates are embedded,
 *                              which comparisons stay in the flags, which callees are elided, which
 *                              encoding a block operation takes
 *   lowerOutgoingStackArguments  turns a call's stack-passed arguments into explicit stores, which is
 *                              only worth doing once the passes above have settled what is implicit
 *   normalizePhiEdges          gives every phi transfer a block it can safely be emitted in
 *   analyzeLoopsAndOrderBlocks lays the blocks out, last, since it invalidates every instruction
 *                              index the passes above reasoned about
 *
 * A pass that changes any of this changes the pipeline table, not the reading order of one function.
 */

// Walks every instruction of every block in list order, with its index within its block. For passes
// that only inspect and annotate: one that inserts or removes instructions has to iterate by index,
// because the list is rewritten underneath it.
template<class F>
static void forEachInst(LowerBase base, LowerFunction& fun, F&& onInst) {
    for(auto b: fun.blocks.contents(base)) {
        Size i = 0;

        for(auto inst: base[b]->instructions.contents(base)) {
            onInst(base[inst], i);
            i++;
        }
    }
}

/*
 * The passes.
 */

// Turns a loop tested at the top into one tested at the bottom, by making the preheader ask the
// header's question itself - see the loop-rotation comment above.
//
// First, and it has to be: it is the only pass here that changes the CFG other than by splitting an
// edge, and every pass below either reasons about an instruction's position within a block or reads
// the branch structure the layout is chosen from.
//
// Expects: the lowering's output, unmodified.  Establishes: no loop of the shape described above
// leaves its test at the top. Mutates: the CFG, the phis of four blocks, and the instruction list of
// the preheader. Invalidates: loops, dominators and every block-relative position.
static void rotateLoops(Context&, LowerBase base, LowerFunction& fun) {
    rotateFunctionLoops(base, fun);
}

// Moves operands into the canonical position for the passes below: an immediate onto the right-hand
// side of a commutative operation, so that nothing downstream has to look at both sides, and a
// floating-point `lt`/`le` exchanged into the `gt`/`ge` this machine can answer for a NaN, and a
// packed `gt`/`ge` exchanged the other way into the predicate `cmpps` has.
// Representation-neutral: no target register or encoding decision is made here.
//
// Expects: the lowering's output, unmodified.  Establishes: commutative immediates on the right, and
// no float comparison below. Mutates: operand order and the comparison an instruction carries.
// Invalidates: nothing.
static void canonicalizeOperands(Context&, LowerBase base, LowerFunction& fun) {
    forEachInst(base, fun, [&](LowerInst* inst, Size i) {
        trySwapOperands(base, inst);
        orderFloatCompare(base, inst);
        orderPackedCompare(base, inst);
    });
}

// Recognizes `base + index*scale + displacement` once, and turns each occurrence into either an
// X86Address folded into the access that reads it (§3.1) or an X86Lea that materializes it (§3.3).
//
// Runs before the peepholes rather than after them: an immediate whose only use was an address
// computation is left with none by the fold, and is then made implicit by the pass below rather than
// being materialized into a register nothing reads. It also runs before liveness, which is what lets
// the arithmetic it eliminates genuinely shorten intervals.
//
// Expects: canonical operands.  Establishes: no memory access reaches allocation with a foldable
// address computation in front of it. Mutates: the instruction lists and every affected use list.
// Invalidates: instruction positions within a block.
static void selectAddressesAndLeas(Context&, LowerBase base, LowerFunction& fun) {
    foldAddresses(base, fun);
    foldLeas(base, fun);
}

// Folds a load into the instruction that consumes it, where the encoding has a form that reads its
// operand out of memory: `add rax, [rdi + rcx*8]` in place of a load and an add.
//
// After the pass above rather than inside it, and the order is required both ways. The address the
// load reads has to be an X86Address already, so that the fold inherits the whole addressing mode
// rather than half of it; and the address folding asks the *opcode* which operand is an address
// (opcodeAddressOperand), an answer that is only stable while no ALU instruction has been moved onto
// a memory-source form.
//
// Expects: addresses selected.  Establishes: no load reaches allocation whose only reader could have
// read it out of memory itself. Mutates: the instruction lists, the operand order of a commutative
// operation, and the affected use lists. Invalidates: instruction positions within a block.
static void selectMemorySources(Context&, LowerBase base, LowerFunction& fun) {
    foldLoads(base, fun);
}

// Folds a load, an operation on it and the store of the result back to the same place into one
// memory-destination instruction: `add [out + i*4], edx` in place of three.
//
// **Above `selectAddressesAndLeas`**, which is the whole of where this may sit. What it produces is
// an instruction with an address operand, and that pass is what turns the pointer arithmetic under
// one into an addressing mode - so running below it would leave the update reading a pointer some
// `lea` had to materialize, which is the instruction this removed put back.
//
// Expects: canonical operands.  Establishes: no store reaches allocation whose value is an operation
// on a load of the same location. Mutates: the instruction lists and the affected use lists.
// Invalidates: instruction positions within a block.
static void selectStoreUpdates(Context&, LowerBase base, LowerFunction& fun) {
    foldStoreUpdates(base, fun);
}

/*
 * The constant pool: one read-only global per distinct floating-point constant.
 *
 * No SSE encoding carries a float as an immediate, so this backend materialized one in a general
 * register and moved it across the bank boundary - ten or eleven bytes, a general register the form
 * had to declare as a clobber, and a value the allocator could never rematerialize. `[rip + k]` is
 * the answer every other x86-64 toolchain gives, and it is eight bytes, no general register at all,
 * and a load the hardware has a whole cache for.
 *
 * §0.2 of Implementation-Vector.md asks for it as a *prerequisite* rather than as an optimization,
 * and that is the part worth writing down: a vector constant cannot be materialized the old way at
 * all. Sixteen bytes do not fit a general register, so there is no register to move across - which
 * makes the pool the thing that has to exist before a single vector literal can be emitted.
 *
 * ## One global per constant rather than one pool with offsets
 *
 * Because a relocation names a global and carries no addend, so an offset into a shared pool would
 * need a field on `AsmRelocation` and a second way of resolving one. A global of its own costs the
 * padding `addGlobal` puts in front of it - up to twelve bytes for a `Float32` - and buys 16-byte
 * alignment on every entry, which is what a vector load will require anyway.
 *
 * The interning is `LowerModule::globals` itself: the name *is* the bit pattern, so two functions
 * that mention `1.0` reach one global without this pass holding a map of its own.
 *
 * ## Where it sits
 *
 * Before `selectMemorySources`, so that `foldLoads` *does* see the loads this creates: a constant
 * read once by the instruction below it becomes `addsd xmm, [rip + k]` rather than a load into a
 * register and an add of it. That fold needs the rip-relative form of `LowerInstX86Address`, which
 * is why this pass ran after it until that existed - without a symbol field the global would have
 * been committed to a register with a `lea` in front of it, worse than the load it replaced.
 *
 * Before `selectMachineInstructions`, because `tryFoldGlobalAddress` is what turns the address of a
 * constant that was *not* folded into the addressing mode of its own load. Without that sweep the
 * global would be a `lea` of its own.
 *
 * After `selectAddressesAndLeas`, which has nothing to say about either.
 */
static LowerGlobal* pooledConstant(Context& ctx, LowerModule& module, U64 bits, Size size) {
    // `$f032$0000000000000001`. Written out rather than formatted so that the name is exactly the
    // bit pattern at a fixed width - two constants of different widths that happen to share a
    // pattern are two entries, and neither can be a prefix of the other.
    static const char digits[] = "0123456789abcdef";
    char text[] = "$f000$0000000000000000";
    auto width = size * 8;

    text[2] = digits[(width / 100) % 10];
    text[3] = digits[(width / 10) % 10];
    text[4] = digits[width % 10];
    for(Size i = 0; i < 16; i++) text[21 - i] = digits[(bits >> (i * 4)) & 0xf];

    // The hash rather than the interning call, because the interning has to happen exactly once -
    // see below - and this is the same number `addUnqualifiedName` would answer with.
    auto length = sizeof(text) - 1;
    auto name = Context::nameHash(text, length);

    auto entry = module.globals.add(name);
    if(entry.existed) return (*module.arena)[*entry.value];

    /*
     * **`addUnqualifiedName` keeps the pointer it is handed rather than a copy of it**, which
     * `addQualifiedName` beside it does not - so a name built on the stack is a dangling one the
     * moment this returns, and what a dump or an ELF symbol table prints is whatever is there now.
     * That is not a crash: the five constants of `Float.yana` all appeared in `readelf` under one
     * four-byte name made of whatever the next call left on the stack.
     *
     * So the text is copied into the arena that outlives the compilation, and only on the branch
     * where the name is new - a repeat would intern to the same id and leave the copy unread.
     */
    auto stored = (char*)module.arena.alloc(length);
    copyMem(text, stored, length);
    ctx.addUnqualifiedName(stored, length);

    auto global = new (module.arena) LowerGlobal(name);
    auto contents = (U8*)module.arena.alloc(size);

    // Repeated to fill the entry, which is what makes a sixteen-byte one the *broadcast* of its
    // pattern: the sign mask a negation exclusive-ors against has to hold the bit in every lane it
    // might reach, and a vector constant will want the same shape for the same reason.
    for(Size at = 0; at < size; at += 8) copyMem(&bits, contents + at, min(Size(8), size - at));

    global->initialContents = { contents, size };
    *entry.value = global - *module.arena;
    module.globalOrder.push(global - *module.arena);

    return global;
}

/*
 * The same pool, entered with the bytes already laid out - which is what a vector constant needs and
 * `pooledConstant` above cannot express, since it takes one word and repeats it.
 *
 * The name is the whole pattern in hex, so the interning `LowerModule::globals` already performs is
 * still exact: two entries collide only if every byte agrees, and a 16-byte constant's name cannot be
 * a prefix of a 32-byte one's because the width is in the name ahead of the bytes.
 */
static LowerGlobal* pooledBytes(Context& ctx, LowerModule& module, const U8* bytes, Size size) {
    static const char digits[] = "0123456789abcdef";

    // `$v128$<2 * size hex digits>`. Sized for the widest constant this language admits.
    char text[6 + 2 * kMaxVectorBytes];
    auto width = size * 8;

    text[0] = '$';
    text[1] = 'v';
    text[2] = digits[(width / 100) % 10];
    text[3] = digits[(width / 10) % 10];
    text[4] = digits[width % 10];
    text[5] = '$';

    for(Size i = 0; i < size; i++) {
        text[6 + i * 2] = digits[bytes[i] >> 4];
        text[7 + i * 2] = digits[bytes[i] & 0xf];
    }

    auto length = 6 + size * 2;
    auto name = Context::nameHash(text, length);

    auto entry = module.globals.add(name);
    if(entry.existed) return (*module.arena)[*entry.value];

    // Copied into the arena for the reason `pooledConstant` states: `addUnqualifiedName` keeps the
    // pointer it is handed, and this one is on the stack.
    auto stored = (char*)module.arena.alloc(length);
    copyMem(text, stored, length);
    ctx.addUnqualifiedName(stored, length);

    auto global = new (module.arena) LowerGlobal(name);
    auto contents = (U8*)module.arena.alloc(size);
    copyMem(bytes, contents, size);

    global->initialContents = { contents, size };
    *entry.value = global - *module.arena;
    module.globalOrder.push(global - *module.arena);

    return global;
}

/*
 * The bytes a value holds if it is a constant vector, and whether it is one.
 *
 * A constant vector in this IR is not a constant - §9.7 of Implementation-Vector.md records that
 * there is deliberately no vector constant form, because a lane pattern is not an immediate on any
 * of these machines. What there is instead is the shape the resolver builds: a `vsplat` of a
 * constant, and a chain of `vwithlane`s over it whose values are constants. `iota` is exactly that,
 * and so is every `splat` of a literal.
 *
 * So this is the folder's job done at the one place that can act on the answer. Reading the chain
 * here rather than folding it into a constant earlier is what keeps the IR's own claim true.
 */
static bool constantVectorBytes(LowerBase base, LowerValue* value, U8* bytes, Size size,
                                InstChain& chain) {
    auto inst = value->inst();
    auto type = value->type;
    auto lane = laneBytes(type.lane);

    // A lane's bits at its own width, from an immediate of the lane's *scalar* form - which for an
    // 8- or 16-bit lane is an Int32, and for a Float32 lane is held as a double until here.
    auto laneBits = [&](LowerValue* from, U8* at) {
        auto source = from->inst();
        if(source->kind != LowerInst::Imm) return false;

        auto imm = (LowerImm*)source;

        if(type.lane == LowerLane::Float32) {
            auto narrow = float(imm->f);
            copyMem(&narrow, at, 4);
            return true;
        }

        if(type.lane == LowerLane::Float64) {
            auto wide = imm->f;
            copyMem(&wide, at, 8);
            return true;
        }

        auto integer = imm->i;
        copyMem(&integer, at, lane);
        return true;
    };

    if(inst->kind == LowerInst::VecSplat) {
        auto splat = (LowerInstVecSplat*)inst;
        auto from = base[splat->from];
        if(!laneBits(from, bytes)) return false;

        for(Size at = lane; at < size; at += lane) copyMem(bytes, bytes + at, lane);

        chain.push(inst);
        chain.push(from->inst());
        return true;
    }

    /*
     * A vector read as another vector of the same width, which changes nothing about the bytes.
     *
     * This is how a constant reaches the *other* lane kind. `expandVectorAbs` builds its mask as an
     * integer splat - a float lane's immediate is held as a double and narrowed, which cannot state
     * a NaN's payload exactly, and `0x7fffffff` is a NaN - and then reads it as the float vector the
     * `andps` wants. Without this the chain would stop at the bitcast, the *inner* splat would be
     * pooled on its own, and what the `and` read would be a bitcast of a load rather than a load:
     * one instruction more, and the fold that puts the constant in the addressing mode gone.
     */
    if(inst->kind == LowerInst::Bitcast) {
        auto from = base[((LowerInstUnary*)inst)->from];
        if(!isVectorLike(from->type) || from->type.byteWidth() != type.byteWidth()) return false;
        if(!constantVectorBytes(base, from, bytes, size, chain)) return false;

        chain.push(inst);
        return true;
    }

    if(inst->kind == LowerInst::VecWithLane) {
        auto write = (LowerInstVecLane*)inst;
        if(!constantVectorBytes(base, base[write->from], bytes, size, chain)) return false;

        auto from = base[write->value];
        if(!laneBits(from, bytes + Size(write->getLane()) * lane)) return false;

        chain.push(inst);
        chain.push(from->inst());
        return true;
    }

    return false;
}

/*
 * Whether this value is a constant vector, asked from the other side of the pass that pools it.
 *
 * `checkVectorSupported` runs at the top of `transformFunction` and refuses what this backend has no
 * form for - and a *lane write* of an 8-bit lane is one of those, since `pinsrw` writes a word and
 * half of one would have to be read back out first. But nothing emits the lane writes of a constant
 * chain: `poolVectorConstants` below replaces the whole chain with a `.rodata` load and removes
 * every link of it, so refusing one is refusing an instruction that will not exist.
 *
 * That is not a hypothetical case, it is `iota` - which `maskUpTo` and `firstSet` are both written
 * over, so every masked tail of every byte-lane loop is exactly this shape. Asked here rather than
 * approximated in machine.cpp, on `packedCompareRelation`'s argument: two readers on opposite sides
 * of a pass have to ask one function or they will drift.
 */
bool isPooledVectorConstant(LowerBase base, LowerValue* value) {
    auto type = value->type;
    if(!type.isVector() && !type.isMask()) return false;

    U8 bytes[kMaxVectorBytes] = {};
    InstChain chain;

    return constantVectorBytes(base, value, bytes, type.byteWidth(), chain);
}

/*
 * The chain that fed the constant, removed.
 *
 * Nothing below this pass is a dead-code elimination - the IR optimizer ran long ago and what is
 * left here is selection - so a link whose only reader was the next link stays in the function and
 * is emitted. Left to itself the pass made `iota` *longer*: the load appeared and the six
 * instructions it replaced were still there.
 *
 * To a fixpoint rather than in one sweep, and that is not caution. A link and the immediate it reads
 * die in that order, so any single ordering leaves half of them standing: walked outermost-first the
 * immediates are still used when they are looked at, and walked innermost-first the lane writes are.
 * The chain is at most two entries per lane, so repeating until nothing moves is bounded by its own
 * length and is what makes "the constant leaves nothing behind" true rather than nearly.
 *
 * Each is removed only once its own use list is empty, which is what keeps a constant that two
 * chains share, or that something else reads, exactly where it is.
 */
static void removeDeadChain(LowerBase base, InstChain& chain) {
    for(Size round = 0; round <= chain.size(); round++) {
        auto moved = false;

        for(Size i = chain.size(); i > 0; i--) {
            auto inst = chain[i - 1];
            if(!inst || inst->createdCount != 1) continue;

            auto result = &((LowerInstSingle*)inst)->result;
            if(!result->uses.isEmpty()) continue;

            removeInst(base, inst);

            /*
             * Cleared so that a second round does not remove it again - `inst` is the outermost link
             * and has already been removed by the caller, so this list may hold one either way.
             *
             * **Every entry holding it, not only this one.** A list built from more than one rewrite
             * holds the shared links twice - two absolute values over one hoisted zero, two masked
             * selects over one `iota` - and the second entry would otherwise be an instruction with
             * an empty use list that is no longer in any block, which `removeInst` reports as the
             * structural error it would be anywhere else.
             */
            for(Size c = 0; c < chain.size(); c++) if(chain[c] == inst) chain[c] = nullptr;
            moved = true;
        }

        if(!moved) break;
    }
}

/*
 * The absolute value of a float vector, which is one bit per lane.
 *
 * `LowerInst::Abs` is the magnitude and says nothing about how - see the `Abs` row in
 * resolve/inst.def, which is where the language rules that the sign of a NaN is unspecified. That
 * ruling is what lets this be one instruction: `v & 0x7fffffff` per lane leaves the exponent and the
 * mantissa exactly where they are, so every finite value, both infinities and both zeros come out
 * with the magnitude they had, and `-0.0` becomes `+0.0`.
 *
 * An **integer** lane is not here at all: `pabsb`/`pabsw`/`pabsd` are ordinary forms, and the
 * quadword - which has no `pabsq` outside AVX-512 - is refused by `unsupportedVectorReason` rather
 * than expanded, the comparison it would have to fall back on being missing at that width too.
 *
 * ## The mask is an integer constant read as a float one
 *
 * `0x7fffffff` is a NaN when read as a float, and a float lane's immediate is held as a double in
 * this IR and narrowed where the bytes are taken - which is exact for every value in the language
 * and not for a NaN's payload. So the constant is built as an *integer* splat and bitcast to the
 * float vector the `and` works at, which `constantVectorBytes` reads through: the pool gets one
 * entry of the right bytes, and `andps` reads it in its own domain.
 *
 * ## The mask goes in the entry block, and that is the whole of what it is worth
 *
 * Built beside the `and`, the mask is the load *immediately above* its reader - and `tryFoldLoad`
 * takes the load immediately above, so the mask won the addressing mode and the value being
 * measured had to be loaded into a register first:
 *
 *     vmovups (%rdx),%ymm3 ; vandps 0x9b6(%rip),%ymm3,%ymm3
 *
 * Two instructions and, less obviously, **two loads**: the pooled mask is re-read from `.rodata`
 * every iteration. There is one r/m field, so only one operand can be the memory one, and the right
 * one to spend it on is the operand that changes. Built in the entry block, the mask leaves the loop
 * (once per function, and interned per lane width), the value's own load becomes the one above the
 * `and`, and the loop body is `vandps (%rdx),%ymmMask,%ymm3` - one instruction and one load.
 *
 * What that costs is a register held across the function, and it is the cheapest kind: a load of a
 * global nothing writes is rematerializable (`recipeFor` in place.cpp), so a function under pressure
 * spills it by forgetting it and re-loading where it is next read.
 */
static void expandVectorAbs(Context&, LowerBase base, LowerFunction& fun) {
    /*
     * The block the mask is built in, which is the entry block's *successor* rather than the entry
     * block itself.
     *
     * `LowerFunction`'s entry block is implicit and holds no instructions - its terminator is index
     * zero, which is what lets the legalizer emit the incoming argument copies ahead of everything
     * the function executes (`runLegalizer` asserts it). So the first block that may hold one is the
     * one that block jumps to, and a value defined at the top of it dominates every use for the same
     * reason the entry block does: every path through the function goes through it.
     */
    auto home = [&]() -> LowerBlock* {
        if(fun.blocks.isEmpty()) return nullptr;

        auto entry = base[fun.blocks.get(base, 0)];
        auto terminator = base[entry->terminator];
        if(terminator->kind != LowerInst::Jmp) return nullptr;

        return base[((LowerInstJmp*)terminator)->then];
    }();

    if(!home) return;

    // One mask per lane width, built on demand and shared by every absolute value in the function -
    // interning it here rather than leaving it to CSE, which does not run below this point.
    LowerValue* masks[2] = { nullptr, nullptr };

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Abs) continue;

            auto type = ((LowerInstUnary*)inst)->result.type;
            if(!isFloatVector(type)) continue;

            auto lane = laneBytes(type.lane);
            auto& mask = masks[lane == 4 ? 0 : 1];

            if(!mask) {
                /*
                 * At the *top* of that block, which is where a value with no operands may go and
                 * where it dominates every use by construction. The three instructions are an
                 * immediate, the splat over it and the reinterpretation - `poolVectorConstants`
                 * folds all three into one load of the whole pattern.
                 */
                auto integerLane = lane == 4 ? LowerLane::Int32 : LowerLane::Int64;
                auto integers = LowerType(integerLane, type.laneShift, false);
                auto bits = lane == 4 ? U64(0x7fffffff) : (~U64(0) >> 1);

                Expansion at { base, fun, home, 0 };
                auto scalar = at.integer(scalarFormOf(integers), bits);
                auto splat = at.splat(integers, scalar);
                mask = at.reinterpret(type, splat);

                // The walk is inside that block whenever this fires there, and three instructions
                // have just been put in front of it.
                if(block == home) i += 3;
            }

            Expansion e { base, fun, block, i };
            auto cleared = e.binary(LowerInst::And, type, base[((LowerInstUnary*)inst)->from], mask,
                                    ((LowerInstUnary*)inst)->result.name);

            replaceAllUses(base, &((LowerInstUnary*)inst)->result, cleared);
            removeInst(base, inst);

            // The `and` stands where the absolute value did, so the walk carries on past it.
            i = e.at - 1;
        }
    }
}

/*
 * A mask the two constants either side of a comparison have already decided, and the two things that
 * read one.
 *
 * This is the tail mask of every bulk operation. `maximumVectors` (resolve/core.cpp) is written as
 *
 *     acc = max(acc, select(maskUpTo(live) :: Mask(a), v, acc))
 *
 * and `occurrencesVectors` beside it as `count(m .& maskUpTo(live))`, so that the last chunk
 * contributes only its live lanes - and the *full* chunks go through the identical line with `live`
 * equal to the lane count. `maskUpTo(n)` is `iota .< splat(n)`, both of whose operands are constant
 * vectors in the full-chunk loop, so the mask is all-ones: the select is its own first arm, and the
 * `and` is its other operand. Left standing the first was a `vpcmpgtd` hoisted out of the loop, a
 * register held for its result across the whole loop, and a `vpblendvb` per chunk that answered its
 * second operand every time; the second was that same hoisted comparison and a `vpand` per chunk
 * that changed nothing.
 *
 * Removing it is worth more than the blend, and that is the reason this pass exists rather than the
 * one instruction it deletes: what the blend stood between was the *load* and the operation that
 * reads it. `vmovdqu (%rdx),%ymm3 ; vpblendvb ; vpmaxsd %ymm3,%ymm0,%ymm0` is three instructions and
 * a register where `vpmaxsd (%rdx),%ymm0,%ymm0` is one - a blend takes three registers and can never
 * be the thing a load folds into, so the fold below is what lets `tryFoldLoad` see the pair at all.
 *
 * ## Why the comparison rather than the mask
 *
 * A mask has no constant form in this IR and is not going to get one: `constantVectorBytes` reads a
 * `vsplat`/`vwithlane` chain of immediates, and a mask lane's immediate is a truth value rather than
 * the all-ones pattern the machine holds - so "the bytes of a constant mask" is a question with two
 * plausible answers and no reader that needs it. The comparison has no such ambiguity: both its
 * operands are ordinary vectors, and what is asked of them is whether every lane answers the same
 * way. So this recognizes `cmp(k1, k2)` in the two positions that read a mask, and nothing more
 * general.
 *
 * A mixed answer is left alone. It could be folded into a shuffle or into a pooled mask, and neither
 * is reachable from anything the library writes - `maskUpTo` of a constant is all-ones or nothing.
 *
 * An `and` against an all-*false* mask is left alone too, and that is a different refusal: the
 * answer is a mask of no lanes, which this IR has no constant form for. `select` has no such gap,
 * both its arms being values that already exist.
 */

// One lane of two constant vectors compared, at the relation and lane type given. The bytes are the
// vector's own, so a lane is read out of them at its width and its kind.
static bool constantLaneCompare(LowerCmp cmp, LowerLane lane, const U8* lhs, const U8* rhs) {
    if(lane == LowerLane::Float32 || lane == LowerLane::Float64) {
        F64 a = 0, b = 0;

        if(lane == LowerLane::Float32) {
            float na = 0, nb = 0;
            copyMem(lhs, &na, 4);
            copyMem(rhs, &nb, 4);
            a = na;
            b = nb;
        } else {
            copyMem(lhs, &a, 8);
            copyMem(rhs, &b, 8);
        }

        // An unordered pair answers false to every ordered relation and true to `neq`, which is what
        // the two tests below say without naming a NaN: `a == a` is false for one.
        auto ordered = (a == a) && (b == b);

        switch(cmp) {
            case LowerCmp::eq:  return a == b;
            case LowerCmp::neq: return a != b;
            case LowerCmp::lt:  return ordered && a < b;
            case LowerCmp::le:  return ordered && a <= b;
            case LowerCmp::gt:  return ordered && a > b;
            case LowerCmp::ge:  return ordered && a >= b;
            case LowerCmp::uno: return !ordered;
            case LowerCmp::ord: return ordered;
            default:            return false; // a signed integer relation between floats
        }
    }

    auto width = laneBytes(lane);
    U64 a = 0, b = 0;
    copyMem(lhs, &a, width);
    copyMem(rhs, &b, width);

    // Sign-extended for the signed relations, which is what makes `ilt` over an `i8` lane read `0xff`
    // as -1 rather than as 255.
    auto shift = 64 - width * 8;
    auto sa = I64(a << shift) >> shift;
    auto sb = I64(b << shift) >> shift;

    switch(cmp) {
        case LowerCmp::eq:  return a == b;
        case LowerCmp::neq: return a != b;
        case LowerCmp::lt:  return a < b;
        case LowerCmp::le:  return a <= b;
        case LowerCmp::gt:  return a > b;
        case LowerCmp::ge:  return a >= b;
        case LowerCmp::ilt: return sa < sb;
        case LowerCmp::ile: return sa <= sb;
        case LowerCmp::igt: return sa > sb;
        case LowerCmp::ige: return sa >= sb;
        default:            return false; // an ordering test on an integer lane
    }
}

/*
 * Whether this value is a comparison of two constant vectors that answers the same way in every
 * lane, and which way - with the chains that fed the constants collected for the sweep.
 *
 * The one question both readers below ask, which is why it is a function: a select wants the answer
 * to choose an arm and an `and` wants it to decide whether the mask takes anything away, and a
 * second copy of the lane walk would be a second chance to disagree about a NaN.
 */
static bool constantMaskAnswer(LowerBase base, LowerValue* value, bool& answer,
                               InstChain& chain) {
    auto inst = value->inst();
    if(inst->kind != LowerInst::Cmp) return false;

    auto cmp = (LowerInstCmp*)inst;
    auto lhs = base[cmp->lhs];
    auto rhs = base[cmp->rhs];
    auto type = lhs->type;

    // A vector of values, and not a mask: a mask lane's immediate is a truth value rather than the
    // pattern the machine holds, so what `constantVectorBytes` would answer about one is not what a
    // comparison of two of them means. Nothing produces that shape today; this is what keeps the
    // answer from depending on that staying true.
    if(!isIntVector(type) && !isFloatVector(type)) return false;

    auto size = Size(type.byteWidth());
    if(size > kMaxVectorBytes) return false;

    U8 left[kMaxVectorBytes] = {};
    U8 right[kMaxVectorBytes] = {};

    if(!constantVectorBytes(base, lhs, left, size, chain)) return false;
    if(!constantVectorBytes(base, rhs, right, size, chain)) return false;

    auto width = laneBytes(type.lane);
    answer = constantLaneCompare(cmp->getCmp(), type.lane, left, right);

    for(Size at = width; at < size; at += width) {
        if(constantLaneCompare(cmp->getCmp(), type.lane, left + at, right + at) != answer) {
            return false;
        }
    }

    return true;
}

static void foldConstantMasks(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // What the walk below leaves behind: a comparison whose last reader it removed, and the
        // constant chains that fed one. Both are cleared after the walk rather than during it,
        // because either may stand *above* the instruction being folded - removing one there would
        // renumber the instructions the walk is indexing, which is the one thing this loop assumes
        // does not happen.
        InstChain dead;

        // Emptied per instruction rather than built per instruction, which is the difference
        // between one list for the block and one for every instruction in it - see InstChain.
        InstChain chain;

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];

            LowerValue* replacement = nullptr;
            LowerValue* condition = nullptr;
            auto answer = false;
            chain.clear();

            if(inst->kind == LowerInst::Select) {
                auto select = (LowerInstSelect*)inst;
                if(!isVectorLike(select->result.type)) continue;

                condition = base[select->cmp];
                if(!constantMaskAnswer(base, condition, answer, chain)) continue;

                // Every lane took the same side, so the select is that side and nothing else.
                replacement = base[answer ? select->lhs : select->rhs];
            } else if(inst->kind == LowerInst::And
                      && ((LowerInstBinary*)inst)->result.type.isMask()) {
                auto binary = (LowerInstBinary*)inst;

                /*
                 * `m .& allOnes` is `m`. Only that direction: the other one answers a mask of no
                 * lanes, which this IR cannot write down - see the note above.
                 */
                for(Size side = 0; side < 2 && !replacement; side++) {
                    chain.clear();
                    condition = base[side ? binary->lhs : binary->rhs];

                    if(!constantMaskAnswer(base, condition, answer, chain) || !answer) continue;
                    replacement = base[side ? binary->rhs : binary->lhs];
                }

                if(!replacement) continue;
            } else {
                continue;
            }

            replaceAllUses(base, &((LowerInstSingle*)inst)->result, replacement);
            removeInst(base, inst);

            // The comparison and the constants that fed it, for the sweep below - nothing between
            // here and emission is a dead-code elimination, so an instruction left with no readers
            // is one that gets encoded.
            dead.push(condition->inst());
            for(auto link: chain) dead.push(link);

            // The walk carries on from where the folded instruction was: what stands there now is
            // whatever followed it, and nothing above it changed.
            i--;
        }

        // Each is removed only once its own use list is empty, which is `removeDeadChain`'s rule and
        // is what keeps a comparison with a second reader, or a constant two chains share, exactly
        // where it is.
        removeDeadChain(base, dead);
    }
}

/*
 * A select one of whose arms is zero, which is an `and`.
 *
 * `select(m, v, 0)` keeps `v` where the mask is set and writes zero everywhere else, and a mask lane
 * is all-ones or all-zeros by construction - so that is `v & m`, one instruction, at every feature
 * level and in both domains. The mirrored `select(m, 0, v)` is `~m & v`, which is `pandn`, and is
 * one instruction as well because the complement is in the opcode rather than in front of it.
 *
 * What it replaces is the select, which is this backend's most expensive vector operation:
 *
 *   cmpltps  %xmm3,%xmm6      cmpltps %xmm3,%xmm6
 *   movaps   %xmm2,%xmm7   →  andps   %xmm6,%xmm5
 *   movaps   %xmm6,%xmm0
 *   pblendvb %xmm0,%xmm5,%xmm7
 *
 * Three instructions to one at SSE4.1, where the mask has to be copied into xmm0 because that is
 * where `pblendvb` reads it (see FormVSelectBlend); two to one under VEX, where `vpblendvb` takes
 * three register operands and the zero is one of them.
 *
 * **The register the zero was living in is the larger half of it.** A blend reads three vectors, so
 * the zero arm is a value with a live range - materialized in the entry block by `poolVectorConstants`
 * or a `pxor`, and held across whatever loop the select is in. Rewriting the select is what takes
 * the last reader off it; the pooled chain then goes the way every other orphaned constant here
 * goes, through `removeDeadChain`.
 *
 * Both arms are asked, and a select with zero on *both* would answer the first - which is a select
 * that is zero, and not a shape anything builds. `foldConstantMasks` above has already taken
 * the ones whose mask is constant, so what reaches here is a genuine runtime mask.
 */
static void selectMaskedVectors(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // The constant chain the zero arm was, cleared after the walk for the reason the two passes
        // above clear theirs: it may stand above the select being rewritten, and removing it there
        // would renumber the instructions this loop is indexing.
        InstChain dead;

        // One list for the block, emptied per instruction - see InstChain.
        InstChain chain;

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Select) continue;

            auto select = (LowerInstSelect*)inst;
            auto type = select->result.type;

            // A vector select, and one this backend has a bitwise form for. A mask is deliberately
            // included: `select(m, k, allZeroMask)` over two masks is the same `pand`.
            if(!isVectorLike(type) || !isWholePackedRegister(type)) continue;

            auto size = Size(type.byteWidth());
            if(size > kMaxVectorBytes) continue;

            // Which arm is the zero, asked of the bytes rather than of the kind: a zero vector is a
            // `vsplat 0` here, a `.rodata` load once `poolVectorConstants` has run, and a lane chain
            // that happens to come to zero in between. This pass runs above that one, so what it
            // sees is the chain - and `constantVectorBytes` is the reader that knows all three.
            auto isZeroArm = [&](LowerPtr<LowerValue> arm, InstChain& chain) {
                U8 bytes[kMaxVectorBytes] = {};
                if(!constantVectorBytes(base, base[arm], bytes, size, chain)) return false;

                for(Size at = 0; at < size; at++) {
                    if(bytes[at]) return false;
                }

                return true;
            };

            chain.clear();
            auto complemented = false;

            if(isZeroArm(select->rhs, chain)) {
                complemented = false;
            } else {
                chain.clear();
                if(!isZeroArm(select->lhs, chain)) continue;
                complemented = true;
            }

            /*
             * The operand order is the machine's: `pand` is commutative and takes the value first so
             * that the tie lands on it, `pandn` computes `~lhs & rhs` and therefore takes the mask
             * first. See LowerInst::X86MaskAnd.
             */
            auto mask = base[select->cmp];
            auto value = base[complemented ? select->rhs : select->lhs];
            auto masked = new (fun.arena) LowerInstX86MaskAnd(
                select->result.name, type,
                (complemented ? mask : value) - base, (complemented ? value : mask) - base,
                complemented
            );

            insertInstAt(base, block, i, masked);
            replaceAllUses(base, &select->result, &masked->result);
            removeInst(base, select);

            for(auto link: chain) dead.push(link);
        }

        removeDeadChain(base, dead);
    }
}

/*
 * Vector constants - Implementation-Vector.md §0.2's prerequisite, finally spent on what it was
 * asked for.
 *
 * §5 concluded that the pool "turned out not to need to be" opened to vectors, on the evidence of
 * four operations that could each build their constant out of a scratch register. That generalized
 * from a sample chosen by being buildable: what is *not* buildable that way is every pattern with
 * more than one distinct lane in it, and the commonest of those is `iota` - which `maskUpTo` and
 * `firstSet` are both written over, so **every masked tail in every vector loop** was paying a chain
 * of `lanes` lane-writes where a load is one instruction.
 *
 * Where it sits is the same argument `poolFloatConstants` makes and it is worth more here: before
 * `selectMemorySources`, so `foldLoads` sees these loads. §5.4.1 opened the vector memory twin, so a
 * pooled constant read once by the instruction below it becomes `vpaddd xmm, xmm, [rip + k]` and the
 * common case is not one instruction but none.
 *
 * **Zero is pooled here where the float pass leaves it an immediate**, and the asymmetry is real
 * rather than an oversight. That pass keeps `0.0` because `xorps xmm, xmm` is a one-instruction form
 * this backend already selects, so the constant has something cheaper to lose to; a vector has no
 * such form - `vsplat 0` is a general register zeroed, a bank crossing and a shuffle - so there is
 * nothing for zero to lose to and the load wins on count outright. If a `pxor` peephole is written
 * it belongs *before* this pass, which will then not see the splat at all.
 */
/*
 * A constant chain that is uniformly zero or uniformly all-ones, rewritten as the splat it means.
 *
 * Reachable where a program wrote the pattern the long way - `withLane`ing a vector into all-ones a
 * lane at a time is not idiomatic but is expressible - and it exists so that the peephole below has
 * one shape to recognize rather than two. The immediate is marked Implicit because the form's
 * operand is `folded()`: the opcode *is* the value, so nothing about the scalar is encoded and it
 * must not be given a register. A fresh one rather than reusing whatever the chain held, since a
 * shared constant may have a scalar reader that does need its register.
 */
static void replaceWithConstantSplat(LowerBase base, LowerBlock* block, Size at, LowerValue* result,
                                     LowerType type, bool zero, InstChain& chain) {
    auto& fun = *base[block->fun];
    auto scalar = scalarFormOf(type);
    auto width = laneBytes(type.lane);
    auto value = zero ? U64(0) : (width >= 8 ? ~U64(0) : ((U64(1) << (width * 8)) - 1));

    auto imm = new (fun.arena) LowerImm(StringId(), scalar, value);
    imm->result.flags |= LowerValue::Implicit;

    auto splat = new (fun.arena) LowerInstVecSplat(result->name, type, &imm->result - base);

    insertInstAt(base, block, at, imm);
    insertInstAt(base, block, at + 1, splat);

    replaceAllUses(base, result, &splat->result);
    removeInst(base, base[result->inst() - base]);
    for(Size c = 0; c < chain.size(); c++) if(chain[c] == result->inst()) chain[c] = nullptr;

    removeDeadChain(base, chain);
}

static void poolVectorConstants(Context& ctx, LowerBase base, LowerFunction& fun) {
    auto& module = *fun.module;

    // One list for the function, emptied per candidate - see InstChain. Every instruction of every
    // block reaches the walk below, and most of them are not constants at all.
    InstChain chain;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];

            // A bitcast is a root as well as a link, and for one reason: it is where a constant
            // written at one lane kind is read at another (see `constantVectorBytes`), and what has
            // to be pooled is the *outer* type - the entry's bytes are the same either way, and the
            // load's type is what decides which instruction reads it.
            if(inst->kind != LowerInst::VecSplat && inst->kind != LowerInst::VecWithLane
               && inst->kind != LowerInst::Bitcast) {
                continue;
            }

            auto result = &((LowerInstSingle*)inst)->result;
            auto type = result->type;
            if(!isVectorLike(type)) continue;

            // Nothing reads it, so there is nothing to point at the pool - and interning a constant
            // no instruction mentions would put bytes in the image for it. The float pass declines
            // the same case for the same reason.
            if(result->uses.isEmpty()) continue;

            /*
             * An intermediate of a longer constant is left alone: its one reader is the next lane
             * write, which is about to be pooled whole, and this link then dies with the rest of the
             * chain. Pooling every link would put `lanes` entries in the image where one is wanted.
             */
            auto onlyReaderExtends = false;

            if(result->uses.size() == 1) {
                auto reader = base[result->uses.get(base, 0)];

                if(reader->kind == LowerInst::VecWithLane &&
                   ((LowerInstVecLane*)reader)->from == (result - base)) {
                    onlyReaderExtends = true;
                }

                // And the same for a bitcast of it, which is the other way a chain continues: the
                // link below is about to be pooled whole at the *outer* type, and pooling this one
                // as well would put two entries in the image and leave the reader reading a bitcast
                // of a load rather than the load.
                if(reader->kind == LowerInst::Bitcast && isVectorLike(reader->created()[0].type)) {
                    onlyReaderExtends = true;
                }
            }

            if(onlyReaderExtends) continue;

            U8 bytes[kMaxVectorBytes] = {};
            auto size = Size(type.byteWidth());
            if(size > kMaxVectorBytes) continue;

            chain.clear();
            if(!constantVectorBytes(base, result, bytes, size, chain)) continue;

            /*
             * The two patterns this machine makes out of nothing, left for the peepholes - §5.7.
             *
             * `pxor r, r` and `pcmpeqd r, r` are one instruction each with no memory, no `.rodata`
             * entry and no general register on the way in, so a load has nothing to offer either.
             * They are left as a *splat of their scalar*, which is the form `selectPackedForm` reads
             * to pick the pseudo - and a chain that happens to be all-zero or all-ones by a route
             * other than a splat is rewritten into one here, so the peephole sees one shape rather
             * than two.
             *
             * Only these two, deliberately. A float sign mask is all-ones shifted, and an abs mask
             * is all-ones shifted the other way - two instructions each, which is *not* obviously
             * better than one load and would need measuring. Guessing is the mistake §5 made in the
             * other direction, and it is not worth making twice.
             */
            auto uniform = [&](U8 value) {
                for(Size at = 0; at < size; at++) if(bytes[at] != value) return false;
                return true;
            };

            if(uniform(0x00) || uniform(0xff)) {
                if(inst->kind != LowerInst::VecSplat) {
                    replaceWithConstantSplat(base, block, i, result, type, uniform(0x00), chain);
                    continue;
                }

                /*
                 * A splat is already the shape the peephole reads - but only if its scalar can be
                 * taken out of allocation, which is what `folded()` means and what
                 * `onlyFeedsMachineSplats` answers. A constant some scalar instruction *also* reads
                 * keeps its register, so this splat has no pseudo to be selected into and would
                 * reach the form as an operand that is folded and placed at once.
                 *
                 * That one takes the pool below like any other constant. It is the same reason
                 * `replaceWithConstantSplat` builds a fresh immediate rather than reusing the
                 * chain's, stated from the other side: the two forms of a shared constant are a
                 * private copy or no pseudo, and only one of them is available here.
                 */
                auto scalar = base[((LowerInstVecSplat*)inst)->from]->inst();

                if(scalar->kind == LowerInst::Imm &&
                   onlyFeedsMachineSplats(base, (LowerImm*)scalar))
                {
                    continue;
                }
            }

            auto global = pooledBytes(ctx, module, bytes, size);
            auto address = new (fun.arena) LowerInstGlobal(StringId(), global - *module.arena);
            auto load = new (fun.arena) LowerInstLoad(
                &address->result - base, result->name, type, U32(size), false
            );

            insertInstAt(base, block, i, address);
            insertInstAt(base, block, i + 1, load);

            replaceAllUses(base, result, &load->result);

            // The outermost link is removed here and cleared from the chain, so the sweep below sees
            // only what fed it.
            removeInst(base, inst);
            for(Size c = 0; c < chain.size(); c++) if(chain[c] == inst) chain[c] = nullptr;

            removeDeadChain(base, chain);

            // The walk resumes at the load. Two were inserted and at least this instruction removed,
            // so the index is restarted from the block rather than adjusted - the chain removal may
            // have taken any number of instructions out from above it.
            i = 0;
        }
    }
}

/*
 * A 256-bit lane pattern that no single shuffle instruction expresses, made into `vpermd`/`vpermps`.
 *
 * **Every shuffle AVX2 has works inside each 128-bit half**, and the one exception moves halves
 * *entire* (`vperm2f128`). So an eight-lane 32-bit pattern like `[0, 4, 1, 5, 2, 6, 3, 7]` - an
 * interleave, and one instruction at four lanes - is not an instruction at this width at all, and
 * `wideShuffleChoice` answered nothing for it.
 *
 * `vpermd` is the general answer and the reason this pass has to exist rather than a form: its
 * pattern is **one lane index per result lane, held in a vector register**. A form cannot produce
 * that, because a form does not create operands - so the pattern has to stop being part of the
 * instruction and become a value, which is a `.rodata` entry, a load, a live range and a register.
 * That is the whole of what happens here, and it is why the note in `wideShuffleChoice` said this
 * needed the vector constant pool: it is the pool that makes a pattern into a value.
 *
 * **One source only.** `vpermd` reads a single vector, so a pattern naming lanes of both sources is
 * left refused - two permutes and a blend would express one, and whether that is worth three
 * instructions and two pooled constants is a question this pass does not answer.
 *
 * The index vector is an **integer** vector at both rows, `vpermps` included: a lane index is a
 * number whatever domain the lanes it indexes are read in. Only the low three bits of each index are
 * read by the instruction, so nothing here has to mask.
 */
static void lowerWideLanePermutes(Context& ctx, LowerBase base, LowerFunction& fun) {
    // The one feature level that has the instruction, and the one that can hold a value wide enough
    // to need it - `targetVectorBytes` answers 32 here and 16 below, so a function compiled without
    // AVX2 has no eight-lane vector for this to be asked about.
    if(!(targetFeatures() & kFeatureAvx2)) return;

    auto& module = *fun.module;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::VecShuffle) continue;

            auto shuffle = (LowerInstVecShuffle*)inst;
            auto type = shuffle->result.type;

            // The one shape with a row: 32-bit lanes filling a 256-bit register. A 64-bit lane's
            // general permute is `vpermq`, whose pattern is an immediate and which therefore belongs
            // to `wideShuffleChoice` rather than here; a narrower one has no crossing permute below
            // AVX-512 at all.
            if(!isWideVector(type) || laneBytes(type.lane) != 4) continue;

            // An instruction already expresses it, which is cheaper by a pooled constant and a
            // register - so this asks the same function the form selection will ask, rather than a
            // restatement of it that could drift.
            if(packedShuffleChoice(inst)) continue;

            auto lanes = type.lanes();
            auto pattern = shuffle->pattern();

            /*
             * Which of the two sources every entry names, and nothing if they name both.
             *
             * The IR numbers the second source's lanes from `lanes` upward, so this is one
             * comparison per entry - and the answer is the operand `vpermd` will read, with the
             * indices then written relative to it.
             */
            auto second = pattern[0] >= lanes;
            auto oneSource = true;

            for(U32 k = 0; k < lanes && oneSource; k++) {
                if((pattern[k] >= lanes) != second) oneSource = false;
            }

            if(!oneSource) continue;

            // The indices, one 32-bit lane each, little-endian like every other entry this pool
            // holds. `vpermd` reads the low three bits of each, so an index relative to the source
            // it belongs to is the whole of what has to be written.
            U8 bytes[kMaxVectorBytes] = {};
            auto size = Size(type.byteWidth());

            for(U32 k = 0; k < lanes; k++) {
                bytes[k * 4] = U8(second ? pattern[k] - lanes : pattern[k]);
            }

            auto indexType = LowerType { LowerLane::Int32, type.laneShift, false };
            auto global = pooledBytes(ctx, module, bytes, size);
            auto address = new (fun.arena) LowerInstGlobal(StringId(), global - *module.arena);
            auto load = new (fun.arena) LowerInstLoad(
                &address->result - base, StringId(), indexType, U32(size), false
            );

            auto source = second ? shuffle->right : shuffle->left;
            auto permute = new (fun.arena) LowerInstX86Permute(
                shuffle->result.name, type, &load->result - base, source
            );

            insertInstAt(base, block, i, address);
            insertInstAt(base, block, i + 1, load);
            insertInstAt(base, block, i + 2, permute);

            replaceAllUses(base, &shuffle->result, &permute->result);
            removeInst(base, inst);

            // Three inserted and one removed, and the walk carries on below the permute - which
            // reads a load this pass has no further business with.
            i += 2;
        }
    }
}

/*
 * §41.6 A vector constant defined above a call it is live across.
 *
 * `sumVectors` builds its zero accumulator at the top of the function and then calls `elements` to
 * get at the array's storage, so the zero is live across a call - and there is no callee-saved
 * vector register on this ABI. What that costs is not a spill and a reload: a 16- or 32-byte slot
 * raises the frame's alignment past what the convention promises, so the function grows a *dynamic*
 * frame - a frame pointer held throughout, `and $-32,%rsp`, and the `leave` that undoes it - all for
 * a value that is one `vpxor` to recreate.
 *
 * The rematerializer would answer this if it could, and it cannot: `%zero` is the incoming arm of
 * the accumulator's phi, so copy coalescing (§17.2) has already made it one web with the phi and the
 * addition, and a web with several definitions has no single recipe that reproduces it. That is not
 * a gap in `recipeFor` - it is the correct answer to the question it was asked.
 *
 * So the definition moves instead. A constant reads nothing, so it may stand anywhere its readers do
 * not precede it, and putting it *below* the call is what makes the live range not cross one at all
 * - no spill, no slot, no alignment, and the same one instruction. The phi is the common case and it
 * needs no reader in this block: a phi's operand is live at the end of the predecessor, so "nothing
 * here reads it" sinks the definition to the bottom of the block, which is exactly where the edge
 * takes it.
 *
 * **Only past a call**, which is what keeps this from being a scheduler. A constant that is already
 * below every call in its block is left where it is: moving it would shorten a live range that costs
 * nothing to hold, and every instruction this pass does not move is one whose position the passes
 * above it chose deliberately - `poolVectorConstants` puts the absolute-value mask at the top of the
 * entry block's successor precisely so that it is *out* of the loop below.
 */
static bool isSinkableVectorConstant(LowerBase base, LowerInst* inst) {
    if(inst->kind != LowerInst::VecSplat) return false;

    auto splat = (LowerInstVecSplat*)inst;
    if(!isVectorLike(splat->result.type)) return false;

    // A splat of a literal, which is the one vector constant that is still an instruction here:
    // everything with more than one distinct lane in it became a `.rodata` load one pass up, and a
    // load's address is a second value that would have to travel with it.
    return base[splat->from]->inst()->kind == LowerInst::Imm;
}

static void sinkVectorConstants(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];
        auto list = block->instructions.contents(base);

        // Nothing to sink past, which is the common block and is answered before anything is built.
        auto hasCall = false;
        for(auto instPtr: list) {
            if(base[instPtr]->kind == LowerInst::Call) { hasCall = true; break; }
        }

        if(!hasCall) continue;

        // What is being moved, and what it is being moved in front of - null for "the end of the
        // block", which is where a value only a phi reads belongs.
        struct Sunk { LowerInst* inst; LowerInst* before; };
        SmallArray<Sunk, 8> sunk;

        for(Size i = 0; i < list.size(); i++) {
            auto inst = base[list[i]];
            if(!isSinkableVectorConstant(base, inst)) continue;

            /*
             * The first reader in this block, and whether a call stands between the two. A reader
             * *above* the definition is impossible in SSA and is not checked for; what is checked is
             * that every position considered is one this instruction may legally occupy, which for
             * something that reads nothing is every position before its first reader.
             */
            LowerInst* before = nullptr;
            auto target = list.size();

            for(Size j = i + 1; j < list.size(); j++) {
                auto reader = base[list[j]];
                auto reads = false;

                for(auto used: reader->used()) {
                    if(base[used]->inst() == inst) { reads = true; break; }
                }

                if(reads) { before = reader; target = j; break; }
            }

            auto crossesCall = false;
            for(Size j = i + 1; j < target; j++) {
                if(base[list[j]]->kind == LowerInst::Call) { crossesCall = true; break; }
            }

            if(!crossesCall) continue;

            /*
             * The immediate the splat reads travels with it where nothing else reads it. Usually it
             * is implicit - a `vsplat 0` selects a form that builds its own zero and the literal has
             * no register at all - but a splat the machine has to build out of a general register
             * would otherwise leave that register live across the call this just moved past.
             */
            auto scalar = base[((LowerInstVecSplat*)inst)->from];
            if(scalar->uses.size() == 1 && scalar->inst()->block == block - base) {
                sunk.push(Sunk { scalar->inst(), before });
            }

            sunk.push(Sunk { inst, before });
        }

        if(sunk.size() == 0) continue;

        // Rebuilt in one walk: everything that stayed, in its own order, with each sunk instruction
        // emitted immediately in front of the reader it was moved to - and the ones with no reader
        // here at the end, in front of the terminator, which is not in this list.
        auto moved = [&](LowerInst* inst) {
            for(auto& entry: sunk) {
                if(entry.inst == inst) return true;
            }

            return false;
        };

        SmallArray<LowerPtr<LowerInst>, 32> rebuilt;

        for(auto instPtr: list) {
            auto inst = base[instPtr];
            if(moved(inst)) continue;

            for(auto& entry: sunk) {
                if(entry.before == inst) rebuilt.push(entry.inst - base);
            }

            rebuilt.push(instPtr);
        }

        for(auto& entry: sunk) {
            if(!entry.before) rebuilt.push(entry.inst - base);
        }

        block->instructions.clear();
        for(auto instPtr: rebuilt) block->instructions.push(fun.arena, instPtr);
    }
}

static void poolFloatConstants(Context& ctx, LowerBase base, LowerFunction& fun) {
    auto& module = *fun.module;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Imm) continue;

            auto imm = (LowerImm*)inst;
            if(!isFloat(imm->result.type)) continue;

            // The IR keeps every float constant as a double, so a single-precision one is rounded to
            // what it will actually be before its bits are taken - the same two lines `emitFloatImm`
            // ran, moved to where the answer is now stored rather than encoded.
            auto is64 = imm->result.type == LowerType::Float64;
            auto size = Size(is64 ? 8 : 4);
            auto bits = U64(0);

            if(is64) {
                auto value = imm->f;
                copyMem(&value, &bits, sizeof(value));
            } else {
                auto value = float(imm->f);
                U32 narrow = 0;
                copyMem(&value, &narrow, sizeof(value));
                bits = narrow;
            }

            /*
             * **A single-precision constant stays an immediate, and that is a measurement.**
             *
             * `mov r32, imm32; movd xmm, r32` is eleven bytes against the load's eight, and on
             * test/bench/programs the three bytes cost 2% of `Float.yana` - 251 ms against 255,
             * reproduced at five different function alignments, so it is the load and not where the
             * loop landed. `escape` is the shape that finds it: a Mandelbrot point usually escapes
             * after a couple of iterations, so the constant is materialized on the entry path far
             * more often than it is used, and a load's latency sits on that path where two ALU
             * operations do not.
             *
             * A double is the same program with one word changed and it comes out the other way:
             * 354 bytes to 329 and no measurable time either way. The immediate form needs
             * `mov r64, imm64` there - ten bytes rather than six - and the four bytes of instruction
             * fetch it saves at the top of a hot function are worth about what the load costs.
             *
             * So the rule is the width, both halves of it measured on one program. It is not the
             * last word: a vector constant has no immediate form at all, and this pass is what will
             * hold it.
             */
            if(!is64) continue;

            // Positive zero stays an immediate: `xorps xmm, xmm` is two bytes where any load is
            // eight, and it needs no general register either - so the pseudo has nothing to lose to
            // here. Negative zero is *not* this, and the bit test rather than a comparison against
            // 0.0 is what says so.
            if(bits == 0) continue;

            // Nothing reads it, so there is nothing to point at the pool. Left where it is: a dead
            // instruction is dropped by the allocator either way, and interning a constant no
            // instruction mentions would put bytes in the image for it.
            if(imm->result.uses.isEmpty()) continue;

            auto global = pooledConstant(ctx, module, bits, size);
            auto address = new (fun.arena) LowerInstGlobal(StringId(), global - *module.arena);
            auto load = new (fun.arena) LowerInstLoad(
                &address->result - base, imm->result.name, imm->result.type, U32(size), false
            );

            insertInstAt(base, block, i, address);
            insertInstAt(base, block, i + 1, load);

            replaceAllUses(base, &imm->result, &load->result);
            removeInst(base, imm);

            // The immediate is gone from the position it held, so the two insertions above net out
            // to one: the walk resumes at the load, and neither of the two is an `Imm`.
            i++;
        }
    }
}

// Chooses the shape of each instruction: which immediates are embedded into the encoding, which
// comparisons stay in the flags, which direct callees need no register, and which of its two
// encodings a block operation takes.
//
// This is where an instruction stops being purely semantic. Every decision here is recorded on the
// instruction - as the Implicit flag, an embedded comparison, or the unrolled flag - so that the
// allocator, the form selection below and the encoder all read one answer instead of each deriving it.
//
// Expects: addresses selected.  Establishes: every value that occupies no location is marked
// Implicit, every Copy/SetPattern has its encoding recorded, and every cast whose extension is a
// no-op is marked as one. Mutates: value flags, instruction annotations, and the order of the
// instructions a compare fold lifts out of its flag window. Invalidates: instruction positions
// within a block.
//
// In two sweeps, and the order between them is the point - load-bearing rather than tidy. Everything
// a peephole can decide about an instruction's *form* is decided first; only then does the compare
// folding walk its windows asking what writes the flags.
//
// Two things need that. Some of the answers a peephole can still change are conservative until it has
// run - an immediate is `xor r, r` until it is embedded - so a comparison looked at first would be
// told that instructions about to disappear stand in its way. And one is not conservative at all: a
// `cast` or `bitcast` of a constant zero becomes the `xor` that materializes it only *once* the
// constant is embedded, so a comparison looked at first would be told that an instruction about to
// start writing the flags does not. Nothing after this pass moves a form's flags effect, which is
// what makes the window the folding cleared still empty when the bytes are written.
/*
 * §42 The mask scan and the branch that guards it, made into one instruction.
 *
 * A search over vectors compiles to two consumers of one movemask in two blocks, which §37 already
 * placed one instruction for:
 *
 *     %bits = pmovmskb %hits          the movemask, placed once
 *     %c    = cmp neq %bits, 0        `any(hits)`
 *     je %c -> hit, miss
 *   hit:
 *     %m    = or %bits, 0x10000       the sentinel, so that `bsf` never sees zero
 *     %f    = bsf %m                  `firstSet(hits)`
 *
 * **Two things are being computed twice here, and the machine computes both of them once.** `bsf`
 * sets ZF exactly when its operand was zero - which is the whole of what the comparison above it
 * asked - and its answer for a *nonzero* operand needs no sentinel, because the sentinel was only
 * ever there to keep the operand from being zero. So the four instructions are two:
 *
 *     %f = bsf %bits ; jne hit
 *
 * Two rewrites, and the second subsumes the first where it applies:
 *
 * - **the sentinel goes** wherever `%bits` is proved nonzero where the scan runs. The proof is the
 *   guard read *locally*, exactly as `isNonNegativeIn` reads one: the scan's block has a single
 *   predecessor, and that predecessor branches here on this mask being nonzero. No dominator tree
 *   and no reasoning about paths - one predecessor is what makes "the branch was taken" true of
 *   every entry to the block.
 * - **the scan moves into the guard's block and the comparison goes**, the branch reading the scan's
 *   own flags. `FormJccLive` is already the form for that - a branch whose condition is a live
 *   register it does not read - so what this needs from the form table is nothing.
 *
 * **The two scans answer the emptiness question in different flags**, which is the one thing here
 * that is not symmetric. `bsf` leaves its destination undefined for a zero operand and says so in
 * ZF; `tzcnt` answers the operand's width and says so in **CF**, ZF meaning something else entirely
 * (that the answer was zero, which is bit zero being set - the opposite of empty). So the condition
 * the branch carries is chosen by which of the two the scan is, and reading ZF off a `tzcnt` would
 * be a search that answered "found" exactly when it had found the mask's *first* lane.
 *
 * Hoisting the scan above the guard is speculation, and the cheapest kind: neither instruction
 * faults, neither touches memory, and the register it writes is dead on the arm that did not want
 * it. What the miss path pays is nothing at all - the `test` it used to run is what the scan
 * replaces.
 */
struct MaskScanGuard {
    LowerInstCmp* compare = nullptr;  // the `any` test, in the guard's block
    LowerInst* scan = nullptr;        // the `bsf`/`tzcnt`, in the guarded block
    LowerInst* sentinel = nullptr;    // the `or` in front of it, where there is one
    LowerValue* bits = nullptr;       // the movemask both of them read
    bool nonzeroIsThen = false;       // whether the guarded block is the branch's `then` arm
};

// The comparison a branch reads, where it is `%bits == 0` or `%bits != 0` and nothing else. The
// constant is asked for by value rather than by `isImm`, which answers "already embedded" and is
// false this early - see the note on it in §37.
static LowerInstCmp* maskEmptinessTest(LowerBase base, LowerInst* terminator) {
    if(terminator->kind != LowerInst::Je) return nullptr;

    auto je = (LowerInstJe*)terminator;
    auto condition = base[je->cond]->inst();
    if(condition->kind != LowerInst::Cmp) return nullptr;

    auto cmp = (LowerInstCmp*)condition;
    if(cmp->getCmp() != LowerCmp::eq && cmp->getCmp() != LowerCmp::neq) return nullptr;
    if(base[cmp->block] != base[terminator->block]) return nullptr;

    /*
     * **The branch has usually already been given this comparison's flags**, and that is not a
     * reason to decline - it is the shape this arrives in.
     *
     * `tryMergeCompare` runs over the *guard's* block in the same walk and reaches it first, blocks
     * being visited in order and a guard standing above what it guards. So by the time this asks,
     * `%c` is implicit and the branch carries its relation. What has to be checked is that the
     * relation is still this comparison's: a branch reading somebody else's flags names a condition
     * that has nothing to do with the value `cond` points at, and rewriting it would be silent.
     */
    auto embedded = je->getEmbeddedCmp();
    if(embedded && embedded.unwrap() != cmp->getCmp()) return nullptr;

    auto rhs = base[cmp->rhs]->inst();
    if(rhs->kind != LowerInst::Imm || ((LowerImm*)rhs)->i != 0) return nullptr;

    return cmp;
}

/*
 * The scan at the top of a guarded block, with the sentinel that may stand in front of it.
 *
 * `bits` names the value it has to read where the caller knows one - a single predecessor's guard
 * tested a value, and a scan of anything else is not this shape - and is null where the caller does
 * not, in which case the scan's own operand is taken and answered back. That second form is what a
 * *join* needs: which value it reads is what says which phi has to be proved nonzero, so the shape
 * is read first and the proof asked for afterwards.
 */
static bool findMaskScan(LowerBase base, LowerBlock* block, LowerValue*& bits, MaskScanGuard& out) {
    auto expected = bits;

    for(auto offset: block->instructions.contents(base)) {
        auto inst = base[offset];

        // A constant is not in the way of anything. The sentinel's own immediate is one of these and
        // is the first instruction of the block in the ordinary case, an `Imm` being placed where it
        // is read; skipping them is what lets this ask about the shape rather than the order.
        if(inst->kind == LowerInst::Imm) continue;

        if(inst->kind == LowerInst::Or) {
            auto binary = (LowerInstBinary*)inst;
            auto rhs = base[binary->rhs]->inst();

            // The sentinel and nothing else: an `or` of *this* mask with a constant, read by one
            // instruction. Anything else in the block is left alone and ends the search, since a
            // scan below it would no longer be the first thing the block does.
            if(rhs->kind != LowerInst::Imm) return false;
            if(expected && base[binary->lhs] != expected) return false;
            if(binary->result.uses.size() != 1) return false;
            if(out.sentinel) return false;

            out.sentinel = inst;
            bits = base[binary->lhs];
            expected = &binary->result;
            continue;
        }

        if(inst->kind != LowerInst::Intrinsic) return false;

        auto which = ((LowerInstIntrinsic*)inst)->getIntrinsic();
        if(which != LowerIntrinsic::Cttz && which != LowerIntrinsic::CttzWidth) return false;

        auto operand = base[inst->used()[0]];
        if(expected && operand != expected) return false;
        if(!out.sentinel) bits = operand;

        out.scan = inst;
        return true;
    }

    return false;
}

// Whether `from` ends in a branch that reaches `block` exactly when `bits` is nonzero, and the
// comparison it reads to do it. Which arm that is depends on the relation: `neq` holds where the
// value is nonzero, so the `then` arm is the nonzero one and `eq` is the other way round.
static LowerInstCmp* branchOnNonzero(LowerBase base, LowerBlock* from, LowerBlock* block,
                                     LowerValue* bits, bool& nonzeroIsThen) {
    auto cmp = maskEmptinessTest(base, base[from->terminator]);
    if(!cmp || base[cmp->lhs] != bits) return nullptr;

    auto je = (LowerInstJe*)base[from->terminator];

    nonzeroIsThen = cmp->getCmp() == LowerCmp::neq;
    if(base[nonzeroIsThen ? je->then : je->otherwise] != block) return nullptr;

    return cmp;
}

/*
 * §52 The same proof through a join: every way in, and not only the one.
 *
 * A search written as a loop and a masked tail branches into one hit block from both, and the bits
 * it scans are the phi of the two arms' bitmaps that `placeJoinedMaskBits` built. What the sentinel
 * is there for is that `bsf` is undefined at zero - and each arm branches here only where its *own*
 * alternative is nonzero, so the phi is nonzero however the block was entered.
 *
 * That is the single-predecessor proof with the quantifier moved and nothing else: it is still read
 * locally, still one branch per edge, and still no dominator tree. What it does not extend to is the
 * fusion below it - the scan cannot stand in two blocks at once, so a join keeps its `test` per arm
 * and loses only the sentinel.
 */
static bool isJoinedNonzero(LowerBase base, LowerBlock* block, LowerValue* bits) {
    auto phi = bits->inst();
    if(!isPhi(phi) || base[phi->block] != block) return false;

    auto sources = ((LowerInstPhi*)phi)->sources();
    if(sources.size() != block->incoming.size()) return false;

    for(Size i = 0; i < sources.size(); i++) {
        auto nonzeroIsThen = false;
        if(!branchOnNonzero(base, base[sources[i]], block, base[phi->used()[i]], nonzeroIsThen)) {
            return false;
        }
    }

    return true;
}

/*
 * The whole shape, recognized from the guarded block.
 *
 * Read from the *guarded* block rather than from the guard, because that is the side the proof is a
 * property of: what makes "the branch was taken" true of every entry to a block is a statement about
 * every edge into it, which the block is where to ask about.
 */
static bool findMaskScanGuard(LowerBase base, LowerBlock* block, MaskScanGuard& out) {
    LowerValue* bits = nullptr;

    if(block->incoming.size() == 1) {
        auto from = base[block->incoming.get(base, 0)];

        // The single predecessor's own test names the value, and `branchOnNonzero` then says whether
        // this block is the arm on which it is the nonzero one.
        auto test = maskEmptinessTest(base, base[from->terminator]);
        if(!test) return false;

        bits = base[test->lhs];

        auto cmp = branchOnNonzero(base, from, block, bits, out.nonzeroIsThen);
        if(!cmp) return false;

        out.compare = cmp;
        out.bits = bits;

        return findMaskScan(base, block, bits, out);
    }

    // A join, where the scan is found first and the phi it reads is what has to be proved - see
    // `isJoinedNonzero`. `compare` stays null, which is what tells the rewrite that there is one
    // guard per edge and no single branch to fold the scan into.
    if(!findMaskScan(base, block, bits, out)) return false;
    if(!isJoinedNonzero(base, block, bits)) return false;

    out.bits = bits;
    return true;
}

// Moves an instruction to just above `into`'s terminator. Only the lists change: every value it
// reads keeps its use and its own result keeps its readers, so nothing outside the two blocks has to
// be told - which is what makes this a placement change rather than a rewrite.
static void moveInstToEndOf(LowerBase base, LowerInst* inst, LowerBlock* into) {
    auto from = base[inst->block];
    auto list = from->instructions.contents(base);

    for(Size i = 0; i < list.size(); i++) {
        if(base[list[i]] == inst) { from->instructions.remove(base, i); break; }
    }

    inst->block = into - base;
    into->instructions.push(base[into->fun]->arena, inst - base);
}

static void fuseMaskScanIntoGuard(LowerBase base, LowerBlock* block) {
    MaskScanGuard found;
    if(!findMaskScanGuard(base, block, found)) return;

    /*
     * The sentinel, removed. `bsf` is undefined at zero and this is the proof that it never sees
     * one; `tzcnt` has no sentinel to begin with, its answer for zero being the width.
     *
     * Done before the fusion below rather than only inside it, because the two have different
     * conditions: this needs the guard alone, and the fusion needs the comparison to have no other
     * reader and the scan to be liftable. A shape that fails the second still gets the first.
     */
    if(found.sentinel) {
        setOperand(base, base[block->fun]->arena, found.scan, found.scan->used()[0], found.bits);
        removeInst(base, found.sentinel);

        auto constant = base[((LowerInstBinary*)found.sentinel)->rhs];
        if(constant->uses.isEmpty()) removeInst(base, constant->inst());
    }

    /*
     * And the fusion, which is the half a *join* does not get: there is one guard per edge and the
     * scan stands in one block, so what a second predecessor would have to read is a scan its own
     * branch does not precede. The sentinel above is the whole of what a join takes.
     */
    if(!found.compare) return;

    /*
     * Three conditions, each of which is a way the branch could stop being the only thing that reads
     * the comparison's answer or the scan could stop being liftable:
     *
     * - the comparison is read by the branch and by nothing else, since it is about to disappear;
     * - the guard's block ends with the branch that reads it, which is what puts the scan's new
     *   position directly in front of the instruction reading its flags - so there is no window to
     *   check, there being nothing between them;
     * - the mask is a register rather than a folded address, so that hoisting the scan above the
     *   guard speculates an instruction and not a load.
     */
    auto guard = base[block->incoming.get(base, 0)];
    auto je = (LowerInstJe*)base[guard->terminator];

    if(found.compare->result.uses.size() != 1) return;
    if(base[found.compare->result.uses.get(base, 0)] != je) return;
    if(base[found.scan->used()[0]]->inst()->kind == LowerInst::X86Address) return;

    // And the scan's own operand, which has to be readable from where the scan is going. Everything
    // else that may stand above it in this block is a constant, which `findMaskScan` skipped and
    // which the scan does not read - so this is the whole of what the move has to be told.
    if(base[base[found.scan->used()[0]]->inst()->block] == block) return;

    moveInstToEndOf(base, found.scan, guard);

    /*
     * Which flag says the mask was empty, and therefore which condition the branch carries.
     *
     * `bsf` sets ZF for a zero operand, so "nonzero" is `neq`. `tzcnt` sets **CF** for one - ZF on a
     * `tzcnt` means the answer was zero, which is the mask's first lane being set - so "nonzero" is
     * CF clear, which is the unsigned `ge` the encoder writes as `jae`.
     */
    auto width = ((LowerInstIntrinsic*)found.scan)->getIntrinsic() == LowerIntrinsic::CttzWidth;
    auto nonzero = width ? LowerCmp::ge : LowerCmp::neq;
    auto empty = width ? LowerCmp::lt : LowerCmp::eq;

    auto scanned = &found.scan->created()[0];
    replaceUse(base, &found.compare->result, je, scanned);
    je->cond = scanned - base;
    je->setEmbeddedCmp(Just(found.nonzeroIsThen ? nonzero : empty));

    removeInst(base, found.compare);

    auto constant = base[found.compare->rhs];
    if(constant->uses.isEmpty()) removeInst(base, constant->inst());
}

static void selectMachineInstructions(Context&, LowerBase base, LowerFunction& fun) {
    forEachInst(base, fun, [&](LowerInst* inst, Size i) {
        if(inst->kind == LowerInst::Imm) {
            tryEmbedImm(base, (LowerImm*)inst);
        }

        if(inst->kind == LowerInst::Fun) {
            tryElideDirectCallee(base, (LowerInstFun*)inst);
        }

        if(inst->kind == LowerInst::Global) {
            tryFoldGlobalAddress(base, (LowerInstGlobal*)inst);
        }

        // After tryEmbedImm rather than in a sweep of its own: whether a constant source has been
        // taken out of its register is what decides whether a cast is a move at all, and an Imm is
        // reached before the instructions that read it.
        if(inst->kind == LowerInst::Cast) {
            trySkipCastExtend(base, (LowerInstCast*)inst);
        }

        selectBlockOpEncoding(base, inst);
    });

    // Walked by index rather than through forEachInst, because a fold that lifts an instruction out
    // of its window moves the comparison down the list by exactly that many places. Skipping past
    // them is right as well as necessary: what was lifted is never itself a comparison.
    for(auto b: fun.blocks.contents(base)) {
        auto block = base[b];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Cmp) continue;

            i += tryMergeCompare(base, (LowerInstCmp*)inst, i);
        }

        // Behind the loop, because a comparison this block ends on is the branch's condition and the
        // fold above is the better answer for it: that one removes the materialization as well.
        // What is left here is a branch on a value nothing compared.
        tryElideBranchTest(base, block);

        /*
         * And the mask scan, which is the same finding one block over: the guard and the scan it
         * guards are two computations of the emptiness of one movemask, and the machine answers both
         * with the scan's own flags.
         *
         * Here rather than beside `lowerVectorReductions`, which is where the shape is *built*,
         * because the fused branch reads flags that the instruction directly above it left - and
         * only this late is "directly above it" a statement no later pass can invalidate. It is the
         * same argument `tryBranchOnLiveCompare` makes about its window, arriving from the other
         * side: that one measures a window, this one leaves none.
         */
        fuseMaskScanIntoGuard(base, block);
    }
}

// Turns a call's stack-passed arguments into explicit stores into the outgoing argument area, placed
// as early as is safe - see the block comment on outgoing stack arguments above.
//
// Expects: machine instructions selected, so that an argument the passes above made implicit is
// already implicit when its location is decided.  Establishes: no call operand is passed on the
// stack; every one of them is an X86PushArg result instead. Mutates: the instruction lists and the
// affected use lists. Invalidates: instruction positions within a block.
static void lowerOutgoingStackArguments(Context&, LowerBase base, LowerFunction& fun) {
    insertStackArgs(base, fun, targetConstraints());
}

// Splits every edge on which a phi transfer needs an insertion point of its own.
//
// Expects: no pass that reasons about instruction positions left to run.  Establishes: no block with
// two successors has a successor with phis, so a phi copy at the end of a predecessor cannot run on
// a path that skips the phis. Mutates: the block list and the CFG. Invalidates: block indices.
static void normalizePhiEdges(Context&, LowerBase base, LowerFunction& fun) {
    splitPhiEdges(base, fun);
}

// Finds the loops and rewrites the block list into the reverse postorder that follows them and the
// branch probabilities - see the block-order comment above.
//
// Expects: the CFG in its final shape, since the edge probabilities it lays the blocks out by are
// read from it. Establishes: blocks in reverse postorder with the likely successor of each branch
// immediately behind it, `index` equal to list position, and `loopDepth` set. Mutates: the block
// list order and block metadata. Invalidates: nothing after it.
static void analyzeLoopsAndOrderBlocks(Context&, LowerBase base, LowerFunction& fun) {
    orderBlocks(base, fun);
}

/*
 * The sign mask a float negation needs, interned into the pool above.
 *
 * `xorps xmm, [rip + m]` is what a negation is on this machine, and it was not writable before the
 * pool existed: §13.8 records the old form as three instructions, a general register the form had to
 * declare as a clobber, and a bank crossing in each direction, taken because a sixteen-byte constant
 * had nowhere to live. That is now one instruction, no general register, and - the part that reaches
 * further than the negation itself - **no flags effect**, where `btc` clobbered them. A comparison's
 * fold window may now hold a negation.
 *
 * Sixteen bytes rather than four or eight because `xorps` reads its memory operand as a whole
 * register and faults on an unaligned one. `addGlobal` puts every pooled entry on a sixteen-byte
 * boundary, so the alignment is already right; the size is what keeps the read inside the entry.
 */
static void poolSignMasks(Context& ctx, LowerBase base, LowerFunction& fun, MachineFunction& machine) {
    forEachInst(base, fun, [&](LowerInst* inst, Size) {
        if(inst->kind != LowerInst::Neg) return;

        auto type = ((LowerInstUnary*)inst)->result.type;
        if(type == LowerType::Float64 && !machine.signMask64) {
            machine.signMask64 = pooledConstant(ctx, *fun.module, U64(1) << 63, 16);
        } else if(type == LowerType::Float32 && !machine.signMask32) {
            machine.signMask32 = pooledConstant(ctx, *fun.module, 0x8000000080000000ull, 16);
        }
    });
}

// Records, for every instruction, the machine opcode and the machine form it was selected into - see
// machine.h. Everything downstream reads its facts from there: which operands are forced into
// particular registers, what the instruction clobbers, which result is written over which operand,
// which operand may stay in a frame slot, what it does to the flags.
//
// Last, and not where §4.3 of the plan puts it, for one reason: an instruction cannot be given a
// form before it exists, and two passes above create instructions - the argument stores, and the
// jumps in the blocks that phi-edge splitting inserts. The peepholes still make every decision the
// form depends on; this pass only writes the answer down.
//
// Expects: no pass left that creates instructions or changes an instruction's shape.  Establishes: a
// selected form for every instruction in the function. Mutates: nothing in the IR.
static void selectMachineForms(LowerBase base, LowerFunction& fun, MachineFunction& machine) {
    auto select = [&](LowerInst* inst) {
        machine.select(inst, opcodeFor(base, inst), selectForm(base, inst), selectCondition(inst));
    };

    for(auto a: fun.args.contents(base)) select((LowerInst*)base[a]);

    for(auto b: fun.blocks.contents(base)) {
        auto block = base[b];

        for(auto p: block->phis.contents(base)) select(base[p]);
        for(auto i: block->instructions.contents(base)) select(base[i]);
        select(base[block->terminator]);
    }
}

/*
 * Pipeline invariants.
 *
 * Checked between passes in debug builds. The structural ones are what the mutating passes can
 * actually break: inserting an instruction, removing a dead one, moving a use from one value to
 * another and splitting an edge all have to keep four separate lists agreeing with each other, and a
 * stale entry in any of them is invisible until the allocator reads it and concludes that a dead
 * value is live - a wrong answer several passes away from its cause.
 */

enum TransformInvariant: U32 {
    // Every pass establishes this one: instruction lists, use lists and CFG links agree.
    InvariantStructure = 1 << 0,

    // No block with two successors has a successor with phis.
    InvariantPhiEdgesNormalized = 1 << 1,

    // Block list position and BlockIndex agree.
    InvariantBlocksOrdered = 1 << 2,
};

struct TransformPass {
    StringView name;
    void (*run)(Context& ctx, LowerBase base, LowerFunction& fun);

    // What holds once this pass has run, and holds for every pass after it.
    U32 establishes;

    /*
     * Whether this pass has anything to look for in a function with no packed value in it.
     *
     * Half the table is vector work, and the overwhelming majority of functions this backend
     * compiles - every one in the embedded library, for a start - contain not one packed type. Each
     * of those passes still walks every block and every instruction of every one of them to ask a
     * question whose answer is decided by the function's types. So the question is asked once, in
     * `transformFunction`, and the ten that read a packed type are skipped outright when it says no.
     *
     * The claim that makes this sound is that nothing above such a pass *creates* a packed value in
     * a function that had none: the vector work is all lowering of vectors that were already there.
     * That is checked rather than asserted in prose - see the debug check at the end of the pipeline.
     */
    bool vectorsOnly = false;
};

static const TransformPass kTransformPipeline[] = {
    { "rotateLoops"_v,                 rotateLoops,                 0 },
    { "expandBankConversions"_v,   expandBankConversions,   0 },
    // After nothing, since what it reads is only the multiply-add itself. It used to have to run
    // before the two lane passes as well, the tree it builds ending in a lane extract each of them
    // might rewrite; both went with the sub-v2 machines that needed them.
    // Not vectors-only, which is worth saying because it sits between two passes that are: a fused
    // multiply-add is a *float* instruction and a scalar one is as much an Fma as a packed one, so a
    // machine without FMA3 needs this pass to reach a function with no packed value in it at all.
    { "expandFusedMultiplyAdd"_v,      expandFusedMultiplyAdd,      0 },
    { "lowerVectorReductions"_v,       lowerVectorReductions,       0, true },

    /*
     * Above `poolVectorConstants`, which is what turns the mask it builds into a `.rodata` entry the
     * `andps` reads out of memory, and above the two passes below - both of which rewrite a select,
     * and this one has to see the absolute value's before either has taken it for something else.
     *
     * Nothing else constrains it: what it reads is a select over a float vector, and no pass above
     * it produces or consumes one.
     */
    { "expandVectorAbs"_v,             expandVectorAbs,             0, true },

    // **Above `poolVectorConstants`**, which is what the constants it reads have to survive: this
    // asks `constantVectorBytes` for the bytes of a `vsplat`/`vwithlane` chain, and that pass turns
    // one into a `.rodata` load. Above `selectPackedMinMax` as well, so that what the minimum is
    // handed is the load rather than the blend that was standing in front of it.
    { "foldConstantMasks"_v,           foldConstantMasks,           0, true },

    // After the reduction, every level of whose min/max tree is exactly the compare-and-select this
    // recognizes, and **before `biasUnsignedPackedCompares`**, which rewrites an unsigned comparison
    // into a signed one over two exclusive-ors: what reaches this has to be the relation the program
    // asked for, since the signedness of the comparison is what picks `pminsd` over `pminud`.
    { "selectPackedMinMax"_v,          selectPackedMinMax,          0, true },

    // After both passes that read a select for something else - the minimum takes the pair whose
    // arms are the compared values, and this takes what is left. **Above `poolVectorConstants`**,
    // for the reason `foldConstantMasks` is: the zero arm it recognizes is a `vsplat` chain
    // until that pass turns it into a `.rodata` load.
    { "selectMaskedVectors"_v,         selectMaskedVectors,         0, true },

    // After the reduction, whose unsigned minimum and maximum are comparisons this then biases, and
    // before canonicalizeOperands, which is what exchanges the signed relations it produces.
    { "biasUnsignedPackedCompares"_v,  biasUnsignedPackedCompares,  0, true },

    // Above `poolVectorConstants`, which is the whole of where this may sit - the count it reads is
    // a `vsplat` of a constant, and that pass turns one into a `.rodata` load.
    { "unwrapVectorShiftCounts"_v,     unwrapVectorShiftCounts,     0, true },

    /*
     * **Above `selectMemorySources`**, which is what `poolFloatConstants` argues: `foldLoads` runs
     * below, so a pooled constant with one reader lands in that reader's addressing mode rather than
     * in a register (§5.4.1's memory twin, and only under VEX - a legacy packed operation faults on
     * an unaligned memory operand, so at v2 the load stands).
     *
     * It also had to run before `lowerLaneInserts`, which rewrote every `vwithlane` of a constant
     * chain into a shift and a `pinsrw` pair and so hid the chain from this pass - `iota` came out
     * one instruction longer. That pass is gone: `pinsrd` is v2.
     */
    { "poolVectorConstants"_v,         poolVectorConstants,         0, true },

    // Below `poolVectorConstants` and not above it: what this builds is already a `.rodata` load, so
    // there is nothing for that pass to find - and putting it above would hand that pass an index
    // vector to walk for no reason. It has to be above `selectMemorySources` for the reason every
    // pass that builds a load does.
    { "lowerWideLanePermutes"_v,       lowerWideLanePermutes,       0, true },

    // Directly behind it, so that what is left standing as an instruction is the set this can move:
    // everything with more than one distinct lane became a `.rodata` load one line up, and a load
    // has an address that would have to travel with it.
    { "sinkVectorConstants"_v,         sinkVectorConstants,         0, true },

    { "canonicalizeOperands"_v,        canonicalizeOperands,        0 },
    { "selectStoreUpdates"_v,          selectStoreUpdates,          0 },
    { "selectAddressesAndLeas"_v,      selectAddressesAndLeas,      0 },
    { "poolFloatConstants"_v,          poolFloatConstants,          0 },
    { "selectMemorySources"_v,         selectMemorySources,         0 },
    { "selectMachineInstructions"_v,   selectMachineInstructions,   0 },
    { "lowerOutgoingStackArguments"_v, lowerOutgoingStackArguments, 0 },
    { "normalizePhiEdges"_v,           normalizePhiEdges,           InvariantPhiEdgesNormalized },
    { "analyzeLoopsAndOrderBlocks"_v,  analyzeLoopsAndOrderBlocks,  InvariantBlocksOrdered },
};

// Every instruction the function owns, in no particular order: the arguments, then each block's
// phis, instructions and terminator. Arguments and phis are not in any block's instruction list but
// do contribute uses, so a check that ignored them would report every one of theirs as stale.
template<class F>
static void forEachOwnedInst(LowerBase base, LowerFunction& fun, F&& f) {
    for(auto a: fun.args.contents(base)) f((LowerInst*)base[a]);

    for(auto b: fun.blocks.contents(base)) {
        auto block = base[b];

        for(auto p: block->phis.contents(base)) f(base[p]);
        for(auto i: block->instructions.contents(base)) f(base[i]);
        if(block->terminator) f(base[block->terminator]);
    }
}

/*
 * Whether any value in this function is a packed one - see TransformPass::vectorsOnly.
 *
 * Over every value the function owns rather than over its instruction lists, because a vector can
 * arrive as an argument or be merged by a phi without any instruction between them producing one.
 * The results are what is asked about and not the operands: every operand is some instruction's
 * result, an argument or a phi, and all three are visited here.
 */
static bool functionHasVectors(LowerBase base, LowerFunction& fun) {
    auto found = false;

    forEachOwnedInst(base, fun, [&](LowerInst* inst) {
        for(auto& created: inst->created()) {
            if(isVectorLike(created.type)) found = true;
        }
    });

    return found;
}

static bool verifyTransformInvariants(Context& ctx, LowerBase base, LowerFunction& fun, U32 established) {
    auto funName = ctx.findName(fun.name);
    auto ok = true;

    auto fail = [&](auto&& fmt, auto&&... args) {
        ok = false;
        logError(fmt, forward<decltype(args)>(args)...);
    };

    // How many times each value is read, counted from the operand lists. Compared afterwards against
    // the value's own use list, which is the direction that catches a use entry left behind by a
    // removed instruction.
    HashMap<LowerValue*, U32> reads;

    forEachOwnedInst(base, fun, [&](LowerInst* inst) {
        for(auto offset: inst->used()) {
            auto v = base[offset];
            auto count = reads.get(v);
            if(count.isJust()) count.unwrap()++;
            else reads.add(v, 1);
        }
    });

    for(auto b: fun.blocks.contents(base)) {
        auto block = base[b];

        if(!block->terminator) {
            fail("%@: block %@ has no terminator", funName, U32(block->index));
            continue;
        }

        // An instruction whose `block` names somewhere it is not listed is one that a move or an
        // insertion left behind, and every later pass that walks from the block would miss it.
        auto ownedBy = [&](LowerInst* inst) {
            if(base[inst->block] != block) {
                fail("%@: block %@ lists an instruction whose own block is %@",
                    funName, U32(block->index), U32(base[inst->block]->index));
            }
        };

        for(auto p: block->phis.contents(base)) ownedBy(base[p]);
        for(auto i: block->instructions.contents(base)) ownedBy(base[i]);
        ownedBy(base[block->terminator]);

        // Successor and predecessor lists are two records of one edge, and a pass that updates only
        // one of them produces a CFG the liveness and the layout disagree about.
        for(auto o: block->outgoing) {
            if(!o) continue;

            bool found = false;
            for(auto p: base[o]->incoming.contents(base)) {
                if(base[p] == block) { found = true; break; }
            }

            if(!found) {
                fail("%@: block %@ names block %@ as a successor, which does not name it back",
                    funName, U32(block->index), U32(base[o]->index));
            }

            if((established & InvariantPhiEdgesNormalized) &&
               block->outgoing[0] && block->outgoing[1] && base[o]->phis.isNotEmpty())
            {
                fail("%@: block %@ has two successors and block %@ has phis",
                    funName, U32(block->index), U32(base[o]->index));
            }
        }

        // Edge likelihood survives every CFG transform, or the layout and the frequencies are
        // reasoning about a branch that no longer exists. Splitting an edge is the case that could
        // lose one - it retargets `then` or `otherwise` - and what would show here is a branch that
        // came out with a weight on one edge and nothing on the other, which is not a ratio.
        if(base[block->terminator]->kind == LowerInst::Je) {
            auto je = (LowerInstJe*)base[block->terminator];

            for(auto& likelihood: je->likelihood) {
                auto stated = likelihood.source != LikelihoodSource::Unknown;

                if(stated != je->hasLikelihood()) {
                    fail("%@: branch in block %@ states an edge weight for one edge only",
                        funName, U32(block->index));
                }

                if(likelihood.weight < 1 || likelihood.weight > kMaxEdgeWeight) {
                    fail("%@: branch in block %@ has an edge weight out of range",
                        funName, U32(block->index));
                }
            }
        }

        // A phi takes one value per predecessor, from a block that is actually one.
        for(auto p: block->phis.contents(base)) {
            auto phi = base[p];
            auto sources = phi->sources();

            if(sources.size() != phi->used().size()) {
                fail("%@: phi in block %@ has %@ sources for %@ operands",
                    funName, U32(block->index), U32(sources.size()), U32(phi->used().size()));
            }

            for(auto source: sources) {
                bool found = false;
                for(auto in: block->incoming.contents(base)) {
                    if(in == source) { found = true; break; }
                }

                if(!found) {
                    fail("%@: phi in block %@ takes a value from block %@, which is not a predecessor",
                        funName, U32(block->index), U32(base[source]->index));
                }
            }
        }
    }

    if(established & InvariantBlocksOrdered) {
        auto blocks = fun.blocks.contents(base);

        for(Size i = 0; i < blocks.size(); i++) {
            if(base[blocks[i]]->index != BlockIndex(i)) {
                fail("%@: block at position %@ is numbered %@",
                    funName, U32(i), U32(base[blocks[i]]->index));
            }
        }
    }

    // The other direction: a use list that claims more or fewer readers than there are.
    forEachOwnedInst(base, fun, [&](LowerInst* inst) {
        for(auto& created: inst->created()) {
            auto counted = reads.get(&created);
            auto expected = counted.isJust() ? counted.unwrap() : 0;

            if(created.uses.size() != expected) {
                fail("%@: a value's use list has %@ entries for %@ actual readers",
                    funName, U32(created.uses.size()), expected);
            }
        }
    });

    return ok;
}

void transformFunction(Context& ctx, LowerBase base, LowerFunction& fun, MachineFunction& machine) {
    // The narrowest point every path through this backend passes, which is why the target's feature
    // set is established here: form selection is asked about a form from a dozen places that have an
    // instruction and no settings, so the answer is process-wide rather than carried. See
    // targetFeatures in target.h.
    setTargetFeatures(x64FeaturesFor(ctx.settings));

    // Asked here because this is the first thing the backend does to a function and the question is
    // about the IR as it arrives - so a frame this backend cannot build is a diagnostic against the
    // program rather than something the frame builder discovers with the code half emitted. See
    // checkFrameSupported; the pipeline still runs, since a reported error stops emission anyway and
    // a half-transformed function is worse to reason about than a whole one.
    checkFrameSupported(ctx, base, fun, targetConstraints());

    // And the same question about the vector operations, at the same point and for the same reason -
    // see checkVectorSupported. It has to stand after setTargetFeatures, since which forms exist is
    // a function of the feature set this build claims.
    checkVectorSupported(ctx, base, fun);

    U32 established = 0;

    // Asked once, here, rather than discovered by each of the ten vector passes walking the whole
    // function to find nothing - see TransformPass::vectorsOnly.
    auto vectors = functionHasVectors(base, fun);

    for(auto& pass: kTransformPipeline) {
        if(pass.vectorsOnly && !vectors) continue;

        pass.run(ctx, base, fun);
        established |= pass.establishes;

        // Debug builds only - assertTrue compiles away entirely in a release build, taking the call
        // with it. Running between passes rather than once at the end is the point: it names the
        // pass that broke the invariant rather than the pipeline that ended up violating it.
        assertTrue(verifyTransformInvariants(ctx, base, fun, established | InvariantStructure));
    }

    // What the skip above assumes, stated where it can fail loudly: a function with no packed value
    // in it does not acquire one on the way through, so the ten passes that were skipped had nothing
    // to do rather than something they were not shown. Debug builds only, like every check here.
    assertTrue(vectors || !functionHasVectors(base, fun));

    // Beside the form selection rather than in the pipeline, for the same reason: it writes on the
    // MachineFunction instead of on the IR. Before it rather than after only so that the two facts a
    // negation needs - its form and its mask - are settled together.
    poolSignMasks(ctx, base, fun, machine);

    // Writes down what the passes above decided. Separate from the pipeline table because it
    // produces the MachineFunction rather than mutating the IR, and because it has to see every
    // instruction the passes above created.
    selectMachineForms(base, fun, machine);

    // The first of the boundary checks, at the boundary it belongs to: everything after this reads
    // the selection rather than the instructions, so a form that does not match the instruction it
    // was chosen for is a wrong answer nothing downstream can notice.
    assertTrue(verifySelection(ctx, base, fun, machine));
}
