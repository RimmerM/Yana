#include "settings.h"
#include "../util/lexer_util.h"

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
        imageBase,
        backend,
        framePointer,
        inlining,
        switchForm,
        tune,
        vectorWidth,
        optimization,
        explain,
        project,
        library,
        package,
        allocator,
        specialize,
        resolver,

        /*
         * Boolean flags.
         */

        help,
        printModules,
        noIrOptimize,
        noJsirOptimize,
        noChecks,
        checkLocations,
        noReplacements,
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
    { "image-base"_v, 1, Flag::imageBase },
    { "backend"_v, 1, Flag::backend },
    { "frame-pointer"_v, 1, Flag::framePointer },
    { "opt"_v, 1, Flag::optimization },
    { "inline"_v, 1, Flag::inlining },

    // Which shape a multi-way branch is spent in - see SwitchForm. A measurement's knob.
    { "switch-form"_v, 1, Flag::switchForm },

    // Which vendor's recent cores the build favours where they disagree - see TargetTuning.
    { "tune"_v, 1, Flag::tune },
    { "vector-width"_v, 1, Flag::vectorWidth },
    { "explain"_v, 1, Flag::explain },
    { "project"_v, 1, Flag::project },
    { "lib"_v, 1, Flag::library },

    // The package this compilation is, when there is no `yana.toml` to say - which is every driver
    // that resolves a source string rather than a tree. See CompileSettings::package.
    { "package"_v, 1, Flag::package },

    // Which general heap the program is built against - see CompileSettings::allocator.
    // An opaque name: the compiler compares it and knows nothing else about it.
    { "allocator"_v, 1, Flag::allocator },
    { "specialize"_v, 1, Flag::specialize },

    // Which resolve stage compiles the program - see ResolverStage, and Analysis-IR.md part 11.
    { "resolver"_v, 1, Flag::resolver },

    { "help"_v, 0, Flag::help },
    { "h"_v, 0, Flag::help },
    { "print-modules"_v, 0, Flag::printModules },
    { "no-ir-opt"_v, 0, Flag::noIrOptimize },
    { "no-jsir-opt"_v, 0, Flag::noJsirOptimize },
    { "no-checks"_v, 0, Flag::noChecks },
    { "check-locations"_v, 0, Flag::checkLocations },
    { "no-replacements"_v, 0, Flag::noReplacements },
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

// SwitchForm, in declaration order.
StringView switchFormTable[] = {
    "auto"_v,   // Auto
    "chain"_v,  // Chain
    "search"_v, // BinarySearch
    "table"_v,  // JumpTable
};

// TargetTuning, in declaration order.
StringView tuneTable[] = {
    "generic"_v, // Generic
    "intel"_v,   // Intel
    "amd"_v,     // Amd
};

// CompileMode, in declaration order. What comes out; `platformTable` below says what it is for, and
// the two used to be one table - see TargetPlatform.
StringView modeTable[] = {
    "exe"_v,       // Executable
    "shared"_v,    // Shared
    "lib"_v,       // Library
    "ast"_v,       // Ast
    "drops"_v,     // Drops
    "ir"_v,        // Ir
    "lower"_v,     // Lower
    "llvm"_v,      // Llvm
    "mir"_v,       // Mir
    "ssa"_v,       // Ssa
    "jsir"_v,      // Jsir
};

// ResolverStage, in declaration order.
StringView resolverTable[] = {
    "legacy"_v,    // Legacy
    "ownership"_v, // Ownership
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
    "pe"_v,    // PE
    "raw"_v,   // Raw
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
    "bare"_v,  // Bare
};

static Maybe<U32> matchString(StringView* table, Size count, const String& arg);

// The `-target` and `-format` words, for a project file that writes them as keys - see project.cpp.
Maybe<TargetType> targetNamed(const String& name) {
    if(auto target = matchString(targetTable, sizeof(targetTable) / sizeof(StringView), name)) {
        return Just((TargetType)target.unwrap());
    }
    return Nothing();
}

Maybe<ExecutableFormat> formatNamed(const String& name) {
    if(auto format = matchString(formatTable, sizeof(formatTable) / sizeof(StringView), name)) {
        return Just((ExecutableFormat)format.unwrap());
    }
    return Nothing();
}

// `-image-base` is a command-line number rather than a Yana integer literal, so accept the same
// decimal and 0x-prefixed hexadecimal spellings without making the driver depend on a host-sized
// integer parser. The image base is deliberately unsigned: it is an address, not a displacement.
static Maybe<U64> parseImageBase(StringView value) {
    if(value.length == 0) return Nothing();

    const char* first = value.ptr;
    const char* last = first + value.length;
    auto hexadecimal = value.length > 2 && first[0] == '0' && (first[1] == 'x' || first[1] == 'X');
    const char* number = hexadecimal ? first + 2 : first;
    if(number == last) return Nothing();

    bool overflowed = false;
    auto parsed = hexadecimal
        ? parseIntLiteral<16>(number, last, parseHexit, overflowed)
        : parseIntLiteral<10>(number, last, parseDigit, overflowed);

    if(overflowed || number != last) return Nothing();
    return Just(parsed);
}

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

StringView targetName(TargetType target) {
    auto index = Size(target);
    return index < sizeof(targetTable) / sizeof(StringView) ? targetTable[index] : "unknown"_v;
}

/*
 * The three levels - Analysis-Avx512.md §0.2 - as the instruction sets each adds to the one below it
 * and the flags every part of it has.
 *
 * **One table**, and it is the only statement of what a level is: `TargetExtensions::sets` reads it for
 * both backends, the LLVM attribute list is derived from the sets it yields, and the selectors and the
 * command line spell a level by its first column. A second table beside it is how the LLVM half of the
 * v3 description came to be written from memory and wrong once.
 *
 * **Each implication is a claim about every part of the level**, and each was checked before it was
 * written here rather than after: every Haswell and later, Excavator and every Zen has AES-NI and
 * PCLMULQDQ; every Ice Lake, Tiger Lake, Rocket Lake, Sapphire Rapids and Zen 4 has VAES, VPCLMULQDQ,
 * GFNI and SHA. A hypervisor may still hide AES from a guest, which is why host detection asks for the
 * implied flags as well before it answers a level - see `applyHostExtensions`.
 */
struct LevelDesc {
    StringView name;
    TargetExtensions::Level level;
    InstructionSets own;
    InstructionSets implied;
};

static const LevelDesc kLevelTable[] = {
    { "sse4"_v, TargetExtensions::Sse4,
      kSetSse3 | kSetSsse3 | kSetSse41 | kSetSse42 | kSetPopcnt | kSetCx16,
      0 },

    { "avx2"_v, TargetExtensions::Avx2,
      kSetAvx | kSetAvx2 | kSetBmi1 | kSetBmi2 | kSetF16c | kSetFma | kSetLzcnt | kSetMovbe | kSetXsave,
      kSetAes | kSetPclmul },

    { "avx512"_v, TargetExtensions::Avx512,
      kSetAvx512f | kSetAvx512cd | kSetAvx512bw | kSetAvx512dq | kSetAvx512vl | kSetAvx512ifma
        | kSetAvx512vbmi | kSetAvx512vbmi2 | kSetAvx512bitalg | kSetAvx512vpopcntdq | kSetAvx512vnni,
      kSetVaes | kSetVpclmul | kSetGfni | kSetSha },
};

/*
 * The flags - §0.3 - and the lowest level each may be named at.
 *
 * A flag below its level is refused, naming the level: `vaes` is VEX- or EVEX-only, so `sse4` with
 * `vaes` has no instruction to emit and a build that asked for one should hear so rather than get
 * nothing. The width a flag's instructions reach follows the level rather than the flag, which is the
 * backend's to read - `vaes` at `avx2` is the 256-bit VEX forms, at `avx512` the 512-bit ones too.
 */
struct FlagDesc {
    StringView name;
    InstructionSets set;
    TargetExtensions::Level lowest;
};

static const FlagDesc kFlagTable[] = {
    { "aes"_v,          kSetAes,          TargetExtensions::Sse4 },
    { "pclmul"_v,       kSetPclmul,       TargetExtensions::Sse4 },
    { "vaes"_v,         kSetVaes,         TargetExtensions::Avx2 },
    { "vpclmul"_v,      kSetVpclmul,      TargetExtensions::Avx2 },
    { "gfni"_v,         kSetGfni,         TargetExtensions::Sse4 },
    { "sha"_v,          kSetSha,          TargetExtensions::Sse4 },
    { "vp2intersect"_v, kSetVp2intersect, TargetExtensions::Avx512 },
    { "fp16"_v,         kSetFp16,         TargetExtensions::Avx512 },
    { "bf16"_v,         kSetBf16,         TargetExtensions::Avx512 },
    { "sha512"_v,       kSetSha512,       TargetExtensions::Avx2 },
    { "sm3"_v,          kSetSm3,          TargetExtensions::Avx2 },
    { "sm4"_v,          kSetSm4,          TargetExtensions::Avx2 },
};

/*
 * The spellings this compiler used to accept and now refuses, and what each became.
 *
 * Refused rather than aliased, which is §0.4's argument: `x86-64-v4` is a machine other tools read in
 * the psABI's sense, and neither answer an alias could give is safe - mapping it up to `avx512` claims
 * VBMI on Cascade Lake, and mapping it down to `avx2` silently throws away what was asked for. The
 * lower two could have stayed, and do not, so that there is one vocabulary.
 */
static const struct { StringView name; StringView replacement; } kRefusedLevels[] = {
    { "v2"_v, "sse4"_v }, { "x86-64-v2"_v, "sse4"_v }, { "sse4.2"_v, "sse4"_v },
    { "v3"_v, "avx2"_v }, { "x86-64-v3"_v, "avx2"_v },
    { "v4"_v, "avx512"_v }, { "x86-64-v4"_v, "avx512"_v },
};

/*
 * The LLVM attribute each instruction set is, in the order the target machine is told them. A set
 * with two names is SSE4.2: `crc32` is a feature of its own in LLVM even though every part with either
 * has both, and without it `llvm.x86.sse42.crc32.*` fails instruction selection outright rather than
 * expanding into anything, which is how that was found.
 */
static const struct { InstructionSets set; const char* first; const char* second; } kLlvmAttributes[] = {
    { kSetSse3, "sse3", nullptr }, { kSetSsse3, "ssse3", nullptr }, { kSetSse41, "sse4.1", nullptr },
    { kSetSse42, "sse4.2", "crc32" }, { kSetPopcnt, "popcnt", nullptr }, { kSetCx16, "cx16", nullptr },

    { kSetAvx, "avx", nullptr }, { kSetAvx2, "avx2", nullptr }, { kSetBmi1, "bmi", nullptr },
    { kSetBmi2, "bmi2", nullptr }, { kSetFma, "fma", nullptr }, { kSetF16c, "f16c", nullptr },
    { kSetLzcnt, "lzcnt", nullptr }, { kSetMovbe, "movbe", nullptr }, { kSetXsave, "xsave", nullptr },

    { kSetAvx512f, "avx512f", nullptr }, { kSetAvx512bw, "avx512bw", nullptr },
    { kSetAvx512cd, "avx512cd", nullptr }, { kSetAvx512dq, "avx512dq", nullptr },
    { kSetAvx512vl, "avx512vl", nullptr }, { kSetAvx512ifma, "avx512ifma", nullptr },
    { kSetAvx512vbmi, "avx512vbmi", nullptr }, { kSetAvx512vbmi2, "avx512vbmi2", nullptr },
    { kSetAvx512bitalg, "avx512bitalg", nullptr }, { kSetAvx512vpopcntdq, "avx512vpopcntdq", nullptr },
    { kSetAvx512vnni, "avx512vnni", nullptr },

    { kSetAes, "aes", nullptr }, { kSetPclmul, "pclmul", nullptr }, { kSetVaes, "vaes", nullptr },
    { kSetVpclmul, "vpclmulqdq", nullptr }, { kSetGfni, "gfni", nullptr }, { kSetSha, "sha", nullptr },
    { kSetVp2intersect, "avx512vp2intersect", nullptr }, { kSetFp16, "avx512fp16", nullptr },
    { kSetBf16, "avx512bf16", nullptr }, { kSetSha512, "sha512", nullptr }, { kSetSm3, "sm3", nullptr },
    { kSetSm4, "sm4", nullptr },
};

InstructionSets TargetExtensions::sets() const {
    InstructionSets all = flags;

    for(auto& entry: kLevelTable) {
        if(entry.level > level) break;
        all |= entry.own | entry.implied;
    }

    return all;
}

// The other direction, searched rather than indexed: a row states its own level, so the two agreeing
// with the array's order is a coincidence a reordering would take away.
static StringView nameOfLevel(TargetExtensions::Level level) {
    for(auto& entry: kLevelTable) {
        if(entry.level == level) return entry.name;
    }

    assertTrue(false); // a level with no row in kLevelTable
    return ""_v;
}

Maybe<TargetExtensions::Level> matchLevel(StringView name) {
    for(auto& entry: kLevelTable) {
        if(entry.name == name) return Just(TargetExtensions::Level(entry.level));
    }

    return Nothing();
}

InstructionSets matchExtensionFlag(StringView name) {
    for(auto& entry: kFlagTable) {
        if(entry.name == name) return entry.set;
    }

    return 0;
}

Maybe<StringView> replacedLevelSpelling(StringView name) {
    for(auto& entry: kRefusedLevels) {
        if(entry.name == name) return Just(StringView(entry.replacement));
    }

    return Nothing();
}

Array<const char*> llvmAttributesOf(const TargetExtensions& extensions) {
    Array<const char*> names;
    auto sets = extensions.sets();

    for(auto& entry: kLlvmAttributes) {
        if(!(sets & entry.set)) continue;
        names.push(entry.first);
        if(entry.second) names.push(entry.second);
    }

    return names;
}

Maybe<String> checkExtensionFlags(const TargetExtensions& extensions) {
    for(auto& flag: kFlagTable) {
        if(!(extensions.flags & flag.set) || extensions.level >= flag.lowest) continue;

        // A flag the level implies is never below it, so the only question left is which level to name.
        return Just(Tritium::format("The extension `%@` needs at least the `%@` level, which this build does "
                                    "not name - add `-enable-inst %@`.",
                                    flag.name, nameOfLevel(flag.lowest), nameOfLevel(flag.lowest)));
    }

    return Nothing();
}

/*
 * One dotted segment of a file name, answered against this compilation.
 *
 * The three tables are the ones the command line already reads, so a selector is spelled the way
 * `-target`, `-arch` and `@platform` spell the same thing and there is no second vocabulary to keep
 * in step. `js` and `native` are the platform axis and are not in a table because `mode` holds more
 * than a platform.
 *
 * **A system environment or an architecture excludes a JS build**, rather than being ignored on one:
 * `Linux.linux.x64.yana` is a syscall table, and a target with no syscalls has no business compiling it.
 * That is the same answer `@platform(native)` would give, which is what makes the file form a
 * replacement for the attribute rather than a second mechanism beside it.
 */
/*
 * The allocator axis - see the declaration, which is where the argument for it being its own
 * function lives.
 *
 * Three lines, and two of them are the point: the comparison is against `settings.allocator` and
 * nothing else, so a provider the library adds needs no edit here, and `Unknown` is never answered -
 * every name is a legal one to *write*, and whether anything declares it is the roles' question
 * rather than this one's. That is the difference from `targetSelector`, which knows every machine
 * there is and reports a name that is not one.
 */
TargetSelector allocatorSelector(const CompileSettings& settings, StringView name) {
    auto chosen = StringView { settings.allocator.text(), settings.allocator.size() };
    return name == chosen ? TargetSelector::Matched : TargetSelector::Excluded;
}

/*
 * The resolve-stage axis - see the declaration. Two names, compared against the one setting.
 */
TargetSelector resolverSelector(const CompileSettings& settings, StringView name) {
    if(name == "legacy"_v) {
        return settings.resolver == ResolverStage::Legacy ? TargetSelector::Matched : TargetSelector::Excluded;
    }

    if(name == "ownership"_v) {
        return settings.resolver == ResolverStage::Ownership ? TargetSelector::Matched : TargetSelector::Excluded;
    }

    return TargetSelector::Unknown;
}

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
     * The x86-64 levels, answered as **at least**: `avx2` matches an `avx2` or an `avx512` build,
     * exactly as `-enable-inst avx2` means "you may use this and everything below it".
     *
     * A level is a selector because a *declaration naming an instruction* has to be able to say
     * which machines have it, and that is the same question a flag answers below. It may also select
     * a **replacement** - Design-Replacements.md - which is a second body of one portable function,
     * held to being observationally identical to the first. What a level must not select is a
     * second *meaning*: a `@platform(avx2)` pair of two functions under one name is a source-level
     * fork of something the compiler already decides by level, with nothing keeping the two in
     * step, and a replacement is what keeps them in step - its baseline is resolved on every build
     * and `-no-replacements` runs the program without it.
     *
     * The names come from the same table `-enable-inst` reads, so a selector is spelled the way the
     * command line spells it. A psABI spelling (`v3`) is not among them and is reported as the
     * unknown name it now is.
     */
    auto onX64 = !isJs && settings.arch == TargetArch::X64;

    if(auto level = matchLevel(name)) {
        return onX64 ? answer(settings.extensions.level >= level.unwrap()) : TargetSelector::Excluded;
    }

    /*
     * The instruction-set extensions that are not a level - Analysis-Avx512.md §0.3 - answered from
     * the same `has` both backends read, so a flag the level *implies* selects exactly as one that was
     * named: `aes` holds on every `avx2` build, and `sha` on every `avx512` one.
     *
     * **One name and no negation.** There used to be a `nosha` beside `sha`, because a selector has no
     * negation and the portable compression needed a file that only a build without the extension
     * read. That is a replacement's job now - Design-Replacements.md: the portable body is the
     * baseline every build resolves, and the hardware one replaces it where the flag holds - so no
     * extension needs a twin, and the portable half is compiled on the machines that have the
     * instructions as well as on the ones that do not.
     *
     * A JS build and a non-x86-64 one have none of them, on the same argument a system environment name
     * makes above: what a flag selects is written in terms of an x86 instruction, and a target with no
     * such instruction has no business compiling it.
     *
     * **The level is not folded in.** `lib/Digest/ShaBlock.sha.avx2.yana` needs AVX as well, for
     * `X86.vzeroupper()`, and it says so in its own name; a declaration that needs only the extension
     * is free to say only that. Goldmont shipped SHA-NI with no AVX at all.
     */
    if(auto flag = matchExtensionFlag(name)) {
        return answer(onX64 && settings.extensions.has(flag));
    }

    return TargetSelector::Unknown;
}

/*
 * What one name *is*, for the implication table below - the vocabulary's own facts, sorted.
 *
 * Each kind is read off the table `targetSelector` reads, so a selector added there is a selector
 * this knows the kind of. `Level` carries its rank; every other kind is compared by name.
 */
enum class SelectorKind: U8 {
    Platform,  // `js`, `native`
    System,    // `linux`, `mac`, `win32`, `bare`
    Arch,      // `x64`, `x86`, `arm`, `arm64`
    Level,     // `sse4`, `avx2`, `avx512`
    Extension, // `sha`, `aes` and the rest of the flags
    Other,     // `test`, and anything no table names
};

static SelectorKind selectorKind(StringView name, U8& rank) {
    rank = 0;
    if(name == "js"_v || name == "native"_v) return SelectorKind::Platform;

    for(auto entry: targetTable) if(name == entry) return SelectorKind::System;
    for(auto entry: archTable) if(name == entry) return SelectorKind::Arch;

    if(auto level = matchLevel(name)) {
        rank = U8(level.unwrap());
        return SelectorKind::Level;
    }

    if(matchExtensionFlag(name)) return SelectorKind::Extension;
    return SelectorKind::Other;
}

/*
 * Whether a build satisfying `p` always satisfies `q` - one written name against another.
 *
 * The table §3.5 lists, and nothing it does not: a system environment, an architecture, a level and an
 * extension each imply `native`; a level implies the levels below it and `x64`; an extension implies
 * `x64`, which is the architecture its instructions are. And a level implies every flag it implies -
 * `avx512` implies `sha` - since those are claims about every part of the level (Analysis-Avx512.md
 * §0.2), and a selector holding on every build of one is exactly what implication means here.
 */
static bool literalImplies(const SelectorLiteral& p, const SelectorLiteral& q) {
    if(p.axis != q.axis) return false;
    if(p.name == q.name) return true;
    if(p.axis != SelectorAxis::Target) return false;

    U8 pRank = 0, qRank = 0;
    auto pKind = selectorKind(p.name, pRank);
    auto qKind = selectorKind(q.name, qRank);

    switch(qKind) {
        case SelectorKind::Platform:
            return q.name == "native"_v && pKind != SelectorKind::Platform && pKind != SelectorKind::Other;
        case SelectorKind::Arch:
            return q.name == "x64"_v && (pKind == SelectorKind::Level || pKind == SelectorKind::Extension);
        case SelectorKind::Level:
            return pKind == SelectorKind::Level && pRank >= qRank;
        case SelectorKind::Extension: {
            if(pKind != SelectorKind::Level) return false;

            TargetExtensions level;
            level.level = TargetExtensions::Level(pRank);
            return level.has(matchExtensionFlag(q.name));
        }
        default:
            return false;
    }
}

/*
 * Whether no build satisfies both - one written name against another.
 *
 * `js` against anything that implies `native`; two system environments, two architectures, and a level
 * or an extension against an architecture that is not `x64`; two allocator providers; the two resolve
 * stages. Two different names on the allocator and resolve-stage axes are always disjoint, because
 * each of those axes has exactly one answer per compilation.
 */
static bool literalsDisjoint(const SelectorLiteral& p, const SelectorLiteral& q) {
    if(p.axis != q.axis) return false;
    if(p.name == q.name) return false;
    if(p.axis != SelectorAxis::Target) return true;

    U8 pRank = 0, qRank = 0;
    auto pKind = selectorKind(p.name, pRank);
    auto qKind = selectorKind(q.name, qRank);

    auto impliesNative = [](SelectorKind kind, StringView name) {
        return kind == SelectorKind::System || kind == SelectorKind::Arch || kind == SelectorKind::Level
            || kind == SelectorKind::Extension || name == "native"_v;
    };

    if(p.name == "js"_v) return impliesNative(qKind, q.name);
    if(q.name == "js"_v) return impliesNative(pKind, p.name);

    if(pKind == SelectorKind::System && qKind == SelectorKind::System) return true;
    if(pKind == SelectorKind::Arch && qKind == SelectorKind::Arch) return true;

    auto onX64 = [](SelectorKind kind) { return kind == SelectorKind::Level || kind == SelectorKind::Extension; };
    if(onX64(pKind) && qKind == SelectorKind::Arch) return q.name != "x64"_v;
    if(onX64(qKind) && pKind == SelectorKind::Arch) return p.name != "x64"_v;

    return false;
}

void appendFileSelectors(SelectorCondition& into, StringView stem) {
    auto provider = false;

    for(Size i = 0, from = 0; i <= stem.length; i++) {
        if(i < stem.length && stem.ptr[i] != '.') continue;

        auto selector = StringView { stem.ptr + from, i - from };
        from = i + 1;
        if(!selector.length) continue;

        // The walk `selectsFileName` makes, and the same routing: the segment after the marker is the
        // allocator's, the two stage names are the resolve stage's, and everything else is a machine.
        if(provider) {
            into.add(into.addClause(), selector, SelectorAxis::Allocator);
            provider = false;
        } else if(selector == kAllocatorMarker) {
            provider = true;
        } else if(selector == "legacy"_v || selector == "ownership"_v) {
            into.add(into.addClause(), selector, SelectorAxis::Resolver);
        } else if(selector != "test"_v) {
            into.add(into.addClause(), selector, SelectorAxis::Target);
        }
    }
}

/*
 * Every clause of `q` is implied by some clause of `p`, where a clause implies another when each of
 * its alternatives implies one of the other's. That is implication between conjunctions of
 * disjunctions, decided clause by clause - sound, and complete for every condition the library
 * writes, which is one name per clause.
 */
bool selectorsImply(const SelectorCondition& p, const SelectorCondition& q) {
    for(U16 qc = 0; qc < q.clauses; qc++) {
        auto implied = false;

        for(U16 pc = 0; pc < p.clauses && !implied; pc++) {
            auto every = false;

            for(auto& a: p.literals) {
                if(a.clause != pc) continue;

                auto some = false;
                for(auto& b: q.literals) some = some || (b.clause == qc && literalImplies(a, b));

                every = some;
                if(!some) break;
            }

            implied = every;
        }

        if(!implied) return false;
    }

    return true;
}

// Some clause of `p` and some clause of `q` whose every pair of alternatives is disjoint.
bool selectorsDisjoint(const SelectorCondition& p, const SelectorCondition& q) {
    for(U16 pc = 0; pc < p.clauses; pc++) {
        for(U16 qc = 0; qc < q.clauses; qc++) {
            auto every = true;
            auto any = false;

            for(auto& a: p.literals) {
                if(a.clause != pc) continue;

                for(auto& b: q.literals) {
                    if(b.clause != qc) continue;

                    any = true;
                    every = every && literalsDisjoint(a, b);
                }
            }

            if(any && every) return true;
        }
    }

    return false;
}

/*
 * The two axes over one file name - see the declaration, which is where the split of answer from
 * policy is argued.
 *
 * The loop is the whole of it. Segment zero is the file's own name; every one after it is a
 * selector, asked of the machine axis, except the segment following `allocator`, which is asked of
 * the policy axis and of nothing else. That the marker is what routes a segment - rather than the
 * machine axis failing to recognize it - is what keeps a name that is neither axis reportable, and
 * is written out at `kAllocatorMarker`.
 */
bool selectsFileName(const CompileSettings& settings, StringView stem, SelectorScope scope,
                     bool& isTest, StringView& unknown) {
    auto selected = true;
    auto provider = false;

    for(Size i = 0, from = 0; i <= stem.length; i++) {
        if(i < stem.length && stem.ptr[i] != '.') continue;

        if(from > 0) {
            auto selector = StringView { stem.ptr + from, i - from };

            if(provider) {
                // The name after the marker. Never asked of the machine axis, so a provider is free
                // to be called `linux` - the marker already said which axis this segment is on.
                if(allocatorSelector(settings, selector) == TargetSelector::Excluded) selected = false;
                provider = false;
            } else if(selector == kAllocatorMarker) {
                provider = true;
            } else if(auto stage = resolverSelector(settings, selector); stage != TargetSelector::Unknown) {
                if(stage == TargetSelector::Excluded) selected = false;
            } else {
                auto answer = targetSelector(settings, selector, scope);

                // Kept as the walk goes past, since a caller's last sight of the base name is here -
                // see ast::Module::test.
                if(selector == "test"_v) isTest = true;

                if(answer == TargetSelector::Excluded) selected = false;
                if(answer == TargetSelector::Unknown && !unknown.length) unknown = selector;
            }
        }

        from = i + 1;
    }

    // A marker with nothing after it names no provider, and is the one shape of mistake this axis
    // can see for itself. Reported as the segment it is, by whoever reports.
    if(provider && !unknown.length) unknown = kAllocatorMarker;

    return selected;
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
    int info7_edx = 0;
    int info7s1_eax = 0;
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
        __cpuidex(info, 7, 0);
        int subleaves = info[0];
        info7_ebx = info[1];
        info7_ecx = info[2];
        info7_edx = info[3];

        if(subleaves >= 1) {
            __cpuidex(info, 7, 1);
            info7s1_eax = info[0];
        }
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
        int subleaves = eax;
        info7_ebx = ebx;
        info7_ecx = ecx;
        info7_edx = edx;

        // Subleaf 1 is where the extensions newer than AVX-512 went - SHA-512, SM3, SM4 and BF16.
        if(subleaves >= 1) {
            __cpuid_count(7, 1, eax, ebx, ecx, edx);
            info7s1_eax = eax;
        }
    }

    if(extendedLevel >= 0x80000001) {
        __cpuid_count(0x80000001, 0, eax, ebx, ecx, edx);
        infoex_ecx = ecx;
        infoex_edx = edx;
    }
#endif // __MSC_VER

    /*
     * The bits each level is defined by, and the whole level is required rather than the part this
     * backend happens to use today: a build that called a machine `avx2` because it had AVX2 would be
     * lying about `movbe` and `f16c` to whatever asks next, and on the LLVM path something does -
     * `-mattr=+...` is written straight from the level table.
     */
    bool hasSSE3    = !!(info_ecx & (1 << 0 ));
    bool hasPCLMUL  = !!(info_ecx & (1 << 1 ));
    bool hasSSSE3   = !!(info_ecx & (1 << 9 ));
    bool hasCx16    = !!(info_ecx & (1 << 13));
    bool hasSSE4_1  = !!(info_ecx & (1 << 19));
    bool hasSSE4_2  = !!(info_ecx & (1 << 20));
    bool hasPOPCNT  = !!(info_ecx & (1 << 23));
    bool hasMOVBE   = !!(info_ecx & (1 << 22));
    bool hasAES     = !!(info_ecx & (1 << 25));
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
    bool hasIFMA    = !!(info7_ebx & (1 << 21));
    bool hasAVX512CD= !!(info7_ebx & (1 << 28));
    bool hasAVX512BW= !!(info7_ebx & (1 << 30));
    bool hasAVX512VL= !!(info7_ebx & (1 << 31));

    bool hasVBMI      = !!(info7_ecx & (1 << 1 ));
    bool hasVBMI2     = !!(info7_ecx & (1 << 6 ));
    bool hasGFNI      = !!(info7_ecx & (1 << 8 ));
    bool hasVAES      = !!(info7_ecx & (1 << 9 ));
    bool hasVPCLMUL   = !!(info7_ecx & (1 << 10));
    bool hasVNNI      = !!(info7_ecx & (1 << 11));
    bool hasBITALG    = !!(info7_ecx & (1 << 12));
    bool hasVPOPCNTDQ = !!(info7_ecx & (1 << 14));

    bool hasVP2INTERSECT = !!(info7_edx & (1 << 8 ));
    bool hasFP16         = !!(info7_edx & (1 << 23));

    bool hasSHA512  = !!(info7s1_eax & (1 << 0));
    bool hasSM3     = !!(info7s1_eax & (1 << 1));
    bool hasSM4     = !!(info7s1_eax & (1 << 2));
    bool hasBF16    = !!(info7s1_eax & (1 << 5));

    bool hasABM     = !!(infoex_ecx & (1 << 5));

    // The SHA extension. Leaf 7's EBX bit 29, and it needs no `xgetbv` question beside it: the seven
    // instructions are `xmm` operations, so what has to be enabled for them is the SSE state every
    // part meeting the floor already saves. AES-NI, PCLMULQDQ and GFNI are the same kind of claim.
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

    /*
     * **A level is met only with the flags it implies**, because the implication is what the build will
     * claim: an `avx2` build uses AES-NI without being told. Every part of the level has them, so on
     * bare metal this asks nothing extra - but a hypervisor may hide AES from a guest that has AVX2, and
     * answering `avx2` there would be a `#UD` in the first cipher the program ran. Such a guest is
     * `sse4`, which is the honest answer to the question this is.
     */
    auto meetsSse4 = hasSSE3 && hasSSSE3 && hasSSE4_1 && hasSSE4_2 && hasCx16 && (hasPOPCNT || hasABM);
    auto meetsAvx2 = meetsSse4 && hasAVX && hasAVX2 && hasBMI1 && hasBMI2 && hasFMA3 && hasF16C
                            && hasMOVBE && hasABM && savesAvx && hasAES && hasPCLMUL;
    auto meetsAvx512 = meetsAvx2 && hasAVX512F && hasAVX512BW && hasAVX512CD && hasAVX512DQ && hasAVX512VL
                            && hasIFMA && hasVBMI && hasVBMI2 && hasBITALG && hasVPOPCNTDQ && hasVNNI
                            && hasVAES && hasVPCLMUL && hasGFNI && hasSHA && savesAvx512;

    /*
     * A machine below the floor still gets the floor, because the floor is a requirement rather than
     * an observation: there is no code generator here for a part without SSE4.2, and answering
     * anything lower would only move the failure to a form table that has no row to select. Compiling
     * *on* such a machine is fine - what is produced is code for a machine one level up, which is
     * what a cross build always is.
     */
    auto hostLevel = meetsAvx512 ? TargetExtensions::Avx512
                   : meetsAvx2 ? TargetExtensions::Avx2
                               : TargetExtensions::Sse4;
    settings.extensions.level = hostLevel;

    /*
     * And the flags beside the level, taken from the host on the same terms: an unnamed build is
     * `-march=native`'s bargain, and each of these is one more thing the host either has or has not.
     *
     * Only a flag the level may carry is kept - a VEX-only extension on an `sse4` host (a part whose
     * OS never enabled AVX state) has nothing to be emitted as - which is the same rule
     * `checkExtensionFlags` holds a named flag to, applied here by dropping rather than by refusing.
     * The wide ones also need the state the level saves, which the level having been met says.
     */
    const struct { bool has; InstructionSets set; } detected[] = {
        { hasAES, kSetAes }, { hasPCLMUL, kSetPclmul }, { hasVAES, kSetVaes }, { hasVPCLMUL, kSetVpclmul },
        { hasGFNI, kSetGfni }, { hasSHA, kSetSha }, { hasVP2INTERSECT, kSetVp2intersect },
        { hasFP16, kSetFp16 }, { hasBF16, kSetBf16 }, { hasSHA512, kSetSha512 }, { hasSM3, kSetSm3 },
        { hasSM4, kSetSm4 },
    };

    InstructionSets flags = 0;
    for(auto& entry: detected) {
        if(!entry.has) continue;

        TargetExtensions candidate;
        candidate.level = hostLevel;
        candidate.flags = entry.set;
        if(checkExtensionFlags(candidate).isNothing()) flags |= entry.set;
    }

    settings.extensions.flags = flags;
#endif // __X86__
}

/*
 * The operating system, the executable format and the backend - the part of the defaults a project
 * file can still change after the command line has been read.
 *
 * Separate from applyDefaults for that reason: `-target`/`-format` and a `yana.toml`'s `target` and
 * `format` are the same two settings, so the pairing between them and the backend derived from both
 * is asked again once the file has had its say, with `hasTarget`/`hasFormat` meaning "named by
 * either". Running it twice is the same as running it once on the final answers, because every
 * branch reads only what was named and recomputes the rest.
 */
void settleTargetAndFormat(CompileSettings& settings, bool hasTarget, bool hasFormat) {
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
            case ExecutableFormat::Raw:
                // Raw has no operating-system ABI of its own. Keep Linux as the default so that
                // `-format raw` is a raw spelling of the ordinary native target, while
                // `-target bare -format raw` explicitly selects freestanding source files.
                settings.target = TargetType::Linux;
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
            case TargetType::Bare:
                // Bare is intentionally independent from the container. ELF remains the default
                // here; `-format raw` is the separate choice for a flat image.
                settings.format = ExecutableFormat::ELF;
                break;
        }
    }

    /*
     * And the backend, which is the one default that has to be taken *after* the three above rather
     * than beside them: which code generators exist is a fact about the target, and the target is
     * only settled here.
     *
     * The local one wherever it exists - see NativeBackend. It is the faster build and needs no
     * toolchain installed, which is what a build being run wants; LLVM is the faster program, which
     * is what a build being shipped wants, and it is one flag away. Everywhere else LLVM is the only
     * one there is. Assigned both ways rather than only raised to `Local`, because a project file
     * settles the target a second time and may name one the first answer does not hold for.
     */
    if(!settings.explicitBackend) {
        settings.backend = localBackendSupported(settings) ? NativeBackend::Local : NativeBackend::Llvm;
    }
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

    settleTargetAndFormat(settings, hasTarget, hasFormat);

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
  -mode <m>             exe (default), or the pipeline stopped early:
                          ast        the parsed form of every source file
                          drops      where each local is live, and where it is dropped
                                     (-resolver legacy)
                          ir         the resolved, optimized program as IR text
                                     (-resolver legacy)
                          lower      the lowered program as IR text (native only)
                          llvm       LLVM IR text (native only)
                          mir        the ownership MIR
                          ssa        the optimized ownership SSA
                          jsir       the JavaScript target IR (implies -platform js)
                        shared and lib are reserved for library builds, which
                        neither stage implements yet.
  -platform <p>         native (default) or js. Decides which @platform declarations exist.
  -main <module>        the module the program starts in, when the input holds several
  -test                 build the @test declarations into a test program

Which machine:
  -target <t>           linux, mac, win32 or bare   -arch <a>       x64, x86, arm, arm64
  -format <f>           elf, mach, pe or raw        -image-base <n> raw image load address
                                                        (default: 0x400000)
  -backend <b>          llvm or local
  -enable-inst <set>    sse4 (the floor), avx2 or avx512 - avx2 implies aes and pclmul, and
                        avx512 is Ice Lake/Zen 4 and implies vaes, vpclmul, gfni and sha as
                        well. Or an extension beside a level: aes, pclmul, gfni, sha (sse4);
                        vaes, vpclmul, sha512, sm3, sm4 (avx2); vp2intersect, fp16, bf16
                        (avx512); neon on ARM. With no level named, the host's
  -tune <t>             generic (default), intel or amd - whose recent cores to favour
  -vector-width <w>     128, 256 or 512 - the widest Vec(a) the level may use, never wider than it

How hard to try:
  -opt <0-3>            the LLVM pipeline's level, on -backend llvm
  -inline <level>       none, size, balanced or speed
  -switch-form <f>      auto (default), chain, search or table - how a `match` dispatches
  -no-ir-opt            switch off the IR optimizer      -no-checks    emit no bounds checks
  -no-jsir-opt          switch off the JSIR optimizer, on -platform js
  -check-locations      inserted checks report their source location
  -no-replacements      run every function's baseline body, never an @replaces one

Which resolve stage:
  -resolver <r>         ownership (default) - the rewrite in compiler/resolve_mir and
                        compiler/resolve_ssa, which compiles both platforms and is what
                        -mode mir, ssa and jsir dump - or legacy, compiler/resolve, which
                        is kept for comparison and answers -mode ir and drops.
                        See doc/inflight/Analysis-IR.md part 11.

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
            case Flag::enableInst: {
                hasExtensions = true;
                auto name = StringView { value.text(), value.size() };

                if(auto level = matchLevel(name)) {
                    // The highest of everything named, so that two flags do not undo each other.
                    if(settings.extensions.level < level.unwrap()) settings.extensions.level = level.unwrap();
                    return true;
                }

                // Beside a level rather than one of them - see TargetExtensions::flags. Whether the level
                // is high enough is asked once every flag has been read, since a level may come later.
                if(auto flag = matchExtensionFlag(name)) {
                    settings.extensions.flags |= flag;
                    return true;
                }

                if(value == "neon") {
                    settings.extensions.neon = true;
                    return true;
                }

                if(auto replacement = replacedLevelSpelling(name)) {
                    error = Tritium::format("The instruction set level \"%@\" is spelled `%@` in this compiler - see "
                                            "`-help`. The psABI names are not accepted, because `x86-64-v4` does not "
                                            "mean what `avx512` does.", value, replacement.unwrap());
                    return false;
                }

                error = Tritium::format("Unrecognized instruction set \"%@\". Valid levels are sse4|avx2|avx512, and "
                                        "the extensions named beside one are aes|pclmul|vaes|vpclmul|gfni|sha|"
                                        "vp2intersect|fp16|bf16|sha512|sm3|sm4, plus neon on ARM.", value);
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
            case Flag::allocator:
                // Every non-empty name is legal to write. Whether anything declares it is answered
                // after selection, by the roles - see `allocatorSelector`.
                if(value.size() == 0) {
                    error = "An allocator needs a name.";
                    return false;
                }

                settings.allocator = move(value);
                return true;
            case Flag::specialize:
                if(value == "always") { settings.forceGeneric = false; return true; }
                if(value == "never") { settings.forceGeneric = true; return true; }
                error = "Specialization is `always` or `never`.";
                return false;
            case Flag::resolver:
                if(auto stage = matchString(resolverTable, sizeof(resolverTable) / sizeof(StringView), value)) {
                    settings.resolver = (ResolverStage)stage.unwrap();
                    settings.explicitResolver = true;
                    return true;
                }

                error = "The resolver is `legacy` or `ownership`.";
                return false;
            case Flag::mode:
                if(auto mode = matchString(modeTable, sizeof(modeTable) / sizeof(StringView), value)) {
                    settings.explicitMode = true;
                    settings.mode = (CompileMode)mode.unwrap();

                    // The one mode that needs something computed that a compilation does not
                    // otherwise compute - see compileDrops.
                    if(settings.mode == CompileMode::Drops) settings.ownershipRanges = true;

                    // The two dumps only one stage produces. Selected here rather than checked
                    // later, for the reason the line above exists: a mode that needs a switch set
                    // should set it, not leave one that can be forgotten.
                    // The JavaScript tier's dump is one target's as well as one stage's, so it
                    // selects both - see CompileMode::Jsir. Written here rather than left to
                    // `checkSettings` so that `-mode jsir` alone is a complete command line.
                    //
                    // Not over a `-platform native` written earlier (J162): the two are a
                    // contradiction, and `checkSettings` says so whichever order they came in -
                    // rather than the later one winning, which let `-platform native -mode jsir`
                    // build JavaScript and sent `-mode jsir -platform native` to the native driver.
                    if(settings.mode == CompileMode::Jsir) {
                        settings.resolver = ResolverStage::Ownership;
                        settings.explicitResolver = true;
                        if(!settings.explicitPlatform || settings.platform != TargetPlatform::Native) {
                            settings.platform = TargetPlatform::Js;
                        }
                    }

                    if(settings.mode == CompileMode::Mir || settings.mode == CompileMode::Ssa) {
                        settings.resolver = ResolverStage::Ownership;
                        settings.explicitResolver = true;
                    }

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

                error = Tritium::format("Unrecognized compilation mode \"%@\". Valid modes are: exe|shared|lib|ast|drops|ir|lower|llvm|mir|ssa|jsir|js.",
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
                    error = Tritium::format("Unrecognized target \"%@\". Valid targets are: linux|mac|win32|bare.", value);
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
                    error = Tritium::format("Unrecognized executable format \"%@\". Valid formats are: elf|mach|pe|raw.", value);
                    return false;
                }
            case Flag::imageBase:
                if(auto base = parseImageBase(StringView { value.text(), value.size() })) {
                    settings.imageBase = base.unwrap();
                    settings.explicitImageBase = true;
                    return true;
                }

                error = Tritium::format("Unrecognized image base \"%@\". Use a decimal or 0x-prefixed hexadecimal address.", value);
                return false;
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
            case Flag::switchForm:
                if(auto form = matchString(switchFormTable, sizeof(switchFormTable) / sizeof(StringView), value)) {
                    settings.switchForm = (SwitchForm)form.unwrap();
                    return true;
                } else {
                    error = Tritium::format("Unrecognized switch form \"%@\". Valid forms are: auto|chain|search|table.", value);
                    return false;
                }
            case Flag::tune:
                if(auto tuning = matchString(tuneTable, sizeof(tuneTable) / sizeof(StringView), value)) {
                    settings.tuning = (TargetTuning)tuning.unwrap();
                    return true;
                } else {
                    error = Tritium::format("Unrecognized tuning \"%@\". Valid tunings are: generic|intel|amd.", value);
                    return false;
                }
            case Flag::vectorWidth:
                if(value == "128" || value == "256" || value == "512") {
                    settings.vectorWidthLimit = value == "128" ? 16 : value == "256" ? 32 : 64;
                    return true;
                }

                error = Tritium::format("Unrecognized vector width \"%@\". Valid widths are 128, 256 and 512 bits.", value);
                return false;
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
            case Flag::noJsirOptimize:
                settings.optimizeJsir = false;
                return true;
            case Flag::checkLocations:
                settings.checkLocations = true;
                return true;
            case Flag::noReplacements:
                settings.noReplacements = true;
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

    if(auto refused = checkExtensionFlags(settings.extensions)) {
        return Err(refused.unwrap());
    }

    settings.explicitExtensions = hasExtensions;
    settings.explicitTarget = hasTarget;
    settings.explicitFormat = hasFormat;
    applyDefaults(settings, hasArch, hasTarget, hasFormat, hasExtensions);
    return Ok(move(settings));
}

/*
 * The two modes the shipping stage still answers alone - see ResolverStage, whose default is the
 * rewrite since 2026-09-22 (J81).
 *
 * `ir` and `drops` are dumps of `compiler/resolve`'s own IR, which the rewrite does not have and
 * whose replacements are `mir` and `ssa`. `shared` and `lib` used to be listed here too, because the
 * sentence saying they were not implemented was the shipping driver's - so the default stage
 * answered them with "write -resolver legacy", and that stage then resolved the whole program to
 * say it could not build one. They are refused by `checkSettings` for both stages since J150.
 */
bool isLegacyOnlyMode(CompileMode mode) {
    return mode == CompileMode::Ir || mode == CompileMode::Drops;
}

Result<void, String> checkSettings(const CompileSettings& settings) {
    if(settings.compileObjects.size() == 0) {
        return Err(String("Nothing to compile. Name a source file or directory, or list the sources "
                          "in a yana.toml."));
    }

    /*
     * A mode only the old stage answers, asked of the new default.
     *
     * **Refused rather than quietly compiled with the old stage.** The alternative - falling back -
     * is the mistake the JavaScript checks below already name: a person who writes `-mode ir` to look
     * at what their program became would be shown a different compiler's answer, and nothing on the
     * command line would say so. Nothing needs the fallback either: no `yana.toml` in the tree sets
     * `mode` to one of these, and every test driver that wants the old stage now names it.
     *
     * Only when the resolver was *not* named. `-resolver legacy -mode ir` is the sentence this
     * message asks for and must go through, and `-resolver ownership -mode ir` keeps the refusal it
     * already had - the one below for a JavaScript build, and the driver's own for a native one,
     * which say what the rewrite does have rather than pointing back here.
     */
    if(!settings.explicitResolver && isLegacyOnlyMode(settings.mode)) {
        auto mode = modeTable[(Size)settings.mode];
        return Err(Tritium::format("-mode %@ is a dump of compiler/resolve's IR, which is no "
                                   "longer the default stage. Write -resolver legacy for it, or "
                                   "%@ for this stage's own dumps.", mode,
                                   isJsMode(settings) ? "-mode mir, -mode ssa or -mode jsir"_v
                                                      : "-mode mir or -mode ssa"_v));
    }

    /*
     * `-mode jsir` beside `-platform native` (J162) - the JavaScript target's IR, asked of a native
     * build. The mode selects the platform when nothing else did, so reaching here native means the
     * platform was written too, in either order.
     */
    if(settings.mode == CompileMode::Jsir && settings.platform != TargetPlatform::Js) {
        return Err(String("-mode jsir is the JavaScript target's IR and implies -platform js, and "
                          "-platform native was also written. Drop one of them."));
    }

    /*
     * The two library modes, which neither stage builds.
     *
     * Refused here, for every stage and before anything is compiled, rather than by whichever driver
     * reached the end of a compile: the shipping stage resolved the whole program before its `switch`
     * said "not implemented yet", and the default stage pointed at the shipping one as if it had an
     * answer. What either would need is a format for resolved declarations that a later compile can
     * link against, plus - for `shared` - an exported symbol table and position-independent
     * placement; none of it exists in `compiler/resolve` to be moved over.
     */
    if(settings.mode == CompileMode::Shared || settings.mode == CompileMode::Library) {
        return Err(Tritium::format("-mode %@ is not implemented yet, by either resolve stage: a "
                                   "library build needs a format for resolved declarations that "
                                   "neither one has.", modeTable[(Size)settings.mode]));
    }

    /*
     * `-specialize never`, asked of the stage that does not read it - the refusal above, for a
     * switch rather than a mode.
     *
     * It is `compiler/resolve`'s `Program::Specialization::Generic` and nothing else reads it, so
     * the rewrite would build a byte-identical binary with it and without it: a person
     * comparing the two forms would have compared one form with itself. Refused rather than given a
     * meaning here, because the rewrite's answer to "which form" is per declaration - it chooses,
     * and `@generic` pins the erased one - and a switch forcing every generic erased is a feature
     * (Analysis-IR.md §11.4's milestone 6 remainder) that this sentence should not pretend exists.
     * `always` is the default and the rewrite's own behaviour, so it stays accepted.
     */
    if(settings.forceGeneric && settings.resolver != ResolverStage::Legacy) {
        return Err(String("-specialize never is compiler/resolve's. The ownership stage chooses each "
                          "generic's form itself, and @generic on a declaration pins the erased "
                          "one - write that, or -resolver legacy for the switch."));
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
            return Err(String("-mode lower is native only: a JavaScript build is generated from JSIR "
                              "and never lowers. Use -mode jsir for a JavaScript build's IR."));
        }

        if(settings.mode == CompileMode::Llvm) {
            return Err(String("-mode llvm is native only. Use -platform native, or -mode jsir for the "
                              "JavaScript program's IR."));
        }
    }

    /*
     * The rewritten stage on a JavaScript build - Analysis-IR-JS.md milestone 2.
     *
     * This used to refuse the combination outright, on the argument that `-mode mir -platform js`
     * was a combination that did not exist; milestone 0 narrowed it to the modes below the fork,
     * because the parse, the MIR and the optimized SSA of a JavaScript program are three things this
     * stage genuinely has. JSIR is the other half of that fork and is now built, so `-mode jsir` and
     * `-mode exe` have answers too and a JavaScript program comes out of this pipeline.
     *
     * What is left is `ir` and `drops` - the *other* stage's dumps of an IR this one does not have;
     * the two library modes are refused for both stages above. Refused by name, saying what is
     * missing: the alternative of quietly compiling such a build with the shipping stage is how a
     * person ends up measuring the wrong compiler.
     */
    // On both platforms since J162: a native build used to reach the driver, which reported the
    // same refusal as a `1:1:` diagnostic after reading the program.
    if(settings.resolver == ResolverStage::Ownership &&
       (settings.mode == CompileMode::Ir || settings.mode == CompileMode::Drops)) {
        return Err(Tritium::format("-mode ir and -mode drops are the shipping stage's own dumps of "
                                   "an IR the ownership pipeline does not have. Its dumps are %@.",
                                   isJsMode(settings) ? "-mode mir, -mode ssa and -mode jsir"_v
                                                      : "-mode mir, -mode ssa and -mode lower"_v));
    }

    if(isJsMode(settings) && settings.resolver == ResolverStage::Ownership &&
       settings.mode != CompileMode::Ast && settings.mode != CompileMode::Mir &&
       settings.mode != CompileMode::Ssa && settings.mode != CompileMode::Jsir &&
       settings.mode != CompileMode::Executable) {
        return Err(String("-resolver ownership answers -mode ast, mir, ssa, jsir and exe for "
                          "-platform js."));
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

        if(!localBackendSupported(settings)) {
            return Err(String("The local backend currently generates x64 ELF images for Linux or bare "
                              "targets, and raw images for those targets. Use -backend llvm or choose "
                              "-target linux -format elf, -target linux -format raw, or "
                              "-target bare -format elf or raw."));
        }
    }

    if(settings.explicitImageBase && settings.format != ExecutableFormat::Raw) {
        return Err(String("-image-base only applies to -format raw."));
    }

    if(settings.format == ExecutableFormat::Raw &&
       (settings.arch != TargetArch::X64 ||
        (settings.target != TargetType::Linux && settings.target != TargetType::Bare))) {
        return Err(String("-format raw currently supports x64 Linux and bare targets only."));
    }

    return Ok();
}
