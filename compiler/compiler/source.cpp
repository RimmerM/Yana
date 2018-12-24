#include "source.h"
#include "../resolve/builtins.h"
#include <File.h>

FileProvider::FileProvider(ModuleMap& map): moduleMap(map) {}

StringBuffer FileProvider::getSource(Id module) {
    if(auto source = sourceMap.get(module)) {
        return *source.get();
    }

    for(auto& e: moduleMap.entries) {

    }
}

Module* FileProvider::getModule(Module* from, Id name) {
    if(name == core->id) {
        if(!core) core = coreModule(context);
        return core;
    } else if(name == native->id) {
        if(!core) core = coreModule(context);
        if(!native) native = nativeModule(context, core);
        return native;
    } else {
        return nullptr;
    }
}

template<class... T>
static String formatError(StringBuffer format, T&&... args) {
    char buffer[4000];
    auto end = formatString(toBuffer(buffer), format, forward<T>(args)...);
    return ownedString(buffer, end - buffer);
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

static void mapFile(ModuleMap& map, const String& root, const String& file) {
    // Only compile actual source files.
    Size extensionLength = 0;
    if(!isYanaSource(file, extensionLength)) return;

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
            if(idStart[i] == '/' || idStart[i] == '\\') {
                segmentCount++;
            }
        }
    }

    idLength = file.text() + file.size() - idStart - extensionLength - 1;

    // If the module has the same name as the directory it resides in,
    // it functions as an "index module" and takes the identifier of the directory.
    auto moduleName = findLast(file, '/');
    if(!moduleName) moduleName = findLast(file, '\\');

    if(moduleName && file.size() > root.size()) {
        auto directory = String(file.text(), moduleName - file.text());
        auto directoryName = findLast(directory, '/');
        if(!directoryName) directoryName = findLast(directory, '\\');

        if(directoryName) {
            directoryName++;
            auto directoryLength = directory.text() + directory.size() - directoryName;

            moduleName++;
            auto moduleLength = file.text() + file.size() - moduleName - extensionLength - 1;

            if(directoryLength == moduleLength && compareMem(directoryName, moduleName, moduleLength) == 0) {
                segmentCount--;
                idLength -= moduleLength + 1;
            }
        }
    }

    // Copy the full path and file identifier into a new buffer.
    auto buffer = (char*)hAlloc(file.size() + idLength + 2 * sizeof(U32) * segmentCount);

    // Put the indexes and hashes first to get the correct alignment.
    auto indexBuffer = (U32*)buffer;
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
        // Calculate the offsets and hashes.
        auto p = idBuffer;
        auto max = idStart + idLength;
        for(U32 i = 0; i < segmentCount; i++) {
            id.segments[i] = (U32)(p - idBuffer);

            // Segments always start with an uppercase letter.
            *p = toUpper(*p);

            Hasher hash;
            U32 segmentLength = 0;
            while(p < max && *p != '.') {
                hash.addByte(*p);
                p++;
                segmentLength++;
            }

            if(p < max && *p == '.') p++;
            id.segmentHashes[i] = hash.get();
        }

        id.segments = indexBuffer;
        id.segmentHashes = hashBuffer;
    } else {
        // Module names always start with an uppercase letter.
        idBuffer[0] = toUpper(idBuffer[0]);

        Hasher hash;
        hash.addBytes(idBuffer, idLength);

        id.segments = nullptr;
        id.segmentHash = hash.get();
    }

    auto entry = map.entries.push(SourceEntry{String(pathBuffer, file.size()), id, buffer});
}

static Result<void, String> mapDirectory(ModuleMap& map, const String& root, const String& dir) {
    Result<void, String> error = Ok();

    auto result = listDirectory(dir, [&](const String& name, bool isDirectory) {
        char pathBuffer[4000];
        String path(pathBuffer, formatString(toBuffer(pathBuffer), "%@/%@"_buffer, dir, name) - pathBuffer);

        if(isDirectory) {
            if(name != "." && name != "..") {
                auto result = mapDirectory(map, root, path);
                if(!result) error = Err(result.moveUnwrapErr());
            }
        } else {
            mapFile(map, root, path);
        }
    });

    if(!result) {
        return Err(formatError("Cannot list source directory %@"_buffer, dir));
    }

    return move(error);
}

static Result<void, String> checkDuplicates(ModuleMap& map) {
    for(auto& entry: map.entries) {
        auto id = String(entry.id.text, entry.id.textLength);
        for(auto& compare: map.entries) {
            if(&entry == &compare) continue;

            if(id == String(compare.id.text, compare.id.textLength)) {
                return Err(formatError(
                    "Duplicate modules found in %@ and %@. Each module needs to have a unique identifier. "
                    "Consider changing the name of the file or the directory it resides in."_buffer,
                    entry.path, compare.path
                ));
            }
        }
    }

    return Ok();
}

Result<void, String> buildModuleMap(ModuleMap& map, const String& root) {
    auto info = File::info(root);
    if(!info) return Err(formatError("Cannot open source file/directory %@"_buffer, root));

    if(info.unwrap().isDirectory) {
        return mapDirectory(map, root, root);
    } else {
        mapFile(map, root, root);
        return Ok();
    }
}

Result<void, String> buildModuleMap(ModuleMap& map, const CompileSettings& settings) {
    for(auto& root: settings.compileObjects) {
        auto result = buildModuleMap(map, root);
        if(result.isErr()) {
            return result;
        }
    }

    return checkDuplicates(map);
}