#include "pass.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Bounds facts - a comparison a dominating branch arm already answers, decided (J189, P12's first
 * slice in `doc/inflight/Implementation-Streams.md`).
 *
 * What it is for is the check a finite decoder leaves behind its one length test:
 *
 *     if length(bytes) < 9 then return Err(Short)
 *     ... readBigU32(bytes, 5) ...        -- checkWordInBounds: 9 < 4 || 5 >u 9 - 4
 *     ... bytes[8] ...                     -- 8 >=u length(bytes)
 *
 * Every check behind the test compares a constant with the length the test already bounded, so each
 * comparison has one answer in the arm and this writes it down. `cleanCfg`, which runs immediately
 * behind this, folds the branch and deletes the refusal arm in the same round.
 *
 * ## Why this is not the `range` pass README.md measured twice
 *
 * `range` related a value to *itself* - a second `== 0` test, an extension pair - and fired zero
 * times because nothing in the corpus produces either shape. This relates *two* values: a length
 * bounded by an arm and a constant compared against it, which is the shape every subscript and every
 * endian reader writes. It needs the test and the check to read one `length` value, which a window
 * parameter's entry value gives for free and a local view's restatement needed `simplify`'s
 * aggregate-field rule to see through one more name for (J189 as well).
 *
 * ## The facts, and why they are written as they are
 *
 * A fact is `x + c <= y + d` in one domain, signed or unsigned, where `x` and `y` are values or zero
 * and `c`, `d` are constants - the plan's statement, so that a relational fact is a row of the same
 * table as a constant one rather than a second table. The constants are held as mathematical
 * integers rather than as bit patterns, which is what lets `x < y` be `x + 1 <= y` without a wrap.
 * Two questions read the table:
 *
 * - **Intervals** (J189): the facts about a value are turned into a signed and an unsigned interval,
 *   the two intervals of a comparison are compared, and a comparison whose intervals do not overlap
 *   is decided. This is the constant half - `len >= 9` in front of `readBigU32(b, 5)`.
 * - **Relations** (J190): each side of the comparison is normalized to `base + offset`, exact in the
 *   comparison's reading, and a fact relating the two bases bounds their difference. This is the
 *   half a decoder that walks a position needs - a check that passed leaves `at <=u len - 4`, and
 *   `at + 3 <u len` is the same two bases.
 *
 * **Both target widths, as `foldAtWidths` reads them.** A `Size` is 32 bits on one target and 64 on
 * another, so every question is asked twice, once at each width `foldableIntWidths` names, and a
 * comparison is decided only where the two answers agree. A value's interval depends on its width
 * (`len >=s 9` is `[9, 2^31)` at one and `[9, 2^63)` at the other), and a subtraction that cannot
 * wrap at one may wrap at the other - as may a conversion: a `U32` is a non-negative `Size` at 64
 * bits and not at 32.
 *
 * **Overflow wraps** (spec/types.md), and D10's rule is what follows from that and nothing else. An
 * addition is taken apart - `len - 4` read as `len + -4`, `at + 2` as `at + 2` - only where the
 * operand's own range says it stays inside the reading; otherwise it is a value of its own. So a
 * constant index under `len >= C` is accepted, and so is `at + j` under D10's `need(b, at, k)` -
 * `len >= k` and `at <=u len - k` bound `at` above by `len - k`, which is what keeps `at + j` from
 * wrapping. A bare `at + k <= len` is declined without a rule of its own: nothing bounds `at`, so
 * `at + k` is not `at` plus anything, and the fact relates a value no check reads.
 *
 * **Where `nonneg` comes from**, which is what carries a signed fact to an unsigned check - the
 * check `at >=u len` behind `while at < length(b)` is decided only where `at >= 0`. Three sources: a
 * fact (`at < 0` tested), a `convert` whose source fits (an owned array's `@bits(30)` `Count` length
 * as a `Size`), and a loop counter every edge into whose block carries a non-negative value
 * (`counterRange`, by induction over the loop's edges). A position the caller hands in untested has
 * none of the three and keeps its check, because a negative one passes the loop's test.
 *
 * ## Where a fact comes from
 *
 * A branch arm whose target has exactly one way in, which is `ConditionDecider`'s rule in `cfg.cpp`
 * and for its reason: "dominated by the target" is then "reached only through this arm". The walk is
 * the dominator tree with a scope per node, as `cse`'s is, so a fact is dropped when the subtree it
 * holds in is left. A comparison, an `and` known true, an `or` known false (the `||` of
 * `checkWordInBounds` where if-conversion has made it one) and a `not` each give their operands'
 * facts; so does a `Bool` block argument with one edge left that can carry the known answer (an
 * inlined `need(...) -> Bool`'s `&&`), with the arms that led to that edge. After each arm,
 * `deriveDifferences` turns the guarded-subtraction pair `len - at >= k`, `at <= len` into
 * `at + k <= len`.
 *
 * Operands are keyed with same-width reinterpretations peeled and, since J190, through a load of a
 * frame slot stored once (a `[shared call]` parameter's), which `forwardMemory` does not answer
 * across a block that writes the slot's neighbours.
 *
 * ## Subslices (J191)
 *
 * `slice`'s clamps are a chain of `select`s, each comparing what the one before chose. A `select`
 * whose condition this has just decided is replaced by its arm at once (`choose`), the relational
 * question takes one step through a third value (`from <= to <= len` answers `from > len`), and a
 * branch on a comparison `hoist` moved above the facts is answered at the branch (`decideBranch`).
 *
 * ## Loops (J199)
 *
 * B4 made the forms an ordinary loop is written in check-free here rather than only in the lower
 * tier's `eliminateBoundedChecks`, so JavaScript has them too. Five pieces, each stated where it is:
 *
 * - `counterRange` reads *every* edge into a counter's block as a range under the hypothesis, so a
 *   walk may start at a position behind `need` or `at >= 0`, and a counter carried between an outer
 *   and an inner loop is proven together with the other;
 * - `addCounterBounds` gives a counter stepping down its upper bound, `i <= n - 1`, as a fact of the
 *   loop header;
 * - `readOnlyField` makes every load of a shared call-bounded parameter's field one key - a
 *   JavaScript window's `length`, reloaded per block - and, since K3, a view parameter's at any
 *   origin and access, so a `&` window's length survives the stores into it;
 * - `arithmetic` and `proveThrough` take `16 * i + j` under `i < n / 16` apart, and let a signed fact
 *   answer an unsigned check where the checked value is non-negative;
 * - `unconverted` reads an owned array's length stored back as a `Count` as the number it was.
 *
 * What the lower tier still does alone is a counter whose step may wrap: `from = end + 1` where `end`
 * may be the length, since a window's length has no bound below `Size`'s top here.
 *
 * ## What it does not look for
 *
 * A *check*. `opt/check.cpp`'s structural rule - a branch whose arm is `call checkFailed` and a jump
 * - is the shape before inlining; by the time this runs `checkFailed` has been copied into the arm
 * and what stands there is `call abortProcess` and `unreachable`. More to the point, the answer to a
 * comparison does not depend on what either arm does, so recognizing the arm would restrict the pass
 * without making it any more correct. Every integer comparison under a fact is asked.
 */

namespace ssa {

namespace {

using Wide = __int128;

enum class Domain: U8 { Signed, Unsigned };

constexpr U32 kNone = ~U32(0);

// `x + c <= y + d`, read in `domain`. A null side is zero. `previousX` and `previousY` are the fact
// below this one with the same `x` (`y`), so the facts about one value are a chain (`factsAbout`).
struct Fact {
    Value* x = nullptr;
    Wide c = 0;
    Value* y = nullptr;
    Wide d = 0;
    Domain domain = Domain::Signed;
    U32 previousX = kNone;
    U32 previousY = kNone;
    U32 bases = 0;
};

struct Interval {
    Wide lo = 0;
    Wide hi = 0;
};

// Both readings of one value's bits at one width - kept together because each tightens the other: a
// signed interval that is non-negative is the same unsigned one, and the reverse.
struct Range {
    Interval s;
    Interval u;
};

/*
 * The firing counter, which is how README.md's phase 8 and phase 11 notes measured `range`: how many
 * comparisons were asked under at least one fact, and how many were decided. Debug builds only, and
 * printed at exit when `YANA_COUNT_BOUNDS` is set, because it is a measurement a slice takes rather
 * than something a build reports.
 */
#if defined(_DEBUG) || defined(DEBUG)
struct BoundsCounts {
    U64 facts = 0;
    U64 asked = 0;
    U64 decided = 0;

    ~BoundsCounts() {
        if(!getenv("YANA_COUNT_BOUNDS")) return;
        fprintf(stderr, "bounds: %llu facts, %llu comparisons asked, %llu decided\n",
                (unsigned long long)facts, (unsigned long long)asked, (unsigned long long)decided);
    }
};

BoundsCounts boundsCounts;
#define countBounds(field) (boundsCounts.field++)
#else
#define countBounds(field) ((void)0)
#endif

/*
 * How many steps the walk from a comparison to what bounds its operands may take. One step is a
 * fact about the operand itself with the fact's other side read structurally, which is all a
 * constant offset needs (J189). Two is what a relational fact needs: `at + 6 <=s len` bounds `len`
 * below only through what bounds `at`, and the guarded-subtraction form's `len >= 6` is exactly
 * that second step (J190). Each step multiplies the facts read per question, so it stays at what a
 * shape needs.
 */
constexpr U32 kFactDepth = 2;

// How far `normalize` peels `x + k` chains; a decoder's offsets are one addition deep.
constexpr U32 kOffsetDepth = 4;

// How many inductions may be open at once (`counterRange`): a counter, one carried into it from an
// enclosing loop, and one more. Each level reads every edge of its block under a fresh decider.
constexpr U32 kInductionDepth = 3;

/*
 * How many facts in scope before they are indexed and the answers remembered (`rangeOf`, `root`,
 * `normalized`, `edgesFrom`). Below it a question scans, which costs about what a lookup does, and
 * most deciders - one per edge of every induction - never get there. Measured in instructions
 * retired against never remembering: R3's 200 guard pairs 1,401G to 11.4G, and `test/base` +0.5%
 * on JavaScript and +0.1% natively, which 128 does not improve and costs `Many50` 16 times over.
 */
constexpr Size kRemembered = 32;

// How many projections deep a read-only parameter field is keyed (`readOnlyField`).
constexpr U32 kFieldDepth = 4;

// How deep `arithmetic` combines ranges and `proveThrough` rewrites a question (J199).
constexpr U32 kCombineDepth = 3;
constexpr U32 kProofDepth = 3;

struct BoundsDecider {
    OptContext& opt;
    Dominators& dominators;

    SmallArray<Fact, 16> facts;

    /*
     * What makes a question cost what its operands' facts cost rather than what every fact in scope
     * costs (R3: 200 guard pairs in one body took 29 s, three quarters of it in `rangeOf` reading
     * every fact for every operand of every fact, and `proveLe` normalizing every fact per question).
     * Once `kRemembered` facts are in scope:
     *
     * - the facts are indexed by each side's key and by every base their `x` may normalize to
     *   (`linkBases`), each a chain newest first, kept a stack so a pop unlinks;
     * - `rangeOf` and `normalized` remember their answers until `generation` moves - which it does
     *   whenever the facts or the body change - and `root` until the body does (`shape`);
     * - `proveLe` reads the edges out of the bases the question names (`edgesFrom`) rather than a
     *   table of every fact.
     *
     * The order a range reads its facts in is kept, since `tighten` is not indifferent to it.
     */
    HashMap<U64, U32> newestX;
    HashMap<U64, U32> newestY;
    U64 generation = 0;

    struct RangeMemo {
        U64 generation = 0;
        U8 bits = 0;
        Maybe<Range> range;
    };
    HashMap<U64, RangeMemo> ranges;

    // Every value a fact's `x` may normalize to, whatever the ranges say: `normalize`'s peeling with
    // its range tests left out. A stack beside the facts, each chaining to the one below it with the
    // same value, so `edgesFrom` reads the facts that can have an edge out of a base and no others.
    struct BaseLink {
        Value* value = nullptr;
        U32 fact = 0;
        U32 previous = kNone;
    };
    SmallArray<BaseLink, 16> bases;
    HashMap<U64, U32> newestBase;

    // The facts below this are linked into the three indices. They are linked once `kRemembered`
    // are in scope, and stay linked until popped; below it every question scans.
    U32 indexed = 0;

    // `root` answers from the body alone, so it is remembered until the body changes (`shape`);
    // `normalize` answers from the facts as well, so until they change.
    U64 shape = 0;
    struct RootMemo {
        U64 shape = 0;
        Value* root = nullptr;
    };
    HashMap<U64, RootMemo> roots;

    struct NormalMemo {
        U64 generation = 0;
        U8 bits = 0;
        Value* base = nullptr;
        Wide offset = 0;
    };
    HashMap<U64, NormalMemo> normals;

    // The joins whose joined condition is being read (`addJoinedCondition`); a fact naming a value
    // one of them dominates is not taken.
    SmallArray<U32, 4> joins;

    // Which of the two widths the question is being asked at: 0 for the guaranteed one, 1 for the
    // widest. A type that states its own width answers the same at both.
    U32 trial = 0;

    // The induction hypotheses while a counter's edges are read (see `counterRange`): the values
    // whose ranges are assumed non-negative rather than derived. More than one where the inductions
    // nest - a counter carried into an inner loop from an outer one whose back edge carries the inner
    // counter back out is two values proven together (J199).
    SmallArray<Value*, 4> assumed;
    U32 nesting = 0;

    // How many `arithmetic` combinations and `proveThrough` steps are open, each bounded.
    U32 combining = 0;
    U32 proving = 0;

    // A counter's range does not depend on the facts in scope - it is read at each edge into the
    // loop - so it is decided once per body and width. Few enough per body that a list is the map.
    struct Counter {
        Value* value = nullptr;
        U32 trial = 0;
        Maybe<Range> range;
    };
    SmallArray<Counter, 8> counters;

    Maybe<U8> bitsOf(Value* value) {
        auto widths = foldableIntWidths(opt, value->occurrence.type);
        if(!widths || widths.unwrap().wide > 64) return Nothing();
        return Just(trial == 0 ? widths.unwrap().narrow.bits : widths.unwrap().wide);
    }

    // The same bits at both widths, which is what makes a `bitcast` between the two a second name
    // for one number rather than a conversion - `Size` and `USize`, `I32` and `U32`.
    bool sameWidths(Value* a, Value* b) {
        auto first = foldableIntWidths(opt, a->occurrence.type);
        auto second = foldableIntWidths(opt, b->occurrence.type);
        if(!first || !second) return false;

        return first.unwrap().narrow.bits == second.unwrap().narrow.bits &&
               first.unwrap().wide == second.unwrap().wide;
    }

    /*
     * A value with its same-width reinterpretations peeled off. `(bitcast(n) :: USize)` is `n` read
     * in the other domain, and the facts carry their domain, so a fact about `n` is a fact about
     * the `bitcast` and the two are one key.
     */
    Value* root(Value* value) {
        if(!value) return nullptr;
        if(facts.size() < kRemembered) return computeRoot(value);

        auto key = U64(uintptr_t(value));
        if(auto memo = roots.get(key)) {
            if(memo.unwrap().shape == shape) return memo.unwrap().root;
        }

        auto found = computeRoot(value);
        roots.add(key, RootMemo { shape, found });
        return found;
    }

    Value* computeRoot(Value* value) {
        for(U32 i = 0; value && value->definer && i < 4; i++) {
            auto inst = opt.base[value->definer];

            if(inst->kind == Inst::Load) {
                if(auto stored = storedOnce(*(InstLoad*)inst)) {
                    value = stored;
                    continue;
                }

                if(auto read = readOnlyField(*(InstLoad*)inst)) return read;
                break;
            }

            if(inst->kind != Inst::Unary || ((InstUnary*)inst)->op != mir::UnaryOp::Bitcast) break;

            auto operand = valueOf(opt, ((InstUnary*)inst)->value);
            if(!operand || !sameWidths(value, operand)) break;
            value = operand;
        }

        return value;
    }

    /*
     * A field of a shared call-bounded parameter, reached through its projections and the
     * `Guaranteed` loads that restate the borrow, as one key per field for the whole body. Nothing
     * writes storage a shared call-bounded loan names while the call runs, so every such load reads
     * one number. This is a JavaScript window's `length`: the view arrives as one record rather than
     * as scalars, and each block that reads the length loads it again - the loop test in one block,
     * the subscript's check in the next - which `forwardMemory` does not unify across a loop's back
     * edge. Natively the window's length is an entry argument already (J199).
     */
    struct FieldKey {
        Value* parameter = nullptr;
        U16 fields[kFieldDepth] = {};
        U32 count = 0;
        Value* first = nullptr;
    };
    SmallArray<FieldKey, 8> fieldKeys;

    bool isFieldKey(Value* value) {
        for(auto& known: fieldKeys) {
            if(known.first == value) return true;
        }
        return false;
    }

    Value* readOnlyField(InstLoad& load) {
        if(load.isVolatile) return nullptr;

        auto read = soleResult(opt, load);
        if(!read || read->convention != Convention::Trivial || !foldableIntWidths(opt, read->occurrence.type)) {
            return nullptr;
        }

        FieldKey key;
        U16 reversed[kFieldDepth];

        auto address = valueOf(opt, load.address);
        for(U32 i = 0; address && i < kFieldDepth * 3; i++) {
            if(!address->definer) break;
            auto inst = opt.base[address->definer];

            if(inst->kind == Inst::FieldAddress) {
                auto& field = *(InstFieldAddress*)inst;
                if(key.count >= kFieldDepth || field.field == InstFieldAddress::kDiscriminant) return nullptr;
                reversed[key.count++] = field.field;
                address = valueOf(opt, field.base);
                continue;
            }

            auto result = soleResult(opt, *inst);
            if(inst->kind != Inst::Load || !result || result->convention != Convention::Guaranteed) return nullptr;
            address = valueOf(opt, ((InstLoad*)inst)->address);
        }

        if(!address || address->definer || !address->block || opt.base[address->block] != opt.base[opt.body->entry]) {
            return nullptr;
        }

        /*
         * Which parameters hold still. A shared call-bounded loan names storage nothing writes while
         * the call runs. A **view** holds still at any origin and under either access (K3): the
         * extent of a `[T]` cannot change through it (`AccessForm::View`, contract.h), so the
         * record's own fields - the base and the length - are one number each for the whole call
         * even where the elements are written. That is the `&space: '[U8]` of every codec kernel,
         * whose length was reloaded after each store into it or call that borrowed it and kept a
         * check per store (J200). Under `Unique` only the record's own fields: a path deeper than
         * one projection reads the elements, which the loan may replace.
         */
        auto contract = address->occurrence.contract;
        auto view = contract.form() == mir::AccessForm::View;
        if(contract.isOwned()) return nullptr;

        if(contract.access() == mir::Access::Shared) {
            if(contract.origin() != mir::Origin::CallBounded && !view) return nullptr;
        } else if(contract.access() != mir::Access::Unique || !view || key.count != 1) {
            return nullptr;
        }

        key.parameter = address;
        for(U32 i = 0; i < key.count; i++) key.fields[i] = reversed[key.count - 1 - i];

        for(auto& known: fieldKeys) {
            if(known.parameter != key.parameter || known.count != key.count) continue;

            bool same = true;
            for(U32 i = 0; i < key.count; i++) same = same && known.fields[i] == key.fields[i];
            if(same) return sameWidths(known.first, read) ? known.first : nullptr;
        }

        key.first = read;
        fieldKeys.push(key);
        return read;
    }

    /*
     * A load of a frame slot that holds one value for its whole life: one store, dominating the
     * load, and nothing else touching the slot but loads. A `[shared call]` parameter is kept in
     * such a slot - `promote` leaves it, since the slot is the place a borrow of the parameter names
     * - and `forwardMemory` answers only within a block, so the length test and a check in a later
     * block read `at` through two loads that nothing else makes one value. The first B2 probe's
     * `need(b, at, 6)` decoder kept both checks for that reason alone (J190).
     */
    Value* storedOnce(InstLoad& load) {
        if(load.isVolatile) return nullptr;

        auto address = valueOf(opt, load.address);
        if(!address || !address->definer || opt.base[address->definer]->kind != Inst::Alloc) return nullptr;

        InstStore* only = nullptr;
        for(auto pointer: address->uses(opt.base)) {
            auto user = opt.base[pointer];
            // A `Guaranteed` load is not a read: it names the storage, and a `represent` of that
            // name is an address a write can go through (FixedContainer's counter). Only plain
            // reads are allowed beside the one store.
            if(user->kind == Inst::Load && valueOf(opt, ((InstLoad*)user)->address) == address) {
                auto read = soleResult(opt, *user);
                if(!read || read->convention != Convention::Trivial) return nullptr;
                continue;
            }

            if(user->kind != Inst::Store || only) return nullptr;
            auto store = (InstStore*)user;
            if(store->isVolatile || valueOf(opt, store->address) != address) return nullptr;
            only = store;
        }

        if(!only) return nullptr;

        auto stored = valueOf(opt, only->value);
        if(!stored || !foldableIntWidths(opt, stored->occurrence.type)) return nullptr;
        if(!sameWidths(stored, soleResult(opt, load))) return nullptr;

        // The store reaches the load: an earlier block that dominates it, or earlier in the same one.
        auto from = opt.base[only->block];
        auto to = opt.base[load.block];
        if(from != to) {
            return dominators.dominates(from->index.index, to->index.index) ? stored : nullptr;
        }

        for(auto pointer: from->instructions(opt.base)) {
            auto inst = opt.base[pointer];
            if(inst == only) return stored;
            if(inst == &load) return nullptr;
        }

        return nullptr;
    }

    static Wide power(U8 bits) { return Wide(1) << bits; }

    static Range full(U8 bits) {
        return Range { Interval { -power(bits - 1), power(bits - 1) - 1 }, Interval { 0, power(bits) - 1 } };
    }

    static bool meet(Interval& into, Interval with) {
        if(with.lo > into.lo) into.lo = with.lo;
        if(with.hi < into.hi) into.hi = with.hi;
        return into.lo <= into.hi;
    }

    /*
     * Each reading tightened by the other. Twice, because a signed bound that moves the unsigned
     * interval may move the signed one back. False where the two readings leave nothing, which is
     * code the facts say cannot run - declined rather than decided, since nothing here needs it.
     */
    static bool tighten(Range& range, U8 bits) {
        auto largest = power(bits - 1) - 1;
        auto modulus = power(bits);

        for(U32 i = 0; i < 2; i++) {
            if(range.s.lo >= 0) {
                if(!meet(range.u, range.s)) return false;
            } else if(range.s.hi < 0) {
                if(!meet(range.u, Interval { range.s.lo + modulus, range.s.hi + modulus })) return false;
            }

            if(range.u.hi <= largest) {
                if(!meet(range.s, range.u)) return false;
            } else if(range.u.lo > largest) {
                if(!meet(range.s, Interval { range.u.lo - modulus, range.u.hi - modulus })) return false;
            }
        }

        return true;
    }

    static Interval& reading(Range& range, Domain domain) {
        return domain == Domain::Signed ? range.s : range.u;
    }

    static Interval reading(const Range& range, Domain domain) {
        return domain == Domain::Signed ? range.s : range.u;
    }

    /*
     * A constant added to every number in a range, with the wrap the language defines: each reading
     * moves as a whole where it stays inside its domain, moves by a whole turn where all of it
     * leaves, and is every number where it straddles the edge.
     */
    static Range shifted(Range range, Wide amount, U8 bits) {
        auto modulus = power(bits);
        auto low = -power(bits - 1);
        auto high = power(bits - 1) - 1;

        Wide signedAmount = amount % modulus;
        if(signedAmount > high) signedAmount -= modulus;
        if(signedAmount < low) signedAmount += modulus;

        auto result = full(bits);

        auto s = Interval { range.s.lo + signedAmount, range.s.hi + signedAmount };
        if(s.lo >= low && s.hi <= high) result.s = s;

        auto unsignedAmount = amount % modulus;
        if(unsignedAmount < 0) unsignedAmount += modulus;

        auto u = Interval { range.u.lo + unsignedAmount, range.u.hi + unsignedAmount };
        if(u.hi < modulus) result.u = u;
        else if(u.lo >= modulus) result.u = Interval { u.lo - modulus, u.hi - modulus };

        tighten(result, bits);
        return result;
    }

    // The number a constant holds, read at `bits`: the pattern's low bits, then each domain's
    // reading of them.
    static Range exact(U64 number, U8 bits) {
        auto mask = bits >= 64 ? ~U64(0) : (U64(1) << bits) - 1;
        Wide pattern = Wide(number & mask);
        Wide asSigned = pattern >= power(bits - 1) ? pattern - power(bits) : pattern;

        return Range { Interval { asSigned, asSigned }, Interval { pattern, pattern } };
    }

    /*
     * What a value's own definition says about it, at `bits`: an induction hypothesis, a constant, a
     * loop counter's induction (`counterRange`), a conversion, a reinterpretation, a constant added
     * or taken away, either arm of a `select`, and the two-operand arithmetic `arithmetic` combines.
     * Anything else is every number the width holds.
     */
    Maybe<Range> structural(Value* value, U8 bits, U32 depth) {
        for(auto each: assumed) {
            if(each == value) return Just(hypothesis(bits));
        }
        if(auto number = constantValueOf(opt, value)) return Just(exact(number.unwrap(), bits));

        if(!value->definer) {
            if(auto counter = counterRange(value, bits)) return counter;
            return Just(full(bits));
        }

        auto inst = opt.base[value->definer];

        if(inst->kind == Inst::Unary && ((InstUnary*)inst)->op == mir::UnaryOp::Convert) {
            return Just(converted(*(InstUnary*)inst, bits, depth));
        }

        if(inst->kind == Inst::Unary && ((InstUnary*)inst)->op == mir::UnaryOp::Bitcast) {
            auto operand = valueOf(opt, ((InstUnary*)inst)->value);
            if(operand && sameWidths(value, operand)) return rangeOf(operand, bits, depth);
        }

        /*
         * Either of two values: the smallest range holding both (K3). `if url then 64 else 0` is a
         * select, and what is added to a masked unit to choose an alphabet.
         */
        if(inst->kind == Inst::Select && combining < kCombineDepth) {
            auto& select = *(InstSelect*)inst;
            auto whenTrue = valueOf(opt, select.whenTrue);
            auto whenFalse = valueOf(opt, select.whenFalse);
            if(!whenTrue || !whenFalse || !sameWidths(value, whenTrue) || !sameWidths(value, whenFalse)) {
                return Just(full(bits));
            }

            combining++;
            auto a = rangeOf(whenTrue, bits, depth);
            auto b = rangeOf(whenFalse, bits, depth);
            combining--;
            if(!a || !b) return Just(full(bits));

            auto& p = a.unwrap();
            auto& q = b.unwrap();
            return Just(Range { Interval { p.s.lo < q.s.lo ? p.s.lo : q.s.lo, p.s.hi > q.s.hi ? p.s.hi : q.s.hi },
                                Interval { p.u.lo < q.u.lo ? p.u.lo : q.u.lo, p.u.hi > q.u.hi ? p.u.hi : q.u.hi } });
        }

        if(inst->kind == Inst::Binary) {
            auto& binary = *(InstBinary*)inst;
            auto lhs = valueOf(opt, binary.lhs);
            auto rhs = valueOf(opt, binary.rhs);

            if(lhs && rhs && (binary.op == mir::BinaryOp::Add || binary.op == mir::BinaryOp::Sub)) {
                auto right = constantValueOf(opt, rhs);
                auto left = constantValueOf(opt, lhs);

                // `x + k`, `x - k` and `k + x`; `k - x` negates and is not a shift.
                Value* moving = nullptr;
                Wide amount = 0;
                if(right) {
                    moving = lhs;
                    amount = exact(right.unwrap(), bits).s.lo;
                    if(binary.op == mir::BinaryOp::Sub) amount = -amount;
                } else if(left && binary.op == mir::BinaryOp::Add) {
                    moving = rhs;
                    amount = exact(left.unwrap(), bits).s.lo;
                }

                if(moving && sameWidths(value, moving)) {
                    // At the same depth: the operand is this side of the fact, one addition away.
                    auto inner = rangeOf(moving, bits, depth);
                    if(!inner) return Nothing();
                    return Just(shifted(inner.unwrap(), amount, bits));
                }
            }

            if(combining < kCombineDepth) {
                combining++;
                auto combined = arithmetic(binary, value, bits, depth);
                combining--;
                return Just(combined);
            }
        }

        return Just(full(bits));
    }

    /*
     * Two ranges combined, each reading on its own: a sum or difference of two values, a value
     * scaled by a constant, and a value divided by a positive constant. A reading keeps the result's
     * interval where every number of it is inside the reading - which is where the wrapping
     * operation and the mathematical one agree - and is every number where it is not.
     *
     * What wants it is the SIMD walk: `xs[16 * i + j]` under `i < n / 16` and `j < 16`, where the
     * relational question needs `16 * i + j` known not to wrap before it can take it apart (J199).
     * Bounded by `kCombineDepth`, since each level reads both operands' ranges.
     */
    Range arithmetic(InstBinary& binary, Value* value, U8 bits, U32 depth) {
        auto result = full(bits);

        auto lhs = valueOf(opt, binary.lhs);
        auto rhs = valueOf(opt, binary.rhs);
        if(!lhs || !rhs || !sameWidths(value, lhs)) return result;

        auto op = binary.op;
        auto scale = constantValueOf(opt, rhs);
        if(!scale && op == mir::BinaryOp::Mul) {
            if(auto left = constantValueOf(opt, lhs)) {
                scale = left;
                auto swap = lhs; lhs = rhs; rhs = swap;
            }
        }

        auto a = rangeOf(lhs, bits, depth);
        if(!a) return result;

        auto whole = full(bits);
        auto keep = [&](Interval& into, Interval computed, Interval bounds) {
            if(computed.lo >= bounds.lo && computed.hi <= bounds.hi) into = computed;
        };

        switch(op) {
            case mir::BinaryOp::Add:
            case mir::BinaryOp::Sub: {
                if(!sameWidths(value, rhs)) return result;
                auto b = rangeOf(rhs, bits, depth);
                if(!b) return result;

                auto& p = a.unwrap();
                auto& q = b.unwrap();
                if(op == mir::BinaryOp::Add) {
                    keep(result.s, Interval { p.s.lo + q.s.lo, p.s.hi + q.s.hi }, whole.s);
                    keep(result.u, Interval { p.u.lo + q.u.lo, p.u.hi + q.u.hi }, whole.u);
                } else {
                    keep(result.s, Interval { p.s.lo - q.s.hi, p.s.hi - q.s.lo }, whole.s);
                    keep(result.u, Interval { p.u.lo - q.u.hi, p.u.hi - q.u.lo }, whole.u);
                }
                break;
            }

            case mir::BinaryOp::Mul:
            case mir::BinaryOp::Shl: {
                auto k = scaleOf(op, scale, bits);
                if(!k) return result;

                auto& p = a.unwrap();
                keep(result.s, Interval { p.s.lo * k.unwrap(), p.s.hi * k.unwrap() }, whole.s);
                keep(result.u, Interval { p.u.lo * k.unwrap(), p.u.hi * k.unwrap() }, whole.u);
                break;
            }

            case mir::BinaryOp::Div: {
                if(!scale || !sameWidths(value, rhs)) return result;
                auto signedDivide = foldableIntWidths(opt, value->occurrence.type).unwrap().narrow.isSigned;
                auto& p = a.unwrap();

                if(signedDivide) {
                    auto k = exact(scale.unwrap(), bits).s.lo;
                    if(k <= 0) return result;
                    result.s = Interval { p.s.lo / k, p.s.hi / k };
                } else {
                    auto k = exact(scale.unwrap(), bits).u.lo;
                    if(k <= 0) return result;
                    result.u = Interval { p.u.lo / k, p.u.hi / k };
                }
                break;
            }

            /*
             * The three that bound a value from above whatever it was (K3): `x and m` is at most
             * `m` and at most `x` - read unsigned, where a mask is a number - `x shr k` is `x`
             * divided by `2^k`, and `x % n` for a non-negative `x` is below `n`. What wants them is
             * a table lookup, `table[(w shr 6) and 63]`, which J200's Base64 kernel had to write as
             * a byte-indexed table of 256 because this pass read no range for either.
             */
            case mir::BinaryOp::And: {
                auto b = rangeOf(rhs, bits, depth);
                if(!b) return result;

                auto hi = a.unwrap().u.hi < b.unwrap().u.hi ? a.unwrap().u.hi : b.unwrap().u.hi;
                result.u = Interval { 0, hi };
                break;
            }

            // Below the next power of two above either operand's largest, read unsigned.
            case mir::BinaryOp::Or: {
                auto b = rangeOf(rhs, bits, depth);
                if(!b) return result;

                auto largest = a.unwrap().u.hi > b.unwrap().u.hi ? a.unwrap().u.hi : b.unwrap().u.hi;
                Wide top = 1;
                while(top <= largest) top <<= 1;
                result.u = Interval { 0, top - 1 };
                break;
            }

            case mir::BinaryOp::Shr: {
                if(!scale) return result;
                auto k = exact(scale.unwrap(), bits).u.lo;
                if(k >= bits) return result;

                auto& p = a.unwrap();
                result.u = Interval { p.u.lo >> int(k), p.u.hi >> int(k) };
                break;
            }

            case mir::BinaryOp::Rem: {
                if(!sameWidths(value, rhs)) return result;
                auto b = rangeOf(rhs, bits, depth);
                if(!b) return result;

                auto& p = a.unwrap();
                auto& q = b.unwrap();
                auto signedRemainder = foldableIntWidths(opt, value->occurrence.type).unwrap().narrow.isSigned;

                if(signedRemainder) {
                    if(p.s.lo < 0 || q.s.lo < 1) return result;
                    result.s = Interval { 0, p.s.hi < q.s.hi - 1 ? p.s.hi : q.s.hi - 1 };
                } else {
                    if(q.u.lo < 1) return result;
                    result.u = Interval { 0, p.u.hi < q.u.hi - 1 ? p.u.hi : q.u.hi - 1 };
                }
                break;
            }

            default: break;
        }

        tighten(result, bits);
        return result;
    }

    // What a `mul` by a constant or a `shl` by one multiplies by, where that is a positive number.
    Maybe<Wide> scaleOf(mir::BinaryOp op, Maybe<U64> constant, U8 bits) {
        if(!constant) return Nothing();

        if(op == mir::BinaryOp::Shl) {
            auto shift = exact(constant.unwrap(), bits).u.lo;
            if(shift >= bits - 1) return Nothing();
            return Just(Wide(1) << shift);
        }

        auto k = exact(constant.unwrap(), bits).s.lo;
        if(k <= 0) return Nothing();
        return Just(k);
    }

    /*
     * An integer `convert`: the source's number, where it is a number of the result's type. The
     * conversion extends by the source's signedness and truncates to the result's width (`fold.cpp`'s
     * `foldConvert`), so a source whose interval in its own reading fits a reading of the result is
     * that interval there - and a reading it does not fit says nothing. This is where `nonneg` comes
     * from for an owned array's length: `self.length :: Size` converts a `@bits(30)` `Count`, which is
     * `[0, 2^30)` at both widths, so the `Size` is non-negative on either target. A `U32` converted to
     * `Size` is the case the width rule is for: non-negative at 64 bits, and possibly not at 32.
     */
    Range converted(InstUnary& convert, U8 bits, U32 depth) {
        auto result = full(bits);

        auto source = valueOf(opt, convert.value);
        if(!source) return result;

        auto facts = foldableIntWidths(opt, source->occurrence.type);
        auto width = bitsOf(source);
        if(!facts || !width) return result;

        auto inner = rangeOf(source, width.unwrap(), depth);
        if(!inner) return result;

        auto number = facts.unwrap().narrow.isSigned ? inner.unwrap().s : inner.unwrap().u;
        auto whole = full(bits);

        if(number.lo >= whole.s.lo && number.hi <= whole.s.hi) result.s = number;
        if(number.lo >= whole.u.lo && number.hi <= whole.u.hi) result.u = number;

        tighten(result, bits);
        return result;
    }

    /*
     * A loop counter's lower bound, by induction over the edges into its block: the counter is assumed
     * non-negative - the hypothesis the induction is allowed - and every value an edge carries is
     * then read **at the edge**, under the facts of the arms that dominate the edge's source, and must
     * be non-negative too. The counter's lower bound is the least of what the edges carry.
     *
     * Not the facts in scope where the question was asked: those hold at that point, and the value an
     * edge carries was computed at the edge. What this buys is the walker's own subscript - `at <s
     * len` answers `at >=u len` only where `at` is known non-negative.
     *
     * J190 accepted three shapes per edge - a non-negative constant, the counter, the counter plus a
     * non-negative constant - which is a walker from zero. Reading every edge's value as a range under
     * the hypothesis is the same rule for those three (`shifted` refuses a step that wraps) and also
     * takes what B4 asked for: a position the caller hands in behind `need` or `at >= 0`, a
     * `Count`-derived start, and a counter carried in from an outer loop. The last is a second
     * induction inside the first, and where the outer loop's back edge carries the inner counter out
     * again the two are proven together: each keeps its hypothesis while the other's edges are read.
     * A nested result is thrown away with the decider that computed it, since it holds only under the
     * hypotheses it was read with (J199).
     */
    Maybe<Range> counterRange(Value* value, U8 bits) {
        if(!value->block || nesting >= kInductionDepth) return Nothing();

        for(auto& counter: counters) {
            if(counter.value == value && counter.trial == trial) return counter.range;
        }

        auto range = inducted(value, bits);
        counters.push(Counter { value, trial, range });
        return range;
    }

    static Range hypothesis(U8 bits) {
        auto range = full(bits);
        range.s.lo = 0;
        tighten(range, bits);
        return range;
    }

    // Which argument of `block` `value` is, or nothing.
    Maybe<Size> argumentPosition(Block& block, Value* value) {
        for(Size i = 0; i < block.argumentCount(); i++) {
            if(opt.base[block.argumentAt(opt.base, i)] == value) return Just(i);
        }
        return Nothing();
    }

    // A decider reading block `index`'s facts at width `at`, with this one's hypotheses and `also`
    // added to them.
    void startAlong(BoundsDecider& inner, U32 index, Value* also, U32 at) {
        inner.trial = at;
        inner.nesting = nesting + 1;
        for(auto each: assumed) inner.assumed.push(each);
        if(also) inner.assumed.push(also);
        inner.collectAlong(index);
    }

    Maybe<Range> inducted(Value* value, U8 bits) {
        auto block = opt.base[value->block];
        if(block == opt.base[opt.body->entry] || !foldableIntWidths(opt, value->occurrence.type)) return Nothing();

        auto position = argumentPosition(*block, value);
        if(!position) return Nothing();

        auto whole = full(bits);
        Wide lowest = whole.s.hi;
        bool ok = block->predecessorCount() > 0;

        for(auto pointer: block->incoming(opt.base)) {
            if(!ok) break;

            auto from = opt.base[pointer];
            auto end = from->terminator(opt.base);
            if(!end) { ok = false; break; }

            forEachEdge(opt.base, *end, [&](Edge& edge) {
                if(!ok || !edge.target || opt.base[edge.target] != block) return;
                if(position.unwrap() >= edge.arguments.size()) { ok = false; return; }

                auto carried = valueOf(opt, edge.arguments.get(opt.base, position.unwrap()));
                if(!carried) { ok = false; return; }
                if(carried == value) return;

                // At the edge's source, under its own arms and the hypotheses.
                BoundsDecider inner { opt, dominators };
                startAlong(inner, from->index.index, value, trial);

                auto at = inner.rangeOf(carried, bits, 0);
                if(!at || at.unwrap().s.lo < 0) { ok = false; return; }
                if(at.unwrap().s.lo < lowest) lowest = at.unwrap().s.lo;
            });
        }

        if(!ok || lowest > whole.s.hi) return Nothing();

        auto range = whole;
        range.s.lo = lowest;
        if(!tighten(range, bits)) return Nothing();
        return Just(range);
    }

    /*
     * A counter's upper bound by the same induction, as a fact relating it to a value the loop does
     * not change: `for i in n downto 0` enters at `n - 1` and steps down, so `i <= n - 1` holds
     * wherever `i` does, and under `n <= length(xs)` that is the subscript's answer. The bound is
     * what an edge from outside the loop carries, normalized to `base + offset`; the hypothesis is
     * that fact, and every edge must carry a value the hypothesis and the edge's own arms keep under
     * it. The base must dominate the loop, so it is one number for the loop's whole run, and the
     * answer must be one fact at both widths.
     *
     * A counter stepping *up* has its upper bound from the loop's own test, which the walk already
     * holds in the body; this is for the ones whose test is their lower bound. Asked at a loop
     * header only - a block one of whose predecessors it dominates - and pushed as a fact of that
     * block, so it is dropped with the loop's subtree (J199).
     */
    void addCounterBounds(Block& block) {
        if(nesting || block.argumentCount() == 0) return;

        auto header = block.index.index;
        bool loop = false;
        for(auto pointer: block.incoming(opt.base)) {
            if(dominators.dominates(header, opt.base[pointer]->index.index)) loop = true;
        }
        if(!loop) return;

        for(Size i = 0; i < block.argumentCount(); i++) {
            auto value = opt.base[block.argumentAt(opt.base, i)];
            if(!value || !foldableIntWidths(opt, value->occurrence.type)) continue;
            addCounterBound(block, value, i);
        }
    }

    void addCounterBound(Block& block, Value* value, Size position) {
        auto widths = foldableIntWidths(opt, value->occurrence.type);
        auto domain = widths.unwrap().narrow.isSigned ? Domain::Signed : Domain::Unsigned;
        auto header = block.index.index;

        // The candidate: an entry edge's value, as `base + offset` at its edge, at both widths.
        Linear bound;
        bool found = false;
        bool ok = true;

        auto forEachCarried = [&](auto&& each) {
            for(auto pointer: block.incoming(opt.base)) {
                if(!ok) return;
                auto from = opt.base[pointer];
                auto end = from->terminator(opt.base);
                if(!end) { ok = false; return; }

                forEachEdge(opt.base, *end, [&](Edge& edge) {
                    if(!ok || !edge.target || opt.base[edge.target] != &block) return;
                    if(position >= edge.arguments.size()) { ok = false; return; }

                    auto carried = valueOf(opt, edge.arguments.get(opt.base, position));
                    if(!carried) { ok = false; return; }
                    if(carried != value) each(*from, carried);
                });
            }
        };

        forEachCarried([&](Block& from, Value* carried) {
            if(found || dominators.dominates(header, from.index.index)) return;

            for(U32 t = 0; t < 2 && ok; t++) {
                BoundsDecider inner { opt, dominators };
                startAlong(inner, from.index.index, nullptr, t);
                auto bits = inner.bitsOf(carried);
                if(!bits) { ok = false; break; }

                auto linear = inner.normalize(carried, domain, bits.unwrap());
                if(t == 0) bound = linear;
                else if(linear.base != bound.base || linear.offset != bound.offset) ok = false;
            }
            found = true;
        });

        if(!ok || !found || !bound.base) return;

        // One number for the loop's whole run: defined in a block that strictly dominates the header,
        // or a read-only parameter field, which is one number wherever it is loaded.
        auto base = root(bound.base);
        auto defined = base->definer ? opt.base[base->definer]->block : base->block;
        if(!defined) return;
        auto home = opt.base[defined]->index.index;
        if((home == header || !dominators.dominates(home, header)) && !isFieldKey(base)) return;

        forEachCarried([&](Block& from, Value* carried) {
            for(U32 t = 0; t < 2 && ok; t++) {
                BoundsDecider inner { opt, dominators };
                startAlong(inner, from.index.index, nullptr, t);
                inner.pushFact(value, 0, base, bound.offset, domain);

                auto bits = inner.bitsOf(carried);
                if(!bits || !inner.proveLe(carried, 0, base, bound.offset, domain, bits.unwrap())) ok = false;
            }
        });

        if(ok) pushFact(value, 0, base, bound.offset, domain);
    }

    // The facts the scoped walk would hold in block `index`: the arms along its dominator chain.
    void collectAlong(U32 index) {
        auto entry = opt.base[opt.body->entry]->index.index;

        for(U32 i = 0; i < 256; i++) {
            if(auto block = blockAt(opt, index)) enterBlock(*block);
            if(index == entry || !dominators.isReachable(index)) break;

            auto up = dominators.idom[index];
            if(up == index) break;
            index = up;
        }
    }

    /*
     * A value's range at `bits`: its definition, then every fact in scope that has it on one side,
     * with the other side read at one step further out. Nothing where the facts contradict each
     * other.
     */
    Maybe<Range> rangeOf(Value* value, U8 bits, U32 depth) {
        static_assert(kFactDepth < 8 && kCombineDepth < 16, "the memo key packs depth and combining");
        if(facts.size() < kRemembered) return computeRange(value, bits, depth);

        auto key = (U64(uintptr_t(value)) << 8) | (U64(trial & 1) << 7) | (U64(depth) << 4) | U64(combining);
        auto now = generation;

        if(auto memo = ranges.get(key)) {
            if(memo.unwrap().generation == now && memo.unwrap().bits == bits) return memo.unwrap().range;
        }

        auto range = computeRange(value, bits, depth);
        ranges.add(key, RangeMemo { now, bits, range });
        return range;
    }

    Maybe<Range> computeRange(Value* value, U8 bits, U32 depth) {
        auto made = structural(value, bits, depth);
        if(!made) return Nothing();

        auto range = made.unwrap();
        if(depth >= kFactDepth) return Just(range);

        auto key = root(value);

        SmallArray<U32, 16> about;
        factsAbout(key, about);

        for(auto index: about) {
            auto& fact = facts[index];

            // `value + c <= y + d`, so `value <= y.hi + d - c`.
            if(fact.x == key) {
                auto other = sideOf(fact.y, bits, depth + 1);
                if(!other) continue;

                auto& into = reading(range, fact.domain);
                auto bound = reading(other.unwrap(), fact.domain).hi + fact.d - fact.c;
                if(bound < into.hi) into.hi = bound;
                if(into.lo > into.hi || !tighten(range, bits)) return Nothing();
            }

            // `x + c <= value + d`, so `value >= x.lo + c - d`.
            if(fact.y == key) {
                auto other = sideOf(fact.x, bits, depth + 1);
                if(!other) continue;

                auto& into = reading(range, fact.domain);
                auto bound = reading(other.unwrap(), fact.domain).lo + fact.c - fact.d;
                if(bound > into.lo) into.lo = bound;
                if(into.lo > into.hi || !tighten(range, bits)) return Nothing();
            }
        }

        return Just(range);
    }

    Maybe<Range> sideOf(Value* side, U8 bits, U32 depth) {
        if(!side) return Just(Range { Interval { 0, 0 }, Interval { 0, 0 } });

        auto width = bitsOf(side);
        if(!width || width.unwrap() != bits) return Nothing();

        return rangeOf(side, bits, depth);
    }

    // --- The facts an arm gives.

    void pushFact(Value* x, Wide c, Value* y, Wide d, Domain domain) {
        Fact fact { x ? root(x) : nullptr, c, y ? root(y) : nullptr, d, domain };
        for(auto join: joins) {
            if(stale(fact.x, join) || stale(fact.y, join)) return;
        }

        facts.push(fact);
        if(facts.size() >= kRemembered) {
            for(; indexed < facts.size(); indexed++) {
                auto& each = facts[indexed];
                each.previousX = link(newestX, each.x, indexed);
                each.previousY = link(newestY, each.y, indexed);
                each.bases = linkBases(each.x, indexed);
            }
        }

        generation++;
        countBounds(facts);
    }

    void popFact() {
        auto& fact = facts[facts.size() - 1];
        if(indexed == facts.size()) {
            for(U32 i = 0; i < fact.bases; i++) {
                auto& base = bases[bases.size() - 1];
                unlink(newestBase, base.value, base.previous);
                bases.pop();
            }
            unlink(newestX, fact.x, fact.previousX);
            unlink(newestY, fact.y, fact.previousY);
            indexed--;
        }

        facts.pop();
        generation++;
    }

    U32 linkBases(Value* x, U32 index) {
        U32 count = 0;
        auto add = [&](Value* value) {
            bases.push(BaseLink { value, index, link(newestBase, value, U32(bases.size())) });
            count++;
        };

        if(!x || constantValueOf(opt, x)) return 0;
        auto v = x;
        add(v);

        for(U32 i = 0; i < kOffsetDepth && v->definer; i++) {
            if(isConvert(v)) {
                Value* w = v;
                for(U32 j = 0; j < 4 && w && isConvert(w); j++) {
                    w = valueOf(opt, ((InstUnary*)opt.base[w->definer])->value);
                    if(w && sameWidths(w, v)) break;
                }
                if(!w || !sameWidths(w, v)) break;
                v = root(w);
                add(v);
                continue;
            }

            auto inst = opt.base[v->definer];
            if(inst->kind != Inst::Binary) break;

            auto& binary = *(InstBinary*)inst;
            if(binary.op != mir::BinaryOp::Add && binary.op != mir::BinaryOp::Sub) break;

            auto lhs = valueOf(opt, binary.lhs);
            auto rhs = valueOf(opt, binary.rhs);
            if(!lhs || !rhs) break;

            Value* moving = nullptr;
            if(constantValueOf(opt, rhs)) moving = lhs;
            else if(constantValueOf(opt, lhs) && binary.op == mir::BinaryOp::Add) moving = rhs;
            if(!moving || !sameWidths(v, moving)) break;

            v = root(moving);
            add(v);
        }

        return count;
    }

    static U32 link(HashMap<U64, U32>& newest, Value* side, U32 index) {
        if(!side) return kNone;
        auto key = U64(uintptr_t(side));
        auto previous = newest.getValue(key);
        newest.add(key, index);
        return previous ? previous.unwrap() : kNone;
    }

    static void unlink(HashMap<U64, U32>& newest, Value* side, U32 previous) {
        if(!side) return;
        if(previous == kNone) newest.remove(U64(uintptr_t(side)));
        else newest.add(U64(uintptr_t(side)), previous);
    }

    // The facts with `key` on either side, oldest first - the order a scan of every fact meets them
    // in, which `tighten` is not indifferent to.
    void factsAbout(Value* key, SmallArray<U32, 16>& about) {
        if(facts.size() < kRemembered) {
            for(U32 i = 0; i < facts.size(); i++) {
                if(facts[i].x == key || facts[i].y == key) about.push(i);
            }
            return;
        }

        SmallArray<U32, 16> onX;
        SmallArray<U32, 16> onY;

        auto x = newestX.getValue(U64(uintptr_t(key)));
        for(auto i = x ? x.unwrap() : kNone; i != kNone; i = facts[i].previousX) onX.push(i);
        auto y = newestY.getValue(U64(uintptr_t(key)));
        for(auto i = y ? y.unwrap() : kNone; i != kNone; i = facts[i].previousY) onY.push(i);

        auto a = onX.size();
        auto b = onY.size();
        while(a || b) {
            if(a && b && onX[a - 1] == onY[b - 1]) {
                about.push(onX[--a]);
                b--;
            } else if(a && (!b || onX[a - 1] < onY[b - 1])) {
                about.push(onX[--a]);
            } else {
                about.push(onY[--b]);
            }
        }
    }

    /*
     * `lhs op rhs` is `holds`, in the domain the operands' type is compared in. A strict order is
     * written with the one, which is exact over the mathematical integers the facts are read as;
     * equality is two facts in each domain, since equal bits are equal in both readings. What `!=`
     * says is a hole rather than an interval, and it gives nothing.
     */
    void addComparison(InstBinary& binary, bool holds) {
        auto lhs = valueOf(opt, binary.lhs);
        auto rhs = valueOf(opt, binary.rhs);
        if(!lhs || !rhs) return;

        auto widths = foldableIntWidths(opt, lhs->occurrence.type);
        if(!widths || !sameWidths(lhs, rhs)) return;

        auto domain = widths.unwrap().narrow.isSigned ? Domain::Signed : Domain::Unsigned;

        auto op = binary.op;
        if(!holds) {
            switch(op) {
                case mir::BinaryOp::Lt: op = mir::BinaryOp::Ge; break;
                case mir::BinaryOp::Le: op = mir::BinaryOp::Gt; break;
                case mir::BinaryOp::Gt: op = mir::BinaryOp::Le; break;
                case mir::BinaryOp::Ge: op = mir::BinaryOp::Lt; break;
                case mir::BinaryOp::Eq: op = mir::BinaryOp::Ne; break;
                case mir::BinaryOp::Ne: op = mir::BinaryOp::Eq; break;
                default: return;
            }
        }

        switch(op) {
            case mir::BinaryOp::Lt: pushFact(lhs, 1, rhs, 0, domain); break;
            case mir::BinaryOp::Le: pushFact(lhs, 0, rhs, 0, domain); break;
            case mir::BinaryOp::Gt: pushFact(rhs, 1, lhs, 0, domain); break;
            case mir::BinaryOp::Ge: pushFact(rhs, 0, lhs, 0, domain); break;
            case mir::BinaryOp::Eq:
                for(auto each: {Domain::Signed, Domain::Unsigned}) {
                    pushFact(lhs, 0, rhs, 0, each);
                    pushFact(rhs, 0, lhs, 0, each);
                }
                break;
            default: break;
        }
    }

    // What a `Bool` known to be `holds` says, through the connectives that split into their
    // operands. Bounded, because a condition is a short expression and the walk is per arm.
    void addCondition(Value* condition, bool holds, U32 depth) {
        if(!condition || depth > 4) return;
        if(!condition->definer) {
            addJoinedCondition(condition, holds, depth);
            return;
        }

        auto inst = opt.base[condition->definer];

        if(inst->kind == Inst::Unary && ((InstUnary*)inst)->op == mir::UnaryOp::Not) {
            addCondition(valueOf(opt, ((InstUnary*)inst)->value), !holds, depth + 1);
            return;
        }

        if(inst->kind != Inst::Binary) return;
        auto& binary = *(InstBinary*)inst;

        if(mir::isComparison(binary.op)) {
            addComparison(binary, holds);
            return;
        }

        // `a && b` true and `a || b` false are each two facts; the other two arms are one or the
        // other and give neither.
        auto splits = (binary.op == mir::BinaryOp::And && holds) || (binary.op == mir::BinaryOp::Or && !holds);
        if(!splits || !isBool(condition)) return;

        addCondition(valueOf(opt, binary.lhs), holds, depth + 1);
        addCondition(valueOf(opt, binary.rhs), holds, depth + 1);
    }

    /*
     * A `Bool` that is a block argument - what an inlined `fn need(...) -> Bool = a && b` leaves, the
     * `&&` short-circuited into `False` on one edge and `b` on the other. Known `holds`, every edge
     * carrying the other constant is one control did not come by, and where exactly one edge is left
     * the join was entered over it: its value holds, and so do the arms that brought control to that
     * edge's source from the join's dominator (`a`, here). Several edges left are one or the other,
     * as the `||` arm is.
     *
     * The facts are about values as they were at the edge. Where the arm is, a value is the one the
     * edge saw unless its definition ran again in between - and a definition the join does not
     * dominate cannot: to reach the arm, which the join dominates, from anywhere past such a
     * definition, control passes the join again, and the last pass is the one the answer is about.
     * A definition the join *does* dominate - the join's own arguments, and everything in a loop the
     * join heads, whose back edge is the edge - may have run again, so a fact naming one is dropped
     * (`stale`). R3's StaleJoin was exactly that: `prev = v > 0` carried around a loop told the next
     * iteration's `if prev` that *its* `v` was positive, and `xs[v]` lost its check.
     */
    void addJoinedCondition(Value* condition, bool holds, U32 depth) {
        if(!condition->block || !isBool(condition)) return;

        auto join = opt.base[condition->block];
        if(join == opt.base[opt.body->entry]) return;

        Size position = ~Size(0);
        for(Size i = 0; i < join->argumentCount(); i++) {
            if(opt.base[join->argumentAt(opt.base, i)] == condition) position = i;
        }
        if(position == ~Size(0)) return;

        Block* source = nullptr;
        Value* carried = nullptr;
        bool ok = true;

        for(auto pointer: join->incoming(opt.base)) {
            auto from = opt.base[pointer];
            auto end = from->terminator(opt.base);
            if(!end) return;

            forEachEdge(opt.base, *end, [&](Edge& edge) {
                if(!ok || !edge.target || opt.base[edge.target] != join) return;
                if(position >= edge.arguments.size()) { ok = false; return; }

                auto value = valueOf(opt, edge.arguments.get(opt.base, position));
                if(!value) { ok = false; return; }

                if(auto number = constantValueOf(opt, value)) {
                    if((number.unwrap() != 0) == holds) ok = false;
                    return;
                }

                if(source) { ok = false; return; }
                source = from;
                carried = value;
            });

            if(!ok) return;
        }

        if(!source || !carried) return;

        auto top = join->index.index;
        joins.push(top);
        addCondition(carried, holds, depth + 1);

        auto index = source->index.index;
        for(U32 i = 0; i < 64 && dominators.isReachable(index) && !dominators.dominates(index, top); i++) {
            if(auto block = blockAt(opt, index)) addArm(*block, depth + 1);

            auto up = dominators.idom[index];
            if(up == index) break;
            index = up;
        }

        joins.pop();
    }

    // Whether `key` may name a different number where an arm below `join` is than it did at an edge
    // into `join`: its definition is `join` or below it. A constant is one number everywhere, and a
    // read-only parameter field one number for the whole call.
    bool stale(Value* key, U32 join) {
        if(!key || constantValueOf(opt, key) || isFieldKey(key)) return false;

        auto block = key->definer ? opt.base[key->definer]->block : key->block;
        if(!block) return true;
        return dominators.dominates(join, opt.base[block]->index.index);
    }

    bool isBool(Value* value) {
        auto facts = foldableInt(opt, value->occurrence.type);
        return facts && facts.unwrap().bits == 1;
    }

    // The arm into `block`, where it is the only way in, and what it gives with the facts in scope.
    void enterBlock(Block& block) {
        auto first = facts.size();
        addArm(block, 0);
        deriveDifferences(first);
    }

    void addArm(Block& block, U32 depth) {
        if(block.predecessorCount() != 1) return;

        Block* from = nullptr;
        for(auto pointer: block.incoming(opt.base)) from = opt.base[pointer];
        if(!from || from == &block) return;

        auto end = from->terminator(opt.base);
        if(!end || end->kind != Inst::Branch) return;

        auto& branch = (InstBranch&)*end;
        if(branch.whenTrue.target == branch.whenFalse.target) return;

        auto whenTrue = opt.base[branch.whenTrue.target] == &block;
        auto whenFalse = opt.base[branch.whenFalse.target] == &block;
        if(whenTrue == whenFalse) return;

        addCondition(valueOf(opt, branch.condition), whenTrue, depth);
    }

    /*
     * The guarded-subtraction form, turned into the relational row it states. `len - at >= k` is a
     * fact about the *difference*, a value no check reads; under `at <= len` in the same domain it
     * is `at + k <= len`, which is one the checks' own operands appear in. Sound on each reading and
     * at each width, which is why nothing here asks a width:
     *
     *  - **unsigned**: `at <=u len` puts the true difference in `[0, 2^n)`, where the subtraction
     *    does not wrap, so its reading is the difference;
     *  - **signed**: `at <=s len` puts it in `[0, 2^n)` as well, and a reading `>= k >= 0` excludes
     *    the half where the subtraction wrapped to a negative number. A negative `k` would not.
     *
     * Which is also why the signed spelling is not the unsigned one: `at <=s len` and
     * `len - at >=s 6` both hold for `at = -10, len = -2` - and for `at = -10` against any real
     * length - so the reads behind them keep their checks until something says `at >= 0`. D10's
     * `need(b, at, k)` shape tests `at` unsigned and has no such hole.
     *
     * Only between the facts `from` added and the ones already in scope, once; what it derives is an
     * order, never a difference, so it cannot feed itself.
     */
    void deriveDifferences(Size from) {
        auto end = facts.size();

        for(Size i = from; i < end; i++) {
            for(Size j = 0; j < end; j++) {
                derivePair(i, j);
                if(j < from) derivePair(j, i);
            }
        }
    }

    // `facts[bound]` a lower bound on a difference `u - v`, `facts[order]` saying `v <= u`.
    void derivePair(Size bound, Size order) {
        auto difference = facts[bound];
        auto ordered = facts[order];
        if(difference.domain != ordered.domain || !difference.y || !difference.y->definer) return;

        Wide k = difference.c - difference.d;
        if(difference.x) {
            auto number = smallConstant(difference.x);
            if(!number) return;
            k += number.unwrap();
        }

        auto inst = opt.base[difference.y->definer];
        if(inst->kind != Inst::Binary || ((InstBinary*)inst)->op != mir::BinaryOp::Sub) return;

        auto u = valueOf(opt, ((InstBinary*)inst)->lhs);
        auto v = valueOf(opt, ((InstBinary*)inst)->rhs);
        if(!u || !v || !sameWidths(u, difference.y) || !sameWidths(v, difference.y)) return;
        if(constantValueOf(opt, u) || constantValueOf(opt, v)) return;

        // `v + c <= u + d` is `v <= u` where `c >= d`.
        if(ordered.x != root(v) || ordered.y != root(u) || ordered.c < ordered.d) return;
        if(difference.domain == Domain::Signed && k < 0) return;

        pushFact(v, k, u, 0, difference.domain);
    }

    // A constant the same number at both widths and small enough to be one, or nothing.
    Maybe<Wide> smallConstant(Value* value) {
        auto number = constantValueOf(opt, value);
        if(!number) return Nothing();

        auto narrow = exact(number.unwrap(), 32).s.lo;
        auto wide = exact(number.unwrap(), 64).s.lo;
        if(narrow != wide) return Nothing();
        return Just(narrow);
    }

    // --- The relational rows.

    // A value as `base + offset`, exact in one reading: `offset` is a mathematical integer and the
    // base is null for a constant.
    struct Linear {
        Value* base = nullptr;
        Wide offset = 0;
    };

    /*
     * `v` read in `domain` at `bits`, with same-width reinterpretations and constant additions peeled
     * off wherever the addition does not wrap in that reading - which is asked of the operand's own
     * range, facts included, so `len - 9` is `len + -9` under `len >=u 9` and is itself anywhere else.
     * The constant is read signed, which is the same bits added in either reading: `x + (2^n - 2)`
     * and `x - 2` are one number modulo the width, and the range check is what says it is also the
     * same integer.
     */
    Linear normalized(Value* v, Domain domain, U8 bits) {
        if(!v) return Linear {};
        if(facts.size() < kRemembered) return normalize(v, domain, bits);

        auto key = (U64(uintptr_t(v)) << 8) | (U64(trial & 1) << 7) | (domain == Domain::Signed ? 0 : 1);
        auto now = generation;
        if(auto memo = normals.get(key)) {
            if(memo.unwrap().generation == now && memo.unwrap().bits == bits) {
                return Linear { memo.unwrap().base, memo.unwrap().offset };
            }
        }

        auto linear = normalize(v, domain, bits);
        normals.add(key, NormalMemo { now, bits, linear.base, linear.offset });
        return linear;
    }

    Linear normalize(Value* v, Domain domain, U8 bits) {
        if(!v) return Linear {};
        v = root(v);

        if(auto number = constantValueOf(opt, v)) {
            return Linear { nullptr, reading(exact(number.unwrap(), bits), domain).lo };
        }

        Wide offset = 0;
        for(U32 i = 0; i < kOffsetDepth && v->definer; i++) {
            if(auto same = unconverted(v, bits)) {
                v = root(same);
                continue;
            }

            auto inst = opt.base[v->definer];
            if(inst->kind != Inst::Binary) break;

            auto& binary = *(InstBinary*)inst;
            if(binary.op != mir::BinaryOp::Add && binary.op != mir::BinaryOp::Sub) break;

            auto lhs = valueOf(opt, binary.lhs);
            auto rhs = valueOf(opt, binary.rhs);
            if(!lhs || !rhs) break;

            Value* moving = nullptr;
            Wide amount = 0;
            if(auto right = constantValueOf(opt, rhs)) {
                moving = lhs;
                amount = exact(right.unwrap(), bits).s.lo;
                if(binary.op == mir::BinaryOp::Sub) amount = -amount;
            } else if(auto left = constantValueOf(opt, lhs); left && binary.op == mir::BinaryOp::Add) {
                moving = rhs;
                amount = exact(left.unwrap(), bits).s.lo;
            }

            if(!moving || !sameWidths(v, moving)) break;

            auto inner = rangeOf(moving, bits, 0);
            if(!inner) break;

            auto whole = reading(full(bits), domain);
            auto held = reading(inner.unwrap(), domain);
            if(held.lo + amount < whole.lo || held.hi + amount > whole.hi) break;

            offset += amount;
            v = root(moving);
        }

        return Linear { v, offset };
    }

    /*
     * A chain of integer conversions that hands back the number it started from, as that number's
     * value - where it ends at the width it began at. Each step extends by its source's signedness and
     * truncates to its result's width (`foldConvert`), so where the number is non-negative and fits
     * every type along the way, every step is the identity; and where it is also below `2^(n-1)`
     * both readings of the two ends are that number. An owned array's length stored back as a
     * `Count` is the shape: `growBytes` writes `truncate(count + more) :: Count` and the endian
     * writer behind it reads `self.length :: Size`, which is `count + more` under the capacity test
     * that has just passed (J193's two checks per appender, J199).
     */
    Value* unconverted(Value* v, U8 bits) {
        if(!isConvert(v)) return nullptr;

        SmallArray<Value*, 4> chain;
        Value* w = v;
        for(U32 i = 0; i < 4 && isConvert(w); i++) {
            chain.push(w);
            w = valueOf(opt, ((InstUnary*)opt.base[w->definer])->value);
            if(!w) return nullptr;
            if(sameWidths(w, v)) break;
        }

        if(!sameWidths(w, v)) return nullptr;

        auto facts = foldableIntWidths(opt, w->occurrence.type);
        auto width = bitsOf(w);
        if(!facts || !width) return nullptr;

        auto range = rangeOf(w, width.unwrap(), 0);
        if(!range) return nullptr;
        auto number = facts.unwrap().narrow.isSigned ? range.unwrap().s : range.unwrap().u;
        if(number.lo < 0 || number.hi >= power(bits - 1)) return nullptr;

        for(auto step: chain) {
            auto type = foldableIntWidths(opt, step->occurrence.type);
            auto stepBits = bitsOf(step);
            if(!type || !stepBits) return nullptr;
            auto top = type.unwrap().narrow.isSigned ? power(stepBits.unwrap() - 1) : power(stepBits.unwrap());
            if(number.hi >= top) return nullptr;
        }

        return w;
    }

    bool isConvert(Value* v) {
        if(!v->definer) return false;
        auto inst = opt.base[v->definer];
        return inst->kind == Inst::Unary && ((InstUnary*)inst)->op == mir::UnaryOp::Convert;
    }

    // Whether `v`'s two readings are one number at `bits`: its range is inside `[0, 2^(n-1))`, where
    // a fact in either domain is a fact in the other.
    bool oneReading(Value* v, U8 bits) {
        if(!v) return true;
        auto range = rangeOf(v, bits, 0);
        return range && range.unwrap().s.lo >= 0 && range.unwrap().u.hi <= power(bits - 1) - 1;
    }

    /*
     * `a + ca <= b + cb` in `domain`, proven. The two sides are normalized; one base on both sides is
     * arithmetic; otherwise a fact relating the two bases is looked for, in the question's domain or -
     * where both bases have one reading - in the other. What a fact gives is a bound on
     * `base(a) - base(b)`, and the question asks for one at least as tight.
     *
     * This is the half of D10 that decides a check behind a passed one: `checkWordInBounds(b, at, 9)`
     * leaves `len >=s 9` and `at <=u len - 9`, and a read of `at + 5` asks `at + 5 <=u len - 4`, which
     * is the same two bases four apart. A sum of two variables is taken apart only where its range says
     * it does not wrap - `normalize` peels a constant under that test, and `proveThrough`'s bounded
     * addend a variable - so `at + k <= len` over an `at` nothing bounds stays one value of its own.
     */
    bool proveLe(Value* a, Wide ca, Value* b, Wide cb, Domain domain, U8 bits) {
        auto left = normalize(a, domain, bits);
        auto right = normalize(b, domain, bits);

        auto need = right.offset + cb - left.offset - ca;
        if(left.base == right.base) return 0 <= need;

        if(left.base && proving < kProofDepth) {
            proving++;
            auto through = proveThrough(a, left, ca, b, cb, domain, bits);
            proving--;
            if(through) return true;
        }

        if(!left.base || !right.base) return false;

        auto anyFrom = [&](Value* from, auto&& each) { return edgesFrom(from, bits, each); };

        auto usable = [&](const FactEdge& edge, Value* between) {
            if(edge.domain == domain) return true;
            return oneReading(left.base, bits) && oneReading(right.base, bits) && oneReading(between, bits);
        };

        auto direct = anyFrom(left.base, [&](FactEdge& edge) {
            return edge.to == right.base && usable(edge, nullptr) && edge.bound <= need;
        });
        if(direct) return true;

        /*
         * One step through a third value, which is what a subslice's clamps ask: `from <= to` and
         * `to <= len` are two facts, and the clamp `from > len` is neither of them. Both edges are read
         * in the question's domain, or - where a fact is in the other one - with all three bases in
         * `[0, 2^(n-1))`, the same condition a single cross-domain fact has. One step, because a chain
         * is `slice`'s two bounds and nothing observed wanted a longer one (J191).
         */
        return anyFrom(left.base, [&](FactEdge& first) {
            if(first.to == right.base) return false;

            return anyFrom(first.to, [&](FactEdge& second) {
                return second.to == right.base && usable(first, first.to) && usable(second, first.to) &&
                       first.bound + second.bound <= need;
            });
        });
    }

    // Each fact as a bound on the difference of its two bases: `x + c <= y + d` is
    // `x.base - y.base <= y.offset + d - x.offset - c`.
    struct FactEdge {
        Value* from = nullptr;
        Value* to = nullptr;
        Wide bound = 0;
        Domain domain = Domain::Signed;
        bool fact = false;
    };

    /*
     * Each edge out of `from` at `bits`, until `each` answers: the facts whose `x` may normalize to
     * `from` (`linkBases`), normalized now, and the two edges of a minimum. What a scan of every fact
     * found, found through the bases a question names - which is what made a question cost every
     * fact in scope times every fact its operands' ranges read (R3's 29 s). A minimum's edges are
     * true of any `select` of its shape, so they are offered wherever one is asked from.
     */
    template<class Each>
    bool edgesFrom(Value* from, U8 bits, Each&& each) {
        auto edgeOf = [&](Fact& fact) {
            auto x = normalized(fact.x, fact.domain, bits);
            if(x.base != from) return false;

            auto y = normalized(fact.y, fact.domain, bits);
            if(!y.base || y.base == from) return false;

            FactEdge edge { from, y.base, y.offset + fact.d - x.offset - fact.c, fact.domain, true };
            return each(edge);
        };

        if(facts.size() < kRemembered) {
            for(U32 i = 0; i < facts.size(); i++) {
                if(edgeOf(facts[i])) return true;
            }
        } else {
            auto head = newestBase.getValue(U64(uintptr_t(from)));
            for(auto i = head ? head.unwrap() : kNone; i != kNone; i = bases[i].previous) {
                if(edgeOf(facts[bases[i].fact])) return true;
            }
        }

        SmallArray<FactEdge, 2> minimum;
        addMinimum(minimum, from, bits);
        for(auto& edge: minimum) {
            if(each(edge)) return true;
        }

        return false;
    }

    /*
     * A `select` that is a minimum - `select(a <= b, a, b)`, what `min` becomes once if-converted -
     * is at most either operand, which is two edges no fact states. `for k in 0..min(length(space),
     * length(block) - at)` writing `space[k]` needs the first of them (J199).
     */
    template<class Edges>
    void addMinimum(Edges& edges, Value* v, U8 bits) {
        if(!v || !v->definer || opt.base[v->definer]->kind != Inst::Select) return;
        auto& select = *(InstSelect*)opt.base[v->definer];

        auto condition = valueOf(opt, select.condition);
        auto whenTrue = valueOf(opt, select.whenTrue);
        auto whenFalse = valueOf(opt, select.whenFalse);
        if(!condition || !whenTrue || !whenFalse || !condition->definer) return;
        if(opt.base[condition->definer]->kind != Inst::Binary) return;

        auto& compare = *(InstBinary*)opt.base[condition->definer];
        if(compare.op != mir::BinaryOp::Le && compare.op != mir::BinaryOp::Lt) return;
        if(valueOf(opt, compare.lhs) != whenTrue || valueOf(opt, compare.rhs) != whenFalse) return;
        if(!sameWidths(v, whenTrue) || !sameWidths(v, whenFalse)) return;

        auto widths = foldableIntWidths(opt, whenTrue->occurrence.type);
        auto domain = widths.unwrap().narrow.isSigned ? Domain::Signed : Domain::Unsigned;

        for(auto operand: {whenTrue, whenFalse}) {
            auto to = normalize(operand, domain, bits);
            if(to.base && to.base != v) edges.push(FactEdge { v, to.base, to.offset, domain });
        }
    }

    /*
     * The question asked again of something it implies, where the facts do not answer it directly.
     * Three rewrites, each exact where it applies:
     *
     *  - **unsigned as signed**: `a + ca <=u b + cb` holds where `a` is non-negative and the same
     *    holds signed, since `a` then reads one number and `b`'s unsigned reading is never below its
     *    signed one. The facts a walker has are signed and its checks unsigned, and this is the
     *    bridge that needs nothing known about `b`;
     *  - **a bounded addend**: `p + q` with `q` in `[lo, hi]` and the sum not wrapping is at most
     *    `p + hi` - `16 * i + j` with `j < 16`;
     *  - **a scaled counter under a quotient**: `k * x` with `x <= n / k + d` is at most
     *    `n + k * d`, where `k * (n / k) <= n` - read as the division reads, always unsigned and signed
     *    where `n >= 0` or the quotient is at least one; read the other way, only where `n >= 0`.
     *    `for i in 0..n / 16` reading `16 * i + j` is the shape.
     */
    bool proveThrough(Value* a, Linear left, Wide ca, Value* b, Wide cb, Domain domain, U8 bits) {
        if(domain == Domain::Unsigned) {
            auto range = rangeOf(a, bits, 0);
            if(range && range.unwrap().s.lo >= 0 && proveLe(a, ca, b, cb, Domain::Signed, bits)) return true;
        }

        auto base = left.base;
        if(!base->definer || opt.base[base->definer]->kind != Inst::Binary) return false;
        auto& binary = *(InstBinary*)opt.base[base->definer];

        auto lhs = valueOf(opt, binary.lhs);
        auto rhs = valueOf(opt, binary.rhs);
        if(!lhs || !rhs || !sameWidths(base, lhs)) return false;

        auto whole = reading(full(bits), domain);

        if(binary.op == mir::BinaryOp::Add && sameWidths(base, rhs) &&
           !constantValueOf(opt, lhs) && !constantValueOf(opt, rhs)) {
            for(U32 order = 0; order < 2; order++) {
                auto p = order ? rhs : lhs;
                auto q = order ? lhs : rhs;

                auto pr = rangeOf(p, bits, 0);
                auto qr = rangeOf(q, bits, 0);
                if(!pr || !qr) continue;

                auto held = reading(pr.unwrap(), domain);
                auto added = reading(qr.unwrap(), domain);
                if(held.lo + added.lo < whole.lo || held.hi + added.hi > whole.hi) continue;

                if(proveLe(p, left.offset + ca + added.hi, b, cb, domain, bits)) return true;
            }
            return false;
        }

        if(binary.op != mir::BinaryOp::Mul && binary.op != mir::BinaryOp::Shl) return false;

        auto x = lhs;
        auto scale = constantValueOf(opt, rhs);
        if(!scale && binary.op == mir::BinaryOp::Mul) {
            scale = constantValueOf(opt, lhs);
            x = rhs;
        }

        auto k = scaleOf(binary.op, scale, bits);
        if(!k || !sameWidths(base, x)) return false;

        // `k * x` is the mathematical product: `x`'s whole range scaled stays in the reading.
        auto xr = rangeOf(x, bits, 0);
        if(!xr) return false;
        auto held = reading(xr.unwrap(), domain);
        if(held.lo * k.unwrap() < whole.lo || held.hi * k.unwrap() > whole.hi) return false;

        auto signedType = foldableIntWidths(opt, base->occurrence.type).unwrap().narrow.isSigned;
        if(signedType != (domain == Domain::Signed)) return false;

        return edgesFrom(root(x), bits, [&](FactEdge& edge) {
            // A fact `x <= D + slack`, the edge's bound being the slack.
            if(!edge.fact || edge.domain != domain || !edge.to->definer) return false;

            auto slack = edge.bound;
            Linear to { edge.to, 0 };

            auto quotient = opt.base[to.base->definer];
            if(quotient->kind != Inst::Binary || ((InstBinary*)quotient)->op != mir::BinaryOp::Div) return false;

            auto n = valueOf(opt, ((InstBinary*)quotient)->lhs);
            auto divisor = constantValueOf(opt, valueOf(opt, ((InstBinary*)quotient)->rhs));
            if(!n || !divisor || !sameWidths(n, to.base)) return false;
            if(reading(exact(divisor.unwrap(), bits), domain).lo != k.unwrap()) return false;

            /*
             * `k * (n / k) <= n` is a property of the division in its own reading. Where the
             * question reads the other one, the two agree only where `n` is non-negative - a signed
             * `-32 / 16` read unsigned is near the top and `16` times it is not below `-32` read
             * unsigned (R3's SignedQuotient), and an unsigned quotient of a top-bit `n` is positive
             * while `n` read signed is negative. Where the readings match, unsigned always holds and
             * signed holds where `n >= 0` or the quotient is at least one, which truncation towards
             * zero makes the same thing.
             */
            auto divideSigned = foldableIntWidths(opt, to.base->occurrence.type).unwrap().narrow.isSigned;
            if(divideSigned != (domain == Domain::Signed) || domain == Domain::Signed) {
                auto nr = rangeOf(n, bits, 0);
                auto nonNegative = nr && nr.unwrap().s.lo >= 0;

                if(divideSigned != (domain == Domain::Signed)) {
                    if(!nonNegative) return false;
                } else {
                    auto qr = rangeOf(to.base, bits, 0);
                    if(!nonNegative && !(qr && qr.unwrap().s.lo >= 1)) return false;
                }
            }

            return proveLe(n, left.offset + ca + k.unwrap() * slack, b, cb, domain, bits);
        });
    }

    Maybe<bool> decideRelation(mir::BinaryOp op, Value* lhs, Value* rhs, Domain domain, U8 bits) {
        auto le = [&](Value* a, Wide ca, Value* b, Wide cb) { return proveLe(a, ca, b, cb, domain, bits); };

        switch(op) {
            case mir::BinaryOp::Lt: if(le(lhs, 1, rhs, 0)) return Just(true); if(le(rhs, 0, lhs, 0)) return Just(false); break;
            case mir::BinaryOp::Le: if(le(lhs, 0, rhs, 0)) return Just(true); if(le(rhs, 1, lhs, 0)) return Just(false); break;
            case mir::BinaryOp::Gt: if(le(rhs, 1, lhs, 0)) return Just(true); if(le(lhs, 0, rhs, 0)) return Just(false); break;
            case mir::BinaryOp::Ge: if(le(rhs, 0, lhs, 0)) return Just(true); if(le(lhs, 1, rhs, 0)) return Just(false); break;
            case mir::BinaryOp::Eq: if(le(lhs, 1, rhs, 0) || le(rhs, 1, lhs, 0)) return Just(false); break;
            case mir::BinaryOp::Ne: if(le(lhs, 1, rhs, 0) || le(rhs, 1, lhs, 0)) return Just(true); break;
            default: break;
        }

        return Nothing();
    }

    // --- The question, at one width.

    Maybe<bool> decideAt(mir::BinaryOp op, Value* lhs, Value* rhs, Domain domain) {
        auto bits = bitsOf(lhs);
        if(!bits) return Nothing();

        auto left = rangeOf(lhs, bits.unwrap(), 0);
        auto right = rangeOf(rhs, bits.unwrap(), 0);
        if(!left || !right) return Nothing();

        auto a = reading(left.unwrap(), domain);
        auto b = reading(right.unwrap(), domain);

        auto disjoint = [&](Interval p, Interval q) { return p.hi < q.lo || q.hi < p.lo; };
        auto intervals = decideIntervals(op, a, b, disjoint(left.unwrap().s, right.unwrap().s) ||
                                                  disjoint(left.unwrap().u, right.unwrap().u));
        if(intervals) return intervals;

        return decideRelation(op, lhs, rhs, domain, bits.unwrap());
    }

    static Maybe<bool> decideIntervals(mir::BinaryOp op, Interval a, Interval b, bool apart) {
        switch(op) {
            case mir::BinaryOp::Lt: if(a.hi < b.lo) return Just(true); if(a.lo >= b.hi) return Just(false); break;
            case mir::BinaryOp::Le: if(a.hi <= b.lo) return Just(true); if(a.lo > b.hi) return Just(false); break;
            case mir::BinaryOp::Gt: if(a.lo > b.hi) return Just(true); if(a.hi <= b.lo) return Just(false); break;
            case mir::BinaryOp::Ge: if(a.lo >= b.hi) return Just(true); if(a.hi < b.lo) return Just(false); break;
            case mir::BinaryOp::Eq: if(apart) return Just(false); break;
            case mir::BinaryOp::Ne: if(apart) return Just(true); break;
            default: break;
        }

        return Nothing();
    }

    // The comparison's answer under the facts in scope, where both widths give one.
    Maybe<bool> answer(InstBinary& binary) {
        auto lhs = valueOf(opt, binary.lhs);
        auto rhs = valueOf(opt, binary.rhs);
        auto widths = foldableIntWidths(opt, lhs->occurrence.type);
        auto domain = widths.unwrap().narrow.isSigned ? Domain::Signed : Domain::Unsigned;

        countBounds(asked);

        trial = 0;
        auto narrow = decideAt(binary.op, lhs, rhs, domain);
        trial = 1;
        auto wide = decideAt(binary.op, lhs, rhs, domain);
        trial = 0;

        if(!narrow || !wide || narrow.unwrap() != wide.unwrap()) return Nothing();
        return narrow;
    }

    /*
     * A branch on a comparison computed in a block above - one `hoist` took out of a loop because
     * its operands are invariant, which puts it above the loop's own test. The readers' `len <
     * width` half of `checkWordInBounds` is the shape: invariant in a decoder's loop, hoisted to the
     * entry, and asked there under no facts at all, while the branch that reads it sits behind
     * `need(b, at, 9)`. The comparison stays where it is, since its other readers may be anywhere;
     * this branch's condition is answered where the branch is (J191).
     */
    bool decideBranch(Block& block) {
        auto end = block.terminator(opt.base);
        if(!end || end->kind != Inst::Branch) return false;

        auto& branch = (InstBranch&)*end;
        auto condition = valueOf(opt, branch.condition);
        if(!condition || !condition->definer || opt.base[condition->block] == &block) return false;

        auto inst = opt.base[condition->definer];
        if(inst->kind != Inst::Binary || !mir::isComparison(((InstBinary*)inst)->op)) return false;

        auto& binary = *(InstBinary*)inst;
        auto lhs = valueOf(opt, binary.lhs);
        auto rhs = valueOf(opt, binary.rhs);
        if(!lhs || !rhs || !sameWidths(lhs, rhs)) return false;
        if(constantValueOf(opt, lhs) && constantValueOf(opt, rhs)) return false;

        auto decided = answer(binary);
        if(!decided) return false;

        auto constant = makeConstant(opt, *end, condition->occurrence.type, decided.unwrap() ? 1 : 0);
        if(!constant) return false;

        opt.ir().replaceUsesIn(*end, *condition, *constant);
        generation++;
        shape++;
        countBounds(decided);
        return true;
    }

    /*
     * One comparison, decided where both widths give the same answer - `foldAtWidths`' agreement
     * rule, for the reason `IntWidths` states. Two constants are the folder's and are left to it.
     */
    bool decide(Inst& inst) {
        if(inst.kind == Inst::Select) return choose((InstSelect&)inst);
        if(inst.kind != Inst::Binary) return false;

        auto& binary = (InstBinary&)inst;
        if(!mir::isComparison(binary.op)) return false;

        auto lhs = valueOf(opt, binary.lhs);
        auto rhs = valueOf(opt, binary.rhs);
        if(!lhs || !rhs || !sameWidths(lhs, rhs)) return false;
        if(constantValueOf(opt, lhs) && constantValueOf(opt, rhs)) return false;

        auto result = soleResult(opt, inst);
        if(!result || !result->useCount()) return false;

        auto decided = answer(binary);
        if(!decided) return false;

        auto constant = makeConstant(opt, inst, result->occurrence.type, decided.unwrap() ? 1 : 0);
        if(!constant) return false;

        auto editor = opt.ir();
        editor.replaceAllUses(*result, *constant);
        editor.eraseInst(inst);
        generation++;
        shape++;

        countBounds(decided);
        return true;
    }

    /*
     * A `select` whose condition this pass has just decided, replaced by the arm it picks - in the
     * pass rather than left to `simplify`, because what reads it is the next comparison of the same
     * chain. `slice`'s clamps are four in a row, each comparing what the one before it chose
     * (`start = from < 0 ? 0 : from`, then `start > length`), and if-conversion has made each a
     * `select`. Left for the next round, the chain costs a round per clamp, and `kMaxRounds` is four.
     * Integers only: they are what the facts are about, and an integer's occurrence is its type, so
     * the arm and the result are interchangeable where a pointer's two occurrences might not be.
     */
    bool choose(InstSelect& select) {
        auto result = soleResult(opt, select);
        if(!result || !result->useCount() || result->convention != Convention::Trivial) return false;
        if(!foldableIntWidths(opt, result->occurrence.type)) return false;

        auto condition = constantValueOf(opt, valueOf(opt, select.condition));
        if(!condition) return false;

        auto chosen = valueOf(opt, condition.unwrap() ? select.whenTrue : select.whenFalse);
        if(!chosen || chosen->convention != Convention::Trivial) return false;

        auto editor = opt.ir();
        editor.replaceAllUses(*result, *chosen);
        editor.eraseInst(select);
        generation++;
        shape++;
        return true;
    }

    void visit(U32 index) {
        auto block = blockAt(opt, index);
        if(!block || block->deleted) return;

        enterBlock(*block);
        addCounterBounds(*block);

        // Over a snapshot, because a decision inserts its constant into this same list.
        SmallArray<Inst*, 32> insts;

        /*
         * With no facts in scope, only a comparison whose operand bounds itself - `(w shr 6) and 63`
         * against a table's 64 at a function's entry (K3). Everything else would be asked to answer
         * from full ranges, which is the whole pass's cost for nothing, so the rest waits for a fact.
         */
        if(!facts.size()) {
            for(auto pointer: block->instructions(opt.base)) {
                auto inst = opt.base[pointer];
                if(inst->kind == Inst::Binary && mir::isComparison(((InstBinary*)inst)->op) &&
                   (boundsItself(valueOf(opt, ((InstBinary*)inst)->lhs)) ||
                    boundsItself(valueOf(opt, ((InstBinary*)inst)->rhs)))) {
                    insts.push(inst);
                }
            }

            for(auto inst: insts) decide(*inst);
            return;
        }

        for(auto pointer: block->instructions(opt.base)) insts.push(opt.base[pointer]);

        for(auto inst: insts) decide(*inst);
        decideBranch(*block);
    }

    // A value whose own definition bounds it below its type's top - a mask, a logical shift or a
    // remainder, under any reinterpretation or widening (`converted` carries the range across one).
    bool boundsItself(Value* value) {
        for(U32 i = 0; value && value->definer && i < 4; i++) {
            auto inst = opt.base[value->definer];

            if(inst->kind == Inst::Unary && (((InstUnary*)inst)->op == mir::UnaryOp::Bitcast ||
                                             ((InstUnary*)inst)->op == mir::UnaryOp::Convert)) {
                value = valueOf(opt, ((InstUnary*)inst)->value);
                continue;
            }

            if(inst->kind != Inst::Binary) return false;
            auto op = ((InstBinary*)inst)->op;
            if(op == mir::BinaryOp::And || op == mir::BinaryOp::Shr || op == mir::BinaryOp::Rem) return true;

            // A bounded value moved by one: `(w and 63) + offset`, `(w and 63) or base`.
            if(op != mir::BinaryOp::Add && op != mir::BinaryOp::Or) return false;
            value = valueOf(opt, ((InstBinary*)inst)->lhs);
        }

        return false;
    }

    // The dominator tree, carrying the facts of the arms on the path from the entry - `cse`'s walk,
    // and for its reason an explicit stack.
    void run() {
        struct Frame {
            U32 index = 0;
            U32 cursor = 0;
            Size facts = 0;
        };

        auto entry = opt.base[opt.body->entry]->index.index;
        if(!dominators.isReachable(entry)) return;

        SmallArray<Frame, 32> stack;

        auto push = [&](U32 index) {
            Frame frame;
            frame.index = index;
            frame.facts = facts.size();
            stack.push(frame);
            visit(index);
        };

        push(entry);

        while(stack.size()) {
            auto& frame = stack[stack.size() - 1];
            auto first = dominators.childStart[frame.index];
            auto last = dominators.childStart[frame.index + 1];

            if(first + frame.cursor < last) {
                auto child = dominators.childList[first + frame.cursor++];
                push(child);
                continue;
            }

            while(facts.size() > frame.facts) popFact();
            stack.pop();
        }
    }
};

} // namespace

void decideBoundedComparisons(OptContext& opt) {
    BoundsDecider decider { opt, dominatorsOf(opt) };
    decider.run();
}

} // namespace ssa
