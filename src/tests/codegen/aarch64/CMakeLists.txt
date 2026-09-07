# SPDX-License-Identifier: GPL-3.0-only
# File: tests/codegen/aarch64/CMakeLists.txt
# Purpose: Build dedicated AArch64 codegen tests over the shared IL corpus.

zanna_add_test(test_codegen_aarch64_shared_corpus
        ${CMAKE_CURRENT_LIST_DIR}/test_shared_il_corpus.cpp
        LIBS zanna_cmd_arm64 zanna_test_common
        COMPILE_DEFS ZANNA_SHARED_IL_CORPUS_DIR="${CMAKE_SOURCE_DIR}/src/tests/shared_corpus/il")
zanna_add_test_labels(test_codegen_aarch64_shared_corpus codegen arm64)

# Edge-copy lowering mode (Phase 3): lowering-only checks on every host.
zanna_add_test(test_aarch64_lowering_edge_copies
        ${CMAKE_SOURCE_DIR}/src/tests/unit/codegen/test_aarch64_lowering_edge_copies.cpp
        LIBS il_codegen_aarch64 zanna_test_common
        COMPILE_DEFS ZANNA_SHARED_IL_CORPUS_DIR="${CMAKE_SOURCE_DIR}/src/tests/shared_corpus/il")
zanna_add_test_labels(test_aarch64_lowering_edge_copies codegen arm64)
set_tests_properties(test_codegen_aarch64_shared_corpus PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 120
        RESOURCE_LOCK aarch64_codegen_artifacts)
