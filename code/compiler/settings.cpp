#include "settings.h"

#ifdef __GNUC__
#include <cpuid.h>
#endif

StringBuffer modeTable[] = {
    "lib"_buffer,       // Library
    "exe"_buffer,       // NativeExecutable
    "shared"_buffer,    // NativeShared
    "js"_buffer,        // JsExecutable
    "jslib"_buffer,     // JsLibrary
    "ir"_buffer,        // Ir
    "llvm"_buffer,      // Llvm
};

StringBuffer formatTable[] = {
    "elf"_buffer,   // ELF
    "mach"_buffer,  // MachO
    "pe"_buffer,  // PE
};

StringBuffer targetTable[] = {
    "linux"_buffer, // Linux
    "mac"_buffer,   // MacOS
    "win32"_buffer, // Win32
};

StringBuffer archTable[] = {
    "x64"_buffer,   // X64
    "x86"_buffer,   // X86
    "arm"_buffer,   // ARM
    "arm64"_buffer, // ARM64
};

StringBuffer sseTable[] = {
    "sse"_buffer,
    "sse2"_buffer,
    "sse3"_buffer,
    "ssse3"_buffer,
    "sse4.1"_buffer,
    "sse4.2"_buffer,
    "avx"_buffer,
    "avx2"_buffer,
    "avx512"_buffer,
};

static Maybe<U32> matchString(StringBuffer* table, Size count, const String& arg) {
    for(U32 i = 0; i < count; i++) {
        if(toString(table[i]) == arg) {
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

    // Parse the supported instruction sets from the cpuid values.
    bool hasSSE    = !!(info_edx & (1 << 25));
    bool hasSSE2   = !!(info_edx & (1 << 26));
    bool hasSSE3   = !!(info_ecx & (1 << 0 ));
    bool hasSSSE3  = !!(info_ecx & (1 << 9 ));
    bool hasSSE4_1 = !!(info_ecx & (1 << 19));
    bool hasSSE4_2 = !!(info_ecx & (1 << 20));
    bool hasPOPCNT = !!(info_ecx & (1 << 23));
    bool hasAVX    = !!(info_ecx & (1 << 28));
    bool hasFMA3   = !!(info_ecx & (1 << 12));

    bool hasAVX2 = !!(info7_ebx & (1 << 5));
    bool hasAVX512 = !!(info7_ebx & (1 << 16));

    bool hasABM = !!(infoex_ecx & (1 << 5));

    settings.extensions.lzcnt = hasABM;
    settings.extensions.popcnt = hasPOPCNT || hasABM;
    settings.extensions.fma3 = hasFMA3;

    if(hasSSE)    settings.extensions.sse = TargetExtensions::SSE;
    if(hasSSE2)   settings.extensions.sse = TargetExtensions::SSE2;
    if(hasSSE3)   settings.extensions.sse = TargetExtensions::SSE3;
    if(hasSSSE3)  settings.extensions.sse = TargetExtensions::SSSE3;
    if(hasSSE4_1) settings.extensions.sse = TargetExtensions::SSE4_1;
    if(hasSSE4_2) settings.extensions.sse = TargetExtensions::SSE4_2;
    if(hasAVX)    settings.extensions.sse = TargetExtensions::AVX;
    if(hasAVX2)   settings.extensions.sse = TargetExtensions::AVX2;
    if(hasAVX512) settings.extensions.sse = TargetExtensions::AVX512;
#endif // __X86__
}

static void applyDefaults(CompileSettings& settings, bool hasArch, bool hasTarget, bool hasFormat, bool hasExtensions) {
    // Set the arch to the current one if nothing was provided.
    if(!hasArch) {
#if __X64__
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
        setings.target = TargetType::Win32;
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

    if(settings.arch == TargetArch::X64 && settings.extensions.sse < TargetExtensions::SSE2) {
        // AMD64 instruction set implies SSE2 support.
        settings.extensions.sse = TargetExtensions::SSE2;
    } else if(settings.arch == TargetArch::ARM64 && !settings.extensions.neon) {
        // All AArch64 cpus seem to support NEON (documentation is unclear if this is required though).
        settings.extensions.neon = true;
    }

    // If no explicit arch and extensions were provided, we default to what the host supports.
    if(!hasArch && !hasExtensions) {
        applyHostExtensions(settings);
    }
}

template<class F>
void parsePairs(const char** argv, Size argc, F&& onPair) {
    for(Size i = 0; i < argc; i += 2) {
        auto k = argv[i];
        if(k[0] == '-') k++;

        auto v = argv[i + 1];
        if(!onPair(String(k), String(v))) break;
    }
}

Result<CompileSettings, String> parseCommandLine(const char** argv, Size argc) {
    if(argc == 0) {
        return Err(String("No command invocation line provided"));
    }

    CompileSettings settings;
    String error;

    bool hasMode = false;
    bool hasArch = false;
    bool hasTarget = false;
    bool hasFormat = false;
    bool hasExtensions = false;

    parsePairs(argv + 1, argc - 1, [&](String&& key, String&& value) -> bool {
        if(key == "add") {
            settings.compileObjects.push(value);
            return true;
        }

        if(key == "to") {
            if(settings.outputDir != "") {
                error = "Only one output directory can be provided";
                return false;
            } else {
                settings.outputDir = value;
                return true;
            }
        }

        if(key == "mode") {
            hasMode = true;
            if(auto mode = matchString(modeTable, sizeof(modeTable) / sizeof(StringBuffer), value)) {
                settings.mode = (CompileMode)mode.unwrap();
                return true;
            } else {
                error = "Unrecognized compilation mode. Valid modes are: lib|exe|shared|js|jslib|ir|llvm.";
                return false;
            }
        }

        if(key == "target") {
            hasTarget = true;
            if(auto target = matchString(targetTable, sizeof(targetTable) / sizeof(StringBuffer), value)) {
                settings.target = (TargetType)target.unwrap();
                return true;
            } else {
                error = "Unrecognized platform target. Valid targets are: linux|mac|win32.";
                return false;
            }
        }

        if(key == "arch") {
            hasArch = true;
            if(auto arch = matchString(archTable, sizeof(archTable) / sizeof(StringBuffer), value)) {
                settings.arch = (TargetArch)arch.unwrap();
                return true;
            } else {
                error = "Unrecognized target instruction set. Valid archs are: x64|x86|arm|arm64.";
                return false;
            }
        }

        if(key == "format") {
            hasFormat = true;
            if(auto format = matchString(formatTable, sizeof(formatTable) / sizeof(StringBuffer), value)) {
                settings.format = (ExecutableFormat)format.unwrap();
                return true;
            } else {
                error = "Unrecognized executable format. Valid formats are: elf|mach|pe.";
                return false;
            }
        }

        if(key == "inst") {
            hasExtensions = true;
            if(auto v = matchString(sseTable, sizeof(sseTable) / sizeof(StringBuffer), value)) {
                auto sse = (TargetExtensions::SSEMode)(v.unwrap() + 1);
                if(settings.extensions.sse < sse) settings.extensions.sse = sse;

                // All targets that support SSE4.2 also support popcnt.
                if(sse >= TargetExtensions::SSE4_2) settings.extensions.popcnt = true;

                // All targets that support AVX2 also support lzcnt and fma3.
                if(sse >= TargetExtensions::AVX2) {
                    settings.extensions.lzcnt = true;
                    settings.extensions.fma3 = true;
                }
                return true;
            } else if(value == "popcnt") {
                settings.extensions.popcnt = true;
                return true;
            } else if(value == "lzcnt") {
                settings.extensions.lzcnt = true;
                return true;
            } else if(value == "neon") {
                settings.extensions.neon = true;
                return true;
            } else {
                error = "Unrecognized instruction set extension.";
                return false;
            }
        }

        char buffer[256];
        error = ownedString(buffer, Tritium::formatString(toBuffer(buffer), "Unknown argument \"%@\""_buffer, value) - buffer);
        return false;
    });

    if(settings.outputDir == "") {
        settings.outputDir = String(argv[0]);
    }

    if(settings.compileObjects.size() == 0) {
        error = "No input objects provided. Add inputs with -add <path>.";
    }

    if(!hasMode) {
        error = "No compilation mode provided. Set the mode with -mode <lib|exe|shared|js|jslib|ir|llvm>.";
    }

    if(error != "") {
        return Err(error);
    }

    applyDefaults(settings, hasArch, hasTarget, hasFormat, hasExtensions);
    return Ok(move(settings));
}