#include "gen.h"
#include "x64_util.h"

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

// Tries to embed this immediate into any instructions that use it.
static bool tryEmbedImm(LowerBase base, LowerImm* imm) {
    if(!isEmbeddableImm(imm)) return false;

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
static bool isNonNegative32(LowerBase base, LowerValue* value, U32 depth = kExtendedDepth) {
    auto inst = value->inst();
    if(value->type != LowerType::Int32) return false;

    switch(inst->kind) {
        case LowerInst::Imm:
            return (((LowerImm*)inst)->i & 0x80000000ull) == 0;

        // One operand without the bit is enough, since masking cannot set a bit neither side has.
        case LowerInst::And: {
            if(depth == 0) return false;

            auto binary = (LowerInstBinary*)inst;
            return isNonNegative32(base, base[binary->lhs], depth - 1)
                || isNonNegative32(base, base[binary->rhs], depth - 1);
        }

        // Where masking can, so both sides have to be clear.
        case LowerInst::Or: case LowerInst::Xor: {
            if(depth == 0) return false;

            auto binary = (LowerInstBinary*)inst;
            return isNonNegative32(base, base[binary->lhs], depth - 1)
                && isNonNegative32(base, base[binary->rhs], depth - 1);
        }

        // A logical shift down by a known distance clears exactly that many bits at the top, so any
        // distance at all clears bit 31. The arithmetic one fills from the sign bit and clears
        // nothing; the shift up is the carrying case this declines outright.
        case LowerInst::Shr: {
            auto count = base[((LowerInstBinary*)inst)->rhs];
            return count->inst()->kind == LowerInst::Imm && immValue(count) >= 1;
        }

        case LowerInst::Cmp:
            return !isImplicit(value);

        // Fewer than four bytes cannot reach bit 31 whichever way the load extends; four bytes can,
        // and a signed narrow load carries its own sign bit up into it.
        case LowerInst::Load: {
            auto load = (LowerInstLoad*)inst;
            return load->getWidth() < 4 && !load->isSigned();
        }

        // A widening from something narrower, unsigned: the bits it fills are zeros, and bit 31 is
        // one of them. A same-width cast is a rename and answers for whatever it renamed.
        case LowerInst::Cast: {
            auto cast = (LowerInstCast*)inst;
            auto source = base[cast->from];
            if(!isIntLike(source->type)) return false;

            if(source->type == LowerType::Int32) {
                return depth > 0 && isNonNegative32(base, source, depth - 1);
            }

            return !cast->isSignedSource();
        }

        default:
            return false;
    }
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

    if(!isZeroExtended(base, source)) return false;

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

// Whether this comparison's result is of a shape that could be left in the flags at all, before
// anything is asked about what stands between it and the things that read it.
static bool canCarryInFlags(LowerBase base, LowerInstCmp* cmp) {
    /*
     * A floating-point equality is not a condition code, so it cannot be carried in the flags.
     *
     * UCOMISS answers "equal" in ZF and "unordered" in PF, and both are set at once by a NaN - so
     * ordered equality is `ZF and not PF` and inequality is `not ZF or PF`. Neither is a `setcc` or
     * a `jcc`; each is two of them and a combining step, which is what genFloatFlagsToReg emits into
     * a register. Refusing the fold here is what guarantees it gets the chance: a branch then tests
     * that register rather than reading flags that cannot say what it needs.
     *
     * The ordering comparisons are not affected - canonicalizeOperands has already put them all in
     * the `gt`/`ge` form, where CF alone is the answer and a NaN makes it false.
     */
    if(isFloat(base[cmp->lhs]->type)) {
        auto kind = cmp->getCmp();
        if(kind == LowerCmp::eq || kind == LowerCmp::neq) return false;
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

// Carries a comparison into the branches and selects that read it, so that its answer stays in the
// flags rather than being materialized. Returns how many instructions were lifted above it to make
// that possible, which is how far its own position in the block moved down.
static Size tryMergeCompare(LowerBase base, LowerInstCmp* cmp, Size index) {
    auto& uses = cmp->result.uses;

    if(uses.size() == 0) {
        cmp->result.flags |= LowerValue::Implicit;
        return 0;
    }

    if(!canCarryInFlags(base, cmp)) return 0;

    auto end = flagsWindowEnd(base, cmp, index);
    if(end.isNothing()) return 0;

    Size hoisted = 0;
    if(!clearFlagsWindow(base, cmp, index, end.unwrap(), hoisted)) return 0;

    // The only uses are instructions that can use flags directly, and nothing writes them in
    // between any more, so the result can stay as flags.
    cmp->result.flags |= LowerValue::Implicit;

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
// each re-deriving the choice and risking disagreement. The unrolled form is only viable for a
// compile-time byte count small enough to be worth straight-lining; everything else takes the
// rep-prefixed string instruction, which needs its operands in fixed registers.
static bool isUnrolledCount(LowerBase base, LowerPtr<LowerValue> count) {
    auto value = base[count];
    if(value->inst()->kind != LowerInst::Imm) return false;

    return ((LowerImm*)value->inst())->i <= kMaxUnrolledMemOp;
}

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

static void splitPhiEdges(LowerBase base, LowerFunction& fun) {
    // Snapshotted because splitting appends to the block list, and a freshly created split block
    // has a single successor and so can never itself need splitting.
    SmallArray<LowerPtr<LowerBlock>, 64> original;
    for(auto b: fun.blocks.contents(base)) original.push(b);

    for(auto offset: original) {
        auto pred = base[offset];

        // Only a block with two successors can reach a phi on a path it might not take.
        if(!pred->outgoing[0] || !pred->outgoing[1]) continue;

        for(Size edge = 0; edge < 2; edge++) {
            if(base[pred->outgoing[edge]]->phis.isNotEmpty()) splitEdge(base, fun, pred, edge);
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
    Array<LowerInst*> users;
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

// A float truncated into an unsigned integer.
static LowerValue* expandFloatToUnsigned(Expansion& e, LowerValue* value, LowerType to, StringId name) {
    if(to == LowerType::Int32) {
        auto wide = e.convert(LowerType::Int64, value, false, true);
        return e.convert(to, wide, false, false, name);
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
    return e.select(LowerType::Int64, isBig, flipped, small, name);
}

static void expandUnsignedConversions(LowerBase base, LowerFunction& fun) {
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
            auto unsignedConversion = (toFloat && !cast->isSignedSource())
                || (fromFloat && !cast->isSignedResult());

            if(!unsignedConversion) { i++; continue; }

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
 *  - **The load has to be the instruction immediately above.** That is what makes the motion free of
 *    any question about what may have written memory in between - nothing runs between the two - and
 *    it is what leaves the address where it has to be: an X86Address sits immediately in front of
 *    the access that folds it, and taking the load out from between them is what puts it in front of
 *    the consumer instead.
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

// Whether exchanging this operation's operands leaves it computing the same thing. The same set
// trySwapOperands uses, and restricted to the integer bank for the same reason: a float addition is
// commutative in value but not in which NaN payload the machine propagates.
static bool isCommutativeInt(LowerInst* inst) {
    if(!isBinary(inst) || !isIntLike(((LowerInstBinary*)inst)->result.type)) return false;

    switch(inst->kind) {
        case LowerInst::Add:
        case LowerInst::Mul:
        case LowerInst::IMul:
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
            return true;
        default:
            return false;
    }
}

// The bytes a value of this type occupies, which is the access a load performs when it extends
// nothing. Every scalar the lowering produces is four bytes or eight, which is exactly the
// distinction a slot class already makes.
static U32 accessWidthOf(LowerType type) {
    return stackSlotClassFor(type) == StackSlotClass::Slot32 ? 4 : 8;
}

// Whether this form requires an operand in a particular register, which is the copy a folded address
// cannot survive - see the third bound above.
static bool hasFixedOperands(const MachineForm& form) {
    for(auto& constraint: form.uses) {
        if(constraint.kind == OperandConstraintKind::FixedRegister) return true;
    }

    return false;
}

// Folds the load above `index` into the instruction at it. Answers where that instruction ended up,
// or Nothing when it was left alone - in which case nothing has been changed at all: the operand
// exchange a commutative operation may need is made at the end, with every question already
// answered, so that a fold which does not happen leaves no trace of having been considered.
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
    auto above = base[block->instructions.get(base, index - 1)];
    if(above->kind != LowerInst::Load) return Nothing();

    auto load = (LowerInstLoad*)above;
    auto value = &load->result;

    // One reader, and it has to be this instruction. `add %v, %v` reads it in both positions and
    // only one of them can be the address, which a use count of one already excludes.
    if(isImplicit(value) || value->uses.size() != 1) return Nothing();

    auto used = inst->used();
    auto at = used.size();
    for(Size i = 0; i < used.size(); i++) {
        if(base[used[i]] == value) at = i;
    }

    // Which operand holds it has to be the one the encoding can dereference, or an operand a
    // commutative operation can exchange into it - which is the shape `arr[i] + sum` arrives in.
    auto exchange = at != memory;
    if(exchange && !(isCommutativeInt(inst) && used.size() == 2 && at < used.size())) return Nothing();

    // The bytes the encoding reads are the bytes the load read, unextended.
    if(load->getWidth() != accessWidthOf(value->type)) return Nothing();
    if(stackSlotClassFor(value->type) != stackSlotClassFor(operationType(base, twin, inst))) return Nothing();

    auto address = base[load->from];

    if(isMem(address)) {
        // Where the address fold put it, which is what removing the load turns into "immediately
        // above the consumer". Checked rather than assumed: an address anywhere else would reach the
        // encoder holding whatever the instructions in between had left in its registers.
        if(index < 2 || base[block->instructions.get(base, index - 2)] != address->inst()) return Nothing();
    } else if(isImplicit(address)) {
        // A pointer the encoding swallowed has no register for an address to be built around.
        return Nothing();
    }

    /*
     * Committed from here: everything below changes the function.
     */

    if(exchange) ::swap(((LowerInstBinary*)inst)->lhs, ((LowerInstBinary*)inst)->rhs);

    if(!isMem(address)) {
        // A pointer that reached the load in a register becomes `[reg]`, so that the operand says
        // what it is without a flag beside it.
        auto computed = new (fun.arena) LowerInstX86Address(
            LowerInst::X86Address, StringId(), address - base, nullptr, 1, 0
        );

        insertInstAt(base, block, index - 1, computed);
        address = &computed->result;
        index++;
    }

    replaceUse(base, value, inst, address);
    inst->used()[memory] = address - base;
    removeInst(base, load);

    return Just(index - 1);
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
        new (&created[i]) LowerValue(clone, created[i].type, StringId());
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

// An unattached phi with room for `count` alternatives - filled in and added to a block by the
// caller, the way lower_promote.cpp's builds one, because adding it is what registers its reads.
static LowerInstPhi* makeRotationPhi(Region<LowerRegion>& arena, LowerType type, Size count) {
    auto storage = arena.alloc(
        sizeof(LowerInstPhi) +
        sizeof(LowerPtr<LowerValue>) * count +
        sizeof(LowerPtr<LowerBlock>) * count);

    auto phi = new (storage) LowerInstPhi(StringId(), type);
    phi->usedCount = U8(count);
    return phi;
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

    auto phi = makeRotationPhi(arena, old->result.type, count + 1);
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
 */
static bool terminalExit(LowerBase base, const LoopInfo& loops, U32 headerIndex, LowerBlock* block) {
    if(block->outgoing[0] || block->outgoing[1]) return false;

    for(auto p: block->incoming.contents(base)) {
        if(!loops.contains(headerIndex, base[p]->index)) return false;
    }

    return true;
}

static Maybe<RotatableLoop> rotatableLoop(LowerBase base, LowerFunction& fun, const LoopInfo& loops,
                                          LowerBlock* header)
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

    // The preheader has to be a block whose whole purpose is to enter the loop, since its jump is
    // what becomes the guard; and the two blocks that gain the preheader as a predecessor have to
    // have had only the header, or a phi in either would need alternatives this cannot supply.
    if(base[loop.pre->terminator]->kind != LowerInst::Jmp) return Nothing();
    if(loop.body->incoming.size() != 1) return Nothing();
    if(loop.exit->incoming.size() != 1) return Nothing();

    // The header has to be the only way out, or every other way out has to be one that goes nowhere
    // - see `terminalExit`, which is what makes a header value read at one still have somewhere to
    // merge.
    for(auto o: fun.blocks.contents(base)) {
        auto block = base[o];
        if(block == header || !loops.contains(index, block->index)) continue;

        for(auto s: block->outgoing) {
            if(!s || loops.contains(index, base[s]->index)) continue;
            if(!terminalExit(base, loops, index, base[s])) return Nothing();
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

static void rotateLoop(LowerBase base, LowerFunction& fun, const LoopInfo& loops, const RotatableLoop& loop) {
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
        r.body = makeRotationPhi(arena, r.header->result.type, 2);
        r.exit = makeRotationPhi(arena, r.header->result.type, 2);
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
     * A terminal second exit reads on an edge that leaves the loop and is nevertheless the *body's*
     * answer: every way into such a block is from inside the loop, so the body dominates it, and the
     * exit's merge - which is what the guard's zero-iteration answer arrives through - is a value it
     * was never reached by. See `terminalExit`.
     */
    for(auto& r: phis) {
        auto value = &r.header->result;

        Array<LowerInst*> users;
        for(auto u: value->uses.contents(base)) users.push(base[u]);

        for(auto user: users) {
            auto used = user->used();

            for(Size slot = 0; slot < used.size(); slot++) {
                if(base[used[slot]] != value) continue;

                auto from = user->kind == LowerInst::Phi
                    ? base[((LowerInstPhi*)user)->sources()[slot]]
                    : base[user->block];

                auto inLoop = loops.contains(headerIndex, from->index) ||
                              terminalExit(base, loops, headerIndex, from);

                auto to = from == header ? r.hdr
                    : inLoop ? &r.body->result
                    : &r.exit->result;

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

static void rotateFunctionLoops(LowerBase base, LowerFunction& fun) {
    auto loops = fun.buildLoops(base);

    // Snapshotted, because rotating one loop is what stops its header from being one. Which blocks a
    // loop *contains* is what everything below asks, and that is what rotation leaves alone: no
    // block is created or renumbered, and the body it moves the entry to was already a member.
    SmallArray<LowerPtr<LowerBlock>, 16> headers;
    for(auto o: fun.blocks.contents(base)) {
        if(loops.isHeader(base[o]->index)) headers.push(o);
    }

    for(auto o: headers) {
        if(auto loop = rotatableLoop(base, fun, loops, base[o])) {
            rotateLoop(base, fun, loops, loop.unwrap());
        }
    }
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
static void rotateLoops(LowerBase base, LowerFunction& fun) {
    rotateFunctionLoops(base, fun);
}

// Moves operands into the canonical position for the passes below: an immediate onto the right-hand
// side of a commutative operation, so that nothing downstream has to look at both sides, and a
// floating-point `lt`/`le` exchanged into the `gt`/`ge` this machine can answer for a NaN.
// Representation-neutral: no target register or encoding decision is made here.
//
// Expects: the lowering's output, unmodified.  Establishes: commutative immediates on the right, and
// no float comparison below. Mutates: operand order and the comparison an instruction carries.
// Invalidates: nothing.
static void canonicalizeOperands(LowerBase base, LowerFunction& fun) {
    forEachInst(base, fun, [&](LowerInst* inst, Size i) {
        trySwapOperands(base, inst);
        orderFloatCompare(base, inst);
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
static void selectAddressesAndLeas(LowerBase base, LowerFunction& fun) {
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
static void selectMemorySources(LowerBase base, LowerFunction& fun) {
    foldLoads(base, fun);
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
static void selectMachineInstructions(LowerBase base, LowerFunction& fun) {
    forEachInst(base, fun, [&](LowerInst* inst, Size i) {
        if(inst->kind == LowerInst::Imm) {
            tryEmbedImm(base, (LowerImm*)inst);
        }

        if(inst->kind == LowerInst::Fun) {
            tryElideDirectCallee(base, (LowerInstFun*)inst);
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
    }
}

// Turns a call's stack-passed arguments into explicit stores into the outgoing argument area, placed
// as early as is safe - see the block comment on outgoing stack arguments above.
//
// Expects: machine instructions selected, so that an argument the passes above made implicit is
// already implicit when its location is decided.  Establishes: no call operand is passed on the
// stack; every one of them is an X86PushArg result instead. Mutates: the instruction lists and the
// affected use lists. Invalidates: instruction positions within a block.
static void lowerOutgoingStackArguments(LowerBase base, LowerFunction& fun) {
    insertStackArgs(base, fun, targetConstraints());
}

// Splits every edge on which a phi transfer needs an insertion point of its own.
//
// Expects: no pass that reasons about instruction positions left to run.  Establishes: no block with
// two successors has a successor with phis, so a phi copy at the end of a predecessor cannot run on
// a path that skips the phis. Mutates: the block list and the CFG. Invalidates: block indices.
static void normalizePhiEdges(LowerBase base, LowerFunction& fun) {
    splitPhiEdges(base, fun);
}

// Finds the loops and rewrites the block list into the reverse postorder that follows them and the
// branch probabilities - see the block-order comment above.
//
// Expects: the CFG in its final shape, since the edge probabilities it lays the blocks out by are
// read from it. Establishes: blocks in reverse postorder with the likely successor of each branch
// immediately behind it, `index` equal to list position, and `loopDepth` set. Mutates: the block
// list order and block metadata. Invalidates: nothing after it.
static void analyzeLoopsAndOrderBlocks(LowerBase base, LowerFunction& fun) {
    orderBlocks(base, fun);
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
    void (*run)(LowerBase base, LowerFunction& fun);

    // What holds once this pass has run, and holds for every pass after it.
    U32 establishes;
};

static const TransformPass kTransformPipeline[] = {
    { "rotateLoops"_v,                 rotateLoops,                 0 },
    { "expandUnsignedConversions"_v,   expandUnsignedConversions,   0 },
    { "canonicalizeOperands"_v,        canonicalizeOperands,        0 },
    { "selectAddressesAndLeas"_v,      selectAddressesAndLeas,      0 },
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
    // Asked here because this is the first thing the backend does to a function and the question is
    // about the IR as it arrives - so a frame this backend cannot build is a diagnostic against the
    // program rather than something the frame builder discovers with the code half emitted. See
    // checkFrameSupported; the pipeline still runs, since a reported error stops emission anyway and
    // a half-transformed function is worse to reason about than a whole one.
    checkFrameSupported(ctx, base, fun, targetConstraints());

    U32 established = 0;

    for(auto& pass: kTransformPipeline) {
        pass.run(base, fun);
        established |= pass.establishes;

        // Debug builds only - assertTrue compiles away entirely in a release build, taking the call
        // with it. Running between passes rather than once at the end is the point: it names the
        // pass that broke the invariant rather than the pipeline that ended up violating it.
        assertTrue(verifyTransformInvariants(ctx, base, fun, established | InvariantStructure));
    }

    // Writes down what the passes above decided. Separate from the pipeline table because it
    // produces the MachineFunction rather than mutating the IR, and because it has to see every
    // instruction the passes above created.
    selectMachineForms(base, fun, machine);

    // The first of the boundary checks, at the boundary it belongs to: everything after this reads
    // the selection rather than the instructions, so a form that does not match the instruction it
    // was chosen for is a wrong answer nothing downstream can notice.
    assertTrue(verifySelection(ctx, base, fun, machine));
}
