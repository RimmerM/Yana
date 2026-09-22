package se.rimmer.yana

import com.intellij.codeInsight.AutoPopupController
import com.intellij.codeInsight.editorActions.BackspaceHandlerDelegate
import com.intellij.codeInsight.editorActions.TypedHandlerDelegate
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.editor.ex.EditorEx
import com.intellij.openapi.fileTypes.FileType
import com.intellij.openapi.project.Project
import com.intellij.psi.PsiFile

/*
 * The backticks of an infix application - `` a `shl` b ``.
 *
 * Not a `BracePair`, because the two ends are the same token and a brace matcher needs them to be
 * different ones; and not a quote, because the lexer makes each backtick a token of its own rather
 * than one token covering the name. So whether a backtick opens or closes is counted: an odd number of
 * backticks earlier on the line means one is open, and this one closes it. A name between backticks
 * never spans a line, which is what makes the line the right extent to count over.
 *
 * Opening one writes the pair where nothing follows the caret yet - the same rule `YanaBraceMatcher`
 * gives an opening bracket - and asks for completion, which the server answers with the functions that
 * may stand there: those with a declared fixity. Typing the closing one where it is already written
 * steps over it, and backspace between an empty pair takes both.
 */
class YanaInfixTypedHandler : TypedHandlerDelegate() {
    override fun beforeCharTyped(c: Char, project: Project, editor: Editor, file: PsiFile, fileType: FileType): Result {
        if (c != '`' || file.fileType != YanaFileType) return Result.CONTINUE

        val offset = editor.caretModel.offset
        if (!YanaInfix.inCode(editor, offset)) return Result.CONTINUE

        val document = editor.document
        val text = document.charsSequence
        val next = if (offset < text.length) text[offset] else null

        if (YanaInfix.openBefore(editor, offset)) {
            if (next != '`') return Result.CONTINUE
            editor.caretModel.moveToOffset(offset + 1)
            return Result.STOP
        }

        AutoPopupController.getInstance(project).scheduleAutoPopup(editor)
        if (next != null && !next.isWhitespace() && next !in ")]},;") return Result.CONTINUE

        document.insertString(offset, "``")
        editor.caretModel.moveToOffset(offset + 1)
        return Result.STOP
    }
}

class YanaInfixBackspaceHandler : BackspaceHandlerDelegate() {
    override fun beforeCharDeleted(c: Char, file: PsiFile, editor: Editor) {
        if (c != '`' || file.fileType != YanaFileType) return

        // The caret is still after the backtick being deleted, so the pair is `` `|` `` exactly when
        // the character at the caret is the closing one and the one being deleted opened it.
        val offset = editor.caretModel.offset
        val text = editor.document.charsSequence
        if (offset >= text.length || text[offset] != '`') return
        if (YanaInfix.openBefore(editor, offset - 1)) return

        editor.document.deleteString(offset, offset + 1)
    }

    override fun charDeleted(c: Char, file: PsiFile, editor: Editor) = false
}

internal object YanaInfix {
    /// Whether an odd number of backtick *tokens* precede `offset` on its line - which is to say one
    /// is open. Tokens rather than characters, so a backtick inside a string or a comment is not
    /// counted.
    fun openBefore(editor: Editor, offset: Int): Boolean {
        val document = editor.document
        val lineStart = document.getLineStartOffset(document.getLineNumber(offset))
        if (offset <= lineStart) return false

        val iterator = (editor as EditorEx).highlighter.createIterator(lineStart)
        var open = false
        while (!iterator.atEnd() && iterator.start < offset) {
            if (iterator.tokenType == YanaTokenTypes.GRAVE && iterator.start >= lineStart) open = !open
            iterator.advance()
        }
        return open
    }

    /*
     * Whether the caret is in code rather than inside a string or a comment, where a backtick is only
     * a character. Asked of the token before the caret: a line comment runs to the end of its line, and
     * a string is still open unless that token is its closing quote - a lone `"` with more of the
     * literal in front of it. The lexer returns each quote as a token of its own, which is what makes
     * that readable. A block comment's `-}` is taken as closing it; a nested one is rare enough in a
     * line being typed that the cost is a backtick paired inside a comment.
     */
    fun inCode(editor: Editor, offset: Int): Boolean {
        if (offset == 0) return true
        val iterator = (editor as EditorEx).highlighter.createIterator(offset - 1)
        if (iterator.atEnd()) return true

        val text = editor.document.charsSequence
        return when (iterator.tokenType) {
            YanaTokenTypes.LINE_COMMENT -> false
            YanaTokenTypes.BLOCK_COMMENT ->
                iterator.end == offset && offset >= 2 && text[offset - 2] == '-' && text[offset - 1] == '}'
            YanaTokenTypes.STRING_ESCAPE -> false
            YanaTokenTypes.STRING -> {
                if (iterator.end != offset || iterator.end - iterator.start != 1 || text[offset - 1] != '"') {
                    false
                } else {
                    iterator.retreat()
                    !iterator.atEnd() && iterator.tokenType in YanaTokenTypes.STRINGS
                }
            }
            else -> true
        }
    }
}
