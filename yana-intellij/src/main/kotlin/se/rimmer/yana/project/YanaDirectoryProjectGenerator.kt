package se.rimmer.yana.project

import com.intellij.openapi.module.Module
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.platform.DirectoryProjectGeneratorBase
import com.intellij.platform.GeneratorPeerImpl
import com.intellij.platform.ProjectGeneratorPeer
import com.intellij.ui.components.JBCheckBox
import com.intellij.util.ui.FormBuilder
import se.rimmer.yana.YanaIcons
import java.io.File
import javax.swing.Icon

/*
 * The New Project entry CLion actually shows - Implementation-Tooling.md §12.
 *
 * There are two New Project dialogs in the JetBrains IDEs and they read different extension points.
 * The IDEA-style unified wizard is built from `newProjectWizard.languageGenerator`
 * (see YanaNewProjectWizard); the dialog CLion shows is `AbstractNewProjectDialog`, and its list is
 * built from `directoryProjectGenerator` - which is where "C++ Executable" and "C++ Library" come
 * from, and every other entry a CLion user has ever seen there.
 *
 * So a language that wants to appear in both registers twice. That is not a workaround: it is what
 * CLion's own bundled Rust support does, and checking `intellij-rust.jar` for
 * `RsDirectoryProjectGenerator` beside `RsNewProjectWizard` is what established it. A plugin that
 * registers only the wizard compiles, loads, logs nothing, and silently is not in the list.
 *
 * What it writes is what the wizard writes, and both go through YanaProjectTemplate so there is one
 * answer to "what is in a new Yana project" rather than two that drift.
 */
class YanaDirectoryProjectGenerator : DirectoryProjectGeneratorBase<YanaProjectSettings>() {
    override fun getName(): String = "Yana"
    override fun getLogo(): Icon = YanaIcons.FILE

    override fun getDescription(): String =
        "A yana.toml naming the root module and its sources, a module to start in, and a run " +
            "configuration that builds with the Yana compiler and runs what it produced."

    override fun createPeer(): ProjectGeneratorPeer<YanaProjectSettings> {
        val settings = YanaProjectSettings()
        val sample = JBCheckBox("Create a sample module", settings.createSample)

        // The settings object the dialog hands back to generateProject is this same instance, so the
        // listener is the whole of the binding - there is one control and one field.
        sample.addActionListener { settings.createSample = sample.isSelected }

        val panel = FormBuilder.createFormBuilder()
            .addComponent(sample)
            .addTooltip("Writes src/Main.yana with a program that returns 42, so that Run works before anything has been typed.")
            .panel

        return GeneratorPeerImpl(settings, panel)
    }

    override fun generateProject(
        project: Project,
        baseDir: VirtualFile,
        settings: YanaProjectSettings,
        module: Module,
    ) {
        // The directory already exists and is already the project root - that is the difference from
        // the wizard path, which is handed a name and a location and has to join them itself.
        YanaProjectTemplate.write(File(baseDir.path), settings.createSample)
        baseDir.refresh(false, true)

        YanaProjectTemplate.addRunConfiguration(project, baseDir.name)
    }
}

/// The one thing a new Yana project has to decide. See YanaProjectTemplate for why there is only one.
class YanaProjectSettings {
    var createSample: Boolean = true
}
