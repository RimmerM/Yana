#include "pass.h"

/*
 * The second of two computations answered by the first, over the whole dominator tree - and the one
 * statement of "these two instructions are the same" that this directory has.
 *
 * `simplify` performs the same rewrite inside one block, where "the first one dominates the second"
 * is the instruction order and needs no tree at all. That is not this pass with a smaller scope: it
 * is the case a worklist over uses reaches for free, and it is what the folder and the inliner keep
 * producing. What is left for a walk of the tree is the redundancy the *program* was written with -
 * one expression spelled twice in two blocks, a length read once per bounds test - and reaching it
 * costs a dominator tree and a scope stack, which is why it is a pass rather than a clause.
 *
 * ## Why it is here at all, when `lower_cse.cpp` exists
 *
 * README.md's section A assigned `eliminateCommonValues` to the shared lower sequence, which runs
 * over this stage's output, and that verdict was right for the native target and only for it.
 * `doc/proposal/Analysis-IR-JS.md` §6.1 is the correction: JSIR consumes OSSA and never lowers, so
 * "source arithmetic folding, CSE and DCE" is named there as work the shared OSSA optimizer keeps.
 * A CSE that lives only below the fork is a CSE the JavaScript backend does not have.
 *
 * The two are not the same pass twice for the same reason `lower_cse.h` gives about itself: down
 * there an address is arithmetic over pointers and a comparison is the flags, so the rules that
 * decide *not* to unify are about registers. Here the rules are about ownership and about what the
 * target spells at its use.
 *
 * ## What may be unified, and the three rules measured against it
 *
 * `isRemovable` is the ownership half and it is `simplify`'s, shared rather than restated: a
 * `kSsaPure` kind, no tracked operand and no consuming use, and results that are `Trivial` or
 * `Address`. `sameComputation` is the equality - same kind, same immediates, same operands compared
 * as SSA values with their use kinds, and results agreeing on the whole occurrence and the
 * convention.
 *
 * On top of those it declines two things the block-local one does not have to, and **admits** a
 * third the first version of this pass declined:
 *
 *  - **a value materialized where it is used** is declined. `materializedAtUse` is `simplify`'s rule
 *    and the measurement behind it is `simplify`'s too: a computation over nothing but constants,
 *    and an instruction with no operands at all, are spelled at the use rather than computed - so
 *    giving one a second reader is what stops that. `global.addr` in `log = log + 1` was fourteen
 *    lines across `lens/Iterator.yana` for one instruction removed.
 *  - **a comparison across blocks** is declined, which is `lower_cse.h`'s rule and
 *    [[yana-cse-two-tiers]]'s measurement. Its result is the flags natively and an expression inside
 *    the `if` on JS; a second reader elsewhere is a `setcc` plus a `test` on one target and
 *    `var v1 = m <= 2 ? 1 : 0` on the other. Kept inside one block, where the two readers read one
 *    flag. Measured here as well and it is the one place the two answers nearly meet: admitting them
 *    is ten more unifications and **four** lowered lines, which is noise against a rule that has a
 *    time measurement behind it at both tiers.
 *  - **an `Address` result across blocks is admitted**, and that is a reversal. The argument for
 *    declining it was `lower_licm.cpp`'s `worthHoisting` - an address is a displacement in the
 *    addressing mode of whatever reads it, so a second reader in another block trades an encoding
 *    byte for a register live between the two. Measured, it is the other way round: unifying them is
 *    **249 computations** instead of 10 and the lowered corpus is **196 lines and 2 `alloca`s
 *    smaller** for it. The two are not in tension - `worthHoisting` prices carrying an address
 *    *across a whole loop*, which `hoist` in `loop.cpp` still declines on that authority, and this
 *    prices computing one twice on a path that runs once.
 *
 * ## The loads, and the one that is deliberately not unified
 *
 * A `Load` is not `kSsaPure` and cannot be, so it is admitted by a rule of its own: a `Trivial` read
 * through an address **rooted in an allocation of this body**, answered by a dominating read through
 * the same address value, where nothing that could run between the two may write that storage.
 * "Could run between" is the shipping stage's region and is `killBetween` below - every block
 * reachable backwards from this block's predecessors without passing its immediate dominator, which
 * is exactly the blocks a path from the dominator to here can pass through. `mayWriteStorage` is the
 * one predicate that answers a write, shared with `forwardMemory`.
 *
 * **A read of a whole allocation is declined**, and that is a measurement rather than a shape rule.
 * It is `isScalarLocal` in the shipping stage's `opt_value.cpp`, arrived at there against the same
 * numbers: such a place is storage both targets already have a better form for, and unifying two
 * reads of one gives a value that has to exist somewhere both readers can see. Admitting it here
 * removes **541** loads at this tier and costs the lowered corpus **620 lines and 22 `alloca`s**,
 * because `promoteStackSlots` had already made every one of those reads a register. What is left
 * after the rule - a field of a record, an element behind a pointer, a payload behind a projection -
 * is **81** unifications and is where the reads worth removing are: it is what takes the lowered
 * corpus *down* rather than up.
 *
 * That leaves the whole-slot pair to `forwardMemory`, which is where it belongs: within one block it
 * is a fact carried across instructions, and one tier down `promoteStackSlots` takes the rest.
 *
 * ## The scope, and why a kill is remembered rather than dropped
 *
 * The walk is the dominator tree with a scope per node, so everything in scope dominates the
 * instruction being looked at. A write inside one subtree says nothing about a sibling subtree,
 * which is reached by a path that ran none of it - so a killed load records the depth it died at and
 * comes back when that depth is left. That is the shipping stage's `killedAt`, and without it the
 * first arm of the first diamond ends every fact for the rest of the function.
 */

namespace ssa {

/*
 * Whether two instructions carry the same thing beside their operands - the shipping stage's
 * `sameExtra`, as one switch.
 *
 * **The default declines**, and that is the whole design of it: a kind this has never been looked at
 * for is one whose immediates might be anything, so it is left alone rather than unified on its
 * operands alone. What that costs is a removal; what the other default would cost is two
 * `field.addr`s at different fields becoming one.
 */
static bool sameImmediates(Inst& a, Inst& b) {
    switch(a.kind) {
        case Inst::Const:
            return ((InstConst&)a).constant == ((InstConst&)b).constant;
        case Inst::Binary:
            return ((InstBinary&)a).op == ((InstBinary&)b).op;
        case Inst::Unary:
            return ((InstUnary&)a).op == ((InstUnary&)b).op;
        case Inst::Select:
            return true;
        case Inst::Fma:
            return true;
        case Inst::GlobalAddress:
            return ((InstGlobalAddress&)a).global == ((InstGlobalAddress&)b).global;
        case Inst::FieldAddress: {
            auto& left = (InstFieldAddress&)a;
            auto& right = (InstFieldAddress&)b;
            return left.record == right.record && left.field == right.field &&
                   left.shape == right.shape && left.property == right.property;
        }
        case Inst::ElementAddress:
            return ((InstElementAddress&)a).container == ((InstElementAddress&)b).container;
        case Inst::Metric: {
            auto& left = (InstMetric&)a;
            auto& right = (InstMetric&)b;
            return left.metric == right.metric && left.of == right.of;
        }
        /*
         * A `map` declines, and the row is here so that it is a decision rather than the default.
         *
         * Two maps are one computation only where their entry lists agree entry for entry, and
         * unifying two tables that merely share a selector is a wrong answer rather than a missed
         * one. What declining costs is nothing worth having: two identical tables in one body is one
         * `match` written twice, which is a source that should say so.
         */
        case Inst::Map:
            return false;
        default:
            return false;
    }
}

/*
 * The ownership half of the rule, without the purity question in front of it.
 *
 * Split out for the one caller that answers purity differently - `isRemovableIfUnread` below - and
 * split rather than copied because this is the clause the linear rule lives in: two spellings of it
 * is one of them learning about a new convention and the other not.
 */
static bool ownershipAdmitsRemoval(OptContext& opt, Inst& inst) {
    auto ok = true;
    forEachUse(opt.base, inst, [&](Use used) {
        if(!used.isSet()) { ok = false; return; }
        if(used.kind() == UseKind::Consume) ok = false;
        if(isTracked(opt.base[used.value()]->convention)) ok = false;
    });

    for(auto resultPointer: inst.results.contents(opt.base)) {
        auto convention = opt.base[resultPointer]->convention;
        if(convention != Convention::Trivial && convention != Convention::Address) ok = false;
    }

    return ok;
}

bool isRemovable(OptContext& opt, Inst& inst) {
    return isPure(inst.kind) && ownershipAdmitsRemoval(opt, inst);
}

/*
 * The same, for an instruction **nothing reads** - which admits one kind more.
 *
 * `Load` is deliberately not `kSsaPure`: purity here is "may be recomputed, or not computed at all",
 * and a read that may be recomputed is one a pass may duplicate or sink past a store. Neither of
 * those is what this asks. Deleting a read whose result has no readers moves nothing and duplicates
 * nothing - it removes a memory access whose value the program does not have a use for - and that is
 * a question the trait column does not answer, because everything else in this directory reaches a
 * dead load through `forwardMemory` and `removeDeadStorage` instead.
 *
 * `dischargeChecks` is the caller that cannot: it runs above the `optimizeIr` gate, where neither of
 * those passes exists, and the `self.length` read a discharged bounds test leaves behind is exactly
 * this shape. The ownership guard is the same one, so a load handing back a borrow or consuming an
 * owner is declined here as it is above.
 */
bool isRemovableIfUnread(OptContext& opt, Inst& inst) {
    if(!isPure(inst.kind) && inst.kind != Inst::Load) return false;
    return ownershipAdmitsRemoval(opt, inst);
}

/*
 * And whether an instruction **nothing reads at all** may simply be taken out - which is weaker
 * again, because the two clauses `ownershipAdmitsRemoval` adds are both about *substitution*.
 *
 * A tracked operand is refused up there because erasing an instruction drops the uses it records,
 * and a consumption that went away leaves an owner unconsumed on a path that had consumed it. That
 * is exactly right for a `Consume` and says nothing about a `Borrow`: dropping a borrow shortens an
 * extent, which is the direction an extent rule is safe in. A non-`Trivial` result is refused up
 * there because unifying two borrows on different loans moves an endpoint - and a result nothing
 * reads is unified with nothing.
 *
 * So what is left is the two statements this rule really has to make. **An `Owned` result nothing
 * reads is IR the verifier has already refused**, so it is not this rule's to remove; an `Affine`
 * one may be abandoned, which is the whole of what that convention says (`inst.h`). And a **`Load`
 * whose result is tracked is a destructive read**, which empties storage that MIR's move-path
 * invariant then says nothing may read again - a rule stated over there and not here, so a rewrite
 * resting on it is one whose failure mode verifies.
 *
 * `Aggregate` and `FunValue` are admitted by name beside the pure kinds. Neither is `kSsaPure` -
 * both consume their components in the general case - and both are what a flattened adaptor chain
 * leaves standing with nothing reading it: an environment record of borrowed fields and the
 * `{code, env}` pair over it, which owe no teardown ([[yana-function-values]]) and are `Affine`
 * precisely so that they may be abandoned. The consuming case is refused by the operand walk above,
 * so naming the kinds here costs nothing the rule does not already check.
 *
 * **`Copy` is admitted for the reason `Load` is**, and its contract is what says so: `InstCopy` is
 * `copy.trivial` and nothing else - "an authored `copy` is a call and stays a call" - so it runs no
 * user code and its result owes whatever the convention says and no more. A copy nothing reads is
 * bytes moved for nobody. The `mustConsume` test below is what keeps an `Owned` one, which is a
 * second owner somebody still has to discharge.
 *
 * It is what a folded read of a global leaves: `constantStepOf` walks through a `copy` to the
 * global's initializer, every reader of the copy is then answered by a constant, and without this
 * the copy itself stayed - `test/resolve/TruthOperators.yana` kept twenty of them, each with the
 * `global.addr` under it, after every branch they fed had folded away.
 */
bool isDiscardableIfUnread(OptContext& opt, Inst& inst) {
    if(!isPure(inst.kind) && inst.kind != Inst::Load && inst.kind != Inst::Aggregate &&
       inst.kind != Inst::FunValue && inst.kind != Inst::Copy) {
        return false;
    }

    auto ok = true;

    forEachUse(opt.base, inst, [&](Use used) {
        if(!used.isSet() || used.kind() == UseKind::Consume) ok = false;
    });

    for(auto resultPointer: inst.results.contents(opt.base)) {
        auto value = opt.base[resultPointer];

        if(mustConsume(value->convention)) ok = false;
        if(inst.kind == Inst::Load && isTracked(value->convention)) ok = false;
    }

    return ok;
}

bool isUnifiable(Value& from, Value& to) {
    return from.convention == to.convention &&
           (from.convention == Convention::Trivial || from.convention == Convention::Address);
}

Value* soleResult(OptContext& opt, Inst& inst) {
    if(inst.results.size() != 1) return nullptr;
    return opt.base[inst.results.get(opt.base, 0)];
}

/*
 * Whether two operands are the same value - identity, and one thing beside it.
 *
 * **Two constants holding one number at one type are one value**, however many times the program
 * spelled it. `Editor::addConst` makes a fresh instruction per site, which is what lets each carry
 * its own source location, so `x * 4` written twice is two `const 4`s and comparing operands by
 * identity alone says the two multiplications are two computations. [[yana-cse-two-tiers]] records
 * the same discovery one tier down and calls it "the first thing to add to any new equality over
 * this IR", and the number here agrees: without it `cse` unifies **253** computations and **69** reads
 * over the corpus, with it **268** and **81**, and the lowered corpus is 118 lines and five
 * `alloca`s smaller for the difference.
 *
 * The type is compared as well as the number, because a `bitcast` is what makes one bit pattern two
 * types and a constant is exactly where the two would otherwise be confused.
 *
 * The use **kind** is compared with the value, because a borrow and an inspection of one value are
 * two different statements about it.
 */
static bool sameOperand(OptContext& opt, Use left, Use right) {
    if(left.packed == right.packed) return true;
    if(!left.isSet() || !right.isSet() || left.kind() != right.kind()) return false;

    auto a = opt.base[left.value()];
    auto b = opt.base[right.value()];
    if(a->type() != b->type() || a->convention != b->convention) return false;

    if(!a->definer || !b->definer) return false;

    auto first = opt.base[a->definer];
    auto second = opt.base[b->definer];
    if(first->kind != Inst::Const || second->kind != Inst::Const) return false;

    return ((InstConst*)first)->constant == ((InstConst*)second)->constant;
}

bool sameComputation(OptContext& opt, Inst& a, Inst& b) {
    if(a.kind != b.kind || !sameImmediates(a, b)) return false;
    if(a.results.size() != b.results.size()) return false;

    for(Size i = 0; i < a.results.size(); i++) {
        auto left = opt.base[a.results.get(opt.base, i)];
        auto right = opt.base[b.results.get(opt.base, i)];

        if(left->occurrence != right->occurrence) return false;
        if(left->convention != right->convention) return false;
    }

    SmallArray<Use, 4> left, right;
    forEachUse(opt.base, a, [&](Use used) { left.push(used); });
    forEachUse(opt.base, b, [&](Use used) { right.push(used); });

    if(left.size() != right.size()) return false;

    for(Size i = 0; i < left.size(); i++) {
        if(!sameOperand(opt, left[i], right[i])) return false;
    }

    return true;
}

bool materializedAtUse(OptContext& opt, Inst& inst) {
    Size operands = 0;
    Size constants = 0;

    forEachUse(opt.base, inst, [&](Use used) {
        operands++;

        auto definer = used.isSet() ? opt.base[used.value()]->definer : nullptr;
        if(definer && opt.base[definer]->kind == Inst::Const) constants++;
    });

    return operands == constants;
}

namespace {
// One read in scope: the address it was made through, the allocation that address is rooted in, and
// the value it answered with. `killedAt` is the tree depth a write invalidated it at and is what
// brings it back when that depth is left - see the file comment.
struct AvailableLoad {
    static constexpr U32 kAlive = ~U32(0);

    Value* address = nullptr;
    Value* allocation = nullptr;
    Value* result = nullptr;
    U32 killedAt = kAlive;
};

struct Eliminator {
    OptContext& opt;
    Dominators& dominators;
    const Contained& contained;

    // The pure computations in scope, which nothing invalidates, and the reads, which anything that
    // writes their storage does.
    SmallArray<Inst*, 32> available;
    SmallArray<AvailableLoad, 8> loads;

    U32 depth = 0;

    Value* valueOf(Use used) { return used.isSet() ? opt.base[used.value()] : nullptr; }

    /*
     * Whether a computation may be answered from an earlier one in **another** block - the one cost
     * rule that survived measurement, see the file comment.
     *
     * Asked of every candidate rather than only of the ones that cross a boundary, and that costs
     * nothing: `simplify` runs immediately in front of this and has already taken every pair
     * standing in one block, so what reaches here is what a tree was needed for.
     */
    bool answerableAcrossBlocks(Inst& inst) {
        if(inst.kind == Inst::Binary) {
            auto op = ((InstBinary&)inst).op;
            auto compares = op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt ||
                            op == BinaryOp::Le || op == BinaryOp::Gt || op == BinaryOp::Ge;
            if(compares) return false;
        }

        return true;
    }

    void killAliasing(Inst& inst) {
        for(Size i = 0; i < loads.size(); i++) {
            if(loads[i].killedAt != AvailableLoad::kAlive) continue;
            if(mayWriteStorage(opt, contained, inst, *loads[i].allocation)) loads[i].killedAt = depth;
        }
    }

    /*
     * Everything that could run between a block's immediate dominator and the block itself, applied
     * to the scope as one invalidation.
     *
     * This is what makes a fact carried down the tree true rather than merely dominated. The scope
     * says an earlier read *dominates* this block, which is a statement about the paths that reach
     * here and none about what happened on them: an arm of a diamond, or a whole loop body, runs
     * after the dominator finishes and before this block starts.
     *
     * Found by walking backwards from the block's predecessors and never stepping through its
     * immediate dominator, which is exactly the blocks that can run in between - every path to here
     * goes through that dominator, so anything reachable backwards from a predecessor without
     * passing it is on such a path. A loop header finds its own body this way, because the latch is
     * one of its predecessors, which is what makes a fact from outside a loop die at the header
     * rather than surviving into a body that overwrites it.
     */
    void killBetween(U32 index) {
        if(!loads.size()) return;

        auto stop = dominators.idom[index];
        auto count = U32(opt.body->blocks.size());

        ScratchSet seen(opt.sets, count);
        SmallArray<U32, 32> pending;

        auto reach = [&](U32 block) {
            if(block == stop || (*seen)[block]) return;

            seen->set(block, true);
            pending.push(block);
        };

        for(auto predecessor: opt.body->block(opt.base, BlockId(index))->incoming(opt.base)) {
            reach(opt.base[predecessor]->index.index);
        }

        while(pending.size()) {
            auto block = opt.body->block(opt.base, BlockId(pending.pop().unwrap()));

            for(auto pointer: block->instructions(opt.base)) killAliasing(*opt.base[pointer]);
            for(auto predecessor: block->incoming(opt.base)) {
                reach(opt.base[predecessor]->index.index);
            }
        }
    }

    /*
     * Whether two addresses name the same bytes: the same allocation, reached by the same sequence
     * of field and element projections.
     *
     * Matching on the address **value** is what this replaces, and the reason is the shape every
     * container has. A body reaches its array through a borrow of the slot - `%a = load %slot`,
     * `Guaranteed`, which is how a borrow is spelled at this tier and is no instruction at all in
     * the lowering - and two blocks that each take one hold two SSA values that nothing unifies:
     * they carry different *occurrences* (a shared borrow in one arm, a unique one in the other),
     * so `isUnifiable` refuses them and rightly, since unifying two borrows moves a loan endpoint.
     * The addresses below them are therefore two chains of `field.addr` over two roots, and the
     * reads at the bottom of them are the same read.
     *
     * `addressStep` is the walk and it is shared with `allocationRoot`, which is what keeps "these
     * two addresses are the same bytes" and "this address is in that allocation" one answer. The
     * significant steps are the projections - a `Guaranteed` load and a `represent` name the storage
     * their operand does and contribute nothing - and each is compared by everything the
     * manifestation reads off it: the record a field belongs to (which is what keeps a window's
     * `.length` apart from its owner's), the field, the shape and the property, and for an element
     * the container and the index value.
     */
    bool sameProjection(Inst& left, Inst& right) {
        if(left.kind != right.kind) return false;

        if(left.kind == Inst::FieldAddress) {
            auto& a = (InstFieldAddress&)left;
            auto& b = (InstFieldAddress&)right;
            return a.record == b.record && a.field == b.field && a.shape == b.shape &&
                   a.property == b.property;
        }

        auto& a = (InstElementAddress&)left;
        auto& b = (InstElementAddress&)right;
        return a.container == b.container && valueOf(a.index) == valueOf(b.index);
    }

    bool samePlace(Value* left, Value* right) {
        auto significant = [&](Value*& at) -> Inst* {
            while(at) {
                auto definer = at->definer;
                if(!definer) return nullptr;

                auto inst = opt.base[definer];
                if(inst->kind == Inst::FieldAddress || inst->kind == Inst::ElementAddress) {
                    return inst;
                }

                if(inst->kind == Inst::Alloc) return inst;

                at = addressStep(opt, at);
            }

            return nullptr;
        };

        while(true) {
            if(left == right) return true;

            auto a = significant(left);
            auto b = significant(right);
            if(!a || !b) return false;

            if(a == b) return true;
            if(a->kind == Inst::Alloc || b->kind == Inst::Alloc) return false;
            if(!sameProjection(*a, *b)) return false;

            left = valueOf(a->kind == Inst::FieldAddress ? ((InstFieldAddress*)a)->base
                                                         : ((InstElementAddress*)a)->base);
            right = valueOf(b->kind == Inst::FieldAddress ? ((InstFieldAddress*)b)->base
                                                          : ((InstElementAddress*)b)->base);
        }
    }

    /*
     * A read this may remember, and the allocation whose writes end it.
     *
     * The address has to be **rooted** in an allocation of this body, which is what makes
     * `mayWriteStorage` answerable about it at all: a global's address, a pointer out of storage and
     * a borrowed parameter have no root here, and a write anywhere might be a write of them.
     */
    Value* readAllocation(Inst& inst) {
        if(inst.kind != Inst::Load) return nullptr;

        auto result = soleResult(opt, inst);
        if(!result || result->convention != Convention::Trivial) return nullptr;

        auto address = valueOf(((InstLoad&)inst).address);
        auto root = allocationRoot(opt, address);
        if(!root) return nullptr;

        // The whole-slot read is the one this declines - see the file comment, where the number is.
        if(root == address) return nullptr;

        return root;
    }

    bool forwardRead(Inst& inst) {
        auto allocation = readAllocation(inst);
        if(!allocation) return false;

        auto result = soleResult(opt, inst);
        auto address = valueOf(((InstLoad&)inst).address);

        // The newest first, so a place written and read again inside one block answers with the read
        // below the write rather than the one above it.
        for(Size i = loads.size(); i-- > 0;) {
            auto& entry = loads[i];
            if(entry.killedAt != AvailableLoad::kAlive) continue;
            if(!samePlace(entry.address, address)) continue;
            if(entry.result->occurrence != result->occurrence) continue;
            if(!isUnifiable(*result, *entry.result)) continue;

            auto editor = opt.ir();
            editor.replaceAllUses(*result, *entry.result);
            editor.eraseInst(inst);

            return true;
        }

        AvailableLoad entry;
        entry.address = address;
        entry.allocation = allocation;
        entry.result = result;
        loads.push(entry);

        return false;
    }

    // One block: the kills the paths into it owe, then its own instructions.
    void enterBlock(U32 index) {
        killBetween(index);

        auto block = opt.body->block(opt.base, BlockId(index));

        for(Size i = 0; i < block->instructionCount();) {
            auto& inst = *opt.base[block->instructionAt(opt.base, i)];

            if(isRemovable(opt, inst) && !materializedAtUse(opt, inst) &&
               answerableAcrossBlocks(inst)) {
                auto result = soleResult(opt, inst);
                Inst* earlier = nullptr;

                if(result) {
                    for(Size a = available.size(); a-- > 0;) {
                        if(!sameComputation(opt, *available[a], inst)) continue;

                        auto with = opt.base[available[a]->results.get(opt.base, 0)];
                        if(!isUnifiable(*result, *with)) continue;

                        earlier = available[a];
                        break;
                    }
                }

                if(earlier) {
                    auto editor = opt.ir();
                    editor.replaceAllUses(*result, *opt.base[earlier->results.get(opt.base, 0)]);
                    editor.eraseInst(inst);

                    continue;
                }

                if(result) available.push(&inst);
                i++;
                continue;
            }

            if(forwardRead(inst)) continue;

            // And what this instruction does to the storage the scope is holding, which is the same
            // invalidation `killBetween` applies for a whole block.
            killAliasing(inst);
            i++;
        }
    }

    /*
     * One walk of the dominator tree, carrying what has been computed on the path from the entry to
     * the block being visited.
     *
     * An explicit stack rather than recursion, which is `dominance.cpp`'s rule at the same question:
     * a body is not bounded by anything that would make a walk of its CFG safe on the C++ stack.
     */
    void run() {
        struct Frame {
            U32 index = 0;
            U32 cursor = 0;
            Size values = 0;
            Size reads = 0;
        };

        auto entry = opt.base[opt.body->entry]->index.index;
        if(!dominators.isReachable(entry)) return;

        SmallArray<Frame, 32> stack;

        auto push = [&](U32 index) {
            Frame frame;
            frame.index = index;
            frame.values = available.size();
            frame.reads = loads.size();
            stack.push(frame);

            depth = U32(stack.size()) - 1;
            enterBlock(index);
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

            depth = U32(stack.size()) - 1;

            while(available.size() > frame.values) available.pop();
            while(loads.size() > frame.reads) loads.pop();

            /*
             * And the entries anything in this subtree invalidated, which are live again for
             * whatever the walk visits next: a sibling subtree is reached by a path that ran none of
             * this one.
             *
             * `>= depth` rather than `>` because this block's own kills are recorded at its own
             * depth, and they are as much this subtree's as a child's are. They had to hold for the
             * rest of the block and for everything under it, which they did - the restore is at the
             * end.
             */
            for(Size i = 0; i < loads.size(); i++) {
                if(loads[i].killedAt >= depth) loads[i].killedAt = AvailableLoad::kAlive;
            }

            stack.pop();
        }
    }
};

} // namespace

void cse(OptContext& opt) {
    Eliminator eliminator { opt, dominatorsOf(opt), containedOf(opt) };
    eliminator.run();
}

} // namespace ssa
