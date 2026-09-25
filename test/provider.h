#pragma once

/*
 * The module provider the fixture drivers share - `YanaResolveTest`, `YanaElfTest`, `YanaLibTest`
 * and, since J165, `YanaBench`.
 *
 * One, since J149. ElfTester carried a verbatim copy of ResolveTester's from J133 and LibTester a
 * third, different one for `lib/modules`, and the copies had already drifted: only LibTester's
 * loaded a library module's `.test` companion, so `YanaElfTest TestFileGlobals` built without
 * `GlobalInit.test.yana` and passed on the one case written in the fixture itself. `YanaBench` kept
 * a fourth until J165 and was benchmarking that same companion-less program. (`Tester.cpp` holds a
 * fifth, but is built by nothing and goes with `compiler/resolve` - J150 §4.)
 *
 * Supplies both the source text a diagnostic quotes and the modules an `import` names, out of one
 * directory - `resolve/modules` for a compiler fixture, `lib/modules` for a library one. A name is
 * answered by `<root>/<Name>.yana` where that file exists, and by every `.yana` file in
 * `<root>/<Name>/` where the directory does. The second form is what a *grouped* module is - a
 * module is a directory of files, Analysis-Modules.md §2.1 - and it is here so that a fixture can
 * exercise file-scoped imports, which are only observable across two files of one module.
 *
 * A file's own name is `<Name>.<Base>`, which is what the module map gives a project file and what a
 * `LocationId` is quoted through. It has to differ per file: which file a declaration was written in
 * is read back off its location, and two files sharing a name would be one file to that lookup.
 */

#include <Core.h>
#include <File.h>
#include "../compiler/parse/parser.h"

// In a namespace of its own only so that the body can say `String` rather than `Tritium::String`
// without a header putting `using namespace` into every driver that includes it.
namespace testProvider {
using namespace Tritium;

struct TestProvider: SourceProvider, ModuleProvider {
    // `Source` rather than `File`, which is Tritium's and is what `load` below opens one with.
    struct Source {
        StringId name;
        Ptr<char, HeapDeleter> text;
        Size length;
        ast::Module* ast;
    };

    struct Loaded {
        StringId name;
        Array<Source> files;
        ast::ModuleGroup* group;
    };

    StringView source;
    Context* context = nullptr;
    Array<Loaded> loaded;

    // Which directory an `import` is looked for in. Set per fixture by a driver that runs over both
    // corpora.
    String moduleRoot = String("resolve/modules");

    /*
     * Whether a single-file module's `<Name>.test.yana` joins it under `-test` - Design-Test.md
     * §3.1. The selector rule these files stand in for is the library walk's, which reads a
     * directory, and there is no directory here. It is how the corpus reaches a *library* module
     * holding a test, which is where a dynamic global is a thing the compiler had to be taught
     * rather than something it already did - so `lib/` turns it on and `resolve/` leaves it off.
     */
    bool testCompanions = false;

    ~TestProvider() override {
        for(auto& entry: loaded) {
            delete entry.group;
            for(auto& file: entry.files) delete file.ast;
        }
    }

    // `addQualifiedName` copies the text into the context's own arena, so a name made from a
    // temporary `String` is safe to keep (`compiler/compiler/context.cpp`).
    StringId intern(const String& text) {
        return context->addQualifiedName(text.text(), text.size());
    }

    StringView getSource(StringId id) override {
        for(auto& entry: loaded) {
            for(auto& file: entry.files) {
                if(file.name == id) return StringView { file.text.get(), file.length };
            }
        }

        return source;
    }

    const Location* getNode(LocationId id) override {
        return context ? context->getLocation(id) : nullptr;
    }

    // One file, parsed into `into`. False where it could not be read - which for the single-file
    // form is how "this directory does not answer that name" is said.
    bool load(Loaded& into, const String& path, StringId name, bool test = false) {
        // `OpenExisting`, which is not a detail: the default mode *creates* the file, so an import
        // this directory does not answer used to leave an empty module behind - and an empty module
        // then shadows the library one of that name on every run afterwards, silently, since the
        // project's own files are looked at first. `import Math` is what found it.
        auto opened = File::openFile(path, readAccess(), File::OpenExisting);
        if(opened.isErr()) return false;

        auto file = opened.moveUnwrapOk();
        auto size = file.size();
        Ptr<char, HeapDeleter> text { (char*)hAlloc(size) };
        file.read({ (Byte*)text.get(), size });

        Lexer lexer(*context, context->diagnostics, StringView { text.get(), size }, name);
        Parser parser(*context, lexer, name);
        auto ast = new ast::Module(parser.parseModule());

        // What the library walk records from the file's name, said here instead because a companion
        // has no directory to walk - see ast::Module::test.
        ast->test = test;

        into.group->files.push(ast);
        into.files.push(Source { name, ::move(text), size, ast });
        return true;
    }

    ast::ModuleGroup* getModule(StringId name) override {
        for(auto& entry: loaded) {
            if(entry.name == name) return entry.group;
        }

        auto text = context->findName(name);
        auto directory = moduleRoot + String("/") + text;

        auto& entry = *loaded.push();
        entry.name = name;
        entry.group = new ast::ModuleGroup { .name = name };

        // The directory form first, because a module that is a directory is the general case and the
        // single file is the one-file spelling of it. The names are sorted, which is what makes the
        // order the declaration passes read a grouped module in the same on every file system.
        Array<String> bases;
        listDirectory(directory, [&](const String& file, bool isDirectory) {
            if(isDirectory) return;
            auto dot = findLastChar(stringView(file), '.');
            if(!dot || String(dot + 1, file.text() + file.size() - dot - 1) != "yana") return;

            // Concatenated rather than sliced, because `String(pointer, length)` is a **view** and
            // `file` is the callback's parameter: a base name kept past the callback has to own its
            // bytes. The empty left operand is what makes `operator +` allocate one.
            bases.push(String("") + String(file.text(), Size(dot - file.text())));
        });

        for(Size i = 1; i < bases.size(); i++) {
            for(Size j = i; j > 0 && stringView(bases[j]) < stringView(bases[j - 1]); j--) {
                swap(bases[j], bases[j - 1]);
            }
        }

        // A `<Base>.test.yana` in the directory is a test file, as the library walk reads one: it
        // joins only a `-test` build of a corpus that loads companions, and is marked as one.
        auto companions = testCompanions && context->settings.test;
        for(auto& base: bases) {
            auto test = stringView(base).endsWith(".test"_v);
            if(test && !companions) continue;

            load(entry, directory + String("/") + base + String(".yana"),
                 intern(text + String(".") + base), test);
        }

        if(bases.isEmpty() && load(entry, directory + String(".yana"), name) && companions) {
            load(entry, directory + String(".test.yana"), intern(text + String(".test")), true);
        }

        // Cached either way, including the empty answer: a name this directory does not hold is
        // asked about once per import that writes it, and each of those would otherwise be a walk.
        if(entry.group->files.isEmpty()) {
            delete entry.group;
            entry.group = nullptr;
        }

        return entry.group;
    }
};

} // namespace testProvider

using testProvider::TestProvider;
