package se.rimmer.yana

import com.intellij.testFramework.fixtures.BasePlatformTestCase

/*
 * What happens when you type - the auto-closing half of Implementation-Tooling.md §9.
 *
 * These exist because the alternative is guessing. Whether the platform closes a bracket for a
 * language with a lexer but no parser is not something the plugin's own code says; it is decided
 * several layers down, in `TypedHandler` and `BraceMatchingUtil`, by conditions that reading the
 * bytecode did not reliably predict. The fixture types a character into a real editor over a real
 * `.yana` file and looks at the document afterwards, which is the same thing a user does.
 */
class YanaTypingTest : BasePlatformTestCase() {
    /// Compares the document text directly rather than through `checkResult`, whose failure is a
    /// FileComparisonFailedError that carries the diff somewhere a CI log does not show it.
    private fun typing(before: String, typed: String, after: String) {
        myFixture.configureByText("Test.yana", before)
        myFixture.type(typed)

        val expected = after.replace("<caret>", "")
        assertEquals("after typing '$typed'", expected, myFixture.editor.document.text)

        val caret = after.indexOf("<caret>")
        if (caret >= 0) assertEquals("caret offset", caret, myFixture.editor.caretModel.offset)
    }

    fun testParenCloses() = typing("fn f<caret>", "(", "fn f(<caret>)")

    fun testBracketCloses() = typing("let xs = <caret>", "[", "let xs = [<caret>]")

    fun testBraceCloses() = typing("let p = Point <caret>", "{", "let p = Point {<caret>}")

    /// Already verified by hand, and here so that the bracket work below cannot regress it.
    fun testQuoteCloses() = typing("let s = <caret>", "\"", "let s = \"<caret>\"")

    /// Typing the closing character where one was auto-inserted moves over it rather than doubling
    /// it - the other half of what makes auto-closing bearable.
    fun testTypingClosingParenStepsOver() = typing("fn f(<caret>)", ")", "fn f()<caret>")
}
