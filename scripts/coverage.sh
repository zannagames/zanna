#!/usr/bin/env bash
# Script: coverage.sh
# Purpose: Build Zanna with Clang source-based coverage and emit local reports.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")"/.. && pwd)"
BUILD_DIR="${ZANNA_COVERAGE_BUILD_DIR:-${ROOT_DIR}/build-coverage}"
REPORT_DIR="${ZANNA_COVERAGE_REPORT_DIR:-${ROOT_DIR}/coverage}"
JOBS="${ZANNA_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
SELF_TEST=false

usage() {
    cat <<'EOF'
Usage: scripts/coverage.sh [options]

Options:
  --build-dir DIR      Coverage build directory (default: build-coverage)
  --report-dir DIR     Report output directory (default: coverage)
  --self-test          Verify required tools and CMake option wiring only
  -h, --help           Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --report-dir) REPORT_DIR="$2"; shift 2 ;;
        --self-test) SELF_TEST=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

# The self-test verifies this script's own tool and CMake-option wiring, so on a
# host without the LLVM coverage toolchain it reports a skip rather than a
# failure. A real coverage run still errors out: there the missing tool means
# the requested reports cannot be produced.
need_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        if $SELF_TEST; then
            echo "SKIP: required tool not found: $1"
            exit 0
        fi
        echo "error: required tool not found: $1" >&2
        exit 1
    fi
}

missing_tool() {
    if $SELF_TEST; then
        echo "SKIP: required tool not found: $1"
        exit 0
    fi
    echo "error: required tool not found: $1" >&2
    exit 1
}

need_tool clang
need_tool clang++
need_tool cmake
need_tool ctest

resolve_llvm_tool() {
    local tool="$1"
    if command -v "$tool" >/dev/null 2>&1; then
        command -v "$tool"
        return 0
    fi

    local clang_major=""
    if command -v clang >/dev/null 2>&1; then
        clang_major="$(clang --version | sed -n -E 's/.*version ([0-9]+).*/\1/p' | head -n 1)"
        if [[ -n "$clang_major" ]] && command -v "${tool}-${clang_major}" >/dev/null 2>&1; then
            command -v "${tool}-${clang_major}"
            return 0
        fi
    fi

    local dir candidate
    IFS=: read -r -a _zanna_path_dirs <<< "${PATH:-}"
    for dir in "${_zanna_path_dirs[@]}"; do
        [[ -n "$dir" ]] || dir="."
        for candidate in "${dir}/${tool}-"*; do
            if [[ -x "$candidate" && ! -d "$candidate" ]]; then
                printf '%s\n' "$candidate"
                return 0
            fi
        done
    done

    if command -v xcrun >/dev/null 2>&1; then
        xcrun -f "$tool" 2>/dev/null && return 0
    fi
    return 1
}

LLVM_PROFDATA="$(resolve_llvm_tool llvm-profdata || true)"
LLVM_COV="$(resolve_llvm_tool llvm-cov || true)"
if [[ -z "$LLVM_PROFDATA" ]]; then
    missing_tool llvm-profdata
fi
if [[ -z "$LLVM_COV" ]]; then
    missing_tool llvm-cov
fi

if $SELF_TEST; then
    if ! grep -q 'ZANNA_ENABLE_COVERAGE' "${ROOT_DIR}/CMakeLists.txt"; then
        echo "coverage self-test: missing ZANNA_ENABLE_COVERAGE CMake option" >&2
        exit 1
    fi
    echo "coverage self-test: ok"
    exit 0
fi

rm -rf "$REPORT_DIR"
mkdir -p "$REPORT_DIR/raw" "$REPORT_DIR/html"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DZANNA_ENABLE_COVERAGE=ON \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++

cmake --build "$BUILD_DIR" -j"$JOBS"

export LLVM_PROFILE_FILE="${REPORT_DIR}/raw/%p-%m.profraw"
ctest --test-dir "$BUILD_DIR" --output-on-failure \
    -E "requires_display|perf_|stress_scalability"

"$LLVM_PROFDATA" merge -sparse "${REPORT_DIR}"/raw/*.profraw -o "${REPORT_DIR}/coverage.profdata"

objects=()
while IFS= read -r object; do
    objects+=("$object")
done < <(
    find "$BUILD_DIR" -type f \( -perm -111 -o -name '*.dylib' -o -name '*.so' \) \
        ! -path '*/CMakeFiles/*' \
        ! -path '*/Testing/*' \
        | sort
)

if [[ ${#objects[@]} -eq 0 ]]; then
    echo "error: no coverage objects found in $BUILD_DIR" >&2
    exit 1
fi

primary="${objects[0]}"
object_args=()
for ((i = 1; i < ${#objects[@]}; i++)); do
    object_args+=("-object=${objects[$i]}")
done

"$LLVM_COV" report "$primary" "${object_args[@]}" \
    -instr-profile="${REPORT_DIR}/coverage.profdata" \
    "${ROOT_DIR}/src" \
    > "${REPORT_DIR}/summary.txt"

"$LLVM_COV" show "$primary" "${object_args[@]}" \
    -instr-profile="${REPORT_DIR}/coverage.profdata" \
    -format=html \
    -output-dir="${REPORT_DIR}/html" \
    "${ROOT_DIR}/src"

awk -v root="${ROOT_DIR}/" '
    /^\/.*src\// {
        file=$1
        sub(root, "", file)
        split(file, parts, "/")
        if (parts[1] == "src" && parts[2] != "") {
            dir=parts[1] "/" parts[2]
            if (parts[3] != "") dir=dir "/" parts[3]
            pct=$10
            gsub(/%/, "", pct)
            if (pct ~ /^[0-9.]+$/) {
                sum[dir] += pct
                count[dir] += 1
            }
        }
    }
    END {
        print "Subsystem line-coverage rollup (unweighted average by file):"
        for (dir in sum) {
            printf "%7.2f%%  %s\n", sum[dir] / count[dir], dir
        }
    }
' "${REPORT_DIR}/summary.txt" | sort -n > "${REPORT_DIR}/subsystems.txt"

echo "Coverage summary: ${REPORT_DIR}/summary.txt"
echo "Subsystem rollup: ${REPORT_DIR}/subsystems.txt"
echo "HTML report:      ${REPORT_DIR}/html/index.html"
