#include "lexer.h"
#include "../util/lexer_util.h"

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
        .sourceStart = { .offset = start.offset, .line = start.line, .column = start.column },
        .sourceEnd = { .offset = end.offset, .line = end.line, .column = end.column },
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

/*
 * The three characters a comment body is allowed to care about.
 *
 * A newline moves the location, a `{` may open a nested comment and a `-` may close one. Every other
 * character inside a comment is skipped without being looked at, which is what lets `findChar` do
 * the skipping sixteen bytes at a time instead of the loop doing it one at a time - see
 * skipWhitespace, where that scan was 4% of the whole compiler.
 */
static const char kCommentChars[] = { '\n', '{', '-' };

void Lexer::skipWhitespace() {
    while(p < m) {
        // Skip whitespace.
        if(!handleWhitespace()) {
            // Check for single-line comments.
            if(m - p > 3 && *p == '-' && p[1] == '-' && !isSymbol(p[2])) {
                // Skip the current line. The line end is searched for rather than walked to, since
                // nothing between here and it means anything.
                p += 2;
                auto end = Tritium::findChar(StringView { p, Size(m - p) }, '\n');
                p = end ? end : m;

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
                    /*
                     * Straight to the next character that could mean something.
                     *
                     * The three tests below are the body this loop always had; what is new is that
                     * the characters none of them can fire on are skipped by the scan rather than
                     * stepped over one at a time. `p` still advances by one past an opener or a
                     * non-final closer, which is load-bearing: `{-}` opens and closes on the same
                     * `-`, and advancing by two past a `{-` would stop it closing.
                     */
                    auto next = Tritium::findChar(StringView { p, Size(m - p) },
                                                  Buffer<char> { (char*)kCommentChars, 3 });
                    if(!next) {
                        p = m;
                        break;
                    }

                    p = next;

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
                    location.sourceEnd = { commentStart.offset + 2, commentStart.line, commentStart.column + 2 };
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
    bool sym3 = (m - p >= 4) && isSymbol(p[3]);

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
        } else if(*p == '\'') {
            // The shared loan marker. A run it is part of is an ordinary VarSym, the same rule every
            // other entry in tokens.def is read under - which is what leaves `&'` free to become one
            // reserved lexeme when named loan groups arrive.
            token.type = Token::opQuote;
        } else if(*p == '?') {
            // Only a `?` alone: a run it is part of - `??`, `<?>` - is still an ordinary VarSym, the
            // same rule every other entry in tokens.def is read under.
            token.type = Token::opQuestion;
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
        } else if(*p == '?' && p[1] == '.') {
            // Optional chaining. Its own lexeme rather than `?` followed by `.`, because the two
            // spellings mean different things: `x?.f` skips the rest of the chain, and `(x?).f`
            // leaves the function. A run rule that made them the same token sequence would make
            // the commoner of the two the one nobody can write.
            token.type = Token::opQuestionDot;
        }  else if(*p == '<' && p[1] == '-') {
            // This is the reserved arrow-left operator.
            token.type = Token::opArrowL;
        } else if(*p == '-' && p[1] == '>') {
            // This is the reserved arrow-right operator.
            token.type = Token::opArrowR;
        }
    } else if(!sym3) {
        // Check for various reserved operators of length 3.
        if(*p == '.' && p[1] == '.' && p[2] == '=') {
            // This is the reserved DotDotEq operator - `..` including its upper bound.
            token.type = Token::opDotDotEq;
        } else if(*p == '&' && p[1] == '-' && p[2] == '>') {
            // `let &->x`, the two binding axes at once. One lexeme rather than `&` followed by
            // `->`, because a symbol run is classified whole: `&->` written as two tokens is not
            // something the run rule can produce, so the combination has to be an entry of its own.
            token.type = Token::opAmpArrowR;
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
        //
        // The test is on the incremented pointer, as it is in parseVariable: `p < m` before the
        // increment is a test that the character *already read* is inside the buffer, so it read one
        // byte past the end for a symbol run that ends the file.
        U32 count = 1;
        auto s = p;
        while(++p < m && isSymbol(*p)) count++;

        // Check for a single minus operator - used for parser optimization.
        token.singleMinus = count == 1 && *s == '-';

        // Store the identifier data.
        *start = s;
        *length = count;
    } else {
        /*
         * Skip to the next token. A reserved operator's length is the length of the symbol *run* it
         * was recognized in, which is what the branch above already selected on - so the test has to
         * be the same conjunction it used, not `sym2` alone.
         *
         * `sym2` on its own says only that the third character is a symbol, which it may well be
         * across a gap: in `= %Y` the run is the single `=`, and `p[2]` is the `%` of the next
         * token. Advancing by three there swallows a token whole and the parse silently loses it.
         */
        if(sym1 && sym2) p += 3;
        else if(sym1) p += 2;
        else p++;
    }
}

/*
 * The three brackets, and the depth layout suppression reads - Analysis-Language.md §6.
 *
 * `{` is unambiguously a bracket here. A layout block is opened by `:` and indentation, `{-` is
 * consumed by skipWhitespace before this is reached, and an interpolation's own braces never arrive:
 * parseStringLiteral consumes the `{` that starts one and the `}` that ends one is read off the
 * `formatting` state above, so neither is a token this sees.
 *
 * A close saturates at zero rather than wrapping. Source with an unbalanced `)` is already being
 * reported, and a depth that went below the enclosing block's would suppress nothing while a wrapped
 * one would suppress everything to the end of the file.
 */
void Lexer::parseSpecial(Token& token) {
    switch(*p) {
        case Token::ParenL:
        case Token::BracketL:
        case Token::BraceL:
            brackets++;
            break;
        case Token::ParenR:
        case Token::BracketR:
        case Token::BraceR:
            if(brackets) brackets--;
            break;
        default:
            break;
    }

    token.type = (Token::Type)*p++;
}

void Lexer::parseQualifier(Token& token) {
    auto start = p;
    U32 length = 1;
    U32 segments = 1;
    token.type = Token::ConID;

    while(true) {
        // See parseVariable: `p` has to end one past the segment, and a look-ahead test leaves it
        // on the last character when the segment runs to the end of the file.
        while(++p < m && isIdentifier(*p)) {
            length++;
        }

        if(p < m && *p == '.') {
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
            // `m - c >= 2` for the same reason every other two-character keyword here has it: an
            // `f` at the end of the file leaves `c` at the end of the buffer, and reading `c[1]`
            // there is two bytes past it. What it read was whatever the allocator left, so `f`
            // became `for` whenever those bytes happened to say so.
            else if(m - c >= 2 && *c == 'o' && c[1] == 'r') {c += 2; token.type = Token::kwFor;}
            break;
        case 'i':
            if(c < m && *c == 'f') {c++; token.type = Token::kwIf;}
            else if(compareConstString(c, m, "mport")) token.type = Token::kwImport;
            // The end of the file ends the identifier, so an `in` that is the last thing in the
            // file is the keyword - and `c[1]` is not there to be asked.
            else if(c < m && *c == 'n' && (c + 1 >= m || !isIdentifier(c[1]))) {c++; token.type = Token::kwIn;}
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
    //
    // `++p < m` rather than a look-ahead test, so that `p` ends one past the identifier in every
    // case including the one where the identifier is the last thing in the file. Testing whether
    // there is a *next* character to look at left `p` on the final character there, and the lexer
    // then produced that one character as another identifier, for ever - see the assertion at the
    // end of next(), which is what that invariant is now written down as.
    U32 count = 1;
    auto s = p;
    while(++p < m && isIdentifier(*p)) count++;

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

    // What the column would have said, for the one reader that still needs it once a bracket has
    // stopped it being said: a construct giving up on a delimiter it will never see closed. Recorded
    // rather than acted on, because acting on it is precisely what a bracket suspends.
    token.suppressedLayout = brackets != blockBrackets &&
        (token.startColumn < indentation || (token.startColumn == indentation && !newItem));

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

    // Check if we need to insert a layout token. A bracket opened since this block began is a
    // construct that spans lines by delimiter rather than by column, so the columns inside it say
    // nothing - see `brackets` in the header.
    else if(brackets == blockBrackets && token.startColumn == indentation && !newItem) {
        token.type = Token::EndOfStmt;
        newItem = true;
        goto newItem;
    }

    // Check if we need to end a layout block.
    else if(brackets == blockBrackets && token.startColumn < indentation) {
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
        auto start = position();
        auto lit = parseNumericLiteral(p, m);

        if(lit.isInteger) {
            /*
             * More digits than a number has.
             *
             * Reported here because this is the only place that has them: by the time a literal is a
             * token it is 64 bits, and every check further on - the one a declaration's constant is
             * held to, the warning a written literal gets at its type - is a question about a value
             * that no longer exists. The token carries 2^64-1 rather than the low bits of what was
             * written, so those checks refuse it as well rather than being told it is zero.
             */
            if(lit.overflowed) {
                auto location = locationFrom(start);
                diag.error("integer literal is too large - the largest is 18446744073709551615"_v, &location);
            }

            token.type = Token::Integer;
            token.data.integer = lit.i;
        } else {
            token.type = Token::Float;
            token.data.floating = lit.f;
        }
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

    /*
     * The one invariant the whole parser rests on: a token that stands for text consumes it.
     *
     * Everything above this loops on "read a token and do something with it", and none of those
     * loops can make progress against a lexer that keeps returning the same token - a hang with no
     * diagnostic, no memory growth and no way to tell from the outside what it is doing. It has
     * happened: a look-ahead test that asked whether there was a *next* character left `p` on the
     * final character of an identifier that ended the file, and the lexer then produced that one
     * character as an identifier for ever.
     *
     * The four kinds that legitimately consume nothing are layout and string formatting, and none
     * of them can repeat: `EndOfStmt` sets `newItem`, `EndOfBlock` is answered by the parser closing
     * a level, and the two format markers flip `formatting`.
     */
    assertTrue(token.endOffset > token.startOffset ||
               token.type == Token::EndOfFile || token.type == Token::EndOfBlock ||
               token.type == Token::EndOfStmt || token.type == Token::StartOfFormat ||
               token.type == Token::EndOfFormat);
}

void Lexer::startLocation(Token& token) {
    // The depth this token was read at - see `brackets` in the header. Recorded before the token
    // itself can change it, so that a layout block opened *on* an opening bracket records the depth
    // outside it: `match x:` whose first alternative begins `{` opens its block at the depth the
    // `match` was written at, and the `}` two tokens later does not then look like a dedent.
    token.startBrackets = brackets;
    token.startLine = line;
    token.startColumn = (p - l) + tabs * (kTabWidth - 1);
    token.startOffset = p - text;

    rewind = { .p = p, .l = l, .line = line, .tabs = tabs, .formatting = formatting };
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
