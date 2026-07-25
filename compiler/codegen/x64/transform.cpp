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

// Takes an instruction nothing reads any more out of its block, and with it the uses it contributed.
// Dropping those is what makes the next instruction of a folded address chain dead in turn, so the
// whole chain comes out by removing its instructions in order.
static void removeInst(LowerBase base, LowerInst* inst) {
    for(auto offset: inst->used()) {
        auto v = base[offset];
        auto uses = v->uses.contents(base);

        for(Size i = 0; i < uses.size(); i++) {
            if(base[uses[i]] == inst) { v->uses.remove(base, i); break; }
        }
    }

    auto block = base[inst->block];
    auto list = block->instructions.contents(base);

    for(Size i = 0; i < list.size(); i++) {
        if(base[list[i]] == inst) {
            block->instructions.remove(base, i);
            return;
        }
    }

    assertTrue("removing an instruction that is not in its own block" == nullptr);
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

/*
 * Addressing modes.
 *
 * x86 computes `base + index*{1,2,4,8} + disp32` as part of a memory access and charges nothing for
 * it. The lowering has no notion of that - it produces the arithmetic as ordinary instructions - so
 * the shape is recognized here and collapsed into the `X86Address` the encoder already knows how to
 * embed into a load or a store.
 *
 * An X86Address emits no code and occupies no register of its own. It is placed immediately in front
 * of the access that reads it, and genLoad/genStore fold it into their ModRM byte. The adjacency is
 * required rather than incidental: the verifier checks an address's operand registers at the address
 * instruction rather than at its user, which is only sound while nothing can come between them.
 *
 * A chain is only taken apart when every instruction in it exists solely to compute this address.
 * Folding half of one would leave the arithmetic behind *and* repeat it inside the address, so the
 * test is "every use is an address operand" at the top of the chain and "this is the only use"
 * further in. The top may legitimately have several users - a pointer read and then written, an
 * array element used twice - and each of them gets an address instruction of its own.
 */

struct AddressPattern {
    LowerValue* base = nullptr;
    LowerValue* index = nullptr;
    U8 scale = 1;
    I64 displacement = 0;
};

// Whether `user` reads `v` as the address of a memory access and nowhere else. The address is
// operand zero of both a load and a store, and it is the only operand position an X86Address can
// occupy - so `store %p, %p` reads the same value once as an address and once as a value, and
// rewriting only the first would leave the second pointing at an instruction about to be removed.
static bool isAddressOperand(LowerBase base, LowerInst* user, LowerValue* v) {
    if(user->kind != LowerInst::Load && user->kind != LowerInst::Store) return false;

    auto used = user->used();
    if(base[used[0]] != v) return false;

    for(Size i = 1; i < used.size(); i++) {
        if(base[used[i]] == v) return false;
    }

    return true;
}

static bool isOnlyUsedAsAddress(LowerBase base, LowerValue* v) {
    if(v->uses.isEmpty()) return false;

    for(auto u: v->uses.contents(base)) {
        if(!isAddressOperand(base, base[u], v)) return false;
    }

    return true;
}

// Whether `inst` is the one and only thing that reads `v`, and so whether folding `v` away leaves
// nothing behind. This is the test at every level of the chain below the top one.
static bool isOnlyUse(LowerBase base, LowerValue* v, LowerInst* inst) {
    return v->uses.size() == 1 && base[v->uses.get(base, 0)] == inst;
}

// The signed displacement `v` contributes, if it is an immediate small enough to be one. x86 sign-
// extends an address displacement from 32 bits, so the range it can hold is exactly what
// encodeImm32 accepts - and whether the immediate was made implicit is irrelevant, since the value
// is read here rather than encoded from a register.
static Maybe<I64> addressDisplacement(LowerValue* v) {
    if(v->inst()->kind != LowerInst::Imm) return Nothing();

    auto imm = encodeImm32(v);
    if(!imm) return Nothing();

    return Just(I64(I32(imm.unwrap())));
}

// Matches `v` against `index * {1,2,4,8}`, the only scaling the SIB byte can encode.
//
// Only a 64-bit multiply qualifies. A 32-bit `shl %i, 2` wraps at 32 bits and the address unit does
// not, so folding one would change what an index near the top of its range produces. A plain
// unscaled index is not subject to that: it reaches the address in the same register the 64-bit add
// would have read it from, whatever its declared width.
static bool matchScaled(LowerBase base, LowerValue* v, LowerInst* user, LowerValue*& index, U8& scale) {
    if(!is64Bit(v->type)) return false;
    if(!isOnlyUse(base, v, user)) return false;

    auto inst = v->inst();
    if(!isBinary(inst)) return false;

    auto binary = (LowerInstBinary*)inst;
    auto factorValue = base[binary->rhs];
    if(factorValue->inst()->kind != LowerInst::Imm) return false;

    auto imm = ((LowerImm*)factorValue->inst())->i;
    U64 factor;

    if(inst->kind == LowerInst::Shl) {
        if(imm > 3) return false;
        factor = U64(1) << imm;
    } else if(inst->kind == LowerInst::Mul || inst->kind == LowerInst::IMul) {
        factor = imm;
        if(factor != 1 && factor != 2 && factor != 4 && factor != 8) return false;
    } else {
        return false;
    }

    auto source = base[binary->lhs];
    if(isImplicit(source)) return false;

    index = source;
    scale = U8(factor);
    return true;
}

// Peels `base + index*scale + displacement` off the value a load or store takes its address from,
// stopping as soon as what is left is not exclusively this address's own arithmetic. `folded`
// collects the instructions that become dead, in the order they can be removed: an outer add before
// the shift it absorbed, so that each is already unused by the time it goes.
static bool matchAddress(LowerBase base, LowerValue* address, AddressPattern& out, Array<LowerInst*>& folded) {
    out.base = address;
    if(!isOnlyUsedAsAddress(base, address)) return false;

    // Loop invariant: everything reading `out.base` is about to be rewritten to read the address
    // instead, so the instruction computing it can be removed.
    for(;;) {
        auto v = out.base;
        auto inst = v->inst();

        // Pointer arithmetic only. A 32-bit add wraps where the address unit does not, and the
        // lowering states the width in the result type of the operation itself.
        if(!isPtr(v->type)) break;
        if(inst->kind != LowerInst::Add && inst->kind != LowerInst::Sub) break;

        auto binary = (LowerInstBinary*)inst;
        auto lhs = base[binary->lhs];
        auto rhs = base[binary->rhs];

        // Decided in full before anything is committed, so that a step that turns out not to fit
        // leaves the pattern as the previous one left it.
        LowerValue* next = nullptr;
        LowerValue* index = out.index;
        U8 scale = out.scale;
        auto displacement = out.displacement;
        LowerInst* scaled = nullptr;

        if(auto d = addressDisplacement(rhs)) {
            displacement += inst->kind == LowerInst::Sub ? -d.unwrap() : d.unwrap();
            if(displacement >= I64(minLimit<I32>) && displacement <= I64(maxLimit<I32>)) next = lhs;
        } else if(inst->kind == LowerInst::Add && !out.index) {
            // Add is commutative and the immediate peephole has already run, so either side may be
            // the one carrying the index.
            if(matchScaled(base, rhs, inst, index, scale)) {
                scaled = rhs->inst();
                next = lhs;
            } else if(matchScaled(base, lhs, inst, index, scale)) {
                scaled = lhs->inst();
                next = rhs;
            } else if(!isImplicit(rhs)) {
                index = rhs;
                scale = 1;
                next = lhs;
            }
        }

        // The base has to reach the address in a register of its own; an operand that was folded
        // into some other instruction's encoding has none.
        if(!next || isImplicit(next)) break;

        out.index = index;
        out.scale = scale;
        out.displacement = displacement;
        out.base = next;

        folded.push(inst);
        if(scaled) folded.push(scaled);

        // Anything else reading what is left stops the chain here: that value stays materialized,
        // and folding further would compute it twice rather than once.
        if(!isOnlyUse(base, next, inst)) break;
    }

    return folded.isNotEmpty();
}

// Where `inst` sits in its own block's instruction list.
static Size indexOfInst(LowerBase base, LowerBlock* block, LowerInst* inst) {
    auto list = block->instructions.contents(base);

    for(Size i = 0; i < list.size(); i++) {
        if(base[list[i]] == inst) return i;
    }

    assertTrue("instruction is not in its own block" == nullptr);
    return 0;
}

static void foldAddresses(LowerBase base, LowerFunction& fun) {
    auto& arena = fun.arena;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Load && inst->kind != LowerInst::Store) continue;

            // The address is operand zero of both, and is already an X86Address when an earlier
            // access on the same chain folded it for every user at once.
            auto address = base[inst->used()[0]];
            if(isMem(address)) continue;

            AddressPattern pattern;
            Array<LowerInst*> folded;
            if(!matchAddress(base, address, pattern, folded)) continue;

            // Snapshotted: the loop below rewrites the very list it is reading.
            Array<LowerInst*> users;
            for(auto u: address->uses.contents(base)) users.push(base[u]);

            for(auto user: users) {
                auto computed = new (arena) LowerInstX86Address(
                    LowerInst::X86Address, 0, pattern.base - base,
                    pattern.index ? pattern.index - base : nullptr,
                    pattern.scale, U32(I32(pattern.displacement))
                );

                auto userBlock = base[user->block];
                insertInstAt(base, userBlock, indexOfInst(base, userBlock, user), computed);

                replaceUse(base, address, user, &computed->result);
                user->used()[0] = &computed->result - base;
            }

            for(auto dead: folded) removeInst(base, dead);

            // Both the insertions and the removals moved things around underneath the walk, so the
            // position to carry on from is wherever this access ended up.
            i = indexOfInst(base, block, inst);
        }
    }
}

/*
 * Block order.
 *
 * The list is rewritten into reverse postorder, so that a block is - wherever the CFG allows it -
 * visited after the predecessors that define the values live on entry to it. Both consumers depend
 * on that: buildRanges numbers instructions in block-list order, and its ranges are only tight when
 * that order follows the control flow; genFunction emits in the same order, so reverse postorder
 * also turns more branches into fallthrough. Keeping one order for both is what lets the allocator
 * work in linear indices and the encoder walk in lockstep with it.
 *
 * *Which* reverse postorder is a further choice, though, and taking the successors as declared is
 * the wrong one around a loop. A header ending in `je body, exit` explores the body first, so the
 * body is finished - and pushed onto the postorder - before the exit, and comes out *after* it once
 * the postorder is reversed: the exit block lands between the header and the body it leaves. Every
 * iteration then pays a taken branch into the body and a jump back, and every interval spanning the
 * loop is split into two ranges around the intruding block.
 *
 * Exploring the successor that *leaves* the loop first fixes both, since it is finished first and so
 * reversed last. That is all the loop analysis below is for: a depth per block, so that two
 * successors can be told apart by which of them is further in.
 */

// A retreating edge found by the walk, from the block that closes a loop back to the one that opens
// it. In a reducible CFG an edge to a block still on the walk's own stack is exactly an edge to a
// block that dominates its source, which is what makes it a loop rather than a diamond.
struct BackEdge {
    LowerBlock* latch;
    LowerBlock* header;
};

static constexpr U32 kOnStack = 1;
static constexpr U32 kFinished = 2;

static void findBackEdges(LowerBase base, LowerBlock* b, Array<BackEdge>& out) {
    b->marker = kOnStack;

    for(auto o: b->outgoing) {
        if(!o) continue;
        auto s = base[o];

        if(s->marker == kOnStack) out.push(BackEdge { b, s });
        else if(s->marker != kFinished) findBackEdges(base, s, out);
    }

    b->marker = kFinished;
}

// Counts one back edge's loop into `depth`: the header, plus everything the latch is reachable from
// without passing back through the header, which is the natural loop of that edge. Nested loops
// simply add on top of each other, so a block inside two of them is counted twice and compares as
// deeper than one inside either alone.
static void addLoopDepth(LowerBase base, LowerFunction& fun, const BackEdge& edge, Array<U32>& depth) {
    Array<bool> inLoop;
    for(Size i = 0; i < fun.blocks.size(); i++) inLoop.push(false);

    // Marked before the walk starts rather than visited by it: the header bounds the loop, and a
    // predecessor reached through it is outside.
    inLoop[edge.header->index] = true;
    depth[edge.header->index]++;

    Array<LowerBlock*> body;

    auto visit = [&](LowerBlock* b) {
        if(inLoop[b->index]) return;

        inLoop[b->index] = true;
        depth[b->index]++;
        body.push(b);
    };

    visit(edge.latch);

    // Walked as a queue rather than popped, so that `body` doubles as the visited list.
    for(Size i = 0; i < body.size(); i++) {
        for(auto p: body[i]->incoming.contents(base)) visit(base[p]);
    }
}

static void traverseOrdered(LowerBase base, LowerBlock* b, const Array<U32>& depth, BlockList& out) {
    b->marker = 1;

    auto first = b->outgoing[0] ? base[b->outgoing[0]] : nullptr;
    auto second = b->outgoing[1] ? base[b->outgoing[1]] : nullptr;

    // The deeper successor is explored last, so that it is pushed last and reversed to the front:
    // the block continuing the loop follows its header directly, and whatever leaves is left for
    // after the body. Successors at the same depth keep the order the branch declares them in.
    if(first && second && depth[first->index] > depth[second->index]) ::swap(first, second);

    if(first && !first->marker) traverseOrdered(base, first, depth, out);
    if(second && !second->marker) traverseOrdered(base, second, depth, out);

    out.push(b->index);
}

static void orderBlocks(LowerBase base, LowerFunction& fun) {
    auto blockList = fun.blocks.contents(base);
    auto entry = base[fun.blocks.get(base, 0)];

    Array<U32> depth;
    for(Size i = 0; i < blockList.size(); i++) {
        auto b = base[blockList[i]];
        b->index = BlockIndex(i);
        b->marker = 0;
        depth.push(0);
    }

    Array<BackEdge> backEdges;
    findBackEdges(base, entry, backEdges);
    for(auto& e: backEdges) addLoopDepth(base, fun, e, depth);

    for(auto o: blockList) base[o]->marker = 0;

    BlockList postorder(blockList.size());
    traverseOrdered(base, entry, depth, postorder);

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

    // Before the peepholes rather than after them: an immediate whose only use was an address
    // computation is left with none by the fold, and is then made implicit by the pass below rather
    // than being materialized into a register nothing reads.
    foldAddresses(base, fun);

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
