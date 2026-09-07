#!/usr/bin/env bash
#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/codegen_stats.sh
# Purpose: Static code-shape measurement for the native backends. Compiles the
#          IL benchmark kernels and the demo projects to assembly at -O0 and
#          -O2 for AArch64 and x86-64 with ZANNA_CODEGEN_STATS=1 and tabulates
#          the module totals (instructions, frame loads/stores, offset
#          prefixes, spill slots, frame bytes, callee-saved registers).
# Key invariants:
#   - Host-agnostic: only assembly is produced, nothing is assembled or run,
#     so AArch64 numbers can be taken on any host.
#   - Output is TSV with a fixed header; --baseline diffs a previous TSV.
#   - Demo IL is produced by `zanna front zia -emit-il`, so the numbers track
#     the frontend too; compare runs from the same commit range with care.
# Ownership/Lifetime: Temporary files live in one mktemp directory.
# Links: src/codegen/aarch64/CodegenStats.hpp, src/codegen/x86_64/CodegenStats.hpp,
#        docs/internals/backend.md (baseline table)
#
#===----------------------------------------------------------------------===#
#
# Usage: ./scripts/codegen_stats.sh [--arch arm64|x64|both] [--opt "0 2"]
#                                   [--baseline old.tsv] [--out new.tsv]
#                                   [--programs benchmarks|demos|all]
# Environment:
#   ZANNA_BIN  Override the zanna binary (default: build/src/tools/zanna/zanna)
#
# Exit codes: 0 success · 2 usage or compile failure.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
ZANNA_BIN="${ZANNA_BIN:-$ROOT_DIR/build/src/tools/zanna/zanna}"

ARCHES="arm64 x64"
OPTS="0 2"
BASELINE=""
OUT=""
PROGRAMS="all"

while [ $# -gt 0 ]; do
    case "$1" in
        --arch)
            shift
            case "${1:-}" in
                arm64|x64) ARCHES="$1" ;;
                both) ARCHES="arm64 x64" ;;
                *) echo "usage: --arch arm64|x64|both" >&2; exit 2 ;;
            esac
            ;;
        --opt) shift; OPTS="${1:-}" ;;
        --baseline) shift; BASELINE="${1:-}" ;;
        --out) shift; OUT="${1:-}" ;;
        --programs) shift; PROGRAMS="${1:-all}" ;;
        -h|--help)
            sed -n '27,35p' "$0"
            exit 0
            ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

if [ ! -x "$ZANNA_BIN" ]; then
    echo "ERROR: zanna binary not found at $ZANNA_BIN (set ZANNA_BIN or build first)" >&2
    exit 2
fi

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/zanna_codegen_stats.XXXXXX")" || exit 2
trap 'rm -rf "$WORKDIR"' EXIT

# program-name<TAB>il-path pairs, one per line.
PROGRAM_LIST="$WORKDIR/programs.txt"
: > "$PROGRAM_LIST"

if [ "$PROGRAMS" = "all" ] || [ "$PROGRAMS" = "benchmarks" ]; then
    for il in "$ROOT_DIR"/examples/il/benchmarks/*.il; do
        [ -f "$il" ] || continue
        printf '%s\t%s\n' "$(basename "$il" .il)" "$il" >> "$PROGRAM_LIST"
    done
fi

emit_demo_il() {
    # $1 = program name, $2 = main .zia
    out="$WORKDIR/$1.il"
    if ! "$ZANNA_BIN" front zia -emit-il "$2" > "$out" 2> "$WORKDIR/$1.front.log"; then
        echo "ERROR: IL emission failed for $2:" >&2
        cat "$WORKDIR/$1.front.log" >&2
        exit 2
    fi
    printf '%s\t%s\n' "$1" "$out" >> "$PROGRAM_LIST"
}

if [ "$PROGRAMS" = "all" ] || [ "$PROGRAMS" = "demos" ]; then
    emit_demo_il chess "$ROOT_DIR/examples/games/chess/main.zia"
    emit_demo_il crackman "$ROOT_DIR/examples/games/crackman/main.zia"
    emit_demo_il paint "$ROOT_DIR/examples/apps/paint/main.zia"
    emit_demo_il openworld_slice "$ROOT_DIR/examples/3d/openworld_slice/main.zia"
fi

HEADER="program	arch	opt	instrs	loads	stores	frameLoads	frameStores	offsetPrefixes	spillSlots	frameBytes	calleeSaved	moves"
RESULT="$WORKDIR/result.tsv"
printf '%s\n' "$HEADER" > "$RESULT"

# Compile one IL file and append the module-total row.
measure() {
    # $1 = program, $2 = il path, $3 = arch, $4 = opt level
    log="$WORKDIR/$1.$3.O$4.log"
    asm="$WORKDIR/$1.$3.O$4.s"
    case "$3" in
        arm64) extra="--target-darwin" ;;
        x64) extra="--target-linux --system-asm" ;;
    esac
    # shellcheck disable=SC2086
    if ! ZANNA_CODEGEN_STATS=1 "$ZANNA_BIN" codegen "$3" "$2" -S "$asm" "-O$4" $extra > "$log" 2>&1; then
        echo "ERROR: codegen $3 -O$4 failed for $1:" >&2
        tail -20 "$log" >&2
        exit 2
    fi
    grep '\[codegen-stats\] arch=' "$log" | grep ' fn=<module> ' | tail -1 |
        awk -v prog="$1" -v arch="$3" -v opt="O$4" '
        {
            for (i = 1; i <= NF; i++) {
                n = index($i, "=")
                if (n > 0) v[substr($i, 1, n - 1)] = substr($i, n + 1)
            }
            printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", prog, arch, opt,
                v["instrs"], v["loads"], v["stores"], v["frameLoads"], v["frameStores"],
                v["offsetPrefixes"], v["spillSlots"], v["frameBytes"], v["calleeSaved"], v["moves"]
        }' >> "$RESULT"
}

while IFS="$(printf '\t')" read -r prog il; do
    [ -n "$prog" ] || continue
    for arch in $ARCHES; do
        for opt in $OPTS; do
            measure "$prog" "$il" "$arch" "$opt"
        done
    done
done < "$PROGRAM_LIST"

# Stray executables the x64 -S path may leave next to the IL.
find "$WORKDIR" -maxdepth 1 -type f -perm -u+x -delete 2> /dev/null || true

if [ -n "$OUT" ]; then
    cp "$RESULT" "$OUT"
fi

if [ -z "$BASELINE" ]; then
    cat "$RESULT"
    exit 0
fi

if [ ! -f "$BASELINE" ]; then
    echo "ERROR: baseline not found: $BASELINE" >&2
    exit 2
fi

# Diff against the baseline: key = program/arch/opt; print old -> new for the
# headline counters with the percentage change.
awk -F '\t' '
    NR == FNR {
        if (FNR == 1) next
        key = $1 "/" $2 "/" $3
        old_instrs[key] = $4; old_frame[key] = $7 + $8; old_prefix[key] = $9; old_slots[key] = $10
        next
    }
    FNR == 1 {
        printf "%-18s %-5s %-3s %26s %26s %20s %18s\n", "program", "arch", "opt",
            "instrs", "frameLoads+Stores", "offsetPrefixes", "spillSlots"
        next
    }
    {
        key = $1 "/" $2 "/" $3
        if (!(key in old_instrs)) next
        printf "%-18s %-5s %-3s %s %s %s %s\n", $1, $2, $3,
            delta(old_instrs[key], $4, 26), delta(old_frame[key], $7 + $8, 26),
            delta(old_prefix[key], $9, 20), delta(old_slots[key], $10, 18)
    }
    function delta(a, b, w,    pct, s) {
        if (a + 0 == 0) pct = (b + 0 == 0) ? 0 : 100
        else pct = (b - a) * 100.0 / a
        s = sprintf("%d -> %d (%+.1f%%)", a, b, pct)
        return sprintf("%" w "s", s)
    }
' "$BASELINE" "$RESULT"
