#pragma once

#include <Core.h>

/**
 * Compares source code to a string constant.
 * @param source The source code to compare. Must point to the first character of the string.
 * If the strings are equal, source is set to the first character after the part that is equal.
 * @param constant The constant string to compare to.
 */
bool compareConstString(const char*& source, const char* max, const char* constant);

// Parses the provided character as a hexit, to an integer in the range 0..15, or -1 if invalid.
int parseHexit(char c);

// Parses the provided character as an octit, to an integer in the range 0..7, or -1 if invalid.
int parseOctit(char c);

// Parses the provided character as a digit, to an integer in the range 0..9, or -1 if invalid.
int parseDigit(char c);

// Parses the provided character as a bit, to an integer in the range 0..1, or -1 if invalid.
int parseBit(char c);

/*
 * Parses a character literal from a text sequence with a certain base.
 * @param p A pointer to the first numeric character.
 * @param parseAtom Parses a single character to the corresponding numeric value.
 * This pointer is increased to the first character after the number.
 * @param numChars The maximum number of characters to parse.
 * @param max The maximum value supported. If the literal exceeds this value, a warning is generated.
 * @param diag The diagnostics to which problems will be written.
 * @return The code point generated from the sequence.
 */
template<U32 Base, class ParseAtom, class Diagnostics>
U32 parseIntSequence(const char*& p, const char* m, ParseAtom parseAtom, U32 numChars, U32 max, Diagnostics& diag) {
    U32 res = 0;
    for(auto i=0; i < numChars && p < m; i++) {
        char c = *p;
        auto num = parseAtom(c);
        if(num < 0) break;

        res *= Base;
        res += num;
        p++;
    }

    if(res > max) diag.warning("character literal out of range: %@"_v, nullptr, res);
    return res;
}

/*
 * Parses an integer literal with a custom base.
 * Supported bases are 2, 8, 10, 16.
 * @param p A pointer to the first numeric character.
 * @param parseAtom Parses a single character to the corresponding numeric value.
 * This pointer is increased to the first character after the number.
 * @return The parsed number.
 */
template<U32 Base, class ParseAtom>
U64 parseIntLiteral(const char*& p, const char* m, ParseAtom parseAtom) {
    U64 res = 0;
    auto num = parseAtom(*p);
    while(num >= 0 && p < m) {
        res *= Base;
        res += num;
        p++;
        num = parseAtom(*p);
    }
    return res;
}

/**
 * Parses a floating point literal.
 * The literal must have the following form:
 *    decimal -> digit{digit}
 *    exponent -> (e|E)[+|-] decimal
 *    float -> decimal . decimal[exponent] | decimal exponent
 * @param p A pointer to the first numeric character.
 * This pointer is increased to the first character after the number.
 * @return The parsed number.
 */
double parseFloatLiteral(const char*& p, const char* m);

struct NumericLiteral {
    union {
        U64 i;
        F64 f;
    };

    bool isInteger;
};

NumericLiteral parseNumericLiteral(const char*& p, const char* m);

/**
 * Returns true if this is an uppercase character.
 * TODO: Currently, only characters in the ASCII range are considered.
 */
bool isUpperCase(char c);

/**
 * Returns true if this is a lowercase character.
 * TODO: Currently, only characters in the ASCII range are considered.
 */
bool isLowerCase(char c);

bool isBit(char c);
bool isDigit(char c);
bool isOctit(char c);
bool isHexit(char c);

// Checks if the provided character is valid as part of an identifier (VarID or ConID).
bool isIdentifier(char c);

// Checks if the provided character is a symbol in the language - what an operator is made of.
// Here rather than in the lexer because the language server reads the same rule out of a document
// the parser never saw: `--` starts a comment only when what follows it is not a symbol.
bool isSymbol(char c);

// Checks if the provided character is ASCII whitespace.
bool isWhiteChar(char c);

// Returns the next UTF-32 code point from the provided UTF-8 string.
template<class Diag>
U32 nextCodePoint(const char*& p, const char* m, Diag& diag) {
    U32 c;
    auto up = (const Byte*)p;

    if(Tritium::Unicode::utf8PointToUtf32(up, (const Byte*)m, &c, &c + 1)) {
        p = (const char*)up;
        return c;
    } else {
        p = (const char*)up;
        diag.warning("Invalid UTF-8 sequence %@"_v, nullptr, (U32)c);
        return ' ';
    }
}

template<class Diag>
U32 parseEscapedLiteral(const char*& p, const char* m, Diag& diag) {
    char c = *p++;

    if(p >= m) {
        diag.warning("End of file inside escape sequence"_v, nullptr);
        return ' ';
    }

    switch(c) {
        case '{':
            // The left brace is used to start a formatting sequence.
            // Escaping it will print a normal brace.
            return '{';
        case 'a':
            return '\a';
        case 'b':
            return '\b';
        case 'f':
            return '\f';
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        case 'v':
            return '\v';
        case '\\':
            return '\\';
        case '\'':
            return '\'';
        case '\"':
            return '\"';
        case '0':
            return 0;
        case 'x':
            // Hexadecimal literal.
            if(!parseHexit(*p)) {
                diag.error("\\x used with no following hex digits"_v, nullptr);
                return ' ';
            }
            return parseIntSequence<16>(p, m, parseHexit, 8, 0xffffffff, diag);
        case 'o':
            // Octal literal.
            if(!parseOctit(*p)) {
                diag.error("\\o used with no following octal digits"_v, nullptr);
                return ' ';
            }
            return parseIntSequence<8>(p, m, parseOctit, 16, 0xffffffff, diag);
        default:
            if(isDigit(c)) {
                return parseIntSequence<10>(p, m, parseDigit, 10, 0xffffffff, diag);
            } else {
                diag.warning("Unknown escape sequence"_v, nullptr);
                return ' ';
            }
    }
}

template<class Diag>
U32 parseCharLiteral(const char*& p, const char* m, Diag& diag) {
    p++;
    U32 c;

    if(p < m && *p == '\\') {
        // This is an escape sequence.
        p++;
        c = parseEscapedLiteral(p, m, diag);
    } else {
        // This is a char literal.
        c = nextCodePoint(p, m, diag);
    }

    // Ignore any remaining characters in the literal.
    // It needs to end on this line.
    if(p >= m || *p++ != '\'') {
        diag.warning("Multi-character character constant"_v, nullptr);
        while(p < m && *p != '\'') {
            if(*p == '\n' || p >= m) {
                diag.warning("Missing terminating ' character in char literal"_v, nullptr);
                break;
            }
            p++;
        }
    }

    return c;
}
