package se.rimmer.yana.clion

import com.intellij.execution.RunManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.roots.ProjectModelExternalSource
import com.jetbrains.cidr.execution.CidrBuildConfiguration
import com.jetbrains.cidr.execution.build.CidrBuildConfigurationProvider
import se.rimmer.yana.run.YanaBuildTarget
import se.rimmer.yana.run.YanaRunConfiguration

/*
 * CLion's Build button, for a Yana configuration.
 *
 * The hammer beside the run configuration - and "Build | Build '<name>'", which is the same action -
 * is CLion's rather than the platform's, and it does not look at the selected run configuration
 * itself. It asks every `cidr.buildConfigurationProvider` what is buildable, is disabled when none of
 * them answers, and hands whatever they return to `ProjectTaskManager`, where YanaProjectTaskRunner
 * picks a Yana one up. CMake, Makefile and Meson projects reach the button the same way.
 *
 * The answer is the *selected* configuration and nothing else, which is what every provider in CLion
 * does: the button is labelled with that configuration's name, and building something else under it
 * would be a lie.
 *
 * This file is the one place the plugin names a CLion class, and it is loaded from yana-clion.xml -
 * an optional dependency - so the rest of the plugin still loads in an IDE without CLion's modules.
 */
class YanaBuildConfigurationProvider : CidrBuildConfigurationProvider {
    override fun getBuildableConfigurations(project: Project): List<CidrBuildConfiguration> {
        val selected = RunManager.getInstanceIfCreated(project)?.selectedConfiguration?.configuration
        return if (selected is YanaRunConfiguration) listOf(YanaCidrBuildConfiguration(selected)) else emptyList()
    }
}

/// A data class so that two asks for the same configuration are one element, which is how the
/// platform recognises a build it is already running.
data class YanaCidrBuildConfiguration(override val configuration: YanaRunConfiguration) :
    CidrBuildConfiguration, YanaBuildTarget {
    override fun getName(): String = configuration.name
    override fun getExternalSource(): ProjectModelExternalSource? = null
}
