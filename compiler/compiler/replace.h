#pragma once

#include "settings.h"

/*
 * Ordering a baseline's replacements - Design-Replacements.md §3.5, written once for both resolve
 * stages.
 *
 * What a stage knows and this does not is its constraints: whether one replacement's extra
 * constraints are a superset of another's, up to superclass entailment, is a question about that
 * stage's classes. Everything else - what "more specific" means, which pairs are errors, and the order
 * a call walks - is here, so the two stages cannot rank one set of declarations differently. That
 * would be one source compiling to two programs, which is the risk the bulk roles this replaced
 * carried as a comment.
 */

struct ReplacementConditions {
    // One per replacement: the selectors of its `@platform`/`@allocator` attributes and its file name.
    const SelectorCondition* selectors;

    // One per replacement: whether it states constraints beyond the baseline's.
    const bool* conditioned;

    Size count;
};

/*
 * What is wrong with a set of replacements, reported through `report` - each at most once per
 * replacement or pair, in declaration order.
 */
enum class ReplacementProblem: U8 {
    // Its condition holds wherever the baseline exists, so the baseline could never run. The fix is to
    // delete the baseline and drop the attribute.
    Unconditioned,

    // Two replacements with the same condition - one of them is never chosen.
    Duplicate,

    // Two replacements that can both hold and neither of which is more specific.
    Ambiguous,
};

/*
 * Orders `count` replacements most specific first, into `order`, and reports what §3.5 refuses.
 *
 * `superset(i, j)` answers whether replacement `i`'s extra constraints entail every one of `j`'s.
 * `baseline` is the baseline's own selectors: a replacement whose selectors hold wherever the
 * baseline's do and which states no extra constraint is unconditioned.
 *
 * **P is more specific than Q** where P's selectors imply Q's and P's constraints entail Q's, and the
 * two are not identical. Every pair must be ordered that way or have selectors no build satisfies
 * together - which is what makes the first candidate whose condition holds, in this order, the most
 * specific one that does. The order is a topological one: a replacement is placed only once nothing
 * left is more specific than it, and pairs that are disjoint keep their declaration order.
 */
template<class Superset, class Report>
void orderReplacements(const ReplacementConditions& set, const SelectorCondition& baseline,
                       Superset&& superset, Report&& report, U32* order) {
    auto identical = [&](Size i, Size j) {
        return selectorsImply(set.selectors[i], set.selectors[j]) &&
               selectorsImply(set.selectors[j], set.selectors[i]) && superset(i, j) && superset(j, i);
    };

    auto moreSpecific = [&](Size i, Size j) {
        return selectorsImply(set.selectors[i], set.selectors[j]) && superset(i, j) && !identical(i, j);
    };

    for(Size i = 0; i < set.count; i++) {
        if(!set.conditioned[i] && selectorsImply(baseline, set.selectors[i])) {
            report(ReplacementProblem::Unconditioned, i, i);
        }

        for(Size j = 0; j < i; j++) {
            if(identical(i, j)) {
                report(ReplacementProblem::Duplicate, j, i);
            } else if(!moreSpecific(i, j) && !moreSpecific(j, i) &&
                      !selectorsDisjoint(set.selectors[i], set.selectors[j])) {
                report(ReplacementProblem::Ambiguous, j, i);
            }
        }
    }

    // A selection sort over "nothing left outranks it". Quadratic, over a set that is one or two.
    Array<bool> placed;
    for(Size i = 0; i < set.count; i++) placed.push(false);

    for(Size at = 0; at < set.count; at++) {
        Size chosen = set.count;

        for(Size i = 0; i < set.count && chosen == set.count; i++) {
            if(placed[i]) continue;

            auto outranked = false;
            for(Size j = 0; j < set.count && !outranked; j++) {
                outranked = !placed[j] && j != i && moreSpecific(j, i);
            }

            if(!outranked) chosen = i;
        }

        // Only an ambiguous set - already reported - can leave nothing unoutranked, since a cycle of
        // "more specific" needs two replacements that each imply the other. Take the first left.
        if(chosen == set.count) {
            for(Size i = 0; i < set.count && chosen == set.count; i++) {
                if(!placed[i]) chosen = i;
            }
        }

        placed[chosen] = true;
        order[at] = U32(chosen);
    }
}
