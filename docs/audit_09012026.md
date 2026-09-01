---
status: active
audience: contributors
last-verified: 2026-09-01
---

# Zanna Defect Audit — 2026-09-01

Engine defects found while validating the documentation tree against the actual
source and running toolchain. Every entry below was reproduced against the
installed toolchain (`zanna v0.3.1-snapshot`, source `533060d82`).

This file records **code** defects only. Documentation corrections are tracked
separately with the affected page.

**31 defects**, found while validating the documentation tree. Highest-impact
first: #26 (segfault calling a function reference), #31 (use-after-free in
`Channel.Send`), #6 (`zanna check` passes code that `build` rejects), #22
(`RESUME` unimplemented), #24/#25 (Zia lowering emits invalid IL), #17/#21
(namespaced BASIC classes unusable), #12 (`Byte` is 32-bit), #15 (static fields
unreadable from their own class).

| # | Area | Severity | Summary |
|---|------|----------|---------|
| 1 | BASIC frontend | High | `STATIC` field emits malformed IL global `@C::VALUE` |
| 2 | BASIC frontend | High | `STATIC DESTRUCTOR` never runs |
| 3 | BASIC frontend | High | `STATIC SUB NEW()` (static constructor) never runs |
| 4 | BASIC frontend | High | No working way to compare an object reference to `NOTHING` |
| 5 | Zia frontend | High | `Byte` fails IL verification in string concat and `as Byte` narrowing |
| 6 | Driver (`zanna check`) | High | `check` passes code that `run`/`build` reject |
| 7 | VM / driver | Medium | Breakpoint exit code `10` only produced when `@main` returns `i64` |
| 8 | Test suite | Medium | `golden/oop/static_destructor` fixture is orphaned |
| 9 | BASIC frontend | Low | Static methods resolve only through an instance receiver |
| 10 | Diagnostics catalog | Low | `B1006` summary covers only one of its several uses |
| 11 | Zia frontend | Low | `foreign func` declaration emits a spurious unused-parameter warning |
| 12 | Zia frontend | High | `Byte` is a 32-bit type, not 8-bit; `as Byte` does not narrow or trap |
| 13 | Zia parser | Medium | A variable named `map` or `set` breaks `for x in <var> { ... }` |
| 14 | Zia parser | Low | Documented `Error.type` accessor is unusable (`type` is a reserved word) |
| 15 | Zia frontend | High | Static fields are unreadable from inside their own class (`V3000 ... reached lowering`) |
| 16 | BASIC frontend | Low | `ME` is accepted inside a `STATIC SUB` instead of being rejected |
| 17 | BASIC frontend | High | Fields on a class declared inside a `NAMESPACE` are inaccessible |
| 18 | BASIC frontend | Medium | `USING` inside a `NAMESPACE` block is accepted but produces broken IL |
| 19 | BASIC frontend | Medium | Aliased `USING X = Ns` works for types but not for procedure calls |
| 20 | BASIC frontend | Medium | False-positive `B3001 index out of bounds` on the highest valid literal index |
| 21 | BASIC frontend | High | A namespaced class gets no implicit default constructor |
| 22 | BASIC frontend | High | `RESUME`, `RESUME NEXT`, and `RESUME <label>` are unimplemented (lower to `trap`) |
| 23 | IL verifier | Medium | Verifier accepts a function whose entry block omits the signature's parameters |
| 24 | Zia lowering | High | Managed local + early `return` + `try`/`catch` emits IL that violates SSA dominance |
| 25 | Zia lowering | High | `return null` from a `String?` function fails IL verification |
| 26 | Zia lowering / VM | **Critical** | Calling a function through a `&function` reference SEGFAULTS |
| 27 | BASIC completions | Low | Completion provider offers 4 builtins that do not exist |
| 28 | BASIC frontend | Low | A trailing label with no following statement fails with a synthetic line number |
| 29 | Runtime doc fragments | Low | `@details` prose names classes `Physics3DBody` / `Physics3DWorld`; the registered names are `PhysicsBody3D` / `PhysicsWorld3D` |
| 30 | Runtime (GC) | Medium | Cycle collection is opt-in and off by default, so cyclic graphs leak silently |
| 31 | Runtime (Threads) | **Critical** | `Channel.Send` does not retain its payload — use-after-free across threads |

---

## 1. `STATIC` field emits a malformed IL global name

**Severity:** High — a two-line class fails to compile.

```basic
CLASS C
  STATIC value AS INTEGER
END CLASS
PRINT "compiled"
END
```

```text
error[V-IL-VERIFY]: invalid IL after BASIC lowering: global has malformed name @C::VALUE
```

The BASIC lowerer builds the static-field global as `<Class>::<FIELD>`, but `::`
is not legal in an IL global name, so the module never verifies. Static fields
are documented in `docs/languages/basic-grammar.md`.

---

## 2. `STATIC DESTRUCTOR` never runs

**Severity:** High — documented cleanup mechanism silently does nothing.

```basic
CLASS K
  STATIC DESTRUCTOR
    PRINT 42
  END DESTRUCTOR
END CLASS
END
```

Expected `42`; produces no output on **all four** execution paths (`zanna run`,
`zbasic`, `zanna run --debug-vm`, `ilrun` on emitted IL).

Emitted IL defines `func @K.__dtor(ptr %ME) -> void` containing the `PRINT`, but
grepping the whole module finds **zero call sites** — nothing schedules it at
shutdown.

---

## 3. `STATIC SUB NEW()` never runs

**Severity:** High — same class of gap as #2.

```basic
CLASS A
  STATIC SUB NEW()
    PRINT "static ctor"
  END SUB
END CLASS
PRINT "main"
END
```

Prints only `main`. `docs/languages/basic-grammar.md` states the static
constructor "is invoked by the module initializer before any user code runs".

---

## 4. No working way to compare an object reference to `NOTHING`

**Severity:** High — there is no documented, working null test.

```basic
DIM a AS SomeClass
IF a = NOTHING THEN ...      ' error[B2001]: operand type mismatch
IF a <> NOTHING THEN ...     ' error[B2001]: operand type mismatch
IF a IS NOTHING THEN ...     ' error[B2111]: unknown type 'nothing'
```

Assignment (`a = NOTHING`) works; only comparison fails. The sole functioning
form is `Zanna.Core.Object.RefEquals(a, NOTHING)`, which appears in
`src/tests/fixtures/runtime_sweep/basic/network_tcp_udp.bas` but was not
documented. Either `=`/`<>`/`IS` should accept `NOTHING`, or the runtime helper
should be promoted to the documented idiom.

---

## 5. Zia `Byte` fails IL verification in two common positions

**Severity:** High — `Byte` arithmetic cannot round-trip or be printed.

`Byte` lowers to `i32` and the required widening/narrowing conversions are not
inserted, so IL verification fails.

```zia
// (a) string concatenation
var a: Byte = 3;
Say("v=" + a);
// error[V-IL-VERIFY]: @Zanna.String.Concat parameter 1 expects str but got i32

// (b) the documented `as Byte` round-trip
var a: Byte = 3; var b: Byte = 4;
var sum: Integer = a + b;
// error[V-IL-VERIFY]: store %t7 %t6: operand type mismatch: operand 1 must be i64
```

`docs/languages/zia-reference.md` states that arithmetic on `Byte` operands
"produces `Integer`" and that you "assign back to a `Byte` with an explicit
`as Byte`". Neither survives IL verification.

Working today: `Integer as Byte`, implicit `Byte` → `Integer` widening in a
direct initialization, and `SayInt(byteValue)`.

---

## 6. `zanna check` passes code that `run` and `build` reject

**Severity:** High — `check` is advertised as the gate for editors, scripts, and
AI agents, so a false "clean" is expensive.

```zia
module M;
func start() { var a: Byte = 3; var b: Byte = 4; var r: Integer = a + b; }
```

| Command | Exit |
|---------|------|
| `zanna check gate.zia` | **0** |
| `zanna run gate.zia` | 1 |
| `zanna build gate.zia -o /dev/null` | 1 |
| `zanna check gate.zia -O0` | 2 |
| `zanna check gate.zia --paranoid-verify` | 2 |
| `zanna check gate.zia --verify-each` | 2 |
| `zanna check gate.zia --build-profile debug` | 2 |
| `zanna check gate.zia -O1` / `-O2` | 0 |

Two causes in `src/tools/zanna/cmd_run.cpp`:

- `check` compiles with `optimizeModule=true`, so the lowering-stage verifier is
  skipped (the optimized path verifies only the final module).
- The check-mode branch re-runs the verifier only `if (!moduleVerified)`, so it
  trusts a stale "already verified" flag from the frontend.

---

## 7. Breakpoint exit code `10` depends on `@main`'s return type

**Severity:** Medium — scripted debugging silently misreports success.

`docs/tools/cli.md` documents exit `10` for "halted at breakpoint with no debug
script". That only happens when the IL module's `@main` returns `i64`:

```text
func @main() -> i64   →  [BREAK] printed, exit 10
func @main() -> void  →  [BREAK] printed, exit 0
```

`zanna build` emits `func @main() -> void` for Zia sources, so every
Zia-built module halted at a breakpoint exits `0` — indistinguishable from a
clean run.

---

## 8. `golden/oop/static_destructor` fixture is orphaned

**Severity:** Medium — this is why #2 went unnoticed.

`src/tests/golden/oop/static_destructor.bas` and its `.out` (expecting `42`)
exist, but `static_destructor` appears in no `CMakeLists.txt` or test
registration. The fixture never runs.

Worth auditing `src/tests/golden/` for other unregistered fixtures.

---

## 9. Static methods resolve only through an instance receiver

**Severity:** Low.

```basic
CLASS C
  STATIC SUB Ping()
    PRINT "ping"
  END SUB
END CLASS
C.Ping()   ' error[B1006]: unknown procedure 'c.ping'
```

`c.Ping()` on an instance works.

---

## 10. `B1006` catalog summary covers only one of its uses

**Severity:** Low.

`zanna explain B1006` reports "Array dimension is invalid", but `B1006` is also
emitted for unknown procedures (`PRINT STR(42)` → "unknown procedure 'str'") and
duplicate same-scope names. Either split the code or broaden the summary.

---

## 11. `foreign func` declaration warns about unused parameters

**Severity:** Low.

```zia
foreign func Factorial(n: Integer) -> Integer
// warning[W001]: Parameter 'n' is declared but never used
```

A foreign declaration has no body by definition, so the unused-parameter check
should not apply to it.


---

## 12. Zia `Byte` is 32-bit, not 8-bit

**Severity:** High — `Byte` does not hold a byte, and the documented checked
narrowing neither narrows nor traps.

`docs/languages/zia-reference.md` describes `Byte` as an "8-bit value" and says
"`Integer as Byte` is a checked narrowing that traps on overflow".

```zia
var v: List[Integer] = [255, 256, 300, -1];
for x in v {
    var b: Byte = x as Byte;
    SayInt(b);
}
```

| Input | Documented | Actual |
|-------|-----------|--------|
| `255` | `255` | `255` |
| `256` | trap | **`256`** |
| `300` | trap | **`300`** |
| `-1`  | trap | **`4294967295`** |

The emitted IL narrows to `i32`, not `i8`:

```text
%t2:i32 = cast.si_narrow.chk 300
store i32, %t5, %t2
```

So `Byte` is IL `i32` throughout: `as Byte` performs a 32-bit checked narrowing
(which almost never trips), and widening back zero-extends the 32-bit value —
hence `-1 as Byte` reading back as `4294967295`.

Related to defect #5: the same `i32` representation is what breaks string
concatenation and `Integer`-typed arithmetic results.


---

## 13. A variable named `map` or `set` breaks `for ... in`

**Severity:** Medium — bites anyone who names a variable after its type, and the
Zia reference's own for-in example does exactly that.

```zia
var map: Map[String, Integer] = new Map[String, Integer]();
map.set("a", 1);
for key in map { Say(key); }
// error[V-ZIA-PARSE-EXPECTED]: expected :, got ;
```

```zia
var set: Set[Integer] = {1};
for v in set { SayInt(v); }
// error[V-ZIA-PARSE-EXPECTED]: expected }, got ;
```

`map {` and `set {` are the explicit empty-map / empty-set literal forms, so when
a bare identifier `map` or `set` is immediately followed by `{`, the parser
commits to a collection literal and then fails on the loop body.

Not affected: a variable named `list` (there is no `list {}` form), and `map` used
in a position where `{` does not immediately follow the identifier — `while
map.count() > 0 { }` and `if map.count() == 0 { }` both parse fine.

The parser should prefer the loop body when it is parsing a `for ... in` iterable.


---

## 14. `Error.type` accessor cannot be written

**Severity:** Low.

`docs/languages/zia-reference.md` documents the catch binding as exposing
"`kind` / `type`: runtime error kind name".

```zia
try { ... } catch(e) { Say(e.type); }
// error[V-ZIA-PARSE-DECL]: expected field name after '.'
```

`type` is a reserved word (type-alias declarations), so the parser rejects it as
a member name. The other five documented accessors all work:

| Accessor | Result for a divide-by-zero |
|----------|-----------------------------|
| `e.kind` | `DivideByZero` |
| `e.type` | **parse error** |
| `e.message` | `Division by zero` |
| `e.code` | `0` |
| `e.line` | `5` |
| `e.location` | `line 5` |

Either allow reserved words as member names after `.`, or drop `type` from the
documented surface.


---

## 15. Static fields are unreadable from inside their own class

**Severity:** High — the Zia reference's own static-member example does not
compile, and the diagnostic is an internal-invariant message.

```zia
class C {
    expose static Integer n;
    expose static func get() -> Integer { return n; }
}
// error[V3000]: Unknown identifier 'n' reached lowering
```

| Access site | Result |
|-------------|--------|
| Bare `n` inside a `static func` of `C` | `V3000: Unknown identifier 'n' reached lowering` |
| Bare `n` inside an instance method of `C` | `V3000: Unknown identifier 'n' reached lowering` |
| Qualified `C.n` inside a method of `C` | `V3000: Unknown identifier 'C' reached lowering` |
| `C.n` from outside the class | **works** |

So a static field is writable and readable only from outside the declaring type —
the opposite of the usual encapsulation expectation, and it makes the documented
instance-counter idiom impossible:

```zia
class Counter {
    expose static Integer instanceCount;
    static func getCount() -> Integer { return instanceCount; }   // fails
    func init() { instanceCount = instanceCount + 1; }            // fails
}
```

Separately, `V3000 ... reached lowering` reads like an internal invariant
assertion; a user-facing resolution failure should be reported by semantic
analysis with a normal diagnostic code.


---

## 16. `ME` is accepted inside a `STATIC SUB`

**Severity:** Low.

`docs/languages/basic-reference.md` states "Static methods do not receive `ME`;
referencing `ME` in a static method is a semantic error". It is not diagnosed:

```basic
CLASS C
  X AS INTEGER
  STATIC SUB Ping()
    PRINT ME.X
  END SUB
END CLASS
DIM c AS C = NEW C()
c.Ping()      ' prints 0
```

The program compiles and runs, printing `0` for `ME.X`.


---

## 17. Fields on a class inside a `NAMESPACE` are inaccessible

**Severity:** High — namespaced classes cannot carry data.

```basic
NAMESPACE App.Types
  CLASS Widget
    PUBLIC N AS INTEGER
  END CLASS
END NAMESPACE

DIM w AS App.Types.Widget
w = NEW App.Types.Widget()
w.N = 5
' error[E_PROP_NO_SUCH_PROPERTY]: no such property 'N' on 'APP.TYPES.WIDGET'
```

The identical class declared outside a namespace works. The field is also
invisible from *inside* the class's own methods:

```basic
NAMESPACE App.Types
  CLASS Widget
    PUBLIC N AS INTEGER
    PUBLIC SUB SetN(v AS INTEGER)
      ME.N = v      ' same E_PROP_NO_SUCH_PROPERTY
    END SUB
  END CLASS
END NAMESPACE
```

Field registration appears not to run for namespace-qualified class names. This
is independent of defect #21 — the field is still invisible when the class
declares an explicit `SUB NEW()`.

---

## 18. `USING` inside a `NAMESPACE` block is accepted but does not work

**Severity:** Medium.

`SemanticAnalyzer_Namespace.cpp` rejects a scoped `USING` with `E_NS_008` only
when runtime namespaces are disabled:

```cpp
// Reject USING inside namespace blocks (E_NS_008) unless runtime namespaces
// are enabled (Phase 2 semantics permit scoped USING).
```

Runtime namespaces are on by default, so the directive is accepted — but the
import has no effect and lowering fails:

```basic
NAMESPACE App
  USING Zanna.Terminal
  SUB Main()
    PrintI64(42)
  END SUB
END NAMESPACE
App.Main()
' error[V-IL-VERIFY]: invalid IL after BASIC lowering: unknown callee @printi64
```

With `--no-runtime-namespaces` the same program correctly reports `E_NS_008`.
Scoped `USING` should either work or be rejected in the default configuration.

---

## 19. Aliased `USING X = Ns` does not resolve procedure calls

**Severity:** Medium.

```basic
USING U = App.Utils
NAMESPACE App.Utils
  SUB Hello()
    Zanna.Terminal.PrintStr("hi")
  END SUB
END NAMESPACE
U.Hello()
' error[B1001]: unknown variable 'U'
```

The same alias *does* resolve type references — `DIM w AS U.Widget` and
`NEW U.Widget()` both bind to `APP.TYPES.WIDGET`. The unaliased form
(`USING App.Utils` then `Hello()`) also works. Only alias-qualified call
expressions fail.


---

## 20. False-positive `B3001: index out of bounds` at the top of an array

**Severity:** Medium — a correct program is flagged.

`DIM A(n)` allocates the **inclusive** range `0..n`. `UBOUND` agrees, and both a
literal and a variable index of `n` work at runtime. But the compile-time bounds
check treats the array as having `n` elements, so a *literal* top index warns:

```basic
DIM A(3) AS INTEGER
A(3) = 99                       ' warning[B3001]: index out of bounds
PRINT "literal index 3 ok:"; A(3)   ' ...yet prints 99
```

```basic
DIM A(3) AS INTEGER
DIM I AS INTEGER
I = 3
A(I) = 99                       ' no warning, works
PRINT UBOUND(A)                 ' 3
```

Only the literal-index path is affected; index `n+1` correctly traps at runtime.
The compile-time check is off by one relative to the allocator and `UBOUND`.

(Both `docs/languages/basic-reference.md` and `docs/tutorials/basic-tutorial.md`
also described the bound as exclusive; corrected.)


---

## 21. A namespaced class gets no implicit default constructor

**Severity:** High.

```basic
NAMESPACE Graphics.Rendering
  CLASS Renderer
    WIDTH AS I64
  END CLASS
END NAMESPACE

DIM R AS Graphics.Rendering.Renderer
R = NEW Graphics.Rendering.Renderer()
' error[V-IL-VERIFY]: invalid IL after BASIC lowering:
'   call %t1: unknown callee @GRAPHICS.RENDERING.RENDERER.__ctor
```

A class outside a namespace with no `SUB NEW` constructs fine, so the implicit
`__ctor` is simply not emitted for namespace-qualified classes. Declaring an
explicit `SUB NEW()` works around it:

```basic
NAMESPACE G
  CLASS R
    PUBLIC SUB NEW()
    END SUB
    PUBLIC SUB Hello()
      Zanna.Terminal.PrintStr("hi")
    END SUB
  END CLASS
END NAMESPACE
DIM r AS G.R
r = NEW G.R()
r.Hello()      ' prints "hi"
```

Together with defect #17 (fields invisible), a namespaced `CLASS` is usable only
as a method-only type with a hand-written constructor.


---

## 22. `RESUME` is unimplemented — all three forms lower to `trap`

**Severity:** High — `ON ERROR` / `RESUME` is a headline BASIC feature and is
documented in both the reference and the tutorial.

```basic
ON ERROR GOTO H
PRINT "before"
OPEN "nope.txt" FOR INPUT AS #1
PRINT "after-open"
PRINT "done"
END
H:
PRINT "handled"
RESUME NEXT
```

Expected (per the docs): `before / handled / after-open / done`.
Actual: `before / handled` — the program ends inside the handler.

`RESUME <label>` behaves the same way. The repo's own golden fixture
`src/tests/golden/eh_lowering/resume_forms.bas` shows why — every form lowers to
a bare `trap`:

```text
L100:
  .loc 1 5 5
  trap          ; RESUME
L200:
  .loc 1 7 5
  trap          ; RESUME NEXT
L300:
  .loc 1 9 5
  trap          ; RESUME 400
```

The e2e fixture `src/tests/e2e/basic/errors_io.bas` encodes the non-resuming
behaviour (its expected output is just `caught`, and the line after the failing
`OPEN` is literally commented "this should not print"), so the test suite locks
in the stub rather than flagging it.

Documented behaviour to restore:

- `RESUME` — retry the statement that caused the error
- `RESUME NEXT` — continue at the statement after the one that errored
- `RESUME <label>` — jump to a numeric or named line label


---

## 23. Verifier accepts a parameter-count mismatch between signature and entry block

**Severity:** Medium — `il-verify` reports success on a module that traps.

In Zanna IL, a function's parameters are bound by its **entry block**, not by the
signature. A signature with parameters and a bare `entry:` silently yields a
zero-argument function:

```text
il 0.3.0
extern @Zanna.Terminal.PrintI64(i64) -> void
func @add(i64 %a, i64 %b) -> i64 {
entry:
  %sum = iadd.ovf %a, %b
  ret %sum
}
func @main() -> i64 {
entry:
  %v0 = call @add(2, 3)
  call @Zanna.Terminal.PrintI64(%v0)
  ret 0
}
```

```console
$ il-verify add.il
OK
$ zanna -run add.il
Trap @main:entry#0: InvalidOperation (code=0): argument count mismatch for
function add: expected 0 arguments, received 2
```

The verifier should reject either the signature/entry-block disagreement or the
call-site arity mismatch. Writing `entry(%a:i64, %b:i64):` makes the program run
and print `5`; that is the form `zanna build` itself emits.

(The IL guide's "Locals, params, and calls" quickstart used the non-working form;
corrected.)


---

## 24. Zia lowering emits SSA-invalid IL for a managed local after an early return

**Severity:** High — a straightforward, correct program fails to compile, and the
diagnostic is an internal verifier message.

Minimal reproduction (no runtime dependencies):

```zia
module M;
bind Zanna.Terminal;

func cond() -> Boolean { return true; }

func f() -> List[String] {
    if cond() {
        return [];
    }
    var loaded: List[String] = [];
    try {
        loaded.add("x");
    } catch {
        return [];
    }
    return loaded;
}

func start() { SayInt(f().count()); }
```

```text
error[V-IL-VERIFY]: f:catch_cont_5: %34 = load %t3:
  use of %3 in ^catch_cont_5 not dominated by definition in ^if_end_1
```

`zanna check`, `zanna run`, and `zanna build` all fail.

Required ingredients — removing any one makes it compile:

| Variant | Result |
|---------|--------|
| As above | **fails** |
| Same, but `loaded` is an `Integer` instead of `List[String]` | passes |
| Same, but without the early `return` guard | passes |
| Same, but without the `try`/`catch` | passes |

So the trigger is a **managed/reference-typed local** declared after an early
return, whose slot is then loaded on the `catch` continuation path. The lowerer
places the alloca/definition in a block that does not dominate the continuation.

Found via `docs/book/part2-building-blocks/09-files.md` — its "Note Keeper"
complete example (242 lines) does not compile for this reason.


---

## 25. `return null` from a `String?` function fails IL verification

**Severity:** High — a one-line function using a core language feature does not
compile.

```zia
module M;
func f() -> String? { return null; }
func start() { var r = f(); }
```

```text
error[V-IL-VERIFY]: f:entry_0: ret null:
  ret value type mismatch: expected str but got ptr
```

`String?` is the only optional type affected:

| Return type | `return null;` |
|-------------|----------------|
| `String?`   | **fails** |
| `Integer?`  | works |
| `Number?`   | works |
| `Boolean?`  | works |
| `List[String]?` | works |
| a class type `C?` | works |

Assigning is fine too — `var x: String? = null;` compiles. Only the `ret` path
is wrong: `String` lowers to `str`, the null literal is emitted as `ptr`, and the
return-type check rejects the mismatch instead of materialising a null `str`.

**Workaround:** return through a typed local.

```zia
func f() -> String? { var n: String? = null; return n; }   // compiles
```

Found via `docs/book/part4-applications/22-networking.md` — its `robustFetch`
retry example returns `null` from a `String?` function.


---

## 26. Calling a function through a `&function` reference segfaults

**Severity:** Critical — silent crash (SIGSEGV, exit 139) with no diagnostic, in
a documented core feature.

Minimal reproduction:

```zia
module M;
bind Zanna.Terminal;

func h(n: Integer) { Say("called h"); }

func start() {
    var g: (Integer) -> Void = &h;
    g(1);
}
```

```console
$ zanna run crash.zia
$ echo $?
139        # SIGSEGV — no output, no diagnostic
```

`zanna check` and `zanna build` both succeed; the crash happens at execution.

Scope — every form that **calls through** a `&`-reference crashes:

| Form | Result |
|------|--------|
| `var g: (Integer) -> Void = &h; g(1);` | **segfault** |
| `var g = &h; g(3);` (inferred) | **segfault** |
| `func call(f: (Integer) -> Void) { f(2); }` + `call(&h)` | **segfault** |
| `l.add(&h); var g = l.get(0); g(1);` | **segfault** |
| `l.add(&h); SayInt(l.count());` (stored, never called) | works |
| `var g: (Integer) -> Integer = (n: Integer) => n * 2; g(21);` (lambda) | works — prints 42 |
| `h(1);` (direct call) | works |
| `Thread.Start(&worker, 0)` (runtime callback bridge) | works |

So storing a function reference is fine, lambdas assigned to function-typed
variables are fine, and the runtime's callback bridge is fine. Only Zia's own
direct call through a `&function` value crashes.

`docs/languages/zia-reference.md` documents `&` function references as a core
feature ("The `&` operator explicitly obtains a typed function reference"), so
this blocks callback-style code that does not go through a runtime API.

Found via `docs/book/part5-mastery/28-architecture.md` — the Event Bus /
Publish-Subscribe example stores handlers and invokes them.


---

## 27. BASIC completion provider offers builtins that do not exist

**Severity:** Low — but it actively misleads editors and AI assistants, which is
the whole point of the completion surface.

`src/frontends/basic/BasicCompletion.cpp` advertises 32 builtins. Four of them
are rejected by the compiler:

| Advertised | Compiler result |
|------------|-----------------|
| `HEX$(n) -> STRING` | `error[B1006]: unknown procedure 'HEX$'` |
| `OCT$(n) -> STRING` | `error[B1006]: unknown procedure 'OCT$'` |
| `SPACE$(n) -> STRING` | `error[B1006]: unknown procedure 'SPACE$'` |
| `STRING$(n, ch) -> STRING` | `error[B1006]: unknown procedure 'STRING$'` |

The other 28 all resolve. Either implement these four or drop them from the
completion table.

Hex formatting is currently reachable only through the runtime:
`Zanna.Collections.Bytes.ToHex()` and `Zanna.Graphics.Color.ToHex(i64)`.

(`docs/zannalib/io/advanced.md` used `HEX(...)` in a BASIC example; corrected.)


---

## 28. A trailing label with no following statement is rejected

**Severity:** Low — but the diagnostic exposes an internal synthetic line number.

```basic
PRINT "a"
GOTO Done
Done:
' just a comment
```

```text
error[B1003]: unknown line 1000000
```

The label resolves fine when any executable statement follows it:

| Program tail | Result |
|--------------|--------|
| `Done:` then `END` | OK |
| `Done:` then a comment then `END` | OK |
| `Done:` then only a comment | **B1003: unknown line 1000000** |
| `Done:` at EOF | **B1003: unknown line 1000000** |

Jumping to a trailing label ought to be a no-op that ends the program. At
minimum the message should not surface `1000000`, which is an internal
synthetic line id rather than anything in the user's source.


---

## 29. Runtime doc fragments use non-existent class names

**Severity:** Low — but it propagates into the generated reference, which the
doc-style guide calls the canonical signature reference.

`src/il/runtime/defs/graphics3d/lighting.def` registers the classes under one
name and describes them under another:

```c
/// Create `Zanna.Graphics3D.Physics3DBody` values through its registered constructor and use the
RT_CLASS_BEGIN("Zanna.Graphics3D.PhysicsBody3D", Physics3DBody, "obj", Body3DNew)
```

```c
/// Create `Zanna.Graphics3D.Physics3DWorld` values through its registered constructor and use the
RT_CLASS_BEGIN("Zanna.Graphics3D.PhysicsWorld3D", Physics3DWorld, "obj", World3DNew)
```

The registered (callable) names are `PhysicsBody3D` and `PhysicsWorld3D`; the
`Physics3DBody` / `Physics3DWorld` spellings in the `@details` prose do not
exist and reach users through `docs/generated/runtime/graphics3d.md`
(lines ~1663 and ~1935). A reader following that prose gets
`Unknown runtime namespace: Zanna.Graphics3D.Physics3DBody`.

Fix the prose in the `.def` fragments and regenerate; the generated file must not
be hand-edited.

(`docs/graphics3d-guide.md` had the same wrong name in 19 places; corrected there.)


---

## 30. Cycle collection is off by default, so cycles leak silently

**Severity:** Medium — this is a correctness/robustness gap rather than a crash,
but it is invisible to users.

`rt_gc.c` implements a trial-deletion cycle collector with an allocation-debt
auto-trigger, but the threshold starts disabled:

```c
static int64_t g_gc_threshold = 0;   // 0 = disabled
```

`Zanna.Runtime.GC` exposes `Collect`, `SetThreshold`, `GetThreshold`,
`TrackedCount`, `TotalCollected`, and `PassCount`, so a program *can* opt in —
but nothing does so by default. A program that builds a cyclic object graph
(doubly-linked list, parent/child back-references) leaks it for the process
lifetime with no diagnostic.

Worth considering: a small non-zero default threshold, or a one-line notice at
shutdown when `TrackedCount()` is non-zero and no collection ever ran.

(Two docs described this incorrectly and have been corrected:
`docs/memory-management.md` said collection happens *only* via an explicit
`GC.Collect()` and never mentioned the threshold API;
`docs/languages/lifetime.md` claimed "The GC runs periodically during
allocation", which is false in the default configuration.)


---

## 31. `Channel.Send` does not retain its payload (use-after-free)

**Severity:** Critical — intermittent, data-dependent corruption across threads.

A `Zanna.Threads.Channel` stores a **borrowed** reference to the value it is
given. A boxed temporary passed straight into `Send` is released when the
sending statement ends, so the receiving thread can unbox freed memory.

Reproduction (fails intermittently — roughly 1 run in 5 here):

```zia
module C;
bind Thread = Zanna.Threads.Thread;
bind Channel = Zanna.Threads.Channel;
bind Box = Zanna.Core.Box;
bind Zanna.Terminal as Terminal;

var ch = Channel.New(64);

func producer(arg: Any) {
    for i in 1..=200 { ch.Send(Box.I64(i)); }   // temporary is not retained
    ch.Close();
}

func start() {
    var p = Thread.Start(&producer, 0);
    var total = 0; var n = 0;
    while (!ch.IsClosed || !ch.IsEmpty) {
        var item = ch.Recv();
        if item != null { total = total + Box.ToI64(item); n = n + 1; }
    }
    Thread.Join(p);
    Terminal.SayInt(n); Terminal.SayInt(total);   // want 200 / 20100
}
```

```text
Trap @main:if_then_7#145 (...): DomainError (code=0): rt_unbox_i64: invalid boxed value
```

**Proof it is a lifetime problem, not a race in the loop condition:** keeping a
strong reference alive in the producer makes it pass every time.

```zia
var keepAlive = Seq.New();
...
var b = Box.I64(i);
Seq.Push(keepAlive, b);   // strong reference
ch.Send(b);
```

| Variant | Result |
|---------|--------|
| `ch.Send(Box.I64(i))` | fails intermittently (1/5 runs) |
| strong reference held by the producer | 6/6 runs correct |

`Recv()` itself is well behaved — on a closed, empty channel it correctly
returns `null` (verified separately).

`Channel.Send` should retain the value and release it when it is received or the
channel is cleared, the way `Map.Values` / `Set.Items` / `ToSeq` already use
retained-element mode (see `docs/memory-management.md` §Known Unsoundness #2).

Found via `docs/book/part4-applications/24-concurrency.md`, whose producer/
consumer pipeline example fails ~30% of runs for this reason.
