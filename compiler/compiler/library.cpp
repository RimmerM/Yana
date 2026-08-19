#include "context.h"
#include <File.h>
#include <stdlib.h>

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
    return File::exists(formatPath("%@/Core.yana"_v, candidate));
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

    // `-lib` first and unconditionally: a path that was named and is wrong is a mistake to see
    // rather than one to fall through, so this one is taken whether or not it holds a Core - the
    // "cannot find the standard library" report then names the directory the caller asked for.
    if(context.settings.libraryPath != "") {
        root = context.settings.libraryPath;
        return root;
    }

    if(auto fromEnvironment = getenv("YANA_LIB")) {
        auto candidate = String(fromEnvironment);
        if(holdsLibrary(candidate)) {
            root = candidate;
            return root;
        }
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

StringView LibrarySource::source(Context& context, StringId module) {
    if(auto cached = loaded(module); cached.length) return cached;

    auto& base = directory(context);
    if(base == "") return {};

    // The name as a path: `Native.Linux` is `Native/Linux.yana`. The identifier is the interned
    // text, so this is the module map's rule read backwards and there is no second naming
    // convention to keep in step with it.
    auto name = context.findName(module);
    if(name.size() == 0) return {};

    char relative[512];
    if(name.size() + 6 > sizeof(relative)) return {};

    Size last = 0;
    for(Size i = 0; i < name.size(); i++) {
        auto c = name.text()[i];
        if(c == '.') {
            relative[i] = '/';
            last = i + 1;
        } else {
            relative[i] = c;
        }
    }

    auto path = formatPath("%@/%@.yana"_v, base, String(relative, name.size()));

    // The index form, for a module that has grown into a directory: `Native/Native.yana` is
    // `Native` exactly as `mapFile` would name it. Second, so the flat file wins where both exist -
    // which is the same precedence a source tree gives, since two files that map to one name are a
    // duplicate the module map rejects outright.
    if(!File::exists(path)) {
        auto tail = String(relative + last, name.size() - last);
        auto indexed = formatPath("%@/%@/%@.yana"_v, base, String(relative, name.size()), tail);
        if(!File::exists(indexed)) return {};

        path = indexed;
    }

    auto opened = File::openFile(path, readAccess(), File::OpenExisting);
    if(opened.isErr()) {
        context.diagnostics.error("cannot open standard library module %@ at %@: %@"_v, nullptr,
                                  name, path, describeError(opened.unwrapErr()));
        return {};
    }

    auto file = opened.moveUnwrapOk();
    auto size = file.size();
    Ptr<char, HeapDeleter> text { (char*)hAlloc(size) };

    if(size && file.read({ (Byte*)text.get(), size }).isErr()) {
        context.diagnostics.error("cannot read standard library module %@ at %@"_v, nullptr, name, path);
        return {};
    }

    auto& entry = *entries.push();
    entry.name = module;
    entry.path = ::move(path);
    entry.text = ::move(text);
    entry.length = size;

    return StringView { entry.text.get(), entry.length };
}

StringView LibrarySource::loaded(StringId module) const {
    for(auto& entry: entries) {
        if(entry.name == module) return StringView { entry.text.get(), entry.length };
    }

    return {};
}
