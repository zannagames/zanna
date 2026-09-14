#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: src/tests/tools/PackageCliTests.cmake
# Purpose: Exercise package and install-package command-line contracts,
#          including store depot (steam-*) dry runs and diagnostics, and the
#          pack groups every package lists (ADR 0355).
#
# Key invariants: Unsafe or ambiguous packaging arguments fail without output.
#
# Ownership/Lifetime: Test artifacts remain under TEST_WORK_DIR.
#
# Links: cmd_package.cpp, cmd_install_package.cpp, StoreDepotBuilder.hpp,
#        docs/adr/0354-store-depot-packaging.md
#
#===----------------------------------------------------------------------===#

cmake_minimum_required(VERSION 3.20)

foreach (_required ZANNA_BIN TEST_WORK_DIR)
    if (NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} must be provided to PackageCliTests.cmake")
    endif ()
endforeach ()

file(REMOVE_RECURSE "${TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${TEST_WORK_DIR}")

execute_process(
        COMMAND "${ZANNA_BIN}" package --help
        RESULT_VARIABLE _help_rv
        OUTPUT_VARIABLE _help_out
        ERROR_VARIABLE _help_err)
if (NOT _help_rv EQUAL 0)
    message(FATAL_ERROR "zanna package --help should exit 0\nstdout:\n${_help_out}\nstderr:\n${_help_err}")
endif ()

execute_process(
        COMMAND "${ZANNA_BIN}" package --target
        RESULT_VARIABLE _missing_target_rv
        OUTPUT_VARIABLE _missing_target_out
        ERROR_VARIABLE _missing_target_err)
if (_missing_target_rv EQUAL 0)
    message(FATAL_ERROR "zanna package --target without a value should fail")
endif ()

set(_quoted_project "${TEST_WORK_DIR}/quoted-project")
file(MAKE_DIRECTORY "${_quoted_project}/asset dir")
file(MAKE_DIRECTORY "${_quoted_project}/scripts")
file(WRITE "${_quoted_project}/main.zia" "func start() {}\n")
file(WRITE "${_quoted_project}/asset dir/config.txt" "ok\n")
file(WRITE "${_quoted_project}/scripts/post install.sh" "echo post-install\n")
file(WRITE "${_quoted_project}/zanna.project"
        "project quotedpkg
version 1.0.0
lang zia
entry main.zia
package-name \"Quoted Package\"
asset \"asset dir\" \"data files\"
post-install \"scripts/post install.sh\"
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_quoted_project}" --target tarball --dry-run
        RESULT_VARIABLE _quoted_rv
        OUTPUT_VARIABLE _quoted_out
        ERROR_VARIABLE _quoted_err)
if (NOT _quoted_rv EQUAL 0)
    message(FATAL_ERROR "quoted package dry-run should succeed\nstdout:\n${_quoted_out}\nstderr:\n${_quoted_err}")
endif ()

set(_missing_project "${TEST_WORK_DIR}/missing-project")
file(MAKE_DIRECTORY "${_missing_project}")
file(WRITE "${_missing_project}/main.zia" "func start() {}\n")
file(WRITE "${_missing_project}/zanna.project"
        "project missingpkg
version 1.0.0
lang zia
entry main.zia
asset missing data
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_missing_project}" --target tarball --dry-run
        RESULT_VARIABLE _missing_asset_rv
        OUTPUT_VARIABLE _missing_asset_out
        ERROR_VARIABLE _missing_asset_err)
if (_missing_asset_rv EQUAL 0)
    message(FATAL_ERROR "dry-run with missing asset should fail")
endif ()
if (NOT _missing_asset_err MATCHES "asset source path not found")
    message(FATAL_ERROR "missing asset diagnostic did not mention the missing source\nstdout:\n${_missing_asset_out}\nstderr:\n${_missing_asset_err}")
endif ()

set(_bad_url_project "${TEST_WORK_DIR}/bad-url-project")
file(MAKE_DIRECTORY "${_bad_url_project}")
file(WRITE "${_bad_url_project}/main.zia" "func start() {}\n")
file(WRITE "${_bad_url_project}/zanna.project"
        "project badurl
version 1.0.0
lang zia
entry main.zia
package-homepage https:///missing-host
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_bad_url_project}" --target tarball --dry-run
        RESULT_VARIABLE _bad_url_rv
        OUTPUT_VARIABLE _bad_url_out
        ERROR_VARIABLE _bad_url_err)
if (_bad_url_rv EQUAL 0)
    message(FATAL_ERROR "dry-run with malformed package-homepage should fail")
endif ()
if (NOT _bad_url_err MATCHES "URL host")
    message(FATAL_ERROR "bad URL diagnostic did not mention the URL host\nstdout:\n${_bad_url_out}\nstderr:\n${_bad_url_err}")
endif ()

set(_bad_assoc_project "${TEST_WORK_DIR}/bad-assoc-project")
file(MAKE_DIRECTORY "${_bad_assoc_project}")
file(WRITE "${_bad_assoc_project}/main.zia" "func start() {}\n")
file(WRITE "${_bad_assoc_project}/zanna.project"
        "project badassoc
version 1.0.0
lang zia
entry main.zia
file-assoc . \"Bad\" text/plain
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_bad_assoc_project}" --target linux --dry-run
        RESULT_VARIABLE _bad_assoc_rv
        OUTPUT_VARIABLE _bad_assoc_out
        ERROR_VARIABLE _bad_assoc_err)
if (_bad_assoc_rv EQUAL 0)
    message(FATAL_ERROR "dry-run with a bare-dot file association should fail")
endif ()
if (NOT _bad_assoc_err MATCHES "dotted extension")
    message(FATAL_ERROR "bad association diagnostic did not mention dotted extension\nstdout:\n${_bad_assoc_out}\nstderr:\n${_bad_assoc_err}")
endif ()

set(_dup_assoc_project "${TEST_WORK_DIR}/dup-assoc-project")
file(MAKE_DIRECTORY "${_dup_assoc_project}")
file(WRITE "${_dup_assoc_project}/main.zia" "func start() {}\n")
file(WRITE "${_dup_assoc_project}/zanna.project"
        "project dupassoc
version 1.0.0
lang zia
entry main.zia
file-assoc .zia \"Zia Source\" text/x-zia
file-assoc .ZIA \"Zia Source 2\" text/x-zia-2
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_dup_assoc_project}" --target linux --dry-run
        RESULT_VARIABLE _dup_assoc_rv
        OUTPUT_VARIABLE _dup_assoc_out
        ERROR_VARIABLE _dup_assoc_err)
if (_dup_assoc_rv EQUAL 0)
    message(FATAL_ERROR "dry-run with duplicate file associations should fail")
endif ()
if (NOT _dup_assoc_err MATCHES "duplicate file association")
    message(FATAL_ERROR "duplicate association diagnostic did not mention duplicates\nstdout:\n${_dup_assoc_out}\nstderr:\n${_dup_assoc_err}")
endif ()

set(_bad_scalar_project "${TEST_WORK_DIR}/bad-scalar-project")
file(MAKE_DIRECTORY "${_bad_scalar_project}")
file(WRITE "${_bad_scalar_project}/main.zia" "func start() {}\n")
file(WRITE "${_bad_scalar_project}/zanna.project"
        "project badscalar
version 1.0.0
lang zia
entry main.zia
package-name \"Foo\" bar
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_bad_scalar_project}" --target tarball --dry-run
        RESULT_VARIABLE _bad_scalar_rv
        OUTPUT_VARIABLE _bad_scalar_out
        ERROR_VARIABLE _bad_scalar_err)
if (_bad_scalar_rv EQUAL 0)
    message(FATAL_ERROR "package-name with multiple scalar tokens should fail")
endif ()
if (NOT _bad_scalar_err MATCHES "requires exactly one scalar value")
    message(FATAL_ERROR "bad scalar diagnostic did not mention scalar arity\nstdout:\n${_bad_scalar_out}\nstderr:\n${_bad_scalar_err}")
endif ()

set(_linux_dep_project "${TEST_WORK_DIR}/linux-dep-project")
file(MAKE_DIRECTORY "${_linux_dep_project}")
file(WRITE "${_linux_dep_project}/main.zia" "func start() {}\n")
file(WRITE "${_linux_dep_project}/zanna.project"
        "project linuxdeps
version 1.0.0
lang zia
entry main.zia
package-category Utility
package-depends libc6 (>= 2.34),\tlibstdc++6 | libc++1
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_linux_dep_project}" --target linux --dry-run
        RESULT_VARIABLE _linux_dep_rv
        OUTPUT_VARIABLE _linux_dep_out
        ERROR_VARIABLE _linux_dep_err)
if (NOT _linux_dep_rv EQUAL 0)
    message(FATAL_ERROR "valid Debian dependency dry-run should succeed\nstdout:\n${_linux_dep_out}\nstderr:\n${_linux_dep_err}")
endif ()

set(_bad_deb_version_project "${TEST_WORK_DIR}/bad-deb-version-project")
file(MAKE_DIRECTORY "${_bad_deb_version_project}")
file(WRITE "${_bad_deb_version_project}/main.zia" "func start() {}\n")
file(WRITE "${_bad_deb_version_project}/zanna.project"
        "project baddebversion
version -1.0
lang zia
entry main.zia
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_bad_deb_version_project}" --target linux --dry-run
        RESULT_VARIABLE _bad_deb_version_rv
        OUTPUT_VARIABLE _bad_deb_version_out
        ERROR_VARIABLE _bad_deb_version_err)
if (_bad_deb_version_rv EQUAL 0)
    message(FATAL_ERROR "dry-run with an invalid Debian version should fail")
endif ()
if (NOT _bad_deb_version_err MATCHES "Debian version")
    message(FATAL_ERROR "bad Debian version diagnostic did not mention Debian version\nstdout:\n${_bad_deb_version_out}\nstderr:\n${_bad_deb_version_err}")
endif ()

set(_bad_macos_id_project "${TEST_WORK_DIR}/bad-macos-id-project")
file(MAKE_DIRECTORY "${_bad_macos_id_project}")
file(WRITE "${_bad_macos_id_project}/main.zia" "func start() {}\n")
file(WRITE "${_bad_macos_id_project}/zanna.project"
        "project badmacid
version 1.0.0
lang zia
entry main.zia
package-identifier org.zanna.bad_id
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_bad_macos_id_project}" --target macos --dry-run
        RESULT_VARIABLE _bad_macos_id_rv
        OUTPUT_VARIABLE _bad_macos_id_out
        ERROR_VARIABLE _bad_macos_id_err)
if (_bad_macos_id_rv EQUAL 0)
    message(FATAL_ERROR "dry-run with an invalid macOS bundle identifier should fail")
endif ()
if (NOT _bad_macos_id_err MATCHES "bundle identifier")
    message(FATAL_ERROR "bad macOS identifier diagnostic did not mention bundle identifier\nstdout:\n${_bad_macos_id_out}\nstderr:\n${_bad_macos_id_err}")
endif ()

set(_bad_macos_sign_project "${TEST_WORK_DIR}/bad-macos-sign-project")
file(MAKE_DIRECTORY "${_bad_macos_sign_project}")
file(WRITE "${_bad_macos_sign_project}/main.zia" "func start() {}\n")
file(WRITE "${_bad_macos_sign_project}/zanna.project"
        "project badmacsign
version 1.0.0
lang zia
entry main.zia
macos-sign-mode bogus
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_bad_macos_sign_project}" --target macos --dry-run
        RESULT_VARIABLE _bad_macos_sign_rv
        OUTPUT_VARIABLE _bad_macos_sign_out
        ERROR_VARIABLE _bad_macos_sign_err)
if (_bad_macos_sign_rv EQUAL 0)
    message(FATAL_ERROR "dry-run with an invalid macOS sign mode should fail")
endif ()
if (NOT _bad_macos_sign_err MATCHES "sign mode")
    message(FATAL_ERROR "bad macOS sign mode diagnostic did not mention sign mode\nstdout:\n${_bad_macos_sign_out}\nstderr:\n${_bad_macos_sign_err}")
endif ()

set(_bad_macos_notary_project "${TEST_WORK_DIR}/bad-macos-notary-project")
file(MAKE_DIRECTORY "${_bad_macos_notary_project}")
file(WRITE "${_bad_macos_notary_project}/main.zia" "func start() {}\n")
file(WRITE "${_bad_macos_notary_project}/zanna.project"
        "project badmacnotary
version 1.0.0
lang zia
entry main.zia
macos-sign-mode adhoc
macos-notary-profile profile
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_bad_macos_notary_project}" --target macos --dry-run
        RESULT_VARIABLE _bad_macos_notary_rv
        OUTPUT_VARIABLE _bad_macos_notary_out
        ERROR_VARIABLE _bad_macos_notary_err)
if (_bad_macos_notary_rv EQUAL 0)
    message(FATAL_ERROR "dry-run with macOS notarization outside Developer ID mode should fail")
endif ()
if (NOT _bad_macos_notary_err MATCHES "developer-id")
    message(FATAL_ERROR "bad macOS notarization diagnostic did not mention developer-id\nstdout:\n${_bad_macos_notary_out}\nstderr:\n${_bad_macos_notary_err}")
endif ()

set(_macos_sign_project "${TEST_WORK_DIR}/macos-sign-project")
file(MAKE_DIRECTORY "${_macos_sign_project}")
file(WRITE "${_macos_sign_project}/main.zia" "func start() {}\n")
file(WRITE "${_macos_sign_project}/zanna.project"
        "project macsign
version 1.0.0
lang zia
entry main.zia
macos-sign-mode adhoc
macos-hardened-runtime on
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_macos_sign_project}" --target macos --dry-run
        RESULT_VARIABLE _macos_sign_rv
        OUTPUT_VARIABLE _macos_sign_out
        ERROR_VARIABLE _macos_sign_err)
if (NOT _macos_sign_rv EQUAL 0)
    message(FATAL_ERROR "dry-run with valid macOS signing metadata should succeed\nstdout:\n${_macos_sign_out}\nstderr:\n${_macos_sign_err}")
endif ()
if (NOT _macos_sign_out MATCHES "macOS signing: adhoc")
    message(FATAL_ERROR "macOS signing dry-run did not report signing mode\nstdout:\n${_macos_sign_out}\nstderr:\n${_macos_sign_err}")
endif ()

set(_windows_scope_project "${TEST_WORK_DIR}/windows-scope-project")
file(MAKE_DIRECTORY "${_windows_scope_project}")
file(WRITE "${_windows_scope_project}/main.zia" "func start() {}\n")
file(WRITE "${_windows_scope_project}/zanna.project"
        "project winscope
version 1.0.0
lang zia
entry main.zia
windows-install-scope user
windows-install-dir WinScopeRoot
windows-sign off
windows-sign-thumbprint ABCDEFFE00112233445566778899AABBCCDDEEFF
file-assoc .zap \"ZAPS Project\" text/x-zaps --open-project
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_windows_scope_project}" --target windows --dry-run
        RESULT_VARIABLE _windows_scope_rv
        OUTPUT_VARIABLE _windows_scope_out
        ERROR_VARIABLE _windows_scope_err)
if (NOT _windows_scope_rv EQUAL 0)
    message(FATAL_ERROR "Windows user-scope dry-run should succeed\nstdout:\n${_windows_scope_out}\nstderr:\n${_windows_scope_err}")
endif ()
if (NOT _windows_scope_out MATCHES "Windows install scope: user")
    message(FATAL_ERROR "Windows dry-run did not report user install scope\nstdout:\n${_windows_scope_out}\nstderr:\n${_windows_scope_err}")
endif ()
if (NOT _windows_scope_out MATCHES "Windows install directory: WinScopeRoot")
    message(FATAL_ERROR "Windows dry-run did not report custom install directory\nstdout:\n${_windows_scope_out}\nstderr:\n${_windows_scope_err}")
endif ()
if (NOT _windows_scope_out MATCHES "Windows signing thumbprint: ABCDEFFE00112233445566778899AABBCCDDEEFF")
    message(FATAL_ERROR "Windows dry-run did not report signing thumbprint\nstdout:\n${_windows_scope_out}\nstderr:\n${_windows_scope_err}")
endif ()
if (NOT _windows_scope_out MATCHES "Windows open args: --open-project")
    message(FATAL_ERROR "Windows dry-run did not report file-association open arguments\nstdout:\n${_windows_scope_out}\nstderr:\n${_windows_scope_err}")
endif ()

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_windows_scope_project}" --target windows --dry-run --windows-install-scope machine
        RESULT_VARIABLE _windows_scope_cli_rv
        OUTPUT_VARIABLE _windows_scope_cli_out
        ERROR_VARIABLE _windows_scope_cli_err)
if (NOT _windows_scope_cli_rv EQUAL 0)
    message(FATAL_ERROR "Windows CLI scope override dry-run should succeed\nstdout:\n${_windows_scope_cli_out}\nstderr:\n${_windows_scope_cli_err}")
endif ()
if (NOT _windows_scope_cli_out MATCHES "Windows install scope: machine")
    message(FATAL_ERROR "Windows CLI scope override was not reported\nstdout:\n${_windows_scope_cli_out}\nstderr:\n${_windows_scope_cli_err}")
endif ()

set(_windows_thumbprint_only_project "${TEST_WORK_DIR}/windows-thumbprint-only-project")
file(MAKE_DIRECTORY "${_windows_thumbprint_only_project}")
file(WRITE "${_windows_thumbprint_only_project}/main.zia" "func start() {}\n")
file(WRITE "${_windows_thumbprint_only_project}/zanna.project"
        "project winthumbonly
version 1.0.0
lang zia
entry main.zia
windows-sign-thumbprint ABCDEFFE00112233445566778899AABBCCDDEEFF
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_windows_thumbprint_only_project}" --target windows --dry-run
        RESULT_VARIABLE _windows_thumbprint_only_rv
        OUTPUT_VARIABLE _windows_thumbprint_only_out
        ERROR_VARIABLE _windows_thumbprint_only_err)
if (NOT _windows_thumbprint_only_rv EQUAL 0)
    message(FATAL_ERROR "Windows thumbprint-only dry-run should succeed\nstdout:\n${_windows_thumbprint_only_out}\nstderr:\n${_windows_thumbprint_only_err}")
endif ()
if (_windows_thumbprint_only_err MATCHES "no package-\\* directives")
    message(FATAL_ERROR "Windows thumbprint-only manifest was treated as missing package directives\nstdout:\n${_windows_thumbprint_only_out}\nstderr:\n${_windows_thumbprint_only_err}")
endif ()
if (NOT _windows_thumbprint_only_out MATCHES "Windows signing thumbprint: ABCDEFFE00112233445566778899AABBCCDDEEFF")
    message(FATAL_ERROR "Windows thumbprint-only dry-run did not report signing thumbprint\nstdout:\n${_windows_thumbprint_only_out}\nstderr:\n${_windows_thumbprint_only_err}")
endif ()

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_windows_scope_project}" --target windows --dry-run --windows-sign-thumbprint 1234
        RESULT_VARIABLE _bad_windows_thumb_rv
        OUTPUT_VARIABLE _bad_windows_thumb_out
        ERROR_VARIABLE _bad_windows_thumb_err)
if (_bad_windows_thumb_rv EQUAL 0)
    message(FATAL_ERROR "dry-run with invalid Windows signing thumbprint should fail")
endif ()
if (NOT _bad_windows_thumb_err MATCHES "SHA-1 thumbprint")
    message(FATAL_ERROR "bad Windows thumbprint diagnostic did not mention SHA-1\nstdout:\n${_bad_windows_thumb_out}\nstderr:\n${_bad_windows_thumb_err}")
endif ()

set(_bad_windows_scope_project "${TEST_WORK_DIR}/bad-windows-scope-project")
file(MAKE_DIRECTORY "${_bad_windows_scope_project}")
file(WRITE "${_bad_windows_scope_project}/main.zia" "func start() {}\n")
file(WRITE "${_bad_windows_scope_project}/zanna.project"
        "project badwinscope
version 1.0.0
lang zia
entry main.zia
windows-install-scope portable
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_bad_windows_scope_project}" --target windows --dry-run
        RESULT_VARIABLE _bad_windows_scope_rv
        OUTPUT_VARIABLE _bad_windows_scope_out
        ERROR_VARIABLE _bad_windows_scope_err)
if (_bad_windows_scope_rv EQUAL 0)
    message(FATAL_ERROR "dry-run with invalid Windows install scope should fail")
endif ()
if (NOT _bad_windows_scope_err MATCHES "windows-install-scope")
    message(FATAL_ERROR "bad Windows scope diagnostic did not mention the directive\nstdout:\n${_bad_windows_scope_out}\nstderr:\n${_bad_windows_scope_err}")
endif ()

set(_no_version_project "${TEST_WORK_DIR}/no-version-project")
file(MAKE_DIRECTORY "${_no_version_project}")
file(WRITE "${_no_version_project}/main.zia" "func start() {}\n")
file(WRITE "${_no_version_project}/zanna.project"
        "project noversionpkg
lang zia
entry main.zia
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_no_version_project}" --target tarball --dry-run
        RESULT_VARIABLE _no_version_rv
        OUTPUT_VARIABLE _no_version_out
        ERROR_VARIABLE _no_version_err)
if (NOT _no_version_rv EQUAL 0)
    message(FATAL_ERROR "dry-run without a version should use the package fallback version\nstdout:\n${_no_version_out}\nstderr:\n${_no_version_err}")
endif ()
if (NOT _no_version_out MATCHES "noversionpkg-0\\.0\\.0-tarball")
    message(FATAL_ERROR "default output path did not use fallback version 0.0.0\nstdout:\n${_no_version_out}\nstderr:\n${_no_version_err}")
endif ()

set(_portable_version_project "${TEST_WORK_DIR}/portable-version-project")
file(MAKE_DIRECTORY "${_portable_version_project}")
file(WRITE "${_portable_version_project}/main.zia" "func start() {}\n")
file(WRITE "${_portable_version_project}/zanna.project"
        "project portableversion
version 1^2
lang zia
entry main.zia
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_portable_version_project}" --target tarball --dry-run --json
        RESULT_VARIABLE _portable_version_rv
        OUTPUT_VARIABLE _portable_version_out
        ERROR_VARIABLE _portable_version_err)
if (NOT _portable_version_rv EQUAL 0)
    message(FATAL_ERROR "tarball dry-run should accept portable non-Debian versions\nstdout:\n${_portable_version_out}\nstderr:\n${_portable_version_err}")
endif ()
if (NOT _portable_version_out MATCHES "\"target\"")
    message(FATAL_ERROR "package --dry-run --json did not emit JSON-looking output\nstdout:\n${_portable_version_out}\nstderr:\n${_portable_version_err}")
endif ()

set(_rpm_version_project "${TEST_WORK_DIR}/rpm-version-project")
file(MAKE_DIRECTORY "${_rpm_version_project}")
file(WRITE "${_rpm_version_project}/main.zia" "func start() {}\n")
file(WRITE "${_rpm_version_project}/zanna.project"
        "project rpmversion
version 1^2
lang zia
entry main.zia
package-rpm-depends libX11 >= 1.8
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_rpm_version_project}" --target rpm --dry-run
        RESULT_VARIABLE _rpm_version_rv
        OUTPUT_VARIABLE _rpm_version_out
        ERROR_VARIABLE _rpm_version_err)
if (NOT _rpm_version_rv EQUAL 0)
    message(FATAL_ERROR "RPM dry-run should use RPM version/dependency validation\nstdout:\n${_rpm_version_out}\nstderr:\n${_rpm_version_err}")
endif ()

file(WRITE "${TEST_WORK_DIR}/not-an-installer.zip" "not an installer\n")
execute_process(
        COMMAND "${ZANNA_BIN}" install-package --verify-only "${TEST_WORK_DIR}/not-an-installer.zip"
        RESULT_VARIABLE _verify_unknown_rv
        OUTPUT_VARIABLE _verify_unknown_out
        ERROR_VARIABLE _verify_unknown_err)
if (_verify_unknown_rv EQUAL 0)
    message(FATAL_ERROR "install-package --verify-only should reject unknown extensions")
endif ()
if (NOT _verify_unknown_err MATCHES "cannot infer")
    message(FATAL_ERROR "unknown verify-only extension diagnostic did not mention inference\nstdout:\n${_verify_unknown_out}\nstderr:\n${_verify_unknown_err}")
endif ()

execute_process(
        COMMAND "${ZANNA_BIN}" install-package --target appimage --stage-dir "${TEST_WORK_DIR}"
        RESULT_VARIABLE _legacy_toolchain_target_rv
        OUTPUT_VARIABLE _legacy_toolchain_target_out
        ERROR_VARIABLE _legacy_toolchain_target_err)
if (_legacy_toolchain_target_rv EQUAL 0)
    message(FATAL_ERROR "install-package --target appimage must not remain a toolchain alias")
endif ()
if (NOT _legacy_toolchain_target_err MATCHES "unknown install-package target 'appimage'")
    message(FATAL_ERROR
            "removed toolchain alias produced the wrong diagnostic\nstdout:\n${_legacy_toolchain_target_out}\nstderr:\n${_legacy_toolchain_target_err}")
endif ()

file(WRITE "${TEST_WORK_DIR}/not-a-toolchain.AppImage" "not a toolchain bundle\n")
execute_process(
        COMMAND "${ZANNA_BIN}" install-package --verify-only "${TEST_WORK_DIR}/not-a-toolchain.AppImage"
        RESULT_VARIABLE _legacy_toolchain_suffix_rv
        OUTPUT_VARIABLE _legacy_toolchain_suffix_out
        ERROR_VARIABLE _legacy_toolchain_suffix_err)
if (_legacy_toolchain_suffix_rv EQUAL 0)
    message(FATAL_ERROR "install-package verification must not retain the .AppImage alias")
endif ()
if (NOT _legacy_toolchain_suffix_err MATCHES "cannot infer")
    message(FATAL_ERROR
            "removed .AppImage inference alias produced the wrong diagnostic\nstdout:\n${_legacy_toolchain_suffix_out}\nstderr:\n${_legacy_toolchain_suffix_err}")
endif ()

execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env SOURCE_DATE_EPOCH=1700000000
                "${ZANNA_BIN}" install-package --stage-dir "${TEST_WORK_DIR}"
                --target tarball --release --no-verify
        RESULT_VARIABLE _release_bypass_rv
        OUTPUT_VARIABLE _release_bypass_out
        ERROR_VARIABLE _release_bypass_err)
if (_release_bypass_rv EQUAL 0 OR NOT _release_bypass_err MATCHES "--release forbids --no-verify")
    message(FATAL_ERROR
            "release verification bypass was not rejected\nstdout:\n${_release_bypass_out}\nstderr:\n${_release_bypass_err}")
endif ()

execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=SOURCE_DATE_EPOCH
                "${ZANNA_BIN}" install-package --stage-dir "${TEST_WORK_DIR}"
                --target tarball --release
        RESULT_VARIABLE _release_epoch_rv
        OUTPUT_VARIABLE _release_epoch_out
        ERROR_VARIABLE _release_epoch_err)
if (_release_epoch_rv EQUAL 0 OR NOT _release_epoch_err MATCHES "numeric SOURCE_DATE_EPOCH")
    message(FATAL_ERROR
            "release without SOURCE_DATE_EPOCH was not rejected\nstdout:\n${_release_epoch_out}\nstderr:\n${_release_epoch_err}")
endif ()

execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env SOURCE_DATE_EPOCH=1700000000
                "${ZANNA_BIN}" install-package --verify-only "${TEST_WORK_DIR}/not-an-installer.zip"
                --release
        RESULT_VARIABLE _release_verify_rv
        OUTPUT_VARIABLE _release_verify_out
        ERROR_VARIABLE _release_verify_err)
if (_release_verify_rv EQUAL 0 OR NOT _release_verify_err MATCHES "--release is a generation mode")
    message(FATAL_ERROR
            "release mode was incorrectly accepted for verify-only\nstdout:\n${_release_verify_out}\nstderr:\n${_release_verify_err}")
endif ()

execute_process(
        COMMAND "${ZANNA_BIN}" install-package --stage-dir "${TEST_WORK_DIR}"
                --target windows --stage-only --windows-channel stable
        RESULT_VARIABLE _local_stable_rv
        OUTPUT_VARIABLE _local_stable_out
        ERROR_VARIABLE _local_stable_err)
if (_local_stable_rv EQUAL 0 OR
   NOT _local_stable_err MATCHES "stable Windows channel is reserved for --release")
    message(FATAL_ERROR
            "local package could claim the stable Windows identity\nstdout:\n${_local_stable_out}\nstderr:\n${_local_stable_err}")
endif ()

execute_process(
        COMMAND "${ZANNA_BIN}" install-package --stage-dir "${TEST_WORK_DIR}"
                --target windows --stage-only --windows-channel "-unsafe"
        RESULT_VARIABLE _unsafe_channel_rv
        OUTPUT_VARIABLE _unsafe_channel_out
        ERROR_VARIABLE _unsafe_channel_err)
if (_unsafe_channel_rv EQUAL 0 OR
   NOT _unsafe_channel_err MATCHES "Windows release channel must be")
    message(FATAL_ERROR
            "unsafe Windows channel was accepted during stage-only validation\nstdout:\n${_unsafe_channel_out}\nstderr:\n${_unsafe_channel_err}")
endif ()

execute_process(
        COMMAND "${ZANNA_BIN}" install-package --stage-dir "${TEST_WORK_DIR}"
                --target tarball --require-checksum
        RESULT_VARIABLE _generation_checksum_rv
        OUTPUT_VARIABLE _generation_checksum_out
        ERROR_VARIABLE _generation_checksum_err)
if (_generation_checksum_rv EQUAL 0 OR NOT _generation_checksum_err MATCHES "--require-checksum requires --verify-only")
    message(FATAL_ERROR
            "generation silently accepted verify-only checksum semantics\nstdout:\n${_generation_checksum_out}\nstderr:\n${_generation_checksum_err}")
endif ()

execute_process(
        COMMAND "${ZANNA_BIN}" install-package --verify-only "${TEST_WORK_DIR}/not-an-installer.zip" --windows-sign-thumbprint 1234
        RESULT_VARIABLE _install_bad_thumb_rv
        OUTPUT_VARIABLE _install_bad_thumb_out
        ERROR_VARIABLE _install_bad_thumb_err)
if (_install_bad_thumb_rv EQUAL 0)
    message(FATAL_ERROR "install-package with invalid Windows signing thumbprint should fail")
endif ()
if (NOT _install_bad_thumb_err MATCHES "SHA-1 thumbprint")
    message(FATAL_ERROR "install-package bad thumbprint diagnostic did not mention SHA-1\nstdout:\n${_install_bad_thumb_out}\nstderr:\n${_install_bad_thumb_err}")
endif ()

# ---------------------------------------------------------------------------
# Store depot targets (ADR 0354). Dry runs need no executable or redistributable
# binary, so these contracts run on every host.
# ---------------------------------------------------------------------------

function(_expect_contains haystack needle context)
    string(FIND "${haystack}" "${needle}" _pos)
    if (_pos EQUAL -1)
        message(FATAL_ERROR "${context}: expected to find '${needle}' in:\n${haystack}")
    endif ()
endfunction()

set(_steam_project "${TEST_WORK_DIR}/steam-project")
file(MAKE_DIRECTORY "${_steam_project}/data")
file(WRITE "${_steam_project}/main.zia" "func start() {}\n")
file(WRITE "${_steam_project}/data/readme.txt" "hello\n")
file(WRITE "${_steam_project}/zanna.project"
        "project steamdepot
version 2.0.0
lang zia
entry main.zia
package-name \"Steam Depot\"
asset data data
steam-app-id 480
steam-depot linux 482
steam-depot macos 483
steam-build-description \"Release candidate\"
steam-set-live beta
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_steam_project}" --target steam-linux --arch x64
                --dry-run --json -o "${TEST_WORK_DIR}/steam-root"
        RESULT_VARIABLE _steam_json_rv
        OUTPUT_VARIABLE _steam_json_out
        ERROR_VARIABLE _steam_json_err)
if (NOT _steam_json_rv EQUAL 0)
    message(FATAL_ERROR "steam-linux JSON dry-run should succeed\nstdout:\n${_steam_json_out}\nstderr:\n${_steam_json_err}")
endif ()
# Depot plan paths use native separators; Windows hosts emit them JSON-escaped
# (`\\`), so compare path suffixes against a forward-slash spelling.
string(REPLACE "\\\\" "/" _steam_json_paths "${_steam_json_out}")
foreach (_needle
        "\"target\": \"steam-linux\""
        "\"steam\": {"
        "\"appId\": \"480\""
        "\"platform\": \"linux\""
        "\"depotId\": \"482\""
        "\"launch\": \"steamdepot\""
        "\"redistributable\": null"
        "\"buildDescription\": \"Release candidate\""
        "\"setLive\": \"beta\""
        "scripts/app_build_480.vdf\""
        "manifests/linux.json\""
        "\"usesProvider\": null")
    _expect_contains("${_steam_json_paths}" "${_needle}" "steam-linux JSON dry-run")
endforeach ()

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_steam_project}" --target steam-macos --dry-run
                -o "${TEST_WORK_DIR}/steam-root"
        RESULT_VARIABLE _steam_text_rv
        OUTPUT_VARIABLE _steam_text_out
        ERROR_VARIABLE _steam_text_err)
if (NOT _steam_text_rv EQUAL 0)
    message(FATAL_ERROR "steam-macos dry-run should succeed\nstdout:\n${_steam_text_out}\nstderr:\n${_steam_text_err}")
endif ()
foreach (_needle
        "Steam depot: 483 (macos)"
        "Steam launch path: Steam Depot.app"
        "Steam entitlements: com.apple.security.cs.disable-library-validation com.apple.security.cs.allow-dyld-environment-variables")
    _expect_contains("${_steam_text_out}" "${_needle}" "steam-macos dry-run")
endforeach ()

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_steam_project}" --target steam-windows --arch arm64 --dry-run
        RESULT_VARIABLE _steam_arm_rv
        OUTPUT_VARIABLE _steam_arm_out
        ERROR_VARIABLE _steam_arm_err)
if (_steam_arm_rv EQUAL 0)
    message(FATAL_ERROR "steam-windows arm64 dry-run should fail")
endif ()
_expect_contains("${_steam_arm_err}"
        "Steam Windows depots are x64-only: Valve ships no Windows arm64 Steamworks redistributable"
        "steam-windows arm64")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_steam_project}" --target steam-linux --arch x64 --dry-run
                --steam-redist "${TEST_WORK_DIR}/no-such-sdk"
        RESULT_VARIABLE _steam_redist_rv
        OUTPUT_VARIABLE _steam_redist_out
        ERROR_VARIABLE _steam_redist_err)
if (_steam_redist_rv EQUAL 0)
    message(FATAL_ERROR "steam-linux dry-run with a missing redistributable should fail")
endif ()
_expect_contains("${_steam_redist_err}" "Steam redistributable not found: expected " "missing redistributable")
_expect_contains("${_steam_redist_err}" "(from --steam-redist '" "missing redistributable origin")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_steam_project}" --target tarball --dry-run
                --steam-redist "${TEST_WORK_DIR}/no-such-sdk"
        RESULT_VARIABLE _steam_wrong_target_rv
        OUTPUT_VARIABLE _steam_wrong_target_out
        ERROR_VARIABLE _steam_wrong_target_err)
if (_steam_wrong_target_rv EQUAL 0)
    message(FATAL_ERROR "--steam-redist with a non-Steam target should fail")
endif ()
_expect_contains("${_steam_wrong_target_err}"
        "--steam-redist applies only to --target steam-windows, steam-macos, or steam-linux"
        "--steam-redist target check")

set(_steam_no_app "${TEST_WORK_DIR}/steam-no-app")
file(MAKE_DIRECTORY "${_steam_no_app}")
file(WRITE "${_steam_no_app}/main.zia" "func start() {}\n")
file(WRITE "${_steam_no_app}/zanna.project"
        "project steamnoapp
version 1.0.0
lang zia
entry main.zia
")
execute_process(
        COMMAND "${ZANNA_BIN}" package "${_steam_no_app}" --target steam-linux --arch x64 --dry-run
        RESULT_VARIABLE _steam_no_app_rv
        OUTPUT_VARIABLE _steam_no_app_out
        ERROR_VARIABLE _steam_no_app_err)
if (_steam_no_app_rv EQUAL 0)
    message(FATAL_ERROR "steam-linux dry-run without steam-app-id should fail")
endif ()
_expect_contains("${_steam_no_app_err}" "Steam depot packaging requires steam-app-id in zanna.project"
        "missing steam-app-id")

set(_steam_bad_branch "${TEST_WORK_DIR}/steam-bad-branch")
file(MAKE_DIRECTORY "${_steam_bad_branch}")
file(WRITE "${_steam_bad_branch}/main.zia" "func start() {}\n")
file(WRITE "${_steam_bad_branch}/zanna.project"
        "project steambadbranch
version 1.0.0
lang zia
entry main.zia
steam-app-id 480
steam-set-live default
")
execute_process(
        COMMAND "${ZANNA_BIN}" package "${_steam_bad_branch}" --target steam-linux --arch x64 --dry-run
        RESULT_VARIABLE _steam_bad_branch_rv
        OUTPUT_VARIABLE _steam_bad_branch_out
        ERROR_VARIABLE _steam_bad_branch_err)
if (_steam_bad_branch_rv EQUAL 0)
    message(FATAL_ERROR "steam-set-live default should be rejected")
endif ()
_expect_contains("${_steam_bad_branch_err}"
        "steam-set-live cannot be 'default': Steam sets the default branch live only through the App Admin panel"
        "steam-set-live default")

# A host's secondary native formats (dmg on macOS, linux-bundle on Linux) compile their
# own payload: the host check compares operating systems, not target names. A project
# that fails to compile proves the command got past that check without packaging anything.
if (CMAKE_HOST_APPLE)
    set(_host_family_target dmg)
elseif (CMAKE_HOST_UNIX)
    set(_host_family_target linux-bundle)
endif ()
if (DEFINED _host_family_target)
    set(_host_family_project "${TEST_WORK_DIR}/host-family-project")
    file(MAKE_DIRECTORY "${_host_family_project}")
    file(WRITE "${_host_family_project}/main.zia" "func start() {\n    var x: Integer = \"text\";\n}\n")
    file(WRITE "${_host_family_project}/zanna.project"
            "project hostfamily
version 1.0.0
lang zia
entry main.zia
")
    execute_process(
            COMMAND "${ZANNA_BIN}" package "${_host_family_project}" --target ${_host_family_target}
                    -o "${TEST_WORK_DIR}/host-family-output"
            RESULT_VARIABLE _host_family_rv
            OUTPUT_VARIABLE _host_family_out
            ERROR_VARIABLE _host_family_err)
    if (_host_family_rv EQUAL 0)
        message(FATAL_ERROR "packaging a project that does not compile should fail")
    endif ()
    string(FIND "${_host_family_err}" "requires --executable" _host_family_pos)
    if (NOT _host_family_pos EQUAL -1)
        message(FATAL_ERROR "--target ${_host_family_target} refused to compile on its own host\nstderr:\n${_host_family_err}")
    endif ()
    _expect_contains("${_host_family_err}" "error: compilation failed" "host-family compile")
endif ()

# ---------------------------------------------------------------------------
# Pack groups travel with every package (ADR 0355): dry runs name the files.
# ---------------------------------------------------------------------------

set(_packs_project "${TEST_WORK_DIR}/packs-project")
file(MAKE_DIRECTORY "${_packs_project}/levels")
file(WRITE "${_packs_project}/main.zia" "func start() {}\n")
file(WRITE "${_packs_project}/levels/one.txt" "level one\n")
file(WRITE "${_packs_project}/zanna.project"
        "project packsproject
version 1.0.0
lang zia
entry main.zia
pack-compressed Levels levels
")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_packs_project}" --target tarball --dry-run --json
        RESULT_VARIABLE _packs_json_rv
        OUTPUT_VARIABLE _packs_json_out
        ERROR_VARIABLE _packs_json_err)
if (NOT _packs_json_rv EQUAL 0)
    message(FATAL_ERROR "pack dry-run should succeed\nstdout:\n${_packs_json_out}\nstderr:\n${_packs_json_err}")
endif ()
_expect_contains("${_packs_json_out}" "\"packs\": [{\"group\":\"Levels\",\"file\":\"packsproject-levels.zpak\"}]"
        "pack dry-run JSON")

execute_process(
        COMMAND "${ZANNA_BIN}" package "${_packs_project}" --target linux --dry-run
        RESULT_VARIABLE _packs_text_rv
        OUTPUT_VARIABLE _packs_text_out
        ERROR_VARIABLE _packs_text_err)
if (NOT _packs_text_rv EQUAL 0)
    message(FATAL_ERROR "pack text dry-run should succeed\nstdout:\n${_packs_text_out}\nstderr:\n${_packs_text_err}")
endif ()
_expect_contains("${_packs_text_out}" "  Pack: Levels -> packsproject-levels.zpak" "pack dry-run text")
