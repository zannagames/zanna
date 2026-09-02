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
