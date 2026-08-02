package se.rimmer.yana

import com.intellij.application.options.CodeStyle
import com.intellij.codeInsight.CodeInsightSettings
import com.intellij.codeInsight.editorActions.BackspaceHandlerDelegate
import com.intellij.codeInsight.editorActions.SmartBackspaceMode
import com.intellij.openapi.editor.Editor
import com.intellij.psi.PsiFile

/*
 * Backspace inside an indent goes back one level, not one space.
 *
 * This matters more in Yana than in a brace language, and that is the reason it is here rather than
 * left to taste: the layout rule makes indentation *syntax*. A line at the wrong column is a
 * different program - it closes a block, or fails to - so walking an indent back a space at a time
 * is walking through states that are all wrong, and the one that is right is not marked.
 *
 * What it does not try to be is context-dependent. Knowing which column a line *may* legally sit at
 * needs the enclosing block structure, which is the server's knowledge and not the lexer's; getting
 * that wrong would be worse than this, because it would move the caret somewhere confident and
 * incorrect. Snapping to the indent stop below is right whenever the file is indented in multiples
 * of the indent size, which is every file anybody writes, and predictable when it is not.
 */
class YanaBackspaceHandler : BackspaceHandlerDelegate() {
    override fun beforeCharDeleted(c: Char, file: PsiFile, editor: Editor) {}

    /// True when this handler consumed the keystroke. The platform has already deleted `c` by the
    /// time it asks, so consuming means removing the rest of the level.
    override fun charDeleted(c: Char, file: PsiFile, editor: Editor): Boolean {
        if (file.fileType != YanaFileType) return false
        if (c != ' ' && c != '\t') return false

        // Honour the user's own Smart Keys setting rather than overriding it: someone who turned
        // backspace smartness off asked for one character at a time everywhere.
        if (CodeInsightSettings.getInstance().backspaceMode == SmartBackspaceMode.OFF) return false

        val document = editor.document
        val offset = editor.caretModel.offset
        val text = document.charsSequence

        val line = document.getLineNumber(offset)
        val lineStart = document.getLineStartOffset(line)

        // Only within the leading whitespace. Past the first non-blank character a backspace is an
        // ordinary one, whatever the character happens to be.
        for (i in lineStart until offset) {
            if (text[i] != ' ' && text[i] != '\t') return false
        }

        val column = offset - lineStart
        if (column == 0) return false

        val indent = CodeStyle.getIndentOptions(file).INDENT_SIZE
        if (indent <= 1) return false

        /*
         * The stop below where the caret now is.
         *
         * A partial indent snaps down to the nearest stop rather than removing a fixed `indent`,
         * which is what keeps a file that is two spaces out from staying two spaces out forever.
         */
        val target = ((column - 1) / indent) * indent
        if (target >= column) return false

        document.deleteString(lineStart + target, offset)
        return true
    }
}
