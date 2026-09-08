#pragma once

#include "body.h"
#include "gen.h"
#include "intrinsic/roles.h"
#include "../lower/convention.h"
#include "../parse/library.h"

namespace ssa { struct Body; }

namespace mir {

struct Class;

struct Module;
struct Program;
struct GenEnvironment;

/*
 * The declaration region - Analysis-IR.md §8.1.
 *
 * Everything here lives for the whole compilation, and everything a *call* names is here. That is
 * the rule the two body arenas depend on: an SSA node may name a `Function`, never another body's
 * node, so the MIR arena can be released the moment conversion finishes and nothing has to be
 * scanned for stale handles first (§8.8).
 *
 * **Nothing placement-allocated here may own heap memory.** A bump arena is released whole and runs
 * no destructor, so a `HashMap` or an `Array` held by an arena-allocated struct leaks its buffer
 * once per instance and once per compilation. `Types` states the rule from the other side - it is
 * held by value on `Program` precisely because it needs a hash map - and everything below obeys it:
 * the dense lists are `RegionArray`, and the by-name indexes belong to `Program`, which is a real
 * object with a real destructor.
 */

/*
 * How many positions one packed `U16` domain may number - the origin slots of one scope, the
 * variables and frozen slots of one generic context, the constructors and fields of one record, the
 * parameters of one signature.
 *
 * `0xffff` is "none" in four of those domains (`kNoOriginSlot`, `SlotScope::kNone`,
 * `Function::kNoContinuation`, `InstFieldAddress::kNoProperty`) and `0xfffe` is the SSA
 * discriminant field (`InstFieldAddress::kDiscriminant`), so a count that reached either wrapped an
 * identity into a reserved value with nothing said: a generated declaration wide enough had a field
 * that read as the discriminant. The representation stays `U16` - the compactness is the point of
 * every one of these tables - and what every producing scope does instead is check this one number
 * and report before it converts. `Resolver::withinPacked` is the check; `freezeSchema` makes the
 * same one for the slot list it derives, since that list sums four of the others.
 */
static constexpr U32 kMaxPackedPositions = 0xfff0;

/*
 * What the compiler puts in a `@caller` position the call site left out - Design-Test.md §11.1's F2.
 *
 * `Site` is the call's own location; `Source` is the *text* of the expression that reached some
 * other parameter. Both are constants built at the **call's** source rather than at the
 * declaration's, which is the whole of what separates this from an ordinary default - and the only
 * thing it needs from the rest of the resolver, since "a position no argument reached is filled by
 * its default" is already how a default works.
 */
enum class CallerFill: U8 {
    None,
    Site,
    Source,
};

/*
 * The marker as a declaration carries it, once `declareCallerFill` has accepted it.
 *
 * A pair rather than two fields of `Arg` because the second is meaningful for one of the fills
 * only: `source` is the *parameter index* whose written expression a `Source` fill is the text of,
 * and says nothing for a `Site`. Set only where the fill was checked and found buildable - a marker
 * that failed leaves `None`, since a position every call may leave out and nothing fills is worse
 * than the diagnostic that refused it.
 */
struct CallerInfo {
    CallerFill fill = CallerFill::None;
    U16 source = 0;

    bool isCallerFilled() const { return fill != CallerFill::None; }
};

/*
 * One argument of a signature.
 *
 * The occurrence is the whole of the calling convention at this level: `->x: T` is owned, `x: T` is
 * shared and call-bounded, `&x: T` is unique and call-bounded, and `x: a'T` is shared in published
 * slot `a`. A call records the mode on its operand from *this*, once, when the signature has been
 * selected - which is what makes a direct, class, dynamic and specialized call have the same local
 * representation (§5.6).
 */
struct Arg {
    /*
     * The one eight-byte-aligned member leads and the one-byte flag trails, which is what makes
     * this forty-eight bytes rather than fifty-six: laid out in the order the fields were argued
     * in, `lazy` cost seven bytes of padding before `def` and `defThunk` four after it.
     */
    /*
     * `fn open(path: String, mode: Mode = Mode.Read)` - what a call that leaves this position out
     * passes, and `Constant::None` where there is none.
     *
     * The same field a record's `RecordField::def` is, carrying the same thing for the same reason,
     * and `doc/spec/functions.md` says so outright: a default argument is subject to the constraints
     * a field default is. What it adds here is the *lookup* - `requiredArity` counts the positions
     * that have none, and a declaration answers every call arity from that count up to
     * `writtenArity` - which is the whole of what a caller has to know.
     *
     * Three positions may not carry one and `declareArgs` refuses each, every time for a reason
     * about the caller rather than about the constant. `@lazy` is deliberately not among them - see
     * `defThunk` below, which is what a call that omits one passes.
     */
    Constant def;

    StringId name {};
    Occurrence occurrence;
    LocationId source = kNullLocation;
    U32 index = 0;

    /*
     * The nullary function a call that omits a **deferred** default passes - `Resolver::defaultThunk`.
     *
     * A `@lazy` position holds `() -> T`, so what fills an omitted one is not the constant but a
     * thunk answering it. Its body is `return <constant>`, which is the whole of why the marker and
     * a default get along: there is no expression to run at the wrong moment, nothing to observe if
     * the callee never forces it, and no evaluation order for it to be in.
     *
     * Cached on the parameter rather than on the constant, because the parameter is what has one:
     * the alternative is a program-wide table keyed by the constant's bits, which buys sharing
     * between two unrelated declarations that happen to default to the same number and costs a hash
     * of a value that is otherwise never a key. Built on first use rather than at the declaration,
     * because a body may not exist until the body phase - see `resolveProgram`'s order.
     */
    DeclPtr<Function> defThunk = nullptr;

    /*
     * `@lazy` - `doc/spec/functions.md`'s Deferred arguments.
     *
     * The parameter is not evaluated at the call site: `occurrence` is a shared, call-bounded borrow
     * of `() -> T` where `T` is what the author wrote, and reading the name in the body is a call of
     * it. So this flag is not a second description of the position - the type already says what the
     * callee holds - it is the one bit that says the *caller* writes a `T` there rather than a thunk,
     * which is what `resolveArguments` needs and what a reader of the signature needs.
     *
     * Borrowed and call-bounded for `declareContinuation`'s reason, which is the same one: the thunk
     * closes over the caller's frame, so `checkCarriedOrigins` has to refuse it at every owned
     * position, and a borrow granted for the call is exactly the extent the spec gives it.
     */
    bool lazy = false;

    /*
     * `@caller` - Design-Test.md §11.1's F2, checked by `Resolver::declareCallerFill`.
     *
     * Beside `def` rather than folded into it, because the two answer the same question about a
     * call - what fills a position it left out - from different places: a default is one constant
     * written at the declaration, and this is a constant built per call site. Everything that only
     * asks *whether* the position may be left out reads `omittable()` and sees no difference, which
     * is what keeps arity, named arguments and overload selection unchanged.
     */
    CallerInfo caller;

    // Whether a call may leave this position out. A default and a `@caller` fill are the same answer
    // to that question, and every arity check asks it here rather than at the two fields.
    bool omittable() const { return def.isSet() || caller.isCallerFilled(); }
};

/*
 * How much is known about one published fact - Analysis-Sharing.md §4.
 *
 * One flag for the whole summary was wrong, and the way it was wrong is the thing separate
 * compilation makes expensive: "the body is unavailable" and "this particular fact is unavailable"
 * are different statements. A foreign declaration has no body and a perfectly good published
 * suspension answer; an imported function has a body somewhere and no *computed* retention until
 * its module is read; a recursive group has facts that are provisional until the fixpoint settles.
 * A consumer that had one flag to check had to treat all three as the worst of them.
 *
 *  - `Unknown` is no answer. Every consumer falls back to its own conservative default.
 *  - `Assumed` is a fixpoint's provisional answer, and is not yet safe to publish or to cache.
 *  - `Known` is settled, and is what a module image carries.
 */
enum class Knowledge: U8 {
    Unknown,
    Assumed,
    Known,
};

/*
 * One published fact and how much is known about it - the *reading* vocabulary.
 *
 * The pair rather than a value beside a shared flag, so that a fact cannot be read without its
 * confidence: `fact.value` on its own does not compile into anything meaningful, and `fact.get(d)`
 * makes the caller name what it falls back to.
 *
 * Handed out **by value** by `Summary`'s accessors rather than stored as a field of one. Stored, it
 * costs seven bytes of padding for every fact whose value is a `U64` - and four of the six are - so
 * a `Summary` spent thirty-two of its eighty bytes on six two-bit numbers. The states now live in
 * one word of the summary and this is what a reader is handed when it asks; a `set` on a temporary
 * would write nowhere, so the write half is a named setter per fact instead.
 */
template<class T>
struct Fact {
    T value {};
    Knowledge knowledge = Knowledge::Unknown;

    bool isKnown() const { return knowledge == Knowledge::Known; }

    // The fact, or the caller's conservative answer where there is none. `Assumed` counts as an
    // answer here: a fixpoint reads its own provisional values, and what it may not do is publish
    // them, which is `isKnown`.
    T get(T fallback) const { return knowledge == Knowledge::Unknown ? fallback : value; }
};

/*
 * A set of a function's parameters, in a word - the lattice a per-argument fact joins over.
 *
 * `retainsArguments: bool` was the shape this replaces, and what it lost is the only thing a caller
 * wanted to know: *which* argument. A caller with one frame-placed argument and three registers had
 * to treat all four as retained, so the fact answered "no" usefully and "yes" not at all - which
 * makes it a fact nobody can act on, and an unusable fact is how a consumer ends up recomputing its
 * own.
 *
 * Parameters at index 63 and beyond share the top bit. That is an over-approximation and is the safe
 * direction for a *may* set: a query for parameter 70 reads the bit that parameter 64 set, and both
 * answer true when either is in. Sixty-three exact bits is well past the arity anything has, and the
 * alternative - a variable-length set in the declaration region - buys precision for a case that
 * does not occur at the cost of storage a summary would then have to own.
 */
struct ArgumentSet {
    static constexpr U32 kLast = 63;

    U64 bits = 0;

    static U64 bit(U32 index) { return U64(1) << (index < kLast ? index : kLast); }

    bool has(U32 index) const { return (bits & bit(index)) != 0; }
    bool any() const { return bits != 0; }

    void add(U32 index) { bits |= bit(index); }

    // Every parameter, which is what an unknown callee contributes. The join is a union, so this is
    // the top of the lattice and joining anything with it is itself.
    static ArgumentSet all() { return ArgumentSet { ~U64(0) }; }

    ArgumentSet join(ArgumentSet other) const { return ArgumentSet { bits | other.bits }; }

    bool operator == (ArgumentSet other) const { return bits == other.bits; }
    bool operator != (ArgumentSet other) const { return bits != other.bits; }
};

/*
 * How much frame an activation of this function needs, including everything it calls.
 *
 * `boundedStack: bool` was the shape this replaces, and it could not publish the one thing an
 * external iterator needs: the number. A resumable frame has to be *allocated* by whoever starts it,
 * and "bounded" without a bound tells the allocator nothing - so a consumer with a bool would have
 * gone and computed the bytes itself, which is the second answer the summary exists to prevent.
 *
 * `kUnbounded` is the top of the lattice and is what recursion, an indirect call and a separately
 * compiled callee all give. Joining is a saturating maximum: a caller needs the largest of its
 * callees' frames, not their sum, because they are not live at the same time - a *chain* is
 * accounted for by each callee having already added its own callees in.
 */
struct StackBound {
    static constexpr U64 kUnbounded = ~U64(0);

    U64 bytes = 0;

    bool isBounded() const { return bytes != kUnbounded; }

    static StackBound unbounded() { return StackBound { kUnbounded }; }

    StackBound join(StackBound other) const {
        if(!isBounded() || !other.isBounded()) return unbounded();
        return StackBound { bytes > other.bytes ? bytes : other.bytes };
    }

    // This function's own frame on top of what it calls. Saturating, so that a bound and an unknown
    // callee give an unknown rather than a wrapped-around small number.
    StackBound plus(U64 own) const {
        if(!isBounded()) return unbounded();
        return bytes + own < bytes ? unbounded() : StackBound { bytes + own };
    }

    bool operator == (StackBound other) const { return bytes == other.bytes; }
    bool operator != (StackBound other) const { return bytes != other.bytes; }
};

/*
 * How much of a body is work on its **generic environment** - the term the form of a generic is
 * chosen on, and the reason it is a fact rather than a number a pass keeps to itself.
 *
 * A *share* and not a count. What specializing buys is the environment reads turning into nothing,
 * and what it costs is one copy of the whole body per argument list; so the question is what
 * fraction of the body those reads are. Two hundred instructions with three of them indirect is
 * nearly the same code either way and should exist once. Four instructions that are all descriptor
 * work is the opposite, and is also the body a caller most wants inlined.
 *
 * Out of `kWhole` rather than as a pair of counts, for the reason `Knowledge` is two bits: this is a
 * summary, the two counts are worth nothing to a consumer separately, and a byte keeps the whole
 * structure in the forty the assertion below holds it to. A body that touches the environment at all
 * is never zero - a share that rounds to nothing is still not nothing - which is what makes
 * `share == 0` mean *no environment work* and not *not much*.
 *
 * The join is a **maximum**, and it is what the interprocedural term will be when there is one: a
 * body that hands its environment to another generic is at least as much environment work as the
 * body it hands it to. Nothing composes over it today because an erased body cannot forward an
 * environment at all - see `checkErasable` - so every measurement is complete on its own and is
 * published `Known`.
 */
struct GenCost {
    static constexpr U32 kWhole = 256;

    U8 share = 0;

    static GenCost measured(U32 environment, U32 total) {
        if(!environment || !total) return GenCost { 0 };

        auto share = (U64(environment) * kWhole) / total;
        return GenCost { U8(share < 1 ? 1 : (share > kWhole - 1 ? kWhole - 1 : share)) };
    }

    GenCost join(GenCost other) const { return share > other.share ? *this : other; }

    bool operator == (GenCost other) const { return share == other.share; }
    bool operator != (GenCost other) const { return share != other.share; }
};

/*
 * What one function published about itself - Analysis-Sharing.md §4.
 *
 * One structure and one fixpoint, deliberately: storage class, mutation demand, argument retention,
 * linear consumption, the instantiation set and suspension all give up at the same boundary - an
 * indirect call, a separately compiled callee - and each having its own lattice and its own
 * conservative fallback is how the current compiler acquired six of them that disagree about where
 * that boundary is.
 *
 * What is *not* shared is how much is known, which is per fact - see `Knowledge`. `bodyKnown` is
 * still here and still first, because it answers a different question from all of them: whether
 * there is a body to analyse at all.
 *
 * Every fact here is computed. Suspension, placement, the three per-argument sets, the stack bound
 * and divergence are `computeSummaries`' one worklist over the reversed call graph; the generic cost
 * is `selectGenForms`', for the pass-order reason its accessor gives. That they are one structure
 * filled by one pass is the whole of §4's point - the arrangement it is a report on is six passes
 * each with a private answer and its own idea of where the boundary is.
 */
struct Summary {
    /*
     * Which two-bit field of `states` each fact's `Knowledge` lives in.
     *
     * An enumerator rather than six constants, so that adding a fact is one row and the shift is
     * never written at a use site. `Count` times two bits is what has to fit in `states`, and the
     * assertion below is what says it does rather than a comment claiming it.
     */
    enum class Which: U8 {
        Suspension,
        Stack,
        Retains,
        Escapes,
        Mutates,
        Consumes,
        Placement,
        Diverges,
        GenCost,

        Count,
    };

    // Whether calling this may suspend the enclosing fiber. Inferred and published rather than
    // written: Analysis-Sharing.md §3.3 reverses the colouring decision, and the across-suspend rule
    // needs the answer on native whether or not JavaScript does.
    Fact<bool> suspension() const { return read(suspends, Which::Suspension); }
    void setSuspension(bool value, Knowledge how) { suspends = value; write(Which::Suspension, how); }

    /*
     * How much frame an activation needs, itself and everything it calls - `StackBound`. What a
     * stack depth check, a region reservation and an external iterator's child stack all need, and
     * what recursion is the boundary of.
     *
     * **The bound is `isBounded()` and the bytes are zero**, which is Analysis-External-Iterators
     * §4.5's own split rather than a gap: "the resolver can validate that the call graph is bounded,
     * but it cannot know the exact bytes", because spill slots, the outgoing argument area and saved
     * registers are decided after register allocation. Its stage 2 is the backend's, and a byte
     * count invented here would be one an iterator's child stack got reserved from.
     */
    Fact<StackBound> stack() const { return read(stackBound, Which::Stack); }
    void setStack(StackBound value, Knowledge how) { stackBound = value; write(Which::Stack, how); }

    // Which arguments outlive the call - the retention half of the escape analysis, and what decides
    // whether a caller may hand over a frame-placed value *at that position*. Read by
    // `ssa::Contained`, which admits a call argument at a position nothing here names.
    Fact<ArgumentSet> retains() const { return read(retained, Which::Retains); }
    void setRetains(ArgumentSet value, Knowledge how) { retained = value; write(Which::Retains, how); }

    /*
     * Which arguments' **storage address** may still be named after the call - the second half of the
     * retention question, and the one `ssa::Contained` asks.
     *
     * `retains` is a reach set over *values*: `growArray(&self, n)` stores a run pointer it read out
     * of `self` back into `self`, so the walk joins the written value's parameter reach and argument
     * 0 reads as retained. That is the right answer for the loan checker, which is asking whether
     * anything the caller can still see was computed from what it lent; it is the wrong answer for
     * containment, which is asking whether the *address the caller handed over* was written anywhere.
     * A pointer loaded out of the storage is not that address - and it cannot be, because an
     * allocation containment admits is one whose address was never stored, so nothing the callee
     * loads can be it.
     *
     * So this row follows only chains that never pass through a load of the caller's bytes: an
     * address taken of the parameter's place or of a projection of it, the incoming pointer of a
     * `%T` parameter, and a place handed on at another call's argument position. It is therefore a
     * subset of `retains` in every body the two walks agree about, and both are computed in the one
     * walk `computeArgumentFacts` already makes.
     */
    Fact<ArgumentSet> escapes() const { return read(escaped, Which::Escapes); }
    void setEscapes(ArgumentSet value, Knowledge how) { escaped = value; write(Which::Escapes, how); }

    // Which arguments the callee writes through. Read by the overlap check, which may otherwise not
    // distinguish two `&` arguments where only one is written, and by `ssa::mayWriteStorage`, where
    // a call through a `&` parameter nothing writes stops being a barrier for a forwarded load.
    Fact<ArgumentSet> mutates() const { return read(mutated, Which::Mutates); }
    void setMutates(ArgumentSet value, Knowledge how) { mutated = value; write(Which::Mutates, how); }

    /*
     * Which arguments the callee discharges - the linear-consumption fact. An owned argument a
     * callee consumes is one the caller may not drop, and one it does *not* consume on some path is
     * a leak the caller cannot see; per argument for the reason the two above are.
     *
     * The one row here that a body cannot disagree with its signature about, and that is a fact
     * about this language rather than a shortcut: `checkLinear` refuses a body where an owned
     * parameter is not discharged on every path, so the owned positions *are* the answer and the
     * call-graph join contributes nothing. Published `Known` off one read of the declaration; a
     * function with no body publishes the top of the lattice, which is the direction that keeps a
     * caller from dropping something twice.
     */
    Fact<ArgumentSet> consumes() const { return read(consumed, Which::Consumes); }
    void setConsumes(ArgumentSet value, Knowledge how) { consumed = value; write(Which::Consumes, how); }

    // The strongest placement any object this activation needs - see `StorageClass`. Transitive over
    // the call graph, because that is what a caller asks: whether anything reached from here will
    // want the allocator. `computePlacement` is the fixpoint, over what `analyze_escape.cpp` decided.
    Fact<StorageClass> placement() const { return read(placed, Which::Placement); }
    void setPlacement(StorageClass value, Knowledge how) { placed = value; write(Which::Placement, how); }

    /*
     * Whether calling this ever comes back - the divergence fact, and the shipping stage's declared
     * `Function::noReturn` **inferred** instead.
     *
     * Inferred rather than written for the reason suspension is: the seeds are the roles the library
     * claims (`checkFailed`, `abort`), and everything above them - a body whose every path ends in
     * one, a loop with no exit - is a CFG answer nobody should have to restate in a signature. A
     * declaration is also the wrong place for it: `endNonReturningBlocks` wants the answer for every
     * function on the way to a check's abort arm, and an attribute would only ever be on the first.
     *
     * False is the conservative answer, which is what makes an unknown callee safe: a caller that
     * believes a diverging callee returns keeps a block it did not have to, and one that believes a
     * returning callee diverges deletes the code after it.
     */
    Fact<bool> diverges() const { return read(nonReturning, Which::Diverges); }
    void setDiverges(bool value, Knowledge how) { nonReturning = value; write(Which::Diverges, how); }

    /*
     * How much of this body is work on a generic environment - `GenCost`, and what `selectGenForms`
     * decides the form of a generic on.
     *
     * **The one fact here that `computeSummaries` does not compute**, and the exception is the pass
     * order rather than an oversight: which form a generic is compiled in is what decides whether a
     * call makes a specialization at all, so it is settled *before* the walk that builds the call
     * graph - and this is that decision's input. It is published here anyway because it is a fact
     * about a function that more than one pass wants: the inliner is the next reader, and a body
     * that is mostly descriptor work is a body inlining is worth more on.
     *
     * Measured over one body in isolation, which is currently the whole answer - see `GenCost`.
     */
    Fact<GenCost> genCost() const { return read(genCosted, Which::GenCost); }
    void setGenCost(GenCost value, Knowledge how) { genCosted = value; write(Which::GenCost, how); }

    Knowledge knowledgeOf(Which which) const {
        return Knowledge((states >> (U32(which) * 2)) & 3);
    }

    /*
     * **The instantiation set is deliberately not a field yet**, and this is what it is waiting for.
     *
     * Every fact above is a lattice element that fits in a word, which is what lets a `Summary` live
     * in the declaration arena beside declarations that may own no heap memory. An instantiation set
     * is a *set of targets* - a function and the type arguments it was reached with - and it needs
     * storage the summary would have to own, in a region, keyed by an identity that does not exist:
     * what a target is is decided by the substitution a specialization is read through, and a shape
     * chosen without it in hand is a shape the substitution then has to be bent to fit.
     *
     * So the commitment recorded here is where it goes rather than a placeholder for it: one more
     * accessor pair on this structure and one more `Which`, joined by the same fixpoint, with a
     * `RegionArray` in the declaration region behind it. What it must not become is a table beside
     * the summary that a second pass fills, which is the arrangement §4 is a report on.
     */

private:
    template<class T> Fact<T> read(T value, Which which) const {
        return Fact<T> { value, knowledgeOf(which) };
    }

    void write(Which which, Knowledge how) {
        auto shift = U32(which) * 2;
        states = U32((states & ~(U32(3) << shift)) | (U32(how) << shift));
    }

    /*
     * The values, largest first, with the confidence for all six in one word behind them.
     *
     * The arrangement this replaces was six `{value, Knowledge}` pairs, and a `Knowledge` after a
     * `U64` is a byte plus seven of padding: four such facts spent sixty-four bytes carrying
     * thirty-two of payload and four two-bit numbers. Two bits each in a `U16` is the same six
     * answers in one field, and it is private for the reason the accessors exist - a caller that
     * could reach the value without the confidence is the thing `Fact` was introduced to prevent.
     *
     * A `U32` since J37's ninth row, which is what the assertion below turned into rather than a
     * comment claiming a limit: sixteen bits held eight facts exactly, and it costs nothing here
     * because the two bytes come out of the padding the five single-byte values below already leave.
     */
    StackBound stackBound;
    ArgumentSet retained;
    ArgumentSet escaped;
    ArgumentSet mutated;
    ArgumentSet consumed;
    U32 states = 0;
    bool suspends = false;
    bool nonReturning = false;
    StorageClass placed = StorageClass::Frame;
    GenCost genCosted;

public:
    /*
     * Whether the body is available to analyse - false for an unresolved import, a dynamic target or
     * a foreign declaration. Distinct from every fact above: a foreign declaration has no body and
     * may still publish a settled suspension answer. A flag rather than a `Fact`, because it is not
     * one: there is no fallback a consumer could name for "is there a body".
     *
     * Last rather than first, which is where it used to be and where it reads better. First, it is a
     * byte in front of a `U64` and costs seven of padding; here it shares the tail with the two
     * bytes of confidences and the two other single-byte values, and the assertion below is what
     * holds it there.
     */
    bool bodyKnown = false;
};

static_assert(U32(Summary::Which::Count) * 2 <= 32, "`Summary::states` holds two bits per fact");

/*
 * Fifty-six bytes: five eight-byte lattice values, the nine confidences in four, and five single
 * bytes, the last nine of which round up to sixteen. Eighty was the size before the confidences were
 * gathered, and that difference was entirely padding - `Knowledge` is one byte and followed a `U64`
 * four times.
 *
 * Forty until J37, and `escapes` is what the note here predicted would end it: a fact whose value is
 * a `U64` adds eight. It cost eight more than that, and the second eight is worth stating because it
 * is the shape of the next one too. Eight facts fitted `states` exactly at two bits each, so the
 * ninth took it from a `U16` to a `U32` - and the five single bytes below it were living in the
 * padding the `U16` left, so the two extra bytes of `states` push the tail from eight bytes to
 * sixteen rather than costing two. A tenth and an eleventh fact are free again at either width;
 * `GenCost` is a byte precisely so that adding it cost nothing and `Diverges` is the bool that took
 * the next byte of the same tail, and those are still the cheap shapes.
 */
static_assert(sizeof(Summary) == 56, "the layout note above says where the fifty-six bytes go");

/*
 * The two constructors of the wrapper a *skipping* traversal answers with - `declareOutcome`.
 *
 * Named here rather than in the function that builds the record, because the conversion is what
 * reads them back: the resolver states which constructor a path finishes with and `convertSkip`
 * tests for it, and a discriminant written in one file and compared in another is exactly the pair
 * that must not be two literals. `Proceed` first, so that "the continuation ran" is case zero.
 */
static constexpr U16 kOutcomeProceed = 0;
static constexpr U16 kOutcomeExit = 1;

/*
 * Which member of `Try` and of `Rewrap` each of the three operations is - `known.def`'s `carrier`
 * and `rewrap` rows, whose `members` column is the count these index into.
 *
 * By **position**, which is the same dependency every other class role here is under and is stated
 * once at `provideMember`: an emitter reads `Ord`'s `compare` as member 4 and this reads `Try`'s
 * `fromExit` as member 1, so reordering a class in the library breaks it unless the count check
 * catches it. The order is `lib/Core/Core.yana`'s, and it is the order the operators read them in:
 * ask which path a value is on, then rebuild.
 */
static constexpr U16 kTryToOutcome = 0;
static constexpr U16 kTryFromExit = 1;
static constexpr U16 kRewrapRewrap = 0;

/*
 * The ownership classes the compiler itself reaches for are `reclaim`, `drop` and `copy` in
 * `compiler/intrinsic/known.def`, and they are reached through `Program::roles`.
 *
 * They used to be an enum here with their names written beside it, recognized by comparing a
 * declared class's name against those spellings. What replaced it is the attribute: a class says
 * which role it fills, and the compiler holds no library names at all - `intrinsic/roles.h` is the
 * argument for that, and it applies to every other declaration the compiler is fastened to as well,
 * which is why the mechanism is one table rather than three enumerators.
 *
 * `Presence::Synthesized` on those three rows is what keeps a program with no standard library
 * compilable: where nothing claims the role the compiler makes the class, and what it makes and what
 * a library declares are one class to everything below.
 */

/*
 * The attributes this stage reads off a declaration - `doc/spec/declarations.md`'s table, restricted
 * to the rows that mean something here.
 *
 * An enumerator rather than a spelling compared where it is wanted, on a compiler role's terms and for
 * the same reason: **the name is written once**, in `attributeName`, so a rename is one edit and a
 * misspelling in one of the several places that mention `@explicitDrop` is not an attribute silently
 * read as absent. That failure is the one this stage is least able to catch - an attribute is a
 * *removal*, so reading one as absent gives the type back a capability the author took away, and
 * nothing downstream is in a position to notice.
 *
 * Which declarations accept which is deliberately **not** a column here. It is the destination a
 * caller passes to `readAttributes`, because the caller is what has a field to put the answer in: a
 * table that also listed the placements would be the same fact written twice, and the two would
 * disagree the first time a placement was added.
 */
enum class DeclAttribute: U8 {
    ExplicitDrop,
    Local,

    /*
     * `@inline`, `@inline(always)` and `@noinline` on a function - `Function::alwaysInline` and
     * `Function::noInline`.
     *
     * Two rows because they are two spellings, and one reader because they are one question: what a
     * declaration says about being expanded at its call sites. `readInlineAttribute` is that reader,
     * and it is the one attribute walk whose argument is a word rather than a number.
     */
    Inline,
    NoInline,

    // `@specialize` and `@generic` - which form a generic body is compiled in. Rows here rather than
    // a reader of their own so that the spelling is written once, and so that writing one on a
    // `data` reports the placement message every other misplaced attribute gets.
    Specialize,
    Generic,

    /*
     * `@value(n)` on a constructor - `doc/spec/declarations.md`'s pinned constructor values.
     *
     * A row here rather than a reader of its own so that the spelling is written once, and so that
     * writing it on a `data` head or on a `fn` gets the placement message every other misplaced
     * attribute gets. It is the one attribute this stage reads that takes an *argument*, which is
     * why `readAttributes` cannot be its reader: the destination is a number rather than a flag.
     */
    Value,

    /*
     * `@builtin(role)` - which of the compiler's roles this declaration fills.
     *
     * A row here so that the spelling is written once, and so that this stage's attribute walks do
     * not report it as unknown. It is **not** read by them: it is not a flag, its argument is a role
     * from `compiler/intrinsic`, and it can be written on a class, a `data`, a signature or a `let`.
     * `bindRole` in `intrinsic/link.cpp` is its one reader, and every declaration form calls it -
     * including the forms that can fill no role, which is what makes a misplaced one a diagnostic.
     */
    Builtin,

    /*
     * `@bits(n)` on a **type** - `doc/spec/repr.md`'s bit-width refinements.
     *
     * A row here so that the spelling is written once and so that a misplaced one - `@bits(4) fn` -
     * gets the placement message every other attribute gets. It is read by `resolveType`, which is
     * the only walk that sees the position it belongs in: it refines the type it is written on, and
     * a declaration's attribute walk has no type in hand at all.
     */
    Bits,

    /*
     * `@box` on a **field** - `doc/spec/repr.md`'s per-field indirection.
     *
     * Written in the same position `@bits` is and read by the same walk, because the two are the
     * same shape of thing: a declaration-site annotation that changes a field's physical
     * representation and that generic code sees straight through. What they differ in is the axis.
     * `@bits` narrows a width and produces a distinct *type*, so it reaches `resolveType`; `@box`
     * moves the storage out of line and produces a distinct *field*, so it never does and the field
     * keeps whatever type was written after it - see `RecordField::boxed`.
     *
     * A row here for the spelling and for the placement message. `readBoxAttribute` is the reader,
     * and `declareFields` is its one caller: written anywhere a field is not, the attribute is
     * refused rather than silently ignored.
     */
    Box,

    /*
     * `@capacity(n)` on a container type, which is only ever written beside `@inline(n)` -
     * Implementation-Containers.md §7.1's one built row.
     *
     * A row here for the spelling and for the placement message; `readContainerRefinement` is the
     * reader, and it reads the pair because the pair is what selects a layout. `@inline` has no row
     * of its own for it, since the function attribute already spells that word and the positions are
     * disjoint - a type's attributes and a declaration's are read by different walks.
     */
    Capacity,

    /*
     * `@platform(js)`, `@platform(native, x64)` - `doc/spec/declarations.md`'s platform selection.
     *
     * A row here so that the spelling is written once and so that the attribute walks do not report
     * it as unknown. It is **not** read by them, and unlike every other row it is not misplaceable:
     * it restricts *any* declaration, so there is no position at which writing it is a mistake.
     *
     * Its readers are `Resolver::platformEnabled`, which answers whether a declaration is part of
     * this compilation, and `Resolver::checkPlatform`, which is where the attribute's own spelling
     * is judged. The two are split because the first is asked once per pass and the second has to
     * report once - see `eachDeclaration`.
     */
    Platform,

    /*
     * `@allocator(bootstrap)` - Design-Allocator.md §18.3's provider selection.
     *
     * The second clause of the same filter `@platform` is the first of, and here for exactly the
     * reason it is: selection has to **delete** the providers it does not pick rather than pick
     * between claims after reading them all. A role-level choice would leave unused provider code
     * resolved, and would make a half-finished provider a build failure in every program - which is
     * the pair of requirements §18 and §23 state and the pair a pick would fail.
     *
     * The axis is *policy* rather than machine, which is why it does not join `targetSelector`: see
     * `allocatorSelector`, where the difference is written out. Not misplaceable, like `@platform`:
     * it restricts any declaration, so there is no position at which writing it is a mistake.
     */
    Allocator,

    /*
     * `@layout(c)`, `@layout(js)`, `@layout(auto)` - `doc/spec/repr.md`'s pinned layouts.
     *
     * A **declared pin**: the layout is the declaration's to decide and no target may improve on it.
     * That is what makes it an attribute on the `data` rather than a target's choice - a record whose
     * bytes cross a boundary somebody else designed has a layout that is part of its API, and
     * `repr.md` says changing one is an API change.
     *
     * Its argument is a word rather than a number, so `readLayoutAttribute` is its reader on the
     * `@value` pattern and this row is here for the spelling and for the placement message. Both
     * names are meaningful on **both** targets: `@layout(js)` on a native build is declaration order
     * with no packing, and `@layout(c)` on JS is the same statement from the other side - so this is
     * a pin on the type and never a selection of which target's table answers.
     */
    Layout,

    /*
     * `@x86_legacy_sse` - this function's vector instructions are encoded without a VEX prefix.
     *
     * One architecture, one instruction family, one file, and the only backend fact this stage
     * carries: the SHA extension has no VEX spelling in the architecture, so a body holding one of
     * those instructions crosses between prefixed and unprefixed encodings every few instructions
     * unless the whole of it is unprefixed - which `legacyVectorEncodings` in codegen/x64/target.h
     * measured at 140x. The x64 backend *refuses* such a body without the marker, so a stage that
     * dropped the attribute could not compile `lib/Digest/Hardware.sha.yana` at all.
     *
     * Read by `readAttributes` into `Function::legacySse` and carried to
     * `LowerFunction::legacyVectors`, which is the one thing anything below reads. Declared rather
     * than inferred from a pass over the call graph - Analysis-Warts.md §7.
     */
    LegacySse,

    /*
     * `@convention(clobber)` - the calling convention a function is entered under.
     *
     * The second backend fact this stage carries, and it is here for `@x86_legacy_sse`'s reason
     * rather than a new one: what a program may name is a *semantic* promise about a call boundary
     * (`clobber` keeps nothing, which is what makes a vector-state reset at entry legal) or one of
     * the two foreign ABIs, and dropping it silently compiles a body that is entered wrongly.
     * `lower/convention.h` is the one name table, shared with the lower IR's parser and printer.
     *
     * `readConventionAttribute` is its reader - the argument is a word, so `readAttributes` cannot
     * be - and `Function::convention` is the destination, carried to `LowerFunction::callType`.
     */
    Convention,

    /*
     * `@host` on a **field** - `doc/spec/repr.md`'s host-visible property.
     *
     * Written where `@box` is and read by the same walk, and a row here for the spelling and for the
     * placement message. It has no native semantics at all: the field is stored exactly as it would
     * have been, and what reads it is the JS pipeline, where the property is the object's own rather
     * than a slot of the record. So this stage's whole job is to record it and to refuse the
     * spellings that could not mean anything - see `RecordField::host`.
     */
    Host,

    /*
     * `@heap` on a **binding** - `doc/Design.md`'s "forces that one binding onto the general heap".
     *
     * The one attribute this stage reads that is written in front of a statement rather than in
     * front of a declaration, so `readAttributes` is not its reader either: `readBindingAttributes`
     * in `resolve_stmt.cpp` is, and it is the only walk that has a local to place. A row here for
     * the spelling and for the placement message - written on a `fn` or a `data` it is refused like
     * any other misplaced attribute, rather than silently doing nothing.
     */
    Heap,

    Count,
};

StringView attributeName(DeclAttribute attribute);

// The attribute a written name selects, or `Count` where it names none this stage reads. The one
// direction that has to search, since a name arrives from source and an enumerator does not.
DeclAttribute findAttribute(StringView name);

/*
 * Which form a generic body is compiled in - Analysis-IR.md §11.4's milestone 6.
 *
 * **Specialized** is one `Function` per argument list sharing one resolved body, which is what
 * `specialize.cpp` makes: every type is known, so nothing is loaded and nothing is indirect.
 * **Erased** is one body for every argument list, which reads what it needs out of a generic
 * environment built by its caller - `GenSchema` is that environment's layout.
 *
 * A property of the *declaration* and not of a call, which is what makes it decidable at all: the
 * body is resolved once, so the count of sites that would have to consult an environment is the same
 * number whoever asks. A form chosen per call site would be the same body compiled two ways with
 * nothing saying which one a given caller reached.
 *
 * `Auto` is what a signature that wrote neither attribute holds until `selectGenForms` runs, and
 * never afterwards - the pass is what turns the measurement into one of the two answers, and a
 * reader finding `Auto` after it has run is reading a function the pass did not see.
 */
/*
 * Where one call gets the function it calls - `CallSite` in specialize.h, and what `ssa::InstCall`
 * carries down.
 *
 * Here rather than beside `CallSite` because both tiers hold one: MIR resolves the question and SSA
 * carries the answer, and a tier that had to include the whole of specialization to name the answer
 * would be depending on instance selection to describe a call.
 *
 *  - `Declaration` names a function. Every call outside an erased body is one.
 *  - `ClassSlot` loads it out of the class witness the caller supplied, at a member position.
 *  - `FunctionSlot` loads it out of the function slot one `f: (a) -> b` requirement numbers.
 *  - `Value` reads it out of a **function value** the body holds - `NodeCall::target`.
 *
 * The fourth row is what makes a function value a value rather than a fifth kind of call: everything
 * else about the node is unchanged, and where the callee comes from is the question this enum was
 * already the answer to.
 */
enum class CallThrough: U8 {
    Declaration,
    ClassSlot,
    FunctionSlot,
    Value,
};

StringView callThroughName(CallThrough through);

enum class GenForm: U8 {
    Auto,
    Specialized,
    Erased,
};

StringView genFormName(GenForm form);

/*
 * `alias Pair = Two(I32, I32)`, `alias Bytes(a) = Array(a)` - a second name for one type.
 *
 * **Transparent**: an alias is not a type, it is a spelling, so `resolveType` answers with what it
 * names and nothing below this line ever sees one. That is what makes it free - no conversion rule,
 * no coherence question, no second entry in the ownership classification - and it is also what makes
 * it exactly not a `qualified` alias, which *is* a type and needs all three.
 *
 * `gen` is the head's parameters where it has any, and `target` is written in them: `alias Pair(a) =
 * Two(a, a)` is an application waiting for arguments, which `resolveApplication` supplies by
 * substituting rather than by interning a record of its own.
 */
struct Alias {
    Alias(StringId name, DeclPtr<Module> module, LocationId source):
        name(name), module(module), source(source) {}

    StringId name {};
    DeclPtr<Module> module;
    LocationId source = kNullLocation;

    // `pub` - see `exportedSymbol` in `name.h`, which is where the one rule about it lives.
    bool exported = false;

    DeclPtr<GenContext> gen = nullptr;
    DeclPtr<Type> target = nullptr;

    /*
     * Where this alias is in the on-demand walk that resolves the targets - `Resolver::aliasTarget`.
     *
     * An alias may name another, so the targets cannot be resolved in declaration order: `alias A =
     * B` above `alias B = I32` is legal and reads bottom-up. A three-colour walk is what resolves
     * each exactly once and turns `alias A = B` with `alias B = A` into a diagnostic rather than a
     * walk that does not terminate.
     */
    enum class State: U8 { Unread, Reading, Read } state = State::Unread;

    /*
     * Which written declaration this came from - an index into the resolver's own array, not a
     * pointer into the parse region.
     *
     * The target is read on demand and the AST is what it is read from, so something has to carry
     * the connection. An index rather than an `ast::Decl*` because a declaration-arena node holding
     * a raw pointer into another arena is exactly the cross-arena reference this region's rule is
     * about; the index is meaningful only while the resolver is running, which is the only time
     * anything asks.
     */
    U32 written = 0;
};

/*
 * A class declaration - `class Describe(a)`, `class Contiguous(c -> a)`.
 *
 * A class applies to an **argument list**, and every asymmetry between the parameters is written on
 * the head rather than inferred from one: `determined` says which of them the others decide, and
 * `superclasses` says what this class requires of its own arguments. What the head does *not* decide
 * is which instance answers - an instance head is a pattern and `select.h` matches it, so nothing
 * here has to be a type a value has.
 *
 * **The three the compiler itself reaches for are declarations like any other.** `class Drop(a)` is
 * written in the library - `lib/Core/Core.yana` -
 * so `instance Drop(T)` is checked against a class exactly as `instance Describe(T)` is, by one rule
 * rather than by a hand-written shape test per enumerator. `role` is what the ownership classifier
 * keys off, and it is a *tag on a declaration* rather than a second kind of class.
 */
struct Class {
    Class(StringId name, DeclPtr<Module> module, LocationId source):
        name(name), module(module), source(source) {}

    StringId name {};
    DeclPtr<Module> module;
    LocationId source = kNullLocation;

    // `pub` on the class head, which exports the class *and its member signatures* - they are the
    // interface it declares, so there is nothing for a second marker on a member to mean.
    bool exported = false;

    /*
     * The head's parameters - a closed `GenContext`, exactly as a `data` head has one.
     *
     * Every member's signature is a generic function *over this context*, which is what makes a
     * class call and a generic call one mechanism: the call binds the class's arguments the way an
     * ordinary call binds a callee's, and `callTarget` turns the pair into a callee - by selecting
     * an instance here where it would specialize there.
     */
    DeclPtr<GenContext> gen = nullptr;

    // What an instance must supply, in declaration order. A member with a body is a **default**: an
    // instance that leaves it out gets that body specialized at its own type, which is what a
    // generic body already is.
    DeclList<DeclPtr<Function>> members;

    /*
     * `class (Eq(a)) Ord(a)` - the classes this one requires, each applied to types of *this* head.
     *
     * A requirement rather than a bare class handle, which is what lets a superclass be applied to
     * something other than the whole parameter list in order: `class (Eq(b)) Keyed(a, b)` constrains
     * the second parameter, and a list of class handles could only have said "some superclass". An
     * instance of this class needs an instance of each, and a requirement of this class supplies
     * each - both through `classProves`, which is where the substitution is written.
     */
    DeclList<GenRequirement, false> superclasses;

    /*
     * `class Contiguous(c -> a)` - the index of the first parameter the ones before it decide, and
     * the arity where there is no arrow.
     *
     * A **functional dependency**, and it is what makes a constrained signature usable: `fn
     * (Contiguous(c, a)) first(xs: c) -> a` binds `c` from its argument and has nothing to bind `a`
     * with, so without the promise that one `c` has one `a` the call reports "cannot infer `a`" and
     * every such signature is unwritable. What the promise buys is a *lookup*: selection keys on the
     * deciding prefix alone, and the determined arguments are read back off the head that matched.
     *
     * Zero is not a legal value - something has to determine the first parameter - so the arity
     * standing for "no arrow" costs no second field and no sentinel of its own.
     */
    U16 determined = 0;

    /*
     * Which compiler role this class fills, or `Count` - `compiler/intrinsic/known.def`.
     *
     * Written by `@builtin(role)` on the declaration, and by nothing else - a class the compiler
     * made for itself was the other source, and there is no longer one. A field here as well as an
     * entry in `Program::roles` because the two answer opposite questions: the table maps a role to
     * its class, and this maps a class back to its role - which is what the walks that are *handed*
     * a class ask, and what a search of the table for every such question would cost.
     */
    intrinsic::Known role = intrinsic::Known::Count;

    U16 arity(DeclBase base);

    // Whether parameter `index` is one the others decide, which is the whole of what the arrow says.
    bool isDetermined(U16 index) const { return index >= determined; }
};

/*
 * One `instance C(T)` the program wrote - the head, and the members it supplies.
 *
 * The member is an ordinary `Function` with an ordinary body, and a teardown's is called with the
 * value it releases: everything from the argument mode down is the same machinery every other call
 * uses. A teardown that were a special kind of body would be a second call path for the one call the
 * compiler inserts rather than the author.
 *
 * `Copy`'s member is the same shape and is *not* a teardown: it borrows its subject and returns a
 * second value, so `Function::teardown` stays false for it and the drop elaboration never reaches
 * it. What it shares with the other two is the table, which is the only thing the classifier asks.
 */
struct Instance {
    DeclPtr<Class> klass = nullptr;

    /*
     * The head - one written type per class parameter, in the class's order.
     *
     * These are **patterns** rather than types a value has: `instance (Eq(a)) Eq(Opt(a))` writes
     * `Opt(a)` over the instance's own context, and what selection does is match the types a call
     * reached it with against them. A ground head - `instance Eq(I32)` - is the same structure with
     * nothing to bind, which is why there is one shape here and not two.
     */
    DeclList<DeclPtr<Type>, false> args;

    /*
     * The instance's own generic context, or null for a ground head.
     *
     * `instance (Eq(a)) Eq(Opt(a))` binds `a` in its head and *every member it writes is generic in
     * it* - so a member is an ordinary generic function over this context, and what runs for
     * `Eq(Opt(I32))` is that member specialized at the bindings the match produced. That is the same
     * sentence a class default already obeys with the class's context, which is why a parametric
     * instance needed no second mechanism below this line.
     *
     * The context also carries the head's own requirements - the `(Eq(a))` - so an instance is
     * selected only where they hold, and a member's body may rely on them.
     */
    DeclPtr<GenContext> gen = nullptr;

    // One per class member, in the class's order - the instance's own where it wrote one, and null
    // where it left one out. A left-out member is the class's **default**, and what fills the gap is
    // `instanceMember` at the point of use rather than a slot here: one instance serves every
    // argument list its head matches, so a slot could only ever have held the first of them.
    DeclList<DeclPtr<Function>> members;

    LocationId source = kNullLocation;

    /*
     * `@local` - `doc/spec/modules.md`'s local declarations, at the `instance` placement: the
     * teardown may only be triggered from the module that declared it.
     *
     * Not a visibility flag, and the difference is what makes the attribute sound. The instance is
     * still unique, still in the one table, and still what selection finds from anywhere; what is
     * restricted is permission to reach its member. Hiding the instance instead would make coherence
     * depend on who is looking, which is the thing a `private instance` cannot be.
     *
     * The module to compare against is the *instance's* rather than the type's, and they can differ:
     * the orphan rule admits an instance in the module of any type in the head.
     */
    bool local = false;

    DeclPtr<Module> module = nullptr;

    /*
     * Which `provided.def` row generated this instance, or `Count` for one an author wrote.
     *
     * The one thing about a generated instance that selection, the summaries and the conversion do
     * not need to know, and the one thing `finishCall` does: a member of a provided instance
     * **expands where it is called** when the call is concrete, and is called like any other
     * member where it is not. So the tag is read at exactly one place and is a tag rather than a
     * kind - the instance is in the same table under the same key as every other.
     */
    intrinsic::Provided provided = intrinsic::Provided::Count;
};

/*
 * One implicit disposal a body places at a type it does not yet know - `doc/spec/ownership.md`'s
 * removals, and waypoint G4's whole subject.
 *
 * A **removal** is a capability an attribute takes away rather than replaces: `@explicitDrop`
 * withholds the implicit teardown and `@pinned` withholds relocation, and neither supplies anything
 * for a generic body to call instead. The spec says what follows from that - "a question with a
 * universal fallback is answered once in the body, and a question with no fallback has to be asked
 * at the instantiation" - and this is the record that carries it from the one place to the other.
 *
 * So the generic body asks and the specialization answers, which is the sentence the waypoint is.
 * `analyze_drop.cpp` writes one of these wherever it chose the point a value of a variable-carrying
 * type dies; `specializeFunction` substitutes the type and asks the two questions a disposal has to
 * pass - may this be disposed of implicitly, and may *this module* trigger its teardown. Without it
 * a generic function launders both: `fn discard(->x: a): return` at an `@explicitDrop` type is the
 * implicit disposal the attribute exists to refuse, with no diagnostic anywhere.
 *
 * The **path's declared type** rather than the variable it mentions, because the two are not the
 * same question: `Boxed(a)` at `a = Conn` is an `@explicitDrop` record even though `Boxed` is not
 * one, and substituting the type is what says so. It is recorded once per distinct type, since what
 * is checked is the type and a body that lets three of them die says one thing three times.
 */
struct Disposal {
    DeclPtr<Type> type = nullptr;

    // Where in the *generic* body the value dies. Carried so that a report at the instantiation can
    // be about a point, and read by nothing else.
    LocationId source = kNullLocation;

    /*
     * Whether the *compiler* chose the point - no `->x` in the source, and not the subject of a
     * teardown, which are the two exemptions the concrete check already has.
     *
     * It separates the two questions asked of one record. `@explicitDrop` is about who chose, so it
     * reads this; `@local` is about the disposal happening at all in a module that may not trigger
     * it, so it does not - a written `->x` at a foreign local teardown is the same violation as an
     * implicit one.
     */
    bool implicit = false;
};

/*
 * A function declaration.
 *
 * It has at most one body of each kind at a time, and both only during conversion. `mir` is cleared
 * when the MIR region is released; `ssa` is what optimization and lowering read. They are separate
 * fields rather than a tagged one because the conversion genuinely holds both for the length of one
 * function.
 */
struct Function {
    Function(StringId name, DeclPtr<Module> module): name(name), symbol(name), module(module) {}

    /*
     * The eight-byte-aligned member leads and the two-byte and one-byte members trail, which is
     * what makes this 168 bytes rather than 192: laid out in the order the fields were argued in,
     * the summary sat behind fifteen bytes of padding and the flags between four-byte members cost
     * three each. The order below is by size and nothing else; the argument for each field is on
     * the field.
     */
    Summary summary;

    /*
     * What source calls this function - the lookup key, together with its arity.
     *
     * Not unique on its own, and that is the point of `symbol` beside it. **One plain function per
     * name, any arity** is the surface rule, so `fn negate(x)` and `fn negate(x, from)` are two
     * declarations that share a name and a module - which is exactly what `Core`'s unary and binary
     * `-` need - and a call picks between them by how many arguments it wrote.
     */
    StringId name {};

    /*
     * What the emitted program calls it - unique within the module, and what a dump prints.
     *
     * A separate field because two things stopped being the same the moment one name could mean two
     * declarations: everything below the resolver keys a function by a single name - the lowered
     * module holds one map from `StringId` to function and the backend's symbol table one entry per
     * name - so two `negate`s would be one symbol with two bodies, which is not a diagnostic
     * anywhere and simply loses the second.
     *
     * Equal to `name` for the overwhelming majority, which is every function whose name nothing else
     * in its module took; `Program::uniqueSymbol` is what makes it differ, and it appends rather than
     * encoding the arity so that a reader sees the name they wrote and a suffix, rather than a
     * mangling to decode.
     */
    StringId symbol {};

    DeclPtr<Module> module;

    LocationId source = kNullLocation;

    DeclList<DeclPtr<Arg>> args;

    /*
     * What leaves this function.
     *
     * For a `lens fn` that is the *continuation's* result and therefore the fresh variable the
     * desugaring binds - see `handover` below, which is the other half. One field per fact: this
     * one used to hold the hand-over as well, with `lens` saying which was meant, and the two came
     * apart the moment a lens had a result of its own.
     */
    Occurrence result;

    /*
     * What a `lens fn`'s `yield` hands over, and empty on every other function.
     *
     * The declared outgoing occurrence of a *binding* position, resolved by `resolveOccurrence` like
     * a parameter: an unqualified spelling borrows for the continuation's extent
     * (Analysis-Borrows-Revised.md §3.3) where a result would own. It is also what the continuation
     * parameter's one position is declared as, so the two can never disagree - `declareSignature`
     * builds the parameter out of this field.
     */
    Occurrence handover;

    /*
     * What the **continuation** answers with, which is not what the function returns.
     *
     * They are the same type for every lens that cannot fail - a transparent lens returns whatever
     * its continuation answered, which is `$r` - and they come apart exactly at `| E`: the
     * continuation still answers the exit signal, and the *function* answers that or a failure.
     * Recorded rather than derived from `result`, because a reader that had to undo the wrapper
     * would be a second statement of which half of it means "the continuation ran".
     *
     * `declareContinuation` fills it and builds the continuation parameter out of it, so the
     * parameter's declared result and this cannot disagree.
     */
    Occurrence continuationResult;

    /*
     * `| E` - the failure channel a `lens fn` or an `iter fn` declares, and null where it has none.
     *
     * Analysis-External-Iterators.md §2.2, read for lenses and internal iterators. A traversal with
     * one may end *without having handed over*, which is Analysis-Lens.md §7.1's **skipping** lens,
     * and the two are one declaration: the ability to skip is the presence of this type. A call site
     * reads it to know it owes an alternative, `checkHandover` reads it to know the exactly-once
     * rule has an exit it does not cover, and `return` inside the body means "finish with one".
     *
     * A type rather than a flag, because absence is `Never` and every reader wants the type anyway.
     */
    DeclPtr<Type> failure = nullptr;

    /*
     * The environment record a **lifted lambda** takes as its first parameter - the Closures
     * section of `doc/spec/functions.md`, and null for every other function.
     *
     * Its presence is what says argument zero is the environment rather than something the source
     * wrote, which is the one thing a reader of `args` has to know about a closure: `funTypeOf`
     * skips that position, so the type a value of this function has is the one the lambda was
     * written with. Everything else about a lifted lambda is an ordinary function.
     *
     * The type rather than a flag, because the field has a second reader: the SSA lowering builds
     * the environment from it, and a flag would send that reader back to the parameter to find out
     * what it is building. **Argument zero is one slot, not two** - a generic environment for an
     * erased body, captures for a closure - so a lifted lambda is never generic, which
     * `resolveLambda` refuses at the point the two would collide rather than here.
     */
    DeclPtr<Type> environment = nullptr;

    /*
     * Where that environment's storage is - D17, and the one bit that decides what a value of this
     * lambda carries in its second word.
     *
     * `Frame` is a local of the function the lambda was written in, reached by a borrow: the value
     * carries `Shapes::closure` and may not leave. `Heap` is a block of its own, allocated where
     * the value is built, whose first word is the teardown that releases the captures and hands the
     * block back - so the value owns its environment, carries no origin at all, and may go anywhere.
     * `selectStorage` is what ranks the two, from the position the lambda fills.
     *
     * A `StorageClass` and not a bool because that is the vocabulary the rest of the compiler places
     * storage in, and the day `Region` is selectable an environment is one more thing that can go in
     * one. Meaningless where `environment` is null.
     */
    StorageClass environmentStorage = StorageClass::Frame;

    /*
     * The generic environment this signature binds - `gen.h`, and null for a plain function.
     *
     * Its variables are what the parameters and the result are written in terms of, and its frozen
     * `GenSchema` is the numbering a caller and this callee both index by. A specialization has none:
     * `specializes` below is what it holds instead, because its parameters are bound.
     *
     * §10.4's region placement is what will extend it. An ambient region needs a *witness*:
     * `StorageClass::Region` means "the region the witness names", and the witness is a declared
     * member of this environment that a caller substitutes - a parameter, like a type parameter,
     * arriving with the call. What it must never be is something lowering synthesizes because it
     * noticed an object outliving its frame; that is a region invented at the point of use, and two
     * uses invent two. Until there is a slot kind for one, `verifyBody` refuses an object placed in
     * a region, so the first pass that selects that rung without a witness reports rather than
     * lowering something with nowhere to go.
     */
    DeclPtr<GenContext> gen = nullptr;

    /*
     * What this function is a specialization of, and with which arguments - `specialize.cpp`.
     *
     * Set on the **clone**, never on the generic: a specialization is a substituted copy of an
     * already-resolved and already-analysed body, so what it records is where it came from rather
     * than a second environment to be read through. Both are null on an ordinary function.
     *
     * The pair is what interns them: one specialization per (generic, argument list), so a function
     * called at the same types from ten places is one declaration.
     *
     * Non-embedding, and that is a requirement rather than a preference: `substitutionOf` hands out
     * a pointer into this list, and a `SmallList`'s embedded encoding tags the high bit of each
     * entry - so the entries would not be addressable and the handles read out of them would not be
     * the handles that were put in.
     */
    DeclPtr<Function> specializes = nullptr;

    DeclList<DeclPtr<Type>, false> typeArguments;

    /*
     * How many argument lists this **generic** has been specialized at - the budget's per-declaration
     * term, `specialize.cpp`.
     *
     * On the generic and not on the clone, which is what makes it a count rather than a flag: what
     * costs code is the number of bodies one declaration turns into, and each of those bodies is a
     * clone that would have nothing to count. Maintained by `specializeFunction` on an intern
     * *miss*, so ten calls at one argument list are one body and count once.
     */
    U32 specializations = 0;

    /*
     * Which entry of `Resolver::inferringDecls` this body is read out of - meaningless unless
     * `inferResult`.
     *
     * A caller may need the result before the body walk has reached the declaration, and what
     * answers it is resolving that body now - `Resolver::requireResult`. So the declaration has to
     * be findable from the function rather than only from the walk that is going to reach it.
     *
     * An index rather than an `ast::Decl*`, for `Alias::written`'s reason: a declaration-arena node
     * holding a raw pointer into the parse region is the cross-arena reference this region's rule is
     * about, and the index is meaningful only while the resolver is running, which is the only time
     * anything asks.
     */
    U32 written = 0;

    /*
     * Which class member this signature *is* - null on every other function.
     *
     * Set on the **class's** declaration of the member, not on an instance's implementation of it:
     * this is the signature a call is checked against and the thing `callTarget` recognizes. Its
     * `gen` is the class's own context, so the type arguments a call binds are the class's, and the
     * two questions "which specialization" and "which instance" are one call to one function asked
     * of two kinds of callee.
     *
     * A body here is the class **default**, which is an ordinary generic body: an instance that
     * leaves the member out gets it specialized at the instance's type, so a default and a hand
     * written implementation are the same kind of thing by the time anything calls one.
     */
    DeclPtr<Class> klass = nullptr;

    MirPtr<Body> mir = nullptr;

    SsaPtr<ssa::Body> ssa = nullptr;

    /*
     * The implicit disposals this body places at types it does not know - `Disposal`.
     *
     * Empty on every function whose signature binds no type variable, which is almost all of them:
     * a concrete body's disposals are checked where they are placed, and there is nothing to defer.
     * Filled by the ownership analysis and read by `specializeFunction`, in that order - which the
     * pass order gives, since every body is analysed before the first call is resolved to a callee.
     */
    DeclList<Disposal, false> disposals;

    /*
     * Which parameter is the continuation, for a `lens fn`, and `kNoContinuation` otherwise.
     *
     * Always the last one under the `yield` desugaring, and recorded rather than derived because the
     * explicit-callback form names it and may not put it last for ever. `yield` reads it, and so
     * does the call site when it types the arguments the *author* wrote against the ones the
     * signature has.
     */
    static constexpr U16 kNoContinuation = 0xffff;
    U16 continuationArg = kNoContinuation;

    /*
     * How many arguments a *call* writes, which is not how many the signature has.
     *
     * The one number every name lookup, arity check and call site uses, because a lens's
     * continuation is a parameter of the desugaring and never something the source passes. Kept as a
     * function of `continuationArg` rather than as a second field, so that the two cannot disagree
     * about a signature that has one.
     */
    U32 writtenArity() {
        return U32(args.size()) - (continuationArg == kNoContinuation ? 0 : 1);
    }

    /*
     * How few a call may write - the positions that carry no default.
     *
     * The pair with `writtenArity` is the *range* of call arities this declaration answers, and it is
     * a range rather than a number because `Arg::omittable` exists: `fn open(path, mode = Read)` is
     * one declaration a call reaches with one argument or with two, and so is a signature whose last
     * position is `@caller`. `Program::addFunctionName` files it
     * under every arity in the range, so the set a name reaches is still a single hash read - what a
     * second *plain* declaration collides with is a range instead of a point.
     *
     * Counted rather than stored, for `writtenArity`'s reason: a second field is a second answer,
     * and a signature's parameter list is the length somebody typed.
     */
    U32 requiredArity(DeclBase base) {
        U32 count = 0;
        for(U32 i = 0; i < U32(args.size()); i++) {
            if(i == continuationArg) continue;
            if(base[args.get(base, i)]->omittable()) continue;
            count++;
        }

        return count;
    }

    // Whether a call writing this many arguments reaches this declaration. The one predicate every
    // "does this serve the call" site asks, so that a lookup by arity and a match by arity cannot
    // come to disagree about a defaulted position.
    bool acceptsArity(DeclBase base, U32 count) {
        return count <= writtenArity() && count >= requiredArity(base);
    }

    /*
     * How many published origin sets this signature introduces.
     *
     * The scope `Contract::slot` and `ExternalSlot::origin` are numbered in. The *structure* of what
     * relates through each one is on the occurrences themselves - each argument's contract for its
     * root and its interned shape for what is inside it - so what is left here is the count, which
     * is what a caller substitutes against and what a diagnostic names.
     *
     * Zero until loans are implemented, which is every signature so far.
     */
    U16 originSlots = 0;

    // Which member of `klass` - beside the other two-byte members rather than beside the class,
    // which is what keeps this struct at 168 bytes; see the note on `summary`.
    U16 classMember = 0;

    /*
     * Which member of which generated instance this is - `provided.def`'s row and the class's
     * member index - or `Count` for every function an author wrote.
     *
     * Read by two things and nothing else: the emitter that expands a concrete call to it in place
     * of the call, and the pass that writes its body, which is the same emitter over the parameters.
     * Neither reads the *class* member index off `classMember`, because that field is the class's
     * declaration's and this is an instance's implementation; the index is stored beside the row so
     * that one lookup answers "what does this compute".
     */
    intrinsic::Provided provided = intrinsic::Provided::Count;

    U16 providedMember = 0;

    /*
     * Which operation the compiler implements this signature as, or `Count` - `op.def`.
     *
     * Set by `@builtin(role)` on a declaration with no body, and it is what makes the pair legal:
     * "no body" is otherwise a call this stage refuses to emit, and an intrinsic is precisely a
     * declaration whose body the compiler supplies. The declared signature is still *the* signature -
     * every call is checked against it exactly as against a function with a body - and what this
     * changes is only which node the call site builds at the end. See `intrinsic/expand.h`.
     *
     * `checkRoles` has verified the shape by the time any body is resolved, so an emitter reading
     * this may rely on the positions `op.def` states and does not test them again.
     */
    intrinsic::Op intrinsic = intrinsic::Op::Count;

    /*
     * `pub` - whether a module that imports this one may write the name.
     *
     * A **class member** takes the class's answer rather than carrying its own: `pub class` exports
     * the signatures it declares, and an instance member is reached by selection rather than by
     * name. So this is written by `declareSignature` for a plain `fn` and copied from the head for
     * a member, which is what keeps "who may name this" one question with one answer.
     */
    bool exported = false;

    /*
     * `@inline(always)` - a body expanded where it is called, by the SSA conversion, rather than
     * called and perhaps inlined by the optimizer later.
     *
     * A **directive**: `@inline` is a hint the optimizer may decline, and this is one the resolver
     * carries out at every call or refuses at the declaration. What it buys is the property
     * `doc/spec/collections.md` states for `xs[i]` and `length(xs)` - that the language's most
     * common operations cost nothing *with no optimizer having run* - written on the library body
     * that implements each rather than on a node the compiler holds instead. `resolve_ssa/convert.cpp`
     * grafts the body into the caller's own blocks; `checkAlwaysInline` is what makes it refusable
     * at the declaration.
     */
    bool alwaysInline = false;

    /*
     * `@inline` with no argument - a **hint**, and the one of the three that is neither directive.
     *
     * What it says is "weigh this callee as if it were smaller", and what it must not say is "copy
     * this body": a callee the inliner refuses structurally - a closure, a lens, a recursive one - is
     * still refused, and the ceiling still applies. That is what makes it honest, and it is why the
     * reader records a flag rather than a number: the number is the optimizer's and is stated in one
     * place, `opt/inline.cpp`'s `Budget`.
     *
     * On the declaration rather than at the call site, on the same terms as `@platform`: a property
     * read from the declaration is one two separate compilations agree about without seeing each
     * other. `resolve_ssa/opt/inline.cpp` is the one reader.
     */
    bool inlineHint = false;

    /*
     * `@noinline` - a body the optimizer may not expand into its callers.
     *
     * A **directive** in the other direction, and the one of the three the compiler can always carry
     * out: declining to inline is always possible, where inlining is not.
     *
     * What depends on it is a call boundary a program *needs*: a measurement that would otherwise
     * fold into its caller, a wrapper whose whole content is the call, and every fixture whose
     * subject is what a body compiles to rather than what a program answers.
     * `resolve_ssa/opt/inline.cpp` is the one reader, and the conflict with `@inline(always)` is
     * refused where both are read.
     */
    bool noInline = false;

    // `@x86_legacy_sse` - see the attribute's row, where the reason a resolver carries a backend
    // fact at all is written. Carried to `LowerFunction::legacyVectors` and read nowhere else here.
    bool legacySse = false;

    /*
     * `@convention(clobber)` - see the attribute's row.
     *
     * A `Maybe` rather than a `LowerCallType` initialized to the default, because "wrote nothing"
     * and "wrote the convention the compiler would have picked" are different statements: the first
     * leaves the backend free and the second does not, and a field that could not tell them apart
     * would make a future default change silently break a declaration that pinned the old one.
     */
    Maybe<LowerCallType> convention = Nothing();

    /*
     * `lens fn` in the `yield` form - Analysis-Lens.md §5.2.
     *
     * A declaration fact, and after milestone 7 an ordinary function with one extra parameter:
     * `lens fn f(a) -> P` declares `fn ($r) f(a, body: (P) -> $r) -> $r`, `yield e` is a call of
     * that parameter, and the body is resolved and converted once like any other. What the marker
     * still says is the *contract* - the continuation runs at most once, during this activation, and
     * is not kept - which is what a call site reads to know it may split its own block, and what
     * lets the caller build the continuation's environment on its own frame.
     */
    bool lens = false;

    /*
     * `iter fn` - a lens, plus the two things §7.3 says an iterator specializes.
     *
     * It is `lens` too, and that is the point: the declaration, the hand-over position, the
     * continuation parameter and the call site's region are one construct, and an iterator is the
     * instance of it whose continuation runs any number of times. What this flag decides is exactly
     * §7.3's "it fixes both types rather than letting the author declare them" - the continuation
     * answers the step signal rather than a free `$r` - and the two rules that follow from a body
     * that hands over repeatedly: a `yield` stops the iterator where the consumer left, and there is
     * no exactly-once rule to check.
     */
    bool iterator = false;

    /*
     * Which form this generic body is compiled in - `GenForm`, decided by `selectGenForms`.
     *
     * Written by `@specialize` or `@generic` and otherwise measured. Meaningless on a function that
     * binds nothing, where the pass leaves it `Specialized`: a declaration with no environment is
     * compiled one way and the two answers are the same code.
     *
     * On the **generic** and read through `bodyOwnerOf`, for the reason `mir` is: a specialization is
     * the generic's body under a substitution, so a form recorded on the clone would be one answer
     * per argument list to a question the body asks once.
     */
    GenForm form = GenForm::Auto;

    /*
     * Whether this function *is* a teardown - the member of an `instance Drop(T)` or
     * `instance Reclaim(T)`, whose first parameter is the object being destroyed.
     *
     * The reason it is a flag rather than something the analysis works out: that parameter must not
     * be given a move path. It arrives owned and is dead by the end of the body, so the ordinary rule
     * would place a teardown for it - and the teardown of a `T` is *this function*, so the body would
     * end by calling itself on every path. `doc/spec/ownership.md` says where the obligation stops -
     * "destructuring is where it stops: the value ceases to exist and its fields take over" - and a
     * teardown is that destruction, so the subject is not an ownership root of this frame at all.
     */
    bool teardown = false;

    /*
     * Whether this function *is* a derivation - the member that turns an owner into its view,
     * `isDerivationMember` in `type.cpp` and the same `ViewRow` column `ownerSubject` reads.
     *
     * A flag for the reason `teardown` is one, and it is the same failure: the body may not ask for
     * the thing it makes. An owner reaching a view position is a call to this member, so a
     * derivation whose own subject reached one would call itself on every path - and unlike a
     * missing teardown that is silent all the way to the machine. `adaptToView` is the one reader.
     */
    bool derivation = false;

    // Set for a function the program can reach. Reachability starts at the entry for a program and
    // at the exported declarations for a library, and nothing unreached is converted or lowered.
    bool reachable = false;

    /*
     * `fn double(x: I32) = x * 2` with no `-> T` - the body is what decides the result.
     *
     * `result` holds `{}` until it does, and that is a placeholder rather than an answer: unit is a
     * type that checks, so a body left at it would have its value computed and dropped at the `ret`
     * with nothing downstream able to tell. Whoever needs the answer before the body has run asks
     * `Resolver::requireResult`, which is what makes the type independent of declaration order; the
     * flag is cleared the moment the body decides, so it doubles as "has this been settled".
     *
     * Only a **plain free function** ever sets it - `declareFunction`, which is the one path such a
     * declaration takes. A class member's result is the class signature's and an instance member's
     * is inherited from it, so neither has a body to read it off that the caller could not already
     * see; a lens takes its result from its continuation, and the `=` form is refused on one.
     */
    bool inferResult = false;

    /*
     * The `=` form asked to infer through a cycle, reported once at the call that closed it.
     *
     * The poison, and it is a flag rather than an error *type* because this stage has no such type:
     * recording unit instead would be a second diagnostic about a signature the author never wrote -
     * a recursive `fact` becomes "no `*` accepts `(I32, {})`". So the function stops being callable
     * instead, silently, and `checkCallable` is where every later call to it stops.
     */
    bool resultUnknown = false;

    /*
     * Whether this body has been built - the three-colour walk `Alias::state` is, and for the same
     * shape of reason.
     *
     * `Reading` is the cycle guard: a call reached while the callee's own body is on the stack is
     * the call that closes a recursion, and it is the one place the `=` form can say so. `Read` is
     * what keeps the body walk from building a body a caller already demanded - two walks over one
     * declaration would be two sets of blocks in one function.
     */
    enum class BodyState: U8 { Unread, Reading, Read };
    BodyState bodyState = BodyState::Unread;
};

/*
 * A module-level `let`.
 *
 * `writable` is the source language's `let &`. `initial` is the constant its declaration recorded,
 * and an immutable global *is* that constant: reading one is not a load unless something took its
 * address, which is a decision the manifestation table makes rather than this declaration.
 */
struct Global {
    Global(StringId name, Occurrence occurrence): occurrence(occurrence), name(name) {}

    Occurrence occurrence;
    StringId name {};
    LocationId source = kNullLocation;

    // By value. A global outlives every body, so its initializer belongs to it rather than to a
    // region-allocated node that would have to be kept alive for its sake - see `Constant`.
    Constant initial;

    bool writable = false;
    bool reachable = false;

    // `pub`, on the same terms as everything else - `exportedSymbol`.
    bool exported = false;

    /*
     * Whether the **entry sequence** fills this rather than the initializer folding to a constant -
     * Analysis-Initialization.md stage B.
     *
     * `initial` is unset here, and that is the whole difference: a constant global holds its value
     * before the program starts and occupies read-only storage, and a dynamic one starts at the zero
     * of its type and is written where its declaration stands in the root module's top level.
     *
     * Only the root module has any. A library module's top-level code would have to run at some
     * point in a program's startup and defining that point is the cross-module half stage B
     * deliberately does not do, so a non-constant initializer there is refused where it is written.
     */
    bool dynamic = false;

    /*
     * Whether anything **writes** this global's storage, which is what a lowering has to know and is
     * not the question `writable` answers.
     *
     * A `dynamic` global is immutable to every expression that names it and is still written once,
     * by the entry sequence that produces its value. Lowering one as read-only data is not a missed
     * optimization but a wrong program: LLVM's `constant` is a promise, so every read of such a
     * global folds to the zero its storage was declared with and the store is dropped as dead. A
     * compiler-built table, a string literal's bytes and a constant `let` are the globals the
     * promise is true for.
     *
     * Here rather than in either lowering, because both of them ask it and the answer is a fact
     * about the declaration: a target that got `writable` and re-derived the rest would be a second
     * statement of a rule that has one. `test/resolve/GlobalWritten.yana` is the fixture.
     */
    bool isWritten() const { return writable || dynamic; }
};

/*
 * One `import`, linked - Analysis-Modules.md §2.1.2.
 *
 * An import is **written in a file and scoped to that file**, which is why `file` is here rather
 * than the list being per module: two files of one module importing the same thing is two files each
 * naming what it uses, and a name one file imported is not visible to its siblings. `kEveryFile` is
 * for the edges no file wrote - the implicit one every module has to Core.
 *
 * `include` and `exclude` are the two list forms, and at most one of them is non-empty. They name
 * the *leading segment* of a symbol, so `hiding (Maybe)` hides `Maybe.Just` along with the type, and
 * hiding a single constructor of a visible type is not something an import list can say.
 */
struct Import {
    DeclPtr<Module> module = nullptr;

    // What this module is written as here - its own name, or the `as X` alias. Possibly several
    // segments either way, since a module name is `Native.Linux` as readily as `Math`.
    StringId localName {};

    DeclList<StringId, false> include;
    DeclList<StringId, false> exclude;

    // An index into `Module::files`, or `kEveryFile`.
    U16 file = 0;

    // `import qualified M` - contributes its qualified spelling and nothing to unqualified lookup.
    bool qualified = false;

    static constexpr U16 kEveryFile = maxLimit<U16>;

    // Whether this import is in scope in `from` - see `Module::activeFile`.
    bool inScope(U16 from) const { return file == kEveryFile || file == from; }

    // Whether one symbol of this import's module may be named here, per its own lists. The
    // *declaring* side of visibility is `exportedSymbol`; this is the importing side, and the two
    // are asked together by `search` so that neither can be forgotten at a call site.
    bool visible(DeclBase base, StringId name);
};

/*
 * One module's declarations.
 *
 * Placement-allocated in the declaration arena, and therefore holding **no heap containers**: the
 * dense lists are `RegionArray`, whose storage is the region's, and the by-name indexes are on
 * `Program`. This struct used to hold two `Array`s and two `HashMap`s, which leaked four heap
 * buffers per module per compilation - a bump arena is released whole and runs no destructor, so
 * nothing it holds may own memory. `Types` is the same rule seen from the other side.
 *
 * Declaration order, which is emission order. A hash map would make the data section's layout a
 * property of the hash function - the note on `LowerModule::globalOrder` is the record of that
 * having actually happened.
 */
struct Module {
    explicit Module(StringId name): name(name) {}

    StringId name {};

    /*
     * What this module can see, with each entry tagged by the file that wrote it - `Import::file`.
     *
     * One list rather than one per file, because the traversal in `search` is over all of them at
     * once: two candidates found through two imports are ambiguous whichever files they came from,
     * and the implicit edge belongs to every file.
     */
    RegionArray<DeclRegion, Import> imports;

    /*
     * The files this module is made of, by name, in the order the passes read them.
     *
     * Names rather than `ast::Module*`, and that is the arena rule rather than economy: a
     * declaration-region node holding a pointer into the parse region is a cross-arena reference
     * that outlives what it points at. A file's name is what a `Location` records as its
     * `sourceModule`, which is the only thing `fileOf` needs.
     */
    RegionArray<DeclRegion, StringId> files;

    /*
     * Which file of this module is being read - Analysis-Modules.md §2.1.2.
     *
     * An import is scoped to the file that wrote it, so every name lookup has to know which file it
     * is a lookup *from*. The alternative is threading that through `resolveType` and every `find`
     * call site, for a fact that is in practice a well-nested dynamic extent: a declaration pass
     * reads one file at a time, and a body belongs to the file its declaration was written in.
     *
     * So it is set at the few points that begin such an extent and never anywhere else, through
     * `FileScope`, which **restores** the previous value rather than clearing it - resolution nests,
     * because instantiating a generic resolves a body in the middle of another one.
     */
    U16 activeFile = 0;

    // Which file a location is in, as an index into `files`. The declaration's own record of where
    // it was written is the ground truth, so nothing carries a second copy of that fact to fall out
    // of step with it. File 0 for a location in no file of this module, which is what a synthesized
    // declaration has - and those are in modules of one file, where 0 is the only answer there is.
    U16 fileOf(DeclBase base, Context& context, LocationId source);

    // Whether this module's files are the standard library package's - `ast::ModuleGroup::library`.
    // The importer's half of the package check: a package's own modules see all of it.
    bool fromLibrary = false;

    /*
     * A module of a package this compilation is not, which that package's manifest does not export.
     *
     * Decided where the module is found, because that is the only place that knows *which* package
     * answered, and read at the import that named it, because that is where a person can act on it.
     */
    bool packagePrivate = false;

    RegionArray<DeclRegion, DeclPtr<Function>> functions;
    RegionArray<DeclRegion, DeclPtr<Global>> globals;

    // The `data` declarations, in declaration order. A record type is nominal, so the declaration
    // and the type are one object - see `RecordType` - and this is the list `classifyTypes` folds.
    RegionArray<DeclRegion, DeclPtr<RecordType>> records;

    RegionArray<DeclRegion, DeclPtr<Instance>> instances;
};

/*
 * The overload set one `(name, arity)` reaches - `doc/spec/overloading.md`'s R1.
 *
 * "Its members are the class functions with that name and arity, plus **at most one** plain
 * function." Both halves are here and they are held apart rather than in one list, because the two
 * are read differently at every step that reads them: R5 says a plain function that matches *wins*
 * over every class candidate, so which half a candidate is in is not a property to recover by asking
 * it afterwards - it is the shape of the rule.
 *
 * This is the answered form, a value the caller owns. `Overloads` beside it is the stored form; the
 * two are separate because the stored one lives in the declaration arena and is written to as
 * declarations arrive, and a lookup crossing several modules must not hand back a handle into a
 * table a later declaration may grow.
 *
 * A set with one member is the overwhelming case and costs one `DeclPtr` and an empty array, which
 * is why the lookup answers this rather than answering a single function and making a second,
 * slower call for the rare case: two lookups that could disagree about what a name means is exactly
 * the shape of bug this stage exists to not have.
 */
struct OverloadSet {
    // R1's at-most-one, of this arity. Null where the name declares none, which is the ordinary
    // case for an operator and for every class member.
    DeclPtr<Function> plain = nullptr;

    // Every class function of this name and arity. More than one is R3's and R4's business; two
    // classes declaring one member name is legal and the standard library declines to do it as a
    // policy rather than because the language refuses.
    SmallArray<DeclPtr<Function>, 4> members;

    Size size() const { return (plain ? 1 : 0) + members.size(); }
    bool isEmpty() const { return size() == 0; }

    // The one candidate, where there is exactly one - null both where the name reaches nothing and
    // where it reaches several. A caller that wants a single declaration and has no arguments to
    // choose with is a caller for which "several" is not an answer.
    DeclPtr<Function> sole() const {
        if(size() != 1) return nullptr;
        return plain ? plain : members[0];
    }

    // Every candidate in R5's order - the plain function first, since it is what wins where it fits.
    DeclPtr<Function> at(Size index) const {
        if(plain) return index == 0 ? plain : members[index - 1];
        return members[index];
    }
};

/*
 * The stored half of R1's key, one entry per `(module, name, arity)`.
 *
 * The list is in the declaration arena because that is where everything a `Program` index points at
 * lives; the entry itself is a hash-map value, so it holds the list by handle and not by pointer -
 * a map that rehashes moves its entries, and a `SmallList` is a value that survives being moved.
 */
struct Overloads {
    DeclPtr<Function> plain = nullptr;
    DeclList<DeclPtr<Function>> members;
};

/*
 * One compilation.
 *
 * Owns the declaration arena and the MIR arena. The SSA arena is owned by the SSA stage, which is
 * what keeps the release order honest: MIR is destroyed at the end of conversion, and a compilation
 * that never converts - a rejected program, a completion request - never allocates the third region
 * at all.
 *
 * It also owns everything that needs a destructor: the interning tables and the by-name indexes.
 * That is not incidental - it is where the "nothing in an arena owns heap memory" rule puts them.
 */
/*
 * One `@test` declaration, as the attribute pass read it - Design-Test.md §11.2's F1.
 *
 * The `Test.Case` the entry builds out of this holds the same five things, which is not a
 * coincidence: the library's record is the contract and this is what the compiler can fill it from.
 */
struct TestDeclaration {
    DeclPtr<Function> function = nullptr;

    // The display name - `@test("a key inserted twice keeps the last value")` - or the declaration's
    // own name where none was written. A test name is prose and reads better than it computes, which
    // is why it is a string rather than a symbol.
    StringId name {};

    // Which module it was written in, so a report can group by it, and where, so a failure can be
    // jumped to. The site is the *declaration's* rather than any assertion's: `@caller` is what
    // gives an assertion its own.
    StringId module {};
    LocationId source = kNullLocation;

    // `@test(aborts)`: the case is expected to stop the process, so the harness runs it in a child
    // and reads the status rather than a report line. Design-Test.md §5.3.
    bool aborts = false;
};

/*
 * The `@test` declarations of one compilation, in the order they were read.
 *
 * A **list and nothing else**: which module they come from, which library type they are wrapped in
 * and what runs them are `resolveTestEntry`'s questions, asked once where the entry is built. The
 * registry is the compiler's rather than the library's because there is no static initializer, no
 * linker section and no reflection here for a `Test` module to register from - and a *written*
 * registration list, the only alternative, means every test is written twice and the second half is
 * invisible when it is missing.
 */
struct TestRegistry {
    Array<TestDeclaration> declarations;
};

struct Program {
    Program() = default;

    DeclArena decl { 4 * 1024 * 1024 };
    MirArena mir { 4 * 1024 * 1024 };

    Types types { decl };
    Shapes shapes { decl };

    Array<DeclPtr<Module>> modules;

    /*
     * The module the program was asked to compile, which is the first one discovered.
     *
     * Index zero rather than a flag on `Module`, so that "which module is the root" has one
     * definition instead of two that can disagree: `discoverModules` pushes it before it walks a
     * single import, and every other module of the program is something it reached from here. What
     * reads it is what the root is *for* - the entry point is its `main` and nobody else's, and its
     * declarations are what a program with no entry roots at.
     */
    DeclPtr<Module> root() const { return modules[0]; }

    /*
     * `Core`, where the program has one - the module every other one implicitly imports.
     *
     * Null until something builds it, and that is not a placeholder: `linkImports` adds the implicit
     * edge only where this is set, so the whole mechanism is dormant in a compilation that never
     * reaches Core rather than being a case each reader has to remember to skip. `[a]`, `[1, 2, 3]`
     * and a string literal are grammar rather than library, so what they mean has to be reachable
     * without being asked for - which is what the implicit edge is for and why there is exactly one.
     */
    DeclPtr<Module> core = nullptr;

    DeclPtr<Module> findModule(StringId name) const;

    /*
     * The library modules this compilation parsed, and their ASTs.
     *
     * Owned here because they have to outlive every declaration resolved against them - a diagnostic
     * quotes a location in one long after the parse - and a `Program` is the thing whose lifetime
     * that is. Empty in a compilation that imports nothing out of `lib/`, which is every fixture
     * that names only its own modules.
     */
    ast::LibraryModules libraryModules;

    /*
     * Every class in the program - `Class`.
     *
     * Program-wide rather than per module, because instance selection is: an instance is matched
     * against a class whatever module either was written in, so the walk that answers a constraint
     * wants one list. What a *reader* wants per module - the dump, which lists what a file declared
     * - is this list filtered by `Class::module`, which is one list with two readings rather than
     * two lists to keep in step.
     */
    Array<DeclPtr<Class>> classes;

    // Every alias in the program, in declaration order - what `resolveAliasTargets` walks. A list
    // rather than a per-module one for the reason `classes` is: the walk is over all of them and the
    // module is already on each.
    Array<DeclPtr<Alias>> aliases;

    /*
     * Every anonymous record - `RecordType::tuple`.
     *
     * Here rather than in a module's `records` because a tuple belongs to no module: it has no
     * declaration, so there is nowhere for it to have been written and nothing looks it up by name.
     * What the list is *for* is the one walk that would otherwise miss it - `classifyTypes` reaches a
     * record through the module that declares it, and a tuple made while the declarations are still
     * being read has to be classified by that pass exactly as an early instantiation is.
     */
    Array<DeclPtr<RecordType>> tuples;

    /*
     * Every aggregate constant, which is the **interning index** and not how one is reached:
     * `Constant::Aggregate` carries the handle itself, so any reader holding a `DeclBase` can
     * follow it - which is what a printer and a manifestation walk have and a `Program` is not.
     *
     * One table for the whole program rather than one per region, because a `Constant` is sixteen
     * bytes wherever it is held and its payload has to outlive all of them - see
     * `AggregateConstant`. Interned structurally by `aggregateConstant`, so that `Constant`'s
     * equality stays the comparison of three words it is for every other kind and a body's constant
     * table keeps interning the way it does.
     */
    Array<DeclPtr<AggregateConstant>> aggregates;

    /*
     * Whether `classifyTypes` has run - see `classifyRecord`.
     *
     * What it separates is a record made *before* the declaration walk finished, which the pass will
     * reach, from one made after it by a type application in a body, which has to classify itself.
     * A flag rather than each producer knowing where in the pipeline it stands, because the producer
     * is one function and the two answers are the whole difference between them.
     */
    bool typesClassified = false;

    /*
     * Whether any field in this program is an indirection - `RecordField::boxed`, written by `@box`
     * or by `breakLayoutCycles`.
     *
     * What it is for is **reachability**. A box is an allocation, so a program with one calls
     * `allocateHeap` and `freeHeap` - and neither call is written anywhere in the source, so the
     * walk that decides which functions are emitted has nothing to follow to them. This is what it
     * follows instead: set where a boxed field is made or where an object is placed off the frame,
     * read once by `markReachable`, which roots the allocator roles when it is true.
     *
     * Two producers and one flag, because they ask the allocator for the same thing: `@box`'s
     * indirection and `StorageClass::Heap` are both storage the lowering allocates with no node in
     * any body to draw an edge from. It is named after the first because that is what it was, and
     * renaming it would be a bigger diff than the fact deserves.
     *
     * Deliberately a property of the *declarations* rather than of what a body does with them. A
     * program that declares a boxed record and never builds one emits the allocator for nothing,
     * which costs a few hundred bytes; the precise question - "does any reachable body construct or
     * release one" - is answered by a walk that would have to run before the walk that needs the
     * answer.
     */
    bool usesBoxes = false;

    /*
     * Whether any reachable release site cannot see where the value's interior boxes were placed -
     * `settleBoxProvenance`, and `ssa::storesBoxProvenance`, which is what spends it.
     *
     * The bit a box carries answers one question and only at one kind of site: a teardown reached
     * through a borrow, a global or a raw address, where the caller may have placed the value
     * somewhere this program does not hand back to. A program with no such site stores a word that
     * nothing reads and pays for it in every record that holds a box - `Maybe(Tree)` is a pointer
     * with a byte behind it, so the record's stride doubles.
     *
     * Unlike `usesBoxes` above this is a property of the reachable *bodies*, and it has to be: every
     * library holds a function that overwrites through a `&`, so the declaration-level answer is
     * "yes" for every program that links one.
     */
    bool asksBoxProvenance = false;

    /*
     * Whether a supplied global was refused - `bindSuppliedGlobal`, which runs as each initializer
     * settles rather than in a pass of its own.
     *
     * A flag rather than a report `checkRoles` makes for itself, because the verdict is reached
     * earlier than the pass that acts on it: what "the compiler and this library disagree" means is
     * that no body should be resolved at all, and that decision is `checkRoles`' to make wherever
     * the fact was found.
     */
    bool suppliedRoleFailed = false;

    // Where the program starts. `main` where a module declares one; a synthesized entry once
    // top-level statements are implemented, since a program with initialization that started at
    // `main` would start after its own initialization. Under `-test` it is `resolveTestEntry`'s
    // synthesized runner instead - a test build runs tests.
    DeclPtr<Function> entry = nullptr;

    /*
     * Everything this compilation knows about its `@test` declarations - Design-Test.md §11.2's F1.
     *
     * A member of the program rather than a global, on the same terms as every other program-wide
     * table: two compilations in one process are two programs, and a registry that outlived one of
     * them would be a suite that grows every time the language server recompiles.
     *
     * Empty in every build without `-test`, where a `@test` is a diagnostic rather than an entry.
     */
    TestRegistry tests;

    Module* addModule(StringId name);

    /*
     * The by-name indexes, keyed by module and name together.
     *
     * Here rather than on `Module` because a `HashMap` owns a heap buffer and `Module` lives in the
     * arena. One map for the whole program rather than one per module, since the module is part of
     * the key anyway and a per-module map would be a per-module allocation with the same lifetime.
     */
    /*
     * Files a declaration under `(name, arity)`, and answers false where R1 says it may not be.
     *
     * A function's key is its name **and its arity** - which is what lets `Num` declare a unary and
     * a binary `-` - and what the key holds is `doc/spec/overloading.md`'s R1 set: every *class*
     * function of that name and arity, plus **at most one** plain function. So the only thing this
     * refuses is a second plain function at a key, which is the one part of the rule that is a
     * count; a class function joins the set beside whatever is already there.
     *
     * It used to hold exactly one declaration per key, and refuse everything after it. That reads as
     * a stricter reading of the same rule and is not one: R1's set is what a *call* selects from, so
     * a table holding one entry per key is not a stricter language, it is a table that cannot
     * express the language at all. `lib/Core` is where it showed - `Core/Vector.yana`'s plain `min`
     * over vectors beside `Ord`'s member of the same name and arity - and two classes declaring one
     * member name was refused by the same line.
     *
     * The key is a *range* of arities rather than one - `Function::requiredArity`. A defaulted
     * position makes one declaration reachable at several counts, and filing it once per count is
     * what keeps the lookup a single hash read where it would otherwise need a search.
     *
     * `conflict` is filled with the arity a second plain function collided at, so the message names
     * the count the two declarations actually collide at rather than either one's own length.
     * Nothing is written unless the whole range is admissible, for the reason it always was: a
     * half-registered declaration is reachable at the arities that happened to be checked first.
     */
    bool addFunctionName(DeclPtr<Module> module, DeclPtr<Function> function,
                         U32* conflict = nullptr);
    bool addGlobalName(DeclPtr<Module> module, StringId name, DeclPtr<Global> global);
    bool addRecordName(DeclPtr<Module> module, StringId name, DeclPtr<RecordType> record);

    // A transparent second name for a type - `Alias`. One table beside the records rather than an
    // entry in theirs, because what a lookup gets back is a *type* and not a declaration: an alias
    // has no fields, no instances and no layout for a record table's readers to ask it about.
    bool addAliasName(DeclPtr<Module> module, StringId name, DeclPtr<Alias> alias);
    DeclPtr<Alias> findAlias(DeclPtr<Module> module, StringId name) const;

    /*
     * R1's set at one `(name, arity)` in one module - see `OverloadSet`.
     *
     * Copied out rather than answered by handle: the caller holds it while it resolves arguments,
     * and resolving an argument can declare a specialization, which is a write to the very table
     * this came out of.
     */
    void findOverloads(DeclPtr<Module> module, StringId name, U32 arity, OverloadSet& into) const;

    /*
     * The one function of that name and arity, where the set has exactly one - null where it has
     * none and *also* where it has several.
     *
     * For the callers that have no arguments to select with and therefore no answer to "which of
     * them": a callable requirement's witness, the entry point, a synthesized call. A caller with a
     * call site to select at asks `findOverloads` and selects; one that asks this and gets null on
     * an overloaded name is asking a question the name does not have a single answer to, and saying
     * so is the honest report.
     */
    DeclPtr<Function> findFunction(DeclPtr<Module> module, StringId name, U32 arity) const;

    /*
     * A name nothing else in this module has taken, for a function's `symbol`.
     *
     * `negate` where it is free, `negate'2` and then `negate'3` where it is not. Asked by every
     * producer of a function - a written `fn`, an instance member, a specialization - because all
     * three can now collide: two arities share a source name, two class members share a synthesized
     * one, and two specializations of two same-named generics share a mangling.
     *
     * The bytes are interned, so the caller hands over text and gets a `StringId` that outlives it.
     */
    StringId uniqueSymbol(DeclPtr<Module> module, Context& context, StringView text);

    /*
     * A name built at compile time, interned over a copy of its bytes.
     *
     * **An interned name borrows its text** - `addUnqualifiedName` keeps the pointer it is handed -
     * so a name formatted into a `String` has to be copied somewhere that outlives the caller
     * before it is interned, and the declaration arena is that place. One producer, because the
     * failure is silent: the hash is right, so lookup and comparison work, and only a dump reads
     * whatever the freed bytes hold next. Every synthesized name in either tier goes through here.
     */
    StringId internName(Context& context, StringView text);

    /*
     * A record or a fixed array built out of constants, interned - see `AggregateConstant`.
     *
     * A linear scan, and it stays one for `Builder::addConstant`'s reason: the distinct aggregate
     * constants of a program are the ones somebody wrote down, which is a handful, and a hash map
     * over a tree-shaped key would be a heap allocation and a hash function to save a walk over
     * them. When a program appears whose table is long enough to matter, the measurement belongs
     * beside this comment.
     *
     * Interning is what lets the parts be compared by `Constant::operator ==`: the values are built
     * bottom-up, so a nested aggregate already has its index by the time the one containing it is
     * looked for.
     */
    Constant aggregateConstant(DeclPtr<Type> type, U16 constructor, const Constant* values,
                               Size count);

    /*
     * One function of that name in this module, whatever its arity, or null.
     *
     * What it is *for* is a message that can tell "no such function" from "not at this arity" - two
     * questions with two answers, since the second is the one an author who wrote the wrong number
     * of arguments has to read. A declaration rather than a bool, because the answer has to survive
     * the visibility clause in `mir::hasFunctionName`: a name only a private declaration carries is
     * not a name the author got the arity wrong on.
     *
     * An **exported** declaration wins where the module has both, so a public `f(x, y)` beside a
     * private `f(x)` still tells an importer which one it is about.
     */
    DeclPtr<Function> functionNamed(DeclPtr<Module> module, StringId name) const;
    DeclPtr<Global> findGlobal(DeclPtr<Module> module, StringId name) const;
    DeclPtr<RecordType> findRecord(DeclPtr<Module> module, StringId name) const;

    /*
     * The constructor table - a name to the record it builds and which arm of it.
     *
     * Separate from the record table because the two answer different questions and the answers
     * differ: `Proceed` is a constructor and not a type, `Outcome` is a type and not a constructor.
     * A record with one constructor is in both under one name, which is what makes
     * `data Pair {x: I32}` the general case with a list of one rather than a second rule.
     */
    bool addConstructorName(DeclPtr<Module> module, StringId name, ConstructorRef constructor);
    ConstructorRef findConstructor(DeclPtr<Module> module, StringId name) const;

    /*
     * The instance table, keyed by the class and its head's **outer constructors** - `select.h`.
     *
     * `instance Eq(Array(a))` is filed under `(Eq, Array)`, so the key is what an instance is *for*
     * rather than the whole type it was written at. A deciding position the head leaves *open* -
     * `instance (TrivialCopy(a)) Copy(a)`, and the `b` of `instance Rewrap(Maybe(a), b, Maybe(b))`
     * - contributes a wildcard to the key rather than making the whole head unfilable. See
     * `instanceKey`, where the pair of states this replaced is argued.
     *
     * A **list** per key, because the key is a filter and identity is not its job: `Copy(Box(I32))`
     * and `Copy(Box(I64))` share it and are two instances. What makes two heads of one shape a
     * diagnostic rather than one of two answers the teardown walk picks between by hash order is
     * `sameShape` in `addInstance`, which asks `matchType` both ways round; what keeps a bucket's
     * own order from being hash order is the specificity `addInstance` inserts by. Coherence is
     * still a property of this table, stated by the two of them rather than by the key alone.
     */
    /*
     * The instantiations made before the declaration they came from had been read - see
     * `instantiateRecord`, and `completeInstantiations`, which is what empties this.
     *
     * On the program rather than on the resolver because the interning table it shadows is here: an
     * instantiation is created once for the whole compilation, so the record of which ones are still
     * empty belongs beside the table that answers who has one.
     */
    Array<DeclPtr<RecordType>> pendingRecords;

    bool addInstance(DeclBase base, DeclPtr<Instance> instance);

    /*
     * Every head that could serve this argument list, most specific first - the narrowing step and
     * not the answer.
     *
     * What it hands back still has to be matched against the arguments, because `Eq(Array(a))` and
     * `Eq(Array(I32))` share a key by construction and only one of them serves `Array(Guard)`.
     */
    void candidateInstances(DeclBase base, DeclPtr<Class> klass, const DeclPtr<Type>* args,
                            Size count, SmallArray<DeclPtr<Instance>, 4>& into);

    /*
     * The heads that name a constructor in **every** deciding position, most specific first, or
     * nothing - the ownership classifier's question, and deliberately not the one above.
     *
     * `declaredInstanceFor` is the only caller: what it asks is what somebody declared *about this
     * type*, so a head that would have matched by wildcarding a position is not an answer to it.
     * A list for the same reason the one above is: `Copy(Box(I32))` and `Copy(Box(I64))` are one
     * key and only one of them is a statement about a given box.
     */
    void keyedInstances(DeclBase base, DeclPtr<Class> klass, const DeclPtr<Type>* args, Size count,
                        SmallArray<DeclPtr<Instance>, 4>& into);

    /*
     * The class table, by module and name - the name lookup a `class` head and an `instance` head
     * both go through.
     *
     * `core` names the three the compiler reaches for, whether they were declared or synthesized.
     * A pointer rather than a bool per class, because what the classifier needs is the declaration:
     * its member is what a teardown calls.
     */
    bool addClassName(DeclPtr<Module> module, StringId name, DeclPtr<Class> klass);
    DeclPtr<Class> findClass(DeclPtr<Module> module, StringId name) const;

    /*
     * `infixl 8 +` - what one module declares an operator's precedence and associativity to be.
     *
     * Keyed by module and name like every other declaration, and looked up through the imports by
     * `mir::findFixity`, because that is what a fixity is: a declaration of the module that wrote
     * it, in scope wherever that module is imported. False from `addFixity` where the module already
     * declared one for that operator.
     */
    bool addFixity(DeclPtr<Module> module, StringId name, OpProperties properties);
    Maybe<OpProperties> findFixity(DeclPtr<Module> module, StringId name) const;

    /*
     * What filled each of the compiler's roles - `intrinsic/roles.h`.
     *
     * The whole of the source-to-compiler coupling, in one field. Everything this stage knows about
     * the standard library is a subscript of this, and nothing below the declaration passes compares
     * a name against a spelling.
     */
    Roles roles;

    /*
     * The specialization table - one entry per (generic, argument list), `specialize.h`.
     *
     * Here rather than on the generic `Function` for the reason the name tables are here: a hash map
     * owns a heap buffer and a `Function` is placement-allocated in an arena that runs no
     * destructors. It is a *lookup* and not a cache - a second specialization for one argument list
     * would be two declarations of one function, which is a duplicated body rather than a slow one.
     *
     * One function rather than a find beside an add, because the two would probe the same table
     * twice and the second probe would have to land where the first did. What comes back is the
     * *slot*: null contents mean there is no such specialization yet and the caller fills it in,
     * which is `HashMap::add`'s own shape one level up.
     */
    DeclPtr<Function>* internSpecialization(DeclPtr<Function> generic, DeclPtr<Type>* args,
                                            Size count);

    /*
     * The environment table - one entry per (generic, argument list), `specialize.h`.
     *
     * Keyed exactly as the specialization table is, because the two answer the same question about
     * the same pair: what a call at these types reaches. Which of the two a call fills is the
     * generic's `form` and nothing else, so a program never has both for one pair - and separate
     * tables is what makes that unrepresentable rather than a rule about a shared one.
     */
    DeclPtr<GenEnvironment>* internEnvironment(DeclPtr<Function> generic, DeclPtr<Type>* args,
                                               Size count);

    // Releases the MIR region. Called once conversion has finished with every body, and it clears
    // each function's MIR handle first so that a stale one cannot be dereferenced afterwards.
    void releaseMir();

    /*
     * The function whose body the reachability walk is reading - `markReachable`, and null outside
     * it - and, per specialization, the one that was being read when it was made.
     *
     * Together they are the **chain of discoveries**: a specialization was made because some body
     * called at its argument list, that body is a specialization made because of another, and so on
     * back to a plain function. `growthPermitted` walks it, because that chain is where polymorphic
     * recursion shows - `f(a)` reaching `f(Boxed(a))` has an ancestor of the same generic with a
     * strictly shallower argument list at every step - and an absolute depth of the *type* was not:
     * it refused a one-off call at a legitimately deep record with nothing recursing at all.
     *
     * A side table keyed by the specialization's offset rather than a field of `Function`, because
     * only a specialization has one and `Function` is sized to the byte.
     */
    DeclPtr<Function> discoveringFrom = nullptr;
    HashMap<U32, U32> specializationParents;

private:
    static U64 nameKey(DeclPtr<Module> module, StringId name);

    HashMap<U64, Overloads> functionsByName;

    // Every name any function in a module has, whatever its arity - `functionNamed`.
    HashMap<U64, DeclPtr<Function>> functionNames;

    // How many functions of a module have asked for each symbol text - `uniqueSymbol`.
    HashMap<U64, U32> symbolCounts;
    HashMap<U64, DeclPtr<Global>> globalsByName;
    HashMap<U64, DeclPtr<RecordType>> recordsByName;
    HashMap<U64, ConstructorRef> constructorsByName;
    HashMap<U64, DeclPtr<Alias>> aliasesByName;
    HashMap<U64, SmallArray<DeclPtr<Instance>, 2>> instancesByHead;

    // One bucket of `instancesByHead` appended to a candidate list - the one place the table's
    // storage shape is read, and what keeps both lookups from each spelling it out.
    void keyedBucket(U64 key, SmallArray<DeclPtr<Instance>, 4>& into);
    HashMap<U64, DeclPtr<Class>> classesByName;
    HashMap<U64, OpProperties> fixities;

    // Keyed by a hash of the generic and its arguments, with the entry re-checked structurally -
    // `internSlot`, where the reason is written once for all four of this stage's intern tables.
    HashMap<U64, DeclPtr<Function>> specializations;
    HashMap<U64, DeclPtr<GenEnvironment>> environments;
};

/*
 * A class head's own answer for its parameter - `class FromInt(a = Int)`'s `Int`.
 *
 * The one question the compiler asks a `Known` class other than "where is your member", and it is
 * asked of exactly the two literal roles: what an unadorned `1` or `1.5` *is* has to come from the
 * library, because the library is where the language states it. Null where nothing claimed the role
 * or where the head declares no default, which is a broken library and is reported at the use site -
 * `sequenceType` is the same arrangement for the same reason.
 *
 * The **first** parameter, since a role of arity one has no other, and `checkRoles` is what refused
 * a claimant of a different arity before anything got here.
 */
DeclPtr<Type> classDefaultType(Program& program, intrinsic::Known role);

/*
 * The truth type - whatever declaration claimed `Known::boolean`, and null where nothing did.
 *
 * Through the role rather than by name, for `isSequenceType`'s reason: the library calls it `Bool`
 * and could call it anything. A function rather than a handle on `Types`, because a cached copy of
 * the role's answer is a second answer, and this one is not asked often enough to be worth having
 * two of. Null only reaches a caller while `checkRoles`' report about a library with no `Bool` is on
 * its way, since it stops the compilation before a body is resolved.
 */
DeclPtr<Type> booleanType(Program& program);

/*
 * `False` and `True` as constants of it - the constructors at index 0 and index 1.
 *
 * The order is the role's requirement rather than this function's assumption: `known.def` states
 * that a record claiming `boolean` numbers its two constructors from zero and `checkRoles` refuses
 * one that does not, because the comparisons the compiler emits answer 0 and 1. Which words the
 * library chose for them is not asked here and is not asked anywhere.
 */
Constant truthConstant(Program& program, bool value);

/*
 * Whether this type is the growable array - an instantiation of whatever claimed `Known::array`, or
 * the declaration itself.
 *
 * Through the role rather than by name, which is the property `compiler/intrinsic` exists to buy:
 * the library calls it `Array` and could call it anything, and what fastens the compiler to it is
 * the `@builtin` written where it is declared.
 *
 * The **declaration** as well as an instantiation, because this is asked of a written signature too:
 * a position spelled `Array` with no arguments is the generic declaration and has no `instanceOf` to
 * compare, and it is the same mistake to make there as at an instantiation.
 */
bool isSequenceType(DeclBase base, Program& program, DeclPtr<Type> type);

// What a sequence or a fixed array holds, or null for anything that is neither. The one question
// every site that reaches an element asks, and one answer for both spellings.
DeclPtr<Type> elementTypeOf(DeclBase base, Program& program, DeclPtr<Type> type);

/*
 * The **view table** - Analysis-Manifestations.md Part 8's one compiler-owned decision about a
 * representation record: which record an occurrence manifests as where it is not the type's own.
 *
 * A row pairs the role an *owner* claims with the role its view record claims, says which accesses
 * the view exists for, and names the class member that derives one from the owner - the adaptation
 * `adaptToView` inserts at the one site a value reaches a view position. Everything else about a
 * view is a declaration: its fields are the library's, its layout is `layoutRecord`'s, its
 * derivation is a body. What the compiler holds is this table, and the table is keyed by roles so
 * that it holds no name.
 *
 * `spelled` is the one way two rows differ in kind. An array's view is asked for by grammar - `[a]`
 * in a binding position - and `Array(a)` written by name is the owner; a string's has no spelling,
 * so a shared `String` in a binding position *is* the view and only `&s: String`, an owned one, or
 * the subject of `Viewed.view` is the owner. `resolveOccurrence` reads that column and nothing else
 * decides it.
 */
struct ViewRow {
    intrinsic::Known owner;
    intrinsic::Known view;

    bool shared;
    bool unique;
    bool spelled;

    // The class member that builds the view from the owner, per access - `Contiguous.elements`,
    // `Writable.elementsMut`, `Viewed.view`. `Count` where the access has no view.
    intrinsic::Known sharedAdaptor;
    intrinsic::Known uniqueAdaptor;
    U16 sharedMember;
    U16 uniqueMember;
};

// The row whose owner this type is an instantiation or the declaration of, or null. Asked with the
// logical type, never with a record that is itself a view.
const ViewRow* viewRowOf(DeclBase base, Program& program, DeclPtr<Type> type);

// Whether the type has a view record for this access - the question the adaptation and the
// occurrence reader ask before either does anything.
bool hasViewFor(DeclBase base, Program& program, DeclPtr<Type> type, Access access);

// Whether the record is a view record of some row - the reverse lookup, which is what a
// constructed value's conversion to the occurrence it manifests asks. Answers the row, or null.
const ViewRow* viewRowOfRecord(DeclBase base, Program& program, DeclPtr<Type> record);

/*
 * Whether a value occurring as `held` has to become a view before it can fill `position` - both
 * occurrences already **substituted**.
 *
 * The one predicate behind the adaptation a specialization needs and its generic body could not
 * write. `substitutedContract` turns a shared variable position into the view, so inside a body
 * compiled at `a := String` a *position* is two words while the storage a `->x: a` parameter or an
 * owning field holds is still the owner - and the generic had no concrete type to call a derivation
 * at. Asked here rather than at each site so that the reachability walk that must keep the
 * derivation and the conversion that emits it cannot come to disagree about when there is one.
 *
 * The reverse - a view where the owner is wanted - is not a derivation and answers false: nothing
 * reconstructs an owner out of two words, and the caller reports it.
 */
bool needsViewDerivation(DeclBase base, Program& program, const Occurrence& held,
                         const Occurrence& position);

// The row this role is the *view* of, or null where it is not a view role at all. The same reverse
// lookup one step earlier, for `checkDeclaredRoles`, which has a role in hand and no type yet.
const ViewRow* viewRowForView(intrinsic::Known role);

/*
 * Whether member `index` of a class claiming `role` is the derivation of a view - the one member
 * whose subject is the **owner** where every other occurrence of the type is the view.
 *
 * Read off the same table the adaptation calls through, so that "which member builds a view" is
 * stated once: a row naming `Contiguous.elements` as its shared adaptor is what makes that member's
 * subject the owner, and a row added with its derivation at some other member index needs no second
 * edit here.
 */
bool isDerivationMember(intrinsic::Known role, U32 index);

/*
 * What a donor at a view position holds - the element of a fixed array, of an owner, or of the view
 * record an owner manifests as.
 *
 * `elementTypeOf` answers the first two, and this is the third: an `ArrayView(I32)` in hand at an
 * `xs: [a]` position says `a` is `I32`, where the whole type against the whole type says nothing.
 * A view whose owner is not a container of anything answers null, so nothing here names a row.
 */
DeclPtr<Type> donorElementOf(DeclBase base, Program& program, DeclPtr<Type> type);

/*
 * How a binding position is spelled in source: the type, with the form written where the form has a
 * spelling of its own. A shared or unique view of an array is `[a]`, and everything else - the owner,
 * a view-less type, a string's view - is the type's name.
 *
 * For a diagnostic that tells an author what to write, and for nothing else. `typeText` is the
 * logical type and a form is not part of one, so a message that rendered the type alone would tell
 * the author to write `self: Array(a)` for a position that has to be written `self: [a]`, and the
 * two are the very thing the message is about.
 */
String bindingText(Context& context, Program& program, const Occurrence& occurrence);

/*
 * The record an occurrence manifests as where it is a view, instantiated at the owner's own
 * arguments, or null where the occurrence manifests as its own type.
 *
 * The one function both tiers read: field lookup in the resolver and in the MIR builder, the
 * field's address in the SSA converter, and the manifestation table. Keying on the whole occurrence
 * is what closes the hole where a window accepted the owner's field names - the type alone says
 * `Array(I32)`, and only the contract says which of its two records the bytes are.
 */
DeclPtr<Type> viewRecordOf(Context& context, Program& program, const Occurrence& occurrence);

// The record a field of this occurrence is looked up on: the view where there is one, else the
// type itself - which is null where that is not a record at all.
DeclPtr<Type> fieldRecordOf(Context& context, Program& program, const Occurrence& occurrence);

} // namespace mir
