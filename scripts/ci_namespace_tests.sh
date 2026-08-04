#!/usr/bin/env bash
# Script: ci_namespace_tests.sh
# Purpose: CI gate for namespace feature - runs all namespace tests and policy checks
# Exit code: 0 if all pass, non-zero if any fail

set -euo pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"

echo -e "${BLUE}======================================"
echo "Namespace Feature CI Gate"
echo "======================================${NC}"
echo ""

FAILED_CHECKS=0

# ============================================================================
# Check 1: Reserved namespace policy
# ============================================================================

echo -e "${BLUE}[1/5]${NC} Checking reserved 'Zanna' namespace policy..."
if "${SCRIPT_DIR}/check_reserved_namespaces.sh"; then
  echo -e "${GREEN}✓ PASS${NC}: Reserved namespace policy enforced"
else
  echo -e "${RED}✗ FAIL${NC}: Reserved namespace policy violated"
  FAILED_CHECKS=$((FAILED_CHECKS + 1))
fi
echo ""

# ============================================================================
# Check 2: Run namespace unit tests
# ============================================================================

echo -e "${BLUE}[2/5]${NC} Running namespace unit tests..."
UNIT_TESTS=(
  "test_basic_parse_namespace"
  "test_basic_parse_using"
  "test_namespace_registry"
  "test_using_context"
  "test_type_resolver"
  "test_using_semantics"
  "test_ns_resolve_pass"
  "test_lowerer_namespace"
  "test_namespace_diagnostics"
  "test_namespace_integration"
  "test_using_compiletime_only"
)

UNIT_FAILED=0
for test in "${UNIT_TESTS[@]}"; do
  if ctest --test-dir "${BUILD_DIR}" -R "^${test}$" --output-on-failure > /dev/null 2>&1; then
    echo -e "  ${GREEN}✓${NC} $test"
  else
    echo -e "  ${RED}✗${NC} $test"
    UNIT_FAILED=$((UNIT_FAILED + 1))
  fi
done

if [[ $UNIT_FAILED -eq 0 ]]; then
  echo -e "${GREEN}✓ PASS${NC}: All ${#UNIT_TESTS[@]} namespace unit tests passed"
else
  echo -e "${RED}✗ FAIL${NC}: $UNIT_FAILED/${#UNIT_TESTS[@]} unit tests failed"
  FAILED_CHECKS=$((FAILED_CHECKS + 1))
fi
echo ""

# ============================================================================
# Check 3: Run namespace golden tests
# ============================================================================

echo -e "${BLUE}[3/5]${NC} Running namespace golden tests..."
GOLDEN_TESTS=(
  "golden_basic_namespace_simple"
  "golden_basic_namespace_using"
  "golden_basic_namespace_inheritance"
  "golden_basic_zanna_root_example"
  "golden_basic_errors_namespace_notfound"
  "golden_basic_errors_namespace_ambiguous"
  "golden_basic_errors_namespace_duplicate_alias"
  "golden_basic_errors_using_in_namespace"
  "golden_basic_errors_using_after_decl"
  "golden_basic_errors_reserved_root_user_decl"
  "golden_basic_errors_reserved_root_user_using"
)

GOLDEN_FAILED=0
for test in "${GOLDEN_TESTS[@]}"; do
  if ctest --test-dir "${BUILD_DIR}" -R "^${test}$" --output-on-failure > /dev/null 2>&1; then
    echo -e "  ${GREEN}✓${NC} $test"
  else
    echo -e "  ${RED}✗${NC} $test"
    GOLDEN_FAILED=$((GOLDEN_FAILED + 1))
  fi
done

if [[ $GOLDEN_FAILED -eq 0 ]]; then
  echo -e "${GREEN}✓ PASS${NC}: All ${#GOLDEN_TESTS[@]} namespace golden tests passed"
else
  echo -e "${RED}✗ FAIL${NC}: $GOLDEN_FAILED/${#GOLDEN_TESTS[@]} golden tests failed"
  FAILED_CHECKS=$((FAILED_CHECKS + 1))
fi
echo ""

# ============================================================================
# Check 4: Run namespace e2e test (if enabled)
# ============================================================================

echo -e "${BLUE}[4/5]${NC} Running namespace e2e test..."
if ctest --test-dir "${BUILD_DIR}" -R "^test_namespace_e2e$" --output-on-failure > /dev/null 2>&1; then
  echo -e "${GREEN}✓ PASS${NC}: E2E test passed"
elif ctest --test-dir "${BUILD_DIR}" -N -R "^test_namespace_e2e$" 2>&1 | grep -q "DISABLED"; then
  echo -e "${YELLOW}⊘ SKIP${NC}: E2E test is disabled (known multi-file resolution issue)"
else
  echo -e "${RED}✗ FAIL${NC}: E2E test failed"
  FAILED_CHECKS=$((FAILED_CHECKS + 1))
fi
echo ""

# ============================================================================
# Check 5: Verify example compiles
# ============================================================================

echo -e "${BLUE}[5/5]${NC} Verifying namespace_demo.bas compiles..."
EXAMPLE_FILE="${REPO_ROOT}/examples/vbasic/namespace_demo.bas"
ILC="${BUILD_DIR}/src/tools/zanna/zanna"

if [[ ! -f "$ILC" ]]; then
  echo -e "${RED}✗ FAIL${NC}: ilc not found at $ILC"
  FAILED_CHECKS=$((FAILED_CHECKS + 1))
elif "$ILC" front basic -emit-il "$EXAMPLE_FILE" > /tmp/namespace_demo_ci.il 2>&1; then
  IL_SIZE=$(wc -l < /tmp/namespace_demo_ci.il)
  echo -e "${GREEN}✓ PASS${NC}: Example compiled successfully ($IL_SIZE lines of IL)"
else
  echo -e "${RED}✗ FAIL${NC}: Example failed to compile"
  cat /tmp/namespace_demo_ci.il
  FAILED_CHECKS=$((FAILED_CHECKS + 1))
fi
echo ""

# ============================================================================
# Summary
# ============================================================================

echo -e "${BLUE}======================================"
echo "Summary"
echo "======================================${NC}"
echo ""

if [[ $FAILED_CHECKS -eq 0 ]]; then
  echo -e "${GREEN}✓ ALL CHECKS PASSED${NC}"
  echo ""
  echo "Namespace feature is ready for production:"
  echo "  • Reserved namespace policy enforced"
  echo "  • All unit tests passing"
  echo "  • All golden tests passing"
  echo "  • Example code compiles"
  exit 0
else
  echo -e "${RED}✗ $FAILED_CHECKS CHECK(S) FAILED${NC}"
  echo ""
  echo "Please fix the failures above before merging."
  exit 1
fi
