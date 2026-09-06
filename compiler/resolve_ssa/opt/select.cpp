#include "pass.h"

/*
 * If-conversion: a branch whose two arms only *compute a value* stops being a branch.
 *
 * The shape is what every conditional expression leaves behind at this tier - a `branch`, two arms
 * that merge nothing but a block argument, and a join that declares it:
 *
 *     bb0: %c = gt %x, %y          bb0: %c = gt %x, %y
 *          branch %c -> bb1 -> bb2      %v = select %c, 1, 2
 *     bb1: jump bb3(1)                  jump bb3(%v)
 *     bb2: jump bb3(2)             bb3(%a): ...
 *     bb3(%a): ...
 *
 * and what is on the right is one instruction the machine already has: a `cmov` natively, with the
 * comparison's flags folded straight into it, and no branch to predict or two-block detour to lay
 * out. It also makes the *result* a value every pass below can see - a select is `kSsaPure`, so the
 * folder decides one whose condition is known and the block-local CSE unifies two of them, where a
 * diamond is three blocks that stop `forwardMemory` and `simplify`'s CSE at their boundaries.
 *
 * ## What makes it sound, which is one sentence
 *
 * **Both arms are evaluated.** So an arm may hold only instructions that may be computed on a path
 * that would not have reached them, and everything else here follows from that:
 *
 *  - **`kSsaPure`, plus one read that is not.** That column says an instruction may be recomputed or
 *    not computed at all, which is exactly the question this is asking. What it excludes is the whole
 *    of what makes an arm an arm rather than an expression: a `Call` (anything at all), a `Store` or
 *    an `Alloc` (storage the analyses below believe holds something else), a `Copy`, an `Atomic` (a
 *    synchronization edge), a `Native` (a system call) and a `Drop` (a teardown that must run on one
 *    path and not the other, which is the case drop flags exist for). The exception is a whole
 *    `Load` of a frame slot, and `speculatableLoad` is the argument for it - without which this pass
 *    converts the arithmetic diamonds and none of the boolean ones.
 *  - **A division is declined even though it is pure**, and here that is a cost rather than a fault:
 *    `makeDivisionTotal` runs one tier down and gives `x / 0` the answer the language specifies, so
 *    what speculating one costs is the divide *and* the test that totalization adds, on every path.
 *    That is the most expensive arithmetic there is, put where it was written not to be.
 *  - **The linear rule is a precondition rather than a check**, which is README.md's section B row
 *    for this pass. An arm that hands an owner over its edge, or drops one, is not a value
 *    computation at all: the select would consume it on both paths where the branch consumed it on
 *    one. So every join argument and every operand feeding one is required to be `Trivial`, and an
 *    arm instruction is held to `simplify`'s `isRemovable` rule - no tracked operand, no `Consume`
 *    use, and results that are `Trivial` or `Address`. A `Guaranteed` **result** is what that last
 *    clause declines, and for `replaceAllUses`' reason: what a borrow is checked against is its loan,
 *    and two arms borrowing different storage are two loans a select would have to be on both of.
 *
 *    A `Guaranteed` **operand** is admitted, because `isTracked` is `Owned` and `Affine` and no
 *    more - and that is sound rather than an oversight, on the one property the rewrite rests on: an
 *    arm's instruction moves to the end of the head, which is *earlier* on the path it was already
 *    on and no later on the other, so a use of a borrow cannot cross an endpoint it stood in front
 *    of. What it reaches in practice is a `field.addr` of a borrowed parameter, whose own reader is
 *    a load through a projection and therefore not speculatable - so the arm is refused one
 *    instruction further down and the operand clause has never yet decided anything.
 *
 * The rest of the pure roster is safe to run unconditionally and was checked one kind at a time. A
 * `Const`, a `GlobalAddress`, a `GenValue` and a `Metric` read nothing - the widest thing any of
 * them lowers to is a load of an image table cell written once when the image is assembled. An
 * `ElementAddress` is `base + index * stride` and an address computation is not an access, so an
 * index out of range makes a pointer nothing dereferences; `elementAddress` in `lower/inst.cpp`
 * asserts about the *container* being a fixed array and about nothing the index is. A
 * `FieldAddress` is a constant offset, with one case that is not - a **boxed** field, where
 * `fieldAddress` loads the slot's pointer - and that load reads a slot the record has whatever path
 * arrived, and hands back a pointer this arm does not follow. `Fma`, `Select`, `Binary`, `Unary`,
 * the five vector rows and `AtomicOk` read their operands and nothing else.
 *
 * The second condition is a **cost** and has no correctness in it: both arms now run, so the work
 * they do is paid for on every path. `kMaxSpeculated` is the budget.
 *
 * ## What the block-argument representation makes easy, and what it makes impossible
 *
 * Two of the shipping stage's five rulings ([[yana-if-conversion]]) have no counterpart here, and
 * both are the same difference - a join declares its arguments and every edge supplies them, where a
 * phi is a value *in* the join naming its predecessors.
 *
 * **The join keeps its other predecessors, without a rewrite.** A `match` of three cases is a
 * diamond feeding a join a third arm also reaches, and over there the two folded alternatives had to
 * be replaced by one arriving from the head. Here the head's new edge simply supplies the select and
 * every other edge is untouched, so nothing has to be said about it.
 *
 * **A join that is a loop header needs no case either**, where the shipping stage declines an
 * alternative that is a phi of the join. Its reason is that phis happen at once and the selects
 * replacing them are instructions in a row, so one reading another would read the new value. Nothing
 * here replaces anything at the join: the select is built in the *head*, which is a predecessor, so
 * an operand that is one of the join's own arguments is the value the current iteration has - which
 * is what an edge operand means. The parallel copy stays where it was.
 *
 * What the representation makes impossible is the **triangle** with a value in it. `Editor::
 * makeBranch` pushes one argument list onto both edges - this tier's rule that a value live before a
 * branch is live on both sides of it - so a branch whose two targets declare different arguments is
 * IR that cannot be written. A triangle's two targets are its arm and the join, the arm declares no
 * arguments (`armEdge` requires it), and therefore the join declares none either: the shape removes a
 * branch and produces no select at all.
 *
 * It is written anyway, and it converts nothing. Measured over the corpus the shape is *matched*
 * seventeen times and converted zero, and the reason is what a triangle is in the source: `if c:` with
 * no else, whose arm is there because it has an effect - a call or a store, neither of which is
 * speculatable. An arm that computed only values would be one whose values nothing can read, since it
 * dominates nothing past itself, and `simplify` plus `cleanCfg`'s splice have already taken that one.
 * So the case is here because "this shape needs no rule of its own" and "this shape never arrives"
 * are different statements and it is the second one that is true.
 *
 * ## Where it runs
 *
 * Between `cleanCfg` and `threadBranches`, which is README.md's section B rows 6 and 7. The fold has
 * first claim - a deleted arm beats a select over both - and threading is behind this one because a
 * diamond that could be one expression should be one before a predecessor is sent past its join:
 * threading duplicates the decision onto an edge, and once it has, there is no diamond left to
 * convert. `cfg.cpp`'s file comment says the same thing from the other side.
 *
 * What this leaves behind is a join with one predecessor left and two dead arms, which is exactly
 * the shape `cleanCfg` folds back into the head - so the round after a conversion is what turns
 * three blocks into one, and a nested diamond converts one round at a time from the inside out.
 */

namespace ssa {

namespace {

/*
 * How much work one conversion may make unconditional.
 *
 * Four, across **both** arms rather than per arm, because what is paid for is the total: an arm of
 * three and an empty one costs what two of one and a half do. It is the shipping stage's number,
 * taken as the starting one rather than re-derived - the shapes this exists for are small, and both
 * of the two that dominate the corpus (`if c then a else b` over values already computed, and a
 * short-circuit `&&` whose skipped arm is a literal) compute nothing at all in either arm. Raise it
 * against a measurement, which is what the shipping stage's own comment asks for.
 */
constexpr Size kMaxSpeculated = 4;

/*
 * Which two edges feed the join, and which blocks stop existing.
 *
 * `arms` are the blocks spliced out and the two edges are what the selects are built from; they are
 * not the same list, because a triangle has one arm and takes one of its edges from the branch
 * itself. See the file comment for why a triangle carries no value here.
 */
struct Diamond {
    Block* join = nullptr;
    Edge* whenTrue = nullptr;
    Edge* whenFalse = nullptr;
    Block* arms[2] = { nullptr, nullptr };
};

struct SelectConverter {
    OptContext& opt;

    // The two region readers `cfg.cpp` has, asked for the same two reasons: a block a `Continuation`
    // names is one this directory does not delete, and a region's `entry` is the one point it may be
    // entered at. Duplicated rather than shared because they are three lines each and the alternative
    // is a header for two predicates over a field.
    bool isAnchor(Block& block) {
        for(auto pointer: opt.body->regions.contents(opt.base)) {
            auto region = opt.base[pointer];

            if(region->entry.valid() && region->entry == block.index) return true;
            if(region->destination.valid() && region->destination == block.index) return true;
        }

        return false;
    }

    bool isRegionEntry(Block& block) {
        auto region = opt.body->region(opt.base, block.region);
        return region && region->entry.valid() && region->entry == block.index;
    }

    /*
     * Whether a value of this type is one a select may hold, which is the question "does it live in
     * a register" asked one tier above the layer that decides register widths.
     *
     * An integer, a float, a pointer and a payload-free sum are the four, and each of them is a
     * **structural** answer rather than a target's: a record is address-only on every target
     * (`lower/README.md`'s first section), a `RecordLayout::Enum` is a tag whose bits are constructor
     * numbers, and a pointer is an address that *is* the value. That is what keeps this out of
     * `fold.cpp`'s refusal to fold a target-answered width - how wide a `Size` turns out to be is the
     * representation layer's, and whether it is in a register at all is not.
     *
     * **A vector is declined**, and that is the one that would otherwise look like an omission.
     * `InstSelect` is also `Lanewise.select`, whose condition is a `Mask` of the arms' own shape -
     * `validateSelect` in `lower/lower_validate.cpp` holds it to that - and the condition this pass
     * has is a branch's, which is a `Bool`. A vector chosen by one boolean is a shape no backend
     * holds. An `Array`, a `Fun` and a `Variable` are declined for the first reason instead: each is
     * a value the IR names by the address of its storage, so selecting between two of them would be
     * selecting addresses and would mean something else.
     */
    bool selectableType(DeclPtr<mir::Type> type) {
        if(!type) return false;

        auto& logical = *opt.decl[type];
        if(logical.kind == mir::Type::Int || logical.kind == mir::Type::Float) return true;
        if(logical.kind == mir::Type::Pointer) return true;

        return logical.kind == mir::Type::Record &&
               ((mir::RecordType&)logical).layout == mir::RecordLayout::Enum;
    }

    /*
     * The one instruction that is **not** pure and is still safe here: a whole read of a slot this
     * frame allocated.
     *
     * `inst.def` leaves `Load` off `kSsaPure` for the reason its own comment gives - a load reads
     * storage another instruction may write, which is a question about aliasing rather than about
     * the instruction. That question does not arise at this site, and the reason is that **an arm's
     * instructions move across nothing**: the arm begins immediately after the branch, so a load
     * lifted out of it is placed at the end of the head with only the branch and the other arm's
     * instructions between it and where it was - and the other arm is held to this same rule, so it
     * holds no write either. The value the load produces is therefore the value it produced before.
     *
     * What is left is whether the read itself can fail, and there are two halves. The **storage** is
     * a slot this body allocated, so it is there whatever path arrived - which is why the address has
     * to be the `Alloc` itself and not a projection of one: `xs[i]` past the end is the canonical way
     * an if-conversion invents a fault, and a field of a partially-moved record is a place with
     * nothing at it. And the **contents** may be whatever the path that skipped this arm left there,
     * which is a number nobody reads: the select discards it, and requiring the result to be
     * `Trivial` is what says the value carries no obligation and is not an address something below
     * would follow.
     *
     * It is not optional, and that is the shipping stage's finding arriving here with a number:
     * without it this pass fires 99 times over the corpus and the `Bool` rewrite in `simplify.cpp`
     * fires **zero**, because every arm of a short-circuit `&&` over computed booleans holds the
     * `load` of a parameter slot that `forwardMemory` cannot answer - the store into that slot hands
     * over a `Guaranteed` borrow and the load produces a `Trivial` value, which is not a
     * substitution. `larger`'s two arms in `Select.yana` are one load each and nothing else.
     */
    bool speculatableLoad(Inst& inst) {
        if(inst.kind != Inst::Load) return false;

        auto& load = (InstLoad&)inst;
        if(!load.address.isSet()) return false;

        auto address = opt.base[load.address.value()];
        auto definer = address->definer ? opt.base[address->definer] : nullptr;
        if(!definer || definer->kind != Inst::Alloc) return false;

        if(!inst.results.size()) return false;
        return opt.base[inst.results.get(opt.base, 0)]->convention == Convention::Trivial;
    }

    /*
     * Whether one instruction may be computed on a path that would not have reached it - the file
     * comment's one sentence, and the ownership rule `simplify::isRemovable` states for the same
     * reason in the other direction.
     *
     * The two questions are the same question: an instruction that may be *erased* without changing
     * what the linear rule sees is one that may be *run twice* without changing it either. What is
     * added here is the division, which is a cost.
     */
    bool speculatable(Inst& inst) {
        if(!isPure(inst.kind) && !speculatableLoad(inst)) return false;

        if(inst.kind == Inst::Binary) {
            auto op = ((InstBinary&)inst).op;
            if(op == BinaryOp::Div || op == BinaryOp::Rem) return false;
        }

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

    // The instructions of one arm, or nothing where one of them may not be made unconditional. The
    // terminator is not counted: it is the jump this conversion removes.
    Maybe<Size> armCost(Block& arm) {
        Size count = 0;

        for(auto pointer: arm.instructions(opt.base)) {
            auto inst = opt.base[pointer];
            if(isTerminator(inst->kind)) break;

            if(!speculatable(*inst)) return Nothing();
            count++;
        }

        return Just(count);
    }

    /*
     * The block one arm of a branch is, answered as the edge it leaves by - or null where it is not
     * an arm at all.
     *
     * Five questions, and each is a way the conversion would be wrong rather than merely
     * unprofitable. The branch has to be the **only** way in, or removing the edge strands a path
     * that still needs the block. It must declare **no arguments**, since those are values the head's
     * branch supplies and there is nothing left to supply them once the arm is gone - and one
     * argument list serves both of that branch's edges, so an arm that declared any would force the
     * other side to declare the same. It has to leave by one plain **jump**, because a block that
     * branches again is a second decision and not an arm. It must be in the head's **region**, which
     * is `cfg.cpp`'s rule in its fifth spelling. And it must not be an **anchor**, because this
     * deletes it.
     */
    Edge* armEdge(Block& head, SsaPtr<Block> pointer) {
        if(!pointer) return nullptr;

        auto block = opt.base[pointer];
        if(block == &head || block->deleted) return nullptr;
        if(block == opt.base[opt.body->entry]) return nullptr;

        if(block->argumentCount()) return nullptr;
        if(block->predecessorCount() != 1) return nullptr;
        if(opt.base[block->predecessorAt(opt.base, 0)] != &head) return nullptr;
        if(block->region != head.region || isAnchor(*block)) return nullptr;

        auto last = block->terminator(opt.base);
        if(!last || last->kind != Inst::Jump) return nullptr;

        auto& jump = (InstJump&)*last;
        return jump.to.target ? &jump.to : nullptr;
    }

    /*
     * Which shape this branch is, if it is one of the two.
     *
     * Both edges at one block is a branch that decides nothing and is `cleanCfg`'s `foldBranch`; a
     * join that is the head is a loop, and turning its branch into a jump would be an infinite one.
     */
    bool findDiamond(Block& head, InstBranch& branch, Diamond& result) {
        if(branch.whenTrue.target == branch.whenFalse.target) return false;

        auto fromTrue = armEdge(head, branch.whenTrue.target);
        auto fromFalse = armEdge(head, branch.whenFalse.target);

        if(fromTrue && fromFalse && fromTrue->target == fromFalse->target) {
            result.join = opt.base[fromTrue->target];
            result.whenTrue = fromTrue;
            result.whenFalse = fromFalse;
            result.arms[0] = opt.base[branch.whenTrue.target];
            result.arms[1] = opt.base[branch.whenFalse.target];
        } else if(fromTrue && fromTrue->target == branch.whenFalse.target) {
            // The false side is the join, reached directly: `if c then <pure>` with nothing on the
            // other side. See the file comment for why this carries no value here.
            result.join = opt.base[branch.whenFalse.target];
            result.whenTrue = fromTrue;
            result.whenFalse = &branch.whenFalse;
            result.arms[0] = opt.base[branch.whenTrue.target];
        } else if(fromFalse && fromFalse->target == branch.whenTrue.target) {
            result.join = opt.base[branch.whenTrue.target];
            result.whenTrue = &branch.whenTrue;
            result.whenFalse = fromFalse;
            result.arms[0] = opt.base[branch.whenFalse.target];
        } else {
            return false;
        }

        if(result.join == &head || result.join->deleted) return false;

        // The join in the head's region, and not the one point that region is entered at: every
        // predecessor being redirected is inside it, and an edge from inside to the entry is the
        // re-entry `Continuation::multiplicity` forbids. Both hold by construction where the arms
        // already reach the join from this region, and both are checked for `cfg.cpp`'s reason -
        // the cost is a comparison and the alternative is an argument a later kind of region has to
        // be re-checked against.
        if(result.join->region != head.region || isRegionEntry(*result.join)) return false;

        return true;
    }

    // What one edge hands the join's argument `index`.
    Use operandAt(Edge& edge, Size index) { return edge.arguments.get(opt.base, index); }

    /*
     * Whether the join's arguments and the two operands feeding each of them are ones a select may
     * carry - the linear rule as a precondition, and the register-shape question beside it.
     *
     * All or nothing, checked before anything moves: the branch is what tells the two sides apart
     * and there is no half of the conversion to leave behind.
     */
    bool selectableJoin(Diamond& diamond) {
        auto& join = *diamond.join;

        if(join.argumentCount() != diamond.whenTrue->arguments.size()) return false;
        if(join.argumentCount() != diamond.whenFalse->arguments.size()) return false;

        for(Size i = 0; i < join.argumentCount(); i++) {
            auto argument = opt.base[join.argumentAt(opt.base, i)];
            if(argument->convention != Convention::Trivial) return false;
            if(!selectableType(argument->type())) return false;

            auto whenTrue = operandAt(*diamond.whenTrue, i);
            auto whenFalse = operandAt(*diamond.whenFalse, i);
            if(!whenTrue.isSet() || !whenFalse.isSet()) return false;

            // The operands as well as the argument, because `checkEdge` compares an occurrence and a
            // convention on every edge and the select's result has to satisfy the join with one
            // answer for both sides.
            for(auto used: { whenTrue, whenFalse }) {
                auto value = opt.base[used.value()];
                if(value->convention != Convention::Trivial) return false;
                if(value->occurrence != argument->occurrence) return false;
            }
        }

        return true;
    }

    bool convert(Block& head) {
        auto last = head.terminator(opt.base);
        if(!last || last->kind != Inst::Branch) return false;

        auto& branch = (InstBranch&)*last;
        if(!branch.condition.isSet()) return false;

        /*
         * The condition is a `Bool`, asked rather than assumed.
         *
         * Every branch this stage builds has one, and nothing verifies it - `verifyBody` has no rule
         * about a terminator's condition type. What the select needs it for is the tier below:
         * `validateSelect` requires a scalar select's condition to be one `Int32`, which is what a
         * `Bool` manifests as, and the same instruction with a wider condition is the *lane-wise*
         * select, whose condition is a mask. So this is where the two are told apart, on the same
         * terms `simplify`'s `Bool` rewrite tells them apart one pass later.
         */
        auto boolean = mir::booleanType(opt.program);
        if(!boolean || opt.base[branch.condition.value()]->type() != boolean) return false;

        Diamond diamond;
        if(!findDiamond(head, branch, diamond)) return false;

        Size cost = 0;
        for(auto arm: diamond.arms) {
            if(!arm) continue;

            auto size = armCost(*arm);
            if(!size) return false;

            cost += size.unwrap();
        }

        if(cost > kMaxSpeculated) return false;
        if(!selectableJoin(diamond)) return false;

        /*
         * Everything the rewrite reads, read out before it starts.
         *
         * The two edges are fields of terminators this is about to erase, and the operands they hand
         * over stop being reachable through them the moment an arm is deleted - so the pairs are
         * copied out first and the edges are not consulted again.
         */
        auto condition = opt.base[branch.condition.value()];
        auto source = last->source;
        auto join = diamond.join;

        SmallArray<Use, 4> whenTrue, whenFalse;
        for(Size i = 0; i < join->argumentCount(); i++) {
            whenTrue.push(operandAt(*diamond.whenTrue, i));
            whenFalse.push(operandAt(*diamond.whenFalse, i));
        }

        auto editor = opt.ir();

        /*
         * The arms move up first, so that a value one of them computed is defined above the select
         * that reads it.
         *
         * Moving is all it takes and the reason is dominance: the head is an arm's only predecessor,
         * so everything an arm's instruction reads is either defined in the head or dominates it, and
         * both are available at the end of the head. `moveInst` rather than a remove and an add, for
         * `build.h`'s reason - re-adding registers every operand a second time.
         */
        for(auto arm: diamond.arms) {
            if(!arm) continue;

            for(;;) {
                if(!arm->instructionCount()) break;

                auto inst = opt.base[arm->instructionAt(opt.base, 0)];
                if(isTerminator(inst->kind)) break;

                editor.moveInst(*inst, head, head.instructionCount() - 1);
            }
        }

        /*
         * One select per join argument, in front of the branch and therefore below everything that
         * just moved.
         *
         * Two operands that are one value need no select at all. Not building one keeps the join's
         * edge pointed at the value itself rather than at a temporary `simplify` would remove on its
         * next visit - which is the shipping stage's reading and holds here for the same reason.
         *
         * The three operands are `Inspect`, where the edge handed them over as `Consume`. That is
         * what a select does with a value - it reads three and takes none - and it is load-bearing
         * rather than cosmetic: `simplify::isRemovable` declines an instruction with a consuming use,
         * so a select built with the edge's own kinds would be one the `Bool` rewrite below never
         * looks at. Every operand here is `Trivial`, so neither kind is an obligation.
         */
        editor.setInsertBefore(*last);

        SmallArray<Use, 4> arguments;
        for(Size i = 0; i < whenTrue.size(); i++) {
            if(whenTrue[i].value() == whenFalse[i].value()) {
                arguments.push(whenTrue[i]);
                continue;
            }

            auto argument = opt.base[join->argumentAt(opt.base, i)];
            auto select = editor.addSelect(source, editor.use(*condition, UseKind::Inspect),
                                           whenTrue[i].withKind(UseKind::Inspect),
                                           whenFalse[i].withKind(UseKind::Inspect),
                                           argument->occurrence, Convention::Trivial);

            // The kind the true side's edge already used, so that a converted join reads like the
            // ones around it. Cosmetic where every operand is `Trivial`, which is what was checked.
            arguments.push(editor.use(*editor.resultOf(*select), whenTrue[i].kind()));
        }

        /*
         * And the branch itself, which becomes a jump straight to the join.
         *
         * The head's edge into the join is created only where it did not already have one, and that
         * is `replaceTerminator`'s multiset rule rather than a test written here: a triangle's branch
         * already led to the join down one side, and that is the edge that survives - arguments
         * included, which is why the triangle case above is the one that carries none.
         */
        editor.replaceTerminator(head, editor.makeJump(source, *join, arguments.pointer(),
                                                       arguments.size()));

        /*
         * The arms, which now have no way in at all.
         *
         * Deleted here rather than left for `cleanCfg`'s sweep, on the rule a pass owes the verifier
         * IR it accepts at its own checkpoint: an unreachable block is checked structurally, so an
         * arm left holding a jump into the join would be reported at this pass's checkpoint for the
         * edge it still owns. `deleteBlock` is legal in exactly this state - the incoming list was
         * emptied by the replacement above, and everything the arm still holds is its own jump.
         */
        for(auto arm: diamond.arms) {
            if(arm) editor.deleteBlock(*arm);
        }

        return true;
    }

    void run() {
        // Over a snapshot of the table, because a conversion deletes blocks the walk would otherwise
        // still be pointed at. Nothing here makes a block, so one visit per block is one attempt at
        // the branch it ends with, and a nest converts one round at a time.
        SmallArray<Block*, 32> blocks;
        for(auto pointer: opt.body->blocks.contents(opt.base)) blocks.push(opt.base[pointer]);

        for(auto block: blocks) {
            if(block->deleted) continue;
            convert(*block);
        }
    }
};

} // namespace

void convertSelects(OptContext& opt) {
    SelectConverter converter { opt };
    converter.run();
}

} // namespace ssa
