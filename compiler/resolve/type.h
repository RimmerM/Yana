#pragma once

#include "../compiler/context.h"
#include "../parse/ast.h"
#include "../util/container.h"

struct Program;
struct Module;
struct GlobalRegion {};

using GlobalBase = RegionBase<GlobalRegion>;

template<class T>
using GlobalPtr = RegionPtr<GlobalRegion, T>;

template<class T>
using GlobalList = SmallList<GlobalRegion, T, false>;

/*
 * The IR region, declared here rather than in inst.h - which is where it used to be and where
 * everything that uses it still lives - because one thing in this file names something in it: a
 * field default is a `ConstValue`, and a constant is allocated in the IR region for the reason
 * `ConstValue` gives. A region tag is four lines and no dependency, so the two live together.
 */
struct ModuleRegion {};

using ModuleBase = RegionBase<ModuleRegion>;

template<class T>
using ModulePtr = RegionPtr<ModuleRegion, T>;

template<class T, bool allowEmbed = true>
using ModuleList = SmallList<ModuleRegion, T, allowEmbed>;

struct ConstValue;
struct Type;
struct GenEnv;
struct GenType;
struct TypeClass;
using TypePtr = GlobalPtr<Type>;

/*
 * A list of type arguments: what a generic call inferred, what a class's variables were bound to,
 * what an instance head resolved its own variables to.
 *
 * Four inline, which is a guess about programs rather than about the language - there is no limit on
 * how many variables a declaration may have, and `Result(a, b)` is already at the wide end of what
 * anyone writes. Overload resolution builds one of these per candidate it tries and per position it
 * tries them at, so this is the list the resolver allocates more of than any other.
 */
using TypeList = SmallArray<TypePtr, 4>;

/*
 * How one half of a teardown is supplied.
 *
 * `None` is nothing at all, `Derived` is the glue the compiler writes - recurse into each member,
 * and for the reclaim half also release this owner's own indirect storage - and `Authored` is an
 * instance someone wrote.
 */
enum class TeardownKind: U8 {
    None,
    Derived,
    Authored,
};

/*
 * The structural ownership facts of one type (Implementation-IR.md part 4, Design-Memory §4.1).
 *
 * These are computed from the shape of the type rather than looked up per use, because every one
 * of them is needed long before typeclass dispatch is available: whether a `let x = e` copies or
 * borrows is decided while resolving the `let`, and it is decided by whether `e`'s type is
 * TrivialCopy.
 *
 * `trivialCopy` and `trivialSink` are the two implicit classes; `authoredCopy`, `reclaim` and
 * `drop` record what an `instance Copy(T)` / `Reclaim(T)` / `Drop(T)` supplied, since those *are*
 * spellable and are what InstCopy and InstDrop call.
 *
 * **There is no authored relocation.** A move is the bytes, always - `moveInit` in a type
 * descriptor is the target's own bulk relocation and never a user function. `trivialSink` therefore
 * asks *whether* a type may be relocated rather than *how*, which is why it needs no witness: a
 * type that answers no is one nothing can move, not one that moves by a call. See the note on
 * `Sink` in doc/spec/core.md for what was removed and why.
 *
 * Teardown is two fields rather than one because Design-Memory §4 splits it, and the split is what
 * makes three previously separate rules one sentence each: `Reclaim` compiles to nothing on the JS
 * target while `Drop` runs; `Reclaim` is elided for region-placed storage while `Drop` still runs
 * at last use; closing a region discharges every `Reclaim` inside it in bulk. **Region eligibility
 * is therefore `drop == None`**, computed over the member graph exactly as TrivialCopy is, which is
 * what lets `Map(String, Connection)` be arena-placeable with its connection teardowns intact.
 *
 * The relations between the fields are not independent:
 *
 *  - a type with either half of a teardown is not TrivialCopy, because copying it would duplicate
 *    the resource that teardown releases;
 *  - a type that is not TrivialSink is not TrivialCopy either, because a duplicate is strictly more
 *    than a relocation: bytes that cannot be moved cannot be duplicated by copying them;
 *  - every property is structural over members, so one non-trivial field is enough.
 */
struct Ownership {
    bool trivialCopy = true;
    bool trivialSink = true;
    bool authoredCopy = false;

    // Release this value's own storage. Elidable: something else may reclaim it in bulk.
    TeardownKind reclaim = TeardownKind::None;

    // Run an effect at last use. Never elided, on any target, ever.
    TeardownKind drop = TeardownKind::None;

    bool needsTeardown() const { return reclaim != TeardownKind::None || drop != TeardownKind::None; }

    // Whether a round of the fixpoint below moved anything, which is the whole of its termination
    // test - so it compares every field rather than the three a caller usually reads.
    bool operator == (const Ownership& o) const {
        return trivialCopy == o.trivialCopy && trivialSink == o.trivialSink
            && authoredCopy == o.authoredCopy && reclaim == o.reclaim && drop == o.drop;
    }

    bool operator != (const Ownership& o) const { return !(*this == o); }
};

/*
 * The state of one ownership classification - see ownershipOf, which is the only thing that touches
 * it.
 *
 * A recursive data type reaches itself through its own members, so the fold that classifies it asks
 * for an answer that is still being computed. Cutting the cycle with a conservative constant is the
 * obvious thing to do and it is wrong in the direction that costs code: every legitimately recursive
 * type - every one whose cycle runs through a box, which is every one that has a finite value - came
 * out with a derived `Drop` it does not need, and a `drop` is never elided on any target. A binary
 * tree therefore carried a second complete traversal of itself that executed nothing.
 *
 * So the cycle is *solved* rather than cut: assume the least a member could owe, fold, and repeat
 * until a round moves nothing. The assumption is the optimistic end of the lattice and every fold in
 * type_own.cpp is monotone downwards from it, so the iteration descends and terminates.
 *
 * On the Program rather than in a static, because the types of two programs resolved at once are two
 * different regions and this is state over one of them. Not on the `Type`, because a Type's size is
 * emitted output - see the note on `exported` - and there is no spare byte in one.
 */
struct OwnershipSolve {
    struct Answer {
        // The current assumption, or - once `round` names the round in progress - what that round
        // folded out of it.
        Ownership value;

        // Which classification this entry belongs to. Entries outlive the solve that made them so
        // that the map keeps its buckets; one whose generation is stale is not there.
        U32 generation = 0;

        // The round `value` was last recomputed in, which is what stops a shared member from being
        // folded once per path through it.
        U32 round = 0;
    };

    // Keyed by TypePtr::offset. Kept across classifications rather than cleared, because before
    // Program::declarationsComplete nothing may be remembered on the type itself and every query
    // therefore runs a fresh solve.
    HashMap<U32, Answer> answers;

    // The types this solve gave a provisional answer to, in the order it reached them. The walk
    // visits the same set every round - which member is folded is structural and does not depend on
    // any answer - so this is what there is to write back when the rounds are done.
    Array<TypePtr> reached;

    U32 generation = 0;
    U32 round = 0;
    bool running = false;

    // Whether any query this round read an assumption, and whether any answer moved. A round that
    // read no assumption saw no cycle and is already the answer; a round that moved nothing is the
    // fixpoint.
    bool usedAssumption = false;
    bool changed = false;
};

/*
 * Resolve types live in one region shared by every module of a program, so that a type resolved in
 * Core is the same TypePtr when a user module names it. Interning is what makes that identity
 * meaningful: sameType() is pointer equality, and instance selection, generic instantiation caching
 * and Repr all depend on it.
 *
 * **A Type carries no layout.** No size, no alignment, no field offsets - see compiler/repr/repr.h
 * for where those live and why they are a code generator's business rather than the resolver's. What
 * is here is the *logical* shape, and the resolver reasons entirely in terms of it: a place is a
 * root plus a path of field indices, never a byte offset, so ownership, borrows, drops, escape and
 * provenance are all decided without anyone knowing how wide anything is.
 *
 * The one layout-shaped question resolve does answer is isDirectType() - whether a value is carried
 * as a copy in a register or as an address - and it is deliberately computed from the kind alone. It
 * has to be target-independent, because it decides whether a call result gets a local and therefore
 * what the ownership passes see; a version of it that consulted a target's Repr would make the set of
 * accepted programs depend on which backend was running.
 *
 * It reads like an observation about layout and is not one. It is a *decision*, and the contract runs
 * one way: resolve states it and every target's calling convention is bound by it. That direction is
 * what makes arrivesAsCopy() below a sound rule rather than a coincidence, and ReprTable::of asserts
 * the half of it a target could break - see checkAbiContract in repr.cpp.
 */
struct Type {
    enum Kind: U8 {
        Error,
        Unit,
        Int,
        Float,
        Borrow,
        Ref,
        RegionPtr,
        Ptr,
        Region,
        Fun,
        Array,
        Map,
        Tup,
        Record,
        Gen,
        Literal,

        /*
         * `String` - Implementation-String.md part 1, and a primitive rather than a record.
         *
         * The surface language has one `String`; the two targets do not agree on what one *is*, and
         * that disagreement is the reason this is a kind of its own instead of the
         * `@platform`-split `data` declaration `Array(a)` gets. On JS a string is the host string
         * value, so it has to sit in a plain variable and be boxed only by the rule every non-object
         * already follows - a wrapper record there would make every string an object allocation and
         * would put it in a *record* place for the ownership passes, which is the shape
         * Implementation-Containers.md §14.1 records building for `HostArray(a)` and removing.
         * Natively it is the two words `stringContent` names, which is exactly `Array(U8)`'s layout.
         *
         * So the logical type is one thing and the Repr is asked per target, which is the split
         * Implementation-Repr.md already draws everywhere else. What is *not* per target is
         * ownership: `String` is non-TrivialCopy on both, per Implementation-String.md part 2, even
         * though a JS string is free to duplicate at the codegen level.
         */
        String,

        // `Vec(a, n)` and `Mask(a)` - Design-Vector §2, and one kind for both. See VectorType.
        Vector,

        /*
         * A number in a position a type is written - Implementation-Const-Generics.md §2.1.
         *
         * The `4` of `[Int *4]` and of `Vec(Float, 4)`. It is a Type so that a count and a *const
         * variable* are the same kind of child: `[a *n]` holds a GenType where `[a *4]` holds one
         * of these, and every substitution, matching and interning rule that already walks a type
         * child therefore applies to a count with no case of its own.
         *
         * A value of one is not a thing that exists. It never appears as the type of a local, an
         * argument or a field, so nothing asks it for a layout, an ownership class or a Repr - see
         * ConstType.
         */
        Const,
    };

    explicit Type(Kind kind): kind(kind), generic(false), exported(false) {}

    GlobalPtr<Byte> descriptor = nullptr;
    U16 descriptorLength = 0;
    Kind kind;

    // Set when a type variable is reachable inside this type. A generic type has no Repr and
    // never reaches the IR; it exists to be substituted.
    bool generic: 1;

    /*
     * Whether the declaration that named this type wrote `pub`.
     *
     * On the Type rather than beside the name, because the *value* of `Module::namedTypes` is what
     * a lookup has in its hand and the name is what it started from - see exportedSymbol(). A type
     * nothing names, which is every borrow, tuple, pointer and function type there is, has no
     * declaration to have marked and nothing ever asks this of one.
     *
     * A record's constructors are covered by it too: there is no way to export a type without them,
     * which doc/spec/modules.md records as an open question rather than a decision.
     *
     * A bit rather than a byte, sharing `generic`'s, because a Type's *size* is observable output: a
     * type descriptor's first slot is the type's own offset in the global region, so a `Type` one
     * byte wider moves every descriptor in every emitted program and rewrites six golden files that
     * have nothing to do with visibility.
     */
    bool exported: 1;

    // Cached by ownershipOf(). Ownership classification is a whole-program property - one type has
    // one answer - so it is cached on the type the way Repr is, rather than recomputed per module
    // that asks. That relies on instance coherence, which the language already requires, and on
    // every instance already existing: nothing is remembered here until Program::declarationsComplete,
    // because an `instance Reclaim(T)` read later is exactly a statement that this answer was wrong.
    Ownership ownership;
    bool ownershipReady = false;

    // Whether the fold that classifies this type is on the stack right now, which is exactly the
    // question "is a member of it reaching back at it". The answer a re-entrant query gets is the
    // current assumption rather than a constant - see OwnershipSolve. Here rather than in the solve's
    // own map because it is asked of every member of every type and a Type has the byte.
    bool resolvingOwnership = false;
};

/*
 * An integer type.
 *
 * `bits` is how wide the value is in memory and `width` is the primitive it occupies once loaded,
 * which is what Design.md's "integer types can have different sizes when stored in memory,
 * however when loaded they are converted to the closest primitive integer in size" means: `U8`
 * and `I16` are one byte and two bytes of storage but both arrive in a 32-bit register, and only
 * the 64-bit family needs a wider one.
 *
 * `name` is carried rather than derived from `(bits, isSigned)` because Core's `Int` and Native's
 * `I32` are two distinct types of identical shape - separate interned Types with separate class
 * instances - and a diagnostic has to say which one it meant.
 *
 * ## `@bits(n)`, and why it makes a type
 *
 * `alias Id = @bits(53) U64` interns an IntType with `bits = 53` and `canonical` naming `U64`.
 *
 * Design.md's "Bit-width refinements" says `@bits(n)` is Repr-only and that generic code sees a
 * plain `UInt`, which reads as though it should not make a type at all. It has to. Repr is a
 * function of the logical type, so if `Id` and `U64` were one type then `Maybe(Id)` and `Maybe(U64)`
 * would be one type as well, and could not have different layouts - which is exactly the thing the
 * refinement exists to buy. `Maybe(Id)` is one machine word because eleven of `Id`'s patterns are
 * unreachable, and `Maybe(U64)` is two because none of `U64`'s are.
 *
 * What the design document is actually asking for is that the refinement never reaches *dispatch*,
 * and `canonical` is how that is delivered rather than by collapsing the types. Instance selection,
 * matchType, literal defaulting and overload resolution all canonicalize first, so `Num(Id)`,
 * `Eq(Id)` and the literal in `let x: Id = 1` are answered by `U64`'s instances and nobody writes an
 * instance per width. Assigning an `Id` to a `U64` is free, and the other direction masks.
 *
 * So the split the type already had - `bits` is storage, `width` is what a load produces - is the
 * whole mechanism, and the only new thing is that `bits` may now be set independently of `width`.
 */
/*
 * Which target quantity answers an integer's width, or `None` where the type answers for itself.
 *
 * Analysis-Modules.md Move 2. Three primitives are not a number of bits the language picked - they
 * are a number the *machine* picks, and resolve's business is to state the bound rather than the
 * answer. `Size` and `USize` are the target's index word; `CodeUnit` is one unit of its string
 * encoding.
 *
 * The bound is what everything above `repr` reasons with, and it is two numbers rather than one
 * because the two directions of a conversion need opposite ends of it - see `integerRangeFits`.
 */
enum class TargetInt: U8 {
    None,
    Word,
    CodeUnit,
};

struct IntType: Type {
    enum Width: U8 {
        Bool,
        Int,
        Long,

        /*
         * The target's own word, which is the one width class resolve cannot name.
         *
         * A separate class rather than `Long` with an abstract `bits`, because the register an
         * operation on one uses is the thing that differs: `Size` is a 64-bit register natively and
         * a 32-bit one on JS, and `lowerType` has to answer that question with the target in hand.
         * `CodeUnit` needs no such class - eight bits and sixteen both arrive in a 32-bit register,
         * so only its *storage* width is abstract and its class is `Int`.
         */
        Word,
    };

    // The bound resolve states for each abstract width - see TargetInt. The word is at least 32 bits
    // because the smallest target this compiler admits is JS, whose index type is a signed `Int`; it
    // is at most 64 because there is no wider machine to compile for.
    static constexpr U16 kWordMinBits = 32;
    static constexpr U16 kWordMaxBits = 64;
    static constexpr U16 kCodeUnitMinBits = 8;
    static constexpr U16 kCodeUnitMaxBits = 16;

    /*
     * The width class a given number of bits falls into - the smallest one that holds it.
     *
     * One rule in one place, because two callers construct integer types: the primitives in
     * `addInteger` and the `@bits` refinements in `resolveBitsType`. They disagreed once, and the
     * symptom was not a diagnostic - a 53-bit primitive built with a hardcoded `bits == 64` test
     * came out in the `Int` class and was silently emitted as 32-bit arithmetic on both targets.
     */
    static Width widthFor(U16 bits) {
        return bits <= 1 ? Bool : bits <= 32 ? Int : Long;
    }

    /*
     * How many bits the register a value of this width class is held in actually has.
     *
     * Not the same question as the type's own width, and the difference is where narrowing has to
     * happen: `U8` fills 8 of a 32-bit register and `WideInt` 53 of a 64-bit one, so arithmetic on
     * either can produce a register value the type cannot represent. `U32` and `I64` fill theirs,
     * which is why nothing has to be emitted for them.
     */
    static U16 registerBits(Width width) {
        assertTrue("a Word's register is the target's - ask registerBitsOn" && width != Word);
        return width == Bool ? 1 : width == Int ? 32 : 64;
    }

    IntType(U16 bits, Width width, bool isSigned, StringId name = StringId(), TypePtr canonical = nullptr,
            TargetInt target = TargetInt::None):
        Type(Type::Int), name(name), canonical(canonical), bits(bits), width(width),
        isSigned(isSigned), target(target) {}

    /*
     * The bound, from either end.
     *
     * Every reader of `bits` that is not asking the target has to pick one of these, and which one
     * it picks is the whole of the correctness argument: a question about what a value of this type
     * may *hold* takes `maxBits`, and a question about what this type is guaranteed to *fit* takes
     * `minBits`. A concrete type answers both with its own width, which is why the rest of the
     * compiler reads exactly as it did.
     */
    U16 minBits() const {
        return target == TargetInt::Word ? kWordMinBits
             : target == TargetInt::CodeUnit ? kCodeUnitMinBits : bits;
    }

    U16 maxBits() const { return bits; }

    bool isTargetWidth() const { return target != TargetInt::None; }

    // The answer, once there is a target to ask. The two below are the only way a width reaches a
    // number for an abstract type, and everything downstream of resolve goes through them.
    U16 bitsOn(IntWidths widths) const {
        return target == TargetInt::Word ? widths.word
             : target == TargetInt::CodeUnit ? widths.codeUnit : bits;
    }

    U16 registerBitsOn(IntWidths widths) const {
        return width == Word ? widths.word : registerBits(width);
    }

    StringId name;

    // The unrefined type this is a `@bits` refinement of, or null when this *is* the unrefined one.
    // Followed by everything that dispatches, and by nothing that lays out.
    TypePtr canonical;

    /*
     * The type's storage width, which for an abstract type is the *largest* it may be on any target.
     *
     * Holding the maximum rather than a sentinel is deliberate. A reader that has not been taught
     * about `target` then treats a `Size` as the 64-bit integer it actually is on the only native
     * target there is, so an unaudited site is wrong on JS - where the fixture corpus runs every
     * day - rather than silently wrong everywhere. See minBits/maxBits for the readers that were
     * taught.
     */
    U16 bits;
    Width width;
    bool isSigned;

    // Which target quantity `bits` is a bound on, or None where it is the answer.
    TargetInt target;
};

/*
 * Whether every value of one integer type is a value of another - which is the whole of what makes
 * a conversion between them lossless, and therefore the one question `Widen` is generated from.
 *
 * Asked as a range containment and not as "is the target wider", which is what it used to be. Two
 * primitives of the same width and signedness are two names for one set of values - `Long` and
 * `I64`, `Int` and `I32` - and a strict `>` on the widths said the conversion between them lost
 * information. It does not: `let n: I64 = someLong` was rejected, and `someLong :: I64` was accepted
 * only because `::` used to narrow, which is exactly the accident §0.1.1 removed.
 *
 * Unsigned needs its whole width inside the target's positive half; signed needs a signed target at
 * least as wide, since a signed type of any width holds -1 and no unsigned target does.
 *
 * The same predicate answers it for a `@bits` refinement against a type of another canonical family
 * - see convertRefinement, which is the only other caller and the reason this is not in core.cpp.
 */
inline bool integerRangeFits(const IntType& from, const IntType& to) {
    // The two ends of the bound, and they are not the same end - see IntType::minBits. What has to
    // fit is every value the source *may* hold, and what it has to fit into is the capacity the
    // destination is *guaranteed* to have, so an abstract type on either side answers conservatively
    // and the pair is decided once for every target rather than for the one being built.
    auto fromBits = from.maxBits();
    auto toBits = to.minBits();

    if(from.isSigned) return to.isSigned && toBits >= fromBits;
    return U32(toBits) - (to.isSigned ? 1u : 0u) >= fromBits;
}

/*
 * The range a float-to-integer conversion saturates into - Design-Vector §3.4's ruling, which is a
 * scalar one and lives here because all three backends need the same two numbers.
 *
 * **A value outside the target's range clamps to the nearest end of it, and NaN becomes zero.** The
 * alternative was three answers from one source: `cvttsd2si` produces the integer indefinite value
 * on x86, ARM saturates already, and JS wraps modulo 2^32 through `|0`. Saturation is where WASM,
 * Rust and Swift converged, and it costs a compare and a select.
 *
 * The bounds are the type's own, as `F64`. Every one of them is exact except a 64-bit maximum -
 * `2^63 - 1` and `2^64 - 1` are not doubles - so a backend that clamps in the *float* domain must
 * use a strict comparison against the returned high bound and produce the integer maximum itself,
 * rather than converting the clamped float. `highIsExact` says which case it is in.
 */
struct SaturationRange {
    F64 low;
    F64 high;
    bool highIsExact;
};

inline SaturationRange saturationRange(const IntType& integer, IntWidths widths = {}) {
    // The target's answer and not the bound: a clamp has to produce the number the machine's own
    // range ends at, and there is no conservative version of that. Every caller is downstream of
    // resolve and has a target; the default is the native one, for the callers that build a
    // concrete IntType on the spot to ask about a width they already have.
    auto held = integer.bitsOn(widths);
    auto bits = held >= 64 ? 64u : U32(held);

    if(!integer.isSigned) {
        auto high = bits >= 64 ? 18446744073709551616.0 : F64((U64(1) << bits) - 1);
        return SaturationRange { 0.0, high, bits <= 53 };
    }

    auto magnitude = F64(U64(1) << (bits - 1));
    return SaturationRange { -magnitude, bits >= 64 ? magnitude : magnitude - 1.0, bits <= 54 };
}

// A value in the normal form of its type: the type's own bits, sign-extended if it is signed. The
// same form `convertRefinement` puts a runtime value in, and `truncateToWidth` an arithmetic result.
inline U64 reduceToWidth(const IntType& integer, U64 value, IntWidths widths = {}) {
    // The target's answer, for the reason `saturationRange` takes one: a normal form is the bits a
    // value actually occupies, and a bound has no normal form.
    auto bits = integer.bitsOn(widths);
    if(bits >= 64) return value;

    auto mask = (U64(1) << bits) - 1;
    value &= mask;

    // A signed type's high bit is its sign, so a value that has it set is the negative number it
    // stands for and not the small positive one the mask left behind.
    if(integer.isSigned && (value & (U64(1) << (bits - 1)))) value |= ~mask;

    return value;
}

/*
 * Whether a written number is a value of this integer type.
 *
 * The magnitude and the sign are separate because that is how a number is *written*: the lexer
 * produces only the magnitude and a `-` in front of it is a different token. Deciding
 * representability from the magnitude is what makes the two ranges say what they mean - `I8` holds
 * -128 and not 128, so a negative literal reaches exactly one further than a positive one, and an
 * unsigned type does not accept a negative at all. It is also the only way to ask the question about
 * a 64-bit type: folding the sign in first leaves `18446744073709551615` and `-1` the same bits, and
 * one of them is an `I64` and the other is not.
 *
 * Shared by the two positions that ask it, which disagree about what to do with the answer rather
 * than about the answer: `checkLiteralRange` warns and truncates, because a full-width mask written
 * at a signed type is an idiom, and a declaration's constant is an error, because there is no
 * conversion a declaration could run.
 */
inline bool integerHolds(const IntType& integer, U64 magnitude, bool negative) {
    // The guaranteed capacity, because a written literal has to be a value of the type on *every*
    // target - `let n: Size = 3000000000` is not a program, however wide the machine being built for
    // happens to be.
    auto bits = integer.minBits();

    if(bits == 0) return magnitude == 0;
    if(negative && !integer.isSigned) return magnitude == 0;

    auto width = U32(bits) - (integer.isSigned ? 1 : 0);
    if(width >= 64) return true;

    auto limit = (U64(1) << width) - 1;
    return magnitude <= (negative ? limit + 1 : limit);
}

/*
 * `String`, and the type whose bytes it occupies on this target.
 *
 * There is exactly one of these in a program - `String` takes no arguments and is interned once by
 * `definePreludeTypes` - so the content is a field on the type rather than a lookup through the Program.
 * That is not just convenience: `ReprTable` holds a `GlobalBase` and a target and deliberately not a
 * `Program`, because a layout must be a function of the type graph alone. A type that needs one more
 * fact to be laid out has to carry it.
 *
 * `content` is `Native.StringData` - `{run: Run(U8), length: Count}` - on a native target, and null
 * on JS, where a string is one host value and there is nothing to lay out. It is filled in after
 * `Native` has been resolved rather than at construction, because the record it names is declared
 * there and the primitive is made before any file is read. Nothing reads it until lowering, which is
 * long after.
 */
struct StringType: Type {
    StringType(): Type(Type::String) {}

    TypePtr content = nullptr;
};

/*
 * The type a dispatch should see - a `@bits` refinement's unrefined form, and everything else
 * unchanged.
 *
 * Every caller is somewhere a *decision* is made about which code runs: which instance serves a
 * constraint, whether two types unify, what a literal defaults to. Layout deliberately does not call
 * it, because the refinement is precisely what layout is for.
 */
TypePtr canonicalType(GlobalBase base, TypePtr type);

// The `@bits(n)` refinement of an integer type, interned per (base type, width). Reports and
// returns the base type when `n` is out of range or the type is not an integer.
TypePtr resolveBitsType(Module& module, TypePtr base, U32 bits, LocationId source);

/*
 * A raw pointer - Design.md's `%T`, aliased `Ptr(a)`.
 *
 * Interned on its target type, so that `%Int` written in two places is one TypePtr and pointer
 * equality keeps answering sameType(). A pointer is a direct type: it lives in a register like an
 * Int does, and it is the target address of a load rather than something loaded.
 *
 * The pointee is what pointer arithmetic scales by and what a deref place projects to, so it is
 * kept even though nothing about the machine representation depends on it.
 */
struct PtrType: Type {
    explicit PtrType(TypePtr to):
        Type(Type::Ptr), to(to) {}

    TypePtr to;
};

/*
 * A borrow - rung 2 of Design.md's reference-kind ladder, written `&T`.
 *
 * Interned on its target and its mutability, so that the `&Int` written in a signature and the one
 * an InstBorrow produces are one TypePtr and sameType() stays pointer equality.
 *
 * This exists as a *type* only where a borrow has to survive being handed to someone: a function
 * result, and the binding that receives one. A parameter still says `&` on itself rather than
 * having a `&T` type, because a convention is a property of the parameter and not of what it
 * refers to - `fn f(&x: Int)` takes a mutable borrow of an Int, not a value of type `&Int`.
 *
 * What a borrow is made of is an address, and that is representation rather than structure: a
 * borrow has no members, cannot be matched on, and `.` on one always means a field of its target.
 */
struct BorrowType: Type {
    BorrowType(TypePtr to, bool mut):
        Type(Type::Borrow), to(to), mut(mut) {}

    TypePtr to;

    // Exclusive while live. Immutable borrows of one place coexist with any number of others.
    bool mut;
};

/*
 * `Vec(a, n)` and `Mask(a)` - Design-Vector §2.
 *
 * ## One kind, and a flag
 *
 * A mask is the same kind with `isMask` set rather than a second kind, so every switch over
 * `Type::Kind` gains one arm instead of two and the places that genuinely differ - the result type
 * of a lane comparison, the JS zero value, the AVX-512 register class - test the flag. §2.4 makes a
 * mask's identity its lane *width* and lane count and not the element it came from, since a mask
 * produced by comparing `Vec(Float)` is meaningful applied to `Vec(I32)`; so `content` is normalized
 * to the unsigned integer of the lane's width when the flag is set, and `Mask(Float)` and
 * `Mask(I32)` intern to one pointer.
 *
 * ## The lane count
 *
 * `count` is a ConstType for a resolved count and a GenType for the `n` of `Vec(a, n)`, and it is
 * null for the *natural* form `Vec(a)` written over a type variable, which cannot be resolved until
 * the element is: `targetVectorBytes / stride` needs a stride, and a variable has none.
 * `substituteType` spends it exactly as the fixed array's element is substituted, so nothing below
 * resolution ever sees a null one - which is Design-Vector §2.1's "after type resolution there is
 * one type constructor with a concrete lane count", and is the single simplification the rest of the
 * design rides on.
 *
 * A const-generic count is the other thing that survives resolution, and it is not an exception to
 * that: `Vec(Float, n)` is a *generic* type, so it reaches the IR only through a body that was
 * either specialized (and then the count is concrete) or compiled erased (and then the count is a
 * slot read - Implementation-Const-Generics.md §3.2). The null and the variable are therefore two
 * different absences: "not computed yet" and "the caller knows".
 *
 * ## What it is not
 *
 * Not a container. `contiguousElement` answers null for one, so a `Vec(a)` never silently converts
 * to `[a]`; there are no fields and no per-lane projection, so no place walk is ever asked about a
 * lane. Reading one is an instruction (`VecLane`), which is a boundary stage 3 holds.
 */
struct VectorType: Type {
    VectorType(TypePtr content, TypePtr count, bool isMask):
        Type(Type::Vector), content(content), count(count), isMask(isMask) {}

    // The lane type, normalized to the unsigned integer of the lane's width for a mask.
    TypePtr content;

    // A ConstType, a const variable, or null for "the target's natural count, not computed yet" -
    // see above.
    TypePtr count;

    bool isMask;
};

/*
 * `[T *n]` - the fixed array (Implementation-Containers.md §6).
 *
 * Exactly `n` elements at a stride and no count anywhere, on any target. Interned on the pair, so
 * that the `[Int *4]` two signatures write is one TypePtr and sameType() stays pointer equality.
 *
 * It is a *type* and not a Repr refinement of `[T]`, which is the decision §6 turns on: the two
 * differ in capability rather than in layout - a fixed array cannot grow - and a Repr variant may
 * never change what a type can do (§9). So `Array(T)`'s instances do not serve it and nothing here
 * is reached by conversion from one.
 *
 * There are no `fields` and no per-element projection, deliberately. `n` elements at a stride is
 * exactly what a `Run(a)`'s slots are, and both are reached the same way - the base address plus a
 * scaled index - which is what keeps a thousand-element literal a number rather than a type with a
 * thousand fields. The consequence is that the *inline run's address is computed from the owner and
 * never stored*: storing it would make the type self-referential and break `TrivialSink`, which is
 * Implementation-Storage.md §3's trap and applies here verbatim.
 */
struct ArrayType: Type {
    ArrayType(TypePtr content, TypePtr count):
        Type(Type::Array), content(content), count(count) {}

    TypePtr content;

    // A ConstType, or the const variable of `[a *n]`. Never null: unlike a vector's, a fixed array's
    // count has no natural form to be waiting for.
    TypePtr count;
};

/*
 * A number written where a type is - Implementation-Const-Generics.md §2.1.
 *
 * Interned on the pair, because `4` as an `Int` and `4` as a `Size` are one number and two
 * parameters: a const parameter's type is what an argument is checked against and what an expression
 * reading it has, so two counts of different types that happened to agree numerically must not
 * collapse into one. `canonicalType` is applied to the annotation before interning, so that a
 * `@bits` refinement of the count's own type does not split it either way.
 *
 * `generic` is false. A const *variable* is an ordinary GenType, which is what makes every rule
 * about generic types apply to one without a word being added to any of them.
 */
struct ConstType: Type {
    ConstType(U64 value, TypePtr type):
        Type(Type::Const), value(value), type(type) {}

    U64 value;

    // What the value is of - the annotation's type, canonicalized.
    TypePtr type;
};

/*
 * One argument of a function *type*.
 *
 * Implementation-IR.md part 3 is explicit that the convention and the `return` marker belong here
 * rather than on a declaration: a caller that reaches a function through a generic parameter, a
 * function value or dynamic dispatch has only the type to read, and a contract that survived a
 * direct call and evaporated at an abstraction boundary would be worse than no contract. So the
 * same two bits `Arg` carries are part of what makes two function types the same one.
 *
 * `name` is deliberately *not* part of identity. `(a: Int) -> Int` and `(Int) -> Int` are one type;
 * the name exists for diagnostics and for printing a signature back the way it was written.
 */
struct FunArg {
    TypePtr type = nullptr;
    StringId name {};
    ast::BindType convention = ast::BindType::Borrow;
    bool returnRoot = false;

    // The `@lazy` marker. `type` stays the argument's declared type for the same reason a `&`
    // parameter's does - what travels is a nullary thunk over the caller's frame, and that is a
    // fact about how this position is passed rather than about what it is. Part of the type's
    // identity, so that strictness survives every abstraction boundary a call can cross.
    bool lazy = false;
};

/*
 * A function type - Design.md's "Function types", and what a function value has.
 *
 * Interned on everything that decides whether two of them accept the same calls: the argument types
 * in order, each argument's convention and `return` marker, the result, and the `lens`/`iter` kind.
 * Nothing else may join that key, which is why `name` above is left out of it.
 *
 * The representation is two words: a code pointer, plus the environment its captures live in
 * (Design-Memory §8). *Releasing* that environment is a per-closure question rather than a per-type
 * one - two values of one function type can capture completely different things - but the answer is
 * reached through the code pointer rather than copied into the value, because which lambda a closure
 * came from is what decides both. See ClosureHeaderLayout.
 *
 * A non-capturing lambda and a plain function referenced by name have a null environment, so the
 * teardown is a branch that never fires rather than a second representation.
 */
struct FunType: Type {
    FunType(): Type(Type::Fun) {}

    GlobalList<FunArg> args;
    TypePtr result = nullptr;
    ast::FunKind kind = ast::FunKind::Plain;

    /*
     * The convention what a `lens`/`iter` hands over is received under - `iter (a) -> ->b`, which is
     * Analysis-Language.md §3a's spelling.
     *
     * Part of the interning key, because it is part of what calls a type accepts: an iterator that
     * hands its values *out* and one that lends them are two different contracts, and a `for` body
     * written against one of them will not do for the other.
     *
     * Where a declaration is desugared into its continuation this ends up on that continuation's own
     * `FunArg::convention`, which is where every pass that has an opinion about a binding already
     * looks - see resolveLensSignature. It is kept here as well so that the written spelling
     * survives being read back and composed into another type.
     */
    ast::BindType resultBind = ast::BindType::Borrow;

    // The argument indices whose `returnRoot` bit is set, as a mask - the single return-root group
    // Implementation-IR.md part 3 gives one function type. Kept alongside the args so that a caller
    // composing provenance through a call reads one word rather than walking the list.
    U64 returnRoots = 0;
};

struct FloatType: Type {
    enum Width: U8 {
        Float,
        Double,
    };

    explicit FloatType(Width width): Type(Type::Float), width(width) {}

    Width width;
};

/*
 * A literal that has not been given a type yet.
 *
 * A literal is a class-polymorphic value (`1` is `FromInt.fromInt(1)`), so the type it ends up
 * with is decided by where it flows rather than by how it is written. Resolving one with no
 * expected type produces a fresh literal variable - printed `?n` - tagged with the classes it
 * has to satisfy, and every position it reaches either binds it to a concrete type or leaves it
 * open for the next one. Whatever is still open when the statement ends takes its class's
 * `default`.
 *
 * `classes` is a list rather than a single class because two literal variables can meet: in
 * `1 + 2.5` the integer literal's FromInt and the decimal literal's FromDecimal are both
 * requirements on one type, and Float is the type that answers both.
 *
 * A literal variable exists only inside one function body's resolution. It never reaches the IR,
 * has no Repr, and is deliberately not interned: two literals written in one expression are two
 * variables even when they end up at the same type.
 */
struct LiteralType: Type {
    explicit LiteralType(U32 index): Type(Type::Literal), index(index) {}

    GlobalList<GlobalPtr<TypeClass>> classes;
    U32 index;
};

/*
 * One field of a tuple: what it is, what it is called, and whether it is reached through an
 * indirection. Where it *sits* is a Repr answer and lives in the code generator's table - see
 * FieldRepr in compiler/repr/repr.h.
 *
 * ## `boxed`
 *
 * The field's storage is an owning non-null pointer to a `type`, rather than a `type`. Two features
 * produce one - Design.md's "Representation and layout" keeps them apart on purpose:
 *
 *  - **automatic indirection**, written by `breakLayoutCycles` at the back edge of a layout cycle,
 *    because a type that reaches itself by inline containment has no finite size and *every*
 *    occurrence of it must therefore be a pointer. Nobody chose it, so nothing in the source names
 *    it and it is not part of the type;
 *  - **`@box`**, which the programmer writes on a field whose storage should be out of line for
 *    reasons having nothing to do with cycles.
 *
 * Both are per *declaration* and uniform across every value of the enclosing type, which is what
 * makes it legal for them to change the ownership classification (a boxed field costs `TrivialCopy`)
 * while an inferred Repr variant may not.
 *
 * **The field's type is unchanged.** `cfg.cold` on a `@box Diagnostics` is a `Diagnostics`, and
 * `f(cfg.cold)` against `fn f(d: Diagnostics)` is an ordinary borrow of the box target. What the
 * flag changes is the *place*: a Field projection onto a boxed field produces a `%type`, and the
 * resolver appends a Deref - see `boxedField` and `projectField` in expr.h.
 *
 * It is part of the tuple's interned identity for the same reason `TypeLayout` is: content tuples
 * are interned structurally and the Repr cache is keyed on the type, so `{Tree}` boxed and `{Tree}`
 * unboxed have to be two types or one of them gets the other's layout.
 *
 * ## `host`
 *
 * The field is not stored: it is the host property of the same name on the value field zero holds -
 * `@host`, and Implementation-Containers.md §14's elision. So a tuple that has one of these has
 * exactly one field of its own, which makes it the wrapped value the way a one-field tuple is, and
 * every field after that one is reached as a property of it.
 *
 * `@platform(js) data Array(a) {items: %a, length: @host Count}` is the declaration it exists for.
 * A host array's `length` *is* its occupancy and assigning it truncates - which is exactly what
 * `remove` means - so a container over one needs no count of its own, and the object that carried
 * one was a wrapper both rows paid for and only the typed row needed.
 *
 * **It is a claim a target may refuse**, and that is the difference from `boxed`. What a host value's
 * property means is not something this stage can check: a `TypedArray`'s `length` is its fixed
 * capacity rather than its occupancy and cannot be assigned at all, so the same declaration must
 * keep its stored field there. The flag says the elision is *available*; `hostPropertiesElided` in
 * resolve/host.h is the one rule that says where it holds, and the JS code generator is its reader.
 * Every other target ignores the flag and stores the field.
 *
 * Interned like `boxed`, and for the stronger version of its reason: two tuples that differ in this
 * differ in how many properties a value of them has.
 */
struct Field {
    TypePtr type = nullptr;
    StringId name {};
    bool boxed = false;
    bool host = false;
};

/*
 * How much freedom a target has over where the fields of an aggregate sit - Design.md's `@layout`,
 * and Design-Memory §11's "a declared pin is uniform across every instance of the type".
 *
 * `Auto` is the default and says nothing: a target may reorder the fields, co-pack the narrow ones,
 * and represent the whole aggregate as a single scalar. `C` pins the layout to what a C compiler
 * would have produced for the same declaration - declaration order kept, offsets and alignment
 * computed C's way, and bit-fields allocated in units of their *declared* type rather than of their
 * `@bits` width. A type that crosses an FFI boundary needs the second one, and everything else is
 * better off with the first.
 *
 * `Js` is the third, and it exists for the same reason `C` does: something outside the program reads
 * this type's representation, so the compiler may not choose it. What it pins is what a JS consumer
 * can see - the record is an object, its properties are the declared field names, and they are
 * assigned in declaration order - which rules out the four things the JS Repr family would otherwise
 * do to it: co-pack narrow fields into one property, represent the whole record as a `number`, fold
 * a sum's discriminant into `null`, and minify the names. It is the pin that makes the other three
 * safe to enable by default, since a record that crosses the host boundary now has a way to say so.
 *
 * All three are the *declaration's* statement rather than a target's, so `Js` pins on every target
 * and not only on JS - what it means there is declaration order with no packing and no scalar form,
 * which is a coherent layout everywhere. A type is not split in two by being shared; it is just not
 * optimized, which is what its author asked for.
 *
 * This is a property of the *tuple* rather than only of the declaration it came from, and it has to
 * be, because content tuples are interned structurally: `data A {x: Bool, y: Bool}` and
 * `data B {x: Bool, y: Bool}` are otherwise one `TupType`, and one of them being `@layout(c)` would
 * make the layout of the other depend on which was declared. Carrying it here makes the two
 * distinct interned types, so the Repr cache - which is keyed on the type alone - stays sound.
 */
enum class TypeLayout: U8 {
    Auto,
    C,
    Js,
};

struct TupType: Type {
    TupType(): Type(Type::Tup) {}

    GlobalList<Field> fields;
    TypeLayout layout = TypeLayout::Auto;

    /*
     * A container refinement - Implementation-Containers.md §7 - carried here as well as on the
     * record, and it *has to be* here for the same reason `@layout(c)` does.
     *
     * A field's offset is read off the *content tuple*, because that is what a Downcast reaches, and
     * content tuples are interned structurally with the Repr cache keyed on the type. So a
     * refinement recorded only on `RecordType` would leave two records with identical fields sharing
     * one layout - which is precisely the bug it is here to prevent: the plain array's `run` is
     * twelve bytes, the refined one's is `n * stride`, and the count that follows lands on top of an
     * element if the tuple cannot tell the two apart.
     */
    U32 inlineSlots = 0;
    U32 capacityBound = 0;

    bool named = false;
};

/*
 * What one field of a constructor is when a construction leaves it out -
 * `data Flags {read: Bool = False, ...}`.
 *
 * A default is kept as the *constant* the field starts at rather than as the expression it was
 * written as, for the same reason a global's initializer is (see declareGlobal): there is no program
 * point at which a declaration's code would run, and an expression would additionally belong to the
 * parse arena of the module that wrote it, which is not the one constructing the value. What may be
 * written is therefore whatever `evaluateConstant` accepts, and nothing here has to know which of
 * those forms it was.
 *
 * `field` indexes the constructor's content tuple, so only a named field can carry one.
 */
struct FieldDefault {
    U16 field = 0;
    ModulePtr<ConstValue> value = nullptr;
};

struct Constructor {
    StringId name {};
    TypePtr content = nullptr;
    U32 index = 0;

    /*
     * The number this constructor is, as opposed to which one it is - Analysis-Language.md §5.1.
     *
     * A payload-free sum already lowered to an integer carrying the declaration order, and what it
     * could not do was *say* so. The cost of that showed up the moment the number had to cross to a
     * syscall: `asInt` over a three-constructor enum was two compares and two selects computing the
     * identity function, because the mapping lived in a `match` nothing had a reason to recognise.
     *
     * `@value(n)` pins it, and an unpinned constructor continues from the one before it - so a
     * declaration that pins nothing is exactly the declaration order this always had, and one that
     * pins its first constructor renumbers the rest from there. That is C's rule, and it is what
     * makes partial pinning mean something predictable rather than something to check for
     * collisions against.
     *
     * Held on the *declaration*; an instantiation reads it through base(), like `source`.
     */
    I64 value = 0;

    // Whether `@value` was written on this constructor, as opposed to the number being carried over
    // from the one before. Only for diagnostics - what it changes is which of two constructors a
    // collision is reported at.
    bool pinnedValue = false;

    // Where the constructor was written, for the editor to jump to - resolve/index.h. Null for
    // every constructor Core and Native generate rather than parse, which is what makes it a
    // declaration nothing can navigate to rather than a missing one.
    LocationId source = kNullLocation;

    /*
     * The payload is reached through an owning non-null pointer - the same statement `Field::boxed`
     * makes, for the one edge that is not a field.
     *
     * A constructor written `Just(a)` carries its payload directly rather than as a one-field tuple,
     * so when a layout cycle's back edge lands on such a payload there is no `Field` to mark. That
     * is not a corner case: it is exactly what `Maybe(Tree)` is, and cutting there rather than at
     * `Branch.left` is what makes a child one word instead of a pointer to a heap-allocated `Maybe`.
     *
     * Set only by `breakLayoutCycles`, and only on a *record instance* or a non-generic declaration -
     * never on a generic declaration, whose layout is not a thing that exists. `Maybe(Tree)` has it
     * and `Maybe(Int)` does not, which is sound because the two are different types.
     */
    bool boxed = false;

    // Only the fields that were given one, in field order; most constructors have none. Read from
    // the declaration rather than from an instantiation of it, since an instantiation can be
    // created before the declaration's defaults have been read - see resolveConstruct.
    GlobalList<FieldDefault> defaults;
};

/*
 * Generic contexts.
 *
 * A generic type variable belongs to exactly one context - the declaration that introduced it -
 * rather than being ambient, which is what lets `Serialize(type, target)`-shaped constraints
 * relate two variables of the same context (Design.md's "Resolving"). `data`, `alias`, `class`
 * and `instance` declarations each get one; a function gets an *open* one, because a function
 * declares its variables by using them rather than in a list of its own.
 */

/*
 * Which sort of thing one variable of a context stands for - Implementation-Const-Generics.md §2.2.
 *
 * `Type` is the ordinary `a`; `Const` is the `n` of `fn (n: Int) f(v: Vec(Float, n))`, which stands
 * for a *number*. A variable is one or the other and never both, and using one at both kinds is
 * reported at the second occurrence - see genVariableKind.
 */
enum class GenKind: U8 {
    Type,
    Const,
};

struct GenType: Type {
    GenType(GlobalPtr<GenEnv> env, StringId name, U16 index):
        Type(Type::Gen), env(env), name(name), index(index) { generic = true; }

    GlobalPtr<GenEnv> env;
    StringId name;
    U16 index;

    // Where the variable was first written, which is its binder: a function declares its variables
    // by using them, so the first occurrence is the declaration. See genVariable.
    LocationId source = kNullLocation;

    GenKind kind = GenKind::Type;

    // A const variable's declared type - the `Int` of `n: Int`. Null for a type variable, and null
    // for a const variable whose kind was inferred from a use position before its type was known;
    // `constVariableType` is what fills it in and what a reader should go through.
    TypePtr constType = nullptr;

    /*
     * What an application that omits this parameter gets - the `Int` of `a = Int` and the `0` of
     * `n: Int = 0`. Null for a parameter with no default.
     *
     * A resolved type and not the written one, and always a *concrete* one: a default that mentioned
     * another parameter would be an argument whose meaning depended on the order the list was filled
     * in, and there is no reading of that worth having in a first version. `genDefault` is what
     * checks it and what fills this in.
     *
     * Read at two different moments, which is the one thing about this feature that is not one rule.
     * A *type application* that omits an argument takes the default where it is written -
     * `applyGenDefaults`, at resolve time - because a written type has nothing later to decide it. A
     * function's or a class's parameter takes it at the **settle** instead - `Solver::settle` - so
     * that inference gets first refusal: `vectorAt(p) :: Vec(U8, 16)` has to bind `n` to sixteen,
     * and a default filled in eagerly would have bound zero and then failed to unify with it.
     */
    TypePtr def = nullptr;
};

/*
 * One `Class(a, b)` requirement of a context. `args` are the context's own types (or concrete
 * types, for a partially applied constraint), in the class's argument order.
 *
 * An argument is an arbitrary type - `Num(Vec(I16, n))`, `Contiguous(c, Pair(k, v))` - and every
 * reader here already treated it as one: `fillDetermined` substitutes and matches it,
 * `superclassPath` expresses a superclass in it, and `internedEnv` substitutes it before asking for
 * a witness. Implementation-Const-Generics.md §10 is the change that let one be *written*.
 *
 * `written` is what the declaration said, kept only while the class is still unknown. A `data` or
 * `class` head may constrain itself by a class declared further down the file, so its arguments
 * cannot be resolved where the head is - which positions are counts is read off the class's own
 * parameter list. Those two contexts are closed, so nothing they mention is introduced by the
 * constraint and resolving late costs no ordering; `resolveConstraintClasses` finishes them. A
 * function's or an instance's context resolves in place, since every class is declared by then.
 */
struct ClassConstraint {
    GlobalPtr<TypeClass> typeClass = nullptr;
    GlobalList<TypePtr> args;
    ast::ParseList<ast::Type> written;
    StringId name {};
    LocationId source = kNullLocation;
};

/*
 * One `a.field: b` requirement of a context - Design.md's structural field constraint.
 *
 * `owner` and `result` are the context's own types, so the relation is between two slots of one
 * context rather than a fact attached globally to `a`. What satisfies it at an instantiation is a
 * PropertyWitness: the scoped read/set/modify of that one field, on the owner's selected Repr.
 */
struct PropertyConstraint {
    TypePtr owner = nullptr;
    TypePtr result = nullptr;
    StringId field {};
    LocationId source = kNullLocation;
};

/*
 * One `f: (a) -> b` requirement of a context.
 *
 * `signature` is a FunType, so the conventions and the `return` group a constrained callable
 * promises are part of the requirement rather than being lost at the boundary - which is exactly
 * what FunArg exists for. Satisfied by a FunctionWitness.
 */
struct FunctionConstraint {
    TypePtr signature = nullptr;
    StringId name {};
    LocationId source = kNullLocation;
};

/*
 * What a runtime environment carries, and in which order.
 *
 * Implementation-Generics.md part 2 asks for slots canonicalized by structural key rather than by
 * the order a hash table happened to return them, because a slot number is what emitted code
 * *loads*: the caller writes slot 3 and the callee reads slot 3, and if the two disagreed about
 * what 3 meant nothing would say so. So the numbering is derived once, from the context, by a rule
 * that does not depend on how the context was built up.
 */
enum class GenSlotKind: U8 {
    // A TypeDesc: the identity, size, alignment and lifecycle of one type variable or of one
    // applied type expression the body uses.
    Type,

    // A ClassWitness: one typeclass implementation and its method table.
    Class,

    // A PropertyWitness: the scoped read/set/modify of one constrained field.
    Property,

    // A FunctionWitness: one constrained callable.
    Function,

    /*
     * One number - the `n` of a const parameter, Implementation-Const-Generics.md §3.1.
     *
     * The narrowest entry the environment has: not a pointer to anything, just the value the caller
     * had in its hand. It sits after the type descriptors and before the witnesses so that both
     * fixed-width groups stay a prefix - see GenSchema::constCount.
     */
    Const,
};

struct GenSlot {
    GenSlotKind kind = GenSlotKind::Type;
    U16 index = 0;

    // Type slot: the variable or applied expression it describes.
    // Property slot: the owner type. Function slot: the signature.
    TypePtr type = nullptr;

    // Class slot: the class and the types it is required for.
    GlobalPtr<TypeClass> typeClass = nullptr;
    GlobalList<TypePtr> args;

    // Property slot: the field name and its type. Function slot: the function name.
    StringId name {};
    TypePtr result = nullptr;

    LocationId source = kNullLocation;
};

struct GenSchema {
    GlobalList<GenSlot> slots;

    // How many leading slots are type descriptors. Everything else indexes off this, and it is what
    // a caller building an environment fills in first.
    U16 typeCount = 0;

    // How many slots after those are const parameters - Implementation-Const-Generics.md §3.1. The
    // two fixed-width groups are a prefix together, so a caller fills `typeCount + constCount` slots
    // before it reaches anything pointer-shaped.
    U16 constCount = 0;
};

/*
 * One parameter's default as it was written, kept until something asks for it.
 *
 * Resolved lazily rather than in the pass that builds the head, because a default may name a type
 * declared further down the file and a head is built in declaration order. That is the same reason
 * `ClassConstraint::written` exists; what differs is who does the deferred work. A constraint's
 * class is finished by its owner at a point the owner picks (`resolveConstraintClasses`), which
 * works because nothing outside asks. A default *is* asked from outside - by every application that
 * omits an argument, in whatever module wrote it - so it is finished on demand and in the module
 * that declared it instead.
 */
struct GenDefault {
    U16 index = 0;
    ast::ParsePtr<ast::Type> written = nullptr;
    LocationId source = kNullLocation;
};

struct GenEnv {
    enum Kind: U8 {
        Record,
        Alias,
        Class,
        Instance,
        Function,
    };

    explicit GenEnv(Kind kind): kind(kind) {}

    GlobalList<GlobalPtr<GenType>> types;

    // Applied type expressions the body uses - `Maybe(a)`, `Pair(b, a)`. They get descriptor slots
    // of their own so that the caller, which knows the concrete arguments, builds each one once
    // instead of the callee re-applying a type constructor per use.
    GlobalList<TypePtr> derivedTypes;

    GlobalList<ClassConstraint> classes;

    /*
     * Classes the body dispatches on that `classes` does not name directly, and that nothing in
     * `classes` reaches either.
     *
     * A requirement one already in scope *implies* is deliberately not recorded as a constraint -
     * `fn (Num(a)) inc(x: a) = x + 1` declares `Num(a)` and not also the `FromInt(a)` its superclass
     * guarantees, because a diagnostic naming both would be naming the same promise twice. Nor does
     * it get a slot: a `ClassWitness` names its superclasses' witnesses, so the literal's `fromInt`
     * is dispatched through the `Num` witness the caller already passed - see genWitnessPath, which
     * is Implementation-Generics.md part 6's "superclasses reference other class witnesses".
     *
     * What is left for this list is a requirement no declared one implies, which a body infers by
     * using it - the `Ord(a)` a comparison records. It is kept apart from the declared list so that
     * only what the author wrote is printed; by the time anything reads the numbering, the two are
     * the same kind of entry.
     */
    GlobalList<ClassConstraint> dispatched;

    GlobalList<PropertyConstraint> properties;
    GlobalList<FunctionConstraint> functions;

    // The defaults this head wrote, in parameter order, and only for the parameters that wrote one.
    // Empty once `defaults` has moved them onto the variables, which is where every reader looks.
    GlobalList<GenDefault> writtenDefaults;

    // Where this context was declared: what the names in a default mean, and where a diagnostic
    // about one belongs. Mirrors TypeAlias::module and is set for the same reason.
    Module* module = nullptr;

    Kind kind;

    // A function context has no declared variable list: `fn id(x: a) -> a` introduces `a` by
    // using it. An open context adds a variable the first time a type mentions one, which numbers
    // them in order of appearance across the constraints and then the signature.
    bool open = false;

    // Whether `writtenDefaults` has been spent. Set *before* the defaults are resolved, so that a
    // default written in terms of its own declaration - `data A(a = A)` - ends as the arity
    // diagnostic it is rather than as a loop. Same shape as TypeAlias::resolving.
    bool defaultsResolved = false;

    // The canonical numbering, built on first request and invalidated by anything that adds a
    // requirement. Deliberately derived rather than maintained: a body infers requirements while it
    // is being resolved, and a numbering that shifted underneath half-emitted code would be worse
    // than one that does not exist yet.
    GlobalPtr<GenSchema> schema = nullptr;
};

// The canonical schema of one context, built if it does not exist yet. Every slot number anything
// emits comes from here.
GenSchema& genSchemaOf(Module& module, GenEnv& env);

// Discards a context's cached numbering. Called by whatever adds a requirement to it.
inline void invalidateGenSchema(GenEnv& env) { env.schema = nullptr; }

// Where in the canonical numbering one requirement sits, or maxLimit when the context has no such
// slot. These are what an emitted load of an environment slot is built from.
U16 genTypeSlot(Module& module, GenEnv& env, TypePtr type);
U16 genClassSlot(Module& module, GenEnv& env, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args);

// Records that the body dispatches on this class, giving it a slot if it does not have one. Adding
// one renumbers the context, so this happens while the body is being resolved and never after.
void requireClassSlot(Module& module, GenEnv& env, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                      LocationId source);
U16 genPropertySlot(Module& module, GenEnv& env, TypePtr owner, StringId field);
U16 genFunctionSlot(Module& module, GenEnv& env, StringId name, TypePtr signature);

// Records that the body needs a TypeDesc for this type expression - a type variable, or an applied
// type such as `Maybe(a)` it constructs or matches. Adding one renumbers the context, so this
// happens while the body is being resolved and never after.
void requireTypeSlot(Module& module, GenEnv& env, TypePtr type);

/*
 * Where one const parameter sits in the numbering - Implementation-Const-Generics.md §3.1.
 *
 * There is no `requireConstSlot` beside it, unlike every other kind of slot, and the reason is
 * §2.5: a count is a bare variable or a literal and never an expression, so the *only* counts a body
 * can read are the ones its context already declares. There is nothing for a body to discover, so
 * every const variable is numbered from the declaration exactly as a type variable is.
 */
U16 genConstSlot(Module& module, GenEnv& env, TypePtr variable);

// The type variable of `env` called `name`, adding it if the context is open. Null when the
// context is closed and has no such variable.
// `source` is where this occurrence was written, and it is recorded as the variable's binder when
// this is the occurrence that creates it - see GenType::source.
GlobalPtr<GenType> genVariable(Module& module, GenEnv& env, StringId name, LocationId source = kNullLocation);

// The same lookup without the creation, which is what a caller that needs to know whether an
// occurrence *introduced* a variable asks first - see §1.5's kind inference.
GlobalPtr<GenType> findGenVariable(Module& module, GenEnv& env, StringId name);

struct RecordType: Type {
    enum Layout: U8 {
        Enum,
        Single,
        Multi,
    };

    explicit RecordType(StringId name):
        Type(Type::Record), name(name) {}

    // The declaration this type came from: itself for a plain or generic declaration, and the
    // generic declaration for one of its instantiations.
    GlobalPtr<RecordType> base(GlobalBase global) {
        return instanceOf ? instanceOf : (RecordType*)this - global;
    }

    GlobalList<Constructor> constructors;
    StringId name;

    // Where the declaration was written. Carried on the declaration only: an instantiation reads
    // it through base(), since `Maybe(Int)` is not something anything jumps to.
    LocationId source = kNullLocation;

    // Set on a generic declaration: its type variables, and the instantiations made from it.
    GlobalPtr<GenEnv> gen = nullptr;
    GlobalList<GlobalPtr<RecordType>> instances;

    // Set on an instantiation: what it was made from, and with which concrete arguments.
    GlobalPtr<RecordType> instanceOf = nullptr;
    GlobalList<TypePtr> instanceArgs;

    /*
     * A Repr refinement of a container - Implementation-Containers.md §7.
     *
     * `@inline(i)` is how many elements are stored inside the container itself and `@capacity(c)` is
     * how many it may ever hold; zero in each is "unrefined", which is what every record that is not
     * one of these is. A refined instantiation has the *same* constructors and the same field types
     * as the plain one and differs only in what its Repr does with them, which is what §9's "a Repr
     * variant may never change what a type can do" means concretely: it is a second instantiation of
     * one declaration rather than a second declaration.
     *
     * `canonical` is the plain instantiation, and it is what makes the refinement invisible to
     * dispatch without a single change to instance selection: `matchType`'s Record case compares
     * `instanceOf` and `instanceArgs`, both of which a refinement leaves alone, so
     * `instance Reclaim(Array(a))` answers a refined array already. What canonical is *for* is the
     * conversion - a call taking `Array(a)` is compiled once, against the plain layout, so a refined
     * argument reaches it through the descriptor `inlineArrayDescriptor` builds.
     */
    U32 inlineSlots = 0;
    U32 capacityBound = 0;
    GlobalPtr<RecordType> canonical = nullptr;

    bool isRefined() const { return canonical != nullptr; }

    Layout layout = Multi;

    // Set by `@layout(c)`: the layout is the declaration's to decide and no target may improve on
    // it. The content tuples carry the same fact as part of their identity - see TypeLayout - and
    // this is the declaration's own copy of it, for the paths that have the record and not a tuple.
    bool pinned = false;
    bool qualified = false;
    bool definitionReady = false;

    /*
     * Set once `breakLayoutCycles` has walked this record with everything reachable from it already
     * defined, which is what makes the cut it chooses a function of declaration order rather than of
     * which declaration the walk happened to start at.
     *
     * Without it, `data A {b: B}` / `data B {a: Maybe(A)}` is cut once starting from `A` and cut a
     * *second* time starting from `B` - both correct, and two indirections where one was needed. The
     * flag is deliberately not set when the walk met a record that was still being defined, since
     * such a walk has not seen the cycle it is meant to find.
     */
    bool layoutBroken = false;
};

struct ConstructorRef {
    GlobalPtr<RecordType> record = nullptr;
    U16 index = 0;

    explicit operator bool() const { return record != nullptr; }
};

// A generic `alias` declaration. Aliases are transparent - resolving one substitutes straight
// through to the target type - so they are not a Type kind and never reach the IR.
//
// The target is kept as AST and resolved on first use, so an alias may name a type declared
// after it. `module` is where that resolution happens: an alias reached through an import
// resolves its target in the module that wrote it, not in the one that named it.
struct TypeAlias {
    StringId name {};
    Module* module = nullptr;
    GlobalPtr<GenEnv> gen = nullptr;
    ast::ParsePtr<ast::Decl> ast = nullptr;
    TypePtr resolved = nullptr;
    LocationId source = kNullLocation;
    bool resolving = false;

    // Whether the declaration wrote `pub`. An alias is transparent, so this restricts the *name*
    // and says nothing about the type behind it: a private alias for a `pub` record hides one
    // spelling of a type an importer can still name.
    bool exported = false;
};

struct ScalarTypes {
    TypePtr error = nullptr;
    TypePtr unit = nullptr;
    TypePtr bool_ = nullptr;
    TypePtr int_ = nullptr;
    TypePtr long_ = nullptr;
    TypePtr float_ = nullptr;
    TypePtr double_ = nullptr;
    TypePtr ordering = nullptr;

    // `Size` - the target's own index width (Implementation-Containers.md §4.4). A name for one of
    // the two above rather than a type of its own, recorded here because the compiler builds indices
    // as well as reading them: a host element's index is a place's operand, and a `Long` one would
    // emit a `bigint` where the host wants a number.
    TypePtr size = nullptr;

    // The same width, unsigned - what a bounds test compares at, so that a negative index arrives as
    // a number above every length there is and one comparison rejects both ways of being wrong.
    TypePtr unsignedSize = nullptr;

    // `String` itself, and the tuple its native Repr is computed from - see Type::String.
    TypePtr string_ = nullptr;

    /*
     * `{Run(U8), Count}` - what a native `String` occupies, borrowed from the container that
     * already occupies it.
     *
     * Delegating to a tuple rather than writing sixteen bytes into the Repr by hand is what keeps
     * this honest: the count packs into the run's spare bits by Implementation-Containers.md §10.3's
     * own rule, so a `String` is two words for the same reason and by the same code that makes
     * `Array(a)` two words. Writing the layout out here would be a second implementation of §10.3
     * that could drift from the first, and the symptom would be a string whose length field and the
     * library's idea of where it lives disagreed.
     *
     * Null on JS, where a string is one host value and there is nothing to lay out.
     */
    TypePtr stringContent = nullptr;

    /*
     * `I8`, `I16`, `I32` and `I64`, indexed by the logarithm of their byte width.
     *
     * Here for one reader, and it is a *lane number* rather than a lane: `maskUpTo` compares the
     * lane indices against the live count, and both of those are small exact integers whatever the
     * vector is over - so the comparison is built at the lane's width in the integer domain and the
     * mask reinterpreted, rather than converted into the lane's own type and compared there. Signed
     * rather than unsigned because that is the relation the machines have: an unsigned packed
     * comparison is a bias and two exclusive-ors on x64, and nothing here is ever negative.
     *
     * Asked for from any module - the intrinsic runs where the call is - and only Core knows these
     * types by name. It replaces an unsigned family that was left behind when a mask stopped
     * normalizing its element (see `resolveVectorType`) and that nothing had read since.
     */
    TypePtr signedLanes[4] = {};
};

// The five Core classes the resolver has to know by name rather than by lookup, because the
// language's own syntax is written in terms of them: a literal is a call to one of the first
// two, an implicit conversion is a call to `widen`, and a condition is a call to `truthy`. They
// are ordinary classes in every other respect - user types join them by writing an instance, and
// nothing about selection or instance lookup treats them specially.
struct CoreClasses {
    GlobalPtr<TypeClass> fromInt = nullptr;
    GlobalPtr<TypeClass> fromDecimal = nullptr;
    GlobalPtr<TypeClass> widen = nullptr;
    GlobalPtr<TypeClass> narrow = nullptr;
    GlobalPtr<TypeClass> truth = nullptr;

    // What a payload-free sum's number is reached through - Analysis-Language.md §5.1. Known by name
    // for the reason the vector classes below are: no instance of it is declared anywhere, so
    // "could this type join it" is asked at every lookup that found nothing and must not be a
    // string comparison.
    GlobalPtr<TypeClass> enum_ = nullptr;

    // Which of a carrier type's two paths a value is on - Implementation-Semantics.md part 5. Known
    // by name because a skipping lens's call site asks it about a type the *program* named, so there
    // is no argument position for ordinary overload selection to find it from.
    GlobalPtr<TypeClass> try_ = nullptr;

    // The same carrier around a different payload, which is what `?.` rebuilds a chain's result
    // with. `Try` reads a carrier to what is inside it; this reads the other way, and is known by
    // name for the same reason `Try` is - nothing in an argument list would find it.
    GlobalPtr<TypeClass> rewrap = nullptr;

    // What `xs[i]` dispatches through. Known by name only so that a type with no instance gets a
    // diagnostic about subscripting rather than the overload set's answer about `get` - the call
    // itself goes through the ordinary overload set like every other class function.
    GlobalPtr<TypeClass> index = nullptr;

    // And what `m[k] = v` dispatches through, where the container has an instance of it -
    // Implementation-Map.md §7. Known by name for the same reason `index` is, and consulted at the
    // same place: an assignment through a subscript prefers this and falls back to `getMut`, so
    // `Array`'s assignment is unchanged and a map's inserts instead of trapping on a key it has not
    // got. Null on a program whose `Collections` did not declare it, which is every program that
    // predates the map.
    GlobalPtr<TypeClass> indexInsert = nullptr;

    // The ownership classes. They are known by name for the same reason the five above are:
    // `let ->z = x` and the end of a value's lifetime are language syntax, and what they compile
    // to is a lookup of these.
    GlobalPtr<TypeClass> copy = nullptr;
    GlobalPtr<TypeClass> reclaim = nullptr;
    GlobalPtr<TypeClass> drop = nullptr;

    // The two implicit ones. Unlike the four above, no instance of either is ever *written* - the
    // compiler answers them structurally - but they are real classes so that a signature can
    // constrain a type variable by one and a body may then act on the fact. See ownershipOf.
    GlobalPtr<TypeClass> trivialCopy = nullptr;
    GlobalPtr<TypeClass> trivialSink = nullptr;

    /*
     * The two container classes - Implementation-Containers.md §5. Collections' rather than Core's,
     * because a container is written over `Native` and Core cannot name it.
     *
     * `Contiguous` is known by name because it is what `[a]` *means*: a container that promises a
     * buffer address may be passed where a slice is expected, and that conversion is looked for at
     * an argument position with nothing to select it from. `Chunked` is known by name only so that
     * the refusal can name it - a container that iterates but does not promise an address is
     * rejected there, and what the author has to change is the parameter rather than the argument.
     */
    GlobalPtr<TypeClass> contiguous = nullptr;
    GlobalPtr<TypeClass> chunked = nullptr;

    // What `"a{x}b"` dispatches through - Implementation-Storage.md part 7. Known by name for the
    // same reason `Try` is: a hole's `show` and `showBound` are selected by the compiler from the
    // hole's type, and there is no written call for the ordinary overload set to start from.
    GlobalPtr<TypeClass> show = nullptr;

    // The two comparisons a payload-free sum gets for nothing - see `enumEqInstance`. Known by name
    // on `enum_`'s terms and consulted at the same place: a type with no declared instance is asked
    // whether it is an enum, and a string comparison at every failed lookup is what that must not
    // cost.
    GlobalPtr<TypeClass> eq = nullptr;
    GlobalPtr<TypeClass> ord = nullptr;

    /*
     * The classes a *vector* joins on demand - Implementation-Vector.md §9 items 1 to 3.
     *
     * Known by name for a reason none of the above has: no instance of any of them over a vector is
     * declared anywhere, and one is generated the first time it is asked for (see simd.cpp). So the
     * question "is this the class a vector could join" is asked at every instance lookup that finds
     * nothing, and asking it by string lookup would put a hash of `"Integral"` on that path.
     *
     * `widen`, `narrow` and `fromInt` are up above and are read for this too; these are the five
     * that had no other reader.
     */
    GlobalPtr<TypeClass> num = nullptr;
    GlobalPtr<TypeClass> integral = nullptr;
    GlobalPtr<TypeClass> bitwise = nullptr;
    GlobalPtr<TypeClass> bitcast = nullptr;
    GlobalPtr<TypeClass> lanewise = nullptr;
};

/*
 * Resolving types.
 *
 * `env` is the generic context the type is written in, if any: it is what makes a lowercase
 * name in a declaration resolve to that declaration's own type variable rather than being an
 * error. A null env means no type variable is in scope.
 */
TypePtr resolveType(Module& module, const ast::Type& type, GenEnv* env = nullptr);

/*
 * The type of `value :: [T *_]` - the count read off the literal the ascription is written at.
 *
 * A helper rather than a case inside `resolveType`, because the count is not in the type at all:
 * it is a fact about the *expression* on the other side of the `::`, and `resolveType` is handed
 * only the type. The two callers are the two places an ascription is resolved - the expression
 * resolver's `Coerce` arm and the constant evaluator's - which is exactly the set of positions
 * where a literal supplies one.
 *
 * Null, having reported, where the ascribed value is not an array literal. `resolveType` reports
 * the same kind reaching it any other way, since a parameter or a field has nothing to count.
 */
TypePtr inferredArrayType(Module& module, const ast::Type& written, const ast::Expr& value,
                          GenEnv* env);

// One argument of a written application, at whichever kind the *declaration's* parameter at that
// index is: a type, or a count that takes an integer literal or a const parameter. `declared` is the
// context of whatever is being applied - a record's, an alias's or a class's.
TypePtr resolveAppArg(Module& module, GlobalPtr<GenEnv> declared, Size index,
                      const ast::Type& arg, GenEnv* env);

/*
 * One parameter's written default, at whichever kind that parameter is - the `Int` of `a = Int` and
 * the `0` of `n: Int = 0`. Null where the default was refused, having reported.
 *
 * The same two forms `resolveAppArg` reads, because a default *is* an argument: what it supplies is
 * what the omitted position would have carried, so reading it by a second set of rules would make
 * `A()` and `A(Int)` two questions.
 *
 * `env` is the declaration's own context, which the default is resolved in only so that a name it
 * mentions is looked up where the declaration is - not so that it may use one. A default must be
 * concrete, and this is where that is reported.
 */
TypePtr resolveGenDefault(Module& module, GlobalPtr<GenType> variable, const ast::Type& written,
                          GenEnv* env, LocationId source);

/*
 * The arguments a written application left off, filled in from the declaration's defaults.
 *
 * Appends to `args` until it is as long as `declared`'s parameter list or until it reaches a
 * parameter with no default, and answers whether it got all the way. False leaves `args` as it
 * found it plus whatever it could fill, and the *caller* reports: an arity message belongs to
 * whatever was applied, and this function is shared by the record, the alias, the class constraint
 * and the vector constructor.
 *
 * Defaults fill from the right, because an application writes its arguments in order and stops, so
 * the positions it omitted are a suffix. A declaration whose *defaulted* parameters are not a
 * suffix is refused where it is written - see resolveGenDefaults - rather than here.
 */
bool applyGenDefaults(Module& module, GlobalPtr<GenEnv> declared, TypeList& args);

/*
 * A head's written defaults, moved onto its variables. Idempotent, and normally reached through
 * `applyGenDefaults` rather than called - the exception is a reader that wants one parameter's
 * default rather than a whole argument list, which is what a class default is.
 */
void resolveGenDefaults(Module& from, GlobalPtr<GenEnv> declared);

TupType* resolveTupleType(Module& module, Buffer<Field> fields, LocationId source,
                          TypeLayout layout = TypeLayout::Auto, U32 inlineSlots = 0,
                          U32 capacityBound = 0);

// The raw pointer type to `to`, interned per target type.
TypePtr resolvePointerType(Module& module, TypePtr to);

/*
 * The type one step of a place path arrives at, where the edge it crossed is a box.
 *
 * `Field::boxed` and `Constructor::boxed` mean the storage at that step holds a `%T`, and `project`
 * appends the Deref that turns it back into a `T`. Every walk over a path - the resolver's, the
 * native and JS place walks, the optimizer's alias analysis - has to agree about that, because one
 * that missed it would add the next offset to a pointer instead of loading through it.
 *
 * So the rule is written once and each walk calls it with whatever it already read out of the field
 * or the constructor.
 */
inline TypePtr boxedStep(Module& module, TypePtr type, bool boxed) {
    return boxed && type ? resolvePointerType(module, type) : type;
}

// The borrow type `&to`, interned per target type and mutability.
TypePtr resolveBorrowType(Module& module, TypePtr to, bool mut);

// The function type these arguments, result and kind name, interned on all three. Every argument's
// convention and `return` marker is part of the key - see FunArg.
TypePtr resolveFunType(Module& module, Buffer<FunArg> args, TypePtr result, ast::FunKind kind,
                       ast::BindType resultBind = ast::BindType::Borrow);

// The type a `@lazy` argument travels as: the nullary thunk that produces the declared type. Not a
// type source can write in that position - the signature says `T` - so it exists only between the
// call site that builds one and the force that runs it.
TypePtr resolveThunkType(Module& module, TypePtr result);

// Whether a `@lazy` marker is valid on an argument of this convention, reporting what is wrong with
// it when it is not. Shared by a declaration's signature and by a written function type.
bool checkLazyArgument(Module& module, ast::BindType convention, bool returnRoot, LocationId source);

// Whether a `return` marker is valid on an argument of this type, convention and position,
// reporting what is wrong with it when it is not. Shared by a declaration's signature and by a
// written function type, so that a contract means the same thing in both places.
bool checkReturnRoot(Module& module, TypePtr type, ast::BindType convention, U32 index, LocationId source);

// Instantiates a generic record for a set of fully concrete arguments, interning the result so
// that `Maybe(Int)` names one type no matter how many places write it.
TypePtr instantiateRecord(Module& module, GlobalPtr<RecordType> record, Buffer<TypePtr> args, LocationId source);

/*
 * The four container questions the resolver asks by type rather than by name.
 *
 * `arrayElement` and `sliceElement` are what an `Array(T)` and a `Flat(T)` hold, or null for
 * anything else. `sliceOf` is the slice a borrow of a container becomes - `Array(T)` and `Flat(T)`
 * both answer `Flat(T)`, since a borrow of a slice is that slice - and null for every type that is
 * not one, which is what makes it usable as a test as well as a conversion.
 *
 * `sliceLengthType` is the declared type of a slice descriptor's second field, asked because
 * `convertSlice` builds one out of an *owner's* count and the two are not the same type: an owner's
 * is narrow and unsigned so that it packs (§10.2), a descriptor's is an `Int` because `Flat` is a
 * type programs are written against. They were one type by accident until one of them moved, and
 * reading the declaration is what keeps the next move from being silent.
 */
// What a parameter's written type means, which is not always what the same syntax means in a type
// position: `[T]` in a binding is a slice. See the definition for which positions keep the owner.
// Shared by a declaration's signature and by a written function type, so that a contract means the
// same thing in both places.
TypePtr bindingType(Module& module, const ast::Type& written, ast::BindType bind, GenEnv* env);

TypePtr arrayElement(Module& module, TypePtr type);
TypePtr sliceElement(Module& module, TypePtr type);
TypePtr sliceLengthType(Module& module, TypePtr type);
TypePtr sliceOf(Module& module, TypePtr type);

/*
 * And the fifth, which is the map's - `Map(K, V)`'s two arguments, or false for anything that is not
 * one. The same test-and-read `arrayElement` is, with two answers instead of one.
 *
 * Both come out of one call rather than two, because a caller wanting either almost always wants
 * both and asking twice would walk the record twice for one answer. False for the generic
 * declaration itself, on the same terms as `arrayElement`: what it recognizes is an instantiation.
 */
bool mapKeyValue(Module& module, TypePtr type, TypePtr& key, TypePtr& value);

/*
 * A container of somebody else's, and what it says it is - Implementation-Containers.md §5.
 *
 * `contiguousElement` is the element type of a `Contiguous` instance for this type, which is what
 * makes a container written outside the library passable where `[T]` is expected: the conversion is
 * a call to `elements`, and this is the question that finds it. `chunkedElement` is the same for
 * `Chunked` and exists only so a refusal can name the class - a container that iterates and does not
 * promise a buffer address is exactly the case §5 rules out, and the fix is the parameter rather
 * than the argument.
 *
 * Both answer null for the two containers the compiler already knows the layout of, so `Array(T)`
 * and `Flat(T)` keep reaching a slice through convertSlice rather than through a call: the instances
 * exist for generic code and are not what the direct path should be spending.
 */
TypePtr contiguousElement(Module& module, TypePtr type);
TypePtr chunkedElement(Module& module, TypePtr type);

/*
 * `[T *n]` - Implementation-Containers.md §6.
 *
 * `resolveFixedArrayType` interns one; `fixedElement` is what one holds, or null for anything that
 * is not one, so that it reads as a test the way `arrayElement` does.
 *
 * `ownedElement` is either kind of *owner* - `Array(T)` or `[T *n]` - and it is the question every
 * site that converts an owner to a slice actually asks. Deliberately not including `Flat(T)`: the
 * two owners are what a borrow is *taken of*, and a caller that treats a slice as a third one ends
 * up trying to borrow a descriptor out of itself.
 */
/*
 * The longest `[T *n]` the compiler accepts.
 *
 * A bound on the *count* and not on the byte size, because the byte size is a target's answer and
 * this type is a resolve-stage one: `[U8 *65536]` and `[Buffer *65536]` are the same declaration
 * here and differ only in what some backend later multiplies by. Sixteen bits is far past anything
 * §6's purpose - a small inline array that flattens into a record - and it keeps the derived
 * teardown's unrolled/looped split a decision about *shape* rather than a guard against a length
 * that would take a compiler down.
 */
constexpr U32 kMaxFixedArrayLength = 0xffff;

TypePtr resolveFixedArrayType(Module& module, TypePtr content, U32 length, LocationId source);

// The same, for a caller holding a count that may be a variable - which is every path that walks an
// existing type rather than reading one that was written.
TypePtr resolveFixedArrayType(Module& module, TypePtr content, TypePtr count, LocationId source);

TypePtr fixedElement(Module& module, TypePtr type);
TypePtr ownedElement(Module& module, TypePtr type);

/*
 * `Vec(a)`, `Vec(a, n)` and `Mask(a)` - Design-Vector §2, Implementation-Vector.md §1.
 *
 * `lanes` of zero asks for the natural form, which is `targetVectorBytes(settings) / laneStride`.
 * Over a type variable there is no stride to divide by, so the zero survives into the type and
 * `substituteType` resolves it again once the element is concrete - the same deferral the fixed
 * array's element already has, and the only thing in this design that is not settled by the end of
 * resolution.
 *
 * Two rejections, both reported here so that a bad element names the type it was written on rather
 * than failing later in Repr:
 *
 *  - the element has to be an integer or a float whose storage is 8, 16, 32 or 64 bits;
 *  - a 64-bit integer element is refused on JS, since a `BigInt64Array` lane is a heap value and not
 *    a lane at all (Design-Vector §7.3).
 */
TypePtr resolveVectorType(Module& module, TypePtr content, U32 lanes, bool isMask, LocationId source);

// The same, for a count that may be a variable. Null is the natural form, exactly as zero is above.
TypePtr resolveVectorType(Module& module, TypePtr content, TypePtr count, bool isMask, LocationId source);

/*
 * A number in a count position - Implementation-Const-Generics.md §2.1.
 *
 * `constType` interns one. `constValue` reads a count back as a number and is only for a position
 * that has already established the type is concrete: a variable count has no number here, which is
 * the whole of what makes it a variable, so asking is a compiler bug rather than a program error.
 * `writtenCount` is the same question asked by a caller that may legitimately get either answer.
 */
TypePtr constType(Module& module, U64 value, TypePtr of);

/*
 * Whether a written annotation is a type a const parameter may have -
 * Implementation-Const-Generics.md §2.5.
 *
 * The integer types and nothing else in this version, which is what every count position that
 * exists needs. Which types are admissible is a *semantic* rule rather than a syntactic one - the
 * grammar takes any type - so loosening it is this one predicate and no production. Reported at the
 * declaration, since that is where the inadmissible type was named.
 */
bool admissibleConstType(Module& module, TypePtr type, LocationId source);
U64 constValue(GlobalBase base, TypePtr count);
Maybe<U64> writtenCount(GlobalBase base, TypePtr count);

// Whether a vector's count is the request for the target's natural one, which is spelled as a null
// count where nothing wrote a number and as a zero where a deferred type had to write it down - see
// resolveVectorType. Nothing outside that function should distinguish the two.
bool isNaturalCount(GlobalBase base, TypePtr count);

// How many bytes one lane of this type occupies, or zero where it is not a type a lane may be -
// which is what the rule above is written in terms of. An integer answers its natural storage and
// so a `@bits` refinement answers the storage it is held in, exactly as a standalone value of it
// does; Design-Vector §2.2 accepts the refinement precisely so that the lanes keep its range.
U32 laneStride(GlobalBase base, TypePtr type, IntWidths widths);

// The lane type of a vector or a mask, or null for anything else - the shape `fixedElement` has, and
// usable as a test for the same reason.
TypePtr vectorLane(GlobalBase base, TypePtr type);

// How many lanes a vector or a mask has, or zero for anything that is not one. Zero is also the
// unresolved natural form, which is only reachable inside a generic body.
U32 vectorLanes(GlobalBase base, TypePtr type);

bool isVectorType(GlobalBase base, TypePtr type);
bool isMaskType(GlobalBase base, TypePtr type);

// Whether `/` and `%` at this type are the checked, language-defined pair - see the ruling beside
// `Div` in inst.def. Scalar integers only.
bool isCheckedDivisionType(GlobalBase base, TypePtr type);

// The mask a comparison of this vector answers - Design-Vector §2.4. Null for anything that is not
// a vector, so it reads as the test "does a lane comparison of this mean anything".
TypePtr maskFor(Module& module, TypePtr type);

/*
 * The `@inline(i)` / `@capacity(c)` family - Implementation-Containers.md §7.
 *
 * `refineContainerType` interns the refined instantiation; `inlineRefinement` is the question every
 * other stage asks - "is this array one whose slots are its own bytes", answered as the refined
 * record or null so that the caller has the counts without a second lookup.
 *
 * `unrefined` is what a *call* needs: every function taking `Array(a)` was compiled once against the
 * plain layout, so a refined argument is converted into a descriptor over the plain one at the
 * boundary. That is §7.2's tier-1 borrow, and it is the same mechanism a `@bits` field's `&` uses.
 */
/*
 * The bound on `@inline(n)`, and it is the fixed array's for the same reason.
 *
 * `n` inline slots is `n` elements of storage inside whatever contains the array, so the number that
 * would be unreasonable here is the number that would be unreasonable in `[T *n]` - and the derived
 * teardown over the elements is the same walk, with the same unrolled/looped split.
 */
constexpr U32 kMaxInlineSlots = kMaxFixedArrayLength;

/*
 * `Native.runFixed`, which is the one value of `HeapFlag` the compiler writes rather than the escape
 * analysis. The other two are `InstAlloc::storageFlag`'s answer to "is this storage the allocator's";
 * this one is the answer to "may these slots be replaced", and only a descriptor built over an
 * owner's own bytes says no. Kept beside the refinement helpers because that descriptor is the only
 * thing that produces it - see ExprResolver::inlineArrayDescriptor.
 */
constexpr U64 kRunFixed = 2;

TypePtr refineContainerType(Module& module, TypePtr plain, U32 inlineSlots, U32 capacityBound,
                            LocationId source);
RecordType* inlineRefinement(Module& module, TypePtr type);
TypePtr unrefined(GlobalBase base, TypePtr type);

// Whether this is Collections' growable array - an instantiation of it, or the generic declaration
// itself, which is what a signature written `Array(a)` resolves to. Asked where a diagnostic has to
// tell a growable *parameter* apart from any other record, since only the operations that grow name
// the type and a fixed array reaching one of them is §6's one rejection.
bool isGrowableArray(Module& module, TypePtr type);

/*
 * Whether a value of this type names storage it does not own.
 *
 * `&T` and the slice a borrow of a container *is* (Implementation-Containers.md §4), which are the
 * same thing said twice: a slice is a borrow whose representation happens to be a record, so every
 * rule about a reference outliving what it refers to has to ask this rather than isBorrow. Returning
 * one, capturing one in an escaping closure, and storing one past the loan that made it are all the
 * same mistake, and all three were invisible while the question was about the type's *kind*.
 *
 * A raw pointer is deliberately not one. `%T` carries no lifetime by construction - that is what
 * makes Native the unchecked module - so including it here would report on the one type whose whole
 * purpose is to be outside this analysis.
 */
bool isBorrowLike(Module& module, TypePtr type);

/*
 * The same question about a type's members, which is what a *result* has to be judged by.
 *
 * `data Cursor {items: &[Int], at: Int}` returned from a function hands a reference to the caller as
 * surely as `&[Int]` does, so the return-root contract has to cover it: the signature must say which
 * argument the reference inside it came from, or there is nothing the caller can be checked against.
 *
 * Bounded rather than exhaustive, on the same terms ownershipIn's walk is - a type reachable from
 * itself is finite in the answers it can give and the bound is what makes that true of the walk.
 */
bool containsBorrowLike(Module& module, TypePtr type);

// Fills in the constructors of every instantiation that was created before the declaration it
// came from had been read. Runs once per module after its data declarations are complete.
void completePendingInstances(Module& module);

// Replaces every type variable of one context with the matching entry of `args`. Used to build
// an instantiation's constructors and to specialize a class method's signature.
TypePtr substituteType(Module& module, TypePtr type, Buffer<TypePtr> args, LocationId source);

/*
 * How a match resolves a variable that is already bound to a type other than the one this position
 * carries. Absent - the default - is strict: the two must be one `TypePtr`.
 *
 * **Strict is what *selection* means, and it is not what a *call* means.** An instance head that
 * binds `a` twice binds it to one type, and two spellings of one type are already one pointer. A
 * call's two argument positions may legitimately disagree in ways something later settles: a
 * literal that has not chosen a type yet takes the one the other position wrote, and a `@bits`
 * refinement meets the type it refines at the canonical, which is the load-bearing half of
 * repr.md's *"`@bits(n)` never participates in typeclass dispatch"*.
 *
 * Those rules need an `ExprResolver`, which this file has no business knowing about - so the caller
 * that has one supplies them, and the structural walk stays in one place. It had not been: both
 * rules were written into `bindInto`'s own outermost arm, so they held for `f(x: a, y: a)` and
 * failed for `f(x: Box(a), y: Box(a))` - the same question one constructor deeper.
 *
 * `bound` is updated in place where the two meet at a third type, and the caller discards the
 * binding list if the match then fails elsewhere.
 */
struct MatchRebind {
    bool (*resolve)(void* context, TypePtr& bound, TypePtr concrete) = nullptr;
    void* context = nullptr;

    bool operator()(TypePtr& bound, TypePtr concrete) const {
        return resolve && resolve(context, bound, concrete);
    }
};

// Structural match of a type written against a generic context (`pattern`) with a concrete type,
// binding each type variable it meets in `bindings`. Returns false on a mismatch, including a
// variable that would have to bind to two different types - unless `rebind` says the two meet, for
// which see MatchRebind. This is the whole of instance selection's inference, and call-site
// inference uses the same function with the weaker rule.
bool matchType(GlobalBase global, TypePtr pattern, TypePtr concrete, Buffer<TypePtr> bindings,
               MatchRebind rebind = {});

// Decides how a record is laid out, from the shape of its constructor list alone. This is
// deliberately independent of its type arguments: a generic body has to project into `Maybe(a)`
// the same way every instantiation does, so the declaration decides once and each instantiation
// inherits the answer.
void computeRecordLayout(GlobalBase base, RecordType& record);

/*
 * Automatic indirection: boxes the back edge of every inline-containment cycle reachable from this
 * type, so that a recursive declaration has a layout at all.
 *
 * The one layout question that stays in resolve, and it stays because its answer is a fact about the
 * program rather than about a machine - true of every target at once, and two code generators
 * choosing the edge separately could choose differently. What it produces is `Field::boxed` and
 * `Constructor::boxed`; nothing in the source names either, and neither is part of the type. See the
 * comment on the implementation for where the cut lands and why that is uniform.
 *
 * Asked once per declaration, after every content type in the module is resolved, and once per
 * record instantiation as it completes.
 */
void breakLayoutCycles(Module& module, TypePtr type, LocationId source);

/*
 * Whether this type's inline containment is acyclic, reporting when it is not.
 *
 * The backstop behind breakLayoutCycles rather than the primary check: a cycle surviving the walk
 * above is one nothing knows how to break, and the alternative to reporting it is an unbounded
 * recursion in whichever pass asks for a size next. Both walks agree on what an edge is - a pointer,
 * a borrow, a function value and a boxed field or constructor all have a size independent of what
 * they name.
 */
bool checkTypeAcyclic(Module& module, TypePtr type, LocationId source);

bool sameType(TypePtr lhs, TypePtr rhs);

// Whether two type argument lists are the same one. Interning makes this pointer equality per
// element, which is what instance selection, specialization caching and requirement matching all
// key on - so they all ask it here rather than each writing the loop. The second form compares a
// list where it is stored, without copying it out first.
bool sameTypes(Buffer<TypePtr> lhs, Buffer<TypePtr> rhs);

template<class List, class Base>
inline bool sameTypes(List& list, Base base, Buffer<TypePtr> args) {
    if(list.size() != args.length) return false;

    Size index = 0;
    for(auto type: list.contents(base)) {
        if(!sameType(type, args[index++])) return false;
    }

    return true;
}

/*
 * The ownership classification of a type, computed structurally and cached.
 *
 * `module` is needed only to find the authored instances, and the answer does not depend on which
 * module asked - see Type::ownership. A cycle reachable without an indirection would be an
 * infinitely large value and cannot be constructed; the guard exists so that a declaration which
 * *is* recursive (through a raw pointer, which is never recursed into) still terminates rather than
 * relying on the pointer case being reached first.
 */
Ownership ownershipOf(Module& module, TypePtr type);

/*
 * The classification a body written in `env` may act on, which is not always the structural one.
 *
 * Design-Memory §2.1: "a generic parameter gets copy-on-read only when the signature asks for it.
 * An unconstrained parameter is treated as non-TrivialCopy inside the body regardless of what a
 * caller later substitutes, so a generic function's accepted programs and behaviour are fixed by
 * its own signature." So a type variable answers conservatively *unless* the context declares
 * `TrivialCopy(a)`, and the answer is deliberately not cached on the Type - it belongs to one
 * context rather than to the type.
 *
 * A null `env` is the ordinary non-generic case and is exactly ownershipOf().
 */
Ownership ownershipIn(Module& module, GenEnv* env, TypePtr type);

// Whether the end of this value's lifetime has to run anything at all - either half. Shorthand for
// the question drop insertion asks of every place, which is the one ownership fact most callers
// want.
bool needsTeardown(Module& module, TypePtr type);

// Whether this type has a `Drop` - an effect that runs at last use and is never elided. This is the
// narrower question, and it is the one region eligibility asks: storage whose teardown is entirely
// `Reclaim` may be released in bulk (Design-Memory §4).
bool needsDrop(Module& module, TypePtr type);

bool isUnit(GlobalBase base, TypePtr type);
bool isLiteral(GlobalBase base, TypePtr type);
bool isInteger(GlobalBase base, TypePtr type);
bool isPointer(GlobalBase base, TypePtr type);
bool isBorrow(GlobalBase base, TypePtr type);
bool isFunction(GlobalBase base, TypePtr type);

// What a pointer points at, or null for anything else.
TypePtr pointeeType(GlobalBase base, TypePtr type);
bool isFloat(GlobalBase base, TypePtr type);
bool isNumeric(GlobalBase base, TypePtr type);
bool isGeneric(GlobalBase base, TypePtr type);

// Which of a context's variables `type` mentions, as a bit per index, or'ed into `mask`. Indices at
// or above 64 are not reported; see the definition for why that is safe for its callers.
void genVariablesIn(GlobalBase base, TypePtr type, U64& mask);
/*
 * Whether a value of this type is carried as a copy in a register rather than as an address.
 *
 * Three different questions are asked of this, and they are worth telling apart because only one of
 * them decides what compiles:
 *
 *  - **the ABI**, which is what it literally says, and which every target is bound by rather than
 *    free to disagree with - see the header comment above and checkAbiContract in repr.cpp;
 *  - **the IR shape**, through isMemoryType: whether resolve models a value of this type as a place
 *    or as a value. That is a convention of this IR and nothing more;
 *  - **whether a `return` parameter can root a borrow**, which is the semantic one, and which is
 *    asked through arrivesAsCopy below rather than here.
 *
 * Whether a value that *could* be a register actually gets one at run time is no longer asked here at
 * all - promoteStackSlots in compiler/lower answers that over finished IR, where it is unobservable.
 */
bool isDirectType(GlobalBase base, TypePtr type);
bool isMemoryType(GlobalBase base, TypePtr type);

/*
 * Whether a parameter of this type arrives as a copy of the caller's value rather than as the
 * caller's storage - and so whether a `return` on it has anything to root a borrow in.
 *
 * The one place directness decides what compiles, which is why it is named rather than spelled out at
 * the call site. Design-Memory states the rule over TrivialCopy; this is the same rule one step
 * earlier, since what disqualifies a parameter is arriving as a copy and a TrivialCopy *aggregate*
 * still arrives as the caller's address.
 *
 * A raw pointer is the exception among direct types: the copy it arrives as *is* an address, so what
 * it names is still the caller's.
 *
 * Known gap, recorded here because the compiler cannot yet close it: a newtype over a direct type has
 * that type's representation and not its directness, so `data One {a: Bool}` arrives as an address
 * where `Bool` arrives as a copy, and a `return` on it is accepted where the same `return` on `Bool`
 * is not. That is sound only because resolve's answer is also what the ABI does - which is exactly
 * the contract checkAbiContract exists to keep true. Closing it means a value-producing path for
 * single-field records in expr_construct.cpp and value-based field projection to match, since a
 * direct value has no address for a Downcast to walk.
 */
bool arrivesAsCopy(GlobalBase base, TypePtr type);

/*
 * How wide a value is, and how wide the storage a machine would give it is.
 *
 * The two questions field packing is decided by, and the reason they are here rather than in
 * `compiler/repr` is Design.md's "Packed fields and mutable borrowing": which fields a target *may*
 * co-pack has to be the same answer on every target, because whether a borrow of one needs the
 * materialize/write-back treatment is a source-level fact and a diagnostic that fired on one backend
 * and not the other would not be one.
 *
 * `logical` is zero for a type this does not have an answer for. An integer answers its `@bits` width
 * against the smallest power-of-two byte count that holds it, and an enum-layout record answers the
 * bits its constructor count needs against the discriminant word.
 *
 * An *aggregate* answers where the whole of it fits in fewer bits than its own storage - a record of
 * two `Bool`s is two bits in a byte - which is what makes a record co-packable into a parent and
 * borrowable as a bit range rather than as an address. That answer is the packed placement below, so
 * `logical` is the same number `compiler/repr` will lay the fields out within, mask width included.
 */
struct ValueWidth {
    U32 logical = 0;
    U32 natural = 0;

    // Narrower than its own storage, and therefore worth co-packing with a neighbour.
    bool isNarrow() const { return logical != 0 && logical < natural; }
};

ValueWidth valueWidth(GlobalBase base, TypePtr type, IntWidths widths);

/*
 * The numbers a payload-free sum's constructors occupy - Analysis-Language.md §5.1.
 *
 * A sum with nothing in it *is* a number, and until `@value` there was one number it could be: the
 * declaration order. Everything that sizes one, packs one, or takes a niche out of one used to read
 * the constructor *count* and derive the range from it. That derivation stops holding the moment a
 * value is pinned - nineteen errno constructors reaching 122 need seven bits and not five - so the
 * range is asked for directly and the count is no longer anybody's proxy for it.
 *
 * Where nothing is pinned the answer is bit-for-bit what the count gave: values run 0 to count-1, so
 * `highest + 1` is the count and every rule below reproduces itself.
 */
struct EnumRange {
    I64 lowest = 0;
    I64 highest = 0;

    // What holds every value. A logical width for the ordinary case, so that a three-constructor
    // enum is still two bits and still co-packs with its neighbours.
    U32 bits = 1;

    /*
     * A value below zero, which takes the type out of two decisions rather than changing them.
     *
     * Packing and niching both describe a value as a range of *patterns* counted from zero: a niche
     * is what is left above `validEnd`, and a co-packed field is a bit range read back by masking.
     * Neither statement is true of a number whose top bit is its sign, and making them true is a
     * representation change rather than an attribute. So a sum with a negative value takes a whole
     * signed word of its natural width, packs with nothing and offers no niche - which is what the
     * ABI that asked for `-1` was going to insist on anyway.
     */
    bool signedValues = false;
};

EnumRange enumRange(GlobalBase base, RecordType& record);

// The storage a machine gives an integer of `bits` logical width: the smallest power-of-two byte
// count that holds it, in bits. Repr's own `naturalBytes` is this divided by eight, and reads it
// from here so that the two sides of the packing contract cannot drift apart.
U32 naturalStorageBits(U32 bits);

/*
 * Whether a target may co-pack field `index` of this tuple with another.
 *
 * Narrow, *and* sharing the tuple with something else narrow. The second half is what keeps a record
 * with one `Bool` in it addressable: co-packing a lone field with nothing saves no space and would
 * cost every borrow of it a temporary, so a field only gives up its address where the declaration is
 * actually asking to be packed.
 *
 * "Sharing the tuple with" rather than "written next to", because an `Auto` layout is free to reorder
 * the fields and does, so a narrow field's neighbours are decided by the target rather than by the
 * declaration - `{a: Bool, b: U64, c: Bool}` packs `a` with `c`. Under `C` the order *is* the
 * declaration's, so there the neighbours are the written ones, exactly as a C bit-field's are.
 *
 * The contract with `compiler/repr` runs one way. A target may pack fewer fields than this names -
 * it may decline entirely, as JS does - and may never pack one this does not name, because resolve
 * has already decided, on the strength of this answer, which borrows needed rewriting and which
 * fields have no address to hand to `addressOf`.
 */
bool packCandidate(GlobalBase base, TupType& tuple, U16 index);

/*
 * The widest word a run of co-packed fields may occupy, as the *language* rather than as a target.
 *
 * A target may be narrower than this and JS is (53 bits, the point at which a host `number` stops
 * counting), and the one-way contract above is what makes that safe: a narrower target packs fewer
 * runs and scalarizes fewer records. What no target may be is *wider*, because `valueWidth` is
 * answered without knowing which one is emitting and a record it called scalar has to stay scalar.
 *
 * Raising this is how a target with wider registers than its integers - SSE, where the unit is 128
 * bits - would come to pack into them. Two things have to move with it and neither is here: the
 * lowered read-modify-write computes in a 64-bit value (see decodePackedField in resolve/lower.cpp),
 * and `naturalStorageBits` stops at 64. So this is the *budget*, and widening it is a decision about
 * layout that a wider access path then has to be able to serve.
 */
static const U32 kMaxPackBits = 64;

/*
 * Where a run of co-packable fields sits inside one word.
 *
 * `span` is the total bits the run occupies - including any gap the straddle rule left behind, since
 * what a container needs to know is how wide the word has to be rather than how much of it is live.
 * `count` is how many of the fields offered were placed; a run too long for one word stops early and
 * the caller places the rest another way.
 */
struct PackedRun {
    U32 span = 0;
    U16 count = 0;
};

/*
 * The bit placement of a run of narrow fields, and the one rule both stages have to agree on.
 *
 * `order` is the field indices to place, in the order they should be tried - which is not
 * declaration order for an `Auto` layout, see `packOrder`. `offsets`, when given, receives the bit
 * offset of each field placed, in the same order.
 *
 * **No field straddles the natural storage unit of its own width.** That is the C bit-field rule, and
 * it is here for a reason C did not have: a `&` of a packed field is an address plus a shift
 * (Design.md's tier 2), and the *width of the load* that shift applies to has to be recoverable from
 * the field's type alone, because a callee holding one was compiled once and has only the type.
 * Guaranteeing the field sits inside one `naturalStorageBits(bits)` unit is what makes that true: the
 * unit is the load, and the shift is the position within it.
 *
 * Under `C` the unit is the *declared* type's width instead - `@bits(4) Int` is allocated in a 32-bit
 * unit the way `int x: 4` is - because matching a C compiler is the whole content of that layout.
 *
 * This lives in resolve rather than in `compiler/repr` because both stages need the answer and they
 * need the same one: repr lays the fields out at these offsets, and `valueWidth` reports the span as
 * the mask width a callee holding a reference to the whole aggregate will use. Two implementations of
 * it would be two mask widths.
 */
/*
 * The two lists the packing walk works in, both inline.
 *
 * An aggregate with more than sixteen narrow fields is not one this bound is deciding anything
 * about; the point is that the ordinary record - a handful of fields, laid out once per declaration
 * per target - never reaches the heap for either of them. See SmallArray.
 */
using PackOrder = SmallArray<U16, 16>;
using PackOffsets = SmallArray<U32, 16>;

PackedRun packBits(GlobalBase base, TupType& tuple, Buffer<const U16> order, U32 maxBits,
                   PackOffsets* offsets, IntWidths widths);

/*
 * The storage unit a bit-field is allocated in under a pinned layout, or zero for anything that is not
 * one - which is everything but a written `@bits` refinement.
 *
 * `@bits(4) Int` is `int x: 4`, and C allocates that in an `int`: a lone one occupies four bytes and
 * two of them share those four bytes. That is why the number is the *declared* type's width rather
 * than the refinement's, and why matching C needs it in two places - the unit a run is packed in, and
 * the storage a field that shares its unit with nobody still takes up.
 */
U32 declaredUnitBits(GlobalBase base, TypePtr type);

/*
 * The order narrow fields are packed in: widest first, and stable within a width.
 *
 * Widest first because the straddle rule wastes the *tail* of a unit rather than its head, so placing
 * the wide fields while the word is still empty leaves the leftover bits in one run at the top where a
 * niche can use them. `{d: @bits(4), f: @bits(4), a: Bool}` fills a byte with `d` and `f` and starts a
 * second one with `a`, where declaration order would split `f` across the two.
 *
 * Declaration order under `C`, which is what pinning the layout means.
 */
void packOrder(GlobalBase base, TupType& tuple, PackOrder& into, IntWidths widths);

/*
 * Whether the whole of an aggregate is one narrow scalar, and where its fields sit inside it.
 *
 * The layout half of what `valueWidth` reports about the same tuple, so that the target laying the
 * fields out and the callee masking a reference to the whole thing are working from one placement.
 * False where the aggregate has no scalar form, in which case `run` and `offsets` are untouched.
 *
 * `offsets` comes back indexed by *field* - one entry per field of the tuple, in declaration order -
 * rather than in the order the fields were placed, so that a caller needs to know nothing about the
 * ordering to use it.
 */
bool scalarLayout(GlobalBase base, TupType& tuple, PackedRun& run, PackOffsets* offsets,
                  IntWidths widths);

/*
 * Whether a value of this type is narrow enough that a `&` of it carries a shift - Design.md's
 * tier 2, and the thing that makes a packed field borrowable without a temporary at all.
 *
 * Stated over the pointee rather than over where the borrow came from, which is the whole point: a
 * callee declaring `&b: Bool` is compiled once and takes a reference into a bit range whether or not
 * its caller's `Bool` turned out to be packed. The unpacked case passes shift zero and costs a mask
 * the type makes constant.
 *
 * A non-narrow pointee's shift is *provably* zero - a full-width value cannot sit at an offset
 * inside a word of its own width - so `&Int` stays exactly the address it always was, with nothing
 * elided at run time because there was nothing there.
 */
inline bool isNarrowValue(GlobalBase base, TypePtr type) {
    // No target, for the reason `packCandidate` needs none: narrowness is the one width question an
    // abstract type answers the same way everywhere, since `Size` and `CodeUnit` fill their storage
    // at every width they can take. The proof is written out there.
    return valueWidth(base, type, IntWidths {}).isNarrow();
}

// How a type is written in a diagnostic or in printed IR. The builder form is the one that
// composes; the String form allocates a copy for a diagnostic argument.
void describeType(Context& context, GlobalBase base, TypePtr type, StringBuilder& target);
String describeType(Context& context, GlobalBase base, TypePtr type);

// A comma-separated list of types, as an argument list or an instance's types are written. Every
// diagnostic that names more than one type at once goes through this, so they all read alike.
void describeTypes(Context& context, GlobalBase base, Buffer<TypePtr> types, StringBuilder& target);

// The name of something the compiler generated for one type: `drop$Array(Int)`, `typeDesc$Bool`.
// None of them is addressable in source, so all they need is to be unique and to say what they are
// about - which is the prefix and the type, every time.
StringId derivedName(Module& module, StringView prefix, TypePtr type);

// The interned name of a symbol built up in a StringBuilder. Every generated function and table
// ends the same way, and writing it out spells the same three arguments each time.
StringId builtName(Context& context, StringBuilder& text);

/*
 * Whether one variable of a generic context occurs anywhere inside a type, by index rather than by
 * identity - which is how selection binds, so it is how occurrence is asked.
 *
 * Three passes need it and none of them owns it: instance selection, to reject a head over a
 * variable nothing can bind; the derive pass, because what makes a class forwardable is *where* its
 * own variable occurs; and the call path, to tell a variable a call cannot decide from one it need
 * not decide.
 */
bool mentionsVariable(GlobalBase global, TypePtr type, U16 index);

/*
 * A floating-point value as the bits its storage holds, and back.
 *
 * A global's initializer and a field's default are both recorded as one U64 of storage rather than
 * as a number, so that nothing downstream has to convert again - and the conversion is at the *type's*
 * width, since an `F32` field holds four bytes of single precision and not a truncated double. Both
 * directions exist because both are taken: the resolver records the bits, and building the constant
 * that fills the storage reads them back.
 */
U64 floatBits(GlobalBase base, TypePtr type, F64 value);
F64 floatFromBits(GlobalBase base, TypePtr type, U64 bits);
