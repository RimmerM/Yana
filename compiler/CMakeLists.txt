find_package(LLVM REQUIRED CONFIG)

set(CORE_SOURCE_FILES
    compiler/context.h
    compiler/context.cpp
    compiler/settings.h
    compiler/settings.cpp
    compiler/replace.h
    compiler/library.h
    compiler/library.cpp
    compiler/project.h
    compiler/project.cpp
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

set(INTRINSIC_SOURCE_FILES
    intrinsic/role.h
    intrinsic/role.cpp
    intrinsic/known.def
    intrinsic/known.h
    intrinsic/op.def
    intrinsic/op.h
    intrinsic/shape.h
    intrinsic/supplied.def
    intrinsic/supplied.h
    intrinsic/provided.def
    intrinsic/provided.h
    intrinsic/primitive.def
    intrinsic/primitive.h
    intrinsic/convert.h
    intrinsic/text.h
)

add_library(YanaIntrinsic ${INTRINSIC_SOURCE_FILES})
target_link_libraries(YanaIntrinsic PUBLIC YanaCore)

set(PARSE_SOURCE_FILES
    parse/lexer.cpp
    parse/lexer.h
    parse/ast.h
    parse/parser.cpp
    parse/parser.h
    parse/ast_print.cpp
    parse/ast_print.h
    parse/library.h
    parse/library.cpp
)

add_library(YanaParse ${PARSE_SOURCE_FILES})
target_link_libraries(YanaParse PUBLIC YanaCore)

set(LOWER_SOURCE_FILES
    lower/lower.h
    lower/lower.cpp
    lower/lower_inst.h
    lower/inst.def
    lower/inst.cpp
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
    lower/fold_arith.h
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
    lower/lower_params.h
    lower/lower_params.cpp
    lower/lower_sweep.h
    lower/lower_sweep.cpp
    lower/lower_alias.h
    lower/lower_alias.cpp
    lower/lower_teardown.h
    lower/lower_teardown.cpp
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
    lower/lower_pipeline.h
    lower/lower_pipeline.cpp
)

# The instruction table is #included twice by lower/lower_inst.h - once to generate the enum and
# once to read the traits - so it is listed for the sake of being findable and never compiled on its
# own.
set_source_files_properties(lower/inst.def PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(YanaLower ${LOWER_SOURCE_FILES})
target_link_libraries(YanaLower PUBLIC YanaCore YanaIntrinsic)

set(RESOLVE_MIR_SOURCE_FILES
    resolve_mir/region.h
    resolve_mir/intern.h
    resolve_mir/type.def
    resolve_mir/type.h
    resolve_mir/type.cpp
    resolve_mir/contract.h
    resolve_mir/constant.h
    resolve_mir/op.def
    resolve_mir/op.h
    resolve_mir/op.cpp
    resolve_mir/place.h
    resolve_mir/loan.h
    resolve_mir/inst.def
    resolve_mir/native.def
    resolve_mir/host.def
    resolve_mir/inst.h
    resolve_mir/inst.cpp
    resolve_mir/body.h
    resolve_mir/decl.h
    resolve_mir/build.h
    resolve_mir/build.cpp
    resolve_mir/own.h
    resolve_mir/own.cpp
    resolve_mir/gen.h
    resolve_mir/gen.cpp
    resolve_mir/select.h
    resolve_mir/select.cpp
    resolve_mir/specialize.h
    resolve_mir/specialize.cpp
    resolve_mir/analyze.h
    resolve_mir/analyze_internal.h
    resolve_mir/analyze_escape.cpp
    resolve_mir/analyze_path.cpp
    resolve_mir/analyze_loan.cpp
    resolve_mir/analyze_live.cpp
    resolve_mir/analyze_drop.cpp
    resolve_mir/verify.h
    resolve_mir/verify.cpp
    resolve_mir/summary.h
    resolve_mir/summary.cpp
    resolve_mir/print.h
    resolve_mir/print.cpp
    resolve_mir/resolve.h
    resolve_mir/resolve_internal.h
    resolve_mir/resolve.cpp

    resolve_mir/name.h
    resolve_mir/name.cpp
    resolve_mir/index.h
    resolve_mir/index.cpp
    resolve_mir/explain.h
    resolve_mir/explain.cpp
    resolve_mir/complete.h
    resolve_mir/complete.cpp
    resolve_mir/import.cpp
    resolve_mir/export.cpp

    resolve_mir/resolve_decl.cpp
    resolve_mir/resolve_sig.cpp
    resolve_mir/resolve_caller.cpp
    resolve_mir/resolve_test.cpp
    resolve_mir/convert.cpp
    resolve_mir/resolve_class.cpp
    resolve_mir/resolve_derive.cpp
    resolve_mir/resolve_replace.cpp
    resolve_mir/resolve_expr.cpp
    resolve_mir/resolve_overload.cpp
    resolve_mir/resolve_stmt.cpp
    resolve_mir/resolve_try.cpp
    resolve_mir/resolve_lambda.cpp
    resolve_mir/resolve_match.cpp
    resolve_mir/resolve_object.cpp
    resolve_mir/fold.h
    resolve_mir/fold.cpp

    resolve_mir/text.cpp
    resolve_mir/resolve_intrinsic.cpp

    resolve_mir/intrinsic/roles.h
    resolve_mir/intrinsic/link.h
    resolve_mir/intrinsic/link.cpp
    resolve_mir/intrinsic/expand.h
    resolve_mir/intrinsic/expand_internal.h
    resolve_mir/intrinsic/expand.cpp
    resolve_mir/intrinsic/expand_pointer.cpp
    resolve_mir/intrinsic/expand_scalar.cpp
    resolve_mir/intrinsic/expand_exchange.cpp
    resolve_mir/intrinsic/expand_native.cpp
    resolve_mir/intrinsic/expand_host.cpp
    resolve_mir/intrinsic/expand_atomic.cpp
    resolve_mir/intrinsic/expand_vector.cpp
    resolve_mir/intrinsic/expand_container.cpp
    resolve_mir/intrinsic/expand_provided.cpp
    resolve_mir/intrinsic/provide.cpp
)

set(RESOLVE_SSA_SOURCE_FILES
    resolve_ssa/inst.def
    resolve_ssa/inst.h
    resolve_ssa/inst.cpp
    resolve_ssa/build.h
    resolve_ssa/build.cpp
    resolve_ssa/convert.h
    resolve_ssa/convert.cpp
    resolve_ssa/dominance.h
    resolve_ssa/dominance.cpp
    resolve_ssa/verify.h
    resolve_ssa/verify.cpp
    resolve_ssa/print.h
    resolve_ssa/print.cpp
    resolve_ssa/pipeline.h
    resolve_ssa/pipeline.cpp

    resolve_ssa/opt/opt.h
    resolve_ssa/opt/opt.cpp
    resolve_ssa/opt/pass.h
    resolve_ssa/opt/analysis.h
    resolve_ssa/opt/analysis.cpp
    resolve_ssa/opt/arg.cpp
    resolve_ssa/opt/teardown.cpp
    resolve_ssa/opt/promote.cpp
    resolve_ssa/opt/simplify.cpp
    resolve_ssa/opt/cse.cpp
    resolve_ssa/opt/fold.cpp
    resolve_ssa/opt/cfg.cpp
    resolve_ssa/opt/select.cpp
    resolve_ssa/opt/memory.cpp
    resolve_ssa/opt/place.cpp
    resolve_ssa/opt/loop.cpp
    resolve_ssa/opt/inline.cpp
    resolve_ssa/opt/reach.cpp
    resolve_ssa/opt/check.cpp
    resolve_ssa/lower/repr.h
    resolve_ssa/lower/repr.cpp
    resolve_ssa/lower/repr_print.h
    resolve_ssa/lower/repr_print.cpp
    resolve_ssa/lower/manifest.h
    resolve_ssa/lower/manifest.cpp
    resolve_ssa/lower/gen.h
    resolve_ssa/lower/internal.h
    resolve_ssa/lower/gen.cpp
    resolve_ssa/lower/inst.cpp
    resolve_ssa/lower/convention.cpp
    resolve_ssa/lower/box.cpp
    resolve_ssa/lower/erase.cpp
    resolve_ssa/lower/fun.cpp
    resolve_ssa/lower/map.cpp
    resolve_ssa/lower/pack.cpp
    resolve_ssa/lower/width.cpp

    resolve_ssa/js/name.h
    resolve_ssa/js/name.cpp
    resolve_ssa/js/inst.def
    resolve_ssa/js/module.h
    resolve_ssa/js/inst.cpp
    resolve_ssa/js/repr.h
    resolve_ssa/js/repr.cpp
    resolve_ssa/js/abi.h
    resolve_ssa/js/abi.cpp
    resolve_ssa/js/build.h
    resolve_ssa/js/convert.h
    resolve_ssa/js/convert_internal.h
    resolve_ssa/js/convert.cpp
    resolve_ssa/js/constant.cpp
    resolve_ssa/js/teardown.cpp
    resolve_ssa/js/host.cpp
    resolve_ssa/js/vector.cpp
    resolve_ssa/js/verify.h
    resolve_ssa/js/verify.cpp
    resolve_ssa/js/cfg.h
    resolve_ssa/js/cfg.cpp
    resolve_ssa/js/opt.h
    resolve_ssa/js/opt_internal.h
    resolve_ssa/js/opt_analysis.cpp
    resolve_ssa/js/opt_alias.cpp
    resolve_ssa/js/opt_move.cpp
    resolve_ssa/js/opt_number.cpp
    resolve_ssa/js/opt_objects.cpp
    resolve_ssa/js/opt_variables.cpp
    resolve_ssa/js/opt_driver.cpp
    resolve_ssa/js/opt_coalesce.h
    resolve_ssa/js/opt_coalesce.cpp
    resolve_ssa/js/print.h
    resolve_ssa/js/print.cpp
)

set_source_files_properties(resolve_mir/inst.def resolve_mir/type.def resolve_mir/op.def
                            resolve_ssa/inst.def resolve_ssa/js/inst.def
                            PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(YanaResolveMir ${RESOLVE_MIR_SOURCE_FILES})
target_link_libraries(YanaResolveMir PUBLIC YanaCore YanaParse YanaIntrinsic)

add_library(YanaResolveSsa ${RESOLVE_SSA_SOURCE_FILES})
target_link_libraries(YanaResolveSsa PUBLIC YanaResolveMir YanaLower)

set(RAW_SOURCE_FILES
    codegen/raw/raw.h
    codegen/raw/raw.cpp
)

add_library(YanaRaw ${RAW_SOURCE_FILES})
target_link_libraries(YanaRaw PUBLIC YanaCore)

set(LLVM_SOURCE_FILES
    codegen/entry.h
    codegen/llvm/gen.h
    codegen/llvm/build.h
    codegen/llvm/gen.cpp
    codegen/llvm/inst.cpp
    codegen/llvm/intrinsic.cpp
    codegen/llvm/emit.cpp
)

llvm_map_components_to_libnames(LLVM_LIBRARIES
    core support analysis passes target codegen mc linker object
    x86codegen x86asmparser x86desc x86info
)

option(YANA_LLVM_DYLIB "Link the LLVM backend against the shared libLLVM" OFF)
if(YANA_LLVM_DYLIB)
    set(LLVM_LIBRARIES LLVM)
endif()

add_library(YanaLLVM ${LLVM_SOURCE_FILES})
target_include_directories(YanaLLVM PUBLIC ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITION_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_definitions(YanaLLVM PUBLIC ${LLVM_DEFINITION_LIST})
target_link_libraries(YanaLLVM PUBLIC YanaCore YanaLower YanaRaw ${LLVM_LIBRARIES})

# The JavaScript backend's back half: `structure.cpp` turns a verified JSIR CFG (resolve_ssa/js) into
# the syntax tree of ast.h and `assemble.cpp` is the file the functions go into; the rest is that
# tree's naming, runtime helpers, peephole and printing.
set(JS_SOURCE_FILES
    codegen/js/ast.h
    codegen/js/shared.h
    codegen/js/jsir.h
    codegen/js/name.cpp
    codegen/js/wide.cpp
    codegen/js/bits.cpp
    codegen/js/helpers.cpp
    codegen/js/opt.cpp
    codegen/js/format.cpp
    codegen/js/structure.cpp
    codegen/js/assemble.cpp
)

add_library(YanaJS ${JS_SOURCE_FILES})
target_link_libraries(YanaJS PUBLIC YanaCore YanaResolveSsa)

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
    codegen/x64/evex.cpp
    codegen/x64/machine_forms_scalar.cpp
    codegen/x64/machine_forms_packed.cpp
    codegen/x64/machine_forms_tiers.cpp
    codegen/x64/machine_forms_evex.cpp
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
    codegen/x64/transform_switch.cpp
    codegen/x64/transform_expand.cpp
    codegen/x64/transform_block.cpp
    codegen/x64/transform_reduce.cpp
    codegen/x64/transform_constant.cpp
    codegen/x64/transform_move.cpp
    codegen/x64/transform_split.cpp
    codegen/x64/transform_transfer.cpp
    codegen/x64/place.cpp
    codegen/x64/legalize.cpp
    codegen/x64/register.cpp
    codegen/x64/verify.cpp
    codegen/x64/frame.cpp
    codegen/x64/constraint.cpp
    codegen/entry.h
    codegen/x64/emit.h
    codegen/x64/emit.cpp
    codegen/x64/x64_util.h
)

add_library(YanaX64 ${X64_SOURCE_FILES})
target_link_libraries(YanaX64 PUBLIC YanaCore YanaLower YanaElf YanaRaw)

set(PROJECT_SOURCE_FILES
    compiler/source.h
    compiler/source.cpp
    compiler/overlay.h
    compiler/overlay.cpp
)

add_library(YanaProject ${PROJECT_SOURCE_FILES})
target_link_libraries(YanaProject PUBLIC YanaCore YanaParse)

set(DRIVER_SOURCE_FILES
    compiler/driver.cpp
)

add_executable(Yana ${DRIVER_SOURCE_FILES})
target_link_libraries(Yana PUBLIC YanaCore YanaIntrinsic YanaParse YanaResolveSsa YanaProject YanaLower YanaLLVM YanaJS YanaX64)

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
target_link_libraries(YanaLspCore PUBLIC YanaCore YanaParse YanaResolveMir YanaProject)

add_executable(YanaLsp lsp/main.cpp)
set_target_properties(YanaLsp PROPERTIES OUTPUT_NAME yana-lsp)
target_link_libraries(YanaLsp PUBLIC YanaLspCore)
