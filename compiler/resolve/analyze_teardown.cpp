#include "analyze_pass.h"
#include "generic.h"
#include "name.h"

/*
 * Derived teardown glue.
 *
 * "Recurse into each member, and for the reclaim half release this type's own storage" written out
 * as a function, so that it can be printed, called recursively and lowered like anything else. It
 * takes a raw pointer to what it is tearing down, which is what an InstDrop hands it.
 *
 * There are two of these per type rather than one, because Design-Memory §4's two halves are
 * elidable under different conditions and a caller has to be able to run one without the other: a
 * region reset discharges every `Reclaim` in bulk and leaves every `Drop` to still run at last use.
 * Generating one function that did both would make that choice unavailable to anyone downstream.
 *
 * Interned per type and per half on the Program: a record with two fields of one type generates one
 * of each, and a type reachable from itself terminates because the entry is added before the body
 * is built.
 */

// The two halves differ only in which classification decides whether a member contributes and which
// instance an authored member reaches, so they share one generator rather than being written twice.
static ModulePtr<Function> teardownGlueFor(Module& module, TypePtr type, Teardown half, LocationId source);

static TeardownKind teardownKind(const Ownership& ownership, Teardown half) {
    return half == Teardown::Drop ? ownership.drop : ownership.reclaim;
}

// The name a glue function is printed and linked under. It is not addressable in source; what it
// needs is to be unique and to say what it tears down.
static StringId teardownGlueName(Module& module, TypePtr type, Teardown half) {
    return derivedName(module, half == Teardown::Drop ? "drop$"_v : "reclaim$"_v, type);
}

// The implementation one type's teardown half runs, or null when that half has nothing to do.
ModulePtr<Function> teardownFor(Module& module, TypePtr type, Teardown half, LocationId source) {
    auto ownership = ownershipOf(module, type);
    auto kind = teardownKind(ownership, half);

    if(kind == TeardownKind::Authored) {
        auto typeClass = half == Teardown::Drop ? module.coreClasses.drop : module.coreClasses.reclaim;
        return instanceImplementation(module, typeClass, type, source);
    }

    if(kind == TeardownKind::Derived) return teardownGlueFor(module, type, half, source);
    return nullptr;
}

// Emits one InstDrop for each member of `content` that has something to do for this half, projected
// off `base`. Shared by the tuple case and by a record constructor's payload.
static void teardownMembers(ExprResolver& resolver, Module& module, Place base, TypePtr content,
                            Teardown half, LocationId source) {
    auto global = *module.types;
    if(!content || global[content]->kind != Type::Tup) return;

    auto tuple = (TupType*)global[content];
    U16 index = 0;

    for(auto field: tuple->fields.contents(global)) {
        auto implementation = teardownFor(module, field.type, half, source);
        auto kind = teardownKind(ownershipOf(module, field.type), half);

        if(implementation) {
            auto place = resolver.project(base, ProjectionKind::Field, index);
            auto isDrop = half == Teardown::Drop;
            auto drop = resolver.emit<InstDrop>(source, 0, module.scalar.unit, place,
                                                isDrop ? kind : TeardownKind::None,
                                                isDrop ? TeardownKind::None : kind);

            if(isDrop) drop->drop = implementation;
            else drop->reclaim = implementation;
        }

        index++;
    }
}

// Whether this member contributes anything to this half of a teardown.
static bool contributes(Module& module, TypePtr type, Teardown half) {
    return teardownKind(ownershipOf(module, type), half) != TeardownKind::None;
}

/*
 * A function value's teardown.
 *
 * The word that matters is the environment, and what has to run is whatever the *closure header*
 * says - the static data in front of the entry point the code word names (ClosureHeaderLayout). That
 * indirection is what makes releasing a closure a per-closure question without making it a
 * per-value one: two closures of one function type can capture completely different things, and
 * which of them this is was decided by which lambda it came from.
 *
 * A value that captured nothing has a null environment, so this is a branch that never fires rather
 * than a second representation - and it is also why nothing here reads a header that was never
 * emitted: only a capturing lambda has one, and only a capturing lambda's values reach the branch.
 *
 * Nothing here decides anything about the environment's storage, and it is worth saying why not:
 * where one lambda's environment lives is fixed at compile time, and this code is not per lambda -
 * it is interned per function *type*, and one `(Int) -> Int` teardown serves closures over the frame,
 * closures over the heap and function values with no environment at all. So the decision is spent
 * where it is known, in which reclaim the header names, and what is left here is a call.
 */
static void teardownFunValue(ExprResolver& resolver, Module& module, Place base, Teardown half,
                             LocationId source) {
    auto address = funValueFieldType(module, FunValueLayout::kEnv);
    auto word = module.scalar.long_;

    auto env = resolver.load(resolver.project(base, ProjectionKind::Field, FunValueLayout::kEnv), source);
    auto empty = resolver.constantBits(address, 0, source);
    auto present = resolver.emit<InstCmp>(source, 0, module.scalar.bool_, env, empty, CompareOp::Ne);

    auto run = resolver.addBlock();
    auto exit = resolver.addBlock();
    resolver.terminate(resolver.emit<InstJe>(source, 0, module.scalar.unit, resolver.ref(present), run, exit));
    resolver.current = run;

    /*
     * The header, from wherever this target keeps it.
     *
     * Native keeps it in front of the entry point, so it is found by subtracting a constant from the
     * code word - through the integer rather than as a place projection, because the offset is
     * negative: a projection walks *into* an aggregate, and this walks backwards out of one. The two
     * casts are both reinterpretations of one machine word - asInt and asPtr - so what they cost is
     * nothing and what they buy is that the arithmetic is stated where the layout is.
     *
     * A target whose code word is not an address has no bytes in front of it to subtract from, and
     * attaches the header to the code word instead. That is FunValueLayout::kHeader, and asking for
     * it is a projection like any other - which is the whole of the difference, because everything
     * around it reads the same two slots out of the same layout either way.
     */
    auto headerType = resolvePointerType(module, closureHeaderPlaceType(module));
    Place header;

    if(isJsMode(module.context.settings.mode)) {
        header = Place::atPointer(resolver.load(
            resolver.project(base, ProjectionKind::Field, FunValueLayout::kHeader), source));
    } else {
        auto codeWord = resolver.load(resolver.project(base, ProjectionKind::Field, FunValueLayout::kCode), source);
        auto codeInt = resolver.ref(resolver.emit<InstUnary>(source, 0, word, Value::Cast, codeWord));
        auto distance = resolver.constantBits(word, ClosureHeaderLayout::kSize_, source);
        auto headerInt = resolver.ref(resolver.emit<InstBinary>(source, 0, word, Value::Sub, codeInt, distance));

        header = Place::atPointer(
            resolver.ref(resolver.emit<InstUnary>(source, 0, headerType, Value::Cast, headerInt)));
    }

    auto slot = half == Teardown::Drop ? ClosureHeaderFields::kDrop : ClosureHeaderFields::kReclaim;
    auto operation = resolver.load(resolver.project(header, ProjectionKind::Field, slot), source);

    // No signature: this is the compiler calling a teardown it generated, not a program calling a
    // function value, so there are no conventions to honour and no environment convention either.
    auto teardown = resolver.create<InstCallDyn>(source, 0, module.scalar.unit, nullptr, operation, nullptr);
    teardown->args.push(module.arena, env);
    resolver.append(teardown);

    resolver.terminate(resolver.emit<InstJmp>(source, 0, module.scalar.unit, exit));
    resolver.current = exit;
}

/*
 * Built in the module that asked for it, not in Core.
 *
 * The glue has to resolve `instance Reclaim(Buffer)` for each of its members, and instance lookup is
 * relative to the module doing the looking - so building it in Core would find nothing an ordinary
 * program declared and silently produce empty glue. Interning is still program-wide, which relies
 * on instance coherence: two modules that can both see a type agree on what tearing it down means,
 * and the language already requires that.
 */
static ModulePtr<Function> teardownGlueFor(Module& module, TypePtr type, Teardown half, LocationId source) {
    auto& program = module.program;
    auto& interned = half == Teardown::Drop ? program.dropGlue : program.reclaimGlue;
    if(auto found = interned.get(U32(type))) return found.unwrap();

    // addAnonymousFunction already registers it in the module's function order, which is what puts
    // it in front of printing and lowering.
    auto function = addAnonymousFunction(module, teardownGlueName(module, type, half), source);
    auto pointer = function - *module.arena;

    // Registered before the body is built, so a type reachable from itself finds the entry rather
    // than generating glue forever.
    *interned.add(U32(type)).value = pointer;

    function->returnType = module.scalar.unit;
    function->used = true;

    auto valueName = module.context.addQualifiedName("value", 5, 1);
    auto arg = function->addArg(module, valueName, resolvePointerType(module, type), source);

    ExprResolver resolver(module.context, module, *function);
    auto base = Place::atPointer((ModulePtr<Value>)(arg - *module.arena));
    auto global = *module.types;

    if(global[type]->kind == Type::Fun) {
        teardownFunValue(resolver, module, base, half, source);
    } else if(global[type]->kind == Type::Tup) {
        teardownMembers(resolver, module, base, type, half, source);
    } else if(global[type]->kind == Type::Record) {
        auto record = (RecordType*)global[type];

        if(record->layout == RecordType::Single) {
            auto content = record->constructors.get(global, 0).content;
            teardownMembers(resolver, module, resolver.project(base, ProjectionKind::Downcast, 0),
                            content, half, source);
        } else if(record->layout == RecordType::Multi) {
            /*
             * Each constructor carries a different payload, so the glue reads the discriminant and
             * tears down the members of whichever one is present.
             *
             * Built as a chain of tests rather than as a jump table, because that is what the IR
             * has: `je` is its only conditional, and a record with a dozen constructors is not the
             * case worth a second control-flow construct for. A constructor whose payload has
             * nothing to do for this half is skipped entirely, so the chain is as long as the
             * number of constructors that contribute rather than the number that exist.
             */
            auto exit = resolver.addBlock();

            for(auto constructor: record->constructors.contents(global)) {
                auto content = constructor.content;
                if(!content || !contributes(module, content, half)) continue;

                auto discriminant = resolver.load(
                    resolver.project(base, ProjectionKind::Discriminant, 0), source);

                auto index = resolver.makeInt(source, module.scalar.int_, constructor.index);
                auto matches = resolver.emit<InstCmp>(source, 0, module.scalar.bool_,
                                                      discriminant, index, CompareOp::Eq);

                auto drops = resolver.addBlock();
                auto next = resolver.addBlock();
                resolver.terminate(resolver.emit<InstJe>(source, 0, module.scalar.unit,
                                                         resolver.ref(matches), drops, next));

                resolver.current = drops;
                teardownMembers(resolver, module, resolver.project(base, ProjectionKind::Downcast,
                                                                   U16(constructor.index)),
                                content, half, source);
                resolver.terminate(resolver.emit<InstJmp>(source, 0, module.scalar.unit, exit));

                resolver.current = next;
            }

            resolver.terminate(resolver.emit<InstJmp>(source, 0, module.scalar.unit, exit));
            resolver.current = exit;
        }
    }

    resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, nullptr));
    return pointer;
}

/*
 * What an authored `Reclaim` is allowed to do.
 *
 * Design-Memory §4 constrains it by shape rather than trusting it for purity, and says exactly why:
 * a region discharges every `Reclaim` inside it in bulk, at a point the author did not choose, so a
 * `Reclaim` that ran an effect would run it somewhere the program never asked for. The permitted
 * body is control flow, arithmetic over its own metadata, reads of storage it owns, calls to the
 * compiler's per-member teardown, and storage release - and no other call.
 *
 * Checking that is a walk over the call graph, which is what this is. The author is trusted about
 * "I call nothing else" and never about "my members are effect-free": whether `Map(k, v)`'s
 * teardown has effects is *computed* from whether `k` and `v` have a `Drop`, above.
 */
bool checkReclaimShape(Module& module, Function& function) {
    auto local = *module.arena;
    auto& program = module.program;
    auto ok = true;

    auto permitted = [&](ModulePtr<Function> callee) {
        if(!callee) return true;
        if(callee == program.freeHeap || callee == program.allocateHeap) return true;

        auto target = local[callee];

        // Another type's teardown - the per-member recursion this one is allowed to drive, whether
        // it is generated glue or an authored instance of either half.
        if(target->instanceOf == program.coreClasses.reclaim) return true;
        if(target->instanceOf == program.coreClasses.drop) return true;

        for(auto entry: program.reclaimGlue) {
            if(entry == callee) return true;
        }

        for(auto entry: program.dropGlue) {
            if(entry == callee) return true;
        }

        // A specialization stands in for whatever its generic original was, so it is judged by the
        // same rule rather than by having a different name.
        if(target->specializationOf) {
            auto generic = local[target->specializationOf];
            if(generic->instanceOf == program.coreClasses.reclaim) return true;
            if(generic->instanceOf == program.coreClasses.drop) return true;
        }

        return false;
    };

    for(auto blockPointer: function.blocks.contents(local)) {
        for(auto instruction: local[blockPointer]->instructions.contents(local)) {
            auto& inst = *local[instruction];
            if(inst.kind != Value::Call) continue;

            auto callee = ((InstCall&)inst).callee;
            if(permitted(callee)) continue;

            module.context.diagnostics.error("an authored `Reclaim` may only release storage - it cannot call %@, because a region discharges every `Reclaim` inside it in bulk and this would then run somewhere the program never asked for. Write a `Drop` for an effect that has to happen at last use"_v,
                                             inst.source, module.context.findName(local[callee]->name));
            ok = false;
        }
    }

    return ok;
}

// Declared in analyze.h so that a TypeDesc can name both halves - see witness.cpp.
ModulePtr<Function> teardownImplementation(Module& module, TypePtr type, Teardown half, LocationId source) {
    return teardownFor(module, type, half, source);
}
