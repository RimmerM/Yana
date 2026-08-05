set(SOURCE_FILES Tester.cpp)

add_executable(YanaTest ${SOURCE_FILES})
add_executable(YanaResolveTest ResolveTester.cpp)
target_link_libraries(YanaResolveTest YanaParse YanaResolve YanaLower YanaX64 YanaJS YanaLLVM)

# The compiler's own speed over the same fixture corpus, for the container work that no golden file
# can see - see the header comment in BenchTester.cpp. Not a test: it asserts nothing and is not run
# by anything automatically.
add_executable(YanaBench BenchTester.cpp)
target_link_libraries(YanaBench YanaParse YanaResolve YanaLower YanaX64 YanaJS YanaLLVM)

add_executable(YanaParseTest ParserTester.cpp)
target_link_libraries(YanaParseTest YanaParse)

add_executable(YanaLowerTest LowerTester.cpp)
target_link_libraries(YanaLowerTest YanaLower)

add_executable(YanaX64Test X64Tester.cpp)
target_link_libraries(YanaX64Test YanaX64)

add_executable(YanaEditTest EditTester.cpp)
target_link_libraries(YanaEditTest YanaResolve)

add_executable(YanaLspTest LspTester.cpp)
target_link_libraries(YanaLspTest YanaLspCore)

add_executable(YanaLspProtocolTest LspProtocolTester.cpp)
target_link_libraries(YanaLspProtocolTest YanaLspCore)
