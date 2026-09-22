package se.rimmer.yana.project

import com.intellij.ide.actions.CreateFileFromTemplateAction
import com.intellij.ide.actions.CreateFileFromTemplateDialog
import com.intellij.openapi.actionSystem.ActionGroup
import com.intellij.openapi.actionSystem.ActionGroupWrapper
import com.intellij.openapi.actionSystem.ActionManager
import com.intellij.openapi.actionSystem.ActionUpdateThread
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.actionSystem.Constraints
import com.intellij.openapi.actionSystem.DefaultActionGroup
import com.intellij.openapi.project.DumbAware
import com.intellij.openapi.project.Project
import com.intellij.openapi.project.guessProjectDir
import com.intellij.openapi.startup.ProjectActivity
import com.intellij.psi.PsiDirectory
import se.rimmer.yana.YanaIcons

/*
 * "New | Yana File" - the one thing the New menu offers for this language.
 *
 * Two kinds, because a Yana file is one of two things and which is decided by its first line rather
 * than by its name (`doc/spec/modules.md`): a **file** of the module its directory is, which needs
 * nothing written at all, and a **module** of its own, which starts with `module`. The third form,
 * `module M` joining an ancestor, names a module the dialog cannot know, and is one word to type.
 */
class YanaNewFileAction : CreateFileFromTemplateAction(), DumbAware {
    override fun buildDialog(project: Project, directory: PsiDirectory, builder: CreateFileFromTemplateDialog.Builder) {
        builder.setTitle("New Yana File")
            .addKind("File (part of this directory's module)", YanaIcons.FILE, FILE_TEMPLATE)
            .addKind("Module (a module of its own)", YanaIcons.FILE, MODULE_TEMPLATE)
    }

    override fun getActionName(directory: PsiDirectory?, newName: String, templateName: String?) =
        "Create Yana file $newName"

    companion object {
        const val FILE_TEMPLATE = "Yana File"
        const val MODULE_TEMPLATE = "Yana Module"
    }
}

/*
 * The New menu in a Yana project: this plugin's entry first, and CLion's C++ group gone.
 *
 * CLion puts `CIDR.Lang.NewGroup` - C++ class, source file, header, module - at the top of `NewGroup`
 * for every project, because to CLion every project is a C++ one. A project whose root holds a
 * `yana.toml` and no `CMakeLists.txt` is not, and offering it a header is offering a file its build
 * will never read. So the group is wrapped once, application-wide, in one that hides itself in such a
 * project and is otherwise exactly the group it wraps: a mixed project, or any project that is not a
 * Yana one, sees CLion's menu unchanged.
 *
 * Done on startup rather than in `plugin.xml`, because a plugin descriptor can add to a group but not
 * change another plugin's entry in it - and without CLion there is no group, and nothing is done.
 */
class YanaNewMenuSetup : ProjectActivity {
    override suspend fun execute(project: Project) {
        val manager = ActionManager.getInstance()
        val newGroup = manager.getAction("NewGroup") as? DefaultActionGroup ?: return

        // Idempotent, so it runs for every project opened rather than keeping a flag: a second
        // replace finds the registered group no longer in the menu and changes nothing.
        val cpp = manager.getAction(CPP_GROUP) as? ActionGroup
        if (cpp != null) newGroup.replaceAction(cpp, HiddenInYanaProject(cpp))

        // First, and after the wrap: CLion's group is anchored first too, and which of two `first`
        // anchors wins is the order the plugins loaded in.
        val ours = manager.getAction(ACTION_ID) ?: return
        newGroup.remove(ours)
        newGroup.add(ours, Constraints.FIRST)
    }

    companion object {
        const val ACTION_ID = "Yana.NewFile"
        const val CPP_GROUP = "CIDR.Lang.NewGroup"
    }
}

class HiddenInYanaProject(group: ActionGroup) : ActionGroupWrapper(group) {
    override fun update(e: AnActionEvent) {
        super.update(e)
        val project = e.project ?: return
        if (isYanaOnly(project)) e.presentation.isEnabledAndVisible = false
    }

    override fun getActionUpdateThread() = ActionUpdateThread.BGT
}

/// A project whose root says it is Yana and does not say it is also CMake.
fun isYanaOnly(project: Project): Boolean {
    val root = project.guessProjectDir() ?: return false
    return root.findChild("yana.toml") != null && root.findChild("CMakeLists.txt") == null
}
