#pragma once

#include "lexer.h"
#include "ast.h"

struct Parser {
    Parser(Context& context, ast::Module& module, const char* text);

    void parseModule();
    void parseImport();
    void parseFixity();
    ast::Decl* parseDecl();
    ast::Decl* parseFunDecl();
    ast::Decl* parseDataDecl();
    ast::Decl* parseTypeDecl();
    ast::Decl* parseForeignDecl();
    ast::Decl* parseClassDecl();
    ast::Decl* parseInstanceDecl();

    ast::Expr* parseBlock(bool isFun);
    ast::Expr* parseExprSeq();
    ast::Expr* parseExpr();
    ast::Expr* parseTypedExpr();
    ast::Expr* parseInfixExpr();
    ast::Expr* parsePrefixExpr();
    ast::Expr* parseLeftExpr();
    ast::Expr* parseAppExpr();
    ast::Expr* parseChain(ast::Expr* base);
    ast::Expr* parseBaseExpr();
    ast::Expr* parseSelExpr();

    ast::Expr* parseCaseExpr();
    ast::Expr* parseStringExpr();
    ast::Expr* parseDeclExpr();
    ast::Expr* parseTupleExpr();
    ast::Expr* parseArrayExpr();
    ast::Expr* parseIfExpr();

    ast::Arg parseArg(bool requireType);
    ast::ArgDecl parseTypeArg();
    ast::ArgDecl parseArgDecl();
    ast::TupArg parseTupArg();
    ast::Expr* parseVarDecl();
    ast::Alt parseAlt();
    ast::VarExpr* parseVar();
    ast::VarExpr* parseQop();

    ast::Type* parseType();
    ast::Type* parseAType();
    ast::SimpleType* parseSimpleType();
    ast::Type* parseTupleType();
    ast::Con parseCon();

    ast::Pat* parseLeftPattern();
    ast::Pat* parsePattern();

    void error(const char* text, Node* node = nullptr);

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

    auto tokenRequire(Token::Type type, const char* errorText) {
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

    template<class F> auto between(F&& f, Token::Type start, Token::Type end, const char* startError, const char* endError) {
        return between(f, tokenRequire(start, startError), tokenRequire(end, endError));
    }

    template<class F> auto maybeBetween(F&& f, Token::Type start, Token::Type end, const char* startError, const char* endError) {
        return token.type == start ? between(f, start, end, startError, endError) : nullptr;
    }

    template<class F> auto parens(F&& f) {
        return between(f, Token::Type::ParenL, Token::Type::ParenR, "expected '('", "expected ')'");
    }

    template<class F> auto maybeParens(F&& f) {
        return maybeBetween(f, Token::Type::ParenL, Token::Type::ParenR, "expected '('", "expected ')'");
    }

    template<class F> auto braces(F&& f) {
        return between(f, Token::Type::BraceL, Token::Type::BraceR, "expected '{'", "expected '}'");
    }

    template<class F> auto maybeBraces(F&& f) {
        return maybeBetween(f, Token::Type::BraceL, Token::Type::BraceR, "expected '{'", "expected '}'");
    }

    template<class F> auto brackets(F&& f) {
        return between(f, Token::Type::BracketL, Token::Type::BracketR, "expected '['", "expected ']'");
    }

    template<class F> auto maybeBrackets(F&& f) {
        return maybeBetween(f, Token::Type::BracketL, Token::Type::BracketR, "expected '['", "expected ']'");
    }

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
    ast::Module& module;

    Arena buffer;
    Token token;
    Lexer lexer;

    Id qualifiedId;
    Id hidingId;
    Id fromId;
    Id asId;
    Id refId;
    Id ptrId;
    Id valId;
    Id downtoId;
    Id hashId;
};