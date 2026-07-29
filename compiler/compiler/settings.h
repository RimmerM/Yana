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

struct CompileSettings {
    Array<Tritium::String> compileObjects;
    Array<Tritium::String> rootObjects;
    Tritium::String outputDir;

    // Defaulted rather than left indeterminate: `@platform` reads it on every declaration, so a
    // driver that never set it would select declarations by whatever was on the stack.
    CompileMode mode = CompileMode::NativeExecutable;
    ExecutableFormat format = ExecutableFormat::ELF;
    TargetType target;
    TargetArch arch;
    TargetExtensions extensions;

    FramePointerMode framePointer = FramePointerMode::Needed;

    /// How hard the optimizing native backend works, 0-3. Only the LLVM path reads it: the local
    /// amd64 backend is the fast one by construction and has no levels to choose between.
    U32 optimization = 2;

    bool printModules = false; /// Debug flag: Print a list of modules found in the input.
    bool printAst = false;     /// Debug flag: Create .ast files for each source file.
    bool printIr = false;      /// Debug flag: Create .ir files for each source file.
};

/// Parses the provided command line into a set of compiler options.
/// If invalid arguments are provided, returns a human-readable error string.
Result<CompileSettings, Tritium::String> parseCommandLine(const char** argv, Size argc);
