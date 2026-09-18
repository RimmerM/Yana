#include "transform_internal.h"

// For `nameForInst`, which is what a refusal names the instruction by.
#include "../../lower/lower_print.h"

/*
 * An operation on a 512-bit vector that has no 512-bit form, written as two 256-bit halves -
 * Analysis-Avx512.md §1.2's stage 2, and step 10 of its order of work.
 *
 * At `avx512` the natural width is 64 bytes, and most of what a library loop does at that width is
 * one instruction: the arithmetic, the bitwise operations, the comparisons into a mask register, the
 * select, the splat, the loads and stores, the permutes and the masked transfers all have a 512-bit
 * form (`registerFullForms`). What does not is the long tail - a byte shift by a count per lane, a
 * lane read or written by index, a reduction of a vector, a conversion with an expansion of its own -
 * and each of those already has a 256-bit answer that this backend has built, tested and measured. So rather than a second 512-bit expansion of each, the
 * operation is run twice at 256 bits on the two halves of its operands and the answers joined:
 * `vextracti64x4` for a high half, the register itself read at 256 bits for a low one, and
 * `vinserti64x4` to join (FormVFullLow and its siblings). Everything below this pass then sees
 * 256-bit instructions it already knows, which is the point of running it this early.
 *
 * **Correct first, and fast where it is measured to matter.** A split operation costs its two halves
 * and up to three crossings, which is more than the 256-bit loop would have spent; `fullWidthNative`
 * is the list that decides what stays whole, and moving a kind onto it is how a measured loss is
 * repaired - see test/bench/avx512/findings.md §10.
 *
 * A value read by several split instructions is split once, right below its definition, so the halves
 * dominate every reader; a join this pass wrote is read back as the two halves it was built from
 * rather than split again, which is what keeps a chain of split operations in 256-bit registers from
 * one end to the other. A splat's halves are two narrower splats of the same scalar, and a constant's
 * are a width-changing shuffle of it that `poolVectorConstants` folds into a constant of its own.
 */

namespace {

// A vector or a mask of a whole 512-bit register - what this pass is about, and nothing scalar.
bool isFull(LowerType type) {
    return isVectorLike(type) && isFullVector(type);
}

bool touchesFull(LowerBase base, LowerInst* inst) {
    for(auto& value: inst->created()) {
        if(isFull(value.type)) return true;
    }

    for(auto operand: inst->used()) {
        if(isFull(base[operand]->type)) return true;
    }

    return false;
}

LowerType halfOf(LowerType type) {
    return LowerType { type.lane, U8(type.laneShift - 1), type.isMask() };
}

/*
 * Whether an instruction over a 512-bit vector is kept whole - the one list this pass is, and the one
 * place a kind moves from "two halves" to "one instruction" when a 512-bit form of it is built.
 *
 * Each answer is a statement that every pass below this one takes the instruction at 512 bits as it
 * takes it at 256, and that `selectPackedForm` finds a `full` twin at the end of the row it reads. A
 * kind that is here and has no form is an assertion in `widthForm`, not a wrong answer.
 */
bool fullWidthNative(LowerBase base, LowerInst* inst) {
    auto resultType = inst->createdCount == 1 ? inst->created()[0].type : LowerType {};
    auto lane = laneBytes(resultType.lane);

    switch(inst->kind) {
        // Transfers and calls: the register class's own moves, whatever is in it.
        case LowerInst::Arg:
        case LowerInst::Load:
        case LowerInst::Store:
        case LowerInst::Copy:
        case LowerInst::Phi:
        case LowerInst::Call:
        case LowerInst::Ret:
        case LowerInst::Set:
        case LowerInst::Bitcast:
        case LowerInst::VecLoadUpTo:
        case LowerInst::VecStoreUpTo:
            return true;

        // A lane-wise operation at every lane width, with a 512-bit row at each.
        case LowerInst::Add:
        case LowerInst::Sub:
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
        case LowerInst::Not:
        case LowerInst::Neg:
        case LowerInst::Abs:
        case LowerInst::Div:
        case LowerInst::Sqrt:
        case LowerInst::Trunc:
        case LowerInst::Floor:
        case LowerInst::Ceil:
        case LowerInst::Fma:
        case LowerInst::Cmp:
        case LowerInst::Select:
        case LowerInst::VecSplat:
            return true;

        /*
         * The byte lane is the one these are expansions at rather than rows, and the expansions are
         * written in terms of the type, so they run at 512 bits as they stand: a byte product is two
         * word products and a blend (`expandByteMul`), and a byte shift by a count every lane shares
         * is a word shift and a mask (`expandByteShifts`). What is left to the halves is a byte shift
         * by a count per lane, which is scalarized a lane at a time and is cheaper as two runs of
         * thirty-two, and a byte rotation.
         */
        case LowerInst::Mul:
        case LowerInst::IMul:
            return true;

        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar: {
            if(lane != 1) return true;
            auto count = base[((LowerInstBinary*)inst)->rhs];
            return !isVectorLike(count->type) || count->inst()->kind == LowerInst::VecSplat;
        }

        case LowerInst::Rol:
        case LowerInst::Ror:
            return lane != 1;

        // `vpmulhuw`/`vpmulhw`, at the one width the machine has a high product.
        case LowerInst::MulHi:
        case LowerInst::IMulHi:
            return lane == 2;

        // A reduction of a mask is a question about a mask register's bits, which `kmovq` and
        // `kortestq` answer whole. One of a vector folds its halves together first.
        case LowerInst::VecReduce:
            return base[((LowerInstVecReduce*)inst)->from]->type.isMask();

        // A vector literal is a chain of these over a splat, which `poolVectorConstants` turns into
        // one 64-byte `.rodata` entry; any other lane written by index is a lane of one half.
        case LowerInst::VecWithLane:
            return isPooledVectorConstant(base, &((LowerInstVecLane*)inst)->result);

        // Any pattern is one `vpermi2*` at `avx512`, and a width-changing one is a slot either way.
        case LowerInst::VecShuffle:
            return true;

        // `vpcompress*`/`vpexpand*` at every lane, and a lookup is `vpermi2*` - over the table itself
        // where it is as wide as its indices, and over the table repeated to 512 bits where it is
        // narrower (`lookupRepeated`). Every lookup the library writes reads a table of 128 or 256
        // bits, whose lane count divides the indices'; one that does not is read a half at a time.
        case LowerInst::VecMove: {
            auto move = (LowerInstVecMove*)inst;
            if(move->getMove() != LowerLaneMove::Lookup) return true;

            auto table = base[move->source]->type;
            auto control = base[move->control]->type;
            if(table.lanes() == control.lanes()) return true;
            return isFullVector(control) && (table.byteWidth() == 16 || table.byteWidth() == 32)
                && laneBytes(table.lane) == laneBytes(control.lane);
        }

        /*
         * A conversion that keeps its lanes and their width - `vcvtdq2ps` and `vcvttps2dq`, and
         * `vcvtqq2pd`. Every other one changes the register width at one end or goes through an
         * expansion of its own, and runs on the halves.
         */
        case LowerInst::Cast: {
            auto source = base[((LowerInstCast*)inst)->from]->type;
            if(!isVectorLike(source) || source.lanes() != resultType.lanes()) return false;

            // An integer lane doubled or halved, with the 512-bit register at the wide end - one
            // `vpmovzx`/`vpmovsx` or `vpmov{wb,dw,qd}` (FormVFullZeroExtend8).
            if(isIntVector(source) && isIntVector(resultType)) {
                if(lane == 2 * laneBytes(source.lane) && isFullVector(resultType)) return true;
                if(2 * lane == laneBytes(source.lane) && isFullVector(source)) return true;
            }

            if(laneBytes(source.lane) != lane) return false;

            auto floats = isFloatVector(source) + isFloatVector(resultType);
            if(floats != 1) return false;
            return lane == 4 || (lane == 8 && isFloatVector(resultType) && hasEvexRow(EvexRow::QuadwordToDouble));
        }

        default:
            return false;
    }
}

struct Split {
    LowerBase base;
    LowerFunction& fun;

    struct Halves {
        LowerValue* whole;
        LowerValue* low;
        LowerValue* high;
    };

    // The halves each value has been split into so far, so a second reader shares them.
    SmallArray<Halves, 16> made;

    // Where the halves of `value` go: right below its definition, and below every phi of its block
    // if it is one, so that they dominate whatever reads the value.
    Expansion belowDefinition(LowerValue* value) {
        auto inst = value->inst();

        // An argument is defined outside every block, and the entry block holds no instructions of
        // its own - so its halves go at the top of the block the entry jumps to, which every path
        // through the function passes through as well (`constantHome`).
        if(inst->kind == LowerInst::Arg) {
            auto home = constantHome(base, fun);
            assertTrue(home != nullptr); // an entry block that does not jump straight to its successor

            return Expansion { base, fun, home, 0 };
        }

        // A phi is in its block's own list rather than among the instructions, so everything in the
        // instruction list is below it.
        auto block = base[inst->block];
        if(inst->kind == LowerInst::Phi) return Expansion { base, fun, block, 0 };

        auto at = positionOf(base, block, inst).unwrap() + 1;
        return Expansion { base, fun, block, at };
    }

    // The value's two halves, made once.
    Halves halves(LowerValue* value) {
        for(auto& entry: made) {
            if(entry.whole == value) return entry;
        }

        auto type = value->type;
        auto half = halfOf(type);
        auto inst = value->inst();
        Halves answer { value, nullptr, nullptr };

        // A join this pass wrote is read back as what it joined.
        if(isFullVector(type) && inst->kind == LowerInst::VecShuffle && isFullHalvesShuffle(base, inst)
           && base[((LowerInstVecShuffle*)inst)->left]->type.byteWidth() == 32) {
            auto join = (LowerInstVecShuffle*)inst;
            answer.low = base[join->left];
            answer.high = base[join->right];
        } else if(inst->kind == LowerInst::VecSplat) {
            // A splat's halves are two splats of the same scalar, which is what every later pass
            // expects of a shift count or a constant - a shuffle of one would hide both.
            auto e = belowDefinition(value);
            auto scalar = base[((LowerInstVecSplat*)inst)->from];
            answer.low = e.splat(half, scalar);
            answer.high = e.splat(half, scalar);
        } else {
            auto e = belowDefinition(value);
            auto lanes = half.lanes();
            answer.low = e.shuffle(half, value, value, [&](Size j) { return U8(j); });
            answer.high = e.shuffle(half, value, value, [&](Size j) { return U8(lanes + j); });
        }

        made.push(answer);
        return answer;
    }

    LowerValue* halfOfValue(LowerValue* value, bool high) {
        auto parts = halves(value);
        return high ? parts.high : parts.low;
    }

    // The two halves joined back into one register, where the instruction stood.
    LowerValue* join(Expansion& e, LowerType type, LowerValue* low, LowerValue* high) {
        return e.shuffle(type, low, high, [](Size j) { return U8(j); });
    }

    /*
     * One instruction copied at half its width - the same kind, the same flags and fields, `type` for
     * its result and `operands` for what it reads. What makes this safe for every kind it is asked
     * of is that `lowerInstSize` answers for it: the allocation is the struct `inst.def` names and
     * nothing past it, so a flat copy is the whole instruction - the loop rotation's clone, one width
     * down.
     */
    LowerValue* clone(Expansion& e, LowerInst* inst, LowerType type, SmallArray<LowerValue*, 4>& operands) {
        auto bytes = lowerInstSize(inst);
        assertTrue(bytes != 0 && inst->createdCount == 1);
        assertTrue(operands.size() == inst->usedCount);

        auto copy = (LowerInst*)fun.arena.alloc(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
        copyMem(inst, copy, bytes);

        copy->block = nullptr;
        copy->liveId = kNullLive;

        auto& original = inst->created()[0];
        new (copy->created().ptr) LowerValue(copy, type, original.name);

        for(Size i = 0; i < operands.size(); i++) copy->used()[i] = operands[i] - base;

        insertInstAt(base, e.block, e.at++, copy);
        return copy->created().ptr;
    }

    // Every operand at its half, where it is a full vector; a scalar - a lane's value, a count - is
    // read by both halves as it is.
    void halvedOperands(LowerInst* inst, bool high, SmallArray<LowerValue*, 4>& into) {
        into.clear();

        for(auto operand: inst->used()) {
            auto value = base[operand];
            into.push(isFull(value->type) ? halfOfValue(value, high) : value);
        }
    }

    // An instruction whose lane `i` reads lane `i` of each operand and nothing else: the two halves
    // are the same instruction at half the width, and the answer their join.
    LowerValue* lanewise(Expansion& e, LowerInst* inst) {
        auto type = inst->created()[0].type;
        auto half = halfOf(type);

        SmallArray<LowerValue*, 4> operands;

        halvedOperands(inst, false, operands);
        auto low = clone(e, inst, half, operands);

        halvedOperands(inst, true, operands);
        auto high = clone(e, inst, half, operands);

        return join(e, type, low, high);
    }

    // A lane read by index, which is a lane of one half.
    LowerValue* laneOf(Expansion& e, LowerInstVecLane* read) {
        auto from = base[read->from];
        auto perHalf = from->type.lanes() / 2;
        auto index = read->getLane();
        auto part = halfOfValue(from, index >= perHalf);

        return e.lane(part, U8(index % perHalf));
    }

    // A lane written by index: that half with the lane written, and the other half as it was.
    LowerValue* withLaneOf(Expansion& e, LowerInstVecLane* write) {
        auto from = base[write->from];
        auto type = write->result.type;
        auto perHalf = from->type.lanes() / 2;
        auto index = write->getLane();
        auto high = index >= perHalf;

        auto parts = halves(from);
        auto written = e.withLane(halfOf(type), high ? parts.high : parts.low, U8(index % perHalf), base[write->value]);

        return high ? join(e, type, parts.low, written) : join(e, type, written, parts.high);
    }

    // A reduction of a vector: the halves combined lane by lane, which is the first level of the tree
    // `lowerVectorReductions` would have built, and the 256-bit reduction of what that leaves.
    LowerValue* reduction(Expansion& e, LowerInstVecReduce* reduce) {
        auto from = base[reduce->from];
        auto parts = halves(from);
        auto combined = reduceStep(e, reduce->getReduce(), halfOf(from->type), parts.low, parts.high);

        return e.emit(new (fun.arena) LowerInstVecReduce(reduce->result.name, reduce->result.type,
                                                         combined - base, reduce->getReduce()));
    }

    /*
     * A conversion between lane widths, or one with an expansion of its own: each half of the lanes
     * converted, and the two answers joined at the result's width.
     *
     * Halves of the *lanes*, which is not halves of the register at both ends: a widening conversion
     * with a 512-bit result reads a 256-bit source, and its halves are that source's two 128-bit
     * halves - a width-changing shuffle at 256 bits, which is `lowerWidthChangingShuffles`' as it
     * always was. Handing each half the whole of a narrower source was the first cut of this, and
     * converted sixteen lanes of thirty-two twice.
     */
    LowerValue* conversion(Expansion& e, LowerInstCast* cast) {
        auto type = cast->result.type;
        auto half = halfOf(type);
        auto source = base[cast->from];
        auto parts = halves(source);

        SmallArray<LowerValue*, 4> operands;
        operands.push(parts.low);
        auto low = clone(e, cast, half, operands);

        operands.clear();
        operands.push(parts.high);
        auto high = clone(e, cast, half, operands);

        return join(e, type, low, high);
    }

    /*
     * A lookup whose table is narrower than its indices - a sixteen-byte digit table read at sixty-four
     * byte indices, which is `pushHex` - as two lookups of the same table at half the indices each.
     * The 256-bit one doubles the table into a `ymm` and is one `vpshufb`.
     */
    LowerValue* lookup(Expansion& e, LowerInstVecMove* move) {
        auto type = move->result.type;
        auto control = base[move->control];
        auto table = base[move->source];

        auto indices = halves(control);
        auto half = halfOf(type);

        auto read = [&](LowerValue* index) {
            return e.emit(new (fun.arena) LowerInstVecMove(StringId(), half, table - base, index - base,
                                                           LowerLaneMove::Lookup));
        };

        auto low = read(indices.low);
        auto high = read(indices.high);
        return join(e, type, low, high);
    }

    // Whether the pass knows how to split this instruction at all - anything it does not is reported
    // by name, which is `checkVectorSupported`'s shape, rather than reaching selection.
    bool splittable(LowerInst* inst) {
        switch(inst->kind) {
            case LowerInst::VecLane:
            case LowerInst::VecWithLane:
            case LowerInst::VecReduce:
            case LowerInst::VecMove:
                return true;

            // A conversion is halved by its lanes, which needs a vector at both ends with one count.
            case LowerInst::Cast: {
                auto source = base[((LowerInstCast*)inst)->from]->type;
                return isVectorLike(source) && source.lanes() == inst->created()[0].type.lanes();
            }
            default:
                break;
        }

        if(inst->createdCount != 1 || lowerInstSize(inst) == 0) return false;
        auto type = inst->created()[0].type;
        if(!isFull(type)) return false;

        // Every full operand has the result's lane count, which is what "lane-wise" means here.
        for(auto operand: inst->used()) {
            auto value = base[operand];
            if(isFull(value->type) && value->type.lanes() != type.lanes()) {
                return false;
            }
        }

        return true;
    }

    void split(LowerInst* inst) {
        // Every operand's halves first: making one inserts below its definition, which may be above
        // this instruction in its own block, so the position is read only once they all exist.
        for(auto operand: inst->used()) {
            auto value = base[operand];
            if(isFull(value->type)) halves(value);
        }

        // A widening conversion's source is a 256-bit vector, which is halved by its lanes all the same.
        if(inst->kind == LowerInst::Cast) halves(base[((LowerInstCast*)inst)->from]);

        auto block = base[inst->block];
        auto at = positionOf(base, block, inst).unwrap();
        Expansion e { base, fun, block, at };

        LowerValue* answer = nullptr;

        switch(inst->kind) {
            case LowerInst::VecLane:     answer = laneOf(e, (LowerInstVecLane*)inst); break;
            case LowerInst::VecWithLane: answer = withLaneOf(e, (LowerInstVecLane*)inst); break;
            case LowerInst::VecReduce:   answer = reduction(e, (LowerInstVecReduce*)inst); break;
            case LowerInst::Cast:        answer = conversion(e, (LowerInstCast*)inst); break;
            case LowerInst::VecMove:     answer = lookup(e, (LowerInstVecMove*)inst); break;
            default:                     answer = lanewise(e, inst); break;
        }

        replaceAllUses(base, inst->created().ptr, answer);
        removeInst(base, inst);
    }
};

} // namespace

void splitFullVectors(Context& ctx, LowerBase base, LowerFunction& fun) {
    if(!(targetFeatures() & kFeatureAvx512f)) return;

    // Collected first, because splitting one inserts halves below the definitions of its operands -
    // in other blocks as well as this one - and a walk by index would step over them or into them.
    SmallArray<LowerInst*, 32> pending;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(auto i: block->instructions.contents(base)) {
            auto inst = base[i];
            if(touchesFull(base, inst) && !fullWidthNative(base, inst)) pending.push(inst);
        }
    }

    Split s { base, fun };

    for(auto inst: pending) {
        if(!s.splittable(inst)) {
            ctx.diagnostics.error("x64: `%@` in %@ has no 512-bit form and cannot be split into two 256-bit halves"_v,
                                  inst->source, nameForInst(base, *inst), ctx.findName(fun.name));
            continue;
        }

        s.split(inst);
    }

    /*
     * And the whole values nothing reads any more - a splat whose halves are now two splats, a join
     * both halves of which were read back directly. Every other value split here is still read by the
     * shuffles that took its halves. Not a courtesy: the scalar under a dead 512-bit splat still
     * counts it as a reader, and a shift count read by anything but a shift is one the peephole will
     * not embed.
     */
    for(Size i = s.made.size(); i > 0; i--) {
        auto whole = s.made[i - 1].whole;
        auto inst = whole->inst();
        if(!whole->uses.isEmpty()) continue;
        if(inst->kind == LowerInst::VecSplat || inst->kind == LowerInst::VecShuffle) removeInst(base, inst);
    }
}
