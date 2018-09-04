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
    set(CMAKE_CXX_FLAGS_DEBUG "-g -D_DEBUG -DDEBUG")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(OPTIMIZE_FLAGS "-Ofast -flto -fno-stack-protector -ffunction-sections -fdata-sections")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto")
    else()
        set(OPTIMIZE_FLAGS "-Ofast -flto -fno-stack-protector -ffunction-sections -fdata-sections -Wl,-flto,-s")
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