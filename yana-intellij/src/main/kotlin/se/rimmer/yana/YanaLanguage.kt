package se.rimmer.yana

import com.intellij.lang.Language
import com.intellij.openapi.fileTypes.LanguageFileType
import com.intellij.openapi.util.IconLoader
import javax.swing.Icon

object YanaLanguage : Language("Yana") {
    private fun readResolve(): Any = YanaLanguage
    override fun getDisplayName() = "Yana"
    override fun isCaseSensitive() = true
}

object YanaIcons {
    @JvmField
    val FILE: Icon = IconLoader.getIcon("/icons/yana.svg", YanaIcons::class.java)
}

/*
 * The file type is what makes everything else reachable.
 *
 * Associating `.yana` with a Language is what gives the file a lexer, and the lexer is what a great
 * many editor features are built on - brace matching, the commenter, word selection, spell checking
 * that skips code, TODO scanning, "find in path" highlighting. None of those is an LSP feature and
 * none of them should stop working because the server is starting up, compiling, or dead.
 * Implementation-Tooling.md §9.
 */
object YanaFileType : LanguageFileType(YanaLanguage) {
    private fun readResolve(): Any = YanaFileType

    override fun getName() = "Yana"
    override fun getDescription() = "Yana source file"
    override fun getDefaultExtension() = "yana"
    override fun getIcon() = YanaIcons.FILE
}
