#pragma once

#include "gen.h"
#include "x64_util.h"
#include "../../lower/lower_fold.h"

// For `setOperand`, which is how a rewritten operand keeps the use lists agreeing with it.
#include "../../lower/lower_builder.h"

/*
 * The transform pipeline, split across eight files, and what each of them answers.
 *
 * `transformFunction` and the pipeline table are in transform.cpp; everything below is a pass it
 * names or a helper one of those passes is built out of. Read this list before hunting for a
 * function - the file a pass lives in is the stage of the pipeline it belongs to, not the order it
 * happens to have been written in.
 *
 *   transform.cpp            the pipeline: the pass table, the order and the reasons for it, the
 *                            invariant verifier, and the two passes that are nothing but a walk -
 *                            the machine-form selection and the outgoing stack arguments.
 *   transform_edit.cpp       the edits every pass here makes to the IR: inserting and removing an
 *                            instruction, retargeting a use, splitting a critical edge, and where a
 *                            value an expansion needs everywhere is allowed to go.
 *   transform_peephole.cpp   the decisions about one instruction's *shape*: which immediates are
 *                            embedded, which callee needs no register, which cast extends nothing,
 *                            which comparison stays in the flags, and the sign-extend peephole.
 *   transform_address.cpp    `base + index*scale + disp` recognized once, as a folded addressing
 *                            mode, an `lea`, a memory-source operand, or an in-place update.
 *   transform_loop.cpp       loop rotation, the preheaders it needs, and the block order that
 *                            spends it.
 *   transform_expand.cpp     operations the machine has no instruction for, written out as IR: the
 *                            unsigned bank conversions, the packed shifts and multiplies it lacks
 *                            at some lane width, the scalarized lanes, `round` and `abs`.
 *   transform_reduce.cpp     a vector reduced to a scalar, the movemask every mask consumer shares,
 *                            and the scan-and-guard the two of them fuse into.
 *   transform_constant.cpp   the constant pool and what enters it: a float, a vector, a sign mask -
 *                            plus the vector constants that are recognized rather than pooled.
 *
 * Everything below is what one of those files needs from another. A name that is not here is that
 * file's own business and stays `static` in it.
 */

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
 * transform_edit.cpp - the edits.
 *
 * Declared first because `Expansion` below is built out of them, and because everything else here
 * is a pass rather than a primitive.
 */

// Inserts `inst` into `block`'s instruction list at `at`, shifting what follows up one.
void insertInstAt(LowerBase base, LowerBlock* block, Size at, LowerInst* inst);

// Takes an instruction nothing reads any more out of its block, and with it the uses it contributed.
void removeInst(LowerBase base, LowerInst* inst);

// Points one use of `from` in `user` at `to` instead, and moves the entry in the use lists with it.
void replaceUse(LowerBase base, LowerValue* from, LowerInst* user, LowerValue* to);

// The same for every reader of `from`.
void replaceAllUses(LowerBase base, LowerValue* from, LowerValue* to);

// Moves an instruction to just above `into`'s terminator.
void moveInstToEndOf(LowerBase base, LowerInst* inst, LowerBlock* into);

// Where an instruction sits in its own block, or nothing if it is not in one.
Maybe<Size> positionOf(LowerBase base, LowerBlock* block, LowerInst* inst);

// Where a constant an expansion needs everywhere goes - see the comment on the definition.
LowerBlock* constantHome(LowerBase base, LowerFunction& fun);

// Gives every phi transfer a block it can safely be emitted in, by splitting each critical edge.
void splitPhiEdges(LowerBase base, LowerFunction& fun);
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

    // A read of `width` bytes from an address, at whatever type the caller wants those bytes as.
    // Unsigned, since nothing here loads a narrow value in order to widen it - a block transfer's
    // bytes are moved rather than interpreted.
    LowerValue* load(LowerType type, LowerValue* from, U32 width, StringId name = StringId()) {
        return emit(new (fun.arena) LowerInstLoad(from - base, name, type, width, false));
    }

    // And a write of them, which produces nothing and so cannot go through `emit`.
    void store(LowerValue* to, LowerValue* value, U32 width) {
        insertInstAt(base, block, at++, new (fun.arena) LowerInstStore(to - base, value - base, width));
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

/*
 * transform_block.cpp - block operations with a compile-time size, written out as loads and stores.
 *
 * Above `selectAddressesAndLeas`, which is what turns the offsets it emits into addressing modes,
 * and above `poolVectorConstants`, which is what turns a fill's constant splat into a `.rodata`
 * entry. See the header of that file for why this is a pass rather than an encoding.
 */
void expandBlockOperations(Context& ctx, LowerBase base, LowerFunction& fun);

/*
 * transform_peephole.cpp - the shape of one instruction.
 *
 * Each `try` answers whether it applied, which is what lets the selection walk in transform.cpp
 * state the whole sweep as a list rather than as a nest of conditions.
 */
bool tryEmbedImm(LowerBase base, LowerImm* imm);
bool tryElideDirectCallee(LowerBase base, LowerInstFun* fun);
bool tryFoldGlobalAddress(LowerBase base, LowerInstGlobal* global);
bool trySkipCastExtend(LowerBase base, LowerInstCast* cast);
bool trySwapOperands(LowerBase base, LowerInst* inst);
bool orderFloatCompare(LowerBase base, LowerInst* inst);
bool orderPackedCompare(LowerBase base, LowerInst* inst);

// The compare folding's two entry points - see §3.5.2 beside them.
Size tryMergeCompare(LowerBase base, LowerInstCmp* cmp, Size index);
void tryElideBranchTest(LowerBase base, LowerBlock* block);

// Whether every use of this constant is a splat the machine builds out of nothing - §5.7. Read by
// the vector-constant pooling, which must not pool what needs no pool entry.
bool onlyFeedsMachineSplats(LowerBase base, LowerImm* imm);

// The pass: the BMI pair-replacements - `andn`, the three lowest-bit operations, and the left
// rotation rewritten as the right one so that `rorx` is reachable. See §3.5.4 beside it.
void selectBitOps(Context&, LowerBase base, LowerFunction& fun);

// The pass: `x << k >> k` put back as one `movsx`.
void selectSignExtends(Context&, LowerBase base, LowerFunction& fun);

/*
 * transform_address.cpp - the four memory folds, in the order transform.cpp runs them.
 */
void foldAddresses(LowerBase base, LowerFunction& fun);
void foldLeas(LowerBase base, LowerFunction& fun);
void foldLoads(LowerBase base, LowerFunction& fun);
void foldStoreUpdates(LowerBase base, LowerFunction& fun);

// Folds a load into the byte reversal that reads it, and a reversal into the store that writes it -
// `movbe`, where the target has it. See the header above the pass.
void selectByteSwapMemory(LowerBase base, LowerFunction& fun);

/*
 * transform_loop.cpp - the CFG shaping, and the layout that spends it.
 */
void rotateFunctionLoops(LowerBase base, LowerFunction& fun);
void orderBlocks(LowerBase base, LowerFunction& fun);

// A phi this file emptied, taken back out - defined with the rotation's own phi helpers, and read
// by the reductions because a joined mask leaves one behind.
bool dropUnusedPhi(LowerBase base, LowerBlock* block, LowerInstPhi*& phi);

/*
 * transform_expand.cpp - what the machine has no instruction for.
 */
void expandBankConversions(Context&, LowerBase base, LowerFunction& fun);
void unwrapVectorShiftCounts(Context&, LowerBase base, LowerFunction& fun);
void expandQuadwordSar(Context&, LowerBase base, LowerFunction& fun);
void expandByteShifts(Context&, LowerBase base, LowerFunction& fun);
void expandByteMul(Context&, LowerBase base, LowerFunction& fun);
void expandQuadwordMul(Context&, LowerBase base, LowerFunction& fun);
void expandVectorMulHi(Context&, LowerBase base, LowerFunction& fun);
void scalarizeVectorLanes(Context&, LowerBase base, LowerFunction& fun);
void expandFusedMultiplyAdd(Context&, LowerBase base, LowerFunction& fun);
void biasUnsignedPackedCompares(Context&, LowerBase base, LowerFunction& fun);
void expandRoundAway(Context&, LowerBase base, LowerFunction& fun);
void expandBitScans(Context&, LowerBase base, LowerFunction& fun);

// The three operations BMI2 has an instruction for and the floor does not - `bitsUpTo` and the two
// directions of a bit permutation. See the header above the pass.
void expandBitOperations(Context&, LowerBase base, LowerFunction& fun);
void expandVectorRotate(Context&, LowerBase base, LowerFunction& fun);
void expandVectorAbs(Context&, LowerBase base, LowerFunction& fun);

/*
 * transform_reduce.cpp - a vector answered as a scalar.
 */
void lowerVectorReductions(Context&, LowerBase base, LowerFunction& fun);
void fuseMaskScanIntoGuard(LowerBase base, LowerBlock* block);

/*
 * transform_constant.cpp - the pool, and the vector constants that need none.
 */
void selectPackedMinMax(Context&, LowerBase base, LowerFunction& fun);
void foldConstantMasks(Context&, LowerBase base, LowerFunction& fun);
void selectMaskedVectors(Context&, LowerBase base, LowerFunction& fun);
void poolVectorConstants(Context& ctx, LowerBase base, LowerFunction& fun);
void lowerWideLanePermutes(Context& ctx, LowerBase base, LowerFunction& fun);
void sinkVectorConstants(Context&, LowerBase base, LowerFunction& fun);
void poolFloatConstants(Context& ctx, LowerBase base, LowerFunction& fun);

// One read-only global per distinct constant, which the sign masks enter by hand.
LowerGlobal* pooledConstant(Context& ctx, LowerModule& module, U64 bits, Size size);

/*
 * The two readers of a constant vector chain, declared here because the passes that call them are
 * spread across three files above their definitions.
 *
 * Both live beside the constant pooling they were written for: the bytes a `vsplat`/`vwithlane`
 * chain comes to, and the sweep that takes such a chain back out once nothing reads it.
 */
bool constantVectorBytes(LowerBase base, LowerValue* value, U8* bytes, Size size, InstChain& chain);
void removeDeadChain(LowerBase base, InstChain& chain);
