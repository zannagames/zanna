#!/usr/bin/env bash
# Script: update_goldens.sh
# Purpose: Re-run golden tests with UPDATE_GOLDEN=1 to overwrite expected
#          output files with actual output. Use after intentional changes
#          to compiler diagnostics, IL output, or optimizer behavior.
#
# Usage:
#   ./scripts/update_goldens.sh              # Update all golden files
#   ./scripts/update_goldens.sh il_opt       # Update only IL optimizer goldens
#   ./scripts/update_goldens.sh basic_error  # Update only BASIC error goldens
#
# The script re-runs each failing golden test with UPDATE_GOLDEN=1 exported.
# Both harness kinds honour it: the cmake runner scripts through
# src/tests/golden/GoldenUpdateMode.cmake, and compiled runners such as
# test_basic_to_il_goldens_batch directly. Tests that already pass are skipped.
# Some failures cannot be refreshed and are reported for manual inspection:
# contains-style cases have no golden file, regex expectations must not be
# replaced with literal output, and a compile failure has nothing to record.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"

if [ ! -d "${BUILD_DIR}" ]; then
    echo "Error: Build directory not found at ${BUILD_DIR}"
    echo "Run the appropriate build script first (for example ./scripts/build_zanna_linux.sh or ./scripts/build_zanna_mac.sh)."
    exit 1
fi

FILTER="${1:-}"

# Runners that honour UPDATE_GOLDEN. Tests are selected by which runner drives
# them rather than by CTest label: refreshable goldens are spread across the
# golden, il, ilopt, e2e, oop, conformance, and runtime labels, so querying
# -L golden silently skips a third of them.
GOLDEN_RUNNERS='check_il\.cmake|check_il_bounds\.cmake|check_error\.cmake'
GOLDEN_RUNNERS="${GOLDEN_RUNNERS}|check_run_output\.cmake|check_basic_run_output\.cmake"
GOLDEN_RUNNERS="${GOLDEN_RUNNERS}|check_constfold\.cmake|check_opt\.cmake"
GOLDEN_RUNNERS="${GOLDEN_RUNNERS}|test_basic_to_il_goldens_batch"

# `ctest -N -V` prints `<num>: Test command: ...` before `  Test #<num>: <name>`,
# so match commands first and translate the numbers to names afterwards.
CTEST_DUMP="$(mktemp)"
trap 'rm -f "${CTEST_DUMP}"' EXIT
ctest --test-dir "${BUILD_DIR}" -N -V > "${CTEST_DUMP}" 2>/dev/null || true

TESTS=""
for num in $(grep -E "^[0-9]+: Test command: .*(${GOLDEN_RUNNERS})" "${CTEST_DUMP}" \
        | sed 's/:.*//' | sort -u); do
    name=$(grep -E "^ *Test #${num}: " "${CTEST_DUMP}" | sed "s/^ *Test #${num}: //" | head -1)
    [ -n "${name}" ] || continue
    if [ -n "${FILTER}" ]; then
        case "${name}" in
            *"${FILTER}"*) ;;
            *) continue ;;
        esac
    fi
    TESTS="${TESTS}${name}
"
done
TESTS=$(printf '%s' "${TESTS}")

if [ -z "${TESTS}" ]; then
    echo "No golden tests found${FILTER:+ matching '${FILTER}'}."
    exit 0
fi

TOTAL=$(echo "${TESTS}" | wc -l | tr -d ' ')
UPDATED=0
SKIPPED=0
FAILED=0

echo "Checking ${TOTAL} golden tests${FILTER:+ matching '${FILTER}'}..."
echo ""

for test_name in ${TESTS}; do
    # First, run normally to see if it passes
    if ctest --test-dir "${BUILD_DIR}" -R "^${test_name}$" --timeout 30 > /dev/null 2>&1; then
        SKIPPED=$((SKIPPED + 1))
        continue
    fi

    # Test failed — re-run its recorded command with UPDATE_GOLDEN=1 exported.
    # Splicing -DUPDATE_GOLDEN=1 into that command instead is not viable: ctest
    # quotes every argument (`"-P"`), and compiled runners have no -P to splice
    # before. Look the command up by test number, because an Environment
    # variables block separates it from the name line in the dump.
    TEST_NUM=$(grep -E "^ *Test #[0-9]+: ${test_name}\$" "${CTEST_DUMP}" \
        | head -1 | sed 's/^ *Test #//; s/:.*//')
    TEST_CMD=""
    if [ -n "${TEST_NUM}" ]; then
        TEST_CMD=$(grep -E "^${TEST_NUM}: Test command: " "${CTEST_DUMP}" \
            | head -1 | sed 's/.*Test command: //')
    fi

    if [ -z "${TEST_CMD}" ]; then
        echo "  SKIP  ${test_name} (could not extract command)"
        FAILED=$((FAILED + 1))
        continue
    fi

    # Export inside a subshell so the pass/fail probe above never sees it.
    if (export UPDATE_GOLDEN=1; eval "${TEST_CMD}") > /dev/null 2>&1; then
        echo "  UPDATE  ${test_name}"
        UPDATED=$((UPDATED + 1))
    else
        # Not every failing golden test can be refreshed: contains-style cases
        # have no golden file, regex expectations must not be overwritten with
        # literal output, and a compile failure has nothing to record.
        echo "  FAIL    ${test_name} (cannot refresh automatically; inspect manually)"
        FAILED=$((FAILED + 1))
    fi
done

echo ""
echo "Done: ${UPDATED} updated, ${SKIPPED} already passing, ${FAILED} failed."
