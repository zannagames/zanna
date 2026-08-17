#!/usr/bin/env bash
#===----------------------------------------------------------------------===//
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
# File: zannastudio/scripts/check_architecture.sh
# Purpose: Ratchet Studio file-size, source-header, bind, and layer boundaries.
# Key invariants:
#   - Existing size/layer debt is explicit and cannot grow silently.
#   - Every local bind resolves to a source file inside the Studio source root.
#   - New source files carry either the full source header or a legacy teaching block.
# Ownership/Lifetime:
#   - Reads Studio sources and one checked-in baseline; writes only temporary files.
#   - --print-baseline emits a deterministic replacement baseline to stdout.
# Links: zannastudio/scripts/architecture_baseline.tsv,
#        zannastudio/docs/architecture.md,
#        docs/adr/0266-ratcheted-studio-architecture-guard.md
#
#===----------------------------------------------------------------------===//

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
studio_root="$(cd "${script_dir}/.." && pwd -P)"
baseline_file=""
print_baseline=0
quiet=0

usage() {
    echo "Usage: $0 [--root STUDIO_ROOT] [--baseline FILE] [--print-baseline] [--quiet]"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --root)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            studio_root="$2"
            shift 2
            ;;
        --baseline)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            baseline_file="$2"
            shift 2
            ;;
        --print-baseline)
            print_baseline=1
            shift
            ;;
        --quiet)
            quiet=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if ! studio_root="$(cd "${studio_root}" 2>/dev/null && pwd -P)"; then
    echo "ERROR: Studio root does not exist." >&2
    exit 2
fi

source_root="${studio_root}/src"
if [ ! -d "${source_root}" ]; then
    echo "ERROR: Studio source root does not exist: ${source_root}" >&2
    exit 2
fi

if [ -z "${baseline_file}" ]; then
    baseline_file="${studio_root}/scripts/architecture_baseline.tsv"
fi

if [ "${print_baseline}" -eq 0 ] && [ ! -f "${baseline_file}" ]; then
    echo "ERROR: Architecture baseline does not exist: ${baseline_file}" >&2
    exit 2
fi

guard_tmp="$(mktemp -d "${TMPDIR:-/tmp}/zanna-studio-architecture.XXXXXX")"
trap 'rm -rf "${guard_tmp}"' EXIT
generated_debt="${guard_tmp}/generated.tsv"
observed_keys="${guard_tmp}/observed.keys"
: > "${generated_debt}"
: > "${observed_keys}"

failures=0
source_count=0
line_debt_count=0
dependency_debt_count=0
default_limit=600
probe_limit=1700

fail() {
    echo "ERROR: $*" >&2
    failures=$((failures + 1))
}

status() {
    if [ "${quiet}" -eq 0 ]; then
        if [ "${print_baseline}" -eq 1 ]; then
            echo "$*" >&2
        else
            echo "$*"
        fi
    fi
}

limit_for() {
    case "$1" in
        src/main.zia) echo 800 ;;
        src/ui/app_shell.zia) echo 2050 ;;
        src/commands/edit_commands.zia) echo 1325 ;;
        src/editor/completion.zia) echo 1300 ;;
        src/commands/search_commands.zia) echo 925 ;;
        src/core/project_manager.zia) echo 900 ;;
        src/commands/file_commands.zia) echo 900 ;;
        src/core/document_manager.zia) echo 650 ;;
        src/ui/ide_overlays.zia) echo 750 ;;
        src/build/debug_session.zia) echo 500 ;;
        src/build/build_system.zia) echo 450 ;;
        src/probes/*) echo "${probe_limit}" ;;
        *) echo "${default_limit}" ;;
    esac
}

layer_for() {
    local relative_path="$1"
    local below_src="${relative_path#src/}"
    case "${below_src}" in
        */*) echo "${below_src%%/*}" ;;
        *) echo "root" ;;
    esac
}

# Return success when an edge points upward against architecture.md. These are
# deliberately narrow: orchestration layers may depend broadly, but leaf/model
# layers cannot acquire new UI or command ownership.
is_forbidden_dependency() {
    local source_layer="$1"
    local target_layer="$2"
    case "${source_layer}:${target_layer}" in
        zia:zia|zia:services) return 1 ;;
        zia:*) return 0 ;;
        basic:basic|basic:services|basic:zia) return 1 ;;
        basic:*) return 0 ;;
        services:services|services:zia) return 1 ;;
        services:*) return 0 ;;
        core:app|core:commands|core:ui|core:editor|core:build|core:scm|core:terminal) return 0 ;;
        editor:app|editor:commands|editor:ui|editor:build|editor:scm|editor:terminal) return 0 ;;
        build:app|build:commands|build:ui|build:editor|build:scm|build:terminal) return 0 ;;
        commands:app) return 0 ;;
        ui:app|ui:commands) return 0 ;;
        *) return 1 ;;
    esac
}

baseline_line_limit() {
    local relative_path="$1"
    awk -F '\t' -v path="${relative_path}" '
        $1 == "line" && $2 == path { print $3; found = 1; exit }
        END { if (!found) exit 1 }
    ' "${baseline_file}"
}

baseline_has_dependency() {
    local source_path="$1"
    local target_path="$2"
    awk -F '\t' -v source="${source_path}" -v target="${target_path}" '
        $1 == "dep" && $2 == source && $3 == target { found = 1; exit }
        END { exit found ? 0 : 1 }
    ' "${baseline_file}"
}

record_line_debt() {
    local relative_path="$1"
    local lines="$2"
    printf 'line\t%s\t%s\n' "${relative_path}" "${lines}" >> "${generated_debt}"
    printf 'line\t%s\n' "${relative_path}" >> "${observed_keys}"
}

record_dependency_debt() {
    local source_path="$1"
    local target_path="$2"
    printf 'dep\t%s\t%s\n' "${source_path}" "${target_path}" >> "${generated_debt}"
    printf 'dep\t%s\t%s\n' "${source_path}" "${target_path}" >> "${observed_keys}"
}

validate_baseline() {
    [ "${print_baseline}" -eq 1 ] && return

    if ! awk -F '\t' '
        /^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
        $1 == "line" && NF == 3 && $2 ~ /^src\// && $3 ~ /^[0-9]+$/ { next }
        $1 == "dep" && NF == 3 && $2 ~ /^src\// && $3 ~ /^src\// { next }
        { print "invalid baseline row " NR ": " $0 > "/dev/stderr"; bad = 1 }
        END { exit bad ? 1 : 0 }
    ' "${baseline_file}"; then
        fail "Architecture baseline has invalid rows: ${baseline_file}"
    fi

    awk -F '\t' '
        /^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
        $1 == "line" { print $1 "\t" $2; next }
        $1 == "dep" { print $1 "\t" $2 "\t" $3 }
    ' "${baseline_file}" | LC_ALL=C sort | uniq -d > "${guard_tmp}/duplicate.keys"
    while IFS= read -r duplicate; do
        [ -n "${duplicate}" ] && fail "Duplicate architecture baseline key: ${duplicate}"
    done < "${guard_tmp}/duplicate.keys"
}

validate_baseline
status "Zanna Studio architecture guard"

while IFS= read -r file; do
    source_count=$((source_count + 1))
    relative_path="${file#${studio_root}/}"
    lines="$(wc -l < "${file}" | tr -d ' ')"
    limit="$(limit_for "${relative_path}")"

    if [ "${lines}" -gt "${limit}" ]; then
        line_debt_count=$((line_debt_count + 1))
        record_line_debt "${relative_path}" "${lines}"
        if [ "${print_baseline}" -eq 0 ]; then
            if baseline_limit="$(baseline_line_limit "${relative_path}")"; then
                if [ "${lines}" -gt "${baseline_limit}" ]; then
                    fail "${relative_path} grew to ${lines} lines; ratchet is ${baseline_limit} (target ${limit})."
                elif [ "${lines}" -lt "${baseline_limit}" ]; then
                    fail "${relative_path} shrank to ${lines} lines; tighten its baseline from ${baseline_limit}."
                fi
            else
                fail "${relative_path} has ${lines} lines; budget is ${limit} and no debt baseline exists."
            fi
        fi
    fi

    header="$(sed -n '1,90p' "${file}")"
    if ! grep -q 'MODULE:' <<< "${header}"; then
        if ! grep -q '^// File:' <<< "${header}" ||
           ! grep -q '^// Purpose:' <<< "${header}" ||
           ! grep -q '^// Key invariants:' <<< "${header}"; then
            fail "${relative_path} lacks a full source header or legacy MODULE teaching block near the top."
        fi
    fi

    module_count="$(grep -Ec '^[[:space:]]*module[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*;' "${file}" || true)"
    case "${relative_path}" in
        src/probes/*)
            if [ "${module_count}" -gt 1 ]; then
                fail "${relative_path} has ${module_count} module declarations; probe entry points allow at most one."
            fi
            ;;
        *)
            if [ "${module_count}" -ne 1 ]; then
                fail "${relative_path} has ${module_count} module declarations; expected exactly one."
            fi
            ;;
    esac

    while IFS= read -r bind_path; do
        [ -n "${bind_path}" ] || continue
        candidate="$(dirname "${file}")/${bind_path}"
        case "${candidate}" in
            *.zia) ;;
            *) candidate="${candidate}.zia" ;;
        esac

        candidate_dir="$(dirname "${candidate}")"
        if [ ! -d "${candidate_dir}" ]; then
            fail "${relative_path} binds missing local module '${bind_path}'."
            continue
        fi
        candidate_dir="$(cd "${candidate_dir}" && pwd -P)"
        target="${candidate_dir}/$(basename "${candidate}")"
        if [ ! -f "${target}" ]; then
            fail "${relative_path} binds missing local module '${bind_path}'."
            continue
        fi
        case "${target}" in
            "${source_root}"/*) ;;
            *)
                fail "${relative_path} binds outside the Studio source root: '${bind_path}'."
                continue
                ;;
        esac

        target_relative="${target#${studio_root}/}"
        source_layer="$(layer_for "${relative_path}")"
        target_layer="$(layer_for "${target_relative}")"
        if is_forbidden_dependency "${source_layer}" "${target_layer}"; then
            dependency_debt_count=$((dependency_debt_count + 1))
            record_dependency_debt "${relative_path}" "${target_relative}"
            if [ "${print_baseline}" -eq 0 ] &&
               ! baseline_has_dependency "${relative_path}" "${target_relative}"; then
                fail "Forbidden layer edge ${relative_path} -> ${target_relative}; move shared behavior downward."
            fi
        fi
    done < <(sed -n 's/^[[:space:]]*bind[[:space:]]*"\([^"]*\)".*/\1/p' "${file}")
done < <(find "${source_root}" -name '*.zia' -type f | LC_ALL=C sort)

if [ "${print_baseline}" -eq 0 ]; then
    awk -F '\t' '
        /^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
        $1 == "line" { print $1 "\t" $2; next }
        $1 == "dep" { print $1 "\t" $2 "\t" $3 }
    ' "${baseline_file}" | LC_ALL=C sort -u > "${guard_tmp}/baseline.keys"
    LC_ALL=C sort -u "${observed_keys}" > "${guard_tmp}/current.keys"
    comm -23 "${guard_tmp}/baseline.keys" "${guard_tmp}/current.keys" > "${guard_tmp}/stale.keys"
    while IFS= read -r stale; do
        [ -n "${stale}" ] && fail "Stale architecture baseline entry: ${stale}; regenerate the baseline."
    done < "${guard_tmp}/stale.keys"
fi

status "Checked ${source_count} sources; tracked ${line_debt_count} size debts and ${dependency_debt_count} layer debts."

if [ "${failures}" -ne 0 ]; then
    status "Architecture guard failed with ${failures} issue(s)."
    exit 1
fi

if [ "${print_baseline}" -eq 1 ]; then
    printf '# Zanna Studio architecture debt baseline.\n'
    printf '# Generated by: scripts/check_architecture.sh --print-baseline\n'
    printf '# kind<TAB>source<TAB>maximum-lines-or-target\n'
    LC_ALL=C sort -u "${generated_debt}"
else
    status "Architecture guard passed."
fi
