package se.rimmer.yana.run

import com.intellij.openapi.fileChooser.FileChooserDescriptorFactory
import com.intellij.openapi.options.SettingsEditor
import com.intellij.openapi.ui.TextFieldWithBrowseButton
import com.intellij.ui.components.JBTextField
import com.intellij.util.ui.FormBuilder
import javax.swing.JComponent
import javax.swing.JPanel

/*
 * The configuration's own panel.
 *
 * Every field is optional, and that is the point: a project with a `yana.toml` at its root needs
 * none of them filled in, because the project file already answers what would go in each one. What
 * is here is for the projects that are not that - a workspace with two programs in it, a build
 * directory somewhere else, a program that takes arguments.
 *
 * Whether to build first is deliberately absent: that is a before-run task, so it belongs in the
 * platform's own "Before launch" list at the bottom of this dialog rather than in a checkbox of ours
 * that would say the same thing twice.
 */
class YanaRunConfigurationEditor : SettingsEditor<YanaRunConfiguration>() {
    private val projectFile = TextFieldWithBrowseButton()
    private val outputDirectory = TextFieldWithBrowseButton()
    private val workingDirectory = TextFieldWithBrowseButton()
    private val programArguments = JBTextField()
    private val mode = JBTextField()

    init {
        // The title and the description belong to the descriptor rather than to the listener; the
        // overload that took them separately is deprecated.
        projectFile.addBrowseFolderListener(
            null,
            FileChooserDescriptorFactory.createSingleFileDescriptor("toml")
                .withTitle("Yana Project File")
                .withDescription("The yana.toml this configuration builds"),
        )

        outputDirectory.addBrowseFolderListener(
            null,
            FileChooserDescriptorFactory.createSingleFolderDescriptor()
                .withTitle("Yana Output Directory")
                .withDescription("Where the compiler writes the executable"),
        )

        workingDirectory.addBrowseFolderListener(
            null,
            FileChooserDescriptorFactory.createSingleFolderDescriptor()
                .withTitle("Working Directory")
                .withDescription("The directory the program is started in"),
        )
    }

    override fun createEditor(): JComponent = FormBuilder.createFormBuilder()
        .addLabeledComponent("Project file:", projectFile, 1, false)
        .addTooltip("Empty uses yana.toml at the project root.")
        .addLabeledComponent("Output directory:", outputDirectory, 1, false)
        .addTooltip("Empty uses the project file's `output`, and build/ beside it when it has none.")
        .addLabeledComponent("Mode:", mode, 1, false)
        .addTooltip("Empty uses the project file's `target`. One of exe, js, jslib, lib, shared, llvm.")
        .addLabeledComponent("Program arguments:", programArguments, 1, false)
        .addLabeledComponent("Working directory:", workingDirectory, 1, false)
        .addComponentFillVertically(JPanel(), 0)
        .panel

    override fun resetEditorFrom(configuration: YanaRunConfiguration) {
        projectFile.text = configuration.projectFile
        outputDirectory.text = configuration.outputDirectory
        workingDirectory.text = configuration.workingDirectory
        programArguments.text = configuration.programArguments
        mode.text = configuration.mode
    }

    override fun applyEditorTo(configuration: YanaRunConfiguration) {
        configuration.projectFile = projectFile.text.trim()
        configuration.outputDirectory = outputDirectory.text.trim()
        configuration.workingDirectory = workingDirectory.text.trim()
        configuration.programArguments = programArguments.text.trim()
        configuration.mode = mode.text.trim()
    }
}
