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
 * R1's overload set, through the same traversal every other name goes through.
 *
 * The traversal answers *one* value per reading, because one value is what visibility and ambiguity
 * are asked about - so what it is given is the set's first candidate, and what it decides is which
 * *reading* of the name this call reaches. The set is then gathered from that reading.
 *
 * Gathered again rather than recorded as the readings go by, and this is the one thing in here that
 * is not obvious. `search` calls `find` after it already has a hit, because two imports answering is
 * an ambiguity it has to report - and it discards a reading *after* calling `find`, when the
 * importing side hides the name or the declaring side never exported it. So a set recorded from
 * inside `find` is the last reading that had candidates, which is not the same thing as the reading
 * that won: `import Second hiding (weigh)` beside an `import First` that answers would leave the set
 * holding `Second`'s. `OverloadReading` carries out what it takes to ask the winner again, which is
 * the module and how much of the name the reading consumed.
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

    auto found = search<OverloadReading>(context, program, from, name, source,
                                         [&](DeclPtr<Module> in, NameRef reference) {
        OverloadSet here;
        gather(in, reference, here);
        if(here.isEmpty()) return OverloadReading {};

        return OverloadReading { here.at(0), reference };
    }, report);

    if(found) gather(found.module, (*found).reference, into);
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
