---
status: active
audience: developers
last-verified: 2026-07-31
---

# Testing Guide

This document describes the testing infrastructure for the Zanna compiler stack. The test suite
contains 1,740 tests across unit, golden, end-to-end, differential, conformance, audit, and fuzz
categories.

## Test Suite Overview

```text
Unit Tests (src/tests/unit/, src/tests/il/, src/tests/vm/)
    |  Individual component testing: IL, VM opcodes, codegen, runtime
    v
Golden Tests (src/tests/golden/)
    |  Textual stability: diagnostic messages, IL output, optimizer output
    v
End-to-End Tests (src/tests/e2e/)
    |  Full pipeline: source -> IL -> VM/native -> compare output
    v
Differential Tests (src/tests/unit/codegen/)
    |  VM vs native backend equivalence (property-based)
    v
Conformance Tests (src/tests/conformance/)
    |  Cross-layer arithmetic semantics equivalence
    v
Fuzz Tests (src/tests/fuzz/)
       Continuous fuzzing of parser/lexer inputs
```

## Running Tests

```bash
# Build and run all tests
./scripts/build_zanna_linux.sh   # Linux
./scripts/build_zanna_mac.sh     # macOS
```

The build scripts honor environment variables for faster iteration on all
platforms (ccache is auto-detected; disable with `ZANNA_NO_CCACHE=1`):

| Variable | Effect |
|----------|--------|
| `ZANNA_BUILD_TYPE=RelWithDebInfo` | Override the full-suite script default build type (`Debug`) |
| `ZANNA_JOBS=<n>` | Override build parallelism |
| `ZANNA_CTEST_JOBS=<n>` | Override CTest parallelism independently from build jobs; macOS defaults to performance-core count |
| `ZANNA_FAST_DEBUG=0` | Disable the default fast-Debug compile mode (`-Og` on Linux/macOS or `/O1` with lean STL checks on Windows) |
| `ZANNA_SKIP_CLEAN=1` | Skip the clean-all step (incremental rebuild) |
| `ZANNA_SKIP_TESTS=1` | Build without running ctest |
| `ZANNA_TEST_LABEL=<label>` | Run only tests with the given ctest label |
| `ZANNA_RUN_SLOW_TESTS=1` | Include tests labeled `slow` |
| `ZANNA_SKIP_LINT=1`, `ZANNA_SKIP_AUDIT=1`, `ZANNA_SKIP_SMOKE=1`, `ZANNA_SKIP_INSTALL=1` | Skip the corresponding post-build stages |
| `ZANNA_SKIP_STUDIO=1` | Configure with `-DZANNA_INSTALL_ZANNASTUDIO=OFF`, skipping the multi-minute Zanna Studio native compile (the longest single build step) |
| `ZANNA_EXTRA_CMAKE_ARGS="-DZANNA_ENABLE_INDIVIDUAL_BASIC_TO_IL_GOLDEN_TESTS=ON"` | Register legacy per-case BASIC-to-IL golden tests alongside the default batch shards |
| `ZANNA_GFX_NO_ACTIVATE=1` | On macOS and Linux, show new ZannaGFX windows without making them the active app/window; CTest applies this automatically to `requires_display` and `graphics3d` tests |
| `ZANNA_GFX_HIDE_WINDOWS=1` | On macOS and Linux, keep ZannaGFX windows hidden while preserving framebuffer rendering; CTest applies this automatically to `requires_display` and `graphics3d` tests |
| `ZANNA_AUDIO_SILENT=1` | Keep platform-device output silent while still advancing voices, music, effects, and playback state; CTest applies this automatically to the main repository test suite |

Each build script holds an exclusive lock for its resolved build directory until
all build, test, validation, and install stages finish. A concurrent invocation
using the same directory exits with the owning process ID instead of cleaning or
regenerating files underneath the active run. Use a distinct `ZANNA_BUILD_DIR`
when concurrent builds are intentional.

```bash

# Run all tests (after building)
ctest --test-dir build

# Run tests by label
ctest --test-dir build -L codegen          # Code generation tests only
ctest --test-dir build -L bytecode         # Bytecode VM and VM/bytecode parity
ctest --test-dir build -L golden           # Golden file tests only
ctest --test-dir build -L "vm"             # VM tests only
ctest --test-dir build -L audit            # Local structural/source-health audits
ctest --test-dir build -L slow             # Opt-in long-running tests only

# Run a specific test
ctest --test-dir build -R test_zia_lexer

# Run the focused Game3D gameplay-runtime audit regressions
ctest --test-dir build -R 'test_rt_game3d_(runtime_audit|dialogue_facial|thirdperson)$' \
  --output-on-failure

# Prove the dependency-free Linux headless graphics configuration
ctest --test-dir build -R linux_headless_graphics_smoke --output-on-failure

# Run tests in parallel
ctest --test-dir build -j$(nproc)

# Run with verbose output on failure
ctest --test-dir build --output-on-failure

# List all labels
ctest --test-dir build --print-labels

# Update golden files after intentional changes
./scripts/update_goldens.sh                # Update all
./scripts/update_goldens.sh il_opt         # Update only optimizer goldens

# Run with sanitizers
./scripts/ci_full_sanitizer.sh

# Optional: run runtime correctness/performance/portability diagnostics
cmake --build build --target cppcheck-runtime -j 1
```

The `cppcheck-runtime` target is a manual, optional check; the canonical build
scripts never invoke it, so builds do not depend on cppcheck being installed.
When run by hand it analyzes `src/runtime` through the configured
`compile_commands.json` with the platform definitions applied and fails for
every unsuppressed warning, performance, or portability diagnostic.
Suppressions belong in `cppcheck-runtime.supp` and should name a checker or
exact intentional site, not hide a runtime directory.

### Measuring demo build performance

Use a forced rebuild for repeatable cold compiler measurements and a second
unforced run to validate dependency-stamp behavior:

```bash
./scripts/build_demos_linux.sh --rebuild --jobs "$(nproc)" --timings
./scripts/build_demos_linux.sh --jobs "$(nproc)" --timings
./scripts/build_demos_linux.sh --release --rebuild --jobs "$(nproc)" --timings
```

The first command measures the interactive O1/fast-link path, the second should
report unchanged demos as up to date, and the third measures release O2. For an
optimizer change-report audit, rerun a representative target with
`ZANNA_VERIFY_PASS_CHANGE_REPORTS=1`; this deliberately restores full-module
fingerprints around every pass and is not a performance configuration.

### Windows demo build and launch gate

Use the canonical PowerShell driver to rebuild, PE-validate, launch-smoke, and
publish the curated Windows demos:

```powershell
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File `
    scripts/build_demos_win.ps1 --clean --run
```

Each demo is compiled once into a private directory with its declared assets.
That exact generation is copied to a private smoke directory and replaces the
published `examples/bin/<demo>/` directory only after validation succeeds.
Publication retains a complete rollback generation during the directory swap.
`ZANNA_DEMO_TIMEOUT` controls the per-demo launch budget in seconds (default
`5`; selected large demos receive at least `10`), and
`ZANNA_DEMO_MAX_OUTPUT_BYTES` bounds each redirected stream (default `1048576`,
accepted range `4096` through `16777216`). A timeout counts as a successful
interactive smoke only after the complete Windows process tree is terminated
and the root process is reaped.

External `ZANNA_DEMO_ROOT`, `ZANNA_DEMO_BIN_DIR`, and `ZANNA_DEMO_MANIFEST`
values are resolved relative to the repository root when not absolute. Source,
output, manifest, host-tool build, and target build paths must be non-reparse
and non-overlapping. Destructive `--clean` is intentionally narrower: it is
allowed only when the output is exactly `<demo-root>\bin`. Demo inventories and
`zanna.project` metadata must be bounded BOM-free UTF-8; inventory names and
categories use the same lowercase grammar on every platform.

## Test Labels

| Label | Count | Description |
|-------|-------|-------------|
| `basic` | 95 | BASIC frontend (lexer, parser, sema, lowerer) |
| `bytecode` | — | Bytecode VM direct tests and IL VM/bytecode parity |
| `il` | 196 | IL core (parsing, serialization, verification, analysis, transforms) |
| `vm` | 112 | VM runtime (opcodes, traps, debugging, concurrency) |
| `runtime` | 373 | C runtime library (strings, collections, I/O, math, graphics, etc.) |
| `codegen` | 125 | Code generation (x86_64, AArch64, linker, binary encoding) |
| `oop` | 31 | Object-oriented programming (classes, inheritance, interfaces) |
| `golden` | 203 | Golden file regression (diagnostic messages, IL/optimizer output) |
| `e2e` | 19 | End-to-end pipeline tests |
| `examples` | — | Example/demo manifest audit and fast smoke |
| `demos` | 2 | Bridge to the externalized zannademos suite (see below) |
| `fuzz` | — | Fuzz corpus replay and fuzz-lane self-checks |
| `audit` | 46 | Zia audit corpus plus local source-health and structural drift audits |
| `ilopt` | 4 | IL optimizer pass golden tests |
| `conformance` | 10 | Arithmetic semantics cross-layer equivalence |
| `zia` | 101 | Zia language frontend tests |
| `namespace` | 5 | Namespace/module system tests |
| `tools` | 29 | CLI tools, language server tests |
| `smoke` | 5 | Quick sanity tests |
| `tui` | 28 | Terminal UI / REPL tests |
| `perf` | — | Performance benchmarks (excluded from default runs) |
| `slow` | platform-dependent | Long-running tests (excluded by the build scripts unless explicitly enabled) |

The build scripts exclude `slow` tests by default. Set `ZANNA_RUN_SLOW_TESTS=1`
to include them in the full suite, or use `ctest --test-dir build -L slow` to run
only that lane. A test may be slow on one platform only: Windows Debug tests
whose representative runtime is about 30 seconds or more are labeled `slow` on
Windows without changing the default lanes on Linux or macOS.

### The zannademos bridge (`demos` label)

The large showcase demos live in the separate
[zannademos repository](https://github.com/zannagames/zannademos),
conventionally cloned nested at `<zanna>/zannademos/` (gitignored). Their
probe suites run through `zannademos/scripts/run_demo_tests.sh`, bridged into
CTest by `scripts/zannademos_bridge.sh` (ADR 0241):

- `zannademos_smoke` — the fast lane; runs in every full ctest when the clone
  is present, shows as **Skipped** (never missing) when it is not.
- `zannademos_full` — the full probe suite; additionally gated on
  `ZANNA_RUN_DEMOS_FULL=1` to keep default wall time bounded:
  `ZANNA_RUN_DEMOS_FULL=1 ctest --test-dir build -L demos`.

The Studio scene-preview tests (`zia_zannastudio_scene_gameplay_preview{,_2d}`)
use zannademos scene fixtures and skip visibly when the clone is absent.

## Test Categories

### Unit Tests

Located in `src/tests/unit/`, `src/tests/il/`, `src/tests/vm/`. Test individual components:

Shared frontend infrastructure has a dedicated `test_frontend_common` unit
target. It covers arithmetic-folding boundaries, locale-independent literal
parsing, cursor and escape safety, scope/loop/block invariants, deterministic
string and keyword tables, instruction emission, and generated runtime method
inheritance. Run it directly with:

```bash
ctest --test-dir build -R '^test_frontend_common$' --output-on-failure
```

- IL core types, parsing, serialization
- IL optimizer passes (EHOpt, LoopRotate, Reassociate, SCCP, GVN, etc.)
- IL verifier checks (positive and negative)
- VM opcode semantics, traps, debugging
- Codegen utilities (peephole, register allocation, ISel)
- Runtime C functions (strings, collections, I/O, math)
- Runtime audio coverage, including `test_rt_audio_fx`,
  `test_rt_audio_integration`, `test_rt_sound3d_contract`, and
  `test_rt_sound3d_objects`
- Game3D gameplay-runtime hardening in `test_rt_game3d_runtime_audit`, with
  companion dialogue/facial and third-person target-lock cases; see the
  [July 2026 audit ledger](graphics3d-game-runtime-audit-2026-07.md)
- Frontend parser, sema, lowerer for both Zia and BASIC

### Golden Tests

Located in `src/tests/golden/`. Test textual stability of outputs:

- IL serialization format
- Zia/BASIC compiler diagnostic messages
- IL optimizer transformations
- Constant folding results

Golden tests use 10 CMake runner scripts. Helper functions in `TestHelpers.cmake` reduce
registration to one line per test.

BASIC-to-IL goldens are batched by default through an in-process runner split across
`ZANNA_BASIC_TO_IL_GOLDEN_BATCH_SHARDS` CTest shards (default `8`). Set
`-DZANNA_ENABLE_INDIVIDUAL_BASIC_TO_IL_GOLDEN_TESTS=ON` in `ZANNA_EXTRA_CMAKE_ARGS` to also
register the legacy one-CTest-per-case entries for targeted debugging.

### End-to-End Tests

Located in `src/tests/e2e/`. Test complete pipelines:

- Zia programs compiled and executed
- BASIC programs compiled and executed
- IL programs through VM and native backends

### Differential Tests

Located in `src/tests/unit/codegen/`. Verify that VM and native backends produce identical results
for the same IL programs.

`src/tests/codegen/aarch64/` is the dedicated home for AArch64 backend tests that
consume shared or end-to-end corpus inputs. Low-level instruction, pass, or
allocator tests may remain in `src/tests/unit/codegen/` when that is the clearer
ownership boundary.

### Shared IL Corpus

`src/tests/shared_corpus/il/` contains deterministic IL programs used by more
than one backend or execution engine. `success/` programs return a stable `i64`
and may write stable runtime stdout; `traps/` programs intentionally terminate
with one of the shared VM trap kinds. Avoid time, randomness, host file system
state, network access, and unbounded loops in this corpus.

The main consumers are:

- `test_bytecode_full_program_parity`: runs each success program on the IL VM,
  bytecode switch dispatch, and bytecode threaded dispatch, then compares return
  value and runtime stdout. It also checks trap programs and locks down
  bytecode/IL `TrapKind` value alignment for kinds 0-11.
- `test_codegen_aarch64_shared_corpus`: compiles representative success programs
  through the AArch64 command pipeline on every host without executing ARM64
  code, and checks deterministic assembly plus stable mnemonic markers.

Cross-backend native parity is enforced transitively through the VM: x86_64
native and AArch64 native each compare covered programs to the same VM
semantics, so widening shared-corpus VM-differential coverage widens backend
equivalence. Direct both-native comparison remains a slow, opt-in workflow for
hosts with emulation.

---

## Property-Based Differential Testing

### Overview

The differential testing framework generates random-but-valid IL programs and verifies that:

1. The IL passes verification
2. VM execution produces a result
3. Native (AArch64) execution produces the same result

This approach catches semantic differences between execution backends that hand-written tests might miss.

### IL Generator

The `ILGenerator` class (`src/tests/common/ILGenerator.hpp`) generates random IL modules:

```cpp
#include "common/ILGenerator.hpp"

// Create generator with specific seed for reproducibility
zanna::tests::ILGenerator generator(12345);

// Configure generation
zanna::tests::ILGeneratorConfig config;
config.minInstructions = 5;
config.maxInstructions = 15;
config.includeComparisons = true;

// Generate IL module
auto result = generator.generate(config);
std::cout << result.ilSource << "\n";
```

#### Configuration Options

| Option               | Default | Description                            |
|----------------------|---------|----------------------------------------|
| `minInstructions`    | 3       | Minimum instructions per block         |
| `maxInstructions`    | 20      | Maximum instructions per block         |
| `minBlocks`          | 1       | Minimum basic blocks                   |
| `maxBlocks`          | 4       | Maximum basic blocks                   |
| `includeComparisons` | true    | Include comparison operations          |
| `includeControlFlow` | true    | Include branches and control flow       |
| `minConstant`        | -10     | Minimum constant value                 |
| `maxConstant`        | 10      | Maximum constant value                 |
| `includeFloats`      | false   | Include floating-point operations      |
| `includeBitwise`     | true    | Include bitwise operations             |
| `includeShifts`      | true    | Include shift operations               |

#### Generated Operations

The generator produces IL using checked operations per the IL spec:

- `iadd.ovf` - Signed addition with overflow trap
- `isub.ovf` - Signed subtraction with overflow trap
- `imul.ovf` - Signed multiplication with overflow trap
- `sdiv.chk0` - Signed division with divide-by-zero trap
- `icmp_eq`, `icmp_ne` - Equality comparisons
- `scmp_lt`, `scmp_le`, `scmp_gt`, `scmp_ge` - Signed comparisons

### Reproducibility

All tests use seeded random number generators. When a test fails:

1. The seed is printed in the error message
2. Re-run with the same seed to reproduce the failure:
   ```cpp
   ILGenerator generator(failing_seed);
   auto result = generator.generate(config);
   ```

The `ReproducibilityWithSeed` test verifies that identical seeds produce identical IL.

### Running Differential Tests

```bash
# Build the test
cmake --build build --target test_diff_vm_native_property

# Run via ctest
ctest --test-dir build -R diff_vm_native_property --output-on-failure

# Run directly (shows iteration progress)
./build/src/tests/test_diff_vm_native_property
```

### Test Iterations

By default, the test runs 10 iterations per test case. You can override this with
`ZANNA_DIFF_ITERATIONS` (e.g., `ZANNA_DIFF_ITERATIONS=50` for local fuzzing). Each iteration:

1. Generates a new IL program from a unique seed
2. Verifies the IL
3. Runs on VM
4. Runs on native backend (AArch64 on Apple Silicon)
5. Compares exit codes (masked to 8 bits)

---

## Test Fixtures

### VmFixture

Located in `src/tests/common/VmFixture.hpp`. Provides a clean VM environment for testing:

```cpp
VmFixture fixture;
int64_t result = fixture.run(module);
```

### CodegenFixture

Located in `src/tests/common/CodegenFixture.hpp`. Orchestrates comparison between VM and native execution.

---

## Test Framework (TestHarness.hpp)

The framework is a lightweight, header-only, dependency-free test harness in `src/tests/TestHarness.hpp`.

### Assertion Macros

| Macro | Fatal | Description |
|-------|-------|-------------|
| `EXPECT_TRUE(expr)` / `ASSERT_TRUE(expr)` | No/Yes | Expression is truthy |
| `EXPECT_FALSE(expr)` / `ASSERT_FALSE(expr)` | No/Yes | Expression is falsy |
| `EXPECT_EQ(a, b)` / `ASSERT_EQ(a, b)` | No/Yes | Equality (prints both values) |
| `EXPECT_NE(a, b)` / `ASSERT_NE(a, b)` | No/Yes | Inequality (prints both values) |
| `EXPECT_GT(a, b)` / `ASSERT_GT(a, b)` | No/Yes | Greater than |
| `EXPECT_LT(a, b)` / `ASSERT_LT(a, b)` | No/Yes | Less than |
| `EXPECT_GE(a, b)` / `ASSERT_GE(a, b)` | No/Yes | Greater or equal |
| `EXPECT_LE(a, b)` / `ASSERT_LE(a, b)` | No/Yes | Less or equal |
| `EXPECT_NEAR(a, b, eps)` | No | Float near-equality |
| `EXPECT_CONTAINS(str, sub)` | No | String contains substring |
| `EXPECT_THROWS(expr, Type)` | No | Exception of Type thrown |
| `EXPECT_NO_THROW(expr)` | No | No exception thrown |
| `ZANNA_TEST_SKIP(reason)` | — | Skip test with reason |

Comparison macros (`EQ`, `NE`, `GT`, `LT`, `GE`, `LE`) print actual operand values on failure.

### Command-Line Options

- `--filter=PATTERN` — Run only tests matching glob (e.g., `--filter=ZiaLexer.*`)
- `--xml=PATH` — Write JUnit XML results for CI integration

### Fixtures (TEST_F)

```cpp
class MyFixture : public zanna_test::TestFixture {
protected:
    void SetUp() override { /* before each test */ }
    void TearDown() override { /* after each test */ }
};

TEST_F(MyFixture, TestName) {
    // fixture members accessible here
}
```

## Adding New Tests

### Unit Test Template

```cpp
#include "tests/TestHarness.hpp"

TEST(MySuite, MyTest) {
    EXPECT_EQ(1 + 1, 2);
    EXPECT_GT(result.size(), 0U);
    EXPECT_CONTAINS(output, "expected text");
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
```

### Golden Error Test

Add a `.bas` + `.stderr` file pair, then one line in `golden/CMakeLists.txt`:

```cmake
_golden_error(basic_error_my_test ${_BE} my_test)
```

### Golden Run Test

Add a `.bas` + `.stdout` file pair:

```cmake
_golden_basic_run(my_run_test ${_DIR} my_program my_program.stdout)
```

### IL Verifier Negative Test

Add a `.il` + `.expected` file pair in `src/tests/il/negatives/`, then register:

```cmake
zanna_add_ctest(il_verify_negative_my_case
    ${CMAKE_COMMAND} -DIL_VERIFY=${IL_VERIFY}
    -DFILE=${_DIR}/my_case.il -DEXPECT_FILE=${_DIR}/my_case.expected
    -P ${_DIR}/check_negative.cmake)
```

### Zia Runtime Test

Add a `.zia` file in `src/tests/fixtures/runtime/` that prints `RESULT: ok` on success:

```zia
// test_my_feature.zia
func main() {
    // ... test logic ...
    Say("RESULT: ok")
}
```

Register in `src/tests/CMakeLists.txt` under `ZIA_RUNTIME_TESTS`.

### Differential Test Template

For new differential tests, extend the pattern in `test_diff_vm_native_property.cpp`:

1. Create a test case with specific `ILGeneratorConfig`
2. Use `runDifferentialTest()` helper
3. Check `result.passed` and report `result.errorMessage` on failure

---

## Platform Support

- **VM**: All platforms
- **AArch64 Native**: macOS on Apple Silicon (tested)
- **x86_64 Native**: Validated on Windows with full codegen test suite passing

Differential tests automatically skip native execution on unsupported platforms.

---

## Concurrency Testing

### Overview

The VM concurrency tests verify thread-safety of the VM execution model:

- Each VM instance is single-threaded
- Thread-local storage (`tlsActiveVM`) correctly tracks active VM per thread
- `ActiveVMGuard` RAII pattern properly manages VM context binding
- Runtime callbacks preserve thread-local context
- Trap reports include correct VM context

`Zanna.Threads` adds additional tests that verify shared-memory threading semantics:

- FIFO-fair, re-entrant monitor behavior (`Zanna.Threads.Monitor`)
- Thread lifecycle and join timeouts (`Zanna.Threads.Thread`)
- FIFO-serialized safe variables (`Zanna.Threads.SafeI64`)
- Future/Async/Parallel result retention, listener trap isolation, and one-shot pool task error reporting
- Scheduler, debouncer/throttler, cancellation-token, channel, and concurrent collection synchronization behavior
- VM thread start override (`Zanna.Threads.Thread.Start`) and shared globals behavior

### Stress Test

Located in `src/tests/unit/test_vm_concurrency_stress.cpp`. Exercises:

- Multiple VMs running concurrently across threads
- Runtime function callbacks (e.g., `Zanna.Math.AbsInt`)
- Rapid VM creation and destruction
- Nested `ActiveVMGuard` usage

```bash
# Run with default settings (8 threads, 100 iterations)
./build/src/tests/test_vm_concurrency_stress

# Run with debug logging
./build/src/tests/test_vm_concurrency_stress --debug

# Custom thread/iteration counts
./build/src/tests/test_vm_concurrency_stress --threads 16 --iterations 500
```

### Running with ThreadSanitizer (TSan)

Sanitizer lanes are local opt-in checks. `scripts/ci_full_sanitizer.sh` is the
canonical entry point for broad ASan/UBSan coverage; the legacy
`scripts/ci_sanitizer_tests.sh` wrapper delegates to it. Use the TSan variants after VM,
runtime, or graphics concurrency changes.

```bash
# Broad ASan + UBSan lane
./scripts/ci_full_sanitizer.sh

# Generic VM/runtime TSan lane
./scripts/ci_full_sanitizer.sh --tsan

# Focused graphics3d concurrency TSan lane
./scripts/g3d_tsan_concurrency_lane.sh

# Confirm sanitizer toolchain availability without running the full lane
./scripts/ci_full_sanitizer.sh --self-test
```

Sanitizer builds default to four compiler and test workers because instrumented
debug translation units consume substantially more memory than ordinary builds.
Set `ZANNA_JOBS` and `ZANNA_CTEST_JOBS` to positive integers to tune build and
test parallelism respectively. `ZANNA_SANITIZER_TIMEOUT` controls the per-test
instrumented budget and defaults to 600 seconds. The driver passes the same
value into CMake as the minimum for tests with explicit `TIMEOUT` properties,
so those properties cannot silently override the sanitizer budget. The driver
rejects invalid or unbounded values.

The broad lanes exclude slow/performance-labelled and explicit scalability
tests, native output smokes and runtime-import archive parsing (their purpose is
inspecting unsanitized output artifacts), and installer smokes that require the
Studio payload disabled in sanitizer configurations. Their ordinary and slow
CTest lanes remain mandatory and are run separately.

#### Interpreting TSan Output

TSan reports data races with stack traces. Example:

```text
WARNING: ThreadSanitizer: data race
  Write of size 8 at 0x7fff5fbff9a0 by thread T2:
    #0 vm::someFunction() src/vm/VMContext.cpp:123
  Previous read of size 8 at 0x7fff5fbff9a0 by thread T1:
    #0 vm::otherFunction() src/vm/VMContext.cpp:456
```

Key fields:

- **Location**: Memory address involved in race
- **Operation**: Read or write, with size
- **Threads**: Which threads are racing
- **Stack traces**: Where the racing accesses occur

#### Known TSan Suppressions

Some benign races may be suppressed in a `tsan.supp` file:

```text
# Example suppression file (create as tsan.supp)
race:deliberate_benign_race_function
```

Run with suppressions:

```bash
TSAN_OPTIONS="suppressions=tsan.supp" ./build-tsan/src/tests/test_vm_concurrency_stress
```

### Measuring Coverage

Clang source-based coverage is available through the opt-in
`ZANNA_ENABLE_COVERAGE` CMake option and the local coverage lane:

```bash
./scripts/coverage.sh
```

The script configures `build-coverage/`, runs CTest with coverage profile output, and
writes:

- `coverage/summary.txt` — `llvm-cov report` summary.
- `coverage/subsystems.txt` — ranked per-subsystem line-coverage rollup.
- `coverage/html/index.html` — drill-down HTML report.

Coverage is for visibility only; it does not enforce thresholds. Use it when adding a new
subsystem or when ranking weak areas by measured coverage instead of intuition.

### Example Smoke

Examples are classified by `examples/smoke_manifest.tsv` and checked by
`scripts/example_smoke.sh`.

```bash
./scripts/example_smoke.sh --audit              # all example sources classified
./scripts/example_smoke.sh --fast               # fast runnable/checkable subset
./scripts/example_smoke.sh --all                # full manifest sweep
ctest --test-dir build -L examples --output-on-failure
```

The manifest keeps graphical, project, benchmark, and non-standalone examples explicit
while letting CTest run a fast headless smoke over compact Zia language examples,
tutorial BASIC, and runnable IL samples.

### Performance Baselines

`scripts/benchmark.sh` stores JSONL runs in `misc/benchmarks/results.jsonl`; checked
baselines live in `misc/benchmarks/baseline.jsonl`.

```bash
./scripts/benchmark.sh --zanna-only
./scripts/benchmark_compare.sh
./scripts/benchmark_compare.sh --self-test
```

Use `--zanna-only` for the canonical local regression lane when external language
toolchains are not relevant. `benchmark_compare.sh` compares only common
program/mode pairs and fails on Zanna-mode regressions above the configured threshold.
Refresh baselines with `benchmark.sh --set-baseline` only after reviewing the measured
delta and host metadata in the JSONL output.

### Threading Model Invariants

The tests verify these invariants from `docs/internals/vm.md`:

1. **Single-threaded per VM**: Each VM instance processes instructions on one thread at a time
2. **Thread-local binding**: `tlsActiveVM` holds the active VM for the current thread
3. **Nesting allowed**: Same VM can be bound multiple times (recursive callbacks)
4. **Different VM forbidden**: Attempting to bind a different VM on the same thread triggers assertion failure (debug
   builds)
5. **Clean state**: After VM::run() completes, thread-local state is cleared

### Defined vs Undefined Threaded Programs

Zanna’s VM/native determinism guarantee applies to **defined** threaded programs: shared mutable state must be accessed
via `Zanna.Threads.Monitor` (or the `Zanna.Threads.Safe*` wrappers). Programs with data races are **undefined** (VM and
native are not required to match).

---

## Golden File Update Workflow

When you intentionally change compiler diagnostics, optimizer output, or IL format:

```bash
# Update all failing golden files
./scripts/update_goldens.sh

# Update only optimizer golden files
./scripts/update_goldens.sh il_opt

# Update only BASIC error golden files
./scripts/update_goldens.sh basic_error
```

The script runs each golden test, skips those already passing, and re-runs failures with
`UPDATE_GOLDEN=1` to overwrite expected output files with actual output.

## Fuzz Testing

Fuzz harnesses are in `src/tests/fuzz/` (requires `ZANNA_ENABLE_FUZZ=ON`). Each harness
has a committed seed corpus under `src/tests/fuzz/corpus/<target-without-fuzz-prefix>/`.

Two cadences are supported:

- **Replay:** fuzz-enabled CMake builds register `<target>_replay` CTests over committed
  corpora, labelled `fuzz`.
- **Exploration:** `scripts/fuzz_smoke.sh` builds every discovered fuzzer and time-boxes
  mutation over each corpus.

```bash
./scripts/fuzz_smoke.sh --self-test
./scripts/fuzz_smoke.sh --list
ZANNA_FUZZ_SECONDS=10 ./scripts/fuzz_smoke.sh

cmake -S . -B build-fuzz -DZANNA_ENABLE_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz --target fuzz_zia_parser
ctest --test-dir build-fuzz -L fuzz --output-on-failure
```

When a new parser, wire format, or protocol surface is added, add a harness plus a small
minimized corpus before treating the surface as covered. New crashes should be minimized
and checked into the relevant corpus directory.

## Source-Health Audit

`scripts/source_health_audit.sh` is a local structural guardrail for
high-ownership subsystems. It tracks 35 source-backed risk and coverage counters
across runtime surface policy, VM duplication and callback gaps, backend
unsupported paths, graphics-disabled stubs, fuzz corpus coverage, platform
policy debt, large files, manual allocation hotspots, machine-readable tooling,
MCP/LSP server coverage, Zanna Studio capability gates, debugger protocol coverage,
and packaging verification.

```bash
scripts/source_health_audit.sh --summary
scripts/source_health_audit.sh --check
ctest --test-dir build -R source_health_audit --output-on-failure
ctest --test-dir build -L audit --output-on-failure
```

The check compares current values against
`scripts/source_health_baseline.tsv`. Debt metrics should move down over time.
Coverage/scaffolding metrics should not drop. See
[Source Health Guardrails](source-health.md).

## Future Work

- Reusable DifferentialTestFixture for VM vs native comparisons
- Test consolidation: merge 196 standalone runtime tests into ~30 themed files
- LoopRotate, GVN, DSE, LICM edge-case tests
- x86_64 codegen parity with AArch64 test coverage
- Zia diagnostic golden test directory
