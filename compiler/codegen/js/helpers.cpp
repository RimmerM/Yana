#include "shared.h"

/*
 * The runtime helpers a file declares once and calls by name: saturating conversions, float bits,
 * rounding, bit counts, bit operations, rotates, byte swaps, division, a function value's drop, a
 * vector's growth and the word operations. Each `...HelperAt` names one on first use and the
 * `emit...` functions write the ones a file named, after its functions (`assemble.cpp`).
 */

namespace js {

namespace {

/*
 * The scratch pair a `Float`/`I32` bitcast goes through, and the two helpers that use it.
 *
 * A `Float` on this target is a `number` that `Math.fround` has made exactly representable as a
 * binary32, so its thirty-two bits are perfectly well defined and completely unreachable: no host
 * operator sees them. Two views over one `ArrayBuffer` is the only way, and one buffer per program
 * is enough, because the write and the read after it are the whole of the operation and nothing
 * runs between them.
 *
 * A helper *function* and not two statements at the call site, and that is a correctness matter
 * rather than a size one: the read of `$bci[0]` is not pure, so a binding holding one would be
 * eligible for the one-use inlining in opt.cpp and could be carried across the next bitcast's
 * write. An ordinary call is the form this backend already cannot move.
 */
static void ensureFloatBits(JsShared& g, bool wide) {
    if(wide) {
        if(g.doubleBitsBuffer.text) return;

        g.doubleBitsBuffer = uniqueName(g, "$bcd"_v, false);
        g.doubleBitsInts = uniqueName(g, "$bcl"_v, false);
        return;
    }

    if(g.floatBitsBuffer.text) return;

    g.floatBitsBuffer = uniqueName(g, "$bcf"_v, false);
    g.floatBitsInts = uniqueName(g, "$bci"_v, false);
}

static JsPtr<Expr> construct(JsShared& g, StringView type, JsPtr<Expr> argument) {
    auto node = make<CallExpr>(g, variable(g, literalName(g, type)));
    node->args.push(g.file.arena, argument);
    node->construct = true;
    return asExpr(g, node);
}

// `toBits` says which direction - a float in and its bits out, or the reverse - and `wide` says at
// which width, the 64-bit pair answering in `bigint` where the 32-bit one answers in `number`.
static Name floatBitsHelper(JsShared& g, bool toBits, bool wide) {
    auto& into = wide ? (toBits ? g.doubleToBitsHelper : g.bitsToDoubleHelper)
                      : (toBits ? g.floatToBitsHelper : g.bitsToFloatHelper);
    if(into.text) return into;

    ensureFloatBits(g, wide);
    into = uniqueName(g, wide ? (toBits ? "$bc$d2l"_v : "$bc$l2d"_v)
                              : (toBits ? "$bc$f2i"_v : "$bc$i2f"_v), false);
    return into;
}

} // namespace

/*
 * A test, as the value a `Bool` is here.
 *
 * A `Bool` is the number 0 or 1 on this target, never a host `boolean`. It was a `boolean` until
 * packing needed a reference into a bit range: one compiled `flip(&Bool)` serves a bit of a packed
 * record and a whole local alike, the read-modify-write that entails turns `true` into `1`, so the
 * two forms cannot coexist behind one reference and the number is the one that composes. Measured
 * before choosing: 0/1 is 105% on a field read and branch, 107% on `&&`, 108% on negation, 169% on
 * comparing two of them, identical in bytes per record, and it makes `[Bool]` a `Uint8Array` at one
 * byte per element against eight.
 *
 * Every consumer of one is written to that: a `Bool` reaching arithmetic is widened by the cast
 * tower's ternary (`convertExpr` in `structure.cpp`), a `[Bool]` is a `Uint8Array`, and a bit of a
 * packed word is stored as a number. A *comparison*, though, is a host boolean - JS has no other
 * answer for `===` - so an instruction that produces a `Bool` from one has to say which of the two
 * representations it means.
 *
 * It matters in exactly one place, and it is not a place anything is free to avoid: `===`. Every
 * other consumer reads only truthiness, where `true` and `1` agree, so the two forms coexisted for
 * as long as no program compared two `Bool`s - and `if truthy(x) == True` then answered *false*,
 * because `(x !== 0) === 1` is `true === 1`. Widening here rather than at the comparison is what
 * keeps that from being a rule every future consumer has to know: there is one representation, and
 * the two instructions that could produce the other one do not.
 *
 * Free in the emitted text wherever it is redundant, and the two rules that make it so are rules
 * about *positions*: `simplifyCondition` takes it off a condition, which is the shape it names and
 * the reason it names it, and `foldBitwiseOperand` takes it off an operand of a bitwise operator,
 * where `ToInt32` reads the boolean as the same two numbers. What is left is a `Bool` that is
 * genuinely a value - returned, stored, passed, or compared - and there it is the representation.
 */
JsPtr<Expr> boolNumber(JsShared& g, JsPtr<Expr> test) {
    return ternary(g, test, number(g, 1), number(g, 0));
}

Name saturatingLongHelperAt(JsShared& g, U16 held, bool isSigned) {
    auto integerIsSigned = isSigned;
    auto key = (U32(held) << 1) | U32(integerIsSigned);
    if(auto found = g.satHelpers.get(key)) return found.unwrap();

    char buffer[32];
    Size length = 0;
    buffer[length++] = '$';
    buffer[length++] = 's';
    buffer[length++] = 'a';
    buffer[length++] = 't';
    length += show(U64(held), buffer + length, sizeof(buffer) - length);
    buffer[length++] = integerIsSigned ? 'i' : 'u';

    auto name = uniqueName(g, StringView { buffer, length }, false);
    g.satHelpers.add(key, name);
    g.satHelperOrder.push(SatHelper { name, held, integerIsSigned });
    return name;
}

Name floatBitsHelperAt(JsShared& g, bool toBits, bool wide) {
    return floatBitsHelper(g, toBits, wide);
}

Name byteSwapHelperAt(JsShared& g, U32 bits) {
    auto slot = bits == 16 ? 0 : bits == 32 ? 1 : 2;
    if(g.byteSwapHelpers[slot].text) return g.byteSwapHelpers[slot];

    auto name = bits == 16 ? "$swap16"_v : bits == 32 ? "$swap32"_v : "$swap64"_v;
    g.byteSwapHelpers[slot] = uniqueName(g, name, false);
    return g.byteSwapHelpers[slot];
}

Name bitCountHelperAt(JsShared& g, BitFamily of, bool wide) {
    // The one case the host answers directly, and the reason this may answer no name at all:
    // `leadingZeros` at thirty-two bits is `Math.clz32`, whose argument goes through ToUint32.
    if(of == BitFamily::LeadingZeros && !wide) return Name {};

    auto want = [&](BitFamily at, bool atWide) {
        auto slot = bitCountHelperSlot(at, atWide);
        if(g.bitCountHelpers[slot].text) return;

        auto name = at == BitFamily::CountBits    ? (atWide ? "$popcount64"_v : "$popcount32"_v)
                  : at == BitFamily::LeadingZeros ? "$clz64"_v
                                                  : (atWide ? "$ctz64"_v : "$ctz32"_v);

        g.bitCountHelpers[slot] = uniqueName(g, name, false);
    };

    want(of, wide);

    // The 64-bit population and trailing counts are written over their operand's two halves and
    // call the 32-bit helper on each, so asking for one asks for the other. The leading count needs
    // no such partner: its halves go to `Math.clz32`, which is the host's own.
    if(wide && of != BitFamily::LeadingZeros) want(of, false);

    return g.bitCountHelpers[bitCountHelperSlot(of, wide)];
}

Name rotateHelperAt(JsShared& g, BitFamily of, U32 bits) {
    auto slot = rotateHelperSlot(of, bits);
    if(slot == kNoRotateHelper) return Name {};
    if(g.rotateHelpers[slot].text) return g.rotateHelpers[slot];

    char buffer[16];
    Size length = 0;
    buffer[length++] = '$';
    buffer[length++] = 'r';
    buffer[length++] = 'o';
    buffer[length++] = of == BitFamily::Rol ? 'l' : 'r';
    length += show(U64(bits), buffer + length, sizeof(buffer) - length);

    g.rotateHelpers[slot] = uniqueName(g, StringView { buffer, length }, false);
    return g.rotateHelpers[slot];
}

Name bitOpHelperAt(JsShared& g, BitFamily of, bool wide) {
    auto slot = bitOpHelperSlot(of, wide);
    if(g.bitOpHelpers[slot].text) return g.bitOpHelpers[slot];

    auto stem = of == BitFamily::BitsUpTo ? "$bzhi"_v
              : of == BitFamily::GatherBits ? "$pext"_v : "$pdep"_v;

    char buffer[16];
    Size length = 0;
    for(Size i = 0; i < stem.length; i++) buffer[length++] = stem.ptr[i];
    length += show(U64(wide ? 64 : 32), buffer + length, sizeof(buffer) - length);

    g.bitOpHelpers[slot] = uniqueName(g, StringView { buffer, length }, false);
    return g.bitOpHelpers[slot];
}

Name funDropHelperAt(JsShared& g) {
    if(!g.funDropHelper.text) g.funDropHelper = uniqueName(g, "$fd"_v, false);
    return g.funDropHelper;
}

void emitSaturationHelpers(JsShared& g) {
    if(g.satHelperOrder.size() == 0) return;

    emit(g, make<CommentStmt>(g, internText(g,
        "float to integer saturates, and `BigInt` throws rather than clamping - see codegen/js/helpers.cpp"_v)));

    for(Size i = 0; i < g.satHelperOrder.size(); i++) {
        auto helper = g.satHelperOrder[i];
        auto function = make<FunStmt>(g, helper.name);
        auto argument = literalName(g, "v"_v);
        auto value = variable(g, argument);
        function->args.push(g.file.arena, argument);

        auto range = saturationRange(helper.bits, helper.isSigned);

        auto low = helper.isSigned ? -(U64(1) << (helper.bits - 1)) : U64(0);
        auto high = helper.isSigned ? (U64(1) << (helper.bits - 1)) - 1 : ~U64(0) >> (64 - helper.bits);

        function->body = collect(g, [&] {
            // NaN first, since it answers neither comparison below and is the one input `BigInt`
            // refuses outright.
            auto isNaN = binary(g, BinaryOp::Ne, value, value);
            emit(g, make<IfStmt>(g, isNaN, collect(g, [&] {
                emit(g, make<ReturnStmt>(g, bigInt(g, 0, helper.isSigned)));
            }), StmtList {}));

            emit(g, make<IfStmt>(g, binary(g, BinaryOp::Le, value, number(g, range.low, false)),
                                 collect(g, [&] {
                emit(g, make<ReturnStmt>(g, bigInt(g, low, helper.isSigned)));
            }), StmtList {}));

            emit(g, make<IfStmt>(g, binary(g, BinaryOp::Ge, value, number(g, range.high, false)),
                                 collect(g, [&] {
                emit(g, make<ReturnStmt>(g, bigInt(g, high, helper.isSigned)));
            }), StmtList {}));

            emit(g, make<ReturnStmt>(g, globalCall(g, "BigInt"_v, hostCall(g, "Math"_v, "trunc"_v, value))));
        });

        emit(g, function);
    }
}

// One scratch pair and the up-to-two helpers over it, at one width. Two pairs rather than one
// serving both, because a double's sixty-four bits come back as a `bigint` through a
// `BigInt64Array` and a float's thirty-two as a `number` through an `Int32Array`.
static void emitFloatBitsPair(JsShared& g, bool wide) {
    auto buffer = wide ? g.doubleBitsBuffer : g.floatBitsBuffer;
    if(!buffer.text) return;

    auto ints = wide ? g.doubleBitsInts : g.floatBitsInts;

    emit(g, make<CommentStmt>(g, internText(g, wide
        ? "a double's bits, which are a `bigint` here - see codegen/js/helpers.cpp"_v
        : "a float's bits, which no host operator reaches - see codegen/js/helpers.cpp"_v)));

    // The float array first and the integer view over its buffer second, which is the whole of what
    // makes the two names one storage location. `BigInt64Array` is the wide pair's integer side and
    // is what makes the value coming back a `bigint` rather than a truncated number.
    emit(g, make<DeclStmt>(g, buffer,
                           construct(g, wide ? "Float64Array"_v : "Float32Array"_v, number(g, 1)), true));
    emit(g, make<DeclStmt>(g, ints,
                           construct(g, wide ? "BigInt64Array"_v : "Int32Array"_v,
                                     field(g, variable(g, buffer), literalName(g, "buffer"_v))),
                           true));

    for(auto toBits: { true, false }) {
        auto name = wide ? (toBits ? g.doubleToBitsHelper : g.bitsToDoubleHelper)
                         : (toBits ? g.floatToBitsHelper : g.bitsToFloatHelper);
        if(!name.text) continue;

        auto written = toBits ? buffer : ints;
        auto read = toBits ? ints : buffer;

        auto function = make<FunStmt>(g, name);
        auto argument = literalName(g, "v"_v);
        auto value = variable(g, argument);
        function->args.push(g.file.arena, argument);

        function->body = collect(g, [&] {
            emitExpr(g, assign(g, index(g, variable(g, written), 0), value));
            emit(g, make<ReturnStmt>(g, index(g, variable(g, read), 0)));
        });

        emit(g, function);
    }
}

void emitFloatBitsHelpers(JsShared& g) {
    emitFloatBitsPair(g, false);
    emitFloatBitsPair(g, true);
}

void emitRoundAwayHelper(JsShared& g) {
    if(!g.roundAwayHelper.text) return;

    emit(g, make<CommentStmt>(g, internText(g,
        "`Math.round` ties toward +Infinity and this language ties away from zero - see codegen/js/helpers.cpp"_v)));

    auto function = make<FunStmt>(g, g.roundAwayHelper);
    auto argument = literalName(g, "v"_v);
    auto value = variable(g, argument);
    function->args.push(g.file.arena, argument);

    /*
     * The negative arm is `-Math.round(-v)`, which is the reflection of ties-toward-+Infinity and so
     * is ties-toward-minus-infinity on the magnitude - away from zero, which is what is wanted.
     *
     * `v < 0` and not `v <= 0`: a negative zero takes the positive arm, where `Math.round(-0)` is
     * `-0` and the sign survives. Through the negative arm it would be `-Math.round(0)`, which is
     * `-0` as well - so this is a statement of intent rather than a correction, and the arm that
     * matters is the NaN one, which compares false and reaches `Math.round(NaN)`.
     */
    function->body = collect(g, [&] {
        emit(g, make<IfStmt>(g, binary(g, BinaryOp::Lt, value, number(g, 0, false)), collect(g, [&] {
            emit(g, make<ReturnStmt>(g, unary(g, UnaryOp::Neg,
                                              hostCall(g, "Math"_v, "round"_v,
                                                       unary(g, UnaryOp::Neg, value)))));
        }), StmtList {}));

        emit(g, make<ReturnStmt>(g, hostCall(g, "Math"_v, "round"_v, value)));
    });

    emit(g, function);
}

/*
 * The three byte reversals - see JsShared::byteSwapHelpers.
 *
 *     function $swap16(v) { return ((v & 255) << 8) | ((v >> 8) & 255) }
 *     function $swap32(v) { return ((v & 255) << 24) | (((v >> 8) & 255) << 16) | ((v >> 8) & 65280) | ((v >> 24) & 255) }
 *     function $swap64(v) { return ((v & 255n) << 56n) | ... | ((v >> 56n) & 255n) }
 *
 * One shape for all three, and `>>` throughout rather than `>>>`. That is not a preference: `>>>`
 * does not exist for a `bigint` at all, and for the two narrow widths it would be the *wrong* half
 * of the pair anyway - what makes a shift-down safe here is the mask that follows it, which clears
 * the sign bits a negative `I16` or `I32` brings down whichever shift brought them.
 *
 * Each byte is masked to itself before it is placed, so no term ever holds a bit belonging to
 * another. What the emitted text costs is a few operators more than the compact idiom; what it buys
 * is that the same three lines are right for a signed operand, an unsigned one and a `bigint`.
 *
 * The answer is the raw reversal at the helper's width, and the caller's coercion is what takes it
 * back to the type's own normal form - which is what lets one helper serve both signednesses.
 */
/*
 * The five bit-count helpers - see JsShared::bitCountHelpers.
 *
 *     function $popcount32(v) { var a = v - ((v >>> 1) & 0x55555555)
 *                               var b = (a & 0x33333333) + ((a >>> 2) & 0x33333333)
 *                               var c = (b + (b >>> 4)) & 0x0f0f0f0f
 *                               return Math.imul(c, 0x01010101) >>> 24 }
 *     function $ctz32(v)      { return v ? 31 - Math.clz32(v & -v) : 32 }
 *     function $popcount64(v) { return BigInt($popcount32(Number(v & 0xffffffffn))
 *                                           + $popcount32(Number((v >> 32n) & 0xffffffffn))) }
 *     function $clz64(v)      { var h = Number((v >> 32n) & 0xffffffffn)
 *                               return BigInt(h ? Math.clz32(h)
 *                                               : 32 + Math.clz32(Number(v & 0xffffffffn))) }
 *     function $ctz64(v)      { var l = Number(v & 0xffffffffn)
 *                               return BigInt(l ? $ctz32(l)
 *                                               : 32 + $ctz32(Number((v >> 32n) & 0xffffffffn))) }
 *
 * ## The 32-bit pair
 *
 * `$popcount32` is the five-step SWAR fold, and every operator in it is one JS has at exactly 32
 * bits: `>>>` and `&` are ToUint32 and ToInt32, so a negative `I32` folds as its two's complement
 * pattern with no coercion written anywhere, and `Math.imul` is the one exact 32-bit multiply the
 * host has - the ordinary `*` would round the byte-summing product, whose true value passes 2^53.
 * The final `>>> 24` is what makes the answer non-negative at every input.
 *
 * `$ctz32` is `Math.clz32` of the lowest set bit isolated, which is `v & -v` - the shortest thing
 * the host expresses, since it has no trailing-zero count of its own. The ternary is not the zero
 * guard it looks like: `31 - Math.clz32(0)` is -1, and the language's answer at zero is the width.
 *
 * ## The 64-bit three, and why they are written over halves
 *
 * A `bigint` has bitwise operators, so each of these *could* be a loop or a shift chain in the
 * `bigint` domain. They are not, because `bigint` arithmetic is heap arithmetic on every engine and
 * `Math.clz32` is a single machine instruction behind an intrinsic. Splitting once - two masks, two
 * `Number` conversions - and then working in the fast domain is a bounded number of cheap operations
 * where the loop is an unbounded number of expensive ones.
 *
 * `v & 0xffffffffn` is the low half for a negative operand as much as for a positive one, because a
 * `bigint`'s bitwise operators are defined on the two's complement representation extended
 * infinitely - so an `I64` needs no separate case, exactly as the byte reversal above needs none.
 *
 * The result is a `bigint` because the operand's type is 64 bits wide and that is what a value of it
 * is on this target. It is at most 64 either way; the conversion is about the type, not the range.
 */
void emitBitCountHelpers(JsShared& g) {
    auto define = [&](BitFamily of, bool wide, void (*body)(JsShared&, JsPtr<Expr>)) {
        auto& name = g.bitCountHelpers[bitCountHelperSlot(of, wide)];
        if(!name.text) return;

        auto function = make<FunStmt>(g, name);
        auto argument = literalName(g, "v"_v);
        function->args.push(g.file.arena, argument);
        function->body = collect(g, [&] { body(g, variable(g, argument)); });

        emit(g, function);
    };

    // The one thing the three wide bodies share: a half of the operand, as a `number`.
    static auto half = [](JsShared& g, JsPtr<Expr> value, bool high) {
        auto shifted = high ? binary(g, BinaryOp::Sar, value, bigInt(g, 32, false)) : value;
        return globalCall(g, "Number"_v,
                          binary(g, BinaryOp::And, shifted, bigInt(g, 0xffffffff, false)));
    };

    define(BitFamily::CountBits, false, [](JsShared& g, JsPtr<Expr> v) {
        auto step = [&](JsPtr<Expr> of, U32 shift, U32 mask, bool before) {
            auto down = binary(g, BinaryOp::Shr, of, number(g, F64(shift)));
            return before ? binary(g, BinaryOp::And, down, number(g, F64(mask)))
                          : binary(g, BinaryOp::Add, of, down);
        };

        auto a = declare(g, literalName(g, "a"_v),
                         binary(g, BinaryOp::Sub, v, step(v, 1, 0x55555555, true)));
        auto b = declare(g, literalName(g, "b"_v),
                         binary(g, BinaryOp::Add, binary(g, BinaryOp::And, a, number(g, F64(0x33333333))),
                                step(a, 2, 0x33333333, true)));
        auto c = declare(g, literalName(g, "c"_v),
                         binary(g, BinaryOp::And, step(b, 4, 0, false), number(g, F64(0x0f0f0f0f))));

        emit(g, make<ReturnStmt>(g, binary(g, BinaryOp::Shr,
                                           hostCall(g, "Math"_v, "imul"_v, c, number(g, F64(0x01010101))),
                                           number(g, 24))));
    });

    define(BitFamily::TrailingZeros, false, [](JsShared& g, JsPtr<Expr> v) {
        auto lowest = binary(g, BinaryOp::And, v, unary(g, UnaryOp::Neg, v));
        auto index = binary(g, BinaryOp::Sub, number(g, 31), hostCall(g, "Math"_v, "clz32"_v, lowest));

        emit(g, make<ReturnStmt>(g, ternary(g, v, index, number(g, 32))));
    });

    define(BitFamily::CountBits, true, [](JsShared& g, JsPtr<Expr> v) {
        auto count = [&](bool high) {
            auto node = make<CallExpr>(g, variable(g, g.bitCountHelpers[bitCountHelperSlot(BitFamily::CountBits, false)]));
            node->args.push(g.file.arena, half(g, v, high));
            node->pure = true;
            return asExpr(g, node);
        };

        emit(g, make<ReturnStmt>(g, globalCall(g, "BigInt"_v,
                                               binary(g, BinaryOp::Add, count(false), count(true)))));
    });

    define(BitFamily::LeadingZeros, true, [](JsShared& g, JsPtr<Expr> v) {
        auto high = declare(g, literalName(g, "h"_v), half(g, v, true));
        auto below = binary(g, BinaryOp::Add, number(g, 32),
                            hostCall(g, "Math"_v, "clz32"_v, half(g, v, false)));

        emit(g, make<ReturnStmt>(g, globalCall(g, "BigInt"_v,
            ternary(g, high, hostCall(g, "Math"_v, "clz32"_v, high), below))));
    });

    define(BitFamily::TrailingZeros, true, [](JsShared& g, JsPtr<Expr> v) {
        auto scan = [&](JsPtr<Expr> of) {
            auto node = make<CallExpr>(g, variable(g, g.bitCountHelpers[bitCountHelperSlot(BitFamily::TrailingZeros, false)]));
            node->args.push(g.file.arena, of);
            node->pure = true;
            return asExpr(g, node);
        };

        auto low = declare(g, literalName(g, "l"_v), half(g, v, false));
        auto above = binary(g, BinaryOp::Add, number(g, 32), scan(half(g, v, true)));

        emit(g, make<ReturnStmt>(g, globalCall(g, "BigInt"_v, ternary(g, low, scan(low), above))));
    });
}

/*
 * The ten rotations - see JsShared::rotateHelpers.
 *
 *     function $rol32(v, c) { var n = c & 31;  return n ? (v << n) | (v >>> (32 - n)) : v }
 *     function $rol16(v, c) { var n = c & 15;  var x = v & 65535
 *                             return n ? (x << n) | (x >>> (16 - n)) : v }
 *     function $rol53(v, c) { var n = (c < 0 ? c + 9007199254740992 : c) % 53
 *                             return n ? $w53i$or($w53i$shl(v, n), $w53i$shr(v, 53 - n)) : v }
 *     function $rol64(v, c) { var n = BigInt.asUintN(64, c) % 64n
 *                             return n ? (v << n) | (BigInt.asUintN(64, v) >> (64n - n)) : v }
 *
 * ## The ternary is the whole argument, and it is not the zero case
 *
 * Every body guards on `n` and answers the operand unchanged for a zero count. That is not a
 * shortcut for a common case: it is what keeps the *other* arm's shift distances inside `[1, w-1]`,
 * where every host domain agrees about them. A count of zero would otherwise want a shift by the
 * full width, which is where the three domains stop agreeing - JS masks `>>>` to five bits, wide.cpp
 * would be asked for a distance it does not describe, and a `bigint` shift by 64 is the one that
 * *is* well defined. Guarding once is shorter than arguing three times.
 *
 * ## The modulus, and where the width is not a power of two
 *
 * Two of the five widths mask (`c & (w - 1)`), because a power of two's mask is its modulus and it
 * is the same operation the machine performs on its count register. `WideInt` is 53 bits and is the
 * one that cannot: the count is read as unsigned - which is what a negative `number` in the band
 * means as a bit pattern, and what `llvm.fshl` and the machine both do - and then reduced. The
 * native lowering does exactly this pair, with `maskToWidth` and a remainder.
 *
 * ## The three domains
 *
 * Below 33 bits the host operators are the operation and the mask is where the width lives: `v` may
 * be a negative `number` standing for a 16-bit pattern, so it is masked before either shift and the
 * caller's coercion signs the answer. At exactly 32 no mask is needed - the `int32` the operators
 * work in *is* the value. From 33 to 53 the operators do not exist and wide.cpp's helpers are what
 * a shift is; above that a value is a `bigint`, whose `>>` is arithmetic, so the right half reads
 * the unsigned form first for the reason the scalar `shr` does.
 */
/*
 * The six BMI2 helpers - see JsShared::bitOpHelpers.
 *
 *     function $bzhi32(v, n) { return (n >>> 0) < 32 ? v & ((1 << n) - 1) : v }
 *     function $bzhi64(v, n) { var u = BigInt.asUintN(64, n)
 *                              return u < 64n ? v & ((1n << u) - 1n) : v }
 *     function $pext32(v, m) { var o = 0; var b = 1
 *                              $L: for(;;) { if(!m) break $L
 *                                            var l = m & -m
 *                                            o = o | ((v & l) ? b : 0)
 *                                            m = m ^ l; b = b << 1 }
 *                              return o }
 *     function $pdep32(v, m) { ... o = o | ((v & b) ? l : 0) ... }
 *
 * ## `bitsUpTo`, and why the guard is a ternary rather than a mask
 *
 * The operation saturates: a count at or above the width answers the value unchanged (`bitsUpTo` in
 * `lib/Core/Bits.yana` rules on it). JS masks a shift count to five bits, so `1 << 32` is 1 rather
 * than 0 and the mask arithmetic would answer zero where the language answers everything - which is
 * exactly the disagreement `bzhi`'s byte-wide count causes on the machine, arriving here through a
 * different door. The ternary is what makes the shift distance unreachable outside `[0, w-1]`, and
 * it is short-circuiting, so the out-of-range shift is never evaluated at all.
 *
 * `n >>> 0` is the unsigned reading, which is what the ruling says the count is: a negative `I32`
 * count is a large number and answers the value, not the value masked by a small one.
 *
 * ## The permutations, and the one line that keeps the loop finite
 *
 * Both loops walk the set bits of the mask lowest-first, and both terminate because `m ^= m & -m`
 * clears one bit per iteration. At 64 bits that is only true of the *unsigned* reading: a `bigint`'s
 * bitwise operators are defined on a two's complement representation extended infinitely, so a
 * negative mask has infinitely many set bits and the loop would never end. `BigInt.asUintN(64, m)`
 * before the loop is the whole of the fix, and it is the same conversion the 64-bit `shr` makes for
 * the same reason.
 *
 * A loop and not the parallel-suffix network the x64 backend expands to, because the trade is the
 * other way round here: there is no instruction to lose to, an engine's `bigint` operations are heap
 * arithmetic, and the loop runs once per set bit where the network runs five or six rounds whatever
 * the mask is.
 *
 * `var` inside the body rather than `let`, as everywhere in this backend: these are function-scoped
 * and re-entering the block re-assigns rather than re-declares.
 */
void emitBitOpHelpers(JsShared& g) {
    auto define = [&](BitFamily of, bool wide, void (*body)(JsShared&, bool, JsPtr<Expr>, JsPtr<Expr>)) {
        auto& name = g.bitOpHelpers[bitOpHelperSlot(of, wide)];
        if(!name.text) return;

        auto function = make<FunStmt>(g, name);
        // The second parameter is a *count* for one of the three and a *mask* for the other two,
        // which is worth two letters in the emitted text: `$bzhi32(v, n)` and `$pext32(v, m)`.
        auto valueName = literalName(g, "v"_v);
        auto otherName = literalName(g, of == BitFamily::BitsUpTo ? "n"_v : "m"_v);
        function->args.push(g.file.arena, valueName);
        function->args.push(g.file.arena, otherName);

        function->body = collect(g, [&] {
            body(g, wide, variable(g, valueName), variable(g, otherName));
        });

        emit(g, function);
    };

    // A constant in whichever domain the width works in - a `number` below 33 bits and a `bigint`
    // above, which is the same split every arithmetic emitter here makes.
    static auto constant = [](JsShared& g, bool wide, U64 n) {
        return wide ? bigInt(g, n, false) : number(g, F64(n));
    };

    static auto bitsUpTo = [](JsShared& g, bool wide, JsPtr<Expr> value, JsPtr<Expr> count) {
        auto width = U64(wide ? 64 : 32);

        // The count as an unsigned number, which is what the operation's ruling reads it as.
        auto unsignedCount = wide
            ? declare(g, literalName(g, "u"_v),
                      hostCall(g, "BigInt"_v, "asUintN"_v, number(g, 64), count))
            : binary(g, BinaryOp::Shr, count, constant(g, false, 0));

        auto small = binary(g, BinaryOp::Lt, unsignedCount, constant(g, wide, width));
        auto bit = binary(g, BinaryOp::Shl, constant(g, wide, 1), unsignedCount);
        auto mask = binary(g, BinaryOp::Sub, bit, constant(g, wide, 1));

        emit(g, make<ReturnStmt>(g, ternary(g, small,
                                            binary(g, BinaryOp::And, value, mask), value)));
    };

    static auto permute = [](JsShared& g, bool wide, bool gather, JsPtr<Expr> value, JsPtr<Expr> maskArg) {
        auto zero = constant(g, wide, 0);
        auto one = constant(g, wide, 1);

        // The mask read unsigned, without which a negative 64-bit one has infinitely many set bits
        // and the loop below never ends. Under a name of its own rather than over the parameter's,
        // which `var` would have allowed and which reads as a mistake.
        auto mask = wide
            ? declare(g, literalName(g, "u"_v),
                      hostCall(g, "BigInt"_v, "asUintN"_v, number(g, 64), maskArg))
            : maskArg;

        auto outName = literalName(g, "o"_v);
        auto bitName = literalName(g, "b"_v);
        auto out = declare(g, outName, zero);
        auto bit = declare(g, bitName, one);

        auto label = literalName(g, "$bp"_v);

        auto body = collect(g, [&] {
            auto done = collect(g, [&] { emit(g, make<BreakStmt>(g, label)); });
            emit(g, make<IfStmt>(g, unary(g, UnaryOp::Not, mask), done, StmtList {}));

            auto low = declare(g, literalName(g, "l"_v),
                               binary(g, BinaryOp::And, mask, unary(g, UnaryOp::Neg, mask)));

            // Which bit is tested and which is contributed is the whole of the difference between
            // the two directions: an extract asks the mask and answers the next low position, and a
            // deposit asks the next low position and answers the mask's.
            auto tested = binary(g, BinaryOp::And, value, gather ? low : bit);
            auto contributed = ternary(g, tested, gather ? bit : low, constant(g, wide, 0));

            emitExpr(g, assign(g, out, binary(g, BinaryOp::Or, out, contributed)));
            emitExpr(g, assign(g, mask, binary(g, BinaryOp::Xor, mask, low)));
            emitExpr(g, assign(g, bit, binary(g, BinaryOp::Shl, bit, constant(g, wide, 1))));
        });

        emit(g, make<LabelledStmt>(g, label, asStmt(g, make<LoopStmt>(g, body))));
        emit(g, make<ReturnStmt>(g, out));
    };

    define(BitFamily::BitsUpTo, false, [](JsShared& g, bool wide, JsPtr<Expr> v, JsPtr<Expr> n) { bitsUpTo(g, wide, v, n); });
    define(BitFamily::BitsUpTo, true, [](JsShared& g, bool wide, JsPtr<Expr> v, JsPtr<Expr> n) { bitsUpTo(g, wide, v, n); });
    define(BitFamily::GatherBits, false, [](JsShared& g, bool wide, JsPtr<Expr> v, JsPtr<Expr> m) { permute(g, wide, true, v, m); });
    define(BitFamily::GatherBits, true, [](JsShared& g, bool wide, JsPtr<Expr> v, JsPtr<Expr> m) { permute(g, wide, true, v, m); });
    define(BitFamily::ScatterBits, false, [](JsShared& g, bool wide, JsPtr<Expr> v, JsPtr<Expr> m) { permute(g, wide, false, v, m); });
    define(BitFamily::ScatterBits, true, [](JsShared& g, bool wide, JsPtr<Expr> v, JsPtr<Expr> m) { permute(g, wide, false, v, m); });
}

void emitRotateHelpers(JsShared& g) {
    auto define = [&](BitFamily of, U32 bits) {
        auto slot = rotateHelperSlot(of, bits);
        auto& name = g.rotateHelpers[slot];
        if(!name.text) return;

        auto function = make<FunStmt>(g, name);
        auto valueName = literalName(g, "v"_v);
        auto countName = literalName(g, "c"_v);
        function->args.push(g.file.arena, valueName);
        function->args.push(g.file.arena, countName);

        auto left = of == BitFamily::Rol;
        auto wide = bits == 64;

        function->body = collect(g, [&] {
            auto value = variable(g, valueName);
            auto count = variable(g, countName);
            auto constant = [&](U64 n) { return wide ? bigInt(g, n, false) : number(g, F64(n)); };

            // The count, reduced. A power of two masks; 53 reads the count unsigned and divides.
            JsPtr<Expr> reduced;

            if(wide) {
                reduced = binary(g, BinaryOp::Rem,
                                 hostCall(g, "BigInt"_v, "asUintN"_v, number(g, 64), count),
                                 constant(64));
            } else if(bits == 53) {
                auto unsignedCount = ternary(g, binary(g, BinaryOp::Lt, count, number(g, 0)),
                                             binary(g, BinaryOp::Add, count, number(g, 9007199254740992.0)),
                                             count);

                reduced = binary(g, BinaryOp::Rem, unsignedCount, number(g, F64(bits)));
            } else {
                reduced = binary(g, BinaryOp::And, count, number(g, F64(bits - 1)));
            }

            auto n = declare(g, literalName(g, "n"_v), reduced);
            auto back = binary(g, BinaryOp::Sub, constant(bits), n);

            // The value's own bits, which below 32 are not all of the register's: a negative
            // `number` standing for a narrow pattern carries ones above its width, and a rotation
            // would bring them round into the answer where a shift only pushes them out.
            JsPtr<Expr> bitsOfValue = value;
            if(bits < 32) {
                bitsOfValue = declare(g, literalName(g, "x"_v),
                                      binary(g, BinaryOp::And, value, number(g, F64((U64(1) << bits) - 1))));
            }

            JsPtr<Expr> up, down;

            if(bits <= 32) {
                up = binary(g, BinaryOp::Shl, bitsOfValue, left ? n : back);
                down = binary(g, BinaryOp::Shr, bitsOfValue, left ? back : n);
            } else if(bits == 53) {
                up = wideCallAt(g, WideOp::Shl, bits, true, value, left ? n : back);
                down = wideCallAt(g, WideOp::Shr, bits, true, value, left ? back : n);
            } else {
                // `>>` on a `bigint` is arithmetic and there is no `>>>`, so the half travelling down
                // reads the unsigned form - the scalar `shr` does the same thing at the same width.
                auto unsignedValue = hostCall(g, "BigInt"_v, "asUintN"_v, number(g, 64), value);

                up = binary(g, BinaryOp::Shl, value, left ? n : back);
                down = binary(g, BinaryOp::Sar, unsignedValue, left ? back : n);
            }

            auto joined = bits == 53
                ? wideCallAt(g, WideOp::Or, bits, true, up, down)
                : binary(g, BinaryOp::Or, up, down);

            emit(g, make<ReturnStmt>(g, ternary(g, n, joined, value)));
        });

        emit(g, function);
    };

    static const U32 widths[] = { 8, 16, 32, 53, 64 };

    for(auto bits: widths) define(BitFamily::Rol, bits);
    for(auto bits: widths) define(BitFamily::Ror, bits);
}

void emitByteSwapHelpers(JsShared& g) {
    auto define = [&](Size slot, U32 bits) {
        auto& name = g.byteSwapHelpers[slot];
        if(!name.text) return;

        auto function = make<FunStmt>(g, name);
        auto argument = literalName(g, "v"_v);
        auto value = variable(g, argument);
        function->args.push(g.file.arena, argument);

        auto wide = bits == 64;
        auto constant = [&](U64 n) { return wide ? bigInt(g, n, false) : number(g, F64(n)); };
        auto shift = [&](BinaryOp op, JsPtr<Expr> of, U32 by) {
            return by ? binary(g, op, of, constant(by)) : of;
        };

        JsPtr<Expr> result = nullptr;

        for(U32 i = 0; i < bits / 8; i++) {
            // Where this byte is and where the reversal puts it. A byte travelling up is masked
            // where it stands and shifted after; one travelling down is shifted first and masked in
            // its new place, so that neither the mask nor the shifted value is ever wider than the
            // type.
            auto from = i * 8;
            auto to = bits - 8 - from;

            auto term = to >= from
                ? shift(BinaryOp::Shl,
                        binary(g, BinaryOp::And, shift(BinaryOp::Sar, value, from), constant(0xff)), to)
                : binary(g, BinaryOp::And, shift(BinaryOp::Sar, value, from - to), constant(U64(0xff) << to));

            result = result ? binary(g, BinaryOp::Or, result, term) : term;
        }

        function->body = collect(g, [&] { emit(g, make<ReturnStmt>(g, result)); });
        emit(g, function);
    };

    define(0, 16);
    define(1, 32);
    define(2, 64);
}

/*
 * `function $div(a, b) { return b ? a / b : b }` and `function $rem(a, b) { return b ? a % b : a }`.
 *
 * The zero arm of the division returns the *divisor* rather than a written zero, which is what lets
 * one function serve a `number` and a `bigint` both: `0` and `0n` are each their own type's zero,
 * and the caller's coercion is happy with either. `b` as the condition rather than `b !== 0` for the
 * same reason - falsy is true of both zeros and of no other integer this backend produces.
 *
 * `a / b` is left untruncated deliberately. Every caller already applies the truncation its band
 * needs - `Math.trunc` in the 33-to-53-bit band, a bitwise coercion below it, and nothing at all for
 * a `bigint`, whose division truncates itself - so doing it here would be doing it twice.
 */
void emitDivisionHelpers(JsShared& g) {
    auto define = [&](Name& name, BinaryOp op, bool zeroIsDividend) {
        if(!name.text) return;

        auto function = make<FunStmt>(g, name);
        auto left = literalName(g, "a"_v);
        auto right = literalName(g, "b"_v);
        function->args.push(g.file.arena, left);
        function->args.push(g.file.arena, right);

        auto a = variable(g, left);
        auto b = variable(g, right);

        function->body = collect(g, [&] {
            emit(g, make<ReturnStmt>(g, ternary(g, b, binary(g, op, a, b),
                                                zeroIsDividend ? a : b)));
        });

        emit(g, function);
    };

    if(g.divideByZeroHelper.text || g.remainderByZeroHelper.text) {
        emit(g, make<CommentStmt>(g, internText(g,
            "`x / 0` is 0 and `x % 0` is x - see doc/spec/types.md"_v)));
    }

    define(g.divideByZeroHelper, BinaryOp::Div, false);
    define(g.remainderByZeroHelper, BinaryOp::Rem, true);
}

/*
 * `function $grow(a, n) { var b = new a.constructor(n); b.set(a); return b; }`
 *
 * One helper for every element type, because the array carries its own constructor. `.set` is the
 * typed array's own bulk copy and is what the engine has a memcpy behind; writing the loop here
 * would be the same operation spelled so that it cannot be recognised.
 */
/*
 * A function value's release - `JsShared::funDropHelper`, and `lower/fun.cpp`'s `funTeardown` read
 * on a target whose collector owns the block.
 *
 * `function $fd(p) { var e = p.$e; if (e !== null) { var h = p.$c.$h; if (h) h(e); } }`
 *
 * Two questions in the native order and for the native reasons. The environment word first: null is
 * a lambda that captured nothing, a plain function named as a value, and the empty pair a boxed
 * iterator member holds, and only a lambda *with* an environment has a header - so the null test is
 * what keeps the read off a plain function's thunk. The header second: it is the lambda's own
 * answer to "do my captures owe anything", hung on the code word once beside the declaration rather
 * than stored per closure.
 *
 * What is not here is the native half's third act. `funTeardown` ends in `freeHeap`, which is what
 * hands a heap-placed environment's block back; here the collector does that, so the header is
 * attached only where the captures owe an **authored** teardown and a lambda whose environment owes
 * nothing has no header at all. That is §3.9 read at the one place a function value could have been
 * an exception to it.
 */
void emitFunDropHelper(JsShared& g) {
    if(!g.funDropHelper.text) return;

    emit(g, make<CommentStmt>(g, internText(g,
        "a function value's release - the environment word, then the lambda's own header"_v)));

    auto function = make<FunStmt>(g, g.funDropHelper);
    auto pair = literalName(g, "p"_v);
    auto environment = literalName(g, "e"_v);
    auto header = literalName(g, "h"_v);

    function->args.push(g.file.arena, pair);

    function->body = collect(g, [&] {
        emit(g, make<DeclStmt>(g, environment, field(g, variable(g, pair), g.envField), true));

        auto then = collect(g, [&] {
            emit(g, make<DeclStmt>(g, header,
                                   field(g, field(g, variable(g, pair), g.codeField),
                                         g.headerField), true));

            auto run = collect(g, [&] {
                emitExpr(g, call(g, variable(g, header), variable(g, environment)));
            });

            emit(g, make<IfStmt>(g, variable(g, header), run, StmtList {}));
        });

        // `!=` rather than `!==`, for `BinaryOp::LooseNe`'s own reason: a lambda that captured
        // nothing carries `null` and a pair whose word nothing attached reads back `undefined`, and
        // one test is meant to cover both.
        emit(g, make<IfStmt>(g, binary(g, BinaryOp::LooseNe, variable(g, environment),
                                       nullValue(g)), then, StmtList {}));
    });

    emit(g, function);
}

void emitGrowHelper(JsShared& g) {
    if(!g.growHelper.text) return;

    emit(g, make<CommentStmt>(g, internText(g,
        "a typed array's growth - see Implementation-Containers.md, section 14"_v)));

    auto function = make<FunStmt>(g, g.growHelper);
    auto array = literalName(g, "a"_v);
    auto capacity = literalName(g, "n"_v);
    auto grown = literalName(g, "b"_v);

    function->args.push(g.file.arena, array);
    function->args.push(g.file.arena, capacity);

    function->body = collect(g, [&] {
        auto made = make<CallExpr>(g, field(g, variable(g, array), literalName(g, "constructor"_v)));
        made->args.push(g.file.arena, variable(g, capacity));
        made->construct = true;

        emit(g, make<DeclStmt>(g, grown, asExpr(g, made), true));

        auto copy = make<CallExpr>(g, field(g, variable(g, grown), literalName(g, "set"_v)));
        copy->args.push(g.file.arena, variable(g, array));
        emitExpr(g, asExpr(g, copy));

        emit(g, make<ReturnStmt>(g, variable(g, grown)));
    });

    emit(g, function);
}

/*
 * The word transfers, and the one thing that makes them worth having:
 *
 *     function $view(a) { return a.$dv || (a.$dv = new DataView(a.buffer, a.byteOffset)); }
 *     function $readU32(a, i, e) { return $view(a).getUint32(i, e); }
 *     function $writeU64(a, i, v, e) { $view(a).setBigUint64(i, v, e); }
 *
 * **The view is built once per array and kept on it.** That is not a tidy-up: `new DataView` at each
 * call measures 134 ms against the 9.7 ms of the shift chain these replace, so a form without a
 * cache is fifteen times *worse* than doing nothing. Nor is the cache free to site anywhere - a
 * one-entry memo on `.buffer` is 33 ms, and 352 ms once a loop alternates between two arrays, which
 * is what a digest reading a message and writing a pad does; a `WeakMap` is 27 ms on a write where
 * four stores are 4.6. The property is the only one of the four that is not slower than the shifts.
 *
 * It costs the array nothing measurable: an element sum over a `Uint8Array` carrying `$dv` is 8.4 ms
 * against 8.4 ms without it, and the shift chain over one is 9.8 against 9.7. The map transitions
 * once and stays monomorphic.
 *
 * **What it buys is the 64-bit pair.** At 32 bits the host and the shifts measure the same (9.7 both
 * ways on a read; the write is 2.3 against 4.6). At 64 bits a `U64` is a `bigint` here, so the
 * alternative to `getBigUint64` is composing one out of two 32-bit halves - 13.6 ms against 7.1 -
 * and the alternative to `setBigUint64` is eight stores and eight shifts in the `bigint` domain,
 * which is 52 ms against 7.1. All figures Node 24, 64 KB, best of repeated runs.
 *
 * `$view` is a call rather than the expression inlined into each of the four, for the reason the
 * user of this file will care about: it is the same speed - 9.8 ms against 9.7 - and it is one
 * function instead of four copies of the same three operations.
 */
void emitWordHelpers(JsShared& g) {
    auto wanted = false;
    for(U32 i = 0; i < 3; i++) {
        wanted = wanted || g.readWordHelper[i].text || g.writeWordHelper[i].text;
    }

    if(!wanted) return;

    emit(g, make<CommentStmt>(g, internText(g,
        "the machine words of a binary format, through one DataView per array"_v)));

    g.viewHelper = uniqueName(g, "$view"_v, false);

    // `function $view(a) { return a.$dv || (a.$dv = new DataView(a.buffer, a.byteOffset)); }`
    {
        auto function = make<FunStmt>(g, g.viewHelper);
        auto array = literalName(g, "a"_v);
        function->args.push(g.file.arena, array);

        function->body = collect(g, [&] {
            auto slot = field(g, variable(g, array), literalName(g, "$dv"_v));

            auto made = make<CallExpr>(g, variable(g, literalName(g, "DataView"_v)));
            made->args.push(g.file.arena, field(g, variable(g, array), literalName(g, "buffer"_v)));
            made->args.push(g.file.arena, field(g, variable(g, array), literalName(g, "byteOffset"_v)));
            made->construct = true;

            auto stored = asExpr(g, make<AssignExpr>(g, slot, asExpr(g, made)));
            emit(g, make<ReturnStmt>(g, asExpr(g, make<BinaryExpr>(g, BinaryOp::LogicalOr, slot, stored))));
        });

        emit(g, function);
    }

    auto define = [&](Name name, StringView accessor, bool read) {
        if(!name.text) return;

        auto function = make<FunStmt>(g, name);
        auto array = literalName(g, "a"_v);
        auto index = literalName(g, "i"_v);
        auto amount = literalName(g, "v"_v);
        auto order = literalName(g, "e"_v);

        function->args.push(g.file.arena, array);
        function->args.push(g.file.arena, index);
        if(!read) function->args.push(g.file.arena, amount);
        function->args.push(g.file.arena, order);

        function->body = collect(g, [&] {
            auto view = make<CallExpr>(g, variable(g, g.viewHelper));
            view->args.push(g.file.arena, variable(g, array));

            auto call = make<CallExpr>(g, field(g, asExpr(g, view), literalName(g, accessor)));
            call->args.push(g.file.arena, variable(g, index));
            if(!read) call->args.push(g.file.arena, variable(g, amount));
            call->args.push(g.file.arena, variable(g, order));

            if(read) emit(g, make<ReturnStmt>(g, asExpr(g, call)));
            else emitExpr(g, asExpr(g, call));
        });

        emit(g, function);
    };

    define(g.readWordHelper[0], "getUint16"_v, true);
    define(g.readWordHelper[1], "getUint32"_v, true);
    define(g.readWordHelper[2], "getBigUint64"_v, true);
    define(g.writeWordHelper[0], "setUint16"_v, false);
    define(g.writeWordHelper[1], "setUint32"_v, false);
    define(g.writeWordHelper[2], "setBigUint64"_v, false);
}

} // namespace js
