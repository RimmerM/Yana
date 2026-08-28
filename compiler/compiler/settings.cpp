#include "settings.h"

#ifdef __GNUC__
#include <cpuid.h>
#endif

using Tritium::String;

struct Flag {
    enum Type {
        /*
         * Additive flags.
         */

        add,
        mainModule,
        enableInst,

        /*
         * Value flags.
         */

        to,
        outputName,
        mode,
        platform,
        target,
        arch,
        format,
        backend,
        framePointer,
        inlining,
        optimization,
        explain,
        project,
        library,
        package,
        specialize,

        /*
         * Boolean flags.
         */

        help,
        printModules,
        noIrOptimize,
        noChecks,
        checkLocations,
        explainAll,
        noProject,
        test,
    };

    StringView name;
    U32 argCount;
    Type type;

    /*
     * Whether naming this flag twice adds a second value rather than replacing the first.
     *
     * The three that are lists are lists because a compilation genuinely has several of the thing -
     * source roots, instruction-set extensions. Everything else answers a question that has one
     * answer, and naming it twice with two answers used to be resolved by argument order: `-to` and
     * `-output` each grew a check of their own and `-mode exe -mode ir` silently built IR. One rule
     * in one place instead - see parseFlags.
     */
    bool repeatable = false;
};

Flag flagTable[] = {
    { "add"_v, 1, Flag::add, true },
    { "main"_v, 1, Flag::mainModule, true },
    { "enable-inst"_v, 1, Flag::enableInst, true },

    { "to"_v, 1, Flag::to },
    { "output"_v, 1, Flag::outputName },
    { "mode"_v, 1, Flag::mode },
    { "platform"_v, 1, Flag::platform },
    { "target"_v, 1, Flag::target },
    { "arch"_v, 1, Flag::arch },
    { "format"_v, 1, Flag::format },
    { "backend"_v, 1, Flag::backend },
    { "frame-pointer"_v, 1, Flag::framePointer },
    { "opt"_v, 1, Flag::optimization },
    { "inline"_v, 1, Flag::inlining },
    { "explain"_v, 1, Flag::explain },
    { "project"_v, 1, Flag::project },
    { "lib"_v, 1, Flag::library },

    // The package this compilation is, when there is no `yana.toml` to say - which is every driver
    // that resolves a source string rather than a tree. See CompileSettings::package.
    { "package"_v, 1, Flag::package },
    { "specialize"_v, 1, Flag::specialize },

    { "help"_v, 0, Flag::help },
    { "h"_v, 0, Flag::help },
    { "print-modules"_v, 0, Flag::printModules },
    { "no-ir-opt"_v, 0, Flag::noIrOptimize },
    { "no-checks"_v, 0, Flag::noChecks },
    { "check-locations"_v, 0, Flag::checkLocations },
    { "explain-all"_v, 0, Flag::explainAll },
    { "no-project"_v, 0, Flag::noProject },
    { "test"_v, 0, Flag::test },
};

// InlineLevel, in declaration order.
StringView inlineTable[] = {
    "none"_v,     // None
    "size"_v,     // Size
    "balanced"_v, // Balanced
    "speed"_v,    // Speed
};

// CompileMode, in declaration order. What comes out; `platformTable` below says what it is for, and
// the two used to be one table - see TargetPlatform.
StringView modeTable[] = {
    "exe"_v,       // Executable
    "shared"_v,    // Shared
    "lib"_v,       // Library
    "ast"_v,       // Ast
    "ownership"_v, // Ownership
    "ir"_v,        // Ir
    "lower"_v,     // Lower
    "llvm"_v,      // Llvm
};

// TargetPlatform, in declaration order. The same two words `@platform` is written with and the same
// two a file name selects on, because they are the same question asked in three places.
StringView platformTable[] = {
    "native"_v, // Native
    "js"_v,     // Js
};

/*
 * The two spellings that used to be modes and are now a mode and a platform together.
 *
 * Kept because every documented invocation and every script writes them, and because they are not
 * ambiguous: `js` was only ever "an executable, for JavaScript". They set the platform as well as
 * the mode, so `-mode js -platform native` is a contradiction rather than a silent winner - see
 * where this table is read.
 */
struct ModeAlias { StringView name; CompileMode mode; TargetPlatform platform; };

static const ModeAlias modeAliasTable[] = {
    { "js"_v, CompileMode::Executable, TargetPlatform::Js },
    { "jslib"_v, CompileMode::Library, TargetPlatform::Js },
};

StringView formatTable[] = {
    "elf"_v,   // ELF
    "mach"_v,  // MachO
    "pe"_v,  // PE
};

// NativeBackend, in declaration order. "local" rather than "x64", because what it selects is the
// compiler's own code generator rather than an architecture - `-arch` is already the flag that says
// which machine, and naming one here would make `-backend x64 -arch arm64` a sentence.
StringView backendTable[] = {
    "llvm"_v,  // Llvm
    "local"_v, // Local
};

StringView targetTable[] = {
    "linux"_v, // Linux
    "mac"_v,   // MacOS
    "win32"_v, // Win32
};

StringView framePointerTable[] = {
    "all"_v,      // All
    "non-leaf"_v, // NonLeaf
    "needed"_v,   // Needed
};

StringView archTable[] = {
    "x64"_v,   // X64
    "x86"_v,   // X86
    "arm"_v,   // ARM
    "arm64"_v, // ARM64
};

// The name an architecture is written with on the command line, for a diagnostic that has to say
// which one was asked for. One table serves both directions, so a message can never name an
// architecture by a spelling the parser would reject.
StringView archName(TargetArch arch) {
    auto index = Size(arch);
    return index < sizeof(archTable) / sizeof(StringView) ? archTable[index] : "unknown"_v;
}

/*
 * The three levels, and the three older spellings that name the same machines.
 *
 * The aliases are kept because they read better at a call site that is about one instruction set -
 * `-enable-inst avx2` says why a benchmark row exists more clearly than `v3` does - and because
 * every script and fixture already writes them. What is *not* kept is a spelling for a machine no
 * level describes: `avx` alone was Sandy Bridge, which is a v2 part everywhere else, and `sse4.1`
 * alone was Penryn, which is below the floor. Both now name the level that contains them.
 */
struct LevelName { StringView name; TargetExtensions::Level level; };

static const LevelName levelTable[] = {
    { "v2"_v, TargetExtensions::V2 },
    { "v3"_v, TargetExtensions::V3 },
    { "v4"_v, TargetExtensions::V4 },

    { "sse4.2"_v, TargetExtensions::V2 },
    { "avx2"_v, TargetExtensions::V3 },
    { "avx512"_v, TargetExtensions::V4 },
};

/*
 * One dotted segment of a file name, answered against this compilation.
 *
 * The three tables are the ones the command line already reads, so a selector is spelled the way
 * `-target`, `-arch` and `@platform` spell the same thing and there is no second vocabulary to keep
 * in step. `js` and `native` are the platform axis and are not in a table because `mode` holds more
 * than a platform.
 *
 * **An operating system or an architecture excludes a JS build**, rather than being ignored on one:
 * `Linux.x64.yana` is a syscall table, and a target with no syscalls has no business compiling it.
 * That is the same answer `@platform(native)` would give, which is what makes the file form a
 * replacement for the attribute rather than a second mechanism beside it.
 */
TargetSelector targetSelector(const CompileSettings& settings, StringView name, SelectorScope scope) {
    auto isJs = isJsMode(settings);
    auto answer = [](bool matched) { return matched ? TargetSelector::Matched : TargetSelector::Excluded; };

    if(name == "js"_v) return answer(isJs);
    if(name == "native"_v) return answer(!isJs);

    /*
     * `test` - Design-Test.md §3.1, and the one selector that names a *mode* rather than a machine.
     *
     * A `.test.yana` file is a file of its module, so it sees every declaration of every sibling
     * unqualified, `pub` or not - which is the whole of what "unit test" means as against
     * "integration test", obtained with no new concept. And it costs nothing in a shipping build,
     * because a file this compilation does not select is not read at all.
     *
     * It composes with the rest, which is what makes `.test.native.yana` and `.test.v3.yana` the
     * direct answer to "a test for a path that only exists at v3". Unlike an architecture or a
     * level, it excludes nothing else: `test` is a claim about what this compilation *is*.
     */
    /*
     * **And a dependency's test files are never selected**, which is the whole of how a library's
     * tests stay out of every program written in the language. Before this the answer was
     * `settings.test` for every walk, so a `.test.yana` file under `lib/` was read into any
     * consumer's test build: its cases joined their suite, it dragged `Test` and the harness into
     * their compile - one `Show`-sized test file doubled a small one - and since F5 its top-level
     * initializers ran in their program. A library test that failed to compile broke every consumer.
     *
     * The rule is the one every ecosystem lands on and it is a package rule, not a library one: a
     * test file belongs to whoever is compiling its package. `LibrarySource::isOwnPackage` is what
     * lets the standard library still test itself - the walk over `lib/` asks as `Project` when the
     * compilation *is* `base`, and as `Dependency` otherwise.
     */
    if(name == "test"_v) return answer(settings.test && scope == SelectorScope::Project);

    for(U32 i = 0; i < sizeof(targetTable) / sizeof(StringView); i++) {
        if(name == targetTable[i]) return answer(!isJs && settings.target == TargetType(i));
    }

    for(U32 i = 0; i < sizeof(archTable) / sizeof(StringView); i++) {
        if(name == archTable[i]) return answer(!isJs && settings.arch == TargetArch(i));
    }

    /*
     * The x86-64 levels, answered as **at least**: `v3` matches a v3 or a v4 build, exactly as
     * `-enable-inst v3` means "you may use this and everything below it".
     *
     * A level is a selector because a *declaration naming an instruction* has to be able to say
     * which machines have it, and that is the same question `sha` answers below. What a level name
     * must not be used for is choosing between two implementations of a portable operation: the
     * compiler already selects instructions by level, and a source-level fork would be a second
     * answer to that question with nothing keeping the two in step. The distinction is between
     * "this name *is* an instruction" and "this name has a faster spelling on a better machine";
     * only the first belongs here, and `lib/Native/Intrinsic/X86.yana` is the only file that has
     * one.
     *
     * The aliases come from the same table `-enable-inst` reads, so a selector is spelled the way
     * the command line spells it. `avx` is deliberately not among them - it alone was Sandy Bridge,
     * which is a v2 part everywhere in this compiler.
     */
    if(!isJs && settings.arch == TargetArch::X64) {
        for(auto& entry: levelTable) {
            if(name == entry.name) return answer(settings.extensions.level >= entry.level);
        }
    } else {
        for(auto& entry: levelTable) {
            if(name == entry.name) return TargetSelector::Excluded;
        }
    }

    /*
     * The instruction-set extensions that are not a level - `sha` today, and see
     * `TargetExtensions::sha` for why it is not inside one.
     *
     * **Two names per extension and not one, because a selector has no negation.** `sha` selects the
     * declaration written over the instructions and `nosha` the one that stands in for it, and every
     * target satisfies exactly one of the two - which makes them a partition of the same shape
     * `js`/`native` is, and is what lets a *file* hold the hardware implementation. That matters
     * more here than the spelling does: a build without the extension must not *resolve* a body
     * naming instructions it cannot encode, and a body kept behind a compile-time `if` would still
     * be resolved and would still reach a backend in a build with the IR optimizer off.
     *
     * A JS build and a non-x86-64 one are `nosha`, on the same argument an operating system name
     * makes above: what `sha` selects is written in terms of an x86 instruction, and a target with
     * no such instruction has no business compiling it.
     *
     * **`sha` requires v3 as well as the extension**, which is not a claim about the hardware -
     * Goldmont shipped SHA-NI with no AVX at all. It is a claim about `lib/Digest/Hardware.sha.yana`,
     * which is a legacy-encoded region and therefore written in terms of `X86.vzeroupper()`: the
     * reset is an AVX instruction, so the file needs both, and the part of the partition that says
     * so is this line. A SHA-without-AVX target is `nosha` and takes the portable compression, which
     * is the whole of what those parts give up.
     */
    auto hasSha = !isJs && settings.arch == TargetArch::X64 && settings.extensions.sha
               && settings.extensions.level >= TargetExtensions::V3;

    if(name == "sha"_v) return answer(hasSha);
    if(name == "nosha"_v) return answer(!hasSha);

    return TargetSelector::Unknown;
}

Maybe<TargetExtensions::Level> matchLevel(const String& arg) {
    for(auto& entry: levelTable) {
        if(Tritium::toString(entry.name) == arg) return Just(TargetExtensions::Level(entry.level));
    }

    return Nothing();
}

static Maybe<U32> matchString(StringView* table, Size count, const String& arg) {
    for(U32 i = 0; i < count; i++) {
        if(Tritium::toString(table[i]) == arg) {
            return Just(i);
        }
    }

    return Nothing();
}

static void applyHostExtensions(CompileSettings& settings) {
    int info_ecx = 0;
    int info_edx = 0;
    int info7_ebx = 0;
    int info7_ecx = 0;
    int infoex_ecx = 0;
    int infoex_edx = 0;

#ifdef __X86__
#if defined(__MSC_VER)
    int info[4] = {0, 0, 0, 0};

    __cpuid(info, 0);
    int level = info[0];

    __cpuid(info, 0x80000000);
    int extendedLevel = info[0];

    if(level >= 1) {
        __cpuid(info, 1);
        info_ecx = info[2];
        info_edx = info[3];
    }

    if(level >= 7) {
        __cpuid(info, 7);
        info7_ebx = info[1];
        info7_ecx = info[2];
    }

    if(extendedLevel >= 0x80000001) {
        __cpuid(info, 0x80000001);
        infoex_ecx = info[2];
        infoex_edx = info[3];
    }
#elif defined(__GNUC__)
    int eax, ebx, ecx, edx;

    // Use the explicit count version here, as some compilers do not reset ecx correctly causing calls to return nothing.
    __cpuid_count(0, 0, eax, ebx, ecx, edx);
    int level = eax;

    __cpuid_count(0x80000000, 0, eax, ebx, ecx, edx);
    int extendedLevel = eax;

    if(level >= 1) {
        __cpuid_count(1, 0, eax, ebx, ecx, edx);
        info_ecx = ecx;
        info_edx = edx;
    }

    if(level >= 7) {
        __cpuid_count(7, 0, eax, ebx, ecx, edx);
        info7_ebx = ebx;
        info7_ecx = ecx;
    }

    if(extendedLevel >= 0x80000001) {
        __cpuid_count(0x80000001, 0, eax, ebx, ecx, edx);
        infoex_ecx = ecx;
        infoex_edx = edx;
    }
#endif // __MSC_VER

    /*
     * The bits each level is defined by, and the whole level is required rather than the part this
     * backend happens to use today. The name is the psABI's, so what it promises has to be the
     * psABI's too: a build that called a machine v3 because it had AVX2 would be lying about
     * `movbe` and `f16c` to whatever asks next, and on the LLVM path something does - `-mattr=+...`
     * is written straight from this.
     */
    bool hasSSE3    = !!(info_ecx & (1 << 0 ));
    bool hasSSSE3   = !!(info_ecx & (1 << 9 ));
    bool hasCx16    = !!(info_ecx & (1 << 13));
    bool hasSSE4_1  = !!(info_ecx & (1 << 19));
    bool hasSSE4_2  = !!(info_ecx & (1 << 20));
    bool hasPOPCNT  = !!(info_ecx & (1 << 23));
    bool hasMOVBE   = !!(info_ecx & (1 << 22));
    bool hasFMA3    = !!(info_ecx & (1 << 12));
    bool hasF16C    = !!(info_ecx & (1 << 29));
    bool hasAVX     = !!(info_ecx & (1 << 28));
    bool hasXSAVE   = !!(info_ecx & (1 << 26));
    bool hasOSXSAVE = !!(info_ecx & (1 << 27));

    bool hasAVX2    = !!(info7_ebx & (1 << 5 ));
    bool hasBMI1    = !!(info7_ebx & (1 << 3 ));
    bool hasBMI2    = !!(info7_ebx & (1 << 8 ));
    bool hasAVX512F = !!(info7_ebx & (1 << 16));
    bool hasAVX512DQ= !!(info7_ebx & (1 << 17));
    bool hasAVX512CD= !!(info7_ebx & (1 << 28));
    bool hasAVX512BW= !!(info7_ebx & (1 << 30));
    bool hasAVX512VL= !!(info7_ebx & (1 << 31));

    bool hasABM     = !!(infoex_ecx & (1 << 5));

    // The SHA extension, which is in no level - see TargetExtensions::sha. Leaf 7's EBX bit 29, and
    // it needs no `xgetbv` question beside it: the seven instructions are `xmm` operations, so what
    // has to be enabled for them is the SSE state every part meeting the v2 floor already saves.
    bool hasSHA     = !!(info7_ebx & (1 << 29));

    /*
     * **What the processor has is not what the program may use.** The wide registers are extended
     * state, and an operating system that has not enabled saving them leaves their instructions
     * faulting however the feature bits read - so the enabling is asked about too, through the same
     * `xgetbv` a runtime check would use. Without this a `-march=native` default would be a
     * `#UD` on a kernel that never turned AVX on, which is exactly the kind of machine this compiler
     * is also used to write.
     */
    U64 xcr0 = 0;
    if(hasXSAVE && hasOSXSAVE) {
#if defined(__MSC_VER)
        xcr0 = _xgetbv(0);
#elif defined(__GNUC__)
        U32 lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        xcr0 = U64(lo) | (U64(hi) << 32);
#endif
    }

    auto savesSse = (xcr0 & 0x2) != 0;                  // XMM state
    auto savesAvx = savesSse && (xcr0 & 0x4) != 0;      // and the upper half of each YMM
    auto savesAvx512 = savesAvx && (xcr0 & 0xe0) == 0xe0; // opmask, ZMM_hi256, hi16_ZMM

    auto meetsV2 = hasSSE3 && hasSSSE3 && hasSSE4_1 && hasSSE4_2 && hasCx16 && (hasPOPCNT || hasABM);
    auto meetsV3 = meetsV2 && hasAVX && hasAVX2 && hasBMI1 && hasBMI2 && hasFMA3 && hasF16C
                          && hasMOVBE && hasABM && savesAvx;
    auto meetsV4 = meetsV3 && hasAVX512F && hasAVX512BW && hasAVX512CD && hasAVX512DQ && hasAVX512VL
                          && savesAvx512;

    /*
     * A machine below the floor still gets the floor, because the floor is a requirement rather than
     * an observation: there is no code generator here for a part without SSE4.2, and answering "v1"
     * would only move the failure to a form table that has no row to select. Compiling *on* such a
     * machine is fine - what is produced is code for a machine one level up, which is what a cross
     * build always is.
     */
    settings.extensions.level = meetsV4 ? TargetExtensions::V4
                              : meetsV3 ? TargetExtensions::V3
                                        : TargetExtensions::V2;

    // And the extension beside the level, taken from the host on the same terms: an unnamed build is
    // `-march=native`'s bargain, and this is one more thing the host either has or has not.
    settings.extensions.sha = hasSHA;
#endif // __X86__
}

static void applyDefaults(CompileSettings& settings, bool hasArch, bool hasTarget, bool hasFormat, bool hasExtensions) {
    // Set the arch to the current one if nothing was provided.
    //
    // AArch64 is tested for first and by its own macro rather than through the platform header's,
    // because that header's `__X64__` states a *word width* and not an instruction set: an aarch64
    // host defines both `__ARM__` and `__X64__`, so asking for x86-64 first answers x86-64 there.
    if(!hasArch) {
#if defined(__aarch64__) || defined(_M_ARM64)
        settings.arch = TargetArch::ARM64;
#elif __X64__
        settings.arch = TargetArch::X64;
#elif __X86__
        settings.arch = TargetArch::X86;
#elif __ARM__
        settings.arch = TargetArch::ARM;
#else
#error Cannot determine arch for current host, please add a #define here for the current platform.
#endif
    }

    // Set the target to the current one if nothing was provided.
    if(!hasTarget) {
#if __LINUX__
        settings.target = TargetType::Linux;
#elif __OSX__
        settings.target = TargetType::MacOS;
#elif __WINDOWS__
        settings.target = TargetType::Win32;
#else
#error Cannot determine target type for current host, please add a #define here for the current platform.
#endif
    }

    // Set the executable format to the current one if nothing was provided.
    // (this is ignored either way for non-executable modes).
    if(!hasFormat) {
#if __LINUX__
        settings.format = ExecutableFormat::ELF;
#elif __OSX__
        settings.format = ExecutableFormat::MachO;
#elif __WINDOWS__
        settings.format = ExecutableFormat::PE;
#else
#error Cannot determine executable format for current host, please add a #define here for the current platform.
#endif
    }

    // Apply sane defaults if only one of target or format was set explicitly.
    if(hasFormat && !hasTarget) {
        switch(settings.format) {
            case ExecutableFormat::ELF:
                settings.target = TargetType::Linux;
                break;
            case ExecutableFormat::MachO:
                settings.target = TargetType::MacOS;
                break;
            case ExecutableFormat::PE:
                settings.target = TargetType::Win32;
                break;
        }
    } else if(!hasFormat && hasTarget) {
        switch(settings.target) {
            case TargetType::Linux:
                settings.format = ExecutableFormat::ELF;
                break;
            case TargetType::MacOS:
                settings.format = ExecutableFormat::MachO;
                break;
            case TargetType::Win32:
                settings.format = ExecutableFormat::PE;
                break;
        }
    }

    // All AArch64 cpus seem to support NEON (documentation is unclear if this is required though).
    // x86-64 needs no such line any more: its level is v2 unless something raises it, and v2 is the
    // floor rather than a default - see TargetExtensions.
    if(settings.arch == TargetArch::ARM64 && !settings.extensions.neon) {
        settings.extensions.neon = true;
    }

    /*
     * And the level, which is the *host's* where a build named neither an architecture nor a level.
     *
     * That is `-march=native`'s bargain and it is worth stating plainly: a build with no flags
     * produces code for the machine it ran on, so the same source compiled on a Haswell and on a
     * Nehalem is two different programs - eight lanes in a `Vec(Int)` and four. A build that has to
     * be reproducible names its level, which every fixture in the test suite and every row of the
     * benchmark corpus does. Detection is skipped when an architecture was named, since the host's
     * CPUID says nothing about a machine it is not.
     */
    if(!hasArch && !hasExtensions) {
        applyHostExtensions(settings);
    }

    /*
     * And the backend, which is the one default that has to be taken *after* the three above rather
     * than beside them: which code generators exist is a fact about the target, and the target is
     * only settled here.
     *
     * The local one wherever it exists - see NativeBackend. It is the faster build and needs no
     * toolchain installed, which is what a build being run wants; LLVM is the faster program, which
     * is what a build being shipped wants, and it is one flag away. Everywhere else there is nothing
     * to choose and the value stays what it was constructed as.
     */
    if(!settings.explicitBackend && localBackendSupported(settings)) {
        settings.backend = NativeBackend::Local;
    }
}

template<class F, class I>
void parseFlags(const char** argv, Size argc, String& error, F&& onFlag, I&& onInput) {
    // What each single-valued flag was given, for the duplicate check below. A list rather than a
    // map: there are thirty flags and a command line names a handful of them.
    struct Seen { Flag::Type type; String value; };
    Array<Seen> seen;

    auto seenValue = [&](Flag::Type type) -> String* {
        for(auto& entry: seen) {
            if(entry.type == type) return &entry.value;
        }

        return nullptr;
    };

    auto recordValue = [&](Flag::Type type, const String& value) {
        seen.push(Seen { type, value });
    };

    for(Size i = 0; i < argc; i++) {
        auto k = argv[i];

        /*
         * A word that is not a flag is an input path - see CompileSettings::inputs.
         *
         * Before this every argument had to be a flag, so `yana hello.yana` was answered with
         * `Unknown argument "hello.yana"` - the first thing anyone types, refused for a reason that
         * is about this driver's history rather than about the program being compiled. A value
         * belonging to a flag never reaches here: the flag consumed it below.
         */
        if(k[0] != '-') {
            if(!onInput(String(k))) return;
            continue;
        }

        // One dash or two, and `--key=value` as well as `-key value`. Both forms are accepted
        // because both are written: this driver's own flags are `-key value`, and every other
        // compiler a caller has used takes the other, so refusing either buys nothing.
        if(k[0] == '-') k++;
        if(k[0] == '-') k++;

        bool found = false;
        String inlineValue;
        auto joined = false;

        auto separator = k;
        while(*separator && *separator != '=') separator++;

        String key = *separator == '=' ? String(k, Size(separator - k)) : String(k);
        if(*separator == '=') {
            inlineValue = String(separator + 1);
            joined = true;
        }

        for(auto flag: flagTable) {
            if(key == Tritium::toString(flag.name)) {
                if(joined && !flag.argCount) {
                    char buffer[256];
                    auto size = Tritium::format(toBuffer(buffer), "Flag \"%@\" does not take a value", key);
                    error = Tritium::ownedString(buffer, size);
                    return;
                }

                if(!joined && argc - i <= flag.argCount) {
                    char buffer[256];
                    auto size = Tritium::format(toBuffer(buffer), "-%@ needs a value after it", key);
                    error = Tritium::ownedString(buffer, size);
                    return;
                }

                /*
                 * A value flag whose value is the next *flag* - `-mode -to out`, where `-to` was
                 * swallowed as the mode and the real error arrived two steps later as "unrecognized
                 * compilation mode", naming neither argument.
                 *
                 * Refused only in the separated form: `--to=-3` is a value that happens to start
                 * with a dash and was written as one deliberately.
                 */
                if(!joined && flag.argCount && argv[i + 1][0] == '-') {
                    char buffer[256];
                    auto size = Tritium::format(toBuffer(buffer),
                                                "-%@ needs a value, but the next argument is the flag \"%@\". "
                                                "Write --%@=%@ if that was meant as the value.",
                                                key, String(argv[i + 1]), key, String(argv[i + 1]));
                    error = Tritium::ownedString(buffer, size);
                    return;
                }

                String arg;
                if(joined) {
                    arg = inlineValue;
                } else if(flag.argCount) {
                    arg = String(argv[i + 1]);
                    i += flag.argCount;
                }

                /*
                 * The same flag twice - see Flag::repeatable.
                 *
                 * Twice with the same value is allowed, because a script that builds a command line
                 * from two places and arrives at one answer has not contradicted itself. Twice with
                 * two values is refused, naming both: silently taking the last was the behaviour
                 * that made `-mode exe -mode ir` build IR.
                 */
                if(flag.argCount && !flag.repeatable) {
                    auto seen = seenValue(flag.type);
                    if(seen && *seen != arg) {
                        char buffer[512];
                        auto size = Tritium::format(toBuffer(buffer),
                            "-%@ was given twice, as \"%@\" and \"%@\". Name it once.", key, *seen, arg);
                        error = Tritium::ownedString(buffer, size);
                        return;
                    }

                    if(!seen) recordValue(flag.type, arg);
                }

                if(!onFlag(flag.type, move(arg))) return;
                found = true;
            }
        }

        if(!found) {
            char buffer[256];
            error = Tritium::ownedString(buffer, Tritium::format(toBuffer(buffer),
                "Unknown flag \"-%@\". Run `yana -help` for the flags this compiler takes.", key));
            return;
        }
    }
}

/*
 * `-help`.
 *
 * Written out here rather than generated from `flagTable`, because what a reader needs is the shape
 * of an invocation and the four or five flags that answer an actual question - not thirty names in
 * declaration order. The table is the parser's; this is the documentation, and the two are kept in
 * step by the same rule as every other pair of that kind in this compiler: an edit to one is an edit
 * to both.
 *
 * There was no help at all before, which is the reason the first invocation anyone wrote was wrong:
 * with no positional inputs, no default mode and no usage text, the interface could only be learned
 * by reading the parser.
 */
StringView helpText() {
    return R"(yana - the Yana compiler

  yana                       compile the project described by ./yana.toml
  yana <directory>           compile the project there, or the directory as source
  yana <file.yana>           compile one file as a program
  yana explain <name>        report what a function does, and compile nothing
  yana explain <M>.<name>    the one in module M, when several share the name

Where the output goes:
  -to <directory>       write the build here (default: the project's `to`, else `.`)
  -output <name>        call the artifact this (default: the main module's name)

What to build:
  -mode <m>             exe (default), shared, lib, or the pipeline stopped early:
                          ast        the parsed form of every source file
                          ownership  where each local is live, and where it is dropped
                          ir         the resolved, optimized program as IR text
                          lower      the lowered program as IR text (native only)
                          llvm       LLVM IR text (native only)
  -platform <p>         native (default) or js. Decides which @platform declarations exist.
  -main <module>        the module the program starts in, when the input holds several
  -test                 build the @test declarations into a test program

Which machine:
  -target <os>          linux, mac or win32          -arch <a>       x64, x86, arm, arm64
  -format <f>           elf, mach or pe              -backend <b>    llvm or local
  -enable-inst <level>  v2, v3, v4, or sha / neon beside one

How hard to try:
  -opt <0-3>            the LLVM pipeline's level, on -backend llvm
  -inline <level>       none, size, balanced or speed
  -no-ir-opt            switch off the IR optimizer      -no-checks    emit no bounds checks
  -check-locations      inserted checks report their source location

Where things are:
  -project <path>       the yana.toml to read, rather than looking in the working directory
  -no-project           read no project file at all
  -lib <directory>      the standard library, holding Core/Core.yana
  -add <path>           a source root, named explicitly rather than as an input
  -package <name>       the package this compilation is, when no yana.toml says

  -print-modules        list the modules that were found, and the file each came from
  -help                 this text
)"_v;
}

/*
 * `Server.handle` - the module and the function, from the one name the query was given.
 *
 * A qualified name rather than a `-module` flag beside an unqualified one. It is how this language
 * writes a name that needs saying which one it is - `Core.Text`, `State.Idle` - so the query is
 * asked in the same words as everything else, and a flag whose name promised something global
 * (`-module`, next to `-main` and `-package`) but meant something only inside one subcommand is
 * gone. Both spellings of the query read it, which is what makes them the same query.
 *
 * Split at the *last* dot, because everything before it is the module: `Core.Text.hexValue` is
 * `hexValue` in `Core.Text`. A function name cannot contain one, so there is no ambiguity to
 * resolve - and a name with no dot at all is every function of that name, whichever module.
 */
static void setExplainTarget(CompileSettings& settings, String&& name) {
    auto text = name.text();
    auto separator = name.size();
    while(separator > 0 && text[separator - 1] != '.') separator--;

    if(separator == 0 || separator == name.size()) {
        settings.explainName = ::move(name);
        return;
    }

    settings.explainModule = String(text, separator - 1);
    settings.explainName = String(text + separator, name.size() - separator);
}

Result<CompileSettings, String> parseCommandLine(const char** argv, Size argc) {
    if(argc == 0) {
        return Err(String("No arguments provided"));
    }

    CompileSettings settings;
    String error;

    /*
     * `yana explain <name> ...` - the form §7.3 specifies.
     *
     * A leading word rather than a flag, and the only one this driver has: everything else it does
     * is *compile*, and a subcommand for the thing that does not compile is what tells a reader
     * which of the two they are asking for. `-explain <name>` does the same thing for anyone who
     * would rather write flags.
     */
    Size firstFlag = 1;
    if(argc > 1 && String(argv[1]) == "explain") {
        if(argc < 3 || argv[2][0] == '-') {
            return Err(String("The explain query needs a function name: yana explain <name>, "
                              "or yana explain <Module>.<name> for one module's."));
        }

        setExplainTarget(settings, String(argv[2]));
        firstFlag = 3;
    }

    // Whether the platform came from a mode alias rather than from `-platform` - see modeAliasTable.
    // Only a diagnostic reads it: what it decides is which of two contradicting flags to name.
    bool platformFromMode = false;

    bool hasArch = false;
    bool hasTarget = false;
    bool hasFormat = false;
    bool hasExtensions = false;

    auto onInput = [&](String&& path) -> bool {
        settings.inputs.push(move(path));
        return true;
    };

    parseFlags(argv + firstFlag, argc - firstFlag, error, [&](Flag::Type type, String&& value) -> bool {
        switch(type) {
            case Flag::add:
                settings.compileObjects.push(move(value));
                return true;
            case Flag::mainModule:
                settings.mainModules.push(move(value));
                return true;
            case Flag::enableInst:
                hasExtensions = true;
                if(auto level = matchLevel(value)) {
                    // The highest of everything named, so that two flags do not undo each other.
                    if(settings.extensions.level < level.unwrap()) settings.extensions.level = level.unwrap();
                    return true;
                } else if(value == "neon") {
                    settings.extensions.neon = true;
                    return true;
                } else if(value == "sha") {
                    // Beside a level rather than one of them - see TargetExtensions::sha. Written as
                    // a second `-enable-inst`, since naming it does not say which level the rest of
                    // the machine meets.
                    settings.extensions.sha = true;
                    return true;
                } else {
                    error = "Unrecognized instruction set level. Valid levels are: "
                            "v2|v3|v4 (or sse4.2|avx2|avx512, which name the same three), plus the "
                            "extensions `sha` and `neon`, which are named beside a level rather than "
                            "instead of one.";
                    return false;
                }
            case Flag::to:
                settings.outputDir = move(value);
                settings.explicitOutput = true;
                return true;
            case Flag::outputName:
                settings.outputName = move(value);
                return true;
            case Flag::project:
                settings.projectFile = move(value);
                return true;
            case Flag::noProject:
                settings.noProject = true;
                return true;
            case Flag::test:
                settings.test = true;
                return true;
            case Flag::library:
                settings.libraryPath = move(value);
                return true;
            case Flag::package:
                settings.package = move(value);
                return true;
            case Flag::specialize:
                if(value == "always") { settings.forceGeneric = false; return true; }
                if(value == "never") { settings.forceGeneric = true; return true; }
                error = "Specialization is `always` or `never`.";
                return false;
            case Flag::mode:
                if(auto mode = matchString(modeTable, sizeof(modeTable) / sizeof(StringView), value)) {
                    settings.explicitMode = true;
                    settings.mode = (CompileMode)mode.unwrap();

                    // The one mode that needs something computed that a compilation does not
                    // otherwise compute - see compileOwnership.
                    if(settings.mode == CompileMode::Ownership) settings.ownershipRanges = true;
                    return true;
                }

                // The two spellings that carry a platform with them - see modeAliasTable. The
                // platform is recorded as if `-platform` had been written, so that naming both and
                // contradicting yourself is caught below rather than resolved by argument order.
                for(auto& alias: modeAliasTable) {
                    if(Tritium::toString(alias.name) != value) continue;

                    if(settings.explicitPlatform && settings.platform != alias.platform) {
                        error = Tritium::format("-mode %@ is a JavaScript build, which contradicts -platform native.",
                                                value);
                        return false;
                    }

                    settings.explicitMode = true;
                    settings.mode = alias.mode;
                    settings.explicitPlatform = true;
                    settings.platform = alias.platform;
                    platformFromMode = true;
                    return true;
                }

                error = Tritium::format("Unrecognized compilation mode \"%@\". Valid modes are: exe|shared|lib|ast|ownership|ir|lower|llvm.",
                                        value);
                return false;
            case Flag::platform:
                if(auto platform = matchString(platformTable, sizeof(platformTable) / sizeof(StringView), value)) {
                    auto named = (TargetPlatform)platform.unwrap();
                    if(settings.explicitPlatform && settings.platform != named) {
                        // Which of the two wrote the platform down decides which sentence is useful:
                        // one of them did not mention a platform at all - see modeAliasTable.
                        error = platformFromMode
                            ? Tritium::format("-platform %@ contradicts the -mode already named, which is a JavaScript build.", value)
                            : String("The platform was named twice, as both native and js.");
                        return false;
                    }

                    settings.explicitPlatform = true;
                    settings.platform = named;
                    return true;
                } else {
                    error = Tritium::format("Unrecognized platform \"%@\". Valid platforms are: native|js.", value);
                    return false;
                }
            case Flag::target:
                hasTarget = true;
                if(auto target = matchString(targetTable, sizeof(targetTable) / sizeof(StringView), value)) {
                    settings.target = (TargetType)target.unwrap();
                    return true;
                } else {
                    error = Tritium::format("Unrecognized operating system \"%@\". Valid targets are: linux|mac|win32.", value);
                    return false;
                }
            case Flag::arch:
                hasArch = true;
                if(auto arch = matchString(archTable, sizeof(archTable) / sizeof(StringView), value)) {
                    settings.arch = (TargetArch)arch.unwrap();
                    return true;
                } else {
                    error = Tritium::format("Unrecognized instruction set \"%@\". Valid archs are: x64|x86|arm|arm64.", value);
                    return false;
                }
            case Flag::format:
                hasFormat = true;
                if(auto format = matchString(formatTable, sizeof(formatTable) / sizeof(StringView), value)) {
                    settings.format = (ExecutableFormat)format.unwrap();
                    return true;
                } else {
                    error = Tritium::format("Unrecognized executable format \"%@\". Valid formats are: elf|mach|pe.", value);
                    return false;
                }
            case Flag::backend:
                settings.explicitBackend = true;
                if(auto backend = matchString(backendTable, sizeof(backendTable) / sizeof(StringView), value)) {
                    settings.backend = (NativeBackend)backend.unwrap();
                    return true;
                } else {
                    error = Tritium::format("Unrecognized native backend \"%@\". Valid backends are: llvm|local.", value);
                    return false;
                }
            case Flag::framePointer:
                if(auto fp = matchString(framePointerTable, sizeof(framePointerTable) / sizeof(StringView), value)) {
                    settings.framePointer = (FramePointerMode)fp.unwrap();
                    return true;
                } else {
                    error = Tritium::format("Unrecognized frame pointer mode \"%@\". Valid modes are: all|non-leaf|needed.", value);
                    return false;
                }
            case Flag::inlining:
                if(auto level = matchString(inlineTable, sizeof(inlineTable) / sizeof(StringView), value)) {
                    settings.inlining = (InlineLevel)level.unwrap();
                    return true;
                } else {
                    error = Tritium::format("Unrecognized inlining level \"%@\". Valid levels are: none|size|balanced|speed.", value);
                    return false;
                }
            case Flag::optimization: {
                // 0 to 3, as every compiler spells it. What it selects is LLVM's own pipeline; the
                // local amd64 backend has one setting and is it.
                if(value.size() != 1 || value.text()[0] < '0' || value.text()[0] > '3') {
                    error = Tritium::format("Unrecognized optimization level \"%@\". Valid levels are 0-3.", value);
                    return false;
                }

                settings.optimization = U32(value.text()[0] - '0');
                settings.explicitOptimization = true;
                return true;
            }
            case Flag::help:
                settings.help = true;
                return true;
            case Flag::printModules:
                settings.printModules = true;
                return true;
            case Flag::noIrOptimize:
                settings.optimizeIr = false;
                return true;
            case Flag::checkLocations:
                settings.checkLocations = true;
                return true;
            case Flag::noChecks:
                settings.checks = false;
                return true;
            case Flag::explain:
                setExplainTarget(settings, move(value));
                return true;
            case Flag::explainAll:
                settings.explainAll = true;
                return true;
            default:
                error = Tritium::format("Unhandled argument type %@. This is an internal error.", (Size)type);
                return false;
        }
    }, onInput);

    if(error != "") {
        return Err(error);
    }

    settings.explicitExtensions = hasExtensions;
    applyDefaults(settings, hasArch, hasTarget, hasFormat, hasExtensions);
    return Ok(move(settings));
}

Result<void, String> checkSettings(const CompileSettings& settings) {
    if(settings.compileObjects.size() == 0) {
        return Err(String("Nothing to compile. Name a source file or directory, or list the sources "
                          "in a yana.toml."));
    }

    // `-o` names the artifact, and `-to` is the one thing that says where a build goes. A separator
    // here would be a second answer to that question, and one that can point anywhere.
    for(Size i = 0; i < settings.outputName.size(); i++) {
        auto c = settings.outputName.text()[i];
        if(c == '/' || c == '\\') {
            return Err(Tritium::format("-o names the output file \"%@\", which cannot contain a path. "
                                       "Use -to for the directory.", settings.outputName));
        }
    }

    /*
     * What the two axes can be at once.
     *
     * The mode used to answer both questions, so a combination that does not exist could not be
     * written down and needed no check. Now that it can be written down, each one is refused where
     * it would otherwise be silently reinterpreted: emitting LLVM text for a program with no machine
     * code in it, or lowering one that never goes through `lowerProgram` at all.
     */
    if(isJsMode(settings)) {
        if(settings.mode == CompileMode::Lower) {
            return Err(String("-mode lower is native only: the JavaScript backend generates from the "
                              "resolved program and never lowers. Use -mode ir for a JavaScript build."));
        }

        if(settings.mode == CompileMode::Llvm) {
            return Err(String("-mode llvm is native only. Use -platform native, or -mode ir for the "
                              "JavaScript program's IR."));
        }

        if(settings.mode == CompileMode::Shared) {
            return Err(String("-mode shared is native only. A JavaScript library is -mode lib -platform js."));
        }
    }

    /*
     * What the local backend can actually produce.
     *
     * Only when it was *asked for*. A defaulted backend is derived from the same target these
     * questions are about, so it can only ever be one this target supports - and the modes that do
     * not read a backend at all must not become errors because of a value nothing chose. Naming one
     * is different: `-backend local -arch arm64` is a sentence, and answering it with an executable
     * for the wrong machine, or silently with LLVM, are both worse than saying so.
     *
     * Reported here rather than where the file would be written, because all of it is answerable
     * from the flags alone and the useful moment to hear about a target that has no code generator
     * is before the program has been compiled for it. The LLVM path has its own list of the
     * architectures it can emit for - see createMachine - and says so at the same point in its own
     * pipeline; this is the same statement for a backend whose list is shorter.
     */
    /*
     * A named backend is checked only where a backend is what produces the output. The modes that
     * write text about the program reach no code generator at all, so `-mode ir -backend local` is
     * a flag that does not apply rather than a contradiction - and answering it with "the local
     * backend only generates executables" would be refusing a sentence nobody wrote.
     */
    if(settings.explicitBackend && settings.backend == NativeBackend::Local && !isTextMode(settings.mode)) {
        if(settings.mode != CompileMode::Executable) {
            return Err(String("The local backend only generates executables. "
                              "Use -mode exe, or -backend llvm."));
        }

        if(settings.arch != TargetArch::X64) {
            return Err(String("The local backend only generates amd64 code. "
                              "Use -arch x64, or -backend llvm."));
        }

        if(settings.format != ExecutableFormat::ELF || settings.target != TargetType::Linux) {
            return Err(String("The local backend only generates ELF executables for Linux. "
                              "Use -target linux -format elf, or -backend llvm."));
        }
    }

    return Ok();
}