package se.rimmer.yana

import com.intellij.codeInsight.editorActions.enter.EnterHandlerDelegate
import com.intellij.codeInsight.editorActions.enter.EnterHandlerDelegateAdapter
import com.intellij.openapi.actionSystem.DataContext
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.editor.actionSystem.EditorActionHandler
import com.intellij.openapi.util.Ref
import com.intellij.psi.PsiFile

/*
 * Enter is a syntax operation in Yana - Implementation-Tooling.md §9.
 *
 * The line-indent provider supplies the actual number of spaces, but the platform only asks it
 * when smart indentation is enabled. That setting is reasonable for ordinary languages and
 * wrong for Yana: a disabled convenience setting must not turn a valid record into an invalid
 * one. This delegate applies the provider's answer directly and keeps the platform's brace-pair
 * adjustment from changing Yana's layout.
 */
class YanaEnterHandlerDelegate : EnterHandlerDelegateAdapter() {
    override fun preprocessEnter(
        file: PsiFile,
        editor: Editor,
        caretOffset: Ref<Int>,
        caretAdvance: Ref<Int>,
        dataContext: DataContext,
        originalHandler: EditorActionHandler?,
    ): EnterHandlerDelegate.Result {
        if (file.language != YanaLanguage || editor.selectionModel.hasSelection()) {
            return EnterHandlerDelegate.Result.Continue
        }

        val offset = caretOffset.get()
        val indent = YanaLineIndentProvider().getLineIndent(file.project, editor, YanaLanguage, offset)
            ?: return EnterHandlerDelegate.Result.Continue

        // Insert the complete layout decision before the platform can apply its brace-pair
        // adjustment. That adjustment is useful for brace languages, but in Yana `}` is ordinary
        // record syntax and must remain on the following source line.
        val document = editor.document
        val text = document.charsSequence
        var suffix = offset
        while (suffix < text.length && (text[suffix] == ' ' || text[suffix] == '\t')) suffix++

        // Match the platform's useful empty-record behavior: `{<caret>}` becomes an empty line
        // between the braces. If the close follows a space, move it to the new line without that
        // space - `field: A<caret> }` is the same record layout, not a request for a fifth space.
        val closesRecord = suffix < text.length && text[suffix] == '}'
        if (closesRecord && suffix > offset) document.deleteString(offset, suffix)

        val insertion = if (closesRecord && suffix == offset) "\n$indent\n" else "\n$indent"
        document.insertString(offset, insertion)
        editor.caretModel.moveToOffset(offset + 1 + indent.length)
        return EnterHandlerDelegate.Result.Stop
    }
}
