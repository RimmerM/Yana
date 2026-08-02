package se.rimmer.yana

import com.intellij.extapi.psi.ASTWrapperPsiElement
import com.intellij.extapi.psi.PsiFileBase
import com.intellij.lang.ASTNode
import com.intellij.lang.ParserDefinition
import com.intellij.lang.PsiParser
import com.intellij.lexer.Lexer
import com.intellij.openapi.project.Project
import com.intellij.psi.FileViewProvider
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiFile
import com.intellij.psi.tree.IFileElementType
import com.intellij.psi.tree.TokenSet
import se.rimmer.yana.lexer.YanaLexerAdapter

/*
 * A flat parser, and why there is one at all - Implementation-Tooling.md §9.
 *
 * §9 declines a native PSI implementation: a Grammar-Kit parser and a real tree would mean writing
 * Yana's front end twice, and the server already knows everything a tree would tell us. This is not
 * that. It builds **one node containing every token** - no grammar, no structure, nothing to keep in
 * step with the compiler - and exists only so that a `.yana` file *is* a Yana file to the platform.
 *
 * Without it the platform has no ParserDefinition for the language, so `PsiFile` for a `.yana`
 * buffer is a `PsiPlainTextFileImpl` whose language is PlainText. Enough of the editor works anyway
 * to be misleading: the lexer drives highlighting, brace *matching* and the quote handler, all of
 * which read the highlighter's tokens directly. What silently does not work is everything routed
 * through the PSI file's language - and auto-closing brackets is the one a user notices first.
 *
 * Established by test rather than by reading: `YanaTypingTest` types a bracket into a real editor
 * and looks at the document. Before this, all four bracket cases inserted nothing.
 */
class YanaParserDefinition : ParserDefinition {
    override fun createLexer(project: Project?): Lexer = YanaLexerAdapter()

    override fun createParser(project: Project?): PsiParser = PsiParser { root, builder ->
        val mark = builder.mark()
        while (!builder.eof()) builder.advanceLexer()
        mark.done(root)
        builder.treeBuilt
    }

    override fun getFileNodeType(): IFileElementType = FILE

    // Read by the platform rather than by us: the commenter and "spell check skips code" use the
    // comment set, and TypedHandler's quote handling consults the string set.
    override fun getCommentTokens(): TokenSet = YanaTokenTypes.COMMENTS
    override fun getStringLiteralElements(): TokenSet = YanaTokenTypes.STRINGS

    override fun createElement(node: ASTNode): PsiElement = ASTWrapperPsiElement(node)
    override fun createFile(viewProvider: FileViewProvider): PsiFile = YanaPsiFile(viewProvider)

    companion object {
        val FILE = IFileElementType(YanaLanguage)
    }
}

class YanaPsiFile(viewProvider: FileViewProvider) : PsiFileBase(viewProvider, YanaLanguage) {
    override fun getFileType() = YanaFileType
    override fun toString() = "Yana file"
}
