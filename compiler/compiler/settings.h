#pragma once

#include <Core.h>

/// The main available compilation modes that define the type of output generated.
enum class CompileMode {
    Library,          /// Compiles into a platform-independent Yana library that can be included in other programs.
    NativeExecutable, /// Compiles into a native executable program.
    NativeShared,     /// Compiles into a native shared library for dynamic linking (.dll, .so, .dylib).
    JsExecutable,     /// Compiles into an executable javascript file, including all dependencies.
    JsLibrary,        /// Compiles into a javascript file as a library that can be included in a larger project.
    Llvm,             /// Compiles into a native program, but outputs LLVM IR files rather than object files.
};

/// Whether a compilation mode emits JavaScript rather than machine code.
/// This is what `@platform(js)` and `@platform(native)` select on: the two targets differ in which
/// declarations exist at all, because the host-shaped implementations of `String`, `Array`, `Map`
/// and `Storage` are separate declarations rather than separate representations of one.
/// See Analysis-JS.md §2.4.
inline bool isJsMode(CompileMode mode) {
    return mode == CompileMode::JsExecutable || mode == CompileMode::JsLibrary;
}

/// Native executable formats that can be generated.
/// Only applicable to CompileMode::NativeExecutable and CompileMode::NativeShared.
enum class ExecutableFormat {
    ELF,   /// Compiles to ELF files (.so for shared libraries) compatible with operating systems such as Linux.
    MachO, /// Compiles to Mach-O files (.dylib for shared libraries) compatible with macOS.
    PE,    /// Compiles to PE/COFF files (.exe, .dll for shared libraries) compatible with Windows.
};

/// Operating system that can be targeted.
/// This defines what implementation of the runtime and standard library will be chosen,
/// as well as the platform libraries to link to.
/// Not applicable to JS compilation modes, as they only support one target.
enum class TargetType {
    Linux,
    MacOS,
    Win32,
};

/// The instruction set to generate code for.
/// This defines the base instruction set to use; extensions can be enabled separately if available.
/// This may also have implications on what runtime functions are chosen.
/// Only applicable to CompileMode::NativeExecutable and CompileMode::NativeShared.
enum class TargetArch {
    X64,   /// AMD64 64-bit instruction set. Implies enabling SSE2 extensions.
    X86,   /// Normal 32-bit instruction set, no extensions implied.
    ARM,   /// Base ARMv6 instruction set, should be supported by most ARM cpus.
    ARM64, /// AArch64 64-bit ARMv8-A instruction set.
};

/// Defines optional instruction set extensions that can be enabled.
/// Some extensions are defines as enums of related sets;
/// in those cases, enabling a high set implies enabling the ones before it.
struct TargetExtensions {
    enum SSEMode {
        NoSSE,
        SSE,
        SSE2,
        SSE3,
        SSSE3,
        SSE4_1,
        SSE4_2,
        AVX,
        AVX2,
        AVX512,
    };

    /*
     * x86 extensions.
     */

    SSEMode sse = NoSSE; /// The highest SSE/AVX instruction set to support.
    bool popcnt = false; /// Enable the popcnt instruction (separate from SSE due to differences in supported cpus).
    bool lzcnt = false;  /// Enable the lzcnt instruction (separate from SSE due to differences in supported cpus).
    bool fma3 = false;   /// Enable the FMA3 instruction set.

    /*
     * ARM extensions.
     */

    bool neon = false;   /// Enabled ARM NEON SIMD instructions.
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

struct CompileSettings {
    Array<Tritium::String> compileObjects;
    Array<Tritium::String> rootObjects;
    Tritium::String outputDir;

    /*
     * What the command line said for itself.
     *
     * A `yana.toml` fills in the rest - see compiler/project.h - and the two settings it can supply
     * that have a usable default are the two that need this: a mode is always set to something, and
     * an output directory is always some string, so "unset" is not visible in the value. Everything
     * else the file can say is a list or a name that is empty when nothing said it.
     */
    bool explicitMode = false;
    bool explicitOutput = false;

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
    CompileMode mode = CompileMode::NativeExecutable;
    ExecutableFormat format = ExecutableFormat::ELF;
    TargetType target = TargetType::Linux;
    TargetArch arch = TargetArch::X64;
    TargetExtensions extensions;

    FramePointerMode framePointer = FramePointerMode::Needed;

    /// How hard the optimizing native backend works, 0-3. Only the LLVM path reads it: the local
    /// amd64 backend is the fast one by construction and has no levels to choose between.
    U32 optimization = 2;

    /// Whether the IR optimizer (compiler/opt) runs at all.
    ///
    /// Distinct from `optimization`, which is a level handed to LLVM: this one is a switch on our
    /// own passes over the resolve IR, and it is off only to answer the question "did the optimizer
    /// change what this program does". The fixture runner compiles every runnable fixture both ways
    /// and compares the results on both targets, which is the same equivalence check
    /// `Program::Specialization` gets and exists for the same reason - an optimization must not be
    /// where a semantic decision quietly moved to.
    bool optimizeIr = true;

    /// How aggressively compiler/opt inlines - see InlineLevel. Read only when `optimizeIr` is on,
    /// since the whole stage is off otherwise.
    InlineLevel inlining = InlineLevel::Balanced;

    /*
     * The checks the compiler inserts into the program - a subscript's bounds test, and the range
     * test on a store through a `@bits` refinement.
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
     */
    bool checks = true;

    bool printModules = false; /// Debug flag: Print a list of modules found in the input.
    bool printAst = false;     /// Debug flag: Create .ast files for each source file.
    bool printIr = false;      /// Debug flag: Create .ir files for each source file.

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
    Tritium::String explainName;   /// The function to explain, or empty for no query.
    Tritium::String explainModule; /// Restrict the query to one module, or empty for every module.

    /// Explain every function in the program instead of one - the report Analysis-Ambient.md §7.5's
    /// capability audit is a filter over.
    bool explainAll = false;

    bool explaining() const { return explainAll || explainName != ""; }
};

/// Parses the provided command line into a set of compiler options.
/// If invalid arguments are provided, returns a human-readable error string.
///
/// What is *missing* is not checked here, because a `yana.toml` may still supply it - see
/// checkSettings, which the driver calls once the project file has been applied.
Result<CompileSettings, Tritium::String> parseCommandLine(const char** argv, Size argc);

/// Checks that everything a compile needs was named by something - the command line, or the project
/// file applied on top of it. Separate from parseCommandLine for that reason and no other.
Result<void, Tritium::String> checkSettings(const CompileSettings& settings);

/// The name `-arch` uses for an architecture. Exposed so that a diagnostic about a target names it
/// the way the author would have written it.
StringView archName(TargetArch arch);
