#include "machine.h"

/*
 * The EVEX spellings of the VEX instructions this backend writes - see `EvexSpelling` in machine.h.
 *
 * Written from the instruction set reference, one opcode at a time, and checked the way every other
 * encoding here is: `test/x64/EvexUpper.lower` holds a byte listing that reaches the upper sixteen
 * registers through each family, and running `objdump` over its `=>` lines is what says the table
 * reads as the instructions it claims.
 *
 * Three facts decide a row, and the first is the one that is easy to get wrong:
 *
 *  - **EVEX.W is not VEX.W.** Most of the packed set is VEX.WIG, and its EVEX spelling is W1 at a
 *    quadword or double lane (`vpaddq` is `EVEX.66.0F.W1 D4`) and W0 everywhere else; written with the
 *    other W it is a different instruction or none. `vpbroadcastq` is the sharpest case - VEX.W0 59 and
 *    EVEX.W1 59.
 *  - **The byte and word rows are AVX512BW**, the dword and qword ones F, and every one of them at 128
 *    or 256 bits is VL. The level has all of them (Analysis-Avx512.md §0.2), so a row here needs no
 *    feature of its own.
 *  - **A memory operand's displacement is scaled** under EVEX - disp8*N - by a factor the tuple type
 *    decides. A row that states no tuple still encodes correctly: its displacements are written in
 *    four bytes wherever they are not zero.
 *
 * Anything not listed has no spelling, which is the safe direction for every question asked of this:
 * the form keeps its webs in the lower sixteen, and the prefix writer refuses to promote it.
 */

static constexpr EvexSpelling none() { return EvexSpelling {}; }

static constexpr EvexSpelling spell(EvexWidth width, EvexTuple tuple) {
    return EvexSpelling { .exists = true, .width = width, .tuple = tuple };
}

// A row whose memory operand may also be one element repeated - see EvexSpelling::broadcast.
static constexpr EvexSpelling spellBroadcast(EvexWidth width) {
    return EvexSpelling { .exists = true, .width = width, .tuple = EvexTuple::Vector, .broadcast = true };
}

static constexpr auto W0 = EvexWidth::W0;
static constexpr auto W1 = EvexWidth::W1;
static constexpr auto WV = EvexWidth::FromVex;

static constexpr auto TNone = EvexTuple::None;
static constexpr auto TVec = EvexTuple::Vector;
static constexpr auto TE4 = EvexTuple::Element4;
static constexpr auto TE8 = EvexTuple::Element8;
static constexpr auto TW = EvexTuple::FromVex;
static constexpr auto T16 = EvexTuple::Half16;

/*
 * A packed float operation's four spellings: `ps` and `ss` are W0, `pd` and `sd` are W1, and the
 * scalar two read one element where the packed two read the whole vector.
 */
static EvexSpelling floatQuad(U8 mandatory, bool broadcast) {
    switch(mandatory) {
        case 0x00: return broadcast ? spellBroadcast(W0) : spell(W0, TVec);
        case 0x66: return broadcast ? spellBroadcast(W1) : spell(W1, TVec);
        case 0xf3: return spell(W0, TE4);
        case 0xf2: return spell(W1, TE8);
        default:   return none();
    }
}

// The two packed-only float rows - `andps`/`andpd`, the interleaves, `shufps`/`shufpd`.
static EvexSpelling floatPair(U8 mandatory, bool broadcast) {
    switch(mandatory) {
        case 0x00: return broadcast ? spellBroadcast(W0) : spell(W0, TVec);
        case 0x66: return broadcast ? spellBroadcast(W1) : spell(W1, TVec);
        default:   return none();
    }
}

static EvexSpelling map0F(U8 mandatory, U8 opcode, U8 extension) {
    auto is66 = mandatory == 0x66;

    switch(opcode) {
        // movups/movupd/movss/movsd, both directions.
        case 0x10: case 0x11:
            return floatQuad(mandatory, false);

        case 0x28: case 0x29:                       // movaps/movapd
            return floatPair(mandatory, false);

        case 0x14: case 0x15:                       // unpcklps/unpckhps and the pd pair
        case 0x54: case 0x55: case 0x56: case 0x57: // andps, andnps, orps, xorps (DQ)
        case 0xc6:                                  // shufps/shufpd
            return floatPair(mandatory, true);

        // cvtsi2ss/cvtsi2sd, and the two truncating conversions back. W is the general register's
        // width under both prefixes; the memory operand of the first is an integer of that width and
        // of the other two a float of the precision the prefix says.
        case 0x2a:
            if(mandatory == 0xf3 || mandatory == 0xf2) return spell(WV, TW);
            return none();
        case 0x2c: case 0x2d:
            if(mandatory == 0xf3) return spell(WV, TE4);
            if(mandatory == 0xf2) return spell(WV, TE8);
            return none();

        // ucomiss/ucomisd and comiss/comisd.
        case 0x2e: case 0x2f:
            if(mandatory == 0x00) return spell(W0, TE4);
            if(is66) return spell(W1, TE8);
            return none();

        case 0x51: // sqrt
        case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
            return floatQuad(mandatory, true);

        // cvtps2pd, cvtpd2ps, cvtss2sd, cvtsd2ss. The first reads half a vector, which is not a tuple
        // this table states - so its displacements go long.
        case 0x5a:
            switch(mandatory) {
                case 0x00: return spell(W0, TNone);
                case 0x66: return spell(W1, TVec);
                case 0xf3: return spell(W0, TE4);
                case 0xf2: return spell(W1, TE8);
            }
            return none();

        // cvtdq2ps, cvtps2dq, cvttps2dq - all dword lanes, all W0.
        case 0x5b:
            return mandatory == 0xf2 ? none() : spell(W0, TVec);

        // The interleaves and packs at an integer lane: byte, word and dword are W0, qword W1.
        case 0x60: case 0x61: case 0x63: case 0x67:
        case 0x68: case 0x69:
            return is66 ? spell(W0, TVec) : none();
        case 0x62: case 0x6a: case 0x6b:
            return is66 ? spellBroadcast(W0) : none();
        case 0x6c: case 0x6d:
            return is66 ? spellBroadcast(W1) : none();

        // movd/movq across the banks, whose W is the general register's.
        case 0x6e:
            return is66 ? spell(WV, TW) : none();
        case 0x7e:
            if(is66) return spell(WV, TW);
            if(mandatory == 0xf3) return spell(W1, TE8); // movq xmm, xmm/m64
            return none();

        // movdqa and movdqu, which EVEX spells `vmovdqa32`/`vmovdqu32` - the same bytes moved, the
        // lane width of the name being a statement about masking and nothing this backend writes.
        case 0x6f: case 0x7f:
            return is66 || mandatory == 0xf3 ? spell(W0, TVec) : none();

        // pshufd, pshufhw, pshuflw.
        case 0x70:
            return mandatory == 0x00 ? none() : spell(W0, TVec);

        // The shifts by an immediate, whose extension is what the instruction is: /2 right, /4
        // arithmetic, /6 left. Group 12 and 13 are word and dword lanes, W0; under EVEX /0 and /1
        // of group 13 are the rotations and /4 at W1 is `vpsraq`, none of which VEX has.
        case 0x71: case 0x72:
            if(!is66) return none();
            return extension == 2 || extension == 4 || extension == 6 ? spell(W0, TNone) : none();

        // Group 14 is the quadword lane at /2 and /6, and the byte shifts of a whole register at /3
        // and /7 - `psrldq` and `pslldq`, which are W0.
        case 0x73:
            if(!is66) return none();
            if(extension == 2 || extension == 6) return spell(W1, TNone);
            if(extension == 3 || extension == 7) return spell(W0, TNone);
            return none();

        // pinsrw and pextrw, one word through a general register.
        case 0xc4: case 0xc5:
            return is66 ? spell(W0, TNone) : none();

        // psrlw/psrld/psrlq, psraw/psrad, psllw/pslld/psllq by a count held in an xmm - which is one
        // 128-bit operand at every length.
        case 0xd1: case 0xd2: case 0xe1: case 0xe2: case 0xf1: case 0xf2:
            return is66 ? spell(W0, T16) : none();
        case 0xd3: case 0xf3:
            return is66 ? spell(W1, T16) : none();

        // paddq, psubq and pmuludq - the quadword rows.
        case 0xd4: case 0xfb: case 0xf4:
            return is66 ? spellBroadcast(W1) : none();

        // movq xmm/m64, xmm.
        case 0xd6:
            return is66 ? spell(W1, TE8) : none();

        // The rest of the integer arithmetic at byte, word and dword lanes, and the four bitwise
        // operations, which EVEX spells `vpandd`, `vpandnd`, `vpord` and `vpxord`.
        // The doubleword rows and the four bitwise ones broadcast a doubleword; the byte and word rows
        // have no broadcast form.
        case 0xd5: case 0xd8: case 0xd9: case 0xda: case 0xdc: case 0xdd: case 0xde:
        case 0xe0: case 0xe3: case 0xe4: case 0xe5: case 0xe8: case 0xe9: case 0xea:
        case 0xec: case 0xed: case 0xee:
        case 0xf5: case 0xf6: case 0xf8: case 0xf9: case 0xfc: case 0xfd:
            return is66 ? spell(W0, TVec) : none();
        case 0xdb: case 0xdf: case 0xeb: case 0xef: case 0xfa: case 0xfe:
            return is66 ? spellBroadcast(W0) : none();

        // cvtdq2pd reads half a vector; cvttpd2dq and cvtpd2dq read a whole one of doubles.
        case 0xe6:
            if(mandatory == 0xf3) return spell(W0, TNone);
            if(is66 || mandatory == 0xf2) return spell(W1, TVec);
            return none();

        default:
            return none();
    }
}

static EvexSpelling map0F38(U8 mandatory, U8 opcode) {
    if(mandatory != 0x66) return none();

    switch(opcode) {
        // pshufb, pmaddubsw, pmulhrsw, and the three absolute values - byte and word lanes are BW,
        // `pabsd` is F, all of them W0.
        case 0x00: case 0x04: case 0x0b: case 0x1c: case 0x1d: case 0x1e:
            return spell(W0, TVec);

        // vpermps and vpermd.
        case 0x16: case 0x36:
            return spellBroadcast(W0);

        // vbroadcastss and vbroadcastsd, reading one element.
        case 0x18: return spell(W0, TE4);
        case 0x19: return spell(W1, TE8);

        // pmovsx and pmovzx read part of a vector, which is not a tuple stated here.
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
            return spell(W0, TNone);

        // pmuldq.
        case 0x28: return spellBroadcast(W1);

        // packusdw.
        case 0x2b: return spellBroadcast(W0);

        // pminsb/pminsd/pminuw/pminud and the four maximums, and pmulld. W1 at 39, 3B, 3D, 3F and 40
        // is EVEX's own quadword rows (`vpminsq`, `vpmullq`), which VEX has none of.
        case 0x38: case 0x3a: case 0x3c: case 0x3e:
            return spell(W0, TVec);
        case 0x39: case 0x3b: case 0x3d: case 0x3f: case 0x40:
            return spellBroadcast(W0);

        // psrlv and psllv, whose W is the lane width under both prefixes; psravd, which VEX has at
        // W0 alone.
        case 0x45: case 0x47: return spell(WV, TVec);
        case 0x46: return spellBroadcast(W0);

        // vaesenc/vaesenclast/vaesdec/vaesdeclast - VEX.WIG and EVEX.WIG, VAES with VL at 128 and
        // 256 bits, both of which the level implies. No broadcast: a block is not an element.
        // `vaesimc` (DB) has no EVEX spelling and is not listed.
        case 0xdc: case 0xdd: case 0xde: case 0xdf:
            return spell(W0, TVec);

        // vpbroadcastd and vpbroadcastq - VEX.W0 both, EVEX.W0 and EVEX.W1 - and the byte and word
        // broadcasts, whose one-element tuples are narrower than any stated here.
        case 0x58: return spell(W0, TE4);
        case 0x59: return spell(W1, TE8);
        case 0x78: case 0x79: return spell(W0, TNone);

        default:
            break;
    }

    // The fused multiply-adds: 96-9F, A6-AF and B6-BF, whose W is the precision under both prefixes.
    // The packed rows end in an even nibble and read a vector; the scalar ones end in 9, B, D or F and
    // read one element.
    auto fma = (opcode >= 0x96 && opcode <= 0x9f) || (opcode >= 0xa6 && opcode <= 0xaf)
            || (opcode >= 0xb6 && opcode <= 0xbf);

    if(fma) return spell(WV, (opcode & 1) ? TW : TVec);
    return none();
}

static EvexSpelling map0F3A(U8 mandatory, U8 opcode) {
    if(mandatory != 0x66) return none();

    switch(opcode) {
        // vpermq and vpermpd - VEX.W1 and EVEX.W1.
        case 0x00: case 0x01: return spell(W1, TVec);

        // roundps/roundpd/roundss/roundsd, which EVEX spells `vrndscale*` - the same immediate with
        // the scale nibble zero, which is what every rounding this backend writes has.
        case 0x08: return spell(W0, TVec);
        case 0x09: return spell(W1, TVec);
        case 0x0a: return spell(W0, TE4);
        case 0x0b: return spell(W1, TE8);

        // palignr.
        case 0x0f: return spell(W0, TVec);

        // vpclmulqdq - WIG under both prefixes, VPCLMULQDQ with VL, which the level implies.
        // `vaeskeygenassist` (DF) has no EVEX spelling and is not listed.
        case 0x44: return spell(W0, TVec);

        // pextrb, pextrw, pinsrb - one narrow element through a general register.
        case 0x14: case 0x15: case 0x20: return spell(W0, TNone);

        // pextrd/pextrq and pinsrd/pinsrq, whose W is the element width under both prefixes.
        case 0x16: case 0x22: return spell(WV, TW);

        // extractps and insertps.
        case 0x17: case 0x21: return spell(W0, TE4);

        // vinsertf128/vextractf128 and vinserti128/vextracti128, which EVEX spells with `32x4` in the
        // name - one 128-bit half either way.
        case 0x18: case 0x19: case 0x38: case 0x39: return spell(W0, T16);

        default:
            return none();
    }
}

EvexSpelling evexSpellingOf(U8 mandatory, U8 map, U8 opcode, U8 extension) {
    switch(map) {
        case kOpcodeMap0F:   return map0F(mandatory, opcode, extension);
        case kOpcodeMap0F38: return map0F38(mandatory, opcode);
        case kOpcodeMap0F3A: return map0F3A(mandatory, opcode);
        default:             return none();
    }
}

U8 evexBroadcastBytes(const EncodingDescriptor& e) {
    if(e.prefixEncoding == PrefixEncoding::Evex) return e.evexBroadcast ? (e.evexW ? 8 : 4) : 0;
    if(e.prefixEncoding != PrefixEncoding::Vex) return 0;

    auto spelling = evexSpellingOf(e.prefix, e.opcodeMap, e.opcode, e.extension);
    if(!spelling.exists || !spelling.broadcast) return 0;
    return spelling.width == EvexWidth::W1 ? 8 : 4;
}
