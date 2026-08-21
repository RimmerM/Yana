#include "transform_internal.h"

/*
 * The x64 transform pipeline.
 *
 * Everything between the lowering's output and register allocation happens here: the passes are
 * named, their order is stated once in kTransformPipeline, and each one's contract is written next
 * to it. The passes themselves live in the seven transform_*.cpp files beside this one - see
 * transform_internal.h, which is the map.
 */

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

            ArgLocationList locations;
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
 * The transform pipeline.
 *
 * The passes below used to be one function with the order expressed as the sequence of statements in
 * it, and the reasons for that order as comments between them. They are named passes now, with the
 * order stated once in kTransformPipeline and each pass's contract stated next to the pass.
 *
 * The order is not arbitrary and each step of it is load-bearing:
 *
 *   rotateLoops                changes the CFG, and so goes before every pass that reasons about a
 *                              position within it - and before liveness is ever built, since it both
 *                              creates and removes phis
 *   canonicalizeOperands       puts immediates where the later passes expect to find them, so that
 *                              nothing downstream has to check both sides of a commutative operation
 *   selectAddressesAndLeas     removes address arithmetic *before* liveness, which is the only point
 *                              at which removing it actually shortens an interval - and before the
 *                              immediate peephole, so that an immediate the fold leaves with no uses
 *                              is made implicit rather than materialized into a register nothing reads
 *   selectMemorySources        folds a load into the instruction that consumes it, which needs the
 *                              address above it to be an X86Address already
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

// Turns a loop tested at the top into one tested at the bottom, by making the preheader ask the
// header's question itself - see the loop-rotation comment above.
//
// First, and it has to be: it is the only pass here that changes the CFG other than by splitting an
// edge, and every pass below either reasons about an instruction's position within a block or reads
// the branch structure the layout is chosen from.
//
// Expects: the lowering's output, unmodified.  Establishes: no loop of the shape described above
// leaves its test at the top. Mutates: the CFG, the phis of four blocks, and the instruction list of
// the preheader. Invalidates: loops, dominators and every block-relative position.
static void rotateLoops(Context&, LowerBase base, LowerFunction& fun) {
    rotateFunctionLoops(base, fun);
}

// Moves operands into the canonical position for the passes below: an immediate onto the right-hand
// side of a commutative operation, so that nothing downstream has to look at both sides, and a
// floating-point `lt`/`le` exchanged into the `gt`/`ge` this machine can answer for a NaN, and a
// packed `gt`/`ge` exchanged the other way into the predicate `cmpps` has.
// Representation-neutral: no target register or encoding decision is made here.
//
// Expects: the lowering's output, unmodified.  Establishes: commutative immediates on the right, and
// no float comparison below. Mutates: operand order and the comparison an instruction carries.
// Invalidates: nothing.
static void canonicalizeOperands(Context&, LowerBase base, LowerFunction& fun) {
    forEachInst(base, fun, [&](LowerInst* inst, Size i) {
        trySwapOperands(base, inst);
        orderFloatCompare(base, inst);
        orderPackedCompare(base, inst);
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
static void selectAddressesAndLeas(Context&, LowerBase base, LowerFunction& fun) {
    foldAddresses(base, fun);
    foldLeas(base, fun);
}

// Folds a byte reversal and the access it reverses into one `movbe`, where the target has it.
//
// Below `selectAddressesAndLeas`, so that what the access inherits is the whole addressing mode
// rather than a pointer some `lea` had to materialize - the same order `selectMemorySources` is in,
// and for the same reason. Above `selectMachineInstructions`, which is where a value stops being
// purely semantic.
//
// Expects: addresses selected.  Establishes: no byte reversal reaches allocation beside an access
// that could have performed it. Mutates: the instruction lists and the affected use lists.
// Invalidates: instruction positions within a block.
static void selectByteSwapAccesses(Context&, LowerBase base, LowerFunction& fun) {
    selectByteSwapMemory(base, fun);
}

// Folds a load into the instruction that consumes it, where the encoding has a form that reads its
// operand out of memory: `add rax, [rdi + rcx*8]` in place of a load and an add.
//
// After the pass above rather than inside it, and the order is required both ways. The address the
// load reads has to be an X86Address already, so that the fold inherits the whole addressing mode
// rather than half of it; and the address folding asks the *opcode* which operand is an address
// (opcodeAddressOperand), an answer that is only stable while no ALU instruction has been moved onto
// a memory-source form.
//
// Expects: addresses selected.  Establishes: no load reaches allocation whose only reader could have
// read it out of memory itself. Mutates: the instruction lists, the operand order of a commutative
// operation, and the affected use lists. Invalidates: instruction positions within a block.
static void selectMemorySources(Context&, LowerBase base, LowerFunction& fun) {
    foldLoads(base, fun);
}

// Folds a load, an operation on it and the store of the result back to the same place into one
// memory-destination instruction: `add [out + i*4], edx` in place of three.
//
// **Above `selectAddressesAndLeas`**, which is the whole of where this may sit. What it produces is
// an instruction with an address operand, and that pass is what turns the pointer arithmetic under
// one into an addressing mode - so running below it would leave the update reading a pointer some
// `lea` had to materialize, which is the instruction this removed put back.
//
// Expects: canonical operands.  Establishes: no store reaches allocation whose value is an operation
// on a load of the same location. Mutates: the instruction lists and the affected use lists.
// Invalidates: instruction positions within a block.
static void selectStoreUpdates(Context&, LowerBase base, LowerFunction& fun) {
    foldStoreUpdates(base, fun);
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
// Implicit, every Copy/SetPattern has its encoding recorded, and every cast whose extension is a
// no-op is marked as one. Mutates: value flags, instruction annotations, and the order of the
// instructions a compare fold lifts out of its flag window. Invalidates: instruction positions
// within a block.
//
// In two sweeps, and the order between them is the point - load-bearing rather than tidy. Everything
// a peephole can decide about an instruction's *form* is decided first; only then does the compare
// folding walk its windows asking what writes the flags.
//
// Two things need that. Some of the answers a peephole can still change are conservative until it has
// run - an immediate is `xor r, r` until it is embedded - so a comparison looked at first would be
// told that instructions about to disappear stand in its way. And one is not conservative at all: a
// `cast` or `bitcast` of a constant zero becomes the `xor` that materializes it only *once* the
// constant is embedded, so a comparison looked at first would be told that an instruction about to
// start writing the flags does not. Nothing after this pass moves a form's flags effect, which is
// what makes the window the folding cleared still empty when the bytes are written.
static void selectMachineInstructions(Context&, LowerBase base, LowerFunction& fun) {
    forEachInst(base, fun, [&](LowerInst* inst, Size i) {
        if(inst->kind == LowerInst::Imm) {
            tryEmbedImm(base, (LowerImm*)inst);
        }

        if(inst->kind == LowerInst::Fun) {
            tryElideDirectCallee(base, (LowerInstFun*)inst);
        }

        if(inst->kind == LowerInst::Global) {
            tryFoldGlobalAddress(base, (LowerInstGlobal*)inst);
        }

        // After tryEmbedImm rather than in a sweep of its own: whether a constant source has been
        // taken out of its register is what decides whether a cast is a move at all, and an Imm is
        // reached before the instructions that read it.
        if(inst->kind == LowerInst::Cast) {
            trySkipCastExtend(base, (LowerInstCast*)inst);
        }
    });

    // Walked by index rather than through forEachInst, because a fold that lifts an instruction out
    // of its window moves the comparison down the list by exactly that many places. Skipping past
    // them is right as well as necessary: what was lifted is never itself a comparison.
    for(auto b: fun.blocks.contents(base)) {
        auto block = base[b];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::Cmp) continue;

            i += tryMergeCompare(base, (LowerInstCmp*)inst, i);
        }

        // Behind the loop, because a comparison this block ends on is the branch's condition and the
        // fold above is the better answer for it: that one removes the materialization as well.
        // What is left here is a branch on a value nothing compared.
        tryElideBranchTest(base, block);

        /*
         * And the mask scan, which is the same finding one block over: the guard and the scan it
         * guards are two computations of the emptiness of one movemask, and the machine answers both
         * with the scan's own flags.
         *
         * Here rather than beside `lowerVectorReductions`, which is where the shape is *built*,
         * because the fused branch reads flags that the instruction directly above it left - and
         * only this late is "directly above it" a statement no later pass can invalidate. It is the
         * same argument `tryBranchOnLiveCompare` makes about its window, arriving from the other
         * side: that one measures a window, this one leaves none.
         */
        fuseMaskScanIntoGuard(base, block);
    }
}

// Turns a call's stack-passed arguments into explicit stores into the outgoing argument area, placed
// as early as is safe - see the block comment on outgoing stack arguments above.
//
// Expects: machine instructions selected, so that an argument the passes above made implicit is
// already implicit when its location is decided.  Establishes: no call operand is passed on the
// stack; every one of them is an X86PushArg result instead. Mutates: the instruction lists and the
// affected use lists. Invalidates: instruction positions within a block.
static void lowerOutgoingStackArguments(Context&, LowerBase base, LowerFunction& fun) {
    insertStackArgs(base, fun, targetConstraints());
}

// Splits every edge on which a phi transfer needs an insertion point of its own.
//
// Expects: no pass that reasons about instruction positions left to run.  Establishes: no block with
// two successors has a successor with phis, so a phi copy at the end of a predecessor cannot run on
// a path that skips the phis. Mutates: the block list and the CFG. Invalidates: block indices.
static void normalizePhiEdges(Context&, LowerBase base, LowerFunction& fun) {
    splitPhiEdges(base, fun);
}

// Finds the loops and rewrites the block list into the reverse postorder that follows them and the
// branch probabilities - see the block-order comment above.
//
// Expects: the CFG in its final shape, since the edge probabilities it lays the blocks out by are
// read from it. Establishes: blocks in reverse postorder with the likely successor of each branch
// immediately behind it, `index` equal to list position, and `loopDepth` set. Mutates: the block
// list order and block metadata. Invalidates: nothing after it.
static void analyzeLoopsAndOrderBlocks(Context&, LowerBase base, LowerFunction& fun) {
    orderBlocks(base, fun);
}

/*
 * The sign mask a float negation needs, interned into the pool above.
 *
 * `xorps xmm, [rip + m]` is what a negation is on this machine, and it was not writable before the
 * pool existed: §13.8 records the old form as three instructions, a general register the form had to
 * declare as a clobber, and a bank crossing in each direction, taken because a sixteen-byte constant
 * had nowhere to live. That is now one instruction, no general register, and - the part that reaches
 * further than the negation itself - **no flags effect**, where `btc` clobbered them. A comparison's
 * fold window may now hold a negation.
 *
 * Sixteen bytes rather than four or eight because `xorps` reads its memory operand as a whole
 * register and faults on an unaligned one. `addGlobal` puts every pooled entry on a sixteen-byte
 * boundary, so the alignment is already right; the size is what keeps the read inside the entry.
 */
static void poolSignMasks(Context& ctx, LowerBase base, LowerFunction& fun, MachineFunction& machine) {
    forEachInst(base, fun, [&](LowerInst* inst, Size) {
        if(inst->kind != LowerInst::Neg) return;

        auto type = ((LowerInstUnary*)inst)->result.type;
        if(type == LowerType::Float64 && !machine.signMask64) {
            machine.signMask64 = pooledConstant(ctx, *fun.module, U64(1) << 63, 16);
        } else if(type == LowerType::Float32 && !machine.signMask32) {
            machine.signMask32 = pooledConstant(ctx, *fun.module, 0x8000000080000000ull, 16);
        }
    });
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
    void (*run)(Context& ctx, LowerBase base, LowerFunction& fun);

    // What holds once this pass has run, and holds for every pass after it.
    U32 establishes;

    /*
     * Whether this pass has anything to look for in a function with no packed value in it.
     *
     * Half the table is vector work, and the overwhelming majority of functions this backend
     * compiles - every one in the embedded library, for a start - contain not one packed type. Each
     * of those passes still walks every block and every instruction of every one of them to ask a
     * question whose answer is decided by the function's types. So the question is asked once, in
     * `transformFunction`, and the ten that read a packed type are skipped outright when it says no.
     *
     * The claim that makes this sound is that nothing above such a pass *creates* a packed value in
     * a function that had none: the vector work is all lowering of vectors that were already there.
     * That is checked rather than asserted in prose - see the debug check at the end of the pipeline.
     */
    bool vectorsOnly = false;

    // The one exception to that claim, and the reason it stays a claim rather than becoming a
    // caveat: `expandBlockOperations` turns a sixteen-byte block transfer into an `i8x16`, so the
    // question is re-asked after it. Nothing else in the table sets this.
    bool addsVectors = false;
};

static const TransformPass kTransformPipeline[] = {
    { "rotateLoops"_v,                 rotateLoops,                 0 },

    /*
     * Directly behind it, and above everything: what it produces is ordinary loads and stores, so
     * every pass below gets to treat them as such - `selectAddressesAndLeas` folds the offsets it
     * emits into addressing modes, `poolVectorConstants` turns a fill's constant splat into a
     * `.rodata` entry, and the allocator hands out the registers rather than a form reserving one.
     * That is the whole argument for it being a pass; see transform_block.cpp.
     *
     * It is also the one pass here that can put a packed value into a function that had none, which
     * is why `transformFunction` asks `functionHasVectors` again after it - see `vectorsOnly`.
     */
    { "expandBlockOperations"_v,       expandBlockOperations,       0, false, true },

    { "expandBankConversions"_v,   expandBankConversions,   0 },
    // After nothing, since what it reads is only the multiply-add itself. It used to have to run
    // before the two lane passes as well, the tree it builds ending in a lane extract each of them
    // might rewrite; both went with the sub-v2 machines that needed them.
    // Not vectors-only, which is worth saying because it sits between two passes that are: a fused
    // multiply-add is a *float* instruction and a scalar one is as much an Fma as a packed one, so a
    // machine without FMA3 needs this pass to reach a function with no packed value in it at all.
    { "expandFusedMultiplyAdd"_v,      expandFusedMultiplyAdd,      0 },
    // Beside it, and not vectors-only for its reason: a scalar `round` is as much a `Round` as a
    // packed one. Above every pass that rewrites a select, since the one it builds is an ordinary
    // compare-and-select from here down and nothing below needs to know where it came from.
    { "expandRoundAway"_v,             expandRoundAway,             0 },
    { "lowerVectorReductions"_v,       lowerVectorReductions,       0, true },

    /*
     * After the reduction, which is the other producer of a defined-at-zero bit scan - `firstSet`
     * off a movemask that fills its word. That one emits `CttzWidth` only where it has asserted
     * BMI1, so this finds nothing of its there and everything of `Value::TrailingZeros`' and
     * `Value::LeadingZeros`' below the level that has the instructions. Running it after rather
     * than before is what makes the coverage a fact about the order instead of about that pass.
     *
     * Above every pass that rewrites a select, for `expandRoundAway`'s reason: the one this builds
     * is an ordinary compare-and-select from here down and nothing below needs to know where it came
     * from. Not vectors-only - a bit count is integer work and most functions holding one hold no
     * packed value at all.
     */
    { "expandBitScans"_v,              expandBitScans,              0 },

    /*
     * Beside `expandBitScans` and for its reasons: what it produces is ordinary integer arithmetic
     * and an ordinary select, so it needs only to be above the pass that rewrites a select and above
     * every fold that would be worth running over what it emits.
     *
     * Above `selectBitOps` and unrelated to it. The permutation network this writes is full of
     * `x & ~m`, which that pass would turn into `andn` - but the network is only written where the
     * target has no BMI2, and the levels grant BMI1 and BMI2 together, so the two passes never see
     * each other's work. Not `vectorsOnly`: none of the three is a vector operation and all three
     * reach a function with no packed value in it.
     */
    { "expandBitOperations"_v,         expandBitOperations,         0 },

    /*
     * Above `poolVectorConstants`, which is what turns the mask it builds into a `.rodata` entry the
     * `andps` reads out of memory, and above the two passes below - both of which rewrite a select,
     * and this one has to see the absolute value's before either has taken it for something else.
     *
     * Nothing else constrains it: what it reads is a select over a float vector, and no pass above
     * it produces or consumes one.
     */
    { "expandVectorAbs"_v,             expandVectorAbs,             0, true },

    // **Above `poolVectorConstants`**, which is what the constants it reads have to survive: this
    // asks `constantVectorBytes` for the bytes of a `vsplat`/`vwithlane` chain, and that pass turns
    // one into a `.rodata` load. Above `selectPackedMinMax` as well, so that what the minimum is
    // handed is the load rather than the blend that was standing in front of it.
    { "foldConstantMasks"_v,           foldConstantMasks,           0, true },

    // After the reduction, every level of whose min/max tree is exactly the compare-and-select this
    // recognizes, and **before `biasUnsignedPackedCompares`**, which rewrites an unsigned comparison
    // into a signed one over two exclusive-ors: what reaches this has to be the relation the program
    // asked for, since the signedness of the comparison is what picks `pminsd` over `pminud`.
    { "selectPackedMinMax"_v,          selectPackedMinMax,          0, true },

    // After both passes that read a select for something else - the minimum takes the pair whose
    // arms are the compared values, and this takes what is left. **Above `poolVectorConstants`**,
    // for the reason `foldConstantMasks` is: the zero arm it recognizes is a `vsplat` chain
    // until that pass turns it into a `.rodata` load.
    { "selectMaskedVectors"_v,         selectMaskedVectors,         0, true },

    // After the reduction, whose unsigned minimum and maximum are comparisons this then biases, and
    // before canonicalizeOperands, which is what exchanges the signed relations it produces.
    { "biasUnsignedPackedCompares"_v,  biasUnsignedPackedCompares,  0, true },

    // Above `poolVectorConstants`, which is the whole of where this may sit - the count it reads is
    // a `vsplat` of a constant, and that pass turns one into a `.rodata` load.
    /*
     * **Above `unwrapVectorShiftCounts` and above `expandByteShifts`**, which is the whole of its
     * placement: what it emits is a pair of ordinary packed shifts, and those two passes are what
     * take a shared count's splat off one and expand an 8-bit lane's. Emitting the rotation's
     * arithmetic here means neither of them had to learn that a rotation exists.
     *
     * Vectors only, and the scalar pair is *not* expanded at all: `rol`/`ror` on a general register
     * are baseline x86, so the scalar case goes straight to a form.
     */
    { "expandVectorRotate"_v,          expandVectorRotate,          0, true },

    { "unwrapVectorShiftCounts"_v,     unwrapVectorShiftCounts,     0, true },

    /*
     * Directly behind it, and **above the two shift expansions**, which is the ordering that matters
     * here: this is what takes away the shift whose count is one per lane, so the two below may
     * assume the count is the scalar `unwrapVectorShiftCounts` left. They check anyway - the cost is
     * a comparison and the alternative is a silent miscompile - but the order is what makes the
     * check never fire.
     *
     * Below `makeDivisionTotal`, which is a *lower* pass and therefore below this whole file: the
     * guard a scalarized division depends on is already there.
     */
    { "scalarizeVectorLanes"_v,        scalarizeVectorLanes,        0, true },

    // Directly behind those, which is the whole of where these two may sit: what they read is the
    // shift's count as a *scalar*, which is what those passes leave, and what they build is a
    // constant splat, which `poolVectorConstants` below has to still be able to see as one.
    { "expandQuadwordSar"_v,           expandQuadwordSar,           0, true },
    { "expandByteShifts"_v,            expandByteShifts,            0, true },

    // The two multiplies the machine has no row for, which sit here for the same reason: what they
    // build is constant splats and shifts by a written-down count, so they have to be above
    // `poolVectorConstants` and are unaffected by everything between. Neither reads a shift, so
    // neither cares that the two passes above rewrite some.
    { "expandByteMul"_v,               expandByteMul,               0, true },
    { "expandQuadwordMul"_v,           expandQuadwordMul,           0, true },

    // Beside them: what it builds is the same widening product and two in-lane shuffles, and what it
    // reads is a `mulhi` that `strengthReduceFunction` wrote a whole tier above this.
    { "expandVectorMulHi"_v,           expandVectorMulHi,           0, true },


    /*
     * **Above `selectMemorySources`**, which is what `poolFloatConstants` argues: `foldLoads` runs
     * below, so a pooled constant with one reader lands in that reader's addressing mode rather than
     * in a register (§5.4.1's memory twin, and only under VEX - a legacy packed operation faults on
     * an unaligned memory operand, so at v2 the load stands).
     *
     * It also had to run before `lowerLaneInserts`, which rewrote every `vwithlane` of a constant
     * chain into a shift and a `pinsrw` pair and so hid the chain from this pass - `iota` came out
     * one instruction longer. That pass is gone: `pinsrd` is v2.
     */
    { "poolVectorConstants"_v,         poolVectorConstants,         0, true },

    // Below `poolVectorConstants` and not above it: what this builds is already a `.rodata` load, so
    // there is nothing for that pass to find - and putting it above would hand that pass an index
    // vector to walk for no reason. It has to be above `selectMemorySources` for the reason every
    // pass that builds a load does.
    { "lowerWideLanePermutes"_v,       lowerWideLanePermutes,       0, true },

    // Directly behind it, so that what is left standing as an instruction is the set this can move:
    // everything with more than one distinct lane became a `.rodata` load one line up, and a load
    // has an address that would have to travel with it.
    { "sinkVectorConstants"_v,         sinkVectorConstants,         0, true },

    { "canonicalizeOperands"_v,        canonicalizeOperands,        0 },
    { "selectStoreUpdates"_v,          selectStoreUpdates,          0 },

    /*
     * Below `selectStoreUpdates`, and that is the whole of where it may sit. `mask &= ~bit` is a
     * load, an `and` and a store to one place; the pass above turns those into `and [m], r`, which
     * is two instructions counting the complement, against three for an `andn` that has no
     * memory-destination form to be folded into. Taking the `and` out from under that pass would
     * lose the better of the two.
     *
     * Above `selectMemorySources`, so that a load feeding one of the four still folds into the
     * instruction that replaces the pair - the replacements carry memory twins of their own. And
     * above `selectMachineInstructions` for `selectSignExtends`' reason: the flags these stop
     * writing are half of what the rewrite is worth, and the compare folding is where that is spent.
     */
    { "selectBitOps"_v,                selectBitOps,                0 },

    /*
     * Below every pass that folds a constant shift, so that what is left standing as a pair is a
     * pair over a value: an `x << k >> k` whose operand is an immediate is a number, and the
     * peepholes in `selectMachineInstructions` still reduce one.
     *
     * Above `selectMemorySources`, which is what turns an operand this left in the frame into the
     * memory ModRM a `movsx` reads it out of directly, and above `selectMachineInstructions`, whose
     * flags window is the whole reason the flags effect is worth removing - a comparison whose
     * result was recomputed across one of these shifts can now stand.
     *
     * Nothing above it reads a scalar shift by a distance this large: `selectAddressesAndLeas` takes
     * one only at 1, 2 or 3, which is a scale rather than a width.
     */
    { "selectSignExtends"_v,           selectSignExtends,           0 },
    { "selectAddressesAndLeas"_v,      selectAddressesAndLeas,      0 },
    { "selectByteSwapAccesses"_v,      selectByteSwapAccesses,      0 },
    { "poolFloatConstants"_v,          poolFloatConstants,          0 },
    { "selectMemorySources"_v,         selectMemorySources,         0 },
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

/*
 * Whether any value in this function is a packed one - see TransformPass::vectorsOnly.
 *
 * Over every value the function owns rather than over its instruction lists, because a vector can
 * arrive as an argument or be merged by a phi without any instruction between them producing one.
 * The results are what is asked about and not the operands: every operand is some instruction's
 * result, an argument or a phi, and all three are visited here.
 */
static bool functionHasVectors(LowerBase base, LowerFunction& fun) {
    auto found = false;

    forEachOwnedInst(base, fun, [&](LowerInst* inst) {
        for(auto& created: inst->created()) {
            if(isVectorLike(created.type)) found = true;
        }
    });

    return found;
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
    // The narrowest point every path through this backend passes, which is why the target's feature
    // set is established here: form selection is asked about a form from a dozen places that have an
    // instruction and no settings, so the answer is process-wide rather than carried. See
    // targetFeatures in target.h.
    setTargetFeatures(x64FeaturesFor(ctx.settings));

    // Asked here because this is the first thing the backend does to a function and the question is
    // about the IR as it arrives - so a frame this backend cannot build is a diagnostic against the
    // program rather than something the frame builder discovers with the code half emitted. See
    // checkFrameSupported; the pipeline still runs, since a reported error stops emission anyway and
    // a half-transformed function is worse to reason about than a whole one.
    checkFrameSupported(ctx, base, fun, targetConstraints());

    // And the same question about the vector operations, at the same point and for the same reason -
    // see checkVectorSupported. It has to stand after setTargetFeatures, since which forms exist is
    // a function of the feature set this build claims.
    checkVectorSupported(ctx, base, fun);

    U32 established = 0;

    // Asked once, here, rather than discovered by each of the ten vector passes walking the whole
    // function to find nothing - see TransformPass::vectorsOnly.
    auto vectors = functionHasVectors(base, fun);

    for(auto& pass: kTransformPipeline) {
        if(pass.vectorsOnly && !vectors) continue;

        pass.run(ctx, base, fun);
        established |= pass.establishes;

        /*
         * And asked again after the one pass that can answer differently.
         *
         * `expandBlockOperations` is the only entry in the table that *creates* a packed value in a
         * function that had none - a sixteen-byte transfer is an `i8x16` - which is the claim the
         * skip above rests on and the debug check below states. Re-asking here rather than making
         * the pass report is what keeps the two agreeing: the check reads `functionHasVectors` and
         * so does this.
         */
        if(pass.addsVectors) vectors = functionHasVectors(base, fun);

        // Debug builds only - assertTrue compiles away entirely in a release build, taking the call
        // with it. Running between passes rather than once at the end is the point: it names the
        // pass that broke the invariant rather than the pipeline that ended up violating it.
        assertTrue(verifyTransformInvariants(ctx, base, fun, established | InvariantStructure));
    }

    // What the skip above assumes, stated where it can fail loudly: a function with no packed value
    // in it does not acquire one on the way through, so the ten passes that were skipped had nothing
    // to do rather than something they were not shown. Debug builds only, like every check here.
    assertTrue(vectors || !functionHasVectors(base, fun));

    // Beside the form selection rather than in the pipeline, for the same reason: it writes on the
    // MachineFunction instead of on the IR. Before it rather than after only so that the two facts a
    // negation needs - its form and its mask - are settled together.
    poolSignMasks(ctx, base, fun, machine);

    // Writes down what the passes above decided. Separate from the pipeline table because it
    // produces the MachineFunction rather than mutating the IR, and because it has to see every
    // instruction the passes above created.
    selectMachineForms(base, fun, machine);

    // The first of the boundary checks, at the boundary it belongs to: everything after this reads
    // the selection rather than the instructions, so a form that does not match the instruction it
    // was chosen for is a wrong answer nothing downstream can notice.
    assertTrue(verifySelection(ctx, base, fun, machine));
}
