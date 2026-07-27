---
status: active
audience: public
last-verified: 2026-07-26
---

# Zia — Getting Started

Learn Zia by example. For a complete reference, see **[Zia Reference](../languages/zia-reference.md)**.

> **What is Zia?**
> A modern, statically-typed language with C-like syntax, first-class classes (reference types), struct types,
> generics, and module bindings. It runs on Zanna's VM and can be compiled to native code.

---

## Table of Contents

1. [Your First Program](#1-your-first-program)
2. [Variables and Types](#2-variables-and-types)
3. [Control Flow](#3-control-flow)
4. [Functions](#4-functions)
5. [Class Types](#5-class-types)
6. [Struct Types](#6-struct-types)
7. [Generic Collections](#7-generic-collections)
8. [Modules and Bindings](#8-modules-and-bindings)
9. [Working with the Runtime](#9-working-with-the-runtime)
10. [Example Project](#10-example-project)
11. [Where to Go Next](#11-where-to-go-next)

---

## 1. Your First Program

Create a file named `hello.zia`:

```zia
module Hello;

bind Zanna.Terminal;

func start() {
    Say("Hello, World!");
}
```

Run it:

```bash
zanna run hello.zia
```

**Key points:**

- Every file starts with a `module` declaration
- `start()` is the entry point (like `main()` in C)
- Use `bind Zanna.Terminal;` to import terminal functions, then `Say()` for console output with newline
- Statements end with semicolons; blocks use `{ }`
- Comments use `//` for single-line and `/* */` for multi-line

---

## Try Zia Interactively

Before writing files, you can experiment with Zia in the interactive REPL:

```bash
zanna repl
```

The REPL lets you type Zia code and see results immediately:

```text
zia> "Hello, world"
Hello, world
zia> 2 + 3 * 4
14
zia> var x = 42
zia> x
42
zia> func square(n: Integer) -> Integer { return n * n; }
zia> square(7)
49
```

Expressions are automatically evaluated and printed. Variables persist across inputs. You can define functions, import modules with `bind`, and redefine functions. Errors are reported without losing your session state.

Type `.help` for available commands and `.quit` to exit. See the **[REPL Guide](../tools/repl.md)** for full documentation.

---

## 2. Variables and Types

### Variable Declaration

Use `var` for mutable variables and `final` for constants. `let` is accepted as
a compatibility alias for `final`:

```zia
var x = 42;              // Type inferred as Integer
var name: String = "Alice";  // Explicit type
final PI = 3.14159;      // Immutable constant
let answer = 42;         // Also immutable
```

### Built-in Types

| Type | Description | Example |
|------|-------------|---------|
| `Boolean` | True or false | `true`, `false` |
| `Integer` | 64-bit signed integer | `42`, `-17`, `0xFF` |
| `Number` | 64-bit floating-point | `3.14`, `1e-5` |
| `Byte` | Byte-sized integer value | `0xFF` |
| `String` | UTF-8 string | `"hello"`, `"line\n"` |
| `Any` | Managed top type for boxed values, objects, strings, and function references | `42`, `"text"`, `&handler` |
| `Never` | Bottom type for code paths that do not produce a value | — |
| `Unit` | Explicit no-value marker for `Result[Unit]` and `()` | `()` |

Common lowercase aliases are accepted, such as `int`, `bool`, `double`, and
`string`. `Bytes` names the runtime class `Zanna.Collections.Bytes`.

Zia code does not use raw pointers. Runtime handles are represented by typed
Zanna runtime classes, collections, `Any`, and callback bridges.

### String Interpolation

Embed expressions in strings with `${...}`:

```zia
bind Zanna.Terminal;

var name = "Alice";
var age = 30;
Say("${name} is ${age} years old");
```

---

## 3. Control Flow

### If Statements

```zia
bind Zanna.Terminal;

if score > 100 {
    Say("High score!");
} else if score > 50 {
    Say("Good score");
} else {
    Say("Try again");
}
```

Note: Parentheses around conditions are optional.

### While Loops

```zia
bind Zanna.Terminal;

var i = 0;
while i < 10 {
    PrintInt(i);
    i = i + 1;
}
```

### For Loops

C-style for loops:

```zia
bind Zanna.Terminal;

for (var i = 0; i < 10; i = i + 1) {
    PrintInt(i);
}
```

### For-In Loops (Ranges)

```zia
bind Zanna.Terminal;

// Exclusive range: 0 to 9
for i in 0..10 {
    PrintInt(i);
}

// Inclusive range: 0 to 10
for i in 0..=10 {
    PrintInt(i);
}

for i in (0..10).step(2) {
    PrintInt(i);
}
```

Range steps must be positive. Literal zero or negative steps are rejected at
compile time; dynamic non-positive steps trap before iteration starts.

---

## 4. Functions

### Basic Functions

```zia
bind Zanna.Terminal;

func greet(name: String) {
    Say("Hello, ${name}!");
}

func add(a: Integer, b: Integer) -> Integer {
    return a + b;
}

func start() {
    greet("World");
    var sum = add(3, 4);
    SayInt(sum);  // 7
}
```

### Function Features

- Parameters require type annotations
- Return type follows `->` (omit for void functions)
- Functions can call other functions defined in the same module

### Exporting Functions

Use `expose` before `func` to make a function visible to other modules in a
mixed-language project:

```zia
expose func calculateScore(x: Integer, y: Integer) -> Integer {
    return x * y + 10;
}

expose async func fetchScore() -> Integer {
    return 10;
}
```

### Importing Foreign Functions

Use `foreign func` to declare a function defined in another module (e.g., a
BASIC library). Foreign declarations have no body:

```zia
bind Zanna.Terminal;

expose foreign func Factorial(n: Integer) -> Integer;

func start() {
    var result = Factorial(10);
    SayInt(result);
}
```

For full details on mixed-language projects, see the
[Cross-Language Interop Guide](../languages/interop.md).

---

## 5. Class Types

Classes are reference types with identity, inheritance, and methods.

### Defining a Class

```zia
class Player {
    String name;
    Integer health;
    Integer score;

    func init(playerName: String) {
        name = playerName;
        health = 100;
        score = 0;
    }

    expose func takeDamage(amount: Integer) {
        health = health - amount;
        if health < 0 {
            health = 0;
        }
    }

    expose func addScore(points: Integer) {
        score = score + points;
    }

    expose func isAlive() -> Integer {
        if health > 0 {
            return 1;
        }
        return 0;
    }

    expose func getHealth() -> Integer {
        return health;
    }
}
```

### Using Classes

```zia
bind Zanna.Terminal;

func start() {
    // Create a new class instance
    var player = new Player("Alice");

    // Call methods
    player.addScore(100);
    player.takeDamage(25);

    Print("Health: ");
    SayInt(player.getHealth());  // 75
}
```

**Key points:**

- Use `new ClassName()` to create instances
- Class members are **private by default**; mark a method (or field) `expose` to make it accessible from outside the class
- `init()` is the conventional initializer method (callable via `new` without `expose`)
- Methods can access their own fields directly (implicit `self`)

---

## 6. Struct Types

Struct types have copy semantics — assignments create copies.

```zia
bind Zanna.Math;

struct Point {
    Integer x;
    Integer y;

    func init(px: Integer, py: Integer) {
        x = px;
        y = py;
    }

    expose func distanceFromOrigin() -> Number {
        return Sqrt(x * x + y * y);
    }
}
```

---

## 7. Generic Collections

Zia has language-level generic collections such as `List[T]`, `Map[String, T]`,
and `Set[T]`. These are distinct from the object-style runtime classes under
`Zanna.Collections.*`.

### Lists

```zia
bind Zanna.Terminal;

// Create a list of integers
var numbers: List[Integer] = [];
numbers.add(10);
numbers.add(20);
numbers.add(30);

// Access elements
var first = numbers.get(0);  // 10
var hasTwenty = numbers.has(20);
numbers.sortDesc();
numbers.shuffle();

// Iterate
var i = 0;
while i < numbers.count() {
    SayInt(numbers.get(i));
    i = i + 1;
}
```

### Maps

```zia
bind Zanna.Terminal;

// Empty map literal: {}. Map literals also support {key: value, ...}.
var scores: Map[String, Integer] = {};
scores.set("Alice", 95);
scores.set("Bob", 87);

if scores.has("Alice") {
    SayInt(scores.get("Alice") ?? 0);
}
```

> `Map.get` returns `V?`; use `??`, `getOr`, or an explicit null check before
> passing the value to non-optional code. Map keys must be `String` or `Integer`
> (the Sema layer rejects other key types). For `Map[String, String]`, a missing
> key is `null`, not an empty string. Use `{}` for an empty map literal, or
> `new Map[String, V]()` when an explicit constructed map is clearer.

### Class Instance Lists

```zia
// List of class instances
var players: List[Player] = [];

var p1 = new Player("Alice");
players.add(p1);

var p2 = new Player("Bob");
players.add(p2);
```

When you need the object-based runtime collection classes instead, bind
`Zanna.Collections` and use constructors such as `List.New()`, `Map.New()`, or
`Set.New()`.

---

## 8. Modules and Bindings

### Module Declaration

Every file starts with a module declaration:

```zia
module MyGame;
```

### Binding Other Modules

Bind other `.zia` files to use their types and functions:

```zia
module Game;

bind "./entities";    // binds entities.zia from same directory
bind "./utils";       // binds utils.zia
bind "./config" as C; // bind with alias

func start() {
    var player = new Player();  // Player defined in entities.zia
    player.init("Hero");
}
```

If two bound files export the same top-level type name, qualify that type with
the bound module name or alias:

```zia
bind "./alpha"; // module Alpha; expose class WishDup { ... }
bind "./beta";  // module Beta;  expose class WishDup { ... }

var a: Alpha.WishDup = new Alpha.WishDup();
var b: Beta.WishDup = new Beta.WishDup();
```

**Bind path rules:**

- `"./foo"` — Relative path, adds `.zia` extension
- `"../bar"` — Parent directory relative path
- `"foo"` — Same directory, adds `.zia` extension

### Binding Runtime Namespaces

Bind Zanna runtime namespaces to use their functions without qualification:

```zia
module Game;

bind Zanna.Terminal;     // Import terminal functions
bind Zanna.Graphics;     // Import graphics classes

func start() {
    Say("Hello from Zia!");           // No need for Zanna.Terminal.Say()
    var canvas = new Canvas("Game", 800, 600);
    Zanna.Time.Clock.Sleep(16);           // SleepMs is under Zanna.Time
}
```

**Namespace bind options:**

- `bind Zanna.Terminal;` — Import all symbols
- `bind Zanna.Terminal as T;` — Import with alias (use `T.Say()`)
- `bind Zanna.Terminal { Say };` — Import specific symbols only

---

## 9. Working with the Runtime

Zia programs have access to the registered Zanna runtime APIs. Import namespaces
with `bind` or use fully qualified names; the exposed surface is generated from
`src/il/runtime/runtime.def`.

### Terminal I/O

```zia
bind Zanna.Terminal;

// Output
Say("Hello");           // Print with newline
Print("No newline");    // Print without newline
SayInt(42);             // Print integer with newline
PrintInt(42);           // Print integer without newline

// Input
var line = TryReadLine();       // Read a line (returns Option<String>, None on EOF)
var result = ReadLineResult();  // Read a line (returns Result<String, String>)
var key = GetKey();             // Wait for key press (blocking)
var keyTimeout = GetKeyTimeout(100);  // With timeout (ms), "" on timeout
var peek = InKey();             // Non-blocking key check, "" if no key

// Terminal control
Clear();                // Clear screen
SetPosition(row, col);  // Move cursor
SetColor(fg, bg);       // Set foreground/background (0-15)
SetCursorVisible(false);    // Hide cursor
```

> **Note:** You can also use fully qualified names like `Zanna.Terminal.Say()` without binding.

### Color Codes

| Code | Color |
|------|-------|
| 0 | Black |
| 1 | Red |
| 2 | Green |
| 3 | Yellow |
| 4 | Blue |
| 5 | Magenta |
| 6 | Cyan |
| 7 | White |
| 8-15 | Bright variants |

### Time Functions

```zia
// SleepMs is available as Zanna.Time.Clock.Sleep (use fully qualified name or bind)
Zanna.Time.Clock.Sleep(500);        // Sleep for 500 milliseconds
```

### Math Functions

```zia
bind Zanna.Math;

var abs = AbsInt(-42);                       // 42
var sqrt = Sqrt(16.0);                       // 4.0
var rand = Zanna.Math.Random.NextInt(100);   // Random 0-99
```

---

## 10. Example Project

Here's a complete mini-game demonstrating Zia features:

```zia
module GuessGame;

bind Zanna.Terminal;

var secretNumber = 0;
var guessCount = 0;
var gameOver = 0;

func start() {
    Say("=== Number Guessing Game ===");
    Say("I'm thinking of a number between 1 and 100.");

    // Generate random number 1-100
    secretNumber = Zanna.Math.Random.NextInt(100) + 1;
    guessCount = 0;
    gameOver = 0;

    while gameOver == 0 {
        Print("Your guess: ");

        // For simplicity, we'll use a fixed sequence
        // In a real game, you'd read user input
        var guess = getNextGuess();

        guessCount = guessCount + 1;

        if guess == secretNumber {
            Say("Correct! You got it in ${guessCount} guesses!");
            gameOver = 1;
        } else if guess < secretNumber {
            Say("Too low!");
        } else {
            Say("Too high!");
        }
    }
}

func getNextGuess() -> Integer {
    // Placeholder - would normally read from input
    return 50;
}
```

For more complete examples, see the `examples/games/` directory:
- `frogger/` — Full Frogger game with classes and collision detection
- `centipede/` — Centipede arcade game with classes and game loop
- `pacman/` — Crackman maze-chase game demonstrating movement and collision

---

## 11. Where to Go Next

**Language Documentation:**

- [Zia Reference](../languages/zia-reference.md) — Complete language specification
- [Runtime Library](../zannalib/README.md) — All available classes and methods

**Examples:**

- `examples/games/frogger/` — Complete Frogger game example
- `examples/games/centipede/` — Centipede arcade game
- `examples/games/crackman/` — Crackman game

**Interactive:**

- [REPL Guide](../tools/repl.md) — Interactive Zia experimentation

**Related Guides:**

- [IL Guide](../il/il-guide.md) — Understand the compiled output
- [Cross-Language Interop](../languages/interop.md) — Mixed Zia + BASIC projects
- [Frontend How-To](../internals/frontend-howto.md) — How frontends work

---

## Quick Reference

### Program Structure

```zia
module ModuleName;

bind "./other";

// Global variables
var globalVar = 0;

// Class types
class MyClass {
    Integer field;
    func method() { }
}

// Struct types
struct MyStruct {
    Integer field;
}

// Functions
func myFunction(param: Type) -> ReturnType {
    return value;
}

// Entry point
func start() {
    // Program starts here
}
```

### Operators

| Category | Operators |
|----------|-----------|
| Arithmetic | `+`, `-`, `*`, `/`, `%` |
| Comparison | `==`, `!=`, `<`, `<=`, `>`, `>=` |
| Logical | `&&`, `\|\|`, `!` |
| Bitwise | `&`, `\|`, `^`, `~` |
| Assignment | `=` |
