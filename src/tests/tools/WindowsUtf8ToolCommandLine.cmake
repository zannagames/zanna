#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: src/tests/tools/WindowsUtf8ToolCommandLine.cmake
# Purpose: Exercise native Windows tool argv and filesystem paths outside the ACP.
# Key invariants:
#   - The zanna driver, standalone frontends, and rtgen receive strict UTF-8 paths.
#   - Test files and directories use characters that legacy ACP argv cannot preserve.
# Ownership/Lifetime: The script removes its private workspace before and after use.
# Links: src/common/Utf8CommandLine.hpp, src/common/Filesystem.hpp,
#        src/tools/rtgen/rtgen.cpp
#
#===----------------------------------------------------------------------===#

foreach(required IN ITEMS ZANNA ZIA ZBASIC RTGEN RUNTIME_DEF WORK_ROOT)
    if (NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif ()
endforeach()

set(root "${WORK_ROOT}/zanna-tool-東京-α")
set(source "${root}/入力-猫.zia")
set(basic_source "${root}/入力-猫.bas")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}")
file(WRITE "${source}" "module Utf8WindowsTool;\n\nfunc start() {}\n")
file(WRITE "${basic_source}" "PRINT \"UTF-8 path\"\n")

execute_process(
    COMMAND "${ZANNA}" check "${source}" --diagnostic-format=json
    RESULT_VARIABLE zanna_result
    OUTPUT_VARIABLE zanna_stdout
    ERROR_VARIABLE zanna_stderr
    TIMEOUT 30)
if (NOT zanna_result EQUAL 0)
    message(FATAL_ERROR
        "zanna rejected a Unicode command-line path (${zanna_result})\n"
        "stdout:\n${zanna_stdout}\nstderr:\n${zanna_stderr}")
endif ()

set(il_output "${root}/出力/結果.il")
execute_process(
    COMMAND "${ZANNA}" build "${source}" -o "${il_output}"
    RESULT_VARIABLE zanna_build_result
    OUTPUT_VARIABLE zanna_build_stdout
    ERROR_VARIABLE zanna_build_stderr
    TIMEOUT 30)
if (NOT zanna_build_result EQUAL 0 OR NOT EXISTS "${il_output}")
    message(FATAL_ERROR
        "zanna failed to publish a Unicode IL output path (${zanna_build_result})\n"
        "stdout:\n${zanna_build_stdout}\nstderr:\n${zanna_build_stderr}")
endif ()

set(unicode_project "東京Project")
execute_process(
    COMMAND "${ZANNA}" init "${unicode_project}" --lang zia
    WORKING_DIRECTORY "${root}"
    RESULT_VARIABLE zanna_init_result
    OUTPUT_VARIABLE zanna_init_stdout
    ERROR_VARIABLE zanna_init_stderr
    TIMEOUT 30)
if (NOT zanna_init_result EQUAL 0 OR
    NOT EXISTS "${root}/${unicode_project}/zanna.project" OR
    NOT EXISTS "${root}/${unicode_project}/main.zia")
    message(FATAL_ERROR
        "zanna init rejected a Unicode project name (${zanna_init_result})\n"
        "stdout:\n${zanna_init_stdout}\nstderr:\n${zanna_init_stderr}")
endif ()

execute_process(
    COMMAND "${ZIA}" "${source}" --emit-il
    RESULT_VARIABLE zia_result
    OUTPUT_VARIABLE zia_stdout
    ERROR_VARIABLE zia_stderr
    TIMEOUT 30)
if (NOT zia_result EQUAL 0)
    message(FATAL_ERROR
        "zia rejected a Unicode command-line path (${zia_result})\n"
        "stdout:\n${zia_stdout}\nstderr:\n${zia_stderr}")
endif ()

execute_process(
    COMMAND "${ZBASIC}" "${basic_source}" --emit-il
    RESULT_VARIABLE zbasic_result
    OUTPUT_VARIABLE zbasic_stdout
    ERROR_VARIABLE zbasic_stderr
    TIMEOUT 30)
if (NOT zbasic_result EQUAL 0)
    message(FATAL_ERROR
        "zbasic rejected a Unicode command-line path (${zbasic_result})\n"
        "stdout:\n${zbasic_stdout}\nstderr:\n${zbasic_stderr}")
endif ()

set(rtgen_output "${root}/生成-runtime")
execute_process(
    COMMAND "${RTGEN}" "${RUNTIME_DEF}" "${rtgen_output}"
    RESULT_VARIABLE rtgen_result
    OUTPUT_VARIABLE rtgen_stdout
    ERROR_VARIABLE rtgen_stderr
    TIMEOUT 60)
if (NOT rtgen_result EQUAL 0)
    message(FATAL_ERROR
        "rtgen rejected a Unicode output path (${rtgen_result})\n"
        "stdout:\n${rtgen_stdout}\nstderr:\n${rtgen_stderr}")
endif ()
if (NOT EXISTS "${rtgen_output}/RuntimeNameMap.inc" OR
    NOT EXISTS "${rtgen_output}/RuntimeNames.hpp")
    message(FATAL_ERROR "rtgen did not publish its complete Unicode-path output")
endif ()

file(REMOVE_RECURSE "${root}")
