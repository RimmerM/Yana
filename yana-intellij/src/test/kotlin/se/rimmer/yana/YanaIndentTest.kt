package se.rimmer.yana

import com.intellij.openapi.actionSystem.IdeActions
import com.intellij.testFramework.fixtures.BasePlatformTestCase

/*
 * Indentation, which in Yana is syntax rather than style.
 *
 * The layout rule means a wrong indent is a different program, not an ugly one - so backspace
 * landing on an indent stop matters more here than in a brace language. Asserted through a real
 * editor for the same reason YanaTypingTest is: what the platform does for a given language depends
 * on machinery several layers down, and reading it is not a reliable way to find out.
 */
class YanaIndentTest : BasePlatformTestCase() {
    private fun backspace(before: String, after: String) {
        myFixture.configureByText("Test.yana", before)
        myFixture.performEditorAction(IdeActions.ACTION_EDITOR_BACKSPACE)

        assertEquals(after.replace("<caret>", ""), myFixture.editor.document.text)

        val caret = after.indexOf("<caret>")
        if (caret >= 0) assertEquals("caret offset", caret, myFixture.editor.caretModel.offset)
    }

    /// A caret in leading whitespace goes back a whole indent level, not one space.
    fun testBackspaceInIndentUnindents() =
        backspace("fn main() -> Int:\n        <caret>\n", "fn main() -> Int:\n    <caret>\n")

    /// From the first level, back to the margin.
    fun testBackspaceFromFirstLevelClearsIndent() =
        backspace("fn main() -> Int:\n    <caret>\n", "fn main() -> Int:\n<caret>\n")

    /// A partial indent snaps to the stop below rather than removing a fixed four.
    fun testBackspaceFromPartialIndentSnapsToStop() =
        backspace("fn main() -> Int:\n      <caret>\n", "fn main() -> Int:\n    <caret>\n")

    /// Outside the indent it is an ordinary backspace - one character, whatever that character is.
    fun testBackspaceInTextDeletesOneCharacter() =
        backspace("fn main() -> Int:\n    let ab<caret>\n", "fn main() -> Int:\n    let a<caret>\n")
}
