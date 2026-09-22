package se.rimmer.yana.run

/*
 * The driver's diagnostics, read back out of its error stream - one entry each in the Build window.
 *
 * The format is `PrintDiagnostics::message` in compiler/compiler/diagnostics.cpp: a header, then
 * the quoted source line and a `^~~^` marker under it when the diagnostic has a location. So a
 * diagnostic is a header and everything up to the next one. The header has two shapes:
 *
 *     /path/to/Main.yana:4:10: error: text     the file the diagnostic is in
 *     4:10: error: text                        no file behind it - a report with no location
 *
 * The first is what makes an entry open its file. Both count lines and columns from one.
 *
 * `level` is `error`, `warning`, or empty for a note - the driver's own three.
 */
data class YanaDiagnostic(
    val path: String?,
    val level: String,
    val line: Int,
    val column: Int,
    val text: String,
    val detail: String,
) {
    val isError: Boolean get() = level == "error"

    /// The tree entry's text where there is no file to group it under - the position first, since
    /// it is then the only thing telling two otherwise identical messages apart.
    val title: String get() = "$line:$column: $text"

    companion object {
        // The path is greedy and optional, so that a Windows drive's colon stays part of it: the
        // match backs off to the last `:<line>:<column>: <level>: ` on the line.
        private val header = Regex("""^(?:(.*):)?(\d+):(\d+): (error|warning|): (.*)$""")

        fun parse(output: String): List<YanaDiagnostic> {
            val diagnostics = mutableListOf<YanaDiagnostic>()
            var current: MatchResult? = null
            val block = StringBuilder()

            fun flush() {
                val match = current ?: return
                val (path, line, column, level, text) = match.destructured

                // `toIntOrNull`, because `\d+` matches a run of digits longer than an Int as happily
                // as a line number - and a build that threw here would report nothing at all.
                val atLine = line.toIntOrNull() ?: return
                val atColumn = column.toIntOrNull() ?: return

                diagnostics += YanaDiagnostic(
                    path.ifEmpty { null }, level, atLine, atColumn, text, block.toString().trimEnd(),
                )
            }

            for (raw in output.lineSequence()) {
                val line = raw.trimEnd('\r')
                val match = header.find(line)
                if (match != null) {
                    flush()
                    current = match
                    block.setLength(0)
                }

                // Text before the first header is not a diagnostic - it is whatever the driver said
                // about itself, and the caller shows the whole stream beside these anyway.
                if (current != null) block.append(line).append('\n')
            }

            flush()
            return diagnostics
        }
    }
}
