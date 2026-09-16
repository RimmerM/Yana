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
DeclPtr<Function> teardownMember(Context& context, Program& program, intrinsic::Known klass,
                                 DeclPtr<Type> type, LocationId source);

/*
 * What turns an owner of this type into its view - the `ViewRow`'s derivation member, as a callable
 * function, on `teardownMember`'s exact terms.
 *
 * Two readers and one answer, which is the whole reason it is a function: the reachability walk
 * names it so a specialization's derivation is kept, and the SSA conversion names it to emit the
 * call. Reading `row->sharedAdaptor` and looking the instance up at each end would be two lookups
 * that agree until an instance is parametric - what derives `Array(I32)`'s window is a
 * *specialization* of what `instance (Contiguous(a)) Contiguous(Array(a), a)` wrote.
 *
 * Null where the type has no view for the access, which both readers treat as nothing to derive.
 */
DeclPtr<Function> viewMember(Context& context, Program& program, DeclPtr<Type> type, Access access,
                             LocationId source);

/*
 * What answers one `f: (a) -> b` requirement at one argument list.
 *
 * Filled by `matchCallable`, which is the whole of the question and is asked from two ends. The
 * **call** that supplies an argument list asks it to report - a requirement it cannot answer is a
 * program error, and the message belongs at the call. The **specialization** made from that argument
 * list asks it again to get the callee, and by then the answer is known to exist.
 *
 * One structure and one walk rather than a check beside a lookup, because the two ends have to agree
 * about *which* function answers: a discharge that admitted a call the target then bound differently
 * is a body typed against one signature and lowered against another, and nothing below would say so.
 */
struct CallableWitness {
    // The requirement's signature under the substitution - what a callee has to look like.
    SmallArray<DeclPtr<Type>, 4> args;
    DeclPtr<Type> result = nullptr;

    // Whether the substitution closed it. A signature still mentioning a variable names nothing to
    // look up: what answers it is the *enclosing* declaration having promised the same thing, which
    // is a question about a context rather than about a module and is asked by the caller.
    bool concrete = false;

    // The declaration name and arity reached, and null where there is none.
    DeclPtr<Function> found = nullptr;

    // Whether `found`'s declared signature is the one the requirement asked for.
    bool agrees = false;

    // `found`'s own type arguments where it is generic, bound by matching its declared positions
    // against the requirement's. Empty where it is not - which is `isGeneric` and never a null test
    // on the context, since every signature has one and a plain function's binds nothing.
    bool generic = false;
    SmallArray<DeclPtr<Type>, 4> bindings;
};

/*
 * The one place a callable requirement is answered - see `CallableWitness`.
 *
 * `from` is the module the name is looked up in, and it is the **generic's own** at both ends. That
 * is a limit rather than a preference: the useful reading is the instantiating module's, so that a
 * library's `fn (grow: (a) -> a) twice(x: a)` could be answered by a `grow` the caller declares. What
 * that needs is the witness recorded *into* the specialization at the call, the way a class witness
 * is, because by the time a specialization is compiled the caller is gone. Until it is, both ends
 * ask the same module and a cross-module witness is refused rather than resolved two ways.
 *
 * Answers false where a substitution failed outright, which is an error already reported and not a
 * refusal - a caller gives up rather than reporting a second time.
 */
bool matchCallable(Context& context, Program& program, DeclPtr<Module> from,
                   GenCallable required, const Substitution& with, CallableWitness& into);

/*
 * One filled slot of a generic environment - the answer a caller supplies for one `GenSlot`.
 *
 * The **resolved** answer and not a description of one: the concrete type, the number, the member
 * functions of the instance that was selected. That is what makes an environment a MIR artifact
 * rather than a lowering one - the reachability walk has to follow those functions or they are
 * never converted, and a lowering that discovered them itself would be selecting instances below
 * the pass that decides what a program contains.
 *
 * One struct for five kinds rather than a union, on `GenSlot`'s own terms: a slot has at most one
 * name, one type and one list, so the rows that do not apply are empty and no reader has to know
 * which shape a kind unpacks to.
 */
struct GenWitness {
    GenSlotKind kind = GenSlotKind::Type;

    // `Type`: the concrete type this slot describes. `Property`: the concrete subject it indexes.
    DeclPtr<Type> type = nullptr;

    /*
     * `Type`: **how the values this slot describes were allocated**, which is what decides the
     * teardown behind the descriptor rather than the type alone.
     *
     * A body compiled once holds an address and a descriptor, and what it may do with the storage
     * under that address is not something the type answers: a value in a region has had its storage
     * discharged in bulk, so running the reclaim half again would release every member twice, and a
     * value the caller boxed has storage that has to be handed back after the members are released.
     * The old stage reached the same conclusion the other way round and wrote it down at
     * `NativeTypeDesc::kTeardown` - "the region case does not want a second slot; it wants a second
     * descriptor" - which is exactly this field.
     *
     * `Frame` everywhere today, because it is the only rung anything selects. What has to change
     * with the first one that is not is `Program::internEnvironment`: two call sites at one argument
     * list that allocated differently need two environments, and the key is what makes them two.
     * `environmentFor` asserts the invariant that lets the key stay as it is.
     */
    StorageClass storage = StorageClass::Frame;

    // `Const`: the number, at the width its parameter counts in.
    U64 constant = 0;

    // `Property`: which field of `type`, as the index a layout is asked for.
    U16 field = 0;

    /*
     * `Class`: one per member of the class, in the class's declaration order. `Function`: exactly
     * one, the callable the requirement was answered by.
     *
     * Both are already specialized - what runs for `Show(Boxed(I32))` is a specialization of what
     * `instance (Show(a)) Show(Boxed(a))` wrote - so this is the list the reachability walk marks
     * and the list the lowering builds a table out of, with no second selection between them.
     */
    DeclList<DeclPtr<Function>, false> functions;
};

/*
 * What one call to an erased generic hands over - the canonical numbering, filled in.
 *
 * Interned by (generic, argument list) exactly as a specialization is, and for the same reason: the
 * contents are a function of that pair alone, so ten calls at one argument list are one environment
 * and one table in the image. What makes it a *different* table from the specialization table is
 * that nothing here is a declaration - an environment names functions, it does not become one.
 *
 * `slots` is parallel to the generic's frozen `GenSchema::slots` and in the same order, which is the
 * whole of the contract between the two ends: the caller writes slot 3 and the callee reads slot 3,
 * and neither has to describe the other.
 */
struct GenEnvironment {
    DeclPtr<Function> generic = nullptr;
    DeclList<DeclPtr<Type>, false> arguments;
    DeclList<GenWitness, false> slots;
};

/*
 * The environment for one generic at one concrete argument list, made if it does not exist.
 *
 * Every requirement of the generic's frozen schema is discharged here - an instance selected, a
 * field found, a callable matched - which is the same work `specializeFunction` does implicitly by
 * substituting into a body. Doing it explicitly is what an erased form *is*: the answers become data
 * the caller passes rather than types the callee was compiled at.
 *
 * Null where the discharge failed, which is reported: an argument list that cannot answer a
 * requirement is a program error, and the call is where it belongs.
 */
DeclPtr<GenEnvironment> environmentFor(Context& context, Program& program, DeclPtr<Function> generic,
                                       DeclPtr<Type>* args, Size count, LocationId source);

/*
 * Where one call gets the function it calls - the whole answer, in one structure.
 *
 * A call reaches its callee in one of three ways and they are not variations on each other: it names
 * a declaration, or it loads a function pointer out of the environment the enclosing erased body was
 * handed, or it names an erased declaration and has to build an environment of its own. Three
 * out-parameters would be three things a caller could read without the others; one structure is one
 * question with one answer, which is what `callTarget` being the single place a call is resolved
 * means.
 */
struct CallSite {
    /*
     * The function, where the call names one - and the *signature* the call was checked against
     * where it does not: the class's declaration of the member, or the declaration a callable
     * requirement matched. Set in every form, because a dump prints it and the argument list was
     * built from it.
     */
    DeclPtr<Function> target = nullptr;

    CallThrough through = CallThrough::Declaration;

    // Which requirement of the **enclosing** declaration's context supplies the slot. Unset on a
    // direct call.
    U16 requirement = 0;
    U16 member = 0;

    // Set where `target` is a generic compiled once, which is the one direct call that hands
    // something over besides its arguments.
    DeclPtr<GenEnvironment> environment = nullptr;

    /*
     * A call through a **site type**: `target` is the lifted function and the value the node calls
     * through is its **environment**, argument zero - Design-Iterator-Objects.md §4.
     *
     * A direct call in every respect that matters below - one declaration, one convention, one entry
     * in the reachability walk - and the one thing it does not share with an ordinary one is where
     * the first argument comes from, which is what this says. The conversion is where it is spent;
     * SSA has no such form, because by then the environment is the first of the arguments.
     */
    bool siteCall = false;
};

/*
 * What `callTarget` reads of a call - the two node kinds that name a callee, seen as one.
 *
 * A `call` and a `lens.call` reach their callee by the same rules and differ only in what the node
 * does with control afterwards, so the resolution is one function asked of a small request rather
 * than two copies of the specialization, instance-selection and environment logic that would then
 * have to be kept in step. `callRequestOf` is how each kind produces one.
 */
struct CallRequest {
    DeclPtr<Function> callee = nullptr;
    MirList<DeclPtr<Type>>* typeArguments = nullptr;
    LocationId source = kNullLocation;
    U16 genCallable = 0xffff;
    bool callsValue = false;

    // The type of the value a call through one goes through - `NodeCall::targetType`, which is what
    // tells a **site type** from a function type once the enclosing substitution is applied.
    DeclPtr<Type> targetType = nullptr;
};

inline CallRequest callRequestOf(NodeCall& node) {
    return CallRequest { node.callee, &node.typeArguments, node.source, node.genCallable,
                         node.callsValue(), node.targetType };
}

inline CallRequest callRequestOf(NodeLensCall& node) {
    return CallRequest { node.callee, &node.typeArguments, node.source, NodeCall::kNoCallable,
                         false, nullptr };
}

void callTarget(Context& context, Program& program, Function& enclosing, CallRequest node,
                CallSite& into);

/*
 * Which form each generic body is compiled in - `GenForm` in decl.h, and Analysis-IR.md §11.4's G6.
 *
 * Run once, after the bodies are analysed, because the teardowns the analysis places are part of
 * what an erased body would have to do and are the most expensive part of it. A written
 * `@specialize` or `@generic` is honoured; everything else is measured.
 *
 * One pass over the declarations rather than a question asked where a form is wanted, for the reason
 * `freezeSchemas` is a pass: the answer is shared between the caller that builds an environment and
 * the body that reads one, so it is decided once and read afterwards rather than derived twice.
 */
void selectGenForms(Context& context, Program& program);

} // namespace mir
