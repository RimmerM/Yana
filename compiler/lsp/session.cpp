#include "session.h"

namespace lsp {

Result<void, String> Session::open(StringView rootPathText) {
    rootPath = ownedString(rootPathText.ptr, rootPathText.length);

    // The project file is what says which files are in the program, and it is read by the same code
    // the driver reads it with - §5.1. Without one the server would have to guess, and a wrong
    // guess shows up as every cross-module reference being unresolved.
    settings = CompileSettings();
    settings.projectFile = rootPath;

    auto found = locateProjectFile(settings);
    if(!found) {
        return Err(format("no yana.toml was found in %@ or above it. "
                                "A project file is what says which files are in the program.",
                                rootPath));
    }

    projectPath = found.unwrap();
    auto project = readProjectFile(projectPath);
    if(project.isErr()) return Err(project.moveUnwrapErr());

    applyProjectFile(settings, project.unwrapOk());

    // A target the file did not name still has to be one, because `@platform` selects which
    // declarations exist and there is no program that is both. Native is the default the driver
    // already has, and the status bar shows which one a session is - §5.4.
    if(!settings.explicitMode) {
        settings.mode = CompileMode::NativeExecutable;
        settings.explicitMode = true;
    }

    auto scanned = rescan();
    if(scanned.isErr()) return scanned;

    opened = true;
    return Ok();
}

Result<void, String> Session::rescan() {
    moduleMap.entries.clear();

    auto result = buildModuleMap(moduleMap, settings);
    if(result.isErr()) return result;

    if(moduleMap.entries.size() == 0) {
        return Err(format("no Yana modules were found under the source directories %@ lists.",
                                projectPath));
    }

    return Ok();
}

void Session::compile() {
    // The order matters. The program holds arenas whose pointers are offsets into them, so it can
    // outlive neither the context nor the provider; and the ASTs hold LocationIds, which index one
    // context's location array in the order that context created them. So everything goes and
    // everything is rebuilt - which is the whole of §5.3's v1 answer, and is measured to be fast
    // enough that the alternative is not worth its cost in invalidation.
    program = nullptr;
    context = nullptr;
    index = nullptr;
    positions.clear();

    diagnostics.reset();
    provider.reset();

    context = Ptr(new Context(diagnostics));
    context->settings = settings;

    // Before anything is resolved, because it is what turns the recording sites on. A driver never
    // does this, which is the whole of what Implementation-Tooling.md §1.1 means by the cost being
    // opt-in.
    index = Ptr(new SemanticIndex());
    context->index = index.get();

    provider.prepare(*context);

    String error;
    auto root = findRootModule(moduleMap, settings, error);
    if(!root) {
        diagnostics.error("%@"_v, nullptr, error);
        return;
    }

    auto ast = provider.parse(*root);
    if(!ast) return;

    program = resolveProgram(*context, *ast, &provider);
}

PositionIndex* Session::positionsOf(StringId module) {
    if(!context || !module) return nullptr;

    for(auto& entry: positions) {
        if(entry->module == module) return &entry->index;
    }

    auto built = Ptr(new ModulePositions());
    built->module = module;
    built->index.build(*context, module);

    auto result = &built->index;
    positions.push(::move(built));
    return result;
}

const Reference* Session::referenceAt(StringId module, U32 offset) {
    if(!index) return nullptr;

    auto found = positionsOf(module);
    if(!found) return nullptr;

    /*
     * Innermost first, and outward until something answers.
     *
     * The innermost node at a position is often not the one a name was recorded against - a call
     * records the callee at the callee's own location, and a cursor inside an argument that means
     * nothing on its own should still say what call it is in. Walking out is what makes the answer
     * "the most specific thing that has one" rather than "nothing".
     */
    Array<LocationId> enclosing;
    found->findEnclosing(offset, enclosing);

    for(auto id: enclosing) {
        if(auto reference = index->findReference(id)) return reference;
    }

    return nullptr;
}

const Symbol* Session::definitionAt(StringId module, U32 offset) {
    if(!index) return nullptr;

    auto found = positionsOf(module);
    if(!found) return nullptr;

    Array<LocationId> enclosing;
    found->findEnclosing(offset, enclosing);

    for(auto id: enclosing) {
        if(auto symbol = index->findDefinition(id)) return symbol;
    }

    return nullptr;
}

StringView Session::pathOf(StringId module) {
    auto entry = moduleMap.find(module);
    return entry ? entry->path : StringView {};
}

} // namespace lsp
