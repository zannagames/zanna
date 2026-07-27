---
status: active
audience: public
last-verified: 2026-07-26
---

# System

> System interaction, environment, and process execution.

**Part of the [Zanna Runtime Library](README.md)**

## Contents

- [Zanna.System.Environment](#zannasystemenvironment)
- [Zanna.System.Clipboard](#zannasystemclipboard)
- [Zanna.System.Shutdown](#zannasystemshutdown)
- [Zanna.System.Exec](#zannasystemexec)
- [Zanna.System.Process](#zannasystemprocess)
- [Zanna.System.Pty](#zannasystempty)
- [Zanna.System.Machine](#zannasystemmachine)
- [Zanna.Runtime.Unsafe](#zannaruntimeunsafe)
- [Zanna.Runtime.GC](#zannaruntimegc)
- [Zanna.Memory.WeakRef](#zannamemoryweakref)
- [Zanna.Terminal](#zannaterminal)

---

## Zanna.System.Environment

Command-line arguments and environment access.

**Type:** Static utility class

### Methods

| Method                     | Signature              | Description                                                             |
|----------------------------|------------------------|-------------------------------------------------------------------------|
| `GetArgumentCount()`       | `Integer()`            | Returns the number of program arguments                                 |
| `GetArgument(index)`       | `String(Integer)`      | Returns the program argument at the specified zero-based index           |
| `GetCommandLine()`         | `String()`             | Returns the program arguments joined as a single string                  |
| `GetVariable(name)`        | `String(String)`       | Returns the value of an environment variable, or `""` when missing      |
| `HasVariable(name)`        | `Boolean(String)`      | Returns `TRUE` when the environment variable exists                     |
| `IsNative()`               | `Boolean()`            | Returns `TRUE` when running native code, `FALSE` when running in the VM |
| `SetVariable(name, value)` | `Void(String, String)` | Sets or overwrites an environment variable (empty value allowed)        |
| `Exit(code)`         | `Void(Integer)`        | Terminates the program with the provided exit code                      |

### Behavior Notes

- Tool runners expose only program arguments after `--`, so index 0 is the first user argument.
  A native legacy context that was not initialized by a runner can instead import the host
  process's complete `argv`, including its executable name. `GetArgument` traps for a negative or
  out-of-range index.
- `GetCommandLine` is a lossy display form: it joins stored arguments with one space and adds no
  quoting or escaping, so argument boundaries cannot always be recovered.
- A missing variable and a present variable whose value is empty both make `GetVariable` return
  `""`; use `HasVariable` to distinguish them. Empty names and embedded NUL bytes trap.
  `SetVariable` changes the current process environment, which normally becomes the inherited
  environment of subsequently launched children.
- Host facts (OS, version, user, home directory, core count) live on `Zanna.System.Machine`
  queries documented below. `Cwd` is process-wide and traps if the current directory cannot be
  determined.
- `Exit` does not return. The host operating system's exit-status width and normalization
  still apply to the supplied 64-bit value.

### Zia Example

```zia
module EnvDemo;

bind Zanna.Terminal;
bind Zanna.System.Environment as Env;
bind Zanna.Text.Fmt as Fmt;

func start() {
    Say("Args: " + Fmt.Int(Env.GetArgumentCount()));
    Say("Native: " + Fmt.Bool(Env.IsNative()));

    // Environment variables
    var home = Env.GetVariable("HOME");
    Say("HOME: " + home);
    Say("Has HOME: " + Fmt.Bool(Env.HasVariable("HOME")));
}
```

### BASIC Example

```basic
' Program invoked as: zanna run app.bas -- arg1 arg2 arg3

DIM count AS INTEGER
count = Zanna.System.Environment.GetArgumentCount()
PRINT "Argument count: "; count  ' Output: 3

FOR i = 0 TO count - 1
    PRINT "Arg "; i; ": "; Zanna.System.Environment.GetArgument(i)
NEXT i
' Output:
' Arg 0: arg1
' Arg 1: arg2
' Arg 2: arg3

PRINT "Full command: "; Zanna.System.Environment.GetCommandLine()

' Arguments before -- belong to the zanna tool. GetArgument(0) is the first
' argument after --, not the tool or executable name.

' Environment variables
DIM name AS STRING
DIM value AS STRING
name = "ZANNA_SAMPLE_ENV"
value = Zanna.System.Environment.GetVariable(name)
IF Zanna.System.Environment.HasVariable(name) THEN
    PRINT name; " is set to "; value
ELSE
    PRINT name; " is not set"
END IF

Zanna.System.Environment.SetVariable(name, "abc")
PRINT "Updated value: "; Zanna.System.Environment.GetVariable(name)

' Values round-trip as UTF-8 across platforms.
Zanna.System.Environment.SetVariable("ZANNA_UTF8_SAMPLE", "café")
PRINT Zanna.System.Environment.GetVariable("ZANNA_UTF8_SAMPLE")

' Process exit
' Zanna.System.Environment.Exit(7)
```

---

## Zanna.System.Clipboard

UTF-8 text clipboard access backed by the active desktop clipboard backend.

**Type:** Static utility class

### Methods

| Method      | Signature      | Description                                      |
|-------------|----------------|--------------------------------------------------|
| `Get()`     | `String()`     | Returns clipboard text, or `""` when unavailable |
| `HasText()` | `Boolean()`    | Returns `TRUE` when text is available            |
| `Set(text)` | `Void(String)` | Copies UTF-8 text to the system clipboard        |

### Zia Example

```zia
module ClipboardDemo;

bind Zanna.System.Clipboard as Clipboard;

func start() {
    Clipboard.Set("Copied from Zanna");
    if (Clipboard.HasText()) {
        var text = Clipboard.Get();
    }
}
```

### BASIC Example

```basic
Zanna.System.Clipboard.Set("Copied from Zanna")
IF Zanna.System.Clipboard.HasText() THEN
    PRINT Zanna.System.Clipboard.Get()
END IF
```

The clipboard helpers are available on desktop graphics backends and must be called from the GUI
main thread there. In headless or graphics-disabled builds, `Get()` returns an empty string,
`HasText()` returns `FALSE`, and `Set()` is a no-op. Embedded NUL bytes in text passed to `Set` are
replaced with U+FFFD by the GUI-string bridge rather than truncating the suffix.

---

## Zanna.System.Shutdown

Poll-based graceful shutdown requests for long-running servers, games, and tools.

**Type:** Static utility class

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `None` | `0` | No shutdown request is pending |
| `Interrupt` | `1` | VM Ctrl-C / interrupt, or a cooperative interrupt request |
| `Terminate` | `2` | VM `SIGTERM` / Windows close event, or a cooperative terminate request |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Request(reason)` | `Void(Integer)` | Publish one or more shutdown reason bits |
| `Poll()` | `Integer()` | Return and clear pending reason bits |
| `IsPending()` | `Boolean()` | Return `TRUE` when any reason is pending without clearing it |
| `Clear()` | `Void()` | Clear pending reasons and the VM interrupt epoch |
| `InstallSignalHandlers()` | `Void()` | Install OS signal/console handlers that publish shutdown requests (opt-in for native programs) |

`Poll()` and `IsPending()` arm graceful handling for the next VM interrupt. If Ctrl-C
arrives after a loop has polled, the VM records `INTERRUPT` and lets the program reach
its next `Shutdown.Poll()` call. If a program never polls, Ctrl-C still raises the
normal `Interrupt` trap. This is deliberately poll-based; signal and console handlers
only publish atomic state and never run managed callbacks.

Pending reasons are a process-wide atomic bitmask, not per-VM state. `Request` ignores bits other
than `Interrupt` and `Terminate` and ORs repeated requests together; `Poll` returns and clears the
accumulated mask. The VM wires automatic Ctrl-C, `SIGTERM`, and Windows console-event publication
for you. A native/AOT program calls `InstallSignalHandlers()` once at startup to opt in to the same
OS integration — it is not automatic there, because the compiler and other tools also link the
runtime and must keep the default signal behavior. After that call, Ctrl-C / termination are
published through `Request` just like under the VM, so `Poll` observes `Interrupt` / `Terminate`.

### Zia Example

```zia
module GracefulServer;

bind Zanna.System.Shutdown as Shutdown;
bind Zanna.Terminal;

func start() {
    Shutdown.Request(Shutdown.Interrupt);
    var running = true;
    while running {
        var reason = Shutdown.Poll();
        if reason != Shutdown.None {
            Say("shutdown requested");
            running = false;
        }
    }
}
```

### BASIC Example

```basic
DO
    reason = Zanna.System.Shutdown.Poll()
    IF reason <> Zanna.System.Shutdown.None THEN
        PRINT "shutdown requested"
        EXIT DO
    END IF

    ' frame/update work
LOOP
```

---

## Zanna.System.Exec

External command execution for running system commands and capturing output.

**Type:** Static utility class

### Methods

| Method                       | Signature              | Description                                            |
|------------------------------|------------------------|--------------------------------------------------------|
| `Run(program)`               | `Integer(String)`      | Execute program, wait for completion, return exit code |
| `RunArgs(program, args)`     | `Integer(String, Seq)` | Execute program with arguments, return exit code       |
| `Capture(program)`           | `String(String)`       | Execute program, capture and return stdout             |
| `CaptureArgs(program, args)` | `String(String, Seq)`  | Execute program with arguments, capture stdout         |
| `Shell(command)`             | `Integer(String)`      | Run command through system shell, return exit code     |
| `ShellCapture(command)`      | `String(String)`       | Run command through shell, capture stdout              |
| `ShellResult(command)`       | `CommandResult(String)`| Run command through shell and return captured stdout plus exit code |

### Zanna.System.CommandResult

Returned by `Exec.ShellResult(command)`.

| Property | Type | Description |
|----------|------|-------------|
| `Output` | String | Captured stdout from the shell command |
| `ExitCode` | Integer | Normalized shell command exit code, or `-1` for launch/signalled failure |
| `IsSuccess` | Boolean | True when `ExitCode == 0` |

`ShellResult()` is the shell-capture entry point that reports both stdout and the exit code.
`ShellCapture()` returns stdout only; `Shell()` returns the exit code only.

### Security Warning

**Shell injection vulnerability:** The `Shell` and `ShellCapture` methods pass commands directly to the system shell (
`/bin/sh -c` on Unix, `cmd /c` on Windows). Never pass unsanitized user input to these functions. If you need to run a
command with user-provided data, use `RunArgs` or `CaptureArgs` instead, which safely handle arguments without shell
interpretation.

```basic
' DANGEROUS - shell injection risk:
userInput = "file.txt; rm -rf /"
Zanna.System.Exec.Shell("cat " + userInput)  ' DO NOT DO THIS

' SAFE - use RunArgs with explicit arguments:
DIM args AS OBJECT = Zanna.Collections.Seq.New()
args.Push(userInput)
Zanna.System.Exec.RunArgs("/bin/cat", args)  ' Arguments are passed directly
```

### Zia Example

The commands in the following Zia and BASIC examples are POSIX examples. On Windows, use commands
and executable paths available on that host.

```zia
module ExecDemo;

bind Zanna.Terminal;
bind Zanna.System.Exec as Exec;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Capture shell command output and exit status together
    var result = Exec.ShellResult("echo Hello from shell");
    Say("Shell: " + result.Output);
    Say("Exit code: " + Fmt.Int(result.ExitCode));

    // Run a command and get exit code
    var code = Exec.Shell("true");
    Say("Exit code: " + Fmt.Int(code));

    // Capture program output directly
    var captured = Exec.Capture("/bin/echo");
    Say("Echo: " + captured);
}
```

### BASIC Example

```basic
' Simple command execution
DIM exitCode AS INTEGER
exitCode = Zanna.System.Exec.Shell("echo Hello World")
PRINT "Exit code: "; exitCode

' Capture command output
DIM shellResult AS OBJECT
shellResult = Zanna.System.Exec.ShellResult("date")
PRINT "Current date: "; Zanna.System.CommandResult.get_Output(shellResult)
PRINT "Exit code: "; Zanna.System.CommandResult.get_ExitCode(shellResult)

' Execute program with arguments
DIM args AS OBJECT = Zanna.Collections.Seq.New()
args.Push("-l")
args.Push("-a")
exitCode = Zanna.System.Exec.RunArgs("/bin/ls", args)

' Capture output with arguments
DIM result AS STRING
args = Zanna.Collections.Seq.New()
args.Push("--version")
result = Zanna.System.Exec.CaptureArgs("/usr/bin/python3", args)
PRINT "Python version: "; result

' Check if command succeeded
IF Zanna.System.Exec.Shell("test -f /etc/passwd") = 0 THEN
    PRINT "File exists"
ELSE
    PRINT "File does not exist"
END IF

' Run a script and check result
exitCode = Zanna.System.Exec.Shell("./myscript.sh")
IF exitCode <> 0 THEN
    PRINT "Script failed with exit code: "; exitCode
END IF
```

### Platform Notes

- **Run/RunArgs/Capture/CaptureArgs**: Execute programs directly, wait for completion, and pass
  arguments without shell interpretation. Unix uses `posix_spawnp` (including `PATH` lookup);
  Windows uses `CreateProcessW`.
- **Shell/ShellCapture**: Use `/bin/sh -c` on Unix or `cmd /c` on Windows. Commands are interpreted by the shell.
- Direct execution returns a negative signal number when a Unix child is signalled and `-1` on
  launch/wait failure. Shell execution normalizes either launch failure or signal termination to
  `-1`. Windows returns the child or command processor's unsigned 32-bit exit code as an Integer.
- Capture methods collect stdout only. They return `""` both when a program produces no output and
  when it cannot start, so those cases are indistinguishable. Captured output is capped at 16 MiB;
  reaching the cap traps instead of returning a silent prefix.
- Program, command, and direct-execution argument strings reject embedded NUL bytes. An empty
  direct program name traps; an empty shell command succeeds with no output.

---

## Zanna.System.Process

Streaming child-process control for tools, build jobs, and long-running IDE tasks.

**Type:** Static utility class returning `Zanna.System.Process.ProcessHandle` objects

### Static Methods

| Method                         | Signature                  | Description                                             |
|--------------------------------|----------------------------|---------------------------------------------------------|
| `Start(program, args)`         | `ProcessHandle(String, Seq)` | Start a process with inherited cwd and environment   |
| `StartIn(program, args, cwd)`  | `ProcessHandle(String, Seq, String)` | Start a process in a working directory        |
| `StartWithEnv(program, args, cwd, env)` | `ProcessHandle(String, Seq, String, Seq)` | Start with explicit cwd and environment |

`args` is a `Seq` of argument strings and does not include the program name. `env` is either `NULL` to inherit the
current environment or a `Seq` of `KEY=value` strings. `cwd` may be `NULL` or empty to inherit the current working
directory.

`StartWithEnv` replaces the child's entire environment; it does not overlay the listed entries on
the parent environment. On POSIX, `Start*` PATH-searches a bare program name (using the supplied environment's `PATH` when
present, else the inherited one), so a bare name resolves the same way with or without an explicit
environment. Startup/OS failures return `NULL`;
invalid empty/NUL-containing inputs and malformed environment entries trap.

On Windows, `StartWithEnv` converts entries to a sorted UTF-16 environment block and launches the
child with `CREATE_UNICODE_ENVIRONMENT`, so non-ASCII values are delivered intact. Variable names
are case-insensitive; duplicate names are rejected rather than allowing an ambiguous child
environment.

### Zanna.System.Process.ProcessHandle

| Method         | Signature    | Description                                                  |
|----------------|--------------|--------------------------------------------------------------|
| `IsValid()`    | `Boolean()`  | Returns `TRUE` while the handle still owns process resources |
| `Poll()`       | `Boolean()`  | Polls process state; returns `TRUE` while still running      |
| `IsRunning()`  | `Boolean()`  | Alias for polling the current running state                  |
| `ReadStdout()` | `String()`   | Returns buffered stdout bytes available now, then clears them |
| `ReadStderr()` | `String()`   | Returns buffered stderr bytes available now, then clears them |
| `ReadStdoutResult()` | `Map()` | Returns `text` and `truncated` for buffered stdout |
| `ReadStderrResult()` | `Map()` | Returns `text` and `truncated` for buffered stderr |
| `WriteStdin(data)` | `Integer(String)` | Write bytes to stdin; return bytes accepted, or `-1` on failure |
| `ExitCode()`   | `Integer()`  | Returns exit code; `-1` also represents running/invalid state |
| `Kill()`       | `Boolean()`  | Requests process termination                                |
| `Wait()`       | `Integer()`  | Blocks until process exit and returns the exit code          |
| `Destroy()`    | `Void()`     | Closes process resources; terminates a still-running child   |

### Zia Example

The command below is a POSIX example. On Windows, use `cmd.exe` with arguments such as `/c` and a Windows command
string, or start the desired executable directly without a shell.

```zia
module ProcessDemo;

bind Zanna.Collections.Seq as Seq;
bind Zanna.System.Process as Process;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var args = Seq.New();
    Seq.Push(args, "-c");
    Seq.Push(args, "echo build-start; echo warning >&2; exit 7");

    var proc = Process.Start("/bin/sh", args);
    while proc.IsRunning() {
        var out = proc.ReadStdout();
        var err = proc.ReadStderr();
        if Zanna.String.get_Length(out) > 0 { Say(out); }
        if Zanna.String.get_Length(err) > 0 { Say(err); }
    }

    Say(proc.ReadStdout());
    Say(proc.ReadStderr());
    Say("Exit: " + Fmt.Int(proc.Wait()));
    proc.Destroy();
}
```

### Platform Notes

- `Start*` executes the program directly; it does not invoke a shell. Use an explicit shell executable only when shell
  syntax is required.
- A spawn failure returns `NULL`. Process resolves the program to a full path (PATH-searching bare
  names) before `posix_spawn`, so an unresolvable name fails at start rather than producing a live
  handle with code `126`/`127`.
- `ReadStdout()` and `ReadStderr()` are non-blocking incremental reads intended for simple callers. They trap when the runtime had to truncate unread stream data.
- `ReadStdoutResult()` and `ReadStderrResult()` are preferred for long-running tools and GUI frame loops. They return a map with `text` and `truncated` so callers can surface overflow without crashing.
- `WriteStdin` uses non-blocking pipes on POSIX and may return a partial byte count; callers must
  retry the remaining suffix. Null/empty data returns 0.
- `ExitCode()` and `Wait()` return a negative signal number on POSIX. Consequently `-1` is
  ambiguous: it can mean running/invalid or a child terminated by signal 1.
- `Kill()` sends a termination request (`SIGTERM` on POSIX, `TerminateProcess` on Windows) without
  waiting for exit. `Destroy()` is idempotent; POSIX allows 500 ms after `SIGTERM` before
  `SIGKILL`, while Windows terminates a live child immediately.
- Output buffers are capped at 16 MB per stream between reads.

---

## Zanna.System.Pty

Pseudo-terminal-backed child-process control for interactive shells and
terminal-like IDE surfaces.

**Type:** Static factory class plus PTY session handle object

### Static Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Open(program, args, cwd, env, cols, rows)` | `PtySession(String, Object, String, Object, Integer, Integer)` | Open a PTY-backed child |
| `OpenResult(program, args, cwd, env, cols, rows)` | `Result(String, Object, String, Object, Integer, Integer)` | Open a PTY-backed child as `Ok(PtySession)` or `Err(message)` |
| `IsSupported()` | `Boolean()` | Returns `TRUE` when the current platform can create PTYs |

### Zanna.System.Pty.PtySession

| Method | Signature | Description |
|--------|-----------|-------------|
| `IsValid()` | `Boolean()` | Returns `TRUE` while the handle owns PTY resources |
| `Poll()` | `Boolean()` | Polls child state; returns `TRUE` while still running |
| `IsRunning()` | `Boolean()` | Alias for polling the current running state |
| `Read()` | `String()` | Returns merged stdout/stderr terminal bytes available now |
| `ReadResult()` | `Map()` | Returns `text` and `truncated` for merged terminal output |
| `Write(data)` | `Integer(String)` | Writes bytes to terminal input |
| `Resize(cols, rows)` | `Boolean(Integer, Integer)` | Updates terminal size |
| `ExitCode()` | `Integer()` | Returns exit code, or `-1` while running/invalid |
| `Kill()` | `Boolean()` | Requests child termination |
| `Wait()` | `Integer()` | Blocks until child exit and returns the exit code |
| `Destroy()` | `Void()` | Closes PTY resources; terminates a still-running child |

Prefer `OpenResult()` for production code. It returns `Err(message)` for
unsupported platforms, startup failures, and validation errors. `Open()` and
`LastError()` remain available for compatibility; existing callers can keep
checking the nullable `Open()` result.

Prefer `ReadResult()` for IDEs and other long-running terminal surfaces. The
legacy `Read()` method returns a string directly and traps if the runtime had to
drop unread PTY output; `ReadResult()` reports that condition through the
`truncated` flag.

`args`, `cwd`, and `env` follow the Process conventions above; a non-null `env` replaces the full
child environment. On POSIX, both the inherited- and explicit-environment paths `PATH`-search bare program names via
`execvp` (the explicit environment's `PATH` is used), matching `Process.Start*`. A program that
fails to exec surfaces as a failed open (`Err` / `NULL`) rather than an apparently-valid session
that only fails later.
Initial and resized dimensions default to 80 columns by 24 rows when non-positive and clamp each
dimension to 4096.

`Open` returns `NULL` for a null program and ordinary startup/unsupported-platform failures. A
non-null empty or NUL-containing program/cwd, malformed sequence entry, or allocation failure can
trap. `OpenResult` converts both forms to `Err(String)` and is the preferred API. In Zia, annotate
the extracted payload because `Result.Unwrap()` has the generic `Object` return type:

```zia
var opened = Pty.OpenResult(program, args, cwd, env, 80, 24);
if opened.IsOk {
    var session: Zanna.System.Pty.PtySession = opened.Unwrap();
    // use session, then call session.Destroy()
}
```

On macOS and Linux the backend uses a controlling POSIX PTY; Windows requires ConPTY (Windows 10
version 1809 or newer). `LastError` is a compatibility side channel stored
in a thread-local buffer, so concurrent PTY calls on different threads keep independent diagnostics;
read it on the same thread that performed the failing operation (prefer `OpenResult` for structured
errors). `Read`/`ReadResult` share one merged
stdout/stderr buffer capped at 16 MiB. `Write` can report a partial byte count, and POSIX exit codes
use the same negative-signal convention as Process. `Destroy` uses the same POSIX 500 ms
termination grace period.

`Resize` returns `TRUE` only when the backend actually applied the new size (`TIOCSWINSZ` on POSIX,
`ResizePseudoConsole` on Windows). A backend failure returns `FALSE` and records the OS diagnostic
in `LastError`, so it is a real confirmation of the OS resize rather than an accepted-request flag.

---

## Zanna.System.Machine

System information queries providing read-only access to machine properties.

**Type:** Static utility class

### Properties

| Property   | Type      | Description                                                              |
|------------|-----------|--------------------------------------------------------------------------|
| `Os`       | `String`  | `"linux"`, `"macos"`, `"windows"`, or `"unknown"`                        |
| `OsVersion` | `String` | Operating system version string (e.g., `"14.2.1"` on macOS)              |
| `Arch`     | `String`  | CPU architecture identifier                                              |
| `Host`     | `String`  | Machine hostname                                                         |
| `User`     | `String`  | Current username                                                         |
| `Home`     | `String`  | Path to user's home directory                                            |
| `TempDir`  | `String`  | Path to system temporary directory                                       |
| `Cores`    | `Integer` | Number of logical CPU cores                                              |
| `MemoryTotal` | `Integer` | Total RAM in bytes                                                    |
| `MemoryFree` | `Integer` | Available physical RAM in bytes (memory usable for new work without swapping) |
| `PageSize` | `Integer` | Host memory page size in bytes                                           |
| `PointerSize` | `Integer` | Native pointer width in bits                                           |
| `Endian`   | `String`  | Byte order: `"little"` or `"big"`                                        |

### Zia Example

```zia
module MachineDemo;

bind Zanna.Terminal;
bind Zanna.System.Machine as Machine;
bind Zanna.Text.Fmt as Fmt;

func start() {
    Say("OS: " + Machine.get_Os());
    Say("Arch: " + Machine.get_Arch());
    Say("Endian: " + Machine.get_Endian());
    Say("Cores: " + Fmt.Int(Machine.get_Cores()));
    Say("Pointer bits: " + Fmt.Int(Machine.get_PointerSize()));
    Say("Home: " + Machine.get_Home());
    Say("User: " + Machine.get_User());
}
```

### BASIC Example

```basic
' Operating system information
PRINT "OS: "; Zanna.System.Machine.Os
PRINT "Version: "; Zanna.System.Machine.OsVersion

' User and host
PRINT "User: "; Zanna.System.Machine.User
PRINT "Host: "; Zanna.System.Machine.Host

' Directory paths
PRINT "Home: "; Zanna.System.Machine.Home
PRINT "Temp: "; Zanna.System.Machine.TempDir

' Hardware information
PRINT "CPU Cores: "; Zanna.System.Machine.Cores
PRINT "Architecture: "; Zanna.System.Machine.Arch
PRINT "Page Size: "; Zanna.System.Machine.PageSize
PRINT "Pointer Bits: "; Zanna.System.Machine.PointerSize
PRINT "Total RAM: "; Zanna.System.Machine.MemoryTotal / 1073741824; " GB"
PRINT "Free RAM: "; Zanna.System.Machine.MemoryFree / 1073741824; " GB"

' System characteristics
PRINT "Byte Order: "; Zanna.System.Machine.get_Endian

' Conditional behavior based on OS
IF Zanna.System.Machine.Os = "macos" THEN
    PRINT "Running on macOS"
ELSEIF Zanna.System.Machine.Os = "linux" THEN
    PRINT "Running on Linux"
ELSEIF Zanna.System.Machine.Os = "windows" THEN
    PRINT "Running on Windows"
END IF

' Check available memory before large allocation
DIM requiredMem AS INTEGER = 1073741824  ' 1 GB
IF Zanna.System.Machine.MemoryFree > requiredMem THEN
    PRINT "Sufficient memory available"
ELSE
    PRINT "Warning: Low memory"
END IF
```

### Platform Notes

- **OS**: Returns a lowercase, compile-time platform identifier.
- **OSVer**: macOS reads `kern.osproductversion`; Linux reads `VERSION_ID` from
  `/etc/os-release`, with a `uname` fallback. Windows uses `RtlGetVersion` (ntdll), which returns
  the true OS version independent of the executable's compatibility manifest, so unmanifested and
  custom hosts report the real Windows 10/11 version; it falls back to `GetVersionExA` only if
  `RtlGetVersion` is unavailable.
- **Host**: Uses `gethostname()` on Unix, `GetComputerName()` on Windows.
- **User**: Uses `getpwuid()` on Unix and `GetUserNameW()` on Windows (wide + UTF-8 conversion), with environment fallbacks.
- **Home**: Uses `HOME` (then the passwd entry) on Unix; Windows tries `USERPROFILE`, then
  `HOMEDRIVE` + `HOMEPATH`.
- **Temp**: Uses `TMPDIR`/`TMP`/`TEMP` on Unix (default `/tmp`). Windows uses `GetTempPathW` (wide + UTF-8 conversion) and
  falls back to `C:\Temp`.
- **Cores**: Returns logical (hyper-threaded) cores using `hw.logicalcpu` on macOS,
  `sysconf(_SC_NPROCESSORS_ONLN)` on other Unix, and `GetSystemInfo()` on Windows. Every platform
  returns at least 1: a failed or non-positive probe (including a `-1` `sysconf` result) applies the
  safe minimum, so callers sizing worker pools never receive zero or a negative count.
- **Arch**: Returns `x86_64`, `arm64`, `x86`, `arm`, `wasm32`, or `unknown` from compile-time
  architecture macros. **PageSize** falls back to 4096 if the platform query fails;
  **PointerSize** is `sizeof(void*) * 8`.
- **MemoryTotal/MemoryFree**: `MemoryFree` reports one portable "available memory" estimate — the memory
  usable for new work without swapping, including reclaimable page cache — on every platform:
  Windows uses `GlobalMemoryStatusEx.ullAvailPhys`, macOS uses `(free + inactive)` reclaimable
  pages from `host_statistics64`, and Linux uses `MemAvailable` from `/proc/meminfo` (falling back
  to `free + buffers` on kernels without it). The same threshold is now comparable across hosts.
- **Endian**: Runtime detection via union trick. Most modern systems are little-endian.
- Windows Host/User/Home/TempDir queries use the wide Win32 APIs (`GetComputerNameW`,
  `GetUserNameW`, `GetTempPathW`, `_wgetenv`) and the same validated UTF-8 conversion as
  `Zanna.System.Environment`, so non-ASCII host names, user names, and paths round-trip as valid
  UTF-8.

`Zanna.System.Machine` properties call these same
Machine implementations and inherit all of their fallbacks and limitations.

---

## Zanna.Runtime.Unsafe

Sharp retain/release hooks for deterministic ownership handoff. Most programs
should rely on normal lexical lifetimes and class `deinit`; use these only when
integrating with runtime handles that require explicit ownership control.

**Type:** Static utility class

### Methods

| Method            | Signature        | Description                                                            |
|-------------------|------------------|------------------------------------------------------------------------|
| `Retain(handle)`  | `Void(Object)`   | Increment a live runtime object, array, or string handle's reference count |
| `Release(handle)` | `Integer(Object)`| Decrement a live runtime handle; returns remaining references           |
| `RetainStr(text)` | `Void(String)`   | String-typed retain wrapper for callers that cannot pass `String` as `Object` |
| `ReleaseStr(text)`| `Integer(String)`| String-typed release wrapper; returns the remaining string references   |
| `ValueType(size)` | `Object(Integer)`| Allocate a heap-owned boxed value-type payload for compiler/runtime interop |
| `ValueTypeAddField(obj, offset, kind, retainNow)` | `Void(Object, Integer, Integer, Boolean)` | Register a managed field inside a boxed value-type payload |
| `SetThrowMsg(message)` | `Void(String)` | Store the current throw message for compiler/runtime catch binding |
| `ClearThrowMsg()` | `Void()` | Clear the stored throw message |
| `SetTrapFields(kind, code, line)` | `Void(Integer, Integer, Integer)` | Store thread-local trap metadata fields before raising or lowering a trap |
| `RaiseKind(kind, code, line)` | `Void(Integer, Integer, Integer)` | Raise a runtime trap from explicit trap metadata |

### Notes

- `Release()` runs managed object finalization when it drops the last reference.
- If a finalizer resurrects an object, `Release()` returns the live post-finalizer refcount instead of the transient zero count.
- `ReleaseStr()` returns the actual post-release string refcount for both heap-backed and small-string handles; immortal strings return the maximum `Integer` value.
- Arrays released through `Release()` run element cleanup for object, string, and boxed-value arrays before freeing the array storage.
- Passing `Nothing` is a no-op. Passing a non-runtime, already-freed, raw string payload, or unsupported heap payload traps.
- Reference counts are checked for overflow and underflow in release builds.
- Value-type hooks are for compiler/runtime interop. Size `0` is valid and creates a managed empty value-type object; negative sizes trap. Field registration is idempotent for the same offset/kind and traps for conflicting field kinds.
- Trap-state hooks are for generated code, runtime bridges, and low-level tests.
  Applications should inspect trap data through `Zanna.Diagnostics.CurrentTrap`
  instead of setting or polling mutable trap fields. `Zanna.Runtime.Unsafe.SetThrowMsg`,
  `ClearThrowMsg`, `SetTrapFields`, and `RaiseKind` remain compatibility names
  for these unsafe hooks.

---

## Zanna.Runtime.GC

Garbage collector diagnostics and tuning controls. Provides visibility into the
reference-counting GC's cycle-collection activity. Most programs do not need to
call these methods directly.

**Type:** Static utility class

### Methods

| Method              | Signature    | Description                                                          |
|---------------------|--------------|----------------------------------------------------------------------|
| `Collect()`         | `Integer()`  | Trigger a GC sweep immediately; returns count of objects collected   |
| `TrackedCount()`    | `Integer()`  | Return the number of objects currently tracked by the GC             |
| `TotalCollected()`  | `Integer()`  | Return cumulative count of all objects collected since program start  |
| `PassCount()`       | `Integer()`  | Return the number of GC passes (sweeps) performed so far             |
| `SetThreshold(n)`   | `Void(Integer)` | Run automatic cycle collection after every `n` heap allocations; `0` disables |
| `GetThreshold()`    | `Integer()`  | Return the current automatic collection threshold, or `0` if disabled |

### Notes

- The Zanna runtime uses reference counting with a cycle-collector sweep. `Collect()` forces a sweep to run now rather than waiting for the next automatic trigger.
- Promoted long-lived roots are still used as restore roots during non-full passes, so newly attached young children remain reachable.
- Finalizers for collected objects run before weak references are cleared; if any finalizer resurrects an unreachable cycle member, the entire candidate set is restored and weak references remain live.
- If a finalizer traps during `Collect()` or shutdown finalizer sweeping, the collector balances retained snapshots and clears its in-progress state before re-raising the trap.
- `TrackedCount` is useful for detecting object leaks in long-running programs.
- `TotalCollected()` saturates at the maximum `Integer` value rather than wrapping.
- `SetThreshold()` treats negative values as `0`.
- `TrackedCount()` counts only objects registered with the cycle collector; it is not the total
  number of live runtime heap allocations. Most collection passes may skip promoted objects, with
  a periodic full scan, so one forced pass is not a guarantee that every newly formed long-lived
  cycle was examined.
- These are diagnostic APIs; calling `Collect()` frequently in hot paths can reduce throughput.

### Zia Example

```zia
module GCDemo;

bind Zanna.Terminal;
bind Zanna.Runtime.GC as GC;
bind Zanna.Text.Fmt as Fmt;

func start() {
    Say("Tracked: " + Fmt.Int(GC.TrackedCount()));
    Say("Total collected: " + Fmt.Int(GC.TotalCollected()));

    // Force a sweep and report how many objects were collected
    var freed = GC.Collect();
    Say("Freed this pass: " + Fmt.Int(freed));
    Say("GC passes so far: " + Fmt.Int(GC.PassCount()));
}
```

### BASIC Example

```basic
' Print GC state before a large operation
PRINT "Tracked objects: "; Zanna.Runtime.GC.TrackedCount()
PRINT "Total collected: "; Zanna.Runtime.GC.TotalCollected()

' Allocate a small batch of managed values
DIM values AS OBJECT = Zanna.Collections.Seq.New()
FOR i = 1 TO 100
    values.Push(Zanna.Text.Fmt.Int(i))
NEXT i

' Force a GC sweep to reclaim cycle garbage
DIM freed AS INTEGER = Zanna.Runtime.GC.Collect()
PRINT "Freed by GC: "; freed
PRINT "GC passes: "; Zanna.Runtime.GC.PassCount()
```

---

## Zanna.Memory.WeakRef

Zeroing weak references to managed objects, arrays, and strings. A weak reference observes its
target without keeping that target alive.

**Type:** Instance class, constructed with `Zanna.Memory.WeakRef.New(target)`

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `New(target)` | `WeakRef(Object)` | Create a weak reference without retaining `target`; `Nothing` is allowed |
| `Get()` | `Object()` | Return and retain the live target, or `Nothing` after it has been freed or cleared |
| `IsAlive()` | `Boolean()` | Return `TRUE` while the target is non-null and still live |
| `Reset(target)` | `Void(Object)` | Point at a different live managed target, or clear the reference with `Nothing` |
| `Free()` | `Void()` | Detach from the target and release one owned weak-reference handle |

The registry also exposes static forms `Get(ref)`, `IsAlive(ref)`, `Reset(ref, target)`, and
`Free(ref)`. Ordinary source should use the instance forms.

`New` and `Reset` accept managed runtime objects, arrays, string handles, or `Nothing`; a raw,
freed, or otherwise unsupported pointer traps. They never retain the target. When the target's
last strong reference is released, registered weak references are set to `Nothing`. `Get`
atomically promotes a still-live target to a retained strong reference, so a stored result keeps
the target alive until that result is released.

`Free` is an explicit ownership operation, not just a synonym for `Reset(Nothing)`: it consumes one
reference to the weak-reference object. If that was its last reference, using the old handle again
traps as an invalid or freed weak reference. Passing a null weak-reference receiver to the runtime
helpers returns the neutral result (`Nothing`, `FALSE`, or no-op), but source-level dispatch may
reject a null receiver before the helper is called.

### Zia Example

```zia
module WeakRefDemo;

bind Zanna.Terminal;
bind Zanna.Collections.Seq as Seq;
bind Zanna.Memory.WeakRef as WeakRef;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var target = Seq.New();
    var ref = WeakRef.New(target);

    Say("Target alive: " + Fmt.Bool(ref.IsAlive()));
    var promoted = ref.Get(); // A strong reference when non-null
    Say("Promoted: " + Fmt.Bool(promoted != null));

    ref.Reset(null);
    Say("After clear: " + Fmt.Bool(ref.IsAlive()));
    ref.Free();
}
```

---

## Zanna.Terminal

Terminal input and output operations.

**Type:** Static utility class

### Methods

#### Output

| Method            | Signature       | Description                                             |
|-------------------|-----------------|---------------------------------------------------------|
| `Print(text)`     | `Void(String)`  | Writes text without a trailing newline (flushes output) |
| `PrintInt(value)` | `Void(Integer)` | Writes an integer without a trailing newline (flushes)  |
| `PrintNum(value)` | `Void(Float)`   | Writes a floating-point number without a trailing newline (flushes) |
| `PrintBool(value)`| `Void(Boolean)` | Writes `true` or `false` without a trailing newline (flushes) |
| `Say(text)`       | `Void(String)`  | Writes text followed by a newline                       |
| `SayBool(value)`  | `Void(Boolean)` | Writes `true` or `false` followed by a newline          |
| `SayInt(value)`   | `Void(Integer)` | Writes an integer followed by a newline                 |
| `SayNum(value)`   | `Void(Float)`   | Writes a floating-point number followed by a newline    |

#### Input

| Method               | Signature          | Description                                                 |
|----------------------|--------------------|-------------------------------------------------------------|
| `TryReadLine()`      | `Option<String>()` | Reads a line from stdin; returns `None` on EOF              |
| `ReadLineResult()`   | `Result<String, String>()` | Reads a line from stdin; returns `Err` with an EOF message on EOF |
| `TryAsk(prompt)`     | `Option<String>(String)` | Prints prompt, reads a line from stdin; returns `None` on EOF |
| `AskResult(prompt)`  | `Result<String, String>(String)` | Prints prompt, reads a line from stdin; returns `Err` with an EOF message on EOF |
| `ReadLine()`         | `String()`         | Reads a line from stdin; returns `""` on EOF, so prefer `TryReadLine` or `ReadLineResult` when EOF must be distinguished |
| `ReadKey()`           | `String()`         | Blocks for a single key press and returns a 1-character string |
| `ReadKeyFor(ms)`  | `String(Integer)`  | Waits up to `ms` for a key; returns `""` on timeout (negative = block) |
| `PollKey()`            | `String()`         | Non-blocking key poll; returns `""` if no key is available   |

#### Screen Control

| Method                    | Signature                | Description                                              |
|---------------------------|--------------------------|----------------------------------------------------------|
| `Clear()`                 | `Void()`                 | Clears the terminal screen (TTY only)                    |
| `SetPosition(row, col)`   | `Void(Integer, Integer)` | Move cursor to 1-based row/column (clamped to 1)          |
| `SetColor(fg, bg)`        | `Void(Integer, Integer)` | Set BASIC color codes; use `-1` to leave a channel unchanged |
| `SetCursorVisible(show)`  | `Void(Boolean)`          | Show or hide the cursor                                  |
| `SetAltScreen(enable)`    | `Void(Boolean)`          | Enter or exit the alternate screen                       |
| `Bell()`                  | `Void()`                 | Emits the terminal bell                                  |

#### Output Batching

| Method          | Signature | Description                                               |
|-----------------|-----------|-----------------------------------------------------------|
| `BeginBatch()`  | `Void()`  | Begin batch mode (defers flushes for terminal control)     |
| `EndBatch()`    | `Void()`  | End batch mode and flush pending output                   |
| `Flush()`       | `Void()`  | Force buffered output to be written immediately           |

#### Low-Level Compatibility Methods

| Method             | Signature       | Description |
|--------------------|-----------------|-------------|
| `PrintStr(text)`   | `Void(String)`  | Batch-aware low-level string output without a newline |
| `PrintI64(value)`  | `Void(Integer)` | Batch-aware low-level integer output without a newline |
| `PrintF64(value)`  | `Void(Float)`   | Batch-aware low-level floating-point output without a newline |

### Zia Example

```zia
module TerminalDemo;

bind Zanna.Terminal;

func start() {
    Say("Hello from Terminal!");       // prints with newline
    Print("No newline: ");             // prints without newline
    Say("done.");                       // No newline: done.
}
```

### BASIC Example

```basic
DIM name AS STRING
name = Zanna.Option.UnwrapOrStr(Zanna.Terminal.TryAsk("What is your name? "), "there")
Zanna.Terminal.Say("Hello, " + name + "!")
```

### Note

For most BASIC programs, the `PRINT` and `INPUT` statements are more convenient. Use `Zanna.Terminal` when you need
explicit control or are working at the IL level.

- `ReadLine` returns a null/Nothing string at EOF despite its non-null `String` registry signature. Use the Option or Result forms when EOF must be represented safely.
- Key input is byte-oriented. On Windows, extended keys arrive as `_getch` prefix/scan-code calls;
  on UTF-8 terminals, one Unicode character can require multiple calls. A failed/non-TTY blocking
  read can return the character with code zero rather than `""`.
- `ReadKeyFor`, `SetPosition`, and `SetColor` take 64-bit Integer arguments that are clamped to the
  32-bit range before use rather than truncated: a value above `2147483647` saturates to
  `2147483647` instead of wrapping to a negative one. So a very large `ReadKeyFor` timeout waits the
  maximum instead of blocking indefinitely, and large cursor/color values no longer change sign.
  Use `-1` for an indefinite `ReadKeyFor` wait.
- Screen-control methods other than `Bell` are silent no-ops when stdout is not a TTY. `SetColor`
  ignores the entire request if either channel is below `-1`; values 0–15 select BASIC/bright
  colors, and values 16 or greater are emitted as ANSI 256-color indexes without a 255 clamp.
- `SetAltScreen(TRUE)` enters the alternate screen once, enabling cached raw input and one level of
  output batching; `SetAltScreen(FALSE)` exits and balances them. The toggle is idempotent — it
  transitions (and adjusts batch depth) only on an actual state change, so repeated enable calls
  cannot strand output in batch mode. Normal-exit cleanup is balanced: if the program is still on
  the alternate screen, it ends the batch, emits the alternate-screen exit sequence, and restores
  raw mode.
- Batching is nested. It defers control-sequence flushes and the low-level `Print*` methods above;
  the higher-level `Print`, `PrintInt`, `PrintNum`, `PrintBool`, and `Say*` methods explicitly flush
  and therefore do not remain buffered.

---

## See Also

- [Input/Output](io/README.md) - File system operations and stream I/O
- [Network](network.md) - Network operations and DNS resolution
