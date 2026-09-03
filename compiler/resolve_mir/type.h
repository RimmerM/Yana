#pragma once

#include "constant.h"
#include "../intrinsic/convert.h"
#include "../intrinsic/primitive.h"
#include "../intrinsic/constructor.h"

namespace mir {

struct Module;
struct GenContext;
struct Program;

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
 * An integer of a width and a signedness - `compiler/intrinsic/primitive.def`, which is the roster.
 *
 * The width is in bits. It is *not* the register width the value arrives in - that is a
 * manifestation question.
 *
 * `canonical` is `@bits(n)`'s whole representation - the type this one refines, and null for a type
 * whose width is its own. `doc/spec/repr.md`: a refinement is a distinct type because its *storage*
 * differs, and it is invisible to everything else because a value read out of that storage is the
 * canonical integer. So `bits` here means "how many of the storage's bits this value owns", which is
 * the same thing it means for `WideInt`, and the arithmetic width is the canonical's.
 *
 * `target` is set for the three widths the machine picks rather than the language, and `bits` then
 * holds the **maximum** they may turn out to be. The maximum rather than a sentinel deliberately: a
 * reader that has not been audited for target widths then treats a `Size` as the sixty-four-bit
 * integer it really is on the only native target this compiler emits for, so it goes wrong on a
 * *narrower* target - which is a build that fails visibly - rather than reading a zero width and
 * laying out a value of no bytes. `minBits`/`maxBits` are the two readings that hold here; what the
 * width actually *is* is a question only the representation layer may ask, and `resolve_ssa`'s
 * `manifestNative` is the one place that asks it.
 *
 * Identity is still handle identity: `Size` and `I64` are two types however wide the word turns out,
 * which is the whole point of the row - a name for a width is how a thirty-two-bit machine comes to
 * be handed a sixty-four-bit index type.
 */
struct IntType: Type {
    IntType(U8 bits, bool isSigned, intrinsic::TargetWidth target = intrinsic::TargetWidth::Fixed,
            DeclPtr<Type> canonical = nullptr):
        Type(Int), canonical(canonical), bits(bits), isSigned(isSigned), target(target) {}

    // The narrowest this type is guaranteed to be, and the widest it may turn out to be. Equal for
    // everything that states its own width; see `intrinsic::minBitsOf` for which to ask when.
    U8 minBits() const { return U8(intrinsic::minBitsOf(target, bits)); }
    U8 maxBits() const { return bits; }

    bool isTargetWidth() const { return target != intrinsic::TargetWidth::Fixed; }

    // This type as the shared conversion rules see it - `intrinsic/convert.h`, which is where which
    // class relates a pair of numbers is decided, once, for both resolve stages.
    intrinsic::Numeric numeric() const { return intrinsic::integerOf(bits, isSigned, target); }

    // The unrefined type, or null - see the note above. `canonicalInt` is what reads it, because
    // "the type this one dispatches as" is the question every caller has and a null is not it.
    DeclPtr<Type> canonical;

    U8 bits;
    bool isSigned;
    intrinsic::TargetWidth target;
};

/*
 * What an integer type dispatches, computes and converts as - itself, or what a `@bits(n)` refines.
 *
 * The one reader of `IntType::canonical`, so that "a refinement acts as its base" is one statement
 * rather than a null check repeated at every site that has to know. Total over every type, and the
 * identity for all but one kind, because that is what lets a caller apply it without first asking
 * what it is holding.
 */
inline DeclPtr<Type> canonicalInt(DeclBase base, DeclPtr<Type> type) {
    if(!type || base[type]->kind != Type::Int) return type;

    auto canonical = ((IntType*)base[type])->canonical;
    return canonical ? canonical : type;
}

// Whether this is a `@bits(n)` refinement - the question `canonicalInt` cannot answer, since a
// refinement of a type and that type are equally "themselves" once it has run.
inline bool isRefinedInt(DeclBase base, DeclPtr<Type> type) {
    return type && base[type]->kind == Type::Int && ((IntType*)base[type])->canonical;
}

/*
 * A number as this integer type holds it - the low `bits` of it, sign-extended where the type is
 * signed.
 *
 * The **normal form**, which `resolve_ssa/lower_width.cpp` states for a value in a register and this
 * applies to a constant: a value of a type that does not fill its register is held sign- or
 * zero-extended, and every reader below is entitled to assume it. A constant is the one producer
 * that could put a number outside the range in without an operation having done it, which is why the
 * reduction is here rather than left to the store that eventually truncates - a value that is only
 * repaired by a round trip through storage is wrong until it makes one.
 *
 * A **target-answered** width is left alone, for the reason `IntType` states about its own field:
 * how wide a `Size` is is a question only the representation layer may ask, and reducing to the
 * maximum it may turn out to be would be normalizing at a width no target necessarily has.
 */
inline U64 reducedToWidth(const IntType& type, U64 value) {
    if(type.isTargetWidth()) return value;

    auto bits = U32(type.bits);
    if(bits >= 64) return value;

    auto mask = (U64(1) << bits) - 1;
    value &= mask;

    // A signed type's high bit is its sign, so a value that has it set is the negative number it
    // stands for and not the small positive one the mask left behind.
    if(type.isSigned && (value & (U64(1) << (bits - 1)))) value |= ~mask;

    return value;
}

/*
 * Whether a written number is a value of this integer type.
 *
 * The magnitude and the sign are separate because that is how a number is *written* - see
 * `WrittenLiteral`, where the same split is argued from the other end.
 *
 * The **guaranteed** capacity, because a written literal has to be a value of the type on every
 * target: `let n: Size = 3000000000` is not a program, however wide the machine being built for
 * happens to be.
 */
inline bool integerHolds(const IntType& type, U64 magnitude, bool negative) {
    auto bits = U32(type.minBits());
    if(bits == 0) return magnitude == 0;
    if(negative && !type.isSigned) return magnitude == 0;

    auto width = bits - (type.isSigned ? 1u : 0u);
    if(width >= 64) return true;

    auto limit = (U64(1) << width) - 1;
    return magnitude <= (negative ? limit + 1 : limit);
}

// A binary floating-point number of 32 or 64 bits.
struct FloatType: Type {
    explicit FloatType(U8 bits): Type(Float), bits(bits) {}

    intrinsic::Numeric numeric() const { return intrinsic::floatOf(bits); }

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
 * One type variable of one generic context - `gen.h`.
 *
 * `owner` and `index` together are its identity, and the pair is what substitution matches on: a
 * variable is replaced only by the environment of the declaration that bound it, so a body resolved
 * inside `f` cannot have `g`'s `a` substituted underneath it. The name is carried for diagnostics
 * and dumps alone and is never compared - two contexts may both call their first variable `a`, and
 * they are two variables.
 *
 * `source` is where the variable was first written, which *is* its binder: a function declares its
 * variables by using them, so the first occurrence is the declaration.
 */
struct VariableType: Type {
    VariableType(DeclPtr<GenContext> owner, U16 index, StringId name, LocationId source):
        Type(Variable), owner(owner), name(name), source(source), index(index) {}

    DeclPtr<GenContext> owner;
    StringId name {};
    LocationId source = kNullLocation;
    U16 index = 0;

    /*
     * `n: I32` - the annotation that makes this a **const** parameter, and null for a type one.
     *
     * A field here rather than a second list on the context, because a const parameter is a
     * *position* of the same parameter list: `data Buf(a, n: I32)` binds two things, an application
     * supplies two arguments, and arity, defaults, matching and substitution are one rule for both.
     * A separate list would have made every one of those two rules that agree until one is changed.
     *
     * What it changes is what may bind here - a `ConstType` of exactly this type rather than any
     * type at all, which `matchType` checks - and where the name may be written: a const parameter
     * stands for a number, so it is a type argument and an expression and never a value's type.
     */
    DeclPtr<Type> constType = nullptr;

    bool isConst() const { return constType != nullptr; }
};

/*
 * A number written where a type is - type.def's `Const` row.
 *
 * The value is held as 64 bits with `type` saying how to read them, which is `Constant`'s own
 * arrangement and for its reason: the width is a property of the type and storing it twice is two
 * answers to one question.
 */
struct ConstType: Type {
    ConstType(U64 value, DeclPtr<Type> type): Type(Const), type(type), value(value) {}

    DeclPtr<Type> type;
    U64 value;
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
 * What a type owes - `doc/spec/ownership.md`'s ownership classes, as **five independent questions**.
 *
 * Folded over the members and over the instances the program declared, which is why it is a stored
 * answer rather than a function of the kind: it is the one classification a *later declaration* can
 * change, and a query that recomputed it would answer differently depending on when it was asked.
 * `classifyTypes` is the one pass that fills it, after every declaration is in.
 *
 * ## Five, and why not fewer
 *
 * Every pair of these agrees on every type that exists today, which is exactly why they are separate
 * fields: the first type where two of them differ then changes one field rather than splitting a
 * flag that half the compiler is already reading.
 *
 *  - `trivialCopy` - **may duplicate**. Whether a second owner can be made by copying the bytes.
 *  - `relocatable`  - **may relocate**. Whether the object may be moved to a different address at
 *    all. A pinned type - one somebody else holds the address of, a self-referential one, an
 *    intrusive list node - is owned, released normally, and may not travel. This is not
 *    `!trivialCopy`: a copy makes a *second* object, a relocation makes the *same* object live
 *    somewhere else, and a type may refuse either without refusing the other.
 *  - `explicitDrop` - **the compiler inserts no teardown**. `doc/spec/ownership.md`'s
 *    [`@explicitDrop`](../../doc/spec/ownership.md): a value of the type that would die without the
 *    source saying so is a diagnostic, reported where it dies. Deliberately *not* a `TeardownKind`,
 *    because it is orthogonal to both halves: the type usually has a perfectly ordinary authored
 *    `Drop` and what it withholds is the permission to *call* it unasked.
 *  - `reclaim`/`drop` - derived or authored, per half. See `TeardownKind`.
 *
 * The trap the fields exist to keep out is the one `conventionFor` fell into: reading "has a
 * teardown" as "is linear". The two agree on every type that exists today and part company at the
 * one the feature is for - a linear token with nothing to run is still the most linear thing in the
 * language, and a tier that keyed the linear rule off the teardown would have made exactly it freely
 * duplicable and freely discardable. `isLinear` below is the question that mapping has to ask
 * instead.
 */
struct Ownership {
    bool trivialCopy = true;
    bool relocatable = true;
    bool explicitDrop = false;
    TeardownKind reclaim = TeardownKind::None;
    TeardownKind drop = TeardownKind::None;

    bool needsTeardown() const {
        return reclaim != TeardownKind::None || drop != TeardownKind::None;
    }

    /*
     * Whether an owned value of this type carries an obligation - the subject of §7.1's linear rule.
     *
     * Two sources and not one: something to run at the end, or a disposal the source has to write.
     * Not `!trivialCopy`, which is a rule about *duplication* and is enforced where a duplicate is
     * made (`readMode`, and the `CopyTrivial` check in `verifyBody`) rather than by counting
     * consumptions - a unique token with nothing to run may be dropped on the floor and may not be
     * copied, and a linear rule would demand a consumer it does not have.
     */
    bool isLinear() const { return needsTeardown() || explicitDrop; }
};

/*
 * One field of a record.
 *
 * The occurrence here is the *declaration's*, and its contract says one thing only: whether the
 * record owns what is at this field or merely reaches it through one of the record's own origin
 * slots. `T` is owned, `a'T` and `&a'T` are `Origin::Slot` in the record's schema scope -
 * Analysis-Borrows-Revised.md §2.1's second table.
 *
 * It is not the contract a *place* projecting this field holds: a field of a record the function
 * only shares is shared however the field was declared, because reaching into storage does not
 * change whose it is. `Builder::fieldPlace` is where the declaration and the parent's contract are
 * combined, once.
 *
 * A whole `Occurrence` and not the type/contract pair it was, because a field may now be
 * `inner: Reader(a')` - owned here, and holding a loan of the record's slot `a` at a position one
 * level further in. The pair had nowhere to keep the third part, and the readers that needed it
 * rebuilt an `Occurrence` with a null shape - which is the shape being dropped rather than absent.
 */
struct RecordField {
    StringId name {};
    Occurrence occurrence;
    LocationId source = kNullLocation;
    U16 index = 0;

    /*
     * `data Flags {read: Bool = False}` - what a construction that leaves this field out builds, and
     * `Constant::None` where there is none.
     *
     * A **constant** and never an expression, which `doc/spec/declarations.md` states and which is
     * what makes the field able to carry it at all: there is no program point at which a declaration
     * would run, so what is written here has to be a value already. `resolveConstruction` builds one
     * per construction that omits the field, by the same `addConst` a written literal takes - the
     * default is not shared storage a second constructor would alias.
     *
     * Held by value beside the field for the reason `Constant` is a value at all: sixteen bytes
     * where the default is named, and no allocation for the overwhelming majority of fields that
     * have none.
     */
    Constant def;
};

/*
 * How a record's constructors are represented - Analysis-Language.md §5.1 and Analysis-IR.md's G7d.
 *
 * A property of the **declaration** and never of an instantiation, which is the old stage's
 * `computeRecordLayout` reasoning reproduced: a generic body projects into `Outcome(a, e)` before
 * any `a` is known, so the shape a projection is emitted against has to be the one every
 * instantiation has. Deciding it from the constructor list alone gives that - substituting an
 * argument cannot move a constructor from having a payload to not having one.
 *
 * `Single` is every ordinary `data Pair {x: I32}`: one constructor, no discriminant, and the fields
 * are the record's. `Enum` is several constructors none of which carries anything - the value is a
 * number and nothing else. `Multi` is several constructors at least one of which carries something:
 * a discriminant, then one payload area the arms share.
 */
enum class RecordLayout: U8 {
    Single,
    Enum,
    Multi,
};

/*
 * One constructor of a record - `Proceed` of `data Outcome(a, e) = Proceed(a) | Exit(e)`.
 *
 * **The fields stay one flat list on the record** and a constructor names a range of it. That is the
 * one decision this representation is built on, and what it buys is that every walk already written
 * against a record keeps working unchanged: the ownership fold, the schema fold, the layout prefix,
 * `RecordField::index` and the projection that names one are all about a field index, and a field
 * index here identifies its constructor as well. The alternative - a payload *type* per constructor,
 * which is what the old stage's `Constructor::content` is - makes every one of those walks either
 * two levels deep or a special case, and adds a `Downcast` step to a place path whose only job is to
 * say what the field index already said.
 *
 * What the range does *not* say is where the fields sit, because the arms share their storage: two
 * constructors' fields overlap, and `fieldOffsetNative` is the one place that knows it.
 */
struct RecordCon {
    StringId name {};
    LocationId source = kNullLocation;

    /*
     * Which constructor this is, counting from zero, and the number a value of it carries.
     *
     * Two fields because `@value(n)` separates them - `doc/spec/declarations.md`. The index is the
     * position in `RecordType::cons` and is what a construction and a pattern name each other by;
     * the value is what is *stored*, and it is somebody else's ABI wherever an attribute wrote one.
     * A declaration that pins nothing has `value == index` everywhere, which is what makes the
     * unpinned case the general one rather than a second rule.
     */
    U16 index = 0;
    I64 value = 0;

    // Whether an attribute wrote that value - which is not `value != index`, since pinning a
    // constructor to its own position is legal and says something. Read by the check that `@value`
    // needs a constructor carrying nothing, which is about what the author asked for.
    bool pinned = false;

    // The half-open range of `RecordType::fields` this constructor owns.
    U16 first = 0;
    U16 count = 0;
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

    /*
     * `pub` on the `data` declaration - see `exportedSymbol` in `resolve_mir/name.h`.
     *
     * On the **declaration**, and an instantiation reads it through `instanceOf`: `Boxed(I32)` is
     * made where it was first written rather than by a declaration pass, so nothing wrote `pub` on
     * one and its own flag says nothing. A type's constructors are exported with it and never on
     * their own, which is what stops a qualified spelling being a way around the marker.
     */
    bool exported = false;

    // Non-embedding: `SmallList`'s embedded encoding is for elements of four bytes or fewer, and a
    // field is a name, a type and a position.
    DeclList<RecordField, false> fields;

    /*
     * The constructors, in declaration order, and **never empty**.
     *
     * A plain `data Pair {x: I32}` has exactly one, whose name is the type's and whose range is
     * every field. That uniformity is the point: a reader asks `cons` rather than testing whether
     * the record "is a sum", so the single-constructor case is the general one with a list of one
     * rather than a second shape to remember.
     */
    DeclList<RecordCon, false> cons;

    // Decided by `recordLayoutOf` once the constructors' fields are in, and read by everything that
    // places or projects. See `RecordLayout`.
    RecordLayout layout = RecordLayout::Single;

    // Which constructor owns this field, from the ranges above. A search rather than a column on
    // `RecordField`, because a field belongs to exactly one constructor and a stored copy is a
    // second answer to keep in step; the lists are short and every caller has the record in hand.
    U16 constructorOf(DeclBase base, U16 field);

    /*
     * The two halves of `data Boxed(a) {value: a}` - the declaration, and one instantiation of it.
     *
     * `gen` is set on the **declaration** and holds the parameters it binds; `instanceOf` and
     * `arguments` are set on an **instantiation** and say which declaration it came from and with
     * what. Never both, and a record with neither is an ordinary concrete `data`.
     *
     * An instantiation is a *record of its own* with its fields substituted, rather than a base
     * plus an argument list read through at every query. That is what keeps every reader below this
     * line - the field walk, the schema fold, the ownership classification, the native layout,
     * projection, construction - written once against a plain record: `Boxed(I32)` has a field of
     * type `I32`, and nothing has to learn to substitute while it walks. What it costs is one
     * record per argument list, which is what the interning in `Types` bounds.
     *
     * A generic declaration is a type nothing has a value of. It is classified like any other -
     * over its variables, which answer conservatively - because the *generic body* asks what its
     * own parameters owe and has to get the same answer every specialization was placed against.
     */
    DeclPtr<GenContext> gen = nullptr;
    DeclPtr<RecordType> instanceOf = nullptr;
    DeclList<DeclPtr<Type>> arguments;

    Ownership ownership;

    /*
     * The record's own ownership schema - Analysis-IR.md §5.11.
     *
     * How many origin sets its fields publish, and where they sit. `originSlots` is filled by the
     * declaration walk, because it is what the author wrote; `schema` is filled by `classifyTypes`,
     * because a field whose type is another record contributes *that* record's slots at a deeper
     * path and the fold has to see both declarations.
     *
     * Null is the empty shape, which is every record whose fields are all owned - and that is the
     * overwhelming majority, which is why the handle is null rather than an empty list.
     */
    U16 originSlots = 0;
    DeclPtr<OwnershipSchema> schema = nullptr;

    // Set by `classifyTypes` when it has folded this record. Not a cache - the fold runs exactly
    // once, and this is what a cycle in the member graph is detected with.
    bool classified = false;

    /*
     * `@explicitDrop` as it was *written* on this declaration, which is not `ownership.explicitDrop`.
     *
     * The fold produces the second from the first and from the member graph, and the two differ in
     * both directions: a record that merely *holds* a linear member is `explicitDrop` without having
     * said so, and a record that says so keeps it even where an authored `Drop` would have cleared a
     * member's. Keeping the written fact is what lets `classify` apply the declaration last - see
     * the order there, which is the whole of the containment rule.
     */
    bool writtenExplicitDrop = false;

    /*
     * An **anonymous** record - `{I32, I32}` and `{x: I32, y: I32}`, `doc/spec/types.md`'s tuple.
     *
     * A row on this struct rather than a kind of its own in type.def, and the decision is the one
     * `Types::recordType` already anticipates. What a tuple has is fields, one constructor, a layout,
     * an ownership fold, a schema and projections - which is every reader of a record, and a kind
     * beside this one would be a parallel case in each of them for a shape that differs in exactly
     * two facts. Those two facts are here: it has no declaration to be nominal about, so it is
     * **interned on its field list** (`Types::tupleType`), and it belongs to no module, so nothing
     * looks it up by name.
     *
     * Interning is what makes it structural without weakening anything: two spellings of one field
     * list are one handle, so `sameType` stays a handle comparison as it is for every other kind, and
     * `data Pair {x: I32, y: I32}` is still a different type from `{x: I32, y: I32}` because a
     * declaration is not a field list.
     *
     * `name` is empty on one, which is what the printers key on: a tuple prints as its fields
     * because that is its whole identity, where a nominal record prints as the name it was declared
     * under.
     */
    bool tuple = false;

    /*
     * `@local` on the declaration - `doc/spec/modules.md`'s local declarations, at the `data`
     * placement: the type is exported and its constructors and fields are not.
     *
     * Stored and checked, and the check cannot fire yet: every name table in this stage is keyed by
     * `(module, name)` and there is exactly one module, so nothing outside the declaring module can
     * name the type in the first place. Here rather than with the milestone that adds the second
     * module, on `@pinned`'s terms - the restriction is written where the fact is, so arriving at
     * cross-module code is a name table growing a case and not a search for the positions that
     * construct and project.
     */
    bool local = false;

    /*
     * The native layout, memoized by `manifestNative` - and the one thing on a type that *is* a
     * cache.
     *
     * `nativeAlign` is zero until the walk has run, which is a state no real answer has: every
     * layout is at least byte-aligned. What it buys is the nesting: a record holding a record asks
     * for the member's manifestation to place it, so a query on a type three deep used to re-lay out
     * every type below it, and every field of every one of those. Filled only for the whole-record
     * answer; a prefix walk for one field's offset does not, since a prefix is not a layout.
     *
     * Here rather than in a table beside the lowering, because the answer is a property of the type
     * and outlives every body that asks - and because the alternative is a map keyed by a type,
     * which is the shape the offset walk in the current compiler grew out of.
     */
    U32 nativeSize = 0;
    U16 nativeAlign = 0;

    RecordField field(DeclBase base, U16 index) { return fields.get(base, index); }
};

/*
 * One parameter position of a function type.
 *
 * An `Occurrence` and nothing else, which is the same triple a declared parameter carries and is
 * carried for the same reason: a caller reaching a function through a *value* has only the type to
 * read, so whether the position owns what it is handed, borrows it, or publishes an origin is part
 * of what calls the value accepts. Two shapes that disagree about that accept different calls and
 * are two types.
 *
 * A struct of one field rather than a bare `Occurrence`, because the result below is the position
 * that will grow first - `iter (a) -> ->b` writes a convention on what a hand-over produces - and a
 * list of a named row is what a second field can be added to without every reader changing shape.
 */
struct FunParam {
    Occurrence occurrence;
};

/*
 * A function value's type - see type.def, where the structural argument is written out.
 *
 * `params` and `result` are the whole of its identity and the whole of what `Types::funType` interns
 * on. There is no declaration here and no name: what a lambda or a named function *is* belongs to
 * the `Function` it lifted to, and this describes only the calls a value of it accepts.
 */
struct FunType: Type {
    FunType(): Type(Fun) {}

    // Non-embedding, because every reader wants the positions as a buffer - matching, substitution
    // and the interning key each walk them by index against a second list of the same length.
    DeclList<FunParam, false> params;
    Occurrence result;
};

/*
 * `[T *n]` - see type.def, where the argument for it being a kind is written out.
 *
 * **The count is held two ways and exactly one of them is set.** `count` names a const type variable
 * where the length is quantified (`fn (n: Int) firstOf(xs: [Int *n])`); otherwise it is null and
 * `length` is the number. The alternative - one `DeclPtr<Type>` naming either a `ConstType` or a
 * variable, which is what the shipping stage carries - has to answer "which integer type is the `4`
 * of `[I32 *4]`", and every answer to that is arbitrary: a head declared `n: I64` binds
 * `ConstType(4, I64)` and one declared `n: I32` binds `ConstType(4, I32)`, so a written `[I32 *4]`
 * would intern as a third type that matches neither. Holding the number as a number has no such
 * question - `matchType` binds a const variable to a `ConstType` of *the variable's own* declared
 * type, which is the one place the type of a count is decided and the only place it needs to be.
 *
 * `kMaxFixedArrayLength` bounds the count and not the byte size, because the byte size is a target's
 * answer and this is a logical type: `[U8 *65536]` and `[Buffer *65536]` are one declaration here
 * and differ only in what the manifestation later multiplies by.
 */
constexpr U64 kMaxFixedArrayLength = 0xffff;

struct ArrayType: Type {
    ArrayType(DeclPtr<Type> element, DeclPtr<Type> count, U64 length):
        Type(Array), element(element), count(count), length(length) {}

    DeclPtr<Type> element;

    // The const type variable the length is quantified over, or null where it is a written number.
    // Named for what it holds rather than for the `*n` it is spelled as, because the two states are
    // told apart by whether it is *there*.
    DeclPtr<Type> count;

    // Meaningful only where `count` is null - see the note above, which is the whole of the rule.
    U64 length;

    // Whether the length is a number here. False for a quantified count, whose `length` means
    // nothing until a substitution supplies one.
    bool hasFixedLength() const { return count == nullptr; }
};

/*
 * `Vec(a, n)` and `Mask(a, n)` - type.def's `Vector` row, where the argument for the kind is.
 *
 * The count is held the way `ArrayType` holds one and for its reason - a number is a number, and a
 * `ConstType` in the field would make the *type of the count* part of this type's identity - but
 * there is one state a fixed array has no use for and a vector does: `count` null with `lanes` zero
 * is the **natural** form, `Vec(a)` written over a lane this stage cannot yet measure.
 *
 * Three states, then, and each is reachable from source:
 *
 *  - a number - `Vec(F32, 4)`, and every vector whose lane is concrete, since the natural form is
 *    spent the moment there is a stride to divide the register's width by;
 *  - a const variable - `Vec(a, n)`, which a specialization binds;
 *  - neither - `Vec(a)` inside a body generic in `a`, which is the natural form deferred.
 *
 * The natural form is spent during *resolution* rather than left to the manifestation, which is
 * sound because resolution is already per target: `@platform` selects declarations the same way, so
 * a vector's width joins the target and the extension set as something the resolved program is a
 * function of. What that buys is that nothing below this stage ever reasons about a count's absence.
 */
struct VectorType: Type {
    VectorType(DeclPtr<Type> lane, DeclPtr<Type> count, U16 lanes, bool isMask):
        Type(Vector), lane(lane), count(count), lanes(lanes), isMask(isMask) {}

    // An integer or a float whose storage is 8, 16, 32 or 64 bits, or a type variable awaiting one.
    // `laneBytes` is the rule and `makeVectorType` is where it is applied.
    DeclPtr<Type> lane;

    // The const type variable the count is quantified over, or null where `lanes` is the answer -
    // `ArrayType::count`'s arrangement exactly.
    DeclPtr<Type> count;

    // Meaningful only where `count` is null. **Zero is the natural form**, which is a state a
    // resolved vector over a concrete lane is never in - see the three states above.
    U16 lanes;

    bool isMask;

    // Whether the count is a number here - false for a quantified one, whose `lanes` means nothing
    // until a substitution supplies one, and true for the natural form, whose number is the
    // target's and is filled in as soon as the lane is known.
    bool hasFixedCount() const { return count == nullptr; }

    // Whether this is the natural form still waiting for a lane to measure. Only reachable inside a
    // generic body; `makeVectorType` is the one function that produces it and the one that spends it.
    bool isNaturalCount() const { return count == nullptr && lanes == 0; }
};

/*
 * `Atomic(a)` - type.def's `Atomic` row.
 *
 * Interned on the content the way a pointer is interned on its target, so the `Atomic(I32)` two
 * modules write is one handle and `sameType` stays a comparison.
 */
struct AtomicType: Type {
    explicit AtomicType(DeclPtr<Type> content): Type(Atomic), content(content) {}

    // One of the family Analysis-Atomics.md §3.2 admits, or a type variable awaiting substitution.
    DeclPtr<Type> content;
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

    // Indexed by [signed][log2(bytes)], so [1][2] is I32. Materialized in the constructor.
    DeclPtr<Type> ints[2][4];

    /*
     * The integers that are not one of the eight fixed widths above - `primitive.def`'s tail.
     *
     * Named handles rather than rows of `ints`, because neither indexing reaches them: `WideInt` is
     * fifty-three bits and has no byte count, and the target widths have no width here at all. They
     * are still materialized once at construction like every other builtin, so `intType` stays a
     * read and type identity stays handle identity.
     */
    DeclPtr<Type> wideInt;
    DeclPtr<Type> words[2];
    DeclPtr<Type> codeUnit;

    // Indexed by log2(bytes) - 2, so [0] is F32 and [1] is F64.
    DeclPtr<Type> floats[2];

    /*
     * The one integer type accessor - by width, signedness and which quantity answers the width.
     *
     * Every integer this stage builds comes through here, whether a signature named it or the
     * compiler invented it, so that two spellings of one type cannot become two handles.
     */
    DeclPtr<Type> intType(U8 bits, bool isSigned,
                          intrinsic::TargetWidth target = intrinsic::TargetWidth::Fixed);

    /*
     * `@bits(n) T` - the refinement of an integer type, interned on the pair.
     *
     * Interned for `intType`'s reason and one of its own: `alias Small = @bits(4) U32` written in two
     * modules has to be one handle, or a value of it would not fit a parameter declared with the
     * other. The signedness and the target width are the canonical's - a refinement narrows a range
     * and changes nothing else about what the type is - so the pair is the whole key.
     *
     * The *caller* checks what may be refined and by how much; this builds what it is asked for.
     * `Resolver::resolveBitsType` is where those rules are, beside the diagnostics they produce.
     */
    DeclPtr<Type> refinedInt(DeclBase base, DeclPtr<Type> canonical, U8 bits);

    /*
     * What a lens's continuation answers with - Analysis-Lens.md §12.1's fifth item.
     *
     * The exit signal is a *manifestation* and not a source type, and this is the one place its
     * width is decided: a continuation returns which of its exits it took, and everything an exit
     * carries - a `return`'s value, an outward edge's block arguments - travels in a caller-frame
     * slot the continuation already reaches. So the discriminant is a small integer, `$r` is bound
     * to it at every lens call site, and the conversion is what numbers the rows.
     *
     * Named rather than written as `intType(32, true)` at the two sites that need it, because it is
     * one decision: the call site binding the type argument and the conversion emitting the switch
     * have to agree, and a second spelling is how they come to disagree.
     */
    DeclPtr<Type> exitCode() { return intType(32, true); }
    DeclPtr<Type> floatType(U8 bits);
    DeclPtr<Type> pointerType(DeclPtr<Type> to);

    // `4 :: I32` as a type argument, interned on the pair. Two spellings of one number at one type
    // are one handle, which is what keeps `Buf(4)` written in two signatures one type.
    DeclPtr<Type> constType(U64 value, DeclPtr<Type> of);

    /*
     * A record, made fresh for one declaration - deliberately *not* interned.
     *
     * Nominal identity is what the row in type.def argues for: two `data` declarations with
     * identical fields are two types. It goes through this table anyway so that every logical type
     * in the program is made in one place, which is what a later structural kind - a tuple, which
     * does intern - has to sit beside without the two ways of making a type diverging.
     */
    RecordType* recordType(StringId name, DeclPtr<Module> module, LocationId source);

    /*
     * `Boxed(I32)` - the instantiation of a generic declaration for one argument list.
     *
     * Interned, so `Boxed(I32)` written in two signatures is one type and `sameType` stays a handle
     * comparison. That is not an optimization: instance selection, the move-path tree and every
     * `checkType` in the resolver are equality on handles, so two records with identical fields
     * would be two types the program cannot pass between.
     *
     * The shell is registered **before** its fields are filled, and `created` is how the caller is
     * told to fill them. A recursive generic - `data List(a) {tail: Box(List(a))}` - reaches its own
     * instantiation while substituting, and finding the shell already interned is what makes that a
     * cycle the classifier reports rather than a substitution that does not terminate.
     */
    RecordType* instantiate(DeclBase base, DeclPtr<RecordType> declaration, DeclPtr<Type>* args,
                            Size count, bool& created);

    /*
     * `{I32, I32}` - an anonymous record, interned on the field list that is its whole identity.
     *
     * Structural interning, which is what `RecordType::tuple` argues for, and the key is each
     * field's **name and type** in order: `{x: I32, y: I32}` and `{y: I32, x: I32}` are two types,
     * and so are `{x: I32}` and `{I32}`. Re-checked structurally on a hash hit for `intern.h`'s
     * reason.
     *
     * The shell is handed back empty for the caller to fill, exactly as `instantiate`'s is and for a
     * weaker version of the same reason: the fields are already resolved here, so nothing recurses,
     * but the fold that classifies one has to run on a record that is registered.
     */
    RecordType* tupleType(DeclBase base, const RecordField* fields, Size count, bool& created);

    /*
     * `(A, B) -> C` - a function value's type, interned on the whole shape.
     *
     * Structural interning, which is what type.def's row argues for: the positions are compared as
     * occurrences - type, contract and shape - so two spellings that accept the same calls are one
     * handle and `sameType` stays a comparison here as it is everywhere else.
     *
     * Re-checked structurally on a hash hit for `intern.h`'s reason, and the check is the whole list
     * rather than its length: a key mixing several handles has room to collide, and a collision here
     * would be two different signatures sharing a type.
     */
    DeclPtr<Type> funType(DeclBase base, const FunParam* params, Size count, const Occurrence& result);

    /*
     * `[T *n]` - interned on the element and the count, so `[I32 *4]` written in two signatures is
     * one handle and `sameType` stays a comparison.
     *
     * Two entry points because the two callers hold different things and neither should have to
     * assemble the other's. A written type has a number; a substitution has whatever the const
     * argument turned out to be, and that is a `ConstType` it should not have to unwrap - see
     * `substituteType`, which passes the substituted count straight through.
     */
    DeclPtr<Type> arrayType(DeclBase base, DeclPtr<Type> element, U64 length);
    DeclPtr<Type> arrayType(DeclBase base, DeclPtr<Type> element, DeclPtr<Type> count);

    /*
     * `Vec(a, n)`, `Mask(a, n)` and `Atomic(a)`, interned on what they are made of.
     *
     * **Nothing outside `makeVectorType` and `makeAtomicType` calls these.** What a written `Vec(a)`
     * means - which lanes are admissible, what the natural count divides out to, which widths a
     * target refuses - is a rule rather than a table read, and a second caller building one of these
     * directly is that rule not applied. The two free functions are where it is stated once; these
     * are the interning underneath them, exposed for the reason `funType` is - the table is the one
     * place a type is made.
     */
    DeclPtr<Type> internVector(DeclBase base, DeclPtr<Type> lane, DeclPtr<Type> count, U16 lanes,
                               bool isMask);
    DeclPtr<Type> internAtomic(DeclPtr<Type> content);

    // The type a source name spells, or null. Only the widths and `{}` are builtin names; every
    // other type comes from a declaration, and Core's aliases are declarations.
    DeclPtr<Type> findBuiltin(Context& context, StringId name);

private:
    HashMap<U32, DeclPtr<Type>> pointers;
    HashMap<U64, DeclPtr<Type>> constants;

    // Keyed by a hash of the declaration and its arguments, with the entry re-checked structurally -
    // `Shapes::intern`'s shape, and for its reason: a hash is not an identity.
    HashMap<U64, DeclPtr<RecordType>> instantiations;

    // The same arrangement for function types, which intern on their whole shape rather than on a
    // declaration - see `funType`.
    HashMap<U64, DeclPtr<Type>> functions;

    // The same arrangement again, keyed on the element handle and the count - a length, or the
    // handle of the variable standing in for one.
    HashMap<U64, DeclPtr<Type>> arrays;

    // And again for the anonymous records, keyed on the whole field list - see `tupleType`.
    HashMap<U64, DeclPtr<RecordType>> tuples;

    // The `@bits(n)` refinements, keyed on the refined type and the width - see `refinedInt`.
    HashMap<U64, DeclPtr<Type>> refinements;

    // The vectors, keyed on the lane, the count - a number or the handle of the variable standing
    // for one - and the mask flag; and the atomics, keyed on their content alone.
    HashMap<U64, DeclPtr<Type>> vectors;
    HashMap<U32, DeclPtr<Type>> atomics;
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

/*
 * What the source calls an integer of this shape - `intrinsic::primitiveIntName`.
 *
 * A function rather than a sign letter and a number at each site that prints one, because the two
 * disagree for four of the twelve: `Size` printed by width is `I64`, which is a different type and
 * one a narrower target may not even have.
 */
StringView intTypeName(const IntType& type);

/*
 * Which layout a record's constructor list asks for - the one place the three are told apart.
 *
 * Decided from the constructors alone, so it is stable under substitution - see `RecordLayout`.
 * Called once per declaration when its fields are in, and copied onto each instantiation.
 */
RecordLayout recordLayoutOf(DeclBase base, RecordType& record);

/*
 * Whether this type is a record with more than one constructor - the question every pass that has to
 * treat a sum differently from a product actually asks.
 *
 * One spelling rather than `kind == Record && layout != Single` written out at each site, because it
 * is a *rule* and not a test: a field of one is not a partial-ownership position, two of its fields
 * may share storage, and a projection into one has to have been proved by a match. Those are three
 * passes reading one fact, and a fourth arriving should find the sentence rather than the condition.
 */
bool isSumRecord(DeclBase base, DeclPtr<Type> type);

/*
 * What an aggregate is built out of - how many components, and the occurrence each one fills.
 *
 * Two kinds of aggregate go through them, and the difference between the two is exactly what these
 * answer: a record's components are its declared fields, each with a contract of its own, and a
 * fixed array's are `n` copies of one element type, all of them owned. The builder, the verifier and
 * the conversion each ask, and a second reading of the split is how one of them comes to write a
 * component past the end of an allocation.
 */
Size aggregateComponentCount(DeclBase base, DeclPtr<Type> type, U16 constructor);
Occurrence aggregateComponent(DeclBase base, DeclPtr<Type> type, U16 constructor, Size index);

/*
 * The contract one component of an aggregate is taken under, with the destination's shape applied.
 *
 * `aggregateComponent` answers what the *declaration* says the position is; this answers what the
 * position being filled is, which differs by exactly the qualified type arguments the destination
 * wrote - `Maybe('v)`'s payload is a reference where `Maybe(v)`'s is a member. It is `fieldContract`
 * with the component index turned into a field index, which is the one step a caller holding an
 * aggregate rather than a record does not have.
 *
 * An array has no shape to apply: its elements are its storage and there is no path step that names
 * one, so it answers the element's own contract whatever the shape says.
 */
Contract aggregateComponentContract(DeclBase base, DeclPtr<Type> type, U16 constructor, Size index,
                                    DeclPtr<OwnershipSchema> shape);

/*
 * One constructor named - which record, and which of its arms.
 *
 * A pair rather than a pointer into the list, because a constructor is not a declaration of its own:
 * it has no identity apart from the record that declares it, and what every reader wants is the type
 * it builds beside the index it builds. `Program::findConstructor` is what produces one.
 */
struct ConstructorRef {
    DeclPtr<RecordType> record = nullptr;
    U16 index = 0;

    explicit operator bool() const { return record != nullptr; }
};

/*
 * The span a record's constructor numbers occupy, and the width that holds them.
 *
 * One answer rather than three questions, because the three are derived from one walk over the
 * constructor list and a reader that asked them separately could get a width that does not hold the
 * range it was computed beside. `@value(n)` is what makes the walk necessary at all: without it the
 * numbers are 0 to count-1 and every field here follows from the count.
 */
struct TagRange {
    I64 lowest = 0;
    I64 highest = 0;

    // The **stored** width - 8, 16, 32 or 64 - and zero for a record with one constructor, which
    // has nothing to tell apart. A stored width rather than a logical one because a discriminant is
    // written into memory here; co-packing is what would want the narrower number, and nothing in
    // this stage packs yet.
    U8 bits = 0;

    // Set where a constructor was pinned below zero - see `tagRangeOf`, where the consequence is
    // argued. What it changes is the tag's own type, which is then signed like any other integer.
    bool isSigned = false;
};

TagRange tagRangeOf(DeclBase base, RecordType& record);

// The storage a machine gives an integer of this logical width: the smallest power-of-two byte
// count that holds it, in bits.
inline U8 storageBitsFor(U8 bits) {
    return bits <= 8 ? 8 : bits <= 16 ? 16 : bits <= 32 ? 32 : 64;
}

/*
 * The type of a record's discriminant, and null where it has none.
 *
 * A real `IntType` and not a width the manifestation invents, which is what makes a discriminant an
 * ordinary value everywhere above the target: the place that names one carries this as its
 * occurrence, the comparison a pattern emits is an ordinary `Binary`, and the storage width follows
 * from the type the way every other integer's does. What decides it is how many numbers there are to
 * tell apart, and nothing else - `@value(n)` widens the range rather than adding a rule.
 *
 * Unsigned wherever a constructor number is a count, and signed wherever `@value(n)` pinned one
 * below zero - `TagRange::isSigned`, which is where the consequence of that is argued.
 */
DeclPtr<Type> tagTypeOf(DeclBase base, Types& types, DeclPtr<Type> record);

// Which declaration a record type came from - itself, or the generic it instantiates. What a
// constructor's name resolves to is a declaration, and a value's type is usually an instantiation,
// so this is what the two are compared at.
DeclPtr<RecordType> recordDeclarationOf(DeclBase base, DeclPtr<Type> type);

// How wide that discriminant is, in bits, and zero for a record that has none - `TagRange::bits`
// read on its own. The half of `tagTypeOf` the manifestation table needs, which has a base and no
// `Types` to intern in, so the width is decided once and the type is built from it.
U8 tagBitsOf(DeclBase base, RecordType& record);

// Whether two logical types are the same. Pointer identity, because the table interns; the function
// exists so that a caller states what it is asking rather than comparing handles by hand.
inline bool sameType(DeclPtr<Type> a, DeclPtr<Type> b) { return a == b; }

/*
 * How many bytes one lane of this type occupies, and zero where it is not a type a lane may be.
 *
 * The **natural storage** of an integer rather than its declared bits, which is what makes a
 * `@bits(n)` refinement an ordinary lane: `@bits(30) U32` occupies four bytes standing alone, so a
 * lane of it occupies four, and `Vec(Count)` has the shape `Vec(U32)` has. Design-Vector §2.2 admits
 * the refinement precisely so the lanes keep the range it states.
 *
 * The target's own widths for `Size`, `USize` and `CodeUnit` - `IntWidths` rather than the whole
 * settings, because those three are the only target input a lane width has, and a manifestation
 * asking this holds the widths and not the settings. A lane width is a target question already: `Vec(CodeUnit)` is a vector of bytes natively and of UTF-16 units on a host
 * whose strings are, which is a different vector rather than a wider one.
 *
 * Zero for everything else, `Bool` included: one bit is not a lane, and what a comparison of a
 * vector answers is a `Mask` rather than a vector of booleans - Design-Vector §2.4 is the argument
 * for the two being different types.
 */
U32 laneBytes(DeclBase base, IntWidths widths, DeclPtr<Type> type);

/*
 * `Vec(a, n)` and `Mask(a, n)` - the one place the rules about them are applied.
 *
 * `lanes` of zero asks for the **natural** form, `targetVectorBytes / laneBytes`. Over a lane this
 * stage cannot measure - a type variable - there is no stride to divide by, so the zero survives
 * into the type and `substituteType` asks again once the lane is concrete. That is the only deferral
 * in the design and it mirrors the fixed array's element exactly.
 *
 * Everything it can refuse it refuses here, so that a bad vector names the type it was written on:
 * a lane that is not a machine lane, a count that is not a power of two between 2 and the ceiling,
 * a width past the widest register any target has, and the lane widths a target has no arithmetic
 * for. `source` is what all four are reported at.
 *
 * Null on refusal, which every caller propagates - there is no error type in this stage.
 */
DeclPtr<Type> makeVectorType(Context& context, Program& program, DeclPtr<Type> lane, U64 lanes,
                             bool isMask, LocationId source);

// The same, for a caller holding a count that may be a const variable - which is every path that
// substitutes an existing type rather than reading a written one. A null count is the natural form,
// exactly as a zero is above.
DeclPtr<Type> makeVectorType(Context& context, Program& program, DeclPtr<Type> lane,
                             DeclPtr<Type> count, U64 lanes, bool isMask, LocationId source);

/*
 * `Atomic(a)` - Analysis-Atomics.md §3.1, and the one place the admitted family is enforced.
 *
 * A content outside what §3.2 lists is reported here, so nothing downstream has to ask whether a
 * target could perform the access. A generic content defers the check to substitution, exactly as a
 * vector's lane does.
 */
DeclPtr<Type> makeAtomicType(Context& context, Program& program, DeclPtr<Type> content,
                             LocationId source);

// Whether `Atomic(a)` admits this content - the `AtomicValue(a)` family of §3.2. The integers, any
// payload-free enumeration (`Bool` among them), and a raw pointer.
bool isAtomicValue(DeclBase base, DeclPtr<Type> type);

// The subset `AtomicInteger(a)` admits, which is that list without the enumerations and without a
// pointer: the arithmetic and bitwise updates of §3.4 apply to a number and nothing else.
bool isAtomicInteger(DeclBase base, DeclPtr<Type> type);

// Whether this is a vector, and whether it is a mask - the two readings of one kind, so that a
// caller states which it means rather than testing a flag it may read backwards.
inline bool isVectorType(DeclBase base, DeclPtr<Type> type) {
    return type && base[type]->kind == Type::Vector && !((VectorType*)base[type])->isMask;
}

inline bool isMaskType(DeclBase base, DeclPtr<Type> type) {
    return type && base[type]->kind == Type::Vector && ((VectorType*)base[type])->isMask;
}

// The lane type of a vector or a mask, and null for anything else - usable as a test for the reason
// `elementTypeOf` is.
inline DeclPtr<Type> laneTypeOf(DeclBase base, DeclPtr<Type> type) {
    if(!type || base[type]->kind != Type::Vector) return nullptr;
    return ((VectorType*)base[type])->lane;
}

/*
 * How many lanes a vector or a mask has, and zero for anything that is not one.
 *
 * Zero is also the unresolved natural form and the quantified count, both of which are reachable
 * only inside a generic body: every vector a body actually computes with has been through a
 * specialization. A caller that has to tell "not a vector" from "not decided yet" asks the type.
 */
inline U32 vectorLanes(DeclBase base, DeclPtr<Type> type) {
    if(!type || base[type]->kind != Type::Vector) return 0;

    auto& vector = *(VectorType*)base[type];
    return vector.hasFixedCount() ? vector.lanes : 0;
}

// The mask a comparison of this vector answers - the same shape with the flag set. Null where the
// argument is not a vector, which is what makes it usable as the test as well as the answer.
DeclPtr<Type> maskFor(Context& context, Program& program, DeclPtr<Type> vector, LocationId source);

/*
 * A type as `intrinsic/convert.h` sees it, or nothing where it is not a number.
 *
 * The one place a handle becomes a `Numeric`, so that every caller of the conversion rules asks them
 * the same question about the same four columns. Nothing where the type is a record, a window, a
 * pointer or `{}` - which is not a refusal, only "this is not a rung of a numeric ladder"; what a
 * conversion between two such types would be is the library's to declare an instance for.
 */
inline Maybe<intrinsic::Numeric> numericOf(DeclBase base, DeclPtr<Type> type) {
    if(!type) return Nothing();

    auto& written = *base[type];
    if(written.kind == Type::Int) return Just(((IntType&)written).numeric());
    if(written.kind == Type::Float) return Just(((FloatType&)written).numeric());

    return Nothing();
}

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
 * Whether the record owns what is at this field - Analysis-IR.md §5.11's first sentence, as one
 * function.
 *
 * A field in `Origin::Slot` is a position the record *reaches* and does not hold: `a'Guard` is
 * somebody else's guard, and the record contributes one schema slot for it and nothing else. Every
 * other origin is a member.
 *
 * It exists as a named question because three passes ask it and they used to ask it three ways:
 * `classifyTypes` folded the ownership over the members, the lowering emitted a derived teardown
 * over them and the reachability walk collected the teardowns they call. Only the first read the
 * contract, so a record holding both a borrowed guard and an owned one classified correctly, then
 * released the borrowed one - a `Drop` run on storage the caller still owns. One decision in one
 * place is what stops the next pass from being the fourth answer.
 */
inline bool ownsField(const RecordField& field) {
    return field.occurrence.contract.origin() != Origin::Slot;
}

// Whether a *derived* teardown of the enclosing record descends into this field: the record has to
// own it, and there has to be something to run. Both halves, because either one alone is a walk into
// a field that has no business being reached - see `ownsField`.
inline bool tearsDownField(DeclBase base, const RecordField& field) {
    return ownsField(field) && hasTeardown(base, field.occurrence.type);
}

/*
 * The interned ownership schema of a logical type, or null for the empty one - §5.11.
 *
 * A property of the *type* rather than of an occurrence, because a nominal record's interior
 * structure is its declaration: `Reader` has one external position however it is passed. What
 * `Occurrence::shape` is for is qualification a generic *application* varies - `Maybe(a'T)` against
 * `Maybe(T)` - and until higher-kinded application exists the two are the same answer, so there is
 * one place to ask.
 */
DeclPtr<OwnershipSchema> schemaOf(DeclBase base, DeclPtr<Type> type);

/*
 * Whether `variable` stands anywhere inside `type` - the question a qualified type argument asks.
 *
 * `Maybe('v)` qualifies every position inside `Maybe` that holds the argument, and the positions are
 * found by walking to them; this is what tells "the head never uses this parameter" from "it uses it
 * somewhere the walk cannot name". The two are different mistakes and need different sentences, and
 * neither is derivable from an empty result on its own.
 *
 * Structural and total: it answers for every kind, so a kind added to type.def that can hold a type
 * has to be given a row here rather than silently answering no. Termination is the instantiation
 * graph's - an argument list is finite and a record's arguments are strictly smaller than the
 * application that made them - with the one cycle a nominal record can have, a field of its own
 * type, cut by never descending into fields. Fields are the *path* walk's business and this is not
 * that walk; what it reads is the type's own structure.
 */
bool mentionsVariable(DeclBase base, DeclPtr<Type> type, DeclPtr<Type> variable);

/*
 * The interior shape of one *occurrence* - the one place anything asks what is inside a value.
 *
 * Two sources, and the order is what makes them one answer. An occurrence's own shape wins where it
 * has one, because it is the more specific statement: `Reader(a')` says which of *this* signature's
 * origin sets fills `Reader`'s interior position, and the declaration of `Reader` cannot know. Where
 * the occurrence says nothing the type's own schema is the answer, which is every record passed
 * around inside the module that declared it.
 *
 * `schemaOf` is deliberately still separate and is asked exactly twice: here, and by `own.cpp` to
 * find out whether a field's *type* has interior positions the field then has to bind. Everything
 * else asks this, because everything else has an occurrence and asking the type alone is how a
 * qualification a use site wrote gets dropped.
 */
inline DeclPtr<OwnershipSchema> shapeOf(DeclBase base, const Occurrence& occurrence) {
    return occurrence.shape ? occurrence.shape : schemaOf(base, occurrence.type);
}

// Whether a value of this occurrence refers to storage somebody else owns - a borrow, or a record
// with an external field. What decides whether a local carries an origin set even though it owns
// what is in it.
inline bool carriesOrigins(DeclBase base, const Occurrence& occurrence) {
    return !occurrence.contract.isOwned() || shapeOf(base, occurrence) != nullptr;
}

/*
 * How many origin *positions* an occurrence has - the slot axis of §5.11's `point x slot -> origins`.
 *
 * The root, and then one per interior slot the type's schema names. `Reader` has two: what the value
 * itself refers to, and what its `held` field refers to. Collapsing them into one set is what makes
 * reading one external field keep another's loan live, which is a false rejection and not a rule of
 * the language - §2.2 groups fields under a name and is explicit that grouping "does not claim that
 * the members are one object".
 *
 * The root is a position even where the occurrence is **owned** and it is therefore always empty. It
 * costs a row and it buys the property every mapping over these positions relies on: a place and the
 * value read out of it have the same count, because both are `1 + slots(type)`. A representation
 * that dropped the root for an owned value would make a read of a whole record a renumbering.
 */
inline U32 originPositions(DeclBase base, const Occurrence& occurrence) {
    if(!carriesOrigins(base, occurrence)) return 0;

    auto schema = shapeOf(base, occurrence);
    return schema ? 1 + U32(base[schema]->slots.size()) : 1;
}

/*
 * Which published origin set position `p` of a **declared** occurrence names.
 *
 * Three answers, and the middle one is the whole point of the function.
 *
 * The **root** reads its own contract. An **interior position the declaration bound** reads the
 * occurrence's `shape` - and deliberately not `shapeOf`, which is the one place the two sources of a
 * shape are not interchangeable: a shape the *occurrence* carries came from `Reader(a')`, so its
 * slot numbers are this signature's, while a shape read off the *type* is the record's own
 * declaration and its numbers belong to the record's scope, where they mean something else.
 *
 * An **interior position nothing bound** falls back to the root's answer, and that is the
 * conservative direction rather than an omission. `x: a'Reader` says the reader is in `a` and says
 * nothing about the guard inside it, so everything reachable through `x` has to be treated as `a`'s
 * too - the alternative credits a result derived from `x` with the reader's extent and not the
 * guard's, and drops the guard while a borrow of it is live. `kNoOriginSlot` is therefore reached
 * only where the *root* names nothing either, which is an owned occurrence whose interior the
 * signature declined to describe - exactly what `checkReturnConformance` refuses.
 *
 * The three functions below are that paragraph split where its readers split. **Whether** the
 * declaration bound a position, **which contract** it bound, and **which published slot** that
 * contract names are three different questions, and the middle one is not derivable from the last:
 * `Origin::CallBounded` and "nothing was bound" both answer `kNoOriginSlot`.
 */

// Whether the declaration described position `p` itself, rather than the root's answer standing in
// for it. The root is always described - it is the occurrence's own contract.
inline bool bindsOriginAt(DeclBase base, const Occurrence& occurrence, U32 position) {
    if(!position) return true;

    auto shape = occurrence.shape;
    return shape && position - 1 < base[shape]->slots.size();
}

/*
 * The contract a **declared** occurrence binds at position `p`, with the fallback above applied.
 *
 * The indexing - zero is the root, `p - 1` indexes the shape - is written once, here, and the three
 * readers differ only in what they ask of the answer. `originSlotAt` below wants the published slot
 * number; `checkHandoverConformance` wants the whole contract, because a hand-over's positions are
 * `Origin::CallBounded` rather than slots and a slot number cannot tell one of those from a position
 * the declaration never described - which is what `bindsOriginAt` is separately for.
 */
inline Contract originContractAt(DeclBase base, const Occurrence& occurrence, U32 position) {
    if(position && bindsOriginAt(base, occurrence, position)) {
        return base[base[occurrence.shape]->slots.get(base, position - 1)]->contract;
    }

    return occurrence.contract;
}

inline U16 originSlotAt(DeclBase base, const Occurrence& occurrence, U32 position) {
    return originContractAt(base, occurrence, position).originSlot();
}

/*
 * Which origin *position* a shape gives an immediate field of the value, or zero for none.
 *
 * Numbered the way `originSlotAt` indexes - zero is the root, so zero also means "this field is not
 * one of the positions", which is the same answer for both and is why they compose. Only the
 * one-step case, deliberately: a shape reached through several fields is `Analysis::slotAt`, which
 * is a projection walk rather than a lookup, and a second general version of it here would be the
 * same question answered twice.
 */
inline U32 fieldPosition(DeclBase base, DeclPtr<OwnershipSchema> shape, U16 field) {
    if(!shape) return 0;

    auto& schema = *base[shape];
    for(U32 i = 0; i < schema.slots.size(); i++) {
        auto& slot = *base[schema.slots.get(base, i)];
        if(slot.path.size() != 1 || slot.path.get(base, 0) != PathStep::field(field)) continue;

        return 1 + i;
    }

    return 0;
}

/*
 * Whether an occurrence's shape names positions the *type* does not - `Maybe('v)` against `Maybe(v)`.
 *
 * The one test that tells a **renumbering** from a **qualification**, and the difference decides
 * every other question about the value. `Reader(a')` says which of this signature's origin sets fills
 * a position `Reader` already has: same positions, same members, same layout, same teardown. A
 * qualified type argument *takes a member away* - the payload of a `Maybe('v)` is somebody else's -
 * so the layout differs and so does what dies with it.
 *
 * A count, because that is what separates them: a renumbering has one slot per declared slot, and a
 * qualification has more. Two shapes with equal counts describe the same structure by construction,
 * since `resolveApplication` builds a renumbering by walking the declared list.
 */
inline bool qualifiesInterior(DeclBase base, const Occurrence& occurrence) {
    if(!occurrence.shape) return false;

    /*
     * A **record**, because a field is the only interior position a qualifier can take away.
     *
     * The two structural kinds that carry a shape are not this: a closure's environment word and a
     * pointer's pointee are positions those kinds have whether or not anybody said whose storage
     * they name, so `%yield'I32` and a capturing lambda are statements about *provenance* over an
     * unchanged value. `schemaOf` has nothing to answer for either, which is exactly why the count
     * test has to be told which kinds it is a count of.
     */
    if(base[occurrence.type]->kind != Type::Record) return false;

    auto schema = schemaOf(base, occurrence.type);
    auto declared = schema ? base[schema]->slots.size() : 0;
    return base[occurrence.shape]->slots.size() > declared;
}

/*
 * What an occurrence's shape says about one immediate field, or the field's own contract.
 *
 * The reading half of a qualified type argument. `Maybe('v)` is `Maybe(v)` with a shape naming the
 * payload, so what the payload position *is* is the shape's answer where it has one and the
 * declaration's where it does not - and every place that installs into a field, reads one back or
 * lays one out has to ask the same question in the same order, which is why it is a function.
 *
 * A null shape is the overwhelming majority and answers the field's own contract, so the ordinary
 * record costs one null test.
 */
inline Contract fieldContract(DeclBase base, DeclPtr<OwnershipSchema> shape,
                              const RecordField& field) {
    auto position = fieldPosition(base, shape, field.index);
    if(!position) return field.occurrence.contract;

    return base[base[shape]->slots.get(base, position - 1)]->contract;
}

/*
 * What a value of this occurrence owes - the type's answer, unless a qualified argument took its
 * members away.
 *
 * `checkQualifiedShapes` is what makes the second half a constant rather than a fold: a qualified
 * argument is admitted only where removing it leaves a value with nothing to run and nothing to deep
 * copy, so an occurrence that qualifies one is trivial and there is nothing left to walk. The rule is
 * checked once, at the declaration; this is where it is spent.
 *
 * Asked of an occurrence rather than a type wherever the answer decides what *this* value does - a
 * drop placed against a local, a copy at a read - and deliberately not everywhere: what a type owes
 * is still the type's question, and a descriptor handed to generic code answers it.
 *
 * The four below are `hasTeardown`, `isTracked`, `isTriviallyCopyable` and `isLinear` asked here
 * instead, and the rule for which form a site wants is the one sentence above: **a site holding an
 * occurrence asks the occurrence**. They have to move together, because they are read against each
 * other - `readMode` answers `CopyTrivial` from one and `verifyBody` checks it with the same one,
 * `buildPaths` makes a path from a second and `placeDrops` ends it with a third. A site that asked
 * the type while its neighbour asked the occurrence is how a value that copies freely got a move
 * path and a diagnostic saying it could not be copied.
 *
 * `captureKindOf` is the deliberate exception and the reason the rule is about the *site* rather
 * than about the value: a capture that copied a value carrying somebody's borrow into an
 * environment would put the borrow somewhere no loan records it, so a capture asks the type and
 * takes the shared rung. Analysis-Lens.md §12.3(c)'s bare `%T` is the same sentence about a
 * pointer.
 */
inline Ownership ownershipOfOccurrence(DeclBase base, const Occurrence& occurrence) {
    if(qualifiesInterior(base, occurrence)) return Ownership {};
    return ownershipOf(base, occurrence.type);
}

inline bool hasTeardownAt(DeclBase base, const Occurrence& occurrence) {
    return ownershipOfOccurrence(base, occurrence).needsTeardown();
}

inline bool isTrackedAt(DeclBase base, const Occurrence& occurrence) {
    auto ownership = ownershipOfOccurrence(base, occurrence);
    return ownership.needsTeardown() || !ownership.trivialCopy;
}

inline bool isTriviallyCopyableAt(DeclBase base, const Occurrence& occurrence) {
    return ownershipOfOccurrence(base, occurrence).trivialCopy;
}

inline bool isLinearAt(DeclBase base, const Occurrence& occurrence) {
    return ownershipOfOccurrence(base, occurrence).isLinear();
}

/*
 * Whether a second owner may be made by copying the bytes, with no call and no observable effect.
 *
 * Not the negation of `hasTeardown`: a type may have no teardown and still refuse duplication - a
 * unique token, a handle whose whole point is that there is one - and the evidence
 * `UseMode::CopyTrivial` claims is *this* one. Separate fields on `Ownership` so that the first type
 * where they differ changes one of them.
 */
bool isTriviallyCopyable(DeclBase base, DeclPtr<Type> type);

/*
 * One question about a type's representation - the rows of metric.def.
 *
 * Here rather than beside `NodeMetric`, with `isTriviallyCopyable` above and `hasTeardown` further
 * up, because two of the five *are* those two functions asked at a type this compilation laid out.
 * The node is the same question asked where there may be no such type to ask it about.
 */
enum class TypeMetric: U8 {
#define YANA_MIR_METRIC(kind, name) kind,
#include "metric.def"
#undef YANA_MIR_METRIC
};

// How many there are, which is the count `expandTypeTraits` holds `TypeTraits`' field list to.
static constexpr Size kTypeMetricCount = 0
#define YANA_MIR_METRIC(kind, name) + 1
#include "metric.def"
#undef YANA_MIR_METRIC
    ;

// What a metric is called in a dump - the second column of metric.def, and the only reader of it.
inline StringView typeMetricName(TypeMetric metric) {
    switch(metric) {
#define YANA_MIR_METRIC(kind, name) case TypeMetric::kind: return name;
#include "metric.def"
#undef YANA_MIR_METRIC
    }

    return "?"_v;
}

/*
 * Whether an owned value of this type is linear - `Ownership::isLinear`, as the question a tier
 * boundary asks.
 *
 * **This is the one place the linear rule's subject is decided.** `ssa::Conversion::conventionFor`
 * used to ask `hasTeardown` instead, which is a different question that happens to have the same
 * answer for every type that does not use `@explicitDrop`; for one that does with nothing to run it
 * is the wrong one, and wrong in the direction that silently removes the obligation rather than the
 * direction that reports.
 */
bool isLinear(DeclBase base, DeclPtr<Type> type);

/*
 * Whether the compiler may insert this type's teardown - `Ownership::explicitDrop`, negated.
 *
 * The question `analyze_drop.cpp` asks at every point it was about to place one, and the whole of
 * what the attribute does. Separate from `isLinear` because they answer opposite halves of the same
 * situation: `isLinear` says an owned value of this type is *followed*, and this says what happens
 * at the point the following ends with the value still there.
 */
bool needsExplicitDisposal(DeclBase base, DeclPtr<Type> type);

/*
 * Whether this stage follows where a value of this type went - the move-path question, and the
 * question "is this an object at all".
 *
 * The third of the trio and deliberately not either of its neighbours. `isLinear` is the
 * *obligation*; `isTriviallyCopyable` is *duplication*; this is the *initialization state*. A value
 * that may not be duplicated has to be followed so that a use after it moved is a diagnostic,
 * whether or not anything runs when it dies, and a linear value has to be followed so that the
 * teardown is placed where it actually is.
 *
 * It is also what decides whether a produced value gets an `Object`: a lineage exists for the things
 * whose lifetime this frame is answerable for, and a scalar is not one of them.
 */
bool isTracked(DeclBase base, DeclPtr<Type> type);

/*
 * Whether the object may be moved to another address - `Ownership::relocatable`.
 *
 * The fact a `UseMode::Take` out of a place claims, and what `verifyBody` checks it against, on the
 * same terms as `CopyTrivial` and `isTriviallyCopyable`. Nothing sets it false yet: the fold below
 * is the only producer until `@pinned` is a declaration this stage reads, and the point of asking it
 * now is that the relocation sites are already enumerated - a `Take` operand, and nothing else - so
 * the attribute arrives as a classification change rather than as a search for them.
 */
bool isRelocatable(DeclBase base, DeclPtr<Type> type);

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
