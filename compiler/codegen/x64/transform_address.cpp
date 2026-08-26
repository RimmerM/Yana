#include "transform_internal.h"

/*
 * Memory, folded into the instruction that touches it.
 *
 * Four folds, run in the order transform.cpp states and for the reasons it gives there: the address
 * arithmetic becomes an addressing mode or an `lea`, a load becomes an operand of its reader, and a
 * load-modify-store of one place becomes one memory-destination instruction.
 */

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
 * required rather than incidental: the address's base and index are live *into* the access, so
 * anything that came between the two could overwrite them.
 *
 * One thing does come between them, and it was a live miscompile - the access's own operand copies.
 * Legalization hands scratch registers out per instruction, so an operand of the access could be
 * given the register the base had just been materialized into, and `xs[0] = 1` became a store
 * through the number 1. `foldedAddressRegs` in legalize.cpp is the rule that keeps this true, and
 * `checkFoldedAddress` in verify.cpp is the assertion that says so in a debug build - it was missing
 * because the rest of that file checks *operands*, and a folded address is not one.
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

// Whether every use of `v` is address arithmetic this fold is going to take apart, so that `v` is
// dead once the last of them has been rewritten even though no single one of them is its only use.
//
// This is what lets one shift serve as the scaled index of several addresses. The rule further in is
// "this is the only use", because folding a computation something else still reads would perform it
// twice; but a shift whose *every* reader is an address performs it zero times once they have all
// been rewritten, and the readers need not be the same instruction for that to hold.
//
// Deliberately narrow, since the cost of being wrong is a live range extended for nothing: each user
// has to be a pointer `add` - the one shape the peel below absorbs an index into - reading `v` once,
// and its result has to be an address and nothing else. Anything longer, and the chain above it might
// stop for a reason of its own and leave `v` materialized after all.
static bool isOnlyUsedAsScaledIndex(LowerBase base, LowerValue* v) {
    for(auto u: v->uses.contents(base)) {
        auto user = base[u];
        if(user->kind != LowerInst::Add) return false;

        auto binary = (LowerInstBinary*)user;
        auto lhs = base[binary->lhs];
        auto rhs = base[binary->rhs];

        // `add %o, %o` reads it in both positions, and an address has one index.
        if(lhs == rhs) return false;
        if(!isPtr(binary->result.type)) return false;
        if(!isOnlyUsedAsAddress(base, &binary->result)) return false;
    }

    return !v->uses.isEmpty();
}

// Matches `v` against `index * {1,2,4,8}`, the only scaling the SIB byte can encode. `exclusive` says
// whether this fold is what makes `v` dead - false when it is shared between several addresses, in
// which case the last of them to be folded is the one that removes it.
//
// Only a 64-bit multiply qualifies. A 32-bit `shl %i, 2` wraps at 32 bits and the address unit does
// not, so folding one would change what an index near the top of its range produces. A plain
// unscaled index is not subject to that: it reaches the address in the same register the 64-bit add
// would have read it from, whatever its declared width.
static bool matchScaled(LowerBase base, LowerValue* v, LowerInst* user, LowerValue*& index, U8& scale, bool& exclusive) {
    if(!is64Bit(v->type)) return false;

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

    // Last, since it is the only test here that walks a list: the shape has to be one the SIB byte
    // can hold before it is worth asking who else reads it.
    auto onlyUse = isOnlyUse(base, v, user);
    if(!onlyUse && !isOnlyUsedAsScaledIndex(base, v)) return false;

    index = source;
    scale = U8(factor);
    exclusive = onlyUse;
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
static bool peelAddress(LowerBase base, LowerValue* address, AddressPattern& out, SmallArray<LowerInst*, 8>& folded) {
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
            //
            // A shift shared between several addresses is taken apart by each of them but removed
            // only by the last, so `scaled` stays null for all but that one - the value is still
            // read, and the instruction has to stay until it is not.
            bool exclusive = false;

            if(matchScaled(base, rhs, inst, index, scale, exclusive)) {
                if(exclusive) scaled = rhs->inst();
                next = lhs;
            } else if(matchScaled(base, lhs, inst, index, scale, exclusive)) {
                if(exclusive) scaled = lhs->inst();
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
        bool exclusive = false;

        // matchScaled proves what this needs: a 64-bit multiply or shift by an encodable factor, read
        // by nothing but the instruction that is about to be folded away - or by nothing but other
        // addresses, in which case it stays until the last of them has been rewritten.
        if(matchScaled(base, candidate, user, index, scale, exclusive) && scale != 1) {
            // Outermost first, so that each is already unused by the time it goes.
            if(bitcast) folded.push(bitcast);
            if(exclusive) folded.push(candidate->inst());

            out.index = index;
            out.scale = scale;
            out.base = nullptr;
        }
    }

    return folded.isNotEmpty();
}

static bool matchAddress(LowerBase base, LowerValue* address, AddressPattern& out, SmallArray<LowerInst*, 8>& folded) {
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

void foldAddresses(LowerBase base, LowerFunction& fun) {
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
            SmallArray<LowerInst*, 8> folded;
            if(!matchAddress(base, address, pattern, folded)) continue;

            // Snapshotted: the loop below rewrites the very list it is reading.
            SmallArray<LowerInst*, 8> users;
            for(auto u: address->uses.contents(base)) users.push(base[u]);

            for(auto user: users) {
                auto computed = new (arena) LowerInstX86Address(
                    LowerInst::X86Address, StringId(),
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
static bool isLeaProfitable(const AddressPattern& pattern, const SmallArray<LowerInst*, 8>& folded) {
    if(folded.size() > 1) return true;

    // An index-only address folded the multiply that produced it, so there is nothing left for an
    // `add` to have been - and no base whose use count could say anything either way.
    if(!pattern.base) return true;

    return pattern.base->uses.size() > 1;
}

void foldLeas(LowerBase base, LowerFunction& fun) {
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
            SmallArray<LowerInst*, 8> folded;
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
 * Folding a load into the instruction that consumes it.
 *
 * Most of the AMD64 ALU reads one operand straight out of memory, and §5.5 already takes that for a
 * *frame slot*. What it does not take is a load the program wrote: `mov rax, [rdi]` followed by
 * `add rcx, rax` is two instructions where `add rcx, [rdi]` is one, at the same length in bytes.
 * The form that reads it there is the memory-source twin (MachineForm::memorySource); this is what
 * moves an instruction onto one. §5 of test/bench/findings.md is the measurement.
 *
 * What the fold rewrites is one operand. The consumer stops reading the load's result and reads the
 * *address* instead, and the load is removed - so what reaches allocation is an instruction shaped
 * exactly like a load: an `address()` operand holding an X86Address placed immediately above it.
 * Nothing in placement, legalization, emission or the verifiers learns a case for it; each asks the
 * selected form which operand is an address and gets an answer it already knew what to do with.
 *
 * That the operand holds an X86Address is also the whole record of the fold. It is the one value
 * that can only ever be an address, so selection reads the decision back off the operand list rather
 * than off a flag that would have to be kept in step with it - which is why a load whose pointer
 * arrived in a register is given one here as well. `[reg]` is an addressing mode like any other and
 * costs nothing to say.
 *
 * Three things bound it, and each of the three is load-bearing:
 *
 *  - **Nothing between the load and its reader may write memory.** Where the two are adjacent that is
 *    free, and it is all this asked for at first. Lifting it is §3.1.2's sink: the load is moved down
 *    to its reader rather than the reader moved up, so what has to hold is the same thing
 *    `foldStoreUpdates` asks of the same stretch - `writesStorage` over every instruction between,
 *    with a call not on the whitelist. The load's *address* travels with it, since an X86Address has
 *    to stand immediately in front of whatever dereferences it, and that is what needs the address to
 *    have no other reader: one left behind would be reading a value defined below it.
 *
 *    The distance is bounded (kMaxLoadSinkDistance) because the search walks up from the reader, so
 *    an unbounded one would make the pass quadratic in a block that folds nothing.
 *  - **The encoding reads exactly the bytes the load read.** A narrow load extends into its result,
 *    which an operand of an ALU instruction has no room to do, and an access at any other width
 *    would read a neighbouring value. This is the rule directMemoryOperands applies to a frame slot,
 *    asked of an address instead.
 *  - **Nothing may be copied into a fixed register in front of the instruction.** The address's own
 *    base and index are live *into* the consumer - they belong to the X86Address one above it - so a
 *    copy emitted in front could overwrite one. The destructive copy is covered, by
 *    collectTieConflicts in place.cpp; a fixed-register operand is not, which is what keeps the
 *    group-3 `mul` and `div` shapes out of this.
 */

/*
 * Whether running this instruction can write memory.
 *
 * `writesStorage` in lower_inst.h, which is the same question the mid-level passes ask and is now
 * asked through the same rows: it is the *write* half of "touches memory" rather than the whole of
 * it, which is what lets a load stand between the two accesses being fused - and one always does,
 * `b[k]` being read in the same expression.
 *
 * Ordered operations answer yes although some of them write nothing. A plain load sunk across an
 * acquire is a read moved to the wrong side of the edge it was meant to be after, which is the same
 * reason `writesStorage` includes them for every other reader.
 */

/*
 * How far above its reader a load may stand and still be folded into it.
 *
 * The search walks up from the reader, so this is what keeps the pass linear in the block rather
 * than quadratic over one that folds nothing. Sixteen is well past what the shapes this catches
 * need: what separates a load from its reader is the *other* operand's computation, which for an
 * array element is an index cast, a shift and an add.
 */
static constexpr Size kMaxLoadSinkDistance = 16;

// Whether exchanging this operation's operands leaves it computing the same thing. The same set
// trySwapOperands uses, and restricted to the integer bank for the same reason: a float addition is
// commutative in value but not in which NaN payload the machine propagates.
static bool isCommutativeInt(LowerInst* inst) {
    /*
     * The packed minimum and maximum, at an integer lane and not at a float one.
     *
     * `min(a, b)` and `min(b, a)` hold the same lanes for integers, so the operand the load feeds
     * may be moved into the position the encoding dereferences - which is the whole of what turns
     * `vmovdqu ; vpmaxsd` into one instruction. At a float lane the order is *the answer* for a NaN
     * and for a pair of zeros of opposite sign (see LowerInst::X86MinMax), so exchanging it there
     * would be a different operation wearing the same name.
     */
    if(inst->kind == LowerInst::X86MinMax) return isIntVector(((LowerInstX86MinMax*)inst)->result.type);

    /*
     * A masked vector, where the mask is the arm that is *kept*.
     *
     * That one is `pand`/`andps`, which is commutative for the reason the bitwise three below are:
     * what it does is to bits. So the operand a load feeds may be moved into the position the
     * encoding dereferences, which is what turns `vmovups (%rdx),%ymm3 ; vandps` into one
     * instruction in a masked loop. The *complemented* one is `pandn`, which computes `~lhs & rhs`
     * and means two different things read the two ways round.
     */
    if(inst->kind == LowerInst::X86MaskAnd) return !((LowerInstX86MaskAnd*)inst)->isComplemented();

    /*
     * An equality, at every type it can be asked about.
     *
     * `a == b` is `b == a` whatever the operands are - the one relation for which that is true of a
     * float and of a NaN as well, both orders answering false - so the load may be exchanged into
     * the memory-capable position here too. That is what the AVX2 string and integer loops needed:
     * `vpcmpeqb` reads its second operand out of memory quite happily, and the comparison arrived
     * with the load on the *left*, so a separate `vmovdqu` was emitted in front of every one.
     *
     * `neq` rides along for the same reason. A packed one is the equality inverted rather than an
     * instruction of its own, and what is inverted is a mask that does not care which side is which.
     */
    if(inst->kind == LowerInst::Cmp) {
        auto relation = ((LowerInstCmp*)inst)->getCmp();
        return relation == LowerCmp::eq || relation == LowerCmp::neq;
    }

    if(!isBinary(inst) || !isCommutative(inst)) return false;
    auto type = ((LowerInstBinary*)inst)->result.type;

    /*
     * Which leaves the type, and the two answers differ.
     *
     * **The bitwise three exchange at every type whose bits they are** - a vector and a mask
     * included, and a *float* vector included, which is the one that matters here: an absolute value
     * is an `and` against a sign mask over `f32x8`, and with the operands as written the mask stands
     * where the encoding's address goes. Exchanging is what puts the value being measured there
     * instead, so the loop's own load folds and the loop-invariant mask keeps its register. A float
     * `and` is commutative in the way that matters, and in a way a float `add` is not: what these do
     * is to bits, so there is no rounding and no NaN payload to be taken from one side or the other.
     *
     * **The arithmetic exchanges at integer lanes alone.** A float add and a float multiply are
     * exchangeable in value, and this backend does not exchange them: `addps` takes the payload of a
     * NaN from its destination, so which operand is which is visible in a way it is not above.
     *
     * The high multiplies are the one pair this now admits that the list it replaces did not, and it
     * costs nothing: `mulhi` is commutative, but `mul r/m` reads one operand out of a fixed register,
     * so its memory twin is refused by `hasFixedOperands` in `tryFoldLoad` before this is ever asked.
     */
    if(hasLowerTrait(inst, kLowerBitwise)) return isIntLike(type) || isVectorLike(type);
    return isIntLike(type) || isIntVector(type);
}

// Whether this form requires an operand in a particular register, which is the copy a folded address
// cannot survive - see the third bound above.
static bool hasFixedOperands(const MachineForm& form) {
    for(auto& constraint: form.uses) {
        if(constraint.kind == OperandConstraintKind::FixedRegister) return true;
    }

    return false;
}

/*
 * Folds the load feeding operand `at` of the instruction at `index` into it. Answers where that
 * instruction ended up, or Nothing where it was left alone - in which case nothing has been changed
 * at all: the operand exchange a commutative operation may need is made at the end, with every
 * question already answered, so that a fold which does not happen leaves no trace of having been
 * considered. That is what lets the caller try one operand and then the other.
 */
static Maybe<Size> tryFoldLoadOperand(LowerBase base, LowerFunction& fun, LowerBlock* block,
    Size index, const MachineForm& twin, Size memory, Size at)
{
    auto inst = base[block->instructions.get(base, index)];
    auto used = inst->used();
    auto value = base[used[at]];
    auto load = (LowerInstLoad*)value->inst();

    /*
     * Where the load stands, and whether the stretch between it and here leaves memory alone.
     *
     * One walk answers both: upwards from the reader to the load, refusing at the first instruction
     * that could change what the load would read. A load in another block, or further up than the
     * bound, is one this declines to look for.
     */
    if(base[load->block] != block) return Nothing();

    auto loadAt = index;
    for(Size steps = 0; ; steps++) {
        if(loadAt == 0 || steps > kMaxLoadSinkDistance) return Nothing();

        loadAt--;
        auto above = base[block->instructions.get(base, loadAt)];
        if(above == (LowerInst*)load) break;
        if(writesStorage(above)) return Nothing();
    }

    // Whether this is a *sink* rather than the adjacent fold §3.1.2 started as, which is the one
    // thing below that has to be decided rather than assumed: a sunk load takes its address down
    // with it, and an address with another reader may not travel.
    auto sunk = loadAt != index - 1;

    // Which operand holds it has to be the one the encoding can dereference, or an operand a
    // commutative operation can exchange into it - which is the shape `arr[i] + sum` arrives in.
    auto exchange = at != memory;
    if(exchange && !(isCommutativeInt(inst) && used.size() == 2 && at < used.size())) return Nothing();

    /*
     * An operand the encoding was carrying as an immediate has nowhere to go.
     *
     * The memory twin names a register in the field the constant occupied, so the fold displaces it
     * into one - and materializing a constant is exactly the instruction the fold removed, at more
     * bytes. `%v = load %p ; add %v, 1` is `mov (%rdx),%edx ; inc %edx` and folds to
     * `mov $1,%ecx ; add (%rdx),%ecx`: the same two instructions, three bytes longer, and the
     * constant now holds a register across them.
     *
     * Unreachable while the load had to be adjacent - the `imm` that defines the constant stands
     * between the two - so this is a rule the sink needs and the adjacent fold never met.
     *
     * Asked of the twin rather than of the operand, because a great many twins carry both at once:
     * `cmp [m], $0` is one instruction and folding a load into it displaces nothing. Only a twin with
     * no immediate field at all has nowhere to put one. The operand's *kind* rather than `isImm`,
     * which reads a flag no pass has set this early - embedding an immediate is a decision
     * `selectMachineInstructions` makes below here.
     */
    if(twin.immediateWidth() == ImmediateWidth::None) {
        for(Size i = 0; i < used.size(); i++) {
            if(i != at && base[used[i]]->inst()->kind == LowerInst::Imm) return Nothing();
        }
    }

    // The bytes the encoding reads are the bytes the load read, unextended.
    if(load->getWidth() != accessWidthOf(value->type)) return Nothing();
    if(stackSlotClassFor(value->type) != stackSlotClassFor(operationType(base, twin, inst))) return Nothing();

    auto address = base[load->from];

    /*
     * A pooled constant, which becomes the *whole* address rather than something to build one from.
     *
     * This is the case the rip-relative form of `LowerInstX86Address` exists for. It is checked
     * before the two below because the answers differ: the address of a pooled constant is neither
     * a folded `X86Address` sitting two instructions up nor a pointer in a register, and left to
     * the general path the global would be committed to a register with a `lea` in front of it -
     * strictly worse than the load being folded.
     *
     * Any global nothing writes, not only a pooled constant: `mut` clear is a promise, derived from
     * `Global::isWritten` for a real program and written as `mut @g` in a `.lower` fixture. A global
     * that is written is left to the general path, where nothing is assumed about it.
     */
    auto pooledSymbol = LowerPtr<LowerGlobal>(nullptr);
    if(address->inst()->kind == LowerInst::Global) {
        auto target = ((LowerInstGlobal*)address->inst())->target;
        if(base[target]->mut) return Nothing();

        // The load is about to be the only reader gone; anything else reading the address still
        // needs it in a register, and this fold would leave that reader without a definition.
        if(address->uses.size() != 1) return Nothing();

        pooledSymbol = target;
    } else if(isMem(address)) {
        // Where the address fold put it: immediately in front of the load it serves. Checked rather
        // than assumed, an address anywhere else being one whose registers the instructions in
        // between could have written.
        if(loadAt == 0 || base[block->instructions.get(base, loadAt - 1)] != address->inst()) return Nothing();

        // A sunk load takes its address with it, so an address something else reads may not go: the
        // reader left behind would be reading a value defined below it. Where the two are adjacent
        // the address does not move and nothing is asked of its other readers.
        if(sunk && address->uses.size() != 1) return Nothing();
    } else if(isImplicit(address)) {
        // A pointer the encoding swallowed has no register for an address to be built around.
        return Nothing();
    }

    /*
     * Committed from here: everything below changes the function.
     */

    // Through the operand list rather than through `LowerInstBinary`'s two fields, because the kinds
    // this exchanges are no longer all binary: `X86MinMax` has its own struct, and what "exchange the
    // operands" means is the same for every two-operand instruction - the first two used values,
    // which is what the encoder and the form both read positionally.
    if(exchange) {
        auto operands = inst->used();
        auto first = operands[0];

        operands[0] = operands[1];
        operands[1] = first;
    }

    if(pooledSymbol) {
        auto producer = address->inst();
        auto computed = new (fun.arena) LowerInstX86Address(
            LowerInst::X86Address, StringId(), nullptr, nullptr, 1, 0
        );

        computed->symbol = pooledSymbol;

        replaceUse(base, value, inst, &computed->result);
        inst->used()[memory] = &computed->result - base;

        // The load first, because it is the address's last reader and the `global` that produced it
        // has nothing left to produce once it is gone - the symbol is in the encoding now. Both are
        // removed here rather than left to a dead-value sweep, because there is none between this
        // pass and allocation.
        removeInst(base, load);
        removeInst(base, producer);

        // Both removals were above the consumer and the address goes back immediately in front of
        // it, so where everything ended up is asked rather than counted: the `global` need not have
        // been adjacent to the load it fed.
        auto here = indexOfInst(base, block, inst);
        insertInstAt(base, block, here, computed);

        return Just(here + 1);
    }

    // The load goes first, wherever it stood: it is what the address was in front of, and taking it
    // out is what lets the address end up in front of the consumer instead.
    auto computedHere = !isMem(address);

    if(computedHere) {
        // A pointer that reached the load in a register becomes `[reg]`, so that the operand says
        // what it is without a flag beside it.
        address = &(new (fun.arena) LowerInstX86Address(
            LowerInst::X86Address, StringId(), address - base, nullptr, 1, 0
        ))->result;
    }

    replaceUse(base, value, inst, address);
    inst->used()[memory] = address - base;
    removeInst(base, load);

    // And the address is put immediately in front of the consumer. For an adjacent fold that is
    // where it already is - the load having been between them - so only a sink moves anything.
    if(computedHere) {
        insertInstAt(base, block, indexOfInst(base, block, inst), address->inst());
    } else if(sunk) {
        auto producer = address->inst();
        removeInst(base, producer);
        insertInstAt(base, block, indexOfInst(base, block, inst), producer);
    }

    return Just(indexOfInst(base, block, inst));
}

/*
 * Folds a load into the instruction at `index`, whichever of its operands one feeds.
 *
 * The operand the encoding dereferences is offered first and every other one after it, because the
 * two answers are not interchangeable: reaching another operand needs an exchange, which only a
 * commutative operation has, and the load feeding that one may be somewhere this cannot fold from at
 * all. `abs(v[i]) `is where that matters - the `and` reads the element and a mask loaded outside the
 * loop, the mask is in the r/m position, and stopping at the first operand that *looks* like a
 * candidate refused the whole fold and left the element's load in the loop.
 */
static Maybe<Size> tryFoldLoad(LowerBase base, LowerFunction& fun, LowerBlock* block, Size index) {
    if(index == 0) return Nothing();

    auto inst = base[block->instructions.get(base, index)];
    auto& form = machineTarget().form(selectForm(base, inst));

    // Nothing to fold into: either a form with no memory-capable operand, or one already on its
    // twin - a folded operand is one the form reads as an address, and there is one r/m field.
    if(!form.memorySource) return Nothing();

    auto& twin = machineTarget().form(form.memorySource);
    if(hasFixedOperands(twin)) return Nothing();

    auto memory = Size(form.memoryUse());
    auto used = inst->used();

    // A load with one reader, and that reader is this instruction. `add %v, %v` reads it in both
    // positions and only one of them can be the address, which a use count of one already excludes.
    auto candidate = [&](Size i) {
        auto operand = base[used[i]];
        if(operand->inst()->kind != LowerInst::Load) return false;

        return !isImplicit(operand) && operand->uses.size() == 1;
    };

    if(memory < used.size() && candidate(memory)) {
        if(auto folded = tryFoldLoadOperand(base, fun, block, index, twin, memory, memory)) return folded;
    }

    for(Size i = 0; i < used.size(); i++) {
        if(i == memory || !candidate(i)) continue;

        if(auto folded = tryFoldLoadOperand(base, fun, block, index, twin, memory, i)) return folded;
    }

    return Nothing();
}

void foldLoads(LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            // A fold removes the load above the instruction and may insert an address in its place,
            // so the instruction just examined does not stay where the walk left it. Nothing above
            // it changed shape, so carrying on from wherever it ended up skips nothing.
            if(auto folded = tryFoldLoad(base, fun, block, i)) i = folded.unwrap();
        }
    }
}

/*
 * §45.2 An accumulating write, folded into one memory-destination instruction.
 *
 * `out[i] = out[i] + x` is a load, an operation and a store, and on this machine it is one
 * instruction: the group-1 ALU writes its result back through the very r/m field it read its operand
 * from. What that removes from `Matrix`'s inner loop is two decoded instructions and, more to the
 * point, the *register* the loaded value occupied for the length of the operation - in the innermost
 * loop of every program that accumulates into an array.
 *
 *     mov  r11d, [out + i*4]           imul edx, left
 *     imul r15d, [b + i*4]      =>     add  [out + i*4], edx
 *     add  r11d, r15d
 *     mov  [out + i*4], r11d
 *
 * The rewrite is a *sink*: the load moves down to where the store is and the two become one access.
 * Four things have to hold for that, and each is checked rather than assumed.
 *
 *  - **One address, and it is the same value.** The load and the store name the same SSA pointer,
 *    which is what makes "the same location" a fact rather than an alias question. Two computations
 *    of one address would be that question, and the tier above has already unified them where they
 *    are unifiable.
 *  - **The load feeds the operation and the operation feeds the store, and nothing else reads
 *    either.** Otherwise the load has to be performed anyway and this adds an access rather than
 *    removing one.
 *  - **Nothing between them writes memory.** The load is being moved across whatever lies between
 *    it and the store, so anything that could change what it would read is a refusal.
 *    `writesStorage` answers yes for a call, so a call between the three is refused.
 *  - **The location is the left-hand side of a subtraction.** `[m] - v` is what the machine
 *    performs; `v - [m]` is a different number. The four commutative ones are folded from either
 *    side.
 *
 * The load may sit in the block *above* the store's, which is not an extension of the rule but the
 * shape a bounds-checked subscript has: `out[i]` is checked, and the check is a branch, so the load
 * and the store the program wrote as one line are on two sides of an edge. The condition there is
 * that the store's block has exactly one way in - then every path that reaches the store has just
 * run the tail of the load's block, and the two spans that have to be clean are that tail and the
 * head of the store's own block.
 */

// The five operations the machine can perform through its r/m field, which is the whole of what the
// form table describes - see OpStoreAdd and the block beside it in machine_forms_memory.cpp.
static bool isStoreUpdateOp(LowerInst* inst) {
    // Asked of the form table rather than restated here: which operations have an in-place memory
    // form is one fact, and it is the same one the opcode and the form selection read. See
    // StoreUpdateOp.
    return storeUpdateOpFor(inst->kind) != nullptr;
}

// Whether every instruction in `[from, to)` of this block leaves memory as it found it. The
// terminator is not one of them and is not asked about: a branch writes nothing, and the two spans
// this is asked about are a block's tail and another's head.
static bool spanCannotWrite(LowerBase base, LowerBlock* block, Size from, Size to) {
    auto list = block->instructions.contents(base);

    for(Size i = from; i < to && i < list.size(); i++) {
        if(writesStorage(base[list[i]])) return false;
    }

    return true;
}

// Whether nothing between the load and the store can change what the load would read. The load is
// either in the store's own block, or in the one block the store's block is entered from - see the
// header above for why that second case is the ordinary one rather than the exotic one.
static bool nothingWritesBetween(LowerBase base, LowerBlock* block, Size store, LowerInst* load) {
    auto from = base[load->block];

    if(from == block) {
        auto at = indexOfInst(base, block, load);
        return at < store && spanCannotWrite(base, block, at + 1, store);
    }

    if(block->incoming.size() != 1) return false;
    if(base[block->incoming.get(base, 0)] != from) return false;

    auto at = indexOfInst(base, from, load);
    return spanCannotWrite(base, from, at + 1, from->instructions.size())
        && spanCannotWrite(base, block, 0, store);
}

/*
 * ## What it costs, which is why a loop does not get one
 *
 * Measured, `add [m], r` is *slower* than the three instructions it replaces - on this machine, in
 * every loop it was put in:
 *
 * | loop | split | in place |
 * | --- | --- | --- |
 * | `out[i] += src[i]` through a handle (as the IR writes it) | 3.43 ms | 4.14 ms |
 * | the same with the base already in a register (as LLVM writes it) | 2.68 ms | 3.72 ms |
 * | `out[i] += 1`, where the fold also removes the register | 2.70 ms | 2.95 ms |
 *
 * Those are hand-written assembly loops differing in nothing else (test/bench/findings.md §45.2),
 * and `programs/Matrix.yana` agrees to within a point: 153.9 ms against 166.0 ms, the only program
 * in the corpus the fold reaches. Fewer instructions and more time. The read-modify-write is one
 * instruction whose store cannot leave the store buffer until its own load has returned, and a loop
 * that stores every iteration is limited by exactly that.
 *
 * So the fold is applied where a store is **not in a loop**, which is where the trade it makes is
 * the one it looked like: three decoded instructions and a register become one instruction in code
 * that runs once. That is most of what it reaches anyway - a teardown, a field updated on a path, a
 * counter bumped outside a loop - and it is the whole of the size win with none of the cost.
 */

// Folds the store at `index`, the operation feeding it and the load feeding that into one in-place
// update. Answers where the update ended up, or Nothing - in which case nothing has been changed at
// all, every question being asked before the first rewrite.
static Maybe<Size> tryFoldStoreUpdate(LowerBase base, LowerFunction& fun, LowerBlock* block, Size index) {
    auto inst = base[block->instructions.get(base, index)];
    if(inst->kind != LowerInst::Store) return Nothing();

    auto store = (LowerInstStore*)inst;
    auto stored = base[store->value];
    auto op = stored->inst();

    // The operation, read where the store reads it: in this block, with this store as its one
    // reader. Anything else and the operation stands anyway, and this adds an access to it.
    if(!isStoreUpdateOp(op) || isImplicit(stored)) return Nothing();
    if(stored->uses.size() != 1 || base[op->block] != block) return Nothing();

    auto binary = (LowerInstBinary*)op;
    if(!isIntLike(binary->result.type)) return Nothing();

    auto lhs = base[binary->lhs];
    auto rhs = base[binary->rhs];

    // Which side the location is. A subtraction has one answer and the other four have two, and
    // `lhs == rhs` is `x + x` - one value read twice, which is not an update of anything.
    if(lhs == rhs) return Nothing();

    auto from = lhs;
    auto with = rhs;

    if(lhs->inst()->kind != LowerInst::Load) {
        if(op->kind == LowerInst::Sub) return Nothing();

        from = rhs;
        with = lhs;
    }

    if(from->inst()->kind != LowerInst::Load || isImplicit(from)) return Nothing();
    if(from->uses.size() != 1) return Nothing();

    auto load = (LowerInstLoad*)from->inst();

    // The same location, and the same bytes of it. An overread is a load of more than it says and
    // has no in-place operation to be: what would be written back is the width, and what was read
    // is not.
    if(base[load->from] != base[store->to]) return Nothing();
    if(load->getWidth() != store->getWidth() || load->isOverread()) return Nothing();

    /*
     * A constant right-hand side at a width the immediate forms do not cover.
     *
     * The two of them are the group-1 `imm8`/`imm32` pair and sit at four bytes and eight, which is
     * where that pair is; the byte and word updates are register-only. `canEmbedImm` asks the
     * *opcode* whether a constant can be embedded and would be answered yes by those two forms, so a
     * narrow update of a constant would have its operand taken out of allocation and then selected
     * into a form with nowhere to read it. Refused here rather than answered there, since the
     * question that pass asks has one answer per opcode.
     */
    if(with->inst()->kind == LowerInst::Imm && store->getWidth() < 4) return Nothing();

    if(!nothingWritesBetween(base, block, index, load)) return Nothing();

    /*
     * Committed: everything below changes the function.
     */
    auto update = new (fun.arena) LowerInstX86StoreOp(
        store->to, with - base, store->getWidth(), op->kind
    );

    insertInstAt(base, block, index, update);

    // In this order: the store is the operation's only reader and the operation is the load's, so
    // each is dead only once the one below it has gone. There is no dead-value sweep between here
    // and allocation - an instruction nothing reads is an instruction that gets emitted.
    removeInst(base, store);
    removeInst(base, op);
    removeInst(base, load);

    return Just(indexOfInst(base, block, update));
}

/*
 * `movbe` - the byte reversal folded into the access that was going to be reversed.
 *
 *     mov  eax, [rdi]                  movbe eax, [rdi]
 *     bswap eax                 =>
 *
 * Two rewrites, one per direction, and both are the same trade `foldLoads` makes everywhere else:
 * an instruction and a *register* go away, the register being the one that held the value the wrong
 * way round for the length of the reversal. What makes this a pass rather than a memory twin of the
 * `bswap` form is that `bswap` has no memory encoding at all - `movbe` is a different opcode, in a
 * different map, with the destination in ModRM.reg and no tie - so there is nothing for the twin
 * derivation to derive.
 *
 * Three conditions on the load side:
 *
 *  - **The load has one reader and it is this reversal.** Otherwise the load stands anyway and this
 *    adds an access rather than removing one.
 *  - **The access is the whole register.** A narrow load *extends* into its destination, and what
 *    `movbe` reverses is the operand size - so a two-byte load feeding a 32-bit reversal is four
 *    bytes reversed rather than two, which is a different number. (No 16-bit reversal reaches this
 *    backend at all - see `Value::ByteSwap` - so what this refuses is a program that loaded fewer
 *    bytes than it reversed, which the reversal's own operand type is the record of.)
 *  - **An overread load is refused**, its width being a promise about the access rather than about
 *    the value - see LowerInstLoad::isOverread.
 *
 * What is deliberately *not* a condition is anything about what lies between the two, and that is
 * because of where the access is put: the `movbe` goes where the **load** was, not where the
 * reversal was. Nothing about the memory access moves, so no question about intervening writes
 * arises - and, more to the point, the address the load reads stays exactly where it is. §3.1.2's
 * sink has to move an `X86Address` down with the load it serves, and refuse the move when something
 * else reads it, precisely because the registers the address is built from could be written by what
 * lies between; folding upward asks none of that. What moves instead is the *reversal*, which is
 * pure and whose one operand is defined immediately above it.
 *
 * The store side is the mirror: the `movbe` goes where the store was and the reversal above it is
 * deleted, its operand dominating the store it now feeds directly.
 *
 * **The feature test is here**, and it is the whole of why this is a pass and not a form selected on
 * the way past: `0f 38 f0` is not an encoding at all below x86-64-v3, so a target that does not have
 * it has to keep the two instructions it was given rather than have a form refused later.
 */
static Maybe<Size> tryFoldByteSwapLoad(LowerBase base, LowerFunction& fun, LowerBlock* block, Size index) {
    auto inst = base[block->instructions.get(base, index)];
    if(inst->kind != LowerInst::Bswap) return Nothing();

    auto reversal = (LowerInstUnary*)inst;
    auto source = base[reversal->from];

    if(source->inst()->kind != LowerInst::Load || isImplicit(source)) return Nothing();
    if(source->uses.size() != 1) return Nothing();

    auto load = (LowerInstLoad*)source->inst();
    auto width = is64Bit(reversal->result.type) ? 8u : 4u;

    // In this block, since that is where the access is put - see the header. A load in a block above
    // is left alone rather than reached into, which costs the fold nothing that has been measured: a
    // reversal is written beside the access it reverses.
    if(base[load->block] != block) return Nothing();
    if(load->getWidth() != width || load->isOverread()) return Nothing();

    /*
     * Committed: everything below changes the function.
     */
    auto movbe = new (fun.arena) LowerInstX86MovbeLoad(
        load->from, reversal->result.name, reversal->result.type, width
    );

    insertInstAt(base, block, indexOfInst(base, block, load), movbe);
    replaceUses(base, fun.arena, inst->created().ptr - base, movbe->created().ptr - base);

    // The reversal first: it is the load's only reader, so the load is dead only once it has gone.
    // Nothing sweeps dead values between here and allocation - an instruction nothing reads is an
    // instruction that gets emitted.
    removeInst(base, inst);
    removeInst(base, load);

    // Where the access ended up, which is *above* where the walk was: the instructions between it
    // and the reversal it replaced are examined a second time, which each of them answers the same
    // way. Every fold removes two instructions and adds one, so there is nothing here to circle in.
    return Just(indexOfInst(base, block, movbe));
}

static Maybe<Size> tryFoldByteSwapStore(LowerBase base, LowerFunction& fun, LowerBlock* block, Size index) {
    auto inst = base[block->instructions.get(base, index)];
    if(inst->kind != LowerInst::Store) return Nothing();

    auto store = (LowerInstStore*)inst;
    auto stored = base[store->value];
    auto reversal = stored->inst();

    if(reversal->kind != LowerInst::Bswap || isImplicit(stored)) return Nothing();
    if(stored->uses.size() != 1 || base[reversal->block] != block) return Nothing();
    if(store->getWidth() != (is64Bit(stored->type) ? 8u : 4u)) return Nothing();

    /*
     * Committed: everything below changes the function.
     */
    auto movbe = new (fun.arena) LowerInstX86MovbeStore(
        store->to, ((LowerInstUnary*)reversal)->from, store->getWidth()
    );

    insertInstAt(base, block, index, movbe);

    removeInst(base, store);
    removeInst(base, reversal);

    return Just(indexOfInst(base, block, movbe));
}

void selectByteSwapMemory(LowerBase base, LowerFunction& fun) {
    if((kFeatureMovbe & ~targetFeatures()) != 0) return;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            // Each fold removes an instruction that stood above the one it replaced, so where the
            // access ended up is asked rather than counted - the same reason `foldStoreUpdates` asks.
            if(auto folded = tryFoldByteSwapLoad(base, fun, block, i)) { i = folded.unwrap(); continue; }
            if(auto folded = tryFoldByteSwapStore(base, fun, block, i)) i = folded.unwrap();
        }
    }
}

void foldStoreUpdates(LowerBase base, LowerFunction& fun) {
    // Which blocks are in a loop, which is the whole of what decides where this fires - see the
    // table above. Built once for the function; nothing here creates or renumbers a block, so the
    // indexes it is read by stay the ones it was built from.
    auto loops = fun.buildLoops(base);

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];
        if(loops.depth[block->index] > 0) continue;

        for(Size i = 0; i < block->instructions.size(); i++) {
            // The fold removes two instructions that may both be above the store, so where the
            // update ended up is asked rather than counted. Everything it consumed was at or before
            // that point, and the update itself is not a candidate.
            if(auto folded = tryFoldStoreUpdate(base, fun, block, i)) i = folded.unwrap();
        }
    }
}
