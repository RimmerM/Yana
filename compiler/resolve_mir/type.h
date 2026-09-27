#pragma once

#include "constant.h"
#include "../intrinsic/convert.h"
#include "../intrinsic/primitive.h"
#include "../intrinsic/constructor.h"
#include "../lower/convention.h"

namespace mir {

struct Module;
struct GenContext;
struct Program;
struct Function;

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
 * A kind is added by adding a row to type.def, which generates the enum and `visitType`.
 */
struct Type {
    enum Kind: U8 {
#define YANA_MIR_TYPE(kind, Struct) kind,
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

/*
 * Whether a call argument of `supplied` fills a parameter declared `declared` - the same type, or a
 * `@bits(n)` place lent to a `&` of exactly what it refines (repr.md's tier 1). The second is not a
 * conversion: the refinement is storage alone, and the lowering puts a narrow reference through a
 * temporary at a position that cannot hold one. `convertValue` admits it at a `&` position only;
 * the builder and both verifiers ask this rather than `sameType` so the three agree.
 */
inline bool fillsParameter(DeclBase base, DeclPtr<Type> supplied, DeclPtr<Type> declared) {
    if(supplied == declared) return true;
    if(!supplied || base[supplied]->kind != Type::Int) return false;

    auto canonical = ((IntType*)base[supplied])->canonical;
    return canonical && canonical == declared;
}

// Whether this is a `@bits(n)` refinement - the question `canonicalInt` cannot answer, since a
// refinement of a type and that type are equally "themselves" once it has run.
inline bool isRefinedInt(DeclBase base, DeclPtr<Type> type) {
    return type && base[type]->kind == Type::Int && ((IntType*)base[type])->canonical;
}

/*
 * Whether an integer is one the **machine** has whole-register instructions for - 32 bits and 64,
 * and the target's word wherever that lands.
 *
 * The set `Bits` is instanced over and the set `mulHigh` is admitted at, and one predicate because
 * it is one fact: a narrow value is some bits of a wider register, so a population count, a leading
 * zero count and the high half of a product are all questions about a *width* that the operand's
 * storage does not have. Answering them would mean counting the register's own padding, and every
 * one of them has a different wrong answer.
 *
 * The word's two ends - 32 and 64 - are both in the set, so `Size` and `USize` join without this
 * having to know which the target is. **A `CodeUnit` does not**, although it is a target width too:
 * its two ends are eight and sixteen, and neither is a width the machine counts in - which is the
 * same reading that declines it a byte swap. `WideInt` is out by its fixed 53. A `@bits(n)`
 * refinement answers as the type it refines, which is `canonicalInt`'s rule everywhere else and is
 * no different here.
 */
inline bool hasMachineWidth(DeclBase base, DeclPtr<Type> type) {
    auto canonical = canonicalInt(base, type);
    if(!canonical || base[canonical]->kind != Type::Int) return false;

    auto& integer = *(IntType*)base[canonical];
    if(integer.target == intrinsic::TargetWidth::Word) return true;
    if(integer.isTargetWidth()) return false;

    return integer.bits == 32 || integer.bits == 64;
}

/*
 * Whether an integer's bytes are a set this operation can name - `ByteSwap`'s family.
 *
 * `hasMachineWidth` with sixteen added, and the difference between the two questions is what each is
 * about. A count reads the *value*, so it needs a width the machine counts in; a reversal reads the
 * value's **bytes**, and a sixteen-bit value has two of them however wide the register holding it is.
 *
 * What is out is out for the same reason in both: `U8` has one byte and nothing to exchange,
 * `WideInt` is fifty-three bits and therefore not a whole number of bytes of anything, and a
 * `CodeUnit`'s two ends are eight and sixteen - so one target would have an answer and the other
 * would not, which is not something an instance set may depend on. The target's *word* is in, both
 * of whose ends have the instruction.
 */
inline bool hasByteOrder(DeclBase base, DeclPtr<Type> type) {
    auto canonical = canonicalInt(base, type);
    if(!canonical || base[canonical]->kind != Type::Int) return false;

    auto& integer = *(IntType*)base[canonical];
    if(integer.target == intrinsic::TargetWidth::Word) return true;
    if(integer.isTargetWidth()) return false;

    return integer.bits == 16 || integer.bits == 32 || integer.bits == 64;
}

/*
 * A number as this integer type holds it - the low `bits` of it, sign-extended where the type is
 * signed.
 *
 * The **normal form**, which `resolve_ssa/lower/width.cpp` states for a value in a register and this
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
 *
 * **A number, or the one expression that produces one.** `lanesOf` is the second state: the natural
 * vector count of a lane type this stage cannot measure yet - `Vec(a)` inside a body generic in `a`,
 * whose count is `vectorBytes / sizeOf(a)` and is therefore decided at the specialization that binds
 * `a`. See `Types::naturalLanesType`, where the choice of an expression over a per-position default
 * is argued.
 */
struct ConstType: Type {
    ConstType(U64 value, DeclPtr<Type> type, DeclPtr<Type> lanesOf = nullptr):
        Type(Const), type(type), lanesOf(lanesOf), value(value) {}

    DeclPtr<Type> type;

    /*
     * The lane type whose natural vector count this stands for, and null for a number that is
     * already one. `value` means nothing while this is set - there is no number yet.
     */
    DeclPtr<Type> lanesOf = nullptr;

    U64 value;

    // Whether this is the expression rather than the number. A walk that reads `value` has to ask,
    // which is what makes the second state cost something to ignore rather than nothing.
    bool isDerived() const { return lanesOf != nullptr; }
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

    /*
     * Whether a value of the type holds a **host handle** - on JavaScript, a raw pointer in a record
     * that is not `@local`, which is what `data String {host: %U8}` and `data Collator {host: %U8}`
     * are there. Never set on native.
     *
     * A sixth field and not a subtraction from `trivialCopy`, because the two answer different
     * questions and the target is right to answer the first one differently. A host string is
     * immutable and the host collects it, so duplicating the handle *is* a copy, and a `String` that
     * was not `trivialCopy` would need storage at every borrowed position - `lib/Core/String.yana`
     * says what that cost. What the target may not change is what a program *means*, and a `let &`
     * of a place is a loan or a second value according to whether the type is `TrivialCopy`
     * (`doc/spec/ownership.md`, "A second name for an owned value"). Natively a `String` owns its
     * run and is not, so `let &d = s` then `d = "bcd"` writes `s`; read off this target's
     * `trivialCopy` it would be a copy, and the same program would answer differently on the two
     * targets. `bindsAsCopy` below is the binding's question, and this is the half of it the target
     * does not get to decide.
     */
    bool hostHandle = false;

    // Whether a `let` of a place of this type makes a second *value* rather than a loan - the spec's
    // `TrivialCopy` row read the same on every target. See `hostHandle`.
    bool bindsAsCopy() const { return trivialCopy && !hostHandle; }

    bool needsTeardown() const {
        return reclaim != TeardownKind::None || drop != TeardownKind::None;
    }

    /*
     * Whether an authored `Drop` answers for the **whole** value - `doc/spec/ownership.md`'s
     * "Authored teardown", K5 (J210).
     *
     * `drop(->value: T)` owns its subject, so what the body does not consume is torn down at the end
     * of the body, member by member at each member's own teardown (`analyze_drop.cpp`, and the
     * conversion's `emitDrop` for a subject that dies whole), and a request that runs the authored
     * member runs **nothing after it** - no derived `Reclaim` walk over the members. The split this
     * replaces ran the authored body and then reclaimed every member from outside: a member with a
     * `Drop` of its own never had it run (J209's `Fd`), and one the body moved out was reclaimed a
     * second time. The body knows what it consumed and the caller cannot, so the body is the one
     * that finishes the value.
     *
     * Not where the `Reclaim` is authored too: that instance already answers for the members'
     * storage and runs after the `Drop`, so the value is not the `Drop` body's to finish. No type in
     * `lib/` or `test/` declares both.
     */
    bool dropOwnsValue() const {
        return drop == TeardownKind::Authored && reclaim != TeardownKind::Authored;
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
/*
 * The parameter names a function type was **written** with at one declared position - `cb: (count:
 * Int) -> {}` - which is what a call through the value that position holds may name
 * (`Resolver::valueCallNames`; doc/spec/types.md, "Function types").
 *
 * Beside the position and not in the type, deliberately. `FunType` is interned on identity and a
 * parameter name is not part of it, so `(count: Int) -> {}` and `(n: Int) -> {}` are one pointer:
 * names stored on it would be whichever spelling was interned first, read back by every other (J139).
 * Interning a names-annotated view equal to it for identity was the alternative, and it is two
 * pointers for one type everywhere a pointer comparison *is* the type comparison - specialization
 * keys, instance selection, `sameType` - which is every reader this stage has. So the type stays
 * name-free, unification and interning never see a name, and the names travel with what an author
 * wrote a type *at*: an `Arg`, a `RecordField`, a function's result, a `Global` and a body `Local`.
 * A read of one of those carries them to the call; a `let` or a capture copies them on.
 *
 * Empty for a position whose type is not a function type written out - including one written
 * without names, `(Int, Int) -> Int`, which is how a type says its arguments are positional. One
 * entry per parameter otherwise, the empty name standing for a position written without one. Only
 * the *outermost* function type of a position is read: `(x: Int) -> (y: Int) -> Int` names `x`, and
 * what its call returns is a value no declaration wrote.
 */
using CallNames = DeclList<StringId, false>;

struct RecordField {
    StringId name {};
    Occurrence occurrence;
    LocationId source = kNullLocation;
    U16 index = 0;

    // The names the field's function type was written with - `CallNames`. Set for a declared
    // record's field only: a tuple's fields are part of its interned identity, so names on one would
    // be the first spelling's (`tupleType` clears them). A lambda's environment copies them from
    // the binding it captured.
    CallNames callNames;

    /*
     * Whether the *declaration* named this field - false for the one-payload form, `Proceed(a)` and
     * every newtype, whose `name` above is the constructor's rather than a field name of its own.
     *
     * A bit rather than a null name, because the name is what a dump prints and what a pattern
     * binding reads back; and a bit rather than "the name is a `ConID`", because whether a field can
     * be *selected* is a fact the declaration settled and not one to re-derive from spelling. It is
     * what `.0` tests: `doc/spec/types.md`'s "the content is the one unnamed field the constructor
     * carries, so `.0` reads it".
     */
    bool anonymous = false;

    /*
     * The field's storage is an owning non-null pointer to a value of `occurrence.type`, rather than
     * that value - `doc/spec/repr.md`'s `@box`, and the same statement `compiler/resolve`'s
     * `Field::boxed` made.
     *
     * Two features produce one and the spec keeps them apart on purpose:
     *
     *  - **automatic indirection**, written by `breakLayoutCycles` at the back edge of an
     *    inline-containment cycle, because a type that reaches itself by containment has no finite
     *    size and *every* occurrence of it must be a pointer. Nobody chose it, so nothing in the
     *    source names it;
     *  - **`@box`**, written on a field whose storage should be out of line for reasons having
     *    nothing to do with cycles.
     *
     * Both are per *declaration* and uniform over every value of the enclosing type, which is what
     * makes it legal for them to change the ownership classification - a boxed field costs
     * `TrivialCopy` and gives its owner a derived `Reclaim` - where an inferred layout variant may
     * not.
     *
     * **The field's type is unchanged, and so is its place.** `cfg.cold` on a `@box Diagnostics` is
     * a `Diagnostics`, `fieldPlace` returns the same owned projection it returns for an inline
     * field, and every ownership walk above this - move paths, drop placement, the loan checker -
     * sees no difference. That is the one thing this stage does differently from the old one, which
     * appends a `Deref` to the path and carries the boxed edge through every place walk in the
     * compiler. Here the indirection is a **layout** fact: `fieldAddress` loads through the slot
     * instead of adding to it, construction allocates, and teardown hands the storage back. The
     * place a boxed field names *is* the box's contents, and the address of those contents is the
     * pointer stored in the slot - which is why releasing the storage a place occupies and releasing
     * the box are the same operation.
     *
     * Part of a tuple's interned identity, for the reason `TypeLayout` is: `{Tree}` boxed and
     * `{Tree}` unboxed have different layouts, so they have to be two types or one gets the other's.
     */
    bool boxed = false;

    /*
     * `@host` - the field is not stored, and the value the record holds answers the property under
     * this field's own name (Implementation-Containers.md §14's elision).
     *
     * Recorded and read by nothing here, deliberately: on a native target the field is laid out
     * exactly as it would be without the attribute, so the fact has no representation consequence
     * this stage could implement. It is a JS-visible *source* fact - the JS pipeline is what elides
     * the slot - and dropping it in the resolver would leave that pipeline nothing to read.
     *
     * Not part of the interned identity that `boxed` is part of, and that is the difference between
     * them said in one line: `@box` changes what a field's slot *is* on every target, and this one
     * changes nothing on this one.
     */
    bool host = false;

    /*
     * The field is **storage** a record keeps its own occupancy of - `InlineArray`'s `slots`, the
     * elements of an `@inline(n) @capacity(n) [T]` (`known.def`'s `inlineArray`, J143).
     *
     * Only a prefix of it holds values, and which prefix is the record's other field. So no derived
     * walk descends into it - `tearsDownField` answers false - and the record's own `Reclaim`
     * releases exactly the live elements; the classifier reads what the element owes from it and
     * nothing else (`own.cpp`). A literal writes only the elements it has, which is the one
     * aggregate that supplies fewer components than a fixed array's length (`verify.cpp`).
     *
     * Set where the fields of the declaration claiming the role are read, and copied into every
     * instantiation with the rest of the field.
     */
    bool storage = false;

    /*
     * The field holds the **address of the lender's storage**, whatever its occurrence says about
     * the value that storage holds - a continuation environment's captures, and nothing else.
     *
     * The occurrence cannot say it alone. A capture's contract carries the binding's *form*, because
     * on JavaScript the storage of a shared window local holds a view object and a field spelled at
     * the owner asks it for the wrong one (J70). But a field `Array(a) [unique slot n view]` is, by
     * every other record's reading, a **writable window by value** - an iterator record's `&xs: [a]`
     * argument is exactly that and is sixteen bytes inline - and `manifestNative`'s view row answers
     * before its reference row, so natively the capture was laid out and filled as a copy of the
     * window while `captureOf` read the first word back as the storage's address. The optimizer
     * scalar-replaced the environment away and hid it; `-no-ir-opt` segfaulted.
     *
     * A bit rather than a fourth access form, because it is a fact about the *record* - the same
     * contract names a window by value in an iterator record and a pointer here - and rather than a
     * test for "this record is an environment", because the one site that makes such a record is
     * the one that knows, and every layout reader then asks the field. Read by the native layout
     * (`membersOf`) and its construction (`Inst::Aggregate`); JavaScript's representation is keyed
     * on the occurrence and wants the form it already reads.
     */
    bool holdsAddress = false;

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
 * A property of the **declaration** and never of an instantiation, which is the reasoning of
 * `compiler/resolve`'s `computeRecordLayout` (deleted in J173) reproduced: a generic body projects
 * into `Outcome(a, e)` before any `a` is known, so the shape a projection is emitted against has to
 * be the one every instantiation has. Deciding it from the constructor list alone gives that -
 * substituting an argument cannot move a constructor from having a payload to not having one.
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
 * A **declared pin** on a record's physical layout - `@layout(c)`, `@layout(js)`,
 * `@layout(packed)`, and the default.
 *
 * `doc/spec/repr.md` puts this in a tier of its own: a pin is uniform across every instance of the
 * type, it is visible in the declaration, and changing one is an API change. `Auto` is the target's
 * to decide - it may reorder fields, co-pack narrow ones, scalarize the whole record and fold a
 * discriminant into a niche. `C` and `Js` each freeze that: fields stay in declaration order, only a
 * written `@bits` refinement is a bit-field, and no target may improve on either.
 * `Packed` keeps declaration order too, but makes the record a byte stream: ordinary fields have no
 * alignment padding and narrow fields must fill every bit of the bytes they occupy.
 *
 * **A statement about the type and never about which target is emitting.** All three names mean
 * something on every native target - `@layout(js)` natively is declaration order with no packing,
 * `@layout(c)` on a host target is the same from the other side, and `packed` is the same byte stream
 * wherever the native representation can serve it - which is why this lives on the logical type
 * here, above every family's layout table, rather than being a switch one of them reads.
 */
enum class TypeLayout: U8 {
    Auto,
    C,
    Js,
    Packed,

    // A name `@layout` did not recognize, which is a reading of the attribute and never a type's
    // answer. A row here rather than a `Maybe` at the one caller, so that the reader has one return
    // type and the refusal is a value in it.
    Unknown,
};

/*
 * One constructor of a record - `Proceed` of `data Outcome(a, e) = Proceed(a) | Exit(e)`.
 *
 * **The fields stay one flat list on the record** and a constructor names a range of it. That is
 * the one decision this representation is built on, and what it buys is that every walk already
 * written against a record keeps working unchanged: the ownership fold, the schema fold, the layout
 * prefix, `RecordField::index` and the projection that names one are all about a field index, and a
 * field index here identifies its constructor as well. The alternative - a payload *type* per
 * constructor, which is what `compiler/resolve`'s `Constructor::content` was - makes every one of
 * those walks either two levels deep or a special case, and adds a `Downcast` step to a place path
 * whose only job is to say what the field index already said.
 *
 * What the range does *not* say is where the fields sit, because the arms share their storage: two
 * constructors' fields overlap, and `fieldOffsetNative` is the one place that knows it.
 */
struct RecordCon {
    /*
     * Which constructor this is, counting from zero, and the number a value of it carries.
     *
     * Two fields because `@value(n)` separates them - `doc/spec/declarations.md`. The index is the
     * position in `RecordType::cons` and is what a construction and a pattern name each other by;
     * the value is what is *stored*, and it is somebody else's ABI wherever an attribute wrote one.
     * A declaration that pins nothing has `value == index` everywhere, which is what makes the
     * unpinned case the general one rather than a second rule.
     *
     * The value leads because it is the one eight-byte member: after the name and the source it
     * cost six bytes of padding, and this is twenty-four bytes rather than thirty-two.
     */
    I64 value = 0;

    StringId name {};
    LocationId source = kNullLocation;
    U16 index = 0;

    // The half-open range of `RecordType::fields` this constructor owns.
    U16 first = 0;
    U16 count = 0;

    // Whether an attribute wrote that value - which is not `value != index`, since pinning a
    // constructor to its own position is legal and says something. Read by the check that `@value`
    // needs a constructor carrying nothing, which is about what the author asked for.
    bool pinned = false;
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

    /*
     * A field of this declaration did not resolve, so the list below is missing it - and saying
     * "has no field called x" about a record whose declaration plainly writes one is a second
     * report of the first mistake, in a file that has none. A recovering compile reaches those
     * uses (`mir::recovering`), and `Resolver::quietAboutBroken` is what keeps them quiet.
     * Carried to an instantiation for the reason `pin` is.
     */
    bool broken = false;

    /*
     * Two constructors of this declaration are pinned to one number - "A and C are both 2", which
     * the declaration walk has reported. Kept because a compile that reported still provides the
     * `Enum` bodies and still resolves matches over the type, and each of those building a map
     * would hand `addMap` a repeated selector; they read this and take the chain instead. Set where
     * the collision is found rather than searched for by each of them, since that walk has already
     * compared every pair. Carried to an instantiation for the reason `pin` is.
     */
    bool valuesCollide = false;

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

    /*
     * `@layout(c)` or `@layout(packed)` as it was written - `TypeLayout`, and the one thing a
     * declaration says about its own *physical* form.
     *
     * Here rather than in a layout table because it is the declaration's statement and not a
     * target's answer: every family's table reads it, and a pin that lived in one of them would be a
     * pin the other did not honour. `pinned()` is what a table asks for `C` and `Js`; `Packed` has
     * its own no-padding walk below the same declaration-level choice.
     *
     * An **instantiation** carries its declaration's pin, because a pin is part of the type: it is
     * copied in `instantiate` beside the fields, on `exported`'s reasoning inverted - nobody wrote
     * anything on `Boxed(I32)`, and what it is, is what `Boxed` said it is.
     */
    TypeLayout pin = TypeLayout::Auto;

    // Whether the declaration froze this record's layout - see `pin`, where the two names are one
    // question to everything below. Asked rather than compared, so that a third pin is a change here.
    bool pinned() const { return pin != TypeLayout::Auto; }

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
     * declaration walk, because it is what the author wrote; `schema` is filled by `foldSchemas`,
     * because a field whose type is another record contributes *that* record's slots at a deeper
     * path and the fold has to see both declarations. That pass runs **before** signatures, which is
     * what lets a result's elided interior positions be read off the record it names.
     *
     * Null is the empty shape, which is every record whose fields are all owned - and that is the
     * overwhelming majority, which is why the handle is null rather than an empty list.
     */
    U16 originSlots = 0;
    DeclPtr<OwnershipSchema> schema = nullptr;

    // `foldRecordSchema`'s state: 0 not folded, 1 being folded, 2 folded, 3 folded and refused. A
    // field whose type is another record contributes that record's slots (J141), so the fold asks
    // for the inner one on demand, and `foldSchemas` skips a record some other record's fold already
    // reached. It starts every other record over, but not a refused one: its sentence is out, and a
    // second fold would print it again.
    U8 schemaFolded = 0;

    // `hidesLoans`' memo: 0 not asked yet, 1 being asked, 2 no, 3 yes. Filled on demand rather than
    // by `foldSchemas`, whose walk is flat and would see `Array(Store)` before `Run(Store)`.
    U8 hiddenLoans = 0;

    // Set by `classifyTypes` when it has folded this record. Not a cache - the fold runs exactly
    // once, and this is what a cycle in the member graph is detected with.
    bool classified = false;

    /*
     * Set by `breakLayoutCycles` when it has walked this record's members and boxed whichever of
     * them were back edges - `RecordField::boxed`.
     *
     * Not a cache either, and for a sharper reason than `classified`'s: the walk *changes* the
     * record, so running it twice over one declaration would ask whether a field it already boxed is
     * a back edge, find that it is not - a boxed field is an indirection - and leave the answer
     * alone. Correct, and only by accident. What the flag actually buys is that a declaration
     * reached from two roots is walked once, which is what keeps the pass linear in the member graph
     * rather than exponential in the number of ways to reach a type.
     */
    bool layoutBroken = false;

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
     * A **site type**: the lifted function a value of this record is the environment of, and the
     * shape calling one has - `doc/inflight/Design-Iterator-Objects.md` §4, built 2026-09-15.
     *
     * A lambda's environment record already existed (`resolve_lambda.cpp`); what these two fields
     * add is that the *code* is known from the type, so a value of one can be called without a
     * `{code, env}` pair ever being built. `lambda` is argument zero's own callee and `lambdaShape`
     * is the `(A) -> B` the erased form has - the position's type with the result the body decided
     * written in, which is what `closeFunPosition` answers and what an adaptation produces.
     *
     * Two fields rather than one, because the shape is not derivable from the declaration: a lifted
     * function's first parameter is the environment, so `funTypeOf` of it would answer a signature
     * one position longer than anything a program may write. Deriving it by dropping that parameter
     * is the same fact stated twice, and the position is where it came from.
     *
     * Null on every other record. A lambda written in a **generic** body (C3) makes a generic
     * record - the declaration shares the body's context and *is* the value's type there - and each
     * instantiation carries the lifted function specialized at its arguments and the shape
     * substituted, which `fillInstantiation` writes, so every reader below sees a plain site type.
     */
    DeclPtr<Function> lambda = nullptr;
    DeclPtr<Type> lambdaShape = nullptr;

    /*
     * An **iterator site type**: the `iter fn` a value of this record is the suspended state of, and
     * the step function that advances it - `doc/inflight/Design-Iterator-Objects.md` §3, built
     * 2026-09-15.
     *
     * The iterator's half of `lambda` above, and it is two handles for the same reason that one is:
     * the code is known from the type, so `it.next()` is a direct call and no handle, no stack and
     * no allocation is involved. `producer` is the declaration the author wrote and is what a
     * diagnostic names; `producerStep` is the function `resolve_ssa` converts the producer's body
     * into a second time, and is null until `convertProgram` has made it - nothing in `resolve_mir`
     * calls it, because the call it emits names the record and the *lowering* resolves it.
     *
     * `producer` is the **specialization**, never the generic: one `iter fn` generic in `a` has one
     * site type per argument list, so what the record names is the declaration whose body would be
     * converted at those types - `bodyOwnerOf` and `substitutionOf` are what read it back, exactly
     * as they do for any other call target.
     *
     * Null on every other record, and never copied by `instantiate`: a site type has no parameters
     * of its own. A construction inside a generic body makes an `openSite` record instead, whose
     * `producer` is the producer read at the body's variables and which a substitution replaces
     * with the concrete record rather than instantiates.
     */
    DeclPtr<Function> producer = nullptr;
    DeclPtr<Function> producerStep = nullptr;

    /*
     * `f$new` - the construction, kept here as well as on the resolver's own `iterSites`.
     *
     * The call site finds it by asking the resolver; a **nested** traversal cannot, because what
     * builds the child is the outer producer's step and that is emitted one tier down, where
     * `iterSites` does not exist. So the handle lives on the type beside the other four, which is
     * also what lets the reachability walk root a child's constructor from the outer record.
     */
    DeclPtr<Function> producerNew = nullptr;

    /*
     * The lent value, borrowed from the record - `f$item`, and null for a producer that hands over by
     * value.
     *
     * A declaration rather than a field read, because what it is for is its **signature**: the result
     * is rooted in the record, so the next step needs the record whole and is refused while the item
     * is live. `iter(T)$item` makes the same statement about the erased form, where what it is
     * handed is the address a step answered rather than a field of a record in hand.
     */
    DeclPtr<Function> producerItem = nullptr;

    /*
     * `f$take` - the item **moved** out of the slot, and null unless the record carries `$held`.
     *
     * A declaration for the reason `f$item` is one, said about a move rather than a loan: a `for`
     * that reads the field directly takes the value out of a path the analysis then sees used again
     * on the back edge, because nothing in MIR says that the next step refills it. A call hides the
     * move inside a body whose `&self` says only that the record was written - which is what the
     * step itself already says - and clears `$held` in the same place, so the two facts cannot come
     * apart.
     */
    DeclPtr<Function> producerTake = nullptr;

    /*
     * `f$failure(&self) -> ->E`, where the producer declared `| E` - P2, J216. The failure moved
     * out of `$failure` and `$index` set to finished in the same breath, which is what makes a
     * failure terminal: the step that failed left the record at `kIterFailed`, and the next step
     * of a record that answered its failure is the dispatch's fallback - `Done`.
     *
     * A call rather than a field read for `f$take`'s reason: a consumer that moved out of a path of
     * the record would have to say that nothing reads the path again, and only the step knows.
     */
    DeclPtr<Function> producerFailure = nullptr;

    /*
     * Where the payload starts, and what the three fixed members are - the field order §3.1 states.
     *
     * `$index` is field 0, `$slot` is field 1, the captured arguments are `[2, payload)` and the
     * payload is `[payload, fields.size())`. A count rather than a second list, because the fields
     * *are* the list and a member's identity is its index in it.
     */
    U16 payloadFirst = 0;

    /*
     * `$held` - the flag that says whether the slot still holds a value the record owes, or zero
     * where it never does.
     *
     * Only an **owning** hand-over of a type with a teardown needs one: the slot then holds a value
     * the consumer will take, and the index cannot say whether it has - a record suspended at a
     * `yield` looks the same whether the item was moved out of the slot or left in it. So the step
     * sets the flag as it writes, every consumer that takes clears it, and the teardown drops the
     * slot where it is still set.
     *
     * At the **end** of the field list rather than beside the index, which is the one place §3.1's
     * order is not followed: `$index` is 0, `$slot` is 1 and capture `i` is `2 + i` in five walks
     * across two tiers, and a flag threaded through the middle would renumber all of them to say
     * something no walk but the hand-over's reads. `payloadLocals` stays parallel to the fields from
     * `payloadFirst` because the flag comes after the payload rather than before it.
     */
    U16 heldField = 0;

    /*
     * Whether `$slot` holds a **window** - the descriptor's own two words, where the producer hands
     * over `[a]` or a shared `String`.
     *
     * The third thing a slot can hold, beside the value and a lent value's address, and it is its own
     * row for `IterCaptureKind::View`'s reason exactly: a window is already a value, so what the
     * record keeps is its bytes, and the address of a window is the address of two words that belong
     * to whoever built them. Storing the *lender's* address there instead is what made
     * `iter fn chunks(self: [a]) -> [a]` hand its consumer a descriptor laid out as an owner.
     *
     * A flag rather than a comparison against `viewRecordOf`, because three readers ask it - the
     * `for` over a held object, `Iterate`'s steppers and the nested traversal's item - and a
     * re-derivation is how the field and the reader come to disagree about what is in it.
     */
    bool slotWindow = false;

    /*
     * Whether what the slot points at may be **inside this record** - a lending producer whose
     * `yield` names one of its own payload members held by value, or a borrow it bound out of a
     * nested traversal of that kind. `items` lends the caller's elements through a window and is not
     * one; `iter fn boxes() -> Box` yielding a `let b = Box {...}` is.
     *
     * One reader: the producer that *nests* this one, which may not keep such an item across its
     * own `yield` - the item would then be an address inside a record that moves with whatever holds
     * it (`refuseHeldNestItems`, J131). The item is good for as long as the consumer holds it within
     * one step, which is what a standalone object's consumer does.
     */
    bool itemInRecord = false;

    /*
     * Whether the producer is a **`lens fn`** rather than an `iter fn` - a one-hand-over site type,
     * minted for a lens region a producer runs over part of its own body
     * (`doc/inflight/Design-Iterator-Objects.md` §6.6, built 2026-09-16).
     *
     * A flag rather than `Function::iterator` read off `producer`, because `producer` is the
     * *specialization* and `specializeFunction` does not carry that marker (J53 §1.5). Three
     * readers ask it and each would otherwise ask the generic: `typeText` spells the record
     * `around.Lens` instead of `around.Iter`, the mint declines to generate an `Iterate` instance
     * for something no consumer iterates, and the step conversion reads it to say whether falling
     * out of the embedded region is the next element or the lens's cleanup.
     */
    bool lensSite = false;

    /*
     * Whether this is the site type of an object built **inside a generic body at that body's own
     * variables** - P16, J223. `readBlockCursor(left)` inside `fn (Source(ls, le)) equalInputs(&left:
     * Input(ls, le), ...)` has no concrete producer to mint from, so the record is minted from the
     * producer read under the body's substitution (`producer` is then an *open* clone that no table
     * interns), shares the body's `gen` the way C3's lambda environment does, and is what the
     * generic body's own analyses read: its constructor's borrows, its steppers' signatures, its
     * `Iterate` instance. Nothing converts it. `substituteType` turns it into the site type minted
     * at the specialization's arguments (`Program::openSites`), and `callTarget` turns a call of one
     * of its generated functions into the same member of that record.
     *
     * The rejected alternative was an instantiation (`f.Iter(ls, le)`) whose fields are substituted
     * per argument list, as a lambda's environment is: a site record's *payload* is decided by the
     * producer's body read at those types - which members share a field, what is copied and what is
     * borrowed - so an instantiation of the open record would be a second layout of the same object
     * that nothing but the mint may decide.
     */
    bool openSite = false;

    /*
     * How each captured argument travels - `resolve_object.cpp`'s `IterCaptureKind`, one entry per
     * written parameter of the producer.
     *
     * Recorded rather than re-derived, for `RecordType::lambdaShape`'s reason: the decision is made
     * once, at the mint, out of the parameter's declared contract, and the step conversion has to
     * make the *same* one - a second reading of one contract is how the field and the storage the
     * step gives the producer's local come to disagree about whether the record holds the value or
     * its address.
     */
    DeclList<U8, false> captureKinds;

    // The one row of `IterCaptureKind` the other tier has to name: the field holds the lender's
    // address rather than the value's own bytes, which is what decides whether the step's storage
    // for that local is the field or what is in it. Spelled here rather than repeated as a literal,
    // since `resolve_ssa` cannot see the enumeration and a bare `!= 0` was wrong the moment a third
    // kind arrived.
    static constexpr U8 kCaptureAddress = 1;

    // And the other row the dumps name - the field holds a window's own view record.
    static constexpr U8 kCaptureView = 2;

    // The first captured argument's field: `$index` and `$slot` come before it (`payloadFirst`).
    static constexpr U16 kCaptureFirst = 2;

    // How the field at `index` holds a captured argument - one of the rows above, or 0, the value
    // itself, for a field outside the captures. The index arithmetic stated once, for the two dumps
    // that name the kind beside the field.
    U8 captureKindAt(DeclBase base, U16 index) {
        if(index < kCaptureFirst || index >= payloadFirst) return 0;
        return captureKinds.get(base, index - kCaptureFirst);
    }

    /*
     * Which local of the producer's body each payload member is, and which `yield`s keep it alive.
     *
     * Parallel entries, one per payload member: the first is what the step conversion reads to give
     * a local its storage; the second is one bit per `yield`, in the order they appear in the body,
     * and is what the `.ssa.expect` dump and `yana explain` print - §7 stage 3's item 5.
     *
     * A **bitmap** and not a word, since J52. `payloadYields` is flat - `payloadYieldWords` words for
     * entry 0, then as many for entry 1 - because a list of lists here would be an allocation per
     * member for a set whose usual size is one bit. What the width bought is the design's own
     * headline: §5 says fifty `yield`s over three locals is four words, and a thirty-two bit mask
     * made fifty `yield`s a refusal rather than a record.
     */
    DeclList<U32, false> payloadLocals;
    DeclList<U64, false> payloadYields;

    // How many hand-overs the producer has, and how many words of `payloadYields` each member
    // therefore owns. Both are on the record because every reader of the bitmap needs them and
    // neither can be recovered from the producer without walking its body again.
    U16 handovers = 0;
    U16 payloadYieldWords = 0;

    // Whether hand-over `handover` keeps payload entry `entry` alive - the one question the bitmap
    // answers, asked by the step conversion, the `.ssa` dump and `yana explain`.
    bool keepsAcross(DeclBase decl, U32 entry, U32 handover) {
        if(handover >= handovers) return false;

        auto word = payloadYields.get(decl, Size(entry) * payloadYieldWords + handover / 64);
        return (word >> (handover % 64)) & 1;
    }

    /*
     * And which **field** each of those entries lives in - §3.2's third step, slot sharing.
     *
     * Parallel to the two lists above and no longer derivable from `payloadFirst`, which is the
     * whole of what sharing changed here: two members whose live ranges never meet are one field,
     * so the payload is the largest set at once rather than the sum and the entry list is longer
     * than the field list. A field an entry does not own is named by an earlier entry, and the
     * field's *name* is that first member's - the dump prints the rest beside it.
     *
     * A list rather than a count for the reason `payloadLocals` is one: the step's storage walk asks
     * "which field is this local in" and the constructor asks "which field is this run's pointer
     * in", and neither may re-derive the answer from a position.
     */
    DeclList<U16, false> payloadFields;

    /*
     * The **boxed** nest members - Design-Iterator-Objects.md §6.3's recursion, built 2026-09-16.
     *
     * One entry per `kNestMember` payload entry, in payload order: the `g$erased.heap` shim the
     * member's pair points at, or null where the member holds the child's own record inline. A boxed
     * member is the one thing about a nest the field's *type* cannot say - the field is the protocol
     * `iter(Y)` rather than a record - so the child the step has to build is named here, and the
     * shim's own `generatedRecord` is that record.
     *
     * Filled by `completeBoxedNests` rather than by the mint, and the delay is mutual recursion:
     * `walkA` mints `walkB` inside itself, so when `walkB`'s member boxes `walkA` there is no
     * `walkA.Iter` yet. Every record in the program is minted by the time that pass runs.
     */
    DeclList<DeclPtr<Function>, false> nestBoxed;

    /*
     * And which of them **delegate** - Design-Iterator-Objects.md §6.3's `yield from`, built
     * 2026-09-16.
     *
     * One entry per nest member, in payload order, beside `nestBoxed`: 1 where the region is the
     * re-yield shape over a child in this producer's own recursion cycle, so that entering it is a
     * suspension of this level rather than a loop that steps the child. Such a member's field is not
     * the protocol `iter(Y)` but `linkType` - the pair beside one word of chain - because the level
     * the consumer's trampoline has to come back to is the one *above* the child, and that word is
     * the only place there is to say which.
     *
     * A list rather than a bit on `nestBoxed`'s entry for that list's reason: the step's conversion
     * asks both questions of one region and neither is derivable from the other - a boxed member may
     * be stepped here (a pass that does more than re-yield) and a delegating one is always boxed.
     */
    DeclList<U8, false> nestDelegates;

    /*
     * `f$walk` - the **trampoline**, and null on a record with no delegating nest.
     *
     * What every consumer of a delegating object steps through: `f$next`, `f$resume`, the `for` over
     * a held record and the erased shim all call this rather than `f$step`, because a step of one of
     * these records may answer "I have entered a child" and only this function knows what to do with
     * that. It answers where the item is - the erased step's own channel - since the item it hands
     * back is the innermost level's and is not in this record's slot at all.
     */
    DeclPtr<Function> producerWalk = nullptr;

    /*
     * `f$erased.step` - the **raw** shim a delegable box's pair points at, and null everywhere else.
     *
     * `f$erased` answers two rows and this answers three. A box built for a delegating region is
     * stepped only by a trampoline, so its pair names this; a box built for any other region, and
     * every pair a consumer holds, names `f$erased` - which is what keeps the third row off every
     * channel that has no use for it, and is why a non-delegating step costs nothing at all.
     */
    DeclPtr<Function> producerRaw = nullptr;

    /*
     * `f$erased.heap` - the ordinary heap shim over this record, kept beside the raw one so that a
     * delegating region can reach both.
     *
     * A delegating level builds its child with the raw shim on the path it delegates on and with
     * this one on the path it does **not** - the departure, where the consumer is gone and the child
     * has to be stepped here or not at all. Null until something has asked for the shim.
     */
    DeclPtr<Function> producerBox = nullptr;

    /*
     * `{iter(Y), %Word}` - what a **delegating** nest member holds, and null on every other record.
     *
     * The child's pair and one word naming the level above it, in that order and adjacent, because a
     * trampoline that has just been handed the address of one of these has to find both from it: the
     * pair is what it steps and the word is where it goes back to when that level runs out. Interned
     * as a tuple, so every level of a cycle - which is several record types and several sizes - hands
     * over one shape.
     */
    DeclPtr<Type> linkType = nullptr;

    /*
     * `$down` and `$inner`, the two words delegation adds to a record that has one - both zero where
     * it has none.
     *
     * `$down` is the step's **third answer**: a level that has just entered a delegable child writes
     * *which of its own link members* it entered here and returns `True` as it would for a hand-over,
     * and `f$erased.step` is the one reader - it clears the word, resolves it against its own `self`
     * and answers the address with the low bit set. A member rather than a second result, because
     * that is the one form of the third row that costs a step which never delegates nothing at all:
     * no existing channel changes width and no existing consumer grows a test.
     *
     * `$inner` is the trampoline's own state - the innermost level, or zero for "this record is the
     * innermost". It is a word rather than a pointer because nothing owns what it names: the level it
     * points at is owned by the level above it, all the way up to this record.
     *
     * **A chain word has two readings**, and the second is what lets a suspended delegating object be
     * *moved* - `let moved = it`, an argument taken by `->`, a `return`, a store into a field. With
     * bit zero clear it is the address of a link inside a heap block, which never moves; with bit
     * zero set it is `field << 1 | 1`, one of the reader's own link members, resolved against
     * whatever `self` the step was entered with. The root's own link is the one link in a chain that
     * lives in storage a consumer can relocate, and an absolute address into it was left naming where
     * the object used to be - J60's own revisit item. `ssa::iterLinkAddress` is the resolution and
     * the invariant that makes one tagged reading enough.
     */
    U16 downField = 0;
    U16 innerField = 0;

    /*
     * `$failure`, last, where the producer declared `| E` - 0 otherwise (P2, J216).
     *
     * Typed `Outcome(exit, E)`, the value the producer's own body returns, so the step's `return`
     * is one store of what it was handed rather than a switch that takes the wrapper apart first -
     * the tag is read back out of the field once the store is done. Never initialized and never read
     * except where `$index` says the step failed, which is the payload's own rule.
     */
    U16 failureField = 0;

    /*
     * The bit in a `payloadLocals` entry that says the field holds the member's **run address**
     * rather than its bytes - `@heap` on the producer's binding, Design-Iterator-Objects.md §6.1.
     *
     * Named here rather than written as a literal in `resolve_ssa`, for `kCaptureAddress`'s reason:
     * the step's storage walk and the constructor both read it and neither can see the enumeration
     * the mint keeps. The high bit beside it is `kIterValueMember`, which is the mint's own and says
     * the entry is a node rather than a local.
     */
    static constexpr U32 kHeapMember = 0x40000000;

    /*
     * And the bit that says the field holds the member's **address** because the member is a loan.
     *
     * `let held = base` inside a producer is a second name for storage rather than a second object,
     * and what the record has to carry across a hand-over is which storage. The lender is one the
     * refusal in `checkObjectBody` has already vouched for - `@heap`, or a parameter - so the
     * address stays good while the object moves.
     */
    static constexpr U32 kBorrowMember = 0x20000000;

    /*
     * And the bit that says the payload member is a **nested traversal** - `for x in g(...)` written
     * inside the producer, Design-Iterator-Objects.md §3.2's first step.
     *
     * The field holds `g.Iter` itself: the outer object keeps the child's whole state, so a `yield`
     * written inside the nested pass suspends the outer object with the child suspended inside it.
     * The entry's payload is the **node** index of the `lens.call` that opened the region rather
     * than a local, because a nested traversal is not something the producer's body names - which is
     * the same reading `kIterValueMember` gives a value that crosses a hand-over without a name.
     *
     * A nested member never shares a field: it is live from the call that initializes it to the
     * exit that cancels it, and a shared field would be one child's state read as another's.
     */
    static constexpr U32 kNestMember = 0x10000000;

    /*
     * The native layout, memoized by `ssa::reprOf` - and the one thing on a type that *is* a cache.
     *
     * Null until the walk has run. What it buys is the nesting: a record holding a record asks for
     * the member's layout to place it, so a query on a type three deep would otherwise re-lay out
     * every type below it, and every field of every one of those.
     *
     * Here rather than in a table beside the lowering, because the answer is a property of the type
     * and outlives every body that asks - and because the alternative is a map keyed by a type,
     * which is the shape `compiler/resolve`'s offset walk grew out of.
     *
     * **One slot per target family**, which is why it is named for the family rather than called
     * `repr`: a host target's answer is not a `ssa::Repr` at all, and a shared slot would be the
     * struct with two readings that `resolve_ssa/lower/repr.h` is written to avoid.
     *
     * A bare offset rather than a `DeclPtr`, because the thing it points at is a layout table's and
     * a logical type may not name one - a `data` that had to include `repr.h` would be the coupling
     * this whole arrangement removes. Zero is absent, on the same convention every handle here uses.
     */
    U32 nativeRepr = 0;

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

    // A convention makes this a raw code pointer rather than a closure pair.  It is part of the
    // type's identity because an indirect call must use the convention the pointer promises.
    Maybe<LowerCallType> convention = Nothing();

    // Non-embedding, because every reader wants the positions as a buffer - matching, substitution
    // and the interning key each walk them by index against a second list of the same length.
    DeclList<FunParam, false> params;
    Occurrence result;
};

/*
 * `iter(T)` - the erased iterator object, see type.def.
 *
 * Four facts and they are one statement about a step. `handover` is what a step hands the consumer,
 * at the convention the producer declared - which is what separates `iter(T)` from `iter(->T)` and
 * is why the type carries an occurrence rather than a bare type. `resume` is what a `yield`
 * evaluates to (`{}` where nothing was written) and `sent` is what the step's first argument
 * therefore is - `Outcome(R, exit)` where there is a resume channel and the bare exit signal where
 * there is not, which is J49's pair of wrappers read at the erased boundary.
 *
 * `step` is the **function type the pair is called across**: `(sent, Bool) -> %Y`, answering where
 * the item is and null when the producer is done. A field rather than a derivation, because it is
 * what `addCallValue` needs at every step site and what the value's two words are manifested from -
 * a second derivation is how a call and a layout come to disagree about a signature.
 *
 * Two rows, and it stayed two when §6.3's delegation added a third thing a level can say. A pair a
 * consumer holds names `f$erased`, which answers these two; a pair a **delegable box** holds names
 * `f$erased.step`, which may also answer a child's link with the low bit set - and only a trampoline
 * ever steps one of those. So the signature is one signature and which shim is behind it is what
 * decides whether the third row can arrive at all.
 *
 * `lends` and `window` are what the *slot* holds, carried here because the step's answer is an
 * address either way and the consumer has to know what it points at. The answer is **always the
 * slot's address**: a lending producer's slot holds the value's address, which the consumer loads, a
 * window producer's holds a descriptor, and everything else holds the value. A lending producer
 * used to answer the value's own address, one load earlier - which an `&U8` item makes odd, and
 * an odd answer is what the two tagged rows below are told apart by (the review of J218). The slot
 * is laid out at least two-aligned for the same reason (`membersOf`). `RecordType::slotWindow` and
 * the slot field's pointer type are the same two questions asked of a site type.
 *
 * `failure` is `E` of `iter(T | E)` (C1c, J218), null where the protocol cannot fail, and `failed`
 * is `Outcome(exit, E)` - the type of the `$failure` member every record behind such a pair has. A
 * failure is the step's **third consumer-facing row**, and it travels on the address channel rather
 * than beside it: `f$erased` answers `&$failure | 1`, which no item answer can be because an item
 * answer is a slot's address and a slot is at least two-aligned - `Outcome`'s own four-alignment
 * is what makes the *failure's* address untaggable, and is only half of the argument; J218 stated
 * it as the whole, and an odd lent `&U8` read as a failure. That is the tag bit delegation
 * uses one shim over, and the two never meet on one channel - a trampoline steps `f$erased.step`,
 * which answers a failure only when its own level re-asks for it (`emitIterErasedRaw`). Keyed on,
 * because `iter(T)` and `iter(T | E)` accept different consumers.
 */
struct IterType: Type {
    IterType(): Type(Iter) {}

    Occurrence handover;
    DeclPtr<Type> resume = nullptr;
    DeclPtr<Type> sent = nullptr;
    DeclPtr<Type> step = nullptr;
    DeclPtr<Type> failure = nullptr;
    DeclPtr<Type> failed = nullptr;
    bool window = false;
    bool lends = false;
};

// Whether a producer handing over `handover` **lends** it - its slot holds the value's address.
// `resolve_object.cpp` has the argument; the erased `IterType::lends` asks the same question.
bool objectLends(DeclBase base, const Occurrence& handover);

/*
 * `[T *n]` - see type.def, where the argument for it being a kind is written out.
 *
 * **The count is held two ways and exactly one of them is set.** `count` names a const type
 * variable where the length is quantified (`fn (n: Int) firstOf(xs: [Int *n])`); otherwise it is
 * null and `length` is the number. The alternative - one `DeclPtr<Type>` naming either a
 * `ConstType` or a variable, which is what `compiler/resolve` carried - has to answer "which
 * integer type is the `4` of `[I32 *4]`", and every answer to that is arbitrary: a head declared
 * `n: I64` binds `ConstType(4, I64)` and one declared `n: I32` binds `ConstType(4, I32)`, so a
 * written `[I32 *4]` would intern as a third type that matches neither. Holding the number as a
 * number has no such question - `matchType` binds a const variable to a `ConstType` of *the
 * variable's own* declared type, which is the one place the type of a count is decided and the only
 * place it needs to be.
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

    /*
     * The result a lambda has not decided yet - one variable, owned by nothing.
     *
     * A lambda that writes its own parameter types is a signature with no result (`resolveLambda`),
     * and what says so has to be a type the *position* machinery already understands: G7c's open
     * result is a type variable of the callee's context, `isConcreteType` answers false for one, and
     * `closeFunPosition` is what writes the body's answer back into the shape. So this is that same
     * state said by a lambda rather than by a callee, and it is a variable for that reason rather
     * than a kind of its own.
     *
     * **Owned by nothing on purpose.** A substitution matches on the owner, so a variable no context
     * declared is one nothing can bind - which is what keeps a placeholder that outlived a reported
     * body from being filled in by somebody else's specialization.
     */
    DeclPtr<Type> undecided;

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
     * The natural lane count of a lane this stage cannot measure - the `ConstType` whose number is
     * an expression over `lane` rather than a number.
     *
     * **An expression and not a default filled at the substitution**, and the two were weighed.
     * `Vec(a)` inside a body generic in `a` is handed to `fn (n: Int) horizontalSum(v: Vec(a, n))`,
     * and `n` is a *type argument of that call* - it travels in the call's argument list, is
     * numbered by the callee's schema and is substituted at every instantiation. A default would
     * have had to be re-derived by whatever read the slot, which is three readers (the specializer's
     * const slot, the conversion's `GenValue`, and the erased witness) each having to know that this
     * particular number came from a vector; an expression is decided in the one place substitution
     * already runs and every reader below it sees a plain number.
     *
     * `of` is the const parameter's own declared integer type, exactly as a written number's is.
     */
    DeclPtr<Type> naturalLanesType(DeclPtr<Type> lane, DeclPtr<Type> of);

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
    DeclPtr<Type> funType(DeclBase base, const FunParam* params, Size count, const Occurrence& result,
                          Maybe<LowerCallType> convention = Nothing());

    /*
     * `iter(T)` - interned on the whole protocol, for `funType`'s reason: nothing declares one, so
     * two spellings that describe the same steps have to be one handle.
     *
     * The key is the hand-over occurrence, the resume type and the step signature. `sent` and
     * `window` are derived from those three at the mint rather than keyed on - two protocols that
     * agree on all three agree on both - which keeps the key the same three fields `sameType`
     * compares.
     */
    DeclPtr<Type> iterType(DeclBase base, const Occurrence& handover, DeclPtr<Type> resume,
                           DeclPtr<Type> sent, DeclPtr<Type> step, bool window,
                           DeclPtr<Type> failure = nullptr, DeclPtr<Type> failed = nullptr);

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

    // And for the erased iterator objects, on the same terms - see `iterType`.
    HashMap<U64, DeclPtr<Type>> iterators;

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
#define YANA_MIR_TYPE(kind, Struct) case Type::kind: return f((Struct&)type);
#include "type.def"
#undef YANA_MIR_TYPE
        case Type::KindCount: break;
    }

    assertTrue(false);
    return f((UnitType&)type);
}

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
    // has nothing to tell apart. A stored width rather than a logical one because that is what a
    // discriminant *word* costs; what a target may co-pack an enum-layout record into is the
    // narrower number, and it is `Repr::scalarBits` rather than this - a logical width here would
    // be one this tier had no way to check against the storage the tag is written through.
    U8 bits = 0;

    // Set where a constructor was pinned below zero - see `tagRangeOf`, where the consequence is
    // argued. What it changes is the tag's own type, which is then signed like any other integer.
    bool isSigned = false;
};

TagRange tagRangeOf(DeclBase base, RecordType& record);

// The width `class Enum(a -> w)` answers at - see the definition, where the rule is argued. Read off
// `tagRangeOf` so that "which numbers this declaration has" is answered once.
DeclPtr<Type> enumValueTypeOf(DeclBase base, Types& types, RecordType& record);

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

/*
 * Which constructor of `lib/Core`'s `IterStep(e, y)` is which - `IterateFailing`'s answer (C1b).
 *
 * Found by what each carries on the **declaration** - the one holding `y`, the one holding `e`, and
 * the one holding nothing - rather than by position, for the reason `iterateInstanceFor` finds
 * `Maybe`'s `Just` that way: the order the library writes them in is not a second fact the compiler
 * should hold. Reading the declaration rather than an instantiation is what tells the two carrying
 * constructors apart at `IterStep(I32, I32)`. `0xffff` for a row the type does not have.
 */
struct IterStepRows {
    U16 yield = 0xffff;
    U16 done = 0xffff;
    U16 failed = 0xffff;
};

IterStepRows iterStepRows(DeclBase base, DeclPtr<Type> type);

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
 * Null on refusal, which every caller propagates - there is no error type in this stage. `report`
 * false asks the same refusals as a question rather than raising them. A replacement's condition
 * (Design-Replacements.md §3.3) asks by silencing the diagnostics instead, because what it asks is a
 * whole class requirement and this is one of the many rules proving one can reach.
 */
DeclPtr<Type> makeVectorType(Context& context, Program& program, DeclPtr<Type> lane, U64 lanes,
                             bool isMask, LocationId source, bool report = true);

// The same, for a caller holding a count that may be a const variable - which is every path that
// substitutes an existing type rather than reading a written one. A null count is the natural form,
// exactly as a zero is above.
DeclPtr<Type> makeVectorType(Context& context, Program& program, DeclPtr<Type> lane,
                             DeclPtr<Type> count, U64 lanes, bool isMask, LocationId source,
                             bool report = true);

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

/*
 * Whether a boxed field is *actually* an indirection - `RecordField::boxed` narrowed by what the
 * field turned out to hold.
 *
 * A box around nothing is nothing. A unit field occupies no storage, so there is nothing to
 * allocate, nothing to point at and no address to keep stable - and the write that would have filled
 * the pointer is elided, which would leave the derived teardown handing back whatever the frame
 * happened to contain.
 *
 * Asked here rather than cleared where the flag is set, because the case that reaches it is a
 * *substitution*: nobody writes `@box ()`, and `data Holder(a) {v: @box a}` at `a := ()` gets it
 * without asking. Clearing it at the declaration would leave `Holder(a)`'s own generic body
 * disagreeing with the instantiation about whether the field is a pointer.
 *
 * The one exception to reading `field.boxed` directly, and every walk that lays out, projects,
 * constructs or releases a field asks this instead.
 */
inline bool indirectField(DeclBase base, const RecordField& field) {
    return field.boxed && base[field.occurrence.type]->kind != Type::Unit;
}

/*
 * Whether a *derived* teardown of the enclosing record descends into this field: the record has to
 * own it, and there has to be something to run. Both halves, because either one alone is a walk into
 * a field that has no business being reached - see `ownsField`.
 *
 * A **boxed** field always has something to run, whatever its target owes: the allocation the box is
 * has to be handed back, and there is nowhere else for that to happen. That is the same sentence
 * `includeBoxedMember` makes one level up, where it gives such a record a derived `Reclaim`
 * unconditionally - and the two have to agree, or a record would be classified as owing a release
 * that this walk then declines to emit.
 */
inline bool tearsDownField(DeclBase base, const RecordField& field) {
    if(!ownsField(field) || field.storage) return false;
    return indirectField(base, field) || hasTeardown(base, field.occurrence.type);
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

/*
 * Whether a value of this type holds loans at positions its schema cannot name - the **element**
 * position of a container.
 *
 * `[Store]` over `data Store {held: kept'Cell}` holds every element's `held`, but the elements are
 * behind the run's `%a`, and a schema is a list of *paths*: a path through a pointer names one
 * pointee, not `length` of them, and a fixed array's elements are not fields at all ("Elements are
 * not fields"). So `Array(Store)` has no schema and, before this question, no origin position - its
 * elements' loans were dropped at the store and a write of the lender was admitted while the array
 * was read.
 *
 * What answers it is structural, as ownership.md says qualification is: a record whose owned field -
 * through any number of pointers - is a type with a schema or one that hides loans itself, and a
 * fixed array whose element is. A loan field is not followed: what is inside the lender is the
 * lender's business, and the loan of it is already a position. The origin analysis gives such a
 * value one more position, after its schema's slots - `originPositions` - and everything written
 * anywhere under it goes there.
 *
 * Refusing these instead - the answer `foldRecordSchema` gives `data Outer {inner: Reader}` - would
 * make `[Store]` unwritable; the fold refuses an owned field there because the *field* has a spelling
 * that names the slot, and an element has none.
 */
bool hidesLoansWalk(DeclBase base, DeclPtr<Type> type);

// Inline for the answer every local and value of every body asks for: a scalar, or a record already
// decided.
inline bool hidesLoans(DeclBase base, DeclPtr<Type> type) {
    if(!type) return false;

    auto found = base[type];
    if(found->kind == Type::Record) {
        auto memo = ((RecordType*)found)->hiddenLoans;
        if(memo >= 2) return memo == 3;
    } else if(found->kind != Type::Array) {
        return false;
    }

    return hidesLoansWalk(base, type);
}

// Whether a value of this occurrence refers to storage somebody else owns - a borrow, or a record
// with an external field. What decides whether a local carries an origin set even though it owns
// what is in it.
inline bool carriesOrigins(DeclBase base, const Occurrence& occurrence) {
    return !occurrence.contract.isOwned() || shapeOf(base, occurrence) != nullptr ||
           hidesLoans(base, occurrence.type);
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
 * A container of loan-carrying values has one more, last: the element position `hidesLoans`
 * describes, which every element's positions are folded into. One rather than one per element slot,
 * because the elements are not a path and a store into one of them is not told which.
 *
 * The root is a position even where the occurrence is **owned** and it is therefore always empty. It
 * costs a row and it buys the property every mapping over these positions relies on: a place and the
 * value read out of it have the same count, because both are `1 + slots(type)`. A representation
 * that dropped the root for an owned value would make a read of a whole record a renumbering.
 */
inline U32 originPositions(DeclBase base, const Occurrence& occurrence) {
    auto schema = shapeOf(base, occurrence);
    auto hidden = hidesLoans(base, occurrence.type);
    if(occurrence.contract.isOwned() && !schema && !hidden) return 0;

    auto count = schema ? 1 + U32(base[schema]->slots.size()) : 1;
    return hidden ? count + 1 : count;
}

// The position `hidesLoans` adds - the last one - or zero where the type hides nothing. Zero is the
// root, which is the conservative place for anything that has no position of its own. The count
// `originPositions` makes before adding it, rather than a call to it: that would ask `hidesLoans`
// a second time.
inline U32 elementPosition(DeclBase base, const Occurrence& occurrence) {
    if(!hidesLoans(base, occurrence.type)) return 0;
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
 * Whether an occurrence's shape **takes a member away** - `Maybe('v)` against `Maybe(v)`.
 *
 * The one test that tells a **qualification** from a statement about *provenance*, and the
 * difference decides every other question about the value. `Reader(a')` says which of this
 * signature's origin sets fills a position `Reader` already has, and `Maybe(%s'U8)` says whose
 * storage an owned pointer points at: same members, same layout, same teardown, and only the extent
 * differs. A qualified type argument replaces a member with a reference to somebody else's - the
 * payload of a `Maybe('v)` is not this value's - so the layout differs, and so does what dies with
 * it.
 *
 * **A field the record owns, held at this occurrence as a borrow**, which is that sentence written
 * as the thing it decides: `membersOf` lays a record out through `fieldContract`, so a position
 * that changes one is a position that changes the bytes, and one that does not cannot. Three
 * readers rest on it and they have to be one test - `ownershipOfOccurrence` for what a value owes,
 * `checkQualifiedOccurrence` for what the declaration may say, and `checkCarriedOrigins` for where
 * such a value may go - because a site that decided this differently from its neighbour is how a
 * value laid out one way came to be read as the other.
 *
 * It was a **count** - more slots than the type's own schema - and the count is an approximation of
 * this that is wrong in both directions. `Maybe(%s'U8)` adds a slot and takes no member, so a value
 * that still owed a teardown was answered trivial; and the count cannot see that a record whose own
 * schema has as many slots has had a different one of them replaced.
 *
 * A **record**, because a field is the only interior position a qualifier can take away, and a
 * walk over the shape rather than over the fields: a shape is one or two slots and a record is not.
 */
inline bool qualifiesInterior(DeclBase base, const Occurrence& occurrence) {
    if(!occurrence.shape) return false;
    if(base[occurrence.type]->kind != Type::Record) return false;

    auto& record = *(RecordType*)base[occurrence.type];

    for(auto slotPointer: base[occurrence.shape]->slots.contents(base)) {
        auto& slot = *base[slotPointer];

        // One field step and nothing after it. A deeper path reaches through the member rather than
        // standing in for it, which is what makes `Maybe(%s'U8)` compose and `Maybe(Boxed('v))` the
        // case `resolveOccurrence` refuses.
        if(slot.path.size() != 1) continue;

        auto step = slot.path.get(base, 0);
        if(PathStep::isPointee(step)) continue;

        // And a field the *declaration* owns. A record that already reaches this position through
        // one of its own slots is being renumbered, not emptied - `Reader(a')`, whose `held` is
        // somebody else's however this occurrence numbers it.
        if(ownsField(record.field(base, PathStep::fieldIndex(step)))) return true;
    }

    return false;
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
 * The second half is `residueOwnership`, a fold over the members the qualification left - which
 * was a constant until P15 (J187), when `checkQualifiedShapes` admitted only a residue that owed
 * nothing. It now admits one that owes a derived teardown, and that teardown is `convert.cpp`'s to
 * run field by field; what the check still refuses is a record whose teardown is authored. What it
 * does *not* check is an authored `copy` of the whole value - that is a call, and
 * `checkCarriedOrigins` refuses the argument, which is the third reader of `qualifiesInterior` and
 * the reason it is one test.
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
/*
 * Whether this occurrence's environment word is **not this value's** - `Shapes::closure` and
 * `Shapes::noEnvironment`, which are the two ways of saying it.
 *
 * `qualifiesInterior`'s sentence read at the one kind that is not a record: a function value's
 * second word is a member of the pair, and either shape is a position that takes it away. A
 * frame-placed closure owes nothing because the environment is a local of the function that built
 * it; a plain function value owes nothing because the word is null; and a heap-placed closure
 * carries no shape at all and owes its block. An erased iterator object is the same three readings
 * of the same two words.
 *
 * Its own function rather than an arm of `qualifiesInterior` because the two answers differ in the
 * other half: a record whose member was replaced by a borrow is trivially copyable, and a function
 * value never is - it may hold an exclusive loan, so its uses stay moves at every placement.
 */
inline bool holdsNoOwnedEnvironment(DeclBase base, const Occurrence& occurrence) {
    if(!occurrence.shape) return false;

    // ... and an **erased iterator object**, which is the same pair over the same word - a
    // frame-placed one points at a record this function owns, and the drop that would test the tag
    // and do nothing is not emitted at all. See `mir::IterType`.
    if(base[occurrence.type]->kind != Type::Fun &&
       base[occurrence.type]->kind != Type::Iter) {
        return false;
    }

    for(auto slotPointer: base[occurrence.shape]->slots.contents(base)) {
        auto& slot = *base[slotPointer];
        if(slot.path.size() != 1) continue;

        auto step = slot.path.get(base, 0);
        if(!PathStep::isPointee(step) &&
           PathStep::fieldIndex(step) == kFunEnvironmentComponent) {
            return true;
        }
    }

    return false;
}

/*
 * What a qualified occurrence's **surviving members** owe - `classifyTypes`' member fold, over the
 * fields the shape did not take away.
 *
 * `Result(String, '[U8])` still owns its error, so the value is not trivial: it moves, and where it
 * dies `convert.cpp`'s `emitResidueDrop` releases that field and leaves the borrowed one alone. The
 * common case - every wrapper whose members are exactly its parameters, `Maybe('v)` - folds to
 * nothing and is the trivial answer it always was. A record with an authored teardown, and a boxed
 * survivor, never reach here: `checkQualifiedShapes` refuses both where the type is written, because
 * the first takes the whole value and the second's box is released by the type's own walk.
 */
inline Ownership residueOwnership(DeclBase base, const Occurrence& occurrence) {
    auto& record = *(RecordType*)base[occurrence.type];

    Ownership residue;
    residue.relocatable = record.ownership.relocatable;

    for(auto field: record.fields.contents(base)) {
        if(fieldPosition(base, occurrence.shape, field.index)) continue;
        if(!ownsField(field)) continue;

        auto inner = ownershipOf(base, field.occurrence.type);
        residue.trivialCopy = residue.trivialCopy && inner.trivialCopy;
        residue.explicitDrop = residue.explicitDrop || inner.explicitDrop;
        residue.hostHandle = residue.hostHandle || inner.hostHandle;
        if(inner.reclaim != TeardownKind::None) residue.reclaim = TeardownKind::Derived;
        if(inner.drop != TeardownKind::None) residue.drop = TeardownKind::Derived;
    }

    return residue;
}

inline Ownership ownershipOfOccurrence(DeclBase base, const Occurrence& occurrence) {
    if(qualifiesInterior(base, occurrence)) return residueOwnership(base, occurrence);

    // A function value whose environment word is not its own - the same statement as above, at the
    // one kind whose interior is not a field. `trivialCopy` stays false, which is why this is not
    // that test.
    if(holdsNoOwnedEnvironment(base, occurrence)) {
        return Ownership { false, true, false, TeardownKind::None, TeardownKind::None };
    }

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
 * Whether `function` is an authored `Drop` whose subject - its first parameter - is the body's to
 * finish: a member of `instance Drop(T)` at a record `T` whose `Ownership::dropOwnsValue` holds.
 * Not an authored `Reclaim`, and not an iterator object's `$drop`, whose step is the producer's own
 * departure path and already releases every member it wrote (J57).
 */
bool ownsTeardownSubject(DeclBase base, const Function& function);

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
