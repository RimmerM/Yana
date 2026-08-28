#pragma once

#include "settings.h"

/*
 * `yana.toml` - what a Yana project is, written down.
 *
 * The driver learns what to compile from `-add` and `-root`. An editor has no command line, so
 * without a file on disk the language server would have to guess which files are in the program,
 * and guessing wrong means every cross-module reference in the editor is unresolved while the
 * command-line build is fine. Implementation-Tooling.md §5.2.
 *
 * The flags still win. A project file fills in what the command line did not say, so that
 * `yana -mode js -add src` in a directory with a native `yana.toml` compiles what it was asked to.
 */
struct ProjectFile {
    /// The file itself, and the directory holding it. Every relative path in the file is relative
    /// to that directory rather than to the process's, so that where the compiler was invoked from
    /// cannot change what a project contains.
    Tritium::String path;
    Tritium::String directory;

    /// `root = "Main"` - the module whose `main` the program enters through. Empty when unset.
    Tritium::String root;

    /*
     * `[package] name = "base"` - the package this source tree *is*. Empty for a project that has
     * not said, which is every program that only consumes packages.
     *
     * A package is a boundary and not yet a unit of distribution - Analysis-Modules.md's Part 7 list
     * defers versions, resolution and registries, and none of what reads this needs them. What it
     * buys is that two questions stop being answerable only by hardcoding: which modules a consumer
     * of this tree may import (`exports` below), and whose tests a test build runs. The second is
     * the one that could not be said at all before - a `.test.yana` file of a library module joined
     * every consumer's suite, because "is this a test build" was one global answer for every walk.
     */
    Tritium::String name;

    /*
     * `[package] exports = ["Core", "Math", ...]` - the modules a consumer of this package may
     * import. Everything else in the tree is the package's own business.
     *
     * **At module granularity and in the manifest, rather than a second level of `pub`.** Visibility
     * is one bit (`ast::Decl::exported`) and stays one bit: a per-declaration package level would
     * put the annotation on the *common* case, since most of a library's declarations exist so the
     * rest of it can be written. Drawing the line once, at the granularity of the thing you already
     * import, is what Go's `internal/` and Java's `exports` clause both settled on. The cost is a
     * leak check rather than a syntax - see `checkExportedSignatures`.
     *
     * Empty means "no boundary declared", which is every project that is not a library, and those
     * export everything. A package that lists nothing and wants to export nothing says so by having
     * no consumers.
     */
    Array<Tritium::String> exports;

    /// `sources = ["src", "lib"]` - already joined onto `directory`.
    Array<Tritium::String> sources;

    /*
     * `[test] sources = ["test"]` - Design-Test.md §3.4, already joined onto `directory`.
     *
     * Read only under `-test`, and then they are ordinary source roots added to the ones above:
     * `test/Api/` is the module `Api`, which sees `src`'s modules through their `pub` names and
     * their imports. A *unit* test needs no root of its own - it is a `.test.yana` file of the module
     * it tests - so these are the integration half and only that.
     */
    Array<Tritium::String> testSources;

    /// `target = "native"` - which output, and therefore which `@platform` declarations exist.
    /// Nothing when unset. See §5.4: this is the one the server resolves against.
    Maybe<CompileMode> target;

    /// `output = "build"` - where compilation results go, joined onto `directory`. Empty when unset.
    Tritium::String output;
};

/// Looks for a `yana.toml` in `directory` and then in each directory above it. Returns the path of
/// the first one found, or nothing. `maxDepth` bounds the walk so a path that cannot be shortened -
/// a relative one, a root - terminates rather than looping.
Maybe<Tritium::String> findProjectFile(const Tritium::String& directory, U32 maxDepth = 24);

/// The project file a set of settings names: the `-project` path when there is one - taken as the
/// file itself, or as a directory holding one - and otherwise the first `yana.toml` at or above the
/// working directory. Nothing when `-no-project` was given or nothing was found.
Maybe<Tritium::String> locateProjectFile(const CompileSettings& settings);

/// Reads and parses one project file. The accepted syntax is a subset of TOML: line comments, and
/// top-level `key = value` where a value is a quoted string or an array of them. A `[table]` header
/// begins a section this reader has no keys in, so everything under one is ignored rather than
/// rejected - a project file may carry settings for something that is not the compiler.
Result<ProjectFile, Tritium::String> readProjectFile(const Tritium::String& path);

/// Fills in the settings the command line left unset. Never overwrites one it set: see above.
void applyProjectFile(CompileSettings& settings, const ProjectFile& project);

/// Joins two path segments with a separator, unless `relative` is already absolute - in which case
/// it is returned as it stands, because a project file naming `/opt/yana/lib` means that directory.
Tritium::String joinProjectPath(const Tritium::String& directory, const Tritium::String& relative);
