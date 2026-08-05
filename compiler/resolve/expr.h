#pragma once

#include "builder.h"
#include "name.h"
#include "place.h"
#include "../parse/ast.h"

/*
 * The AST -> IR translation for one function body.
 *
 * ExprResolver is shared by the four expr_*.cpp files rather than being local to one of them:
 *
 *   expr.cpp            the resolve() dispatch, literals, conversions, control flow and `let`.
 *   expr_call.cpp       operator fixity, precedence climbing, and call/overload selection.
 *   expr_construct.cpp  places and projections, tuple and record construction, field access.
 *   expr_pat.cpp        patterns, refutability, exhaustiveness, and `match`.
 *
 * The division follows what the resolver has to know rather than what it produces: everything
 * in expr_construct.cpp is about addressing storage, everything in expr_pat.cpp is about
 * deciding which of several shapes a value has, and the two meet only through Place.
 */

/*
 * One name in scope.
 *
 * An immutable binding is a name for an SSA value and nothing more. A `let &x` is a name for
 * *storage*: it has a local, reads of it load and assignments to it write, so that the two
 * statements of `let &i = 0` / `i = i + 1` are about the same slot rather than about two values.
 *
 * A third form is a name for a *borrow* - `let &entry = f(...)`, where the storage is whatever the
 * callee's return-root group named. It is a place like the second, and differs only in what roots
 * it. Nothing here checks anything: exclusivity and last use are resolve/analyze.cpp's, stated over
 * the places these produce.
 */
struct Binding {
    StringId name = 0;
    ModulePtr<Value> value = nullptr;
    U32 local = maxLimit<U32>;

    // Set for a name bound to a borrow rather than to storage of its own - `let &entry = f(...)`,
    // where what the name refers to is whatever the callee's return-root group named. The binding
    // is a place either way; only what roots it differs.
    ModulePtr<Value> borrow = nullptr;

    /*
     * A fourth form: a name a lambda body captured, which lives in the environment the body was
     * handed rather than in this frame at all.
     *
     * `captureField` is which word of that environment holds it, and `captureBorrow` says whether
     * the word is the value or a borrow of storage the enclosing frame still owns - Design-Memory
     * §8's two answers, decided where the capture was created rather than where it is read.
     */
    bool captured = false;
    bool captureBorrow = false;
    U16 captureField = 0;

    // A fifth: the name of a `@lazy` parameter, which holds the thunk the caller built rather than
    // the value the signature declared. Reading it is what runs the caller's expression, so this
    // is the one binding whose use is an effect - see ExprResolver::force.
    bool lazy = false;

    // Where the name was introduced - the pattern, the parameter, or the `let`. Carried so that
    // the editor can jump to it and so that two `x` in two scopes are two symbols: a slot index is
    // not an identity, and a name is one only within the scope that bound it. See resolve/index.h.
    LocationId definition = kNullLocation;

    bool isPlace() const { return captured || local != maxLimit<U32> || borrow != nullptr; }
    Place place() const { return borrow ? Place::inBorrow(borrow) : Place::inLocal(local); }
};

/*
 * One binding a lambda body named that belongs to an enclosing function - Design-Memory §8.
 *
 * There is no capture list, so this is discovered rather than declared: the first time the body
 * names an outer binding, one of these is appended and the environment gains a word. `convention`
 * is the ordinary binding convention, inferred from what the body does with the name, and it is
 * what decides whether the environment holds the value or an address.
 */
struct Capture {
    StringId name = 0;

    // The captured value's type. The environment's field is `&T` for a by-reference capture and
    // `T` for the two that own, which is the whole of what the convention changes here.
    TypePtr type = nullptr;
    ast::BindType convention = ast::BindType::Borrow;
    bool byReference = false;

    // The enclosing binding's own declaration, carried through so that a capture navigates to what
    // it captured rather than to the body that captured it.
    LocationId definition = kNullLocation;
};

// One class function that fits a call, together with what its class's type variables had to be
// and the instance that supplies them. `instance` is null when the signature fits but nothing
// implements it for these types, which is a different diagnostic from "wrong function".
//
// `instanceArgs` is what selecting that instance bound *its* own variables to, which is empty for
// the concrete head that is the usual case and one type per variable for a parametric one.
struct ClassMatch {
    GlobalPtr<TypeClass> typeClass = nullptr;
    ModulePtr<ClassInstance> instance = nullptr;
    TypeList args;
    TypeList instanceArgs;
    U16 index = 0;

    /*
     * Set on a *failed* match whose only problem was a functional dependency nothing answered:
     * the signature fits, the deciding positions are this body's own type variables, and no
     * requirement of the enclosing function says what they determine.
     *
     * Kept apart from the match itself because it is a diagnostic and not a result - "no class
     * function head accepts (c)" is true and useless, since what the author has to write is the
     * constraint rather than a different call.
     */
    bool undeclaredDependency = false;
};

/*
 * What matching every class candidate of one call against its arguments found.
 *
 * The facts, with no policy attached to them: which candidates fit and selected an instance, which
 * fit and found none, which fit but are still about this body's own type variables, and which fit
 * except for a dependency nothing declares. What to *do* with each is the caller's - an ordinary
 * call defers an undecided match to a generic dispatch, and a `for` loop has no dispatch to defer to
 * - and that is the whole of what the two selections differ in.
 *
 * Stated as one type because it drifted while it was two. The `for` loop's copy discarded a match
 * that had no instance instead of keeping it, so with two classes declaring one iterator name, the
 * "no instance of %@" diagnostic named whichever class came first rather than the one that matched.
 */
struct ClassSelection {
    // The candidate that fit with an instance, and how many did. More than one is an ambiguity.
    ClassMatch selected;
    Size selectedCount = 0;

    // The first candidate whose signature fit and whose types have no instance. It is what separates
    // "wrong function" from "no instance for these types", which are very different diagnostics.
    ClassMatch withoutInstance;
    Size withoutInstanceCount = 0;

    // Matches on this function's own type variables. Which class a call is, and with which type
    // arguments, is decided here and once; only the instance has to wait until the types become
    // concrete.
    SmallArray<ClassMatch, 4> undecided;

    // Every class that turned out to apply, kept only so an ambiguity can name them all.
    SmallArray<GlobalPtr<TypeClass>, 4> applicable;

    // A candidate the signature fit and a functional dependency did not, kept so the failure can be
    // reported as the missing constraint it is rather than as a call nothing accepts.
    GlobalPtr<TypeClass> undeclared = nullptr;
};

// Gives `into` everything `from` matched. Not assignment: the two lists are TypeLists, whose
// assignment is deleted precisely so that this reads as the replacement it is - see SmallArray.
inline void adopt(ClassMatch& into, const ClassMatch& from) {
    into.typeClass = from.typeClass;
    into.instance = from.instance;
    into.index = from.index;

    replaceContents(into.args, from.args);
    replaceContents(into.instanceArgs, from.instanceArgs);
}

struct ExprResolver;

// What an editor shows for a class function's name, recorded at the point the call decided which
// class and which instance answered it. Shared with the `for` loop's own selection, which reaches a
// class iterator without going through resolveCall.
void recordClassFunReference(ExprResolver& resolver, LocationId source, ClassMatch& match,
                             ModulePtr<ClassInstance> instance);

/*
 * Where `break` and `continue` go.
 *
 * A `while` has both blocks before its body and jumps to them outright. A counted `for` does not:
 * its step and its exit are created *after* the body, so that the block list stays in the reverse
 * postorder `resolve/lower.cpp` walks it in and `compiler/opt` rebuilds it in. The sites are
 * collected instead and terminated once the blocks they leave to exist, which is what
 * ContinuationExit already does for a `return` inside a lifted continuation.
 */
struct LoopTarget {
    ModulePtr<Block> continueBlock;
    ModulePtr<Block> breakBlock;

    Array<ModulePtr<Block>>* deferredContinue = nullptr;
    Array<ModulePtr<Block>>* deferredBreak = nullptr;
};

/*
 * Where a deferred right operand resumes from.
 *
 * `resolveBinary` flattens an infix chain into operands and operators before re-associating it, so
 * the right operand of an operator is a *span* of those two lists rather than an AST node anyone
 * could point at - `a && b + c` has no node for `b + c` at all. What is remembered is therefore the
 * position precedence climbing would have continued from, which resolving it again from replays
 * exactly the same sub-chain into whichever block the force happens in.
 */
struct DeferredChain {
    SmallArray<const ast::Expr*, 8>* operands = nullptr;
    SmallArray<StringId, 8>* operators = nullptr;
    SmallArray<LocationId, 8>* operatorSources = nullptr;
    Size operandIndex = 0;
    Size operatorIndex = 0;
    U8 minimumPrecedence = 0;
};

/*
 * A `@lazy` argument, between the call site that wrote it and the callee that asked for it.
 *
 * Exactly one of the four forms is set, and which one it is decides whether the program pays for a
 * closure at all:
 *
 *  - `expr`, an argument of a written call, still unresolved;
 *  - `chain`, the right operand of an infix chain - see DeferredChain;
 *  - `thunk`, the nullary closure an opaque callee is handed, and what a body forcing its own
 *    `@lazy` parameter holds;
 *  - `value`, an argument that was already evaluated before anything knew the position was lazy.
 *    Not a promise broken: it is only reached where a value is all there is, and forcing it is
 *    reading what is already there.
 *
 * The first two are the whole point. A callee that can see the argument - an intrinsic, and
 * therefore `&&` on a Bool - emits it into the block where it is needed, so short-circuiting is a
 * branch and no closure, no call, and no allocation exist anywhere in the result.
 */
struct Deferred {
    const ast::Expr* expr = nullptr;
    const DeferredChain* chain = nullptr;
    ModulePtr<Value> thunk = nullptr;
    ModulePtr<Value> value = nullptr;

    // The type the parameter declared, filled in once the callee is known.
    TypePtr type = nullptr;

    bool isSet() const { return expr || chain || thunk || value; }
};

/*
 * What one argument position of a call is, at the point a callee has to be chosen for it.
 *
 * The distinction exists because this resolver spells four different things as a null value, and
 * three of them are not the fourth: a position that failed is the only one a call must stop on. A
 * `{}` argument, or one whose call returns `{}`, is a perfectly good value that carries nothing; a
 * `@lazy` position is not a value *yet*, on purpose.
 *
 * Stated per position rather than as a bitmask, which is what it used to be. Two `U32` masks meant
 * the answer simply ran out after argument 32: a `@lazy` position past that was evaluated eagerly,
 * and a unit argument past it aborted the whole call as though its expression had been reported on.
 * Neither had a symptom short of the wrong program, and neither is a limit anything about a call
 * justifies - so the list is as long as the call is.
 */
enum class ArgResult: U8 {
    Value,    /// The position produced a value, which is what `ResolvedArg::value` holds.
    Unit,     /// It produced a value carrying nothing, which is spelled as no value at all.
    Deferred, /// A `@lazy` position, left unevaluated - `ResolvedArg::promise` holds it.
    Failed,   /// It was reported on, so nothing further should be said about this call.
};

/*
 * One argument of a call, between the expression that produced it and the callee that takes it.
 *
 * The four states above and their payloads travel together because they are one fact about one
 * position. They used to be three parallel lists - the values, the `Deferred` entries, and the
 * states saying which null was which - each of which a caller could pass short, absent or out of
 * step with the others, and each of which every step of selection had to index by hand. What is
 * left is a list of these: as long as the call is, filled in once at the point the position is
 * decided, and read the same way by every candidate the call is matched against.
 *
 * A plain `ModulePtr<Value>` converts to one, which is the reading a synthesized call - a pattern's
 * `==`, an array literal's `slice` - has always had: its arguments are values this resolver just
 * produced, so nothing there means the value it was made from failed. A call site that knows better
 * says so with `unit()` or `failed()`.
 */
struct ResolvedArg {
    ResolvedArg(ModulePtr<Value> value = nullptr):
        value(value), state(value ? ArgResult::Value : ArgResult::Failed) {}

    static ResolvedArg unit() { return ResolvedArg(nullptr, ArgResult::Unit); }
    static ResolvedArg failed() { return ResolvedArg(nullptr, ArgResult::Failed); }

    // A `@lazy` position. The promise is empty where the call site only knows *that* the position is
    // deferred - the overload set answers that before any argument is resolved, see
    // OverloadSet::strictness - and is filled in by whoever has the argument to put in it.
    static ResolvedArg deferred(Deferred promise = Deferred()) {
        auto arg = ResolvedArg(nullptr, ArgResult::Deferred);
        arg.promise = promise;
        return arg;
    }

    bool isValue() const { return state == ArgResult::Value; }
    bool isDeferred() const { return state == ArgResult::Deferred; }
    bool isFailed() const { return state == ArgResult::Failed; }

    // The value this position holds, which is null for every state but `Value`. `valueType` answers
    // `{}` for it, which is what makes the unit case an ordinary argument to everything below.
    ModulePtr<Value> value = nullptr;

    // Set exactly for `Deferred`, and only fully once the callee is known: the parameter type it was
    // declared at is what decides where the argument runs and what it converts to.
    Deferred promise;

    ArgResult state = ArgResult::Failed;

private:
    ResolvedArg(ModulePtr<Value> value, ArgResult state): value(value), state(state) {}
};

// The arguments of one call. Eight inline for the same reason ValueList holds eight - see
// util/container.h - and it is the list every call is built in, matched from and emitted through.
using ArgList = SmallArray<ResolvedArg, 8>;

/*
 * Whether any position of this call has already been reported on.
 *
 * `Failed` is the one state that stops a call, and this is where that is decided. Selecting an
 * overload against a type nobody worked out reports a second, worse diagnostic about a call the
 * author may not have got wrong, and emitting one builds a call with a hole where an argument should
 * be - so a written call asks this once, at the point it commits to a callee, and every route out of
 * that point is covered: the plain function `resolveCall` decided on by itself, the overload set
 * `emitCall` selects from, and the two lens ones in expr_lens.cpp.
 *
 * Deliberately not inside `emitDirectCall` and the rest. Those are also reached with arguments this
 * resolver has just built - a specialization's cloned operands, a witness entry's own parameters -
 * where a position holding no value is the ordinary unit case rather than a report, and where
 * declining to emit would lose a call nobody said anything about.
 */
inline bool anyArgumentFailed(Buffer<ResolvedArg> args) {
    for(auto& arg: args) {
        if(arg.isFailed()) return true;
    }

    return false;
}

/*
 * What sort of call site is selecting from a set, and the whole of what that changes.
 *
 * There were two implementations of Design.md's R5 - one for calls and one for `for` loops - and
 * three of the four defects a review of the first pass found lived in the gap between them: the
 * loop's copy shadowed the class half, pushed the wrong signature into its arguments, and blamed
 * the wrong class for a missing instance. Each was a rule the ordinary selection already kept.
 *
 * These three fields are what stopped them being one function, written down so that they are the
 * argument for the parameter rather than a reason to copy the code again.
 */
struct CallShape {
    /*
     * The kind of function that may serve this call, and it is exclusive in both directions: an
     * ordinary call that names an `iter fn` is told to write the loop, and a loop that names a plain
     * function is told it is not an iterator. Which is why the candidates of the *other* kind are
     * kept rather than dropped - see OverloadSet::wrongKind.
     */
    ast::FunKind kind = ast::FunKind::Plain;

    /*
     * Whether the *plain* half must be declared as `kind`, which only a loop requires.
     *
     * The asymmetry is real and is what the sugar rests on: a `lens fn` or an `iter fn` whose
     * continuation is written out is an ordinary call and stays one - `plusOne(4, (n: Int) -> n * 3)`
     * is that form, and it is always legal. What a loop needs instead is a callee with a
     * continuation parameter left over for the loop to fill, which only an `iter fn` has.
     *
     * The *class* half is split by kind whatever this says, because a class member is not desugared
     * (see resolveSignature): its signature has no continuation parameter to write out, so an
     * ordinary call to a class `iter fn` is the wrong syntax rather than a longer spelling of the
     * right one. See OverloadSet::wrongKind.
     */
    bool requiresKind = false;

    /*
     * How many trailing parameters the call site does not write.
     *
     * One for a loop, which supplies the continuation itself - so the plain half declares one more
     * parameter than the call has arguments, and is matched over a leading prefix of its signature.
     * The plain half only: a class member is not desugared (see resolveSignature), so it declares
     * exactly what the call site writes and needs no adjustment at all.
     */
    Size supplied = 0;

    // Whether an undecided match may be left to a generic dispatch. A loop has none - it needs an
    // implementation to desugar its body against - so for one an undecided match is a class that
    // cannot serve the call here rather than one that will serve it later.
    bool dispatches = true;

    bool isLoop() const { return kind == ast::FunKind::Iter; }
};

/*
 * The overload set one call is selected from, gathered once.
 *
 * Design.md's R1 keys the set by (name, arity) and admits at most one plain function beside any
 * number of class functions, so this is that key and the two halves it names. Everything a call does
 * before it has a callee - deciding which positions are `@lazy`, resolving the arguments, matching
 * each candidate - is asked of this rather than of the name, and it is looked up once.
 *
 * **The two halves treat the arity half of the key differently, and that is a decision.** `direct`
 * is exact: R1 admits one plain function per (name, arity), so a wrong-arity one is not a candidate
 * and is held apart in `mismatched`, where the only thing that reads it is the diagnostic it is
 * good for. The class half is every candidate of the *name*, because rejecting on arity there is
 * what produces "no class function `==` accepts (Int, Int, Int)" - the list of types is the
 * diagnostic, and it needs the candidates that did not fit to say it. `matchClassFun` is the one
 * place that states the rule, and it states it as part of matching rather than beside it.
 *
 * It used to be looked up three times per written call, and the three disagreed about what they
 * were for: the strictness pass gathered the set to read strictness off it and threw it away,
 * `resolveCall` gathered it again to find out whether the plain function was the whole of it, and
 * `emitCall` gathered it a third time to select from. Each walk is `findClassFunctions`, which scans
 * every visible module's class-function table - so an infix chain of five operators did fifteen of
 * them for five calls.
 *
 * **Strictness is part of the set rather than a separate answer beside it.** Which positions are
 * deferred has to be decided before any argument is evaluated, which is before selection has run -
 * so it is what every candidate of one (name, arity) has in common, and a set is exactly the thing
 * that can say that. Design.md's rule that strictness is fixed by the signature rather than by the
 * instance is this, stated from the other side: two candidates that disagree are a declaration error
 * rather than a call-site one, and this is the only place the two are visible together.
 */
struct OverloadSet {
    // R1's at-most-one plain function, of this call's kind and at this arity. Null where the name
    // declares one that cannot serve the call - that is `mismatched`, and it is not a candidate.
    ModulePtr<Function> direct = nullptr;

    /*
     * The plain function of this name that this call cannot use, and which is therefore the whole
     * diagnostic when nothing else serves it: it takes a different number of arguments, or it is an
     * `iter fn` where a call was written, or a plain function where a loop was. "takes two arguments
     * but was given three" says more than "unknown function", which is what a set that simply
     * dropped it would leave selection with. Nothing selects it.
     */
    ModulePtr<Function> mismatched = nullptr;

    // The class half of this call's kind, at every arity - see the note above on why this one is not
    // narrowed by arity.
    ClassFunList candidates;

    /*
     * The class functions of the name declared as the *other* kind, which is what makes "chunks is
     * an `iter fn` of class Chunked, so it is run by a `for` loop rather than called" possible.
     *
     * Held apart rather than dropped, and apart rather than mixed in, because the two answers are
     * different: a candidate of the right kind that does not fit is "no class function accepts
     * these types", and one of the wrong kind that fits perfectly is "you wrote the wrong syntax".
     * Only reached when nothing else serves the call.
     */
    ClassFunList wrongKind;

    // What sort of call this set was gathered for - see CallShape.
    CallShape shape;

    /*
     * One entry per position: a `Deferred` one carrying nothing yet where the set declares `@lazy`,
     * and an empty one for the rest. It is the list a call is built in - the caller replaces each
     * strict entry with what resolving that argument produced, and a deferred one keeps the
     * unevaluated argument instead. Whether any position is deferred is then a property of the list.
     */
    ArgList strictness;

    StringId name = 0;
    Size arity = 0;

    // Where the name was written, which is not where the call was: an editor asking about the name
    // under the cursor needs the name. `kNullLocation` for a synthesized call - a pattern's `==`, an
    // array literal's `slice` - which is what keeps those out of the index. See resolve/index.h.
    LocationId nameSource = kNullLocation;

    bool isEmpty() const { return !direct && candidates.isEmpty(); }
};

/*
 * The one callee a call was selected onto, and the arguments selection settled on.
 *
 * What separates it from the emission that follows is that everything *judged* about the call has
 * happened by the time this exists: which half of the overload set serves it, which class and which
 * instance, whether it was ambiguous, whether an argument had to be read through a borrow to be
 * accepted at all. Emitting is then a switch over four cases with nothing left to decide, which is
 * what the seven steps of "normalize calls once" ask for - selection ends in one value, and the call
 * forms are reached from that value rather than from the middle of the selection that found them.
 *
 * `Failed` is a reported failure and not an absence: selection says everything there is to say about
 * a call it cannot serve, because it is the only thing that knows what the alternatives were.
 */
struct ResolvedCallee {
    enum class Kind: U8 {
        Failed,   /// Selection reported; nothing further is to be said about this call.
        Plain,    /// The overload set's plain function - R1's at-most-one - serves it.
        Instance, /// One class function, and the instance selected for these types.
        Dispatch, /// One class function whose instance this body's type variables cannot decide.
    };

    Kind kind = Kind::Failed;

    // Set for `Plain`. Generic or not: which of the two call forms it takes is a property of the
    // callee rather than of the selection, and is read off it where the call is emitted.
    ModulePtr<Function> function = nullptr;

    // Set for `Instance` and `Dispatch`. The instance is null for the second, which is what makes it
    // the second - see emitGenericDispatch.
    ClassMatch match;

    /*
     * The arguments as selection settled them, which is not always the list it was handed.
     *
     * A borrow is transparent for reading, so where nothing in the set accepts the arguments as they
     * stand they are each read through and the whole set matched again. That rewrite is part of
     * deciding the call rather than of emitting it - it is what a candidate was matched against -
     * so what comes out here is the list the selected callee was chosen for.
     */
    ArgList args;

    /*
     * The type arguments a *generic* plain callee was selected at, so that emitting does not solve
     * the same signature against the same arguments a second time. Empty when nothing decided them
     * here, which is not the same as "there are none": R1 admits one plain function per (name,
     * arity), so a set with no class candidates selects it without matching it at all, and the
     * emission solves for itself. A generic signature has at least one variable, so an empty list is
     * unambiguously the absence of an answer.
     *
     * The substitution rather than the whole `Solution`, because that is the part emission needs -
     * what else the solve knew, selection has already turned into `kind` and into the diagnostics it
     * reported. The class half carries a ClassMatch for the same reason and no more.
     */
    TypeList typeArgs;
};

// One `yield` in a `lens fn` body: where it landed, and where it was written. The block is what the
// exactly-once check is stated over and the source is what its diagnostics point at.
struct LensYield {
    ModulePtr<Block> block;
    LocationId source;
};

/*
 * One `return` inside a lens continuation, waiting for the shape of the continuation's result.
 *
 * A `return` there leaves the *enclosing* function rather than the lifted body it is written in, so
 * what it compiles to depends on something not known while it is being resolved: whether any path
 * of the continuation also finishes normally. The block is therefore left without a terminator and
 * finished by finishContinuationExits once the whole body has been seen - see expr_lens.cpp.
 */
struct ContinuationExit {
    ModulePtr<Block> block;
    ModulePtr<Value> value;
    LocationId source;
};

/*
 * One `break` or `continue` inside a `for` loop's lifted body, waiting for the same thing.
 *
 * The loop it leaves is in the function this body was split out of, so it is a value returned to
 * the iterator rather than a jump - and which value depends on whether any *other* path of the same
 * body also returns from the enclosing function, which is not known while it is being resolved. The
 * block is left without a terminator for exactly the reason ContinuationExit's is.
 */
struct ContinuationLoopExit {
    ModulePtr<Block> block;
    bool isBreak = false;
    LocationId source;
};

/*
 * What a lifted continuation turned out to be, which is what the call site has to join against.
 *
 * The three combinations of the two flags are Analysis-Lens.md §5.1's three cases: a continuation
 * that only finishes normally is an ordinary function and the exit signal costs nothing; one that
 * only leaves returns the enclosing function's result outright; and one that does both returns
 * `Outcome(value, exit)`, which is the only shape that pays for a discriminator.
 *
 * A `for` loop's body reads the same three cases against §7.3's step signal instead: `breaks` is
 * what makes the third of them necessary there, since a loop that only ever breaks needs nothing
 * told apart, and `carried` is what an `Exit` of the signal holds once it is.
 */
struct ContinuationShape {
    TypePtr value = nullptr;
    TypePtr outcome = nullptr;
    TypePtr carried = nullptr;
    bool fallsThrough = false;
    bool exits = false;
    bool breaks = false;
};

// What resolving one pattern proved about it. `Never` means the pattern cannot match this pivot
// at all (a type error, already reported); `Always` means no test was emitted, so the following
// alternatives are unreachable; `Maybe` means a test was emitted and control may reach `onFail`.
enum class PatternResult: I8 {
    Never = -1,
    Maybe = 0,
    Always = 1,
};

// One alternative's contribution to a branching expression's result: the block control leaves
// through, and the value produced there. Collected by if/multi-if/match alike, then unified into
// a single phi by finishBranches().
struct BranchArm {
    ModulePtr<Block> end;
    ModulePtr<Value> value;
    LocationId source;
};

// Inline, because the arms of an `if` are two and the arms of the ordinary `match` are three, and
// there is one of these list per branching expression in the program. The eight it holds cover a
// `match` over a reasonably sized sum type; past that it grows like any other array.
using BranchArmList = SmallArray<BranchArm, 8>;

/*
 * One `?.` that skipped, waiting for the chain it is in to say what it should become.
 *
 * Left unterminated on purpose. What the skip produces is the *chain's* result type, which is not
 * known until the rest of the chain has been resolved - so the block is recorded holding the raw
 * reason the carrier exited with, and finished afterwards. The same shape finishContinuationExits
 * uses, and for the same reason: appending to a finished block is what makes a CFG walk read one
 * thing and lower another.
 */
struct OptionalSkip {
    ModulePtr<Block> block = nullptr;
    ModulePtr<Value> reason = nullptr;   // Null for a carrier whose exit carries nothing.
    TypePtr reasonType = nullptr;
    LocationId source = kNullLocation;
};

/*
 * The optional chain currently being resolved - `a?.b.c(x)`.
 *
 * A `?.` skips **the rest of its chain**, not the rest of the function, which is what every
 * language with the spelling means by it and what makes `a?.b.c` produce one wrapped value rather
 * than leaving. So the extent of the skip is a syntactic span - from the `?.` to the end of the
 * chain it is written in - and this is that span, made available to the `?.` nodes inside it.
 *
 * `spine` is the chain's nodes by address. Membership rather than a depth counter, because a chain
 * node reached as an *argument* of one of these is a chain of its own: in `a?.b(c?.d)` the inner
 * `?.` skips to the end of `c?.d` and has nothing to do with the outer one. Comparing addresses is
 * what tells those two apart, and a counter cannot.
 */
struct OptionalChain {
    SmallArray<const ast::Expr*, 8> spine;
    SmallArray<OptionalSkip, 4> skips;

    // The first `?.`'s operand type, which is the wrapper the whole chain comes back in -
    // `Rewrap(carrier, whatever the chain produced)`. Later `?.`s in the same chain may carry a
    // different one, and only their *reasons* have to meet, by the ordinary Widen step.
    TypePtr carrier = nullptr;
};

// The type one name has, without emitting anything to find out. Deliberately separate from
// placeOf(), which for a by-reference capture emits the load that reaches the storage.
TypePtr bindingType(ExprResolver& resolver, const Binding& binding);

// Records one occurrence of a local, an argument or a capture - see resolve/index.h. Called from
// findBinding, which is the one funnel every read of a name in a body goes through.
void recordBinding(ExprResolver& resolver, const Binding& binding, LocationId source);

// Which of the three function-local kinds a binding is, and the slot it addresses. Shared by the
// index and by completion, so a name offered and the same name hovered describe one thing.
struct Symbol bindingSymbol(ExprResolver& resolver, const Binding& binding);

// Records a binding as a *definition*, where it is introduced. See expr.cpp; a no-op in a batch
// compile, like every other recording site.
void recordBindingDefinition(ExprResolver& resolver, const Binding& binding);

struct ExprResolver {
    ExprResolver(Context& context, Module& module, Function& function):
        context(context), module(module), function(function), parse(module.parse),
        global(*module.types), local(*module.arena), current(module.entry(function) - *module.arena) {}

    /*
     * Building blocks.
     */

    Block& block() { return *local[current]; }
    ModulePtr<Value> ref(Value* value) { return value - local; }
    TypePtr valueType(ModulePtr<Value> value) { return value ? local[value]->type : module.scalar.unit; }

    template<class T, class... Args>
    T* emit(LocationId source, StringId name, TypePtr type, Args&&... args) {
        return addInst<T>(module, function, block(), source, name, type, forward<Args>(args)...);
    }

    // create() + append() is emit() split in two, for the instructions whose operands are filled
    // in between the two halves - see builder.h.
    template<class T, class... Args>
    T* create(LocationId source, StringId name, TypePtr type, Args&&... args) {
        return createInst<T>(module, function, block(), source, name, type, forward<Args>(args)...);
    }

    void append(Inst* inst) { IrEditor(module, function).append(block(), inst); }

    template<class T, class... Args>
    ModulePtr<Value> constant(LocationId source, TypePtr type, Args&&... args) {
        return ref(addConstant<T>(module, function, block(), source, type, forward<Args>(args)...));
    }

    void terminate(Inst* inst);
    ModulePtr<Block> addBlock() { return function.addBlock(module) - local; }

    /*
     * A check the compiler inserted, as a call to `Collections.checkCondition`.
     *
     * `failed` is the condition under which the program is wrong, so it reads as the *mistake*
     * rather than as the invariant - `index >= length`, not `index < length`.
     *
     * A call and not a branch, which is the one design decision here. Emitting `if failed then
     * checkFailed()` inline would be shorter and is what this was first: a subscript is expanded
     * *inside* whatever expression contains it, so splitting the current block splits it underneath
     * a construct that had already recorded which blocks it owns. A generic `iter fn` whose body
     * subscripts is the case that finds it - the lifted body's blocks stop being in the reverse
     * postorder lowering walks them in, and a value is then used before its definition is lowered.
     * A call is one instruction in the block that is already current, and the branch lives inside
     * `checkCondition` where nothing is looking. Implementation-Containers.md §15 asks for this
     * shape for its own reason: a library container calls the same function.
     *
     * Emits nothing at all when the checks are off, which is what makes `-no-checks` free rather
     * than cheap - see CompileSettings::checks and Program::checkCondition.
     */
    void emitCheck(ModulePtr<Value> failed, LocationId source);

    // Whether a check would be emitted at all, for a caller that has to decide whether to compute
    // the condition. Answered before the operands are built rather than after, since a length load
    // that nothing reads is still a load until an optimizer removes it.
    bool checksEnabled() const {
        return context.settings.checks && module.program.checkCondition != nullptr;
    }

    /*
     * Values and conversions (expr.cpp).
     */

    ModulePtr<Value> find(StringId name);
    // `source` is where the name was written, for the one diagnostic a lookup can produce: a
    // capture this version cannot make. Omitted by the callers that are asking whether a name is
    // bound at all rather than reading it.
    Binding* findBinding(StringId name, LocationId source = kNullLocation);

    // The storage one name refers to. For an ordinary binding this is Binding::place(); for a
    // capture it is a word of the environment, and for one taken by reference it is the storage
    // that word points at - one more load, at each use, because a capture discovered half-way
    // through a body has no entry block left to hoist it into.
    Place placeOf(const Binding& binding, LocationId source);

    // The place an assignable expression names - a mutable binding, a field of one, or the memory
    // a raw pointer points at. Null root when the expression names no storage, which is the one
    // diagnostic assignment has of its own.
    // `(x)` and `x` name the same thing, and every rule that looks at the *shape* of an
    // expression - a dereference in assignment position, a field of one - has to see through the
    // parentheses to find it.
    const ast::Expr& unwrapNested(const ast::Expr& expr) {
        auto current = &expr;
        while(current->kind == ast::Expr::Nested) current = parse[current->nested];
        return *current;
    }

    // `through` says the place is about to be projected into rather than assigned to as a whole,
    // which is what lets an immutable binding holding a raw pointer root one - see resolvePlace.
    Maybe<Place> resolvePlace(const ast::Expr& expr, bool through = false);
    ModulePtr<Value> resolveAssign(const ast::Expr& expr, const ast::AssignExpr& assignment);
    /*
     * `fresh` is the local count taken before the initializer was resolved, which is the whole of
     * what makes the elision in `adoptableLocal` safe - see there.
     */
    void bindMutable(const ast::VarDecl& declaration, ModulePtr<Value> value, U32 fresh);

    /*
     * The local a mutable binding may take over rather than copy out of, or nothing.
     *
     * A binding whose initializer produced storage *this declaration just made* has no reason to
     * allocate a second slot and copy: the temporary has no other name, so letting the binding be
     * that name is the same program with one allocation and one whole-value write taken out. It is
     * what an immutable binding has always done - `resolveBinding` binds the construction's own
     * storage - and the reason the mutable form could not simply do the same is that the two differ
     * exactly where it matters: aliasing an existing local is harmless for a name that cannot be
     * written and a miscompile for one that can, since `let &y = x` must not make `y = 5` write `x`.
     *
     * `fresh` is what tells the two apart, and it is a count rather than an analysis: a local whose
     * index is below the mark existed before this initializer ran, so it is something the program
     * already had a name for. Everything at or above it was made while building this value.
     */
    Maybe<U32> adoptableLocal(ModulePtr<Value> value, U32 fresh);

    // A name for a borrow someone else's storage backs, rather than for a slot of this frame.
    void bindBorrow(const ast::VarDecl& declaration, ModulePtr<Value> value, bool mutable_);

    // `@heap` and whatever joins it - the attributes written before a `let`. `bindingBase` is where
    // this declaration's own bindings start, which is how the slot it introduced is found.
    void applyBindingAttributes(const ast::VarDecl& declaration, ModulePtr<Value> value, Size bindingBase);
    ModulePtr<Value> makeInt(LocationId source, TypePtr type, U64 value);
    ModulePtr<Value> makeFloat(LocationId source, TypePtr type, F64 value);

    // What reading a module-level name produces: a constant for an immutable global of direct
    // type, and a load of its place for anything else. See expr.cpp.
    ModulePtr<Value> globalValue(ModulePtr<Global> global_, LocationId source);

    // The constant `bits` names at `type` - see expr.cpp. Shared by an immutable global and a
    // field default, which are recorded the same way and for the same reason.
    ModulePtr<Value> constantBits(TypePtr type, U64 bits, LocationId source);
    ModulePtr<Value> convert(ModulePtr<Value> value, TypePtr target, LocationId source, bool implicit = true);

    // Taking a borrow of what a value names, reading through one, or weakening a mutable one.
    // Null when neither type is a borrow, which is every conversion the rest of the language has.
    ModulePtr<Value> convertBorrow(ModulePtr<Value> value, TypePtr from, TypePtr target, LocationId source);

    // Between a `@bits` refinement and what it refines, in either direction. Null when the two types
    // are not related that way, so that convert() falls through to the ordinary paths.
    ModulePtr<Value> convertRefinement(ModulePtr<Value> value, TypePtr from, TypePtr target,
                                       LocationId source);

    // An owned container to a borrow of one: the `{base, length}` descriptor of
    // Implementation-Containers.md §4, taken behind a loan. Null when the target is not a slice, so
    // that convert() falls through to the ordinary paths.
    ModulePtr<Value> convertSlice(ModulePtr<Value> value, TypePtr from, TypePtr target, LocationId source,
                                  bool mut = false);

    // The same conversion where a container is a host array rather than storage - see the definition
    // and Implementation-Containers.md §14. Reached from convertSlice once the loan is taken, so
    // that the two targets differ in what the descriptor holds and in nothing above it.
    ModulePtr<Value> convertSliceJs(ModulePtr<Value> value, const Place& array, const Place& owner,
                                    TypePtr from, TypePtr target, TypePtr element, TypePtr fixed,
                                    LocationId source, bool mut);

    // `arr.length`, as the one host property read it is.
    ModulePtr<Value> hostArrayLength(ModulePtr<Value> items, LocationId source);

    // Whether convert() would succeed implicitly, without reporting anything if it wouldn't.
    bool convertible(ModulePtr<Value> value, TypePtr target, LocationId source);

    /*
     * The same question about two types rather than a value and a type.
     *
     * It is what "fits" means at a position that has no variable to bind, and both halves of
     * selection ask it: `convertible` for a non-generic candidate, and `Solver::bind` for a concrete
     * position of a generic signature. Those were two approximations of one rule until this existed,
     * and the generic one was the shorter - it knew about slices and `Widen` and not about borrows,
     * `@bits` or an error type, so `fn f(x: a, n: U64)` rejected an `@bits(53) U64` that the
     * identical non-generic signature accepted. See Generic.Concrete.yana.
     */
    bool convertibleType(TypePtr from, TypePtr target);

    // One step of `typeClass`'s conversion, or null when no instance relates these two types.
    // Never a chain: `A -> B -> C` is not searched for, which is what keeps conversion as
    // predictable as the no-backtracking rule the rest of resolution follows.
    ModulePtr<Value> emitConversion(GlobalPtr<TypeClass> typeClass, StringId method, ModulePtr<Value> value,
                                    TypePtr target, LocationId source);

    // The unique `Widen` upper bound of two types, or null when neither widens to the other.
    // This is the one place a conversion may decide which overload matches: the positions bound
    // to a single class variable are unified before the instance is looked for, which is what
    // makes `1 + 2.5` reach Num(Float) rather than no instance at all.
    TypePtr commonWiden(TypePtr lhs, TypePtr rhs);

    /*
     * Literals (expr.cpp).
     */

    // A fresh literal variable carrying one literal class.
    TypePtr literalVariable(GlobalPtr<TypeClass> literalClass);

    // The type a literal variable takes when nothing else decided one, or null when its classes
    // have no default they agree on. Pure: speculative overload matching asks this too.
    TypePtr literalDefault(TypePtr type);

    // `type` with a literal variable replaced by its default. Applied wherever an inferred type
    // is about to be committed to - a class's type argument, a specialization's, a branch join.
    TypePtr settleType(TypePtr type);

    // `value` built at the type its literal variable defaults to. Everything else passes through.
    ModulePtr<Value> settle(ModulePtr<Value> value, LocationId source);

    // Whether a literal variable may become `target`: it needs an instance of each of its classes,
    // and a type variable needs the enclosing function to require them instead.
    bool literalFits(TypePtr literal, TypePtr target);

    // One literal variable carrying the classes of both. `1 + 2.5` is why this exists.
    TypePtr mergeLiterals(TypePtr lhs, TypePtr rhs);

    // Builds a literal at `target` by calling the class function that constructs it. Core's
    // instances are intrinsics that fold a constant argument, so a literal at a primitive type is
    // still one constant and the IR is what it always was.
    ModulePtr<Value> materializeLiteral(ModulePtr<Value> value, TypePtr target, LocationId source);

    /*
     * Warns where a written literal does not fit the integer type it is being built at. Called only
     * from the three positions a *written* literal reaches, never from makeInt itself - see its
     * comment.
     *
     * `written` is the magnitude and `negative` the sign, which is how the source has it and the
     * only way the question has an answer at 64 bits: folded together, `-1` and
     * `18446744073709551615` are one number and exactly one of them is an `I64`. An expression's
     * literal is never negative - `-1` is two literals and an operator there - so the third
     * position, a pattern, is the one that passes the flag.
     */
    void checkLiteralRange(LocationId source, TypePtr type, U64 written, bool negative = false);

    /*
     * `implicit` says who owns the conversion to `target`.
     *
     * True - the ordinary case - means this position is asking for a value of that type, so a
     * narrowing conversion is an error about precision. False means an ascription above has already
     * asked for it explicitly, and is threaded down through the forms that have no type of their
     * own: a parenthesis, a block, the arms of an `if` or a `match`. Those are pass-throughs, so the
     * ascription belongs to each leaf rather than to the value they join, and `(x) :: U8` has to
     * mean what `x :: U8` means. It is also what a call takes as `convertResult`, which is the same
     * condition said in the caller's words - the ascription that selected the instance *is* the
     * conversion, so the call must not convert its own result a second time.
     */
    ModulePtr<Value> resolve(const ast::Expr& expr, TypePtr target = nullptr, bool used = true,
                             bool implicit = true);

    // A form whose value is its leaves' value, so an expected type belongs to each leaf rather than
    // to the result. The whitelist an ascription pushes through.
    static bool isPassThrough(const ast::Expr& expr) {
        switch(expr.kind) {
            case ast::Expr::Nested:
            case ast::Expr::Multi:
            case ast::Expr::If:
            case ast::Expr::MultiIf:
            case ast::Expr::Match:
                return true;
            default:
                return false;
        }
    }
    ModulePtr<Value> resolveLiteral(const ast::Expr& expr, TypePtr target);
    ModulePtr<Value> resolveInteger(LocationId source, TypePtr target, U64 value);
    ModulePtr<Value> resolveDecimal(LocationId source, TypePtr target, F64 value);

    // A string literal, per target - see Implementation-String.md part 9 and ConstString.
    ModulePtr<Value> resolveString(LocationId source, StringId text);

    // `"a{x}b{y}c"` - Implementation-Storage.md part 8.
    ModulePtr<Value> resolveFormat(const ast::Expr& expr);
    // Resolves a condition into a branch. On return, `current` is the block reached when the
    // condition holds - which is where an `is` test's bindings are live - and `onFail` is the
    // block reached when it does not. A caller that already has a block to fail into (a loop's
    // exit) passes it in; one that does not passes null and is given a fresh one.
    //
    // A condition is either an expression whose type has a `Truth` instance or an `is` test, which
    // are the same idea named two ways: `if x` asks whether x matches the pattern its type
    // considers non-empty, and `if x is p` names the pattern instead.
    PatternResult resolveCondition(const ast::Expr& expr, ModulePtr<Block>& onFail);

    // The `Truth` instance of this value's own type, applied. Never reached through a conversion:
    // what `if x` means is decided by x's type alone.
    ModulePtr<Value> truthy(ModulePtr<Value> value, LocationId source);

    // `expr is pat` outside condition position, where there is no branch for its bindings to live
    // in: an ordinary Bool, with what the pattern bound discarded.
    ModulePtr<Value> resolveIs(const ast::Expr& expr, const ast::IsExpr& test, bool used);

    ModulePtr<Value> resolveIf(const ast::Expr& expr, const ast::IfExpr& branch, TypePtr target, bool used, bool implicit = true);
    ModulePtr<Value> resolveMultiIf(const ast::Expr& expr, ast::ParseList<ast::IfCase> cases, TypePtr target, bool used, bool implicit = true);
    void resolveWhile(const ast::WhileExpr& loop);

    // `for pat in f(as): body` - the iterator form. Produces nothing: a loop is a statement in this
    // version, since a value of its own is what a `break` carrying one would decide - see
    // expr_lens.cpp.
    void resolveFor(const ast::Expr& expr, const ast::ForExpr& loop);

    // `for pat in a .. b step s: body` and its `..=`/`downto` spellings - a counted loop over an
    // interval, which shares nothing with the iterator form but the keyword. See expr.cpp.
    void resolveCountedFor(const ast::Expr& expr, const ast::ForExpr& loop);
    ModulePtr<Value> resolveDecl(ast::ParseList<ast::VarDecl> declarations, TypePtr target, bool used);
    void resolveReturn(const ast::Expr& expr);

    // Joins the arms of a branching expression: picks the result type, converts each arm to it
    // in the arm's own block, jumps them all to one join block, and produces the phi. The
    // conversions belong here rather than where each arm was resolved, because the type they
    // convert to is only known once every arm has been seen, and a conversion has to be emitted
    // in the predecessor it flows from rather than after the join.
    ModulePtr<Value> finishBranches(BranchArmList& arms, LocationId source, bool used);

    /*
     * Calls and operators (expr_call.cpp).
     */

    ModulePtr<Value> resolveBinary(const ast::Expr& expr, const ast::InfixExpr& binary, TypePtr target, bool convertResult = true);
    ModulePtr<Value> resolvePrefix(const ast::Expr& expr, const ast::PrefixExpr& prefix, TypePtr target, bool convertResult = true);
    // `operatorSources` runs parallel to `operators`: an operator is a name the program wrote, so
    // the index has to be able to point at it - see emitCall's `nameSource`.
    ModulePtr<Value> resolvePrecedence(SmallArray<const ast::Expr*, 8>& operands, SmallArray<StringId, 8>& operators, SmallArray<LocationId, 8>& operatorSources, Size& operandIndex, Size& operatorIndex, U8 minimumPrecedence, TypePtr target = nullptr);
    ModulePtr<Value> resolveCall(const ast::Expr& expr, const ast::AppExpr& call, TypePtr target, bool convertResult = true);

    // One written argument, and what its absence means if it produces nothing. The two are one
    // question - see ResolvedArg - and the only thing that can separate a `{}` from a failure is
    // whether resolving it reported, which is why every call site that resolves an argument asks it
    // here rather than each keeping its own before-and-after count.
    ResolvedArg resolveArgument(const ast::Expr& expr, TypePtr expected);

    /*
     * Deferred arguments (Design.md's Deferred arguments).
     */

    /*
     * The overload set this name reaches at this arity, and the strictness its candidates agree on.
     *
     * The first step of every written call, and the only lookup any of them does. `source` is where
     * the set is looked up from, which decides what is visible; `nameSource` is where the name was
     * written, and is `kNullLocation` for a synthesized call - see OverloadSet::nameSource.
     */
    void gatherOverloads(StringId name, Size arity, LocationId source, LocationId nameSource,
                         OverloadSet& out, CallShape shape = CallShape());

    // The signature this call's arguments are pushed down against: the sole candidate's, or none
    // where the set holds more than one and pushing either in would decide the call before selection
    // runs. Both call sites ask it - see expr_call.cpp.
    ModulePtr<Function> pushdownSignature(const OverloadSet& set);

    // Runs a deferred argument, here, in the block that is current now. This is the whole of what
    // `@lazy` means; everything else is about getting the argument to the point that calls this.
    ModulePtr<Value> force(const Deferred& deferred, TypePtr expected, LocationId source);

    // The nullary closure a callee that cannot see the argument is handed. Null, after reporting,
    // where the thunk would have to capture something this version cannot - see expr_fun.cpp.
    ModulePtr<Value> makeThunk(const Deferred& deferred, TypePtr type, LocationId source);

    // What a `@lazy` position holding an already-resolved value stands for. A value of the thunk
    // type is one being *forwarded* - a witness entry passing its own parameter on, or one `@lazy`
    // parameter handed to another - so forcing it is calling it; anything else was evaluated before
    // the position was known to be lazy, and forcing it is reading what is already there.
    Deferred deferredValue(ModulePtr<Value> value, TypePtr type);

    // A call whose callee is a value rather than a name - a binding of function type, or any
    // expression at all in callee position. Null when the call is not one of those.
    ModulePtr<Value> resolveIndirectCall(const ast::Expr& expr, const ast::AppExpr& call, TypePtr target);
    /*
     * `nameSource` is where the callee's *name* was written, which is not `source`: `source` is the
     * whole call, and an editor asking about the name under the cursor needs the name.
     *
     * Null for every synthesized call - a pattern's `==`, an array literal's `slice` - and that is
     * what keeps those out of the index: nothing in the source spelled them, so there is no
     * occurrence to record. See resolve/index.h.
     */
    /*
     * `convert`, applied to an argument without forgetting what the position is.
     *
     * Three of the four states have no value to convert, and each is left as it stands: a deferred
     * position is not a value yet, and what it converts to is decided where it is forced; a failure
     * has been reported; and a `{}` carries nothing, so there is nothing to convert. Handing the
     * payload to `convert` directly instead - `push(convert(arg.value, ...))` - reads all three back
     * as a failure, because a null value is what a failure is spelled as.
     *
     * The one case that is a diagnostic rather than a passthrough is a `{}` at a position that
     * wanted something: `convert` answers a null value for it and says nothing, which is how
     * `f({})` for `fn f(x: Int)` became a call with no argument in it at all.
     */
    ResolvedArg convertArgument(const ResolvedArg& arg, TypePtr declared, LocationId source);

    /*
     * The value a *positional* argument list holds at one position.
     *
     * A value carrying nothing is spelled as no value at all, and these lists are paired with the
     * callee's parameters by index - so `f(2, {}, 3)` would otherwise punch a hole that drops
     * argument 3 rather than argument 2. The position gets storage instead, of the zero bytes the
     * type occupies, which is what lowering makes for the erased case anyway: the entry exists and
     * carries its type. `unitValue()` in the same argument arrives as an ordinary value and always
     * did, which is why only the literal ever shifted a list.
     *
     * Every list that pairs by index goes through this - the direct call, the generic call, the
     * erased call and the generic dispatch - because each of them is the same rule and three of them
     * learned it separately, after a crash in the ownership walk that read every argument's type and
     * found a position with none.
     */
    ModulePtr<Value> positionalUnit(ModulePtr<Value> value, TypePtr declared, LocationId source);

    // A callee's declared type, in the caller's terms. `typeArgs` is empty where the signature
    // already is - a plain function, or one this call site specialized - and holds the call's type
    // arguments where it is a generic, class or erased signature read at the types this call
    // decided. One spelling, so that the two cases are one line rather than two code paths.
    TypePtr substituted(TypePtr declared, Buffer<TypePtr> typeArgs, LocationId source) {
        return typeArgs.length ? substituteType(module, declared, typeArgs, source) : declared;
    }

    // Restates an argument list in a callee's signature, read at the types this call decided. The
    // step between selecting a callee and emitting one of the forms a call to it can take.
    void substituteArguments(ModulePtr<Function> callee, Buffer<ResolvedArg> args,
                             Buffer<TypePtr> typeArgs, LocationId source, ArgList& out);

    // Completes every `@lazy` position of `args` against the callee that takes it, and answers
    // whether the callee declares one at all. See the definition in expr_call.cpp.
    bool fillDeferred(ModulePtr<Function> callee, Buffer<ResolvedArg> args, Buffer<TypePtr> typeArgs,
                      LocationId source, ArgList& out);

    // Applies the callee's parameter conventions to an argument list - the one place a call knows
    // both what the callee asked for and what the caller produced. Takes fillDeferred's output.
    void prepareArguments(ModulePtr<Function> callee, Buffer<ResolvedArg> args, Buffer<TypePtr> typeArgs,
                          LocationId source, bool positional, ValueList& out);

    // Matches every class candidate against these arguments and reports what fit, deciding nothing.
    // Shared by the ordinary selection and a `for` loop's - see ClassSelection.
    void matchClassCandidates(const ClassFunList& candidates, Buffer<ResolvedArg> args, TypePtr target,
                              ClassSelection& out);

    // Which candidate of the set serves this call, and the arguments it was chosen for. Reports
    // everything there is to say about a call it cannot serve - see ResolvedCallee.
    void selectCallee(const OverloadSet& set, Buffer<ResolvedArg> args, TypePtr target,
                      LocationId source, ResolvedCallee& out);

    // Selects one callee out of an already-gathered set and emits the call to it. The written form:
    // `resolveCall` and the operators reach it with the set they resolved their arguments against.
    ModulePtr<Value> emitCall(const OverloadSet& set, Buffer<ResolvedArg> args, LocationId source,
                              TypePtr target = nullptr, StringId resultName = 0);

    // The synthesized form, which gathers the set itself. Everything this resolver builds on the
    // author's behalf - a pattern's `==`, an array literal's `slice`, a `for` loop's arithmetic -
    // has a name and a list of values it just produced, and nothing else to say about the callee.
    ModulePtr<Value> emitCall(StringId name, Buffer<ResolvedArg> args, LocationId source, TypePtr target = nullptr, StringId resultName = 0, LocationId nameSource = kNullLocation);
    ModulePtr<Value> emitDirectCall(ModulePtr<Function> callee, Buffer<ResolvedArg> args, LocationId source, TypePtr target = nullptr, StringId resultName = 0);

    // A call to a function the call site has already settled on, generic or not. The one place that
    // fork is stated - see expr_call.cpp.
    // `solved` is the substitution a selection already decided, or empty - see
    // ResolvedCallee::typeArgs. Every other caller reaches a callee without having solved it.
    ModulePtr<Value> emitKnownFunction(ModulePtr<Function> callee, Buffer<ResolvedArg> args,
                                       LocationId source, TypePtr target = nullptr, StringId resultName = 0,
                                       Buffer<TypePtr> solved = {});

    // A call to a generic function: infers its type arguments from the call - or takes the ones
    // `solved` already decided - and then either instantiates it or, when this body is itself
    // generic and the arguments are not concrete yet, defers the whole decision to the
    // instantiation that will make them concrete.
    ModulePtr<Value> emitGenericCall(ModulePtr<Function> callee, Buffer<ResolvedArg> args, LocationId source,
                                     TypePtr target, StringId resultName, Buffer<TypePtr> solved = {});

    // A call that passes its callee a runtime environment instead of being specialized for these
    // types. Null when the environment cannot be built yet, which leaves the call site for the
    // specializing path.
    ModulePtr<Value> emitErasedCall(ModulePtr<Function> callee, Buffer<TypePtr> typeArgs,
                                    Buffer<ResolvedArg> args, LocationId source, StringId resultName);

    // A generic intrinsic, generated for the types this call decided. Shared with generic.cpp,
    // which reaches the same intrinsics through an InstGenCall a specialization made concrete.
    ModulePtr<Value> expandIntrinsic(ModulePtr<Function> callee, Buffer<TypePtr> typeArgs,
                                     Buffer<ResolvedArg> args, LocationId source, StringId resultName);

    // A class function whose instance cannot be chosen here, because the types it would be chosen
    // by are this function's own type variables. Records the requirement and emits InstGenCall.
    ModulePtr<Value> emitGenericDispatch(ClassMatch& match, Buffer<ResolvedArg> args, LocationId source,
                                         StringId resultName);

    /*
     * Whether one class function can serve this call, and if so which instance it selects.
     *
     * The solve is solve.h's - what is here is the arity rule and what a fitting signature with no
     * instance means, which is a different diagnostic from "wrong function" and is why the answer
     * is a bool beside a ClassMatch rather than the match alone.
     */
    bool matchClassFun(const ClassFunRef& reference, Buffer<ResolvedArg> args, TypePtr target, ClassMatch& resolved);

    /*
     * Whether a plain function can serve this call - the same question matchClassFun asks of a
     * class function, so that both halves of an overload set are judged by one rule.
     *
     * `declaredArgs` is how many of the callee's parameters this call is expected to fill, which is
     * all of them everywhere but one place: a `for` loop's iterator declares a continuation the loop
     * supplies rather than the call site, so its written arity is one short and the R5 test over it
     * has to be about the leading positions. Passed rather than defaulted so that the one case that
     * differs says so, and the two ordinary sites read as what they are.
     *
     * `typeArgs` is what the solve decided, for the caller that goes on to commit to this callee -
     * see ResolvedCallee::typeArgs. Left empty for a non-generic callee and for a solve that did not
     * settle every variable, so what comes back is an answer or nothing.
     */
    bool matchFunction(ModulePtr<Function> callee, Buffer<ResolvedArg> args, TypePtr target,
                       LocationId source, Size declaredArgs, TypeList& typeArgs);

    // Calls one implementation of a selected instance. A concrete instance's is an ordinary
    // function; a parametric one's is generic over the instance's own variables, so it is expanded
    // where it is an intrinsic and specialized where it is not. `site` is the module the call was
    // written in, which is what decides the instances its own requirements are proved against.
    ModulePtr<Value> emitInstanceCall(Module& site, ModulePtr<ClassInstance> instance, Buffer<TypePtr> instanceArgs,
                                      U16 index, Buffer<ResolvedArg> args, LocationId source,
                                      TypePtr target = nullptr, StringId resultName = 0);

    ModulePtr<ClassInstance> selectInstance(GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                                            TypeList& instanceArgs);

    /*
     * Function values and closures (expr_fun.cpp).
     */

    // `(a, b) -> expr` and `(a, b): block`. Lifts the body into a function of its own and builds
    // the `{code, env}` value that reaches it - see expr_fun.cpp.
    ModulePtr<Value> resolveFun(const ast::Expr& expr, const ast::FunExpr& fun, TypePtr target);

    // A named function in value position. The value's code word is a thunk that drops the
    // environment every callable is handed, so that a plain function and a closure are one shape.
    ModulePtr<Value> functionValue(ModulePtr<Function> callee, LocationId source);

    // Builds a `{code, env}` value in fresh storage. `env` is null for a function value that
    // captured nothing, which is what makes its teardown a branch that never fires rather than a
    // second representation. What is *in* the environment is said by the closure header in front of
    // `code` rather than by this value - see ClosureHeaderLayout.
    ModulePtr<Value> makeFunValue(TypePtr type, ModulePtr<Function> code, ModulePtr<Value> env,
                                  LocationId source, StringId name);

    // Calling a value of function type: loads the code and the environment out of it and emits
    // InstCallDyn, with each argument taken by the convention the *type* declares.
    ModulePtr<Value> emitDynamicCall(ModulePtr<Value> callable, Buffer<ModulePtr<Value>> args,
                                     LocationId source, StringId resultName);

    // The binding a lambda body named that belongs to an enclosing function, added to this body's
    // capture list the first time it is named. Null when no enclosing body has it either.
    Binding* captureBinding(StringId name, LocationId source);

    /*
     * Lenses (expr_lens.cpp).
     */

    // `yield e` - a call of the continuation parameter a `yield`-form lens did not have to write.
    ModulePtr<Value> resolveYield(const ast::Expr& expr);

    /*
     * The call-site split: `f(as)` and the rest of the block become `f(as, K)`.
     *
     * `block`/`index` say which statement of which sequence the call is, since the continuation is
     * the statements after it and no AST node stands for that span. False when the statement is not
     * a lens call at all - which is every statement but the ones this exists for - and the block
     * loop then resolves it as it always did.
     */
    bool resolveLensStatement(ast::ParseList<ast::Expr> block, Size index, bool used,
                              ModulePtr<Value>& result);

    // What the call site does with the value the continuation produced, which is Analysis-Lens.md
    // §5.1's three shapes and nothing else. Reached by both kinds of lens: a transparent one holds
    // the call's own result, and a skipping one holds what came out of its wrapper.
    ModulePtr<Value> finishLensCall(ModulePtr<Value> value, ContinuationShape& shape, bool used,
                                    LocationId source);

    // The `| else ->` beside a skipping lens call, resolved against what the skip carried - null for
    // a carrier whose skip carries nothing, which is `Maybe`'s. Each arm that does not leave the
    // block contributes to the join with the code the continuation ran.
    void resolveSkipAlternatives(const ast::VarDecl& declaration, ModulePtr<Value> reason, bool used,
                                 BranchArmList& arms);

    // Which `iter fn` a `for` loop names, with the call it was written as. Null after reporting
    // which of phase 1's exclusions this loop's source reached - see expr_lens.cpp.
    ModulePtr<Function> findLoopIterator(const ast::ForExpr& loop, const ast::AppExpr*& call, ArgList& values);

    // The arguments written before the continuation, resolved and settled. Shared by both call
    // sites, which differ in what they do with the rest of the block and in nothing before it.
    //
    // A null `callee` pushes no expected type into any of them, which is what a set with more than
    // one candidate needs: pushing either candidate's parameter types down would decide the call
    // before it was selected. See pushdownSignature.
    void resolveHandedArguments(ModulePtr<Function> callee, ast::ParseList<ast::TupArg> arguments,
                                ArgList& values);

    /*
     * `x?` - Implementation-Semantics.md part 5's early exit.
     *
     * Compiler-known rather than a library suffix operator, because its skip destination is the one
     * destination no call site can name: the enclosing function's return edge, typed by the
     * enclosing signature. The polymorphism is still the library's - `Try` is an ordinary class in
     * Core and a carrier joins by writing an instance - and only where the skip goes is built in.
     */
    ModulePtr<Value> resolveTry(const ast::Expr& expr, TypePtr target, bool used, bool implicit);

    /*
     * `a?.b` - optional chaining, which is a different operator from `?` rather than a spelling of
     * it. `?` leaves the enclosing function; this skips the rest of the chain and produces the
     * carrier's empty case, so `row?.name.trim()` is a `Maybe(String)` and nothing departs.
     *
     * `resolveOptionalChain` owns the whole span: it is entered at the chain's *topmost* node, sets
     * up the join, resolves the chain the ordinary way underneath, and rewraps what came out.
     * `resolveUnwrap` is one `?.` inside that span - a branch, with the skip arm recorded and left
     * for the join to finish, producing the payload for whatever suffix was written after it.
     *
     * Which suffix that is needs no case anywhere here: `a?.b`, `a?.[i]` and `a?.(x)` are a field,
     * a subscript and a call *of an unwrap*, and each is resolved by the code that already resolves
     * those. The whole of `?.`'s extra behaviour is in the one node underneath them.
     */
    ModulePtr<Value> resolveOptionalChain(const ast::Expr& expr, TypePtr target, bool used, bool implicit);
    ModulePtr<Value> resolveUnwrap(const ast::Expr& expr);

    // Whether this expression is a chain node that is part of the chain being resolved, rather than
    // the top of one of its own. See OptionalChain::spine.
    bool onOptionalSpine(const ast::Expr& expr) const {
        return optionalChain && optionalChain->spine.contains([&](const ast::Expr* node) { return node == &expr; });
    }

    // Whether the chain this node tops contains a `?.` anywhere down its spine, which is what
    // decides that a join has to be set up before any of it is resolved.
    bool chainSkips(const ast::Expr& expr);

    // What a `return` in this body leaves: the enclosing function's result type, which for a lifted
    // continuation is the type of the function it was split out of rather than its own.
    TypePtr enclosingResultType() const { return inContinuation ? exitType : function.returnType; }

    // The body a `return` here leaves, which is this one unless it is a lifted continuation - then
    // it is the one the block was split out of, however many frames up that is. `enclosingResultType`
    // is this function's result type, cached at the point the continuation was built.
    const ExprResolver& enclosingBody() const {
        return const_cast<ExprResolver*>(this)->enclosingBody();
    }

    ExprResolver& enclosingBody() {
        auto resolver = this;
        while(resolver->inContinuation && resolver->enclosing) resolver = resolver->enclosing;
        return *resolver;
    }

    // Leaving the enclosing function with a value. An ordinary body returns; a continuation records
    // the departure and has its block finished once the shape of its result is known.
    void emitFunctionReturn(ModulePtr<Value> value, LocationId source);

    // The rest of a block, lifted into the function the lens calls. `declaration` is the `let` the
    // call site wrote, or null for a bare statement; `skipping` says its alternatives belong to the
    // lens's skip rather than to the pattern, so the pattern is bound with nowhere to fail into.
    // `loop` instead makes it a `for` loop's body, in which case `declaration`/`block`/`from` say
    // nothing - see expr_lens.cpp.
    ModulePtr<Value> makeContinuation(Buffer<FunArg> params, const ast::VarDecl* declaration,
                                      ast::ParseList<ast::Expr> block, Size from, LocationId source,
                                      ContinuationShape& shape, bool skipping = false,
                                      const ast::ForExpr* loop = nullptr);

    // `Proceed(value)` or `Exit(value)` of a concrete `Outcome`, built directly rather than through
    // a written constructor: this is emitted code, and there is no source for it to come from.
    ModulePtr<Value> makeOutcome(TypePtr type, bool proceed, ModulePtr<Value> value, LocationId source);

    // Whether an `Outcome` holds its leaving case, as a `Bool`. The one test the exit and step
    // signals cost, in the one shape of each that pays for it.
    ModulePtr<Value> outcomeIsExit(ModulePtr<Value> value, LocationId source);

    // The discriminant test and payload projection the call-site join needs, as a place. Shared by
    // the two arms so that both read the same downcast.
    ModulePtr<Value> outcomePayload(ModulePtr<Value> value, bool proceed, LocationId source);

    /*
     * Storage and aggregates (expr_construct.cpp).
     */

    // Storage for one value. `convention` is what the name that owns the slot may do with it: a
    // temporary and an immutable binding get the default, a `let &` gets Ref, and it is what both
    // assignment and a `&` argument check before writing through.
    // `closureEnv` marks the storage a closure's captures live in: released by the function value
    // that owns it rather than by this frame - see Local::closureEnv.
    ModulePtr<Value> allocate(TypePtr type, LocationId source, StringId name = 0,
                              ast::BindType convention = ast::BindType::Borrow, bool closureEnv = false);

    // Storage for `extent` values of one type, laid out at a stride - Implementation-Containers.md
    // §2's `Run(a)`. The result is the run's local, whose address is what a run holds; nothing ever
    // reads it as a value of `type`, and nothing initializes it, which is what keeps it out of the
    // ownership graph until a container's own traversal puts it there.
    ModulePtr<Value> allocateRun(TypePtr type, ModulePtr<Value> extent, LocationId source);

    /*
     * Builds a `Run(a)` of `count` slots *into* storage the caller already has, and reports in
     * `items` the address its slots start at. `into` is the run's own place - the `Run` downcast is
     * this function's to add, since the shape of a run is what it knows and the caller does not.
     *
     * Shared by Native's `newRun` and by the array literal path, which are the only two places in
     * the compiler that make one. False, after reporting, when `runType` is not a `Run`.
     */
    bool buildRunInto(TypePtr runType, ModulePtr<Value> count, LocationId source,
                      ModulePtr<Value>& items, const Place& into);

    // The same into a fresh temporary, for a caller with nowhere in particular to put it. Null,
    // after reporting, when `runType` is not a `Run`.
    ModulePtr<Value> buildRun(TypePtr runType, ModulePtr<Value> count, LocationId source,
                              ModulePtr<Value>& items);

    /*
     * The plain `Array(a)` descriptor an `@inline(n) @capacity(n)` one is passed as -
     * Implementation-Containers.md §7.2's tier 1.
     *
     * Three constants and a load over the same bytes, so that everything taking an array goes on
     * being compiled once against the plain layout. `mut` queues the count's write-back on the same
     * list a packed field's borrow uses; null where the type carries no refinement.
     */
    ModulePtr<Value> inlineArrayDescriptor(const Place& array, TypePtr refinedType, LocationId source,
                                           bool mut);

    // `base + index`, at `element`'s stride - the same arithmetic Native's `p + n` emits, and it
    // folds to a constant offset wherever the index is one.
    ModulePtr<Value> offsetPointer(ModulePtr<Value> base, TypePtr element, ModulePtr<Value> index,
                                   LocationId source);

    // Where a `[T *n]`'s elements start, as a `%T` - Implementation-Containers.md §6. Computed from
    // the owner's place and never stored, which is Implementation-Storage.md §3's `TrivialSink`
    // trap: a fixed array holding its own base would survive a memcpy pointing at the old one.
    ModulePtr<Value> fixedArrayBase(const Place& array, TypePtr element, LocationId source);

    /*
     * Runs `body(place, index)` at each element of a `[T *n]` whose storage is `array`.
     *
     * Unrolled below kFixedArrayUnrollLimit and a counted loop above it, which is one decision made
     * in one place so that a teardown, a relocation and anything else walking a fixed array agree
     * about the shape. Both hand the body a place rooted in a raw pointer rather than a projection,
     * because there is no per-element projection to hand it - §6's elements are `n` values at a
     * stride, exactly as a run's slots are.
     *
     * The place is a `ProjectionKind::Index` off the array rather than an address the walk computed,
     * and that is load-bearing rather than tidy: a write through a raw pointer has no root, so the
     * ownership passes never see the array initialized and never drop it. An Index stays inside the
     * storage the root names, which is exactly what `rootLocal` asks for.
     *
     * `index` is the same value the place was built from - a constant in the unrolled shape and the
     * loop counter in the other - which is what lets a body walking *two* arrays at once step the
     * second one without knowing which shape it is inside.
     *
     * The loop is worth having at all because `n` is bounded by kMaxFixedArrayLength rather than by
     * anything about the machine: sixty-five thousand unrolled calls is a compiler that appears to
     * hang. The unrolled form is worth having because the case the type exists for is small, and a
     * four-block loop around one drop is code the optimizer then has to prove things about.
     */
    template<class F>
    void eachFixedElement(const Place& array, TypePtr element, U32 length, LocationId source, F&& body);

    Maybe<Place> findPlace(ModulePtr<Value> value);
    Place placeFor(ModulePtr<Value> value, LocationId source);
    bool isWritablePlace(const Place& place);

    // The value passed for a `&` parameter: a mutable borrow of whatever storage the argument
    // named. Null, after reporting, when the argument names none or names storage that may not be
    // written - the two ways a mutable borrow can fail before any liveness question arises.
    // `loaned` is the parameter's `return` marker. A borrow that outlives the call cannot be the
    // temporary Design.md's tier 1 makes, since the write-back happens *at* the call and everything
    // the caller wrote afterwards would be lost - so a packed field in that position is reported
    // here, where the declaration that caused it can be named.
    ModulePtr<Value> borrowArgument(ModulePtr<Value> value, TypePtr expected, LocationId source,
                                    bool loaned = false);

    /*
     * A borrow of a place, with Design.md's tier 1 applied where the place needs it.
     *
     * The one place an `InstBorrow` of a place is created, so that the rewrite cannot be forgotten
     * at one of them: a borrow of a field with no address of its own becomes a fresh local holding
     * the field's value, a borrow of *that*, and - if the borrow is mutable - a write-back queued
     * for whoever consumes it. An immutable one needs the temporary for the same reason and needs
     * no commit, since nothing wrote to it.
     *
     * `loaned` says the borrow is meant to outlive the call, which is what a `return` parameter
     * declares. That is tier 2 and is reported here, where the declaration causing it can be named.
     */
    ModulePtr<Value> borrowPlace(Place place, TypePtr borrowType, LocationId source,
                                 bool loaned = false);

    // Where the pending write-back list currently ends. A call takes one before converting its
    // arguments and hands it back to flushPackedBorrows afterwards, so that a nested call commits
    // its own arguments and not the enclosing call's.
    Size packedMark() { return packedBorrows.size(); }

    // Emits the write-back for every borrow materialized since `mark`. Called immediately after the
    // instruction that consumed the borrows, which is where the loan ends.
    void flushPackedBorrows(Size mark);

    // The value a `->` binding or a `->` argument produces - a move, an independent copy, or the
    // value unchanged, decided by the source's ownership classification. See expr_construct.cpp.
    ModulePtr<Value> sinkValue(ModulePtr<Value> value, LocationId source);

    // The value a `return` hands over, which is the move half of sinkValue and not its copy half.
    // See expr_construct.cpp for why returning has to say so in the IR.
    ModulePtr<Value> returnValue(ModulePtr<Value> value, LocationId source);

    // Storage for a moved value whose relocation is a call rather than its bytes. A no-op for
    // every other value, including a bitwise move - see expr_construct.cpp for which consumers of
    // a move need this and why the rest do not.
    ModulePtr<Value> rootSink(ModulePtr<Value> value, LocationId source);

    Place materialize(ModulePtr<Value> value, LocationId source);

    // One step of a path. Where the step crosses an indirection - a `@box` field, or the automatic
    // one a recursive type gets - the box is followed, so that everything above this is written as
    // though the field were held inline. See the implementation in expr_construct.cpp.
    Place project(Place place, ProjectionKind kind, U16 index, ModulePtr<Value> value = nullptr);

    // The same step, stopping *at* the box rather than following it: the place of the pointer
    // itself. Only the two operations on a box - creating it and releasing it - want this.
    Place projectStorage(Place place, ProjectionKind kind, U16 index, ModulePtr<Value> value = nullptr);
    bool crossesBox(const Place& place, ProjectionKind kind, U16 index);

    // The pointer a path's final Deref followed, where that Deref is one `project` appended for a
    // box. Nothing, for every other path - including one whose last step is a Deref a program wrote.
    Maybe<Place> boxOf(const Place& place);

    // Allocates the target of a boxed edge and stores its address in `pointer`. Emitted by the
    // initialization of a boxed field, which is the only thing that creates one.
    void createBox(Place pointer, TypePtr target, LocationId source);
    TypePtr placeRootType(const Place& place);
    TypePtr placeType(const Place& place);
    ModulePtr<Value> load(Place place, LocationId source, StringId name = 0);
    void buildAggregate(Place place, TypePtr element, Buffer<ModulePtr<Value>> values,
                        TypePtr indexType, LocationId source);
    bool buildFieldAggregate(Place place, TupType& tuple, Buffer<ModulePtr<Value>> values,
                             LocationId source, U16 constructor = maxLimit<U16>,
                             ModulePtr<Value> tag = nullptr);
    bool buildSumAggregate(Place root, TypePtr recordType, U16 constructor, ModulePtr<Value> tag,
                           ModulePtr<Value> payload, LocationId source);
    void initialize(Place place, ModulePtr<Value> value, LocationId source);
    void assign(Place place, ModulePtr<Value> value, LocationId source);
    void write(Place place, ModulePtr<Value> value, LocationId source, Value::Kind kind);
    ModulePtr<Value> addressOf(Place place, LocationId source, StringId name = 0);

    // `[1, 2, 3]`, and `xs[i]` in either a reading or an assigning position. Both build calls into
    // Collections rather than anything the IR knows about - see expr_construct.cpp.
    ModulePtr<Value> resolveArray(const ast::Expr& expr, ast::ParseList<ast::Expr> items, TypePtr target);

    // The same literal where the expected type is a `[T *n]` - Implementation-Containers.md §8.
    // `target` is known to be one; the length is checked against it rather than inferred.
    ModulePtr<Value> resolveFixedArray(const ast::Expr& expr, ast::ParseList<ast::Expr> items,
                                       TypePtr target);
    ModulePtr<Value> resolveSubscript(const ast::Expr& expr, const ast::AppExpr& subscript, bool mutable_);

    ModulePtr<Value> resolveTuple(const ast::Expr& expr, ast::ParseList<ast::TupArg> args, TypePtr target);
    ModulePtr<Value> resolveTupUpdate(const ast::Expr& expr, const ast::TupUpdateExpr& update, TypePtr target);
    ModulePtr<Value> resolveConstruct(const ast::Expr& expr, const ast::ConExpr& construct, TypePtr target);
    TypePtr constructedType(ConstructorRef reference, ast::ParseList<ast::TupArg> args, TypePtr target, ValueList& resolved, LocationId source);
    ModulePtr<Value> resolveField(const ast::Expr& expr, const ast::FieldExpr& field);

    // Whether the cursor is in a field-name position of this construction, and the completion
    // request was answered with its fields - Implementation-Tooling.md §8.1. False in every
    // ordinary compile, where there is no request to answer.
    bool captureConstructionFields(ast::ParseList<ast::TupArg> args, TypePtr owner, TypePtr content);

    // The place of one named field of `place`, following the downcast a single-constructor
    // record needs and the dereference a reference does. Shared by field reads and field
    // assignments so that both reach a field the same way.
    Maybe<Place> projectField(Place place, const ast::Expr& field, LocationId source);
    Maybe<Place> projectField(Place place, StringId field, LocationId fieldSource, LocationId source);

    // Reports a reference kind `.` cannot follow yet - a region pointer or a checked reference,
    // whose dereferences need more than an address. False for anything else, including a raw
    // pointer, which is followed. See expr_construct.cpp.
    bool reportUnfollowedReference(TypePtr type, LocationId source);
    /*
     * The sum a tuple is the payload of, where it is one - see `fillTuple`, whose aggregate is over
     * the *value* rather than over the payload so that the discriminant is one of its components.
     *
     * `owner` is the record's own place and `place` is the payload's, which is `owner` stepped
     * through `constructor`. Both, rather than one derived from the other, because the caller
     * projected the payload already and the step is what carries a box where the constructor has one.
     */
    struct SumOwner {
        SumOwner() {}
        SumOwner(Place owner, U16 constructor, ModulePtr<Value> tag):
            owner(owner), constructor(constructor), tag(tag) {}

        Place owner;
        U16 constructor = maxLimit<U16>;
        ModulePtr<Value> tag = nullptr;
    };

    bool fillTuple(Place place, TupType& tuple, ast::ParseList<ast::TupArg> args,
                   GlobalList<FieldDefault>* defaults, LocationId source, SumOwner sum = {});

    // The default declared for one field of a constructor, or nothing where it has none.
    Maybe<U64> fieldDefault(GlobalList<FieldDefault>* defaults, U16 field);

    /*
     * Patterns (expr_pat.cpp).
     */

    ModulePtr<Value> resolveMatch(const ast::Expr& expr, const ast::MatchExpr& match, TypePtr target, bool used, bool implicit = true);

    // One declaration's pattern, bound to the value its initializer produced, together with the
    // alternatives that cover what the pattern does not. Everything a `let` needs beyond
    // evaluating its initializer, which is resolveDecl's half.
    void resolveBinding(const ast::VarDecl& declaration, ModulePtr<Value> value);

    // One pattern bound to a value it has to cover, with nowhere to fail into. A `for` loop's
    // pattern is the shape of what the iterator hands over rather than a test of it: there is no
    // `| else ->` on a loop, so a pattern that could fail would silently skip an element. A skipping
    // lens's `let` is the other such place - its alternatives are the skip's, not the pattern's - and
    // `reason` is what the diagnostic says about which of the two this is.
    void bindIrrefutable(const ast::Pat& pattern, ModulePtr<Value> value, StringView reason);

    // Emits the tests `pattern` needs and binds the names it introduces. A null `onFail` means
    // the pattern is already known to match every value that can reach it - either because it is
    // irrefutable, or because the alternatives before it ruled everything else out - so no test
    // is emitted and there is no failure edge to take.
    PatternResult resolvePattern(const ast::Pat& pattern, ModulePtr<Value> pivot, ModulePtr<Block> onFail) {
        return resolvePattern(pattern, pivot, onFail, bindings.size());
    }

    // `bindingBase` is where the bindings this one pattern introduces start, which is what makes
    // a name it binds twice tellable from one that merely shadows an outer binding. The recursion
    // passes its own base down; the entry point above takes it from the scope it was called in.
    PatternResult resolvePattern(const ast::Pat& pattern, ModulePtr<Value> pivot, ModulePtr<Block> onFail,
                                 Size bindingBase);

    // A tuple pattern matched against the elements themselves rather than against a tuple holding
    // them - the decomposed pivot, see resolveMatch. `_` is accepted here too, since a pattern that
    // looks at nothing needs no pivot of any shape.
    PatternResult resolveDecomposed(const ast::Pat& pattern, Buffer<ModulePtr<Value>> elements,
                                    ModulePtr<Block> onFail, Size bindingBase);

    PatternResult branchPattern(ModulePtr<Value> condition, ModulePtr<Block> onFail, LocationId source);
    ModulePtr<Value> patternBound(const ast::Pat& pattern, TypePtr target);
    ModulePtr<Value> bindPatternConvention(const ast::Pat& pattern, ModulePtr<Value> pivot);

    Context& context;
    Module& module;
    Function& function;
    ast::ParseBase parse;
    GlobalBase global;
    ModuleBase local;

    // The block instructions are currently appended to, or null once control cannot reach the
    // code that follows (after a `return`, or a branch every arm of which left through one).
    ModulePtr<Block> current;

    // Scratch state for one body, deliberately not in the module arena: it is gone once the
    // function is resolved, and the arena is a bump allocator that never gives anything back.
    SmallArray<Binding, 16> bindings;
    Array<LoopTarget> loops;

    /*
     * Mutable borrows of packed fields awaiting their write-back - Design.md's tier 1.
     *
     * A list rather than one entry because a call can take several, and ordered because the
     * commits have to be: each one reads the containing word as it stands, so two fields of one
     * word merge in sequence rather than racing.
     */
    struct PackedBorrow {
        Place field;
        Place temporary;

        // What the field holds, which is not what the temporary holds when the field is `@bits`
        // refined: the commit narrows back into it, and that narrowing is what keeps the
        // refinement's range - and therefore the niche above it - true.
        TypePtr fieldType;
        LocationId source;
    };

    Array<PackedBorrow> packedBorrows;

    /*
     * The lambda half, all null or empty for an ordinary function body.
     *
     * `enclosing` is what makes a capture possible at all: a name this body does not bind is looked
     * for there, and finding one is the definition of a capture. It is a chain rather than a single
     * link, so a nested lambda naming a binding two frames out captures it through the one in
     * between - which is the same thing happening twice rather than a second mechanism.
     */
    ExprResolver* enclosing = nullptr;

    // The environment parameter - argument zero of a lifted lambda - and the tuple type it points
    // at, which gains a field per capture as the body names them.
    ModulePtr<Value> envArg = nullptr;
    TupType* envType = nullptr;
    Array<Capture> captures;

    // The names the lambda body assigns to, collected from its AST before it is resolved. A capture
    // the body writes has to be a mutable borrow (Design-Memory §8), and which it is has to be
    // decided at the *first* use rather than at the one that happens to be a write.
    Array<StringId> written;

    // Set while resolving a lambda whose result type its body decides, which is what makes an
    // explicit `return` inside one something to report rather than something to convert.
    bool resultInferred = false;

    // Set while resolving the body of a `@lazy` argument's thunk. A `return` there is the exit
    // signal Analysis-Lens.md §5.1 describes - a non-local exit through the callee's live frame -
    // and this version rejects it rather than letting it mean something accidental.
    bool inThunk = false;

    /*
     * The lens halves, all empty for an ordinary body.
     *
     * `yields` is where each `yield` landed, which is what the exactly-once check is stated over;
     * `yieldResult` is the value the continuation produced, which is what a `yield`-form lens
     * returns when it falls off the end of its cleanup.
     */
    Array<LensYield> yields;
    ModulePtr<Value> yieldResult = nullptr;

    // Set while resolving a lifted lens continuation. A `return` there leaves the function this one
    // was split out of, so it is collected rather than emitted - see ContinuationExit.
    bool inContinuation = false;
    TypePtr exitType = nullptr;

    // The optional chain being resolved, or null outside one. Saved and restored around each chain
    // rather than owned by the resolver, so that a chain written inside another one's arguments
    // nests instead of joining it - see OptionalChain.
    OptionalChain* optionalChain = nullptr;
    Array<ContinuationExit> exits;

    // Set while resolving a `for` loop's lifted body, which is a continuation *and* a loop body: the
    // nearest enclosing loop of a `break` written here is the `for` itself, and leaving it is the
    // step signal rather than a jump to a block this function has.
    bool inLoopBody = false;
    Array<ContinuationLoopExit> loopExits;

    // Set when this body contained something the parser could not read. What such a body does not
    // do - return a value, cover every case - is the same mistake seen from the other side, and
    // the parser has already reported it once. See Implementation-Tooling.md §3.2.
    bool sawParseError = false;
};

/*
 * Above this many elements a walk over a `[T *n]` is a loop rather than `n` copies of its body.
 *
 * Four, because that is where the two costs cross for the shape §6 exists for: a `[Point *4]`'s
 * teardown unrolled is four calls, and looped it is four calls plus a counter, a comparison, a
 * multiply and three extra blocks for every pass afterwards to walk. Above it the unrolled form is
 * what gets expensive, and it gets expensive in the compiler rather than in the program.
 */
constexpr U32 kFixedArrayUnrollLimit = 4;

template<class F>
void ExprResolver::eachFixedElement(const Place& array, TypePtr element, U32 length,
                                    LocationId source, F&& body) {
    if(!length) return;

    if(length <= kFixedArrayUnrollLimit) {
        for(U32 i = 0; i < length; i++) {
            auto index = makeInt(source, module.scalar.size, i);
            body(project(array, ProjectionKind::Index, 0, index), index);
        }

        return;
    }

    /*
     * The counted form: `i = 0; while i < n: body(base + i); i = i + 1`.
     *
     * The counter is storage rather than a phi because that is what this stage produces - the
     * resolver emits places and lets lowering promote them, which is exactly what `promoteStackSlots`
     * then does to this one. Written the same way Collections' own element walk is, so that the two
     * loops a container can have optimize identically.
     */
    // `Size` rather than a machine word: this counter is an index, and the two are the same type
    // natively and different host types on JS - see the note at the unrolled form above.
    auto word = module.scalar.size;
    auto counter = allocate(word, source, 0, ast::BindType::Ref);
    auto counterPlace = placeFor(counter, source);
    initialize(counterPlace, makeInt(source, word, 0), source);

    auto test = addBlock();
    auto step = addBlock();
    auto exit = addBlock();

    terminate(emit<InstJmp>(source, 0, module.scalar.unit, test));
    current = test;

    auto index = load(counterPlace, source);
    auto limit = makeInt(source, word, length);
    auto more = ref(emit<InstCmp>(source, 0, module.scalar.bool_, index, limit, CompareOp::Lt));
    terminate(emit<InstJe>(source, 0, module.scalar.unit, more, step, exit));

    current = step;
    body(project(array, ProjectionKind::Index, 0, index), index);

    auto one = makeInt(source, word, 1);
    auto next = ref(emit<InstBinary>(source, 0, word, Value::Add, index, one));
    assign(counterPlace, next, source);

    terminate(emit<InstJmp>(source, 0, module.scalar.unit, test));
    current = exit;
}

// Creates a function that is reached through something other than its own name - a class
// instance's implementation - with a unique name for printing and lowering.
Function* addAnonymousFunction(Module& module, StringId name, LocationId source);

// What storage a place names, and what that storage holds after its projections are followed, are
// both in resolve/place.h - the one walk every consumer of a Place shares. Free functions rather
// than only ExprResolver methods because the drop pass asks the same question of a place it did not
// build, long after the resolver that built it is gone.

/*
 * Whether a place names a field a target may co-pack, and therefore one whose borrow needs
 * Design.md's tier 1 materialize/write-back rather than an address.
 *
 * Asked in resolve and answered from the logical type, so that the rewrite and the diagnostics that
 * go with it are the same on every target. Whether the field is *actually* packed is
 * `compiler/repr`'s answer and may be no; the cost of the difference is a temporary that a
 * declining target did not need, and the cost of getting it the other way round is a miscompile.
 */
bool placeIsPackCandidate(Module& module, Function& function, const Place& place);

// Whether the place names a narrow field of a `@layout(js)` record, which may not be borrowed -
// see the definition for why the pin and the reference cannot both be honoured.
bool placeIsHostPinnedField(Module& module, Function& function, const Place& place);

/*
 * Whether a mutable borrow of this place has to be a temporary rather than a reference.
 *
 * One reason, and it is not that the field has no address: a narrow field is borrowed by a
 * reference that carries its shift (Design.md's tier 2, `NarrowRef` in resolve/lower.cpp), which
 * works wherever the field is. What a reference cannot do is *convert* - so a parameter declared at
 * the unrefined type, which is what makes `increment(&x: Int)` accept `&h.length`, gets the value
 * widened into a temporary and narrowed back at the end of the loan.
 *
 * That narrowing is not an optimization. A `@bits(13)` field whose storage was written a
 * twenty-bit value would falsify the niche above its range, and a `Maybe` folded into that niche
 * would start reading one constructor as another.
 */
bool needsBorrowTemporary(Module& module, Function& function, const Place& place, TypePtr wanted);

// Names one binding per parameter, and storage for the ones that need it. `firstArg` skips the
// leading closure environment of anything reached as a function value - see expr.cpp.
void bindFunctionArgs(ExprResolver& resolver, Module& module, Function& function, Size firstArg);

// Writes one word into each field of a lifted body's environment, in the frame that builds the
// closure. Shared by a lambda and a lens continuation, which differ in what their body is and in
// nothing else - see expr_fun.cpp.
void fillEnvironment(ExprResolver& resolver, ExprResolver& body, Place place, LocationId source);

// The continuation parameter of a lens, read off its last argument. Null for anything that is not
// a lens, which is what a call site checks before trying to split its block.
FunType* lensContinuationType(GlobalBase global, Function& function, ModuleBase local);

// Core's `Outcome(value, exit)`, or null after reporting when Core has no such type.
TypePtr resolveOutcomeType(Module& module, TypePtr value, TypePtr exit, LocationId source);

// Terminates every block a `return` inside a continuation left open, now that the shape of what the
// continuation returns is known. `outcome` is null when the continuation only ever leaves, in which
// case the enclosing function's result travels unwrapped.
void finishContinuationExits(Module& module, ExprResolver& body, TypePtr result, TypePtr outcome,
                             LocationId source);
