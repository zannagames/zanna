#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: src/tests/tools/SteamDepotPackageSmoke.cmake
# Purpose: Build a real Steam depot for the host operating system from a
#          Zanna.Services project, using the from-scratch fake steam_api library
#          as the Steamworks redistributable, then run the staged executable so
#          the runtime proves it finds the redistributable and pack groups where
#          the packager put them.
# Key invariants:
#   - ZANNA_SERVICES_STEAM_LIBRARY is unset while the staged executable runs,
#     so the provider loads the library beside the executable (Contents/MacOS
#     inside the macOS bundle).
#   - The executable runs from a directory without project files, so assets
#     can only come from the staged .zpak pack group.
#   - macOS depots are ad-hoc signed and must verify with the Steam entitlements.
# Ownership/Lifetime: Test artifacts remain under TEST_WORK_DIR.
# Links: src/tools/common/packaging/StoreDepotBuilder.hpp,
#        src/tests/runtime/RTServicesFakeSteamApi.c,
#        docs/adr/0354-store-depot-packaging.md
#
#===----------------------------------------------------------------------===#

cmake_minimum_required(VERSION 3.20)

foreach (_required ZANNA_BIN FAKE_STEAM_LIBRARY TEST_WORK_DIR)
    if (NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} must be provided to SteamDepotPackageSmoke.cmake")
    endif ()
endforeach ()

if (CMAKE_HOST_WIN32 AND "$ENV{PROCESSOR_ARCHITECTURE}" STREQUAL "ARM64")
    message(STATUS "Skipping Steam depot smoke: Steam Windows depots are x64-only")
    return()
endif ()

file(REMOVE_RECURSE "${TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${TEST_WORK_DIR}/run")

set(_project "${TEST_WORK_DIR}/project")
set(_root "${TEST_WORK_DIR}/steam-root")
set(_sdk "${TEST_WORK_DIR}/sdk/redistributable_bin")

if (CMAKE_HOST_APPLE)
    set(_target steam-macos)
    configure_file("${FAKE_STEAM_LIBRARY}" "${_sdk}/osx/libsteam_api.dylib" COPYONLY)
elseif (CMAKE_HOST_WIN32)
    set(_target steam-windows)
    configure_file("${FAKE_STEAM_LIBRARY}" "${_sdk}/win64/steam_api64.dll" COPYONLY)
else ()
    set(_target steam-linux)
    # One fake serves whichever Linux slot the host architecture selects.
    configure_file("${FAKE_STEAM_LIBRARY}" "${_sdk}/linux64/libsteam_api.so" COPYONLY)
    configure_file("${FAKE_STEAM_LIBRARY}" "${_sdk}/linuxarm64/libsteam_api.so" COPYONLY)
endif ()

# No trailing newline: file(WRITE) emits CRLF on Windows, and the program below
# checks the pack payload's exact byte count.
file(WRITE "${_project}/packdata/level.txt" "level one")
file(WRITE "${_project}/data/readme.txt" "hello\n")
file(WRITE "${_project}/main.zia"
        "module main;
bind Zanna.Terminal as Terminal;
bind Zanna.Services as Services;
bind Zanna.IO.Assets as Assets;

func start() {
    if !Assets.Exists(\"packdata/level.txt\") {
        Terminal.Say(\"pack group was not found where the runtime looks\");
        return;
    }
    var level = Assets.LoadBytes(\"packdata/level.txt\");
    if level == null || level.Length != 9 || Assets.List().Count < 1 {
        Terminal.Say(\"pack group contents are wrong\");
        return;
    }
    var result = Services.Platform.Init(\"steam\", \"480\");
    if result.IsErr {
        Terminal.Say(\"Init error: \" + result.UnwrapErrStr());
        return;
    }
    Terminal.Say(\"library: \" + Services.Steam.LibraryPath);
    Services.Platform.Shutdown();
    Terminal.Say(\"RESULT: ok\");
}
")
file(WRITE "${_project}/zanna.project"
        "project steamsmoke
version 1.0.0
lang zia
entry main.zia
package-name \"Steam Smoke\"
asset data data
pack core packdata/level.txt
steam-app-id 480
steam-depot windows 481
steam-depot macos 482
steam-depot linux 483
steam-depot linux-arm64 484
steam-redist ../sdk
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_project}" --target ${_target} -o "${_root}" --verbose
        RESULT_VARIABLE _pkg_rv
        OUTPUT_VARIABLE _pkg_out
        ERROR_VARIABLE _pkg_err
        TIMEOUT 240)
if (NOT _pkg_rv EQUAL 0)
    message(FATAL_ERROR "${_target} packaging failed\nstdout:\n${_pkg_out}\nstderr:\n${_pkg_err}")
endif ()
string(FIND "${_pkg_err}" "warning:" _warning_pos)
if (NOT _warning_pos EQUAL -1)
    message(FATAL_ERROR "${_target} packaging warned unexpectedly\nstderr:\n${_pkg_err}")
endif ()

if (CMAKE_HOST_APPLE)
    set(_platform macos)
    set(_depot 482)
    set(_app "${_root}/content/macos/Steam Smoke.app")
    set(_exe "${_app}/Contents/MacOS/steamsmoke")
    set(_redist "${_app}/Contents/MacOS/libsteam_api.dylib")
    set(_pack "${_app}/Contents/Resources/steamsmoke-core.zpak")
    set(_asset "${_app}/Contents/Resources/data/readme.txt")
elseif (CMAKE_HOST_WIN32)
    set(_platform windows)
    set(_depot 481)
    set(_exe "${_root}/content/windows/steamsmoke.exe")
    set(_redist "${_root}/content/windows/steam_api64.dll")
    set(_pack "${_root}/content/windows/steamsmoke-core.zpak")
    set(_asset "${_root}/content/windows/data/readme.txt")
else ()
    if (EXISTS "${_root}/content/linux-arm64")
        set(_platform linux-arm64)
        set(_depot 484)
    else ()
        set(_platform linux)
        set(_depot 483)
    endif ()
    set(_exe "${_root}/content/${_platform}/steamsmoke")
    set(_redist "${_root}/content/${_platform}/libsteam_api.so")
    set(_pack "${_root}/content/${_platform}/steamsmoke-core.zpak")
    set(_asset "${_root}/content/${_platform}/data/readme.txt")
endif ()

foreach (_path "${_exe}" "${_redist}" "${_pack}" "${_asset}"
        "${_root}/manifests/${_platform}.json" "${_root}/scripts/app_build_480.vdf")
    if (NOT EXISTS "${_path}")
        message(FATAL_ERROR "expected depot path missing: ${_path}\nstderr:\n${_pkg_err}")
    endif ()
endforeach ()

file(GLOB_RECURSE _appid_files "${_root}/content/*steam_appid.txt")
if (_appid_files)
    message(FATAL_ERROR "steam_appid.txt must never be staged: ${_appid_files}")
endif ()

file(READ "${_root}/manifests/${_platform}.json" _manifest)
foreach (_needle "\"app_id\": \"480\"" "\"depot_id\": \"${_depot}\"" "\"platform\": \"${_platform}\"")
    string(FIND "${_manifest}" "${_needle}" _pos)
    if (_pos EQUAL -1)
        message(FATAL_ERROR "depot manifest is missing ${_needle}:\n${_manifest}")
    endif ()
endforeach ()

file(READ "${_root}/scripts/app_build_480.vdf" _script)
foreach (_needle "\"${_depot}\"" "\"LocalPath\" \"${_platform}/*\"")
    string(FIND "${_script}" "${_needle}" _pos)
    if (_pos EQUAL -1)
        message(FATAL_ERROR "app build script is missing ${_needle}:\n${_script}")
    endif ()
endforeach ()

if (CMAKE_HOST_APPLE)
    execute_process(
            COMMAND /usr/bin/codesign --verify --deep --strict --verbose=2 "${_app}"
            RESULT_VARIABLE _verify_rv
            OUTPUT_VARIABLE _verify_out
            ERROR_VARIABLE _verify_err)
    if (NOT _verify_rv EQUAL 0)
        message(FATAL_ERROR "staged Steam bundle failed codesign verification\n${_verify_out}${_verify_err}")
    endif ()
    execute_process(
            COMMAND /usr/bin/codesign -d --entitlements - "${_app}"
            RESULT_VARIABLE _ent_rv
            OUTPUT_VARIABLE _ent_out
            ERROR_VARIABLE _ent_err)
    if (NOT _ent_rv EQUAL 0)
        message(FATAL_ERROR "cannot read staged bundle entitlements\n${_ent_out}${_ent_err}")
    endif ()
    foreach (_key com.apple.security.cs.disable-library-validation
            com.apple.security.cs.allow-dyld-environment-variables)
        string(FIND "${_ent_out}${_ent_err}" "${_key}" _pos)
        if (_pos EQUAL -1)
            message(FATAL_ERROR "staged bundle entitlements lack ${_key}:\n${_ent_out}${_ent_err}")
        endif ()
    endforeach ()
endif ()

unset(ENV{ZANNA_SERVICES_STEAM_LIBRARY})
execute_process(
        COMMAND "${_exe}"
        WORKING_DIRECTORY "${TEST_WORK_DIR}/run"
        RESULT_VARIABLE _run_rv
        OUTPUT_VARIABLE _run_out
        ERROR_VARIABLE _run_err
        TIMEOUT 60)
if (NOT _run_out MATCHES "RESULT: ok")
    message(FATAL_ERROR "staged executable failed (exit ${_run_rv})\nstdout:\n${_run_out}\nstderr:\n${_run_err}")
endif ()
get_filename_component(_redist_name "${_redist}" NAME)
string(FIND "${_run_out}" "${_redist_name}" _lib_pos)
if (_lib_pos EQUAL -1)
    message(FATAL_ERROR "runtime did not load the staged redistributable\nstdout:\n${_run_out}")
endif ()
