package se.rimmer.yana

import com.intellij.execution.RunManager
import com.intellij.testFramework.fixtures.BasePlatformTestCase
import se.rimmer.yana.lsp.YanaSemanticTokens
import se.rimmer.yana.run.PROVIDER_ID
import se.rimmer.yana.run.YanaRunConfigurationType

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
