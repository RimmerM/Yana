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
 * `4 :: I32` - Implementation-Const-Generics.md §2.1's pair.
 *
 * Keyed on the value and the type's offset together, so `4` as an `I32` and `4` as an `I64` are two
 * handles: they are one number and two parameters, and an instance selected for one may not answer
 * for the other. Re-checked structurally on a hit for `intern.h`'s reason - a hash is not an
 * identity, and a 64-bit value mixed into one has room to collide.
 */
DeclPtr<Type> Types::constType(U64 value, DeclPtr<Type> of) {
    InternKey key;
    key.mix(value).mix(of.offset);

    auto created = false;
    auto entry = internSlot(constants, key, created, [&](DeclPtr<Type> existing) {
        auto& held = *(ConstType*)(*arena)[existing];
        return held.value == value && held.type == of;
    });

    if(created) *entry = asBase<Type>(new (arena) ConstType(value, of) - *arena);
    return *entry;
}

/*
 * `(A, B) -> C`, interned on the whole shape - the note on `Types::funType` in type.h.
 *
 * The key mixes each position's three fields and a separator between them, which is `InternKey`'s
 * own rule about variable-length keys: without one, a two-parameter shape and a one-parameter shape
 * whose fields happen to line up would key alike, and the structural re-check would then be doing
 * the whole of the work.
 */
DeclPtr<Type> Types::funType(DeclBase base, const FunParam* params, Size count,
                             const Occurrence& result) {
    InternKey key;
    for(Size i = 0; i < count; i++) {
        auto& occurrence = params[i].occurrence;
        key.mix(occurrence.type.offset).mix(occurrence.contract.packed)
           .mix(occurrence.shape.offset).next();
    }

    key.mix(result.type.offset).mix(result.contract.packed).mix(result.shape.offset);

    auto created = false;
    auto entry = internSlot(functions, key, created, [&](DeclPtr<Type> existing) {
        auto& held = *(FunType*)base[existing];
        if(held.params.size() != count || held.result != result) return false;

        for(Size i = 0; i < count; i++) {
            if(held.params.get(base, i).occurrence != params[i].occurrence) return false;
        }

        return true;
    });

    if(created) {
        auto type = new (arena) FunType();
        for(Size i = 0; i < count; i++) type->params.push(arena, params[i]);
        type->result = result;
        *entry = asBase<Type>(type - *arena);
    }

    return *entry;
}

RecordType* Types::recordType(StringId name, DeclPtr<Module> module, LocationId source) {
    return new (arena) RecordType(name, module, source);
}

RecordType* Types::instantiate(DeclBase base, DeclPtr<RecordType> declaration, DeclPtr<Type>* args,
                               Size count, bool& created) {
    InternKey key;
    key.mix(declaration.offset);
    for(Size i = 0; i < count; i++) key.mix(args[i].offset);

    auto entry = internSlot(instantiations, key, created, [&](DeclPtr<RecordType> existing) {
        auto& held = *base[existing];
        if(held.instanceOf != declaration || held.arguments.size() != count) return false;

        for(Size i = 0; i < count; i++) {
            if(held.arguments.get(base, i) != args[i]) return false;
        }

        return true;
    });

    if(!created) return base[*entry];

    auto& written = *base[declaration];
    auto record = new (arena) RecordType(written.name, written.module, written.source);
    record->instanceOf = declaration;
    record->local = written.local;
    record->writtenExplicitDrop = written.writtenExplicitDrop;

    for(Size i = 0; i < count; i++) record->arguments.push(arena, args[i]);

    // Registered before its fields exist, which is what makes a recursive generic reach a handle
    // rather than substitute forever - see the note on the declaration.
    *entry = record - base;
    return record;
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
 * `alias` is now a declaration this stage resolves, so nothing about the *mechanism* is missing -
 * what is still missing is Core itself, which is where the two are written. The rows go when this
 * stage reads it: they are then deleted and the corpus keeps compiling, because Core says the same
 * thing.
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

        /*
         * An unconstrained type variable - Implementation-Generics.md §1's first invariant, as the
         * one answer that decides how every generic body is compiled.
         *
         * "An unconstrained `a` is treated as non-`TrivialCopy` even when one call substitutes
         * `Int`." A generic function has one fixed semantic body, so what the body may assume about
         * `a` is what *every* substitution supplies, which is nothing: it may not be duplicated
         * without a `Copy` the context does not promise, and it has to be assumed to release
         * something so that the teardown is placed where the value actually dies. A specialization
         * then supplies a type with nothing to run and emits nothing - the placement is the same and
         * the code is not, which is the direction the invariant permits.
         *
         * `relocatable` is the one that is **true**, and deliberately. Every other field here is
         * conservative because being wrong the other way silently removes an obligation; this one is
         * the reverse - a body that may not move what it owns cannot hand a parameter on at all, so
         * a false here would make every generic function unwritable rather than making an unsound
         * one compile. What withholds relocation is `@pinned`, which is a *removal* and therefore
         * has to be visible at the declaration; a constraint carrying it is what a context needs
         * before this may say no, and until then there is no pinned type to substitute.
         *
         * `explicitDrop` is false on the same reading the fallback below gives: the conservative
         * answer to an undecided obligation is that there is none to break.
         */
        case Type::Variable:
            return Ownership { false, true, false, TeardownKind::Derived, TeardownKind::Derived };

        /*
         * A number written where a type is, and no value has one.
         *
         * Stated rather than left to the fallback below, which is deliberately the answer that
         * forbids something: there is nothing here to forbid, so the conservative reading would be
         * describing a value that does not exist. Every position that accepts a `ConstType` is a
         * type *argument*, and `requireValueType` is what keeps it out of the ones that are not.
         */
        case Type::Const:
            return Ownership {};

        /*
         * A function value - two words, `{code, env}`, and the second is *owned*.
         *
         * So it duplicates no more freely than the captures behind it do: copying the pair would
         * make two owners of one environment, and the one that ran its teardown second would
         * release storage the first had already handed back.
         *
         * **No teardown, and that is a statement about placement rather than about function
         * values.**
         * An environment is a local of the frame that built the closure (`resolve_lambda.cpp`), it
         * holds copies and borrows, and neither owes anything: a copy is trivial by the rule that
         * chose it and a borrow is somebody else's. The capture that *would* make an environment own
         * something is the spec's **consumed** one - and a closure that consumes a capture is
         * precisely a closure that has to outlive the frame it was built in, which is the heap
         * placement `verifyBody` refuses for want of an allocator. So the two arrive together or not
         * at all, and until they do a function value releases nothing and a teardown that went
         * looking for a release routine would read a word nothing writes.
         *
         * `relocatable` is true. Nothing points *into* a function value - the environment is
         * reached through the pointer the pair holds, not through the pair's own address - so the
         * pair may be moved like any other two words.
         *
         * `trivialCopy` is false all the same, and the two are not the same question. A closure may
         * hold an exclusive loan of a capture, and duplicating the bytes would be a second exclusive
         * capability over one object - so its uses are moves, which is what `isTracked` reads this
         * for and the reason the flags are separate fields.
         */
        case Type::Fun:
            return Ownership { false, true, false, TeardownKind::None, TeardownKind::None };

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

// A view of static text as a `String`, which is what `typeText` builds with. The bytes are a
// literal's and outlive every message, so a view is enough and a copy would be a copy per mark.
static String stringText(StringView text) { return String(text.ptr, text.size()); }

/*
 * How a position's contract is spelled in a printed function type.
 *
 * The mark a bare spelling would *not* have meant, which is why the position's own default is an
 * argument: a parameter borrows and a result owns, so `(I32) -> Guard` is the pair everyone writes
 * and prints unmarked, and `(->Guard) -> I32` prints the one mark that is news.
 */
StringView conventionMark(Contract contract, Access written) {
    if(contract.access() == written) return ""_v;

    switch(contract.access()) {
        case Access::Owned:  return "->"_v;
        case Access::Shared: return "'"_v;
        case Access::Unique: return "&"_v;
    }

    return ""_v;
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
        case Type::Const:
            // The number alone - `printType`'s reasoning, and the same answer, so that a message
            // and a dump spell one type one way.
            return Tritium::format("%@", ((ConstType&)logical).value);
        case Type::Variable: {
            // The written name, which is what the author has to recognize. It says nothing about
            // which declaration bound it, and that is right for a message: a diagnostic about `a`
            // is always about the signature it is quoting.
            auto text = stringView(context.findName(((VariableType&)logical).name));
            return String(text.ptr, text.size());
        }
        case Type::Fun: {
            // `(I32, ->Guard) -> I32` - the shape, which is the whole of a structural type's
            // spelling. The conventions are printed because they are part of the identity: two
            // shapes that differ only in one of them accept different calls.
            auto& fun = (FunType&)logical;
            auto result = String("(");
            auto first = true;

            for(auto param: fun.params.contents(base)) {
                if(!first) result = result + String(", ");
                first = false;
                result = result +
                         stringText(conventionMark(param.occurrence.contract, Access::Shared)) +
                         typeText(context, base, param.occurrence.type);
            }

            return result + String(") -> ") +
                   stringText(conventionMark(fun.result.contract, Access::Owned)) +
                   typeText(context, base, fun.result.type);
        }
        case Type::Record: {
            // The declared name, which is the whole of a nominal type's spelling. A field list would
            // be describing the declaration rather than naming the type the author wrote.
            auto& record = (RecordType&)logical;
            auto text = stringView(context.findName(record.name));

            // A view of the name table's own bytes rather than a copy: an interned name borrows its
            // text and outlives every diagnostic built from it.
            auto name = String(text.ptr, text.size());
            if(!record.arguments.size()) return name;

            // `Boxed(I32)` - the arguments, because the name alone would call every instantiation of
            // one declaration the same thing, and telling two of them apart is the whole reason a
            // message about a generic type is being written.
            auto result = name + String("(");
            auto first = true;

            for(auto argument: record.arguments.contents(base)) {
                if(!first) result = result + String(", ");
                first = false;
                result = result + typeText(context, base, argument);
            }

            return result + String(")");
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
