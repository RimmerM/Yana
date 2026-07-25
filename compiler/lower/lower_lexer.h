#pragma once

#include "../compiler/diagnostics.h"
#include "../compiler/context.h"

struct LowerToken {
    enum Type {
        EndOfFile,

        /* Special symbols */
        ParenL = '(',
        ParenR = ')',
        Comma = ',',
        Semicolon = ';',
        EndOfStmt = Semicolon,
        BracketL = '[',
        BracketR = ']',
        BraceL = '{',
        BraceR = '}',
        Equals = '=',
        Colon = ':',
        Minus = '-',

        /* Literals */
        FirstLiteral = 128,
        Int = FirstLiteral,
        Long,
        Float,
        Double,
        String,
        LastLiteral = String,

        /* Identifiers */
        RegID,
        GlobalID,
        LabelID,
    };

    union Payload {
        U64 integer;
        F64 floating;
        StringId id;
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
    Payload data;
};

struct LowerLexer {
    LowerLexer(Context& context, Diagnostics& diag, const StringView& text);
    void next(LowerToken&);

private:
    void skipWhitespace();
    bool handleWhitespace();
    void nextLine();

    StringId parseStringLiteral();

    void startLocation(LowerToken& token);
    void startWhitespace(LowerToken& token);
    void endLocation(LowerToken& token);

    Diagnostics& diag;
    Context& context;
    const char* text; // The full source code.
    const char* p; // The current source pointer.
    const char* l; // The first character of the current line.
    const char* m; // The source end.
    U32 line = 0; // The current source line.
};
