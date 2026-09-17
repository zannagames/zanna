#!/usr/bin/env bash
# build_zanna_unix.sh — canonical POSIX Zanna build + test script; install is
# delegated to install_zanna_unix.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
PLATFORM="$(uname -s 2>/dev/null || true)"

detect_build_jobs() {
    case "$PLATFORM" in
        Darwin)
            sysctl -n hw.ncpu 2>/dev/null || echo 8
            ;;
        Linux)
            if command -v nproc >/dev/null 2>&1; then
                nproc
            else
                getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8
            fi
            ;;
    esac
}

detect_macos_performance_cores() {
    local perf_cores
    perf_cores="$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || true)"
    if [[ -n "$perf_cores" && "$perf_cores" =~ ^[0-9]+$ && "$perf_cores" -gt 0 ]]; then
        echo "$perf_cores"
        return
    fi
    sysctl -n hw.physicalcpu 2>/dev/null || detect_build_jobs
}

case "$PLATFORM" in
    Darwin)
        DEFAULT_BUILD_TYPE="Debug"
        DEFAULT_CTEST_JOBS="$(detect_macos_performance_cores)"
        DEFAULT_FAST_DEBUG="1"
        ;;
    Linux)
        DEFAULT_BUILD_TYPE="Debug"
        DEFAULT_CTEST_JOBS="$(detect_build_jobs)"
        DEFAULT_FAST_DEBUG="1"
        ;;
    *)
        echo "Error: build_zanna_unix.sh supports macOS and Linux only"
        exit 1
        ;;
esac

JOBS="${ZANNA_JOBS:-$(detect_build_jobs)}"
CTEST_JOBS="${ZANNA_CTEST_JOBS:-$DEFAULT_CTEST_JOBS}"
BUILD_DIR="${ZANNA_BUILD_DIR:-$ROOT_DIR/build}"
BUILD_TYPE="${ZANNA_BUILD_TYPE:-$DEFAULT_BUILD_TYPE}"
FAST_DEBUG="${ZANNA_FAST_DEBUG:-$DEFAULT_FAST_DEBUG}"
SKIP_INSTALL="${ZANNA_SKIP_INSTALL:-0}"
SKIP_AUDIT="${ZANNA_SKIP_AUDIT:-0}"
SKIP_LINT="${ZANNA_SKIP_LINT:-0}"
LINT_CHANGED_ONLY="${ZANNA_LINT_CHANGED_ONLY:-1}"
SKIP_SMOKE="${ZANNA_SKIP_SMOKE:-0}"
SKIP_TESTS="${ZANNA_SKIP_TESTS:-0}"
SKIP_CLEAN="${ZANNA_SKIP_CLEAN:-0}"
SKIP_STUDIO="${ZANNA_SKIP_STUDIO:-0}"
TEST_LABEL="${ZANNA_TEST_LABEL:-}"
NO_CCACHE="${ZANNA_NO_CCACHE:-0}"
RUN_SLOW_TESTS="${ZANNA_RUN_SLOW_TESTS:-0}"
CTEST_TIMEOUT="${ZANNA_CTEST_TIMEOUT:-}"

# A clean/build/test run mutates and consumes the same build tree throughout its
# lifetime. Serializing by build directory prevents a second clean from deleting
# executables or generated files while the first invocation is compiling or
# running tests.
if ! mkdir -p "$BUILD_DIR"; then
    echo "error: cannot create build directory: $BUILD_DIR" >&2
    exit 1
fi
BUILD_DIR="$(cd "$BUILD_DIR" && pwd -P)"
BUILD_LOCK_PATH="$BUILD_DIR/.zanna-build.lock"

# shellcheck source=SCRIPTDIR/zanna_build_lock.sh
source "$SCRIPT_DIR/zanna_build_lock.sh"
acquire_build_lock

CONFIGURE_ARGS=(
    -S "$ROOT_DIR"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DZANNA_FAST_DEBUG="$FAST_DEBUG"
)

if [[ -n "${ZANNA_CMAKE_GENERATOR:-}" ]]; then
    CONFIGURE_ARGS+=(-G "${ZANNA_CMAKE_GENERATOR}")
fi

if command -v clang >/dev/null 2>&1; then
    CONFIGURE_ARGS+=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
elif command -v gcc >/dev/null 2>&1; then
    CONFIGURE_ARGS+=(-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++)
fi

# Compiler cache: speeds up rebuilds after clean or branch switches.
# Auto-detected; set ZANNA_NO_CCACHE=1 to disable.
if [[ "$NO_CCACHE" != "1" ]] && command -v ccache >/dev/null 2>&1; then
    echo "[build_zanna] ccache detected; enabling compiler launcher (set ZANNA_NO_CCACHE=1 to disable)"
    CONFIGURE_ARGS+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

# The Zanna Studio native compile is the longest single build step. Pass the
# toggle explicitly every configure so a skipped run cannot leave a stale OFF
# cached into later full runs.
if [[ "$SKIP_STUDIO" == "1" ]]; then
    echo "[build_zanna] Skipping Zanna Studio build (ZANNA_SKIP_STUDIO=1)"
    CONFIGURE_ARGS+=(-DZANNA_INSTALL_ZANNASTUDIO=OFF)
else
    CONFIGURE_ARGS+=(-DZANNA_INSTALL_ZANNASTUDIO=ON)
fi

if [[ -n "${ZANNA_EXTRA_CMAKE_ARGS:-}" ]]; then
    # shellcheck disable=SC2206
    EXTRA_ARGS=( ${ZANNA_EXTRA_CMAKE_ARGS} )
    CONFIGURE_ARGS+=("${EXTRA_ARGS[@]}")
fi

if [[ "$SKIP_CLEAN" != "1" ]]; then
    cmake --build "$BUILD_DIR" --target clean-all 2>/dev/null || true
else
    echo "[build_zanna] Skipping clean (ZANNA_SKIP_CLEAN=1); incremental rebuild"
fi
cmake "${CONFIGURE_ARGS[@]}"
echo "[build_zanna] Build type: $BUILD_TYPE"
echo "[build_zanna] Fast Debug: $FAST_DEBUG"
echo "[build_zanna] Build jobs: $JOBS"
cmake --build "$BUILD_DIR" -j"$JOBS"

if command -v sync >/dev/null 2>&1; then
    sync
    sleep 1
else
    cmake -E sleep 1
fi

if [[ "$SKIP_TESTS" == "1" ]]; then
    echo "[build_zanna] Skipping tests (ZANNA_SKIP_TESTS=1)"
else
    if [[ -n "$TEST_LABEL" ]]; then
        echo "[build_zanna] Running tests labeled '$TEST_LABEL' (ZANNA_TEST_LABEL)..."
    else
        echo "[build_zanna] Running full test suite..."
    fi
    echo "[build_zanna] CTest jobs: $CTEST_JOBS"
    CTEST_ARGS=(--test-dir "$BUILD_DIR" --output-on-failure -j"$CTEST_JOBS")
    if [[ -n "$TEST_LABEL" ]]; then
        CTEST_ARGS+=(-L "$TEST_LABEL")
    fi
    if [[ -n "$CTEST_TIMEOUT" ]]; then
        CTEST_ARGS+=(--timeout "$CTEST_TIMEOUT")
    fi
    if [[ "$RUN_SLOW_TESTS" != "1" ]]; then
        echo "[build_zanna] Skipping tests labeled slow (set ZANNA_RUN_SLOW_TESTS=1 to include them)"
        CTEST_ARGS+=(-LE slow)
    fi
    ctest "${CTEST_ARGS[@]}"
    echo "[build_zanna] Test run complete"
fi

if [[ "$SKIP_LINT" != "1" && -x "$SCRIPT_DIR/lint_platform_policy.sh" ]]; then
    echo "[build_zanna] Running platform policy lint..."
    if [[ "$LINT_CHANGED_ONLY" == "1" ]]; then
        "$SCRIPT_DIR/lint_platform_policy.sh" --changed-only
    else
        "$SCRIPT_DIR/lint_platform_policy.sh"
    fi
fi

if [[ "$SKIP_AUDIT" != "1" && -x "$SCRIPT_DIR/audit_runtime_surface.sh" ]]; then
    echo "[build_zanna] Running runtime surface audit..."
    "$SCRIPT_DIR/audit_runtime_surface.sh" --build-dir="$BUILD_DIR"
fi

if [[ "$SKIP_SMOKE" != "1" && -x "$SCRIPT_DIR/run_cross_platform_smoke.sh" ]]; then
    echo "[build_zanna] Running cross-platform smoke tests..."
    "$SCRIPT_DIR/run_cross_platform_smoke.sh" --build-dir "$BUILD_DIR"
fi

if [[ "$SKIP_INSTALL" == "1" ]]; then
    echo "[build_zanna] Skipping install (ZANNA_SKIP_INSTALL=1); run install_zanna_<platform>.sh later to install"
    exit 0
fi

# The install lives in its own script so a failed install (for example a sudo
# prompt that timed out) can be finished with install_zanna_<platform>.sh
# without rebuilding. exec keeps this PID, so the build lock is handed over
# rather than released.
exec "$SCRIPT_DIR/install_zanna_unix.sh"
