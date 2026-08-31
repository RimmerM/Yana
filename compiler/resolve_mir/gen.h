#pragma once

#include "type.h"

namespace mir {

struct Class;
struct Program;

/*
 * Generic contexts - Implementation-Generics.md part 2, and Analysis-IR.md §11.4's milestone 6.
 *
 * Two structures and not one, which is the whole of what this file decides.
 *
 *  - A **`GenContext`** is what a declaration binds while it is being read: the type variables it
 *    introduces, and - as later waypoints add them - the class, property and callable requirements
 *    its signature declared and its body inferred. It grows.
 *  - A **`GenSchema`** is the canonical numbering derived from a *finished* context. It does not
 *    grow, and nothing may read a slot number before it exists.
 *
 * The shipping stage keeps one structure and caches the numbering on it, discarding the cache
 * whenever a requirement arrives. That works only while nothing has already acted on a number: a
 * slot number is what emitted code *loads*, so a body that read slot 3 and then inferred a fourth
 * requirement holds a number that has silently come to mean something else. Splitting them makes
 * that unrepresentable rather than merely unlikely - `freezeSchema` is called once, from one place,
 * and refuses to run twice.
 *
 * What follows from the split is that **a body names a requirement, never a number**. Until the
 * erased ABI exists there is nothing in a body that names either; when it does, the numbering is
 * applied where the environment is built rather than where the body is resolved.
 */

/*
 * What one slot of a generic environment holds - Implementation-Generics.md part 2's inventory.
 *
 * One row today, and the roster is written as a roster anyway: the canonical order below is stated
 * in terms of these groups, and a numbering rule that had a single case would have to be rewritten
 * rather than extended when the second arrives. What is deliberately *not* here is a row for a kind
 * nothing builds - a class slot with no class table under it is a description checked against
 * nothing, which is the mistake this stage is organized against.
 */
enum class GenSlotKind: U8 {
    // A type descriptor: the identity, size, alignment and lifecycle of one type variable.
    Type,

    // A class witness: the members an instance of one class for one type supplies.
    Class,

    Count,
};

StringView genSlotKindName(GenSlotKind kind);

struct GenSlot {
    GenSlotKind kind = GenSlotKind::Type;

    // `Type` slot: the variable it describes. Null on a `Class` slot, whose subject is its whole
    // argument list rather than any one position of it.
    DeclPtr<Type> type = nullptr;

    // `Class` slot: which class, and what it is applied to - one per parameter, in the class's
    // order. Held here rather than as an index into the context's requirement list so that a schema
    // is readable on its own: it is the numbering a *caller* fills in, and a caller that had to
    // reach back through the callee's context to learn what goes in slot 3 would be reading the
    // thing the numbering exists to have already answered.
    DeclPtr<Class> klass = nullptr;
    DeclList<DeclPtr<Type>, false> args;
};

/*
 * One class requirement a declaration states - `fn (Show(a)) present(x: a)`.
 *
 * An argument **list**, one per parameter of the class, because a class is applied rather than
 * attached: `Contiguous(c, a)` says two types stand in a relation, and a structure holding one
 * subject could only have recorded the first of them. The arguments are types of *this* context,
 * which in practice are its variables - a requirement mentioning no variable at all is a fact about
 * the program and is discharged where it is written.
 *
 * A **superclass** is the same structure over the class's own context, which is what makes
 * `class (Eq(a)) Ord(a)` and `class (Contiguous(c, a)) Sliceable(c, a)` one form: the arguments a
 * superclass is applied to are written in the declaring class's variables, so proving `Ord(T)` gives
 * `Eq(T)` by substituting this class's own argument list into them. Holding a bare `DeclPtr<Class>`
 * instead - which is what this was - can only express a superclass over the identical parameter
 * list, and silently reads `class (Eq(b)) Pair(a, b)` as a constraint on `a`.
 */
struct GenRequirement {
    DeclPtr<Class> klass = nullptr;

    // One per parameter of `klass`, in the class's declaration order. Non-embedding, because every
    // reader wants the arguments as a buffer - `classProves` and `matchType` take a pointer and a
    // count - and an embedded list holds them inside whatever holds the requirement.
    DeclList<DeclPtr<Type>, false> args;

    LocationId source = kNullLocation;
};

// The requirement's arguments as a buffer - the shape `classProves` and `matchType` take. The
// storage is the requirement's own and lives in the declaration arena, so the pointer outlives every
// caller here.
inline Contents<DeclPtr<Type>> requirementArgs(DeclBase base, GenRequirement& requirement) {
    return Contents<DeclPtr<Type>> { requirement.args.pointer(base), U32(requirement.args.size()) };
}

/*
 * The canonical numbering of one context - what a caller fills in and a callee reads out.
 *
 * Canonical by *structural key* rather than by the order requirements were discovered in, because a
 * slot number is shared: the caller writes slot 3 and the callee reads slot 3, and nothing at run
 * time would say if the two disagreed about what 3 meant. The order is
 * Implementation-Generics.md part 2's, restricted to the groups that exist:
 *
 *   1. declared type variables, in declaration order;
 *   2. explicit class requirements, in source order.
 *
 * `typeCount` is the length of that first group, and everything after it indexes off it. It is a
 * field rather than a walk because a caller building an environment fills the fixed-width prefix
 * before it reaches anything pointer-shaped, and asking how long the prefix is by scanning would be
 * asking the numbering to describe itself.
 */
struct GenSchema {
    DeclList<GenSlot, false> slots;
    U16 typeCount = 0;
};

struct GenContext {
    explicit GenContext(bool open): open(open) {}

    // The variables this declaration binds, in declaration order. Handles to `VariableType`, held
    // as `Type` so that every reader that wants the logical type does not cast.
    DeclList<DeclPtr<Type>> variables;

    /*
     * `data Boxed(a = I32)` - what an application that omits a parameter gets, and null where it was
     * written without one.
     *
     * Parallel to `variables` and the same length, rather than a field on the variable: a default is
     * a property of the *binding position*, and a `VariableType` is interned as the identity of one
     * parameter of one context - two heads that both bind `a` with different defaults are two
     * positions and one name.
     *
     * Written in this context's own terms, so `data Pair(a, b = a)` is admitted and `fillDefaults`
     * substitutes the arguments already supplied into it. That is why they are resolved after the
     * whole parameter list is read rather than beside each one.
     */
    DeclList<DeclPtr<Type>, false> defaults;

    /*
     * The class requirements this declaration states, in source order.
     *
     * What a *body* may rely on and what a *caller* has to prove, which are the same list read from
     * two ends. A call to a class member inside a generic body is only admitted where the
     * requirement is here; a specialization is only made where the substituted requirement has an
     * instance. Neither is a search: `satisfiesRequirement` walks superclasses and nothing else.
     */
    DeclList<GenRequirement, false> classes;

    // Null until `freezeSchema`. Reading a slot number before that is a compiler error, not a
    // numbering built on demand - see the note at the top of this file.
    DeclPtr<GenSchema> schema = nullptr;

    /*
     * Whether a name the context does not have introduces a variable.
     *
     * True while a function's *signature* is being read and false everywhere else, including in its
     * own body: `fn id(->x: a)` declares `a` by writing it, and a body naming a type nobody declared
     * is a typo rather than a second declaration. `closeContext` is what makes the transition, and
     * it is what stops a mistyped type name inside a body from becoming a variable no caller can
     * ever supply.
     *
     * The one thing a context is asked about itself. What kind of declaration it belongs to was a
     * field beside it and had no reader: a `data` head, a `class` head and a signature differ in
     * *when* they close and in nothing this file does afterwards, so the flag they set is the whole
     * of the difference and a second field naming which of them set it described nothing.
     */
    bool open = false;
};

// Whether this declaration is generic at all. A context with no variables is not one: a `data` head
// with an empty parameter list and a plain `fn` are the same declaration, and giving one an empty
// environment would make every reader below ask two questions where there is one.
inline bool isGeneric(DeclBase base, DeclPtr<GenContext> context) {
    return context && base[context]->variables.size();
}

/*
 * The variable of this context called `name`, introducing it where the context is open.
 *
 * Null when the context is closed and has no such name, which is the caller's cue to report - the
 * position that asked is what knows whether it wanted a type or a parameter, so the message belongs
 * there rather than here.
 *
 * `source` is recorded as the variable's binder when this occurrence is the one that creates it.
 */
DeclPtr<Type> genVariable(Program& program, DeclPtr<GenContext> context, StringId name,
                          LocationId source);

// The same lookup with no creation, for a caller that needs to know whether an occurrence would
// *introduce* a variable rather than name one.
DeclPtr<Type> findGenVariable(DeclBase base, DeclPtr<GenContext> context, StringId name);

/*
 * Whether holding `have(haveArgs)` proves `want(wantArgs)` - directly, or through a superclass.
 *
 * The superclass walk substitutes: `class (Eq(a)) Ord(a)` records `Eq` applied to `Ord`'s own
 * variable, so proving `Ord(T)` gives `Eq(T)` by substituting `[T]` into that argument list. One
 * function because the two questions below are the same question asked of a list of requirements
 * rather than of one, and a second copy of the walk is how a superclass comes to be honoured at a
 * call site and not at an instance head.
 *
 * `Program&` rather than a base, because substituting into `Eq(Boxed(a))` interns a record.
 */
bool classProves(Context& context, Program& program, DeclPtr<Class> have,
                 const DeclPtr<Type>* haveArgs, Size haveCount, DeclPtr<Class> want,
                 const DeclPtr<Type>* wantArgs, Size wantCount);

/*
 * Whether this context already promises `klass` for these arguments.
 *
 * True where the requirement was declared, and true where a *superclass* relation gives it: a
 * declaration that requires `Ord(a)` may call `Eq(a)`'s members, because an instance of `Ord(T)`
 * could not have been declared without one of `Eq(T)`. The walk is over the declared requirements
 * and their superclass closure and is deliberately nothing more - a search that also tried the
 * instance table would answer differently inside a generic body than at the call that specialized
 * it, which is the one thing a generic body may not do.
 */
bool satisfiesRequirement(Context& context, Program& program, DeclPtr<GenContext> gen,
                          DeclPtr<Class> klass, const DeclPtr<Type>* args, Size count);

// Stops the context accepting new variables. Called when a signature has been read, so that the
// body under it names variables and never declares them.
void closeContext(DeclBase base, DeclPtr<GenContext> context);

/*
 * Derives the canonical numbering of a finished context - the one place a slot number comes from.
 *
 * Called from `resolveProgram` after every declaration and every body is in, so that a requirement
 * a body infers is part of the numbering rather than something that shifts one already read. A
 * context frozen twice is a compiler error: the second freeze would be a second numbering, which is
 * exactly the disagreement the split exists to prevent.
 */
void freezeSchemas(Program& program);

/*
 * One substitution - a context's variables replaced by the arguments an instantiation supplies.
 *
 * By value and tiny, because it is passed down every arm of the type walk. `owner` is what makes
 * the replacement safe: only the variables of *that* context are replaced, so a type mentioning an
 * enclosing declaration's variable passes through untouched rather than being renumbered into a
 * position of a different environment.
 */
struct Substitution {
    DeclPtr<GenContext> owner = nullptr;
    DeclPtr<Type>* arguments = nullptr;
    Size count = 0;

    bool isEmpty() const { return count == 0; }
};

/*
 * A logical type with one context's variables replaced - Implementation-Generics.md §1's
 * "substitutes into an already-resolved body".
 *
 * Structural and total: a variable of the substitution's own context becomes its argument, a
 * pointer to one becomes a pointer to the result, a record instantiation has its arguments
 * substituted and is re-interned, and everything else is itself. What it deliberately never does is
 * *decide* anything - no instance is selected, no ownership is reconsidered, no field is re-read -
 * because a substitution that could decide something is one that makes a specialization mean
 * something different from the body it came from.
 *
 * `Program&` rather than a base, because interning a pointer or an instantiation is what makes the
 * answer a canonical handle rather than a second type that happens to look alike.
 */
DeclPtr<Type> substituteType(Context& context, Program& program, DeclPtr<Type> type,
                             const Substitution& with);

// The same for a whole occurrence. The contract and the interior shape are qualification rather than
// structure and pass through unchanged - what a value is borrowed *as* does not depend on what its
// type variable turned out to be.
Occurrence substituteOccurrence(Context& context, Program& program, const Occurrence& occurrence,
                                const Substitution& with);

// Whether this type mentions any variable at all - what a call site asks before it may specialize,
// and what a signature asks to know whether it needs an environment.
bool isConcreteType(DeclBase base, DeclPtr<Type> type);

/*
 * Extends a written argument list with the head's own defaults - `GenContext::defaults`.
 *
 * `Boxed` applied to nothing is `Boxed(I32)` where the head said `a = I32`, and the substitution is
 * over the arguments already in hand, so `Pair(Bool)` at `data Pair(a, b = a)` fills `b` with `Bool`
 * rather than with a variable nothing binds. That is why this is one function rather than a copy of
 * the list: filling left to right and substituting as it goes is the whole of the rule, and a second
 * writer would eventually fill them in the order they were declared instead.
 *
 * Answers false where a position has no argument and no default, which the caller reports - the
 * arity message belongs where the application is, and this is called from two of them.
 */
bool fillDefaults(Context& context, Program& program, DeclPtr<GenContext> owner,
                  SmallArray<DeclPtr<Type>, 4>& arguments);

/*
 * Marks every variable of `owner` that occurs anywhere in `type`.
 *
 * What an instance head asks to know whether each variable it binds appears in a position a call can
 * decide - `instance Contiguous(Array(a), a)` binds `a` where the key is built, and a head that only
 * mentioned it where the dependency *answers* could never be selected. A marker walk rather than
 * `matchType` against itself, which is what this was: matching a pattern to itself succeeds by
 * nominal identity at the first node and binds nothing below it.
 *
 * `into` is indexed by variable and is the caller's to size and to clear.
 */
void markVariables(DeclBase base, DeclPtr<GenContext> owner, DeclPtr<Type> type,
                   SmallArray<bool, 4>& into);

/*
 * One **pattern** matched against one concrete type, binding the pattern's own variables.
 *
 * One-way and structural: a variable of `owner` binds to whatever stands opposite it, a position
 * already bound has to agree, and everything else matches by nominal identity. Two instantiations of
 * one declaration match argument by argument; two of *different* declarations do not match at all
 * however their fields look, which is what nominal identity means and is why the fields are never
 * walked.
 *
 * The one matcher in the stage, and it has three callers that would otherwise each have written it:
 * a call site inferring type arguments from what was passed, an **instance head** selected against
 * the types a call reached it with, and a class member's signature checked against the class's. They
 * are the same operation - `Boxed(a)` against `Boxed(I32)` binds `a` - and three copies of a
 * structural match is how one of them comes to admit a shape the others refuse.
 *
 * `into` is indexed by variable, is sized by the caller to the context's arity, and holds null for
 * a position nothing has decided. Answers whether the two agree; a match that stops part way has
 * bound what it reached, which the recursion needs and no caller does.
 */
bool matchType(DeclBase base, DeclPtr<GenContext> owner, SmallArray<DeclPtr<Type>, 4>& into,
               DeclPtr<Type> pattern, DeclPtr<Type> concrete);

/*
 * `Boxed(I32)` from `Boxed` and one argument list, fully built.
 *
 * Interning, substituting the declaration's fields, folding the instantiation's ownership schema and
 * - once the classification pass has run - classifying it, in that order. One function because the
 * order is a dependency order and a caller that did three of the four would produce a record whose
 * ownership answer is its default rather than its fold.
 *
 * Null where the argument count is wrong, which is reported here: the arity of a type application
 * is a property of the declaration and there is nothing at the use site that could check it later.
 */
DeclPtr<Type> instantiateRecord(Context& context, Program& program, DeclPtr<RecordType> declaration,
                                DeclPtr<Type>* args, Size count, LocationId source);

} // namespace mir
