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
 *  - a function value is a host *function*. The two words FunValueLayout describes are the closure
 *    and what it closed over, so calling one is an ordinary call with nothing to unpack, and a
 *    capturing lambda is built by a factory - `L$make(env)` - which is what gives each closure its
 *    own environment. A lambda that captured nothing, and a plain function used as a value, are the
 *    function itself and cost no allocation at all;
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
    if(!module->root && !function->used) return false;

    return true;
}

bool emitFunction(Gen& g, Module* module, ModulePtr<Function> pointer) {
    return hasBody(g, module, pointer) && !g.excluded.contains(U32(pointer));
}

/*
 * A global this target emits.
 *
 * `Native`'s own storage is the heap allocator's bookkeeping, and the functions that read it are not
 * emitted either.
 *
 * A closure header is emitted like any other table. On native it is `prefixOf` - bytes placed
 * immediately in front of a lifted function, reached by subtracting from the code address - and here
 * it is an ordinary module-level `const` that the lambda's factory attaches to each closure it
 * builds. Same two slots, same contents, and the only difference is how a teardown gets to it.
 */
bool emitGlobal(Gen& g, Module* module, ModulePtr<Global> pointer) {
    auto global_ = g.local[pointer];

    if(!module->root && !global_->used && !global_->prefixOf) return false;
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

    // A `&` parameter names storage the caller owns, so the box was made there and arrives as the
    // argument. Nothing here allocates it and nothing here writes it back.
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
 * Whether a closure of this lambda has to carry its own teardown metadata.
 *
 * The header's two slots are the environment's `Drop` and its `Reclaim`, and the second is nothing
 * here - the host collector owns reclamation, which is Design-Memory §4's carve-out for this target.
 * So the question is only whether the captures have an authored `Drop` between them, and where they
 * do not there is nothing for a teardown to reach and nothing to attach.
 */
bool closureNeedsTeardown(Gen& g, Function& function) {
    if(!function.closureHeader || function.args.isEmpty()) return false;

    auto envType = pointeeType(g.global, g.local[function.args.get(g.local, 0)]->type);
    if(!envType) return false;

    return ownershipOf(*function.module, envType).drop != TeardownKind::None;
}

// The body, once the parameters have been named and bound. Split out because a capturing lambda's
// goes inside a function *expression* and every other function's goes inside a declaration, and
// nothing else about building one differs.
StmtList genBody(Gen& g, Function& function) {
    prepareLocals(g, function);
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
 * One function, in whichever of the two forms it has here.
 *
 * A *code word* - a lifted lambda, or the thunk that makes a named function a function value - does
 * not take the environment as a parameter on this target, because a function value is a host
 * function and there is no second word to pass. Which leaves two shapes:
 *
 *  - one that captured nothing has the parameter dropped and is otherwise an ordinary declaration,
 *    since its body never reads it;
 *  - one that captured something becomes a *factory*: `L$make(env)` returning a closure over `env`.
 *    The environment is a parameter of the factory rather than of the closure, so each call builds
 *    a separate function object over separate storage - which is what a value the program can hold
 *    and return has to be, and what binding the environment into the emitted lambda directly could
 *    not give, since `var` is function-scoped and a loop would hand every closure the last one.
 *
 * The captures themselves still go through the environment object rather than becoming parameters
 * of the factory. That object is storage the ownership model tracks - it is the local whose drop a
 * closure's teardown devirtualizes to, and the one the header's reclaim names - so dissolving it is
 * a decision for the resolver rather than a rewrite here.
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
    g.writebacks.clear();
    g.pendingCode.reset();
    g.labelCounter = 0;
    g.genEnv = nullptr;
    g.genContext = functionGen(g.global, function);
    g.genModule = function.module;

    auto found = g.functionNames.get(U32(pointer));
    auto result = make<FunStmt>(g, found ? found.unwrap() : Name {});
    auto closure = function.takesEnv && function.closureHeader ? make<FunValueExpr>(g) : nullptr;

    // The environment comes first, on the same terms as native: every unspecialized generic
    // function receives it, whatever its signature says.
    if(g.genContext) {
        auto name = uniqueName(g, "genEnv"_v, true);
        result->args.push(g.file.arena, name);
        g.genEnv = variable(g, name);
    }

    U16 index = 0;
    JsPtr<Expr> environment = nullptr;

    // The same answer every caller computes from the same declarations - see functionFlattensRefs.
    auto flattensRefs = functionFlattensRefs(g, function);

    for(auto argPointer: function.args.contents(g.local)) {
        auto arg = g.local[argPointer];
        auto name = valueName(g, *arg);

        if(function.takesEnv && index == 0) {
            // The environment: the factory's parameter where there is a factory, and nothing at all
            // where nothing was captured, since the body never reads it.
            if(closure) result->args.push(g.file.arena, name);
            environment = variable(g, name);
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

        auto& into = closure ? closure->args : result->args;

        /*
         * A narrow reference arrives as its three parts rather than as an object - see
         * refIsFlattened. There is nothing to allocate and nothing to project: `flip(o, k, s)` reads
         * `(o[k] >>> s) & 1` directly, where the descriptor form read `(r.$o[r.$k] >>> r.$s) & 1`
         * and the caller had to build `r` first.
         *
         * The parts are named after the parameter so that the emitted source still says which
         * reference they belong to, and the whole triple is dropped if the body never touches it.
         */
        if(flattensRefs && refIsFlattened(g, arg->type, arg->convention)) {
            auto owner = refPartName(g, *arg, "$o"_v);
            auto key = refPartName(g, *arg, "$k"_v);

            into.push(g.file.arena, owner);
            into.push(g.file.arena, key);

            RefParts parts;
            parts.owner = variable(g, owner);
            parts.key = variable(g, key);

            if(narrowRefCarriesScale(g)) {
                auto scale = refPartName(g, *arg, "$s"_v);
                into.push(g.file.arena, scale);
                parts.scale = variable(g, scale);
            }

            g.flatRefs.add(U32((ModulePtr<Value>)argPointer), parts);
            index++;
            continue;
        }

        into.push(g.file.arena, name);
        g.values.add(U32((ModulePtr<Value>)argPointer), variable(g, name));
        index++;
    }

    if(!closure) {
        result->body = genBody(g, function);
    } else {
        closure->body = genBody(g, function);

        /*
         * The teardown metadata, where the environment has any.
         *
         * A closure the program can hold is torn down by whatever its *lambda* captured, and which
         * lambda a value came from is a run-time fact once two of them reach one drop. Native
         * answers that by putting the header in front of the entry point; here the factory hangs it
         * on the closure it just built, along with the environment the header's slots are run over.
         *
         * Two stores, and only for a closure whose environment has something to tear down - which
         * is what keeps the ordinary lambda, the one over an `Int`, at one allocation and nothing
         * else. Everything else is left to the host collector, exactly as §3.3 says.
         */
        auto header = closureNeedsTeardown(g, function) ? function.closureHeader : nullptr;

        if(!header) {
            result->body.push(g.file.arena, asStmt(g, make<ReturnStmt>(g, asExpr(g, closure))));
        } else {
            auto name = uniqueName(g, "closure"_v, true);
            auto self = variable(g, name);

            result->body.push(g.file.arena, asStmt(g, make<DeclStmt>(g, name, asExpr(g, closure), false)));
            result->body.push(g.file.arena, asStmt(g, make<ExprStmt>(g,
                assign(g, field(g, self, g.envField), environment))));
            result->body.push(g.file.arena, asStmt(g, make<ExprStmt>(g,
                assign(g, field(g, self, g.headerField), globalValue(g, header)))));
            result->body.push(g.file.arena, asStmt(g, make<ReturnStmt>(g, self)));
        }
    }

    g.file.statements.push(g.file.arena, asStmt(g, result));
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

    if(isDirectType(g.global, global_.type)) {
        ConstInt constant(nullptr, global_.type, global_.initial);
        initial = constantValue(g, constant);
    } else {
        initial = zeroValue(g, global_.type);
    }

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

            /*
             * A capturing lambda is emitted as the factory that builds its closures rather than as
             * itself - see genFunction - so what this name belongs to is the factory, and saying
             * so is the difference between reading the output and guessing at it.
             */
            if(function->takesEnv && function->closureHeader) {
                char buffer[512];
                auto length = min(text.length, sizeof(buffer) - 8);
                copy(text.ptr, buffer, length);
                copy("$make", buffer + length, 5);

                g.functionNames.add(U32(pointer), uniqueName(g, StringView { buffer, length + 5 }, false));
                continue;
            }

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
 */
static const Size kFlatArityLimit = 24;

template<class F>
static bool signatureFlattens(Gen& g, Size count, F&& isRef) {
    Size arity = 0;
    auto any = false;

    for(Size i = 0; i < count; i++) {
        if(isRef(i)) {
            arity += flatRefArity(g);
            any = true;
        } else {
            arity++;
        }
    }

    return any && arity <= kFlatArityLimit;
}

bool functionFlattensRefs(Gen& g, Function& function) {
    return signatureFlattens(g, function.args.size(), [&](Size i) {
        auto arg = g.local[function.args.get(g.local, i)];
        return refIsFlattened(g, arg->type, arg->convention);
    });
}

static bool funTypeFlattensRefs(Gen& g, FunType& type) {
    return signatureFlattens(g, type.args.size(), [&](Size i) {
        auto arg = type.args.get(g.global, i);
        return refIsFlattened(g, arg.type, arg.convention);
    });
}

// The declared parameter at one argument position, whichever kind of call this is. The arity a
// reference argument occupies is decided from this and never from the argument's own type - see
// pushArg - so both the emitter and the question below have to read it from the same place.
bool callParameterIsFlatRef(Gen& g, Value& user, Size index) {
    auto fromFunction = [&](ModulePtr<Function> callee) {
        if(!callee) return false;

        auto function = g.local[callee];
        if(index >= function->args.size()) return false;
        if(!functionFlattensRefs(g, *function)) return false;

        auto arg = g.local[function->args.get(g.local, index)];
        return refIsFlattened(g, arg->type, arg->convention);
    };

    switch(user.kind) {
        case Value::Call:
            return fromFunction(((InstCall&)user).callee);
        case Value::GenCall:
            return fromFunction(((InstGenCall&)user).callee);
        case Value::CallDyn: {
            // An indirect call has a signature where a direct one has a callee, and it carries the
            // same declarations.
            auto signature = ((InstCallDyn&)user).signature;
            if(!signature || g.global[signature]->kind != Type::Fun) return false;

            auto type = (FunType*)g.global[signature];
            if(index >= type->args.size()) return false;
            if(!funTypeFlattensRefs(g, *type)) return false;

            auto arg = type->args.get(g.global, index);
            return refIsFlattened(g, arg.type, arg.convention);
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
    g.envField = literalName(g, "$e"_v);
    g.refObject = literalName(g, "$o"_v);
    g.refKey = literalName(g, "$k"_v);
    g.refScale = literalName(g, "$s"_v);
    g.headerField = literalName(g, "$h"_v);
    if(program.root) g.headerType = closureHeaderPlaceType(*program.root);

    excludeFunctions(g);
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
