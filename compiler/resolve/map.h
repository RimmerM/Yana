#pragma once

#include "expr.h"
#include "const.h"

/*
 * Constructor maps - a `match` over a value's top-level constructors whose every arm is compile-time
 * data.
 *
 * ## Why this is a resolver transform rather than an optimizer one
 *
 * A `match` becomes equality tests chained through blocks with a phi joining the results, which is
 * exactly the decision tree this replaces. So a property function written the obvious way -
 *
 *     fn arity(op: Op) -> Int = match op:
 *         Add -> 2
 *         Neg -> 1
 *         ...
 *
 * - is an O(N) comparison chain today, and the mapping it states lives in a control-flow shape that
 * nothing downstream has a reason to recognise. `Enum.fromValue` already makes the same observation
 * about a *membership* test and emits a range instead of one comparison per constructor
 * (intrinsic.cpp); this is the same recognition for the general case, and it is done here for the
 * same reason: beside `resolveMatch` the constructor patterns and their coverage are still explicit,
 * so nothing has to recover source-level intent from an arbitrary CFG. Lowering it immediately into
 * ordinary comparisons, arithmetic, immutable globals and indexed loads is what makes both targets
 * get the same answer and makes the guarantee survive `-no-opt`.
 *
 * The guarantee is deliberately narrow and deliberately stated: **an eligible map over three or more
 * constructors never remains a comparison chain.** It is not that every two-constructor match must
 * touch memory - see `chooseForm`, which documents the size rule.
 *
 * ## What the description is
 *
 * One normalized form - selector value, one static result per constructor - that four different
 * lowerings consume. It exists as a description rather than as an IR instruction on purpose: a real
 * `Switch` terminator is a separate project (resolve fixes `kMaxSuccessors` at two and lower embeds
 * exactly two outgoing blocks), and nothing here needs one, because every arm is data.
 *
 * The *selector* is defined carefully, because the two sums number themselves differently:
 *
 *  - a payload-free enumeration's selector is `Constructor::value`, which `@value(n)` pins and which
 *    may be negative or outside 32 bits;
 *  - any other sum's is the constructor index its discriminant holds.
 *
 * "Contiguous" below therefore always means contiguous *selector values*, never contiguous source
 * declarations.
 *
 * The domain is closed, and that is what makes every form here total: a well-typed value of a record
 * holds one of that record's own constructors and nothing else, so a wildcard arm is not an open
 * default but a fill for the constructors the alternatives did not name. There is no out-of-range
 * case to guard, on any of the five paths.
 */

// One constructor's answer. `selector` is the number the pivot carries for it - see the header.
struct ConstructorMapEntry {
    I64 selector = 0;
    ConstantPtr value = nullptr;
};

struct ConstructorMap {
    // The record being matched, and the type of the value the match produces. Every entry's constant
    // is of `resultType`; nothing here joins two types, which is why the recognition declines a
    // match whose arms disagree rather than widening them.
    TypePtr pivotType = nullptr;
    TypePtr resultType = nullptr;

    // The selector is `Constructor::value` rather than the constructor index - true exactly for a
    // payload-free enumeration, whose value *is* its number.
    bool byValue = false;

    // One per constructor of the record, in constructor order. Never sparse: the recognition
    // declines a match that does not cover every constructor, since the alternative is a default
    // that can never be taken.
    Array<ConstructorMapEntry> entries;
};

/*
 * Whether this `match` is one, and what it maps to - filled in only when the answer is yes.
 *
 * Reports nothing, ever. Every outcome here is "the ordinary path handles this", including the ones
 * that are mistakes: a match that is not exhaustive, an arm at the wrong type and a constructor that
 * does not belong are all diagnosed by `resolveMatch` in its own words, and a second opinion from a
 * recognizer would be a worse message about the same line.
 */
bool recognizeConstructorMap(ExprResolver& resolver, const ast::MatchExpr& match,
                             Buffer<ast::Pat> patterns, TypePtr pivotType, TypePtr target,
                             ConstructorMap& map);

/*
 * The map, as code. Null where the chosen size rule declines it, and the caller then resolves the
 * match the ordinary way - see chooseForm.
 *
 * `pivot` is the already-resolved value being matched. It is read at most once, for its selector,
 * and not at all where the map is constant.
 */
ModulePtr<Value> emitConstructorMap(ExprResolver& resolver, const ConstructorMap& map,
                                    ModulePtr<Value> pivot, LocationId source);
