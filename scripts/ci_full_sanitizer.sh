#!/usr/bin/env bash
# Script: ci_full_sanitizer.sh
# Purpose: Run Zanna's broad sanitizer-compatible test inventory under ASan,
#          UBSan, and TSan.
#          Unlike ci_sanitizer_tests.sh (namespace tests only), this script
#          covers every ordinary compatible test for comprehensive
#          memory/undefined-behavior/race detection. Slow/performance and
#          artifact-inspection lanes remain separate because instrumentation
#          changes their timing or output format.
#
# Usage:
#   ./scripts/ci_full_sanitizer.sh          # Run ASan + UBSan
#   ./scripts/ci_full_sanitizer.sh --tsan   # Also run TSan (thread tests only)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR_BASE="${BUILD_DIR:-${REPO_ROOT}/build}"
JOBS="${ZANNA_JOBS:-4}"
CTEST_JOBS="${ZANNA_CTEST_JOBS:-${JOBS}}"
SANITIZER_TEST_TIMEOUT="${ZANNA_SANITIZER_TIMEOUT:-600}"
RUN_TSAN=false
SELF_TEST=false

# Native-output smokes and the runtime-import object reader consume the host
# archive format directly, which sanitizer instrumentation intentionally changes.
# Installer smokes require Studio, while sanitizer trees deliberately omit it.
SANITIZER_EXCLUDE_TESTS="test_diff_vm_native_property|perf_|stress_scalability|native_smoke_|test_linker_runtime_import_audit|installer_installed_config_smoke|macos_toolchain_installer_smoke"

for arg in "$@"; do
    case "$arg" in
        --tsan) RUN_TSAN=true ;;
        --self-test) SELF_TEST=true ;;
    esac
done

if [[ ! "${JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "Error: ZANNA_JOBS must be a positive integer." >&2
    exit 1
fi
if [[ ! "${CTEST_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "Error: ZANNA_CTEST_JOBS must be a positive integer." >&2
    exit 1
fi
if [[ ! "${SANITIZER_TEST_TIMEOUT}" =~ ^[1-9][0-9]*$ ]]; then
    echo "Error: ZANNA_SANITIZER_TIMEOUT must be a positive integer." >&2
    exit 1
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

# Only run on Clang. The self-test verifies this script's own wiring, so on a
# host whose toolchain is GCC there is nothing for it to check and it reports a
# skip; an actual sanitizer run still fails loudly, because there the missing
# compiler means the requested work cannot be done at all.
if ! command -v clang++ > /dev/null 2>&1; then
    if $SELF_TEST; then
        echo "SKIP: clang++ not found; sanitizers require Clang"
        exit 0
    fi
    echo -e "${RED}Error: clang++ not found. Sanitizers require Clang.${NC}"
    exit 1
fi

if $SELF_TEST; then
    tmpdir="$(mktemp -d)"
    trap "rm -rf '$tmpdir'" EXIT
    cat > "$tmpdir/sanitizer_probe.c" <<'EOF'
#include <stdint.h>
int main(void) {
    volatile int x = 40;
    volatile int y = 2;
    return (x + y == 42) ? 0 : 1;
}
EOF
    clang "$tmpdir/sanitizer_probe.c" -fsanitize=address -o "$tmpdir/asan_probe"
    "$tmpdir/asan_probe"
    clang "$tmpdir/sanitizer_probe.c" -fsanitize=undefined -o "$tmpdir/ubsan_probe"
    "$tmpdir/ubsan_probe"
    if $RUN_TSAN; then
        clang "$tmpdir/sanitizer_probe.c" -fsanitize=thread -o "$tmpdir/tsan_probe"
        "$tmpdir/tsan_probe"
    fi
    echo "sanitizer self-test: ok"
    exit 0
fi

FAILED=0

# ============================================================================
# ASan — Address Sanitizer (heap/stack buffer overflow, use-after-free)
# ============================================================================
echo -e "${BLUE}[1/3] Building with AddressSanitizer...${NC}"
ASAN_DIR="${BUILD_DIR_BASE}_asan_full"

cmake -S "${REPO_ROOT}" -B "${ASAN_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DIL_SANITIZE_ADDRESS=ON \
    -DZANNA_SANITIZER_TEST_TIMEOUT="${SANITIZER_TEST_TIMEOUT}" \
    -DZANNA_INSTALL_ZANNASTUDIO=OFF \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_COMPILER=clang \
    > /dev/null 2>&1

cmake --build "${ASAN_DIR}" -j"${JOBS}" > /dev/null 2>&1

echo "Running all tests under ASan..."
if ctest --test-dir "${ASAN_DIR}" -j"${CTEST_JOBS}" --output-on-failure \
    --timeout "${SANITIZER_TEST_TIMEOUT}" -LE "performance|perf|slow" \
    -E "${SANITIZER_EXCLUDE_TESTS}" 2>&1 | tail -5; then
    echo -e "${GREEN}ASan: PASS${NC}"
else
    echo -e "${RED}ASan: FAIL${NC}"
    FAILED=$((FAILED + 1))
fi
echo ""

# ============================================================================
# UBSan — Undefined Behavior Sanitizer
# ============================================================================
echo -e "${BLUE}[2/3] Building with UndefinedBehaviorSanitizer...${NC}"
UBSAN_DIR="${BUILD_DIR_BASE}_ubsan_full"

cmake -S "${REPO_ROOT}" -B "${UBSAN_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DIL_SANITIZE_UNDEFINED=ON \
    -DZANNA_SANITIZER_TEST_TIMEOUT="${SANITIZER_TEST_TIMEOUT}" \
    -DZANNA_INSTALL_ZANNASTUDIO=OFF \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_COMPILER=clang \
    > /dev/null 2>&1

cmake --build "${UBSAN_DIR}" -j"${JOBS}" > /dev/null 2>&1

echo "Running all tests under UBSan..."
if ctest --test-dir "${UBSAN_DIR}" -j"${CTEST_JOBS}" --output-on-failure \
    --timeout "${SANITIZER_TEST_TIMEOUT}" -LE "performance|perf|slow" \
    -E "${SANITIZER_EXCLUDE_TESTS}" 2>&1 | tail -5; then
    echo -e "${GREEN}UBSan: PASS${NC}"
else
    echo -e "${RED}UBSan: FAIL${NC}"
    FAILED=$((FAILED + 1))
fi
echo ""

# ============================================================================
# TSan — Thread Sanitizer (optional, thread-related tests only)
# ============================================================================
if $RUN_TSAN; then
    echo -e "${BLUE}[3/3] Building with ThreadSanitizer...${NC}"
    TSAN_DIR="${BUILD_DIR_BASE}_tsan_full"

    cmake -S "${REPO_ROOT}" -B "${TSAN_DIR}" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DIL_SANITIZE_THREAD=ON \
        -DZANNA_SANITIZER_TEST_TIMEOUT="${SANITIZER_TEST_TIMEOUT}" \
        -DZANNA_INSTALL_ZANNASTUDIO=OFF \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_COMPILER=clang \
        > /dev/null 2>&1

    cmake --build "${TSAN_DIR}" -j"${JOBS}" > /dev/null 2>&1

    echo "Running VM and runtime tests under TSan..."
    if ctest --test-dir "${TSAN_DIR}" -j"${CTEST_JOBS}" --output-on-failure \
        --timeout "${SANITIZER_TEST_TIMEOUT}" -L "vm|runtime" \
        -LE "performance|perf|slow" \
        -E "${SANITIZER_EXCLUDE_TESTS}" 2>&1 | tail -5; then
        echo -e "${GREEN}TSan: PASS${NC}"
    else
        echo -e "${RED}TSan: FAIL${NC}"
        FAILED=$((FAILED + 1))
    fi
else
    echo -e "${BLUE}[3/3] TSan skipped (pass --tsan to enable)${NC}"
fi
echo ""

# ============================================================================
# Summary
# ============================================================================
if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All sanitizer suites passed.${NC}"
    exit 0
else
    echo -e "${RED}${FAILED} sanitizer suite(s) failed.${NC}"
    exit 1
fi
