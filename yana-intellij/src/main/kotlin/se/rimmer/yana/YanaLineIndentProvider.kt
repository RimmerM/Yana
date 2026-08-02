package se.rimmer.yana

import com.intellij.application.options.CodeStyle
import com.intellij.lang.Language
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.project.Project
import com.intellij.psi.codeStyle.lineIndent.LineIndentProvider

/*
 * What column Enter puts the caret in - Implementation-Tooling.md §9.
 *
 * Indentation is syntax in Yana, so this is not a convenience: a line at the wrong column is a
 * different program, and an editor that leaves every new line at column zero makes the author type
 * the structure twice. The platform's own answer for a language with no formatter is to copy the
 * previous line's indent, which is right for the line *inside* a block and wrong for the first line
 * *of* one - and opening a block is exactly the keystroke where the author has just said what they
 * want.
 *
 * The rule is the language's own, read off the line above:
 *
 *   - a line that *opens* a block indents one level further. What opens one is the `:` a `fn`, an
 *     `if`, a `match`, an `instance` or a `class` ends its header with, and the `->` an alternative
 *     or a lambda ends with;
 *   - anything else keeps the previous line's indent;
 *   - a blank line looks further up, since the indent to continue is the last one anything was
 *     written at rather than the nothing on the line between.
 *
 * Read from the *document* rather than from the PSI, which is what a LineIndentProvider is for: it
 * runs on the EDT while the file is being typed in, ahead of the parser, and Yana's layout is a
 * property of the text anyway. It deliberately does not ask the language server - an indent has to
 * be there before the next keystroke, and a request that has to wait for a compile is one that
 * arrives after the author has already typed past it.
 */
class YanaLineIndentProvider : LineIndentProvider {
    override fun isSuitableFor(language: Language?) = language == YanaLanguage

    override fun getLineIndent(project: Project, editor: Editor, language: Language?, offset: Int): String? {
        if (language != YanaLanguage) return null

        val document = editor.document
        val text = document.charsSequence
        if (offset > text.length) return null

        // By document rather than by file type, which is the overload that honours a per-file
        // indent provider and the project's own settings at once - the same answer the backspace
        // handler gets from the file it is given.
        val options = CodeStyle.getIndentOptions(project, document)
        val step = if (options.INDENT_SIZE > 0) options.INDENT_SIZE else 4

        // The line the caret was on when Enter was pressed. `offset` is where the new line will
        // begin, so what decides its indent is everything before it.
        var line = document.getLineNumber(if (offset > 0) offset - 1 else 0)

        while (line >= 0) {
            val start = document.getLineStartOffset(line)
            val end = minOf(document.getLineEndOffset(line), offset)
            val content = trimComment(text.subSequence(start, maxOf(start, end)).toString())

            if (content.isBlank()) {
                // A blank line belongs to whatever surrounds it rather than closing anything, which
                // is the lexer's rule as well as this one's.
                line--
                continue
            }

            val indent = content.indexOfFirst { it != ' ' && it != '\t' }.let { if (it < 0) 0 else it }
            val width = columnWidth(content, indent, options.TAB_SIZE)

            return indentOf(width + if (opensBlock(content)) step else 0, options.USE_TAB_CHARACTER,
                            if (options.TAB_SIZE > 0) options.TAB_SIZE else step)
        }

        return ""
    }

    private companion object {
        // Whether a line ends with something that opens an indentation block. The trailing token is
        // the whole of the rule - `fn f():`, `match s:`, `Circle(c) ->`, `(x) ->` - and it is the
        // same one the parser reads before calling `withLevel`.
        fun opensBlock(content: String): Boolean {
            val line = content.trimEnd()
            return line.endsWith(":") || line.endsWith("->") || line.endsWith("=")
        }

        /*
         * The line without its trailing line comment, so that a `--` holding a `:` does not open a
         * block. A `--` inside a string is not a comment, so quotes are tracked while scanning; a
         * `--` that is part of a longer operator is not one either, which is the lexer's own rule.
         */
        fun trimComment(content: String): String {
            var quote = ' '
            var i = 0

            while (i < content.length) {
                val c = content[i]

                if (quote != ' ') {
                    if (c == '\\') i++
                    else if (c == quote) quote = ' '
                } else if (c == '"' || c == '\'') {
                    quote = c
                } else if (c == '-' && i + 1 < content.length && content[i + 1] == '-') {
                    val after = if (i + 2 < content.length) content[i + 2] else ' '
                    if (!isOperatorChar(after)) return content.substring(0, i)
                }

                i++
            }

            return content
        }

        fun isOperatorChar(c: Char) = c in "!#$%&*+-./<=>?@\\^|~:"

        // How wide the leading whitespace is in columns, with a tab going to the next tab stop -
        // which is what the compiler's own lexer does with one.
        fun columnWidth(content: String, indent: Int, tabSize: Int): Int {
            val tab = if (tabSize > 0) tabSize else 4
            var width = 0

            for (i in 0 until indent) {
                if (content[i] == '\t') width += tab - (width % tab) else width++
            }

            return width
        }

        fun indentOf(width: Int, useTabs: Boolean, tabSize: Int): String {
            if (!useTabs) return " ".repeat(width)
            return "\t".repeat(width / tabSize) + " ".repeat(width % tabSize)
        }
    }
}
