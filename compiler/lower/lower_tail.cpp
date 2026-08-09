#include "lower_tail.h"
#include "lower_builder.h"

/*
 * See lower_tail.h for what shape is recognized and why the pass is at this tier. This file is the
 * mechanics: the walk from a self-call to a `ret`, and the surgery that turns the one into the
 * other.
 */

namespace {

// The operations an accumulator may be carried through: associative, commutative, and with an
// identity. Both multiplies qualify because only the low half is produced, and the low half of a
// product does not depend on the operands' signedness.
bool isAccumulator(LowerInst::Kind kind) {
    switch(kind) {
        case LowerInst::Add:
        case LowerInst::Mul:
        case LowerInst::IMul:
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
            return true;
        default:
            return false;
    }
}

// What the accumulator holds before the first frame contributes to it. `And`'s is all ones, which is
// the one that has to know how wide the register is.
U64 identityFor(LowerInst::Kind kind, LowerType type) {
    switch(kind) {
        case LowerInst::Mul:
        case LowerInst::IMul:
            return 1;
        case LowerInst::And:
            return type == LowerType::Int32 ? 0xffffffffull : ~U64(0);
        default:
            return 0;
    }
}

/*
 * One call this pass is going to turn into a jump back to the top.
 *
 * `steps` are the accumulations between the call and the return, in the order they were written -
 * the part of the sum this frame contributes is whichever operand of each of them is not the value
 * being carried. They all live in the call's own block, which is what makes them available where the
 * back edge is built: a block the walk merely passed through holds nothing but phis.
 *
 * The *operands* are deliberately not recorded here, only the instructions holding them. Between the
 * analysis and the rewrite every reader of an argument is pointed at that argument's new phi, and a
 * step adding the argument to the call's answer is one of those readers - so an operand copied out
 * during the analysis would name the value the function was entered with rather than the one this
 * iteration has. That was worth six on `countDown(4)` in test/resolve/Inline.yana.
 */
struct Chain {
    LowerInst* call = nullptr;
    LowerBlock* latch = nullptr;

    SmallArray<LowerInst*, 4> steps;

    // The accumulated value this iteration hands to the next, filled in by the rewrite.
    LowerPtr<LowerValue> carried = nullptr;
};

// Whether an instruction is a call to `fun` itself, made the way this function's own callers make
// one - same convention, same number of arguments, and a single returned value for the walk below
// to follow. A call returning through memory has no value to accumulate and is not one of these.
bool isSelfCall(LowerBase base, LowerFunction& fun, LowerInst* inst) {
    if(inst->kind != LowerInst::Call) return false;
    if(inst->createdCount != 1) return false;
    if(((LowerInstCall*)inst)->getCallType() != fun.callType) return false;
    if(inst->usedCount != fun.args.size() + 1) return false;

    auto callee = base[inst->used()[0]]->inst();
    return callee->kind == LowerInst::Fun && base[((LowerInstFun*)callee)->target] == &fun;
}

/*
 * The walk from a call's result to a `ret`, which is the whole of the analysis.
 *
 * It carries one value and asks what reads it. An accumulation consumes it and hands on its own
 * result; a phi consumes it and hands on the merged value one block along; a `ret` ends the walk.
 * Anything else - including a second reader of the value at any point - means the call's answer
 * outlives the call, and there is no tail position to speak of.
 */
bool analyzeChain(LowerBase base, LowerBlock* block, Size callIndex, LowerInst::Kind& op, Chain& chain) {
    auto current = block;
    auto start = callIndex + 1;
    auto value = base[current->instructions.get(base, callIndex)]->created().ptr - base;

    for(;;) {
        for(Size i = start; i < current->instructions.size(); i++) {
            auto inst = base[current->instructions.get(base, i)];
            if(!isBinary(inst) || !isAccumulator(inst->kind)) return false;

            auto result = ((LowerInstSingle*)inst)->created().ptr;
            if(!isInt(result->type)) return false;
            if(op != LowerInst::Nop && op != inst->kind) return false;

            // Exactly one operand is the value being carried. Both being it would mean the value has
            // two readers, which the check below refuses anyway.
            auto binary = (LowerInstBinary*)inst;
            if((binary->lhs == value) == (binary->rhs == value)) return false;
            if(base[value]->uses.size() != 1) return false;

            op = inst->kind;
            chain.steps.push(inst);
            value = result - base;
        }

        auto terminator = base[current->terminator];

        if(terminator->kind == LowerInst::Ret) {
            if(terminator->usedCount != 1 || terminator->used()[0] != value) return false;
            return base[value]->uses.size() == 1;
        }

        if(terminator->kind != LowerInst::Jmp) return false;
        auto next = base[((LowerInstJmp*)terminator)->then];

        /*
         * A block the walk steps into may hold nothing but phis, and must have somewhere else to be
         * entered from.
         *
         * An instruction in it is work that happens after the call on this path and before it on
         * every other one, so a back edge in place of this jump would skip it. And a block whose only
         * predecessor is this one becomes unreachable when the jump is redirected, which would leave
         * its phis with no alternatives at all.
         */
        if(next->instructions.isNotEmpty()) return false;
        if(next->incoming.size() < 2) return false;
        if(base[value]->uses.size() != 1) return false;

        auto user = base[base[value]->uses.get(base, 0)];
        if(!isPhi(user) || base[user->block] != next) return false;

        auto operands = user->used();
        auto sources = ((LowerInstPhi*)user)->sources();
        auto carried = false;

        for(Size i = 0; i < operands.length; i++) {
            if(base[sources.ptr[i]] != current) continue;
            carried = operands.ptr[i] == value;
            break;
        }

        if(!carried) return false;

        value = ((LowerInstSingle*)user)->created().ptr - base;
        current = next;
        start = 0;
    }
}

/*
 * Dropping one alternative from a phi.
 *
 * The operands and the source blocks are two arrays in one allocation, the second placed directly
 * after the first - so shrinking the operand count moves where the sources begin, and they have to
 * be written to their new home rather than merely shifted. Copied out first for that reason: the two
 * ranges overlap by all but one slot.
 */
void removePhiInput(LowerBase base, LowerInstPhi* phi, Size at) {
    auto operands = phi->used();
    auto sources = phi->sources();
    auto count = operands.length;
    assertTrue(at < count);

    dropUse(base, operands.ptr[at], (LowerInst*)phi - base);

    SmallArray<LowerPtr<LowerBlock>, 8> kept;
    for(Size i = 0; i < count; i++) {
        if(i != at) kept.push(sources.ptr[i]);
    }

    for(Size i = at; i + 1 < count; i++) operands.ptr[i] = operands.ptr[i + 1];
    phi->usedCount = U8(count - 1);

    auto moved = phi->sources();
    for(Size i = 0; i < kept.size(); i++) moved.ptr[i] = kept[i];
}

// One edge, forgotten from the side that receives it: the alternative every phi in the block offered
// for it, and the predecessor entry itself.
void removeIncomingEdge(LowerBase base, LowerBlock* block, LowerBlock* from) {
    for(auto phiPtr: block->phis.contents(base)) {
        auto phi = base[phiPtr];
        auto sources = phi->sources();

        for(Size i = 0; i < sources.length; i++) {
            if(base[sources.ptr[i]] != from) continue;
            removePhiInput(base, phi, i);
            break;
        }
    }

    for(Size i = 0; i < block->incoming.size(); i++) {
        if(base[block->incoming.get(base, i)] != from) continue;
        block->incoming.remove(base, i);
        return;
    }
}

// A block left with no terminator, ready for a new one. Everything the old one referred to stops
// counting as a reader of it - the value a `ret` handed back, and the edges a jump made.
void cutTerminator(LowerBase base, LowerBlock* block) {
    auto terminator = base[block->terminator];
    detach(base, terminator);

    for(auto& out: block->outgoing) {
        if(!out) continue;
        removeIncomingEdge(base, base[out], block);
        out = nullptr;
    }

    block->terminator = nullptr;
}

// Taking one instruction out of the block it is in. Its operands stop counting it as a reader; its
// own results must already have none, which is what the walk above established for every one of
// these.
void eraseInst(LowerBase base, LowerInst* inst) {
    detach(base, inst);
    auto block = base[inst->block];

    for(Size i = 0; i < block->instructions.size(); i++) {
        if(base[block->instructions.get(base, i)] != inst) continue;
        block->instructions.remove(base, i);
        return;
    }
}

/*
 * The block the back edges will arrive at.
 *
 * It is already there: a function's implicit entry block holds the arguments and an unconditional
 * jump, so the block it jumps to has one predecessor and no phis and a loop can be built round it as
 * it stands. What this checks is that it really is that block - one that is *already* a loop header
 * is declined rather than rebuilt, because the accumulator's initial value has to arrive on an edge
 * taken exactly once per call and an edge coming back round somebody else's loop would reset it.
 */
LowerBlock* findHeader(LowerBase base, LowerFunction& fun) {
    auto entry = base[fun.blocks.get(base, 0)];
    if(entry->instructions.isNotEmpty()) return nullptr;
    if(!entry->terminator || base[entry->terminator]->kind != LowerInst::Jmp) return nullptr;

    auto header = base[((LowerInstJmp*)base[entry->terminator])->then];
    if(header->phis.isNotEmpty() || header->incoming.size() != 1) return nullptr;

    return header;
}

/*
 * A block between the entry and the header, for the accumulator's initial value to be defined in.
 *
 * It has to be defined *somewhere*, and a phi alternative has to dominate the block the edge comes
 * from - which for the entry edge means dominating the entry block, and only the entry block does
 * that. Putting it there is not available either: `runLegalizer` in codegen/x64 emits the copies
 * that fetch the incoming arguments at index zero and asserts the entry block is empty, so an
 * instruction in front of them would be one those copies could overwrite the operands of.
 *
 * So the edge grows a block of its own. It runs exactly once per call, which is the property the
 * initial value needs and the only one this asks of it.
 */
LowerBlock* insertPreheader(LowerBase base, LowerModule& module, LowerFunction& fun, LowerBlock* header) {
    auto entry = base[fun.blocks.get(base, 0)];
    auto preheader = fun.addBlock(base, StringId());
    preheader->source = header->source;

    detach(base, base[entry->terminator]);
    entry->terminator = nullptr;
    entry->outgoing[0] = nullptr;

    // The header's only predecessor was the entry, and it has no phis for the edge to have been
    // named in - both of which `findHeader` established.
    header->incoming.remove(base, 0);

    jmp(base, module, *entry, preheader);
    jmp(base, module, *preheader, header);
    return preheader;
}

// A phi built detached, with room for one alternative per predecessor the header will have once the
// back edges are in place. Detached because `addInst` is what records its alternatives as uses, and
// they are not known until the edges exist - see `promoteStackSlots`, which builds its phis the same
// way and for the same reason.
LowerInstPhi* makePhi(LowerModule& module, Size count, LowerType type) {
    auto storage = module.arena.alloc(
        sizeof(LowerInstPhi) +
        sizeof(LowerPtr<LowerValue>) * count +
        sizeof(LowerPtr<LowerBlock>) * count);

    auto phi = new (storage) LowerInstPhi(StringId(), type);
    phi->usedCount = U8(count);
    return phi;
}

// The alternatives of one header phi: whatever each latch computed for it, and the value the
// function was entered with everywhere else. Written in `incoming` order because that is the order
// the edges were made in and the only one that is certainly complete.
void fillPhi(LowerBase base, LowerBlock& header, LowerInstPhi* phi, Array<Chain>& chains,
             LowerPtr<LowerValue> outside, SmallArray<LowerPtr<LowerValue>, 8>& perChain) {
    auto operands = phi->used();
    auto sources = phi->sources();
    Size at = 0;

    for(auto predecessor: header.incoming.contents(base)) {
        auto value = outside;

        for(Size i = 0; i < chains.size(); i++) {
            if(chains[i].latch - base == predecessor) { value = perChain[i]; break; }
        }

        operands.ptr[at] = value;
        sources.ptr[at] = predecessor;
        at++;
    }

    assertTrue(at == operands.length);
}

/*
 * The phis that say nothing, removed - the same shapes `promoteStackSlots` sweeps.
 *
 * There are three producers here. An argument the recursion passes through unchanged gets a phi whose
 * only other alternative is the phi itself; an argument nothing reads gets one nothing reads either;
 * and a block the walk stepped through loses the alternative the chain was arriving on, which can
 * leave it with one.
 */
void removeTrivialPhis(LowerBase base, Region<LowerRegion>& arena, LowerFunction& fun) {
    auto changed = true;

    while(changed) {
        changed = false;

        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];

            for(Size at = 0; at < block->phis.size(); at++) {
                auto phi = base[block->phis.get(base, at)];
                auto result = ((LowerInstSingle*)phi)->created().ptr;
                auto operands = phi->used();

                if(result->uses.isEmpty()) {
                    detach(base, (LowerInst*)phi);
                    block->phis.remove(base, at--);
                    changed = true;
                    continue;
                }

                LowerPtr<LowerValue> only = nullptr;
                auto trivial = true;

                for(Size i = 0; i < operands.length; i++) {
                    if(base[operands.ptr[i]] == result) continue;

                    if(!only) only = operands.ptr[i];
                    else if(only != operands.ptr[i]) { trivial = false; break; }
                }

                if(!trivial || !only) continue;

                detach(base, (LowerInst*)phi);
                replaceUses(base, arena, result - base, only);
                block->phis.remove(base, at--);
                changed = true;
            }
        }
    }
}

} // namespace

void eliminateTailRecursion(LowerBase base, LowerModule& module, LowerFunction& fun) {
    if(fun.blocks.size() < 2 || fun.args.size() > 32) return;

    /*
     * Storage the frame reserved, which is what makes a loop iteration and a recursive call
     * different things. See lower_tail.h - this is asked of the whole function rather than per
     * candidate, because the address a callee was handed is not one this pass can trace.
     */
    for(auto blockPtr: fun.blocks.contents(base)) {
        for(auto instPtr: base[blockPtr]->instructions.contents(base)) {
            if(base[instPtr]->kind == LowerInst::Alloca) return;
        }
    }

    auto op = LowerInst::Nop;
    Array<Chain> chains;

    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(!isSelfCall(base, fun, inst)) continue;

            Chain chain;
            chain.call = inst;
            chain.latch = block;

            // Speculative: `analyzeChain` fixes the accumulating operation for the whole function,
            // so a candidate it then declines must not leave that decision behind.
            auto attempt = op;
            if(!analyzeChain(base, block, i, attempt, chain)) continue;

            op = attempt;
            chains.push(::move(chain));

            // At most one chain per block: the walk requires the call's result to reach the block's
            // terminator, so a second call below the first is not in tail position and a second
            // above it would have had this one as a reader.
            break;
        }
    }

    if(chains.isEmpty()) return;

    auto header = findHeader(base, fun);
    if(!header) return;

    auto& arena = module.arena;
    auto accumulatorType = ((LowerInstSingle*)chains[0].call)->created().ptr->type;

    LowerInstPhi* accumulator = nullptr;
    LowerValue* identity = nullptr;

    // Before the latches are cut, because it changes what the header's one existing predecessor is
    // and the phis below are filled from that.
    if(op != LowerInst::Nop) insertPreheader(base, module, fun, header);

    /*
     * Every latch disconnected from what it used to leave by, before anything is built.
     *
     * This is what takes the chain's last value out of the phi that was reading it, and it has to
     * happen before the phis are sized: `cutTerminator` is what settles how many edges into the
     * header there will be.
     */
    for(auto& chain: chains) cutTerminator(base, chain.latch);

    /*
     * The phis, built detached and pointed at from their readers straight away.
     *
     * One per argument, and one for the accumulator where there is an operation to accumulate with.
     * The header dominates the whole function bar the two blocks above it, which hold nothing that
     * reads an argument, so every reader of one is a reader of its phi. That has to happen before
     * the calls are read below: an argument passed straight through must name the phi rather than
     * the value the function was entered with, or the second time round the loop it would hand on
     * the first iteration's.
     */
    auto predecessors = header->incoming.size() + chains.size();

    SmallArray<LowerInstPhi*, 8> argumentPhis;
    for(auto argPtr: fun.args.contents(base)) {
        auto arg = base[argPtr];
        auto phi = makePhi(module, predecessors, arg->result.type);
        phi->source = arg->source;

        replaceUses(base, arena, &arg->result - base, &phi->result - base);
        argumentPhis.push(phi);
    }

    if(op != LowerInst::Nop) {
        accumulator = makePhi(module, predecessors, accumulatorType);

        auto preheader = base[header->incoming.get(base, 0)];
        auto imm = new (arena) LowerImm(StringId(), accumulatorType, identityFor(op, accumulatorType));
        identity = ((LowerInstSingle*)preheader->addInst(base, imm))->created().ptr;
    }

    /*
     * Each call, replaced by the jump back.
     *
     * The accumulation is appended to the latch, which is where the call's block ends and so where
     * every operand of it is available. Only then are the chain and the call removed, by which point
     * neither has a reader left.
     */
    SmallArray<LowerPtr<LowerValue>, 8> carriedArguments;

    for(auto& chain: chains) {
        for(Size i = 0; i < fun.args.size(); i++) {
            carriedArguments.push(chain.call->used()[i + 1]);
        }

        // A chain with nothing to accumulate leaves the accumulator exactly as it found it, which is
        // what makes a plain tail call compatible with a function that accumulates elsewhere.
        LowerPtr<LowerValue> carried = accumulator ? &accumulator->result - base : nullptr;
        auto value = ((LowerInstSingle*)chain.call)->created().ptr - base;

        for(auto step: chain.steps) {
            auto binary = (LowerInstBinary*)step;
            auto other = binary->lhs == value ? binary->rhs : binary->lhs;

            auto sum = new (arena) LowerInstBinary(StringId(), accumulatorType, carried, other, op);
            sum->source = step->source;
            carried = ((LowerInstSingle*)chain.latch->addInst(base, sum))->created().ptr - base;
            value = ((LowerInstSingle*)step)->created().ptr - base;
        }

        chain.carried = carried;

        for(Size i = chain.steps.size(); i > 0; i--) eraseInst(base, chain.steps[i - 1]);

        auto callee = base[chain.call->used()[0]]->inst();
        eraseInst(base, chain.call);

        // The symbol the call named, where nothing else did. A code generator emits one of these as
        // an address computation, so leaving it behind would cost the instruction the call cost.
        if(((LowerInstSingle*)callee)->created().ptr->uses.isEmpty()) eraseInst(base, callee);

        jmp(base, module, *chain.latch, header);
    }

    // The alternatives, now that every edge into the header exists.
    SmallArray<LowerPtr<LowerValue>, 8> perChain;

    for(Size a = 0; a < fun.args.size(); a++) {
        perChain.clear();
        for(Size i = 0; i < chains.size(); i++) perChain.push(carriedArguments[i * fun.args.size() + a]);

        auto arg = base[fun.args.get(base, a)];
        fillPhi(base, *header, argumentPhis[a], chains, &arg->result - base, perChain);
        header->addInst(base, (LowerInst*)argumentPhis[a]);
    }

    if(accumulator) {
        perChain.clear();
        for(auto& chain: chains) perChain.push(chain.carried);

        fillPhi(base, *header, accumulator, chains, identity - base, perChain);
        header->addInst(base, (LowerInst*)accumulator);

        /*
         * And what every way out of the loop owes: the accumulator holds what the frames above this
         * one contributed, so a `ret` returns its own answer combined with that rather than on its
         * own. Every return in the function, not only the one the walk reached - the others are the
         * paths that used to be the recursion's base case.
         */
        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];
            auto terminator = base[block->terminator];
            if(terminator->kind != LowerInst::Ret || terminator->usedCount != 1) continue;

            auto returned = terminator->used()[0];
            auto sum = new (arena) LowerInstBinary(StringId(), accumulatorType,
                                                   &accumulator->result - base, returned, op);
            sum->source = terminator->source;

            auto total = ((LowerInstSingle*)block->addInst(base, sum))->created().ptr - base;
            setOperand(base, arena, terminator, terminator->used().ptr[0], base[total]);
        }
    }

    removeTrivialPhis(base, arena, fun);
}
