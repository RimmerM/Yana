#include "expand_internal.h"

namespace mir {

using namespace intrinsic;

/*
 * The roles a container asks the compiler about its own key or element type - `lib/Core/Map.yana`.
 *
 * One family so far, and what it has in common is the question rather than the node: each of these
 * is a *number the language cannot compute about itself*, because what decides it is a property of
 * a type rather than of any value. So the expansion is a constant, folded at the call, and the
 * declaration beside it in the library is what says which number is being asked for.
 *
 * Nothing here reads a field of a container. A key's type comes off the *type arguments* of the
 * record the pointer names, which is the same thing the source wrote two lines above the call; the
 * layout of an `Entry(k, v)` is not asked and could not be answered here.
 */

/*
 * `scanLimitFor(entries)` and `scanLimitForKeys(keys)` - how far a map's probe may scan from a slot
 * before the map builds an index instead, Implementation-Map.md §2 and §4.4.
 *
 * A fact about the *allocation* rather than about the map: the index run ends in a cloned copy of
 * its first group, so a group-wide read at any slot stays inside it. What the number depends on is
 * how expensive the key's hash is, and that is a question about the key's type - **8** for a machine
 * word, whose hash is a multiply and a shift; **32** for a string, whose hash is a walk over its
 * units; **64** for anything structural, whose hash is that walk plus a fold per field.
 *
 * Two roles because the two targets do not hold a key in the same place - native reaches it through
 * the `Entry` it is a field of, JS has a run of bare keys - and which of the two a caller uses is
 * `@platform`'s answer at `scanLimitOf` rather than this function's. One emitter and a column, on
 * the terms every other pair in this directory shares one: what differs is where the key is, and
 * `unwrapEntry` is that.
 *
 * A key this stage cannot see through - a type variable in an erased body - takes the largest, which
 * is a slower small map rather than a wrong one: the threshold decides where a scan stops paying,
 * and both sides of it answer the same questions.
 */
Node* expandScanLimit(Builder& builder, Function& callee, bool unwrapEntry, Node* const* arguments,
                      const Substitution& with, LocationId source) {
    auto base = builder.declBase();
    auto held = valueOccurrence(*arguments[0]).type;

    auto pointee = held && base[held]->kind == Type::Pointer ? ((PointerType*)base[held])->to
                                                             : DeclPtr<Type>();

    /*
     * The key of an `Entry(k, v)`, taken from the instantiation's **first type argument**.
     *
     * The declaration two lines above the call in `lib/Core/Map.yana` is `data Entry(k, v)`, so the
     * key is argument zero of the record the pointer names - a fact about the type's parameter list
     * and not about its layout. `compiler/resolve` read the first *field* instead, which is the
     * same answer only for as long as nobody reorders the record.
     */
    auto key = pointee;

    if(unwrapEntry) {
        key = nullptr;

        if(pointee && base[pointee]->kind == Type::Record) {
            auto& record = *(RecordType*)base[pointee];
            if(record.arguments.size()) key = record.arguments.get(base, 0);
        }
    }

    auto word = key && (base[key]->kind == Type::Int || base[key]->kind == Type::Float);

    // The string, through its role rather than by name: the library calls it `String` and could
    // call it anything. The declaration as well as an instantiation of it, since a `String` is
    // concrete and reaches this either way.
    auto text = builder.program.roles.record(intrinsic::Known::string);
    auto record = key && base[key]->kind == Type::Record ? (RecordType*)base[key] : nullptr;
    auto isString = record && text &&
                    (record == base[text] || base[record->instanceOf] == base[text]);

    U64 limit = word ? 8 : isString ? 32 : 64;

    auto result = substituteType(builder.context, builder.program, callee.result.type, with);
    return builder.addConst(source, intConstant(result, limit));
}

} // namespace mir
