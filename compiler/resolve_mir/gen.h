#pragma once

#include "type.h"

namespace mir {

struct Class;
struct Function;
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
 * `compiler/resolve` (deleted in J173) kept one structure and cached the numbering on it,
 * discarding the cache whenever a requirement arrived. That works only while nothing has already
 * acted on a number: a slot number is what emitted code *loads*, so a body that read slot 3 and
 * then inferred a fourth requirement holds a number that has silently come to mean something else.
 * Splitting them makes that unrepresentable rather than merely unlikely - `freezeSchema` is called
 * once, from one place, and refuses to run twice.
 *
 * What follows from the split is that **a body names a requirement, never a number**. `GenValue`
 * names a const parameter, `Place::Property` names a field requirement and `NodeCall::genCallable`
 * names a callable one; not one of them holds a slot. Where the answer comes from is a property of
 * the *form* the body is compiled in rather than of the body - `GenForm` in decl.h - so a
 * specialization substitutes and finds it, and the erased form loads it out of the numbering here.
 */

/*
 * What one slot of a generic environment holds - Implementation-Generics.md part 2's inventory.
 *
 * Five rows, and they are the five answers a substitution supplies: a type, a number, an instance, a
 * field offset and a function. The canonical order below is stated in terms of these *groups* rather
 * than of the rows, so a group that ever gains a second kind is one row here and no change to the
 * numbering.
 *
 * What is deliberately *not* here is a row for a kind nothing builds - a slot with nothing under it
 * is a description checked against nothing, which is the mistake this stage is organized against.
 * Every row below is named by something a body can hold: a variable-typed value, a class member
 * call, `GenValue`, `Place::Property`, `NodeCall::genCallable`.
 */
enum class GenSlotKind: U8 {
    /*
     * A type descriptor: the size, the alignment and the teardown of one **type variable** of this
     * context - `freezeSchema` numbers one per declared variable and `variableSlot` finds it by
     * identity.
     *
     * A variable and not a type *expression*, which is the line `erasableType` draws from the other
     * end: a body that mentions `Boxed(a)` is compiled per argument list rather than once, because
     * what it does with a record is project fields out of it and a descriptor carries no table of
     * offsets to project by. Giving it one is a second layout algorithm beside `fieldOffsetNative`,
     * to be kept in step with it forever; giving the *caller* a descriptor for the whole applied
     * type is the same thing said from the outside, since somebody still has to have computed the
     * offsets the body would read.
     *
     * A **field requirement** is what closes most of that gap without a table - `Property` below -
     * because a body that names the one field it wants asks for the one accessor pair it needs
     * instead of a layout it might index anywhere.
     */
    Type,

    /*
     * One const parameter's value - Implementation-Const-Generics.md §3.1's fifth kind.
     *
     * The narrowest entry there is and the only one that is not a pointer, which is why it sits
     * beside the descriptors rather than among the witnesses: the two fixed-width groups stay a
     * prefix, and a caller fills both before it builds anything pointer-shaped.
     */
    Const,

    // A class witness: the members an instance of one class for one argument list supplies.
    Class,

    /*
     * A property witness: how one constrained field of its subject is reached.
     *
     * One cell in one of three shapes - `ssa::PropertyForm` - chosen by the field type the
     * requirement declared and read the same way at both ends: a byte offset, that offset with a bit
     * shift above it where the field does not fill its storage, or a `{read, set}` pair where the
     * body does not know the type at all. What the numbering promises is only that the field is
     * reachable; *how* is the slot's, which is what lets a `Repr` choose one.
     *
     * The requirement also **decides** its own result type - `fillFromProperty`. `a.field: b` states
     * a functional dependency in its syntax: the subject decides and the result is decided, so a
     * signature naming a `b` no argument mentions is one a call can still satisfy.
     */
    Property,

    // A function witness: the one callable a `f: (a) -> b` requirement names.
    Function,

    Count,
};

StringView genSlotKindName(GenSlotKind kind);

struct GenSlot {
    GenSlotKind kind = GenSlotKind::Type;

    /*
     * What this slot is *about*, as a type of the owning context.
     *
     * A `Type` slot's type expression, a `Const` slot's variable, and a `Property` slot's subject.
     * Null on a `Class` slot, whose subject is its whole argument list rather than any one position
     * of it, and on a `Function` slot, whose subject is a name.
     */
    DeclPtr<Type> type = nullptr;

    // `Property` slot: which field of `type`. `Function` slot: which name is called. Unset
    // everywhere else - the two are one field because a slot has at most one name.
    StringId name {};

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
 * `a.field: b` - one structural field requirement.
 *
 * A field is reached by *name* in the source and by *index* in a body, and the two part company
 * exactly here: the body is resolved once, so it cannot know an index that differs per
 * instantiation. What it names instead is this requirement, and where the index comes from depends
 * on which form the body reaches the caller in - a substitution finds it in the subject's fields,
 * an environment loads it out of a property slot.
 *
 * `subject` is a type of this context and in practice its variable; a requirement about a concrete
 * type is a fact about the program, which `readFieldRequirement` reports where it is written.
 */
struct GenProperty {
    DeclPtr<Type> subject = nullptr;
    StringId field {};

    // What the field holds, as the requirement declared it. Checked against the real field when the
    // subject becomes concrete - a requirement that named the wrong type would otherwise be a body
    // typed one way and a field laid out another.
    DeclPtr<Type> result = nullptr;

    LocationId source = kNullLocation;
};

/*
 * `f: (a) -> b` - one constrained callable.
 *
 * The signature is held **decomposed** rather than as a function type, and it stays that way now
 * that there *is* one: what a requirement states is a shape to select a function by, not a value
 * anything holds. A requirement is discharged where the call is written and never becomes a value,
 * so composing it into a `FunType` would be building the one structure whose whole content - a code
 * word and an environment - a requirement has no use for.
 *
 * The paragraph this replaces said the stage had no function type and gave that as the reason.
 * G7 added the row, for the reason it named: a closure is a value anything holds. That changed
 * nothing here, which is the useful half of the answer.
 *
 * Discharged by **name and arity** at the point of use, exactly as an ordinary call selects an
 * overload, and then checked position by position against the substituted signature. So a callable
 * requirement is a class of one member whose dispatch key is a name.
 */
struct GenCallable {
    StringId name {};

    DeclList<DeclPtr<Type>, false> args;
    DeclPtr<Type> result = nullptr;

    LocationId source = kNullLocation;
};

/*
 * The canonical numbering of one context - what a caller fills in and a callee reads out.
 *
 * Canonical by *structural key* rather than by the order requirements were discovered in, because a
 * slot number is shared: the caller writes slot 3 and the callee reads slot 3, and nothing at run
 * time would say if the two disagreed about what 3 meant. The order is
 * Implementation-Generics.md part 2's, restricted to the groups that exist:
 *
 *   1. declared type variables, in declaration order;
 *   2. declared const parameters, in the same order;
 *   3. explicit class requirements, in source order;
 *   4. structural field requirements, in source order;
 *   5. callable requirements, in source order.
 *
 * The two fixed-width groups lead so that a caller fills a prefix of machine words before it reaches
 * anything pointer-shaped, and the parameter list is walked twice rather than filtered once: which
 * group a parameter is in is the numbering's question, not the parameter's.
 *
 * `typeCount` is the length of that first group, and everything after it indexes off it. It is a
 * field rather than a walk because a caller building an environment fills the fixed-width prefix
 * before it reaches anything pointer-shaped, and asking how long the prefix is by scanning would be
 * asking the numbering to describe itself.
 */
struct GenSchema {
    DeclList<GenSlot, false> slots;
    U16 typeCount = 0;

    // The length of the second fixed-width group, which starts at `typeCount`. Everything
    // pointer-shaped starts at `typeCount + constCount`, and both numbers are fields for the reason
    // the first one is: a caller fills the environment in group order, and asking how long a group
    // is by scanning would be asking the numbering to describe itself.
    U16 constCount = 0;

    /*
     * Where each of the three witness groups starts.
     *
     * The numbering states its own groups, so that "which slot answers class requirement 2" is a
     * field plus an index rather than a sum of two counts and a context's list lengths worked out at
     * each of the several places that ask. Every one of those sums is the same arithmetic, and the
     * one that eventually differs would be a caller writing a slot the callee does not read.
     */
    U16 classStart = 0;
    U16 propertyStart = 0;
    U16 functionStart = 0;
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
     * whole parameter list is read rather than beside each one. (J134 refused it for a while; J144
     * made declarations.md say this instead, and refuses only a default naming its own parameter or
     * a later one - `Resolver::declareDefaults`.)
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

    /*
     * The structural field and callable requirements this declaration states, in source order.
     *
     * Two lists rather than one tagged list, because nothing ever walks them together: a field
     * expression asks the first, a call asks the second, and the numbering asks each once in its
     * own group. A tagged list would have every reader filter, which is a filter that eventually
     * disagrees with the numbering about which group a row is in.
     */
    DeclList<GenProperty, false> properties;
    DeclList<GenCallable, false> callables;

    /*
     * A bitmask over `variables`: the ones the head mentions nowhere and a **functional dependency**
     * among the requirements above decides.
     *
     * `instance (Magnitude(a, w), Show(w)) ShowWith(a)` is the shape - `w` is in no head position,
     * so nothing a call writes binds it, and `Magnitude(a -> w)` is the promise that one `a` has one
     * `w`. Filled by `declareInstance` for an instance and zero everywhere else, because it answers
     * a question only a head asks: which of my variables may a match leave open for the context to
     * settle.
     *
     * A mask rather than a flag on the variable, for the reason the numbering is a schema: it is a
     * property of this context as a whole - the closure over *its* requirement list - and a variable
     * is interned as one parameter of one context, so two contexts binding the same name are two
     * positions and one type.
     */
    U64 deduced = 0;

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

    /*
     * Whether this head's default list was **abandoned** at a cycle - `data P(a, b = P(a))`
     * (`Resolver::demandDefaults`). The cycle is reported where it is written; every application
     * that leaves out the position the lost default was for is that same mistake, and saying "P
     * takes 2 type arguments and this names 1" at each (and at an alias over it) only restated it
     * (J162). So the arity sentence is not said for such a head.
     */
    bool defaultsAbandoned = false;
};

/*
 * A declaration read as the **type of a value of it** - `fn add(a: I32, b: I32) -> I32` as
 * `(I32, I32) -> I32`.
 *
 * One function, and every position that needs the pair goes through it: the node that makes a
 * function value, the check that a value fills a declared position, and the thunk the lowering
 * builds. A signature read as a type at two sites is two readings, and what they eventually disagree
 * about is a convention - which is the one part of a function type nothing at a use site can see.
 *
 * Null where the declaration has no type a value could have. A generic function is the case that
 * matters: `fn (a) id(->x: a) -> a` is a family, and which member of it a value would be is a
 * question the value has nowhere to carry the answer to.
 */
DeclPtr<Type> funTypeOf(Context& context, Program& program, DeclPtr<Function> function);

// Whether this declaration is generic at all. A context with no variables is not one: a `data` head
// with an empty parameter list and a plain `fn` are the same declaration, and giving one an empty
// environment would make every reader below ask two questions where there is one.
inline bool isGeneric(DeclBase base, DeclPtr<GenContext> context) {
    return context && base[context]->variables.size();
}

/*
 * The parameter one argument of an application fills, or null where the head has no such position.
 *
 * Null rather than a report, because the arity of an application is checked once by whoever applies
 * the head - a second count here would be the same mistake reported twice, in the wrong order. Two
 * callers ask it for two reasons: a written number takes its *type* from the parameter it fills, and
 * a qualified argument names the positions the parameter stands at.
 */
inline DeclPtr<Type> headParameterOf(DeclBase base, DeclPtr<GenContext> head, Size position) {
    if(!head || position >= base[head]->variables.size()) return nullptr;
    return base[head]->variables.get(base, position);
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
                          LocationId source, DeclPtr<Type> constType = nullptr);

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
/*
 * How many function slots one class's witness table has - its own members, then its superclasses'.
 *
 * A witness table is **flattened**: `class (Eq(a)) Ord(a)` builds one table holding `Ord`'s members
 * followed by `Eq`'s, so reaching a superclass's member out of a subclass's witness is an *offset*
 * rather than a second load. The alternative - a pointer per superclass, which is what
 * `compiler/resolve` wrote - buys a shared grandparent's table being one table and costs a load per
 * level at every dispatch; a class hierarchy here is two or three deep and its tables are three or
 * four cells, so the duplication is smaller than the indirection.
 *
 * Both ends read this: `environmentFor` fills a witness in this order and `witnessFor` writes the
 * table in it, which is the same contract `freezeSchema`'s slot numbering has.
 */
U16 classWitnessCount(DeclBase base, DeclPtr<Class> klass);

// How many of one class's own members are written `iter fn`, which is the size of its object-maker
// region - Design-Iterator-Objects.md §6.4.
U16 classObjectCount(DeclBase base, DeclPtr<Class> klass);

// Where in that region one member's entry is - how many members written `iter fn` come before it.
U16 classObjectIndex(DeclBase base, DeclPtr<Class> klass, U16 member);

/*
 * Which class and which of its members flat index `at` names, in the order above.
 *
 * The reader's half of the layout: the lowering has a flat index and needs the *declared* signature
 * behind it to build a thunk, and only the walk knows which class that index fell into.
 *
 * `object` says the slot is that member's **"make an object" entry** rather than the member itself -
 * §6.4. The member index is the declared one either way, so a reader that has no use for objects
 * reads the pair it always read; what differs is which signature the thunk is built against, and
 * `Function::iterObject` is where that one is.
 */
bool classWitnessAt(DeclBase base, DeclPtr<Class> klass, U16 at, DeclPtr<Class>& into, U16& member,
                    bool& object);

/*
 * `classProves` with the **offset** the proof lands at - where `want`'s members start in `have`'s
 * flattened table.
 *
 * One walk and not two, because the offset is a property of the path the proof took: two
 * superclasses that are the same class at different arguments are two ranges of the table, and only
 * the walk that matched the arguments knows which one it matched.
 */
bool classWitnessPath(Context& context, Program& program, DeclPtr<Class> have,
                      const DeclPtr<Type>* haveArgs, Size haveCount, DeclPtr<Class> want,
                      const DeclPtr<Type>* wantArgs, Size wantCount, U16& offset);

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
void freezeSchemas(Context& context, Program& program);

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
 * The contract a variable's position has once the variable is bound - `AccessForm`'s note on
 * `WholeValue`.
 *
 * A shared or unique `c` bound to a type with a view for that access becomes the view: a body
 * compiled once has one ABI for the position, and the view is the one form every argument of the
 * type can be adapted to. Nothing else changes, and nothing changes for a position that wrote the
 * type by name - `declared` is the variable, or this answers what it was given.
 */
Contract substitutedContract(DeclBase base, Program& program, DeclPtr<Type> declared,
                             DeclPtr<Type> substituted, Contract contract);

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

/*
 * The erased `iter(T)` for one protocol, interned - Design-Iterator-Objects.md §3.4.
 *
 * `record` false mints the type and records nothing, which is what a **signature** written over a
 * class's own variables wants: `chunks$obj(self: c) -> iter('[a]')` is a shape for a thunk to be
 * built against and not a protocol any value has, so the `Iterate` instance that would step one
 * would be a blanket instance over a class whose whole job is to say which step to call.
 */
DeclPtr<Type> erasedIterTypeOf(Context& context, Program& program, const Occurrence& handover,
                               DeclPtr<Type> resume, LocationId source, bool record);

// The same for a whole occurrence. The contract and the interior shape are qualification rather than
// structure and pass through unchanged - what a value is borrowed *as* does not depend on what its
// type variable turned out to be.
Occurrence substituteOccurrence(Context& context, Program& program, const Occurrence& occurrence,
                                const Substitution& with);

/*
 * `'%T` read as `'%'T` - a published loan of a pointer whose pointee names no origin has its pointee
 * in the loan's own slot (J164). The occurrence unchanged where it is not one: an owned or call-bounded
 * root, a type that is not a pointer, or a shape that already says something.
 *
 * Applied where an occurrence is *written* (`resolveOccurrence`) and nowhere else. A substitution
 * does not apply it: `source'v` at `v := %Int` is `Index(Array(a)).get`'s result, and a pointer copied
 * out of that would keep the container borrowed while holding nothing of what the container's
 * elements point into, since a container does not carry a pointer element's pointee (J164 §3).
 */
Occurrence impliedPointeeOrigin(Program& program, const Occurrence& occurrence);

// Whether this type mentions any variable at all - what a call site asks before it may specialize,
// and what a signature asks to know whether it needs an environment.
bool isConcreteType(DeclBase base, DeclPtr<Type> type);

// Whether this type mentions a variable of *this* context - see the definition, which is where the
// difference from `isConcreteType` is argued.
bool mentionsContext(DeclBase base, DeclPtr<Type> type, DeclPtr<GenContext> context);

/*
 * Whether a value can be made for a position that is *still open* - Analysis-IR.md §11.4's G7c.
 *
 * A hint is an input to inference, so a position that has not been decided supplies none, and that
 * is the rule everywhere but here. The exception is a **function** position, and it is an exception
 * because the value it receives is what decides the open part: `fn (r) run(body: (I32) -> r) -> r`
 * has nothing but `body` to say what `r` is, so refusing to hand the position over until `r` is
 * known is refusing to ever know it.
 *
 * What has to be closed is the **parameters**, and what may be open is the **result**. That is not a
 * convenience: a lambda writes no parameter types at all and takes them from the position, so an
 * open parameter is a value nothing could make; a result is what a body computes, and taking it back
 * off the value is what `TypeArguments::bind` already does with every other argument.
 *
 * A function type nested in a parameter is therefore refused with everything else, since `isConcrete`
 * on the parameter is what is asked. One level, deliberately - `((r) -> I32) -> I32` would need the
 * value's own parameter to be decided by a value it has not been handed.
 */
bool isOpenFunPosition(DeclBase base, DeclPtr<Type> type);

/*
 * The position's shape with what the value decided written into it - `isOpenFunPosition`.
 *
 * The two consumers of an open position both need this and need it to be one function: a named
 * function and a lambda answer the open result differently - one reads it off a declaration and the
 * other off a body - and what each has to hand back is *the position, closed*. A value typed by its
 * own result instead would drop whatever the position said about the result's convention, which is
 * part of the type and not a decoration on it.
 *
 * Identity stays the comparison afterwards. What is filled in here is exactly the result the
 * position left open, so a shape that differs anywhere else is still two types and still reports.
 */
DeclPtr<Type> closeFunPosition(Context& context, Program& program, DeclPtr<Type> position,
                               DeclPtr<Type> result);

/*
 * What this argument counts *in*, and null where it is a type rather than a count.
 *
 * The one place the two spellings of a count are read as one: a written `4` carries its width and a
 * const parameter passed along carries its annotation, and every position that compares two counts
 * has to see through both. A generic body calling a generic function supplies the second, which is
 * exactly the case a predicate over `Type::Const` alone gets wrong.
 */
DeclPtr<Type> countType(DeclBase base, DeclPtr<Type> type);

// Whether this argument stands for a number at all. The predicate the kind checks share, so that
// "a count" means one thing.
bool isCountType(DeclBase base, DeclPtr<Type> type);

/*
 * Every argument of one written application against the parameter it fills - see the definition.
 * Reports each position that disagrees and answers whether they all agreed.
 */
bool checkArgumentKinds(Context& context, DeclBase base, DeclPtr<GenContext> owner, StringId name,
                        const DeclPtr<Type>* args, Size count, LocationId source);

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
 * `decided`, closed over the context's own class requirements - one rule, to a fixpoint.
 *
 * A requirement whose **deciding** arguments are all decided decides every variable in its
 * **determined** ones, which is what a functional dependency is: `Magnitude(a -> w)` at a known `a`
 * answers `w`. So a variable an instance head never mentions can still be settled, provided
 * something the head does bind leads to it.
 *
 * A fixpoint rather than one pass, because the requirements are in source order and the chain is
 * not: `(Show(w), Magnitude(a, w))` decides `w` at the second requirement and only then knows what
 * the first is about. Terminates because a round either marks a variable or stops.
 *
 * `decided` is the caller's to size and to seed, and this only ever adds to it. The seed is what
 * something *outside* the context supplies - for an instance, the head's deciding positions.
 */
void closeDecidedVariables(DeclBase base, DeclPtr<GenContext> gen, SmallArray<bool, 4>& decided);

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
 *
 * `types` is here because a match can *create* a type, which is the one thing this function does
 * beyond reading two: a quantified array count binds to a `ConstType` of the variable's own declared
 * type, and that number has to be interned like any other so `sameType` stays a comparison when the
 * same position is matched twice.
 */
bool matchType(DeclBase base, Types& types, DeclPtr<GenContext> owner,
               SmallArray<DeclPtr<Type>, 4>& into, DeclPtr<Type> pattern, DeclPtr<Type> concrete);

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

/*
 * `{I32, Bool}` from one field list, fully built.
 *
 * `instantiateRecord`'s sibling, doing the same four things in the same order for the kind of record
 * that has no declaration: intern, fill, fold the schema and - once the classification pass has run
 * - classify. Written beside it rather than merged with it because the two share none of the
 * middle: an instantiation substitutes a declaration's fields and copies its constructors, and this
 * one has the fields already and makes the single constructor that a product with no name has.
 *
 * The fields' occurrences are the caller's, and every one of them is an owner: a tuple position with
 * a contract of its own would be a `RecordField` the source has no way to spell, since what a
 * position holds is written on the declaration a tuple has not got.
 */
DeclPtr<Type> tupleType(Context& context, Program& program, const RecordField* fields, Size count,
                        LocationId source);

/*
 * The instantiations `instantiateRecord` could not fill because the declaration they came from had
 * not been read yet - see the note there, where the source order that produces one is argued.
 *
 * Run once, immediately after every `data`'s fields are in. Anything that reads a record's fields or
 * its constructors before this has run is reading a record that may still be empty, which is why the
 * pass order in `resolveProgram` puts it where it is rather than leaving it to whoever notices.
 */
void completeInstantiations(Context& context, Program& program);


} // namespace mir
