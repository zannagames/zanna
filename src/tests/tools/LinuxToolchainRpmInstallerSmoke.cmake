cmake_minimum_required(VERSION 3.20)

foreach (_required CMAKE_BIN ZANNA_BIN ZANNA_BUILD_DIR)
    if (NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} must be provided to LinuxToolchainRpmInstallerSmoke.cmake")
    endif ()
endforeach ()

include("${CMAKE_CURRENT_LIST_DIR}/ToolchainInstallerSmokeHelpers.cmake")

if (NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Skipping Linux installer smoke; host is not Linux")
    return()
endif ()

find_program(RPMBUILD_BIN rpmbuild)
if (NOT RPMBUILD_BIN)
    message(STATUS "Skipping Linux installer smoke; rpmbuild is not available")
    return()
endif ()

find_program(RPM_BIN rpm)
if (NOT RPM_BIN)
    message(STATUS "Skipping Linux installer smoke; rpm is not available")
    return()
endif ()

set(_tmp_root "${ZANNA_BUILD_DIR}/tests/linux-toolchain-rpm-installer-smoke")
set(_artifact "${_tmp_root}/zanna-toolchain.rpm")
set(_src_dir "${_tmp_root}/consumer-src")
set(_build_dir "${_tmp_root}/consumer-build")
file(REMOVE_RECURSE "${_tmp_root}")
file(MAKE_DIRECTORY "${_tmp_root}")

set(_install_package_cmd
        "${ZANNA_BIN}" install-package
        --build-dir "${ZANNA_BUILD_DIR}"
        --target linux-rpm
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
            "zanna install-package --target linux-rpm failed\nstdout:\n${_build_out}\nstderr:\n${_build_err}")
endif ()

if (NOT EXISTS "${_artifact}")
    message(FATAL_ERROR "expected .rpm artifact was not created: ${_artifact}")
endif ()

execute_process(
        COMMAND "${ZANNA_BIN}" install-package --verify-only "${_artifact}"
        RESULT_VARIABLE _verify_rv
        OUTPUT_VARIABLE _verify_out
        ERROR_VARIABLE _verify_err
)
if (NOT _verify_rv EQUAL 0)
    message(FATAL_ERROR
            "zanna install-package --verify-only failed for .rpm\nstdout:\n${_verify_out}\nstderr:\n${_verify_err}")
endif ()

execute_process(
        COMMAND "${RPM_BIN}" -qip "${_artifact}"
        RESULT_VARIABLE _info_rv
        OUTPUT_VARIABLE _info_out
        ERROR_VARIABLE _info_err
)
if (NOT _info_rv EQUAL 0)
    message(FATAL_ERROR "rpm -qip failed\nstdout:\n${_info_out}\nstderr:\n${_info_err}")
endif ()
foreach (_needle IN ITEMS
        "Name"
        "zanna"
        "License"
        "GPL-3.0-only"
        "Requires"
        "glibc"
        "libgcc"
        "Zanna compiler toolchain")
    if (NOT _info_out MATCHES "${_needle}")
        message(FATAL_ERROR "rpm -qip output did not contain '${_needle}'\n${_info_out}")
    endif ()
endforeach ()

execute_process(
        COMMAND "${RPM_BIN}" -qlp "${_artifact}"
        RESULT_VARIABLE _list_rv
        OUTPUT_VARIABLE _list_out
        ERROR_VARIABLE _list_err
)
if (NOT _list_rv EQUAL 0)
    message(FATAL_ERROR "rpm -qlp failed\nstdout:\n${_list_out}\nstderr:\n${_list_err}")
endif ()
zanna_installer_smoke_required_tool_names(_required_tools)
set(_required_listing_paths
        "/usr/lib/cmake/Zanna/ZannaConfig.cmake"
        "/usr/share/applications/zannastudio.desktop"
        "/usr/share/man/man1/zanna.1")
foreach (_tool IN LISTS _required_tools)
    list(APPEND _required_listing_paths "/usr/bin/${_tool}")
endforeach ()
zanna_installer_smoke_require_listing_paths("${_list_out}" "RPM installer smoke" ${_required_listing_paths})

if (NOT DEFINED ENV{ZANNA_RUN_LINUX_INSTALLER_SMOKE} OR NOT "$ENV{ZANNA_RUN_LINUX_INSTALLER_SMOKE}" STREQUAL "1")
    message(STATUS
            "Linux RPM artifact smoke passed; set ZANNA_RUN_LINUX_INSTALLER_SMOKE=1 to add the privileged install lifecycle")
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
    message(STATUS "Skipping Linux installer smoke; installing the .rpm requires root")
    return()
endif ()

execute_process(
        COMMAND "${RPM_BIN}" -q zanna
        RESULT_VARIABLE _preexisting_rv
        OUTPUT_QUIET
        ERROR_QUIET)
if (_preexisting_rv EQUAL 0)
    message(FATAL_ERROR
            "RPM installer lifecycle smoke requires a clean host; remove the existing zanna package first")
endif ()

set(_baseline_stage "${_tmp_root}/baseline-stage")
set(_baseline_artifact "${_tmp_root}/zanna-toolchain-baseline.rpm")
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
                --target linux-rpm
                --output-file "${_baseline_artifact}"
        RESULT_VARIABLE _baseline_build_rv
        OUTPUT_VARIABLE _baseline_build_out
        ERROR_VARIABLE _baseline_build_err)
if (NOT _baseline_build_rv EQUAL 0)
    message(FATAL_ERROR
            "baseline RPM generation failed\nstdout:\n${_baseline_build_out}\nstderr:\n${_baseline_build_err}")
endif ()

set(_rpm_install_args -Uvh --replacepkgs)
if (EXISTS "/etc/debian_version")
    # Debian-family hosts do not register their dpkg-provided glibc/libgcc packages in the RPM
    # database. Dependency metadata was already checked above, so bypass only RPM's foreign
    # database dependency lookup for this cross-package-manager lifecycle exercise.
    list(APPEND _rpm_install_args --nodeps)
endif ()

execute_process(
        COMMAND "${RPM_BIN}" ${_rpm_install_args} "${_baseline_artifact}"
        RESULT_VARIABLE _baseline_install_rv
        OUTPUT_VARIABLE _baseline_install_out
        ERROR_VARIABLE _baseline_install_err)
if (NOT _baseline_install_rv EQUAL 0)
    message(FATAL_ERROR
            "baseline rpm -Uvh failed\nstdout:\n${_baseline_install_out}\nstderr:\n${_baseline_install_err}")
endif ()
if (NOT EXISTS "${_stale_installed}")
    message(FATAL_ERROR "baseline RPM did not install its upgrade-stale file: ${_stale_installed}")
endif ()
file(WRITE "${_unrelated_installed}" "preserve unrelated package-manager content\n")

execute_process(
        COMMAND "${RPM_BIN}" ${_rpm_install_args} "${_artifact}"
        RESULT_VARIABLE _install_rv
        OUTPUT_VARIABLE _install_out
        ERROR_VARIABLE _install_err
)
if (NOT _install_rv EQUAL 0)
    message(FATAL_ERROR "rpm -Uvh failed\nstdout:\n${_install_out}\nstderr:\n${_install_err}")
endif ()
if (EXISTS "${_stale_installed}")
    message(FATAL_ERROR "RPM package upgrade left a stale baseline-owned file: ${_stale_installed}")
endif ()
if (NOT EXISTS "${_unrelated_installed}")
    message(FATAL_ERROR "RPM package upgrade removed unrelated content: ${_unrelated_installed}")
endif ()

zanna_installer_smoke_verify_installed_tools("/usr/bin" "" "RPM installer smoke")

zanna_installer_smoke_verify_cmake_consumer(
        "${CMAKE_BIN}"
        "${_src_dir}"
        "${_build_dir}"
        "${ZANNA_CONFIG}"
        "RPM installer smoke")
zanna_installer_smoke_verify_native_codegen(
        "${CMAKE_BIN}"
        /usr/bin/zanna
        "${_tmp_root}"
        "RPM installer smoke")

execute_process(
        COMMAND "${RPM_BIN}" -e zanna
        RESULT_VARIABLE _remove_rv
        OUTPUT_VARIABLE _remove_out
        ERROR_VARIABLE _remove_err
)
if (NOT _remove_rv EQUAL 0)
    message(FATAL_ERROR "rpm -e zanna failed\nstdout:\n${_remove_out}\nstderr:\n${_remove_err}")
endif ()
if (EXISTS "/usr/bin/zanna" OR IS_SYMLINK "/usr/bin/zanna")
    message(FATAL_ERROR "rpm -e left the owned /usr/bin/zanna command")
endif ()
if (NOT EXISTS "${_unrelated_installed}")
    message(FATAL_ERROR "rpm -e removed unrelated content: ${_unrelated_installed}")
endif ()
file(REMOVE "${_unrelated_installed}")
