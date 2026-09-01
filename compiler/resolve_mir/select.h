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
 * `into` is filled where it is given, so that a caller which wants the member rather than the fact -
 * `teardownMember` - does not repeat the match to get at the bindings.
 */
DeclPtr<Instance> declaredInstanceFor(Context& context, Program& program, DeclPtr<Class> klass,
                                      DeclPtr<Type> type, InstanceMatch* into = nullptr);

DeclPtr<Instance> declaredInstanceFor(Context& context, Program& program, intrinsic::Known klass,
                                      DeclPtr<Type> type, InstanceMatch* into = nullptr);

} // namespace mir
