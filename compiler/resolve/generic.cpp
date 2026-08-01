#include "generic.h"
#include "expr.h"
#include "witness.h"
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

/*
 * Whether `have(haveArgs)` proves `want(wantArgs)`, by walking `have` up its own superclasses, and
 * which superclasses were stepped through to get there.
 *
 * `steps` is the answer emitted code uses rather than a by-product of the search: a witness holds one
 * pointer per superclass its class declares, so each step of the walk is one load at one offset, and
 * the offsets together are the sequence that reaches the wanted witness from the one in hand. Only
 * this walk knows which class each step is in, which is why it records the offsets rather than the
 * indices. `steps` is left holding the successful path and is unwound on every branch that failed,
 * so a caller reading it after a `true` reads that path and nothing from an attempt that did not
 * work out.
 *
 * `depth` bounds the walk rather than tracking what has been visited: a superclass cycle is a
 * declaration error, and the classes a real hierarchy stacks are few.
 */
static bool superclassPath(Module& module, GlobalPtr<TypeClass> have, Buffer<TypePtr> haveArgs,
                           GlobalPtr<TypeClass> want, Buffer<TypePtr> wantArgs,
                           Array<U32>& steps, U32 depth) {
    auto global = *module.types;
    if(!have) return false;

    if(have == want && sameTypes(haveArgs, wantArgs)) return true;
    if(!depth) return false;

    auto env = global[global[have]->gen];
    if(env->types.size() != haveArgs.length) return false;

    U16 index = 0;

    for(auto superclass: env->classes.contents(global)) {
        auto step = index++;
        if(!superclass.typeClass) continue;

        // A superclass is written in its own class's variables, so it is expressed in the types
        // this requirement was declared with before being asked about.
        TypeList substituted;
        for(auto arg: superclass.args.contents(global)) {
            substituted.push(substituteType(module, arg, haveArgs, superclass.source));
        }

        auto depthBefore = U32(steps.size());
        steps.push(classSuperclassSlot(global, have, step));

        if(superclassPath(module, superclass.typeClass, toBuffer(substituted), want, wantArgs,
                          steps, depth - 1)) {
            return true;
        }

        steps.resize(depthBefore);
    }

    return false;
}

bool provesClass(Module& module, const GenEnv& env, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    auto global = *module.types;
    auto classes = env.classes;
    Array<U32> steps;

    for(auto constraint: classes.contents(global)) {
        TypeList have;
        for(auto arg: constraint.args.contents(global)) have.push(arg);

        steps.clear();
        if(superclassPath(module, constraint.typeClass, toBuffer(have), typeClass, args, steps, 8)) return true;
    }

    return false;
}

U16 genWitnessPath(Module& module, GenEnv& env, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                   Array<U32>& supers) {
    auto global = *module.types;
    supers.clear();

    // A slot for exactly this requirement first. The two answers are equally usable, and preferring
    // the direct one keeps a body that declared what it dispatches on loading its witness rather
    // than deriving it from a superclass relation it also happens to have.
    if(auto slot = genClassSlot(module, env, typeClass, args); slot != maxLimit<U16>) return slot;

    auto& schema = genSchemaOf(module, env);

    for(auto slot: schema.slots.contents(global)) {
        if(slot.kind != GenSlotKind::Class) continue;

        TypeList have;
        for(auto arg: slot.args.contents(global)) have.push(arg);

        supers.clear();
        if(superclassPath(module, slot.typeClass, toBuffer(have), typeClass, args, supers, 8)) {
            return slot.index;
        }
    }

    supers.clear();
    return maxLimit<U16>;
}

/*
 * `a.field` in a body that cannot see what `a` is.
 *
 * The constraint is the whole of what makes this legal: a body may read a field of a type variable
 * exactly when its context promises that variable has one. Answers which slot records that promise,
 * or reports and answers maxLimit<U16>.
 *
 * ## Why this does not infer the constraint
 *
 * Implementation-Generics.md part 2 asks for inference in private bodies, and it is not done here,
 * because inferring `a.name: b` means inventing `b` and then *solving* it from how the body uses the
 * field. This resolver has no unification for that: a type variable is bound one-way, positionally,
 * by matchType against a call site's arguments, so an invented variable is one more thing a caller
 * would have to supply and nothing would ever determine it. `fn nameOf(p: a) -> Int = p.name`
 * would resolve to a function of two type arguments with no way to give the second.
 *
 * So the constraint is required, and the diagnostic names exactly what to write. The public/private
 * split that part 2 is really about - a `pub` signature being a contract that a body edit must not
 * change silently - is unaffected by requiring it everywhere: it is the strict end of that rule.
 * Inference is a later addition, and what it waits for is a solver rather than anything here.
 */
U16 requireProperty(Module& module, Function& function, TypePtr owner, StringId field,
                    LocationId source) {
    auto global = *module.types;
    auto env = functionGen(global, function);
    if(!env) return maxLimit<U16>;

    if(auto existing = genPropertySlot(module, *env, owner, field); existing != maxLimit<U16>) {
        return existing;
    }

    StringBuilder text;
    describeType(module.context, global, owner, text);
    text << '.' << module.context.findName(field) << ": <type>";

    module.context.diagnostics.error(
        "%@ reads a field %@ that its context does not promise - add the constraint `%@`"_v,
        source, module.context.findName(function.name), module.context.findName(field),
        text.view());

    return maxLimit<U16>;
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
    invalidateGenSchema(*env);
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

/*
 * Turning `a.name` into `.0` now that `a` is known.
 *
 * The owner comes from the schema slot rather than from walking the place, because the slot is what
 * recorded the constraint and the constraint names the owner directly. Substituting it gives the
 * concrete type, and from there this is the same search projectField does against a record it could
 * see all along - which is the point: one description of where a field is, used twice.
 *
 * A field that is not there is a compiler bug rather than a program error. What makes a caller
 * legal is that its concrete type satisfies the constraint, and that is checked where the
 * specialization is asked for; reaching here with a type that does not have the field would mean
 * that check let something through.
 */
static void resolveProperty(Clone& clone, Place& into, U16 slot, const Place& place) {
    auto global = clone.global;
    auto env = functionGen(global, clone.from);
    if(!env) return;


    auto& schema = genSchemaOf(clone.module, *env);
    TypePtr owner = nullptr;
    StringId field = 0;

    for(auto entry: schema.slots.contents(global)) {
        if(entry.kind == GenSlotKind::Property && entry.index == slot) {
            owner = entry.type;
            field = entry.name;
        }
    }

    if(!owner) return;

    auto concrete = cloneType(clone, owner);
    auto content = concrete;

    // The same two steps projectField takes: a single-constructor record is its content, reached
    // through a downcast that costs nothing.
    if(concrete && global[concrete]->kind == Type::Record) {
        auto record = (RecordType*)global[concrete];
        if(record->layout != RecordType::Single || record->constructors.isEmpty()) return;

        into = clone.resolver.project(into, ProjectionKind::Downcast, 0);
        content = record->constructors.get(global, 0).content;
    }

    if(!content || global[content]->kind != Type::Tup) return;

    auto tuple = (TupType*)global[content];
    for(Size i = 0; i < tuple->fields.size(); i++) {
        if(tuple->fields.get(global, i).name != field) continue;

        into = clone.resolver.project(into, ProjectionKind::Field, U16(i));
        return;
    }

    (void)place;
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

    /*
     * Whether the step just taken added a Deref of its own, so that the original's copy of that same
     * Deref is not added twice.
     *
     * A generic body can hold a path into a type that was *already* concrete and already boxed - a
     * `lens fn f(c: Config) -> a` is generic in its continuation and not in `Config` - and such a
     * path arrives here with the Deref in it. The same path into a type that only becomes boxed once
     * the arguments are known arrives without one. Both have to come out with exactly one.
     */
    auto followed = false;

    for(auto projection: projections.contents(clone.local)) {
        // The Deref the previous step already supplied. Nothing else can be mistaken for it: a Deref
        // a *program* wrote steps off a `%T` field, whose declared type is a pointer and which is
        // therefore not boxed.
        if(followed && projection.kind == ProjectionKind::Deref) {
            followed = false;
            continue;
        }

        followed = false;

        // A constrained field becomes the ordinary access it always described, now that the owner
        // is a type with a layout. This is the whole of the compile-time property half: after this
        // point the specialization is indistinguishable from a body someone wrote concretely.
        if(projection.kind == ProjectionKind::Property) {
            resolveProperty(clone, result, projection.index, place);
            continue;
        }

        /*
         * Rebuilt through `project` rather than copied, so that an edge which is boxed in the
         * *concrete* type gets the Deref that reaches through it.
         *
         * This is the one place automatic indirection meets specialization, and it has to be here
         * rather than in the generic body. `Maybe(a)` has no layout and no box; `Maybe(Tree)` has
         * both, because `Tree` is what closes the cycle. So a body written once over `Maybe(a)` has
         * a bare Downcast in it and the specialization at `a = Tree` needs a Downcast and a load -
         * and the substituted type is the first thing that knows. Nothing else has to change,
         * because `project` is where that question is asked for source code too.
         */
        auto before = result.projections.size();
        result = clone.resolver.project(result, projection.kind, projection.index,
                                        cloneValue(clone, projection.value));

        followed = result.projections.size() > before + 1;
    }

    return result;
}

// Turns one InstGenCall into the call it always meant: the class implementation for the now-known
// types, or the callee's specialization. An intrinsic implementation expands here exactly as it
// would at an ordinary call site, so a specialized `x + x` is an `add` rather than a call.
static void cloneGenCall(Clone& clone, InstGenCall& call) {
    TypeList typeArgs;
    for(auto arg: call.typeArgs.contents(clone.local)) typeArgs.push(cloneType(clone, arg));

    ValueList args;
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
            auto& source = (InstMove&)inst;

            /*
             * A move of something that turned out to be register-sized is that value and nothing
             * else.
             *
             * A generic body reaches a result of unknown size through storage, because that is all
             * it can do without the size - so `let ->x = f(...)` moves out of the local backing the
             * call's result. Substituting a scalar takes the storage away: the specialization has
             * the value in a register, and the local it was reached through never gets an
             * allocation. Cloning the place as written would name storage nothing allocated, which
             * lowering rejects on the spot.
             *
             * The test is that the local was backed by a *value* rather than by an Alloc. A local
             * with an Alloc has storage in the clone too, whatever its type turned out to be, and
             * a move out of it is an ordinary move; one backed by a call result had storage only
             * because the generic body could not see a size.
             *
             * This is the mirror of the parameter materialization further down. That one hands
             * storage back where the body needs a place; this one drops it where the body only ever
             * needed a value. Both keep Implementation-Generics.md's first invariant: the body's
             * decisions are preserved and only their representation is adapted.
             */
            if(!isMemoryType(clone.global, type) && source.place.root == PlaceRoot::Local &&
               source.place.projections.isEmpty() && source.place.local < clone.from.localCount()) {
                auto backing = clone.from.localAt(clone.local, source.place.local).value;

                if(backing && clone.local[backing]->kind != Value::Alloc) {
                    clone.values.add(pointer, cloneValue(clone, backing));
                    return;
                }
            }

            // The relocation is not carried across, because the one the generic body recorded is
            // not the one this clone needs: `move %x : a` left it null, since a body that cannot
            // see the type has nothing concrete to name and relocates through the descriptor its
            // caller passed instead. A specialization has the type, so it answers the question
            // again for the substituted one - the same question sinkValue asks at an ordinary
            // `->`, asked the same way, which is what keeps an authored `Sink` from being skipped
            // for exactly the types that need it.
            auto moved = resolver.emit<InstMove>(inst.source, inst.name, type,
                                                 clonePlace(clone, source.place));

            auto ownership = ownershipIn(clone.module, functionGen(clone.global, resolver.function), type);
            if(!ownership.trivialSink) moved->sink = sinkFor(clone.module, type, inst.source);

            result = moved;
            break;
        }
        /*
         * The relocation is re-derived rather than carried across, for the reason the Move case
         * above gives at length: the generic body left it null because it had no concrete type to
         * name one for, and a specialization does have one. Carrying the null across is exactly how
         * an authored `Sink` would be skipped for the types that have one.
         *
         * Asked of the *exchanged* type rather than of `type`, which for a swap is unit.
         */
        case Value::Swap: {
            auto& swap = (InstSwap&)inst;
            auto a = clonePlace(clone, swap.a);
            auto b = clonePlace(clone, swap.b);
            auto swapped = cloneType(clone, swap.content);
            auto cloned = resolver.emit<InstSwap>(inst.source, inst.name, type, a, b, swapped);

            auto ownership = ownershipIn(clone.module, functionGen(clone.global, resolver.function), swapped);
            if(!ownership.trivialSink) cloned->sink = sinkFor(clone.module, swapped, inst.source);

            result = cloned;
            break;
        }
        case Value::Exchange: {
            auto& exchange = (InstExchange&)inst;
            auto place = clonePlace(clone, exchange.place);
            auto cloned = resolver.emit<InstExchange>(inst.source, inst.name, type, place,
                                                      cloneValue(clone, exchange.value));

            auto ownership = ownershipIn(clone.module, functionGen(clone.global, resolver.function), type);
            if(!ownership.trivialSink) cloned->sink = sinkFor(clone.module, type, inst.source);

            // The same reconciliation the Copy case performs: a type that turned out to be
            // register-sized has no second storage to make, so the result wants no slot.
            if(isMemoryType(clone.global, type)) {
                cloned->local = resolver.function.addLocal(clone.module, type, inst.name,
                                                           resolver.ref(cloned));
            }

            result = cloned;
            break;
        }
        case Value::Copy: {
            auto place = clonePlace(clone, ((InstCopy&)inst).place);

            // A duplicate of something that turned out to be register-sized is a load, and nothing
            // else: there is no second storage to make, so what the generic body wrote as "an
            // independent copy with its own root" is already true of the loaded value. This is the
            // same reconciliation the materialization above performs on the way in.
            if(!isMemoryType(clone.global, type)) {
                result = resolver.emit<InstLoadPlace>(inst.source, inst.name, type, place);
                break;
            }

            auto cloned = resolver.emit<InstCopy>(inst.source, inst.name, type, place);

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
        case Value::TypeMetric: {
            // The measured type is substituted like any other, which is what turns `sizeOf(x)` in a
            // generic body from a load out of the descriptor into a constant in the specialization.
            auto& metric = (InstTypeMetric&)inst;
            result = resolver.emit<InstTypeMetric>(inst.source, inst.name, type,
                                                   cloneType(clone, metric.of), metric.metric);
            break;
        }
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
            ValueList args;
            for(auto arg: call.args.contents(clone.local)) args.push(cloneValue(clone, arg));

            auto value = resolver.emitDirectCall(call.callee, toBuffer(args), inst.source, nullptr, inst.name);
            if(value) clone.values.add(pointer, value);
            return;
        }
        case Value::Symbol: {
            auto& symbol = (InstSymbol&)inst;
            result = resolver.emit<InstSymbol>(inst.source, inst.name, type, symbol.callee, symbol.global);
            break;
        }
        case Value::CallDyn: {
            /*
             * An indirect call clones unchanged.
             *
             * A function value's callee is decided at run time, so there is nothing here for a
             * substitution to make concrete - unlike an InstGenCall, which exists precisely to be
             * decided by one. What the signature substitutes to still matters for the conventions,
             * so it goes through cloneType like every other type in the body.
             */
            auto& call = (InstCallDyn&)inst;
            auto dynamic = resolver.create<InstCallDyn>(
                inst.source, inst.name, type,
                call.callable ? cloneValue(clone, call.callable) : nullptr,
                call.address ? cloneValue(clone, call.address) : nullptr,
                cloneType(clone, call.signature));

            for(auto arg: call.args.contents(clone.local)) {
                dynamic->args.push(clone.module.arena, cloneValue(clone, arg));
            }

            resolver.append(dynamic);

            if(call.local != maxLimit<U32>) {
                dynamic->local = resolver.function.addLocal(clone.module, type, inst.name,
                                                            resolver.ref(dynamic));
            }

            result = dynamic;
            break;
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
        if(arg->lazyType) created->lazyType = cloneType(clone, arg->lazyType);
        clone.values.add((ModulePtr<Value>)argPointer, (ModulePtr<Value>)(created - local));
    }

    // A Place names a local by index, so the table is copied position for position before any
    // instruction that addresses it. The value each one holds is filled in afterwards, once the
    // instruction that produced it has been cloned.
    for(Size i = 0; i < from.localCount(); i++) {
        auto slot = from.localAt(local, U32(i));
        to.locals.push(clone.module.arena, Local {
            cloneType(clone, slot.type), slot.name, nullptr, slot.convention, slot.storage, slot.borrowed,
            slot.closureEnv,
        });
    }

    /*
     * Parameters that stopped being addresses.
     *
     * A generic body reaches a value of unknown size through its storage, because that is the only
     * thing it can do without knowing the size - and so a parameter of type `a` gets a local and
     * every use of it is a place. Substituting a scalar for `a` takes that away: the argument now
     * arrives in a register, and the places the body already built have nothing to point at.
     *
     * So the specialization gives it storage back. This is the same materialization the erased ABI
     * performs at a concrete-to-erased boundary, done at clone time instead, and it is what keeps
     * Implementation-Generics.md's first invariant intact: the body's decisions are preserved
     * exactly, and only their representation is adapted.
     */
    IndexSet materialized;
    IndexSet addressed;
    materialized.reset(from.localCount());
    addressed.reset(from.localCount());

    auto note = [&](const Place& place) {
        if(place.root == PlaceRoot::Local && place.local < addressed.size()) addressed.set(place.local, true);
    };

    for(auto blockPointer: from.blocks.contents(local)) {
        auto block = local[blockPointer];

        for(auto instruction: block->instructions.contents(local)) {
            eachPlace(*local[instruction], note);
        }
    }

    clone.resolver.current = to.blocks.get(local, 0);

    for(Size i = 0; i < from.localCount(); i++) {
        auto slot = from.localAt(local, U32(i));
        if(!addressed[i]) continue;
        if(slot.borrowed || !slot.value || local[slot.value]->kind != Value::Arg) continue;

        auto target = to.localAt(local, U32(i));
        if(!isMemoryType(clone.global, slot.type) || isMemoryType(clone.global, target.type)) continue;

        auto argument = cloneValue(clone, slot.value);
        if(!argument) continue;

        auto allocation = clone.resolver.emit<InstAlloc>(clone.source, target.name, target.type, U32(i));
        target.value = clone.resolver.ref(allocation);
        to.locals.set(local, i, target);
        materialized.set(i, true);

        clone.resolver.initialize(Place::inLocal(U32(i)), argument, clone.source);
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
        if(materialized[i]) continue;

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

    return builtName(module.context, text);
}

/*
 * The type of one named field of a concrete type, or null where it has none.
 *
 * The type half of ExprResolver::projectField, and it follows the same two steps for the same
 * reasons: a raw pointer is read through, and a single-constructor record's fields are the fields
 * of its one constructor's content. Anything the use site would reject - a multi-constructor record,
 * a scalar - has no field of any name, which is the answer a constraint needs rather than a
 * diagnostic of its own.
 */
static TypePtr fieldTypeOf(Module& module, TypePtr type, StringId field) {
    auto global = *module.types;
    if(!type) return nullptr;

    if(global[type]->kind == Type::Ptr) type = ((PtrType*)global[type])->to;
    if(!type) return nullptr;

    if(global[type]->kind == Type::Record) {
        auto record = (RecordType*)global[type];
        if(record->layout != RecordType::Single || record->constructors.isEmpty()) return nullptr;

        type = record->constructors.get(global, 0).content;
    }

    if(!type || global[type]->kind != Type::Tup) return nullptr;

    for(auto member: ((TupType*)global[type])->fields.contents(global)) {
        if(member.name == field) return member.type;
    }

    return nullptr;
}

/*
 * One constraint written back out the way its author wrote it - `a.x: Int`, `f: (a) -> Int`.
 *
 * Implementation-Generics.md part 12: a diagnostic names the source constraint, never the
 * environment slot it was numbered into. Spelled from the constraint's own types rather than from
 * the substituted ones, so what a message quotes is the text in the signature and the concrete
 * types appear beside it as what failed to satisfy it.
 */
static void describeProperty(Context& context, GlobalBase global, const PropertyConstraint& constraint,
                             StringBuilder& target) {
    describeType(context, global, constraint.owner, target);
    target << '.' << context.findName(constraint.field) << ": ";
    describeType(context, global, constraint.result, target);
}

static void describeFunctionConstraint(Context& context, GlobalBase global, const FunctionConstraint& constraint,
                                       StringBuilder& target) {
    target << context.findName(constraint.name) << ": ";
    describeType(context, global, constraint.signature, target);
}

// The type a named function would have as a value: its declared arguments, with their conventions,
// and its result. What a `f: (a) -> b` constraint is satisfied by, and the reason FunctionConstraint
// holds a FunType rather than a name and an arity - the conventions are part of the promise.
static TypePtr signatureOf(Module& module, Function& function) {
    auto local = *module.arena;

    Array<FunArg> args;
    for(auto argPointer: function.args.contents(local)) {
        auto declared = local[argPointer];
        args.push(FunArg { declared->declaredType(), declared->name, declared->convention,
                           declared->returnRoot, declared->isLazy() });
    }

    return resolveFunType(module, toBuffer(args), function.returnType, ast::FunKind::Plain);
}

/*
 * Proves every requirement of the context for these arguments.
 *
 * Reported against the requirement rather than against the call inside the body that needs it:
 * `Ord(a)` is what the signature promises, and `Ord(Shape)` is what the caller failed to supply.
 * Implementation-Generics.md part 12 says the same about the other two kinds - a diagnostic names
 * the constraint the author wrote, never the environment slot it was numbered into - which is why
 * each message below is about `a.x: Int` rather than about slot 1.
 *
 * All three kinds are proved here even though only the class ones have a witness to build. A
 * constraint that is checked nowhere is not a constraint: the declaration would accept every
 * argument type and the rejection would fall to whatever the *body* happened to do with it, which
 * reports at the wrong place, only for the members the body reaches, and not at all for a body that
 * does not reach any of them.
 */
static bool proveRequirements(Module& from, Function& generic, GenEnv& env, Buffer<TypePtr> args, LocationId source) {
    auto& context = from.context;
    auto global = *from.types;
    auto ok = true;

    for(auto constraint: env.classes.contents(global)) {
        if(!constraint.typeClass) continue;

        TypeList concrete;
        for(auto arg: constraint.args.contents(global)) {
            concrete.push(substituteType(from, arg, args, source));
        }

        if(findInstance(from, constraint.typeClass, toBuffer(concrete))) continue;

        StringBuilder text;
        describeTypes(context, global, toBuffer(concrete), text);

        context.diagnostics.error("no instance of %@ for (%@), required by %@"_v, source,
                                  context.findName(global[constraint.typeClass]->name),
                                  text.view(), context.findName(generic.name));
        ok = false;
    }

    for(auto constraint: env.properties.contents(global)) {
        StringBuilder declared;
        describeProperty(context, global, constraint, declared);

        auto owner = substituteType(from, constraint.owner, args, source);
        auto expected = substituteType(from, constraint.result, args, source);
        auto actual = fieldTypeOf(from, owner, constraint.field);

        if(!actual) {
            context.diagnostics.error("%@ has no field %@, required by %@ of %@"_v, source,
                                      describeType(context, global, owner),
                                      context.findName(constraint.field), declared.view(),
                                      context.findName(generic.name));
            ok = false;
            continue;
        }

        // Exactly, not convertibly. A property witness reads and writes the owner's storage, so a
        // field that merely converts to the declared type would give the body a place of a type it
        // cannot write back through - which is the same reason a `&` argument takes no conversion.
        if(sameType(actual, expected)) continue;

        context.diagnostics.error("field %@ of %@ is %@, not the %@ required by %@ of %@"_v, source,
                                  context.findName(constraint.field),
                                  describeType(context, global, owner),
                                  describeType(context, global, actual),
                                  describeType(context, global, expected),
                                  declared.view(), context.findName(generic.name));
        ok = false;
    }

    for(auto constraint: env.functions.contents(global)) {
        StringBuilder declared;
        describeFunctionConstraint(context, global, constraint, declared);

        auto expected = substituteType(from, constraint.signature, args, source);
        auto target = findFunction(from, constraint.name, source);

        if(!target) {
            context.diagnostics.error("there is no function %@, required by %@ of %@"_v, source,
                                      context.findName(constraint.name), declared.view(),
                                      context.findName(generic.name));
            ok = false;
            continue;
        }

        /*
         * A generic candidate is not proof. Satisfying `f: (Point) -> Int` with a `fn f(x: a) -> Int`
         * means instantiating it, and what instantiates it is the witness this constraint is owed -
         * which does not exist yet. Rejecting keeps that a missing feature rather than a silently
         * accepted declaration, and it is the answer genEnvFor already gives the erased path.
         */
        auto found = (*from.arena)[target];
        if(found->gen) {
            context.diagnostics.error("%@ is generic, and cannot satisfy %@ of %@ yet - that needs a function witness, which is not built yet"_v,
                                      source, context.findName(constraint.name), declared.view(),
                                      context.findName(generic.name));
            ok = false;
            continue;
        }

        auto actual = signatureOf(from, *found);
        if(sameType(actual, expected)) continue;

        context.diagnostics.error("%@ is %@, not the %@ required by %@ of %@"_v, source,
                                  context.findName(constraint.name),
                                  describeType(context, global, actual),
                                  describeType(context, global, expected),
                                  declared.view(), context.findName(generic.name));
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

    // `@inline` and `@noinline` are properties of the declaration, so every specialization of it has
    // them. A specialization has no declaration of its own to read them off, and it is exactly the
    // form the optimizer sees - so forgetting them here would mean the attributes worked on a
    // concrete function and silently did nothing on a generic one.
    specialized->inlineHint = generic->inlineHint;
    specialized->noInline = generic->noInline;

    /*
     * A specialization of a class implementation is still that implementation.
     *
     * It matters for one rule beyond printing: a `Drop`, `Reclaim` or `Sink` implementation is the
     * one place a `->` parameter's disposal is the body's own business, and a specialization that
     * had forgotten which class it implements would be given a drop of its own argument - which is
     * a call to itself, forever. `instance Reclaim(Array(a))` specialized at `Int` is exactly that
     * shape, so this is not a hypothetical.
     */
    specialized->instanceOf = generic->instanceOf;
    for(auto type: generic->instanceArgs.contents(local)) {
        specialized->instanceArgs.push(owner.arena, substituteType(owner, type, args, source));
    }
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

ModulePtr<Function> instanceImplementation(Module& module, GlobalPtr<TypeClass> typeClass, TypePtr type,
                                           LocationId source) {
    TypePtr args[] = { type };
    auto match = matchInstance(module, typeClass, toBuffer(args));
    if(!match) return nullptr;

    auto local = *module.arena;
    auto instance = local[match.instance];
    if(instance->functions.isEmpty()) return nullptr;

    auto implementation = instance->functions.get(local, 0);
    if(!implementation) return nullptr;

    if(local[implementation]->gen) {
        implementation = instantiateFunction(module, implementation, toBuffer(match.args), source);
        if(!implementation) return nullptr;
    }

    (*module.arena)[implementation]->used = true;
    return implementation;
}
