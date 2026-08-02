#include "project.h"
#include <File.h>

using namespace Tritium;

template<class... T>
static String formatError(StringView format, T&&... args) {
    char buffer[4000];
    auto length = Tritium::format(toBuffer(buffer), toString(format), forward<T>(args)...);
    return Tritium::ownedString(buffer, length);
}

static bool isSeparator(char c) {
    return c == '/' || c == '\\';
}

static bool isAbsolute(const String& path) {
    if(path.size() == 0) return false;
    if(isSeparator(path.text()[0])) return true;

    // A Windows drive letter. Checked because a project file is shared between machines far more
    // often than it is shared between operating systems, and an absolute path in one is a relative
    // path in the other only if nobody looks.
    return path.size() >= 2 && path.text()[1] == ':';
}

String joinProjectPath(const String& directory, const String& relative) {
    if(isAbsolute(relative) || directory == "") return relative;

    auto trimmed = directory.size();
    while(trimmed > 1 && isSeparator(directory.text()[trimmed - 1])) trimmed--;

    StringBuilder path(trimmed + relative.size() + 1);
    path.append(directory.text(), trimmed);
    path.append("/"_v);
    path.append(relative.text(), relative.size());

    return path.string();
}

// The directory a file is in. Removes the last segment, and fails when the path is one segment -
// "yana.toml" names a file in the working directory, and this cannot say which directory that is.
static bool directoryOf(String& path) {
    auto text = path.text();
    auto end = path.size();

    while(end > 0 && isSeparator(text[end - 1])) end--;
    if(end == 0) return false;

    while(end > 0 && !isSeparator(text[end - 1])) end--;
    if(end == 0) return false;

    while(end > 1 && isSeparator(text[end - 1])) end--;

    path = Tritium::ownedString(text, end);
    return true;
}

// The directory above one, or false at the top.
//
// Two paths have a parent that removing text cannot name: a single relative segment, and one that
// already ends in `..`. Those get `/..` appended instead, which is why this is not just
// directoryOf - a search that started at "." would otherwise look in one directory and stop. The
// walk is bounded by its own depth and every step asks the file system, so climbing past the real
// root finds nothing rather than looping.
static bool parentDirectory(String& path) {
    auto text = path.text();
    auto end = path.size();

    while(end > 1 && isSeparator(text[end - 1])) end--;
    if(end == 0) return false;
    if(end == 1 && isSeparator(text[0])) return false;

    auto segment = end;
    while(segment > 0 && !isSeparator(text[segment - 1])) segment--;

    auto length = end - segment;
    auto climbs = length == 2 && text[segment] == '.' && text[segment + 1] == '.';

    if(segment == 0 || climbs) {
        path = joinProjectPath(path, String(".."));
        return true;
    }

    end = segment;
    while(end > 1 && isSeparator(text[end - 1])) end--;

    path = Tritium::ownedString(text, end);
    return true;
}

Maybe<String> findProjectFile(const String& directory, U32 maxDepth) {
    auto current = directory == "" ? String(".") : directory;

    for(U32 i = 0; i < maxDepth; i++) {
        auto candidate = joinProjectPath(current, String("yana.toml"));
        if(Tritium::File::exists(candidate)) return Just(::move(candidate));
        if(!parentDirectory(current)) break;
    }

    return Nothing();
}

Maybe<String> locateProjectFile(const CompileSettings& settings) {
    if(settings.noProject) return Nothing();

    if(settings.projectFile == "") {
        return findProjectFile(String("."));
    }

    // A directory is the friendlier thing to have been given - it is what an editor knows about a
    // project - so it is tried first, and the path is taken literally only when it holds no file
    // of its own. Naming a file that is not there is an error the caller reports; answering
    // Nothing here would report it as "no project file anywhere", which is a different thing.
    auto inDirectory = joinProjectPath(settings.projectFile, String("yana.toml"));
    if(Tritium::File::exists(inDirectory)) return Just(::move(inDirectory));
    if(Tritium::File::exists(settings.projectFile)) return Just(String(settings.projectFile));

    return Nothing();
}

/*
 * The reader.
 *
 * A subset of TOML rather than the format: what this file has to express is three keys and a list
 * of paths, and every line of syntax accepted here is a line somebody can write that the driver and
 * the server then have to agree on the meaning of. Anything under a `[table]` header is skipped
 * without being understood, which is the one deliberate piece of tolerance - a project may carry
 * settings for a tool that is not this one.
 */
struct TomlReader {
    TomlReader(const char* text, Size length): p(text), max(text + length) {}

    const char* p;
    const char* max;
    U32 line = 1;
    String error;

    bool hasError() const { return error != ""; }

    void fail(StringView message) {
        if(!hasError()) error = formatError("%@ on line %@"_v, toString(message), line);
    }

    void skipSpace(bool overNewlines) {
        while(p < max) {
            auto c = *p;
            if(c == '\n') {
                if(!overNewlines) return;
                line++;
                p++;
            } else if(c == ' ' || c == '\t' || c == '\r') {
                p++;
            } else if(c == '#') {
                while(p < max && *p != '\n') p++;
            } else {
                return;
            }
        }
    }

    String readString() {
        if(p >= max || *p != '"') {
            fail("expected a quoted string"_v);
            return {};
        }

        p++;
        StringBuilder text;
        while(p < max && *p != '"') {
            auto c = *p++;
            if(c == '\n') {
                fail("unterminated string"_v);
                return {};
            }

            if(c == '\\' && p < max) {
                auto escape = *p++;
                switch(escape) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case '"': c = '"'; break;
                    case '\\': c = '\\'; break;
                    default:
                        fail("unknown string escape"_v);
                        return {};
                }
            }

            text.append(&c, 1);
        }

        if(p >= max) {
            fail("unterminated string"_v);
            return {};
        }

        p++;
        return text.string();
    }

    // The name before the `=`. Bare keys only: a quoted key would be a key this reader has no
    // meaning for, and a key with no meaning is better reported than silently accepted.
    StringView readKey() {
        auto start = p;
        while(p < max) {
            auto c = *p;
            auto isBare = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                       || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            if(!isBare) break;
            p++;
        }

        return StringView { start, Size(p - start) };
    }
};

Result<ProjectFile, String> readProjectFile(const String& path) {
    auto opened = Tritium::File::openFile(path, readAccess(), Tritium::File::OpenExisting);
    if(opened.isErr()) {
        return Err(formatError("cannot open project file %@: %@"_v, path,
                               toString(describeError(opened.unwrapErr()))));
    }

    auto file = opened.moveUnwrapOk();
    auto size = file.size();
    Ptr<char, HeapDeleter> text { (char*)hAlloc(size + 1) };

    if(size && file.read({ (Byte*)text.get(), size }).isErr()) {
        return Err(formatError("cannot read project file %@"_v, path));
    }

    ProjectFile project;
    project.path = path;
    project.directory = path;
    if(!directoryOf(project.directory)) project.directory = String(".");

    TomlReader reader(text.get(), size);
    auto inTable = false;

    while(true) {
        reader.skipSpace(true);
        if(reader.p >= reader.max || reader.hasError()) break;

        if(*reader.p == '[') {
            while(reader.p < reader.max && *reader.p != ']' && *reader.p != '\n') reader.p++;
            if(reader.p >= reader.max || *reader.p != ']') {
                reader.fail("unterminated table header"_v);
                break;
            }

            reader.p++;
            inTable = true;
            continue;
        }

        auto key = reader.readKey();
        if(key.length == 0) {
            reader.fail("expected a key"_v);
            break;
        }

        reader.skipSpace(false);
        if(reader.p >= reader.max || *reader.p != '=') {
            reader.fail("expected '=' after a key"_v);
            break;
        }

        reader.p++;
        reader.skipSpace(false);

        // Everything below the first table header belongs to something else. The value still has to
        // be consumed rather than skipped by line, since an array can span several of them.
        Array<String> values;
        if(reader.p < reader.max && *reader.p == '[') {
            reader.p++;
            while(true) {
                reader.skipSpace(true);
                if(reader.p >= reader.max) {
                    reader.fail("unterminated array"_v);
                    break;
                }

                if(*reader.p == ']') {
                    reader.p++;
                    break;
                }

                values.push(reader.readString());
                if(reader.hasError()) break;

                reader.skipSpace(true);
                if(reader.p < reader.max && *reader.p == ',') reader.p++;
            }
        } else {
            values.push(reader.readString());
        }

        if(reader.hasError()) break;
        if(inTable) continue;

        if(key == "root"_v) {
            if(values.size() != 1) {
                reader.fail("root names one module"_v);
                break;
            }
            project.root = ::move(values[0]);
        } else if(key == "sources"_v) {
            for(auto& value: values) {
                project.sources.push(joinProjectPath(project.directory, value));
            }
        } else if(key == "output"_v) {
            if(values.size() != 1) {
                reader.fail("output names one directory"_v);
                break;
            }
            project.output = joinProjectPath(project.directory, values[0]);
        } else if(key == "target"_v) {
            if(values.size() != 1) {
                reader.fail("target names one output"_v);
                break;
            }

            // `native` rather than `exe` because that is what a project file is about - which
            // platform the code is for, and therefore which `@platform` declarations exist. The
            // driver's own spellings are accepted too so the two documents do not disagree.
            auto& value = values[0];
            if(value == "native" || value == "exe") project.target = Just(CompileMode::NativeExecutable);
            else if(value == "js") project.target = Just(CompileMode::JsExecutable);
            else if(value == "jslib") project.target = Just(CompileMode::JsLibrary);
            else if(value == "lib") project.target = Just(CompileMode::Library);
            else if(value == "shared") project.target = Just(CompileMode::NativeShared);
            else if(value == "llvm") project.target = Just(CompileMode::Llvm);
            else {
                reader.fail("unknown target: valid targets are native|js|jslib|lib|shared|llvm"_v);
                break;
            }
        } else {
            reader.fail(stringView(formatError("unknown key \"%@\""_v, toString(key))));
            break;
        }
    }

    if(reader.hasError()) {
        return Err(formatError("%@: %@"_v, path, reader.error));
    }

    return Ok(::move(project));
}

SourceEntry* findRootModule(ModuleMap& map, const CompileSettings& settings, String& error) {
    if(settings.rootObjects.size() > 1) {
        error = String("a program has one root module. Provide one with -root <module>.");
        return nullptr;
    }

    if(settings.rootObjects.size() == 1) {
        auto root = map.find(settings.rootObjects[0]);
        if(!root) error = formatError("cannot find root module %@"_v, settings.rootObjects[0]);
        return root;
    }

    // A single module needs no naming: it is the program. Every other module in the tree is
    // compiled because something imported it, which is why an unreferenced file is not an error.
    if(map.entries.size() == 1) return &map.entries[0];

    error = formatError("%@ modules were found and none was named as the root. "
                        "Provide one with -root <module>, or as `root` in a yana.toml."_v,
                        map.entries.size());
    return nullptr;
}

void applyProjectFile(CompileSettings& settings, const ProjectFile& project) {
    if(settings.compileObjects.size() == 0) {
        for(auto& source: project.sources) settings.compileObjects.push(source);
    }

    if(settings.rootObjects.size() == 0 && project.root != "") {
        settings.rootObjects.push(project.root);
    }

    if(!settings.explicitOutput && project.output != "") {
        settings.outputDir = project.output;
        settings.explicitOutput = true;
    }

    if(!settings.explicitMode && project.target) {
        settings.mode = project.target.unwrap();
        settings.explicitMode = true;
    }
}
