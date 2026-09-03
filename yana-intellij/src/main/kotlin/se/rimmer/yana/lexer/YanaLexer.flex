package se.rimmer.yana.lexer;

import com.intellij.lexer.FlexLexer;
import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;
import se.rimmer.yana.YanaTokenTypes;

/*
 * The lexical half of the plugin - Implementation-Tooling.md §9, §11.
 *
 * This is deliberately not the compiler's lexer translated. It cannot be: an IntelliJ lexer
 * partitions the file into ranges of characters, and the compiler's lexer inserts layout tokens
 * (`EndOfStmt`, `EndOfBlock`) that have no text. What is shared is the thing that would otherwise
 * drift - the list of keywords and reserved operators, which comes from
 * `compiler/parse/tokens.def` through the generated `YanaKeywords`.
 *
 * The character classes below are the ones `compiler/util/lexer_util.cpp`'s `isIdentifier` and
 * `isSymbol` define, plus the special characters of `compiler/parse/lexer.cpp`'s `isSpecial`. They
 * are small and they are written out here rather than generated, because a table of 94 booleans is
 * not a list anybody edits by hand.
 */
%%

%public
%class YanaFlexLexer
%implements FlexLexer
%function advance
%type IElementType
%unicode

%{
  // How deep the nested block comment currently being scanned is. Yana's `{- -}` nest, so no
  // regular expression describes them and a counter is what closes the right one.
  private int commentDepth = 0;

  private IElementType identifierOrKeyword() {
    return YanaKeywords.KEYWORDS.contains(yytext().toString())
      ? YanaTokenTypes.KEYWORD
      : YanaTokenTypes.VAR_ID;
  }

  // A symbol run is one lexeme. If it spells a reserved operator exactly it is that operator, and
  // otherwise it is a user-defined one - a constructor operator when it begins with a colon. This
  // is the same rule `Lexer::parseSymbol` applies, expressed as a lookup rather than as a tree of
  // length checks.
  private IElementType symbolRun() {
    String text = yytext().toString();

    // The two `?` operators, each its own class - see YanaTokenTypes. `?` leaves the enclosing
    // function and `?.` skips to the end of the chain, which is the difference worth seeing. A run
    // that is neither of those exactly - `??`, `?:` - is an ordinary operator here as it is in the
    // compiler's lexer.
    if(text.equals("?")) return YanaTokenTypes.TRY;
    if(text.equals("?.")) return YanaTokenTypes.OPTIONAL_CHAIN;

    if(YanaKeywords.RESERVED_OPERATORS.contains(text)) return YanaTokenTypes.RESERVED_OP;
    return text.charAt(0) == ':' ? YanaTokenTypes.CON_SYM : YanaTokenTypes.VAR_SYM;
  }
%}

%state BLOCK_COMMENT
%state STRING

WHITE_SPACE   = [ \t\f\r\n]+

IDENT_PART    = [a-zA-Z0-9_]
VAR_ID        = [a-z_] {IDENT_PART}*
CON_ID        = [A-Z] {IDENT_PART}*

// `'` is deliberately absent, exactly as it is from the compiler's `isSymbol`: it is the origin
// marker, its own token and never part of a run, which is what lets `%'a` and `&'a` be written.
SYMBOL        = [!#$%&*+\-./:<=>?@\\\^|~]
NOT_SYMBOL    = [^!#$%&*+\-./:<=>?@\\\^|~]

// `--` opens a comment only when what follows is not another symbol, so `-->` is an operator and
// not an empty comment. The longest-match rule does the rest: for `-- text` the comment reaches the
// end of the line and wins, and for `-->` only the symbol run matches at all.
LINE_COMMENT  = "--" ({NOT_SYMBOL} [^\r\n]*)?

DIGIT         = [0-9]
DECIMAL       = {DIGIT}+
HEX           = 0 [xX] [0-9a-fA-F]+
OCTAL         = 0 [oO] [0-7]+
BINARY        = 0 [bB] [01]+
EXPONENT      = [eE] [+\-]? {DIGIT}+
FLOAT         = {DIGIT}+ ("." {DIGIT}+ {EXPONENT}? | {EXPONENT})


%%

<YYINITIAL> {
  {WHITE_SPACE}       { return TokenType.WHITE_SPACE; }
  {LINE_COMMENT}      { return YanaTokenTypes.LINE_COMMENT; }

  // `{->` is a brace followed by an arrow rather than a comment opener, which is why the lookahead
  // is here and why the `{` rule below is what handles that case.
  //
  // The opener returns a token of its own. An action that only changes state still consumes what it
  // matched - JFlex advances the token start on every match, not on every return - so leaving this
  // one to be swallowed by the next token would leave the two characters covered by nothing, and a
  // lexer that does not partition the file is one the platform will not accept.
  "{-" / {NOT_SYMBOL} { commentDepth = 1; yybegin(BLOCK_COMMENT); return YanaTokenTypes.BLOCK_COMMENT; }

  {FLOAT}             { return YanaTokenTypes.FLOAT; }
  {HEX}               { return YanaTokenTypes.INTEGER; }
  {OCTAL}             { return YanaTokenTypes.INTEGER; }
  {BINARY}            { return YanaTokenTypes.INTEGER; }
  {DECIMAL}           { return YanaTokenTypes.INTEGER; }

  \"                  { yybegin(STRING); return YanaTokenTypes.STRING; }

  // `@data` is a keyword whose first character is a symbol, so it is matched here and looked up in
  // the same table as every other keyword. Anything else beginning with `@` is the reserved `@`
  // operator followed by a name, which is what an attribute is.
  "@" {IDENT_PART}+   { if(YanaKeywords.KEYWORDS.contains(yytext().toString())) return YanaTokenTypes.KEYWORD;
                        yypushback(yylength() - 1);
                        return YanaTokenTypes.RESERVED_OP; }

  // An identifier immediately in front of the marker is the origin's *label*, and is coloured as
  // part of the marker rather than as a variable - see YanaTokenTypes.ORIGIN_LABEL. Ahead of the
  // plain identifier rule because the two match the same text and JFlex breaks that tie by order;
  // a keyword still wins, which is the compiler's rule too.
  //
  // Two tokens where the compiler's lexer produces one - its `Token::OriginLabel` fuses the pair,
  // since the parser wants one thing to test. Kept apart here because a highlighter wants the
  // opposite: the label and the marker are separately themable, and every character of both is
  // covered either way. What the compiler also requires and this cannot see is that the name be
  // *unqualified*; `A.thing'` is three tokens here and the label colour lands on `thing`, which is
  // a wrong colour on code the compiler rejects rather than a disagreement about valid code.
  {VAR_ID} / "'"      { return YanaKeywords.KEYWORDS.contains(yytext().toString())
                          ? YanaTokenTypes.KEYWORD
                          : YanaTokenTypes.ORIGIN_LABEL; }
  "'"                 { return YanaTokenTypes.ORIGIN; }

  {VAR_ID}            { return identifierOrKeyword(); }
  {CON_ID}            { return YanaTokenTypes.CON_ID; }
  {SYMBOL}+           { return symbolRun(); }

  "("                 { return YanaTokenTypes.PAREN_L; }
  ")"                 { return YanaTokenTypes.PAREN_R; }
  "["                 { return YanaTokenTypes.BRACKET_L; }
  "]"                 { return YanaTokenTypes.BRACKET_R; }
  "{"                 { return YanaTokenTypes.BRACE_L; }
  "}"                 { return YanaTokenTypes.BRACE_R; }
  ","                 { return YanaTokenTypes.COMMA; }
  ";"                 { return YanaTokenTypes.SEMICOLON; }
  "`"                 { return YanaTokenTypes.GRAVE; }

  [^]                 { return TokenType.BAD_CHARACTER; }
}

/*
 * Block comments, which nest.
 *
 * Several adjacent tokens rather than one, deliberately. A single token would mean scanning to the
 * closing `-}` without returning, and an unterminated comment would then reach the end of the file
 * with text consumed and no token to account for it - which is the one thing an IntelliJ lexer may
 * not do. Returning a token per run leaves nothing pending at any point, and adjacent tokens of one
 * type are indistinguishable once they are coloured.
 *
 * `commentDepth` is assigned rather than incremented at the opener above, which is what makes it
 * correct after an incremental relex: the platform restarts scanning only where the state is the
 * initial one, and every entry into this state therefore begins at depth one.
 */
<BLOCK_COMMENT> {
  "{-" / {NOT_SYMBOL} { commentDepth++; return YanaTokenTypes.BLOCK_COMMENT; }
  "-}"                { commentDepth--;
                        if(commentDepth == 0) yybegin(YYINITIAL);
                        return YanaTokenTypes.BLOCK_COMMENT; }
  [^{\-]+             { return YanaTokenTypes.BLOCK_COMMENT; }
  [^]                 { return YanaTokenTypes.BLOCK_COMMENT; }
}

/*
 * Strings.
 *
 * The opening quote is part of the string token and so is the closing one; an escape is its own
 * token so that it can be coloured differently, which is the one thing inside a literal worth
 * seeing at a glance.
 *
 * A `{expr}` interpolation is left as string text. The compiler's lexer leaves the literal and
 * returns to it, which an IntelliJ lexer could do with another state - but until the server
 * highlights the expression inside it semantically, colouring it as code would only make it look
 * like something the editor understands.
 */
<STRING> {
  \\ [^\r\n]          { return YanaTokenTypes.STRING_ESCAPE; }
  \"                  { yybegin(YYINITIAL); return YanaTokenTypes.STRING; }
  // A line ends the literal whether or not it was closed. The newline itself is whitespace rather
  // than a bad character: the missing quote is worth a diagnostic, which the server publishes, and
  // not worth colouring a line break red.
  [\r\n]              { yybegin(YYINITIAL); return TokenType.WHITE_SPACE; }
  [^\\\"\r\n]+        { return YanaTokenTypes.STRING; }
}
