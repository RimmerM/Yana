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

    /// In front of existing code an opening bracket is being put *around* it, so nothing is closed.
    fun testParenBeforeNameDoesNotClose() = typing("let x = <caret>a + b", "(", "let x = (<caret>a + b")

    fun testBracketBeforeLiteralDoesNotClose() = typing("let x = <caret>1", "[", "let x = [<caret>1")

    /// And before a space, a separator or an enclosing closer it still is.
    fun testParenBeforeSpaceCloses() = typing("f(<caret> 1)", "(", "f((<caret>) 1)")

    fun testParenBeforeCloserCloses() = typing("f(<caret>)", "(", "f((<caret>))")

    /// The backticks of an infix application pair like brackets, step over like them and delete
    /// together - see YanaInfixHandler.
    fun testGravePairs() = typing("let m = a <caret>", "`", "let m = a `<caret>`")

    fun testGraveStepsOverClosing() = typing("let m = a `shl<caret>`", "`", "let m = a `shl`<caret>")

    fun testGraveBeforeNameDoesNotPair() = typing("let m = a <caret>shl` b", "`", "let m = a `<caret>shl` b")

    fun testGraveTypedWhole() = typing("let m = a <caret>", "`shl` b", "let m = a `shl` b<caret>")

    fun testGraveAfterClosedPairPairsAgain() =
        typing("let m = a `shl` b <caret>", "`", "let m = a `shl` b `<caret>`")

    fun testGraveInStringDoesNotPair() = typing("let s = \"a <caret>", "`", "let s = \"a `<caret>")

    fun testGraveAfterStringPairs() = typing("let s = \"a\" <caret>", "`", "let s = \"a\" `<caret>`")

    fun testGraveInCommentDoesNotPair() = typing("-- a <caret>", "`", "-- a `<caret>")

    fun testBackspaceDeletesEmptyGravePair() {
        myFixture.configureByText("Test.yana", "let m = a `<caret>`")
        myFixture.type("\b")
        assertEquals("let m = a ", myFixture.editor.document.text)
    }

    fun testBackspaceKeepsClosingGraveOfName() {
        myFixture.configureByText("Test.yana", "let m = a `shl`<caret>")
        myFixture.type("\b")
        assertEquals("let m = a `shl", myFixture.editor.document.text)
    }
}
