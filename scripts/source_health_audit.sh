#!/usr/bin/env bash
#===----------------------------------------------------------------------===//
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===//
#
# File: scripts/source_health_audit.sh
# Purpose: Enforces local source-health guardrails for high-ownership subsystems.
# Key invariants:
#   - Debt metrics may not exceed their checked-in ceilings.
#   - Coverage and capability metrics may not fall below their checked-in floors.
# Ownership/Lifetime:
#   - Repository files are borrowed read-only for one bounded audit invocation.
#   - Temporary pipeline state is process-local and released when the script exits.
# Links: scripts/source_health_baseline.tsv, docs/internals/testing.md
#
#===----------------------------------------------------------------------===//

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")"/.. && pwd)"
BASELINE_FILE="${ROOT_DIR}/scripts/source_health_baseline.tsv"
MODE="check"
VERBOSE=0

usage() {
    cat <<'EOF'
Usage: scripts/source_health_audit.sh [--check|--summary|--self-test] [--baseline FILE]

Local source-health audit. This is intentionally a repository-local CTest/tooling
gate, not a GitHub CI hook.

Options:
  --check          Compare current metrics to scripts/source_health_baseline.tsv.
  --summary        Print current metrics without failing on baseline drift.
  --self-test      Verify the audit can discover its required inputs.
  --baseline FILE  Override the baseline file.
  --verbose        Print extra detail for source file group scans.
  -h, --help       Show this help.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --check) MODE="check"; shift ;;
        --summary) MODE="summary"; shift ;;
        --self-test) MODE="self-test"; shift ;;
        --baseline)
            if [[ $# -lt 2 || "$2" == --* ]]; then
                echo "error: --baseline requires a file path" >&2
                usage >&2
                exit 1
            fi
            BASELINE_FILE="$2"
            shift 2
            ;;
        --verbose) VERBOSE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

cd "$ROOT_DIR"

RG_BIN=""
if command -v rg >/dev/null 2>&1; then
    RG_BIN="rg"
elif command -v rg.exe >/dev/null 2>&1; then
    # WSL inherits Windows executables with their .exe suffix. Prefer the
    # native ripgrep binary over recursively scanning /mnt/c with GNU grep.
    RG_BIN="rg.exe"
fi

need_file() {
    local path="$1"
    if [[ ! -e "$path" ]]; then
        echo "source_health_audit: missing required path: $path" >&2
        exit 1
    fi
}

rg_count() {
    local pattern="$1"
    shift
    if [[ -n "$RG_BIN" ]]; then
        { "$RG_BIN" -n -- "$pattern" "$@" </dev/null 2>/dev/null || true; } \
            | wc -l | tr -d ' '
    else
        { grep -R -n -E -- "$pattern" "$@" 2>/dev/null || true; } | wc -l | tr -d ' '
    fi
}

file_line_count_over() {
    local threshold="$1"
    shift
    local roots=()
    local root
    for root in "$@"; do
        [[ -e "$root" ]] && roots+=("$root")
    done

    if [[ -n "$RG_BIN" ]]; then
        { "$RG_BIN" --hidden --no-ignore --count '^' "${roots[@]}" \
            -g '*.c' -g '*.h' -g '*.cpp' -g '*.hpp' -g '*.inc' -g '*.def' \
            -g '*.zia' -g '*.bas' -g '*.il' -g '*.cmake' -g 'CMakeLists.txt' \
            -g '*.sh' -g '*.cmd' -g '*.ps1' </dev/null 2>/dev/null || true; } \
            | awk -F: -v threshold="$threshold" -v verbose="$VERBOSE" '
                $NF > threshold {
                    count += 1
                    if (verbose == 1) {
                        lines = $NF
                        sub(/:[^:]*$/, "", $0)
                        printf "  mega-file %s %s\n", lines, $0 > "/dev/stderr"
                    }
                }
                END { print count + 0 }
            '
        return
    fi

    local count=0
    local lines
    local file
    while read -r lines file; do
        [[ "$file" == "total" ]] && continue
        [[ "$lines" =~ ^[0-9]+$ ]] || continue
        if [[ "$lines" -gt "$threshold" ]]; then
            count=$((count + 1))
            if [[ "$VERBOSE" -eq 1 ]]; then
                printf '  mega-file %s %s\n' "$lines" "$file" >&2
            fi
        fi
    done < <(find "${roots[@]}" -type f ! -name '*.md' \
        \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' -o \
           -name '*.inc' -o -name '*.def' -o -name '*.zia' -o -name '*.bas' -o \
           -name '*.il' -o -name '*.cmake' -o -name 'CMakeLists.txt' -o \
           -name '*.sh' -o -name '*.cmd' -o -name '*.ps1' \) \
        -exec wc -l {} +)
    printf '%s\n' "$count"
}

manual_alloc_hotspot_count() {
    local threshold="$1"
    local count=0
    local hits=0
    local path
    while read -r hits path; do
        [[ -n "${path:-}" ]] || continue
        if [[ "$hits" -gt "$threshold" ]]; then
            count=$((count + 1))
            if [[ "$VERBOSE" -eq 1 ]]; then
                printf '  allocation-hotspot %s %s\n' "$hits" "$path" >&2
            fi
        fi
    done < <(
        if [[ -n "$RG_BIN" ]]; then
            { "$RG_BIN" -n 'malloc|calloc|realloc|free|new |delete ' \
                src/runtime src/bytecode src/tools/common/packaging src/tools/zanna \
                -g '!*.md' </dev/null 2>/dev/null || true; }
        else
            { grep -R -n -E 'malloc|calloc|realloc|free|new |delete ' \
                src/runtime src/bytecode src/tools/common/packaging src/tools/zanna 2>/dev/null || true; }
        fi | cut -d: -f1 | sort | uniq -c | sort -nr
    )
    printf '%s\n' "$count"
}

fuzz_targets_missing_corpus() {
    local missing=0
    local target
    local name
    while IFS= read -r target; do
        [[ -n "$target" ]] || continue
        name="${target#fuzz_}"
        if [[ ! -d "src/tests/fuzz/corpus/${name}" ]]; then
            missing=$((missing + 1))
            if [[ "$VERBOSE" -eq 1 ]]; then
                printf '  missing-corpus %s\n' "$target" >&2
            fi
        fi
    done < <(sed -n -E 's/^[[:space:]]*zanna_add_(3d_loader_)?fuzzer\((fuzz_[^[:space:]\)]+).*/\2/p' \
        src/tests/fuzz/CMakeLists.txt | sort -u)
    printf '%s\n' "$missing"
}

graphics_stub_function_count() {
    rg_count '^[A-Za-z_][A-Za-z0-9_ *]*[[:space:]]+rt_[A-Za-z0-9_]+[[:space:]]*\(' \
        src/runtime/graphics/common/*_stubs.c
}

graphics_stub_unclassified_count() {
    awk -v verbose="$VERBOSE" '
        BEGIN {
            function_pattern = "^[A-Za-z_][A-Za-z0-9_ *]*[[:space:]]+rt_[A-Za-z0-9_]+[[:space:]]*\\("
            classification_pattern = "trapping stub|silent stub|fallback|no-op|rt_graphics_trap|graphics_unavailable"
        }
        FNR == 1 {
            for (line_number in prior_lines)
                delete prior_lines[line_number]
        }
        {
            if ($0 ~ function_pattern) {
                classified = 0
                for (offset = 1; offset <= 10; ++offset) {
                    prior_number = FNR - offset
                    if (prior_number > 0 &&
                        tolower(prior_lines[prior_number]) ~ classification_pattern) {
                        classified = 1
                        break
                    }
                }
                if (!classified) {
                    count += 1
                    if (verbose == 1)
                        printf "  unclassified-stub %s:%s\n", FILENAME, FNR > "/dev/stderr"
                }
            }
            prior_lines[FNR] = $0
            if (FNR > 10)
                delete prior_lines[FNR - 10]
        }
        END { print count + 0 }
    ' src/runtime/graphics/common/*_stubs.c
}

metric_value() {
    local name="$1"
    case "$name" in
        runtime_api_contract_files)
            find src/il/runtime src/runtime -type f ! -name '*.md' \
                \( -name 'runtime.def' -o -name 'RuntimeSignatures*' -o -name 'rt_*.c' -o -name 'rt_*.h' \) \
                | wc -l | tr -d ' '
            ;;
        runtime_surface_policy_rules)
            rg_count 'RUNTIME_SURFACE_(EXPECT|INTERNAL)_' src/il/runtime/RuntimeSurfacePolicy.inc
            ;;
        vm_bridge_duplicate_markers)
            rg_count 'duplicate of ThreadsRuntime.cpp' src/bytecode src/vm
            ;;
        vm_callback_pointer_traps)
            rg_count 'VM callback pointers are not supported' src/vm src/bytecode
            ;;
        vm_semantics_duplication_tests)
            rg_count 'check_vm_semantics_duplication|VM_SEMANTICS_DUPLICATION' \
                src/tests scripts
            ;;
        x86_placeholder_il_adapters)
            rg_count 'placeholder until the canonical IL headers are wired|temporary IL structures' \
                src/codegen/x86_64
            ;;
        codegen_unsupported_markers)
            rg_count 'Phase A does not support|not supported|unsupported' src/codegen
            ;;
        aarch64_backend_test_targets)
            rg_count 'test_aarch64_|test_codegen_arm64|test_cross_platform_abi' \
                src/tests
            ;;
        runtime_import_audit_tests)
            rg_count 'test_runtime_import_audit|runtime_import_audit' \
                src/tests
            ;;
        graphics_stub_functions)
            graphics_stub_function_count
            ;;
        graphics_stub_unclassified_functions)
            graphics_stub_unclassified_count
            ;;
        graphics_disabled_test_markers)
            rg_count 'requires_graphics_disabled|ZANNA_GRAPHICS_DISABLED|graphics disabled' \
                src/tests src/runtime
            ;;
        fuzz_targets_missing_corpus)
            fuzz_targets_missing_corpus
            ;;
        fuzz_corpus_seed_files)
            find src/tests/fuzz/corpus -type f ! -name '*.md' | wc -l | tr -d ' '
            ;;
        fuzz_harness_registrations)
            rg_count 'zanna_add_(3d_loader_)?fuzzer\(' src/tests/fuzz/CMakeLists.txt
            ;;
        platform_skip_markers)
            rg_count 'ZANNA_PLATFORM_SKIP|SKIP:' src/tests tests examples scripts
            ;;
        platform_policy_allowlist_entries)
            rg_count '^[^#[:space:]]' scripts/platform_policy_allowlist.txt
            ;;
        raw_platform_macro_occurrences)
            rg_count '(_WIN32|__APPLE__|__linux__)' src include zannastudio tests
            ;;
        mega_files_over_4000_lines)
            file_line_count_over 4000 src include examples scripts cmake zannastudio tests
            ;;
        manual_alloc_hotspots_over_70)
            manual_alloc_hotspot_count 70
            ;;
        sanitizer_coverage_options)
            rg_count 'IL_SANITIZE_ADDRESS|IL_SANITIZE_UNDEFINED|IL_SANITIZE_THREAD|ZANNA_ENABLE_COVERAGE' \
                CMakeLists.txt
            ;;
        diagnostic_json_entrypoints)
            rg_count '--diagnostic-format|DiagnosticFormat::Json|--json' \
                src/tools/zanna src/tools/zia src/tools/zbasic src/tests/tools src/tests/unit
            ;;
        mcp_tool_definitions)
            rg_count 'tools\.push_back' src/tools/lsp-common/McpHandler.cpp
            ;;
        mcp_tool_dispatch_branches)
            rg_count 'name == p \+ "/' src/tools/lsp-common/McpHandler.cpp
            ;;
        mcp_argument_validators)
            rg_count 'validate[A-Za-z]+Args|require(Int|String|Object|Optional|NonEmpty)' \
                src/tools/lsp-common/McpHandler.cpp
            ;;
        lsp_optional_capability_gates)
            rg_count 'supports(Definition|References|Rename|SignatureHelp|WorkspaceSymbols|SemanticTokens)' \
                src/tools/lsp-common src/tools/zia-server src/tools/zbasic-server
            ;;
        basic_server_semantic_entrypoints)
            rg_count 'BasicCompilerBridge::(check|compile|completions|hover|symbols|dumpIL|dumpAst|dumpTokens)' \
                src/tools/zbasic-server
            ;;
        zbasic_server_test_targets)
            rg_count 'test_zbasic_server|test_basic_completion|test_basic_analysis' \
                src/tests/CMakeLists.txt src/tests/zbasic-server src/tests/basic
            ;;
        ide_basic_capability_gates)
            rg_count 'basicService\.name|basic semantic commands disabled|format unsupported basic' \
                src/zannastudio/src
            ;;
        ide_scheduler_capability_jobs)
            # Count both scheduler-backed feature jobs and the long-lived async
            # query-job owners that replaced direct scheduler markers. This
            # preserves the capability floor across the ownership refactor.
            rg_count 'JOB_(DIAGNOSTIC|COMPLETION|HOVER|SIGNATURE)|Basic(Query|WorkspaceQuery)Job|ProjectBindQueryJob|ZiaWorkspaceQueryJob' \
                src/zannastudio/src/editor
            ;;
        debug_adapter_protocol_markers)
            rg_count '@@VDBG@@|setBreakpoints|callStack|locals|continue|stepOver' \
                src/tools/zanna src/zannastudio/src/build
            ;;
        packaging_verifier_entrypoints)
            rg_count 'verify.*Payload|verifyPEZipOverlayNestedPayload|PkgVerify' \
                src/tools/common/packaging src/tools/zanna
            ;;
        packaging_negative_test_markers)
            rg_count 'reject|invalid|tamper|duplicate|uncovered|verify' \
                src/tests/tools/PackageCliTests.cmake
            ;;
        todo_fixme_markers)
            rg_count 'TODO|FIXME|HACK' src include zannastudio tests examples scripts
            ;;
        raw_not_implemented_markers)
            rg_count 'not implemented|unimplemented|placeholder' \
                src include zannastudio tests examples
            ;;
        *)
            echo "error: unknown metric: $name" >&2
            exit 2
            ;;
    esac
}

baseline_limit_for() {
    local name="$1"
    awk -v key="$name" '
        $0 ~ /^[[:space:]]*#/ || $0 ~ /^[[:space:]]*$/ { next }
        $1 == key { print $2; found = 1; exit }
        END { if (!found) exit 1 }
    ' "$BASELINE_FILE"
}

print_metrics() {
    local name
    while IFS= read -r name; do
        [[ -n "$name" ]] || continue
        printf '%-42s %s\n' "$name" "$(metric_value "$name")"
    done <<'EOF'
runtime_api_contract_files
runtime_surface_policy_rules
vm_bridge_duplicate_markers
vm_callback_pointer_traps
vm_semantics_duplication_tests
x86_placeholder_il_adapters
codegen_unsupported_markers
aarch64_backend_test_targets
runtime_import_audit_tests
graphics_stub_functions
graphics_stub_unclassified_functions
graphics_disabled_test_markers
fuzz_targets_missing_corpus
fuzz_corpus_seed_files
fuzz_harness_registrations
platform_skip_markers
platform_policy_allowlist_entries
raw_platform_macro_occurrences
mega_files_over_4000_lines
manual_alloc_hotspots_over_70
sanitizer_coverage_options
diagnostic_json_entrypoints
mcp_tool_definitions
mcp_tool_dispatch_branches
mcp_argument_validators
lsp_optional_capability_gates
basic_server_semantic_entrypoints
zbasic_server_test_targets
ide_basic_capability_gates
ide_scheduler_capability_jobs
debug_adapter_protocol_markers
packaging_verifier_entrypoints
packaging_negative_test_markers
todo_fixme_markers
raw_not_implemented_markers
EOF
}

run_check() {
    if [[ ! -f "$BASELINE_FILE" ]]; then
        echo "source_health_audit: baseline not found: $BASELINE_FILE" >&2
        exit 1
    fi

    local failures=0
    local name
    local value
    local limit
    while read -r name limit _rest; do
        [[ -n "${name:-}" ]] || continue
        [[ "$name" =~ ^# ]] && continue
        value="$(metric_value "$name")"
        printf '%-42s current=%s baseline=%s\n' "$name" "$value" "$limit"
        case "$name" in
            runtime_surface_policy_rules|vm_semantics_duplication_tests|aarch64_backend_test_targets|\
            runtime_import_audit_tests|graphics_disabled_test_markers|fuzz_corpus_seed_files|\
            fuzz_harness_registrations|sanitizer_coverage_options|diagnostic_json_entrypoints|\
            mcp_tool_definitions|mcp_tool_dispatch_branches|mcp_argument_validators|\
            lsp_optional_capability_gates|basic_server_semantic_entrypoints|\
            zbasic_server_test_targets|ide_scheduler_capability_jobs|\
            debug_adapter_protocol_markers|packaging_verifier_entrypoints|\
            packaging_negative_test_markers)
                if [[ "$value" -lt "$limit" ]]; then
                    echo "  regression: $name dropped below baseline" >&2
                    failures=$((failures + 1))
                fi
                ;;
            *)
                if [[ "$value" -gt "$limit" ]]; then
                    echo "  regression: $name exceeded baseline" >&2
                    failures=$((failures + 1))
                fi
                ;;
        esac
    done < "$BASELINE_FILE"

    if [[ "$failures" -ne 0 ]]; then
        echo "source_health_audit: ${failures} metric(s) regressed" >&2
        echo "Update the underlying code first; update scripts/source_health_baseline.tsv only when the new debt is intentional." >&2
        exit 1
    fi
    echo "source_health_audit: OK"
}

need_file src/bytecode/BytecodeVM.cpp
need_file src/vm/ThreadsRuntime.cpp
need_file src/codegen/x86_64/LowerILToMIR.hpp
need_file src/tests/fuzz/CMakeLists.txt
need_file src/tests/fuzz/corpus
need_file src/tools/common/packaging/PkgVerify.cpp
need_file src/zannastudio/src/editor/language_service.zia

if [[ "$MODE" == "self-test" ]]; then
    print_metrics >/dev/null
    echo "source_health_audit self-test: OK"
    exit 0
fi

if [[ "$MODE" == "summary" ]]; then
    print_metrics
    exit 0
fi

run_check
