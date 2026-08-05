---
status: active
audience: contributors
last-verified: 2026-08-04
---

# Source Health Guardrails

Zanna owns large parts of its stack directly: runtime APIs, VM dispatch, native
backends, graphics-disabled stubs, packaging formats, IDE services, and parser
fuzzers. That ownership is intentional. The local source-health audit keeps the
known high-ownership areas visible and prevents new debt from being added
silently.

This is a local repository check. It is not a GitHub CI workflow.

## Running The Audit

```bash
scripts/source_health_audit.sh --summary
scripts/source_health_audit.sh --check
ctest --test-dir build -R source_health_audit --output-on-failure
ctest --test-dir build -L audit --output-on-failure
```

`--summary` prints current metric values. `--check` compares those values with
`scripts/source_health_baseline.tsv`.

## Baseline Policy

Metrics in `scripts/source_health_baseline.tsv` are guardrails, not goals. A
baseline value means "do not make this worse without saying so." If a change
reduces debt, update the baseline downward in the same patch. If a change must
increase a metric, update the baseline only with an explanation in the review
notes and relevant documentation.

The audit handles two metric directions:

- Debt/complexity metrics fail when they increase.
- Coverage/scaffolding metrics fail when they decrease.

### August 2026 reconciliation

The runtime contract-file baseline includes six small collection-private
headers introduced to centralize ownership transfer and concrete-layout access.
They are intentional implementation boundaries rather than frontend API, and
`RuntimeSurfacePolicy.inc` classifies each one explicitly. The contract-file
baseline therefore moves from 871 to 877 instead of forcing those private
interfaces back into unrelated translation units.

Sixteen Graphics3D readback symbols also moved from internal-only policy into
the registered frontend surface. Each now has an explicit canonical-name to C
symbol expectation, and the six collection headers have explicit internal
classifications, raising the policy coverage baseline from 1165 to 1172.

The manual-allocation hotspot baseline remains 28. Centralized destruction of
HTTP-client recovery state reduced `rt_http_client.c` from 73 allocation/free
markers to 65, returning it below the greater-than-70 hotspot threshold.

## Current Metrics

| Metric | Purpose |
|--------|---------|
| `runtime_api_contract_files` | Tracks the size of the runtime ABI contract surface under `src/il/runtime` and `src/runtime`. |
| `runtime_surface_policy_rules` | Ensures the frontend-visible runtime surface policy remains explicit. |
| `vm_bridge_duplicate_markers` | Keeps known VM bridge duplication visible until it is removed. |
| `vm_callback_pointer_traps` | Tracks managed callback gaps in VM thread/parallel handlers. |
| `vm_semantics_duplication_tests` | Ensures VM/bytecode semantic-duplication checks remain registered. |
| `x86_placeholder_il_adapters` | Tracks x86 lowering code that still uses temporary IL adapter structures. |
| `codegen_unsupported_markers` | Tracks backend unsupported-feature/error-only paths. |
| `aarch64_backend_test_targets` | Tracks AArch64 and cross-platform backend test registrations. |
| `runtime_import_audit_tests` | Ensures native runtime import audit tests remain present. |
| `graphics_stub_functions` | Tracks the graphics-disabled runtime stub surface. |
| `graphics_stub_unclassified_functions` | Tracks stub functions lacking nearby trap/no-op/fallback classification language. |
| `graphics_disabled_test_markers` | Tracks disabled-graphics behavior coverage and platform markers. |
| `fuzz_targets_missing_corpus` | Fails if a fuzzer is added without a committed corpus directory. |
| `fuzz_corpus_seed_files` | Fails if committed fuzz seed coverage drops. |
| `fuzz_harness_registrations` | Ensures parser/protocol fuzz harness registrations remain present. |
| `platform_skip_markers` | Tracks platform/environment skip debt in tests and examples. |
| `platform_policy_allowlist_entries` | Tracks raw-platform-macro allowlist debt. |
| `raw_platform_macro_occurrences` | Tracks raw platform macro usage under source, scripts, and tests. |
| `mega_files_over_4000_lines` | Tracks very large source files that need special review care. |
| `manual_alloc_hotspots_over_70` | Tracks manual lifetime hotspots in runtime, bytecode, and packaging code. |
| `sanitizer_coverage_options` | Ensures local sanitizer/coverage knobs remain present. |
| `diagnostic_json_entrypoints` | Ensures machine-readable diagnostic surfaces remain present. |
| `mcp_tool_definitions` | Ensures MCP tool definitions remain registered in the shared server handler. |
| `mcp_tool_dispatch_branches` | Ensures every MCP tool keeps a dispatch branch. |
| `mcp_argument_validators` | Ensures MCP argument validation helpers remain present. |
| `lsp_optional_capability_gates` | Tracks LSP optional-capability gating. |
| `basic_server_semantic_entrypoints` | Tracks BASIC language-server semantic entry points. |
| `vbasic_server_test_targets` | Tracks BASIC server analysis/completion/LSP/MCP tests. |
| `ide_basic_capability_gates` | Tracks explicit BASIC IDE capability gating until adapters are wired. |
| `ide_scheduler_capability_jobs` | Tracks non-blocking Zanna Studio semantic job scheduling coverage. |
| `debug_adapter_protocol_markers` | Tracks debugger JSON protocol request/event coverage. |
| `packaging_verifier_entrypoints` | Ensures package verification entry points remain present. |
| `packaging_negative_test_markers` | Tracks package CLI negative-verification coverage. |
| `todo_fixme_markers` | Tracks source TODO/FIXME/HACK markers. |
| `raw_not_implemented_markers` | Tracks source "not implemented", "unimplemented", and placeholder markers. |

## Recommendation Mapping

The audit is the local enforcement layer for broad project-review
recommendations. It does not make the deep architectural changes by itself, but
it turns each recommendation area into source-backed pressure that fails locally
when the project moves backward.

| Area | Local guardrail |
|------|-----------------|
| Keep runtime ABI ownership explicit | `runtime_api_contract_files` |
| Keep frontend-visible runtime policy explicit | `runtime_surface_policy_rules` |
| Remove VM bridge duplication deliberately | `vm_bridge_duplicate_markers` |
| Close managed VM callback gaps deliberately | `vm_callback_pointer_traps` |
| Preserve VM/bytecode semantic duplication tests | `vm_semantics_duplication_tests` |
| Replace temporary x86 IL adapters with canonical IL structures | `x86_placeholder_il_adapters` |
| Shrink unsupported backend paths | `codegen_unsupported_markers` |
| Preserve AArch64/backend parity coverage | `aarch64_backend_test_targets` |
| Preserve native runtime import auditing | `runtime_import_audit_tests` |
| Keep graphics-disabled stubs visible | `graphics_stub_functions` |
| Classify graphics-disabled stubs as trap, no-op, or fallback | `graphics_stub_unclassified_functions` |
| Preserve disabled-graphics behavior tests | `graphics_disabled_test_markers` |
| Require a corpus for every fuzzer | `fuzz_targets_missing_corpus` |
| Preserve committed fuzz seeds | `fuzz_corpus_seed_files` |
| Preserve fuzz harness registrations | `fuzz_harness_registrations` |
| Reduce platform skip debt | `platform_skip_markers` |
| Reduce platform-policy allowlist debt | `platform_policy_allowlist_entries` |
| Reduce raw platform macro usage | `raw_platform_macro_occurrences` |
| Split or scrutinize very large files | `mega_files_over_4000_lines` |
| Reduce manual lifetime hotspots | `manual_alloc_hotspots_over_70` |
| Preserve sanitizer/coverage switches | `sanitizer_coverage_options` |
| Preserve machine-readable diagnostics | `diagnostic_json_entrypoints` |
| Preserve MCP tool registration | `mcp_tool_definitions` |
| Preserve MCP dispatch coverage | `mcp_tool_dispatch_branches` |
| Preserve MCP argument validation | `mcp_argument_validators` |
| Keep optional LSP capabilities gated | `lsp_optional_capability_gates` |
| Preserve BASIC server semantic entry points | `basic_server_semantic_entrypoints` |
| Preserve BASIC server tests | `vbasic_server_test_targets` |
| Keep BASIC IDE capability gaps explicit | `ide_basic_capability_gates` |
| Preserve non-blocking IDE semantic scheduling | `ide_scheduler_capability_jobs` |
| Preserve debugger JSON protocol coverage | `debug_adapter_protocol_markers` |
| Preserve packaging verification entry points | `packaging_verifier_entrypoints` |
| Preserve packaging negative tests | `packaging_negative_test_markers` |
| Drive TODO/FIXME/HACK markers down | `todo_fixme_markers` |
| Drive unimplemented/placeholder markers down | `raw_not_implemented_markers` |

## What This Does Not Do

This audit does not replace behavioral tests. It prevents untracked expansion of
known risk areas while deeper work lands through normal source changes and
focused tests. Examples:

- Removing the VM bridge duplication still needs VM/bytecode thread and async
  behavioral tests.
- Wiring BASIC into Zanna Studio still needs non-blocking adapters and display-safe
  probes.
- Backend lowering convergence still needs shared IL fixtures that run through
  every backend.
- Graphics stub classification still needs runtime tests that assert trap,
  no-op, or fallback behavior for representative APIs.
