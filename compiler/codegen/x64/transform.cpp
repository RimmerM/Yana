#include "gen.h"
#include "x64_util.h"

// Checks if the flags register can be modified by running this instruction.
inline bool modifiesFlags(LowerInst* inst) {
    return isBinary(inst) || isCall(inst) || isUnaryArith(inst) || inst->kind == LowerInst::Alloca;
}

// Checks if this is a binary arithmetic instruction that supports an immediate right-hand operand.
inline bool allowsImmRhs(U32 kind) {
    return
        kind == LowerInst::Add || kind == LowerInst::Sub || kind == LowerInst::IMul ||
        kind == LowerInst::Shl || kind == LowerInst::Shr || kind == LowerInst::Sar ||
        kind == LowerInst::And || kind == LowerInst::Or || kind == LowerInst::Xor ||
        kind == LowerInst::Cmp;
}

// Checks if this immediate value can possibly be embedded into any instruction.
static bool isEmbeddableImm(LowerImm* imm) {
    // Floats can never be embedded.
    if(!isIntLike(imm->result.type)) return false;

    // X86 can embed up to 4-byte immediates into instructions.
    // For 1-byte ones, we always embed.
    // For 4-byte ones, we embed for <= 2 uses; for higher use counts, it's better to save space instead.
    if(encodeImm8(&imm->result)) {
        return true;
    } else if(encodeImm32(&imm->result) && imm->result.uses.size() <= 2) {
        return true;
    }

    return false;
}

// Checks if this specific instruction can embed the provided embeddable immediate operand.
static bool canEmbedImm(LowerBase base, LowerInst* inst, LowerValue* op) {
    // We only check if the instruction type can embed;
    // register types have already been checked here.
    auto kind = inst->kind;
    if(kind == LowerInst::Set) {
        assertTrue(op == base[((LowerInstUnary*)inst)->from]);
        return true;
    } else if(kind == LowerInst::Alloca) {
        // A compile-time size is consumed by frame layout rather than by any instruction: the
        // alloca becomes the address of a frame object, and nothing ever reads the count. Leaving
        // it explicit would cost a `mov r, imm32` and a register for a number that is already known.
        assertTrue(op == base[((LowerInstAlloca*)inst)->byteCount]);
        return true;
    } else if(kind == LowerInst::Cast || kind == LowerInst::Bitcast) {
        return isIntLike(((LowerInstUnary*)inst)->result.type);
    } else if(allowsImmRhs(kind)) {
        return op == base[((LowerInstCmp*)inst)->rhs];
    }

    return false;
}

// Tries to embed this immediate into any instructions that use it.
static bool tryEmbedImm(LowerBase base, LowerImm* imm) {
    if(!isEmbeddableImm(imm)) return false;

    for(auto use: imm->result.uses.contents(base)) {
        if(!canEmbedImm(base, base[use], &imm->result)) return false;
    }

    imm->result.flags |= LowerValue::Implicit;
    return true;
}

// A call to a statically known function is encoded as a direct rel32 call, which never reads the
// target address out of a register. Materializing it costs a `lea` that nothing reads, and - worse -
// a register that has to survive the call's clobber set, of which there are only a handful. Mark the
// address implicit unless something other than a direct callee position actually needs it.
static bool tryElideDirectCallee(LowerBase base, LowerInstFun* fun) {
    for(auto offset: fun->result.uses.contents(base)) {
        auto use = base[offset];
        if(use->kind != LowerInst::Call) return false;
        if(((LowerInstCall*)use)->getCallType() == LowerCallType::Syscall) return false;

        // used()[0] is the callee; anywhere else it is an ordinary argument and needs a register.
        auto used = use->used();
        if(base[used[0]] != &fun->result) return false;

        for(Size i = 1; i < used.size(); i++) {
            if(base[used[i]] == &fun->result) return false;
        }
    }

    fun->result.flags |= LowerValue::Implicit;
    return true;
}

// Tries to swap operands to the provided instruction in order to make it easier to perform further optimizations.
// This needs to be done before register allocation,
// since swapping and then embedding may reduce the number of registers needed.
static bool trySwapOperands(LowerBase base, LowerInst* inst) {
    if(!isBinary(inst)) return false;

    auto binary = (LowerInstBinary*)inst;
    if(!isIntLike(binary->result.type)) return false;

    // For register and memory operands, both directions can be encoded, so it is pointless to swap.
    // Because of that, we only check if immediates can swapped.
    if(base[binary->lhs]->inst()->kind != LowerInst::Imm) return false;

    // Only swap for operations that are safe.
    auto kind = binary->kind;
    if(!(kind == LowerInst::Add || kind == LowerInst::Mul || kind == LowerInst::IMul ||
       kind == LowerInst::And || kind == LowerInst::Or || kind == LowerInst::Xor)) return false;

    // Swap lhs with rhs to ensure the immediate is on the right side.
    ::swap(binary->lhs, binary->rhs);
    return true;
}

static bool hasFlagsInterference(LowerBase base, LowerInstCmp* cmp, LowerInst* use, Size startIndex) {
    // Currently, we simply check if there is any interfering instruction below the creation, until we get to the use.
    // TODO: Follow paths between blocks from the definition to the use.
    if(use->block != cmp->block) return true;

    auto block = base[cmp->block];
    auto list = block->instructions.contents(base);

    for(Size i = startIndex + 1; i < list.size(); i++) {
        auto inst = base[list[i]];
        if(inst == use) return false;
        if(modifiesFlags(inst)) return true;
    }

    if(use == base[block->terminator]) return false;
    return true;
}

static bool tryMergeCompare(LowerBase base, LowerInstCmp* cmp, Size index) {
    auto& uses = cmp->result.uses;

    if(uses.size() == 0) {
        cmp->result.flags |= LowerValue::Implicit;
        return true;
    }

    for(auto offset: uses.contents(base)) {
        auto use = base[offset];

        // If the result is used as an actual value, it needs to written to a register.
        if(use->kind != LowerInst::Je && use->kind != LowerInst::Select) return false;

        // Check if there is any instruction between the definition and use that could modify the flags.
        if(hasFlagsInterference(base, cmp, use, index)) return false;
    }

    // The only uses are instructions that can use flags directly, and no interference, so the result can stay as flags.
    cmp->result.flags |= LowerValue::Implicit;

    for(auto offset: uses.contents(base)) {
        auto use = base[offset];

        if(use->kind == LowerInst::Je) {
            ((LowerInstJe*)use)->setEmbeddedCmp(Just(cmp->getCmp()));
        } else if(use->kind == LowerInst::Select) {
            ((LowerInstSelect*)use)->setEmbeddedCmp(Just(cmp->getCmp()));
        }
    }

    return true;
}

// Records, once, which of the two encodings a Copy/SetPattern will take, so that the register
// constraints (constraint.cpp) and the encoder (genCopy/genSetPattern) read one field instead of
// each re-deriving the choice and risking disagreement. The unrolled form is only viable for a
// compile-time byte count small enough to be worth straight-lining; everything else takes the
// rep-prefixed string instruction, which needs its operands in fixed registers.
static bool isUnrolledCount(LowerBase base, LowerPtr<LowerValue> count) {
    auto value = base[count];
    if(value->inst()->kind != LowerInst::Imm) return false;

    return ((LowerImm*)value->inst())->i <= kMaxUnrolledMemOp;
}

static void selectBlockOpEncoding(LowerBase base, LowerInst* inst) {
    if(inst->kind == LowerInst::Copy) {
        auto copy = (LowerInstCopy*)inst;
        copy->setUnrolled(isUnrolledCount(base, copy->count));
    } else if(inst->kind == LowerInst::SetPattern) {
        auto set = (LowerInstSetPattern*)inst;
        set->setUnrolled(isUnrolledCount(base, set->count));
    }
}

// Inserts an empty block on the edge from `pred` (its `outgoing[edge]`) to `succ`, so that the
// moves that feed `succ`'s phis have a block of their own to live in.
//
// Phi moves are emitted at the end of the predecessor, which is only sound if control reaching that
// point is guaranteed to continue into the phi's block. When the predecessor ends in a conditional
// branch, it is not: the moves would run on the way to *both* successors, writing phi registers on
// a path where they hold something else. Splitting gives the edge a block whose only successor is
// `succ`, which restores that guarantee.
static void splitEdge(LowerBase base, LowerFunction& fun, LowerBlock* pred, Size edge) {
    auto& arena = fun.arena;
    auto succ = base[pred->outgoing[edge]];
    auto predOffset = pred - base;

    auto split = new (arena) LowerBlock(pred->fun, 0, BlockIndex(fun.blocks.size()));
    fun.blocks.push(arena, split - base);

    // Wired up by hand rather than through addInst, which would append the split block to `succ`'s
    // incoming list instead of replacing the predecessor entry that the phis still refer to.
    auto jmp = (LowerInst*)new (arena) LowerInstJmp(succ - base);
    jmp->block = split - base;
    split->terminator = jmp - base;
    split->outgoing[0] = succ - base;
    split->incoming.push(arena, predOffset);

    auto je = (LowerInstJe*)base[pred->terminator];
    assertTrue(je->kind == LowerInst::Je);
    if(edge == 0) je->then = split - base;
    else je->otherwise = split - base;
    pred->outgoing[edge] = split - base;

    for(Size i = 0; i < succ->incoming.size(); i++) {
        if(succ->incoming.get(base, i) == predOffset) {
            succ->incoming.set(base, i, split - base);
            break;
        }
    }

    for(auto p: succ->phis.contents(base)) {
        auto sources = base[p]->sources();
        for(Size i = 0; i < sources.size(); i++) {
            if(sources.ptr[i] == predOffset) sources.ptr[i] = split - base;
        }
    }
}

static void splitPhiEdges(LowerBase base, LowerFunction& fun) {
    // Snapshotted because splitting appends to the block list, and a freshly created split block
    // has a single successor and so can never itself need splitting.
    Array<LowerPtr<LowerBlock>> original;
    for(auto b: fun.blocks.contents(base)) original.push(b);

    for(auto offset: original) {
        auto pred = base[offset];

        // Only a block with two successors can reach a phi on a path it might not take.
        if(!pred->outgoing[0] || !pred->outgoing[1]) continue;

        for(Size edge = 0; edge < 2; edge++) {
            if(base[pred->outgoing[edge]]->phis.isNotEmpty()) splitEdge(base, fun, pred, edge);
        }
    }
}

/*
 * Outgoing stack arguments.
 *
 * A call whose convention runs out of argument registers passes the rest in the argument area, and
 * each of those becomes an explicit store ahead of the call.
 *
 * The store exists to break the argument's lifetime. Left as an ordinary operand of the call, a
 * stack argument would have to sit in a register from wherever it was computed all the way to the
 * call, competing for registers with every other argument being computed in between - which is
 * precisely where a call with more arguments than registers is under the most pressure. Storing it
 * early ends its live range at the store, and memory holds it from there on.
 *
 * That is also why the store has to be an instruction rather than a move hung off the call: liveness
 * runs over instructions, so a store it cannot see shortens nothing.
 *
 * Which arguments these are is the convention's answer and never the author's, so the caller writes
 * into exactly the offsets the callee reads back from.
 */

// Inserts `inst` into `block`'s instruction list at `at`, shifting what follows up one. The list has
// no insert of its own, and the linear shift costs less than adding one would: this runs once per
// stack argument, over a list every pass already walks end to end.
static void insertInstAt(LowerBase base, LowerBlock* block, Size at, LowerInst* inst) {
    auto& arena = base[block->fun]->arena;

    inst->block = block - base;
    for(auto use: inst->used()) base[use]->uses.push(arena, inst - base);

    block->instructions.push(arena, inst - base);

    for(auto i = block->instructions.size() - 1; i > at; i--) {
        block->instructions.set(base, i, block->instructions.get(base, i - 1));
    }

    block->instructions.set(base, at, inst - base);
}

// Moves `user`'s use of `from` over to `to`. Both use lists have to reflect it: they are how every
// later pass finds who consumes a value, and a stale entry would keep a dead value looking live.
static void replaceUse(LowerBase base, LowerValue* from, LowerInst* user, LowerValue* to) {
    auto uses = from->uses.contents(base);

    for(Size i = 0; i < uses.size(); i++) {
        if(base[uses[i]] == user) {
            from->uses.remove(base, i);
            break;
        }
    }

    to->uses.push(base[base[user->block]->fun]->arena, user - base);
}

// Where the store for an argument can go, as an index into its block's instruction list. As early as
// possible, since shortening the live range is the whole point: just after whichever comes last of
// the value's own definition and the preceding call, and never later than the call it feeds.
//
// The preceding call matters because the argument area is shared between the calls of a function -
// it is reserved once, sized for the largest - so a store hoisted above an earlier call would
// overwrite an argument that call has not read yet.
static Size stackArgPosition(LowerBase base, LowerBlock* block, LowerValue* value, Size callIndex) {
    Size position = 0;
    auto instructions = block->instructions.contents(base);

    for(Size i = 0; i < callIndex; i++) {
        auto inst = base[instructions[i]];

        if(inst->kind == LowerInst::Call) position = i + 1;

        for(auto& created: inst->created()) {
            if(&created == value) position = i + 1;
        }
    }

    return position;
}

static void insertStackArgs(LowerBase base, LowerFunction& fun, const Constraints& constraints) {
    auto& arena = fun.arena;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // Indexed rather than buffered, because inserting a store rewrites the list underneath.
        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Call) continue;

            auto callType = ((LowerInstCall*)inst)->getCallType();
            auto& convention = constraints.getConvention(callType);
            auto used = inst->used();

            // A syscall's first operand is its number, which the convention places like any other
            // argument; every other call names its target there, and that is not an argument.
            Size argStart = callType == LowerCallType::Syscall ? 0 : 1;

            Array<ArgLocation> locations;
            classifyArgs(convention, used.size() - argStart, [&](Size a) {
                return base[used[a + argStart]]->type;
            }, locations);

            for(Size a = 0; a < locations.size(); a++) {
                if(locations[a].kind != ArgLocation::Stack) continue;

                auto operand = used[a + argStart];
                auto value = base[operand];

                auto push = new (arena) LowerInstX86PushArg(operand, locations[a].stackOffset, value->type);
                insertInstAt(base, block, stackArgPosition(base, block, value, i), push);

                // The call names the store's result from here on, so it still lists every argument
                // in order while the value itself is dead from the store onwards.
                replaceUse(base, value, inst, &push->result);
                used[a + argStart] = &push->result - base;

                i++; // the call has moved up one
            }
        }
    }
}

// Rewrites the block list into reverse postorder, so that a block is (wherever the CFG allows it)
// visited after the predecessors that define the values live on entry to it.
//
// Both consumers depend on this: buildRanges numbers instructions in block-list order, and its
// ranges are only tight when that order follows the control flow; genFunction emits in the same
// order, so reverse postorder also turns more branches into fallthrough. Keeping one order for both
// is what lets the allocator work in linear indices and the encoder walk in lockstep with it.
static void orderBlocks(LowerBase base, LowerFunction& fun) {
    auto postorder = fun.buildPostorder(base);

    // A block that the entry point cannot reach has no place in the ordering, and nothing
    // downstream is prepared to allocate registers for one.
    assertTrue(postorder.size() == fun.blocks.size());

    Array<LowerPtr<LowerBlock>> ordered;
    for(Size i = postorder.size(); i > 0; i--) {
        ordered.push(fun.blocks.get(base, postorder[i - 1]));
    }

    for(Size i = 0; i < ordered.size(); i++) {
        fun.blocks.set(base, i, ordered[i]);
        base[ordered[i]]->index = BlockIndex(i);
    }
}

void transformFunction(LowerBase base, LowerFunction& fun) {
    auto pass = [&](auto onInst) {
        for(auto b: fun.blocks.contents(base)) {
            Size i = 0;

            for(auto inst: base[b]->instructions.contents(base)) {
                onInst(base[inst], i);
                i++;
            }
        }
    };

    // Move operands into place for later passes.
    pass([&](LowerInst* inst, Size i) {
        trySwapOperands(base, inst);
    });

    pass([&](LowerInst* inst, Size i) {
        // Make immediates implicit where possible.
        if(inst->kind == LowerInst::Imm) {
            tryEmbedImm(base, (LowerImm*)inst);
        }

        // Make comparisons implicit if flags aren't changed between the creation and any uses.
        if(inst->kind == LowerInst::Cmp) {
            tryMergeCompare(base, (LowerInstCmp*)inst, i);
        }

        if(inst->kind == LowerInst::Fun) {
            tryElideDirectCallee(base, (LowerInstFun*)inst);
        }

        selectBlockOpEncoding(base, inst);
    });

    // After the peepholes, so that an argument the passes above made implicit is already implicit
    // when its location is decided, and before liveness runs, which is what lets the stores it
    // inserts actually shorten the ranges they exist to shorten.
    insertStackArgs(base, fun, targetConstraints());

    // Shape of the CFG last, once no pass that reasons about instruction positions is left to run.
    splitPhiEdges(base, fun);
    orderBlocks(base, fun);
}
