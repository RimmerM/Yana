#include "build.h"
#include "../../opt/opt.h"

/*
 * resolve IR -> JavaScript.
 *
 * This file is the program: which functions and globals exist at all, what they are called, and the
 * order they come out in. The four representation decisions those rest on, all from §2.3 and §3.2:
 *
 *  - a record or tuple is one object built by one object literal with every property present, so
 *    each type has one hidden class;
 *  - a sum type is that object plus a `$tag` property, with every constructor's payload *flattened*
 *    into the same object - which is what makes a Downcast free, exactly as it is on native where
 *    the payload is inline. A constructor whose content is not a tuple keeps it in `$p`;
 *  - a function value is the two words FunValueLayout describes, exactly as it is on native: a code
 *    word and an environment, here as the two properties `{$c, $e}`. The code word is an ordinary
 *    top-level declaration taking the environment as parameter zero, so calling one is
 *    `f.$c(f.$e, ...)` and there is no factory anywhere. A lambda that captured nothing, and a plain
 *    function used as a value, hold `null` in `$e` and ignore the parameter, which is what keeps one
 *    call shape for all three;
 *  - a compiler-built constant table is an array, one element per slot, so that a load the native
 *    side writes as `[base + 24]` is `table[5]` here. Resolve describes those tables as slots and
 *    nothing else, so this is a materialization rather than a reinterpretation - which is what it
 *    used to be, when the cells were the 32-bit words of a native memory image and every second one
 *    was the empty high half of an address.
 *
 * What has no representation on JS is an address of something that is not an object. A borrow of a
 * `Int` local is the case that matters, since `&counter: Int` is ordinary Yana. A borrow whose every
 * use is the root of a place is a second *name* for the storage and costs nothing; one that is
 * handed over needs somewhere to point, so the local it names is *stored* as a one-property box -
 * see prepareLocals, which is §2.3's value-in/value-out lowering for the half that does not cross a
 * call.
 *
 * The rest of the target is in build.h's neighbours: name.cpp, type.cpp, place.cpp, inst.cpp and
 * flow.cpp, in that order of altitude, and opt.cpp over the finished tree.
 */

namespace js {

namespace {

/*
 * Selection - what this target can have at all.
 */

bool nativeModule(Gen& g, Module* module) {
    auto text = stringView(g.context.findName(module->name));
    return text == "Native"_v || text.startsWith("Native."_v);
}

/*
 * Functions this target has no way to emit.
 *
 * Rather than a list of names, the test is what the body contains: a syscall, a block memory
 * operation, arithmetic that produces an address, or a conversion between an address and an
 * integer. All four are Design.md's `Native`, which is native-only by construction - there is no
 * `ArrayBuffer` behind a JS object to take the address of, and §5.1 is the argument for not
 * inventing one.
 *
 * The exclusion is then closed under calls: a function that calls one it cannot have is one it
 * cannot have either. That is what removes the heap allocator, the free lists and the reclaim glue
 * built on them without naming any of them here - which matters because the reclaim half is
 * supposed to compile to nothing on this target anyway (Design-Memory §4).
 */
bool expressibleInJs(Gen& g, Function& function) {
    auto expressible = true;

    eachInstruction(g, function, [&](Value& instruction) {
        switch(instruction.kind) {
            case Value::Native:
                // A host operation is *only* expressible here - Implementation-Containers.md §14.1.
                // It reaches this target and no other, which is what `@platform(js)` on every
                // declaration that produces one already guarantees.
                if(isHostOp(((InstNative&)instruction).op)) break;

                // A whole-value block copy is a shape this target can express - see
                // blockCopyShape. A syscall, a fill, or a copy of anything else is not.
                if(!blockCopyShape(g, (InstNative&)instruction)) expressible = false;
                break;
            case Value::Cast: {
                auto& source = *g.local[((InstUnary&)instruction).from];
                auto fromPointer = isPointer(g.global, source.type);
                auto toPointer = isPointer(g.global, instruction.type);

                // A *constant* address is the exception, and the only one that has a meaning here:
                // the IR has no pointer immediate, so `null()` is the integer zero reinterpreted.
                if(toPointer && !fromPointer && source.kind == Value::ConstInt) break;

                /*
                 * And a `[T *n]` becoming a `%T`, which is the one reinterpretation that moves
                 * nothing on this target for a reason the *target* supplies rather than the IR:
                 * a fixed array is a host array here (see zeroValue) and so is a run of elements,
                 * so the two are the same value under two names. It is what lets a `[T *n]` be
                 * borrowed as a `[T]` - Implementation-Containers.md §6's "not done here: the JS
                 * half" - and it is emitted only by convertSliceJs.
                 */
                if(toPointer && source.type && g.global[source.type]->kind == Type::Array) break;

                if(fromPointer != toPointer) expressible = false;
                break;
            }
            case Value::Add:
            case Value::Sub:
            case Value::Mul:
                if(isPointer(g.global, instruction.type)) expressible = false;
                break;
            default:
                break;
        }
    });

    return expressible;
}

// The function a call names directly, where it names one. A deferred class dispatch does not: what
// it reaches is chosen by a witness at run time, and every candidate is included or excluded on its
// own terms.
ModulePtr<Function> directCallee(Value& instruction) {
    switch(instruction.kind) {
        case Value::Call:
            return ((InstCall&)instruction).callee;
        case Value::Symbol:
            return ((InstSymbol&)instruction).callee;
        case Value::GenCall: {
            auto& call = (InstGenCall&)instruction;
            if(!call.typeClass) return call.callee;
            break;
        }
        default:
            break;
    }

    return nullptr;
}

// Whether this function has a body of its own in the emitted file, before exclusion is considered.
bool hasBody(Gen& g, Module* module, ModulePtr<Function> pointer) {
    auto function = g.local[pointer];

    if(function->signature) return false;

    // A generic function has a body of its own only where something took the erased path to it.
    if(function->gen && !function->genericallyUsed) return false;
    // Not `!module->root && !used`, which is what this was. A named declaration of the root module
    // is a root of the reachability walk and is therefore always `used`; what the root check
    // additionally exempted was every compiler-built function generated into that module, which is
    // where a program's own glue is built. See markProgramReachable.
    if(!function->used) return false;

    return true;
}

bool emitFunction(Gen& g, Module* module, ModulePtr<Function> pointer) {
    return hasBody(g, module, pointer) && !g.excluded.contains(U32(pointer));
}

/*
 * Whether a closure of this lambda has to carry its own teardown metadata.
 *
 * The header's two slots are the environment's `Drop` and its `Reclaim`, and the second is nothing
 * here - the host collector owns reclamation, which is Design-Memory §4's carve-out for this target.
 * So the question is only whether the captures have an authored `Drop` between them, and where they
 * do not there is nothing for a teardown to reach and nothing to attach.
 *
 * Decidable from the lambda alone, which is what makes it safe to act on: the environment type is
 * this lambda's own, and no part of the program can change the answer. What makes it *sound* is that
 * the shared glue tests the header rather than the environment - see teardownFunValue - so a value
 * whose lambda skipped one is a branch that does not fire rather than a property read of `undefined`.
 */
bool closureNeedsTeardown(Gen& g, Function& function) {
    if(!function.closureHeader || function.args.isEmpty()) return false;

    // Nothing can find it - see markClosureHeaders. Asked first because it is the answer that does
    // not depend on the environment's type: a closure the compiler tore down by name has a dead
    // header whatever was in it.
    if(!function.closureHeaderRead) return false;

    auto envType = pointeeType(g.global, g.local[function.args.get(g.local, 0)]->type);
    if(!envType) return false;

    return ownershipOf(*function.module, envType).drop != TeardownKind::None;
}

/*
 * A global this target emits.
 *
 * `Native`'s own storage is the heap allocator's bookkeeping, and the functions that read it are not
 * emitted either.
 *
 * A closure header is emitted like any other table. On native it is `prefixOf` - bytes placed
 * immediately in front of a lifted function, reached by subtracting from the code address - and here
 * it is an ordinary module-level `const` assigned to the code word as `$h`. Same two slots, same
 * contents, and the only difference is how a teardown gets to it.
 *
 * Except where the lambda it belongs to does not get one: a header whose slots are all
 * `teardown$none` is not attached (see closureNeedsTeardown), and emitting the table anyway would
 * leave a `const` in every file that nothing names. Native cannot make that choice - the bytes are
 * *placed* in front of the entry point, so they exist whether or not anything reads them - which is
 * why this is asked here rather than of the global.
 */
bool emitGlobal(Gen& g, Module* module, ModulePtr<Global> pointer) {
    auto global_ = g.local[pointer];

    if(auto lambda = global_->prefixOf) {
        if(!closureNeedsTeardown(g, *g.local[lambda])) return false;
    }

    // Not `!module->root && ...`: a declared global of the root module is always `used`, and what
    // the root check additionally exempted was every compiler-built table generated into it. A
    // header is the exception that keeps `prefixOf` here - it belongs to a function rather than to
    // the module, and the line above is what decides it. See markProgramReachable.
    if(!global_->used && !global_->prefixOf) return false;
    return !nativeModule(g, module);
}

void excludeFunctions(Gen& g) {
    Array<ModulePtr<Function>> candidates;

    for(auto module: g.program.modules) {
        auto native = nativeModule(g, module);

        for(auto pointer: module->functionOrder.contents(g.local)) {
            if(!hasBody(g, module, pointer)) continue;

            if(native || !expressibleInJs(g, *g.local[pointer])) {
                g.excluded.add(U32(pointer));
                continue;
            }

            candidates.push(pointer);
        }
    }

    // Closed under calls: a function that reaches one it cannot have cannot be emitted either.
    auto changed = true;
    while(changed) {
        changed = false;

        for(auto pointer: candidates) {
            if(g.excluded.contains(U32(pointer))) continue;

            auto reaches = false;
            eachInstruction(g, *g.local[pointer], [&](Value& instruction) {
                auto callee = directCallee(instruction);
                if(callee && g.excluded.contains(U32(callee))) reaches = true;
            });

            if(reaches) {
                g.excluded.add(U32(pointer));
                changed = true;
            }
        }
    }
}

/*
 * Functions.
 */

/*
 * Whether one use of a borrow is only reaching the storage it names.
 *
 * A borrow that never becomes anything but the root of a place is a second *name* for that place,
 * because that is all this backend does with it: the place walk reads or writes the variable the
 * root stands for. A borrow that goes anywhere else - into a call, into a value, into a phi -
 * outlives the expression it appears in, and a name is not something that can be handed over.
 *
 * Asked of `Value::uses`, which the IR maintains centrally, rather than of an operand list written
 * out here. An instruction added to the IR is a use this sees whether or not anybody remembered
 * this file, and the default answer is the conservative one.
 */
bool borrowStaysHere(Gen& g, Value& user, ModulePtr<Value> borrow) {
    switch(user.kind) {
        case Value::LoadPlace:
        case Value::Borrow:
        case Value::Address:
        case Value::Move:
        case Value::Swap:
            break;
        case Value::Init:
        case Value::Assign:
            if(((InstInit&)user).value == borrow) return false;
            break;
        case Value::Exchange:
            if(((InstExchange&)user).value == borrow) return false;
            break;
        case Value::Copy:
            // An authored `Copy` is handed a reference, and referenceTo() makes a temporary box for
            // a place that is not one - which a callee reading through it is fine with and a name
            // is not what it receives.
            if(((InstCopy&)user).copy) return false;
            break;
        default:
            return false;
    }

    // The kinds above name places, but naming one is not the same as being rooted in this borrow:
    // an index projection is an ordinary operand, and a place rooted elsewhere is not this use.
    auto rooted = false;
    eachPlace(user, [&](const Place& place) {
        if(place.root != PlaceRoot::Pointer && place.root != PlaceRoot::Borrow) return;
        if(place.pointer == borrow) rooted = true;
    });

    return rooted;
}

/*
 * Locals, and which of them have to be boxed.
 *
 * A reference to something that is not a host object has nowhere to point, so a local whose borrow
 * has to survive being handed over and whose type is a scalar is *stored* as a one-property object.
 * Deciding it per local rather than per borrow is what keeps the borrow itself free: the box
 * already exists, so `InstBorrow` is still "hand over the reference and emit nothing".
 *
 * What decides it is whether the borrow ever leaves the expression it was taken in. §2.3's
 * value-in/value-out lowering is the observation that most of them do not: `counter = counter + 1`
 * through a `&` binding is a read and a write of one local, and a box would be storage invented for
 * a reference nobody ever holds. So a borrow whose every use is the root of a place is recorded as
 * an *alias* - the emitted name of the storage itself - and only a borrow that is passed on, stored
 * or returned forces the box.
 *
 * The aliases are collected here rather than in genBorrow because the decision is not local to one
 * instruction: a re-borrow of an alias is an alias too, and a use of *that* one which escapes has
 * to reach back and box the local both of them name.
 */
void prepareLocals(Gen& g, Function& function) {
    g.aliasBorrows.reset();
    g.boxed.reset(function.localCount());

    // Every value that is a name for one local's storage, and which local that is.
    HashMap<U32, U32> aliases;
    ValueList pending;

    auto consider = [&](ModulePtr<Value> value, const Place& place) {
        auto projections = place.projections;
        if(place.root != PlaceRoot::Local || projections.isNotEmpty()) return;
        if(place.local >= g.boxed.size()) return;

        auto type = function.localAt(g.local, place.local).type;
        if(isJsObject(g, type)) return;

        if(!aliases.add(U32(value), place.local)) pending.push(value);
    };

    /*
     * A local that is not an object, any part of which is borrowed, needs the box - whether the
     * borrow names the whole of it or one bit of it.
     *
     * `consider` above is about *aliasing*, which only a borrow of the whole local can be. This is
     * the other requirement, and scalarization is what introduced it: `&f.optionA` where `f` is a
     * scalarized record has to name something writable, and `f` is a `var` holding a number. The box
     * is what the reference names, with the field's shift saying where in it the bit is.
     */
    auto boxIfNarrowRoot = [&](const Place& place) {
        if(place.root != PlaceRoot::Local || place.local >= g.boxed.size()) return;
        if(place.local >= function.localCount()) return;

        auto type = function.localAt(g.local, place.local).type;
        if(type && !isJsObject(g, type)) g.boxed.set(place.local, true);
    };

    eachInstruction(g, function, [&](Value& instruction) {
        auto pointer = (ModulePtr<Value>)((Inst*)&instruction - g.local);

        if(instruction.kind == Value::Borrow) {
            consider(pointer, ((InstBorrow&)instruction).place);
            boxIfNarrowRoot(((InstBorrow&)instruction).place);
        } else if(instruction.kind == Value::Address) {
            consider(pointer, ((InstAddress&)instruction).place);
            boxIfNarrowRoot(((InstAddress&)instruction).place);
        }
    });

    while(pending.isNotEmpty()) {
        auto borrow = pending[pending.size() - 1];
        pending.pop();

        auto found = aliases.get(U32(borrow));
        if(!found) continue;
        auto local = found.unwrap();

        for(auto userPointer: g.local[borrow]->uses.contents(g.local)) {
            auto& user = *g.local[userPointer];

            if(!borrowStaysHere(g, user, borrow)) {
                g.boxed.set(local, true);
                continue;
            }

            // A borrow of a borrow is the same storage under another name, so whatever that one
            // turns out to be used for is this local's answer as well.
            if(user.kind != Value::Borrow && user.kind != Value::Address) continue;

            auto result = (ModulePtr<Value>)userPointer;
            if(!aliases.add(U32(result), local)) pending.push(result);
        }
    }

    /*
     * A `&` parameter names storage the caller owns, so the box was made there and arrives as the
     * argument. Nothing here allocates it and nothing here writes it back.
     */
    for(Size i = 0; i < function.localCount(); i++) {
        auto slot = function.localAt(g.local, i);
        if(!slot.borrowed || !slot.type) continue;
        if(!isJsObject(g, slot.type)) g.boxed.set(i, true);
    }

    /*
     * A local that is not an object and is borrowed at all needs the box, even where the borrow never
     * leaves this function.
     *
     * Aliasing - the borrow being a second *name* for the storage - is what makes a borrow free here,
     * and it works because a reference to an object is the object. A scalarized record is a `number`,
     * so there is no second name to be had: a reference to one has to name something that can be
     * written through, which is the box. Without this a `&` of such a local silently becomes a copy.
     */
    for(auto entry: aliases.entries()) {
        auto slot = function.localAt(g.local, entry.value);
        if(slot.type && !isJsObject(g, slot.type)) g.boxed.set(entry.value, true);
    }

    for(auto entry: aliases.entries()) {
        if(!g.boxed[entry.value]) g.aliasBorrows.add(entry.key);
    }
}

/*
 * Which function-value locals are carried as their two words rather than as an object.
 *
 * The default is flat and the rule is what takes it away, which is the opposite of how the boxing
 * above reads and deliberately so: every use a function value has *wants* the two words - the call
 * enters with them, the teardown runs off them, a flattened argument passes them - and the uses
 * that want one value are the exceptions, which `useValue` builds an object for where it finds one.
 *
 * Two disqualifications, and each is about something that needs the storage to be an object:
 *
 *  - **a borrow or an address of it.** An object is its own reference here, which is what makes
 *    `InstBorrow` free (§3.3) - and two variables are not an object. There is nothing for the
 *    reference to name, and boxing the pair instead would be the copy-with-write-back that
 *    refIsTriple exists to avoid.
 *  - **a projection that is not one of the two words.** A `Fun` is a leaf, so in a well-formed body
 *    there is no such projection; declining on one is what keeps a place the walk cannot answer
 *    from becoming a silently wrong answer rather than an obvious one.
 *
 * A `&` parameter is excluded by the first: the storage is the caller's and arrives as a reference,
 * so this frame has no words to hold. A parameter *declared* as a function value is the opposite
 * case and is registered in genFunction, where the two words are two parameters.
 */
void prepareFunLocals(Gen& g, Function& function) {
    g.flatFuns.reset(function.localCount());

    for(Size i = 0; i < function.localCount(); i++) {
        auto slot = function.localAt(g.local, i);
        if(!slot.type || g.global[slot.type]->kind != Type::Fun) continue;
        if(slot.borrowed) continue;

        g.flatFuns.set(i, true);
    }

    eachInstruction(g, function, [&](Value& instruction) {
        auto decline = [&](const Place& place) {
            if(place.root != PlaceRoot::Local || place.local >= g.flatFuns.size()) return;
            g.flatFuns.set(place.local, false);
        };

        if(instruction.kind == Value::Borrow) {
            decline(((InstBorrow&)instruction).place);
            return;
        }

        if(instruction.kind == Value::Address) {
            decline(((InstAddress&)instruction).place);
            return;
        }

        eachPlace(instruction, [&](const Place& place) {
            if(place.root != PlaceRoot::Local || place.local >= g.flatFuns.size()) return;
            if(!g.flatFuns[place.local]) return;

            auto projections = place.projections;
            if(projections.isEmpty()) return;

            if(projections.size() > 1) {
                g.flatFuns.set(place.local, false);
                return;
            }

            auto projection = projections.get(g.local, 0);
            if(projection.kind != ProjectionKind::Field ||
               projection.index >= FunValueLayout::kProjectionCount) {
                g.flatFuns.set(place.local, false);
            }
        });
    });
}

/*
 * The locals whose whole value one `InstAggregate` writes - see Gen::builtWhole.
 *
 * Two conditions. `wholeLocalPlan` must call the aggregate eligible, which is the whole of the
 * shape question and is asked there so that the emitter and this cannot disagree; and it must be the
 * local's **only** aggregate, because two of them are two complete values and only the first would
 * be a declaration's initializer.
 *
 * Nothing else about the local is asked, and nothing else needs to be. A later store into a field is
 * an ordinary write of a property the literal already created, and a read before the aggregate is
 * not something the IR can contain - the instruction is what initializes the storage, so a body that
 * read it first would be reading storage the ownership passes had not seen initialized.
 */
void prepareBuiltLocals(Gen& g, Function& function) {
    g.builtWhole.reset(function.localCount());

    IndexSet seen;
    seen.reset(function.localCount());

    eachInstruction(g, function, [&](Value& instruction) {
        if(instruction.kind != Value::Aggregate) return;

        auto& aggregate = (InstAggregate&)instruction;
        auto& place = aggregate.place;
        if(place.root != PlaceRoot::Local || place.local >= g.builtWhole.size()) return;

        if(!wholeLocalPlan(g, aggregate).eligible) return;

        g.builtWhole.set(place.local, !seen[place.local]);
        seen.set(place.local, true);
    });
}

// The body, once the parameters have been named and bound. Split out because a capturing lambda's
// goes inside a function *expression* and every other function's goes inside a declaration, and
// nothing else about building one differs.
StmtList genBody(Gen& g, Function& function) {
    prepareLocals(g, function);
    prepareFunLocals(g, function);
    prepareBuiltLocals(g, function);
    prepareCfg(g, function);

    return collect(g, [&] {
        /*
         * A flattened reference parameter that some use needs as one value, reassembled once at the
         * top rather than at each use. The parts stay registered alongside it, so a call site still
         * passes them flat and only the use that genuinely needs an object reads this.
         */
        for(auto argPointer: function.args.contents(g.local)) {
            auto value = (ModulePtr<Value>)argPointer;
            auto found = g.flatRefs.get(U32(value));
            if(!found || !narrowRefNeedsObject(g, value)) continue;

            auto& arg = *g.local[argPointer];
            g.values.add(U32(value), declare(g, valueName(g, arg), materializeRef(g, found.unwrap())));
        }

        /*
         * Phi results are declared before anything that assigns them, because a phi is written by
         * each predecessor and read at the join - which is two different places in the emitted
         * structure and one value in the IR.
         */
        for(auto blockPointer: function.blocks.contents(g.local)) {
            for(auto phiPointer: g.local[blockPointer]->phis.contents(g.local)) {
                auto& phi = *g.local[phiPointer];
                auto name = valueName(g, phi);

                g.phis.add(U32(phiPointer), name);
                g.values.add(U32((ModulePtr<Value>)phiPointer), variable(g, name));
                emit(g, make<DeclStmt>(g, name, JsPtr<Expr>(nullptr), false));
            }
        }

        if(function.blocks.isNotEmpty()) emitChain(g, 0, kNoBlock);
    });
}


/*
 * One function, and there is now only one form of one.
 *
 * A *code word* - a lifted lambda, or the thunk that makes a named function a function value - is an
 * ordinary top-level declaration taking the environment as parameter zero, which is what
 * `Function::takesEnv` has meant on every other target all along. There is no factory and no
 * function expression: a function value is the `{$c, $e}` pair, so the thing that varies per closure
 * is the environment word of the value rather than the identity of the code.
 *
 * That is what removed the shape this used to have. The factory existed because the environment had
 * to be a parameter of *something* - `var` is function-scoped, so a closure built in a loop would
 * otherwise have seen the last iteration's - and with the environment travelling in the value there
 * is nothing to bind and nothing to build.
 *
 * The captures still go through the environment object rather than becoming parameters. That object
 * is storage the ownership model tracks - it is the local whose drop a closure's teardown
 * devirtualizes to, and the one the header's reclaim names - so dissolving it is a decision for the
 * resolver rather than a rewrite here.
 */
void genFunction(Gen& g, ModulePtr<Function> pointer) {
    auto& function = *g.local[pointer];

    g.function = &function;
    g.functionPointer = pointer;
    // Emptied rather than released: one Gen serves a whole module, so each of these settles at the
    // widest function it has seen - see HashMap::reset.
    g.values.reset();
    g.phis.reset();
    g.localNames.reset();
    g.funParts.reset();
    g.labelCounter = 0;
    g.genEnv = nullptr;
    g.genContext = functionGen(g.global, function);
    g.genModule = function.module;

    auto found = g.functionNames.get(U32(pointer));
    auto result = make<FunStmt>(g, found ? found.unwrap() : Name {});

    // The environment comes first, on the same terms as native: every unspecialized generic
    // function receives it, whatever its signature says.
    if(g.genContext) {
        auto name = uniqueName(g, "genEnv"_v, true);
        result->args.push(g.file.arena, name);
        g.genEnv = variable(g, name);
    }

    U16 index = 0;

    // The same answer every caller computes from the same declarations - see functionFlattensArgs.
    auto flattensArgs = functionFlattensArgs(g, function);

    for(auto argPointer: function.args.contents(g.local)) {
        auto arg = g.local[argPointer];
        auto name = valueName(g, *arg);

        if(function.takesEnv && index == 0) {
            // The environment, received by every code word alike. One that captured nothing is
            // handed `null` and never reads it, which is what keeps `f.$c(f.$e, ...)` one shape.
            result->args.push(g.file.arena, name);
            g.values.add(U32((ModulePtr<Value>)argPointer), variable(g, name));
            index++;
            continue;
        }

        // A parameter that carries nothing is not received - see declaredArgIsAbsent. Skipped
        // before the name is bound, so that a body naming it fails loudly rather than reading a
        // parameter the caller never passed.
        if(declaredArgIsAbsent(g, arg->type, arg->convention)) {
            index++;
            continue;
        }

        auto& into = result->args;

        /*
         * A narrow reference arrives as its three parts rather than as an object - see
         * refIsFlattened. There is nothing to allocate and nothing to project: `flip(o, k, s)` reads
         * `(o[k] >>> s) & 1` directly, where the descriptor form read `(r.$o[r.$k] >>> r.$s) & 1`
         * and the caller had to build `r` first.
         *
         * The parts are named after the parameter so that the emitted source still says which
         * reference they belong to, and the whole triple is dropped if the body never touches it.
         */
        if(flattensArgs && refIsFlattened(g, arg->type, arg->convention)) {
            auto owner = partName(g, *arg, "$o"_v);
            auto key = partName(g, *arg, "$k"_v);

            into.push(g.file.arena, owner);
            into.push(g.file.arena, key);

            RefParts parts;
            parts.owner = variable(g, owner);
            parts.key = variable(g, key);

            // A reference to a function value carries a second key where a narrow one carries a
            // shift - see RefParts::envKey. Both sides count the arity from the declaration, so
            // this has to be the same split flatRefArity makes.
            if(isFunValue(g, arg->type)) {
                auto envKey = partName(g, *arg, "$ke"_v);
                into.push(g.file.arena, envKey);
                parts.envKey = variable(g, envKey);
            } else if(narrowRefCarriesScale(g)) {
                auto scale = partName(g, *arg, "$s"_v);
                into.push(g.file.arena, scale);
                parts.scale = variable(g, scale);
            }

            g.flatRefs.add(U32((ModulePtr<Value>)argPointer), parts);
            index++;
            continue;
        }

        /*
         * A function value arrives as its two words, on the same terms and for the same reason -
         * see funIsFlattened. `apply(f$c, f$e, x)` enters the callee with the pair already taken
         * apart, so the callee's own call of it is `f$c(f$e, x)` and no object is built on either
         * side of the handoff. That is the pattern the language produces most of, and it is the one
         * the whole representation was chosen for.
         */
        if(flattensArgs && funIsFlattened(g, arg->type, arg->convention)) {
            auto code = partName(g, *arg, "$c"_v);
            auto env = partName(g, *arg, "$e"_v);

            into.push(g.file.arena, code);
            into.push(g.file.arena, env);

            FunParts parts;
            parts.code = variable(g, code);
            parts.env = variable(g, env);

            g.funParts.add(U32((ModulePtr<Value>)argPointer), parts);
            index++;
            continue;
        }

        into.push(g.file.arena, name);
        g.values.add(U32((ModulePtr<Value>)argPointer), variable(g, name));
        index++;
    }

    result->body = genBody(g, function);
    g.file.statements.push(g.file.arena, asStmt(g, result));

    /*
     * The teardown metadata, where the environment has any.
     *
     * A closure the program can hold is torn down by whatever its *lambda* captured, and which
     * lambda a value came from is a run-time fact once two of them reach one drop. Native answers
     * that by putting the header in front of the entry point and subtracting a constant from the
     * code word; a JS function has no bytes in front of it, so the header is a property of the code
     * word instead - one assignment beside the declaration, for every closure of this lambda that
     * will ever exist.
     *
     * That is the whole of what moved. It used to be two stores *per closure*, hung on each function
     * object by the factory; it is now one store per lambda and none per closure, and the place walk
     * reaches it as `value.$c.$h` where native reaches it as `code - sizeof(header)`.
     *
     * **Every lambda that has a header gets it**, including one whose captures have nothing to tear
     * down, and that is a change rather than an oversight. It used to be conditional on the captures
     * having an authored `Drop` between them, which was sound only because the environment word was
     * conditional in the same way: `drop$T` guards on `value.$e != null`, and a closure that had
     * neither word skipped the branch. The pair always carries an environment, so the guard now
     * passes for every capturing lambda and the header has to be there when it does. Native has
     * always worked this way - `closureHeaderFor` emits one for every capturing lambda and the drop
     * slot is `teardown$none` where there is nothing to run - so this is the two targets agreeing
     * again rather than this one giving something up: what the elision saved was two stores per
     * closure, and there are none of those left to save.
     */
    if(auto header = closureNeedsTeardown(g, function) ? function.closureHeader : nullptr) {
        auto code = variable(g, result->name);
        g.file.statements.push(g.file.arena, asStmt(g, make<ExprStmt>(g,
            assign(g, field(g, code, g.headerField), globalValue(g, header)))));
    }

    g.function = nullptr;
}

/*
 * Globals.
 */

/*
 * A compiler-built table as an array, one element per slot - see tableCell.
 *
 * This is the JS materialization of resolve/witness.h's tables, and it is the whole of it. An
 * address is the emitted name of what it names, which is the JS equivalent of a relocation and needs
 * no loader; a number is a number. Nothing is laid out, because there is nowhere to lay it out: the
 * slot number *is* the position.
 */
JsPtr<Expr> tableValue(Gen& g, Global& global_) {
    auto table = make<ArrayExpr>(g);

    for(auto slot: global_.table.contents(g.local)) {
        switch(slot.kind) {
            case TableCell::Function: {
                /*
                 * A slot naming something this target does not have becomes `null` rather than a
                 * diagnostic. That is not a hole: the slots in question are a descriptor's
                 * `moveInit` and `reclaim`, and neither is ever read here - an erased relocation is
                 * an assignment because a JS value has no size to copy, and reclamation is the host
                 * collector's. A diagnostic would be reporting a table entry nothing loads.
                 *
                 * A null `function` is the deliberately empty slot, and lands here too, for the
                 * same reason and with the same value.
                 */
                JsPtr<Expr> cell = nullValue(g);

                if(slot.function) {
                    if(auto found = g.functionNames.get(U32(slot.function))) {
                        cell = variable(g, found.unwrap());
                    }
                }

                table->values.push(g.file.arena, cell);
                break;
            }

            case TableCell::Global: {
                if(slot.global && g.emittedGlobals.contains(U32(slot.global))) {
                    table->values.push(g.file.arena, globalValue(g, slot.global));
                } else if(slot.global) {
                    g.forward.push(Forward { g.tableName, U32(table->values.size()), slot.global });
                    table->values.push(g.file.arena, nullValue(g));
                } else {
                    table->values.push(g.file.arena, nullValue(g));
                }

                break;
            }

            // How wide a type is *here*, which is not what the native target would have said. The
            // descriptor was built without an answer for exactly this reason - see TableCell::Metric.
            case TableCell::Metric:
                table->values.push(g.file.arena,
                                   number(g, F64(tableMetricValue(g.repr, slot))));
                break;

            // A number, a type or a class - all three are the word in `value`. What distinguishes
            // the last two is only that a dump can name them; here they are what they were written
            // as, which is a region offset an emitted debug check would compare.
            case TableCell::Int:
            case TableCell::Type:
            case TableCell::Class:
                table->values.push(g.file.arena, number(g, F64(slot.value)));
                break;
        }
    }

    return asExpr(g, table);
}

void genGlobal(Gen& g, ModulePtr<Global> pointer) {
    auto& global_ = *g.local[pointer];
    auto found = g.globalNames.get(U32(pointer));
    auto name = found ? found.unwrap() : Name {};

    if(global_.isTable) {
        // The table is built before this global counts as emitted, so that a cell naming the table
        // it is inside becomes a forward patch rather than a `const` that reads itself.
        g.tableName = name;
        emit(g, make<DeclStmt>(g, name, tableValue(g, global_), true));
        g.emittedGlobals.add(U32(pointer));
        return;
    }

    g.emittedGlobals.add(U32(pointer));

    /*
     * A scalar starts at the bits of its constant and an aggregate at its zero value, which is the
     * same statement in both cases: there is no program point at which module-level code would run,
     * so an initializer is a constant rather than an expression.
     */
    JsPtr<Expr> initial;

    if(isFloat(g.global, global_.type)) {
        /*
         * `initial` is *storage* - see floatBits - and a `var` holds a number rather than storage.
         *
         * Native emission writes those bytes out and the value reappears when the load reads them
         * back, so the two targets need different halves of the same fact and only this one has to
         * say so. Read as an integer, `let &one = 1.0 :: Float` was `var one = 1065353216`.
         */
        ConstDouble constant(nullptr, global_.type, floatFromBits(g.global, global_.type, global_.initial));
        initial = constantValue(g, constant);
    } else if(isDirectType(g.global, global_.type)) {
        ConstInt constant(nullptr, global_.type, global_.initial);
        initial = constantValue(g, constant);
    } else {
        initial = zeroValue(g, global_.type);
    }

    // And the box where something takes a reference to this one, which is the storage a `var` has
    // no other way to offer - see Gen::boxedGlobals.
    if(g.boxedGlobals.contains(U32(pointer))) initial = boxOf(g, initial);

    emit(g, make<DeclStmt>(g, name, initial, false));
}

/*
 * The cells that name a table declared further down.
 *
 * These tables refer to each other - a `Num` witness holds the `FromInt` witness for the same type -
 * and interning order is not a topological order. A `const` is not hoisted, so naming a later one
 * inside an earlier one's initializer is a run-time error rather than a forward reference; assigning
 * the cell once every table exists says the same thing and needs no ordering at all. Functions need
 * none of this, because a function declaration *is* hoisted.
 */
void genForwardCells(Gen& g) {
    for(auto& patch: g.forward) {
        if(!g.globalNames.get(U32(patch.target))) continue;

        emitExpr(g, assign(g, index(g, variable(g, patch.table), patch.cell),
                           globalValue(g, patch.target)));
    }
}

/*
 * Which globals are stored boxed - see Gen::boxedGlobals.
 *
 * A global that is referred to at all and is not a host object needs the box, on exactly the terms
 * prepareLocals states for a local: a reference to something that is not an object has nowhere to
 * point, and a global has no enclosing object to be reached through even when it is one.
 *
 * Asked of the whole program before anything is emitted, because a global crosses functions and the
 * two sides cannot see each other. Read off `InstBorrow` and `InstAddress` alone: those are the only
 * instructions that produce a reference, and an ordinary load or store of a global names the `var`
 * whether or not it is boxed.
 */
void boxedGlobals(Gen& g) {
    for(auto module: g.program.modules) {
        for(auto pointer: module->functionOrder.contents(g.local)) {
            if(!hasBody(g, module, pointer) || g.excluded.contains(U32(pointer))) continue;

            eachInstruction(g, *g.local[pointer], [&](Value& instruction) {
                const Place* place = nullptr;
                if(instruction.kind == Value::Borrow) place = &((InstBorrow&)instruction).place;
                else if(instruction.kind == Value::Address) place = &((InstAddress&)instruction).place;
                if(!place || place->root != PlaceRoot::Global) return;

                auto type = g.local[place->global]->type;
                if(!isJsObject(g, type)) g.boxedGlobals.add(U32(place->global));
            });
        }
    }
}

/*
 * Which one-field tuples keep their wrapper - see Gen::opaqueTuples.
 *
 * Transparency is what removes the object, and `genBorrow` has exactly one shape it cannot name
 * afterwards: the address of storage reached *through a projection* whose type is no longer an
 * object. A whole local has the slot `prepareLocals` boxed and a root that is already a pointer has
 * itself, but a field of an object holds a *value*, so there is nothing for `%T` to point at and the
 * box that would stand in for one is a copy - which is precisely the form B removed. `Sink.yana`'s
 * `&p.left` is the case, and `ErasedRelocate.yana`'s `relocate(p)` is the same shape arriving
 * through the erased ABI.
 *
 * So those types are told to keep the wrapper, and the wrapper is then the slot.
 *
 * Whole-program for `boxedGlobals`' reason: a type is constructed in one function and has its
 * address taken in another, and the two cannot see each other. Read off `InstBorrow` and
 * `InstAddress` alone, which are the only instructions that produce a reference.
 *
 * **One pass is enough, and that is a property rather than an assumption.** Excluding a tuple can
 * only turn a type that was not an object into one, never the other way round - so a second round
 * could find no case this one did not. What it *cannot* see is `aliasBorrows` and the boxed-local
 * list, which are per-function and not built yet; both would only let a type back in, so not seeing
 * them costs a wrapper and never a miscompile.
 */
void opaqueTuples(Gen& g) {
    for(auto module: g.program.modules) {
        for(auto pointer: module->functionOrder.contents(g.local)) {
            if(!hasBody(g, module, pointer) || g.excluded.contains(U32(pointer))) continue;

            // `placeType` walks the place against the function's own schema, so the walk has to be
            // told which function it is in - the one piece of per-function state this needs.
            g.function = g.local[pointer];

            eachInstruction(g, *g.function, [&](Value& instruction) {
                const Place* place = nullptr;
                if(instruction.kind == Value::Borrow) place = &((InstBorrow&)instruction).place;
                else if(instruction.kind == Value::Address) place = &((InstAddress&)instruction).place;

                // A reference to a whole root is the case that already has an answer, whichever
                // root it is - a boxed local, or a pointer that is its own slot.
                if(!place) return;

                auto projections = const_cast<Place*>(place)->projections;
                auto count = projections.size();
                if(!count) return;

                auto type = placeType(g, *place);

                if(!type || isJsObject(g, type)) return;

                if(auto tuple = transparentTupleOf(g, type)) g.opaqueTuples.add(U32(tuple));
            });
        }
    }

    g.function = nullptr;
}

/*
 * Naming, which happens before anything is generated and all of it at once: a function's very first
 * statement may name a global, a table holds the address of a function nothing has emitted yet, and
 * a local must not shadow either.
 */
void nameProgram(Gen& g) {
    for(auto module: g.program.modules) {
        for(auto pointer: module->globalOrder.contents(g.local)) {
            if(!emitGlobal(g, module, pointer)) continue;

            auto text = g.context.findName(g.local[pointer]->name);
            g.globalNames.add(U32(pointer), uniqueName(g, stringView(text), false));
        }

        for(auto pointer: module->functionOrder.contents(g.local)) {
            if(!emitFunction(g, module, pointer)) continue;

            auto function = g.local[pointer];
            auto text = stringView(g.context.findName(function->name));

            g.functionNames.add(U32(pointer), uniqueName(g, text, false));
        }
    }
}

} // namespace

/*
 * Whether a flattened narrow reference has to be reassembled into an object anyway.
 *
 * Flattening is a calling convention, and it covers every use that is a call or a dereference. What
 * it cannot cover is a use that needs the reference to be *one value*: JS has no multi-value return,
 * so a reference that is returned, stored in a record or captured by a closure has to become the
 * `{$o,$k,$s}` object again. The measured alternatives to allocating there are all worse than
 * allocating - a caller-provided out-object came to 88% of a fresh one - so the object stays as the
 * fallback rather than being engineered away.
 *
 * A reference can need both forms, and then it has both: the parts are still what a call site reads,
 * and the object is built once beside them.
 *
 * Conservative by default, on the same terms as borrowStaysHere: an instruction kind this does not
 * recognize is one whose operand handling has not been checked, and building an object that turns out
 * to be unnecessary costs an allocation while failing to build a needed one emits `undefined`.
 */
/*
 * The arity guard.
 *
 * Extra arguments are free in the range flattening produces - holding callee work constant, an
 * inlinable callee measured flat from 2 to 64 arguments and a non-inlinable one was free to about 12
 * and decayed past 16-24 as TurboFan ran out of registers to keep them in. A signature with many
 * ordinary parameters *and* many references is the one shape that reaches the far end of that, and
 * there the descriptor it would have allocated is cheaper than the spills.
 *
 * All or nothing for a signature, and computed from the declarations alone, because the caller and
 * the callee decide it separately and have to agree: a per-parameter rule would need both of them to
 * arrive at the same count anyway, and this way the count is the rule.
 *
 * **One count for both flattened forms**, references and function values together, which is what
 * `opt/opt_arg.cpp` says about its own: "a signature is flattened once, counting flattened fields
 * and ordinary parameters together". Two counters would let a signature flatten its references and
 * decline its function values, and the arity the guard is protecting is the whole signature's.
 */
static const Size kFlatArityLimit = 24;

template<class F>
static bool signatureFlattens(Gen& g, Size count, F&& arityOf) {
    Size arity = 0;
    auto any = false;

    for(Size i = 0; i < count; i++) {
        auto width = arityOf(i);

        arity += width;
        if(width > 1) any = true;
    }

    return any && arity <= kFlatArityLimit;
}

bool functionFlattensArgs(Gen& g, Function& function) {
    return signatureFlattens(g, function.args.size(), [&](Size i) -> Size {
        auto arg = g.local[function.args.get(g.local, i)];
        if(refIsFlattened(g, arg->type, arg->convention)) return flatRefArity(g, arg->type);
        if(funIsFlattened(g, arg->type, arg->convention)) return kFlatFunArity;

        return 1;
    });
}

static bool funTypeFlattensArgs(Gen& g, FunType& type) {
    return signatureFlattens(g, type.args.size(), [&](Size i) -> Size {
        auto arg = type.args.get(g.global, i);
        if(refIsFlattened(g, arg.type, arg.convention)) return flatRefArity(g, arg.type);
        if(funIsFlattened(g, arg.type, arg.convention)) return kFlatFunArity;

        return 1;
    });
}

/*
 * The declared parameter at one argument position, whichever kind of call this is.
 *
 * The arity an argument occupies is decided from this and never from the argument's own type - see
 * pushArg - so both the emitter and the question below have to read it from the same place. The two
 * flattened forms ask it the same way and differ only in what they then ask of the declaration,
 * which is why the walk is written once here.
 */
template<class Declared, class Signature>
static bool callParameterIs(Gen& g, Value& user, Size index, Declared&& declared, Signature&& signature) {
    auto fromFunction = [&](ModulePtr<Function> callee) {
        if(!callee) return false;

        auto function = g.local[callee];
        if(index >= function->args.size()) return false;
        if(!functionFlattensArgs(g, *function)) return false;

        return declared(*(Arg*)g.local[function->args.get(g.local, index)]);
    };

    switch(user.kind) {
        case Value::Call:
            return fromFunction(((InstCall&)user).callee);
        case Value::GenCall:
            return fromFunction(((InstGenCall&)user).callee);
        case Value::CallDyn: {
            // An indirect call has a signature where a direct one has a callee, and it carries the
            // same declarations.
            auto declaredType = ((InstCallDyn&)user).signature;
            if(!declaredType || g.global[declaredType]->kind != Type::Fun) return false;

            auto type = (FunType*)g.global[declaredType];
            if(index >= type->args.size()) return false;
            if(!funTypeFlattensArgs(g, *type)) return false;

            return signature(type->args.get(g.global, index));
        }
        default:
            return false;
    }
}

bool callParameterIsFlatRef(Gen& g, Value& user, Size index) {
    return callParameterIs(g, user, index,
        [&](Arg& arg) { return refIsFlattened(g, arg.type, arg.convention); },
        [&](const FunArg& arg) { return refIsFlattened(g, arg.type, arg.convention); });
}

bool callParameterIsFlatFun(Gen& g, Value& user, Size index) {
    return callParameterIs(g, user, index,
        [&](Arg& arg) { return funIsFlattened(g, arg.type, arg.convention); },
        [&](const FunArg& arg) { return funIsFlattened(g, arg.type, arg.convention); });
}

/*
 * Whether the declared parameter at one argument position takes a *value* rather than a reference.
 *
 * The resolver hands a **borrow** over for a by-value parameter of memory type - there is nothing to
 * copy and the callee wants the storage - and on this target such a parameter arrives as the box
 * prepareLocals reads it back through. That used to need no saying, because an immutable borrow *was*
 * the box; once it became the triple the two stopped agreeing, and `unwrapOr(Tree)` read `$v` off a
 * `{$o,$k,$s}` and got `undefined`.
 *
 * Decided from the declaration, like every other question about an argument position, so that the
 * caller and the callee reach it separately and agree. `&` parameters are excluded because they are
 * references on both sides; a reference *type* is excluded for the same reason.
 */
static bool declaredArgTakesValue(Gen& g, TypePtr type, ast::BindType convention) {
    if(convention == ast::BindType::Ref || !type) return false;

    auto kind = g.global[type]->kind;
    return kind != Type::Borrow && kind != Type::Ptr && kind != Type::RegionPtr;
}

bool callParameterTakesValue(Gen& g, Value& user, Size index) {
    auto fromFunction = [&](ModulePtr<Function> callee) {
        if(!callee) return false;

        auto function = g.local[callee];
        if(index >= function->args.size()) return false;

        auto arg = g.local[function->args.get(g.local, index)];
        return declaredArgTakesValue(g, arg->type, arg->convention);
    };

    switch(user.kind) {
        case Value::Call:
            return fromFunction(((InstCall&)user).callee);
        case Value::GenCall:
            return fromFunction(((InstGenCall&)user).callee);
        case Value::CallDyn: {
            auto signature = ((InstCallDyn&)user).signature;
            if(!signature || g.global[signature]->kind != Type::Fun) return false;

            auto type = (FunType*)g.global[signature];
            if(index >= type->args.size()) return false;

            auto arg = type->args.get(g.global, index);
            return declaredArgTakesValue(g, arg.type, arg.convention);
        }
        default:
            return false;
    }
}

/*
 * Whether one declared parameter is a position at all.
 *
 * A unit value has no representation on this target either - `define` emits what has an effect and
 * names nothing - so a parameter of unit type is neither passed nor received. It is not a shape
 * anyone writes: it is what a generic function's type variable becomes when the call site has
 * nothing to hand over, which is the ordinary case for a callback's result.
 *
 * A `&` parameter is the exception. What travels there is a reference to the caller's storage, and
 * a reference exists whatever it refers to.
 */
bool declaredArgIsAbsent(Gen& g, TypePtr type, ast::BindType convention) {
    return convention != ast::BindType::Ref && isUnit(g.global, type);
}

// The same, read off whichever kind of call this is - and off the *declared* parameter rather than
// off the argument, for the reason callParameterIsFlatRef gives: both sides have to leave the same
// positions out or every argument after one shifts.
bool callParameterIsAbsent(Gen& g, Value& user, Size index) {
    auto fromFunction = [&](ModulePtr<Function> callee) {
        if(!callee) return false;

        auto function = g.local[callee];
        if(index >= function->args.size()) return false;

        auto arg = g.local[function->args.get(g.local, index)];
        return declaredArgIsAbsent(g, arg->type, arg->convention);
    };

    switch(user.kind) {
        case Value::Call:
            return fromFunction(((InstCall&)user).callee);
        case Value::GenCall:
            return fromFunction(((InstGenCall&)user).callee);
        case Value::CallDyn: {
            auto signature = ((InstCallDyn&)user).signature;
            if(!signature || g.global[signature]->kind != Type::Fun) return false;

            auto type = (FunType*)g.global[signature];
            if(index >= type->args.size()) return false;

            auto arg = type->args.get(g.global, index);
            return declaredArgIsAbsent(g, arg.type, arg.convention);
        }
        default:
            return false;
    }
}

// Whether this reference reaches a call at a position that takes it flat. A position that does not -
// a generic `&a` parameter, whose body has no width to mask with - needs the object like any other
// use that wants one value.
static bool passedFlat(Gen& g, Value& user, ModulePtr<Value> reference) {
    Size index = 0;
    auto flat = true;

    ModuleList<ModulePtr<Value>, false>* args = nullptr;
    switch(user.kind) {
        case Value::Call: args = &((InstCall&)user).args; break;
        case Value::CallDyn: args = &((InstCallDyn&)user).args; break;
        case Value::GenCall: args = &((InstGenCall&)user).args; break;
        default: return false;
    }

    for(auto arg: args->contents(g.local)) {
        if(arg == reference && !callParameterIsFlatRef(g, user, index)) flat = false;
        index++;
    }

    // The callee of a dynamic call is an operand too, and it is a function rather than a reference.
    if(user.kind == Value::CallDyn) {
        auto& dyn = (InstCallDyn&)user;
        if(dyn.callable == reference || dyn.address == reference) return false;
    }

    return flat;
}

bool narrowRefNeedsObject(Gen& g, ModulePtr<Value> reference) {
    for(auto userPointer: g.local[reference]->uses.contents(g.local)) {
        auto& user = *g.local[userPointer];

        switch(user.kind) {
            // Passed on, which is what the flat convention is for - at every position that takes it
            // flat. One that does not wants the object, and this is the only use that can be both.
            case Value::Call:
            case Value::CallDyn:
            case Value::GenCall:
                if(!passedFlat(g, user, reference)) return true;
                continue;

            // Dereferenced, re-borrowed, or the target of a write - all of them walk the place and
            // want the parts rather than the object.
            case Value::LoadPlace:
            case Value::Borrow:
            case Value::Address:
            case Value::Move:
            case Value::Swap:
                continue;

            // A store *of* the reference needs the value; a store *through* it does not.
            case Value::Init:
            case Value::Assign:
                if(((InstInit&)user).value == reference) return true;
                continue;
            case Value::Exchange:
                if(((InstExchange&)user).value == reference) return true;
                continue;

            default:
                return true;
        }
    }

    return false;
}

Ptr<File> genProgram(Context& context, Program& program) {
    // The optimizer, against this target - see compiler/opt. The native path makes the same call
    // with its own target at the top of lowerProgram, and neither program has been through the
    // other's: `@platform` selects declarations during resolution, so a JS build and a native build
    // are two resolved programs and this rewrites one of them.
    optimizeProgram(context, program, jsReprTarget());

    auto file = Ptr<File>(new File(8 * 1024 * 1024));

    ReprTable repr(*program.types, jsReprTarget());

    Gen g {
        context, program, *file, *program.types, *program.arena, *file->arena, repr
    };

    g.tagField = literalName(g, "$tag"_v);
    g.payloadField = literalName(g, "$p"_v);
    g.boxField = literalName(g, "$v"_v);
    g.codeField = literalName(g, "$c"_v);
    g.envField = literalName(g, "$e"_v);
    g.refObject = literalName(g, "$o"_v);
    g.refKey = literalName(g, "$k"_v);
    g.refScale = literalName(g, "$s"_v);
    g.refEnvKey = literalName(g, "$ke"_v);
    g.headerField = literalName(g, "$h"_v);
    if(program.root) g.headerType = closureHeaderPlaceType(*program.root);

    excludeFunctions(g);

    // Ahead of `boxedGlobals`, which asks `isJsObject` - and what a type's shape is has to be
    // settled before anything reads it, or two answers to one question is exactly the split
    // Gen::opaqueTuples exists to prevent.
    opaqueTuples(g);

    boxedGlobals(g);
    nameProgram(g);

    g.body = &file->statements;

    for(auto module: program.modules) {
        for(auto pointer: module->globalOrder.contents(g.local)) {
            if(emitGlobal(g, module, pointer)) genGlobal(g, pointer);
        }
    }

    genForwardCells(g);

    for(auto module: program.modules) {
        auto first = true;

        for(auto pointer: module->functionOrder.contents(g.local)) {
            if(!emitFunction(g, module, pointer)) continue;

            g.body = &file->statements;

            if(first) {
                emit(g, make<CommentStmt>(g, module->name));
                first = false;
            }

            genFunction(g, pointer);
        }
    }

    g.body = &file->statements;

    // Bit ranges first: one of those bodies may ask for a wide helper, and `emitWideHelpers` walks
    // a list that is allowed to grow underneath it while the reverse is not true.
    emitBitHelpers(g);
    emitWideHelpers(g);

    optimizeFile(g);
    return file;
}

void printProgramJs(Net::Writer& writer, Context& context, Program& program, bool minify) {
    auto file = genProgram(context, program);
    formatFile(writer, context, *file, minify);
}

} // namespace js
