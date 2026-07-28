#pragma once

#include "module.h"

/*
 * Generic functions: one body, many instantiations.
 *
 * A generic function is resolved exactly once, against its own type variables. Everything the
 * body decides - which class function an operator is, which constructor a name is, where a value
 * lives, how an aggregate is projected - is decided there and then, and an instantiation only
 * substitutes types into the decisions already made. That is Implementation-Generics.md's first
 * invariant, and the reason a specialization is a clone rather than a second resolution: if the
 * AST were resolved again per call site, `a` substituted by `Int` would silently make different
 * choices than `a` substituted by `Maybe(Int)`, and a generic function would not have one meaning.
 *
 * What the body cannot decide is which *instance* satisfies a class it uses, because that needs
 * the concrete type. Those calls become InstGenCall and are recorded as requirements on the
 * function's context - declared ones are checked, undeclared ones are collected. An instantiation
 * proves every requirement before it clones anything, so a missing instance is reported once,
 * against the requirement, rather than as a pile of errors inside a body the user did not write
 * in that form.
 *
 * This milestone specializes every generic call: there is no erased ABI, no runtime GenEnv and no
 * witness passing, so a call whose type arguments are not concrete at some point in the call
 * chain is rejected rather than lowered.
 */

// The generic context of a function, or null when it is not generic.
GenEnv* functionGen(GlobalBase global, const Function& function);

// Records that `function` needs `typeClass` for these types, adding the requirement to its
// context when the signature did not declare it. Body-inferred requirements are allowed because
// a body-bearing function's context is derived from it rather than maintained separately.
void requireClass(Module& module, Function& function, GlobalPtr<TypeClass> typeClass,
                  Buffer<TypePtr> args, LocationId source);

// Whether `function` already declares (or has already inferred) this exact requirement.
bool hasClassRequirement(GlobalBase global, const GenEnv& env, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args);

// Whether the requirements in scope prove `typeClass(args)` - directly, or because one of them is
// a class that declares it as a superclass. This is what makes `fn (Num(a)) inc(x: a) = x + 1`
// compile as written: `Num(a)` is what the author declared, and `FromInt(a)` is what the literal
// needs, and `class (FromInt(a)) Num(a)` is what connects them.
bool provesClass(Module& module, const GenEnv& env, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args);

/*
 * Where a body finds the witness for one class requirement, as emitted code reaches it: the
 * environment slot holding a witness, and the superclasses to step through from there.
 *
 * The steps are what keeps `fn (Num(a)) inc(x: a) = x + 1` to one witness slot. The literal
 * dispatches `FromInt(a)`, which the author never declared because `Num` already guarantees it, and
 * a slot of its own would be the caller passing the same class witness twice under two numbers. A
 * `ClassWitness` names its superclasses' witnesses, so what the body loads instead is the `Num`
 * witness it was given and then the `FromInt` pointer inside it - see ClassWitnessLayout.
 *
 * `supers` is filled with that path as one byte offset into a witness per step - the class at each
 * step decides where its superclass pointers sit, and only this walk knows what those classes are.
 * Empty for a slot that holds the wanted witness directly.
 * maxLimit<U16> when the context proves the requirement nowhere, which is what a caller that has to
 * specialize instead is told.
 */
U16 genWitnessPath(Module& module, GenEnv& env, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                   Array<U32>& supers);

/*
 * Instantiates `generic` for one set of fully concrete type arguments, cloning its resolved body
 * and substituting. The result is interned per argument list, so a function called at the same
 * types from ten places is specialized once.
 *
 * `from` is the module the call is written in: it decides which instances are visible for the
 * requirements, and it is where the diagnostics are reported. The specialization itself belongs
 * to the generic function's own module, so that two callers share one copy.
 */
ModulePtr<Function> instantiateFunction(Module& from, ModulePtr<Function> generic, Buffer<TypePtr> args,
                                        LocationId source);
