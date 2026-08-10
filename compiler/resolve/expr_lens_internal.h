#pragma once

/*
 * Lenses and the two constructs built on them, shared between the files they are split across.
 *
 * The interface is expr.h, as it is for every other part of the expression resolver. What is here
 * is the seam between four translation units that are one feature seen from four sides:
 *
 *  - expr_lens.cpp - the lens itself. What a lens declaration means, what `yield` is, and how the
 *                    rest of a block becomes the continuation a call site left out.
 *  - expr_cont.cpp - the continuation, as a function. Lifting a block into one, the environment it
 *                    captures, the outcome type it hands back, and where a `return` inside one goes.
 *  - expr_iter.cpp - `for x in c`, which is a lens call whose continuation is the loop body.
 *  - expr_try.cpp  - `?` and `?.`, which are a lens's skipping half without the lens: selecting the
 *                    `Try` instance a carrier has, and threading the two cases through.
 *
 * Only what has a caller in another one of those four is here.
 */

#include "expr.h"

/*
 * The `Try` instance a skipping lens's result carries - Analysis-Lens.md §3.2, §7.1.
 *
 * `class Try(m -> a, e)` is keyed on `m` alone, so the two type arguments nothing constrains are
 * asked for as holes and read back off the instance that matched. That is the load-bearing half of
 * reading B: the resolver never has to relate a return type to a type *constructor* applied to a
 * variable - the higher-kinded machinery Implementation-Generics.md part 7 fences off - because the
 * question it actually needs answered is "which case of the wrapper means the continuation ran", and
 * only an instance can answer that.
 *
 * The keying is the class's declared functional dependency rather than a rule this file knows, so
 * it is enforced where instances are written: two `Try` instances for one carrier that disagree
 * about what it proceeds with are a rejected declaration.
 *
 * Selected twice for one lens, and deliberately: once at the declaration, where `m` still mentions
 * the lens's own variables and what is being checked is that the instance's `a` *is* the
 * continuation's result; and once per call site, where `m` is concrete and what is wanted is the
 * implementation to call.
 */
struct TrySelection {
    ModulePtr<ClassInstance> instance = nullptr;
    TypeList instanceArgs;

    // The instance's own second and third arguments, under the bindings the head match made: what
    // the carrier holds when the continuation ran, and what it holds when it did not.
    TypePtr proceeds = nullptr;
    TypePtr reason = nullptr;

    U16 toOutcome = 0;

    explicit operator bool() const { return instance != nullptr; }
};

// -- expr_lens.cpp -----------------------------------------------------------------------------

// The name a lifted continuation is compiled under. Unique per module and not addressable in
// source; what it has to do is say where it came from in a dump.
StringId continuationFunctionName(Module& module);

// One step of a skipping lens: the outcome type its continuation hands back.
TypePtr stepType(Module& module, TypePtr carried, LocationId source);

// -- expr_cont.cpp -----------------------------------------------------------------------------

// The parameters a lifted continuation must have, read off the callee this call is to. False when
// the callee is not a lens, or its continuation parameter is not one this call site can fill.
bool continuationSignature(ExprResolver& resolver, Module& module, ModulePtr<Function> callee,
                           Buffer<ResolvedArg> args, LocationId source, Array<FunArg>& out);

// The same, for a class member, which is not desugared and so has no continuation parameter to read
// the shape off. False after reporting a written result these type arguments cannot make concrete.
bool classContinuationSignature(ExprResolver& resolver, Module& module, ClassMatch& match,
                                LocationId source, Array<FunArg>& out);

// -- expr_try.cpp ------------------------------------------------------------------------------

bool selectTry(Module& module, Function& function, TypePtr carrier, TrySelection& out);
