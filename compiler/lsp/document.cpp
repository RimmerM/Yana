#include "document.h"

namespace lsp {

void Document::setText(String next) {
    text = ::move(next);
    lines.build({ text.text(), text.size() });
}

U32 Document::offsetAt(U32 line, U32 character, bool utf16) const {
    return lines.offsetAt({ text.text(), text.size() }, line, character, utf16);
}

void Document::applyChange(U32 startLine, U32 startCharacter, U32 endLine, U32 endCharacter,
                           StringView replacement, bool utf16) {
    auto start = offsetAt(startLine, startCharacter, utf16);
    auto end = offsetAt(endLine, endCharacter, utf16);
    if(end < start) end = start;

    StringBuilder next(text.size() + replacement.length);
    next.append(text.text(), start);
    next.append(replacement.ptr, replacement.length);
    next.append(text.text() + end, text.size() - end);

    setText(next.string());
}

Document* DocumentStore::find(StringView uri) {
    for(auto& document: documents) {
        if(document.uri.size() == uri.length
        && compareMem(document.uri.text(), uri.ptr, uri.length) == 0) {
            return &document;
        }
    }

    return nullptr;
}

Document& DocumentStore::open(StringView uri, String path, String text, I32 version) {
    auto document = find(uri);
    if(!document) {
        documents.push(Document {});
        document = &documents[documents.size() - 1];
        document->uri = ownedString(uri.ptr, uri.length);
    }

    document->path = ::move(path);
    document->version = version;
    document->setText(::move(text));

    return *document;
}

void DocumentStore::close(StringView uri) {
    for(U32 i = 0; i < documents.size(); i++) {
        auto& document = documents[i];
        if(document.uri.size() == uri.length
        && compareMem(document.uri.text(), uri.ptr, uri.length) == 0) {
            documents.remove(i);
            return;
        }
    }
}

} // namespace lsp
