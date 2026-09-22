package se.rimmer.yana.run

import com.intellij.execution.BeforeRunTask
import com.intellij.execution.BeforeRunTaskProvider
import com.intellij.execution.configurations.RunConfiguration
import com.intellij.execution.runners.ExecutionEnvironment
import com.intellij.openapi.actionSystem.DataContext
import com.intellij.openapi.util.Key
import se.rimmer.yana.YanaIcons
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
 * `resolveOutputDirectory` are the same methods the run half calls to find the executable, so there
 * is still one answer. The build itself is YanaBuild, which the Build action runs as well.
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

        // Returning false is what stops the launch; the platform says nothing else about why, so
        // the Build window YanaBuild brings forward on a failure is where the diagnostics are - the
        // run never started, so there is no console of its own to print into.
        return YanaBuild.run(configuration)
    }
}
