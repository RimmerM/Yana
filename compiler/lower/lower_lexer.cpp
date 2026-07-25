#include "lower_lexer.h"
#include "../util/lexer_util.h"

static bool isSpecial(char c) {
    return
        c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']' ||
        c == ',' || c == ';' || c == '=' || c == ':' || c == '-' ||
        c == '<' || c == '>';
}

LowerLexer::LowerLexer(Context& context, Diagnostics& diag, const StringView& text) :
    text(text.ptr), p(text.ptr), l(text.ptr), m(text.ptr + text.length), context(context), diag(diag) {}

void LowerLexer::next(LowerToken& token) {
    while(1) {
        // Skip any whitespace and comments.
        startWhitespace(token);
        skipWhitespace();
        startLocation(token);

        // Check for the end of the file.
        if(p >= m) {
            // Tokens past the file end never have indentation.
            token.startColumn = 0;
            token.type = LowerToken::EndOfFile;
            break;
        }

        if(*p == '\n') {
            nextLine();
            token.type = LowerToken::EndOfStmt;
            p++;
            break;
        }

        // Check for integral literals.
        if(isDigit(*p)) {
            auto lit = parseNumericLiteral(p, m);

            if(lit.isInteger) {
                if(p < m && (*p == 'l' || *p == 'L')) {
                    p++;
                    token.type = LowerToken::Long;
                } else {
                    token.type = LowerToken::Int;
                }

                token.data.integer = lit.i;
            } else {
                if(p < m && (*p == 'f' || *p == 'F')) {
                    p++;
                    token.type = LowerToken::Float;
                } else {
                    token.type = LowerToken::Double;
                }

                token.data.floating = lit.f;
            }

            break;
        }

        // Check for character literals.
        if(*p == '\'') {
            token.data.integer = parseCharLiteral(p, m, diag);
            token.type = LowerToken::Int;
            break;
        }

        // Check for string literals.
        if(*p == '\"') {
            // Since string literals can span multiple lines, this may update location.line.
            token.type = LowerToken::String;
            token.data.id = parseStringLiteral();
            break;
        }

        // Check for special operators.
        if(isSpecial(*p)) {
            token.type = (LowerToken::Type)*p++;
            break;
        }

        // Parse symbols.
        if(*p == '%' || *p == '@' || isIdentifier(*p)) {
            if(*p == '%') {
                p++;
                token.type = LowerToken::RegID;
            } else if(*p == '@') {
                p++;
                token.type = LowerToken::GlobalID;
            } else {
                token.type = LowerToken::LabelID;
            }

            auto s = p;
            while(p < m && isIdentifier(*p)) {
                p++;
            }

            auto length = p - s;
            auto name = (char*)context.stringArena.alloc(length);
            copyMem(s, name, length);

            token.data.id = context.addUnqualifiedName(name, length);
            break;
        }

        // Unknown token - issue an error and skip it.
        diag.error("unknown token '%@'"_v, nullptr, *p);
        p++;
    }

    endLocation(token);
}

void LowerLexer::nextLine() {
    l = p + 1;
    line++;
}

bool LowerLexer::handleWhitespace() {
    return *p == '\t' || isWhiteChar(*p);
}

void LowerLexer::skipWhitespace() {
    while(p < m) {
        // Newlines are handled separately, since they indicate statement ends.
        if(*p == '\n') break;

        // Skip whitespace.
        if(!handleWhitespace()) {
            // Check for single-line comments.
            if(*p == '#') {
                // Skip the current line.
                p += 1;
                while(p < m && *p != '\n') p++;

                // If this is a newline, we update the location.
                // If it is the file end, the caller will take care of it.
                if(p < m && *p == '\n') {
                    nextLine();
                    p++;
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

StringId LowerLexer::parseStringLiteral() {
    // There is no real limit on the length of a string literal, so we use a dynamic array while parsing.
    Array<char> chars(128);

    auto terminated = false;
    p++;

    while(p < m) {
        if(*p == '\\') {
            // This is an escape sequence or gap.
            p++;
            if(p >= m) break;

            if(handleWhitespace()) {
                // This is a gap - we skip characters until the next '\'.
                // Update the current source line if needed.
                p++;
                while(p < m && handleWhitespace()) p++;

                if(p >= m || *p != '\\') {
                    // The first character after a gap must be '\'.
                    diag.warning("Missing gap end in string literal"_v, nullptr);
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

    if(!terminated) {
        // If the line ends without terminating the string, we issue a warning.
        diag.warning("Missing terminating quote in string literal"_v, nullptr);
    }

    // Create a new buffer for this string.
    auto buffer = (char*)context.stringArena.alloc(chars.size());
    copyMem(chars.pointer(), buffer, chars.size());
    return context.addUnqualifiedName(buffer, chars.size());
}

void LowerLexer::startLocation(LowerToken& token) {
    token.startLine = line;
    token.startColumn = p - l;
    token.startOffset = p - text;
}

void LowerLexer::startWhitespace(LowerToken& token) {
    token.whitespaceLine = line;
    token.whitespaceColumn = p - l;
    token.whitespaceOffset = p - text;
}

void LowerLexer::endLocation(LowerToken& token) {
    token.endLine = line;
    token.endColumn = p - l;
    token.endOffset = p - text;
}
