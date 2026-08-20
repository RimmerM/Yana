#include "opt_pass.h"

/*
 * The two passes that need to know where a value is *available* rather than only what it is.
 *
 * Common-subexpression elimination replaces a computation with an earlier one, which is only sound
 * where the earlier one has definitely happened - so it walks the dominator tree with a scope per
 * node, and everything still in scope dominates the instruction being looked at. Dead-value
 * elimination needs no such thing and is here because it is what the other two leave work for.
 *
 * The dominator tree itself is in opt_flow.cpp, which the loop pass shares.
 *
 * ## Three things are eliminated, and only one of them is arithmetic
 *
 * A pure computation is the easy case and the one this pass started as: nothing can disturb `x * n`,
 * so an earlier one that dominates is an answer wherever the later one stands.
 *
 * A **load** is the case that pays, and it is the whole of §9.2 of test/bench/findings.md. The
 * arithmetic under a subscript is `mul`s and `add`s over values *read out of locals*, so two
 * spellings of one index are two `mul`s of two distinct `load %row`s - and comparing operands by
 * identity says they are different. Nothing above this unified them: `forwardPlaces` answers a
 * second read of a place from the first, and stops at the end of the block; `promotePlaces`
 * deliberately declines a whole scalar local, because the phi form it would produce is worse than
 * the `var` JS already has. So `out[row * n + column] = out[row * n + column] + ...` compiled the
 * index three times, and on the native target - where the bounds check is inlined into a branch, and
 * the branch cuts the block in three - block-local forwarding could not reach any of it.
 *
 * What a load needs and a computation does not is a reason to believe the storage still holds what
 * it held. That is `writesUnknownStorage` and `placesMayAlias`, asked not of the instructions in
 * front of this one but of **every block that could run between the two loads** - see `killBetween`.
 *
 * A **check** is the third, and it is the same §9.2 sentence read at the other end: two checks of one
 * index against one length are one check. `checkCondition(c)` returns only when `c` is false, and `c`
 * is an SSA value that nothing can change, so a second call of it with the same operand cannot do
 * anything the first did not already do. It needs no storage reasoning at all for that reason, which
 * is why it sits with the pure computations rather than with the loads.
 *
 * On a target that keeps the check a call - JS - that removes the check itself. Natively the inliner
 * has already turned it into a branch by the time this runs, and the branch stays: what it tests is
 * the same value twice, and folding the second away needs the abort arm to be known not to come back.
 * The IR has no way to say that today. See §9.2 of findings.md.
 */

namespace {

/*
 * Whether two operands are the same value.
 *
 * Identity, and one thing beside it: two constants of one type holding one number are one value
 * however many times the program spelled it. Constants are *not* interned - `addConstant` in
 * resolve/builder.h makes a fresh one per site, which is what lets each carry its own source
 * location - so the two `4`s in `xs[i] = xs[i] + 1` are two values, and comparing operands by
 * identity alone says the two `mul %i, 4`s under them are two computations.
 *
 * The comparison is on the stored representation rather than on the number, which is what keeps the
 * two floating cases honest: `-0.0` and `0.0` compare equal as doubles and are not the same value,
 * and a NaN compares equal to nothing including itself.
 */
bool sameOperand(OptContext& opt, ModulePtr<Value> first, ModulePtr<Value> second) {
    if(first == second) return true;
    if(!first || !second) return false;

    auto& a = *opt.local[first];
    auto& b = *opt.local[second];
    if(a.kind != b.kind || a.type != b.type) return false;

    // The stored bits of a float, which is what "the same constant" means for one - see above.
    auto bitsOf = [](F64 value) { union { F64 f; U64 bits; } punned = { value }; return punned.bits; };

    switch(a.kind) {
        case Value::ConstInt:    return ((ConstInt&)a).value == ((ConstInt&)b).value;
        case Value::ConstFloat:  return bitsOf(((ConstFloat&)a).value) == bitsOf(((ConstFloat&)b).value);
        case Value::ConstDouble: return bitsOf(((ConstDouble&)a).value) == bitsOf(((ConstDouble&)b).value);
        default:                 return false;
    }
}

/*
 * Whether two pure instructions compute the same thing.
 *
 * Operands are compared as SSA values - see `sameOperand` - which is what makes this an equality
 * rather than a congruence: two instructions with equal-but-distinct operands are left alone, and
 * the fixed point catches them on the next round once the operands themselves have been unified.
 *
 * Everything a kind carries beside its operands has to be compared here. A kind whose extra state is
 * not listed would have two instructions declared equal on their operands alone - which is why the
 * default is to decline rather than to accept.
 */
bool sameComputation(OptContext& opt, Value& a, Value& b) {
    if(a.kind != b.kind || a.type != b.type) return false;

    auto same = [&](ModulePtr<Value> x, ModulePtr<Value> y) { return sameOperand(opt, x, y); };

    switch(a.kind) {
        case Value::Cast: case Value::Bitcast: case Value::Neg: case Value::Not:
        case Value::Sqrt: case Value::Abs:
        case Value::Trunc: case Value::Floor: case Value::Ceil: case Value::Round:
            return same(((InstUnary&)a).from, ((InstUnary&)b).from);

        // Three operands and no state beside them. Commutative in `a` and `b` alone: the product is,
        // and the addend is not one of the pair.
        case Value::Fma: {
            auto& x = (InstFma&)a;
            auto& y = (InstFma&)b;

            return same(x.c, y.c)
                && ((same(x.a, y.a) && same(x.b, y.b)) || (same(x.a, y.b) && same(x.b, y.a)));
        }
        // Commutative, so the operands are compared as a pair rather than in order. The folder
        // already moves a constant to the right, which settles the common case before this is
        // asked; this is what catches `x + y` against a `y + x` neither of whose operands is one.
        case Value::Add: case Value::Mul:
        case Value::And: case Value::Or: case Value::Xor:
            if(same(((InstBinary&)a).lhs, ((InstBinary&)b).rhs) &&
               same(((InstBinary&)a).rhs, ((InstBinary&)b).lhs)) {
                return true;
            }
            [[fallthrough]];
        case Value::Sub: case Value::Div: case Value::Rem:
        case Value::Shl: case Value::Shr: case Value::Sar:
            return same(((InstBinary&)a).lhs, ((InstBinary&)b).lhs) &&
                   same(((InstBinary&)a).rhs, ((InstBinary&)b).rhs);
        case Value::Cmp:
            return same(((InstCmp&)a).lhs, ((InstCmp&)b).lhs) &&
                   same(((InstCmp&)a).rhs, ((InstCmp&)b).rhs) &&
                   ((InstCmp&)a).cmp == ((InstCmp&)b).cmp;
        // Not commutative in any useful sense: swapping the arms is only the same instruction with
        // the condition inverted, and there is no inversion to compare against here.
        case Value::Select:
            return same(((InstSelect&)a).cond, ((InstSelect&)b).cond) &&
                   same(((InstSelect&)a).whenTrue, ((InstSelect&)b).whenTrue) &&
                   same(((InstSelect&)a).whenFalse, ((InstSelect&)b).whenFalse);
        /*
         * The vector kinds. Every one of them carries state beside its operands - a lane index, a
         * shuffle pattern, which reduction - and this is the switch where a kind whose extra state
         * went uncompared would have two different instructions declared equal.
         *
         * `isRepeatable` already answers yes for all five, from the purity column in inst.def, so
         * they were CSE candidates the moment they existed and the arm below is what makes that
         * sound rather than what makes it happen.
         */
        case Value::VecSplat:
            return same(((InstVecSplat&)a).from, ((InstVecSplat&)b).from);
        case Value::VecLane:
        case Value::VecWithLane:
            return same(((InstVecLane&)a).from, ((InstVecLane&)b).from) &&
                   same(((InstVecLane&)a).value, ((InstVecLane&)b).value) &&
                   ((InstVecLane&)a).lane == ((InstVecLane&)b).lane;
        case Value::VecShuffle: {
            auto& first = (InstVecShuffle&)a;
            auto& second = (InstVecShuffle&)b;

            if(!same(first.left, second.left) || !same(first.right, second.right)) return false;
            if(first.pattern.size() != second.pattern.size()) return false;

            for(Size i = 0; i < first.pattern.size(); i++) {
                if(first.pattern[i] != second.pattern[i]) return false;
            }

            return true;
        }
        case Value::VecReduce:
            return same(((InstVecReduce&)a).from, ((InstVecReduce&)b).from) &&
                   ((InstVecReduce&)a).reduce == ((InstVecReduce&)b).reduce;
        case Value::TypeMetric:
            return ((InstTypeMetric&)a).of == ((InstTypeMetric&)b).of &&
                   ((InstTypeMetric&)a).metric == ((InstTypeMetric&)b).metric;
        case Value::Symbol:
            return ((InstSymbol&)a).callee == ((InstSymbol&)b).callee &&
                   ((InstSymbol&)a).global == ((InstSymbol&)b).global;

        /*
         * And the compiler's own check, which is not pure and is idempotent - see the header. Its
         * one operand decides it completely: `isCheckCall` has already established that the callee
         * reads that flag and touches nothing else.
         *
         * By identity rather than through `sameOperand`, and this is the one place that matters: a
         * check whose flag folded to a constant is a check that already knows its answer, and
         * `isDischargedCheck` removes the `false` one outright rather than pairing two of them up.
         */
        case Value::Call:
            if(!isCheckCall(opt, ((InstCall&)a).callee)) return false;
            if(!isCheckCall(opt, ((InstCall&)b).callee)) return false;
            if(((InstCall&)a).args.size() != 1 || ((InstCall&)b).args.size() != 1) return false;

            return ((InstCall&)a).args.get(opt.local, 0) == ((InstCall&)b).args.get(opt.local, 0);

        default:
            return false;
    }
}

/*
 * The storage an instruction may reach, as places - `eachPlace` with the aggregate spelled out.
 *
 * An `InstAggregate`'s own place is the record it fills, with no path, so reporting that is right
 * for a kill of the whole thing and wrong as a description of what it wrote: a literal built in one
 * field of a local would be read as a write of the local. Its components are the stores it replaced,
 * and those are the places.
 */
template<class F>
void eachWritten(OptContext& opt, Value& instruction, F&& f) {
    if(instruction.kind != Value::Aggregate) {
        eachPlace(instruction, forward<F>(f));
        return;
    }

    eachWrittenComponent(opt.local, opt.module->arena, (InstAggregate&)instruction,
                         [&](Place place, ModulePtr<Value>, Size) { f(place); });
}

// Whether this instruction is one whose result the scope below may hold on to without asking
// anything about storage - see the header for why a check is one of these and a load is not.
bool isRepeatable(OptContext& opt, Value& instruction) {
    if(isPureValue(instruction)) return true;

    return instruction.kind == Value::Call && isCheckCall(opt, ((InstCall&)instruction).callee) &&
           ((InstCall&)instruction).args.size() == 1;
}

/*
 * A computation over nothing but constants, which is worth *more* than what removing it would save.
 *
 * Both targets spell one of these where it is used rather than computing it: natively it is an
 * immediate operand, and on JS it is a literal inside the expression that reads it. Giving it a
 * second reader is what stops that - the value has to exist somewhere both readers can see, which is
 * a register on one target and a `var` on the other - so unifying two of them turns two free things
 * into one that costs.
 *
 * `null` is what found it. `cast 0 : %U8` is not foldable - a pointer has no width to fold at - so
 * two of them are two instructions, and `sameOperand` calls them equal because the two zeroes are.
 * Unifying them took `{ first$e: null, second$e: null }` apart into a literal and two assignments to
 * it, because the shared `var v5 = null` is defined after the literal that wanted it.
 *
 * An instruction with no operands at all is not one of these and keeps the general rule: `sizeof` and
 * a symbol's address are constants the *target* supplies rather than ones written here.
 */
bool computesFromConstants(OptContext& opt, Value& instruction) {
    auto operands = 0;
    auto constants = 0;

    eachOperand(opt.local, instruction, [&](ModulePtr<Value> operand) {
        operands++;
        if(operand && isConstant(*opt.local[operand])) constants++;
    });

    return operands != 0 && operands == constants;
}

/*
 * Whether one of these may be answered from an earlier one in another block.
 *
 * A comparison may not, and it is the one kind that may not. What a `Cmp` costs to recompute is
 * nothing on either target and what it costs to *carry* is real on both: natively its result is the
 * flags, which the branch reading it consumes in place, so a second reader in another block turns it
 * into a `setcc` into a register plus a `test` at each use - which is §2 of test/bench/findings.md
 * arriving from the other direction. On JS it is an expression the emitter puts inside the `if` that
 * reads it, and a second reader turns `if (m <= 2)` into `var v1 = m <= 2 ? 1 : 0` and two `=== `
 * tests against it.
 *
 * Within one block it is kept, because there the two readers are already reading one flag.
 */
bool answerableAcrossBlocks(Value& instruction) {
    return instruction.kind != Value::Cmp;
}

/*
 * A read whose result stands for the contents of storage, while that is still true.
 *
 * Two kinds of storage, because there are two kinds of read that answer with something a later write
 * can change. `place` is a `LoadPlace`, which is every read on the native target. `host` is a
 * property of a host value - `xs.length` on JS, where `Array(a)` *is* the host array - and it is
 * named by the value it is read from rather than by a place, because there is no place to name.
 *
 * `depth` is the dominator-tree depth the entry was pushed at, and is what pops it: everything a
 * block established leaves scope with the block's subtree. `killedAt` is the depth some write
 * invalidated it at, and is what *restores* it - a write inside one subtree says nothing about a
 * sibling, so the entry comes back rather than going away when that depth is left.
 */
struct AvailableLoad {
    Place place;

    // Set for a host property and null for a load, which is what tells the two apart. A host entry
    // is invalidated by *any* write, since nothing here can say which value a store reached.
    ModulePtr<Value> host = nullptr;
    StringId method {};

    ModulePtr<Value> value = nullptr;
    U32 depth = 0;
    U32 killedAt = kAlive;

    static constexpr U32 kAlive = maxLimit<U32>;
};

// A host property read - `xs.length`, and the whole of NativeOp::HostField. Answered off the
// instruction rather than by the caller so that "is this one of these" and "what identifies it" are
// one question: an operation of this kind always has exactly the one operand it reads.
bool isHostFieldRead(Value& instruction) {
    return instruction.kind == Value::Native &&
           ((InstNative&)instruction).op == NativeOp::HostField &&
           ((InstNative&)instruction).args.size() == 1;
}

/*
 * A whole local of scalar type, which this declines to hold on to - `collectCandidates` in
 * opt_promote.cpp's rule, for the same reason and against the same measurement.
 *
 * Such a place is storage both targets already have a better form for. On JS a scalar local was
 * never memory: it is a `var`, and two reads of one are two reads of a register, so unifying them
 * buys nothing and costs a `var v7 = i` in front of every loop that reads its own counter twice - 837
 * bytes of emitted JavaScript across the fixture corpus, measured, for no read removed. Natively
 * `promoteStackSlots` has already turned the same local into a phi by the time anything is emitted,
 * so the second read was never a load either; what is left redundant there is the *arithmetic* over
 * it, which is the lower-IR pass's to collect - see lower_cse.h.
 *
 * What is left after the rule is exactly the storage neither target can flatten for itself: a field
 * of a record, a payload behind a downcast, an element behind a pointer, a module-level global. That
 * is where the reads worth removing are, and §9.2 of test/bench/findings.md is one of them - an
 * array's `length` and `items`, read once per bounds check.
 */
bool isScalarLocal(const Place& place) {
    return place.root == PlaceRoot::Local && const_cast<Place&>(place).projections.isEmpty();
}

/*
 * What one block does to storage that this pass cannot see through, asked once per function rather
 * than once per block that could run after it.
 *
 * `exposes` is `writesUnknownStorage` over the block's instructions, and `writes` the places its
 * stores name - two different invalidations, because they are two different claims. An instruction
 * that may have written anything only reaches storage a callee has a way to *name*, which is
 * `staysInFrame`; a store reaches exactly what `placesMayAlias` says it might.
 */
struct BlockEffects {
    U32 firstWrite = 0;
    U32 writeCount = 0;
    bool exposes = false;
};

struct Effects {
    // Most functions in the optimizer have only a handful of blocks. Keeping those rows beside the
    // analysis avoids a heap allocation on every fixed-point visit; larger functions spill to the
    // ordinary backing store without changing the indexing below.
    SmallArray<BlockEffects, 8> blocks;

    // One list for the whole function, indexed into per block, rather than a list per block: a
    // block writes a handful of places and there is no reason to allocate per row for that. Sixteen
    // also covers the whole list for the common small-function case.
    SmallArray<Place, 16> writes;
};

void computeEffects(OptContext& opt, Effects& effects) {
    effects.blocks.clear();
    effects.writes.clear();

    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        BlockEffects row;
        row.firstWrite = U32(effects.writes.size());

        for(auto pointer: opt.local[blockPointer]->instructions(opt.local)) {
            auto instruction = opt.local[pointer];

            if(writesUnknownStorage(opt, *instruction)) row.exposes = true;

            /*
             * Every place the instruction names, and not only the ones it writes.
             *
             * Conservative on a read, and deliberately so: what is being built is the reason to
             * *stop* believing something, and an instruction whose place slots this pass has not
             * classified is one whose effect on them it does not know. `Init` and `Assign` are the
             * writes; a `Swap` writes both of the two it names; a `LoadPlace` writes neither and
             * still costs nothing here, because a place is only ever compared against a fact about
             * the same storage - so a read invalidating its own place removes a fact that was about
             * to be re-established by the read itself.
             *
             * The exception is the aggregate, whose own place has no path at all - leaving it to
             * `eachPlace` would make every construction reach every field of every local it is
             * built in, which is the whole of it rather than the elements it writes.
             */
            eachWritten(opt, *instruction, [&](const Place& place) { effects.writes.push(place); });

            /*
             * And the storage the instruction was *handed*, which `exposes` is the wrong rule for.
             *
             * `computeContainment` admits an unretained call argument and an unretained borrow, so a
             * record passed to `==` and a container `push` is taken a `&mut` of both stay contained -
             * and `killExposed` therefore spares their loads at every call in the function. That is
             * the whole point of admitting them, and it is only true of the calls that were handed
             * something else. The one that received this storage may have written it, so it is a
             * write of the local like any other and `killAliasing` ends the facts here.
             */
            eachAddressedLocal(opt, *instruction, [&](U32 local) {
                effects.writes.push(Place::inLocal(local));
            });
        }

        row.writeCount = U32(effects.writes.size()) - row.firstWrite;
        effects.blocks.push(row);
    }
}

// The walk's state, which is everything that is not one block.
struct Eliminator {
    OptContext& opt;
    Dominance& dominance;
    Effects effects;

    // Per local, whether a callee could reach its storage - see `containmentOf`. What decides
    // whether a fact survives a block that may have written anything. A pointer rather than a
    // reference because it is taken lazily - see `ensureStorageAnalysis` - and null until then.
    const IndexSet* contained = nullptr;

    /*
     * The storage analysis is wanted only if this walk finds a load worth remembering.
     *
     * CSE also handles pure expressions, and most of the small functions reached on each optimizer
     * round contain only those. Building `effects` and `contained` for every one of them allocated
     * two growing arrays per visit and then never read either. Delaying the work until the first
     * load keeps the analysis exactly as function-wide as it was -- in particular, `killBetween`
     * can still inspect blocks visited before the load -- without charging expression-only CSE for
     * storage facts it has no entry to invalidate.
     */
    bool storageReady = false;

    void ensureStorageAnalysis() {
        if(storageReady) return;

        computeEffects(opt, effects);
        contained = &containmentOf(opt);
        storageReady = true;
    }

    // The repeatable results in scope, which nothing invalidates, and the loads, which anything
    // that writes their storage does.
    SmallArray<ModulePtr<Inst>, 32> available;
    SmallArray<AvailableLoad, 16> loads;

    void killAliasing(const Place& place) {
        for(Size i = loads.size(); i-- > 0;) {
            if(loads[i].killedAt != AvailableLoad::kAlive) continue;

            // A host property is reached through a value rather than through a local, so there is
            // nothing to compare a place against and every write is one that may have reached it.
            auto reached = loads[i].host ? true : placesMayAlias(opt, loads[i].place, place);
            if(reached) loads[i].killedAt = depth;
        }
    }

    // Everything an instruction this pass cannot see through may have written. Not everything: a
    // local whose address was never handed out is storage no callee has a way to name, which is the
    // same exemption `forwardPlaces` makes and for the same reason - and it does not extend to a
    // host value, which `contained` has nothing to say about.
    void killExposed() {
        // A load in the list is a load that was remembered, and remembering one is what takes the
        // storage analysis - so an entry here means `contained` was taken. Asserted rather than
        // guarded, because the alternative reading would be a load kept across a call on the
        // strength of a set nothing filled in.
        assertTrue(loads.isEmpty() || storageReady);

        for(Size i = loads.size(); i-- > 0;) {
            if(loads[i].killedAt != AvailableLoad::kAlive) continue;

            auto safe = !loads[i].host && staysInFrame(opt, *contained, loads[i].place);
            if(!safe) loads[i].killedAt = depth;
        }
    }

    /*
     * Everything that could run between a block's immediate dominator and the block itself, applied
     * to the scope as one invalidation.
     *
     * This is what makes a fact carried down the dominator tree true rather than merely dominated.
     * The scope says an earlier load *dominates* this block, which is a statement about the paths
     * that reach here and none about what happened on them: an arm of a diamond, or a whole loop
     * body, runs after the dominator finishes and before this block starts.
     *
     * The region is found by walking backwards from the block's predecessors and never stepping
     * through its immediate dominator, which is exactly the blocks that can run in between - every
     * path to here goes through that dominator, so anything reachable backwards from a predecessor
     * without passing it is on such a path. A loop header finds its own body this way, because the
     * latch is one of its predecessors, which is what makes a fact from outside a loop die at the
     * header rather than surviving into a body that overwrites it.
     */
    void killBetween(U32 index) {
        // There is no fact to invalidate before the first tracked load. `forwardRead` initializes
        // the tables when it adds that first fact, so every later visit that reaches below has them.
        if(loads.isEmpty()) return;

        auto stop = dominance.immediate[index];

        ScratchSet seen(opt.sets, dominance.blocks.size());
        SmallArray<U32, 32> pending;

        auto reach = [&](U32 block) {
            if(block == stop || (*seen)[block]) return;

            seen->set(block, true);
            pending.push(block);
        };

        for(auto predecessor: opt.local[dominance.blocks[index]]->incoming(opt.local)) {
            reach(opt.local[predecessor]->index);
        }

        while(pending.size()) {
            auto block = pending.pop().unwrap();
            auto& row = effects.blocks[block];

            if(row.exposes) killExposed();
            for(U32 i = 0; i < row.writeCount; i++) {
                killAliasing(effects.writes[row.firstWrite + i]);
            }

            for(auto predecessor: opt.local[dominance.blocks[block]]->incoming(opt.local)) {
                reach(opt.local[predecessor]->index);
            }
        }
    }

    // The read in scope that answers this one, if any. The newest first, so a place written and read
    // again inside one block answers with the read below the write rather than the one above it.
    ModulePtr<Value> availableLoad(Value& read) {
        auto host = isHostFieldRead(read);

        for(Size i = loads.size(); i-- > 0;) {
            auto& entry = loads[i];
            if(entry.killedAt != AvailableLoad::kAlive) continue;
            if((entry.host != nullptr) != host) continue;
            if(opt.local[entry.value]->type != read.type) continue;

            if(host) {
                auto& native = (InstNative&)read;
                if(entry.method != native.method) continue;
                if(entry.host != native.args.get(opt.local, 0)) continue;
            } else if(!samePlace(opt, entry.place, ((InstLoadPlace&)read).place)) {
                continue;
            }

            return entry.value;
        }

        return nullptr;
    }

    // A read remembered, and the one already in hand where there is one. Answers whether the
    // instruction was replaced, so the caller knows whether it is still there to walk past.
    bool forwardRead(ModulePtr<Inst> pointer, Value& read) {
        if(auto value = availableLoad(read)) {
            opt.ir().replaceValue((ModulePtr<Value>)pointer, value);
            return true;
        }

        // From this point on a write may have to invalidate the entry being added, including one
        // later in this same block. Have the containment facts ready before that can happen; the
        // effect table is built beside them so a dominated block can inspect every intervening path.
        ensureStorageAnalysis();

        AvailableLoad entry;
        entry.value = (ModulePtr<Value>)pointer;
        entry.depth = depth;

        if(isHostFieldRead(read)) {
            entry.host = ((InstNative&)read).args.get(opt.local, 0);
            entry.method = ((InstNative&)read).method;
        } else {
            entry.place = ((InstLoadPlace&)read).place;
        }

        loads.push(::move(entry));
        return false;
    }

    U32 depth = 0;

    // One walk of the dominator tree, carrying what has been computed on the path from the entry to
    // the block being visited. Recursive because the depth is the tree's rather than the function's.
    void run(U32 index) {
        auto scope = available.size();
        auto loadScope = loads.size();

        killBetween(index);

        auto block = opt.local[dominance.blocks[index]];

        for(Size i = 0; i < block->instructionCount(); i++) {
            auto pointer = block->instructionAt(opt.local, i);
            auto instruction = opt.local[pointer];

            if(isRepeatable(opt, *instruction)) {
                if(computesFromConstants(opt, *instruction)) continue;

                ModulePtr<Inst> existing = nullptr;
                for(Size a = available.size(); a-- > 0;) {
                    if(!sameComputation(opt, *opt.local[available[a]], *instruction)) continue;
                    if(!answerableAcrossBlocks(*instruction) &&
                       opt.local[available[a]]->block != instruction->block) {
                        continue;
                    }

                    existing = available[a];
                    break;
                }

                if(!existing) {
                    available.push(pointer);
                    continue;
                }

                opt.ir().replaceValue((ModulePtr<Value>)pointer, (ModulePtr<Value>)existing);

                /*
                 * And the check itself, which pointing the readers somewhere does not remove: it is
                 * still a call, and nothing below this would take it away - `isDischargedCheck` only
                 * removes one whose flag folded to `false`, and this one's has not folded at all. A
                 * pure computation is left where it is, because `eliminateDeadValues` is what
                 * removes a value nothing reads and it runs immediately after this.
                 */
                if(!isPureValue(*instruction)) {
                    opt.ir().eraseInstruction(pointer);
                    i--;
                }

                continue;
            }

            /*
             * The two reads, and the one guard both of them need.
             *
             * A load of a memory type answers with the storage it names rather than with the
             * contents of it, so two of them are two names for one place and not one value seen
             * twice - see `holdsLoadableValue`. `forwardPlaces` does unify those, under a rule about
             * which of the two names outlives the other, and this pass has no such rule.
             */
            if(instruction->kind == Value::LoadPlace || isHostFieldRead(*instruction)) {
                auto worthIt = instruction->kind != Value::LoadPlace ||
                               !isScalarLocal(((InstLoadPlace&)*instruction).place);

                if(worthIt && holdsLoadableValue(opt, instruction->type)) {
                    forwardRead(pointer, *instruction);
                }

                continue;
            }

            // And what this instruction does to the storage the scope is holding, which is the same
            // three invalidations `killBetween` applies for a whole block.
            if(writesUnknownStorage(opt, *instruction)) killExposed();
            eachWritten(opt, *instruction, [&](const Place& place) { killAliasing(place); });
            eachAddressedLocal(opt, *instruction, [&](U32 local) {
                killAliasing(Place::inLocal(local));
            });
        }

        depth++;
        for(auto child: dominance.children[index]) run(child);
        depth--;

        while(available.size() > scope) available.pop();
        while(loads.size() > loadScope) loads.pop();

        /*
         * And the entries anything in this subtree invalidated, which are live again for whatever
         * the walk visits next: a sibling subtree is reached by a path that ran none of this one.
         *
         * `>= depth` rather than `>` because this block's own kills are recorded at its own depth,
         * and they are as much this subtree's as a child's are. They had to hold for the rest of the
         * block and for everything under it, which they did - the restore is at the end.
         */
        for(Size i = 0; i < loads.size(); i++) {
            if(loads[i].killedAt >= depth) loads[i].killedAt = AvailableLoad::kAlive;
        }
    }
};

}

void eliminateCommonValues(OptContext& opt) {
    if(opt.function->blocks.isEmpty()) return;

    // The stage's, not this pass's: the loop pass asks for the same thing on the same function a
    // few lines later, and neither wants the rows rebuilt - nor, now, recomputed.
    auto& dominance = dominanceOf(opt);

    Eliminator eliminator { opt, dominance };
    eliminator.run(0);
}

/*
 * Whether one host operation reads and does nothing else.
 *
 * `Value::Native` is not `kInstPure` and must not become one: the same kind carries `copyWithin`,
 * which writes a shared buffer, `console.log`, and the throw. So the question is asked per
 * operation, and by allowlist rather than by exclusion - a host member added to resolve/host.cpp
 * that this has never heard of is answered "no", which costs a removal and cannot cost correctness.
 *
 * `HostField` and `HostBinary` are read-only as *kinds*: the first is `.length` and the second is
 * one of the host's own operators over two values. `HostCall` is the one that is mixed, so its two
 * readers are named. Matched on the member's spelling for the same reason `genHost` matches on it -
 * the set is closed, `attachIntrinsic` is its only producer, and a second enum here would be a
 * thing to keep in step with that one.
 */
static bool isReadOnlyHostOp(OptContext& opt, InstNative& instruction) {
    if(instruction.op == NativeOp::HostField || instruction.op == NativeOp::HostBinary) return true;
    if(instruction.op != NativeOp::HostCall) return false;

    auto text = stringView(opt.context.findName(instruction.method));
    return text == "charCodeAt"_v || text == "indexOf"_v;
}

/*
 * A read whose result nothing reads.
 *
 * Reading a place has no effect, so removing one is only a question of whether it could have
 * *failed*. A local or a global is storage the checker proved is there; a pointer or a borrow root
 * is an address the program computed, and removing a load through one would remove a fault the
 * program is entitled to take. So the first two go and the last two stay.
 *
 * These exist in quantity rather than as an oddity: the resolver emits a whole-aggregate load in
 * front of every field access - `%v9 = load %e : Entry` before `%v10 = load %e@Entry.live` - and
 * nothing has ever read one.
 */
static bool isDeadRead(OptContext& opt, Value& instruction) {
    /*
     * And an `addressof` nothing reads, on the same two roots and for a reason that reads the same
     * way from the other side.
     *
     * Taking an address performs nothing and cannot fail. The reason the kind is not `kInstPure` is
     * that it is a *promise about placement* rather than a computation - a value it is applied to
     * cannot stay in a register - and a promise nothing reads is no promise at all. So a dead one
     * goes, and the storage it was pinning is free to be whatever the passes below decide.
     *
     * Which is what makes it worth having: a closure whose call this stage resolved leaves the
     * address of its environment behind, and an environment with its address taken is one
     * `forwardPlaces` will not answer a read of - so the captures stay behind a load and the *next*
     * call through one of them stays a `calldyn`. See inlineDynamicCall.
     */
    if(instruction.kind == Value::Address) {
        auto& place = ((InstAddress&)instruction).place;
        return place.root == PlaceRoot::Local || place.root == PlaceRoot::Global;
    }

    /*
     * And a borrow nothing reads, on the same two roots and for the same reason.
     *
     * `collapseBorrows` is what removes a borrow whose readers can name the borrowed place for
     * themselves, and it opens by declining a borrow with *no* readers - there is nothing to
     * rewrite, so it has nothing to say about one. That left a dead borrow with no pass that
     * removes it, which is worse than the live one: every consequence opt_borrow.cpp lists for a
     * borrowed local still holds. On JS the local is boxed, so `Box {item: 5}` came out as
     * `{ $v: 5 }.$v`; natively the address stops `promoteStackSlots` holding it in a register; and
     * `forwardPlaces` will not answer a read of it on either, so a drop flag stored one line above
     * the branch that tests it stays a branch.
     *
     * `BorrowPair.yana` is the fixture, where the whole of `main` is a constant once this runs.
     */
    if(instruction.kind == Value::Borrow) {
        auto& place = ((InstBorrow&)instruction).place;
        return place.root == PlaceRoot::Local || place.root == PlaceRoot::Global;
    }

    // And a host operation that only reads - see isReadOnlyHostOp, and the fold in opt_fold.cpp
    // that leaves one behind.
    if(instruction.kind == Value::Native) return isReadOnlyHostOp(opt, (InstNative&)instruction);

    if(instruction.kind != Value::LoadPlace) return false;

    auto& place = ((InstLoadPlace&)instruction).place;
    return place.root == PlaceRoot::Local || place.root == PlaceRoot::Global;
}

/*
 * A check whose condition folded to `false`.
 *
 * The one call this pass is entitled to remove, and only in this one state: the argument is what the
 * check would have to look at, so a condition that is already known not to hold makes the call a
 * jump to a `return` and back. Everything the callee could otherwise do - the branch, the message,
 * the process ending - is behind that flag.
 *
 * A condition that folded the other way is deliberately left alone. `checkCondition(true)` is a
 * program that stops here, and removing it would remove the stop rather than the check.
 *
 * This is where a discharged bounds test goes away on a target that keeps the check a call. Natively
 * the inliner has usually got there first and the same fact is a branch on a constant, which
 * opt_branch.cpp removes along with the arm behind it; the two are the same elimination arriving
 * through whichever shape the check is in.
 */
static bool isDischargedCheck(OptContext& opt, Value& instruction) {
    if(instruction.kind != Value::Call) return false;

    auto& call = (InstCall&)instruction;
    if(!isCheckCall(opt, call.callee) || call.args.size() != 1) return false;

    auto condition = constantValueOf(opt, call.args.get(opt.local, 0));
    return condition && condition.unwrap() == 0;
}

void eliminateDeadValues(OptContext& opt) {
    /*
     * To a fixed point within this pass rather than across the driver's rounds, because the shape it
     * produces is a chain: the operands of an instruction it removed are exactly the values that may
     * have just become unread, and walking the blocks backwards catches most of that in one sweep.
     *
     * Only the pure kinds. Everything else in the IR either has an effect, is one of the ownership
     * decisions the analyses already took, or reads storage whose writers this pass cannot see.
     */
    auto changed = true;
    while(changed) {
        changed = false;

        for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
            auto block = opt.local[blockPointer];

            for(Size i = block->instructionCount(); i-- > 0;) {
                auto pointer = block->instructionAt(opt.local, i);
                auto instruction = opt.local[pointer];

                if(instruction->useCount() != 0) continue;
                if(!isPureValue(*instruction) && !isDeadRead(opt, *instruction) &&
                   !isDischargedCheck(opt, *instruction)) {
                    continue;
                }

                opt.ir().eraseInstruction(pointer);
                changed = true;
            }

            /*
             * And the phis, which are in a list of their own and were therefore never reached by the
             * walk above - see Block, where the two lists are separate so that "before everything
             * else" is a property of the IR rather than of insertion order.
             *
             * A phi is pure by construction, so the only question about one is whether anything reads
             * it. `promotePlaces` cleans up after itself and nothing else used to leave one behind,
             * which is why this went unnoticed; `splitPhiOfLocals` leaves exactly this - the phi of
             * storage whose readers have all been given a phi of values instead - and a phi kept
             * alive by nothing keeps every allocation feeding it alive too.
             */
            for(Size i = block->phiCount(); i-- > 0;) {
                auto pointer = block->phiAt(opt.local, i);
                if(opt.local[pointer]->useCount() != 0) continue;

                opt.ir().erasePhi(pointer);
                changed = true;
            }
        }
    }
}
