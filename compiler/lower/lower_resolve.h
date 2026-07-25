#pragma once

#include "lower.h"

struct LowerResolve;
using InstResolver = Maybe<LowerInst*> (*)(LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast);

struct LowerPendingInst {
    LowerPtr<LowerInst> inst;
    RegionPtr<LowerParserRegion, LowerInstAst> ast;
};

struct LowerResolve {
    LowerResolve(Diagnostics& diag, Context& context, Region<LowerRegion>& moduleArena, Region<LowerParserRegion>& parserArena);

    // Local state when resolving functions.
    HashMap<StringId, LowerPtr<LowerValue>> knownLocals;

    // Queue of instructions that need to be resolved after all locals are known (currently only phi).
    // Allocated into the parser arena, since it isn't used by the final resolved CFG.
    SmallList<LowerParserRegion, LowerPendingInst, false> pending;

    // Global resources.
    HashMap<StringId, InstResolver> instructionSet;
    Diagnostics& diag;
    Context& context;

    // Arena for the current module.
    Region<LowerRegion>& moduleArena;
    Region<LowerParserRegion>& parserArena;
};

bool resolveLowerModule(LowerResolve& resolve, LowerBase moduleBase, RegionBase<LowerParserRegion> parserBase, LowerModule& module);
void resolveLowerBlock(LowerResolve& resolve, LowerBase moduleBase, RegionBase<LowerParserRegion> parserBase, LowerBlock& block);
