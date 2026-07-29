# ZannaGFX - Cross-Platform Software 2D Graphics Library
# Part of the Zanna project

# Can be built standalone or via add_subdirectory
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # Standalone build
    cmake_minimum_required(VERSION 3.10)
    project(ZannaGFX VERSION 1.0.0 LANGUAGES C)
    set(VGFX_STANDALONE TRUE)

    # C11 standard (Zanna sets this globally)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)

    # Warning flags (Zanna sets these globally)
    if (MSVC)
        add_compile_options(/W4)
    else ()
        add_compile_options(-Wall -Wextra -Wpedantic)
    endif ()

    # Options for standalone build
    option(VGFX_BUILD_TESTS "Build ZannaGFX tests" ON)
    option(VGFX_BUILD_EXAMPLES "Build ZannaGFX examples" ON)
else ()
    # Integrated into Zanna build
    set(VGFX_STANDALONE FALSE)

    # Use Zanna's test option
    set(VGFX_BUILD_TESTS ${ZANNA_BUILD_TESTING})
    set(VGFX_BUILD_EXAMPLES ${BUILD_EXAMPLES})
endif ()

include(GNUInstallDirs)

# Core source files (platform-independent)
set(VGFX_CORE_SOURCES
        src/vgfx.c
        src/vgfx_draw.c
        src/vgfx_embed_channel.c
)

set(ZANNAGFX_AVAILABLE OFF CACHE INTERNAL "Whether ZannaGFX is available in this build" FORCE)
set(ZANNAGFX_UNAVAILABLE_REASON "graphics support disabled" CACHE INTERNAL
        "Reason ZannaGFX is unavailable" FORCE)
set(ZANNAGFX_BACKEND "disabled" CACHE INTERNAL "Selected ZannaGFX backend" FORCE)
set(ZANNAGFX_AUTO_HAS_X11 OFF CACHE INTERNAL "Whether Linux AUTO contains X11" FORCE)

if (NOT DEFINED ZANNA_GRAPHICS_MODE)
    message(FATAL_ERROR "ZANNA_GRAPHICS_MODE must be defined before adding src/lib/graphics")
endif ()

if (ZANNA_GRAPHICS_MODE STREQUAL "OFF")
    message(STATUS "ZannaGFX: disabled by ZANNA_GRAPHICS_MODE=OFF")
    set(ZANNAGFX_UNAVAILABLE_REASON "disabled by ZANNA_GRAPHICS_MODE=OFF" CACHE INTERNAL
            "Reason ZannaGFX is unavailable" FORCE)
    return()
endif ()

if (NOT DEFINED ZANNA_GRAPHICS_BACKEND)
    message(FATAL_ERROR "ZANNA_GRAPHICS_BACKEND must be defined before adding src/lib/graphics")
endif ()

# Platform detection and source selection
if (ZANNA_GRAPHICS_BACKEND STREQUAL "HEADLESS")
    message(STATUS "ZannaGFX: Building dependency-free headless backend")
    set(VGFX_PLATFORM_SOURCES src/vgfx_platform_mock.c)
    set(VGFX_PLATFORM_LIBS "")
    set(ZANNAGFX_BACKEND "Headless" CACHE INTERNAL "Selected ZannaGFX backend" FORCE)
elseif ((ZANNA_GRAPHICS_BACKEND STREQUAL "X11" OR
         ZANNA_GRAPHICS_BACKEND STREQUAL "WAYLAND") AND (APPLE OR WIN32))
    message(FATAL_ERROR
            "ZannaGFX: ${ZANNA_GRAPHICS_BACKEND} backend is supported only on Linux/Unix hosts")
elseif (ZANNA_GRAPHICS_BACKEND STREQUAL "AUTO" AND UNIX AND NOT APPLE)
    message(STATUS "ZannaGFX: Building Linux runtime selector (Wayland first, X11 fallback)")
    find_package(Threads REQUIRED)
    find_package(X11 QUIET)
    set(VGFX_PLATFORM_SOURCES
            src/vgfx_platform_linux_auto.c
            src/vgfx_platform_wayland.c
            src/vgfx_wayland_activation.c
            src/vgfx_wayland_connection.c
            src/vgfx_wayland_csd.c
            src/vgfx_wayland_cursor.c
            src/vgfx_wayland_data.c
            src/vgfx_wayland_input.c
            src/vgfx_wayland_loader.c
            src/vgfx_wayland_protocol.c
            src/vgfx_wayland_relative.c
            src/vgfx_wayland_scale.c
            src/vgfx_wayland_shell.c
            src/vgfx_wayland_shm.c
            src/vgfx_wayland_text_input.c)
    set_property(SOURCE src/vgfx_platform_wayland.c APPEND PROPERTY
            COMPILE_OPTIONS "-include${CMAKE_CURRENT_SOURCE_DIR}/src/vgfx_platform_prefix_wayland.h")
    set(VGFX_PLATFORM_LIBS ${CMAKE_DL_LIBS} Threads::Threads)
    if (X11_FOUND)
        list(APPEND VGFX_PLATFORM_SOURCES src/vgfx_platform_linux.c)
        list(APPEND VGFX_PLATFORM_LIBS ${X11_LIBRARIES})
        set_property(SOURCE src/vgfx_platform_linux.c APPEND PROPERTY
                COMPILE_OPTIONS "-include${CMAKE_CURRENT_SOURCE_DIR}/src/vgfx_platform_prefix_x11.h")
        set(VGFX_AUTO_HAS_X11 ON)
        set(ZANNAGFX_AUTO_HAS_X11 ON CACHE INTERNAL "Whether Linux AUTO contains X11" FORCE)
        include_directories(${X11_INCLUDE_DIR})
    else ()
        message(STATUS "ZannaGFX: X11 development files unavailable; AUTO will use Wayland only")
    endif ()
    set(ZANNAGFX_BACKEND "LinuxAuto" CACHE INTERNAL "Selected ZannaGFX backend" FORCE)
elseif (ZANNA_GRAPHICS_BACKEND STREQUAL "WAYLAND" OR
        (ZANNA_GRAPHICS_BACKEND STREQUAL "NATIVE" AND UNIX AND NOT APPLE))
    message(STATUS "ZannaGFX: Building for Linux (native Wayland backend)")
    set(VGFX_PLATFORM_SOURCES
            src/vgfx_platform_wayland.c
            src/vgfx_wayland_activation.c
            src/vgfx_wayland_connection.c
            src/vgfx_wayland_csd.c
            src/vgfx_wayland_cursor.c
            src/vgfx_wayland_data.c
            src/vgfx_wayland_input.c
            src/vgfx_wayland_loader.c
            src/vgfx_wayland_protocol.c
            src/vgfx_wayland_relative.c
            src/vgfx_wayland_scale.c
            src/vgfx_wayland_shell.c
            src/vgfx_wayland_shm.c
            src/vgfx_wayland_text_input.c)
    set(VGFX_PLATFORM_LIBS ${CMAKE_DL_LIBS})
    set(ZANNAGFX_BACKEND "Wayland" CACHE INTERNAL "Selected ZannaGFX backend" FORCE)
elseif (APPLE)
    message(STATUS "ZannaGFX: Building for macOS (Cocoa backend)")
    set(VGFX_PLATFORM_SOURCES src/vgfx_platform_macos.m)
    set(VGFX_PLATFORM_LIBS "-framework Cocoa")
    set(ZANNAGFX_BACKEND "Cocoa" CACHE INTERNAL "Selected ZannaGFX backend" FORCE)
    enable_language(OBJC)
elseif (UNIX)
    find_package(X11 QUIET)
    find_package(Threads REQUIRED)
    if (NOT X11_FOUND)
        set(ZANNAGFX_UNAVAILABLE_REASON
                "X11 not found; install libx11-dev/libX11-devel or set ZANNA_GRAPHICS_MODE=OFF"
                CACHE INTERNAL "Reason ZannaGFX is unavailable" FORCE)
        if (ZANNA_GRAPHICS_MODE STREQUAL "REQUIRE" OR
            ZANNA_GRAPHICS_BACKEND STREQUAL "NATIVE" OR
            ZANNA_GRAPHICS_BACKEND STREQUAL "X11")
            message(FATAL_ERROR
                    "ZannaGFX: X11 not found and ZANNA_GRAPHICS_MODE=REQUIRE.\n"
                    "Install libx11-dev (Debian/Ubuntu) or libX11-devel (Fedora/RHEL),\n"
                    "or reconfigure with -DZANNA_GRAPHICS_MODE=OFF.")
        endif ()
        message(STATUS "ZannaGFX: disabled (${ZANNAGFX_UNAVAILABLE_REASON})")
        return()
    endif ()
    message(STATUS "ZannaGFX: Building for Linux (X11 backend)")
    set(VGFX_PLATFORM_SOURCES src/vgfx_platform_linux.c)
    set(VGFX_PLATFORM_LIBS ${X11_LIBRARIES} Threads::Threads)
    set(ZANNAGFX_BACKEND "X11" CACHE INTERNAL "Selected ZannaGFX backend" FORCE)
    include_directories(${X11_INCLUDE_DIR})
elseif (WIN32)
    message(STATUS "ZannaGFX: Building for Windows (Win32 backend)")
    set(VGFX_PLATFORM_SOURCES src/vgfx_platform_win32.c)
    set(VGFX_PLATFORM_LIBS user32 gdi32 imm32)
    set(ZANNAGFX_BACKEND "Win32" CACHE INTERNAL "Selected ZannaGFX backend" FORCE)
else ()
    message(FATAL_ERROR "ZannaGFX: Unsupported platform")
endif ()

# Static library target
add_library(zannagfx STATIC
        ${VGFX_CORE_SOURCES}
        ${VGFX_PLATFORM_SOURCES}
)

target_compile_features(zannagfx PUBLIC c_std_11)

if (ZANNAGFX_BACKEND STREQUAL "Wayland")
    target_compile_definitions(zannagfx PUBLIC ZANNA_GRAPHICS_WAYLAND)
elseif (ZANNAGFX_BACKEND STREQUAL "LinuxAuto")
    target_compile_definitions(zannagfx PUBLIC ZANNA_GRAPHICS_LINUX_AUTO)
    if (VGFX_AUTO_HAS_X11)
        target_compile_definitions(zannagfx PRIVATE VGFX_AUTO_HAS_X11)
    endif ()
endif ()

# Public include directory
target_include_directories(zannagfx
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# MSVC: force synchronous PDB writes and UTF-8 source encoding
if (MSVC)
    target_compile_options(zannagfx PRIVATE /utf-8 /FS)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
        CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    target_compile_options(zannagfx PRIVATE -mno-outline-atomics)
endif ()

# POSIX feature test macros for Linux (posix_memalign, etc.)
if (NOT WIN32 AND NOT APPLE)
    target_compile_definitions(zannagfx PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
endif ()

# Link platform-specific libraries
target_link_libraries(zannagfx PRIVATE ${VGFX_PLATFORM_LIBS})
if (UNIX AND NOT APPLE)
    # shm_open lives in librt on older glibc; harmless where merged into libc.
    target_link_libraries(zannagfx PRIVATE rt)
endif ()

# Set output name
set_target_properties(zannagfx PROPERTIES
        OUTPUT_NAME "zannagfx"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

set(ZANNAGFX_AVAILABLE ON CACHE INTERNAL "Whether ZannaGFX is available in this build" FORCE)
set(ZANNAGFX_UNAVAILABLE_REASON "" CACHE INTERNAL "Reason ZannaGFX is unavailable" FORCE)

# Subdirectories (only if options enabled)
if (VGFX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (VGFX_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

# Install rules (only for standalone builds)
if (VGFX_STANDALONE)
    # Install library
    install(TARGETS zannagfx
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif ()

install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
)

# Summary
message(STATUS "ZannaGFX Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  C Standard: C${CMAKE_C_STANDARD}")
message(STATUS "  Build Tests: ${VGFX_BUILD_TESTS}")
message(STATUS "  Build Examples: ${VGFX_BUILD_EXAMPLES}")
message(STATUS "  Standalone Build: ${VGFX_STANDALONE}")
