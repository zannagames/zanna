# End-to-end test for ARM64 native compilation and linking
# Tests that demos can be compiled, assembled, and linked on ARM64
# Only runs on macOS ARM64 or when ARM64_E2E_TESTS is set

cmake_minimum_required(VERSION 3.16)

# Detect architecture using uname since CMAKE_SYSTEM_PROCESSOR may not be set in script mode
execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE HOST_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Skip if not on ARM64 macOS and env var not set
if (NOT HOST_ARCH STREQUAL "arm64" AND NOT DEFINED ENV{ARM64_E2E_TESTS})
    message(STATUS "Skipping ARM64 native link test (not on ARM64 and ARM64_E2E_TESTS not set)")
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
if (NOT DEFINED DEMO_NAME)
    message(FATAL_ERROR "DEMO_NAME variable must be set")
endif ()

set(IL_FILE "/tmp/test_${DEMO_NAME}.il")
set(ASM_FILE "/tmp/test_${DEMO_NAME}.s")
set(OBJ_FILE "/tmp/test_${DEMO_NAME}.o")
set(EXE_FILE "/tmp/test_${DEMO_NAME}_native")

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

# Verify executable exists
if (NOT EXISTS ${EXE_FILE})
    message(FATAL_ERROR "Executable ${EXE_FILE} was not created")
endif ()

message(STATUS "${DEMO_NAME} native binary created successfully: ${EXE_FILE}")
