#include "name.h"

namespace mir {

/*
 * The typed wrappers over `search`.
 *
 * Each one contributes only the rule that is specific to its kind of symbol - a constructor can be
 * namespaced under its record, a function is keyed by arity as well as by name - and leaves module
 * order, qualified prefixes, visibility and ambiguity to the traversal.
 */

bool exportedSymbol(DeclBase base, DeclPtr<RecordType> record) {
    auto& declared = *base[record];
    return declared.instanceOf ? base[declared.instanceOf]->exported : declared.exported;
}

DeclPtr<RecordType> findRecord(Context& context, Program& program, DeclPtr<Module> from,
                               StringId name, LocationId source) {
    auto found = search<DeclPtr<RecordType>>(context, program, from, name, source,
                                             [&](DeclPtr<Module> in, NameRef reference) {
        return program.findRecord(in, reference.rest());
    });

    return *found;
}

DeclPtr<Alias> findAlias(Context& context, Program& program, DeclPtr<Module> from, StringId name,
                         LocationId source) {
    auto found = search<DeclPtr<Alias>>(context, program, from, name, source,
                                        [&](DeclPtr<Module> in, NameRef reference) {
        return program.findAlias(in, reference.rest());
    });

    return *found;
}

// A constructor is reachable by its own name and by `Record.Constructor` in either case - which is
// the one thing a flat table cannot answer, so it is written out here rather than filed twice.
ConstructorRef findConstructor(Context& context, Program& program, DeclPtr<Module> from,
                               StringId name, LocationId source) {
    auto base = *program.decl;

    auto found = search<ConstructorRef>(context, program, from, name, source,
                                        [&](DeclPtr<Module> in, NameRef reference) -> ConstructorRef {
        if(reference.segments() == 1) return program.findConstructor(in, reference.single());

        // `Record.Constructor`, where the record's own name may itself be namespaced - everything
        // but the last segment is the type, which for the ordinary `Maybe.Just` is one segment.
        auto record = program.findRecord(in, reference.range(0, reference.segments() - 1));
        if(!record) return {};

        auto wanted = reference.segment(reference.segments() - 1);
        auto& declared = *base[record];

        for(U32 i = 0; i < declared.cons.size(); i++) {
            if(declared.cons.get(base, i).name == wanted) return ConstructorRef { record, U16(i) };
        }

        return {};
    });

    return *found;
}

DeclPtr<Class> findClass(Context& context, Program& program, DeclPtr<Module> from, StringId name,
                         LocationId source) {
    auto found = search<DeclPtr<Class>>(context, program, from, name, source,
                                        [&](DeclPtr<Module> in, NameRef reference) {
        return program.findClass(in, reference.rest());
    });

    return *found;
}

DeclPtr<Global> findGlobal(Context& context, Program& program, DeclPtr<Module> from, StringId name,
                           LocationId source) {
    auto found = search<DeclPtr<Global>>(context, program, from, name, source,
                                         [&](DeclPtr<Module> in, NameRef reference) {
        return program.findGlobal(in, reference.rest());
    });

    return *found;
}

/*
 * R1's overload set, through the same traversal every other name goes through - and the one lookup
 * that asks it for **every** reading rather than for the first.
 *
 * The two halves of a set are gathered by two different rules, and `doc/spec/overloading.md` is
 * where both are stated:
 *
 *  - The **plain function** shadows, like every other name. The nearest reading that has one is the
 *    set's, a local declaration beats an import, and two imports offering one are ambiguous. That
 *    is `search`, unchanged.
 *  - The **class functions** join. R5 is that "a plain function joins the overload set rather than
 *    shadowing it", and the reading that makes that rule mean anything is the symmetric one: a
 *    declaration in *this* module may not hide a candidate an import offers either, or a `Num.+`
 *    written in Core would take `%p + n` away from every file in Core that imports `Native` - which
 *    is exactly the shape R5's own example refuses, one module out.
 *
 * So the members are collected from the module and from every visible import, deduplicated, and it
 * is R2 through R5 in `resolve_overload.cpp` that choose between them. An ambiguity between two
 * classes is R4's, answered by qualifying the call rather than by the traversal declining to look.
 *
 * `gather` is called per reading rather than once on a winner, which is what the member half needs
 * and what the plain half is content with: `search` calls `find` after it already has a hit, and
 * discards a reading *after* calling it when the importing side hides the name or the declaring side
 * never exported it, so nothing may be recorded from inside `find`.
 */
void findOverloads(Context& context, Program& program, DeclPtr<Module> from, StringId name,
                   U32 arity, LocationId source, OverloadSet& into, bool report) {
    auto base = *program.decl;

    into.plain = nullptr;
    into.members.clear();

    /*
     * One module's set, with the traversal's own visibility rule applied **per candidate**.
     *
     * Per candidate and not per set is the one thing this adds to the rule: a module may export
     * `min` as a class member and keep a plain one private, and an importer's set is the public
     * half. A reading whose candidates are all private answers empty, which is what makes the search
     * go on to the next import exactly as it does for any other private name.
     */
    auto gather = [&](DeclPtr<Module> in, NameRef reference, OverloadSet& set) {
        set.plain = nullptr;
        set.members.clear();

        /*
         * `Class.member` - R4's qualification, and the one form that names a *single* candidate.
         *
         * "`Bitwise.and(x, y)` selects the class explicitly, in the same form a namespaced type
         * method uses." So an ambiguous name has a spelling that is not ambiguous, which is what
         * makes an overload set usable rather than a dead end - and the ambiguity diagnostic names
         * exactly these spellings.
         *
         * Split the way `findConstructor` splits `Record.Constructor`, for the same reason and by
         * the same `NameRef::range`: everything before the last segment is the class, and the class's
         * own name may itself be namespaced. Tried first, because a class prefix that resolves is
         * unambiguous and there is nothing for the ordinary reading to add to it.
         *
         * `acceptsArity` and not an equality, for the reason that predicate exists: a member with a
         * defaulted position answers a range of counts, and the module's table files it under every
         * one of them. Asking a narrower question here would make `Class.member` reach a member that
         * the bare name does reach, which is the one thing this spelling may not do.
         */
        if(reference.segments() > 1) {
            auto klass = program.findClass(in, reference.range(0, reference.segments() - 1));

            if(klass) {
                auto member = reference.segment(reference.segments() - 1);

                for(auto candidate: base[klass]->members.contents(base)) {
                    auto& declared = *base[candidate];
                    if(declared.name != member || !declared.acceptsArity(base, arity)) continue;

                    set.members.push(candidate);
                    break;
                }
            }
        }

        if(set.isEmpty()) program.findOverloads(in, reference.rest(), arity, set);
        if(in == from) return;

        if(set.plain && !exportedSymbol(base, set.plain)) set.plain = nullptr;

        U32 kept = 0;
        for(U32 i = 0; i < set.members.size(); i++) {
            if(exportedSymbol(base, set.members[i])) set.members[kept++] = set.members[i];
        }

        set.members.resize(kept);
    };

    // The plain half, which shadows. `search` decides about one value, so what it is given is the
    // plain function alone - a reading that has only class candidates is "not here" for this
    // question, and does not stand in the way of an import that has one.
    auto found = search<DeclPtr<Function>>(context, program, from, name, source,
                                           [&](DeclPtr<Module> in, NameRef reference) {
        OverloadSet here;
        gather(in, reference, here);
        return here.plain;
    }, report);

    into.plain = *found;

    /*
     * And the member half, which joins - every reading, deduplicated by declaration.
     *
     * Two readings answering with the same member is the ordinary case rather than an oddity: a
     * module and an import that re-exports it are two readings of one declaration, and a set holding
     * it twice would make R2's "two candidates match" fire on one candidate.
     */
    visitReadings<DeclPtr<Function>>(context, program, from, name,
                                     [&](DeclPtr<Module> in, NameRef reference) {
        OverloadSet here;
        gather(in, reference, here);

        for(auto member: here.members) {
            auto seen = false;
            for(auto existing: into.members) seen = seen || existing == member;
            if(!seen) into.members.push(member);
        }

        // The traversal's own visibility check is asked of what this answers, and `gather` has
        // already applied it per candidate - so answering the first member is answering something
        // already known to be nameable, and answering null is what makes the walk go on.
        return here.members.isEmpty() ? DeclPtr<Function>(nullptr) : here.members[0];
    }, [](DeclPtr<Function>, DeclPtr<Module>) { return true; });
}

DeclPtr<Function> findFunction(Context& context, Program& program, DeclPtr<Module> from,
                               StringId name, U32 arity, LocationId source, bool report) {
    OverloadSet set;
    findOverloads(context, program, from, name, arity, source, set, report);
    return set.sole();
}

/*
 * Whether anything in scope declares a function of this name at any arity.
 *
 * The traversal rather than a direct probe, because the question is about *scope*: a name only
 * another module's private declarations answer is not a name the author called with the wrong number
 * of arguments, and telling them so would be reporting a declaration they cannot see.
 *
 * `Program::functionNamed` is what makes this expressible in one traversal - it answers with a
 * declaration rather than a bool, so the visibility clause has something to ask `pub` about.
 * `search`'s own ambiguity report is off: two modules both having the name is still a yes.
 */
bool hasFunctionName(Context& context, Program& program, DeclPtr<Module> from, StringId name) {
    auto found = search<DeclPtr<Function>>(context, program, from, name, kNullLocation,
                                          [&](DeclPtr<Module> in, NameRef reference) {
        return program.functionNamed(in, reference.rest());
    }, false);

    return bool(found);
}

Fixity findFixity(Context& context, Program& program, DeclPtr<Module> from, StringId name) {
    /*
     * The traversal, used only to get the same module order everything else has.
     *
     * A fixity is not a definition, so a missing one is not an error and two modules declaring the
     * same grouping do not conflict - the ambiguity report is off for that reason rather than
     * because the caller is probing.
     *
     * The one lookup that still insists on a single segment, because an operator cannot be
     * namespaced: its whole point is the bare spelling, and the parser rejects a qualified one where
     * it is written.
     */
    return *search<Fixity>(context, program, from, name, kNullLocation,
                           [&](DeclPtr<Module> in, NameRef reference) -> Fixity {
        if(reference.segments() != 1) return {};

        auto found = program.findFixity(in, reference.single());
        return found ? Fixity { found.unwrap(), true } : Fixity {};
    }, false);
}

} // namespace mir
