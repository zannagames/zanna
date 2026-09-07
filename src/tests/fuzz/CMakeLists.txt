# SPDX-License-Identifier: GPL-3.0-only
# File: tests/fuzz/CMakeLists.txt
# Purpose: Build libFuzzer harnesses for Zia lexer and parser.
#          Gated on ZANNA_ENABLE_FUZZ=ON; not registered as ctest targets.

if (NOT ZANNA_ENABLE_FUZZ)
    return()
endif ()

function(zanna_add_fuzzer target source)
    set(options)
    set(oneValueArgs CORPUS)
    set(multiValueArgs LIBS INCLUDES)
    cmake_parse_arguments(VF "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    add_executable(${target} ${source})
    target_include_directories(${target} PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_BINARY_DIR}/generated
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/src/runtime
            ${CMAKE_SOURCE_DIR}/src/runtime/core
            ${CMAKE_SOURCE_DIR}/src/runtime/localization
            ${CMAKE_SOURCE_DIR}/src/runtime/network
            ${CMAKE_SOURCE_DIR}/src/runtime/text
            ${VF_INCLUDES})
    target_link_libraries(${target} PRIVATE ${VF_LIBS})
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)

    if (VF_CORPUS)
        set(_corpus "${VF_CORPUS}")
    else ()
        string(REGEX REPLACE "^fuzz_" "" _corpus_name "${target}")
        set(_corpus "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${_corpus_name}")
    endif ()
    if (EXISTS "${_corpus}")
        add_test(NAME ${target}_replay
                COMMAND $<TARGET_FILE:${target}> -runs=1 "${_corpus}")
        set_tests_properties(${target}_replay PROPERTIES
                LABELS "fuzz"
                TIMEOUT 30)
    endif ()
endfunction()

# ── Localization parser fuzzers ─────────────────────────────────────────────
zanna_add_fuzzer(fuzz_plural_rule_parser fuzz_plural_rule_parser.cpp LIBS zanna_runtime)
zanna_add_fuzzer(fuzz_date_pattern_parser fuzz_date_pattern_parser.cpp LIBS zanna_runtime)
zanna_add_fuzzer(fuzz_locale_json_loader fuzz_locale_json_loader.cpp LIBS zanna_runtime)

# ── Frontend / IL parser fuzzers ────────────────────────────────────────────
zanna_add_fuzzer(fuzz_zia_lexer fuzz_zia_lexer.cpp LIBS fe_zia)
zanna_add_fuzzer(fuzz_zia_parser fuzz_zia_parser.cpp LIBS fe_zia)
zanna_add_fuzzer(fuzz_basic_lexer fuzz_basic_lexer.cpp LIBS fe_basic)
zanna_add_fuzzer(fuzz_basic_parser fuzz_basic_parser.cpp LIBS fe_basic)
zanna_add_fuzzer(fuzz_il_parser fuzz_il_parser.cpp LIBS zanna::il_full)
zanna_add_fuzzer(fuzz_il_verifier fuzz_il_verifier.cpp LIBS zanna::il_full)

# ── Native codegen differential fuzzer ──────────────────────────────────────
# Seeds an IL kernel from the input and requires the VM, the -O0 native build,
# and the -O2 native build to agree (see tests/common/ILKernelDiff.hpp). Built
# only where the host can run its own native backend.
if (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    zanna_add_fuzzer(fuzz_il_native_diff fuzz_il_native_diff.cpp
            LIBS zanna_test_common zanna_cmd_arm64 il_codegen_aarch64
            INCLUDES ${CMAKE_SOURCE_DIR}/src/tests)
    target_compile_definitions(fuzz_il_native_diff PRIVATE ZANNA_KERNEL_DIFF_ARM64)
elseif (ZANNA_BUILD_NATIVE_LINK_X86_64)
    zanna_add_fuzzer(fuzz_il_native_diff fuzz_il_native_diff.cpp
            LIBS zanna_test_common zanna_native_compiler zanna_codegen_x86_64
                 il_api il_transform il_tools_common zanna_bytecode
            INCLUDES ${CMAKE_SOURCE_DIR}/src/tests)
    target_sources(fuzz_il_native_diff PRIVATE ${CMAKE_SOURCE_DIR}/src/tools/zanna/cmd_codegen_x64.cpp)
    target_compile_definitions(fuzz_il_native_diff PRIVATE ZANNA_KERNEL_DIFF_X64)
endif ()

# ── Runtime/protocol parser fuzzers ─────────────────────────────────────────
zanna_add_fuzzer(fuzz_json fuzz_json.cpp LIBS zanna_runtime)
zanna_add_fuzzer(fuzz_http_request fuzz_http_request.cpp LIBS zanna_runtime)
zanna_add_fuzzer(fuzz_lsp_jsonrpc fuzz_lsp_jsonrpc.cpp LIBS lsp_common_lib)
zanna_add_fuzzer(fuzz_mcp_jsonrpc fuzz_mcp_jsonrpc.cpp LIBS lsp_common_lib)

# ── 3D Content Loader Fuzzers ───────────────────────────────────────────────
function(zanna_add_3d_loader_fuzzer target source)
    zanna_add_fuzzer(${target} ${source}
            LIBS zanna_runtime
            INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/assets
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/render
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/scene)
endfunction()

zanna_add_3d_loader_fuzzer(fuzz_gltf_loader fuzz_gltf_loader.cpp)
zanna_add_3d_loader_fuzzer(fuzz_gltf_draco fuzz_gltf_draco.cpp)
zanna_add_3d_loader_fuzzer(fuzz_vw3dsav_reader fuzz_vw3dsav_reader.cpp)
zanna_add_3d_loader_fuzzer(fuzz_fbx_loader fuzz_fbx_loader.cpp)
zanna_add_3d_loader_fuzzer(fuzz_vscn_loader fuzz_vscn_loader.cpp)
zanna_add_3d_loader_fuzzer(fuzz_obj_stl fuzz_obj_stl.cpp)
zanna_add_3d_loader_fuzzer(fuzz_stream_manifest fuzz_stream_manifest.cpp)

zanna_add_3d_loader_fuzzer(fuzz_ktx2_loader fuzz_ktx2_loader.cpp)
zanna_add_fuzzer(fuzz_zstd fuzz_zstd.cpp LIBS zanna_runtime INCLUDES ${CMAKE_SOURCE_DIR}/src/runtime/io)
zanna_add_fuzzer(fuzz_image_decoders fuzz_image_decoders.cpp LIBS zanna_runtime)
