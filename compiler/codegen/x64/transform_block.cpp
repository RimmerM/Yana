#include "transform_internal.h"
#include "../../compiler/settings.h"

/*
 * Block operations with a compile-time size, written out as ordinary loads and stores.
 *
 * `rep movsb` and `rep stosb` have a **flat** startup of about thirty cycles: a copy of one byte and
 * a copy of a hundred and twenty-eight cost the same. So every block operation short enough to be
 * written out as transfers is cheaper written out, and the only question is where "short enough"
 * stops - which is `BlockExpansion` in target.h, and the one decision in this backend that trades
 * size for speed rather than being a straight win.
 *
 * ## Why this is a pass and not an encoding
 *
 * It used to be an encoding: a pseudo that read the byte count out of the IR at emission time and
 * wrote that many `mov`s through a fixed scratch register the form reserved as a clobber. Everything
 * wrong with that shape came from the same root - the transfers did not exist as *values* until
 * after every pass that could have done something with them:
 *
 *  - **The scratch register was reserved rather than allocated.** One register carried every step,
 *    so the expansion could not use two, and the form had to declare a clobber that kept a live
 *    value out of that register at every block operation whether or not the expansion wanted it.
 *    Widening the copy to vectors meant a *second* reserved register and a second pair of forms to
 *    say which of the two a given count would use.
 *  - **The addresses were computed by the encoder.** Each step wrote `[base + offset]` directly, so
 *    the transfers could not be folded, moved, or scheduled - and `selectAddressesAndLeas`, which
 *    exists to turn exactly that arithmetic into an addressing mode, never saw them.
 *  - **The byte count was an operand no form could describe.** It is not carried in any encoded
 *    instruction, so it had to be `folded()` in some forms and a register in others, and the
 *    immediate peephole needed a special case to agree with the selection about which. That whole
 *    mechanism - `isUnrolledCount`, `selectBlockOpEncoding`, the `isUnrolled` flag on the
 *    instruction, four forms where two would do - existed to say something the IR can say for
 *    itself once the transfers are instructions.
 *  - **And a vector step dirtied an upper half no value in the IR mentioned**, so
 *    `moduleDirtiesUpperHalves` had to ask the byte count what the encoder was going to do.
 *
 * Written here instead, every one of those goes away: a step is a `load` and a `store` of an
 * ordinary SSA value, allocated by the allocator, with an address the address folding turns into a
 * ModRM. The instruction that is left - a block operation whose count is not a constant, or one too
 * long to be worth straight-lining - always takes the `rep` form, whose operands are ordinary fixed
 * registers.
 *
 * **In this backend and not in `compiler/lower`**, which is the other place it could go. The LLVM
 * path wants the `Copy` intact: `llvm.memcpy` is what its own pipeline is written to recognize, and
 * expanding here would hand it sixteen loads and stores to put back together.
 *
 * ## What is emitted
 *
 * **One width for the whole steps, and a ragged end that is one transfer either way.** The step is
 * the widest the policy and the count allow, and every whole step of it is emitted in ascending
 * order; what finishes a count that is not a whole number of them is `eachStep`'s question, and the
 * measurement behind it is written down there. Neither form ever touches a byte outside the region,
 * which is what makes both usable on a copy whose ends are somebody's exact allocation.
 *
 * **The steps may run in any order, because the regions are disjoint.** `copyMemory` is the
 * non-overlapping promise and `moveMemory` is the other one - the library's own, written over these
 * same transfers. Ascending is the order that reads best, not the order correctness needs.
 *
 * **A load and its store are emitted as a pair rather than in two groups.** Each pair's value dies
 * at its store, so no two are live at once and the allocator is free to give every pair the same
 * register - which costs nothing on any out-of-order part, since a load that only *writes* the
 * register a store just read is a write-after-read the renamer breaks. Emitting all the loads first
 * would hold `n / W` vector registers live across the whole expansion to buy the same thing.
 */

namespace {

/*
 * The fill's pattern, replicated to the width of one step.
 *
 * **`SetPattern` names a byte** - `setMemory(to, value: U8, count)`, and `genSetPattern` in the LLVM
 * backend truncates to `i8` before handing it to `llvm.memset`. The expansion has to replicate it,
 * and the encoding it replaced did not: it stored eight bytes of whatever register the pattern was
 * in, so `setMemory(p, 0xAB, 12)` wrote `AB 00 00 00 00 00 00 00 AB 00 00 00`. Nothing caught it
 * because nothing in the library ever passes a pattern that is not zero, where replicating is the
 * identity - see `test/lib/SetMemory.yana`, which is the fixture that would have.
 */
struct Pattern {
    // The byte, masked and available as a scalar - `Nothing` where it is not a constant.
    Maybe<U64> constant;

    // The byte as a value, masked to eight bits. Null until something asks for it.
    LowerValue* masked = nullptr;
};

// The byte repeated up the width of one general-register step.
U64 repeatByte(U64 byte, U32 width) {
    U64 out = 0;
    for(U32 i = 0; i < width; i++) out |= (byte & 0xff) << (i * 8);
    return out;
}

// The integer type a general-register step of this width moves its bytes as. Four bytes and below
// travel in a 32-bit register, which is what every narrow value in this IR is held in; the *access*
// is still the step's own width, which is what the load and store carry.
LowerType stepType(U32 width) {
    return width == 8 ? LowerType::Int64 : LowerType::Int32;
}

// And the type a step of this width moves its bytes as, whichever bank that is. Sixteen bytes and up
// is a vector, which is what makes the transfer the unaligned `movdqu`/`vmovdqu` this needs.
LowerType typeForStep(U32 width) {
    return width >= 16 ? vectorType(LowerLane::Int8, width) : stepType(width);
}

// The step width for a block operation of `bytes`: the largest power of two that is both at most the
// policy's step and at most the whole operation. Every whole step is this wide; only the ragged end
// may be narrower, and only where `eachStep` says so.

U32 stepWidth(U64 bytes, U32 widest) {
    auto step = widest;
    while(step > bytes) step >>= 1;
    return step;
}

// An address `count` bytes above `base`, or the base itself at zero. The `add` is left for
// `selectAddressesAndLeas` to fold into the access that reads it, which is the whole reason this
// pass runs above that one.
LowerValue* offsetAddress(Expansion& e, LowerValue* address, U64 offset) {
    if(offset == 0) return address;

    auto amount = e.integer(LowerType::Int64, offset);
    return e.binary(LowerInst::Add, LowerType::Pointer, address, amount);
}

/*
 * Every (offset, width) one expansion visits: the whole steps, and then the ragged end.
 *
 * **The ragged end is one step either way, and which of the two it is comes down to the
 * remainder.** A count that is not a whole number of steps can be finished in two ways:
 *
 *  - **Overlapping.** One more transfer of the *same* width placed at `bytes - width`, which
 *    rewrites bytes the step before it already wrote and finishes the ones it could not reach.
 *    Always exactly one step, whatever the remainder is.
 *  - **Narrowing.** Descending powers of two over the remainder, which is what a ladder of widths
 *    does: 8, then 4, then 2, then 1. One step where the remainder is a power of two, and up to
 *    three where it is not.
 *
 * Measured, neither is the answer. Twenty-eight bytes at a 16-byte step is one overlapping transfer
 * against 8 and 4, and the overlap is **10% faster**; thirty-six bytes is one 4-byte transfer either
 * way, and there the overlap is **7.6% slower** - a store that partially covers one still in the
 * store buffer is not free, and where it saves nothing that is all it does. Fifty-two repeats the
 * second result at 6% and forty-four repeats the first at 12%.
 *
 * So the rule is the one the measurements draw: **overlap only where it removes a step**, which is
 * exactly where the remainder is not a single power of two. `test/bench/blockcopy/findings.md` is
 * the standing measurement.
 */
template<class F>
void eachStep(U64 bytes, U32 width, F&& step) {
    U64 offset = 0;
    while(offset + width <= bytes) {
        step(offset, width);
        offset += width;
    }

    auto remainder = bytes - offset;
    if(remainder == 0) return;

    // A power of two is one narrowing transfer, which is smaller than a whole-width one and lands on
    // bytes nothing has written. Anything else would be two or three, and one overlapping transfer
    // beats them.
    if((remainder & (remainder - 1)) == 0) step(offset, U32(remainder));
    else step(bytes - width, width);
}

void expandCopy(Expansion& e, LowerInstCopy* copy, U64 bytes, U32 widest) {
    auto to = e.base[copy->to];
    auto from = e.base[copy->from];
    auto width = stepWidth(bytes, widest);

    // A vector step moves its bytes as `i8xW`, which is the type whose loads and stores are the
    // unaligned `movdqu`/`vmovdqu` this needs. Below sixteen bytes there is no vector to be had and
    // the bytes travel in a general register.
    auto type = typeForStep(width);

    eachStep(bytes, width, [&](U64 offset, U32 stepBytes) {
        // The narrowing tail moves its bytes at its own width, which may be below the vector line
        // even where every step above it was a vector one.
        auto stepValueType = stepBytes == width ? type : typeForStep(stepBytes);

        auto value = e.load(stepValueType, offsetAddress(e, from, offset), stepBytes);
        e.store(offsetAddress(e, to, offset), value, stepBytes);
    });
}

/*
 * The value one store of `width` bytes writes, which is the pattern repeated up to that width.
 *
 * A constant pattern becomes a constant - one immediate for a general step, and a splat
 * `poolVectorConstants` turns into a `.rodata` load (or `pxor`, for the zero every compiler-
 * generated fill uses) for a vector one. A pattern only known at run time is replicated from the
 * byte `Pattern::masked` holds: by the multiply that is `0x0101...` for a general step, and by the
 * machine's own broadcast for a vector one.
 */
LowerValue* patternOfWidth(Expansion& e, Pattern& pattern, U32 width) {
    auto type = typeForStep(width);

    if(width >= 16) {
        auto scalar = pattern.constant ? e.integer(LowerType::Int32, pattern.constant.unwrap())
                                       : pattern.masked;
        return e.splat(type, scalar);
    }

    if(pattern.constant) return e.integer(type, repeatByte(pattern.constant.unwrap(), width));

    // `IMul` and not `Mul`, which is the widening one: the low half of a product is the same either
    // way, and the signed form is the one with a two-operand encoding that keeps its result in one
    // register and can embed the factor. `Mul` would put the product in edx:eax and clobber a second
    // register to throw the top half away.
    //
    // At one byte there is nothing to replicate and the masked byte is the value.
    if(width == 1) return pattern.masked;

    auto factor = e.integer(type, repeatByte(1, width));
    return e.binary(LowerInst::IMul, type, pattern.masked, factor);
}

void expandSet(Expansion& e, LowerInstSetPattern* set, U64 bytes, U32 widest, Pattern& pattern) {
    auto to = e.base[set->to];
    auto width = stepWidth(bytes, widest);

    auto value = patternOfWidth(e, pattern, width);

    // The narrowing tail needs a value of its own: the pattern above is as wide as a whole step and
    // a store below that width cannot read it. Built lazily, since most counts have no tail at all
    // and every whole step shares the one value.
    LowerValue* tailValue = nullptr;

    eachStep(bytes, width, [&](U64 offset, U32 stepBytes) {
        if(stepBytes == width) {
            e.store(offsetAddress(e, to, offset), value, stepBytes);
            return;
        }

        if(!tailValue) tailValue = patternOfWidth(e, pattern, stepBytes);
        e.store(offsetAddress(e, to, offset), tailValue, stepBytes);
    });
}

} // namespace

/*
 * The pass.
 *
 * Both kinds in one walk, since they share the ceiling question, the step question and the ragged
 * end - and since a function holding one usually holds the other.
 */
void expandBlockOperations(Context& ctx, LowerBase base, LowerFunction& fun) {
    auto policy = x64BlockExpansionFor(ctx.settings);

    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        // Indexed and advanced by hand, because an expansion inserts in front of the operation and
        // removing it closes the gap that leaves - the same shape `expandBankConversions` has.
        for(Size i = 0; i < block->instructions.size(); ) {
            auto inst = base[block->instructions.get(base, i)];
            auto copy = inst->kind == LowerInst::Copy;
            auto set = inst->kind == LowerInst::SetPattern;

            if(!copy && !set) { i++; continue; }

            auto countValue = base[copy ? ((LowerInstCopy*)inst)->count
                                        : ((LowerInstSetPattern*)inst)->count];

            // A count only known at run time keeps the string instruction, whose operands are
            // ordinary fixed registers - there is nothing here to write out.
            if(countValue->inst()->kind != LowerInst::Imm) { i++; continue; }

            auto bytes = ((LowerImm*)countValue->inst())->i;
            if(bytes > (copy ? policy.copyLimit : policy.setLimit)) { i++; continue; }

            // A block operation of nothing, which the IR does produce - a zero-sized aggregate's
            // relocation glue is one. It is removed rather than expanded, and the walk stays where
            // it is because whatever followed has moved into this position.
            if(bytes == 0) {
                removeInst(base, inst);
                continue;
            }

            Expansion e { base, fun, block, i };

            if(copy) {
                expandCopy(e, (LowerInstCopy*)inst, bytes, policy.copyStep);
            } else {
                auto pattern = base[((LowerInstSetPattern*)inst)->pattern];
                Pattern replicated;

                if(pattern->inst()->kind == LowerInst::Imm) {
                    replicated.constant = Just(((LowerImm*)pattern->inst())->i & 0xff);
                } else {
                    // Masked once, above every store: `SetPattern` names a byte and its operand is
                    // an integer of whatever width the caller had - see the note on `Pattern`.
                    auto mask = e.integer(LowerType::Int32, 0xff);
                    replicated.masked = e.binary(LowerInst::And, LowerType::Int32, pattern, mask);
                }

                expandSet(e, (LowerInstSetPattern*)inst, bytes, policy.setStep, replicated);
            }

            removeInst(base, inst);

            // Past the whole expansion, which now occupies the positions the operation's own began
            // at - removing it from the end closed the gap.
            i = e.at;
        }
    }
}
