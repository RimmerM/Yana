#pragma once

#include "body.h"
#include "gen.h"
#include "intrinsic/roles.h"
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
 * One argument of a signature.
 *
 * The occurrence is the whole of the calling convention at this level: `->x: T` is owned, `x: T` is
 * shared and call-bounded, `&x: T` is unique and call-bounded, and `x: a'T` is shared in published
 * slot `a`. A call records the mode on its operand from *this*, once, when the signature has been
 * selected - which is what makes a direct, class, dynamic and specialized call have the same local
 * representation (§5.6).
 */
struct Arg {
    StringId name {};
    Occurrence occurrence;
    LocationId source = kNullLocation;
    U32 index = 0;

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
 * Suspension and the generic cost are computed at this milestone - the first by the fixpoint below
 * and the second by `selectGenForms`, for the pass-order reason its accessor gives. The rest are
 * declared now because the alternative - a summary bolted on once several passes already have
 * private answers - is precisely what §4 of that document is a report on.
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
        Mutates,
        Consumes,
        Placement,
        GenCost,

        Count,
    };

    // Whether calling this may suspend the enclosing fiber. Inferred and published rather than
    // written: Analysis-Sharing.md §3.3 reverses the colouring decision, and the across-suspend rule
    // needs the answer on native whether or not JavaScript does.
    Fact<bool> suspension() const { return read(suspends, Which::Suspension); }
    void setSuspension(bool value, Knowledge how) { suspends = value; write(Which::Suspension, how); }

    // How much frame an activation needs, itself and everything it calls - `StackBound`. What a
    // stack depth check, a region reservation and an external iterator's child stack all need, and
    // what recursion is the boundary of.
    Fact<StackBound> stack() const { return read(stackBound, Which::Stack); }
    void setStack(StackBound value, Knowledge how) { stackBound = value; write(Which::Stack, how); }

    // Which arguments outlive the call - the retention half of the escape analysis, and what decides
    // whether a caller may hand over a frame-placed value *at that position*.
    Fact<ArgumentSet> retains() const { return read(retained, Which::Retains); }
    void setRetains(ArgumentSet value, Knowledge how) { retained = value; write(Which::Retains, how); }

    // Which arguments the callee writes through. Read by the overlap check, which may otherwise not
    // distinguish two `&` arguments where only one is written.
    Fact<ArgumentSet> mutates() const { return read(mutated, Which::Mutates); }
    void setMutates(ArgumentSet value, Knowledge how) { mutated = value; write(Which::Mutates, how); }

    // Which arguments the callee discharges - the linear-consumption fact. An owned argument a
    // callee consumes is one the caller may not drop, and one it does *not* consume on some path is
    // a leak the caller cannot see; per argument for the reason the two above are.
    Fact<ArgumentSet> consumes() const { return read(consumed, Which::Consumes); }
    void setConsumes(ArgumentSet value, Knowledge how) { consumed = value; write(Which::Consumes, how); }

    // The strongest placement any object this function creates needs - see `StorageClass`. The
    // published half of §10.4's lattice; a caller ranks its own answer against this one.
    Fact<StorageClass> placement() const { return read(placed, Which::Placement); }
    void setPlacement(StorageClass value, Knowledge how) { placed = value; write(Which::Placement, how); }

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
        states = U16((states & ~(U16(3) << shift)) | (U16(how) << shift));
    }

    /*
     * The values, largest first, with the confidence for all six in one word behind them.
     *
     * The arrangement this replaces was six `{value, Knowledge}` pairs, and a `Knowledge` after a
     * `U64` is a byte plus seven of padding: four such facts spent sixty-four bytes carrying
     * thirty-two of payload and four two-bit numbers. Two bits each in a `U16` is the same six
     * answers in one field, and it is private for the reason the accessors exist - a caller that
     * could reach the value without the confidence is the thing `Fact` was introduced to prevent.
     */
    StackBound stackBound;
    ArgumentSet retained;
    ArgumentSet mutated;
    ArgumentSet consumed;
    U16 states = 0;
    bool suspends = false;
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

static_assert(U32(Summary::Which::Count) * 2 <= 16, "`Summary::states` holds two bits per fact");

/*
 * Forty bytes: four eight-byte lattice values, the seven confidences in two, and four single bytes
 * that share the tail padding those two leave. Eighty was the size before the confidences were
 * gathered, and the difference was entirely padding - `Knowledge` is one byte and followed a `U64`
 * four times.
 *
 * A fact whose value does not fit in the tail is what would end this: `GenCost` is a byte precisely
 * so that adding it cost nothing, and the next one that is a `U64` adds eight.
 */
static_assert(sizeof(Summary) == 40, "the layout note above says where the forty bytes go");

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
 * written in the library - `prelude/Core/Core.yana` where the stage cannot yet read the real one -
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

    /*
     * `pub` - whether a module that imports this one may write the name.
     *
     * A **class member** takes the class's answer rather than carrying its own: `pub class` exports
     * the signatures it declares, and an instance member is reached by selection rather than by
     * name. So this is written by `declareSignature` for a plain `fn` and copied from the head for
     * a member, which is what keeps "who may name this" one question with one answer.
     */
    bool exported = false;

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
     * a range rather than a number because `Arg::def` exists: `fn open(path, mode = Read)` is one
     * declaration a call reaches with one argument or with two. `Program::addFunctionName` files it
     * under every arity in the range, so a call still selects exactly one callee by a hash lookup and
     * `doc/spec/overloading.md`'s one-function-per-name-and-arity rule is unchanged - what a second
     * declaration now collides with is a range instead of a point.
     *
     * Counted rather than stored, for `writtenArity`'s reason: a second field is a second answer,
     * and a signature's parameter list is the length somebody typed.
     */
    U32 requiredArity(DeclBase base) {
        U32 count = 0;
        for(U32 i = 0; i < U32(args.size()); i++) {
            if(i == continuationArg) continue;
            if(base[args.get(base, i)]->def.isSet()) continue;
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
    U16 classMember = 0;

    Summary summary;

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

    MirPtr<Body> mir = nullptr;
    SsaPtr<ssa::Body> ssa = nullptr;

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

    // Set for a function the program can reach. Reachability starts at the entry for a program and
    // at the exported declarations for a library, and nothing unreached is converted or lowered.
    bool reachable = false;

    /*
     * The implicit disposals this body places at types it does not know - `Disposal`.
     *
     * Empty on every function whose signature binds no type variable, which is almost all of them:
     * a concrete body's disposals are checked where they are placed, and there is nothing to defer.
     * Filled by the ownership analysis and read by `specializeFunction`, in that order - which the
     * pass order gives, since every body is analysed before the first call is resolved to a callee.
     */
    DeclList<Disposal, false> disposals;
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
     * Whether `classifyTypes` has run - see `classifyRecord`.
     *
     * What it separates is a record made *before* the declaration walk finished, which the pass will
     * reach, from one made after it by a type application in a body, which has to classify itself.
     * A flag rather than each producer knowing where in the pipeline it stands, because the producer
     * is one function and the two answers are the whole difference between them.
     */
    bool typesClassified = false;

    // Where the program starts. `main` where a module declares one; a synthesized entry once
    // top-level statements are implemented, since a program with initialization that started at
    // `main` would start after its own initialization.
    DeclPtr<Function> entry = nullptr;

    Module* addModule(StringId name);

    /*
     * The by-name indexes, keyed by module and name together.
     *
     * Here rather than on `Module` because a `HashMap` owns a heap buffer and `Module` lives in the
     * arena. One map for the whole program rather than one per module, since the module is part of
     * the key anyway and a per-module map would be a per-module allocation with the same lifetime.
     */
    /*
     * False where the name was already taken in this module. Answered rather than discarded, so that
     * a second declaration of one name is a diagnostic instead of a declaration nothing can reach.
     *
     * A function's key is its name **and its arity** - `doc/spec` calls it one plain function per
     * name, any arity - which is what lets `Num` declare a unary and a binary `-`. It is not
     * overloading on types: two functions of one name and one arity are still a redeclaration, so a
     * call still has exactly one callee before a single argument is looked at, and inference stays a
     * walk down a known signature rather than a search over candidates.
     *
     * The key is a *range* of arities rather than one - `Function::requiredArity`. A defaulted
     * position makes one declaration reachable at several counts, and filing it once per count is
     * what keeps the rule above intact where it would otherwise need a search: the lookup stays a
     * single hash read, and two declarations whose ranges overlap are still a redeclaration.
     *
     * `conflict` is filled with the arity that was already taken, so the message names the count the
     * two declarations actually collide at rather than either one's own length. Nothing is written
     * unless the whole range is free: a half-registered declaration would be reachable at the arities
     * that happened to be checked first.
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
     * rather than the whole type it was written at. Answering false from `addInstance` is what makes
     * two heads of one shape a diagnostic rather than one of two answers the teardown walk picks
     * between by hash order - and it is why coherence stays a property of a hash table now that
     * selection has become a match.
     *
     * `keyedInstance` is the narrowing step and not the answer: what it hands back still has to be
     * matched against the arguments, because `Eq(Array(a))` and `Eq(Array(I32))` share a key by
     * construction and only one of them serves `Array(Guard)`.
     */
    bool addInstance(DeclBase base, DeclPtr<Instance> instance);
    DeclPtr<Instance> keyedInstance(DeclBase base, DeclPtr<Class> klass, const DeclPtr<Type>* args,
                                    Size count) const;

    /*
     * The one head of this class that names no constructor in a deciding position, or null.
     *
     * `instance (TrivialCopy(a)) Copy(a)` matches every type, so it cannot be filed under one and is
     * held here instead. At most one per class: a second would be an overlap that only specificity
     * reasoning could decide between, and there is none here by design.
     */
    DeclPtr<Instance> openInstance(DeclPtr<Class> klass) const;

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

private:
    static U64 nameKey(DeclPtr<Module> module, StringId name);

    HashMap<U64, DeclPtr<Function>> functionsByName;

    // Every name any function in a module has, whatever its arity - `functionNamed`.
    HashMap<U64, DeclPtr<Function>> functionNames;

    // How many functions of a module have asked for each symbol text - `uniqueSymbol`.
    HashMap<U64, U32> symbolCounts;
    HashMap<U64, DeclPtr<Global>> globalsByName;
    HashMap<U64, DeclPtr<RecordType>> recordsByName;
    HashMap<U64, ConstructorRef> constructorsByName;
    HashMap<U64, DeclPtr<Alias>> aliasesByName;
    HashMap<U64, DeclPtr<Instance>> instancesByHead;
    HashMap<U32, DeclPtr<Instance>> openInstances;
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

} // namespace mir
