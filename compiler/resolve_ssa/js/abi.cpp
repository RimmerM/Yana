#include "abi.h"
#include "convert.h"

namespace jsir {

static JsPassing passingOf(JsTarget& target, const mir::Occurrence& occurrence, LocationId where) {
    auto manifest = jsManifest(target, occurrence, where);
    auto& rep = target.reps[manifest.rep.index];

    JsPassing passing;
    passing.erased = !mir::isConcreteType(target.decl, occurrence.type);

    // See `JsPassing::shared`. A loan the callee may only read, which is the whole of what §6.3's
    // rewrite asks of the other side of a call.
    passing.shared = occurrence.contract.access() == mir::Access::Shared;

    /*
     * `Void` occupies no argument position at all - §3.7's "`Void` is omitted".
     *
     * Rather than passing `null`, which is what makes an arity check meaningful: a signature whose
     * unit parameter took a slot would have a caller and a callee that agree about a value neither
     * of them has, and the first one to stop passing it would be a silent shift of everything after.
     */
    if(rep.kind == JsRepKind::Void) {
        passing.kind = JsPassingKind::Omitted;
        passing.rep = manifest.rep;
        passing.positions = 0;
        return passing;
    }

    passing.rep = manifest.rep;

    /*
     * A reference crosses **flat** - `(owner, key)` - except where the position is erased.
     *
     * A descriptor is already a pair of host values, so the object around it buys nothing at a
     * position that can take two arguments and costs one allocation at every mutable borrow that
     * crosses a call. It also takes a level off every read through it: the callee names
     * `owner[key]` where it named `x.$o[x.$k]`. That is the shipping backend's own arrangement -
     * `showWith(v699, ..., v11, "$v", 1)` is its flattened `{$o, $k, $s}`. The scale is a third
     * position only where the content is narrow and something packs - `jsRefCarriesScale` - so
     * `positions` is read off the parts representation rather than written as two.
     *
     * **There is no arity guard** (J84), where the shipping backend declines to flatten a signature
     * past 24 arguments - `Packed.yana`'s `nine`, nine `&Bool`s and 27 arguments. Its reason is
     * native: a callee past 16-24 arguments starts spilling them. V8 passes every JavaScript
     * argument on the stack whatever the count, so there is no cliff for a guard to stand in front
     * of, and the cost of a position is one push against the descriptor's allocation and two
     * property reads. The guard would also make this a question about the whole signature where it
     * is a question about one position, and `forEachPosition`, the thunks and `checkCall` all rely
     * on it being the second. `PackedRef.yana`'s `nine` is 27 positions and gives native's answer.
     *
     * **An erased position keeps the object**, which is J70's rule read one step on: a body
     * compiled once reads a `Cell` argument as `x.$o[x.$k]` whatever is in it, and the substitution
     * decides the content rather than the convention. Flattening there would need the erased body's
     * own parameter list to change with its instantiation, which is the one thing an erased ABI
     * exists not to do. `passing.erased` is read off the declaration, so the two ends agree.
     */
    if(manifest.needsCell) {
        if(passing.erased) {
            passing.kind = JsPassingKind::Cell;
            passing.positions = 1;
            return passing;
        }

        passing.kind = JsPassingKind::Parts;
        passing.rep = jsCellPartsRep(target, manifest.rep);
        passing.positions = target.rep(passing.rep).partCount;
        return passing;
    }

    /*
     * A multi-part value crosses **flat** - §3.7's `Parts -> Parts` row, and §5.4's "no wrapper
     * objects whose consumers all accept parts".
     *
     * `positions` is the part count at a parameter and one at a result, which is the whole of the
     * asymmetry: JavaScript takes as many arguments as it likes and returns one thing. See
     * `carriedRepOf`, where the return convention is argued. A `JsAbi` is a pure function of the
     * declaration, so a caller and a callee cannot disagree about which of the two a position is.
     */
    if(rep.kind == JsRepKind::Parts) {
        passing.kind = JsPassingKind::Parts;
        passing.positions = rep.partCount;
        return passing;
    }

    passing.kind = JsPassingKind::One;
    passing.positions = 1;
    return passing;
}

JsAbiId jsAbiOf(JsTarget& target, DeclPtr<mir::Function> function, LocationId where) {
    if(auto found = target.abiOfFunction.get(U32(function.offset))) return found.unwrap();

    auto& declared = *target.decl[function];
    auto site = where != kNullLocation ? where : declared.source;

    JsAbi abi;

    /*
     * A lifted lambda's environment is argument zero of the declared list, which the resolver put
     * there - so there is no separate leading slot here and the flag says which one it is.
     *
     * What reads it is `FunCode`: a function value's code word may be the function itself exactly
     * when the callee already begins with an environment, and is a thunk otherwise. Stating it on
     * the plan rather than re-deriving it at each `fun.value` is what keeps the two ends agreeing.
     */
    abi.takesEnv = declared.environment != nullptr;

    /*
     * A body compiled once takes its caller's table as argument zero - the erased ABI, which on this
     * target is one host array and an index rather than a run of four-byte cells and an anchor
     * (`lower/erase.cpp` is where the native numbers are, and `codegen/js/place.cpp`'s `tableCell`
     * is the shipping backend's version of this one line).
     *
     * Read off `GenForm` rather than off "has type variables", because the two are different
     * questions and only the first is the one both ends were compiled against: a *specialization*
     * has variables in its generic's signature and none of its own, and `selectGenForms` is the one
     * pass that decides which form a body is in.
     */
    abi.takesGenEnv = declared.form == mir::GenForm::Erased;
    if(abi.takesGenEnv) abi.arity += 1;

    for(U32 i = 0; i < declared.args.size(); i++) {
        auto& argument = *target.decl[declared.args.get(target.decl, i)];
        auto passing = passingOf(target, argument.occurrence, site);
        abi.arity += passing.positions;
        abi.parameters.push(passing);
    }

    abi.result = passingOf(target, declared.result, site);
    if(abi.result.kind == JsPassingKind::Parts) abi.result.positions = 1;

    auto id = target.addAbi(::move(abi));
    target.abiOfFunction.add(U32(function.offset), id);
    return id;
}

JsAbiId jsErasedDropAbi(JsTarget& target) {
    if(target.erasedDropAbi.valid()) return target.erasedDropAbi;

    JsAbi abi;
    abi.parameters.push(JsPassing { JsPassingKind::One, jsErasedRep(target), 1 });
    abi.result = JsPassing { JsPassingKind::Omitted, jsVoidRep(target), 0 };
    abi.arity = 1;

    target.erasedDropAbi = target.addAbi(::move(abi));
    return target.erasedDropAbi;
}

JsAbiId jsTeardownBodyAbi(JsTarget& target, const JsPassing& value) {
    auto key = (U64(value.rep.index) << 24) | (U64(value.positions) << 8) |
               (U64(U8(value.kind)) << 2) | (U64(value.erased) << 1) | U64(value.shared);

    if(auto found = target.teardownBodyAbis.get(key)) return found.unwrap();

    JsAbi abi;
    abi.parameters.push(value);
    abi.result = JsPassing { JsPassingKind::Omitted, jsVoidRep(target), 0 };

    // One, whatever the position's own `positions` says: the body declares exactly one argument -
    // `carriedRepOf` of the passing - because what enters it is the one word the drop site holds.
    abi.arity = 1;

    auto id = target.addAbi(::move(abi));
    target.teardownBodyAbis.add(key, id);
    return id;
}

JsAbiId jsAbiOfType(JsTarget& target, DeclPtr<mir::Type> type, LocationId where) {
    if(!type) return JsAbiId();

    // Keyed on the type handle, in the same table the declarations use: the two key spaces are
    // `mir::Function` and `mir::Type` offsets, which are different arenas, so the high bit says
    // which question was asked rather than a second map existing to answer half of one.
    auto key = U32(type.offset) | (U32(1) << 31);
    if(auto found = target.abiOfFunction.get(key)) return found.unwrap();

    auto declared = target.decl[type];

    // §3.2.1: an erased iterator is the same pair, called across the step's own signature.
    if(declared->kind == mir::Type::Iter) {
        auto id = jsAbiOfType(target, ((mir::IterType*)declared)->step, where);
        if(id.valid()) target.abiOfFunction.add(key, id);
        return id;
    }

    if(declared->kind != mir::Type::Fun) return JsAbiId();

    auto& signature = *(mir::FunType*)declared;

    // A raw code pointer has no pair and no environment word, so the one call shape this target
    // spells does not describe it. Reported by the caller, which has the site.
    if(signature.convention) return JsAbiId();

    JsAbi abi;
    abi.takesEnv = true;

    for(auto parameter: signature.params.contents(target.decl)) {
        auto passing = passingOf(target, parameter.occurrence, where);
        abi.arity += passing.positions;
        abi.parameters.push(passing);
    }

    abi.result = passingOf(target, signature.result, where);
    if(abi.result.kind == JsPassingKind::Parts) abi.result.positions = 1;

    // The environment word, which every call through a pair passes and no parameter of the type
    // stands for. Counted so that an arity check compares against the emitted header.
    abi.arity += 1;

    auto id = target.addAbi(::move(abi));
    target.abiOfFunction.add(key, id);
    return id;
}

/*
 * Which globals are stored boxed - `JsTarget::boxedGlobals`, and the same question `Gen::boxedGlobals`
 * asks of the shipping backend's program.
 *
 * A global whose address is handed to anybody needs an object for a `{$o, $k}` descriptor to name,
 * and a module-level binding is not a property of anything. The test is the demand pass's, read off
 * the whole program instead of one body: every SSA value that denotes the global's storage -
 * following the reborrows, because `%b = load %a` over an address is a loan of the same storage -
 * and the question is whether any use of one hands it out. A load, a store, a projection and a
 * release all name storage and give nothing away; everything else is a second holder.
 *
 * A projection is deliberately not followed, for `planDemand`'s reason: `field.addr` hands out a
 * *different* place, whose own escape is its own question, and what that one needs is the enclosing
 * object rather than a box around the global.
 *
 * A global whose value is already a host object is skipped - see the field's own comment.
 */
static void planBoxedGlobals(JsTarget& target) {
    auto decl = target.decl;
    auto ssa = target.ssa;

    Array<SsaPtr<ssa::Value>> pending;
    IndexSet seen;

    for(auto modulePointer: target.program.modules) {
        auto module = decl[modulePointer];

        for(auto functionPointer: module->functions.contents(decl)) {
            auto function = decl[functionPointer];
            if(!function->reachable || !function->ssa) continue;

            auto from = ssa[function->ssa];

            for(U32 i = 0; i < from->blocks.size(); i++) {
                auto block = from->block(ssa, BlockId(i));
                if(block->deleted) continue;

                for(auto instPointer: block->instructions(ssa)) {
                    auto inst = ssa[instPointer];
                    if(inst->kind != ssa::Inst::GlobalAddress || !inst->results.size()) continue;

                    auto& written = (ssa::InstGlobalAddress&)*inst;
                    if(target.boxedGlobals.contains(U32(written.global.offset))) continue;

                    auto global = decl[written.global];
                    auto manifest = jsManifest(target, global->occurrence, global->source);
                    auto& rep = target.rep(manifest.rep);
                    if(rep.kind == JsRepKind::GcObject || rep.kind == JsRepKind::Void) continue;

                    if(jsAddressEscapes(target, *from, inst->results.get(ssa, 0), pending, seen)) {
                        target.boxedGlobals.add(U32(written.global.offset));
                    }
                }
            }
        }
    }
}

bool planProgram(JsTarget& target) {
    auto& program = target.program;

    /*
     * The lambda environments, **before** anything asks for a representation.
     *
     * `jsRep` memoizes on the type handle, so a record reached as an ordinary field before this set
     * existed would be interned elided and stay that way. This is one walk over the declarations
     * with no representation question in it, which is what makes the ordering safe rather than
     * lucky - see `JsTarget::environmentRecords`.
     */
    for(auto modulePointer: program.modules) {
        auto module = target.decl[modulePointer];

        for(auto functionPointer: module->functions.contents(target.decl)) {
            auto function = target.decl[functionPointer];
            if(!function->reachable || !function->environment) continue;

            target.environmentRecords.add(U32(function->environment.offset));
        }
    }

    // The fields a property witness names, for the same reason and before the same question - see
    // `JsTarget::witnessedFields`. Every environment, reachable or not: keeping one field unpacked
    // that no running body reads costs a property, and missing one costs the program.
    program.forEachEnvironment([&](DeclPtr<mir::GenEnvironment> environment) {
        auto& filled = *target.decl[environment];
        for(U32 index = 0; index < filled.slots.size(); index++) {
            auto witness = filled.slots.get(target.decl, index);
            if(witness.kind != mir::GenSlotKind::Property || !witness.type) continue;

            target.witnessedFields.add((U64(witness.type.offset) << 32) | U64(witness.field));
        }
    });

    /*
     * Every reachable declaration, in module order - the same walk `lower/gen.cpp` opens with, and
     * the same predicate: a function is emitted when it is reachable and has a body.
     *
     * Globals first, because a global's representation is what a body reading one asks for and
     * nothing about a function's plan depends on a body.
     */
    for(auto modulePointer: program.modules) {
        auto module = target.decl[modulePointer];

        for(auto globalPointer: module->globals.contents(target.decl)) {
            auto global = target.decl[globalPointer];
            if(!global->reachable) continue;

            jsManifest(target, global->occurrence, global->source);
        }
    }

    for(auto modulePointer: program.modules) {
        auto module = target.decl[modulePointer];

        for(auto functionPointer: module->functions.contents(target.decl)) {
            auto function = target.decl[functionPointer];
            if(!function->reachable || !function->ssa) continue;

            jsAbiOf(target, functionPointer, function->source);
        }
    }

    planBoxedGlobals(target);

    /*
     * Whether any lambda environment in this program owes an authored teardown -
     * `JsTarget::closureOwesTeardown`, and the reason it is answered here rather than at a drop.
     *
     * A fixpoint over one boolean, which is what a capture that is *itself* a closure needs: a
     * lambda holding another lambda owes something exactly when that one does, so the predicate
     * reads the flag it is computing. Monotone and over one bit, so a second pass that changes
     * nothing is the end of it - which is why this is a loop with a bound of two rather than a
     * worklist.
     */
    for(U32 pass = 0; pass < 2 && !target.closureOwesTeardown; pass++) {
        for(auto modulePointer: program.modules) {
            auto module = target.decl[modulePointer];

            for(auto functionPointer: module->functions.contents(target.decl)) {
                auto function = target.decl[functionPointer];
                if(!function->reachable || !function->environment) continue;
                if(!jsOwesTeardown(target, function->environment)) continue;

                target.closureOwesTeardown = true;
                break;
            }
        }
    }

    return !target.reported;
}

} // namespace jsir
