#pragma once

#include "../resolve/module.h"
#include "settings.h"

struct SourceEntry {
    SourceEntry(StringView path, Identifier id, Ptr<char> buffer): path(path), id(id), buffer(::move(buffer)) {}
    SourceEntry(SourceEntry&& other) noexcept: path(other.path), id(other.id), buffer(::move(other.buffer)) {}

    StringView path;
    Identifier id;
    Ptr<char> buffer;

    Ptr<ast::Module> ast = nullptr;
    Ptr<Module> ir = nullptr;
};

struct ModuleMap {
    Array<SourceEntry> entries;
};

Result<void, String> buildModuleMap(ModuleMap& map, const String& root);
Result<void, String> buildModuleMap(ModuleMap& map, const CompileSettings& settings);

struct FileProvider: ModuleProvider, SourceProvider {
    ModuleMap& moduleMap;
    Context* context;
    Module* core;
    Module* native;
    HashMap<StringId, StringView> sourceMap;

    explicit FileProvider(ModuleMap& map);
    StringView getSource(StringId module) override;
    const Location* getNode(LocationId id) override;
    Module* getModule(Module* from, StringId name) override;
};
