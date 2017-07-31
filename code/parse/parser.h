#pragma once

#include "lexer.h"
#include "ast.h"

struct Parser {
    static const char kRefSigil;
    static const char kPtrSigil;

    Parser(Context& context, Diagnostics& diag, Module& module, const char* text):
        text(text), context(context), diag(diag), module(module), lexer(context, diag, text, &token) {

        qualifiedId = context.addUnqualifiedName("qualified", 9);
        hidingId = context.addUnqualifiedName("hiding", 6);
        fromId = context.addUnqualifiedName("from", 4);
        asId = context.addUnqualifiedName("as", 2);
        refId = context.addUnqualifiedName(&kRefSigil, 1);
        ptrId = context.addUnqualifiedName(&kPtrSigil, 1);

        lexer.next();
    }

    void parseModule();
    void parseImport();
    void parseFixity();
    Decl* parseDecl();
    Decl* parseFunDecl();
    Decl* parseDataDecl();
    Decl* parseTypeDecl();
    Decl* parseForeignDecl();
    Decl* parseClassDecl();
    Decl* parseInstanceDecl();

    Expr* parseBlock(bool isFun);
    Expr* parseExprSeq();
    Expr* parseExpr();
    Expr* parseTypedExpr();
    Expr* parseInfixExpr();
    Expr* parsePrefixExpr();
    Expr* parseLeftExpr();
    Expr* parseAppExpr();
    Expr* parseChain(Expr* base);
    Expr* parseBaseExpr();
    Expr* parseSelExpr();

    Expr* parseCaseExpr();
    Expr* parseStringExpr();
    Expr* parseDeclExpr();
    Expr* parseTupleExpr();
    Expr* parseArrayExpr();
    Expr* parseIfExpr();

    Arg parseArg(bool requireType);
    ArgDecl parseTypeArg();
    ArgDecl parseArgDecl();
    TupArg parseTupArg();
    Expr* parseVarDecl();
    Alt parseAlt();
    VarExpr* parseVar();
    VarExpr* parseQop();

    Type* parseType();
    Type* parseAType();
    SimpleType* parseSimpleType();
    Type* parseTupleType();
    Con parseCon();

    Pat* parseLeftPattern();
    Pat* parsePattern();

    void error(const char* text);

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
        end(); // Don't fail the whole thing because the closing token is missing.
        return res;
    }

    template<class F> auto between(F&& f, Token::Type start, Token::Type end) {
        return between(f, tokenRequire(start), tokenRequire(end));
    }

    template<class F> auto maybeBetween(F&& f, Token::Type start, Token::Type end) {
        return token.type == start ? between(f, start, end) : nullptr;
    }

    template<class F> auto parens(F&& f) {return between(f, Token::Type::ParenL, Token::Type::ParenR);}
    template<class F> auto maybeParens(F&& f) {return maybeBetween(f, Token::Type::ParenL, Token::Type::ParenR);}
    template<class F> auto braces(F&& f) {return between(f, Token::Type::BraceL, Token::Type::BraceR);}
    template<class F> auto maybeBraces(F&& f) {return maybeBetween(f, Token::Type::BraceL, Token::Type::BraceR);}
    template<class F> auto brackets(F&& f) {return between(f, Token::Type::BracketL, Token::Type::BracketR);}
    template<class F> auto maybeBrackets(F&& f) {return maybeBetween(f, Token::Type::BracketL, Token::Type::BracketR);}

    template<class F, class Sep, class End>
    auto sepBy(F&& f, Sep&& sep, End&& end) -> List<decltype(f())>* {
        if(end()) return nullptr;

        auto n = list(f());
        auto p = n;
        while(sep()) {
            auto l = list(f());
            p->next = l;
            p = l;
        }

        return n;
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

    const char* text;
    Context& context;
    Diagnostics& diag;
    Module& module;

    Arena buffer;
    Token token;
    Lexer lexer;

    Id qualifiedId;
    Id hidingId;
    Id fromId;
    Id asId;
    Id refId;
    Id ptrId;
};