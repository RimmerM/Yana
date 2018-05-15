find_package(LLVM REQUIRED CONFIG)

set(SOURCE_FILES)
set(SOURCE_FILES ${SOURCE_FILES}
    compiler/context.h
    compiler/settings.h
    compiler/settings.cpp
    compiler/source.h
    compiler/source.cpp
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
    resolve/expr.h
    resolve/expr_call.cpp
    resolve/expr_construct.cpp
    resolve/expr_convert.cpp
    resolve/expr_pat.cpp
    resolve/builtins.cpp
    resolve/builtins.h
    resolve/eval.cpp
)

set(SOURCE_FILES ${SOURCE_FILES}
    codegen/llvm/gen.cpp
    codegen/llvm/gen.h
    codegen/llvm/link.cpp
)

set(SOURCE_FILES ${SOURCE_FILES}
    codegen/js/gen.h
    codegen/js/gen.cpp
    codegen/js/ast.h
    codegen/js/format.cpp
)

include_directories(${LLVM_INCLUDE_DIR})
add_library(YanaLib ${SOURCE_FILES})
add_executable(Yana ${SOURCE_FILES})

target_link_libraries(Yana
    TritiumCore ${OTHER_LIBS}
    LLVMLinker
    LLVMTransformUtils
    LLVMCore
    LLVMBinaryFormat
    LLVMSupport
    LLVMDemangle
)

if(APPLE)
    target_link_libraries(Yana ncurses)
endif()