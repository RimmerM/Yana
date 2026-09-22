/*
 * A `match` over constants, turned into the shape the machine wants - `ssa::InstMap` spent.
 *
 * ## Why the algebra is here and not in a backend
 *
 * Which of five shapes a mapping *is* - the constant it always answers, the interval a `Bool`
 * property occupies, the bits of one word, the line `selector * stride + bias`, the fields of one
 * immediate, or a run of slots in read-only data - is a fact about the numbers and about nothing
 * else. It is target-independent, so it is decided once, here, immediately below the tier that
 * recognized the mapping and immediately above the two backends that would otherwise each answer it.
 * `resolve/expr_map.cpp` is the shipping stage's version of the same five, and this is that
 * reasoning ported minus its memory arm - which is the half that *is* a target question, and which
 * ends here as a global plus an indexed read that both backends already know how to emit.
 *
 * The size rule the shipping file has to state - "three or more constructors, a span of at most 256,
 * a span of at most eight times the count" - is **not** here, and its absence is the point of
 * carrying the mapping down as a node. That file has to promise its caller an emission before it
 * knows what it will emit, so it consults the rule twice; here the node always exists and what it
 * becomes is decided once, at the tier that owns the target question. What recognition still bounds
 * is representability and not policy - see `kMapSpanLimit` in resolve_match.cpp.
 *
 * ## Every form is total
 *
 * A `map` over a **closed** domain - a record's own constructors - answers for every value the
 * selector can carry, so the arithmetic forms need no guard at all. An **open** one - an integer
 * pivot with a wildcard arm - is served by the table with one extra slot holding the fallback and a
 * branch-free `select` that picks it: two comparisons and a `cmov` rather than a branch, so a `map`
 * still costs no block wherever it stands.
 */

#include "internal.h"

namespace ssa {

namespace {

/*
 * How many bits of one immediate the packed and mask forms may use.
 *
 * Thirty-one rather than sixty-four, and the reason is JS: `Long` there is a `bigint`, so a map
 * packed into a 64-bit word emits `BigInt.asUintN` around every shift and is *slower* than the chain
 * it replaced. The JS pipeline is not built yet (Analysis-IR-JS.md), and the constant is kept at the
 * value that makes both targets agree because it costs a native build nothing - a map too wide for
 * it is a table, which is an indexed load either way.
 */
constexpr U32 kMapImmediateBits = 31;

enum class MapForm: U8 {
    Uniform,
    Range,
    Mask,
    Affine,
    Packed,
    Table,
};

struct MapPlan {
    MapForm form = MapForm::Table;

    // The closed interval of selector values the entries occupy. Every indexed form is relative to
    // `lowest`, and `highest` is what lets a one-sided range test drop its second comparison.
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

// `sameConstant` - structural, bit-exact constant equality - is `resolve_ssa/build.h`'s since J88,
// where the JavaScript target's `match` over records asks it too.

/*
 * A scalar constant as the number it is, sign-extended from its own width.
 *
 * Every arithmetic decision below - an affine fit, an interval, a stride - is about the number, and
 * reading a stored pattern as one would make `-1` compare above every positive value. A float and an
 * address decline: a float's bits are not its value and an address has none until the image is
 * placed, and both reach the table instead, where the bits are all that is wanted.
 */
bool constantNumber(DeclBase decl, const mir::Constant& constant, I64& into) {
    if(constant.kind == mir::Constant::Enum) {
        auto& record = *(mir::RecordType*)decl[constant.type];
        into = I64(record.cons.get(decl, constant.asConstructor()).value);
        return true;
    }

    if(constant.kind != mir::Constant::Int) return false;
    if(!constant.type || decl[constant.type]->kind != mir::Type::Int) return false;

    into = I64(mir::reducedToWidth(*(mir::IntType*)decl[constant.type], constant.asInt()));
    return true;
}

// Whether the arithmetic forms may produce this type at all: a number computed at the selector's
// width and converted. A float is not one - its constant is a bit pattern rather than a number - and
// neither is a record with fields.
bool arithmeticResult(DeclBase decl, DeclPtr<mir::Type> type) {
    if(!type) return false;
    if(decl[type]->kind == mir::Type::Int) return true;

    return decl[type]->kind == mir::Type::Record &&
           ((mir::RecordType*)decl[type])->layout == mir::RecordLayout::Enum;
}

} // namespace

/*
 * Which of the five shapes this mapping is, and the numbers it needs.
 *
 * Tried by what each costs to run rather than by how general it is, which is `expr_map.cpp`'s order
 * and its reasoning: a uniform map reads nothing, a predicate branches nowhere, an affine step is
 * two instructions, a packed word is three, and only the table touches memory.
 */
static MapPlan chooseMapForm(DeclBase decl, Buffer<const mir::MapEntry> entries,
                             const mir::Constant& fallback, bool closed,
                             DeclPtr<mir::Type> result, bool isBool) {
    MapPlan plan;
    plan.lowest = entries[0].selector;
    plan.highest = entries[0].selector;

    for(auto& entry: entries) {
        if(entry.selector < plan.lowest) plan.lowest = entry.selector;
        if(entry.selector > plan.highest) plan.highest = entry.selector;
    }

    // Every arm the same answer, so the selector is never read - and where the domain is open the
    // fallback has to agree too, since it is one of the answers.
    auto uniform = closed || sameConstant(decl, fallback, entries[0].value);
    for(auto& entry: entries) {
        if(!sameConstant(decl, entry.value, entries[0].value)) uniform = false;
    }

    if(uniform) {
        plan.form = MapForm::Uniform;
        return plan;
    }

    /*
     * A `Bool` property as a test with no branch in it: the run of selectors answering `True` where
     * they form one, and the bits of one word where they do not.
     *
     * The interval is over the **domain** rather than over the integers, so a pinned enumeration
     * with a gap in it may still state a contiguous property with non-consecutive numbers - a value
     * of the record cannot carry a number in the gap. That reading is what makes `Range` a closed
     * domain's form only: over an open one, a selector *between* two entries is a value the fallback
     * answers and an interval test would answer `True` for.
     *
     * The **mask** serves both, because each selector owns a bit of its own: over an open domain the
     * shift is guarded and everything outside the span reads a bit that is zero. That is what makes
     * `Enum.fromValue` over a pinned enumeration one word rather than a comparison per constructor.
     */
    if(isBool && (closed || !constantBits(decl, fallback))) {
        auto count = Size(0);

        for(auto& entry: entries) {
            if(!constantBits(decl, entry.value)) continue;

            if(!count || entry.selector < plan.first) plan.first = entry.selector;
            if(!count || entry.selector > plan.last) plan.last = entry.selector;
            count++;
        }

        // All false is the fallback everywhere, and all true over a closed domain is the uniform
        // case that was decided above.
        if(count && (!closed || count != entries.size())) {
            auto inside = Size(0);
            for(auto& entry: entries) {
                if(entry.selector >= plan.first && entry.selector <= plan.last) inside++;
            }

            if(closed && inside == count) {
                plan.form = MapForm::Range;
                return plan;
            }

            if(plan.span() <= I64(kMapImmediateBits)) {
                for(auto& entry: entries) {
                    if(!constantBits(decl, entry.value)) continue;
                    plan.immediate |= U64(1) << U64(entry.selector - plan.lowest);
                }

                plan.form = MapForm::Mask;
                return plan;
            }
        }
    }

    // Only the table is total over an open domain: the arithmetic below is defined by the closed
    // domain's own selectors and has no shape for a value outside them.
    if(!closed) return plan;

    if(!arithmeticResult(decl, result)) return plan;

    /*
     * `selector * stride + bias`, where the results are that - the general case of what
     * `Enum.valueOf` already is.
     *
     * Solved from the first two selectors and then verified against every entry, because a fit that
     * holds for two points and not for a third is not a fit, and checked for overflow at each step,
     * since a stride that wraps is a different function.
     */
    if(entries.size() >= 2) {
        SmallArray<I64, 16> numbers;
        auto numeric = true;

        for(auto& entry: entries) {
            I64 number = 0;
            if(!constantNumber(decl, entry.value, number)) { numeric = false; break; }

            numbers.push(number);
        }

        auto runSelector = numeric ? entries[1].selector - entries[0].selector : 0;
        auto runValue = numeric ? numbers[1] - numbers[0] : 0;

        if(numeric && runSelector && !(runValue % runSelector)) {
            auto stride = runValue / runSelector;
            I64 scaled = 0;
            auto fits = !__builtin_mul_overflow(entries[0].selector, stride, &scaled);
            auto bias = numbers[0] - scaled;

            for(Size i = 0; fits && i < entries.size(); i++) {
                I64 term = 0;
                I64 total = 0;
                fits = !__builtin_mul_overflow(entries[i].selector, stride, &term)
                    && !__builtin_add_overflow(term, bias, &total)
                    && total == numbers[i];
            }

            // A zero stride is the uniform case, which was decided above and left nothing for a
            // multiply to do. Reaching it here would mean the entries disagreed about a shared value.
            if(fits && stride) {
                plan.form = MapForm::Affine;
                plan.stride = stride;
                plan.bias = bias;
                return plan;
            }
        }
    }

    /*
     * The whole map in the bits of one immediate.
     *
     * The arity, operand-class and purity tables an instruction description is made of are all this:
     * a handful of constructors answering numbers two or four bits wide. Packing them costs a shift
     * and a mask and touches nothing, which is strictly better than an indexed byte read. The width
     * is a power of two so that the shift amount is a shift rather than a multiply, and what is
     * packed is the *stored* pattern each constant holds - so a signed result narrower than the
     * field comes back out through the same truncating cast that put it in.
     */
    auto span = plan.span();
    if(span >= 1 && span <= I64(kMapImmediateBits)) {
        U64 widest = 0;
        auto packable = true;

        for(auto& entry: entries) {
            if(entry.value.kind != mir::Constant::Int && entry.value.kind != mir::Constant::Enum) {
                packable = false;
                break;
            }

            auto bits = constantBits(decl, entry.value);
            if(bits > widest) widest = bits;
        }

        U32 width = 1;
        while(width < 32 && (widest >> width)) width *= 2;

        if(packable && U64(span) * width <= kMapImmediateBits) {
            U64 packed = 0;
            for(auto& entry: entries) {
                packed |= constantBits(decl, entry.value) << U64((entry.selector - plan.lowest) * width);
            }

            plan.form = MapForm::Packed;
            plan.immediate = packed;
            plan.width = width;
            return plan;
        }
    }

    return plan;
}

// Two runs of bytes, compared. `Tritium`'s own comparison answers an ordering over a `String`, and
// what is wanted here is identity over raw storage.
static bool sameBytes(const Byte* a, const Byte* b, U32 count) {
    for(U32 i = 0; i < count; i++) {
        if(a[i] != b[i]) return false;
    }

    return true;
}

// The constant a slot of the table holds. A pinned enumeration may leave holes inside its own span,
// and a value of a closed domain cannot carry one of those numbers - so what fills them decides
// nothing beyond the slot having the right shape, and the fallback or the first entry is as good as
// any.
static const mir::Constant& mapSlot(Buffer<const mir::MapEntry> entries,
                                    const mir::Constant& filler, I64 selector) {
    for(auto& entry: entries) {
        if(entry.selector == selector) return entry.value;
    }

    return filler;
}

void Lowering::lowerMap(LowerBlock& block, InstMap& written) {
    auto selectorValue = mapped(written.selector.value());
    auto result = ssa[written.results.get(ssa, 0)];
    if(!selectorValue) return;

    auto contents = written.entries.contents(ssa);
    SmallArray<mir::MapEntry, 16> held;
    for(auto entry: contents) held.push(entry);

    Buffer<const mir::MapEntry> entries(held.pointer(), held.size());
    auto resultType = result->type();
    auto manifest = manifestOf(*result);
    auto isBool = mir::sameType(resultType, mir::booleanType(program));

    auto plan = chooseMapForm(decl, entries, written.fallback, written.closed, resultType, isBool);

    if(plan.form == MapForm::Uniform) {
        values.add(result->id.index, emitConstant(block, entries[0].value, result->occurrence));
        return;
    }

    auto selectorManifest = manifestOf(*ssa[written.selector.value()]);
    auto word = LowerType::Int32;

    auto imm = [&](LowerType type, U64 bits) {
        auto value = block.addInst(lower, new (to.arena) LowerImm(StringId(), type, bits));
        return ((LowerInstSingle*)value)->created().ptr - lower;
    };

    auto binary = [&](LowerType type, LowerPtr<LowerValue> lhs, LowerPtr<LowerValue> rhs,
                      LowerInst::Kind kind) {
        auto value = block.addInst(lower, new (to.arena) LowerInstBinary(
            StringId(), type, lhs, rhs, kind));
        return ((LowerInstSingle*)value)->created().ptr - lower;
    };

    auto compare = [&](LowerPtr<LowerValue> lhs, LowerPtr<LowerValue> rhs, LowerCmp kind,
                       LowerType type) {
        auto value = block.addInst(lower, new (to.arena) LowerInstCmp(
            StringId(), lhs, rhs, kind, type));
        return ((LowerInstSingle*)value)->created().ptr - lower;
    };

    // A value at another width, and nothing where the two already agree - the common `Int` selector
    // stays one value and not a round trip through a cast that says nothing.
    auto castTo = [&](LowerPtr<LowerValue> from, LowerType type, bool signedSource,
                      bool signedResult) {
        if(lower[from]->type == type) return from;

        auto value = block.addInst(lower, new (to.arena) LowerInstCast(
            StringId(), type, from, signedSource, signedResult));

        return ((LowerInstSingle*)value)->created().ptr - lower;
    };

    auto widened = [&](LowerType type) {
        return castTo(selectorValue - lower, type, selectorManifest.isSigned, true);
    };

    /*
     * `selector - lowest` at `type`, or the selector itself where the offset is zero. Every indexed
     * form starts here, since a map's selectors rarely start at zero.
     *
     * The offset arrives in the **normal form** of the width it is subtracted at - sign-extended
     * from thirty-two bits, which is how every other narrow immediate in this IR is held. At that
     * width it may truncate a `@value(n)` outside `I32`, and the truncation is exact rather than
     * approximate: the selector is reduced by the same width and the difference is bounded by the
     * span, so the low bits of `selector - lowest` are the whole answer whether or not either end
     * fits. Written out because the alternative reading - that a wide pinned enumeration has to take
     * the table - is what it looks like and is wrong.
     */
    auto relative = [&](LowerType type) {
        auto value = widened(type);
        if(!plan.lowest) return value;

        auto offset = type == LowerType::Int64 ? plan.lowest : I64(I32(plan.lowest));
        return binary(type, value, imm(type, U64(offset)), LowerInst::Sub);
    };

    switch(plan.form) {
        /*
         * `True` for one contiguous run of selectors, as the comparisons the closed domain still
         * needs: a run reaching the lowest selector needs no lower test, one reaching the highest
         * needs no upper, and a run of one is a single equality. None of those is a peephole - a
         * value of the domain holds one of these selectors and nothing else.
         */
        case MapForm::Range: {
            auto type = selectorManifest.type;
            auto ordered = selectorManifest.isSigned;
            auto self = selectorValue - lower;

            if(plan.first == plan.last) {
                values.add(result->id.index,
                           compare(self, imm(type, U64(plan.first)), LowerCmp::eq, manifest.type));
                return;
            }

            LowerPtr<LowerValue> low = nullptr;
            LowerPtr<LowerValue> high = nullptr;

            if(plan.first > plan.lowest) {
                low = compare(self, imm(type, U64(plan.first)),
                              ordered ? LowerCmp::ige : LowerCmp::ge, manifest.type);
            }

            if(plan.last < plan.highest) {
                high = compare(self, imm(type, U64(plan.last)),
                               ordered ? LowerCmp::ile : LowerCmp::le, manifest.type);
            }

            values.add(result->id.index, low && high
                ? binary(manifest.type, low, high, LowerInst::And)
                : (low ? low : high));
            return;
        }

        /*
         * `((mask >> (selector - lowest)) & 1) != 0` - a sparse property over a small domain.
         *
         * Over a **closed** domain the shift is in range for every selector a value can carry, since
         * the span was checked against the word. Over an **open** one it is guarded: the difference
         * is compared unsigned against the span, so a selector below `lowest` wraps to a huge number
         * and fails the same test, and what a value outside reads is bit `kMapImmediateBits` - which
         * is the bit the immediate's own width leaves clear, so the guard costs a `cmov` and no
         * second mask.
         */
        case MapForm::Mask: {
            auto width = written.closed ? word : LowerType::Int64;
            auto index = relative(width);

            if(!written.closed) {
                auto inside = compare(index, imm(width, U64(plan.span())), LowerCmp::lt,
                                      LowerType::Int32);

                auto chosen = block.addInst(lower, new (to.arena) LowerInstSelect(
                    StringId(), index, imm(width, kMapImmediateBits), inside, width));

                index = ((LowerInstSingle*)chosen)->created().ptr - lower;
            }

            auto shifted = binary(width, imm(width, plan.immediate), index, LowerInst::Shr);
            auto bit = binary(width, shifted, imm(width, 1), LowerInst::And);

            values.add(result->id.index,
                       compare(bit, imm(width, 0), LowerCmp::neq, manifest.type));
            return;
        }

        /*
         * `selector * stride + bias`, at the narrowest width that holds every step of it.
         *
         * The identity - stride one, no bias - is the shape a `match` computing `valueOf` has, and
         * it is a cast and nothing else, or not even that where the two widths already agree. The
         * rest is done at the selector's own width where every product and every sum fits in it, and
         * at sixty-four bits otherwise: this runs before nothing, and a widening round trip nothing
         * needs is two real instructions in the emitted program.
         */
        /*
         * Neither arm below calls `putInRange`/`narrowWidthOf` the way every narrow producer in
         * `inst.cpp` does after computing a result that might have overflowed the declared width -
         * `castTo` here is a plain width change, not a re-normalization. Safe for two separate
         * reasons, one per arm, neither obvious from reading this file alone.
         *
         * `selector * stride + bias` does not compute an arbitrary sum: `buildPlan` above verified
         * the formula reproduces every entry's own constant exactly (`total == numbers[i]`), so what
         * reaches `castTo` at runtime is always one of those already-valid arm values, computed at
         * `Int32` or `Int64` - never a value the formula's arithmetic pushed outside the declared
         * range, which is the only shape `putInRange` exists to correct. And a value already in
         * range needs no re-extension to reach **normal form**: ordinary two's complement arithmetic
         * at a type's own width already agrees, bit for bit, with "the logical value sign-extended
         * to that width," because there is no gap between the two to disagree in - a `@bits(n)`
         * refinement narrows which values are legal, not how a legal one is stored. Confirmed by
         * hand: a `match` over a closed six-way enum returning `@bits(5) I32` arms `-8..-3` lowers
         * `arm(k)` to exactly `add %k, -8`, and running it at the arm for `-7` answers `-7`.
         */
        case MapForm::Affine: {
            if(plan.stride == 1 && !plan.bias) {
                values.add(result->id.index,
                           castTo(selectorValue - lower, manifest.type,
                                  selectorManifest.isSigned, manifest.isSigned));
                return;
            }

            /*
             * The **stride** is bounded before it is truncated, which is the half the shipping
             * stage's `emitAffine` leaves out: `I32(plan.stride)` of a factor that does not fit
             * answers a different number, and every overflow test below then passes about it.
             * `LiteralWidth.yana`'s `castMatch` is the program - two arms over `Bool` at a `WideInt`,
             * whose stride is 2^35 + 255 and truncates to -1.
             */
            auto narrow = selectorManifest.type == LowerType::Int32 &&
                          plan.stride >= I64(minLimit<I32>) && plan.stride <= I64(maxLimit<I32>);

            for(auto& entry: entries) {
                I32 term = 0;
                I32 total = 0;
                if(!narrow) break;

                narrow = entry.selector >= I64(minLimit<I32>) && entry.selector <= I64(maxLimit<I32>)
                    && !__builtin_mul_overflow(I32(entry.selector), I32(plan.stride), &term)
                    && plan.bias >= I64(minLimit<I32>) && plan.bias <= I64(maxLimit<I32>)
                    && !__builtin_add_overflow(term, I32(plan.bias), &total);
            }

            auto width = narrow ? LowerType::Int32 : LowerType::Int64;
            auto value = widened(width);

            if(plan.stride != 1) {
                value = binary(width, value, imm(width, U64(plan.stride)), LowerInst::IMul);
            }

            if(plan.bias) value = binary(width, value, imm(width, U64(plan.bias)), LowerInst::Add);

            values.add(result->id.index,
                       castTo(value, manifest.type, true, manifest.isSigned));
            return;
        }

        /*
         * The second reason, and it is `buildPlan`'s rather than this arm's: `Packed` never reaches
         * here holding a negative to begin with, so `castTo` below is only ever widening a stored
         * *unsigned* field, which needs no sign-extending re-wrap of its own.
         *
         * `constantBits` (`gen.cpp`) answers a negative `Int` constant's **stored** pattern - the
         * full two's-complement bits reinterpreted as `U64`, not the magnitude - so one negative arm
         * alone makes `widest` a number near 2^32 or 2^64. `buildPlan`'s width loop is capped at 32
         * (`while(width < 32 && (widest >> width)) width *= 2`), so a negative arm always drives
         * `width` to that cap, and `span * width` - at least `1 * 32` - then always exceeds
         * `kMapImmediateBits` (31), which is `buildPlan`'s own admission test for this form just
         * above. So `packable` is never actually enough by itself; every negative arm fails the size
         * test that follows it, and `Packed` is never chosen. Confirmed by hand: six arms
         * `-8, 2, 7, 1, -1, 5` - `Packed`-shaped otherwise (six three-bit magnitudes) - compiled to
         * the general per-selector table instead, whose full-width load needs no mask or re-wrap to
         * answer `-8` and `-1` correctly. The day `constantBits` is widened to answer a value's
         * declared bit count instead of its full stored pattern, this reasoning breaks silently and
         * `Packed` would need the same `castTo`-is-a-re-wrap treatment `Affine` never needed.
         */
        case MapForm::Packed: {
            auto index = relative(word);

            if(plan.width > 1) {
                U32 shift = 0;
                while((U32(1) << shift) < plan.width) shift++;

                index = binary(word, index, imm(word, shift), LowerInst::Shl);
            }

            auto shifted = binary(word, imm(word, plan.immediate), index, LowerInst::Shr);

            // The top field owns the rest of the word, so there is nothing above it to mask off.
            if(U64(plan.span()) * plan.width < kMapImmediateBits) {
                shifted = binary(word, shifted, imm(word, (U64(1) << plan.width) - 1),
                                 LowerInst::And);
            }

            if(isBool) {
                values.add(result->id.index,
                           compare(shifted, imm(word, 0), LowerCmp::neq, manifest.type));
                return;
            }

            values.add(result->id.index, castTo(shifted, manifest.type, true, manifest.isSigned));
            return;
        }

        default:
            break;
    }

    /*
     * A run of slots in read-only data, indexed by the selector.
     *
     * The general form, and the one every result type reaches - a float, a record, a string. What a
     * slot holds is laid out by `materialize`, the same walk every other constant of that type is
     * laid out with, so a `String` entry is the two words `stringLiteral` built and nothing here
     * spells a field of one.
     *
     * A **memory-typed** result stops at the slot's address, which is what a value of one *is*: the
     * consumer copies from it exactly as it copies from a placed constant, and a load here would be
     * reading the first eight bytes of a record into a register.
     *
     * An **open** domain gets one extra slot holding the fallback and a branch-free `select` that
     * picks it. Two comparisons and a conditional move, so a `map` costs no block wherever it
     * stands - which is what lets the recognizer stay silent about control flow entirely.
     */
    auto span = plan.span();
    auto stride = strideOf(manifest);
    auto slots = U64(span) + (written.closed ? 0 : 1);

    auto bytes = U32(slots * stride);

    /*
     * The table is **built before it is named**, and looked up by its contents.
     *
     * The inliner clones a `map` per call site, so one `nameOf` opened three times is three
     * instructions carrying three equal tables - three runs of identical bytes in the image, which
     * is what a reader of `ConstructorMap.yana`'s data section saw. What makes two of them the same
     * is the bytes *and* the relocations in them, and neither exists until `materialize` has run; so
     * this builds an unregistered global, compares, and registers only what is new. A module has a
     * handful of these, so the walk is over a handful.
     */
    auto built = new (to.arena) LowerGlobal(StringId());
    built->mut = false;
    built->initialContents = ByteBuffer((Byte*)to.arena.alloc(bytes), bytes);
    set(built->initialContents.ptr, bytes, 0);

    for(I64 i = 0; i < span; i++) {
        materialize(result->occurrence,
                    mapSlot(entries, written.closed ? entries[0].value : written.fallback,
                            plan.lowest + i),
                    built, built->initialContents.ptr + U32(i) * stride);
    }

    if(!written.closed) {
        materialize(result->occurrence, written.fallback, built,
                    built->initialContents.ptr + U32(span) * stride);
    }

    LowerGlobal* table = nullptr;

    for(auto held: placedMapTables) {
        auto& candidate = *lower[held];
        if(candidate.initialContents.size() != bytes) continue;
        if(!sameBytes(candidate.initialContents.ptr, built->initialContents.ptr, bytes)) continue;
        if(candidate.relocations.size() != built->relocations.size()) continue;

        auto same = true;
        for(Size i = 0; i < built->relocations.size() && same; i++) {
            auto one = candidate.relocations.get(lower, i);
            auto two = built->relocations.get(lower, i);
            same = one.offset == two.offset && one.anchorRelative == two.anchorRelative &&
                   one.function == two.function && one.global == two.global;
        }

        if(!same) continue;

        table = &candidate;
        break;
    }

    if(!table) {
        built->name = erasedName(Tritium::format("map$%@", U64(placedConstantCount++)));
        to.globals.add(built->name, built - lower);
        to.globalOrder.push(built - lower);
        placedMapTables.push(built - lower);
        table = built;
    }

    auto wide = LowerType::Int64;
    auto index = relative(wide);

    if(!written.closed) {
        auto self = selectorValue - lower;
        auto type = selectorManifest.type;
        auto ordered = selectorManifest.isSigned;

        auto low = compare(self, imm(type, U64(plan.lowest)),
                           ordered ? LowerCmp::ige : LowerCmp::ge, LowerType::Int32);
        auto high = compare(self, imm(type, U64(plan.highest)),
                            ordered ? LowerCmp::ile : LowerCmp::le, LowerType::Int32);
        auto inside = binary(LowerType::Int32, low, high, LowerInst::And);

        auto chosen = block.addInst(lower, new (to.arena) LowerInstSelect(
            StringId(), index, imm(wide, U64(span)), inside, wide));

        index = ((LowerInstSingle*)chosen)->created().ptr - lower;
    }

    auto base = block.addInst(lower, new (to.arena) LowerInstGlobal(table->name, table - lower));
    auto address = binary(LowerType::Pointer,
                          &((LowerInstSingle*)base)->result - lower,
                          binary(wide, index, imm(wide, U64(stride)), LowerInst::Mul),
                          LowerInst::Add);

    if(manifest.inMemory) {
        values.add(result->id.index, address);
        return;
    }

    auto load = block.addInst(lower, new (to.arena) LowerInstLoad(
        address, StringId(), manifest.type, manifest.size, manifest.isSigned));

    values.add(result->id.index, &((LowerInstSingle*)load)->result - lower);
}

} // namespace ssa
