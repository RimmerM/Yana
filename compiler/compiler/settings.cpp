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
        root,
        enableInst,

        /*
         * Value flags.
         */

        to,
        mode,
        target,
        arch,
        format,
        backend,
        framePointer,
        inlining,
        optimization,
        explain,
        module,
        project,
        library,

        /*
         * Boolean flags.
         */

        printModules,
        printAst,
        printIr,
        noOptimize,
        noChecks,
        explainAll,
        noProject,
        test,
    };

    StringView name;
    U32 argCount;
    Type type;
};

Flag flagTable[] = {
    { "add"_v, 1, Flag::add },
    { "root"_v, 1, Flag::root },
    { "enable-inst"_v, 1, Flag::enableInst },

    { "to"_v, 1, Flag::to },
    { "mode"_v, 1, Flag::mode },
    { "target"_v, 1, Flag::target },
    { "arch"_v, 1, Flag::arch },
    { "format"_v, 1, Flag::format },
    { "backend"_v, 1, Flag::backend },
    { "frame-pointer"_v, 1, Flag::framePointer },
    { "opt"_v, 1, Flag::optimization },
    { "inline"_v, 1, Flag::inlining },
    { "explain"_v, 1, Flag::explain },
    { "module"_v, 1, Flag::module },
    { "project"_v, 1, Flag::project },
    { "lib"_v, 1, Flag::library },

    { "print-modules"_v, 0, Flag::printModules },
    { "print-ast"_v, 0, Flag::printAst },
    { "print-ir"_v, 0, Flag::printIr },
    { "no-opt"_v, 0, Flag::noOptimize },
    { "no-checks"_v, 0, Flag::noChecks },
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

StringView modeTable[] = {
    "lib"_v,       // Library
    "exe"_v,       // NativeExecutable
    "shared"_v,    // NativeShared
    "js"_v,        // JsExecutable
    "jslib"_v,     // JsLibrary
    "llvm"_v,      // Llvm
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
TargetSelector targetSelector(const CompileSettings& settings, StringView name) {
    auto isJs = isJsMode(settings.mode);
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
    if(name == "test"_v) return answer(settings.test);

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

template<class F>
void parseFlags(const char** argv, Size argc, String& error, F&& onFlag) {
    for(Size i = 0; i < argc; i++) {
        auto k = argv[i];

        // One dash or two, and `--key=value` as well as `-key value`. Both forms are here because
        // Analysis-Ambient.md §7.3 writes the query as `yana explain handle --module=Server` while
        // every flag this driver already had is written the other way, and a documented invocation
        // that the program rejects is worse than either convention.
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
                    auto size = Tritium::format(toBuffer(buffer), "Not enough arguments to flag \"%@\"", key);
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

                if(!onFlag(flag.type, move(arg))) return;
                found = true;
            }
        }

        if(!found) {
            char buffer[256];
            error = Tritium::ownedString(buffer, Tritium::format(toBuffer(buffer), "Unknown argument \"%@\"", key));
            return;
        }
    }
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
            return Err(String("The explain query needs a function name: yana explain <name> [--module=<M>]"));
        }

        settings.explainName = String(argv[2]);
        firstFlag = 3;
    }

    bool hasArch = false;
    bool hasTarget = false;
    bool hasFormat = false;
    bool hasExtensions = false;

    parseFlags(argv + firstFlag, argc - firstFlag, error, [&](Flag::Type type, String&& value) -> bool {
        switch(type) {
            case Flag::add:
                settings.compileObjects.push(move(value));
                return true;
            case Flag::root:
                settings.rootObjects.push(move(value));
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
                if(settings.outputDir != "") {
                    error = "Only one output directory can be provided";
                    return false;
                } else {
                    settings.outputDir = move(value);
                    settings.explicitOutput = true;
                    return true;
                }
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
            case Flag::mode:
                settings.explicitMode = true;
                if(auto mode = matchString(modeTable, sizeof(modeTable) / sizeof(StringView), value)) {
                    settings.mode = (CompileMode)mode.unwrap();
                    return true;
                } else {
                    error = "Unrecognized compilation mode. Valid modes are: lib|exe|shared|js|jslib|llvm.";
                    return false;
                }
            case Flag::target:
                hasTarget = true;
                if(auto target = matchString(targetTable, sizeof(targetTable) / sizeof(StringView), value)) {
                    settings.target = (TargetType)target.unwrap();
                    return true;
                } else {
                    error = "Unrecognized platform target. Valid targets are: linux|mac|win32.";
                    return false;
                }
            case Flag::arch:
                hasArch = true;
                if(auto arch = matchString(archTable, sizeof(archTable) / sizeof(StringView), value)) {
                    settings.arch = (TargetArch)arch.unwrap();
                    return true;
                } else {
                    error = "Unrecognized target instruction set. Valid archs are: x64|x86|arm|arm64.";
                    return false;
                }
            case Flag::format:
                hasFormat = true;
                if(auto format = matchString(formatTable, sizeof(formatTable) / sizeof(StringView), value)) {
                    settings.format = (ExecutableFormat)format.unwrap();
                    return true;
                } else {
                    error = "Unrecognized executable format. Valid formats are: elf|mach|pe.";
                    return false;
                }
            case Flag::backend:
                settings.explicitBackend = true;
                if(auto backend = matchString(backendTable, sizeof(backendTable) / sizeof(StringView), value)) {
                    settings.backend = (NativeBackend)backend.unwrap();
                    return true;
                } else {
                    error = "Unrecognized native backend. Valid backends are: llvm|local.";
                    return false;
                }
            case Flag::framePointer:
                if(auto fp = matchString(framePointerTable, sizeof(framePointerTable) / sizeof(StringView), value)) {
                    settings.framePointer = (FramePointerMode)fp.unwrap();
                    return true;
                } else {
                    error = "Unrecognized frame pointer mode. Valid modes are: all|non-leaf|needed.";
                    return false;
                }
            case Flag::inlining:
                if(auto level = matchString(inlineTable, sizeof(inlineTable) / sizeof(StringView), value)) {
                    settings.inlining = (InlineLevel)level.unwrap();
                    return true;
                } else {
                    error = "Unrecognized inlining level. Valid levels are: none|size|balanced|speed.";
                    return false;
                }
            case Flag::optimization: {
                // 0 to 3, as every compiler spells it. What it selects is LLVM's own pipeline; the
                // local amd64 backend has one setting and is it.
                if(value.size() != 1 || value.text()[0] < '0' || value.text()[0] > '3') {
                    error = "Unrecognized optimization level. Valid levels are 0-3.";
                    return false;
                }

                settings.optimization = U32(value.text()[0] - '0');
                return true;
            }
            case Flag::printModules:
                settings.printModules = true;
                return true;
            case Flag::printAst:
                settings.printAst = true;
                return true;
            case Flag::printIr:
                settings.printIr = true;
                return true;
            case Flag::noOptimize:
                settings.optimizeIr = false;
                return true;
            case Flag::noChecks:
                settings.checks = false;
                return true;
            case Flag::explain:
                settings.explainName = move(value);
                return true;
            case Flag::module:
                settings.explainModule = move(value);
                return true;
            case Flag::explainAll:
                settings.explainAll = true;
                return true;
            default:
                error = Tritium::format("Unhandled argument type %@. This is an internal error.", (Size)type);
                return false;
        }
    });

    if(settings.outputDir == "") {
        settings.outputDir = String(argv[0]);
    }

    if(error != "") {
        return Err(error);
    }

    settings.explicitExtensions = hasExtensions;
    applyDefaults(settings, hasArch, hasTarget, hasFormat, hasExtensions);
    return Ok(move(settings));
}

Result<void, String> checkSettings(const CompileSettings& settings) {
    if(settings.compileObjects.size() == 0) {
        return Err(String("No input objects provided. Add inputs with -add <path>, "
                          "or list them as `sources` in a yana.toml."));
    }

    // The explain query emits nothing, so there is no output format to ask for. It still *has* a
    // mode, because `@platform` selects which declarations exist and the answer is therefore the
    // answer for one target; the default is simply the one every other flag already defaults to.
    if(!settings.explicitMode && !settings.explaining()) {
        return Err(String("No compilation mode provided. Set the mode with "
                          "-mode <lib|exe|shared|js|jslib|ir|llvm>, or as `target` in a yana.toml."));
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
    if(settings.explicitBackend && settings.backend == NativeBackend::Local) {
        if(settings.mode != CompileMode::NativeExecutable) {
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