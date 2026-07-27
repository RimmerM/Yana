#include "generic.h"
#include "expr.h"
#include "name.h"

GenEnv* functionGen(GlobalBase global, const Function& function) {
    if(!function.gen) return nullptr;

    auto env = global[function.gen];
    return env->types.isEmpty() ? nullptr : env;
}

bool hasClassRequirement(GlobalBase global, const GenEnv& env, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    auto classes = env.classes;

    for(auto constraint: classes.contents(global)) {
        if(constraint.typeClass != typeClass) continue;
        if(sameTypes(constraint.args, global, args)) return true;
    }

    return false;
}

// Whether `have(haveArgs)` proves `want(wantArgs)`, by walking `have` up its own superclasses.
// `depth` bounds the walk rather than tracking what has been visited: a superclass cycle is a
// declaration error, and the classes a real hierarchy stacks are few.
static bool impliesClass(Module& module, GlobalPtr<TypeClass> have, Buffer<TypePtr> haveArgs,
                         GlobalPtr<TypeClass> want, Buffer<TypePtr> wantArgs, U32 depth) {
    auto global = *module.types;
    if(!have) return false;

    if(have == want && sameTypes(haveArgs, wantArgs)) return true;
    if(!depth) return false;

    auto env = global[global[have]->gen];
    if(env->types.size() != haveArgs.length) return false;

    for(auto superclass: env->classes.contents(global)) {
        if(!superclass.typeClass) continue;

        // A superclass is written in its own class's variables, so it is expressed in the types
        // this requirement was declared with before being asked about.
        Array<TypePtr> substituted;
        for(auto arg: superclass.args.contents(global)) {
            substituted.push(substituteType(module, arg, haveArgs, superclass.source));
        }

        if(impliesClass(module, superclass.typeClass, toBuffer(substituted), want, wantArgs, depth - 1)) return true;
    }

    return false;
}

bool provesClass(Module& module, const GenEnv& env, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    auto global = *module.types;
    auto classes = env.classes;

    for(auto constraint: classes.contents(global)) {
        Array<TypePtr> have;
        for(auto arg: constraint.args.contents(global)) have.push(arg);

        if(impliesClass(module, constraint.typeClass, toBuffer(have), typeClass, args, 8)) return true;
    }

    return false;
}

void requireClass(Module& module, Function& function, GlobalPtr<TypeClass> typeClass,
                  Buffer<TypePtr> args, LocationId source) {
    auto global = *module.types;
    auto env = functionGen(global, function);

    // A requirement one already in scope implies is not recorded again, so `fn (Num(a)) inc(x: a)`
    // carries the constraint its author wrote and not also the `FromInt(a)` that `Num`'s
    // superclass already guarantees for every instance of it.
    if(!env || provesClass(module, *env, typeClass, args)) return;

    ClassConstraint constraint;
    constraint.typeClass = typeClass;
    constraint.name = global[typeClass]->name;
    constraint.source = source;
    for(auto arg: args) constraint.args.push(module.types, arg);

    env->classes.push(module.types, constraint);
}

/*
 * Cloning.
 *
 * The clone walks the resolved body once and rebuilds it in a new function, substituting types
 * and mapping every handle to its counterpart. Three things need care:
 *
 *  - phis reference values defined after them, so their shells are created before any instruction
 *    and their inputs filled once everything exists;
 *  - locals are copied position for position, because a Place addresses one by index and those
 *    indices are baked into instructions that are being copied verbatim;
 *  - constants belong to no block, so they are cloned the first time an operand names one.
 */
struct Clone {
    Clone(Module& module, Module& site, Function& from, Function& to, Buffer<TypePtr> args, LocationId source):
        module(module), site(site), context(module.context), global(*module.types), local(*module.arena),
        from(from), resolver(module.context, module, to), args(args), source(source) {}

    // Where the clone is built, and where the call that asked for it was written. They differ
    // whenever a generic function is instantiated from another module, and the difference matters:
    // the requirements are proved against the instances the *caller* can see, so a nested generic
    // call has to keep asking on the original caller's behalf rather than on its own module's.
    Module& module;
    Module& site;
    Context& context;
    GlobalBase global;
    ModuleBase local;
    Function& from;
    ExprResolver resolver;
    Buffer<TypePtr> args;
    LocationId source;

    // Keyed by region offset, which is the identity the rest of the resolver uses too.
    HashMap<U32, U32> values;
    HashMap<U32, U32> blocks;

    // Cleared once a call inside the body turned out not to be instantiable. The clone runs to
    // the end anyway, but stops claiming that the holes it now has are compiler bugs.
    bool ok = true;
};

static TypePtr cloneType(Clone& clone, TypePtr type) {
    return substituteType(clone.module, type, clone.args, clone.source);
}

static ModulePtr<Block> cloneBlock(Clone& clone, ModulePtr<Block> block) {
    if(!block) return nullptr;

    auto found = clone.blocks.getValue(block);
    return found ? ModulePtr<Block>(found.unwrap()) : nullptr;
}

static ModulePtr<Value> cloneValue(Clone& clone, ModulePtr<Value> value) {
    if(!value) return nullptr;

    if(auto found = clone.values.getValue(value)) return ModulePtr<Value>(found.unwrap());

    auto source = clone.local[value];
    auto type = cloneType(clone, source->type);
    ModulePtr<Value> result = nullptr;

    switch(source->kind) {
        case Value::ConstInt:
            result = clone.resolver.constant<ConstInt>(source->source, type, ((ConstInt*)source)->value);
            break;
        case Value::ConstFloat:
            result = clone.resolver.constant<ConstFloat>(source->source, type, ((ConstFloat*)source)->value);
            break;
        case Value::ConstDouble:
            result = clone.resolver.constant<ConstDouble>(source->source, type, ((ConstDouble*)source)->value);
            break;
        default:
            // Everything else is created before anything can use it, so reaching this means the
            // body was not in the order the clone assumes - unless an earlier call in it already
            // failed, in which case the missing value is that failure and not a new one.
            if(clone.ok) {
                clone.context.diagnostics.error("internal: generic body references a value before it is defined"_v,
                                                source->source);
            }

            return nullptr;
    }

    clone.values.add(value, result);
    return result;
}

static Place clonePlace(Clone& clone, const Place& place) {
    Place result = place;
    result.projections = {};

    // A local index and a global are the same in the clone; a pointer root is a value of the
    // body being cloned and has to be mapped like any other operand.
    if(place.root == PlaceRoot::Pointer || place.root == PlaceRoot::Borrow) {
        result.pointer = cloneValue(clone, place.pointer);
    }

    auto projections = place.projections;

    for(auto projection: projections.contents(clone.local)) {
        result.projections.push(clone.module.arena, Projection {
            projection.kind, projection.index, cloneValue(clone, projection.value),
        });
    }

    return result;
}

// Turns one InstGenCall into the call it always meant: the class implementation for the now-known
// types, or the callee's specialization. An intrinsic implementation expands here exactly as it
// would at an ordinary call site, so a specialized `x + x` is an `add` rather than a call.
static void cloneGenCall(Clone& clone, InstGenCall& call) {
    Array<TypePtr> typeArgs;
    for(auto arg: call.typeArgs.contents(clone.local)) typeArgs.push(cloneType(clone, arg));

    Array<ModulePtr<Value>> args;
    for(auto arg: call.args.contents(clone.local)) args.push(cloneValue(clone, arg));

    ModulePtr<Function> callee = nullptr;

    if(call.typeClass) {
        auto instance = matchInstance(clone.site, call.typeClass, toBuffer(typeArgs));

        // The requirements were all proved before cloning started, so a miss here is a compiler
        // bug rather than a program error.
        if(!instance) {
            clone.context.diagnostics.error("internal: no instance for a proved requirement of %@"_v,
                                            call.source, clone.context.findName(clone.from.name));
            clone.ok = false;
            return;
        }

        if(!clone.local[instance.instance]->functions.get(clone.local, call.index)) {
            clone.ok = false;
            return;
        }

        // The implementation of a parametric instance is itself generic, so it is specialized (or
        // expanded) for what selecting the instance bound - the same step an ordinary call site
        // takes, which is why both go through emitInstanceCall.
        auto pointer = (ModulePtr<Value>)((Inst*)&call - clone.local);
        auto result = clone.resolver.emitInstanceCall(clone.site, instance.instance, toBuffer(instance.args),
                                                      call.index, toBuffer(args), call.source, nullptr, call.name);

        if(result) clone.values.add(pointer, result);
        return;
    } else if(clone.local[call.callee]->intrinsic) {
        // A generic intrinsic is generated rather than instantiated, here for the same reason it
        // is at an ordinary call site: there is no body for these types until there are types.
        auto pointer = (ModulePtr<Value>)((Inst*)&call - clone.local);
        auto result = clone.resolver.expandIntrinsic(call.callee, toBuffer(typeArgs), toBuffer(args),
                                                     call.source, call.name);

        if(result) clone.values.add(pointer, result);
        else clone.ok = false;

        return;
    } else {
        callee = instantiateFunction(clone.site, call.callee, toBuffer(typeArgs), call.source);
    }

    if(!callee) {
        clone.ok = false;
        return;
    }

    auto pointer = (ModulePtr<Value>)((Inst*)&call - clone.local);
    auto result = clone.resolver.emitDirectCall(callee, toBuffer(args), call.source, nullptr, call.name);
    if(result) clone.values.add(pointer, result);
}

static void cloneInstruction(Clone& clone, Inst& inst) {
    auto pointer = (ModulePtr<Value>)(&inst - clone.local);
    auto type = cloneType(clone, inst.type);
    auto& resolver = clone.resolver;
    Value* result = nullptr;

    switch(inst.kind) {
        case Value::Alloc:
            result = resolver.emit<InstAlloc>(inst.source, inst.name, type, ((InstAlloc&)inst).local);
            break;
        case Value::LoadPlace:
            result = resolver.emit<InstLoadPlace>(inst.source, inst.name, type,
                                                  clonePlace(clone, ((InstLoadPlace&)inst).place));
            break;
        case Value::Init:
        case Value::Assign: {
            auto& init = (InstInit&)inst;
            result = resolver.emit<InstInit>(inst.source, inst.name, type, clonePlace(clone, init.place),
                                             cloneValue(clone, init.value), inst.kind);
            break;
        }
        case Value::Move: {
            // The Sink and Copy implementations are deliberately not carried across. A clone is
            // being made for concrete types, and which implementation serves them is a question
            // only the substituted type can answer - see finishOwnership, which runs on the
            // specialization the same way it runs on any other function.
            result = resolver.emit<InstMove>(inst.source, inst.name, type,
                                             clonePlace(clone, ((InstMove&)inst).place));
            break;
        }
        case Value::Copy: {
            auto cloned = resolver.emit<InstCopy>(inst.source, inst.name, type,
                                                  clonePlace(clone, ((InstCopy&)inst).place));

            if(((InstCopy&)inst).local != maxLimit<U32>) {
                cloned->local = resolver.function.addLocal(clone.module, type, inst.name,
                                                           resolver.ref(cloned));
            }

            result = cloned;
            break;
        }
        case Value::Borrow: {
            auto& borrow = (InstBorrow&)inst;
            result = resolver.emit<InstBorrow>(inst.source, inst.name, type,
                                               clonePlace(clone, borrow.place), borrow.mut);
            break;
        }
        case Value::Address:
            result = resolver.emit<InstAddress>(inst.source, inst.name, type,
                                                clonePlace(clone, ((InstAddress&)inst).place));
            break;
        case Value::Native: {
            auto& native = (InstNative&)inst;
            auto cloned = resolver.create<InstNative>(inst.source, inst.name, type, native.op);

            for(auto arg: native.args.contents(clone.local)) {
                cloned->args.push(clone.module.arena, cloneValue(clone, arg));
            }

            resolver.append(cloned);
            result = cloned;
            break;
        }
        case Value::Cast:
        case Value::Neg:
        case Value::Not:
            result = resolver.emit<InstUnary>(inst.source, inst.name, type, inst.kind,
                                              cloneValue(clone, ((InstUnary&)inst).from));
            break;
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
            auto& binary = (InstBinary&)inst;
            result = resolver.emit<InstBinary>(inst.source, inst.name, type, inst.kind,
                                               cloneValue(clone, binary.lhs), cloneValue(clone, binary.rhs));
            break;
        }
        case Value::Cmp: {
            auto& compare = (InstCmp&)inst;
            result = resolver.emit<InstCmp>(inst.source, inst.name, type, cloneValue(clone, compare.lhs),
                                            cloneValue(clone, compare.rhs), compare.cmp);
            break;
        }
        case Value::Call: {
            auto& call = (InstCall&)inst;
            Array<ModulePtr<Value>> args;
            for(auto arg: call.args.contents(clone.local)) args.push(cloneValue(clone, arg));

            auto value = resolver.emitDirectCall(call.callee, toBuffer(args), inst.source, nullptr, inst.name);
            if(value) clone.values.add(pointer, value);
            return;
        }
        case Value::GenCall:
            cloneGenCall(clone, (InstGenCall&)inst);
            return;
        case Value::Je: {
            auto& branch = (InstJe&)inst;
            resolver.emit<InstJe>(inst.source, 0, type, cloneValue(clone, branch.cond),
                                  cloneBlock(clone, branch.thenBlock), cloneBlock(clone, branch.elseBlock));
            return;
        }
        case Value::Jmp:
            resolver.emit<InstJmp>(inst.source, 0, type, cloneBlock(clone, ((InstJmp&)inst).target));
            return;
        case Value::Ret:
            resolver.emit<InstRet>(inst.source, 0, type, cloneValue(clone, ((InstRet&)inst).value));
            return;
        default:
            clone.context.diagnostics.error("internal: this instruction cannot be specialized"_v, inst.source);
            return;
    }

    if(result) clone.values.add(pointer, resolver.ref((Inst*)result));
}

static void cloneBody(Clone& clone, Function& to) {
    auto local = clone.local;
    auto& from = clone.from;

    // Block 0 already exists; the rest are created up front so that a branch can name a block
    // that has not been walked yet.
    Size index = 0;
    for(auto blockPointer: from.blocks.contents(local)) {
        auto target = index ? to.addBlock(clone.module) - local : to.blocks.get(local, 0);
        clone.blocks.add(blockPointer, target);
        index++;
    }

    for(auto argPointer: from.args.contents(local)) {
        auto arg = local[argPointer];
        auto created = to.addArg(clone.module, arg->name, cloneType(clone, arg->type), arg->source);
        created->convention = arg->convention;
        created->returnRoot = arg->returnRoot;
        clone.values.add((ModulePtr<Value>)argPointer, (ModulePtr<Value>)(created - local));
    }

    // A Place names a local by index, so the table is copied position for position before any
    // instruction that addresses it. The value each one holds is filled in afterwards, once the
    // instruction that produced it has been cloned.
    for(Size i = 0; i < from.localCount(); i++) {
        auto slot = from.localAt(local, U32(i));
        to.locals.push(clone.module.arena, Local {
            cloneType(clone, slot.type), slot.name, nullptr, slot.convention, slot.storage, slot.borrowed,
        });
    }

    // Phi shells first: a phi is the one instruction whose operands need not dominate it, so
    // anything else may reference one before the block it lives in has been reached.
    for(auto blockPointer: from.blocks.contents(local)) {
        for(auto phiPointer: local[blockPointer]->phis.contents(local)) {
            auto phi = local[phiPointer];
            clone.resolver.current = cloneBlock(clone, blockPointer);

            auto created = clone.resolver.create<InstPhi>(phi->source, phi->name, cloneType(clone, phi->type));
            clone.values.add((ModulePtr<Value>)phiPointer, (ModulePtr<Value>)(created - local));
        }
    }

    for(auto blockPointer: from.blocks.contents(local)) {
        auto block = local[blockPointer];
        clone.resolver.current = cloneBlock(clone, blockPointer);

        for(auto instruction: block->instructions.contents(local)) {
            cloneInstruction(clone, *local[instruction]);
        }

        if(block->terminator) cloneInstruction(clone, *local[block->terminator]);
    }

    for(auto blockPointer: from.blocks.contents(local)) {
        for(auto phiPointer: local[blockPointer]->phis.contents(local)) {
            auto phi = local[phiPointer];
            auto created = (InstPhi*)local[ModulePtr<Value>(clone.values.getValue(phiPointer).unwrap())];

            for(auto input: phi->inputs.contents(local)) {
                created->inputs.push(clone.module.arena, PhiInput {
                    cloneBlock(clone, input.block), cloneValue(clone, input.value),
                });
            }

            local[cloneBlock(clone, blockPointer)]->add(clone.module, created);
        }
    }

    for(Size i = 0; i < from.localCount(); i++) {
        auto slot = to.localAt(local, U32(i));
        slot.value = cloneValue(clone, from.localAt(local, U32(i)).value);
        to.locals.set(local, i, slot);
    }
}

/*
 * Instantiation.
 */

// The printed name of one specialization: `swap(Int, Bool)`. Like an instance implementation,
// it is not addressable in source but everything downstream needs a unique name.
static StringId specializationName(Module& module, Function& generic, Buffer<TypePtr> args) {
    StringBuilder text;
    text << module.context.findName(generic.name) << '(';
    describeTypes(module.context, *module.types, args, text);
    text << ')';

    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

// Proves every requirement of the context for these arguments. Reported against the requirement
// rather than against the call inside the body that needs it: `Ord(a)` is what the signature
// promises, and `Ord(Shape)` is what the caller failed to supply.
static bool proveRequirements(Module& from, Function& generic, GenEnv& env, Buffer<TypePtr> args, LocationId source) {
    auto& context = from.context;
    auto ok = true;

    for(auto constraint: env.classes.contents(*from.types)) {
        if(!constraint.typeClass) continue;

        Array<TypePtr> concrete;
        for(auto arg: constraint.args.contents(*from.types)) {
            concrete.push(substituteType(from, arg, args, source));
        }

        if(findInstance(from, constraint.typeClass, toBuffer(concrete))) continue;

        StringBuilder text;
        describeTypes(context, *from.types, toBuffer(concrete), text);

        context.diagnostics.error("no instance of %@ for (%@), required by %@"_v, source,
                                  context.findName((*from.types)[constraint.typeClass]->name),
                                  text.view(), context.findName(generic.name));
        ok = false;
    }

    return ok;
}

ModulePtr<Function> instantiateFunction(Module& from, ModulePtr<Function> pointer, Buffer<TypePtr> args,
                                        LocationId source) {
    auto& context = from.context;
    auto global = *from.types;
    auto local = *from.arena;
    auto generic = local[pointer];

    auto env = functionGen(global, *generic);
    if(!env || env->types.size() != args.length) {
        context.diagnostics.error("internal: %@ cannot be instantiated with these arguments"_v, source,
                                  context.findName(generic->name));
        return nullptr;
    }

    for(auto arg: args) {
        if(!isGeneric(global, arg)) continue;

        context.diagnostics.error("%@ cannot be instantiated for %@ - every type argument must be concrete"_v,
                                  source, context.findName(generic->name), describeType(context, global, arg));
        return nullptr;
    }

    for(auto existing: generic->specializations.contents(local)) {
        if(sameTypes(local[existing]->genericArgs, local, args)) return existing;
    }

    if(generic->resolving) {
        context.diagnostics.error("%@ cannot be instantiated from inside its own body"_v, source,
                                  context.findName(generic->name));
        return nullptr;
    }

    // Reaching a generic function that is already being cloned, with arguments the cache did not
    // match, means each instantiation asks for another: `f(a)` calling `f(Maybe(a))` has no
    // finite set of specializations.
    if(generic->instantiating) {
        context.diagnostics.error("%@ is polymorphically recursive - it would need endlessly many specializations"_v,
                                  source, context.findName(generic->name));
        return nullptr;
    }

    // The body comes first, and not only because it has to exist before it can be cloned: it is
    // what collects the requirements the signature did not declare, so proving them before it has
    // been resolved would prove a shorter list than the one the clone needs.
    auto& owner = *generic->module;
    if(!resolveFunctionBody(owner, *generic)) return nullptr;
    if(!proveRequirements(from, *generic, *env, args, source)) return nullptr;

    auto specialized = addAnonymousFunction(owner, specializationName(owner, *generic, args), generic->source);
    specialized->specializationOf = pointer;
    specialized->returnType = substituteType(owner, generic->returnType, args, source);
    specialized->used = true;
    for(auto arg: args) specialized->genericArgs.push(owner.arena, arg);

    // Registered before the body is cloned, so a recursive call that substitutes to these same
    // arguments finds this function instead of instantiating a second one forever.
    generic->specializations.push(owner.arena, specialized - local);
    generic->instantiating = true;

    Clone clone(owner, from, *generic, *specialized, args, source);
    cloneBody(clone, *specialized);

    generic->instantiating = false;
    return specialized - local;
}
