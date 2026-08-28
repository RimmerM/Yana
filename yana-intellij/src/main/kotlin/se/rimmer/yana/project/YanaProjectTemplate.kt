package se.rimmer.yana.project

import com.intellij.execution.RunManager
import com.intellij.openapi.project.Project
import se.rimmer.yana.run.YanaRunConfigurationType
import java.io.File

/*
 * What a new Yana project is - Implementation-Tooling.md §12.
 *
 * Shared by the two entry points that create one, because there are two New Project dialogs in the
 * JetBrains IDEs and a language that wants to appear in both has to register with both (see
 * YanaDirectoryProjectGenerator). Two copies of "what goes in a new project" is two things to keep
 * in step, and the first thing to drift would be the sample module the run configuration expects.
 *
 * It writes a project file and *nothing else about the build*. It does not generate a
 * `CMakeLists.txt` wrapping the compiler: §12 says the plugin should not, and it reads as a shortcut
 * that makes the plugin responsible for build configuration it does not own.
 */
object YanaProjectTemplate {
    fun write(root: File, createSample: Boolean) {
        root.mkdirs()
        File(root, "src").mkdirs()

        File(root, "yana.toml").writeText(PROJECT_FILE)
        File(root, ".gitignore").writeText(GIT_IGNORE)

        if (createSample) File(root, "src/Main.yana").writeText(SAMPLE_MODULE)
    }

    /*
     * The run configuration, created rather than left to be written by hand.
     *
     * It needs no settings - every field of it defaults to what the project file says (§16) - so
     * creating one costs nothing and is the difference between a new project that runs and one that
     * asks the author to work out what a Yana run configuration is first.
     */
    fun addRunConfiguration(project: Project, name: String) {
        val manager = RunManager.getInstance(project)
        val factory = YanaRunConfigurationType.getInstance().configurationFactories.first()
        // `createConfiguration` asks every BeforeRunTaskProvider for a default task, so the build
        // step is attached here without this having to name it - see YanaBuildBeforeRunTaskProvider.
        val settings = manager.createConfiguration(name, factory)

        manager.addConfiguration(settings)
        manager.selectedConfiguration = settings
    }

    /*
     * There is deliberately no `main` here.
     *
     * A directory is a module, so the one module this project has is `Src` - the source root - and
     * `src/Main.yana` is a *file* of it rather than a module called `Main`. The template used to
     * write `root = "Main"` - the key `main` is now spelled after what it names - which named
     * nothing and made every generated project fail on its first build. A project with exactly one
     * module needs no entry named at all: the module is the program.
     *
     * Add one when the project grows a second module, which is also when the compiler starts asking
     * for it by name.
     */
    private val PROJECT_FILE = """
        # What this program is: where its source lives, what it is built for, and where the build
        # goes. The compiler and the language server both read this file, so the editor and the
        # build agree about which files are in the program.
        #
        # Add `main = "<module>"` when there is more than one module and one of them is the program.
        sources = ["src"]
        platform = "native"
        to = "build"
    """.trimIndent() + "\n"

    private val SAMPLE_MODULE = """
        -- The program is entered through `main`. This file is part of the module `Src`, which is
        -- the source directory it sits in - a directory is a module.

        data Greeting {times: Int}

        fn total(g: Greeting) -> Int = g.times * 2

        fn main() -> Int:
            let g = Greeting {times: 21}
            return total(g)
    """.trimIndent() + "\n"

    private val GIT_IGNORE = """
        # Where `to` in yana.toml sends the build.
        build/
    """.trimIndent() + "\n"
}
