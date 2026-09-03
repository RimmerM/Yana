#include "resolve_internal.h"

namespace mir {

/*
 * `doc/spec/overloading.md`'s five rules, in the one place a call chooses between candidates.
 *
 * R1 is not here - it is the *table*, `Program::addFunctionName`, and what a lookup answers is
 * therefore the set rather than a declaration. What is here is everything that happens once a name
 * and an arity have found more than one thing:
 *
 *  - **R2**, a candidate matches or it does not, with no ranking. Two that both match is an error
 *    and not a contest.
 *  - **R3**, in generic code the constraints in scope decide - the candidate whose class the
 *    enclosing signature already requires wins over one whose class it does not.
 *  - **R4**, an ambiguity is reported by naming the qualified spellings the author can choose
 *    between, never resolved by a tiebreak.
 *  - **R5**, a plain function that matches wins over every class candidate, and one that does not
 *    match does not shadow them.
 *
 * ## Why a set of one does not come here
 *
 * A call whose name and arity reach exactly one candidate takes the ordinary path in
 * `resolve_expr.cpp` and this file never runs. That is R5's own last paragraph rather than a
 * shortcut: a plain function's parameter types are known before its arguments are, so they are
 * pushed down as each argument's expected type - which is what lets `f(Nothing)` know which `Maybe`
 * it is building - and that "happens **only** where the plain function is the whole overload set".
 * Where a name has something to choose between there is no parameter type to push down, because
 * which parameter types apply is the question the arguments are being resolved to answer.
 *
 * So the arguments here are resolved **on their own**, exactly as an argument to a class member with
 * an open parameter type already is, and the candidates are matched against what came back. Nothing
 * is resolved twice and nothing is rolled back: a speculative walk that emitted nodes and then
 * discarded them would leave them in the block it was building, in a stage that runs no pass to
 * remove a value nothing names.
 */

/*
 * Whether a value of one type fills a position of another, asked without reporting.
 *
 * The **accepting** branches of `convertValue` and nothing else, which is what makes this a
 * predicate rather than a second conversion rule: the same three questions in the same order, with
 * the answer where that function reports being false here. A fourth rule that admitted something
 * `convertValue` refuses would be a candidate selected and then rejected at the position it was
 * selected for, which is the worst of the two diagnostics.
 *
 * A destination still holding a **type variable** accepts anything, and that is not a hole. Such a
 * position belongs to a candidate whose own context is what constrains it - `Ord.min(lhs: a)` at a
 * bound `a` has already been substituted by the caller, and one that is still open is one the trial
 * bound nothing to, which is a candidate this call does not decide by its arguments. R3 and R4 are
 * what decide those.
 */
bool Resolver::acceptsType(DeclPtr<Type> from, DeclPtr<Type> to, LocationId source) {
    if(!from || !to) return false;
    if(sameType(from, to)) return true;

    auto kind = decl()[to]->kind;
    if(kind == Type::Variable) return true;

    // A `@bits(n)` destination is reached in two steps and the first is the one that decides -
    // `convertValue`, where the composition is argued.
    if(auto canonical = canonicalInt(decl(), to); canonical != to) {
        return acceptsType(from, canonical, source);
    }

    if(refinesInteger(from, to)) return true;

    return relatedBy(intrinsic::Known::widen, from, to, source);
}

/*
 * R2's "every argument binds its declared position", asked of one candidate.
 *
 * The trial binds the candidate's own type arguments from what stands opposite them, exactly as the
 * real call does - `TypeArguments::bind` is structural, one-way and reports nothing, which is what
 * makes it usable here at all - and then asks whether the value fills the position it bound.
 *
 * The bindings are **kept** in `into`, so the candidate that wins does not solve its signature a
 * second time. That is not only an economy: two solves of one signature against one argument list
 * are two answers that could differ, and a call selected on one and emitted on the other is the
 * shape of bug this stage is a rewrite to avoid.
 *
 * A position the call left out is not asked about. Its value is the parameter's own constant, whose
 * type is the parameter's by construction, so there is nothing for an argument to fail to fill.
 */
bool Resolver::candidateAccepts(Function& callee, const ArgMap& map,
                                const DeclPtr<Type>* argumentTypes, Size count, LocationId source,
                                TypeArguments& into) {
    for(Size i = 0; i < count; i++) {
        // Every written argument has a position: `checkCallShape` answered false for this candidate
        // otherwise, and it ran before this did. Read without a guard, so that this and the two
        // other walks over the same map say the same thing about it.
        auto& parameter = *decl()[callee.args.get(decl(), map.position(U32(i)))];
        auto declared = parameter.occurrence.type;

        /*
         * A `@lazy` position takes a thunk over the expression rather than a value of the parameter
         * type, so what stands opposite it is not the argument's type and there is nothing here to
         * compare. Admitted, and `agreedShape` below is what makes that safe: a set whose
         * candidates disagree about which positions are deferred is refused before a single argument
         * is resolved.
         */
        if(parameter.lazy) continue;

        into.bind(declared, argumentTypes[i]);
        if(!acceptsType(argumentTypes[i], into.expected(declared), source)) return false;
    }

    return true;
}

namespace {

/*
 * One candidate of the set, as selection sees it.
 *
 * `bound` and `usable` are two questions and the difference is what the diagnostics are made of. A
 * candidate is **bound** when its declared positions accept the arguments; it is **usable** when it
 * is also dischargeable - a plain function always is, and a class function is where an instance
 * exists at the bound types or the enclosing signature requires the class.
 *
 * Several bound and none usable is `doc/spec/overloading.md`'s own example: `fn (a) what(x: a) =
 * size(x)` where two classes declare `size`. Reporting that as "nothing accepts these types" would
 * be false - both accept them - so the two answers are kept apart and R4's message is what such a
 * call gets.
 */
struct Candidate {
    DeclPtr<Function> function = nullptr;
    ArgMap map;
    bool bound = false;
    bool usable = false;
    bool required = false;  // R3: its class is one the enclosing signature declares.
};

} // namespace

/*
 * Whether the enclosing signature already requires this candidate's class, at the types the trial
 * bound - R3, and the whole of it.
 *
 * "The candidate whose class is a declared requirement of the enclosing function wins", which is
 * Haskell's rule without defaulting and has the property the spec names: adding a class to a
 * signature can *fix* an ambiguity rather than only ever adding an obligation.
 *
 * Asked of the class's own arguments after substitution, so `fn (Area(a)) area(x: a) = size(x)`
 * matches its own `Area(a)` and not a `Area(Circle)` somebody else declared.
 */
static bool requiredByEnclosing(Resolver& resolver, Function& candidate, TypeArguments& bound) {
    auto base = resolver.decl();
    auto klass = candidate.klass;
    if(!klass || !resolver.currentFunction) return false;

    auto enclosing = resolver.currentFunction->gen;
    if(!enclosing) return false;

    auto with = bound.with();

    for(auto requirement: base[candidate.gen]->classes.contents(base)) {
        if(requirement.klass != klass) continue;

        SmallArray<DeclPtr<Type>, 4> arguments;
        auto complete = true;

        for(auto argument: requirement.args.contents(base)) {
            auto supplied = substituteType(resolver.context, resolver.program, argument, with);
            complete = complete && supplied;
            arguments.push(supplied);
        }

        if(!complete) continue;

        if(satisfiesRequirement(resolver.context, resolver.program, enclosing, klass,
                                arguments.pointer(), arguments.size())) {
            return true;
        }
    }

    return false;
}

// Whether every requirement this candidate's context states can be proved at the bound types -
// `dischargeRequirement`, asked silently. A candidate that cannot is a candidate, not a mistake.
static bool candidateDischarges(Resolver& resolver, Function& candidate, TypeArguments& bound,
                                LocationId source) {
    auto base = resolver.decl();
    if(!candidate.gen) return true;

    auto with = bound.with();

    for(auto requirement: base[candidate.gen]->classes.contents(base)) {
        if(!resolver.dischargeRequirement(requirement, with, source, false)) return false;
    }

    return true;
}

/*
 * How a candidate is named in an ambiguity report - R4's "the qualified names the author can choose
 * between, rather than leaving them to find the classes".
 *
 * `Bitwise.and` for a class function and the bare name for the plain one, because that is what the
 * author would write to select each: a class function is reached through its class, and a plain
 * function is what the bare name means once the ambiguity is gone.
 */
static String candidateText(Context& context, DeclBase base, Function& candidate) {
    auto name = context.findName(candidate.name);
    if(!candidate.klass) return String(name.text(), name.size());

    auto klass = context.findName(base[candidate.klass]->name);
    return String(klass.text(), klass.size()) + String(".") + String(name.text(), name.size());
}

// The candidates as a message lists them - "Area.size, Volume.size". One function, so an ambiguity
// and a "nothing accepts these" name the same alternatives the same way.
static String candidateList(Context& context, DeclBase base, const Candidate* candidates,
                            Size count) {
    String text;

    for(Size i = 0; i < count; i++) {
        if(i) text = text + String(i + 1 == count ? " or " : ", ");
        text = text + candidateText(context, base, *base[candidates[i].function]);
    }

    return text;
}

/*
 * What each written position asks of the value that fills it, decided for the **set** rather than
 * for the candidate that has not been chosen yet.
 *
 * It has to be the set's, because the request is spent before selection can run: `resolveValue`
 * takes it, and `readPlace` reads a name differently under each - a shared read of a local is a
 * borrow and an owning one is a copy or a move. So a position where the candidates disagree needs an
 * answer that serves all of them, and there are two kinds of disagreement with two answers.
 *
 * **Publishing is irreconcilable.** A `&` position or one naming an origin needs *storage*, and an
 * unpublished one needs a value; a `@lazy` position needs the expression itself, unevaluated. There
 * is no reading that is both, so a set that disagrees there is refused - and it is a declaration
 * mistake rather than a call-site one, reported here because this is the only place the two
 * declarations are visible together.
 *
 * **Owning over borrowing is not.** `class Ord(a)` declares `min(->lhs: a, ->rhs: a)` and
 * `Core/Vector.yana` declares a plain `min(lhs: Vec(a, n), rhs: Vec(a, n))` beside it, which is R1
 * exactly and is the case this whole file exists for. An owning read serves both: a position that
 * borrows accepts a value the caller owns, which is what it already gets from a literal or a call
 * result, while the reverse does not hold - a shared read cannot be handed to a `->`.
 *
 * The cost is written down rather than hidden. Where the candidates disagree and a *borrowing* one
 * wins, a non-trivial argument has been moved for nothing, and using it afterwards is then refused.
 * That is a false rejection with a clear message at the later use rather than a call that quietly
 * hands over what it only borrowed, and it is reachable only from a name whose overloads disagree
 * about consuming - which is a thing a library chooses to write.
 */
static bool agreedShape(Resolver& resolver, const Candidate* candidates, Size count, Size arguments,
                        LocationId source, SmallArray<Request, 8>& requests,
                        SmallArray<bool, 8>& deferred) {
    auto base = resolver.decl();
    auto& first = *base[candidates[0].function];

    for(Size i = 0; i < arguments; i++) {
        auto position = candidates[0].map.position(U32(i));
        auto& parameter = *base[first.args.get(base, position)];

        requests.push(requestOf(parameter.occurrence));
        deferred.push(parameter.lazy);
    }

    for(Size c = 1; c < count; c++) {
        auto& candidate = *base[candidates[c].function];

        for(Size i = 0; i < arguments; i++) {
            auto position = candidates[c].map.position(U32(i));
            auto& parameter = *base[candidate.args.get(base, position)];
            auto request = requestOf(parameter.occurrence);

            if(request.published != requests[i].published || parameter.lazy != deferred[i]) {
                resolver.context.diagnostics.error(
                    "%@ and %@ are both candidates here and disagree about what their %@ argument takes, so this call cannot be resolved - one of them borrows through to the caller's storage or defers the expression, and the other wants a value"_v,
                    source, candidateText(resolver.context, base, first),
                    candidateText(resolver.context, base, candidate), U64(i + 1));
                return false;
            }

            // The owning read where any of them owns - see the note above, where the direction and
            // what it costs are argued.
            if(request.access == Access::Owned) requests[i].access = Access::Owned;
        }
    }

    return true;
}

/*
 * The tail an overloaded call shares with an ordinary one - see the declaration.
 *
 * Every value is already resolved, so what is left is `prepareArgument` at the position the winner
 * declared: the conversion, the window adaptation, the origin check and the borrow are the same four
 * steps an ordinary call takes, because they are what a *position* asks and not what a selection
 * decided.
 *
 * A **deferred** position is the one thing resolved here rather than above. Its argument becomes a
 * thunk whose result type is the parameter's, and which parameter that is was the question selection
 * had to answer - so it could not have been built earlier, and every candidate agreeing about which
 * positions are deferred is what makes leaving it until now sound.
 */
Node* Resolver::finishOverloadedCall(Builder& builder, ast::AppExpr& written, Function& callee,
                                     const ArgMap& map, SmallArray<Node*, 8>& values,
                                     DeclPtr<Type> hint, LocationId source) {
    TypeArguments typeArguments(*this, callee.gen);

    SmallArray<Node*, 8> arguments;
    for(U32 i = 0; i < map.count(); i++) arguments.push(nullptr);

    HeldList<SmallArray<Node*, 8>> held(*this, arguments);

    auto at = U32(0);
    for(auto argument: written.args.contents(parse)) {
        auto index = at++;
        auto position = map.position(index);
        auto& parameter = *decl()[callee.args.get(decl(), position)];

        if(parameter.lazy) {
            auto thunk = resolveDeferred(builder, argument.value,
                                         typeArguments.expected(parameter.occurrence.type));
            if(!thunk) return nullptr;

            arguments[position] = prepareArgument(builder, *thunk, parameter, typeArguments,
                                                  argument.value.source);
        } else {
            arguments[position] = prepareArgument(builder, *values[index], parameter, typeArguments,
                                                  argument.value.source);
        }

        if(!arguments[position]) return nullptr;
    }

    if(!fillDefaultArguments(builder, callee, map, source, arguments)) return nullptr;

    return finishCall(builder, callee, arguments, typeArguments, hint, source);
}

/*
 * A call whose name and arity reach more than one candidate - R2 through R5.
 *
 * The shape is four steps and they are in this order for a reason each: the candidates that can be
 * *mapped* are found first, because a candidate the arguments cannot even be assigned to positions
 * of is not a candidate and must not constrain what the set agrees about; the set's shape is agreed
 * next, because it decides how the arguments are resolved; the arguments are resolved once; and
 * only then is a candidate chosen.
 */
Node* Resolver::resolveOverloadedCall(Builder& builder, ast::AppExpr& written,
                                      const OverloadSet& set, StringId name, LocationId nameSource,
                                      DeclPtr<Type> hint, LocationId source) {
    SmallArray<Candidate, 4> candidates;

    /*
     * The candidates this call's *shape* admits - the written names map onto declared positions, and
     * every position without an argument has a default.
     *
     * Silently, which is what `ArgSite::report` is for: a candidate that takes a `by:` this call did
     * not write is not a mistake at the call, it is a candidate the call did not mean.
     */
    for(Size i = 0; i < set.size(); i++) {
        Candidate candidate;
        candidate.function = set.at(i);

        if(!checkCallShape(written, *decl()[candidate.function], source, candidate.map, false)) {
            continue;
        }

        candidates.push(static_cast<Candidate&&>(candidate));
    }

    /*
     * Nothing the call's shape admits, which is a statement about the *names and count* written and
     * not about any type - so it is reported before an argument is resolved, exactly as an ordinary
     * call's shape check is.
     *
     * The whole set is listed, because which of them the author meant is what they have to be told:
     * one of these declares the position they wrote and none of them takes it as written.
     */
    if(!candidates.size()) {
        SmallArray<Candidate, 4> all;
        for(Size i = 0; i < set.size(); i++) all.push(Candidate { set.at(i) });

        context.diagnostics.error("no %@ takes these arguments - the candidates at this arity are %@, and none of them has the positions this call writes"_v,
                                  nameSource, context.findName(name),
                                  candidateList(context, decl(), all.pointer(), all.size()));
        return nullptr;
    }

    auto arguments = Size(written.args.size());

    SmallArray<Request, 8> requests;
    SmallArray<bool, 8> deferred;
    if(!agreedShape(*this, candidates.pointer(), candidates.size(), arguments, source, requests,
                    deferred)) {
        return nullptr;
    }

    /*
     * The arguments, resolved once and in written order, with **no expected type** - R5's last
     * paragraph, and the note at the top of this file.
     *
     * `HeldList` for the reason `resolveArguments` uses one: an `if` in argument three puts a branch
     * between arguments one and two and the call that uses them, so a value already produced has to
     * travel that branch's edges.
     */
    SmallArray<Node*, 8> values;
    for(Size i = 0; i < arguments; i++) values.push(nullptr);

    HeldList<SmallArray<Node*, 8>> held(*this, values);

    {
        auto at = U32(0);
        for(auto argument: written.args.contents(parse)) {
            auto index = at++;

            // A deferred position is not evaluated here any more than it is at an ordinary call -
            // the expression becomes a thunk, and the thunk's own type is the parameter's, which is
            // not known until a candidate is chosen. So it is left for `resolveArguments` below.
            if(deferred[index]) continue;

            values[index] = resolveValue(builder, argument.value, nullptr, requests[index]);
            if(!values[index]) return nullptr;
        }
    }

    /*
     * R2 and R3, asked of each candidate: does it bind, and can it be used.
     *
     * Both, and in that order, because they are the two halves the diagnostics are made of - see
     * `Candidate`, where the difference between "nothing accepts these types" and "name a class" is
     * argued.
     */
    SmallArray<DeclPtr<Type>, 8> argumentTypes;
    for(Size i = 0; i < arguments; i++) {
        argumentTypes.push(values[i] ? valueType(*values[i]) : nullptr);
    }

    for(Size i = 0; i < candidates.size(); i++) {
        auto& candidate = *decl()[candidates[i].function];
        TypeArguments bound(*this, candidate.gen);

        if(!candidateAccepts(candidate, candidates[i].map, argumentTypes.pointer(), arguments,
                             source, bound)) {
            continue;
        }

        candidates[i].bound = true;
        candidates[i].required = requiredByEnclosing(*this, candidate, bound);
        candidates[i].usable = !candidate.klass || candidates[i].required ||
                               candidateDischarges(*this, candidate, bound, source);
    }

    /*
     * **R5 first**: a plain function that matches wins over every class candidate.
     *
     * "If a plain function matches, it wins over any class candidate, which preserves 'my definition
     * beats the imported one' for the case that actually overlaps. If it does not match, resolution
     * continues into the class candidates." That is why this is a first case rather than a
     * tiebreak - it is not that the plain function is *preferred*, it is that the class candidates
     * are not consulted at all once it fits.
     */
    if(set.plain && candidates[0].function == set.plain && candidates[0].bound) {
        return finishOverloadedCall(builder, written, *decl()[set.plain], candidates[0].map, values,
                                    hint, source);
    }

    // R2's survivors, and then R3's filter over them where there is more than one. `required` is a
    // property of the candidate rather than a comparison between two, so the filter is a pass.
    SmallArray<Candidate, 4> matched;
    for(auto& candidate: candidates) {
        if(candidate.usable) matched.push(candidate);
    }

    if(matched.size() > 1) {
        SmallArray<Candidate, 4> constrained;
        for(auto& candidate: matched) {
            if(candidate.required) constrained.push(candidate);
        }

        if(constrained.size() == 1) {
            return finishOverloadedCall(builder, written, *decl()[constrained[0].function],
                                        constrained[0].map, values, hint, source);
        }
    }

    if(matched.size() == 1) {
        return finishOverloadedCall(builder, written, *decl()[matched[0].function], matched[0].map,
                                    values, hint, source);
    }

    /*
     * R4 - an ambiguity names the qualified spellings, and is never resolved by a tiebreak.
     *
     * Two shapes reach here and they are different mistakes. Several candidates *usable* is a call
     * whose types genuinely fit more than one, which is what "two candidates that both match are an
     * error rather than a contest" is about. Several **bound** and none usable is the fully generic
     * body of the spec's own example, where no instance exists yet and no constraint in scope says
     * which class was meant - and naming a class is what fixes it, which is exactly what R3 says
     * adding one to the signature does.
     */
    if(matched.size() > 1) {
        context.diagnostics.error("this call is ambiguous - %@ both accept these arguments, so which one is meant has to be written"_v,
                                  nameSource,
                                  candidateList(context, decl(), matched.pointer(), matched.size()));
        return nullptr;
    }

    SmallArray<Candidate, 4> bound;
    for(auto& candidate: candidates) {
        if(candidate.bound) bound.push(candidate);
    }

    if(bound.size() > 1) {
        context.diagnostics.error("this call is ambiguous - %@ accept these arguments and nothing in scope decides between them, so name the one that is meant or require its class in this signature"_v,
                                  nameSource,
                                  candidateList(context, decl(), bound.pointer(), bound.size()));
        return nullptr;
    }

    /*
     * Exactly one candidate bound and it was not usable - so the call is *that* candidate's, and
     * what it could not prove is the message this call deserves.
     *
     * Carried on with rather than reported here, so the failure is reported by the same walk that
     * would have reported it if the name had reached one declaration: what it needed and could not
     * discharge is `dischargeRequirements`' sentence, and a second one from here would be a summary
     * of a message the author has not read yet.
     */
    if(bound.size() == 1) {
        return finishOverloadedCall(builder, written, *decl()[bound[0].function], bound[0].map,
                                    values, hint, source);
    }

    context.diagnostics.error("no %@ accepts these arguments - the candidates are %@"_v, nameSource,
                              context.findName(name),
                              candidateList(context, decl(), candidates.pointer(),
                                            candidates.size()));
    return nullptr;
}

} // namespace mir
