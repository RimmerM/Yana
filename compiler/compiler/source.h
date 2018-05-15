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

struct SourceMap {
    Array<SourceEntry> entries;
};

Result<void, String> buildSourceMap(SourceMap& map, const String& root);
Result<void, String> buildSourceMap(SourceMap& map, const CompileSettings& settings);

struct FileHandler: ModuleHandler {
    SourceMap& map;
    Module* core;
    Module* native;

    FileHandler(SourceMap& map, Context* context);
    Module* require(Context* context, Module* from, Id name) override;
};
