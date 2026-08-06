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
    StringId name = 0;

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
};

// Where a class function was found: the class, and which of its signatures.
struct ClassFunRef {
    GlobalPtr<TypeClass> typeClass = nullptr;
    StringId name = 0;
    U16 index = 0;
};
