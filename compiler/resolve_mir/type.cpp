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

RecordType* Types::recordType(StringId name, DeclPtr<Module> module, LocationId source) {
    return new (arena) RecordType(name, module, source);
}

/*
 * The names that are types without a declaration.
 *
 * The widths, `{}` and `Bool`. `Bool` is here because a comparison has to answer something and this
 * stage has no declarations to answer with; `BoolType` says what it costs.
 *
 * **`Int` and `Long` are temporary and come out when this stage reads Core.** They are `alias Int =
 * I32` and `alias Long = I64` in `lib/Core/Core.yana`, so what a fixture writing one *means* is
 * already decided elsewhere and this table only repeats it - which is the whole reason they can be
 * repeated here without the answer being able to drift. Their purpose is the fixture corpus: most
 * of `test/resolve` spells its integers `Int`, and without these two names every one of those
 * fixtures is unreachable by this stage for a reason that has nothing to do with what the fixture
 * is about. An alias is not a type of its own, so nothing downstream can tell which spelling was
 * written - `typeText` prints `I32` either way, and the goldens say `I32`.
 *
 * They go when milestone 6 makes `alias` a declaration this stage resolves; the two rows are then
 * deleted and the corpus keeps compiling, because Core will be saying the same thing.
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

        // Temporary - see above. Deliberately last, so that deleting the two rows is the whole edit.
        { "Int"_v,  32, true, false }, { "Long"_v, 64, true, false },
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


/*
 * What a type owes, read rather than recomputed.
 *
 * Every kind but `Record` answers a constant, and those constants are here rather than in `own.cpp`
 * because they are properties of the kind: nothing about a `%T` duplicating freely depends on what
 * it points at, which is exactly what makes it the unsafe boundary rather than a reference. A
 * record's answer was folded by `classifyTypes` over its members and the program's instances, and is
 * read off the declaration.
 */
Ownership ownershipOf(DeclBase base, DeclPtr<Type> type) {
    if(!type) return Ownership {};

    switch(base[type]->kind) {
        case Type::Unit:
        case Type::Bool:
        case Type::Int:
        case Type::Float:

        // A raw pointer duplicates freely and releases nothing because it owns nothing - that is
        // what makes it the unsafe boundary rather than a reference.
        case Type::Pointer:
            return Ownership {};

        case Type::Record:
            return ((RecordType*)base[type])->ownership;

        case Type::KindCount:
            break;
    }

    /*
     * Not a fallback. A kind this has no answer for is one that was added without deciding the
     * question, and the safe answer to each of the five is the one that forbids something: it cannot
     * be duplicated, it cannot be moved, and it does have to be released. `explicitDrop` is the one
     * exception and is false, because the conservative reading of an undecided obligation is that
     * the value has none to break - a `true` there would reject every program that used the kind.
     */
    return Ownership { false, false, false, TeardownKind::Derived, TeardownKind::Derived };
}

bool hasTeardown(DeclBase base, DeclPtr<Type> type) {
    return ownershipOf(base, type).needsTeardown();
}

bool isTriviallyCopyable(DeclBase base, DeclPtr<Type> type) {
    return ownershipOf(base, type).trivialCopy;
}

/*
 * Whether this stage follows where a value of this type went - the move-path question.
 *
 * A third question, and deliberately not either of its neighbours. `isLinear` is the *obligation* -
 * something to run, or a discharge the author owes - and is what the SSA tier's convention is keyed
 * off. This one is the *initialization state*: a value that may not be duplicated has to be followed
 * so that a use after it moved is a diagnostic, whether or not anything runs when it dies, and a
 * linear value has to be followed so that the teardown is placed where it actually is.
 *
 * `explicitDrop` needs no term of its own here because a type that owes a disposal is never
 * trivially copyable - `classifyTypes` states that once, where the fold is.
 */
bool isTracked(DeclBase base, DeclPtr<Type> type) {
    auto ownership = ownershipOf(base, type);
    return ownership.needsTeardown() || !ownership.trivialCopy;
}

bool isLinear(DeclBase base, DeclPtr<Type> type) {
    return ownershipOf(base, type).isLinear();
}

bool needsExplicitDisposal(DeclBase base, DeclPtr<Type> type) {
    return ownershipOf(base, type).explicitDrop;
}

bool isRelocatable(DeclBase base, DeclPtr<Type> type) {
    return ownershipOf(base, type).relocatable;
}

String typeText(Context& context, DeclBase base, DeclPtr<Type> type) {
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
            return Tritium::format("%%%@", typeText(context, base, ((PointerType&)logical).to));
        case Type::Record: {
            // The declared name, which is the whole of a nominal type's spelling. A field list would
            // be describing the declaration rather than naming the type the author wrote.
            auto text = stringView(context.findName(((RecordType&)logical).name));

            // A view of the name table's own bytes rather than a copy: an interned name borrows its
            // text and outlives every diagnostic built from it.
            return String(text.ptr, text.size());
        }
        case Type::KindCount:
            break;
    }

    return String("?");
}

DeclPtr<OwnershipSchema> schemaOf(DeclBase base, DeclPtr<Type> type) {
    if(!type || base[type]->kind != Type::Record) return nullptr;
    return ((RecordType*)base[type])->schema;
}

} // namespace mir
