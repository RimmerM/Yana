#include "select.h"
#include "print.h"

namespace mir {

namespace {

/*
 * How deep a chain of head contexts one selection may follow.
 *
 * `Copy(Array(Array(I32)))` reaches `Copy(Array(I32))` and then `Copy(I32)`, and each step is a
 * strictly smaller type - so a well-formed program terminates long before this. What the bound is
 * for is a head whose context asks for something no smaller than what it matched, which a program
 * can write and which would otherwise be a stack overflow rather than a diagnostic.
 */
constexpr U32 kContextDepth = 32;

struct Selector {
    Context& context;
    Program& program;
    DeclBase base;
    bool report;
    U32 depth = 0;

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
            if(!matchType(base, instance.gen, into.bindings, head[i], args[i])) return false;
        }

        /*
         * Every variable of the head bound, which a match can leave undone.
         *
         * `instance Index(Map(k, v), k, v)` asked only for its first parameter binds `k` and `v`
         * from it; asked for a head that mentions a variable in a *determined* position alone, it
         * would not. A partially bound instance cannot be specialized, so it is not a match.
         */
        for(auto binding: into.bindings) {
            if(!binding) return false;
        }

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

    // The head's own `(Eq(a))`, at the bindings the match produced. A goal rather than a filter -
    // see `select.h` - so a failure here is reported against the type the author actually wrote.
    bool holdsContext(Instance& instance, const InstanceMatch& found, LocationId source) {
        if(!instance.gen) return true;

        Substitution with { instance.gen, (DeclPtr<Type>*)found.bindings.pointer(),
                            found.bindings.size() };

        for(auto requirement: base[instance.gen]->classes.contents(base)) {
            SmallArray<DeclPtr<Type>, 4> arguments;
            for(auto argument: requirement.args.contents(base)) {
                arguments.push(substituteType(context, program, argument, with));
            }

            InstanceMatch inner;
            if(select(requirement.klass, arguments.pointer(), arguments.size(), source, inner)) {
                continue;
            }

            return false;
        }

        return true;
    }

    bool select(DeclPtr<Class> klass, const DeclPtr<Type>* args, Size count, LocationId source,
                InstanceMatch& into) {
        if(!klass) return false;

        if(depth >= kContextDepth) {
            if(report) {
                context.diagnostics.error(
                    "selecting an instance of %@ leads back through its own context, so there is no instance to arrive at"_v,
                    source, context.findName(base[klass]->name));
            }

            return false;
        }

        depth++;
        auto found = selectHere(klass, args, count, source, into);
        depth--;
        return found;
    }

    bool selectHere(DeclPtr<Class> klass, const DeclPtr<Type>* args, Size count, LocationId source,
                    InstanceMatch& into) {
        /*
         * The keyed head first and the open one second, which is the specificity order rather than a
         * preference: a head naming a constructor matches strictly fewer types than one naming none,
         * so trying it first is what makes `instance (Copy(a)) Copy(Array(a))` win over
         * `instance (TrivialCopy(a)) Copy(a)` for an array without either of them saying so.
         */
        DeclPtr<Instance> tried[] = { program.keyedInstance(base, klass, args, count),
                                      program.openInstance(klass) };

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

bool selectInstance(Context& context, Program& program, DeclPtr<Class> klass,
                    const DeclPtr<Type>* args, Size count, LocationId source, bool report,
                    InstanceMatch& into) {
    Selector selector { context, program, *program.decl, report };
    return selector.select(klass, args, count, source, into);
}

DeclPtr<Instance> declaredInstanceFor(Context& context, Program& program, DeclPtr<Class> klass,
                                      DeclPtr<Type> type, InstanceMatch* into) {
    if(!klass) return nullptr;

    auto base = *program.decl;
    auto candidate = program.keyedInstance(base, klass, &type, 1);
    if(!candidate) return nullptr;

    /*
     * Keyed and still matched, because a key is the outer constructor alone: `instance Copy(Box(I32))`
     * and a use at `Box(Guard)` share one, and only the first is a statement about the second's type.
     * The head's own context is not asked about - what this answers is what the program *declared*
     * about this type, and a context that does not hold is a mistake to report where the class is
     * selected rather than a reason for the declaration to be invisible here.
     */
    InstanceMatch scratch;
    auto& found = into ? *into : scratch;

    Selector selector { context, program, base, false };
    if(!selector.match(*base[candidate], &type, 1, found)) return nullptr;

    return candidate;
}

DeclPtr<Instance> declaredInstanceFor(Context& context, Program& program, CoreClass klass,
                                      DeclPtr<Type> type, InstanceMatch* into) {
    return declaredInstanceFor(context, program, program.core[Size(klass)], type, into);
}

} // namespace mir
