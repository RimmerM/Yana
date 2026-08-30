#pragma once

#include "body.h"

namespace ssa { struct Body; }

namespace mir {

struct Module;
struct Program;

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
 * One published fact and how much is known about it.
 *
 * The pair rather than a value beside a shared flag, so that a fact cannot be read without its
 * confidence: `fact.value` on its own does not compile into anything meaningful, and `fact.get(d)`
 * makes the caller name what it falls back to.
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

    void set(T next, Knowledge how) { value = next; knowledge = how; }
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
 * Only suspension is computed at this milestone. The rest are declared now because the alternative -
 * a summary bolted on once several passes already have private answers - is precisely what §4 of
 * that document is a report on.
 */
struct Summary {
    // Whether the body is available to analyse - false for an unresolved import, a dynamic target or
    // a foreign declaration. Distinct from every fact below: a foreign declaration has no body and
    // may still publish a settled suspension answer.
    bool bodyKnown = false;

    // Whether calling this may suspend the enclosing fiber. Inferred and published rather than
    // written: Analysis-Sharing.md §3.3 reverses the colouring decision, and the across-suspend rule
    // needs the answer on native whether or not JavaScript does.
    Fact<bool> suspension;

    // How much frame an activation needs, itself and everything it calls - `StackBound`. What a
    // stack depth check, a region reservation and an external iterator's child stack all need, and
    // what recursion is the boundary of.
    Fact<StackBound> stack;

    // Which arguments outlive the call - the retention half of the escape analysis, and what decides
    // whether a caller may hand over a frame-placed value *at that position*.
    Fact<ArgumentSet> retains;

    // Which arguments the callee writes through. Read by the overlap check, which may otherwise not
    // distinguish two `&` arguments where only one is written.
    Fact<ArgumentSet> mutates;

    // Which arguments the callee discharges - the linear-consumption fact. An owned argument a
    // callee consumes is one the caller may not drop, and one it does *not* consume on some path is
    // a leak the caller cannot see; per argument for the reason the two above are.
    Fact<ArgumentSet> consumes;

    // The strongest placement any object this function creates needs - see `StorageClass`. The
    // published half of §10.4's lattice; a caller ranks its own answer against this one.
    Fact<StorageClass> placement;

    /*
     * **The instantiation set is deliberately not a field yet**, and this is what it is waiting for.
     *
     * Every fact above is a lattice element that fits in a word, which is what lets a `Summary` live
     * in the declaration arena beside declarations that may own no heap memory. An instantiation set
     * is a *set of targets* - a function and the type arguments it was reached with - and it needs
     * storage the summary would have to own, in a region, keyed by an identity that does not exist:
     * what a target is is decided by milestone 6's substitution, and a shape chosen before that is a
     * shape the substitution then has to be bent to fit.
     *
     * So the commitment recorded here is where it goes rather than a placeholder for it: one more
     * `Fact` on this structure, joined by the same fixpoint, with a `RegionArray` in the declaration
     * region behind it. What it must not become is a table beside the summary that a second pass
     * fills, which is the arrangement §4 is a report on.
     */
};

/*
 * The ownership classes the compiler itself reaches for - `doc/spec/ownership.md`'s Teardown.
 *
 * **Builtin here, exactly as `Bool` is a builtin type**, and for the same reason: this stage does
 * not build Core, and a teardown has to be nameable before there is a library to declare one in.
 * Core declares `class Reclaim(a)` and `class Drop(a)` with one member each, and when this stage
 * reads Core these become two ordinary class declarations found by name rather than two enumerators.
 *
 * Two and not one. `Reclaim` releases the value's own storage and is elided wherever something else
 * reclaims it in bulk - a region, the JS collector; `Drop` runs an effect at last use and is never
 * elided anywhere. A single combined class makes a type region-ineligible for the sake of a release
 * the region was going to do anyway, which is the capability the split exists to keep.
 *
 * `Copy` is the third and is not a teardown. It is here because it *subtracts* the same thing the
 * other two do - `doc/spec/ownership.md`'s "an authored `Copy` subtracts `TrivialCopy`" - and so has
 * to be visible to `classifyTypes` on the same walk, before typeclass dispatch exists. Only the
 * subtraction is implemented at this milestone: the classifier reads the instance, and nothing yet
 * *calls* the member, because `copy(x)` is a class call and this stage has no solver above the
 * table. Recorded rather than deferred, because the ownership answer is what the rest of the stage
 * keys off and getting it wrong is silent - the very failure the rule exists to prevent.
 */
enum class CoreClass: U8 {
    Reclaim,
    Drop,
    Copy,

    Count,
};

StringView coreClassName(CoreClass klass);

/*
 * One `instance Drop(T)`, `instance Reclaim(T)` or `instance Copy(T)` the program wrote.
 *
 * **Concrete heads only**, which is the restriction that makes this a milestone-4 slice of
 * milestone 6 rather than a parallel mechanism: selection is a lookup keyed by the class and the
 * *logical type*, with no substitution and no context to discharge. That is already
 * Analysis-IR.md §11.4's "instance selection over logical types only", asked of a table with one
 * entry shape; what milestone 6 adds is a parametric head and a solver above it, over this table.
 *
 * The member is an ordinary `Function` with an ordinary body - it is called with the value it
 * releases, and everything from the argument mode down is the same machinery every other call uses.
 * A teardown that were a special kind of body would be a second call path for the one call the
 * compiler inserts rather than the author.
 *
 * `Copy`'s member is the same shape and is *not* a teardown: it borrows its subject and returns a
 * second value, so `Function::teardown` stays false for it and the drop elaboration never reaches
 * it. What it shares with the other two is the table, which is the only thing the classifier asks.
 */
struct Instance {
    CoreClass klass = CoreClass::Drop;
    DeclPtr<Type> type = nullptr;
    DeclPtr<Function> function = nullptr;
    LocationId source = kNullLocation;
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
    Function(StringId name, DeclPtr<Module> module): name(name), module(module) {}

    StringId name {};
    DeclPtr<Module> module;
    LocationId source = kNullLocation;

    DeclList<DeclPtr<Arg>> args;
    Occurrence result;

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
     * **The generic environment is not a field yet**, and this is where it goes.
     *
     * §10.4's region placement needs an *ambient region witness*: `StorageClass::Region` means "the
     * region the witness names", and the witness is a declared member of this function's environment
     * that a caller substitutes - a parameter, like a type parameter, arriving with the call. What it
     * must never be is something lowering synthesizes because it noticed an object outliving its
     * frame; that is a region invented at the point of use, and two uses invent two.
     *
     * It is not here because there is no environment to put it in: this stage has no generics, and
     * an environment schema chosen before substitution exists is a schema the substitution then has
     * to be bent around. What stands in for it meanwhile is a check rather than a comment -
     * `verifyBody` refuses an object placed in a region, so the first pass that selects that rung
     * without a witness reports instead of lowering something with nowhere to go.
     */
    Summary summary;

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
    // False where the name was already taken in this module. Answered rather than discarded, so that
    // a second declaration of one name is a diagnostic instead of a declaration nothing can reach.
    bool addFunctionName(DeclPtr<Module> module, StringId name, DeclPtr<Function> function);
    bool addGlobalName(DeclPtr<Module> module, StringId name, DeclPtr<Global> global);
    bool addRecordName(DeclPtr<Module> module, StringId name, DeclPtr<RecordType> record);

    DeclPtr<Function> findFunction(DeclPtr<Module> module, StringId name) const;
    DeclPtr<Global> findGlobal(DeclPtr<Module> module, StringId name) const;
    DeclPtr<RecordType> findRecord(DeclPtr<Module> module, StringId name) const;

    /*
     * The instance table - `Instance`, whose comment is why it is a lookup and not a solve.
     *
     * Keyed by the class and the logical type together. Answering false from `addInstance` is what
     * makes a second `instance Drop(T)` a diagnostic rather than one of two answers the teardown
     * walk picks between by hash order.
     */
    bool addInstance(DeclPtr<Instance> instance);
    DeclPtr<Instance> findInstance(CoreClass klass, DeclPtr<Type> type) const;

    // Releases the MIR region. Called once conversion has finished with every body, and it clears
    // each function's MIR handle first so that a stale one cannot be dereferenced afterwards.
    void releaseMir();

private:
    static U64 nameKey(DeclPtr<Module> module, StringId name);

    HashMap<U64, DeclPtr<Function>> functionsByName;
    HashMap<U64, DeclPtr<Global>> globalsByName;
    HashMap<U64, DeclPtr<RecordType>> recordsByName;
    HashMap<U64, DeclPtr<Instance>> instancesByType;
};

} // namespace mir
