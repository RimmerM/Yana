package se.rimmer.yana.run

import com.intellij.execution.ExecutionException
import com.intellij.execution.Executor
import com.intellij.execution.configurations.CommandLineState
import com.intellij.execution.configurations.ConfigurationFactory
import com.intellij.execution.configurations.ConfigurationType
import com.intellij.execution.configurations.ConfigurationTypeBase
import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.execution.configurations.LocatableConfigurationBase
import com.intellij.execution.configurations.RunConfiguration
import com.intellij.execution.configurations.LocatableRunConfigurationOptions
import com.intellij.execution.configurations.RunProfileState
import com.intellij.execution.configurations.RuntimeConfigurationError
import com.intellij.execution.process.KillableColoredProcessHandler
import com.intellij.execution.process.ProcessHandler
import com.intellij.execution.process.ProcessTerminatedListener
import com.intellij.execution.runners.ExecutionEnvironment
import com.intellij.openapi.options.SettingsEditor
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.SystemInfo
import com.intellij.openapi.util.io.FileUtil
import com.intellij.util.execution.ParametersListUtil
import se.rimmer.yana.YanaIcons
import se.rimmer.yana.settings.YanaCompilerLocator
import java.io.File

/*
 * The Yana run configuration - Implementation-Tooling.md §16.
 *
 * CLion is CMake-shaped and Yana is not (§12), so this is the supported route for a project the IDE
 * does not build itself: one step that shells out to the Yana driver, and one that runs what it
 * produced. Nothing here reimplements a build system - `yana.toml` already says what the program is,
 * and the driver already knows how to read it, so the configuration reads the same file rather than
 * asking the user to restate any of it.
 *
 * The *debug* half of §16 is deliberately absent. It is built on CLion's own GDB/LLDB driver over
 * the DWARF the LLVM backend does not emit yet, so a debug configuration today would attach, stop
 * nowhere and show nothing. It lands with M10, together with the `.yana` breakpoint type that gates
 * it.
 */
class YanaRunConfigurationOptions : LocatableRunConfigurationOptions() {
    /// The `yana.toml` this configuration builds. Empty means the one at the project root, which is
    /// what a single-program project has and what the new-project template creates.
    var projectFile by string("")

    /// Where the driver writes its output. Empty means whatever `output` in the project file says,
    /// and `build/` beside it when it says nothing - see resolveOutputDirectory.
    var outputDirectory by string("")

    /// The compile mode, spelled as the driver's `-mode` spells it. Empty means the project file's
    /// own `target`, which is the answer for every project that has one.
    var mode by string("")

    var programArguments by string("")
    var workingDirectory by string("")
}

class YanaRunConfiguration(project: Project, factory: ConfigurationFactory, name: String) :
    LocatableConfigurationBase<YanaRunConfigurationOptions>(project, factory, name) {

    public override fun getOptions() = super.getOptions() as YanaRunConfigurationOptions

    var projectFile: String
        get() = options.projectFile.orEmpty()
        set(value) { options.projectFile = value }

    var outputDirectory: String
        get() = options.outputDirectory.orEmpty()
        set(value) { options.outputDirectory = value }

    var mode: String
        get() = options.mode.orEmpty()
        set(value) { options.mode = value }

    var programArguments: String
        get() = options.programArguments.orEmpty()
        set(value) { options.programArguments = value }

    var workingDirectory: String
        get() = options.workingDirectory.orEmpty()
        set(value) { options.workingDirectory = value }

    private fun projectBase() = File(project.basePath ?: FileUtil.getTempDirectory())

    /// The project file this configuration acts on. A relative path is relative to the IDE project,
    /// which is what makes a checked-in run configuration mean the same thing on another machine.
    fun resolveProjectFile(): File {
        val configured = projectFile.trim()
        if (configured.isEmpty()) return File(projectBase(), "yana.toml")

        val file = File(configured)
        return if (file.isAbsolute) file else File(projectBase(), configured)
    }

    fun readProjectFile(): YanaProjectFile? = YanaProjectFile.read(resolveProjectFile())

    /*
     * Where the build writes, and therefore where the executable is.
     *
     * The driver writes into the working directory when nothing says otherwise, which is not what an
     * IDE means - so the configuration always passes `-to`. What it passes is the project file's
     * `to` when it has one, so naming the directory here does not override what the project already
     * said; `build/` beside the project file is the fallback, and it is what the template creates a
     * `.gitignore` entry for.
     */
    fun resolveOutputDirectory(): File {
        val configured = outputDirectory.trim()
        if (configured.isNotEmpty()) {
            val file = File(configured)
            return if (file.isAbsolute) file else File(projectBase(), configured)
        }

        val project = readProjectFile()
        project?.to?.let { return it }

        val directory = project?.directory ?: resolveProjectFile().parentFile ?: projectBase()
        return File(directory, "build")
    }

    /*
     * What the program is called, or null when the project file does not say and there is more than
     * one module to choose between.
     *
     * The build passes this to the compiler as `-output` and the run half looks for exactly it, so the
     * two agree by construction. They used to agree by coincidence: the driver named the artifact
     * after the *module* it had resolved as the root, and this predicted that name from the file
     * layout - which is a different rule, and a wrong one for the layout this plugin's own template
     * generates. A project that names an `output` or a `main` still gets that name, since the
     * compiler would resolve the same one.
     */
    fun resolveExecutableName(): String? = readProjectFile()?.executableName()

    /// The executable the driver will have written, under the name above.
    fun resolveExecutable(): File? {
        val name = resolveExecutableName() ?: return null
        return File(resolveOutputDirectory(), if (SystemInfo.isWindows) "$name.exe" else name)
    }

    override fun checkConfiguration() {
        val file = resolveProjectFile()
        if (!file.isFile) {
            throw RuntimeConfigurationError(
                "No project file at ${file.path}. Yana reads which files are in a program from a " +
                    "yana.toml; create one, or point this configuration at the one you have."
            )
        }

        if (resolveExecutable() == null) {
            throw RuntimeConfigurationError(
                "${file.name} does not name a main module and the project has more than one, so " +
                    "there is no way to tell what the program is called. Add `main = \"App\"` to it."
            )
        }

        if (YanaCompilerLocator.find() == null) {
            throw RuntimeConfigurationError(
                "The Yana compiler was not found. Put yana on PATH, or set its location in " +
                    "Settings | Languages & Frameworks | Yana."
            )
        }
    }

    /// Where both halves run: the compiler for the build, and the program for the run. On the
    /// configuration rather than in the state, because the before-run task needs it as well.
    fun buildWorkingDirectory(): String =
        workingDirectory.trim().ifEmpty {
            resolveProjectFile().parentFile?.path ?: project.basePath ?: FileUtil.getTempDirectory()
        }

    override fun getConfigurationEditor(): SettingsEditor<out RunConfiguration> = YanaRunConfigurationEditor()

    override fun getState(executor: Executor, environment: ExecutionEnvironment): RunProfileState =
        YanaRunState(this, environment)
}

/*
 * Running what the build produced.
 *
 * The build itself is a before-run task (see YanaBuildBeforeRunTask) rather than something this
 * does first. It has to be: `startProcess` may be called on the EDT, and waiting on a subprocess
 * there freezes the UI - the platform logs a SEVERE for it, which is how the original arrangement
 * was found to be wrong.
 */
private class YanaRunState(
    private val configuration: YanaRunConfiguration,
    environment: ExecutionEnvironment,
) : CommandLineState(environment) {

    override fun startProcess(): ProcessHandler {
        val executable = configuration.resolveExecutable()
            ?: throw ExecutionException(
                "The project file does not name a main module, so there is no way to tell which " +
                    "executable to run. Add `main = \"App\"` to ${configuration.resolveProjectFile().name}."
            )

        if (!executable.isFile) {
            throw ExecutionException(
                "There is no executable at ${executable.path}. If the build was turned off under " +
                    "\"Before launch\", turn it back on; if the project writes elsewhere, set the " +
                    "output directory on this configuration."
            )
        }

        val command = GeneralCommandLine(executable.absolutePath)
        command.withParameters(ParametersListUtil.parse(configuration.programArguments))
        command.withWorkDirectory(configuration.buildWorkingDirectory())
        command.withCharset(Charsets.UTF_8)

        val handler = KillableColoredProcessHandler(command)
        ProcessTerminatedListener.attach(handler, environment.project)
        return handler
    }
}

/*
 * The type, and its one factory.
 *
 * `Yana Application` rather than `Yana`: the name is what appears under `Run | Edit Configurations |
 * +`, next to `C/C++ Application` and the rest, and a bare language name there says nothing about
 * what the configuration does.
 */
class YanaRunConfigurationType : ConfigurationTypeBase(
    ID,
    "Yana Application",
    "Builds a Yana project with the Yana driver and runs the executable it produced",
    YanaIcons.FILE,
) {
    init {
        addFactory(object : ConfigurationFactory(this) {
            override fun getId() = "Yana"

            override fun createTemplateConfiguration(project: Project): RunConfiguration =
                YanaRunConfiguration(project, this, "Yana")

            override fun getOptionsClass() = YanaRunConfigurationOptions::class.java
        })
    }

    companion object {
        const val ID = "YanaRunConfiguration"

        @JvmStatic
        fun getInstance(): YanaRunConfigurationType =
            ConfigurationType.CONFIGURATION_TYPE_EP.findExtensionOrFail(YanaRunConfigurationType::class.java)
    }
}
