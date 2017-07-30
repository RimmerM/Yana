#pragma once

#include <assert.h>
#include "../compiler/context.h"
#include "../compiler/diagnostics.h"

struct Token {
    enum Type {
        EndOfFile,
        Comment,
        Whitespace,
        EndOfBlock,
        StartOfFormat,
        EndOfFormat,

        /* Special symbols */
        ParenL = '(',
        ParenR = ')',
        Comma = ',',
        Semicolon = ';',
        EndOfStmt = Semicolon,
        BracketL = '[',
        BracketR = ']',
        Grave = '`',
        BraceL = '{',
        BraceR = '}',

        /* Literals */
        FirstLiteral = 128,
        Integer = FirstLiteral,
        Float,
        String,
        Char,
        LastLiteral = Char,

        /* Identifiers */
        VarID,
        ConID,
        VarSym,
        ConSym,

        /* Keywords */
        kwAlias,
        kwClass,
        kwData,
        kwDefault,
        kwDeriving,
        kwDo,
        kwElse,
        kwFor,
        kwForeign,
        kwFn,
        kwIf,
        kwImport,
        kwIn,
        kwInfix,
        kwInfixL,
        kwInfixR,
        kwPrefix,
        kwInstance,
        kwLet,
        kwMatch,
        kwModule,
        kwNewType,
        kwReturn,
        kwThen,
        kwWhere,
        kwWhile,
        kw_,

        /* Reserved operators */
        opDot,
        opDotDot,
        opColon,
        opColonColon,
        opEquals,
        opBackSlash,
        opBar,
        opArrowL, // <-
        opArrowR, // ->
        opAt,
        opDollar,
        opTilde,
        opArrowD, // =>
    };

    // The token position including any whitespace preceding it.
    U32 whitespaceLine = 0;
    U32 whitespaceColumn = 0;
    U32 whitespaceOffset = 0;

    // The starting position of the actual token data.
    U32 startLine = 0;
    U32 startColumn = 0;
    U32 startOffset = 0;

    // The end position of the actual token data.
    U32 endLine = 0;
    U32 endColumn = 0;
    U32 endOffset = 0;

    Type type;

    union {
        I64 integer;
        double floating;
        U32 character;
        Id id;
    } data;

    // Special case for VarSym, used to find unary minus more easily.
    // Undefined value if the type is not VarSym.
    bool singleMinus = false;
};

struct Lexer {
    Lexer(Context& context, Diagnostics& diag, const char* text, Token* tok);

    Token* next();
private:

    void skipWhitespace();
    bool handleWhitespace();
    U32 nextCodePoint();
    void nextLine();

    Id parseStringLiteral();
    U32 parseCharLiteral();
    U32 parseEscapedLiteral();
    void parseNumericLiteral();

    void parseSymbol();
    void parseSpecial();
    void parseQualifier();
    void parseVariable();

    void parseToken();

    void startLocation();
    void startWhitespace();
    void endLocation();

    friend struct SaveLexer;
    friend struct IndentLevel;

    static const U32 kTabWidth = 4;
    static const char kFormatStart = '{';
    static const char kFormatEnd = '}';

    Token* token; // The token currently being parsed.
    Diagnostics& diag;
    Context& context;
    Qualified qualifier; // The current qualified name being built up.
    const char* text; // The full source code.
    const char* p; // The current source pointer.
    const char* l; // The first character of the current line.
    U32 blockCount = 0; // The current number of indentation blocks.
    U32 indentation = 0; // The current indentation level.
    U32 line = 0; // The current source line.
    U32 tabs = 0; // The number of tabs processed on the current line.
    U32 formatting = 0; // Indicates that we are currently inside a formatting string literal.
    bool newItem = false; // Indicates that a new item was started by the previous token.
};

struct IndentLevel {
    IndentLevel(Token& start, Lexer& lexer) : lexer(lexer), previous(lexer.indentation) {
        lexer.indentation = start.startColumn;
        lexer.blockCount++;
    }

    void end() {
        lexer.indentation = previous;
        assert(lexer.blockCount > 0);
        lexer.blockCount--;
    }

    Lexer& lexer;
    const U32 previous;
};

struct SaveLexer {
    SaveLexer(Lexer& lexer) :
        lexer(lexer),
        p(lexer.p),
        l(lexer.l),
        line(lexer.line),
        indent(lexer.indentation),
        blocks(lexer.blockCount),
        tabs(lexer.tabs),
        formatting(lexer.formatting),
        newItem(lexer.newItem) {}

    void restore() {
        lexer.p = p;
        lexer.l = l;
        lexer.line = line;
        lexer.indentation = indent;
        lexer.newItem = newItem;
        lexer.tabs = tabs;
        lexer.blockCount = blocks;
        lexer.formatting = formatting;
    }

    Lexer& lexer;
    const char* p;
    const char* l;
    U32 line;
    U32 indent;
    U32 blocks;
    U32 tabs;
    U32 formatting;
    bool newItem;
};
