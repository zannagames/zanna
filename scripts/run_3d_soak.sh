#!/usr/bin/env bash
#===----------------------------------------------------------------------===//
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===//
#
# File: scripts/run_3d_soak.sh
# Purpose: Long-run 3D soak — renders the churn scene for a configurable
#          duration while sampling process RSS, then asserts memory stays flat
#          and the scene's degradation counters report clean.
# Key invariants:
#   - The scene itself asserts Game3D degradation counters do not grow after
#     warmup; this wrapper adds external RSS sampling the runtime cannot see.
#   - Memory is compared between the 25%-mark sample and the peak; growth
#     beyond the tolerance fails the run.
#   - POSIX-only (uses ps); run by hand, not registered in ctest.
# Ownership/Lifetime:
#   - Writes only /tmp/zanna_soak_*.log scratch files.
# Links: src/tests/graphics_conformance/soak_scene.zia
#
#===----------------------------------------------------------------------===//

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/run_3d_soak.sh [options]
  --zia PATH       zia interpreter (default: build/src/tools/zia/zia)
  --minutes N      simulated minutes to run (default 5; 60 fps fixed step)
  --backend NAME   3D backend (default software; metal/opengl need a display)
  --grow-pct N     allowed RSS growth percent after the 25% mark (default 15)
EOF
}

ZIA="build/src/tools/zia/zia"
MINUTES=5
BACKEND="software"
GROW_PCT=15

while [[ $# -gt 0 ]]; do
    case "$1" in
        --zia) ZIA="$2"; shift 2 ;;
        --minutes) MINUTES="$2"; shift 2 ;;
        --backend) BACKEND="$2"; shift 2 ;;
        --grow-pct) GROW_PCT="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "ERROR: unknown option $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ ! -x "${ZIA}" ]]; then
    echo "ERROR: zia interpreter not found at ${ZIA} (build first or pass --zia)." >&2
    exit 2
fi

FRAMES=$((MINUTES * 60 * 60))
RSS_LOG="/tmp/zanna_soak_rss.log"
OUT_LOG="/tmp/zanna_soak_out.log"
: > "${RSS_LOG}"

echo "== soak: backend=${BACKEND} frames=${FRAMES} (~${MINUTES} simulated min)"

ZANNA_3D_BACKEND="${BACKEND}" \
ZANNA_SOAK_FRAMES="${FRAMES}" \
    "${ZIA}" src/tests/graphics_conformance/soak_scene.zia > "${OUT_LOG}" 2>&1 &
soak_pid=$!

# Sample resident set size once a second until the scene exits.
while kill -0 "${soak_pid}" 2>/dev/null; do
    # The scene may exit after kill -0 succeeds but before ps samples it.
    # Keep that expected race from tripping errexit under pipefail.
    rss="$(ps -o rss= -p "${soak_pid}" 2>/dev/null | tr -d ' ' || true)"
    [[ -n "${rss}" ]] && echo "${rss}" >> "${RSS_LOG}"
    sleep 1
done
soak_status=0
wait "${soak_pid}" || soak_status=$?

grep -E "SOAK|RESULT" "${OUT_LOG}" || true

if [[ "${soak_status}" -ne 0 ]] || ! grep -q "RESULT: ok" "${OUT_LOG}"; then
    echo "FAIL: soak scene reported failure (see ${OUT_LOG})" >&2
    echo "RESULT: fail"
    exit 1
fi

samples="$(wc -l < "${RSS_LOG}" | tr -d ' ')"
if [[ "${samples}" -lt 8 ]]; then
    echo "NOTE: run too short for a meaningful RSS trend (${samples} samples)."
    echo "RESULT: ok"
    exit 0
fi

# Compare the settled sample (25% mark) against the peak of the rest.
settle_line=$((samples / 4))
[[ "${settle_line}" -lt 1 ]] && settle_line=1
settled="$(sed -n "${settle_line}p" "${RSS_LOG}")"
peak="$(sort -n "${RSS_LOG}" | tail -1)"
allowed=$((settled + settled * GROW_PCT / 100))

echo "== soak RSS: settled=${settled}KB peak=${peak}KB allowed=${allowed}KB samples=${samples}"
if [[ "${peak}" -gt "${allowed}" ]]; then
    echo "FAIL: resident memory grew beyond ${GROW_PCT}% after settling" >&2
    echo "RESULT: fail"
    exit 1
fi
echo "RESULT: ok"
