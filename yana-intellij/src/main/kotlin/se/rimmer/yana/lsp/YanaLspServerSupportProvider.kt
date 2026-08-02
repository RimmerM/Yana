package se.rimmer.yana.lsp

import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.notification.NotificationAction
import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.options.ShowSettingsUtil
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.platform.lsp.api.LspServerSupportProvider
import com.intellij.platform.lsp.api.ProjectWideLspServerDescriptor
import com.intellij.platform.lsp.api.customization.LspCustomization
import com.intellij.platform.lsp.api.customization.LspSemanticTokensCustomizer
import se.rimmer.yana.YanaFileType
import se.rimmer.yana.settings.YanaConfigurable
import se.rimmer.yana.settings.YanaServerLocator
import se.rimmer.yana.settings.YanaSettings
import java.io.File

/*
 * The language server half - Implementation-Tooling.md §9, §10.
 *
 * The platform's own LSP API rather than LSP4IJ, because it is present in every commercial
 * JetBrains IDE and covers what this plan needs. It is registered from an optional configuration
 * file, so the lexical half of the plugin still loads in an IDE that has no LSP API - and syntax
 * highlighting, brace matching and the commenter keep working there.
 */
class YanaLspServerSupportProvider : LspServerSupportProvider {
    override fun fileOpened(
        project: Project,
        file: VirtualFile,
        serverStarter: LspServerSupportProvider.LspServerStarter,
    ) {
        if (file.fileType != YanaFileType) return
        if (!YanaSettings.getInstance().serverEnabled) return

        val executable = YanaServerLocator.find(project)
        if (executable == null) {
            reportMissingServer(project)
            return
        }

        serverStarter.ensureServerStarted(YanaLspServerDescriptor(project, executable))
    }

    // Not silence. The platform surfaces server status in its own tool window, but a server that was
    // never found never reaches it - so the one failure the user can actually fix gets a
    // notification with the settings page attached to it.
    private fun reportMissingServer(project: Project) {
        if (!reportedProjects.add(project.locationHash)) return

        val notification = NotificationGroupManager.getInstance()
            .getNotificationGroup("Yana")
            .createNotification(
                "Yana language server not found",
                "Put yana-lsp on PATH, or set its location in Settings | Languages & Frameworks | Yana. " +
                    "Syntax highlighting works without it; errors, hover and completion do not.",
                NotificationType.WARNING,
            )

        notification.addAction(NotificationAction.createSimple("Open settings") {
            ShowSettingsUtil.getInstance().showSettingsDialog(project, YanaConfigurable::class.java)
        })

        notification.notify(project)
    }

    private companion object {
        // One notification per project rather than one per file opened.
        val reportedProjects = java.util.concurrent.ConcurrentHashMap.newKeySet<String>()
    }
}

class YanaLspServerDescriptor(project: Project, private val executable: File) :
    ProjectWideLspServerDescriptor(project, "Yana") {

    override fun isSupportedFile(file: VirtualFile) = file.fileType == YanaFileType

    /*
     * The one thing the plugin customizes about the protocol - Implementation-Tooling.md §11.
     *
     * Through `lspCustomization` rather than the `lspSemanticTokensSupport` property beside it,
     * which is deprecated. Everything else on the customization is the platform's own default, and
     * that is the point of the LSP route: a capability the server declares gets wired up without
     * the plugin knowing it exists.
     */
    override val lspCustomization: LspCustomization = YanaCustomization

    override fun createCommandLine(): GeneralCommandLine {
        val command = GeneralCommandLine(executable.absolutePath)

        // The server finds the project the same way the driver does - a `yana.toml` at or above the
        // directory it is pointed at - and the client tells it where that is through `rootUri`, not
        // through the working directory. This is set anyway so that a relative path in a project
        // file means what it does on the command line.
        project.basePath?.let { command.withWorkDirectory(it) }
        command.withCharset(Charsets.UTF_8)

        return command
    }
}

private object YanaCustomization : LspCustomization() {
    override val semanticTokensCustomizer: LspSemanticTokensCustomizer = YanaSemanticTokens()
}
