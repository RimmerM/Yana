package se.rimmer.yana

import com.intellij.ide.fileTemplates.FileTemplateManager
import com.intellij.openapi.actionSystem.ActionManager
import com.intellij.openapi.actionSystem.Constraints
import com.intellij.openapi.actionSystem.DefaultActionGroup
import com.intellij.openapi.actionSystem.impl.SimpleDataContext
import com.intellij.testFramework.TestActionEvent
import com.intellij.openapi.project.guessProjectDir
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import kotlinx.coroutines.runBlocking
import se.rimmer.yana.project.HiddenInYanaProject
import se.rimmer.yana.project.YanaNewFileAction
import se.rimmer.yana.project.YanaNewMenuSetup
import se.rimmer.yana.project.isYanaOnly

/// "New | Yana File" and what the New menu looks like in a Yana project - YanaNewFile.kt.
class YanaNewFileTest : BasePlatformTestCase() {
    fun testTemplates() {
        val templates = FileTemplateManager.getInstance(project)
        assertEquals("", templates.getInternalTemplate(YanaNewFileAction.FILE_TEMPLATE).text)
        assertEquals("module\n\n", templates.getInternalTemplate(YanaNewFileAction.MODULE_TEMPLATE).text)
    }

    fun testActionIsFirstInNewMenu() {
        runBlocking { YanaNewMenuSetup().execute(project) }

        val manager = ActionManager.getInstance()
        val newGroup = manager.getAction("NewGroup") as DefaultActionGroup
        val children = newGroup.getChildActionsOrStubs()
        assertTrue(children.first() is YanaNewFileAction)

        // CLion's C++ group, where this platform has one, is the wrapped one.
        if (manager.getAction(YanaNewMenuSetup.CPP_GROUP) != null) {
            assertTrue(children.any { it is HiddenInYanaProject })
        }
    }

    /// The test platform has no CLion, so a stand-in is registered under CLion's id: it is wrapped,
    /// the wrapper hides it in a Yana project, and running the setup again changes nothing.
    fun testCppGroupHiddenInYanaProject() {
        val manager = ActionManager.getInstance()
        val newGroup = manager.getAction("NewGroup") as DefaultActionGroup
        if (manager.getAction(YanaNewMenuSetup.CPP_GROUP) != null) return

        val cpp = DefaultActionGroup()
        manager.registerAction(YanaNewMenuSetup.CPP_GROUP, cpp)
        newGroup.add(cpp, Constraints.FIRST)

        try {
            runBlocking { YanaNewMenuSetup().execute(project) }
            runBlocking { YanaNewMenuSetup().execute(project) }

            val children = newGroup.getChildActionsOrStubs()
            assertTrue(children.first() is YanaNewFileAction)
            assertFalse(children.any { it === cpp })
            val wrapped = children.filterIsInstance<HiddenInYanaProject>().single { it.delegate === cpp }

            assertTrue(visible(wrapped))
            myFixture.addFileToProject("yana.toml", "")
            assertFalse(visible(wrapped))
        } finally {
            newGroup.getChildActionsOrStubs().filter { it === cpp || (it is HiddenInYanaProject && it.delegate === cpp) }
                .forEach { newGroup.remove(it) }
            manager.unregisterAction(YanaNewMenuSetup.CPP_GROUP)
        }
    }

    private fun visible(group: HiddenInYanaProject): Boolean {
        val event = TestActionEvent.createTestEvent(group, SimpleDataContext.getProjectContext(project))
        group.update(event)
        return event.presentation.isVisible
    }

    fun testYanaOnlyProject() {
        val root = project.guessProjectDir()!!
        assertFalse(isYanaOnly(project))

        myFixture.addFileToProject("yana.toml", "sources = [\"src\"]\n")
        assertEquals("yana.toml is looked for at the project root", root, root.findChild("yana.toml")?.parent)
        assertTrue(isYanaOnly(project))

        myFixture.addFileToProject("CMakeLists.txt", "")
        assertFalse(isYanaOnly(project))
    }
}
