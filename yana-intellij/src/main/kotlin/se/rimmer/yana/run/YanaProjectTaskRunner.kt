package se.rimmer.yana.run

import com.intellij.openapi.progress.ProgressIndicator
import com.intellij.openapi.progress.Task
import com.intellij.openapi.project.Project
import com.intellij.openapi.roots.ProjectModelBuildableElement
import com.intellij.task.ProjectModelBuildTask
import com.intellij.task.ProjectTask
import com.intellij.task.ProjectTaskContext
import com.intellij.task.ProjectTaskRunner
import com.intellij.task.TaskRunnerResults
import org.jetbrains.concurrency.AsyncPromise
import org.jetbrains.concurrency.Promise

/*
 * A Yana configuration, as something the platform's build machinery can be asked to build.
 *
 * The IDE's Build actions do not build run configurations: they hand `ProjectTaskManager` a list of
 * buildable elements, and it asks each registered `ProjectTaskRunner` which of them it can build.
 * This is the element, and the runner below is the one that says yes to it. What produces the
 * element is IDE-specific - in CLion it is `cidr.buildConfigurationProvider`, which is what the
 * hammer beside the run configuration asks (see se.rimmer.yana.clion) - so this half is written
 * against the platform alone and loads everywhere the rest of the plugin does.
 */
interface YanaBuildTarget : ProjectModelBuildableElement {
    val configuration: YanaRunConfiguration
}

/*
 * Build, without Run.
 *
 * Run already built first, through the before-run task, but that is the only way a build happened -
 * and a library, a JavaScript program and a binary for another operating system are all things that
 * are built and not run here. So this is the same build (YanaBuild), reached from Build instead.
 *
 * Registered `order="first"`: CLion's own runners claim broadly and the platform takes the first
 * runner that answers `canRun`, so a runner after them would never be asked.
 */
class YanaProjectTaskRunner : ProjectTaskRunner() {
    override fun canRun(projectTask: ProjectTask): Boolean =
        projectTask is ProjectModelBuildTask<*> && projectTask.buildableElement is YanaBuildTarget

    override fun run(project: Project, context: ProjectTaskContext, vararg tasks: ProjectTask): Promise<Result> {
        val targets = tasks.mapNotNull { (it as? ProjectModelBuildTask<*>)?.buildableElement as? YanaBuildTarget }
        val promise = AsyncPromise<Result>()

        // A promise rather than a result because the platform may ask on the EDT, and a build is a
        // subprocess to wait for - the same reason the before-run task exists at all.
        object : Task.Backgroundable(project, "Building ${targets.joinToString { it.configuration.name }}", true) {
            override fun run(indicator: ProgressIndicator) {
                // In order, and stopping at the first failure: a second build's output under the
                // first one's errors is noise, and the first is the one to fix.
                val built = targets.all { YanaBuild.run(it.configuration, showWindow = true) }
                promise.setResult(
                    when {
                        indicator.isCanceled -> TaskRunnerResults.ABORTED
                        built -> TaskRunnerResults.SUCCESS
                        else -> TaskRunnerResults.FAILURE
                    }
                )
            }

            override fun onCancel() {
                if (!promise.isDone) promise.setResult(TaskRunnerResults.ABORTED)
            }

            // YanaBuild catches its own failures, so this is the task machinery's - and a rejected
            // promise is reported by nobody, so it is resolved as a failure instead.
            override fun onThrowable(error: Throwable) {
                super.onThrowable(error)
                if (!promise.isDone) promise.setResult(TaskRunnerResults.FAILURE)
            }
        }.queue()

        return promise
    }
}
