#pragma once

#include "decl.h"

namespace mir {

struct Instance;
struct Program;

/*
 * Instance selection - a *match* above the table, and what G3's lookup grew into.
 *
 * G3 keyed the table by (class, logical type) and called the result selection. That is a lookup, and
 * it can express exactly the instances whose head is a bare type name: `instance Eq(I32)`. Every
 * instance in `lib/` that is worth having is not one - `instance (Eq(a)) Eq(Array(a))`,
 * `instance (Hash(k)) Index(Map(k, v), k, v)` - because what a container implements a class for is a
 * *shape* and not a type. So the head becomes a pattern and selection becomes matching.
 *
 * Three things make that a bounded operation rather than a solver:
 *
 *  - **The head's outer constructors are the key.** `Eq(Array(a))` is filed under `(Eq, Array)`, so
 *    a lookup narrows to one candidate before anything is matched. That is what keeps coherence a
 *    property of a hash table - two instances with one key is a diagnostic at the declaration - and
 *    what keeps selection off a search over every instance in the program.
 *  - **An open head is tried second and there is at most one per class.** `instance (TrivialCopy(a))
 *    Copy(a)` names no constructor and therefore matches everything; a keyed head is strictly more
 *    specific, so the order is the specificity order and not a preference. Two open heads for one
 *    class is a diagnostic, because deciding between them is the overlap reasoning this is written
 *    to not need.
 *  - **The head's own context is a goal, not a filter.** A matched head whose `(Eq(a))` does not
 *    hold is reported rather than passed over. Passing over it is what turns one missing instance
 *    into "no instance of `Eq(Array(Guard))`", which names a type nobody wrote; reporting it names
 *    `Guard`, which is the one the author has to fix.
 */

/*
 * What selection produced - which instance, what its head bound, and the class arguments in full.
 *
 * `arguments` is the class's own argument list with every position filled, which is not the same as
 * what the caller asked with: a **functional dependency** lets a call supply the deciding prefix and
 * read the rest off the head that matched, so `elements(xs)` at `xs: Array(I32)` asks
 * `Contiguous(Array(I32), ?)` and gets `a = I32` back here.
 *
 * `bindings` is indexed by the *instance's* own context - what `Opt(a)` bound `a` to - and is what a
 * member of a parametric instance is specialized at.
 */
struct InstanceMatch {
    DeclPtr<Instance> instance = nullptr;
    SmallArray<DeclPtr<Type>, 4> bindings;
    SmallArray<DeclPtr<Type>, 4> arguments;

    explicit operator bool() const { return instance != nullptr; }
};

/*
 * The instance serving `klass` at these arguments.
 *
 * `args` is the class's full arity, with **null** in any position the caller has not decided - which
 * has to be a determined one, since a deciding position is what the key is built from. Those are
 * filled in from the head that matched, which is the whole of what a functional dependency buys.
 *
 * Reports when there is no instance, when the matched head's own context does not hold, or when a
 * deciding position was left open. `report` is false for a caller that is asking whether rather than
 * which - the ownership classifier, and the check that an instance's superclasses are present -
 * where a missing instance is an answer instead of a mistake.
 */
bool selectInstance(Context& context, Program& program, DeclPtr<Class> klass,
                    const DeclPtr<Type>* args, Size count, LocationId source, bool report,
                    InstanceMatch& into);

/*
 * The same question asked from *inside* a generic body, with that body's own variables in the query.
 *
 * `selectInstance` is asked about types a caller settled. This is asked where a requirement's head
 * still mentions the asking declaration's variables - `Index(Array(a), ?, ?)` inside
 * `fn (Ord(a)) firstBelow(xs: [a], ...)` - and it differs from that one in exactly three ways, each
 * argued in select.cpp: a deciding position that is a **bare variable** is refused rather than
 * looked up, two heads that both apply and **answer the determined positions differently** are an
 * ambiguity naming both, and the head's own context may be discharged by `enclosing` having
 * promised it. `doc/progress/2026-09-06-inventory.md` D13.
 */
bool selectOpenInstance(Context& context, Program& program, DeclPtr<Class> klass,
                        const DeclPtr<Type>* args, Size count, DeclPtr<GenContext> enclosing,
                        LocationId source, bool report, InstanceMatch& into);

/*
 * Whether that is a question at all - `selectOpenInstance`'s own precondition, exported.
 *
 * A caller has to tell "this is not something the table may be asked" from "the table has no
 * answer", because only the second is worth a diagnostic: `fillFromRequirements` runs during the
 * argument walk, where a deciding position is routinely still an inference variable, and a message
 * there would name a type the author never wrote. Every variable in a deciding position has to be
 * one of `enclosing`'s, and the position itself may not be a bare variable.
 */
bool openHeadAnswerable(Program& program, DeclPtr<Class> klass, const DeclPtr<Type>* args,
                        Size count, DeclPtr<GenContext> enclosing);

/*
 * The instance whose head *names* this type for this class - the question the ownership classifier
 * asks, and deliberately not `selectInstance`.
 *
 * `Copy`, `Drop` and `Reclaim` each **subtract** a capability from their subject, and
 * `doc/spec/ownership.md` places a removal in the module that declares what it is removing from. A
 * keyed head does exactly that: `instance (Copy(a)) Copy(Array(a))` is a statement about `Array`,
 * made where `Array` is. An **open** head - `instance (TrivialCopy(a)) Copy(a)` - is a statement
 * about every type there is, including ones already compiled against the derived answer, so it may
 * not be what takes `trivialCopy` away from one of them. Selection is therefore the wrong question
 * here and this is the right one: what did somebody declare *about this type*.
 *
 * **This is the whole of what keeps a blanket subtraction-free**, and `declareInstance` now relies
 * on it rather than refusing the blanket: a blanket is selectable, so `copy(x)` at a trivial type
 * finds it, and it is invisible here, so nothing is classified by it. A future that let this
 * function read an open head would have to bring the refusal back with it.
 *
 * `into` is filled where it is given, so that a caller which wants the member rather than the fact -
 * `teardownMember` - does not repeat the match to get at the bindings.
 */
DeclPtr<Instance> declaredInstanceFor(Context& context, Program& program, DeclPtr<Class> klass,
                                      DeclPtr<Type> type, InstanceMatch* into = nullptr);

// The same over a whole head, where a caller has one - a determined position may be left null and
// the match reads it back off the head that answered.
DeclPtr<Instance> declaredInstanceFor(Context& context, Program& program, DeclPtr<Class> klass,
                                      const DeclPtr<Type>* args, Size count,
                                      InstanceMatch* into = nullptr);

DeclPtr<Instance> declaredInstanceFor(Context& context, Program& program, intrinsic::Known klass,
                                      DeclPtr<Type> type, InstanceMatch* into = nullptr);

/*
 * **What calling a value of this type means**, as the signature the call is checked against - the
 * one function three tiers ask, Design-Iterator-Objects.md §2.3.
 *
 * Three answers and one absence. A **function type** is its own signature, and a value of one is
 * called through the pair. A **site type** names the code it is called through, so its signature is
 * the `(A) -> B` its erased form would have had and the call is direct. A **type variable** has
 * whatever the enclosing signature promised about it - the `Call(f, S)` clause, which is the whole
 * of what that constraint buys a body. Null is "not callable", which is the message a call gives.
 *
 * One function rather than the pair of tests spelled at each reader, because the readers are the
 * resolver, the MIR verifier and the erasability check, and a call that three tiers disagree about
 * the shape of is a call whose arguments were bound against one signature and checked against
 * another. `gen` is null wherever the question is not being asked inside a generic body, which makes
 * the variable arm answer nothing rather than having to be guarded at each site.
 */
DeclPtr<Type> callableShapeOf(Program& program, DeclPtr<GenContext> gen, DeclPtr<Type> type);

/*
 * Which declared class requirement of `gen` is the `Call(type, S)` a body compiled **once**
 * dispatches through - the index into `GenContext::classes`, which is also the schema's class-group
 * position (`freezeSchema` numbers them in that order).
 *
 * The cell behind it is synthesized rather than an instance's member list, because `Call` is
 * structural and declares none - see `CallThrough::CallWitness`. Three readers and one walk: the
 * erasability check admits the call, `callTarget` resolves it, and the reachability marks the slot.
 */
bool callWitnessRequirement(Program& program, DeclPtr<GenContext> gen, DeclPtr<Type> type,
                            U16& into);

/*
 * Whether this role's class is one the compiler answers from the shape of the type - the
 * **structural** classes, `structuralAnswer` in select.cpp being where each is answered.
 *
 * One predicate and three readers, which is why it is here rather than a list repeated at each:
 * selection answers such a class without consulting the table, `checkRoles` holds a claimant to
 * declaring no members - there is nothing for a caller to dispatch through - and an `instance` of
 * one written in source is refused where instances are declared. A role added to the group has to
 * satisfy all three, and this is the one line that says which roles are in it.
 */
inline bool isStructuralClass(intrinsic::Known role) {
    return role == intrinsic::Known::atomicValue || role == intrinsic::Known::atomicInteger
        || role == intrinsic::Known::trivialCopy || role == intrinsic::Known::trivialSink
        || role == intrinsic::Known::call;
}

} // namespace mir
