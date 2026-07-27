---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 13: The Standard Library

Imagine you're building a house. You could make your own bricks, forge your own nails, mill your own lumber. Or you could go to the hardware store and buy materials that thousands of engineers have already perfected.

Programming works the same way. You *could* write your own code to calculate square roots, parse dates, generate random numbers, and encrypt data. Mathematicians, computer scientists, and engineers have spent decades solving these problems. Why solve them again?

The answer is: you shouldn't have to. That's what the standard library is for.

---

## What Is a Standard Library?

A *standard library* is a collection of pre-written code that comes bundled with a programming language. When you install Zanna, you don't just get the ability to write `if` statements and `for` loops. You get thousands of lines of carefully tested, optimized code for common tasks.

Think of it as a toolbox that comes free with the language:

- Need to find the absolute value of a number? There's a function for that.
- Want to read a file from disk? Already written.
- Need to shuffle an array randomly? One line of code.
- Want to know what time it is? Just ask.

Every mainstream programming language has a standard library. Python is famous for its "batteries included" philosophy. Java has a massive standard library. C has a minimal one. Zanna strikes a balance: comprehensive enough to be useful, organized enough to be learnable.

**Why does this matter?** Three reasons:

1. **You don't have to reinvent the wheel.** Someone already wrote code to format dates. It handles edge cases you haven't thought of. Use it.

2. **Standard library code is tested.** Millions of programs use these functions. Bugs get found and fixed. Your hand-written version won't have that testing.

3. **Other programmers know it.** When you use `Zanna.Math.Sqrt()`, any Zanna programmer knows what it does. Your custom `mySquareRoot()` function? They'd have to read your code.

The standard library is a superpower. Learning it makes you a dramatically more productive programmer.

---

## A Tour of Zanna's Standard Library

Zanna's standard library is organized into modules under the `Zanna` namespace. Each module handles a different category of tasks:

| Module | Purpose | You'll Use It For |
|--------|---------|-------------------|
| `Zanna.Collections` | Data structures | Lists, maps, sets |
| `Zanna.Core.Convert` | String-to-number conversion | Processing user input |
| `Zanna.Crypto` | Hashing and encoding | Security, verification |
| `Zanna.System.Environment` | System information | Config, command-line args |
| `Zanna.Text.Fmt` | String formatting | Creating output messages |
| `Zanna.Graphics` | Low-level drawing | Games, visualizations |
| `Zanna.GUI` | Widget-based UI | Desktop applications |
| `Zanna.IO.Dir` | Directory operations | Navigating the filesystem |
| `Zanna.IO.File` | File operations | Reading/writing data |
| `Zanna.IO.Path` | Path manipulation | Building file paths safely |
| `Zanna.Math` | Mathematical functions | Calculations, geometry |
| `Zanna.Network` | TCP/UDP networking | Web clients, servers |
| `Zanna.Math.Random` | Random number generation | Games, simulations, testing |
| `Zanna.String` | Advanced string operations | Text processing |
| `Zanna.Terminal` | Console input/output | User interaction, debugging |
| `Zanna.Threads` | Concurrency | Parallel processing |
| `Zanna.Time` | Date and time | Timestamps, scheduling |

Let's explore each one in depth.

---

## Zanna.Terminal: Console I/O

You've used `Zanna.Terminal` throughout this book. It's how your programs talk to users.

### Basic Input and Output

```zia
bind Zanna.Terminal;

Say("Hello!");              // Print with newline
Print("No newline here");   // Print without newline
var input = ReadLine();     // Read a line of text, or null on EOF
var char = GetKey();        // Read a single keypress
```

### When to Use Each

- **`Say()`**: Most output. Each message on its own line.
- **`Print()`**: When you want to build up a line piece by piece, or when prompting for input on the same line.
- **`ReadLine()`**: Getting text input when EOF should be handled explicitly.
- **`GetKey()`**: Games, menus, or "press any key" prompts.

### Terminal Control

For interactive applications, you can control the cursor and colors:

```zia
bind Zanna.Terminal;

Clear();                    // Clear the screen
SetPosition(10, 5);         // Move cursor to column 10, row 5
SetColor(1, 0);             // Set foreground/background color codes
SetCursorVisible(false);        // Hide the blinking cursor
SetCursorVisible(true);        // Show it again
```

These are essential for building text-based games, progress bars, or any program where you want precise control over what the user sees.

### Practical Example: A Simple Menu

```zia
bind Zanna.Terminal;
bind Convert = Zanna.Core.Convert;

func showMenu() -> Integer {
    Clear();
    Say("=== Main Menu ===");
    Say("1. New Game");
    Say("2. Load Game");
    Say("3. Options");
    Say("4. Quit");
    var choice = TryAsk("Choose (1-4): ");
    if choice.IsNone {
        return 4;
    }
    return Convert.ToInt64(choice.UnwrapStr().Trim());
}
```

---

## Zanna.Math: Mathematics

Math operations beyond basic arithmetic live in `Zanna.Math`. These functions have been implemented by experts, optimized for speed, and tested for accuracy across edge cases.

### Basic Functions

```zia
bind Zanna.Math as Math;

Math.Abs(-5.0);          // 5.0 (absolute value)
Math.AbsInt(-5);         // 5   (integer absolute value)
Math.MinInt(3, 7);       // 3 (smaller of two integers)
Math.MaxInt(3, 7);       // 7 (larger of two integers)
Math.ClampInt(15, 0, 10); // 10 (constrain to range 0-10)
```

### Rounding

```zia
bind Zanna.Math as Math;

Math.Floor(3.7);       // 3.0 (round down)
Math.Ceil(3.2);        // 4.0 (round up)
Math.Round(3.5);       // 4.0 (round to nearest)
Math.Round(3.4);       // 3.0
```

### Powers and Roots

```zia
bind Zanna.Math as Math;

Math.Sqrt(16.0);       // 4.0 (square root)
Math.Pow(2.0, 8.0);    // 256.0 (2 to the 8th power)
Math.Log(2.718);       // ~1.0 (natural logarithm)
Math.Log10(100.0);     // 2.0 (base-10 logarithm)
Math.Exp(1.0);         // ~2.718 (e^x)
```

### Trigonometry

```zia
bind Zanna.Math as Math;

Math.Sin(0.0);         // 0.0
Math.Cos(0.0);         // 1.0
Math.Tan(0.0);         // 0.0
Math.Atan2(y, x);      // Angle from coordinates
```

### Constants

```zia
bind Zanna.Math as Math;

Math.Pi;               // 3.14159265358979...
Math.Euler;                // 2.71828182845904...
```

### When Would You Use This?

**Game development:** Calculate distances, angles, movement vectors.

```zia
bind Zanna.Math as Math;

// Distance between two points
func distance(x1: Number, y1: Number, x2: Number, y2: Number) -> Number {
    var dx = x2 - x1;
    var dy = y2 - y1;
    return Math.Sqrt(dx * dx + dy * dy);
}
```

**Scientific calculations:** Statistics, physics simulations, financial modeling.

```zia
bind Zanna.Math as Math;

// Compound interest
func compoundInterest(principal: Number, rate: Number, years: Integer) -> Number {
    return principal * Math.Pow(1.0 + rate, years * 1.0);
}
```

**Graphics:** Smooth animations, circular motion, wave patterns.

```zia
bind Zanna.Math as Math;

// Move in a circle
var angle = time * speed;
var x = centerX + radius * Math.Cos(angle);
var y = centerY + radius * Math.Sin(angle);
```

### You Don't Want to Write This Yourself

Computing square roots, trigonometric functions, and logarithms accurately is *hard*. These algorithms have been refined for decades. A naive square root implementation might:

- Be slow (iterating too many times)
- Be inaccurate (floating-point errors)
- Crash on edge cases (negative numbers, infinity)

The standard library handles all of this. Use it.

---

## Zanna.Math.Random: Randomness

Games need dice rolls. Simulations need random data. Testing needs random inputs. `Zanna.Math.Random` (aliased as `Random` below) provides it all.

### Basic Random Values

```zia
bind Zanna.Math.Random as Random;

Random.Range(1, 100);  // Random integer from 1 to 100 (inclusive)
Random.NextDouble();   // Random float from 0.0 to 1.0
Random.Dice(2);        // 1 or 2 — simulates a coin flip
```

### Working with Collections

```zia
bind Zanna.Math.Random as Random;
bind Seq = Zanna.Collections.Seq;
bind Box = Zanna.Core.Box;
bind Zanna.Terminal;

var deck = Seq.New();
var i = 1;
while i <= 10 {
    deck.Push(Box.I64(i));
    i = i + 1;
}
Random.Shuffle(deck);  // Shuffle in place
SayInt(deck.Count);
```

### Reproducible Randomness

For testing or game replays, you can *seed* the random number generator:

```zia
bind Zanna.Math.Random as Random;

Random.Seed(12345);  // Same seed = same sequence of "random" numbers
```

This is crucial for debugging. If a bug only appears sometimes, set a seed to reproduce it reliably.

### Practical Examples

**Dice roll:**
```zia
bind Zanna.Math.Random as Random;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

var die = Random.Dice(6);
Say("You rolled: " + Fmt.Int(die));
```

**Coin flip:**
```zia
bind Zanna.Math.Random as Random;
bind Zanna.Terminal;

if Random.Chance(0.5) {
    Say("Heads!");
} else {
    Say("Tails!");
}
```

**Random enemy spawn:**
```zia
bind Zanna.Math.Random as Random;

// Simple weighted random (for illustration)
var roll = Random.Range(1, 100);
if roll <= 50 {
    spawn("goblin");
} else if roll <= 80 {
    spawn("orc");
} else if roll <= 95 {
    spawn("troll");
} else {
    spawn("dragon");
}
```

**Password generator:**
```zia
bind Zanna.Math.Random as Random;

func generatePassword(length: Integer) -> String {
    var chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%";
    var password = "";

    var i = 0;
    while i < length {
        var index = Random.Range(0, chars.Length - 1);
        password = password + chars[index];
        i = i + 1;
    }

    return password;
}
```

---

## Zanna.Time: Date and Time

Time is surprisingly complex. Leap years, time zones, daylight saving, calendar systems. The standard library handles the complexity so you don't have to.

### Getting the Current Time

```zia
bind Zanna.Time;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

var dt = Time.DateTime.Now();

Say("Year: "   + Fmt.Int(Time.DateTime.Year(dt)));
Say("Month: "  + Fmt.Int(Time.DateTime.Month(dt)));
Say("Day: "    + Fmt.Int(Time.DateTime.Day(dt)));
Say("Hour: "   + Fmt.Int(Time.DateTime.Hour(dt)));
Say("Minute: " + Fmt.Int(Time.DateTime.Minute(dt)));
Say("Second: " + Fmt.Int(Time.DateTime.Second(dt)));
```

### Formatting Dates

Dates need to be displayed in different formats depending on context:

```zia
bind Zanna.Time;
bind Zanna.Text.Fmt as Fmt;
bind Zanna.Terminal;

var dt = Time.DateTime.Now();
var y  = Time.DateTime.Year(dt);
var mo = Time.DateTime.Month(dt);
var d  = Time.DateTime.Day(dt);
var h  = Time.DateTime.Hour(dt);
var mi = Time.DateTime.Minute(dt);
var s  = Time.DateTime.Second(dt);

// ISO format (international standard)
Say(Fmt.IntPad(y, 4, "0") + "-" + Fmt.IntPad(mo, 2, "0") + "-" + Fmt.IntPad(d, 2, "0"));
// 2024-03-15

// With time
Say(Fmt.IntPad(y, 4, "0") + "-" + Fmt.IntPad(mo, 2, "0") + "-" + Fmt.IntPad(d, 2, "0") +
    " " + Fmt.IntPad(h, 2, "0") + ":" + Fmt.IntPad(mi, 2, "0") + ":" + Fmt.IntPad(s, 2, "0"));
// 2024-03-15 14:30:45
```

### Measuring Elapsed Time

For performance measurement or timing games:

```zia
bind Zanna.Time;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

var start = Time.Clock.NowMs();

// Do some work...
processData();

var elapsed = Time.Clock.NowMs() - start;
Say("Processing took " + Fmt.Int(elapsed) + " ms");
```

### Delays and Pauses

```zia
bind Zanna.Terminal;
bind Zanna.Time;

Say("Loading...");
Time.Clock.Sleep(2000);  // Pause for 2000 milliseconds (2 seconds)
Say("Done!");
```

### Practical Example: Simple Stopwatch

```zia
bind Zanna.Terminal;
bind Zanna.Time;
bind Zanna.Text.Fmt as Fmt;

func stopwatch() {
    Say("Press Enter to start...");
    TryReadLine().UnwrapOrStr("");

    var start = Time.Clock.NowMs();
    Say("Stopwatch running. Press Enter to stop.");
    TryReadLine().UnwrapOrStr("");

    var elapsed = Time.Clock.NowMs() - start;
    var seconds = elapsed * 1.0 / 1000.0;

    Say("Elapsed: " + Fmt.NumFixed(seconds, 2) + " seconds");
}
```

### Why You Don't Want to Write This Yourself

Date and time code is notoriously bug-prone:

- Is 2024 a leap year? (Yes. 2100? No. 2000? Yes.)
- How many days in February? (Depends on the year.)
- When does daylight saving time start? (Depends on the country, and it changes.)
- What time is it in Tokyo right now? (Depends on when you ask.)

The standard library has been battle-tested against these edge cases. Trust it.

---

## Zanna.System.Environment: System Information

Your program doesn't run in isolation. It runs on a specific computer, in a specific directory, perhaps launched with command-line arguments. `Zanna.System.Environment` gives you access to this context.

### Command-Line Arguments

When someone runs your program from the terminal with arguments:

```bash
$ zia myprogram.zia input.txt --verbose
```

You can access those arguments:

```zia
bind Zanna.System.Environment;
bind Zanna.Terminal;

var count = Zanna.System.Environment.GetArgumentCount();

for i in 0..count {
    Say("Argument: " + Zanna.System.Environment.GetArgument(i));
}
// Output:
// Argument: input.txt
// Argument: --verbose
```

### Environment Variables

Operating systems have configuration through environment variables:

```zia
bind Env = Zanna.System.Environment;

var home = Env.GetVariable("HOME");       // /Users/alice
var path = Env.GetVariable("PATH");       // System PATH
var editor = Env.GetVariable("EDITOR");   // Preferred text editor

// Check if a variable exists
if Env.HasVariable("DEBUG") {
    // Debug mode is enabled
    enableVerboseLogging();
}
```

### System Information

```zia
bind Env = Zanna.System.Environment;
bind Zanna.System.Machine as Machine;

var osName = Machine.Os;         // "windows", "macos", or "linux"
var home = Env.GetVariable("HOME");  // User's home directory
```

### Practical Example: Cross-Platform Configuration

```zia
bind Env = Zanna.System.Environment;
bind Zanna.IO.Path as Path;
bind Zanna.System.Machine as Machine;

func getConfigPath() -> String {
    var home = Machine.Home;

    if Machine.Os == "windows" {
        return Path.Join(Path.Join(Path.Join(home, "AppData"), "Local"), "MyApp/config.json");
    } else if Machine.Os == "macos" {
        return Path.Join(Path.Join(Path.Join(home, "Library"), "Application Support"), "MyApp/config.json");
    } else {
        return Path.Join(Path.Join(home, ".config"), "myapp/config.json");
    }
}
```

---

## Zanna.Text.Fmt: String Formatting

Concatenating strings with `+` gets messy when mixing numbers and text. `Zanna.Text.Fmt` makes formatting cleaner.

### Basic Formatting

```zia
bind Zanna.Text.Fmt as Fmt;
bind Zanna.Terminal;

var name = "Alice";
var score = 95;

// Format values to strings
Say("Player " + name + " scored " + Fmt.Int(score) + " points!");
// "Player Alice scored 95 points!"

// Fixed decimal places
Say(Fmt.NumFixed(3.14159, 2));   // "3.14"
Say(Fmt.NumFixed(3.14159, 4));   // "3.1416"
```

### Number Formatting

```zia
bind Zanna.Text.Fmt as Fmt;

// Decimal places
Fmt.NumFixed(3.14159, 2);    // "3.14"
Fmt.NumFixed(3.14159, 4);    // "3.1416"

// Zero padding
Fmt.IntPad(42, 5, "0");      // "00042"
Fmt.IntPad(1234, 8, "0");    // "00001234"

// Hex, binary, octal
Fmt.Hex(255);                // "ff"
Fmt.Bin(10);                 // "1010"
Fmt.Oct(8);                  // "10"
```

### Practical Example: Formatted Table

```zia
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func printScoreboard(players: List[Player]) {
    Say("Name            Score");
    Say("--------------------------");

    var i = 0;
    while i < players.count() {
        var p = players.get(i);
        Say(p.name + "  " + Fmt.Int(p.score));
        i = i + 1;
    }
}

// Output:
// Name            Score
// --------------------------
// Alice  950
// Bob  875
// Charlie  1200
```

---

## Zanna.String: String Utilities

Beyond basic concatenation, `Zanna.String` provides advanced text operations as instance methods on strings.

### Padding and Alignment

```zia
// PadLeft, PadRight, and Repeat are instance methods on String values
var padded = "42".PadLeft(5, "0");    // "00042"
var rpad   = "hi".PadRight(5, " ");   // "hi   "
```

### Repetition and Reversal

```zia
var dashes = "-".Repeat(40);          // A line of dashes
var rev    = "hello".Flip();          // "olleh"
```

### Text Searching and Splitting

```zia
// String methods for searching
var s = "Hello, World!";
var idx  = s.IndexOf("World");       // 7
var has  = s.Has("Hello");           // true
var up   = s.ToUpper();              // "HELLO, WORLD!"
var low  = s.ToLower();              // "hello, world!"
var trim = "  hi  ".Trim();          // "hi"

// Split into parts
var parts = "a,b,c".Split(",");      // Seq of ["a", "b", "c"]
```

### Practical Example: Validating User Input

```zia
func isValidUsername(username: String) -> Boolean {
    // Must be 3-20 characters
    if username.Length < 3 || username.Length > 20 {
        return false;
    }

    // Must start with a letter (A-Z or a-z)
    var first = username.Substring(0, 1);
    if first.ToLower() == first.ToUpper() {
        // Not a letter — both cases are same for non-alpha chars
        return false;
    }

    return true;
}
```

---

## Collections in Zia and Zanna.Collections

Zia has two collection layers that work together:

- **Language-level generic collections** like `List[T]`, `Map[String, T]`, and `Set[T]`. These are the most natural choice in everyday Zia code.
- **Runtime collection classes** under `Zanna.Collections`, such as `Queue`, `Stack`, `Heap`, `OrderedMap`, and `BitSet`. These cover specialized data structures or boxed, dynamically typed storage.

Use the language-level generic collections first. Reach for `Zanna.Collections.*` when you need a specialized runtime container.

### Generic List

```zia
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

var list: List[String] = [];

list.add("first");
list.add("second");
list.add("third");

Say(list.get(0));          // "first"
Say(Fmt.Int(list.count()));   // 3

list.removeAt(0);          // Remove first element
list.insert(1, "inserted");
list.clear();
```

### Generic Map

```zia
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

var scores: Map[String, Integer] = {};

scores.set("Alice", 950);
scores.set("Bob", 875);
scores.set("Charlie", 1200);

Say(Fmt.Int(scores.get("Alice") ?? 0));    // 950
Say(Fmt.Int(scores.Length));          // 3

if scores.has("David") {
    Say("David is in the game");
} else {
    Say("David hasn't played yet");
}

var keys = scores.keys();
var i = 0;
while i < keys.Count {
    var key = keys.GetStr(i);
    Say(key + ": " + Fmt.Int(scores.get(key) ?? 0));
    i = i + 1;
}

scores.remove("Bob");
```

### Generic Set

```zia
bind Zanna.Collections;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

var tags: Set[String] = Set.New();

tags.add("important");
tags.add("urgent");
tags.add("important");  // Ignored - already present

Say(Fmt.Int(tags.count()));          // 2
Say(Fmt.Bool(tags.has("urgent")));   // true

tags.remove("urgent");
```

Non-empty set literals like `{"a", "b"}` are supported. The empty literal `{}` is a map by default, but it is accepted as an empty set in a declared `Set[T]` initializer. `set {}` and `Set.New()` are also unambiguous empty-set forms.

### Specialized Runtime Collections

Some containers are runtime classes rather than language literals. These live under `Zanna.Collections`:

```zia
bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;

var queue = Queue.New();
queue.Push(Box.Str("first"));
queue.Push(Box.Str("second"));
Say(Box.ToStr(queue.Pop()));   // "first"

var stack = Stack.New();
stack.Push(Box.I64(10));
stack.Push(Box.I64(20));
SayInt(Box.ToI64(stack.Pop()));  // 20
```

Use these when you need FIFO/LIFO behavior, heaps, ordered maps, frozen collections, bit sets, or other specialized runtime containers.

### Practical Example: Word Frequency Counter

```zia
bind Zanna.String as Str;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func countWords(text: String) -> Map[String, Integer] {
    var frequency: Map[String, Integer] = {};

    for word in Str.Split(text.ToLower(), " ") {
        var cleaned = word.Trim();
        if cleaned.Length > 0 {
            if frequency.has(cleaned) {
                frequency.set(cleaned, (frequency.get(cleaned) ?? 0) + 1);
            } else {
                frequency.set(cleaned, 1);
            }
        }
    }

    return frequency;
}

var text = "the quick brown fox jumps over the lazy dog the fox";
var counts = countWords(text);

for word, count in counts {
    Say(word + ": " + Fmt.Int(count));
}
```

---

## Zanna.Core.Convert: Parsing Values

Converting strings to other types is so common it gets its own module.

### Basic Parsing

```zia
bind Convert = Zanna.Core.Convert;

Convert.ToInt64("42");        // 42
Convert.ToDouble("3.14");     // 3.14
Convert.ToStringInt(42);      // "42"
Convert.ToStringDouble(3.14); // "3.14"
```

### Error Handling

Parsing can fail. Handle it gracefully:

```zia
bind Convert = Zanna.Core.Convert;
bind Zanna.Terminal;

try {
    var num = Convert.ToInt64("not a number");
} catch(e) {
    Say("Invalid input - please enter a number");
}
```

### Practical Example: Robust Input Function

```zia
bind Zanna.Terminal;
bind Convert = Zanna.Core.Convert;
bind Parse = Zanna.Core.Parse;

func getNumber(prompt: String) -> Integer {
    while true {
        Print(prompt);
        var input = TryReadLine().UnwrapOrStr("").Trim();

        if !Parse.IsInt(input) {
            Say("Please enter a valid number.");
            continue;
        }
        return Convert.ToInt64(input);
    }
    return 0;
}

// Usage:
var age = getNumber("Enter your age: ");
```

---

## Zanna.IO.File / Zanna.IO.Dir / Zanna.IO.Path

We covered file operations in Chapter 9, but let's review the key patterns.

### File Operations

```zia
bind File = Zanna.IO.File;

// Reading
var content = File.ReadAllText("data.txt");
var lines = File.ReadAllLines("data.txt");
var bytes = File.ReadAllBytes("image.png");

// Writing
File.WriteAllText("output.txt", "Hello, World!");
File.Append("log.txt", "New entry\n");
File.WriteAllBytes("copy.png", bytes);

// Checking
if File.Exists("config.json") {
    // Load configuration
}

// Deleting
File.Delete("temp.txt");
```

### Directory Operations

```zia
bind Zanna.IO.Dir as Dir;

Dir.Make("output");
Dir.MakeAll("output/reports/2024");  // Creates intermediate directories

var files = Dir.Files("data");
var dirs = Dir.Dirs("data");

if Dir.Exists("backup") {
    // Directory exists
}

Dir.Remove("temp");
```

### Path Manipulation

The **critical** module for working with file paths:

```zia
bind Zanna.IO.Path as Path;

// Join paths safely (handles OS-specific separators)
var path = Path.Join(Path.Join("users/alice", "documents"), "file.txt");
// On Windows: users\alice\documents\file.txt
// On macOS/Linux: users/alice/documents/file.txt

// Extract components
Path.Name("/path/to/file.txt");     // "file.txt"
Path.Extension("/path/to/file.txt");      // ".txt"
Path.Directory("/path/to/file.txt");      // "/path/to"
Path.Stem("/path/to/file.txt");     // "file"
```

### Why Path.Join() Matters

Never concatenate paths with `+`:

```zia
bind Zanna.IO.Path as Path;

var dir = "users";
var filename = "file.txt";

// BAD - breaks on different operating systems
var badPath = dir + "/" + filename;

// GOOD - works everywhere
var goodPath = Path.Join(dir, filename);
```

Windows uses backslashes (`\`), Unix uses forward slashes (`/`). `Path.Join()` handles this automatically.

---

## Zanna.Crypto: Security and Encoding

For hashing, encoding, and unique identifiers.

### Hashing

Hashing converts data into a fixed-size fingerprint. The same input always produces the same hash, but you can't reverse a hash back to the original.

```zia
bind Zanna.Crypto.Hash as Hash;

var sha = Hash.SHA256("hello");    // 64-character hex string
var mac = Hash.HmacSHA256("key", "hello");
```

**When to use:**
- Verifying file integrity (did this file change?)
- Storing passwords (never store plain text!)
- Creating cache keys
- Detecting duplicates

MD5, SHA-1, CRC32, and their legacy HMAC variants remain available through
`Zanna.Crypto.Legacy.Hash` for compatibility with old formats, but new code
should use SHA-256, HMAC-SHA256, `Password`, `KeyDerive`, or `Cipher`.

### Encoding

Base64 encoding converts binary data to text. This lives in `Zanna.Text.Codec`:

```zia
bind Codec = Zanna.Text.Codec;

var encoded = Codec.Base64Encode("Hello, World!");
// "SGVsbG8sIFdvcmxkIQ=="

var decoded = Codec.Base64Decode(encoded);
// "Hello, World!"
```

**When to use:**
- Embedding binary data in text formats (JSON, XML)
- Basic data obfuscation (not security!)
- Email attachments

### Unique Identifiers

GUIDs (Globally Unique Identifiers) are guaranteed-unique strings:

```zia
bind Uuid = Zanna.Text.Uuid;

var id = Uuid.Generate();
// "550e8400-e29b-41d4-a716-446655440000"
```

**When to use:**
- Database primary keys
- Session IDs
- Tracking unique objects
- File names that won't collide

### Practical Example: Password Storage

**Never store passwords in plain text.** Hash them:

```zia
bind Zanna.Crypto.Hash as Hash;
bind Uuid = Zanna.Text.Uuid;
bind Zanna.Terminal;

func hashPassword(password: String, salt: String) -> String {
    // Combine password with salt to prevent rainbow table attacks
    var salted = password + salt;
    return Hash.SHA256(salted);
}

func checkPassword(input: String, salt: String, storedHash: String) -> Boolean {
    var inputHash = hashPassword(input, salt);
    return inputHash == storedHash;
}

// Registration:
var salt = Uuid.Generate();  // Random salt for this user
var hash = hashPassword(userPassword, salt);
// Store both hash and salt in database

// Login:
if checkPassword(inputPassword, storedSalt, storedHash) {
    Say("Login successful!");
}
```

---

## Putting It All Together

Here's a complete program using multiple standard library modules:

```zia
module StdlibDemo;

bind Zanna.Terminal;
bind Env = Zanna.System.Environment;
bind Zanna.Time.DateTime as DateTime;
bind Zanna.Time.Clock as Clock;
bind Zanna.Math as Math;
bind Zanna.Text.Fmt as Fmt;
bind Zanna.Math.Random as Random;
bind Zanna.System.Machine as Machine;
bind Zanna.Crypto.Hash as Hash;

func start() {
    Say("=== Zanna Standard Library Demo ===");
    Say("");

    // Environment
    Say("System Information:");
    Say("  Home: " + Machine.Home);
    Say("");

    // Time
    var dt = DateTime.Now();
    Say("Current Time:");
    Say("  Year: " + Fmt.Int(DateTime.Year(dt)));
    Say("  Month: " + Fmt.Int(DateTime.Month(dt)));
    Say("");

    // Math
    Say("Math Demo:");
    var angle = Math.Pi / 4.0;
    Say("  sin(45 deg) = " + Fmt.NumFixed(Math.Sin(angle), 4));
    Say("  sqrt(2) = " + Fmt.NumFixed(Math.Sqrt(2.0), 4));
    Say("");

    // Random
    Say("Random Numbers:");
    var i = 0;
    while i < 5 {
        var roll = Random.Dice(6);
        Say("  Dice roll: " + Fmt.Int(roll));
        i = i + 1;
    }
    Say("");

    // Collections
    var scores: Map[String, Integer] = {};
    scores.set("Alice", 950);
    scores.set("Bob", 875);
    scores.set("Charlie", 1200);

    Say("Leaderboard:");
    Say("  Alice: " + Fmt.Int(scores.get("Alice") ?? 0) + " points");
    Say("  Bob: " + Fmt.Int(scores.get("Bob") ?? 0) + " points");
    Say("  Charlie: " + Fmt.Int(scores.get("Charlie") ?? 0) + " points");
    Say("");

    // Crypto
    var message = "Hello, Zanna!";
    Say("Crypto Demo:");
    Say("  Message: " + message);
    Say("  SHA256: " + Hash.SHA256(message).Substring(0, 16) + "...");
    Say("");

    // Performance measurement
    Say("Performance Test:");
    var startMs = Clock.NowMs();

    var sum = 0.0;
    var k = 0;
    while k < 100000 {
        sum = sum + Math.Sqrt(k * 1.0);
        k = k + 1;
    }

    var elapsed = Clock.NowMs() - startMs;
    Say("  100,000 square roots in " + Fmt.Int(elapsed) + " ms");

    Say("");
    Say("Demo complete!");
}
```

---

## Things You Don't Need to Reinvent

Here are some things the standard library does that would be *very hard* to write yourself:

### Accurate Square Roots

A naive approach:

```zia
// DON'T DO THIS - slow and possibly inaccurate
func naiveSqrt(n: Number) -> Number {
    var guess = n / 2.0;
    var i = 0;
    while i < 100 {
        guess = (guess + n / guess) / 2.0;
        i = i + 1;
    }
    return guess;
}
```

The standard library implementation is faster, handles edge cases (0, negative numbers, infinity), and is accurate to the last bit.

### Correct Date Formatting

```zia
// DON'T DO THIS - buggy and incomplete
func formatDate(year: Integer, month: Integer, day: Integer) -> String {
    var monthNames = ["Jan", "Feb", "Mar", ...];  // All 12 months
    return monthNames[month - 1] + " " + day + ", " + year;
}
```

What about time zones? Localization? 12-hour vs 24-hour time? The standard library handles it all.

### Cryptographic Hashing

```text
// DON'T DO THIS - SHA256 is complex
func sha256(input: String) -> String {
    // This is literally hundreds of lines of careful bit manipulation
    // with specific constants and transformations.
    // One mistake = security vulnerability.
}
```

Cryptography is easy to get wrong. Use the standard library.

### Random Number Generation

```text
// DON'T DO THIS - not actually random
func badRandom() -> Integer {
    // Most simple approaches produce predictable sequences
    // or have statistical biases.
}
```

Good random number generators are mathematically complex. The standard library uses proven algorithms.

---

## How to Discover Standard Library Features

You won't memorize every function. Here's how to find what you need:

### 1. Explore in Your Editor

Most editors offer autocomplete. Type `Zanna.` and see what modules appear. Type `Zanna.Math.` and see available functions.

### 2. Read the Documentation

The official Zanna documentation covers every module. Keep it bookmarked. Appendix D of this book provides a quick reference.

### 3. Guess Intelligently

Standard libraries follow conventions:
- `something.Length` or `something.Size()` for size
- `collection.count()` for generic Zia collections, `Length` for runtime containers
- `has(...)` or `Has(...)` for membership, depending on whether you're using a generic collection or a runtime class
- `Fmt.Int(n)` or `Fmt.NumFixed(x, digits)` for number-to-string conversion
- `Path.Join(a, b)` for file-system-safe path construction

If a function exists, it probably has the name you'd expect.

### 4. Search Before You Write

Before implementing something:
1. Think: "Is this a common problem?"
2. If yes, search: "Zanna [what you need]"
3. Check if the standard library has it

You'll be surprised how often the answer is "yes, there's a function for that."

---

## Tips for Learning New APIs

When you encounter a new standard library module:

### Start with the Basics

Don't try to learn every function. Start with the most common operations:

```text
bind File = Zanna.IO.File;

// For File, start with:
File.ReadAllText()
File.WriteAllText()
File.Exists()

// Learn the advanced stuff when you need it
```

### Read Examples

Examples teach faster than API reference lists. This chapter is full of examples. Seek out more in documentation and tutorials.

### Experiment in a Scratch File

Create a `test.zia` file and try things:

```zia
module Test;

bind Zanna.Math as Math;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Experiment here
    var x = Math.Sqrt(2.0);
    Say(Fmt.NumFixed(x, 4));

    // What happens if...
    var y = Math.Sqrt(-1.0);  // Error? NaN?
    Say(Fmt.NumFixed(y, 4));
}
```

Run it, see what happens, modify, repeat.

### Check Edge Cases

Once you understand the basics, explore edge cases:

- What happens with empty strings?
- What happens with negative numbers?
- What happens with null?
- What happens with very large values?

Understanding edge cases prevents bugs.

---

## Common Patterns

Some standard library patterns appear constantly. Learn these by heart.

### Pattern: Safe User Input

```zia
bind Zanna.Terminal;
bind Convert = Zanna.Core.Convert;
bind Parse = Zanna.Core.Parse;

func getInt(prompt: String) -> Integer {
    while true {
        Print(prompt);
        var input = TryReadLine();
        if input.IsNone || !Parse.IsInt(input.UnwrapStr().Trim()) {
            Say("Invalid input. Please enter a number.");
            continue;
        }
        return Convert.ToInt64(input.UnwrapStr().Trim());
    }
    return 0;
}
```

### Pattern: Read Config with Default

```zia
bind Env = Zanna.System.Environment;

func getConfig(key: String, defaultValue: String) -> String {
    if Env.HasVariable(key) {
        var envValue = Env.GetVariable(key);
        if envValue.Length > 0 {
            return envValue;
        }
    }
    return defaultValue;
}
```

### Pattern: Safe File Read

```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

func readFileSafe(path: String) -> String {
    if !File.Exists(path) {
        return "";
    }

    try {
        return File.ReadAllText(path);
    } catch(e) {
        Say("Error reading " + path + ": " + e.message);
        return "";
    }
}
```

### Pattern: Measure Performance

```zia
bind Zanna.Time;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

var start = Time.Clock.NowMs();

// Code being measured goes here.
var total = 0;
var i = 0;
while i < 1000 {
    total = total + i;
    i = i + 1;
}

var elapsed = Time.Clock.NowMs() - start;
Say("Loop took " + Fmt.Int(elapsed) + " ms");
```

### Pattern: Build a Path

```zia
bind Zanna.IO.Path as Path;
bind Zanna.System.Machine as Machine;
bind Zanna.Time;
bind Zanna.Text.Fmt as Fmt;

var dt = Time.DateTime.Now();
var dateStr = Fmt.IntPad(Time.DateTime.Year(dt), 4, "0") + "-" +
              Fmt.IntPad(Time.DateTime.Month(dt), 2, "0") + "-" +
              Fmt.IntPad(Time.DateTime.Day(dt), 2, "0");

var logDir = Path.Join(Path.Join(Machine.Home, ".myapp"), "logs");
var logFile = Path.Join(logDir, dateStr + ".log");
```

---

## Summary

The Zanna standard library provides:

| Category | Modules | Key Functions |
|----------|---------|---------------|
| I/O | Terminal, IO.File, IO.Dir, IO.Path | Say, ReadLine, File.ReadAllText, Path.Join |
| Numbers | Math, Math.Random, Convert | Math.Sqrt, Math.Sin, Random.Range, Convert.ToInt64 |
| Text | String, Fmt | String.Trim, String.Split, Fmt.Int, Fmt.NumFixed |
| Time | Time | Time.DateTime.Now, Time.Clock.NowMs, Time.Clock.Sleep |
| Data | Generic collections, Zanna.Collections | `list.add`, `map.set`, `set.add`, `Queue.New` |
| System | Environment, Machine | Env.GetArgument, Env.GetVariable, Machine.Os |
| Security | Crypto.Hash, Codec, Uuid | Hash.SHA256, Hash.HmacSHA256, Uuid.Generate |

The standard library is your first resort when you need functionality. It's tested, optimized, and familiar to other programmers. Learning it is as important as learning the language syntax.

Think of the standard library as your tool belt. You don't build a hammer every time you need to drive a nail. You reach for the tool that's already there.

---

## Exercises

**Exercise 13.1**: Write a program that prints the current date in three different formats: ISO (2024-03-15), US (March 15, 2024), and EU (15/03/2024).

**Exercise 13.2**: Use `Zanna.Math` to calculate the distance between two points (x1, y1) and (x2, y2) using the Pythagorean theorem.

**Exercise 13.3**: Create a simple stopwatch: prompt to start, wait for user input, then display elapsed time in seconds and milliseconds.

**Exercise 13.4**: Use a `Map[String, Integer]` to build a word frequency counter. Given a sentence, count how many times each word appears.

**Exercise 13.5**: Write a program that takes a filename as a command-line argument and prints its SHA256 hash. If the file doesn't exist, print a helpful error message.

**Exercise 13.6**: Build a function that validates an email address (must contain @, must have text before and after @, domain must contain a dot).

**Exercise 13.7**: Create a password generator that accepts length as an argument and generates a random password with lowercase, uppercase, digits, and symbols.

**Exercise 13.8** (Challenge): Build a simple file backup utility. It should:
- Take a source file path
- Create a backup directory if it doesn't exist
- Copy the file with a timestamp in the name (e.g., `file_2024-03-15_143022.txt`)
- Print a summary of what it did

**Exercise 13.9** (Challenge): Create a "quote of the day" program that:
- Stores quotes in a file (one per line)
- Uses today's date as a seed for the random number generator
- Displays a "random" quote that stays the same all day
- Shows a different quote tomorrow

---

*We've finished Part II! You now have the building blocks: strings, files, error handling, structures, modules, and the standard library.*

*Part III introduces object-oriented programming — a powerful way to model complex systems with objects that combine data and behavior.*

*[Continue to Part III: Thinking in Objects](../part3-objects/14-objects.md)*
