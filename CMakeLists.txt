cmake_minimum_required(VERSION 3.3)
project(Yana)

add_subdirectory(external/tritium/Code)

get_directory_property(OTHER_LIBS DIRECTORY external/tritium/Code DEFINITION OTHER_LIBS)
include_directories(external/tritium/Code/Tritium)
include_directories(external/tritium/Code/Tritium/Core)

add_subdirectory(code)
