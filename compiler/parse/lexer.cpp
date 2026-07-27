#include "lexer.h"
#include "../util/lexer_util.h"

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

//------------------------------------------------------------------------------

Lexer::Lexer(Context& context, Diagnostics& diag, const StringView& text, StringId moduleName) :
    moduleName(moduleName), text(text.ptr), p(text.ptr), l(text.ptr), m(text.ptr + text.length),
    newItem(true), context(context), diag(diag) {
    // The first indentation level of a file should be 0.
    indentation = 0;
}

Lexer::Position Lexer::position() const {
    return {
        .line = line,
        .column = U32((p - l) + tabs * (kTabWidth - 1)),
        .offset = U32(p - text),
    };
}

Location Lexer::locationFrom(const Position& start) const {
    auto end = position();

    return {
        .sourceModule = moduleName,
        .sourceStart = { .offset = start.offset, .line = U16(start.line), .column = U16(start.column) },
        .sourceEnd = { .offset = end.offset, .line = U16(end.line), .column = U16(end.column) },
    };
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
    while(p < m) {
        // Skip whitespace.
        if(!handleWhitespace()) {
            // Check for single-line comments.
            if(m - p > 3 && *p == '-' && p[1] == '-' && !isSymbol(p[2])) {
                // Skip the current line.
                p += 2;
                while(p < m && *p != '\n') p++;

                // If this is a newline, we update the location.
                // If it is the file end, the caller will take care of it.
                if(p < m && *p == '\n') {
                    nextLine();
                    p++;
                }

                continue;
            }

            // Check for multi-line comments.
            else if(m - p > 2 && *p == '{' && p[1] == '-' && p[2] != '>') {
                // The current nested comment depth.
                U32 level = 1;
                auto commentStart = position();

                // Skip until the comment end.
                p += 2;
                while(p < m) {
                    // Update the source location if needed.
                    if(*p == '\n') nextLine();

                    // Check for nested comments.
                    if(m - p > 2 && *p == '{' && p[1] == '-' && p[2] != '>') level++;

                    // Check for comment end.
                    if(m - p > 2 && *p == '-' && p[1] == '}') {
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
                    // The comment runs to the end of the file, so the range starts at the opener
                    // and the reader is shown where the unterminated comment began.
                    auto location = locationFrom(commentStart);
                    location.sourceEnd = { commentStart.offset + 2, U16(commentStart.line), U16(commentStart.column + 2) };
                    diag.warning("Incorrectly nested comment: missing comment terminator(s)."_v, &location);
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

StringId Lexer::parseStringLiteral() {
    // There is no real limit on the length of a string literal, so we use a dynamic array while parsing.
    Array<char> chars(128);

    auto terminated = false;
    auto stringStart = position();
    p++;

    while(p < m) {
        if(*p == '\\') {
            // This is an escape sequence or gap.
            auto gapStart = position();
            p++;
            if(p >= m) break;

            if(handleWhitespace()) {
                // This is a gap - we skip characters until the next '\'.
                // Update the current source line if needed.
                p++;
                while(p < m && handleWhitespace()) p++;

                if(p >= m || *p != '\\') {
                    // The first character after a gap must be '\'.
                    auto location = locationFrom(gapStart);
                    diag.warning("Missing gap end in string literal"_v, &location);
                }

                // Continue parsing the string.
                p++;
            } else {
                WChar32 codePoint = parseEscapedLiteral(p, m, diag);
                auto cp = (const U32*)&codePoint;

                char buffer[5];
                auto max = Unicode::utf32PointToUtf8(cp, cp + 1, buffer, buffer + 5);
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
                terminated = true;
                p++;
                break;
            } else if(*p == '\n') {
                break;
            } else {
                chars.push(*p);
                p++;
            }
        }
    }

    // A format sequence ends this chunk but not the literal - the text after the formatted
    // expression is lexed as a further chunk, and the terminating quote is that chunk's to find.
    if(!terminated && formatting != 1) {
        // If the line ends without terminating the string, we issue a warning. It points at the
        // whole unterminated literal, starting at the quote that opened it.
        auto location = locationFrom(stringStart);
        diag.warning("Missing terminating quote in string literal"_v, &location);
    }

    // Create a new buffer for this string.
    auto buffer = (char*)context.stringArena.alloc(chars.size());
    copyMem(chars.pointer(), buffer, chars.size());
    return context.addUnqualifiedName(buffer, chars.size());
}

void Lexer::parseSymbol(Token& token, const char** start, U32* length, bool allowKeywords) {
    bool sym1 = (m - p >= 2) && isSymbol(p[1]);
    bool sym2 = (m - p >= 3) && isSymbol(p[2]);

    token.type = Token::VarSym;

    if(!sym1) {
        // Check for various reserved operators of length 1.
        if(*p == ':') {
            // Single colon.
            token.type = Token::opColon;
        } else if(*p == '.') {
            // Single dot.
            token.type = Token::opDot;
        } else if(*p == '=') {
            // This is the reserved Equals operator.
            token.type = Token::opEquals;
        } else if(*p == '\\') {
            // This is the reserved backslash operator.
            token.type = Token::opBackSlash;
        } else if(*p == '|') {
            // This is the reserved bar operator.
            token.type = Token::opBar;
        } else if(*p == '$') {
            // This is the reserved dollar operator.
            token.type = Token::opDollar;
        } else if(*p == '@') {
            // Handle some special keywords that start with @.
            auto c = p + 1;
            if(allowKeywords && compareConstString(c, m, "data")) {
                token.type = Token::kwAtData;
                p = c;
                return;
            } else {
                // This is the reserved at operator.
                token.type = Token::opAt;
            }
        } else if(*p == '~') {
            // This is the reserved tilde operator.
            token.type = Token::opTilde;
        } else if(*p == '&') {
            token.type = Token::opAmp;
        }
    } else if(!sym2) {
        // Check for various reserved operators of length 2.
        if(*p == ':' && p[1] == ':') {
            // This is the reserved ColonColon operator.
            token.type = Token::opColonColon;
        } else if(*p == '=' && p[1] == '>') {
            // This is the reserved double-arrow operator.
            token.type = Token::opArrowD;
        } else if(*p == '.' && p[1] == '.') {
            // This is the reserved DotDot operator.
            token.type = Token::opDotDot;
        }  else if(*p == '<' && p[1] == '-') {
            // This is the reserved arrow-left operator.
            token.type = Token::opArrowL;
        } else if(*p == '-' && p[1] == '>') {
            // This is the reserved arrow-right operator.
            token.type = Token::opArrowR;
        }
    }

    if(token.type == Token::VarSym) {
        // Check if this is a constructor.
        if(*p == ':') {
            token.type = Token::ConSym;
        } else {
            token.type = Token::VarSym;
        }

        // Parse a symbol sequence.
        // Get the length of the sequence, we already know that the first one is a symbol.
        U32 count = 1;
        auto s = p;
        while(p < m && isSymbol(*(++p))) count++;

        // Check for a single minus operator - used for parser optimization.
        token.singleMinus = count == 1 && *s == '-';

        // Store the identifier data.
        *start = s;
        *length = count;
    } else {
        // Skip to the next token.
        if(sym1) p += 2;
        else p++;
    }
}

void Lexer::parseSpecial(Token& token) {
    token.type = (Token::Type)*p++;
}

void Lexer::parseQualifier(Token& token) {
    auto start = p;
    U32 length = 1;
    U32 segments = 1;
    token.type = Token::ConID;

    while(true) {
        while(m - p >= 2 && isIdentifier(*(++p))) {
            length++;
        }

        if(*p == '.') {
            if(m - p < 2) break;

            bool u = isUpperCase(p[1]);
            bool l = isLowerCase(p[1]) || p[1] == '_';
            bool s = isSymbol(p[1]);

            if(!(u || l || s)) break;

            length++;
            segments++;
            p++;

            // If the next character is upper case, we either have a ConID or another qualifier.
            if(u) {
                length++;
                continue;
            }

            // If the next character is lowercase, we either have a VarID or keyword.
            if(l) {
                const char* subStart;
                U32 subLength;
                parseVariable(token, &subStart, &subLength);

                // If this was a keyword, we parse as a constructor and dot operator instead.
                if(token.type == Token::VarID) {
                    length += subLength;
                } else {
                    token.type = Token::ConID;
                    length--;
                    p = start + length;
                }

                break;
            }

            // If the next character is a symbol, we have a VarSym or ConSym.
            if(s) {
                const char* subStart;
                U32 subLength;
                parseSymbol(token, &subStart, &subLength, false);

                // If this was a builtin symbol, we parse as a constructor and dot operator instead.
                if(token.type == Token::VarSym) {
                    length += subLength;
                } else {
                    token.type = Token::ConID;
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
    token.data.id = id;
}

void Lexer::parseVariable(Token& token, const char** start, U32* length) {
    token.type = Token::VarID;

    // First, check if we have a reserved keyword.
    auto c = p + 1;
    switch(*p) {
        case '_':
            token.type = Token::kw_;
            break;
        case 'a':
            if(compareConstString(c, m, "lias")) token.type = Token::kwAlias;
            break;
        case 'b':
            if(compareConstString(c, m, "reak")) token.type = Token::kwBreak;
            break;
        case 'c':
            if(compareConstString(c, m, "lass")) token.type = Token::kwClass;
            else if(compareConstString(c, m, "ontinue")) token.type = Token::kwContinue;
            break;
        case 'd':
            if(compareConstString(c, m, "ata")) token.type = Token::kwData;
            else if(compareConstString(c, m, "efault")) token.type = Token::kwDefault;
            else if(compareConstString(c, m, "eriving")) token.type = Token::kwDeriving;
            else if(c < m && *c == 'o') {c++; token.type = Token::kwDo;}
            break;
        case 'e':
            if(compareConstString(c, m, "lse")) token.type = Token::kwElse;
            break;
        case 'f':
            if(c < m && *c == 'n') {c++; token.type = Token::kwFn;}
            else if(compareConstString(c, m, "oreign")) token.type = Token::kwForeign;
            else if(*c == 'o' && c[1] == 'r') {c += 2; token.type = Token::kwFor;}
            break;
        case 'i':
            if(c < m && *c == 'f') {c++; token.type = Token::kwIf;}
            else if(compareConstString(c, m, "mport")) token.type = Token::kwImport;
            else if(c < m && *c == 'n' && !isIdentifier(c[1])) {c++; token.type = Token::kwIn;}
            else if(compareConstString(c, m, "nfix")) {
                if(c < m && *c == 'l') {c++; token.type = Token::kwInfixL;}
                else if(c < m && *c == 'r') {c++; token.type = Token::kwInfixR;}
            } else if(compareConstString(c, m, "nstance")) token.type = Token::kwInstance;
            else if(c < m && *c == 's') {c++; token.type = Token::kwIs;}
            else if(compareConstString(c, m, "ter")) token.type = Token::kwIter;
            break;
        case 'l':
            if(compareConstString(c, m, "ens")) token.type = Token::kwLens;
            else if(m - c >= 2 && *c == 'e' && c[1] == 't') {c += 2; token.type = Token::kwLet;}
            break;
        case 'm':
            if(compareConstString(c, m, "atch")) token.type = Token::kwMatch;
            else if(compareConstString(c, m, "odule")) token.type = Token::kwModule;
            break;
        case 'n':
            if(compareConstString(c, m, "ewtype")) token.type = Token::kwNewType;
            break;
        case 'p':
            if(compareConstString(c, m, "refixr")) token.type = Token::kwPrefixR;
            else if(m - c >= 2 && *c == 'u' && c[1] == 'b') {c += 2; token.type = Token::kwPub;}
            break;
        case 'r':
            if(compareConstString(c, m, "eturn")) token.type = Token::kwReturn;
            break;
        case 's':
            if(compareConstString(c, m, "uffixl")) token.type = Token::kwSuffixL;
            break;
        case 't':
            if(compareConstString(c, m, "hen")) token.type = Token::kwThen;
            break;
        case 'w':
            if(compareConstString(c, m, "here")) token.type = Token::kwWhere;
            else if(compareConstString(c, m, "hile")) token.type = Token::kwWhile;
            break;
        case 'y':
            if(compareConstString(c, m, "ield")) token.type = Token::kwYield;
            break;
        default: ;
    }

    // We have to read the longest possible lexeme.
    // If a reserved keyword was found, we check if a longer lexeme is possible.
    if(token.type != Token::VarID) {
        if(c < m && isIdentifier(*c)) {
            token.type = Token::VarID;
        } else {
            p = c;
            return;
        }
    }

    // Read the identifier name.
    U32 count = 1;
    auto s = p;
    while(m - p >= 2 && isIdentifier(*(++p))) count++;

    *start = s;
    *length = count;
}

void Lexer::next(Token& token) {
    parseT:
    // This needs to be reset manually.
    token.singleMinus = false;

    startWhitespace(token);

    // Check if we are inside a string literal.
    if(formatting == 3) {
        startLocation(token);
        formatting = 0;
        goto stringLit;
    } else {
        // Skip any whitespace and comments.
        skipWhitespace();
        startLocation(token);
    }

    // Check for the end of the file.
    if(p >= m) {
        // Tokens past the file end never have indentation.
        token.startColumn = 0;
        if(blockCount) {
            token.type = Token::EndOfBlock;
        } else {
            token.type = Token::EndOfFile;
        }
    }

    // Check if we need to insert a layout token.
    else if(token.startColumn == indentation && !newItem) {
        token.type = Token::EndOfStmt;
        newItem = true;
        goto newItem;
    }

    // Check if we need to end a layout block.
    else if(token.startColumn < indentation) {
        token.type = Token::EndOfBlock;
    }

    // Check for start of string formatting.
    else if(formatting == 1) {
        token.type = Token::StartOfFormat;
        formatting = 2;
    }

    // Check for end of string formatting.
    else if(formatting == 2 && *p == kFormatEnd) {
        // Issue a format end and make sure the next token is parsed as a string literal.
        // Don't skip the character - ParseStringLiteral skips one at the beginning.
        token.type = Token::EndOfFormat;
        formatting = 3;
    }

    // Check for integral literals.
    else if(isDigit(*p)) {
        auto lit = parseNumericLiteral(p, m);

        if(lit.isInteger) {
            token.type = Token::Integer;
            token.data.integer = lit.i;
        } else {
            token.type = Token::Float;
            token.data.floating = lit.f;
        }
    }

    // Check for character literals.
    else if(*p == '\'') {
        token.data.character = parseCharLiteral(p, m, diag);
        token.type = Token::Char;
    }

    // Check for string literals.
    else if(*p == '\"') {
        stringLit:
        // Since string literals can span multiple lines, this may update location.line.
        token.type = Token::String;
        token.data.id = parseStringLiteral();
    }

    // Check for special operators.
    else if(isSpecial(*p)) {
        parseSpecial(token);
    }

    // Parse symbols.
    else if(isSymbol(*p)) {
        const char* start;
        U32 length;
        parseSymbol(token, &start, &length, true);

        if(token.type == Token::VarSym) {
            auto name = (char*)context.stringArena.alloc(length);
            copyMem(start, name, length);
            token.data.id = context.addUnqualifiedName(name, length);
        }
    }

    // Parse ConIDs
    else if(isUpperCase(*p)) {
        parseQualifier(token);
    }

    // Parse variables and reserved ids.
    else if(isLowerCase(*p) || *p == '_') {
        const char* start;
        U32 length;
        parseVariable(token, &start, &length);

        if(token.type == Token::VarID) {
            auto name = (char*)context.stringArena.alloc(length);
            copyMem(start, name, length);
            token.data.id = context.addUnqualifiedName(name, length);
        }
    }

    // Unknown token - issue an error and skip it.
    else {
        auto start = position();
        auto character = *p;
        p++;

        // Bytes are what the lexer works in, so a character it cannot use is shown as itself if it
        // is printable and as an escape otherwise - which is what the bytes of, say, a pasted
        // typographic quote come out as.
        char described[8];
        Size describedLength;

        if(character >= ' ' && character <= '~') {
            described[0] = character;
            describedLength = 1;
        } else {
            described[0] = '\\';
            described[1] = 'x';
            describedLength = 2 + showHex(U8(character), described + 2, sizeof(described) - 2);
        }

        auto location = locationFrom(start);
        diag.error("unknown token '%@'"_v, &location, StringView { described, describedLength });
        goto parseT;
    }

    newItem = false;
    newItem:
    endLocation(token);
}

void Lexer::startLocation(Token& token) {
    token.startLine = line;
    token.startColumn = (p - l) + tabs * (kTabWidth - 1);
    token.startOffset = p - text;
}

void Lexer::startWhitespace(Token& token) {
    token.whitespaceLine = line;
    token.whitespaceColumn = (p - l) + tabs * (kTabWidth - 1);
    token.whitespaceOffset = p - text;
}

void Lexer::endLocation(Token& token) {
    token.endLine = line;
    token.endColumn = (p - l) + tabs * (kTabWidth - 1);
    token.endOffset = p - text;
}
