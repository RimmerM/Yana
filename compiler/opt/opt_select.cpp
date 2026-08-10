#include "opt_pass.h"

/*
 * If-conversion: a branch whose two arms only *compute a value* stops being a branch.
 *
 * The shape is the one every conditional expression leaves behind - a `je`, two arms that merge
 * nothing but a value, and a phi at the join:
 *
 *     b0: %c = cmp_gt %x, 0        b0: %c = cmp_gt %x, 0
 *         je %c, b1, b2                %v = select %c ? 1 : 2
 *     b1: jmp b3                       ret %v
 *     b2: jmp b3
 *     b3: %v = phi [b1, 1], [b2, 2]
 *         ret %v
 *
 * and what is on the right is one instruction both targets already have. Natively it is a `cmov`,
 * which the x64 selector folds the comparison's flags straight into - no branch to predict and no
 * two-block detour to lay out. On JS it is `c ? a : b`, which matters more than it looks: an `if`
 * there is a *statement*, so the value has to be a `var` the two arms assign, and the assignment
 * cannot be inlined into whatever reads it. Converting collapses the statement, the variable and
 * the two assignments together, and then js/opt.cpp inlines the ternary into its one use.
 *
 * ## Why here rather than in either backend
 *
 * The same argument the directory is built on. Both backends would have to recognize the same
 * diamond over their own value model, one of them (JS) has already turned the join into a variable
 * by the time it could look, and a rule written twice is a rule that eventually differs. Doing it
 * once above the fork also means the *result* is optimized: a select is a pure value, so the folder
 * decides one whose condition is known, CSE unifies two of them, and the inliner carries one into a
 * caller like any other computation.
 *
 * ## What makes it sound, which is one sentence
 *
 * **Both arms are evaluated.** So an arm may only hold instructions that may be computed
 * unconditionally - `speculatable` below - and everything else about the pass follows from that:
 *
 *  - the ownership instructions are excluded because they are not computations at all. An `init`
 *    that ran on a path it should not have is a write to storage the analyses believe still holds
 *    something else, and a conditional `drop` is exactly the case drop flags exist for;
 *  - a read is excluded wherever reaching the storage could fail - `xs[i]` on the arm a bounds test
 *    guards is the canonical way an if-conversion invents a fault. A whole scalar local is the one
 *    exception, and `speculatable` is where the argument for it is;
 *  - a division is excluded even though it is pure. It is the one arithmetic instruction that
 *    traps, and speculating a divide is a pessimization even where the divisor is known good.
 *
 * The second condition is a *cost* one and has no correctness in it: both arms now run, so the work
 * they do is paid for on every path. `kMaxSpeculated` is the budget, and it is small because the
 * shapes this exists for are small - a constant per arm, a comparison, a negation.
 *
 * ## The join keeps its other predecessors
 *
 * A phi is not required to have only these two alternatives, and refusing one that has more would
 * decline most of what this is for: a `match` of three cases is a diamond feeding a join that a
 * third arm also reaches. So the two alternatives being folded are replaced by *one*, arriving from
 * the head - and where they were the only two, the phi is gone and the join usually merges into the
 * head on the way out. That is also how nested conditionals convert: the inner diamond becomes a
 * select in the arm of the outer one, which is then an arm holding one speculatable instruction, so
 * the next round of the driver converts that too.
 */

namespace {

/*
 * How much work one conversion may make unconditional.
 *
 * Counted in instructions across both arms rather than per arm, because what is being paid for is
 * the total: an arm of three and an empty one costs the same as two of one and a half. Four is
 * chosen against the shapes this pass exists for - `x > 0 ? 1 : -1` computes nothing at all in
 * either arm, and a `match` arm that folds to a constant computes nothing either - so the budget is
 * what keeps an arm holding real arithmetic from being straight-lined into every path.
 */
constexpr Size kMaxSpeculated = 4;

bool selectableType(OptContext& opt, TypePtr type);

/*
 * Whether this instruction may be computed on a path that would not have reached it. Pure is
 * necessary and not sufficient: see the file comment for the division, which is the one pure
 * instruction in the IR that can fault.
 *
 * And one read that is not pure and is still safe, because the loop accumulator depends on it:
 *
 *     while i < limit:
 *         best = if candidate < best then candidate else best
 *
 * whose second arm is `load %best` and nothing else. `isPureValue` declines every load, for the
 * reason its own comment gives - a load reads storage something else may be writing, which is a
 * question about aliasing rather than about the instruction. That question does not arise here:
 * everything in an arm moves to the *end* of the head, so no write it followed stops preceding it,
 * and the only other thing that moves is the other arm, which holds no writes either.
 *
 * What is left is whether the read itself can fail, and the answer is no for exactly this shape: a
 * whole scalar local. The storage is a slot this frame allocated, so it is there whatever path
 * arrived; there is no index to be out of range and no pointer to be null; and a slot holding
 * something the other path had not written yet is a number nobody reads - the select discards it.
 *
 * A projection of any kind is declined rather than reasoned about. `xs[i]` past the end is the
 * canonical way an if-conversion invents a fault, and `p.field` where `p` is a reference that this
 * path never gave one is a `TypeError` on the managed target even where it is only a wasted load on
 * the other.
 */
bool speculatable(OptContext& opt, Value& value) {
    if(value.kind == Value::Div || value.kind == Value::Rem) return false;
    if(isPureValue(value)) return true;

    if(value.kind != Value::LoadPlace || !selectableType(opt, value.type)) return false;

    auto& place = ((InstLoadPlace&)value).place;
    return place.root == PlaceRoot::Local && place.projections.isEmpty();
}

/*
 * Whether a value of this type is one a select can hold.
 *
 * An integer, a float and a pointer are the three things that live in a register, which is
 * `validateSelect`'s list in the lower IR. An enum record is one too - `lowerType` answers `Int32`
 * for one, and a `Bool` is the enum this is asked about most - and unit merges nothing.
 *
 * **A pointer is a value here and a memory type is not**, and that distinction is the whole of why
 * this is stricter than the lower IR's own test. `lowerType` answers `Pointer` for both: a `Ptr` or
 * a `Borrow` *is* an address, so selecting between two of them is selecting a value, while a record
 * is a value the IR names by the address of its storage, and a phi of two of those is a phi of
 * addresses that the passes below read as one aggregate. Selecting those addresses would be
 * mechanically fine and would mean something else - so the line is drawn where the address is the
 * value rather than where the register is wide enough.
 *
 * The pointer case is not reached until ownership has been discharged, which is what makes it a
 * plain value join: `dischargeOwnership` runs before any function is optimized, so a drop is an
 * ordinary call by the time a diamond gets here and there is no transfer left for a select to be
 * ambiguous about. `speculatable` still declines everything that writes storage, so neither arm can
 * be what *produced* the storage its address names. The two analyses that reason about where an
 * address went are unaffected for two different reasons, and both were checked rather than assumed:
 * `analyze_provenance.cpp` runs in `runProgramOwnership`, which is before this stage entirely, and
 * `computeContainment`/`borrowEndsAtItsCalls` decline any reader they do not recognize - so a local
 * whose borrow reached a select is simply not contained.
 *
 * `Ptr` is what the corpus reaches: `mapMemory` answering zero on a failed `mmap` is 46 sites across
 * `test/resolve`, and it is the case §15.5 asked for. `Borrow` is here because it is the same
 * statement - an address that is the value - and not because anything builds one today; there is no
 * surface syntax whose two arms produce borrows rather than the storage behind them.
 */
bool selectableType(OptContext& opt, TypePtr type) {
    if(!type) return false;

    auto value = opt.global[type];
    if(value->kind == Type::Int || value->kind == Type::Float) return true;
    if(value->kind == Type::Ptr || value->kind == Type::Borrow) return true;

    return value->kind == Type::Record && ((RecordType*)value)->layout == RecordType::Enum;
}

/*
 * The block one arm of a branch is, answered as the join it leads to - or null where it is not one.
 *
 * Three questions, and each is a way the conversion would be wrong rather than merely unprofitable:
 * the branch has to be the *only* way in, or removing the edge strands a path that still needs the
 * block; nothing may merge inside it, since a phi there is a value produced by predecessors that no
 * longer exist; and it has to leave by one plain jump, because a block that branches again is a
 * second decision and not an arm at all.
 */
ModulePtr<Block> armTarget(OptContext& opt, ModulePtr<Block> head, ModulePtr<Block> pointer) {
    if(pointer == head) return nullptr;

    auto block = opt.local[pointer];

    // Something has to be first, and the entry block is reached without an edge - so one incoming
    // edge does not mean one way in for it.
    if(block->index == 0) return nullptr;

    if(block->phiCount() != 0) return nullptr;
    if(block->predecessorCount() != 1 || block->predecessorAt(opt.local, 0) != head) return nullptr;
    if(!block->terminator() || opt.local[block->terminator()]->kind != Value::Jmp) return nullptr;

    return ((InstJmp&)*opt.local[block->terminator()]).target;
}

// The instructions of one arm, or nothing where one of them may not be made unconditional.
Maybe<Size> armCost(OptContext& opt, ModulePtr<Block> arm) {
    Size count = 0;

    for(auto pointer: opt.local[arm]->instructions(opt.local)) {
        if(!speculatable(opt, *opt.local[pointer])) return Nothing();
        count++;
    }

    return Just(count);
}

// The alternative a phi takes over the edge from one block, or null where it names no such edge.
ModulePtr<Value> inputFrom(OptContext& opt, InstPhi& phi, ModulePtr<Block> from) {
    for(auto input: phi.inputs.contents(opt.local)) {
        if(input.block == from) return input.value;
    }

    return nullptr;
}

// Whether a value is one of the phis of this block, which is what a select may not be built out of.
// See the guard in `convertBranch` for why.
bool isPhiOf(OptContext& opt, Block& block, ModulePtr<Value> value) {
    for(auto phi: block.phis(opt.local)) {
        if((ModulePtr<Value>)phi == value) return true;
    }

    return false;
}

/*
 * Which two edges of a join the branch at `head` decides between, where it decides between two.
 *
 * `arms` are the blocks spliced out, and `sources` are the predecessors of the join whose
 * alternatives become the two sides of each select. They are not the same list: a *triangle* - one
 * arm, with the other side of the branch going straight to the join - has one arm and one of its
 * sources is the head itself.
 */
struct Diamond {
    ModulePtr<Block> join = nullptr;
    ModulePtr<Block> trueSource = nullptr;
    ModulePtr<Block> falseSource = nullptr;
    ModulePtr<Block> arms[2] = { nullptr, nullptr };
};

bool findDiamond(OptContext& opt, ModulePtr<Block> head, InstJe& branch, Diamond& result) {
    // Both edges at one block is a branch that decides nothing, and its join has two alternatives
    // arriving from the same predecessor - which is not a shape with two sides to select between.
    if(branch.thenBlock == branch.elseBlock) return false;

    auto fromThen = armTarget(opt, head, branch.thenBlock);
    auto fromElse = armTarget(opt, head, branch.elseBlock);

    result.trueSource = branch.thenBlock;
    result.falseSource = branch.elseBlock;

    if(fromThen && fromThen == fromElse) {
        result.join = fromThen;
        result.arms[0] = branch.thenBlock;
        result.arms[1] = branch.elseBlock;
    } else if(fromThen == branch.elseBlock) {
        // The `else` side is the join: `if c then f(x)` with nothing on the other side.
        result.join = branch.elseBlock;
        result.arms[0] = branch.thenBlock;
        result.falseSource = head;
    } else if(fromElse == branch.thenBlock) {
        result.join = branch.thenBlock;
        result.arms[0] = branch.elseBlock;
        result.trueSource = head;
    } else {
        return false;
    }

    // A join that is the head is a loop, and turning its branch into a jump would be an infinite
    // one. Nothing above rules it out: an arm whose one predecessor is the head may jump back to it.
    return result.join != head;
}

/*
 * §Return unification, which is this pass's shape with the join left implicit.
 *
 * A conditional whose two arms both *return* has no join block for `findDiamond` to find - the two
 * values merge at the function's exit rather than at a block:
 *
 *     b0: %c = cmp_ilt %r, 0       b0: %c = cmp_ilt %r, 0
 *         je %c, b1, b2       ->       %v = select %c ? 0 : %r
 *     b1: ret 0                        ret %v
 *     b2: ret %r
 *
 * §15.5 of `test/bench/findings.md` is where this came from, and it asked for the general transform:
 * every return merged into one block with a phi, which would give the diamond above a join like any
 * other. That is the wrong shape to build. Unifying returns everywhere costs a jump per return and
 * is only ever wanted where something then reads the join - and the only thing that reads it is this
 * pass. Two returns that stay two returns are what §7.2 of `codegen/x64/README.md` shares an epilogue
 * between, and it does that better than a phi would.
 *
 * So the unification is *local to the conversion*: the exit is treated as the join it already is,
 * and where the conversion does not apply nothing is merged at all. Everything else - what an arm
 * may hold, what it may cost, what a select may carry - is the rule above unchanged.
 */

// One arm of such a branch: a block reached only from the head, computing a little and returning.
// The returned value comes back through `value`, and is null for a function returning unit.
Maybe<Size> returnArm(OptContext& opt, ModulePtr<Block> head, ModulePtr<Block> pointer,
                      ModulePtr<Value>& value)
{
    if(pointer == head) return Nothing();

    auto block = opt.local[pointer];

    // The same three the arm of a diamond has to satisfy, for the same three reasons - see
    // `armTarget`. The fourth differs: this arm leaves the function rather than joining.
    if(block->index == 0) return Nothing();
    if(block->phiCount() != 0) return Nothing();
    if(block->predecessorCount() != 1 || block->predecessorAt(opt.local, 0) != head) return Nothing();

    auto terminator = block->terminator();
    if(!terminator || opt.local[terminator]->kind != Value::Ret) return Nothing();

    value = ((InstRet&)*opt.local[terminator]).value;
    return armCost(opt, pointer);
}

bool convertReturnBranch(OptContext& opt, ModulePtr<Block> pointer) {
    auto head = opt.local[pointer];
    if(!head->terminator()) return false;

    auto terminator = opt.local[head->terminator()];
    if(terminator->kind != Value::Je) return false;

    // Read out before anything is rewritten: the branch is the instruction being replaced, so
    // nothing below may go on asking it what its arms were.
    auto& branch = (InstJe&)*terminator;
    auto condition = branch.cond;
    auto source = terminator->source;
    auto arms = { branch.thenBlock, branch.elseBlock };

    if(branch.thenBlock == branch.elseBlock) return false;

    ModulePtr<Value> whenTrue = nullptr;
    ModulePtr<Value> whenFalse = nullptr;

    auto trueCost = returnArm(opt, pointer, branch.thenBlock, whenTrue);
    if(!trueCost) return false;

    auto falseCost = returnArm(opt, pointer, branch.elseBlock, whenFalse);
    if(!falseCost) return false;

    if(trueCost.unwrap() + falseCost.unwrap() > kMaxSpeculated) return false;

    /*
     * Two returns of one value need no select, which is also how a function returning unit converts:
     * both arms return nothing, the two nothings are equal, and what the conversion removes is the
     * branch alone. Otherwise the value has to be one a select can hold.
     */
    if(whenTrue != whenFalse) {
        if(!whenTrue || !whenFalse) return false;
        if(!selectableType(opt, opt.local[whenTrue]->type)) return false;
        if(!selectableType(opt, opt.local[whenFalse]->type)) return false;
    }

    // The arms move up first, so that a value one of them computed is defined before the select that
    // reads it - the same order, and the same argument, as the diamond above.
    for(auto arm: arms) opt.ir().moveInstructions(*opt.local[arm], *head);

    auto selected = whenTrue;

    if(whenTrue != whenFalse) {
        // Through the stage's own editor rather than `addInst`, which builds one of its own with no
        // way to report what it wrote - see IrVersion, and the cached analyses that read it.
        auto instruction = createInst<InstSelect>(*opt.module, *opt.function, *head, source,
                                                  StringId(), opt.local[whenTrue]->type, condition,
                                                  whenTrue, whenFalse);
        opt.ir().append(*head, instruction);

        selected = (ModulePtr<Value>)((Value*)instruction - opt.local);
    }

    // The branch becomes the return, which takes both edges with it; the arms are then unreachable
    // and hold nothing, exactly as a converted diamond's are.
    auto ret = createInst<InstRet>(*opt.module, *opt.function, *head, source, StringId(),
                                   opt.program.scalar.unit, selected);

    opt.ir().setTerminator(*head, ret);
    for(auto arm: arms) opt.ir().clearTerminator(*opt.local[arm]);

    opt.changed = true;
    return true;
}

bool convertBranch(OptContext& opt, ModulePtr<Block> pointer) {
    auto head = opt.local[pointer];
    if(!head->terminator()) return false;

    auto terminator = opt.local[head->terminator()];
    if(terminator->kind != Value::Je) return false;

    auto& branch = (InstJe&)*terminator;

    Diamond diamond;
    if(!findDiamond(opt, pointer, branch, diamond)) return false;

    Size cost = 0;
    for(auto arm: diamond.arms) {
        if(!arm) continue;

        auto armSize = armCost(opt, arm);
        if(!armSize) return false;

        cost += armSize.unwrap();
    }

    if(cost > kMaxSpeculated) return false;

    /*
     * Every phi at the join, checked before anything is moved: this is all or nothing, because the
     * branch is what tells the phis apart and there is no half of it to leave behind. One phi of a
     * type a select cannot hold declines the whole conversion.
     */
    auto join = opt.local[diamond.join];
    for(auto phiPointer: join->phis(opt.local)) {
        auto& phi = *opt.local[phiPointer];
        if(!selectableType(opt, phi.type)) return false;

        auto whenTrue = inputFrom(opt, phi, diamond.trueSource);
        auto whenFalse = inputFrom(opt, phi, diamond.falseSource);
        if(!whenTrue || !whenFalse) return false;

        /*
         * And an alternative that is one of this block's own phis, which only a loop produces and
         * which this must not rewrite.
         *
         * Phis happen at once - they are one parallel copy on the way in - while the selects
         * replacing them are instructions in a row, so one that reads another would read the new
         * value where the phi read the old. The same guard covers the degenerate case of a phi
         * whose alternative is itself, where collapsing it would leave a select reading its own
         * result.
         */
        if(isPhiOf(opt, *join, whenTrue) || isPhiOf(opt, *join, whenFalse)) return false;
    }

    /*
     * The arms move up first, so that a value one of them computed is defined before the select that
     * reads it. Moving is all it takes: the head dominates both arms, so everything an arm's
     * instruction read is available where it lands, and a use list records who reads a value rather
     * than where from.
     */
    for(auto arm: diamond.arms) {
        if(!arm) continue;

        opt.ir().moveInstructions(*opt.local[arm], *head);
    }

    // Backwards, because a phi that is answered entirely is removed from the list.
    for(Size i = join->phiCount(); i-- > 0;) {
        auto phiPointer = join->phiAt(opt.local, i);
        auto& phi = *opt.local[phiPointer];

        auto whenTrue = inputFrom(opt, phi, diamond.trueSource);
        auto whenFalse = inputFrom(opt, phi, diamond.falseSource);

        /*
         * Two alternatives that are one value need no select at all. The folder would remove one
         * built here on its next visit, but not building it keeps the phi's readers pointed at the
         * value itself rather than at a temporary that exists for one round.
         */
        auto selected = whenTrue;
        if(whenTrue != whenFalse) {
            auto instruction = createInst<InstSelect>(*opt.module, *opt.function, *head, phi.source,
                                                      phi.name, phi.type, branch.cond, whenTrue,
                                                      whenFalse);
            opt.ir().append(*head, instruction);

            selected = (ModulePtr<Value>)((Value*)instruction - opt.local);
        }

        for(Size j = phi.inputs.size(); j-- > 0;) {
            auto input = phi.inputs.get(opt.local, j);
            if(input.block != diamond.trueSource && input.block != diamond.falseSource) continue;

            opt.ir().removePhiInput(phiPointer, j);
        }

        if(phi.inputs.isEmpty()) {
            opt.ir().replaceValue((ModulePtr<Value>)phiPointer, selected);

            // And the slots this phi filled, which `replaceValue` does not reach: a slot names the
            // value its storage came from rather than reading it, so the storage follows the phi
            // into the select that replaced it. Before the removal, since `erasePhi` empties every
            // slot the phi was the whole contents of.
            opt.ir().repointLocalValue((ModulePtr<Value>)phiPointer, selected);
            opt.ir().erasePhi(phiPointer);
        } else {
            opt.ir().addPhiInput(phiPointer, PhiInput { pointer, selected });
        }
    }

    /*
     * And the branch itself, which becomes a jump straight to the join. The condition loses its use
     * here and gains one per select above.
     *
     * The head's edge into the join is created only where it did not already have one, and that is
     * `setTerminator`'s multiset rule rather than a test written here: a triangle's branch already
     * led to the join down one side, and that is the edge that survives.
     */
    auto jump = createInst<InstJmp>(*opt.module, *opt.function, *head, terminator->source, StringId(),
                                    opt.program.scalar.unit, diamond.join);

    opt.ir().setTerminator(*head, jump);

    /*
     * The arms are now unreachable and hold nothing. Emptied rather than left for the sweep to find,
     * so that no edge into the join outlives the block it came from - and after the head's own
     * terminator rather than before it, since it is that rewrite which takes the last way in.
     */
    for(auto arm: diamond.arms) {
        if(!arm) continue;

        opt.ir().clearTerminator(*opt.local[arm]);
    }

    opt.changed = true;
    return true;
}

}

void convertSelects(OptContext& opt) {
    auto converted = false;

    // Over a snapshot, since the cleanup below rewrites the block list. Nothing in the walk itself
    // adds or removes a block, so one visit per block is one attempt at the branch it ends with.
    SmallArray<ModulePtr<Block>, 64> blocks;
    for(auto pointer: opt.function->blocks.contents(opt.local)) blocks.push(pointer);

    for(auto pointer: blocks) {
        converted = convertBranch(opt, pointer) || convertReturnBranch(opt, pointer) || converted;
    }

    if(!converted) return;

    /*
     * The two arms, and then the join.
     *
     * The arms are unreachable by construction and go with the sweep. The join usually has one way
     * in afterwards and no phis left, which is exactly the block `mergeBlocks` folds back into its
     * predecessor - and that is what turns a converted diamond into one straight-line block rather
     * than three, which is the state the block-local passes below can see through.
     */
    removeUnreachableBlocks(opt);
    mergeBlocks(opt);
}
