#!/usr/bin/env bash
#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/benchmark_compare.sh
# Purpose: Compare benchmark result files and flag Zanna performance regressions.
# Key invariants:
#   - Interpreter discovery verifies that a candidate can actually execute Python.
#   - Only regressions in Zanna modes determine the script's failure status.
# Ownership/Lifetime: Temporary comparison files exist only for this invocation.
# Links: docs/internals/testing.md, src/tests/CMakeLists.txt
#
#===----------------------------------------------------------------------===#

set -euo pipefail

# ============================================================================
# Zanna Benchmark Comparison Tool
# ============================================================================
# Compares two benchmark runs and displays a color-coded delta table.
#
# Usage:
#   scripts/benchmark_compare.sh                           # latest vs baseline
#   scripts/benchmark_compare.sh <base.jsonl> <head.jsonl> # arbitrary comparison
#   scripts/benchmark_compare.sh <head.jsonl>              # head vs baseline
#   scripts/benchmark_compare.sh --run N                   # Nth run vs baseline
#
# Exit code: 1 if any Zanna mode regressed >5%, 0 otherwise.
# ============================================================================

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")"/.. && pwd)"
RESULTS_FILE="$ROOT_DIR/misc/benchmarks/results.jsonl"
BASELINE_FILE="$ROOT_DIR/misc/benchmarks/baseline.jsonl"

if [[ "${1:-}" == "--self-test" ]]; then
    tmpdir="$(mktemp -d)"
    trap "rm -rf '$tmpdir'" EXIT
    base="$tmpdir/base.jsonl"
    pass="$tmpdir/pass.jsonl"
    fail="$tmpdir/fail.jsonl"
    cat > "$base" <<'JSON'
{"version":1,"metadata":{"timestamp":"2026-01-01T00:00:00","commit":"base","platform":"test"},"benchmarks":[{"program":"arith","modes":{"bc-switch":{"success":true,"time_ms":100},"native-x86_64":{"success":true,"time_ms":200},"python3":{"success":true,"time_ms":500}}}]}
JSON
    cat > "$pass" <<'JSON'
{"version":1,"metadata":{"timestamp":"2026-01-01T00:01:00","commit":"pass","platform":"test"},"benchmarks":[{"program":"arith","modes":{"bc-switch":{"success":true,"time_ms":104},"native-x86_64":{"success":true,"time_ms":205},"python3":{"success":true,"time_ms":600}}}]}
JSON
    cat > "$fail" <<'JSON'
{"version":1,"metadata":{"timestamp":"2026-01-01T00:02:00","commit":"fail","platform":"test"},"benchmarks":[{"program":"arith","modes":{"bc-switch":{"success":true,"time_ms":111},"native-x86_64":{"success":true,"time_ms":200},"python3":{"success":true,"time_ms":500}}}]}
JSON
    "$0" "$base" "$pass" >/dev/null
    if "$0" "$base" "$fail" >/dev/null 2>&1; then
        echo "benchmark_compare self-test: expected regression was not detected" >&2
        exit 1
    fi
    echo "benchmark_compare self-test: ok"
    exit 0
fi

# --- Parse arguments ---
BASE_JSON=""
HEAD_JSON=""
RUN_N=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --run)
            RUN_N="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: scripts/benchmark_compare.sh [base.jsonl] [head.jsonl]"
            echo "       scripts/benchmark_compare.sh <head.jsonl>"
            echo "       scripts/benchmark_compare.sh --run N"
            echo "       scripts/benchmark_compare.sh --self-test"
            echo "       scripts/benchmark_compare.sh  (latest vs baseline)"
            exit 0 ;;
        *)
            if [[ -z "$BASE_JSON" ]]; then
                BASE_JSON="$1"
            elif [[ -z "$HEAD_JSON" ]]; then
                HEAD_JSON="$1"
            else
                echo "error: too many arguments" >&2; exit 1
            fi
            shift ;;
    esac
done

# --- Resolve inputs to JSON strings ---
resolve_to_json() {
    local input="$1" which="$2"
    if [[ -f "$input" ]]; then
        # It's a file — read first line (for baseline) or last line (for head)
        if [[ "$which" == "base" ]]; then
            head -1 "$input"
        else
            tail -1 "$input"
        fi
    else
        echo "$input"
    fi
}

if [[ -n "$RUN_N" ]]; then
    if [[ ! -f "$RESULTS_FILE" ]]; then echo "error: $RESULTS_FILE not found" >&2; exit 1; fi
    if [[ ! -f "$BASELINE_FILE" ]]; then echo "error: $BASELINE_FILE not found" >&2; exit 1; fi
    HEAD_JSON=$(sed -n "${RUN_N}p" "$RESULTS_FILE")
    BASE_JSON=$(head -1 "$BASELINE_FILE")
    if [[ -z "$HEAD_JSON" ]]; then echo "error: run $RUN_N not found" >&2; exit 1; fi
elif [[ -z "$BASE_JSON" && -z "$HEAD_JSON" ]]; then
    if [[ ! -f "$BASELINE_FILE" ]]; then echo "error: no baseline. Run: benchmark.sh --set-baseline" >&2; exit 1; fi
    if [[ ! -f "$RESULTS_FILE" ]]; then echo "error: no results. Run: benchmark.sh" >&2; exit 1; fi
    BASE_JSON=$(head -1 "$BASELINE_FILE")
    HEAD_JSON=$(tail -1 "$RESULTS_FILE")
elif [[ -n "$BASE_JSON" && -z "$HEAD_JSON" ]]; then
    if [[ ! -f "$BASELINE_FILE" ]]; then echo "error: no baseline. Run: benchmark.sh --set-baseline" >&2; exit 1; fi
    HEAD_JSON=$(resolve_to_json "$BASE_JSON" "head")
    BASE_JSON=$(head -1 "$BASELINE_FILE")
else
    local_base="$BASE_JSON"
    local_head="$HEAD_JSON"
    BASE_JSON=$(resolve_to_json "$local_base" "base")
    HEAD_JSON=$(resolve_to_json "$local_head" "head")
fi

# --- Write to temp files for python ---
TMPBASE=$(mktemp)
TMPHEAD=$(mktemp)
trap "rm -f '$TMPBASE' '$TMPHEAD'" EXIT
echo "$BASE_JSON" > "$TMPBASE"
echo "$HEAD_JSON" > "$TMPHEAD"

# --- Run comparison via python ---
# Windows application-execution aliases can make command -v report python3 even
# when invoking it only prints a Microsoft Store prompt. Probe each candidate
# before selecting it, and support the standard Windows py launcher as a final
# fallback.
PYTHON_CMD=()
if [[ -n "${PYTHON:-}" ]]; then
    if ! "$PYTHON" -c 'import sys' >/dev/null 2>&1; then
        echo "error: PYTHON does not name a working interpreter: $PYTHON" >&2
        exit 1
    fi
    PYTHON_CMD=("$PYTHON")
else
    for candidate in python3 python; do
        if command -v "$candidate" >/dev/null 2>&1 &&
                "$candidate" -c 'import sys' >/dev/null 2>&1; then
            PYTHON_CMD=("$candidate")
            break
        fi
    done
    if [[ ${#PYTHON_CMD[@]} -eq 0 ]] && command -v py >/dev/null 2>&1 &&
            py -3 -c 'import sys' >/dev/null 2>&1; then
        PYTHON_CMD=(py -3)
    fi
    if [[ ${#PYTHON_CMD[@]} -eq 0 ]]; then
        echo "error: Python interpreter not found (tried python3, python, and py -3)" >&2
        exit 1
    fi
fi

"${PYTHON_CMD[@]}" - "$TMPBASE" "$TMPHEAD" << 'PYEOF'
import json, sys

RED = '\033[0;31m'
GREEN = '\033[0;32m'
BOLD = '\033[1m'
NC = '\033[0m'

base_file = sys.argv[1]
head_file = sys.argv[2]

try:
    with open(base_file) as f:
        base = json.loads(f.readline().strip())
    with open(head_file) as f:
        head = json.loads(f.readline().strip())
except (json.JSONDecodeError, FileNotFoundError) as e:
    print(f"error: {e}", file=sys.stderr)
    sys.exit(2)

bm = base.get('metadata', {})
hm = head.get('metadata', {})

preferred_mode_order = [
    'bc-switch', 'bc-threaded',
    'native-arm64', 'zia-arm64', 'basic-arm64', 'native-x86_64',
    'c-O0', 'c-O2', 'c-O3',
    'rust-O', 'lua', 'python3', 'java', 'csharp'
]

zanna_modes = {'bc-switch','bc-threaded','native-arm64','native-x86_64'}

print()
print(f"{BOLD}{'='*72}{NC}")
print(f"{BOLD}                      ZANNA BENCHMARK COMPARISON{NC}")
print(f"{BOLD}{'='*72}{NC}")
print()
print(f"  Base: {bm.get('timestamp','?')[:16]}  commit {bm.get('commit','?')}  ({bm.get('platform','?')})")
print(f"  Head: {hm.get('timestamp','?')[:16]}  commit {hm.get('commit','?')}  ({hm.get('platform','?')})")
print()
print(f"  {'Program':<18} {'Mode':<16} {'Base':>10} {'Head':>10} {'Delta':>10}")
print(f"  {'-'*68}")

base_idx = {b['program']: b for b in base.get('benchmarks', [])}
head_programs = {b.get('program') for b in head.get('benchmarks', [])}
missing_in_head = sorted(set(base_idx) - head_programs)
missing_in_base = sorted(head_programs - set(base_idx))

improved = 0
regressed = 0
stable = 0
zanna_regressed = False

def fmt_time(t):
    if t >= 10000:
        return f"{t/1000:.1f}s"
    if t >= 1000:
        return f"{t/1000:.2f}s"
    return f"{t:.1f}ms"

for bh in head.get('benchmarks', []):
    prog = bh['program']
    bb = base_idx.get(prog)
    if not bb:
        continue

    common_modes = set(bb.get('modes', {})) & set(bh.get('modes', {}))
    ordered_modes = [m for m in preferred_mode_order if m in common_modes]
    ordered_modes.extend(sorted(common_modes - set(ordered_modes)))

    for mode in ordered_modes:
        mh = bh['modes'].get(mode, {})
        mb = bb['modes'].get(mode, {})

        if not mh.get('success') or not mb.get('success'):
            continue

        th = mh['time_ms']
        tb = mb['time_ms']

        if tb == 0:
            continue

        pct = ((th - tb) / tb) * 100

        if pct < -2:
            color = GREEN
            improved += 1
        elif pct > 5:
            color = RED
            regressed += 1
            if mode in zanna_modes:
                zanna_regressed = True
        else:
            color = ''
            stable += 1
        nc_c = NC if color else ''

        flag = ''
        if pct < -10:
            flag = '  IMPROVED'
        elif pct > 5:
            flag = '  REGRESSED'

        print(f"  {prog:<18} {mode:<16} {fmt_time(tb):>10} {fmt_time(th):>10} {color}{pct:>+8.1f}%{nc_c}{flag}")

print()
if missing_in_base:
    print("  Missing baseline entries: " + ", ".join(missing_in_base))
if missing_in_head:
    print("  Missing head entries: " + ", ".join(missing_in_head))
if missing_in_base or missing_in_head:
    print()
print(f"  {BOLD}SUMMARY{NC}")
print(f"  {GREEN}Improved:{NC} {improved:>3} (>2% faster)     {RED}Regressed:{NC} {regressed:>3} (>5% slower)")
print(f"  Stable:   {stable:>3} (within noise)")
print()
print(f"{BOLD}{'='*72}{NC}")
print()

if zanna_regressed:
    sys.exit(1)
PYEOF
