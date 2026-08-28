#include "builtin.h"

/*
 * The table, and the two questions asked of it.
 *
 * One row per role, in the enum's order - the arrays that hold what a program declared are indexed
 * by that enum, so the two orders are the same order and `builtinDef` is a subscript.
 */
const BuiltinDef builtinTable[kBuiltinCount] = {
    { "commandLineCount"_v,       BuiltinKind::Written,  BuiltinShape::Word    },
    { "commandLineValues"_v,      BuiltinKind::Written,  BuiltinShape::Address },
    { "commandLineEnvironment"_v, BuiltinKind::Written,  BuiltinShape::Address },

    { "pageBytes"_v,              BuiltinKind::Supplied, BuiltinShape::Word    },
    { "vectorBytes"_v,            BuiltinKind::Supplied, BuiltinShape::Word    },
};

Maybe<Builtin> findBuiltin(StringView name) {
    // A linear scan of five rows, asked once per `@builtin` attribute in a program. A map of them
    // would be a hash per lookup to save four comparisons.
    for(Size i = 0; i < kBuiltinCount; i++) {
        if(builtinTable[i].role == name) return Just(Builtin(i));
    }

    return Nothing();
}

Tritium::String builtinRoleList() {
    Tritium::StringBuilder list;

    for(Size i = 0; i < kBuiltinCount; i++) {
        if(i) list.append(", ", 2);
        list.append(builtinTable[i].role);
    }

    // Owning its bytes, since the builder's are about to go out of scope - the same shape
    // `operator +` on a String builds, and for the same reason.
    auto text = (char*)Tritium::hAlloc(list.size());
    Tritium::copy(list.text(), text, list.size());
    return Tritium::String(text, list.size(), true);
}

Maybe<U64> builtinValue(const CompileSettings& settings, Builtin which) {
    switch(which) {
        case Builtin::pageBytes: {
            auto page = targetPageBytes(settings);
            return page ? Just(U64(page)) : Nothing();
        }

        /*
         * The register, and every target has one - there is no `? :` here because
         * `targetVectorBytes` answers sixteen for a target it knows nothing else about, JavaScript
         * included. A build that could not name a width would be a build with no `Vec(a)` in it,
         * and the type would be the thing to refuse rather than this.
         */
        case Builtin::vectorBytes: return Just(U64(targetVectorBytes(settings)));

        /*
         * The written ones have no compile-time value at all, and answering zero for them would be
         * a different statement: zero is what one of these globals *holds* in a build where nothing
         * filled it, and it is the declaration's own initializer that says so. What is asked here
         * is what the compiler decides, and about these it decides nothing.
         */
        case Builtin::commandLineCount:
        case Builtin::commandLineValues:
        case Builtin::commandLineEnvironment:
            return Nothing();
    }

    return Nothing();
}
