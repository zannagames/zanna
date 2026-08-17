#!/usr/bin/env bash
#===----------------------------------------------------------------------===//
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
# File: zannastudio/scripts/test_check_architecture.sh
# Purpose: Exercise architecture-guard success, failure, and ratchet behavior.
# Key invariants:
#   - Every negative fixture must be rejected before the test continues.
#   - All fixture writes and deletions stay below one private temporary root.
# Ownership/Lifetime:
#   - Owns and removes one temporary Studio-shaped source tree.
# Links: zannastudio/scripts/check_architecture.sh
#
#===----------------------------------------------------------------------===//

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
guard="${script_dir}/check_architecture.sh"
fixture_root="$(mktemp -d "${TMPDIR:-/tmp}/zanna-studio-architecture-test.XXXXXX")"
trap 'rm -rf "${fixture_root}"' EXIT
baseline="${fixture_root}/baseline.tsv"

mkdir -p "${fixture_root}/src/services" "${fixture_root}/src/zia" \
    "${fixture_root}/src/core" "${fixture_root}/src/ui" "${fixture_root}/src/app"

write_source() {
    local path="$1"
    local module_name="$2"
    shift 2
    {
        echo '// File: fixture.zia'
        echo '// Purpose: Exercise the architecture guard.'
        echo '// Key invariants: The fixture is deterministic.'
        echo "module ${module_name};"
        while [ "$#" -gt 0 ]; do
            echo "$1"
            shift
        done
    } > "${path}"
}

expect_failure() {
    local description="$1"
    shift
    if "$@" > /dev/null 2>&1; then
        echo "FAIL: architecture guard accepted ${description}." >&2
        exit 1
    fi
}

printf '# empty architecture debt baseline\n' > "${baseline}"
write_source "${fixture_root}/src/services/text.zia" text
write_source "${fixture_root}/src/zia/scan.zia" scan 'bind "../services/text";'
"${guard}" --root "${fixture_root}" --baseline "${baseline}" --quiet

write_source "${fixture_root}/src/core/missing_header.zia" missing_header
sed -i.bak '1,3d' "${fixture_root}/src/core/missing_header.zia"
rm -f "${fixture_root}/src/core/missing_header.zia.bak"
expect_failure "a source without a teaching header" \
    "${guard}" --root "${fixture_root}" --baseline "${baseline}" --quiet
rm -f "${fixture_root}/src/core/missing_header.zia"

write_source "${fixture_root}/src/app/missing_bind.zia" missing_bind 'bind "../core/not_here";'
expect_failure "an unresolved local bind" \
    "${guard}" --root "${fixture_root}" --baseline "${baseline}" --quiet
rm -f "${fixture_root}/src/app/missing_bind.zia"

write_source "${fixture_root}/src/ui/view.zia" view
write_source "${fixture_root}/src/services/upward.zia" upward 'bind "../ui/view";'
expect_failure "a new upward layer dependency" \
    "${guard}" --root "${fixture_root}" --baseline "${baseline}" --quiet
"${guard}" --root "${fixture_root}" --print-baseline --quiet > "${baseline}"
"${guard}" --root "${fixture_root}" --baseline "${baseline}" --quiet

write_source "${fixture_root}/src/services/upward.zia" upward
expect_failure "a stale dependency baseline after an improvement" \
    "${guard}" --root "${fixture_root}" --baseline "${baseline}" --quiet
"${guard}" --root "${fixture_root}" --print-baseline --quiet > "${baseline}"

write_source "${fixture_root}/src/app/large.zia" large
for index in $(seq 1 601); do
    echo "// filler ${index}" >> "${fixture_root}/src/app/large.zia"
done
expect_failure "a new oversized source" \
    "${guard}" --root "${fixture_root}" --baseline "${baseline}" --quiet
"${guard}" --root "${fixture_root}" --print-baseline --quiet > "${baseline}"
"${guard}" --root "${fixture_root}" --baseline "${baseline}" --quiet

echo '// growth' >> "${fixture_root}/src/app/large.zia"
expect_failure "growth beyond a recorded size ratchet" \
    "${guard}" --root "${fixture_root}" --baseline "${baseline}" --quiet

echo "PASS: Zanna Studio architecture guard ratchets architecture debt."
