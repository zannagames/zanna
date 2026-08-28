---
status: active
audience: public
last-verified: 2026-08-28
---

# Zanna REPL

The Zanna REPL (Read-Eval-Print Loop) provides an interactive environment for experimenting with Zia or BASIC code. Each line of input is compiled and executed immediately, with results displayed inline.

## Getting Started

Launch the REPL:

```bash
# Zia REPL (default — only via the unified driver)
zanna repl

# BASIC REPL
zanna repl basic
```

> Note: the standalone `zia` and `zbasic` binaries require source files and do
> **not** launch REPLs. Use `zanna repl` or `zanna repl basic` for interactive
> sessions.

You'll see a prompt where you can type code:

```text
Zanna zia REPL v0.3.0
Type .help for commands, .quit to exit.

zia> 2 + 3
5
zia> "hello" + " world"
hello world
zia> Say("explicit output")
explicit output
```

Expressions are automatically evaluated and printed. Semicolons are automatically appended, so you can type `Say("Hello")` instead of `Say("Hello");`.

## Language Support

The REPL supports both the **Zia** and **BASIC** languages.

### Zia — What Works

- **Expression auto-print**: Expressions are automatically evaluated and printed with type-aware coloring (integers in blue, strings in yellow, booleans in magenta, objects in cyan)
- **Variable persistence**: Variables declared in one input are available in subsequent inputs, including reassignment
- **Statements**: Any Zia statement (function calls, variable declarations, control flow)
- **Function definitions**: Define functions and call them in subsequent inputs
- **Function redefinition**: Redefine a function to update its behavior
- **Bind statements**: Import runtime modules with `bind`
- **Class/struct/interface definitions**: Define types for use in subsequent inputs
- **Type inspection**: Use `.type <expr>` to see the inferred type of any expression
- **Error recovery**: Syntax and type errors are reported without losing session state

### Zia Limitations

- **Collection mutation**: Method calls like `list.add(1)` are not persisted across inputs; use functions to build collection state

### BASIC — What Works

- **Expression auto-print**: Expressions are automatically wrapped with `PRINT`
- **Variable persistence**: Variables declared with `DIM` persist across inputs, including reassignment
- **Statements**: Any BASIC statement (`PRINT`, `DIM`, `IF`/`END IF`, `FOR`/`NEXT`, etc.)
- **SUB/FUNCTION definitions**: Define procedures and call them in subsequent inputs
- **Multi-line blocks**: Block keywords (`IF`/`END IF`, `DO`/`LOOP`, `FOR`/`NEXT`, `WHILE`/`WEND`, `SUB`/`END SUB`, `FUNCTION`/`END FUNCTION`, `SELECT`/`END SELECT`) are tracked for automatic continuation
- **Keyword completion**: Tab completion for BASIC keywords and session variables
- **Error recovery**: Syntax and type errors are reported without losing session state

```text
basic> DIM x AS Integer = 42
basic> x + 8
50
basic> SUB Greet()
...>   PRINT "Hello from SUB"
...> END SUB
basic> Greet()
Hello from SUB
```

### BASIC Limitations

- **No bind system**: BASIC does not support `bind` statements; runtime functions are available directly
- **No `.type` or `.il` commands**: Type probing and IL inspection are Zia-only features
- **Keyword-only completion**: Tab completion is limited to BASIC keywords, session variables, and session procedures (no member access or runtime symbol completion)

## Line Editing

The REPL includes a built-in line editor with the following keybindings:

| Key | Action |
|-----|--------|
| Left/Right | Move cursor |
| Home / Ctrl-A | Move to start of line |
| End / Ctrl-E | Move to end of line |
| Ctrl-Left/Right | Move by word |
| Backspace | Delete character before cursor |
| Delete | Delete character under cursor |
| Ctrl-U | Delete from cursor to start of line |
| Ctrl-K | Delete from cursor to end of line |
| Ctrl-W | Delete word before cursor |
| Ctrl-L | Clear screen |
| Up/Down | Navigate history |
| Tab | Trigger tab completion |
| Ctrl-C | Cancel current input |
| Ctrl-D | Exit the REPL (on empty line) |

## History

The REPL maintains a history of previous inputs. Use the Up and Down arrow keys to navigate through history. Duplicate consecutive entries and empty lines are not added to history.

### Persistent History

History is automatically saved to disk when the REPL exits and loaded on startup. The history file location is:

```text
~/.zanna/repl_history_zia      # Zia REPL history
~/.zanna/repl_history_basic    # BASIC REPL history
```

The `~/.zanna/` directory is created automatically if it doesn't exist. Each language has its own history file, so Zia and BASIC histories don't interfere with each other. The maximum history size is 1000 entries.

Use `.save <file>` to save your session history to a custom file for later reference.

## Tab Completion

Press **Tab** to trigger context-aware completions powered by the Zia `CompletionEngine`. The REPL supports:

- **Keywords**: `var`, `func`, `class`, `if`, `while`, `for`, `return`, etc.
- **Session variables**: Variables you've declared in the current session
- **Session functions**: Functions you've defined in the current session
- **Runtime symbols**: Functions and types from bound runtime namespaces
- **Type names**: After `new` or in type annotations
- **Member access**: After `.`, shows methods and fields of the expression type

```text
zia> var myCounter = 42
zia> myC<Tab>
myCounter
zia> Zanna.Math.<Tab>
Sqrt  AbsInt  AbsNum  ...
```

When there's a single match, it's inserted directly. When there are multiple matches, they're displayed below the prompt for you to choose.

## Multi-Line Input

When you type an input with unclosed brackets or block keywords, the REPL automatically prompts for continuation.

**Zia** — tracks bracket depth (`{`, `(`, `[`):

```text
zia> func greet(name: String) -> String {
...>     return "Hello, " + name;
...> }
zia> Say(greet("World"))
Hello, World
```

**BASIC** — tracks block keywords (`IF`/`END IF`, `FOR`/`NEXT`, `DO`/`LOOP`, `SUB`/`END SUB`, etc.):

```text
basic> FOR i = 1 TO 3
...>   PRINT i
...> NEXT
1
2
3
```

The continuation prompt `...>` indicates that the REPL is waiting for more input to complete the current expression or block. Single-line `IF...THEN <statement>` in BASIC is treated as complete (no `END IF` needed).

## Expression Evaluation

When you type an expression (not a statement like `var` or `if`), the REPL automatically evaluates it and prints the result:

```text
zia> 2 + 3 * 4
14
zia> "Hello, " + "World"
Hello, World
zia> 3.14 * 2.0
6.28
zia> true
true
```

The REPL detects the expression type and formats the output accordingly. You can still use explicit `Say()` calls — they work as normal and are not auto-printed a second time.

Inputs that start with keywords (`var`, `if`, `while`, `for`, `func`, etc.) are treated as statements and executed without auto-printing.

## Variables

Variables persist across inputs. Declare a variable and use it in later inputs:

```text
zia> var x = 42
zia> var y = x + 8
zia> y
50
zia> x = 100
zia> x
100
```

Use `.vars` to see all declared variables with their types:

```text
zia> .vars
  x : Integer
  y : Integer
```

Variable types are automatically inferred from their initializer. Reassignment updates the stored value for subsequent inputs.

## Function Definitions

Define functions and call them across inputs:

```text
zia> func square(x: Integer) -> Integer { return x * x; }
zia> square(7)
49
```

You can redefine a function to update its behavior:

```text
zia> func greet(name: String) -> String { return "Hi, " + name; }
zia> Say(greet("World"))
Hi, World
zia> func greet(name: String) -> String { return "Hey there, " + name; }
zia> Say(greet("World"))
Hey there, World
```

## Bind Statements

Import runtime modules using `bind`:

```text
zia> bind Math = Zanna.Math
zia> Math.Sqrt(16.0)
4
```

The default binds (`Zanna.Terminal`, `Fmt = Zanna.Text.Fmt`, and `Obj = Zanna.Core.Object`) are available automatically.

## Meta-Commands

Meta-commands start with `.` and provide REPL-specific functionality:

| Command | Description |
|---------|-------------|
| `.help` | Show available meta-commands |
| `.quit` | Exit the REPL |
| `.exit` | Exit the REPL (alias for .quit) |
| `.clear` | Reset all session state (functions, types, binds, variables) |
| `.vars` | List session variables with inferred types |
| `.funcs` | List defined functions with signatures |
| `.binds` | List active bind statements |
| `.type <expr>` | Show the inferred type of an expression without evaluating it |
| `.il <expr>` | Show the generated IL (Intermediate Language) for an expression |
| `.time <expr>` | Evaluate an expression and display the execution time |
| `.load <file>` | Load and execute a Zia source file |
| `.save <file>` | Save session history to a file |

### Examples

```text
zia> .help
Available commands:
  .help   Show this help message
  .quit   Exit the REPL
  .exit   Exit the REPL
  .clear  Reset session state
  .vars   List session variables
  .funcs  List defined functions
  .binds  List active bind statements
  .type   Show type of expression
  .il     Show generated IL for expression
  .time   Evaluate and show execution time
  .load   Load and execute a source file
  .save   Save session history to a file

zia> .type 2 + 3
Integer
zia> .type "hello"
String

zia> .il 2 + 3
il 0.3.0
func @main() -> void {
entry_0:
  %t0 = iadd.ovf 2, 3
  %t1 = call @Zanna.Text.Fmt.Int(%t0)
  call @Zanna.Terminal.Say(%t1)
  ...
}

zia> .time 2 + 3
5
Elapsed: 1.234ms

zia> .save session.txt
Saved 5 entries to: session.txt
```

## Signal Handling

- **Ctrl-C during input**: Cancels the current line. If you're in multi-line continuation, cancels all accumulated input and prints "(input cancelled)".
- **Double Ctrl-C on empty line**: Pressing Ctrl-C twice consecutively on an empty prompt exits the REPL. The first Ctrl-C prints "(press Ctrl-C again to exit)" as a hint. Any other input resets the counter.
- **Ctrl-C during execution**: Interrupts a running computation gracefully without killing the REPL.
- **Ctrl-D on empty line**: Exits the REPL immediately.

## Non-Interactive Mode

The REPL can also accept piped input for scripting:

```bash
echo 'Say("Hello")' | zanna repl
```

In non-interactive mode, the REPL reads lines from stdin without a prompt and executes them sequentially. This is useful for testing and automation.

## Colorized Output

When connected to a terminal, the REPL displays colorized output:

- **Prompt**: Bold cyan
- **Continuation prompt**: Cyan
- **Integer/Number results**: Blue
- **String results**: Yellow
- **Boolean results**: Magenta
- **Object results**: Cyan
- **Statement output**: Bold green
- **Errors**: Bold red
- **Warnings**: Bold yellow

Expression auto-print results are colored based on their detected type. Explicit `Say()` output uses the default green color.

Colors are automatically disabled when output is piped or redirected.

---

## Architecture (Developer Reference)

> This section is for contributors. Users can safely skip it.

The REPL compiles each input to a fresh IL (Intermediate Language) module and executes it via the BytecodeVM. Both language adapters share the same core session loop (`ReplSession`), line editor, meta-command registry, and pretty printer.

### Zia Pipeline

1. Build synthetic Zia source (binds + types + functions + replayed variables + `func start() { input }`)
2. Compile via the Zia frontend
3. Verify the IL
4. Execute via BytecodeVM

**Expression auto-print** works by compile-time type probing: the REPL tries wrapping the input with different formatters (`Fmt.Bool`, `Fmt.Int`, `Fmt.Num`, bare `Say`) and picks the first that compiles successfully. This leverages the Zia type system itself to determine the expression type.

**Variable persistence** works by replaying previous variable declarations and assignments at the top of each `start()` function body.

**Tab completion** uses the Zia `CompletionEngine` (the same engine that powers IDE IntelliSense). The REPL builds a synthetic source file from accumulated session state and the current input, then calls `CompletionEngine::complete()` with the cursor position in that source. Results are supplemented with session-local variables and functions (which are local to `start()` and thus not visible to the engine's global symbol provider).

### BASIC Pipeline

1. Build synthetic BASIC source (SUB/FUNCTION definitions + DIM declarations + replayed assignments + current input)
2. Compile via the BASIC frontend
3. Verify the IL
4. Execute via BytecodeVM

**Expression auto-print** wraps the input with `PRINT <expr>` and checks if it compiles. If it does, the wrapped version is executed.

**Variable persistence** works by replaying `DIM` declarations and assignments as top-level statements before the current input.

**Multi-line detection** tracks block keyword depth (case-insensitive): openers (`IF...THEN`, `FOR`, `DO`, `WHILE`, `SUB`, `FUNCTION`, `SELECT`, `CLASS`, `TYPE`, `TRY`, `PROPERTY`, `NAMESPACE`) increment depth, closers (`END IF`, `NEXT`, `LOOP`, `WEND`, `END SUB`, etc.) decrement depth. Single-line `IF...THEN <statement>` is detected as complete.

**Tab completion** matches BASIC keywords and session variables/procedures using case-insensitive prefix matching.
