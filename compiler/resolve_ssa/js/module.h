#pragma once

#include "../inst.h"

/*
 * JSIR - the typed managed-value IR between OSSA and the JavaScript syntax tree.
 * Analysis-IR-JS.md parts 3, 4 and 8.
 *
 * This file is the IR itself: the identities, the representation and shape handles, the values, the
 * places, the instructions and the CFG. `repr.h` decides what a representation *is*, `abi.h` freezes
 * how a call passes one, `build.h` is how a node is made, `convert.cpp` is where SSA becomes one of
 * these, `verify.cpp` is what holds it to its invariants and `print.cpp` is `-mode jsir`.
 *
 * ## The three rules that are this tier's whole reason to exist
 *
 * **Every value has a `JsRepId`.** Not a `mir::Type` - several logical types share one target
 * representation, and one logical value may have a representation no single type node describes. A
 * lowering decision taken from the type is a decision this tier cannot check.
 *
 * **Values and places are separate ID spaces.** A value is an immutable computation; a place is a
 * recipe for reading or writing storage and says which protocol. Only `Load` and `Store` accept a
 * place, so a phi can never denote a slot on one edge and its contents on another - which is the
 * `Value` problem `Analysis-IR.md` is removing one tier up, met again down here.
 *
 * **Nothing falls through to `null`.** A source operation this tier has no answer for is a
 * diagnostic naming the construct at the offending instruction. §4's argument is that the failures
 * this backend has had all look like working JavaScript that computes the wrong number, and the only
 * defence is refusing to emit rather than emitting something.
 *
 * ## Storage
 *
 * A body lives in one reusable arena and is released after the function becomes AST (§7.1). The
 * stable tables - representations, shapes and ABIs - are `JsTarget`'s and live for the compilation.
 * Nothing outside this directory holds a `JsirPtr`.
 */
namespace jsir {

/*
 * The region a JSIR body lives in.
 *
 * Declared here rather than beside the three in `resolve_mir/region.h`, and the reason that file
 * gives for grouping those does not apply: they are grouped because `mir::Function` names a body in
 * two of them, so the declaration header has to see all three whatever anyone does. Nothing in
 * `resolve_mir` or `resolve_ssa` names a JSIR body - the handle never leaves this directory - so the
 * tag belongs where its contents are, and a fourth row over there would be a dependency the include
 * graph does not have.
 */
struct JsirRegion {};

template<class T> using JsirPtr = RegionPtr<JsirRegion, T>;
using JsirBase = RegionBase<JsirRegion>;
template<class T, bool allowEmbed = true> using JsirList = SmallList<JsirRegion, T, allowEmbed>;
using JsirArena = AlignedRegion<JsirRegion>;

/*
 * A plain array's contents as a buffer.
 *
 * The region lists spell this `contents(base)`, and a `SmallArray` - which is what a run of lanes or
 * of parts is held in while it is being built - has no such spelling. Written once here rather than
 * at each of the two dozen hand-over sites in the vector conversion.
 */
template<class T, U32 N> Buffer<T> held(SmallArray<T, N>& values) {
    return Buffer<T> { values.pointer(), values.size() };
}

struct JsValueDomain {};
struct JsPlaceDomain {};
struct JsBlockDomain {};
struct JsInstDomain {};
struct JsRepDomain {};
struct JsShapeDomain {};
struct JsAbiDomain {};

// Body-lifetime, reset for every function.
using JsValueId = Id<JsValueDomain>;
using JsPlaceId = Id<JsPlaceDomain>;
using JsBlockId = Id<JsBlockDomain>;
using JsInstId = Id<JsInstDomain>;

// Program-lifetime, stable while functions are emitted.
using JsRepId = Id<JsRepDomain>;
using JsShapeId = Id<JsShapeDomain>;
using JsAbiId = Id<JsAbiDomain>;

/*
 * What a value *is* on this target - §3.2's vocabulary.
 *
 * `Void` has no value at all and is not `null`: an AST expression may write `null` in a discarded
 * position where the syntax needs one, but that is a spelling and not a JSIR value. The verifier is
 * what holds the two apart.
 *
 * `Nullable` is the niche-folded form - `Maybe(Person)` as `Person | null` - and is produced since
 * J74. What may be one is `nicheFoldOf`'s answer and the nesting rule is `jsAdmitsAbsent`'s: an
 * inner representation that already admits `null` makes one pattern stand for two values, so
 * `Maybe(Maybe(T))` keeps its `$tag` over a folded inner one.
 */
enum class JsRepKind: U8 {
    Void,
    Number,

    /*
     * A host `boolean` - `true` or `false`, and not the number a `Bool` is.
     *
     * The one representation on this target that exists because a *mask lane* is cheapest as one.
     * `Bool` is deliberately the number 0 or 1 (see `NumberDomain::Bool01`), and a mask is the other
     * answer for the reason LLVM's `<N x i1>` is not `<N x i8>`: a mask lane is only ever produced
     * by a comparison, combined by `and`/`or`/`not` and consumed by a select or a reduction, and a
     * host boolean is what each of those four is cheapest over. Spending a mask as `Bool01` would
     * put `? 1 : 0` on every comparison lane and `!== 0` in front of every select's condition, which
     * is `lanes` of both in code whose whole point is that it costs nothing.
     *
     * Not reachable from any logical type: `jsRep` never answers one. It is the lane of a
     * `Mask(a, n)` and nothing else, which is why a value of one meets a scalar only through an
     * explicit `Convert`.
     */
    HostBool,

    BigInt,
    HostString,
    GcObject,
    HostArray,
    Nullable,
    Parts,
    Cell,
    Erased,
};

/*
 * Which numbers a `Number` holds.
 *
 * `Bool01` is the number 0 or 1 rather than a host boolean, which is measured rather than assumed -
 * `codegen/js/type.cpp`'s `isBool` carries the figures, and the deciding one is that a reference to
 * a bit of a packed word reads and writes through a mask that is the identity on 0 and 1 and turns
 * `true` into `1`.
 *
 * `Signed`/`Unsigned` carry the width, and the width is what says which of the host's three integer
 * domains an operation happens in: at 32 bits and below the host's operators, from 33 to 53 the
 * helpers in `codegen/js/wide.cpp`, and above that `bigint`. That is one number rather than three
 * kinds because the *representation* is the same host `number` for the first two bands.
 */
enum class NumberDomain: U8 {
    Bool01,
    Signed,
    Unsigned,
    Float32,
    Float64,
};

// The widest integer a host `number` holds every value of. Above it consecutive integers stop being
// representable and the type is a `bigint`. Stated here because the value tower switches on it.
constexpr U8 kMaxNumberBits = 53;

/*
 * Which typed row a `HostArray` is, or `Plain` for an ordinary `Array`.
 *
 * A property of the *element representation* and not of the container, which is what lets one rule
 * answer it for a `[U8 *n]`, for `Array(U8)`'s storage and for a constant row alike - and what makes
 * a `Uint8Array` reachable by a word transfer whichever of the three built it.
 */
enum class TypedArray: U8 {
    Plain,
    Int8, Uint8, Int16, Uint16, Int32, Uint32,
    BigInt64, BigUint64,
    Float32, Float64,
};

StringView typedArrayName(TypedArray kind);

/*
 * One interned representation.
 *
 * By value everywhere: it is a handful of numbers and two handles, so a copy is words and no caller
 * has to reason about the table growing under it. Parts are a range of `JsTarget::partList` rather
 * than an embedded list, because the ordinary arity is two - a function pair - and an inline array
 * sized for a vector's lanes would cost every scalar rep sixteen bytes it never uses.
 */
struct JsRep {
    JsRepKind kind = JsRepKind::Void;

    // `Number` only.
    NumberDomain domain = NumberDomain::Signed;

    // `Number` and `BigInt`: the declared width, which is what a coercion is emitted against.
    U8 bits = 0;

    // `HostArray` only.
    TypedArray typedArray = TypedArray::Plain;

    // `GcObject` and `Cell`; a `Parts` whose materialized form is an object names it here too.
    JsShapeId shape;

    /*
     * `Nullable`'s payload, `HostArray`'s element, `Cell`'s content - and a `Parts`' **materialized
     * form**, which is the one host value its parts become where a position can hold only one.
     *
     * Invalid for a `Parts` that has none, which is §3.2's "some parts never have a materialized
     * form; a verifier rejects an attempt to build one". Nothing this slice produces is in that
     * state; the field is what makes the refusal a check rather than an assumption.
     */
    JsRepId content;

    /*
     * `Nullable` only: what a discriminant of one *is*, and which field the payload is.
     *
     * Carried on the representation rather than re-derived from `source` at every site, because
     * `JsRep::source` is for dumps and diagnostics and a lowering decision taken from it is the
     * decision this tier exists to move. Three numbers, and they are what make the decode, the
     * projection and the construction one statement: `tag` is the representation a discriminant
     * load of this value produces, `absentTag` and `presentTag` are the two constructors' own
     * values at it, and `payloadField` is the record field `Just` carries.
     *
     * They are part of the interning key, which is not an economy: two two-constructor sums with
     * one payload each may fold into the same `T | null` and number their constructors the other
     * way round, and one representation for both would read every `Nothing` of one as the other's
     * `Just`.
     */
    JsRepId tag;
    I32 absentTag = 0;
    I32 presentTag = 0;
    U16 payloadField = 0;

    // `Parts`: a range of `JsTarget::partList`.
    U32 firstPart = 0;
    U16 partCount = 0;

    /*
     * `Parts` only: whether the parts are **lanes of one value** - a vector or a mask - rather than
     * the components of a record.
     *
     * Two producers of one kind, and the difference decides whether an *operation* distributes over
     * them: `a + b` over two vectors is `lanes` additions, and over two windows it is nothing at all.
     * `convertVectorInst` is the one reader and runs in front of the scalar switch, so without this
     * bit a `represent` between two window occurrences arrived there as a lane-wise conversion of a
     * host array and two numbers.
     *
     * A bit rather than the structural test that was here first - "the materialized form is a host
     * row" - because that one is true of a window whose record happens to be transparent and is a
     * statement about the materialization rather than about the parts.
     */
    bool lanewise = false;

    /*
     * `Parts` only: whether a **materialization may be made again** at every position that asks for
     * one, rather than built once and memoized on the logical value.
     *
     * The memo exists because two rows of one vector are two objects and every identity question
     * downstream then has two answers. A scalarized record of primitives has no such question - it
     * is a value, copied by being read - and the memo is positively wrong for it: a `return` in two
     * arms materializes in whichever block asked first, and the other arm then names a value its own
     * block is not dominated by. Which is the verifier working, and it is where this bit came from.
     */
    bool freshEachUse = false;

    /*
     * How many elements a `HostArray` has, where the *type* says - a `[T *n]`, and zero for a `%a`.
     *
     * Carried because a structural copy needs it: `slice()` duplicates a row of numbers and shares a
     * row of objects, so a fixed array of records is written out element by element. A row whose
     * length this target does not know and whose elements are objects has no copy this tree can
     * spell, and is reported rather than aliased.
     */
    U32 length = 0;

    /*
     * The logical type this was computed from, for dumps and diagnostics and for **nothing else**.
     *
     * Read by `print.cpp` and by the sentences a not-yet diagnostic names a construct with. A
     * lowering decision taken from it would be the decision this whole tier exists to move.
     */
    DeclPtr<mir::Type> source = nullptr;

    bool isObjectLike() const {
        return kind == JsRepKind::GcObject || kind == JsRepKind::HostArray;
    }

    // Whether a value of this representation is duplicated by being read - a number, a bigint, a
    // host string. What `copy.trivial` becomes an alias of rather than a clone of.
    bool isPrimitive() const {
        return kind == JsRepKind::Number || kind == JsRepKind::BigInt ||
               kind == JsRepKind::HostString || kind == JsRepKind::Void ||
               kind == JsRepKind::HostBool;
    }

    // Whether the host's own operators work on it - false for the two bands that need helpers.
    bool isHostInt32() const {
        return kind == JsRepKind::Number && bits <= 32 &&
               (domain == NumberDomain::Bool01 || domain == NumberDomain::Signed ||
                domain == NumberDomain::Unsigned);
    }

    bool isWideNumber() const {
        return kind == JsRepKind::Number && bits > 32 && bits <= kMaxNumberBits &&
               (domain == NumberDomain::Signed || domain == NumberDomain::Unsigned);
    }

    bool isFloat() const {
        return kind == JsRepKind::Number &&
               (domain == NumberDomain::Float32 || domain == NumberDomain::Float64);
    }

    bool isSigned() const {
        return domain == NumberDomain::Signed ||
               (kind == JsRepKind::BigInt && domain == NumberDomain::Signed);
    }
};

/*
 * What one property of a shape is *for*.
 *
 * The role rather than the name, because the name is a spelling and the role is what a reader asks
 * about: `Tag` is what a discriminant read names, `Field` is what a projection names, and the three
 * compiler-owned ones are what a cell, a pair and a header are. Matching on the spelling is what
 * §4.4 forbids and what a renamed property silently breaks.
 */
enum class JsPropertyRole: U8 {
    Field,
    Tag,
    Box,
    Code,
    Env,

    // The two words of a reference descriptor - `{$o, $k}`, the object and the key inside it.
    Owner,
    Key,
};

struct JsProperty {
    // The final identifier, decided when the shape is interned - see `name.h`. A property is not an
    // identifier in a scope, so it needs no disambiguation against locals; it does need to be
    // distinct *within the shape*, which is what the interning arranges.
    StringId key;
    JsRepId rep;
    JsPropertyRole role = JsPropertyRole::Field;

    // Which `mir::RecordType` field this stands for, and `kNoField` for the compiler's own.
    static constexpr U16 kNoField = 0xffff;
    U16 field = kNoField;
};

enum class JsShapeKind: U8 {
    // A record, a tuple, or a sum with its `$tag` in front of the constructors' properties.
    Object,

    // `{$v: ...}` - stable replaceable storage for one value that is not a host object.
    Cell,

    // `{$c, $e}` - the materialized form of a function pair, for the positions that need one value.
    FunPair,

    /*
     * `{$o, $k}` - a reference to storage somebody else owns, as a value.
     *
     * Design.md's tier 2, as this target spells it. Native carries an address; here a place is
     * already an `(object, key)` pair, so a reference to one is that pair reified and read as
     * `r.$o[r.$k]`. Which makes it a genuine reference rather than a copy with a write-back - it has
     * no commit point, so it outlives the call that produced it exactly as the native form does.
     *
     * This is what `&Int` and `&String` are on this target, and the reason a *boxed local* is two
     * objects: the box `{$v}` is the storage and the descriptor names a property of it. A shape that
     * tried to be both would be a reference that can only ever point at a whole local.
     */
    Ref,

    /*
     * The properties a value a **transparent** record elided into already has - its `@host` fields.
     *
     * `Array(Handle)` is the bare host row and `length` is a property that row keeps itself, so a
     * projection into one names a property of a value whose shape this compiler did not decide. A
     * kind of its own rather than an `Object`, because the verifier's most valuable check is that a
     * property owner is a host object this tier laid out - and this is the one family where the
     * owner is legitimately a host array or a host string instead. Answering `Object` would have
     * meant relaxing that check for every shape rather than for the one that earns it.
     */
    Host,
};

/*
 * One hidden-class construction order.
 *
 * Properties are ordered and a construction supplies every one of them, which is §2.3 of
 * Analysis-JS.md read as a representation rule: construction order is this target's field offsets,
 * and a record built two ways in two places is a polymorphic call site for every reader downstream.
 *
 * The shape records the **target property** rather than the source field, which is what the four
 * consumers of the shipping backend's `fieldProperty` each re-derived. `slotOfField` is the one
 * lookup, and it is a linear scan over a handful of entries rather than a map: a record of forty
 * fields is rare and a hash per projection is not.
 */
struct JsShape {
    JsShapeKind kind = JsShapeKind::Object;

    // The record this describes, for dumps and diagnostics. Null for the compiler's own shapes.
    DeclPtr<mir::Type> source = nullptr;

    // Which constructor's payload each property belongs to, or `kShared` - used by the dump and by
    // the construction check, which is what catches a sum built with another arm's property.
    Array<JsProperty> properties;

    U16 slotOfField(U16 field) const {
        for(U32 i = 0; i < properties.size(); i++) {
            if(properties[i].field == field) return U16(i);
        }

        return JsProperty::kNoField;
    }

    U16 tagSlot() const {
        for(U32 i = 0; i < properties.size(); i++) {
            if(properties[i].role == JsPropertyRole::Tag) return U16(i);
        }

        return JsProperty::kNoField;
    }
};

/*
 * How one value crosses a call boundary - §3.7.
 *
 * `Omitted` is `Void`, which occupies no argument slot at all rather than passing `null`; `Cell` is
 * the boxed form a mutable borrow of a non-object takes; `Parts` is the flattened pair; `Erased` is
 * a value whose representation the callee decides.
 */
/*
 * How one declared position is carried - and there is deliberately no `Erased` row.
 *
 * What makes a position erased is a fact about the *declaration* rather than a convention of its
 * own, so it is the `erased` bit below and the kind stays what the position would be anyway: a body
 * compiled once takes its `&a` as a `Cell` and its `a` as `One`, and its caller has to spell the
 * same thing. A fifth kind was declared here and never produced, which is a row a reader has to
 * check every switch against and no switch could ever reach.
 */
enum class JsPassingKind: U8 {
    Omitted,
    One,
    Cell,
    Parts,
};

struct JsPassing {
    JsPassingKind kind = JsPassingKind::One;
    JsRepId rep;

    // How many argument positions this occupies - 0 for `Omitted`, the part count for `Parts`, 1
    // otherwise. Stored rather than derived so that a caller counting positions does not have to
    // reach the representation table for every argument.
    U16 positions = 1;

    /*
     * Whether the **declaration** wrote a type its own context binds at this position - `a`,
     * `Map(k, v)`, `Array(a)`.
     *
     * The rule the erased boundary turns on, and it is about the declaration rather than about any
     * argument list: what makes a position erased is that the signature wrote a variable somewhere
     * in it, so every caller answers it the same way and one plan describes them all
     * (`lower/erase.cpp`'s `erasedPosition`, which is the same predicate).
     *
     * What it buys is that the representation check at a call can be **strict everywhere else**. The
     * rep of `Map(k, v)` and the rep of `Map(I32, String)` are two interned objects with the same
     * property keys in the same order, so a body compiled once reads the caller's object correctly
     * and no comparison of the two ids could say so; before this the waiver was "either side happens
     * to be `Erased`", which let two genuinely different concrete representations past whenever one
     * of them was a type variable's.
     */
    bool erased = false;

    /*
     * Whether the **callee** cannot write what this position names - a shared loan, and nothing
     * else.
     *
     * The one interprocedural fact JSIR's copy-to-alias elision rests on (§6.3): a caller holding a
     * structural duplicate of a value it is about to hand over may hand the original instead only
     * if nothing on the other side writes it, and a shared loan is exactly the declaration saying
     * so. `Owned` is not: a consuming position hands the callee a value of its own, which it may
     * assign to. `Unique` plainly is not.
     *
     * Read off the **declaration** like `erased`, and for the same reason - one plan describes every
     * caller, so a direct call, an indirect one through a signature and a witness cell all answer
     * the question the same way without any of them looking at a body. Storing it here rather than
     * re-deriving it at the rewrite is §2.4's rule read literally: the ownership facts are the
     * resolver's and this tier reads them rather than recomputing them from instruction kinds.
     */
    bool shared = false;
};

/*
 * One declaration's frozen calling convention - §3.7.
 *
 * Computed from the declaration and never from a call site, so that direct, indirect and recursive
 * calls agree by construction rather than by two predicates arriving at the same answer. The
 * verifier compares a call's argument forms against this, which is what makes a disagreement a
 * report rather than a program that returns the wrong number.
 */
struct JsAbi {
    /*
     * A `SmallArray` and not an `Array`, which is `compiler/util/README.md`'s rule for a list that
     * is a field of something there is one of per declaration: eight is what an ordinary signature
     * has, and past it this behaves exactly as an `Array` and has paid the one allocation it was
     * paying anyway. It is safe as an element of `JsTarget::abis`, which grows - the inline buffer
     * is not pointed at from inside the object.
     */
    SmallArray<JsPassing, 8> parameters;
    JsPassing result;

    /*
     * Whether the emitted function's first parameter is a closure environment.
     *
     * A property of the *declaration* - a lifted lambda takes its environment record as argument
     * zero - rather than of a call, which is what lets a function value's code word be the function
     * itself where this is true and a thunk where it is not. `assemble.cpp` is where the thunk is
     * written and this is what it reads.
     */
    bool takesEnv = false;

    /*
     * Whether the emitted function's first parameter is a **generic** environment - a body compiled
     * once, and the table its caller filled.
     *
     * A second flag rather than a reuse of `takesEnv`, because the two words are different things
     * that happen to sit in the same position: a closure environment is a *record the lambda
     * captured* and a generic environment is a *table of answers about types*, and one function can
     * never want both - a lambda over an erased body reaches it through a thunk that supplies the
     * table out of its own captures. Stating them apart is what lets `FunCode` keep asking only
     * about the first.
     */
    bool takesGenEnv = false;

    // The number of emitted parameters, environment included. What a `function f(a, b)` header is
    // built from and what an arity check compares against.
    U32 arity = 0;

    /*
     * One bit per emitted **argument** position, set where the callee may only read what is in it -
     * `JsPassing::shared` with the multi-part positions flattened out.
     *
     * The flattening is a walk over `parameters` and the copy elision asks it once per argument of
     * every call, which makes it quadratic in the argument count and runs twelve times over a body
     * in a full round of that pass. The mask is the same answer as an indexed load. Sixty-four
     * positions, and `sharedAt` walks for a call wider than that rather than answering wrongly.
     */
    U64 sharedMask = 0;

    bool sharedAt(U32 ordinal) const {
        if(ordinal < 64) return ((sharedMask >> ordinal) & 1) != 0;

        U32 position = 0;
        for(auto& parameter: parameters) {
            if(!parameter.positions) continue;
            if(ordinal < position + parameter.positions) return parameter.shared;
            position += parameter.positions;
        }

        return false;
    }

    // Filled once, where the plan is built - see `sharedMask`.
    void sealShared() {
        sharedMask = 0;

        U32 position = 0;
        for(auto& parameter: parameters) {
            for(U16 i = 0; i < parameter.positions && position < 64; i++, position++) {
                if(parameter.shared) sharedMask |= U64(1) << position;
            }

            if(position >= 64) break;
        }
    }
};

/*
 * Every emitted argument **position** of a plan, in order - `f(position, index, parameter, part)`.
 *
 * `positions` is 0 for an omitted parameter, 1 for an ordinary one and the part count for a
 * multi-part one, so "which argument does this parameter's second lane go in" is a running total
 * and not a field. Four sites wrote that total out by hand and a fifth got it wrong: `emitThunks`
 * counted *parameters* rather than positions, so a thunk over a flat reference dropped every
 * argument after the first and the callee read `undefined` (`NarrowRef.yana` under `-no-ir-opt`).
 *
 * The **index** is the parameter's own, because a caller that is naming things wants the declared
 * argument it came from; the **part** is which position within that parameter, which is what a
 * per-part representation is looked up by. `position` excludes the leading environment word - a
 * caller that emits one adds its own offset, since whether there is one is `takesGenEnv`'s.
 */
template<class F> void forEachPosition(const JsAbi& abi, F&& f) {
    U32 position = 0;
    for(U32 index = 0; index < abi.parameters.size(); index++) {
        auto& parameter = abi.parameters[index];
        for(U16 part = 0; part < parameter.positions; part++) f(position++, index, parameter, part);
    }
}

// How many emitted argument positions a plan's declared parameters occupy - what a call's argument
// count is held to, and the arity minus the leading environment word.
inline U32 positionCount(const JsAbi& abi) {
    U32 total = 0;
    for(auto& parameter: abi.parameters) total += parameter.positions;
    return total;
}

/*
 * A place - §3.5, and the whole reason a write cannot bypass one.
 *
 * Six kinds, and the sixth is the one this target has that the document's list did not name.
 * `Value` is storage that *is* a host object: a `&Point` parameter arrives as the object itself, so
 * there is nothing to rebind and a whole-value store writes its properties one at a time. The other
 * five are rebindable, and a store into one is an assignment.
 *
 * Keeping the distinction in the place rather than in the store is what makes it checkable: the
 * shipping backend decides it at each write site by asking `isJsObject` of a source type, which is
 * the predicate `Implementation-JS-Repr.md` centralized once and which four writers still had to
 * remember to call.
 */
enum class JsPlaceKind: U8 {
    // A JSIR-local variable - what an allocation becomes when nothing takes its address away. The
    // slot is an index into the body's variable table and becomes one `var` in the emitted function.
    Var,

    // `box.$v` - what an allocation becomes when its address does escape and its content is not a
    // host object. The box is a value, so a place rooted in it survives the call that made it.
    Cell,

    // `owner.<key>`, where the key is the owner shape's slot rather than the source field's name.
    Property,

    // `owner[index]`.
    Element,

    // A module-level binding.
    Global,

    // The storage *is* this object - see above.
    Value,
};

struct JsValue;
struct JsInst;
struct JsBlock;

struct JsPlace {
    JsPlaceId id;
    JsPlaceKind kind = JsPlaceKind::Var;

    // What reading this produces and what a write into it must be. The verifier checks both ends
    // against it, which is the check that catches a number stored into a cell holding an object.
    JsRepId content;

    // `Var`: the variable slot. `Property`: the slot in `shape`.
    U32 slot = 0;

    // `Property` and `Cell`: the owner's shape, so that a reader has the key without a type walk.
    JsShapeId shape;

    // `Cell`, `Property`, `Element` and `Value`.
    JsirPtr<JsValue> owner = nullptr;

    // `Element` only.
    JsirPtr<JsValue> index = nullptr;

    // `Global` only.
    DeclPtr<mir::Global> global = nullptr;

    /*
     * Whether this place names the **payload of a niche-folded value** stored here rather than the
     * value itself - `Just`'s field, which on this target *is* the storage (§3.2's `Nullable`).
     *
     * A flag rather than a place kind, because the storage is exactly the same storage and the only
     * thing that differs is what a reader of it is entitled to assume: `content` is the payload's
     * representation where the slot, the property or the element holds the nullable. The verifier
     * reads it as a licence for precisely that mismatch and for no other - see `checkPlace`.
     */
    bool nichePayload = false;
};

/*
 * One JSIR value - immutable, and with exactly one representation.
 *
 * `definer` is null for a block argument, whose defining position is the block. `uses` is a count
 * rather than a list: the only consumer in this slice is the AST builder's "may this expression be
 * written at its single use", and a list per value is `Analysis-IR-JS.md` §7.4's "not appropriate
 * for every SSA value". A pass that needs the users builds them once, over the body it is walking.
 */
struct JsValue {
    JsValue(JsRepId rep): rep(rep) {}

    JsValueId id;
    JsRepId rep;

    JsirPtr<JsInst> definer = nullptr;
    JsirPtr<JsBlock> block = nullptr;

    U32 uses = 0;
};

/*
 * What a literal is on this target.
 *
 * Its own small struct rather than `mir::Constant`, because the two ask different questions: that
 * one is a source value of a logical type, and this is a host value of a representation. An
 * aggregate constant has already become an `Object` or an `Array` by the time it reaches here, and
 * there is no `Address` row at all - a host string is its text and has nothing under it.
 */
struct JsConstant {
    enum Kind: U8 {
        Number,
        BigInt,
        Text,
        Null,
        Undefined,

        // A host `true` or `false`, read off `number`. One kind rather than two so that a mask
        // lane's literal is the same node an arithmetic lane's is, with the spelling in one place.
        Bool,
    };

    Kind kind = Number;

    // `Number` and `Bool`: the value. `integral` is set where it has to print without an exponent or
    // a fraction, since the same double is an array index in one place and a float in another.
    F64 number = 0;
    bool integral = true;

    // `BigInt`: the raw sixty-four bits, read as `isSigned` says.
    U64 bits = 0;
    bool isSigned = false;

    // `Text`.
    StringId text {};
};

static constexpr U16 kJsPure = 1;
static constexpr U16 kJsReads = 2;
static constexpr U16 kJsWrites = 4;
static constexpr U16 kJsThrows = 8;
static constexpr U16 kJsControl = 16;
static constexpr U16 kJsAllocates = 32;
static constexpr U16 kJsCalls = 64;
static constexpr U16 kJsTerminator = 128;

/*
 * The result column of inst.def.
 *
 * `kMaybeResult` is read by *not* being either of the other two: `verify.cpp` refuses a
 * `kOneResult` row with no result and a `kNoResult` row with one, and a call whose callee returns
 * nothing has to be allowed both ways. So it is the third value rather than a synonym for
 * `kOneResult`, and deleting it would make every unit-returning call a verifier failure.
 */
static constexpr U8 kNoResult = 0;
static constexpr U8 kOneResult = 1;
static constexpr U8 kMaybeResult = 2;

struct JsEdge {
    JsirPtr<JsBlock> target = nullptr;
    JsirList<JsirPtr<JsValue>, false> arguments;
};

struct JsInst {
    enum Kind: U8 {
#define YANA_JSIR_INST(kind, Struct, mnemonic, result, effects) kind,
#include "inst.def"
#undef YANA_JSIR_INST
        KindCount,
    };

    JsInst(Kind kind, LocationId source): source(source), kind(kind) {}

    JsInstId id;
    LocationId source = kNullLocation;
    JsirPtr<JsBlock> block = nullptr;
    Kind kind;

    // The one value this defines, or null. JSIR has no multi-result instruction: the two SSA rows
    // that have one - a compare-exchange and a split call result - are native, and a JavaScript
    // value that is several things is `Parts` rather than several results.
    JsirPtr<JsValue> result = nullptr;
};

struct JsInstConst: JsInst {
    JsInstConst(LocationId source, const JsConstant& constant):
        JsInst(Const, source), constant(constant) {}

    JsConstant constant;

    template<class F> void eachUse(JsirBase, F&&) {}
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstFunCode: JsInst {
    JsInstFunCode(LocationId source, DeclPtr<mir::Function> callee):
        JsInst(FunCode, source), callee(callee) {}

    DeclPtr<mir::Function> callee;

    template<class F> void eachUse(JsirBase, F&&) {}
    template<class F> void eachEdge(JsirBase, F&&) {}
};

/*
 * The generic environment one concrete call hands over - §3.7's erased boundary, as one name.
 *
 * A **module-level table** rather than a row built here, which is the one decision this node
 * carries. Every cell of it is a compile-time constant - a descriptor, a witness, a number, a
 * function - because this milestone builds an environment only where the argument list at the call
 * is concrete, so a row built at the site would be an allocation per call of a table that never
 * differs. The shipping backend takes the same trade (`genEnvTable` against an interned global) and
 * `mir::GenEnvironment` is already interned on (generic, argument list), so one name per environment
 * is one table in the file.
 *
 * What is in it is `assemble.cpp`'s, for the reason `JsInstTypeDrop` names a function it does not
 * write: the contents are names of other generated things, and one of them may ask for another.
 */
struct JsInstGenTable: JsInst {
    JsInstGenTable(LocationId source, DeclPtr<mir::GenEnvironment> environment):
        JsInst(GenTable, source), environment(environment) {}

    DeclPtr<mir::GenEnvironment> environment;

    template<class F> void eachUse(JsirBase, F&&) {}
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstObject: JsInst {
    JsInstObject(LocationId source, JsShapeId shape): JsInst(Object, source), shape(shape) {}

    JsShapeId shape;

    // One per property of the shape, in shape order and never fewer - which is the check that keeps
    // one record to one hidden class.
    JsirList<JsirPtr<JsValue>, false> values;

    template<class F> void eachUse(JsirBase base, F&& f) {
        for(auto used: values.contents(base)) f(used);
    }

    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstArray: JsInst {
    explicit JsInstArray(LocationId source): JsInst(Array, source) {}

    JsirList<JsirPtr<JsValue>, false> values;

    template<class F> void eachUse(JsirBase base, F&& f) {
        for(auto used: values.contents(base)) f(used);
    }

    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstClone: JsInst {
    JsInstClone(LocationId source, JsirPtr<JsValue> from): JsInst(Clone, source), from(from) {}

    JsirPtr<JsValue> from;

    template<class F> void eachUse(JsirBase, F&& f) { f(from); }
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstAlias: JsInst {
    JsInstAlias(LocationId source, JsirPtr<JsValue> from): JsInst(Alias, source), from(from) {}

    JsirPtr<JsValue> from;

    template<class F> void eachUse(JsirBase, F&& f) { f(from); }
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstLoad: JsInst {
    JsInstLoad(LocationId source, JsPlaceId place): JsInst(Load, source), place(place) {}

    JsPlaceId place;

    template<class F> void eachUse(JsirBase, F&&) {}
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstStore: JsInst {
    JsInstStore(LocationId source, JsPlaceId place, JsirPtr<JsValue> value):
        JsInst(Store, source), place(place), value(value) {}

    JsPlaceId place;
    JsirPtr<JsValue> value;

    template<class F> void eachUse(JsirBase, F&& f) { f(value); }
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstBinary: JsInst {
    JsInstBinary(LocationId source, mir::BinaryOp op, JsirPtr<JsValue> lhs, JsirPtr<JsValue> rhs):
        JsInst(Binary, source), lhs(lhs), rhs(rhs), op(op) {}

    JsirPtr<JsValue> lhs, rhs;
    mir::BinaryOp op;

    template<class F> void eachUse(JsirBase, F&& f) { f(lhs); f(rhs); }
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstUnary: JsInst {
    JsInstUnary(LocationId source, mir::UnaryOp op, JsirPtr<JsValue> value):
        JsInst(Unary, source), value(value), op(op) {}

    JsirPtr<JsValue> value;
    mir::UnaryOp op;

    template<class F> void eachUse(JsirBase, F&& f) { f(value); }
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstCoerce: JsInst {
    JsInstCoerce(LocationId source, JsirPtr<JsValue> value): JsInst(Coerce, source), value(value) {}

    JsirPtr<JsValue> value;

    template<class F> void eachUse(JsirBase, F&& f) { f(value); }
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstConvert: JsInst {
    JsInstConvert(LocationId source, JsirPtr<JsValue> value): JsInst(Convert, source), value(value) {}

    JsirPtr<JsValue> value;

    // Whether the source was read as a signed number where the two representations disagree - a
    // narrowing that has to sign-extend rather than mask. Carried on the node because the source
    // rep alone does not say it for a `Bool01` widening.
    bool fromSigned = false;

    template<class F> void eachUse(JsirBase, F&& f) { f(value); }
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstSelect: JsInst {
    JsInstSelect(LocationId source, JsirPtr<JsValue> condition, JsirPtr<JsValue> whenTrue,
                 JsirPtr<JsValue> whenFalse):
        JsInst(Select, source), condition(condition), whenTrue(whenTrue), whenFalse(whenFalse) {}

    JsirPtr<JsValue> condition, whenTrue, whenFalse;

    template<class F> void eachUse(JsirBase, F&& f) { f(condition); f(whenTrue); f(whenFalse); }
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsMapEntry {
    // The selector value this arm matches, as the host number it compares against.
    F64 from = 0;
    JsConstant to;
};

struct JsInstMap: JsInst {
    JsInstMap(LocationId source, JsirPtr<JsValue> selector):
        JsInst(Map, source), selector(selector) {}

    JsirPtr<JsValue> selector;
    JsirList<JsMapEntry, false> entries;
    JsConstant fallback;

    template<class F> void eachUse(JsirBase, F&& f) { f(selector); }
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstCall: JsInst {
    JsInstCall(LocationId source, DeclPtr<mir::Function> callee, JsAbiId abi):
        JsInst(Call, source), callee(callee), abi(abi) {}

    DeclPtr<mir::Function> callee;
    JsAbiId abi;

    /*
     * The generic environment a body compiled once is entered with, and null for every other call.
     *
     * A field rather than the first of `arguments`, for the reason `ssa::InstCall::target` is one:
     * the argument list is the *callee's parameters*, and a list whose leading entry meant something
     * else is a list every consumer has to offset by a flag. `JsAbi::takesGenEnv` says whether this
     * is set and the verifier holds the two together.
     */
    JsirPtr<JsValue> genEnv = nullptr;

    // The emitted arguments, already in ABI order and already flattened - so a walk over this list
    // is a walk over the emitted call, and nothing below has to re-apply the convention.
    JsirList<JsirPtr<JsValue>, false> arguments;

    template<class F> void eachUse(JsirBase base, F&& f) {
        if(genEnv) f(genEnv);
        for(auto used: arguments.contents(base)) f(used);
    }

    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstCallValue: JsInst {
    JsInstCallValue(LocationId source, JsirPtr<JsValue> code, JsirPtr<JsValue> environment,
                    JsAbiId abi):
        JsInst(CallValue, source), code(code), environment(environment), abi(abi) {}

    JsirPtr<JsValue> code;

    /*
     * The word a call through a pair leads with, and **null** where the value called is not one.
     *
     * Null is the class and function witness dispatch: what a witness cell holds is a concrete
     * function, selected by a caller that knew the instance, so it neither takes an environment nor
     * has one to be given. Every other indirect call here is a `{$c, $e}` pair and leads with `$e`
     * unconditionally, which is what makes one convention serve a lambda and a plain function alike.
     */
    JsirPtr<JsValue> environment;
    JsAbiId abi;
    JsirList<JsirPtr<JsValue>, false> arguments;

    template<class F> void eachUse(JsirBase base, F&& f) {
        f(code);
        if(environment) f(environment);
        for(auto used: arguments.contents(base)) f(used);
    }

    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstHost: JsInst {
    JsInstHost(LocationId source, mir::HostOp op, StringId member):
        JsInst(Host, source), member(member), op(op) {}

    // `mir::NodeHost::member` carried the whole way down - the literal read off the call at
    // expansion. It is a field rather than an operand precisely so that a property access is
    // `o.length` rather than `o[k]` over a string built at run time; §3.10 item 2.
    StringId member;
    mir::HostOp op;

    /*
     * Whether the **host** guarantees a narrower range than the result's representation does.
     *
     * One row reaches this: `hostField(x, "length")`, declared to answer a `Size` - a signed 32-bit
     * number here - where what a host container promises is an unsigned 32-bit one. Without it
     * every reader of a count pays a `>>> 0` that can never do anything.
     *
     * A bit on the node rather than a representation, because it is not one: a `JsRepId` says what
     * the value *is*, the two ends of every edge and every `return` are checked against it, and a
     * narrower claim written there is a value the verifier is right to refuse. And a bit rather
     * than the member name matched at the AST tier, which is where this was until 2026-09-22 (J80):
     * `codegen/js/structure.cpp`'s rule is that it switches on nothing but a `JsRep`, a `JsShape`
     * and a `JsPlace`, and the shipping backend had to say the same sentence beside it.
     */
    bool hostRange = false;

    JsirList<JsirPtr<JsValue>, false> arguments;

    template<class F> void eachUse(JsirBase base, F&& f) {
        for(auto used: arguments.contents(base)) f(used);
    }

    template<class F> void eachEdge(JsirBase, F&&) {}
};

/*
 * Which runtime helper a `Helper` names - §2.3's semantic tag.
 *
 * The roster this target answers arithmetic with where the host has no operator, and the one place
 * the set is written down. Nothing below JSIR recognizes a helper from its generated name, which is
 * §4.4's rule; what the emitter does with a tag is look up the family's slot in `JsShared` and
 * reserve the name, so a helper renamed changes one table and no reader.
 *
 * `FunDrop` sits here rather than beside the teardowns because it is exactly the same kind of
 * thing - a function this compiler writes, called with the value - and giving it a row of its own
 * would be a second mechanism for one shape.
 */
enum class JsHelperOp: U8 {
    // The typed-array pair a float's bits go through, in both directions. The width is the value's.
    FloatToBits,
    BitsToFloat,

    ByteSwap,
    CountBits,
    LeadingZeros,
    TrailingZeros,
    Rol,
    Ror,
    BitsUpTo,
    GatherBits,
    ScatterBits,

    // A function value's release - the pair's two questions, in one shared function.
    FunDrop,
};

StringView jsHelperOpName(JsHelperOp op);

struct JsInstHelper: JsInst {
    JsInstHelper(LocationId source, JsHelperOp op): JsInst(Helper, source), op(op) {}

    JsHelperOp op;
    JsirList<JsirPtr<JsValue>, false> arguments;

    template<class F> void eachUse(JsirBase base, F&& f) {
        for(auto used: arguments.contents(base)) f(used);
    }

    template<class F> void eachEdge(JsirBase, F&&) {}
};

/*
 * A release the site cannot write inline - one call to the type's own teardown function.
 *
 * The one shape that needs it is a **sum** whose walk is derived: which arm a value holds is a
 * run-time fact, so the walk branches on the tag, and a branch is a CFG change in the middle of a
 * block the conversion is filling. Natively that is `teardownHalves`, a function per (type, halves)
 * that every site calls; here it is the same answer and the same key.
 *
 * The two halves travel separately for `emitTeardown`'s reason: an authored instance replaces the
 * derived walk for *its own* half, so a record with an authored `Drop` and a member that owns
 * storage still has the member reclaimed.
 */
struct JsInstTypeDrop: JsInst {
    JsInstTypeDrop(LocationId source, JsirPtr<JsValue> value, DeclPtr<mir::Type> type,
                   bool drop, bool reclaim):
        JsInst(TypeDrop, source), value(value), type(type), drop(drop), reclaim(reclaim) {}

    JsirPtr<JsValue> value;
    DeclPtr<mir::Type> type;
    bool drop = true;
    bool reclaim = true;

    template<class F> void eachUse(JsirBase, F&& f) { f(value); }
    template<class F> void eachEdge(JsirBase, F&&) {}
};

/*
 * N parts as the one host value a position can hold - §3.6's `parts.materialize`.
 *
 * The node names the **`Parts` representation** rather than the shape of the answer, which is the
 * difference between this and the version that only ever served a function pair: a pair materializes
 * into a two-property object and a vector into a row of `lanes`, so "what shape" has two answers and
 * "which multi-part value" has one. `JsRep::content` of `parts` is the form, and a `Parts` with no
 * form at all is what the verifier refuses here rather than at the emitter.
 */
struct JsInstMaterialize: JsInst {
    JsInstMaterialize(LocationId source, JsRepId parts):
        JsInst(Materialize, source), parts(parts) {}

    JsRepId parts;
    JsirList<JsirPtr<JsValue>, false> values;

    template<class F> void eachUse(JsirBase base, F&& f) {
        for(auto used: values.contents(base)) f(used);
    }

    template<class F> void eachEdge(JsirBase, F&&) {}
};

/*
 * One part of a materialized multi-part value - §3.6's `parts.split`.
 *
 * Carries the `Parts` representation for `Materialize`'s reason, and for one more: the materialized
 * form alone does not say which multi-part value it is the form *of*. A row of four `number`s is a
 * `Vec(I32, 4)` and is also an ordinary `[I32 *4]`, and only one of them has parts to name.
 */
struct JsInstSplit: JsInst {
    JsInstSplit(LocationId source, JsirPtr<JsValue> from, JsRepId parts, U16 part):
        JsInst(Split, source), from(from), parts(parts), part(part) {}

    JsirPtr<JsValue> from;
    JsRepId parts;
    U16 part;

    template<class F> void eachUse(JsirBase, F&& f) { f(from); }
    template<class F> void eachEdge(JsirBase, F&&) {}
};

/*
 * Which of the host library's two binary numeric functions a `Math` names.
 *
 * The one pair this target needs and has neither an operator nor a helper for. `Math.min` and
 * `Math.max` are not `a < b ? a : b`: they answer NaN where either operand is one and distinguish
 * the two zeroes, and a vector reduction that used the conditional would disagree with the native
 * backends on exactly those two inputs. So the spelling is the host's own, and a row here is what
 * lets it be a **pure** node - a `Host` row would have carried `host.def`'s conservative effects and
 * made every level of a reduction tree a barrier nothing may move across.
 */
enum class JsMathOp: U8 {
    Min,
    Max,
};

StringView jsMathOpName(JsMathOp op);

struct JsInstMath: JsInst {
    JsInstMath(LocationId source, JsMathOp op, JsirPtr<JsValue> lhs, JsirPtr<JsValue> rhs):
        JsInst(Math, source), lhs(lhs), rhs(rhs), op(op) {}

    JsirPtr<JsValue> lhs, rhs;
    JsMathOp op;

    template<class F> void eachUse(JsirBase, F&& f) { f(lhs); f(rhs); }
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstThrow: JsInst {
    JsInstThrow(LocationId source, JsirPtr<JsValue> value): JsInst(Throw, source), value(value) {}

    JsirPtr<JsValue> value;

    template<class F> void eachUse(JsirBase, F&& f) { f(value); }
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstJump: JsInst {
    explicit JsInstJump(LocationId source): JsInst(Jump, source) {}

    JsEdge to;

    template<class F> void eachUse(JsirBase, F&&) {}
    template<class F> void eachEdge(JsirBase, F&& f) { f(to); }
};

struct JsInstBranch: JsInst {
    JsInstBranch(LocationId source, JsirPtr<JsValue> condition):
        JsInst(Branch, source), condition(condition) {}

    JsirPtr<JsValue> condition;
    JsEdge whenTrue;
    JsEdge whenFalse;

    template<class F> void eachUse(JsirBase, F&& f) { f(condition); }
    template<class F> void eachEdge(JsirBase, F&& f) { f(whenTrue); f(whenFalse); }
};

struct JsSwitchCase {
    F64 value = 0;
    JsEdge edge;
};

struct JsInstSwitch: JsInst {
    JsInstSwitch(LocationId source, JsirPtr<JsValue> selector):
        JsInst(Switch, source), selector(selector) {}

    JsirPtr<JsValue> selector;
    JsirList<JsSwitchCase, false> cases;
    JsEdge fallback;

    template<class F> void eachUse(JsirBase, F&& f) { f(selector); }

    template<class F> void eachEdge(JsirBase base, F&& f) {
        auto items = cases.pointer(base);
        for(Size i = 0; i < cases.size(); i++) f(items[i].edge);

        f(fallback);
    }
};

struct JsInstReturn: JsInst {
    explicit JsInstReturn(LocationId source): JsInst(Return, source) {}

    // Null for a function whose result is `Void` - which is a returned *nothing* rather than a
    // returned `null`, and is the one place the two are easy to confuse.
    JsirPtr<JsValue> value = nullptr;

    template<class F> void eachUse(JsirBase, F&& f) { if(value) f(value); }
    template<class F> void eachEdge(JsirBase, F&&) {}
};

struct JsInstUnreachable: JsInst {
    explicit JsInstUnreachable(LocationId source): JsInst(Unreachable, source) {}

    template<class F> void eachUse(JsirBase, F&&) {}
    template<class F> void eachEdge(JsirBase, F&&) {}
};

template<class F>
auto visitInst(JsInst& inst, F&& f) -> decltype(f((JsInstConst&)inst)) {
    switch(inst.kind) {
#define YANA_JSIR_INST(kind, Struct, mnemonic, result, effects) \
        case JsInst::kind: return f((Struct&)inst);
#include "inst.def"
#undef YANA_JSIR_INST
        case JsInst::KindCount: break;
    }

    assertTrue(false);
    return f((JsInstConst&)inst);
}

// Every operand, by value. The same gatekeeper rule `ssa::forEachUse` states: an operand held in a
// list cannot be handed out by reference, so a walk that offered references for some kinds would be
// an interface that lies about half its cases.
template<class F>
void forEachUse(JsirBase base, JsInst& inst, F&& f) {
    visitInst(inst, [&](auto& i) { i.eachUse(base, f); });
}

template<class F>
void forEachEdge(JsirBase base, JsInst& inst, F&& f) {
    visitInst(inst, [&](auto& i) { i.eachEdge(base, f); });
}

struct JsBlock {
    JsBlock(JsBlockId index, StringId name): name(name), index(index) {}

    auto arguments(JsirBase base) { return argumentList.contents(base); }
    Size argumentCount() { return argumentList.size(); }
    JsirPtr<JsValue> argumentAt(JsirBase base, Size index) { return argumentList.get(base, index); }

    auto instructions(JsirBase base) { return instructionList.contents(base); }
    Size instructionCount() { return instructionList.size(); }

    auto incoming(JsirBase base) { return incomingList.contents(base); }
    Size predecessorCount() { return incomingList.size(); }

    JsInst* terminator(JsirBase base);

    StringId name {};
    JsBlockId index;

    // Whether this block was reached at all. A JSIR body is built from an SSA body the optimizer
    // already pruned, so this is the conversion's own answer about an SSA block it declined to
    // convert rather than a deletion of its own.
    bool deleted = false;

    // Only the builder writes these three - see `build.h`. The lists are two halves of statements
    // the IR makes twice (an instruction names its block back, a successor records its edges), and
    // a writer that could push into one could state half of one.
    friend struct JsEditor;

    JsirList<JsirPtr<JsValue>, true> argumentList;
    JsirList<JsirPtr<JsInst>, false> instructionList;
    JsirList<JsirPtr<JsBlock>> incomingList;
};

/*
 * One function's JSIR - §7.1's "one reusable body arena for the current function".
 *
 * Everything here lives in `JsTarget::arena`, which is reset between functions. Nothing outside this
 * directory holds a handle into it, which is what makes the reset safe rather than a lifetime
 * question the AST builder has to participate in.
 */
struct JsBody {
    JsirPtr<JsBlock> entry = nullptr;

    RegionArray<JsirRegion, JsirPtr<JsBlock>> blocks;
    RegionArray<JsirRegion, JsirPtr<JsValue>> values;
    RegionArray<JsirRegion, JsirPtr<JsInst>> insts;
    RegionArray<JsirRegion, JsirPtr<JsPlace>> places;

    // The variables `JsPlaceKind::Var` names, and the representation each holds. Declared as one
    // `var` apiece at the top of the emitted function.
    RegionArray<JsirRegion, JsRepId> variables;

    DeclPtr<mir::Function> function = nullptr;
    JsAbiId abi;

    // The values the entry block's arguments are - one per emitted parameter, environment included.
    // Held so that the AST builder names a parameter rather than re-deriving which argument it was.
    RegionArray<JsirRegion, JsirPtr<JsValue>> parameters;

    JsBlock* block(JsirBase base, JsBlockId index) const { return base[blocks.get(base, index.index)]; }
    JsValue* value(JsirBase base, JsValueId id) const { return base[values.get(base, id.index)]; }
    JsInst* inst(JsirBase base, JsInstId id) const { return base[insts.get(base, id.index)]; }
    JsPlace* place(JsirBase base, JsPlaceId id) const { return base[places.get(base, id.index)]; }
};

/*
 * The parts of one multi-part value - a range of the conversion's own run of part values.
 *
 * A range rather than a list per value, for the reason `makeParts` gives: there are `lanes` of
 * these per vector instruction and a container apiece would be an allocation per lane.
 */
struct JsPartsRef {
    // The `Parts` representation, which is what a materialization and a split are named by.
    JsRepId rep;
    U32 first = 0;
    U16 count = 0;

    bool valid() const { return rep.valid(); }
};

/*
 * What one SSA value became on this target - §8.2's `lowerValue` answer, as a row of a dense table.
 *
 * `form` is what the **definer** said, and the three handles below are what anything has found
 * since: a value defined as `Parts` and then asked for as one host value keeps both, because the
 * materialization is memoized at the position that asked (§3.6), and a value that is one host value
 * and is then asked for a place keeps both for the same reason. The four maps this replaces carried
 * a comment saying "exactly one of the three holds an entry for a given value", and that was not
 * true of any of them - what decided the answer was the *order* `valueOf` happened to ask in, which
 * is a fact about one function rather than about the value.
 *
 * So the order is stated where it is read and the fields are named for what they hold. A row is
 * eight words and there is one per SSA value of the body being converted; four hash maps keyed on a
 * dense id is what §7.4 names as the wrong container for exactly this.
 */
struct JsTargetValue {
    enum class Form: U8 {
        // Nothing has converted this value. Reading one is an internal error, never a `null`.
        None,

        // Converted to nothing at all - a unit result, which occupies no position anywhere.
        Void,

        One,
        Parts,
        Place,
    };

    Form form = Form::None;

    JsirPtr<JsValue> one = nullptr;
    JsPlaceId place;
    JsPartsRef parts;

    /*
     * `jsManifest(occurrence).rep`, filled on first ask.
     *
     * A pure function of the occurrence, which does not change while a body is converted - so it is
     * a memo and not a second answer. Worth having because the walk behind it is not one indexed
     * load: a view occurrence goes through `viewRecordOf`, which *instantiates* a record, and 26
     * sites in the conversion ask this of the same values over and over - a result's, an operand's,
     * and both again inside every multi-part arm.
     */
    JsRepId manifest;

    /*
     * The storage behind a projection of a **niche-folded** discriminant.
     *
     * The projection itself answers a *value* - the decoded constructor index - because that is what
     * every reader of a tag wants and what a folded record makes cheap. A write into one is the one
     * reader that wants the storage instead: setting the absent constructor is `v = null` into the
     * place the value is in, and setting the present one is nothing at all. A field here rather than
     * a place kind because no instruction ever names it.
     */
    JsPlaceId nicheTag;
};

/*
 * The conversion's per-function side tables - §7.4's "dense scratch, reset per function".
 *
 * Owned by `JsTarget` rather than by the conversion, so the steady state is the largest function
 * seen rather than four hash tables and two arrays per body. `reset` keeps whatever storage the
 * widest body so far grew, which is `compiler/util/README.md`'s rule for a table rebuilt once per
 * function.
 */
struct JsConvertScratch {
    // By SSA value id.
    Array<JsTargetValue> values;

    // By SSA block index - the JSIR block each became.
    Array<JsirPtr<JsBlock>> blocks;

    /*
     * `reserve` before each fill, because `clear` keeps the buffer and `push` does not know that
     * the count is already known. Without it the first body of every compilation pays one
     * allocation per doubling on the way up to its width - which is log2(widest) per array per
     * program rather than the one the dense scratch was for.
     */
    void reset(Size valueCount, Size blockCount) {
        values.clear();
        values.reserve(U32(valueCount));
        for(Size i = 0; i < valueCount; i++) values.push(JsTargetValue {});

        blocks.clear();
        blocks.reserve(U32(blockCount));
        for(Size i = 0; i < blockCount; i++) blocks.push(nullptr);
    }

    /*
     * The row for one value, grown on demand.
     *
     * A body's value count is known before the conversion starts, so the growth here is for the
     * values the *conversion* invents - which are JSIR values and have ids of their own, so in
     * practice none. It is a bound rather than a case.
     */
    JsTargetValue& at(U32 id) {
        while(values.size() <= id) values.push(JsTargetValue {});
        return values[id];
    }
};

/*
 * What one operand position *does* to the value in it - `opt_alias.cpp`'s vocabulary, here because
 * the scratch below holds a run of them.
 *
 * A total switch over the opcodes rather than a default, which is this directory's rule read for a
 * pass: a row added to `inst.def` without a decision there fails to compile, where a
 * `default: read` would silently make the next opcode safe to alias through.
 */
enum class JsUseRole: U8 {
    // The value is read and nothing more - a property read through it, a duplicate of it, an
    // argument at a position the callee may only read.
    Read,

    // The same object under a second name: the answer for this use is the answer for the alias.
    Aliased,

    // Something may write through this position - a `&` argument, a consuming one, a place a store
    // names, a teardown.
    Written,

    // The reference outlives this body's view of it - a field of an object, storage, a `return`, an
    // edge. Not necessarily a write, but not something that pass can bound.
    Kept,

    // The host may observe the identity, or this tier cannot say what happens.
    Opaque,
};

struct JsUse {
    JsInst* inst = nullptr;
    JsUseRole role = JsUseRole::Opaque;
};

/*
 * One instruction becoming an `Alias` of a value already in scope, collected and applied in two
 * passes - see `opt_alias.cpp`, where the two-pass shape is argued.
 */
struct JsOptRewrite {
    JsInst* inst = nullptr;
    JsValue* from = nullptr;
};

/*
 * The JSIR optimizer's per-function side tables - §7.4, and the same discipline as
 * `JsConvertScratch` above.
 *
 * Every one of these used to be an `Array` declared inside the pass, and the pass is constructed
 * per body - so a program of two thousand functions paid every one of them two thousand times, and
 * the walk lists inside `computeDominance` and `sinkAllocations` once per body on top of that. They
 * are reset here instead, which is `compiler/util/README.md`'s rule for a table rebuilt once per
 * function.
 */
struct JsOptScratch {
    // The CSR use index - one entry per value into `useList`.
    Array<U32> useStart;
    Array<JsUse> useList;

    // How many live instructions name each place, and how many uses of each value the index sees.
    Array<U32> placeRefs;
    Array<U32> liveUses;

    // Which body parameters are at a `shared` position, by value id.
    IndexSet sharedParameter;

    // The CFG facts `sinkAllocations` needs, by block index.
    Array<U32> rpo;
    Array<U32> rank;
    Array<U32> idom;
    Array<U64> loopMask;

    // Successors, as a second CSR index.
    Array<U32> succStart;
    Array<U32> succList;

    // Where each instruction sits in its own block, so that applying a rewrite is an indexed load
    // rather than a scan of the block it is in - which was quadratic in a body the pass fired on
    // many times.
    Array<U32> instAt;

    Array<JsOptRewrite> pending;

    // The walk lists. Named rather than declared where they are used, for the reason above.
    Array<U32> fill;
    Array<U32> postorder;
    Array<U32> stack;
    Array<U32> next;
    Array<U32> work;
    IndexSet seen;
    Array<JsInst*> candidates;
};


/*
 * The CFG facts every tier below the conversion asks for, computed **once** per body.
 *
 * There were three of these and they were three different algorithms over one graph: `verify.cpp`
 * had a reverse postorder and a Cooper-Harvey-Kennedy immediate dominator, `opt_alias.cpp` had the
 * same pair plus natural-loop membership, and `codegen/js/structure.cpp` had a set-based dominance
 * fixpoint, a `closestOf` scan to get the immediate dominator back out of it, and its own order.
 * Two of them ran on every body and the third on every body the sinking pass had a candidate in.
 *
 * The graph is the same for all three, and that is a property rather than a coincidence: the
 * optimizer's rewrites all replace one instruction with an `Alias` of a value already in scope and
 * none of them touches an edge, so §2.3's "the CFG is unchanged across the AST boundary" holds from
 * the conversion to the structurer. `JsTarget::beginBody` is what says when it stops being the same
 * graph, and it is called where the body arena is reset.
 *
 * ## The order is load-bearing
 *
 * `rpo` is a **total order over every block**, not only the reachable ones: the reachable blocks
 * come first in reverse postorder and the rest follow in index order. The structurer reads it to
 * decide which of two merge regions under one block is the outer one (J67), and post-dominance -
 * which is what it used to ask - stops being a total order the moment a path leaves the function.
 * So the numbering is part of the contract and not an implementation detail; `reachedCount` is
 * where the two halves meet and `reachable` is the bit that says which half a block is in.
 */
struct JsCfg {
    static constexpr U32 kNoBlock = maxLimit<U32>;

    // position -> block, and block -> position. See the note above: `rpo` covers every block.
    Array<U32> order;
    Array<U32> rpo;

    // Immediate dominator per block, `kNoBlock` for the entry's own and for anything unreachable.
    Array<U32> idom;

    // How many *forward* edges arrive at each block - what says a block is where two arms meet.
    Array<U32> forwardPreds;

    IndexSet reachable;
    IndexSet loopHeader;

    // The natural loop of each header, by header - filled only for the rows `loopHeader` names.
    IndexSetList loops;

    /*
     * The same membership by block, as a bit per loop - what the sinking pass reads.
     *
     * Two spellings of one fact because the two questions are asked in opposite directions: the
     * structurer asks "is this block in *that* header's loop" and the sinking pass asks "are these
     * two blocks in the same loops". A mask is the second one in a word.
     */
    Array<U64> loopMask;
    U32 loopCount = 0;

    // Successors as a CSR index, which is what the order walk and the back-edge scan both read.
    Array<U32> succStart;
    Array<U32> succList;

    // How many blocks the entry reaches. Positions below this in `rpo` are the reachable ones.
    U32 reachedCount = 0;

    /*
     * Whether every cycle in this graph is a natural loop.
     *
     * False where a retreating edge's target does not dominate its source, which names a cycle with
     * more than one entry - `loopMask` cannot describe one and the sinking pass refuses rather than
     * approximating. Also false past sixty-four loops, which is the mask's width. The structurer
     * does not ask: it discovers an unstructurable graph by meeting a block it has written and
     * re-emits the whole function as a state machine.
     */
    bool reducible = false;

    // Whether this instance describes a body at all. `beginBody` clears it.
    bool ready = false;

    // The walk lists this is built with, kept here so that the storage is the widest body's rather
    // than one set of arrays per body - `compiler/util/README.md`'s rule.
    Array<U32> walkStack;
    Array<U32> walkCursor;
    Array<U32> walkPostorder;
    IndexSet walkSeen;

    Size blockCount() const { return rpo.size(); }

    bool dominates(U32 owner, U32 block) const {
        while(block != kNoBlock && block < idom.size()) {
            if(block == owner) return true;
            if(idom[block] == block) return owner == block;
            block = idom[block];
        }

        return false;
    }

    U32 successorCount(U32 block) const { return succStart[block + 1] - succStart[block]; }
    U32 successorAt(U32 block, U32 index) const { return succList[succStart[block] + index]; }
};


struct JsCoalesced {
    static constexpr U32 kNoClass = maxLimit<U32>;

    /*
     * By `JsValueId`: the value id of the representative block argument of this one's class, and
     * `kNoClass` for a value that is not a coalescable block argument at all.
     *
     * A value id rather than a dense class number because the emitter names the variable after it -
     * `p17` is the class whose representative is `%17` - and two bodies that coalesce the same way
     * then spell the same names.
     */
    Array<U32> root;

    U32 rootOf(U32 value) const { return value < root.size() ? root[value] : kNoClass; }
};

/*
 * The per-function scratch the analysis runs in - §7.4, on the same terms as `JsConvertScratch`.
 *
 * The liveness planes are `BitMatrix`es rather than the `Array<bool>` planes this had when it lived
 * in the emitter: a plane is one bit per (block, argument), five of them are rebuilt per body, and
 * the fixpoint over them runs a word at a time rather than a byte.
 */
struct JsCoalesceScratch {
    // Dense numbering of the block arguments - by value id, and back.
    Array<U32> dense;
    Array<U32> valueId;
    Array<U32> union_;

    BitMatrix up;        // what a block hands its predecessors - its own arguments excluded
    BitMatrix liveAt;    // what is live anywhere in a block - its own arguments included
    BitMatrix outOf;     // what is live at the end of a block
    BitMatrix used;      // what a block reads, computed once
    BitMatrix defined;   // a block's own arguments, which are written on the edge into it
    BitMatrix interferes;

    // One row belonging to no table, which is what a fixpoint's running state is - see `BitMatrix`.
    Array<U64> row;

    // The arguments merged into each class, as rows rather than a pairwise scan - see
    // `classesInterfere`, where the cubic form this replaced is named.
    ArrayList<U32, 4> classMembers;

    Array<U32> live;
    Array<U32> candidates;
};

/*
 * What the JSIR optimizer's rewrites did, summed over the program - the counter §9.9 asks each
 * optimization for.
 *
 * Printed by `-mode jsir` under `optimizer`, so that "how much did this fire" is a `grep` over a
 * dump rather than a build of the compiler with something switched on. Here rather than in `opt.h`
 * because `JsTarget` carries it and `repr.h` is below that file.
 */
struct JsOptCounts {
    // Structural duplicates the conversion emitted, and how many of them became an alias (§6.3).
    U32 clones = 0;
    U32 aliased = 0;

    // Property reads answered from the object literal that built them, and the instructions that
    // then had no user at all (§6.6).
    U32 forwarded = 0;
    U32 removed = 0;

    // Escaping locals whose storage was taken from the first value written into it rather than
    // built at its zero and then overwritten - `adoptInitialStores`.
    U32 adopted = 0;

    // Allocations moved to the point that dominates their uses rather than made where the frame
    // slot was declared - `sinkAllocations` (§6.5's first and third bullets).
    U32 sunk = 0;
};

StringView jsInstMnemonic(JsInst::Kind kind);
U16 jsInstEffects(JsInst::Kind kind);
U8 jsInstResult(JsInst::Kind kind);

inline bool isTerminator(JsInst::Kind kind) {
    return (jsInstEffects(kind) & kJsTerminator) != 0;
}

/*
 * Whether one **instruction** may be moved or dropped - the kind's answer, refined where the kind's
 * is not the whole answer.
 *
 * Only asked of an instruction and never of a bare kind: there is exactly one kind whose column is
 * not the whole answer, so a `isPure(JsInst::Kind)` beside this is an overload a caller can reach
 * for by accident and get the wrong answer from for `Host`. It existed here with no callers.
 *
 * That kind is `Host`, whose effects are a `resolve_mir/host.def` row rather than a column here.
 * The name is `ssa::isPure(const Inst&)`'s one tier up, so a pass asking "may I move this" asks it
 * the same way whatever it is looking at.
 */
U16 effectsOf(const JsInst& inst);

inline bool isPure(const JsInst& inst) { return (effectsOf(inst) & kJsPure) != 0; }

inline JsInst* JsBlock::terminator(JsirBase base) {
    auto count = instructionList.size();
    if(!count) return nullptr;

    auto last = base[instructionList.get(base, count - 1)];
    return isTerminator(last->kind) ? last : nullptr;
}

StringView jsRepKindName(JsRepKind kind);
StringView jsPlaceKindName(JsPlaceKind kind);
StringView jsPropertyRoleName(JsPropertyRole role);
StringView jsPassingKindName(JsPassingKind kind);

} // namespace jsir
