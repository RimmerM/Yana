package se.rimmer.yana.run

import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.execution.process.CapturingProcessHandler
import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.diagnostic.ControlFlowException
import com.intellij.openapi.diagnostic.logger
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.progress.ProgressManager
import se.rimmer.yana.settings.YanaCompilerLocator

/*
 * One build of what a Yana configuration describes - shared by "Before launch" and by Build.
 *
 * There are two ways into a build: the before-run task, which builds so that Run has something to
 * start, and the Build action, which builds and stops - which is all a library, a JavaScript program
 * or a cross-compiled binary can ask for, since none of them is something this machine runs. Both
 * come here, so the flags a build is given and where its diagnostics go are decided once.
 *
 * The diagnostics go to a notification, not to the Build tool window. The Build window was tried and
 * broke both Run and Build: its event API is `@Experimental`, and between 2025.3 (compiled against)
 * and 2026.1 (installed) every builder changed signature and the `*Impl` events and
 * `DefaultBuildDescriptor` left the jar they were in. A plugin with no `until-build` cannot lean on
 * an API that moves like that. A notification is the API every JetBrains IDE has had unchanged.
 *
 * Blocking, and so never called on the EDT: both callers are already on a background thread.
 */
object YanaBuild {
    /// Builds, and says so: an error notification on a failure, and an information one on a success
    /// when `announceSuccess` - which Build asks for and Run does not, since there the program
    /// starting is the news.
    fun run(configuration: YanaRunConfiguration, announceSuccess: Boolean = false): Boolean {
        /*
         * Nothing thrown gets out of here.
         *
         * Both callers turn an exception into something worse than a failed build: the before-run
         * task never returns and Run spins forever, and the task runner rejects a promise the
         * platform does not report. That is exactly how the Build-window version failed, with a
         * `NoSuchMethodError`, so a failure of the plugin's own is a failed build with its reason.
         * Cancellation is the exception to the exception - it is how the platform stops a task.
         */
        return try {
            build(configuration, announceSuccess)
        } catch (e: Throwable) {
            if (e is ControlFlowException) throw e
            logger<YanaBuild>().error("The Yana build of ${configuration.name} failed inside the plugin", e)
            report(configuration, NotificationType.ERROR, "Yana build failed", e.toString())
            false
        }
    }

    private fun build(configuration: YanaRunConfiguration, announceSuccess: Boolean): Boolean {
        /*
         * The editor's buffers, onto disk, before the compiler reads them.
         *
         * The compiler is a separate process reading files, so anything still only in a document is
         * invisible to it - and the symptom is not an error but a build of the *previous* edit, with
         * the next build picking up what the last one missed.
         *
         * `invokeAndWait` because saving is a write action and this is on a background thread,
         * which is the same fact that lets the build below block.
         */
        ApplicationManager.getApplication().invokeAndWait {
            FileDocumentManager.getInstance().saveAllDocuments()
        }

        val projectFile = configuration.resolveProjectFile()
        if (!projectFile.isFile) {
            report(configuration, NotificationType.ERROR, "Yana build failed", "No project file at ${projectFile.path}.")
            return false
        }

        val command = commandLine(configuration)
        if (command == null) {
            report(
                configuration,
                NotificationType.ERROR,
                "Yana build failed",
                "The Yana compiler was not found. Put yana on PATH, or set its location in " +
                    "Settings | Languages & Frameworks | Yana.",
            )
            return false
        }

        // Under the caller's progress indicator where there is one - both callers have one - so that
        // cancelling the build in the status bar stops the compiler rather than only the bar.
        val handler = CapturingProcessHandler(command)
        val indicator = ProgressManager.getInstance().progressIndicator
        val result = if (indicator != null) handler.runProcessWithProgressIndicator(indicator) else handler.runProcess()
        if (result.isCancelled) return false

        if (result.exitCode == 0) {
            if (announceSuccess) {
                report(
                    configuration,
                    NotificationType.INFORMATION,
                    "Built ${configuration.name}",
                    "Into ${configuration.resolveOutputDirectory().path}",
                )
            }
            return true
        }

        // Both streams: the driver reports diagnostics on one and its own failures on the other, and
        // which of the two holds the reason depends on how it failed.
        val message = result.stderr.trim().ifEmpty { result.stdout.trim() }
            .ifEmpty { "the Yana compiler exited with status ${result.exitCode}" }

        report(configuration, NotificationType.ERROR, "Yana build failed", message)
        return false
    }

    private fun report(configuration: YanaRunConfiguration, type: NotificationType, title: String, message: String) {
        NotificationGroupManager.getInstance()
            .getNotificationGroup("Yana")
            .createNotification(title, message, type)
            .notify(configuration.project)
    }

    /// What the compiler is asked to do, or null when there is no compiler to ask.
    fun commandLine(configuration: YanaRunConfiguration): GeneralCommandLine? {
        val compiler = YanaCompilerLocator.find() ?: return null

        val command = GeneralCommandLine(compiler.absolutePath)
        command.withParameters("-project", configuration.resolveProjectFile().absolutePath)
        command.withParameters("-to", configuration.resolveOutputDirectory().absolutePath)

        // What to call it, so that the file this writes is the file the run half looks for - see
        // YanaRunConfiguration.resolveExecutableName. Left out when the project does not say enough
        // to know: a build can still go ahead under the compiler's own name, and it is only Run that
        // needs to know it in advance, which `checkConfiguration` refuses.
        configuration.resolveExecutableName()?.let { command.withParameters("-output", it) }

        // Left out when the configuration does not set one, so that the project file's `mode`
        // decides - which matters for more than the output format: `@platform` selects which
        // declarations exist, so the mode is part of what the program *is*.
        configuration.mode.trim().takeIf { it.isNotEmpty() }?.let { command.withParameters("-mode", it) }

        // The rewritten resolve stage for **every** build - the stage the language server answers
        // from, so what runs is the program the editor described.
        //
        // Still written out after the compiler's own default became `ownership` (2026-09-22): what
        // the fallback below is the fallback *for* is a project that names a stage, and a project
        // saying `resolver = "legacy"` is built the way it asks. Dropping the flag would leave that
        // project building one way from the terminal and another from the run button.
        //
        // JavaScript used to be left out of this, because it was the one target only the shipping
        // stage built and the driver refused the flag for it. JSIR changed that (2026-09-22, J75):
        // the rewrite builds a JavaScript program, the language server resolves a `platform = "js"`
        // project as one, and a run configuration that still asked the shipping stage would be
        // running a different program from the one whose errors the editor had been showing - which
        // is the single thing this line exists to prevent.
        val resolver = configuration.readProjectFile()?.resolver
            ?.trim()
            ?.takeIf { it.isNotEmpty() }
            ?: "ownership"
        command.withParameters("-resolver", resolver)

        command.withWorkDirectory(configuration.buildWorkingDirectory())
        command.withCharset(Charsets.UTF_8)
        return command
    }
}
