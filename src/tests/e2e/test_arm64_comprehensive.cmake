# End-to-end test for ARM64 native compilation with output verification
# Compiles BASIC to native ARM64, runs it, and compares output to golden file

cmake_minimum_required(VERSION 3.16)

# Detect architecture
execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE HOST_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Skip if not on ARM64 macOS
if (NOT HOST_ARCH STREQUAL "arm64")
    message(STATUS "Skipping ARM64 comprehensive test (not on ARM64)")
    return()
endif ()

# Required variables
if (NOT DEFINED ILC)
    message(FATAL_ERROR "ILC variable must be set")
endif ()
if (NOT DEFINED RUNTIME_LIB)
    message(FATAL_ERROR "RUNTIME_LIB variable must be set")
endif ()
if (NOT DEFINED REGEX_ENGINE_LIB)
    message(FATAL_ERROR "REGEX_ENGINE_LIB variable must be set")
endif ()
if (NOT DEFINED BAS_FILE)
    message(FATAL_ERROR "BAS_FILE variable must be set")
endif ()
if (NOT DEFINED GOLDEN)
    message(FATAL_ERROR "GOLDEN variable must be set")
endif ()

# Get base name for temp files
get_filename_component(BASE_NAME ${BAS_FILE} NAME_WE)
set(IL_FILE "/tmp/comprehensive_${BASE_NAME}.il")
set(ASM_FILE "/tmp/comprehensive_${BASE_NAME}.s")
set(OBJ_FILE "/tmp/comprehensive_${BASE_NAME}.o")
set(EXE_FILE "/tmp/comprehensive_${BASE_NAME}_native")
set(OUT_FILE "/tmp/comprehensive_${BASE_NAME}.out")

# Step 1: Compile BASIC to IL
message(STATUS "Compiling ${BAS_FILE} to IL...")
execute_process(
        COMMAND ${ILC} front basic -emit-il ${BAS_FILE}
        OUTPUT_FILE ${IL_FILE}
        RESULT_VARIABLE RESULT
        ERROR_VARIABLE STDERR
)
if (NOT RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to compile BASIC to IL:\n${STDERR}")
endif ()

# Step 2: Compile IL to ARM64 assembly
message(STATUS "Compiling IL to ARM64 assembly...")
execute_process(
        COMMAND ${ILC} codegen arm64 ${IL_FILE} -S ${ASM_FILE}
        RESULT_VARIABLE RESULT
        ERROR_VARIABLE STDERR
)
if (NOT RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to compile IL to ARM64:\n${STDERR}")
endif ()

# Step 3: Assemble
message(STATUS "Assembling ${ASM_FILE}...")
execute_process(
        COMMAND as ${ASM_FILE} -o ${OBJ_FILE}
        RESULT_VARIABLE RESULT
        ERROR_VARIABLE STDERR
)
if (NOT RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to assemble:\n${STDERR}")
endif ()

# Locate zannaaud (audio backend) — derived from RUNTIME_LIB path
get_filename_component(_RT_DIR ${RUNTIME_LIB} DIRECTORY)
get_filename_component(_BUILD_DIR ${_RT_DIR} DIRECTORY ABSOLUTE)
set(ZANNAAUD_LIB "${_BUILD_DIR}/lib/libzannaaud.a")

# Step 4: Link
message(STATUS "Linking ${OBJ_FILE} with runtime...")
set(_LINK_CMD clang++ ${OBJ_FILE} ${RUNTIME_LIB} ${REGEX_ENGINE_LIB})
if (EXISTS ${ZANNAAUD_LIB})
    list(APPEND _LINK_CMD ${ZANNAAUD_LIB})
endif ()
# macOS: AudioToolbox framework for audio backend
if (APPLE)
    list(APPEND _LINK_CMD -framework AudioToolbox)
endif ()
list(APPEND _LINK_CMD -o ${EXE_FILE})
execute_process(
        COMMAND ${_LINK_CMD}
        RESULT_VARIABLE RESULT
        ERROR_VARIABLE STDERR
)
if (NOT RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to link:\n${STDERR}")
endif ()

# Step 5: Run the executable
message(STATUS "Running ${EXE_FILE}...")
execute_process(
        COMMAND ${EXE_FILE}
        OUTPUT_FILE ${OUT_FILE}
        ERROR_VARIABLE STDERR
        RESULT_VARIABLE RESULT
        TIMEOUT 60
)
if (NOT RESULT EQUAL 0)
    message(FATAL_ERROR "Execution failed with code ${RESULT}:\n${STDERR}")
endif ()

# Step 6: Compare output to golden file
message(STATUS "Comparing output to golden file...")
execute_process(
        COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT_FILE} ${GOLDEN}
        RESULT_VARIABLE DIFF_RESULT
)
if (NOT DIFF_RESULT EQUAL 0)
    # Show diff on failure
    execute_process(
            COMMAND diff -u ${GOLDEN} ${OUT_FILE}
            OUTPUT_VARIABLE DIFF_OUTPUT
    )
    message(FATAL_ERROR "Output mismatch!\n${DIFF_OUTPUT}")
endif ()

message(STATUS "ARM64 comprehensive test PASSED: ${BASE_NAME}")
