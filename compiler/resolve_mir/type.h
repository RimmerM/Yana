#pragma once

#include "contract.h"
#include "../intrinsic/primitive.h"

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
 * An integer of a width and a signedness - `compiler/intrinsic/primitive.def`, which is the roster.
 *
 * The width is in bits. It is *not* the register width the value arrives in - that is a
 * manifestation question - and it is not a declared range either; a refined width belongs on the
 * contract when refinement is implemented, so that `I32` stays one type.
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
    IntType(U8 bits, bool isSigned, intrinsic::TargetWidth target = intrinsic::TargetWidth::Fixed):
        Type(Int), bits(bits), isSigned(isSigned), target(target) {}

    // The narrowest this type is guaranteed to be, and the widest it may turn out to be. Equal for
    // everything that states its own width; see `intrinsic::minBitsOf` for which to ask when.
    U8 minBits() const { return U8(intrinsic::minBitsOf(target, bits)); }
    U8 maxBits() const { return bits; }

    bool isTargetWidth() const { return target != intrinsic::TargetWidth::Fixed; }

    U8 bits;
    bool isSigned;
    intrinsic::TargetWidth target;
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

// No published origin at this position - the answer for an owned root, and for an interior position
// the declaration did not bind.
static constexpr U16 kNoOriginSlot = ~U16(0);

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
 */
inline U16 originSlotAt(DeclBase base, const Occurrence& occurrence, U32 position) {
    auto contract = occurrence.contract;

    if(position) {
        auto shape = occurrence.shape;
        if(shape && position - 1 < base[shape]->slots.size()) {
            contract = base[base[shape]->slots.get(base, position - 1)]->contract;
        }
    }

    return contract.origin() == Origin::Slot ? contract.slot() : kNoOriginSlot;
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
        if(slot.path.size() != 1 || slot.path.get(base, 0) != field) continue;

        return 1 + i;
    }

    return 0;
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
