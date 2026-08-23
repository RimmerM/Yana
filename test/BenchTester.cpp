/*
 * The compiler's own speed, measured on the fixture corpus.
 *
 * This exists because the data-structure work in `compiler/util/container.h` has no other way of
 * being checked. A pass that allocates once per instruction and one that allocates once per
 * function produce byte-identical output, so the golden files say nothing about either; the only
 * observable difference is how long the compiler takes and how many times it reaches the heap.
 *
 * Both numbers are reported, and the second is the one to trust. Wall time on a shared machine
 * moves by a few percent between runs of the same binary, while the allocation count is a property
 * of the code alone - the same source compiled twice reaches the heap exactly as often - so a
 * change that is real shows up there first and unambiguously. `malloc` is interposed below to count
 * it; see the note there.
 *
 * The corpus is `test/resolve/*.yana`, which is what the resolve suite already compiles: a few
 * hundred functions across a hundred-odd modules, exercising resolve, the optimizer, lowering, the
 * amd64 backend and the JavaScript backend. It is not a large program - no real one is in this
 * repository yet - so `-repeat` is how the measurement is made long enough to be stable rather than
 * `-repeat` standing in for a bigger input.
 *
 * The phases are what a compilation is made of rather than what its entry points are called. Three
 * of them could not be timed from where they are called at all and are marked inside the compiler -
 * see compiler/compiler/stage.h - because a module is parsed wherever an import or an embedded
 * library module first reaches it, the ownership passes run at the end of resolution, and the IR
 * optimizer runs on resolve IR from inside `lowerProgram`. Timing the two entry points alone
 * reported "resolve" for a number that was a third parsing, and "lower" for a number that was mostly
 * the optimizer. Every phase's number here is its *own* cost, with whatever nested inside it
 * subtracted; see the phase stack below.
 *
 * Run it from `test/`, like the other drivers:
 *
 *     ../build-bench/test/YanaBench -repeat 20
 *
 * ## Which build
 *
 * `build-bench` is CMAKE_BUILD_TYPE=Bench, which is `-O2 -g` and *not* what ships: CMakeLists.txt
 * appends `-O3 -flto` to Release and RelWithDebInfo only. That is deliberate - an incremental
 * rebuild is 1.5s here against 24s for Release, which is the difference between profiling being a
 * loop and being an errand - but wall time runs about 6% high across every phase, and a small
 * function in another translation unit is a call here that link-time optimization may remove there.
 *
 * So a *profile* taken against this build can name a cost that is not in the shipped compiler. The
 * predicates in util/lexer_util.cpp are the example: `isDigit` and its neighbours are a call each
 * here and are inlined away under LTO, and an afternoon can be spent making them faster for nobody.
 *
 * Configure a third directory to check one, rather than reading a stripped Release binary:
 *
 *     cmake -S . -B build-lto -DCMAKE_BUILD_TYPE=Bench \
 *           -DCMAKE_CXX_FLAGS_BENCH="-O2 -g -DNDEBUG -flto -fno-stack-protector" \
 *           -DCMAKE_EXE_LINKER_FLAGS="-flto"
 *
 * which lands within half a percent of Release and keeps its symbols. Release itself is linked
 * `-Wl,-s`, so `nm` reports nothing about it and "the symbol is gone" says nothing about inlining -
 * that mistake is why this paragraph is here.
 *
 * Allocation counts are unaffected by any of this - they are a property of the code, and every build
 * reports them identically.
 *
 * A Debug build measures the assertions and answers nothing.
 */

#include <Core.h>
#include <File.h>
#include <Date.h>
#include <cstdlib>
#include <cstdio>
#include <dlfcn.h>

#include "../compiler/compiler/stage.h"
#include "../compiler/parse/parser.h"
#include "../compiler/resolve/analyze.h"
#include "../compiler/resolve/lower.h"
#include "../compiler/lower/lower_validate.h"
#include "../compiler/codegen/x64/gen.h"
#include "../compiler/codegen/js/gen.h"
#include "corpus.h"

using namespace Tritium;

/*
 * The allocation counter.
 *
 * A strong `malloc` in the executable interposes on every caller in the process, including the ones
 * inside libc, which is what makes this a count of what the *compiler* does rather than of what the
 * parts of it that happen to call `hAlloc` do. Each wrapper forwards to the `__libc_` entry point,
 * so nothing about the allocator's behaviour changes - a block from any of these is still freed by
 * `__libc_free`, whichever path allocated it.
 *
 * `counting` is off until the measured region starts, because process startup, the fixture reads
 * and the printing afterwards are not what is being measured and vary with the environment.
 */
extern "C" {
    void* __libc_malloc(size_t size);
    void* __libc_calloc(size_t count, size_t size);
    void* __libc_realloc(void* p, size_t size);
    void __libc_free(void* p);
}

static bool gCounting = false;
static U64 gAllocations = 0;

// `-trace` names each fixture as it is reached. For when the benchmark stops on one and the
// question is which; it prints from inside the measured region, so it is not for measuring with.
static bool gTrace = false;

/*
 * Where the allocations come from, under `-sites`.
 *
 * A total says a change helped; this says what to change. The key is the address `malloc` was
 * called from, which after inlining is a line inside whichever pass built the list - not a line
 * inside the array - so the twenty entries this prints are twenty places to look at.
 *
 * A fixed open-addressed table rather than a HashMap, because this runs inside `malloc`: anything
 * that could allocate would call back into here, and a table that never grows cannot. 16k slots is
 * far more than the few hundred distinct sites a compile has, and a full table drops sites rather
 * than growing - which would only happen if the assumption behind that sentence were wrong, so it
 * is reported rather than ignored.
 */
static constexpr Size kSiteSlots = 16384;

struct Site {
    void* address;
    U64 count;
};

static Site gSites[kSiteSlots];
static bool gProfiling = false;
static U64 gSitesDropped = 0;

static void recordSite(void* address) {
    auto hash = (Size(address) * 0x9E3779B97F4A7C15ull) >> 45;

    for(Size i = 0; i < 64; i++) {
        auto& slot = gSites[(hash + i) % kSiteSlots];

        if(slot.address == address) {
            slot.count++;
            return;
        }

        if(!slot.address) {
            slot.address = address;
            slot.count = 1;
            return;
        }
    }

    gSitesDropped++;
}

#ifndef YANA_BENCH_NO_INTERPOSE
extern "C" void* malloc(size_t size) {
    if(gCounting) {
        gAllocations++;
        if(gProfiling) recordSite(__builtin_return_address(0));
    }

    return __libc_malloc(size);
}

extern "C" void* calloc(size_t count, size_t size) {
    if(gCounting) {
        gAllocations++;
        if(gProfiling) recordSite(__builtin_return_address(0));
    }

    return __libc_calloc(count, size);
}

// Counted as an allocation, since that is what it is when the block has to move and the caller
// cannot tell which time it did. Growth by doubling is the pattern this whole exercise is about.
extern "C" void* realloc(void* p, size_t size) {
    if(gCounting) {
        gAllocations++;
        if(gProfiling) recordSite(__builtin_return_address(0));
    }

    return __libc_realloc(p, size);
}

extern "C" void free(void* p) {
    __libc_free(p);
}
#endif

// Modules an `import` names, read from `resolve/modules/` exactly as the resolve driver does. The
// source text is only ever asked for by a diagnostic, and this driver reports none.
struct BenchProvider: SourceProvider, ModuleProvider {
    struct Loaded {
        StringId name;
        Ptr<char, HeapDeleter> text;
        Size length;
        ast::Module* ast;
        ast::ModuleGroup* group;
    };

    StringView source;
    Context* context = nullptr;
    Array<Loaded> loaded;

    // Which directory an `import` this driver has to answer is looked for in - `resolve/modules/`
    // for a compiler fixture and `lib/modules/` for a library one. Set per fixture, because the two
    // corpora are compiled by one process here.
    String moduleRoot = String("resolve/modules/");

    ~BenchProvider() override {
        for(auto& entry: loaded) {
            delete entry.group;
            delete entry.ast;
        }
    }

    StringView getSource(StringId id) override {
        for(auto& entry: loaded) {
            if(entry.name == id) return StringView { entry.text.get(), entry.length };
        }

        return source;
    }

    const Location* getNode(LocationId id) override {
        return context ? context->getLocation(id) : nullptr;
    }

    ast::ModuleGroup* getModule(StringId name) override {
        for(auto& entry: loaded) {
            if(entry.name == name) return entry.group;
        }

        // `OpenExisting`, for the reason ResolveTester gives: the default mode *creates* the file,
        // so an import this directory does not answer leaves an empty module behind - which then
        // shadows the library module of that name on every run afterwards, silently.
        auto path = moduleRoot + context->findName(name) + String(".yana");
        auto opened = File::openFile(path, readAccess(), File::OpenExisting);
        if(opened.isErr()) return nullptr;

        auto file = opened.moveUnwrapOk();
        auto size = file.size();
        Ptr<char, HeapDeleter> text { (char*)hAlloc(size) };
        file.read({ (Byte*)text.get(), size });

        Lexer lexer(*context, context->diagnostics, StringView { text.get(), size }, name);
        Parser parser(*context, lexer, name);
        auto ast = new ast::Module(parser.parseModule());

        // A group of one file. These fixtures name a module per file, which is what a file that
        // wrote `module` would be under the directory rule too - Analysis-Modules.md §2.1.
        auto group = new ast::ModuleGroup { .name = name };
        group->files.push(ast);

        loaded.push(Loaded { name, ::move(text), size, ast, group });
        return group;
    }
};

struct Fixture {
    String path;
    Ptr<char, HeapDeleter> source;
    Size length = 0;

    // The two backends are opted into per fixture, on exactly the terms the resolve driver uses:
    // a fixture is only put through amd64 if it has a `.run.expect` naming an answer, and only
    // through the JavaScript backend if it has a `.js.expect`. Neither backend accepts every
    // program the resolver does - a fixture about a construct one of them has not reached yet is
    // not an omission here but the same opt-in the golden suite already makes.
    bool native = false;
    bool js = false;
};

// True for a file that exists and has something in it. An empty `.run.expect` is how the resolve
// driver spells "resolved but not run", so an empty one is not an opt-in here either.
// The marker files a library fixture opts in with are empty, so existence is the question there
// rather than content - see hasContent below, which answers the other one.
static bool fileExists(const String& path) {
    auto info = File::info(path);
    return info && !info.unwrapOk().isDirectory;
}

static bool hasContent(const String& path) {
    auto info = File::info(path);
    return info && !info.unwrapOk().isDirectory && info.unwrapOk().size > 0;
}

// One phase's share of the run. Nanoseconds while accumulating; printed as milliseconds.
struct Phase {
    const char* name;
    U64 time = 0;
    U64 allocations = 0;
};

static Phase gPhases[] = {
    { "parse" }, { "resolve" }, { "ownership" }, { "opt" }, { "lower" }, { "amd64" }, { "js" },
};

enum PhaseId { PhaseParse, PhaseResolve, PhaseOwnership, PhaseOpt, PhaseLower, PhaseX64, PhaseJs, PhaseCount };

/*
 * The phase stack, and why a phase's number is not simply its elapsed time.
 *
 * Phases nest: a module is parsed from inside resolution, the ownership passes run at the end of it,
 * and the optimizer runs from inside lowering. What a reader wants of each is its *own* cost, so an
 * entry subtracts whatever its children reported and hands its whole elapsed time up to its parent
 * to be subtracted there. The columns then add up to the total rather than double-counting the
 * inner ones - which the old flat timers did, silently, since nothing they measured nested.
 *
 * The depth is bounded by the pipeline itself: parse inside resolve is two, plus whatever a driver
 * brackets around them, and the assert below is what says so out loud rather than corrupting the
 * numbers if that stops being true.
 */
struct PhaseFrame {
    PhaseId id;
    U64 startTime;
    U64 startAllocations;
    U64 childTime;
    U64 childAllocations;
};

static PhaseFrame gPhaseStack[8];
static Size gPhaseDepth = 0;

static void enterPhase(PhaseId id) {
    assertTrue(gPhaseDepth < 8); // phases nested deeper than the pipeline has stages
    gPhaseStack[gPhaseDepth++] = PhaseFrame { id, nanoTime(), gAllocations, 0, 0 };
}

static void leavePhase() {
    auto& frame = gPhaseStack[--gPhaseDepth];
    auto time = nanoTime() - frame.startTime;
    auto allocations = gAllocations - frame.startAllocations;

    gPhases[frame.id].time += time - frame.childTime;
    gPhases[frame.id].allocations += allocations - frame.childAllocations;

    if(gPhaseDepth) {
        gPhaseStack[gPhaseDepth - 1].childTime += time;
        gPhaseStack[gPhaseDepth - 1].childAllocations += allocations;
    }
}

// Charges everything done in its scope to one phase. A scope rather than a wrapper around a lambda
// because two of the phases produce a value the rest of the function goes on to use, and a
// scope-bound timer says that without the value having to travel through a return.
struct PhaseTimer {
    explicit PhaseTimer(PhaseId id) { enterPhase(id); }
    ~PhaseTimer() { leavePhase(); }
};

// The three stages the compiler marks for itself, mapped onto the phases above. Installed for the
// whole run - see gStageObserver, which is null in every build that is not this one.
struct BenchStages: StageObserver {
    void enterStage(CompileStage stage) override {
        switch(stage) {
            case CompileStage::Parse: enterPhase(PhaseParse); break;
            case CompileStage::Ownership: enterPhase(PhaseOwnership); break;
            case CompileStage::Optimize: enterPhase(PhaseOpt); break;
            default: break;
        }
    }

    void leaveStage(CompileStage) override { leavePhase(); }
};

// The root module's parse. No timer of its own: Parser::parseModule marks the parse stage for
// itself, which is what also catches the imports resolution reaches and the library modules the
// compiler carries as embedded source - so one mechanism reports all three.
static ast::Module parseFixture(Context& context, Diagnostics& diagnostics, StringView source,
                                StringId name) {
    Lexer lexer(context, diagnostics, source, name);
    Parser parser(context, lexer, name);
    return parser.parseModule();
}

/*
 * The native pipeline for one fixture, from text to machine code.
 *
 * Codegen is run but nothing is executed and nothing is written: what is being measured is the
 * compiler, and a fixture's answer is the resolve suite's business. A fixture the resolver rejects
 * - there are a few, and they are as much of a compile as any other - stops after resolve rather
 * than lowering a program that has no meaning.
 */
static void benchNative(const Fixture& fixture) {
    if(gTrace) println("native %@", fixture.path);
    BenchProvider provider;
    provider.source = StringView { fixture.source.get(), fixture.length };
    if(stringView(fixture.path).startsWith("lib/"_v)) provider.moduleRoot = String("lib/modules/");
    Diagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    auto name = context.addUnqualifiedName("Bench", 5);

    auto ast = parseFixture(context, diagnostics, provider.source, name);

    Ptr<Program> module;
    {
        PhaseTimer timer(PhaseResolve);
        module = resolveProgram(context, ast, &provider, Program::Specialization::Always);
    }

    if(diagnostics.errorCount()) return;

    Ptr<LowerModule> lowered;
    {
        PhaseTimer timer(PhaseLower);
        lowered = lowerProgram(context, *module);
    }

    if(diagnostics.errorCount() || !fixture.native) return;

    // Outside any phase, and it has to be: the backend crashes rather than complains when handed IR
    // that does not hold together, so a fixture the resolve suite is currently failing would take
    // the benchmark with it. The same check the resolve driver makes before it generates code, for
    // the same reason - what is being measured is the compiler working, not the compiler falling
    // over. It costs the same on every run, so leaving it out of the phases keeps the numbers
    // comparable with each other rather than with anything absolute.
    if(!validateLowerModule(&diagnostics, lowered.get())) return;

    {
        PhaseTimer timer(PhaseX64);
        auto base = *lowered->arena;
        AsmModule assembly;
        RegScratch scratch;
        FunctionRegs registers;
        MachineFunction machine;

        for(auto functionPointer: lowered->functionOrder) {
            auto function = base[functionPointer];
            machine.reset();
            transformFunction(context, base, *function, machine);

            scratch.resetRecords();
            allocateRegisters(context, base, *function, machine, scratch, registers);
            genFunction(context, base, assembly, *function, machine, registers);
        }

        for(auto globalPointer: lowered->globalOrder) assembly.addGlobal(base, base[globalPointer]);
        assembly.resolveRelocations(lowered->imageAnchor ? base[lowered->imageAnchor] : nullptr);
    }
}

/*
 * The same fixture through the JavaScript backend, which needs its own resolution.
 *
 * Not an alternative to the native run but a second half of the same measurement: `@platform`
 * selects which declarations exist, so the two targets do not share a resolved program - and the
 * two backends between them are most of what a change to the shared containers touches.
 */
static void benchJs(const Fixture& fixture) {
    if(!fixture.js) return;

    BenchProvider provider;
    provider.source = StringView { fixture.source.get(), fixture.length };
    if(stringView(fixture.path).startsWith("lib/"_v)) provider.moduleRoot = String("lib/modules/");
    Diagnostics diagnostics(provider);
    Context context(diagnostics);
    context.settings.mode = CompileMode::JsExecutable;
    provider.context = &context;

    auto name = context.addUnqualifiedName("Bench", 5);

    // Charged to the same two phases the native half is, so that the phases still add up to the
    // total: this resolution is as much of the compiler's work as the other one, and the only
    // thing that makes it the JavaScript build is which declarations `@platform` leaves in it.
    auto ast = parseFixture(context, diagnostics, provider.source, name);

    Ptr<Program> module;
    {
        PhaseTimer timer(PhaseResolve);
        module = resolveProgram(context, ast, &provider, Program::Specialization::Always);
    }

    if(diagnostics.errorCount()) return;

    {
        PhaseTimer timer(PhaseJs);
        auto file = js::genProgram(context, *module);
        Net::Writer writer(16384);
        js::formatFile(writer, context, *file, false);
    }
}

// The fixtures `bench.skip` names, one per line, `#` starting a comment. See that file for why the
// list exists at all - it is the compiler's current failures rather than anything about the
// benchmark, and it is meant to shrink.
static Array<String> readSkipList() {
    Array<String> names;

    auto opened = File::openFile(String("bench.skip"), readAccess());
    if(opened.isErr()) return names;

    auto file = opened.moveUnwrapOk();
    auto size = file.size();
    Ptr<char, HeapDeleter> text { (char*)hAlloc(size) };
    file.read({ (Byte*)text.get(), size });

    Size start = 0;
    for(Size i = 0; i <= size; i++) {
        if(i != size && text.get()[i] != '\n') continue;

        auto line = StringView { text.get() + start, i - start };
        start = i + 1;

        while(line.length && (line.ptr[0] == ' ' || line.ptr[0] == '\t')) {
            line.ptr++;
            line.length--;
        }

        while(line.length && (line.ptr[line.length - 1] == '\r' || line.ptr[line.length - 1] == ' ')) {
            line.length--;
        }

        if(!line.length || line.ptr[0] == '#') continue;

        // Owned, because `String(pointer, length)` is a view and the text it would point into is
        // this function's and freed on the way out.
        names.push(ownedString(line.ptr, line.length));
    }

    return names;
}

/*
 * The twenty places most of the allocations came from.
 *
 * Printed as an offset within the executable rather than as a name, because a name after inlining
 * is whichever function the linker happened to keep and is usually the wrong one. The offset is
 * exact, and
 *
 *     addr2line -e ../build-bench/test/YanaBench -f -i -C 0x1234
 *
 * turns it into the file, the line, and the chain of things that were inlined into it - which is
 * the actual answer. The subtraction is what makes the offset meaningful under a position
 * independent executable, where the addresses this recorded are wherever the loader put the text
 * this time.
 */
static void reportSites(Size repeat) {
    Dl_info info;
    Size base = 0;
    if(dladdr((void*)&reportSites, &info)) base = (Size)info.dli_fbase;

    Array<Site> sites;
    for(auto& slot: gSites) {
        if(slot.address) sites.push(slot);
    }

    println("");
    println("allocation sites (addr2line -e ../build-bench/test/YanaBench -f -i -C <offset>)");

    // Twenty passes over a few hundred entries, picking the largest still unprinted each time.
    // Sorting would be tidier and this is a report printed once, after the measurement.
    char offset[32];
    U64 ceiling = maxLimit<U64>;

    for(Size printed = 0; printed < 20; printed++) {
        Site best { nullptr, 0 };

        for(auto& site: sites) {
            if(site.count <= ceiling && site.count > best.count) best = site;
        }

        if(!best.address) break;

        auto length = (Size)snprintf(offset, sizeof(offset), "+0x%zx", (Size)best.address - base);
        println("  %@  %@ /repeat", StringView { offset, length }, best.count / repeat);
        ceiling = best.count - 1;
    }

    if(gSitesDropped) {
        println("  (%@ allocations went uncounted - the site table filled up)", gSitesDropped);
    }
}

int main(int argc, const char** argv) {
    Size repeat = 10;

    for(int i = 1; i < argc; i++) {
        if(String(argv[i]) == "-repeat" && i + 1 < argc) repeat = (Size)atoi(argv[++i]);
        else if(String(argv[i]) == "-trace") gTrace = true;
        else if(String(argv[i]) == "-sites") gProfiling = true;
    }

    auto skipped = readSkipList();

    /*
     * Both corpora, because the compiler's cost is the whole of what it compiles.
     *
     * `resolve/` and `lib/` were one directory until the library suite was split out, and the
     * fixtures that moved are among the heaviest here - `File`, `Real` and the two `MoveMemory`s
     * are hundreds of lines each. Dropping them would have moved every number in this table for a
     * reason that has nothing to do with the compiler, which is exactly what a benchmark must not
     * do. How a fixture opts a backend in is the only thing that differs: a compiler fixture names
     * its answer in `.run.expect` and its JavaScript in `.js.expect`, and a library fixture has
     * neither - it runs on amd64 always and on JavaScript where the `.js` marker exists.
     */
    Array<Fixture> fixtures;

    auto collect = [&](const char* directory, bool library) {
        listDirectory(directory, [&](const String& name, bool isDirectory) {
            if(isDirectory) return;
            auto dot = findLastChar(stringView(name), '.');
            if(!dot) return;
            if(String(dot + 1, name.text() + name.size() - dot - 1) != "yana") return;
            if(skipped.contains([&](const String& s) { return s == name; })) return;

            auto path = String(directory) + String("/") + name;
            auto opened = File::openFile(path, readAccess());
            if(opened.isErr()) return;

            auto file = opened.moveUnwrapOk();
            auto size = file.size();
            Ptr<char, HeapDeleter> source { (char*)hAlloc(size) };
            file.read({ (Byte*)source.get(), size });

            auto native = library || hasContent(path + String(".run.expect"));
            auto js = library ? fileExists(path + String(".js"))
                              : hasContent(path + String(".js.expect"));
            fixtures.push(Fixture { path, ::move(source), size, native, js });
        });
    };

    // `lib/` has to be the fixture corpus and not the standard library of that name - see corpus.h.
    if(reportWrongDirectory()) return 1;

    collect("resolve", false);
    collect("lib", true);

    if(fixtures.isEmpty()) {
        println("no fixtures found - run this from the test directory");
        return 1;
    }

    // One untimed pass, so that whatever the allocator and the page tables do the first time a
    // process touches this much memory lands outside the measurement.
    for(auto& fixture: fixtures) {
        benchNative(fixture);
        benchJs(fixture);
    }

    for(auto& phase: gPhases) {
        phase.time = 0;
        phase.allocations = 0;
    }

    // After the warm-up rather than before it: what the observer costs is two virtual calls per
    // marked scope, and the warm-up should pay them too so that the two runs do the same work.
    BenchStages stages;
    gStageObserver = &stages;

    gCounting = true;
    auto start = nanoTime();

    for(Size i = 0; i < repeat; i++) {
        for(auto& fixture: fixtures) {
            benchNative(fixture);
            benchJs(fixture);
        }
    }

    auto elapsed = nanoTime() - start;
    auto total = gAllocations;
    gCounting = false;
    gStageObserver = nullptr;
    assertTrue(gPhaseDepth == 0); // a phase entered and never left

    println("%@ fixtures x %@ repeats", fixtures.size(), repeat);

    U64 phaseTime = 0;
    U64 phaseAllocations = 0;

    for(auto& phase: gPhases) {
        println("  %@: %@ ms, %@ allocations/repeat", phase.name,
                F64(phase.time) / 1000000.0, phase.allocations / repeat);

        phaseTime += phase.time;
        phaseAllocations += phase.allocations;
    }

    // What no phase covers, printed so that the columns visibly add up to the total rather than
    // being trusted to. It is the driver's own work between the brackets - building a Context and a
    // Program and tearing them down, reading an imported module off disk, and validateLowerModule,
    // which is deliberately outside the phases for the reason given in benchNative. A few percent is
    // this; more than that is a phase that stopped covering what it says it does.
    println("  other: %@ ms, %@ allocations/repeat", F64(elapsed - phaseTime) / 1000000.0,
            (total - phaseAllocations) / repeat);

    println("  total: %@ ms, %@ allocations/repeat", F64(elapsed) / 1000000.0, total / repeat);

    if(gProfiling) reportSites(repeat);
    return 0;
}
