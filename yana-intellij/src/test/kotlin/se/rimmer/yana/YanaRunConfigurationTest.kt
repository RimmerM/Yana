package se.rimmer.yana

import com.intellij.execution.RunManager
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import se.rimmer.yana.lsp.YanaSemanticTokens
import com.intellij.task.ProjectModelBuildTask
import com.intellij.task.impl.ProjectTaskList
import com.intellij.task.ProjectTaskManager
import com.intellij.task.ProjectTaskRunner
import com.jetbrains.cidr.execution.build.CidrBuildConfigurationProvider
import se.rimmer.yana.project.YanaProjectTemplate
import se.rimmer.yana.run.PROVIDER_ID
import se.rimmer.yana.run.YanaBuildTarget
import se.rimmer.yana.run.YanaProjectFile
import se.rimmer.yana.run.YanaProjectTaskRunner
import se.rimmer.yana.run.YanaRunConfigurationType
import java.io.File
import java.nio.file.Files

/*
 * The parts of the plugin that are wiring rather than logic, and that failed silently when they
 * were wrong.
 *
 * Each of these asserts something that produced no error when it broke - a missing before-run task
 * looks like a Run button that does nothing, and a language the platform stops asking the server
 * about looks like colouring that quietly went away. Nothing logs; the only signal is a user
 * noticing. That is exactly what a test is for.
 */
class YanaRunConfigurationTest : BasePlatformTestCase() {
    /*
     * A new configuration builds before it runs, without anyone adding the step by hand.
     *
     * The platform asks every `BeforeRunTaskProvider` for a default task when a configuration is
     * created, and takes the ones that come back enabled. A configuration made *before* the provider
     * existed does not get one retroactively - which is why an older configuration needed the step
     * added manually, and why this asserts the new-configuration path rather than that one.
     */
    fun testNewConfigurationBuildsBeforeRunning() {
        val manager = RunManager.getInstance(project)
        val factory = YanaRunConfigurationType.getInstance().configurationFactories.first()
        val settings = manager.createConfiguration("yana", factory)

        val tasks = settings.configuration.beforeRunTasks
        val build = tasks.find { it.providerId == PROVIDER_ID }

        assertNotNull("a new Yana configuration should have the build step attached", build)
        assertTrue("the build step should be enabled", build!!.isEnabled)
    }

    /*
     * CLion's Build button builds the selected Yana configuration.
     *
     * Two registrations have to line up for it, and neither says anything when it does not: the
     * button is simply disabled when no `cidr.buildConfigurationProvider` answers, and a build
     * nobody's runner claims is dropped. So this walks the path the button walks - the providers,
     * then `ProjectTaskManager`'s build task, then the first runner that says yes.
     */
    fun testSelectedConfigurationIsBuildable() {
        val manager = RunManager.getInstance(project)
        val factory = YanaRunConfigurationType.getInstance().configurationFactories.first()
        val settings = manager.createConfiguration("app", factory)
        manager.addConfiguration(settings)
        manager.selectedConfiguration = settings

        val buildable = CidrBuildConfigurationProvider.EP_NAME.extensionList
            .flatMap { it.getBuildableConfigurations(project) }
            .filterIsInstance<YanaBuildTarget>()

        assertEquals("the selected configuration should be offered to the Build button", 1, buildable.size)
        assertSame(settings.configuration, buildable[0].configuration)

        val task = ProjectTaskManager.getInstance(project).createBuildTask(true, buildable[0])
        val tasks = if (task is ProjectTaskList) task.toList() else listOf(task)
        val build = tasks.filterIsInstance<ProjectModelBuildTask<*>>().single()

        val runner = ProjectTaskRunner.EP_NAME.extensionList.firstOrNull { it.canRun(project, build) }
        assertTrue("the Yana runner should be the one to build it, found $runner", runner is YanaProjectTaskRunner)
    }

    /*
     * A new project builds under its own name.
     *
     * Left to the compiler, the artifact is named after the main module, and the template's only
     * module is the source root - so every new project built as `Src`.
     */
    fun testNewProjectIsNamedAfterItself() {
        val parent = Files.createTempDirectory("yana-template").toFile()
        try {
            for (name in listOf("Game", "My \"quoted\" \\ game")) {
                val root = File(parent, name)
                YanaProjectTemplate.write(root, createSample = true)

                val file = YanaProjectFile.read(File(root, "yana.toml"))
                assertEquals(name, file!!.executableName())
            }
        } finally {
            parent.deleteRecursively()
        }
    }

    /*
     * The server is asked for semantic tokens for `.yana` files.
     *
     * `LspSemanticTokensSupport` only asks for languages whose id is `TEXT` or `textmate`, on the
     * assumption that a language with a PSI does its own highlighting. Yana's PSI is one flat node
     * and knows nothing, so everything past the lexer comes from the server. Adding a
     * `ParserDefinition` moved the file off `TEXT` and silently turned this off once already.
     */
    fun testSemanticTokensAreRequestedForYanaFiles() {
        myFixture.configureByText("Test.yana", "fn main() -> Int = 42\n")

        assertTrue(
            "the server must be asked for semantic tokens, or nothing past the lexer is coloured",
            YanaSemanticTokens().shouldAskServerForSemanticTokens(myFixture.file),
        )
    }

    /// The flat parser is in place - which is what makes brackets close, and what broke the check
    /// above. Asserted here so the two are visibly connected.
    fun testYanaFilesHaveTheirOwnLanguage() {
        myFixture.configureByText("Test.yana", "fn main() -> Int = 42\n")

        assertEquals(YanaLanguage, myFixture.file.language)
        assertEquals(YanaFileType, myFixture.file.fileType)
    }
}
