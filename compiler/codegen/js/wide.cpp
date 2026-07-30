#include "build.h"

/*
 * Integers of 33 to 53 bits - the band where a host `number` still holds every value exactly but
 * the operators that would manipulate one have stopped existing.
 *
 * JS gives 32-bit `&`, `|`, `^`, `<<` and `>>>`, and a `number` that holds 53 consecutive integer
 * bits. Between those two facts sits this file. A value here is an ordinary `number` carrying its
 * mathematical value - non-negative for an unsigned type, two's complement in range for a signed
 * one, exactly as the 32-bit tower already does - and every operation JS cannot express directly is
 * a call to a helper emitted once per (operation, width, signedness) actually used.
 *
 * Why a helper rather than the inline expansion: the split form of `a & b` is eight operations, and
 * a program that uses one of these types uses it everywhere. `opt.cpp` then fuses *chains* of them
 * back into one inline unpack-operate-pack, which is where the measured win is - see
 * benchmark/bits53-js. So the compact form is the default and the expanded form is what the
 * peephole earns.
 *
 * Everything here rests on three exactness facts about doubles, and each is why the corresponding
 * spelling was chosen over an obvious alternative:
 *
 *  - `a >>> 0` is ToUint32, which is a *modulo*, so it is the low half of the two's complement
 *    representation for a negative value as much as for a positive one. No division needed.
 *  - `a * 2**-32` is an exact rescaling of the mantissa, so truncating it gives the high half.
 *    The reciprocal rather than `/ 2**32` because it measured 10% faster and, both being powers of
 *    two, they are the same value rather than merely close. Signed values need `Math.floor` where
 *    unsigned ones can use `| 0`, because truncation toward zero is the wrong rounding below zero.
 *  - `a + b` is exact whenever the true sum is below 2^53, which is what lets a wrapping add be a
 *    comparison and a subtraction rather than a remainder. `%` on doubles is a real floating-point
 *    remainder and measured three times slower.
 *
 * The last one is also the bound on the whole band: an *unsigned* 53-bit add can reach 2^54 and
 * silently round, so `kMaxNumberBits` is honest only because `wrapAdd` below is written to add in
 * halves when it has to. A signed 53-bit type has no such problem - its operands are bounded by
 * 2^52 - which is why Core's `Wide` is signed.
 */

namespace js {

// 2**32 and 2**-32, spelled once. The reciprocal is exact, so multiplying by it is the same
// operation as dividing by TWO32 and not an approximation of it.
static const F64 kTwo32 = 4294967296.0;
static const F64 kInvTwo32 = 1.0 / 4294967296.0;

/*
 * The name of one helper, interned per (operation, width, signedness).
 *
 * Keyed on all three because every constant in the body - the modulus, the sign threshold, the
 * high-half mask - is a literal in the emitted text. That is not only tidiness: V8's own truncation
 * fast path keys on seeing the width as a literal, and a helper taking its width as an argument
 * measured eight times slower on the equivalent `bigint` shape. So a program using two wide types
 * gets two sets of helpers rather than one parameterized set.
 */
static U32 helperKey(WideOp op, U32 bits, bool isSigned) {
    return (U32(op) << 8) | (bits << 1) | U32(isSigned);
}

static StringView opSuffix(WideOp op) {
    switch(op) {
        case WideOp::Wrap: return "wrap"_v;
        case WideOp::Add: return "add"_v;
        case WideOp::Sub: return "sub"_v;
        case WideOp::Mul: return "mul"_v;
        case WideOp::And: return "and"_v;
        case WideOp::Or: return "or"_v;
        case WideOp::Xor: return "xor"_v;
        case WideOp::Not: return "not"_v;
        case WideOp::Shl: return "shl"_v;
        case WideOp::Shr: return "shr"_v;
        case WideOp::Sar: return "sar"_v;
        case WideOp::High: return "hi"_v;
    }

    return "op"_v;
}

Name wideHelper(Gen& g, WideOp op, U32 bits, bool isSigned) {
    auto key = helperKey(op, bits, isSigned);
    if(auto found = g.wideHelpers.get(key)) return found.unwrap();

    // `$w53i$and`. The `$` prefix is the convention the compiler's own names already use, and
    // `uniqueName` guarantees the rest: a program that declared `$w53i$and` itself gets this one
    // disambiguated rather than shadowed.
    //
    // A width of zero is a helper that does not have one - the split below - and it leaves the
    // digits out rather than claiming a width it ignores: `$wi$hi`.
    char buffer[32];
    Size length = 0;
    buffer[length++] = '$';
    buffer[length++] = 'w';
    if(bits) length += show(U64(bits), buffer + length, sizeof(buffer) - length);
    buffer[length++] = isSigned ? 'i' : 'u';
    buffer[length++] = '$';

    auto suffix = opSuffix(op);
    copy(suffix.ptr, buffer + length, suffix.length);
    length += suffix.length;

    auto name = uniqueName(g, StringView { buffer, length }, false);
    g.wideHelpers.add(key, name);
    g.wideHelperOrder.push(WideHelper { name, op, U16(bits), isSigned });
    return name;
}

JsPtr<Expr> wideCall(Gen& g, WideOp op, IntType* type, JsPtr<Expr> a, JsPtr<Expr> b) {
    return wideCallAt(g, op, type->bits, type->isSigned, a, b);
}

JsPtr<Expr> wideCallAt(Gen& g, WideOp op, U32 bits, bool isSigned, JsPtr<Expr> a, JsPtr<Expr> b) {
    auto name = wideHelper(g, op, bits, isSigned);
    auto node = make<CallExpr>(g, variable(g, name));
    node->args.push(g.file.arena, a);
    if(b) node->args.push(g.file.arena, b);

    // Pure in exactly the sense `Math.imul` is: it reads nothing and writes nothing, and saying so
    // is what lets a one-use binding holding one collapse into its use like the arithmetic beside
    // it. It also marks the call for the fusion peephole, which needs to know these are arithmetic.
    node->pure = true;
    node->wide = op;
    node->wideBits = U16(bits);
    node->wideSigned = isSigned;
    return asExpr(g, node);
}

/*
 * The pieces every helper body is built out of.
 *
 * `lowHalf` and `highHalf` are the split; `join` is its inverse for an unsigned result, and
 * `resign` turns that into a signed one. Written as builders rather than as emitted text so that the
 * formatter still decides parenthesization and the peephole still sees ordinary nodes.
 */
static JsPtr<Expr> lowHalf(Gen& g, JsPtr<Expr> value) {
    return binary(g, BinaryOp::Shr, value, number(g, 0));
}

static JsPtr<Expr> splitHigh(Gen& g, JsPtr<Expr> value, bool isSigned) {
    auto scaled = binary(g, BinaryOp::Mul, value, number(g, kInvTwo32, false));

    // Truncation toward zero is the wrong rounding for a negative value - `-5 * 2**-32` truncates
    // to 0 where the high half is -1 - so a signed operand floors and an unsigned one, which can
    // never be negative, takes the cheaper `| 0`.
    if(isSigned) return hostCall(g, "Math"_v, "floor"_v, scaled);
    return binary(g, BinaryOp::Or, scaled, number(g, 0));
}

/*
 * The same thing as a call, which is what every site actually gets.
 *
 * `Math.floor(a * 2.3283064365386963E-10)` is thirty-eight characters and the single most repeated
 * expression this backend emits - twice in most helper bodies, once per leaf of every fused tree.
 * `$wi$hi(a)` is eight, and the helper is width-independent, so one definition serves every wide
 * type in the program rather than one per width.
 *
 * Interned at width zero for exactly that reason: this is the first half of every operation rather
 * than an operation of any particular type, and keying it on a width would emit identical bodies for
 * each one in use.
 *
 * It puts a call back inside the expression fusion exists to make call-free, which is the one thing
 * against it. Measured on the chained case at 6.7734 against 6.7627 ns/iter - inside the noise of
 * that harness, because the engine inlines a body this small - so what it costs is a dependency on
 * that inlining happening, and what it buys is about a quarter of the emitted text of any program
 * that uses these types.
 */
static JsPtr<Expr> highHalf(Gen& g, JsPtr<Expr> value, bool isSigned) {
    return wideCallAt(g, WideOp::High, 0, isSigned, value, nullptr);
}

// `h * 2**32 + (l >>> 0)`, the unsigned value the two halves denote. A low half the builder already
// knows to be zero is dropped rather than emitted as `+ (0 >>> 0)`, which is the shape a shift by
// 32 or more produces.
static JsPtr<Expr> join(Gen& g, JsPtr<Expr> high, JsPtr<Expr> low) {
    auto scaled = binary(g, BinaryOp::Mul, high, number(g, kTwo32));
    auto literal = g.base[low];
    if(literal->kind == Expr::Number && ((NumberExpr*)literal)->value == 0) return scaled;

    return binary(g, BinaryOp::Add, scaled, binary(g, BinaryOp::Shr, low, number(g, 0)));
}

// The high half masked to the bits this width actually owns. Also what makes a signed operand's
// floored - and therefore negative - high half read back as the right unsigned bits.
static JsPtr<Expr> maskHigh(Gen& g, JsPtr<Expr> high, U32 bits) {
    return binary(g, BinaryOp::And, high, number(g, powerOfTwo(bits - 32) - 1));
}

static JsPtr<Expr> variableNamed(Gen& g, StringView text, Name& into) {
    into = literalName(g, text);
    return variable(g, into);
}

/*
 * Return an unsigned result as the type's own value, binding it first when the sign has to be
 * re-applied.
 *
 * The binding is not a tidiness measure: `resignExpr` mentions its operand three times, so handing
 * it a joined expression rather than a name emits that whole expression three times. The first
 * version of this did, and `$w53i$and` came out as three copies of the split.
 */
static void returnWide(Gen& g, JsPtr<Expr> value, U32 bits, bool isSigned) {
    if(!isSigned) {
        emit(g, make<ReturnStmt>(g, value));
        return;
    }

    Name name;
    variableNamed(g, "v"_v, name);
    emit(g, make<ReturnStmt>(g, resignExpr(g, declare(g, name, value), bits)));
}

/*
 * The body of one helper.
 *
 * Every one is a `return` of a single expression, or a couple of `var`s and a return where a value
 * is needed twice. They are written here rather than as a string template because the tree is what
 * the formatter and the peephole read; a helper built out of text would be opaque to both.
 */
static StmtList wideHelperBody(Gen& g, WideOp op, U32 bits, bool isSigned, JsList<Name, false>& args) {
    // Ahead of everything else because it is the one body with no width: the constants below are all
    // computed from `bits`, and this one is interned at zero.
    if(op == WideOp::High) {
        Name aName;
        auto a = variableNamed(g, "a"_v, aName);
        args.push(g.file.arena, aName);

        return collect(g, [&] { emit(g, make<ReturnStmt>(g, splitHigh(g, a, isSigned))); });
    }

    auto modulus = powerOfTwo(bits);
    auto half = powerOfTwo(bits - 1);

    Name aName, bName;
    auto a = variableNamed(g, "a"_v, aName);
    auto b = variableNamed(g, "b"_v, bName);
    args.push(g.file.arena, aName);

    auto binaryOp = op == WideOp::And || op == WideOp::Or || op == WideOp::Xor;
    auto shift = op == WideOp::Shl || op == WideOp::Shr || op == WideOp::Sar;
    if(binaryOp || shift || op == WideOp::Add || op == WideOp::Sub || op == WideOp::Mul) {
        args.push(g.file.arena, bName);
    }

    return collect(g, [&] {
        /*
         * Reducing an out-of-range value, which is the general coercion every other case avoids.
         *
         * `%` rather than the comparison the add below uses, because this one has no bound on how
         * far out of range its argument is - it is what a cast from a wider type reaches. The
         * remainder JS gives takes the sign of the dividend, so an unsigned result needs one
         * conditional add to bring a negative one back up.
         */
        if(op == WideOp::Wrap) {
            Name vName;
            auto v = variableNamed(g, "v"_v, vName);
            declare(g, vName, binary(g, BinaryOp::Rem, a, number(g, modulus)));

            if(isSigned) {
                emit(g, make<ReturnStmt>(g, ternary(g,
                    binary(g, BinaryOp::Ge, v, number(g, half)),
                    binary(g, BinaryOp::Sub, v, number(g, modulus)),
                    ternary(g, binary(g, BinaryOp::Lt, v, unary(g, UnaryOp::Neg, number(g, half))),
                            binary(g, BinaryOp::Add, v, number(g, modulus)), v))));
            } else {
                emit(g, make<ReturnStmt>(g, ternary(g,
                    binary(g, BinaryOp::Lt, v, number(g, 0)),
                    binary(g, BinaryOp::Add, v, number(g, modulus)), v)));
            }

            return;
        }

        /*
         * Wrapping add and subtract.
         *
         * The sum of two in-range values is exact - below 2^53 for every width and signedness this
         * band allows, which is the fact `kMaxNumberBits` rests on - and it overshoots the range by
         * less than one modulus, so a comparison and one adjustment is the whole wrap. That is
         * three times faster than the remainder form and it is the reason arithmetic here is not
         * simply `wrap(a + b)`.
         *
         * The one case that is not exact is an *unsigned* 53-bit add, where the true sum can reach
         * 2^54. There the halves are added separately, which keeps every intermediate below 2^53.
         */
        if(op == WideOp::Add || op == WideOp::Sub) {
            auto isAdd = op == WideOp::Add;

            if(!isSigned && bits > 52 && isAdd) {
                // Carry-propagating add, for the one width where the direct sum would round.
                Name loName, hiName;
                auto lo = variableNamed(g, "l"_v, loName);
                auto hi = variableNamed(g, "h"_v, hiName);
                declare(g, loName, binary(g, BinaryOp::Add, lowHalf(g, a), lowHalf(g, b)));
                declare(g, hiName, binary(g, BinaryOp::Add,
                    binary(g, BinaryOp::Add, highHalf(g, a, false), highHalf(g, b, false)),
                    ternary(g, binary(g, BinaryOp::Ge, lo, number(g, kTwo32)), number(g, 1),
                            number(g, 0))));

                emit(g, make<ReturnStmt>(g, join(g, maskHigh(g, hi, bits),
                    ternary(g, binary(g, BinaryOp::Ge, lo, number(g, kTwo32)),
                            binary(g, BinaryOp::Sub, lo, number(g, kTwo32)), lo))));
                return;
            }

            Name vName;
            auto v = variableNamed(g, "v"_v, vName);
            declare(g, vName, binary(g, isAdd ? BinaryOp::Add : BinaryOp::Sub, a, b));

            if(isSigned) {
                emit(g, make<ReturnStmt>(g, ternary(g,
                    binary(g, BinaryOp::Ge, v, number(g, half)),
                    binary(g, BinaryOp::Sub, v, number(g, modulus)),
                    ternary(g, binary(g, BinaryOp::Lt, v, unary(g, UnaryOp::Neg, number(g, half))),
                            binary(g, BinaryOp::Add, v, number(g, modulus)), v))));
            } else {
                emit(g, make<ReturnStmt>(g, ternary(g,
                    binary(g, BinaryOp::Ge, v, number(g, modulus)),
                    binary(g, BinaryOp::Sub, v, number(g, modulus)),
                    ternary(g, binary(g, BinaryOp::Lt, v, number(g, 0)),
                            binary(g, BinaryOp::Add, v, number(g, modulus)), v))));
            }

            return;
        }

        /*
         * Wrapping multiply, in 16-bit limbs.
         *
         * The full product of two 32-bit halves passes 2^53, so it cannot be formed as one double
         * and then reduced - the low bits would already be gone. This is the same limb
         * decomposition the 64-bit helpers in benchmark/long-js use, masked to the declared width
         * instead of to 64. `Math.imul` is the only exact 32-bit multiply JS has, for the same
         * reason it is the only correct one in the 32-bit tower.
         */
        if(op == WideOp::Mul) {
            Name alName, ahName, blName, bhName;
            Name p00Name, p16aName, p16bName, midName;
            auto al = variableNamed(g, "al"_v, alName);
            auto ah = variableNamed(g, "ah"_v, ahName);
            auto bl = variableNamed(g, "bl"_v, blName);
            auto bh = variableNamed(g, "bh"_v, bhName);
            auto p00 = variableNamed(g, "p"_v, p00Name);
            auto p16a = variableNamed(g, "q"_v, p16aName);
            auto p16b = variableNamed(g, "r"_v, p16bName);
            auto mid = variableNamed(g, "m"_v, midName);

            declare(g, alName, lowHalf(g, a));
            declare(g, ahName, maskHigh(g, highHalf(g, a, isSigned), bits));
            declare(g, blName, lowHalf(g, b));
            declare(g, bhName, maskHigh(g, highHalf(g, b, isSigned), bits));

            auto a00 = binary(g, BinaryOp::And, al, number(g, 65535));
            auto a16 = binary(g, BinaryOp::Shr, al, number(g, 16));
            auto b00 = binary(g, BinaryOp::And, bl, number(g, 65535));
            auto b16 = binary(g, BinaryOp::Shr, bl, number(g, 16));

            declare(g, p00Name, binary(g, BinaryOp::Mul, a00, b00));
            declare(g, p16aName, binary(g, BinaryOp::Mul, a16, b00));
            declare(g, p16bName, binary(g, BinaryOp::Mul, a00, b16));
            declare(g, midName, binary(g, BinaryOp::Add,
                binary(g, BinaryOp::Add, binary(g, BinaryOp::Shr, p00, number(g, 16)),
                       binary(g, BinaryOp::And, p16a, number(g, 65535))),
                binary(g, BinaryOp::And, p16b, number(g, 65535))));

            auto low = binary(g, BinaryOp::Or,
                binary(g, BinaryOp::Shl, binary(g, BinaryOp::And, mid, number(g, 65535)),
                       number(g, 16)),
                binary(g, BinaryOp::And, p00, number(g, 65535)));

            /*
             * The high half: the two cross products that reach bit 32, the carries out of the three
             * limb products, and - the one that is easy to leave out - `a16 * b16`, which is the low
             * half's own top limbs meeting at bit 32. Dropping it produces a result that is right
             * whenever either operand fits in 16 bits and wrong the moment neither does.
             */
            auto high = binary(g, BinaryOp::Add,
                binary(g, BinaryOp::Add,
                    binary(g, BinaryOp::Add, hostCall(g, "Math"_v, "imul"_v, ah, bl),
                           hostCall(g, "Math"_v, "imul"_v, al, bh)),
                    binary(g, BinaryOp::Add, binary(g, BinaryOp::Mul, a16, b16),
                           binary(g, BinaryOp::Add, binary(g, BinaryOp::Shr, p16a, number(g, 16)),
                                  binary(g, BinaryOp::Shr, p16b, number(g, 16))))),
                binary(g, BinaryOp::Shr, mid, number(g, 16)));

            returnWide(g, join(g, maskHigh(g, high, bits), low), bits, isSigned);
            return;
        }

        /*
         * The bitwise operators, which are the reason this file exists.
         *
         * Each half is plain 32-bit work once the split has happened, and the halves never interact,
         * so this is the whole of `&`, `|` and `^`. `~` is the same shape with one operand. The
         * high half is masked because a signed operand's floored high half is negative and because
         * `~` sets bits above the declared width.
         */
        if(binaryOp || op == WideOp::Not) {
            BinaryOp kind = op == WideOp::And ? BinaryOp::And
                          : op == WideOp::Or ? BinaryOp::Or : BinaryOp::Xor;

            JsPtr<Expr> high, low;
            if(op == WideOp::Not) {
                high = unary(g, UnaryOp::BitNot, highHalf(g, a, isSigned));
                low = unary(g, UnaryOp::BitNot, a);
            } else {
                high = binary(g, kind, highHalf(g, a, isSigned), highHalf(g, b, isSigned));
                low = binary(g, kind, a, b);
            }

            returnWide(g, join(g, maskHigh(g, high, bits), low), bits, isSigned);
            return;
        }

        /*
         * Shifts, which are the one place the amount is not known here.
         *
         * A constant amount would let the whole split collapse - a right shift by a literal is one
         * divide and a floor - but a helper takes it as an argument, so this is the general form
         * with its branches intact. `opt.cpp` is where a literal amount gets the collapsed version.
         *
         * `>>>` on the halves rather than a division, because after the split both are 32-bit
         * values and the host operators are exact on them.
         */
        Name nName;
        auto n = variableNamed(g, "b"_v, nName);

        if(op == WideOp::Shl) {
            auto low = lowHalf(g, a);
            auto high = maskHigh(g, highHalf(g, a, isSigned), bits);

            // `n >= 32` moves the low half into the high one and clears the low; below that both
            // halves move and the low half's top `32 - n` bits carry up.
            auto shifted = ternary(g,
                binary(g, BinaryOp::Ge, n, number(g, 32)),
                join(g, maskHigh(g, binary(g, BinaryOp::Shl, low,
                                           binary(g, BinaryOp::Sub, n, number(g, 32))), bits),
                     number(g, 0)),
                join(g, maskHigh(g, binary(g, BinaryOp::Or,
                        binary(g, BinaryOp::Shl, high, n),
                        binary(g, BinaryOp::Shr, low, binary(g, BinaryOp::Sub, number(g, 32), n))),
                     bits),
                     binary(g, BinaryOp::Shl, low, n)));

            /*
             * A shift by zero is answered before the split, and has to be: the general arm reads
             * `low >>> (32 - n)`, and JS masks a shift count to five bits, so at `n == 0` that
             * becomes `low >>> 0` and would copy the whole low half up into the high one.
             *
             * The result is joined as an unsigned pattern, so a signed type re-applies the sign -
             * to the shifted arm only, since `a` is already a value of the type. The binding is
             * what keeps `resignExpr`'s three mentions of its operand from emitting the split three
             * times, and it is unconditional because the shift is pure: the value it computes at
             * `n == 0` is unused rather than unsafe.
             */
            if(!isSigned) {
                emit(g, make<ReturnStmt>(g, ternary(g,
                    binary(g, BinaryOp::Eq, n, number(g, 0)), a, shifted)));
                return;
            }

            Name sName;
            variableNamed(g, "s"_v, sName);
            auto s = declare(g, sName, shifted);

            emit(g, make<ReturnStmt>(g, ternary(g,
                binary(g, BinaryOp::Eq, n, number(g, 0)), a, resignExpr(g, s, bits))));
            return;
        }

        /*
         * Logical and arithmetic right shift.
         *
         * The join needs no re-signing in either case. A logical shift by one or more leaves the
         * top bit clear by construction, so the pattern is already the value; and the arithmetic
         * form's correction below produces the negative result directly.
         */
        auto low = lowHalf(g, a);
        auto high = maskHigh(g, highHalf(g, a, isSigned), bits);

        auto shifted = ternary(g,
            binary(g, BinaryOp::Ge, n, number(g, 32)),
            binary(g, BinaryOp::Shr, high, binary(g, BinaryOp::Sub, n, number(g, 32))),
            join(g, binary(g, BinaryOp::Shr, high, n),
                 binary(g, BinaryOp::Or, binary(g, BinaryOp::Shr, low, n),
                        binary(g, BinaryOp::Shl, high, binary(g, BinaryOp::Sub, number(g, 32), n)))));

        if(op == WideOp::Sar && isSigned) {
            /*
             * The sign bits an arithmetic shift should have brought in, which the logical form
             * above cannot produce: the operand was reduced to its width before the split, so there
             * are no sign bits left to shift. For a negative operand the logical result is the true
             * one plus `2**(bits-n)`, so subtracting that is the whole correction - one comparison
             * rather than a second shift path.
             *
             * Inside the zero guard rather than after it. A shift by zero must return the operand
             * unchanged, and the correction at `n == 0` would subtract a whole modulus from it.
             */
            Name sName;
            variableNamed(g, "s"_v, sName);

            // Bound unconditionally, which is safe because the shift is pure arithmetic: at
            // `n == 0` the general arm computes a value that is simply not used, and there is no
            // fault or side effect to avoid by guarding it.
            auto s = declare(g, sName, shifted);

            emit(g, make<ReturnStmt>(g, ternary(g,
                binary(g, BinaryOp::Eq, n, number(g, 0)), a,
                ternary(g, binary(g, BinaryOp::Ge, a, number(g, 0)), s,
                        binary(g, BinaryOp::Sub, s,
                               binary(g, BinaryOp::Div, number(g, modulus),
                                      hostCall(g, "Math"_v, "pow"_v, number(g, 2), n)))))));
            return;
        }

        emit(g, make<ReturnStmt>(g, ternary(g,
            binary(g, BinaryOp::Eq, n, number(g, 0)), a, shifted)));
    });
}

/*
 * `v >= 2**(bits-1) ? v - 2**bits : v` - the unsigned pattern read back as a signed value.
 *
 * Emitted as a ternary over a duplicated operand rather than a binding because every caller already
 * has the value in a variable or a helper argument, so the duplication is a name and not a
 * recomputation.
 */
JsPtr<Expr> resignExpr(Gen& g, JsPtr<Expr> value, U32 bits) {
    return ternary(g, binary(g, BinaryOp::Ge, value, number(g, powerOfTwo(bits - 1))),
                   binary(g, BinaryOp::Sub, value, number(g, powerOfTwo(bits))), value);
}

/*
 * The helpers, emitted once each after everything that calls them.
 *
 * After rather than before, because which ones are needed is only known once every function has
 * been generated - and a `function` declaration hoists, so a call textually above its definition is
 * the same program. Nothing else in this backend depends on statement order either.
 */
void emitWideHelpers(Gen& g) {
    if(g.wideHelperOrder.size() == 0) return;

    auto heading = make<CommentStmt>(g, internText(g,
        "integers of 33 to 53 bits - see codegen/js/wide.cpp"_v));
    g.wideHelperComment = asStmt(g, heading);
    emit(g, heading);

    // Indexed rather than ranged, because a helper body may request another helper and push onto
    // this array while it is being walked.
    for(Size i = 0; i < g.wideHelperOrder.size(); i++) {
        auto helper = g.wideHelperOrder[i];
        auto function = make<FunStmt>(g, helper.name);
        function->body = wideHelperBody(g, helper.op, helper.bits, helper.isSigned, function->args);
        emit(g, function);
    }
}

/*
 * A value widened into a 33-to-53-bit type from a narrower integer.
 *
 * Both sides are already the same host type, so this is only ever a question of range, and there
 * are exactly two cases where nothing has to happen:
 *
 *  - a *signed* destination holds every value of any narrower type, positive or negative, so the
 *    widening is the identity whatever the source's signedness;
 *  - an *unsigned* destination holds every value of a narrower *unsigned* type.
 *
 * What is left is a negative value widening into an unsigned type, where the two's complement
 * pattern the wider type would have held is a different number and `Wrap` is what produces it.
 */
JsPtr<Expr> wideFromNarrow(Gen& g, IntType* to, IntType* from, JsPtr<Expr> value) {
    if(from->bits >= to->bits) return wideCall(g, WideOp::Wrap, to, value, nullptr);
    if(to->isSigned || !from->isSigned) return value;

    return wideCall(g, WideOp::Wrap, to, value, nullptr);
}

/*
 * Fusing a whole bitwise expression, which is what the compact form above exists to be traded for.
 *
 * `a and (b or c)` as calls splits `b` and `c`, joins, splits the join again, and joins again -
 * three unpacks and two packs for a tree with two operators in it. Nothing about the middle join is
 * needed: the halves never interact under `&`, `|`, `^` or `~`, so the whole tree can be evaluated
 * on each half separately and joined once. That is measured at 2.2-2.3x on a chain in
 * benchmark/bits53-js, and it is the only rewrite there that closes the gap against `bigint`.
 *
 * It also subsumes the individual patterns rather than joining them as a list. Setting, clearing and
 * testing a bit are each one operator against a constant, and a constant's halves are known here -
 * so `flags and 255` reduces to `(flags & 255) >>> 0` by the ordinary identity `h & 0 = 0`, with no
 * rule in this file that mentions bits or masks. The findings document reached the same conclusion
 * from the other side: fusing the patterns individually is a fixed set of shapes, and fusing the
 * tree is every combination of them anyone writes.
 *
 * The two things it will not do are stated where they are decided: a leaf has to be readable twice
 * (`isSplittable`), and a tree of nothing but literals is declined (`fuseWideBitwise`).
 */

namespace {

/*
 * One 32-bit half of a value, held as a constant wherever it is one.
 *
 * The constants are not an optimization of the emitted text so much as what makes the identities
 * below reachable at all: a literal operand contributes a *known* half, and it is knowing that a
 * mask's high half is zero that removes the high half of the whole expression.
 */
struct Half {
    JsPtr<Expr> expr = nullptr;
    I32 value = 0;
    bool constant = true;

    /*
     * A bound on which bits this half can have set, or -1 for "no idea".
     *
     * Only ever narrowed by an `and` against a known operand, which is the case worth carrying: a
     * mask says the answer is inside the width already, and `packWide` is then spared the
     * reduction it would otherwise apply to every high half on principle. `x and 255` is the whole
     * of why - it should come out as one `&`, not as one `&` and a two-instruction no-op.
     */
    I32 known = -1;
};

struct Split {
    Half high;
    Half low;
};

Half constantHalf(I32 value) {
    Half half;
    half.value = value;
    half.known = value >= 0 ? value : -1;
    return half;
}

Half builtHalf(JsPtr<Expr> expr) {
    Half half;
    half.expr = expr;
    half.constant = false;
    return half;
}

JsPtr<Expr> halfExpr(Gen& g, Half half) {
    return half.constant ? number(g, F64(half.value)) : half.expr;
}

bool isFusible(WideOp op) {
    return op == WideOp::And || op == WideOp::Or || op == WideOp::Xor || op == WideOp::Not;
}

/*
 * One operator applied to one half, with the identities that make a constant operand disappear.
 *
 * Every operand reaching here is a name, a literal, or a split of one, so an operand an identity
 * drops was never going to do anything - which is also why the constant may be moved to the right
 * and each rule stated once rather than twice.
 */
Half combine(Gen& g, WideOp op, Half a, Half b) {
    if(a.constant && b.constant) {
        switch(op) {
            case WideOp::And: return constantHalf(a.value & b.value);
            case WideOp::Or: return constantHalf(a.value | b.value);
            default: return constantHalf(a.value ^ b.value);
        }
    }

    if(a.constant) {
        auto swapped = a;
        a = b;
        b = swapped;
    }

    if(b.constant) {
        switch(op) {
            case WideOp::And:
                if(b.value == 0) return constantHalf(0);
                if(b.value == -1) return a;
                break;
            case WideOp::Or:
                if(b.value == 0) return a;
                if(b.value == -1) return constantHalf(-1);
                break;
            default:
                if(b.value == 0) return a;
                if(b.value == -1) return builtHalf(unary(g, UnaryOp::BitNot, halfExpr(g, a)));
                break;
        }
    }

    auto host = op == WideOp::And ? BinaryOp::And : op == WideOp::Or ? BinaryOp::Or : BinaryOp::Xor;
    auto result = builtHalf(binary(g, host, halfExpr(g, a), halfExpr(g, b)));

    // What the operator does to the bound. `and` keeps whichever side is known, since a bit not in
    // one operand is not in the result; `or` and `xor` need both, since either side contributes.
    if(op == WideOp::And) {
        result.known = a.known >= 0 && b.known >= 0 ? a.known & b.known : max(a.known, b.known);
    } else if(a.known >= 0 && b.known >= 0) {
        result.known = a.known | b.known;
    }

    return result;
}

Half complement(Gen& g, Half a) {
    if(a.constant) return constantHalf(~a.value);
    return builtHalf(unary(g, UnaryOp::BitNot, a.expr));
}

/*
 * Whether a leaf may be taken apart.
 *
 * The split reads it twice - once for each half - so it has to be something that costs nothing to
 * read twice and cannot tell that the two reads happened in the other order. A name and a literal
 * are both; a property read is the first but not obviously the second, and a call is neither.
 *
 * A name that is assigned elsewhere is still fine, and deliberately so: a fused tree contains
 * nothing but reads, so there is nothing between the two of them that could write.
 */
bool isSplittable(Gen& g, JsPtr<Expr> pointer) {
    auto expr = g.base[pointer];
    if(expr->kind == Expr::Var) return true;
    return expr->kind == Expr::Number && ((NumberExpr*)expr)->integral;
}

Split splitLeaf(Gen& g, JsPtr<Expr> pointer, bool isSigned) {
    auto expr = g.base[pointer];

    if(expr->kind == Expr::Number) {
        auto bits = I64(((NumberExpr*)expr)->value);
        return Split { constantHalf(I32(bits >> 32)), constantHalf(I32(U32(U64(bits)))) };
    }

    // The low half needs no `>>> 0` of its own: `&`, `|` and `^` apply ToInt32 to their operands,
    // so the value *is* its low half wherever one of them consumes it. The one place that is not
    // true - a low half that reaches the join unchanged - is where `packWide` puts the `>>> 0`.
    return Split { builtHalf(highHalf(g, pointer, isSigned)), builtHalf(pointer) };
}

// The tree as a pair of half-expressions, or false where something in it is not one of these
// operators over splittable leaves. `names` counts the leaves that are not literals.
bool splitTree(Gen& g, JsPtr<Expr> pointer, U16 bits, bool isSigned, Split& into, U32& names) {
    auto expr = g.base[pointer];

    if(expr->kind == Expr::Call) {
        auto& node = *(CallExpr*)expr;
        if(node.wideBits == bits && node.wideSigned == isSigned && isFusible(node.wide) &&
           node.args.size() == (node.wide == WideOp::Not ? Size(1) : Size(2))) {
            Split left;
            if(!splitTree(g, node.args.get(g.base, 0), bits, isSigned, left, names)) return false;

            if(node.wide == WideOp::Not) {
                into = Split { complement(g, left.high), complement(g, left.low) };
                return true;
            }

            Split right;
            if(!splitTree(g, node.args.get(g.base, 1), bits, isSigned, right, names)) return false;

            into = Split { combine(g, node.wide, left.high, right.high),
                           combine(g, node.wide, left.low, right.low) };
            return true;
        }
    }

    if(!isSplittable(g, pointer)) return false;
    if(expr->kind == Expr::Var) names++;

    into = splitLeaf(g, pointer, isSigned);
    return true;
}

/*
 * The high half reduced to the bits this width owns above 32.
 *
 * An unsigned one masks, exactly as the helpers do. A signed one *sign-extends* instead, which is
 * where this beats the form it replaces: the helpers mask and then re-apply the sign with
 * `resignExpr`, and that mentions its operand three times and therefore needs a binding. Shifting
 * the top bit up to bit 31 and arithmetically back down is the same answer as one expression - the
 * high half is then already negative where the value is, so the join produces a signed result
 * directly. Same pair `decodeBits` narrows a signed field with, for the same reason.
 */
Half topHalf(Gen& g, Half high, U32 bits, bool isSigned) {
    // Already inside the width, and below its sign bit where there is one, so neither the mask nor
    // the sign extension would change anything. This is what a mask against a constant produces,
    // which is the shape most of these are.
    auto reach = I32(U32(1) << (bits - 32 - (isSigned ? 1 : 0)));
    if(high.known >= 0 && high.known < reach) return high;

    if(!isSigned) return combine(g, WideOp::And, high, constantHalf(I32((U32(1) << (bits - 32)) - 1)));

    auto spare = 64 - bits;
    if(high.constant) return constantHalf(I32(U32(high.value) << spare) >> spare);

    auto distance = number(g, F64(spare));
    return builtHalf(binary(g, BinaryOp::Sar,
                            binary(g, BinaryOp::Shl, high.expr, distance), distance));
}

JsPtr<Expr> packWide(Gen& g, Split split, U32 bits, bool isSigned) {
    auto top = topHalf(g, split.high, bits, isSigned);
    auto low = split.low.constant ? number(g, F64(U32(split.low.value)))
                                  : binary(g, BinaryOp::Shr, split.low.expr, number(g, 0));

    // An empty high half is the whole of what a mask against a constant below bit 32 comes out as,
    // and the answer is then the low half alone - non-negative and below 2^32 whatever the
    // signedness, since the sign lives in the half that turned out to be zero.
    if(top.constant && top.value == 0) return low;

    auto scaled = binary(g, BinaryOp::Mul, halfExpr(g, top), number(g, kTwo32));
    if(split.low.constant && split.low.value == 0) return scaled;

    return binary(g, BinaryOp::Add, scaled, low);
}

} // namespace

JsPtr<Expr> fuseWideBitwise(Gen& g, JsPtr<Expr> pointer) {
    auto expr = g.base[pointer];
    if(expr->kind != Expr::Call) return nullptr;

    auto& node = *(CallExpr*)expr;
    if(node.wideBits <= 32 || node.wideBits > kMaxNumberBits) return nullptr;
    if(!isFusible(node.wide)) return nullptr;

    Split split;
    U32 names = 0;
    if(!splitTree(g, pointer, node.wideBits, node.wideSigned, split, names)) return nullptr;

    /*
     * A tree of nothing but literals, which is declined rather than fused.
     *
     * Expanding it would trade one call for the same arithmetic written out over constants - no
     * fewer operations at run time and a good deal more text. What it wants instead is constant
     * folding, and that belongs where the arithmetic is already known at the right width for every
     * target rather than here. It also leaves the helpers themselves reachable from a program made
     * of literals, which is what WideInt.yana's cases are.
     */
    if(!names) return nullptr;

    return packWide(g, split, node.wideBits, node.wideSigned);
}

} // namespace js
