#include "select.h"
#include "print.h"
#include "intern.h"

namespace mir {

namespace {

/*
 * How deep a chain of head contexts one selection may follow before it stops on resource grounds.
 *
 * A **budget** and not the cycle check. A head whose context asks for the goal being answered -
 * `instance (Show(a)) Show(a)` - leads back to itself, and that is caught as what it is, by the
 * active-goal test in `select`: the same class at the same arguments is already being selected. A
 * chain that is merely long is a different thing: `Copy(Array(Array(...I32...)))` thirty-three deep
 * reaches `Copy(I32)` in thirty-three finite steps, and the absolute depth of thirty-two that used
 * to sit here reported it as cycling. So the number is now far above anything a program writes, and
 * the message it produces says "more than the compiler follows" rather than "cycles".
 */
constexpr U32 kContextBudget = 256;

struct Selector {
    Context& context;
    Program& program;
    DeclBase base;
    bool report;

    /*
     * The goals being selected right now, outermost first - the class at position `i` asked at
     * `activeArgs[activeStart[i] .. activeStart[i + 1])`. Flat, because a struct holding a list is
     * a list that cannot be reassigned (`compiler/util/README.md`), and because what is asked of it
     * is one comparison per level.
     */
    SmallArray<DeclPtr<Class>, 8> activeClasses;
    SmallArray<U32, 8> activeStart;
    SmallArray<DeclPtr<Type>, 32> activeArgs;

    /*
     * The goals this selection has finished, with their answers.
     *
     * A context asks the same subgoal from several requirements - `instance (Copy(a), Copy(b))
     * Copy(Pair(a, b))` at `Pair(X, X)` asks `Copy(X)` twice, and nested pairs ask it a number of
     * times that doubles per level - so a derivation is a DAG walked as a tree without this. Per
     * selection rather than per program, because the instance table is still being declared while
     * the earliest selections run and a memo that outlived a declaration could name a less specific
     * instance than the program ends up with. Successes only: a failure is reported where it is
     * asked, and re-asking it is what reports it at every site.
     */
    struct Answered {
        DeclPtr<Class> klass;
        DeclPtr<Instance> instance;
        U32 argStart = 0, argCount = 0;
        U32 bindStart = 0, bindCount = 0;
        U32 resultStart = 0, resultCount = 0;
    };

    Array<Answered> answered;
    Array<DeclPtr<Type>> answeredTypes;
    HashMap<U64, U32> answeredIndex;

    static U64 goalKey(DeclPtr<Class> klass, const DeclPtr<Type>* args, Size count) {
        InternKey key;
        key.mix(klass.offset);
        for(Size i = 0; i < count; i++) key.next().mix(args[i] ? args[i].offset : 0);

        // All ones is `HashMap`'s empty-key sentinel.
        return key.hash == ~U64(0) ? 0 : key.hash;
    }

    static bool sameArgs(const DeclPtr<Type>* a, const DeclPtr<Type>* b, Size count) {
        for(Size i = 0; i < count; i++) {
            if(a[i] != b[i]) return false;
        }

        return true;
    }

    /*
     * One candidate head matched against what was asked for.
     *
     * The determined positions are *read off the head* rather than matched against: a call that
     * supplied one is checked below, and a call that left it null is what the dependency exists to
     * answer. Everything else is `matchType`, which is the same one-way match a call site infers
     * type arguments with.
     */
    bool match(Instance& instance, const DeclPtr<Type>* args, Size count, InstanceMatch& into) {
        auto& klass = *base[instance.klass];
        auto head = instance.args.pointer(base);
        if(instance.args.size() != count) return false;

        into.bindings.clear();
        if(instance.gen) {
            for(Size i = 0; i < base[instance.gen]->variables.size(); i++) into.bindings.push(nullptr);
        }

        for(Size i = 0; i < count; i++) {
            if(!args[i]) continue;
            if(!matchType(base, program.types, instance.gen, into.bindings, head[i], args[i])) return false;
        }

        /*
         * Every variable of the head bound, which a match can leave undone.
         *
         * `instance Index(Map(k, v), k, v)` asked only for its first parameter binds `k` and `v`
         * from it; asked for a head that mentions a variable in a *determined* position alone, it
         * would not. A partially bound instance cannot be specialized, so it is not a match.
         */
        for(Size i = 0; i < into.bindings.size(); i++) {
            if(into.bindings[i]) continue;

            /*
             * A variable the head mentions **nowhere**, which the instance's own context decides -
             * `GenContext::deduced`, filled where the instance was declared.
             *
             * `instance (Magnitude(a, w), Show(w)) ShowWith(a)` is the shape: nothing in `ShowWith`
             * says what `w` is, and `Magnitude(a -> w)` does. Left open here rather than bound to
             * itself, because binding it to itself would specialize the member at a type variable;
             * `holdsContext` is where the dependency is read, and it fills this in.
             */
            if(base[instance.gen]->deduced & (U64(1) << i)) continue;

            /*
             * A variable the query spells **as itself** - the instance's own body asking about its
             * own head. `instance Reclaim(Run(a))` reads `value: Run(a)` at the very `a` the head
             * binds, and `matchType`'s identical-type shortcut binds nothing on the way through, so
             * the head looked partially bound and the record was classified as if the instance did
             * not exist - an owner read as a trivial copy, which the SSA tier refused at `Run(I32)`.
             * A variable that stands for itself is bound to itself.
             */
            auto variable = base[instance.gen]->variables.get(base, U32(i));
            auto mentioned = false;
            for(Size j = 0; j < count && !mentioned; j++) {
                mentioned = args[j] && mentionsVariable(base, args[j], variable);
            }

            if(!mentioned) return false;
            into.bindings[i] = variable;
        }

        // A `deduced` variable may still be null here, and the head cannot mention one - that is
        // what `deduced` means - so nothing this substitution reaches is left open by it.
        Substitution with { instance.gen, into.bindings.pointer(), into.bindings.size() };

        into.arguments.clear();
        for(Size i = 0; i < count; i++) {
            auto filled = substituteType(context, program, head[i], with);

            // A determined position the caller also supplied has to agree with the instance: two
            // answers to a question the dependency promised has one is the promise being false.
            if(args[i] && klass.isDetermined(U16(i)) && !sameType(filled, args[i])) return false;

            into.arguments.push(args[i] ? args[i] : filled);
        }

        into.instance = &instance - base;
        return true;
    }

    // Whether `type` still mentions a variable of this instance that nothing has bound.
    bool mentionsOpen(Instance& instance, const InstanceMatch& found, DeclPtr<Type> type) {
        SmallArray<bool, 4> mentioned;
        for(Size i = 0; i < found.bindings.size(); i++) mentioned.push(false);

        markVariables(base, instance.gen, type, mentioned);

        for(Size i = 0; i < mentioned.size(); i++) {
            if(mentioned[i] && !found.bindings[i]) return true;
        }

        return false;
    }

    // `type` read as one still-unbound variable of this instance, or `kNoVariable` - the position a
    // dependency can answer, as opposed to a shape that merely contains one.
    static constexpr U32 kNoVariable = ~U32(0);

    U32 openVariable(Instance& instance, const InstanceMatch& found, DeclPtr<Type> type) {
        if(!type || base[type]->kind != Type::Variable) return kNoVariable;

        auto& variable = *(VariableType*)base[type];
        if(variable.owner != instance.gen || variable.index >= found.bindings.size()) return kNoVariable;

        return found.bindings[variable.index] ? kNoVariable : U32(variable.index);
    }

    /*
     * The head's own `(Eq(a))`, at the bindings the match produced. A goal rather than a filter -
     * see `select.h` - so a failure here is reported against the type the author actually wrote.
     *
     * And the one place a **functional dependency in the context binds** rather than checks.
     * `instance (Magnitude(a, w), Show(w)) ShowWith(a)` reaches here with `w` open; solving
     * `Magnitude(Fixed, ?)` answers it, and every requirement after it is then an ordinary goal. So
     * this is a small fixpoint rather than a walk: the requirements are in source order and the
     * chain is not, a requirement whose deciding half is still open is *skipped* rather than
     * guessed at, and a round that binds nothing new ends it.
     *
     * It is the same rule `closeDecidedVariables` states at the declaration, which is why the two
     * had to arrive together: without this an instance with a context-decided variable declares
     * cleanly and is never found.
     */
    bool holdsContext(Instance& instance, InstanceMatch& found, LocationId source) {
        if(!instance.gen) return true;

        auto count = base[instance.gen]->classes.size();

        SmallArray<bool, 4> discharged;
        for(Size i = 0; i < count; i++) discharged.push(false);

        auto remaining = count;

        for(auto progressed = true; progressed && remaining;) {
            progressed = false;

            for(Size at = 0; at < count; at++) {
                if(discharged[at]) continue;

                auto requirement = base[instance.gen]->classes.get(base, U32(at));
                auto& klass = *base[requirement.klass];

                Substitution with { instance.gen, (DeclPtr<Type>*)found.bindings.pointer(),
                                    found.bindings.size() };

                /*
                 * The arguments, with each still-open variable in a *determined* position handed
                 * over as null - which is `selectInstance`'s way of asking the dependency for it.
                 * Anything else still mentioning an open variable means this requirement is not the
                 * one that can be solved yet.
                 */
                SmallArray<DeclPtr<Type>, 4> arguments;
                SmallArray<U32, 4> asking;
                auto ready = true;

                for(Size i = 0; i < requirement.args.size(); i++) {
                    auto argument = substituteType(context, program,
                                                   requirement.args.get(base, U32(i)), with);

                    auto open = klass.isDetermined(U16(i))
                              ? openVariable(instance, found, argument) : kNoVariable;

                    if(open != kNoVariable) {
                        arguments.push(nullptr);
                        asking.push(open);
                        continue;
                    }

                    if(mentionsOpen(instance, found, argument)) {
                        ready = false;
                        break;
                    }

                    arguments.push(argument);
                    asking.push(kNoVariable);
                }

                if(!ready) continue;

                InstanceMatch inner;
                if(!select(requirement.klass, arguments.pointer(), arguments.size(), source, inner)) {
                    return false;
                }

                // What the dependency answered, written back into the instance's own bindings. The
                // full argument list is `inner.arguments`, which is where a determined position the
                // caller left null comes back filled.
                for(Size i = 0; i < asking.size(); i++) {
                    if(asking[i] == kNoVariable) continue;
                    found.bindings[asking[i]] = inner.arguments[i];
                }

                discharged[at] = true;
                remaining--;
                progressed = true;
            }
        }

        /*
         * A requirement nothing could make ready. Reported here rather than passed over, on the same
         * reading as a requirement that fails: the head matched, so this instance is the one the
         * program says applies, and the author's mistake is in its context.
         */
        if(remaining) {
            if(report) {
                context.diagnostics.error(
                    "selecting %@ leaves a variable of its context undecided, so its own requirements cannot be proved"_v,
                    source, context.findName(base[instance.klass]->name));
            }

            return false;
        }

        return true;
    }

    bool select(DeclPtr<Class> klass, const DeclPtr<Type>* args, Size count, LocationId source,
                InstanceMatch& into) {
        if(!klass) return false;

        auto key = goalKey(klass, args, count);

        // Answered already, in this selection. Re-checked structurally: a hash is not an identity.
        if(auto found = answeredIndex.getValue(key)) {
            auto& entry = answered[found.unwrap()];
            auto types = answeredTypes.pointer();

            if(entry.klass == klass && entry.argCount == count &&
               sameArgs(types + entry.argStart, args, count)) {
                into.instance = entry.instance;

                into.bindings.clear();
                for(U32 i = 0; i < entry.bindCount; i++) into.bindings.push(types[entry.bindStart + i]);

                into.arguments.clear();
                for(U32 i = 0; i < entry.resultCount; i++) {
                    into.arguments.push(types[entry.resultStart + i]);
                }

                return true;
            }
        }

        /*
         * The goal is one being answered further up - a context that asks for what it is supplying.
         * That is the cycle, told from a long chain by being the *same* goal rather than a deep one.
         */
        for(U32 i = 0; i < activeClasses.size(); i++) {
            if(activeClasses[i] != klass || activeStart[i + 1] - activeStart[i] != count) continue;
            if(!sameArgs(activeArgs.pointer() + activeStart[i], args, count)) continue;

            if(report) {
                context.diagnostics.error(
                    "selecting an instance of %@ leads back through its own context, so there is no instance to arrive at"_v,
                    source, context.findName(base[klass]->name));
            }

            return false;
        }

        if(activeClasses.size() >= kContextBudget) {
            if(report) {
                context.diagnostics.error(
                    "selecting an instance of %@ goes through more than %@ nested contexts, which is more than the compiler follows"_v,
                    source, context.findName(base[klass]->name), U64(kContextBudget));
            }

            return false;
        }

        if(!activeStart.size()) activeStart.push(0);
        activeClasses.push(klass);
        for(Size i = 0; i < count; i++) activeArgs.push(args[i]);
        activeStart.push(U32(activeArgs.size()));

        auto found = selectHere(klass, args, count, source, into);

        activeStart.resize(activeStart.size() - 1);
        activeClasses.resize(activeClasses.size() - 1);
        activeArgs.resize(activeStart[activeStart.size() - 1]);

        if(found) {
            Answered entry;
            entry.klass = klass;
            entry.instance = into.instance;

            entry.argStart = U32(answeredTypes.size());
            entry.argCount = U32(count);
            for(Size i = 0; i < count; i++) answeredTypes.push(args[i]);

            entry.bindStart = U32(answeredTypes.size());
            entry.bindCount = U32(into.bindings.size());
            for(auto binding: into.bindings) answeredTypes.push(binding);

            entry.resultStart = U32(answeredTypes.size());
            entry.resultCount = U32(into.arguments.size());
            for(auto argument: into.arguments) answeredTypes.push(argument);

            auto index = U32(answered.size());
            answered.push(entry);

            auto slot = answeredIndex.add(key);
            *slot.value = index;
        }

        return found;
    }

    bool selectHere(DeclPtr<Class> klass, const DeclPtr<Type>* args, Size count, LocationId source,
                    InstanceMatch& into) {
        /*
         * Most specific first, which is the specificity order rather than a preference: a head
         * naming a constructor in a position matches strictly fewer types than one naming none
         * there, so trying it first is what makes `instance (Copy(a)) Copy(Array(a))` win over
         * `instance (TrivialCopy(a)) Copy(a)` for an array without either of them saying so.
         */
        SmallArray<DeclPtr<Instance>, 4> tried;
        program.candidateInstances(base, klass, args, count, tried);

        for(auto candidate: tried) {
            if(!candidate || !match(*base[candidate], args, count, into)) continue;
            if(holdsContext(*base[candidate], into, source)) return true;

            /*
             * Matched and its context did not hold, which ends the search rather than falling
             * through to the next row. The head that matched is the one the program says applies,
             * so a less specific one taking over would be selection answering a question the author
             * did not ask - and the message is the recursive select's, naming the requirement that
             * failed instead of this class.
             */
            into.instance = nullptr;
            return false;
        }

        if(report) {
            SmallArray<DeclPtr<Type>, 4> written;
            for(Size i = 0; i < count; i++) written.push(args[i]);

            context.diagnostics.error("there is no instance of %@"_v, source,
                                      appliedClassText(context, base, klass, written.pointer(),
                                                       written.size()));
        }

        into.instance = nullptr;
        return false;
    }
};

} // namespace

/*
 * A class the compiler answers from the type's own shape rather than from a declaration - the
 * **structural** classes.
 *
 * `Nothing` is "not one of these classes", which is a different answer from "this class, and no":
 * the first falls through to the instance table and the second is a refusal. One function over all
 * of them, so that a class added to the group is a row here rather than a test at each asking site.
 *
 * Both of today's rows are the atomics' - Analysis-Atomics.md §3.2's two closed classifications -
 * and each reads the predicate in `type.cpp` that is already the one statement of the family.
 * Writing an instance of either is refused where instances are declared, for the reason a closed
 * classification is closed: the family is what a target can perform indivisibly, and a program
 * saying otherwise about its own type would be claiming a machine instruction that does not exist.
 *
 * A null class handle answers `Nothing` through the comparison, which is what an `Optional` role in
 * a program whose library does not declare it leaves behind - so there is no test for it here.
 */
static Maybe<bool> structuralAnswer(Program& program, DeclBase base, DeclPtr<Class> klass,
                                    const DeclPtr<Type>* args, Size count) {
    if(!klass || count != 1 || !args[0]) return Nothing();

    auto role = base[klass]->role;
    if(!isStructuralClass(role)) return Nothing();

    switch(role) {
        case intrinsic::Known::atomicValue:   return Just(isAtomicValue(base, args[0]));
        case intrinsic::Known::atomicInteger: return Just(isAtomicInteger(base, args[0]));
        default: break;
    }

    // A role `isStructuralClass` admits and this does not answer - which is a role added to the
    // group without deciding what it means, and the one thing the split into two functions can get
    // wrong.
    assertTrue(false);
    return Nothing();
}

bool selectInstance(Context& context, Program& program, DeclPtr<Class> klass,
                    const DeclPtr<Type>* args, Size count, LocationId source, bool report,
                    InstanceMatch& into) {
    auto base = *program.decl;

    /*
     * Asked **before** the table, because a structural class has no table entry to find: nothing
     * declares an instance of one and nothing may. The match that comes back names no instance,
     * which is sound because these classes declare no members - `checkRoles` holds a claimant to
     * that - so there is nothing for a caller to dispatch through.
     */
    if(auto answer = structuralAnswer(program, base, klass, args, count)) {
        if(answer.unwrap()) {
            into.arguments.clear();
            for(Size i = 0; i < count; i++) into.arguments.push(args[i]);
            return true;
        }

        if(report) {
            context.diagnostics.error("%@ does not hold - it is a classification the compiler makes from the type rather than one a program declares"_v,
                                      source, appliedClassText(context, base, klass, args, count));
        }

        return false;
    }

    Selector selector { context, program, base, report };
    return selector.select(klass, args, count, source, into);
}

DeclPtr<Instance> declaredInstanceFor(Context& context, Program& program, DeclPtr<Class> klass,
                                      DeclPtr<Type> type, InstanceMatch* into) {
    if(!klass) return nullptr;

    auto base = *program.decl;
    SmallArray<DeclPtr<Instance>, 4> keyed;
    program.keyedInstances(base, klass, &type, 1, keyed);

    /*
     * Keyed and still matched, because a key is the outer constructor alone: `instance Copy(Box(I32))`
     * and a use at `Box(Guard)` share one, and only the first is a statement about the second's type.
     * Several of them share it as well - `Copy(Box(I64))` is a third - which is why this is a walk
     * over what the key narrowed to rather than a match against one row, and why it is in the same
     * specificity order `candidateInstances` hands back.
     *
     * The head's own context is not asked about - what this answers is what the program *declared*
     * about this type, and a context that does not hold is a mistake to report where the class is
     * selected rather than a reason for the declaration to be invisible here.
     */
    InstanceMatch scratch;
    auto& found = into ? *into : scratch;

    Selector selector { context, program, base, false };
    for(auto candidate: keyed) {
        if(selector.match(*base[candidate], &type, 1, found)) return candidate;
    }

    return nullptr;
}

DeclPtr<Instance> declaredInstanceFor(Context& context, Program& program, intrinsic::Known klass,
                                      DeclPtr<Type> type, InstanceMatch* into) {
    return declaredInstanceFor(context, program, program.roles.klass(klass), type, into);
}

} // namespace mir
