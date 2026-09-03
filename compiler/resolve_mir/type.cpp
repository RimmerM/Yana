#include "decl.h"
#include "../intrinsic/primitive.h"

namespace mir {

// log2 of a byte count, for the fixed-width table. Only the four widths that have one - `WideInt`
// and the target widths are named handles beside it, because neither has a byte count to index by.
static U32 widthIndex(U8 bits) {
    switch(bits) {
        case 8:  return 0;
        case 16: return 1;
        case 32: return 2;
        case 64: return 3;
        default: assertTrue(false); return 3;
    }
}

Types::Types(DeclArena& arena): arena(arena) {
    unit = asBase<Type>(new (arena) UnitType() - *arena);

    static const U8 widths[4] = { 8, 16, 32, 64 };
    for(U32 i = 0; i < 4; i++) {
        ints[0][i] = asBase<Type>(new (arena) IntType(widths[i], false) - *arena);
        ints[1][i] = asBase<Type>(new (arena) IntType(widths[i], true) - *arena);
    }

    /*
     * The four integers `ints` has no row for, because neither indexing reaches them: `WideInt` is
     * fifty-three bits and has no byte count, and the three target widths have no width to be
     * indexed by at all. Those three are built at their **maximum** - see the note on
     * `IntType::bits` for why a target width carries its maximum rather than nothing.
     */
    using intrinsic::TargetWidth;
    auto wordBits = U8(intrinsic::maxBitsOf(TargetWidth::Word, 0));
    auto unitBits = U8(intrinsic::maxBitsOf(TargetWidth::CodeUnit, 0));

    wideInt = asBase<Type>(new (arena) IntType(U8(intrinsic::kWideIntBits), true) - *arena);
    words[0] = asBase<Type>(new (arena) IntType(wordBits, false, TargetWidth::Word) - *arena);
    words[1] = asBase<Type>(new (arena) IntType(wordBits, true, TargetWidth::Word) - *arena);
    codeUnit = asBase<Type>(new (arena) IntType(unitBits, false, TargetWidth::CodeUnit) - *arena);

    floats[0] = asBase<Type>(new (arena) FloatType(32) - *arena);
    floats[1] = asBase<Type>(new (arena) FloatType(64) - *arena);
}

DeclPtr<Type> Types::intType(U8 bits, bool isSigned, intrinsic::TargetWidth target) {
    switch(target) {
        case intrinsic::TargetWidth::Word:     return words[isSigned ? 1 : 0];
        case intrinsic::TargetWidth::CodeUnit: return codeUnit;
        case intrinsic::TargetWidth::Fixed:    break;
    }

    // `WideInt` before the byte-count table, which has no row for it: fifty-three bits are not a
    // whole number of bytes of anything, and that is the property the type exists for.
    if(bits == intrinsic::kWideIntBits) return wideInt;

    return ints[isSigned ? 1 : 0][widthIndex(bits)];
}

/*
 * `@bits(n) T`, interned - see the declaration, which is where the key is argued.
 *
 * Re-checked structurally on a hit for `constType`'s reason: a hash is not an identity.
 */
DeclPtr<Type> Types::refinedInt(DeclBase base, DeclPtr<Type> canonical, U8 bits) {
    auto& original = *(IntType*)base[canonical];

    InternKey key;
    key.mix(canonical.offset).mix(bits);

    auto created = false;
    auto entry = internSlot(refinements, key, created, [&](DeclPtr<Type> existing) {
        auto& held = *(IntType*)(*arena)[existing];
        return held.canonical == canonical && held.bits == bits;
    });

    if(created) {
        *entry = asBase<Type>(new (arena) IntType(bits, original.isSigned, original.target,
                                                  canonical) - *arena);
    }

    return *entry;
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

/*
 * `[T *n]`, interned on the element and the count - the note on `Types::arrayType` in type.h.
 *
 * The two entry points meet here. A quantified count keys on the variable's handle and a written one
 * on the number, and the separator between the two halves of the key is what keeps `[T *4]` and a
 * `[T *n]` whose variable happens to sit at offset four from keying alike - `InternKey`'s own rule,
 * and the structural re-check is what makes a collision cost a comparison rather than a wrong type.
 */
static DeclPtr<Type> internArray(DeclArena& arena, DeclBase base,
                                 HashMap<U64, DeclPtr<Type>>& arrays, DeclPtr<Type> element,
                                 DeclPtr<Type> count, U64 length) {
    InternKey key;
    key.mix(element.offset).next().mix(count.offset).next().mix(length);

    auto created = false;
    auto entry = internSlot(arrays, key, created, [&](DeclPtr<Type> existing) {
        auto& held = *(ArrayType*)base[existing];
        return held.element == element && held.count == count && held.length == length;
    });

    if(created) *entry = asBase<Type>(new (arena) ArrayType(element, count, length) - *arena);
    return *entry;
}

DeclPtr<Type> Types::arrayType(DeclBase base, DeclPtr<Type> element, U64 length) {
    return internArray(arena, base, arrays, element, nullptr, length);
}

/*
 * The same, for a caller holding whatever a substitution produced.
 *
 * A `ConstType` is unwrapped to its number here rather than at each caller, which is what keeps one
 * array type per element-and-length: `[I32 *n]` substituted at `n = 4` and a written `[I32 *4]` are
 * the same array however the four arrived, and the integer type the const argument was declared at
 * is the *parameter's* business and not the array's.
 */
DeclPtr<Type> Types::arrayType(DeclBase base, DeclPtr<Type> element, DeclPtr<Type> count) {
    if(count && base[count]->kind == Type::Const) {
        return arrayType(base, element, ((ConstType*)base[count])->value);
    }

    return internArray(arena, base, arrays, element, count, 0);
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
 * `{I32, I32}` - see the declaration, where the argument for a tuple being a record is written.
 *
 * The key is the field list in order, name and type at each position, with a separator between them
 * so that a differently grouped list of the same words cannot key alike. That is the whole identity:
 * there is no declaration, no module and no arguments, so what is compared on a hash hit is exactly
 * what was mixed in.
 */
RecordType* Types::tupleType(DeclBase base, const RecordField* fields, Size count, bool& created) {
    InternKey key;
    for(Size i = 0; i < count; i++) {
        key.mix(U32(fields[i].name)).mix(fields[i].occurrence.type.offset).next();
    }

    auto entry = internSlot(tuples, key, created, [&](DeclPtr<RecordType> existing) {
        auto& held = *base[existing];
        if(!held.tuple || held.fields.size() != count) return false;

        for(Size i = 0; i < count; i++) {
            auto field = held.fields.get(base, U16(i));
            if(field.name != fields[i].name) return false;
            if(field.occurrence.type != fields[i].occurrence.type) return false;
        }

        return true;
    });

    if(!created) return base[*entry];

    // No name and no module, which is what "anonymous" is here: nothing looks one up, and the
    // printers read the empty name as the instruction to print the fields instead.
    auto record = new (arena) RecordType(StringId(), nullptr, kNullLocation);
    record->tuple = true;

    *entry = record - base;
    return record;
}

/*
 * The names that are types without a declaration - `compiler/intrinsic/primitive.def`.
 *
 * The widths and `{}`, which is all of them: everything the language names that a `data` head could
 * have declared *is* declared, in `lib/Core` and in the prelude beside it. `Bool` was the last one
 * here and is `known.def`'s `boolean` role now.
 *
 * The table is in `compiler/intrinsic` rather than here for the reason everything else there is:
 * these are source-visible strings, and they are the compiler's statement of what the language's own
 * type names mean. What is left in this function is turning that answer into *this* stage's
 * representation, which is the one thing the table cannot do - `resolve_ssa` and a future JavaScript
 * pipeline build different things from the same rows.
 */
DeclPtr<Type> Types::findBuiltin(Context& context, StringId name) {
    auto text = context.findName(name);
    auto found = intrinsic::findPrimitive(StringView { text.text(), text.size() });
    if(!found) return nullptr;

    switch(found->kind) {
        case intrinsic::PrimitiveKind::Unit:  return unit;
        case intrinsic::PrimitiveKind::Int:
            return intType(U8(found->bits), found->isSigned, found->target);
        case intrinsic::PrimitiveKind::Float: return floatType(U8(found->bits));
    }

    return nullptr;
}

StringView intTypeName(const IntType& type) {
    return intrinsic::primitiveIntName(type.bits, type.isSigned, type.target);
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

        /*
         * A fixed array owes exactly what its elements owe, `n` times over.
         *
         * Folded here rather than stored the way a record's is, and the difference is that there is
         * nothing to fold *over*: a record's answer depends on the instances the program declared -
         * an `instance Drop(Pair)` is a later declaration changing an earlier type's answer - and an
         * array's cannot, because there is no declaration for one to be written on. So it is a
         * property of the kind and its element, which is what makes it safe to answer before
         * `classifyTypes` has run.
         *
         * A count of **zero** releases nothing, and that is worth the arm: `[Guard *0]` has no
         * element to run a teardown over and no first address to compute one from, which is exactly
         * what a derived walk would go looking for. A quantified count is not known to be zero and
         * takes the conservative answer.
         */
        case Type::Array: {
            auto& array = *(ArrayType*)base[type];
            auto element = ownershipOf(base, array.element);

            if(array.hasFixedLength() && array.length == 0) return Ownership {};

            // `authored` is deliberately not carried over. A teardown written on the element runs
            // per element, and the walk that runs it is this array's own - which is a *derived*
            // teardown whatever the element's is, and is the distinction `TeardownKind` exists to
            // make: `Authored` means "this type's own body replaces the walk", and an array has no
            // body to put there.
            auto derived = [](TeardownKind kind) {
                return kind == TeardownKind::None ? TeardownKind::None : TeardownKind::Derived;
            };

            // Field by field rather than a positional list, because `reclaim` precedes `drop` in the
            // declaration and the two are interchangeable at a glance - which is one way this was
            // already written wrong, and the symptom was three teardowns silently not running.
            Ownership answer;
            answer.trivialCopy = element.trivialCopy;
            answer.relocatable = element.relocatable;
            answer.explicitDrop = element.explicitDrop;
            answer.reclaim = derived(element.reclaim);
            answer.drop = derived(element.drop);

            return answer;
        }

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
        // `{}` is grammar rather than a name, which is why `primitive.def` has no row for it; every
        // other spelling here is that file's, so a message and a dump name one type one way.
        case Type::Unit:    return String("{}");
        case Type::Int: {
            auto& integer = (IntType&)logical;

            // The refinement as it was written - `printType` spells it the same way, so a message
            // and a dump name one type one way.
            if(integer.canonical) {
                return Tritium::format("@bits(%@) %@", U64(integer.bits),
                                       typeText(context, base, integer.canonical));
            }

            auto name = intTypeName(integer);
            return String(name.ptr, name.size());
        }
        case Type::Float: {
            auto name = intrinsic::primitiveName(intrinsic::PrimitiveKind::Float,
                                                 ((FloatType&)logical).bits);
            return String(name.ptr, name.size());
        }
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
            auto& record = (RecordType&)logical;

            /*
             * A tuple, spelled as its fields - which for an anonymous record is the same decision a
             * function type's spelling is: the field list *is* the type, so there is no name to
             * print instead and printing one would be inventing a declaration.
             */
            if(record.tuple) {
                auto result = String("{");
                auto first = true;

                for(auto field: record.fields.contents(base)) {
                    if(!first) result = result + String(", ");
                    first = false;

                    if(field.name != StringId()) {
                        auto label = stringView(context.findName(field.name));
                        result = result + String(label.ptr, label.size()) + String(": ");
                    }

                    result = result + typeText(context, base, field.occurrence.type);
                }

                return result + String("}");
            }

            // The declared name, which is the whole of a nominal type's spelling. A field list would
            // be describing the declaration rather than naming the type the author wrote.
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
        /*
         * `[I32 *4]`, and `[I32 *n]` where the count is quantified.
         *
         * The count is spelled the way it was written in both cases, which is what a message about a
         * mismatched length has to show: "cannot convert [I32 *4] to [I32 *2]" says the whole of what
         * is wrong, and a spelling that dropped the number would be describing the one property this
         * type exists to carry.
         */
        case Type::Array: {
            auto& array = (ArrayType&)logical;
            auto element = typeText(context, base, array.element);

            if(array.hasFixedLength()) return Tritium::format("[%@ *%@]", element, array.length);
            return Tritium::format("[%@ *%@]", element, typeText(context, base, array.count));
        }

        case Type::KindCount:
            break;
    }

    return String("?");
}

U16 RecordType::constructorOf(DeclBase base, U16 field) {
    for(auto constructor: cons.contents(base)) {
        if(field >= constructor.first && field < U16(constructor.first + constructor.count)) {
            return constructor.index;
        }
    }

    // Every field belongs to the constructor that declared it, so this is a record whose ranges and
    // whose field list disagree - which the declaration walk cannot produce and a reader may not
    // paper over.
    assertTrue(false);
    return 0;
}

RecordLayout recordLayoutOf(DeclBase base, RecordType& record) {
    // One constructor is one shape whether or not it carries anything: `data Marker = Only` has a
    // single value and no number to tell it from, exactly as `data Pair {x: I32}` does.
    if(record.cons.size() <= 1) return RecordLayout::Single;

    for(auto constructor: record.cons.contents(base)) {
        if(constructor.count) return RecordLayout::Multi;
    }

    return RecordLayout::Enum;
}

TagRange tagRangeOf(DeclBase base, RecordType& record) {
    TagRange range;
    if(record.layout == RecordLayout::Single) return range;

    auto constructors = record.cons.contents(base);
    if(!constructors.size()) return range;

    range.lowest = constructors[0].value;
    range.highest = constructors[0].value;

    for(auto constructor: constructors) {
        if(constructor.value < range.lowest) range.lowest = constructor.value;
        if(constructor.value > range.highest) range.highest = constructor.value;
    }

    /*
     * A value below zero takes a whole signed word of its natural width.
     *
     * `doc/spec/declarations.md` says so and says why: a stored tag is otherwise a range of patterns
     * counted from zero, which is what makes it packable and nichable, and neither statement is true
     * of a number whose top bit is its sign. Sized off whichever end is further from zero, since
     * two's complement needs the magnitude and the sign both.
     */
    if(range.lowest < 0) {
        auto magnitude = U64(range.highest > -(range.lowest + 1) ? range.highest
                                                                 : -(range.lowest + 1));
        U8 bits = 1;
        while(bits < 63 && (U64(1) << bits) <= magnitude) bits++;

        range.isSigned = true;
        range.bits = storageBitsFor(U8(bits + 1));
        return range;
    }

    // The narrowest stored width whose patterns reach `highest`. Where nothing was pinned that is
    // the constructor count's own answer, since the values run 0 to count-1.
    U8 bits = 1;
    while(bits < 64 && (U64(1) << bits) <= U64(range.highest)) bits++;

    range.bits = storageBitsFor(bits);
    return range;
}

U8 tagBitsOf(DeclBase base, RecordType& record) {
    return tagRangeOf(base, record).bits;
}

bool isSumRecord(DeclBase base, DeclPtr<Type> type) {
    return type && base[type]->kind == Type::Record &&
           ((RecordType*)base[type])->layout != RecordLayout::Single;
}

DeclPtr<Type> tagTypeOf(DeclBase base, Types& types, DeclPtr<Type> record) {
    if(!record || base[record]->kind != Type::Record) return nullptr;

    auto range = tagRangeOf(base, *(RecordType*)base[record]);
    return range.bits ? types.intType(range.bits, range.isSigned) : nullptr;
}

// The argument is in type.h, beside the declarations.
Size aggregateComponentCount(DeclBase base, DeclPtr<Type> type, U16 constructor) {
    if(base[type]->kind == Type::Array) {
        // A quantified count has no component count: the number arrives with the specialization, and
        // until then there is nothing to build. Zero rather than an assertion because this is what
        // the verifier compares against, and a literal at such a type is refused where it is written
        // - `resolveArrayLiteral`, whose message names the const parameter.
        auto& array = *(ArrayType*)base[type];
        return array.hasFixedLength() ? Size(array.length) : 0;
    }

    return ((RecordType*)base[type])->cons.get(base, constructor).count;
}

Occurrence aggregateComponent(DeclBase base, DeclPtr<Type> type, U16 constructor, Size index) {
    // Owned, and every element the same: an array's elements *are* its storage, so there is no
    // declaration for one of them to have a different contract than another.
    if(base[type]->kind == Type::Array) return owned(((ArrayType*)base[type])->element);

    auto declaration = (RecordType*)base[type];
    auto arm = declaration->cons.get(base, constructor);
    return declaration->field(base, U16(arm.first + index)).occurrence;
}

// The argument is in decl.h, beside the declaration.
DeclPtr<Type> classDefaultType(Program& program, intrinsic::Known role) {
    auto declaration = program.roles.klass(role);
    if(!declaration) return nullptr;

    auto base = *program.decl;
    auto gen = base[declaration]->gen;
    if(!gen || !base[gen]->defaults.size()) return nullptr;

    return base[gen]->defaults.get(base, 0);
}

// Both arguments are in decl.h, beside the declarations.
DeclPtr<Type> booleanType(Program& program) {
    return asBase<Type>(program.roles.record(intrinsic::Known::boolean));
}

Constant truthConstant(Program& program, bool value) {
    return enumConstant(booleanType(program), value ? 1 : 0);
}

bool isSequenceType(DeclBase base, Program& program, DeclPtr<Type> type) {
    auto declaration = program.roles.record(intrinsic::Known::array);
    if(!declaration || !type || base[type]->kind != Type::Record) return false;

    auto& record = *(RecordType*)base[type];
    return record.instanceOf == declaration || &record == base[declaration];
}

DeclPtr<Type> elementTypeOf(DeclBase base, Program& program, DeclPtr<Type> type) {
    if(!type) return nullptr;
    if(base[type]->kind == Type::Array) return ((ArrayType*)base[type])->element;

    if(!isSequenceType(base, program, type)) return nullptr;

    // An instantiation's one argument. The bare declaration has none, and answers null: `Array`
    // written without arguments is not a container of anything in particular.
    auto& record = *(RecordType*)base[type];
    return record.arguments.size() == 1 ? record.arguments.get(base, 0) : nullptr;
}

DeclPtr<OwnershipSchema> schemaOf(DeclBase base, DeclPtr<Type> type) {
    if(!type || base[type]->kind != Type::Record) return nullptr;
    return ((RecordType*)base[type])->schema;
}

} // namespace mir
