#include "overlay.h"

using Tritium::String;

SourceEntry* findEntryByPath(ModuleMap& map, StringView path) {
    for(auto& entry: map.entries) {
        if(entry.path.length == path.length && compareMem(entry.path.ptr, path.ptr, path.length) == 0) {
            return &entry;
        }
    }

    return nullptr;
}

const OverlayDocument* OverlayProvider::findDocument(StringView path) const {
    for(auto& document: documents) {
        if(document.path.size() == path.length
        && compareMem(document.path.text(), path.ptr, path.length) == 0) {
            return &document;
        }
    }

    return nullptr;
}

bool OverlayProvider::setDocument(StringView path, String text, I32 version) {
    for(auto& document: documents) {
        if(document.path.size() == path.length
        && compareMem(document.path.text(), path.ptr, path.length) == 0) {
            document.text = ::move(text);
            document.version = version;
            return findEntryByPath(moduleMap, path) != nullptr;
        }
    }

    documents.push(OverlayDocument { Tritium::ownedString(path.ptr, path.length), ::move(text), version });
    return findEntryByPath(moduleMap, path) != nullptr;
}

bool OverlayProvider::clearDocument(StringView path) {
    for(U32 i = 0; i < documents.size(); i++) {
        auto& document = documents[i];
        if(document.path.size() == path.length
        && compareMem(document.path.text(), path.ptr, path.length) == 0) {
            documents.remove(i);
            return true;
        }
    }

    return false;
}

bool OverlayProvider::loadText(SourceEntry& entry) {
    if(entry.text) return true;

    auto document = findDocument(entry.path);
    if(!document) return FileProvider::loadText(entry);

    // Copied rather than referenced. The entry owns its text with a HeapDeleter, and the document
    // is replaced wholesale by the next `didChange` - which would otherwise free the buffer a
    // half-finished compile is still lexing out of.
    auto size = document->text.size();
    Ptr<char, HeapDeleter> text { (char*)hAlloc(size ? size : 1) };
    copy(document->text.text(), text.get(), size);

    entry.text = ::move(text);
    entry.length = size;
    return true;
}
