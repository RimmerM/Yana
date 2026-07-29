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
    resolve/witness.h
    resolve/witness.cpp
    resolve/analyze.h
    resolve/analyze_pass.h
    resolve/analyze.cpp
    resolve/analyze_effects.cpp
    resolve/analyze_live.cpp
    resolve/analyze_own.cpp
    resolve/analyze_provenance.cpp
    resolve/analyze_escape.cpp
    resolve/analyze_demand.cpp
    resolve/analyze_summary.cpp
    resolve/analyze_borrow.cpp
    resolve/analyze_drop.cpp
    resolve/analyze_teardown.cpp
    resolve/analyze_print.cpp
    resolve/print.cpp
    resolve/print.h
    resolve/builder.h
    resolve/expr.cpp
    resolve/expr.h
    resolve/expr_call.cpp
    resolve/expr_construct.cpp
    resolve/expr_fun.cpp
    resolve/expr_pat.cpp
    resolve/lower.h
    resolve/lower.cpp

    # Physical layout. Deliberately a directory of its own rather than part of resolve/: nothing in
    # the resolver may include it, and a separate directory is what makes an accidental include
    # visible in review. It links into YanaResolve because resolve/lower.cpp - the resolve-to-lower
    # translation, which is already on the code generator's side of that line - is its first caller,
    # and a library of its own would only make the dependency circular.
    repr/repr.h
    repr/repr.cpp
    repr/repr_print.h
    repr/repr_print.cpp
    repr/table.h
    repr/table.cpp
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
    codegen/llvm/gen.h
    codegen/llvm/build.h
    codegen/llvm/gen.cpp
    codegen/llvm/inst.cpp
    codegen/llvm/intrinsic.cpp
    codegen/llvm/emit.cpp
)

llvm_map_components_to_libnames(LLVM_LIBRARIES
    core support analysis passes target codegen mc linker
    x86codegen x86asmparser x86desc x86info
)

add_library(YanaLLVM ${LLVM_SOURCE_FILES})
target_include_directories(YanaLLVM PUBLIC ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITION_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_definitions(YanaLLVM PUBLIC ${LLVM_DEFINITION_LIST})
target_link_libraries(YanaLLVM PUBLIC YanaCore YanaLower ${LLVM_LIBRARIES})

# The pre-migration JS backend is retained, unchanged, in codegen/js_old/ and intentionally has no
# build target: it consumes the aggregate/Value IR the old resolver produced and predates places,
# ownership instructions, Repr and generic contexts. codegen/js is the rewrite against the current
# resolve IR that Analysis-JS.md part 3 describes.
set(JS_SOURCE_FILES
    codegen/js/gen.h
    codegen/js/build.h
    codegen/js/name.cpp
    codegen/js/type.cpp
    codegen/js/place.cpp
    codegen/js/inst.cpp
    codegen/js/flow.cpp
    codegen/js/gen.cpp
    codegen/js/ast.h
    codegen/js/opt.cpp
    codegen/js/format.cpp
)

add_library(YanaJS ${JS_SOURCE_FILES})
target_link_libraries(YanaJS PUBLIC YanaCore YanaResolve)

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
    compiler/driver.cpp
    compiler/source.cpp
    compiler/source.h
)

add_executable(Yana ${DRIVER_SOURCE_FILES})
target_link_libraries(Yana PUBLIC YanaCore YanaParse YanaResolve YanaLower YanaX64 YanaLLVM YanaJS)

if(APPLE)
    target_link_libraries(Yana ncurses)
endif()