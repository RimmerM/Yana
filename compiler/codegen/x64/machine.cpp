#include "machine_internal.h"

/*
 * The AMD64 form table's spine.
 *
 * The opcode names, and the order the table is built in. Every form itself is registered by one of
 * the machine_forms_*.cpp files - see machine_internal.h, which is the map.
 */

MachineTarget::MachineTarget() {
    for(Size i = 0; i < kMachineOpcodeCount; i++) opcodes[i].id = MachineOpcodeId(i);

    MachineFormBuilder b(*this);

    // The opcode names, which are what a form table dump and a verifier message say.
    auto name = [&](MachineOpcodeId id, StringView text, bool flagsSelective = false) {
        b.name(id, text, flagsSelective);
    };

    name(OpNone, "none"_v);
    name(OpNop, "nop"_v);
    name(OpArg, "arg"_v);
    name(OpPhi, "phi"_v);

    // An immediate of zero is materialized with `xor r, r` rather than `mov r, 0` - two bytes
    // instead of five, at the cost of writing the flags, which the move does not. Which of the two
    // it is depends on the value alone and on nothing any peephole decides, which is what makes it
    // safe for the compare folding to ask this question while those passes are still running.
    name(OpImm, "imm"_v, true);

    name(OpGlobalAddress, "globaladdr"_v);
    name(OpFunctionAddress, "funaddr"_v);
    name(OpMove, "move"_v);

    // A cast whose source is an embedded constant is a materialization, and takes the same two forms
    // a materialization does: `xor r, r` for zero and `mov r, imm` for everything else. Which of the
    // two follows the constant's value alone, exactly as it does for OpImm above.
    name(OpCast, "cast"_v, true);

    // Not flags-selective, and that is the point of it standing apart from OpCast: `movsx` writes no
    // flags at any width, where the shift pair it replaces writes them at all of them.
    name(OpSext, "sext"_v);

    // And a bitcast of one, for the same reason - and this is the pair that pays: `bitcast 0` is
    // what the lowering makes of every null pointer, where a cast of a constant is folded away
    // before it is ever built (foldCast in lower_builder.h) and only a hand-written .lower file
    // has one.
    //
    // Both are the first flags-selective opcodes whose answer moves the *wrong* way as the peepholes
    // run - the form that writes nothing is the one they start in. What makes that safe is the sweep
    // order rather than anything about these rows; see MachineOpcodeDesc::flagsSelective and §3.5.2
    // of the README.
    name(OpBitcast, "bitcast"_v, true);
    name(OpNeg, "neg"_v);
    name(OpNot, "not"_v);
    name(OpBswap, "bswap"_v);
    name(OpAdd, "add"_v);
    name(OpSub, "sub"_v);
    name(OpMul, "mul"_v);
    name(OpIMul, "imul"_v);
    name(OpDiv, "div"_v);
    name(OpIDiv, "idiv"_v);
    name(OpRem, "rem"_v);
    name(OpIRem, "irem"_v);
    name(OpMulHi, "mulhi"_v);
    name(OpIMulHi, "imulhi"_v);
    name(OpShl, "shl"_v);
    name(OpShr, "shr"_v);
    name(OpSar, "sar"_v);
    name(OpAnd, "and"_v);
    name(OpOr, "or"_v);
    name(OpXor, "xor"_v);
    // A comparison against zero whose answer the arithmetic above it already put in ZF emits
    // nothing and writes no flags, where every other form of this opcode writes them - so the two
    // do differ. Unlike the four selective opcodes above, which a *peephole* decides, this one is
    // decided by the compare folding itself, in the second sweep, after the last question anything
    // asks about a form's flags effect. See §3.5.2.2 of the README.
    name(OpCmp, "cmp"_v, true);

    // The packed set. None of them touches the flags at any lane type, which is what makes them the
    // one group here with nothing to declare: a comparison can be folded across a whole vector loop.
    name(OpVAdd, "vadd"_v);
    name(OpVSub, "vsub"_v);
    name(OpVMul, "vmul"_v);
    name(OpVMulHi, "vmulhi"_v);
    name(OpVIMulHi, "vimulhi"_v);
    name(OpVMulWide, "vmulwide"_v);
    name(OpVIMulWide, "vimulwide"_v);
    name(OpVDiv, "vdiv"_v);
    name(OpVAnd, "vand"_v);
    name(OpVOr, "vor"_v);
    name(OpVXor, "vxor"_v);
    name(OpVAndNot, "vandnot"_v);
    name(OpVShl, "vshl"_v);
    name(OpVShr, "vshr"_v);
    name(OpVSar, "vsar"_v);
    name(OpVCmp, "vcmp"_v);
    name(OpVAbs, "vabs"_v);
    name(OpVMin, "vmin"_v);
    name(OpVMax, "vmax"_v);
    name(OpVShuffle, "vshuffle"_v);
    name(OpVPermute, "vpermute"_v);
    name(OpVBroadcast, "vbroadcast"_v);
    name(OpVExtract, "vextract"_v);
    name(OpVMaskBits, "vmaskbits"_v);
    name(OpVInsert, "vinsert"_v);
    name(OpVBlend, "vblend"_v);
    name(OpVNot, "vnot"_v);
    name(OpVNeg, "vneg"_v);
    name(OpSqrt, "sqrt"_v);
    name(OpRound, "round"_v);
    name(OpFma, "fma"_v);
    name(OpVZeroUpper, "vzeroupper"_v);

    name(OpFAdd, "fadd"_v);
    name(OpFSub, "fsub"_v);
    name(OpFMul, "fmul"_v);
    name(OpFDiv, "fdiv"_v);
    name(OpFNeg, "fneg"_v);
    name(OpFCmp, "fcmp"_v);

    // A select whose condition arrives in a register tests it first, and that test writes the flags;
    // one whose condition is already in the flags reads them and writes nothing.
    name(OpSelect, "select"_v, true);

    // A compile-time size is one `lea` and touches nothing; a run-time one rounds the size up and
    // moves the stack pointer, which writes the flags. Which of the two applies follows the count
    // being an embedded constant, so this is one of the opcodes whose selection a peephole moves -
    // see MachineOpcodeDesc::flagsSelective for why that is still safe.
    name(OpAlloca, "alloca"_v, true);

    name(OpLoad, "load"_v);
    name(OpStore, "store"_v);
    name(OpMovbeLoad, "movbeload"_v);
    name(OpMovbeStore, "movbestore"_v);

    // The in-place updates, which write the flags at every form they have - the arithmetic they
    // perform is the same arithmetic wherever its destination lives.
    name(OpStoreAdd, "storeadd"_v);
    name(OpStoreSub, "storesub"_v);
    name(OpStoreAnd, "storeand"_v);
    name(OpStoreOr, "storeor"_v);
    name(OpStoreXor, "storexor"_v);
    name(OpBlockCopy, "blockcopy"_v);
    name(OpBlockSet, "blockset"_v);
    name(OpCall, "call"_v);
    name(OpPushArg, "pusharg"_v);
    name(OpAddress, "address"_v);
    name(OpLea, "lea"_v);
    name(OpJmp, "jmp"_v);

    // As with the select above: a branch on a register tests it, a branch on the flags does not.
    name(OpJcc, "jcc"_v, true);

    name(OpRet, "ret"_v);

    // The end of a block control never leaves. Named like any other opcode so that the printers and
    // the verifiers have something to say about it, and encoding to nothing at all - see FormNoReturn.
    name(OpNoReturn, "noreturn"_v);

    b.registerScalarForms();
    b.registerPackedForms();
    b.registerWideForms();
    b.registerMemoryAndControlForms();
    b.registerVexTier();

    // The intrinsics' forms go into the same table, after the described ones, so that everything
    // downstream asks an intrinsic the same questions it asks an `add` - see intrinsic.cpp.
    addIntrinsics(*this);

    assertTrue(validateMachineForms(*this));
    assertTrue(validateIntrinsics(*this));
}

const MachineTarget& machineTarget() {
    static MachineTarget target;
    return target;
}
