#include <cstdlib>
#include "lexer.h"
#include "../util/string.h"

/**
 * Compares source code to a string constant.
 * @param source The source code to compare. Must point to the first character of the string.
 * If the strings are equal, source is set to the first character after the part that is equal.
 * @param constant The constant string to compare to.
 */
bool compareConstString(const char*& source, const char* constant) {
    auto src = source;
    while(*constant == *source) {
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

// Parses the provided character as a hexit, to an integer in the range 0..15, or -1 if invalid.
int parseHexit(char c) {
    static const U8 table[] = {
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9,	/* 0..9 */
        255,255,255,255,255,255,255,			/* :..@ */
        10, 11, 12, 13, 14, 15,					/* A..F */
        255,255,255,255,255,255,255,			/* G..` */
        255,255,255,255,255,255,255,
        255,255,255,255,255,255,255,
        255,255,255,255,255,255,
        10, 11, 12, 13, 14, 15,					/* a..f */
    };

    // Anything lower than '0' will underflow, giving some large number above 54.
    U32 index = (U32)c - '0';

    if(index > 54) return -1;

    auto res = table[index];
    if(res > 15) return -1;

    return res;
}

// Parses the provided character as an octit, to an integer in the range 0..7, or -1 if invalid.
int parseOctit(char c) {
    // Anything lower than '0' will underflow, giving some large number above 7.
    U32 index = (U32)c - '0';
    if(index > 7) return -1;
    else return index;
}

// Parses the provided character as a digit, to an integer in the range 0..9, or -1 if invalid.
int parseDigit(char c) {
    U32 index = (U32)c - '0';
    if(index > 9) return -1;
    else return index;
}

// Parses the provided character as a bit, to an integer in the range 0..1, or -1 if invalid.
int parseBit(char c) {
    U32 index = (U32)c - '0';
    if(index > 1) return -1;
    else return index;
}

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
template<U32 Base, class ParseAtom>
U32 parseIntSequence(const char*& p, ParseAtom parseAtom, U32 numChars, U32 max, Diagnostics& diag) {
    U32 res = 0;
    for(auto i=0; i<numChars; i++) {
        char c = *p;
        auto num = parseAtom(c);
        if(num < 0) break;

        res *= Base;
        res += num;
        p++;
    }

    if(res > max) diag.warning("character literal out of range: %i", nullptr, nullptr, res);
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
U64 parseIntLiteral(const char*& p, ParseAtom parseAtom) {
    U64 res = 0;
    auto num = parseAtom(*p);
    while(num >= 0) {
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
double parseFloatLiteral(const char*& p) {
    auto start = p;
    char* end;
    auto v = strtod(p, &end);
    p += (end - start);
    return v;
}

/**
 * Returns true if this is an uppercase character.
 * TODO: Currently, only characters in the ASCII range are considered.
 */
static bool isUpperCase(char c) {
    U32 index = (U32)c - 'A';
    return index <= ('Z' - 'A');
}

/**
 * Returns true if this is a lowercase character.
 * TODO: Currently, only characters in the ASCII range are considered.
 */
static bool isLowerCase(char c) {
    U32 index = (U32)c - 'a';
    return index <= ('z' - 'a');
}

static bool isBit(char c) {
    U32 index = (U32)c - '0';
    return index <= 1;
}

static bool isDigit(char c) {
    U32 index = (U32)c - '0';
    return index <= 9;
}

static bool isOctit(char c) {
    U32 index = (U32)c - '0';
    return index <= 7;
}

static bool isHexit(char c) {
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

// Checks if the provided character is valid as part of an identifier (VarID or ConID).
static bool isIdentifier(char c) {
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

// Checks if the provided character is a symbol in the language.
static bool isSymbol(char c) {
    // We use a small lookup table for this,
    // since the number of branches would be ridiculous otherwise.
    static const bool table[] = {
        true, /* ! */
        false, /* " */
        true, /* # */
        true, /* $ */
        true, /* % */
        true, /* & */
        false, /* ' */
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

// Checks if the provided character is a special character that cannot be used in identifiers.
static bool isSpecial(char c) {
    // We use a small lookup table for this,
    // since the number of branches would be ridiculous otherwise.
    static const bool table[] = {
        true, /* ( */
        true, /* ) */
        false, /* * */
        false, /* + */
        true, /* , */
        false, /* - */
        false, /* . */
        false, /* / */
        false, false, false, false, false, false, false, false, false, false, /* 0..9 */
        false, /* : */
        true, /* ; */
        false, /* < */
        false, /* = */
        false, /* > */
        false, /* ? */
        false, /* @ */
        false, false, false, false, false, false, false, false, false, false, /* A..Z */
        false, false, false, false, false, false, false, false, false, false,
        false, false, false, false, false, false,
        true, /* [ */
        false, /* \ */
        true, /* ] */
        false, /* ^ */
        false, /* _ */
        true, /* ` */
        false, false, false, false, false, false, false, false, false, false, /* a..z */
        false, false, false, false, false, false, false, false, false, false,
        false, false, false, false, false, false,
        true, /* { */
        false, /* | */
        true /* } */
    };

    U32 index = (U32)c - '(';
    if(index > 85) return false;
    else return table[index];
}

// Checks if the provided character is ASCII whitespace.
static bool isWhiteChar(char c) {
    // Spaces are handled separately.
    // All other white characters are in the same range.
    // Anything lower than TAB will underflow, giving some large number above 4.
    U32 index = (U32)c - 9;
    return index <= 4 || c == ' ';
}

//------------------------------------------------------------------------------

Lexer::Lexer(Context& context, Diagnostics& diag, const char* text, Token* tok) :
    token(tok), text(text), p(text), l(text), newItem(true), context(context), diag(diag) {
    // The first indentation level of a file should be 0.
    indentation = 0;
}

Token* Lexer::next() {
    parseToken();
    return token;
}

U32 Lexer::nextCodePoint() {
    U32 c;
    if(convertNextPoint(p, &c)) {
        return c;
    } else {
        diag.warning("Invalid UTF-8 sequence %i", nullptr, nullptr, c);
        return ' ';
    }
}

void Lexer::nextLine() {
    l = p + 1;
    line++;
    tabs = 0;
}

bool Lexer::handleWhitespace() {
    if(*p == '\n') {
        nextLine();
        return true;
    }

    if(*p == '\t') {
        tabs++;
        return true;
    }

    return isWhiteChar(*p);
}

void Lexer::skipWhitespace() {
    while(*p) {
        // Skip whitespace.
        if(!handleWhitespace()) {
            // Check for single-line comments.
            if(*p == '-' && p[1] == '-' && !isSymbol(p[2])) {
                // Skip the current line.
                p += 2;
                while(*p && *p != '\n') p++;

                // If this is a newline, we update the location.
                // If it is the file end, the caller will take care of it.
                if(*p == '\n') {
                    nextLine();
                    p++;
                }

                continue;
            }

            // Check for multi-line comments.
            else if(*p == '{' && p[1] == '-') {
                // The current nested comment depth.
                U32 level = 1;

                // Skip until the comment end.
                p += 2;
                while(*p) {
                    // Update the source location if needed.
                    if(*p == '\n') nextLine();

                    // Check for nested comments.
                    if(*p == '{' && p[1] == '-') level++;

                    // Check for comment end.
                    if(*p == '-' && p[1] == '}') {
                        level--;
                        if(level == 0) {
                            p += 2;
                            break;
                        }
                    }

                    p++;
                }

                // p now points to the first character after the comment, or the file end.
                // Check if the comments were nested correctly.
                if(level) {
                    diag.warning("Incorrectly nested comment: missing comment terminator(s).", nullptr, nullptr);
                }

                continue;
            }

            // No comment or whitespace - we are done.
            break;
        }

        // Check the next character.
        p++;
    }
}

Id Lexer::parseStringLiteral() {
    // There is no real limit on the length of a string literal, so we use a dynamic array while parsing.
    Array<char> chars(128);

    p++;
    while(1) {
        if(*p == '\\') {
            // This is an escape sequence or gap.
            p++;
            if(handleWhitespace()) {
                // This is a gap - we skip characters until the next '\'.
                // Update the current source line if needed.
                p++;
                while(handleWhitespace()) p++;

                if(*p != '\\') {
                    // The first character after a gap must be '\'.
                    diag.warning("Missing gap end in string literal", nullptr, nullptr);
                }

                // Continue parsing the string.
                p++;
            } else {
                char buffer[5];
                auto max = encodeCodePoint(parseEscapedLiteral(), buffer);
                for(char* c = buffer; c < max; c++) {
                    chars.push(*c);
                }
            }
        } else if(*p == kFormatStart) {
            // Start a string format sequence.
            formatting = 1;
            p++;
            break;
        } else {
            if(*p == '\"') {
                // Terminate the string.
                p++;
                break;
            } else if(!*p || *p == '\n') {
                // If the line ends without terminating the string, we issue a warning.
                diag.warning("Missing terminating quote in string literal", nullptr, nullptr);
                break;
            } else {
                chars.push(*p);
                p++;
            }
        }
    }

    // Create a new buffer for this string.
    auto buffer = (char*)context.stringArena.alloc(chars.size());
    memcpy(buffer, chars.pointer(), chars.size());
    return context.addUnqualifiedName(buffer, chars.size());
}

U32 Lexer::parseCharLiteral() {
    p++;
    U32 c;

    if(*p == '\\') {
        // This is an escape sequence.
        p++;
        c = parseEscapedLiteral();
    } else {
        // This is a char literal.
        c = nextCodePoint();
    }

    // Ignore any remaining characters in the literal.
    // It needs to end on this line.
    if(*p++ != '\'') {
        diag.warning("Multi-character character constant", nullptr, nullptr);
        while(*p != '\'') {
            if(*p == '\n' || *p == 0) {
                diag.warning("Missing terminating ' character in char literal", nullptr, nullptr);
                break;
            }
            p++;
        }
    }
    return c;
}

U32 Lexer::parseEscapedLiteral() {
    char c = *p++;
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
                diag.error("\\x used with no following hex digits", nullptr, nullptr);
                return ' ';
            }
            return parseIntSequence<16>(p, parseHexit, 8, 0xffffffff, diag);
        case 'o':
            // Octal literal.
            if(!parseOctit(*p)) {
                diag.error("\\o used with no following octal digits", nullptr, nullptr);
                return ' ';
            }
            return parseIntSequence<8>(p, parseOctit, 16, 0xffffffff, diag);
        default:
            if(isDigit(c)) {
                return parseIntSequence<10>(p, parseDigit, 10, 0xffffffff, diag);
            } else {
                diag.warning("Unknown escape sequence", nullptr, nullptr);
                return ' ';
            }
    }
}

void Lexer::parseNumericLiteral() {
    token->type = Token::Integer;

    // Parse the type of this literal.
    if(p[1] == 'b' || p[1] == 'B') {
        if(isBit(p[2])) {
            // This is a binary literal.
            p += 2;
            token->data.integer = parseIntLiteral<2>(p, parseBit);
        } else {
            token->data.integer = parseIntLiteral<10>(p, parseDigit);
        }
    } else if(p[1] == 'o' || p[1] == 'O') {
        if(isOctit(p[2])) {
            // This is an octal literal.
            p += 2;
            token->data.integer = parseIntLiteral<8>(p, parseOctit);
        } else {
            token->data.integer = parseIntLiteral<10>(p, parseDigit);
        }
    } else if(p[1] == 'x' || p[1] == 'X') {
        if(isHexit(p[2])) {
            // This is a hexadecimal literal.
            p += 2;
            token->data.integer = parseIntLiteral<16>(p, parseHexit);
        } else {
            token->data.integer = parseIntLiteral<10>(p, parseDigit);
        }
    } else {
        // Check for a dot or exponent to determine if this is a float.
        auto d = p + 1;
        while(1) {
            if(*d == '.') {
                // The first char after the dot must be numeric, as well.
                if(isDigit(d[1])) break;
            } else if(*d == 'e' || *d == 'E') {
                // This is an exponent. If it is valid, the next char needs to be a numeric,
                // with an optional sign in-between.
                if(d[1] == '+' || d[1] == '-') d++;
                if(isDigit(d[1])) break;
            } else if(!isDigit(*d)) {
                // This wasn't a valid float.
                token->data.integer = parseIntLiteral<10>(p, parseDigit);
                return;
            }

            d++;
        }

        // Parse a float literal.
        token->type = Token::Float;
        token->data.floating = parseFloatLiteral(p);
    }
}

void Lexer::parseSymbol(const char** start, U32* length) {
    bool sym1 = isSymbol(p[1]);
    bool sym2 = sym1 && isSymbol(p[2]);

    token->type = Token::VarSym;

    if(!sym1) {
        // Check for various reserved operators of length 1.
        if(*p == ':') {
            // Single colon.
            token->type = Token::opColon;
        } else if(*p == '.') {
            // Single dot.
            token->type = Token::opDot;
        } else if(*p == '=') {
            // This is the reserved Equals operator.
            token->type = Token::opEquals;
        } else if(*p == '\\') {
            // This is the reserved backslash operator.
            token->type = Token::opBackSlash;
        } else if(*p == '|') {
            // This is the reserved bar operator.
            token->type = Token::opBar;
        } else if(*p == '$') {
            // This is the reserved dollar operator.
            token->type = Token::opDollar;
        } else if(*p == '@') {
            // This is the reserved at operator.
            token->type = Token::opAt;
        } else if(*p == '~') {
            // This is the reserved tilde operator.
            token->type = Token::opTilde;
        }
    } else if(!sym2) {
        // Check for various reserved operators of length 2.
        if(*p == ':' && p[1] == ':') {
            // This is the reserved ColonColon operator.
            token->type = Token::opColonColon;
        } else if(*p == '=' && p[1] == '>') {
            // This is the reserved double-arrow operator.
            token->type = Token::opArrowD;
        } else if(*p == '.' && p[1] == '.') {
            // This is the reserved DotDot operator.
            token->type = Token::opDotDot;
        }  else if(*p == '<' && p[1] == '-') {
            // This is the reserved arrow-left operator.
            token->type = Token::opArrowL;
        } else if(*p == '-' && p[1] == '>') {
            // This is the reserved arrow-right operator.
            token->type = Token::opArrowR;
        }
    }

    if(token->type == Token::VarSym) {
        // Check if this is a constructor.
        if(*p == ':') {
            token->type = Token::ConSym;
        } else {
            token->type = Token::VarSym;
        }

        // Parse a symbol sequence.
        // Get the length of the sequence, we already know that the first one is a symbol.
        U32 count = 1;
        auto s = p;
        while(isSymbol(*(++p))) count++;

        // Check for a single minus operator - used for parser optimization.
        token->singleMinus = count == 1 && *s == '-';

        // Store the identifier data.
        *start = s;
        *length = count;
    } else {
        // Skip to the next token.
        if(sym1) p += 2;
        else p++;
    }
}

void Lexer::parseSpecial() {
    token->type = (Token::Type)*p++;
}

void Lexer::parseQualifier() {
    auto start = p;
    U32 length = 1;
    U32 segments = 1;
    token->type = Token::ConID;

    while(true) {
        while(isIdentifier(*(++p))) {
            length++;
        }

        if(*p == '.') {
            bool u = isUpperCase(p[1]);
            bool l = isLowerCase(p[1]) || p[1] == '_';
            bool s = isSymbol(p[1]);

            if(!(u || l || s)) break;

            length++;
            segments++;
            p++;

            // If the next character is upper case, we either have a ConID or another qualifier.
            if(u) continue;

            // If the next character is lowercase, we either have a VarID or keyword.
            if(l) {
                const char* subStart;
                U32 subLength;
                parseVariable(&subStart, &subLength);

                // If this was a keyword, we parse as a constructor and dot operator instead.
                if(token->type == Token::VarID) {
                    length += subLength;
                } else {
                    token->type = Token::ConID;
                    length--;
                    p = start + length;
                }

                break;
            }

            // If the next character is a symbol, we have a VarSym or ConSym.
            if(s) {
                const char* subStart;
                U32 subLength;
                parseSymbol(&subStart, &subLength);

                // If this was a builtin symbol, we parse as a constructor and dot operator instead.
                if(token->type == Token::VarSym) {
                    length += subLength;
                } else {
                    token->type = Token::ConID;
                    length--;
                    p = start + length;
                }

                break;
            }
        } else {
            break;
        }
    }

    // Create the identifier.
    auto id = context.addQualifiedName(start, length, segments);
    token->data.id = id;
}

void Lexer::parseVariable(const char** start, U32* length) {
    token->type = Token::VarID;

    // First, check if we have a reserved keyword.
    auto c = p + 1;
    switch(*p) {
        case '_':
            token->type = Token::kw_;
            break;
        case 'a':
            if(compareConstString(c, "lias")) token->type = Token::kwAlias;
            break;
        case 'c':
            if(compareConstString(c, "lass")) token->type = Token::kwClass;
            break;
        case 'd':
            if(compareConstString(c, "ata")) token->type = Token::kwData;
            else if(compareConstString(c, "efault")) token->type = Token::kwDefault;
            else if(compareConstString(c, "eriving")) token->type = Token::kwDeriving;
            else if(*c == 'o') {c++; token->type = Token::kwDo;}
            break;
        case 'e':
            if(compareConstString(c, "lse")) token->type = Token::kwElse;
            break;
        case 'f':
            if(*c == 'n') {c++; token->type = Token::kwFn;}
            else if(compareConstString(c, "oreign")) token->type = Token::kwForeign;
            else if(*c == 'o' && c[1] == 'r') {c += 2; token->type = Token::kwFor;}
            break;
        case 'i':
            if(*c == 'f') {c++; token->type = Token::kwIf;}
            else if(compareConstString(c, "mport")) token->type = Token::kwImport;
            else if(*c == 'n' && !isIdentifier(c[1])) {c++; token->type = Token::kwIn;}
            else if(compareConstString(c, "nfix")) {
                if(*c == 'l') {c++; token->type = Token::kwInfixL;}
                else if(*c == 'r') {c++; token->type = Token::kwInfixR;}
                else token->type = Token::kwInfix;
            } else if(compareConstString(c, "nstance")) token->type = Token::kwInstance;
            break;
        case 'l':
            if(*c == 'e' && c[1] == 't') {c += 2; token->type = Token::kwLet;}
            break;
        case 'm':
            if(compareConstString(c, "atch")) token->type = Token::kwMatch;
            else if(compareConstString(c, "odule")) token->type = Token::kwModule;
            break;
        case 'n':
            if(compareConstString(c, "ewtype")) token->type = Token::kwNewType;
            break;
        case 'p':
            if(compareConstString(c, "refix")) token->type = Token::kwPrefix;
            break;
        case 'r':
            if(compareConstString(c, "eturn")) token->type = Token::kwReturn;
            break;
        case 't':
            if(compareConstString(c, "hen")) token->type = Token::kwThen;
            break;
        case 'w':
            if(compareConstString(c, "here")) token->type = Token::kwWhere;
            else if(compareConstString(c, "hile")) token->type = Token::kwWhile;
            break;
        default: ;
    }

    // We have to read the longest possible lexeme.
    // If a reserved keyword was found, we check if a longer lexeme is possible.
    if(token->type != Token::VarID) {
        if(isIdentifier(*c)) {
            token->type = Token::VarID;
        } else {
            p = c;
            return;
        }
    }

    // Read the identifier name.
    U32 count = 1;
    auto s = p;
    while(isIdentifier(*(++p))) count++;

    *start = s;
    *length = count;
}

void Lexer::parseToken() {
    auto b = p;

    parseT:
    // This needs to be reset manually.
    token->singleMinus = false;

    startWhitespace();

    // Check if we are inside a string literal.
    if(formatting == 3) {
        startLocation();
        formatting = 0;
        goto stringLit;
    } else {
        // Skip any whitespace and comments.
        skipWhitespace();
        startLocation();
    }

    // Check for the end of the file.
    if(!*p) {
        if(blockCount) token->type = Token::EndOfBlock;
        else token->type = Token::EndOfFile;
    }

    // Check if we need to insert a layout token.
    else if(token->startColumn == indentation && !newItem) {
        token->type = Token::EndOfStmt;
        newItem = true;
        goto newItem;
    }

    // Check if we need to end a layout block.
    else if(token->startColumn < indentation) {
        token->type = Token::EndOfBlock;
    }

    // Check for start of string formatting.
    else if(formatting == 1) {
        token->type = Token::StartOfFormat;
        formatting = 2;
    }

    // Check for end of string formatting.
    else if(formatting == 2 && *p == kFormatEnd) {
        // Issue a format end and make sure the next token is parsed as a string literal.
        // Don't skip the character - ParseStringLiteral skips one at the beginning.
        token->type = Token::EndOfFormat;
        formatting = 3;
    }

    // Check for integral literals.
    else if(isDigit(*p)) {
        parseNumericLiteral();
    }

    // Check for character literals.
    else if(*p == '\'') {
        token->data.character = parseCharLiteral();
        token->type = Token::Char;
    }

    // Check for string literals.
    else if(*p == '\"') {
        stringLit:
        // Since string literals can span multiple lines, this may update location.line.
        token->type = Token::String;
        token->data.id = parseStringLiteral();
    }

    // Check for special operators.
    else if(isSpecial(*p)) {
        parseSpecial();
    }

    // Parse symbols.
    else if(isSymbol(*p)) {
        const char* start;
        U32 length;
        parseSymbol(&start, &length);

        if(token->type == Token::VarSym) {
            auto name = (char*)context.stringArena.alloc(length);
            memcpy(name, start, length);
            token->data.id = context.addUnqualifiedName(name, length);
        }
    }

    // Parse ConIDs
    else if(isUpperCase(*p)) {
        parseQualifier();
    }

    // Parse variables and reserved ids.
    else if(isLowerCase(*p) || *p == '_') {
        const char* start;
        U32 length;
        parseVariable(&start, &length);

        if(token->type == Token::VarID) {
            auto name = (char*)context.stringArena.alloc(length);
            memcpy(name, start, length);
            token->data.id = context.addUnqualifiedName(name, length);
        }
    }

    // Unknown token - issue an error and skip it.
    else {
        diag.error("unknown token '%c'", nullptr, nullptr, *p);
        p++;
        goto parseT;
    }

    newItem = false;
    newItem:
    endLocation();
}

void Lexer::startLocation() {
    token->startLine = line;
    token->startColumn = (p - l) + tabs * (kTabWidth - 1);
    token->startOffset = p - text;
}

void Lexer::startWhitespace() {
    token->whitespaceLine = line;
    token->whitespaceColumn = (p - l) + tabs * (kTabWidth - 1);
    token->whitespaceOffset = p - text;
}

void Lexer::endLocation() {
    token->endLine = line;
    token->endColumn = (p - l) + tabs * (kTabWidth - 1);
    token->endOffset = p - text;
}
