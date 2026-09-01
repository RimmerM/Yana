#include "role.h"
#include "known.h"
#include "op.h"
#include "supplied.h"
#include "provided.h"
#include "primitive.h"

namespace intrinsic {

/*
 * The tables, each in its own enum's order.
 *
 * A row's `role` is `YANA_ROLE_TEXT` of its enumerator and nothing else, so there is no name here
 * for a `.def` to disagree with - which is the property the whole arrangement turns on. Adding a row
 * makes `@builtin` accept its name; renaming one stops the old spelling being accepted, at the
 * declaration, rather than leaving a role quietly unfilled.
 */

const KnownDef knownTable[Size(Known::Count)] = {
#define YANA_KNOWN_ROLE(role, entity, arity, members, standIn, member, teardown, presence) \
    { YANA_ROLE_TEXT(role), entity, arity, members, standIn, member, teardown, presence },
#include "known.def"
#undef YANA_KNOWN_ROLE
};

const OpDef opTable[Size(Op::Count)] = {
#define YANA_INTRINSIC_OP(role, result, ...) makeOp(YANA_ROLE_TEXT(role), result, ##__VA_ARGS__),
#include "op.def"
#undef YANA_INTRINSIC_OP
};

const BuiltinDef builtinTable[kBuiltinCount] = {
#define YANA_SUPPLIED_ROLE(role, kind, shape) { YANA_ROLE_TEXT(role), kind, shape },
#include "supplied.def"
#undef YANA_SUPPLIED_ROLE
};

/*
 * `provided.def` has no rows yet, and a zero-length array is not a type C++ has.
 *
 * So the table is reached only through `providedDef`, which asserts. When the first row arrives this
 * becomes an ordinary array beside the three above; until then, writing one with a dummy element
 * would be a row nothing declared that `findRole` would have to remember to skip.
 */
static const ProvidedDef* providedTable = nullptr;
static constexpr Size providedCount = Size(Provided::Count);

const ProvidedDef& providedDef(Provided role) {
    assertTrue(Size(role) < providedCount);
    return providedTable[Size(role)];
}

StringView roleKindName(RoleKind kind) {
    switch(kind) {
        case RoleKind::Known:    return "a declaration the compiler references"_v;
        case RoleKind::Op:       return "an operation the compiler implements"_v;
        case RoleKind::Supplied: return "a global the compiler fills"_v;
    }

    return "?"_v;
}

StringView entityName(Entity entity) {
    switch(entity) {
        case Entity::Class:       return "a class"_v;
        case Entity::Record:      return "a data declaration"_v;
        case Entity::Constructor: return "a constructor"_v;
        case Entity::Function:    return "a function"_v;
        case Entity::Global:      return "a global"_v;
    }

    return "?"_v;
}

/*
 * The three role tables walked as one, which is what makes the role namespace one namespace.
 *
 * A visitor rather than three copies of the loop, because every reader below wants all three in a
 * fixed order and two of them would otherwise be two chances to leave one out - which is exactly
 * how a fourth table would come to be missing from a diagnostic that claims to list every role the
 * compiler knows.
 *
 * `primitive.def` and `provided.def` are deliberately not here: neither is claimable by an
 * attribute, so neither is in the role namespace - see `RoleKind`.
 */
template<class F>
static void eachRole(F&& visit) {
    for(Size i = 0; i < Size(Known::Count); i++) {
        visit(RoleRef { RoleKind::Known, U16(i) }, knownTable[i].role);
    }

    for(Size i = 0; i < Size(Op::Count); i++) {
        visit(RoleRef { RoleKind::Op, U16(i) }, opTable[i].role);
    }

    for(Size i = 0; i < kBuiltinCount; i++) {
        visit(RoleRef { RoleKind::Supplied, U16(i) }, builtinTable[i].role);
    }
}

Maybe<RoleRef> findRole(StringView name) {
    // A linear scan, asked once per `@builtin` attribute in a program. A map would be a hash per
    // lookup to save a few dozen comparisons of short strings that mostly differ in their first
    // character.
    Maybe<RoleRef> found = Nothing();
    eachRole([&](RoleRef role, StringView text) {
        if(text == name) found = Just(role);
    });

    return found;
}

StringView roleName(RoleRef role) {
    StringView found = "?"_v;
    eachRole([&](RoleRef candidate, StringView text) {
        if(candidate == role) found = text;
    });

    return found;
}

/*
 * A builder's bytes, owned.
 *
 * The builder's are about to go out of scope and a diagnostic holds what it was given until it is
 * printed, so the text has to outlive this - the same shape `operator +` on a String builds, and
 * for the same reason. One copy of it, because both lists below want it and the second would be
 * the one that forgot the `true`.
 */
static Tritium::String owned(Tritium::StringBuilder& list) {
    auto text = (char*)Tritium::hAlloc(list.size());
    Tritium::copy(list.text(), text, list.size());
    return Tritium::String(text, list.size(), true);
}

Tritium::String roleList() {
    Tritium::StringBuilder list;
    auto first = true;

    eachRole([&](RoleRef, StringView text) {
        if(!first) list.append(", ", 2);
        first = false;
        list.append(text);
    });

    return owned(list);
}

Tritium::String suppliedRoleList() {
    Tritium::StringBuilder list;

    for(Size i = 0; i < kBuiltinCount; i++) {
        if(i) list.append(", ", 2);
        list.append(builtinTable[i].role);
    }

    return owned(list);
}

void verifyRoleTable() {
#if defined(_DEBUG) || defined(DEBUG)
    /*
     * No two rows anywhere share a spelling.
     *
     * It cannot be a `static_assert` - the names are string comparisons across three independently
     * included tables - and it must not run in a release build, because a duplicate is a mistake in
     * this directory rather than anything a program can provoke. What it prevents is real: two
     * tables are free to use one word, and the one that answered would be whichever `eachRole`
     * reached first, silently.
     */
    eachRole([&](RoleRef role, StringView text) {
        eachRole([&](RoleRef other, StringView otherText) {
            // Two rows in compiler/intrinsic sharing a role name.
            assertTrue(role == other || text != otherText);
        });
    });
#endif
}

StringView shapeTermName(ShapeTerm term) {
    switch(term) {
        case ShapeTerm::Any:       return "anything"_v;
        case ShapeTerm::Var:       return "a type variable of this signature"_v;
        case ShapeTerm::Pointer:   return "a pointer"_v;
        case ShapeTerm::PointeeOf: return "what that pointer points at"_v;
        case ShapeTerm::PointerTo: return "a pointer to that"_v;
        case ShapeTerm::SameAs:    return "the same type as that"_v;
        case ShapeTerm::Integer:   return "an integer"_v;
        case ShapeTerm::Bool:      return "Bool"_v;
        case ShapeTerm::Unit:      return "{}"_v;
    }

    return "?"_v;
}

StringView shapeContractName(ShapeContract contract) {
    switch(contract) {
        case ShapeContract::Unchecked: return "passed however the declaration likes"_v;
        case ShapeContract::Value:     return "a plain value"_v;
        case ShapeContract::Handover:  return "handed over, written `->`"_v;
        case ShapeContract::Unique:    return "exclusive, written `&`"_v;
        case ShapeContract::Published: return "rooted in a published origin, written `return`"_v;
    }

    return "?"_v;
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
         * The word, and every target has one for the same reason: `targetIntWidths` is total, and a
         * build whose word had no width would be a build with no `Size` in it.
         */
        case Builtin::wordBits: return Just(U64(targetIntWidths(settings).word));

        /*
         * The written ones have no compile-time value at all, and answering zero for them would be
         * a different statement: zero is what one of these globals *holds* in a build where nothing
         * filled it, and it is the declaration's own initializer that says so. What is asked here
         * is what the compiler decides, and about these it decides nothing.
         */
        case Builtin::commandLineCount:
        case Builtin::commandLineValues:
        case Builtin::commandLineEnvironment:
        case Builtin::Count:
            return Nothing();
    }

    return Nothing();
}

const PrimitiveDef primitiveTable[kPrimitiveCount] = {
#define YANA_PRIMITIVE_TYPE(name, kind, bits, isSigned) { name##_v, kind, bits, isSigned },
#include "primitive.def"
#undef YANA_PRIMITIVE_TYPE
};

const PrimitiveDef* findPrimitive(StringView name) {
    for(auto& entry: primitiveTable) {
        if(entry.name == name) return &entry;
    }

    return nullptr;
}

} // namespace intrinsic
