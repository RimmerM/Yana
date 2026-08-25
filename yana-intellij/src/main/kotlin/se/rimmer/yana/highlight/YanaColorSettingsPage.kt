package se.rimmer.yana.highlight

import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.fileTypes.SyntaxHighlighter
import com.intellij.openapi.options.colors.AttributesDescriptor
import com.intellij.openapi.options.colors.ColorDescriptor
import com.intellij.openapi.options.colors.ColorSettingsPage
import se.rimmer.yana.YanaIcons
import javax.swing.Icon

/// Settings > Editor > Color Scheme > Yana. Without this every key above is themable only by
/// editing a scheme file by hand, which nobody does.
class YanaColorSettingsPage : ColorSettingsPage {
    override fun getIcon(): Icon = YanaIcons.FILE
    override fun getHighlighter(): SyntaxHighlighter = YanaSyntaxHighlighter()
    /*
     * The semantic keys, made previewable.
     *
     * The demo below is coloured by the *lexer*, and the three keys the server drives are not
     * reachable that way - there is no server running behind a settings dialog. The platform's
     * answer is these tags: `<borrowed>p</borrowed>` in the demo text is stripped before display and
     * painted with the key it names, so the preview shows what the editor will show.
     */
    override fun getAdditionalHighlightingTagToDescriptorMap(): Map<String, TextAttributesKey> = mapOf(
        "borrowed" to YanaSemanticColors.BORROWED,
        "sunk" to YanaSemanticColors.SUNK,
        "heap" to YanaSemanticColors.HEAP_PLACED,
    )
    override fun getAttributeDescriptors(): Array<AttributesDescriptor> = DESCRIPTORS
    override fun getColorDescriptors(): Array<ColorDescriptor> = ColorDescriptor.EMPTY_ARRAY
    override fun getDisplayName() = "Yana"

    // A sample that exercises every key, since the preview is the only place their differences are
    // visible side by side.
    override fun getDemoText() = """
        -- A comment, and a {- nested {- block -} one -}.
        import Core.Array

        data Maybe(a) = Just(a) | Nothing
        data Point {x: Int, y: Int}

        class Show(a):
            fn show(a) -> String

        infixl 6 <+>

        @inline
        fn distance(&<borrowed>p</borrowed>: Point, -><sunk>q</sunk>: Point) -> Float =
            sqrt(toFloat(<borrowed>p</borrowed>.x - <sunk>q</sunk>.x) ^ 2.0 + 1.0e3)

        fn find(xs: Array(Point)) -> Maybe(Int):
            let first = xs.first()?
            return Just(first.x)

        fn nearest(xs: Array(Point)) -> Maybe(Int) = xs.first()?.origin.x

        fn main() -> Int:
            let name = "hello\n{p.x}"
            let c = 'a'
            let bits = 0xff | 0b1010
            let &<heap>escaping</heap> = Point {x: 1, y: 2}
            match name is:
                _ -> 0
    """.trimIndent()

    companion object {
        private val DESCRIPTORS = arrayOf(
            AttributesDescriptor("Keyword", YanaColors.KEYWORD),
            AttributesDescriptor("Reserved operator", YanaColors.RESERVED_OP),
            AttributesDescriptor("Early exit (?)", YanaColors.TRY),
            AttributesDescriptor("Optional chaining (?.)", YanaColors.OPTIONAL_CHAIN),
            AttributesDescriptor("Identifiers//Variable (VarID)", YanaColors.VAR_ID),
            AttributesDescriptor("Identifiers//Constructor (ConID)", YanaColors.CON_ID),
            AttributesDescriptor("Operators//Variable (VarSym)", YanaColors.VAR_SYM),
            AttributesDescriptor("Operators//Constructor (ConSym)", YanaColors.CON_SYM),
            AttributesDescriptor("Literals//Number", YanaColors.NUMBER),
            AttributesDescriptor("Literals//String", YanaColors.STRING),
            AttributesDescriptor("Literals//String escape", YanaColors.STRING_ESCAPE),
            AttributesDescriptor("Comments//Line comment", YanaColors.LINE_COMMENT),
            AttributesDescriptor("Comments//Block comment", YanaColors.BLOCK_COMMENT),
            AttributesDescriptor("Braces and operators//Parentheses", YanaColors.PARENTHESES),
            AttributesDescriptor("Braces and operators//Brackets", YanaColors.BRACKETS),
            AttributesDescriptor("Braces and operators//Braces", YanaColors.BRACES),
            AttributesDescriptor("Braces and operators//Comma", YanaColors.COMMA),
            AttributesDescriptor("Braces and operators//Semicolon", YanaColors.SEMICOLON),
            AttributesDescriptor("Braces and operators//Backtick", YanaColors.GRAVE),
            AttributesDescriptor("Bad character", YanaColors.BAD_CHARACTER),

            // From the language server - Implementation-Tooling.md §11. Grouped apart because they
            // are the only keys here that need a server running to appear, and because what they
            // say is about ownership rather than about syntax.
            AttributesDescriptor("Ownership//Borrowed binding (&)", YanaSemanticColors.BORROWED),
            AttributesDescriptor("Ownership//Sunk binding (->)", YanaSemanticColors.SUNK),
            AttributesDescriptor("Ownership//Heap-placed binding", YanaSemanticColors.HEAP_PLACED),
        )
    }
}
