#pragma once

#include <Core.h>

/*
 * Where a compilation's time goes, for a driver that is measuring it.
 *
 * This exists because the three stages below are not separable from outside. `resolveProgram` and
 * `lowerProgram` are each one call, and the work a reader wants told apart is inside them: a module
 * is parsed wherever an import or an embedded library module first reaches it, the ownership passes
 * run at the end of resolution rather than as a call of their own, and the IR optimizer runs on
 * resolve IR from inside `lowerProgram` - so timing the two entry points reports "resolve" for a
 * number that is mostly parsing, and "lower" for a number that is mostly optimizing.
 *
 * A global rather than something threaded through Context, because the marked scopes are reached
 * from six places across three libraries and a parameter for a measurement would be in the signature
 * of everything between. It is null in every build that is not measuring, which makes each marker a
 * predictable branch on a pointer that is a constant for the run - see StageScope.
 *
 * The stages are the ones that could not be timed from outside. Everything else a driver wants to
 * separate - lowering, either backend - it can already bracket for itself, and should.
 */
enum class CompileStage: U8 {
    // Lexing and parsing one module, wherever it is reached from: the file a driver hands over, an
    // import resolved through a ModuleProvider, or one of the library modules the compiler carries
    // as embedded source and parses while it builds Core, Native and Host.
    Parse,

    // runProgramOwnership: liveness, provenance, drop placement and the summaries they derive.
    // Required work rather than an optimization - a body without its drops is wrong, not slow - but
    // it is the expensive half of resolution and it is not what "resolve" means to a reader.
    Ownership,

    // optimizeProgram, which rewrites resolve IR and is called from inside `lowerProgram` and from
    // the JavaScript backend. This is the stage that could be switched off and still produce a
    // working program, which is the line worth being able to see.
    Optimize,

    Count,
};

/*
 * Told when a marked stage is entered and left, in the order it happens.
 *
 * Stages nest: parsing an import happens inside resolution, and both backends optimize. An observer
 * that wants a stage's own cost has to subtract what its children reported, which is the caller's
 * business rather than this interface's - see BenchTester.cpp, where that arithmetic is the whole of
 * what the phase stack does.
 */
struct StageObserver {
    virtual ~StageObserver() = default;
    virtual void enterStage(CompileStage stage) = 0;
    virtual void leaveStage(CompileStage stage) = 0;
};

// Null unless a driver installed one. Not owned, and not synchronized: a measuring driver compiles
// on one thread, which is what the benchmark does and the only thing this is for.
extern StageObserver* gStageObserver;

struct StageScope {
    explicit StageScope(CompileStage stage): stage(stage) {
        if(gStageObserver) gStageObserver->enterStage(stage);
    }

    ~StageScope() {
        if(gStageObserver) gStageObserver->leaveStage(stage);
    }

    StageScope(const StageScope&) = delete;
    StageScope& operator = (const StageScope&) = delete;

private:
    CompileStage stage;
};
