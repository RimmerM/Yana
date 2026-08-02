import org.jetbrains.grammarkit.tasks.GenerateLexerTask
import org.jetbrains.intellij.platform.gradle.IntelliJPlatformType
import org.jetbrains.intellij.platform.gradle.TestFrameworkType

/*
 * The `plugins` block cannot read gradle.properties, so these versions are literals and the entries
 * in gradle.properties are documentation - change both together.
 *
 * The Kotlin version is not free: the IDE's own classes carry Kotlin metadata written by the
 * compiler that built them, and a compiler older than that cannot read it. Compiling against a
 * 2025.3 platform - metadata 2.2 - with Kotlin 2.0 does not fail with a version mismatch, it fails
 * with an internal compiler error while reporting one. Raise this whenever platformVersion moves
 * past the Kotlin bundled with the IDE, and do not raise it further than that: the stdlib at
 * runtime is the platform's, not ours.
 */
plugins {
    id("java")
    id("org.jetbrains.kotlin.jvm") version "2.2.21"
    id("org.jetbrains.intellij.platform") version "2.11.0"
    id("org.jetbrains.grammarkit") version "2022.3.2.2"
}

group = providers.gradleProperty("pluginGroup").get()
version = providers.gradleProperty("pluginVersion").get()

repositories {
    mavenCentral()

    intellijPlatform {
        defaultRepositories()
    }
}

dependencies {
    intellijPlatform {
        create(
            IntelliJPlatformType.CLion,
            providers.gradleProperty("platformVersion").get(),
        )

        // No instrumentationTools(): the plugin does it on its own from 2.x, and calling it is
        // deprecated. Removed here rather than left warning, since the comment above already tracks
        // what a version bump costs.
        testFramework(TestFrameworkType.Platform)
    }

    testImplementation("junit:junit:4.13.2")
}

kotlin {
    jvmToolchain(21)
}

tasks.test {
    // The platform test framework initialises AWT whether or not a test touches the UI, so a
    // machine with no reachable X server fails all twelve lexer tests on `X11GraphicsEnvironment`
    // before any of them runs. The lexer is a pure function of text; nothing here needs a display.
    systemProperty("java.awt.headless", "true")
}

intellijPlatform {
    pluginConfiguration {
        ideaVersion {
            sinceBuild = providers.gradleProperty("pluginSinceBuild")

            // Absent, not empty. `until-build=""` is an invalid descriptor, and an upper bound at
            // all is what the platform advises against from build 243 - see gradle.properties.
            untilBuild = provider { null }
        }
    }
}

/*
 * The one keyword list - Implementation-Tooling.md §9.
 *
 * `compiler/parse/tokens.def` is what the compiler's own lexer is built from, and this generates
 * the plugin's tables from the same file rather than repeating it. A keyword added to the language
 * and not to the editor is the drift this exists to prevent, and it is the kind that shows up as
 * "the new keyword is not coloured" months later.
 *
 * A Gradle task rather than a committed generated file, so that the two cannot be out of step in a
 * working tree either.
 */
val tokensDef = layout.projectDirectory.file("../compiler/parse/tokens.def")
val generatedTokensDir = layout.buildDirectory.dir("generated/sources/tokens")

val generateYanaKeywords by tasks.registering {
    val input = tokensDef.asFile
    val outputDir = generatedTokensDir

    inputs.file(input)
    outputs.dir(outputDir)

    doLast {
        val entry = Regex("""^YANA_(KEYWORD|RESERVED_OP)\(\s*\w+\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)""")
        val keywords = mutableListOf<String>()
        val operators = mutableListOf<String>()

        input.forEachLine { line ->
            val match = entry.find(line.trim()) ?: return@forEachLine
            // The text is a C string literal, so a backslash operator is written `"\\"`. Unescaping
            // it here and re-escaping it below is what keeps `\` a single character in both.
            val text = match.groupValues[2].replace("\\\\", "\\").replace("\\\"", "\"")
            if (match.groupValues[1] == "KEYWORD") keywords += text else operators += text
        }

        require(keywords.isNotEmpty() && operators.isNotEmpty()) {
            "no entries were read from $input - the X-macro format it is parsed with has changed"
        }

        // Kotlin's own escaping on the way out. The dollar sign needs it too: a bare `$` in a
        // string literal is the start of a template, and `$` is one of Yana's reserved operators.
        fun quote(text: String) = "\"" + text
            .replace("\\", "\\\\")
            .replace("\"", "\\\"")
            .replace("\$", "\\\$") + "\""

        val target = outputDir.get().dir("se/rimmer/yana/lexer").asFile
        target.mkdirs()
        target.resolve("YanaKeywords.kt").writeText(
            """
            package se.rimmer.yana.lexer

            // Generated from compiler/parse/tokens.def by the generateYanaKeywords Gradle task.
            // Do not edit: edit tokens.def, which the compiler's own lexer is built from.
            object YanaKeywords {
                @JvmField
                val KEYWORDS: Set<String> = setOf(
                    ${keywords.joinToString(",\n                    ") { quote(it) }}
                )

                @JvmField
                val RESERVED_OPERATORS: Set<String> = setOf(
                    ${operators.joinToString(",\n                    ") { quote(it) }}
                )
            }
            """.trimIndent() + "\n"
        )
    }
}

val generateYanaLexer by tasks.registering(GenerateLexerTask::class) {
    sourceFile.set(file("src/main/kotlin/se/rimmer/yana/lexer/YanaLexer.flex"))
    targetOutputDir.set(layout.buildDirectory.dir("generated/sources/flex/se/rimmer/yana/lexer"))
    purgeOldFiles.set(true)
}

// The generated scanner is Java and the generated keyword table is Kotlin, so they go into
// different source sets - and the Kotlin compiler sees the Java sources either way.
sourceSets["main"].java.srcDir(layout.buildDirectory.dir("generated/sources/flex"))
kotlin.sourceSets["main"].kotlin.srcDir(generatedTokensDir)

tasks.named("compileKotlin") {
    dependsOn(generateYanaKeywords, generateYanaLexer)
}

tasks.named("compileJava") {
    dependsOn(generateYanaLexer)
}
