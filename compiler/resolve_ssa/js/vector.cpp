#include "convert_internal.h"

/*
 * Every operation over a multi-part value - Implementation-Vector.md §7 on this tier's terms.
 *
 * `convertVectorInst` runs **before** the scalar switch rather than as arms inside it, for the old
 * JavaScript backend's reason read one level down: `add` over two vectors and `add` over two
 * integers are one instruction, and what tells them apart is not the kind. There the discriminator
 * was the *type*; here it is the **representation**, which is the stronger form of the same test
 * and the one this tier is allowed to make - so `isVectorType`, `vectorLanes` and
 * `VectorType::isMask` appear nowhere below `repr.cpp`.
 *
 * Four of the five vector-only opcodes emit **no instruction at all**: a splat is one value named
 * `lanes` times, a lane read is one of the parts, a lane write is the parts with one replaced, and
 * a shuffle is the parts permuted. That is the clearest statement of what this representation buys,
 * since each of the four is a real instruction on the machine.
 */

namespace jsir {

/*
 * Every operation over a multi-part value - Implementation-Vector.md §7 on this tier's terms.
 *
 * Asked before the scalar switch rather than as arms inside it, for the old JavaScript backend's
 * reason read one level down: `add` over two vectors and `add` over two integers are one
 * instruction, and what tells them apart is not the kind. There the discriminator was the *type*;
 * here it is the **representation**, which is the stronger form of the same test and the one this
 * tier is allowed to make - a value is multi-part or it is not, and a mask is multi-part for the
 * same reason a vector is. So `isVectorType`, `vectorLanes` and `VectorType::isMask` appear nowhere
 * below the representation table.
 *
 * Four of the five vector-only opcodes emit **no instruction at all**: a splat is one value named
 * `lanes` times, a lane read is one of the parts, a lane write is the parts with one replaced, and a
 * shuffle is the parts permuted. That is the clearest statement of what this representation buys,
 * since each of the four is a real instruction on the machine.
 */
bool Conversion::convertVectorInst(ssa::Inst& inst) {
    auto where = inst.source;

    /*
     * An instruction whose result is an **address** is nobody's business here, whatever it holds.
     *
     * A `represent` over storage is the case: its result is the same storage under another
     * occurrence, so the scalar path answers the operand's *place* and emits nothing. Taken as a
     * lane-wise conversion instead it loaded the row, converted four numbers and handed back a
     * value - and the store through it then met an address nothing had converted. Which is the same
     * trap the scalar `Represent` arm already carries a paragraph about, met a second time because
     * this switch runs in front of it.
     */
    if(inst.results.size()) {
        auto result = ssa[inst.results.get(ssa, 0)];
        if(result->convention == ssa::Convention::Address ||
           loanDenotesStorage(result->occurrence)) {
            return false;
        }
    }

    auto resultParts = [&]() {
        return inst.results.size() ? lanesRepOf(*ssa[inst.results.get(ssa, 0)]) : JsRepId();
    };

    switch(inst.kind) {
        case ssa::Inst::VecSplat: {
            auto& written = (ssa::InstVecSplat&)inst;
            auto repId = resultParts();
            if(!repId.valid()) return false;

            auto from = valueOf(written.from.value(), where);
            if(!from) {
                internal("met a splat with no operand"_v, where);
                return true;
            }

            // The same value in every lane, and not `lanes` copies of the expression that produced
            // it: a part is a JSIR value, so naming one `lanes` times is `lanes` reads of one `var`.
            SmallArray<JsValue*, 16> lanes;
            for(U16 i = 0; i < target.rep(repId).partCount; i++) lanes.push(from);

            defineParts(inst, makeParts(repId, held(lanes)));
            return true;
        }

        case ssa::Inst::VecLane:
        case ssa::Inst::VecWithLane: {
            auto& written = (ssa::InstVecLane&)inst;
            auto sourceRep = lanesRepOf(*ssa[written.from.value()]);
            if(!sourceRep.valid()) return false;

            auto from = requireParts(written.from.value(), sourceRep, where);
            if(!from.valid()) {
                internal("met a lane access whose vector has no parts"_v, where);
                return true;
            }

            auto lane = written.lane < from.count ? written.lane : 0;

            if(!written.writesLane()) {
                define(inst, partAt(from, lane));
                return true;
            }

            auto value = valueOf(written.value.value(), where);
            if(!value) {
                internal("met a lane write with no value"_v, where);
                return true;
            }

            SmallArray<JsValue*, 16> lanes;
            for(U16 i = 0; i < from.count; i++) lanes.push(i == lane ? value : partAt(from, i));

            defineParts(inst, makeParts(from.rep, held(lanes)));
            return true;
        }

        case ssa::Inst::VecShuffle: {
            auto& written = (ssa::InstVecShuffle&)inst;
            auto repId = resultParts();
            if(!repId.valid()) return false;

            auto leftRep = lanesRepOf(*ssa[written.left.value()]);
            auto rightRep = lanesRepOf(*ssa[written.right.value()]);
            auto left = requireParts(written.left.value(), leftRep, where);
            auto right = requireParts(written.right.value(), rightRep, where);
            if(!left.valid() || !right.valid()) {
                internal("met a shuffle whose operands have no parts"_v, where);
                return true;
            }

            // A pattern entry names a lane of the two sources concatenated, so anything from the
            // left's count up is a lane of the second - the same reading every backend gives it.
            SmallArray<JsValue*, 16> lanes;
            for(U16 i = 0; i < target.rep(repId).partCount; i++) {
                auto entry = i < written.pattern.size() ? written.pattern.get(ssa, i) : U8(0);
                lanes.push(entry < left.count            ? partAt(left, entry)
                         : U32(entry - left.count) < right.count ? partAt(right, entry - left.count)
                                                                 : partAt(left, 0));
            }

            defineParts(inst, makeParts(repId, held(lanes)));
            return true;
        }

        case ssa::Inst::VecReduce: {
            auto& written = (ssa::InstVecReduce&)inst;
            auto sourceRep = lanesRepOf(*ssa[written.from.value()]);
            if(!sourceRep.valid()) return false;

            auto from = requireParts(written.from.value(), sourceRep, where);
            if(!from.valid()) {
                internal("met a reduction whose vector has no parts"_v, where);
                return true;
            }

            auto repId = repOf(*ssa[inst.results.get(ssa, 0)]);
            define(inst, reduceLanes(written.reduce, from, repId, isMaskRep(sourceRep), 0,
                                     from.count, where));
            return true;
        }

        /*
         * The five `native.def` rows that are vector operations rather than one machine's - the
         * three lane movements whose control is a value and the two widening multiply-accumulates.
         *
         * Portable rows, which `native.def` says are "the backend's to write because the backend
         * that has the instruction should see the operation rather than have to recognize one" -
         * and this backend has none of them, so each is its definition written out over `lanes`
         * values. Everything else a `Native` row names is a machine's and is reported by name.
         */
        case ssa::Inst::Native: {
            auto& written = (ssa::InstNative&)inst;
            auto repId = resultParts();

            switch(written.op) {
                case mir::NativeOp::VecCompress:
                case mir::NativeOp::VecExpand:
                case mir::NativeOp::VecLookup:
                    if(!repId.valid() || written.arguments.size() < 2) return false;
                    convertLaneMove(inst, written, repId, where);
                    return true;

                case mir::NativeOp::VecDotBytes:
                case mir::NativeOp::VecDotWords:
                    if(!repId.valid() || written.arguments.size() < 3) return false;
                    convertDotProduct(inst, written, repId, where);
                    return true;

                default:
                    return false;
            }
        }

        default:
            break;
    }

    /*
     * And the operations a vector shares with a scalar, taken lane by lane.
     *
     * The result's representation decides, except for a **comparison**, whose result is the mask and
     * whose operands are the vectors - which is the one place the question is asked of an operand,
     * and the same exception the old backend's gate made.
     */
    auto repId = resultParts();

    switch(inst.kind) {
        case ssa::Inst::Binary: {
            auto& written = (ssa::InstBinary&)inst;
            auto lhsRep = lanesRepOf(*ssa[written.lhs.value()]);
            if(!repId.valid() && !lhsRep.valid()) return false;

            auto operandRep = lhsRep.valid() ? lhsRep : repId;
            auto lhs = requireParts(written.lhs.value(), operandRep, where);
            auto rhs = requireParts(written.rhs.value(), operandRep, where);
            if(!lhs.valid() || !rhs.valid()) {
                internal("met a lane-wise operation whose operands have no parts"_v, where);
                return true;
            }

            if(!repId.valid()) {
                internal("met a lane-wise operation whose result is not multi-part"_v, where);
                return true;
            }

            auto lane = partRepAt(target, repId, 0);
            SmallArray<JsValue*, 16> lanes;

            for(U16 i = 0; i < target.rep(repId).partCount; i++) {
                lanes.push(laneBinary(written.op, lane, partAt(lhs, i), partAt(rhs, i), where));
            }

            for(auto value: lanes) {
                if(value) continue;
                report(mir::binaryOpName(written.op), where);
                return true;
            }

            defineParts(inst, makeParts(repId, held(lanes)));
            return true;
        }

        case ssa::Inst::Unary: {
            auto& written = (ssa::InstUnary&)inst;
            auto sourceRep = lanesRepOf(*ssa[written.value.value()]);
            if(!repId.valid() && !sourceRep.valid()) return false;

            /*
             * A conversion between two vectors is the conversion between their lanes, lane by lane
             * - which is what the lane count being preserved across a cast is *for*.
             *
             * A **bitcast** is the same statement one step down, and only where the two lanes are
             * the same width: `i8x16` read as `i32x4` is one register read another way natively and
             * is sixteen numbers read as four here, and this target has no bits for that to mean
             * anything about. The test is the lane *count*, which is what having the same width
             * amounts to once both sides are vectors of one logical width.
             */
            if(!repId.valid() || !sourceRep.valid()) {
                report("a conversion between a vector and something that is not one"_v, where);
                return true;
            }

            auto& result = target.rep(repId);
            auto& source = target.rep(sourceRep);

            if(written.op == mir::UnaryOp::Bitcast && result.partCount != source.partCount) {
                report("a bitcast between vectors of different lane widths, where a lane is a "
                       "number here rather than a run of bits"_v, where);
                return true;
            }

            auto from = requireParts(written.value.value(), sourceRep, where);
            if(!from.valid()) {
                internal("met a lane-wise operation whose operand has no parts"_v, where);
                return true;
            }

            auto lane = partRepAt(target, repId, 0);
            SmallArray<JsValue*, 16> lanes;

            for(U16 i = 0; i < result.partCount; i++) {
                if(i >= from.count) {
                    lanes.push(zeroOf(lane, where));
                    continue;
                }

                switch(written.op) {
                    /*
                     * A reinterpretation between two lanes of one width, and a restatement: both are
                     * the coercion the destination lane needs, for the scalar path's reason - two
                     * integer types of one width are the same host value in two normal forms, and a
                     * float meeting an integer goes through the shared typed-array pair.
                     */
                    case mir::UnaryOp::Bitcast: {
                        auto held = partAt(from, i);
                        auto& fromLane = target.rep(held->rep);
                        auto& toLane = target.rep(lane);

                        if(fromLane.isFloat() != toLane.isFloat()) {
                            JsValue* one[] = { held };
                            lanes.push(coerced(
                                helperOf(fromLane.isFloat() ? JsHelperOp::FloatToBits
                                                            : JsHelperOp::BitsToFloat,
                                         lane, Buffer<JsValue*> { one, 1 }, where), lane, where));
                            break;
                        }

                        lanes.push(coerced(convertedTo(held, lane, where), lane, where));
                        break;
                    }

                    case mir::UnaryOp::Convert:
                    case mir::UnaryOp::Represent:
                        lanes.push(convertedTo(partAt(from, i), lane, where));
                        break;

                    default:
                        lanes.push(laneUnary(written.op, lane, partAt(from, i), where));
                        break;
                }
            }

            for(auto value: lanes) {
                if(value) continue;
                report(mir::unaryOpName(written.op), where);
                return true;
            }

            defineParts(inst, makeParts(repId, held(lanes)));
            return true;
        }

        // A lane-wise choice, whose condition is a mask with one lane per arm lane. The same
        // instruction a scalar select is, with a condition per lane instead of one for the value.
        case ssa::Inst::Select: {
            auto& written = (ssa::InstSelect&)inst;
            if(!repId.valid()) return false;

            auto conditionRep = lanesRepOf(*ssa[written.condition.value()]);
            if(!conditionRep.valid()) {
                report("a select of vectors under a condition that is not a mask"_v, where);
                return true;
            }

            auto condition = requireParts(written.condition.value(), conditionRep, where);
            auto whenTrue = requireParts(written.whenTrue.value(), repId, where);
            auto whenFalse = requireParts(written.whenFalse.value(), repId, where);
            if(!condition.valid() || !whenTrue.valid() || !whenFalse.valid()) {
                internal("met a lane-wise select whose operands have no parts"_v, where);
                return true;
            }

            auto lane = partRepAt(target, repId, 0);
            SmallArray<JsValue*, 16> lanes;

            for(U16 i = 0; i < target.rep(repId).partCount; i++) {
                lanes.push(selectOf(partAt(condition, i), partAt(whenTrue, i),
                                    partAt(whenFalse, i), lane, where));
            }

            defineParts(inst, makeParts(repId, held(lanes)));
            return true;
        }

        /*
         * `a * b + c`, at one rounding natively and at two here.
         *
         * There is no fused multiply-add on this target - nothing multiplies and adds without
         * rounding in between - and Design-Vector §3.3 makes that a permitted answer rather than a
         * gap: `fma` is permission to fuse, so a target with no fused instruction spends it as the
         * two operations, which is the same answer the x64 backend gives below FMA3.
         *
         * The coercion is the lane's own, which for an `f32` lane is the `Math.fround` that makes it
         * one: without it the product would be computed at double precision and the sum would be
         * too, which is not two roundings of a float expression but none.
         */
        case ssa::Inst::Fma: {
            auto& written = (ssa::InstFma&)inst;

            auto fuse = [&](JsRepId lane, JsValue* a, JsValue* b, JsValue* c) {
                auto product = coerced(binaryOf(mir::BinaryOp::Mul, lane, a, b, where), lane, where);
                return coerced(binaryOf(mir::BinaryOp::Add, lane, product, c, where), lane, where);
            };

            if(!repId.valid()) {
                auto scalar = repOf(*ssa[inst.results.get(ssa, 0)]);
                define(inst, fuse(scalar, valueOf(written.lhs.value(), where),
                                  valueOf(written.rhs.value(), where),
                                  valueOf(written.addend.value(), where)));
                return true;
            }

            auto lhs = requireParts(written.lhs.value(), repId, where);
            auto rhs = requireParts(written.rhs.value(), repId, where);
            auto addend = requireParts(written.addend.value(), repId, where);
            if(!lhs.valid() || !rhs.valid() || !addend.valid()) {
                internal("met a fused multiply-add whose operands have no parts"_v, where);
                return true;
            }

            auto lane = partRepAt(target, repId, 0);
            SmallArray<JsValue*, 16> lanes;

            for(U16 i = 0; i < target.rep(repId).partCount; i++) {
                lanes.push(fuse(lane, partAt(lhs, i), partAt(rhs, i), partAt(addend, i)));
            }

            defineParts(inst, makeParts(repId, held(lanes)));
            return true;
        }

        default:
            return false;
    }
}

/*
 * The three lane movements whose control is a **value** - `compress`, `expand` and `lookup`.
 *
 * The one place in the vector conversion that builds a row rather than naming parts, because a lane
 * chosen by a value is a computed index and the row is the only thing here a computed index reads.
 * Each row is built once and every lane of the answer is one read of it, so the cost is one
 * allocation and `lanes` reads - against the `lanes`-squared conditionals a chain of comparisons
 * would be.
 *
 * `lookup`'s index is taken modulo the table's lane count with an `and`, which is the language's rule
 * and is exact for a negative index too: a lane here is a 32-bit or narrower number, and `&` reads
 * its two's complement bits the way the machine's instruction reads its low ones.
 *
 * `compress` and `expand` count the mask's set lanes as they go, one prefix per lane. A compressed
 * lane `i` is written at its prefix where the mask keeps it and into a spare slot past the end where
 * it does not, so the writes are unconditional and the tail keeps the zero it started with.
 */
void Conversion::convertLaneMove(ssa::Inst& inst, ssa::InstNative& written, JsRepId repId,
                                 LocationId where) {
    auto sourceRep = lanesRepOf(*ssa[written.arguments.get(ssa, 0).value()]);
    auto controlRep = lanesRepOf(*ssa[written.arguments.get(ssa, 1).value()]);
    auto from = requireParts(written.arguments.get(ssa, 0).value(), sourceRep, where);
    auto control = requireParts(written.arguments.get(ssa, 1).value(), controlRep, where);

    if(!from.valid() || !control.valid()) {
        internal("met a lane movement whose operands have no parts"_v, where);
        return;
    }

    auto lanes = target.rep(repId).partCount;
    auto lane = partRepAt(target, repId, 0);
    auto indexRep = jsNumberRep(target, NumberDomain::Signed, 32);
    SmallArray<JsValue*, 16> answer;

    if(written.op == mir::NativeOp::VecLookup) {
        auto table = requireOne(from, where);
        if(!table) return;

        auto wrap = builder.integer(from.count - 1, partRepAt(target, controlRep, 0), where);

        for(U16 i = 0; i < lanes; i++) {
            auto index = binaryOf(mir::BinaryOp::And, partRepAt(target, controlRep, 0),
                                  partAt(control, i), wrap, where);

            // A 64-bit index is a `bigint`, which a row will not take as a subscript - so its
            // modulus is taken in its own domain and the answer made a number.
            answer.push(builder.load(builder.addElement(table, convertedTo(index, indexRep, where),
                                                        lane), where));
        }

        defineParts(inst, makeParts(repId, held(answer)));
        return;
    }

    // How many lanes the mask sets below lane `i`, for every `i` - each named, so that nothing reads
    // one a later statement changes.
    SmallArray<JsValue*, 16> prefix;
    auto running = builder.number(0, indexRep, where);

    for(U16 i = 0; i < lanes; i++) {
        prefix.push(running);
        if(U16(i + 1) == lanes) break;

        running = binaryOf(mir::BinaryOp::Add, indexRep, running,
                           selectOf(partAt(control, i), builder.number(1, indexRep, where),
                                    builder.number(0, indexRep, where), indexRep, where), where);
    }

    auto zero = zeroOf(lane, where);
    if(!zero) return;

    if(written.op == mir::NativeOp::VecExpand) {
        auto packed = requireOne(from, where);
        if(!packed) return;

        for(U16 i = 0; i < lanes; i++) {
            auto taken = builder.load(builder.addElement(packed, prefix[i], lane), where);
            answer.push(selectOf(partAt(control, i), taken, zero, lane, where));
        }

        defineParts(inst, makeParts(repId, held(answer)));
        return;
    }

    // One slot longer than the vector, so that a lane the mask drops has somewhere unobserved to be
    // written - which is what makes every write unconditional.
    SmallArray<JsValue*, 16> blank;
    for(U16 i = 0; i <= lanes; i++) blank.push(zero);

    auto row = builder.add<JsInstArray>(jsHostArrayRep(target, lane, lanes + 1u), where);
    for(auto value: blank) {
        row->values.push(target.arena, value - base());
        value->uses++;
    }

    auto packed = base()[row->result];
    auto past = builder.number(F64(lanes), indexRep, where);

    for(U16 i = 0; i < lanes; i++) {
        auto slot = selectOf(partAt(control, i), prefix[i], past, indexRep, where);
        builder.store(builder.addElement(packed, slot, lane), partAt(from, i), where);
    }

    for(U16 i = 0; i < lanes; i++) {
        answer.push(builder.load(builder.addElement(packed,
            builder.number(F64(i), indexRep, where), lane), where));
    }

    defineParts(inst, makeParts(repId, held(answer)));
}

/*
 * The two portable widening multiply-accumulates - `accumulateByteProducts` and
 * `accumulateWordProducts`, Analysis-Avx512.md §3.6.
 *
 * A vector is `lanes` host values here, so this is the definition written out: each accumulator lane
 * gets its group's products added to it one at a time, and the coercion at the end is what wraps the
 * sum back into 32 bits the way `+` over an `I32` does.
 *
 * **Every product is exact before the sum wraps**, which needs no arrangement on this target: a byte
 * product is below 2^15 in magnitude and a word product below 2^31, so the running total stays far
 * inside the 2^53 a double holds. The lanes arrive as signed host numbers wherever the lane type is
 * signed, which is exactly the reading `vpdpbusd` and `vpdpwssd` give them, so nothing here masks.
 */
void Conversion::convertDotProduct(ssa::Inst& inst, ssa::InstNative& written, JsRepId repId,
                                   LocationId where) {
    auto accRep = lanesRepOf(*ssa[written.arguments.get(ssa, 0).value()]);
    auto lhsRep = lanesRepOf(*ssa[written.arguments.get(ssa, 1).value()]);
    auto rhsRep = lanesRepOf(*ssa[written.arguments.get(ssa, 2).value()]);

    auto acc = requireParts(written.arguments.get(ssa, 0).value(), accRep, where);
    auto lhs = requireParts(written.arguments.get(ssa, 1).value(), lhsRep, where);
    auto rhs = requireParts(written.arguments.get(ssa, 2).value(), rhsRep, where);

    if(!acc.valid() || !lhs.valid() || !rhs.valid()) {
        internal("met a multiply-accumulate whose operands have no parts"_v, where);
        return;
    }

    auto lanes = target.rep(repId).partCount;
    auto lane = partRepAt(target, repId, 0);
    auto group = written.op == mir::NativeOp::VecDotBytes ? 4u : 2u;
    auto source = partRepAt(target, lhsRep, 0);

    SmallArray<JsValue*, 16> answer;
    for(U16 i = 0; i < lanes; i++) {
        auto sum = partAt(acc, i);

        for(U32 k = 0; k < group; k++) {
            auto at = U16(i * group + k);
            if(at >= lhs.count || at >= rhs.count) break;

            auto product = binaryOf(mir::BinaryOp::Mul, source, partAt(lhs, at), partAt(rhs, at),
                                    where);
            sum = binaryOf(mir::BinaryOp::Add, lane, sum, convertedTo(product, lane, where), where);
        }

        answer.push(coerced(sum, lane, where));
    }

    defineParts(inst, makeParts(repId, held(answer)));
}

/*
 * Every lane folded into one scalar - an unrolled adjacent-pair tree, and not a loop and not a
 * left-to-right fold.
 *
 * The order is a stated language property (Design-Vector §4.5), so what this emits has to be the
 * same tree the other backends emit or a float sum answers different bits on different targets.
 * Integer reductions are associative and could be written any way; they are written this way because
 * two shapes for one operation is how two backends drift apart.
 */
JsValue* Conversion::reduceLanes(mir::ReduceOp reduce, const PartsRef& from, JsRepId repId,
                                 bool mask, U16 at, U16 count, LocationId where) {
    /*
     * The lowest set lane, which is the one mask consumer that is not a tree.
     *
     * A chain of conditionals built from the last lane back - `m0 ? 0 : m1 ? 1 : 2` for two lanes -
     * so the answer is the first index whose lane holds, and the lane count where none does. Nothing
     * here is a bit scan, because there are no bits: a lane is a value on this target, so "which
     * lane matched" is a question about a run of booleans.
     */
    if(reduce == mir::ReduceOp::FirstSet) {
        auto answer = builder.number(F64(from.count), repId, where);
        for(auto i = from.count; i-- > 0;) {
            answer = selectOf(partAt(from, i), builder.number(F64(i), repId, where), answer, repId,
                              where);
        }

        return answer;
    }

    auto lane = partAt(from, at);

    if(count == 1) {
        // A mask's `count` is a sum of its lanes, which is the one place the boolean representation
        // costs something: each lane is read as the number it stands for before the tree adds it.
        if(mask && reduce != mir::ReduceOp::And && reduce != mir::ReduceOp::Or) {
            return convertedTo(lane, repId, where);
        }

        return lane;
    }

    auto half = U16(count / 2);
    auto lhs = reduceLanes(reduce, from, repId, mask, at, half, where);
    auto rhs = reduceLanes(reduce, from, repId, mask, U16(at + half), U16(count - half), where);

    if(mask) {
        // `any` and `all` over booleans are the logical operators, spelled as the bitwise ones over
        // a host-boolean representation - see `binaryExpr`, where `&&` and `||` are what those mean
        // at this representation. Anything else has already become a number above.
        switch(reduce) {
            case mir::ReduceOp::And: return binaryOf(mir::BinaryOp::And, repId, lhs, rhs, where);
            case mir::ReduceOp::Or:  return binaryOf(mir::BinaryOp::Or, repId, lhs, rhs, where);
            default: break;
        }

        return coerced(binaryOf(mir::BinaryOp::Add, repId, lhs, rhs, where), repId, where);
    }

    switch(reduce) {
        case mir::ReduceOp::Add:
            return coerced(binaryOf(mir::BinaryOp::Add, repId, lhs, rhs, where), repId, where);
        case mir::ReduceOp::Mul:
            return coerced(binaryOf(mir::BinaryOp::Mul, repId, lhs, rhs, where), repId, where);
        case mir::ReduceOp::And:
            return coerced(binaryOf(mir::BinaryOp::And, repId, lhs, rhs, where), repId, where);
        case mir::ReduceOp::Or:
            return coerced(binaryOf(mir::BinaryOp::Or, repId, lhs, rhs, where), repId, where);
        case mir::ReduceOp::Min:
        case mir::ReduceOp::Max: {
            auto op = reduce == mir::ReduceOp::Min ? JsMathOp::Min : JsMathOp::Max;

            /*
             * A 64-bit lane is a `bigint`, and `Math.min`/`Math.max` coerce their arguments to
             * numbers and *throw* on one - so the extremum there is the comparison written out. No
             * NaN question arises, unlike the float lanes: a `bigint` has no NaN, so the comparison
             * is total and either order of the operands answers the same thing.
             */
            if(target.rep(repId).kind == JsRepKind::BigInt) {
                auto keep = binaryOf(reduce == mir::ReduceOp::Min ? mir::BinaryOp::Lt
                                                                  : mir::BinaryOp::Gt,
                                     repId, lhs, rhs, where);
                return selectOf(keep, lhs, rhs, repId, where);
            }

            return mathOf(op, repId, lhs, rhs, where);
        }

        default:
            break;
    }

    report(mir::reduceOpName(reduce), where);
    return lhs;
}

} // namespace jsir
