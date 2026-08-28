#include "context.h"
#include "project.h"
#include <File.h>

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace Tritium;

// Where this compiler was built from, as an absolute path - see compiler/CMakeLists.txt. The last
// candidate and the one that makes a build tree work with no configuration at all, which is what
// every test driver in test/ relies on: they construct a Context and resolve a program, and none of
// them has a command line to pass a path on.
#ifndef YANA_LIBRARY_DIR
#define YANA_LIBRARY_DIR ""
#endif

template<class... T>
static String formatPath(StringView pattern, T&&... args) {
    char buffer[4000];
    auto length = Tritium::format(toBuffer(buffer), toString(pattern), forward<T>(args)...);
    return ownedString(buffer, length);
}

// The one file every candidate is judged by. A directory that does not hold Core is not a library,
// whatever else is in it.
static bool holdsLibrary(const String& candidate) {
    if(candidate == "") return false;
    return File::exists(formatPath("%@/Core/Core.yana"_v, candidate));
}

/*
 * The directory the running executable is in, or empty where this platform has no way to ask.
 *
 * Worth having even though the build tree is covered by YANA_LIBRARY_DIR below it: an installed
 * compiler has been copied away from the tree it was built in, and `<bindir>/../lib` is where its
 * library went. The three forms are `<bindir>/lib` for a compiler that sits beside its library,
 * `<bindir>/../lib` for the usual prefix layout, and `<bindir>/../../lib` for a build tree that puts
 * its binaries a directory deeper than its root.
 */
static String executableDirectory() {
    char buffer[4000];
    Size length = 0;

#if defined(__linux__)
    auto read = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if(read <= 0) return String();
    length = (Size)read;
#elif defined(__APPLE__)
    U32 size = sizeof(buffer);
    if(_NSGetExecutablePath(buffer, &size) != 0) return String();
    length = stringLength(buffer);
#elif defined(_WIN32)
    length = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
    if(length == 0 || length >= sizeof(buffer)) return String();
#else
    return String();
#endif

    // Cut the file name off. A path with no separator in it is a compiler invoked through a bare
    // name from the working directory, which is not a location to search relative to.
    Size cut = length;
    while(cut > 0 && buffer[cut - 1] != '/' && buffer[cut - 1] != '\\') cut--;
    if(cut == 0) return String();

    return ownedString(buffer, cut - 1);
}

const String& LibrarySource::directory(Context& context) {
    if(searched) return root;
    searched = true;

    /*
     * `-lib`, or `library` in the project file, first and unconditionally: a path that was named and
     * is wrong is a mistake to see rather than one to fall through, so this one is taken whether or
     * not it holds a Core - the "cannot find the standard library" report then names the directory
     * the caller asked for.
     *
     * A `YANA_LIB` environment variable used to sit between this and the candidates below. It is
     * gone: a build whose standard library came from the shell said so nowhere a reader of the build
     * could see, and the same fact belongs either to one invocation (`-lib`) or to the project
     * (`library` in its `yana.toml`), both of which are written down.
     */
    if(context.settings.libraryPath != "") {
        root = context.settings.libraryPath;
        return root;
    }

    auto binary = executableDirectory();
    if(binary != "") {
        const StringView relative[] = { "%@/lib"_v, "%@/../lib"_v, "%@/../../lib"_v };
        for(auto& pattern: relative) {
            auto candidate = formatPath(pattern, binary);
            if(holdsLibrary(candidate)) {
                root = candidate;
                return root;
            }
        }
    }

    auto built = String(YANA_LIBRARY_DIR);
    if(holdsLibrary(built)) root = built;

    return root;
}

// A library name as a path fragment: dots are directory separators, so `Core.Text` is `Core/Text`.
// The identifier is the interned text, so this is the module map's rule read backwards and there is
// no second naming convention to keep in step with it.
static bool pathFragment(Context& context, StringId name, char* buffer, Size size, Size& length) {
    auto text = context.findName(name);
    if(text.size() == 0 || text.size() + 8 > size) return false;

    for(Size i = 0; i < text.size(); i++) {
        auto c = text.text()[i];
        buffer[i] = c == '.' ? '/' : c;
    }

    length = text.size();
    return true;
}

/*
 * One directory of the library, and everything under it that could be a file of this module.
 *
 * Recursive, because §2.1 lets a file two directories down write `module Core` and join the module
 * above it - which is how `Core/Float/` holds three files that are private to `Core` rather than a
 * `Core.Float` nobody imports. What a file *wrote* cannot be known here, so this hands back every
 * candidate and `parseLibraryGroup` drops the ones whose membership says otherwise. The library is
 * a few dozen files; the alternative is a second parse of every file to decide whether to parse it.
 *
 * A `.yana` name may carry target selectors - `Array.native.yana`, `Linux.x64.yana` - and one this
 * compilation does not satisfy is not a file of the module at all (Analysis-Modules.md §2.5). The
 * exact path is recorded as it is found, so that `source` below never has to turn a name back into
 * a path: a selector segment and a directory separator are both spelled with a dot in a name, and
 * only the walk that found the file knows which is which.
 */
static void walkModuleFiles(Context& context, LibrarySource& library, const String& path,
                            const String& qualifier, bool inDirectory, Array<LibraryFile>& result)
{
    struct Walk {
        Context& context;
        LibrarySource& library;
        const String& path;
        const String& qualifier;
        bool inDirectory;
        Array<LibraryFile>& result;
        Array<String> directories;
        SelectorScope scope;
    };

    /*
     * Whose files these are. `Project` when this compilation *is* the library's package, which is
     * how `base` tests itself, and `Dependency` for every program that merely imports it - see
     * SelectorScope, where the difference is one selector and the reason it matters.
     */
    auto scope = library.isOwnPackage(context, context.settings) ? SelectorScope::Project
                                                                 : SelectorScope::Dependency;

    Walk walk { context, library, path, qualifier, inDirectory, result, {}, scope };

    listDirectory(path, [](void* data, const String& fileName, bool isDirectory) {
        auto& walk = *(Walk*)data;

        /*
         * Subdirectories are collected and walked after the listing, since the callback is inside
         * the open directory handle.
         *
         * **`ownedString` and not the name itself.** `listDirectory` builds each name as a view of
         * its own 4 KiB `getdents64` buffer, which is a stack frame that is gone by the time this
         * loop runs - so what is kept has to be a copy. A view read after the fact is a use of freed
         * stack, and an unoptimized build hides it completely: the bytes are still there. This cost
         * the release build an infinite recursion that the two debug builds passed.
         *
         * A leading dot is skipped and that is three rules in one: `.` and `..` would walk the whole
         * file system rather than one module, and a hidden directory is not a module - editors and
         * tools leave them inside source trees, and the library is not the place to discover that.
         */
        if(isDirectory) {
            if(fileName.size() && fileName.text()[0] != '.') walk.directories.push(ownedString(fileName));
            return;
        }

        auto stem = StringView { fileName.text(), fileName.size() };
        if(stem.length <= 5 || !stem.endsWith(".yana"_v)) return;
        stem.length -= 5;

        // Every segment after the first is a target selector. An unknown one is reported and the
        // file is left out: a name that is not a selector cannot be a name of anything else here,
        // and compiling it into every target would be the one outcome nothing would notice.
        auto isTest = false;

        for(Size i = 0, from = 0; i <= stem.length; i++) {
            if(i < stem.length && stem.ptr[i] != '.') continue;

            if(from > 0) {
                auto selector = StringView { stem.ptr + from, i - from };
                auto answer = targetSelector(walk.context.settings, selector, walk.scope);

                // Kept as the walk goes past, since this is the last place the file's base name is
                // in hand - see ast::Module::test.
                if(selector == "test"_v) isTest = true;

                if(answer == TargetSelector::Unknown) {
                    walk.context.diagnostics.error("%@ in the standard library names %@, which is not a target - a file name selects a target with `native`, `js`, an operating system, an architecture, an x86-64 level or an instruction-set extension"_v,
                                                   nullptr, formatPath("%@/%@"_v, walk.path, fileName),
                                                   toString(selector));
                    return;
                }

                if(answer == TargetSelector::Excluded) return;
            }

            from = i + 1;
        }

        auto name = formatPath("%@.%@"_v, walk.qualifier, String(stem.ptr, stem.length));
        auto id = walk.context.addQualifiedName(name.text(), name.size());

        walk.library.record(id, formatPath("%@/%@"_v, walk.path, fileName));
        walk.result.push(LibraryFile { id, walk.inDirectory, isTest });
    }, &walk);

    // After the listing rather than during it, since the callback is inside the directory handle.
    for(auto& directory: walk.directories) {
        walkModuleFiles(context, library, formatPath("%@/%@"_v, path, directory),
                        formatPath("%@.%@"_v, qualifier, directory), false, result);
    }
}

Array<LibraryFile> LibrarySource::files(Context& context, StringId module) {
    Array<LibraryFile> result;

    auto& base = directory(context);
    if(base == "") return result;

    char relative[512];
    Size length = 0;
    if(!pathFragment(context, module, relative, sizeof(relative), length)) return result;

    auto path = formatPath("%@/%@"_v, base, String(relative, length));

    /*
     * A directory of files, or one file, and nothing in between.
     *
     * There is no index form - a `Native/Native.yana` naming the module `Native` was magic-by-naming
     * that existed because a directory was not a module and there was no way to say otherwise
     * (Analysis-Modules.md §2.1.2).
     */
    if(!directoryExists(path)) {
        auto file = formatPath("%@.yana"_v, path);
        if(File::exists(file)) {
            record(module, ::move(file));
            result.push(LibraryFile { module, true });
        }

        return result;
    }

    auto name = context.findName(module);
    walkModuleFiles(context, *this, path, String(name.text(), name.size()), true, result);

    // By name, so that the order the passes read the files in is a property of the library rather
    // than of the file system that stored it. Insertion sort: a module is a handful of files.
    for(Size i = 1; i < result.size(); i++) {
        auto value = result[i];
        auto text = context.findName(value.name);
        auto j = i;

        while(j > 0 && context.findName(result[j - 1].name) > text) {
            result[j] = result[j - 1];
            j--;
        }

        result[j] = value;
    }

    return result;
}

// Where a file was found, kept from the walk that found it. Idempotent: `files` may be asked for one
// module more than once, and the second answer must not push a second entry for each of its files.
void LibrarySource::record(StringId file, String path) {
    for(auto& entry: entries) {
        if(entry.name == file) return;
    }

    auto& entry = *entries.push();
    entry.name = file;
    entry.path = ::move(path);
}

StringView LibrarySource::source(Context& context, StringId file) {
    if(auto cached = loaded(file); cached.length) return cached;

    auto& base = directory(context);
    if(base == "") return {};

    auto name = context.findName(file);

    // The path the walk recorded, and only that: a file id is not a path any more. `Core.Array.native`
    // is one file and `Core.Float.Ryu` is another two directories down, and nothing about the two
    // names says which. Reaching here with no record is a name nothing enumerated, which is a caller
    // asking for a file rather than for a module - `parseLibraryGroup` is the only way in.
    const Entry* found = nullptr;
    for(auto& entry: entries) {
        if(entry.name == file) {
            found = &entry;
            break;
        }
    }

    if(!found) return {};

    auto& path = found->path;
    if(!File::exists(path)) return {};

    auto opened = File::openFile(path, readAccess(), File::OpenExisting);
    if(opened.isErr()) {
        context.diagnostics.error("cannot open standard library module %@ at %@: %@"_v, nullptr,
                                  name, path, describeError(opened.unwrapErr()));
        return {};
    }

    auto handle = opened.moveUnwrapOk();
    auto size = handle.size();
    Ptr<char, HeapDeleter> text { (char*)hAlloc(size) };

    if(size && handle.read({ (Byte*)text.get(), size }).isErr()) {
        context.diagnostics.error("cannot read standard library module %@ at %@"_v, nullptr, name, path);
        return {};
    }

    for(auto& entry: entries) {
        if(entry.name != file) continue;

        entry.text = ::move(text);
        entry.length = size;
        return StringView { entry.text.get(), entry.length };
    }

    return {};
}

StringView LibrarySource::loaded(StringId file) const {
    for(auto& entry: entries) {
        if(entry.name == file) return StringView { entry.text.get(), entry.length };
    }

    return {};
}

/*
 * The library's manifest, read once whether or not it is there.
 *
 * `readPackage` guards the read and not the answer: a library with no `yana.toml` is a library that
 * exports everything and tests nothing, which is what every tree looked like before packages
 * existed, and asking the file system that question once per import would be the same answer at a
 * syscall apiece.
 *
 * Failures are silent here. A malformed manifest is worth reporting, but this is called from the
 * middle of resolving an import and the report would arrive attached to whichever import happened to
 * be first; the driver reads the same file through `readProjectFile` when a project names it, which
 * is where a person can act on the message.
 */
void LibrarySource::readManifest(Context& context) {
    if(readPackage) return;
    readPackage = true;

    auto& base = directory(context);
    if(base == "") return;

    auto read = readProjectFile(formatPath("%@/yana.toml"_v, base));
    if(!read) return;

    auto manifest = read.moveUnwrapOk();
    packageName = ::move(manifest.name);
    for(auto& name: manifest.exports) packageExports.push(::move(name));
}

bool LibrarySource::isOwnPackage(Context& context, const CompileSettings& settings) {
    readManifest(context);

    // Both empty is not a match. An unnamed package is nobody's, so a program that says nothing
    // never acquires the library's own privileges by accident.
    if(packageName == "" || settings.package == "") return false;
    return packageName == settings.package;
}

bool LibrarySource::exportsModule(Context& context, StringId module) {
    readManifest(context);

    // A manifest that lists none draws no boundary - see ProjectFile::exports.
    if(packageExports.size() == 0) return true;

    auto name = context.findName(module);
    for(auto& exported: packageExports) {
        if(exported == name) return true;
    }

    return false;
}
