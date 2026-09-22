#pragma once

#include <Core.h>

/*
 * Which machine the generated program runs on - `-platform`, and `platform` in a `yana.toml`.
 *
 * **Its own axis rather than a reading of the mode**, which is the distinction the two used to share
 * one enum for: `-mode js` said both "produce a program" and "produce it for JavaScript", so the
 * modes that are neither - an IR dump, an AST dump - had no way to say which platform they were a
 * dump *of*, and the JavaScript resolve IR was reachable only through a debug flag. A mode says what
 * comes out; this says what it is for.
 *
 * This is what `@platform(js)` and `@platform(native)` select on, and the two are genuinely
 * different programs rather than two encodings of one: the host-shaped implementations of `String`,
 * `Array`, `Map` and `Storage` are separate declarations, so a JS build and a native build do not
 * share a resolved program. See Analysis-JS.md §2.4.
 */
enum class TargetPlatform: U8 {
    Native, /// Machine code for the target and architecture the other flags name.
    Js,     /// JavaScript, which has one target and reads none of `-target`, `-arch` or `-format`.
};

/*
 * What the compilation produces - `-mode`, and `mode` in a `yana.toml`.
 *
 * The four that emit something a machine runs are the first four; the three after them are the
 * pipeline stopped early, each one stage later than the last. `-mode ast` is what the parser made,
 * `-mode ir` the resolved program the backends read, `-mode lower` the form both native backends
 * are generated from, and `-mode llvm` LLVM's own text - so the ladder from source to executable can
 * be halted anywhere and read. They replace the `-print-ast`/`-print-ir` debug flags, which wrote
 * fixed file names beside the source they came from and had no way to say "and stop here".
 */
enum class CompileMode {
    Executable, /// A program: a native executable, or a JavaScript file that runs standalone.
    Shared,     /// A native shared library for dynamic linking (.dll, .so, .dylib).
    Library,    /// A Yana library that can be included in other programs.
    Ast,        /// The parsed form of every source file, and nothing after parsing.
    Drops,      /// What the ownership passes concluded: where each local is live, and where it is dropped.
    Ir,         /// The resolved and optimized program, as resolve IR text.
    Lower,      /// The lowered program, as lower IR text. Native only - the JS backend never lowers.
    Llvm,       /// LLVM IR text for the program, rather than an object file built from it.

    /// The two dumps of the rewritten stage - doc/inflight/Analysis-IR.md part 11. `mir` is the
    /// checked ownership MIR - the document calls it OMIR - and `ssa` the converted ownership SSA,
    /// its OSSA, after `resolve_ssa/opt` has run over it. Optimized for the reason `-mode ir` is:
    /// that is the program the backend is given, and `-no-ir-opt` is how the form before it is asked
    /// for. Both imply `-resolver ownership`, since nothing else produces either.
    Mir,
    Ssa,

    /// The JavaScript target's own dump - Analysis-IR-JS.md §8.4. The typed managed-value IR between
    /// OSSA and the syntax tree, with its representations, its shapes, its ABIs and one body per
    /// function. Implies `-resolver ownership -platform js`, since nothing else produces one - the
    /// same argument `mir` and `ssa` make, which is that a tier whose dump is a driver flag is a
    /// tier whose output is asserted from its first fixture.
    Jsir,
};

/*
 * Which resolve stage compiles this program - Analysis-IR.md part 11.2.
 *
 * `Ownership` is the rewrite in `compiler/resolve_mir` and `compiler/resolve_ssa`, which shares the
 * parser and everything from `compiler/lower` down and nothing in between, and **is the default on
 * both platforms** since 2026-09-22 (J81). `Legacy` is `compiler/resolve`, which is now the
 * comparison stage: what is still only its is `-mode ir`, `-mode drops`, `-mode shared`,
 * `-mode lib` and the goldens the test drivers pin it for.
 *
 * The default flipped rather than the old stage being deleted, deliberately: two front halves that
 * can compile one fixture is the whole value of the second set of goldens, and a default that names
 * the stage a person actually ships is what keeps the *other* one from being what they measure.
 *
 * A run-time switch rather than a build-time one, deliberately: what is worth having is one binary
 * that can compile one fixture both ways, so that a disagreement between them is a difference in
 * what the two front halves *decided* with the whole backend held fixed.
 */
enum class ResolverStage {
    Legacy,
    Ownership,
};

/// Whether this compilation emits JavaScript rather than machine code - see TargetPlatform. Takes
/// the settings rather than one field of them because the answer used to be read off the mode, and
/// a call that still passes a mode should stop compiling rather than silently mean something else.
inline bool isJsMode(const struct CompileSettings& settings);

/// Whether a compilation mode produces a whole program rather than something to be linked or
/// imported into one. This is what decides where reachability starts: a program has exactly one
/// root - where it starts - and everything else it can arrive at, it arrives at from there, while a
/// library's declarations are the roots because whoever will call them is not in this compilation.
/// `Ast` is not among them because it stops before there is a program to be reachable in.
/// See markProgramReachable.
inline bool isExecutableMode(CompileMode mode) {
    return mode == CompileMode::Executable || mode == CompileMode::Drops ||
           mode == CompileMode::Ir || mode == CompileMode::Lower || mode == CompileMode::Llvm ||
           mode == CompileMode::Mir || mode == CompileMode::Ssa || mode == CompileMode::Jsir;
}

/// Whether a mode writes text about the program rather than something that runs. The three that do
/// share every step up to the point they stop at, and none of them reads a backend or a format.
inline bool isTextMode(CompileMode mode) {
    return mode == CompileMode::Ast || mode == CompileMode::Drops ||
           mode == CompileMode::Ir || mode == CompileMode::Lower || mode == CompileMode::Llvm ||
           mode == CompileMode::Mir || mode == CompileMode::Ssa || mode == CompileMode::Jsir;
}

/// Native executable formats that can be generated.
/// Only applicable on TargetPlatform::Native, and there only to the modes that emit machine code.
enum class ExecutableFormat {
    ELF,   /// Compiles to ELF files (.so for shared libraries) compatible with operating systems such as Linux.
    MachO, /// Compiles to Mach-O files (.dylib for shared libraries) compatible with macOS.
    PE,    /// Compiles to PE/COFF files (.exe, .dll for shared libraries) compatible with Windows.
    Raw,   /// Writes the flat image bytes without an executable-file container.
};

/// System environment that can be targeted.
/// This defines what implementation of the runtime and standard library will be chosen, as well as
/// the platform libraries to link to. `Bare` deliberately names no operating-system ABI: it is the
/// freestanding selection used with a bootloader or another caller that supplies the entry context.
/// Not applicable on TargetPlatform::Js, which has one target and reads none of this.
enum class TargetType {
    Linux,
    MacOS,
    Win32,
    Bare,
};

/// The instruction set to generate code for.
/// This defines the base instruction set to use; extensions can be enabled separately if available.
/// This may also have implications on what runtime functions are chosen.
/// Only applicable on TargetPlatform::Native.
enum class TargetArch {
    X64,   /// AMD64 64-bit instruction set. Implies enabling SSE2 extensions.
    X86,   /// Normal 32-bit instruction set, no extensions implied.
    ARM,   /// Base ARMv6 instruction set, should be supported by most ARM cpus.
    ARM64, /// AArch64 64-bit ARMv8-A instruction set.
};

/*
 * The x86-64 instruction sets this compiler can be told a machine has, one bit each.
 *
 * **Named sets rather than levels**, because the two things that read them want sets: a backend's
 * form requires the extension its encoding actually needs, and the LLVM target machine is handed a
 * list of attributes. A level is a *table* over these bits - `kLevelTable` in settings.cpp - and so is
 * every flag, so a set a level contains and the same set named on the command line are the same bit.
 *
 * The first three groups are what the levels contain and are never named one at a time; the last is
 * the flags of Analysis-Avx512.md §0.3, each of which may be named beside a level and some of which a
 * level also implies. Appending is free; the numbering means nothing outside this process.
 */
using InstructionSets = U64;

enum : InstructionSets {
    // The floor - exactly x86-64-v2.
    kSetSse3      = InstructionSets(1) << 0,
    kSetSsse3     = InstructionSets(1) << 1,
    kSetSse41     = InstructionSets(1) << 2,
    kSetSse42     = InstructionSets(1) << 3,
    kSetPopcnt    = InstructionSets(1) << 4,
    kSetCx16      = InstructionSets(1) << 5,

    // `avx2` - exactly x86-64-v3 on top of the floor.
    kSetAvx       = InstructionSets(1) << 6,
    kSetAvx2      = InstructionSets(1) << 7,
    kSetBmi1      = InstructionSets(1) << 8,
    kSetBmi2      = InstructionSets(1) << 9,
    kSetF16c      = InstructionSets(1) << 10,
    kSetFma       = InstructionSets(1) << 11,
    kSetLzcnt     = InstructionSets(1) << 12,
    kSetMovbe     = InstructionSets(1) << 13,
    kSetXsave     = InstructionSets(1) << 14,

    // `avx512` - the Ice Lake and Zen 4 intersection, which is x86-64-v4 and six more.
    kSetAvx512f         = InstructionSets(1) << 15,
    kSetAvx512cd        = InstructionSets(1) << 16,
    kSetAvx512bw        = InstructionSets(1) << 17,
    kSetAvx512dq        = InstructionSets(1) << 18,
    kSetAvx512vl        = InstructionSets(1) << 19,
    kSetAvx512ifma      = InstructionSets(1) << 20,
    kSetAvx512vbmi      = InstructionSets(1) << 21,
    kSetAvx512vbmi2     = InstructionSets(1) << 22,
    kSetAvx512bitalg    = InstructionSets(1) << 23,
    kSetAvx512vpopcntdq = InstructionSets(1) << 24,
    kSetAvx512vnni      = InstructionSets(1) << 25,

    // The flags - in no level's own list, named or implied. See kFlagTable in settings.cpp.
    kSetAes          = InstructionSets(1) << 32,
    kSetPclmul       = InstructionSets(1) << 33,
    kSetVaes         = InstructionSets(1) << 34,
    kSetVpclmul      = InstructionSets(1) << 35,
    kSetGfni         = InstructionSets(1) << 36,
    kSetSha          = InstructionSets(1) << 37,
    kSetVp2intersect = InstructionSets(1) << 38,
    kSetFp16         = InstructionSets(1) << 39,
    kSetBf16         = InstructionSets(1) << 40,
    kSetSha512       = InstructionSets(1) << 41,
    kSetSm3          = InstructionSets(1) << 42,
    kSetSm4          = InstructionSets(1) << 43,
};

/*
 * Which machine the generated code is for: one of three levels, and the flags beside it.
 *
 * **Three levels rather than a ladder of extensions**, so that a machine is described by something
 * that cannot be inconsistent - `AVX2` without `BMI1`, or `SSE4.2` without `POPCNT`, are not machines.
 * They are this compiler's names rather than the psABI's, for the reasons Analysis-Avx512.md §0.4
 * gives: the lower two are exactly x86-64-v2 and -v3 in what they *contain*, but `avx2` also implies
 * AES-NI and PCLMULQDQ, which the psABI does not, and x86-64-v4 is a set nothing since Cooper Lake has
 * been - so `v4` could not keep its meaning and a ladder of `v2`, `v3` and `avx512` would read as an
 * accident. The psABI spellings are refused rather than aliased, naming the replacement.
 *
 *   sse4    SSSE3, SSE4.1, SSE4.2, POPCNT, CMPXCHG16B - Nehalem and Bulldozer. A vector is 16 bytes.
 *           **The floor and not a choice**: POPCNT, RDTSCP and XSAVE were claimed unconditionally by
 *           the x64 backend long before any of this was named, so the compiler already required it.
 *   avx2    AVX, AVX2, BMI1, BMI2, F16C, FMA, LZCNT, MOVBE, and `aes` and `pclmul` implied - Haswell,
 *           Excavator, every Zen, and every Intel hybrid client part. A vector is 32 bytes.
 *   avx512  AVX-512 F, CD, BW, DQ, VL, IFMA, VBMI, VBMI2, BITALG, VPOPCNTDQ and VNNI, with `vaes`,
 *           `vpclmul`, `gfni` and `sha` implied as well - Ice Lake, Tiger Lake, Rocket Lake, Sapphire
 *           Rapids, Zen 4 and Zen 5. **Not** Skylake-SP/X, Cascade Lake or Cooper Lake, whose
 *           frequency licensing is what made heavy 512-bit code a loss; those parts are `avx2`.
 *
 * The one machine that stops being describable is AVX without AVX2 - Sandy and Ivy Bridge - which
 * the psABI also declines to name. Those parts are `sse4` here, as they are everywhere else.
 */
struct TargetExtensions {
    enum Level: U8 {
        Sse4,
        Avx2,
        Avx512,
    };

    /// The x86-64 level the generated code may use. See above; `sse4` is the floor.
    Level level = Sse4;

    /*
     * The flags named beside the level - `-enable-inst sha` - or detected on the host.
     *
     * **Only what was named.** A flag the level implies is not written here, and asking `has` is what
     * adds the implication: a build that names `aes` at `sse4` and is later raised to `avx2` by a second
     * `-enable-inst` holds the same bit either way, which is §0.3's "a flag the level already implies is
     * accepted silently" and is also what lets the check that refuses a flag below its lowest level run
     * once, after every flag has been read.
     *
     * The hazard of claiming one wrongly is the loud kind for every flag here: each is an opcode that
     * decodes as nothing at all on a part without it, so a wrong claim faults rather than answering
     * differently. That is what makes host detection safe to default to.
     */
    InstructionSets flags = 0;

    /*
     * Every instruction set this target has: the level's own, the flags it implies, and the flags
     * that were named. The one question everything past the command line asks - see `kLevelTable`,
     * which is what answers it.
     */
    InstructionSets sets() const;

    bool has(InstructionSets wanted) const {
        return (sets() & wanted) == wanted;
    }

    /*
     * ARM extensions.
     */

    bool neon = false;   /// Enabled ARM NEON SIMD instructions.
};

/*
 * The names the command line, a file selector and a fixture directive all spell the x86-64 machine
 * with - one table for the three of them, so that a spelling one accepts is a spelling all accept.
 */

// A level by its name, or nothing.
Maybe<TargetExtensions::Level> matchLevel(StringView name);

// A flag by its name, or zero.
InstructionSets matchExtensionFlag(StringView name);

// The level a psABI spelling (`v2`, `x86-64-v3`, `sse4.2`...) would have been, for the message that
// refuses it; nothing for a name that is not one.
Maybe<StringView> replacedLevelSpelling(StringView name);

// The LLVM target attributes a target's instruction sets come to, one name each - read from the same
// table the levels are, so the attribute list cannot come to describe a different machine from the
// one the local backend is told about. `test/bench/findings.md` records the v3 list once being written
// from memory, and wrong.
Array<const char*> llvmAttributesOf(const TargetExtensions& extensions);

// Whether every flag that was named is legal at the level, and the message naming the lowest level a
// flag needs where one is not - §0.3's first rule. Asked once the whole command line has been read.
Maybe<Tritium::String> checkExtensionFlags(const TargetExtensions& extensions);

/*
 * Which code generator a native build goes through.
 *
 * A flag of its own rather than a `CompileMode`, because it answers a different question: a mode
 * says what is produced - an executable, a shared library, LLVM text - and this says who produces
 * it. The same `-mode exe` means the same thing under both, which is what makes switching between
 * them a way to compare two compilers on one program rather than two different builds.
 *
 * The two are not interchangeable. LLVM runs its own optimization pipeline and links through the
 * platform's `cc`, so it produces the faster program and needs a toolchain to be installed; the
 * local backend generates and lays out the machine code itself and writes the executable directly,
 * so it needs nothing on the machine and is much faster to run.
 *
 * **The local one is the default where it exists**, which is x64 ELF for Linux or bare targets and
 * nowhere else - see
 * localBackendSupported. That trades the faster program for the faster build and for a compiler that
 * needs nothing installed beside it, which is the trade a build being run wants far more often than
 * a build being shipped: `-backend llvm` is one flag, and it is the flag a release build passes.
 * Everywhere the local backend does not exist the default is LLVM, which is why this is chosen from
 * the target rather than written down as a value.
 */
enum class NativeBackend {
    Llvm,  /// The optimizing path: LLVM IR, LLVM's pipeline, an object file and the system linker.
    Local, /// The self-contained path: compiler/codegen/x64 and compiler/codegen/elf, and nothing else.
};

/// When a function should establish a frame pointer.
/// A frame pointer costs a push, a move, a pop and one of the general registers for the whole
/// function; what it buys is an address for fixed frame objects that stays valid while the stack
/// pointer moves, and a frame that a debugger or profiler can walk without unwind tables.
enum class FramePointerMode {
    All,     /// Every function gets one, so every stack can be walked.
    NonLeaf, /// Only functions that call something - a leaf's stack frame has nothing above it to walk to.
    Needed,  /// Only functions that cannot address their frame objects through the stack pointer.
};

/*
 * How hard compiler/opt tries to inline, which is the one decision in that stage that trades size
 * for speed rather than being a straight win.
 *
 * Its own knob rather than a reading of `optimization` below, because that one is a level handed to
 * LLVM and says nothing about the IR stage - and because the interesting axis here is the same one
 * `-Os` and `-Ofast` name in Clang. Every other pass in compiler/opt makes a program smaller and
 * faster at once and has nothing to ask about.
 *
 * `Size` is not "off": inlining a callee with exactly one call site in the whole program removes a
 * call, a frame and a body, so it is a size win at every level and is what `Size` still does. It
 * also still honours `@inline`, because that is a statement by the author rather than a guess by the
 * compiler, and this knob is about how far the guessing goes. `None` is the one that ignores
 * everything, which is what makes it the switch to reach for when bisecting a bad inline.
 */
enum class InlineLevel: U8 {
    None,     /// No inlining at all. What a bisection of a miscompile switches off first.
    Size,     /// Only where inlining cannot grow the program: a callee with one call site.
    Balanced, /// The default. Small callees anywhere, larger ones where the call site pays for it.
    Speed,    /// Larger budgets and smaller penalties for a callee called from several places.
};

/*
 * Which shape a multi-way branch is spent in, for a target that has to choose - see
 * `chooseSwitchForm` in codegen/x64/transform_switch.cpp.
 *
 * `Auto` is the thresholds, and is what everything but a measurement wants. The other three force
 * one form on every switch in the program, which is the same kind of knob `-inline` is and exists
 * for the same reason: which form is worth taking is a *measurement*, and a measurement needs the
 * three compiled from one binary over one program. §77 of test/bench/findings.md is what it
 * produced, and it is also how the binary search - which the thresholds never pick - stays exercised.
 */
enum class SwitchForm: U8 {
    Auto,         /// The thresholds decide. The default and the only thing a build should ship with.
    Chain,        /// A comparison per case, in case order.
    BinarySearch, /// A tree of comparisons over the sorted case values.
    JumpTable,    /// A placed table of code offsets, indexed by the selector.
};

/*
 * Which vendor's recent cores a build favours where the two disagree - `-tune`.
 *
 * **Speed only, never correctness.** `-enable-inst` says which instructions exist; this says which of
 * two programs that both run everywhere is picked. A build tuned for one vendor runs on the other,
 * only slower in the places listed below, so it can default to something without host detection and
 * stay reproducible.
 *
 * **A gate exists only where both vendors were measured and disagree.** One that cannot be measured
 * again on either side goes stale without saying so - §55.4 of test/bench/findings.md found a loop
 * refusal that had quietly reversed - so every decision reading this names the measurements it
 * rests on. `Generic` takes the answer that is least bad on both, which so far has always been the
 * one the compiler gave before there was a choice.
 *
 * What reads it today:
 *
 *  - `foldStoreUpdates` in codegen/x64/transform_address.cpp, in a loop. `add [m], r` against its
 *    three instructions was +8% on `Matrix` on a 13900K (§45.2, §55.4) and -8.2% on Zen 5 (§112), so
 *    only `Amd` folds inside a loop.
 *  - The LLVM backend's `tune-cpu`, so that the two backends answer the question for the same part:
 *    Raptor Lake for `Intel`, Zen 5 for `Amd`, and LLVM's generic tuning for `Generic`.
 */
enum class TargetTuning: U8 {
    Generic, /// The default. Whatever is least bad on both vendors.
    Intel,   /// Recent Intel performance cores - Golden Cove and Raptor Cove, the 13900K the corpus was measured on.
    Amd,     /// Recent AMD cores - Zen 4 and Zen 5.
};

// The one provider name the compiler knows, and it knows it only as what `-allocator` starts at -
// see `allocatorSelector`, where the argument for that is written.
static constexpr StringView kDefaultAllocator = "bootstrap"_v;

struct CompileSettings {
    /*
     * The positional arguments, before anything has decided what they are.
     *
     * Separate from `compileObjects` because an input is not necessarily a source: a directory
     * holding a `yana.toml` is a *project*, and which of the two a path is cannot be answered
     * without asking the file system. `resolveInputs` in compiler/project.h is what asks, and it
     * empties this into either `projectFile` or `compileObjects`. `-add` skips the question and
     * says "a source root" outright, which is the only way to say that about a directory that has a
     * project file in it.
     */
    Array<Tritium::String> inputs;

    Array<Tritium::String> compileObjects;

    /*
     * `-main` - the module the program is entered through, or empty to work it out.
     *
     * Named `-main` rather than `-root`, and held apart from `ModuleMap::roots`, because those were
     * two different things wearing one word: a *compile* root is a directory the sources were found
     * under, and this is the one module of them that holds the program's entry. `-root Main` read as
     * a path and was a module name, which is the mistake the spelling now prevents.
     *
     * A list because the flag can be repeated, and repeating it is an error rather than a choice -
     * see findRootModule. Holding them lets that error say how many were named.
     */
    Array<Tritium::String> mainModules;

    Tritium::String outputDir;

    /*
     * `-output` - what the artifact is called, without any extension the mode adds. Empty for the default,
     * which is the root module's name.
     *
     * A name and not a path: it is joined onto `outputDir` like every other artifact, so that where
     * a build writes stays one question with one answer - `-to`. A name with a separator in it is
     * refused rather than silently writing outside the output directory.
     *
     * The default is the root module because that is the one name a program certainly has. It is a
     * poor name for a program whose root module is called `Src` after the directory its files sit
     * in, which is what made this flag worth having: the alternative was renaming a directory to
     * rename a binary.
     */
    Tritium::String outputName;

    /// Whether `-opt` was named, so that a build can be told when nothing will read it - see
    /// `CompileSettings::optimization`, which only the LLVM backend reads.
    bool explicitOptimization = false;

    /*
     * What the command line said for itself.
     *
     * A `yana.toml` fills in the rest - see compiler/project.h - and the settings it can supply that
     * have a usable default are the ones that need this: a mode and a platform are always set to
     * something, and an output directory is always some string, so "unset" is not visible in the
     * value. Everything else the file can say is a list or a name that is empty when nothing said it.
     */
    bool explicitMode = false;
    bool explicitPlatform = false;
    bool explicitResolver = false;
    bool explicitOutput = false;
    bool explicitImageBase = false;

    /// Whether `-target` and `-format` were named, so that a project file's `target` and `format`
    /// fill in only what they did not - and so that the pairing between the two is settled again on
    /// what either named. See settleTargetAndFormat.
    bool explicitTarget = false;
    bool explicitFormat = false;

    /// Whether `-backend` was named. Not for a project file's sake - one cannot set a backend - but
    /// for the difference between a backend that was *chosen* and one that was *defaulted*: naming
    /// one this target has no code generator for is a mistake to report, and defaulting to one is
    /// not, since the default is derived from that same target. See localBackendSupported.
    bool explicitBackend = false;

    /*
     * Where the standard library is, or empty to search for it - see LibrarySource.
     *
     * A path rather than a list, because there is one standard library: a search *path* is what a
     * program's own modules are found through, and this is the one set of modules that is part of
     * the compiler rather than part of the program.
     *
     * Named explicitly it is taken as-is, without the "does it hold a Core/Core.yana" test the searched
     * candidates get. A directory that was pointed at and is wrong should be reported as the
     * directory that was pointed at, rather than silently becoming whichever one came next.
     */
    Tritium::String libraryPath;

    /// Where to look for a `yana.toml`, or empty to look upwards from the working directory.
    /// `noProject` skips the search: a build that has been given every path it needs on the command
    /// line should not change behaviour because of a file in a directory above it.
    Tritium::String projectFile;
    bool noProject = false;

    /*
     * Defaulted rather than left indeterminate: `@platform` reads `mode` on every declaration, so a
     * driver that never set it would select declarations by whatever was on the stack.
     *
     * `target` and `arch` were left indeterminate on the same argument that did not apply to them,
     * and it cost: `applyDefaults` fills them from the host, but only on the path that parses a
     * command line, and a `Context` built directly - which is every test driver - got whatever the
     * allocation happened to hold. It read as X64 for years because a fresh heap tends to read as
     * zero; running the fixture corpus in several processes at once perturbed that, and one fixture
     * with a `.llvm.expect` started reporting "the 32-bit architectures are not supported yet" in
     * one shard and passing in every other arrangement of the same fixtures.
     *
     * The values here are the ones that go together and the ones every driver that does not say
     * otherwise means: a 64-bit ELF executable. `applyDefaults` still replaces them with the host's
     * when a real compilation is being configured.
     */
    CompileMode mode = CompileMode::Executable;
    TargetPlatform platform = TargetPlatform::Native;
    ExecutableFormat format = ExecutableFormat::ELF;
    TargetType target = TargetType::Linux;
    TargetArch arch = TargetArch::X64;
    // The address the first byte of a raw image is loaded at. Raw images are not relocatable;
    // keep the existing native-image default, while allowing a freestanding loader to choose its
    // own address with -image-base.
    U64 imageBase = 0x400000;
    TargetExtensions extensions;

    /*
     * Whether `-extensions` was named, in the same sense `explicitBackend` above is.
     *
     * `applyDefaults` fills `extensions` in from the host's own CPUID when nothing said otherwise,
     * which is a *guess* about where the program will run and not a promise about it. The two
     * backends are entitled to read that guess differently and do: LLVM passes it through as a
     * feature string, where the local backend takes an unnamed extension as unavailable, because a
     * compiler whose output depends on which machine built it has no reproducible goldens and no
     * comparable benchmark numbers. Naming one is the promise; detecting one is not.
     */
    bool explicitExtensions = false;

    /*
     * Which native code generator runs - see NativeBackend. Read only by `-mode exe`; `-mode llvm`
     * and the JavaScript modes have exactly one path each and nothing to choose between, and a
     * value left here for one of them is ignored rather than contradicted.
     *
     * LLVM here rather than the real default, for the reason the four above are what they are: this
     * is the value a driver that configures nothing gets, and the local backend is not a thing every
     * host can produce. `applyDefaults` replaces it with the one the target actually implies, on the
     * path that parses a command line - which is the only path that knows what the target is.
     */
    NativeBackend backend = NativeBackend::Llvm;

    FramePointerMode framePointer = FramePointerMode::Needed;

    /// How hard the optimizing native backend works, 0-3. Only the LLVM path reads it: the local
    /// amd64 backend is the fast one by construction and has no levels to choose between - so on the
    /// platform where that one is the default, `-opt` does nothing until `-backend llvm` is passed
    /// with it, and the driver says so rather than leaving it to be discovered. `optimizeIr` below
    /// is the switch that means something on both, and is `-no-ir-opt` for exactly that reason.
    U32 optimization = 2;

    /// Whether the IR optimizer runs at all - `-no-ir-opt`. Read by all three of them: `compiler/opt`
    /// over the resolve IR, `compiler/resolve_ssa/opt` over ownership SSA, and the shared sequence
    /// over the lower IR that both stages end with (`lower/lower_pipeline.h`), so the flag means one
    /// thing whichever stage compiled the program.
    ///
    /// Distinct from `optimization`, which is a level handed to LLVM: this one is a switch on our
    /// own passes over our own IR, and it is off only to answer the question "did the optimizer
    /// change what this program does". The flag was `-no-opt`, one letter from `-opt 0` and a
    /// different optimizer - the two are now named after the thing each switches. The fixture runner compiles every runnable fixture both ways
    /// and compares the results on both targets, which is the same equivalence check
    /// `Program::Specialization` gets and exists for the same reason - an optimization must not be
    /// where a semantic decision quietly moved to.
    bool optimizeIr = true;

    /// Whether the **JSIR** optimizer runs - `-no-jsir-opt`, read only by `resolve_ssa/js/opt.h`'s
    /// passes and therefore only on `-platform js -resolver ownership`.
    ///
    /// A third axis rather than a reading of `optimizeIr`, because Analysis-IR-JS.md §10.2 asks for
    /// the three configurations separately: shared OSSA optimization on and off, JSIR optimization
    /// on and off. What the pair answers that neither answers alone is which tier a disagreement
    /// came from - a program that differs with `-no-ir-opt` and agrees with this one has a defect
    /// above the fork, and the other way round has one below it.
    bool optimizeJsir = true;

    /// How aggressively compiler/opt inlines - see InlineLevel. Read only when `optimizeIr` is on,
    /// since the whole stage is off otherwise.
    InlineLevel inlining = InlineLevel::Balanced;

    /// Which shape a multi-way branch is spent in - see SwitchForm. A measurement's knob; `Auto` is
    /// the thresholds and is what every build but a probe uses.
    SwitchForm switchForm = SwitchForm::Auto;

    /// Which vendor's recent cores the build favours where they disagree - see TargetTuning.
    TargetTuning tuning = TargetTuning::Generic;

    /*
     * The widest natural vector the build may hand out, in bytes - `-vector-width`, and zero for the
     * level's own. A cap and never a raise: `targetVectorBytes` takes the smaller of the two, so it can
     * hold `avx512` at 256 bits and cannot ask `sse4` for more than it has.
     *
     * **Speed only, like `-tune`**, and for the same reason it exists: Analysis-Avx512.md §1.2 leaves
     * the 512-bit width a measurement rather than a certainty, and a program written against `Vec(a)`
     * means the same thing at every width - so this is two builds of one program that both run, and
     * the pair is how test/bench/avx512/findings.md §10 measured one against the other.
     */
    U32 vectorWidthLimit = 0;

    /*
     * The checks a program stops on - a subscript's bounds test, and the range test on a store
     * through a `@bits` refinement.
     *
     * On by default, which is the decision rather than the switch: an index nothing checked reads
     * memory that belongs to something else, and a `@bits(13)` field holding a twenty-bit value
     * falsifies the niche above its range, so a `Maybe` folded into that niche starts reading one
     * constructor as another. Neither is a mistake a program can be trusted not to make, and neither
     * has a symptom at the point it happens.
     *
     * `-no-checks` turns them off wholesale, for the build that has measured what they cost and
     * decided. There is deliberately no per-check switch: what a reader needs to know about a binary
     * is whether it is the checked one.
     *
     * **What a check *is* differs between the two resolve stages, and the switch means the same
     * thing either way.** `compiler/resolve` emits `checkCondition(index >= length)` at the
     * subscript and `opt/opt_check.cpp` removes the calls it emitted. The ownership stage emits
     * nothing at all: `lib/Core/Array.yana`'s `Index.get` writes the test as a library `if` and
     * `@inline(always)` grafts it into the caller, so what `resolve_ssa/opt/check.cpp` removes is a
     * branch whose whole taken arm calls the `checkFailed` role. Both are read in the optimizer and
     * above its own gate, so a resolved program is the same either way - a library built checked
     * links into an unchecked one - and `-no-checks` is free rather than cheap in both.
     *
     * **Under the ownership stage it takes the library's other stops with it**, and that is the one
     * place the two switches do not remove the same set. What is recognized there is the role and
     * not the site, so every `checkFailed` the library reached for goes:
     * `lib/Core/Array.yana`'s `reserve` writes `if !resize(self.run, wide) then checkFailed()`,
     * so an unchecked build has no allocation-failure stop and a `push` that could not get the room
     * carries on into storage it does not have. That is the honest consequence of a library whose
     * checks are library code rather than a hole in the rule - recognizing the *condition* instead
     * would miss the test the day the library spells it differently - and it is stated here rather
     * than only in `resolve_ssa/opt/check.cpp`, because it is a fact about the binary and not about
     * the pass. A program that relies on that stop is one that keeps the checks on.
     */
    bool checks = true;

    /*
     * Whether an inserted check says *where* it was - `-check-locations`.
     *
     * A check that does not hold stops the process and says nothing at all today, which is the whole
     * of what a bounds failure tells you: status 134, and a core file if you kept one. With this,
     * every check site carries its module, line and column, and `checkFailed` prints them before it
     * stops.
     *
     * A switch and not the default, because it is paid for per *site*: three constants have to be
     * materialized on the way to a call that used to be a bare `jmp`, so every subscript in the
     * program grows some cold code, and the printing pulls the formatting path into a binary that
     * may never print. Off is what ships and on is what a debug build wants - see the measurement in
     * Design-Test.md's staging note.
     */
    bool checkLocations = false;

    /*
     * Whether a call may reach a **replacement** at all - `-no-replacements`, Design-Replacements.md
     * §7.
     *
     * Every replacement is still read and resolved wherever its selectors admit it; this only chooses
     * none, so every call runs its baseline. It exists because a replacement is held to being
     * observationally identical to its baseline and no compiler can prove that: running one suite
     * both ways and asking the answers to agree is the check, and it is cheaper than a build per
     * target because it compares the two bodies on the machine the suite runs on.
     */
    bool noReplacements = false;

    /*
     * Whether this is a test build - Design-Test.md §11.2's F1.
     *
     * A flag and not a mode, because `-mode` already answers *which output* and a test build needs
     * both: `-test -mode exe` is a native test binary and `-test -mode js` is the same suite on
     * JavaScript. Three things turn on it, and each of them is a rule that already existed pointed
     * at a new answer:
     *
     *  - `.test.yana` files are selected, on the file-selector rule §3.1 gives. One honest widening
     *    goes with it: the vocabulary becomes "what this compilation is" rather than "which machine
     *    it is for". `test` excludes nothing else, unlike an architecture or a level.
     *  - every `@test` declaration is a reachability root, which without this would be dead in `exe`
     *    mode where the entry is the only root;
     *  - the entry is synthesized from them, exactly as `resolveProgramEntry` already synthesizes one
     *    for a root module's top-level statements. A user-written `main` is not the entry and is not
     *    a root: a test build runs tests.
     *
     * A `@test` in a build without it is a diagnostic naming this flag rather than a declaration
     * silently dropped - a test that is not built is the failure mode the whole document is about.
     */
    bool test = false;

    /*
     * The package this compilation is - `[package] name` from its own `yana.toml`, or `-package`.
     *
     * Empty for a program that has not said, which is every one that only consumes packages. What
     * reads it is the *dependency* side of two questions that used to have one global answer:
     *
     *  - **whose tests run.** `.test.yana` selection is answered per walk now, not per compilation:
     *    the project's own sources honour `-test`, and a package reached as a dependency never
     *    does - unless it is this package, which is how the standard library tests itself. Before
     *    this, a `.test.yana` file under `lib/` joined *every* consumer's suite, ran its cases, and
     *    since F5 ran its initializers too.
     *  - **what may be imported.** A dependency's module is importable only if that package's
     *    manifest exports it - see `ProjectFile::exports` and `moduleIsExported`.
     *
     * Compared by name against the library's own manifest rather than by path, because that is the
     * fact a package has: two trees at different paths that both call themselves `base` are a
     * configuration mistake, and one tree reached by two paths is not.
     */
    Tritium::String package;

    /*
     * Which general heap this program is built against - Design-Allocator.md §18.4.
     *
     * An **opaque string**, compared against what an `@allocator` attribute or a file-name segment
     * wrote and against nothing else. The compiler knows one name, the default below, and knows it
     * only as the value this field starts at; every other provider is a declaration in the library,
     * and a name nothing declares is reported by the roles - a `Required` role claimed zero times -
     * rather than by a table here that would have to be edited alongside the library.
     *
     * Empty is not a state: a program has a heap, and the day one does not is a flag that says so
     * rather than a name that happens to match nothing.
     */
    Tritium::String allocator = Tritium::String(kDefaultAllocator.ptr, kDefaultAllocator.length);

    /*
     * Whether generic functions are specialized per instantiation, or emitted once and shared -
     * Program::Specialization.
     *
     * A flag because it is a row of the test matrix: §8's `agree` asks whether a library's answer
     * depends on how it was compiled, and "specialized" against "forced generic" is one of the pairs
     * it asks about. It was reachable only from inside a C++ driver, which is exactly what stopped
     * the standard library's suite from being an ordinary program.
     */
    bool forceGeneric = false;

    /// `-help`: print what this compiler takes and stop. Not an error, and not a compilation - the
    /// driver prints `helpText` and answers zero.
    bool help = false;

    /// Debug flag: print a list of the modules found in the input, and where each came from.
    /// The one dump that is not a `-mode`: it is about how the files were grouped rather than about
    /// the program they form, so it is a thing to ask *while* compiling rather than instead of it.
    bool printModules = false;

    /*
     * Whether the ownership passes keep each local's live ranges - OwnershipResult::ranges.
     *
     * Off, because `printOwnership` is the only thing in the compiler that reads one and nothing on
     * a compilation's path to an executable calls it - `-mode drops` is what turns both on, and
     * is the only thing on a command line that does. Building them is not free: the range builder
     * replays the backward walk *per local per block* to recover liveness at each point inside a
     * block, which the fixpoint only kept at the two ends - so it is the single largest item in the
     * ownership stage, and on a small program it was 4% of the whole compile.
     *
     * A driver that prints the dump asks for them, which today is the resolve fixture runner. The
     * facts every other decision is made from - the lattice, the summaries, where a drop goes - are
     * computed either way; this is the *presentation* of liveness and only that.
     */
    bool ownershipRanges = false;

    /// `-resolver legacy|ownership`, and implied by `-mode mir`, `-mode ssa` and `-mode jsir`. The
    /// rewrite since 2026-09-22 (J81) - see ResolverStage for why the old stage is still reachable.
    ResolverStage resolver = ResolverStage::Ownership;

    /*
     * The `explain` query - Analysis-Ambient.md §7.3.
     *
     * A query rather than a compilation mode: it stops after resolution and the ownership passes,
     * which is everything it reads, and emits nothing. It is not a `CompileMode` because those
     * choose an *output format* and this one has no output to format - and because a mode would
     * make `-mode exe -explain f` a contradiction rather than a sensible thing to ask.
     *
     * `@platform` still selects which declarations exist, so the answer is the answer *for the
     * target the other flags name*. A JS build and a native build genuinely have different programs
     * to explain, and this reports whichever one was asked for.
     */
    /// The function to explain, or empty for no query, and the module it was qualified with - see
    /// `setExplainTarget`, which splits `Server.handle` into the two. Empty module means every
    /// module, which is what an unqualified name asks for.
    Tritium::String explainName;
    Tritium::String explainModule;

    /// Explain every function in the program instead of one - the report Analysis-Ambient.md §7.5's
    /// capability audit is a filter over.
    bool explainAll = false;

    bool explaining() const { return explainAll || explainName != ""; }
};

inline bool isJsMode(const CompileSettings& settings) {
    return settings.platform == TargetPlatform::Js;
}

/*
 * How wide one vector is, in bytes - Design-Vector §2.3, and the one number the natural form
 * `Vec(a)` is spent against: the lane count is this divided by the element's stride.
 *
 * A setting derived from the target rather than an annotation, because a program that could ask for
 * a different width per declaration would make `Vec(Float)` mean two things in one program and every
 * call between them a conversion. What a program can name is a fixed count, which is `Vec(a, n)`.
 *
 * **The extensions are read only where they were named**, which is `x64FeaturesFor`'s rule and is
 * here for the stronger version of its reason. `applyDefaults` fills the extension set in from the
 * host's own CPUID when nothing said otherwise, so reading it unconditionally would make the *type*
 * `Vec(Float)` - its lane count, its Repr, the answers every `.expect` file records - a function of
 * which machine ran the compiler. A guess about where a program will run may decide which form of an
 * instruction is selected; it may not decide what the program's types are.
 *
 * 256 bits needs AVX2 rather than AVX: AVX widens the float operations only, and a `Vec(I32)` that
 * had to be split for want of the integer half would not be one vector.
 */
/*
 * The widest vector any target this compiler describes has, which is a 512-bit register, and the
 * most lanes one may have, which is that register full of bytes.
 *
 * Both, because they are two bounds rather than one seen twice: the byte width is what a register
 * has to hold and what the ABI contract is checked against, and the count is what every walk over
 * lanes is written in terms of - a shuffle pattern has one entry per lane, and a reduction is a tree
 * log2 of it deep. `Vec(I64, 64)` violates the first while satisfying the second.
 *
 * These are the *language's* ceiling and not the running target's, which is what `targetVectorBytes`
 * below answers. A vector wider than the target being compiled for is a value that target has no
 * register for, and the backend that cannot hold one is the one that refuses it - which is where the
 * refusal can name the register. The two live together because a reader of either wants both, and
 * because the tail-read guarantee is stated in terms of the ceiling: what has to be reserved after a
 * heap region, a stack frame or a data segment is the widest read *any* build of this program could
 * make, not the one this build makes.
 */
/*
 * The widths a target chooses for the three integers the language leaves abstract - `Size`, `USize`
 * and `CodeUnit`. See TargetInt in resolve/type.h, which is what states the bound they answer.
 *
 * Here beside `targetVectorBytes` rather than with the types, because it is the same kind of fact
 * and has the same two readers: `ReprTarget::integers` carries it to everything downstream of
 * resolve, and the vector lane stride reads it directly - a lane width is target-dependent already
 * (Design-Vector §2.1), and that is the one fork Move 2 deliberately does not close.
 *
 * The defaults are the 64-bit native answers, so a target that forgets to state them behaves as the
 * only native machine there is.
 */
struct IntWidths {
    U16 word = 64;
    U16 codeUnit = 8;
};

/*
 * What an address costs on the machine being compiled for, in bytes - the one number every other
 * target-dependent width is derived from.
 *
 * Read off `arch` and nowhere else, because this used to be a constant: both native layouts put a
 * pointer at eight bytes and `targetIntWidths` below made every native `Size` sixty-four wide, none
 * of which read the architecture, so a build for x86 or 32-bit ARM was accepted and handed LLVM a
 * program with sixteen-byte windows and eight-byte pointer loads for a machine whose pointers are
 * four. A JavaScript build has no addresses and never asks; a native one asks here.
 */
inline U32 targetAddressBytes(const CompileSettings& settings) {
    switch(settings.arch) {
        case TargetArch::X86:
        case TargetArch::ARM:
            return 4;
        case TargetArch::X64:
        case TargetArch::ARM64:
            return 8;
    }

    return 8;
}

inline IntWidths targetIntWidths(const CompileSettings& settings) {
    // A signed 32-bit index, because a host array's length is a `uint32` by specification and
    // nothing wider can be described anyway; and a UTF-16 unit, because that is what a host string
    // is made of. Natively the word is the address width and a unit is a UTF-8 byte.
    if(isJsMode(settings)) return IntWidths { 32, 16 };
    return IntWidths { U16(targetAddressBytes(settings) * 8), 8 };
}

constexpr U32 kMaxVectorBytes = 64;
constexpr U32 kMaxVectorLanes = kMaxVectorBytes;

inline U32 targetVectorBytes(const CompileSettings& settings) {
    // JavaScript's 16 is a decision rather than an observation - Design-Vector §8. There is no
    // register to read it off, and four lanes is what the scalarized form stays cheap at.
    if(isJsMode(settings)) return 16;

    /*
     * The level, always - where this used to ask `explicitExtensions` first, so that a lane count
     * could never be a function of the machine that ran the compiler.
     *
     * That guard is gone because the *default* is no longer "assume nothing": a build that names no
     * level gets the one the host meets, which is `-march=native`'s bargain - the code fits the
     * machine in front of you, and a build that has to be reproducible says which machine it is for.
     * `-enable-inst sse4` is that statement, and every fixture in the test suite makes it.
     *
     * `avx512` answers 64, which is Analysis-Avx512.md §1.2's stage 2: its stage 1 held it at 32 while
     * every instruction the level brings was built at the width `avx2` already has. The overread
     * guarantee was always stated at `kMaxVectorBytes`, so nothing about storage changed with it.
     */
    auto natural = U32(16);

    if(settings.arch == TargetArch::X64 && settings.extensions.level >= TargetExtensions::Avx512) {
        natural = 64;
    } else if(settings.arch == TargetArch::X64 && settings.extensions.level >= TargetExtensions::Avx2) {
        natural = 32;
    }

    // `-vector-width`, which only ever narrows - see CompileSettings::vectorWidthLimit. It is never
    // below sixteen, which the flag has no spelling for.
    if(settings.vectorWidthLimit && settings.vectorWidthLimit < natural) natural = settings.vectorWidthLimit;

    // SSE2 on amd64, NEON on ARM, and the width every target this compiler emits for has at least.
    return natural;
}

/*
 * The smallest unit the target's kernel maps memory in - the number a mapping is rounded up to, and
 * the guard `lib/Native/Heap.native.yana` puts after a region.
 *
 * **A lower bound rather than the exact page**, and every use of it has to stay correct under that
 * reading. On x86-64 Linux 4096 is the psABI's and there is nothing to be a bound about; on arm64 a
 * kernel is built for 4K, 16K or 64K pages and the value is not knowable until the program runs. The
 * uses the library has are both safe under a bound: rounding a mapping up to less than the kernel's
 * page leaves the kernel to round it again, and mapping one extra page as a tail guard gets at least
 * that much slack whichever page the kernel is using.
 *
 * The exact page is a *runtime* fact and the kernel already hands it to a fresh process, in the
 * auxiliary vector the entry stub walks past on its way to the environment. A library that needs the
 * real number - one that wants to `mprotect` a guard, or to report it - should read it there rather
 * than widen this. See `pageBytes` in compiler/intrinsic/supplied.def.
 *
 * Not the same number as an executable's segment alignment, which is what `kElfPageSize` is: that
 * one is a promise made to a *loader* about the file, and the two are only equal by convention on
 * x86-64. They are deliberately not written in terms of each other.
 */
inline U32 targetPageBytes(const CompileSettings& settings) {
    // A JavaScript program maps nothing and has no answer to give.
    if(isJsMode(settings)) return 0;

    // 16K on Apple silicon, which is the one target here whose page is fixed and is not 4096.
    if(settings.arch == TargetArch::ARM64 && settings.target == TargetType::MacOS) return 16384;

    return 4096;
}

/*
 * Whether the local backend can produce an executable for the target these settings name.
 *
 * One statement of the list, read twice and for two different questions: `applyDefaults` asks it to
 * decide what an unnamed backend should be, and `checkSettings` asks it to decide whether a named
 * one is a mistake. Written apart from both so that the answers cannot drift - a target that gained
 * a code generator and only half of them heard about it would default to the local backend on a
 * machine that then refused to use it, or the reverse.
 *
 * The mode is deliberately not part of it. A backend is a fact about the target and a mode is a
 * question about the output, and mixing them would make the default depend on a setting a project
 * file can still change after this has been asked.
 */
inline bool localBackendSupported(const CompileSettings& settings) {
    if(settings.arch != TargetArch::X64) return false;

    if(settings.format == ExecutableFormat::Raw) {
        return settings.target == TargetType::Linux || settings.target == TargetType::Bare;
    }

    return settings.format == ExecutableFormat::ELF &&
           (settings.target == TargetType::Linux || settings.target == TargetType::Bare);
}

/// Parses the provided command line into a set of compiler options.
/// If invalid arguments are provided, returns a human-readable error string.
///
/// What is *missing* is not checked here, because a `yana.toml` may still supply it - see
/// checkSettings, which the driver calls once the project file has been applied.
Result<CompileSettings, Tritium::String> parseCommandLine(const char** argv, Size argc);

/// Defaults the operating system and executable format that neither `-target`/`-format` nor a
/// project file named, pairs each with the other where only one was, and derives the backend from
/// the result. The command line runs it; `applyProjectFile` runs it again on what the file added.
void settleTargetAndFormat(CompileSettings& settings, bool hasTarget, bool hasFormat);

/// The value a `-target` or `-format` word names, or nothing. For `yana.toml`, which spells the two
/// keys with the flags' words.
Maybe<TargetType> targetNamed(const Tritium::String& name);
Maybe<ExecutableFormat> formatNamed(const Tritium::String& name);

/// The usage text `-help` prints. See its definition for why it is written rather than generated.
StringView helpText();

/// Checks that everything a compile needs was named by something - the command line, or the project
/// file applied on top of it. Separate from parseCommandLine for that reason and no other.
Result<void, Tritium::String> checkSettings(const CompileSettings& settings);

/// The name `-arch` uses for an architecture. Exposed so that a diagnostic about a target names it
/// the way the author would have written it.
StringView archName(TargetArch arch);

/// The name `-target` uses for an operating system, for the same reason.
StringView targetName(TargetType target);

/*
 * A file-name selector - Analysis-Modules.md §2.5.
 *
 * A file may name the targets it is for in its own name: `Array.native.yana` is compiled on a
 * native build and absent from a JS one, and `Linux.linux.x64.yana` is compiled only for Linux on x86-64. Every
 * dotted segment after the first is one selector, and a file is part of its module when this
 * compilation satisfies all of them - so `Foo.native.linux.yana` reads as "and", the way two
 * `@platform` attributes on one declaration do.
 *
 * **Three axes and not one**, which §4.2.1 is the argument for: `lib/Native/Linux.yana` is the
 * x86-64 syscall table and has no way to say so today, because architecture is expressed only by
 * being imported or not. Platform, system environment and architecture are all selectors here, drawn
 * from the spellings `-target`, `-arch` and `@platform` already use, so nothing has a second name.
 *
 * `Unknown` is a name that is not a selector at all, and it is reported rather than ignored: a file
 * name is not otherwise dotted, so `Array.helpers.yana` is a typo that would silently be compiled
 * into every target under a name nothing can import.
 */
enum class TargetSelector: U8 {
    Unknown,  /// Not the name of any target this compiler knows.
    Matched,  /// A selector this compilation satisfies.
    Excluded, /// A selector naming some other target.
};

/*
 * Which side of a package boundary is asking - Design-Test.md §3.1, and ProjectFile::exports.
 *
 * `test` is the only selector that differs between the two, and it differs completely: it names what
 * a *compilation* is rather than what a machine has, so "is this a test build" is the right question
 * for the tree being compiled and the wrong one for a package it merely depends on. Everything else
 * here is a fact about the target and is the same answer whoever asks.
 */
enum class SelectorScope {
    Project,    /// A file of the tree this compilation is - `-add`, or a `yana.toml`'s sources.
    Dependency, /// A file of a package reached by import. Never a test file of one.
};

TargetSelector targetSelector(const CompileSettings& settings, StringView name,
                              SelectorScope scope = SelectorScope::Project);

/*
 * The **allocator** axis - Design-Allocator.md §18.3 and §18.4.
 *
 * The same shape of answer as `targetSelector` and deliberately not the same function. That one
 * answers "does this machine have this", and its `SelectorScope` exists because the one selector
 * that names a *mode* rather than a machine - `test` - needed different answers for a project and a
 * dependency. An allocator needs the *same* answer for both: a program has one general heap, and a
 * dependency compiled against a different one would be a program with two. Folding it in would put a
 * third behaviour inside a two-behaviour function.
 *
 * **The compiler knows no provider names.** The flag's value is an opaque string compared against
 * what a declaration or a file name wrote, so a provider added to the library needs no compiler
 * change and a name nothing declares is caught by the roles rather than by a table here: after
 * selection, `known.def`'s "a `Required` role is claimed exactly once" says precisely *this program
 * has exactly one general heap*. One default constant is this function's whole knowledge.
 *
 * Selection **deletes** the providers it does not pick, rather than picking between claims after
 * they are all read. That is §18's requirement that unused provider code be absent, and §23's that a
 * half-finished provider must not have to compile in every build - both of which a role-level pick
 * fails. So it belongs where `@platform` is, which is the one place a declaration is not read at all.
 */
TargetSelector allocatorSelector(const CompileSettings& settings, StringView name);

/*
 * The **resolve-stage** axis: `legacy` and `ownership` in a file name, e.g. `Text.legacy.yana`.
 *
 * It exists for one transition and should go when `compiler/resolve` does (Analysis-IR.md 11.5). The
 * two stages read one `lib/`, and a few library operations cannot have one signature both accept.
 * `String.slice` is the first of them: the rewrite hands back a borrowed window of the string, and the
 * old stage, whose shared `String` is the owner's address, has no way to represent a partial window
 * and copies instead (Implementation-String.md part 4). The two bodies then differ in their
 * *signatures*, so choosing between them after reading both is not an option; the file that is not
 * this stage's is not read at all, exactly as a `.js.yana` file is not read by a native build.
 *
 * A file-name selector only, not a `@platform` target: it is a fact about the compiler rather than
 * the machine, and keeping it off the declaration attribute keeps it out of every program that is not
 * the standard library's. Deleting the old stage is then deleting every `*.legacy.yana` file and
 * folding each `*.ownership.yana` into its module's main file.
 */
TargetSelector resolverSelector(const CompileSettings& settings, StringView name);

/*
 * The marker segment that hands a file name's next segment to the allocator axis -
 * `Heap.native.allocator.bootstrap.yana`, Design-Allocator.md §18.3.
 *
 * A marker rather than "any segment no machine answers to", which is what this replaced and what did
 * not work. `allocatorSelector` never answers `Unknown` - every name is one a provider may be called
 * - so letting it answer for an unrecognized segment makes *every* unrecognized segment a provider
 * name, and two things follow that neither axis wants: a project's `Data.Helpers.yana` is excluded
 * from its own build because `Helpers` is not the chosen allocator, and a library file with a
 * mistyped selector is silently dropped instead of reported, because the branch that reported it
 * can no longer be reached.
 *
 * One segment the compiler knows, and the name after it still open - which is what §18.4 asks for.
 * It is the *axis* that is closed here, exactly as `-allocator` is a flag the compiler knows whose
 * value it does not.
 */
static constexpr StringView kAllocatorMarker = "allocator"_v;

/*
 * Whether this compilation selects a file by its dotted name - Analysis-Modules.md §2.5,
 * Design-Test.md §3.1's `test`, and Design-Allocator.md §18.3's provider.
 *
 * One walk for the library and the project, which read the same name and differ in exactly one
 * thing: what an unrecognized segment means. A library file name is not otherwise dotted, so a
 * segment that is not a selector is a mistake and is reported; a project's may well be dotted, and
 * turning every existing `Data.Helpers.yana` into a diagnostic is a change to what compiles. So the
 * *answer* is here and the *policy* is the caller's - `unknown` is the first segment that named
 * nothing, and whether that is an error is decided where the file's path is in hand to say so.
 *
 * Returns false when a segment excluded the file, which is independent of `unknown`: a name may both
 * be excluded and contain a typo, and the caller that reports wants to report either way.
 */
bool selectsFileName(const CompileSettings& settings, StringView stem, SelectorScope scope,
                     bool& isTest, StringView& unknown);

/*
 * What a declaration's selectors say, as a formula - Design-Replacements.md §3.5.
 *
 * `targetSelector` answers one name for *this* compilation. A replacement needs a different question
 * answered, and needs it answered for every compilation at once: does one declaration's condition
 * imply another's, and can two ever hold together. Both are facts about the vocabulary rather than
 * about a machine - `avx2` is `v3`, a level implies `x64`, `js` and `native` never meet - so they are
 * decided here, beside the one place the vocabulary is written, and neither resolve stage keeps a
 * table of its own that could come to disagree.
 *
 * The shape is the one the spellings already have. One `@platform(a, b)` is a clause read as **or**;
 * the attribute written twice, and every dotted segment of the file name, are further clauses read
 * as **and**. So a condition is a conjunction of disjunctions, and each literal is one written name
 * on one axis - the machine axis `targetSelector` answers, the allocator axis `@allocator` and
 * `allocator.<provider>` name, or the resolve-stage axis `legacy`/`ownership` name.
 *
 * The names are views of interned text, which lives as long as the compilation does.
 */
enum class SelectorAxis: U8 {
    Target,
    Allocator,
    Resolver,
};

struct SelectorLiteral {
    StringView name;
    SelectorAxis axis = SelectorAxis::Target;
    U16 clause = 0;
};

struct SelectorCondition {
    // Every literal of every clause, in clause order. Flat rather than a list of lists: a condition
    // is a handful of names and is built once per replacement.
    Array<SelectorLiteral> literals;
    U16 clauses = 0;

    // Opens a clause - one `@platform(...)`, or one file-name segment - and answers its index.
    U16 addClause() { return clauses++; }
    void add(U16 clause, StringView name, SelectorAxis axis) {
        literals.push(SelectorLiteral { name, axis, clause });
    }

    bool isEmpty() const { return clauses == 0; }
};

/*
 * The selectors of one file name, appended to `into` as one clause per segment - `selectsFileName`'s
 * walk, read as a formula rather than answered. `stem` is the dotted part after the base name
 * (`sha.avx2` for `ShaBlock.sha.avx2.yana`); the `test` segment is left out, because what it selects
 * is a *mode* and says nothing about which of two bodies is faster.
 */
void appendFileSelectors(SelectorCondition& into, StringView stem);

/*
 * Whether every compilation that satisfies `p` also satisfies `q`, and whether no compilation
 * satisfies both - checked against the vocabulary, for every target at once.
 *
 * Sound and deliberately not complete: a pair this cannot prove ordered or disjoint is reported as
 * ambiguous, and the fix it names - add one condition to the other - always makes it provable.
 */
bool selectorsImply(const SelectorCondition& p, const SelectorCondition& q);
bool selectorsDisjoint(const SelectorCondition& p, const SelectorCondition& q);
