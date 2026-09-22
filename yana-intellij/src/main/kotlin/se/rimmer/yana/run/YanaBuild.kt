package se.rimmer.yana.run

import com.intellij.build.BuildViewManager
import com.intellij.build.DefaultBuildDescriptor
import com.intellij.build.FilePosition
import com.intellij.build.events.MessageEvent
import com.intellij.build.progress.BuildProgress
import com.intellij.build.progress.BuildProgressDescriptor
import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.execution.process.CapturingProcessHandler
import com.intellij.execution.process.ProcessEvent
import com.intellij.execution.process.ProcessListener
import com.intellij.execution.process.ProcessOutputType
import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.diagnostic.ControlFlowException
import com.intellij.openapi.diagnostic.logger
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.progress.ProgressManager
import com.intellij.openapi.util.Key
import se.rimmer.yana.settings.YanaCompilerLocator
import java.io.File

/*
 * One build of what a Yana configuration describes - shared by "Before launch" and by Build.
 *
 * There are two ways into a build: the before-run task, which builds so that Run has something to
 * start, and the Build action, which builds and stops - which is all a library, a JavaScript program
 * or a cross-compiled binary can ask for, since none of them is something this machine runs. Both
 * come here, so the flags a build is given and where its output goes are decided once.
 *
 * The output goes to the Build tool window, and **every API that reaches it is `@Experimental`**,
 * so the window is used with a fallback rather than relied on. The first version used the event
 * builders (`BuildEvents`): between 2025.3, which this compiles against, and 2026.1 every builder
 * changed signature and the `*Impl` events were deleted, and Run spun forever and Build did nothing
 * on a `NoSuchMethodError`. `BuildViewManager.createBuildProgress` is the layer above those - the
 * one the Gradle and Maven integrations write through - and it is identical in both releases, so
 * it is what BuildWindowReport uses. If it moves too, the `LinkageError` lands in `run`, which
 * builds again reporting through a notification: the diagnostics are harder to read there, but they
 * arrive, and nothing hangs. `verifyPlugin -PverifyIde=...` is what says whether that has happened.
 *
 * Blocking, and so never called on the EDT: both callers are already on a background thread.
 */
object YanaBuild {
    /// Builds, and reports into the Build window - which comes forward on a failure always, and on a
    /// success when `showWindow`: Build asks for that, since the window is the whole answer to it,
    /// and Run does not, since there the program starting is the news.
    fun run(configuration: YanaRunConfiguration, showWindow: Boolean = false): Boolean {
        /*
         * Nothing thrown gets out of here.
         *
         * Both callers turn an exception into something worse than a failed build: the before-run
         * task never returns and Run spins forever, and the task runner's promise is rejected with
         * nobody reporting it. Cancellation is the exception to the exception - it is how the
         * platform stops a task.
         */
        try {
            return try {
                build(configuration, BuildWindowReport(configuration, showWindow))
            } catch (e: LinkageError) {
                // The Build window's API moved under this plugin. Almost always before the compiler
                // started - the first call into it is the one that fails - so building again costs
                // nothing in the case that matters.
                logger<YanaBuild>().warn("The Build tool window's API has changed; reporting through notifications", e)
                build(configuration, NotificationReport(configuration, showWindow))
            }
        } catch (e: Throwable) {
            if (e is ControlFlowException) throw e
            logger<YanaBuild>().error("The Yana build of ${configuration.name} failed inside the plugin", e)
            notify(configuration, NotificationType.ERROR, "Yana build failed", e.toString())
            return false
        }
    }

    private fun build(configuration: YanaRunConfiguration, report: Report): Boolean {
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

        report.start()

        val projectFile = configuration.resolveProjectFile()
        if (!projectFile.isFile) return report.fail("No project file at ${projectFile.path}.")

        val command = commandLine(configuration)
            ?: return report.fail(
                "The Yana compiler was not found. Put yana on PATH, or set its location in " +
                    "Settings | Languages & Frameworks | Yana."
            )

        // The command line first, the way a run console starts: what was asked of the compiler is
        // the first thing to check when what came back is surprising.
        report.output(command.commandLineString + "\n", ProcessOutputType.SYSTEM)

        // Streamed as it arrives, so a long build shows where it is rather than all at the end.
        val handler = CapturingProcessHandler(command)
        handler.addProcessListener(object : ProcessListener {
            override fun onTextAvailable(event: ProcessEvent, outputType: Key<*>) {
                if (outputType == ProcessOutputType.SYSTEM) return
                report.output(event.text, outputType as? ProcessOutputType ?: ProcessOutputType.STDOUT)
            }
        })

        // Under the caller's progress indicator where there is one - both callers have one - so that
        // cancelling the build in the status bar stops the compiler rather than only the bar.
        val indicator = ProgressManager.getInstance().progressIndicator
        val result = if (indicator != null) handler.runProcessWithProgressIndicator(indicator) else handler.runProcess()

        if (result.isCancelled) {
            report.cancel()
            return false
        }

        val diagnostics = YanaDiagnostic.parse(result.stderr)
        diagnostics.forEach(report::diagnostic)

        if (result.exitCode == 0) {
            report.finish()
            return true
        }

        // A failure the driver reported without a located diagnostic - a missing library, an
        // unreadable project file - still gets said, so a failed build never says only "failed".
        if (diagnostics.none { it.isError }) {
            return report.fail(
                result.stderr.trim().ifEmpty { result.stdout.trim() }
                    .ifEmpty { "The Yana compiler exited with status ${result.exitCode}." }
            )
        }

        return report.fail(null)
    }

    /// Where a build's progress goes. Two of them, so that the Build window can be the one used
    /// without being the one depended on - see the comment on YanaBuild.
    private interface Report {
        fun start()
        fun output(text: String, type: ProcessOutputType)
        fun diagnostic(diagnostic: YanaDiagnostic)
        fun finish()
        fun cancel()

        /// Always false, so that `return report.fail(...)` reads as the failure it is. `reason` is
        /// null when the diagnostics already said why.
        fun fail(reason: String?): Boolean
    }

    /*
     * The Build tool window: the compiler's output as a console, and each diagnostic as its own
     * entry in the tree beside it, with the quoted line and its marker as the detail - which is
     * what makes twenty errors readable where one stream of them was not.
     */
    private class BuildWindowReport(
        private val configuration: YanaRunConfiguration,
        private val showWindow: Boolean,
    ) : Report {
        private lateinit var progress: BuildProgress<BuildProgressDescriptor>

        override fun start() {
            progress = BuildViewManager.createBuildProgress(configuration.project)

            val title = "Build ${configuration.name}"

            // An id of our own rather than `progress.id`: a root progress answers its id *from* the
            // descriptor it is started with, so asking for it before `start` is an AssertionError -
            // "The task was not started yet", which is what the first Build click reported.
            val descriptor = DefaultBuildDescriptor(
                Any(),
                title,
                configuration.buildWorkingDirectory(),
                System.currentTimeMillis(),
            )
            descriptor.isActivateToolWindowWhenAdded = showWindow
            descriptor.isActivateToolWindowWhenFailed = true

            progress.start(object : BuildProgressDescriptor {
                override fun getTitle() = title
                override fun getBuildDescriptor() = descriptor
            })
        }

        override fun output(text: String, type: ProcessOutputType) {
            progress.output(text, type)
        }

        // Warnings on a successful build too, since that is when they are missed.
        override fun diagnostic(diagnostic: YanaDiagnostic) {
            val kind = when (diagnostic.level) {
                "error" -> MessageEvent.Kind.ERROR
                "warning" -> MessageEvent.Kind.WARNING
                else -> MessageEvent.Kind.INFO
            }
            /*
             * A diagnostic that names its file is a `fileMessage`, which is what makes the entry
             * open the file at the line - and groups it under the file in the tree, the way a C++
             * build's are. The driver prints a path whenever it read the text from one.
             *
             * A relative path is relative to where the compiler ran, which is the build's working
             * directory; the driver is given an absolute project file, so in practice it prints
             * absolute ones. Both numbers are 1-based there and 0-based in a `FilePosition`.
             */
            val file = diagnostic.path?.let { written ->
                val path = File(written)
                if (path.isAbsolute) path else File(configuration.buildWorkingDirectory(), written)
            }

            if (file == null) {
                progress.message(diagnostic.title, diagnostic.detail, kind, null)
                return
            }

            val at = FilePosition(file, diagnostic.line - 1, diagnostic.column - 1)
            progress.fileMessage(diagnostic.text, diagnostic.detail, kind, at)
        }

        override fun finish() {
            progress.finish()
        }

        override fun cancel() {
            progress.cancel()
        }

        override fun fail(reason: String?): Boolean {
            if (reason != null) progress.message(reason.lineSequence().first(), reason, MessageEvent.Kind.ERROR, null)
            progress.fail()
            return false
        }
    }

    /*
     * The fallback: what the plugin did before it had a Build window - the diagnostics in a
     * notification when a build fails, since the run never started and there is no console of its
     * own to print into. Names no class of the Build window, which is the whole point of it.
     */
    private class NotificationReport(
        private val configuration: YanaRunConfiguration,
        private val showWindow: Boolean,
    ) : Report {
        private val diagnostics = StringBuilder()

        override fun start() {}
        override fun output(text: String, type: ProcessOutputType) {}

        override fun diagnostic(diagnostic: YanaDiagnostic) {
            diagnostics.append(diagnostic.detail).append('\n')
        }

        override fun finish() {
            if (showWindow) notify(configuration, NotificationType.INFORMATION, "Built ${configuration.name}", "")
        }

        override fun cancel() {}

        override fun fail(reason: String?): Boolean {
            notify(configuration, NotificationType.ERROR, "Yana build failed", reason ?: diagnostics.toString().trim())
            return false
        }
    }

    private fun notify(configuration: YanaRunConfiguration, type: NotificationType, title: String, message: String) {
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
