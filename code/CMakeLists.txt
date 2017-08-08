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
    compiler/context.cpp
    compiler/diagnostics.h
    compiler/diagnostics.cpp
    compiler/driver.cpp
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

set(SOURCE_FILES ${SOURCE_FILES}
    resolve/inst.h
    resolve/inst.cpp
    resolve/block.h
    resolve/block.cpp
    resolve/module.h
    resolve/module.cpp
    resolve/type.h
    resolve/type.cpp
    resolve/print.cpp
    resolve/print.h
    resolve/expr.cpp
    resolve/builtins.cpp
    resolve/builtins.h
)

set(SOURCE_FILES ${SOURCE_FILES}
    codegen/llvm/gen.cpp
    codegen/llvm/gen.h
)

if(APPLE)
    include_directories(/usr/local/include)
endif()

add_executable(code ${SOURCE_FILES})