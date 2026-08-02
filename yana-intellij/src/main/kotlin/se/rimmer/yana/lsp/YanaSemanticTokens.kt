package se.rimmer.yana.lsp

import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.platform.lsp.api.customization.LspSemanticTokensSupport
import com.intellij.psi.PsiFile
import se.rimmer.yana.YanaFileType
import se.rimmer.yana.highlight.YanaSemanticColors

/*
 * Which colour a semantic token gets - Implementation-Tooling.md §11.
 *
 * The server emits the standard LSP token types, and three modifiers that are Yana's own. The
 * platform knows the standard types and nothing about the three, so this does two things: declares
 * them, so the decoded modifier names reach this code at all, and maps the ones that carry meaning
 * onto themable keys.
 *
 * Everything else falls through to `super`, deliberately. The platform's mapping for `function`,
 * `parameter`, `enumMember` and the rest is what every other LSP-backed language in the IDE uses,
 * and a plugin that overrode it would be fighting the user's theme to say nothing new.
 */
class YanaSemanticTokens : LspSemanticTokensSupport() {
    /*
     * Ask the server for tokens for `.yana` files.
     *
     * The base class only asks for files whose language id is `TEXT` or `textmate` - the platform
     * assumes that a language with a PSI of its own does its own highlighting and needs nothing from
     * a server. That assumption is wrong here in a way that is easy to miss: adding a
     * `ParserDefinition` for the brackets to close (see YanaParserDefinition) moved `.yana` off
     * `TEXT`, and semantic colouring stopped without anything failing - diagnostics kept working,
     * because they are a different mechanism entirely.
     *
     * Yana's PSI is one flat node. It knows nothing. Everything worth colouring past the lexer comes
     * from the server, which is the whole design.
     */
    override fun shouldAskServerForSemanticTokens(psiFile: PsiFile): Boolean =
        psiFile.fileType == YanaFileType || super.shouldAskServerForSemanticTokens(psiFile)

    /*
     * The server's legend, minus what the platform already knows.
     *
     * Appended rather than replaced: the standard modifiers - `declaration`, `readonly`, `static` -
     * are ones the server also emits, and dropping them here would drop the platform's own handling
     * of them.
     */
    override val tokenModifiers: List<String>
        get() = super.tokenModifiers + listOf(BORROWED, SUNK, HEAP_PLACED)

    // `modifiers` rather than `tokenModifiers`: the supertype names it that, and the property above
    // already owns the other name.
    override fun getTextAttributesKey(tokenType: String, modifiers: List<String>): TextAttributesKey? {
        // Only a binding can carry one of these. A `function` or a `type` never does, and checking
        // the type rather than the modifier alone is what keeps a future modifier of the same name
        // on some other kind from silently picking up a binding's colour.
        if (tokenType == VARIABLE || tokenType == PARAMETER) {
            /*
             * Storage placement wins over the ownership convention, which is the one ordering
             * decision here.
             *
             * A token gets exactly one attributes key, so the two axes cannot both be shown. `&` and
             * `->` are *written in the source*, so an editor colouring them tells the reader
             * something already on screen; where a binding was placed is not in the text at all and
             * cannot be worked out by reading. Colour what cannot otherwise be seen. It costs
             * nothing by default either, since HEAP_PLACED is invisible until somebody themes it.
             */
            if (HEAP_PLACED in modifiers) return YanaSemanticColors.HEAP_PLACED
            if (BORROWED in modifiers) return YanaSemanticColors.BORROWED
            if (SUNK in modifiers) return YanaSemanticColors.SUNK
        }

        return super.getTextAttributesKey(tokenType, modifiers)
    }

    private companion object {
        // The names the server writes into the legend - see compiler/lsp/feature.cpp's
        // writeTokenLegend. These two lists are the one place the plugin and the server have to
        // agree, and they are three strings.
        const val BORROWED = "borrowed"
        const val SUNK = "sunk"
        const val HEAP_PLACED = "heapPlaced"

        const val VARIABLE = "variable"
        const val PARAMETER = "parameter"
    }
}
