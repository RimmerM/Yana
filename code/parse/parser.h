#pragma once

#include "lexer.h"
#include "ast.h"

struct Parser {
    Parser(Context& context, Diagnostics& diag, Module& module, const char* text):
        context(context), module(module), lexer(context, diag, text, &token) {
        lexer.next();
    }

    void parseModule();
    void parseDecl();
    Decl* parseFunDecl();
    void parseDataDecl();
    void parseTypeDecl();
    void parseForeignDecl();

    Expr* parseExprSeq();
    Expr* parseExpr();
    Expr* parseTypedExpr();
    Expr* parseInfixExpr();
    Expr* parsePrefixExpr();
    Expr* parseLeftExpr();
    Expr* parseCallExpr();
    Expr* parseAppExpr();
    Expr* parseCaseExpr();
    Expr* parseBaseExpr();

    /// Parses a literal token. The caller should ensure that the token is a literal.
    Expr* parseLiteral();

    /// Parses a string literal token. The caller should ensure that the token is a string literal.
    Expr* parseStringLiteral();

    Expr* parseVarDecl(bool constant);
    Expr* parseDeclExpr(bool constant);
    void parseFixity();
    Alt* parseAlt();
    VarExpr* parseVar();
    VarExpr* parseQop();

    Type* parseType();
    Type* parseAType();
    SimpleType* parseSimpleType();
    Type* parseTupleType();
    Expr* parseTupleExpr();
    Expr* parseElse();
    Con* parseCon();

    Pat* parseLeftPattern();
    Pat* parsePattern();

    void addFixity(Fixity f);
    Expr* error(const char* text);

    void eat() {lexer.next();}

    template<class T> List<T>* list(const T& t) {return new(buffer) List<T>(t);}

    auto tokenEat(Token::Type type) {
        return [=] {
            if(token.type == type) {
                eat();
                return true;
            } else {
                return false;
            }
        };
    }

    auto tokenCheck(Token::Type type) {
        return [=] {
            return token.type == type;
        };
    }

    auto tokenRequire(Token::Type type) {
        return [=] {
            if(token.type == type) {
                eat();
                return true;
            } else {
                error("expected token");
                return false;
            }
        };
    }

    template<class F>
    auto node(F&& f) {
        auto startLine = token.startLine;
        auto startColumn = token.startColumn;
        auto startOffset = token.startOffset;
        auto result = f();
        result->sourceModule = module.name;
        result->sourceStart.line = startLine;
        result->sourceStart.column = startColumn;
        result->sourceStart.offset = startOffset;
        result->sourceEnd.line = token.whitespaceLine;
        result->sourceEnd.column = token.whitespaceColumn;
        result->sourceEnd.offset = token.whitespaceOffset;
        return result;
    }

    template<class F>
    auto withLevel(F&& f) {
        IndentLevel level{token, lexer};
        auto r = f();
        level.end();
        if(token.type == Token::EndOfBlock) eat();
        return r;
    }

    template<class F, class Start, class End>
    auto between(F&& f, Start&& start, End&& end) -> decltype(f()) {
        if(!start()) return nullptr;
        auto res = f();
        if(!end()) return nullptr;
        return res;
    }

    template<class F> auto between(F&& f, Token::Type start, Token::Type end) {
        return between(f, tokenRequire(start), tokenRequire(end));
    }

    template<class F, class Sep, class End>
    auto sepBy(F&& f, Sep&& sep, End&& end) -> List<decltype(f())>* {
        if(!end()) return nullptr;

        auto list = list(f());
        auto p = list;
        while(sep()) {
            auto l = list(f());
            p->next = l;
            p = l;
        }

        return list;
    }

    template<class F, class Sep>
    auto sepBy1(F&& f, Sep&& sep) -> List<decltype(f())>* {
        auto expr = list(f());
        auto p = expr;
        while(sep()) {
            auto l = list(f());
            p->next = l;
            p = l;
        }

        return expr;
    }

    template<class F> auto sepBy1(F&& f, Token::Type sep) {return sepBy1(f, tokenEat(sep));}
    template<class F> auto sepBy(F&& f, Token::Type sep, Token::Type end) {return sepBy(f, tokenEat(sep), tokenCheck(end));}

    Context& context;
    Module& module;
    Arena buffer;
    Token token;
    Lexer lexer;
};