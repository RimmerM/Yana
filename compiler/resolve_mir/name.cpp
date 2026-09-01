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

DeclPtr<Function> findFunction(Context& context, Program& program, DeclPtr<Module> from,
                               StringId name, U32 arity, LocationId source, bool report) {
    auto found = search<DeclPtr<Function>>(context, program, from, name, source,
                                           [&](DeclPtr<Module> in, NameRef reference) {
        return program.findFunction(in, reference.rest(), arity);
    }, report);

    return *found;
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
