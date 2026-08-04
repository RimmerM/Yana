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
 * Read off what the *lower* IR will accept, which is `validateSelect`'s list: an integer or a float,
 * both of which are a register. An enum record is one too - `lowerType` answers `Int32` for one, and
 * a `Bool` is the enum this is asked about most - and everything else is either a memory type,
 * whose phi is an address rather than a value, or unit, which merges nothing.
 *
 * A pointer is deliberately not here. The machine could select one and the JS target could not care
 * less, but the lower IR's validator declines it today, and widening that is a change to what the
 * IR means rather than to what this pass decides.
 */
bool selectableType(OptContext& opt, TypePtr type) {
    if(!type) return false;

    auto value = opt.global[type];
    if(value->kind == Type::Int || value->kind == Type::Float) return true;

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

    if(block->phis.isNotEmpty()) return nullptr;
    if(block->incoming.size() != 1 || block->incoming.get(opt.local, 0) != head) return nullptr;
    if(!block->terminator || opt.local[block->terminator]->kind != Value::Jmp) return nullptr;

    return ((InstJmp&)*opt.local[block->terminator]).target;
}

// The instructions of one arm, or nothing where one of them may not be made unconditional.
Maybe<Size> armCost(OptContext& opt, ModulePtr<Block> arm) {
    Size count = 0;

    for(auto pointer: opt.local[arm]->instructions.contents(opt.local)) {
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
    for(auto phi: block.phis.contents(opt.local)) {
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

bool convertBranch(OptContext& opt, ModulePtr<Block> pointer) {
    auto head = opt.local[pointer];
    if(!head->terminator) return false;

    auto terminator = opt.local[head->terminator];
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
    for(auto phiPointer: join->phis.contents(opt.local)) {
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

        auto block = opt.local[arm];
        for(auto instruction: block->instructions.contents(opt.local)) {
            opt.local[instruction]->block = pointer;
            head->instructions.push(opt.program.arena, instruction);
        }

        block->instructions.clear();
    }

    // Backwards, because a phi that is answered entirely is removed from the list.
    for(Size i = join->phis.size(); i-- > 0;) {
        auto phiPointer = join->phis.get(opt.local, i);
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
            auto instruction = addInst<InstSelect>(*opt.module, *opt.function, *head, phi.source,
                                                   phi.name, phi.type, branch.cond, whenTrue,
                                                   whenFalse);

            selected = (ModulePtr<Value>)((Value*)instruction - opt.local);
        }

        for(Size j = phi.inputs.size(); j-- > 0;) {
            auto input = phi.inputs.get(opt.local, j);
            if(input.block != diamond.trueSource && input.block != diamond.falseSource) continue;

            dropUse(opt, input.value, (ModulePtr<Inst>)phiPointer);
            phi.inputs.remove(opt.local, j);
        }

        if(phi.inputs.isEmpty()) {
            replaceValue(opt, (ModulePtr<Value>)phiPointer, selected);
            join->phis.remove(opt.local, i);
        } else {
            phi.inputs.push(opt.program.arena, PhiInput { pointer, selected });
            opt.local[selected]->uses.push(opt.program.arena, (ModulePtr<Inst>)phiPointer);
        }
    }

    /*
     * And the branch itself, replaced the way opt_branch.cpp replaces a folded one: written into the
     * block rather than added through `Block::add`, which would record the edge into the join a
     * second time. The condition loses its use here and gains one per select above.
     */
    dropUse(opt, branch.cond, head->terminator);

    auto jump = createInst<InstJmp>(*opt.module, *opt.function, *head, terminator->source, 0,
                                    opt.program.scalar.unit, diamond.join);

    head->terminator = (ModulePtr<Inst>)((Value*)jump - opt.local);
    head->outgoing[0] = diamond.join;
    head->outgoing[1] = nullptr;

    for(auto arm: diamond.arms) {
        if(!arm) continue;

        for(Size i = 0; i < join->incoming.size(); i++) {
            if(join->incoming.get(opt.local, i) != arm) continue;

            join->incoming.remove(opt.local, i);
            break;
        }

        // The arm is now unreachable and holds nothing. Emptied rather than left for the sweep to
        // find, so that no edge into the join outlives the block it came from.
        auto block = opt.local[arm];
        while(block->incoming.size()) block->incoming.remove(opt.local, block->incoming.size() - 1);

        block->terminator = nullptr;
        block->outgoing[0] = nullptr;
        block->outgoing[1] = nullptr;
    }

    // The head's own edge into the join, where it did not already have one. A triangle did: the side
    // of the branch that was already the join is the edge that survived.
    if(diamond.trueSource != pointer && diamond.falseSource != pointer) {
        join->incoming.push(opt.program.arena, pointer);
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

    for(auto pointer: blocks) converted = convertBranch(opt, pointer) || converted;

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

    // An instruction in a block that no longer exists is still in the use list of everything it
    // read, and the phi rewrite above pushed alternatives by hand. Rebuilding settles both.
    rebuildUses(opt);
}
