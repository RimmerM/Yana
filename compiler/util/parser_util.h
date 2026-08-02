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
                    .line = startLine,
                    .column = startColumn,
                },
                .sourceEnd = {
                    .offset = parser.token.whitespaceOffset,
                    .line = parser.token.whitespaceLine,
                    .column = parser.token.whitespaceColumn,
                },
            };
        }

        const BasicParser<Lexer, Token>& parser;
        U32 startLine;
        U32 startColumn;
        U32 startOffset;
    };

    BasicParser(Diagnostics& diag, Lexer& lexer, StringId moduleName): moduleName(moduleName), diag(diag), lexer(lexer) {}

    /*
     * Whether this parse may still report. One mistake in a layout-sensitive language routinely
     * costs a diagnostic per production that gave up on it, so a file that is broken enough to
     * pass the limit is one where the next message tells the reader nothing the first fifty did
     * not - and an editor that reparses on every keystroke pays for each of them twice, once to
     * produce and once to render.
     *
     * Only the reporting stops. Recovery runs to the end of the file either way, because the AST
     * after the limit is what the rest of the editor reads.
     */
    bool canReport() {
        if(reportedErrors < errorLimit) {
            reportedErrors++;
            return true;
        }

        // The message that says why the others are missing is itself the last one reported.
        if(reportedErrors == errorLimit) {
            reportedErrors++;
            auto where = currentNode();
            diag.error("too many parse errors - the rest of this file is parsed without reporting"_v, &where);
        }

        return false;
    }

    void error(StringView text, Location* node = nullptr) {
        if(!canReport()) return;
        auto where = currentNode();
        diag.error(text, node ? node : &where);
    }

    void error(StringView text, LocationId location) {
        if(!canReport()) return;
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
                .line = token.startLine,
                .column = token.startColumn,
            },
            .sourceEnd = {
                .offset = token.endOffset,
                .line = token.endLine,
                .column = token.endColumn,
            },
        };
    }

    void eat() {
        /*
         * Where the text the parser has actually read reaches to, which is what the gap before the
         * next token is measured from - see Parser::beforeCursorToken.
         *
         * Only tokens that stand for text. A layout token is zero-width and sits at the position of
         * the token it precedes, so counting one would put the end of what has been read *after*
         * the blank lines it was emitted for - and the gap those blank lines are would vanish. A
         * token's own `whitespaceOffset` does not answer this either, for the same reason from the
         * other side: the layout token consumed the whitespace, so the real token after one begins
         * where it begins.
         */
        if(token.endOffset > token.startOffset) previousEnd = token.endOffset;
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

    /*
     * Discards tokens until one the caller can resume at: any type in `until`, any token
     * `alsoStop` accepts, or the end of the file, which is always a stop because nothing else is
     * left. The stopping token is not consumed - it is the one the caller is meant to see - and
     * its type is returned so the caller can tell which of its reasons for stopping applied.
     *
     * This is the discard half of error recovery. The other half is the caller's: what it puts in
     * the AST in place of what it skipped, so that a position inside the skipped range still maps
     * to the construct that contained it.
     */
    template<class F>
    Type sync(Buffer<const Type> until, F&& alsoStop) {
        while(token.type != Type::EndOfFile) {
            for(auto type: until) {
                if(token.type == type) return token.type;
            }

            if(alsoStop(token)) return token.type;
            eat();
        }

        return token.type;
    }

    Type sync(Buffer<const Type> until) {
        return sync(until, [](Token&) { return false; });
    }

    // `expect`, recovering: where `expect` leaves the offending token in place and makes it the
    // caller's problem, this one discards up to the next synchronization point. Use it where what
    // follows the missing token cannot be read as anything - `default` names a class and then says
    // what it defaults to, and without the class neither half means anything - and not where the
    // rest of the construct is still worth keeping. Discarding a declaration costs an editor the
    // arguments and the body it would have completed against, which is worse than a cascade.
    Maybe<Payload> expectSync(Type type, StringView errorText, Buffer<const Type> until) {
        return expectSync(errorText, until, [&](Token& t) { return t.type == type; });
    }

    template<class F>
    Maybe<Payload> expectSync(StringView errorText, Buffer<const Type> until, F&& predicate) {
        auto r = maybe(forward<F>(predicate));
        if(!r) {
            error(errorText);
            sync(until);
        }

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

    // Where the last token the parser consumed ended - see eat().
    U32 previousEnd = 0;

    // How many diagnostics this parse reports before it goes quiet - see canReport().
    U32 errorLimit = 50;
    U32 reportedErrors = 0;
};
