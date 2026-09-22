#pragma once

#include "../resolve_mir/type.h"

/*
 * The bit placement of a run of narrow fields - the one rule a native layout and the JavaScript
 * packing table agree on.
 *
 * Shared as two templates over "something with `valueBits` and `unitBits`" rather than as a call
 * into the native layout, because the two targets ask it about different *values*: natively a
 * member's bits are its `Repr`'s `scalarBits`, and on JavaScript they are the width of the `Number`
 * the field is held as - and a `bigint` field, which is narrow natively, is not a candidate there at
 * all. What is shared is the rule, not the layout: the two targets pass different budgets (64
 * bits natively, 53 on JavaScript) and disagree about candidacy, so one record may be one word
 * natively and two `$pN` words there - which nothing observes, since a `.run.expect` sees values
 * and not placement. One rule is what keeps a layout readable from either side. The rejected
 * alternative was JSIR calling `ssa::reprOf` and reading the offsets back: that memo is
 * keyed on the type alone and stores the native answer on the record, so a JavaScript budget asked
 * through it would have been a native layout decided by whichever backend asked first.
 */
namespace ssa {

/*
 * The two lists the packing walk works in, both inline.
 *
 * A record with more than sixteen narrow fields is not one this bound is deciding anything about;
 * the point is that the ordinary record - a handful of fields, laid out once per declaration - never
 * reaches the heap for either of them.
 */
using PackOrder = SmallArray<U16, 16>;
using PackOffsets = SmallArray<U32, 16>;

// Where a run of co-packed fields ended up. `span` is the bits it occupies including any gap the
// straddle rule left behind, since what a container needs to know is how wide the word has to be
// rather than how much of it is live; `count` is how many of the fields offered were placed.
struct PackedRun {
    U32 span = 0;
    U16 count = 0;
};

/*
 * The bit placement of a run of narrow fields.
 *
 * `order` is the field indices to try, from `from`, in the order they should be placed. The run
 * stops at the first field that does not fit the budget, and the caller places the rest another way.
 */
template<class Members>
PackedRun packBits(const Members& members, const PackOrder& order, Size from, U32 budget,
                   PackOffsets* offsets) {
    PackedRun run;

    for(auto at = from; at < order.size(); at++) {
        auto& member = members[order[at]];
        if(!member.valueBits) break;

        auto placed = run.span;

        // Bumped to the next unit boundary where it would otherwise cross one. Measured before the
        // budget check, since the bump is what decides whether the field still fits.
        auto unit = member.unitBits;
        if(unit && placed / unit != (placed + member.valueBits - 1) / unit) {
            placed = (placed + unit - 1) / unit * unit;
        }

        if(placed + member.valueBits > budget) break;

        if(offsets) offsets->push(placed);
        run.span = placed + member.valueBits;
        run.count++;
    }

    return run;
}

/*
 * The order narrow fields are packed in: widest first, and stable within a width.
 *
 * Widest first because the straddle rule wastes the *tail* of a unit rather than its head, so
 * placing the wide fields while the word is still empty leaves the leftover bits in one run at the
 * top where a niche can use them. `{d: @bits(4), f: @bits(4), a: Bool}` fills a byte with `d` and
 * `f` and starts a second one with `a`, where declaration order would split `f` across the two.
 *
 * Declaration order under a pin, which is what pinning the layout means.
 */
template<class Members>
void packOrder(const Members& members, PackOrder& into, bool pinned) {
    if(pinned) return;

    for(Size i = 1; i < into.size(); i++) {
        auto index = into[i];
        auto width = members[index].valueBits;
        auto at = i;

        while(at > 0 && members[into[at - 1]].valueBits < width) {
            into[at] = into[at - 1];
            at--;
        }

        into[at] = index;
    }
}

} // namespace ssa
