/*
 * Ownership: what a type owes on the way out.
 *
 * Folded from what its members owe, in two halves that stay independent - a member with only a
 * `Reclaim` must not give its container a `Drop` and cost it region eligibility. It is neither a
 * structural question nor a physical one, which is why it is its own file: the answer depends on
 * the instances the program declared, so it is the one classification that a later declaration can
 * change.
 */

#include "type_internal.h"
#include "generic.h"
#include "module.h"
#include "name.h"
#include "index.h"

/*
 * Ownership classification (Implementation-IR.md part 4).
 *
 * Three questions, answered from the shape of the type plus whatever instances the program wrote:
 * can this be duplicated by copying its bytes, can it be relocated by copying its bytes, and does
 * the end of its lifetime run anything. The order below matters - the authored instances are
 * looked for first, because writing one is exactly the statement that the structural answer is
 * wrong for this type.
 */

// Whether the program wrote `instance Class(type)`. A class the program never declared (no Core,
// or a module built before Core's classes exist) has no instances, which is the right answer
// rather than a reason to check.
static bool hasInstance(Module& module, GlobalPtr<TypeClass> typeClass, TypePtr type) {
    if(!typeClass) return false;

    TypePtr args[] = { type };
    return findInstance(module, typeClass, toBuffer(args)) != nullptr;
}

// Folds one member into an aggregate's classification. A member that is not trivial makes the whole
// aggregate not trivial, and a member with either half of a teardown gives the aggregate a derived
// half of the same kind - which is the whole of "recurse into each field, then release this type's
// own storage". The two halves are folded independently, which is the point of splitting them: a
// member with only a `Reclaim` must not give its container a `Drop` and cost it region eligibility.
/*
 * A boxed member - see Field::boxed.
 *
 * The three answers all change, and each of them is the box rather than what is in it:
 *
 *  - **TrivialCopy is lost.** A bitwise duplicate would copy the pointer and leave two owners of one
 *    allocation. `Copy` is still writable by hand - allocate a new box, copy the target - so what
 *    boxing does is demote TrivialCopy to Copy rather than remove copying, which is the one
 *    non-transparent consequence of `@box` and the reason it is an API change.
 *  - **TrivialSink is preserved, and sometimes gained.** Relocating the owner moves a pointer and
 *    the target keeps its address, so a type that was self-referential - and therefore not
 *    TrivialSink - can become one by boxing the self-referential edge. That is why the inner value's
 *    answer is *not* folded in here.
 *  - **Reclaim is always derived**, because the box itself has to be handed back even where its
 *    target releases nothing. Drop follows the target: a box around something with no effect at last
 *    use has none either.
 */
static void includeBoxedMember(Module& module, TypePtr member, Ownership& target) {
    auto inner = ownershipOf(module, member);

    target.trivialCopy = false;
    target.reclaim = TeardownKind::Derived;

    if(inner.drop != TeardownKind::None && target.drop == TeardownKind::None) {
        target.drop = TeardownKind::Derived;
    }
}

// Folds one member into an aggregate's classification. A member that is not trivial makes the whole
// aggregate not trivial, and a member with either half of a teardown gives the aggregate a derived
// half of the same kind - which is the whole of "recurse into each field, then release this type's
// own storage". The two halves are folded independently, which is the point of splitting them: a
// member with only a `Reclaim` must not give its container a `Drop` and cost it region eligibility.
static void includeMember(Module& module, TypePtr member, Ownership& target, bool boxed = false) {
    if(boxed) return includeBoxedMember(module, member, target);

    auto inner = ownershipOf(module, member);

    target.trivialCopy = target.trivialCopy && inner.trivialCopy;
    target.trivialSink = target.trivialSink && inner.trivialSink;

    if(inner.reclaim != TeardownKind::None && target.reclaim == TeardownKind::None) {
        target.reclaim = TeardownKind::Derived;
    }

    if(inner.drop != TeardownKind::None && target.drop == TeardownKind::None) {
        target.drop = TeardownKind::Derived;
    }
}

Ownership ownershipOf(Module& module, TypePtr type) {
    auto base = *module.types;
    if(!type) return Ownership {};

    auto value = base[type];
    if(value->ownershipReady) return value->ownership;

    // A type reached from inside its own classification can only be a declaration that contains
    // itself without an indirection, which has no finite value. Answering conservatively rather
    // than recursing leaves the diagnostic to whoever computes its Repr, which is where an
    // infinitely large type is already reported.
    if(value->resolvingOwnership) {
        return Ownership { false, false, false, false, TeardownKind::Derived, TeardownKind::Derived };
    }

    Ownership result;
    value->resolvingOwnership = true;

    switch(value->kind) {
        case Type::Error:
        case Type::Unit:
        case Type::Int:
        case Type::Float:
        case Type::Literal:
            // The scalars, and the literal variable that becomes one. Nothing to release, and
            // both duplication and relocation are the bytes themselves.
            break;

        case Type::Ptr:
            // A raw pointer is an address, and an address is TrivialCopy by Design.md's own list.
            // Whatever it points at is owned by something else - that is what makes `%T` unsafe
            // and what keeps it out of this analysis entirely.
            break;

        case Type::String:
            /*
             * Implementation-String.md part 2's table, as the two entries that are not the Repr.
             *
             * **Not TrivialCopy, on both targets**, which that part states and then spends a
             * paragraph defending: a JS string really is free to duplicate - host strings are
             * immutable and the collector owns them - and it stays out of the class anyway, because
             * TrivialCopy is a resolve-stage fact that changes binding semantics and *"the same
             * borrow/move/drop rules apply identically on the native and JS targets; only the
             * codegen strategy differs"*. The JS backend is still free to notice that a move of one
             * needs no invalidation, which is the same category of thing as a `Reclaim` compiling to
             * nothing there.
             *
             * **TrivialSink**, because relocating a string is moving two words that do not refer to
             * their own address - the bytes are somewhere else and do not care where the descriptor
             * went. That is what lets a string be returned and stored without a call.
             *
             * The teardown is left at `None` here and supplied by the `instance Reclaim(String)`
             * below this switch, which is the ordinary authored path rather than a case in the
             * derived generator. That is deliberate: releasing a string is the *run's* placement
             * switch and nothing else, so the one comparison already written as `releaseRun` is the
             * whole of it, and a derived walk over a type with no members visible to resolve would
             * have had to grow a special case to reach it.
             */
            result.trivialCopy = false;
            break;

        case Type::Borrow:
            // A borrow owns nothing, so it releases nothing and relocates by copying its address.
            // Only the exclusive one is kept out of TrivialCopy: duplicating a mutable borrow on
            // read would hand out a second exclusive access to one place, which is the one thing
            // exclusivity means. An immutable borrow may be duplicated freely, which is exactly
            // Design.md's "any number of immutable borrows can be alive simultaneously".
            result.trivialCopy = ((BorrowType*)value)->mut == false;
            break;

        case Type::Gen:
            // Design.md: an unconstrained generic parameter is treated as non-TrivialCopy inside
            // the body regardless of what a caller substitutes, so that a generic function's
            // accepted programs are fixed by its own signature. The same argument applies to the
            // other two: the body must be written as though the type owns something.
            result = Ownership { false, false, false, false, TeardownKind::Derived, TeardownKind::Derived };
            break;

        case Type::Tup: {
            auto tuple = (TupType*)value;
            for(auto field: tuple->fields.contents(base)) {
                includeMember(module, field.type, result, field.boxed);
            }
            break;
        }

        case Type::Array: {
            /*
             * `n` members of one type, folded once - Implementation-Containers.md §6's "TrivialCopy
             * when `T` is; no indirect storage; derived teardown over exactly `n` members".
             *
             * Once and not `n` times, because every one of these folds is idempotent: `n` copies of
             * one answer is that answer. What that buys is that `[Buffer *65535]` costs the same to
             * classify as `[Buffer *1]`, which matters because ownership is asked of every type
             * reachable from every declaration.
             *
             * An empty one is left alone deliberately. A `[Buffer *0]` occupies nothing and has no
             * element to release, so folding `Buffer`'s teardown into it would give a value with no
             * members a derived teardown over them - which is a loop that runs zero times on a good
             * day and a walk off the end of a zero-size allocation on a bad one.
             */
            auto array = (ArrayType*)value;
            if(array->length) includeMember(module, array->content, result);
            break;
        }

        case Type::Record: {
            auto record = (RecordType*)value;

            // An enum-layout record is a discriminant and nothing else, so there are no member
            // types to fold and the scalar answer is already right.
            if(record->layout != RecordType::Enum) {
                for(auto constructor: record->constructors.contents(base)) {
                    if(!constructor.content) continue;
                    includeMember(module, constructor.content, result, constructor.boxed);
                }
            }

            break;
        }

        case Type::Fun:
            // A function value owns the environment its captures live in (Design-Memory §8), so a
            // bitwise duplicate would alias that environment and it is not TrivialCopy. It *is*
            // TrivialSink - relocating it moves three words and the environment keeps its address -
            // and its teardown is derived: run the environment descriptor's drop, if it has one.
            //
            // That answer is the same for a non-capturing lambda, whose descriptor is null. Making
            // it depend on what one value captured would make ownership a property of a value
            // rather than of a type, which is exactly what the model does not allow.
            result = Ownership { false, true, false, false, TeardownKind::Derived, TeardownKind::Derived };
            break;

        default:
            // Ref, RegionPtr, Region, Array and Map. None of them are constructible yet;
            // classifying them conservatively is what makes adding one a decision rather than a
            // silently wrong default.
            result = Ownership { false, false, false, false, TeardownKind::Derived, TeardownKind::Derived };
            break;
    }

    // An authored instance overrides the structural answer, which is what writing one means. A
    // generic declaration is skipped: `Maybe(a)` is not a type anything can have an instance for,
    // and asking would match the instance of whatever `a` last resolved to.
    if(!value->generic) {
        if(hasInstance(module, module.coreClasses.reclaim, type)) {
            result.reclaim = TeardownKind::Authored;

            /*
             * A container's teardown is computed from its elements - Implementation-Containers.md
             * §13.
             *
             * An authored `Reclaim` over a *parametric* head is a container's one traversal over its
             * live elements, and the author is trusted about "I call nothing else" - which
             * checkReclaimShape verifies - and never about "my members are effect-free", which is
             * this. Whether that traversal has effects is decided by whether the type arguments have
             * a `Drop`, so `Array(Int)`'s is a reclaim and nothing more while `Array(Buffer)`'s is
             * also a drop, and the two differ in region eligibility rather than in code.
             *
             * Not derivable structurally, and that is the whole reason this rule exists: the run's
             * members are a raw pointer and two counts, so a fold over them says a container of
             * connections has no teardown. Which slots hold values is private to the container, and
             * the only thing the compiler can see about them is the type they are of.
             */
            if(value->kind == Type::Record) {
                for(auto arg: ((RecordType*)value)->instanceArgs.contents(base)) {
                    if(ownershipOf(module, arg).drop != TeardownKind::None) {
                        result.drop = TeardownKind::Authored;
                    }
                }
            }
        }

        if(hasInstance(module, module.coreClasses.drop, type)) result.drop = TeardownKind::Authored;
        if(hasInstance(module, module.coreClasses.copy, type)) result.authoredCopy = true;

        if(hasInstance(module, module.coreClasses.sink, type)) {
            result.authoredSink = true;
            result.trivialSink = false;
        }
    }

    // Duplicating a value whose lifetime releases something would release it twice, so a teardown
    // of either kind rules out TrivialCopy. This is stated once here rather than at each producer
    // of one above, because it holds for the authored cases as well as the derived ones.
    if(result.needsTeardown()) result.trivialCopy = false;

    // And TrivialCopy implies TrivialSink, because a duplicate is strictly more than a relocation:
    // a type whose bytes cannot even be *moved* without a call - it refers to its own address -
    // certainly cannot have those bytes duplicated into a second live value. Saying so here is what
    // makes an authored `Sink` reachable at all, since `->` copies rather than moves a TrivialCopy
    // source and a type left in both classes would never take the move path its instance is for.
    if(!result.trivialSink) result.trivialCopy = false;

    value->resolvingOwnership = false;

    /*
     * Remembered only once no further instance can appear - see Program::declarationsComplete.
     *
     * This is the one classification a later declaration can change, and the header above says so;
     * what it did not say is that the cache made "later" mean "never". Three of the built-in modules
     * resolve their own bodies inside their define step, so a type reached from one of those bodies
     * was classified against a program that was still being declared - and `instance Reclaim(String)`
     * is declared two modules further up than the first body to ask a `String` what it owes.
     *
     * Recomputing until then costs a walk over the members of whatever built-in bodies mention, and
     * that is the whole of the cost: the first query after this point caches, and every query the
     * ownership pass makes is after it.
     */
    if(module.program.declarationsComplete) {
        value->ownership = result;
        value->ownershipReady = true;
    }

    return result;
}

/*
 * The context-sensitive half of the classification.
 *
 * This mirrors ownershipOf's structural fold, and differs from it in exactly one place: at a type
 * variable, where the answer comes from what the context declared rather than from the type. The
 * result is never cached, because two contexts can legitimately disagree about the same `a`.
 *
 * `depth` bounds the walk the way instance proving does. A type reachable from itself without an
 * indirection has no finite value and is reported by whoever computes its Repr.
 */
static Ownership ownershipInAt(Module& module, GenEnv* env, TypePtr type, U32 depth) {
    auto base = *module.types;
    if(!type || !isGeneric(base, type) || !depth) return ownershipOf(module, type);

    auto value = base[type];

    switch(value->kind) {
        case Type::Gen: {
            auto result = ownershipOf(module, type);
            TypePtr args[] = { type };

            if(env) {
                if(provesClass(module, *env, module.coreClasses.trivialCopy, toBuffer(args))) {
                    result.trivialCopy = true;
                    result.reclaim = TeardownKind::None;
                    result.drop = TeardownKind::None;
                }

                if(provesClass(module, *env, module.coreClasses.trivialSink, toBuffer(args))) {
                    result.trivialSink = true;
                }
            }

            return result;
        }

        case Type::Tup: {
            Ownership result;
            for(auto field: ((TupType*)value)->fields.contents(base)) {
                auto inner = ownershipInAt(module, env, field.type, depth - 1);

                // The boxed rules, stated the same way includeBoxedMember states them: the pointer
                // is what is copied, moved and released, so TrivialCopy goes, TrivialSink stays, the
                // reclaim is the box's own, and only the drop follows what is inside it.
                if(field.boxed) {
                    result.trivialCopy = false;
                    result.reclaim = TeardownKind::Derived;
                    if(inner.drop != TeardownKind::None) result.drop = TeardownKind::Derived;
                    continue;
                }

                result.trivialCopy = result.trivialCopy && inner.trivialCopy;
                result.trivialSink = result.trivialSink && inner.trivialSink;
                if(inner.reclaim != TeardownKind::None) result.reclaim = TeardownKind::Derived;
                if(inner.drop != TeardownKind::None) result.drop = TeardownKind::Derived;
            }

            if(result.needsTeardown() || !result.trivialSink) result.trivialCopy = false;
            return result;
        }

        case Type::Array: {
            // `n` members of one type, folded once - the same rule ownershipOf states, asked in a
            // context where the element may still be a variable the constraints say something about.
            auto array = (ArrayType*)value;
            if(!array->length) return Ownership {};

            auto result = ownershipInAt(module, env, array->content, depth - 1);
            if(result.needsTeardown() || !result.trivialSink) result.trivialCopy = false;
            return result;
        }

        case Type::Record: {
            auto record = (RecordType*)value;
            Ownership result;

            if(record->layout != RecordType::Enum) {
                for(auto constructor: record->constructors.contents(base)) {
                    if(!constructor.content) continue;

                    auto inner = ownershipInAt(module, env, constructor.content, depth - 1);

                    if(constructor.boxed) {
                        result.trivialCopy = false;
                        result.reclaim = TeardownKind::Derived;
                        if(inner.drop != TeardownKind::None) result.drop = TeardownKind::Derived;
                        continue;
                    }

                    result.trivialCopy = result.trivialCopy && inner.trivialCopy;
                    result.trivialSink = result.trivialSink && inner.trivialSink;
                    if(inner.reclaim != TeardownKind::None) result.reclaim = TeardownKind::Derived;
                    if(inner.drop != TeardownKind::None) result.drop = TeardownKind::Derived;
                }
            }

            /*
             * An authored instance overrides the structural answer here too, and asking for one is
             * what this used to skip.
             *
             * The reason it skipped was that `Maybe(a)` is not a type anything writes an instance
             * for, and asking might match whatever `a` last resolved to. The first half is true and
             * the second is not: findInstance matches the *arguments*, so `Cell(a)` does not reach
             * an `instance Drop(Cell(Int))` - and an instance written over a parametric head is a
             * type something does write one for. `Reclaim(Array(a))` is the whole of Core's
             * container teardown.
             *
             * Skipping it made `Array(a)` inside a generic body structurally trivial - a raw pointer
             * and two counts - so `let ->ys = xs` took sinkValue's duplicate path and left two
             * owners of one run, and a returned one was released before it left. Both were invisible
             * in a concrete body, where ownershipOf answers the same question with the instance in
             * hand, which is why nothing caught it until an adaptor materialized into an array.
             *
             * The parametric-head reclaim's *drop* half is deliberately not derived from the type
             * arguments the way ownershipOf derives it. In here an argument is a variable whose
             * `Drop` the context decides, and asking would answer for the declaration's own
             * variable instead. The conservative answer is the reclaim alone, which costs a
             * generic body region eligibility it might have had and never costs it correctness.
             */
            if(hasInstance(module, module.coreClasses.reclaim, type)) result.reclaim = TeardownKind::Authored;
            if(hasInstance(module, module.coreClasses.drop, type)) result.drop = TeardownKind::Authored;

            if(hasInstance(module, module.coreClasses.sink, type)) {
                result.authoredSink = true;
                result.trivialSink = false;
            }

            if(result.needsTeardown() || !result.trivialSink) result.trivialCopy = false;
            return result;
        }

        default:
            return ownershipOf(module, type);
    }
}

Ownership ownershipIn(Module& module, GenEnv* env, TypePtr type) {
    return ownershipInAt(module, env, type, 8);
}
