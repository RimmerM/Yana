set(SOURCE_FILES Tester.cpp)

add_executable(YanaTest ${SOURCE_FILES})
target_link_libraries(YanaTest YanaParse YanaResolve YanaLower YanaX64 YanaLLVM YanaJS)

add_executable(YanaParseTest ParserTester.cpp)
target_link_libraries(YanaParseTest YanaParse)

add_executable(YanaLowerTest LowerTester.cpp)
target_link_libraries(YanaLowerTest YanaLower)

add_executable(YanaX64Test X64Tester.cpp)
target_link_libraries(YanaX64Test YanaX64)
