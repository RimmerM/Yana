#pragma once

#include "module.h"

/*
 * The vector library - Design-Vector §3, Implementation-Vector.md §9.
 *
 * Beside native.cpp rather than inside core.cpp, and for the same reason Native's own intrinsics are
 * their own file: what is here is one family of operations that the language cannot write in itself,
 * and the *declarations* it belongs to are ordinary Core source. Core owns the type constructors and
 * the class declarations; this owns what a call to one of them generates.
 *
 * Two halves, and the second is the one that is not an obvious shape.
 *
 * **The portable set** (§3.3) is generic intrinsics with no bodies: `splat`, `iota`, the shuffles,
 * the reductions, the lane reads. One operation per lane type, so there is nothing to generate until
 * a call says which, and `expandIntrinsic` generates it where it is called. None of them is ever a
 * call in the IR.
 *
 * **The instances are generated on demand**, which is a departure from what §9 items 1 and 2 predict
 * and is worth stating outright. Those items describe a loop that generates `Num`, `Integral` and
 * the conversion ladder over every lane type at every lane count, the way `defineIntegerInstances`
 * does for scalars. Counted before it was written, that loop is about seven hundred instances and
 * upward of two thousand generated functions in Core - twelve lane types, six lane counts, and a
 * conversion ladder that is a *pair* of those - which every program in the language would then carry
 * whether or not it mentions a vector. The compiler's IR arena holds a program of one to two
 * thousand functions in total (see the arena ceiling), so the eager form is not merely wasteful.
 *
 * What is here instead answers the same question with the same rules at the point it is asked:
 * `vectorInstance` is consulted by instance selection when nothing declared answers, and generates
 * the instance for that exact head. A program that writes `Vec(Float)` pays for `Vec(Float)`. The
 * rules are unchanged - the vector ladder is still the scalar one lifted, and it is lifted by asking
 * the scalar question rather than by a second table of pairs, which is what item 2 asks for and is a
 * stronger form of it: there is no way for the two ladders to disagree because there is only one.
 */

// The portable set's hooks, attached to the signatures Core's source declares with no body. Called
// from definePreludeCore once those declarations are read.
void defineVectorIntrinsics(Module& core);

// The machine-specific set - the SHA extension and the predicate that gates it. Separate from the
// portable set above because the declarations it attaches to are `@platform(x64)`, so on any other
// target there is nothing there to attach to. See `lib/Core/X86.sha.yana`.
void defineCpuIntrinsics(Module& core);
bool hasCpuIntrinsics(const CompileSettings& settings);

/*
 * The bulk operations' hooks - §9 items 6 and 7.
 *
 * Each of `sum`, `product`, `maximum`, `minimum`, `occurrences` and `indexOf` is declared in
 * Collections with no body and two implementations beside it, one written over `vectors` and one
 * over the elements. What is attached here chooses between them at the call site, by asking whether
 * the container's element has a vector *on this target* - which is a question about a lane stride
 * and a register width, and so is not one a class constraint could carry.
 */
void defineBulkOperations(Module& collections);

/*
 * The instance of `typeClass` over these vector types, generated if it does not exist yet.
 *
 * Null for every head this does not cover, which is the ordinary answer: a class that is not one of
 * the seven below, an argument that is not a vector, or a pair the rules relate no instance for.
 *
 * `args` may hold a null in a position the asker does not constrain - `Lanewise(Vec(Int), _)` is how
 * a functional dependency is read - and what comes back has the position filled in.
 */
ModulePtr<ClassInstance> vectorInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args);
