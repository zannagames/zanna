# SPDX-License-Identifier: GPL-3.0-only
# File: tests/e2e/differential_opt_levels.cmake
# Purpose: Native -O0 vs -O2 determinism gate. Runs one IL program through the
#          native backend twice (`zanna codegen <arch> -run-native --verify-mir`
#          at -O0 and at -O2) and asserts identical stdout AND exit code, so an
#          optimizer or post-RA rewrite that changes behaviour is caught even
#          when the VM oracle (differential_vm_native.cmake) is not run. The
#          `scripts/native_opt_diff.sh` harness is the same check for source
#          projects.
# Links: docs/internals/backend-codegen-review-2026-09.md (Phase 2.5),
#        docs/internals/architecture.md (determinism Core Principle)

if (NOT DEFINED ILC)
    message(FATAL_ERROR "ILC not set")
endif ()
if (NOT DEFINED IL_FILE)
    message(FATAL_ERROR "IL_FILE not set")
endif ()
if (NOT DEFINED ARCH)
    message(FATAL_ERROR "ARCH not set")
endif ()

# ERROR_QUIET drops the linker's "dead-strip: removed N sections" diagnostic so
# only the program's own stdout is compared.
execute_process(
    COMMAND ${ILC} codegen ${ARCH} ${IL_FILE} -run-native --verify-mir -O0
    OUTPUT_VARIABLE o0_out
    RESULT_VARIABLE o0_exit
    ERROR_QUIET)

execute_process(
    COMMAND ${ILC} codegen ${ARCH} ${IL_FILE} -run-native --verify-mir -O2
    OUTPUT_VARIABLE o2_out
    RESULT_VARIABLE o2_exit
    ERROR_QUIET)

if (NOT o0_out STREQUAL o2_out)
    message(FATAL_ERROR
        "-O0/-O2 stdout mismatch for ${IL_FILE}:\n  -O0: [${o0_out}]\n  -O2: [${o2_out}]")
endif ()
if (NOT o0_exit EQUAL o2_exit)
    message(FATAL_ERROR
        "-O0/-O2 exit-code mismatch for ${IL_FILE}: -O0=${o0_exit} -O2=${o2_exit}")
endif ()
