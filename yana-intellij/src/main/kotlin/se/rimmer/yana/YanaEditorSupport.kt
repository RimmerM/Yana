package se.rimmer.yana

import com.intellij.codeInsight.editorActions.SimpleTokenSetQuoteHandler
import com.intellij.lang.BracePair
import com.intellij.lang.Commenter
import com.intellij.lang.PairedBraceMatcher
import com.intellij.psi.PsiFile
import com.intellij.psi.tree.IElementType

/*
 * The features that come from having a lexer.
 *
 * All three are why §9 recommends a native lexer rather than relying on the server for everything:
 * none is an LSP feature, each is one small table, and none should stop working because the server
 * is starting up or dead.
 */

class YanaBraceMatcher : PairedBraceMatcher {
    override fun getPairs(): Array<BracePair> = PAIRS

    override fun isPairedBracesAllowedBeforeType(lbraceType: IElementType, next: IElementType?) = true

    override fun getCodeConstructStart(file: PsiFile?, openingBraceOffset: Int) = openingBraceOffset

    companion object {
        private val PAIRS = arrayOf(
            BracePair(YanaTokenTypes.PAREN_L, YanaTokenTypes.PAREN_R, false),
            BracePair(YanaTokenTypes.BRACKET_L, YanaTokenTypes.BRACKET_R, false),
            BracePair(YanaTokenTypes.BRACE_L, YanaTokenTypes.BRACE_R, true),
        )
    }
}

class YanaCommenter : Commenter {
    override fun getLineCommentPrefix() = "--"

    // `{- -}` nest, so commenting out a region that already holds a block comment is safe - which is
    // what `getCommentedBlockCommentPrefix` returning null would otherwise have to work around.
    override fun getBlockCommentPrefix() = "{-"
    override fun getBlockCommentSuffix() = "-}"
    override fun getCommentedBlockCommentPrefix() = "{-"
    override fun getCommentedBlockCommentSuffix() = "-}"
}

/*
 * `"` and `'` - §9's "quote handling", which is the half of auto-closing that brackets get for free.
 *
 * The platform auto-closes brackets off the `PairedBraceMatcher` above, but a quote is not a paired
 * *token*: the lexer produces one STRING token covering both ends, so telling an opening quote from
 * a closing one is a question about where in that token the caret is. That is what a QuoteHandler
 * answers, and `SimpleTokenSetQuoteHandler` answers it from the highlighter's tokens - so this works
 * with no parser, which is the whole point of the lexical layer.
 *
 * STRING_ESCAPE is in the set with the literals deliberately: `"a\n` lexes as STRING, STRING_ESCAPE,
 * and a handler that did not recognize the escape as part of a literal would treat the quote before
 * it as unclosed and stop closing quotes for the rest of the line.
 */
class YanaQuoteHandler : SimpleTokenSetQuoteHandler(
    YanaTokenTypes.STRING,
    YanaTokenTypes.STRING_ESCAPE,
    YanaTokenTypes.CHAR,
)
