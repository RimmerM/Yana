package se.rimmer.yana

import com.intellij.codeInsight.completion.CompletionInitializationContext
import com.intellij.codeInsight.completion.CompletionParameters
import com.intellij.codeInsight.completion.CompletionProcess
import com.intellij.codeInsight.completion.CompletionType
import com.intellij.codeInsight.completion.InsertionContext
import com.intellij.codeInsight.completion.OffsetMap
import com.intellij.codeInsight.template.impl.TemplateManagerImpl
import com.intellij.openapi.command.WriteCommandAction
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import org.eclipse.lsp4j.CompletionItem
import org.eclipse.lsp4j.InsertTextFormat
import se.rimmer.yana.lsp.YanaCompletion

/** Runs the same provisional-text replacement and live-template start as CLion's LSP lookup. */
class YanaCompletionTest : BasePlatformTestCase() {
    override fun setUp() {
        super.setUp()
        TemplateManagerImpl.setTemplateTesting(testRootDisposable)
    }

    private fun insert(prefix: String, typed: String, snippet: String, expected: String, arguments: List<String>) {
        val name = snippet.substringBefore('(')
        myFixture.configureByText("Test.yana", "$prefix$typed<caret>")
        val editor = myFixture.editor
        val document = editor.document
        val start = prefix.length
        val item = CompletionItem(name).apply {
            insertText = snippet
            insertTextFormat = InsertTextFormat.Snippet
        }
        val process = object : CompletionProcess {
            override fun isAutopopupCompletion() = false
        }
        val parameters = CompletionParameters(
            myFixture.file.findElementAt(start)!!, myFixture.file, CompletionType.BASIC,
            editor.caretModel.offset, 1, editor, process,
        )
        val lookup = YanaCompletion().createLookupElement(parameters, item)!!
        assertEquals(name, lookup.lookupString)
        assertEquals(InsertTextFormat.PlainText, item.insertTextFormat)

        WriteCommandAction.runWriteCommandAction(project) {
            // CodeCompletionHandlerBase provisionally inserts the lookup string before invoking
            // its handler. This is the Enter path for a partially typed qualified call.
            document.replaceString(start, document.textLength, lookup.lookupString)
            editor.caretModel.moveToOffset(document.textLength)
            val offsets = OffsetMap(document)
            offsets.addOffset(CompletionInitializationContext.START_OFFSET, start)
            offsets.addOffset(InsertionContext.TAIL_OFFSET, document.textLength)
            val context = InsertionContext(offsets, '\n', arrayOf(lookup), myFixture.file, editor, false)
            try {
                lookup.handleInsert(context)
                assertEquals(expected, document.text)
                assertFalse("raw LSP placeholder reached the source", document.text.contains("\${"))

                val state = TemplateManagerImpl.getTemplateState(editor)
                assertNotNull("completion must start a live template", state)
                for ((index, argument) in arguments.withIndex()) {
                    val range = state!!.currentVariableRange
                    assertNotNull("argument ${index + 1} must be editable", range)
                    assertEquals(argument, document.getText(range!!))
                    if (index + 1 < arguments.size) state.nextTab()
                }
                state!!.gotoEnd()
            } finally {
                offsets.dispose()
            }
        }
    }

    fun testQualifiedRdmsr() = insert(
        "fn main() = X86.", "rdm",
        "rdmsr(\${1:msr})",
        "fn main() = X86.rdmsr(msr)",
        listOf("msr"),
    )

    fun testQualifiedWrmsrKeepsThreeEditableArguments() = insert(
        "let base = (base `and` 0xffffff000) `or` (1 `shl` 11) in X86.", "wrm",
        "wrmsr(\${1:msr}, \${2:low}, \${3:high})",
        "let base = (base `and` 0xffffff000) `or` (1 `shl` 11) in X86.wrmsr(msr, low, high)",
        listOf("msr", "low", "high"),
    )
}
