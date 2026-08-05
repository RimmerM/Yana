cmake_minimum_required(VERSION 3.3)
project(Yana)

set(CMAKE_CXX_STANDARD 17)

if(MSVC)
    enable_language(ASM_MASM)
    set(OPTIMIZE_FLAGS "/O2 /GL /Oy /Ob2 /Oi /GS-")
    set(CMAKE_CXX_FLAGS "/std:c++17 /arch:AVX /GR- /EHs")
    set(CMAKE_CXX_FLAGS_DEBUG "/Zi /D_DEBUG /DDEBUG /Od /MDd /Ob0")
else()
    set(CMAKE_CXX_FLAGS "-std=c++1z -msse4.2 -mfpmath=sse -fno-rtti")

    # Appended rather than assigned, so that a `-DCMAKE_CXX_FLAGS_DEBUG=...` on the configure line
    # survives.
    #
    # An assignment here declares a *normal* variable, and a normal variable shadows the cache entry
    # that `-D` on the command line creates - so the flags asked for were read into the cache,
    # overwritten for the whole configure, and silently discarded. `build-assert` was configured with
    # `-O2 -g -D_DEBUG -DDEBUG` and compiled at -O0 for as long as it has existed: its `flags.make`
    # was byte-identical to `build`'s, and the two trees' test binaries timed within 0.2% of each
    # other. What is left here is only what CMake's own default for this build type does not already
    # supply, which for Debug is `-g`.
    #
    # The MSVC branch above assigns for the same reason and has the same shape; it is left alone
    # because nothing here can test it.
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_DEBUG -DDEBUG")
    # -O3 rather than -Ofast, because -Ofast is not free and buys nothing here.
    #
    # -Ofast is -O3 plus -ffast-math plus a handful of relaxations. For this compiler the
    # relaxations are all cost and no benefit: -ffast-math turns off signed zeros and permits
    # reassociation, which is a hazard for a program whose job includes folding its input's float
    # constants exactly; -fallow-store-data-races lets the optimizer invent stores that are not in
    # the source; and -Ofast additionally links crtfastmath.o, which sets flush-to-zero and
    # denormals-are-zero in MXCSR before main - a process-wide FPU mode that would make a denormal
    # literal fold to zero here and nowhere else. GCC's link spec matches -Ofast literally, so
    # -fno-fast-math does not switch that last one off.
    #
    # Against that, the measured difference is nothing: -Ofast, -Ofast -fno-fast-math and -O3
    # compile the fixture corpus within 0.2% of each other, which is run-to-run noise. This is
    # pointer-chasing integer code with no float math of its own.
    #
    # This is not a hypothetical. -Ofast broke float literals for two years: external/tritium's own
    # CMakeLists appends -Ofast to CMAKE_CXX_FLAGS_RELEASE, so `Tritium::readDouble` - which is what
    # lexes every float in a Yana program - was compiled with -fno-signed-zeros, which let gcc fold
    # `hasNegativeMantissa ? -0.0 : 0.0` down to the negative arm unconditionally. Every `0.0` in a
    # source file became -0.0, so `1.0 / 0.0` folded to -Infinity. That is what FoldFloat.yana and
    # Truth.yana were failing on in Release and nowhere else. Fixed in FloatRead.cpp by building the
    # zero from its bit pattern; tritium still passes -Ofast to itself, so anything float-valued
    # crossing that boundary is worth this much suspicion.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(OPTIMIZE_FLAGS "-O3 -flto -fno-stack-protector -ffunction-sections -fdata-sections")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto")
    else()
        set(OPTIMIZE_FLAGS "-O3 -flto -fno-stack-protector -ffunction-sections -fdata-sections -Wl,-flto,-s")
    endif()
endif()

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${OPTIMIZE_FLAGS}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${OPTIMIZE_FLAGS}")

add_subdirectory(external/tritium/Code)

get_directory_property(OTHER_LIBS DIRECTORY external/tritium/Code DEFINITION OTHER_LIBS)
include_directories(external/tritium/Code/Tritium)
include_directories(external/tritium/Code/Tritium/Core)

add_subdirectory(compiler)
add_subdirectory(test)