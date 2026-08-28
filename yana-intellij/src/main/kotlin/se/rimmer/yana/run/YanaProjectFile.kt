package se.rimmer.yana.run

import java.io.File

/*
 * `yana.toml`, as much of it as a run configuration needs - Implementation-Tooling.md §5.2, §12.
 *
 * The plugin reads the project file rather than asking the user to restate it, because the driver
 * and the language server both already read it and a third answer to "what is this program called"
 * is a third chance to point at the wrong binary. §12's rule is the same one from the other side:
 * the plugin does not own the build configuration, so it reads what does.
 *
 * The accepted syntax is the compiler's own subset - line comments, and top-level `key = value`
 * where a value is a quoted string or an array of them. A `[table]` header starts a section this has
 * no keys in, so everything under one is skipped rather than rejected, exactly as `readProjectFile`
 * does it.
 */
class YanaProjectFile private constructor(val file: File) {
    val directory: File = file.parentFile ?: File(".")

    /// `main = "App"` - the module the program is entered through. Empty when unset, which is legal
    /// for a project with exactly one module.
    var main: String = ""
        private set

    /// `to = "build"` - where the build goes, joined onto the project directory. Null when unset.
    var to: File? = null
        private set

    /// `output = "app"` - what the artifact is called. Empty when unset, and then it is the main
    /// module's name - which is why `executableName` below has to answer rather than read this.
    var output: String = ""
        private set

    /// `sources = ["src"]` - joined onto the project directory.
    val sources: MutableList<File> = mutableListOf()

    /*
     * What to call the executable, or null when the project does not say enough to know.
     *
     * Passed to the compiler as `-output` rather than predicted from what it would have chosen: the
     * driver's own default is the main *module's* name, and a source root's module is named after
     * the directory, so `src/Main.yana` in a project with no `main` builds as `Src`. Naming it here
     * makes the build and the run agree without this having to reimplement the grouping rules.
     *
     * A project with no `main` and exactly one source file is the template's own shape, and the file
     * is the program - so its name is the useful one to give.
     */
    fun executableName(): String? {
        if (output.isNotEmpty()) return output
        if (main.isNotEmpty()) return main

        val modules = sources.flatMap { source ->
            source.walkTopDown().filter { it.isFile && it.extension == "yana" }.toList()
        }

        return if (modules.size == 1) modules[0].nameWithoutExtension else null
    }

    companion object {
        private val entry = Regex("""^([A-Za-z0-9_-]+)\s*=\s*(.+)$""")
        private val quoted = Regex(""""((?:[^"\\]|\\.)*)"""")

        fun read(file: File): YanaProjectFile? {
            if (!file.isFile) return null

            val project = YanaProjectFile(file)
            var inTable = false

            for (raw in file.readLines()) {
                val line = raw.substringBefore('#').trim()
                if (line.isEmpty()) continue

                if (line.startsWith("[")) {
                    inTable = true
                    continue
                }

                if (inTable) continue

                val match = entry.find(line) ?: continue
                val values = quoted.findAll(match.groupValues[2]).map { it.groupValues[1] }.toList()
                if (values.isEmpty()) continue

                when (match.groupValues[1]) {
                    "main" -> project.main = values[0]
                    "to" -> project.to = project.directory.resolve(values[0])
                    "output" -> project.output = values[0]
                    "sources" -> values.forEach { project.sources += project.directory.resolve(it) }
                }
            }

            return project
        }
    }
}
