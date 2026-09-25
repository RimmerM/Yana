#include "internal.h"

namespace ssa {

/*
 * The width seam - what happens to a value that is narrower than the register holding it.
 *
 * A `U8` manifests as one byte of storage and a *thirty-two bit register*, and those two numbers are
 * what this file exists between. `manifest.h` decides them: the storage is the declared width
 * rounded up to something a machine addresses, and the register is the lane the lower IR has. There
 * is no eight-bit register in that IR and there should not be - a machine adds in its whole
 * register whatever the program declared - so every operation on a narrow value happens at
 * thirty-two or sixty-four bits and something has to say what the extra bits hold.
 *
 * ## The invariant
 *
 * **A narrow value is held in normal form**: sign-extended where its type is signed, zero-extended
 * where it is not. Every producer leaves it that way and every consumer may assume it, and the whole
 * of this file is the first half of that sentence.
 *
 * It is worth being explicit about what rests on the second half, because none of those readers has
 * a mask of its own and none of them should:
 *
 *  - **A comparison.** `a < b` at a `U8` is a thirty-two bit comparison, and it answers correctly
 *    exactly because both registers hold the byte and nothing above it.
 *  - **A widening.** `widen(x) :: I32` from an `I8` is a `cast`, which extends whatever the register
 *    held; a register the arithmetic never narrowed makes the widening carry the dirt on.
 *  - **A store.** One byte is written and the rest of the register is discarded, so a value out of
 *    range is silently *repaired* by a round trip through storage and wrong until it makes one.
 *    That last is why the gap this file closes was invisible for so long: `(200 :: U8) + 100` read
 *    back out of a local is 44, and compared in a register is 300.
 *
 * ## Where it is not
 *
 * A `@bits(n)` refinement is **not** a second rule here, and that is deliberate: it is an integer
 * type whose declared width is narrower than its register, which is exactly what this file is
 * written over. `resolve_mir` decides *where* one is widened and narrowed - repr.md's "load always
 * widens, store always narrows" - and what arrives is a `convert` whose result type declares fewer
 * bits; putting that back in range is the same mask a `U8` addition gets. **Packing** is the one
 * thing with a rule of its own, and it is not here: a field sharing a word is a shift and a mask
 * against a *cell* rather than a value in a register, which is `decodePacked` in pack.cpp and
 * `propertyPlace` in erase.cpp - the same arithmetic with the shift from the field's own `FieldRepr`
 * at one end and out of a property slot at the other. What comes back from either is an ordinary
 * value in normal form, which is what keeps this file's rule the only one below them.
 *
 * Nothing here decides *which* operations need it either. That is `op.def`'s width column, because
 * "can the result leave the range" is arithmetic and true on every machine; what this file adds is
 * the target's half - whether the register is wider than the range in the first place.
 */

/*
 * The declared width of a value that does not fill its register, or nothing.
 *
 * One question rather than the two `compiler/resolve` asked (`narrowerThanRegister`, then the width
 * again at each use), because they are one question: a caller that knows a value is narrow always
 * needs to know by how much, and a second lookup is a second chance to read a different type than
 * the one that was tested.
 *
 * **Integers only.** A float fills its lane by construction - there is an `F32` register and an
 * `F64` one - and an enum-layout record is a *tag*, whose bits are the constructor numbers rather
 * than an arithmetic range: `Bool` owns one bit of its byte, and nothing adds two of them. The
 * operations a record reaches here are `not` and the comparisons, and `inst.cpp` answers the
 * first with an exclusive or against one, which is already the whole of the normal form.
 */
NarrowWidth Lowering::narrowWidthOf(DeclPtr<mir::Type> type) {
    auto& logical = *decl[type];
    if(logical.kind != mir::Type::Int) return NarrowWidth {};

    auto lowered = manifestNative(layout, mir::owned(type)).type;
    auto bits = intBitsOn(layout.target, (mir::IntType&)logical);
    if(bits >= laneBytes(lowered.lane) * 8) return NarrowWidth {};

    return NarrowWidth { bits, U32(laneBytes(lowered.lane) * 8), ((mir::IntType&)logical).isSigned,
                         lowered };
}

// The low `bits` of a value, as an immediate this block can read. Sixty-four is not reachable - a
// value that wide fills every register there is - but the shift is written to have an answer anyway,
// since `1 << 64` does not.
LowerPtr<LowerValue> Lowering::widthMask(LowerBlock& block, NarrowWidth width) {
    auto bits = width.bits >= 64 ? ~U64(0) : (U64(1) << width.bits) - 1;
    auto value = block.addInst(lower, new (to.arena) LowerImm(StringId(), width.type, bits));
    return ((LowerInstSingle*)value)->created().ptr - lower;
}

// A value with everything above its declared width cleared - one `and`, unconditionally. The
// primitive `zeroExtended` is written in terms of, and what a rotation needs of *both* its operands.
LowerPtr<LowerValue> Lowering::maskToWidth(LowerBlock& block, LowerPtr<LowerValue> value,
                                           NarrowWidth width) {
    auto mask = widthMask(block, width);
    auto result = binary<LowerInst::And>(lower, to, block, lower[value], lower[mask], width.type,
                                         StringId());

    return ((LowerInstSingle*)result)->created().ptr - lower;
}

/*
 * A narrow value read as the unsigned number its low bits are - the mask above, where one is needed.
 *
 * What it is *for* is the one operation that reads a narrow value's storage rather than its value:
 * a logical right shift. A signed narrow value is held sign-extended, so `shr` on it brings the
 * register's own copies of the sign bit down into the answer, and no mask afterwards can tell them
 * from bits the value had - `((0 :: WideInt) - 1) shr 1` was 2^63-1 rather than 2^52-1. So the
 * operand is put in range first, and the result is wrapped like every other.
 *
 * Only where the type is **signed**, which is the whole difference from `maskToWidth`: an unsigned
 * narrow value is already exactly this, and masking it again is an instruction to compute what the
 * register already holds.
 */
LowerPtr<LowerValue> Lowering::zeroExtended(LowerBlock& block, LowerPtr<LowerValue> value,
                                            NarrowWidth width) {
    if(!width.isSigned) return value;

    return maskToWidth(block, value, width);
}

/*
 * A value put back into its declared width - the normal form, and the one place it is established.
 *
 * Unsigned is one `and`: the value is the low bits and everything above them is zero by definition.
 *
 * Signed is the **shift pair**, and it is a pair rather than a mask because a mask answers the wrong
 * question. An `I8` holding -1 is `0xFF` in eight bits and has to be `0xFFFFFFFF` in the register,
 * so what is wanted is not "clear the bits above the width" but "make every bit above the width a
 * copy of the width's top bit". Shifting the value up until its sign bit is the register's and
 * arithmetically back down is exactly that, in two instructions and with no branch on the sign.
 *
 * Both builders fold, which is what keeps this from being a cost on the ordinary case: a value the
 * lower IR already knows is in range - a literal, most often - has its mask folded away before any
 * backend sees it.
 */
LowerPtr<LowerValue> Lowering::putInRange(LowerBlock& block, LowerPtr<LowerValue> value,
                                          NarrowWidth width, StringId name) {
    if(!width.isSigned) {
        auto mask = widthMask(block, width);
        auto result = binary<LowerInst::And>(lower, to, block, lower[value], lower[mask], width.type,
                                             name);
        return ((LowerInstSingle*)result)->created().ptr - lower;
    }

    auto distance = block.addInst(lower, new (to.arena) LowerImm(
        StringId(), width.type, U64(width.registerBits - width.bits)));
    auto shift = ((LowerInstSingle*)distance)->created().ptr - lower;

    auto up = binary<LowerInst::Shl>(lower, to, block, lower[value], lower[shift], width.type,
                                     StringId());
    auto down = binary<LowerInst::Sar>(lower, to, block,
                                       lower[((LowerInstSingle*)up)->created().ptr - lower],
                                       lower[shift], width.type, name);

    return ((LowerInstSingle*)down)->created().ptr - lower;
}

/*
 * A rotation at a width the machine has no rotate instruction for - `op.def`'s `Rotates`.
 *
 * The register is thirty-two or sixty-four bits wide, so `rol` on it turns within *that* whatever
 * the program declared: a `U8` rotated by one would move its top bit twenty-four places away from
 * where the type says it goes. So the definition is emitted instead of the instruction:
 *
 *     n = count mod w
 *     rol(v, n) = (v shl n) or (v shr (w - n))
 *
 * with three things the seam requires. The operand is masked **unconditionally**, not through
 * `zeroExtended`: both halves read its storage, so a sign-extended one would carry the register's
 * own copies of the sign bit round into the low end - and where the type is unsigned the mask is the
 * one the result would need anyway. `w - n` reaches `w` at a zero count, which is a shift the register has an
 * answer for precisely because the operand is `w` bits and `w` is below the register's width - it
 * shifts out to zero rather than being undefined. And the result is put back into the declared
 * width at the end like any other producer, since the `shl` half has left bits above it.
 *
 * The modulus is a mask where the width is a power of two, which is every declared width except
 * one: `WideInt` is fifty-three bits, so it divides. The count is masked to the width first, which
 * is what makes it the **unsigned** reading the modulus is defined over - a negative count is a bit
 * pattern here, exactly as it is to `rol cl`.
 */
/*
 * A byte reversal at a width the lower IR has no scalar for - sixteen bits, which is the only one.
 *
 * `lowerType` answers `Int32` for every integer below the sixty-four bit family, so a `Bswap` there
 * would say "reverse four bytes" whatever the program declared. The definition is emitted instead:
 * the two halves are shifted past each other and joined, which is one instruction fewer than the
 * `((v shl 8) and 0xFF00) or ((v shr 8) and 0xFF)` a library body would have written, because the
 * masks are what `putInRange` already owes at the end.
 *
 * The operand goes through `zeroExtended` and not `maskToWidth`, which is the difference from
 * `narrowRotate` above: only the `shr` half reads the operand's storage, so an unsigned value needs
 * nothing and a signed one needs its register's copies of the sign cleared before they are shifted
 * down into the answer. The `shl` half is wrapped away by the same `putInRange` that puts the
 * declared width's sign back.
 */
LowerPtr<LowerValue> Lowering::narrowByteSwap(LowerBlock& block, LowerPtr<LowerValue> value,
                                              NarrowWidth width, StringId name) {
    auto operand = zeroExtended(block, value, width);

    auto eightImm = block.addInst(lower, new (to.arena) LowerImm(StringId(), width.type, U64(8)));
    auto eight = ((LowerInstSingle*)eightImm)->created().ptr - lower;

    auto up = binary<LowerInst::Shl>(lower, to, block, lower[operand], lower[eight], width.type,
                                     StringId());
    auto down = binary<LowerInst::Shr>(lower, to, block, lower[operand], lower[eight], width.type,
                                       StringId());

    auto joined = binary<LowerInst::Or>(lower, to, block,
                                        lower[((LowerInstSingle*)up)->created().ptr - lower],
                                        lower[((LowerInstSingle*)down)->created().ptr - lower],
                                        width.type, StringId());

    return putInRange(block, ((LowerInstSingle*)joined)->created().ptr - lower, width, name);
}

LowerPtr<LowerValue> Lowering::narrowRotate(LowerBlock& block, LowerPtr<LowerValue> lhs,
                                            LowerPtr<LowerValue> rhs, NarrowWidth width, bool left,
                                            StringId name) {
    auto value = maskToWidth(block, lhs, width);

    auto widthImm = block.addInst(lower, new (to.arena) LowerImm(
        StringId(), width.type, U64(width.bits)));
    auto whole = ((LowerInstSingle*)widthImm)->created().ptr - lower;

    LowerPtr<LowerValue> count;
    if((width.bits & (width.bits - 1)) == 0) {
        auto maskImm = block.addInst(lower, new (to.arena) LowerImm(
            StringId(), width.type, U64(width.bits - 1)));
        auto reduced = binary<LowerInst::And>(
            lower, to, block, lower[rhs],
            lower[((LowerInstSingle*)maskImm)->created().ptr - lower], width.type, StringId());
        count = ((LowerInstSingle*)reduced)->created().ptr - lower;
    } else {
        auto unsignedCount = maskToWidth(block, rhs, width);
        auto reduced = binary<LowerInst::Rem>(lower, to, block, lower[unsignedCount], lower[whole],
                                              width.type, StringId());
        count = ((LowerInstSingle*)reduced)->created().ptr - lower;
    }

    auto backInst = binary<LowerInst::Sub>(lower, to, block, lower[whole], lower[count], width.type,
                                           StringId());
    auto back = ((LowerInstSingle*)backInst)->created().ptr - lower;

    auto up = left ? count : back;
    auto down = left ? back : count;

    auto high = binary<LowerInst::Shl>(lower, to, block, lower[value], lower[up], width.type,
                                       StringId());
    auto low = binary<LowerInst::Shr>(lower, to, block, lower[value], lower[down], width.type,
                                      StringId());

    auto joined = binary<LowerInst::Or>(lower, to, block,
                                        lower[((LowerInstSingle*)high)->created().ptr - lower],
                                        lower[((LowerInstSingle*)low)->created().ptr - lower],
                                        width.type, StringId());

    return putInRange(block, ((LowerInstSingle*)joined)->created().ptr - lower, width, name);
}

} // namespace ssa
