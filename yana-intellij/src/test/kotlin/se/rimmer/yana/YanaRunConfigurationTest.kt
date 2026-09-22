package se.rimmer.yana

import com.intellij.build.BuildViewManager
import com.intellij.build.events.BuildEvent
import com.intellij.build.events.FailureResult
import com.intellij.build.events.FileMessageEvent
import com.intellij.build.events.FinishBuildEvent
import com.intellij.execution.RunManager
import com.intellij.openapi.application.ApplicationManager
import com.intellij.testFramework.PlatformTestUtil
import com.intellij.testFramework.replaceService
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import se.rimmer.yana.lsp.YanaSemanticTokens
import com.intellij.task.ProjectModelBuildTask
import com.intellij.task.impl.ProjectTaskList
import com.intellij.task.ProjectTaskManager
import com.intellij.task.ProjectTaskRunner
import com.jetbrains.cidr.execution.build.CidrBuildConfigurationProvider
import se.rimmer.yana.project.YanaProjectTemplate
import se.rimmer.yana.run.PROVIDER_ID
import se.rimmer.yana.run.YanaBuild
import se.rimmer.yana.run.YanaBuildTarget
import se.rimmer.yana.run.YanaDiagnostic
import se.rimmer.yana.run.YanaProjectFile
import se.rimmer.yana.run.YanaProjectTaskRunner
import se.rimmer.yana.run.YanaRunConfiguration
import se.rimmer.yana.run.YanaRunConfigurationType
import se.rimmer.yana.settings.YanaSettings
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
     * The driver's error stream, split into one Build-window entry per diagnostic.
     *
     * The format is compiler/compiler/diagnostics.cpp's, and it is text: a change there shows up
     * as a build whose tree says only "failed", so this pins the shape the parser expects.
     */
    fun testDiagnosticsAreSplitPerMessage() {
        val output = """
            /work/src/Main.yana:4:10: error: a `:` block here is a lens call's continuation
                let x: Int = "no"
                     ^
            C:\work\src\Other.yana:7:2: warning: unused
            2:0: : a note, with no file behind it
        """.trimIndent() + "\n"

        val diagnostics = YanaDiagnostic.parse(output)
        assertEquals(listOf("error", "warning", ""), diagnostics.map { it.level })
        assertEquals(listOf("/work/src/Main.yana", "C:\\work\\src\\Other.yana", null), diagnostics.map { it.path })
        assertEquals(listOf(4, 7, 2), diagnostics.map { it.line })
        assertEquals("a `:` block here is a lens call's continuation", diagnostics[0].text)
        assertTrue("the quoted line and marker belong to the first", diagnostics[0].detail.endsWith("     ^"))
        assertEquals("2:0: a note, with no file behind it", diagnostics[2].title)
    }

    /*
     * A whole build, through the Build window, against a stand-in compiler.
     *
     * Everything else here checks wiring; this is the one that runs YanaBuild, and it exists because
     * the first Build-window version passed every other test and failed on its first click - an
     * AssertionError out of `BuildProgress`, which only running the build finds. A plugin error is
     * logged with `error`, which fails a platform test, so a repeat of that fails here.
     *
     * What it asserts is what makes the window worth having: the diagnostic arrives as a message
     * that names its file, at the line and column the driver printed less one.
     */
    fun testBuildReportsDiagnosticsIntoTheBuildWindow() {
        val root = Files.createTempDirectory("yana-build").toFile()
        val settings = YanaSettings.getInstance()
        val previousCompiler = settings.compilerPath
        try {
            val source = File(root, "src/Main.yana")
            source.parentFile.mkdirs()
            source.writeText("fn main() -> Int = 0\n")
            File(root, "yana.toml").writeText("sources = [\"src\"]\noutput = \"app\"\n")

            // The stand-in prints what the driver prints for one located error, and fails.
            val compiler = File(root, "yana")
            compiler.writeText("#!/bin/sh\necho '${source.path}:3:5: error: nope' >&2\necho '  quoted' >&2\nexit 1\n")
            compiler.setExecutable(true)
            settings.compilerPath = compiler.path

            val events = mutableListOf<BuildEvent>()
            val view = object : BuildViewManager(project) {
                override fun onEvent(buildId: Any, event: BuildEvent) {
                    synchronized(events) { events += event }
                }
            }
            project.replaceService(BuildViewManager::class.java, view, testRootDisposable)

            val manager = RunManager.getInstance(project)
            val factory = YanaRunConfigurationType.getInstance().configurationFactories.first()
            val configuration = manager.createConfiguration("app", factory).configuration as YanaRunConfiguration
            configuration.projectFile = File(root, "yana.toml").path

            // Off the EDT, as both real callers are, while this thread pumps the events the build's
            // save-all waits on.
            val built = ApplicationManager.getApplication().executeOnPooledThread<Boolean> { YanaBuild.run(configuration) }
            assertFalse("the stand-in compiler fails, so the build should", PlatformTestUtil.waitForFuture(built, 30_000))

            val messages = synchronized(events) { events.filterIsInstance<FileMessageEvent>() }
            assertEquals("one diagnostic, as a message naming its file", 1, messages.size)
            assertEquals("nope", messages[0].message)
            assertEquals(source.canonicalFile, messages[0].filePosition.file!!.canonicalFile)
            assertEquals(2, messages[0].filePosition.startLine)
            assertEquals(4, messages[0].filePosition.startColumn)
            assertTrue("the build should finish as failed", synchronized(events) { events.any { it is FinishBuildEvent && it.result is FailureResult } })
        } finally {
            settings.compilerPath = previousCompiler
            root.deleteRecursively()
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
