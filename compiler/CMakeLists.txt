find_package(LLVM REQUIRED CONFIG)

set(CORE_SOURCE_FILES
    compiler/context.h
    compiler/context.cpp
    compiler/settings.h
    compiler/settings.cpp
    compiler/library.h
    compiler/library.cpp
    compiler/diagnostics.h
    compiler/diagnostics.cpp
    compiler/position.h
    compiler/position.cpp

    util/lexer_util.h
    util/lexer_util.cpp
    util/parser_util.h
    util/container.h
)

add_library(YanaCore ${CORE_SOURCE_FILES})
target_link_libraries(YanaCore PUBLIC TritiumNet TritiumCore ${OTHER_LIBS})

# The standard library's location, baked in as the last of LibrarySource's four candidates. It is
# what makes a build tree work with nothing configured: every driver in test/ constructs a Context
# and resolves a program, and none of them has a command line to pass `-lib` on. An installed
# compiler finds its library beside itself instead - see executableDirectory - so this is a
# fallback for the tree rather than the answer a shipped compiler relies on.
target_compile_definitions(YanaCore PRIVATE YANA_LIBRARY_DIR="${CMAKE_SOURCE_DIR}/lib")

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
    resolve/inst.def
    resolve/inst.h
    resolve/inst.cpp
    resolve/block.h
    resolve/edit.h
    resolve/edit.cpp
    resolve/module.h
    resolve/module_internal.h
    resolve/module.cpp
    resolve/module_decl.cpp
    resolve/module_sig.cpp
    resolve/module_class.cpp
    resolve/module_default.cpp
    resolve/module_import.cpp
    resolve/module_reach.cpp
    resolve/module_derive.cpp
    resolve/module_export.cpp
    resolve/const.h
    resolve/const.cpp
    resolve/name.h
    resolve/name.cpp
    resolve/index.h
    resolve/index.cpp
    resolve/complete.h
    resolve/complete.cpp
    resolve/class.h
    resolve/core.h
    resolve/core.cpp
    resolve/native.h
    resolve/native.cpp
    resolve/atomic.cpp
    resolve/atomic.h
    resolve/simd.h
    resolve/simd.cpp
    resolve/host.h
    resolve/host.cpp
    resolve/intrinsic.h
    resolve/intrinsic.cpp
    resolve/type.h
    resolve/type_internal.h
    resolve/type.cpp
    resolve/type_query.cpp
    resolve/type_ast.cpp
    resolve/type_schema.cpp
    resolve/type_layout.cpp
    resolve/type_own.cpp
    resolve/type_print.cpp
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
    resolve/verify.h
    resolve/verify.cpp
    resolve/explain.h
    resolve/explain.cpp
    resolve/builder.h
    resolve/solve.h
    resolve/solve.cpp
    resolve/expr.h
    resolve/expr.cpp
    resolve/expr_convert.cpp
    resolve/expr_flow.cpp
    resolve/expr_decl.cpp
    resolve/expr_body.cpp
    resolve/expr_call.cpp
    resolve/expr_construct.cpp
    resolve/expr_fun.cpp
    resolve/expr_lens_internal.h
    resolve/expr_lens.cpp
    resolve/expr_cont.cpp
    resolve/expr_iter.cpp
    resolve/expr_try.cpp
    resolve/expr_pat.cpp
    resolve/lower.h
    resolve/lower_internal.h
    resolve/lower.cpp
    resolve/lower_type.cpp
    resolve/lower_gen.cpp
    resolve/lower_place.cpp
    resolve/lower_pack.cpp
    resolve/lower_inst.cpp
    resolve/lower_mem.cpp
    resolve/lower_calc.cpp
    resolve/lower_call.cpp

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
    repr/constant.h
    repr/constant.cpp

    # The optimizer - see Analysis-Optimization.md. A directory of its own on exactly the terms repr/
    # is one: it runs over the resolve IR but sits on the *emitting* side of the layout wall, so it
    # may include compiler/repr and nothing in compiler/resolve may include it. The separate
    # directory is what makes an accidental include visible in review. It links in here because both
    # of its callers - resolve/lower.cpp and codegen/js - are already on that side, and a library of
    # its own would be circular with the resolve IR it rewrites.
    opt/opt.h
    opt/opt_pass.h
    opt/opt.cpp
    opt/opt_arg.cpp
    opt/opt_bool.cpp
    opt/opt_borrow.cpp
    opt/opt_branch.cpp
    opt/opt_check.cpp
    opt/opt_closure.cpp
    opt/opt_discharge.cpp
    opt/opt_flow.cpp
    opt/opt_fold.cpp
    opt/opt_inline.cpp
    opt/opt_loop.cpp
    opt/opt_range.cpp
    opt/opt_pack.cpp
    opt/opt_place.cpp
    opt/opt_promote.cpp
    opt/opt_scalar.cpp
    opt/opt_select.cpp
    opt/opt_value.cpp
)
set_source_files_properties(resolve/inst.def PROPERTIES HEADER_FILE_ONLY TRUE)

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
    lower/lower_fold.h
    lower/lower_fold.cpp
    lower/lower_forward.h
    lower/lower_forward.cpp
    lower/lower_split.h
    lower/lower_split.cpp
    lower/lower_promote.h
    lower/lower_promote.cpp
    lower/lower_tail.h
    lower/lower_tail.cpp
    lower/lower_store.h
    lower/lower_store.cpp
    lower/lower_cse.h
    lower/lower_cse.cpp
    lower/lower_licm.h
    lower/lower_licm.cpp
    lower/lower_recover.h
    lower/lower_recover.cpp
    lower/lower_strength.h
    lower/lower_strength.cpp
    lower/lower_divide.h
    lower/lower_divide.cpp
    lower/lower_induction.h
    lower/lower_induction.cpp
    lower/lower_merge.h
    lower/lower_merge.cpp
    lower/lower_thread.h
    lower/lower_thread.cpp
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
    codegen/js/wide.cpp
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
set(ELF_SOURCE_FILES
    codegen/elf/elf.h
    codegen/elf/elf.cpp
)

add_library(YanaElf ${ELF_SOURCE_FILES})
target_link_libraries(YanaElf PUBLIC YanaCore)

set(X64_SOURCE_FILES
    codegen/x64/target.h
    codegen/x64/target.cpp
    codegen/x64/machine.h
    codegen/x64/machine_internal.h
    codegen/x64/machine.cpp
    codegen/x64/machine_forms_scalar.cpp
    codegen/x64/machine_forms_packed.cpp
    codegen/x64/machine_forms_tiers.cpp
    codegen/x64/machine_forms_memory.cpp
    codegen/x64/machine_validate.cpp
    codegen/x64/machine_select.cpp
    codegen/x64/machine_vector.cpp
    codegen/x64/intrinsic.cpp
    codegen/x64/gen.h
    codegen/x64/gen.cpp
    codegen/x64/transform_internal.h
    codegen/x64/transform.cpp
    codegen/x64/transform_edit.cpp
    codegen/x64/transform_peephole.cpp
    codegen/x64/transform_address.cpp
    codegen/x64/transform_loop.cpp
    codegen/x64/transform_expand.cpp
    codegen/x64/transform_block.cpp
    codegen/x64/transform_reduce.cpp
    codegen/x64/transform_constant.cpp
    codegen/x64/place.cpp
    codegen/x64/legalize.cpp
    codegen/x64/register.cpp
    codegen/x64/verify.cpp
    codegen/x64/frame.cpp
    codegen/x64/constraint.cpp
    codegen/x64/emit.h
    codegen/x64/emit.cpp
    codegen/x64/x64_util.h
)

add_library(YanaX64 ${X64_SOURCE_FILES})
target_link_libraries(YanaX64 PUBLIC YanaCore YanaLower YanaElf)

set(PROJECT_SOURCE_FILES
    compiler/source.h
    compiler/source.cpp
    compiler/project.h
    compiler/project.cpp
    compiler/overlay.h
    compiler/overlay.cpp
)

add_library(YanaProject ${PROJECT_SOURCE_FILES})
target_link_libraries(YanaProject PUBLIC YanaCore YanaParse YanaResolve)
set(DRIVER_SOURCE_FILES
    compiler/driver.cpp
)

add_executable(Yana ${DRIVER_SOURCE_FILES})
target_link_libraries(Yana PUBLIC YanaCore YanaParse YanaResolve YanaProject YanaLower YanaLLVM YanaJS YanaX64)

if(APPLE)
    target_link_libraries(Yana ncurses)
endif()

set(LSP_SOURCE_FILES
    lsp/json.h
    lsp/json.cpp
    lsp/protocol.h
    lsp/protocol.cpp
    lsp/document.h
    lsp/document.cpp
    lsp/session.h
    lsp/session.cpp
    lsp/feature.h
    lsp/feature.cpp
    lsp/server.h
    lsp/server.cpp
)
add_library(YanaLspCore ${LSP_SOURCE_FILES})
target_link_libraries(YanaLspCore PUBLIC YanaCore YanaParse YanaResolve YanaProject)

add_executable(YanaLsp lsp/main.cpp)
set_target_properties(YanaLsp PROPERTIES OUTPUT_NAME yana-lsp)
target_link_libraries(YanaLsp PUBLIC YanaLspCore)
