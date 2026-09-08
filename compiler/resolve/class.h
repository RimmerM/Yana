#pragma once

#include "inst.h"

struct Module;
struct Function;

/*
 * Typeclasses.
 *
 * A class is a set of function signatures written against the type variables of its own generic
 * context; an instance supplies one implementation per signature for one concrete assignment of
 * those variables. Calling a class function is therefore two steps - work out what the class
 * variables must be at this call site, then find the instance for them - and both steps are
 * driven by matchType() against the signature, which is why a variable that only appears in the
 * return type is inferable exactly like one that appears in an argument.
 */

struct ClassFun {
    StringId name {};

    // The signature, as a body-less Function: its args carry the declared types and names, and
    // its returnType the declared result. Types in it belong to the class's generic context.
    ModulePtr<Function> fun = nullptr;
    U16 index = 0;

    // Recorded at declaration time so that two functions of one class can share a name when
    // their arities differ - a class declares both the binary and the unary `-`.
    U16 arity = 0;

    // The body the class wrote for this signature, if it wrote one: a generic function over the
    // class's own type variables that carries the class itself as a requirement, so `!=` written
    // as `!(lhs == rhs)` is exactly `fn (Eq(a)) !=(lhs: a, rhs: a) -> Bool`. An instance that
    // supplies no implementation gets this one, specialized for its own types.
    ModulePtr<Function> defaultFun = nullptr;

    // How far this function is from one an instance must write. A signature with no default has
    // rank 0; a default has one more than the highest-ranked class function it calls, which makes
    // a set of defaults that call each other in a circle a rejected declaration rather than a
    // program that compiles and hangs. See checkDefaultRanks().
    U16 rank = 0;
};

struct TypeClass {
    TypeClass(StringId name, GlobalPtr<GenEnv> gen): name(name), gen(gen) {}

    StringId name;
    GlobalPtr<GenEnv> gen;
    GlobalList<ClassFun> functions;
    Module* module = nullptr;
    ast::ParsePtr<ast::Decl> ast = nullptr;
    LocationId source = kNullLocation;
    bool ready = false;

    // Whether the declaration wrote `pub`. A class's member functions are covered by it: they are
    // the interface it declares, and a class nobody may name has no interface to offer.
    bool exported = false;

    // What `default Class = Type` declared, if anything. It is the type a value of this class
    // takes when nothing in the program picked one, which today is what settles a literal whose
    // type nothing decided. Declared in the class's own module so that a default is as coherent
    // as the class itself.
    TypePtr defaultType = nullptr;
    LocationId defaultSource = kNullLocation;

    /*
     * The functional dependency `class Contiguous(c -> a)` declared: the index of the first
     * parameter the earlier ones determine, and 0 for a class that declared none.
     *
     * The resolver binds type variables one-way and positionally, so a parameter appearing only in
     * a member's *result* - `fn elements(return self: c) -> [a]` - is bound by nothing a caller
     * writes and every use of the class fails to infer it. What this records is the author's
     * promise that it need not be inferred: `c` decides `a`, so a call that bound `c` may ask the
     * instance table with a hole in `a`'s position and read back what the head bound.
     *
     * The promise is checked where instances are declared (see checkDependency), and that check is
     * what makes reading the hole back meaningful rather than a race between two instances that
     * disagree. It is deliberately not inferred from the member signatures: `Widen(a, b)` puts `b`
     * in no argument position and is a relation on purpose - Core declares Widen(Int, Long),
     * Widen(Int, Float) and Widen(Int, Double) - while Try's `e` appears in an argument of
     * `fromExit` and is determined all the same.
     */
    U16 determined = 0;

    bool determines() const { return determined != 0; }
};

struct ClassInstance {
    explicit ClassInstance(GlobalPtr<TypeClass> typeClass): typeClass(typeClass) {}

    GlobalPtr<TypeClass> typeClass;

    // One type per class type variable, and one implementation per class function.
    ModuleList<TypePtr, false> forTypes;
    ModuleList<ModulePtr<Function>, false> functions;

    // Set when the head is written over type variables - `instance Ord(Ptr(a))`. The context holds
    // those variables and the requirements the head itself declares, so selecting this instance is
    // matching its types rather than comparing them, plus a proof of its own constraints for what
    // the match bound. Each implementation is then a generic function over the same context,
    // specialized for the types one selection decided.
    GlobalPtr<GenEnv> gen = nullptr;

    Module* module = nullptr;
    LocationId source = kNullLocation;

    /*
     * The head positions at which a **window** may stand for the container it is over - one bit per
     * position, computed once by `windowStandIns` when the instance is declared.
     *
     * A window is a shared borrow of its container, so it can be given to a class that only *reads*
     * the container and to no other. That is the whole rule, and the bit is set exactly when this
     * instance satisfies it at that position: every occurrence of the class variable is a
     * `Borrow`-convention parameter whose type is the container itself, this instance's member takes
     * the *window* there (which is what writing `[a]` in the member asked for - see
     * `module_class.cpp`), and no result or hand-over mentions the variable at all.
     *
     * `Eq([a])`, `Show([a])`, `Chunked([a], a)` and `ShowWith(s, [U8])` qualify. `Copy` and `Append`
     * do not, because their result is the class variable and a window is not something either can
     * produce; `Reclaim` and `Drop` do not, because they consume - which is what stops a window from
     * acquiring a teardown it has nothing to run.
     *
     * Eight positions, because a class head with more than eight parameters does not exist and a
     * wider word here would be carried by every instance in the program.
     */
    U8 windowArgs = 0;

    /*
     * Whether the library is allowed to declare this instance itself - the bridge to `lib/` writing
     * its own container instances, and nothing wider.
     *
     * `Written` is every instance in a program's source and every one the compiler generates for a
     * primitive width: a second declaration at the same head is the duplicate it always was.
     * `Replaceable` is the container set alone - `defineContainerInstances` and
     * `defineNativeIndexInstances` - which `lib/` is in the process of writing for itself. A source
     * instance at one of those heads *supersedes* the generated one rather than colliding with it,
     * so the two stages read one library while only one of them still generates anything, and the
     * generated one becomes `Superseded` and is skipped by every lookup.
     *
     * `Overriding` is the same bridge run **backwards**, and it exists for exactly one head set -
     * `Index`, `defineIndex`'s three. The library writes that instance too, because the ownership
     * stage generates nothing and has to read it; this stage may not use it, because the two stages
     * disagree about what a written `getMut` *means*. A generated one hands back a borrow whose
     * place is the element - `atPointer(items + i)` - and a written one hands back a borrow the
     * signature roots in `self`, so `xs[k] = xs[k] + 1`, a subscript in a lens argument and the
     * erased build's return rooting are all refused here and all compile there. So a source
     * instance at one of those heads is *retired where it is declared* rather than superseding the
     * generated one, and nothing of it - no member, no body, no export - is ever built. See D14 in
     * `doc/progress/2026-09-06-inventory.md` and the note in `lib/Core/Array.yana`.
     *
     * The alternative was to make the generators consult the source declarations before running,
     * which they cannot: `definePreludeContainers` runs between `passFunctionSignatures` and
     * `passInstances`, so at that point no instance the library wrote exists yet. Deciding it where
     * the collision is *found* is the same decision made in the one place that already knows both,
     * and it is what lets the two directions be one test.
     *
     * This whole field goes with the generators - `doc/inflight/Analysis-IR.md` part 11.5.
     */
    enum class Origin: U8 { Written, Replaceable, Overriding, Superseded };
    Origin origin = Origin::Written;
};

// Where a class function was found: the class, and which of its signatures.
struct ClassFunRef {
    GlobalPtr<TypeClass> typeClass = nullptr;
    StringId name {};
    U16 index = 0;
};
