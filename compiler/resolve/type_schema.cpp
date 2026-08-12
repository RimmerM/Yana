/*
 * Generic environments: which slot holds which answer.
 *
 * A generic body reads what it cannot know off a schema its caller filled in, so caller and callee
 * have to agree that slot N means the same thing. That agreement is this file: one canonical
 * numbering, grown as a requirement is discovered, and read back by both sides through the same
 * four `gen*Slot` functions - see Implementation-Generics.md.
 */

#include "type_internal.h"
#include "generic.h"
#include "module.h"
#include "name.h"
#include "index.h"

/*
 * Resolving written types.
 */

static GlobalPtr<GenType> findGen(GlobalBase global, GenEnv* env, StringId name) {
    if(!env) return nullptr;

    for(auto type: env->types.contents(global)) {
        if(global[type]->name == name) return type;
    }

    return nullptr;
}

GlobalPtr<GenType> findGenVariable(Module& module, GenEnv& env, StringId name) {
    return findGen(*module.types, &env, name);
}

GlobalPtr<GenType> genVariable(Module& module, GenEnv& env, StringId name, LocationId source) {
    auto global = *module.types;
    if(auto existing = findGen(global, &env, name)) return existing;
    if(!env.open) return nullptr;

    auto type = new (module.types) GenType(&env - global, name, U16(env.types.size()));
    type->source = source;
    auto pointer = type - global;
    env.types.push(module.types, pointer);
    invalidateGenSchema(env);

    return pointer;
}

/*
 * The canonical numbering.
 *
 * Built in the order Implementation-Generics.md part 2 gives, and the order matters for exactly one
 * reason: emitted code loads slot N, so the caller filling the environment and the callee reading it
 * have to agree on what N is without either of them consulting the other. Deriving the numbering
 * from the finished context is what makes them agree - two compilations of the same context produce
 * the same numbers, however the requirements were discovered.
 *
 * Within each group the source order is kept rather than sorted. The declared variables and the
 * explicit constraints have a written order that a diagnostic can point at, and the inferred ones
 * are deduplicated structurally as they are added (see requireClass), so what is left is already
 * the order in which the body first needed each of them.
 */
GenSchema& genSchemaOf(Module& module, GenEnv& env) {
    auto global = *module.types;
    if(env.schema) return *global[env.schema];

    auto schema = new (module.types) GenSchema;
    env.schema = schema - global;

    U16 index = 0;

    // 1. The type variables, in declaration order, then the derived type expressions the body
    //    needed. Both are TypeDesc slots and are numbered together, because what distinguishes them
    //    is where they came from rather than what a reader does with them.
    for(auto variable: env.types.contents(global)) {
        // A const parameter is a variable of this context and is *not* a type descriptor: what it
        // stands for is a number. It is numbered in group 2 below, which is what keeps both
        // fixed-width groups a prefix - Implementation-Const-Generics.md §3.1.
        if(global[variable]->kind == GenKind::Const) continue;

        GenSlot slot;
        slot.kind = GenSlotKind::Type;
        slot.index = index++;
        slot.type = (Type*)global[variable] - global;
        slot.name = global[variable]->name;
        schema->slots.push(module.types, slot);
    }

    // Applied expressions the body constructs or matches - `Maybe(a)`, `Pair(a, b)`. Their
    // descriptors are built once by the caller rather than by applying a type constructor at run
    // time, which is what keeps ordinary rank-1 generics away from higher-kinded machinery.
    for(auto derived: env.derivedTypes.contents(global)) {
        GenSlot slot;
        slot.kind = GenSlotKind::Type;
        slot.index = index++;
        slot.type = derived;
        schema->slots.push(module.types, slot);
    }

    schema->typeCount = index;

    /*
     * 2. The const parameters, in declaration order.
     *
     * One integer each and nothing to point at, which is why they sit here rather than among the
     * witnesses: `typeCount` exists so that everything else indexes off it, and a second fixed-width
     * group beside it keeps that true of a caller filling an environment as well as of a reader.
     */
    for(auto variable: env.types.contents(global)) {
        if(global[variable]->kind != GenKind::Const) continue;

        GenSlot slot;
        slot.kind = GenSlotKind::Const;
        slot.index = index++;
        slot.type = (Type*)global[variable] - global;
        slot.name = global[variable]->name;
        slot.result = global[variable]->constType;
        schema->slots.push(module.types, slot);
    }

    schema->constCount = U16(index - schema->typeCount);

    // 3. The class constraints, declared ones and inferred ones alike. By the time anything reads
    //    the numbering the two are the same entry, which is the point of recording them in one list.
    for(auto constraint: env.classes.contents(global)) {
        if(!constraint.typeClass) continue;

        GenSlot slot;
        slot.kind = GenSlotKind::Class;
        slot.index = index++;
        slot.typeClass = constraint.typeClass;
        slot.name = constraint.name;
        slot.source = constraint.source;

        for(auto arg: constraint.args.contents(global)) slot.args.push(module.types, arg);
        schema->slots.push(module.types, slot);
    }

    for(auto constraint: env.dispatched.contents(global)) {
        GenSlot slot;
        slot.kind = GenSlotKind::Class;
        slot.index = index++;
        slot.typeClass = constraint.typeClass;
        slot.name = constraint.name;
        slot.source = constraint.source;

        for(auto arg: constraint.args.contents(global)) slot.args.push(module.types, arg);
        schema->slots.push(module.types, slot);
    }

    for(auto constraint: env.properties.contents(global)) {
        GenSlot slot;
        slot.kind = GenSlotKind::Property;
        slot.index = index++;
        slot.type = constraint.owner;
        slot.result = constraint.result;
        slot.name = constraint.field;
        slot.source = constraint.source;
        schema->slots.push(module.types, slot);
    }

    for(auto constraint: env.functions.contents(global)) {
        GenSlot slot;
        slot.kind = GenSlotKind::Function;
        slot.index = index++;
        slot.type = constraint.signature;
        slot.name = constraint.name;
        slot.source = constraint.source;
        schema->slots.push(module.types, slot);
    }

    return *schema;
}

U16 genTypeSlot(Module& module, GenEnv& env, TypePtr type) {
    auto global = *module.types;
    auto& schema = genSchemaOf(module, env);

    for(auto slot: schema.slots.contents(global)) {
        if(slot.kind == GenSlotKind::Type && slot.type == type) return slot.index;
    }

    return maxLimit<U16>;
}

U16 genConstSlot(Module& module, GenEnv& env, TypePtr variable) {
    auto global = *module.types;
    auto& schema = genSchemaOf(module, env);

    for(auto slot: schema.slots.contents(global)) {
        if(slot.kind == GenSlotKind::Const && slot.type == variable) return slot.index;
    }

    return maxLimit<U16>;
}

U16 genClassSlot(Module& module, GenEnv& env, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    auto global = *module.types;
    auto& schema = genSchemaOf(module, env);

    for(auto slot: schema.slots.contents(global)) {
        if(slot.kind != GenSlotKind::Class || slot.typeClass != typeClass) continue;
        if(sameTypes(slot.args, global, args)) return slot.index;
    }

    return maxLimit<U16>;
}

U16 genPropertySlot(Module& module, GenEnv& env, TypePtr owner, StringId field) {
    auto global = *module.types;
    auto& schema = genSchemaOf(module, env);

    for(auto slot: schema.slots.contents(global)) {
        if(slot.kind == GenSlotKind::Property && slot.type == owner && slot.name == field) {
            return slot.index;
        }
    }

    return maxLimit<U16>;
}

U16 genFunctionSlot(Module& module, GenEnv& env, StringId name, TypePtr signature) {
    auto global = *module.types;
    auto& schema = genSchemaOf(module, env);

    for(auto slot: schema.slots.contents(global)) {
        if(slot.kind == GenSlotKind::Function && slot.name == name && slot.type == signature) {
            return slot.index;
        }
    }

    return maxLimit<U16>;
}

void requireClassSlot(Module& module, GenEnv& env, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                      LocationId source) {
    if(!typeClass) return;

    // A witness the context can already reach needs no slot of its own, whether it sits in one
    // directly or inside one that names it as a superclass. The second is what keeps
    // `fn (Num(a)) inc(x: a) = x + 1` to a single witness: the literal's `FromInt(a)` is loaded out
    // of the `Num` witness the caller already passed.
    SuperclassSteps supers;
    if(genWitnessPath(module, env, typeClass, args, supers) != maxLimit<U16>) return;

    ClassConstraint constraint;
    constraint.typeClass = typeClass;
    constraint.name = (*module.types)[typeClass]->name;
    constraint.source = source;
    for(auto arg: args) constraint.args.push(module.types, arg);

    env.dispatched.push(module.types, constraint);
    invalidateGenSchema(env);
}

void requireTypeSlot(Module& module, GenEnv& env, TypePtr type) {
    auto global = *module.types;
    if(!type || !isGeneric(global, type)) return;

    // A bare type variable already has a slot from the declaration, and a duplicate applied
    // expression is one slot however many times the body writes it.
    if(genTypeSlot(module, env, type) != maxLimit<U16>) return;
    if(global[type]->kind == Type::Gen) return;

    // Recorded on the context rather than on the schema, so that rebuilding the numbering produces
    // the same answer however many times it happens - see genSchemaOf. Derived expressions stay
    // inside the leading run of TypeDesc slots, which is what keeps `typeCount` meaningful.
    env.derivedTypes.push(module.types, type);
    invalidateGenSchema(env);
}
