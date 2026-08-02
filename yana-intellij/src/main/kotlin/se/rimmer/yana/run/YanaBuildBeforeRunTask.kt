package se.rimmer.yana.run

import com.intellij.execution.BeforeRunTask
import com.intellij.execution.BeforeRunTaskProvider
import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.execution.configurations.RunConfiguration
import com.intellij.execution.process.CapturingProcessHandler
import com.intellij.execution.runners.ExecutionEnvironment
import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.actionSystem.DataContext
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.util.Key
import se.rimmer.yana.YanaIcons
import se.rimmer.yana.settings.YanaCompilerLocator
import javax.swing.Icon

/*
 * The build, as a before-run task - Implementation-Tooling.md §16.
 *
 * This used to run inside `RunProfileState.startProcess`, and that was a real defect rather than a
 * style question: `startProcess` may be called on the EDT, and waiting on a subprocess there freezes
 * the UI. The platform says so out loud - `OSProcessHandler.checkEdtAndReadAction` logs a SEVERE
 * with a stack trace - and it was right. How fast the compile is does not enter into it.
 *
 * A before-run task is the mechanism built for this: the platform runs it on a background thread
 * with a progress indicator, it appears under "Before launch" in the run configuration where a user
 * expects to find it, and it can be turned off there without a checkbox of our own.
 *
 * The earlier objection to one - that it would be a second place deciding which binary was just
 * built - is answered by asking the configuration rather than repeating it: `resolveProjectFile` and
 * `resolveOutputDirectory` below are the same methods the run half calls to find the executable, so
 * there is still one answer.
 */
class YanaBuildBeforeRunTask : BeforeRunTask<YanaBuildBeforeRunTask>(PROVIDER_ID) {
    init {
        isEnabled = true
    }
}

// A top-level key rather than one inside the provider, so the task and the provider can both name it
// without either having to construct the other.
val PROVIDER_ID: Key<YanaBuildBeforeRunTask> = Key.create("Yana.BuildBeforeRun")

class YanaBuildBeforeRunTaskProvider : BeforeRunTaskProvider<YanaBuildBeforeRunTask>() {
    override fun getId(): Key<YanaBuildBeforeRunTask> = PROVIDER_ID
    override fun getName(): String = "Build with the Yana compiler"
    override fun getIcon(): Icon = YanaIcons.FILE
    override fun isConfigurable(): Boolean = false
    override fun isSingleton(): Boolean = true

    /// Non-null for a Yana configuration and null for everything else, which is what puts the task on
    /// new Yana configurations by default and keeps it off every other kind.
    override fun createTask(configuration: RunConfiguration): YanaBuildBeforeRunTask? =
        if (configuration is YanaRunConfiguration) YanaBuildBeforeRunTask() else null

    override fun executeTask(
        context: DataContext,
        configuration: RunConfiguration,
        environment: ExecutionEnvironment,
        task: YanaBuildBeforeRunTask,
    ): Boolean {
        if (configuration !is YanaRunConfiguration) return true

        /*
         * The editor's buffers, onto disk, before the compiler reads them.
         *
         * The compiler is a separate process reading files, so anything still only in a document is
         * invisible to it - and the symptom is not an error but a build of the *previous* edit, with
         * the next run picking up what the last one missed. Nothing else in the run flow does this
         * for a custom before-run task.
         *
         * `invokeAndWait` because saving is a write action and this method is on a background
         * thread, which is the same fact that lets the build below block.
         */
        ApplicationManager.getApplication().invokeAndWait {
            FileDocumentManager.getInstance().saveAllDocuments()
        }

        val compiler = YanaCompilerLocator.find()
        if (compiler == null) {
            report(
                environment,
                "The Yana compiler was not found. Put yana on PATH, or set its location in " +
                    "Settings | Languages & Frameworks | Yana.",
            )
            return false
        }

        val command = GeneralCommandLine(compiler.absolutePath)
        command.withParameters("-project", configuration.resolveProjectFile().absolutePath)
        command.withParameters("-to", configuration.resolveOutputDirectory().absolutePath)

        // Left out when the configuration does not set one, so that the project file's `target`
        // decides - which matters for more than the output format: `@platform` selects which
        // declarations exist, so the mode is part of what the program *is*.
        configuration.mode.trim().takeIf { it.isNotEmpty() }?.let { command.withParameters("-mode", it) }

        command.withWorkDirectory(configuration.buildWorkingDirectory())
        command.withCharset(Charsets.UTF_8)

        // This method is already off the EDT - that is the whole reason the build lives here - so
        // waiting for the process is allowed, and blocking is what "before run" means.
        val result = CapturingProcessHandler(command).runProcess()
        if (result.exitCode == 0) return true

        // Both streams: the driver reports diagnostics on one and its own failures on the other, and
        // which of the two holds the reason depends on how it failed.
        val message = result.stderr.trim().ifEmpty { result.stdout.trim() }
            .ifEmpty { "the Yana compiler exited with status ${result.exitCode}" }

        report(environment, message)
        return false
    }

    /*
     * A notification rather than a thrown exception.
     *
     * Returning false is what stops the launch; the platform says nothing else about why, so this is
     * the only place the driver's diagnostics reach the user. A balloon is the right weight for it -
     * the run never started, so there is no console to print into.
     */
    private fun report(environment: ExecutionEnvironment, message: String) {
        NotificationGroupManager.getInstance()
            .getNotificationGroup("Yana")
            .createNotification("Yana build failed", message, NotificationType.ERROR)
            .notify(environment.project)
    }
}
