#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: src/tests/tools/PackageAssetPacksSmoke.cmake
# Purpose: Package a project whose assets live in a `pack` group, unpack the
#          package, and run the packaged executable from an unrelated working
#          directory, proving the pack ships where the runtime mounts it
#          (ADR 0355).
# Key invariants:
#   - The packaged program can only find its asset inside the generated pack:
#     the working directory holds no project files.
#   - Every host checks the portable tarball; macOS also checks the .app ZIP
#     (packs in Contents/Resources) and Linux the self-extracting bundle
#     (executable beside its packs in usr/lib/<package>, launched via AppRun).
# Ownership/Lifetime: Test artifacts remain under TEST_WORK_DIR.
# Links: src/tools/zanna/cmd_package.cpp,
#        src/tools/common/packaging/LinuxPackageBuilder.cpp,
#        src/tools/common/packaging/MacOSPackageBuilder.cpp,
#        docs/adr/0355-package-formats-ship-pack-groups.md
#
#===----------------------------------------------------------------------===#

cmake_minimum_required(VERSION 3.20)

foreach (_required ZANNA_BIN TEST_WORK_DIR)
    if (NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} must be provided to PackageAssetPacksSmoke.cmake")
    endif ()
endforeach ()

if (CMAKE_HOST_WIN32)
    message(STATUS "Skipping package asset packs smoke on Windows: installers are covered by unit tests")
    return()
endif ()

file(REMOVE_RECURSE "${TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${TEST_WORK_DIR}/run")

set(_project "${TEST_WORK_DIR}/project")
file(WRITE "${_project}/packdata/level.txt" "level one\n")
file(WRITE "${_project}/main.zia"
        "module main;
bind Zanna.Terminal as Terminal;
bind Zanna.IO.Assets as Assets;

func start() {
    // A directory pack source names its entries relative to that directory.
    var level = Assets.LoadBytes(\"level.txt\");
    if level == null || level.Length != 10 {
        Terminal.Say(\"pack group was not found where the runtime looks\");
        return;
    }
    Terminal.Say(\"RESULT: ok\");
}
")
file(WRITE "${_project}/zanna.project"
        "project packsmoke
version 1.0.0
lang zia
entry main.zia
package-name \"Pack Smoke\"
pack-compressed core packdata
")

# Run a packaged executable from the empty run directory and require success.
function(_run_packaged label)
    execute_process(
            COMMAND ${ARGN}
            WORKING_DIRECTORY "${TEST_WORK_DIR}/run"
            RESULT_VARIABLE _rv
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err
            TIMEOUT 60)
    if (NOT _out MATCHES "RESULT: ok")
        message(FATAL_ERROR "${label} did not find its pack (exit ${_rv})\nstdout:\n${_out}\nstderr:\n${_err}")
    endif ()
endfunction()

# Package the project for one target and require success.
function(_package target output)
    execute_process(
            COMMAND "${ZANNA_BIN}" package "${_project}" --target ${target} -o "${output}"
            RESULT_VARIABLE _rv
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err
            TIMEOUT 240)
    if (NOT _rv EQUAL 0)
        message(FATAL_ERROR "zanna package --target ${target} failed\nstdout:\n${_out}\nstderr:\n${_err}")
    endif ()
endfunction()

# Portable tarball: the pack sits beside the executable at the top directory.
_package(tarball "${TEST_WORK_DIR}/packsmoke.tar.gz")
file(MAKE_DIRECTORY "${TEST_WORK_DIR}/tarball")
execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xzf "${TEST_WORK_DIR}/packsmoke.tar.gz"
        WORKING_DIRECTORY "${TEST_WORK_DIR}/tarball"
        RESULT_VARIABLE _untar_rv)
if (NOT _untar_rv EQUAL 0)
    message(FATAL_ERROR "cannot extract the tarball")
endif ()
if (NOT EXISTS "${TEST_WORK_DIR}/tarball/packsmoke-1.0.0/packsmoke-core.zpak")
    message(FATAL_ERROR "the tarball does not ship packsmoke-core.zpak beside the executable")
endif ()
_run_packaged("tarball executable" "${TEST_WORK_DIR}/tarball/packsmoke-1.0.0/packsmoke")

if (CMAKE_HOST_APPLE)
    # macOS .app ZIP: the pack sits in Contents/Resources.
    _package(macos "${TEST_WORK_DIR}/packsmoke.zip")
    file(MAKE_DIRECTORY "${TEST_WORK_DIR}/app")
    execute_process(
            COMMAND /usr/bin/ditto -x -k "${TEST_WORK_DIR}/packsmoke.zip" "${TEST_WORK_DIR}/app"
            RESULT_VARIABLE _unzip_rv)
    if (NOT _unzip_rv EQUAL 0)
        message(FATAL_ERROR "cannot extract the macOS app ZIP")
    endif ()
    set(_app "${TEST_WORK_DIR}/app/Pack Smoke.app")
    if (NOT EXISTS "${_app}/Contents/Resources/packsmoke-core.zpak")
        message(FATAL_ERROR "the app does not ship packsmoke-core.zpak in Contents/Resources")
    endif ()
    _run_packaged("macOS app executable" "${_app}/Contents/MacOS/packsmoke")
else ()
    # Self-extracting Linux bundle: AppRun launches usr/bin/<exe>, a link to the executable
    # beside its packs in usr/lib/<package>.
    _package(linux-bundle "${TEST_WORK_DIR}/packsmoke.run")
    set(ENV{XDG_CACHE_HOME} "${TEST_WORK_DIR}/cache")
    set(ENV{ZANNA_BUNDLE_QUIET} "1")
    _run_packaged("Linux bundle" /bin/sh "${TEST_WORK_DIR}/packsmoke.run")
endif ()
