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
static ModulePtr<Function> teardownGlueFor(Module& module, TypePtr type, Teardown half, LocationId source,
                                           bool headerKnown = false);

static TeardownKind teardownKind(const Ownership& ownership, Teardown half) {
    return half == Teardown::Drop ? ownership.drop : ownership.reclaim;
}

// The name a glue function is printed and linked under. It is not addressable in source; what it
// needs is to be unique and to say what it tears down.
static StringId teardownGlueName(Module& module, TypePtr type, Teardown half, bool headerKnown) {
    if(headerKnown) {
        return derivedName(module, half == Teardown::Drop ? "dropKnown$"_v : "reclaimKnown$"_v, type);
    }

    return derivedName(module, half == Teardown::Drop ? "drop$"_v : "reclaim$"_v, type);
}

// The implementation one type's teardown half runs, or null when that half has nothing to do.
ModulePtr<Function> teardownFor(Module& module, TypePtr type, Teardown half, LocationId source) {
    auto ownership = ownershipOf(module, type);
    auto kind = teardownKind(ownership, half);

    /*
     * A refined container's teardown is glue around the authored one - Implementation-Containers.md
     * §7.2.
     *
     * The traversal is the right traversal: `Reclaim(Array(a))` walks the live prefix and releases
     * the run, which is exactly what an `@inline(n)` array needs, and its release folds to nothing
     * because the run's tag says the slots are not the allocator's. What it cannot do is *read* one,
     * since it was compiled against the plain layout. So the glue is the same boundary every other
     * use of a refined array crosses - build the descriptor, then drop through it - and it exists so
     * that the boundary is crossed in one place rather than at every InstDrop that names this type.
     *
     * **The two halves have to answer with the same function wherever the plain type's do**, which
     * is what the second line below is for. Lowering runs the reclaim half only when it names
     * something other than the drop half (see the InstDrop case there), and a container's walk is one
     * traversal serving both - so two glue functions wrapping one traversal would run it twice and
     * release every element twice. That is invisible in the IR and invisible in a size assertion; it
     * shows up as a drop counter going negative, which is what the fixture reads.
     *
     * The drop half is the one that carries it, for the reason teardownPlace gives at the same
     * decision: it is the half a region does not discharge in bulk.
     */
    if(kind != TeardownKind::None && inlineRefinement(module, type)) {
        auto plain = unrefined(*module.types, type);
        auto plainDrop = teardownFor(module, plain, Teardown::Drop, source);
        auto shared = plainDrop && plainDrop == teardownFor(module, plain, Teardown::Reclaim, source);

        return teardownGlueFor(module, type, shared ? Teardown::Drop : half, source);
    }

    if(kind == TeardownKind::Authored) {
        auto typeClass = half == Teardown::Drop ? module.coreClasses.drop : module.coreClasses.reclaim;
        if(auto authored = instanceImplementation(module, typeClass, type, source)) return authored;

        /*
         * A container's one traversal, standing in for the drop half it was not written as -
         * Implementation-Containers.md §13.
         *
         * The author wrote a walk over the live elements and called it a `Reclaim`; ownershipOf
         * decided that for these element types the walk also has effects, and there is no second
         * body to run for that. So the same one runs, and lowering emits it once when both halves
         * name it - see the InstDrop case there.
         *
         * Only in this direction. A `Drop` is never elidable, so borrowing one to serve the reclaim
         * half would make it elidable in a region, which is the one thing the split exists to
         * prevent.
         */
        if(half == Teardown::Drop) {
            return instanceImplementation(module, module.coreClasses.reclaim, type, source);
        }

        return nullptr;
    }

    if(kind == TeardownKind::Derived) return teardownGlueFor(module, type, half, source);
    return nullptr;
}

/*
 * One place's teardown.
 *
 * `boxed` is the whole of what an indirect edge changes here, and it changes two things. The
 * *reclaim* half additionally hands the box back, whether or not the target had anything of its own
 * to release - which is why a boxed field gives its owner a derived `Reclaim` unconditionally (see
 * includeBoxedMember). The *drop* half is unchanged: whatever effect the target's last use runs, it
 * runs through the pointer exactly as it would have run inline.
 *
 * `place` is the target rather than the pointer, because `project` follows the box - so the address
 * an `InstDrop` computes for it is the box's own address, which is exactly what `releaseStorage`
 * hands to `freeHeap`. The two answers coincide, and that is not a coincidence: releasing the
 * storage a place occupies *is* releasing the box when the place is the box's contents.
 */
static void teardownPlace(ExprResolver& resolver, Module& module, Place place, TypePtr type,
                          bool boxed, Teardown half, LocationId source) {
    auto implementation = teardownFor(module, type, half, source);
    auto teardown = teardownKind(ownershipOf(module, type), half);
    auto releases = boxed && half == Teardown::Reclaim;

    /*
     * One traversal serving both halves, a level down - Implementation-Containers.md §13.
     *
     * Lowering already declines to run the reclaim half of an InstDrop whose two halves name one
     * function, which is what keeps `Array(Buffer)`'s single walk from freeing the run twice. That
     * check cannot see across a *derived* teardown, though: a record holding one generates `drop$X`
     * and `reclaim$X`, each with an InstDrop of its own naming that walk in its own half, and the two
     * glue functions are different functions - so the InstDrop on the record runs both and the walk
     * runs twice. A `data X {xs: [Handle]}` released every handle twice, and the fixture that found
     * it is the container refinement's, which is the first thing to put one inside a record.
     *
     * Emitted in the *drop* half and skipped in the reclaim one, which is the same choice lowering
     * makes for the flat case and for the same reason: a region discharging the reclaim half in bulk
     * leaves the drop half to run at last use, and the release inside the walk does nothing there
     * because the run's tag says the region owns the storage.
     */
    if(half == Teardown::Reclaim && implementation &&
       implementation == teardownFor(module, type, Teardown::Drop, source)) {
        implementation = nullptr;
        teardown = TeardownKind::None;
    }

    if(!implementation && !releases) return;

    auto isDrop = half == Teardown::Drop;
    auto drop = resolver.emit<InstDrop>(source, StringId(), module.scalar.unit, place,
                                        isDrop ? teardown : TeardownKind::None,
                                        isDrop ? TeardownKind::None : teardown);

    if(isDrop) drop->drop = implementation;
    else drop->reclaim = implementation;

    drop->releaseStorage = releases;
}

// The same, for a member reached by one projection off `base`.
static void teardownMember(ExprResolver& resolver, Module& module, Place base, ProjectionKind kind,
                           U16 index, TypePtr type, bool boxed, Teardown half, LocationId source) {
    teardownPlace(resolver, module, resolver.project(base, kind, index), type, boxed, half, source);
}

/*
 * Emits one InstDrop for each member of `content` that has something to do for this half, projected
 * off `base`. Shared by the tuple case and by a record constructor's payload.
 *
 * A payload is only a tuple when it was *written* as one. `Two {first: Buffer, second: Buffer}`
 * declares a Tup and has fields to project; `Held(Buffer)` declares the `Buffer` itself, and the
 * Downcast the caller already took is that member's whole place. Both spellings reach here, so the
 * non-tuple one cannot be a case this walks past - it is the ordinary shape of every `Just(a)`,
 * `Ok(a)` and `Some(H)` in the language, and skipping it leaks whatever the payload owned.
 */
static void teardownMembers(ExprResolver& resolver, Module& module, Place base, TypePtr content,
                            Teardown half, LocationId source) {
    auto global = *module.types;
    if(!content) return;

    if(global[content]->kind != Type::Tup) {
        // Never boxed: a boxed payload is handled by the caller as one member off the Downcast,
        // because the release of the box belongs to the edge rather than to what is on its far side.
        teardownPlace(resolver, module, base, content, false, half, source);
        return;
    }

    auto tuple = (TupType*)global[content];
    U16 index = 0;

    for(auto field: tuple->fields.contents(global)) {
        teardownMember(resolver, module, base, ProjectionKind::Field, index, field.type, field.boxed,
                       half, source);
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
                             LocationId source, bool headerKnown) {
    auto address = funValueFieldType(module, FunValueLayout::kEnv);
    auto word = module.scalar.long_;

    auto env = resolver.load(resolver.project(base, ProjectionKind::Field, FunValueLayout::kEnv), source);
    auto empty = resolver.constantBits(address, 0, source);

    /*
     * `headerKnown` is the caller saying the test below has one answer.
     *
     * This is interned per function *type* and therefore has to assume the worst about a value: a
     * lambda that captured nothing has no header at all, and so does the thunk that makes a plain
     * function into a function value. A *drop site* often knows better - see
     * devirtualizeClosureDrop, which proves that every lambda able to reach one particular drop has
     * a header that is emitted and non-empty - and this is the same glue with the question already
     * answered. The two are separate interned functions rather than a parameter, because which one
     * a site gets is decided per site while the body is shared by all of them.
     */
    ModulePtr<Block> exit = nullptr;

    /*
     * What the branch tests, which is not the same question on the two targets.
     *
     * Native tests the **environment**: a header always exists in front of a lifted lambda's entry
     * point, so a null environment is the only thing that says there is nothing here to release.
     *
     * A target that hangs the header on the code word tests the **header** instead, and gets a
     * strictly better answer for it. A missing header means one of two things and both are "nothing
     * to run": the value captured nothing, or it captured only things with no teardown between them
     * - and the second is a lambda whose header would have held `teardown$none` in every slot. So
     * the test subsumes the environment's *and* lets that header stop being emitted at all, which is
     * one static table and one store per lambda that no longer exist. See closureNeedsTeardown.
     *
     * It is also one property load where the environment test was a load and then a second load to
     * reach the header, so the branch that does fire is no more expensive than it was.
     */
    if(!headerKnown) {
        ModulePtr<Value> tested = env;

        if(isJsMode(module.context.settings.mode)) {
            tested = resolver.load(resolver.project(base, ProjectionKind::Field, FunValueLayout::kHeader), source);
        }

        auto present = resolver.emit<InstCmp>(source, StringId(), module.scalar.bool_, tested, empty, CompareOp::Ne);

        auto run = resolver.addBlock();
        exit = resolver.addBlock();
        resolver.terminate(resolver.emit<InstJe>(source, StringId(), module.scalar.unit, resolver.ref(present), run, exit));
        resolver.current = run;
    }

    /*
     * The header, from wherever this target keeps it.
     *
     * Native keeps it in front of the entry point, so it is found by subtracting the header's own
     * size from the code word - through the integer rather than as a place projection, because the
     * offset is negative: a projection walks *into* an aggregate, and this walks backwards out of
     * one. The two casts are both reinterpretations of one machine word - asInt and asPtr - so what
     * they cost is nothing and what they buy is that the arithmetic is stated where the layout is.
     *
     * How far back is a TypeMetric rather than a constant. This pass has no idea how wide two
     * addresses are, and a number written here would be this compiler subtracting the layout some
     * other target chose; whoever emits folds it, having just laid the header out itself.
     *
     * A target whose code word is not an address has no bytes in front of it to subtract from, and
     * attaches the header to the code word instead. That is FunValueLayout::kHeader, and asking for
     * it is a projection like any other - which is the whole of the difference, because everything
     * around it reads the same two slots out of the same layout either way.
     */
    auto headerContent = closureHeaderPlaceType(module);
    auto headerType = resolvePointerType(module, module.scalar.unit);
    ModulePtr<Value> header;

    if(isJsMode(module.context.settings.mode)) {
        header = resolver.load(
            resolver.project(base, ProjectionKind::Field, FunValueLayout::kHeader), source);
    } else {
        auto codeWord = resolver.load(resolver.project(base, ProjectionKind::Field, FunValueLayout::kCode), source);
        auto codeInt = resolver.ref(resolver.emit<InstUnary>(source, StringId(), word, Value::Cast, codeWord));
        auto distance = resolver.ref(resolver.emit<InstTypeMetric>(source, StringId(), word, headerContent,
                                                                   TypeMetricKind::Size));
        auto headerInt = resolver.ref(resolver.emit<InstBinary>(source, StringId(), word, Value::Sub, codeInt, distance));

        header = resolver.ref(resolver.emit<InstUnary>(source, StringId(), headerType, Value::Cast, headerInt));
    }

    /*
     * The slot, as a question rather than as a load.
     *
     * This used to be an ordinary field projection into a tuple laid out like the header, which was
     * only ever right because an address slot happened to be exactly a pointer wide. It is four
     * bytes and self-relative now on native and a bare array element on JS, and neither is something
     * this pass may know: it runs before a target is chosen. So it states which slot of which table
     * and lets whoever emits decode it - see InstTableSlot, and TypeMetric two lines above, which is
     * the same trade for the same reason.
     */
    auto slot = half == Teardown::Drop ? ClosureHeaderFields::kDrop : ClosureHeaderFields::kReclaim;
    auto operation = resolver.ref(resolver.emit<InstTableSlot>(
        source, StringId(), headerType, header, slot));

    // No signature: this is the compiler calling a teardown it generated, not a program calling a
    // function value, so there are no conventions to honour and no environment convention either.
    auto teardown = resolver.create<InstCallDyn>(source, StringId(), module.scalar.unit, nullptr, operation, nullptr);
    teardown->args.push(module.arena, env);
    resolver.append(teardown);

    // Nothing to rejoin where there was no branch: the call is the whole body and the caller's
    // `ret` follows it.
    if(exit) {
        resolver.terminate(resolver.emit<InstJmp>(source, StringId(), module.scalar.unit, exit));
        resolver.current = exit;
    }
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
static ModulePtr<Function> teardownGlueFor(Module& module, TypePtr type, Teardown half, LocationId source,
                                           bool headerKnown) {
    auto& program = module.program;
    auto& interned = half == Teardown::Drop
        ? (headerKnown ? program.dropGlueKnown : program.dropGlue)
        : (headerKnown ? program.reclaimGlueKnown : program.reclaimGlue);

    if(auto found = interned.get(U32(type))) return found.unwrap();

    // addAnonymousFunction already registers it in the module's function order, which is what puts
    // it in front of printing and lowering.
    auto function = addAnonymousFunction(module, teardownGlueName(module, type, half, headerKnown), source);
    auto pointer = function - *module.arena;

    // Registered before the body is built, so a type reachable from itself finds the entry rather
    // than generating glue forever.
    *interned.add(U32(type)).value = pointer;

    function->returnType = module.scalar.unit;
    function->used = true;

    /*
     * The subject, taken by `->` rather than as a `%T`.
     *
     * A teardown *consumes*, so `->` is the convention that states what is true - and both halves
     * of Design-Memory §4 are statements about a value whose lifetime ends here. What it replaces
     * is a storage handle, which is the *erased* ABI's form and was being used at every site
     * whether or not the site was erased. Natively that is the same address either way, because a
     * memory type's `->` parameter is passed by address; on a managed target a `%T` whose pointee is
     * not a host object is a **box**, so every concrete drop of one allocated an object to be read
     * once and thrown away. Thirty-seven of the JS corpus's remaining boxes were this.
     *
     * `disposer`, because a `->` parameter is storage this frame owns, and without it the drop pass
     * gives this function a drop of the value it exists to drop. See Function::disposer for what
     * that failure looks like, which is not a diagnostic.
     *
     * The erased entry point is unaffected and is not this: a descriptor slot has one signature for
     * every type it might hold and still takes an address. See teardownEntryFor.
     */
    auto valueName = module.context.addQualifiedName("value", 5, 1);
    auto arg = function->addArg(module, valueName, type, source);
    arg->convention = ast::BindType::Sink;
    function->disposer = true;

    auto subject = function->addLocal(module, type, valueName,
                                      (ModulePtr<Value>)(arg - *module.arena), ast::BindType::Sink);

    ExprResolver resolver(module.context, module, *function);
    auto base = Place::inLocal(subject);
    auto global = *module.types;

    if(global[type]->kind == Type::Fun) {
        teardownFunValue(resolver, module, base, half, source, headerKnown);
    } else if(global[type]->kind == Type::Array) {
        /*
         * `[T *n]` - Implementation-Containers.md §6's "derived teardown over exactly `n` members".
         *
         * Exactly `n` and not "however many are live", which is the whole of what separates this
         * from a growable container: a fixed array has no count, so every slot holds a value and
         * there is no prefix to stop at. That is why this is derived glue at all rather than an
         * authored traversal - `Reclaim(Array(a))` exists because occupancy is Collections' private
         * business, and here there is no occupancy to have an opinion about.
         */
        auto array = (ArrayType*)global[type];

        resolver.eachFixedElement(base, array->content, array->count, source,
                                  [&](Place element, ModulePtr<Value>) {
            teardownPlace(resolver, module, element, array->content, false, half, source);
        });
    } else if(global[type]->kind == Type::Tup) {
        teardownMembers(resolver, module, base, type, half, source);
    } else if(inlineRefinement(module, type)) {
        /*
         * `@inline(n) @capacity(n) [T]` - Implementation-Containers.md §7.2.
         *
         * One descriptor and one InstDrop through it. What runs is Collections' own traversal over
         * the live prefix, reached at the plain type, which is what keeps "a Repr variant may never
         * change what a type can do" true of the teardown as well as of the interface.
         *
         * The descriptor is a view rather than a copy - `borrowed`, so this frame does not also
         * release what it names - and the plain `Reclaim` it reaches is the one that owns the walk.
         * Its `releaseRun` reads `runFixed` and hands nothing back, which is the correct answer and
         * which folds away wherever the tag survives to the optimizer as the constant it is.
         */
        if(auto descriptor = resolver.inlineArrayDescriptor(base, type, source, false)) {
            teardownPlace(resolver, module, resolver.placeFor(descriptor, source),
                          unrefined(global, type), false, half, source);
        }
    } else if(global[type]->kind == Type::Record) {
        auto record = (RecordType*)global[type];

        if(record->layout == RecordType::Single) {
            auto constructor = record->constructors.get(global, 0);

            // A boxed payload is one member rather than a list of them: what has to happen is the
            // target's own teardown and then the release of the box, and both are one InstDrop on
            // the place the Downcast reaches through the pointer.
            if(constructor.boxed) {
                teardownMember(resolver, module, base, ProjectionKind::Downcast, 0,
                               constructor.content, true, half, source);
            } else {
                teardownMembers(resolver, module,
                                resolver.project(base, ProjectionKind::Downcast, 0),
                                constructor.content, half, source);
            }
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
                if(!content) continue;

                // A boxed payload always contributes to the reclaim half even where its target has
                // nothing to release, because the box itself has to be handed back.
                auto releases = constructor.boxed && half == Teardown::Reclaim;
                if(!releases && !contributes(module, content, half)) continue;

                auto discriminant = resolver.load(
                    resolver.project(base, ProjectionKind::Discriminant, 0), source);

                auto index = resolver.makeInt(source, module.scalar.int_, constructor.index);
                auto matches = resolver.emit<InstCmp>(source, StringId(), module.scalar.bool_,
                                                      discriminant, index, CompareOp::Eq);

                auto drops = resolver.addBlock();
                auto next = resolver.addBlock();
                resolver.terminate(resolver.emit<InstJe>(source, StringId(), module.scalar.unit,
                                                         resolver.ref(matches), drops, next));

                resolver.current = drops;

                if(constructor.boxed) {
                    teardownMember(resolver, module, base, ProjectionKind::Downcast,
                                   U16(constructor.index), content, true, half, source);
                } else {
                    teardownMembers(resolver, module,
                                    resolver.project(base, ProjectionKind::Downcast,
                                                     U16(constructor.index)),
                                    content, half, source);
                }
                resolver.terminate(resolver.emit<InstJmp>(source, StringId(), module.scalar.unit, exit));

                resolver.current = next;
            }

            resolver.terminate(resolver.emit<InstJmp>(source, StringId(), module.scalar.unit, exit));
            resolver.current = exit;
        }
    }

    resolver.terminate(resolver.emit<InstRet>(source, StringId(), module.scalar.unit, nullptr));
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

        // A run's placement switch, which is storage release written once instead of copied into
        // every container built on one - see Program::releaseRun. A specialization of it stands in
        // for it, on the same terms as a specialized teardown below.
        if(callee == program.releaseRun) return true;
        if(local[callee]->specializationOf == program.releaseRun) return true;

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
        for(auto instruction: local[blockPointer]->instructions(local)) {
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

/*
 * The erased entry point of one type's teardown half - what a descriptor slot holds.
 *
 * A teardown takes its subject by `->`, and both halves of that convention are decided by the
 * *target*: natively a memory type's `->` parameter is an address, and on a managed target it is
 * the host value. A descriptor slot cannot be either, because erased code has neither - it holds
 * storage, and the slot has one signature for every type that might fill it. So the slot holds a
 * `%T` entry, and the entry is a single `InstDrop` through the address it was handed.
 *
 * This is `erasedThunkFor`'s argument (witness.cpp) applied to the one uniform-ABI slot that did not
 * have one. Before, a teardown reached both ways happened to agree with both call sites - a `%T`
 * glue read through a pointer and a JS reference to an *object* is that object, so the two coincided
 * for every shape the corpus had. They stop coinciding as soon as a type is not a host object, which
 * is what made every concrete drop of a niche-folded value allocate a box.
 *
 * Generated only where a descriptor asks for one, so a program with no erased generics emits none.
 */
ModulePtr<Function> teardownEntry(Module& module, TypePtr type, Teardown half, LocationId source) {
    auto implementation = teardownFor(module, type, half, source);
    if(!implementation) return nullptr;

    /*
     * Natively there is nothing to adapt, so the entry *is* the implementation.
     *
     * A teardown's subject is always a memory type, and a memory type's `->` parameter is passed as
     * its address - which is what a slot hands over. The two spellings name one convention, and
     * generating a forwarder for them would put a real call in front of every erased teardown to
     * change nothing.
     *
     * Asked of the mode rather than of a layout, on the same terms as teardownFunValue above: where
     * the header lives is the same kind of question and is asked the same way. Over-generating an
     * entry is only a wasted call and under-generating one is a miscompile, so the JS side keeps
     * its entry unconditionally - the case it exists for is a subject that is not a host object,
     * and which types those are is a Repr answer this pass deliberately cannot see.
     */
    if(!isJsMode(module.context.settings.mode)) return implementation;

    auto& program = module.program;
    auto& interned = half == Teardown::Drop ? program.dropEntry : program.reclaimEntry;
    if(auto found = interned.get(U32(type))) return found.unwrap();

    auto name = derivedName(module, half == Teardown::Drop ? "dropAt$"_v : "reclaimAt$"_v, type);
    auto function = addAnonymousFunction(module, name, source);
    auto pointer = function - *module.arena;

    // Registered before the body, on the same terms as the glue above: the InstDrop below can reach
    // a type whose own entry is being built.
    *interned.add(U32(type)).value = pointer;

    function->returnType = module.scalar.unit;
    function->used = true;

    auto valueName = module.context.addQualifiedName("value", 5, 1);
    auto arg = function->addArg(module, valueName, resolvePointerType(module, type), source);

    ExprResolver resolver(module.context, module, *function);
    teardownPlace(resolver, module, Place::atPointer((ModulePtr<Value>)(arg - *module.arena)),
                  type, false, half, source);
    resolver.terminate(resolver.emit<InstRet>(source, StringId(), module.scalar.unit, nullptr));

    return pointer;
}

/*
 * The drop-site form of the glue above: the same body with the header test left out.
 *
 * Only for a function type, and only for a caller that has proved what the test would have found -
 * see devirtualizeClosureDrop, which is the one caller. Interned per type beside the conditional
 * one, so a program in which no site can prove it never generates one.
 */
ModulePtr<Function> funTeardownKnownHeader(Module& module, TypePtr type, Teardown half, LocationId source) {
    assertTrue((*module.types)[type]->kind == Type::Fun);
    return teardownGlueFor(module, type, half, source, true);
}
