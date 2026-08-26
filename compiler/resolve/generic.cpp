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
 * Substitution that leaves an undecided variable standing for itself.
 *
 * Handing substituteType a binding list with holes in it builds types *around* those holes - a null
 * element inside a record's arguments - so a constraint cannot be examined until every variable in
 * it is known. Which is exactly backwards for a dependency, whose whole job is to answer a
 * constraint that is only partly known. Substituting an unbound variable by itself gives the
 * constraint as written, and `isGeneric` on the result is then the question "was this decided".
 */
static void bindingsOrVariables(GlobalBase global, const GenEnv& env, Buffer<TypePtr> bindings, TypeList& target) {
    auto types = env.types;
    Size index = 0;

    for(auto variable: types.contents(global)) {
        auto binding = index < bindings.length ? bindings[index] : nullptr;
        target.push(binding ? binding : TypePtr(variable));
        index++;
    }
}

// Whether these are the types a requirement decides by. Only the deciding positions are compared,
// which is the whole point: the determined ones are what the caller is asking about.
static bool decidesWith(Buffer<TypePtr> declared, U16 determined, Buffer<TypePtr> args) {
    if(declared.length != args.length) return false;

    for(U16 i = 0; i < determined; i++) {
        if(args[i] && declared[i] != args[i]) return false;
    }

    return true;
}

bool findClassRequirement(Module& module, const GenEnv& env, GlobalPtr<TypeClass> typeClass,
                          Buffer<TypePtr> args, TypeList& out) {
    auto global = *module.types;
    if(!typeClass || !global[typeClass]->determines()) return false;

    auto determined = global[typeClass]->determined;
    auto classes = env.classes;

    for(auto constraint: classes.contents(global)) {
        if(constraint.typeClass != typeClass) continue;

        TypeList declared;
        for(auto arg: constraint.args.contents(global)) declared.push(arg);

        if(!decidesWith(toBuffer(declared), determined, args)) continue;

        replaceContents(out, declared);
        return true;
    }

    // A requirement the declared ones only imply. `Contiguous(c, a)` promises `Chunked(c, a)`
    // through its superclass, and a body that calls `chunks` has to reach it without the author
    // having written the superclass out - the same rule provesClass states for the ordinary case.
    for(auto constraint: classes.contents(global)) {
        if(!constraint.typeClass || constraint.typeClass == typeClass) continue;

        auto declaringEnv = global[global[constraint.typeClass]->gen];
        if(!declaringEnv || declaringEnv->types.size() != constraint.args.size()) continue;

        TypeList declaredWith;
        for(auto arg: constraint.args.contents(global)) declaredWith.push(arg);

        for(auto superclass: declaringEnv->classes.contents(global)) {
            if(superclass.typeClass != typeClass) continue;

            // The superclass is written in its own class's variables, so it is expressed in the
            // types the requirement was declared with before being compared.
            TypeList expressed;
            for(auto arg: superclass.args.contents(global)) {
                expressed.push(substituteType(module, arg, toBuffer(declaredWith), constraint.source));
            }

            if(!decidesWith(toBuffer(expressed), determined, args)) continue;

            replaceContents(out, expressed);
            return true;
        }
    }

    return false;
}

void fillDetermined(Module& module, GenEnv& env, TypeList& bindings, LocationId source,
                    const GenEnv* caller) {
    auto global = *module.types;

    // A chain resolves in one call rather than in declaration order, so each round that changes
    // anything is bounded by one more variable being decided.
    for(Size round = 0; round <= env.classes.size(); round++) {
        auto moved = false;

        for(auto constraint: env.classes.contents(global)) {
            auto typeClass = constraint.typeClass;
            if(!typeClass || !global[typeClass]->determines()) continue;

            auto determined = global[typeClass]->determined;
            if(constraint.args.size() <= determined) continue;

            TypeList safe;
            bindingsOrVariables(global, env, toBuffer(bindings), safe);

            TypeList concrete;
            auto ready = true;
            auto open = false;
            Size index = 0;

            for(auto arg: constraint.args.contents(global)) {
                auto substituted = substituteType(module, arg, toBuffer(safe), source);
                auto decided = substituted && !isGeneric(global, substituted);

                if(index < determined) {
                    /*
                     * Nothing to look an instance up by until every deciding position names a head.
                     * A later round may still decide it.
                     *
                     * A *bare* variable is what cannot select, and only a bare one - which is the
                     * rule `resolveDetermined` states at length and this test used to be stricter
                     * than: `Flat(a)` is generic and is still a container the author wrote out, one
                     * instance answers for it, and what that instance determines is the same answer
                     * whatever `a` turns out to be. Asking for concreteness here meant a body
                     * generic in the *element* could not call an operation over its own chunk -
                     * `fn (Num(a)) f(chunk: [a])` calling `vectors(chunk)` - while the identical
                     * body over `[Int]` could.
                     */
                    if(!substituted || global[substituted]->kind == Type::Gen) ready = false;
                    concrete.push(substituted);
                } else {
                    if(!decided) open = true;
                    concrete.push(decided ? substituted : nullptr);
                }

                index++;
            }

            if(!open) continue;

            /*
             * What answers the dependency, and there are two things that can.
             *
             * An *instance* answers when every deciding position is a real type, which is the
             * ordinary case and the one this pass was written for. Inside a generic body a deciding
             * position is frequently the calling function's own type variable - `fn (Chunked(c, a))
             * sum(xs: c)` calling `vectors(xs)` hands over its own `c` - and there is no instance to
             * look up, because which container `c` will be is that function's caller's business.
             *
             * What answers there is the **caller's own declared requirement**, which named the
             * determined position already: the `a` of the `Chunked(c, a)` this function declared is
             * what `chunks(xs)` yields, whatever `c` turns out to be. Without this, every generic
             * function over a dependency that calls another one reports that the call does not
             * decide what it hands over - for a call that decides it perfectly well - and that is
             * every bulk operation in Implementation-Vector.md §9 item 7.
             *
             * `fillDependency` states the same rule for the single-instance case and cites the same
             * reason; this is the chain, which it did not reach.
             */
            auto answered = false;

            if(ready) {
                answered = bool(resolveDetermined(module, typeClass, concrete));
            } else if(caller) {
                TypeList declared;

                if(findClassRequirement(module, *caller, typeClass, toBuffer(concrete), declared)) {
                    for(Size i = determined; i < concrete.size() && i < declared.size(); i++) {
                        if(!concrete[i]) concrete[i] = declared[i];
                    }

                    answered = true;
                }
            }

            if(!answered) continue;

            /*
             * What the instance answered, matched back against the constraint *as written*, which
             * is what binds this function's own variables rather than the class's.
             *
             * Pattern-side, so a constraint naming a structure - `Contiguous(c, Pair(k, v))` -
             * binds both of its variables from one answer, and a position the bindings already
             * decided constrains rather than rebinds.
             */
            index = 0;
            for(auto arg: constraint.args.contents(global)) {
                if(index >= determined && concrete[index]) {
                    if(matchType(global, arg, concrete[index], { bindings.pointer(), bindings.size() })) {
                        moved = true;
                    }
                }

                index++;
            }
        }

        if(!moved) break;
    }
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
                           SuperclassSteps& steps, U32 depth) {
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
    SuperclassSteps steps;

    for(auto constraint: classes.contents(global)) {
        TypeList have;
        for(auto arg: constraint.args.contents(global)) have.push(arg);

        steps.clear();
        if(superclassPath(module, constraint.typeClass, toBuffer(have), typeClass, args, steps, 8)) return true;
    }

    return false;
}

U16 genWitnessPath(Module& module, GenEnv& env, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                   SuperclassSteps& supers) {
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

    // Which slots of the copied table already hold their value - see `pairClonedLocal` and the
    // use-recording loop at the end of cloneBody, which is the other reader.
    IndexSet filled;
};

/*
 * The instruction's own copy of "which slot my result fills".
 *
 * `Local::value` and `Value::slot` are the pairing every pass reads, and six instruction kinds hold
 * the same number *again* in a field of their own - which is what lowering reads to find the storage
 * a result is written into. Re-pointing the pairing without this leaves the two disagreeing, and the
 * disagreement is silent until something acts on the instruction's copy.
 *
 * The two fixups below are where that happened, and both are the specializer's: `cloneBody` copies
 * the local table position for position while the emitters *append*, so a cloned result briefly owns
 * a slot past the end and the fixup points the original index at it - moving `Value::slot` and
 * leaving `InstExchange::local` on the appended one. `dischargeExchange` allocates over that field,
 * so the exchange built its storage in an empty slot while every reader of the result went on naming
 * the slot the fixup had moved it to, and the exchange was then erased with those readers still
 * pointing at it. A generic `exchange` inside an `iter fn` body - which is what folding a container
 * with a consuming `++` was - crashed the compiler in lowering, two passes after the verifier first
 * reported the use lists as wrong.
 *
 * Written here rather than inside `IrEditor::setLocalValue`, which is where it looks like it
 * belongs: a slot's value and an instruction's own field are *not* always the same number, and the
 * inliner's graft depends on that - it points several of the caller's slots at one cloned call while
 * the call keeps writing into the one it was given. What is specific to these two fixups is that
 * they are correcting a pairing the emitter got wrong, so the instruction's copy is wrong with it.
 *
 * A switch over the six rather than a virtual, on `inst.def`'s terms: the list is closed, and a kind
 * that grows a slot of its own later fails to compile here rather than keeping a stale one.
 */
static void setResultLocal(Value& value, U32 slot) {
    switch(value.kind) {
        case Value::Alloc:    ((InstAlloc&)value).local = slot; break;
        case Value::Exchange: ((InstExchange&)value).local = slot; break;
        case Value::Copy:     ((InstCopy&)value).local = slot; break;
        case Value::Call:     ((InstCall&)value).local = slot; break;
        case Value::CallDyn:  ((InstCallDyn&)value).local = slot; break;
        case Value::GenCall:  ((InstGenCall&)value).local = slot; break;
        default: break;
    }
}

/*
 * The slot a cloned result fills.
 *
 * The local table is copied position for position before any instruction is cloned, so a value that
 * had a slot in the generic body has one *waiting at the same index* here. What the emitters do
 * instead is append: `emitDirectCall`, and the Copy, Exchange and CallDyn arms below, each call
 * `Function::addLocal`, which hands out `locals.size()`. So the result briefly answers with a slot
 * past the end of the copied table, and the loop at the end of cloneBody then points the original
 * index at the same value - leaving two slots naming one instruction.
 *
 * **The window between those two is where it bit.** Anything cloned after the result and before the
 * fixup that asks `findPlace` for it - which is every `->` argument, since `sinkValue` builds an
 * `InstMove` out of the place - baked the appended index into a Place. The fixup then moved
 * `Value::slot` back to the original index and the Move was left naming a slot no instruction
 * defines: `deriveEffects` recorded no init for it, the move recorded a Moved, and a loop carried
 * that state to the next iteration as "this value has been moved out of and cannot be used again".
 * It reproduced only where the *generic* body gave the result a slot at all, which is why a
 * concrete body written out by hand was fine and its specialization was not.
 *
 * So the pairing is made here, at the point the value exists, and the appended slot is released.
 * Nothing points at it - it was created moments ago and the value has just been re-pointed - so it
 * is left empty rather than removed, which is the state every not-yet-filled slot is already in.
 *
 * **Every emitter that produces a result has to call this, and the three in `cloneGenCall` are the
 * ones easiest to miss.** A deferred generic call leaves that function three ways - a class
 * dispatch through `emitInstanceCall`, a generic intrinsic through `expandIntrinsic`, and an
 * ordinary instantiation through `emitDirectCall` - and only the last was paired. The first is what
 * `copy(x)` is in a body with `Copy(a)` in scope, so `acc = acc ++ copy(x)` specialized to a `drop`
 * of the original slot sitting between the call and a `move` of the appended one: the copy's run was
 * released and handed straight back to the accumulator's own growth, and the join read the bytes it
 * had just written there. In a loop the same split reported "this value has been moved out of and
 * cannot be used again" instead, because the state the move recorded belonged to a slot no
 * instruction defines.
 */
static void pairClonedLocal(Clone& clone, const Inst& original, ModulePtr<Value> cloned) {
    auto slot = original.slot;
    if(!cloned || slot == maxLimit<U32> || slot >= clone.from.localCount()) return;

    auto& to = clone.resolver.function;
    auto appended = clone.local[cloned]->slot;
    if(appended == slot) return;

    IrEditor editor(clone.module, to);
    if(appended < to.localCount()) editor.setLocalValue(appended, nullptr);

    editor.setLocalValue(slot, cloned);
    clone.filled.set(slot, true);
    setResultLocal(*clone.local[cloned], slot);
}

static TypePtr cloneType(Clone& clone, TypePtr type) {
    return substituteType(clone.module, type, clone.args, clone.source);
}

static ModulePtr<Block> cloneBlock(Clone& clone, ModulePtr<Block> block) {
    if(!block) return nullptr;

    auto found = clone.blocks.getValue(block);
    return found ? ModulePtr<Block>(found.unwrap()) : nullptr;
}

/*
 * Storage the substitution turned back into a value.
 *
 * A generic body reaches an `a` through an allocation, because that is all it can do without the
 * size: `let ->g = h` allocates for the relocation, and every use of `g` is that allocation. Give
 * `a` a scalar and the allocation is still needed - the places the body built point into it - but a
 * *use* of it now has to be the contents rather than the address, because a scalar operand is a
 * register and lowering has nowhere to put an address instead.
 *
 * This is the third of the reconciliations the clone performs, and the mirror of the other two. The
 * parameter materialization further down hands storage back where the body needs a place; the Move
 * and Copy cases drop it where the body only ever needed a value; and this one reads out of it where
 * the body needed both. All three keep Implementation-Generics.md's first invariant: the decisions
 * are preserved, and only their representation is adapted.
 *
 * Asked of the *original* type as well as the substituted one, which is what keeps it to the case it
 * is for. A raw pointer's allocation was never a memory type in the generic body either, so it is
 * not something the substitution changed and nothing here should touch it.
 */
static ModulePtr<Value> readIfMaterialized(Clone& clone, ModulePtr<Value> original, ModulePtr<Value> cloned) {
    if(!cloned || !original) return cloned;

    auto& produced = *clone.local[cloned];
    if(produced.kind != Value::Alloc) return cloned;
    if(isMemoryType(clone.global, produced.type)) return cloned;
    if(!isMemoryType(clone.global, clone.local[original]->type)) return cloned;

    auto place = Place::inLocal(((InstAlloc&)produced).local);
    return clone.resolver.ref(clone.resolver.emit<InstLoadPlace>(produced.source, produced.name,
                                                                 produced.type, place));
}

/*
 * The clone of a value, as the thing itself rather than as an operand.
 *
 * Two callers want this rather than cloneValue, and both are asking which *instruction* the original
 * became rather than what to read at a use: the local table, whose `value` field has to stay the
 * allocation that backs the slot, and a place rooted in a pointer or a borrow. Reading through
 * readIfMaterialized at either would replace the definition with a load of itself.
 */
static ModulePtr<Value> cloneDefinition(Clone& clone, ModulePtr<Value> value);

static ModulePtr<Value> cloneValue(Clone& clone, ModulePtr<Value> value) {
    return readIfMaterialized(clone, value, cloneDefinition(clone, value));
}

static ModulePtr<Value> cloneDefinition(Clone& clone, ModulePtr<Value> value) {
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
        case Value::ConstString:
            result = clone.resolver.constant<ConstString>(source->source, type, ((ConstString*)source)->text);
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
    StringId field = StringId();

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
        result.pointer = cloneDefinition(clone, place.pointer);
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

    ArgList args;
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

        if(result) {
            clone.values.add(pointer, result);
            pairClonedLocal(clone, call, result);
        }

        return;
    } else if(clone.local[call.callee]->intrinsic) {
        // A generic intrinsic is generated rather than instantiated, here for the same reason it
        // is at an ordinary call site: there is no body for these types until there are types.
        auto pointer = (ModulePtr<Value>)((Inst*)&call - clone.local);
        auto result = clone.resolver.expandIntrinsic(call.callee, toBuffer(typeArgs), toBuffer(args),
                                                     call.source, call.name);

        if(result) {
            clone.values.add(pointer, result);
            pairClonedLocal(clone, call, result);
        } else {
            clone.ok = false;
        }

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

    if(result) {
        clone.values.add(pointer, result);
        pairClonedLocal(clone, call, result);
    }
}

static void cloneBody(Clone& clone, Function& to);
static StringId specializationName(Module& module, Function& generic, Buffer<TypePtr> args);

/*
 * The other half of a lifted continuation in a generic body - see Function::liftedFrom.
 *
 * A body lifted out of the function being cloned names that function's type variables, so it is
 * cloned under the same bindings and the symbol referring to it is rewritten to the clone. Anything
 * else - a named function, a lifted body belonging to somebody else - is shared as it always was.
 *
 * The cache is the lifted function's own specialization list, keyed by the same argument list a
 * specialization is keyed by. That matters for more than repeat work: a continuation is referred to
 * once by the symbol that makes it a function value and again by the closure link on its
 * environment allocation, and the two have to name one function.
 */
static ModulePtr<Function> cloneLiftedCallee(Clone& clone, ModulePtr<Function> callee) {
    auto local = clone.local;
    if(!callee) return callee;

    auto lifted = local[callee];
    if(lifted->liftedFrom != (ModulePtr<Function>)(&clone.from - local)) return callee;

    for(auto existing: lifted->specializations.contents(local)) {
        if(sameTypes(local[existing]->genericArgs, local, clone.args)) return existing;
    }

    auto& module = clone.module;
    auto specialized = addAnonymousFunction(module, specializationName(module, *lifted, clone.args),
                                            lifted->source);

    specialized->specializationOf = callee;
    specialized->used = true;
    specialized->takesEnv = lifted->takesEnv;
    specialized->funKind = lifted->funKind;
    specialized->yieldForm = lifted->yieldForm;
    specialized->skipping = lifted->skipping;
    specialized->inlineHint = lifted->inlineHint;
    specialized->noInline = lifted->noInline;
    specialized->returnType = substituteType(module, lifted->returnType, clone.args, clone.source);

    for(auto arg: clone.args) specialized->genericArgs.push(module.arena, arg);

    // Registered before the body is cloned, so a continuation that refers to itself - which a loop
    // body does not, but a nested one may reach - finds this function rather than starting again.
    lifted->specializations.push(module.arena, specialized - local);

    Clone inner(module, clone.site, *lifted, *specialized, clone.args, clone.source);
    cloneBody(inner, *specialized);
    if(!inner.ok) clone.ok = false;

    /*
     * Its own header, because a closure header is emitted at the entry point it belongs to.
     *
     * Two specializations are two entry points, so sharing the original's would put one function's
     * teardown in front of another's code - and the teardown is not the same one anyway: what it
     * releases is the captured environment, whose type has just been substituted.
     */
    if(lifted->closureHeader && specialized->args.size()) {
        auto envArg = local[specialized->args.get(local, 0)];
        if(auto envType = pointeeType(clone.global, envArg->type)) {
            closureHeaderFor(module, specialized - local, envType, lifted->source);
        }
    }

    return specialized - local;
}

static void cloneInstruction(Clone& clone, Inst& inst) {
    auto pointer = (ModulePtr<Value>)(&inst - clone.local);
    auto type = cloneType(clone, inst.type);
    auto& resolver = clone.resolver;
    Value* result = nullptr;

    switch(inst.kind) {
        case Value::Alloc: {
            auto& source = (InstAlloc&)inst;

            /*
             * The count and the storage tag come across; the storage *class* does not.
             *
             * Where a run lives is decided per body by the escape analysis, and a specialization is a
             * body of its own - so it gets its own answer. What it needs from the original is that
             * there was a tag to patch at all, because a clone that lost it would leave its own
             * constant reading `Inline` while the allocation went to the heap, and the `Reclaim`
             * switch would then hand nothing back. The constant itself is cloned fresh, so the two
             * bodies patch two different values.
             *
             * **The count is cloned before the allocation is emitted, and handed to it**, because it
             * is an *operand* - see InstAlloc::extent - and `emit` is what records an instruction's
             * uses. Assigning it afterwards left the count with a use list of zero: nothing read it,
             * so `eliminateDeadValues` removed the instruction computing it, and the allocation was
             * left naming a value no block defined. Lowering then read a null and `scaleBy`
             * dereferenced it, which is a segfault in the compiler rather than a diagnostic - the
             * exact failure `createInst`'s note in builder.h exists to prevent. It needed a run whose
             * length is *computed* inside a body that gets specialized, which is why no fixture had
             * one until an `iter fn` allocated a buffer of its argument.
             *
             * `storageFlag` stays below it and stays safe: it is a constant, which belongs to no
             * block, is never a use, and cannot be removed.
             */
            auto extent = source.extent ? cloneValue(clone, source.extent) : nullptr;
            auto allocation = resolver.emit<InstAlloc>(inst.source, inst.name, type, source.local,
                                                       extent);

            if(source.storageFlag) allocation->storageFlag = cloneValue(clone, source.storageFlag);

            // A closure environment names the body it belongs to, so that the teardown reaches that
            // body's header. A lifted body that was cloned above has its own header, and this is the
            // link that has to follow it.
            if(source.closure) allocation->closure = cloneLiftedCallee(clone, source.closure);

            result = allocation;
            break;
        }
        /*
         * The three instructions a substitution can empty out.
         *
         * A generic body reads, relocates and writes an `a` because an `a` is something; substituting
         * unit takes the something away, and what is left has no bytes to touch and no value to be.
         * The resolver never builds these for a concrete unit - `load` answers nothing and `write`
         * emits nothing - so the clone has to reach the same shape rather than a load of no bytes and
         * a store of no value, both of which lowering asserts on.
         *
         * Mapping the value to null rather than dropping the entry is what makes a *use* of it null
         * too, which is how the emptiness reaches the `init` below and the `ret` at the end.
         */
        case Value::LoadPlace:
            if(isUnit(clone.global, type)) {
                clone.values.add(pointer, ModulePtr<Value>(nullptr));
                return;
            }

            result = resolver.emit<InstLoadPlace>(inst.source, inst.name, type,
                                                  clonePlace(clone, ((InstLoadPlace&)inst).place));

            // And the overread flag with it. A clone that dropped it produced a load nothing had
            // exempted from the bounds reasoning - which is not a wrong answer here but is one
            // everywhere the flag is read, and every one of those readers is downstream of this
            // walk. `loadVectorTail` is a generic function, so *every* overreading load in a real
            // program arrives through a specialization of one.
            ((InstLoadPlace*)result)->overread = ((InstLoadPlace&)inst).overread;
            break;
        case Value::Init:
        case Value::Assign: {
            auto& init = (InstInit&)inst;
            auto value = cloneValue(clone, init.value);

            // Written as "the operand vanished" rather than as a question about the place, because
            // that is the only way it can happen and it says which end the emptiness came from.
            if(init.value && !value) return;

            result = resolver.emit<InstInit>(inst.source, inst.name, type, clonePlace(clone, init.place),
                                             value, inst.kind);
            break;
        }
        /*
         * The element list is cloned whole, and an element that vanished takes its index with it.
         *
         * A substitution at `{}` is where that happens: the elements carry nothing, so `cloneValue`
         * answers null for each and what is left is an aggregate of no elements - which expands to
         * no stores, exactly as the per-element `Init`s above would each have been skipped.
         */
        case Value::Aggregate: {
            auto& source = (InstAggregate&)inst;
            auto aggregate = resolver.create<InstAggregate>(inst.source, inst.name, type,
                                                            clonePlace(clone, source.place));
            aggregate->constructor = source.constructor;

            eachAggregateComponent(clone.local, source, [&](AggregateComponent component, Size) {
                auto value = cloneValue(clone, component.value);
                if(!value) return;

                if(component.step.value) component.step.value = cloneValue(clone, component.step.value);

                aggregate->components.push(clone.module.arena,
                                           AggregateComponent { component.step, value });
            });

            resolver.append(aggregate);
            result = aggregate;
            break;
        }
        case Value::Move: {
            auto& source = (InstMove&)inst;

            /*
             * A move of something that turned out to occupy nothing is nothing.
             *
             * The generic body relocated an `a` because an `a` is something it cannot see the size
             * of; substituting unit takes the something away, and what is left has no storage to
             * relocate and no lower value to be. `Just(->v)` over a `Maybe(())` is where this
             * arrives - a concrete body never reaches sinkValue with a unit, because reading one out
             * of a place produces no value in the first place.
             */
            if(isUnit(clone.global, type)) {
                clone.values.add(pointer, ModulePtr<Value>(nullptr));
                return;
            }

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

            result = cloned;
            break;
        }
        case Value::Exchange: {
            auto& exchange = (InstExchange&)inst;
            auto place = clonePlace(clone, exchange.place);
            auto cloned = resolver.emit<InstExchange>(inst.source, inst.name, type, place,
                                                      cloneValue(clone, exchange.value));

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
            auto cloned = resolver.create<InstNative>(inst.source, inst.name, type, native.op,
                                                      native.method);

            cloned->relocates = native.relocates;

            for(auto arg: native.args.contents(clone.local)) {
                cloned->args.push(clone.module.arena, cloneValue(clone, arg));
            }

            resolver.append(cloned);
            result = cloned;
            break;
        }
        case Value::Atomic: {
            auto& atomic = (InstAtomic&)inst;
            auto cloned = resolver.create<InstAtomic>(inst.source, inst.name, type, atomic.kind, atomic.order);

            cloned->failure = atomic.failure;
            cloned->weak = atomic.weak;

            for(auto arg: atomic.args.contents(clone.local)) {
                cloned->args.push(clone.module.arena, cloneValue(clone, arg));
            }

            resolver.append(cloned);
            result = cloned;
            break;
        }
        case Value::AtomicOk: {
            auto cloned = resolver.create<InstAtomicOk>(inst.source, inst.name, type,
                                                        cloneValue(clone, ((InstAtomicOk&)inst).cas));
            resolver.append(cloned);
            result = cloned;
            break;
        }
        case Value::Cast:
        case Value::Bitcast:
        case Value::Neg:
        case Value::Not:
        case Value::ByteSwap:
        case Value::CountBits:
        case Value::LeadingZeros:
        case Value::TrailingZeros:
        case Value::Sqrt:
        case Value::Abs:
        case Value::Trunc:
        case Value::Floor:
        case Value::Ceil:
        case Value::Round:
            result = resolver.emit<InstUnary>(inst.source, inst.name, type, inst.kind,
                                              cloneValue(clone, ((InstUnary&)inst).from));
            break;
        case Value::Fma:
            result = resolver.emit<InstFma>(inst.source, inst.name, type,
                                            cloneValue(clone, ((InstFma&)inst).a),
                                            cloneValue(clone, ((InstFma&)inst).b),
                                            cloneValue(clone, ((InstFma&)inst).c));
            break;
        case Value::Sha256Rounds:
            result = resolver.emit<InstSha256Rounds>(inst.source, inst.name, type,
                                                     cloneValue(clone, ((InstSha256Rounds&)inst).state),
                                                     cloneValue(clone, ((InstSha256Rounds&)inst).feed),
                                                     cloneValue(clone, ((InstSha256Rounds&)inst).keys));
            break;
        case Value::ShaBinary:
            result = resolver.emit<InstShaBinary>(inst.source, inst.name, type,
                                                  cloneValue(clone, ((InstShaBinary&)inst).lhs),
                                                  cloneValue(clone, ((InstShaBinary&)inst).rhs),
                                                  ((InstShaBinary&)inst).op);
            break;
        case Value::Add:
        case Value::Sub:
        case Value::Mul:
        case Value::MulHi:
        case Value::Div:
        case Value::Rem:
        case Value::Shl:
        case Value::Shr:
        case Value::Sar:
        case Value::Rol:
        case Value::Ror:
        case Value::And:
        case Value::Or:
        case Value::Xor:
        case Value::BitsUpTo:
        case Value::GatherBits:
        case Value::ScatterBits:
        case Value::Crc32: {
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
        /*
         * The vector kinds, cloned like every other computation.
         *
         * `type` is already the substituted one, which is what carries the whole of the natural
         * form's deferral through specialization: a `Vec(a)` in the generic body has no lane count,
         * and the clone of an instruction over it is an instruction over the resolved vector because
         * `cloneType` resolved it. Nothing here has to know that happened.
         */
        case Value::VecSplat:
            result = resolver.emit<InstVecSplat>(inst.source, inst.name, type,
                                                 cloneValue(clone, ((InstVecSplat&)inst).from));
            break;
        case Value::VecLane: {
            auto& lane = (InstVecLane&)inst;
            result = resolver.emit<InstVecLane>(inst.source, inst.name, type,
                                                cloneValue(clone, lane.from), lane.lane);
            break;
        }
        case Value::VecWithLane: {
            auto& lane = (InstVecLane&)inst;
            result = resolver.emit<InstVecLane>(inst.source, inst.name, type,
                                                cloneValue(clone, lane.from), lane.lane,
                                                cloneValue(clone, lane.value));
            break;
        }
        case Value::VecShuffle: {
            auto& shuffle = (InstVecShuffle&)inst;
            auto cloned = resolver.create<InstVecShuffle>(inst.source, inst.name, type,
                                                          cloneValue(clone, shuffle.left),
                                                          cloneValue(clone, shuffle.right));

            for(auto entry: shuffle.pattern) cloned->pattern.push(entry);
            resolver.append(cloned);
            result = cloned;
            break;
        }
        case Value::VecReduce: {
            auto& reduce = (InstVecReduce&)inst;
            result = resolver.emit<InstVecReduce>(inst.source, inst.name, type,
                                                  cloneValue(clone, reduce.from), reduce.reduce);
            break;
        }
        case Value::Call: {
            auto& call = (InstCall&)inst;
            ArgList args;
            for(auto arg: call.args.contents(clone.local)) args.push(cloneValue(clone, arg));

            auto value = resolver.emitDirectCall(call.callee, toBuffer(args), inst.source, nullptr, inst.name);

            if(value) {
                clone.values.add(pointer, value);
                pairClonedLocal(clone, inst, value);
            }

            return;
        }
        case Value::Symbol: {
            auto& symbol = (InstSymbol&)inst;

            // The one symbol a specialization may not share: a continuation lifted out of *this*
            // body names this body's type variables, so it gets a clone of its own under the same
            // bindings. Everything else - a named function, a global, a lifted body of some other
            // function - is the same symbol in every specialization.
            result = resolver.emit<InstSymbol>(inst.source, inst.name, type,
                                               cloneLiftedCallee(clone, symbol.callee), symbol.global);
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

            // A clone of a `yield` is still a `yield`: which parameter the callee is was decided by
            // the declaration, and substituting types does not reach it.
            dynamic->handover = call.handover;

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
            resolver.emit<InstJe>(inst.source, StringId(), type, cloneValue(clone, branch.cond),
                                  cloneBlock(clone, branch.thenBlock), cloneBlock(clone, branch.elseBlock));
            return;
        }
        case Value::Jmp:
            resolver.emit<InstJmp>(inst.source, StringId(), type, cloneBlock(clone, ((InstJmp&)inst).target));
            return;
        case Value::Ret:
            resolver.emit<InstRet>(inst.source, StringId(), type, cloneValue(clone, ((InstRet&)inst).value));
            return;
        case Value::Drop:
            /*
             * Dropped rather than cloned, and that is what makes the specialization correct.
             *
             * A drop is not something a body *says*; it is where the pass that placed it found a
             * value's last use, and both halves of it name functions chosen for a type this clone is
             * about to substitute. The clone is an ordinary function that will be analyzed like any
             * other - specializations are generated *during* analysis, and the sweep repeats until
             * no module grew, so this one gets its own pass - and that pass inserts its own drops
             * against its own types. Copying these in as well would be the same value released
             * twice.
             *
             * It only ever sees one at all because of the erased mode: a generic body gets drops
             * where it is the code rather than a template (see analyzeFunction), and a call site
             * that specializes such a body anyway is exactly this case.
             */
            return;
        default:
            clone.context.diagnostics.error("internal: this instruction cannot be specialized"_v, inst.source);
            return;
    }

    if(result) {
        auto value = resolver.ref((Inst*)result);
        clone.values.add(pointer, value);
        pairClonedLocal(clone, inst, value);
    }
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
        created->loan = arg->loan;
        if(arg->lazyType) created->lazyType = cloneType(clone, arg->lazyType);
        clone.values.add((ModulePtr<Value>)argPointer, (ModulePtr<Value>)(created - local));
    }

    // A Place names a local by index, so the table is copied position for position before any
    // instruction that addresses it. The value each one holds is filled in afterwards, once the
    // instruction that produced it has been cloned.
    for(Size i = 0; i < from.localCount(); i++) {
        auto slot = from.localAt(local, U32(i));
        // The two fields addLocal does not build positionally come across by assignment, which is
        // also how they were set on the original - see Local.
        auto copied = Local {
            cloneType(clone, slot.type), slot.name, nullptr, slot.convention, slot.storage, slot.borrowed,
            slot.closureEnv,
        };

        copied.materialized = slot.materialized;
        copied.viewOf = slot.viewOf;
        to.locals.push(clone.module.arena, copied);
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

        for(auto instruction: block->instructions(local)) {
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
        IrEditor(clone.module, to).setLocalValue(U32(i), clone.resolver.ref(allocation));
        materialized.set(i, true);

        clone.resolver.initialize(Place::inLocal(U32(i)), argument, clone.source);
    }

    /*
     * A parameter's own slot, filled in before the body rather than with the rest of the table.
     *
     * Everything else waits for the instruction that produced it to be cloned, which is the only
     * order that works for an allocation. A parameter has no such instruction - its value is the
     * `Arg`, and every one of those exists already - and something being cloned may have to know
     * *while* it is being cloned that the slot is where the parameter lives. An intrinsic expanded
     * here is what does: it asks findPlace for its receiver, and an empty entry answers "this
     * aggregate has no place", which makes it build storage of its own and store a value the frame
     * only borrows into it.
     */
    // Which slots hold a value before the body is cloned, and therefore have their uses recorded by
    // `IrEditor::append` in the ordinary way. Everything else is owed them afterwards - see below.
    // On `Clone` rather than here, because `pairClonedLocal` adds to it while the body is cloned.
    auto& filled = clone.filled;
    filled.reset(from.localCount());

    for(Size i = 0; i < from.localCount(); i++) {
        if(materialized[i]) {
            filled.set(i, true);
            continue;
        }

        auto slot = from.localAt(local, U32(i));
        if(!slot.value || local[slot.value]->kind != Value::Arg) continue;

        IrEditor(clone.module, to).setLocalValue(U32(i), cloneDefinition(clone, slot.value));
        filled.set(i, true);
    }

    // Phi shells first: a phi is the one instruction whose operands need not dominate it, so
    // anything else may reference one before the block it lives in has been reached.
    for(auto blockPointer: from.blocks.contents(local)) {
        for(auto phiPointer: local[blockPointer]->phis(local)) {
            auto phi = local[phiPointer];
            clone.resolver.current = cloneBlock(clone, blockPointer);

            auto created = clone.resolver.create<InstPhi>(phi->source, phi->name, cloneType(clone, phi->type));
            clone.values.add((ModulePtr<Value>)phiPointer, (ModulePtr<Value>)(created - local));
        }
    }

    for(auto blockPointer: from.blocks.contents(local)) {
        auto block = local[blockPointer];
        clone.resolver.current = cloneBlock(clone, blockPointer);

        for(auto instruction: block->instructions(local)) {
            cloneInstruction(clone, *local[instruction]);
        }

        if(block->terminator()) cloneInstruction(clone, *local[block->terminator()]);
    }

    for(auto blockPointer: from.blocks.contents(local)) {
        for(auto phiPointer: local[blockPointer]->phis(local)) {
            auto phi = local[phiPointer];
            auto created = (InstPhi*)local[ModulePtr<Value>(clone.values.getValue(phiPointer).unwrap())];

            for(auto input: phi->inputs.contents(local)) {
                // A phi's operand arrives on the edge, so anything cloning it has to emit into the
                // predecessor rather than wherever the last block-clone happened to leave off. The
                // only thing that emits here is readIfMaterialized, and a load in the wrong block is
                // a value that does not dominate its use.
                auto from = cloneBlock(clone, input.block);
                clone.resolver.current = from;

                created->inputs.push(clone.module.arena, PhiInput { from, cloneValue(clone, input.value) });
            }

            IrEditor(clone.module, to).append(*local[cloneBlock(clone, blockPointer)], created);
        }
    }

    for(Size i = 0; i < from.localCount(); i++) {
        if(materialized[i] || filled[i]) continue;

        auto definition = cloneDefinition(clone, from.localAt(local, U32(i)).value);
        IrEditor(clone.module, to).setLocalValue(U32(i), definition);

        // The instruction's own copy of the same number - see setResultLocal. This is the fixup
        // `pairClonedLocal` exists to run early for; anything it did not reach arrives here.
        if(definition) setResultLocal(*local[definition], U32(i));
    }

    /*
     * And the uses those slots owe, which is the price of filling the table last.
     *
     * A place rooted in a local is a use of the value that fills the slot - `addPlaceUse` in
     * edit.cpp, which is what makes "everything that touches this storage" answerable by walking one
     * use list. Every instruction above was added while its slot still held nothing, so `recordUses`
     * had nothing to attribute the use to and recorded none, and the specialization went out with
     * a value whose readers were invisible.
     *
     * `compiler/opt` used to hide it, by rebuilding every list before anything else ran there. What
     * it did not hide was everything in between - the ownership analyses read use lists, and so does
     * `lowerProgram`'s decision about which aggregate slots can be held in registers, which `-no-opt`
     * reaches with no repair in front of it. Found by verifyFunction, on a `reserve(U8)` whose `wide`
     * was named by six instructions and read by none. There is no such repair any more.
     */
    IrEditor editor(clone.module, to);

    for(auto blockPointer: to.blocks.contents(local)) {
        for(auto instruction: local[blockPointer]->instructions(local)) {
            // Per place rather than per instruction, and only the slots that were empty: a swap
            // names two, either of which may have been filled already, and a use recorded twice is
            // as wrong as one not recorded at all.
            eachPlace(*local[instruction], [&](const Place& place) {
                if(place.root != PlaceRoot::Local || place.local >= to.localCount()) return;

                // A slot the *clone* created - the storage a Move, a Copy or a call result wanted -
                // was filled the moment it was made, so its uses were recorded the ordinary way.
                // `filled` is the original table's size, and reading past it answers false.
                if(place.local >= from.localCount() || filled[place.local]) return;

                editor.recordUse(to.localAt(local, place.local).value, instruction);
            });
        }
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

    SmallArray<FunArg, 8> args;
    for(auto argPointer: function.args.contents(local)) {
        auto declared = local[argPointer];
        args.push(FunArg { declared->declaredType(), declared->name, declared->convention,
                           declared->loan, declared->isLazy() });
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

    // And the same for a class default, which carries no `instanceOf` to be recognized by: what a
    // deferred dispatch reaches is this specialization, so it is the one checkClassBorrows has to
    // hold to the promise rather than only the generic form it came from.
    specialized->classDefault = generic->classDefault;

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

ModulePtr<Function> instanceMember(Module& module, GlobalPtr<TypeClass> typeClass, TypePtr type,
                                   U16 member, LocationId source) {
    TypePtr args[] = { type };
    auto match = matchInstance(module, typeClass, toBuffer(args));
    if(!match) return nullptr;

    auto local = *module.arena;
    auto instance = local[match.instance];
    if(member >= instance->functions.size()) return nullptr;

    auto implementation = instance->functions.get(local, member);
    if(!implementation) return nullptr;

    if(local[implementation]->gen) {
        implementation = instantiateFunction(module, implementation, toBuffer(match.args), source);
        if(!implementation) return nullptr;
    }

    (*module.arena)[implementation]->used = true;
    return implementation;
}

// The first member, which is what a one-member class - `Reclaim`, `Drop` - is asked for. `Show` has
// two and asks for them by index through the function above.
ModulePtr<Function> instanceImplementation(Module& module, GlobalPtr<TypeClass> typeClass, TypePtr type,
                                           LocationId source) {
    return instanceMember(module, typeClass, type, 0, source);
}
