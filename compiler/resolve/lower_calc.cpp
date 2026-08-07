/*
 * Computation: the instructions whose result is a value rather than a place.
 *
 * The layout metric, the native operations, casts, the unary and binary arithmetic, comparison,
 * select and a symbol's address. What most of them share is lower_type.cpp's masking rule: a
 * declared width narrower than a register is only true if every operation that could have widened
 * past it puts it back, so `truncateToWidth` is the last thing several of these do.
 */

#include "lower_internal.h"

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

            auto integerWiden = isInteger(lower.global, sourceType) &&
                                isInteger(lower.global, instruction.type) &&
                                sourceLower == LowerType::Int32 &&
                                targetLower == LowerType::Int64;

            auto signedSource = signedType(lower.global, sourceType) &&
                                (integerWiden || isFloat(lower.global, instruction.type));

            auto signedResult = signedType(lower.global, instruction.type) &&
                                (integerWiden || isFloat(lower.global, sourceType));

            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstCast(instruction.name, targetLower, from, signedSource, signedResult));
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
        case Value::Add:
        case Value::Sub:
        case Value::Mul:
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

                auto& repr = lower.repr.of(metric.of);
                auto number = metric.metric == TypeMetricKind::Align ? repr.align
                            : metric.metric == TypeMetricKind::Stride ? repr.stride
                            : repr.size;
                return number == 1;
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

            result = cmp(lower.lower, lower.to, block, lower.lower[lhs], lower.lower[rhs], lowerCmp(lower, compare), instruction.name);
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
