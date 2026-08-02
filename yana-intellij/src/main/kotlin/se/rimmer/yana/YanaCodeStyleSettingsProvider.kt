package se.rimmer.yana

import com.intellij.application.options.IndentOptionsEditor
import com.intellij.application.options.SmartIndentOptionsEditor
import com.intellij.lang.Language
import com.intellij.psi.codeStyle.CodeStyleSettingsCustomizable
import com.intellij.psi.codeStyle.CommonCodeStyleSettings
import com.intellij.psi.codeStyle.LanguageCodeStyleSettingsProvider

/*
 * What one indent level is - Implementation-Tooling.md §9.
 *
 * Without this the platform has no indent options for Yana and falls back to a default nobody chose,
 * which is invisible until something reads it: the backspace handler asks for the indent size, and
 * so do Tab, Enter and "Reformat". Registering it also puts Yana in
 * `Settings | Editor | Code Style`, so the size is the author's to change rather than this file's to
 * decide.
 *
 * Four spaces and no tabs, matching every `.yana` file in the tree. Tabs are a defensible choice in
 * a language whose layout is syntax and a bad default for one: the compiler's lexer expands a tab to
 * the next tab stop (`kTabWidth`), so a file mixing the two means something different depending on
 * what the reader's editor thinks a tab is.
 */
class YanaCodeStyleSettingsProvider : LanguageCodeStyleSettingsProvider() {
    override fun getLanguage(): Language = YanaLanguage

    override fun getIndentOptionsEditor(): IndentOptionsEditor = SmartIndentOptionsEditor()

    override fun customizeDefaults(
        commonSettings: CommonCodeStyleSettings,
        indentOptions: CommonCodeStyleSettings.IndentOptions,
    ) {
        indentOptions.INDENT_SIZE = 4
        indentOptions.CONTINUATION_INDENT_SIZE = 4
        indentOptions.TAB_SIZE = 4
        indentOptions.USE_TAB_CHARACTER = false
    }

    override fun customizeSettings(consumer: CodeStyleSettingsCustomizable, settingsType: SettingsType) {
        // Only the indent tab is shown. Everything else a code style page can offer - wrapping,
        // blank lines, spacing - is about a formatter, and there is none: reformatting a
        // layout-sensitive language by rules the compiler does not share is how a working file
        // stops compiling.
    }

    override fun getCodeSample(settingsType: SettingsType): String = """
        data Point {x: Int, y: Int}

        fn distance(&p: Point, q: Point) -> Int:
            let dx = p.x - q.x
            let dy = p.y - q.y
            return dx * dx + dy * dy
    """.trimIndent()
}
