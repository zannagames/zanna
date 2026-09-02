# SPDX-License-Identifier: GPL-3.0-only
# File: tests/tools/RtgenDefinitionManifestTests.cmake
# Purpose: Validate recursive runtime definition includes and documentation grammar.

cmake_minimum_required(VERSION 3.20)

if (NOT DEFINED RTGEN_EXE)
    message(FATAL_ERROR "RTGEN_EXE must be provided")
endif ()
if (NOT DEFINED TEST_WORK_DIR)
    message(FATAL_ERROR "TEST_WORK_DIR must be provided")
endif ()

file(REMOVE_RECURSE "${TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${TEST_WORK_DIR}/positive/nested")
file(WRITE "${TEST_WORK_DIR}/positive/runtime.def"
     "#include \"domain.def\"\n")
file(WRITE "${TEST_WORK_DIR}/positive/domain.def"
     "RT_FUNC(ExampleNew, rt_example_new, \"Zanna.Example.New\", \"obj()\", owned)\n"
     "#include \"nested/class.def\"\n")
file(WRITE "${TEST_WORK_DIR}/positive/nested/class.def"
     "/// @summary Provides an example runtime class.\n"
     "/// @details\n"
     "/// Exercises nested manifests and multiline documentation.\n"
     "///\n"
     "/// The paragraph break must be preserved.\n"
     "RT_CLASS_BEGIN(\"Zanna.Example\", Example, \"obj\", ExampleNew)\n"
     "    RT_METHOD(\"New\", \"obj()\", ExampleNew)\n"
     "RT_CLASS_END()\n")

execute_process(
        COMMAND "${RTGEN_EXE}" --validate "${TEST_WORK_DIR}/positive/runtime.def"
        RESULT_VARIABLE _positive_rc
        OUTPUT_VARIABLE _positive_out
        ERROR_VARIABLE _positive_err)
if (NOT _positive_rc EQUAL 0 OR
    NOT _positive_out MATCHES "Validated 1 functions, 1 classes")
    message(FATAL_ERROR "valid nested runtime manifest failed:\n${_positive_out}\n${_positive_err}")
endif ()

function(expect_rtgen_failure case_name root_text expected_error)
    set(_dir "${TEST_WORK_DIR}/${case_name}")
    file(MAKE_DIRECTORY "${_dir}")
    file(WRITE "${_dir}/runtime.def" "${root_text}")
    execute_process(
            COMMAND "${RTGEN_EXE}" --validate "${_dir}/runtime.def"
            RESULT_VARIABLE _rc
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err)
    if (_rc EQUAL 0)
        message(FATAL_ERROR "${case_name}: malformed manifest unexpectedly passed")
    endif ()
    if (NOT _err MATCHES "${expected_error}")
        message(FATAL_ERROR "${case_name}: expected '${expected_error}', got:\n${_err}")
    endif ()
endfunction()

file(MAKE_DIRECTORY "${TEST_WORK_DIR}/cycle")
file(WRITE "${TEST_WORK_DIR}/cycle/a.def" "#include \"runtime.def\"\n")
expect_rtgen_failure(cycle "#include \"a.def\"\n" "cyclic runtime definition include")

file(MAKE_DIRECTORY "${TEST_WORK_DIR}/duplicate")
file(WRITE "${TEST_WORK_DIR}/duplicate/row.def"
     "RT_FUNC(One, rt_one, \"Zanna.One\", \"void()\")\n")
expect_rtgen_failure(duplicate
     "#include \"row.def\"\n#include \"row.def\"\n"
     "duplicate runtime definition include")

file(WRITE "${TEST_WORK_DIR}/outside.def" "// outside\n")
expect_rtgen_failure(escape "#include \"../outside.def\"\n"
     "runtime definition include escapes definition root")

file(MAKE_DIRECTORY "${TEST_WORK_DIR}/inside_class")
file(WRITE "${TEST_WORK_DIR}/inside_class/row.def" "// row\n")
expect_rtgen_failure(inside_class
     "RT_CLASS_BEGIN(\"Zanna.Bad\", Bad, \"obj\", none)\n#include \"row.def\"\nRT_CLASS_END()\n"
     "include is not allowed inside a class block")

expect_rtgen_failure(orphan_docs
     "/// @summary Orphaned documentation.\n/// @details\n/// Has nowhere to go.\n"
     "orphaned runtime documentation block")

expect_rtgen_failure(missing_details
     "/// @summary Missing long documentation.\nRT_CLASS_BEGIN(\"Zanna.Bad\", Bad, \"obj\", none)\nRT_CLASS_END()\n"
     "documentation is missing @details")

expect_rtgen_failure(unknown_base
     "/// @summary Child class.\n/// @details\n/// Child details.\nRT_CLASS_BEGIN(\"Zanna.Child\", Child, \"obj\", none, \"Zanna.Missing\")\nRT_CLASS_END()\n"
     "unknown base class Zanna.Missing")

expect_rtgen_failure(class_cycle
     "/// @summary First class.\n/// @details\n/// First details.\nRT_CLASS_BEGIN(\"Zanna.First\", First, \"obj\", none, \"Zanna.Second\")\nRT_CLASS_END()\n/// @summary Second class.\n/// @details\n/// Second details.\nRT_CLASS_BEGIN(\"Zanna.Second\", Second, \"obj\", none, \"Zanna.First\")\nRT_CLASS_END()\n"
     "cyclic base-class chain")
