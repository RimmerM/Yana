#pragma once

#include "expr.h"

/*
 * Solving a call's type variables.
 *
 * Every decision the resolver makes about a call asks one question: what would this signature's
 * type variables have to be for these arguments to fit it, and does anything answer them. It was
 * asked in six places - an ordinary function match, a class function match, a generic call, a
 * lens's continuation shape, `Try`'s carrier and a record construction - and each of them wrote out
 * the same steps: a binding list one entry per variable, the arguments bound positionally, the
 * expected result filling only what the arguments left open, the settle that turns a literal
 * variable into the type it defaults to, and the functional dependencies that answer a position
 * nothing wrote.
 *
 * One of the steps is deliberately not here: whether an argument fits a position with *no* variable
 * in it is `ExprResolver::convertibleType`, which is what a non-generic signature is judged by. A
 * solve that answered that itself would be a second opinion about it, and was - it knew about
 * `Widen` and not about borrows or `@bits`, so the same position meant two things depending on
 * whether some other parameter mentioned a variable.
 *
 * The steps are here once. `Solver` performs them, and `Solution` is what it answers - the
 * substitution, the instance those types select, and what is left over. What each answer *means* is
 * the caller's: an overload match discards a solve that does not fit, a generic call reports the
 * position it stopped at, and a continuation's shape is inferred from a solve that is deliberately
 * incomplete. That split is the whole of why the callers still differ, and it is the same one
 * ClassSelection makes - facts here, policy there.
 *
 * **What this is not.** Binding is still one-way and positional: a signature's parameter is a
 * pattern, the argument is the type it is matched against, and nothing propagates backwards from a
 * later position to an earlier one. Two positions that mention one variable are reconciled by the
 * rules in `bind` - a literal takes the other's type, two concretes take their common `Widen` - and
 * not by unification, so `f(x: Maybe(a), y: a)` still decides `a` from whichever position binds it
 * first. Analysis-Status.md point 5 asks for the solver that lifts that, and this is the half of it
 * that could be built without changing what any program means: one statement of the rules, in one
 * place, with the answer named. Structural inference and higher-kinded variables are what the
 * shape below is *for*, and they are not in it yet.
 */

/*
 * What a call site's types have to be for one signature to serve it.
 *
 * The answer, and nothing about what to do with it. A caller reads the state, and every state but
 * `Solved` is a fact rather than a verdict: an incomplete solve is a failure to an ordinary call
 * and the ordinary case for a `for` loop's iterator, whose continuation is what decides the rest.
 */
struct Solution {
    // One type per variable of the environment solved against, in schema order. A null entry is a
    // variable this call decided nothing about.
    TypeList types;

    /*
     * The instance those types select, for a solve about a class function.
     *
     * Null is not a failure: a signature that fits with no instance for its types is what separates
     * "wrong function" from "nothing implements this here", which are very different diagnostics.
     * `instanceArgs` is what selecting it bound the instance's *own* variables to - empty for the
     * concrete head that is the usual case, one type per variable for a parametric one.
     */
    ModulePtr<ClassInstance> instance = nullptr;
    TypeList instanceArgs;

    enum class State: U8 {
        // Every variable holds a type.
        Solved,

        // Argument `position` does not fit the parameter it fills, so this signature is not a
        // candidate at all. Which is a different thing from every state below it.
        Argument,

        // Variable `position` is one nothing here decided and nothing defaults.
        Undecided,
    };

    State state = State::Solved;
    Size position = 0;

    /*
     * Set on a solve whose only problem was a functional dependency nothing answered: the signature
     * fits, the deciding positions are the enclosing body's own type variables, and no requirement
     * it declares says what they determine.
     *
     * Carried beside the state rather than being one of them, because it is a diagnostic and not a
     * result - what the author has to write is the constraint, and "nothing accepts this call" is
     * true and useless. See ClassMatch::undeclaredDependency, which is where it ends up.
     */
    bool undeclaredDependency = false;

    // Whether the signature fits these arguments at all. Deliberately not the same question as
    // whether the solve is complete: a call that fills a leading prefix of a signature leaves the
    // rest of the variables to whatever fills the rest of it.
    bool fits() const { return state != State::Argument; }

    explicit operator bool() const { return state == State::Solved; }
};

/*
 * What a position holding no value at all means to a solve.
 *
 * Three callers, three answers, and they are three different questions rather than a preference.
 * `ResolvedArg::Failed` is a position something below already reported on, so what it carries is
 * not a type anybody wrote.
 */
enum class Unresolved: U8 {
    // An overload match. A position nothing was worked out for decides nothing about a signature
    // either, and judging one against `{}` - which is what a failed position's null payload reads
    // as - rejects candidates for a type the author never wrote. Binding stops there: a caller that
    // rejects the whole solve has no use for the positions after it.
    Rejects,

    // A continuation's shape. The positions that *did* fit still say what the call hands over, and
    // the call below re-runs the whole of selection anyway - so this is the one mode that keeps
    // binding past a position it could not use.
    Skips,

    // An emitted call. `()` is what a synthesized argument's null value legitimately is - a
    // specialization's cloned operands, a witness entry's own parameters - and those calls are
    // reached with no diagnostic behind them, so the position binds as the unit value it is.
    Binds,
};

/*
 * Where the positions a functional dependency determines came from.
 *
 * Two sources, and which one applies is decided by the deciding positions rather than by the
 * caller: a `c` that names a type reads its `a` off the instance the table selects, and a `c` that
 * is the enclosing body's own variable reads it off the constraint that body declares. A body's
 * meaning is fixed by its own signature (Design-Memory §2.1), so the second is not a fallback for
 * the first - a bare variable must not select a blanket instance and commit the body to it.
 */
enum class Determined: U8 {
    // The class declares no dependency, or nothing was left for one to answer.
    Nothing,

    // The instance table answered, and `instance` holds what it selected.
    Instance,

    // The enclosing function's declared requirement answered.
    Requirement,

    // The class determines these positions and nothing in scope says what they are. The constraint
    // has to be declared: inferring it would mean inventing a variable every caller then has to
    // satisfy without the author having written it.
    Undeclared,
};

/*
 * The steps of one solve, over one binding list.
 *
 * Built per solve rather than held, like IrEditor: it is a resolver, an answer to fill and the
 * variables to fill it with. Nothing here reports - a solve is asked speculatively by every
 * overload match, so what it knows is written into the answer and the caller decides whether that
 * is worth a diagnostic.
 */
struct Solver {
    Solver(ExprResolver& resolver, Solution& solution, Size variables);

    /*
     * One position of a signature, against the type the call site has there.
     *
     * `widen` says which direction this is. An argument may be widened, sliced or converted into
     * its parameter, because needing a conversion is part of fitting; a *result* may not, because
     * what a call produces has not decided the type arguments by needing one. That asymmetry is the
     * one-directional rule the whole resolver follows, stated for one position.
     */
    bool bind(TypePtr pattern, TypePtr actual, bool widen);

    // The written arguments, against the parameters they fill. Positions past the end of either
    // list are left to whatever supplies them - a `for` loop's continuation is the one that does.
    void bindArguments(ModulePtr<Function> signature, Buffer<ResolvedArg> args, Unresolved unresolved);

    /*
     * The expected result, which fills only the variables the arguments left open.
     *
     * A binding a *literal* argument made is still open in this sense: it is a default waiting to be
     * overridden rather than a decision, which is what keeps `inc(1) :: Long` a Long computation
     * while `inc(x) :: Long` on an `Int` x stays an Int one widened afterwards. A result that does
     * not fit at all is not a failure - it decides nothing, and the conversion is reported where it
     * happens rather than here.
     */
    void bindResult(TypePtr declared, TypePtr target);

    // Literal variables over [from, limit) take the types they default to. False, with the answer
    // set to `Undecided`, at the first one that has no type to take.
    bool settle(Size from, Size limit);
    bool settle() { return settle(0, solution.types.size()); }

    /*
     * Settling with the enclosing constraints' dependencies given the last word.
     *
     * Three steps rather than one, and the middle one is the whole reason this exists. A binding a
     * literal made is not an answer: `fn (Index(c, k, v)) at(xs: c, i: k) -> v` called as
     * `at(xs, 0)` binds `k` from the `0`, and settling that gives `Int` - so by the time the
     * dependency is asked, `k` looks decided and the instance is never consulted. Which is
     * backwards. `c` decides `k`, and a container whose key is `Size` should take the literal at
     * `Size` rather than have no instance at `Int`.
     *
     * So the dependency is asked with those positions cleared, and what it answers wins. A position
     * no instance decides keeps the default the settle already gave it, which is why the answer is
     * merged back rather than assigned: a class that determines nothing is unaffected, and so is
     * every call whose arguments were not literals.
     *
     * The settle still comes first for the *deciding* positions, and only what is already decided
     * can be settled - the dependency is answered by looking an instance up, and `sum([1, 2, 3])`
     * binds the container to a literal type until something defaults it. A deciding position is
     * never one of the cleared ones: `Array(<literal>)` is a record and not itself a literal, which
     * is what keeps the lookup answerable.
     */
    void settleDependencies(GenEnv& env, LocationId source);

    /*
     * Settles the variables this solve decided, and lets each one it did not stand for itself -
     * answering which those were, as a bit per position.
     *
     * What a call *hands over* is asked this way rather than through `settle`. At least one variable
     * is always open there - a continuation's own result is what the block below it decides, and
     * nothing above can have - so an empty slot is the ordinary case rather than a failure, and what
     * is *not* fine is a handed type that depends on one. Substituting through a list with holes in
     * it builds types *around* the holes rather than leaving them alone (see bindingsOrVariables in
     * generic.cpp), so a variable standing for itself is what makes the answer readable at all, and
     * the mask is what says which of it is still about this call.
     *
     * A decided variable whose literal has no default settles to nothing and is left that way,
     * unmarked: it is not open - this call did decide it - and what it produces is a null type where
     * it is substituted, which is where it is reported.
     */
    U64 settleOpen(GenEnv& env);

    // Whether any variable from `from` on is still undecided.
    bool anyOpen(Size from) const;

    ExprResolver& resolver;
    Solution& solution;
};

/*
 * The whole of a plain function's solve: its arguments, the expected result, the variables its own
 * constraints determine, and the settle that commits what is left.
 *
 * One statement of what "this call fits this generic signature" means, asked speculatively by
 * `matchFunction` and performed for real by `emitGenericCall`. The two used to be written out
 * separately and had to agree; the erased call's missing borrow guard is what that cost the last
 * time two copies of one rule drifted.
 *
 * A callee that is not generic solves to nothing and says so - there are no variables to decide, so
 * whether the arguments convert is `convertible`'s question rather than this one's.
 */
void solveSignature(ExprResolver& resolver, ModulePtr<Function> callee, Buffer<ResolvedArg> args,
                    TypePtr target, LocationId source, Unresolved unresolved, Solution& out);

/*
 * The whole of a class function's solve, which is the same one with the class's own dependency in
 * the middle of it.
 *
 * The determining positions settle *before* the rest, because the instance they select is what
 * decides the rest. Settling everything first would ask the table for a literal's type rather than
 * for its default and find nothing; filling first would leave the determined positions holding
 * whatever a literal determiner happened to point at.
 */
void solveClassFun(ExprResolver& resolver, GlobalPtr<TypeClass> typeClass, ModulePtr<Function> signature,
                   Buffer<ResolvedArg> args, TypePtr target, Solution& out);

/*
 * Fills the positions a class's functional dependency determines, in place, from whichever of its
 * two sources applies - see Determined.
 *
 * `args` is the class's type arguments with a hole in each position the caller wants answered, and
 * what the dependency says is what comes back in them - a determined position the call site had
 * already decided is answered by the instance rather than kept, which is the same "the dependency
 * has the last word" rule Solver::settleDependencies applies to a literal's default.
 *
 * The requirement half fills only the holes, because there is no lookup there to disagree with: the
 * deciding positions are what it was matched on, and a determined one the caller decided is an
 * ascription with nothing to overrule it.
 *
 * `bindGeneric` is passed through to resolveDetermined, for a caller asking what an instance *looks
 * like* rather than which one to call. It does not lift the rule about a bare type variable: one in
 * a deciding position selects nothing whatever the caller is asking, because a body's meaning is
 * fixed by its own signature and a blanket instance would silently replace the one its caller's
 * type actually has.
 */
Determined fillDependency(Module& module, Function& function, GlobalPtr<TypeClass> typeClass,
                          TypeList& args, InstanceMatch& instance, bool bindGeneric = false);
