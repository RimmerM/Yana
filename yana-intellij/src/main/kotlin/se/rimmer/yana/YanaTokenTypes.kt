package se.rimmer.yana

import com.intellij.psi.tree.IElementType
import com.intellij.psi.tree.TokenSet
import org.jetbrains.annotations.NonNls

class YanaTokenType(@NonNls debugName: String) : IElementType(debugName, YanaLanguage) {
    override fun toString() = "Yana:" + super.toString()
}

/*
 * The lexical token classes, which are not the compiler's `Token::Type`.
 *
 * The compiler distinguishes every keyword from every other because the parser has to; a
 * highlighter does not, and one token class per colour is what the rest of the plugin is written
 * against. The list of *which words are keywords* is the part that must not drift, and that comes
 * from `compiler/parse/tokens.def` through the generated `YanaKeywords` - §9.
 *
 * The layout tokens the compiler's lexer inserts - `EndOfStmt`, `EndOfBlock` - have no text and
 * therefore cannot exist here: an IntelliJ lexer partitions the file, so every token is a range of
 * characters and nothing may be invented between them.
 */
object YanaTokenTypes {
    @JvmField val KEYWORD = YanaTokenType("KEYWORD")
    @JvmField val RESERVED_OP = YanaTokenType("RESERVED_OP")

    /*
     * `?` - the early-exit suffix, apart from the reserved operators it is one of.
     *
     * The one reserved operator that is control flow rather than notation. `x?` may return from the
     * enclosing function, which is the same thing a `return` written out does and nothing any other
     * operator does - and unlike a `return` it is one character wide at the end of a long line. A
     * class of its own is what lets it be coloured as the departure it is.
     */
    @JvmField val TRY = YanaTokenType("TRY")

    /*
     * `?.` - optional chaining, which is a *different* operator from `?` and not a spelling of it.
     *
     * Its own class rather than sharing `TRY`, because the difference between them is the one thing
     * a reader has to see: `?` leaves the enclosing function and `?.` skips to the end of the chain.
     * Two operators that look alike and do different things are exactly the pair worth colouring
     * apart.
     */
    @JvmField val OPTIONAL_CHAIN = YanaTokenType("OPTIONAL_CHAIN")

    @JvmField val VAR_ID = YanaTokenType("VAR_ID")
    @JvmField val CON_ID = YanaTokenType("CON_ID")
    @JvmField val VAR_SYM = YanaTokenType("VAR_SYM")
    @JvmField val CON_SYM = YanaTokenType("CON_SYM")

    @JvmField val INTEGER = YanaTokenType("INTEGER")
    @JvmField val FLOAT = YanaTokenType("FLOAT")
    @JvmField val STRING = YanaTokenType("STRING")
    @JvmField val STRING_ESCAPE = YanaTokenType("STRING_ESCAPE")
    @JvmField val CHAR = YanaTokenType("CHAR")

    @JvmField val LINE_COMMENT = YanaTokenType("LINE_COMMENT")
    @JvmField val BLOCK_COMMENT = YanaTokenType("BLOCK_COMMENT")

    @JvmField val PAREN_L = YanaTokenType("PAREN_L")
    @JvmField val PAREN_R = YanaTokenType("PAREN_R")
    @JvmField val BRACKET_L = YanaTokenType("BRACKET_L")
    @JvmField val BRACKET_R = YanaTokenType("BRACKET_R")
    @JvmField val BRACE_L = YanaTokenType("BRACE_L")
    @JvmField val BRACE_R = YanaTokenType("BRACE_R")
    @JvmField val COMMA = YanaTokenType("COMMA")
    @JvmField val SEMICOLON = YanaTokenType("SEMICOLON")
    @JvmField val GRAVE = YanaTokenType("GRAVE")

    @JvmField val COMMENTS: TokenSet = TokenSet.create(LINE_COMMENT, BLOCK_COMMENT)
    @JvmField val STRINGS: TokenSet = TokenSet.create(STRING, STRING_ESCAPE, CHAR)
}
