package se.rimmer.yana

import com.intellij.openapi.actionSystem.IdeActions
import com.intellij.openapi.ide.CopyPasteManager
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import java.awt.datatransfer.StringSelection

/*
 * Pasting, which in a layout language decides what the pasted code *means* - YanaPasteHandler.
 *
 * Asserted through the platform's own paste action rather than by calling the handler, because what
 * went wrong before it existed was the platform's generic block indent, and only the real action
 * shows which of the two ran.
 */
class YanaPasteTest : BasePlatformTestCase() {
    private fun paste(before: String, clipboard: String, after: String) {
        myFixture.configureByText("Test.yana", before)
        CopyPasteManager.getInstance().setContents(StringSelection(clipboard))
        myFixture.performEditorAction(IdeActions.ACTION_PASTE)
        assertEquals(after, myFixture.editor.document.text)
    }

    /// Lines copied with their indentation, pasted at the caret on an indented empty line: the caret
    /// is where the first one goes, and the rest keep their place relative to it.
    fun testIndentedLinesAtIndentedCaret() = paste(
        "fn f():\n    <caret>\n",
        "    foo()\n    bar()",
        "fn f():\n    foo()\n    bar()\n",
    )

    /// A selection that began at the first character of a line has lost that line's indent; the lines
    /// after it still carry theirs, and are what says where the first one was.
    fun testSelectionFromFirstCharacter() = paste(
        "fn f():\n    <caret>\n",
        "foo()\n    bar()",
        "fn f():\n    foo()\n    bar()\n",
    )

    /// The same, where the first line opens a block: the line after it is one level deeper.
    fun testSelectionOpeningABlock() = paste(
        "fn f():\n    <caret>\n",
        "if x:\n        y()\n    z()",
        "fn f():\n    if x:\n        y()\n    z()\n",
    )

    /// A block moved to a different depth keeps its shape.
    fun testBlockMovedDeeper() = paste(
        "fn f():\n    if c:\n        <caret>\n",
        "if x:\n    y()\nz()",
        "fn f():\n    if c:\n        if x:\n            y()\n        z()\n",
    )

    /// In the middle of a line the first line stays where it lands, and the rest are placed relative
    /// to the line they were pasted into.
    fun testIntoTheMiddleOfALine() = paste(
        "fn f(y: Int) -> Int:\n    let x = <caret>\n",
        "match y:\n    0 -> 1\n    _ -> 2",
        "fn f(y: Int) -> Int:\n    let x = match y:\n        0 -> 1\n        _ -> 2\n",
    )

    /// Whole lines pasted at the start of a line go in at that line's indent - the shape a line copied
    /// with nothing selected arrives in.
    fun testWholeLinesBeforeAnIndentedLine() = paste(
        "fn f() -> Int:\n<caret>    return 1\n",
        "        foo()\n",
        "fn f() -> Int:\n    foo()\n    return 1\n",
    )

    /// A declaration pasted at the margin stays at the margin.
    fun testTopLevelDeclaration() = paste(
        "fn f():\n    g()\n\n<caret>",
        "fn h():\n    g()",
        "fn f():\n    g()\n\nfn h():\n    g()",
    )

    /// One line with no layout in it is pasted as it is.
    fun testSingleWord() = paste("let x = <caret>", "foo", "let x = foo")
}
