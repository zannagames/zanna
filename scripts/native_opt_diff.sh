#!/bin/bash
# native_opt_diff.sh
# Differential harness for native codegen optimization levels: builds a Zia/
# BASIC target twice (-O0 and -O2), runs both binaries with identical
# arguments, and diffs stdout and exit codes. Divergence means an optimizer
# or lowering correctness bug (the ridgebound BUG-006 / struct-return class).
#
# Usage: ./scripts/native_opt_diff.sh <target> [-- program-args...]
#   <target>  A .zia/.bas file, project directory, or zanna.project path.
#
# Environment:
#   ZANNA_BIN   Override the zanna binary (default: build/src/tools/zanna/zanna)
#   OPT_A/OPT_B Override the compared levels (default: -O0 vs -O2)
#   Backend switches are inherited by every build: the ZANNA_NO_* kill switches
#   bisect a divergence to one stage.
#
# Exit codes: 0 outputs match · 1 divergence · 2 usage/build failure.

set -u

if [ $# -lt 1 ]; then
    echo "usage: $0 <target> [-- program-args...]" >&2
    exit 2
fi

TARGET="$1"
shift
if [ "${1:-}" = "--" ]; then
    shift
fi

ZANNA_BIN="${ZANNA_BIN:-build/src/tools/zanna/zanna}"
OPT_A="${OPT_A:--O0}"
OPT_B="${OPT_B:--O2}"

if [ ! -x "$ZANNA_BIN" ]; then
    echo "ERROR: zanna binary not found at $ZANNA_BIN (set ZANNA_BIN or build first)" >&2
    exit 2
fi

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/zanna_opt_diff.XXXXXX")" || exit 2
trap 'rm -rf "$WORKDIR"' EXIT

BIN_A="$WORKDIR/bin_a"
BIN_B="$WORKDIR/bin_b"

build_one() {
    # $1 = opt flag, $2 = output path
    if ! "$ZANNA_BIN" build "$TARGET" -o "$2" "$1" --quiet-warnings \
        > "$WORKDIR/build_$1.log" 2>&1; then
        echo "ERROR: build $1 failed:" >&2
        cat "$WORKDIR/build_$1.log" >&2
        exit 2
    fi
}

build_one "$OPT_A" "$BIN_A"
build_one "$OPT_B" "$BIN_B"

"$BIN_A" "$@" > "$WORKDIR/out_a.txt" 2> "$WORKDIR/err_a.txt"
RC_A=$?
"$BIN_B" "$@" > "$WORKDIR/out_b.txt" 2> "$WORKDIR/err_b.txt"
RC_B=$?

FAIL=0
if [ "$RC_A" -ne "$RC_B" ]; then
    echo "DIVERGENCE: exit codes differ ($OPT_A: $RC_A, $OPT_B: $RC_B)"
    FAIL=1
fi
if ! cmp -s "$WORKDIR/out_a.txt" "$WORKDIR/out_b.txt"; then
    echo "DIVERGENCE: stdout differs between $OPT_A and $OPT_B:"
    diff "$WORKDIR/out_a.txt" "$WORKDIR/out_b.txt" | head -40
    FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
    echo "FAIL: $TARGET diverges between $OPT_A and $OPT_B"
    echo "stderr ($OPT_A):"; head -5 "$WORKDIR/err_a.txt"
    echo "stderr ($OPT_B):"; head -5 "$WORKDIR/err_b.txt"
    exit 1
fi

echo "PASS: $TARGET identical output at $OPT_A and $OPT_B (exit $RC_A)"
exit 0
