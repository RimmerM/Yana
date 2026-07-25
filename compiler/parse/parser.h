#pragma once

#include "ast.h"
#include "lexer.h"
#include "../util/parser_util.h"

struct Parser: BasicParser<Lexer, Token> {
    Parser(Context& context, Lexer& lexer, StringId moduleName);

    ast::Module parseModule();
    ast::Import parseImport();
    ast::Fixity parseFixity();
    void parseDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported);
    void parseFunDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported, bool requireBody);
    void parseDataDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported);
    void parseTypeDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported);
    void parseForeignDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported);
    void parseTraitDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported);
    void parseInstanceDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported);
    void parseAttrDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported);

    ast::Expr parseBlock(bool isFun);
    ast::Expr parseExprSeq();
    ast::Expr parseExpr();
    ast::Expr parseTypedExpr();
    ast::Expr parseInfixExpr(const WithLocation& location);
    ast::Expr parsePrefixExpr(const WithLocation& location);
    ast::Expr parseLeftExpr(const WithLocation& location);
    ast::Expr parseAppExpr();
    ast::Expr parseChain(ast::Expr base, const WithLocation& startLocation);
    ast::Expr parseBaseExpr();
    ast::Expr parseSelExpr(const WithLocation& location);

    ast::Expr parseMatchExpr(const WithLocation& location);
    ast::Expr parseStringExpr(const WithLocation& location);
    ast::VarDecl parseDeclExpr();
    ast::Expr parseTupleExpr(const WithLocation& location);
    ast::Expr parseArrayExpr(const WithLocation& location);
    ast::Expr parseIfExpr();

    void parseArg(ast::ParseList<ast::Arg>& list, bool requireType);
    void parseTypeArg(ast::ParseList<ast::ArgDecl>& list);
    void parseArgDecl(ast::ParseList<ast::ArgDecl>& list);
    void parseTupArg(ast::ParseList<ast::TupArg>& list);
    ast::Expr parseVarDecl(const WithLocation& location, U32 line);
    void parseAlt(ast::ParseList<ast::Alt>& list);
    ast::Expr parseQop();
    ast::FunKind parseFunKind();

    ast::SimpleType parseSimpleType();
    ast::Type parseType();
    ast::Type parseAType(const WithLocation& location, ast::ParsePtr<ast::AttrList> attributes);
    ast::Type parseTupleType(const WithLocation& location, ast::ParsePtr<ast::AttrList> attributes);
    ast::Type parseArrayType(const WithLocation& location, ast::ParsePtr<ast::AttrList> attributes);
    void parseCon(ast::ParseList<ast::Con>& list);

    ast::FieldPat parseFieldPat();
    ast::Pat parseLeftPattern();
    ast::Pat parsePattern();

    void parseAttribute(ast::AttrList& list);
    void parseAttributes(ast::AttrList& list, bool isInline);

    ast::Constraint parseConstraint();
    void parseConstraints(ast::ConstraintList& list);

    ast::Expr toLiteral(const Token::Payload& payload, Token::Type type, const WithLocation& source);
    ast::Expr toLiteral(const WithLocation& source);

    template<class F>
    void withLevel(F&& f) {
        IndentLevel level{ token, lexer };
        f();
        level.end();
        if(token.type == Token::EndOfBlock) eat();
    }

    template<class F> auto parens(F&& f) {
        return between(f, Token::Type::ParenL, Token::Type::ParenR, "expected '('"_v, "expected ')'"_v);
    }

    template<class F> auto maybeParens(F&& f) {
        return maybeBetween(f, Token::Type::ParenL, Token::Type::ParenR, "expected '('"_v, "expected ')'"_v);
    }

    template<class F> auto braces(F&& f) {
        return between(f, Token::Type::BraceL, Token::Type::BraceR, "expected '{'"_v, "expected '}'"_v);
    }

    template<class F> auto maybeBraces(F&& f) {
        return maybeBetween(f, Token::Type::BraceL, Token::Type::BraceR, "expected '{'"_v, "expected '}'"_v);
    }

    template<class F> auto brackets(F&& f) {
        return between(f, Token::Type::BracketL, Token::Type::BracketR, "expected '['"_v, "expected ']'"_v);
    }

    template<class F> auto maybeBrackets(F&& f) {
        return maybeBetween(f, Token::Type::BracketL, Token::Type::BracketR, "expected '['"_v, "expected ']'"_v);
    }

    auto maybeVar(StringId var) {
        return maybe(Token::VarID, [&](Token& t) { return t.data.id == var; });
    }

    auto expectVarOrCon(StringView error = "expected symbol name"_v) {
        return expect(error, [&](Token& t) {
            return t.type == Token::VarID || t.type == Token::ConID;
        });
    }

    template<class T>
    ast::ParsePtr<T> heap(const T& v) {
        return new (arena) T(v) - *arena;
    }

    Context& context;
    Region<ast::ParseRegion> arena;

    StringId qualifiedId;
    StringId hidingId;
    StringId fromId;
    StringId asId;
    StringId checkedRefId; // sigil '*': checked reference (Type::Ref, aliased Ref(a)).
    StringId rawPtrId;     // sigil '%': raw, unchecked pointer (Type::Ptr, aliased Ptr(a)).
    StringId downtoId;
    StringId stepId;
    StringId setId;
    StringId arraySizeId;
};
