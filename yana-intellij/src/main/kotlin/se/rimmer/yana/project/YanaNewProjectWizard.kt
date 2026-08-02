package se.rimmer.yana.project

import com.intellij.ide.wizard.AbstractNewProjectWizardStep
import com.intellij.ide.wizard.NewProjectWizardBaseData.Companion.baseData
import com.intellij.ide.wizard.NewProjectWizardStep
import com.intellij.ide.wizard.language.LanguageGeneratorNewProjectWizard
import com.intellij.openapi.application.WriteAction
import com.intellij.openapi.diagnostic.logger
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.openapi.vfs.VfsUtil
import com.intellij.ui.dsl.builder.Panel
import com.intellij.ui.dsl.builder.bindSelected
import se.rimmer.yana.YanaIcons
import java.io.File
import javax.swing.Icon

/*
 * "New Project | Yana" - the template Implementation-Tooling.md §12 leaves implicit.
 *
 * CLion's New Project dialog offers CMake templates, because CLion is CMake-shaped and every
 * language it knows about is built by CMake. Yana is not: what says which files are in a program is
 * a `yana.toml` (§5.2), and the compiler builds the whole thing in one invocation. So the template
 * writes that file, one module beside it, and a run configuration pointing at both - which is the
 * same three things a CMake template writes, for a build system that is not CMake.
 *
 * §12's rule holds here too, and this is the shape of honouring it: the template writes a project
 * file and *nothing else about the build*. It does not generate a `CMakeLists.txt` wrapping the
 * compiler, which reads as a shortcut and makes the plugin responsible for build configuration it
 * does not own.
 *
 * This is the **IDEA-style unified wizard's** entry, registered as a
 * `newProjectWizard.languageGenerator`. It is not the one CLion shows - CLion's New Project dialog
 * is built from `directoryProjectGenerator`, and YanaDirectoryProjectGenerator is the entry there.
 * Both exist for the same reason CLion's own bundled Rust support has both, and both write through
 * YanaProjectTemplate so there is one answer to what a new project contains.
 */
class YanaNewProjectWizard : LanguageGeneratorNewProjectWizard {
    override val name: String = "Yana"
    override val icon: Icon = YanaIcons.FILE

    override fun createStep(parent: NewProjectWizardStep): NewProjectWizardStep =
        YanaNewProjectStep(parent)
}

/*
 * The one question worth asking.
 *
 * Everything else about a new Yana project is decided by the language rather than by the author -
 * there is one project file format, one source layout the driver looks in, and one entry point - so
 * a wizard page full of choices would be a page of things there is only one answer to. What is left
 * is whether to write the example module at all, which matters for an author who is about to paste
 * an existing tree in beside it.
 */
class YanaNewProjectStep(parent: NewProjectWizardStep) : AbstractNewProjectWizardStep(parent) {
    private val createSampleProperty = propertyGraph.property(true)
    private var createSample by createSampleProperty

    override fun setupUI(builder: Panel) {
        builder.row {
            checkBox("Create a sample module")
                .bindSelected(createSampleProperty)
                .comment(
                    "Writes src/Main.yana with a program that returns 42, so that Run works before " +
                        "anything has been typed."
                )
        }
    }

    override fun setupProject(project: Project) {
        // The name and the location come from the shared step the wizard puts in front of this one,
        // rather than from fields of our own - which is the whole reason a language generator is a
        // step rather than a dialog of its own.
        val base = baseData
        val root = if (base != null) File(base.path, base.name) else File(project.basePath ?: return)

        try {
            YanaProjectTemplate.write(root, createSample)
        } catch (e: Exception) {
            // A wizard that half-wrote a project is worse than one that says so: the IDE has
            // already opened the directory by this point, so the log entry is what explains an
            // empty project rather than leaving it a mystery.
            logger<YanaNewProjectStep>().error("Cannot write the Yana project in $root", e)
            return
        }

        // Refresh before the run configuration, so that the file the configuration names is one the
        // IDE's virtual file system knows about rather than one it discovers on the next scan.
        WriteAction.runAndWait<RuntimeException> {
            VfsUtil.markDirtyAndRefresh(false, true, true, LocalFileSystem.getInstance().refreshAndFindFileByIoFile(root))
        }

        YanaProjectTemplate.addRunConfiguration(project, root.name)
    }
}
