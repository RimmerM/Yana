#include "source.h"
#include "../parse/parser.h"
#include "Mem/Hash.h"
#include <File.h>

SourceEntry* ModuleMap::find(StringId name) {
    for(auto& entry: entries) {
        if(entry.name == name) return &entry;
    }

    return nullptr;
}

SourceEntry* ModuleMap::find(const String& identifier) {
    for(auto& entry: entries) {
        if(String(entry.id.text, entry.id.textLength) == identifier) return &entry;
    }

    return nullptr;
}

ModuleGroup* ModuleMap::findGroup(StringId name) {
    for(auto& group: groups) {
        if(group.name == name) return &group;
    }

    return nullptr;
}

ModuleGroup* ModuleMap::findGroup(const String& identifier) {
    for(auto& group: groups) {
        if(group.text == identifier) return &group;
    }

    return nullptr;
}

// The module a compile root's own directory forms, or none when the compilation has several roots
// and so several of them. A program has one root module, and where there is a choice it is named.
ModuleGroup* ModuleMap::rootGroup() {
    ModuleGroup* found = nullptr;
    for(auto& group: groups) {
        if(!group.root) continue;
        if(found) return nullptr;

        found = &group;
    }

    return found;
}

/*
 * Which module each file belongs to - Analysis-Modules.md §2.1.
 *
 * Three cases and only two of them are written down. A file that declared nothing belongs to the
 * module its directory forms; `module` makes it a module of its own, named by its path; `module M`
 * joins M. The prefix restriction on the third is checked here rather than in the parser, because a
 * parse knows the name it was handed and nothing about where the file sits - and the restriction is
 * entirely about where the file sits. Without it, answering "what is in module `Core`" would mean
 * reading every file in the program rather than the ones at or under `Core/`.
 */
static void groupFile(Context& context, ModuleMap& map, SourceEntry& entry) {
    auto& root = map.roots[entry.root];
    auto ast = entry.ast.get();
    auto membership = ast ? ast->membership : ast::Membership::Directory;

    // A file named as a compile object of its own is a module whatever it wrote: it has no
    // directory of this compilation's to belong to, and nothing else was mapped beside it.
    if(root.isFile) membership = ast::Membership::Own;

    if(membership == ast::Membership::Own) {
        entry.module = entry.name;
        return;
    }

    if(membership == ast::Membership::Named) {
        auto joins = context.find(ast->joins);
        auto id = String(entry.id.text, entry.id.textLength);
        auto named = String(joins.text, joins.textLength);

        // A *proper* prefix, and on a segment boundary: `module Core` from `Core/Float/Ryu.yana` is
        // the case this exists for, and `module Cor` names nothing.
        auto prefix = named.size() < id.size() && id.text()[named.size()] == '.' &&
                      compareMem(id.text(), named.text(), named.size()) == 0;

        if(prefix) {
            entry.module = ast->joins;
            return;
        }

        context.diagnostics.error("`module %@` may only name a module this file is under - %@ is %@, so `module` alone or one of its prefixes is what it may join"_v,
                                  ast->membershipSource, named, toString(entry.path), id);

        // Its own module rather than its directory's, so that the file's declarations still exist
        // somewhere addressable and the rest of the program reports what it cannot find rather than
        // finding it under a name nobody wrote.
        entry.module = entry.name;
        return;
    }

    // The directory's module. For a file directly in a compile root that is the root's own name,
    // since every identifier here is relative to the root and so has the root's name cut off.
    if(entry.directoryLength) {
        entry.module = context.addQualifiedName(entry.id.text, entry.directoryLength);
    } else if(root.name != "") {
        entry.module = context.addQualifiedName(root.name.text(), root.name.size());
    } else {
        // A root with no name to take - `.`, or a path ending in a separator. The file is its own
        // module, which is what it was before directories were modules.
        entry.module = entry.name;
    }
}

void FileProvider::prepare(Context& target) {
    context = &target;

    for(auto& entry: moduleMap.entries) {
        entry.name = target.addIdentifier(entry.id);
    }

    // Every file, before any grouping: which module a file belongs to is written in the file, so
    // there is no grouping without having read them all. See the comment on this in source.h.
    for(auto& entry: moduleMap.entries) parse(entry);

    for(auto& entry: moduleMap.entries) groupFile(target, moduleMap, entry);

    moduleMap.groups.clear();

    for(Size i = 0; i < moduleMap.entries.size(); i++) {
        auto& entry = moduleMap.entries[i];
        auto group = moduleMap.findGroup(entry.module);

        if(!group) {
            auto name = target.find(entry.module);
            moduleMap.groups.push(ModuleGroup {
                .name = entry.module,
                .text = String(name.text, name.textLength),
            });

            group = &moduleMap.groups[moduleMap.groups.size() - 1];
        }

        // A group is the compile root's own module when any file of it sits directly in a root
        // directory. That is what a program with no `-root` starts from.
        if(!entry.directoryLength && !moduleMap.roots[entry.root].isFile) group->root = true;

        group->files.push(i);
    }

    // In map order, which is directory-walk order and therefore path order within a directory. The
    // pointers are taken after every group is complete, since pushing a group moves the array.
    for(auto& group: moduleMap.groups) {
        group.parsed.name = group.name;
        group.parsed.files.clear();

        for(auto index: group.files) {
            if(auto ast = moduleMap.entries[index].ast.get()) group.parsed.files.push(ast);
        }
    }
}

StringView FileProvider::getSource(StringId module) {
    auto entry = moduleMap.find(module);
    if(!entry || !entry->text) return {};

    return StringView { entry->text.get(), entry->length };
}

const Location* FileProvider::getNode(LocationId id) {
    return context ? context->getLocation(id) : nullptr;
}

// Core and Native are built into the compiler and resolved before anything asks for them, so a name
// reaching here is one an `import` in user source named. A module the source tree has no file for
// is not an error to report from here: the resolver reports it against the import that named it.
//
// A group rather than a file, and every file of it is already parsed - see prepare.
ast::ModuleGroup* FileProvider::getModule(StringId name) {
    auto group = moduleMap.findGroup(name);
    return group ? &group->parsed : nullptr;
}

bool FileProvider::loadText(SourceEntry& entry) {
    if(entry.text) return true;

    auto opened = File::openFile(toString(entry.path), readAccess(), File::OpenExisting);
    if(opened.isErr()) {
        context->diagnostics.error("cannot open file %@: error %@"_v, nullptr,
                                   toString(entry.path), (U32)opened.unwrapErr());
        return false;
    }

    auto file = opened.moveUnwrapOk();
    auto size = file.size();
    Ptr<char, HeapDeleter> text { (char*)hAlloc(size) };

    if(size && file.read({ (Byte*)text.get(), size }).isErr()) {
        context->diagnostics.error("cannot read file %@"_v, nullptr, toString(entry.path));
        return false;
    }

    // Kept for as long as the compilation runs: a diagnostic quotes the line it points at, and it
    // may be reported long after the module it is in was parsed.
    entry.text = ::move(text);
    entry.length = size;
    return true;
}

ast::Module* FileProvider::parse(SourceEntry& entry) {
    if(entry.ast) return entry.ast.get();
    if(!loadText(entry)) return nullptr;

    Lexer lexer(*context, context->diagnostics, StringView { entry.text.get(), entry.length }, entry.name);
    Parser parser(*context, lexer, entry.name);
    entry.ast = Ptr(new ast::Module(parser.parseModule()));

    return entry.ast.get();
}

void FileProvider::reset() {
    for(auto& entry: moduleMap.entries) {
        entry.ast = nullptr;
        entry.text = nullptr;
        entry.length = 0;
        entry.module = StringId();
    }

    // The groups hold ast::Module pointers, so they go with the ASTs rather than surviving as
    // handles to freed nodes. `prepare` rebuilds them on the next compile.
    moduleMap.groups.clear();
}

template<class... T>
static String formatError(StringView format, T&&... args) {
    char buffer[4000];
    auto length = Tritium::format(toBuffer(buffer), toString(format), forward<T>(args)...);
    return ownedString(buffer, length);
}

static const char* findLast(const String& path, char search) {
    int offset = -1;
    for(int i = path.size() - 1; i >= 0; i--) {
        if(path.text()[i] == search) {
            offset = i;
            break;
        }
    }

    if(offset >= 0) {
        return path.text() + offset;
    } else {
        return nullptr;
    }
}

static bool isYanaSource(const String& path, Size& extensionLength) {
    // Find extension start.
    auto found = findLast(path, '.');
    if(!found || found == path.text()) return false;

    found++;
    extensionLength = path.text() + path.size() - found;

    String extension(found, extensionLength);
    return extension == "yana";
}

/*
 * Whether this compilation selects a project file by its name - Analysis-Modules.md §2.5, and
 * Design-Test.md §3.1's `test`.
 *
 * The same question `walkModuleFiles` asks of a library file, asked of a project one, with one
 * deliberate difference: a segment that is not a selector at all leaves the file *in*. The library
 * reports one, on the argument that a library file name is not otherwise dotted - a project's may
 * well be, and turning every existing `Data.Helpers.yana` into a diagnostic is a change to what
 * compiles rather than a test-framework feature. What this decides is only what the selector
 * vocabulary already decided elsewhere: `Map.test.yana` is a file of its module under `-test` and is
 * not read at all without it.
 */
static bool selectsFile(const CompileSettings& settings, const char* name, Size length) {
    Size from = 0;

    for(Size i = 0; i <= length; i++) {
        if(i < length && name[i] != '.') continue;

        if(from > 0 && targetSelector(settings, StringView { name + from, i - from }) == TargetSelector::Excluded) {
            return false;
        }

        from = i + 1;
    }

    return true;
}

/*
 * A path becomes a file identifier, and a directory split.
 *
 * The index-module special case is gone - Analysis-Modules.md §2.1.2. `Data/Map/Map.yana` taking
 * the directory's name existed because a directory was not a module and there was no way to say
 * otherwise; both are fixed, and the file is now `Data.Map.Map` like every other file, sitting in
 * the module `Data.Map` that its directory forms.
 *
 * What is new is `directoryLength`: how much of the identifier names the file's directory. The
 * directory's name is a prefix of the file's, so it is a length rather than a second buffer, and
 * the segment offsets and hashes a qualified name needs are a prefix of the file's too.
 */
static void mapFile(ModuleMap& map, U32 rootIndex, const String& file) {
    auto& root = map.roots[rootIndex].path;

    // Only compile actual source files.
    Size extensionLength = 0;
    if(!isYanaSource(file, extensionLength)) return;

    /*
     * The selectors, read off the file's own name rather than off the path.
     *
     * The base name only: a *directory* separator and a selector segment are both spelled with a dot
     * in an identifier, so asking this of the whole path would read `Data/Map/Map.yana`'s directories
     * as selectors. The walk that found the file is what still knows which is which, and this runs
     * before the identifier is built for exactly that reason.
     */
    if(map.settings) {
        auto base = findLast(file, '/');
        if(!base) base = findLast(file, '\\');
        base = base ? base + 1 : file.text();

        auto stem = file.text() + file.size() - extensionLength - 1;
        if(stem > base && !selectsFile(*map.settings, base, Size(stem - base))) return;
    }

    // Only compile files that are actually inside the root directory.
    if(root.size() > file.size() || compareMem(root.text(), file.text(), root.size()) != 0) return;

    // Find the number of identifier segments we have to reserve space for.
    // We already know that the file name ends in a valid extension and is at least 1 character long.
    const char* idStart;
    Size idLength;
    Size segmentCount = 1;

    // If the file _is_ the root, we just take its name.
    if(root.size() == file.size()) {
        idStart = findLast(file, '/');
        if(!idStart) idStart = findLast(file, '\\');

        if(idStart) {
            idStart++;
        } else {
            idStart = file.text();
        }
    } else {
        // If not, calculate the number of directories between the root and the file.
        idStart = file.text() + root.size() + 1;

        for(Size i = root.size() + 1; i < file.size(); i++) {
            if(file.text()[i] == '/' || file.text()[i] == '\\') {
                segmentCount++;
            }
        }
    }

    idLength = file.text() + file.size() - idStart - extensionLength - 1;

    // Copy the full path and file identifier into a new buffer.
    Ptr<char> buffer { (char*)hAlloc(file.size() + idLength + 2 * sizeof(U32) * segmentCount) };

    // Put the indexes and hashes first to get the correct alignment.
    auto indexBuffer = (U32*)buffer.get();
    auto hashBuffer = indexBuffer + segmentCount;

    // Copy the full path.
    auto pathBuffer = (char*)(hashBuffer + segmentCount);
    copy(file.text(), pathBuffer, file.size());

    // Create the identifier.
    auto idBuffer = pathBuffer + file.size();
    for(Size i = 0; i < idLength; i++) {
        if(idStart[i] == '/' || idStart[i] == '\\') {
            idBuffer[i] = '.';
        } else {
            idBuffer[i] = idStart[i];
        }
    }

    Identifier id;
    id.text = idBuffer;
    id.textLength = idLength;
    id.segmentCount = segmentCount;

    if(segmentCount > 1) {
        /*
         * The two arrays are pointed at their storage *before* they are written, and the walk is
         * bounded by the buffer it walks.
         *
         * Both were wrong, and both were invisible for the same reason: `id.segments[i]` was written
         * through an uninitialized pointer that was assigned its buffer after the loop, and `max`
         * bounded `p` - which walks `idBuffer` - by the end of `idStart`, which is a different
         * allocation entirely. Whether either crashed depended on where the heap happened to be, so
         * a module map with a nested directory in it segfaulted on about one run in four under ASLR
         * and never once under a debugger. Every server start and every rescan builds one.
         */
        id.segments = indexBuffer;
        id.segmentHashes = hashBuffer;

        // Calculate the offsets and hashes.
        auto p = idBuffer;
        auto max = idBuffer + idLength;
        for(U32 i = 0; i < segmentCount; i++) {
            id.segments[i] = (U32)(p - idBuffer);

            // Segments always start with an uppercase letter.
            *p = toUpper(*p);

            Tritium::Hasher hash;
            U32 segmentLength = 0;
            while(p < max && *p != '.') {
                hash.addByte(*p);
                p++;
                segmentLength++;
            }

            if(p < max && *p == '.') p++;
            id.segmentHashes[i] = hash.get();
        }
    } else {
        // Module names always start with an uppercase letter.
        idBuffer[0] = toUpper(idBuffer[0]);

        Hasher hash;
        hash.addBytes(idBuffer, idLength);

        id.segments = nullptr;
        id.segmentHash = hash.get();
    }

    // Where the file's own segment starts, which is where its directory's name ends. Zero for a
    // file directly in the compile root: its directory is the root, which the path says nothing
    // about because every identifier here is relative to it.
    U32 directoryLength = segmentCount > 1 ? (id.segments[segmentCount - 1] - 1) : 0;

    map.entries.push(SourceEntry {
        StringView { pathBuffer, file.size() }, id, directoryLength, rootIndex, ::move(buffer)
    });
}

static Result<void, String> mapDirectory(ModuleMap& map, U32 rootIndex, const String& dir) {
    Result<void, String> error = Ok();

    auto result = listDirectory(dir, [&](const String& name, bool isDirectory) {
        char pathBuffer[4000];
        String path(pathBuffer, format(toBuffer(pathBuffer), "%@/%@", dir, name));

        if(isDirectory) {
            if(name != "." && name != "..") {
                auto result = mapDirectory(map, rootIndex, path);
                if(!result) error = Err(result.moveUnwrapErr());
            }
        } else {
            mapFile(map, rootIndex, path);
        }
    });

    if(!result) {
        return Err(formatError("Cannot list source directory %@"_v, dir));
    }

    return move(error);
}

// Two files that map to one identifier. Still an error and for the reason it always was - a module
// map is a name-to-file lookup - but the identifier is now the *file's*, so two files of one module
// no longer collide. That is the whole of what grouping needed from here.
static Result<void, String> checkDuplicates(ModuleMap& map) {
    for(auto& entry: map.entries) {
        auto id = String(entry.id.text, entry.id.textLength);
        for(auto& compare: map.entries) {
            if(&entry == &compare) continue;

            if(id == String(compare.id.text, compare.id.textLength)) {
                return Err(formatError(
                    "Duplicate modules found in %@ and %@. Each module needs to have a unique identifier. "
                    "Consider changing the name of the file or the directory it resides in."_v,
                    entry.path, compare.path
                ));
            }
        }
    }

    return Ok();
}

// The name of the module a compile root's own files form: the directory's basename, with the
// leading letter raised the way every other module name segment is. A root named as a file has no
// such module - the file is one on its own - so this is not asked for one.
static String rootModuleName(const String& root) {
    auto start = findLast(root, '/');
    if(!start) start = findLast(root, '\\');

    auto text = start ? start + 1 : root.text();
    auto length = root.text() + root.size() - text;

    // A root written with a trailing separator, or as `.`, has nothing to take a name from. The
    // empty answer is handled by the grouping, which falls back to the file's own name.
    if(length <= 0 || (length == 1 && text[0] == '.')) return String();

    char buffer[512];
    if(Size(length) >= sizeof(buffer)) return String();

    copy(text, buffer, length);
    buffer[0] = toUpper(buffer[0]);
    return ownedString(buffer, length);
}

Result<void, String> buildModuleMap(ModuleMap& map, const String& root) {
    auto info = File::info(root);
    if(!info) return Err(formatError("Cannot open source file/directory %@: %@"_v, root, describeError(info.unwrapErr())));

    auto isDirectory = info.unwrapOk().isDirectory;
    auto index = U32(map.roots.size());

    map.roots.push(SourceRoot {
        .path = root,
        .name = isDirectory ? rootModuleName(root) : String(),
        .isFile = !isDirectory,
    });

    if(isDirectory) {
        return mapDirectory(map, index, root);
    } else {
        mapFile(map, index, root);
        return Ok();
    }
}

Result<void, String> buildModuleMap(ModuleMap& map, const CompileSettings& settings) {
    // Before the roots are walked, because it is what decides which files the walk keeps - see
    // selectsFile. A `-add` that names a single file goes through the same test.
    map.settings = &settings;

    for(auto& root: settings.compileObjects) {
        auto result = buildModuleMap(map, root);
        if(result.isErr()) {
            return result;
        }
    }

    return checkDuplicates(map);
}
