#include "shared.h"

/*
 * Bit ranges inside a packed word: where a field of `width` bits at `offset` sits, how it is read
 * and written in place, the `$bitCell` accessor box a `&` of one crosses as, and the helpers a
 * range too wide for an inline expression calls.
 */

namespace js {

// The shift, for the 32-bit form. Null where the range starts at bit zero of what it names, and
// never asked of a range that arrived with a scale.
static JsPtr<Expr> shiftOf(JsShared& g, U32 offset) {
    return offset ? number(g, F64(offset)) : nullptr;
}

// The same position as a *multiplier*, for the wide form: `2**offset` folded into whatever scale a
// reference arrived with. Null means one, which is what lets a range at bit zero cost nothing.
static JsPtr<Expr> scaleOf(JsShared& g, U32 offset, JsPtr<Expr> scale) {
    if(!scale) return offset ? number(g, powerOfTwo(offset)) : nullptr;
    if(!offset) return scale;

    return binary(g, BinaryOp::Mul, scale, number(g, powerOfTwo(offset)));
}

/*
 * Where a range sits, as the two numbers the wide form multiplies by.
 *
 * `down` brings the bits at and above the range to bit zero and `up` puts a field back, and the two
 * are reciprocals - so a position could be one number and is two because of *which* number each end
 * has. A constant offset knows both and multiplies twice, which is what a `Math.floor` wants in
 * front of it. A reference carries the forward scale alone, and deriving its reciprocal would be a
 * divide per access either way, so that end divides instead. `divides` is that difference and it is
 * the only thing separating the two families of helper below.
 */
/*
 * `BitPosition` itself is declared in `shared.h`, with every function below that takes a range
 * apart, and they take the range as numbers and the state as `JsShared` - which is all a JSIR place
 * hands over. The spelling is here rather than under `resolve_ssa/js` because it is the target's:
 * the layout is `resolve_ssa/packing.h`'s, and one statement of how its words read in JavaScript is
 * one thing to keep in step with it.
 */
BitPosition bitPosition(JsShared& g, U32 offset, JsPtr<Expr> scale) {
    BitPosition position;

    if(scale) {
        position.up = scaleOf(g, offset, scale);
        position.divides = true;
        return position;
    }

    if(offset) {
        position.down = number(g, 1.0 / powerOfTwo(offset), false);
        position.up = number(g, powerOfTwo(offset));
    }

    return position;
}

// The bits at and above the range, as an integer. Exact for every value a `number` holds, since
// scaling by a power of two only moves the exponent.
static JsPtr<Expr> shiftedDown(JsShared& g, JsPtr<Expr> owner, BitPosition position) {
    if(position.divides) {
        return hostCall(g, "Math"_v, "floor"_v, binary(g, BinaryOp::Div, owner, position.up));
    }

    if(!position.down) return owner;
    return hostCall(g, "Math"_v, "floor"_v, binary(g, BinaryOp::Mul, owner, position.down));
}

// The low `width` bits of an already-shifted value, read as the field's own type. Shared by both
// forms, because once the range is at bit zero it is inside 32 bits either way.
static JsPtr<Expr> maskToField(JsShared& g, JsPtr<Expr> value, U32 width, bool isSigned) {
    if(width >= 32) return binary(g, BinaryOp::And, value, number(g, F64(~U32(0))));

    /*
     * A signed field widens by *sign*-extension, which masking cannot do: `& 15` on a `@bits(4) I32`
     * holding -4 answers 12, and the field's four bits are all the information there is to tell the
     * two apart. Shifting the field's top bit up to bit 31 and arithmetically back down truncates
     * and sign-extends in the same pair - the shape `decodePackedField` uses natively, and the
     * reason the two targets agreed on everything here except the sign.
     */
    if(isSigned) {
        auto distance = number(g, F64(32 - width));
        return binary(g, BinaryOp::Sar, binary(g, BinaryOp::Shl, value, distance), distance);
    }

    return binary(g, BinaryOp::And, value, number(g, F64((U32(1) << width) - 1)));
}

JsPtr<Expr> decodeRange(JsShared& g, JsPtr<Expr> owner, U32 offset, U32 width, BitPosition position,
                        bool wide, bool isSigned) {
    /*
     * A range entirely below bit 32 takes the 32-bit form whatever the word is, because `>>>` reads
     * its operand as `ToUint32` - the low half - and the field is inside that half by construction.
     * Only a *write* has to care about the bits it is dropping.
     */
    if(!wide) {
        auto value = owner;
        if(auto shift = shiftOf(g, offset)) value = binary(g, BinaryOp::Shr, value, shift);

        return maskToField(g, value, width, isSigned);
    }

    auto value = shiftedDown(g, owner, position);
    if(width < 32) return maskToField(g, value, width, isSigned);

    /*
     * A field of 32 bits or more, which no 32-bit operator can cut down: the remainder is the only
     * thing that reduces a `number` to a width the host has no mask for. `%` is a genuine
     * floating-point remainder and the slowest thing in this file, which is why it is reached only
     * by the one shape that has no alternative - a field wider than the operators, in a word wider
     * than the operators.
     */
    auto reduced = binary(g, BinaryOp::Rem, value, number(g, powerOfTwo(width)));
    if(!isSigned) return reduced;

    // Read back as a signed value. The binding is `resignExpr`'s requirement rather than a tidiness
    // measure: it mentions its operand three times.
    if(g.body) reduced = declare(g, generatedName(g, "bits"_v, g.labelCounter++), reduced);
    return resignExpr(g, reduced, width);
}

/*
 * Putting one back, which is a read-modify-write because the neighbours share the word.
 *
 * At 32 bits and below, `(owner & ~(mask << shift)) | ((value & mask) << shift)`. The inner mask is
 * not redundant: a `@bits(4)` value that arrived wider would otherwise write over the field above it,
 * and a signed one arrives sign-extended, so its high bits are ones exactly when they must not be
 * stored.
 *
 * Above 32 the same sentence becomes arithmetic: `owner + (wanted - held) * 2**offset`, where `held`
 * is the field's current contents. Additive rather than mask-and-or because there is no mask - the
 * hole is above bit 31 as often as not - and correct for the same reason the mask was: the difference
 * only ever moves the bits the field owns, and every intermediate stays under 2^53 because the word
 * does.
 *
 * Which form is chosen from is the *word* rather than the range, and that is where this differs from
 * the load: a field entirely below bit 32 can still be read with `>>>`, because reading only has to
 * find the bits it wants, while writing has to leave alone every bit it does not - and `owner & hole`
 * drops the whole high half of a word that has one.
 */
JsPtr<Expr> encodeRange(JsShared& g, JsPtr<Expr> owner, U32 offset, U32 width, U32 word,
                        BitPosition position, bool wide, JsPtr<Expr> value) {
    auto mask = width >= 32 ? ~U32(0) : (U32(1) << width) - 1;

    if(!wide) {
        value = binary(g, BinaryOp::And, value, number(g, F64(mask)));

        auto shift = shiftOf(g, offset);
        if(shift) value = binary(g, BinaryOp::Shl, value, shift);

        // The hole the field occupies, as a constant - the shift is one, and so is the mask.
        auto hole = shift ? unary(g, UnaryOp::BitNot,
                                  binary(g, BinaryOp::Shl, number(g, F64(mask)), shift))
                          : number(g, F64(U32(~mask)));

        auto written = binary(g, BinaryOp::Or, binary(g, BinaryOp::And, owner, hole), value);

        /*
         * A word that reaches bit 31 is read back unsigned, which is what makes "a packed word is a
         * non-negative pattern" true rather than usually true.
         *
         * It matters one level away. A write *through a reference* cannot know how wide the word it
         * was handed is, so it uses the arithmetic form below - and that form only stays inside the
         * word if what it is adding to is the pattern rather than the pattern minus 2^32. Narrower
         * words cannot be negative in the first place and are left alone.
         */
        if(word < 32) return written;
        return binary(g, BinaryOp::Shr, written, number(g, 0));
    }

    // Both readings are of the *pattern* rather than of the value, so a signed field is reduced the
    // unsigned way at both ends and the subtraction below cancels exactly.
    auto reduce = [&](JsPtr<Expr> from) -> JsPtr<Expr> {
        if(width < 32) return binary(g, BinaryOp::And, from, number(g, F64(mask)));
        if(width == 32) return binary(g, BinaryOp::Shr, from, number(g, 0));

        return wideCallAt(g, WideOp::Wrap, width, false, from, nullptr);
    };

    auto held = reduce(shiftedDown(g, owner, position));
    auto difference = binary(g, BinaryOp::Sub, reduce(value), held);

    if(position.up) difference = binary(g, BinaryOp::Mul, difference, position.up);
    return binary(g, BinaryOp::Add, owner, difference);
}

/*
 * The two above as functions, for the shapes that are worth calling rather than writing out.
 *
 * A range reached with the host's own operators is two or three of them against literals -
 * `p.$p4 >>> 20 & 4095` - and a call could only make that longer. What is worth naming is the
 * *arithmetic* form, which exists at all because a word wider than 32 bits has no mask: the read is
 * a multiply, a floor and a mask, and the write is that plus the arithmetic to put the field back
 * with the read appearing inside it a second time. `flip(&Bool)` was ninety characters of it, and a
 * program that borrows narrow values or holds records above 32 bits does the same thing everywhere.
 *
 * Two families, because the two ends hold the position differently and neither should take the
 * other's cost - see BitPosition. A **packed** helper is handed the reciprocal and the scale, both
 * literals at the site, and multiplies; a **borrowed** one is handed the reference's own scale and
 * divides. Both are interned per (width, signedness, direction), so every offset of a given width
 * shares one, which is what keeps this from becoming a helper per field.
 *
 * Interned the way wide.cpp's operators are and for the same second reason: the width stays a
 * literal inside the body, where the engine's own fast paths want to see it. What deliberately does
 * *not* move inside is the `owner[key]` indexing of a reference - one element access in one shared
 * helper would be a single inline cache for every borrowed word in the program.
 */
static U32 bitHelperKey(U32 bits, bool isSigned, bool store, bool packed) {
    return (bits << 3) | (U32(packed) << 2) | (U32(isSigned) << 1) | U32(store);
}

Name bitHelper(JsShared& g, U32 bits, bool isSigned, bool store, bool packed) {
    // A store reads and writes the *pattern* at both ends, so the sign is not one of its questions -
    // and keying on it anyway produced two helpers with identical bodies.
    if(store) isSigned = false;

    auto key = bitHelperKey(bits, isSigned, store, packed);
    if(auto found = g.bitHelpers.get(key)) return found.unwrap();

    // `$p20i$get` for a packed one and `$b20i$get` for a borrowed one, on wide.cpp's pattern: the
    // `$` prefix is the compiler's own convention and `uniqueName` handles a program that declared
    // the same identifier itself.
    char buffer[32];
    Size length = 0;
    buffer[length++] = '$';
    buffer[length++] = packed ? 'p' : 'b';
    length += show(U64(bits), buffer + length, sizeof(buffer) - length);
    buffer[length++] = isSigned ? 'i' : 'u';
    buffer[length++] = '$';

    auto suffix = store ? "set"_v : "get"_v;
    copy(suffix.ptr, buffer + length, suffix.length);
    length += suffix.length;

    auto name = uniqueName(g, StringView { buffer, length }, false);
    g.bitHelpers.add(key, name);
    g.bitHelperOrder.push(BitHelper { name, U16(bits), isSigned, store, packed });
    return name;
}

static JsPtr<Expr> namedParameter(JsShared& g, StringView text, Name& into) {
    into = literalName(g, text);
    return variable(g, into);
}

void emitBitHelpers(JsShared& g, U32 wordBits) {
    if(g.bitHelperOrder.size() == 0) return;

    auto heading = make<CommentStmt>(g, internText(g,
        "bit ranges of a word wider than the host's operators - see codegen/js/bits.cpp"_v));
    g.bitHelperComment = asStmt(g, heading);
    emit(g, heading);

    for(Size i = 0; i < g.bitHelperOrder.size(); i++) {
        auto helper = g.bitHelperOrder[i];
        auto function = make<FunStmt>(g, helper.name);

        function->body = collect(g, [&] {
            Name wordName, downName, upName, valueName;
            auto word = namedParameter(g, "w"_v, wordName);
            auto down = namedParameter(g, "i"_v, downName);
            auto up = namedParameter(g, "s"_v, upName);
            auto value = namedParameter(g, "v"_v, valueName);

            /*
             * The parameters are the position, which is the whole of what the site knows and the
             * body does not. A packed helper takes both directions because it multiplies in both;
             * a borrowed one takes the scale alone and divides with it, and a load through one
             * therefore needs no second parameter at all.
             */
            BitPosition position;
            function->args.push(g.file.arena, wordName);

            if(helper.packed) {
                position.down = down;
                function->args.push(g.file.arena, downName);

                if(helper.store) {
                    position.up = up;
                    function->args.push(g.file.arena, upName);
                }
            } else {
                position.up = up;
                position.divides = true;
                function->args.push(g.file.arena, upName);
            }

            if(helper.store) function->args.push(g.file.arena, valueName);

            // The word is as wide as this target packs, since the body serves every range of this
            // width - and it is the arithmetic form for all of them by construction.
            emit(g, make<ReturnStmt>(g, helper.store
                ? encodeRange(g, word, 0, helper.bits, wordBits, position, true, value)
                : decodeRange(g, word, 0, helper.bits, position, true, helper.isSigned)));
        });

        emit(g, function);
    }
}

/*
 * Pure in exactly the sense `Math.imul` and the wide helpers are: it reads nothing and writes
 * nothing, and the store's own assignment is at the call site rather than inside it. Saying so is
 * what lets a one-use binding holding one collapse into its use, and what lets a read whose result
 * nothing wanted go away - a reborrow computes the field's value only to describe where it is, and
 * without this the emitted body kept two loads it never looked at.
 */
static JsPtr<Expr> bitCall(JsShared& g, Name name, Buffer<const JsPtr<Expr>> args) {
    auto node = make<CallExpr>(g, variable(g, name));
    for(auto arg: args) node->args.push(g.file.arena, arg);

    node->pure = true;
    return asExpr(g, node);
}

/*
 * Reading and writing a **constant** range, named by numbers - what JSIR's `JsPlaceKind::BitRange`
 * becomes. The shapes are `decodeRange`/`encodeRange`'s with no scale: a word that is its one field
 * is the word, a range reached by the host's operators is written out, and an arithmetic range above
 * bit zero calls the interned packed helper.
 */
/*
 * A range that *is* the word it lives in, which is a single-field record: the one field fills the
 * scalar, so reading it is reading the number and writing it is writing the number.
 *
 * Asked only of a word too wide for the host's operators, where the alternative is a remainder and a
 * re-sign - three operations to recover a value nothing has touched. Below 32 the mask is one cheap
 * operation and stays, which also keeps the two ends of a narrower word agreeing about whether it
 * holds the value or the pattern.
 *
 * Both directions have to take this together or neither may: what makes the identity sound is that
 * the word holds whatever the store put there, so the store must not be masking what the load will
 * not.
 */
/*
 * Whether a range is worth calling for (`wide && offset != 0` below) rather than writing out.
 *
 * A reference always is: its position is a variable, so there is nothing to fold and the expression
 * is the same length at every site. A constant one is worth it only where it is actually *at* an
 * offset - at bit zero the position is the identity, the arithmetic form is already three or four
 * operations, and a helper would add a multiply by one and a floor of it to every access of the low
 * field of every record.
 */
JsPtr<Expr> decodeBitRange(JsShared& g, JsPtr<Expr> owner, U32 offset, U32 width, U32 word,
                           bool isSigned) {
    if(word > 32 && offset == 0 && width >= word) return owner;

    auto wide = word > 32 && offset + width > 32;
    auto position = bitPosition(g, offset, nullptr);
    if(!(wide && offset != 0)) return decodeRange(g, owner, offset, width, position, wide, isSigned);

    auto name = bitHelper(g, width, isSigned, false, true);
    JsPtr<Expr> args[] = { owner, position.down };
    return bitCall(g, name, toBuffer(args));
}

JsPtr<Expr> encodeBitRange(JsShared& g, JsPtr<Expr> owner, U32 offset, U32 width, U32 word,
                           bool isSigned, JsPtr<Expr> value) {
    if(word > 32 && offset == 0 && width >= word) return value;

    auto wide = word > 32;
    auto position = bitPosition(g, offset, nullptr);
    if(!(wide && offset != 0)) {
        return encodeRange(g, owner, offset, width, word, position, wide, value);
    }

    auto name = bitHelper(g, width, isSigned, true, true);
    JsPtr<Expr> args[] = { owner, position.down, position.up, value };
    return bitCall(g, name, toBuffer(args));
}

JsPtr<Expr> decodeScaledRange(JsShared& g, JsPtr<Expr> owner, JsPtr<Expr> scale, U32 offset,
                              U32 width, bool isSigned, bool whole) {
    auto name = bitHelper(g, width, isSigned, false, false);
    JsPtr<Expr> args[] = { owner, scaleOf(g, offset, scale) };
    auto decoded = bitCall(g, name, toBuffer(args));

    if(!whole || !isSigned) return decoded;
    return ternary(g, scale, decoded, owner);
}

JsPtr<Expr> encodeScaledRange(JsShared& g, JsPtr<Expr> owner, JsPtr<Expr> scale, U32 offset,
                              U32 width, bool isSigned, bool whole, JsPtr<Expr> value) {
    auto name = bitHelper(g, width, isSigned, true, false);
    JsPtr<Expr> args[] = { owner, scaleOf(g, offset, scale), value };
    auto encoded = bitCall(g, name, toBuffer(args));

    if(!whole || !isSigned) return encoded;
    return ternary(g, scale, encoded, value);
}

/*
 * A range reference that has to be **one value** - an erased `&Bool`, a returned or stored one, a
 * capture, a phi - as the `{$o, $k}` pair every reader of a reference already knows (J84).
 *
 * ```js
 * var $bitCellProto = Object.defineProperty({}, "$v", { get: $bitCell$get, set: $bitCell$set });
 * function $bitCell(o, k, s, m, i) {
 *     if (s === 0) return { $o: o, $k: k };
 *     var c = Object.create($bitCellProto); c.o = o; c.k = k; c.s = s; c.m = m; c.i = i;
 *     return { $o: c, $k: "$v" };
 * }
 * ```
 *
 * The alternative is a `{$o, $k, $s}` triple - the form of the generator deleted in J173 - and
 * every reader of every reference then reads a scale, including an erased body's, whose `&t` may be
 * anything. Here the scale stays inside the box: `$o[$k]` on it is a getter that decodes and an
 * assignment is a setter that read-modify-writes the word, so an erased body compiled once, a phi
 * and a stored reference all keep reading a `Cell` as a `Cell`, and only the rare escaping range
 * pays for an accessor. The two accessors are functions of their own over `this` rather than
 * closures, because this tree has no function expression, and they share one prototype so every box
 * is one hidden class. A zero scale is a signed content's plain storage (`JsPlace::bitWhole`), which
 * needs no box at all.
 */
JsPtr<Expr> bitCellCall(JsShared& g, JsPtr<Expr> owner, JsPtr<Expr> key, JsPtr<Expr> scale,
                        U32 width, bool isSigned) {
    if(!g.bitCellHelper.text) {
        g.bitCellHelper = uniqueName(g, "$bitCell"_v, false);
        g.bitCellProto = uniqueName(g, "$bitCellProto"_v, false);
    }

    return call(g, variable(g, g.bitCellHelper), owner, key, scale, number(g, powerOfTwo(width)),
                number(g, isSigned ? 1 : 0));
}

void emitBitCellHelper(JsShared& g) {
    if(!g.bitCellHelper.text) return;

    emit(g, make<CommentStmt>(g, internText(g,
        "a range reference as one value - an accessor box; see codegen/js/bits.cpp"_v)));

    auto self = [&] { return variable(g, literalName(g, "this"_v)); };
    auto member = [&](StringView text) { return field(g, self(), literalName(g, text)); };
    auto floorOf = [&](JsPtr<Expr> value) { return hostCall(g, "Math"_v, "floor"_v, value); };

    // `var v = Math.floor(this.o[this.k] / this.s) % this.m;
    //  return this.i && v >= this.m / 2 ? v - this.m : v;`
    auto getter = make<FunStmt>(g, uniqueName(g, "$bitCell$get"_v, false));
    auto getterName = getter->name;
    getter->body = collect(g, [&] {
        auto v = literalName(g, "v"_v);
        auto word = elementAt(g, member("o"_v), member("k"_v));
        emit(g, make<DeclStmt>(g, v, binary(g, BinaryOp::Rem,
                                            floorOf(binary(g, BinaryOp::Div, word, member("s"_v))),
                                            member("m"_v)), true));
        auto negative = binary(g, BinaryOp::LogicalAnd, member("i"_v),
                               binary(g, BinaryOp::Ge, variable(g, v),
                                      binary(g, BinaryOp::Div, member("m"_v), number(g, 2))));
        emit(g, make<ReturnStmt>(g, ternary(g, negative,
                                            binary(g, BinaryOp::Sub, variable(g, v), member("m"_v)),
                                            variable(g, v))));
    });
    emit(g, getter);

    // `var w = this.o[this.k], m = this.m;
    //  this.o[this.k] = w + ((x % m + m) % m - Math.floor(w / this.s) % m) * this.s;`
    auto setter = make<FunStmt>(g, uniqueName(g, "$bitCell$set"_v, false));
    auto setterName = setter->name;
    auto x = literalName(g, "x"_v);
    setter->args.push(g.file.arena, x);
    setter->body = collect(g, [&] {
        auto w = literalName(g, "w"_v);
        auto m = literalName(g, "m"_v);
        emit(g, make<DeclStmt>(g, w, elementAt(g, member("o"_v), member("k"_v)), true));
        emit(g, make<DeclStmt>(g, m, member("m"_v), true));

        auto wanted = binary(g, BinaryOp::Rem,
                             binary(g, BinaryOp::Add,
                                    binary(g, BinaryOp::Rem, variable(g, x), variable(g, m)),
                                    variable(g, m)),
                             variable(g, m));
        auto current = binary(g, BinaryOp::Rem,
                              floorOf(binary(g, BinaryOp::Div, variable(g, w), member("s"_v))),
                              variable(g, m));
        auto updated = binary(g, BinaryOp::Add, variable(g, w),
                              binary(g, BinaryOp::Mul, binary(g, BinaryOp::Sub, wanted, current),
                                     member("s"_v)));
        emitExpr(g, assign(g, elementAt(g, member("o"_v), member("k"_v)), updated));
    });
    emit(g, setter);

    // The one prototype - `Object.defineProperty({}, "$v", {get, set})`.
    auto accessors = make<ObjectExpr>(g);
    accessors->properties.push(g.file.arena, Property { literalName(g, "get"_v), variable(g, getterName) });
    accessors->properties.push(g.file.arena, Property { literalName(g, "set"_v), variable(g, setterName) });
    auto proto = call(g, field(g, variable(g, literalName(g, "Object"_v)), literalName(g, "defineProperty"_v)),
                      asExpr(g, make<ObjectExpr>(g)), asExpr(g, make<StringExpr>(g, internText(g, "$v"_v))),
                      asExpr(g, accessors));
    emit(g, make<DeclStmt>(g, g.bitCellProto, proto, false));

    auto function = make<FunStmt>(g, g.bitCellHelper);
    Name o = literalName(g, "o"_v), k = literalName(g, "k"_v), sc = literalName(g, "s"_v),
         mm = literalName(g, "m"_v), ii = literalName(g, "i"_v), c = literalName(g, "c"_v);
    for(auto arg: { o, k, sc, mm, ii }) function->args.push(g.file.arena, arg);

    function->body = collect(g, [&] {
        auto plain = make<ObjectExpr>(g);
        plain->properties.push(g.file.arena, Property { propertyName(g, "$o"_v), variable(g, o) });
        plain->properties.push(g.file.arena, Property { propertyName(g, "$k"_v), variable(g, k) });
        auto early = collect(g, [&] { emit(g, make<ReturnStmt>(g, asExpr(g, plain))); });
        emit(g, make<IfStmt>(g, binary(g, BinaryOp::Eq, variable(g, sc), number(g, 0)), early,
                             StmtList {}));

        emit(g, make<DeclStmt>(g, c, call(g, field(g, variable(g, literalName(g, "Object"_v)),
                                                   literalName(g, "create"_v)),
                                          variable(g, g.bitCellProto)), true));
        Name members[] = { o, k, sc, mm, ii };
        for(auto name: members) {
            emitExpr(g, assign(g, field(g, variable(g, c), name), variable(g, name)));
        }

        auto boxed = make<ObjectExpr>(g);
        boxed->properties.push(g.file.arena, Property { propertyName(g, "$o"_v), variable(g, c) });
        boxed->properties.push(g.file.arena,
                               Property { propertyName(g, "$k"_v), asExpr(g, make<StringExpr>(g, internText(g, "$v"_v))) });
        emit(g, make<ReturnStmt>(g, asExpr(g, boxed)));
    });

    emit(g, function);
}

/*
 * A word built from its fields - JSIR's `Pack`.
 *
 * Each field is reduced to its own bits first, which is `encodeRange`'s inner mask and for the same
 * reason: a signed field arrives sign-extended. Then the terms are combined by `|` and `<<` where the
 * whole word is inside the host's operators, and by `+` and a power of two where it is not - exact,
 * because the ranges are disjoint and every term is a non-negative integer below 2^53. A word of
 * exactly 32 bits is read back unsigned (`>>> 0`), as `encodeRange` leaves one; a narrower one
 * cannot reach bit 31 and a wider one is built by `+`, which never goes negative.
 */
JsPtr<Expr> packBitRanges(JsShared& g, Buffer<const JsPtr<Expr>> values, Buffer<const U32> offsets,
                          Buffer<const U32> widths, U32 word) {
    JsPtr<Expr> result = nullptr;

    for(Size i = 0; i < values.size(); i++) {
        auto width = widths[i];
        auto offset = offsets[i];

        JsPtr<Expr> term;
        if(width < 32) {
            term = binary(g, BinaryOp::And, values[i], number(g, F64((U32(1) << width) - 1)));
        } else if(width == 32) {
            term = binary(g, BinaryOp::Shr, values[i], number(g, 0));
        } else {
            term = wideCallAt(g, WideOp::Wrap, width, false, values[i], nullptr);
        }

        if(word <= 32) {
            if(offset) term = binary(g, BinaryOp::Shl, term, number(g, F64(offset)));
            result = result ? binary(g, BinaryOp::Or, result, term) : term;
        } else {
            if(offset) term = binary(g, BinaryOp::Mul, term, number(g, powerOfTwo(offset)));
            result = result ? binary(g, BinaryOp::Add, result, term) : term;
        }
    }

    if(!result) return number(g, 0);
    if(word == 32) result = binary(g, BinaryOp::Shr, result, number(g, 0));
    return result;
}

} // namespace js
