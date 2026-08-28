#pragma once

#include "diagnostics.h"

struct Context;
struct CompileSettings;

/*
 * The standard library, as files.
 *
 * Core, Native, Native.Linux, NativeText, Host, Collections and Text used to be seven raw string
 * literals inside compiler/resolve, and then seven files. They are now the files of two modules, They are now files in `lib/`, read through this, and the
 * difference is not only where the text lives: a module that is a file is a module the ordinary
 * machinery can find. `resolveImports` asks here for anything the project's own module map has no
 * file for, so `import Native` in a user program is resolved by the same lookup that builds Core -
 * and a library module that nothing implicitly imports costs nothing until something names it.
 *
 * A **module** name becomes a path by the rule the module map already uses in the other direction:
 * dots are directory separators, so the module `Native.Linux` is `Native/Linux`. A module may be
 * either a directory of files or one file - `files` below is what asks - which is the same question
 * the project's own module map answers by walking a source tree. There is no index form: a directory
 * of files is a module without any one of them being singled out, Analysis-Modules.md §2.1.2. A
 * *file* name is not a path in either direction any more, and `record` below says why.
 *
 * **Where the directory is** is answered once per compilation and cached, from three sources in
 * order: `-lib` on the command line or `library` in the project file, a path beside the running
 * executable, and the tree this compiler was built from. Each candidate is accepted only if
 * `Core/Core.yana` is in it, so a stale entry falls through to the next rather than producing a
 * compilation with half a library.
 *
 * The text of every module read is kept for as long as the compilation runs, for the reason
 * SourceEntry keeps a file's: a diagnostic quotes the line it points at, and it may be reported
 * long after the module was parsed. `librarySource` on the Context is what a diagnostic reaches it
 * through.
 */
/*
 * One file the library walk found, and whether it sits directly in the module's own directory.
 *
 * The flag is here because it cannot be recovered from the name: `Core.Array.native` is a file
 * directly in `Core/` whose name carries a selector, and `Core.Float.Ryu` is a file one directory
 * down, and the two are the same shape. Only the walk knows, so the walk says.
 */
struct LibraryFile {
    StringId name;
    bool inDirectory;

    // Whether a `test` segment was among the selectors - Design-Test.md §3.1. Carried from the walk
    // to `ast::Module::test`, which is where everything else asks.
    bool test = false;
};

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
    StringView source(Context& context, StringId file);

    /*
     * Every file of a library module, as file names - the ids `source` above takes.
     *
     * `Core` is a directory, so this answers `Core.Core`, `Core.Check` and the rest of them; `Math`
     * is one file, so it answers `Math`. Empty where the library has neither, which is what makes an
     * unresolvable import silent here and reported at the import statement.
     *
     * **Candidates rather than members.** Subdirectories are walked, because a file under `Core/`
     * may write `module Core` and join it (§2.1), and nothing here has read a file to know whether
     * it did. `parseLibraryGroup` parses these and keeps the ones whose membership agrees. A file
     * whose name carries a selector for another target is dropped here, since that answer needs no
     * parse - Analysis-Modules.md §2.5.
     *
     * **Sorted by name**, and that is not cosmetic: the order files are handed back in is the order
     * the declaration passes read them, which is what decides the cut of a containment cycle
     * (Analysis-Modules.md §2.3). A directory listing is in whatever order the file system gives,
     * and a library that laid out its records differently on ext4 and on APFS would be a real bug
     * with no way to see it.
     */
    Array<LibraryFile> files(Context& context, StringId module);

    /*
     * The text of a library module that has already been read, or empty.
     *
     * Context-free, which is the whole point of it: `Diagnostics` quotes the line a report points
     * at and has no Context to reach a path through. It needs none - a location inside a library
     * module can only exist because that module was read and parsed, so by the time anything can
     * point into one the text is here.
     */
    StringView loaded(StringId file) const;

    /*
     * Where a file is, remembered by the walk that found it.
     *
     * A file id stopped being a path when a file gained selectors and a module gained
     * subdirectories: `Core.Array.native` is `Core/Array.native.yana` and `Core.Float.Ryu` is
     * `Core/Float/Ryu.yana`, and the two names differ in nothing that could tell them apart. So the
     * mapping is recorded where it is known rather than derived where it is needed, and `source`
     * has no rule of its own to keep in step with the walk's.
     */
    void record(StringId file, Tritium::String path);

    /// The directory the library was found in, or empty if no candidate held a `Core/Core.yana`.
    /// Computed on the first call and cached, including the empty answer - the search touches the
    /// file system, and a compilation that cannot find its library asks about it for every module.
    const Tritium::String& directory(Context& context);

    /*
     * The library's own `yana.toml`, read once - its `[package]` name and export list, kept as the
     * two facts rather than as the file.
     *
     * The standard library is a package under the same rules as any other, which is the point of
     * reading a manifest here rather than hardcoding a name: the two things that used to be special
     * cases - whose tests these files are, and which of these modules a program may import - are
     * answered by the same two keys any package writes.
     *
     * A library with no manifest exports everything and tests nothing, which is exactly what every
     * tree looked like before this existed.
     */
    void readManifest(Context& context);

    /// Whether this compilation *is* the library's package, and may therefore see its test files and
    /// its unexported modules. `CompileSettings::package` against the manifest's name; false when
    /// either is empty, since an unnamed package is nobody's.
    bool isOwnPackage(Context& context, const CompileSettings& settings);

    /// Whether a module of the library may be imported from outside its package. True when the
    /// manifest lists it, and true for everything when the manifest lists none.
    bool exportsModule(Context& context, StringId module);

private:
    Array<Entry> entries;
    Tritium::String root;
    bool searched = false;

    Tritium::String packageName;
    Array<Tritium::String> packageExports;
    bool readPackage = false;
};
