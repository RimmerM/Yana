set(SOURCE_FILES Tester.cpp)

add_executable(YanaTest ${SOURCE_FILES})
add_executable(YanaResolveTest ResolveTester.cpp)
target_link_libraries(YanaResolveTest YanaParse YanaResolve YanaLower YanaX64)

add_executable(YanaParseTest ParserTester.cpp)
target_link_libraries(YanaParseTest YanaParse)

add_executable(YanaLowerTest LowerTester.cpp)
target_link_libraries(YanaLowerTest YanaLower)

add_executable(YanaX64Test X64Tester.cpp)
target_link_libraries(YanaX64Test YanaX64)
