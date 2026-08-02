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

    /// `root = "Main"` - the module the program is entered through. Empty when unset, which is legal
    /// for a project with exactly one module.
    var root: String = ""
        private set

    /// `output = "build"` - joined onto the project directory. Null when unset.
    var output: File? = null
        private set

    /// `sources = ["src"]` - joined onto the project directory.
    val sources: MutableList<File> = mutableListOf()

    /*
     * The executable the driver writes, or null when the project does not say enough to know.
     *
     * The driver names it after the *root module* and puts it in the output directory, so both
     * halves come from here. A project with no `root` and exactly one module is legal - the module
     * is the program - and that case is answered by looking, which is what `findRootModule` does.
     */
    fun executableName(): String? {
        if (root.isNotEmpty()) return root

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
                    "root" -> project.root = values[0]
                    "output" -> project.output = project.directory.resolve(values[0])
                    "sources" -> values.forEach { project.sources += project.directory.resolve(it) }
                }
            }

            return project
        }
    }
}
