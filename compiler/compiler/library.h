#pragma once

#include "diagnostics.h"

struct Context;

/*
 * The standard library, as files.
 *
 * Core, Native, Native.Linux, NativeText, Host, Collections and Text used to be seven raw string
 * literals inside compiler/resolve. They are now seven files in `lib/`, read through this, and the
 * difference is not only where the text lives: a module that is a file is a module the ordinary
 * machinery can find. `resolveImports` asks here for anything the project's own module map has no
 * file for, so `import Native` in a user program is resolved by the same lookup that builds Core -
 * and a library module that nothing implicitly imports costs nothing until something names it.
 *
 * A name becomes a path by the rule the module map already uses in the other direction: dots are
 * directory separators, and `Native.Linux` is `Native/Linux.yana`. The index form a source tree may
 * also use - `Native/Native.yana` for `Native` - is accepted as a second candidate, so a library
 * module that grows into a directory of its own does not have to move.
 *
 * **Where the directory is** is answered once per compilation and cached, from four sources in
 * order: `-lib` on the command line, `YANA_LIB` in the environment, a path beside the running
 * executable, and the tree this compiler was built from. Each candidate is accepted only if
 * `Core.yana` is in it, so a stale entry falls through to the next rather than producing a
 * compilation with half a library.
 *
 * The text of every module read is kept for as long as the compilation runs, for the reason
 * SourceEntry keeps a file's: a diagnostic quotes the line it points at, and it may be reported
 * long after the module was parsed. `librarySource` on the Context is what a diagnostic reaches it
 * through.
 */
struct LibrarySource {
    struct Entry {
        StringId name;
        Tritium::String path;
        Ptr<char, HeapDeleter> text;
        Size length = 0;
    };

    /*
     * The source of one library module, or empty if the library has no file for it.
     *
     * Silent on a miss, deliberately: the two callers want different things said. `defineCore` and
     * its siblings report "the standard library could not be found" - which is a broken
     * installation rather than a broken program - and `resolveImports` reports nothing, because a
     * name that is neither a project module nor a library one is an unresolved import and the
     * import statement is where that is said.
     */
    StringView source(Context& context, StringId module);

    /*
     * The text of a library module that has already been read, or empty.
     *
     * Context-free, which is the whole point of it: `Diagnostics` quotes the line a report points
     * at and has no Context to reach a path through. It needs none - a location inside a library
     * module can only exist because that module was read and parsed, so by the time anything can
     * point into one the text is here.
     */
    StringView loaded(StringId module) const;

    /// The directory the library was found in, or empty if no candidate held a `Core.yana`.
    /// Computed on the first call and cached, including the empty answer - the search touches the
    /// file system, and a compilation that cannot find its library asks about seven modules.
    const Tritium::String& directory(Context& context);

private:
    Array<Entry> entries;
    Tritium::String root;
    bool searched = false;
};
