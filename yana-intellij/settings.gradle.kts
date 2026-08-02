rootProject.name = "yana-intellij"

// The IntelliJ Platform Gradle Plugin resolves the IDE itself and its dependencies from JetBrains'
// own repositories, which are not on Maven Central.
pluginManagement {
    repositories {
        gradlePluginPortal()
        maven("https://oss.sonatype.org/content/repositories/snapshots/")
    }
}
