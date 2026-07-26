#pragma once

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
        kwAtData,
        kwBreak,
        kwClass,
        kwContinue,
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
        kwInfixL,
        kwInfixR,
        kwPrefixR,
        kwSuffixL,
        kwInstance,
        kwIter,
        kwLens,
        kwLet,
        kwMatch,
        kwModule,
        kwNewType,
        kwPub,
        kwReturn,
        kwThen,
        kwWhere,
        kwWhile,
        kwYield,
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
        opAmp,
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

    union Payload {
        I64 integer;
        double floating;
        U32 character;
        StringId id;
    } data;

    // Special case for VarSym, used to find unary minus more easily.
    // Undefined value if the type is not VarSym.
    bool singleMinus = false;
};

struct Lexer {
    Lexer(Context& context, Diagnostics& diag, const StringView& text, StringId moduleName);
    void next(Token& token);

private:
    // A position in the source, remembered so that a diagnostic can point at where a construct
    // started rather than at where the lexer noticed that it was broken.
    struct Position {
        U32 line;
        U32 column;
        U32 offset;
    };

    Position position() const;

    // The source range from a remembered position up to the lexer's current position.
    Location locationFrom(const Position& start) const;

    void skipWhitespace();
    bool handleWhitespace();
    void nextLine();

    StringId parseStringLiteral();

    void parseSymbol(Token& token, const char** start, U32* length, bool allowKeywords);
    void parseVariable(Token& token, const char** start, U32* length);
    void parseSpecial(Token& token);
    void parseQualifier(Token& token);

    void startLocation(Token& token);
    void startWhitespace(Token& token);
    void endLocation(Token& token);

    friend struct SaveLexer;
    friend struct IndentLevel;

    static constexpr U32 kTabWidth = 4;
    static constexpr char kFormatStart = '{';
    static constexpr char kFormatEnd = '}';

    Diagnostics& diag;
    Context& context;

    StringId moduleName; // The module this source belongs to, for the locations of diagnostics.
    const char* text; // The full source code.
    const char* p; // The current source pointer.
    const char* l; // The first character of the current line.
    const char* m; // The source end.
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
        assertTrue(lexer.blockCount > 0);
        lexer.blockCount--;
    }

    Lexer& lexer;
    const U32 previous;
};

struct SaveLexer {
    explicit SaveLexer(Lexer& lexer) :
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
