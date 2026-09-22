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
        return Err(format("no yana.toml was found in %@. "
                                "A project file is what says which files are in the program.",
                                rootPath));
    }

    projectPath = found.unwrap();
    auto reloaded = reloadProject();
    if(reloaded.isErr()) return reloaded;

    opened = true;
    return Ok();
}

Result<void, String> Session::reloadProject() {
    auto project = readProjectFile(projectPath);
    if(project.isErr()) return Err(project.moveUnwrapErr());

    settings = CompileSettings();
    settings.projectFile = rootPath;
    applyProjectFile(settings, project.unwrapOk());

    // A platform the file did not name still has to be one, because `@platform` selects which
    // declarations exist and there is no program that is both. Native is the default the driver
    // already has, and the status bar shows which one a session is - §5.4.
    if(!settings.explicitPlatform) {
        settings.platform = TargetPlatform::Native;
        settings.explicitPlatform = true;
    }

    if(!settings.explicitMode) {
        settings.mode = CompileMode::Executable;
        settings.explicitMode = true;
    }

    return rescan();
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

void Session::compile(CompletionRequest* completion, Cursor cursor) {
    // The order matters. The program holds arenas whose handles are offsets into them, so it can
    // outlive neither the context nor the provider; and the ASTs hold LocationIds, which index one
    // context's location array in the order that context created them. So everything goes and
    // everything is rebuilt - which is the whole of §5.3's v1 answer, and is measured to be fast
    // enough that the alternative is not worth its cost in invalidation.
    calls = nullptr;
    program = nullptr;
    context = nullptr;
    index = nullptr;
    positions.clear();

    diagnostics.reset();
    provider.reset();

    // `yana.toml` is part of the editor's project, not a one-time server setting. Re-read it before
    // every compile so changing `main` or `sources` takes effect without restarting CLion's server.
    auto reloaded = reloadProject();
    if(reloaded.isErr()) {
        diagnostics.error("%@"_v, nullptr, reloaded.unwrapErr());
        return;
    }

    context = Ptr(new Context(diagnostics));
    context->settings = settings;

    // The rewritten stage, which is the only one with an editor surface - `ssa::Pipeline` states the
    // same assignment for a build, and the setting is read below this.
    //
    // **Kept after the default flipped** (2026-09-22, J81), because what this overrides is not the
    // default: a project whose `yana.toml` says `resolver = "legacy"` is still edited here, and a
    // server that honoured it would have no index, no completion and no `explain` to answer from.
    // Which stage *built* it is that project's business; which stage answers the editor is not.
    context->settings.resolver = ResolverStage::Ownership;

    /*
     * **The project's own platform, since 2026-09-22 (J75).**
     *
     * This used to force `Native` whatever the project said, because the rewritten stage did not
     * resolve a JavaScript program and a session that asked for one would have reported every
     * declaration of the library's `@platform(js)` half as not resolving. It does resolve one now -
     * JSIR's whole point - so a `platform = "js"` project is answered as the JavaScript program it
     * is, which is what makes its own `@platform(js)` declarations and its `.js.yana` files
     * reachable by go-to-definition, hover and completion at all.
     *
     * Nothing is forced here now, and that is deliberate rather than an omission: `reloadProject`
     * already fills a platform in for a project file that named none, so there is no state in which
     * this is unset. Which platform a session is running as is what §5.4's status bar shows, and it
     * decides which program the answers are about - a `@platform(native)` declaration is genuinely
     * unknown in a JavaScript project, exactly as it is in a JavaScript build.
     */

    // Before anything is resolved, because it is what turns the recording sites on. A driver never
    // does this, which is the whole of what Implementation-Tooling.md §1.1 means by the cost being
    // opt-in.
    index = Ptr(new SemanticIndex());
    context->index = index.get();

    // Before the parse, because the sentinel is what the parser puts where the cursor is - §8.2.
    // The cursor's module is what makes it one module's concern: every other file in the project
    // parses exactly as it does in an ordinary compile.
    context->completion = completion;
    context->cursor = cursor;

    stale = completion != nullptr;

    provider.prepare(*context);

    String error;
    auto root = findRootModule(moduleMap, settings, error);
    if(!root) {
        diagnostics.error("%@"_v, nullptr, error);
        return;
    }

    program = mir::resolveProgram(*context, root->parsed, &provider);
}

void Session::complete(StringId module, U32 offset, CompletionRequest& request, U32& prefixStart) {
    Cursor cursor;
    cursor.module = module;
    cursor.offset = offset;

    // The cursor with nothing found is a position where the parser reached no production at all -
    // inside a string, inside a comment, in a declaration head. The prefix is then the cursor
    // itself, so a caller that asks for the text between it and the cursor gets nothing, which is
    // what an empty answer should look like.
    prefixStart = offset;

    compile(&request, cursor);

    if(context && context->cursor.wasParsed()) prefixStart = context->cursor.prefixStart;
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

CallSiteIndex* Session::callSites() {
    if(!program) return nullptr;

    if(!calls) {
        calls = Ptr(new CallSiteIndex());
        calls->build(*program);
    }

    return calls.get();
}

StringId Session::moduleOf(StringId file) {
    auto entry = moduleMap.find(file);
    return entry && entry->module ? entry->module : file;
}

StringView Session::pathOf(StringId module) {
    auto entry = moduleMap.find(module);
    if(entry) return entry->path;

    return context ? context->library.pathOf(module) : StringView {};
}

StringView Session::sourceOf(StringId module) {
    auto source = provider.getSource(module);
    if(source.length) return source;

    return context ? context->library.loaded(module) : StringView {};
}

StringId Session::moduleAtPath(StringView path) {
    if(auto entry = findEntry(path)) return entry->name;
    return context ? context->library.moduleAtPath(path) : StringId();
}

} // namespace lsp
