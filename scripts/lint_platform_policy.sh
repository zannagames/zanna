#!/usr/bin/env bash
#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: scripts/lint_platform_policy.sh
# Purpose: Enforce the repository policy for raw host and compiler macros.
# Key invariants:
#   - Only tracked or explicitly selected source-like files are inspected.
#   - Allowlisted and migration-baseline uses retain their existing semantics.
#   - Search processes operate on bounded batches so Windows startup overhead
#     cannot make the strict CTest probe exceed its budget.
# Ownership/Lifetime: Candidate and diagnostic arrays live for one invocation.
# Links: scripts/platform_policy_allowlist.txt,
#        scripts/platform_policy_migration_baseline.txt,
#        src/tests/CMakeLists.txt
#
#===----------------------------------------------------------------------===#

# Lint raw platform-macro usage so new cross-platform policy does not leak into
# random shared code. Default mode is advisory; --strict returns non-zero.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
ALLOWLIST_FILE="$SCRIPT_DIR/platform_policy_allowlist.txt"
BASELINE_FILE="$SCRIPT_DIR/platform_policy_migration_baseline.txt"
STRICT=0
CHANGED_ONLY=0
declare -a PATH_FILTERS=()

usage() {
    cat <<'EOF'
usage: lint_platform_policy.sh [--strict] [--changed-only] [--paths <path>...]

  --strict       Exit non-zero on violations.
  --changed-only Scan files changed from HEAD instead of the whole tracked tree.
  --paths ...    Scan only the provided files/directories.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --strict)
            STRICT=1
            shift
            ;;
        --changed-only)
            CHANGED_ONLY=1
            shift
            ;;
        --paths)
            shift
            while [[ $# -gt 0 && "$1" != --* ]]; do
                PATH_FILTERS+=("$1")
                shift
            done
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ ! -f "$ALLOWLIST_FILE" ]]; then
    echo "error: allowlist file not found: $ALLOWLIST_FILE" >&2
    exit 1
fi

declare -a ALLOWLIST=()
while IFS= read -r line; do
    ALLOWLIST+=("$line")
done < <(grep -v '^[[:space:]]*#' "$ALLOWLIST_FILE" | sed '/^[[:space:]]*$/d')

declare -a BASELINE_PATHS=()
declare -a BASELINE_COUNTS=()
if [[ -f "$BASELINE_FILE" ]]; then
    while read -r path count; do
        [[ -n "$path" && -n "$count" ]] || continue
        BASELINE_PATHS+=("$path")
        BASELINE_COUNTS+=("$count")
    done < <(grep -v '^[[:space:]]*#' "$BASELINE_FILE" | sed '/^[[:space:]]*$/d')
fi

is_allowlisted() {
    local path="$1"
    # macOS still ships Bash 3.2; under `set -u`, expanding an empty array
    # with "${arr[@]}" throws "unbound variable". Guard zero-length arrays
    # explicitly so clean trees and empty allowlists are valid inputs.
    if [[ ${#ALLOWLIST[@]} -eq 0 ]]; then
        return 1
    fi
    for pattern in "${ALLOWLIST[@]}"; do
        case "$path" in
            $pattern)
                return 0
                ;;
        esac
    done
    return 1
}

BASELINE_COUNT_RESULT=0
set_baseline_count_for() {
    local path="$1"
    local i
    BASELINE_COUNT_RESULT=0
    if [[ ${#BASELINE_PATHS[@]} -eq 0 ]]; then
        return
    fi
    for ((i = 0; i < ${#BASELINE_PATHS[@]}; i++)); do
        if [[ "${BASELINE_PATHS[$i]}" == "$path" ]]; then
            BASELINE_COUNT_RESULT="${BASELINE_COUNTS[$i]}"
            return
        fi
    done
}

matches_filter() {
    local path="$1"
    if [[ ${#PATH_FILTERS[@]} -eq 0 ]]; then
        return 0
    fi
    for filter in "${PATH_FILTERS[@]}"; do
        case "$path" in
            "$filter"|"$filter"/*)
                return 0
                ;;
        esac
    done
    return 1
}

collect_candidates() {
    if [[ ${#PATH_FILTERS[@]} -gt 0 ]]; then
        for filter in "${PATH_FILTERS[@]}"; do
            if [[ -d "$ROOT_DIR/$filter" ]]; then
                (cd "$ROOT_DIR" && find "$filter" -type f)
            elif [[ -f "$ROOT_DIR/$filter" ]]; then
                printf '%s\n' "$filter"
            fi
        done
        return
    fi

    if [[ $CHANGED_ONLY -eq 1 ]]; then
        (cd "$ROOT_DIR" && git diff --name-only --diff-filter=ACMRTUXB HEAD)
    else
        (cd "$ROOT_DIR" && git ls-files)
    fi
}

is_source_like() {
    local path="$1"
    case "$path" in
        *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.m|*.mm|*.cmake|*/CMakeLists.txt)
            return 0
            ;;
    esac
    return 1
}

declare -a VIOLATIONS=()
declare -a TOUCHPOINT_VIOLATIONS=()
RAW_PATTERN='_WIN32|_WIN64|__APPLE__|__linux__|_MSC_VER'
search_raw_platform_macros() {
    local -a paths=("$@")
    if command -v rg >/dev/null 2>&1; then
        (cd "$ROOT_DIR" &&
            rg --with-filename -n -w "$RAW_PATTERN" --color never -- "${paths[@]}" ||
            true)
        return
    fi
    (cd "$ROOT_DIR" &&
        grep -nEH "(^|[^[:alnum:]_])(${RAW_PATTERN})([^[:alnum:]_]|$)" \
            -- "${paths[@]}" ||
        true)
}

record_raw_macro_violations_batch() {
    local -a paths=("$@")
    local path
    local baseline_count
    local hit
    local -a hits=()
    local -a path_hits=()

    while IFS= read -r hit; do
        hits+=("$hit")
    done < <(search_raw_platform_macros "${paths[@]}")

    for path in "${paths[@]}"; do
        path_hits=()
        # bash 3.2 + set -u treats an empty array expansion as unbound; the
        # ${arr[@]+...} guard keeps a fully clean batch from aborting the lint.
        for hit in ${hits[@]+"${hits[@]}"}; do
            case "$hit" in
                "$path":*)
                    path_hits+=("$hit")
                    ;;
            esac
        done

        set_baseline_count_for "$path"
        baseline_count="$BASELINE_COUNT_RESULT"
        if [[ ${#path_hits[@]} -le $baseline_count ]]; then
            continue
        fi

        if [[ $baseline_count -gt 0 ]]; then
            VIOLATIONS+=("$path: raw host/compiler macro count ${#path_hits[@]} exceeds migration baseline $baseline_count")
        fi
        for hit in "${path_hits[@]}"; do
            VIOLATIONS+=("$hit")
        done
    done
}

declare -a CANDIDATES=()
while IFS= read -r line; do
    CANDIDATES+=("$line")
done < <(collect_candidates | LC_ALL=C sort -u)

PLATFORM_POLICY_SEARCH_BATCH_SIZE=128
declare -a SEARCH_BATCH=()
if [[ ${#CANDIDATES[@]} -gt 0 ]]; then
    for path in "${CANDIDATES[@]}"; do
        [[ -f "$ROOT_DIR/$path" ]] || continue
        is_source_like "$path" || continue
        matches_filter "$path" || continue

        if ! is_allowlisted "$path"; then
            SEARCH_BATCH+=("$path")
            if [[ ${#SEARCH_BATCH[@]} -ge $PLATFORM_POLICY_SEARCH_BATCH_SIZE ]]; then
                record_raw_macro_violations_batch "${SEARCH_BATCH[@]}"
                SEARCH_BATCH=()
            fi
        fi

        case "$path" in
            src/common/RunProcess.cpp|src/codegen/common/LinkerSupport.cpp|src/codegen/common/RuntimeComponents.hpp|src/codegen/common/linker/NativeLinker.cpp|src/codegen/x86_64/CodegenPipeline.cpp|src/codegen/aarch64/CodegenPipeline.cpp)
                if ! head -n 30 "$ROOT_DIR/$path" | grep -q "Cross-platform touchpoints:"; then
                    TOUCHPOINT_VIOLATIONS+=("$path")
                fi
                ;;
        esac
    done
    if [[ ${#SEARCH_BATCH[@]} -gt 0 ]]; then
        record_raw_macro_violations_batch "${SEARCH_BATCH[@]}"
    fi
fi

if [[ ${#VIOLATIONS[@]} -eq 0 && ${#TOUCHPOINT_VIOLATIONS[@]} -eq 0 ]]; then
    echo "Platform policy lint: clean"
    exit 0
fi

if [[ ${#VIOLATIONS[@]} -gt 0 ]]; then
    echo "Platform policy lint: raw host/compiler macros outside allowlist:" >&2
    printf '  %s\n' "${VIOLATIONS[@]}" >&2
fi

if [[ ${#TOUCHPOINT_VIOLATIONS[@]} -gt 0 ]]; then
    echo "Platform policy lint: missing touchpoint header note:" >&2
    printf '  %s\n' "${TOUCHPOINT_VIOLATIONS[@]}" >&2
fi

if [[ $STRICT -eq 1 ]]; then
    exit 1
fi

echo "Platform policy lint ran in advisory mode; rerun with --strict to fail on these issues." >&2
exit 0
