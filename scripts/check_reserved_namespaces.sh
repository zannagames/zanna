#!/usr/bin/env bash
# Script: check_reserved_namespaces.sh
# Purpose: Ensure user-facing code does not use reserved "Zanna" namespace
# Exit code: 0 if clean, 1 if violations found

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "Checking for reserved 'Zanna' namespace usage in user-facing code..."

# Directories to check for user-facing code
CHECK_DIRS=(
  "${REPO_ROOT}/src/tests/golden/basic"
  "${REPO_ROOT}/src/tests/e2e"
  "${REPO_ROOT}/examples/vbasic"
)

# Allowed exceptions (built-in library examples showing Track B syntax)
ALLOWED_FILES=(
  "src/tests/golden/basic/zanna_root_example.bas"
  "src/tests/golden/basic_errors/reserved_root_user_decl.bas"
  "src/tests/golden/basic_errors/reserved_root_user_using.bas"
)

VIOLATIONS=0

check_file() {
  local file="$1"
  local rel_path="${file#${REPO_ROOT}/}"

  # Check if this file is in the allowed list
  for allowed in "${ALLOWED_FILES[@]}"; do
    if [[ "${rel_path}" == "${allowed}" ]]; then
      return 0
    fi
  done

  # Check for NAMESPACE Zanna patterns
  if grep -E '^\s*NAMESPACE\s+Zanna' "$file" > /dev/null 2>&1; then
    echo -e "${RED}✗ VIOLATION${NC}: $rel_path contains 'NAMESPACE Zanna'"
    grep -n -E '^\s*NAMESPACE\s+Zanna' "$file" | head -3
    VIOLATIONS=$((VIOLATIONS + 1))
  fi

  # Check for USING Zanna patterns (but not USING Zanna.Something)
  # We want to catch "USING Zanna" (root) but allow "USING Zanna.System.Text" in examples
  if grep -E '^\s*USING\s+Zanna\s*($|REM)' "$file" > /dev/null 2>&1; then
    echo -e "${RED}✗ VIOLATION${NC}: $rel_path contains 'USING Zanna' (root)"
    grep -n -E '^\s*USING\s+Zanna\s*($|REM)' "$file" | head -3
    VIOLATIONS=$((VIOLATIONS + 1))
  fi
}

# Check all .bas files in the target directories
for dir in "${CHECK_DIRS[@]}"; do
  if [[ -d "$dir" ]]; then
    while IFS= read -r -d '' file; do
      check_file "$file"
    done < <(find "$dir" -name "*.bas" -print0)
  fi
done

echo ""
if [[ $VIOLATIONS -eq 0 ]]; then
  echo -e "${GREEN}✓ PASS${NC}: No reserved namespace violations found"
  exit 0
else
  echo -e "${RED}✗ FAIL${NC}: Found $VIOLATIONS violation(s) of reserved 'Zanna' namespace"
  echo ""
  echo "The 'Zanna' root namespace is reserved for built-in libraries (Track B)."
  echo "User code should not use 'NAMESPACE Zanna' or 'USING Zanna'."
  echo ""
  echo "If this is intentional (e.g., error test or Track B example),"
  echo "add the file to ALLOWED_FILES in this script."
  exit 1
fi
