cmake_minimum_required(VERSION 3.3)
project(Yana)

set(CMAKE_CXX_STANDARD 17)

add_subdirectory(external/tritium/Code)

get_directory_property(OTHER_LIBS DIRECTORY external/tritium/Code DEFINITION OTHER_LIBS)
include_directories(external/tritium/Code/Tritium)
include_directories(external/tritium/Code/Tritium/Core)

add_subdirectory(compiler)
add_subdirectory(test)