package se.rimmer.yana.highlight

import com.intellij.lexer.Lexer
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.fileTypes.SyntaxHighlighter
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase
import com.intellij.openapi.fileTypes.SyntaxHighlighterFactory
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.psi.TokenType
import com.intellij.psi.tree.IElementType
import se.rimmer.yana.YanaTokenTypes
import se.rimmer.yana.lexer.YanaLexerAdapter

/*
 * The lexical layer of colouring - Implementation-Tooling.md §11.
 *
 * Every key falls back to a platform default, which is what makes the colours right in a theme
 * nobody has configured this plugin for: `DefaultLanguageHighlighterColors` is the set every theme
 * defines, and a key with one of them as its fallback inherits whatever that theme chose.
 *
 * Semantic tokens from the server refine this later - a `VarID` coloured as an identifier here
 * becomes a function call, a field or a borrowed local once the server answers. What is here has to
 * be right on its own, because it is what is shown while the server is starting, compiling, or dead.
 */
object YanaColors {
    val KEYWORD = key("YANA_KEYWORD", DefaultLanguageHighlighterColors.KEYWORD)
    val RESERVED_OP = key("YANA_RESERVED_OP", DefaultLanguageHighlighterColors.KEYWORD)

    /*
     * `?`, falling back to the keyword colour rather than to the operator one.
     *
     * `x?` is a conditional `return`, and a reader scanning a function for the places it can be
     * left has to find it. Arithmetic-coloured, one character at the end of a call is exactly what
     * the eye skips. The fallback is `KEYWORD` because that is what the departure it is closest to
     * - `return` - is painted with, and a scheme that has never heard of this plugin then gets the
     * right answer without configuring anything.
     */
    val TRY = key("YANA_TRY", DefaultLanguageHighlighterColors.KEYWORD)

    /*
     * `?.`, which falls back to the *operator* colour rather than the keyword one.
     *
     * Deliberately not the same as `TRY`. `?.` does not leave the function - it produces a value
     * like any other operator does - so painting it as control flow would say the opposite of what
     * it means. The two being different colours out of the box is the point: they look alike in
     * text and are the pair most worth telling apart at a glance.
     */
    val OPTIONAL_CHAIN = key("YANA_OPTIONAL_CHAIN", DefaultLanguageHighlighterColors.OPERATION_SIGN)

    // A `VarID` and a `ConID` are lexically different things in Yana and mean different things -
    // `Just` and `maybe` are not interchangeable anywhere - so they are separable from the start.
    val VAR_ID = key("YANA_VAR_ID", DefaultLanguageHighlighterColors.IDENTIFIER)
    val CON_ID = key("YANA_CON_ID", DefaultLanguageHighlighterColors.CLASS_NAME)
    val VAR_SYM = key("YANA_VAR_SYM", DefaultLanguageHighlighterColors.OPERATION_SIGN)
    val CON_SYM = key("YANA_CON_SYM", DefaultLanguageHighlighterColors.CLASS_NAME)

    val NUMBER = key("YANA_NUMBER", DefaultLanguageHighlighterColors.NUMBER)
    val STRING = key("YANA_STRING", DefaultLanguageHighlighterColors.STRING)
    val STRING_ESCAPE = key("YANA_STRING_ESCAPE", DefaultLanguageHighlighterColors.VALID_STRING_ESCAPE)

    val LINE_COMMENT = key("YANA_LINE_COMMENT", DefaultLanguageHighlighterColors.LINE_COMMENT)
    val BLOCK_COMMENT = key("YANA_BLOCK_COMMENT", DefaultLanguageHighlighterColors.BLOCK_COMMENT)

    val PARENTHESES = key("YANA_PARENTHESES", DefaultLanguageHighlighterColors.PARENTHESES)
    val BRACKETS = key("YANA_BRACKETS", DefaultLanguageHighlighterColors.BRACKETS)
    val BRACES = key("YANA_BRACES", DefaultLanguageHighlighterColors.BRACES)
    val COMMA = key("YANA_COMMA", DefaultLanguageHighlighterColors.COMMA)
    val SEMICOLON = key("YANA_SEMICOLON", DefaultLanguageHighlighterColors.SEMICOLON)
    val GRAVE = key("YANA_GRAVE", DefaultLanguageHighlighterColors.OPERATION_SIGN)

    val BAD_CHARACTER = key("YANA_BAD_CHARACTER", com.intellij.openapi.editor.HighlighterColors.BAD_CHARACTER)

    private fun key(name: String, fallback: TextAttributesKey) =
        TextAttributesKey.createTextAttributesKey(name, fallback)
}

class YanaSyntaxHighlighter : SyntaxHighlighterBase() {
    override fun getHighlightingLexer(): Lexer = YanaLexerAdapter()

    override fun getTokenHighlights(tokenType: IElementType): Array<TextAttributesKey> {
        val key = when (tokenType) {
            YanaTokenTypes.KEYWORD -> YanaColors.KEYWORD
            YanaTokenTypes.RESERVED_OP -> YanaColors.RESERVED_OP
            YanaTokenTypes.TRY -> YanaColors.TRY
            YanaTokenTypes.OPTIONAL_CHAIN -> YanaColors.OPTIONAL_CHAIN

            YanaTokenTypes.VAR_ID -> YanaColors.VAR_ID
            YanaTokenTypes.CON_ID -> YanaColors.CON_ID
            YanaTokenTypes.VAR_SYM -> YanaColors.VAR_SYM
            YanaTokenTypes.CON_SYM -> YanaColors.CON_SYM

            YanaTokenTypes.INTEGER, YanaTokenTypes.FLOAT -> YanaColors.NUMBER
            YanaTokenTypes.STRING -> YanaColors.STRING
            YanaTokenTypes.STRING_ESCAPE -> YanaColors.STRING_ESCAPE

            YanaTokenTypes.LINE_COMMENT -> YanaColors.LINE_COMMENT
            YanaTokenTypes.BLOCK_COMMENT -> YanaColors.BLOCK_COMMENT

            YanaTokenTypes.PAREN_L, YanaTokenTypes.PAREN_R -> YanaColors.PARENTHESES
            YanaTokenTypes.BRACKET_L, YanaTokenTypes.BRACKET_R -> YanaColors.BRACKETS
            YanaTokenTypes.BRACE_L, YanaTokenTypes.BRACE_R -> YanaColors.BRACES
            YanaTokenTypes.COMMA -> YanaColors.COMMA
            YanaTokenTypes.SEMICOLON -> YanaColors.SEMICOLON
            YanaTokenTypes.GRAVE -> YanaColors.GRAVE

            TokenType.BAD_CHARACTER -> YanaColors.BAD_CHARACTER
            else -> return emptyArray()
        }

        return arrayOf(key)
    }
}

class YanaSyntaxHighlighterFactory : SyntaxHighlighterFactory() {
    override fun getSyntaxHighlighter(project: Project?, virtualFile: VirtualFile?): SyntaxHighlighter =
        YanaSyntaxHighlighter()
}
