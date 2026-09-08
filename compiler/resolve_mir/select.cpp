#include "select.h"
#include "print.h"
#include "intern.h"

namespace mir {

// Declared here and defined below, because `Selector::select` asks it: a structural class is
// answered wherever a requirement is discharged, not only where a caller entered. See the
// definition, where the group and what "no" means for it are argued.
static Maybe<bool> structuralAnswer(Program& program, DeclBase base, DeclPtr<Class> klass,
                                    const DeclPtr<Type>* args, Size count);

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
     * The context of the declaration this selection is being run *inside*, or null.
     *
     * Set only by `selectOpenInstance` - inventory D13. Everywhere else selection is a question
     * about types the caller already settled, and a body's own requirements have nothing to say
     * about them; there it stays null and nothing below reads it. Where it is set, a matched head's
     * context may be discharged by the enclosing signature having promised the same thing, which is
     * the one answer `holdsContext` cannot reach through the instance table: `Eq(a)` of a body's own
     * `a` is not in it and never will be.
     */
    DeclPtr<GenContext> enclosing = nullptr;

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

                /*
                 * The enclosing signature's own promise, asked before the table - D13's recursion.
                 *
                 * `fn (Eq(a)) same(xs: [a]) = xs == xs` reaches `instance (Eq(a)) Eq(Array(a))` with
                 * the head's `a` bound to the *body's* `a`, and the instance's `Eq(a)` is then a
                 * goal about a type variable, which no instance answers. What answers it is the
                 * clause the author wrote, and asking here is what makes the whole derivation
                 * recursive through the same rule rather than only its first step. Skipped where a
                 * position is being *asked* rather than supplied, since a null is a question and a
                 * promise answers none.
                 */
                auto supplied = true;
                for(Size i = 0; i < asking.size(); i++) supplied = supplied && asking[i] == kNoVariable;

                if(enclosing && supplied &&
                   satisfiesRequirement(context, program, enclosing, requirement.klass,
                                        arguments.pointer(), arguments.size())) {
                    discharged[at] = true;
                    remaining--;
                    progressed = true;
                    continue;
                }

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

        /*
         * A **structural** class first, and asked here rather than at the entry point - which is the
         * whole of what moving it was about.
         *
         * `TrivialCopy` and its three neighbours have no table entry and never will: nothing
         * declares an instance of one and nothing may. Asked only where a caller reached the free
         * `selectInstance`, a structural goal standing *inside* another instance's context found no
         * row and was reported as a missing instance - `instance (Magnitude(a, w), TrivialCopy(w),
         * ...) ReadWith(NumberStyle, a)` at `readHexInt`, whose `w` the dependency had decided to
         * `U32` one requirement earlier, reported as "there is no instance of TrivialCopy(U32)".
         * Asking here is that rule said once, wherever a requirement is discharged.
         *
         * Ahead of the memo and of the cycle check, because neither has anything to say about it:
         * the answer is a fold over the type rather than a search, so there is nothing to remember
         * and no context it could lead back through. The match names no instance, which is sound
         * because these classes declare no members.
         */
        if(auto answer = structuralAnswer(program, base, klass, args, count)) {
            if(answer.unwrap()) {
                into.instance = nullptr;
                into.bindings.clear();
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

    /*
     * Whether a head **narrower than the one chosen** answers the determined positions differently.
     *
     * This is the whole of what makes an open query sound, and it is not the same question as "does
     * a second head match". Matching is one-way, so a head naming `Pair(I32)` cannot match a query
     * spelling `Pair(a)` at all - and yet a specialization at `a := I32` reaches exactly that head,
     * because the table orders the narrower one first. So the query's answer and the
     * specialization's would be two different types with nothing between them saying so.
     *
     * Asked by matching the *chosen* head's deciding positions against the narrower head's, which
     * is what a specialization does to it, and then comparing what each says about the determined
     * ones under those bindings. `Sized(Pair(a), a)` against `Sized(Pair(Box(b)), Box(b))` agrees -
     * one is the other at `a := Box(b)`, which is the fundep holding. `Sized(Pair(a), a)` against
     * `Sized(Pair(I32), Bool)` does not, and that is the program the author has to fix.
     */
    bool narrowerDisagrees(Instance& chosen, Instance& other, Size count) {
        auto& klass = *base[chosen.klass];
        if(other.klass != chosen.klass || other.args.size() != count) return false;

        SmallArray<DeclPtr<Type>, 4> bindings;
        if(chosen.gen) {
            for(Size i = 0; i < base[chosen.gen]->variables.size(); i++) bindings.push(nullptr);
        }

        auto head = chosen.args.pointer(base);
        auto narrow = other.args.pointer(base);

        for(Size i = 0; i < count; i++) {
            if(klass.isDetermined(U16(i))) continue;
            if(!matchType(base, program.types, chosen.gen, bindings, head[i], narrow[i])) return false;
        }

        Substitution with { chosen.gen, bindings.pointer(), bindings.size() };

        for(Size i = 0; i < count; i++) {
            if(!klass.isDetermined(U16(i))) continue;
            if(!sameType(substituteType(context, program, head[i], with), narrow[i])) return true;
        }

        return false;
    }

    void reportDisagreement(Instance& first, Instance& second, LocationId source) {
        if(!report) return;

        context.diagnostics.error(
            "%@ and %@ both apply to this body's own type, and they answer differently - so what this call means would change when it is specialized. Require the one this body means"_v,
            source,
            appliedClassText(context, base, first.klass, first.args.pointer(base),
                             first.args.size()),
            appliedClassText(context, base, second.klass, second.args.pointer(base),
                             second.args.size()));
    }

    /*
     * The same table, asked with the *caller's own type variables still in the query* - D13.
     *
     * `xs[i]` inside `fn (Ord(a)) firstBelow(xs: [a], ...)` needs `Index(Array(a), ?, ?)`, and the
     * three things that make that answerable are here rather than in `select`:
     *
     *  - **the match is silent**, because a position nothing decided is the caller's message to
     *    give at the position rather than this one's to give about the class;
     *  - **the first match is not the end of the search.** A head that matches this query matches
     *    every specialization of it, so the specificity order still names the answer - but a head
     *    the query is *too general to match* is one a specialization can still reach, and if it
     *    answers the determined positions differently the body has no single answer. Both shapes go
     *    through `narrowerDisagrees` and are reported naming the two heads;
     *  - **the enclosing context is carried in.** See `Selector::enclosing`.
     */
    bool selectOpen(DeclPtr<Class> klass, const DeclPtr<Type>* args, Size count, LocationId source,
                    InstanceMatch& into) {
        SmallArray<DeclPtr<Instance>, 4> tried;
        program.candidateInstances(base, klass, args, count, tried);

        DeclPtr<Instance> chosen = nullptr;

        for(auto candidate: tried) {
            if(!candidate || !match(*base[candidate], args, count, into)) continue;
            chosen = candidate;
            break;
        }

        if(!chosen) {
            into.instance = nullptr;
            return false;
        }

        /*
         * And every other head, in both directions - the whole list rather than the tail of it,
         * because the head a specialization would prefer is ordered *before* the one this query was
         * general enough to match.
         */
        for(auto candidate: tried) {
            if(!candidate || candidate == chosen) continue;

            InstanceMatch second;
            auto matched = match(*base[candidate], args, count, second);

            auto disagrees = matched
                ? narrowerDisagrees(*base[candidate], *base[chosen], count)
                : narrowerDisagrees(*base[chosen], *base[candidate], count);

            if(!disagrees) continue;

            reportDisagreement(*base[chosen], *base[candidate], source);
            into.instance = nullptr;
            return false;
        }

        // `into` still holds `chosen`'s match: the loop above breaks on the first success and the
        // disagreement walk matches every other head into a local. `match` clears and rewrites all
        // three fields on the way through, so a candidate that failed before `chosen` left nothing.

        // The head's own context, on `selectHere`'s terms - the match decided which instance
        // applies, so a context that does not hold is a mistake reported against it rather than a
        // reason to look at a more general head.
        if(!holdsContext(*base[chosen], into, source)) {
            into.instance = nullptr;
            return false;
        }

        return true;
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
 * Two families, and each row reads the predicate that is already the one statement of its family.
 * The **atomics** are Analysis-Atomics.md §3.2's two closed classifications, read from `type.cpp`;
 * the two **ownership** rows are `Ownership`'s own fields, the same answer `readMode` and
 * `placeDrops` act on. Writing an instance of any of them is refused where instances are declared,
 * for the reason a closed classification is closed: an atomic family is what a target can perform
 * indivisibly, and a program saying otherwise would be claiming a machine instruction that does
 * not exist; TrivialCopy and TrivialSink hold for a type exactly when they hold for every one of
 * its members, so a written instance would contradict a walk the compiler has already done.
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

        /*
         * The two ownership classifications, and the two conditions on asking them.
         *
         * A **generic** type is never answered, which is Design-Memory §2.1 and not a conservatism:
         * a body's behaviour is fixed by its own signature, so an unconstrained `a` may not take
         * TrivialCopy from whatever one call site substituted. `Nothing` rather than `Just(false)`,
         * because the answer for a variable is the *context's*: a declared `(TrivialCopy(a))` is
         * discharged from the requirements in scope, and falling through is how it gets there.
         *
         * And **only once the classification exists**. `ownershipOf` reads what `classifyTypes`
         * folded, and a record asked before that pass answers from a zeroed field - trivial,
         * droppable by nobody - which is the wrong answer rather than an early one. Nothing in the
         * pipeline should ask: `resolve.cpp`'s order puts every declaration before `classifyTypes`
         * and every body after it, so this is an assertion about that order rather than a case.
         */
        case intrinsic::Known::trivialCopy:
        case intrinsic::Known::trivialSink: {
            if(!isConcreteType(base, args[0])) return Nothing();
            assertTrue(program.typesClassified);

            auto ownership = ownershipOf(base, args[0]);
            return Just(role == intrinsic::Known::trivialCopy ? ownership.trivialCopy
                                                              : ownership.relocatable);
        }

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

    // A structural class is answered inside `Selector::select`, so that this entry point and a
    // requirement discharged under a matched head reach the same rule - `checkRoles` holds a
    // claimant to these classes declaring no members, which is what makes an answer naming no
    // instance sound.
    Selector selector { context, program, base, report };
    return selector.select(klass, args, count, source, into);
}

/*
 * The instance serving `klass` at an argument list that still mentions the asking body's variables -
 * `doc/progress/2026-09-06-inventory.md` D13.
 *
 * **A deciding position whose outermost constructor is concrete is answerable; a bare variable is
 * not.** `Index(Array(a), ?, ?)` names `Array`, and a head that matches `Array(a)` with `a` free
 * matches it at every type a specialization binds - the match is one-way, so a head naming a
 * narrower constructor inside cannot match a query that leaves it open. `Index(a, ?, ?)` names
 * nothing, and answering it from whichever instances happen to be declared is exactly the thing
 * that makes a specialization mean something other than the body it came from.
 *
 * **What is recorded is the requirement and not the instance.** The caller uses this to fill in the
 * determined positions a functional dependency promised - which is a fact about the *class* - and
 * the requirement then travels with the body under the substitution, discharged again at every
 * instantiation exactly as a written one is. So a specialization at `a := I32` re-selects and takes
 * the most specific head that applies there, which may be a narrower one than this saw. That is
 * consistent rather than a hole *because* the class's dependency says the deciding prefix decides
 * the rest: two heads under one key that answered `k` and `v` differently is the ambiguity
 * `selectOpen` refuses here, and two heads of one shape is what `addInstance` refuses at the
 * declaration. What is left is a narrower head answering the same `k` and `v`, and reaching it is
 * ordinary specialization.
 *
 * Silent where there is no answer: the caller is filling a position in, and "nothing decided this"
 * is its message to give at the position rather than this function's to give about the class.
 */
/*
 * Whether every variable this type mentions is one of `owner`'s - the walk `isConcreteType` makes,
 * asked about *whose* variables rather than about whether there are any.
 *
 * The distinction is the whole of D13's precondition. A type carrying the asking body's own `a` is
 * a shape that is settled for every specialization; one carrying an *inference* variable - a callee
 * parameter nothing has bound yet, which is what a half-resolved call site is full of - is a shape
 * that is not settled at all, and answering from the table there decides a question the argument
 * walk was still in the middle of.
 */
static bool variablesFrom(DeclBase base, DeclPtr<Type> type, DeclPtr<GenContext> owner) {
    if(!type) return true;

    switch(base[type]->kind) {
        case Type::Variable:
            return owner && ((VariableType*)base[type])->owner == owner;
        case Type::Pointer:
            return variablesFrom(base, ((PointerType*)base[type])->to, owner);
        /*
         * A **quantified count is a variable like any other** here, where `isConcreteType` treats it
         * as an absence. That is the difference the two questions are asking: a body compiled once
         * cannot lay `[a *n]` out, which is what that function is about, and `[a *n]` is still one
         * shape for every `n` a specialization supplies, which is what this one is about.
         * `instance (n: Int) Contiguous([a *n], a)` is the head that matches it.
         */
        case Type::Array: {
            auto& array = *(ArrayType*)base[type];
            return variablesFrom(base, array.element, owner) && variablesFrom(base, array.count, owner);
        }
        case Type::Vector: {
            auto& vector = *(VectorType*)base[type];
            return variablesFrom(base, vector.lane, owner) && variablesFrom(base, vector.count, owner);
        }
        case Type::Atomic:
            return variablesFrom(base, ((AtomicType*)base[type])->content, owner);
        case Type::Record:
            for(auto argument: ((RecordType*)base[type])->arguments.contents(base)) {
                if(!variablesFrom(base, argument, owner)) return false;
            }
            return true;
        case Type::Fun: {
            auto& fun = *(FunType*)base[type];
            for(auto param: fun.params.contents(base)) {
                if(!variablesFrom(base, param.occurrence.type, owner)) return false;
            }
            return variablesFrom(base, fun.result.type, owner);
        }
        default:
            return true;
    }
}

bool openHeadAnswerable(Program& program, DeclPtr<Class> klass, const DeclPtr<Type>* args,
                        Size count, DeclPtr<GenContext> enclosing) {
    if(!klass) return false;

    auto base = *program.decl;

    for(Size i = 0; i < count; i++) {
        if(base[klass]->isDetermined(U16(i))) continue;

        // A bare variable names no constructor, which is the case D13 leaves refused - and a null
        // is a deciding position the caller has not supplied, which is not a question either.
        if(!args[i] || base[args[i]]->kind == Type::Variable) return false;
        if(!variablesFrom(base, args[i], enclosing)) return false;
    }

    return true;
}

bool selectOpenInstance(Context& context, Program& program, DeclPtr<Class> klass,
                        const DeclPtr<Type>* args, Size count, DeclPtr<GenContext> enclosing,
                        LocationId source, bool report, InstanceMatch& into) {
    if(!openHeadAnswerable(program, klass, args, count, enclosing)) return false;

    auto base = *program.decl;
    Selector selector { context, program, base, report, enclosing };
    return selector.selectOpen(klass, args, count, source, into);
}

DeclPtr<Instance> declaredInstanceFor(Context& context, Program& program, DeclPtr<Class> klass,
                                      const DeclPtr<Type>* args, Size count, InstanceMatch* into) {
    if(!klass) return nullptr;

    auto base = *program.decl;
    SmallArray<DeclPtr<Instance>, 4> keyed;
    program.keyedInstances(base, klass, args, count, keyed);

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
        if(selector.match(*base[candidate], args, count, found)) return candidate;
    }

    return nullptr;
}

/*
 * The one-argument form, which is every caller that asks about a **class of one** - the three
 * ownership classes and the two view derivations' subjects.
 *
 * The list is as long as the class's head and the determined positions are left null, because
 * `Selector::match` compares the head's arity against the list it is given and a shorter one is not
 * a match at all. `Contiguous(c -> a)` is the shape that made this matter: the key is the deciding
 * prefix, so the *lookup* is one type, and the *match* is still against two positions.
 */
DeclPtr<Instance> declaredInstanceFor(Context& context, Program& program, DeclPtr<Class> klass,
                                      DeclPtr<Type> type, InstanceMatch* into) {
    if(!klass) return nullptr;

    auto base = *program.decl;
    SmallArray<DeclPtr<Type>, 4> args;
    args.push(type);
    for(U16 i = 1; i < base[klass]->arity(base); i++) args.push(nullptr);

    return declaredInstanceFor(context, program, klass, args.pointer(), args.size(), into);
}

DeclPtr<Instance> declaredInstanceFor(Context& context, Program& program, intrinsic::Known klass,
                                      DeclPtr<Type> type, InstanceMatch* into) {
    return declaredInstanceFor(context, program, program.roles.klass(klass), type, into);
}

} // namespace mir
