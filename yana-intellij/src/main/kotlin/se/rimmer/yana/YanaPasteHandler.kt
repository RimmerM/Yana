package se.rimmer.yana

import com.intellij.application.options.CodeStyle
import com.intellij.codeInsight.CodeInsightSettings
import com.intellij.codeInsight.editorActions.TypingActionsExtension
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.project.Project
import com.intellij.psi.PsiDocumentManager

/*
 * What a paste does to indentation.
 *
 * Without this the platform's generic block indent runs, because the plugin has no formatter: it asks
 * the line-indent provider where the *first* pasted line belongs and moves every line by the same
 * amount. In a brace language that is harmless, since the next reformat puts things right. In Yana it
 * is not, for two reasons. The caret is already standing in the indent the author chose, so moving the
 * first line again doubles it; and a selection copied from the first character of a line has lost that
 * line's indent while the lines after it kept theirs, so "the first line's indent" is the one number in
 * the pasted text that means nothing. Both gave the extra level a paste kept arriving with.
 *
 * So the pasted text keeps its **shape** - every line's indent relative to the others is what it was
 * where it was copied, because that shape is the program - and only *where* the block sits is decided
 * here, from the text either side of it:
 *
 *   - at the start of a line (nothing but whitespace before it), the first line goes at the caret;
 *   - whole lines pasted at column zero in front of an existing line - which is how a line copied with
 *     nothing selected arrives - go at that line's indent, since the caret said nothing;
 *   - in the middle of a line the first line stays where it landed, and the rest are placed relative to
 *     the line it landed in.
 *
 * Where the first line has no indent of its own the lines after it say what it was: the next non-blank
 * line's indent, less a level when the first line opens a block. Nothing here asks the server - a paste
 * has to be finished before the next keystroke, as an indent does.
 */
class YanaPasteHandler : TypingActionsExtension {
    override fun isSuitableContext(project: Project, editor: Editor): Boolean =
        PsiDocumentManager.getInstance(project).getPsiFile(editor.document)?.language == YanaLanguage

    override fun format(
        project: Project,
        editor: Editor,
        howtoReformat: Int,
        startOffset: Int,
        endOffset: Int,
        anchorColumn: Int,
        indentBeforeReformat: Boolean,
        formatInjected: Boolean,
    ) {
        if (howtoReformat == CodeInsightSettings.NO_REFORMAT) return

        val document = editor.document
        val options = CodeStyle.getIndentOptions(project, document)
        val step = if (options.INDENT_SIZE > 0) options.INDENT_SIZE else 4
        val tab = if (options.TAB_SIZE > 0) options.TAB_SIZE else step

        val edits = YanaPasteLayout.plan(document.charsSequence, startOffset, endOffset, step, tab) ?: return

        // Bottom up, so every offset computed above is still the one it was.
        for (edit in edits.asReversed()) {
            val indent = YanaLineIndentProvider.indentOf(edit.width, options.USE_TAB_CHARACTER, tab)
            document.replaceString(edit.start, edit.end, indent)
        }

        PsiDocumentManager.getInstance(project).commitDocument(document)
    }
}

/// The layout decision alone, over text - which is what makes it testable without an editor and
/// keeps the rule in one place.
internal object YanaPasteLayout {
    /// Replace `start until end` - a line's leading whitespace - with an indent `width` columns wide.
    data class Edit(val start: Int, val end: Int, val width: Int)

    fun plan(text: CharSequence, start: Int, end: Int, step: Int, tab: Int): List<Edit>? {
        if (start < 0 || end > text.length || start >= end) return null

        val lineStart = lineStartOf(text, start)
        val atLineStart = (lineStart until start).all { text[it] == ' ' || text[it] == '\t' }

        // The pasted lines, as the offsets each begins at. The first begins at the paste itself.
        val lines = mutableListOf(start)
        for (i in start until end) if (text[i] == '\n' && i + 1 < end) lines += i + 1

        // A paste ending in a line break has an empty last line that belongs to the text after it.
        val endsWithBreak = text[end - 1] == '\n'

        val firstIndentEnd = whitespaceEnd(text, start, end)
        val firstWritten = firstIndentEnd > start

        // Where the first line was, in the columns of the text it was copied from.
        val later = lines.drop(1).firstOrNull { !isBlank(text, it, end) }
        val base = when {
            firstWritten || later == null -> width(text, start, firstIndentEnd, tab)
            else -> {
                val content = text.subSequence(firstIndentEnd, lineEndOf(text, firstIndentEnd, end)).toString()
                val opens = YanaLineIndentProvider.opensBlock(YanaLineIndentProvider.trimComment(content))
                maxOf(0, width(text, later, whitespaceEnd(text, later, end), tab) - if (opens) step else 0)
            }
        }

        // Where it goes. `prefix` is the indent already in front of the paste on its own line.
        val prefix = width(text, lineStart, whitespaceEnd(text, lineStart, start), tab)
        val target: Int
        val edits = mutableListOf<Edit>()

        if (atLineStart) {
            val following = if (endsWithBreak && start == lineStart) end else -1
            target = if (following in 0 until text.length && !isBlank(text, following, text.length)) {
                width(text, following, whitespaceEnd(text, following, text.length), tab)
            } else {
                width(text, lineStart, start, tab)
            }

            // The first line's own indent and whatever stood in front of it on the line become one.
            edits += Edit(lineStart, firstIndentEnd, target)
        } else {
            target = prefix
        }

        for (line in lines.drop(1)) {
            val indentEnd = whitespaceEnd(text, line, end)
            if (indentEnd == end || text[indentEnd] == '\n' || text[indentEnd] == '\r') continue

            val shifted = width(text, line, indentEnd, tab) - base + target
            edits += Edit(line, indentEnd, maxOf(0, shifted))
        }

        return edits
    }

    private fun lineStartOf(text: CharSequence, offset: Int): Int {
        var i = offset
        while (i > 0 && text[i - 1] != '\n') i--
        return i
    }

    private fun lineEndOf(text: CharSequence, offset: Int, limit: Int): Int {
        var i = offset
        while (i < limit && text[i] != '\n' && text[i] != '\r') i++
        return i
    }

    private fun whitespaceEnd(text: CharSequence, offset: Int, limit: Int): Int {
        var i = offset
        while (i < limit && (text[i] == ' ' || text[i] == '\t')) i++
        return i
    }

    private fun isBlank(text: CharSequence, offset: Int, limit: Int): Boolean {
        val i = whitespaceEnd(text, offset, limit)
        return i == limit || text[i] == '\n' || text[i] == '\r'
    }

    // Columns, with a tab going to the next stop - the compiler's lexer's rule.
    private fun width(text: CharSequence, start: Int, end: Int, tab: Int): Int {
        var columns = 0
        for (i in start until end) columns = if (text[i] == '\t') columns + tab - columns % tab else columns + 1
        return columns
    }
}
