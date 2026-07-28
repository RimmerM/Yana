#include "witness.h"
#include "analyze.h"
#include "expr.h"
#include "generic.h"
#include "name.h"

TypePtr typeDescPlaceType(Module& module) {
    auto& context = module.context;
    auto word = module.scalar.int_;
    auto address = resolvePointerType(module, module.scalar.unit);

    Field fields[] = {
        Field { word, context.addUnqualifiedName("logicalType", 11), 0 },
        Field { word, context.addUnqualifiedName("size", 4), 0 },
        Field { word, context.addUnqualifiedName("align", 5), 0 },
        Field { word, context.addUnqualifiedName("stride", 6), 0 },
        Field { word, context.addUnqualifiedName("flags", 5), 0 },
        Field { address, context.addUnqualifiedName("moveInit", 8), 0 },
        Field { address, context.addUnqualifiedName("reclaim", 7), 0 },
        Field { address, context.addUnqualifiedName("drop", 4), 0 },
    };

    auto tuple = resolveTupleType(module, { fields, 8 }, kNullLocation);
    auto base = *module.types;

    // One layout described twice, so the two descriptions are checked against each other rather
    // than believed. Five 32-bit words followed by three addresses is what puts `moveInit` at 24.
    assertTrue(tuple->fields.get(base, TypeDescFields::kMoveInit).offset == TypeDescLayout::kMoveInit);
    assertTrue(tuple->fields.get(base, TypeDescFields::kReclaim).offset == TypeDescLayout::kReclaim);
    assertTrue(tuple->fields.get(base, TypeDescFields::kDrop).offset == TypeDescLayout::kDrop);
    assertTrue(tuple->repr.size == TypeDescLayout::kSize_);

    return (Type*)tuple - base;
}

TypePtr funValueFieldType(Module& module, U16 field) {
    if(field == FunValueLayout::kDesc) return resolvePointerType(module, typeDescPlaceType(module));
    return resolvePointerType(module, module.scalar.unit);
}

/*
 * Building the constants.
 *
 * Everything here follows the same three steps: allocate the bytes in the module arena, write the
 * scalar fields into them, and record a relocation for each address the table holds. The addresses
 * themselves are deliberately not resolved - a witness may name a function that has not been
 * generated yet, and none of them has an address at all until the module is placed.
 */

namespace {

// A global nothing in the source can name. Interned tables need a unique symbol for printing and
// linking, and this is the same trick addAnonymousFunction plays for the same reason.
Global* addAnonymousGlobal(Module& module, StringId name, LocationId source) {
    auto global_ = new (module.arena) Global(&module, name);
    global_->source = source;
    global_->type = module.scalar.unit;
    global_->used = true;
    module.globalOrder.push(module.arena, global_ - *module.arena);
    return global_;
}

struct TableBuilder {
    TableBuilder(Module& module, Size size): module(module) {
        bytes = ByteBuffer((Byte*)module.arena.alloc(size), size);
        set(bytes.ptr, size, 0);
    }

    void putU32(U32 offset, U32 value) {
        copy((const Byte*)&value, bytes.ptr + offset, sizeof(U32));
    }

    // An address is left as zeroes and recorded instead. A null target records nothing, which is
    // how "this type has no drop" reaches the reader as a null slot rather than as a missing entry.
    void putFunction(Global& target, U32 offset, ModulePtr<Function> function) {
        if(!function) return;

        (*module.arena)[function]->used = true;
        target.relocations.push(module.arena, GlobalRelocation { offset, function, nullptr });
    }

    // An interned type, stored as its region offset. Recorded as well as written, so that printing
    // can name the type rather than the number - see Global::typeWords.
    void putType(Global& target, U32 offset, TypePtr type) {
        putU32(offset, U32(type));
        target.typeWords.push(module.arena, offset);
    }

    // An interned class, recorded for the same reason a type is - see Global::classWords.
    void putClass(Global& target, U32 offset, GlobalPtr<TypeClass> typeClass) {
        putU32(offset, U32(typeClass));
        target.classWords.push(module.arena, offset);
    }

    void putGlobal(Global& target, U32 offset, ModulePtr<Global> global_) {
        if(!global_) return;

        (*module.arena)[global_]->used = true;
        target.relocations.push(module.arena, GlobalRelocation { offset, nullptr, global_ });
    }

    Module& module;
    ByteBuffer bytes;
};

/*
 * The teardown a type with nothing to run gets.
 *
 * A descriptor's lifecycle slots are never null, which is what lets erased code call them without
 * first testing them: one unconditional indirect call per half, instead of a load, a comparison and
 * a split block at every drop of a generic value. The flags still record which halves are empty, for
 * a pass that would rather skip the call than make it.
 *
 * One per program rather than one per type: it does nothing, so there is nothing to distinguish.
 */
ModulePtr<Function> emptyTeardown(Module& module, LocationId source) {
    auto& program = module.program;
    if(program.emptyTeardown) return program.emptyTeardown;

    auto& core = *program.core;
    auto function = addAnonymousFunction(core, module.context.addQualifiedName("teardown$none", 13, 1), source);
    function->returnType = core.scalar.unit;
    function->used = true;

    auto name = module.context.addQualifiedName("value", 5, 1);
    function->addArg(core, name, resolvePointerType(core, core.scalar.unit), source);

    ExprResolver resolver(core.context, core, *function);
    resolver.terminate(resolver.emit<InstRet>(source, 0, core.scalar.unit, nullptr));

    program.emptyTeardown = function - *core.arena;
    return program.emptyTeardown;
}

StringId tableName(Module& module, StringView prefix, TypePtr type) {
    StringBuilder text;
    text << prefix;
    describeType(module.context, *module.types, type, text);
    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

} // namespace

/*
 * moveInit.
 *
 * Implementation-Generics.md part 4 lists three answers - a block copy for TrivialSink, an adapter
 * over the authored `Sink` where one exists, and unavailable - and the first two are generated the
 * same way: a two-argument function taking the destination and the source as raw pointers, so that
 * generic code can call it without knowing either type or size.
 *
 * The "unavailable" case is not represented here at all, deliberately. Whether a body may move a
 * value of an unknown type is a question its *schema* answers, and it is answered before any of
 * this is reached; a descriptor that carried "you may not" would be inviting a second, later check
 * of something already settled.
 */
ModulePtr<Function> moveInitFor(Module& module, TypePtr type, LocationId source) {
    auto& program = module.program;
    if(!type || isGeneric(*module.types, type)) return nullptr;

    if(auto found = program.moveInitGlue.get(U32(type))) return found.unwrap();

    auto ownership = ownershipOf(module, type);
    auto size = typeSize(*module.types, type);

    // Nothing to relocate: a zero-sized type, or one whose bytes the caller has already placed.
    if(!size) return nullptr;

    auto function = addAnonymousFunction(module, tableName(module, "moveInit$"_v, type), source);
    auto pointer = function - *module.arena;
    *program.moveInitGlue.add(U32(type)).value = pointer;

    function->returnType = module.scalar.unit;
    function->used = true;

    auto pointerType = resolvePointerType(module, type);
    auto to = function->addArg(module, module.context.addQualifiedName("to", 2, 1), pointerType, source);
    auto from = function->addArg(module, module.context.addQualifiedName("from", 4, 1), pointerType, source);

    ExprResolver resolver(module.context, module, *function);

    if(ownership.trivialSink) {
        // A relocation of a TrivialSink value is its bytes and nothing else, which is what the
        // class means. copyMemory rather than a load and a store because the size is a constant
        // here and the type may be an aggregate with no register form at all.
        auto bytes = resolver.makeInt(source, module.scalar.long_, size);
        auto byteType = resolvePointerType(module, module.scalar.unit);

        auto castTo = resolver.ref(resolver.emit<InstUnary>(source, 0, byteType, Value::Cast,
                                                            (ModulePtr<Value>)(to - *module.arena)));
        auto castFrom = resolver.ref(resolver.emit<InstUnary>(source, 0, byteType, Value::Cast,
                                                              (ModulePtr<Value>)(from - *module.arena)));

        auto copyInst = resolver.create<InstNative>(source, 0, module.scalar.unit, NativeOp::CopyMemory);
        copyInst->args.push(module.arena, castTo);
        copyInst->args.push(module.arena, castFrom);
        copyInst->args.push(module.arena, bytes);
        resolver.append(copyInst);
    } else if(ownership.authoredSink) {
        // The authored `Sink` takes `(&to: a, ->from: a)`, and both conventions arrive as addresses,
        // so the two pointers this function was given are already the arguments it wants.
        TypePtr args[] = { type };
        auto match = matchInstance(module, module.coreClasses.sink, toBuffer(args));

        if(match && !(*module.arena)[match.instance]->functions.isEmpty()) {
            auto implementation = (*module.arena)[match.instance]->functions.get(*module.arena, 0);

            if(implementation && (*module.arena)[implementation]->gen) {
                implementation = instantiateFunction(module, implementation, toBuffer(match.args), source);
            }

            if(implementation) {
                (*module.arena)[implementation]->used = true;

                auto call = resolver.create<InstCall>(source, 0, module.scalar.unit, implementation);
                call->args.push(module.arena, (ModulePtr<Value>)(to - *module.arena));
                call->args.push(module.arena, (ModulePtr<Value>)(from - *module.arena));
                resolver.append(call);
            }
        }
    }

    resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, nullptr));
    return pointer;
}

/*
 * Whether a place can be addressed with constant offsets.
 *
 * Lowering walks a projection path accumulating byte offsets read off each type it passes through,
 * and a generic aggregate has none: `Pair(a, b).second` sits at an offset that depends on what `a`
 * turned out to be, and a body compiled once for every `a` has no constant to use. What it needs is
 * the composite descriptor Implementation-Generics.md part 4 calls `reprOps`, whose "scoped
 * constructor-content projection" is exactly this.
 *
 * Until that exists, a body that projects into a generic type specializes instead. Depending on the
 * declaration's own offsets happening to be right - which they are whenever every field before the
 * projected one has a size independent of the arguments - would be an accident rather than a rule.
 */
static bool lowerablePlace(Module& module, Function& owner, const Place& place) {
    auto local = *module.arena;
    auto global = *module.types;
    auto projections = place.projections;
    if(projections.isEmpty()) return true;

    TypePtr type = nullptr;

    switch(place.root) {
        case PlaceRoot::Local:
            if(place.local >= owner.localCount()) return false;
            type = owner.localAt(local, place.local).type;
            break;
        case PlaceRoot::Global:
            type = local[place.global]->type;
            break;
        case PlaceRoot::Pointer:
            type = pointeeType(global, local[place.pointer]->type);
            break;
        case PlaceRoot::Borrow:
            type = ((BorrowType*)global[local[place.pointer]->type])->to;
            break;
    }

    for(auto projection: projections.contents(local)) {
        if(!type) return false;
        if(isGeneric(global, type)) return false;

        switch(projection.kind) {
            case ProjectionKind::Discriminant:
                type = module.scalar.int_;
                break;
            case ProjectionKind::Downcast:
                type = ((RecordType*)global[type])->constructors.get(global, projection.index).content;
                break;
            case ProjectionKind::Field:
                // A function value's three words are at fixed offsets whatever the body's type
                // arguments turn out to be, so it is projected into like any other aggregate and
                // needs no composite descriptor to do it.
                type = global[type]->kind == Type::Fun
                    ? funValueFieldType(module, projection.index)
                    : ((TupType*)global[type])->fields.get(global, projection.index).type;

                break;
            case ProjectionKind::Deref:
                type = pointeeType(global, type);
                break;
            case ProjectionKind::Index:
                return false;
        }
    }

    return true;
}

static bool lowerablePlaces(Module& module, Function& owner, Value& inst) {
    switch(inst.kind) {
        case Value::LoadPlace: return lowerablePlace(module, owner, ((InstLoadPlace&)inst).place);
        case Value::Init:
        case Value::Assign: return lowerablePlace(module, owner, ((InstInit&)inst).place);
        case Value::Borrow: return lowerablePlace(module, owner, ((InstBorrow&)inst).place);
        case Value::Move: return lowerablePlace(module, owner, ((InstMove&)inst).place);
        case Value::Address: return lowerablePlace(module, owner, ((InstAddress&)inst).place);
        case Value::Drop: return lowerablePlace(module, owner, ((InstDrop&)inst).place);
        default: return true;
    }
}

/*
 * The erased entry point of one class method.
 *
 * A generic body that deferred a dispatch knows only the class *signature*, written over the class's
 * own type variables - so it calls with the shape that signature has, not the shape the instance's
 * implementation has. Those differ exactly where the signature is generic: an argument declared `a`
 * arrives as an address whatever `a` turned out to be, and a result declared `a` goes back through
 * caller storage for the same reason.
 *
 * The thunk is the adapter between the two. It takes that erased shape, reads the concrete values
 * out of it, calls the implementation the ordinary way - which expands an intrinsic exactly as any
 * call site does, so a specialized `<` on Int stays one `cmp` behind the pointer - and writes the
 * result back where the caller asked.
 */
static ModulePtr<Function> erasedThunkFor(Module& module, GlobalPtr<TypeClass> typeClass,
                                          const InstanceMatch& match, Buffer<TypePtr> classArgs,
                                          U16 index, LocationId source) {
    auto& context = module.context;
    auto global = *module.types;
    auto local = *module.arena;

    auto entry = global[typeClass]->functions.get(global, index);
    if(!entry.fun) return nullptr;

    auto signature = local[entry.fun];

    // The method index rather than only the name, because a class may declare one name at two
    // arities - `Num` declares both the binary and the unary `-` - and a thunk per method needs a
    // symbol per method. Everything downstream is keyed by name, so a collision here would silently
    // merge two functions rather than being reported.
    StringBuilder text;
    text << "thunk$" << context.findName(global[typeClass]->name) << '(';
    describeTypes(context, global, classArgs, text);
    text << ")." << context.findName(entry.name) << '#';
    text.appendValue(U32(index));

    auto function = addAnonymousFunction(module, context.addQualifiedName(text.pointer(), text.size(), 1), source);
    auto pointer = function - local;
    function->returnType = module.scalar.unit;
    function->used = true;

    // The result the caller allocated storage for, when the signature returns something whose size
    // the caller could not know. Declared before the arguments so that the erased shape is the same
    // one an unspecialized generic function has - hidden storage first, then what was written.
    auto concreteResult = substituteType(module, signature->returnType, classArgs, source);
    auto erasedResult = isGeneric(global, signature->returnType);
    Arg* resultArg = nullptr;

    if(erasedResult) {
        resultArg = function->addArg(module, context.addQualifiedName("result", 6, 1),
                                     resolvePointerType(module, concreteResult), source);
    } else {
        function->returnType = concreteResult;
    }

    Array<Arg*> parameters;
    Array<bool> byAddress;

    for(auto argPointer: signature->args.contents(local)) {
        auto declared = local[argPointer]->type;
        auto concrete = substituteType(module, declared, classArgs, source);
        auto erased = isGeneric(global, declared);

        byAddress.push(erased);
        parameters.push(function->addArg(module, local[argPointer]->name,
                                         erased ? resolvePointerType(module, concrete) : concrete, source));
    }

    ExprResolver resolver(context, module, *function);
    Array<ModulePtr<Value>> args;

    for(Size i = 0; i < parameters.size(); i++) {
        auto value = (ModulePtr<Value>)(parameters[i] - local);
        args.push(byAddress[i] ? resolver.load(Place::atPointer(value), source) : value);
    }

    Array<TypePtr> instanceArgs;
    for(auto arg: match.args) instanceArgs.push(arg);

    auto errors = context.diagnostics.errorCount();
    auto result = resolver.emitInstanceCall(module, match.instance, toBuffer(instanceArgs), index,
                                            toBuffer(args), source);

    if(context.diagnostics.errorCount() != errors) return nullptr;

    if(erasedResult) {
        // The storage arrived uninitialized, so this is an Init rather than an Assign: there is no
        // old value in it for anything to have to release.
        if(result) {
            resolver.initialize(Place::atPointer((ModulePtr<Value>)(resultArg - local)), result, source);
        }

        resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, nullptr));
    } else {
        resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, result));
    }

    return pointer;
}

ModulePtr<Global> classWitnessFor(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                                  LocationId source) {
    auto& context = module.context;
    auto global = *module.types;
    auto& program = module.program;

    for(auto& existing: program.classWitnesses) {
        if(existing.typeClass != typeClass) continue;
        if(sameTypes(toBuffer(existing.args), args)) return existing.witness;
    }

    for(auto arg: args) {
        if(isGeneric(global, arg)) return nullptr;
    }

    auto match = matchInstance(module, typeClass, args);
    if(!match) {
        StringBuilder types;
        describeTypes(context, global, args, types);

        context.diagnostics.error("no instance of %@ for (%@)"_v, source,
                                  context.findName(global[typeClass]->name), types.view());
        return nullptr;
    }

    auto methodCount = U16(global[typeClass]->functions.size());
    auto argCount = U16(args.length);

    StringBuilder text;
    text << "witness$" << context.findName(global[typeClass]->name) << '(';
    describeTypes(context, global, args, text);
    text << ')';

    auto global_ = addAnonymousGlobal(module, context.addQualifiedName(text.pointer(), text.size(), 1), source);
    auto pointer = global_ - *module.arena;

    // Registered before the thunks are generated, since one of them can ask for this witness again -
    // a class whose default implementation calls another method of the same class. The entry is
    // built whole and then pushed: generating a thunk can push another witness, and a reference into
    // a list that reallocates under it would be writing into freed storage.
    InternedWitness interned { typeClass, Array<TypePtr>(), pointer };
    for(auto arg: args) interned.args.push(arg);
    program.classWitnesses.push(::move(interned));

    TableBuilder table(module, ClassWitnessLayout::sizeFor(argCount, methodCount));
    table.putU32(ClassWitnessLayout::kArgCount, U32(argCount) | (U32(methodCount) << 16));
    global_->contents = table.bytes;
    table.putClass(*global_, ClassWitnessLayout::kClass, typeClass);

    for(U16 i = 0; i < argCount; i++) {
        auto descriptor = typeDescFor(module, args[i], source);
        if(descriptor) table.putGlobal(*global_, ClassWitnessLayout::kArgs + 8 * i, descriptor);
    }

    auto methods = ClassWitnessLayout::methodsOffset(argCount);
    auto ok = true;

    for(U16 i = 0; i < methodCount; i++) {
        // A signature the instance does not implement leaves a null slot rather than failing the
        // whole witness: a body that never calls that method is unaffected, and one that does was
        // already rejected where the call was written.
        if(!(*module.arena)[match.instance]->functions.get(*module.arena, i)) continue;

        auto thunk = erasedThunkFor(module, typeClass, match, args, i, source);
        if(!thunk) {
            ok = false;
            continue;
        }

        table.putFunction(*global_, methods + 8 * i, thunk);
    }

    return ok ? pointer : nullptr;
}

/*
 * Filling one call site's environment.
 *
 * For each slot of the callee's schema, the caller expresses that slot in its *own* terms - by
 * substituting the type arguments it is calling with - and then answers one question: is what came
 * out concrete, or is it one of my own type variables?
 *
 *  - concrete: store the address of an interned constant. This is part 9's static case, and it is
 *    the whole environment whenever the caller is not itself generic.
 *  - one of mine: copy my own slot across. This is the forwarded case, and it is what makes a chain
 *    of generic calls work - `middle` hands `smaller` the `Ord` witness it was handed itself, and
 *    nobody in the chain except the outermost concrete caller ever knows what the type is.
 *
 * A mix of the two is part 9's third case and needs no separate handling: the plan is per slot, so
 * a call that knows two of its four slots concretely simply has two of each.
 */
static bool fillEnvironment(Module& module, Function& caller, InstGenCall& call, GenEnv& calleeEnv,
                            Buffer<TypePtr> typeArgs) {
    auto global = *module.types;
    auto callerEnv = functionGen(global, caller);
    auto& schema = genSchemaOf(module, calleeEnv);
    auto ok = true;
    auto allConstant = true;

    call.fill.clear();

    for(auto slot: schema.slots.contents(global)) {
        GenSlotFill entry;

        if(slot.kind == GenSlotKind::Type) {
            auto expressed = substituteType(module, slot.type, typeArgs, call.source);

            if(isGeneric(global, expressed)) {
                entry.forwarded = callerEnv ? genTypeSlot(module, *callerEnv, expressed) : maxLimit<U16>;
                allConstant = false;
            } else {
                entry.constant = typeDescFor(module, expressed, call.source);
            }
        } else if(slot.kind == GenSlotKind::Class) {
            Array<TypePtr> expressed;
            auto anyGeneric = false;

            for(auto arg: slot.args.contents(global)) {
                auto substituted = substituteType(module, arg, typeArgs, call.source);
                anyGeneric = anyGeneric || isGeneric(global, substituted);
                expressed.push(substituted);
            }

            if(anyGeneric) {
                entry.forwarded = callerEnv
                    ? genClassSlot(module, *callerEnv, slot.typeClass, toBuffer(expressed)) : maxLimit<U16>;
                allConstant = false;
            } else {
                entry.constant = classWitnessFor(module, slot.typeClass, toBuffer(expressed), call.source);
            }
        } else {
            // A property or function requirement. Both need a witness kind that does not exist yet,
            // and the call site falls back to specializing rather than being given a null slot.
            ok = false;
        }

        if(!entry.constant && !entry.isForwarded()) ok = false;
        call.fill.push(module.arena, entry);
    }

    // Every slot concrete is the case worth interning: one constant shared by every call site that
    // supplies the same arguments, rather than a table assembled on each of their frames.
    if(ok && allConstant) {
        call.env = genEnvFor(module, call.callee, typeArgs, call.source);
        if(call.env) call.fill.clear();
    }

    return ok;
}

bool prepareGenericCalls(Program& program) {
    auto local = *program.arena;
    auto global = *program.types;
    auto ok = true;

    /*
     * Which generic bodies actually reach the backend.
     *
     * A call site marks the function it calls, but that function's own calls are only discovered by
     * looking at it - so `middle` being emitted is what makes `smaller` need emitting too. Run to a
     * fixpoint rather than in one pass, since the call graph is not in any particular order and a
     * function may be marked after it has already been walked.
     */
    for(auto changed = true; changed;) {
        changed = false;

        for(auto module: program.modules) {
            for(Size i = 0; i < module->functionOrder.size(); i++) {
                auto function = local[module->functionOrder.get(local, i)];
                if(function->gen && !function->genericallyUsed) continue;

                for(auto blockPointer: function->blocks.contents(local)) {
                    for(auto instruction: local[blockPointer]->instructions.contents(local)) {
                        auto& inst = *local[instruction];
                        if(inst.kind != Value::GenCall) continue;

                        auto& call = (InstGenCall&)inst;
                        if(call.typeClass || !call.callee) continue;

                        auto callee = local[call.callee];
                        if(!callee->gen || callee->genericallyUsed) continue;

                        callee->genericallyUsed = true;
                        callee->used = true;
                        changed = true;
                    }
                }
            }
        }
    }

    for(auto module: program.modules) {
        // Thunks and witnesses are appended while this runs, so the list is walked by index - the
        // same reason resolveModuleBodies and runProgramOwnership do.
        for(Size i = 0; i < module->functionOrder.size(); i++) {
            auto pointer = module->functionOrder.get(local, i);
            auto function = local[pointer];

            // Only a body that will actually be emitted. A generic function every call site
            // specialized has no machine code, so its deferred calls are decided by cloning.
            if(function->gen && !function->genericallyUsed) continue;

            for(auto blockPointer: function->blocks.contents(local)) {
                for(auto instruction: local[blockPointer]->instructions.contents(local)) {
                    auto& inst = *local[instruction];
                    if(inst.kind != Value::GenCall) continue;

                    auto& call = (InstGenCall&)inst;
                    if(call.env) continue;

                    Array<TypePtr> typeArgs;
                    for(auto arg: call.typeArgs.contents(local)) typeArgs.push(arg);

                    if(call.typeClass) {
                        // A deferred class dispatch reads its witness out of the caller's own
                        // environment. Which slot that is could not be recorded when the call was
                        // emitted, because the context was still collecting requirements.
                        auto env = functionGen(global, *function);
                        call.classSlot = env
                            ? genClassSlot(*module, *env, call.typeClass, toBuffer(typeArgs)) : maxLimit<U16>;

                        if(call.classSlot == maxLimit<U16>) {
                            module->context.diagnostics.error("internal: a deferred class call has no witness slot in %@"_v,
                                                              call.source, module->context.findName(function->name));
                            ok = false;
                        }

                        continue;
                    }

                    auto calleeEnv = functionGen(global, *local[call.callee]);
                    if(!calleeEnv) continue;

                    if(!fillEnvironment(*module, *function, call, *calleeEnv, toBuffer(typeArgs))) {
                        module->context.diagnostics.error("internal: %@ cannot be given an environment in %@"_v,
                                                          call.source,
                                                          module->context.findName(local[call.callee]->name),
                                                          module->context.findName(function->name));
                        ok = false;
                    }
                }
            }
        }
    }

    return ok;
}

/*
 * Whether a body can be emitted, and with it every generic body it calls.
 *
 * The recursion is the point rather than an optimization: emitting `middle` means emitting
 * `smaller`, because `middle`'s call to it is a real call that has to land somewhere. So a body is
 * lowerable only if the whole reachable set is, and one function that is not takes the entire chain
 * back to specialization.
 *
 * `visited` is what makes a recursive generic function terminate - it is on the stack, so assuming
 * it lowerable is exactly the right answer while deciding whether it is.
 */
static bool bodyLowerable(Module& module, ModulePtr<Function> function,
                          Array<ModulePtr<Function>>& visited, U32 depth) {
    auto local = *module.arena;
    auto global = *module.types;
    auto target = local[function];

    // A signature or an intrinsic has no body to emit; the first is not callable at all and the
    // second is generated at each call site, so neither is a candidate.
    if(target->signature || target->intrinsic) return false;
    if(!depth) return false;

    for(auto seen: visited) {
        if(seen == function) return true;
    }

    visited.push(function);

    // A field or function requirement needs a witness kind that does not exist yet. Checked on the
    // context rather than on the body, since it is the *contract* that cannot be supplied.
    if(auto env = functionGen(global, *target)) {
        if(env->properties.isNotEmpty() || env->functions.isNotEmpty()) return false;
    }

    for(auto blockPointer: target->blocks.contents(local)) {
        auto block = local[blockPointer];

        for(auto instruction: block->instructions.contents(local)) {
            auto& inst = *local[instruction];

            // An explicit copy of a value whose type the body cannot see needs the `Copy` witness,
            // which does not exist yet - a class witness holds methods, and `Copy` would have to be
            // reached as one rather than through the descriptor's lifecycle slots.
            if(inst.kind == Value::Copy && isGeneric(global, inst.type)) return false;

            // A projection into a generic aggregate. `Pair(a, b).second` sits at an offset that
            // depends on what `a` turned out to be, and a body compiled once for every `a` has no
            // constant to use - it needs the composite descriptor Implementation-Generics.md part 4
            // calls `reprOps`, whose "scoped constructor-content projection" is exactly this.
            //
            // Until that exists the offsets a generic body would compute are the declaration's,
            // which are only right when every field before the projected one has a size independent
            // of the type arguments. Rather than depend on that accident, such a body specializes.
            if(!lowerablePlaces(module, *target, inst)) return false;

            /*
             * A call through a function value whose *type* the body cannot see.
             *
             * `f: (a) -> a` is compiled once for every `a`, so the body passes `x` the way an erased
             * body passes everything - by address - while whatever the caller put in `f` is a
             * concrete function expecting whatever `a` turned out to be. Adapting between the two is
             * what a `FunctionWitness` is for: it carries the environment *and* the shape the
             * callable was compiled at, which a bare `{code, env, envDesc}` does not.
             *
             * Until that exists such a body specializes, which is always available for a concrete
             * argument list - the same staging every other gap here uses.
             */
            if(inst.kind == Value::CallDyn && isGeneric(global, ((InstCallDyn&)inst).signature)) {
                return false;
            }

            if(inst.kind != Value::GenCall) continue;

            auto& call = (InstGenCall&)inst;

            // A deferred class dispatch is supplied from this function's own environment, so what
            // it needs is that the requirement is on the context - which requireClass guarantees.
            if(call.typeClass) continue;

            // A call to another generic function means emitting that one too.
            if(!bodyLowerable(module, call.callee, visited, depth - 1)) return false;
        }
    }

    return true;
}

bool genericBodyLowerable(Module& module, ModulePtr<Function> function) {
    Array<ModulePtr<Function>> visited;
    return bodyLowerable(module, function, visited, 16);
}

ModulePtr<Global> genEnvFor(Module& module, ModulePtr<Function> callee, Buffer<TypePtr> args,
                            LocationId source) {
    auto& context = module.context;
    auto global = *module.types;
    auto local = *module.arena;
    auto& program = module.program;

    auto env = functionGen(global, *local[callee]);
    if(!env) return nullptr;

    for(auto& existing: program.genEnvs) {
        if(existing.callee != callee) continue;
        if(sameTypes(toBuffer(existing.args), args)) return existing.env;
    }

    auto& schema = genSchemaOf(module, *env);
    auto slotCount = schema.slots.size();

    StringBuilder text;
    text << "genEnv$" << context.findName(local[callee]->name) << '(';
    describeTypes(context, global, args, text);
    text << ')';

    auto global_ = addAnonymousGlobal(module, context.addQualifiedName(text.pointer(), text.size(), 1), source);
    auto pointer = global_ - local;

    // Registered before the slots are filled, since building one of them can ask for an environment
    // again - a witness whose implementation is itself generic. Built whole and then pushed, so that
    // a nested request growing the list cannot leave a reference into freed storage.
    InternedEnv interned { callee, Array<TypePtr>(), pointer };
    for(auto arg: args) interned.args.push(arg);
    program.genEnvs.push(::move(interned));

    TableBuilder table(module, GenEnvLayout::sizeFor(slotCount));
    global_->contents = table.bytes;

    auto ok = true;

    for(auto slot: schema.slots.contents(global)) {
        auto offset = GenEnvLayout::slotOffset(slot.index);

        switch(slot.kind) {
            case GenSlotKind::Type: {
                auto concrete = substituteType(module, slot.type, args, source);
                auto descriptor = typeDescFor(module, concrete, source);

                if(!descriptor) {
                    context.diagnostics.error("%@ cannot be passed to generic code - it is not a concrete type"_v,
                                              source, describeType(context, global, concrete));
                    ok = false;
                    break;
                }

                table.putGlobal(*global_, offset, descriptor);
                break;
            }

            case GenSlotKind::Class: {
                Array<TypePtr> concrete;
                for(auto arg: slot.args.contents(global)) {
                    concrete.push(substituteType(module, arg, args, source));
                }

                auto witness = classWitnessFor(module, slot.typeClass, toBuffer(concrete), source);
                if(!witness) {
                    ok = false;
                    break;
                }

                table.putGlobal(*global_, offset, witness);
                break;
            }

            // The two witness kinds that do not exist yet. Each is a separate constraint entry with
            // its own implementation, and neither is derivable from a TypeDesc - knowing a type's
            // size grants nothing else, which is part 1's fifth invariant.
            case GenSlotKind::Property:
            case GenSlotKind::Function:
                context.diagnostics.error("%@ cannot be called generically yet - its %@ requirement needs a witness, which is not built yet"_v,
                                          source, context.findName(local[callee]->name),
                                          slot.kind == GenSlotKind::Property ? "field"_v : "function"_v);
                ok = false;
                break;
        }
    }

    return ok ? pointer : nullptr;
}

ModulePtr<Global> typeDescFor(Module& module, TypePtr type, LocationId source) {
    auto& program = module.program;
    if(!type || isGeneric(*module.types, type)) return nullptr;

    if(auto found = program.typeDescs.get(U32(type))) return found.unwrap();

    auto global_ = addAnonymousGlobal(module, tableName(module, "typeDesc$"_v, type), source);
    auto pointer = global_ - *module.arena;

    // Registered before the lifecycle functions are generated, since generating one for a type
    // reachable from itself asks for this descriptor again.
    *program.typeDescs.add(U32(type)).value = pointer;

    auto ownership = ownershipOf(module, type);
    auto size = typeSize(*module.types, type);
    auto align = typeAlign(*module.types, type);

    TableBuilder table(module, TypeDescLayout::kSize_);
    table.putU32(TypeDescLayout::kSize, size);
    table.putU32(TypeDescLayout::kAlign, align);
    table.putU32(TypeDescLayout::kStride, align ? ((size + align - 1) & ~(align - 1)) : size);

    // Nothing selects a non-canonical Repr yet, and no type declares that it must keep its address,
    // so the only source of a stable-address requirement is a Repr variant - which is Milestone 8's.
    table.putU32(TypeDescLayout::kFlags, typeDescFlags(ownership, false));

    global_->contents = table.bytes;
    table.putType(*global_, TypeDescLayout::kLogicalType, type);

    // Every lifecycle slot holds a callable address, so erased code never has to test one - see
    // emptyTeardown. A type whose bytes are its whole relocation still gets a real moveInit, since
    // that one has a size to copy and is not a no-op.
    auto orEmpty = [&](ModulePtr<Function> implementation) {
        return implementation ? implementation : emptyTeardown(module, source);
    };

    table.putFunction(*global_, TypeDescLayout::kMoveInit, orEmpty(moveInitFor(module, type, source)));
    table.putFunction(*global_, TypeDescLayout::kReclaim,
                      orEmpty(teardownImplementation(module, type, Teardown::Reclaim, source)));
    table.putFunction(*global_, TypeDescLayout::kDrop,
                      orEmpty(teardownImplementation(module, type, Teardown::Drop, source)));

    return pointer;
}
