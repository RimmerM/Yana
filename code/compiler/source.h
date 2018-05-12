#pragma once

#include "../resolve/module.h"
#include "settings.h"

struct FileHandler: ModuleHandler {
    Module* core;
    Module* native;

    FileHandler(Context* context);
    Module* require(Context* context, Module* from, Id name) override;
};

struct SourceMap {
    struct Entry {
        Entry(String path, Identifier id, void* buffer): path(move(path)), id(id), buffer(buffer) {}

        Entry(Entry&& other): path(other.path), id(other.id), buffer(other.buffer) {
            other.buffer = nullptr;
        }

        ~Entry() {
            hFree(buffer);
        }

        String path;
        Identifier id;
        void* buffer;
    };

    Array<Entry> entries;
};

Result<void, String> buildSourceMap(SourceMap& map, const String& root);
Result<void, String> buildSourceMap(SourceMap& map, const CompileSettings& settings);
