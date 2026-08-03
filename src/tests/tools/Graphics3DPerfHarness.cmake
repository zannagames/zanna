# Graphics3D perf harness.
#
# Required -D arguments:
#   ZANNA_EXE    Path to the zanna executable
#   WORKING_DIR  Directory containing the probe script
#   SCRIPT       Probe script filename
#   BACKEND      ZANNA_3D_BACKEND value
#   NAME         Stable fixture/probe name for log output
#
# Optional -D arguments:
#   MAX_AVG_MS   Wall-clock ceiling for the reported per-frame average. Pick
#                generous order-of-magnitude bounds (battery E-cores can run
#                several times slower than AC benchmarks); the goal is to
#                catch algorithmic regressions, not to be a benchmark.
#   MAX_SETUP_MS Wall-clock ceiling for the reported setup time.

foreach (required_var IN ITEMS ZANNA_EXE WORKING_DIR SCRIPT BACKEND NAME)
    if (NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "Graphics3DPerfHarness: missing ${required_var}")
    endif ()
endforeach ()

execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "ZANNA_3D_BACKEND=${BACKEND}" "${ZANNA_EXE}" run "${SCRIPT}"
        WORKING_DIRECTORY "${WORKING_DIR}"
        RESULT_VARIABLE probe_result
        OUTPUT_VARIABLE probe_stdout
        ERROR_VARIABLE probe_stderr
        TIMEOUT 120
)

if (NOT "${probe_stdout}" STREQUAL "")
    message(STATUS "${probe_stdout}")
endif ()
if (NOT "${probe_stderr}" STREQUAL "")
    message(STATUS "${probe_stderr}")
endif ()
if (NOT probe_result EQUAL 0)
    message(FATAL_ERROR "Graphics3DPerfHarness: probe exited with ${probe_result}")
endif ()

string(REGEX MATCH "PERF: [^\r\n]*" perf_line "${probe_stdout}")
if ("${perf_line}" STREQUAL "")
    message(FATAL_ERROR "Graphics3DPerfHarness: missing PERF line")
endif ()

function(extract_metric metric_name out_var)
    string(REGEX MATCH "(^| )${metric_name}=([^ ]+)" metric_match "${perf_line}")
    if ("${metric_match}" STREQUAL "")
        message(FATAL_ERROR "Graphics3DPerfHarness: missing ${metric_name} in '${perf_line}'")
    endif ()
    set(${out_var} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

extract_metric("backend" reported_backend)
extract_metric("setup_ms" setup_ms)
extract_metric("frames" frames)
extract_metric("elapsed_ms" elapsed_ms)
extract_metric("avg_ms" avg_ms)
extract_metric("fps" fps)
extract_metric("draw_count" draw_count)
extract_metric("visible_nodes" visible_nodes)
extract_metric("entities" entities)
extract_metric("bodies" bodies)
extract_metric("stream_bytes" stream_bytes)

if (NOT "${reported_backend}" STREQUAL "${BACKEND}")
    message(FATAL_ERROR
            "Graphics3DPerfHarness: backend mismatch, got ${reported_backend}, expected ${BACKEND}")
endif ()
if (NOT frames GREATER 0)
    message(FATAL_ERROR "Graphics3DPerfHarness: frames must be positive")
endif ()
if (NOT elapsed_ms GREATER 0)
    message(FATAL_ERROR "Graphics3DPerfHarness: elapsed_ms must be positive")
endif ()
if (NOT avg_ms GREATER 0)
    message(FATAL_ERROR "Graphics3DPerfHarness: avg_ms must be positive")
endif ()
if (NOT fps GREATER 0)
    message(FATAL_ERROR "Graphics3DPerfHarness: fps must be positive")
endif ()
if (NOT draw_count GREATER 0)
    message(FATAL_ERROR "Graphics3DPerfHarness: draw_count must be positive")
endif ()
if (NOT visible_nodes GREATER 0)
    message(FATAL_ERROR "Graphics3DPerfHarness: visible_nodes must be positive")
endif ()
if (NOT entities GREATER 0)
    message(FATAL_ERROR "Graphics3DPerfHarness: entities must be positive")
endif ()
if (NOT bodies GREATER 0)
    message(FATAL_ERROR "Graphics3DPerfHarness: bodies must be positive")
endif ()
if (NOT stream_bytes GREATER 0)
    message(FATAL_ERROR "Graphics3DPerfHarness: stream_bytes must be positive")
endif ()

if (DEFINED MAX_AVG_MS AND NOT "${MAX_AVG_MS}" STREQUAL "")
    if (avg_ms GREATER "${MAX_AVG_MS}")
        message(FATAL_ERROR
                "Graphics3DPerfHarness: avg_ms ${avg_ms} exceeds the ${MAX_AVG_MS} ms ceiling "
                "(order-of-magnitude regression guard; see the budget note above before raising)")
    endif ()
endif ()
if (DEFINED MAX_SETUP_MS AND NOT "${MAX_SETUP_MS}" STREQUAL "")
    extract_metric("setup_ms" measured_setup_ms)
    if (measured_setup_ms GREATER "${MAX_SETUP_MS}")
        message(FATAL_ERROR
                "Graphics3DPerfHarness: setup_ms ${measured_setup_ms} exceeds the "
                "${MAX_SETUP_MS} ms ceiling")
    endif ()
endif ()

message("HARNESS: name=${NAME} backend=${reported_backend} frames=${frames} avg_ms=${avg_ms} fps=${fps} draw_count=${draw_count} visible_nodes=${visible_nodes} entities=${entities} bodies=${bodies} stream_bytes=${stream_bytes}")
