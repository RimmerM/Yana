#include "transform_internal.h"

/*
 * The CFG a loop is laid out as.
 *
 * Rotation moves a loop test from the top to the bottom and the layout spends that, so the two are
 * one subject and live in one file. Everything else in this directory reasons about a position
 * within the CFG, which is why the pipeline runs the rotation first and the ordering last.
 */

/*
 * Loop rotation.
 *
 * A loop written with its test at the top costs two branches an iteration: the conditional one that
 * decides whether to run the body, and the unconditional one that goes back for the next iteration.
 * Only one of the two can ever be a fallthrough, whatever order the blocks are put in - which is why
 * this is a transform rather than a further refinement of §3.2.
 *
 *   head:  cmp rax,rsi        =>    pre:   cmp rax,rsi
 *          jge exit                        jge exit
 *          ...body...               body:  ...body...
 *          jmp head                 head:  cmp rax,rsi
 *   exit:                                  jl  body
 *                                   exit:
 *
 * What moves is not the test but the *entry*: the preheader stops jumping into the header and asks
 * the header's own question instead, so the header is left reachable only from the latch and becomes
 * the bottom of the loop. The body is then the block the loop is entered through, the header is the
 * block it is left from, and the two branches an iteration used to pay are one.
 *
 * The test is therefore evaluated in two places, and both of them run exactly when the single copy
 * used to: the preheader's copy is the first iteration's test, and the header's copy is every later
 * one. Nothing is speculated and nothing runs an extra time, which is why a load in the header is as
 * duplicable as a compare - the limit below is about code size and nothing else.
 *
 * ## What it costs in bytes
 *
 * Nothing, to within an instruction. The preheader's `jmp` and the latch's `jmp` both disappear into
 * fallthroughs, and what replaces them is the duplicated test - a compare and a conditional branch
 * against two five-byte jumps.
 *
 * ## SSA, and why the phis move
 *
 * A header phi names a value per predecessor, and after rotation the header has only the latch left.
 * The merge it was performing has moved to the body, which is now what both the preheader and the
 * header lead into, so each header phi becomes one there - and, where the loop's result is read
 * afterwards, one in the exit block as well, since the exit is now reached from the preheader too.
 * Both take the same pair: what the preheader hands over, and what the rotated header holds.
 *
 * The one that is easy to get wrong is what a *header* instruction reads. The rotated header runs
 * after the body, so a phi it used to read has already been advanced by the latch: the value it
 * wants is the phi's latch alternative, and a header phi appearing in that alternative is in turn
 * the body's phi. `%i` in the test becomes `%i2`, which is exactly the induction variable the
 * iteration just finished computing.
 *
 * ## What is declined
 *
 * The shape has to be the ordinary one, and every requirement below exists because the repair above
 * is stated in terms of it - one preheader ending in an unconditional jump, one latch, and a header
 * whose two successors are one block inside the loop and one outside, each reached from nowhere else.
 * The header must also be the only block the loop leaves through, or a value it defines could be
 * read on a path the exit block does not dominate and there would be nowhere to put the phi.
 *
 * A header instruction read anywhere but the header is declined for the same reason in miniature.
 * The repair exists - it is the same pair of phis - but the shape it would serve is a header doing
 * work rather than a header asking a question, and duplicating that work is a different trade.
 */

// The largest header this will duplicate. The shape it is for is a comparison and its operands, and
// a header past this size is one where the duplication is the dominant cost rather than a rounding
// error against the two jumps it removes.
static constexpr Size kMaxRotatedHeader = 4;

// Whether the header's copy of this instruction may also be made in the preheader.
//
// Every kind here computes one value from its operands and reads nothing that the block it moves
// into cannot supply. A store, a call or a `copy` is excluded because duplicating it duplicates an
// effect - even though it would run the same number of times, the second copy is code that has to
// be kept in step with the first - and an `alloca` because a second one is a second allocation.
static bool isRotatableHeaderInst(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Imm:
        case LowerInst::Global:
        case LowerInst::Fun:
        case LowerInst::Set:
        case LowerInst::Cast:
        case LowerInst::Bitcast:
        case LowerInst::Neg:
        case LowerInst::Not:
        case LowerInst::Add:
        case LowerInst::Sub:
        case LowerInst::Mul:
        case LowerInst::IMul:
        case LowerInst::MulHi:
        case LowerInst::IMulHi:
        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar:
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
        case LowerInst::Cmp:
        case LowerInst::Select:
        case LowerInst::Load:
            return true;
        default:
            return false;
    }
}

// How much storage one of the kinds above occupies. Each of them is a fixed-shape allocation - the
// created value and the operand pointers are members rather than a trailing array - so the copy
// below can be a flat one, and every field an instruction carries comes across without this having
// to know which fields those are.
static Size rotatedInstSize(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Imm:    return sizeof(LowerImm);
        case LowerInst::Global: return sizeof(LowerInstGlobal);
        case LowerInst::Fun:    return sizeof(LowerInstFun);
        case LowerInst::Cast:   return sizeof(LowerInstCast);
        case LowerInst::Cmp:    return sizeof(LowerInstCmp);
        case LowerInst::Select: return sizeof(LowerInstSelect);
        case LowerInst::Load:   return sizeof(LowerInstLoad);
        default:                return isUnary(inst) ? sizeof(LowerInstUnary) : sizeof(LowerInstBinary);
    }
}

// A second copy of one header instruction, detached: it belongs to no block and nothing reads it
// yet, and its operands still name what the original's did until the caller remaps them.
//
// The result value is rebuilt in place rather than patched, which is what clears the use list the
// copy inherited - a list whose entries name the *original's* readers - and drops the source name
// with it, so the two copies do not both claim to be `%i`.
static LowerInst* cloneHeaderInst(Region<LowerRegion>& arena, LowerInst* inst) {
    auto size = rotatedInstSize(inst);
    auto clone = (LowerInst*)arena.alloc(size);
    copyMem(inst, clone, size);

    clone->block = nullptr;
    clone->liveId = kNullLive;

    auto created = clone->created();
    for(Size i = 0; i < created.size(); i++) {
        auto flags = created[i].flags;
        new (&created[i]) LowerValue(clone, created[i].type, StringId());
        created[i].flags = flags;
    }

    return clone;
}

// Which alternative of `phi` the edge from `from` carries.
static Size phiSourceIndex(LowerBase base, LowerInstPhi* phi, LowerBlock* from) {
    auto sources = phi->sources();

    for(Size i = 0; i < sources.size(); i++) {
        if(base[sources[i]] == from) return i;
    }

    assertTrue("a phi has no alternative for one of its own predecessors" == nullptr);
    return 0;
}

// Everything an instruction reads stops counting it as a reader. The instruction itself is dropped
// from wherever it is listed by the caller - see removeInst, which is this plus that.
static void detachOperands(LowerBase base, LowerInst* inst) {
    for(auto offset: inst->used()) {
        auto v = base[offset];
        auto uses = v->uses.contents(base);

        for(Size i = 0; i < uses.size(); i++) {
            if(base[uses[i]] == inst) { v->uses.remove(base, i); break; }
        }
    }
}

// Points one operand of `user` at a different value, both use lists included.
static void retargetOperand(LowerBase base, LowerInst* user, Size slot, LowerValue* to) {
    auto from = base[user->used()[slot]];
    if(from == to) return;

    replaceUse(base, from, user, to);
    user->used()[slot] = to - base;
}

// Replaces the phi at `index` with one that takes an alternative from one more predecessor.
//
// A phi's alternatives are allocated with it, so gaining an edge means a new instruction rather than
// a longer list - and the result value moves with it, which is why every reader has to be pointed at
// the replacement. Only the exit and body blocks need this, and only for the preheader's new edge.
static void growPhi(LowerBase base, LowerFunction& fun, LowerBlock* block, Size index,
                    LowerBlock* extraSource, LowerValue* extraValue)
{
    auto& arena = fun.arena;
    auto old = base[block->phis.get(base, index)];
    auto count = Size(old->usedCount);

    auto phi = makePhi(arena, old->result.type, U32(count + 1));
    phi->result.name = old->result.name;
    phi->source = old->source;
    phi->block = block - base;

    auto used = phi->used();
    auto sources = phi->sources();
    auto oldUsed = old->used();
    auto oldSources = old->sources();

    for(Size i = 0; i < count; i++) {
        used[i] = oldUsed[i];
        sources[i] = oldSources[i];
    }

    used[count] = extraValue - base;
    sources[count] = extraSource - base;

    detachOperands(base, (LowerInst*)old);
    for(auto u: used) base[u]->uses.push(arena, (LowerInst*)phi - base);

    block->phis.set(base, index, phi - base);
    replaceAllUses(base, &old->result, &phi->result);
}

// A phi this pass built that nothing turned out to read. Taken back out rather than left behind,
// since a value with no readers is still a live range the allocator would carry through the loop.
bool dropUnusedPhi(LowerBase base, LowerBlock* block, LowerInstPhi*& phi) {
    if(!phi || phi->result.uses.size()) return false;

    for(Size i = 0; i < block->phis.size(); i++) {
        if(base[block->phis.get(base, i)] == phi) {
            block->phis.remove(base, i);
            break;
        }
    }

    detachOperands(base, (LowerInst*)phi);
    phi = nullptr;
    return true;
}

// One loop in the shape the rotation is stated in - see the comment above for what each block has to
// be, and rotatableLoop for what is checked.
struct RotatableLoop {
    LowerBlock* header;
    LowerBlock* pre;    // the one predecessor outside the loop, ending in an unconditional jump
    LowerBlock* latch;  // the one predecessor inside it
    LowerBlock* body;   // the header's successor inside the loop, which the rotation makes the entry
    LowerBlock* exit;   // the header's successor outside it
};

// One header phi and the three values it becomes: what the preheader hands over, what the rotated
// header holds, and the merge of the two in each of the blocks that now sees both.
struct RotatedPhi {
    LowerInstPhi* header;
    LowerValue* pre;
    LowerValue* hdr;
    LowerInstPhi* body;
    LowerInstPhi* exit;
};

/*
 * A block a loop leaves to that carries nothing onwards - the one second exit the rotation can take.
 *
 * The gate below asks for a single exit because of where a header phi's readers are sent: one outside
 * the loop is pointed at the merge built in the *exit* block, which is only its value where the exit
 * dominates it. A second way out reaches code the exit does not dominate, and there is nothing to
 * point such a reader at.
 *
 * Unless the block it reaches is dominated by the loop's own body, in which case there is: the merge
 * built there. That is what this asks for, in the two conditions it is the conjunction of - every way
 * in comes from inside the loop, and there is no way onwards - because together they say the block is
 * dominated by the body after the rotation as well as before it, and that nothing beyond it can be
 * looking at a loop value at all.
 *
 * Two shapes in ordinary code are exactly this. A bounds check's abort arm is one that reads nothing
 * (§10 item 2 of test/bench/findings.md gave every one of them a `ret`, which is what made every
 * checked loop multi-exit); an early `return` out of a `while` is one that reads the induction
 * variable, and `Float.escape` in the corpus is a loop that pays an unconditional jump per iteration
 * for having one.
 *
 * **Or the block reads nothing the loop defines**, which is the same conclusion reached without the
 * first condition. What the two together establish is where a *reader* of a header phi can be
 * pointed; a block with no such reader has nothing to point anywhere, and having no successors it
 * cannot hand one on either. So the predecessors stop mattering, and that is not a corner: it is the
 * abort arm again, once `mergeIdenticalExits` has made the program's copies of it one block. Sharing
 * one exit between two nested loops gives the inner one's arm a predecessor from the outer, and
 * without this every one of Matrix's three loops stopped rotating and its innermost paid a jump per
 * iteration for it.
 */
static bool readsLoopValue(LowerBase base, const LoopInfo& loops, U32 headerIndex, LowerInst* inst) {
    for(auto used: inst->used()) {
        auto from = base[used]->inst()->block;
        if(from && loops.contains(headerIndex, base[from]->index)) return true;
    }

    return false;
}

/*
 * §48.1 The same conclusion for a second exit that does carry on.
 *
 * `terminalExit` above asks for two things at once - every way in comes from the loop, and there is
 * no way onwards - and only the first of them is the dominance argument. The second is there because
 * a *successor* of such a block is a reader the first condition says nothing about.
 *
 * So the first condition is taken to its own fixpoint instead, and the same question is asked of the
 * other side as well, because after the rotation there are two merges to point a reader at and each
 * is only that reader's value where it dominates it:
 *
 *   the body's merge   in a block every path into which passes through the loop
 *   the exit's merge   in a block every path into which passes through the header's exit arm
 *
 * Both are the same walk over one seed set, which is what this is. A block joins when it has
 * predecessors and every one of them is in the seeds or already joined - "every path here passes
 * through the seeds" - and since the rotation makes the body the block the loop is entered through,
 * a block the loop is the only way into is one the body dominates.
 *
 * **A reader in neither set is what refuses the loop, and `Iter.firstOverOrCount` is why.** Its two
 * early exits converge on a block that also falls into the block the header's exit arm reaches, so
 * the count is read where *neither* merge dominates - and rotating it anyway produced a function that
 * read a register the guard path had never written. That is the check `rotatableLoop` makes below,
 * and it is over the phis' readers rather than over the exits, because a reader is what needs a
 * value and an exit is only how one gets there.
 *
 * `indexOfVectors` in the SIMD corpus is the shape this is for: a search loop whose found-arm reads
 * the accumulated index and then jumps to the function's common return, which is one block onwards
 * and so one block too far for `terminalExit`.
 */
static void collectReachedOnlyFrom(LowerBase base, LowerFunction& fun, const IndexSet& seeds,
                                   const IndexSet& excluded, IndexSet& into)
{
    into.reset(fun.blocks.size());

    auto changed = true;
    while(changed) {
        changed = false;

        for(auto o: fun.blocks.contents(base)) {
            auto block = base[o];
            auto index = Size(block->index);

            if(into[index] || seeds[index] || excluded[index]) continue;

            // The entry block has no predecessors, so the rule below would admit it vacuously.
            if(block->incoming.isEmpty()) continue;

            auto only = true;
            for(auto p: block->incoming.contents(base)) {
                auto pred = Size(base[p]->index);
                if(seeds[pred] || into[pred]) continue;

                only = false;
                break;
            }

            if(!only) continue;

            into.set(index, true);
            changed = true;
        }
    }
}

/*
 * Where each of the rotation's three answers is the reader's value, as three sets of blocks.
 *
 * Built per candidate loop and reused across them, since every one of them is a walk over the
 * function's blocks and a loop that is refused has already paid for it.
 */
struct RotationRegions {
    IndexSet inLoop;    // the loop's own blocks
    IndexSet bodySide;  // outside it, and the loop is the only way in
    IndexSet exitSide;  // the header's exit arm, and everything it is the only way into

    // What each walk is given as its seeds and its exclusions; named so that the two calls read as
    // two questions rather than as one function taking four sets.
    IndexSet seeds;
    IndexSet excluded;
};

// Which of the three a read of a header phi arriving from `from` takes, or none at all - which is a
// reader neither merge dominates, and the reason a loop is refused.
enum class RotatedRead: U8 { Header, Body, Exit, Nowhere };

static RotatedRead rotatedRead(const RotationRegions& regions, LowerBlock* header, LowerBlock* from) {
    auto index = Size(from->index);

    if(from == header) return RotatedRead::Header;
    if(regions.inLoop[index] || regions.bodySide[index]) return RotatedRead::Body;
    if(regions.exitSide[index]) return RotatedRead::Exit;

    return RotatedRead::Nowhere;
}

static void buildRotationRegions(LowerBase base, LowerFunction& fun, const LoopInfo& loops,
                                 U32 headerIndex, LowerBlock* exit, RotationRegions& regions)
{
    auto count = fun.blocks.size();
    auto exitIndex = Size(exit->index);

    regions.inLoop.reset(count);
    for(auto o: fun.blocks.contents(base)) {
        auto block = base[o];
        if(loops.contains(headerIndex, block->index)) regions.inLoop.set(Size(block->index), true);
    }

    // The body's side. The header's own exit arm is held out of it, and that is the whole of the care
    // needed: every one of its predecessors is in the loop too, so it would join on the rule above -
    // and it is precisely the block the rotation gives a *new* predecessor to, the preheader's guard,
    // whose purpose is to carry the zero-iteration answer.
    regions.excluded.reset(count);
    regions.excluded.set(exitIndex, true);
    collectReachedOnlyFrom(base, fun, regions.inLoop, regions.excluded, regions.bodySide);

    // And the exit's, which is that arm and everything it is in turn the only way into. Excluding
    // what the body's side already claimed is not a tie-break: a block in both would be one the loop
    // and the exit are each the only way into, which is a block with no way in at all.
    regions.seeds.reset(count);
    regions.seeds.set(exitIndex, true);

    regions.excluded.reset(count);
    regions.excluded.unionWith(regions.inLoop);
    regions.excluded.unionWith(regions.bodySide);
    collectReachedOnlyFrom(base, fun, regions.seeds, regions.excluded, regions.exitSide);
    regions.exitSide.set(exitIndex, true);
}

static bool terminalExit(LowerBase base, const LoopInfo& loops, U32 headerIndex, LowerBlock* block) {
    if(block->outgoing[0] || block->outgoing[1]) return false;

    auto entered = true;
    for(auto p: block->incoming.contents(base)) {
        if(!loops.contains(headerIndex, base[p]->index)) { entered = false; break; }
    }

    if(entered) return true;

    // A phi is a reader on an *edge* rather than in a block, so one here is refused outright rather
    // than asked about - the alternative it takes from a loop predecessor is a loop value read on a
    // path this has just decided not to reason about.
    if(block->phis.size()) return false;

    for(auto i: block->instructions.contents(base)) {
        if(readsLoopValue(base, loops, headerIndex, base[i])) return false;
    }

    return !readsLoopValue(base, loops, headerIndex, base[block->terminator]);
}

static Maybe<RotatableLoop> rotatableLoop(LowerBase base, LowerFunction& fun, const LoopInfo& loops,
                                          LowerBlock* header, RotationRegions& regions)
{
    if(base[header->terminator]->kind != LowerInst::Je) return Nothing();

    auto index = header->index;
    auto first = base[header->outgoing[0]];
    auto second = base[header->outgoing[1]];

    // Exactly one arm may leave. A header that branches within the loop is not the block the loop is
    // left from, and one whose arms both leave is not a loop this pass can read.
    auto firstStays = loops.contains(index, first->index);
    if(firstStays == loops.contains(index, second->index)) return Nothing();

    RotatableLoop loop {};
    loop.header = header;
    loop.body = firstStays ? first : second;
    loop.exit = firstStays ? second : first;

    if(loop.body == header) return Nothing();
    if(header->incoming.size() != 2) return Nothing();

    auto a = base[header->incoming.get(base, 0)];
    auto b = base[header->incoming.get(base, 1)];

    auto aStays = loops.contains(index, a->index);
    if(aStays == loops.contains(index, b->index)) return Nothing();

    loop.latch = aStays ? a : b;
    loop.pre = aStays ? b : a;

    /*
     * The preheader has to be a block whose whole purpose is to enter the loop, since its jump is
     * what becomes the guard; and the two blocks that gain the preheader as a predecessor have to
     * have had only the header, or a phi in either would need alternatives this cannot supply.
     *
     * **Counted, the three are one refusal** - §44 of test/bench/findings.md. Over the 233
     * `test/resolve` programs 149 loops rotate, and of the three conditions here the first two refuse
     * *nothing at all*: no loop in the corpus or the suite has a preheader that is not a plain jump,
     * and none has a body reached by more than one edge. The third refuses 37, and 5 over the
     * benchmark corpus.
     *
     * **And generalizing the third was built and measured out.** Twenty of those 37 are the case that
     * can be answered - every extra edge into the exit leaves the *loop*, so the value a header phi
     * had on it is the iteration's own and the merge can take the body's phi for it - and one of the
     * five is. Built, it is +19 bytes over the 184 executables for no measurable time, which is the
     * verdict §29 reached for three other relaxations of this same pass: the rotation trades a jump
     * per iteration for a copy of the header test in the preheader, and a loop that reaches this
     * condition is one where that trade has already stopped paying. The other 17 cannot be answered
     * at all - an edge into the exit from *outside* the loop means the header does not dominate it,
     * and there is no value for the merge to carry.
     *
     * **§58.4 asked it of what the merge is for instead, and measured that out too.** What an extra
     * predecessor actually breaks is one thing: the merge built in the exit block is a two-source
     * phi carrying the guard's zero-iteration answer and the last iteration's, and a block with
     * three ways in cannot hold one - so where nothing reads a header phi on the exit's side there
     * is no such phi to build and the count stops mattering. That is not the twenty above; it is the
     * *abort arm*, whose predecessors are one per bounds check in the function once
     * `mergeIdenticalExits` has made the copies one block. Built, together with the reader relaxation
     * below it, it is **+158 bytes over the 186 `test/resolve` executables and no measurable time on
     * the corpus**, and it changes not one byte of the sixteen benchmark programs on its own. Third
     * relaxation of this condition to be built and taken back out; the condition is where it is
     * because the loops it refuses are the ones whose trade has already stopped paying.
     */
    if(base[loop.pre->terminator]->kind != LowerInst::Jmp) return Nothing();
    if(loop.body->incoming.size() != 1) return Nothing();
    if(loop.exit->incoming.size() != 1) return Nothing();

    /*
     * §48.1 Every reader of a header phi has to have one of the two merges dominating it.
     *
     * This used to be asked of the *exits* - the header had to be the only way out, or every other
     * way out had to go nowhere - which is a sufficient condition for the real one and refuses a
     * search loop whose found-arm carries on into the function's common return. The real one is
     * asked here instead, of the readers, because a reader is what needs a value and an exit is only
     * how one gets there.
     *
     * A reader in neither region is a read the rotation has nothing to point at, and the loop is
     * refused. `Iter.firstOverOrCount` is the shape: two early exits converging on a block that the
     * header's exit arm also falls into, so the count is read where neither merge dominates.
     */
    buildRotationRegions(base, fun, loops, index, loop.exit, regions);

    for(auto p: header->phis.contents(base)) {
        auto phi = base[p];

        for(auto u: phi->result.uses.contents(base)) {
            auto user = base[u];
            auto used = user->used();

            for(Size slot = 0; slot < used.size(); slot++) {
                if(base[used[slot]] != &phi->result) continue;

                // For a phi the read happens on the edge it names rather than in the block it sits
                // in, which is what lets a merge below the loop take the body's answer on one
                // alternative and the exit's on another.
                auto from = user->kind == LowerInst::Phi
                    ? base[((LowerInstPhi*)user)->sources()[slot]]
                    : base[user->block];

                if(rotatedRead(regions, header, from) == RotatedRead::Nowhere) return Nothing();
            }
        }
    }

    if(header->instructions.size() > kMaxRotatedHeader) return Nothing();

    /*
     * Read only where it is computed. A phi reader is refused whatever block it sits in, since what
     * it reads the value on is an edge - including the latch edge, which leaves the header by a
     * route the block of the reader does not show.
     *
     * **§58.4 relaxed this and measured it out, and the refusal is a measurement now rather than a
     * caution.** A bounds check makes the header's question and the body's work one instruction -
     * the `sext` that widens the index to compare it against the length is the `sext` the body
     * addresses with - so admitting a body-side reader is what lets a checked scan rotate. The
     * repair is real and was built: the reader takes a merge of the preheader's copy and the
     * header's, which is `RotatedPhi` with an instruction in the phi's place. `Sort`'s two partition
     * scans then rotate, and `Sort` measures **217.3 -> 223.4 ms** for it, layout controlled.
     *
     * The reason is the trade rather than the repair. A rotation buys one jump per *iteration* and
     * pays a copy of the header's test per *entry*, and a scan of the form `while xs[i] < pivot: i =
     * i + 1` is entered once per partition step and runs two or three times. Where the header is a
     * bounds check the copy is three instructions, so the loop has to run four times before the
     * trade breaks even. Nothing here knows a trip count, and the loops this rule refuses are
     * precisely the ones whose headers do enough work to be worth duplicating - which is the same
     * verdict §29 and §44 reached about three other relaxations of this pass.
     */
    for(auto i: header->instructions.contents(base)) {
        auto inst = base[i];
        if(!isRotatableHeaderInst(inst)) return Nothing();

        for(auto u: inst->created().ptr->uses.contents(base)) {
            auto user = base[u];
            if(user->kind == LowerInst::Phi || base[user->block] != header) return Nothing();
        }
    }

    return Just(loop);
}

static void rotateLoop(LowerBase base, LowerFunction& fun, const LoopInfo& loops,
                       const RotatableLoop& loop, const RotationRegions& regions)
{
    auto& arena = fun.arena;
    auto header = loop.header;
    auto pre = loop.pre;
    auto body = loop.body;
    auto exit = loop.exit;
    auto headerIndex = header->index;

    SmallArray<RotatedPhi, 8> phis;
    for(auto p: header->phis.contents(base)) {
        auto phi = base[p];
        phis.push(RotatedPhi { phi, base[phi->used()[phiSourceIndex(base, phi, pre)]], nullptr, nullptr, nullptr });
    }

    // What each value the header defines is called at the end of the preheader: a phi is whatever it
    // takes from that edge, and an instruction is the copy made below. Everything else is itself,
    // since a value the header could read already reached the preheader to get there.
    SmallArray<LowerValue*, kMaxRotatedHeader> originals;
    SmallArray<LowerValue*, kMaxRotatedHeader> clones;

    auto inPre = [&](LowerValue* v) -> LowerValue* {
        for(auto& r: phis) if(&r.header->result == v) return r.pre;
        for(Size i = 0; i < originals.size(); i++) if(originals[i] == v) return clones[i];
        return v;
    };

    for(auto i: header->instructions.contents(base)) {
        auto inst = base[i];
        auto clone = cloneHeaderInst(arena, inst);

        auto used = clone->used();
        for(Size k = 0; k < used.size(); k++) used[k] = inPre(base[used[k]]) - base;

        pre->addInst(base, clone);
        originals.push(inst->created().ptr);
        clones.push(clone->created().ptr);
    }

    auto je = (LowerInstJe*)base[header->terminator];
    auto guardCond = inPre(base[je->cond]);

    // The preheader stops entering the loop and starts deciding. Unwired by hand because addInst
    // records an edge rather than replacing one, and refuses a successor that already names it.
    pre->terminator = nullptr;
    pre->outgoing[0] = nullptr;
    pre->outgoing[1] = nullptr;

    for(Size i = 0; i < header->incoming.size(); i++) {
        if(base[header->incoming.get(base, i)] == pre) { header->incoming.remove(base, i); break; }
    }

    auto guard = new (arena) LowerInstJe(guardCond - base, je->then, je->otherwise);
    guard->likelihood[0] = je->likelihood[0];
    guard->likelihood[1] = je->likelihood[1];
    guard->source = je->source;
    pre->addInst(base, guard);

    // Whatever the two blocks already merged, they now merge one more edge of. The alternative the
    // preheader brings is what its own copy of the header computed, which is what the first
    // iteration - or the zero-iteration case, in the exit block - would have arrived with.
    for(auto block: { body, exit }) {
        for(Size i = 0; i < block->phis.size(); i++) {
            auto phi = base[block->phis.get(base, i)];
            auto slot = phiSourceIndex(base, phi, header);
            growPhi(base, fun, block, i, pre, inPre(base[phi->used()[slot]]));
        }
    }

    // The merges the header's own phis become. Built before they are filled in, because what the
    // rotated header holds is stated in terms of them: a phi advanced by the latch reads, at the
    // point the latch now sits, the body's phi rather than the header's.
    for(auto& r: phis) {
        r.body = makePhi(arena, r.header->result.type, 2);
        r.exit = makePhi(arena, r.header->result.type, 2);
    }

    auto inBody = [&](LowerValue* v) -> LowerValue* {
        for(auto& r: phis) if(&r.header->result == v) return &r.body->result;
        return v;
    };

    for(auto& r: phis) {
        r.hdr = inBody(base[r.header->used()[phiSourceIndex(base, r.header, loop.latch)]]);
    }

    for(auto& r: phis) {
        for(auto phi: { r.body, r.exit }) {
            auto used = phi->used();
            auto sources = phi->sources();

            used[0] = r.pre - base;
            sources[0] = pre - base;
            used[1] = r.hdr - base;
            sources[1] = header - base;
        }

        body->addInst(base, r.body);
        exit->addInst(base, r.exit);
    }

    /*
     * Everything that still names a header phi, pointed at whichever of the three answers its own
     * position asks for. What decides is where the read happens, which for a phi is the edge it
     * reads on and not the block it sits in:
     *
     *   in the header      the value the latch just produced, which is what the rotated header sees
     *   elsewhere in loop  the body's phi, which is now what the loop is entered through
     *   outside the loop   the exit's phi, which merges the guard's answer with the last iteration's
     *
     * A second exit reads on an edge that leaves the loop and is nevertheless the *body's* answer:
     * every way into such a block is from inside the loop, so the body dominates it, and the exit's
     * merge - which is what the guard's zero-iteration answer arrives through - is a value it was
     * never reached by. See `terminalExit` for the block that goes nowhere and
     * `collectLoopOnlyBlocks` for the one that carries on into code the loop is still the only way
     * into.
     */
    // One list for the walk, emptied per phi: the readers are snapshotted because the loop below
    // retargets them, and a list per phi is an allocation per phi - see InstChain.
    InstChain users;

    for(auto& r: phis) {
        auto value = &r.header->result;

        users.clear();
        for(auto u: value->uses.contents(base)) users.push(base[u]);

        for(auto user: users) {
            auto used = user->used();

            for(Size slot = 0; slot < used.size(); slot++) {
                if(base[used[slot]] != value) continue;

                auto from = user->kind == LowerInst::Phi
                    ? base[((LowerInstPhi*)user)->sources()[slot]]
                    : base[user->block];

                auto to = &r.exit->result;
                switch(rotatedRead(regions, header, from)) {
                    case RotatedRead::Header: to = r.hdr;             break;
                    case RotatedRead::Body:   to = &r.body->result;   break;
                    case RotatedRead::Exit:   break;

                    // Refused by `rotatableLoop`, which is what makes this unreachable rather than
                    // a case with an answer.
                    case RotatedRead::Nowhere:
                        assertTrue("a header phi is read where neither merge reaches" == nullptr);
                        break;
                }

                retargetOperand(base, user, slot, to);
            }
        }
    }

    for(auto& r: phis) {
        assertTrue(r.header->result.uses.size() == 0);
        detachOperands(base, (LowerInst*)r.header);
    }

    while(header->phis.size()) header->phis.remove(base, header->phis.size() - 1);

    // A loop-carried value that turns out to be read only inside the loop needs no exit merge, and
    // one only the header itself advances needs no body merge. Which of them are unread is not
    // known until the rewriting above has run, and dropping one can leave another unread in turn.
    bool dropped = true;
    while(dropped) {
        dropped = false;

        for(auto& r: phis) {
            dropped |= dropUnusedPhi(base, body, r.body);
            dropped |= dropUnusedPhi(base, exit, r.exit);
        }
    }
}

/*
 * §30.4 A preheader made rather than found.
 *
 * The rotation puts the header's copy of its own test at the end of the preheader, and there is one
 * block in a function that may not receive an instruction: the implicit entry. `runLegalizer` emits
 * the argument-home copies at index 0 and asserts nothing else is there, and `buildRanges` gives an
 * argument its range from outside every block - so an instruction placed there is a read in a block
 * that neither defines the argument nor has it live-in, which is a range that does not exist.
 *
 * A loop whose header the entry block enters directly is therefore unrotatable for a reason that has
 * nothing to do with the loop, and the answer is to stop finding the preheader and make one: an empty
 * block on the edge, which is the shape every other loop the lowering produces already has. It costs
 * nothing where the rotation then declines the loop anyway - `computeBypass` skips a block with no
 * instructions, no moves and an unconditional jump, and this is one until the rotation fills it.
 *
 * The rule is exact rather than a guess about which successor is a header. The entry block has one
 * successor and every block is reached through it, so a second predecessor of that successor is a
 * block the successor reaches: a back edge, and the successor a loop header.
 */
static LowerBlock* insertJumpPreheader(LowerBase base, LowerFunction& fun, LowerBlock* pred) {
    auto& arena = fun.arena;
    auto succ = base[pred->outgoing[0]];
    auto predOffset = pred - base;

    auto split = new (arena) LowerBlock(pred->fun, StringId(), BlockIndex(fun.blocks.size()));
    fun.blocks.push(arena, split - base);

    // Wired up by hand for the reason splitEdge gives: addInst would append the new block to `succ`'s
    // incoming list rather than replacing the entry the phis still name.
    auto jmp = (LowerInst*)new (arena) LowerInstJmp(succ - base);
    jmp->block = split - base;
    split->terminator = jmp - base;
    split->outgoing[0] = succ - base;
    split->incoming.push(arena, predOffset);

    auto old = (LowerInstJmp*)base[pred->terminator];
    assertTrue(old->kind == LowerInst::Jmp);
    old->then = split - base;
    pred->outgoing[0] = split - base;

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

    return split;
}

// Ahead of the loop analysis rather than inside it, because a block created afterwards is one the
// LoopInfo the rotation reads is not indexed for.
static void insertEntryPreheader(LowerBase base, LowerFunction& fun) {
    auto entry = base[fun.blocks.get(base, 0)];
    if(!entry->terminator || base[entry->terminator]->kind != LowerInst::Jmp) return;
    if(base[entry->outgoing[0]]->incoming.size() < 2) return;

    insertJumpPreheader(base, fun, entry);
}

/*
 * §32.3 The rotated header, folded into the latch it now sits behind.
 *
 * The rotation leaves the test in a block of its own, and that block has one predecessor - the latch
 * - and no phis, the header's merges having become the body's and the exit's. So the two are already
 * one straight line of instructions with a block boundary drawn across it, and the boundary costs a
 * real instruction: **the backend treats the flags as dead at every block edge** (`computeFlagsRead`,
 * and `flagsWindowEnd` which refuses a reader outside the comparison's own block), so a counter
 * decremented in the latch and tested in the header is `dec ; test ; jcc` where the decrement has
 * already answered in SF and ZF.
 *
 * Merging them is the ordinary single-predecessor fold and needs no new claim about flags at all:
 * `tryElideCompare` then finds the definition in its own block and elides the comparison the way it
 * already does everywhere else. That is why this is a CFG transform rather than a widening of the
 * flags window - the window's block-locality is what `emitAsLea` depends on (§31.3: a `lea` may
 * replace an `add` exactly because nothing across the edge can be reading the flags it drops), and
 * carrying flags over an edge would make that peephole silently wrong.
 *
 * ## What it costs
 *
 * For a one-block loop the latch *is* the body, so the merged block becomes its own successor and
 * its phis acquire a self-edge. That is a shape the rest of the pipeline already handles -
 * `normalizePhiEdges` splits an edge whose predecessor has two successors and whose successor has
 * phis, which is exactly this one - and it is why this runs where it does: before that pass, and
 * before anything that reasons about block indices.
 *
 * ## Two refusals
 *
 * The entry block never receives instructions - `runLegalizer` asserts it holds none, its terminator
 * being at index zero is what lets the argument copies be emitted ahead of the function - so a
 * header whose predecessor is the entry is left alone. And the predecessor has to end in a plain
 * `Jmp`: a conditional branch to this block means the other arm exists, which contradicts the single
 * incoming edge, and asking rather than assuming keeps the two facts from having to agree.
 */
static bool foldIntoPredecessor(LowerBase base, LowerFunction& fun, LowerBlock* block) {
    if(block->phis.size() != 0 || block->incoming.size() != 1 || !block->terminator) return false;

    auto pred = base[block->incoming.get(base, 0)];
    if(pred == block || pred == base[fun.blocks.get(base, 0)]) return false;

    auto jump = base[pred->terminator];
    if(jump->kind != LowerInst::Jmp || base[((LowerInstJmp*)jump)->then] != block) return false;

    auto& arena = fun.arena;

    // The jump is dropped rather than detached: a `Jmp` reads no value, so no use list mentions it,
    // and the edge it recorded is what the instructions below take over.
    pred->terminator = nullptr;
    pred->outgoing[0] = nullptr;
    pred->outgoing[1] = nullptr;

    // Moved rather than re-added: `addInst` would register every operand a second time, and these
    // instructions are already recorded as readers of what they read. Only the block changes.
    for(auto offset: block->instructions.contents(base)) {
        base[offset]->block = pred - base;
        pred->instructions.push(arena, offset);
    }

    pred->terminator = block->terminator;
    base[block->terminator]->block = pred - base;
    pred->outgoing[0] = block->outgoing[0];
    pred->outgoing[1] = block->outgoing[1];

    // Every successor now arrives from the predecessor, including the predecessor itself where the
    // loop was one block: a self-edge here is a phi alternative that names its own block, which is
    // what a merged latch and header is.
    for(auto successor: pred->outgoing) {
        if(!successor) continue;

        auto to = base[successor];
        for(Size i = 0; i < to->incoming.size(); i++) {
            if(to->incoming.get(base, i) == block - base) to->incoming.set(base, i, pred - base);
        }

        for(auto p: to->phis.contents(base)) {
            auto sources = base[p]->sources();
            for(Size i = 0; i < sources.size(); i++) {
                if(sources.ptr[i] == block - base) sources.ptr[i] = pred - base;
            }
        }
    }

    while(block->instructions.size()) block->instructions.remove(base, block->instructions.size() - 1);
    while(block->incoming.size()) block->incoming.remove(base, block->incoming.size() - 1);
    block->terminator = nullptr;
    block->outgoing[0] = nullptr;
    block->outgoing[1] = nullptr;

    for(Size i = 0; i < fun.blocks.size(); i++) {
        if(fun.blocks.get(base, i) != block - base) continue;

        fun.blocks.remove(base, i);
        break;
    }

    // Renumbering is not optional: `index` is a position in this list and half the analyses index
    // arrays by it.
    for(Size i = 0; i < fun.blocks.size(); i++) base[fun.blocks.get(base, i)]->index = BlockIndex(i);
    return true;
}

void rotateFunctionLoops(LowerBase base, LowerFunction& fun) {
    insertEntryPreheader(base, fun);

    auto loops = fun.buildLoops(base);

    // Snapshotted, because rotating one loop is what stops its header from being one. Which blocks a
    // loop *contains* is what everything below asks, and that is what rotation leaves alone: no
    // block is created or renumbered, and the body it moves the entry to was already a member.
    SmallArray<LowerPtr<LowerBlock>, 16> headers;
    for(auto o: fun.blocks.contents(base)) {
        if(loops.isHeader(base[o]->index)) headers.push(o);
    }

    // One set of regions for the function, filled per candidate: each is a walk over every block,
    // and a loop that is refused has already paid for it - see RotationRegions.
    RotationRegions regions;

    SmallArray<LowerPtr<LowerBlock>, 16> rotated;
    for(auto o: headers) {
        if(auto loop = rotatableLoop(base, fun, loops, base[o], regions)) {
            rotateLoop(base, fun, loops, loop.unwrap(), regions);
            rotated.push(o);
        }
    }

    // Behind every rotation rather than after each one: folding removes a block and renumbers, and
    // the `loops` above is indexed by the numbering it was built with.
    for(auto o: rotated) foldIntoPredecessor(base, fun, base[o]);
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

void orderBlocks(LowerBase base, LowerFunction& fun) {
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

    // Inline on the same terms as BlockList, which this is a permutation of.
    SmallArray<LowerPtr<LowerBlock>, 64> ordered;
    for(Size i = postorder.size(); i > 0; i--) {
        ordered.push(fun.blocks.get(base, postorder[i - 1]));
    }

    for(Size i = 0; i < ordered.size(); i++) {
        auto b = base[ordered[i]];

        fun.blocks.set(base, i, ordered[i]);
        b->index = BlockIndex(i);
    }
}
