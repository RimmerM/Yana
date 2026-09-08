#include "transform_internal.h"

/*
 * The multi-way branch, spent - D3 slice 3.
 *
 * `LowerInst::Switch` reaches this backend as a selector, a sorted case list and a fallback, and
 * leaves it as one of three shapes. Every one of them is written out here, in ordinary lower IR,
 * *above* the whole selection pipeline: the comparisons become forms, the table read becomes an
 * addressing mode, the register allocator hands out the registers, and the only thing below this
 * file that has heard of a jump table is the two bytes of `jmp r64` and the relocation that fills
 * a cell. That is the whole reason this is a transform rather than an encoder - an encoder would
 * have had to reserve registers and re-derive an addressing mode that `selectAddressesAndLeas`
 * already knows how to build.
 *
 * ## The three forms, and what decides between them
 *
 * `test/bench/findings.md` §77 is the measurement, taken by compiling one program three times with
 * `-switch-form`. What it says is that the crossover between the chain and the table is the *same*
 * crossover in a predictable dispatch, an unpredictable one and in bytes - sixteen arms - which is
 * not what §76 expected, and it is why the arm count is the whole of the rule here rather than a
 * proxy for a predictability a compiler with no profile cannot know.
 *
 * `jmp r64` is two bytes; the four instructions in front of it are the address arithmetic, and
 * `selectAddressesAndLeas` folds two of them into one addressing mode.
 *
 *   chain          `cmp`/`je` per case, in case order, falling into the fallback. What every
 *                  multi-way branch was before this file existed, and still the answer below the
 *                  table's threshold and for every case set too sparse for one.
 *   binary search  `log2(n)` comparisons over the sorted case values, with a short chain at each
 *                  leaf. **Never chosen** - see `chooseSwitchForm`, where the numbers are.
 *   jump table     a placed table of anchor-relative four-byte cells, indexed by the selector and
 *                  jumped through. One indirect branch and one load, whatever the arm count.
 *
 * ## Why the jump table has no bounds *branch*
 *
 * The out-of-range selector is folded into the table instead: the index is clamped to one past the
 * span by a `cmp` and a `cmov`, and the cell there names the same block a gap does. So the dispatch
 * is branch-free up to the indirect jump, which is the whole of what a table is for - a bounds test
 * in front of it would be a second branch on the same unpredictable value, and a mispredict is
 * almost the entire cost of this dispatch.
 *
 * It also keeps the CFG simple, which matters more than the instruction: with a bounds branch the
 * fallback would be reached both from the test and from a gap cell, so it would be one block with
 * two edges from the same dispatch and a phi with nowhere to put the second alternative. One
 * block holding `jmp fallback` is named by every gap cell and by the overflow cell, and the
 * fallback keeps exactly the one predecessor it had.
 */

/*
 * When a jump table is taken, and the three numbers that decide it. §77 of test/bench/findings.md
 * is the measurement; the short version is that a table's cost does not depend on the arm count and
 * a chain's does, so the only question is where the two cross.
 *
 * **Sixteen cases.** At twelve the chain is ahead on both currencies (0.306 s against 0.350 for a
 * predictable selector, 114 bytes against 138); at sixteen the two are a dead heat (0.356 / 0.355,
 * 166 bytes against 174); from twenty up the table wins both and keeps winning. The threshold is
 * taken at the tie rather than at the win because the probe's arms are one instruction each, which
 * is the case that flatters a chain the most - a real `match` arm is a block, and every byte of it
 * sits between the compares.
 *
 * **A span of at most 7/4 of the case count.** Every value in the span costs a four-byte cell
 * whether a case names it or not, and that is the whole of what a sparse table costs: the dispatch
 * is the same four instructions either way. Measured, a chain arm is about twelve bytes of code and
 * a table arm about five plus its cell, so a table stops paying for itself somewhere past 1.75
 * cells per case. Written as a fraction because the slope is the whole of the number.
 *
 * **And a span of at most 1024**, which is not a measurement but a ceiling: a table is placed data,
 * and four kilobytes of it for one `match` is a page this compiler should not spend without being
 * asked to.
 */
static constexpr Size kJumpTableMin = 16;
static constexpr U64 kJumpTableSpreadNumerator = 7;
static constexpr U64 kJumpTableSpreadDenominator = 4;
static constexpr U64 kJumpTableMaxSpan = 1024;

// How many cases a leaf of the binary search tests linearly. Halving below this costs a block and a
// comparison to save at most one comparison, and the leaf's tests are the correctly-predicted kind.
static constexpr Size kSwitchLeafCases = 3;

// Four bytes, and the same four `repr/table.h` describes - a signed difference from the label the
// table is measured from. Here that label is the table itself, which is the one departure and is
// what makes the dispatch read one address rather than two.
static constexpr U32 kJumpTableCellBytes = 4;

/*
 * Which of the three a particular switch gets. One function, and every threshold above it.
 *
 * The span is computed in U64 over the *signed* case values, which is what makes a case set
 * straddling zero measure its real width rather than wrapping: the list is ascending, so the
 * difference of its two ends is the span whatever the signs are.
 */
static SwitchForm chooseSwitchForm(const CompileSettings& settings, LowerInstSwitch* sw) {
    // `-switch-form chain|search|table` is what a measurement compiles the same program three ways
    // with - see SwitchForm, and §77 of test/bench/findings.md for what it produced.
    if(settings.switchForm != SwitchForm::Auto) return settings.switchForm;

    auto count = Size(sw->caseCount);
    auto span = U64(sw->highestCase()) - U64(sw->lowestCase()) + 1;

    if(count >= kJumpTableMin &&
       span * kJumpTableSpreadDenominator <= U64(count) * kJumpTableSpreadNumerator &&
       span <= kJumpTableMaxSpan)
    {
        return SwitchForm::JumpTable;
    }

    /*
     * And otherwise the chain, which is the answer for a sparse case set as well as for a small one.
     *
     * **The binary search is never chosen, and that is a measurement rather than an oversight.** It
     * lost to the chain on both currencies at every size §77 measured - 2.26 s against 1.66 at
     * sixteen arms with an unpredictable selector, and 209 bytes against 166 - because its
     * comparisons are the branches that *cannot* be predicted: each one splits the remaining cases in
     * half, so each is taken about half the time, and log2(n) mispredicts is worse than one
     * mispredict plus n/2 compares the predictor gets right. The form is built and stays reachable
     * through `-switch-form search`, which is what took that measurement and what would re-take it on
     * a machine whose predictor is weaker; `test/x64/Switch.Search.lower` pins its output.
     */
    return SwitchForm::Chain;
}

// A phi in `target` naming `from` on the edge it takes its alternative from, renamed at `to`. Every
// form below moves the edges of the switch onto blocks it created, and this is what keeps the
// alternative that arrives on each one attached to the block it now arrives from. The count never
// changes, which is why no phi has to be rebuilt.
static void handOverEdge(LowerBase base, LowerBlock* target, LowerBlock* from, LowerBlock* to) {
    if(from == to) return;

    for(auto p: target->phis.contents(base)) {
        auto sources = base[p]->sources();
        for(Size i = 0; i < sources.length; i++) {
            if(sources.ptr[i] == from - base) sources.ptr[i] = to - base;
        }
    }
}

// The switch taken off its block, with every edge it recorded unwound. The instruction itself is
// left intact and readable - it is what the expansion is written from - and simply never reattached.
static LowerInstSwitch* takeSwitch(LowerBase base, LowerBlock* block) {
    auto sw = (LowerInstSwitch*)base[block->terminator];
    detach(base, (LowerInst*)sw);
    block->terminator = nullptr;

    for(auto successor: block->successors(base)) {
        auto to = base[successor];

        for(Size i = 0; i < to->incoming.size(); i++) {
            if(to->incoming.get(base, i) != block - base) continue;

            to->incoming.remove(base, i);
            break;
        }
    }

    block->clearSuccessors();
    return sw;
}

static LowerBlock* freshBlock(LowerBase base, LowerFunction& fun, LowerBlock* like) {
    auto block = fun.addBlock(base, StringId());
    block->source = like->source;
    return block;
}

/*
 * The chain: `cmp`/`je` per case, in case order, with the fallback last.
 *
 * The last test falls straight into the fallback rather than through a block holding one jump, which
 * is what the SSA lowering this replaces already did. Both edges of that last `je` name distinct
 * blocks because a switch's targets are pairwise distinct - see LowerBlock::outgoing.
 */
static void emitChain(LowerBase base, LowerFunction& fun, LowerBlock* origin, LowerBlock* start,
                      LowerValue* selector, LowerType type, Buffer<LowerSwitchCase> cases,
                      Size from, Size to, LowerBlock* fallback)
{
    auto& arena = fun.arena;
    auto current = start;

    for(Size i = from; i < to; i++) {
        auto target = base[cases[i].target];
        auto last = i + 1 == to;
        auto next = last ? fallback : freshBlock(base, fun, origin);

        auto number = current->addInst(base, new (arena) LowerImm(
            StringId(), type, U64(cases[i].value())));

        auto equal = current->addInst(base, new (arena) LowerInstCmp(
            StringId(), selector - base,
            ((LowerInstSingle*)number)->created().ptr - base, LowerCmp::eq));

        current->addInst(base, new (arena) LowerInstJe(
            ((LowerInstSingle*)equal)->created().ptr - base, target - base, next - base));

        handOverEdge(base, target, origin, current);
        if(last) handOverEdge(base, fallback, origin, current);

        current = next;
    }
}

/*
 * The binary search: a signed comparison against the middle case value per level, and a short chain
 * at each leaf.
 *
 * Signed, because the case list is sorted as `I64` and a search has to compare the way the list was
 * ordered or it looks in the wrong half. A selector the program means as unsigned reaches here with
 * the same bits and the same ordering, since a value outside the signed range is a case value
 * outside it too and the two agree about which is smaller.
 *
 * Explicit worklist rather than recursion: the depth is `log2` of the case count and would recurse
 * perfectly well, but the blocks have to be created as the levels are reached and a worklist says
 * that without the reader having to hold a stack frame in mind.
 */
static void emitBinarySearch(LowerBase base, LowerFunction& fun, LowerBlock* origin,
                             LowerValue* selector, LowerType type, Buffer<LowerSwitchCase> cases,
                             LowerBlock* fallback)
{
    struct Range { LowerBlock* block; Size from; Size to; };

    auto& arena = fun.arena;
    SmallArray<Range, 16> pending;
    pending.push(Range { origin, 0, cases.length });

    while(pending.size()) {
        auto range = pending.pop().unwrap();
        auto count = range.to - range.from;

        if(count <= kSwitchLeafCases) {
            emitChain(base, fun, origin, range.block, selector, type, cases,
                      range.from, range.to, fallback);
            continue;
        }

        auto mid = range.from + count / 2;

        auto number = range.block->addInst(base, new (arena) LowerImm(
            StringId(), type, U64(cases[mid].value())));

        auto less = range.block->addInst(base, new (arena) LowerInstCmp(
            StringId(), selector - base,
            ((LowerInstSingle*)number)->created().ptr - base, LowerCmp::ilt));

        auto below = freshBlock(base, fun, origin);
        auto above = freshBlock(base, fun, origin);

        range.block->addInst(base, new (arena) LowerInstJe(
            ((LowerInstSingle*)less)->created().ptr - base, below - base, above - base));

        pending.push(Range { below, range.from, mid });
        pending.push(Range { above, mid, range.to });
    }
}

/*
 * The table itself: one four-byte cell per value in the span, plus one for everything outside it.
 *
 * A cell holds `target - &table`, which is a difference between two offsets inside the image and so
 * is the same number wherever the image is mapped - the same statement `repr/table.h` makes, with
 * the table standing in for the module's image anchor. That substitution is what the dispatch is
 * paid in: the code reading a cell already holds the table's address, because it indexed with it.
 */
static LowerGlobal* buildJumpTable(Context& ctx, LowerModule& module, Size cells) {
    // `$jt$<n>`, counting up until the name is free. A jump table is never shared - two switches
    // with the same case values still name their own blocks - so this interns nothing and only has
    // to be unique.
    static const char digits[] = "0123456789";

    // `$jt$0000`, written out at a fixed width for `pooledConstant`'s reason: a formatted name is a
    // name of varying length, and two of those can be prefixes of each other.
    char text[] = "$jt$0000";
    auto length = sizeof(text) - 1;
    StringId name {};

    for(U32 n = 0; ; n++) {
        text[4] = digits[(n / 1000) % 10];
        text[5] = digits[(n / 100) % 10];
        text[6] = digits[(n / 10) % 10];
        text[7] = digits[n % 10];

        name = Context::nameHash(text, length);
        if(!module.globals.getValue(name)) break;
    }

    // Copied into the arena because `addUnqualifiedName` keeps the pointer it is handed and this one
    // is on the stack - the trap `pooledConstant` records.
    auto stored = (char*)module.arena.alloc(length);
    copyMem(text, stored, length);
    ctx.addUnqualifiedName(stored, length);

    auto global = new (module.arena) LowerGlobal(name);
    auto bytes = cells * kJumpTableCellBytes;
    auto contents = (U8*)module.arena.alloc(bytes);

    setMem(contents, bytes, 0);
    global->initialContents = { contents, bytes };

    module.globals.add(name, global - *module.arena);
    module.globalOrder.push(global - *module.arena);
    return global;
}

/*
 * The jump table, and the branch-free clamp in front of it.
 *
 *     %norm = selector - low
 *     %over = %norm >=u span             ; unsigned, so a selector below `low` wraps into it
 *     %idx  = %over ? span : %norm       ; a cmov, and the overflow cell is the one past the span
 *     %tab  = &table
 *     %cell = (I64)*(I32*)(%tab + %idx * 4)
 *     jmp %tab + %cell
 *
 * Every one of those but the last is ordinary IR, which is the point: `selectAddressesAndLeas` folds
 * the index arithmetic into the load's addressing mode and the allocator places the two registers.
 */
static void emitJumpTable(Context& ctx, LowerBase base, LowerFunction& fun, LowerBlock* origin,
                          LowerValue* selector, LowerType type, Buffer<LowerSwitchCase> cases,
                          LowerBlock* fallback)
{
    auto& module = *fun.module;
    auto& arena = fun.arena;

    auto low = cases[0].value();
    auto span = Size(U64(cases[cases.length - 1].value()) - U64(low) + 1);

    // The block every gap cell and the overflow cell name. It holds one jump, and `emitsNothing` in
    // gen.cpp declines to skip it for exactly that reason: a cell is a byte offset written after the
    // layout is settled, so there is no branch on the way in to retarget at what it fell through to.
    auto gap = freshBlock(base, fun, origin);

    auto table = buildJumpTable(ctx, module, span + 1);

    // The subtraction is skipped where the lowest case is zero, which is every enumeration that was
    // not pinned. Nothing below this file would remove it: the lower IR's constant folding is a tier
    // above the backend and has already run by the time a switch is expanded.
    auto normValue = selector;

    if(low != 0) {
        auto normImm = origin->addInst(base, new (arena) LowerImm(StringId(), type, U64(low)));
        auto norm = origin->addInst(base, new (arena) LowerInstBinary(
            StringId(), type, selector - base,
            ((LowerInstSingle*)normImm)->created().ptr - base, LowerInst::Sub));

        normValue = ((LowerInstSingle*)norm)->created().ptr;
    }

    auto spanImm = origin->addInst(base, new (arena) LowerImm(StringId(), type, U64(span)));

    auto over = origin->addInst(base, new (arena) LowerInstCmp(
        StringId(), normValue - base,
        ((LowerInstSingle*)spanImm)->created().ptr - base, LowerCmp::ge));

    auto index = origin->addInst(base, new (arena) LowerInstSelect(
        StringId(),
        ((LowerInstSingle*)spanImm)->created().ptr - base,
        normValue - base,
        ((LowerInstSingle*)over)->created().ptr - base, type));

    auto indexValue = ((LowerInstSingle*)index)->created().ptr;

    // The index is an address component and so has to be the machine's word. Unsigned in both
    // directions: the clamp above put it in `0..span`, so nothing is being sign-extended.
    if(type != LowerType::Int64) {
        auto wide = origin->addInst(base, new (arena) LowerInstCast(
            StringId(), LowerType::Int64, indexValue - base, false, false));

        indexValue = ((LowerInstSingle*)wide)->created().ptr;
    }

    auto scale = origin->addInst(base, new (arena) LowerImm(
        StringId(), LowerType::Int64, U64(kJumpTableCellBytes)));

    auto offset = origin->addInst(base, new (arena) LowerInstBinary(
        StringId(), LowerType::Int64, indexValue - base,
        ((LowerInstSingle*)scale)->created().ptr - base, LowerInst::Mul));

    auto address = origin->addInst(base, new (arena) LowerInstGlobal(StringId(), table - *module.arena));
    auto addressValue = ((LowerInstSingle*)address)->created().ptr;

    auto cellAddress = origin->addInst(base, new (arena) LowerInstBinary(
        StringId(), LowerType::Pointer, addressValue - base,
        ((LowerInstSingle*)offset)->created().ptr - base, LowerInst::Add));

    auto cell = origin->addInst(base, new (arena) LowerInstLoad(
        ((LowerInstSingle*)cellAddress)->created().ptr - base, StringId(),
        LowerType::Int64, kJumpTableCellBytes, true));

    auto destination = origin->addInst(base, new (arena) LowerInstBinary(
        StringId(), LowerType::Pointer, addressValue - base,
        ((LowerInstSingle*)cell)->created().ptr - base, LowerInst::Add));

    // The successors, in cell order with the gap block last, and each named once - `outgoing` is
    // duplicate-free for every terminator and a table is where repeats would otherwise arrive.
    auto jump = makeJumpTable(arena, ((LowerInstSingle*)destination)->created().ptr - base,
                              table - *module.arena, U32(cases.length + 1));

    auto targets = jump->targets();
    for(Size i = 0; i < cases.length; i++) targets[i] = cases[i].target;
    targets[cases.length] = gap - base;

    origin->addInst(base, (LowerInst*)jump);

    // The cells. Every value in the span that no case names, and the one past the end that the clamp
    // sends everything outside it to, name the gap block. One walk in step with the case list rather
    // than a search per cell, since both are ascending.
    Size next = 0;

    for(Size i = 0; i <= span; i++) {
        auto target = gap - base;

        if(next < cases.length && U64(cases[next].value()) - U64(low) == U64(i)) {
            target = cases[next].target;
            next++;
        }

        table->relocations.push(arena, LowerDataRelocation {
            .offset = U32(i * kJumpTableCellBytes),
            .anchorRelative = true,
            .block = target,
            .anchor = table - *module.arena,
        });
    }

    gap->addInst(base, new (arena) LowerInstJmp(fallback - base));

    for(auto& item: cases) handOverEdge(base, base[item.target], origin, origin);
    handOverEdge(base, fallback, origin, gap);
}

/*
 * Every switch in the function, spent.
 *
 * First in the pipeline, so that nothing below it ever sees a terminator with a case list: the loop
 * rotation, the block ordering, the edge splitting and the allocator all read the CFG this leaves,
 * and each of them is written against branches and jumps.
 *
 * Expects: nothing. Establishes: no `LowerInst::Switch` in the function. Mutates: the block list,
 * the CFG and the module's globals. Invalidates: block indices.
 */
void expandSwitches(Context& ctx, LowerBase base, LowerFunction& fun) {
    // Snapshotted, because every form below appends blocks and a fresh one never holds a switch.
    SmallArray<LowerPtr<LowerBlock>, 64> original;
    for(auto b: fun.blocks.contents(base)) original.push(b);

    auto expanded = false;

    for(auto offset: original) {
        auto block = base[offset];
        if(!block->terminator || base[block->terminator]->kind != LowerInst::Switch) continue;

        auto sw = (LowerInstSwitch*)base[block->terminator];
        auto form = chooseSwitchForm(ctx.settings, sw);
        auto selector = base[sw->selector];
        auto type = selector->type;

        // Read before the edges are unwound: `takeSwitch` leaves the instruction intact, but the
        // blocks it names are what the forms below are written from.
        SmallArray<LowerSwitchCase, 32> cases;
        for(auto& item: sw->cases()) cases.push(item);
        auto fallback = base[sw->fallback];

        takeSwitch(base, block);
        expanded = true;

        Buffer<LowerSwitchCase> list { cases.pointer(), cases.size() };

        switch(form) {
            case SwitchForm::Auto:
            case SwitchForm::Chain:
                emitChain(base, fun, block, block, selector, type, list, 0, list.length, fallback);
                break;
            case SwitchForm::BinarySearch:
                emitBinarySearch(base, fun, block, selector, type, list, fallback);
                break;
            case SwitchForm::JumpTable:
                emitJumpTable(ctx, base, fun, block, selector, type, list, fallback);
                break;
        }
    }

    // `addBlock` appends, so the indices the new blocks were given are already their positions; what
    // is not true afterwards is anything a caller cached about the old list.
    if(expanded) {
        for(Size i = 0; i < fun.blocks.size(); i++) base[fun.blocks.get(base, i)]->index = BlockIndex(i);
    }
}
