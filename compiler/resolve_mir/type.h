#pragma once

#include "region.h"

namespace mir {

struct Module;

/*
 * A logical type - Analysis-Borrows-Revised.md §1.1.
 *
 * This is the first of the three layers that document separates, and the separation is the whole
 * reason the type table is this small. A logical type is what generics substitute, what instance
 * selection sees and what a diagnostic names. It carries no ownership, no origin, no access form and
 * no target representation: those are `Contract` (§1.2) and the manifestation table (§1.3), and a
 * decision that reads one of them may never reach for a second `Type` instead.
 *
 * That is what the current resolver's `BorrowType` and slice sibling types are, and why they are not
 * reproduced here: a shared `String` and an owned `String` are one logical type with two contracts,
 * so `String` has one set of instances and one substitution however it is passed.
 *
 * Interned in the declaration region, so type identity is pointer identity and `sameType` is `==`.
 * A kind is added by adding a row to type.def and a case to the three switches it generates.
 */
struct Type {
    enum Kind: U8 {
#define YANA_MIR_TYPE(kind, Struct, name) kind,
#include "type.def"
#undef YANA_MIR_TYPE
        KindCount,
    };

    explicit Type(Kind kind): kind(kind) {}

    Kind kind;
};

// The unit type `{}`: exactly one value, no bytes.
struct UnitType: Type {
    UnitType(): Type(Unit) {}
};

/*
 * `Bool` - the two-valued type a comparison answers and `if` consumes.
 *
 * Builtin here and declared in `Core.yana` as `data Bool = False | True`, which is a difference this
 * milestone has and the language does not. It is a row of its own rather than a one-bit integer
 * because instance selection genuinely distinguishes it - `Show(Bool)` is not `Show(U8)` - and
 * because a condition that accepted any integer would make `if 1 then` a legal program here and an
 * error everywhere else. When data declarations arrive it becomes the enum it always was, and what
 * this row buys in the meantime is that nothing above the manifestation table learns a width for it.
 */
struct BoolType: Type {
    BoolType(): Type(Bool) {}
};

/*
 * An integer of a stated width and signedness.
 *
 * The width is in bits and is one of 8/16/32/64 for now. It is *not* the register width the value
 * arrives in - that is a manifestation question - and it is not a declared range either; a refined
 * width belongs on the contract when refinement is implemented, so that `I32` stays one type.
 */
struct IntType: Type {
    IntType(U8 bits, bool isSigned): Type(Int), bits(bits), isSigned(isSigned) {}

    U8 bits;
    bool isSigned;
};

// A binary floating-point number of 32 or 64 bits.
struct FloatType: Type {
    explicit FloatType(U8 bits): Type(Float), bits(bits) {}

    U8 bits;
};

/*
 * A raw pointer - the `%T` of the source language, and outside the safe alias model.
 *
 * The pointee is a logical type; whether a pointer carries a provenance origin set is a property of
 * its *contract* rather than of this type (Analysis-Borrows-Revised.md §4.1), which is why there is
 * no origin field here.
 */
struct PointerType: Type {
    explicit PointerType(DeclPtr<Type> to): Type(Pointer), to(to) {}

    DeclPtr<Type> to;
};

/*
 * How much of a type's lifetime end is its own code - `doc/spec/ownership.md`'s Teardown.
 *
 *  - `None` is nothing to run.
 *  - `Derived` is the compiler's own walk: release each member, then this value's own storage.
 *  - `Authored` is an `instance Drop(T)` or `instance Reclaim(T)`, which replaces the derived walk
 *    for that half rather than running beside it.
 *
 * Two halves and not one, from the first type that has either, because they are elided under
 * different conditions and conflating them costs a real capability: `Reclaim` compiles to nothing on
 * JavaScript and is discharged in bulk by a region, and `Drop` is never elided anywhere. A single
 * combined answer makes `Map(String, Connection)` region-ineligible for the sake of a release the
 * region was going to do anyway.
 */
enum class TeardownKind: U8 {
    None,
    Derived,
    Authored,
};

/*
 * What a type owes - `doc/spec/ownership.md`'s ownership classes, as the three questions this stage
 * actually asks.
 *
 * Folded over the members and over the instances the program declared, which is why it is a stored
 * answer rather than a function of the kind: it is the one classification a *later declaration* can
 * change, and a query that recomputed it would answer differently depending on when it was asked.
 * `classifyTypes` is the one pass that fills it, after every declaration is in.
 *
 * `trivialCopy` is not the negation of `needsTeardown`, even though the two agree on every type that
 * exists so far - a type may have nothing to release and still refuse duplication. They are separate
 * fields so that the first type where they differ changes one of them.
 */
struct Ownership {
    bool trivialCopy = true;
    TeardownKind reclaim = TeardownKind::None;
    TeardownKind drop = TeardownKind::None;

    bool needsTeardown() const {
        return reclaim != TeardownKind::None || drop != TeardownKind::None;
    }
};

/*
 * One field of a record.
 *
 * The logical type alone, and no contract: a field of an owned record is owned and a field of a
 * shared one is shared, which is a property of the *place* that reaches it rather than of the
 * declaration - see `Builder::fieldPlace`, where the one rule lives. A field that names an origin
 * slot is an interior position of the record's ownership schema (§5.11) and is not written yet.
 */
struct RecordField {
    StringId name {};
    DeclPtr<Type> type = nullptr;
    LocationId source = kNullLocation;
    U16 index = 0;
};

/*
 * A record type - see type.def, where the nominal argument is written out.
 *
 * `ownership` is filled by `classifyTypes` once every declaration and every instance is in, and is
 * read through `ownershipOf` and never directly: a reader that took the field would be reading it
 * before the pass in exactly the cases where the answer is not yet final.
 */
struct RecordType: Type {
    RecordType(StringId name, DeclPtr<Module> module, LocationId source):
        Type(Record), name(name), module(module), source(source) {}

    StringId name {};
    DeclPtr<Module> module;
    LocationId source = kNullLocation;

    // Non-embedding: `SmallList`'s embedded encoding is for elements of four bytes or fewer, and a
    // field is a name, a type and a position.
    DeclList<RecordField, false> fields;

    Ownership ownership;

    // Set by `classifyTypes` when it has folded this record. Not a cache - the fold runs exactly
    // once, and this is what a cycle in the member graph is detected with.
    bool classified = false;

    RecordField field(DeclBase base, U16 index) { return fields.get(base, index); }
};

/*
 * The interned table, one per compilation.
 *
 * Held by value on the program rather than allocated in the region it allocates into, because it
 * needs a hash map and a hash map is not something a bump allocator can hold. The builtins are
 * materialized once at construction so that a lookup of `I32` is an array read: the width names are
 * the ones the language actually has, and Core's `Int`/`Long`/`Float`/`Double` are aliases over
 * them rather than types of their own.
 */
struct Types {
    explicit Types(DeclArena& arena);

    DeclArena& arena;

    DeclPtr<Type> unit;
    DeclPtr<Type> boolean;

    // Indexed by [signed][log2(bytes)], so [1][2] is I32. Materialized in the constructor.
    DeclPtr<Type> ints[2][4];

    // Indexed by log2(bytes) - 2, so [0] is F32 and [1] is F64.
    DeclPtr<Type> floats[2];

    DeclPtr<Type> intType(U8 bits, bool isSigned);
    DeclPtr<Type> floatType(U8 bits);
    DeclPtr<Type> pointerType(DeclPtr<Type> to);

    /*
     * A record, made fresh for one declaration - deliberately *not* interned.
     *
     * Nominal identity is what the row in type.def argues for: two `data` declarations with
     * identical fields are two types. It goes through this table anyway so that every logical type
     * in the program is made in one place, which is what a later structural kind - a tuple, which
     * does intern - has to sit beside without the two ways of making a type diverging.
     */
    RecordType* recordType(StringId name, DeclPtr<Module> module, LocationId source);

    // The type a source name spells, or null. Only the widths and `{}` are builtin names; every
    // other type comes from a declaration, and Core's aliases are declarations.
    DeclPtr<Type> findBuiltin(Context& context, StringId name);

private:
    HashMap<U32, DeclPtr<Type>> pointers;
};

// The concrete type a kind may be cast to, from type.def, so that a kind and its struct are one
// statement rather than two that agree.
template<class F>
auto visitType(Type& type, F&& f) -> decltype(f((UnitType&)type)) {
    switch(type.kind) {
#define YANA_MIR_TYPE(kind, Struct, name) case Type::kind: return f((Struct&)type);
#include "type.def"
#undef YANA_MIR_TYPE
        case Type::KindCount: break;
    }

    assertTrue(false);
    return f((UnitType&)type);
}

// What this kind is called in a dump and in a diagnostic.
StringView typeKindName(Type::Kind kind);

// Whether two logical types are the same. Pointer identity, because the table interns; the function
// exists so that a caller states what it is asking rather than comparing handles by hand.
inline bool sameType(DeclPtr<Type> a, DeclPtr<Type> b) { return a == b; }

/*
 * What this type owes, in one place - `own.cpp`.
 *
 * One function rather than the current compiler's three - `isMemoryType`, a droppability flag and an
 * escape analysis - which have disagreed. Every kind but `Record` answers a constant; a record
 * answers what `classifyTypes` folded for it, which is why this reads the stored field rather than
 * recomputing: the fold depends on the instances the program declared, so a query that ran it would
 * answer differently before and after the declaration walk.
 */
Ownership ownershipOf(DeclBase base, DeclPtr<Type> type);

// Whether a value of this type has anything to run when it dies - either half of the teardown.
bool hasTeardown(DeclBase base, DeclPtr<Type> type);

/*
 * Whether a second owner may be made by copying the bytes, with no call and no observable effect.
 *
 * Not the negation of `hasTeardown`: a type may have no teardown and still refuse duplication - a
 * unique token, a handle whose whole point is that there is one - and the evidence
 * `UseMode::CopyTrivial` claims is *this* one. Two fields on `Ownership` so that the first type
 * where they differ changes one of them.
 */
bool isTriviallyCopyable(DeclBase base, DeclPtr<Type> type);

/*
 * A type as the text a diagnostic names it by.
 *
 * `printType` writes into a `Net::Writer`, which is the dump's interface rather than a message's, so
 * the short spellings are written out separately - but *once*: the resolver used to carry its own
 * copy of this switch, which is two answers to "what is this type called" that a new kind changes
 * one of. The `Context` is what a nominal type needs, since a record's name is a `StringId`.
 */
String typeText(Context& context, DeclBase base, DeclPtr<Type> type);

} // namespace mir
