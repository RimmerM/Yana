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
    if(!signedType(lower.global, type) || signShift(lower, type) == 0) return result;

    return truncateToWidth(lower, block, result, type, lowered, name);
}

/*
 * The two atomic enumerations across the seam - Analysis-Atomics.md §5.1.
 *
 * `AtomicOrder` and `LowerOrder` have the same five members in the same order, and translating them
 * with a switch rather than a cast is the point: resolve/inst.h sits above the lower IR and may not
 * name it, so the two are declared separately, and a change to either encoding has to be made here
 * before it can change what a resolved program means.
 */
static LowerOrder lowerOrderOf(AtomicOrder order) {
    switch(order) {
        case AtomicOrder::Relaxed:        return LowerOrder::Relaxed;
        case AtomicOrder::Acquire:        return LowerOrder::Acquire;
        case AtomicOrder::Release:        return LowerOrder::Release;
        case AtomicOrder::AcquireRelease: return LowerOrder::AcquireRelease;
        case AtomicOrder::Sequential:     return LowerOrder::Sequential;
    }

    return LowerOrder::Sequential;
}

static LowerAtomicOp lowerAtomicOpOf(AtomicKind kind) {
    switch(kind) {
        case AtomicKind::Exchange: return LowerAtomicOp::Exchange;
        case AtomicKind::Add:      return LowerAtomicOp::Add;
        case AtomicKind::Sub:      return LowerAtomicOp::Sub;
        case AtomicKind::And:      return LowerAtomicOp::And;
        case AtomicKind::Or:       return LowerAtomicOp::Or;
        case AtomicKind::Xor:      return LowerAtomicOp::Xor;
        default:
            assertTrue("this atomic kind is not a read-modify-write" == nullptr);
            return LowerAtomicOp::Exchange;
    }
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
                                              lowerType(lower, instruction.type))) {
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
            // load and one shift - see NativeTypeDesc::kFlags. The other two are whole cells.
            if(metric.metric == TypeMetricKind::Align) {
                lower.values.add(instValue, descAlign(lower, block, descriptor));
                return nullptr;
            }

            auto offset = metric.metric == TypeMetricKind::Stride ? NativeTypeDesc::kStride
                                                                  : NativeTypeDesc::kSize;

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
                            new (syscall->created().ptr) LowerValue(syscall, lowerType(lower, instruction.type),
                                                                    instruction.name);
                        }

                        for(Size i = 0; i < args.size(); i++) syscall->used()[i] = args[i];
                    });

                    break;
                }

                /*
                 * Starting a thread, which becomes the backend's own intrinsic rather than a call.
                 *
                 * Five operands in, one out, in the order the declaration writes them - the whole of
                 * the mapping, because everything that makes this operation what it is lives in the
                 * encoder. See LowerIntrinsic::CloneThread.
                 */
                case NativeOp::CloneThread: {
                    auto type = lowerType(lower, instruction.type);
                    auto inst = (LowerInstIntrinsic*)lower.to.arena.alloc(
                        sizeof(LowerInstIntrinsic) + sizeof(LowerValue) +
                        sizeof(LowerPtr<LowerValue>) * args.size());

                    new (inst) LowerInstIntrinsic(LowerIntrinsic::CloneThread, 1, args.size());
                    for(Size i = 0; i < args.size(); i++) inst->used().ptr[i] = args[i];
                    new (inst->created().ptr) LowerValue(inst, type, instruction.name);

                    result = block.addInst(lower.lower, (LowerInst*)inst);
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
         * The atomics - Analysis-Atomics.md §5.1.
         *
         * A direct translation, and the interesting part is what does *not* happen on the way. The
         * order travels across unchanged and is neither strengthened nor weakened: an x86 locked
         * update is a full barrier whatever the IR says, and promoting a relaxed one here would make
         * the same program behave differently on a target whose instruction is not, as well as
         * forbidding motion that is legal (§5.3).
         *
         * The width is the *content's* stride rather than the atomic's own, and the two are the same
         * number by construction - `computeAtomic` sets the size from the content and the alignment
         * from the size. Reading it from the content is what keeps the lower instruction's width the
         * access width rather than a padded one.
         *
         * The address is `args[0]` and is already an address: an `Atomic(a)` is not a direct type,
         * so what a body holds of one is its storage, which is what `mappedValue` hands back.
         */
        case Value::Atomic: {
            auto& atomic = (InstAtomic&)instruction;
            SmallArray<LowerPtr<LowerValue>, 4> args;
            for(auto arg: atomic.args.contents(lower.local)) args.push(mappedValue(lower, arg));

            auto order = lowerOrderOf(atomic.order);

            if(atomic.kind == AtomicKind::Fence) {
                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstFence(order));
                break;
            }

            if(atomic.kind == AtomicKind::SpinHint) {
                result = block.addInst(lower.lower, new (lower.to.arena) LowerInstSpinHint());
                break;
            }

            // Every access names a *pointer to* an `Atomic(a)` first - see `atomicAddress` one tier
            // up, where taking the address is argued - and its content is what the access is wide.
            auto address = lower.local[atomic.args.get(lower.local, 0)]->type;
            auto content = ((AtomicType*)lower.global[pointeeType(lower.global, address)])->content;
            auto width = lower.repr.of(content).size;

            /*
             * Whether the answer has to be put back through the narrow-integer normalization.
             *
             * A location narrower than a register arrives in one, and what the bits above it hold is
             * **not** part of the value. `lock xadd byte [m], r8` writes the low byte of the register
             * it names and leaves the operand's own high bits standing; `lock cmpxchg` at a byte
             * leaves the *expected* value's; and LLVM widens an `i8` result with a cast that has to
             * be told which way. Every one of those answers a register that is not the normalized
             * form this IR keeps a narrow value in - so the result ends with the same mask or
             * sign-extension every other narrow producer ends with, and the four spellings collapse
             * into one. See truncateToWidth, and `wrapping` in test/lib/Atomic.yana, which is the
             * case that found this: `U8` at 250 minus 10 came back as 0xFFFFFF04.
             *
             * The load is not among them. It states its signedness on the instruction itself and
             * both backends widen it as they read it, which is one instruction rather than two.
             */
            auto narrow = atomic.kind != AtomicKind::Load
                       && narrowerThanRegister(lower, instruction.type);

            // The name goes on whatever ends up being the value the program named, which is the
            // normalization where there is one.
            auto resultName = narrow ? StringId() : instruction.name;

            switch(atomic.kind) {
                case AtomicKind::Load:
                    result = block.addInst(lower.lower, new (lower.to.arena) LowerInstAtomicLoad(
                        args[0], instruction.name, lowerType(lower, instruction.type), width,
                        signedType(lower.global, instruction.type), order));
                    break;

                case AtomicKind::Store:
                    result = block.addInst(lower.lower, new (lower.to.arena) LowerInstAtomicStore(
                        args[0], args[1], width, order));
                    break;

                case AtomicKind::Compare: {
                    // Two results, and the second is claimed by the `AtomicOk` that names this
                    // instruction rather than produced here - see InstAtomicOk.
                    auto type = lowerType(lower, instruction.type);
                    auto inst = (LowerInstAtomicCas*)lower.to.arena.alloc(sizeof(LowerInstAtomicCas));
                    new (inst) LowerInstAtomicCas(resultName, StringId(), type,
                                                  args[0], args[1], args[2], width, atomic.weak,
                                                  order, lowerOrderOf(atomic.failure));

                    result = block.addInst(lower.lower, inst);
                    break;
                }

                default:
                    result = block.addInst(lower.lower, new (lower.to.arena) LowerInstAtomicRmw(
                        args[0], args[1], resultName, lowerType(lower, instruction.type),
                        width, lowerAtomicOpOf(atomic.kind), order));
                    break;
            }

            if(narrow) {
                result = truncateToWidth(lower, block, result, instruction.type,
                                         lowerType(lower, instruction.type), instruction.name);
            }

            break;
        }

        /*
         * The compare-exchange's second result, taken off the instruction that already produced it.
         * Nothing is emitted: this is a *selector*, and what it selects is a value the lower IR has
         * been holding since its producer was translated.
         *
         * The walk is what the normalization above costs. Where the compared type is narrower than
         * its register, what the compare-exchange's own value maps to is the mask or the shift pair
         * standing in front of it rather than the instruction itself - and the flag lives on the
         * instruction. Each step of that chain reads its predecessor as its first operand, which is
         * what makes following operand zero the way back to the producer.
         */
        case Value::AtomicOk: {
            auto value = lower.lower[mappedValue(lower, ((InstAtomicOk&)instruction).cas)];

            while(value->inst()->kind != LowerInst::AtomicCas) {
                value = lower.lower[value->inst()->used()[0]];
            }

            auto cas = (LowerInstAtomicCas*)value->inst();
            lower.values.add(instValue, (&cas->exchanged) - lower.lower);
            return nullptr;
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
                LowerInst::Bitcast, instruction.name, lowerType(lower, instruction.type),
                mappedValue(lower, bitcastInst.from)));
            break;
        }
        case Value::Cast: {
            auto& castInst = (InstUnary&)instruction;
            auto from = mappedValue(lower, castInst.from);
            auto sourceType = lower.local[castInst.from]->type;

            auto sourceLower = lowerType(lower, sourceType);
            auto targetLower = lowerType(lower, instruction.type);

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
               !narrowerThanRegister(lower, sourceType) &&
               !narrowerThanRegister(lower, instruction.type)) {
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
            if(result && narrowerThanRegister(lower, instruction.type)) {
                result = truncateToWidth(lower, block, result, instruction.type, targetLower,
                                         instruction.name);
            }
            break;
        }
        case Value::Neg:
        case Value::Not: {
            auto& unaryInst = (InstUnary&)instruction;
            auto from = mappedValue(lower, unaryInst.from);
            auto type = lowerType(lower, instruction.type);

            if(instruction.kind == Value::Neg) {
                result = unary<LowerInst::Neg>(
                    lower.lower, lower.to, block, lower.lower[from], type, instruction.name
                );
            } else {
                result = unary<LowerInst::Not>(
                    lower.lower, lower.to, block, lower.lower[from], type, instruction.name
                );
            }

            /*
             * And back into the declared width, exactly as the binary arithmetic below does it and
             * through the same predicate - which already named `Neg` before this call site existed,
             * so a negation of a narrow type was wrapped by a rule nothing consulted.
             *
             * `not(0 :: U8)` was a register of every bit set rather than 255, and the dirt then
             * propagated silently: widening one afterwards is a `cast` that trusts a register the
             * operation never narrowed. The invariant being kept is `narrowerThanRegister`'s - a
             * value of a type that does not fill its register is held in normal form - which is what
             * entitles `widen` and the known-bits folder to skip a mask of their own.
             */
            if(result && wrapsAtDeclaredWidth(lower, instruction.type, instruction.kind)) {
                result = truncateToWidth(lower, block, result, instruction.type, type,
                                         instruction.name);
            }
            break;
        }
        /*
         * The byte reversal, and the one width that does not survive this seam.
         *
         * At 32 and 64 bits it is a unary instruction on both sides, exactly as `Abs` is: every
         * backend has the operation and what it does with one is its own business.
         *
         * At 16 it cannot be, because the lower IR has no 16-bit scalar - `lowerType` answers
         * `Int32` for every integer below the 64-bit family - so an instruction there would say
         * "reverse four bytes" whatever the program wrote. So this is where the swap is spent, and
         * it is spent in the terms the seam already has for a narrow type: the operand is masked
         * where it is held sign-extended (`shr` reads a narrow value's *storage*, which is
         * `zeroExtendsShiftOperand`'s rule at a different operation), the two halves are shifted
         * past each other, and `truncateToWidth` removes the byte that ends up above them and puts
         * the declared width's sign back. Four instructions, against the five the library's own
         * `((v shl 8) and 65280) or ((v shr 8) and 255)` came to.
         */
        case Value::ByteSwap: {
            auto& unaryInst = (InstUnary&)instruction;
            auto from = mappedValue(lower, unaryInst.from);
            auto type = lowerType(lower, instruction.type);

            auto swapBits = ((IntType*)lower.global[instruction.type])->bitsOn(lower.repr.target.integers);
            if(swapBits > 16) {
                result = unary<LowerInst::Bswap>(
                    lower.lower, lower.to, block, lower.lower[from], type, instruction.name
                );
                break;
            }

            auto value = signedType(lower.global, instruction.type)
                       ? maskToWidth(lower, block, from, instruction.type, type)
                       : from;

            auto eight = immediate(lower, 8, type);

            auto up = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[value],
                                             lower.lower[eight], type, StringId())->created().ptr - lower.lower;
            auto down = binary<LowerInst::Shr>(lower.lower, lower.to, block, lower.lower[value],
                                               lower.lower[eight], type, StringId())->created().ptr - lower.lower;

            result = binary<LowerInst::Or>(lower.lower, lower.to, block, lower.lower[up],
                                           lower.lower[down], type, StringId());

            result = truncateToWidth(lower, block, result, instruction.type, type, instruction.name);
            break;
        }
        /*
         * The three bit counts, which are one intrinsic each and no arithmetic around them.
         *
         * Nothing here masks or truncates, unlike the byte reversal above, and both halves of that
         * are the width rule in inst.def doing its job: the operand is 32 or 64 bits, so it fills
         * its register and its storage *is* its value, and the answer is between 0 and 64, so it is
         * in range of every type this can be asked at and needs no narrowing on the way out.
         *
         * Both zero counts lower to the **defined** form - `CttzWidth`, `ClzWidth` - because the
         * language's answer at zero is the width (inst.def rules on it) and those are the kinds that
         * say so. Whether the machine has an instruction for that or has to build it out of a bit
         * scan and a conditional move is the target's question, and `expandBitScans` in the x64
         * backend is where it is answered; the LLVM backend hands the same fact to `llvm.cttz` and
         * `llvm.ctlz` as an argument and lets the target lower it.
         *
         * Built by hand rather than through `unary<>`, for the reason the x64 builder's `intrinsic`
         * gives: `LowerInstIntrinsic`'s results live past the instruction rather than inside it,
         * because an intrinsic may answer none or several, so the allocation is the instruction plus
         * its one result and its one operand. See `handleIntrinsic` in lower_resolve.cpp, which
         * builds the same shape when reading one back in.
         */
        case Value::CountBits:
        case Value::LeadingZeros:
        case Value::TrailingZeros: {
            auto& unaryInst = (InstUnary&)instruction;
            auto from = mappedValue(lower, unaryInst.from);
            auto type = lowerType(lower, instruction.type);

            auto which = instruction.kind == Value::CountBits    ? LowerIntrinsic::Popcnt
                       : instruction.kind == Value::LeadingZeros ? LowerIntrinsic::ClzWidth
                                                                 : LowerIntrinsic::CttzWidth;

            auto inst = (LowerInstIntrinsic*)lower.to.arena.alloc(
                sizeof(LowerInstIntrinsic) + sizeof(LowerValue) + sizeof(LowerPtr<LowerValue>));

            new (inst) LowerInstIntrinsic(which, 1, 1);
            inst->used().ptr[0] = from;
            new (inst->created().ptr) LowerValue(inst, type, instruction.name);

            result = block.addInst(lower.lower, (LowerInst*)inst);
            break;
        }
        /*
         * The three BMI2 operations, which cross this seam as themselves.
         *
         * Not in the binary group below, and the difference is that group's `zeroExtendsShiftOperand`
         * question rather than anything about the arithmetic: these are defined at 32 and 64 bits
         * only - the verifier says so - so the value in the register is the value, and there is no
         * narrow width for this stage to mask into place on the way out. What each of them means at
         * a count or a mask the machine reads differently is the *backend's* to pay; see
         * `LowerInst::BitsUpTo`, where the difference is stated.
         */
        case Value::BitsUpTo:
        case Value::GatherBits:
        case Value::ScatterBits:
        // `crc32` is here for the same reason and not for BMI2's: it too is declared at 32 and 64
        // bits only, so the value in the register is the value.
        case Value::Crc32: {
            auto& binaryInst = (InstBinary&)instruction;
            auto lhs = lower.lower[mappedValue(lower, binaryInst.lhs)];
            auto rhs = lower.lower[mappedValue(lower, binaryInst.rhs)];
            auto type = lowerType(lower, instruction.type);

            result = instruction.kind == Value::BitsUpTo
                ? binary<LowerInst::BitsUpTo>(lower.lower, lower.to, block, lhs, rhs, type, instruction.name)
                : instruction.kind == Value::GatherBits
                ? binary<LowerInst::GatherBits>(lower.lower, lower.to, block, lhs, rhs, type, instruction.name)
                : instruction.kind == Value::ScatterBits
                ? binary<LowerInst::ScatterBits>(lower.lower, lower.to, block, lhs, rhs, type, instruction.name)
                : binary<LowerInst::Crc32>(lower.lower, lower.to, block, lhs, rhs, type, instruction.name);
            break;
        }
        // The two floating-point operations that are neither the unary pair above nor the binary
        // group below: one operand and no arithmetic beside it, and three operands and no other
        // instruction of that arity in this IR.
        case Value::Sqrt: {
            auto& unaryInst = (InstUnary&)instruction;
            result = unary<LowerInst::Sqrt>(
                lower.lower, lower.to, block, lower.lower[mappedValue(lower, unaryInst.from)],
                lowerType(lower, instruction.type), instruction.name
            );
            break;
        }
        // The magnitude, which is a unary instruction on both sides of this lowering - what each
        // backend does with one is its own business, and that is the point of the kind.
        case Value::Abs: {
            auto& unaryInst = (InstUnary&)instruction;
            result = unary<LowerInst::Abs>(
                lower.lower, lower.to, block, lower.lower[mappedValue(lower, unaryInst.from)],
                lowerType(lower, instruction.type), instruction.name
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
            auto type = lowerType(lower, instruction.type);
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
                instruction.name, lowerType(lower, instruction.type),
                mappedValue(lower, fma.a), mappedValue(lower, fma.b), mappedValue(lower, fma.c)));
            break;
        }
        /*
         * The SHA extension's two kinds, which cross this seam as themselves.
         *
         * Nothing is spent here and nothing could be: what each of them computes is a named step of
         * a named algorithm, so there is no narrower width to mask into place and no expansion to
         * write. The op is translated rather than reinterpreted, on `VecReduce`'s terms - the two
         * enums are declared in two headers that do not include each other, and a switch is what
         * keeps them from drifting apart silently.
         */
        // No operands, no result, and no target question to ask: what it means is the same on every
        // machine that has it, and a machine that does not is one this never reaches - see the
        // `@platform(x64)` declaration it comes from.
        case Value::VZeroUpper:
            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstVZeroUpper());
            break;

        case Value::ShaBinary: {
            auto& sha = (InstShaBinary&)instruction;
            auto op = LowerSha::Sha1Msg1;

            switch(sha.op) {
                case ShaOp::Sha1Msg1:    op = LowerSha::Sha1Msg1; break;
                case ShaOp::Sha1Msg2:    op = LowerSha::Sha1Msg2; break;
                case ShaOp::Sha1NextE:   op = LowerSha::Sha1NextE; break;
                case ShaOp::Sha1Rounds0: op = LowerSha::Sha1Rounds0; break;
                case ShaOp::Sha1Rounds1: op = LowerSha::Sha1Rounds1; break;
                case ShaOp::Sha1Rounds2: op = LowerSha::Sha1Rounds2; break;
                case ShaOp::Sha1Rounds3: op = LowerSha::Sha1Rounds3; break;
                case ShaOp::Sha256Msg1:  op = LowerSha::Sha256Msg1; break;
                case ShaOp::Sha256Msg2:  op = LowerSha::Sha256Msg2; break;
            }

            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstShaBinary(
                instruction.name, lowerType(lower, instruction.type),
                mappedValue(lower, sha.lhs), mappedValue(lower, sha.rhs), op));
            break;
        }
        case Value::Sha256Rounds: {
            auto& rounds = (InstSha256Rounds&)instruction;
            result = block.addInst(lower.lower, new (lower.to.arena) LowerInstSha256Rounds(
                instruction.name, lowerType(lower, instruction.type),
                mappedValue(lower, rounds.state), mappedValue(lower, rounds.feed),
                mappedValue(lower, rounds.keys)));
            break;
        }
        /*
         * The two rotations, and the widths that do not survive this seam.
         *
         * At 32 and 64 bits, and over a vector at any lane width, it is one instruction on both
         * sides: the machine has `rol`/`ror` and `vprold`, and what a backend without the packed one
         * does about it is its own business (`expandVectorRotate`).
         *
         * **Every narrower scalar is spent here**, which is the byte reversal's case at a different
         * operation and for the same reason: `lowerType` answers `Int32` for every integer below the
         * 64-bit family, so an instruction there would rotate within *thirty-two* bits whatever width
         * the program wrote. `@bits(30) U32` and `WideInt` are the same case, and the fact that the
         * expansion is written against the declared width rather than against a register is why they
         * are ordinary rather than refused - unlike `CountBits`, which has no such rewrite and is
         * declared at two widths only.
         *
         * The expansion is the definition:
         *
         *     n = count & (w - 1)          the modulus, which the machine's own masking is at 32/64
         *     rol(v, n) = (v << n) | (v >> (w - n))
         *
         * with three things the seam requires. The operand is **masked to its width first**, because
         * a signed narrow value is held sign-extended and a rotation that carried those bits round
         * would bring the register's sign into the low end of the answer - `zeroExtendsShiftOperand`'s
         * rule, which is sharper here than at `shr` because *both* halves read the storage.
         * `truncateToWidth` puts the declared width's sign back at the end. And `w - n` reaches `w`
         * at a zero count, which is a shift the register is wide enough to have an answer for: the
         * masked operand has `w` bits and `w` is below 32, so it shifts out to zero rather than
         * being undefined.
         */
        case Value::Rol:
        case Value::Ror: {
            auto& rotate = (InstBinary&)instruction;
            auto type = lowerType(lower, instruction.type);
            auto lhs = mappedValue(lower, rotate.lhs);
            auto rhs = mappedValue(lower, rotate.rhs);
            auto left = instruction.kind == Value::Rol;

            if(!narrowerThanRegister(lower, instruction.type)) {
                result = left
                    ? binary<LowerInst::Rol>(lower.lower, lower.to, block, lower.lower[lhs],
                                             lower.lower[rhs], type, instruction.name)
                    : binary<LowerInst::Ror>(lower.lower, lower.to, block, lower.lower[lhs],
                                             lower.lower[rhs], type, instruction.name);
                break;
            }

            auto bits = ((IntType*)lower.global[instruction.type])->bitsOn(lower.repr.target.integers);
            auto value = maskToWidth(lower, block, lhs, instruction.type, type);
            auto width = immediate(lower, bits, type);

            /*
             * The count, reduced modulo the width - and the width here is not always a power of two,
             * which is the one thing that makes this more than a mask.
             *
             * Eight and sixteen mask, because a power of two's `w - 1` *is* its modulus and it is
             * the same operation the machine performs on its own count register. `WideInt` is 53
             * bits and is the only type that reaches this without one, so it divides: the count is
             * first masked to the width, which is what makes it the **unsigned** reading the modulus
             * is defined over - a negative count is a bit pattern here, exactly as it is to `rol cl`
             * and to `llvm.fshl` - and the remainder of that is in range by construction.
             */
            LowerPtr<LowerValue> count;

            if((bits & (bits - 1)) == 0) {
                auto mask = immediate(lower, bits - 1, type);
                count = binary<LowerInst::And>(lower.lower, lower.to, block, lower.lower[rhs],
                                               lower.lower[mask], type, StringId())->created().ptr - lower.lower;
            } else {
                auto unsignedCount = maskToWidth(lower, block, rhs, instruction.type, type);
                count = binary<LowerInst::Rem>(lower.lower, lower.to, block, lower.lower[unsignedCount],
                                               lower.lower[width], type, StringId())->created().ptr - lower.lower;
            }
            auto back = binary<LowerInst::Sub>(lower.lower, lower.to, block, lower.lower[width],
                                               lower.lower[count], type, StringId())->created().ptr - lower.lower;

            auto up = left ? count : back;
            auto down = left ? back : count;

            auto high = binary<LowerInst::Shl>(lower.lower, lower.to, block, lower.lower[value],
                                               lower.lower[up], type, StringId())->created().ptr - lower.lower;
            auto low = binary<LowerInst::Shr>(lower.lower, lower.to, block, lower.lower[value],
                                              lower.lower[down], type, StringId())->created().ptr - lower.lower;

            result = binary<LowerInst::Or>(lower.lower, lower.to, block, lower.lower[high],
                                           lower.lower[low], type, StringId());

            result = truncateToWidth(lower, block, result, instruction.type, type, instruction.name);
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
            auto type = lowerType(lower, instruction.type);

            if(zeroExtendsShiftOperand(lower, instruction.type, instruction.kind)) {
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

            if(result && wrapsAtDeclaredWidth(lower, instruction.type, instruction.kind)) {
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
                         lowerType(lower, instruction.type));
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
                lowerType(lower, instruction.type)));

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
                instruction.name, lowerType(lower, instruction.type),
                mappedValue(lower, splat.from)));

            break;
        }
        case Value::VecLane: {
            auto& lane = (InstVecLane&)instruction;
            auto lowered = lowerType(lower, instruction.type);

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
                instruction.name, lowerType(lower, instruction.type), from, U8(lane.lane), value));

            break;
        }
        case Value::VecShuffle: {
            /*
             * The one that is not a plain construction: the lower IR's shuffle keeps its pattern in
             * the allocation past its own operands, so the space has to be reserved before the
             * instruction is built rather than pushed into it afterwards.
             */
            auto& shuffle = (InstVecShuffle&)instruction;
            auto type = lowerType(lower, instruction.type);
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

            auto lowered = lowerType(lower, instruction.type);

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
