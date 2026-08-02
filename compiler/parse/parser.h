#pragma once

#include "ast.h"
#include "lexer.h"
#include "../util/parser_util.h"

struct Parser: BasicParser<Lexer, Token> {
    Parser(Context& context, Lexer& lexer, StringId moduleName);

    // Whether a module-level `fn` may be written without a body, as a class signature may be.
    // This is for the modules the compiler embeds and parses itself: Native declares a
    // dereference and a system call with their real signatures, and what they mean is generated
    // at each call site rather than written down. Ordinary source always needs a body.
    bool allowSignatures = false;

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
    void parseDefaultDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported);

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

    // A `selexpr` followed by any number of the closed suffixes `parseChain` reads - a call, a
    // subscript, a field. What a `for` header's three slots take, and what `selexpr` alone takes
    // everywhere else. See the definition for why the two are not the same production.
    ast::Expr parseChainExpr(const WithLocation& location);

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
    void parseTupUpdateArg(ast::ParseList<ast::TupUpdateArg>& list);
    ast::Expr parseVarDecl(const WithLocation& location, U32 line);
    void parseAlt(ast::ParseList<ast::Alt>& list);
    ast::Expr parseQop();
    ast::FunKind parseFunKind();

    ast::BindType parseBindType();
    bool parseReturnRoot();
    bool parseLazy();

    ast::SimpleType parseSimpleType();
    ast::Type parseType();
    ast::Type parseAType(const WithLocation& location, ast::ParsePtr<ast::AttrList> attributes);
    ast::Type parseTupleType(const WithLocation& location, ast::ParsePtr<ast::AttrList> attributes);
    ast::Type parseArrayType(const WithLocation& location, ast::ParsePtr<ast::AttrList> attributes);
    void parseCon(ast::ParseList<ast::Con>& list);

    ast::FieldPat parseFieldPat();
    ast::Pat parseLeftPattern();
    ast::Pat parsePattern();
    ast::Pat parseBoundPattern();

    void parseAttribute(ast::AttrList& list);
    void parseAttributes(ast::AttrList& list, bool isInline);

    ast::Constraint parseConstraint();
    void parseConstraints(ast::ConstraintList& list);

    ast::Expr toLiteral(const Token::Payload& payload, Token::Type type, const WithLocation& source);
    ast::Expr toLiteral(const WithLocation& source);

    // True when the layout has already ended whatever the current token could have belonged to.
    // Nothing can be parsed here, and everything that opens an indentation block has to ask
    // before opening one - see withLevel().
    bool atBlockEnd() const {
        return token.type == Token::EndOfStmt || token.type == Token::EndOfBlock || token.type == Token::EndOfFile;
    }

    /*
     * Parses an indented block, and returns false without consuming anything when there is none.
     *
     * A block is indented further than the construct it belongs to, so its first token is never a
     * layout token: a token at or left of the enclosing level is one the lexer has already turned
     * into an EndOfStmt or an EndOfBlock. Opening a level there instead sets it to the *enclosing*
     * level's column, and everything that follows - every remaining declaration in the file - is
     * then read as more of this block. That is the state a half-written `fn f():` is in for as
     * long as its body is an empty line, which is most of the time it is being written.
     */
    template<class F>
    bool withLevel(F&& f) {
        if(atBlockEnd()) return false;

        IndentLevel level{ token, lexer };
        f();
        level.end();
        if(token.type == Token::EndOfBlock) eat();
        return true;
    }

    // withLevel(), reporting `emptyError` when the block is missing. The location it points at is
    // the layout token, which is where the block should have been written.
    template<class F>
    bool block(F&& f, StringView emptyError) {
        if(withLevel(forward<F>(f))) return true;

        error(emptyError);
        return false;
    }

    /*
     * Where the parser resumes after something it could not read, and the only synchronization set
     * it needs.
     *
     * Yana's layout supplies the points for free: a token at or left of the level the broken
     * construct started at is one the lexer has already turned into an EndOfStmt or an EndOfBlock.
     * So stopping at those two stops exactly where Implementation-Tooling.md §3.1 asks, without a
     * column comparison of our own - and §3.1's third clause, a declaration keyword at column 0,
     * is unreachable at the root level for the same reason and is not implemented.
     */
    static Buffer<const Token::Type> stmtStops() {
        static constexpr Token::Type stops[] = { Token::EndOfStmt, Token::EndOfBlock };
        return { stops, 2 };
    }

    Token::Type syncStmt() { return sync(stmtStops()); }

    // Consumes the closing token of a construct, or reports `errorText` and skips ahead to it.
    // Returns false if the construct was left open - the caller should then give up on it rather
    // than parse what follows as more of the same. See skipToClose().
    bool expectClose(Token::Type end, StringView errorText);

    // Skips ahead to the closing token a construct is missing, so that parsing resumes after the
    // construct rather than inside it. Never crosses the end of the statement or block the
    // construct is in: a delimiter that was never closed should cost the declaration it is in and
    // not the ones after it. Returns true if the closing token was found, and consumes it.
    bool skipToClose(Token::Type end);

    // Like BasicParser::between, but recovers when the closing token is missing. Returns false
    // when the construct was left open, so that the caller can give up on it rather than read
    // what follows as more of itself.
    template<class F>
    bool delimited(F&& f, Token::Type start, Token::Type end, StringView startError, StringView endError) {
        if(token.type != start) {
            error(startError);
            return false;
        }

        eat();

        // A delimiter left open at the end of a statement has no contents yet rather than empty
        // ones. Asking the production inside for them reports a second thing missing on top of
        // the closing token, which is one mistake described twice - `fn f(` is not also a
        // parameter whose name is missing.
        if(atBlockEnd()) {
            error(endError);
            return false;
        }

        f();
        return expectClose(end, endError);
    }

    template<class F>
    bool maybeDelimited(F&& f, Token::Type start, Token::Type end, StringView startError, StringView endError) {
        if(token.type != start) return true;
        return delimited(f, start, end, startError, endError);
    }

    template<class F> auto parens(F&& f) {
        return delimited(f, Token::Type::ParenL, Token::Type::ParenR, "expected '('"_v, "expected ')'"_v);
    }

    template<class F> auto maybeParens(F&& f) {
        return maybeDelimited(f, Token::Type::ParenL, Token::Type::ParenR, "expected '('"_v, "expected ')'"_v);
    }

    template<class F> auto braces(F&& f) {
        return delimited(f, Token::Type::BraceL, Token::Type::BraceR, "expected '{'"_v, "expected '}'"_v);
    }

    template<class F> auto maybeBraces(F&& f) {
        return maybeDelimited(f, Token::Type::BraceL, Token::Type::BraceR, "expected '{'"_v, "expected '}'"_v);
    }

    template<class F> auto brackets(F&& f) {
        return delimited(f, Token::Type::BracketL, Token::Type::BracketR, "expected '['"_v, "expected ']'"_v);
    }

    template<class F> auto maybeBrackets(F&& f) {
        return maybeDelimited(f, Token::Type::BracketL, Token::Type::BracketR, "expected '['"_v, "expected ']'"_v);
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
    StringId arraySizeId;
    StringId lazyId;       // the one attribute with a meaning in parameter position - see parseLazy.
};
