#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: CMakeLists.txt
# Purpose: Configure Zanna's cross-platform build, generated metadata, and tests.
#
# Key invariants: Build identity is canonical, platform capabilities are explicit,
# and every documented Zanna Studio launch path resolves to the current build.
#
# Ownership/Lifetime: CMake owns generated build-tree files and the ignored
# Zanna Studio compatibility copy under src/zannastudio/bin.
#
# Links: AGENTS.md, docs/internals/architecture.md, docs/internals/testing.md
#
#===----------------------------------------------------------------------===#

cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
endif()

set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Begin: Zanna version wiring (SSOT + snapshots) ----
# Read SSOT files from repo root
file(READ "${CMAKE_SOURCE_DIR}/src/buildmeta/VERSION"     ZANNA_VERSION_STR)
string(STRIP "${ZANNA_VERSION_STR}"         ZANNA_VERSION_STR)
file(READ "${CMAKE_SOURCE_DIR}/src/buildmeta/IL_VERSION"  ZANNA_IL_VERSION_STR)
string(STRIP "${ZANNA_IL_VERSION_STR}"      ZANNA_IL_VERSION_STR)

# Extract numeric MAJOR.MINOR.PATCH for CMake's project() VERSION
string(REGEX REPLACE "^([0-9]+)\\.([0-9]+)\\.([0-9]+).*" "\\1" ZANNA_VERSION_MAJOR "${ZANNA_VERSION_STR}")
string(REGEX REPLACE "^([0-9]+)\\.([0-9]+)\\.([0-9]+).*" "\\2" ZANNA_VERSION_MINOR "${ZANNA_VERSION_STR}")
string(REGEX REPLACE "^([0-9]+)\\.([0-9]+)\\.([0-9]+).*" "\\3" ZANNA_VERSION_PATCH "${ZANNA_VERSION_STR}")
set(ZANNA_SEMVER_NUM "${ZANNA_VERSION_MAJOR}.${ZANNA_VERSION_MINOR}.${ZANNA_VERSION_PATCH}")

# Compute snapshot/source identity from git (empty/unknown if unavailable).
# Source archives and hermetic release builders can provide the immutable
# identity explicitly without requiring a .git directory.
find_package(Git QUIET)
set(ZANNA_SNAPSHOT_STR "")
set(ZANNA_SOURCE_COMMIT "" CACHE STRING
    "Source commit embedded in Zanna build metadata (auto-detected when empty)")
set(ZANNA_SOURCE_STATE "" CACHE STRING
    "Source tree state embedded in build metadata: clean, dirty, or unknown")
mark_as_advanced(ZANNA_SOURCE_COMMIT ZANNA_SOURCE_STATE)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --match "v[0-9]*" --long --dirty
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE _git_r
    OUTPUT_VARIABLE  _git_out
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(_git_r EQUAL 0)
    string(REGEX REPLACE "^v" "" ZANNA_SNAPSHOT_STR "${_git_out}")  # drop leading 'v'
  endif()
  if(ZANNA_SOURCE_COMMIT STREQUAL "")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --verify HEAD
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      RESULT_VARIABLE _git_commit_r
      OUTPUT_VARIABLE _git_commit_out
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_git_commit_r EQUAL 0)
      string(TOLOWER "${_git_commit_out}" ZANNA_SOURCE_COMMIT)
    endif()
  endif()
  if(ZANNA_SOURCE_STATE STREQUAL "")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=normal
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      RESULT_VARIABLE _git_status_r
      OUTPUT_VARIABLE _git_status_out
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_git_status_r EQUAL 0)
      if(_git_status_out STREQUAL "")
        set(ZANNA_SOURCE_STATE "clean")
      else()
        set(ZANNA_SOURCE_STATE "dirty")
      endif()
    endif()
  endif()
endif()
if(ZANNA_SOURCE_STATE STREQUAL "")
  set(ZANNA_SOURCE_STATE "unknown")
endif()
string(LENGTH "${ZANNA_SOURCE_COMMIT}" _zanna_source_commit_length)
if(NOT ZANNA_SOURCE_COMMIT STREQUAL "" AND
   (NOT ZANNA_SOURCE_COMMIT MATCHES "^[0-9a-f]+$" OR
    _zanna_source_commit_length LESS 7 OR _zanna_source_commit_length GREATER 64))
  message(FATAL_ERROR
      "ZANNA_SOURCE_COMMIT must be 7-64 lowercase hexadecimal characters")
endif()
if(NOT ZANNA_SOURCE_STATE MATCHES "^(clean|dirty|unknown)$")
  message(FATAL_ERROR "ZANNA_SOURCE_STATE must be clean, dirty, or unknown")
endif()
# ---- End: Zanna version wiring ----
project(Zanna LANGUAGES CXX VERSION ${ZANNA_SEMVER_NUM})
enable_language(C)
# Enable Objective-C for macOS (needed by ZannaGFX)
if(APPLE)
    enable_language(OBJC)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Runtime namespace transition flag: dual-publish legacy rt_* aliases alongside
# canonical Zanna.* names. Default ON to preserve compatibility while frontends
# and examples migrate to emitting canonical Zanna.* names. Users can re-enable
# legacy aliases explicitly via -DZANNA_RUNTIME_NS_DUAL=ON when loading older IL.
option(ZANNA_RUNTIME_NS_DUAL "Publish legacy rt_* runtime aliases alongside Zanna.*" ON)

# Compute numeric define for headers and C++ sources (1/0)
if(ZANNA_RUNTIME_NS_DUAL)
  set(ZANNA_RUNTIME_NS_DUAL_DEFINE 1)
else()
  set(ZANNA_RUNTIME_NS_DUAL_DEFINE 0)
endif()

# Emit the generated version header
file(MAKE_DIRECTORY
  "${CMAKE_BINARY_DIR}/generated/zanna"
  "${CMAKE_BINARY_DIR}/generated/zanna/platform"
  "${CMAKE_BINARY_DIR}/generated/zanna/runtime"
)
configure_file(
  "${CMAKE_SOURCE_DIR}/include/zanna/version.hpp.in"
  "${CMAKE_BINARY_DIR}/generated/zanna/version.hpp"
  @ONLY
)
install(FILES "${CMAKE_BINARY_DIR}/generated/zanna/version.hpp"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna)

function(zanna_require_feature_mode var_name)
  if(NOT DEFINED ${var_name})
    message(FATAL_ERROR "Required feature mode variable '${var_name}' is not defined")
  endif()

  string(TOUPPER "${${var_name}}" _mode)
  if(NOT _mode STREQUAL "AUTO" AND NOT _mode STREQUAL "REQUIRE" AND NOT _mode STREQUAL "OFF")
    message(FATAL_ERROR
      "${var_name} must be one of AUTO, REQUIRE, or OFF (got '${${var_name}}')")
  endif()
  set(${var_name} "${_mode}" CACHE STRING "" FORCE)
endfunction()

# No external scripting dependencies required (Python removed).

option(ZANNA_INSTALL_TUI "Install TUI apps/libraries" OFF)
option(ZANNA_INSTALL_ZANNASTUDIO "Build and install Zanna Studio with toolchain installers" ON)
set(ZANNA_IDE_ARCH "" CACHE STRING
    "Zanna Studio native target architecture: x64 or arm64 (empty selects host architecture)")
set_property(CACHE ZANNA_IDE_ARCH PROPERTY STRINGS "" x64 arm64)
option(ZANNA_BUILD_TESTING "Enable CTest-based tests" ON)
option(ZANNA_ENABLE_INDIVIDUAL_BASIC_TO_IL_GOLDEN_TESTS
       "Register one CTest per BASIC-to-IL golden case in addition to the batch test" OFF)
set(ZANNA_BASIC_TO_IL_GOLDEN_BATCH_SHARDS "8" CACHE STRING
    "Number of CTest shards used for the batched BASIC-to-IL golden suite")
option(ZANNA_ENABLE_WINDOWS_INSTALLER_E2E "Run the generated Windows toolchain installer end-to-end smoke test" OFF)
option(ZANNA_INSTALLER_ENABLE_TEST_HOOKS
       "Enable destructive Windows installer fault-injection hooks (test builds only)" OFF)
mark_as_advanced(ZANNA_INSTALLER_ENABLE_TEST_HOOKS)
option(ZANNA_BUILD_VSCODE_VSIX "Build and install the packaged Zia VS Code extension" OFF)
option(ZANNA_INSTALL_MSVC_RUNTIME
       "Install the app-local MSVC runtime DLLs required by the Windows toolchain"
       ON)
option(ZANNA_VM_THREADED "Enable threaded VM dispatch (faster on GCC/Clang)" ON)
option(ZANNA_VM_TAILCALL "Enable tailcall" ON)
option(ZANNA_VM_OPCOUNTS "Enable op counters" ON)
set(ZANNA_GRAPHICS_MODE "AUTO" CACHE STRING
    "Graphics capability mode: AUTO, REQUIRE, or OFF")
set(ZANNA_GRAPHICS_BACKEND "AUTO" CACHE STRING
    "Graphics backend: AUTO, NATIVE, WAYLAND, X11, or HEADLESS")
set(ZANNA_AUDIO_MODE "AUTO" CACHE STRING
    "Audio capability mode: AUTO, REQUIRE, or OFF")
set_property(CACHE ZANNA_GRAPHICS_MODE PROPERTY STRINGS AUTO REQUIRE OFF)
set_property(CACHE ZANNA_GRAPHICS_BACKEND PROPERTY STRINGS AUTO NATIVE WAYLAND X11 HEADLESS)
set_property(CACHE ZANNA_AUDIO_MODE PROPERTY STRINGS AUTO REQUIRE OFF)
zanna_require_feature_mode(ZANNA_GRAPHICS_MODE)
zanna_require_feature_mode(ZANNA_AUDIO_MODE)
string(TOUPPER "${ZANNA_GRAPHICS_BACKEND}" ZANNA_GRAPHICS_BACKEND)
if(NOT ZANNA_GRAPHICS_BACKEND STREQUAL "AUTO" AND
   NOT ZANNA_GRAPHICS_BACKEND STREQUAL "NATIVE" AND
   NOT ZANNA_GRAPHICS_BACKEND STREQUAL "WAYLAND" AND
   NOT ZANNA_GRAPHICS_BACKEND STREQUAL "X11" AND
   NOT ZANNA_GRAPHICS_BACKEND STREQUAL "HEADLESS")
  message(FATAL_ERROR
    "ZANNA_GRAPHICS_BACKEND must be AUTO, NATIVE, WAYLAND, X11, or HEADLESS (got '${ZANNA_GRAPHICS_BACKEND}')")
endif()
set(ZANNA_GRAPHICS_BACKEND "${ZANNA_GRAPHICS_BACKEND}" CACHE STRING "" FORCE)

# A Windows toolchain installer must run on a clean machine that has never had
# Visual Studio or another VC++ application installed. Keep the SDK libraries
# and executable targets on CMake's consistent dynamic-runtime model, and stage
# the compiler-provided redistributable DLLs beside bin/zanna.exe. The package
# builder independently verifies that every imported MSVC runtime is present.
if(WIN32 AND MSVC AND ZANNA_INSTALL_MSVC_RUNTIME)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
  include(InstallRequiredSystemLibraries)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
  # Keep windows.h from defining min/max macros that break std::min/std::max.
  add_compile_definitions(NOMINMAX)
endif()

# Default to strict builds: treat warnings as errors unless opted out. MSVC ARM64
# emits a wider warning surface for existing cross-platform code, so keep that
# host buildable by default while still allowing explicit opt-in.
set(_IL_WARN_AS_ERROR_DEFAULT ON)
if(MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|AARCH64|arm64|aarch64)$")
  set(_IL_WARN_AS_ERROR_DEFAULT OFF)
endif()
option(IL_WARN_AS_ERROR "Treat warnings as errors" ${_IL_WARN_AS_ERROR_DEFAULT})
option(IL_USE_LLD "Use lld linker if available (GCC/Clang only)" ON)
option(IL_USE_LIBCXX "Use libc++ on Clang (Linux)" OFF)
option(IL_SANITIZE_ADDRESS "Enable address sanitizer" OFF)
option(IL_SANITIZE_UNDEFINED "Enable undefined behavior sanitizer" OFF)
option(IL_SANITIZE_THREAD "Enable thread sanitizer (mutually exclusive with ASAN)" OFF)
set(ZANNA_SANITIZER_TEST_TIMEOUT "600" CACHE STRING
    "Minimum CTest timeout in seconds for explicitly timed sanitizer tests")
option(ZANNA_ENABLE_COVERAGE "Enable Clang source-based coverage instrumentation" OFF)
option(ZANNA_FAST_DEBUG
       "Use light optimization and reduced STL checking for Debug builds of Zanna targets"
       OFF)

if((IL_SANITIZE_ADDRESS OR IL_SANITIZE_UNDEFINED OR IL_SANITIZE_THREAD) AND
   NOT ZANNA_SANITIZER_TEST_TIMEOUT MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR
          "ZANNA_SANITIZER_TEST_TIMEOUT must be a positive integer")
endif()

if(ZANNA_ENABLE_COVERAGE AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "ZANNA_ENABLE_COVERAGE requires Clang or AppleClang")
endif()

set(IL_COMPILE_FLAGS)
set(IL_LINK_FLAGS)

if(MSVC)
  # Global MSVC flags: synchronous PDB writes (prevents parallel build races)
  # and UTF-8 source encoding. These must apply to ALL targets including
  # sub-libraries that don't link zanna_common_opts.
  add_compile_options(/FS /utf-8)
  add_compile_options($<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)

  # MSVC-specific compiler flags
  list(APPEND IL_COMPILE_FLAGS /W4 /permissive-)
  # Suppress common benign warnings
  list(APPEND IL_COMPILE_FLAGS
    /wd4100  # unreferenced formal parameter (like -Wno-unused-parameter)
    /wd4127  # conditional expression is constant
    /wd4201  # nonstandard extension: nameless struct/union
    /wd4458  # declaration hides class member
  )
  if(IL_WARN_AS_ERROR)
    list(APPEND IL_COMPILE_FLAGS /WX)
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # clang-cl diagnoses a broader set of unused entities than cl.exe and maps
    # them into -Werror under /WX. Keep parity with the existing GCC/Clang test
    # policy so Windows builds do not fail on benign dead helpers or assertion-
    # only locals.
    list(APPEND IL_COMPILE_FLAGS
      -Wno-unused-function
      -Wno-unused-variable
      -Wno-unused-but-set-variable
      -Wno-unused-const-variable
      -Wno-unused-private-field
      -Wno-unused-local-typedef
      -Wno-unused-lambda-capture
      -Wno-overloaded-virtual
    )
  endif()
  if(ZANNA_BUILD_TESTING)
    list(APPEND IL_COMPILE_FLAGS
      /wd4189  # local variable initialized but not referenced
      /wd4505  # unreferenced local function
      /wd4702  # unreachable code (SKIP_TEST_NO_FORK returns early on Windows)
      /wd4245  # signed/unsigned mismatch in assignment (tests intentionally pass -1 to unsigned)
      /wd4389  # signed/unsigned mismatch in comparison (rt_len returns signed, strlen returns size_t)
      /wd4456  # declaration hides previous local declaration (test scoping patterns)
      /wd4267  # conversion from size_t to unsigned int
      /wd4611  # setjmp and C++ object destruction interaction
      /wd4244  # conversion from int to char (tolower/toupper in std::transform)
    )
  endif()
  # MSVC-specific defines for secure CRT
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
else()
  # Windows with non-MSVC compiler (e.g., Clang)
  if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
  endif()
  # GCC/Clang compiler flags
  list(APPEND IL_COMPILE_FLAGS -Wall -Wextra -Wpedantic)

  # AArch64 outline atomics introduce libgcc.a helper references such as
  # __aarch64_ldadd* into static archives. Zanna's native linker does not pull
  # compiler runtime archive members, so keep atomics inline for portable
  # self-contained archives.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64|AARCH64)$" AND
     CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    list(APPEND IL_COMPILE_FLAGS -mno-outline-atomics)
  endif()

  # High-value warnings not included in -Wall/-Wextra
  list(APPEND IL_COMPILE_FLAGS
    -Wnull-dereference   # potential null pointer dereference
    -Wcast-qual          # cast drops const/volatile qualifier
    -Wimplicit-fallthrough  # switch fallthrough without [[fallthrough]]
    -Wformat=2           # stricter printf/scanf format string checking
  )

  # Local hides outer declaration. GCC's full -Wshadow additionally flags
  # constructor parameters that deliberately carry a member's name and
  # scoped-enum enumerators matching a global name; Clang, the canonical
  # compiler, flags neither, and a later -Wshadow=local cannot narrow an
  # already-enabled -Wshadow. Select the level per compiler instead.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND IL_COMPILE_FLAGS -Wshadow=local)
  else()
    list(APPEND IL_COMPILE_FLAGS -Wshadow)
  endif()

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND IL_COMPILE_FLAGS -Wunreachable-code)  # dead code after return/throw/break
    list(APPEND IL_COMPILE_FLAGS -Wno-unused-parameter -Wno-unused-private-field -Wno-mismatched-tags)
    # Apple Clang enables -Woverloaded-virtual in -Wall (upstream Clang does not);
    # suppress it globally since visitor-pattern code legitimately hides overloads.
    list(APPEND IL_COMPILE_FLAGS -Wno-overloaded-virtual)
    # VM threaded dispatch uses computed-goto &&label (GCC/Clang extension);
    # -Wpedantic warns on it, but it is intentional and performance-critical.
    list(APPEND IL_COMPILE_FLAGS -Wno-gnu-label-as-value)
    if(IL_SANITIZE_ADDRESS)
      list(APPEND IL_COMPILE_FLAGS -fsanitize=address)
      list(APPEND IL_LINK_FLAGS -fsanitize=address)
    endif()
    if(IL_SANITIZE_UNDEFINED)
      list(APPEND IL_COMPILE_FLAGS -fsanitize=undefined)
      list(APPEND IL_LINK_FLAGS -fsanitize=undefined)
    endif()
    if(IL_SANITIZE_THREAD)
      list(APPEND IL_COMPILE_FLAGS -fsanitize=thread)
      list(APPEND IL_LINK_FLAGS -fsanitize=thread)
    endif()
    if(ZANNA_ENABLE_COVERAGE)
      list(APPEND IL_COMPILE_FLAGS -fprofile-instr-generate -fcoverage-mapping)
      list(APPEND IL_LINK_FLAGS -fprofile-instr-generate)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Keep parity with Clang: suppress benign unused parameter warnings under -Werror
    list(APPEND IL_COMPILE_FLAGS -Wno-unused-parameter)
    # Same rationale as the Clang branch above: visitor-pattern code
    # legitimately hides base-class overloads. GCC, unlike Clang, diagnoses the
    # option itself as inapplicable when it reaches a C translation unit, so
    # scope it to C++ sources.
    list(APPEND IL_COMPILE_FLAGS $<$<COMPILE_LANGUAGE:CXX>:-Wno-overloaded-virtual>)
    # Zanna's own linker consumes these archives when it produces native
    # binaries, and it resolves a fixed libc surface. Several distributions
    # turn on _FORTIFY_SOURCE by default at -O1 and above, which rewrites
    # calls into glibc's `__*_chk` entry points that this surface does not
    # carry. This is a toolchain requirement rather than a style choice, so it
    # applies to every target in the tree, not only those consuming
    # zanna_common_opts.
    add_compile_options(-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
  endif()

  if(IL_WARN_AS_ERROR)
    list(APPEND IL_COMPILE_FLAGS -Werror)
  endif()

  # Symbol visibility: default to hidden so only explicitly exported symbols
  # appear in shared-library export tables, reducing binary size and enabling
  # better LTO dead-code elimination on Linux/macOS (LOW-4).
  # Static archives are unaffected; this only matters for DSOs and executables.
  if(NOT WIN32)
    list(APPEND IL_COMPILE_FLAGS -fvisibility=hidden)
    # -fvisibility-inlines-hidden is a C++/ObjC++-only option. Clang silently
    # ignores it for C sources, but GCC rejects it outright, which becomes a
    # hard error under -Werror. Scope it to C++ translation units.
    list(APPEND IL_COMPILE_FLAGS $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>)
  endif()

  # Test code often has variables/functions/types used only in assertions (compiled out in release builds)
  if(ZANNA_BUILD_TESTING)
    list(APPEND IL_COMPILE_FLAGS -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      list(APPEND IL_COMPILE_FLAGS -Wno-unused-lambda-capture -Wno-unused-local-typedef)
    else()
      # GCC spells the local-typedef warning in the plural and has no
      # lambda-capture equivalent.
      list(APPEND IL_COMPILE_FLAGS -Wno-unused-local-typedefs)
    endif()
  endif()

  if(IL_USE_LLD)
    include(CheckLinkerFlag)
    check_linker_flag(CXX "-fuse-ld=lld" IL_HAS_LLD)
    if(IL_HAS_LLD)
      list(APPEND IL_LINK_FLAGS -fuse-ld=lld)
    endif()
  endif()
endif()

if(ZANNA_FAST_DEBUG)
  if(MSVC)
    # CMake's MSVC Debug defaults (/Od /RTC1 and checked STL iterators) make
    # interpreter- and software-renderer-heavy tests an order of magnitude
    # slower than the -Og configuration used by the macOS build.  /RTC1 is
    # incompatible with optimization, so remove it from the effective Debug
    # flags while this explicitly requested fast-Debug mode is active.  Keep
    # debug symbols, assertions, and the Debug CRT; only compiler optimization
    # and iterator instrumentation change.
    foreach(_zanna_debug_flags_var CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
      set(_zanna_debug_flags "${${_zanna_debug_flags_var}}")
      foreach(_zanna_slow_debug_flag IN ITEMS
          "[/-]RTC[1su]"
          "[/-]Od"
          "[/-]Ob0")
        string(REGEX REPLACE "(^| )${_zanna_slow_debug_flag}($| )" " "
               _zanna_debug_flags "${_zanna_debug_flags}")
      endforeach()
      string(APPEND _zanna_debug_flags " /O1 /Ob1")
      set(${_zanna_debug_flags_var} "${_zanna_debug_flags}")
    endforeach()
    add_compile_definitions("$<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>")
  else()
    list(APPEND IL_COMPILE_FLAGS "$<$<CONFIG:Debug>:-Og>")
    # Full DWARF for thousands of separately linked test executables can make a
    # clean Debug tree tens of gigabytes.  Fast Debug is the routine local/CI
    # configuration, so retain source/line backtraces without emitting variable
    # and type debug records into every object and executable.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      list(APPEND IL_COMPILE_FLAGS "$<$<CONFIG:Debug>:-gline-tables-only>")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      list(APPEND IL_COMPILE_FLAGS "$<$<CONFIG:Debug>:-g1>")
    endif()
  endif()
endif()

add_compile_definitions(
  ZANNA_ENABLE_OOP=1
  ZANNA_VM_TAILCALL=$<BOOL:${ZANNA_VM_TAILCALL}>
  ZANNA_VM_OPCOUNTS=$<BOOL:${ZANNA_VM_OPCOUNTS}>
  ZANNA_RUNTIME_NS_DUAL=$<BOOL:${ZANNA_RUNTIME_NS_DUAL}>
)

if(IL_USE_LIBCXX AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT APPLE)
  list(APPEND IL_COMPILE_FLAGS -stdlib=libc++)
  list(APPEND IL_LINK_FLAGS -stdlib=libc++)
endif()

add_library(zanna_common_opts INTERFACE)
# Generated headers live here (configured below)
target_include_directories(zanna_common_opts INTERFACE
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>)
target_compile_features(zanna_common_opts INTERFACE cxx_std_20)
if(IL_COMPILE_FLAGS)
  target_compile_options(zanna_common_opts INTERFACE ${IL_COMPILE_FLAGS})
endif()

set(_zanna_link_opts ${IL_LINK_FLAGS})
list(FILTER _zanna_link_opts EXCLUDE REGEX "^$")
if(_zanna_link_opts)
  target_link_options(zanna_common_opts INTERFACE ${_zanna_link_opts})
endif()

# Silence benign duplicate-library warnings on macOS early so it applies to all targets
option(ZANNA_SUPPRESS_DUPLIB_WARN "Suppress ld64 duplicate library warnings" ON)
if(APPLE AND ZANNA_SUPPRESS_DUPLIB_WARN)
  add_link_options(-Wl,-no_warn_duplicate_libraries)
endif()

function(zanna_assert_no_directory_link_libraries dir label)
  if("${dir}" STREQUAL ".")
    get_property(_dir_link_libs DIRECTORY PROPERTY LINK_LIBRARIES)
  else()
    get_property(_dir_link_libs DIRECTORY "${dir}" PROPERTY LINK_LIBRARIES)
  endif()
  if(_dir_link_libs)
    message(FATAL_ERROR
      "Directory-scope link_libraries() detected in ${label}. "
      "Please link libraries on their owning targets instead. "
      "LINK_LIBRARIES=${_dir_link_libs}")
  endif()
endfunction()

function(zanna_assert_no_archive_link_paths)
  get_property(_zanna_all_targets GLOBAL PROPERTY TARGETS)
  if(NOT _zanna_all_targets)
    return()
  endif()

  set(_zanna_archive_offenders)
  foreach(_zanna_target IN LISTS _zanna_all_targets)
    foreach(_zanna_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_target_property(_zanna_links "${_zanna_target}" "${_zanna_prop}")
      if(NOT _zanna_links)
        continue()
      endif()

      foreach(_zanna_entry IN LISTS _zanna_links)
        if(NOT _zanna_entry)
          continue()
        endif()

        if(_zanna_entry STREQUAL "optimized" OR _zanna_entry STREQUAL "debug" OR _zanna_entry STREQUAL "general")
          continue()
        endif()

        if(_zanna_entry MATCHES "^\\$<")
          continue()
        endif()

        string(REPLACE "\\" "/" _zanna_normalized "${_zanna_entry}")
        if(_zanna_normalized MATCHES "/[^/]+\\.a$")
          list(APPEND _zanna_archive_offenders "${_zanna_target} -> ${_zanna_entry}")
        endif()
      endforeach()
    endforeach()
  endforeach()

  if(_zanna_archive_offenders)
    list(REMOVE_DUPLICATES _zanna_archive_offenders)
    list(JOIN _zanna_archive_offenders "\n  " _zanna_archive_message)
    message(FATAL_ERROR
      "Targets linked against archive file paths:\n  ${_zanna_archive_message}\n"
      "Link against the producing CMake target or wrap the archive with add_library(IMPORTED).")
  endif()
endfunction()

file(GLOB_RECURSE ALL_CXX_SOURCE_FILES
     src/*.cpp src/*.hpp
     src/runtime/*.cpp src/runtime/*.hpp
     src/runtime/*.c src/runtime/*.h)

add_custom_target(format
  COMMAND clang-format -i ${ALL_CXX_SOURCE_FILES}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

find_program(CPPCHECK_EXECUTABLE cppcheck)
if(CPPCHECK_EXECUTABLE)
  add_custom_target(cppcheck-codegen
    COMMAND ${CPPCHECK_EXECUTABLE}
            --project=${CMAKE_BINARY_DIR}/compile_commands.json
            "--file-filter=src/codegen/*"
            --enable=all
            --inconclusive
            --inline-suppr
            "--suppressions-list=${CMAKE_SOURCE_DIR}/cppcheck-graphics.supp"
            --suppress=missingIncludeSystem
            --suppress=unusedFunction
            --suppress=unmatchedSuppression
            "--template=gcc"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Run cppcheck over src/codegen")

  add_custom_target(cppcheck-graphics
    COMMAND ${CPPCHECK_EXECUTABLE}
            --project=${CMAKE_BINARY_DIR}/compile_commands.json
            "--file-filter=src/runtime/graphics/*"
            --enable=all
            --inconclusive
            --inline-suppr
            --suppress=missingIncludeSystem
            --suppress=unusedFunction
            --suppress=unmatchedSuppression
            "--template=gcc"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Run cppcheck over src/runtime/graphics")

  add_custom_target(cppcheck-runtime
    COMMAND ${CPPCHECK_EXECUTABLE}
            --project=${CMAKE_BINARY_DIR}/compile_commands.json
            "--file-filter=src/runtime/*"
            --enable=warning,performance,portability
            --inconclusive
            --inline-suppr
            "--suppressions-list=${CMAKE_SOURCE_DIR}/cppcheck-runtime.supp"
            --suppress=missingIncludeSystem
            --suppress=unmatchedSuppression
            --error-exitcode=2
            --quiet
            "--relative-paths=${CMAKE_SOURCE_DIR}"
            "--template=gcc"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Run gating cppcheck diagnostics over src/runtime")
endif()

# Convenience cleanup: clean build tree and remove source-generated artifacts.
add_custom_target(clean-all
  COMMAND ${CMAKE_COMMAND} --build . --target clean
  COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/cmake/clean-source.cmake
  COMMENT "Clean build tree and remove source-generated files")

# CI-safe helper: normalize IL version macro in generated header (no Python)
add_custom_target(bump_il
  COMMAND ${CMAKE_COMMAND}
          -DIL_VERSION_FILE=${CMAKE_SOURCE_DIR}/src/buildmeta/IL_VERSION
          -DHEADER_FILE=${CMAKE_BINARY_DIR}/generated/zanna/version.hpp
          -P ${CMAKE_SOURCE_DIR}/cmake/BumpIlVersion.cmake
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  COMMENT "Normalize IL version macro in generated header"
)

# Stronger cleanup that also removes built binaries and CMake cache/files.
add_custom_target(clean-dist
  COMMAND ${CMAKE_COMMAND} --build . --target clean
  COMMAND ${CMAKE_COMMAND} -E rm -rf
          "${CMAKE_BINARY_DIR}/bin"
          "${CMAKE_BINARY_DIR}/lib"
          "${CMAKE_BINARY_DIR}/src"
          "${CMAKE_BINARY_DIR}/tui"
          "${CMAKE_BINARY_DIR}/generated"
          "${CMAKE_BINARY_DIR}/compile_commands.json"
  COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_BINARY_DIR}/CMakeCache.txt"
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/CMakeFiles"
  COMMENT "Remove all build artifacts (including binaries) and CMake cache/files"
)

option(IL_ENABLE_X64_ASM_SYNTAX_CHECK "Check generated x86_64 asm syntax" ON)
option(IL_ENABLE_X64_ASM_ASSEMBLE_LINK "Assemble+link x86_64 asm" ON)
option(IL_ENABLE_X64_NATIVE_RUN "Run x86_64 binaries" ON)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  set(IL_ENABLE_X64_NATIVE_RUN OFF CACHE BOOL "" FORCE)
  set(IL_ENABLE_X64_ASM_ASSEMBLE_LINK OFF CACHE BOOL "" FORCE)
endif()

set(IL_X64_ASM_FLAGS "" CACHE STRING "Extra flags for assembling x86_64")
set(IL_X64_LD_FLAGS  "" CACHE STRING "Extra flags for linking x86_64")
if(APPLE)
  set(IL_X64_ASM_FLAGS "-arch x86_64 -mmacosx-version-min=12.0" CACHE STRING "" FORCE)
  set(IL_X64_LD_FLAGS  "-arch x86_64 -mmacosx-version-min=12.0" CACHE STRING "" FORCE)
endif()

# LOW-3: Include clang-cl (LLVM MSVC-compatible front-end) so Windows builds
# can locate the assembler/linker driver without a separate PATH entry.
find_program(CLANG_EXECUTABLE NAMES clang clang-cl clang-17 clang-16 clang-15)
set(CLANG_FOUND OFF)
if(CLANG_EXECUTABLE)
  set(CLANG_FOUND ON)
endif()
message(STATUS "CLANG_FOUND=${CLANG_FOUND} (${CLANG_EXECUTABLE})")

message(STATUS "IL_ENABLE_X64_ASM_SYNTAX_CHECK=${IL_ENABLE_X64_ASM_SYNTAX_CHECK}")
message(STATUS "IL_ENABLE_X64_ASM_ASSEMBLE_LINK=${IL_ENABLE_X64_ASM_ASSEMBLE_LINK}")
message(STATUS "IL_ENABLE_X64_NATIVE_RUN=${IL_ENABLE_X64_NATIVE_RUN}")
message(STATUS "IL_X64_ASM_FLAGS=${IL_X64_ASM_FLAGS}")
message(STATUS "IL_X64_LD_FLAGS=${IL_X64_LD_FLAGS}")

message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Warn-as-error: ${IL_WARN_AS_ERROR}")
message(STATUS "Linker: lld=${IL_USE_LLD}")
message(STATUS "Coverage: ${ZANNA_ENABLE_COVERAGE}")
message(STATUS "ZANNA_VM_THREADED=${ZANNA_VM_THREADED}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(STATUS "Stdlib (Clang): libc++=${IL_USE_LIBCXX}")
endif()

if(ZANNA_BUILD_TESTING)
  enable_testing()

  # Exclude slow tests by default (run with: ctest -L slow to include)
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/CTestCustom.cmake.in"
    "${CMAKE_BINARY_DIR}/CTestCustom.cmake"
    COPYONLY
  )

  # Keep an explicit recovery target for intentionally discarding CTest's
  # learned timing data and transient logs without penalizing every test run.
  add_custom_target(clean-test-cache
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/Testing"
    COMMENT "Clearing CTest timing data and transient logs"
  )

  # Shell script test - only runs on Unix
  if(NOT WIN32)
    add_test(
      NAME basic_select_case_string
      COMMAND ${CMAKE_SOURCE_DIR}/src/tests/integration/basic/select_case_string.sh
              ${CMAKE_BINARY_DIR}
    )
    set_tests_properties(basic_select_case_string PROPERTIES LABELS golden)

    add_test(
      NAME build_script_concurrency_lock
      COMMAND ${CMAKE_SOURCE_DIR}/src/tests/integration/build/build_zanna_lock.sh
              ${CMAKE_SOURCE_DIR}/scripts/build_zanna_unix.sh
    )
    set_tests_properties(build_script_concurrency_lock PROPERTIES LABELS "build;tools")
  endif()
endif()

# ---- Shared Text Engine ----
# (Must be added before GUI and TUI so both editors link the same implementation)
add_subdirectory(src/common/text)
zanna_assert_no_directory_link_libraries("src/common/text" "src/common/text")

# ---- ZannaGFX library ----
# (Must be added before runtime so rt_graphics can link to it)
add_subdirectory(src/lib/graphics)
zanna_assert_no_directory_link_libraries("src/lib/graphics" "src/lib/graphics")

# ---- ZannaGUI library ----
# (Must be added after graphics since it depends on zannagfx)
if(TARGET zannagfx)
  add_subdirectory(src/lib/gui)
  zanna_assert_no_directory_link_libraries("src/lib/gui" "src/lib/gui")
endif()

# ---- ZannaAUD library ----
# (Must be added before runtime so rt_audio can link to it)
add_subdirectory(src/lib/audio)
zanna_assert_no_directory_link_libraries("src/lib/audio" "src/lib/audio")

add_subdirectory(${CMAKE_SOURCE_DIR}/src/runtime)
zanna_assert_no_directory_link_libraries("src/runtime" "src/runtime")

if(TARGET zannagfx)
  set(ZANNA_BUILD_HAS_GRAPHICS 1)
else()
  set(ZANNA_BUILD_HAS_GRAPHICS 0)
endif()
if(TARGET zannaaud)
  set(ZANNA_BUILD_HAS_AUDIO 1)
else()
  set(ZANNA_BUILD_HAS_AUDIO 0)
endif()
if(TARGET zannagui)
  set(ZANNA_BUILD_HAS_GUI 1)
else()
  set(ZANNA_BUILD_HAS_GUI 0)
endif()

if(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  set(ZANNA_BUILD_NATIVE_LINK_X86_64 1)
elseif(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  set(ZANNA_BUILD_NATIVE_LINK_X86_64 1)
else()
  set(ZANNA_BUILD_NATIVE_LINK_X86_64 0)
endif()

if((APPLE OR WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
  set(ZANNA_BUILD_NATIVE_LINK_AARCH64 1)
else()
  set(ZANNA_BUILD_NATIVE_LINK_AARCH64 0)
endif()

if(WIN32)
  set(ZANNA_HOST_WINDOWS_VALUE 1)
  set(ZANNA_HOST_MACOS_VALUE 0)
  set(ZANNA_HOST_LINUX_VALUE 0)
elseif(APPLE)
  set(ZANNA_HOST_WINDOWS_VALUE 0)
  set(ZANNA_HOST_MACOS_VALUE 1)
  set(ZANNA_HOST_LINUX_VALUE 0)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(ZANNA_HOST_WINDOWS_VALUE 0)
  set(ZANNA_HOST_MACOS_VALUE 0)
  set(ZANNA_HOST_LINUX_VALUE 1)
else()
  set(ZANNA_HOST_WINDOWS_VALUE 0)
  set(ZANNA_HOST_MACOS_VALUE 0)
  set(ZANNA_HOST_LINUX_VALUE 0)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(ZANNA_COMPILER_MSVC_VALUE 1)
  set(ZANNA_COMPILER_CLANG_VALUE 0)
  set(ZANNA_COMPILER_GCC_VALUE 0)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(ZANNA_COMPILER_MSVC_VALUE 0)
  set(ZANNA_COMPILER_CLANG_VALUE 1)
  set(ZANNA_COMPILER_GCC_VALUE 0)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(ZANNA_COMPILER_MSVC_VALUE 0)
  set(ZANNA_COMPILER_CLANG_VALUE 0)
  set(ZANNA_COMPILER_GCC_VALUE 1)
else()
  set(ZANNA_COMPILER_MSVC_VALUE 0)
  set(ZANNA_COMPILER_CLANG_VALUE 0)
  set(ZANNA_COMPILER_GCC_VALUE 0)
endif()

configure_file(
  "${CMAKE_SOURCE_DIR}/include/zanna/platform/Capabilities.hpp.in"
  "${CMAKE_BINARY_DIR}/generated/zanna/platform/Capabilities.hpp"
  @ONLY
)
install(FILES "${CMAKE_BINARY_DIR}/generated/zanna/platform/Capabilities.hpp"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/platform)

set(ZANNA_RUNTIME_COMPONENT_ARCHIVE_COUNT 0)
set(ZANNA_RUNTIME_COMPONENT_ARCHIVE_ENTRIES "")
if(DEFINED ZANNA_RUNTIME_COMPONENT_ARCHIVES)
  list(LENGTH ZANNA_RUNTIME_COMPONENT_ARCHIVES ZANNA_RUNTIME_COMPONENT_ARCHIVE_COUNT)
  foreach(_zanna_runtime_archive IN LISTS ZANNA_RUNTIME_COMPONENT_ARCHIVES)
    string(APPEND ZANNA_RUNTIME_COMPONENT_ARCHIVE_ENTRIES
           "        \"${_zanna_runtime_archive}\",\n")
  endforeach()
endif()
configure_file(
  "${CMAKE_SOURCE_DIR}/include/zanna/runtime/RuntimeComponentManifest.hpp.in"
  "${CMAKE_BINARY_DIR}/generated/zanna/runtime/RuntimeComponentManifest.hpp"
  @ONLY
)
install(FILES "${CMAKE_BINARY_DIR}/generated/zanna/runtime/RuntimeComponentManifest.hpp"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/runtime)

message(STATUS "")
message(STATUS "==========================================")
message(STATUS " Zanna Capability Summary")
message(STATUS "==========================================")
message(STATUS " Host OS:             ${CMAKE_SYSTEM_NAME}")
message(STATUS " Host Arch:           ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS " Compiler:            ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(ZANNA_BUILD_HAS_GRAPHICS)
  message(STATUS " Graphics:            ENABLED (${ZANNAGFX_BACKEND})")
else()
  message(STATUS " Graphics:            DISABLED (${ZANNAGFX_UNAVAILABLE_REASON})")
endif()
if(ZANNA_BUILD_HAS_AUDIO)
  message(STATUS " Audio:               ENABLED (${ZANNAAUD_BACKEND})")
else()
  message(STATUS " Audio:               DISABLED (${ZANNAAUD_UNAVAILABLE_REASON})")
endif()
if(ZANNA_BUILD_HAS_GUI)
  message(STATUS " GUI:                 ENABLED")
else()
  message(STATUS " GUI:                 DISABLED")
endif()
message(STATUS " Native Link x86_64:  ${ZANNA_BUILD_NATIVE_LINK_X86_64}")
message(STATUS " Native Link AArch64: ${ZANNA_BUILD_NATIVE_LINK_AARCH64}")
message(STATUS "==========================================")
message(STATUS "")

add_subdirectory(src)
zanna_assert_no_directory_link_libraries("src" "src")

if(ZANNA_INSTALL_ZANNASTUDIO)
  set(_ZANNA_IDE_ARCH "${ZANNA_IDE_ARCH}")
  if(_ZANNA_IDE_ARCH STREQUAL "")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64|AARCH64)$")
      set(_ZANNA_IDE_ARCH arm64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
      set(_ZANNA_IDE_ARCH x64)
    else()
      message(FATAL_ERROR
        "ZANNA_INSTALL_ZANNASTUDIO requires ZANNA_IDE_ARCH=x64 or arm64 for "
        "host processor '${CMAKE_SYSTEM_PROCESSOR}'")
    endif()
  endif()
  if(NOT _ZANNA_IDE_ARCH STREQUAL "x64" AND NOT _ZANNA_IDE_ARCH STREQUAL "arm64")
    message(FATAL_ERROR "ZANNA_IDE_ARCH must be x64 or arm64, got '${_ZANNA_IDE_ARCH}'")
  endif()

  set(_ZANNASTUDIO_OUTPUT_DIR "${CMAKE_BINARY_DIR}/zannastudio")
  set(_ZANNASTUDIO_BINARY "${_ZANNASTUDIO_OUTPUT_DIR}/zannastudio${CMAKE_EXECUTABLE_SUFFIX}")
  set(_ZANNASTUDIO_BUILDINFO "${_ZANNASTUDIO_OUTPUT_DIR}/zannastudio.buildinfo")
  # scripts/build_ide.{sh,ps1} historically placed the runnable IDE under the
  # source tree, while the ordinary CMake build placed it under build/. Keep
  # both documented launch paths synchronized. Otherwise a successful full
  # build can leave developers repeatedly launching an obsolete IDE that still
  # exhibits already-fixed startup, navigation, and layout bugs.
  set(_ZANNASTUDIO_COMPAT_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/src/zannastudio/bin")
  set(_ZANNASTUDIO_COMPAT_BINARY
      "${_ZANNASTUDIO_COMPAT_OUTPUT_DIR}/zannastudio${CMAKE_EXECUTABLE_SUFFIX}")
  set(_ZANNASTUDIO_COMPAT_BUILDINFO
      "${_ZANNASTUDIO_COMPAT_OUTPUT_DIR}/zannastudio.buildinfo")
  file(GLOB_RECURSE _ZANNASTUDIO_SOURCE_FILES CONFIGURE_DEPENDS
       "${CMAKE_SOURCE_DIR}/src/zannastudio/src/*.zia"
       "${CMAKE_SOURCE_DIR}/src/zannastudio/zanna.project")
  # The Studio compile is the longest single custom-command step in the build:
  # hundreds of Zia sources pass through one quiet zanna invocation that
  # prints nothing until its final dead-strip summary (typically several
  # minutes). Explicit start/finish markers below make that quiet window
  # distinguishable from a hung build.
  if(APPLE)
    # Cocoa exposes the executable leaf name in the system application menu for
    # an unbundled process. Keep the stable `zannastudio` CLI as a tiny launcher
    # and give the native payload its authored product name.
    set(_ZANNASTUDIO_NATIVE_BINARY "${_ZANNASTUDIO_OUTPUT_DIR}/Zanna Studio")
    set(_ZANNASTUDIO_COMPAT_NATIVE_BINARY
        "${_ZANNASTUDIO_COMPAT_OUTPUT_DIR}/Zanna Studio")
    set(_ZANNASTUDIO_MAC_LAUNCHER
        "${CMAKE_SOURCE_DIR}/cmake/ZannaStudioMacLauncher.sh")
    set(_ZANNASTUDIO_MAC_STAGER
        "${CMAKE_SOURCE_DIR}/cmake/StageZannaStudioMac.cmake")
    add_custom_command(
      OUTPUT "${_ZANNASTUDIO_NATIVE_BINARY}" "${_ZANNASTUDIO_BINARY}"
             "${_ZANNASTUDIO_BUILDINFO}"
             "${_ZANNASTUDIO_COMPAT_NATIVE_BINARY}" "${_ZANNASTUDIO_COMPAT_BINARY}"
             "${_ZANNASTUDIO_COMPAT_BUILDINFO}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_ZANNASTUDIO_OUTPUT_DIR}"
      COMMAND ${CMAKE_COMMAND} -E echo
              "Zanna Studio: compiling the ${_ZANNA_IDE_ARCH} native payload (one quiet step, typically several minutes)..."
      COMMAND ${CMAKE_COMMAND} -E env "ZANNA_BUILD_DIR=${CMAKE_BINARY_DIR}"
              "$<TARGET_FILE:zanna>" build "${CMAKE_SOURCE_DIR}/src/zannastudio"
              --arch "${_ZANNA_IDE_ARCH}" -o "${_ZANNASTUDIO_NATIVE_BINARY}"
      COMMAND ${CMAKE_COMMAND} -E echo
              "Zanna Studio: native compile finished, staging launcher and metadata"
      COMMAND ${CMAKE_COMMAND}
              "-DNATIVE_BINARY=${_ZANNASTUDIO_NATIVE_BINARY}"
              "-DLAUNCHER=${_ZANNASTUDIO_BINARY}"
              "-DLAUNCHER_TEMPLATE=${_ZANNASTUDIO_MAC_LAUNCHER}"
              "-DCOMPAT_NATIVE_BINARY=${_ZANNASTUDIO_COMPAT_NATIVE_BINARY}"
              "-DCOMPAT_LAUNCHER=${_ZANNASTUDIO_COMPAT_BINARY}"
              -P "${_ZANNASTUDIO_MAC_STAGER}"
      COMMAND ${CMAKE_COMMAND}
              "-DOUTPUT_FILE=${_ZANNASTUDIO_NATIVE_BINARY}"
              "-DZANNA_PATH=$<TARGET_FILE:zanna>"
              "-DZANNA_VERSION=${ZANNA_VERSION_STR}"
              "-DZANNA_ARCH=${_ZANNA_IDE_ARCH}"
              "-DROOT_DIR=${CMAKE_SOURCE_DIR}"
              -P "${CMAKE_SOURCE_DIR}/cmake/WriteZannaStudioBuildInfo.cmake"
      COMMAND ${CMAKE_COMMAND}
              "-DOUTPUT_FILE=${_ZANNASTUDIO_COMPAT_NATIVE_BINARY}"
              "-DZANNA_PATH=$<TARGET_FILE:zanna>"
              "-DZANNA_VERSION=${ZANNA_VERSION_STR}"
              "-DZANNA_ARCH=${_ZANNA_IDE_ARCH}"
              "-DROOT_DIR=${CMAKE_SOURCE_DIR}"
              -P "${CMAKE_SOURCE_DIR}/cmake/WriteZannaStudioBuildInfo.cmake"
      DEPENDS zanna ${_ZANNASTUDIO_SOURCE_FILES}
              "${CMAKE_SOURCE_DIR}/src/buildmeta/VERSION"
              "${_ZANNASTUDIO_MAC_LAUNCHER}" "${_ZANNASTUDIO_MAC_STAGER}"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      COMMENT "Building Zanna Studio macOS payload and launcher"
      VERBATIM)
    add_custom_target(zannastudio_native ALL
      DEPENDS "${_ZANNASTUDIO_NATIVE_BINARY}" "${_ZANNASTUDIO_BINARY}"
              "${_ZANNASTUDIO_BUILDINFO}"
              "${_ZANNASTUDIO_COMPAT_NATIVE_BINARY}" "${_ZANNASTUDIO_COMPAT_BINARY}"
              "${_ZANNASTUDIO_COMPAT_BUILDINFO}")
    install(PROGRAMS "${_ZANNASTUDIO_BINARY}" "${_ZANNASTUDIO_NATIVE_BINARY}"
            DESTINATION ${CMAKE_INSTALL_BINDIR})
  else()
    set(_ZANNASTUDIO_STAGED_BINARY
        "${_ZANNASTUDIO_OUTPUT_DIR}/.zannastudio-staged${CMAKE_EXECUTABLE_SUFFIX}")
    set(_ZANNASTUDIO_COMPAT_STAGED_BINARY
        "${_ZANNASTUDIO_COMPAT_OUTPUT_DIR}/.zannastudio-staged${CMAKE_EXECUTABLE_SUFFIX}")
    add_custom_command(
      OUTPUT "${_ZANNASTUDIO_BINARY}" "${_ZANNASTUDIO_BUILDINFO}"
             "${_ZANNASTUDIO_COMPAT_BINARY}" "${_ZANNASTUDIO_COMPAT_BUILDINFO}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_ZANNASTUDIO_OUTPUT_DIR}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_ZANNASTUDIO_COMPAT_OUTPUT_DIR}"
      COMMAND ${CMAKE_COMMAND} -E rm -f "${_ZANNASTUDIO_STAGED_BINARY}"
              "${_ZANNASTUDIO_COMPAT_STAGED_BINARY}"
      COMMAND ${CMAKE_COMMAND} -E echo
              "Zanna Studio: compiling the ${_ZANNA_IDE_ARCH} native binary (one quiet step, typically several minutes)..."
      COMMAND ${CMAKE_COMMAND} -E env "ZANNA_BUILD_DIR=${CMAKE_BINARY_DIR}"
              "$<TARGET_FILE:zanna>" build "${CMAKE_SOURCE_DIR}/src/zannastudio"
              --arch "${_ZANNA_IDE_ARCH}" -o "${_ZANNASTUDIO_STAGED_BINARY}"
      COMMAND ${CMAKE_COMMAND} -E echo
              "Zanna Studio: native compile finished, staging binary and metadata"
      COMMAND ${CMAKE_COMMAND}
              "-DOUTPUT_FILE=${_ZANNASTUDIO_BINARY}"
              "-DSTAGED_OUTPUT_FILE=${_ZANNASTUDIO_STAGED_BINARY}"
              "-DZANNA_PATH=$<TARGET_FILE:zanna>"
              "-DZANNA_VERSION=${ZANNA_VERSION_STR}"
              "-DZANNA_ARCH=${_ZANNA_IDE_ARCH}"
              "-DROOT_DIR=${CMAKE_SOURCE_DIR}"
              -P "${CMAKE_SOURCE_DIR}/cmake/WriteZannaStudioBuildInfo.cmake"
      COMMAND ${CMAKE_COMMAND} -E copy
              "${_ZANNASTUDIO_BINARY}" "${_ZANNASTUDIO_COMPAT_STAGED_BINARY}"
      COMMAND ${CMAKE_COMMAND}
              "-DOUTPUT_FILE=${_ZANNASTUDIO_COMPAT_BINARY}"
              "-DSTAGED_OUTPUT_FILE=${_ZANNASTUDIO_COMPAT_STAGED_BINARY}"
              "-DZANNA_PATH=$<TARGET_FILE:zanna>"
              "-DZANNA_VERSION=${ZANNA_VERSION_STR}"
              "-DZANNA_ARCH=${_ZANNA_IDE_ARCH}"
              "-DROOT_DIR=${CMAKE_SOURCE_DIR}"
              -P "${CMAKE_SOURCE_DIR}/cmake/WriteZannaStudioBuildInfo.cmake"
      DEPENDS zanna ${_ZANNASTUDIO_SOURCE_FILES}
              "${CMAKE_SOURCE_DIR}/src/buildmeta/VERSION"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      COMMENT "Building Zanna Studio native binary"
      VERBATIM)
    add_custom_target(zannastudio_native ALL
      DEPENDS "${_ZANNASTUDIO_BINARY}" "${_ZANNASTUDIO_BUILDINFO}"
              "${_ZANNASTUDIO_COMPAT_BINARY}" "${_ZANNASTUDIO_COMPAT_BUILDINFO}")
    install(PROGRAMS "${_ZANNASTUDIO_BINARY}" DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  install(FILES "${_ZANNASTUDIO_BUILDINFO}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---- ZannaTUI subproject ----
option(BUILD_TUI "Build ZannaTUI subproject" ON)
if(ZANNA_INSTALL_TUI AND NOT BUILD_TUI)
  message(WARNING "ZANNA_INSTALL_TUI is ON but BUILD_TUI is OFF; no TUI targets will be installed.")
endif()
if(BUILD_TUI)
  add_subdirectory(src/tui)
  zanna_assert_no_directory_link_libraries("src/tui" "src/tui")
endif()

if(ZANNA_BUILD_TESTING)
  add_subdirectory(src/tests)
  zanna_assert_no_directory_link_libraries("src/tests" "src/tests")
endif()

# ---- Examples ----
option(BUILD_EXAMPLES "Build C++ examples" ON)
if(BUILD_EXAMPLES)
  add_executable(example_vm_stepping examples/embedding/stepping_example.cpp)
  target_link_libraries(example_vm_stepping PRIVATE zanna_common_opts il_build il_vm zanna_support)
  target_include_directories(example_vm_stepping PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated)

  add_executable(example_vm_register_times2 examples/embedding/register_times2.cpp)
  target_link_libraries(example_vm_register_times2 PRIVATE zanna_common_opts il_build il_vm zanna_support)
  target_include_directories(example_vm_register_times2 PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated)

  add_executable(example_vm_smoke_combined examples/embedding/combined.cpp)
  target_link_libraries(example_vm_smoke_combined PRIVATE zanna_common_opts il_build il_vm zanna_support)
  target_include_directories(example_vm_smoke_combined PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated)
endif()

# Install source-level samples as a selectable toolchain payload. Native Windows
# setup presents this tree as a default-on optional component; other package
# formats retain the complete developer distribution.
option(ZANNA_INSTALL_SAMPLES "Install Zanna sample projects and games" ON)
if(ZANNA_INSTALL_SAMPLES)
  install(DIRECTORY ${CMAKE_SOURCE_DIR}/examples/
          DESTINATION ${CMAKE_INSTALL_DATADIR}/zanna/samples
          PATTERN ".git*" EXCLUDE
          PATTERN "__pycache__" EXCLUDE
          PATTERN "node_modules" EXCLUDE
          PATTERN "build" EXCLUDE
          # examples/bin is a local output tree populated by demo build
          # scripts.  Copying it made Release installers depend on whichever
          # configuration a developer happened to build most recently (and
          # could ship executables linked against the debug CRT).
          PATTERN "bin" EXCLUDE
          PATTERN "*.exe" EXCLUDE
          PATTERN "*.dll" EXCLUDE
          PATTERN "*.pdb" EXCLUDE
          PATTERN "*.ilk" EXCLUDE
          PATTERN "*.lib" EXCLUDE
          PATTERN "*.exp" EXCLUDE
          PATTERN "*.obj" EXCLUDE
          PATTERN "*.o" EXCLUDE
          PATTERN "*.a" EXCLUDE
          PATTERN "*.so" EXCLUDE
          PATTERN "*.dylib" EXCLUDE)
endif()

# Canonical project artwork used by native installers, shortcuts, file
# associations, and installed developer applications.
install(FILES
  ${CMAKE_SOURCE_DIR}/misc/images/zannalogo2.png
  ${CMAKE_SOURCE_DIR}/misc/images/zannawallpaper2.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/zanna/branding
)

# Man pages (sections 1 and 7)
if(EXISTS ${CMAKE_SOURCE_DIR}/docs/man/man1)
  install(DIRECTORY ${CMAKE_SOURCE_DIR}/docs/man/man1/
          DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
          FILES_MATCHING PATTERN "*.1")
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/docs/man/man7)
  install(DIRECTORY ${CMAKE_SOURCE_DIR}/docs/man/man7/
          DESTINATION ${CMAKE_INSTALL_MANDIR}/man7
          FILES_MATCHING PATTERN "*.7")
endif()

install(FILES
  ${CMAKE_SOURCE_DIR}/LICENSE
  ${CMAKE_SOURCE_DIR}/README.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/zanna
)

set(_ZANNA_ZIA_VSCODE_DIR ${CMAKE_SOURCE_DIR}/misc/editors/vscode/zia)
set(_ZANNA_ZIA_VSCODE_VSIX ${_ZANNA_ZIA_VSCODE_DIR}/zia-language-0.1.0.vsix)
if(ZANNA_BUILD_VSCODE_VSIX)
  if(WIN32)
    find_program(NPM_EXECUTABLE NAMES npm.cmd npm)
    set(_ZANNA_NPM_COMMAND cmd /c call "${NPM_EXECUTABLE}")
  else()
    find_program(NPM_EXECUTABLE NAMES npm)
    set(_ZANNA_NPM_COMMAND "${NPM_EXECUTABLE}")
  endif()
  if(NOT NPM_EXECUTABLE)
    message(FATAL_ERROR "ZANNA_BUILD_VSCODE_VSIX requires npm on PATH")
  endif()
  set(_ZANNA_BUILT_ZIA_VSIX ${CMAKE_BINARY_DIR}/vscode/zia-language-0.1.0.vsix)
  add_custom_command(
    OUTPUT ${_ZANNA_BUILT_ZIA_VSIX}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/vscode
    COMMAND ${_ZANNA_NPM_COMMAND} ci
    COMMAND ${_ZANNA_NPM_COMMAND} run package:vsix -- --out ${_ZANNA_BUILT_ZIA_VSIX}
    WORKING_DIRECTORY ${_ZANNA_ZIA_VSCODE_DIR}
    DEPENDS
      ${_ZANNA_ZIA_VSCODE_DIR}/package.json
      ${_ZANNA_ZIA_VSCODE_DIR}/package-lock.json
      ${_ZANNA_ZIA_VSCODE_DIR}/extension.js
      ${_ZANNA_ZIA_VSCODE_DIR}/language-configuration.json
      ${_ZANNA_ZIA_VSCODE_DIR}/syntaxes/zia.tmLanguage.json
    COMMENT "Building Zia VS Code extension VSIX"
    VERBATIM)
  add_custom_target(vscode_zia_vsix ALL DEPENDS ${_ZANNA_BUILT_ZIA_VSIX})
  install(FILES ${_ZANNA_BUILT_ZIA_VSIX}
          DESTINATION ${CMAKE_INSTALL_DATADIR}/zanna/vscode)
elseif(EXISTS ${_ZANNA_ZIA_VSCODE_VSIX})
  install(FILES
    ${_ZANNA_ZIA_VSCODE_VSIX}
    DESTINATION ${CMAKE_INSTALL_DATADIR}/zanna/vscode
  )
endif()
if(EXISTS ${_ZANNA_ZIA_VSCODE_DIR}/package.json)
  install(DIRECTORY ${_ZANNA_ZIA_VSCODE_DIR}/
          DESTINATION ${CMAKE_INSTALL_DATADIR}/zanna/vscode/zia
          PATTERN "node_modules" EXCLUDE
          PATTERN "*.vsix" EXCLUDE)
endif()

# Uninstall convenience target (uses install_manifest.txt)
if(NOT TARGET uninstall)
  configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in
    ${CMAKE_BINARY_DIR}/cmake_uninstall.cmake
    @ONLY)
  add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/cmake_uninstall.cmake
    COMMENT "Uninstall installed files (from install_manifest.txt)")
endif()

# ---- Legacy developer archives (CPack) ----
# The system of record for release installers is `zanna package` / `zanna install-package`
# (src/tools/common/packaging). CPack remains only as an opt-in generic developer-archive
# fallback; enable it with -DZANNA_ENABLE_LEGACY_CPACK=ON.
option(ZANNA_ENABLE_LEGACY_CPACK "Enable the legacy CPack developer-archive fallback" OFF)
if(ZANNA_ENABLE_LEGACY_CPACK)
  set(CPACK_PACKAGE_NAME "Zanna")
  set(CPACK_PACKAGE_VENDOR "Zanna Project")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Zanna IL compiler toolchain and VM")
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
  set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")
  set(CPACK_PACKAGE_CONTACT "zanna@example.com")

  if(APPLE)
    # Build a macOS installer package (.pkg) suitable for CLI tools
    set(CPACK_GENERATOR "productbuild")
    set(CPACK_PACKAGE_FILE_NAME "Zanna-${PROJECT_VERSION}-macos")
  elseif(WIN32)
    # Windows: ZIP archive fallback, plus NSIS if a developer has it installed.
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_FILE_NAME "Zanna-${PROJECT_VERSION}-win64")
    # Optional: Enable NSIS installer if available
    find_program(NSIS_FOUND makensis)
    if(NSIS_FOUND)
      list(APPEND CPACK_GENERATOR "NSIS")
    endif()
  elseif(UNIX)
    # Build both DEB and RPM if possible; users can choose
    set(CPACK_GENERATOR "DEB;RPM")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Zanna Project")
    set(CPACK_PACKAGE_FILE_NAME "zanna-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
  endif()

  include(CPack)
endif()

zanna_assert_no_archive_link_paths()

add_custom_target(distclean
  COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_SOURCE_DIR}/cmake/DistClean.cmake"
  COMMENT "Scrub CMake-generated files in this build tree"
)

set(ZANNA_PUBLIC_LIB_TARGETS
  zanna_common_opts
  zanna_support
)

if(DEFINED ZANNA_RUNTIME_COMPONENT_ARCHIVES)
  list(APPEND ZANNA_PUBLIC_LIB_TARGETS ${ZANNA_RUNTIME_COMPONENT_ARCHIVES})
endif()

list(APPEND ZANNA_PUBLIC_LIB_TARGETS
  zanna_runtime
  zanna_pass
  zanna_il_core
  il_runtime
  il_build
  zanna_il_io
  zanna_il_verify
  zanna_il_full
  il_analysis
  il_transform
  il_utils
  il_vm
  zanna_regex_engine
  zanna_text_core
  fe_common
  fe_basic
  il_api
)
list(REMOVE_DUPLICATES ZANNA_PUBLIC_LIB_TARGETS)

# Add platform-dependent targets to export set only if they were built
if (TARGET zannagfx)
  list(APPEND ZANNA_PUBLIC_LIB_TARGETS zannagfx)
endif ()
if (TARGET zannagui)
  list(APPEND ZANNA_PUBLIC_LIB_TARGETS zannagui)
endif ()
if (TARGET zannaaud)
  list(APPEND ZANNA_PUBLIC_LIB_TARGETS zannaaud)
endif ()

if(ZANNA_FAST_DEBUG AND MSVC)
  # MSVC encodes _ITERATOR_DEBUG_LEVEL into C++ object files and rejects a
  # consumer that links static libraries built with a different value. Export
  # the fast-Debug ABI requirement on every public library so find_package(Zanna)
  # consumers compile their Debug objects compatibly.
  foreach(_zanna_public_target IN LISTS ZANNA_PUBLIC_LIB_TARGETS)
    target_compile_definitions(${_zanna_public_target} INTERFACE
      "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:CXX>>:_ITERATOR_DEBUG_LEVEL=0>")
  endforeach()
endif()

set_target_properties(zanna_common_opts PROPERTIES EXPORT_NAME common_opts)
if (TARGET zanna_support)
  set_target_properties(zanna_support PROPERTIES EXPORT_NAME support)
endif ()
foreach(_rt_target IN LISTS ZANNA_RUNTIME_COMPONENT_ARCHIVES)
  if (TARGET ${_rt_target})
    string(REGEX REPLACE "^zanna_" "" _export_name "${_rt_target}")
    set_target_properties(${_rt_target} PROPERTIES EXPORT_NAME "${_export_name}")
  endif ()
endforeach()

foreach(_rt_target IN LISTS ZANNA_RUNTIME_COMPONENT_ARCHIVES)
  if (TARGET ${_rt_target} AND NOT _rt_target IN_LIST ZANNA_PUBLIC_LIB_TARGETS)
    message(FATAL_ERROR
      "Runtime component target ${_rt_target} is built but not exported via ZANNA_PUBLIC_LIB_TARGETS.")
  endif ()
endforeach()
if (TARGET zanna_runtime)
  set_target_properties(zanna_runtime PROPERTIES EXPORT_NAME runtime)
endif ()
if (TARGET zanna_pass)
  set_target_properties(zanna_pass PROPERTIES EXPORT_NAME pass)
endif ()
if (TARGET zanna_il_core)
  set_target_properties(zanna_il_core PROPERTIES EXPORT_NAME il_core)
endif ()
if (TARGET zanna_il_io)
  set_target_properties(zanna_il_io PROPERTIES EXPORT_NAME il_io)
endif ()
if (TARGET zanna_il_verify)
  set_target_properties(zanna_il_verify PROPERTIES EXPORT_NAME il_verify)
endif ()
if (TARGET zanna_il_full)
  set_target_properties(zanna_il_full PROPERTIES EXPORT_NAME il_full)
endif ()
if (TARGET zanna_text_core)
  set_target_properties(zanna_text_core PROPERTIES EXPORT_NAME text_core)
endif ()
if (TARGET zanna_regex_engine)
  set_target_properties(zanna_regex_engine PROPERTIES EXPORT_NAME regex_engine)
endif ()

install(TARGETS ${ZANNA_PUBLIC_LIB_TARGETS}
  EXPORT ZannaTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

set(ZANNA_CLI_TARGETS zanna il-verify il-dis)
install(TARGETS ${ZANNA_CLI_TARGETS}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

export(EXPORT ZannaTargets
  NAMESPACE zanna::
  FILE "${CMAKE_CURRENT_BINARY_DIR}/ZannaTargets.cmake"
)

install(EXPORT ZannaTargets
  NAMESPACE zanna::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Zanna
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/ZannaConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/ZannaConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/ZannaConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Zanna
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/ZannaConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/ZannaConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Zanna
)

zanna_assert_no_directory_link_libraries("." "root project")
