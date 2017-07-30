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
        EndOfStmt = Comma,
        Semicolon = ';',
        BracketL = '[',
        BracketR = ']',
        Grave = '`',
        BraceL = '{',
        BraceR = '}',

        /* Literals */
        Integer = 128,
        Float,
        String,
        Char,

        /* Identifiers */
        VarID,
        ConID,
        VarSym,
        ConSym,

        /* Keywords */
        kwCase,
        kwClass,
        kwData,
        kwDefault,
        kwDeriving,
        kwDo,
        kwElse,
        kwFor,
        kwForeign,
        kwIf,
        kwImport,
        kwIn,
        kwInfix,
        kwInfixL,
        kwInfixR,
        kwPrefix,
        kwInstance,
        kwLet,
        kwModule,
        kwNewType,
        kwOf,
        kwThen,
        kwType,
        kwVar,
        kwWhere,
        kwWhile,
        kw_,

        /* Reserved operators */
        opDot,
        opDotDot,
        opColon,
        opColonColon,
        opEquals,
        opBackSlash, // also λ
        opBar,
        opArrowL, // <- and ←
        opArrowR, // -> and →
        opAt,
        opDollar,
        opTilde,
        opArrowD,
    };

    U32 sourceLine;
    U32 sourceColumn;
    U32 length;
    Type type;

    union {
        uint64_t integer;
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
        lexer.indentation = start.sourceColumn;
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
