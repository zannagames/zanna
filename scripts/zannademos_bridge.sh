#!/bin/sh
#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/zannademos_bridge.sh
# Purpose: Bridge CTest to the zannademos demo-test runner when the demos
#          repo is cloned at the documented nested location.
# Key invariants:
#   - Absence of the demos clone is a visible skip (exit 0 with a skip
#     message matched by SKIP_REGULAR_EXPRESSION), never a failure.
#   - The full lane runs only when ZANNA_RUN_DEMOS_FULL=1 to keep default
#     ctest wall time bounded.
# Ownership/Lifetime:
#   - The zannademos runner owns all demo test execution and temp files.
# Links: src/tests/CMakeLists.txt (zannademos_smoke, zannademos_full),
#        zannademos/scripts/run_demo_tests.sh, docs/adr/0241
#
#===----------------------------------------------------------------------===#

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
RUNNER=$ROOT/zannademos/scripts/run_demo_tests.sh

MODE="fast"
ZANNA_ARG=""

usage() {
    echo "Usage: scripts/zannademos_bridge.sh [--fast|--full] [--zanna PATH]" >&2
}

while [ $# -gt 0 ]; do
    case $1 in
        --fast) MODE="fast" ;;
        --full) MODE="full" ;;
        --zanna)
            [ $# -ge 2 ] || { echo "error: --zanna requires a path" >&2; exit 1; }
            ZANNA_ARG=$2
            shift
            ;;
        *) echo "error: unknown option '$1'" >&2; usage; exit 1 ;;
    esac
    shift
done

if [ ! -f "$RUNNER" ]; then
    echo "Skipping zannademos suite: demos clone not present at $ROOT/zannademos"
    exit 0
fi

if [ "$MODE" = "full" ] && [ "${ZANNA_RUN_DEMOS_FULL:-0}" != "1" ]; then
    echo "Skipping zannademos full suite: set ZANNA_RUN_DEMOS_FULL=1 to enable"
    exit 0
fi

set --
if [ "$MODE" = "fast" ]; then
    set -- --fast
fi
if [ -n "$ZANNA_ARG" ]; then
    set -- "$@" --zanna "$ZANNA_ARG"
fi

exec sh "$RUNNER" "$@"
