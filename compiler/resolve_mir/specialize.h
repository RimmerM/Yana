#pragma once

#include "decl.h"
#include "gen.h"
#include "select.h"

namespace mir {

/*
 * Specialization - Analysis-IR.md §11.4's G2.
 *
 * A specialization is a `Function` with a **bound signature and no body of its own**: its `mir`
 * handle is the generic's, and what makes it a different function is the argument list it records.
 * That is the whole of how Implementation-Generics.md §1's invariant is kept - there is one resolved
 * body, one ownership analysis and one set of drop points, and a specialization is that body read
 * under a substitution. Copying the body per argument list would have been a second thing that can
 * drift from the first; here there is nothing to drift from, and the fixture that asserts identical
 * drop placement with and without a specialization is asserting a property of the representation
 * rather than of a pass.
 *
 * What a specialization therefore does *not* do is decide anything. Which instance was selected,
 * which use mode each operand carries and where each teardown goes were all settled on the generic
 * body before any specialization existed; substitution reaches the types and nothing else.
 *
 * Split from `gen.h` because the two answer different questions. That file is about what a generic
 * declaration *is* - variables, contexts, the canonical numbering, and what substituting into a type
 * means. This one is about what a *call* to one becomes, which is the only thing that makes a second
 * `Function`.
 */

/*
 * The specialization of `generic` for one argument list, made if it does not exist.
 *
 * Interned by (generic, arguments), so a function called at the same types from ten places is one
 * declaration. The arguments must be concrete - a specialization is what a *bound* signature is, and
 * one holding a variable would be a second generic under another name.
 */
DeclPtr<Function> specializeFunction(Context& context, Program& program, DeclPtr<Function> generic,
                                     DeclPtr<Type>* args, Size count, LocationId source);

/*
 * The substitution a function's body is read under - empty for everything but a specialization.
 *
 * The pointer is into the function's own `typeArguments`, so it is valid as long as the declaration
 * is, which is the whole compilation.
 */
Substitution substitutionOf(DeclBase base, Function& function);

// The declaration whose MIR body this function runs: itself, or the generic it specializes. One
// question with one answer, so that no pass has to test `specializes` for itself.
Function& bodyOwnerOf(DeclBase base, Function& function);

/*
 * Which function one call node reaches, from the body it is written in.
 *
 * **The one place a generic call is resolved to a callee**, and the reason `NodeCall` names the
 * generic rather than a specialization: which one a call reaches depends on the substitution the
 * enclosing body is being compiled under, so the node carries the question and this answers it.
 * Reachability, the summary walk and the SSA conversion all ask here; a version where each worked it
 * out would be three answers that agree until one of them is changed.
 *
 * Answers the callee unchanged for a plain call, which is every call outside milestone 6.
 */
// One member of one instance, made from the class's default where the instance left it out. See
// the note on the definition.
DeclPtr<Function> instanceMember(Context& context, Program& program, const InstanceMatch& found,
                                 U16 member, LocationId source);

/*
 * What runs when a value of this type dies - the teardown half `klass` names, as a callable function.
 *
 * Two readers and one answer: the summary walk names it so the reachability keeps it, and the SSA
 * lowering names it to emit the call. They were two lookups reading `members.get(decl, 0)` off the
 * instance directly, which stops being the answer the moment an instance is parametric - what runs
 * for `Array(I32)` is a *specialization* of what `instance (Copy(a)) Copy(Array(a))` wrote. Asking
 * once means the specialization the summary made is the one the lowering finds.
 *
 * Null where the type has no authored half of that kind, which both readers treat as nothing to run.
 */
DeclPtr<Function> teardownMember(Context& context, Program& program, CoreClass klass,
                                 DeclPtr<Type> type, LocationId source);

DeclPtr<Function> callTarget(Context& context, Program& program, Function& enclosing,
                             NodeCall& node);

} // namespace mir
