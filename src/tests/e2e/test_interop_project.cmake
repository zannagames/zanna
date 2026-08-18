# SPDX-License-Identifier: GPL-3.0-only
# File: tests/e2e/test_interop_project.cmake
# Purpose: Run one mixed-language project and compare stdout to its expected output.
# Links: docs/adr/0268-cross-language-symbol-resolution.md, docs/languages/interop.md
if (NOT DEFINED ILC)
    message(FATAL_ERROR "ILC not set")
endif ()
if (NOT DEFINED PROJECT_DIR)
    message(FATAL_ERROR "PROJECT_DIR not set")
endif ()
if (NOT DEFINED EXPECT_FILE)
    message(FATAL_ERROR "EXPECT_FILE not set")
endif ()

execute_process(
        COMMAND ${ILC} run ${PROJECT_DIR}
        RESULT_VARIABLE RES
        OUTPUT_VARIABLE OUT
        ERROR_VARIABLE ERR)

if (NOT RES EQUAL 0)
    message(FATAL_ERROR "unexpected exit status: ${RES}\nstdout: ${OUT}\nstderr: ${ERR}")
endif ()

file(READ ${EXPECT_FILE} EXPECT_CONTENT)
string(REPLACE "\r\n" "\n" EXPECT_CONTENT "${EXPECT_CONTENT}")
string(REGEX REPLACE "\n+$" "" EXPECT_CONTENT "${EXPECT_CONTENT}")
string(REPLACE "\r\n" "\n" OUT "${OUT}")
string(REGEX REPLACE "\n+$" "" OUT "${OUT}")

if (NOT OUT STREQUAL EXPECT_CONTENT)
    message(FATAL_ERROR "stdout mismatch\nexpected: '${EXPECT_CONTENT}'\nactual: '${OUT}'\nstderr: ${ERR}")
endif ()
