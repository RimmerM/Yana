#pragma once

#include "convert.h"

#include <cstdio>

/*
 * Which string literals stand for one character at which types - Implementation-String.md part 1.1's
 * acceptance table, stated once for both resolve stages.
 *
 * `"," :: CodeUnit` is a literal both stages fold to a constant, and the question of *whether* a
 * literal may be one is the language's rather than a stage's: a program that one stage accepted and
 * the other refused would be the fixity divergence again. So the decoding, the table and the words
 * each refusal is reported in are here, and a stage supplies the target and the reporter.
 */
namespace intrinsic {

/*
 * The code points of a literal's text, as far as the table needs them: how many, and the first.
 *
 * The lexer writes an escape's code point in the *generalized* UTF-8 form, so `"\xD800"` arrives as
 * the three bytes a surrogate would have and decodes as one here, to be refused by name rather than
 * as malformed text. What cannot be decoded at all is a source file's own invalid bytes.
 */
struct LiteralChars {
    Size count = 0;
    U32 first = 0;
    bool malformed = false;
};

inline LiteralChars literalChars(const char* text, Size length) {
    LiteralChars chars;
    auto p = (const U8*)text;
    auto end = p + length;

    while(p < end) {
        auto lead = U32(*p);
        U32 extra = lead < 0x80 ? 0
                  : lead >= 0xC0 && lead < 0xE0 ? 1
                  : lead >= 0xE0 && lead < 0xF0 ? 2
                  : lead >= 0xF0 && lead < 0xF8 ? 3
                  : 4;

        if(extra == 4 || Size(end - p) <= extra) {
            chars.malformed = true;
            return chars;
        }

        auto value = extra == 0 ? lead : lead & (0x3Fu >> extra);
        for(U32 i = 1; i <= extra; i++) {
            if((p[i] & 0xC0) != 0x80) {
                chars.malformed = true;
                return chars;
            }

            value = (value << 6) | (p[i] & 0x3Fu);
        }

        if(chars.count == 0) chars.first = value;
        chars.count++;
        p += extra + 1;
    }

    return chars;
}

// Whether a literal is one code point - the strings a stage holds back for the position to decide.
inline bool isOneCharText(const char* text, Size length) {
    auto chars = literalChars(text, length);
    return !chars.malformed && chars.count == 1;
}

enum class CharRefusal: U8 {
    None,
    Malformed,
    Empty,
    Several,
    PastUnicode,
    Surrogate,
    NotAscii,
    NotBmp,
    DoesNotFit,
};

/*
 * The table. `integer` is the target where it is one, and absent for `Char`.
 *
 * **The rule is one code point that means the same thing as a unit of the encoding the width names.**
 * Eight bits, and `CodeUnit` at its narrowest, take ASCII alone: `"é" :: U8` would be `0xE9`, which is
 * Latin-1 and not the character, and `"é" :: CodeUnit` would be a different number on the two targets.
 * Sixteen bits take a BMP scalar value, which is its own UTF-16 unit, where the type can hold it.
 * Anything wider takes any scalar value, and `Size`'s guaranteed thirty-one positive bits hold every
 * one. No type takes a surrogate, because a surrogate is half of an encoding and not a character.
 */
inline CharRefusal checkLiteralChar(const LiteralChars& chars, const Numeric* integer) {
    if(chars.malformed) return CharRefusal::Malformed;
    if(chars.count == 0) return CharRefusal::Empty;
    if(chars.count > 1) return CharRefusal::Several;

    auto value = chars.first;
    if(value > 0x10FFFF) return CharRefusal::PastUnicode;
    if(value >= 0xD800 && value <= 0xDFFF) return CharRefusal::Surrogate;
    if(!integer) return CharRefusal::None;

    auto bits = integer->target == TargetWidth::CodeUnit ? 8u
                                                         : U32(minBitsOf(integer->target, integer->bits));

    if(bits <= 8 && value >= 0x80) return CharRefusal::NotAscii;
    if(bits <= 16 && value > 0xFFFF) return CharRefusal::NotBmp;

    auto positive = bits - (integer->isSigned ? 1u : 0u);
    if(positive < 32 && value >= (U32(1) << positive)) return CharRefusal::DoesNotFit;

    return CharRefusal::None;
}

// `U+00E9` - how a message names a code point, since the number alone reads as neither.
struct CodePointText {
    explicit CodePointText(U32 value) {
        auto length = snprintf(buffer, sizeof(buffer), "U+%04X", value);
        text = String(buffer, Size(length));
    }

    char buffer[16];
    String text;
};

/*
 * The refusal, in words - handed to `report` as a format and its arguments, so that each stage says
 * it through its own diagnostics at its own location. `type` is the target as the stage prints it.
 */
template<class Report>
void reportCharRefusal(CharRefusal refusal, const LiteralChars& chars, const String& type, Report&& report) {
    CodePointText named(chars.first);

    switch(refusal) {
        case CharRefusal::None:
            break;
        case CharRefusal::Malformed:
            report("this literal is not valid UTF-8, so it is no character a %@ can hold"_v, type);
            break;
        case CharRefusal::Empty:
            report("an empty string is not a character, and a literal of %@ is exactly one"_v, type);
            break;
        case CharRefusal::Several:
            report("this literal is %@ characters, and a literal of %@ is exactly one"_v, chars.count, type);
            break;
        case CharRefusal::PastUnicode:
            report("%@ is past the last Unicode plane, so it is not a character"_v, named.text);
            break;
        case CharRefusal::Surrogate:
            report("%@ is a surrogate - half of a UTF-16 pair and not a character - so it is not a %@"_v,
                   named.text, type);
            break;
        case CharRefusal::NotAscii:
            report("%@ is not ASCII, and a literal of %@ is one ASCII character - the value of any other is not a unit of UTF-8"_v,
                   named.text, type);
            break;
        case CharRefusal::NotBmp:
            report("%@ is outside the Basic Multilingual Plane, so it is two UTF-16 units and not one %@"_v,
                   named.text, type);
            break;
        case CharRefusal::DoesNotFit:
            report("%@ does not fit in %@"_v, named.text, type);
            break;
    }
}

} // namespace intrinsic
