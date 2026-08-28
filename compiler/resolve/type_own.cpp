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

/*
 * The three kinds that hold members, folded - `kTypeMembers`, and the arms type.def deliberately
 * does not carry.
 *
 * Their answer is per instance rather than per kind: `Tup(Int)` and `Tup(Buffer)` are one kind and
 * two classifications, so a column stating one would be stating it for the other too. What the
 * column does say is that these three are folded at all, which is what makes a fourth container kind
 * a `-Wswitch` diagnostic here rather than a scalar answer nobody looked at.
 */
static void foldMembers(Module& module, Type* value, Ownership& result) {
    auto base = *module.types;

    switch(value->kind) {
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
            if(writtenCount(base, array->count).from(1)) includeMember(module, array->content, result);
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

        default:
            assertTrue("only a kTypeMembers kind is folded - see type.def" && false);
            break;
    }
}

// The fold itself: one type's classification from its members', with whatever answers the round in
// progress has for them. Split out from ownershipOf so that the fixpoint can drive it round by round.
static Ownership foldOwnership(Module& module, TypePtr type, Type* value) {
    auto base = *module.types;
    Ownership result;

    /*
     * The rule this kind is classified by - type.def's ownership column, where each row's answer is
     * argued beside the kind it is about.
     *
     * The scalar answers are constants and are stated there rather than here, because they are the
     * ones a *kind* really does decide: nothing about an `Atomic` being uncopyable or a `String`
     * being an owner depends on what either of them was applied to. What is left below is the two
     * rules that are not a constant - a borrow, refined by which capability it is, and the three
     * kinds whose answer is folded out of what they hold, which is per instance and could not be a
     * column without becoming a lie.
     */
    switch(kindOwnership(value->kind)) {
        case OwnershipRule::Bytes:
            break;

        case OwnershipRule::NotCopied:
            result.trivialCopy = false;
            break;

        case OwnershipRule::Referenced:
            // A borrow owns nothing, so it releases nothing and relocates by copying its address.
            // Only the exclusive one is kept out of TrivialCopy: duplicating a mutable borrow on
            // read would hand out a second exclusive access to one place, which is the one thing
            // exclusivity means. An immutable borrow may be duplicated freely, which is exactly
            // Design.md's "any number of immutable borrows can be alive simultaneously".
            result.trivialCopy = ((BorrowType*)value)->mut == false;
            break;

        case OwnershipRule::Captured:
            // A function value owns the environment its captures live in (Design-Memory §8), so a
            // bitwise duplicate would alias that environment and it is not TrivialCopy. It *is*
            // TrivialSink - relocating it moves three words and the environment keeps its address -
            // and its teardown is derived: run the environment descriptor's drop, if it has one.
            //
            // That answer is the same for a non-capturing lambda, whose descriptor is null. Making
            // it depend on what one value captured would make ownership a property of a value
            // rather than of a type, which is exactly what the model does not allow.
            result = Ownership { false, true, false, TeardownKind::Derived, TeardownKind::Derived };
            break;

        case OwnershipRule::Opaque:
            // A type variable, and the kinds that are named but not constructible yet. Both owe
            // everything: a generic body must be written as though its parameter owns something
            // whatever a caller substitutes, and classifying a reserved kind conservatively is what
            // makes building one a decision rather than a silently wrong default.
            result = Ownership { false, false, false, TeardownKind::Derived, TeardownKind::Derived };
            break;

        case OwnershipRule::Members:
            foldMembers(module, value, result);
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
    }

    // Duplicating a value whose lifetime releases something would release it twice, so a teardown
    // of either kind rules out TrivialCopy. This is stated once here rather than at each producer
    // of one above, because it holds for the authored cases as well as the derived ones.
    if(result.needsTeardown()) result.trivialCopy = false;

    // And TrivialCopy implies TrivialSink, because a duplicate is strictly more than a relocation:
    // a type whose bytes cannot be *moved* certainly cannot have those bytes duplicated into a
    // second live value.
    if(!result.trivialSink) result.trivialCopy = false;

    return result;
}

/*
 * One query made from inside a running fixpoint.
 *
 * Three answers, in the order they are asked for: the assumption, if the fold for this type is on
 * the stack and the query is therefore a cycle; what this round already folded, if the type is
 * shared and has been reached by another path; and otherwise the fold, run now.
 */
static Ownership ownershipInSolve(Module& module, TypePtr type, Type* value) {
    auto& solve = module.program.ownershipSolve;

    auto entry = solve.answers.add(type.offset);
    if(!entry.existed) new (entry.value) OwnershipSolve::Answer();

    if(entry.value->generation != solve.generation) {
        // The first time this solve reaches the type. It is assumed to owe nothing - the optimistic
        // end of the lattice - and the rounds below take away whatever it turns out to owe.
        *entry.value = OwnershipSolve::Answer { Ownership {}, solve.generation, 0 };
        solve.reached.push(type);
    } else if(value->resolvingOwnership) {
        solve.usedAssumption = true;
        return entry.value->value;
    } else if(entry.value->round == solve.round) {
        return entry.value->value;
    }

    value->resolvingOwnership = true;
    auto result = foldOwnership(module, type, value);
    value->resolvingOwnership = false;

    // The fold recursed, and a member reached along the way may have rehashed the map - so the entry
    // is looked up again rather than held across it.
    auto answer = solve.answers.get(type.offset).get();
    if(result != answer->value) {
        answer->value = result;
        solve.changed = true;
    }

    answer->round = solve.round;
    return result;
}

Ownership ownershipOf(Module& module, TypePtr type) {
    auto base = *module.types;
    if(!type) return Ownership {};

    auto value = base[type];
    if(value->ownershipReady) return value->ownership;

    auto& solve = module.program.ownershipSolve;
    if(solve.running) return ownershipInSolve(module, type, value);

    solve.running = true;
    solve.generation++;
    solve.reached.clear();

    /*
     * The fixpoint, and the two ways out of it.
     *
     * A round that read no assumption saw no cycle: the fold it ran is the ordinary structural one
     * and there is nothing to iterate, which is what every non-recursive type takes. A round that
     * read one and moved nothing is the fixpoint. The first round always moves something - every
     * answer starts at the assumption and almost none of them stay there - so a recursive type costs
     * at least two.
     *
     * It terminates because every fold above is monotone downwards from the assumption: TrivialCopy
     * and TrivialSink are only ever cleared, a teardown half is only ever raised, and neither can be
     * put back. `kRoundLimit` is a guard against a fold added later that is not, rather than a bound
     * anything real approaches - the deepest cycle in the corpus converges in two.
     */
    const U32 kRoundLimit = 64;
    Ownership result;

    for(solve.round = 1; solve.round <= kRoundLimit; solve.round++) {
        solve.usedAssumption = false;
        solve.changed = false;

        result = ownershipInSolve(module, type, value);
        if(!solve.usedAssumption || !solve.changed) break;
    }

    assertTrue(solve.round <= kRoundLimit);
    solve.running = false;

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
     *
     * Every type the solve reached is written back, not just the one asked for: they were folded
     * together and a member's answer is as settled as the answer that used it.
     */
    if(module.program.declarationsComplete) {
        for(auto reached: solve.reached) {
            auto member = base[reached];
            member->ownership = solve.answers.get(reached.offset).unwrap().value;
            member->ownershipReady = true;
        }
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
            if(!writtenCount(base, array->count).from(1)) return Ownership {};

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
