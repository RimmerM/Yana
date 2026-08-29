#include "type.h"

namespace mir {

// log2 of a byte count, for the two builtin tables. Only the four widths the language has.
static U32 widthIndex(U8 bits) {
    switch(bits) {
        case 8:  return 0;
        case 16: return 1;
        case 32: return 2;
        case 64: return 3;
        default: return 3;
    }
}

Types::Types(DeclArena& arena): arena(arena) {
    unit = asBase<Type>(new (arena) UnitType() - *arena);
    boolean = asBase<Type>(new (arena) BoolType() - *arena);

    static const U8 widths[4] = { 8, 16, 32, 64 };
    for(U32 i = 0; i < 4; i++) {
        ints[0][i] = asBase<Type>(new (arena) IntType(widths[i], false) - *arena);
        ints[1][i] = asBase<Type>(new (arena) IntType(widths[i], true) - *arena);
    }

    floats[0] = asBase<Type>(new (arena) FloatType(32) - *arena);
    floats[1] = asBase<Type>(new (arena) FloatType(64) - *arena);
}

DeclPtr<Type> Types::intType(U8 bits, bool isSigned) {
    return ints[isSigned ? 1 : 0][widthIndex(bits)];
}

DeclPtr<Type> Types::floatType(U8 bits) {
    return floats[bits == 32 ? 0 : 1];
}

DeclPtr<Type> Types::pointerType(DeclPtr<Type> to) {
    // Keyed by the pointee's offset, which is its identity: the table interns, so two pointers to
    // one logical type are one handle and `sameType` stays a comparison.
    auto entry = pointers.add(to.offset);
    if(!entry.existed) *entry.value = asBase<Type>(new (arena) PointerType(to) - *arena);
    return *entry.value;
}

/*
 * The names that are types without a declaration.
 *
 * Only the widths, `{}` and `Bool`. `Int`, `Long`, `Float` and `Double` are Core aliases over four
 * of these and are resolved as declarations like anything else - which is what keeps a diagnostic
 * and a fixture naming one width rather than two spellings of it.
 *
 * `Bool` is here because a comparison has to answer something and this stage has no declarations to
 * answer with; `BoolType` says what it costs.
 */
DeclPtr<Type> Types::findBuiltin(Context& context, StringId name) {
    if(stringView(context.findName(name)) == "Bool"_v) return boolean;

    struct Builtin { StringView text; U8 bits; bool isSigned; bool isFloat; };
    static const Builtin table[] = {
        { "I8"_v,  8,  true,  false }, { "I16"_v, 16, true,  false },
        { "I32"_v, 32, true,  false }, { "I64"_v, 64, true,  false },
        { "U8"_v,  8,  false, false }, { "U16"_v, 16, false, false },
        { "U32"_v, 32, false, false }, { "U64"_v, 64, false, false },
        { "F32"_v, 32, false, true  }, { "F64"_v, 64, false, true  },
    };

    auto text = context.findName(name);
    for(auto& entry: table) {
        if(stringView(text) != entry.text) continue;
        return entry.isFloat ? floatType(entry.bits) : intType(entry.bits, entry.isSigned);
    }

    return nullptr;
}

StringView typeKindName(Type::Kind kind) {
    switch(kind) {
#define YANA_MIR_TYPE(k, Struct, name) case Type::k: return name;
#include "type.def"
#undef YANA_MIR_TYPE
        case Type::KindCount: break;
    }

    return "?"_v;
}


bool hasTeardown(DeclBase base, DeclPtr<Type> type) {
    switch(base[type]->kind) {
        case Type::Unit:
        case Type::Bool:
        case Type::Int:
        case Type::Float:
        case Type::Pointer:
            return false;
        case Type::KindCount:
            break;
    }

    // Not a fallback. A kind this has no answer for is one that was added without deciding the
    // question, and the safe answer to "does this need destroying" is yes.
    return true;
}

bool isTriviallyCopyable(DeclBase base, DeclPtr<Type> type) {
    switch(base[type]->kind) {
        case Type::Unit:
        case Type::Bool:
        case Type::Int:
        case Type::Float:
            return true;

        // A raw pointer duplicates freely because it owns nothing - that is what makes it the
        // unsafe boundary rather than a reference.
        case Type::Pointer:
            return true;
        case Type::KindCount:
            break;
    }

    return false;
}

String typeText(DeclBase base, DeclPtr<Type> type) {
    if(!type) return String("?");

    auto& logical = *base[type];
    switch(logical.kind) {
        case Type::Unit:    return String("{}");
        case Type::Bool:    return String("Bool");
        case Type::Int:
            return Tritium::format("%@%@", ((IntType&)logical).isSigned ? "I"_v : "U"_v,
                                   U64(((IntType&)logical).bits));
        case Type::Float:
            return Tritium::format("F%@", U64(((FloatType&)logical).bits));
        case Type::Pointer:
            return Tritium::format("%%%@", typeText(base, ((PointerType&)logical).to));
        case Type::KindCount:
            break;
    }

    return String("?");
}

} // namespace mir
