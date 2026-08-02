# ZannaGFX Test Suite
# Builds tests using mock platform backend for deterministic testing

cmake_minimum_required(VERSION 3.15)

# ============================================================================
# Mock Platform Library
# ============================================================================
# This library is identical to zannagfx but uses the mock backend instead
# of real OS windowing systems. This allows tests to run without display.

add_library(zannagfx_mock STATIC
        ../src/vgfx_embed_channel.c
        # Core implementation (same as main library)
        ../src/vgfx.c
        ../src/vgfx_draw.c

        # Mock platform backend (instead of macos/linux/win32)
        ../src/vgfx_platform_mock.c
)

target_include_directories(zannagfx_mock PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(zannagfx_mock PUBLIC c_std_11)

# POSIX feature test macros for Linux (posix_memalign, etc.)
if (NOT WIN32 AND NOT APPLE)
    target_compile_definitions(zannagfx_mock PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
    target_link_libraries(zannagfx_mock PRIVATE rt)
endif ()

# Compiler warnings
if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(zannagfx_mock PRIVATE
            -Wall -Wextra -Wpedantic
            -Wno-unused-parameter
    )
endif ()

# ============================================================================
# Test Helper Function
# ============================================================================

function(add_vgfx_test test_name source_file)
    add_executable(${test_name} ${source_file})
    target_link_libraries(${test_name} PRIVATE zannagfx_mock)
    target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # Add to CTest
    add_test(NAME ${test_name} COMMAND ${test_name})

    # Set test properties for better output
    set_tests_properties(${test_name} PROPERTIES
            TIMEOUT 10
            LABELS "unit"
    )
endfunction()

# ============================================================================
# Test Executables
# ============================================================================

add_vgfx_test(test_window test_window.c)
add_vgfx_test(test_pixels test_pixels.c)
add_vgfx_test(test_drawing test_drawing.c)
add_vgfx_test(test_input test_input.c)
add_vgfx_test(test_embed_channel test_embed_channel.c)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_wayland_loader
            test_wayland_loader.c
            ../src/vgfx_wayland_activation.c
            ../src/vgfx_wayland_connection.c
            ../src/vgfx_wayland_cursor.c
            ../src/vgfx_wayland_data.c
            ../src/vgfx_wayland_input.c
            ../src/vgfx_wayland_loader.c
            ../src/vgfx_wayland_protocol.c
            ../src/vgfx_wayland_relative.c
            ../src/vgfx_wayland_scale.c
            ../src/vgfx_wayland_shell.c
            ../src/vgfx_wayland_shm.c
            ../src/vgfx_wayland_text_input.c)
    target_include_directories(test_wayland_loader PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_wayland_loader PRIVATE zannagfx_mock ${CMAKE_DL_LIBS})
    target_compile_features(test_wayland_loader PRIVATE c_std_11)
    add_test(NAME test_wayland_loader COMMAND test_wayland_loader)
    set_tests_properties(test_wayland_loader PROPERTIES
            TIMEOUT 10
            LABELS "unit;gui;wayland;requires_linux")
endif ()

if (ZANNAGFX_BACKEND STREQUAL "Wayland")
    add_executable(test_wayland_backend test_wayland_backend.c)
    target_link_libraries(test_wayland_backend PRIVATE zannagfx)
    target_include_directories(test_wayland_backend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(test_wayland_backend PRIVATE c_std_11)
    add_test(NAME test_wayland_backend COMMAND test_wayland_backend)
    set_tests_properties(test_wayland_backend PROPERTIES
            TIMEOUT 10
            LABELS "integration;gui;wayland;requires_linux;requires_display")
    add_test(NAME test_wayland_backend_csd COMMAND test_wayland_backend)
    set_tests_properties(test_wayland_backend_csd PROPERTIES
            TIMEOUT 10
            ENVIRONMENT "ZANNA_WAYLAND_FORCE_CSD=1"
            LABELS "integration;gui;wayland;requires_linux;requires_display")
endif ()

if (ZANNAGFX_BACKEND STREQUAL "LinuxAuto")
    add_executable(test_linux_auto_backend test_linux_auto_backend.c)
    target_link_libraries(test_linux_auto_backend PRIVATE zannagfx Threads::Threads)
    target_include_directories(test_linux_auto_backend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(test_linux_auto_backend PRIVATE c_std_11)
    add_test(NAME test_linux_auto_wayland COMMAND test_linux_auto_backend wayland)
    set_tests_properties(test_linux_auto_wayland PROPERTIES
            TIMEOUT 10
            RESOURCE_LOCK zanna_display
            LABELS "integration;gui;wayland;requires_linux;requires_display")
    add_test(NAME test_linux_auto_wayland_csd COMMAND test_linux_auto_backend wayland)
    set_tests_properties(test_linux_auto_wayland_csd PROPERTIES
            TIMEOUT 10
            ENVIRONMENT "ZANNA_WAYLAND_FORCE_CSD=1"
            RESOURCE_LOCK zanna_display
            LABELS "integration;gui;wayland;requires_linux;requires_display")
    add_test(NAME test_linux_auto_no_display COMMAND test_linux_auto_backend none)
    set_tests_properties(test_linux_auto_no_display PROPERTIES
            TIMEOUT 10
            ENVIRONMENT "WAYLAND_DISPLAY=;DISPLAY="
            RESOURCE_LOCK zanna_display
            LABELS "unit;gui;requires_linux")
    if (VGFX_AUTO_HAS_X11)
        add_test(NAME test_linux_auto_x11_fallback COMMAND test_linux_auto_backend x11)
        set_tests_properties(test_linux_auto_x11_fallback PROPERTIES
                TIMEOUT 10
                ENVIRONMENT "WAYLAND_DISPLAY=zanna-test-missing-wayland-socket"
                RESOURCE_LOCK zanna_display
                LABELS "integration;gui;x11;requires_linux;requires_display")
    endif ()
endif ()

# ============================================================================
# Test Runner Target
# ============================================================================

# Custom target to run all tests
add_custom_target(run_zannagfx_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        DEPENDS test_window test_pixels test_drawing test_input test_embed_channel
        COMMENT "Running all ZannaGFX tests..."
)
