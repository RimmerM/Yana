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
 * Whether this operand is a **written number and nothing else** - the one argument shape whose type
 * the candidate decides rather than the other way round.
 *
 * `doc/spec/overloading.md` R5 pushes a position's type down only where a plain function is the whole
 * set, so an untyped literal standing in a set of several has nothing to take and is built at its
 * default. `48 + rest` then offers `I32` to `Native`'s pointer `+` and to `Num.+` alike and fits
 * neither, which is the eight `lib/Core` sites of `doc/progress/2026-09-06-J1-lib-crash.md`. A
 * literal is a `FromInt` call at a type to be decided, so what it *is* is exactly what a position
 * decides - and leaving it unbuilt until one has is what turns "no + accepts these" into a
 * selection.
 *
 * A string and a `True` are not this: they have a type of their own before anything asks. A negation
 * of a number is, since the sign folds into the constant, and so is a parenthesized one - both are
 * unwrapped by the same walk that will build it.
 */
bool Resolver::undecidedLiteral(ast::Expr& expr, intrinsic::Known& role) {
    if(expr.kind == ast::Expr::Nested) return undecidedLiteral(*parse[expr.nested], role);

    if(expr.kind == ast::Expr::Prefix && isNegation(context, parse, expr)) {
        return undecidedLiteral(parse[expr.prefix]->on, role);
    }

    if(!ast::isLiteral(expr)) return false;

    switch(ast::Literal::Kind(expr.kind - ast::Expr::Lit)) {
        case ast::Literal::Int:
            role = intrinsic::Known::fromInt;
            return true;
        case ast::Literal::Double:
        case ast::Literal::Float:
            role = intrinsic::Known::fromDecimal;
            return true;
        default:
            return false;
    }
}

/*
 * Whether a number of this role can be built at this type - `literalNode`'s two answers, asked
 * without reporting and without building anything.
 *
 * A **concrete** type needs the instance to exist, which is `literalInstance`'s question with its
 * message left out. A type **variable of the enclosing signature** needs that signature to have
 * required the conversion class, which is the same thing `literalNode` relies on one step later:
 * `0.5 + r` inside `fn (Real(a, m)) sinOf(r: a)` builds the half at `a` through the `FromDecimal(a)`
 * that `Real`'s own context promises, so a viability test that answered only for concrete types
 * would have made every coefficient in `lib/Math` an `F32` standing beside an `a`.
 */
static bool admitsLiteral(Resolver& resolver, intrinsic::Known role, DeclPtr<Type> type,
                          LocationId source) {
    auto base = resolver.decl();
    auto klass = resolver.program.roles.klass(role);
    if(!type) return false;

    if(!isConcreteType(base, type)) {
        auto enclosing = resolver.currentFunction ? resolver.currentFunction->gen : nullptr;
        if(base[type]->kind != Type::Variable) return false;

        return satisfiesRequirement(resolver.context, resolver.program, enclosing, klass, &type, 1);
    }

    InstanceMatch found;
    return selectInstance(resolver.context, resolver.program, klass, &type, 1, source, false, found);
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
                                TypeArguments& into, const intrinsic::Known* literals) {
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

        // An **undecided literal** binds nothing: what it is is what this position turns out to be,
        // so it is checked in the second pass below once every other argument has bound.
        if(literals && literals[i] != intrinsic::Known::Count) continue;

        into.bind(declared, argumentTypes[i]);
        if(!acceptsType(argumentTypes[i], into.expected(declared), source)) return false;
    }

    /*
     * The literal positions, after the rest - which is the whole of the ordering.
     *
     * A candidate is viable at a literal operand when its *other* parameters have fixed the
     * position's type and a conversion instance exists there. `48 + rest` at `Native`'s
     * `+(p: %a, n: Size)` fixes nothing about `%a` from a number and is not viable; at `Num.+(lhs: a,
     * rhs: a)` the second operand has already bound `a`, and whether the number can be made at it is
     * `FromInt`'s own question. Exactly one viable candidate selects, and more than one is the
     * ambiguity R2 already reports - said by the ordinary count below rather than by a rule of its
     * own.
     */
    if(!literals) return true;

    for(Size i = 0; i < count; i++) {
        if(literals[i] == intrinsic::Known::Count) continue;

        auto& parameter = *decl()[callee.args.get(decl(), map.position(U32(i)))];
        if(parameter.lazy) continue;

        auto declared = parameter.occurrence.type;

        /*
         * A position nothing else decided takes the literal's own **default** - `1 - 1023`, whose
         * two operands are both numbers and where there is no other argument to read one off. That
         * is the same rule a literal in any other position obeys and the same one `resolveValue`
         * applies when it builds this operand later, so the trial and the build agree by
         * construction rather than by both spelling `I32`.
         *
         * A position the other arguments left standing at *the enclosing signature's own variable*
         * is not undecided: it is a type this body is generic in, and the number is built at it
         * through whatever conversion class the signature required. So the default is taken only
         * where nothing at all is standing there.
         */
        auto expected = into.expected(declared);

        if(!expected || (!isConcreteType(decl(), expected) &&
                         !admitsLiteral(*this, literals[i], expected, source))) {
            into.bind(declared, literalDefault(literals[i], source));
        }

        if(!admitsLiteral(*this, literals[i], into.expected(declared), source)) return false;
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
static String candidateList(Context& context, DeclBase base, const Candidate* const* candidates,
                            Size count) {
    String text;

    for(Size i = 0; i < count; i++) {
        if(i) text = text + String(i + 1 == count ? " or " : ", ");
        text = text + candidateText(context, base, *base[candidates[i]->function]);
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
 * **A deferred position is irreconcilable.** A `@lazy` position needs the expression itself,
 * unevaluated, and every other position needs it evaluated. There is no reading that is both, and
 * taking one candidate's answer would not do either: the value would be produced *and* thunked, so
 * the operand would run whether or not the winner meant it to. A set that disagrees there is
 * refused - a declaration mistake rather than a call-site one, reported here because this is the
 * only place the two declarations are visible together.
 *
 * **Publishing is reconcilable exactly where R5 is.** A `&` position or one naming an origin needs
 * *storage* and an unpublished one needs a value, and no reading is both - but where the set holds a
 * plain function the spec has already said which candidate wins: "if a plain function matches, it
 * wins over any class candidate, which preserves 'my definition beats the imported one' for the case
 * that actually overlaps". So the plain function's answer is the set's, one step before matching can
 * be asked, and a class candidate that would have taken its argument differently is one the call
 * reaches only when the plain function did not fit - where the message it gets is that its position
 * was not filled the way it asks. That case is what `fn widen(&p: Point)` written beside Core's
 * `Widen.widen` is, and refusing it outright would make declaring a name Core also declares a
 * mistake, which is the one thing R5 exists to say it is not.
 *
 * Between two **class** candidates there is no such rule, and the disagreement stays refused: R4
 * answers an ambiguity by qualifying the call, and there is no qualified spelling that would change
 * how an argument had already been read.
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
static bool agreedShape(Resolver& resolver, const Candidate* candidates, Size count,
                        const OverloadSet& set, Size arguments, LocationId source,
                        SmallArray<Request, 8>& requests, SmallArray<bool, 8>& deferred) {
    auto base = resolver.decl();
    auto& first = *base[candidates[0].function];

    // R5's winner, where the set has one and this call's shape admits it. `set.at(0)` is the plain
    // function and the shape filter keeps the set's order, so it is the first candidate or none.
    auto decided = set.plain && candidates[0].function == set.plain;

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

            if(parameter.lazy != deferred[i]) {
                resolver.context.diagnostics.error(
                    "%@ and %@ are both candidates here and disagree about whether their %@ argument is evaluated, so this call cannot be resolved - one of them defers the expression and the other wants a value"_v,
                    source, candidateText(resolver.context, base, first),
                    candidateText(resolver.context, base, candidate), U64(i + 1));
                return false;
            }

            if(request.published != requests[i].published && !decided) {
                resolver.context.diagnostics.error(
                    "%@ and %@ are both candidates here and disagree about what their %@ argument takes, so this call cannot be resolved - one of them borrows through to the caller's storage and the other wants a value, and there is no plain function of the name to decide it"_v,
                    source, candidateText(resolver.context, base, first),
                    candidateText(resolver.context, base, candidate), U64(i + 1));
                return false;
            }

            // The owning read where any of them owns - see the note above, where the direction and
            // what it costs are argued. Unconditional, R5's winner included: an owning read serves
            // a borrowing position and a borrowing read does not serve an owning one, so this is the
            // direction that keeps every candidate reachable, and `Ord.max` beside `Vector.max` is
            // the pair that says so.
            if(request.access == Access::Owned) requests[i].access = Access::Owned;
        }
    }

    return true;
}

/*
 * R2 through R5, over candidates whose arguments are already resolved - **the choice itself**.
 *
 * Nothing here knows where the values came from, which is what lets an operator and a written call
 * be selected by one rule: an operator has no argument list to name positions in and no `AppExpr` to
 * hang a thunk off, and neither of those is a question selection asks. What it asks is which
 * candidates bind the types in hand and which of those can be used, and both callers have exactly
 * that.
 *
 * Answers the winner, or null having reported. `candidates` is written through - `bound`, `usable`
 * and `required` are filled here - so the caller's list is what the answer points into. `winner` is
 * left holding the chosen candidate's own trial - see `OverloadChoice::args` - so completion prepares
 * every position against the same bindings selection used to pick it, rather than solving the
 * signature a second time.
 */
static const Candidate* chooseCandidate(Resolver& resolver, SmallArray<Candidate, 4>& candidates,
                                        const OverloadSet& set, Node* const* values, Size arguments,
                                        StringId name, LocationId nameSource, LocationId source,
                                        Maybe<TypeArguments>& winner,
                                        const intrinsic::Known* literals = nullptr,
                                        CandidateKind kind = CandidateKind::Any) {
    auto& context = resolver.context;
    auto base = resolver.decl();

    // One trial per candidate, held until a winner is known rather than discarded after each -
    // `chooseCandidate` does not decide which candidate wins until every one of them has been
    // tried, so all of them stay alive until then. `trials[i]` is `candidates[i]`'s own.
    SmallArray<TypeArguments, 4> trials;

    /*
     * The winner, once there is one: whether a call may reach the declaration at all is asked here
     * rather than at each caller, because this is where a set settles on one - `checkCallable`, the
     * same question `resolveCall` and `findOperator` ask of a name that reached a single one.
     *
     * And the moment the trial that bound this candidate is moved into `winner` rather than left to
     * be discarded with the rest of `trials` - the one point every return path below funnels
     * through, so there is exactly one place that decides which trial survives.
     */
    auto chosen = [&](const Candidate* candidate) -> const Candidate* {
        if(!resolver.checkCallable(*base[candidate->function], nameSource,
                                   kind == CandidateKind::Iterator)) {
            return nullptr;
        }

        auto index = Size(candidate - candidates.pointer());
        winner.replace(static_cast<TypeArguments&&>(trials[index]));
        return candidate;
    };

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
        auto& candidate = *base[candidates[i].function];
        trials.push(resolver, candidate.gen);
        auto& bound = trials[i];

        /*
         * Unified, like every real solve in this stage does - `TypeArguments::unifies`. Left off
         * here until now: this trial bound the shared class variable of `Num.+(lhs: a, rhs: a)` to
         * whichever operand happened to be checked first and refused the other unless it was already
         * at least as wide - so `x + y` at `(I32, I64)` selected and `y + x` at the same two types
         * did not, once the name reached more than one candidate. Meeting the two at their `Widen`
         * upper bound instead, exactly as `finishOverloadedCall` and the operator chain's sole-callee
         * path already do, makes selection agree with what completion accepts regardless of which
         * operand came first.
         */
        bound.unifies = true;

        if(!resolver.candidateAccepts(candidate, candidates[i].map, argumentTypes.pointer(),
                                      arguments, source, bound, literals)) {
            continue;
        }

        candidates[i].bound = true;
        candidates[i].required = requiredByEnclosing(resolver, candidate, bound);
        candidates[i].usable = !candidate.klass || candidates[i].required ||
                               candidateDischarges(resolver, candidate, bound, source);
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
        return chosen(&candidates[0]);
    }

    // R2's survivors, and then R3's filter over them where there is more than one. `required` is a
    // property of the candidate rather than a comparison between two, so the filter is a pass.
    SmallArray<const Candidate*, 4> matched;
    for(auto& candidate: candidates) {
        if(candidate.usable) matched.push(&candidate);
    }

    if(matched.size() > 1) {
        SmallArray<const Candidate*, 4> constrained;
        for(auto candidate: matched) {
            if(candidate->required) constrained.push(candidate);
        }

        if(constrained.size() == 1) return chosen(constrained[0]);
    }

    if(matched.size() == 1) return chosen(matched[0]);

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
                                  nameSource, candidateList(context, base, matched.pointer(),
                                                            matched.size()));
        return nullptr;
    }

    SmallArray<const Candidate*, 4> bound;
    for(auto& candidate: candidates) {
        if(candidate.bound) bound.push(&candidate);
    }

    if(bound.size() > 1) {
        context.diagnostics.error("this call is ambiguous - %@ accept these arguments and nothing in scope decides between them, so name the one that is meant or require its class in this signature"_v,
                                  nameSource, candidateList(context, base, bound.pointer(),
                                                            bound.size()));
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
    if(bound.size() == 1) return chosen(bound[0]);

    SmallArray<const Candidate*, 4> all;
    for(auto& candidate: candidates) all.push(&candidate);

    context.diagnostics.error("no %@ accepts these arguments - the candidates are %@"_v, nameSource,
                              context.findName(name),
                              candidateList(context, base, all.pointer(), all.size()));
    return nullptr;
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
bool Resolver::prepareResolvedArguments(Builder& builder, ast::AppExpr& written, Function& callee,
                                        const ArgMap& map, LocationId source,
                                        SmallArray<Node*, 8>& values,
                                        SmallArray<Node*, 8>& arguments,
                                        TypeArguments& typeArguments, Receiver receiver) {
    // What the resolved arguments decide, taken before any position is prepared - the types an
    // undecided literal's own position reads. See `bindResolvedArguments`.
    bindResolvedArguments(callee, map, values.pointer(), values.size(), typeArguments);

    auto at = U32(0);

    // The receiver, whose value was read at the request the set agreed on and which reaches the same
    // `prepareArgument` every other position does - `Receiver`, and `resolveArguments`' own note.
    if(receiver) {
        auto position = map.position(at++);
        auto& parameter = *decl()[callee.args.get(decl(), position)];

        arguments[position] = prepareArgument(builder, *values[0], parameter, typeArguments,
                                              receiver.source);
        if(!arguments[position]) return false;
    }

    for(auto argument: written.args.contents(parse)) {
        auto index = at++;
        auto position = map.position(index);
        auto& parameter = *decl()[callee.args.get(decl(), position)];

        if(parameter.lazy) {
            auto thunk = resolveDeferred(builder, argument.value,
                                         typeArguments.expected(parameter.occurrence.type));
            if(!thunk) return false;

            arguments[position] = prepareArgument(builder, *thunk, parameter, typeArguments,
                                                  argument.value.source);
        } else if(!values[index]) {
            /*
             * The undecided literal, built at the position the winner declared - see
             * `undecidedLiteral`. `bindResolvedArguments` ran above, so this reads what the *other*
             * arguments decided rather than what the positions before it happened to bind.
             *
             * `resolveValue` and not `literalValue`, because what was left standing is the written
             * expression: a negated number is a prefix node whose sign folds into the constant one
             * level down, and a parenthesized one is a nested node, and unwrapping either is the
             * value walk's own job rather than a second copy of it here.
             */
            auto value = resolveValue(builder, argument.value,
                                      typeArguments.hint(parameter.occurrence.type),
                                      requestOf(parameter.occurrence));
            if(!value) return false;

            arguments[position] = prepareArgument(builder, *value, parameter, typeArguments,
                                                  argument.value.source);
        } else {
            arguments[position] = prepareArgument(builder, *values[index], parameter, typeArguments,
                                                  argument.value.source);
        }

        if(!arguments[position]) return false;
    }

    if(!widenPreparedArguments(builder, callee, map, source, arguments, typeArguments)) {
        return false;
    }

    return fillDefaultArguments(builder, callee, map, source, arguments);
}

/*
 * See the declaration. The winner's positions, then the call - the second half of R5, split from the
 * first so that a `for` loop's traversal can reach the same preparation without also being a call.
 *
 * `typeArguments` is `chosen`'s own trial from `chooseCandidate` - `OverloadChoice::args` - and not a
 * fresh one built here. It arrives already `unifies`, already holding whatever selection bound to
 * pick this candidate, and `prepareResolvedArguments` keeps refining the same object rather than
 * starting over: two `TypeArguments` solving one signature against one argument list could disagree,
 * and completion refusing what selection had just accepted - `x + y` at `(I32, I64)` through an
 * overloaded `+`, once selection itself was fixed to widen - was exactly that disagreement.
 */
Node* Resolver::finishOverloadedCall(Builder& builder, ast::AppExpr& written, Function& callee,
                                     const ArgMap& map, SmallArray<Node*, 8>& values,
                                     TypeArguments& typeArguments, DeclPtr<Type> hint,
                                     LocationId source, Receiver receiver) {
    SmallArray<Node*, 8> arguments;
    for(U32 i = 0; i < map.count(); i++) arguments.push(nullptr);

    HeldList<SmallArray<Node*, 8>> held(*this, arguments);

    if(!prepareResolvedArguments(builder, written, callee, map, source, values, arguments,
                                 typeArguments, receiver)) {
        return nullptr;
    }

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
bool Resolver::selectOverload(Builder& builder, ast::AppExpr& written, const OverloadSet& set,
                              StringId name, LocationId nameSource, LocationId source,
                              Receiver receiver, CandidateKind kind, OverloadChoice& into) {
    SmallArray<Candidate, 4> candidates;

    // A dot-call's receiver fills position 0 of every candidate's mapping - `ArgSite::leading`. It
    // takes part in selection exactly as a written argument does, which is what makes `x.f(y)` and
    // `f(x, y)` choose the same candidate.
    auto leading = receiver.leading();

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

        // And the kind the *position* admits, which is the filter this takes: a `for` runs an
        // iterator, so a plain function of the name is not a candidate of that loop at all and must
        // not constrain what the set agrees about. `CandidateKind::Any` is every other call.
        if(kind == CandidateKind::Iterator && !decl()[candidate.function]->iterator) continue;

        if(!checkCallShape(written, *decl()[candidate.function], source, candidate.map, false,
                           leading, receiver.source)) {
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

        SmallArray<const Candidate*, 4> listed;
        for(auto& candidate: all) listed.push(&candidate);

        context.diagnostics.error("no %@ takes these arguments - the candidates at this arity are %@, and none of them has the positions this call writes"_v,
                                  nameSource, context.findName(name),
                                  candidateList(context, decl(), listed.pointer(), listed.size()));
        return false;
    }

    auto arguments = Size(leading) + Size(written.args.size());

    SmallArray<Request, 8> requests;
    SmallArray<bool, 8> deferred;
    if(!agreedShape(*this, candidates.pointer(), candidates.size(), set, arguments, source,
                    requests, deferred)) {
        return false;
    }

    // The receiver's own position, refused on `checkDotReceiverNotLazy`'s terms - shared with
    // `resolveArguments`, which is the same question asked of the single-callee path.
    if(leading && !checkDotReceiverNotLazy(receiver.source, name, deferred[0])) return false;

    /*
     * The arguments, resolved once and in written order, with **no expected type** - R5's last
     * paragraph, and the note at the top of this file.
     *
     * The `HeldList` over them is the **caller's**, for the reason `resolveArguments` has one at
     * all: an `if` in argument three puts a branch between arguments one and two and whatever uses
     * them, so a value already produced has to travel that branch's edges - and what uses them here
     * is the caller, after this returns.
     */
    auto& values = into.values;
    SmallArray<intrinsic::Known, 8> literals;
    for(Size i = 0; i < arguments; i++) {
        values.push(nullptr);
        literals.push(intrinsic::Known::Count);
    }

    {

        // The receiver first, because it was written first - read at the request the whole set
        // agreed on, exactly as every other argument below is.
        if(leading) {
            values[0] = readPlace(builder, receiver.source, *receiver.place, requests[0]);
            if(!values[0]) return false;
        }

        auto at = leading;
        for(auto argument: written.args.contents(parse)) {
            auto index = at++;

            // A deferred position is not evaluated here any more than it is at an ordinary call -
            // the expression becomes a thunk, and the thunk's own type is the parameter's, which is
            // not known until a candidate is chosen. So it is left for `resolveArguments` below.
            if(deferred[index]) continue;

            // And an **undecided literal** is left for the same reason at one remove: what it is is
            // the winner's position to decide - `undecidedLiteral`. Built in `finishOverloadedCall`
            // beside the deferred one, so nothing is resolved twice and no constant is built at a
            // width the call then has to convert away from.
            if(undecidedLiteral(argument.value, literals[index])) continue;

            /*
             * An **argument** keeps what an `iter fn` call answers - §2.1's third row, on the same
             * terms `resolveArguments` states it for the single-callee path and with this argument's
             * own source as the token.
             *
             * Here and not only there because the two walks resolve the arguments of a call, and only
             * one of them had the token: `absorb(upTo(3))` built an object where `absorb` named one
             * declaration and was "upTo is an iterator, and what keeps an object of one is a `let`, a
             * field, an argument, a `return` or an assignment" the moment a second declaration of the
             * name existed. Nothing about selection is affected - the object's type is the producer's
             * site type either way, and what the set agrees about is a `Request`.
             *
             * A deferred position is skipped above rather than given one, exactly as it is there.
             */
            ObjectScope object(*this, argument.value.source);

            values[index] = resolveValue(builder, argument.value, nullptr, requests[index]);
            if(!values[index]) return false;
        }
    }

    auto chosen = chooseCandidate(*this, candidates, set, values.pointer(), arguments, name,
                                  nameSource, source, into.args, literals.pointer(), kind);
    if(!chosen) return false;

    into.function = chosen->function;

    // The winner's mapping, copied out because the candidate list is a local of this walk. Element
    // by element rather than by assignment: a `SmallArray` is deliberately not copy-assignable, so
    // that a list of anything with an identity cannot be duplicated by accident.
    for(Size i = 0; i < chosen->map.sources.size(); i++) into.map.sources.push(chosen->map.sources[i]);
    for(Size i = 0; i < chosen->map.positions.size(); i++) into.map.positions.push(chosen->map.positions[i]);
    for(Size i = 0; i < chosen->map.written.size(); i++) into.map.written.push(chosen->map.written[i]);

    return true;
}

/*
 * A call whose name and arity reach more than one candidate - the selection above, then the call.
 *
 * Two functions because the *first* half serves something that is not a call: a `for` loop runs an
 * iterator of a name that may reach several, and what it does with the winner is open a continuation
 * region rather than emit a call. `handoverCall` takes the choice this made, which is what keeps
 * R2-R5 one rule instead of two that would have to agree.
 */
Node* Resolver::resolveOverloadedCall(Builder& builder, ast::AppExpr& written,
                                      const OverloadSet& set, StringId name, LocationId nameSource,
                                      DeclPtr<Type> hint, LocationId source, Receiver receiver) {
    OverloadChoice chosen;
    HeldList<SmallArray<Node*, 8>> held(*this, chosen.values);

    if(!selectOverload(builder, written, set, name, nameSource, source, receiver,
                       CandidateKind::Any, chosen)) {
        return nullptr;
    }

    recordCallee(nameSource, chosen.function);

    auto call = finishOverloadedCall(builder, written, *decl()[chosen.function], chosen.map,
                                     chosen.values, chosen.args.unwrap(), hint, source, receiver);

    // Again, with the winner's own trial now that its arguments are prepared - which is what names
    // the instance an overloaded class member's call was served by. The first record stands for a
    // call whose arguments did not.
    if(call) recordCallee(nameSource, chosen.function, &chosen.args.unwrap());
    return call;
}

/*
 * An operator's candidates, with the map each takes its operands through.
 *
 * There is **no shape to check**, which is the one step a written call takes here and an operator
 * does not: `checkCallShape` asks whether the names written name positions this candidate has and
 * whether what is left has defaults, and an operator writes no names at all. The second half of
 * that question is already answered too - the set was found at this arity, and
 * `Program::addFunctionName` files a declaration under an arity only where the positions behind it
 * take their own defaults. So every candidate of the set is a candidate of this expression, and
 * `positionalMap` is the whole mapping.
 */
static void operatorCandidates(Resolver& resolver, const OverloadSet& set, Size operands,
                               SmallArray<Candidate, 4>& into) {
    for(Size i = 0; i < set.size(); i++) {
        Candidate candidate;
        candidate.function = set.at(i);

        assertTrue(resolver.decl()[candidate.function]->acceptsArity(resolver.decl(),
                                                                     U32(operands)));

        positionalMap(*resolver.decl()[candidate.function], operands, candidate.map);
        into.push(static_cast<Candidate&&>(candidate));
    }
}

/*
 * What an overloaded operator's operands are asked for - `agreedShape` at an operator's shape.
 *
 * Asked before either operand is resolved, which is the whole reason it is separate from the
 * selection below: `climb` spends a `Request` on its left operand before it knows which candidate
 * will take it, exactly as `resolveOverloadedCall` does, and the answer has to serve every
 * candidate. See `agreedShape`, where the two kinds of disagreement and their two answers are
 * argued.
 *
 * **A deferred operand is refused here rather than agreed about.** `&&` and `||` are the only
 * operators whose right operand is unevaluated and they are `op.def`'s `Flow` roles, laid out as a
 * branch rather than resolved as a call - so a `@lazy` position reaching this function is an
 * operator overloaded with one, and there is nothing to build a thunk over: the right operand of an
 * infix operator is a *span of the chain* that precedence climbing discovers, not an expression any
 * one AST node holds. A call of the same name still reaches it, which is what the message says.
 *
 * `operands` is one or two - a prefix operator and an infix one ask the same question, and asking it
 * once is what keeps `-x` and `x - y` selecting by the same rule.
 */
bool Resolver::operatorRequests(const OverloadSet& set, StringId name, Size operands,
                                LocationId source, Request* into) {
    SmallArray<Candidate, 4> candidates;
    operatorCandidates(*this, set, operands, candidates);

    SmallArray<Request, 8> requests;
    SmallArray<bool, 8> deferred;
    if(!agreedShape(*this, candidates.pointer(), candidates.size(), set, operands, source, requests,
                    deferred)) {
        return false;
    }

    for(Size i = 0; i < operands; i++) {
        if(!deferred[i]) continue;

        context.diagnostics.error("%@ names more than one declaration and one of them leaves an operand unevaluated, which an operator cannot do - write it as a call, where the deferred operand is an argument the selection can be made about"_v,
                                  source, context.findName(name));
        return false;
    }

    for(Size i = 0; i < operands; i++) into[i] = requests[i];
    return true;
}

/*
 * And the choice, over operands already resolved - R2 through R5 through `chooseCandidate`, and
 * then the call itself through `callFunction`.
 *
 * One rule with a written call's, which is what those two exist for: an operator is a call written
 * in a different order, so a set that selected one way at `advance(p, n)` and another at `p + n`
 * would be two answers to one question, and a winner whose positions were filled by a walk of this
 * function's own would be two answers to the next one.
 *
 * `winner` is `chosen`'s own trial, handed to `callFunction` rather than let it solve the signature
 * again - the same reason `finishOverloadedCall` takes one. `x + y` at `(I32, I64)` through an
 * overloaded `+` is exactly this call: `chooseCandidate` widens the shared operand type to decide
 * `+` accepts both operands, and `callFunction` has to keep believing it, not check the narrower one
 * against the type as it stood before that widening ran.
 */
Node* Resolver::selectOperatorCall(Builder& builder, const OverloadSet& set, StringId name,
                                   LocationId source, Node* const* values, Size operands,
                                   DeclPtr<Type> hint, ast::Expr* const* pending,
                                   const intrinsic::Known* literals) {
    SmallArray<Candidate, 4> candidates;
    operatorCandidates(*this, set, operands, candidates);

    Maybe<TypeArguments> winner;
    auto chosen = chooseCandidate(*this, candidates, set, values, operands, name, source, source,
                                  winner, literals);
    if(!chosen) return nullptr;

    return callFunction(builder, *decl()[chosen->function], values, operands, source, hint, pending,
                        &winner.unwrap());
}

} // namespace mir
