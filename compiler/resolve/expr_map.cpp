/*
 * Constructor maps - see map.h, which is where the reasoning is.
 *
 * Three parts that meet at two descriptions. `recognizeConstructorMap` reads the alternatives and
 * answers whether they state a map; `chooseForm` decides which of five shapes that map is, and is
 * the whole of the policy; the emitters build the shape it named. Nothing is decided twice, which is
 * what lets recognition promise the caller that an accepted map will be emitted - the size rule is
 * consulted before the answer is given rather than discovered while building.
 */

#include "map.h"
#include "name.h"
#include "place.h"

namespace {

/*
 * Constant equality, structurally and bit-exactly.
 *
 * Bits rather than numbers is a requirement rather than a convenience: `-0.0` and `0.0` are the same
 * number and different values, and two NaNs with different payloads are neither. A comparison that
 * went through a printed or a converted form would collapse both pairs, and collapsing them is how a
 * table becomes a range test that answers the wrong one of two constants.
 */
bool sameConstant(ModuleBase local, ConstantPtr a, ConstantPtr b) {
    if(a == b) return true;
    if(!a || !b) return false;

    auto& lhs = *local[a];
    auto& rhs = *local[b];
    if(lhs.kind != rhs.kind || lhs.type != rhs.type) return false;

    switch(lhs.kind) {
        case ConstKind::Scalar:
            return lhs.bits == rhs.bits;
        case ConstKind::String:
            return lhs.text == rhs.text;
        case ConstKind::Address:
            return lhs.global == rhs.global;
        case ConstKind::Construct:
            if(lhs.index != rhs.index) return false;
            break;
        case ConstKind::Aggregate:
            break;
    }

    auto ours = lhs.children.contents(local);
    auto theirs = rhs.children.contents(local);
    if(ours.size() != theirs.size()) return false;

    for(Size i = 0; i < ours.size(); i++) {
        if(!sameConstant(local, ours[i], theirs[i])) return false;
    }

    return true;
}

// The payload-free sum behind a type, or null. An enumeration *is* its number, which is what makes
// its selector `Constructor::value` rather than a position - see map.h.
RecordType* enumRecordOf(GlobalBase global, TypePtr type) {
    if(!type || global[type]->kind != Type::Record) return nullptr;

    auto record = (RecordType*)global[type];
    return record->layout == RecordType::Enum ? record : nullptr;
}

/*
 * A scalar constant as the number it is, sign-extended from its own width.
 *
 * `ConstValue::bits` holds the *reduced* pattern - what the storage contains - so an `I8` of -1 is
 * `0xff` there. Every arithmetic decision below (an affine fit, an interval, a stride) is about the
 * number, and reading the pattern as one would make `-1` compare above every positive value.
 *
 * Declines a float and a pointer deliberately: a float's bits are not its value, and a pointer's
 * value is not known until the module is placed. Both reach the table instead, where the bits are
 * all that is wanted.
 */
bool constantNumber(GlobalBase global, ModuleBase local, ConstantPtr constant, I64& into) {
    if(!constant) return false;

    auto& value = *local[constant];
    if(value.kind != ConstKind::Scalar) return false;

    // A constructor's pinned number, which is what an enumeration's constant holds.
    if(enumRecordOf(global, value.type)) {
        into = I64(value.bits);
        return true;
    }

    if(global[value.type]->kind != Type::Int) return false;

    auto& integer = *(IntType*)global[value.type];
    auto width = integer.maxBits();

    if(width >= 64) {
        into = I64(value.bits);
        return true;
    }

    auto mask = (U64(1) << width) - 1;
    auto raw = value.bits & mask;
    auto negative = integer.isSigned && (raw & (U64(1) << (width - 1)));

    into = I64(negative ? (raw | ~mask) : raw);
    return true;
}

// Whether the arithmetic forms may produce this type at all: a number computed at the selector's
// width and then converted. A float is not one - its constant is a bit pattern rather than a number
// - and neither is a pointer, which has no value until the module is placed.
bool arithmeticResult(GlobalBase global, TypePtr type) {
    return global[type]->kind == Type::Int || enumRecordOf(global, type) != nullptr;
}

/*
 * Whether a value of this type may be *read out of a static table* at all.
 *
 * Two answers, and the second is the ownership restriction that decides most of what this feature
 * covers. Reading an element of a table produces a value, and a value with a teardown read out of
 * shared storage is a second owner of something the table still holds - so the lookup alone is not
 * enough for one, however well it lays out.
 *
 *  - A type resolve models in a register is unconditionally fine: the read *is* the copy.
 *  - A memory type is fine when it is `TrivialCopy`, which is exactly the statement "these bytes
 *    duplicated are a second independent value".
 *
 * `String` is neither and is admitted separately - see emitStringTable, and Implementation-String.md
 * part 9 for why a *literal* is the one non-trivial value this can serve.
 */
bool tableElement(Module& module, TypePtr type) {
    auto global = *module.types;
    if(global[type]->kind == Type::String) return module.program.stringLiteral != nullptr;
    if(!isMemoryType(global, type)) return true;

    auto ownership = ownershipOf(module, type);
    return ownership.trivialCopy && ownership.trivialSink && !ownership.needsTeardown();
}

/*
 * The pattern is one constructor of `record`, tests nothing else, and binds nothing.
 *
 * Everything a pattern can do beyond naming a constructor is a reason to decline, and the list is
 * the eligibility rule read from the other side: an `as` binding names the pivot, a payload pattern
 * other than `_` tests or moves something, and a `Sink` binding takes the payload out. None of those
 * is work a table lookup performs, so a match containing one is not this shape however its arms
 * read.
 */
bool constructorPattern(ast::ParseBase parse, Module& module, const ast::Pat& pattern,
                        GlobalPtr<RecordType> record, U32& index) {
    if(pattern.kind != ast::Pat::Con || pattern.asVar) return false;
    if(pattern.bind != ast::BindType::Borrow) return false;

    auto found = findConstructor(module, pattern.con.name, pattern.source);
    if(!found) return false;

    auto reference = found.unwrap();
    if(reference.record != record) return false;

    // A payload pattern is admitted only where it is `_`: irrefutable, binding nothing, moving
    // nothing. `Just(v)` and `Just(->v)` both name the payload and are declined.
    if(pattern.con.pats) {
        auto& payload = *parse[pattern.con.pats];
        if(payload.kind != ast::Pat::Any || payload.asVar) return false;
    }

    index = reference.index;
    return true;
}

// The wildcard arm, which is the only thing that may fill in the constructors the alternatives did
// not name. `_` and nothing else - a `Pat::Var` binds the pivot under a name the body may read.
bool wildcardPattern(const ast::Pat& pattern) {
    return pattern.kind == ast::Pat::Any && !pattern.asVar;
}

/*
 * A form the arm's body may be evaluated as a constant in without a diagnostic being produced for a
 * program the ordinary path would have accepted.
 *
 * `evaluateConstant`'s `notConstant` flag turns "this is not a constant *form*" into an answer, and
 * that covers almost everything - a call, a name, an operator. What it deliberately does not cover
 * is the right form with the wrong contents, and two of those would fire on programs that resolve
 * perfectly well as expressions:
 *
 *  - an ascription, `A -> 0 :: Int`, in a position whose type is something else: the expression
 *    resolver converts, and the constant evaluator requires the two to agree;
 *  - a constructor of a *generic* record with nothing to infer its arguments from, `A -> Just(1)`
 *    where the position states no type: `resolveConstruct` solves that from the resolved argument
 *    values, and a constant has none.
 *
 * Both are refused here rather than reported there, so that the recognizer is silent by construction
 * rather than by inspection of what a callee happens to report.
 */
bool constantForm(const ast::Expr& body, bool typed) {
    if(body.kind == ast::Expr::Coerce) return false;
    if(typed) return true;

    return body.kind != ast::Expr::Con && body.kind != ast::Expr::Tup && body.kind != ast::Expr::Array;
}

/*
 * ## The five forms, and the rule that picks one
 *
 * Tried in the order below, which is by what each costs to run rather than by how general it is:
 *
 *  1. **Uniform** - every constructor answers the same thing, so the pivot is never read;
 *  2. **Range** / **Mask** - a `Bool` property, as two comparisons where the set is one interval and
 *     as a bit test where it is not. Neither branches;
 *  3. **Affine** - the results are `selector * stride + bias`, so the table is arithmetic.
 *     `Enum.valueOf` is the identity case of this, already emitted as a cast;
 *  4. **Packed** - small results, all of them, in the bits of one immediate. Two shifts and a mask,
 *     and no memory touched. Bounded at `kMapImmediateBits`, which is a JS decision - see there;
 *  5. **Table** - a static array indexed by the selector.
 *
 * ### The size rule
 *
 * Forms 1-4 read no memory and are taken whenever they apply, whatever the size. The table is taken
 * when all three of these hold, and the map is declined - back to the ordinary comparison chain -
 * otherwise:
 *
 *  - **three or more constructors.** Two constructors are one comparison and one select, which is
 *    already what indexing a two-entry table costs. This is the documented edge of the guarantee: an
 *    eligible map over three or more constructors never remains a chain, and a two-constructor match
 *    is never forced to touch memory;
 *  - **a span of at most 256 selector values**, so that a pinned enumeration with one distant
 *    outlier does not become kilobytes of fill;
 *  - **a span of at most eight times the constructor count.** The chain this replaces costs about
 *    two instructions per constructor, so a table is worth a good deal of fill - the bound is
 *    against a handful of constructors pinned to numbers that are far apart, where the fill is the
 *    whole table. An `errno` list, nineteen constructors over a span of about 123, is inside it and
 *    is the shape the rule was set against; three constructors over that same span are not.
 *
 * A sparse map above either bound is a perfect-hash or binary-search question, and is deliberately
 * left to the ordinary path rather than answered badly here.
 */
constexpr Size kMapMinTableEntries = 3;
constexpr I64 kMapMaxTableSpan = 256;
constexpr I64 kMapMaxTableSpread = 8;

/*
 * How many bits of one immediate the packed and mask forms may use.
 *
 * Thirty-one rather than sixty-four, and the reason is JS: `Long` there is a `bigint`, so a map
 * packed into a 64-bit word emits `BigInt.asUintN` around every shift and is *slower* than the chain
 * it replaced. An `Int` is a host number on JS and a register natively, so the two targets agree and
 * neither pays for the other - and one below the width keeps the sign bit out of it, so the shift
 * and the mask are the same operation whichever way a target reads the word.
 *
 * A map too wide for this is a table, which is an `Int32Array` read on JS and an indexed load
 * natively. That is the right answer for a wide one on both.
 */
constexpr U32 kMapImmediateBits = 31;

enum class MapForm: U8 {
    None,
    Uniform,
    Range,
    Mask,
    Affine,
    Packed,
    Table,
};

struct MapPlan {
    MapForm form = MapForm::None;

    // The closed interval of selector values the record's own constructors occupy. Every indexed
    // form is relative to `lowest`, and `highest` is what lets a one-sided range test drop its
    // second comparison.
    I64 lowest = 0;
    I64 highest = 0;

    // `Range`: the interval that answers `True`.
    I64 first = 0;
    I64 last = 0;

    // `Mask` and `Packed`: the word, and how many bits of it each selector owns.
    U64 immediate = 0;
    U32 width = 1;

    // `Affine`.
    I64 stride = 1;
    I64 bias = 0;

    I64 span() const { return highest - lowest + 1; }
};

void computeBounds(const ConstructorMap& map, MapPlan& plan) {
    plan.lowest = map.entries[0].selector;
    plan.highest = map.entries[0].selector;

    for(auto& entry: map.entries) {
        if(entry.selector < plan.lowest) plan.lowest = entry.selector;
        if(entry.selector > plan.highest) plan.highest = entry.selector;
    }
}

// A `Bool` map as a test with no branch in it: the run of selectors answering `True` where they form
// one, and the bits of one word where they do not.
bool choosePredicate(ModuleBase local, const ConstructorMap& map, MapPlan& plan) {
    auto count = Size(0);

    for(auto& entry: map.entries) {
        if(!local[entry.value]->bits) continue;

        if(!count || entry.selector < plan.first) plan.first = entry.selector;
        if(!count || entry.selector > plan.last) plan.last = entry.selector;

        if(plan.span() <= I64(kMapImmediateBits)) plan.immediate |= U64(1) << U64(entry.selector - plan.lowest);
        count++;
    }

    // All false and all true are the uniform case, which was decided before this.
    if(!count || count == map.entries.size()) return false;

    /*
     * How many of the record's selectors fall inside `[first, last]`. The interval is over the
     * *domain* rather than over the integers, so a pinned enumeration with a gap in it may still
     * state a contiguous property with non-consecutive numbers - and that is one range test, because
     * a value of the record cannot carry a number in the gap.
     */
    auto inside = Size(0);
    for(auto& entry: map.entries) {
        if(entry.selector >= plan.first && entry.selector <= plan.last) inside++;
    }

    if(inside == count) {
        plan.form = MapForm::Range;
        return true;
    }

    if(plan.span() > I64(kMapImmediateBits)) return false;

    plan.form = MapForm::Mask;
    return true;
}

/*
 * `selector * stride + bias`, where the results are that.
 *
 * The general case of what `Enum.valueOf` already is: a `match` whose arms are the numbers the
 * constructors are, offset or scaled. Solved from the first two selectors and then verified against
 * every entry, because a fit that holds for two points and not for a third is not a fit - and
 * checked for overflow at each step, since a stride that wraps is a different function.
 */
bool chooseAffine(GlobalBase global, ModuleBase local, const ConstructorMap& map, MapPlan& plan) {
    if(!arithmeticResult(global, map.resultType)) return false;
    if(map.entries.size() < 2) return false;

    SmallArray<I64, 16> numbers;
    for(auto& entry: map.entries) {
        I64 number = 0;
        if(!constantNumber(global, local, entry.value, number)) return false;

        numbers.push(number);
    }

    // Two points decide the line. A record's selectors are distinct by construction, so the first
    // two are always two different ones and the division below has a divisor.
    auto runSelector = map.entries[1].selector - map.entries[0].selector;
    auto runValue = numbers[1] - numbers[0];
    if(!runSelector || runValue % runSelector) return false;

    auto stride = runValue / runSelector;
    I64 scaled = 0;
    if(__builtin_mul_overflow(map.entries[0].selector, stride, &scaled)) return false;

    auto bias = numbers[0] - scaled;

    for(Size i = 0; i < map.entries.size(); i++) {
        I64 term = 0;
        I64 total = 0;
        if(__builtin_mul_overflow(map.entries[i].selector, stride, &term)) return false;
        if(__builtin_add_overflow(term, bias, &total)) return false;
        if(total != numbers[i]) return false;
    }

    // A zero stride is the uniform case, which was decided before this and left nothing for a
    // multiply to do. Reaching it here would mean the entries disagreed about a value they share.
    if(!stride) return false;

    plan.form = MapForm::Affine;
    plan.stride = stride;
    plan.bias = bias;
    return true;
}

/*
 * The whole map in the bits of one immediate.
 *
 * The arity, operand-class and purity-category tables an instruction description is made of are all
 * this: a handful of constructors answering numbers two or four bits wide. Packing them costs a
 * shift and a mask and touches nothing, which is strictly better than an indexed byte read.
 *
 * The width is a power of two so that the shift amount is a shift rather than a multiply, and what
 * is packed is the *reduced* pattern each constant holds - so a signed result narrower than the
 * field comes back out through the same truncating cast that put it in, sign and all.
 */
bool choosePacked(GlobalBase global, ModuleBase local, const ConstructorMap& map, MapPlan& plan) {
    if(!arithmeticResult(global, map.resultType)) return false;

    auto span = plan.span();
    if(span < 1 || span > I64(kMapImmediateBits)) return false;

    U64 widest = 0;
    for(auto& entry: map.entries) {
        if(local[entry.value]->kind != ConstKind::Scalar) return false;
        if(local[entry.value]->bits > widest) widest = local[entry.value]->bits;
    }

    U32 width = 1;
    while(width < 32 && (widest >> width)) width *= 2;
    if(U64(span) * width > kMapImmediateBits) return false;

    U64 packed = 0;
    for(auto& entry: map.entries) {
        packed |= local[entry.value]->bits << U64((entry.selector - plan.lowest) * width);
    }

    plan.form = MapForm::Packed;
    plan.immediate = packed;
    plan.width = width;
    return true;
}

bool chooseTable(Module& module, const ConstructorMap& map, MapPlan& plan) {
    auto global = *module.types;
    auto local = *module.arena;

    if(map.entries.size() < kMapMinTableEntries) return false;
    if(plan.span() > kMapMaxTableSpan) return false;
    if(plan.span() > I64(map.entries.size()) * kMapMaxTableSpread) return false;
    if(!tableElement(module, map.resultType)) return false;

    /*
     * A constant reached through an indirection has nothing to point at in static storage - see
     * constantHasStaticForm. Asked here rather than left to whichever backend lays the table out, so
     * that a boxed result declines to the ordinary path instead of failing to emit, and so that the
     * refusal carries no location and needs none.
     *
     * A string's constant is exempt: it is built here rather than laid out, and its static form is
     * the two words emitStringTable writes itself.
     */
    if(global[map.resultType]->kind != Type::String) {
        for(auto& entry: map.entries) {
            if(!constantHasStaticForm(global, local, entry.value)) return false;
        }
    }

    plan.form = MapForm::Table;
    return true;
}

MapPlan chooseForm(Module& module, const ConstructorMap& map) {
    auto global = *module.types;
    auto local = *module.arena;

    MapPlan plan;
    computeBounds(map, plan);

    auto uniform = true;
    for(auto& entry: map.entries) {
        if(!sameConstant(local, entry.value, map.entries[0].value)) uniform = false;
    }

    if(uniform) {
        plan.form = MapForm::Uniform;
        return plan;
    }

    if(map.resultType == module.scalar.bool_ && choosePredicate(local, map, plan)) return plan;
    if(chooseAffine(global, local, map, plan)) return plan;
    if(choosePacked(global, local, map, plan)) return plan;

    chooseTable(module, map, plan);
    return plan;
}

/*
 * ## Emitting
 */

// What the selector arithmetic is done at. `Int` unless a pinned `@value` does not fit in one, which
// it may well not: the numbers an enumeration is pinned to are somebody else's ABI.
TypePtr selectorType(Module& module, const ConstructorMap& map) {
    for(auto& entry: map.entries) {
        if(entry.selector < I64(minLimit<I32>) || entry.selector > I64(maxLimit<I32>)) {
            return module.scalar.long_;
        }
    }

    return module.scalar.int_;
}

/*
 * The selector, as a value.
 *
 * The same two readings `resolvePattern` makes for a constructor test, and for the same reason: an
 * enumeration is its number, so the selector is a cast of the value itself, and every other sum
 * carries a discriminant beside its payload.
 */
ModulePtr<Value> selectorOf(ExprResolver& resolver, const ConstructorMap& map, ModulePtr<Value> pivot,
                            TypePtr type, LocationId source) {
    if(map.byValue) {
        return resolver.ref(resolver.emit<InstUnary>(source, StringId(), type, Value::Cast, pivot));
    }

    auto tag = resolver.load(resolver.project(resolver.placeFor(pivot, source),
                                              ProjectionKind::Discriminant, 0), source);

    if(resolver.valueType(tag) == type) return tag;
    return resolver.ref(resolver.emit<InstUnary>(source, StringId(), type, Value::Cast, tag));
}

// `value` at `type`, where the two may already agree - the cast is the conversion resolve has, and
// an identity one is worth not emitting so that the common `Int` selector stays one instruction.
ModulePtr<Value> castTo(ExprResolver& resolver, ModulePtr<Value> value, TypePtr type, LocationId source) {
    if(!value || resolver.valueType(value) == type) return value;
    return resolver.ref(resolver.emit<InstUnary>(source, StringId(), type, Value::Cast, value));
}

// `selector - offset`, or the selector itself where the offset is zero. Every indexed form starts
// here, since a map's selectors rarely start at zero and every one of them is relative.
ModulePtr<Value> selectorMinus(ExprResolver& resolver, ModulePtr<Value> selector, TypePtr type,
                               I64 offset, LocationId source) {
    if(offset == 0) return selector;

    auto amount = resolver.makeInt(source, type, U64(offset));
    return resolver.ref(resolver.emit<InstBinary>(source, StringId(), type, Value::Sub, selector, amount));
}

/*
 * `True` for one contiguous run of selectors and `False` for everything else, as the two comparisons
 * that says.
 *
 * Only the tests the *closed domain* still needs are emitted, which is what makes the common shapes
 * one instruction: a run reaching the lowest selector needs no lower test, one reaching the highest
 * needs no upper, and a run of one is a single equality. None of those is a peephole - a value of
 * the record holds one of these selectors and nothing else.
 *
 * The unsigned `selector - first <= last - first` form that the two comparisons replace is
 * deliberately not built. It needs a cast into an unsigned type wide enough to hold the difference,
 * and on JS that cast is a host coercion rather than a reinterpretation; two comparisons of the
 * value as it stands are the same answer with nothing to get wrong, and the folder reduces them
 * where a target has the better form.
 */
ModulePtr<Value> emitRange(ExprResolver& resolver, const MapPlan& plan, ModulePtr<Value> selector,
                           TypePtr type, LocationId source) {
    auto bool_ = resolver.module.scalar.bool_;

    if(plan.first == plan.last) {
        auto value = resolver.makeInt(source, type, U64(plan.first));
        return resolver.ref(resolver.emit<InstCmp>(source, StringId(), bool_, selector, value, CompareOp::Eq));
    }

    ModulePtr<Value> lower = nullptr;
    ModulePtr<Value> upper = nullptr;

    if(plan.first > plan.lowest) {
        auto value = resolver.makeInt(source, type, U64(plan.first));
        lower = resolver.ref(resolver.emit<InstCmp>(source, StringId(), bool_, selector, value, CompareOp::Ge));
    }

    if(plan.last < plan.highest) {
        auto value = resolver.makeInt(source, type, U64(plan.last));
        upper = resolver.ref(resolver.emit<InstCmp>(source, StringId(), bool_, selector, value, CompareOp::Le));
    }

    if(!lower) return upper;
    if(!upper) return lower;

    return resolver.ref(resolver.emit<InstBinary>(source, StringId(), bool_, Value::And, lower, upper));
}

/*
 * `((mask >> (selector - lowest)) & 1) != 0` - a sparse `Bool` property over a small domain.
 *
 * This is the `isCommutative`-shaped set: not one interval, so no pair of comparisons states it, and
 * small enough that one immediate holds the whole answer. Better than a byte table for the reason
 * that matters at this size - it reads no memory - and the shift is in range for every selector a
 * value can carry, because the domain is closed and the span was checked against the word.
 */
ModulePtr<Value> emitMask(ExprResolver& resolver, const MapPlan& plan, ModulePtr<Value> selector,
                          TypePtr type, LocationId source) {
    auto& module = resolver.module;
    auto word = module.scalar.int_;

    auto index = castTo(resolver, selectorMinus(resolver, selector, type, plan.lowest, source), word, source);
    auto bits = resolver.makeInt(source, word, plan.immediate);
    auto shifted = resolver.ref(resolver.emit<InstBinary>(source, StringId(), word, Value::Shr, bits, index));
    auto one = resolver.makeInt(source, word, 1);
    auto bit = resolver.ref(resolver.emit<InstBinary>(source, StringId(), word, Value::And, shifted, one));
    auto zero = resolver.makeInt(source, word, 0);

    return resolver.ref(resolver.emit<InstCmp>(source, StringId(), module.scalar.bool_, bit, zero, CompareOp::Ne));
}

/*
 * `selector * stride + bias`, at the narrowest width that holds every step of it.
 *
 * The identity - stride one, no bias - is the shape a `match` computing `valueOf` has, and it is a
 * cast and nothing else. The rest is done at the selector's own width where every product and every
 * sum fits in it, and at `Long` otherwise: this runs before the optimizer and at `-no-opt` it is
 * what the target gets, so a widening round trip nothing needs is two real instructions.
 */
ModulePtr<Value> emitAffine(ExprResolver& resolver, const ConstructorMap& map, const MapPlan& plan,
                            ModulePtr<Value> selector, TypePtr type, LocationId source) {
    if(plan.stride == 1 && !plan.bias) return castTo(resolver, selector, map.resultType, source);

    auto narrow = type == resolver.module.scalar.int_;

    for(auto& entry: map.entries) {
        I32 term = 0;
        I32 total = 0;
        if(!narrow) break;

        narrow = entry.selector >= I64(minLimit<I32>) && entry.selector <= I64(maxLimit<I32>)
            && !__builtin_mul_overflow(I32(entry.selector), I32(plan.stride), &term)
            && plan.bias >= I64(minLimit<I32>) && plan.bias <= I64(maxLimit<I32>)
            && !__builtin_add_overflow(term, I32(plan.bias), &total);
    }

    auto long_ = narrow ? type : resolver.module.scalar.long_;
    auto value = castTo(resolver, selector, long_, source);

    if(plan.stride != 1) {
        auto factor = resolver.makeInt(source, long_, U64(plan.stride));
        value = resolver.ref(resolver.emit<InstBinary>(source, StringId(), long_, Value::Mul, value, factor));
    }

    if(plan.bias) {
        auto offset = resolver.makeInt(source, long_, U64(plan.bias));
        value = resolver.ref(resolver.emit<InstBinary>(source, StringId(), long_, Value::Add, value, offset));
    }

    return castTo(resolver, value, map.resultType, source);
}

ModulePtr<Value> emitPacked(ExprResolver& resolver, const ConstructorMap& map, const MapPlan& plan,
                            ModulePtr<Value> selector, TypePtr type, LocationId source) {
    auto& module = resolver.module;
    auto word = module.scalar.int_;

    auto index = castTo(resolver, selectorMinus(resolver, selector, type, plan.lowest, source), word, source);

    if(plan.width > 1) {
        U32 shift = 0;
        while((U32(1) << shift) < plan.width) shift++;

        auto amount = resolver.makeInt(source, word, shift);
        index = resolver.ref(resolver.emit<InstBinary>(source, StringId(), word, Value::Shl, index, amount));
    }

    auto immediate = resolver.makeInt(source, word, plan.immediate);
    auto shifted = resolver.ref(resolver.emit<InstBinary>(source, StringId(), word, Value::Shr, immediate, index));

    // The top field owns the rest of the word, so there is nothing above it to mask off.
    if(U64(plan.span()) * plan.width < kMapImmediateBits) {
        auto mask = resolver.makeInt(source, word, (U64(1) << plan.width) - 1);
        shifted = resolver.ref(resolver.emit<InstBinary>(source, StringId(), word, Value::And, shifted, mask));
    }

    if(map.resultType == module.scalar.bool_) {
        auto zero = resolver.makeInt(source, word, 0);
        return resolver.ref(resolver.emit<InstCmp>(source, StringId(), module.scalar.bool_, shifted, zero, CompareOp::Ne));
    }

    return castTo(resolver, shifted, map.resultType, source);
}

// A global holding one map's table. Anonymous on `addAnonymousGlobal`'s terms - nothing in the
// source can name it, and the reachability walk finds it through the place that reads it.
Global* addMapTable(Module& module, TypePtr type, ConstantPtr initial, LocationId source) {
    auto& context = module.context;

    StringBuilder name;
    name << context.findName(module.name) << ".map$";
    name.appendValue(module.constructorMapCount++);

    auto table = module.addGlobal(builtName(context, name), source);
    table->type = type;
    table->initial = initial;
    table->used = true;
    table->anonymous = true;
    return table;
}

// The constant a slot of the table holds. A pinned enumeration may leave holes inside its own span,
// and a value of the record cannot carry one of those numbers - so what fills them decides nothing
// beyond the slot having the right shape, and the first entry is as good as any.
ConstantPtr slotConstant(const ConstructorMap& map, const MapPlan& plan, I64 slot) {
    for(auto& entry: map.entries) {
        if(entry.selector - plan.lowest == slot) return entry.value;
    }

    return map.entries[0].value;
}

// The place of `table[selector - lowest]`, which is the one read every table form makes. `Size`
// rather than a machine word for the reason eachFixedElement gives: this is an index, and the two
// are the same type natively and different host types on JS.
Place tableSlot(ExprResolver& resolver, Global* table, ModulePtr<Value> selector, TypePtr type,
                I64 lowest, LocationId source) {
    auto index = castTo(resolver, selectorMinus(resolver, selector, type, lowest, source),
                        resolver.module.scalar.size, source);

    return resolver.project(Place::inGlobal(table - resolver.local), ProjectionKind::Index, 0, index);
}

/*
 * A table of string literals.
 *
 * The one non-`TrivialCopy` result this serves, and it is served by *building the literal* rather
 * than by reading one out - which is what makes it sound. What a native literal is, is the two words
 * `stringLiteral` is handed (Implementation-String.md part 9): the address of a blob, and a length.
 * Both are constants, so both go in the table, and every arm then constructs its string exactly as
 * the arm the author wrote did. Nothing is shared and nothing becomes a second owner: the run is
 * borrowed, so the value's teardown is the same nothing a written literal's is.
 *
 * JS diverges completely and does so in three lines, because a host string is one value with nothing
 * underneath it. The table is an array of them and the read is the value - the same split
 * `resolveString` and `stringConstant` already make, reached here for the same reason.
 */
ModulePtr<Value> emitStringTable(ExprResolver& resolver, const ConstructorMap& map, const MapPlan& plan,
                                 ModulePtr<Value> pivot, TypePtr type, LocationId source) {
    auto& module = resolver.module;
    auto& context = module.context;
    auto local = resolver.local;
    auto span = plan.span();

    if(isJsMode(context.settings.mode)) {
        auto arrayType = resolveFixedArrayType(module, module.scalar.string_, U32(span), source);
        auto contents = new (module.arena) ConstValue(arrayType, ConstKind::Aggregate);

        for(I64 i = 0; i < span; i++) {
            contents->children.push(module.arena, slotConstant(map, plan, i));
        }

        auto table = addMapTable(module, arrayType, contents - local, source);
        auto selector = selectorOf(resolver, map, pivot, type, source);

        return resolver.load(tableSlot(resolver, table, selector, type, plan.lowest, source), source);
    }

    auto constructor = module.program.stringLiteral;
    local[constructor]->used = true;

    // The two argument types come off `stringLiteral`'s own signature rather than being built here,
    // which is what keeps this correct if the code unit ever stops being a byte - the same rule
    // `resolveString` states about the pointee type.
    auto address = local[local[constructor]->args.get(local, 0)]->type;
    auto count = local[local[constructor]->args.get(local, 1)]->type;

    Field fields[] = { Field { address, StringId() }, Field { count, StringId() } };
    TypePtr element = (Type*)resolveTupleType(module, { fields, 2 }, source) - resolver.global;
    auto arrayType = resolveFixedArrayType(module, element, U32(span), source);

    // One blob per *slot* rather than per distinct text, so a string repeated across two arms is two
    // blobs - which is what two written literals of the same text already are, see stringLiteralBytes.
    auto contents = new (module.arena) ConstValue(arrayType, ConstKind::Aggregate);

    for(I64 i = 0; i < span; i++) {
        auto text = local[slotConstant(map, plan, i)]->text;
        auto blob = stringLiteralBytes(module, text, source);

        auto pointer = new (module.arena) ConstValue(address, ConstKind::Address);
        pointer->global = blob - local;

        auto length = new (module.arena) ConstValue(count, ConstKind::Scalar);
        length->bits = U64(context.findName(text).size());

        auto entry = new (module.arena) ConstValue(element, ConstKind::Aggregate);
        entry->children.push(module.arena, pointer - local);
        entry->children.push(module.arena, length - local);

        contents->children.push(module.arena, entry - local);
    }

    auto table = addMapTable(module, arrayType, contents - local, source);
    auto selector = selectorOf(resolver, map, pivot, type, source);
    auto slot = tableSlot(resolver, table, selector, type, plan.lowest, source);

    auto bytes = resolver.load(resolver.project(slot, ProjectionKind::Field, 0), source);
    auto length = resolver.load(resolver.project(slot, ProjectionKind::Field, 1), source);

    auto call = resolver.create<InstCall>(source, StringId(), module.scalar.string_, constructor);
    call->args.push(module.arena, bytes);
    call->args.push(module.arena, length);
    resolver.append(call);

    auto result = resolver.ref(call);

    // The slot, on the same terms as any other call of memory type - a value with none is invisible
    // to the ownership passes, which is the omission `resolveString` records having had.
    if(isMemoryType(resolver.global, module.scalar.string_)) {
        call->local = resolver.function.addLocal(module, module.scalar.string_, StringId(), result);
    }

    return result;
}

// A static array indexed by the selector - the general form, and the one the size rule gates.
ModulePtr<Value> emitTable(ExprResolver& resolver, const ConstructorMap& map, const MapPlan& plan,
                           ModulePtr<Value> pivot, TypePtr type, LocationId source) {
    auto& module = resolver.module;
    auto local = resolver.local;

    if(resolver.global[map.resultType]->kind == Type::String) {
        return emitStringTable(resolver, map, plan, pivot, type, source);
    }

    auto span = plan.span();
    auto arrayType = resolveFixedArrayType(module, map.resultType, U32(span), source);
    auto contents = new (module.arena) ConstValue(arrayType, ConstKind::Aggregate);

    for(I64 i = 0; i < span; i++) {
        contents->children.push(module.arena, slotConstant(map, plan, i));
    }

    auto table = addMapTable(module, arrayType, contents - local, source);
    auto selector = selectorOf(resolver, map, pivot, type, source);

    return resolver.load(tableSlot(resolver, table, selector, type, plan.lowest, source), source);
}

} // namespace

bool recognizeConstructorMap(ExprResolver& resolver, const ast::MatchExpr& match,
                             Buffer<ast::Pat> patterns, TypePtr pivotType, TypePtr target,
                             ConstructorMap& map) {
    auto& module = resolver.module;
    auto global = resolver.global;
    auto local = resolver.local;

    if(!pivotType || global[pivotType]->kind != Type::Record) return false;

    auto record = (RecordType*)global[pivotType];
    auto declaration = record->base(global);
    auto constructors = record->constructors.contents(global);
    if(!constructors.size()) return false;

    /*
     * The type every arm produces, which has to be known before the first arm is evaluated rather
     * than joined out of what they turn out to be.
     *
     * A position that states one hands it over. A position that does not - `let n = match c: ...` -
     * takes the first arm's own, and `constantForm` has already restricted such an arm to the
     * literals that have a type without being told one. What that gives up is a match whose arms
     * widen against each other, which the ordinary path resolves; what it buys is that no arm is
     * ever evaluated at a type nothing decided.
     */
    TypePtr resultType = nullptr;

    if(target) {
        auto kind = global[target]->kind;
        if(kind == Type::Error || kind == Type::Gen || kind == Type::Literal) return false;
        resultType = target;
    }

    // One entry per constructor, filled by the alternatives in order so that the first arm to name a
    // constructor wins - which is the language's rule, and the reason a later duplicate is dead.
    Array<ConstantPtr> results;
    results.reserve(U32(constructors.size()));
    for(Size i = 0; i < constructors.size(); i++) results.push(nullptr);

    auto named = Size(0);
    auto covered = false;
    auto alternativeList = match.alts;
    auto alternatives = alternativeList.contents(resolver.parse);

    for(Size i = 0; i < patterns.size() && !covered; i++) {
        auto& pattern = patterns[i];
        auto body = alternatives[i].expr;

        auto wildcard = wildcardPattern(pattern);
        U32 index = 0;

        if(!wildcard && !constructorPattern(resolver.parse, module, pattern, declaration, index)) {
            return false;
        }

        // A repeated constructor's second arm is unreachable, and so is a wildcard with nothing left
        // to fill. Both are warned about by resolveMatch, and neither contributes a result.
        if(!wildcard && results[index]) continue;

        if(!constantForm(body, resultType != nullptr)) return false;

        auto notConstant = false;
        auto constant = evaluateConstant(module, body, resultType, "a match arm"_v, false, &notConstant);
        if(!constant) return false;

        // The first arm decides the type where the position did not. Every later one is evaluated
        // against it, so this can only differ for the first - but it is checked for all of them,
        // since an arm may come out at another type without an ascription saying so.
        if(!resultType) resultType = local[constant]->type;
        if(!sameType(local[constant]->type, resultType)) return false;

        if(wildcard) {
            for(Size c = 0; c < results.size(); c++) {
                if(!results[c]) results[c] = constant;
            }

            named = constructors.size();
            covered = true;
            continue;
        }

        results[index] = constant;
        named++;
        covered = named == constructors.size();
    }

    // Not every constructor answered, which is a match the ordinary path either completes with a
    // pattern this does not read or reports as inexhaustive. Either way it is that path's to finish.
    if(!covered) return false;

    map.pivotType = pivotType;
    map.resultType = resultType;
    map.byValue = record->layout == RecordType::Enum;

    map.entries.reserve(U32(constructors.size()));
    for(Size i = 0; i < constructors.size(); i++) {
        ConstructorMapEntry entry;
        entry.selector = map.byValue ? constructors[i].value : I64(constructors[i].index);
        entry.value = results[i];
        map.entries.push(entry);
    }

    // The size rule, consulted before the answer rather than while emitting: a caller that is told
    // yes has already warned about the alternatives, and must not then be handed nothing.
    return chooseForm(module, map).form != MapForm::None;
}

ModulePtr<Value> emitConstructorMap(ExprResolver& resolver, const ConstructorMap& map,
                                    ModulePtr<Value> pivot, LocationId source) {
    auto& module = resolver.module;
    auto plan = chooseForm(module, map);
    auto type = selectorType(module, map);

    // Every constructor answers the same thing, so nothing about the pivot is read - the whole match
    // is that value. `constantValue` builds it the way any other constant in an expression is built,
    // which for an aggregate is fresh storage per use: two reads of one constant are two values, and
    // one of them may be written through.
    if(plan.form == MapForm::Uniform) return resolver.constantValue(map.entries[0].value, source);
    if(plan.form == MapForm::Table) return emitTable(resolver, map, plan, pivot, type, source);

    auto selector = selectorOf(resolver, map, pivot, type, source);

    switch(plan.form) {
        case MapForm::Range:
            return emitRange(resolver, plan, selector, type, source);
        case MapForm::Mask:
            return emitMask(resolver, plan, selector, type, source);
        case MapForm::Affine:
            return emitAffine(resolver, map, plan, selector, type, source);
        case MapForm::Packed:
            return emitPacked(resolver, map, plan, selector, type, source);
        default:
            break;
    }

    // Unreachable: recognition asked the same question of the same map and refused a `None`.
    return nullptr;
}
