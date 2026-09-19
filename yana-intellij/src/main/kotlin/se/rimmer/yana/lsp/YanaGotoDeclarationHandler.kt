package se.rimmer.yana.lsp

import com.intellij.codeInsight.navigation.actions.GotoDeclarationHandler
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.platform.lsp.api.LspServer
import com.intellij.platform.lsp.api.LspServerManager
import com.intellij.platform.lsp.util.getLsp4jPosition
import com.intellij.platform.lsp.util.getOffsetInDocument
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiManager
import org.eclipse.lsp4j.DefinitionParams
import org.eclipse.lsp4j.Location
import org.eclipse.lsp4j.Range
import se.rimmer.yana.YanaFileType

/**
 * Supplies the declaration action for Yana files.
 *
 * The platform LSP integration resolves references through its implicit-symbol provider. That
 * provider is deliberately conservative: it only participates when the current action is the
 * platform's navigation action and it drops a result when IntelliJ cannot turn its target into a
 * PSI symbol. Yana has a flat PSI file by design, and a definition can be outside the project
 * (the standard library), so the ordinary declaration handler is a better bridge here. The LSP
 * server remains the source of truth; this only adapts its target range to a PSI element.
 */
class YanaGotoDeclarationHandler : GotoDeclarationHandler {
    override fun getGotoDeclarationTargets(
        sourceElement: PsiElement?,
        offset: Int,
        editor: Editor,
    ): Array<PsiElement>? {
        val file = sourceElement?.containingFile ?: return null
        if (file.virtualFile?.fileType != YanaFileType) return null

        val project = file.project
        val sourceFile = file.virtualFile ?: return null
        val document = editor.document
        val server = findServer(project, sourceFile) ?: return null

        val params = DefinitionParams(
            server.getDocumentIdentifier(sourceFile),
            getLsp4jPosition(document, offset.coerceIn(0, document.textLength)),
        )

        val response = try {
            server.sendRequestSync(2000) { languageServer ->
                languageServer.textDocumentService.definition(params)
            }
        } catch (_: RuntimeException) {
            // The normal LSP provider will still get a chance if the server is restarting or the
            // synchronous action timed out. Navigation must not turn a transient server state into
            // a second IDE error.
            return null
        } ?: return null

        val targets = if (response.isLeft) {
            response.left.orEmpty().map { location ->
                Target(location.uri, location.range)
            }
        } else {
            response.right.orEmpty().map { link ->
                Target(link.targetUri, link.targetSelectionRange ?: link.targetRange)
            }
        }

        return targets.mapNotNull { target -> targetElement(project, server, target) }
            .toTypedArray()
            .takeUnless { it.isEmpty() }
    }

    private fun findServer(project: Project, file: VirtualFile): LspServer? =
        LspServerManager.getInstance(project)
            .getServersForProvider(YanaLspServerSupportProvider::class.java)
            .firstOrNull { it.descriptor.isSupportedFile(file) }

    private fun targetElement(project: Project, server: LspServer, target: Target): PsiElement? {
        val targetFile = server.descriptor.findFileByUri(target.uri) ?: return null
        val targetDocument = FileDocumentManager.getInstance().getDocument(targetFile) ?: return null
        val targetOffset = getOffsetInDocument(targetDocument, target.range.start) ?: return null
        val psiFile = PsiManager.getInstance(project).findFile(targetFile) ?: return null

        // The server's selection range is normally the declaration name. If an older server sends
        // the whole declaration, the first token still gives IntelliJ the right file and a stable
        // navigation target instead of reporting "cannot find declaration".
        return psiFile.findElementAt(targetOffset) ?: psiFile
    }

    private data class Target(val uri: String, val range: Range)
}
