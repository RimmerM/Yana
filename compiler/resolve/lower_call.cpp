/*
 * The three call forms: direct, through an address, and generic.
 *
 * They are one file because they are one ABI seen three ways - the callee first, then the return
 * place if the result is one, then the arguments - and the third is where the erased half of it
 * lives: a generic call passes an environment its callee reads its type answers out of, which is
 * the only argument the other two never have.
 */

#include "lower_internal.h"

// See lowerStorageInst for what a null return means.
LowerInst* lowerCallInst(LowerContext& lower, LowerBlock& block, Inst& instruction,
                         ModulePtr<Value> instValue, Function* function) {
    LowerInst* result = nullptr;

    switch(instruction.kind) {
        case Value::CallDyn: {
            /*
             * A call through an address, laid out exactly as a direct one: the callee first, then
             * the hidden result storage a memory result needs, then the environment, then the
             * declared arguments.
             *
             * The environment sits after the result place rather than before it because a lifted
             * lambda is an ordinary function whose first *declared* parameter it is - the caller
             * builds nothing hidden, and the two sides agree because both read the same list.
             */
            auto& callInst = (InstCallDyn&)instruction;
            LowerPtr<LowerValue> address = nullptr;
            LowerPtr<LowerValue> env = nullptr;

            if(callInst.callable) {
                // The two words the call is reached through. A function value is a memory type, so
                // its lowered form is the address of the three, and the code and the environment
                // are the first two loads off it.
                auto base = mappedValue(lower, callInst.callable);
                auto codeAddress = addOffset(lower, block, base, FunValueLayout::offsetOf(FunValueLayout::kCode));
                auto envAddress = addOffset(lower, block, base, FunValueLayout::offsetOf(FunValueLayout::kEnv));

                address = load(lower.lower, lower.to, block, lower.lower[codeAddress], 8, false,
                               LowerType::Pointer, 0)->created().ptr - lower.lower;

                env = load(lower.lower, lower.to, block, lower.lower[envAddress], 8, false,
                           LowerType::Pointer, 0)->created().ptr - lower.lower;
            } else {
                address = mappedValue(lower, callInst.address);
            }

            auto memoryResult = isMemoryType(lower.global, instruction.type);
            LowerPtr<LowerValue> returnPlace = nullptr;

            if(memoryResult) {
                auto bytes = storageSize(lower, block, instruction.type);
                auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(
                    instruction.name, bytes, typeAlign(lower, instruction.type)));

                returnPlace = allocation->created().ptr - lower.lower;
            }

            /*
             * The declared conventions come off the callee's *function type*, which is all a caller
             * reaching a function through a value has - and is what makes the two sides agree about
             * which positions exist without either consulting the other.
             *
             * `signature` and not `type`: the instruction's own type is what the call *produces*,
             * and reading a result type as a signature is only harmless while every result happens
             * to be a scalar whose bytes read back as an empty argument list. A continuation
             * returning an `Outcome` is not, which is how this was found.
             *
             * Null for the one caller that has no signature to give - a teardown reached through a
             * witness slot (analyze_teardown.cpp) - which falls back to each argument's own type,
             * the same answer the position-past-the-end case already produced.
             */
            auto signatureType = callInst.signature;
            auto signature = signatureType && lower.global[signatureType]->kind == Type::Fun
                           ? (FunType*)lower.global[signatureType] : nullptr;

            SmallArray<LowerPtr<LowerValue>, 8> arguments;
            Size dynIndex = 0;

            for(auto arg: callInst.args.contents(lower.local)) {
                auto declared = signature && dynIndex < signature->args.size()
                    ? signature->args.get(lower.global, dynIndex) : FunArg { lower.local[arg]->type };

                dynIndex++;
                if(!lowerArgExists(lower.global, declared.type,
                                   declared.convention == ast::BindType::Ref)) {
                    continue;
                }

                arguments.push(mappedValue(lower, arg));
            }

            auto created = isUnit(lower.global, instruction.type) || memoryResult ? 0 : 1;
            auto used = arguments.size() + 1 + (env ? 1 : 0) + (memoryResult ? 1 : 0);

            result = call(lower.lower, lower.to, block, created, used, kDefaultCallType, [&](LowerInstCall* dynamic) {
                if(created) {
                    new (dynamic->created().ptr) LowerValue(dynamic, lowerType(lower.global, instruction.type), instruction.name);
                }

                dynamic->used()[0] = address;

                Size index = 1;
                if(memoryResult) dynamic->used()[index++] = returnPlace;
                if(env) dynamic->used()[index++] = env;

                for(auto argument: arguments) dynamic->used()[index++] = argument;
            });

            if(memoryResult) {
                result->source = instruction.source;
                lower.values.add(instValue, returnPlace);
                return nullptr;
            }

            break;
        }
        case Value::Call: {
            auto& callInst = (InstCall&)instruction;
            auto target = lower.functions.getValue(callInst.callee).unwrap();
            auto fun = block.addInst(lower.lower, new (lower.to.arena) LowerInstFun(0, target));
            auto memoryResult = isMemoryType(lower.global, instruction.type);
            LowerPtr<LowerValue> returnPlace = nullptr;

            if(memoryResult) {
                // The hidden result storage. Its size is a load rather than a constant wherever the
                // result type belongs to the caller's own type variables, which is the case
                // Implementation-Generics.md part 8 calls "owned return: hidden uninitialized
                // result pointer" - the caller provides it because only the caller knows where.
                auto bytes = storageSize(lower, block, instruction.type);
                auto alignment = isGeneric(lower.global, instruction.type)
                    ? 16u : typeAlign(lower, instruction.type);

                auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(instruction.name, bytes, alignment));
                returnPlace = allocation->created().ptr - lower.lower;
            }

            // The positions this call actually passes, read off the callee's own parameters so
            // that it agrees with what the callee's signature above received.
            auto callee = lower.local[callInst.callee];
            SmallArray<LowerPtr<LowerValue>, 8> passed;
            Size callIndex = 0;

            for(auto arg: callInst.args.contents(lower.local)) {
                auto parameter = callIndex < callee->args.size()
                    ? lower.local[callee->args.get(lower.local, callIndex)] : nullptr;

                callIndex++;
                auto declared = parameter ? parameter->type : lower.local[arg]->type;

                if(!lowerArgExists(lower.global, declared, parameter && parameter->isMutableBorrow())) {
                    continue;
                }

                passed.push(mappedValue(lower, arg));
            }

            auto created = isUnit(lower.global, instruction.type) || memoryResult ? 0 : 1;
            auto used = passed.size() + 1 + (memoryResult ? 1 : 0);

            result = call(lower.lower, lower.to, block, created, used, lower.lower[target]->callType, [&](LowerInstCall* call) {
                if(created) {
                    new (call->created().ptr) LowerValue(call, lowerType(lower.global, instruction.type), instruction.name);
                }

                call->used()[0] = fun->created().ptr - lower.lower;

                Size index = 1;
                if(memoryResult) call->used()[index++] = returnPlace;

                for(auto argument: passed) call->used()[index++] = argument;
            });

            if(memoryResult) {
                result->source = instruction.source;
                lower.values.add(instValue, returnPlace);
                return nullptr;
            }

            break;
        }
        case Value::GenCall: {
            /*
             * The erased call - Implementation-Generics.md part 9.
             *
             * Structurally an ordinary call with one more argument in front: the environment the
             * callee reads its slots out of. Everything else about the shape is the same, which is
             * the point of the leading position - a caller does not have to know anything about the
             * callee's schema to lay out the call, only to have built the right environment.
             *
             * Reaching here means the environment was static, since that is the only case
             * emitGenericCall takes the erased path for. A forwarded or mixed environment - one
             * generic body calling another - specializes instead, and is what part 9's cases 2 and
             * 3 are still owed.
             */
            auto& callInst = (InstGenCall&)instruction;
            auto callee = lower.local[callInst.callee];

            /*
             * Two shapes reach here, and they differ only in where the code address comes from.
             *
             * A call to a generic *function* names it, and passes the environment the callee reads
             * its own slots out of. A deferred *class* dispatch names nothing: the implementation is
             * chosen by whoever supplied this function's environment, so the address is loaded out
             * of the witness sitting in one of its slots, and the callee - being a concrete thunk -
             * needs no environment of its own.
             */
            auto dispatched = callInst.typeClass != nullptr;
            LowerPtr<LowerValue> address = nullptr;
            LowerPtr<LowerValue> envValue = nullptr;

            if(dispatched) {
                address = genMethod(lower, block, callInst);
            } else {
                auto target = lower.functions.getValue(callInst.callee).unwrap();
                auto fun = block.addInst(lower.lower, new (lower.to.arena) LowerInstFun(0, target));
                address = fun->created().ptr - lower.lower;
                envValue = genEnvironment(lower, block, callInst);
            }

            /*
             * The concrete-to-erased boundary.
             *
             * The callee was compiled against its own type variables, so a parameter whose declared
             * type is one of them arrives as an address whatever the caller substituted - part 8's
             * "unknown-size values use addresses". A caller holding an `Int` in a register therefore
             * has to give it storage first.
             *
             * Done here rather than in the resolver on purpose: the typed IR stays the source of
             * truth for what the call *means*, and only its representation is adapted.
             */
            auto materialize = [&](LowerPtr<LowerValue> value, TypePtr concrete) {
                auto bytes = immediate(lower, typeSize(lower, concrete));
                auto storage = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(
                    0, bytes, typeAlign(lower, concrete)));

                auto address = storage->created().ptr - lower.lower;
                block.addInst(lower.lower, new (lower.to.arena) LowerInstStore(
                    address, value, memoryWidth(lower, concrete)));

                return address;
            };

            SmallArray<LowerPtr<LowerValue>, 8> arguments;
            Size argIndex = 0;

            for(auto arg: callInst.args.contents(lower.local)) {
                auto concrete = lower.local[arg]->type;

                auto parameter = argIndex < callee->args.size()
                    ? lower.local[callee->args.get(lower.local, argIndex)] : nullptr;

                /*
                 * A `&` parameter is an address in both worlds, so there is nothing to adapt: the
                 * caller already passes a borrow. Boxing it would hand the callee the address of
                 * the borrow rather than of what was borrowed, and every write through it would
                 * land in a temporary the caller never reads - which is exactly the bug this
                 * condition exists to avoid.
                 */
                auto byAddress = parameter && parameter->isMutableBorrow();
                auto declared = parameter ? parameter->type : concrete;

                argIndex++;

                /*
                 * Which positions exist is the *callee's* question here, and only here.
                 *
                 * The erased body was compiled against its own variables, so a parameter declared
                 * as one of them is a position in the signature whatever this caller substituted -
                 * including `{}`. Deciding by the concrete type instead would drop an argument the
                 * callee is still reading, so the two rules genuinely differ: a declared unit is
                 * absent, and a declared variable that happens to be unit here is present.
                 */
                if(!lowerArgExists(lower.global, declared, byAddress)) continue;

                if(isUnit(lower.global, concrete)) {
                    // Present, and carrying nothing. The callee takes the address and copies the
                    // size its type descriptor gives, which is zero - so what it points at never
                    // matters, only that it is an address at all. See storageSize.
                    auto storage = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(
                        0, storageSize(lower, block, concrete), 8));

                    arguments.push(storage->created().ptr - lower.lower);
                    continue;
                }

                auto value = mappedValue(lower, arg);

                if(parameter && !byAddress && isGeneric(lower.global, parameter->type) &&
                   !isMemoryType(lower.global, concrete)) {
                    value = materialize(value, concrete);
                }

                arguments.push(value);
            }

            /*
             * The result, decided by what the *callee* declared rather than by what this call
             * substituted. A function returning `a` returns through caller storage however small the
             * substitution turns out to be, because the body it was compiled from has no other way
             * to hand a value back - so the caller provides the storage and reads out of it.
             */
            auto erasedResult = isMemoryType(lower.global, callee->returnType);
            auto concreteResult = isMemoryType(lower.global, instruction.type);
            LowerPtr<LowerValue> returnPlace = nullptr;

            if(erasedResult) {
                // The hidden result storage, which exists because the *callee's* signature said so:
                // a body returning `a` writes through caller storage however small `a` turns out to
                // be here, and `{}` is as small as it gets - see storageSize.
                auto bytes = storageSize(lower, block, instruction.type);
                auto alignment = isGeneric(lower.global, instruction.type) || isUnit(lower.global, instruction.type)
                    ? 16u : typeAlign(lower, instruction.type);

                auto allocation = block.addInst(lower.lower, new (lower.to.arena) LowerInstAlloca(instruction.name, bytes, alignment));
                returnPlace = allocation->created().ptr - lower.lower;
            }

            auto created = isUnit(lower.global, instruction.type) || erasedResult ? 0 : 1;
            auto used = arguments.size() + 1 + (envValue ? 1 : 0) + (erasedResult ? 1 : 0);

            result = call(lower.lower, lower.to, block, created, used, kDefaultCallType, [&](LowerInstCall* call) {
                if(created) {
                    new (call->created().ptr) LowerValue(call, lowerType(lower.global, instruction.type), instruction.name);
                }

                call->used()[0] = address;

                Size index = 1;
                if(envValue) call->used()[index++] = envValue;
                if(erasedResult) call->used()[index++] = returnPlace;

                for(auto argument: arguments) call->used()[index++] = argument;
            });

            if(erasedResult) {
                result->source = instruction.source;

                // Storage on the way in, a value on the way out: a result the caller can hold in a
                // register is loaded back out of the storage the erased signature made it use.
                if(concreteResult) {
                    lower.values.add(instValue, returnPlace);
                } else if(!isUnit(lower.global, instruction.type)) {
                    auto loaded = load(lower.lower, lower.to, block, lower.lower[returnPlace],
                                       memoryWidth(lower, instruction.type),
                                       signedType(lower.global, instruction.type),
                                       lowerType(lower.global, instruction.type), instruction.name);

                    lower.values.add(instValue, loaded->created().ptr - lower.lower);
                }

                return nullptr;
            }

            break;
        }
        default:
            assertTrue("unexpected instruction kind for this lowering" == nullptr);
            return nullptr;
    }

    return result;
}
