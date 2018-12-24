#pragma once

#include "../resolve/module.h"
#include "settings.h"

struct SourceEntry {
    SourceEntry(String path, Identifier id, void* buffer): path(move(path)), id(id), buffer(buffer) {}

    SourceEntry(SourceEntry&& other): path(other.path), id(other.id), buffer(other.buffer) {
        other.buffer = nullptr;
    }

    ~SourceEntry() {
        hFree(buffer);
    }

    String path;
    Identifier id;
    void* buffer;

    ast::Module* ast = nullptr;
    Module* ir = nullptr;
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
    HashMap<StringBuffer, Id> sourceMap;

    explicit FileProvider(ModuleMap& map);
    StringBuffer getSource(Id module) override;
    Module* getModule(Module* from, Id name) override;
};
