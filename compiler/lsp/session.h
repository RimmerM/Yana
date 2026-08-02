#pragma once

#include "../compiler/overlay.h"
#include "../compiler/position.h"
#include "../compiler/project.h"
#include "../resolve/index.h"
#include "../resolve/module.h"

namespace lsp {

using namespace Tritium;

/*
 * The project, and the compile that answers questions about it.
 *
 * One `Session` is one resolved program - see §5.4: `@platform` selects declarations *during*
 * resolution, so a native session and a JS session do not share one and the project file's `target`
 * picks which of the two this is.
 *
 * v1 re-resolves everything on every change, which §5.3 argues for at some length: the region
 * arenas that make the compiler fast are what make invalidating one module's results impossible, so
 * the two-tier scheme belongs in a later milestone and a query engine belongs nowhere.
 */
struct Session {
    /// Reads the project file at or above `rootPath` and builds the module map from it. The error
    /// is something to show the user rather than to log: a server with no project answers nothing,
    /// and silence is the failure mode §10 says half of all bug reports are.
    Result<void, String> open(StringView rootPath);

    /// Rebuilds the module map from the project's source directories. Called when a file the map
    /// does not know is opened, since that is a file that was created since the map was built.
    Result<void, String> rescan();

    /// Re-resolves the whole program. Everything reported lands in `diagnostics.messages`, and the
    /// previous compile's results are dropped first - a `Program` owns arenas that the next one
    /// cannot share.
    void compile();

    /// True once `open` has succeeded.
    bool isOpen() const { return opened; }

    /// The module map entry for a path, or null when the file is not part of the project.
    SourceEntry* findEntry(StringView path) { return findEntryByPath(moduleMap, path); }

    /*
     * The position index for one module, built the first time it is asked for.
     *
     * Lazily rather than per compile, because building one is a pass over every location in the
     * program and a keystroke touches one file. The cache is dropped with the compile that filled
     * it: a LocationId means something only against the context that created it.
     */
    PositionIndex* positionsOf(StringId module);

    /// What is at a byte offset of one module, or null. The two indexes in one call, because every
    /// feature asks the same two questions in the same order - which node, then what did it mean.
    const Reference* referenceAt(StringId module, U32 offset);

    /// The symbol *declared* at an offset, for a cursor sitting on a declaration rather than on a
    /// use. `find references` on a definition is the request this exists for.
    const Symbol* definitionAt(StringId module, U32 offset);

    /// The file one module's source is in, or an empty view for Core and Native - which are
    /// compiled into the compiler and have no file to point at.
    StringView pathOf(StringId module);

    ModuleMap moduleMap;
    OverlayProvider provider { moduleMap };
    CollectDiagnostics diagnostics { provider };

    Ptr<Context> context;
    Ptr<Program> program;

    /*
     * What name resolution decided, kept - resolve/index.h.
     *
     * Owned here and rebuilt with the program, because what it holds are that program's own
     * handles: a Module*, a TypePtr, a region offset. It is created before resolution and hung on
     * the context, which is the whole of how the recording sites are switched on.
     */
    Ptr<SemanticIndex> index;

    CompileSettings settings;
    String rootPath;
    String projectPath;

private:
    bool opened = false;

    // One built index per module asked about, dropped with the compile.
    struct ModulePositions {
        StringId module = 0;
        PositionIndex index;
    };

    Array<Ptr<ModulePositions>> positions;
};

} // namespace lsp
