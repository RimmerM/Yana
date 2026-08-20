#include "transform_internal.h"

/*
 * Constants: the pool, and the ones that need no pool.
 *
 * A floating-point constant and a vector constant both become a read-only global read through
 * `[rip + k]`, which is what `pooledConstant` and `pooledBytes` intern. What is here beside them is
 * every recognition that keeps a constant *out* of the pool - a splat of zero or all-ones, a mask
 * two constants have already decided, a select against zero - and the sinking that keeps one that
 * does get pooled from being live across a call.
 */

/*
 * A comparison and the select that reads it, recognized as one minimum or maximum.
 *
 * `min` and `max` have no instruction in the portable IR - `emitMinMax` in resolve/simd.cpp writes
 * them as `select(a < b, a, b)`, which is what a target without a packed minimum needs anyway and
 * what LLVM's own selection folds back. x86 has the instruction at every lane width but the
 * quadword, so this is where the pair becomes one: three instructions (a compare, a blend, and the
 * mask register between them) down to `vpmaxsd`, and the operand it reads may then come out of
 * memory, which a blend's could not.
 *
 * ## The two shapes, and why one of them exchanges the operands
 *
 * `select(a REL b, a, b)` is the shape the library and the reduction tree both build, and it maps
 * straight across: a `lt` keeps the left operand where it is smaller, which is a minimum with the
 * operands in that order. The mirror `select(a REL b, b, a)` is the same operation with the
 * comparison read the other way round - `a < b ? b : a` is `max(b, a)` - so it is recognized as the
 * opposite kind with the operands exchanged rather than declined.
 *
 * **The order survives the exchange, and that is the whole of what makes this exact at a float
 * lane.** `minps a, b` answers `b` whenever the comparison is false, which is what a NaN in either
 * operand produces and what `-0.0` against `+0.0` produces; so it is `select(a < b, a, b)` bit for
 * bit, and it is *not* `select(b > a, ...)` with the operands left where they were.
 *
 * ## What is declined
 *
 * A non-strict relation at a float lane. `select(a <= b, a, b)` and `minps a, b` differ at the pair
 * `(+0.0, -0.0)` - the comparison holds, so the select answers `+0.0` where the instruction answers
 * `-0.0` - and nothing in the language says a program may not have written it. An integer lane has
 * no such pair and takes `le` and `lt` alike.
 *
 * A quadword integer lane, which has no `pminsq` before AVX-512 (see the form table), and a mask the
 * select reads that anything else reads too: the comparison would then stay and this would be an
 * instruction added rather than two replaced.
 */

// The same comparison with its operands exchanged: `a < b` is `b > a`. Equality and the unordered
// tests are their own mirrors and are not relations this recognizes anyway.
static LowerCmp mirroredCmp(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::lt:  return LowerCmp::gt;
        case LowerCmp::le:  return LowerCmp::ge;
        case LowerCmp::gt:  return LowerCmp::lt;
        case LowerCmp::ge:  return LowerCmp::le;
        case LowerCmp::ilt: return LowerCmp::igt;
        case LowerCmp::ile: return LowerCmp::ige;
        case LowerCmp::igt: return LowerCmp::ilt;
        case LowerCmp::ige: return LowerCmp::ile;
        default:            return cmp;
    }
}

// Which minimum or maximum `select(a REL b, a, b)` is, or nothing where the relation is not an
// ordering this machine has an instruction for at this lane kind.
static Maybe<LowerMinMax> minMaxForRelation(LowerCmp cmp, bool isFloat) {
    switch(cmp) {
        case LowerCmp::lt:  return Just(LowerMinMax::Min);
        case LowerCmp::gt:  return Just(LowerMinMax::Max);

        // The signed pair, which a float lane can never state - `signedOperand` answers a lane's
        // signedness and a float lane is neither - and the non-strict pair, which is exact for an
        // integer lane and not for a float one. See the note above.
        case LowerCmp::ilt: return isFloat ? Nothing() : Just(LowerMinMax::IMin);
        case LowerCmp::igt: return isFloat ? Nothing() : Just(LowerMinMax::IMax);
        case LowerCmp::le:  return isFloat ? Nothing() : Just(LowerMinMax::Min);
        case LowerCmp::ge:  return isFloat ? Nothing() : Just(LowerMinMax::Max);
        case LowerCmp::ile: return isFloat ? Nothing() : Just(LowerMinMax::IMin);
        case LowerCmp::ige: return isFloat ? Nothing() : Just(LowerMinMax::IMax);
        default:            return Nothing();
    }
}

// Answers the minimum or maximum this select performs, with `lhs` and `rhs` set to the operands in
// the order the machine reads them - or nothing where this select is not one.
static Maybe<LowerMinMax> matchPackedMinMax(LowerBase base, LowerInstSelect* select,
                                            LowerValue*& lhs, LowerValue*& rhs) {
    auto type = select->result.type;
    if(!packedMinMaxSupported(type)) return Nothing();

    auto condition = base[select->cmp];
    if(condition->inst()->kind != LowerInst::Cmp) return Nothing();

    // The comparison has to die with the select, or this replaces two instructions with two and
    // leaves the mask being computed for one reader that no longer wants it.
    if(condition->uses.size() != 1) return Nothing();

    auto cmp = (LowerInstCmp*)condition->inst();
    auto a = base[cmp->lhs];
    auto b = base[cmp->rhs];
    // `lhs` is the value taken where the mask is set and `rhs` the other, which is the order both
    // the machine form and the encoder read a select in.
    auto whenTrue = base[select->lhs];
    auto whenFalse = base[select->rhs];
    auto relation = cmp->getCmp();

    if(whenTrue == a && whenFalse == b) {
        lhs = a;
        rhs = b;
    } else if(whenTrue == b && whenFalse == a) {
        // The mirror: `a < b ? b : a` is `max(b, a)`, which is this relation read from the other
        // side with the operands in the order the select already names them.
        lhs = b;
        rhs = a;
        relation = mirroredCmp(relation);
    } else {
        return Nothing();
    }

    return minMaxForRelation(relation, isFloatVector(type));
}

void selectPackedMinMax(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // The comparisons this leaves with no readers, cleared after the walk rather than during it:
        // one of them stands immediately *above* the select being rewritten, and removing it there
        // would renumber the instructions this loop is indexing.
        InstChain dead;

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Select) continue;

            auto select = (LowerInstSelect*)inst;
            LowerValue* lhs = nullptr;
            LowerValue* rhs = nullptr;

            auto kind = matchPackedMinMax(base, select, lhs, rhs);
            if(!kind) continue;

            auto comparison = base[select->cmp]->inst();
            auto minMax = new (fun.arena) LowerInstX86MinMax(
                select->result.name, select->result.type, lhs - base, rhs - base, kind.unwrap()
            );

            insertInstAt(base, block, i, minMax);
            replaceAllUses(base, &select->result, &minMax->result);
            removeInst(base, select);

            // The comparison, whose one use has just gone. It has to go too: there is no dead-code
            // elimination below this point, so an instruction nothing reads is one that gets emitted.
            dead.push(comparison);
        }

        removeDeadChain(base, dead);
    }
}

/*
 * The constant pool: one read-only global per distinct floating-point constant.
 *
 * No SSE encoding carries a float as an immediate, so this backend materialized one in a general
 * register and moved it across the bank boundary - ten or eleven bytes, a general register the form
 * had to declare as a clobber, and a value the allocator could never rematerialize. `[rip + k]` is
 * the answer every other x86-64 toolchain gives, and it is eight bytes, no general register at all,
 * and a load the hardware has a whole cache for.
 *
 * §0.2 of Implementation-Vector.md asks for it as a *prerequisite* rather than as an optimization,
 * and that is the part worth writing down: a vector constant cannot be materialized the old way at
 * all. Sixteen bytes do not fit a general register, so there is no register to move across - which
 * makes the pool the thing that has to exist before a single vector literal can be emitted.
 *
 * ## One global per constant rather than one pool with offsets
 *
 * Because a relocation names a global and carries no addend, so an offset into a shared pool would
 * need a field on `AsmRelocation` and a second way of resolving one. A global of its own costs the
 * padding `addGlobal` puts in front of it - up to twelve bytes for a `Float32` - and buys 16-byte
 * alignment on every entry, which is what a vector load will require anyway.
 *
 * The interning is `LowerModule::globals` itself: the name *is* the bit pattern, so two functions
 * that mention `1.0` reach one global without this pass holding a map of its own.
 *
 * ## Where it sits
 *
 * Before `selectMemorySources`, so that `foldLoads` *does* see the loads this creates: a constant
 * read once by the instruction below it becomes `addsd xmm, [rip + k]` rather than a load into a
 * register and an add of it. That fold needs the rip-relative form of `LowerInstX86Address`, which
 * is why this pass ran after it until that existed - without a symbol field the global would have
 * been committed to a register with a `lea` in front of it, worse than the load it replaced.
 *
 * Before `selectMachineInstructions`, because `tryFoldGlobalAddress` is what turns the address of a
 * constant that was *not* folded into the addressing mode of its own load. Without that sweep the
 * global would be a `lea` of its own.
 *
 * After `selectAddressesAndLeas`, which has nothing to say about either.
 */
LowerGlobal* pooledConstant(Context& ctx, LowerModule& module, U64 bits, Size size) {
    // `$f032$0000000000000001`. Written out rather than formatted so that the name is exactly the
    // bit pattern at a fixed width - two constants of different widths that happen to share a
    // pattern are two entries, and neither can be a prefix of the other.
    static const char digits[] = "0123456789abcdef";
    char text[] = "$f000$0000000000000000";
    auto width = size * 8;

    text[2] = digits[(width / 100) % 10];
    text[3] = digits[(width / 10) % 10];
    text[4] = digits[width % 10];
    for(Size i = 0; i < 16; i++) text[21 - i] = digits[(bits >> (i * 4)) & 0xf];

    // The hash rather than the interning call, because the interning has to happen exactly once -
    // see below - and this is the same number `addUnqualifiedName` would answer with.
    auto length = sizeof(text) - 1;
    auto name = Context::nameHash(text, length);

    auto entry = module.globals.add(name);
    if(entry.existed) return (*module.arena)[*entry.value];

    /*
     * **`addUnqualifiedName` keeps the pointer it is handed rather than a copy of it**, which
     * `addQualifiedName` beside it does not - so a name built on the stack is a dangling one the
     * moment this returns, and what a dump or an ELF symbol table prints is whatever is there now.
     * That is not a crash: the five constants of `Float.yana` all appeared in `readelf` under one
     * four-byte name made of whatever the next call left on the stack.
     *
     * So the text is copied into the arena that outlives the compilation, and only on the branch
     * where the name is new - a repeat would intern to the same id and leave the copy unread.
     */
    auto stored = (char*)module.arena.alloc(length);
    copyMem(text, stored, length);
    ctx.addUnqualifiedName(stored, length);

    auto global = new (module.arena) LowerGlobal(name);
    auto contents = (U8*)module.arena.alloc(size);

    // Repeated to fill the entry, which is what makes a sixteen-byte one the *broadcast* of its
    // pattern: the sign mask a negation exclusive-ors against has to hold the bit in every lane it
    // might reach, and a vector constant will want the same shape for the same reason.
    for(Size at = 0; at < size; at += 8) copyMem(&bits, contents + at, min(Size(8), size - at));

    global->initialContents = { contents, size };
    *entry.value = global - *module.arena;
    module.globalOrder.push(global - *module.arena);

    return global;
}

/*
 * The same pool, entered with the bytes already laid out - which is what a vector constant needs and
 * `pooledConstant` above cannot express, since it takes one word and repeats it.
 *
 * The name is the whole pattern in hex, so the interning `LowerModule::globals` already performs is
 * still exact: two entries collide only if every byte agrees, and a 16-byte constant's name cannot be
 * a prefix of a 32-byte one's because the width is in the name ahead of the bytes.
 */
static LowerGlobal* pooledBytes(Context& ctx, LowerModule& module, const U8* bytes, Size size) {
    static const char digits[] = "0123456789abcdef";

    // `$v128$<2 * size hex digits>`. Sized for the widest constant this language admits.
    char text[6 + 2 * kMaxVectorBytes];
    auto width = size * 8;

    text[0] = '$';
    text[1] = 'v';
    text[2] = digits[(width / 100) % 10];
    text[3] = digits[(width / 10) % 10];
    text[4] = digits[width % 10];
    text[5] = '$';

    for(Size i = 0; i < size; i++) {
        text[6 + i * 2] = digits[bytes[i] >> 4];
        text[7 + i * 2] = digits[bytes[i] & 0xf];
    }

    auto length = 6 + size * 2;
    auto name = Context::nameHash(text, length);

    auto entry = module.globals.add(name);
    if(entry.existed) return (*module.arena)[*entry.value];

    // Copied into the arena for the reason `pooledConstant` states: `addUnqualifiedName` keeps the
    // pointer it is handed, and this one is on the stack.
    auto stored = (char*)module.arena.alloc(length);
    copyMem(text, stored, length);
    ctx.addUnqualifiedName(stored, length);

    auto global = new (module.arena) LowerGlobal(name);
    auto contents = (U8*)module.arena.alloc(size);
    copyMem(bytes, contents, size);

    global->initialContents = { contents, size };
    *entry.value = global - *module.arena;
    module.globalOrder.push(global - *module.arena);

    return global;
}

/*
 * The bytes a value holds if it is a constant vector, and whether it is one.
 *
 * A constant vector in this IR is not a constant - §9.7 of Implementation-Vector.md records that
 * there is deliberately no vector constant form, because a lane pattern is not an immediate on any
 * of these machines. What there is instead is the shape the resolver builds: a `vsplat` of a
 * constant, and a chain of `vwithlane`s over it whose values are constants. `iota` is exactly that,
 * and so is every `splat` of a literal.
 *
 * So this is the folder's job done at the one place that can act on the answer. Reading the chain
 * here rather than folding it into a constant earlier is what keeps the IR's own claim true.
 */
bool constantVectorBytes(LowerBase base, LowerValue* value, U8* bytes, Size size,
                                InstChain& chain) {
    auto inst = value->inst();
    auto type = value->type;
    auto lane = laneBytes(type.lane);

    // A lane's bits at its own width, from an immediate of the lane's *scalar* form - which for an
    // 8- or 16-bit lane is an Int32, and for a Float32 lane is held as a double until here.
    auto laneBits = [&](LowerValue* from, U8* at) {
        auto source = from->inst();
        if(source->kind != LowerInst::Imm) return false;

        auto imm = (LowerImm*)source;

        if(type.lane == LowerLane::Float32) {
            auto narrow = float(imm->f);
            copyMem(&narrow, at, 4);
            return true;
        }

        if(type.lane == LowerLane::Float64) {
            auto wide = imm->f;
            copyMem(&wide, at, 8);
            return true;
        }

        auto integer = imm->i;
        copyMem(&integer, at, lane);
        return true;
    };

    if(inst->kind == LowerInst::VecSplat) {
        auto splat = (LowerInstVecSplat*)inst;
        auto from = base[splat->from];
        if(!laneBits(from, bytes)) return false;

        for(Size at = lane; at < size; at += lane) copyMem(bytes, bytes + at, lane);

        chain.push(inst);
        chain.push(from->inst());
        return true;
    }

    /*
     * A vector read as another vector of the same width, which changes nothing about the bytes.
     *
     * This is how a constant reaches the *other* lane kind. `expandVectorAbs` builds its mask as an
     * integer splat - a float lane's immediate is held as a double and narrowed, which cannot state
     * a NaN's payload exactly, and `0x7fffffff` is a NaN - and then reads it as the float vector the
     * `andps` wants. Without this the chain would stop at the bitcast, the *inner* splat would be
     * pooled on its own, and what the `and` read would be a bitcast of a load rather than a load:
     * one instruction more, and the fold that puts the constant in the addressing mode gone.
     */
    if(inst->kind == LowerInst::Bitcast) {
        auto from = base[((LowerInstUnary*)inst)->from];
        if(!isVectorLike(from->type) || from->type.byteWidth() != type.byteWidth()) return false;
        if(!constantVectorBytes(base, from, bytes, size, chain)) return false;

        chain.push(inst);
        return true;
    }

    if(inst->kind == LowerInst::VecWithLane) {
        auto write = (LowerInstVecLane*)inst;
        if(!constantVectorBytes(base, base[write->from], bytes, size, chain)) return false;

        auto from = base[write->value];
        if(!laneBits(from, bytes + Size(write->getLane()) * lane)) return false;

        chain.push(inst);
        chain.push(from->inst());
        return true;
    }

    /*
     * A constant already in the image, read back.
     *
     * This is what makes the two arms below work at all rather than half the time. `poolVectorConstants`
     * walks a block forwards, so a splat that has readers of its own becomes a `.rodata` load *before*
     * the shuffle of it is looked at - and a shuffle whose source is a load is not a chain this could
     * see through until now. The multiplier of a strength-reduced division is exactly that shape: the
     * value is needed and so is a rearrangement of it.
     *
     * Only a global that is not written and holds at least the bytes being read. A mutable one says
     * nothing about what is there at any moment, which is the same rule `tryFoldLoad` holds a global
     * to before folding a load of one into an addressing mode.
     */
    if(inst->kind == LowerInst::Load) {
        auto load = (LowerInstLoad*)inst;
        if(load->getWidth() != size) return false;

        auto address = base[load->from]->inst();
        if(address->kind != LowerInst::Global) return false;

        auto global = base[((LowerInstGlobal*)address)->target];
        if(!global || global->mut || global->initialContents.size() < size) return false;

        copyMem(global->initialContents.data(), bytes, size);

        chain.push(inst);
        chain.push(address);
        return true;
    }

    /*
     * A constant's lanes rearranged, which is a different constant and not an instruction.
     *
     * This is what an *expansion* leaves behind. `expandVectorMulHi` brings the high limb of its
     * multiplier into reach with a `pshufd`, and the multiplier is a pooled constant - so without
     * this the shuffle stands in the loop for ever, there being no code motion below this point to
     * lift it out. Reading through it puts the rearranged pattern in the image instead and leaves
     * *nothing* in the loop, which is better than hoisting it would have been.
     *
     * The pattern indexes the two sources end to end, so both have to be constant. A shuffle of one
     * value with itself is the ordinary case and is read once rather than twice - not for speed, but
     * because the halves would otherwise be two names for one buffer.
     */
    if(inst->kind == LowerInst::VecShuffle) {
        auto shuffle = (LowerInstVecShuffle*)inst;
        auto left = base[shuffle->left];
        auto right = base[shuffle->right];

        if(left->type != type || right->type != type) return false;
        if(size > kMaxVectorBytes) return false;

        U8 sources[2 * kMaxVectorBytes] = {};
        if(!constantVectorBytes(base, left, sources, size, chain)) return false;

        if(left == right) copyMem(sources, sources + size, size);
        else if(!constantVectorBytes(base, right, sources + size, size, chain)) return false;

        auto pattern = shuffle->pattern();
        for(Size j = 0; j < pattern.length; j++) {
            copyMem(sources + Size(pattern[j]) * lane, bytes + j * lane, lane);
        }

        chain.push(inst);
        return true;
    }

    /*
     * A constant shifted by a count every lane shares, which is a different constant for the same
     * reason - and is the other half of what an expansion leaves: the sign mask a signed
     * multiply-high needs is a `psrad` of the multiplier's high dwords.
     *
     * The count has to be a scalar immediate, which is what `unwrapVectorShiftCounts` leaves and what
     * the machine's immediate rows take. A count per lane is a whole vector and is not this.
     *
     * **A count at or past the lane's width saturates**, which is what the packed shifts do -
     * `pslld xmm, 0xff` is zero and `psrad` fills with the sign - and is not what the scalar rule
     * would say. This is folding a *packed* shift, so it answers what the instruction it replaces
     * would have.
     */
    if(inst->kind == LowerInst::Shl || inst->kind == LowerInst::Shr || inst->kind == LowerInst::Sar) {
        auto shift = (LowerInstBinary*)inst;
        auto count = base[shift->rhs];

        if(isVectorLike(count->type) || count->inst()->kind != LowerInst::Imm) return false;
        if(!isIntVector(type)) return false;
        if(!constantVectorBytes(base, base[shift->lhs], bytes, size, chain)) return false;

        auto bits = U32(lane) * 8;
        auto by = ((LowerImm*)count->inst())->i;
        auto arithmetic = inst->kind == LowerInst::Sar;
        auto spare = 64 - bits;

        if(by >= bits) by = arithmetic ? bits - 1 : bits;

        for(Size at = 0; at < size; at += lane) {
            U64 held = 0;
            copyMem(bytes + at, &held, lane);

            U64 shifted;
            if(by >= bits) shifted = 0;
            else if(inst->kind == LowerInst::Shl) shifted = held << by;
            else if(arithmetic) shifted = U64((I64(held << spare) >> spare) >> by);
            else shifted = held >> by;

            copyMem(&shifted, bytes + at, lane);
        }

        chain.push(inst);
        chain.push(count->inst());
        return true;
    }

    return false;
}

/*
 * Whether this value is a constant vector, asked from the other side of the pass that pools it.
 *
 * `checkVectorSupported` runs at the top of `transformFunction` and refuses what this backend has no
 * form for - and a *lane write* of an 8-bit lane is one of those, since `pinsrw` writes a word and
 * half of one would have to be read back out first. But nothing emits the lane writes of a constant
 * chain: `poolVectorConstants` below replaces the whole chain with a `.rodata` load and removes
 * every link of it, so refusing one is refusing an instruction that will not exist.
 *
 * That is not a hypothetical case, it is `iota` - which `maskUpTo` and `firstSet` are both written
 * over, so every masked tail of every byte-lane loop is exactly this shape. Asked here rather than
 * approximated in machine_vector.cpp, on `packedCompareRelation`'s argument: two readers on opposite sides
 * of a pass have to ask one function or they will drift.
 */
bool isPooledVectorConstant(LowerBase base, LowerValue* value) {
    auto type = value->type;
    if(!type.isVector() && !type.isMask()) return false;

    U8 bytes[kMaxVectorBytes] = {};
    InstChain chain;

    return constantVectorBytes(base, value, bytes, type.byteWidth(), chain);
}

/*
 * The chain that fed the constant, removed.
 *
 * Nothing below this pass is a dead-code elimination - the IR optimizer ran long ago and what is
 * left here is selection - so a link whose only reader was the next link stays in the function and
 * is emitted. Left to itself the pass made `iota` *longer*: the load appeared and the six
 * instructions it replaced were still there.
 *
 * To a fixpoint rather than in one sweep, and that is not caution. A link and the immediate it reads
 * die in that order, so any single ordering leaves half of them standing: walked outermost-first the
 * immediates are still used when they are looked at, and walked innermost-first the lane writes are.
 * The chain is at most two entries per lane, so repeating until nothing moves is bounded by its own
 * length and is what makes "the constant leaves nothing behind" true rather than nearly.
 *
 * Each is removed only once its own use list is empty, which is what keeps a constant that two
 * chains share, or that something else reads, exactly where it is.
 */
void removeDeadChain(LowerBase base, InstChain& chain) {
    for(Size round = 0; round <= chain.size(); round++) {
        auto moved = false;

        for(Size i = chain.size(); i > 0; i--) {
            auto inst = chain[i - 1];
            if(!inst || inst->createdCount != 1) continue;

            auto result = &((LowerInstSingle*)inst)->result;
            if(!result->uses.isEmpty()) continue;

            removeInst(base, inst);

            /*
             * Cleared so that a second round does not remove it again - `inst` is the outermost link
             * and has already been removed by the caller, so this list may hold one either way.
             *
             * **Every entry holding it, not only this one.** A list built from more than one rewrite
             * holds the shared links twice - two absolute values over one hoisted zero, two masked
             * selects over one `iota` - and the second entry would otherwise be an instruction with
             * an empty use list that is no longer in any block, which `removeInst` reports as the
             * structural error it would be anywhere else.
             */
            for(Size c = 0; c < chain.size(); c++) if(chain[c] == inst) chain[c] = nullptr;
            moved = true;
        }

        if(!moved) break;
    }
}

/*
 * A mask the two constants either side of a comparison have already decided, and the two things that
 * read one.
 *
 * This is the tail mask of every bulk operation. `maximumVectors` (resolve/core.cpp) is written as
 *
 *     acc = max(acc, select(maskUpTo(live) :: Mask(a), v, acc))
 *
 * and `occurrencesVectors` beside it as `count(m .& maskUpTo(live))`, so that the last chunk
 * contributes only its live lanes - and the *full* chunks go through the identical line with `live`
 * equal to the lane count. `maskUpTo(n)` is `iota .< splat(n)`, both of whose operands are constant
 * vectors in the full-chunk loop, so the mask is all-ones: the select is its own first arm, and the
 * `and` is its other operand. Left standing the first was a `vpcmpgtd` hoisted out of the loop, a
 * register held for its result across the whole loop, and a `vpblendvb` per chunk that answered its
 * second operand every time; the second was that same hoisted comparison and a `vpand` per chunk
 * that changed nothing.
 *
 * Removing it is worth more than the blend, and that is the reason this pass exists rather than the
 * one instruction it deletes: what the blend stood between was the *load* and the operation that
 * reads it. `vmovdqu (%rdx),%ymm3 ; vpblendvb ; vpmaxsd %ymm3,%ymm0,%ymm0` is three instructions and
 * a register where `vpmaxsd (%rdx),%ymm0,%ymm0` is one - a blend takes three registers and can never
 * be the thing a load folds into, so the fold below is what lets `tryFoldLoad` see the pair at all.
 *
 * ## Why the comparison rather than the mask
 *
 * A mask has no constant form in this IR and is not going to get one: `constantVectorBytes` reads a
 * `vsplat`/`vwithlane` chain of immediates, and a mask lane's immediate is a truth value rather than
 * the all-ones pattern the machine holds - so "the bytes of a constant mask" is a question with two
 * plausible answers and no reader that needs it. The comparison has no such ambiguity: both its
 * operands are ordinary vectors, and what is asked of them is whether every lane answers the same
 * way. So this recognizes `cmp(k1, k2)` in the two positions that read a mask, and nothing more
 * general.
 *
 * A mixed answer is left alone. It could be folded into a shuffle or into a pooled mask, and neither
 * is reachable from anything the library writes - `maskUpTo` of a constant is all-ones or nothing.
 *
 * An `and` against an all-*false* mask is left alone too, and that is a different refusal: the
 * answer is a mask of no lanes, which this IR has no constant form for. `select` has no such gap,
 * both its arms being values that already exist.
 */

// One lane of two constant vectors compared, at the relation and lane type given. The bytes are the
// vector's own, so a lane is read out of them at its width and its kind.
static bool constantLaneCompare(LowerCmp cmp, LowerLane lane, const U8* lhs, const U8* rhs) {
    if(lane == LowerLane::Float32 || lane == LowerLane::Float64) {
        F64 a = 0, b = 0;

        if(lane == LowerLane::Float32) {
            float na = 0, nb = 0;
            copyMem(lhs, &na, 4);
            copyMem(rhs, &nb, 4);
            a = na;
            b = nb;
        } else {
            copyMem(lhs, &a, 8);
            copyMem(rhs, &b, 8);
        }

        // An unordered pair answers false to every ordered relation and true to `neq`, which is what
        // the two tests below say without naming a NaN: `a == a` is false for one.
        auto ordered = (a == a) && (b == b);

        switch(cmp) {
            case LowerCmp::eq:  return a == b;
            case LowerCmp::neq: return a != b;
            case LowerCmp::lt:  return ordered && a < b;
            case LowerCmp::le:  return ordered && a <= b;
            case LowerCmp::gt:  return ordered && a > b;
            case LowerCmp::ge:  return ordered && a >= b;
            case LowerCmp::uno: return !ordered;
            case LowerCmp::ord: return ordered;
            default:            return false; // a signed integer relation between floats
        }
    }

    auto width = laneBytes(lane);
    U64 a = 0, b = 0;
    copyMem(lhs, &a, width);
    copyMem(rhs, &b, width);

    // Sign-extended for the signed relations, which is what makes `ilt` over an `i8` lane read `0xff`
    // as -1 rather than as 255.
    auto shift = 64 - width * 8;
    auto sa = I64(a << shift) >> shift;
    auto sb = I64(b << shift) >> shift;

    switch(cmp) {
        case LowerCmp::eq:  return a == b;
        case LowerCmp::neq: return a != b;
        case LowerCmp::lt:  return a < b;
        case LowerCmp::le:  return a <= b;
        case LowerCmp::gt:  return a > b;
        case LowerCmp::ge:  return a >= b;
        case LowerCmp::ilt: return sa < sb;
        case LowerCmp::ile: return sa <= sb;
        case LowerCmp::igt: return sa > sb;
        case LowerCmp::ige: return sa >= sb;
        default:            return false; // an ordering test on an integer lane
    }
}

/*
 * Whether this value is a comparison of two constant vectors that answers the same way in every
 * lane, and which way - with the chains that fed the constants collected for the sweep.
 *
 * The one question both readers below ask, which is why it is a function: a select wants the answer
 * to choose an arm and an `and` wants it to decide whether the mask takes anything away, and a
 * second copy of the lane walk would be a second chance to disagree about a NaN.
 */
static bool constantMaskAnswer(LowerBase base, LowerValue* value, bool& answer,
                               InstChain& chain) {
    auto inst = value->inst();
    if(inst->kind != LowerInst::Cmp) return false;

    auto cmp = (LowerInstCmp*)inst;
    auto lhs = base[cmp->lhs];
    auto rhs = base[cmp->rhs];
    auto type = lhs->type;

    // A vector of values, and not a mask: a mask lane's immediate is a truth value rather than the
    // pattern the machine holds, so what `constantVectorBytes` would answer about one is not what a
    // comparison of two of them means. Nothing produces that shape today; this is what keeps the
    // answer from depending on that staying true.
    if(!isIntVector(type) && !isFloatVector(type)) return false;

    auto size = Size(type.byteWidth());
    if(size > kMaxVectorBytes) return false;

    U8 left[kMaxVectorBytes] = {};
    U8 right[kMaxVectorBytes] = {};

    if(!constantVectorBytes(base, lhs, left, size, chain)) return false;
    if(!constantVectorBytes(base, rhs, right, size, chain)) return false;

    auto width = laneBytes(type.lane);
    answer = constantLaneCompare(cmp->getCmp(), type.lane, left, right);

    for(Size at = width; at < size; at += width) {
        if(constantLaneCompare(cmp->getCmp(), type.lane, left + at, right + at) != answer) {
            return false;
        }
    }

    return true;
}

void foldConstantMasks(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // What the walk below leaves behind: a comparison whose last reader it removed, and the
        // constant chains that fed one. Both are cleared after the walk rather than during it,
        // because either may stand *above* the instruction being folded - removing one there would
        // renumber the instructions the walk is indexing, which is the one thing this loop assumes
        // does not happen.
        InstChain dead;

        // Emptied per instruction rather than built per instruction, which is the difference
        // between one list for the block and one for every instruction in it - see InstChain.
        InstChain chain;

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];

            LowerValue* replacement = nullptr;
            LowerValue* condition = nullptr;
            auto answer = false;
            chain.clear();

            if(inst->kind == LowerInst::Select) {
                auto select = (LowerInstSelect*)inst;
                if(!isVectorLike(select->result.type)) continue;

                condition = base[select->cmp];
                if(!constantMaskAnswer(base, condition, answer, chain)) continue;

                // Every lane took the same side, so the select is that side and nothing else.
                replacement = base[answer ? select->lhs : select->rhs];
            } else if(inst->kind == LowerInst::And
                      && ((LowerInstBinary*)inst)->result.type.isMask()) {
                auto binary = (LowerInstBinary*)inst;

                /*
                 * `m .& allOnes` is `m`. Only that direction: the other one answers a mask of no
                 * lanes, which this IR cannot write down - see the note above.
                 */
                for(Size side = 0; side < 2 && !replacement; side++) {
                    chain.clear();
                    condition = base[side ? binary->lhs : binary->rhs];

                    if(!constantMaskAnswer(base, condition, answer, chain) || !answer) continue;
                    replacement = base[side ? binary->rhs : binary->lhs];
                }

                if(!replacement) continue;
            } else {
                continue;
            }

            replaceAllUses(base, &((LowerInstSingle*)inst)->result, replacement);
            removeInst(base, inst);

            // The comparison and the constants that fed it, for the sweep below - nothing between
            // here and emission is a dead-code elimination, so an instruction left with no readers
            // is one that gets encoded.
            dead.push(condition->inst());
            for(auto link: chain) dead.push(link);

            // The walk carries on from where the folded instruction was: what stands there now is
            // whatever followed it, and nothing above it changed.
            i--;
        }

        // Each is removed only once its own use list is empty, which is `removeDeadChain`'s rule and
        // is what keeps a comparison with a second reader, or a constant two chains share, exactly
        // where it is.
        removeDeadChain(base, dead);
    }
}

/*
 * A select one of whose arms is zero, which is an `and`.
 *
 * `select(m, v, 0)` keeps `v` where the mask is set and writes zero everywhere else, and a mask lane
 * is all-ones or all-zeros by construction - so that is `v & m`, one instruction, at every feature
 * level and in both domains. The mirrored `select(m, 0, v)` is `~m & v`, which is `pandn`, and is
 * one instruction as well because the complement is in the opcode rather than in front of it.
 *
 * What it replaces is the select, which is this backend's most expensive vector operation:
 *
 *   cmpltps  %xmm3,%xmm6      cmpltps %xmm3,%xmm6
 *   movaps   %xmm2,%xmm7   →  andps   %xmm6,%xmm5
 *   movaps   %xmm6,%xmm0
 *   pblendvb %xmm0,%xmm5,%xmm7
 *
 * Three instructions to one at SSE4.1, where the mask has to be copied into xmm0 because that is
 * where `pblendvb` reads it (see FormVSelectBlend); two to one under VEX, where `vpblendvb` takes
 * three register operands and the zero is one of them.
 *
 * **The register the zero was living in is the larger half of it.** A blend reads three vectors, so
 * the zero arm is a value with a live range - materialized in the entry block by `poolVectorConstants`
 * or a `pxor`, and held across whatever loop the select is in. Rewriting the select is what takes
 * the last reader off it; the pooled chain then goes the way every other orphaned constant here
 * goes, through `removeDeadChain`.
 *
 * Both arms are asked, and a select with zero on *both* would answer the first - which is a select
 * that is zero, and not a shape anything builds. `foldConstantMasks` above has already taken
 * the ones whose mask is constant, so what reaches here is a genuine runtime mask.
 */
void selectMaskedVectors(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // The constant chain the zero arm was, cleared after the walk for the reason the two passes
        // above clear theirs: it may stand above the select being rewritten, and removing it there
        // would renumber the instructions this loop is indexing.
        InstChain dead;

        // One list for the block, emptied per instruction - see InstChain.
        InstChain chain;

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Select) continue;

            auto select = (LowerInstSelect*)inst;
            auto type = select->result.type;

            // A vector select, and one this backend has a bitwise form for. A mask is deliberately
            // included: `select(m, k, allZeroMask)` over two masks is the same `pand`.
            if(!isVectorLike(type) || !isWholePackedRegister(type)) continue;

            auto size = Size(type.byteWidth());
            if(size > kMaxVectorBytes) continue;

            // Which arm is the zero, asked of the bytes rather than of the kind: a zero vector is a
            // `vsplat 0` here, a `.rodata` load once `poolVectorConstants` has run, and a lane chain
            // that happens to come to zero in between. This pass runs above that one, so what it
            // sees is the chain - and `constantVectorBytes` is the reader that knows all three.
            auto isZeroArm = [&](LowerPtr<LowerValue> arm, InstChain& chain) {
                U8 bytes[kMaxVectorBytes] = {};
                if(!constantVectorBytes(base, base[arm], bytes, size, chain)) return false;

                for(Size at = 0; at < size; at++) {
                    if(bytes[at]) return false;
                }

                return true;
            };

            chain.clear();
            auto complemented = false;

            if(isZeroArm(select->rhs, chain)) {
                complemented = false;
            } else {
                chain.clear();
                if(!isZeroArm(select->lhs, chain)) continue;
                complemented = true;
            }

            /*
             * The operand order is the machine's: `pand` is commutative and takes the value first so
             * that the tie lands on it, `pandn` computes `~lhs & rhs` and therefore takes the mask
             * first. See LowerInst::X86MaskAnd.
             */
            auto mask = base[select->cmp];
            auto value = base[complemented ? select->rhs : select->lhs];
            auto masked = new (fun.arena) LowerInstX86MaskAnd(
                select->result.name, type,
                (complemented ? mask : value) - base, (complemented ? value : mask) - base,
                complemented
            );

            insertInstAt(base, block, i, masked);
            replaceAllUses(base, &select->result, &masked->result);
            removeInst(base, select);

            for(auto link: chain) dead.push(link);
        }

        removeDeadChain(base, dead);
    }
}

/*
 * Vector constants - Implementation-Vector.md §0.2's prerequisite, finally spent on what it was
 * asked for.
 *
 * §5 concluded that the pool "turned out not to need to be" opened to vectors, on the evidence of
 * four operations that could each build their constant out of a scratch register. That generalized
 * from a sample chosen by being buildable: what is *not* buildable that way is every pattern with
 * more than one distinct lane in it, and the commonest of those is `iota` - which `maskUpTo` and
 * `firstSet` are both written over, so **every masked tail in every vector loop** was paying a chain
 * of `lanes` lane-writes where a load is one instruction.
 *
 * Where it sits is the same argument `poolFloatConstants` makes and it is worth more here: before
 * `selectMemorySources`, so `foldLoads` sees these loads. §5.4.1 opened the vector memory twin, so a
 * pooled constant read once by the instruction below it becomes `vpaddd xmm, xmm, [rip + k]` and the
 * common case is not one instruction but none.
 *
 * **Zero is pooled here where the float pass leaves it an immediate**, and the asymmetry is real
 * rather than an oversight. That pass keeps `0.0` because `xorps xmm, xmm` is a one-instruction form
 * this backend already selects, so the constant has something cheaper to lose to; a vector has no
 * such form - `vsplat 0` is a general register zeroed, a bank crossing and a shuffle - so there is
 * nothing for zero to lose to and the load wins on count outright. If a `pxor` peephole is written
 * it belongs *before* this pass, which will then not see the splat at all.
 */
/*
 * A constant chain that is uniformly zero or uniformly all-ones, rewritten as the splat it means.
 *
 * Reachable where a program wrote the pattern the long way - `withLane`ing a vector into all-ones a
 * lane at a time is not idiomatic but is expressible - and it exists so that the peephole below has
 * one shape to recognize rather than two. The immediate is marked Implicit because the form's
 * operand is `folded()`: the opcode *is* the value, so nothing about the scalar is encoded and it
 * must not be given a register. A fresh one rather than reusing whatever the chain held, since a
 * shared constant may have a scalar reader that does need its register.
 */
// Answers the splat it left standing, which is what the caller repositions its walk from - see the
// note on `removeDeadChain` moving instructions *above* the cursor.
static LowerInst* replaceWithConstantSplat(LowerBase base, LowerBlock* block, Size at, LowerValue* result,
                                           LowerType type, bool zero, InstChain& chain) {
    auto& fun = *base[block->fun];
    auto scalar = scalarFormOf(type);
    auto width = laneBytes(type.lane);
    auto value = zero ? U64(0) : (width >= 8 ? ~U64(0) : ((U64(1) << (width * 8)) - 1));

    auto imm = new (fun.arena) LowerImm(StringId(), scalar, value);
    imm->result.flags |= LowerValue::Implicit;

    auto splat = new (fun.arena) LowerInstVecSplat(result->name, type, &imm->result - base);

    insertInstAt(base, block, at, imm);
    insertInstAt(base, block, at + 1, splat);

    replaceAllUses(base, result, &splat->result);
    removeInst(base, base[result->inst() - base]);
    for(Size c = 0; c < chain.size(); c++) if(chain[c] == result->inst()) chain[c] = nullptr;

    removeDeadChain(base, chain);
    return splat;
}

void poolVectorConstants(Context& ctx, LowerBase base, LowerFunction& fun) {
    auto& module = *fun.module;

    // One list for the function, emptied per candidate - see InstChain. Every instruction of every
    // block reaches the walk below, and most of them are not constants at all.
    InstChain chain;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];

            /*
             * A bitcast is a root as well as a link, and for one reason: it is where a constant
             * written at one lane kind is read at another (see `constantVectorBytes`), and what has
             * to be pooled is the *outer* type - the entry's bytes are the same either way, and the
             * load's type is what decides which instruction reads it.
             *
             * A shuffle and a shared-count shift are roots for a different reason: they are what an
             * *expansion* builds over a constant it was handed. `expandVectorMulHi` brings the high
             * limb of its multiplier into reach with a `pshufd` and takes the multiplier's sign with
             * a `psrad`, and both stand in whatever loop the division was in - there is no code
             * motion below this point to lift them out. Pooled here they leave nothing at all, which
             * is better than being hoisted.
             */
            if(inst->kind != LowerInst::VecSplat && inst->kind != LowerInst::VecWithLane
               && inst->kind != LowerInst::Bitcast && inst->kind != LowerInst::VecShuffle
               && inst->kind != LowerInst::Shl && inst->kind != LowerInst::Shr
               && inst->kind != LowerInst::Sar) {
                continue;
            }

            auto result = &((LowerInstSingle*)inst)->result;
            auto type = result->type;
            if(!isVectorLike(type)) continue;

            // Nothing reads it, so there is nothing to point at the pool - and interning a constant
            // no instruction mentions would put bytes in the image for it. The float pass declines
            // the same case for the same reason.
            if(result->uses.isEmpty()) continue;


            /*
             * An intermediate of a longer constant is left alone: its one reader is the next lane
             * write, which is about to be pooled whole, and this link then dies with the rest of the
             * chain. Pooling every link would put `lanes` entries in the image where one is wanted.
             */
            auto onlyReaderExtends = false;

            /*
             * **Every** reader has to be the same one, not merely one reader. A shuffle of a value
             * with itself names it twice, so `vshuffle %c, %c` is two uses of one instruction - and
             * that is the ordinary shape of the constant work an expansion leaves behind, since a
             * `pshufd` reads one source.
             */
            LowerInst* sole = nullptr;

            for(auto use: result->uses.contents(base)) {
                auto reader = base[use];
                if(sole && reader != sole) { sole = nullptr; break; }
                sole = reader;
            }

            if(sole) {
                auto reader = sole;

                if(reader->kind == LowerInst::VecWithLane &&
                   ((LowerInstVecLane*)reader)->from == (result - base)) {
                    onlyReaderExtends = true;
                }

                // And the same for a bitcast of it, which is the other way a chain continues: the
                // link below is about to be pooled whole at the *outer* type, and pooling this one
                // as well would put two entries in the image and leave the reader reading a bitcast
                // of a load rather than the load.
                if(reader->kind == LowerInst::Bitcast && isVectorLike(reader->created()[0].type)) {
                    onlyReaderExtends = true;
                }

                // And the two an expansion builds over a constant, both of which answer a constant
                // and are read through above. Pooling this link would put the *unshuffled* pattern
                // in the image and leave the shuffle standing in whatever loop it is in.
                if(reader->kind == LowerInst::VecShuffle) onlyReaderExtends = true;

                if((reader->kind == LowerInst::Shl || reader->kind == LowerInst::Shr
                    || reader->kind == LowerInst::Sar)
                   && ((LowerInstBinary*)reader)->lhs == (result - base))
                {
                    onlyReaderExtends = true;
                }
            }

            if(onlyReaderExtends) continue;

            U8 bytes[kMaxVectorBytes] = {};
            auto size = Size(type.byteWidth());
            if(size > kMaxVectorBytes) continue;

            chain.clear();
            if(!constantVectorBytes(base, result, bytes, size, chain)) continue;

            /*
             * The two patterns this machine makes out of nothing, left for the peepholes - §5.7.
             *
             * `pxor r, r` and `pcmpeqd r, r` are one instruction each with no memory, no `.rodata`
             * entry and no general register on the way in, so a load has nothing to offer either.
             * They are left as a *splat of their scalar*, which is the form `selectPackedForm` reads
             * to pick the pseudo - and a chain that happens to be all-zero or all-ones by a route
             * other than a splat is rewritten into one here, so the peephole sees one shape rather
             * than two.
             *
             * Only these two, deliberately. A float sign mask is all-ones shifted, and an abs mask
             * is all-ones shifted the other way - two instructions each, which is *not* obviously
             * better than one load and would need measuring. Guessing is the mistake §5 made in the
             * other direction, and it is not worth making twice.
             */
            auto uniform = [&](U8 value) {
                for(Size at = 0; at < size; at++) if(bytes[at] != value) return false;
                return true;
            };

            if(uniform(0x00) || uniform(0xff)) {
                if(inst->kind != LowerInst::VecSplat) {
                    auto splat = replaceWithConstantSplat(base, block, i, result, type,
                                                          uniform(0x00), chain);

                    if(auto now = positionOf(base, block, splat)) i = now.unwrap();
                    continue;
                }

                /*
                 * A splat is already the shape the peephole reads - but only if its scalar can be
                 * taken out of allocation, which is what `folded()` means and what
                 * `onlyFeedsMachineSplats` answers. A constant some scalar instruction *also* reads
                 * keeps its register, so this splat has no pseudo to be selected into and would
                 * reach the form as an operand that is folded and placed at once.
                 *
                 * That one takes the pool below like any other constant. It is the same reason
                 * `replaceWithConstantSplat` builds a fresh immediate rather than reusing the
                 * chain's, stated from the other side: the two forms of a shared constant are a
                 * private copy or no pseudo, and only one of them is available here.
                 */
                auto scalar = base[((LowerInstVecSplat*)inst)->from]->inst();

                if(scalar->kind == LowerInst::Imm &&
                   onlyFeedsMachineSplats(base, (LowerImm*)scalar))
                {
                    continue;
                }
            }

            auto global = pooledBytes(ctx, module, bytes, size);
            auto address = new (fun.arena) LowerInstGlobal(StringId(), global - *module.arena);
            auto load = new (fun.arena) LowerInstLoad(
                &address->result - base, result->name, type, U32(size), false
            );

            insertInstAt(base, block, i, address);
            insertInstAt(base, block, i + 1, load);

            replaceAllUses(base, result, &load->result);

            // The outermost link is removed here and cleared from the chain, so the sweep below sees
            // only what fed it.
            removeInst(base, inst);
            for(Size c = 0; c < chain.size(); c++) if(chain[c] == inst) chain[c] = nullptr;

            removeDeadChain(base, chain);

            /*
             * **The cursor is found again rather than trusted**, because what was just removed was
             * not all below it. A chain's links sit *above* its root - that is what a chain is - so
             * `removeDeadChain` shortens this block in front of `i`, and a walk that carried on from
             * where it was would skip exactly as many instructions as the chain had links.
             *
             * Harmless for as long as a chain was only ever a `vsplat` and its lane writes, since
             * what followed one was the reader that had just been rewritten. It stopped being
             * harmless when a constant grew *two* derived chains beside it: pooling the first one
             * stepped the walk clean over the second, which then stood in the loop it had been
             * lifted out of.
             */
            if(auto now = positionOf(base, block, load)) i = now.unwrap();

            // The walk resumes at the load. Two were inserted and at least this instruction removed,
            // so the index is restarted from the block rather than adjusted - the chain removal may
            // have taken any number of instructions out from above it.
            i = 0;
        }
    }
}

/*
 * A 256-bit lane pattern that no single shuffle instruction expresses, made into `vpermd`/`vpermps`.
 *
 * **Every shuffle AVX2 has works inside each 128-bit half**, and the one exception moves halves
 * *entire* (`vperm2f128`). So an eight-lane 32-bit pattern like `[0, 4, 1, 5, 2, 6, 3, 7]` - an
 * interleave, and one instruction at four lanes - is not an instruction at this width at all, and
 * `wideShuffleChoice` answered nothing for it.
 *
 * `vpermd` is the general answer and the reason this pass has to exist rather than a form: its
 * pattern is **one lane index per result lane, held in a vector register**. A form cannot produce
 * that, because a form does not create operands - so the pattern has to stop being part of the
 * instruction and become a value, which is a `.rodata` entry, a load, a live range and a register.
 * That is the whole of what happens here, and it is why the note in `wideShuffleChoice` said this
 * needed the vector constant pool: it is the pool that makes a pattern into a value.
 *
 * **One source only.** `vpermd` reads a single vector, so a pattern naming lanes of both sources is
 * left refused - two permutes and a blend would express one, and whether that is worth three
 * instructions and two pooled constants is a question this pass does not answer.
 *
 * The index vector is an **integer** vector at both rows, `vpermps` included: a lane index is a
 * number whatever domain the lanes it indexes are read in. Only the low three bits of each index are
 * read by the instruction, so nothing here has to mask.
 */
void lowerWideLanePermutes(Context& ctx, LowerBase base, LowerFunction& fun) {
    // The one feature level that has the instruction, and the one that can hold a value wide enough
    // to need it - `targetVectorBytes` answers 32 here and 16 below, so a function compiled without
    // AVX2 has no eight-lane vector for this to be asked about.
    if(!(targetFeatures() & kFeatureAvx2)) return;

    auto& module = *fun.module;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::VecShuffle) continue;

            auto shuffle = (LowerInstVecShuffle*)inst;
            auto type = shuffle->result.type;

            // The one shape with a row: 32-bit lanes filling a 256-bit register. A 64-bit lane's
            // general permute is `vpermq`, whose pattern is an immediate and which therefore belongs
            // to `wideShuffleChoice` rather than here; a narrower one has no crossing permute below
            // AVX-512 at all.
            if(!isWideVector(type) || laneBytes(type.lane) != 4) continue;

            // An instruction already expresses it, which is cheaper by a pooled constant and a
            // register - so this asks the same function the form selection will ask, rather than a
            // restatement of it that could drift.
            if(packedShuffleChoice(inst)) continue;

            auto lanes = type.lanes();
            auto pattern = shuffle->pattern();

            /*
             * Which of the two sources every entry names, and nothing if they name both.
             *
             * The IR numbers the second source's lanes from `lanes` upward, so this is one
             * comparison per entry - and the answer is the operand `vpermd` will read, with the
             * indices then written relative to it.
             */
            auto second = pattern[0] >= lanes;
            auto oneSource = true;

            for(U32 k = 0; k < lanes && oneSource; k++) {
                if((pattern[k] >= lanes) != second) oneSource = false;
            }

            if(!oneSource) continue;

            // The indices, one 32-bit lane each, little-endian like every other entry this pool
            // holds. `vpermd` reads the low three bits of each, so an index relative to the source
            // it belongs to is the whole of what has to be written.
            U8 bytes[kMaxVectorBytes] = {};
            auto size = Size(type.byteWidth());

            for(U32 k = 0; k < lanes; k++) {
                bytes[k * 4] = U8(second ? pattern[k] - lanes : pattern[k]);
            }

            auto indexType = LowerType { LowerLane::Int32, type.laneShift, false };
            auto global = pooledBytes(ctx, module, bytes, size);
            auto address = new (fun.arena) LowerInstGlobal(StringId(), global - *module.arena);
            auto load = new (fun.arena) LowerInstLoad(
                &address->result - base, StringId(), indexType, U32(size), false
            );

            auto source = second ? shuffle->right : shuffle->left;
            auto permute = new (fun.arena) LowerInstX86Permute(
                shuffle->result.name, type, &load->result - base, source
            );

            insertInstAt(base, block, i, address);
            insertInstAt(base, block, i + 1, load);
            insertInstAt(base, block, i + 2, permute);

            replaceAllUses(base, &shuffle->result, &permute->result);
            removeInst(base, inst);

            // Three inserted and one removed, and the walk carries on below the permute - which
            // reads a load this pass has no further business with.
            i += 2;
        }
    }
}

/*
 * §41.6 A vector constant defined above a call it is live across.
 *
 * `sumVectors` builds its zero accumulator at the top of the function and then calls `elements` to
 * get at the array's storage, so the zero is live across a call - and there is no callee-saved
 * vector register on this ABI. What that costs is not a spill and a reload: a 16- or 32-byte slot
 * raises the frame's alignment past what the convention promises, so the function grows a *dynamic*
 * frame - a frame pointer held throughout, `and $-32,%rsp`, and the `leave` that undoes it - all for
 * a value that is one `vpxor` to recreate.
 *
 * The rematerializer would answer this if it could, and it cannot: `%zero` is the incoming arm of
 * the accumulator's phi, so copy coalescing (§17.2) has already made it one web with the phi and the
 * addition, and a web with several definitions has no single recipe that reproduces it. That is not
 * a gap in `recipeFor` - it is the correct answer to the question it was asked.
 *
 * So the definition moves instead. A constant reads nothing, so it may stand anywhere its readers do
 * not precede it, and putting it *below* the call is what makes the live range not cross one at all
 * - no spill, no slot, no alignment, and the same one instruction. The phi is the common case and it
 * needs no reader in this block: a phi's operand is live at the end of the predecessor, so "nothing
 * here reads it" sinks the definition to the bottom of the block, which is exactly where the edge
 * takes it.
 *
 * **Only past a call**, which is what keeps this from being a scheduler. A constant that is already
 * below every call in its block is left where it is: moving it would shorten a live range that costs
 * nothing to hold, and every instruction this pass does not move is one whose position the passes
 * above it chose deliberately - `poolVectorConstants` puts the absolute-value mask at the top of the
 * entry block's successor precisely so that it is *out* of the loop below.
 */
static bool isSinkableVectorConstant(LowerBase base, LowerInst* inst) {
    if(inst->kind != LowerInst::VecSplat) return false;

    auto splat = (LowerInstVecSplat*)inst;
    if(!isVectorLike(splat->result.type)) return false;

    // A splat of a literal, which is the one vector constant that is still an instruction here:
    // everything with more than one distinct lane in it became a `.rodata` load one pass up, and a
    // load's address is a second value that would have to travel with it.
    return base[splat->from]->inst()->kind == LowerInst::Imm;
}

void sinkVectorConstants(Context&, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];
        auto list = block->instructions.contents(base);

        // Nothing to sink past, which is the common block and is answered before anything is built.
        auto hasCall = false;
        for(auto instPtr: list) {
            if(base[instPtr]->kind == LowerInst::Call) { hasCall = true; break; }
        }

        if(!hasCall) continue;

        // What is being moved, and what it is being moved in front of - null for "the end of the
        // block", which is where a value only a phi reads belongs.
        struct Sunk { LowerInst* inst; LowerInst* before; };
        SmallArray<Sunk, 8> sunk;

        for(Size i = 0; i < list.size(); i++) {
            auto inst = base[list[i]];
            if(!isSinkableVectorConstant(base, inst)) continue;

            /*
             * The first reader in this block, and whether a call stands between the two. A reader
             * *above* the definition is impossible in SSA and is not checked for; what is checked is
             * that every position considered is one this instruction may legally occupy, which for
             * something that reads nothing is every position before its first reader.
             */
            LowerInst* before = nullptr;
            auto target = list.size();

            for(Size j = i + 1; j < list.size(); j++) {
                auto reader = base[list[j]];
                auto reads = false;

                for(auto used: reader->used()) {
                    if(base[used]->inst() == inst) { reads = true; break; }
                }

                if(reads) { before = reader; target = j; break; }
            }

            auto crossesCall = false;
            for(Size j = i + 1; j < target; j++) {
                if(base[list[j]]->kind == LowerInst::Call) { crossesCall = true; break; }
            }

            if(!crossesCall) continue;

            /*
             * The immediate the splat reads travels with it where nothing else reads it. Usually it
             * is implicit - a `vsplat 0` selects a form that builds its own zero and the literal has
             * no register at all - but a splat the machine has to build out of a general register
             * would otherwise leave that register live across the call this just moved past.
             */
            auto scalar = base[((LowerInstVecSplat*)inst)->from];
            if(scalar->uses.size() == 1 && scalar->inst()->block == block - base) {
                sunk.push(Sunk { scalar->inst(), before });
            }

            sunk.push(Sunk { inst, before });
        }

        if(sunk.size() == 0) continue;

        // Rebuilt in one walk: everything that stayed, in its own order, with each sunk instruction
        // emitted immediately in front of the reader it was moved to - and the ones with no reader
        // here at the end, in front of the terminator, which is not in this list.
        auto moved = [&](LowerInst* inst) {
            for(auto& entry: sunk) {
                if(entry.inst == inst) return true;
            }

            return false;
        };

        SmallArray<LowerPtr<LowerInst>, 32> rebuilt;

        for(auto instPtr: list) {
            auto inst = base[instPtr];
            if(moved(inst)) continue;

            for(auto& entry: sunk) {
                if(entry.before == inst) rebuilt.push(entry.inst - base);
            }

            rebuilt.push(instPtr);
        }

        for(auto& entry: sunk) {
            if(!entry.before) rebuilt.push(entry.inst - base);
        }

        block->instructions.clear();
        for(auto instPtr: rebuilt) block->instructions.push(fun.arena, instPtr);
    }
}

void poolFloatConstants(Context& ctx, LowerBase base, LowerFunction& fun) {
    auto& module = *fun.module;

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Imm) continue;

            auto imm = (LowerImm*)inst;
            if(!isFloat(imm->result.type)) continue;

            // The IR keeps every float constant as a double, so a single-precision one is rounded to
            // what it will actually be before its bits are taken - the same two lines `emitFloatImm`
            // ran, moved to where the answer is now stored rather than encoded.
            auto is64 = imm->result.type == LowerType::Float64;
            auto size = Size(is64 ? 8 : 4);
            auto bits = U64(0);

            if(is64) {
                auto value = imm->f;
                copyMem(&value, &bits, sizeof(value));
            } else {
                auto value = float(imm->f);
                U32 narrow = 0;
                copyMem(&value, &narrow, sizeof(value));
                bits = narrow;
            }

            /*
             * **A single-precision constant stays an immediate, and that is a measurement.**
             *
             * `mov r32, imm32; movd xmm, r32` is eleven bytes against the load's eight, and on
             * test/bench/programs the three bytes cost 2% of `Float.yana` - 251 ms against 255,
             * reproduced at five different function alignments, so it is the load and not where the
             * loop landed. `escape` is the shape that finds it: a Mandelbrot point usually escapes
             * after a couple of iterations, so the constant is materialized on the entry path far
             * more often than it is used, and a load's latency sits on that path where two ALU
             * operations do not.
             *
             * A double is the same program with one word changed and it comes out the other way:
             * 354 bytes to 329 and no measurable time either way. The immediate form needs
             * `mov r64, imm64` there - ten bytes rather than six - and the four bytes of instruction
             * fetch it saves at the top of a hot function are worth about what the load costs.
             *
             * So the rule is the width, both halves of it measured on one program. It is not the
             * last word: a vector constant has no immediate form at all, and this pass is what will
             * hold it.
             */
            if(!is64) continue;

            // Positive zero stays an immediate: `xorps xmm, xmm` is two bytes where any load is
            // eight, and it needs no general register either - so the pseudo has nothing to lose to
            // here. Negative zero is *not* this, and the bit test rather than a comparison against
            // 0.0 is what says so.
            if(bits == 0) continue;

            // Nothing reads it, so there is nothing to point at the pool. Left where it is: a dead
            // instruction is dropped by the allocator either way, and interning a constant no
            // instruction mentions would put bytes in the image for it.
            if(imm->result.uses.isEmpty()) continue;

            auto global = pooledConstant(ctx, module, bits, size);
            auto address = new (fun.arena) LowerInstGlobal(StringId(), global - *module.arena);
            auto load = new (fun.arena) LowerInstLoad(
                &address->result - base, imm->result.name, imm->result.type, U32(size), false
            );

            insertInstAt(base, block, i, address);
            insertInstAt(base, block, i + 1, load);

            replaceAllUses(base, &imm->result, &load->result);
            removeInst(base, imm);

            // The immediate is gone from the position it held, so the two insertions above net out
            // to one: the walk resumes at the load, and neither of the two is an `Imm`.
            i++;
        }
    }
}
