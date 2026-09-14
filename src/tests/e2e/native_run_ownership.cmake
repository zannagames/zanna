# Native-run lanes for the ownership guarantees of ADR 0313 (synthesized
# destructors + class destructor hook) and ADR 0314 (declared runtime result
# ownership). Each fixture also runs on the bytecode VM through
# ZIA_RUNTIME_TESTS; the native binary must give the same answer because it
# installs the destructor hook with a real function address and releases
# exactly the results the runtime.def rows declare as owned.
foreach (fixture IN ITEMS test_object_field_release test_runtime_result_ownership test_class_cycle_gc)
    string(REPLACE "test_" "" _lane "${fixture}")
    add_test(NAME native_run_zia_${_lane}
            COMMAND ${CMAKE_COMMAND}
            -DZANNA_EXE=$<TARGET_FILE:zanna>
            -DTEST_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/runtime/${fixture}.zia
            -DOUT_EXE=${CMAKE_BINARY_DIR}/zia_${_lane}_native
            -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_zia_native_run.cmake
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(native_run_zia_${_lane} PROPERTIES
            LABELS "zia;native_run"
            TIMEOUT 60)
endforeach ()

# BASIC twin of the ADR 0314 lane; the VM run is basic_runtime_test_basic_runtime_result_ownership.
add_test(NAME native_run_basic_runtime_result_ownership
        COMMAND ${CMAKE_COMMAND}
        -DZANNA_EXE=$<TARGET_FILE:zanna>
        -DTEST_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/runtime/test_basic_runtime_result_ownership.bas
        -DOUT_EXE=${CMAKE_BINARY_DIR}/basic_runtime_result_ownership_native
        -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_zia_native_run.cmake
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(native_run_basic_runtime_result_ownership PROPERTIES
        LABELS "basic;native_run"
        TIMEOUT 60)

# Native twin of basic_runtime_test_basic_class_lifetimes: class array field
# destructors and FUNCTION AS <Class> results.
add_test(NAME native_run_basic_class_lifetimes
        COMMAND ${CMAKE_COMMAND}
        -DZANNA_EXE=$<TARGET_FILE:zanna>
        -DTEST_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/runtime/test_basic_class_lifetimes.bas
        -DOUT_EXE=${CMAKE_BINARY_DIR}/basic_class_lifetimes_native
        -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_zia_native_run.cmake
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(native_run_basic_class_lifetimes PROPERTIES
        LABELS "basic;native_run"
        TIMEOUT 60)

# Native twins of basic_runtime_test_basic_managed_ownership (ADR 0147): native
# code has no register teardown, so every reference the lowering creates must be
# released explicitly; -O0 keeps every slot in memory.
foreach (_owned_opt IN ITEMS default O0)
    set(_owned_flag "")
    if (_owned_opt STREQUAL "O0")
        set(_owned_flag "-O0")
    endif ()
    add_test(NAME native_run_basic_managed_ownership_${_owned_opt}
            COMMAND ${CMAKE_COMMAND}
            -DZANNA_EXE=$<TARGET_FILE:zanna>
            -DTEST_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/runtime/test_basic_managed_ownership.bas
            -DOUT_EXE=${CMAKE_BINARY_DIR}/basic_managed_ownership_native_${_owned_opt}
            -DOPT_FLAG=${_owned_flag}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_zia_native_run.cmake
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(native_run_basic_managed_ownership_${_owned_opt} PROPERTIES
            LABELS "basic;native_run"
            TIMEOUT 60)
endforeach ()

# Native twins of basic_runtime_test_basic_on_error_resume (ADR 0358): the ON ERROR
# dispatcher's setjmp-backed handler, RESUME re-entry, and zero-initialised
# locals, at the default level and at -O0 where no pass promotes the slots.
foreach (_onerr_opt IN ITEMS default O0)
    set(_onerr_flag "")
    if (_onerr_opt STREQUAL "O0")
        set(_onerr_flag "-O0")
    endif ()
    add_test(NAME native_run_basic_on_error_resume_${_onerr_opt}
            COMMAND ${CMAKE_COMMAND}
            -DZANNA_EXE=$<TARGET_FILE:zanna>
            -DTEST_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/runtime/test_basic_on_error_resume.bas
            -DOUT_EXE=${CMAKE_BINARY_DIR}/basic_on_error_resume_native_${_onerr_opt}
            -DOPT_FLAG=${_onerr_flag}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_zia_native_run.cmake
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(native_run_basic_on_error_resume_${_onerr_opt} PROPERTIES
            LABELS "basic;native_run"
            TIMEOUT 60)
endforeach ()
