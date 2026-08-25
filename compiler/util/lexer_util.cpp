#include "lexer_util.h"

bool compareConstString(const char*& source, const char* max, const char* constant) {
    auto src = source;
    while(source < max && *constant == *source) {
        constant++;
        source++;
    }

    if(*constant == 0) {
        return true;
    } else {
        source = src;
        return false;
    }
}

int parseHexit(char c) {
    static const U8 table[] = {
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9,	/* 0..9 */
        255,255,255,255,255,255,255,			/* :..@ */
        10, 11, 12, 13, 14, 15,					/* A..F */
        255,255,255,255,255,255,255,			/* G..` */
        255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,
        255,255,255,255,255,
        10, 11, 12, 13, 14, 15,					/* a..f */
    };

    // Anything lower than '0' will underflow, giving some large number above 54.
    U32 index = (U32)c - '0';

    if(index > 54) return -1;

    auto res = table[index];
    if(res > 15) return -1;

    return res;
}

int parseOctit(char c) {
    // Anything lower than '0' will underflow, giving some large number above 7.
    U32 index = (U32)c - '0';
    if(index > 7) return -1;
    else return index;
}

int parseDigit(char c) {
    U32 index = (U32)c - '0';
    if(index > 9) return -1;
    else return index;
}

int parseBit(char c) {
    U32 index = (U32)c - '0';
    if(index > 1) return -1;
    else return index;
}

double parseFloatLiteral(const char*& p, const char* m) {
    return Tritium::read<double>(p, m - p);
}

NumericLiteral parseNumericLiteral(const char*& p, const char* m) {
    NumericLiteral lit { .i = 0, .isInteger = true, .overflowed = false };

    // Parse the type of this literal.
    if(m - p >= 3 && (p[1] == 'b' || p[1] == 'B')) {
        if(isBit(p[2])) {
            // This is a binary literal.
            p += 2;
            lit.i = parseIntLiteral<2>(p, m, parseBit, lit.overflowed);
        } else {
            lit.i = parseIntLiteral<10>(p, m, parseDigit, lit.overflowed);
        }
    } else if(m - p >= 3 && (p[1] == 'o' || p[1] == 'O')) {
        if(isOctit(p[2])) {
            // This is an octal literal.
            p += 2;
            lit.i = parseIntLiteral<8>(p, m, parseOctit, lit.overflowed);
        } else {
            lit.i = parseIntLiteral<10>(p, m, parseDigit, lit.overflowed);
        }
    } else if(m - p >= 3 && (p[1] == 'x' || p[1] == 'X')) {
        if(isHexit(p[2])) {
            // This is a hexadecimal literal.
            p += 2;
            lit.i = parseIntLiteral<16>(p, m, parseHexit, lit.overflowed);
        } else {
            lit.i = parseIntLiteral<10>(p, m, parseDigit, lit.overflowed);
        }
    } else {
        // Check for a dot or exponent to determine if this is a float.
        auto d = p + 1;
        while(d < m) {
            if(*d == '.' && m - p >= 2 && isDigit(d[1])) {
                // The first char after the dot must be numeric, as well.
                break;
            } else if((*d == 'e' || *d == 'E') && m - p >= 2) {
                // This is an exponent. If it is valid, the next char needs to be a numeric,
                // with an optional sign in-between.
                if(d[1] == '+' || d[1] == '-') d++;
                if(isDigit(d[1])) {
                    break;
                } else {
                    // This wasn't a valid float.
                    lit.i = parseIntLiteral<10>(p, m, parseDigit, lit.overflowed);
                    return lit;
                }
            } else if(!isDigit(*d)) {
                // This wasn't a valid float.
                lit.i = parseIntLiteral<10>(p, m, parseDigit, lit.overflowed);
                return lit;
            }

            d++;
        }

        // Parse a float literal.
        lit.isInteger = false;
        lit.f = parseFloatLiteral(p, m);
    }

    return lit;
}

bool isUpperCase(char c) {
    U32 index = (U32)c - 'A';
    return index <= ('Z' - 'A');
}

bool isLowerCase(char c) {
    U32 index = (U32)c - 'a';
    return index <= ('z' - 'a');
}

bool isBit(char c) {
    U32 index = (U32)c - '0';
    return index <= 1;
}

bool isDigit(char c) {
    U32 index = (U32)c - '0';
    return index <= 9;
}

bool isOctit(char c) {
    U32 index = (U32)c - '0';
    return index <= 7;
}

bool isHexit(char c) {
    // We use a small lookup table for this,
    // since the number of branches would be ridiculous otherwise.
    static const bool table[] = {
        true, true, true, true, true, true, true, true, true, true,	/* 0..9 */
        false,false,false,false,false,false,false,					/* :..@ */
        true, true, true, true, true, true,							/* A..F */
        false,false,false,false,false,false,false,					/* G..` */
        false,false,false,false,false,false,false,
        false,false,false,false,false,false,
        false,false,false,false,false,false,
        true, true, true, true, true, true,							/* a..f */
    };

    // Anything lower than '0' will underflow, giving some large number above 54.
    U32 index = (U32)c - '0';

    if(index > 54) return false;
    else return table[index];
}

bool isIdentifier(char c) {
    static const bool table[] = {
        false, /* ' */
        false, /* ( */
        false, /* ) */
        false, /* * */
        false, /* + */
        false, /* , */
        false, /* - */
        false, /* . */
        false, /* / */
        true, true, true, true, true, true, true, true, true, true,	/* 0..9 */
        false,false,false,false,false,false,false,					/* :..@ */
        true, true, true, true, true, true, true, true, true, true, /* A..Z */
        true, true, true, true, true, true, true, true, true, true,
        true, true, true, true, true, true,
        false, /* [ */
        false, /* \ */
        false, /* ] */
        false, /* ^ */
        true, /* _ */
        false, /* ` */
        true, true, true, true, true, true, true, true, true, true, /* a..z */
        true, true, true, true, true, true, true, true, true, true,
        true, true, true, true, true, true
    };

    // Anything lower than ' will underflow, giving some large number above 83.
    U32 index = (U32)c - '\'';

    if(index > 83) return false;
    else return table[index];
}

bool isWhiteChar(char c) {
    // Spaces are handled separately.
    // All other white characters are in the same range.
    // Anything lower than TAB will underflow, giving some large number above 4.
    U32 index = (U32)c - 9;
    return index <= 4 || c == ' ';
}

// Checks if the provided character is a symbol in the language.
//
// `'` is one, which it would not be in a language with character literals - see
// Implementation-String.md §1.1 for why this one has none. Being a symbol is what lets the loan
// marker of Analysis-Borrows.md be an ordinary reserved operator in `tokens.def` rather than a
// branch of its own in each of the two lexers, and it is what will let `&'` lex as one run when
// named loan groups arrive.
bool isSymbol(char c) {
    // We use a small lookup table for this,
    // since the number of branches would be ridiculous otherwise.
    static const bool table[] = {
        true, /* ! */
        false, /* " */
        true, /* # */
        true, /* $ */
        true, /* % */
        true, /* & */
        true, /* ' */
        false, /* ( */
        false, /* ) */
        true, /* * */
        true, /* + */
        false, /* , */
        true, /* - */
        true, /* . */
        true, /* / */
        false, false, false, false, false, false, false, false, false, false, /* 0..9 */
        true, /* : */
        false, /* ; */
        true, /* < */
        true, /* = */
        true, /* > */
        true, /* ? */
        true, /* @ */
        false, false, false, false, false, false, false, false, false, false, /* A..Z */
        false, false, false, false, false, false, false, false, false, false,
        false, false, false, false, false, false,
        false, /* [ */
        true, /* \ */
        false, /* ] */
        true, /* ^ */
        false, /* _ */
        false, /* ` */
        false, false, false, false, false, false, false, false, false, false, /* a..z */
        false, false, false, false, false, false, false, false, false, false,
        false, false, false, false, false, false,
        false, /* { */
        true, /* | */
        false, /* } */
        true /* ~ */
    };

    U32 index = (U32)c - '!';
    if(index > 93) return false;
    else return table[index];
}
