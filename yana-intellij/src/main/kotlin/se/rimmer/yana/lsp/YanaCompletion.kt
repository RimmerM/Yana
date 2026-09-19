package se.rimmer.yana.lsp

import com.intellij.codeInsight.completion.CompletionParameters
import com.intellij.codeInsight.completion.InsertionContext
import com.intellij.codeInsight.lookup.LookupElement
import com.intellij.codeInsight.lookup.LookupElementBuilder
import com.intellij.codeInsight.template.Template
import com.intellij.codeInsight.template.TemplateManager
import com.intellij.codeInsight.template.impl.ConstantNode
import com.intellij.openapi.project.Project
import com.intellij.platform.lsp.api.customization.LspCompletionSupport
import org.eclipse.lsp4j.CompletionItem
import org.eclipse.lsp4j.InsertTextFormat

/**
 * CLion's LSP decorator calls the lookup element's insert handler and then its own snippet handler.
 * A Yana snippet has one insertion owner: this handler. Marking the item plain text after capturing
 * the snippet stops the decorator's second pass, while retaining the normal LSP lookup and ranking.
 */
class YanaCompletion : LspCompletionSupport() {
    override fun createLookupElement(parameters: CompletionParameters, item: CompletionItem): LookupElement? {
        val snippet = item.insertText
        if (item.insertTextFormat != InsertTextFormat.Snippet || snippet == null) {
            return super.createLookupElement(parameters, item)
        }

        item.insertTextFormat = InsertTextFormat.PlainText
        // The default builder uses insertText as its lookup string, so its provisional insertion
        // writes raw `${1:name}` into the document. Use the visible name for that first pass; the
        // handler below owns the completed call and all of its editable arguments.
        val lookup = LookupElementBuilder.create(item, item.label)
        val filtered = item.filterText?.takeIf { it != item.label }
            ?.let { lookup.withLookupString(it) } ?: lookup
        return filtered.withInsertHandler { context, _ -> insertSnippet(context, snippet) }
    }

    private fun insertSnippet(context: InsertionContext, snippet: String) {
        val editor = context.editor
        val document = context.document
        val start = context.startOffset
        val end = context.tailOffset
        val template = YanaSnippetTemplate.create(context.project, snippet)

        // Completion has provisionally inserted the lookup string. Replace that exact span, and
        // move the caret to its start before runTemplate, which inserts at the editor's caret.
        document.replaceString(start, end, "")
        editor.caretModel.moveToOffset(start)
        val inserted = document.createRangeMarker(start, start).apply { isGreedyToRight = true }
        try {
            TemplateManager.getInstance(context.project).runTemplate(editor, template)
        } catch (failure: RuntimeException) {
            // Preserve compilable source if IntelliJ rejects a template at insertion time.
            document.replaceString(start, inserted.endOffset, YanaSnippetTemplate.plainText(snippet))
            throw failure
        } finally {
            inserted.dispose()
        }
    }
}

/** The subset of LSP snippets emitted by yana-lsp: numbered placeholders with optional defaults. */
internal object YanaSnippetTemplate {
    private sealed interface Part {
        data class Text(val value: String) : Part
        data class Field(val number: Int, val default: String) : Part
    }

    fun create(project: Project, snippet: String): Template {
        // The two-argument overload creates an empty, mutable template. The three-argument
        // overload's third argument is a group name, not template text.
        val template = TemplateManager.getInstance(project).createTemplate("", "")
        val declared = hashSetOf<Int>()
        var hasEnd = false

        for (part in parse(snippet)) {
            when (part) {
                is Part.Text -> template.addTextSegment(part.value)
                is Part.Field -> {
                    if (part.number == 0) {
                        template.addEndVariable()
                        hasEnd = true
                    } else {
                        val name = part.number.toString()
                        if (declared.add(part.number)) {
                            val value = ConstantNode(part.default)
                            // On a mutable template this overload creates both the variable and
                            // its segment. The string overload only creates the variable.
                            template.addVariable(name, value, value, true, false)
                        } else {
                            template.addVariableSegment(name)
                        }
                    }
                }
            }
        }

        if (!hasEnd) template.addEndVariable()
        template.setInline(false)
        return template
    }

    fun plainText(snippet: String): String = buildString {
        for (part in parse(snippet)) {
            when (part) {
                is Part.Text -> append(part.value)
                is Part.Field -> if (part.number != 0) append(part.default)
            }
        }
    }

    private fun parse(snippet: String): List<Part> {
        val parts = mutableListOf<Part>()
        val literal = StringBuilder()
        var at = 0

        fun flush() {
            if (literal.isNotEmpty()) {
                parts += Part.Text(literal.toString())
                literal.clear()
            }
        }

        while (at < snippet.length) {
            if (snippet[at] == '\\' && at + 1 < snippet.length) {
                literal.append(snippet[at + 1])
                at += 2
                continue
            }

            val field = if (snippet[at] == '$') fieldAt(snippet, at) else null
            if (field != null) {
                flush()
                parts += field.first
                at = field.second
            } else {
                literal.append(snippet[at])
                at++
            }
        }

        flush()
        return parts
    }

    private fun fieldAt(snippet: String, start: Int): Pair<Part.Field, Int>? {
        var at = start + 1
        if (at == snippet.length) return null
        val braced = snippet[at] == '{'
        if (braced) at++
        val digits = at
        while (at < snippet.length && snippet[at].isDigit()) at++
        if (at == digits) return null
        val number = snippet.substring(digits, at).toIntOrNull() ?: return null

        var default = ""
        if (braced && at < snippet.length && snippet[at] == ':') {
            val end = snippet.indexOf('}', at + 1)
            if (end < 0) return null
            default = snippet.substring(at + 1, end)
            at = end
        }
        if (braced) {
            if (at == snippet.length || snippet[at] != '}') return null
            at++
        }
        return Part.Field(number, default) to at
    }
}
