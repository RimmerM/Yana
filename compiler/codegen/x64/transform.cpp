#include "gen.h"
#include "x64_util.h"

// Whether running this instruction can change the flags register.
//
// Answered from the form selection would give it, which is the same function the final selection
// pass calls - so the two cannot drift apart. It runs here while the peepholes are still deciding
// what is implicit, and a peephole can change which form an instruction takes: an immediate that
// becomes embedded turns a register form into an immediate one. What it cannot change is whether
// the form writes the flags, which validateMachineForms checks for every opcode that does not
// explicitly declare its forms to differ - and the two that do (an immediate materialized with
// `xor` rather than `mov`, and a branch or select on a register rather than on the flags) select
// from the instruction alone rather than from anything a peephole decides.
inline bool modifiesFlags(LowerBase base, LowerInst* inst) {
    return writesFlags(machineTarget().form(selectForm(base, inst)).flagsEffect);
}

// Whether embedding this constant is worth doing at all, which is a size question rather than a
// legality one - fitsImmediate answers legality, per form, in canEmbedImm below.
//
// A one-byte immediate is always cheaper embedded. A four-byte one is cheaper only while it is read
// a couple of times; past that, one materialization into a register beats repeating four bytes at
// every use.
static bool isEmbeddableImm(LowerImm* imm) {
    // Floats can never be embedded.
    if(!isIntLike(imm->result.type)) return false;

    if(fitsImmediate(ImmediateWidth::Imm8, imm->i)) return true;
    return fitsImmediate(ImmediateWidth::Imm32, imm->i) && imm->result.uses.size() <= 2;
}

// Checks if this specific instruction can embed the provided embeddable immediate operand.
//
// Which operands can swallow which constant is the form table's answer - every operand position the
// value occupies has to have a form that accepts an immediate *of this width* there. A value read
// twice by one instruction, only one of whose positions takes an immediate, is not embeddable at
// all: embedding it would leave the other position with no location to read.
static bool canEmbedImm(LowerBase base, LowerInst* inst, LowerValue* op) {
    // A cast produces a value of its own, so the constant has to be materializable at the target
    // type - which for now means an integer one.
    auto kind = inst->kind;
    if(kind == LowerInst::Cast || kind == LowerInst::Bitcast) {
        if(!isIntLike(((LowerInstUnary*)inst)->result.type)) return false;
    }

    auto opcode = opcodeFor(base, inst);
    auto value = immValue(op);
    auto used = inst->used();
    bool found = false;

    for(Size i = 0; i < used.size(); i++) {
        if(base[used[i]] != op) continue;
        if(!opcodeCanEmbedImmediate(opcode, i, value)) return false;
        found = true;
    }

    return found;
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
        if(modifiesFlags(base, inst)) return true;
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
// constraints (the selected form) and the encoder (genCopy/genSetPattern) read one field instead of
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

// The same for every reader at once, which is what replacing a value with an equivalent one takes.
// The user list is snapshotted because moving a use rewrites the very list it is read from, and a
// user that reads the value twice appears twice and moves both of its entries across.
static void replaceAllUses(LowerBase base, LowerValue* from, LowerValue* to) {
    Array<LowerInst*> users;
    for(auto u: from->uses.contents(base)) users.push(base[u]);

    for(auto user: users) {
        replaceUse(base, from, user, to);

        auto used = user->used();
        for(Size i = 0; i < used.size(); i++) {
            if(base[used[i]] == from) used[i] = to - base;
        }
    }
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
 * Unsigned conversions.
 *
 * AMD64 converts between the two register banks in one instruction only where the integer side is
 * *signed*: `cvtsi2sd` reads an i64 and `cvttsd2si` writes one, and neither has an unsigned form
 * before AVX-512. So an unsigned conversion is a sequence rather than an instruction, which is
 * exactly why it is expanded here instead of being given a machine form - a form describes one
 * encoding with its operands, and there is no encoding to describe.
 *
 * Expanding into ordinary IR rather than into a pseudo is what keeps it cheap. Every instruction
 * below is one the backend already allocates, folds and encodes: each comparison is folded into the
 * select that reads it, each constant is embedded or materialized by the same peephole as any other,
 * and the register pressure is priced by the same costing. A pseudo would have needed scratch
 * registers nothing reserves, clobbers of its own, and an encoder that reproduced half of this file.
 *
 * The two 32-bit cases are exact rather than approximate, and for the same reason in both
 * directions: every u32 fits in an i64, so widening the *other* side and converting signed is
 * correct with nothing to correct afterwards.
 */

// The sequence replacing one conversion, built in front of it so that every value it produces is
// available wherever the conversion's own result was.
//
// Each step is a statement of its own rather than an argument to the next, because emitting appends
// to a list: nesting the calls would leave the order they run in up to the compiler's choice of
// argument evaluation order, and the wrong choice is a use before its definition.
struct Expansion {
    LowerBase base;
    LowerFunction& fun;
    LowerBlock* block;

    // Where the next instruction goes, which is the conversion's own position until the first one
    // has been inserted and pushed it down.
    Size at;

    LowerValue* emit(LowerInstSingle* inst) {
        insertInstAt(base, block, at++, inst);
        return &inst->result;
    }

    LowerValue* integer(LowerType type, U64 value) {
        return emit(new (fun.arena) LowerImm(0, type, value));
    }

    LowerValue* floating(LowerType type, F64 value) {
        return emit(new (fun.arena) LowerImm(0, type, value));
    }

    LowerValue* binary(LowerInst::Kind kind, LowerType type, LowerValue* lhs, LowerValue* rhs, StringId name = 0) {
        return emit(new (fun.arena) LowerInstBinary(name, type, lhs - base, rhs - base, kind));
    }

    LowerValue* convert(LowerType type, LowerValue* from, bool signedSource, bool signedResult, StringId name = 0) {
        return emit(new (fun.arena) LowerInstCast(name, type, from - base, signedSource, signedResult));
    }

    LowerValue* compare(LowerCmp cmp, LowerValue* lhs, LowerValue* rhs) {
        return emit(new (fun.arena) LowerInstCmp(0, lhs - base, rhs - base, cmp));
    }

    // `select` yields its first value when the condition holds, which is the order the machine form
    // and the encoder both read it in.
    LowerValue* select(LowerType type, LowerValue* condition, LowerValue* whenTrue, LowerValue* whenFalse, StringId name = 0) {
        return emit(new (fun.arena) LowerInstSelect(name, whenTrue - base, whenFalse - base, condition - base, type));
    }
};

// An unsigned integer widened into a float.
static LowerValue* expandUnsignedToFloat(Expansion& e, LowerValue* value, LowerType to, StringId name) {
    if(value->type == LowerType::Int32) {
        auto wide = e.convert(LowerType::Int64, value, false, false);
        return e.convert(to, wide, true, false, name);
    }

    // Only the top bit makes the signed conversion wrong, so a value that has it set is halved until
    // it does not and the result doubled back. Halving with `(x >> 1) | (x & 1)` rather than with a
    // plain shift keeps the bit that would have been shifted out where the rounding can still see
    // it: dropping it would round a value that should have gone up to even instead.
    //
    // Both halves are computed and one is selected rather than branched over. Neither can trap - a
    // conversion of a negative i64 is simply a negative float, which this path then discards - so
    // the arm that is wrong on a given input costs an instruction rather than correctness.
    auto one = e.integer(LowerType::Int64, 1);
    auto half = e.binary(LowerInst::Shr, LowerType::Int64, value, one);
    auto odd = e.binary(LowerInst::And, LowerType::Int64, value, one);
    auto rounded = e.binary(LowerInst::Or, LowerType::Int64, half, odd);
    auto halved = e.convert(to, rounded, true, false);
    auto doubled = e.binary(LowerInst::Add, to, halved, halved);
    auto direct = e.convert(to, value, true, false);

    // Signed-less-than-zero is exactly "the top bit is set", which is the one case the direct
    // conversion gets wrong. The comparison is emitted immediately in front of the select so that
    // the folding leaves it in the flags rather than materializing it.
    auto zero = e.integer(LowerType::Int64, 0);
    auto negative = e.compare(LowerCmp::ilt, value, zero);
    return e.select(to, negative, doubled, direct, name);
}

// A float truncated into an unsigned integer.
static LowerValue* expandFloatToUnsigned(Expansion& e, LowerValue* value, LowerType to, StringId name) {
    if(to == LowerType::Int32) {
        auto wide = e.convert(LowerType::Int64, value, false, true);
        return e.convert(to, wide, false, false, name);
    }

    // Everything below 2^63 converts signed exactly. Everything above it is brought into range by
    // subtracting 2^63 - which is exact, both operands being of the same magnitude - and the bit
    // that removes is put back into the integer afterwards.
    //
    // As above, both arms are computed and one selected. The comparison has the select as its only
    // reader and sits directly in front of it, so it stays in the flags.
    auto limit = e.floating(value->type, 9223372036854775808.0);
    auto reduced = e.binary(LowerInst::Sub, value->type, value, limit);
    auto big = e.convert(LowerType::Int64, reduced, false, true);
    auto sign = e.integer(LowerType::Int64, 0x8000000000000000);
    auto flipped = e.binary(LowerInst::Xor, LowerType::Int64, big, sign);
    auto small = e.convert(LowerType::Int64, value, false, true);
    auto isBig = e.compare(LowerCmp::ge, value, limit);
    return e.select(LowerType::Int64, isBig, flipped, small, name);
}

static void expandUnsignedConversions(LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // Indexed rather than buffered, and advanced by hand rather than by the loop, because both
        // things this does to an instruction move the ones after it: an expansion inserts in front
        // of the conversion and every removal closes the gap it leaves.
        for(Size i = 0; i < block->instructions.size(); ) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Cast) { i++; continue; }

            auto cast = (LowerInstCast*)inst;
            auto value = base[cast->from];
            auto to = cast->result.type;

            // A conversion is unsigned when the *integer* side is, which is whichever of the two
            // flags the cast carries depending on the direction it goes in. A cast that crosses no
            // bank boundary is not a conversion at all.
            auto toFloat = isFloat(to) && !isFloat(value->type);
            auto fromFloat = isFloat(value->type) && !isFloat(to);
            auto unsignedConversion = (toFloat && !cast->isSignedSource())
                || (fromFloat && !cast->isSignedResult());

            if(!unsignedConversion) { i++; continue; }

            // A conversion nothing reads is removed rather than expanded. It is dead either way -
            // every instruction the expansion produces is side-effect free - but expanding it first
            // would make ten dead instructions out of one, and no later pass would take them out.
            // Whatever followed has moved into this position, so the walk stays where it is.
            if(cast->result.uses.isEmpty()) {
                removeInst(base, cast);
                continue;
            }

            Expansion e { base, fun, block, i };
            auto replacement = toFloat
                ? expandUnsignedToFloat(e, value, to, cast->result.name)
                : expandFloatToUnsigned(e, value, to, cast->result.name);

            replaceAllUses(base, &cast->result, replacement);
            removeInst(base, cast);

            // Past the whole expansion. The insertions left it occupying the positions the
            // conversion's own used to begin at, and removing the conversion from the end of it
            // closed the gap - so `at` is where the walk carries on. Nothing in what was produced is
            // an unsigned conversion, so there is nothing there to come back for.
            i = e.at;
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

// `base + index*scale + displacement`, with either register absent: x86 encodes a bare displacement,
// a base alone, an index alone (the no-base SIB form) and both together.
struct AddressPattern {
    LowerValue* base = nullptr;
    LowerValue* index = nullptr;
    U8 scale = 1;
    I64 displacement = 0;
};

// Whether `user` reads `v` as the address of a memory access and nowhere else. *Which* operand that
// is comes from the opcode rather than from a list of instruction kinds here - a load, a store and a
// cache-control intrinsic all name one - and an instruction whose opcode names none reads no address
// at all.
//
// An X86Address can only occupy that one position, so `store %p, %p` reads the same value once as an
// address and once as a value, and rewriting only the first would leave the second pointing at an
// instruction about to be removed.
static bool isAddressOperand(LowerBase base, LowerInst* user, LowerValue* v) {
    auto index = opcodeAddressOperand(opcodeFor(base, user));
    if(index < 0) return false;

    auto used = user->used();
    if(base[used[index]] != v) return false;

    for(Size i = 0; i < used.size(); i++) {
        if(I32(i) != index && base[used[i]] == v) return false;
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
// extends an address displacement from 32 bits, so the range it can hold is exactly a four-byte
// immediate's - and whether the immediate was made implicit is irrelevant, since the value is read
// here rather than encoded from a register.
static Maybe<I64> addressDisplacement(LowerValue* v) {
    if(v->inst()->kind != LowerInst::Imm) return Nothing();

    auto imm = immValue(v);
    if(!fitsImmediate(ImmediateWidth::Imm32, imm)) return Nothing();

    return Just(I64(I32(U32(imm))));
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

// Peels `base + index*scale + displacement` off `address`, stopping as soon as what is left is not
// exclusively this address's own arithmetic. `folded` collects the instructions that become dead, in
// the order they can be removed: an outer add before the shift it absorbed, so that each is already
// unused by the time it goes.
//
// The caller decides what the peeled pattern becomes. An address every user reads as an address
// becomes an X86Address folded into each of them; anything else becomes an X86Lea that computes it
// into a register - see foldLeas.
static bool peelAddress(LowerBase base, LowerValue* address, AddressPattern& out, Array<LowerInst*>& folded) {
    out.base = address;

    // The folded instruction that reads whatever `out.base` ended up being, which is what the
    // index-only step below needs in order to prove that nothing else reads it. Null while nothing
    // has been peeled at all, since then the base is the address itself and its readers are the
    // caller's business.
    LowerInst* baseUser = nullptr;

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
        baseUser = inst;

        folded.push(inst);
        if(scaled) folded.push(scaled);

        // Anything else reading what is left stops the chain here: that value stays materialized,
        // and folding further would compute it twice rather than once.
        if(!isOnlyUse(base, next, inst)) break;
    }

    // `[index*scale + disp32]` with no base at all is a legal SIB form, and it is what a scaled index
    // with nothing left to add it to becomes: what the loop above stopped on is the multiply, which
    // the addressing unit does for free but which would otherwise stay an instruction whose result
    // the address reads as an unscaled base. This is the shape an absolute address indexed at run
    // time takes - the offset is the displacement, and there is no pointer to add it to.
    //
    // Only worth it for a real scaling. At scale 1 the index register is the register the base would
    // have been, so nothing is saved - and `[reg]` would become a SIB byte plus a four-byte
    // displacement for the privilege.
    if(!out.index && baseUser) {
        auto candidate = out.base;
        auto user = baseUser;
        LowerInst* bitcast = nullptr;

        // A bitcast is what the lowering has to write to use a computed integer as an address, and
        // between two 64-bit classes it computes nothing: the value and its cast are the same bits in
        // the same register. So the scaled index behind one is still a scaled index, and taking it as
        // the address's index removes the cast along with the multiply.
        //
        // Only looked through here, and not for a base: a base reaches the access in a register
        // either way, so seeing through the cast would change which register that is for no gain.
        if(candidate->inst()->kind == LowerInst::Bitcast && isOnlyUse(base, candidate, user)) {
            auto source = base[((LowerInstUnary*)candidate->inst())->from];

            if(is64Bit(source->type) && is64Bit(candidate->type)) {
                bitcast = candidate->inst();
                user = bitcast;
                candidate = source;
            }
        }

        LowerValue* index = nullptr;
        U8 scale = 1;

        // matchScaled proves what this needs: a 64-bit multiply or shift by an encodable factor, read
        // by nothing but the instruction that is about to be folded away.
        if(matchScaled(base, candidate, user, index, scale) && scale != 1) {
            // Outermost first, so that each is already unused by the time it goes.
            if(bitcast) folded.push(bitcast);
            folded.push(candidate->inst());

            out.index = index;
            out.scale = scale;
            out.base = nullptr;
        }
    }

    return folded.isNotEmpty();
}

static bool matchAddress(LowerBase base, LowerValue* address, AddressPattern& out, Array<LowerInst*>& folded) {
    if(!isOnlyUsedAsAddress(base, address)) return false;

    return peelAddress(base, address, out, folded);
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

            // Every instruction that references memory, which is the ones whose opcode names an
            // address operand - not a list of kinds. The operand is already an X86Address when an
            // earlier access on the same chain folded it for every user at once.
            auto operand = opcodeAddressOperand(opcodeFor(base, inst));
            if(operand < 0) continue;

            auto address = base[inst->used()[operand]];
            if(isMem(address)) continue;

            AddressPattern pattern;
            Array<LowerInst*> folded;
            if(!matchAddress(base, address, pattern, folded)) continue;

            // Snapshotted: the loop below rewrites the very list it is reading.
            Array<LowerInst*> users;
            for(auto u: address->uses.contents(base)) users.push(base[u]);

            for(auto user: users) {
                auto computed = new (arena) LowerInstX86Address(
                    LowerInst::X86Address, 0,
                    pattern.base ? pattern.base - base : nullptr,
                    pattern.index ? pattern.index - base : nullptr,
                    pattern.scale, U32(I32(pattern.displacement))
                );

                auto userBlock = base[user->block];
                insertInstAt(base, userBlock, indexOfInst(base, userBlock, user), computed);

                // Each user's own address operand, which matchAddress already established every one
                // of them has - the users of one folded chain need not all be the same instruction.
                replaceUse(base, address, user, &computed->result);
                user->used()[opcodeAddressOperand(opcodeFor(base, user))] = &computed->result - base;
            }

            for(auto dead: folded) removeInst(base, dead);

            // Both the insertions and the removals moved things around underneath the walk, so the
            // position to carry on from is wherever this access ended up.
            i = indexOfInst(base, block, inst);
        }
    }
}

/*
 * `lea`.
 *
 * The fold above only fires for an address computation every user reads *as an address*, because
 * that is the case where the arithmetic disappears entirely. An address that has to end up in a
 * register - pointer arithmetic passed to a call, an element pointer written to memory, a base kept
 * across a branch - still gets the same addressing unit, just with the answer materialized: that is
 * what `lea` is.
 *
 * `lea` is worth reaching for in exactly two shapes, and neither is "every pointer add". It computes
 * `base + index*{1,2,4,8} + disp` in one instruction where the lowering emitted two or three, and it
 * writes its result somewhere other than its operands, where `add` overwrites the first of them and
 * so needs a copy in front of it whenever that operand is still read afterwards. Where neither
 * applies, `add` is one instruction of the same length and is left alone.
 */

// Whether replacing this chain with an `lea` costs fewer instructions than leaving it alone.
//
// The base's use list still counts the instruction about to be folded away, so "used more than once"
// is what "read somewhere else as well, and therefore copied before an `add` could overwrite it"
// looks like from here.
static bool isLeaProfitable(const AddressPattern& pattern, const Array<LowerInst*>& folded) {
    if(folded.size() > 1) return true;

    // An index-only address folded the multiply that produced it, so there is nothing left for an
    // `add` to have been - and no base whose use count could say anything either way.
    if(!pattern.base) return true;

    return pattern.base->uses.size() > 1;
}

static void foldLeas(LowerBase base, LowerFunction& fun) {
    auto& arena = fun.arena;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // Backwards, so that the top of a chain is reached before the arithmetic feeding it. The
        // other way round, `p + i*4` would become an `lea` of its own and leave the `+ 24` above it
        // behind as a second instruction, where taking the outer add first absorbs both.
        Size i = block->instructions.size();

        while(i > 0) {
            i--;

            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Add && inst->kind != LowerInst::Sub) continue;

            // Pointer arithmetic only, for the reason the fold above gives: the address unit works
            // at 64 bits and does not wrap where a narrower operation does.
            auto& result = ((LowerInstBinary*)inst)->result;
            if(!isPtr(result.type) || isImplicit(&result) || result.uses.isEmpty()) continue;

            AddressPattern pattern;
            Array<LowerInst*> folded;
            if(!peelAddress(base, &result, pattern, folded)) continue;
            if(!isLeaProfitable(pattern, folded)) continue;

            auto lea = new (arena) LowerInstX86Address(
                LowerInst::X86Lea, result.name,
                pattern.base ? pattern.base - base : nullptr,
                pattern.index ? pattern.index - base : nullptr,
                pattern.scale, U32(I32(pattern.displacement))
            );

            // In front of the instruction it replaces, which is where the value was already
            // available to everything that reads it.
            insertInstAt(base, block, i, lea);

            replaceAllUses(base, &result, &lea->result);
            for(auto dead: folded) removeInst(base, dead);

            // Both the insertion and the removals moved things around underneath the walk, so the
            // position to carry on from is wherever the new instruction ended up. Everything the
            // fold consumed was at or before it, and the `lea` itself is not a candidate.
            i = indexOfInst(base, block, lea);
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
 * reversed last.
 *
 * That generalizes, and is what the choice is actually made on: **explore the less likely successor
 * first**, so that the likely one is reversed into the position immediately after the branch and
 * becomes the fallthrough. A loop exit is only the case of it the CFG can derive on its own - the
 * edge leaving a loop is taken once where the edge staying in it is taken every iteration but the
 * last - and a branch the IR says is unlikely gets the same treatment for the same reason. Where the
 * two edges are equally likely there is nothing to prefer, and the loop depth decides: the deeper
 * successor goes last, so that a block entering a loop is followed by the loop rather than by
 * whatever comes after it. `edgeWeightsOf` is where those probabilities come from, and it is the
 * same one the block frequencies are computed from - so layout and cost cannot disagree about which
 * arm of a branch is the common one.
 */

static void traverseOrdered(LowerBase base, const LoopInfo& loops, LowerBlock* b, BlockList& out) {
    b->marker = 1;

    auto first = b->outgoing[0] ? base[b->outgoing[0]] : nullptr;
    auto second = b->outgoing[1] ? base[b->outgoing[1]] : nullptr;

    if(first && second) {
        auto weights = edgeWeightsOf(base, loops, b);

        auto swapThem = weights.weight[0] != weights.weight[1]
            ? weights.weight[0] > weights.weight[1]
            : loops.depth[first->index] > loops.depth[second->index];

        if(swapThem) ::swap(first, second);
    }

    if(first && !first->marker) traverseOrdered(base, loops, first, out);
    if(second && !second->marker) traverseOrdered(base, loops, second, out);

    out.push(b->index);
}

static void orderBlocks(LowerBase base, LowerFunction& fun) {
    auto blockList = fun.blocks.contents(base);
    auto entry = base[fun.blocks.get(base, 0)];

    // Also leaves each block's loop depth on it, which is where the ordering below reads it back
    // from after the renumbering has invalidated the index-keyed result.
    auto loops = fun.buildLoops(base);

    for(auto o: blockList) base[o]->marker = 0;

    BlockList postorder(blockList.size());
    traverseOrdered(base, loops, entry, postorder);

    // A block that the entry point cannot reach has no place in the ordering, and nothing
    // downstream is prepared to allocate registers for one.
    assertTrue(postorder.size() == fun.blocks.size());

    Array<LowerPtr<LowerBlock>> ordered;
    for(Size i = postorder.size(); i > 0; i--) {
        ordered.push(fun.blocks.get(base, postorder[i - 1]));
    }

    for(Size i = 0; i < ordered.size(); i++) {
        auto b = base[ordered[i]];

        fun.blocks.set(base, i, ordered[i]);
        b->index = BlockIndex(i);
    }
}

/*
 * The transform pipeline.
 *
 * The passes below used to be one function with the order expressed as the sequence of statements in
 * it, and the reasons for that order as comments between them. They are named passes now, with the
 * order stated once in kTransformPipeline and each pass's contract stated next to the pass.
 *
 * The order is not arbitrary and each step of it is load-bearing:
 *
 *   canonicalizeOperands       puts immediates where the later passes expect to find them, so that
 *                              nothing downstream has to check both sides of a commutative operation
 *   selectAddressesAndLeas     removes address arithmetic *before* liveness, which is the only point
 *                              at which removing it actually shortens an interval - and before the
 *                              immediate peephole, so that an immediate the fold leaves with no uses
 *                              is made implicit rather than materialized into a register nothing reads
 *   selectMachineInstructions  chooses the shape of each instruction: which immediates are embedded,
 *                              which comparisons stay in the flags, which callees are elided, which
 *                              encoding a block operation takes
 *   lowerOutgoingStackArguments  turns a call's stack-passed arguments into explicit stores, which is
 *                              only worth doing once the passes above have settled what is implicit
 *   normalizePhiEdges          gives every phi transfer a block it can safely be emitted in
 *   analyzeLoopsAndOrderBlocks lays the blocks out, last, since it invalidates every instruction
 *                              index the passes above reasoned about
 *
 * A pass that changes any of this changes the pipeline table, not the reading order of one function.
 */

// Walks every instruction of every block in list order, with its index within its block. For passes
// that only inspect and annotate: one that inserts or removes instructions has to iterate by index,
// because the list is rewritten underneath it.
template<class F>
static void forEachInst(LowerBase base, LowerFunction& fun, F&& onInst) {
    for(auto b: fun.blocks.contents(base)) {
        Size i = 0;

        for(auto inst: base[b]->instructions.contents(base)) {
            onInst(base[inst], i);
            i++;
        }
    }
}

/*
 * The passes.
 */

// Moves operands into the canonical position for the passes below - today, an immediate onto the
// right-hand side of a commutative operation, so that nothing downstream has to look at both sides.
// Representation-neutral: no target register or encoding decision is made here.
//
// Expects: the lowering's output, unmodified.  Establishes: commutative immediates on the right.
// Mutates: operand order within an instruction. Invalidates: nothing.
static void canonicalizeOperands(LowerBase base, LowerFunction& fun) {
    forEachInst(base, fun, [&](LowerInst* inst, Size i) {
        trySwapOperands(base, inst);
    });
}

// Recognizes `base + index*scale + displacement` once, and turns each occurrence into either an
// X86Address folded into the access that reads it (§3.1) or an X86Lea that materializes it (§3.3).
//
// Runs before the peepholes rather than after them: an immediate whose only use was an address
// computation is left with none by the fold, and is then made implicit by the pass below rather than
// being materialized into a register nothing reads. It also runs before liveness, which is what lets
// the arithmetic it eliminates genuinely shorten intervals.
//
// Expects: canonical operands.  Establishes: no memory access reaches allocation with a foldable
// address computation in front of it. Mutates: the instruction lists and every affected use list.
// Invalidates: instruction positions within a block.
static void selectAddressesAndLeas(LowerBase base, LowerFunction& fun) {
    foldAddresses(base, fun);
    foldLeas(base, fun);
}

// Chooses the shape of each instruction: which immediates are embedded into the encoding, which
// comparisons stay in the flags, which direct callees need no register, and which of its two
// encodings a block operation takes.
//
// This is where an instruction stops being purely semantic. Every decision here is recorded on the
// instruction - as the Implicit flag, an embedded comparison, or the unrolled flag - so that the
// allocator, the form selection below and the encoder all read one answer instead of each deriving it.
//
// Expects: addresses selected.  Establishes: every value that occupies no location is marked
// Implicit, and every Copy/SetPattern has its encoding recorded. Mutates: value flags and
// instruction annotations only. Invalidates: nothing.
static void selectMachineInstructions(LowerBase base, LowerFunction& fun) {
    forEachInst(base, fun, [&](LowerInst* inst, Size i) {
        if(inst->kind == LowerInst::Imm) {
            tryEmbedImm(base, (LowerImm*)inst);
        }

        // Needs the instruction's index within its block, to walk forward from the comparison to its
        // use looking for anything that writes the flags in between.
        if(inst->kind == LowerInst::Cmp) {
            tryMergeCompare(base, (LowerInstCmp*)inst, i);
        }

        if(inst->kind == LowerInst::Fun) {
            tryElideDirectCallee(base, (LowerInstFun*)inst);
        }

        selectBlockOpEncoding(base, inst);
    });
}

// Turns a call's stack-passed arguments into explicit stores into the outgoing argument area, placed
// as early as is safe - see the block comment on outgoing stack arguments above.
//
// Expects: machine instructions selected, so that an argument the passes above made implicit is
// already implicit when its location is decided.  Establishes: no call operand is passed on the
// stack; every one of them is an X86PushArg result instead. Mutates: the instruction lists and the
// affected use lists. Invalidates: instruction positions within a block.
static void lowerOutgoingStackArguments(LowerBase base, LowerFunction& fun) {
    insertStackArgs(base, fun, targetConstraints());
}

// Splits every edge on which a phi transfer needs an insertion point of its own.
//
// Expects: no pass that reasons about instruction positions left to run.  Establishes: no block with
// two successors has a successor with phis, so a phi copy at the end of a predecessor cannot run on
// a path that skips the phis. Mutates: the block list and the CFG. Invalidates: block indices.
static void normalizePhiEdges(LowerBase base, LowerFunction& fun) {
    splitPhiEdges(base, fun);
}

// Finds the loops and rewrites the block list into the reverse postorder that follows them and the
// branch probabilities - see the block-order comment above.
//
// Expects: the CFG in its final shape, since the edge probabilities it lays the blocks out by are
// read from it. Establishes: blocks in reverse postorder with the likely successor of each branch
// immediately behind it, `index` equal to list position, and `loopDepth` set. Mutates: the block
// list order and block metadata. Invalidates: nothing after it.
static void analyzeLoopsAndOrderBlocks(LowerBase base, LowerFunction& fun) {
    orderBlocks(base, fun);
}

// Records, for every instruction, the machine opcode and the machine form it was selected into - see
// machine.h. Everything downstream reads its facts from there: which operands are forced into
// particular registers, what the instruction clobbers, which result is written over which operand,
// which operand may stay in a frame slot, what it does to the flags.
//
// Last, and not where §4.3 of the plan puts it, for one reason: an instruction cannot be given a
// form before it exists, and two passes above create instructions - the argument stores, and the
// jumps in the blocks that phi-edge splitting inserts. The peepholes still make every decision the
// form depends on; this pass only writes the answer down.
//
// Expects: no pass left that creates instructions or changes an instruction's shape.  Establishes: a
// selected form for every instruction in the function. Mutates: nothing in the IR.
static void selectMachineForms(LowerBase base, LowerFunction& fun, MachineFunction& machine) {
    auto select = [&](LowerInst* inst) {
        machine.select(inst, opcodeFor(base, inst), selectForm(base, inst), selectCondition(inst));
    };

    for(auto a: fun.args.contents(base)) select((LowerInst*)base[a]);

    for(auto b: fun.blocks.contents(base)) {
        auto block = base[b];

        for(auto p: block->phis.contents(base)) select(base[p]);
        for(auto i: block->instructions.contents(base)) select(base[i]);
        select(base[block->terminator]);
    }
}

/*
 * Pipeline invariants.
 *
 * Checked between passes in debug builds. The structural ones are what the mutating passes can
 * actually break: inserting an instruction, removing a dead one, moving a use from one value to
 * another and splitting an edge all have to keep four separate lists agreeing with each other, and a
 * stale entry in any of them is invisible until the allocator reads it and concludes that a dead
 * value is live - a wrong answer several passes away from its cause.
 */

enum TransformInvariant: U32 {
    // Every pass establishes this one: instruction lists, use lists and CFG links agree.
    InvariantStructure = 1 << 0,

    // No block with two successors has a successor with phis.
    InvariantPhiEdgesNormalized = 1 << 1,

    // Block list position and BlockIndex agree.
    InvariantBlocksOrdered = 1 << 2,
};

struct TransformPass {
    StringView name;
    void (*run)(LowerBase base, LowerFunction& fun);

    // What holds once this pass has run, and holds for every pass after it.
    U32 establishes;
};

static const TransformPass kTransformPipeline[] = {
    { "expandUnsignedConversions"_v,   expandUnsignedConversions,   0 },
    { "canonicalizeOperands"_v,        canonicalizeOperands,        0 },
    { "selectAddressesAndLeas"_v,      selectAddressesAndLeas,      0 },
    { "selectMachineInstructions"_v,   selectMachineInstructions,   0 },
    { "lowerOutgoingStackArguments"_v, lowerOutgoingStackArguments, 0 },
    { "normalizePhiEdges"_v,           normalizePhiEdges,           InvariantPhiEdgesNormalized },
    { "analyzeLoopsAndOrderBlocks"_v,  analyzeLoopsAndOrderBlocks,  InvariantBlocksOrdered },
};

// Every instruction the function owns, in no particular order: the arguments, then each block's
// phis, instructions and terminator. Arguments and phis are not in any block's instruction list but
// do contribute uses, so a check that ignored them would report every one of theirs as stale.
template<class F>
static void forEachOwnedInst(LowerBase base, LowerFunction& fun, F&& f) {
    for(auto a: fun.args.contents(base)) f((LowerInst*)base[a]);

    for(auto b: fun.blocks.contents(base)) {
        auto block = base[b];

        for(auto p: block->phis.contents(base)) f(base[p]);
        for(auto i: block->instructions.contents(base)) f(base[i]);
        if(block->terminator) f(base[block->terminator]);
    }
}

static bool verifyTransformInvariants(Context& ctx, LowerBase base, LowerFunction& fun, U32 established) {
    auto funName = ctx.findName(fun.name);
    auto ok = true;

    auto fail = [&](auto&& fmt, auto&&... args) {
        ok = false;
        logError(fmt, forward<decltype(args)>(args)...);
    };

    // How many times each value is read, counted from the operand lists. Compared afterwards against
    // the value's own use list, which is the direction that catches a use entry left behind by a
    // removed instruction.
    HashMap<LowerValue*, U32> reads;

    forEachOwnedInst(base, fun, [&](LowerInst* inst) {
        for(auto offset: inst->used()) {
            auto v = base[offset];
            auto count = reads.get(v);
            if(count.isJust()) count.unwrap()++;
            else reads.add(v, 1);
        }
    });

    for(auto b: fun.blocks.contents(base)) {
        auto block = base[b];

        if(!block->terminator) {
            fail("%@: block %@ has no terminator", funName, U32(block->index));
            continue;
        }

        // An instruction whose `block` names somewhere it is not listed is one that a move or an
        // insertion left behind, and every later pass that walks from the block would miss it.
        auto ownedBy = [&](LowerInst* inst) {
            if(base[inst->block] != block) {
                fail("%@: block %@ lists an instruction whose own block is %@",
                    funName, U32(block->index), U32(base[inst->block]->index));
            }
        };

        for(auto p: block->phis.contents(base)) ownedBy(base[p]);
        for(auto i: block->instructions.contents(base)) ownedBy(base[i]);
        ownedBy(base[block->terminator]);

        // Successor and predecessor lists are two records of one edge, and a pass that updates only
        // one of them produces a CFG the liveness and the layout disagree about.
        for(auto o: block->outgoing) {
            if(!o) continue;

            bool found = false;
            for(auto p: base[o]->incoming.contents(base)) {
                if(base[p] == block) { found = true; break; }
            }

            if(!found) {
                fail("%@: block %@ names block %@ as a successor, which does not name it back",
                    funName, U32(block->index), U32(base[o]->index));
            }

            if((established & InvariantPhiEdgesNormalized) &&
               block->outgoing[0] && block->outgoing[1] && base[o]->phis.isNotEmpty())
            {
                fail("%@: block %@ has two successors and block %@ has phis",
                    funName, U32(block->index), U32(base[o]->index));
            }
        }

        // Edge likelihood survives every CFG transform, or the layout and the frequencies are
        // reasoning about a branch that no longer exists. Splitting an edge is the case that could
        // lose one - it retargets `then` or `otherwise` - and what would show here is a branch that
        // came out with a weight on one edge and nothing on the other, which is not a ratio.
        if(base[block->terminator]->kind == LowerInst::Je) {
            auto je = (LowerInstJe*)base[block->terminator];

            for(auto& likelihood: je->likelihood) {
                auto stated = likelihood.source != LikelihoodSource::Unknown;

                if(stated != je->hasLikelihood()) {
                    fail("%@: branch in block %@ states an edge weight for one edge only",
                        funName, U32(block->index));
                }

                if(likelihood.weight < 1 || likelihood.weight > kMaxEdgeWeight) {
                    fail("%@: branch in block %@ has an edge weight out of range",
                        funName, U32(block->index));
                }
            }
        }

        // A phi takes one value per predecessor, from a block that is actually one.
        for(auto p: block->phis.contents(base)) {
            auto phi = base[p];
            auto sources = phi->sources();

            if(sources.size() != phi->used().size()) {
                fail("%@: phi in block %@ has %@ sources for %@ operands",
                    funName, U32(block->index), U32(sources.size()), U32(phi->used().size()));
            }

            for(auto source: sources) {
                bool found = false;
                for(auto in: block->incoming.contents(base)) {
                    if(in == source) { found = true; break; }
                }

                if(!found) {
                    fail("%@: phi in block %@ takes a value from block %@, which is not a predecessor",
                        funName, U32(block->index), U32(base[source]->index));
                }
            }
        }
    }

    if(established & InvariantBlocksOrdered) {
        auto blocks = fun.blocks.contents(base);

        for(Size i = 0; i < blocks.size(); i++) {
            if(base[blocks[i]]->index != BlockIndex(i)) {
                fail("%@: block at position %@ is numbered %@",
                    funName, U32(i), U32(base[blocks[i]]->index));
            }
        }
    }

    // The other direction: a use list that claims more or fewer readers than there are.
    forEachOwnedInst(base, fun, [&](LowerInst* inst) {
        for(auto& created: inst->created()) {
            auto counted = reads.get(&created);
            auto expected = counted.isJust() ? counted.unwrap() : 0;

            if(created.uses.size() != expected) {
                fail("%@: a value's use list has %@ entries for %@ actual readers",
                    funName, U32(created.uses.size()), expected);
            }
        }
    });

    return ok;
}

void transformFunction(Context& ctx, LowerBase base, LowerFunction& fun, MachineFunction& machine) {
    U32 established = 0;

    for(auto& pass: kTransformPipeline) {
        pass.run(base, fun);
        established |= pass.establishes;

        // Debug builds only - assertTrue compiles away entirely in a release build, taking the call
        // with it. Running between passes rather than once at the end is the point: it names the
        // pass that broke the invariant rather than the pipeline that ended up violating it.
        assertTrue(verifyTransformInvariants(ctx, base, fun, established | InvariantStructure));
    }

    // Writes down what the passes above decided. Separate from the pipeline table because it
    // produces the MachineFunction rather than mutating the IR, and because it has to see every
    // instruction the passes above created.
    selectMachineForms(base, fun, machine);

    // The first of the boundary checks, at the boundary it belongs to: everything after this reads
    // the selection rather than the instructions, so a form that does not match the instruction it
    // was chosen for is a wrong answer nothing downstream can notice.
    assertTrue(verifySelection(ctx, base, fun, machine));
}
