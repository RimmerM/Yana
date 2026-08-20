/*
 * Computation: the instructions whose result is a value rather than a place.
 *
 * The layout metric, the native operations, casts, the unary and binary arithmetic, comparison,
 * select and a symbol's address. What most of them share is lower_type.cpp's masking rule: a
 * declared width narrower than a register is only true if every operation that could have widened
 * past it puts it back, so `truncateToWidth` is the last thing several of these do.
 */

#include "lower_internal.h"

/*
 * The sign put back on a value that is one lane of a vector, where the lane is narrower than the
 * register it arrives in.
 *
 * The lower IR's contract for a narrow lane - both `vlane` and the scalar a reduction answers - is
 * that it arrives **zero-extended**: that is what `pextrb`/`pextrw` do, and what `outOfLane` in the
 * LLVM backend writes. It has to be a contract rather than a choice, because the lane type below
 * this point states a *width* and nothing else. `LowerLane::Int8` cannot tell `Vec(I8)` from
 * `Vec(U8)`, so no backend can know which extension was wanted and this is the last place that does.
 *
 * `truncateToWidth` is the same shift pair every narrow scalar is put back through, so a lane read
 * and a field read of one type reach the same two instructions. It is asked only for the signed
 * half: the unsigned one is what the contract already guarantees, so calling it there would be a
 * masking `and` against bits that cannot be set.
 */
static LowerInst* signExtendNarrow(LowerContext& lower, LowerBlock& block, LowerInst* result,
                                   TypePtr type, LowerType lowered, StringId name) {
    if(lower.global[type]->kind != Type::Int) return result;
    if(!signedType(lower.global, type) || signShift(lower.global, type) == 0) return result;

    return truncateToWidth(lower, block, result, type, lowered, name);
}

// See lowerStorageInst for what a null return means.
LowerInst* lowerComputeInst(LowerContext& lower, LowerBlock& block, Inst& instruction,
                            ModulePtr<Value> instValue, Function* function) {
    LowerInst* result = nullptr;

    switch(instruction.kind) {
        case Value::TypeMetric: {
            /*
             * The layout question, answered.
             *
             * A concrete type folds to an immediate, exactly as the resolver used to fold it - so
             * `sizeOf(x)` costs nothing it did not cost before, and the difference is only in who
             * knew the number. A type variable has no number here, and the answer is a load out of
             * the descriptor its caller passed: `sizeOf` on a generic value works for the first
             * time, through machinery that already existed for the sizes lowering needed anyway.
             */
            auto& metric = (InstTypeMetric&)instruction;

            // A count this body does not know is one cell of the environment - see genConstValue.
            if(metric.metric == TypeMetricKind::Count) {
                if(auto count = genConstValue(lower, block, metric.of,
                                              lowerType(lower.global, instruction.type))) {
                    lower.values.add(instValue, count);
                    return nullptr;
                }
            }

            auto descriptor = genTypeDesc(lower, block, metric.of);

            // A concrete type's metric is a constant, so it is materialized on demand by
            // mappedValue rather than here - see the note there. Emitting it eagerly would leave an
            // `imm` behind for every one the scaling fold above removed the only use of.
            if(!descriptor) return nullptr;

            // The alignment shares the flags cell and sits above them, so reading it is the same
            // load and one shift - see TypeDescFields::kFlags. The other two are whole cells.
            if(metric.metric == TypeMetricKind::Align) {
                lower.values.add(instValue, descAlign(lower, block, descriptor));
                return nullptr;
            }

            auto offset = metric.metric == TypeMetricKind::Stride ? TypeDescFields::kStride
                                                                  : TypeDescFields::kSize;

            lower.values.add(instValue, descField(lower, block, descriptor, offset));
            return nullptr;
        }

        // The address in one table slot, decoded from the self-relative form this target holds it
        // in. The instruction exists so that the *asker* - a closure teardown, which is resolve IR
        // built before any target is chosen - does not have to know that form. See InstTableSlot.
        case Value::TableSlot: {
            auto& read = (InstTableSlot&)instruction;
            lower.values.add(instValue, tableSlotAddress(lower, block, mappedValue(lower, read.table),
                                                         read.slot));
            return nullptr;
        }

        case Value::Native: {
            auto& native = (InstNative&)instruction;
            SmallArray<LowerPtr<LowerValue>, 8> args;
            for(auto arg: native.args.contents(lower.local)) args.push(mappedValue(lower, arg));

            switch(native.op) {
                case NativeOp::CopyMemory:
                    result = block.addInst(lower.lower, new (lower.to.arena) LowerInstCopy(args[0], args[1], args[2]));
                    break;
                case NativeOp::SetMemory:
                    // setMemory is written (to, value, count) and the instruction takes
                    // (to, count, pattern), which is the order its printed form uses.
                    result = block.addInst(lower.lower, new (lower.to.arena) LowerInstSetPattern(args[0], args[2], args[1]));
                    break;
                /*
                 * The bit scan - one intrinsic in, one value out.
                 *
                 * Built by hand rather than through `block.addInst`, for the reason the x64
                 * builder's `intrinsic` gives: `LowerInstIntrinsic`'s results live past the
                 * instruction rather than inside it, because an intrinsic may answer none or
                 * several, so the allocation is the instruction plus its one result and its one
                 * operand. See `handleIntrinsic` in lower_resolve.cpp, which builds the same shape
                 * when reading one back in.
                 */
                case NativeOp::TrailingZeros: {
                    auto type = lowerType(lower.global, instruction.type);
                    auto inst = (LowerInstIntrinsic*)lower.to.arena.alloc(
                        sizeof(LowerInstIntrinsic) + sizeof(LowerValue) + sizeof(LowerPtr<LowerValue>));

                    new (inst) LowerInstIntrinsic(LowerIntrinsic::Cttz, 1, 1);
                    inst->used().ptr[0] = args[0];
                    new (inst->created().ptr) LowerValue(inst, type, instruction.name);

                    result = block.addInst(lower.lower, (LowerInst*)inst);
                    break;
                }

                case NativeOp::Syscall: {
                    // The kernel is the callee, so there is no function operand: the number is
                    // operand zero, exactly as the lower IR's own syscall form has it.
                    auto created = isUnit(lower.global, instruction.type) ? 0 : 1;

                    result = call(lower.lower, lower.to, block, created, args.size(), LowerCallType::Syscall,
                                  [&](LowerInstCall* syscall) {
                        if(created) {
                            new (syscall->created().ptr) LowerValue(syscall, lowerType(lower.global, instruction.type),
                                                                    instruction.name);
                        }

                        for(Size i = 0; i < args.size(); i++) syscall->used()[i] = args[i];
                    });

                    break;
                }

                case NativeOp::HostCall:
                case NativeOp::HostField:
                case NativeOp::HostArray:
                case NativeOp::HostBinary:
                case NativeOp::HostGlobalCall:
                case NativeOp::HostThrow:
                    /*
                     * Unreachable by construction - Implementation-Containers.md §14.1.
                     *
                     * Every declaration that produces one of these is `@platform(js)`, and
                     * `platformEnabled` runs during resolution, so a native build has no name, no
                     * type and no instance that could reach one. Reaching here means the platform
                     * filter let a host declaration through, which is worth saying rather than
                     * approximating.
                     */
                    lower.context.diagnostics.error("internal: a host operation reached the native lowering"_v,
                                                    instruction.source);
                    break;
            }

            break;
        }
        /*
         * `bitcast(x)` - the same bits under another type, and the one conversion that says so.
         *
         * A direct translation with none of the reasoning below it, because there is nothing to
         * decide: the resolver has already checked that the two types are the same width, so the
         * only question a `Cast` has to answer - how the value changes on the way - has the answer
         * "it does not". `truncateToWidth` is deliberately absent for the same reason: both sides
         * fill the same register and a refinement is not a `Bitcast` target.
         */
        case Value::Bitcast: {
            auto& bitcastInst = (InstUnary&)instruction;

            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstUnary(
                LowerInst::Bitcast, instruction.name, lowerType(lower.global, instruction.type),
                mappedValue(lower, bitcastInst.from)));
            break;
        }
        case Value::Cast: {
            auto& castInst = (InstUnary&)instruction;
            auto from = mappedValue(lower, castInst.from);
            auto sourceType = lower.local[castInst.from]->type;

            auto sourceLower = lowerType(lower.global, sourceType);
            auto targetLower = lowerType(lower.global, instruction.type);

            /*
             * A conversion between two addresses moves no bits: both sides are one machine word, and
             * what changes is only what the program says the word means.
             *
             * Asked of the *lowered* types rather than of `Type::Ptr` on either side, which is the
             * same question one level down and a strictly wider one. A raw pointer, a borrow and a
             * memory-typed value are all `LowerType::Pointer` here - they differ in what the checker
             * knows about them and not in what the machine holds - so a cast between any two of them
             * is a bitcast. The narrower test admitted only the first, which is all that existed
             * until `stringData` reinterpreted a `String` as a borrow of the record describing it
             * (Implementation-String.md part 2); that came out as a numeric conversion between two
             * pointers, which the lower IR validator rejects and rightly.
             */
            if(sourceLower == LowerType::Pointer || targetLower == LowerType::Pointer) {
                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstUnary(
                    LowerInst::Bitcast, instruction.name, targetLower, from));
                break;
            }

            auto integerCast = isInteger(lower.global, sourceType) &&
                               isInteger(lower.global, instruction.type);

            auto sourceSigned = signedType(lower.global, sourceType);
            auto targetSigned = signedType(lower.global, instruction.type);

            /*
             * Reading a full-register signed integer as its unsigned counterpart does not convert
             * bits. In particular, the bounds-check view of a Size index is I64 -> U64 after the
             * Int -> I64 sign extension used by the address. Lowering that outer reading as a
             * bitcast lets both consumers share the sign-extended SSA value; spelling it as another
             * numeric cast let the chain fold into a separate Int -> U64 zero-extension instead.
             *
             * Keep ordinary same-register conversions as casts. Besides making this rule state the
             * exact case it exists for, that preserves their useful type-changing spelling in lower
             * IR and avoids turning every Int/I32 alias conversion into a bitcast.
             */
            if(integerCast && sourceLower == targetLower && sourceSigned && !targetSigned &&
               !narrowerThanRegister(lower.global, sourceType) &&
               !narrowerThanRegister(lower.global, instruction.type)) {
                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstUnary(
                    LowerInst::Bitcast, instruction.name, targetLower, from));
            }

            auto integerWiden = integerCast &&
                                sourceLower == LowerType::Int32 &&
                                targetLower == LowerType::Int64;

            auto signedSource = sourceSigned &&
                                (integerWiden || isFloat(lower.global, instruction.type));

            auto signedResult = targetSigned &&
                                (integerWiden || isFloat(lower.global, sourceType));

            if(!result) {
                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstCast(
                    instruction.name, targetLower, from, signedSource, signedResult));
            }

            /*
             * A primitive narrower than its register has to become a value of that width here, not
             * merely acquire its name. Arithmetic already calls truncateToWidth for the operations
             * that can escape the range; a conversion is another such producer. Without this,
             * `300 :: U8` retained 300 on native while JS masked it to 44.
             *
             * The known-bits fold removes this mask when the source already fits, so truthful type
             * semantics do not charge conversions that are already known to be in range.
             */
            if(result && narrowerThanRegister(lower.global, instruction.type)) {
                result = truncateToWidth(lower, block, result, instruction.type, targetLower,
                                         instruction.name);
            }
            break;
        }
        case Value::Neg:
        case Value::Not: {
            auto& unaryInst = (InstUnary&)instruction;
            auto from = mappedValue(lower, unaryInst.from);
            if(instruction.kind == Value::Neg) {
                result = unary<LowerInst::Neg>(
                    lower.lower, lower.to, block, lower.lower[from],
                    lowerType(lower.global, instruction.type),
                    instruction.name
                );
            } else {
                result = unary<LowerInst::Not>(
                    lower.lower, lower.to, block, lower.lower[from],
                    lowerType(lower.global, instruction.type),
                    instruction.name
                );
            }
            break;
        }
        // The two floating-point operations that are neither the unary pair above nor the binary
        // group below: one operand and no arithmetic beside it, and three operands and no other
        // instruction of that arity in this IR.
        case Value::Sqrt: {
            auto& unaryInst = (InstUnary&)instruction;
            result = unary<LowerInst::Sqrt>(
                lower.lower, lower.to, block, lower.lower[mappedValue(lower, unaryInst.from)],
                lowerType(lower.global, instruction.type), instruction.name
            );
            break;
        }
        // The magnitude, which is a unary instruction on both sides of this lowering - what each
        // backend does with one is its own business, and that is the point of the kind.
        case Value::Abs: {
            auto& unaryInst = (InstUnary&)instruction;
            result = unary<LowerInst::Abs>(
                lower.lower, lower.to, block, lower.lower[mappedValue(lower, unaryInst.from)],
                lowerType(lower.global, instruction.type), instruction.name
            );
            break;
        }
        /*
         * The four roundings, which are unary on both sides of this lowering exactly as `Abs` is.
         *
         * The inner switch is what `unary`'s compile-time kind costs: the four differ in nothing
         * but which instantiation is asked for, and the builder wants that as a template argument
         * so it can range-check it against FirstUnary/LastUnary. x64 has no ties-away instruction
         * and expands `Round` itself - see inst.def for why the tie rule is ruled on there rather
         * than left to a backend.
         */
        case Value::Trunc:
        case Value::Floor:
        case Value::Ceil:
        case Value::Round: {
            auto& unaryInst = (InstUnary&)instruction;
            auto from = lower.lower[mappedValue(lower, unaryInst.from)];
            auto type = lowerType(lower.global, instruction.type);
            auto& to = lower.to;

            switch(instruction.kind) {
                case Value::Trunc:
                    result = unary<LowerInst::Trunc>(lower.lower, to, block, from, type, instruction.name);
                    break;
                case Value::Floor:
                    result = unary<LowerInst::Floor>(lower.lower, to, block, from, type, instruction.name);
                    break;
                case Value::Ceil:
                    result = unary<LowerInst::Ceil>(lower.lower, to, block, from, type, instruction.name);
                    break;
                default:
                    result = unary<LowerInst::Round>(lower.lower, to, block, from, type, instruction.name);
                    break;
            }

            break;
        }
        case Value::Fma: {
            auto& fma = (InstFma&)instruction;
            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstFma(
                instruction.name, lowerType(lower.global, instruction.type),
                mappedValue(lower, fma.a), mappedValue(lower, fma.b), mappedValue(lower, fma.c)));
            break;
        }
        case Value::Add:
        case Value::Sub:
        case Value::Mul:
        case Value::MulHi:
        case Value::Div:
        case Value::Rem:
        case Value::Shl:
        case Value::Shr:
        case Value::Sar:
        case Value::And:
        case Value::Or:
        case Value::Xor: {
            auto& binaryInst = (InstBinary&)instruction;

            /*
             * Scaling by one, removed where the one became known.
             *
             * `p + n` on a `%U8` multiplies by the element stride, and the resolver no longer knows
             * that stride is 1 - it emits the question and this stage answers it. Answering with an
             * immediate and leaving the multiply behind would make byte arithmetic cost an
             * instruction it never used to, which Design.md's pointer section is explicit about, so
             * the fold moves here with the knowledge rather than being lost with it.
             *
             * Asked of the resolve operand rather than of the lowered one, so that the immediate is
             * never materialized at all - checking afterwards would leave a dead `imm 1` in the
             * constant block for every byte-pointer offset in the program.
             *
             * Deliberately narrow: only a metric this stage folded, never a `1` the program wrote.
             * The backends have constant folders; what this owes is the cost the pointer-arithmetic
             * idiom was promised, and nothing beyond it.
             */
            auto metricIsOne = [&](ModulePtr<Value> operand) {
                auto value = lower.local[operand];
                if(value->kind != Value::TypeMetric) return false;

                auto& metric = *(InstTypeMetric*)value;
                if(isGeneric(lower.global, metric.of)) return false;

                return lower.repr.metric(metric.of, metric.metric) == 1;
            };

            if(instruction.kind == Value::Mul || instruction.kind == Value::Div) {
                // Division has only a right identity; multiplication has both.
                if(metricIsOne(binaryInst.rhs)) {
                    lower.values.add(instValue, mappedValue(lower, binaryInst.lhs));
                    return nullptr;
                }

                if(instruction.kind == Value::Mul && metricIsOne(binaryInst.lhs)) {
                    lower.values.add(instValue, mappedValue(lower, binaryInst.rhs));
                    return nullptr;
                }
            }

            auto lhs = mappedValue(lower, binaryInst.lhs);
            auto rhs = mappedValue(lower, binaryInst.rhs);
            auto type = lowerType(lower.global, instruction.type);

            if(zeroExtendsShiftOperand(lower.global, instruction.type, instruction.kind)) {
                lhs = maskToWidth(lower, block, lhs, instruction.type, type);
            }

            switch(binaryKind(lower, binaryInst)) {
                case LowerInst::Add:
                    result = binary<LowerInst::Add>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Sub:
                    result = binary<LowerInst::Sub>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Mul:
                    result = binary<LowerInst::Mul>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::IMul:
                    result = binary<LowerInst::IMul>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::MulHi:
                    result = binary<LowerInst::MulHi>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::IMulHi:
                    result = binary<LowerInst::IMulHi>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Div:
                    result = binary<LowerInst::Div>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::IDiv:
                    result = binary<LowerInst::IDiv>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::IRem:
                    result = binary<LowerInst::IRem>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Rem:
                    result = binary<LowerInst::Rem>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Shl:
                    result = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Shr:
                    result = binary<LowerInst::Shr>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Sar:
                    result = binary<LowerInst::Sar>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::And:
                    result = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Or:
                    result = binary<LowerInst::Or>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                case LowerInst::Xor:
                    result = binary<LowerInst::Xor>(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], type, instruction.name);
                    break;
                default:
                    break;
            }

            if(result && wrapsAtDeclaredWidth(lower.global, instruction.type, instruction.kind)) {
                result = truncateToWidth(lower, block, result, instruction.type, type, instruction.name);
            }

            break;
        }
        case Value::Cmp: {
            auto& compare = (InstCmp&)instruction;
            auto lhs = mappedValue(lower, compare.lhs);
            auto rhs = mappedValue(lower, compare.rhs);

            // The result type travels rather than being assumed: a comparison of two vectors answers
            // a mask of their shape and one of two scalars answers a Bool, and only this instruction's
            // own type tells them apart (§3.1). Nothing above the lower IR produced a vector
            // comparison until `class Lanewise` existed, so the builder's `Int32` default stood
            // unchallenged and every `.<` lowered into an `i32` the validator then rejected.
            result = cmp(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs],
                         lowerCmp(lower, compare), instruction.name,
                         lowerType(lower.global, instruction.type));
            break;
        }
        case Value::Select: {
            /*
             * The instruction the lower IR has had all along, reached at last from above it.
             *
             * `LowerInstSelect` takes its arms in the order the machine form reads them - the value
             * for a condition that holds first - and its condition as an ordinary `Int32` value. A
             * `Bool` is exactly that here (an enum record lowers to Int32), so nothing is needed to
             * turn the branch's own test into a select's, and the x64 transform will fold the
             * comparison that produced it back into the flags a `cmovcc` reads.
             */
            auto& select = (InstSelect&)instruction;
            auto whenTrue = mappedValue(lower, select.whenTrue);
            auto whenFalse = mappedValue(lower, select.whenFalse);
            auto condition = mappedValue(lower, select.cond);

            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstSelect(
                instruction.name, whenTrue, whenFalse, condition,
                lowerType(lower.global, instruction.type)));

            break;
        }
        /*
         * The five vector kinds, each one instruction on both sides - Implementation-Vector.md §3.2
         * and §4.2, which say the same five names.
         *
         * There is nothing to decide here and that is by construction: the resolve IR rejected a
         * runtime lane index and a runtime shuffle pattern, so both arrive as fields, and the
         * natural lane count was spent at the type. What is left is a rename.
         */
        case Value::VecSplat: {
            auto& splat = (InstVecSplat&)instruction;
            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstVecSplat(
                instruction.name, lowerType(lower.global, instruction.type),
                mappedValue(lower, splat.from)));

            break;
        }
        case Value::VecLane: {
            auto& lane = (InstVecLane&)instruction;
            auto lowered = lowerType(lower.global, instruction.type);

            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstVecLane(
                instruction.name, lowered, mappedValue(lower, lane.from), U8(lane.lane)));

            /*
             * The sign, put back on a lane narrower than the register it arrives in.
             *
             * The lower IR's contract is that an 8- or 16-bit lane is read **zero-extended** - which
             * is what `pextrb`/`pextrw` do and what the LLVM backend's `outOfLane` writes - because
             * the lane type it arrives as states a width and not a signedness. `LowerLane::Int8` is
             * eight bits and nothing else, so no backend can know which extension was wanted; this
             * is the last place that does.
             *
             * `truncateToWidth` is the same shift pair every narrow *scalar* is put back through, so
             * a lane read and a field read of one type reach the same two instructions. Only the
             * signed half is asked for: the unsigned one is what the contract already guarantees, so
             * calling it there would be a masking `and` against bits that cannot be set.
             */
            result = signExtendNarrow(lower, block, result, instruction.type, lowered,
                                      instruction.name);
            break;
        }
        case Value::VecWithLane: {
            auto& lane = (InstVecLane&)instruction;
            auto from = mappedValue(lower, lane.from);
            auto value = mappedValue(lower, lane.value);

            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstVecLane(
                instruction.name, lowerType(lower.global, instruction.type), from, U8(lane.lane), value));

            break;
        }
        case Value::VecShuffle: {
            /*
             * The one that is not a plain construction: the lower IR's shuffle keeps its pattern in
             * the allocation past its own operands, so the space has to be reserved before the
             * instruction is built rather than pushed into it afterwards.
             */
            auto& shuffle = (InstVecShuffle&)instruction;
            auto type = lowerType(lower.global, instruction.type);
            auto left = mappedValue(lower, shuffle.left);
            auto right = mappedValue(lower, shuffle.right);

            auto memory = lower.to.arena.alloc(sizeof(LowerInstVecShuffle) +
                                               LowerInstVecShuffle::patternBytes(type));
            auto lowered = new (memory) LowerInstVecShuffle(instruction.name, type, left, right);
            auto pattern = lowered->pattern();

            for(Size i = 0; i < pattern.length; i++) {
                pattern.ptr[i] = i < shuffle.pattern.size() ? shuffle.pattern[i] : 0;
            }

            result = block.addInst(lower.lower, lowered);
            break;
        }
        case Value::VecReduce: {
            /*
             * Where the signed/unsigned split is made. The resolve IR has one `Min` and one `Max`
             * because signedness is in the lane type; the lower IR has `IMin`/`IMax` beside them
             * because by then a type is a lane kind and a count and has forgotten it. Same shape as
             * `Div`/`IDiv` above, and read off the same predicate.
             */
            auto& reduce = (InstVecReduce&)instruction;
            auto lane = vectorLane(lower.global, lower.local[reduce.from]->type);
            auto isSigned = lane && signedType(lower.global, lane);
            auto op = LowerReduce::Add;

            switch(reduce.reduce) {
                case ReduceOp::Add: op = LowerReduce::Add; break;
                case ReduceOp::Mul: op = LowerReduce::Mul; break;
                case ReduceOp::Min: op = isSigned ? LowerReduce::IMin : LowerReduce::Min; break;
                case ReduceOp::Max: op = isSigned ? LowerReduce::IMax : LowerReduce::Max; break;
                case ReduceOp::And: op = LowerReduce::And; break;
                case ReduceOp::Or:  op = LowerReduce::Or; break;

                // The one kind with no signedness to read and no lane type to read it off: what it
                // answers is an index into the lanes rather than one of them.
                case ReduceOp::FirstSet: op = LowerReduce::FirstSet; break;

                // The movemask, which this IR only ever holds because `Native.bits` put it here - see
                // the kind. Its lowered twin is the one the x64 expansion writes for itself.
                case ReduceOp::Bits: op = LowerReduce::Bits; break;
            }

            auto lowered = lowerType(lower.global, instruction.type);

            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstVecReduce(
                instruction.name, lowered, mappedValue(lower, reduce.from), op));

            // A reduction of a narrow lane answers that lane, zero-extended, on the same terms a
            // lane read does - so it gets its sign back the same way. `count` and `firstSet` answer
            // an `Int` and this asks nothing of them.
            result = signExtendNarrow(lower, block, result, instruction.type, lowered,
                                      instruction.name);
            break;
        }
        case Value::Symbol: {
            // An address the loader supplies. The lower IR already has both forms, because a call
            // names its callee this way and a global load names its storage this way; what is new
            // here is only that the address is wanted as an ordinary value.
            auto& symbol = (InstSymbol&)instruction;

            if(symbol.callee) {
                auto target = lower.functions.getValue(symbol.callee).unwrap();
                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstFun(instruction.name, target));
            } else {
                auto global_ = lower.local[symbol.global];
                auto target = lower.to.globals.getValue(global_->name).unwrap();
                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstGlobal(instruction.name, target));
            }

            break;
        }
        default:
            assertTrue("unexpected instruction kind for this lowering" == nullptr);
            return nullptr;
    }

    return result;
}
