package se.rimmer.yana.settings

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.components.PersistentStateComponent
import com.intellij.openapi.components.State
import com.intellij.openapi.components.Storage
import com.intellij.openapi.options.Configurable
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.SystemInfo
import com.intellij.ui.components.JBCheckBox
import com.intellij.ui.components.JBTextField
import com.intellij.util.ui.FormBuilder
import java.io.File
import javax.swing.JComponent
import javax.swing.JPanel

@State(name = "YanaSettings", storages = [Storage("yana.xml")])
class YanaSettings : PersistentStateComponent<YanaSettings.State> {
    class State {
        /// An explicit path to `yana-lsp`. Empty means "look for it", which is what §10 describes.
        @JvmField var serverPath: String = ""

        /// An explicit path to the `yana` driver, which is what a run configuration builds with
        /// (§16). Empty means the same search the server gets, in the other direction.
        @JvmField var compilerPath: String = ""

        /// Whether to start the language server at all. Off is a usable state: the lexer gives
        /// syntax highlighting, brace matching and the commenter without any server running.
        @JvmField var serverEnabled: Boolean = true
    }

    private var state = State()

    override fun getState() = state
    override fun loadState(next: State) { state = next }

    var serverPath: String
        get() = state.serverPath
        set(value) { state.serverPath = value }

    var serverEnabled: Boolean
        get() = state.serverEnabled
        set(value) { state.serverEnabled = value }

    var compilerPath: String
        get() = state.compilerPath
        set(value) { state.compilerPath = value }

    companion object {
        @JvmStatic
        fun getInstance(): YanaSettings = ApplicationManager.getApplication().getService(YanaSettings::class.java)
    }
}

/*
 * Finding `yana-lsp` - Implementation-Tooling.md §10.
 *
 * In order: the setting, then a `yana-lsp` beside the `yana` binary on PATH, then one on PATH
 * itself. When none is found the caller says so with an actionable notification rather than
 * failing quietly, because half of all "the plugin does not work" reports for an LSP-backed plugin
 * are a server nobody could find.
 */
// The directories of `PATH`, which is where both of the locators below look.
private fun pathEntries(): List<File> {
    val path = System.getenv("PATH") ?: return emptyList()
    return path.split(File.pathSeparator).filter { it.isNotEmpty() }.map { File(it) }
}

private val compilerExecutable = if (SystemInfo.isWindows) "yana.exe" else "yana"

object YanaServerLocator {
    private val executableName = if (SystemInfo.isWindows) "yana-lsp.exe" else "yana-lsp"
    private val compilerName = compilerExecutable

    fun find(project: Project?): File? {
        val configured = YanaSettings.getInstance().serverPath.trim()
        if (configured.isNotEmpty()) {
            val file = File(configured)
            return if (file.canExecute()) file else null
        }

        val entries = pathEntries()

        for (entry in entries) {
            val candidate = File(entry, executableName)
            if (candidate.canExecute()) return candidate
        }

        // The compiler and the server are built into the same directory, so finding one finds the
        // other - and a user who put `yana` on PATH did not mean to leave the server behind.
        for (entry in entries) {
            val compiler = File(entry, compilerName)
            if (!compiler.canExecute()) continue

            val sibling = File(compiler.parentFile, executableName)
            if (sibling.canExecute()) return sibling
        }

        return null
    }
}

/*
 * Finding the `yana` driver, which a run configuration builds with - Implementation-Tooling.md §16.
 *
 * The same search as the server's, run the other way round: the setting, then `PATH`, then a `yana`
 * beside a `yana-lsp` on it. The two are built into the same directory, so whichever of them the
 * user put on `PATH` locates both - and a run configuration that cannot find the compiler says so
 * in `checkConfiguration`, before the run rather than during it.
 */
object YanaCompilerLocator {
    private val serverName = if (SystemInfo.isWindows) "yana-lsp.exe" else "yana-lsp"

    fun find(): File? {
        val configured = YanaSettings.getInstance().compilerPath.trim()
        if (configured.isNotEmpty()) {
            val file = File(configured)
            return if (file.canExecute()) file else null
        }

        val entries = pathEntries()

        for (entry in entries) {
            val candidate = File(entry, compilerExecutable)
            if (candidate.canExecute()) return candidate
        }

        for (entry in entries) {
            val server = File(entry, serverName)
            if (!server.canExecute()) continue

            val sibling = File(server.parentFile, compilerExecutable)
            if (sibling.canExecute()) return sibling
        }

        return null
    }
}

class YanaConfigurable : Configurable {
    private val serverPath = JBTextField()
    private val compilerPath = JBTextField()
    private val serverEnabled = JBCheckBox("Start the Yana language server")
    private var panel: JPanel? = null

    override fun getDisplayName() = "Yana"

    override fun createComponent(): JComponent {
        val built = FormBuilder.createFormBuilder()
            .addComponent(serverEnabled)
            .addLabeledComponent("Language server path:", serverPath, 1, false)
            .addLabeledComponent("Compiler path:", compilerPath, 1, false)
            .addComponentFillVertically(JPanel(), 0)
            .panel

        panel = built
        return built
    }

    override fun isModified(): Boolean {
        val settings = YanaSettings.getInstance()
        return serverPath.text != settings.serverPath ||
            compilerPath.text != settings.compilerPath ||
            serverEnabled.isSelected != settings.serverEnabled
    }

    override fun apply() {
        val settings = YanaSettings.getInstance()
        settings.serverPath = serverPath.text.trim()
        settings.compilerPath = compilerPath.text.trim()
        settings.serverEnabled = serverEnabled.isSelected
    }

    override fun reset() {
        val settings = YanaSettings.getInstance()
        serverPath.text = settings.serverPath
        compilerPath.text = settings.compilerPath
        serverEnabled.isSelected = settings.serverEnabled
    }

    override fun disposeUIResources() {
        panel = null
    }
}
