#include "jsir.h"
#include "build.h"
#include "../../resolve_ssa/js/abi.h"
#include "../../resolve_ssa/js/name.h"
#include "../../resolve_ssa/js/opt.h"

/*
 * The file the JSIR pipeline's functions go into - Analysis-IR-JS.md §9.5 items 6 and 7.
 *
 * The counterpart of `gen.cpp` and deliberately a much smaller one, because most of what that file
 * does is decide things this pipeline has already decided: which locals are boxed, which references
 * are flat, which values are kept twice, what shape a type has. All of those are `JsRep`, `JsShape`
 * and `JsPlace` by the time a body reaches here, so what is left is the file itself - names,
 * globals, functions, the helper families and the entry call.
 *
 * **One function at a time, and the body does not survive the loop.** §7.1: the stable tables live
 * for the compilation and one reusable arena holds the body being translated. That is also why
 * `-mode jsir` writes its dump *inside* the loop - there is no program-wide JSIR to print afterwards
 * and the whole memory argument is that there never will be.
 */

namespace js {

using namespace jsir;

namespace {

struct Assembler {
    Assembler(Assembly& a, mir::Program& program): a(a), program(program), target(a.target) {}

    Assembly& a;
    mir::Program& program;
    JsTarget& target;

    DeclBase decl() { return target.decl; }

    bool emits(DeclPtr<mir::Function> function) {
        auto declared = decl()[function];
        return declared->reachable && declared->ssa;
    }

    /*
     * Every emitted name, before any body.
     *
     * A call names a callee this file may not have reached yet, so the naming is its own pass - the
     * same reason `lower/gen.cpp` declares every signature before it lowers the first body.
     */
    void nameProgram() {
        for(auto modulePointer: program.modules) {
            auto module = decl()[modulePointer];

            for(auto globalPointer: module->globals.contents(decl())) {
                auto global = decl()[globalPointer];
                if(!global->reachable) continue;

                auto name = uniqueName(a, stringView(a.context.findName(global->name)), false);
                a.globalNames.add(U32(globalPointer.offset), name);
            }

            for(auto functionPointer: module->functions.contents(decl())) {
                if(!emits(functionPointer)) continue;

                auto declared = decl()[functionPointer];
                auto name = uniqueName(a, stringView(a.context.findName(declared->name)), false);
                a.functionNames.add(U32(functionPointer.offset), name);
            }
        }
    }

    // ---------------------------------------------------------------- constants

    /*
     * A source constant as a host value, for a module-level `var` or a descriptor's zero cell.
     *
     * **One lowering, and it is JSIR's** - §2.4's "no answer should be recomputed below its owner".
     * This file used to carry a second walk over `mir::` types producing AST directly, and the two
     * disagreed twice: the niche fold was missing here entirely, so a `Maybe(I64)` global holding
     * `Just(3000000000000)` came out as a `number` a `bigint` cannot be mixed with; and a recursive
     * record was closed with a depth cap rather than `zeroOf`'s cycle set, which exhausted the arena
     * on a `data Branch = Leaf | Node {left: Branch, right: Branch}`.
     *
     * So the constant is converted into a throwaway body - one block, one `return` - and
     * `structureConstant` spells it through the same arms every instruction in every function goes
     * through. The body does not survive the call, which is the same rule every other body here
     * follows (§7.1).
     */
    JsPtr<Expr> constantValue(const mir::Constant* constant, JsRepId rep, bool boxed,
                              LocationId where) {
        target.beginBody();

        JsBody body;
        if(!convertConstant(target, constant, rep, boxed, where, body)) return nullValue(a);
        if(!verifyBody(target, body)) return nullValue(a);

        return structureConstant(a, body);
    }

    JsPtr<Expr> zeroExpr(JsRepId rep) {
        return constantValue(nullptr, rep, false, kNullLocation);
    }

    // ---------------------------------------------------------------- globals

    void emitGlobals() {
        auto emitted = false;

        for(auto modulePointer: program.modules) {
            auto module = decl()[modulePointer];

            for(auto globalPointer: module->globals.contents(decl())) {
                auto global = decl()[globalPointer];
                if(!global->reachable) continue;

                auto found = a.globalNames.getValue(U32(globalPointer.offset));
                if(!found) continue;

                auto manifest = jsManifest(target, global->occurrence, global->source);

                /*
                 * A **boxed** global is stored in the one object a `{$o, $k}` descriptor can name -
                 * `JsTarget::boxedGlobals`, which is whole-program and decided before any body was
                 * converted, so this and every `GlobalAddress` agree by construction. The object is
                 * built by the conversion, because a shape is a representation decision.
                 */
                auto boxed = target.boxedGlobals.contains(U32(globalPointer.offset));
                auto value = constantValue(global->initial.isSet() ? &global->initial : nullptr,
                                           manifest.rep, boxed, global->source);

                /*
                 * `var` rather than `const`, even for a global nothing assigns.
                 *
                 * A `const` would be the better statement and it is not available: the initializers
                 * are emitted in module order, and a program whose first module's global names a
                 * function declared in the second reads a binding the file has not reached. `var` is
                 * hoisted and reads `undefined` there rather than throwing, which is the same
                 * latitude the shipping backend's forward cells take.
                 */
                emit(a, make<DeclStmt>(a, found.unwrap(), value, false));
                emitted = true;

                // Conservative, and the pessimization it costs is a local in front of a read: the
                // declaration says the global *may* be assigned, and what the optimizer needs is
                // that it never treats one that can be as inert. §6's own note on `mutableGlobals`.
                if(global->isWritten()) a.mutableGlobals.add(found.unwrap().text);

                /*
                 * **The declaration and not a scan of the bodies.** This file used to walk every
                 * instruction of every body after structuring it, collecting the globals a `Store`
                 * names into a set - and nothing ever read the set. What `mutableGlobals` needs is
                 * the line above: `Global::isWritten` is the resolver's own answer, it is available
                 * before any body exists, and it covers a global written by a module this
                 * compilation has not reached yet, which a scan of the bodies here cannot. The scan
                 * was removed 2026-09-22 (J80); §8.1's rule that the AST tier reads JSIR facts
                 * rather than re-deriving them is the shape of the mistake.
                 */
            }
        }

        if(emitted) emit(a, make<CommentStmt>(a, internText(a, ""_v)));
    }

    // ---------------------------------------------------------------- the erased tables

    /*
     * The erased half of the file - Analysis-IR-JS.md §9.7 item 8, and `lower/erase.cpp`'s
     * counterpart with every byte taken out of it.
     *
     * A cell is an element of a host row here, so there is nothing to encode: no anchor, no
     * four-byte relative address, no packed metrics word. What the three tables hold is names, a
     * number and a zero value, which is the same observation `codegen/js/place.cpp`'s `tableCell`
     * makes from the reading end and the reason it is one line.
     *
     * Emitted descriptors, then witnesses, then environments - see `Assembly::genTableNames` for why
     * the order is load-bearing.
     */
    Name descriptorName(DeclPtr<mir::Type> type) {
        auto key = U64(type.offset);
        if(auto found = a.descriptorNames.getValue(key)) return found.unwrap();

        auto text = mir::typeText(a.context, decl(), type);
        auto name = mintPrefixed(a, "$d$"_v, stringView(text));
        a.descriptorNames.add(key, name);
        a.descriptorOrder.push(type);
        return name;
    }

    /*
     * One class requirement's witness - one function per member, in the class's order, with its
     * superclasses' tables flattened in after them.
     *
     * Interned on the pair the table *is* - the class and the members answering it - for
     * `witnessFor`'s reason: two environments naming one instance should name one table, so that a
     * witness is an identity rather than a copy.
     */
    Name witnessName(DeclPtr<mir::Class> klass, mir::GenWitness witness) {
        auto key = mir::InternKey().mix(klass.offset);
        for(U32 i = 0; i < witness.functions.size(); i++) {
            key.mix(witness.functions.get(decl(), i).offset);
        }

        if(auto found = a.witnessNames.getValue(key.hash)) return found.unwrap();

        auto name = mintPrefixed(a, "$w$"_v, stringView(a.context.findName(decl()[klass]->name)));
        a.witnessNames.add(key.hash, name);
        a.witnessOrder.push(witness);
        a.witnessClasses.push(klass);
        return name;
    }

    /*
     * One cell of one environment - the answer a caller supplies for one `GenSlot`.
     *
     * Null where this target has no form for the slot, which the caller turns into a diagnostic at
     * the generic's own source: a cell holding `undefined` is exactly the shape a body reads back
     * and computes a wrong number from.
     */
    JsPtr<Expr> slotExpr(mir::GenSlot slot, mir::GenWitness witness,
                         LocationId where) {
        switch(witness.kind) {
            case mir::GenSlotKind::Type:
                return witness.type ? variable(a, descriptorName(witness.type)) : nullptr;

            /*
             * The number itself, **at the representation its parameter counts in**.
             *
             * A host number is one thing and a `bigint` is another, and which of the two a cell
             * holds is the parameter's declared type rather than the value's magnitude: a body
             * reading `n: I64` reduces what it read with `BigInt.asIntN`, which throws on a `number`
             * rather than converting one. `generic/Const.yana`'s `wideCapacity` is where that
             * arrives, and it arrives as an exception rather than as a wrong answer.
             */
            case mir::GenSlotKind::Const: {
                auto counts = slot.type && decl()[slot.type]->kind == mir::Type::Variable
                    ? ((mir::VariableType*)decl()[slot.type])->constType : nullptr;

                auto rep = counts ? jsRep(target, counts, where) : JsRepId();
                if(rep.valid() && target.rep(rep).kind == JsRepKind::BigInt) {
                    return bigInt(a, witness.constant, target.rep(rep).isSigned());
                }

                return number(a, F64(witness.constant));
            }

            case mir::GenSlotKind::Class: {
                if(!slot.klass) return nullptr;

                // `Call(f, S)` is the structural class with no instance behind it - J58's
                // synthesized cell, which this target has not built. Reported rather than filled
                // with a table that has no members to put in it.
                if(decl()[slot.klass]->role == intrinsic::Known::call) return nullptr;
                if(witness.functions.size() != mir::classWitnessCount(decl(), slot.klass)) {
                    return nullptr;
                }

                return witnessSlotsAgree(slot.klass, witness, where)
                    ? variable(a, witnessName(slot.klass, witness)) : nullptr;
            }

            /*
             * The one callable a `f: (a) -> b` requirement names, and the concrete function is the
             * cell: a host function is a value, so there is nothing to describe about reaching it.
             */
            case mir::GenSlotKind::Function: {
                if(!witness.functions.size()) return nullptr;

                auto found = a.functionNames.getValue(
                    U32(witness.functions.get(decl(), 0).offset));
                return found ? variable(a, found.unwrap()) : nullptr;
            }

            /*
             * A **property** witness - how one constrained field of its subject is reached.
             *
             * A property is a *name* on this target, so the cell is that name and the read through
             * it is `owner[key]`. The three shapes `lower/erase.cpp` gives the native slot - a byte
             * offset, an offset with a bit shift, and a `{read, set}` pair for a field whose width
             * the body cannot know - are all answers to "how wide is it and where does it start",
             * and a host object's property has neither.
             *
             * Off the **shape** rather than off the field's declared name, which is the same rule
             * `convertAggregate` follows and for the same reason: a property's key is a decision
             * this target takes per (record, ownership shape), and the body reading it has to be
             * reading the one the object was built with. A subject with no object of its own - a
             * transparent record, a host row - has no name to hand over and is reported.
             */
            case mir::GenSlotKind::Property: {
                if(!witness.type) return nullptr;

                /*
                 * The **stored** form, which is what a body compiled once is handed.
                 *
                 * An erased position holds one host value, so a subject this target scalarizes
                 * crosses one as the object its parts materialize into - and the key the witness
                 * hands over is a property of that object. Reading `jsRep` alone named a `Parts`
                 * representation, whose shape is invalid, and a record of three scalars was then
                 * refused as "a subject with no host object of its own" although it has one at
                 * exactly the position this cell is read from.
                 */
                auto rep = jsStoredRep(target, jsRep(target, witness.type, where));
                auto shape = target.rep(rep).kind == JsRepKind::GcObject ? target.rep(rep).shape
                                                                        : JsShapeId();
                auto property = shape.valid() ? target.shape(shape).slotOfField(witness.field)
                                              : JsProperty::kNoField;

                /*
                 * A subject with **no host object of its own** - a transparent record, whose value
                 * is its one stored field, or a host row. There is no name to hand over, and the
                 * honest cell would be the native slot's third shape: a `{read, set}` pair. Named
                 * rather than left to the caller's "does not convert property", because what is
                 * missing is about this subject and not about property witnesses.
                 */
                if(property == JsProperty::kNoField) {
                    target.notYet(
                        "a property witness for a subject with no host object of its own"_v,
                        witness.type, where);
                    return nullptr;
                }

                return asExpr(a, make<StringExpr>(a, target.shape(shape).properties[property].key));
            }

            case mir::GenSlotKind::Count:
                return nullptr;
        }

        return nullptr;
    }

    /*
     * Whether every member of a witness can be called across the signature its class promised.
     *
     * **The whole of what a thunk would do, asked as a question.** Natively an erased position is a
     * pointer and a concrete one is a register, so every witness entry is a shim that loads where
     * the call site stores. Here a host value of any type is one value, so the two plans agree at
     * every position they spell the same way - and where they do not (a `Parts` lane run against one
     * value, an omitted unit against a position, a box against an object) the answer is a report
     * rather than a cell that quietly drops or duplicates an argument.
     */
    bool witnessSlotsAgree(DeclPtr<mir::Class> klass, mir::GenWitness witness,
                           LocationId where) {
        for(U32 slot = 0; slot < witness.functions.size(); slot++) {
            DeclPtr<mir::Class> owner = nullptr;
            U16 member = 0;
            auto object = false;
            if(!mir::classWitnessAt(decl(), klass, U16(slot), owner, member, object)) return false;

            auto declared = decl()[owner]->members.get(decl(), member);
            if(object) declared = decl()[declared]->iterObject;
            if(!declared) return false;

            auto concrete = witness.functions.get(decl(), slot);
            if(!concrete || !a.functionNames.getValue(U32(concrete.offset))) return false;

            auto promised = jsAbiOf(target, declared, where);
            auto supplied = jsAbiOf(target, concrete, where);
            if(!promised.valid() || !supplied.valid()) return false;

            auto& from = target.abi(promised);
            auto& to = target.abi(supplied);
            if(from.arity != to.arity || from.takesGenEnv != to.takesGenEnv) return false;
            if(from.parameters.size() != to.parameters.size()) return false;

            for(U32 i = 0; i < from.parameters.size(); i++) {
                if(from.parameters[i].kind != to.parameters[i].kind) return false;
                if(from.parameters[i].positions != to.parameters[i].positions) return false;
            }

            if(from.result.kind != to.result.kind) return false;
        }

        return true;
    }

    bool emitGenTables(Net::Writer* dump) {
        if(!a.genTableOrder.size()) return true;

        auto ok = true;

        /*
         * Every cell resolved before anything is written, which is what makes the three loops below
         * a line rather than a graph: resolving an environment's cells is what *discovers* the
         * descriptors and witnesses it names, and those have to be declared above it.
         */
        Array<JsPtr<Expr>> cells;
        Array<U32> counts;

        for(U32 i = 0; i < a.genTableOrder.size(); i++) {
            auto environment = a.genTableOrder[i];
            auto& filled = *decl()[environment];
            auto& generic = *decl()[filled.generic];
            auto numbering = decl()[decl()[generic.gen]->schema];

            for(U32 index = 0; index < filled.slots.size(); index++) {
                auto witness = filled.slots.get(decl(), index);
                auto slot = numbering->slots.get(decl(), index);
                auto value = slotExpr(slot, witness, generic.source);

                if(!value) {
                    // A slot arm that has already said what is missing about this cell says it
                    // better than the kind's own name does - see the `Property` arm.
                    if(!target.reported) {
                        target.notYet(mir::genSlotKindName(witness.kind), generic.source);
                    }

                    ok = false;
                    value = nullValue(a);
                }

                cells.push(value);
            }

            counts.push(U32(filled.slots.size()));
        }

        if(!ok) return false;

        emit(a, make<CommentStmt>(a, internText(a,
            "the erased ABI - one table per type, per instance and per call to a body compiled once"_v)));

        // The descriptors: the two facts about ownership a body compiled once may ask, the teardown
        // that answers the second, and the zero a fresh slot of the type holds.
        for(U32 i = 0; i < a.descriptorOrder.size(); i++) {
            auto type = a.descriptorOrder[i];
            auto name = a.descriptorNames.getValue(U64(type.offset));
            if(!name) continue;

            auto owes = jsir::jsOwesTeardown(target, type);
            auto cellsOf = make<ArrayExpr>(a);
            cellsOf->values.push(a.file.arena,
                                 number(a, mir::isTriviallyCopyable(decl(), type) ? 1 : 0));
            cellsOf->values.push(a.file.arena, number(a, owes ? 1 : 0));
            cellsOf->values.push(a.file.arena, owes ? typeTeardownName(type) : noDropName());
            cellsOf->values.push(a.file.arena,
                                 zeroExpr(jsManifest(target, mir::owned(type), kNullLocation).rep));

            emit(a, make<DeclStmt>(a, name.unwrap(), asExpr(a, cellsOf), false));
        }

        /*
         * The shared empty teardown, where any descriptor named it. A `function` declaration hoists,
         * so this may go after the `var`s that reference it - which is what lets the name be
         * assigned by the loop above rather than predicted before it.
         */
        if(a.noDropHelper.text) {
            auto function = make<FunStmt>(a, a.noDropHelper);
            function->args.push(a.file.arena, uniqueName(a, "x"_v, true));
            emit(a, function);
        }

        for(U32 i = 0; i < a.witnessOrder.size(); i++) {
            auto witness = a.witnessOrder[i];
            auto klass = a.witnessClasses[i];
            auto key = mir::InternKey().mix(klass.offset);
            for(U32 f = 0; f < witness.functions.size(); f++) {
                key.mix(witness.functions.get(decl(), f).offset);
            }

            auto name = a.witnessNames.getValue(key.hash);
            if(!name) continue;

            auto members = make<ArrayExpr>(a);
            for(U32 f = 0; f < witness.functions.size(); f++) {
                auto found = a.functionNames.getValue(
                    U32(witness.functions.get(decl(), f).offset));
                members->values.push(a.file.arena,
                                     found ? variable(a, found.unwrap()) : nullValue(a));
            }

            emit(a, make<DeclStmt>(a, name.unwrap(), asExpr(a, members), false));
        }

        auto at = U32(0);
        for(U32 i = 0; i < a.genTableOrder.size(); i++) {
            auto name = a.genTableNames.getValue(U32(a.genTableOrder[i].offset));
            auto table = make<ArrayExpr>(a);

            for(U32 c = 0; c < counts[i]; c++) table->values.push(a.file.arena, cells[at++]);
            if(name) emit(a, make<DeclStmt>(a, name.unwrap(), asExpr(a, table), false));
        }

        emit(a, make<CommentStmt>(a, internText(a, ""_v)));
        (void)dump;
        return true;
    }

    // The shared empty teardown, named on first ask and emitted beside the descriptors.
    JsPtr<Expr> noDropName() {
        if(!a.noDropHelper.text) a.noDropHelper = uniqueName(a, "$nd"_v, false);
        return variable(a, a.noDropHelper);
    }

    /*
     * A type's teardown as a value - the same `$td$` function a `TypeDrop` names, reached through
     * the same worklist so that one type has one body whichever of the two asked for it first.
     */
    JsPtr<Expr> typeTeardownName(DeclPtr<mir::Type> type) {
        return variable(a, typeDropNameFor(a, type, true, true, kNullLocation));
    }

    // ---------------------------------------------------------------- functions

    /*
     * What every converted body goes through before anything reads it - the optimizer and the
     * verifier, in the one order that makes a failure attributable.
     *
     * The verifier runs on what is *emitted* in every build, which is §10.5's "the final verifier is
     * enabled in every build": a shipping build that emitted a body the verifier would have refused
     * is a wrong answer with no reproduction. In an assertion build it runs on the converted body as
     * well, so a body the optimizer broke is named by the run before it rather than by the one
     * after.
     *
     * The dump is printed **after** the optimizer, because `-mode jsir` is how a rewrite is read and
     * a dump taken before it would say nothing about the pass that is supposed to have fired. The
     * one body printed before is the one the conversion refused, which has nothing to optimize.
     */
    bool finishBody(JsBody& body, Net::Writer* dump) {
#ifdef _DEBUG
        if(!verifyBody(target, body)) return false;
#endif

        if(a.context.settings.optimizeJsir && !optimizeBody(target, body)) return false;
        if(dump) printBody(*dump, target, body);

        return verifyBody(target, body);
    }

    bool emitFunction(DeclPtr<mir::Function> functionPointer, Net::Writer* dump) {
        auto declared = decl()[functionPointer];

        // The body arena, reused - §7.1. Reset rather than reallocated, so the steady state is the
        // largest function seen rather than one reservation per function. `beginBody` is also what
        // says the cached `JsCfg` stops describing anything.
        target.beginBody();

        JsBody body;
        auto converted = convertFunction(target, functionPointer, body);
        if(!converted) {
            if(dump) printBody(*dump, target, body);
            return false;
        }

        if(!finishBody(body, dump)) return false;

        auto found = a.functionNames.getValue(U32(functionPointer.offset));
        auto function = make<FunStmt>(a, found ? found.unwrap() : Name {});

        a.localNames.reset();
        a.labelCounter = 0;

        function->body = structureBody(a, body, function->args);
        if(target.reported) return false;

        emit(a, function);
        return emitClosureHeader(functionPointer, dump);
    }

    /*
     * The teardown a lambda's values reach through their code word - §3.2.1, and the one thing
     * about a lambda the two targets represent differently.
     *
     * Native puts four bytes of prefix data in front of the entry point and subtracts a constant
     * from the code word; a JavaScript function has no bytes in front of it, so the header is a
     * *property* of the code word, assigned once beside the declaration rather than once per
     * closure. `$fd` is what reads it, past its null test on the environment word.
     *
     * **Only where the captures owe an authored teardown**, which is where this target departs from
     * the native rule rather than from the design: over there every capturing lambda has a header,
     * because a heap-placed environment's block has to be handed back whether or not the captures
     * run anything. Here the collector does that, so a lambda capturing an `Int` gets no header, its
     * pair's `$h` reads back `undefined`, and `$fd` finds nothing - the same three-way agreement the
     * native stage's zero cell states, with the zero spelled as an absent property.
     *
     * Emitted immediately after the lambda so that the assignment follows the declaration it names,
     * which is also what `gen.cpp` does and what makes the two files' output comparable.
     */
    bool emitClosureHeader(DeclPtr<mir::Function> functionPointer, Net::Writer* dump) {
        auto declared = decl()[functionPointer];
        if(!declared->environment) return true;
        if(!jsOwesTeardown(target, declared->environment)) return true;

        auto code = a.functionNames.getValue(U32(functionPointer.offset));
        if(!code) return true;

        target.beginBody();

        JsBody body;
        if(!convertEnvironmentTeardown(target, functionPointer, body)) return false;
        if(!finishBody(body, dump)) return false;

        char buffer[256];
        auto text = stringView(a.context.findName(declared->name));
        auto length = text.length < sizeof(buffer) - 16 ? text.length : sizeof(buffer) - 16;
        copy("$td$", buffer, 4);
        copy(text.ptr, buffer + 4, length);

        auto name = uniqueName(a, StringView { buffer, U32(length + 4) }, false);
        auto function = make<FunStmt>(a, name);

        a.localNames.reset();
        a.labelCounter = 0;

        function->body = structureBody(a, body, function->args);
        if(target.reported) return false;

        emit(a, function);
        emitExpr(a, assign(a, field(a, variable(a, code.unwrap()), a.headerField),
                           variable(a, name)));
        return true;
    }

    /*
     * The shims a function value needs - see `Assembly::thunkNames`.
     *
     * `function f$fn($e, a, b) { return f(a, b); }`: one environment parameter in front, discarded,
     * so that every call through a pair is `code(env, ...)` whatever the code word turned out to be.
     * Emitted after the bodies, because that is when the set of callees taken as values is complete.
     */
    void emitThunks() {
        for(auto callee: a.thunkOrder) {
            auto name = a.thunkNames.getValue(U32(callee.offset));
            auto target_ = a.functionNames.getValue(U32(callee.offset));
            if(!name || !target_) continue;

            auto abi = jsAbiOf(target, callee, decl()[callee]->source);
            auto function = make<FunStmt>(a, name.unwrap());

            a.localNames.reset();
            function->args.push(a.file.arena, uniqueName(a, jsir::key::env, true));

            /*
             * One argument per **position**, not per parameter - a multi-part one is `positions` of
             * them and a `Omitted` one is none.
             *
             * `parameters.size()` was here and is a different number: a vector or a flat reference
             * crosses as two or more arguments, so a thunk written per parameter dropped every one
             * after the first and the callee read `undefined`. `NarrowRef.yana`'s `apply(raise, ...)`
             * is where it arrives - a `&Bool` through a function value - and it arrived as a wrong
             * answer under `-no-ir-opt`, where the call is not devirtualized into a direct one.
             */
            SmallArray<Name, 8> arguments;
            forEachPosition(target.abi(abi), [&](U32, U32, const JsPassing&, U16) {
                arguments.push(generatedName(a, "a"_v, U32(arguments.size())));
            });

            for(auto argument: arguments) function->args.push(a.file.arena, argument);

            function->body = collect(a, [&] {
                auto node = make<CallExpr>(a, variable(a, target_.unwrap()));
                for(auto argument: arguments) node->args.push(a.file.arena, variable(a, argument));

                if(target.abi(abi).result.kind == JsPassingKind::Omitted) {
                    emitExpr(a, asExpr(a, node));
                } else {
                    emit(a, make<ReturnStmt>(a, asExpr(a, node)));
                }
            });

            emit(a, function);
        }
    }

    /*
     * The per-type teardowns a `TypeDrop` named - see `Assembly::typeDropOrder`.
     *
     * A worklist rather than a list: one of these bodies may name another, because a sum holding a
     * sum tears the inner one down with the same call. So the index walks a list that is allowed to
     * grow underneath it, which is `emitBitHelpers`' relationship to `emitWideHelpers` written as
     * one loop instead of an ordering.
     */
    bool emitTypeDrops(Net::Writer* dump) {
        for(U32 i = 0; i < a.typeDropOrder.size(); i++) {
            auto wanted = a.typeDropOrder[i];

            target.beginBody();

            JsBody body;
            if(!convertTypeTeardown(target, wanted.type, wanted.drop, wanted.reclaim,
                                    wanted.source, body)) {
                return false;
            }

            if(!finishBody(body, dump)) return false;

            auto function = make<FunStmt>(a, wanted.name);
            a.localNames.reset();
            a.labelCounter = 0;

            function->body = structureBody(a, body, function->args);
            if(target.reported) return false;

            emit(a, function);
        }

        return true;
    }

    void emitEntry() {
        if(a.context.settings.mode != CompileMode::Executable) return;
        if(!program.entry) return;

        auto found = a.functionNames.getValue(U32(program.entry.offset));
        if(!found) return;

        emitExpr(a, call(a, variable(a, found.unwrap())));
    }

    bool run(Net::Writer* dump) {
        emit(a, make<CommentStmt>(a, internText(a,
            "Generated by the Yana compiler - the ownership pipeline (Analysis-IR-JS.md)"_v)));

        if(!planProgram(target)) return false;

        nameProgram();
        emitGlobals();

        auto ok = true;
        for(auto modulePointer: program.modules) {
            auto module = decl()[modulePointer];

            for(auto functionPointer: module->functions.contents(decl())) {
                if(!emits(functionPointer)) continue;
                ok = emitFunction(functionPointer, dump) && ok;
            }
        }

        /*
         * The representation and shape declarations, **after** the bodies rather than before them.
         *
         * §8.4 asks for them once, and the only place they are complete is here: a representation is
         * interned when something asks for one, and `planProgram` asks only about declarations - so a
         * record a body builds and no signature mentions has no entry until that body is converted.
         * A header printed first named `%r1` with no declaration for it, which is exactly the kind of
         * dangling reference this tier exists not to have.
         */
        if(dump) printTables(*dump, target);

        // The erased tables before the teardowns they name, since a descriptor asks for one and the
        // teardown worklist is walked by index while it grows.
        ok = emitGenTables(dump) && ok;
        ok = emitTypeDrops(dump) && ok;
        emitThunks();

        /*
         * The helper families, once and only where something asked for one - in `gen.cpp`'s order,
         * which is a dependency order and not a tidy one.
         *
         * The rotations come before `emitWideHelpers` because a 53-bit rotation is written in terms
         * of `wide.cpp`'s shifts, so its body asks for three wide helpers that have to be on the
         * list before that pass walks it. The rest ask nothing of each other.
         */
        /*
         * The thrown sentences, before the helper families and therefore before the entry call: a
         * `var` initializer runs where it is written, and `emitEntry` is what makes anything run.
         */
        for(U32 i = 0; i < a.throwOrder.size(); i++) {
            emit(a, make<DeclStmt>(a, a.throwOrder[i],
                                   asExpr(a, make<StringExpr>(a, a.throwTexts[i])), false));
        }

        emitRotateHelpers(a);
        emitWideHelpers(a);
        emitSaturationHelpers(a);
        emitFloatBitsHelpers(a);
        emitRoundAwayHelper(a);
        emitByteSwapHelpers(a);
        emitBitCountHelpers(a);
        emitBitOpHelpers(a);
        emitDivisionHelpers(a);
        emitGrowHelper(a);
        emitWordHelpers(a);
        emitFunDropHelper(a);

        /*
         * The entry call **last**, which is what the shipping file does too and is load-bearing: a
         * `function` declaration hoists and a `const` does not, so a program that ran its entry
         * above the typed-array pair a bitcast rides on reached that binding in its temporal dead
         * zone. `Bitcast.yana` is where that arrives.
         */
        emitEntry();

        return ok;
    }
};

} // namespace

Name typeDropNameFor(Assembly& a, DeclPtr<mir::Type> type, bool drop, bool reclaim,
                     LocationId source) {
    auto key = (U64(type.offset) << 2) | (U64(drop) << 1) | U64(reclaim);
    if(auto found = a.typeDropNames.getValue(key)) return found.unwrap();

    auto text = mir::typeText(a.context, a.target.decl, type);
    auto name = mintPrefixed(a, "$td$"_v, stringView(text));

    a.typeDropNames.add(key, name);
    a.typeDropOrder.push(TypeDrop { name, type, drop, reclaim, source });
    return name;
}

Ptr<File> genJsirProgram(Context& context, mir::Program& program, ssa::Converted& converted,
                         Net::Writer* dump) {
    auto file = Ptr<File>(new File(8 * 1024 * 1024));
    JsTarget target(context, program, converted);

    // The two rows this target recognizes by role rather than by shape - see `JsTarget`. Read once,
    // because they are program facts and asking per type would be a hash lookup per projection.
    target.stringRecord = program.roles.record(intrinsic::Known::string);
    target.boolRecord = mir::recordDeclarationOf(*program.decl, mir::booleanType(program));

    Assembly a { { context, *file, *file->arena }, target };
    a.useJsirRules();
    nameSharedFields(a);

    auto statements = StmtList {};
    a.body = &statements;

    Assembler assembler(a, program);
    auto ok = assembler.run(dump);

    if(!ok || context.diagnostics.errorCount()) {
        file->statements = statements;
        return file;
    }

    // The syntax peepholes, which are the same pass the shipping pipeline runs and are deliberately
    // unchanged: one-use inlining, object-literal merging, condition spelling, wide-helper fusion
    // and dead-helper removal all operate on facts that exist only after the tree is built (§6.1).
    file->statements = statements;
    optimizeFile(a);
    return file;
}

} // namespace js
