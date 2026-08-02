package se.rimmer.yana

import junit.framework.TestCase
import se.rimmer.yana.highlight.YanaSemanticColors
import se.rimmer.yana.lsp.YanaSemanticTokens

/*
 * The token-to-colour mapping - Implementation-Tooling.md §11.
 *
 * A pure function of (type, modifiers), which is what makes it worth testing on its own for the same
 * reason the lexer is. Two of the three decisions it encodes are invisible in the code and silent
 * when wrong: which axis wins when a binding is both heap-placed and borrowed, and that a modifier
 * only colours the kinds that can carry one.
 */
class YanaSemanticTokensTest : TestCase() {
    private val tokens = YanaSemanticTokens()

    private fun keyOf(type: String, vararg modifiers: String) =
        tokens.getTextAttributesKey(type, modifiers.toList())

    fun testOwnershipModifiersColourBindings() {
        assertEquals(YanaSemanticColors.BORROWED, keyOf("parameter", "borrowed"))
        assertEquals(YanaSemanticColors.BORROWED, keyOf("variable", "borrowed"))
        assertEquals(YanaSemanticColors.SUNK, keyOf("parameter", "sunk"))
        assertEquals(YanaSemanticColors.HEAP_PLACED, keyOf("variable", "heapPlaced"))
    }

    /*
     * Placement wins over convention, and the reason is in YanaSemanticTokens: `&` and `->` are
     * written in the source, and where a binding was placed is not. A token carries one key, so this
     * ordering is a decision rather than a detail - asserted here so that changing it is deliberate.
     */
    fun testHeapPlacementWinsOverConvention() {
        assertEquals(YanaSemanticColors.HEAP_PLACED, keyOf("variable", "borrowed", "heapPlaced"))
        assertEquals(YanaSemanticColors.HEAP_PLACED, keyOf("variable", "heapPlaced", "sunk"))
    }

    /// A modifier on a kind that cannot own storage is not a binding's colour. Nothing emits these
    /// today; the check is what stops a future one from silently picking the wrong key up.
    fun testModifiersDoNotLeakOntoOtherKinds() {
        assertNotSame(YanaSemanticColors.BORROWED, keyOf("function", "borrowed"))
        assertNotSame(YanaSemanticColors.HEAP_PLACED, keyOf("property", "heapPlaced"))
        assertNotSame(YanaSemanticColors.SUNK, keyOf("type", "sunk"))
    }

    /// A binding with no Yana modifier is left to the platform, which is what keeps every other
    /// LSP-backed language's colouring and the user's theme in charge.
    fun testPlainBindingsFallThroughToThePlatform() {
        assertNotSame(YanaSemanticColors.BORROWED, keyOf("variable"))
        assertNotSame(YanaSemanticColors.SUNK, keyOf("parameter", "declaration"))
    }

    /// The three names the server's legend writes have to be declared here, or the platform never
    /// decodes them and every mapping above is unreachable. See compiler/lsp/feature.cpp.
    fun testTheServersModifiersAreDeclared() {
        val declared = tokens.tokenModifiers
        assertTrue("borrowed" in declared)
        assertTrue("sunk" in declared)
        assertTrue("heapPlaced" in declared)

        // Appended, not replacing: the server emits the standard ones too.
        assertTrue("declaration" in declared)
    }
}
