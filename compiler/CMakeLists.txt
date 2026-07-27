find_package(LLVM REQUIRED CONFIG)

set(CORE_SOURCE_FILES
    compiler/context.h
    compiler/context.cpp
    compiler/settings.h
    compiler/settings.cpp
    compiler/diagnostics.h
    compiler/diagnostics.cpp

    util/lexer_util.h
    util/lexer_util.cpp
    util/parser_util.h
    util/container.h
)

add_library(YanaCore ${CORE_SOURCE_FILES})
target_link_libraries(YanaCore PUBLIC TritiumNet TritiumCore ${OTHER_LIBS})

set(PARSE_SOURCE_FILES
    parse/lexer.cpp
    parse/lexer.h
    parse/ast.h
    parse/parser.cpp
    parse/parser.h
    parse/ast_print.cpp
    parse/ast_print.h
)

add_library(YanaParse ${PARSE_SOURCE_FILES})
target_link_libraries(YanaParse PUBLIC YanaCore)

set(RESOLVE_SOURCE_FILES
    resolve/inst.h
    resolve/inst.cpp
    resolve/block.h
    resolve/block.cpp
    resolve/module.h
    resolve/module.cpp
    resolve/name.h
    resolve/name.cpp
    resolve/class.h
    resolve/core.h
    resolve/core.cpp
    resolve/native.h
    resolve/native.cpp
    resolve/intrinsic.h
    resolve/intrinsic.cpp
    resolve/type.h
    resolve/type.cpp
    resolve/generic.h
    resolve/generic.cpp
    resolve/print.cpp
    resolve/print.h
    resolve/builder.h
    resolve/expr.cpp
    resolve/expr.h
    resolve/expr_call.cpp
    resolve/expr_construct.cpp
    resolve/expr_pat.cpp
    resolve/lower.h
    resolve/lower.cpp
)

add_library(YanaResolve ${RESOLVE_SOURCE_FILES})
target_link_libraries(YanaResolve PUBLIC YanaCore YanaParse)

set(LOWER_SOURCE_FILES
    lower/lower.h
    lower/lower.cpp
    lower/lower_inst.h
    lower/lower_parser.h
    lower/lower_parser.cpp
    lower/lower_lexer.cpp
    lower/lower_lexer.h
    lower/lower_print.cpp
    lower/lower_print.h
    lower/lower_validate.cpp
    lower/lower_validate.h
    lower/lower_resolve.cpp
    lower/lower_resolve.h
    lower/lower_analyze.cpp
    lower/lower_builder.h
)

add_library(YanaLower ${LOWER_SOURCE_FILES})
target_link_libraries(YanaLower PUBLIC YanaCore)
target_link_libraries(YanaResolve PUBLIC YanaLower)

set(LLVM_SOURCE_FILES
    codegen/llvm/gen.cpp
    codegen/llvm/gen.h
    codegen/llvm/link.cpp
)

add_library(YanaLLVM ${LLVM_SOURCE_FILES})
target_include_directories(YanaLLVM PUBLIC ${LLVM_INCLUDE_DIR})
target_link_libraries(YanaLLVM PUBLIC
    YanaCore
    LLVMLinker
    LLVMTransformUtils
    LLVMCore
    LLVMBinaryFormat
    LLVMSupport
    LLVMDemangle
)

set(JS_SOURCE_FILES
    codegen/js/gen.h
    codegen/js/gen.cpp
    codegen/js/ast.h
    codegen/js/format.cpp
)

add_library(YanaJS ${JS_SOURCE_FILES})
target_link_libraries(YanaJS PUBLIC YanaCore)

set(X64_SOURCE_FILES
    codegen/x64/target.h
    codegen/x64/target.cpp
    codegen/x64/machine.h
    codegen/x64/machine.cpp
    codegen/x64/intrinsic.cpp
    codegen/x64/gen.h
    codegen/x64/gen.cpp
    codegen/x64/transform.cpp
    codegen/x64/place.cpp
    codegen/x64/legalize.cpp
    codegen/x64/register.cpp
    codegen/x64/verify.cpp
    codegen/x64/frame.cpp
    codegen/x64/constraint.cpp
    codegen/x64/x64_util.h
)

add_library(YanaX64 ${X64_SOURCE_FILES})
target_link_libraries(YanaX64 PUBLIC YanaCore YanaLower)

set(DRIVER_SOURCE_FILES
    compiler/source.h
    compiler/source.cpp
    compiler/driver.cpp
)

add_executable(Yana ${DRIVER_SOURCE_FILES})
target_link_libraries(Yana PUBLIC YanaCore YanaParse YanaResolve YanaLower YanaX64 YanaLLVM YanaJS)

if(APPLE)
    target_link_libraries(Yana ncurses)
endif()