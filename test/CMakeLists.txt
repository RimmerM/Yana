set(SOURCE_FILES Tester.cpp)

include_directories(${LLVM_INCLUDE_DIR})
add_executable(YanaTest ${SOURCE_FILES})

target_link_libraries(YanaTest YanaLib TritiumCore ${OTHER_LIBS})
