cmake_minimum_required(VERSION 3.7)
project(code)

set(CMAKE_CXX_STANDARD 17)

set(SOURCE_FILES)
set(SOURCE_FILES ${SOURCE_FILES}
    util/array.cpp
    util/array.h
    util/types.h
    util/hash.h
    util/hash.cpp
    util/map.h
)

set(SOURCE_FILES ${SOURCE_FILES}
    compiler/context.h
    compiler/diagnostics.h
)

set(SOURCE_FILES ${SOURCE_FILES}
    parse/lexer.cpp
    parse/lexer.h
    parse/ast.h
    parse/parser.cpp
    parse/parser.h
    parse/ast_print.cpp
    parse/ast_print.h
)

add_executable(code ${SOURCE_FILES})