#pragma once

#include "source.h"

/*
 * One file as the editor currently has it, which is not what is on disk.
 *
 * `version` is the client's own counter from `textDocument/didOpen` and `didChange`. It is kept so
 * a diagnostic can be published against the version it was computed from: a client that has typed
 * three more characters since discards a report for an older version rather than drawing squiggles
 * against text that has moved.
 */
struct OverlayDocument {
    Tritium::String path;
    Tritium::String text;
    I32 version = 0;
};

/*
 * The source provider a language server reads through - Implementation-Tooling.md §5.1.
 *
 * A sibling of FileProvider rather than a replacement for it: a project is mostly files that are
 * not open in any editor, and those still come from disk. What is open answers from memory instead,
 * and answers the same way to every question the compiler asks - the resolver's `import`, the
 * lexer's text, and the caret line of a diagnostic all go through one object so that there is no
 * arrangement of them that can disagree about what a file says.
 *
 * The document list is small by construction - it holds what is open in an editor - so it is an
 * array searched linearly. A map keyed by path would be the same lookup with a hash in front of it.
 */
struct OverlayProvider: FileProvider {
    using FileProvider::FileProvider;

    // Sets the editor's text for one path, replacing any earlier one. The path is matched against
    // ModuleMap::entries; `true` means the file is part of the project, and `false` means the map
    // has to be rebuilt before this file is anything the compiler will look at.
    bool setDocument(StringView path, Tritium::String text, I32 version);

    // Forgets the editor's text for one path. The file itself is still in the project - the next
    // compile reads it from disk, which is where the editor just saved it.
    bool clearDocument(StringView path);

    const OverlayDocument* findDocument(StringView path) const;

    bool loadText(SourceEntry& entry) override;

    Array<OverlayDocument> documents;
};

/// The module map entry for one path, or null. Exposed because everything the server answers is
/// keyed by the document the client named, and a path is how the client names it.
SourceEntry* findEntryByPath(ModuleMap& map, StringView path);
