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

        /*
         * Keywords and reserved operators.
         *
         * Built from parse/tokens.def, which the JetBrains plugin's lexer is also built from -
         * see Implementation-Tooling.md §9. Adding a keyword here alone would leave the editor
         * colouring it as an identifier, which is exactly the drift one list prevents.
         */
#define YANA_KEYWORD(name, text) name,
#define YANA_RESERVED_OP(name, text) name,
#include "tokens.def"
    };

    // The token position including any whitespace preceding it.
    U32 whitespaceLine = 0;
    U32 whitespaceColumn = 0;
    U32 whitespaceOffset = 0;

    // The starting position of the actual token data.
    U32 startLine = 0;
    U32 startColumn = 0;
    U32 startOffset = 0;

    // How many brackets were open when this token was reached, before the token itself opened or
    // closed one. What a layout block opened here records - see Lexer::brackets.
    U32 startBrackets = 0;

    // Whether an open bracket is the only reason this is not a layout token. What tells a construct
    // whose delimiter was never closed that it has already reached the end of its statement, which
    // is a thing the column alone used to say - see Lexer::brackets.
    bool suppressedLayout = false;

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

    /*
     * Give up on every bracket opened inside the current layout block, and re-read the token in hand
     * as though the lexer had never reached it.
     *
     * The parser's, and only on the paths where it has already decided a delimiter will never be
     * closed. A bracket that stays open suppresses layout to the end of the file, which would make
     * one unclosed `(` cost every declaration after it rather than the one it is in - the opposite
     * of what skipToClose's rule says, and what Recover.Args exists to pin.
     *
     * The re-read is what makes the recovery *identical* to the one the column rule used to give,
     * rather than merely bounded. The token in hand was read while layout was suppressed, so the
     * EndOfStmt that belonged in front of it was never emitted; lifting the suppression alone leaves
     * the parser looking at a token with no statement boundary before it, and skipToClose would eat
     * past the boundary looking for one. Reading it again with the brackets gone produces the layout
     * token first, and everything downstream sees what it always saw.
     *
     * Answers whether anything was abandoned, so that a caller can tell a construct that ran out of
     * statement from one that merely ended somewhere unexpected.
     */
    bool abandonBrackets(Token& token) {
        if(brackets <= blockBrackets) return false;

        brackets = blockBrackets;

        p = rewind.p;
        l = rewind.l;
        line = rewind.line;
        tabs = rewind.tabs;
        formatting = rewind.formatting;
        newItem = false;

        next(token);
        return true;
    }

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

    /*
     * Layout inside brackets - Analysis-Language.md §6.
     *
     * Layout is column-only, and with no notion of a bracket the one shape that fails is the
     * conventional one: a delimiter closed back at the enclosing statement's own column is at
     * `indentation`, so an EndOfStmt used to be inserted in the middle of an argument list.
     *
     * A depth rather than a flag, and a per-block depth rather than a bare counter, because layout
     * still has to work *inside* a bracket: a lambda body or a `match` written as a call argument
     * opens its own block, and that block's statements are separated at their own column while the
     * argument list around them is not. So a layout token is emitted only where the bracket depth is
     * the one its enclosing block was opened at, which is what `blockBrackets` records.
     */
    U32 brackets = 0; // How many brackets are open at the current position.
    U32 blockBrackets = 0; // The depth the innermost layout block was opened at.

    // Where the token currently in hand began, so that abandonBrackets can read it again. Held here
    // rather than on the Token because it is only ever used for the token the lexer just produced,
    // and a Token is written once per token in the hottest loop the parser has.
    struct Rewind {
        const char* p = nullptr;
        const char* l = nullptr;
        U32 line = 0;
        U32 tabs = 0;
        U32 formatting = 0;
    } rewind;
    U32 line = 0; // The current source line.
    U32 tabs = 0; // The number of tabs processed on the current line.
    U32 formatting = 0; // Indicates that we are currently inside a formatting string literal.
    bool newItem = false; // Indicates that a new item was started by the previous token.
};

struct IndentLevel {
    IndentLevel(Token& start, Lexer& lexer) :
        lexer(lexer), previous(lexer.indentation), previousBrackets(lexer.blockBrackets) {
        lexer.indentation = start.startColumn;
        lexer.blockBrackets = start.startBrackets;
        lexer.blockCount++;
    }

    void end() {
        lexer.indentation = previous;
        lexer.blockBrackets = previousBrackets;
        assertTrue(lexer.blockCount > 0);
        lexer.blockCount--;
    }

    Lexer& lexer;
    const U32 previous;
    const U32 previousBrackets;
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
        brackets(lexer.brackets),
        blockBrackets(lexer.blockBrackets),
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
        lexer.brackets = brackets;
        lexer.blockBrackets = blockBrackets;
    }

    Lexer& lexer;
    const char* p;
    const char* l;
    U32 line;
    U32 indent;
    U32 blocks;
    U32 tabs;
    U32 formatting;

    // A look-ahead that opened or closed a bracket has to be undone with everything else, or the
    // suppression outlives the tokens that justified it.
    U32 brackets;
    U32 blockBrackets;

    bool newItem;
};
