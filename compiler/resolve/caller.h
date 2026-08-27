#pragma once

#include "type.h"

// `Program`, `Module` and the pointer aliases come from type.h. These four are defined below this
// header in the include order - `Arg` and `Value` by inst.h, which includes this one.
struct Arg;
struct ExprResolver;
struct Function;
struct ResolvedArg;
struct Value;

/*
 * `@caller` - Design-Test.md §11.1's F2.
 *
 * A parameter the *compiler* fills, at every call that leaves it out, with a constant about the
 * call site. Everything the feature is made of is here: what the marker may say (CallerFill), what a
 * declaration records once it has been checked (CallerInfo), the check itself (resolveCallerFill)
 * and the two fills a call site builds (callerFillValue).
 *
 * The one property the whole thing rests on is that the fill happens **where the position was
 * omitted**. `check(n > 0)` inside a helper that passes its own `Site` on explicitly is never
 * filled here, so the helper reports the line that called *it* - which is Rust's `#[track_caller]`,
 * obtained because "a position no argument reached is filled by its default" is already how
 * defaults work. Nothing about selection, arity or conversion needed a rule of its own.
 *
 * The compiler knows nothing about the library's `Site` record - it lives in `Test` and not in
 * `Core` - so what a declaration may ask for is matched by *field name* against a fixed vocabulary
 * rather than against a type the resolver holds a pointer to.
 */

/*
 * What the compiler puts in a `@caller` position the call site left out.
 *
 * `Site` is the call's own location; `Source` is the text of the expression that reached some other
 * parameter. Both are constants, and both are built at the *call's* source rather than at the
 * declaration's - which is the whole of what separates this from an ordinary default.
 */
enum class CallerFill: U8 {
    None,
    Site,
    Source,
};

/*
 * The marker as a declaration carries it, once resolveCallerFill has accepted it.
 *
 * A struct rather than two fields of `Arg` because the second is only meaningful for one of the
 * fills: `source` is the *parameter index* whose written expression a `Source` fill is the text of,
 * and means nothing for a `Site`. Set only where the fill was checked and found buildable - a
 * marker that failed leaves `None` behind, since a position every call site may leave out and
 * nothing fills is worse than the diagnostic.
 */
struct CallerInfo {
    CallerFill fill = CallerFill::None;
    U16 source = 0;

    bool isCallerFilled() const { return fill != CallerFill::None; }
};

/*
 * A `@caller` parameter, checked against what the compiler is able to put in it.
 *
 * `sourceName` is the parameter named by `@caller(source: p)`, and 0 for the bare location form,
 * which is what tells the two fills apart. Called once every parameter of the signature exists, so
 * that a `@caller(source: p)` finds a `p` written after it - see resolveSignature.
 */
void resolveCallerFill(Module& module, Function& function, Arg& declared, StringId sourceName,
                       LocationId source);

/*
 * The location of a call, as the record the declaration asked for - `@caller at: Site`.
 *
 * Built field by field from the call's own node. The strings are literals, so a site is some
 * constant data and no runtime work: a string literal is a borrowed run over a constant global and
 * its teardown is nothing at all.
 *
 * `containing` names the function the site is *inside*, and defaults to the one being resolved -
 * which is the answer for a `@caller` fill and the wrong one for the synthesized test entry, whose
 * sites belong to the declarations it is building cases out of rather than to itself.
 */
ModulePtr<Value> buildCallerSite(ExprResolver& resolver, TypePtr type, LocationId at,
                                 StringId containing = StringId());

/*
 * What a `@caller` position the call site left out is filled with.
 *
 * `at` is the call's own source. `args` is the call's argument list as it stands before the
 * defaults are materialized, which is where a `Source` fill reads the span of the expression that
 * reached the parameter it names - see ResolvedArg::written.
 */
ModulePtr<Value> callerFillValue(ExprResolver& resolver, const Arg& parameter, LocationId at,
                                 Buffer<ResolvedArg> args);
