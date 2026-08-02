package se.rimmer.yana.highlight

import com.intellij.openapi.editor.DefaultLanguageHighlighterColors
import com.intellij.openapi.editor.colors.TextAttributesKey

/*
 * The semantic layer of colouring - Implementation-Tooling.md §11's second table.
 *
 * The server's `semanticTokens/full` answers with the standard LSP token types plus three modifiers
 * the standard set has no name for, because they are things only Yana has: whether a binding borrows
 * its storage, whether it consumes it, and whether the compiler placed it on the heap. The standard
 * *types* need nothing here - the platform already colours `function`, `parameter`, `enumMember` and
 * the rest out of the box, and second-guessing that would only fight the user's theme.
 *
 * Each key falls back to the platform default it most often decorates, so installing this plugin
 * changes nothing until somebody themes one of them. That is the whole design: §11 asks for these to
 * be *themable*, not for them to be loud.
 */
object YanaSemanticColors {
    /*
     * Design.md's ownership conventions, as written at the binding: `&x` borrows, `->x` consumes.
     *
     * Both fall back to the parameter colour, which makes them a no-op for the case that dominates -
     * a `&` or `->` *parameter* was already coloured as a parameter. What changes by default is a
     * `let &x` local, which now reads as what it is: a name for storage rather than for a value.
     */
    val BORROWED = key("YANA_SEM_BORROWED", DefaultLanguageHighlighterColors.PARAMETER)
    val SUNK = key("YANA_SEM_SUNK", DefaultLanguageHighlighterColors.PARAMETER)

    /*
     * §11's `heapPlaced`: a binding whose `Local::storage` came out heap rather than stack - the
     * `explain` cliff, shown inline.
     *
     * Falls back to the ordinary local colour, so it is invisible until someone gives it attributes.
     * That is what §11 means by "off by default, on for anyone who cares".
     */
    val HEAP_PLACED = key("YANA_SEM_HEAP_PLACED", DefaultLanguageHighlighterColors.LOCAL_VARIABLE)

    private fun key(name: String, fallback: TextAttributesKey) =
        TextAttributesKey.createTextAttributesKey(name, fallback)
}
