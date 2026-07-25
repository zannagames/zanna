cmake_minimum_required(VERSION 3.20)

foreach (_required CMAKE_BIN ZANNA_BIN ZANNA_BUILD_DIR)
    if (NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} must be provided to LinuxToolchainDebInstallerSmoke.cmake")
    endif ()
endforeach ()

include("${CMAKE_CURRENT_LIST_DIR}/ToolchainInstallerSmokeHelpers.cmake")

if (NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Skipping Linux installer smoke; host is not Linux")
    return()
endif ()

find_program(DPKG_DEB_BIN dpkg-deb)
if (NOT DPKG_DEB_BIN)
    message(STATUS "Skipping Linux installer smoke; dpkg-deb is not available")
    return()
endif ()

set(_tmp_root "${ZANNA_BUILD_DIR}/tests/linux-toolchain-deb-installer-smoke")
set(_artifact "${_tmp_root}/zanna-toolchain.deb")
set(_src_dir "${_tmp_root}/consumer-src")
set(_build_dir "${_tmp_root}/consumer-build")
file(REMOVE_RECURSE "${_tmp_root}")
file(MAKE_DIRECTORY "${_tmp_root}")

set(_install_package_cmd
        "${ZANNA_BIN}" install-package
        --build-dir "${ZANNA_BUILD_DIR}"
        --target linux-deb
        -o "${_artifact}")
if (DEFINED ZANNA_CONFIG AND NOT "${ZANNA_CONFIG}" STREQUAL "")
    list(APPEND _install_package_cmd --config "${ZANNA_CONFIG}")
endif ()

execute_process(
        COMMAND ${_install_package_cmd}
        RESULT_VARIABLE _build_rv
        OUTPUT_VARIABLE _build_out
        ERROR_VARIABLE _build_err
)
if (NOT _build_rv EQUAL 0)
    message(FATAL_ERROR
            "zanna install-package --target linux-deb failed\nstdout:\n${_build_out}\nstderr:\n${_build_err}")
endif ()

if (NOT EXISTS "${_artifact}")
    message(FATAL_ERROR "expected .deb artifact was not created: ${_artifact}")
endif ()

execute_process(
        COMMAND "${ZANNA_BIN}" install-package --verify-only "${_artifact}"
        RESULT_VARIABLE _verify_rv
        OUTPUT_VARIABLE _verify_out
        ERROR_VARIABLE _verify_err
)
if (NOT _verify_rv EQUAL 0)
    message(FATAL_ERROR
            "zanna install-package --verify-only failed for .deb\nstdout:\n${_verify_out}\nstderr:\n${_verify_err}")
endif ()

execute_process(
        COMMAND "${DPKG_DEB_BIN}" -I "${_artifact}"
        RESULT_VARIABLE _info_rv
        OUTPUT_VARIABLE _info_out
        ERROR_VARIABLE _info_err
)
if (NOT _info_rv EQUAL 0)
    message(FATAL_ERROR "dpkg-deb -I failed\nstdout:\n${_info_out}\nstderr:\n${_info_err}")
endif ()
foreach (_needle IN ITEMS
        "Package: zanna"
        "Maintainer: Zanna Project <splanck@users.noreply.github.com>"
        "Depends:"
        "libc6"
        "libgcc-s1"
        "libx11-6"
        "libasound2 | libasound2t64"
        "Zanna compiler toolchain")
    if (NOT _info_out MATCHES "${_needle}")
        message(FATAL_ERROR "dpkg-deb -I output did not contain '${_needle}'\n${_info_out}")
    endif ()
endforeach ()

execute_process(
        COMMAND "${DPKG_DEB_BIN}" -c "${_artifact}"
        RESULT_VARIABLE _list_rv
        OUTPUT_VARIABLE _list_out
        ERROR_VARIABLE _list_err
)
if (NOT _list_rv EQUAL 0)
    message(FATAL_ERROR "dpkg-deb -c failed\nstdout:\n${_list_out}\nstderr:\n${_list_err}")
endif ()
zanna_installer_smoke_required_tool_names(_required_tools)
set(_required_listing_paths
        "usr/lib/cmake/Zanna/ZannaConfig.cmake"
        "usr/share/applications/zannastudio.desktop"
        "usr/share/man/man1/zanna.1")
foreach (_tool IN LISTS _required_tools)
    list(APPEND _required_listing_paths "usr/bin/${_tool}")
endforeach ()
zanna_installer_smoke_require_listing_paths("${_list_out}" "Debian installer smoke" ${_required_listing_paths})

if (NOT DEFINED ENV{ZANNA_RUN_LINUX_INSTALLER_SMOKE} OR NOT "$ENV{ZANNA_RUN_LINUX_INSTALLER_SMOKE}" STREQUAL "1")
    message(STATUS
            "Linux DEB artifact smoke passed; set ZANNA_RUN_LINUX_INSTALLER_SMOKE=1 to add the privileged install lifecycle")
    return()
endif ()

find_program(DPKG_BIN dpkg)
if (NOT DPKG_BIN)
    message(STATUS "Skipping Linux installer smoke; dpkg is not available")
    return()
endif ()

execute_process(
        COMMAND id -u
        RESULT_VARIABLE _id_rv
        OUTPUT_VARIABLE _uid
        ERROR_VARIABLE _id_err
        OUTPUT_STRIP_TRAILING_WHITESPACE
)
if (NOT _id_rv EQUAL 0 OR NOT _uid STREQUAL "0")
    message(STATUS "Skipping Linux installer smoke; installing the .deb requires root")
    return()
endif ()

execute_process(
        COMMAND "${DPKG_BIN}" -s zanna
        RESULT_VARIABLE _preexisting_rv
        OUTPUT_QUIET
        ERROR_QUIET)
if (_preexisting_rv EQUAL 0)
    message(FATAL_ERROR
            "Debian installer lifecycle smoke requires a clean host; remove the existing zanna package first")
endif ()

set(_baseline_stage "${_tmp_root}/baseline-stage")
set(_baseline_artifact "${_tmp_root}/zanna-toolchain-baseline.deb")
set(_stale_relative "share/zanna/installer-upgrade-stale.txt")
set(_stale_installed "/usr/${_stale_relative}")
set(_unrelated_installed "/usr/share/zanna/installer-upgrade-unrelated.txt")
set(_baseline_install_cmd "${CMAKE_BIN}" --install "${ZANNA_BUILD_DIR}" --prefix "${_baseline_stage}")
if (DEFINED ZANNA_CONFIG AND NOT "${ZANNA_CONFIG}" STREQUAL "")
    list(APPEND _baseline_install_cmd --config "${ZANNA_CONFIG}")
endif ()
execute_process(
        COMMAND ${_baseline_install_cmd}
        RESULT_VARIABLE _baseline_stage_rv
        OUTPUT_VARIABLE _baseline_stage_out
        ERROR_VARIABLE _baseline_stage_err)
if (NOT _baseline_stage_rv EQUAL 0)
    message(FATAL_ERROR
            "baseline cmake --install failed\nstdout:\n${_baseline_stage_out}\nstderr:\n${_baseline_stage_err}")
endif ()
file(MAKE_DIRECTORY "${_baseline_stage}/share/zanna")
file(WRITE "${_baseline_stage}/${_stale_relative}" "owned only by installer upgrade baseline\n")
execute_process(
        COMMAND "${ZANNA_BIN}" install-package
                --stage-dir "${_baseline_stage}"
                --target linux-deb
                --output-file "${_baseline_artifact}"
        RESULT_VARIABLE _baseline_build_rv
        OUTPUT_VARIABLE _baseline_build_out
        ERROR_VARIABLE _baseline_build_err)
if (NOT _baseline_build_rv EQUAL 0)
    message(FATAL_ERROR
            "baseline .deb generation failed\nstdout:\n${_baseline_build_out}\nstderr:\n${_baseline_build_err}")
endif ()

execute_process(
        COMMAND "${DPKG_BIN}" -i "${_baseline_artifact}"
        RESULT_VARIABLE _baseline_install_rv
        OUTPUT_VARIABLE _baseline_install_out
        ERROR_VARIABLE _baseline_install_err)
if (NOT _baseline_install_rv EQUAL 0)
    message(FATAL_ERROR
            "baseline dpkg -i failed\nstdout:\n${_baseline_install_out}\nstderr:\n${_baseline_install_err}")
endif ()
if (NOT EXISTS "${_stale_installed}")
    message(FATAL_ERROR "baseline .deb did not install its upgrade-stale file: ${_stale_installed}")
endif ()
file(WRITE "${_unrelated_installed}" "preserve unrelated package-manager content\n")

execute_process(
        COMMAND "${DPKG_BIN}" -i "${_artifact}"
        RESULT_VARIABLE _install_rv
        OUTPUT_VARIABLE _install_out
        ERROR_VARIABLE _install_err
)
if (NOT _install_rv EQUAL 0)
    message(FATAL_ERROR "dpkg -i failed\nstdout:\n${_install_out}\nstderr:\n${_install_err}")
endif ()
if (EXISTS "${_stale_installed}")
    message(FATAL_ERROR "Debian package upgrade left a stale baseline-owned file: ${_stale_installed}")
endif ()
if (NOT EXISTS "${_unrelated_installed}")
    message(FATAL_ERROR "Debian package upgrade removed unrelated content: ${_unrelated_installed}")
endif ()

zanna_installer_smoke_verify_installed_tools("/usr/bin" "" "Debian installer smoke")

zanna_installer_smoke_verify_cmake_consumer(
        "${CMAKE_BIN}"
        "${_src_dir}"
        "${_build_dir}"
        "${ZANNA_CONFIG}"
        "Debian installer smoke")
zanna_installer_smoke_verify_native_codegen(
        "${CMAKE_BIN}"
        /usr/bin/zanna
        "${_tmp_root}"
        "Debian installer smoke")

execute_process(
        COMMAND "${DPKG_BIN}" -r zanna
        RESULT_VARIABLE _remove_rv
        OUTPUT_VARIABLE _remove_out
        ERROR_VARIABLE _remove_err
)
if (NOT _remove_rv EQUAL 0)
    message(FATAL_ERROR "dpkg -r zanna failed\nstdout:\n${_remove_out}\nstderr:\n${_remove_err}")
endif ()
if (EXISTS "/usr/bin/zanna" OR IS_SYMLINK "/usr/bin/zanna")
    message(FATAL_ERROR "dpkg -r left the owned /usr/bin/zanna command")
endif ()
if (NOT EXISTS "${_unrelated_installed}")
    message(FATAL_ERROR "dpkg -r removed unrelated content: ${_unrelated_installed}")
endif ()
file(REMOVE "${_unrelated_installed}")
