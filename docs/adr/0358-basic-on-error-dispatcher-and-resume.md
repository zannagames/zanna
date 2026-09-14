---
status: accepted
audience: contributors
last-verified: 2026-09-13
---

# ADR 0358: BASIC ON ERROR Dispatcher and RESUME

## Status

Accepted. Replaces the BASIC `ON ERROR GOTO` lowering, implements `RESUME`, `RESUME NEXT`, and
`RESUME <label>` (defect audit #22), and makes native code honour the IL rule that `alloca`
memory starts zeroed.

## Context

BASIC error handling is dynamic. `ON ERROR GOTO <label>` can run anywhere, including in a loop,
a branch, or a label reached by `GOTO`. The handler is ordinary code that ends with a `RESUME`
form, and `RESUME` must retry the failed statement or continue after it. The IL exception model
is structured:

- Trap dispatch pops the selected handler before entering it, and no `resume.*` form pushes it
  again.
- `resume.*` and `err.get_*` appear only in handler-shaped blocks (`eh.entry` first, error and
  resume-token parameters). The token travels only through block parameters (ADR 0005).
- The verifier simulates the handler stack on every path, so each `ret` must be reached with the
  same depth and no `eh.pop` may underflow. The block holding a handler's `eh.push` must also
  dominate every block the handler protects.

The previous lowering pushed the handler at each `ON ERROR GOTO`, popped it at each return, and
lowered every `RESUME` form to `trap`. Handler code ran in the handler block's chain and ended
with a return, so programs never resumed. Any handler reached by a path that did not pass its
push failed verification.

Two related native defects also surfaced:

- Native backends map an `alloca` to an uninitialised frame slot. `docs/il/il-guide.md` defines
  alloca memory as zero-initialised, and both VMs clear it. A numeric `DIM` read before its first
  assignment returned stack garbage at `-O0`, and in any function whose slots `mem2reg` does not
  promote (for example every function with exception handling).
- `Zanna.Runtime.Unsafe.RaiseKind` raised without a message, so a re-raised error reported
  `Unknown trap`. This also affected Zia's typed-catch rethrow.

## Decision

### One dispatcher per procedure

A procedure, method, or module body that contains `ON ERROR GOTO <label>` gets one dispatcher.

- **Slots.** The entry block allocates and zeroes the dispatcher's slots:
  - `selected`: arm target of the chosen label, or 0 for none.
  - `target`: where the arm continues.
  - `site`: resume site of the running statement.
  - `failed site`: resume site of the statement whose error is handled.
  - `kind`, `code`, `line`: the handled error.
  - `running`: set while the handler runs.
- **Arm block.** The arm is the only `eh.push` of the dispatcher's handler. After the push it
  switches on `target`, falling back to the first body block. The entry, the handler entry, and
  every `RESUME` reach the arm with the handler removed. The stack therefore has depth 1 on every
  path through the body, every return pops exactly once, and the arm dominates all protected code.
- **`ON ERROR GOTO <label>`** stores the label's arm target in `selected`.
- **`ON ERROR GOTO 0`** clears `selected`. While a handler runs, it instead raises the handled
  error again, as QBasic reports it.
- **Handler entry** is a chain of handler-shaped blocks:
  1. If a handler is already running, or no label is selected, raise the error again. The
     dispatcher is no longer installed, so it reaches the caller's handler or ends the program.
     `err.get_kind`, `err.get_code`, and `err.get_line` go through `Zanna.Runtime.Unsafe.RaiseKind`.
  2. Otherwise record `kind`, `code`, and `line`; set `running`; copy `site` to `failed site` and
     `selected` to `target`.
  3. Consume the token with `resume.label %tok, ^arm`.
- **`RESUME`** raises `RESUME without error` unless a handler is running; that error reaches the
  selected handler like any other. It then:
  1. clears `running` and `ERR`;
  2. stores the arm target of its destination: the failed statement's start for `RESUME`, the
     block after it for `RESUME NEXT` (both through dispatch blocks that switch on `failed site`),
     or the label for `RESUME <label>`;
  3. pops the handler and branches to the arm.
- **`ERR`** reads the `code` slot. The code is Zanna's error code (for example
  `Err_FileNotFound` = 1), not a QBasic error number.

### Resume sites

In a body that also contains `RESUME` or `RESUME NEXT`, each statement records a resume site:

- The statement starts in its own block, which stores the site id in `site`.
- Lowering continues in a fresh block, which is the `RESUME NEXT` target. After a statement that
  ends its block (a `RETURN`, or `RESUME` itself), that block is reached only through
  `RESUME NEXT`.
- `RESUME` records a site because "RESUME without error" is an error of its own. Declarations,
  labels, statement lists, `ON ERROR`, `GOTO`, `EXIT`, `END`, and `NEXT` record none.
- A `TRY` or `USING` statement records one site, and statements inside it record none. Their
  bodies run with a second handler installed or hold a resume token, so the arm cannot enter them.

Because the dispatch blocks jump into the middle of loops and `SELECT` arms, state those
constructs carried in SSA values moves to memory in these bodies:

- `FOR` keeps its end and step in entry-block slots.
- `FOR EACH` keeps its array, length, and index in entry-block slots.
- `SELECT CASE` releases selector temporaries on entry to each arm (adding an empty
  `CASE ELSE`) instead of where the arms rejoin.

Bodies without `RESUME` or `RESUME NEXT` keep their previous shape.

### Related BASIC rules

- **B1012** now means "`RESUME` requires `ON ERROR GOTO` in the same procedure", checked by
  scanning the whole body. The previous check followed the source order and was cleared by
  `RETURN`.
- **Labels** resolve only within their own procedure, and B1003 names the label.
- **Parser.** A label on a line of its own is kept, whether it is inside a procedure, before
  `END SUB`, or at the end of the file (defect #28). Procedure bodies keep lowering statements
  after a jump, since a label may follow it.
- **`END`** inside a SUB, FUNCTION, or method ends the program through
  `Zanna.System.Environment.Exit(0)`. It used to trap, or return 0 from an INTEGER FUNCTION.
  `END` also counts as a terminating statement for FUNCTION result analysis.
- **`@main`** starts from a fully reset procedure context, as do the synthesized OOP module
  initialiser and interface thunks. Handler state from an earlier procedure no longer leaks into
  them.

### Runtime and native code generation

- **`RaiseKind` message.** `Zanna.Runtime.Unsafe.RaiseKind(kind, code, line)` raises with the text
  `Zanna.Error.Message` reports for the error: the retained thrown message of a runtime error,
  otherwise the kind's default message (for example `Division by zero`). The signature is
  unchanged.
- **Zeroed allocas.** Both native pipelines run `zeroInitAllocas`
  (`src/codegen/common/NativeAllocaZeroInit.cpp`) after native EH lowering and before IL
  optimization.
  - Each constant-size `alloca` is followed by stores that clear exactly its bytes.
  - Allocations over 512 bytes clear their 8-byte chunks with a counted loop.
  - The IL optimizer removes stores that are overwritten and promotes slots whose zero is only an
    initial value.

## Consequences

- `RESUME`, `RESUME NEXT`, and `RESUME <label>` work on the reference VM, the bytecode VM, and
  native code (AArch64 and x86-64). The native paths are verified at `-O0`, `-O1`, and `-O2` by
  `native_run_basic_on_error_resume_*`.
- A procedure with `ON ERROR GOTO` pays for one handler push per arm entry. A procedure that also
  uses `RESUME` or `RESUME NEXT` gets one extra store and block per statement.
- An error the dispatcher raises again keeps its kind, code, line, and standard message, but the
  unhandled-trap report names the dispatcher block as the location.
- An error inside a `CATCH` body reaches the procedure's `ON ERROR` handler. `RESUME NEXT` then
  continues after the whole `TRY` statement.
- Named labels are still unique across a file.
- Native programs no longer read stack garbage from uninitialised locals, which matches the IL
  specification and the VMs.

Tests:

- `basic_runtime_test_basic_on_error_resume` and its native twins.
- `basic_errors_io`, `basic_errors_all`, `basic_fileio_rw`, `basic_errors_in_handler`, and
  `basic_errors_before_on_error`.
- The `basic_to_il_on_error_push_pop` and `basic_to_il_resume_forms` goldens.
- `test_native_alloca_zero_init`, and the `alloca_zero_init` program in the shared IL corpus
  (VM/native and optimization-level differentials).
