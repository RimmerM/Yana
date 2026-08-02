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

    /*
     * Enter, which is the other half of the same problem: a new line at column zero under a `:` is
     * a different program from the one the author just said they were writing.
     *
     * The whole document is asserted rather than the caret alone, because what the indent provider
     * returns is *text* the platform inserts - a rule that produced the right column by leaving a
     * tab behind, or by indenting the line below as well, would pass a caret check.
     */
    private fun enter(before: String, after: String) {
        myFixture.configureByText("Test.yana", before)
        myFixture.performEditorAction(IdeActions.ACTION_EDITOR_ENTER)

        assertEquals(after.replace("<caret>", ""), myFixture.editor.document.text)

        val caret = after.indexOf("<caret>")
        if (caret >= 0) assertEquals("caret offset", caret, myFixture.editor.caretModel.offset)
    }

    /// A header that opens a block indents one level further than itself.
    fun testEnterAfterColonOpensLevel() =
        enter("fn main() -> Int:<caret>", "fn main() -> Int:\n    <caret>")

    /// And so does an alternative's arrow, which opens one the same way.
    fun testEnterAfterArrowOpensLevel() =
        enter("fn f(s: Shape) -> Int = match s:\n    Circle(c) -><caret>",
              "fn f(s: Shape) -> Int = match s:\n    Circle(c) ->\n        <caret>")

    /// A line inside a block keeps its level: only a header opens one.
    fun testEnterInsideBlockKeepsLevel() =
        enter("fn main() -> Int:\n    let x = 1<caret>", "fn main() -> Int:\n    let x = 1\n    <caret>")

    /// A `:` inside a comment is text, not a block.
    fun testEnterAfterCommentedColonKeepsLevel() =
        enter("fn main() -> Int:\n    let x = 1 -- a note:<caret>",
              "fn main() -> Int:\n    let x = 1 -- a note:\n    <caret>")

    /// A blank line continues whatever was written above it rather than starting at the margin.
    fun testEnterOnBlankLineKeepsEnclosingLevel() =
        enter("fn main() -> Int:\n    let x = 1\n<caret>", "fn main() -> Int:\n    let x = 1\n\n    <caret>")

    /// Nothing above it at all, which is where a new declaration goes.
    fun testEnterAtTopLevelStaysAtMargin() =
        enter("fn main() -> Int = 1<caret>", "fn main() -> Int = 1\n<caret>")
}
