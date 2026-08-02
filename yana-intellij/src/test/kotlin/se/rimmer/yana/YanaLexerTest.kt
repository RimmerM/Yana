package se.rimmer.yana

import com.intellij.lexer.Lexer
import com.intellij.psi.TokenType
import com.intellij.psi.tree.IElementType
import com.intellij.testFramework.LexerTestCase
import se.rimmer.yana.lexer.YanaLexerAdapter

/*
 * The lexer is a pure function of text, so it is the one part of this plugin worth testing on its
 * own - Implementation-Tooling.md's testing strategy. What is asserted is the cases that are easy
 * to get wrong and silent when they are: `-->` is an operator and not an empty comment, `{->` is a
 * brace and not a comment opener, nested `{- -}` close in the right order, and `@data` is a keyword
 * while `@inline` is the `@` operator followed by a name.
 */
class YanaLexerTest : LexerTestCase() {
    override fun createLexer(): Lexer = YanaLexerAdapter()
    override fun getDirPath() = ""

    private fun scan(text: String): List<Pair<IElementType, String>> {
        val lexer = createLexer()
        lexer.start(text)

        val tokens = mutableListOf<Pair<IElementType, String>>()
        while (true) {
            val type = lexer.tokenType ?: break
            if (type != TokenType.WHITE_SPACE) {
                tokens += type to text.substring(lexer.tokenStart, lexer.tokenEnd)
            }
            lexer.advance()
        }

        return tokens
    }

    private fun assertTokens(text: String, vararg expected: Pair<IElementType, String>) {
        assertEquals(expected.toList(), scan(text))
    }

    fun testKeywordsAndIdentifiers() = assertTokens(
        "fn total(xs) = xs",
        YanaTokenTypes.KEYWORD to "fn",
        YanaTokenTypes.VAR_ID to "total",
        YanaTokenTypes.PAREN_L to "(",
        YanaTokenTypes.VAR_ID to "xs",
        YanaTokenTypes.PAREN_R to ")",
        YanaTokenTypes.RESERVED_OP to "=",
        YanaTokenTypes.VAR_ID to "xs",
    )

    fun testConstructorsAreTheirOwnClass() = assertTokens(
        "Just(x) :+: y",
        YanaTokenTypes.CON_ID to "Just",
        YanaTokenTypes.PAREN_L to "(",
        YanaTokenTypes.VAR_ID to "x",
        YanaTokenTypes.PAREN_R to ")",
        YanaTokenTypes.CON_SYM to ":+:",
        YanaTokenTypes.VAR_ID to "y",
    )

    fun testReservedOperatorsAreWholeLexemes() = assertTokens(
        "a ->b .. c ..= d",
        YanaTokenTypes.VAR_ID to "a",
        YanaTokenTypes.RESERVED_OP to "->",
        YanaTokenTypes.VAR_ID to "b",
        YanaTokenTypes.RESERVED_OP to "..",
        YanaTokenTypes.VAR_ID to "c",
        YanaTokenTypes.RESERVED_OP to "..=",
        YanaTokenTypes.VAR_ID to "d",
    )

    // `-->` is a symbol run, so it is an operator someone may have defined - not a comment that
    // swallows the rest of the line, which is what a naive `--` rule would make it.
    fun testArrowIsNotAComment() = assertTokens(
        "a --> b",
        YanaTokenTypes.VAR_ID to "a",
        YanaTokenTypes.VAR_SYM to "-->",
        YanaTokenTypes.VAR_ID to "b",
    )

    fun testLineComment() = assertTokens(
        "a -- and the rest",
        YanaTokenTypes.VAR_ID to "a",
        YanaTokenTypes.LINE_COMMENT to "-- and the rest",
    )

    // The comment is several adjacent tokens by design - see YanaLexer.flex - so this asserts that
    // every one of them is a comment and that the run ends at the outermost `-}`.
    fun testNestedBlockComment() {
        val tokens = scan("{- a {- b -} c -} d")
        assertTrue(tokens.dropLast(1).all { it.first == YanaTokenTypes.BLOCK_COMMENT })
        assertEquals(YanaTokenTypes.VAR_ID to "d", tokens.last())
        assertEquals("{- a {- b -} c -}", tokens.dropLast(1).joinToString("") { it.second })
    }

    fun testBraceArrowIsNotACommentOpener() = assertTokens(
        "{->x}",
        YanaTokenTypes.BRACE_L to "{",
        YanaTokenTypes.RESERVED_OP to "->",
        YanaTokenTypes.VAR_ID to "x",
        YanaTokenTypes.BRACE_R to "}",
    )

    fun testAtDataIsAKeywordAndAttributesAreNot() = assertTokens(
        "@data @inline",
        YanaTokenTypes.KEYWORD to "@data",
        YanaTokenTypes.RESERVED_OP to "@",
        YanaTokenTypes.VAR_ID to "inline",
    )

    fun testNumbers() = assertTokens(
        "0xff 0b1010 0o17 42 1.5e3",
        YanaTokenTypes.INTEGER to "0xff",
        YanaTokenTypes.INTEGER to "0b1010",
        YanaTokenTypes.INTEGER to "0o17",
        YanaTokenTypes.INTEGER to "42",
        YanaTokenTypes.FLOAT to "1.5e3",
    )

    // A line ends a literal whether or not it was closed, and the line break is whitespace: the
    // missing quote is worth a diagnostic, which the server publishes, and not worth colouring a
    // line break red.
    fun testUnterminatedStringEndsAtTheLine() = assertTokens(
        "x = \"open\ny = 1",
        YanaTokenTypes.VAR_ID to "x",
        YanaTokenTypes.RESERVED_OP to "=",
        YanaTokenTypes.STRING to "\"",
        YanaTokenTypes.STRING to "open",
        YanaTokenTypes.VAR_ID to "y",
        YanaTokenTypes.RESERVED_OP to "=",
        YanaTokenTypes.INTEGER to "1",
    )

    /*
     * The invariant the platform requires: the tokens partition the file, with no gap, no overlap
     * and no zero-length token.
     *
     * This is the test that earns its place. The first version of the block comment rules opened
     * the comment with an action that changed state and returned nothing - and JFlex advances the
     * token start on every match rather than on every return, so the `{-` was consumed and covered
     * by no token at all. Every case above still passed; only this one failed.
     */
    fun testTokensPartitionTheFile() {
        val cases = listOf(
            "fn total(xs) = xs",
            "{- a {- b -} c -} d",
            "x {- never closed",
            "x = \"open\ny = 1",
            "x = \"open",
            "a --",
            "a --> b",
            "{->x}",
            "\"h\u00e5ll \uD83D\uDE42\" -- \u00e5",
            "a \u00a7 b",
            "",
            "   \n\t ",
        )

        for (text in cases) {
            val lexer = createLexer()
            lexer.start(text)

            var at = 0
            while (lexer.tokenType != null) {
                assertEquals("gap or overlap in <$text>", at, lexer.tokenStart)
                assertTrue("zero-length token in <$text>", lexer.tokenEnd > lexer.tokenStart)
                at = lexer.tokenEnd
                lexer.advance()
            }

            assertEquals("text not covered in <$text>", text.length, at)
        }
    }

    // The quotes and the text between the escapes are separate string tokens, because the opening
    // quote is what switches the lexer into the string state and a rule that returns cannot also
    // keep scanning. They are adjacent and identically coloured, so nothing downstream can tell.
    fun testStringEscapesAreTheirOwnToken() = assertTokens(
        "\"a\\nb\"",
        YanaTokenTypes.STRING to "\"",
        YanaTokenTypes.STRING to "a",
        YanaTokenTypes.STRING_ESCAPE to "\\n",
        YanaTokenTypes.STRING to "b",
        YanaTokenTypes.STRING to "\"",
    )
}
