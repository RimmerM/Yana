#pragma once

#include "../compiler/diagnostics.h"

template<class Lexer, class Token>
struct BasicParser {
    using Payload = typename Token::Payload;
    using Type = typename Token::Type;

    struct TokenNode {
        Payload payload;
        Location node;
    };

    struct WithLocation {
        WithLocation(const BasicParser<Lexer, Token>& parser):
            parser(parser),
            startLine(parser.token.startLine),
            startColumn(parser.token.startColumn),
            startOffset(parser.token.startOffset) {}

        operator Location() const {
            return {
                .sourceModule = parser.moduleName,
                .sourceStart = {
                    .offset = startOffset,
                    .line = U16(startLine),
                    .column = U16(startColumn),
                },
                .sourceEnd = {
                    .offset = parser.token.whitespaceOffset,
                    .line = U16(parser.token.whitespaceLine),
                    .column = U16(parser.token.whitespaceColumn),
                },
            };
        }

        const BasicParser<Lexer, Token>& parser;
        U32 startLine;
        U32 startColumn;
        U32 startOffset;
    };

    BasicParser(Diagnostics& diag, Lexer& lexer, StringId moduleName): moduleName(moduleName), diag(diag), lexer(lexer) {}

    void error(StringView text, Location* node = nullptr) {
        auto where = currentNode();
        diag.error(text, node ? node : &where);
    }

    void error(StringView text, LocationId location) {
        diag.error(text, location);
    }

    void warning(StringView text, Location* node = nullptr) {
        auto where = currentNode();
        diag.warning(text, node ? node : &where);
    }

    void warning(StringView text, LocationId location) {
        diag.warning(text, location);
    }

    Location currentNode() {
        return {
            .sourceModule = moduleName,
            .sourceStart = {
                .offset = token.startOffset,
                .line = U16(token.startLine),
                .column = U16(token.startColumn),
            },
            .sourceEnd = {
                .offset = token.endOffset,
                .line = U16(token.endLine),
                .column = U16(token.endColumn),
            },
        };
    }

    void eat() {
        lexer.next(token);
    }

    template<class F>
    Maybe<Payload> maybe(F&& predicate) {
        if(predicate(token)) {
            auto payload = token.data;
            eat();
            return Just(payload);
        } else {
            return Nothing();
        }
    }

    Maybe<Payload> maybe(Type type) {
        return maybe([&](Token& t) { return t.type == type; });
    }

    template<class F>
    Maybe<Payload> maybe(Type type, F&& predicate) {
        return maybe([&](Token& t) { return t.type == type && predicate(t); });
    }

    Maybe<Payload> expect(Type type, StringView errorText) {
        auto r = maybe(type);
        if(!r) error(errorText);
        return r;
    }

    template<class F>
    Maybe<Payload> expect(Type type, StringView errorText, F&& predicate) {
        auto r = maybe(type, forward<F>(predicate));
        if(!r) error(errorText);
        return r;
    }

    template<class F>
    Maybe<Payload> expect(StringView errorText, F&& predicate) {
        auto r = maybe(forward<F>(predicate));
        if(!r) error(errorText);
        return r;
    }

    Maybe<TokenNode> maybeNode(Type type) {
        if(token.type == type) {
            TokenNode node {
                .payload = token.data,
                .node = currentNode(),
            };

            eat();
            return Just(node);
        } else {
            return Nothing();
        }
    }

    Maybe<TokenNode> expectNode(Type type, StringView errorText) {
        auto r = maybeNode(type);
        if(!r) error(errorText);
        return r;
    }

    auto tokenEat(Type type) {
        return [=] {
            if(token.type == type) {
                eat();
                return true;
            } else {
                return false;
            }
        };
    }

    auto tokenCheck(Type type) {
        return [=] {
            return token.type == type;
        };
    }

    auto tokenRequire(Type type, StringView errorText) {
        return [=] {
            if(token.type == type) {
                eat();
                return true;
            } else {
                error(errorText);
                return false;
            }
        };
    }

    template<class F, class Start, class End>
    void between(F&& f, Start&& start, End&& end) {
        if(!start()) return;
        f();
        end(); // Don't fail the whole thing because the closing token is missing.
    }

    template<class F> void between(F&& f, Type start, Type end, StringView startError, StringView endError) {
        between(f, tokenRequire(start, startError), tokenRequire(end, endError));
    }

    template<class F> void maybeBetween(F&& f, Type start, Type end, StringView startError, StringView endError) {
        if(token.type == start) between(f, start, end, startError, endError);
    }

    template<class F, class Sep, class End>
    void sepBy(F&& f, Sep&& sep, End&& end) {
        if(end()) return;
        f();

        while(sep()) {
            f();
        }
    }

    template<class F, class Sep>
    void sepBy1(F&& f, Sep&& sep) {
        f();

        while(sep()) {
            f();
        }
    }

    template<class F> void sepBy1(F&& f, Type sep) {
        sepBy1(f, tokenEat(sep));
    }

    template<class F> void sepBy(F&& f, Type sep, Type end) {
        sepBy(f, tokenEat(sep), tokenCheck(end));
    }

    StringId moduleName;
    Diagnostics& diag;
    Lexer& lexer;
    Token token;
};
