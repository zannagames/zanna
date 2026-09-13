#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: src/tests/e2e/test_zia_services_run.cmake
# Purpose: Run the Zanna.Services Zia fixture on the VM or as a native binary
#          with a fake steam_api redistributable (or a missing one) selected
#          through the environment.
# Key invariants:
#   - MODE is "vm" (zanna run) or "native" (zanna build, then run the output).
#   - EXPECT is "available" (ZANNA_SERVICES_STEAM_LIBRARY names FAKE_LIBRARY)
#     or "missing" (it names a path that does not exist).
#   - The fixture must print "RESULT: ok"; VM and native lanes share the same
#     fixture and expectations, so their observable behavior must match.
# Ownership/Lifetime: The native lane writes OUT_EXE into the build tree.
# Links: src/tests/fixtures/runtime/test_services_platform.zia,
#        src/tests/runtime/RTServicesFakeSteamApi.c,
#        docs/adr/0352-platform-services-runtime-loaded-providers.md
#
#===----------------------------------------------------------------------===#

foreach (_required IN ITEMS MODE ZANNA_EXE TEST_FILE FAKE_LIBRARY EXPECT)
    if (NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} must be provided")
    endif ()
endforeach ()

set(ENV{ZANNA_SERVICES_TEST_EXPECT} "${EXPECT}")
if (EXPECT STREQUAL "missing")
    set(ENV{ZANNA_SERVICES_STEAM_LIBRARY} "${FAKE_LIBRARY}.missing")
elseif (EXPECT STREQUAL "available")
    set(ENV{ZANNA_SERVICES_STEAM_LIBRARY} "${FAKE_LIBRARY}")
else ()
    message(FATAL_ERROR "EXPECT must be 'available' or 'missing', got '${EXPECT}'")
endif ()

if (MODE STREQUAL "vm")
    execute_process(
            COMMAND "${ZANNA_EXE}" run "${TEST_FILE}"
            TIMEOUT 30
            RESULT_VARIABLE _run_rc
            OUTPUT_VARIABLE _run_out
            ERROR_VARIABLE _run_err)
elseif (MODE STREQUAL "native")
    if (NOT DEFINED OUT_EXE)
        message(FATAL_ERROR "OUT_EXE must be provided for the native lane")
    endif ()
    execute_process(
            COMMAND "${ZANNA_EXE}" build "${TEST_FILE}" -o "${OUT_EXE}" --quiet-warnings
            RESULT_VARIABLE _build_rc
            OUTPUT_VARIABLE _build_out
            ERROR_VARIABLE _build_err)
    if (NOT _build_rc EQUAL 0)
        message(FATAL_ERROR "native build failed\nstdout:\n${_build_out}\nstderr:\n${_build_err}")
    endif ()
    execute_process(
            COMMAND "${OUT_EXE}"
            TIMEOUT 30
            RESULT_VARIABLE _run_rc
            OUTPUT_VARIABLE _run_out
            ERROR_VARIABLE _run_err)
else ()
    message(FATAL_ERROR "MODE must be 'vm' or 'native', got '${MODE}'")
endif ()

if (NOT _run_out MATCHES "RESULT: ok")
    message(FATAL_ERROR
            "services fixture (${MODE}, ${EXPECT}) failed (exit ${_run_rc})\n"
            "stdout:\n${_run_out}\nstderr:\n${_run_err}")
endif ()
