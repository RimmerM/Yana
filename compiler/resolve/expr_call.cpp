#include "expr.h"
#include "solve.h"
#include "complete.h"
#include "witness.h"
#include "generic.h"
#include "name.h"
#include "index.h"

/*
 * Calls, operators, and typeclass instance selection.
 *
 * Every operator in the language is an ordinary call, and every arithmetic and comparison
 * operator is a class function: `+` is `Num.+`, `==` is `Eq.==`, and a conversion is
 * `Widen.widen` or `Narrow.narrow`. Selecting one is two steps - work out what the class's type
 * variables must be here, then find the instance for them - and both steps run against the class
 * signature, which is why a variable appearing only in the result type is inferable exactly like
 * one appearing in an argument. That is what makes `round(x) :: Long` pick an instance by its
 * return type.
 *
 * The rule for which position gets to decide is deliberately one-directional: arguments bind
 * first, and the expected result type only fills in variables the arguments left open. Design.md
 * asks for bottom-up, left-to-right inference with no backtracking, and this is that rule - it
 * keeps `1 + 2 :: Long` an Int addition widened afterwards rather than silently becoming a Long
 * addition, while still letting `widen(x) :: Long` work at all.
 *
 * A literal argument is the one thing that leaves a variable open without leaving it unbound. It
 * binds a literal variable, which is not a decision, so the expected result may still refine it -
 * `inc(1) :: Long` is a Long computation, while `inc(x) :: Long` on an Int `x` is not.
 *
 * A name may be declared by more than one class and by one class at more than one arity, so
 * emitCall selects out of an overload set rather than off a single signature. Design.md's
 * Overloading section states the five rules it implements: the set is keyed by (name, arity),
 * candidates are not ranked, a constraint declared by the enclosing function is what picks a class
 * in generic code, ambiguity is resolved by writing `Class.method` and never by a tiebreak, and a
 * plain function is one member of the set rather than a shadow over it.
 */

// R4 resolves ambiguity by qualification and never by a tiebreak, so an ambiguity diagnostic has to
// say which qualified names there are to choose between - `Integral.and or Logic.and`. Leaving the
// author to go and find the classes themselves is the difference between a rule and a puzzle.
static String describeQualified(Context& context, GlobalBase global, StringId name,
                               Buffer<GlobalPtr<TypeClass>> classes) {
    StringBuilder text;

    for(Size i = 0; i < classes.length; i++) {
        if(i) text.append(i + 1 == classes.length ? " or "_v : ", "_v);
        text << context.findName(global[classes[i]]->name) << '.' << context.findName(name);
    }

    return text.string();
}

/*
 * The one normalized-call builder.
 *
 * Positional arguments, named arguments and omitted defaults are three spellings of the same thing -
 * "this parameter is filled by that" - and this is the only place any of them is read. Everything a
 * call site does downstream works in *parameter* order and cannot tell which spelling produced the
 * list it was given: matching, selection, the conventions, the intrinsics and the four emission
 * forms all index a parameter and find its argument there.
 *
 * The rule, in the order it is applied:
 *
 *  - A positional argument fills the next parameter, counting from the first.
 *  - A named one fills the parameter of its name, wherever that is.
 *  - **A positional argument may not follow a named one.** Nothing forces this - a name is an
 *    absolute answer and "the next parameter" could go on counting past it - but what it would count
 *    past is a question every reader of the call would have to ask, and the rule that makes the
 *    answer unnecessary costs a call site one reordering. doc/spec/expressions.md states it.
 *  - A parameter no argument reached is filled by its default, and is an error where it has none.
 *
 * `report` is what separates asking from deciding. A candidate that cannot be normalized is simply
 * not one - selection has others to try, and reporting here would name the first candidate rather
 * than the call - so only the point that has run out of candidates says anything, which is the same
 * place every other diagnostic about *choosing* a callee lives.
 */
bool ExprResolver::mapArguments(ModulePtr<Function> signature, Buffer<const StringId> names, Size written,
                                Size supplied, StringId callName, LocationId source, bool report,
                                ArgMapping& out) {
    auto declaration = local[signature];
    auto fillable = declaration->args.size() >= supplied ? declaration->args.size() - supplied : 0;

    out.sources.clear();
    out.parameters.clear();

    for(Size i = 0; i < fillable; i++) out.sources.push(ArgMapping::kDefaulted);

    auto anyNamed = false;
    for(Size i = 0; i < written && i < names.length; i++) anyNamed = anyNamed || names[i] != 0;

    Size required = 0;
    for(Size i = 0; i < fillable; i++) {
        if(!local[declaration->args.get(local, i)]->hasDefault()) required++;
    }

    /*
     * The count diagnostic, which is two messages because a signature with defaults takes a *range*
     * of argument counts and one with none takes a number. Saying "takes 2 arguments" of `fn
     * open(path: String, mode: Mode = Mode.Read)` would be a fact about a call nobody could write.
     *
     * Said only where the count really is what is wrong - too many, or fewer than the signature
     * requires, in a call that wrote no names. A call whose count is inside the range and whose
     * *order* went wrong is told which parameter it left out, and so is one that used names: for
     * either of those the count is the least useful thing anyone could be told.
     */
    auto reportCount = [&]() {
        if(required == fillable) {
            context.diagnostics.error("%@ takes %@ arguments but was given %@"_v, source,
                                      context.findName(callName), U32(fillable), U32(written));
        } else {
            context.diagnostics.error("%@ takes between %@ and %@ arguments but was given %@"_v, source,
                                      context.findName(callName), U32(required), U32(fillable),
                                      U32(written));
        }
    };

    if(written > fillable) {
        if(report) reportCount();
        return false;
    }

    Size position = 0;
    auto named = false;

    for(Size i = 0; i < written; i++) {
        auto name = i < names.length ? names[i] : StringId(0);

        if(!name) {
            if(named) {
                if(report) {
                    context.diagnostics.error("a positional argument cannot follow a named one - which parameter it fills would depend on which ones the names before it took. Write this one as `name: value` too, or move it in front of them"_v,
                                              source);
                }

                return false;
            }

            out.parameters.push(U16(position));
            out.sources[position] = U16(i);
            position++;
            continue;
        }

        named = true;

        auto found = fillable;
        for(Size p = 0; p < fillable; p++) {
            if(local[declaration->args.get(local, p)]->name != name) continue;

            found = p;
            break;
        }

        if(found == fillable) {
            if(report) {
                context.diagnostics.error("%@ has no parameter named %@"_v, source,
                                          context.findName(callName), context.findName(name));
            }

            return false;
        }

        if(out.sources[found] != ArgMapping::kDefaulted) {
            if(report) {
                context.diagnostics.error("%@ is given twice in this call"_v, source, context.findName(name));
            }

            return false;
        }

        out.parameters.push(U16(found));
        out.sources[found] = U16(i);
    }

    // A position nothing reached and nothing declared. Named one at a time, because a call that left
    // two out has two things to fix and naming only the first would hide the second.
    auto complete = true;

    for(Size i = 0; i < fillable; i++) {
        auto parameter = local[declaration->args.get(local, i)];
        if(out.sources[i] != ArgMapping::kDefaulted || parameter->hasDefault()) continue;

        complete = false;
        if(!report) return false;

        // Fewer arguments than the signature requires, written positionally: a miscount, and the
        // count is the whole of it.
        if(!anyNamed && written < required) {
            reportCount();
            return false;
        }

        /*
         * A position the count reached and the order did not.
         *
         * `fn mid(a: Int = 1, b: Int)` called as `mid(4)` is the case: one argument is a legal
         * number of them, and it filled `a` because that is what a positional argument does. What
         * fixes it is the name, so the name is what the message hands over - saying "takes between 1
         * and 2 arguments but was given 1" would be a count that is both true and satisfied.
         */
        auto reachable = false;
        for(Size p = 0; p < i; p++) reachable = reachable || local[declaration->args.get(local, p)]->hasDefault();

        if(reachable && !anyNamed) {
            context.diagnostics.error("argument %@ of %@ was not given - a parameter before it has a default, so a positional argument fills that one instead. Write it as `%@: value`"_v,
                                      source, context.findName(parameter->name),
                                      context.findName(callName), context.findName(parameter->name));
            continue;
        }

        context.diagnostics.error("argument %@ of %@ was not given, and it has no default"_v, source,
                                  context.findName(parameter->name), context.findName(callName));
    }

    return complete;
}

/*
 * The same question asked of a function value.
 *
 * A function *type* carries parameter names - `(count: Int) -> Bool` names its parameter as much as
 * a declaration does - so a named argument reaches one exactly as it reaches a declared function.
 * What a type cannot carry is a default: a default is a constant belonging to one declaration, and
 * two functions of the same type do not agree about it. So a call through a value fills every
 * position, which is what makes this a shorter rule rather than a different one.
 */
bool ExprResolver::mapValueArguments(FunType* signature, Buffer<const StringId> names, Size written,
                                     LocationId source, bool report, ArgMapping& out) {
    auto fillable = signature->args.size();

    out.sources.clear();
    out.parameters.clear();

    for(Size i = 0; i < fillable; i++) out.sources.push(ArgMapping::kDefaulted);

    if(written != fillable) {
        if(report) {
            context.diagnostics.error("this function takes %@ arguments but was given %@"_v, source,
                                      U32(fillable), U32(written));
        }

        return false;
    }

    Size position = 0;
    auto named = false;

    for(Size i = 0; i < written; i++) {
        auto name = i < names.length ? names[i] : StringId(0);

        if(!name) {
            if(named) {
                if(report) {
                    context.diagnostics.error("a positional argument cannot follow a named one - which parameter it fills would depend on which ones the names before it took. Write this one as `name: value` too, or move it in front of them"_v,
                                              source);
                }

                return false;
            }

            out.parameters.push(U16(position));
            out.sources[position] = U16(i);
            position++;
            continue;
        }

        named = true;

        auto found = fillable;
        for(Size p = 0; p < fillable; p++) {
            if(signature->args.get(global, p).name != name) continue;

            found = p;
            break;
        }

        if(found == fillable) {
            if(report) {
                context.diagnostics.error("this function has no parameter named %@"_v, source,
                                          context.findName(name));
            }

            return false;
        }

        if(out.sources[found] != ArgMapping::kDefaulted) {
            if(report) {
                context.diagnostics.error("%@ is given twice in this call"_v, source, context.findName(name));
            }

            return false;
        }

        out.parameters.push(U16(found));
        out.sources[found] = U16(i);
    }

    for(Size i = 0; i < fillable; i++) {
        if(out.sources[i] != ArgMapping::kDefaulted) continue;

        if(report) {
            context.diagnostics.error("argument %@ of this function was not given, and a function value carries no defaults"_v,
                                      source, context.findName(signature->args.get(global, i).name));
        }

        return false;
    }

    return true;
}

void ExprResolver::normalizeArguments(const ArgMapping& mapping, Buffer<ResolvedArg> args, ArgList& out) {
    out.clear();

    for(auto source: mapping.sources) {
        out.push(source == ArgMapping::kDefaulted || source >= args.length
            ? ResolvedArg::defaulted() : args[source]);
    }
}

void ExprResolver::materializeDefaults(ModulePtr<Function> signature, LocationId source, ArgList& args) {
    auto declaration = local[signature];

    for(Size i = 0; i < args.size() && i < declaration->args.size(); i++) {
        if(!args[i].isDefault()) continue;

        auto parameter = local[declaration->args.get(local, i)];

        /*
         * A position selection marked as defaulted whose parameter declares none.
         *
         * Unreachable through the mapping, which is what refuses such a call in the first place, and
         * kept as a failure rather than an assertion because the one thing it must not become is a
         * call with a hole in it: `positionalUnit` would then take the absence for a unit argument
         * and hand the callee whatever the calling convention left in place.
         */
        if(!parameter->hasDefault()) {
            args[i] = ResolvedArg::failed();
            continue;
        }

        args[i] = constantValue(parameter->defaultValue, source);
    }
}

void ExprResolver::collectArgNames(ast::ParseList<ast::TupArg> arguments, ArgNames& out) {
    out.clear();
    for(auto arg: arguments.contents(parse)) out.push(arg.name);
}

/*
 * Which arguments of a call are deferred, decided from the overload set.
 *
 * The order this has to happen in is what makes it a question about the set rather than about a
 * callee. A `@lazy` argument must not be evaluated, and selection needs the argument *types* to pick
 * an overload, so the decision comes before there is a callee to read it off - which leaves only
 * what every candidate of one (name, arity) has in common. Design.md's rule that strictness is fixed
 * by the class signature rather than by the instance is exactly this, stated from the other side.
 *
 * Two candidates that disagree are therefore a declaration error rather than a call-site one, but
 * it is only detectable where the two are visible together, which is here.
 */
/*
 * The `@lazy` states of one candidate, over the positions the *call site* writes.
 *
 * Written positions and not parameters, which is the difference a named argument makes: `f(b: x)`
 * defers its one written argument exactly when `b` is the `@lazy` parameter, wherever `b` is
 * declared. So the states are read *through* the mapping, which is also what makes the answer
 * comparable between two candidates that declare their parameters in different orders.
 *
 * Every list is exactly `arity` long, because the lists are compared with each other and the
 * candidates do not all have the same number of parameters. A `for` loop is where they differ: its
 * plain half is a desugared `iter fn`, so it carries the continuation the loop supplies, while a
 * class `iter fn` is not desugared and declares only what the loop writes. Comparing the whole of
 * each made those two disagree about a position neither of them has an opinion on, and reported the
 * declarations of a perfectly good name as inconsistent - which the `lazyNames` fast path hid until
 * some *other* declaration of the name, of any arity or kind, put it in the set.
 *
 * False for a candidate this call site cannot reach at all, which has nothing to say about what the
 * call evaluates.
 */
static bool lazyStatesOf(ExprResolver& resolver, ModulePtr<Function> signature, const OverloadSet& set,
                         Size supplied, ArgList& out) {
    ArgMapping mapping;
    if(!resolver.mapArguments(signature, toBuffer(set.names), set.arity, supplied, set.name,
                              kNullLocation, false, mapping)) {
        return false;
    }

    auto local = resolver.local;
    auto declaration = resolver.local[signature];

    out.clear();

    for(Size i = 0; i < set.arity; i++) {
        auto parameter = i < mapping.parameters.size() ? mapping.parameters[i] : ArgMapping::kDefaulted;

        out.push(parameter < declaration->args.size() &&
                 local[declaration->args.get(local, parameter)]->isLazy()
            ? ResolvedArg::deferred() : ResolvedArg());
    }

    return true;
}

static bool sameStates(const ArgList& a, const ArgList& b) {
    if(a.size() != b.size()) return false;
    for(Size i = 0; i < a.size(); i++) {
        if(a[i].isDeferred() != b[i].isDeferred()) return false;
    }

    return true;
}

// Every position strict, which is what a name with no `@lazy` parameter anywhere behind it means.
// The entries themselves are empty: what each one is is decided when the argument is resolved.
static void allStrict(Size arity, ArgList& out) {
    out.clear();
    for(Size i = 0; i < arity; i++) out.push(ResolvedArg());
}

// Fills `set.strictness` from the candidates already in `set`. No lookup of its own: what it asks
// is a question about the set, so the set is what it is handed.
static void computeStrictness(ExprResolver& resolver, OverloadSet& set, LocationId source) {
    auto& module = resolver.module;
    auto& context = resolver.context;
    auto local = resolver.local;
    auto global = resolver.global;
    auto& out = set.strictness;

    // The negative answer, which is every call in a program but a handful - see Program::lazyNames.
    if(!module.program.lazyNames.contains(set.name)) {
        allStrict(set.arity, out);
        return;
    }

    ArgList candidateStates;
    auto seen = false;
    auto conflict = false;

    auto consider = [&](ModulePtr<Function> signature, Size supplied) {
        if(!lazyStatesOf(resolver, signature, set, supplied, candidateStates)) return;

        if(seen && !sameStates(candidateStates, out)) conflict = true;
        else replaceContents(out, candidateStates);

        seen = true;
    };

    /*
     * `direct` already serves this call - see gatherOverloads - and its continuation, where it has
     * one, is the loop's rather than the call site's. The class half is not filtered by arity there,
     * so it is filtered here, and by the same rule everything else filters by: strictness is what
     * the candidates this call site can actually reach agree on, and one whose parameters its
     * arguments cannot fill is not one of them. `lazyStatesOf` answers no for those.
     *
     * `wrongKind` is deliberately not asked. A candidate of the other kind is never the callee - it
     * exists to be named in a diagnostic - so what it declares about evaluation is not this call's
     * business, and letting it disagree would reject a call it could not have served.
     */
    if(set.direct) consider(set.direct, set.shape.supplied);

    for(auto& candidate: set.candidates) {
        auto entry = global[candidate.typeClass]->functions.get(global, candidate.index);
        if(!entry.fun) continue;

        // A class member is not desugared, so it declares exactly what the call site writes - see
        // CallShape::supplied, which is the plain half's business alone.
        consider(entry.fun, 0);
    }

    if(conflict) {
        context.diagnostics.error("the declarations of %@ disagree about which arguments are `@lazy`, so a call to it cannot tell what to evaluate - strictness is part of the signature and every overload of one name and arity has to declare the same one"_v,
                                  source, context.findName(set.name));
        allStrict(set.arity, out);
        return;
    }

    // A name that reached no candidate of this arity, or one whose candidates declare nothing lazy.
    // Either way the call is strict, and the list has to be the length the call site indexes.
    auto any = false;
    for(auto& state: out) any = any || state.isDeferred();

    if(!seen || !any) {
        allStrict(set.arity, out);
        return;
    }

    // Already exactly `set.arity` long, whichever candidate it came from - lazyStatesOf makes every
    // list that length so that comparing two of them is a question about the positions and not
    // about how many parameters each candidate happens to declare.
}

void ExprResolver::gatherOverloads(StringId name, Size arity, LocationId source, LocationId nameSource,
                                   OverloadSet& out, CallShape shape, Buffer<const StringId> names) {
    out.name = name;
    out.arity = arity;
    out.nameSource = nameSource;
    out.shape = shape;

    replaceContents(out.names, names);

    /*
     * Both halves, looked up at the same location - and the occurrence recorded at a different one.
     *
     * `source` is where the set is looked up from, which decides what is visible and is where an
     * ambiguity between two imports is reported. `nameSource` is the name the author wrote, which is
     * where the index should hold the answer: recording it against the whole call would make
     * find-references report the call and the name as two hits on the same name. A synthesized call
     * has no such name, so it records nothing at all - which is what `kNullLocation` means here, and
     * why the two have to be passed separately rather than one standing in for the other. See
     * findFunction and resolve/index.h.
     */
    /*
     * A dot-call whose receiver's type declares a function of this name in its own namespace -
     * `String.reserve` for `s.reserve(n)`, where a bare `reserve` is `Array`'s.
     *
     * It is the plain half rather than joining it, which keeps R1's "at most one plain function"
     * intact and states the precedence: a name declared *for this type* is what a dot on a value of
     * the type means, and a plain function of the same name is what it means for every other type.
     * That is the whole reason the namespace exists - `reserveString` was the previous spelling of
     * this - and it is the one place two plain functions could otherwise reach one call.
     *
     * Resolved by name rather than taken from the registry, so that `pub`, the import lists and
     * `hiding` decide here exactly as they do for the written spelling `String.reserve(s, n)`.
     *
     * **Asked before the bare name and not after it**, which is not a tidying. `search` reports an
     * ambiguity where a name is visible through more than one import, and reports it *as it looks* -
     * so a bare lookup performed first has already produced the diagnostic by the time a type method
     * could have replaced its answer. That is the state `store` and `exchange` were in: `Atomic` and
     * `Native` both declare them, so `c.store(7, StoreRelease)` was refused as ambiguous even though
     * the receiver decides it, and every call in the library and its fixtures had to be written out
     * qualified. Looking the method up first means the bare name is never asked about, and there is
     * nothing to be ambiguous.
     *
     * The fallback is unconditional: a type method that is not visible here - not `pub`, hidden by
     * an import list - leaves the bare name as the answer exactly as before, ambiguity and all.
     */
    ModulePtr<Function> plain = nullptr;

    if(shape.receiver) {
        if(auto method = findTypeMethod(module, shape.receiver, name)) {
            plain = findFunction(module, method, source, nameSource);
        }
    }

    if(!plain) plain = findFunction(module, name, source, nameSource);

    /*
     * A plain function is a candidate when it is of this call's kind and the call site's arguments
     * reach its parameters - the kind being what separates a call from a loop, and the arguments
     * being where arity, the names and the defaults are all one question. A loop's callee declares
     * one parameter more than the loop writes, which is `shape.supplied`.
     *
     * "Takes what the call site writes" used to be a count, and a default is exactly what makes it
     * not one: `fn open(path: String, mode: Mode = Mode.Read)` is the candidate for a call with one
     * argument and for a call with two. Anything else of the name is kept where the diagnostic can
     * reach it and nothing else can - see reportMismatched, which asks the same question again with
     * its answers turned on.
     */
    ArgMapping mapping;

    if(plain && (!shape.requiresKind || local[plain]->funKind == shape.kind) &&
       mapArguments(plain, names, arity, shape.supplied, name, source, false, mapping)) {
        out.direct = plain;
    } else {
        out.mismatched = plain;
    }

    ClassFunList found;
    findClassFunctions(module, name, source, found);

    for(auto& candidate: found) {
        auto entry = global[candidate.typeClass]->functions.get(global, candidate.index);
        auto signature = entry.fun ? local[entry.fun] : nullptr;
        if(!signature) continue;

        // Not narrowed by arity - `matchClassFun` states that rule, and the candidates that fail it
        // are what "no class function %@ accepts (%@)" lists. Narrowed by kind, because a candidate
        // of the other kind is a different answer entirely. See OverloadSet::wrongKind.
        (signature->funKind == shape.kind ? out.candidates : out.wrongKind).push(candidate);
    }

    computeStrictness(*this, out, source);
}

/*
 * The signature this call's arguments are pushed down against, which is the sole candidate's or
 * nobody's.
 *
 * A candidate's parameter types are the expected type of each argument only when there is nothing
 * else the call could be: pushing one candidate's types in decides the call before selection runs.
 * That is what lets `f(Nothing)` know which `Maybe` it is building, and it is also what a set of
 * two candidates cannot have.
 *
 * Both call sites ask this, and it is the rule the loop's own version got wrong twice - once by
 * pushing the first class signature down whenever there was one, and once by pushing the first of
 * several. Two class candidates are the same case as a mixed set: they agree about the *conventions*
 * (they would have to, to be callable by one syntax at all) but nothing makes them agree about a
 * concrete position, and it is the concrete positions that get pushed down.
 *
 * Counted over the candidates this call site can reach, since one whose parameters its arguments
 * cannot fill is not one of the things the call could be.
 */
ModulePtr<Function> ExprResolver::pushdownSignature(const OverloadSet& set) {
    auto sole = set.direct;
    Size count = set.direct ? 1 : 0;

    ArgMapping mapping;

    for(auto& candidate: set.candidates) {
        if(!candidate.typeClass) continue;

        auto entry = global[candidate.typeClass]->functions.get(global, candidate.index);
        if(!entry.fun) continue;

        // A class member is not desugared, so it fills every parameter it declares - see
        // CallShape::supplied.
        if(!mapArguments(entry.fun, toBuffer(set.names), set.arity, 0, set.name, kNullLocation,
                         false, mapping)) {
            continue;
        }

        sole = entry.fun;
        count++;
    }

    return count == 1 ? sole : nullptr;
}

/*
 * A fixed-array parameter's *shape*, pushed into an array literal where a generic signature could
 * otherwise tell it nothing.
 *
 * The refusal above is exact and stands: a generic function's parameter types are what the arguments
 * are being resolved to decide, so pushing one down would answer a question the argument was asked.
 * A `[T *n]` parameter has one part that is not such a question. Whether `[1, 2, 3]` builds a
 * growable `Array(T)` or a fixed `[T *3]` is "chosen by the expected type and by nothing else" - see
 * resolveArray - and the *count* is not chosen by it at all. That is resolveFixedArray's own
 * doctrine said one step further: where the type states a count the two are compared, and where it
 * states a variable the literal's length is the only opinion there is.
 *
 * So what goes down is the parameter's element type at the literal's own length, and only where the
 * element decides nothing either: an element mentioning a variable would be exactly the pushdown
 * this declines. `fn (n: Int) firstOf(xs: [Int *n])` accepts `firstOf([7, 8, 9])` because of this,
 * and `Core.shuffle`'s lane pattern is what asked for it.
 */
TypePtr ExprResolver::arrayShapeFor(const Arg& parameter, const ast::Expr& argument) {
    if(argument.kind != ast::Expr::Array || parameter.isMutableBorrow()) return nullptr;

    auto declared = parameter.declaredType();
    if(!declared || global[declared]->kind != Type::Array) return nullptr;

    auto& array = *(ArrayType*)global[declared];
    if(isGeneric(global, array.content)) return nullptr;

    // A count the parameter states is the ordinary check and the declared type carries it; a count
    // it leaves to a variable is the one the literal answers.
    if(writtenCount(global, array.count)) return declared;

    // By value, because `size` is not a const member - the list is a handle into the parse arena.
    auto items = argument.arr;
    return resolveFixedArrayType(module, array.content, U32(items.size()), argument.source);
}

// The instance of `typeClass` that serves `args`, and what selecting it bound its own type
// variables to.
ModulePtr<ClassInstance> ExprResolver::selectInstance(GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                                                      TypeList& instanceArgs) {
    auto match = matchInstance(module, typeClass, args);
    replaceContents(instanceArgs, match.args);
    return match.instance;
}

// See the declarations in expr.h for what each of these is for; both exist because the rule they
// state was written out at four call sites, and three of them stated it differently.
ResolvedArg ExprResolver::convertArgument(const ResolvedArg& arg, TypePtr declared, LocationId source) {
    if(arg.isValue()) return convert(arg.value, declared, source);
    if(arg.state != ArgResult::Unit) return arg;

    /*
     * A `{}` where the position wanted something.
     *
     * Reported here because this is the first place that knows both halves: selection has settled on
     * a callee, so the parameter type is known, and the argument still says it carries nothing. A
     * position that is still a type variable is not this - the erased path substitutes nothing for
     * it - and an error type has had its diagnostic already.
     */
    if(!declared || isUnit(global, declared) || isGeneric(global, declared) ||
       global[declared]->kind == Type::Error) {
        return arg;
    }

    context.diagnostics.error("this argument carries nothing, and the parameter it fills is declared %@ - `{}` is a value of the empty type and converts to nothing else"_v,
                              source, describeType(context, global, declared));
    return ResolvedArg::failed();
}

ModulePtr<Value> ExprResolver::positionalUnit(ModulePtr<Value> value, TypePtr declared, LocationId source) {
    if(value || !isUnit(global, declared)) return value;
    return allocate(declared, source);
}

/*
 * An argument list restated in a callee's signature, read at the types this call decided.
 *
 * The step between selecting a callee and emitting a call to it, and it is not the same thing as
 * handing the arguments over: what comes out is still an ArgList, because it is passed on to
 * whichever *form* the call turns out to take - an intrinsic expanded here, an erased call, or a
 * specialization called directly - and each of those applies the conventions itself. A generic call
 * and a parametric instance's implementation are the two that need it, and the types they read the
 * signature at are the only difference between them.
 *
 * Two things it does not convert, and they are the same rule twice. A deferred argument is not a
 * value yet, so what it converts to is decided where it is forced. And a `&` parameter's argument is
 * left exactly as written: what it is handed is a *borrow* of the caller's storage, and creating
 * that is borrowArgument's job in whichever form this becomes. Converting first would build a
 * read-only temporary and then ask for a mutable borrow of it, which is how `sort(&xs)` on an owned
 * array reported that a `let &` binding was not mutable.
 */
void ExprResolver::substituteArguments(ModulePtr<Function> callee, Buffer<ResolvedArg> args,
                                       Buffer<TypePtr> typeArgs, LocationId source, ArgList& out) {
    auto function_ = local[callee];

    out.clear();

    for(Size i = 0; i < args.length; i++) {
        auto declared = i < function_->args.size() ? local[function_->args.get(local, i)] : nullptr;

        if(!declared || args[i].isDeferred() || declared->isMutableBorrow()) {
            out.push(args[i]);
            continue;
        }

        /*
         * The positional rule, kept here as well as at the call itself.
         *
         * The erased form needs it for a second reason on top of the shift: lowering reads the
         * *concrete* type off this argument to size the storage it hands over, so an argument that
         * is not here has no type to read. A position declared as a type variable exists whatever it
         * was substituted with - `{}` included - which is exactly the case that arrives with nothing
         * to put in it. See positionalUnit.
         */
        auto wanted = substituted(declared->declaredType(), typeArgs, source);
        auto value = convertArgument(args[i], wanted, source);

        out.push(positionalUnit(value.value, wanted, source));
    }
}

ModulePtr<Value> ExprResolver::emitInstanceCall(Module& site, ModulePtr<ClassInstance> instance,
                                                Buffer<TypePtr> instanceArgs, U16 index,
                                                Buffer<ResolvedArg> args, LocationId source,
                                                TypePtr target, StringId resultName) {
    auto implementation = local[instance]->functions.get(local, index);
    if(!implementation) return nullptr;

    auto inherited = implementation == global[local[instance]->typeClass]->functions.get(global, index).defaultFun;

    /*
     * The types the implementation is compiled at, which are not always the ones the head bound.
     *
     * Two things move it off the head's own bindings, and a `lens fn` default is where they meet.
     *
     * A **default** the instance did not override is generic over the *class's* type variables
     * rather than over the head's, so what specializes it is what the head resolves to - `Ptr(Int)`,
     * for `Eq(Ptr(a))` selected at `a = Int`. Reading the class types back off the head is what
     * makes a concrete and a parametric instance the same case here.
     *
     * An **iterator or lens** declares one variable more than either of those lists has: the
     * desugaring gives its continuation a result, and it lands after the ones an instance selection
     * fills because a class has nowhere to put one and the head therefore cannot bind it
     * (Implementation-Containers.md §5, resolveInstance, and resolveClassDefault, which does the
     * same for a body the class wrote). Nothing about the instance answers it - the *call site*
     * does, through the continuation it passes - so the trailing variables are solved from the
     * arguments rather than substituted.
     *
     * The already-decided positions are put back afterwards rather than left to the solve. They are
     * decided, and binding them a second time against arguments that may have been widened or read
     * through a borrow is a second opinion about a question selection has answered.
     */
    auto implEnv = functionGen(global, *local[implementation]);
    auto bound = implEnv ? implEnv->types.size() : 0;

    TypeList typeArgs;

    if(inherited) {
        for(auto type: local[instance]->forTypes.contents(local)) {
            typeArgs.push(substituteType(module, type, instanceArgs, source));
        }
    } else {
        for(auto type: instanceArgs) typeArgs.push(type);
    }

    if(bound > typeArgs.size()) {
        auto decided = typeArgs.size();

        Solution solution;
        Solver solver(*this, solution, implEnv);

        for(Size i = 0; i < decided; i++) solution.types[i] = typeArgs[i];
        solver.bindArguments(implementation, args, Unresolved::Binds);

        if(!solver.settle(decided, bound)) {
            context.diagnostics.error("cannot infer type argument %@ of %@ here"_v, source,
                                      context.findName(global[implEnv->types.get(global, solution.position)]->name),
                                      context.findName(local[implementation]->name));
            return nullptr;
        }

        // Seeded above so that a later position reads them, restored here so that a widen the solve
        // performed on one of them does not become the answer.
        for(Size i = 0; i < decided; i++) solution.types[i] = typeArgs[i];

        replaceContents(typeArgs, solution.types);
    }

    // A default is specialized for the class types and called as the ordinary function that makes
    // it - it is written against the class's variables rather than the head's, so there is no
    // instance signature for the arguments to be restated in.
    if(inherited) {
        auto specialized = instantiateFunction(site, implementation, toBuffer(typeArgs), source);
        if(!specialized) return nullptr;

        return emitDirectCall(specialized, args, source, target, resultName);
    }

    // A concrete instance's implementation is a function like any other - unless the desugaring
    // gave it a variable of its own, which is what the solve above just answered.
    if(!local[instance]->gen && typeArgs.isEmpty()) {
        return emitDirectCall(implementation, args, source, target, resultName);
    }

    // A parametric one's is written against the head's variables, so the types the head bound are
    // what makes it a function about something. An intrinsic has no body to specialize and is
    // generated here for those types, exactly as a generic intrinsic is at an ordinary call site.
    ArgList converted;
    substituteArguments(implementation, args, toBuffer(typeArgs), source, converted);

    if(local[implementation]->intrinsic || local[implementation]->deferredIntrinsic) {
        return expandIntrinsic(implementation, toBuffer(typeArgs), toBuffer(converted), source, resultName);
    }

    auto specialized = instantiateFunction(site, implementation, toBuffer(typeArgs), source);
    if(!specialized) return nullptr;

    return emitDirectCall(specialized, toBuffer(converted), source, target, resultName);
}

// Works out whether one class function can serve this call, and if so which instance it selects.
// Returns false when the call does not fit the signature at all; a fitting signature with no
// instance is reported through `resolved` so the caller can tell "wrong function" from "no
// instance for these types", which are very different diagnostics.
bool ExprResolver::matchClassFun(const ClassFunRef& reference, Buffer<ResolvedArg> args,
                                 Buffer<const StringId> names, TypePtr target, ClassMatch& resolved) {
    auto entry = global[reference.typeClass]->functions.get(global, reference.index);
    auto signature = entry.fun;
    if(!signature) return false;

    /*
     * The arity rule, which is now the normalization: the arguments this call site wrote have to
     * reach this signature's parameters, and what is left over has to have defaults. It is asked
     * silently - a candidate that cannot serve the call is one of several here, and selection is
     * what says why nothing served it.
     */
    ArgMapping mapping;
    if(!mapArguments(signature, names, args.length, 0, entry.name, kNullLocation, false, mapping)) {
        return false;
    }

    // Everything below this line is about the call in *parameter* order, which is what makes a
    // named argument invisible to the solve and to every diagnostic the solve produces.
    normalizeArguments(mapping, args, resolved.normalized);
    args = toBuffer(resolved.normalized);

    Solution solution;
    solveClassFun(*this, reference.typeClass, signature, args, target, solution);

    // Kept whatever the solve answered: a dependency nothing declares is a diagnostic about the
    // constraint the author has to write, and the match it belongs to is a failed one.
    resolved.undeclaredDependency = solution.undeclaredDependency;
    if(!solution) return false;

    resolved.typeClass = reference.typeClass;
    resolved.index = reference.index;
    resolved.instance = solution.instance;

    replaceContents(resolved.args, solution.types);
    replaceContents(resolved.instanceArgs, solution.instanceArgs);

    return true;
}

// The plain-function half of an overload set. Design.md's R1 keys the set by (name, arity) and
// admits at most one plain function, so this is arity plus "do the arguments fit", and the answer
// has to be reached without reporting anything - see ExprResolver::convertible.
bool ExprResolver::matchFunction(ModulePtr<Function> callee, Buffer<ResolvedArg> args, TypePtr target,
                                LocationId source, const OverloadSet& set, TypeList& typeArgs,
                                ArgList& normalized) {
    auto callable = local[callee];
    typeArgs.clear();

    // The arity rule and the names and the defaults, all as one question - see mapArguments. Asked
    // silently: R5 lets the class half serve a call the plain function does not fit, so a plain
    // function that cannot be reached is an answer here rather than a diagnostic.
    ArgMapping mapping;
    if(!mapArguments(callee, toBuffer(set.names), args.length, set.shape.supplied, set.name,
                     kNullLocation, false, mapping)) {
        return false;
    }

    normalizeArguments(mapping, args, normalized);
    args = toBuffer(normalized);

    auto declaredArgs = args.length;

    // A generic function fits when its type arguments can all be inferred here, by the same
    // one-directional rule the classes use - which is one solve, performed for real by
    // emitGenericCall once something has committed to this callee.
    if(functionGen(global, *callable)) {
        Solution solution;
        solveSignature(*this, callee, args, target, source, Unresolved::Rejects, solution);

        if(!solution.fits()) return false;

        /*
         * Every type argument decided - but only when this call fills the whole signature.
         *
         * A call that does not leaves the variables the missing positions mention open, and that is
         * not a failure to fit: a `for` loop's iterator declares a continuation the *loop* supplies,
         * and its result variable is precisely what the block below is about to decide. Requiring it
         * here answered "no" for every generic iterator, including the one the loop was written for.
         * See continuationSignature, which is where those variables are settled instead.
         */
        if(declaredArgs < callable->args.size()) return true;
        if(!solution) return false;

        // The answer, for a caller that is about to commit to this callee. A solve that did not
        // settle every variable hands back nothing at all rather than a list with holes in it.
        replaceContents(typeArgs, solution.types);
        return true;
    }

    for(Size i = 0; i < args.length; i++) {
        // A defaulted position fits by construction: what fills it is a constant of the parameter's
        // own type, decided at the declaration rather than here. See ResolvedArg::defaulted.
        if(args[i].isDeferred() || args[i].isDefault()) continue;

        auto declared = local[callable->args.get(local, i)]->declaredType();

        // A value carrying nothing fits a parameter declared to carry nothing, and nothing else.
        // Said here rather than inside convertible(), which is asked about values and answers "no"
        // for the absence of one - which is the right answer for a failure and the wrong one for a
        // `{}` that this list can now tell apart from it.
        if(args[i].state == ArgResult::Unit) {
            if(!isUnit(global, declared)) return false;
            continue;
        }

        if(!convertible(args[i].value, declared, source)) return false;
    }

    return true;
}

// Precedence climbing over the flattened operand/operator lists. The parser cannot do this
// itself: fixity declarations are module-level, so an operator's precedence is only known once
// the whole module has been read.
/*
 * `target` is the type the whole chain is expected to produce, and it is passed to each operator's
 * own selection rather than to the operands.
 *
 * What it buys is the case where nothing else says: `1 \`or\` 2` at a `WideInt` result used to
 * settle both literals to `Int`, select `Integral(Int)`, compute at 32 bits and widen the *answer* -
 * so `34359738368 \`or\` 255` was 255, having lost the operand before the operator ran.
 * `matchClassFun` already knows what to do with an expected result; it was simply never handed one
 * from here.
 *
 * To the operator and not to the operands, because the operands are where this would stop being
 * safe. `matchClassFun` lets the expected result fill only what the arguments left *open* - a
 * literal binds nothing, a concrete operand binds its own type and wins - so `x + 1` on an `Int` x
 * still computes at `Int` however the result is used. Resolving the operands against `target`
 * instead would convert `x` first and silently change that to `WideInt` arithmetic.
 *
 * The same reason bounds what it fixes: a *parenthesized* sub-expression is resolved through
 * `resolve` below with no expected type, so `(1 \`or\` 2) \`and\` 3` still settles the inner call
 * to `Int` before the outer one is matched. Carrying an expected type down into operand position
 * is bidirectional checking, which this resolver deliberately does not do (it binds one-way and
 * positionally), and it is the same wall the property-constraint inference hit.
 */
/*
 * What the rung below a given operator starts at.
 *
 * This one number is the whole of associativity. A left-associative operator hands its right
 * operand a rung *above* its own, so an operator of equal precedence to its right is left for the
 * loop that is already running and becomes the left operand of the next call - `a - b - c` is
 * `(a - b) - c`. A right-associative one hands over its own rung, so the equal operator is consumed
 * by the recursion instead and becomes part of the right operand: `a ^ b ^ c` is `a ^ (b ^ c)`.
 *
 * Both halves of the climb ask this - the resolving one and the skipping one below - because a
 * deferred operand must span exactly what resolving it would have consumed.
 */
static U8 rightRung(OperatorFixity fixity) {
    return fixity.right ? fixity.precedence : U8(fixity.precedence + 1);
}

// Advances past exactly the sub-chain resolvePrecedence would have consumed, without resolving any
// of it. What a deferred right operand needs: the chain still has to be walked to find where this
// operator's argument ends and the next one begins, but the expression itself belongs in whatever
// block the callee decides to run it in.
static void skipPrecedence(InfixChain& chain, Size& operandIndex, Size& operatorIndex, U8 minimumPrecedence) {
    operandIndex++;

    while(operatorIndex < chain.operators.size() &&
          chain.fixities[operatorIndex].precedence >= minimumPrecedence) {
        auto fixity = chain.fixities[operatorIndex++];
        skipPrecedence(chain, operandIndex, operatorIndex, rightRung(fixity));
    }
}

ModulePtr<Value> ExprResolver::resolvePrecedence(InfixChain& chain, Size& operandIndex, Size& operatorIndex, U8 minimumPrecedence, TypePtr target) {
    auto lhsExpr = chain.operands[operandIndex++];
    auto lhs = resolve(*lhsExpr);

    while(operatorIndex < chain.operators.size() && chain.fixities[operatorIndex].precedence >= minimumPrecedence) {
        auto opSource = chain.operatorSources[operatorIndex];
        auto op = chain.operators[operatorIndex];
        auto fixity = chain.fixities[operatorIndex++];

        /*
         * The right operand of a short-circuiting operator is not resolved here.
         *
         * Only the right one: the left is already a value by the time the operator is read, and an
         * operator whose *first* argument was `@lazy` would need the chain to be walked backwards
         * to find it. Nothing declares one, and Design.md's uses are all second-position, so it is
         * reported rather than supported.
         */
        OverloadSet set;
        gatherOverloads(op, 2, opSource, opSource, set);

        ArgList args;
        replaceContents(args, set.strictness);

        if(args[0].isDeferred()) {
            context.diagnostics.error("the left operand of %@ is declared `@lazy`, which an infix operator cannot be - it is evaluated before the operator is read"_v,
                                      lhsExpr->source, context.findName(op));
            return nullptr;
        }

        /*
         * `out ++= "a{x}b"` - Design-Test.md §11.1's P2, and the one place a written operator is not
         * a call at all.
         *
         * A format expression already builds into a sink and a fresh `String` is merely the case
         * where nothing else supplied one, so where the right operand *is* a format the left one is
         * handed to it: the summed `showBound` becomes a reservation on the string that exists, the
         * same appends run against the same buffer, and no temporary is made to be joined on and
         * released.
         *
         * A recognition of the shape and not a new expression form. `out ++= someString` and `out
         * ++= f()` are the ordinary function below and allocate nothing extra anyway, so there is
         * one spelling and it always means the same thing. Three things have to hold for the shape
         * to be this one: the operator is `++=`, the operand next in the chain is a format, and the
         * chain does not continue into it - `out ++= "{x}" ++ y` joins a format to something and is
         * an ordinary call whose left operand happens to be one.
         */
        if(op == module.program.appendAssign && module.program.reserveString && !args[1].isDeferred() &&
           operandIndex < chain.operands.size() &&
           chain.operands[operandIndex]->kind == ast::Expr::Format &&
           (operatorIndex >= chain.operators.size() ||
            chain.fixities[operatorIndex].precedence < rightRung(fixity))) {
            if(!lhs) return nullptr;

            auto formatted = resolveFormat(*chain.operands[operandIndex++], lhs);
            if(!formatted) return nullptr;

            lhs = formatted;
            continue;
        }

        DeferredChain deferred;

        if(args[1].isDeferred()) {
            deferred.chain = &chain;
            deferred.operandIndex = operandIndex;
            deferred.operatorIndex = operatorIndex;
            deferred.minimumPrecedence = rightRung(fixity);
            args[1].promise.chain = &deferred;

            skipPrecedence(chain, operandIndex, operatorIndex, rightRung(fixity));
        } else {
            auto rhs = resolvePrecedence(chain, operandIndex, operatorIndex, rightRung(fixity));
            if(!rhs) return nullptr;

            args[1] = rhs;
        }

        if(!lhs) return nullptr;

        args[0] = lhs;
        lhs = emitCall(set, toBuffer(args), lhsExpr->source, target, StringId());
    }

    return lhs;
}

/*
 * Two operators of one precedence that disagree about which way they group.
 *
 * `a <> b <+> c` for an `infixl 5 <>` and an `infixr 5 <+>` has two readings and no reason to
 * prefer either, so it is reported rather than decided. Taking the leftmost operator's word for it
 * would make the grouping of a chain depend on an operator somewhere else in it, which is the one
 * thing fixity exists to stop a reader having to work out.
 *
 * Two operators meet at the same rung exactly when everything strictly between them binds tighter:
 * an operator of lower or equal precedence in between is where the chain divides, and neither side
 * of that division can see the other. So the scan runs forward from each operator and stops at the
 * first one that is not tighter, which is the only one it can be in conflict with. `a <+> b * c <>
 * d` is caught - the `*` between them binds tighter and the two rung-5 operators do meet - while
 * `a * b <> c * d` is not, since the two `*`s are on opposite sides of the `<>`.
 */
static bool checkMixedFixity(Context& context, InfixChain& chain) {
    for(Size i = 0; i + 1 < chain.fixities.size(); i++) {
        auto fixity = chain.fixities[i];

        for(Size j = i + 1; j < chain.fixities.size(); j++) {
            auto other = chain.fixities[j];
            if(other.precedence > fixity.precedence) continue;
            if(other.precedence < fixity.precedence || other.right == fixity.right) break;

            context.diagnostics.error("%@ is `infix%@ %@` and %@ is `infix%@ %@`, so this expression has no grouping - parenthesize one of them"_v,
                                      chain.operatorSources[j],
                                      context.findName(chain.operators[i]), fixity.right ? "r"_v : "l"_v, fixity.precedence,
                                      context.findName(chain.operators[j]), other.right ? "r"_v : "l"_v, other.precedence);
            return false;
        }
    }

    return true;
}

ModulePtr<Value> ExprResolver::resolveBinary(const ast::Expr& expr, const ast::InfixExpr& binary, TypePtr target, bool convertResult) {
    InfixChain chain;
    auto node = &binary;

    // The parser nests infix expressions to the right without regard for precedence, so the
    // chain is flattened first and then re-associated by resolvePrecedence.
    while(true) {
        if(node->op.kind != ast::Expr::Var) {
            context.diagnostics.error("an infix operator must be a named operator"_v, node->op.source);
            return nullptr;
        }

        auto fixity = findFixity(module, node->op.var);

        if(!fixity) {
            context.diagnostics.error("operator has no declared fixity %@"_v, node->op.source, context.findName(node->op.var));
            return nullptr;
        }

        chain.operands.push(&node->lhs);
        chain.operators.push(node->op.var);
        chain.operatorSources.push(node->op.source);
        chain.fixities.push(fixity);

        if(node->rhs.kind != ast::Expr::Infix) {
            chain.operands.push(&node->rhs);
            break;
        }

        node = parse[node->rhs.infix];
    }

    if(!checkMixedFixity(context, chain)) return nullptr;

    Size operandIndex = 0;
    Size operatorIndex = 0;

    // Climbing starts at 0 rather than 1 because 0 is a declarable precedence - it is where Core
    // puts the compound assignments. Starting a rung above it would drop such an operator out of
    // the loop and quietly yield its left operand instead of applying it.
    // The target goes into the chain as well as being applied to its result. Where the operators
    // could honour it the conversion afterwards is then the identity; where they could not - a
    // concrete operand decided the instance - it is the conversion that was always emitted.
    auto result = resolvePrecedence(chain, operandIndex, operatorIndex, 0, target);
    if(result && target) result = convert(result, target, expr.source, convertResult);
    return result;
}

ModulePtr<Value> ExprResolver::resolvePrefix(const ast::Expr& expr, const ast::PrefixExpr& prefix, TypePtr target,
                                             bool convertResult) {
    if(prefix.op.kind != ast::Expr::Var) {
        context.diagnostics.error("a prefix operator must be named"_v, prefix.op.source);
        return nullptr;
    }

    OverloadSet set;
    gatherOverloads(prefix.op.var, 1, prefix.op.source, prefix.op.source, set);

    ArgList args;
    replaceContents(args, set.strictness);

    if(args[0].isDeferred()) {
        args[0].promise.expr = &prefix.on;
    } else {
        // The operand is resolved with no expected type of its own. What a prefix operator's
        // argument should be is its selected overload's parameter type, which is not known until
        // the operand has one - and pushing the *result* type down is only right when the two
        // coincide, as they do for `-` and not for a dereference, whose operand is a pointer to
        // its result.
        auto value = resolve(prefix.on);
        if(!value) return nullptr;

        args[0] = value;
    }

    auto result = emitCall(set, toBuffer(args), expr.source, target, StringId());

    return convertResult && target ? convert(result, target, expr.source) : result;
}

/*
 * A call whose callee is a value rather than a name.
 *
 * Two shapes reach here and they are the same question asked twice: `f(x)` where `f` is a local of
 * function type, and `(expr)(x)` where the callee is any expression at all. The first has to be
 * checked before the name is looked up as a function, because a binding shadows a module-level
 * declaration exactly as it does everywhere else.
 *
 * Null when this call is not an indirect one, which leaves it for the ordinary path.
 */
ModulePtr<Value> ExprResolver::resolveIndirectCall(const ast::Expr& expr, const ast::AppExpr& call,
                                                   TypePtr target) {
    auto& callee = unwrapNested(call.callee);
    ModulePtr<Value> callable = nullptr;

    if(callee.kind == ast::Expr::Var) {
        auto binding = findBinding(callee.var, callee.source);
        if(!binding) return nullptr;

        if(binding->lazy) {
            // The name holds the thunk, so calling what it stands for is two calls: force it, and
            // then call the function value the argument produced.
            Deferred deferred;
            deferred.thunk = binding->value;
            callable = force(deferred, nullptr, callee.source);
        } else {
            callable = binding->isPlace() ? load(placeOf(*binding, callee.source), callee.source)
                                          : binding->value;
        }

        if(!isFunction(global, valueType(callable))) {
            context.diagnostics.error("%@ is not callable - it is %@"_v, callee.source,
                                      context.findName(callee.var),
                                      describeType(context, global, valueType(callable)));
            return nullptr;
        }
    } else {
        callable = resolve(callee);

        // Already broken, and said so - see resolveField. Calling an error is not a second fact.
        if(callable && global[valueType(callable)]->kind == Type::Error) return callable;

        if(!callable || !isFunction(global, valueType(callable))) {
            if(callable) {
                context.diagnostics.error("this expression is not callable - it is %@"_v, callee.source,
                                          describeType(context, global, valueType(callable)));
            }

            return nullptr;
        }
    }

    return callIndirect(callable, expr, call, target);
}

/*
 * A call through a value that has already been produced.
 *
 * Split from `resolveIndirectCall` because the dot-call form arrives here with the callable in hand:
 * it resolved the receiver in order to decide whether this was a field at all, and a field of
 * function type is exactly this call. Everything below is about the argument list and the function
 * *type*, and neither of those cares where the value came from.
 */
ModulePtr<Value> ExprResolver::callIndirect(ModulePtr<Value> callable, const ast::Expr& expr,
                                            const ast::AppExpr& call, TypePtr target) {
    if(!callable) return nullptr;

    if(!isFunction(global, valueType(callable))) {
        context.diagnostics.error("this expression is not callable - it is %@"_v, expr.source,
                                  describeType(context, global, valueType(callable)));
        return nullptr;
    }

    auto signature = (FunType*)global[valueType(callable)];
    auto callArgs = call.args;

    /*
     * The same normalization a call by name performs, over the one thing a function *type* knows
     * about its parameters: their names. A default belongs to a declaration rather than to a type -
     * two functions of one type do not agree about it - so every position is filled here, and
     * `mapValueArguments` is the shorter rule that says so.
     *
     * Reported here rather than left to a candidate, because there is only ever one candidate: the
     * value being called is the callee, so anything wrong with the argument list is wrong with the
     * call.
     */
    ArgNames names;
    collectArgNames(callArgs, names);

    ArgMapping mapping;
    if(!mapValueArguments(signature, toBuffer(names), callArgs.size(), expr.source, true, mapping)) {
        return nullptr;
    }

    // A function value's parameter types are known before its arguments are resolved, exactly as a
    // plain function's are, so they are pushed down the same way - which is what lets `f(Nothing)`
    // through a `(Maybe(Int)) -> Bool` know which `Maybe` it is building.
    ArgList written;
    Size index = 0;

    for(auto arg: callArgs.contents(parse)) {
        auto parameter = index < mapping.parameters.size() ? mapping.parameters[index]
                                                           : ArgMapping::kDefaulted;
        auto expected = parameter < signature->args.size()
            ? signature->args.get(global, parameter).type : TypePtr(nullptr);

        written.push(resolve(arg.value, expected));
        index++;
    }

    // In the order the callee takes them, which is not the order they were written in where a name
    // was used - and evaluated above in the order they *were* written, which is what
    // doc/spec/expressions.md's evaluation order says about a named argument.
    ArgList normalized;
    normalizeArguments(mapping, toBuffer(written), normalized);

    ValueList values;
    for(auto& value: normalized) values.push(value.value);

    auto result = emitDynamicCall(callable, toBuffer(values), expr.source, StringId());
    return target ? convert(result, target, expr.source) : result;
}

/*
 * The cursor sentinel in an argument of a written call - `f(x|` and `f(mo|: 1)`.
 *
 * A position of its own rather than an ordinary name that happens to be inside brackets: what may be
 * written at an argument is a value, *or* the name of a parameter the call has not filled yet.
 * Caught ahead of the loop that resolves the arguments, because by the time `resolve` reaches the
 * sentinel the set is out of reach - the ordinary capture sees a name and an expected type, and
 * which call it is an argument of is exactly what it cannot ask.
 *
 * The two positions are the two the construction form has, and for the same reason: a cursor in a
 * *name* (`f(mo|: 1)`) can be nothing but a parameter, while one in a bare argument has not said yet
 * which of the two it is. See captureConstructionFields, whose shape this is.
 *
 * True when it captured, which ends the call: the sentinel names nothing, so there is no call left
 * to resolve underneath it.
 */
bool ExprResolver::captureCallArguments(ast::ParseList<ast::TupArg> arguments, const OverloadSet& set,
                                        const ArgMapping* pushdown, ModulePtr<Function> signature,
                                        Size leading) {
    if(!wantsCompletion(context)) return false;

    auto written = arguments.contents(parse);

    for(Size index = 0; index < written.size(); index++) {
        auto arg = written.pointerAt(index);

        if(arg->name && isCursorSentinel(context, arg->name)) {
            captureArgumentCompletion(*this, set, nullptr, true);
            return true;
        }

        auto& value = unwrapNested(arg->value);
        if(arg->name || value.kind != ast::Expr::Var || !isCursorSentinel(context, value.var)) continue;

        // The type this position was asked for, where one candidate is the whole of the set - the
        // same pushdown the arguments are resolved against, read through the same mapping.
        TypePtr expected = nullptr;

        if(signature && pushdown && index + leading < pushdown->parameters.size()) {
            auto filled = pushdown->parameters[index + leading];

            if(filled < local[signature]->args.size()) {
                expected = local[local[signature]->args.get(local, filled)]->declaredType();
            }
        }

        captureArgumentCompletion(*this, set, expected, false);
        return true;
    }

    return false;
}

/*
 * An argument that resolved to nothing did so for one of two reasons, and the difference is only
 * visible here: either the resolver reported on it, or the expression genuinely produces no value -
 * `{}`, or a call whose result type is `{}`. The same before-and-after count `resolvePattern`'s
 * callers and `witness.cpp` use to ask exactly this question.
 *
 * It is the one judgment of its kind left in a call, and it is here because `resolve` answers with a
 * value and a value is not what a `{}` has. Everything downstream reads the tag instead.
 */
ResolvedArg ExprResolver::resolveArgument(const ast::Expr& expr, TypePtr expected) {
    auto errors = context.diagnostics.errorCount();

    if(auto value = resolve(expr, expected)) return value;
    return errors == context.diagnostics.errorCount() ? ResolvedArg::unit() : ResolvedArg::failed();
}

ModulePtr<Value> ExprResolver::resolveCall(const ast::Expr& expr, const ast::AppExpr& call, TypePtr target, bool convertResult) {
    // A binding of function type shadows a declaration of the same name, and an arbitrary callee
    // expression was never a name at all. Both are the indirect path.
    auto& calleeExpr = unwrapNested(call.callee);

    /*
     * The cursor sentinel in callee position - `f|(x)` - which is where an editor asks most often,
     * since typing a name and then its arguments is the order a call is written in.
     *
     * The call's expected result type is what ranks the answer: a candidate that returns what this
     * position wants is the one being reached for. Ahead of everything, because the arguments are
     * resolved against a callee that does not exist and would each report.
     */
    if(calleeExpr.kind == ast::Expr::Var && isCursorSentinel(context, calleeExpr.var)) {
        captureCompletion(*this, target, nullptr, false);
        return nullptr;
    }

    // `x.f(y)` - Design.md's dot-call form. Ahead of the indirect path because deciding between the
    // two is what it does: a field of function type is still called indirectly, and everything else
    // of that name is `f(x, y)`.
    if(calleeExpr.kind == ast::Expr::Field) {
        return resolveDotCall(expr, call, *parse[calleeExpr.field], target, convertResult);
    }

    auto named = calleeExpr.kind == ast::Expr::Var && !findBinding(calleeExpr.var);

    if(!named) return resolveIndirectCall(expr, call, convertResult ? target : nullptr);

    return resolveNamedCall(expr, calleeExpr.var, calleeExpr.source, nullptr, call.args, target,
                            convertResult);
}

/*
 * A call by name, with or without a receiver written to the left of it.
 *
 * `receiver` is the dot-call form's first argument, resolved before this was reached - null for an
 * ordinary call, and the only difference between the two. It is an argument in every other respect:
 * it occupies position 0 of the set's arity, of the names, of the mapping and of the strictness, so
 * a default, a named argument and a `&` parameter each mean here what they mean anywhere. That is
 * why the receiver is prepended rather than carried beside the list - `CallShape::supplied` counts
 * *trailing* parameters a call site fills, which is the loop's continuation and not this.
 *
 * What the receiver does not get is pushdown. Its type had to be known to decide whether this was a
 * field at all, so it is resolved before the callee is, and no candidate's parameter type can reach
 * it. Every written argument keeps pushdown exactly as before - see the loop below, which reads the
 * mapping at `index + leading`.
 */
ModulePtr<Value> ExprResolver::resolveNamedCall(const ast::Expr& expr, StringId name, LocationId nameSource,
                                                ModulePtr<Value> receiver, ast::ParseList<ast::TupArg> args_,
                                                TypePtr target, bool convertResult) {
    /*
     * The overload set, gathered once, at the *callee's* location rather than the call's.
     *
     * It is where the name is written, so it is both the better place for an ambiguity diagnostic
     * to point and the only location the index should hold this answer against - see
     * resolve/index.h. Recording it against the whole call would make find-references report the
     * call and the name as two hits on the same name.
     *
     * Everything below reads this: which positions are `@lazy`, whether a plain function is the
     * whole of it, whether the callee is a lens or an iterator, and - through emitCall - which
     * candidate serves the call.
     */
    auto callArgs = args_;
    auto leading = receiver ? Size(1) : Size(0);

    /*
     * The names the call site wrote, which the set is gathered *with*: which parameter each argument
     * fills is part of "does this candidate serve the call", so it cannot be asked afterwards. See
     * OverloadSet::names.
     *
     * The receiver's own entry is empty, because a receiver is positional by construction - it is
     * written to the left of the name and there is nowhere to put one. That is also what makes
     * `x.f(self: y)` the ordinary double-fill error rather than a case anything here has to know
     * about: two things claim parameter 0 and `mapArguments` reports it.
     */
    ArgNames names;
    if(leading) names.push(StringId());
    for(auto arg: callArgs.contents(parse)) names.push(arg.name);

    CallShape shape;
    if(receiver) shape.receiver = valueType(receiver);

    OverloadSet set;
    gatherOverloads(name, callArgs.size() + leading, nameSource, nameSource, set,
                    shape, toBuffer(names));

    auto direct = set.direct;

    // A plain function's parameter types are known before its arguments are resolved, so they
    // are pushed down as the expected type of each one. That is what lets `f(Nothing)` know
    // which `Maybe` it is building - neither a class function nor a generic function can do the
    // same, because which types their parameters have is exactly what the arguments are being
    // resolved to decide.
    //
    // Only when the plain function is the whole overload set, though: R5 lets the class half serve
    // a call the plain function does not fit, and pushing its parameter types into the arguments
    // would report the mismatch before selection ever got the chance to look elsewhere.
    //
    // `pushdownSignature` is that question, asked the same way a `for` loop asks it: the sole
    // candidate's types may be pushed down and nobody else's. The extra clause is this site's alone
    // - a generic function's parameter types are exactly what the arguments are being resolved to
    // decide, so there is nothing there to push even when it is the only candidate.
    auto sole = direct && pushdownSignature(set) == direct;
    auto declared = sole && !local[direct]->gen;

    /*
     * A *generic* sole candidate, whose parameter types may not be pushed down and whose fixed-array
     * shapes may - see arrayShapeFor. The mapping is what both readings are indexed through, so it
     * is built for either.
     */
    // Which parameter of that sole candidate each written argument fills, which is the identity for
    // a call with no names in it and is what pushdown reads through where there are - `f(b: x)`
    // expects `b`'s type of its one argument. Only built where there is a signature to push down
    // from; `gatherOverloads` already established that it maps.
    ArgMapping pushdown;
    if(sole) {
        mapArguments(direct, toBuffer(names), callArgs.size() + leading, 0, set.name, expr.source,
                     false, pushdown);
    }

    /*
     * An editor asking what may be written at one of the arguments.
     *
     * Ahead of the diagnostics below as well as of the loop that resolves the arguments: each of
     * them ends the call, and a call the author is still typing is exactly the one an editor asks
     * about. See captureCallArguments.
     */
    if(captureCallArguments(callArgs, set, declared ? &pushdown : nullptr, declared ? direct : nullptr,
                            leading)) {
        return nullptr;
    }

    /*
     * A lens or iterator call that reaches here left its continuation out and is in a position that
     * does not supply one.
     *
     * Nothing splits at this position - Analysis-Lens.md's V1 restriction is that a lens call is the
     * whole right-hand side of a `let` or a statement of its own, and an iterator's is the source of
     * a `for` - so the arity is genuinely one short, and saying that is more use than "takes 3
     * arguments but was given 2".
     */
    // Read off `mismatched` rather than `direct`, and necessarily so: the callee takes one argument
    // more than the call site filled, so it is by construction the plain function this call does not
    // reach. "One more" is asked through the mapping rather than as a count, so a lens whose call
    // also left a default out is still recognized as the lens it is.
    auto continuationShort = set.mismatched;
    ArgMapping handed;

    if(continuationShort && local[continuationShort]->funKind != ast::FunKind::Plain &&
       mapArguments(continuationShort, toBuffer(names), callArgs.size() + leading, 1, set.name,
                    expr.source, false, handed)) {
        context.diagnostics.error(local[continuationShort]->funKind == ast::FunKind::Iter
            ? "%@ is an iterator, so this call has no body to hand its values to - write it as the source of a `for` loop, which is the only thing that supplies one"_v
            : "%@ is a lens, so this call needs the rest of a block to hand its values to - write it as a statement of its own or as the whole right-hand side of a `let`, or pass the continuation as a final argument"_v,
            expr.source, context.findName(name));
        return nullptr;
    }

    /*
     * The call's arguments, one entry per position.
     *
     * Started from the set's strictness - a `Deferred` entry for each `@lazy` position and an empty
     * one for the rest - and each empty one replaced below by what resolving that argument produced:
     * what a position holding no value means is decided at the moment it is produced and nowhere
     * else. See ResolvedArg.
     */
    ArgList args;
    replaceContents(args, set.strictness);

    /*
     * The receiver takes position 0, already resolved.
     *
     * A `@lazy` position 0 is refused rather than honoured: `@lazy` means the argument is not
     * evaluated at the call site, and deciding this was a dot-call at all required the receiver's
     * type, which required evaluating it. Reported here because this is the one place that knows
     * both facts - that the position is deferred, and that something has already been emitted into
     * it - and the plain call form is the spelling that still works.
     */
    if(leading) {
        if(args[0].isDeferred()) {
            context.diagnostics.error("%@ takes its first argument `@lazy`, so it cannot be called with `.` - the receiver would have to be evaluated to find the function, which is what `@lazy` says not to do; write it as %@(...) instead"_v,
                                      expr.source, context.findName(name), context.findName(name));
            return nullptr;
        }

        args[0] = receiver;
    }

    auto written = callArgs.contents(parse);

    for(Size index = 0; index < written.size(); index++) {
        // By address rather than through the iterator: a deferred argument is resolved long after
        // this loop has ended, so what is remembered has to be the node in the parse arena.
        auto arg = written.pointerAt(index);

        // Where this argument sits in the callee's list, which is one to the right of where it was
        // written when a receiver came first. Everything below is indexed by the position rather
        // than by the writing order for that reason.
        auto position = index + leading;

        // A `@lazy` argument is left as written. Not even the expected type is pushed into it here:
        // it is resolved against the parameter type once the callee is known, which is where the
        // force happens and therefore the only place that can convert it.
        if(args[position].isDeferred()) {
            args[position].promise.expr = &arg->value;
            continue;
        }

        auto filled = sole && position < pushdown.parameters.size() ? pushdown.parameters[position]
                                                                     : ArgMapping::kDefaulted;
        auto parameter = sole && filled < local[direct]->args.size()
            ? local[local[direct]->args.get(local, filled)] : nullptr;

        // A `&` parameter's type is deliberately not pushed down. What the argument has to produce
        // is storage to borrow, not a value of the parameter's type - so converting here would build
        // a temporary and then borrowArgument would be asked for a mutable borrow of something this
        // expression owns rather than of what was written.
        auto expected = declared && parameter && !parameter->isMutableBorrow()
            ? parameter->declaredType() : TypePtr(nullptr);

        // The one thing a *generic* signature can tell an argument - see arrayShapeFor.
        if(!expected && parameter) expected = arrayShapeFor(*parameter, arg->value);

        /*
         * A subscript in a `&` position, which is the one argument shape whose *accessor* the
         * convention decides.
         *
         * `xs[i]` is sugar for `get` when it is read and `getMut` when it is written, and an
         * argument was always the first: it was resolved as an ordinary expression, reached `get`,
         * and arrived at the `&` parameter as a borrow that may not be written. So `swap(xs[i],
         * xs[j])` did not compile, and `Array.yana` grew a `swapElements` for the one caller that
         * needed it.
         *
         * The ordering that made that look unavoidable - which accessor is wanted depends on the
         * convention, and the convention is not known until selection has picked a callee, which
         * needs the argument types - is not the ordering this loop is in. `pushdown` already knows
         * the sole candidate and the parameter this position fills, several lines above, and asks it
         * `isMutableBorrow()` to decide whether to push the type down. It is the same question one
         * step further: push the *convention* down as well.
         *
         * Only where the candidate is sole, on exactly the terms pushdown itself is. Where a name
         * has overloads that disagree about the convention there is no answer to push, and the
         * argument stays a read - which is what it was for every call before this.
         */
        auto wantsPlace = parameter && parameter->isMutableBorrow();

        if(wantsPlace && arg->value.kind == ast::Expr::Sub) {
            auto borrowed = resolveSubscript(arg->value, *parse[arg->value.sub], true);
            args[position] = borrowed ? ResolvedArg(borrowed) : ResolvedArg::failed();
            continue;
        }

        args[position] = resolveArgument(arg->value, expected);
    }

    /*
     * One route out, whatever `declared` decided.
     *
     * `declared` is a question about *pushdown* and nothing else: knowing the sole candidate early
     * is what lets its parameter types be the expected type of each argument. It used to be a second
     * route as well - recording the reference itself and calling emitDirectCall - which meant the
     * plain-function case skipped selection, and with it the failed-argument guard, the reference
     * recording and the choice of call form, each of which it then had to repeat or do without.
     *
     * Going through emitCall reaches the same callee by the rule that was already there: `declared`
     * implies the set is one plain function of this arity, which is exactly selectCallee's first
     * case. See ResolvedCallee.
     */
    auto result = emitCall(set, toBuffer(args), expr.source, target, StringId());

    return convertResult && target ? convert(result, target, expr.source) : result;
}

/*
 * `x.f(y)` - Design.md's dot-call form, which is `f(x, y)` for every `f` the body can name.
 *
 * The receiver is resolved once, at the top, and handed to whichever half wins. That is the whole
 * shape of this function and the reason it exists rather than the two halves each doing their own
 * lookup: resolving is emission, so a probe that resolved the receiver and then gave up would leave
 * its instructions behind and the winning half would emit them again.
 *
 * **The field wins.** A record whose field holds a function is called through that field, as it was
 * before this form existed - `test/resolve/Lambda.yana` is that case, and it must not change meaning
 * because a module elsewhere declares a function of the same name. The consequence is stated rather
 * than hidden: adding a field to a record shadows a dot-call on that record's type, which is the
 * same shape as a local binding shadowing a declaration and is checked the same way, by asking the
 * *nearer* thing first.
 *
 * Universal, and that includes this module's private functions. There is no `impl` block and no
 * per-type namespace: what a body can call, a body can call with a dot. It costs nothing in
 * ambiguity to say so, because a plain function does not overload - `findFunction` is a lookup by
 * name with at most one answer, and two visible ones are already an ambiguity at every call site
 * that names it, dot or not. See Design.md's R1.
 */
/*
 * The shared front half of a `for` source and a lens statement - see NamedCallee in expr.h for why
 * these two need one and an ordinary call does not.
 *
 * This was written twice. The `for` loop had it, correct and commented; the lens statement had half
 * of it - a plain name only - and the missing half was not a decision anybody had recorded. What
 * that cost was `c.withLock()`, refused with a message telling its author to write the call as a
 * statement of its own, which is what they had written.
 */
bool ExprResolver::namedCallee(const ast::Expr& calleeExpr, const ast::AppExpr& application,
                               ast::FunKind kind, Size supplied, bool fit, NamedCallee& out) {
    auto dotted = calleeExpr.kind == ast::Expr::Field;
    const ast::FieldExpr* field = nullptr;
    StringId name;
    LocationId nameSource = kNullLocation;

    if(dotted) {
        field = parse[calleeExpr.field];
        auto& selected = field->field;

        if(selected.kind != ast::Expr::Var || isCursorSentinel(context, selected.var)) return false;

        name = selected.var;
        nameSource = selected.source;
    } else {
        if(calleeExpr.kind != ast::Expr::Var || findBinding(calleeExpr.var)) return false;

        name = calleeExpr.var;
        nameSource = calleeExpr.source;
    }

    // Syntax alone, and that is what makes it usable below: the arity and the written names are
    // what "could anything of this name serve this call" is asked about, and neither needs a
    // receiver to exist yet.
    auto written = application.args;
    if(dotted) out.names.push(StringId());
    for(auto arg: written.contents(parse)) out.names.push(arg.name);

    /*
     * Whether anything of this name could serve this call, asked with no occurrence recorded and
     * before the receiver exists.
     *
     * Both halves are asked, because a class member is as much a name as a plain function is - and
     * the plain half alone is what let a `lens fn` of a class have no call site at all.
     *
     * `fit` is the second half of the question and only a lens statement asks it: the arguments
     * have to leave the trailing `supplied` parameters unwritten, since a call that fills them is
     * an ordinary call and the statement must hand it back rather than report on it. Counted
     * against the plain half's desugared signature and against a class member's undesugared one -
     * see CallShape::supplied, which is the same split.
     */
    auto takes = [&](ModulePtr<Function> declared, Size unwritten) {
        if(local[declared]->funKind != kind) return false;
        if(!fit) return true;

        ArgMapping mapping;
        return mapArguments(declared, toBuffer(out.names), out.arity(), unwritten, name, kNullLocation,
                            false, mapping);
    };

    auto serves = [&]() {
        auto plain = findFunction(module, name, nameSource, kNullLocation, false);
        if(plain && takes(plain, supplied)) return true;

        ClassFunList found;
        findClassFunctions(module, name, nameSource, found);

        for(auto& entry: found) {
            auto declared = global[entry.typeClass]->functions.get(global, entry.index);
            if(declared.fun && takes(declared.fun, 0)) return true;
        }

        return false;
    };

    /*
     * Asked where a no costs something, which is not both callers and not both spellings.
     *
     * A dot form always asks, because the receiver is about to be resolved and a name that serves
     * nothing would have emitted it for nothing - `upTo(n).twice()` is that case. A plain name
     * emits nothing at all, so the only reason to ask is that the caller wants the answer: a lens
     * statement does, since its no is a fall-through to an ordinary call, and a `for` loop does not
     * - `for x in plain(n)` is a loop with a mistake in it, and handing the name back is what lets
     * selection say which mistake rather than "that is not a name".
     */
    if((dotted || fit) && !serves()) return false;

    if(dotted) {
        out.receiver = resolve(field->target);
        if(!out.receiver || global[valueType(out.receiver)]->kind == Type::Error) return false;

        // A field of function type is a value, and a value is not this form - only a name that is
        // *not* a field of the receiver is a dot-call. See resolveDotCall, whose choice this
        // repeats, and NamedCallee for the one thing this ordering costs.
        if(hasFieldNamed(valueType(out.receiver), name)) return false;
    }

    // Last, so that every `false` above leaves the name unset - which is what the `for` loop reads
    // in place of the answer, since what it wants to say about a source it cannot name depends on
    // which of these it was.
    out.name = name;
    out.nameSource = nameSource;
    return true;
}

ModulePtr<Value> ExprResolver::resolveDotCall(const ast::Expr& expr, const ast::AppExpr& call,
                                              const ast::FieldExpr& field, TypePtr target,
                                              bool convertResult) {
    auto& calleeExpr = unwrapNested(call.callee);
    auto& selected = field.field;

    auto receiver = resolve(field.target);
    if(!receiver) return nullptr;

    // Already broken, and whatever broke it said so. Neither half of the choice below is a second
    // fact about this expression - see fieldOf, whose guard this is.
    if(global[valueType(receiver)]->kind == Type::Error) return receiver;

    /*
     * The choice, and the only place it is made.
     *
     * A field position that is not a plain name is a field position and nothing else: `t.0(x)` is a
     * tuple index holding a callable, and the cursor sentinel is an editor asking what may be
     * written here - which `projectField` answers, because the receiver's type is what the answer is
     * made of. Neither is a name a function could have.
     *
     * A binding of this name is deliberately *not* consulted either: `f` in `x.f(y)` is a member
     * position, not a scope one, so a local called `map` does not become every value's `.map`. That
     * is the one place this form departs from `resolveCall`'s "a binding shadows a declaration"
     * rule, and it departs from it because the two are not in the same namespace to begin with.
     */
    auto asField = selected.kind != ast::Expr::Var || isCursorSentinel(context, selected.var) ||
                   hasFieldNamed(valueType(receiver), selected.var);

    if(asField) {
        auto callable = fieldOf(receiver, calleeExpr, field);
        if(!callable) return nullptr;

        return callIndirect(callable, expr, call, convertResult ? target : nullptr);
    }

    return resolveNamedCall(expr, selected.var, selected.source, receiver, call.args, target,
                            convertResult);
}

/*
 * The `@lazy` positions of one call, completed in the callee's own terms.
 *
 * This is the point every `@lazy` argument has been travelling towards: the callee is known, so the
 * parameter type it was declared at is known, and the choice between emitting the argument where it
 * is used and wrapping it in a closure can finally be made. An intrinsic that declares one takes the
 * whole list unresolved and decides for itself where each one runs, which is what makes `a && b` a
 * branch; anything else gets the thunk, which prepareArguments below makes.
 *
 * `typeArgs` is what the callee's signature is read at - empty where it is already in the caller's
 * terms, and the call's type arguments where the callee is generic, a class signature or an erased
 * one. The answer is whether the callee declares a `@lazy` parameter at all, which is what decides
 * whether a deferred intrinsic is offered the unresolved list.
 */
bool ExprResolver::fillDeferred(ModulePtr<Function> callee, Buffer<ResolvedArg> args,
                                Buffer<TypePtr> typeArgs, LocationId source, ArgList& out) {
    auto function_ = local[callee];
    auto anyDeferred = false;

    out.clear();

    for(Size i = 0; i < args.length; i++) {
        auto declared = i < function_->args.size() ? local[function_->args.get(local, i)] : nullptr;

        if(!declared || !declared->isLazy()) {
            out.push(args[i]);
            continue;
        }

        auto type = substituted(declared->lazyType, typeArgs, source);
        auto entry = args[i].promise;

        // Not deferred by the call site: the argument was resolved before anything knew this
        // position was lazy, which is what a forwarded value and a synthesized call look like.
        if(!entry.isSet()) entry = deferredValue(args[i].value, type);

        entry.type = type;
        out.push(ResolvedArg::deferred(entry));
        anyDeferred = true;
    }

    return anyDeferred;
}

/*
 * Every argument's convention, applied once.
 *
 * This is the one place a call knows both what the callee asked for and what the caller produced, so
 * it is the one place the five things a parameter can be are decided: a `@lazy` one is handed the
 * closure that runs it, a `&` one a mutable borrow of the argument's storage, a `->` one the value
 * moved out of it, a `return` one a loan that outlives the call, and everything else the ordinary
 * converted value. What comes out is positional - see positionalUnit - and as long as `args`.
 *
 * There used to be two of these - the direct call's and the erased call's - and they had drifted.
 * The erased one loaned a `return` argument whether or not the callee received it by reference,
 * which is precisely the case the long comment below says must not be loaned; the fix reached it by
 * the two becoming one rather than by anyone noticing, which is the argument for this existing.
 *
 * The generic dispatch is deliberately not a third. Its callee is not a function the call site
 * reaches, so the conventions are not the caller's to apply - see emitGenericDispatch, which says
 * what happens instead and what it cost to find out.
 *
 * `positional` says the list becomes the arguments of a call *instruction*, which is what makes a
 * unit position storage rather than a hole - see positionalUnit. An intrinsic expansion is the one
 * caller that says no: it is handed values to build an instruction out of rather than a list paired
 * with parameters by index, and `unitValue()` for a position it ignores is an allocation the program
 * then carries. `Wrap(()).wrap` is the case, and it is why this is a parameter rather than a rule.
 *
 * What is *not* here is what to do with a position that came out null - a conversion that failed.
 * The direct call leaves it out and lets lowering pair what is left, the erased one declines the
 * call, and that stays where it is: it is a policy about the call and not about the argument.
 */
void ExprResolver::prepareArguments(ModulePtr<Function> callee, Buffer<ResolvedArg> args,
                                    Buffer<TypePtr> typeArgs, LocationId source, bool positional,
                                    ValueList& out) {
    auto function_ = local[callee];

    out.clear();

    for(Size i = 0; i < args.length; i++) {
        auto declared = i < function_->args.size() ? local[function_->args.get(local, i)] : nullptr;

        // A position the callee does not declare. There is no convention to apply, and the arity
        // mismatch is reported by whoever committed to this callee - see selectCallee's selectPlain.
        if(!declared) {
            out.push(args[i].value);
            continue;
        }

        // The callee cannot see the argument, so what it is handed is the closure that runs it. The
        // promise was completed by fillDeferred, whose output this is.
        if(declared->isLazy()) {
            auto type = substituted(declared->lazyType, typeArgs, source);
            out.push(makeThunk(args[i].promise, type, source));
            continue;
        }

        auto expected = substituted(declared->type, typeArgs, source);

        if(declared->isMutableBorrow()) {
            out.push(borrowArgument(args[i].value, expected, source, declared->returnRoot()));
            continue;
        }

        auto value = convertArgument(args[i], expected, source).value;

        // A `->` parameter consumes what it is given, so the argument is moved out of its storage
        // - or copied, for a TrivialCopy type. The conversion comes first deliberately: a
        // converted argument is a temporary of the callee's type, and moving out of a temporary is
        // the no-op sinkValue() already reports it as.
        if(declared->convention == ast::BindType::Sink) value = sinkValue(value, source);

        /*
         * A `return` argument is loaned rather than merely read.
         *
         * The marker says a borrow in the result may be rooted here, so the loan has to outlive the
         * call: nothing may write this storage while the result is still live. Making that an
         * explicit InstBorrow is what puts the extent in front of the borrow checker, which
         * otherwise sees only a value passed and returns the storage to general use at the call.
         *
         * The mutable case already has one - `&` created it above - and this is deliberately the
         * immutable one only.
         *
         * And only for a parameter the callee receives by reference. A borrow and the thing
         * borrowed are the same machine value for a memory type - both are an address - so
         * substituting one for the other is free, which is what made this work without anyone
         * having to say so. A scalar is passed by value, so the substitution would hand the callee
         * the address of the caller's variable where it declared the value: `fn get(return self:
         * %a, index: k)` was reached with `&%Int` and added the index to the wrong pointer. There
         * is nothing to protect in that case either - the callee got a copy, and a borrow rooted in
         * a copy is what the return-root check calls invalid - so no loan is the right answer as
         * well as the working one.
         */
        if(declared->returnRoot() && value && isMemoryType(global, expected)) {
            if(auto place = findPlace(value)) {
                value = borrowPlace(place.unwrap(), resolveBorrowType(module, expected, false),
                                    source, true);
            }
        }

        /*
         * The list stays positional, whatever any one argument turned out to be.
         *
         * Lowering pairs it with the callee's parameters by index and decides there which positions
         * survive - a declared unit is left out, a declared *variable* that is unit here is not,
         * since the erased body it was compiled from still reads a position for it. Both of those
         * need the entry to be here to be counted, so a hole punched at argument `i` does not drop
         * argument `i`: it drops argument `i + 1`, and every one after it.
         */
        out.push(positional ? positionalUnit(value, expected, source) : value);
    }
}

/*
 * A call to a function this call site has already settled on.
 *
 * Which of the two forms it takes is a property of the *callee* rather than of the selection that
 * found it: a generic one has its type arguments inferred from this call and is then specialized,
 * erased or expanded, and everything else is called directly. Every site that has a callee in hand
 * asks this - the ordinary call, a lens call site and a `for` loop - because each of them reaches
 * the same fork and none of them has anything to add to it.
 */
ModulePtr<Value> ExprResolver::emitKnownFunction(ModulePtr<Function> callee, Buffer<ResolvedArg> args,
                                                 LocationId source, TypePtr target, StringId resultName,
                                                 Buffer<TypePtr> solved) {
    return local[callee]->gen ? emitGenericCall(callee, args, source, target, resultName, solved)
                              : emitDirectCall(callee, args, source, target, resultName);
}

// Emits a call to a known function, converting each argument to its declared type. An intrinsic
// produces its result directly instead: the primitives are real functions with real bodies, but
// an ordinary call to one expands to the instruction it contains rather than to a call the
// backend would have to inline again later.
ModulePtr<Value> ExprResolver::emitDirectCall(ModulePtr<Function> callee, Buffer<ResolvedArg> args,
                                              LocationId source, TypePtr, StringId resultName) {
    auto function_ = local[callee];

    // An `=` callee whose result type its body decides has to have decided it before this call can
    // be given a type. Ordinarily it already has - resolveModuleBodies() settles them up front -
    // but one inferring function calling another declared after it arrives here first.
    requireReturnType(module, *function_, source);

    // The callee's signature is already in the caller's terms, so nothing is substituted through it.
    ArgList pending;
    auto anyDeferred = fillDeferred(callee, args, {}, source, pending);

    /*
     * Where this call's own packed-field write-backs start.
     *
     * By mark rather than wholesale, because the arguments were resolved before this was reached
     * and a nested call among them has already committed its own: `f(&h.a, g(&h.b))` commits `b`
     * after `g` and `a` after `f`, rather than committing `a` twice or `b` too late.
     */
    auto packed = packedMark();

    if(anyDeferred && function_->deferredIntrinsic) {
        auto expanded = function_->deferredIntrinsic(*this, toBuffer(pending), function_->returnType,
                                                     source, resultName);
        flushPackedBorrows(packed);
        return expanded;
    }

    // Positional only where the list becomes a call instruction. An intrinsic builds an instruction
    // out of the values it is handed, so a unit position there is an allocation nothing reads.
    ValueList converted;
    prepareArguments(callee, toBuffer(pending), {}, source, !function_->intrinsic, converted);

    if(function_->intrinsic) {
        auto expanded = function_->intrinsic(*this, toBuffer(converted), function_->returnType, source, resultName);
        flushPackedBorrows(packed);
        return expanded;
    }

    function_->used = true;
    auto call = create<InstCall>(source, resultName, function_->returnType, callee);

    // A position whose conversion failed is left out rather than declining the call: something has
    // reported on it, and lowering pairs what is left with the parameters that are still there.
    for(auto value: converted) {
        if(value) call->args.push(module.arena, value);
    }

    append(call);
    auto result = ref(call);

    // An aggregate result is returned through storage the caller provides, so it needs a local
    // for the same reason a constructed value does - see resolve/lower.cpp's Call case.
    if(isMemoryType(global, call->type)) {
        call->local = function.addLocal(module, call->type, resultName, result);
    }

    // The loan every `&` argument created ends with the call, so this is where a packed field is
    // told what the callee wrote - Design.md's tier 1.
    flushPackedBorrows(packed);
    return result;
}

/*
 * The selected class function, and the instance that served it.
 *
 * §1.2's second rule made concrete: this is the point of *decision*, so the reference recorded here
 * is the one an editor shows. `findClassFunctions` collected four candidates and recorded none of
 * them, because a call site showing all four would be showing something the program does not mean.
 *
 * The instance is the answer §1.2 calls the one hover most wants - which `Ord` served this
 * `compare` - and it is null exactly when the types that would decide are still variables here.
 */
void recordClassFunReference(ExprResolver& resolver, LocationId source, ClassMatch& match,
                             ModulePtr<ClassInstance> instance) {
    if(!resolver.context.index || source == kNullLocation || !match.typeClass) return;

    auto symbol = classFunSymbol(resolver.module, match.typeClass, match.index);

    // The result type at this occurrence, in the caller's terms rather than the class's.
    TypePtr type = nullptr;
    auto entry = resolver.global[match.typeClass]->functions.get(resolver.global, match.index);

    if(entry.fun) {
        type = substituteType(resolver.module, resolver.local[entry.fun]->returnType,
                              toBuffer(match.args), source);
    }

    recordReference(resolver.context, source, symbol, type, instance);
}

// The synthesized form: nothing here knew a set, so one is gathered for the name. The lookup uses
// the enclosing expression, which is the only location a call nobody wrote has.
ModulePtr<Value> ExprResolver::emitCall(StringId callName, Buffer<ResolvedArg> args, LocationId source,
                                        TypePtr target, StringId resultName, LocationId nameSource) {
    OverloadSet set;
    gatherOverloads(callName, args.length, nameSource != kNullLocation ? nameSource : source,
                    nameSource, set);

    return emitCall(set, args, source, target, resultName);
}

/*
 * Every class candidate matched against these arguments, and nothing decided.
 *
 * The facts only - see ClassSelection. Both selections run this and then differ in what they make of
 * it, which is what they genuinely differ in: an ordinary call defers an undecided match to a
 * generic dispatch, and a `for` loop has no dispatch to defer to.
 */
void ExprResolver::matchClassCandidates(const ClassFunList& candidates, Buffer<ResolvedArg> args,
                                        Buffer<const StringId> names, TypePtr target, ClassSelection& out) {
    for(auto& candidate: candidates) {
        ClassMatch match;

        if(!matchClassFun(candidate, args, names, target, match)) {
            if(match.undeclaredDependency && !out.undeclared) out.undeclared = candidate.typeClass;
            continue;
        }

        auto isUndecided = match.args.contains([&](TypePtr argument) { return isGeneric(global, argument); });

        if(isUndecided) {
            out.applicable.push(match.typeClass);
            out.undecided.push(::move(match));
        } else if(match.instance) {
            out.applicable.push(match.typeClass);
            if(!out.selectedCount) adopt(out.selected, match);
            out.selectedCount++;
        } else {
            if(!out.withoutInstanceCount) adopt(out.withoutInstance, match);
            out.withoutInstanceCount++;
        }
    }
}

/*
 * Which candidate of the set serves this call.
 *
 * Everything judged about a call happens here and nothing is emitted: the answer is one
 * ResolvedCallee, and a failure is a reported one. Selection is the only thing that knows what the
 * alternatives were, so it is the only thing that can say what was wrong with the call - which is
 * why every diagnostic about *choosing* a callee is in this function and none is outside it.
 */
void ExprResolver::selectCallee(const OverloadSet& set, Buffer<ResolvedArg> args, TypePtr target,
                                LocationId source, ResolvedCallee& out) {
    auto callName = set.name;
    auto nameSource = set.nameSource;
    auto& candidates = set.candidates;
    auto direct = set.direct;

    /*
     * A borrow is transparent for reading, and this is where that has to be said.
     *
     * `convert` already reads one through wherever a type is expected - `p.a :: Int` for a
     * `&Int` field is a load and always was - so a borrow reaches an argument position untouched
     * only when nothing there asked for a particular type. Dispatch is exactly that position: `p.a
     * + p.b` binds `Num`'s variable to `&Int` and then looks for an instance of it, and nobody
     * writes `instance Num(&Int)`.
     *
     * Written as a fallback rather than as a rule in the matcher, and the difference is what it
     * cannot break. A parameter declared `&T` still takes a borrow, because the arguments are only
     * rewritten when *nothing at all* accepts them as they stand - so every call that resolves
     * today resolves the same way, and this only turns a diagnostic into a call. The alternative,
     * teaching the matcher that `&T` also matches `T`, makes the two candidates overlap and needs a
     * rule saying which wins.
     *
     * Reading through is a load, so what it produces is a value of the borrowed type: for a scalar
     * a register, for an aggregate the address it already was. Whether the result may then be
     * *stored* is not decided here - checkTransfer answers that, and answers it the same way for a
     * borrow reached like this as for one written out.
     */
    ArgList readThrough;

    auto borrowed = false;
    for(auto& arg: args) borrowed = borrowed || (arg.isValue() && isBorrow(global, valueType(arg.value)));

    if(borrowed) {
        // Its own list rather than the one selection keeps: this match is against the arguments as
        // written, and the loop below may then replace them - so what it solved is not what the
        // selected callee would be emitted for. See the R5 test, which is the match that counts.
        TypeList probed;
        ArgList probedArgs;
        auto accepted = direct && matchFunction(direct, args, target, source, set, probed, probedArgs);

        /*
         * Matching is not enough: it is what fails *after* it that this is for.
         *
         * `Num`'s signature fits `+` with its variable bound to `&Int` - a class function is
         * declared over a variable, and a variable accepts anything - and the call dies at instance
         * selection, because nobody writes `instance Num(&Int)`. So the test is the one selection
         * itself makes: a candidate that selected an instance, or one still about this body's own
         * type variables, whose instance is decided later.
         *
         * Asked through `matchClassCandidates` rather than by classifying the matches again here,
         * which is what it used to do - the same two clauses, written a second time and free to
         * drift from the definition of "serves this call" that selection actually applies.
         */
        if(!accepted) {
            ClassSelection attempt;
            matchClassCandidates(candidates, args, toBuffer(set.names), target, attempt);

            accepted = attempt.selectedCount || attempt.undecided.isNotEmpty();
        }

        if(!accepted) {
            for(auto& arg: args) {
                auto type = arg.isValue() ? valueType(arg.value) : nullptr;

                readThrough.push(type && isBorrow(global, type)
                    ? ResolvedArg(convert(arg.value, ((BorrowType*)global[type])->to, source))
                    : arg);
            }

            args = toBuffer(readThrough);
        }
    }

    /*
     * The arguments the selected callee is emitted with, which is neither the list the call site
     * wrote nor a rebuild of it.
     *
     * Two things happened to it by the time a callee is chosen, and both belong to the *decision*
     * rather than to the emission. Whichever list the candidates were matched against is what the
     * one that won was chosen for - the read-through rewrite above is that - and the list is in the
     * winner's parameter order with its defaults filled in, since which parameter each argument
     * reaches is a question about the candidate. This is where a defaulted position stops being an
     * absence and becomes the constant it was declared as; nothing below selection knows there was
     * ever a position missing. See ArgMapping and ResolvedArg::defaulted.
     */
    auto commit = [&](ModulePtr<Function> signature, const ArgList& normalized) {
        replaceContents(out.args, normalized);
        materializeDefaults(signature, source, out.args);
    };

    auto anyNamed = false;
    for(auto name: set.names) anyNamed = anyNamed || name != 0;

    /*
     * A name no candidate could take, where there is one candidate to be specific about.
     *
     * The messages below are about *types* - which argument did not fit, which instance is missing -
     * and a name that no parameter has has nothing to do with types: "no class function scale
     * accepts (Int, Int)" is true, and says nothing at all about the `times:` that was wrong. So
     * where the call site wrote names and exactly one class function of this name exists, the reason
     * it could not be reached is asked out loud.
     *
     * One candidate only. With two, which one the author meant is exactly what is not known, and a
     * message about one of their parameter lists would be a guess.
     */
    auto reportNames = [&]() {
        if(!anyNamed) return false;

        ModulePtr<Function> sole = nullptr;
        Size count = 0;

        for(auto& candidate: candidates) {
            if(!candidate.typeClass) continue;

            auto entry = global[candidate.typeClass]->functions.get(global, candidate.index);
            if(!entry.fun) continue;

            sole = entry.fun;
            count++;
        }

        if(count != 1) return false;

        ArgMapping mapping;
        return !mapArguments(sole, toBuffer(set.names), args.length, 0, callName, source, true, mapping);
    };

    // Committing to the plain function, once it is the candidate the call is being served by. The
    // normalization is asked again here rather than carried from the R5 test, because the two paths
    // into this reach it having run that test and having skipped it - and it reports, because this
    // is the point the call has committed and there is no other candidate to fall back to.
    TypeList plainTypes;

    auto selectPlain = [&]() {
        ArgMapping mapping;
        if(!mapArguments(direct, toBuffer(set.names), args.length, set.shape.supplied, callName,
                         source, true, mapping)) {
            return;
        }

        recordReference(context, nameSource, functionSymbol(module, direct));

        ArgList normalized;
        normalizeArguments(mapping, args, normalized);
        commit(direct, normalized);

        out.kind = ResolvedCallee::Kind::Plain;
        out.function = direct;

        // What the R5 test below solved, where it ran at all - see ResolvedCallee::typeArgs.
        replaceContents(out.typeArgs, plainTypes);
    };

    /*
     * The plain function of this name that this call cannot reach.
     *
     * Reached only where nothing else serves the call, and then it is the whole diagnostic: a name
     * that is declared and called wrongly is far more often a mistake about that function than a
     * call meant for some class, so "takes two arguments but was given three" beats both "unknown
     * function" and the list of types no class function accepted. The reference is recorded for the
     * same reason an editor wants it - the author meant this function, and got the call wrong.
     *
     * *Why* it cannot be reached is the same question `gatherOverloads` asked to hold it here, asked
     * a second time with its answers turned on: a miscount, a name no parameter has, a name given
     * twice, or a position left out that has no default. One rule, and one place that states it -
     * see mapArguments.
     */
    auto reportMismatched = [&]() {
        recordReference(context, nameSource, functionSymbol(module, set.mismatched));

        auto declared = local[set.mismatched];

        // The wrong *kind* first, where the kind is required at all, because its arity is then
        // beside the point: a loop that names a plain function has nothing to be the body of
        // whatever that function's arity is. An ordinary call has no such case - a lens or an
        // iterator with its continuation written out is one - so it always falls through to arity.
        if(set.shape.requiresKind && declared->funKind != set.shape.kind) {
            context.diagnostics.error(set.shape.isLoop()
                ? "%@ is not an `iter fn`, so a `for` loop has nothing to be the body of - a collection is iterated by an `iter fn` over it rather than directly"_v
                : "%@ is not a `lens fn`, so there is nothing for the rest of this block to be the continuation of"_v,
                source, context.findName(declared->name));
            return;
        }

        /*
         * A loop's count message names the arguments written *before the body*, which the general
         * one has no way to know about - so it is stated here, and only for a loop whose call site
         * wrote no names at all. With a name in it the count is the least useful thing anyone could
         * be told: what went wrong is the name, and mapArguments is what says so.
         */
        if(set.shape.isLoop() && !anyNamed) {
            context.diagnostics.error("%@ takes %@ arguments before the loop body, but this call was given %@"_v,
                                      source, context.findName(callName),
                                      U32(declared->args.size() - set.shape.supplied), U32(args.length));
            return;
        }

        ArgMapping mapping;
        if(!mapArguments(set.mismatched, toBuffer(set.names), args.length, set.shape.supplied, callName,
                         source, true, mapping)) {
            return;
        }

        // Nothing about the arguments held it back, so what did is the kind - which is asked only
        // where a loop asks it, and answered above. Kept as a message rather than as nothing, since
        // a call that selects no callee and reports nothing is a call that silently disappears.
        context.diagnostics.error("%@ cannot be called here"_v, source, context.findName(callName));
    };

    /*
     * A class function of the name declared as the *other* kind, which fits this call exactly.
     *
     * The answer to `chunks(xs)` written as an ordinary call: `Chunked.chunks` is an `iter fn`, so
     * it is not a candidate here at all - but it is what the author meant, and saying so beats "no
     * class function chunks accepts (Array(Int))". Asked only where nothing of the right kind serves
     * the call, and answered only where one of these would have: a candidate of the wrong kind that
     * does not even fit is not evidence of anything. See OverloadSet::wrongKind.
     */
    auto reportWrongKind = [&]() {
        if(set.wrongKind.isEmpty()) return false;

        ClassSelection other;
        matchClassCandidates(set.wrongKind, args, toBuffer(set.names), target, other);
        if(!other.selectedCount) return false;

        auto entry = global[other.selected.typeClass]->functions.get(global, other.selected.index);
        auto declared = local[entry.fun];
        auto className = context.findName(global[other.selected.typeClass]->name);

        if(declared->funKind == ast::FunKind::Iter) {
            context.diagnostics.error("%@ is an `iter fn` of class %@, so it is run by a `for` loop rather than called - write `for x in %@(...)`"_v,
                                      source, context.findName(callName), className, context.findName(callName));
        } else if(declared->funKind == ast::FunKind::Lens) {
            context.diagnostics.error("%@ is a `lens fn` of class %@, so the rest of the block is its continuation - write it as a statement of its own, or as `let pat = %@(...)`, rather than in the middle of an expression"_v,
                                      source, context.findName(callName), className, context.findName(callName));
        } else {
            context.diagnostics.error("%@ is an ordinary class function of %@ rather than an `iter fn`, so a `for` loop has nothing to be the body of"_v,
                                      source, context.findName(callName), className);
        }

        return true;
    };

    // Nothing of this call's kind serves it, and the class half has nothing to say either. What is
    // left is a plain function of the name that cannot serve it, or one of the other kind that would
    // have - and failing both, the name means nothing here.
    auto reportUnserved = [&]() {
        if(set.mismatched) {
            reportMismatched();
            return;
        }

        if(reportWrongKind()) return;
        if(reportNames()) return;

        /*
         * A dot-call that found neither half, which is two answers rather than one - see
         * CallShape::receiver. Both are named because both were looked for, and the rule is stated
         * because this is the message that teaches it: the author who wrote `x.f(y)` and has neither
         * a field nor a function needs to know that either would have served.
         */
        if(set.shape.receiver) {
            context.diagnostics.error("%@ has no field %@, and no function %@ is visible here - `a.b(c)` reaches either a field of function type or `b(a, c)`"_v,
                                      source, describeType(context, global, set.shape.receiver),
                                      context.findName(callName), context.findName(callName));
            return;
        }

        context.diagnostics.error(set.shape.isLoop()
            ? "unknown iterator %@ - a `for` loop names an `iter fn`, or a class function declared as one"_v
            : "unknown function %@"_v, source, context.findName(callName));
    };

    /*
     * R5: a plain function is an ordinary member of the overload set, not a shadow over it. It wins
     * when it fits, which keeps "my definition beats the imported one" for the case that really
     * overlaps; when it doesn't fit, the class candidates are still reachable.
     *
     * The set is what says how the arguments reach the callee - its names, and how many trailing
     * parameters the call site does not write. A loop's callee declares the continuation the loop
     * supplies, so what is tested is the leading prefix the call site actually wrote. See
     * CallShape::supplied.
     */
    ArgList plainArgs;

    if(direct && (candidates.isEmpty() ||
                  matchFunction(direct, args, target, source, set, plainTypes, plainArgs))) {
        selectPlain();
        return;
    }

    if(candidates.isEmpty()) {
        reportUnserved();
        return;
    }

    ClassSelection matched;
    matchClassCandidates(candidates, args, toBuffer(set.names), target, matched);

    auto& selected = matched.selected;
    auto& withoutInstance = matched.withoutInstance;
    auto& undecided = matched.undecided;
    auto& applicable = matched.applicable;
    auto selectedCount = matched.selectedCount;
    auto withoutInstanceCount = matched.withoutInstanceCount;
    auto undeclared = matched.undeclared;

    /*
     * A match on this body's own type variables, left to the instantiation that will make them
     * concrete - but only where the call site has a dispatch to leave it to.
     *
     * A `for` loop does not: it needs the instance's implementation in hand, because the loop body
     * is desugared against it. So for one, an undecided match falls through to the diagnostics
     * below, where it is what it is - a class with no instance for these types, here.
     */
    if(set.shape.dispatches && !selectedCount && undecided.isNotEmpty()) {
        // A requirement the signature already declared wins over one that would have to be
        // inferred, so writing the constraint out is also how an overloaded name is settled.
        auto env = functionGen(global, function);
        Size chosen = 0;
        Size declaredCount = 0;

        for(Size i = 0; env && i < undecided.size(); i++) {
            if(!hasClassRequirement(global, *env, undecided[i].typeClass, toBuffer(undecided[i].args))) continue;

            chosen = i;
            declaredCount++;
        }

        if(declaredCount > 1 || (!declaredCount && undecided.size() > 1)) {
            context.diagnostics.error(
                "ambiguous call to %@ - more than one class applies, and the types that would decide are not known here. Name one class here (%@), or declare which one this function requires"_v,
                source, context.findName(callName),
                describeQualified(context, global, callName, toBuffer(applicable)));
            return;
        }

        // Selected, but not yet decided *which instance* - the types are still this body's own
        // variables. The class function is the answer either way, and the instance is left null,
        // which is what §1.3 means by recording the generic answer.
        recordClassFunReference(*this, nameSource, undecided[chosen], nullptr);

        out.kind = ResolvedCallee::Kind::Dispatch;
        adopt(out.match, undecided[chosen]);

        // Against the *class* signature, which is where a default a dispatched call fills came from:
        // the instance is not known here, and an instance does not get to declare one - see
        // resolveInstance, which reports one written in an instance body.
        commit(global[out.match.typeClass]->functions.get(global, out.match.index).fun,
               out.match.normalized);
        return;
    }

    if(selectedCount > 1) {
        context.diagnostics.error(set.shape.isLoop()
            ? "ambiguous iterator %@ - more than one class instance applies. Name one class here (%@)"_v
            : "ambiguous call to %@ - more than one class instance applies. Name one class here (%@)"_v,
                                  source, context.findName(callName),
                                  describeQualified(context, global, callName, toBuffer(applicable)));
        return;
    }

    if(!selectedCount) {
        // Nothing in the class half of the overload set fits. A plain function of this name is then
        // the only candidate left, and its own diagnostic - which argument did not fit, and what it
        // was declared as - says more than the list of types the classes would not take.
        if(direct) {
            selectPlain();
            return;
        }

        if(set.mismatched) {
            reportMismatched();
            return;
        }

        if(reportWrongKind()) return;

        // Ahead of everything below, because everything below is about types - see reportNames.
        if(reportNames()) return;

        StringBuilder types;

        // The signature fit and the dependency had nothing to answer with. Naming the constraint is
        // the whole diagnostic: the call is right, and what is missing is the promise that gives
        // the determined parameter a name in this body.
        if(undeclared) {
            context.diagnostics.error("%@ needs to know what %@ determines here, and this function does not require it - declare the constraint, as `fn (%@(...)) %@(...)`"_v,
                                      source, context.findName(callName),
                                      context.findName(global[undeclared]->name),
                                      context.findName(global[undeclared]->name),
                                      context.findName(function.name));
            return;
        }

        /*
         * A candidate whose signature fit and whose types have no instance.
         *
         * An undecided match is one of these for a call site that cannot dispatch: its types are
         * this body's own variables, and with no dispatch to defer to, what it amounts to here is a
         * class with no instance for these types. It is the *matching* candidate that is named
         * either way - the `for` loop's own copy of this blamed whichever class came first, which
         * with two of them sharing a name reported a missing instance of a class the call was never
         * about.
         */
        auto unserved = withoutInstanceCount    ? &withoutInstance
                      : undecided.isNotEmpty()  ? &undecided[0]
                                                : nullptr;

        if(unserved) {
            describeTypes(context, global, toBuffer(unserved->args), types);

            context.diagnostics.error(set.shape.isLoop()
                ? "no instance of %@ for (%@), required by the `for` loop's %@"_v
                : "no instance of %@ for (%@), required by %@"_v, source,
                                      context.findName(global[unserved->typeClass]->name),
                                      types.view(), context.findName(callName));
        } else {
            TypeList given;
            auto broken = false;
            for(auto& arg: args) {
                auto type = valueType(arg.value);
                if(global[type]->kind == Type::Error) broken = true;
                given.push(type);
            }

            // An argument that is already an error has had its diagnostic. Reporting that no
            // instance accepts it names the failure a second time, in terms of a type the author
            // never wrote - see the `<error>` in "no class function * accepts (Int, <error>)".
            if(broken) return;

            describeTypes(context, global, toBuffer(given), types);

            context.diagnostics.error("no class function %@ accepts (%@)"_v, source, context.findName(callName),
                                      types.view());
        }

        return;
    }

    if(!local[selected.instance]->functions.get(local, selected.index)) {
        context.diagnostics.error("instance of %@ does not implement %@"_v, source,
                                  context.findName(global[selected.typeClass]->name), context.findName(callName));
        return;
    }

    recordClassFunReference(*this, nameSource, selected, selected.instance);

    out.kind = ResolvedCallee::Kind::Instance;
    adopt(out.match, selected);

    // The class signature and not the instance's implementation, for the reason the dispatch case
    // gives: what a call site may leave out is fixed before an instance is selected.
    commit(global[out.match.typeClass]->functions.get(global, out.match.index).fun, out.match.normalized);
}

ModulePtr<Value> ExprResolver::emitCall(const OverloadSet& set, Buffer<ResolvedArg> args,
                                        LocationId source, TypePtr target, StringId resultName) {
    /*
     * Three things are spelled as a position holding no value, and only one of them is a reason to
     * stop.
     *
     * A failed argument is: something has reported on it already, and matching an overload set
     * against a type nobody worked out would report a second, worse diagnostic about a call the
     * author may not have got wrong. A deferred position holds no value on purpose - it is not one
     * yet. And a value of unit type holds none because that is how this resolver spells a value that
     * carries nothing, which `valueType` already answers `{}` for and which every overload rule
     * handles without knowing it was absent.
     *
     * The third used to be caught here as the first, which made `f({})` resolve to nothing at all
     * for any generic `f` - silently, since the whole point of this guard is that the diagnostic was
     * already written. Which position is which is the call site's knowledge, not this one's, and it
     * arrives in the argument itself.
     */
    if(anyArgumentFailed(args)) return nullptr;

    ResolvedCallee callee;
    selectCallee(set, args, target, source, callee);

    /*
     * Emission, with nothing left to decide.
     *
     * Four call forms and one switch over which was selected. What used to be here instead was the
     * selection itself, with each form reached from the middle of the rule that found it - so
     * "which callee serves this call" and "how is a call to it built" were one function that could
     * only be read top to bottom, and a form was reachable only by re-deriving the path that got
     * there. See ResolvedCallee.
     */
    auto selected = toBuffer(callee.args);

    switch(callee.kind) {
        case ResolvedCallee::Kind::Failed:
            return nullptr;

        case ResolvedCallee::Kind::Plain:
            return emitKnownFunction(callee.function, selected, source, target, resultName,
                                     toBuffer(callee.typeArgs));

        case ResolvedCallee::Kind::Instance:
            return emitInstanceCall(module, callee.match.instance, toBuffer(callee.match.instanceArgs),
                                    callee.match.index, selected, source, target, resultName);

        case ResolvedCallee::Kind::Dispatch:
            return emitGenericDispatch(callee.match, selected, source, resultName);
    }

    return nullptr;
}

// See the declaration in expr.h for why a synthesized class call may not ask for an implementation.
ModulePtr<Value> ExprResolver::emitClassMember(GlobalPtr<TypeClass> typeClass, U16 index, TypePtr subject,
                                               Buffer<ResolvedArg> args, LocationId source,
                                               bool* noInstance) {
    if(noInstance) *noInstance = false;

    ClassMatch match;
    match.typeClass = typeClass;
    match.index = index;
    match.args.push(subject);

    /*
     * Still this body's own variable, so which instance runs is not knowable here and the call is
     * the deferred one selectCallee's undecided branch builds. `emitGenericDispatch` records the
     * requirement and asks for the witness slot, exactly as it does for a call the author wrote.
     */
    if(isGeneric(global, subject)) return emitGenericDispatch(match, args, source, StringId());

    match.instance = selectInstance(typeClass, toBuffer(match.args), match.instanceArgs);

    if(!match.instance) {
        if(noInstance) *noInstance = true;
        return nullptr;
    }

    return emitInstanceCall(module, match.instance, toBuffer(match.instanceArgs), index, args, source);
}

/*
 * Generic calls.
 */

ModulePtr<Value> ExprResolver::emitGenericDispatch(ClassMatch& match, Buffer<ResolvedArg> args,
                                                   LocationId source, StringId resultName,
                                                   TypePtr resultType) {
    auto env = functionGen(global, function);
    if(!env) {
        // Nothing outside a generic body has a type variable to be undecided about.
        context.diagnostics.error("internal: a class call was deferred outside a generic function"_v, source);
        return nullptr;
    }

    requireClass(module, function, match.typeClass, toBuffer(match.args), source);

    // Whatever the declared constraints imply, the dispatch itself needs a witness, and a witness
    // needs a slot. See GenEnv::dispatched.
    requireClassSlot(module, *env, match.typeClass, toBuffer(match.args), source);

    auto typeClass = global[match.typeClass];
    auto entry = typeClass->functions.get(global, match.index);
    auto signature = local[entry.fun];
    if(!resultType) resultType = substituteType(module, signature->returnType, toBuffer(match.args), source);

    // The instance is not known here, so there is nothing that can see through a deferred argument:
    // it becomes the thunk whichever implementation is selected will call.
    ArgList pending;
    fillDeferred(entry.fun, args, toBuffer(match.args), source, pending);

    auto call = create<InstGenCall>(source, resultName, resultType, entry.fun, match.typeClass, match.index);
    for(auto argument: match.args) call->typeArgs.push(module.arena, argument);

    /*
     * The arguments restated in the class signature, and deliberately no conventions applied.
     *
     * `substituteArguments` is the shared half - read the parameter at the types this call bound,
     * convert, and keep the list positional - and it is the same step a generic call and a
     * parametric instance's implementation take. What this form does *not* take is
     * prepareArguments, and that is the whole of the difference.
     *
     * A parameter's convention is not the caller's to apply here, because the callee is not a
     * function this call site reaches: lowering loads the implementation out of a witness and adapts
     * each argument to the erased ABI itself, materializing a scalar into storage where the
     * parameter is declared as a type variable - see lower.cpp's GenCall case. Handing it a borrow
     * instead of the value puts a second indirection in front of that adaptation, which is what
     * `fn (Index(c, k, v)) get(return self: c, ...)` turned into: a `&c` where the boundary expected
     * a `c`, and an assertion in the width it went on to load. The convention is applied where the
     * dispatch becomes a concrete call, which is emitInstanceCall's route into emitDirectCall.
     *
     * A `&` parameter is the one position where sharing this step is a decision rather than an
     * identity: `substituteArguments` leaves it as written, which is the rule lowering states from
     * the other side - *"a `&` parameter is an address in both worlds, so there is nothing to
     * adapt"*. Dispatch.Borrow.yana is the fixture that reaches one, and the IR is unchanged by it.
     *
     * The thunk stays here, because it is a convention: a deferred position becomes the closure
     * whichever implementation is selected will call, and there is no later form to make it.
     */
    ArgList converted;
    substituteArguments(entry.fun, toBuffer(pending), toBuffer(match.args), source, converted);

    /*
     * An argument past the last declared parameter is the loop's continuation, and it travels as it
     * stands - `fillDeferred` and `substituteArguments` both leave a position with no parameter
     * behind it alone, and there is nothing to read a convention or a substitution off anyway. It
     * is what the desugaring would have declared had a class been able to declare it.
     */
    for(Size i = 0; i < converted.size(); i++) {
        if(i < signature->args.size() && local[signature->args.get(local, i)]->isLazy()) {
            call->args.push(module.arena, makeThunk(pending[i].promise, pending[i].promise.type, source));
            continue;
        }

        if(converted[i].value) call->args.push(module.arena, converted[i].value);
    }

    append(call);
    auto result = ref(call);
    if(isMemoryType(global, resultType)) call->local = function.addLocal(module, resultType, resultName, result);

    return result;
}

/*
 * The erased call.
 *
 * Everything the callee needs to know about the types it was instantiated for travels as one
 * constant environment, built for exactly this argument list and interned. What the call itself
 * looks like is unchanged - the same arguments in the same order - because the environment goes in
 * a hidden leading position that only lowering ever names.
 *
 * Null when the environment could not be built, which today means the callee has a requirement no
 * witness exists for yet. The caller then specializes instead, which is always available for a
 * concrete argument list and is what keeps this a staged optimization rather than a cliff.
 */
ModulePtr<Value> ExprResolver::emitErasedCall(ModulePtr<Function> callee, Buffer<TypePtr> typeArgs,
                                              Buffer<ResolvedArg> args, LocationId source,
                                              StringId resultName) {
    auto generic = local[callee];
    auto resultType = substituteType(module, generic->returnType, typeArgs, source);

    generic->used = true;
    generic->genericallyUsed = true;

    /*
     * Every argument's convention still applies.
     *
     * An erased call is a different *representation* of the same call, so the callee's conventions
     * mean exactly what they meant - a `&` parameter is still a mutable borrow of the caller's
     * storage, a `->` still consumes, and a `return` argument is still loaned for the result's
     * lifetime. Reading the value instead would hand a `&` parameter a copy, and the writes it made
     * would land somewhere the caller never looks.
     */
    auto packed = packedMark();

    // Read at the types this call decided, since the signature is written in the callee's variables.
    ArgList pending;
    fillDeferred(callee, args, typeArgs, source, pending);

    ValueList converted;
    prepareArguments(callee, toBuffer(pending), typeArgs, source, true, converted);

    for(auto value: converted) {
        if(!value) return nullptr;
    }

    // The environment itself is filled in by prepareGenericCalls, once the whole program has been
    // resolved. It cannot be built here: a slot number comes from a finished context, and the
    // callee's context is still collecting requirements while its body is being resolved.
    auto call = create<InstGenCall>(source, resultName, resultType, callee, nullptr, 0);

    for(auto argument: typeArgs) call->typeArgs.push(module.arena, argument);
    for(auto value: converted) call->args.push(module.arena, value);

    append(call);
    auto result = ref(call);
    if(isMemoryType(global, resultType)) call->local = function.addLocal(module, resultType, resultName, result);

    flushPackedBorrows(packed);

    return result;
}

/*
 * Generating a generic intrinsic at the call site.
 *
 * A concrete intrinsic - Core's `Num(Int).+` - is a real function whose body an ordinary call
 * expands instead of calling. A generic one has no body at all: `fn (a) *(it: %a) -> a` is not
 * one operation but one per element type, so there is nothing to write down until the call says
 * which. The type arguments are therefore handed to the intrinsic through the substituted result
 * type, which is all any of them needs.
 */
/*
 * Whether two places name the same storage, for the sink check below.
 *
 * The *root* only, and deliberately: an emitter may sink a converted or projected form of what it
 * was handed - emitExchange sinks `convert(args[1], ...)` - and what the check is asking is whether
 * the caller's storage was consumed, not whether the emitter passed the argument on unaltered.
 */
static bool sameRoot(const Place& a, const Place& b) {
    if(a.root != b.root) return false;

    switch(a.root) {
        case PlaceRoot::Local: return a.local == b.local;
        case PlaceRoot::Global: return a.global == b.global;
        case PlaceRoot::Pointer:
        case PlaceRoot::Borrow: return a.pointer == b.pointer;
    }

    return false;
}

ModulePtr<Value> ExprResolver::expandIntrinsic(ModulePtr<Function> callee, Buffer<TypePtr> typeArgs,
                                               Buffer<ResolvedArg> args, LocationId source,
                                               StringId resultName) {
    auto generic = local[callee];
    auto resultType = substituteType(module, generic->returnType, typeArgs, source);

    // An intrinsic that takes a `&` parameter makes the borrow itself - see exchangedPlace in
    // core.cpp - so the loan it creates ends here, where the operation it was made for has been
    // emitted. `swap(&h.a, &h.b)` on two co-packed fields commits both, in order.
    auto mark = packedMark();
    ModulePtr<Value> result = nullptr;

    /*
     * The record the `->` check below reads - see `sunkPlaces` in expr.h for what it is for.
     *
     * The previous value of the flag is restored rather than cleared, because an emitter may expand
     * a second intrinsic inside itself: `convert` reaches one, and a nested expansion truncating the
     * list back to its own mark is what keeps the outer one's sinks visible to the outer check.
     */
#if defined(_DEBUG) || defined(DEBUG)
    auto sinkMark = sunkPlaces.size();
    auto wasRecording = recordingSinks;
    recordingSinks = true;
#endif

    if(generic->deferredIntrinsic) {
        // The declared type of each deferred parameter, at the types this call decided. It is what
        // the argument is resolved and converted against when the expansion runs it.
        ArgList pending;
        fillDeferred(callee, args, typeArgs, source, pending);

        result = generic->deferredIntrinsic(*this, toBuffer(pending), resultType, source, resultName);
    } else {
        // An ordinary intrinsic is handed values: every position of this call has one by now, since
        // a `@lazy` parameter is what the branch above exists for.
        ValueList values;
        for(auto& arg: args) values.push(arg.value);

        result = generic->intrinsic(*this, toBuffer(values), resultType, source, resultName);
    }

    flushPackedBorrows(mark);

    /*
     * And the check: a parameter this callee declares `->` on must have had its storage sunk while
     * the emitter ran.
     *
     * It is the same shape of invariant as verifyLocals' untracked-droppable check and it exists for
     * the same reason - a producer that forgets one line leaves a program that consumes one value
     * twice with nothing to say about it. `intoValue` forgot exactly this: `->self` was declared,
     * expandIntrinsic applied nothing, and `intoValue(c)` twice compiled.
     *
     * Only where the argument names a place. A value that is not storage - a literal, a call's
     * result in a register - has nothing for a handover to take over, which is the same condition
     * `sinkValue` returns early on.
     */
#if defined(_DEBUG) || defined(DEBUG)
    recordingSinks = wasRecording;

    if(!context.diagnostics.errorCount()) {
        for(Size i = 0; i < generic->args.size() && i < args.size(); i++) {
            auto declared = local[generic->args.get(local, i)];
            if(declared->convention != ast::BindType::Sink) continue;
            if(!args[i].value) continue;

            auto place = findPlace(args[i].value);
            if(!place) continue;

            auto sunk = false;
            for(Size s = sinkMark; s < sunkPlaces.size(); s++) {
                if(sameRoot(sunkPlaces[s], place.unwrap())) { sunk = true; break; }
            }

            if(!sunk) {
                context.diagnostics.error("internal error: %@ declares `->` on argument %@ and its intrinsic did not sink it - the caller's storage is never moved out of, so the value can be consumed again"_v,
                                          source, context.findName(generic->name), U32(i + 1));
            }
        }
    }

    sunkPlaces.resize(sinkMark);
#endif

    return result;
}

/*
 * A type variable a call cannot decide and does not have to.
 *
 * Selection skips a `@lazy` position - the argument is not resolved, and resolving it to find out
 * would evaluate it, which is the whole of what the marker says not to do. So a variable occurring
 * *only* in `@lazy` positions is never bound by any call, however concrete the arguments are:
 * `fn (Truth(a), Truth(b)) &&(lhs: a, @lazy rhs: b)` cannot even be called with two `Bool`s.
 *
 * What this asks is whether that is survivable rather than fatal, and the answer is a property of
 * where the variable occurs. It has to be absent from the result, because a caller reads that type;
 * absent from every strict parameter, or the ordinary binding would have decided it; and present in
 * at least one deferred one, or there is nothing lazy about the situation at all.
 *
 * When all three hold, the only thing that ever reads the variable is the *thunk* built for that
 * argument - it is the closure's return type - and a callee that expands rather than being called
 * builds no thunk: the operand is emitted where it is used, and its own type is whatever it turns
 * out to be. So an expansion needs nothing here, and this is what lets it proceed.
 *
 * A callee that is *not* expanded needs the type, and gets it from the thunk it is about to be
 * handed - see inferDeferredArguments, which is where the third condition is dropped: a variable the
 * result mentions is one an expansion cannot leave open and a call can perfectly well read off the
 * argument.
 */
static bool onlyDeferredPositions(GlobalBase global, ModuleBase local, Function& signature, Size index) {
    auto deferred = false;

    for(Size i = 0; i < signature.args.size(); i++) {
        auto arg = local[signature.args.get(local, i)];
        if(!mentionsVariable(global, arg->declaredType(), U16(index))) continue;
        if(!arg->isLazy()) return false;

        deferred = true;
    }

    return deferred;
}

static bool deferredOnlyVariable(GlobalBase global, ModuleBase local, Function& signature, Size index) {
    if(mentionsVariable(global, signature.returnType, U16(index))) return false;
    return onlyDeferredPositions(global, local, signature, index);
}

// Every slot the solve left empty that the rule above forgives, filled with the callee's own
// variable so that substitution has something to walk. Answers whether the solve may proceed.
static bool fillDeferredHoles(GlobalBase global, ModuleBase local, Function& signature,
                              GenEnv& calleeEnv, Solution& solution) {
    auto filled = false;

    for(Size i = 0; i < solution.types.size(); i++) {
        if(solution.types[i]) continue;
        if(!deferredOnlyVariable(global, local, signature, i)) return false;

        solution.types[i] = (Type*)global[calleeEnv.types.get(global, i)] - global;
        filled = true;
    }

    if(filled) solution.state = Solution::State::Solved;
    return filled;
}

/*
 * What asking the argument answered - see inferDeferredArguments.
 */
enum class DeferredInference: U8 {
    // No open variable is one only a deferred position mentions, so there is nothing here to ask.
    None,

    // The thunk could not be built, and building it said why.
    Reported,

    // `out` holds the arguments with their thunks, and `solution` is the solve re-run over them.
    Inferred,
};

/*
 * The hole the argument itself fills - the other half of deferredOnlyVariable.
 *
 * A variable that occurs only in `@lazy` positions is decided by nothing the *call* says, and that
 * is the end of it for a callee that expands. A callee that is called is handed a closure, and the
 * closure has a return type: build it, read the type off it, and the variable is answered by the
 * only thing that was ever going to answer it. `fn (Truth(a), Truth(b)) myAnd(lhs: a, @lazy rhs: b)`
 * is the case, and this is what makes it callable.
 *
 * Nothing about the ordering rule changes, which is what kept this out of the first version. A
 * `@lazy` argument is still resolved *against* the parameter type wherever there is one to resolve
 * it against - `m ?? 0` still takes its `0` at the type the left operand decided - because a
 * position that reaches this one has, by construction, no other position to have taken a type from.
 * The variable is absent from every strict parameter, so nothing was pushed down that this now
 * overrides; the argument is resolved on its own terms because its own terms are all there are.
 *
 * The thunk is built *here* rather than at prepareArguments, where every other one is, and is
 * carried on the promise so that the later build finds it and hands on what already exists. That is
 * the same forwarding rule a `@lazy` parameter passed straight on already uses - see makeThunk - so
 * the argument is resolved once, in the closure, and the call site emits one closure for it.
 *
 * The solve is then re-run rather than patched. What has changed is that a position which bound
 * nothing now binds something, which is the input to a solve and not a correction to its output: a
 * nested occurrence - `@lazy rhs: [b]` - is matched structurally, two deferred positions naming one
 * variable are reconciled by the rules every position gets, and an argument whose type does not fit
 * the pattern its parameter wrote comes back as the ordinary `Argument` failure with a position on
 * it.
 */
static DeferredInference inferDeferredArguments(ExprResolver& resolver, ModulePtr<Function> callee,
                                                Buffer<ResolvedArg> args, TypePtr target,
                                                LocationId source, Solution& solution, ArgList& out) {
    auto global = resolver.global;
    auto local = resolver.local;
    auto& signature = *local[callee];

    /*
     * Which variables the arguments would have to answer, and whether they are all of that kind.
     * An open variable of any other sort is not this rule's to forgive - resolving an argument
     * cannot answer it, and the general diagnostic is the true thing to say about it.
     */
    U64 wanted = 0;

    for(Size i = 0; i < solution.types.size(); i++) {
        if(solution.types[i]) continue;
        if(i >= 64 || !onlyDeferredPositions(global, local, signature, i)) return DeferredInference::None;

        wanted |= U64(1) << i;
    }

    if(!wanted) return DeferredInference::None;

    out.clear();
    for(Size i = 0; i < args.length; i++) out.push(args[i]);

    auto errors = resolver.context.diagnostics.errorCount();
    auto built = false;

    for(Size i = 0; i < out.size() && i < signature.args.size(); i++) {
        if(!out[i].isDeferred() || out[i].promise.thunk) continue;

        auto declared = local[signature.args.get(local, i)]->declaredType();

        U64 mentioned = 0;
        genVariablesIn(global, declared, mentioned);
        if(!(mentioned & wanted)) continue;

        TypePtr inferred = nullptr;
        auto thunk = resolver.inferThunk(out[i].promise, source, inferred);

        // Reported by the build - a capture it may not take, a generic body it may not sit in.
        // Silent only where there was nothing to build from, which the caller says its own thing
        // about.
        if(!thunk) return errors == resolver.context.diagnostics.errorCount()
            ? DeferredInference::None : DeferredInference::Reported;

        out[i].promise.thunk = thunk;
        out[i].promise.type = inferred;
        built = true;
    }

    if(!built) return DeferredInference::None;

    solveSignature(resolver, callee, toBuffer(out), target, source, Unresolved::Binds, solution);
    return DeferredInference::Inferred;
}

ModulePtr<Value> ExprResolver::emitGenericCall(ModulePtr<Function> callee, Buffer<ResolvedArg> args,
                                               LocationId source, TypePtr target, StringId resultName,
                                               Buffer<TypePtr> solved) {
    auto generic = local[callee];
    auto calleeEnv = functionGen(global, *generic);

    if(!calleeEnv || generic->args.size() != args.length) {
        return emitDirectCall(callee, args, source, target, resultName);
    }

    /*
     * The same solve matchFunction ran to decide that this callee fits, performed rather than
     * asked. The two used to be written out separately: this one reports and that one may not, and
     * everything else about them - the one-directional rule, the expected result filling only what
     * the arguments left open, the dependencies, the settle - was the same code twice.
     *
     * The failure states are what is read back here. A solve says which position it stopped at and
     * about what, and the messages below are this call site's account of that - the general one,
     * and the two that name a capability rather than a type.
     */
    Solution solution;

    if(solved.length == calleeEnv->types.size() && solved.length) {
        /*
         * Selection already decided these types, against this very argument list and this very
         * expected result, and handed them over rather than throwing them away - see
         * ResolvedCallee::typeArgs. Solving again would ask the same question of the same inputs.
         *
         * A length that does not match is not one of these: an empty list is a selection that never
         * matched this callee at all (R1 admits one plain function per name and arity, so a set with
         * no class candidates commits to it without a match), and every other caller of this
         * function reaches a callee it has not solved.
         */
        for(auto type: solved) solution.types.push(type);
    } else {
        solveSignature(*this, callee, args, target, source, Unresolved::Binds, solution);
    }

    /*
     * A variable only a `@lazy` position mentions, answered by the argument that mentions it - see
     * inferDeferredArguments.
     *
     * Before the failure states are read back, because it re-runs the solve and what comes out of
     * that is what this call is. A deferred intrinsic is deliberately not offered it: it emits its
     * operand where it runs it rather than being handed a closure, so building the closure to learn
     * the type would build the one thing that callee exists to avoid. fillDeferredHoles is what
     * answers for it, just below.
     */
    ArgList inferred;

    if(solution.state == Solution::State::Undecided && !generic->deferredIntrinsic) {
        switch(inferDeferredArguments(*this, callee, args, target, source, solution, inferred)) {
            case DeferredInference::Inferred: args = toBuffer(inferred); break;
            case DeferredInference::Reported: return nullptr;
            case DeferredInference::None: break;
        }
    }

    if(solution.state == Solution::State::Argument) {
        // A deferred position that failed is one the inference above resolved, so what it produced
        // is on the promise rather than in a value - naming `{}` for it would report the null a
        // deferred position always carries instead of the type the argument turned out to have.
        auto produced = args[solution.position].isDeferred() ? args[solution.position].promise.type
                                                             : TypePtr(nullptr);
        auto given = produced ? produced : valueType(args[solution.position].value);
        auto declared = local[generic->args.get(local, solution.position)]->declaredType();

        /*
         * A fixed array where a growable one was asked for - Implementation-Containers.md §6's
         * "it is never a growable argument. The diagnostic says so directly: a fixed array
         * cannot be pushed to."
         *
         * Said here rather than left to the general message because the general message is
         * true and useless: `[Int *4]` does not fit `Array(a)` for a reason that is the whole
         * design - growth is nominal, so the operations that grow name the growable type - and
         * a reader who has just watched `[Int *4]` pass to five other `[Int]` functions needs
         * to be told which capability this one wanted instead of which types failed to unify.
         */
        if(fixedElement(module, given) && isGrowableArray(module, declared)) {
            context.diagnostics.error("%@ cannot be passed to %@, which asks for a growable array - a fixed array holds exactly the elements its type names and cannot grow. Only the operations that grow say `Array`; everything that reads says `[T]` and accepts this"_v,
                                      source, describeType(context, global, given),
                                      context.findName(generic->name));
            return nullptr;
        }

        /*
         * A `Chunked` container where a `[T]` was asked for - Implementation-Containers.md §5.
         *
         * Said here as well as in convert(), because the two positions fail in different places:
         * a concrete `[Int]` parameter reaches the conversion and reports there, while a `[a]`
         * one fails at the binding above and never has a slice type to convert to. The message
         * is the same one because the mistake is, and what fixes it is the parameter.
         */
        if(sliceElement(module, declared) && chunkedElement(module, given)) {
            context.diagnostics.error("%@ is `Chunked` and not `Contiguous`, so it cannot be passed to %@, which asks for a slice - its elements are not one buffer, and flattening them would be a copy this position does not say it makes. A function that only reads elements should take `fn (Chunked(c, a)) f(xs: c)` instead, which this container satisfies"_v,
                                      source, describeType(context, global, given),
                                      context.findName(generic->name));
            return nullptr;
        }

        context.diagnostics.error("argument %@ of %@ is %@, which does not fit %@"_v, source,
                                  U32(solution.position + 1), context.findName(generic->name),
                                  describeType(context, global, given),
                                  describeType(context, global, declared));
        return nullptr;
    }

    /*
     * A hole an expansion does not need - see deferredOnlyVariable.
     *
     * Only for a callee that will actually expand, which is what makes the omission safe rather than
     * deferred: a deferred intrinsic emits its `@lazy` operand where it runs it, so the thunk whose
     * return type this variable would have been is never built.
     */
    auto expandingWithHoles = solution.state == Solution::State::Undecided && generic->deferredIntrinsic &&
                              fillDeferredHoles(global, local, *generic, *calleeEnv, solution);

    // A specialization is made for concrete types, so a literal variable the call left open settles
    // to its default before it becomes one of them - and a variable nothing decided at all is one
    // this call site has to say out loud.
    if(solution.state == Solution::State::Undecided) {
        auto variable = context.findName(global[calleeEnv->types.get(global, solution.position)]->name);

        /*
         * The same hole, where nothing filled it - see deferredOnlyVariable.
         *
         * A callee that is *called* answers this for itself now: the argument is resolved into the
         * thunk it becomes and the type is read back off it, which is what inferDeferredArguments
         * does. What is left here is the callee that expands - it is handed the argument rather
         * than a closure over it, so there is no thunk for the variable to have been the return
         * type of, and the general advice is wrong for it: no expected type can reach a variable
         * the result does not mention.
         */
        if(deferredOnlyVariable(global, local, *generic, solution.position)) {
            context.diagnostics.error("%@ declares %@ only in `@lazy` positions and expands them where it runs them, so nothing here decides it - a deferred parameter's type is read off the thunk the argument becomes, and a callee that emits the argument in place builds no thunk. Give that parameter a type the other arguments bind, or the same variable as one of them"_v,
                                      source, context.findName(generic->name), variable);
            return nullptr;
        }

        context.diagnostics.error("cannot infer type argument %@ of %@ here - give the expected type"_v, source,
                                  variable, context.findName(generic->name));
        return nullptr;
    }

    auto& bindings = solution.types;

    ArgList converted;
    substituteArguments(callee, args, toBuffer(bindings), source, converted);

    auto undecided = bindings.contains([&](TypePtr binding) { return isGeneric(global, binding); });

    // The holes above are generic on purpose, so this is decided before `undecided` is consulted:
    // what is left open is a type nothing reads, not a call waiting for a caller to make concrete.
    if(expandingWithHoles) {
        return expandIntrinsic(callee, toBuffer(bindings), toBuffer(converted), source, resultName);
    }

    if(!undecided) {
        // A generic intrinsic has nothing to specialize: what it means is generated here from the
        // types the call decided, so there is no body to clone and no function to call. This is
        // what keeps a pointer dereference one load rather than a call per element access.
        if(generic->intrinsic || generic->deferredIntrinsic) {
            return expandIntrinsic(callee, toBuffer(bindings), toBuffer(converted), source, resultName);
        }

        // Both forms are first-class outputs, and which one a concrete call site takes is a choice
        // rather than a property of the callee - see Program::Specialization. Taking the erased path
        // needs the body first, since the body is what collects the requirements the environment has
        // to supply.
        if(module.program.specialization == Program::Specialization::Generic &&
           resolveFunctionBody(*generic->module, *generic) &&
           genericBodyLowerable(module, callee)) {
            if(auto call = emitErasedCall(callee, toBuffer(bindings), toBuffer(converted), source, resultName)) {
                return call;
            }
        }

        auto specialized = instantiateFunction(module, callee, toBuffer(bindings), source);
        if(!specialized) return nullptr;

        return emitDirectCall(specialized, toBuffer(converted), source, target, resultName);
    }

    auto env = functionGen(global, function);
    if(!env) {
        /*
         * A concrete call site whose callee still has an open type argument, and no enclosing
         * signature for the requirement to be passed up to.
         *
         * This read "internal:" and was reachable from ordinary source, which is the part worth
         * fixing: `sort(r)` for a container with a `Contiguous` instance and no `Writable` one gets
         * here, because `Writable(c -> a)` is what would have decided `a` and there is no instance
         * to decide it from. `Sort.yana`'s own note called this out as "rejected badly" when a fixed
         * array reached it for the same reason.
         *
         * What the caller can act on is the class it did not satisfy, so that is what is named -
         * and the argument that is still open beside it, since a variable no instance bound is the
         * observable half of the same fact.
         */
        for(auto constraint: calleeEnv->classes.contents(global)) {
            if(!constraint.typeClass) continue;

            auto unresolved = false;
            SmallArray<TypePtr, 4> classArgs;

            for(auto arg: constraint.args.contents(global)) {
                auto bound = substituteType(module, arg, toBuffer(bindings), source);
                unresolved = unresolved || isGeneric(global, bound);
                classArgs.push(bound);
            }

            if(!unresolved) continue;

            context.diagnostics.error("no instance of %@ for (%@), required by %@ - the class is what would decide the rest of this call's type arguments, so there is nothing left to infer them from"_v,
                                      source,
                                      context.findName(global[constraint.typeClass]->name),
                                      describeType(context, global, classArgs.size() ? classArgs[0] : nullptr),
                                      context.findName(generic->name));
            return nullptr;
        }

        context.diagnostics.error("cannot infer the type arguments of %@ at this call - nothing here decides them and there is no enclosing signature to carry the requirement"_v,
                                  source, context.findName(generic->name));
        return nullptr;
    }

    // The callee's requirements become this function's, expressed in this function's variables:
    // whoever instantiates this one has to prove them, because nobody else can. Its body is
    // resolved first, since that is what collects the ones its signature did not declare - a
    // forward reference would otherwise inherit a shorter list than the callee really has.
    resolveFunctionBody(*generic->module, *generic);

    for(auto constraint: calleeEnv->classes.contents(global)) {
        if(!constraint.typeClass) continue;

        TypeList forwarded;
        for(auto argument: constraint.args.contents(global)) {
            forwarded.push(substituteType(module, argument, toBuffer(bindings), source));
        }

        requireClass(module, function, constraint.typeClass, toBuffer(forwarded), source);
    }

    auto resultType = substituteType(module, generic->returnType, toBuffer(bindings), source);
    auto call = create<InstGenCall>(source, resultName, resultType, callee, nullptr, 0);

    for(auto binding: bindings) call->typeArgs.push(module.arena, binding);

    /*
     * A `@lazy` position becomes the closure the callee will run, exactly as it does for a class
     * dispatch - see emitGenericDispatch, whose loop this is.
     *
     * A deferred `ResolvedArg` carries nothing in `value`; what it has is a promise, and
     * `substituteArguments` leaves it alone because there is no parameter type in the caller's terms
     * to convert it against. Pushing `.value` straight through therefore handed the call a null
     * operand, which printed as `%v0` and lowered to garbage - a bug this path had for as long as it
     * existed and that nothing reached, because until `&&` and `||` became plain functions there was
     * no generic plain function with a `@lazy` parameter for a generic body to forward one into.
     */
    for(Size i = 0; i < converted.size(); i++) {
        auto declared = i < generic->args.size() ? local[generic->args.get(local, i)] : nullptr;

        if(declared && declared->isLazy()) {
            auto type = substituted(declared->lazyType, toBuffer(bindings), source);
            auto entry = converted[i].promise;

            // Not deferred by the call site - a forwarded value, or a synthesized call - which is
            // the same completion fillDeferred makes for every other route to a `@lazy` parameter.
            if(!entry.isSet()) entry = deferredValue(converted[i].value, type);
            entry.type = type;

            call->args.push(module.arena, makeThunk(entry, type, source));
            continue;
        }

        call->args.push(module.arena, converted[i].value);
    }

    append(call);
    auto result = ref(call);
    if(isMemoryType(global, resultType)) call->local = function.addLocal(module, resultType, resultName, result);

    return result;
}
