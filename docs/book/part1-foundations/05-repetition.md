---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 5: Repetition

Close your eyes and imagine this task: print every number from 1 to 1000 on the screen.

With what we know so far, you might start writing:

```zia
bind Zanna.Terminal;

Say(1);
Say(2);
Say(3);
Say(4);
// ... 996 more lines ...
Say(1000);
```

A thousand lines of nearly identical code. Your fingers would ache. Your eyes would glaze over. You'd make typos. You'd miscount. And what if someone asked you to change it to print 1 to 10,000? You'd need to add 9,000 more lines.

This isn't just tedious. It's *wrong*. Fundamentally, philosophically wrong.

Here's the truth that separates programmers from typists: **whenever you find yourself writing the same thing over and over, you're not programming. You're doing the computer's job.** And the computer is much, much better at repetition than you are.

Computers don't get bored. They don't lose focus. They don't make typos on line 847 because they're tired. They can repeat an action a million times in the blink of an eye, each repetition as precise as the first.

This chapter teaches you to harness that power. We call it *looping* or *iteration* - the ability to say "do this thing repeatedly" instead of writing out every repetition by hand.

---

## Why Loops Change Everything

Before we dive into syntax, let's appreciate what loops really give us.

**Problem 1**: Print "Hello" five times.

Without loops:
```zia
bind Zanna.Terminal;

Say("Hello");
Say("Hello");
Say("Hello");
Say("Hello");
Say("Hello");
```

With loops:
```zia
bind Zanna.Terminal;

for i in 1..=5 {
    Say("Hello");
}
```

The loop version is shorter. But more importantly, if you want to change it to print "Hello" 100 times, you change one number. In the non-loop version, you'd need to add 95 more lines.

**Problem 2**: Add up all numbers from 1 to 100.

Without loops:
```zia
var sum = 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12 + 13 + 14 + 15 +
          16 + 17 + 18 + 19 + 20 + 21 + 22 + 23 + 24 + 25 + 26 + 27 + 28 + 29 +
          30 + 31 + 32 + 33 + 34 + 35 + 36 + 37 + 38 + 39 + 40 + 41 + 42 + 43 +
          // ... this is getting absurd ...
```

With loops:
```zia
bind Zanna.Terminal;

var sum = 0;
for i in 1..=100 {
    sum = sum + i;
}
Say(sum);  // 5050
```

The loop version not only works, it can be changed to sum 1 to 1,000,000 by changing one number. Try doing that by hand.

Loops transform programs from static scripts into dynamic engines.

---

## Mental Model: The Assembly Line

Before we look at code, let's build a mental picture.

Think of a factory assembly line. A conveyor belt moves products past a workstation. At the workstation, a worker performs the same action on each product: inspect it, stamp it, package it. The belt keeps moving, bringing product after product, until the supply runs out.

A loop is like that assembly line:
- The **loop body** is the workstation - the actions to perform
- The **loop variable** tracks which product we're on (product 1, product 2, product 3...)
- The **condition** determines when the belt stops

Or think of your morning routine. Every day you:
1. Wake up
2. Check if it's a workday
3. If yes: shower, dress, eat breakfast, commute
4. Repeat tomorrow

That's a loop. The condition is "is it a workday?" The body is your morning activities. Life is full of loops - you just never noticed them as such.

---

## The while Loop

The simplest loop repeats as long as a condition is true. We call it `while` because it loops *while* something remains true.

```zia
bind Zanna.Terminal;

var count = 1;

while count <= 5 {
    Say(count);
    count = count + 1;
}
```

Output:
```text
1
2
3
4
5
```

**The structure:**
```text
while condition {
    // body: code to repeat
}
```

The computer checks the condition. If true, it runs the body. Then it checks the condition again. If still true, it runs the body again. This continues until the condition becomes false.

### Tracing Through Execution

Let's be the computer. We'll trace through the loop above step by step, tracking everything that happens.

**Initial state:**
- `count` = 1

**Iteration 1:**
1. Check condition: Is `count <= 5`? Is `1 <= 5`? **Yes, true.**
2. Enter the loop body.
3. Execute `Say(count)`: Print "1"
4. Execute `count = count + 1`: count becomes 2
5. Reach end of body, go back to condition check.

**Iteration 2:**
1. Check condition: Is `count <= 5`? Is `2 <= 5`? **Yes, true.**
2. Enter the loop body.
3. Print "2"
4. count becomes 3
5. Go back to condition check.

**Iteration 3:**
1. Check condition: Is `3 <= 5`? **Yes, true.**
2. Print "3"
3. count becomes 4
4. Go back.

**Iteration 4:**
1. Is `4 <= 5`? **Yes, true.**
2. Print "4"
3. count becomes 5
4. Go back.

**Iteration 5:**
1. Is `5 <= 5`? **Yes, true** (5 equals 5, so it's less-than-or-equal).
2. Print "5"
3. count becomes 6
4. Go back.

**Attempted Iteration 6:**
1. Check condition: Is `6 <= 5`? **No, false.**
2. **Skip the body entirely. Exit the loop.**

The loop ends. Whatever code comes after the loop's `}` runs next.

This mental tracing is how programmers debug loops. When a loop behaves unexpectedly, sit down with pencil and paper and trace through each iteration. What's the value of each variable? What's the condition? Why did or didn't we enter the body?

### The Condition Comes First

Notice that `while` checks the condition *before* running the body. If the condition is false from the start, the body never runs - not even once:

```zia
bind Zanna.Terminal;

var x = 10;
while x < 5 {
    Say("This never prints");
}
Say("Loop finished");
```

Output:
```text
Loop finished
```

We check: Is `10 < 5`? No. So we skip the body entirely and move on.

This is important. A `while` loop guarantees zero or more iterations. It might not iterate at all if the condition is initially false.

---

## Infinite Loops: The Good, The Bad, and The Intentional

What if the condition never becomes false?

```zia
bind Zanna.Terminal;

while true {
    Say("Forever!");
}
```

The condition is literally `true`. It will never become `false`. This loop runs forever - an *infinite loop*.

### The Bad: Accidental Infinite Loops

The most common cause is forgetting to update the variable you're checking:

```zia
bind Zanna.Terminal;

var count = 1;
while count <= 5 {
    Say(count);
    // Oops! We forgot: count = count + 1;
}
```

Let's trace this:
- count starts at 1
- Is `1 <= 5`? Yes. Print "1".
- Is `1 <= 5`? Yes. Print "1".
- Is `1 <= 5`? Yes. Print "1".
- Is `1 <= 5`? Yes. Print "1".
- ... forever ...

We print "1" infinitely because `count` never changes. The condition `count <= 5` is always true.

Another classic mistake - changing the wrong variable:

```zia
bind Zanna.Terminal;

var i = 0;
var j = 0;
while i < 10 {
    Say(i);
    j = j + 1;  // Wrong! We're incrementing j, but checking i
}
```

We're incrementing `j`, but the condition checks `i`. Since `i` never changes, the loop never ends.

### Detecting Infinite Loops

If your program seems frozen, an infinite loop is often the culprit. Signs include:
- The program doesn't respond
- The same output appears repeatedly (if the loop prints something)
- Your computer's fan spins up (the CPU is working hard doing nothing useful)

To stop an infinite loop: Press **Ctrl+C** in the terminal. This sends an interrupt signal that terminates the program.

### The Good: Intentional Infinite Loops

Sometimes you *want* a loop that runs forever - or at least until something external stops it.

**Game loops** run continuously, processing input and drawing frames until the player quits:

```zia
while true {
    // Handle input
    // Update game state
    // Draw the next frame

    var playerPressedQuit = true;
    if playerPressedQuit {
        break;  // Exit the loop
    }
}
```

**Server loops** wait for connections indefinitely:

```zia
bind Zanna.Terminal;

while true {
    // Wait for the next connection
    var connection = "client-1";
    Say("Handling " + connection);
}
```

The key is intentionality. An infinite loop is a bug when accidental, but a feature when deliberate. When writing an intentional infinite loop, add a comment explaining why:

```zia
// Main game loop - runs until player quits
while true {
    ...
}
```

---

## The for Loop

When you know how many times to repeat, `for` is cleaner than `while`. Instead of manually initializing, checking, and incrementing a counter, `for` handles all of that:

```zia
bind Zanna.Terminal;

for i in 1..6 {
    Say(i);
}
```

Output:
```text
1
2
3
4
5
```

The variable `i` automatically takes on values 1, 2, 3, 4, 5. You don't need to declare it with `var`, initialize it, or increment it. The `for` loop does all of that.

**The structure:**
```text
for variable in range {
    // body
}
```

### Understanding Ranges

The `1..6` is called a *range*. It represents a sequence of numbers. But there's something important to understand:

**`..` (two dots) excludes the end:**
```zia
bind Zanna.Terminal;

for i in 1..5 {
    Say(i);  // Prints 1, 2, 3, 4 (NOT 5!)
}
```

The range `1..5` includes 1, 2, 3, 4 but stops *before* 5.

**`..=` (two dots and equals) includes the end:**
```zia
bind Zanna.Terminal;

for i in 1..=5 {
    Say(i);  // Prints 1, 2, 3, 4, 5
}
```

The range `1..=5` includes 1, 2, 3, 4, and 5.

Why have two notations? The exclusive range (`..`) makes certain patterns elegant:

```zia
// Process array indices 0 through length-1
for i in 0..array.Length {
    // When length is 5, i goes 0, 1, 2, 3, 4
    // That's exactly indices 0 through 4
}
```

If you're iterating over array indices, exclusive ranges match naturally. If you're counting things humans count (1 to 10), inclusive ranges (`..=`) feel more natural.

### Range Examples

```zia
bind Zanna.Terminal;

// Count from 0 to 9
for i in 0..10 {
    Say(i);  // 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
}

// Count from 1 to 10
for i in 1..=10 {
    Say(i);  // 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
}

// Count from 5 to 9
for i in 5..10 {
    Say(i);  // 5, 6, 7, 8, 9
}

// Count from 10 to 10 (one iteration)
for i in 10..=10 {
    Say(i);  // 10
}

// Empty range: no iterations
for i in 5..5 {
    Say(i);  // Nothing prints - range is empty
}
```

### Counting Down

To count backward, use `.rev()` to reverse the range:

```zia
bind Zanna.Terminal;

for i in (1..=10).rev() {
    Say(i);
}
```

Output:
```text
10
9
8
7
6
5
4
3
2
1
```

Note the parentheses around the range - they're necessary because we're calling `.rev()` on the range.

Classic countdown:

```zia
bind Zanna.Terminal;

for i in (1..=5).rev() {
    Say(i);
}
Say("Liftoff!");
```

Output:
```text
5
4
3
2
1
Liftoff!
```

### Stepping (Counting by 2s, 3s, etc.)

Sometimes you don't want every number - you want every second number, or every tenth:

```zia
bind Zanna.Terminal;

// Count by 2s: even numbers from 0 to 10
for i in (0..=10).step(2) {
    Say(i);  // 0, 2, 4, 6, 8, 10
}

// Count by 3s
for i in (0..20).step(3) {
    Say(i);  // 0, 3, 6, 9, 12, 15, 18
}

// Count by 5s
for i in (0..=100).step(5) {
    Say(i);  // 0, 5, 10, 15, ... 95, 100
}
```

You can combine stepping with reversing:

```zia
bind Zanna.Terminal;

// Count down by 2s
for i in (0..=10).step(2).rev() {
    Say(i);  // 10, 8, 6, 4, 2, 0
}
```

### Discarding the Loop Variable

Sometimes you only want to repeat work a fixed number of times and never use
the counter itself. Name the variable `_` (a single underscore) to say so
explicitly — the compiler treats `_` as an intentional discard and will not
warn about it being unused:

```zia
bind Zanna.Terminal;

// Fire three times; we don't care which iteration we're on.
for _ in 0..3 {
    Say("beep");
}
```

If you name the variable (`for i in 0..3`) and never read `i`, the compiler
emits warning `W001` about the unused variable. Use `_` when the repetition
count is all that matters. You can reuse `_` in as many loops as you like
within the same function.

---

## Choosing Between while and for

Both loops can accomplish the same tasks, but each has its natural use cases.

**Use `for` when you know the number of iterations:**
- "Do this 10 times"
- "Process items 1 through 100"
- "Count down from 5 to 1"
- "Go through each index of an array"

```zia
bind Zanna.Terminal;

// We know: exactly 10 iterations
for i in 1..=10 {
    Say("Iteration " + i);
}
```

**Use `while` when you don't know how many iterations you'll need:**
- "Keep asking until they enter a valid password"
- "Read lines until end of file"
- "Run until the user quits"
- "Search until we find a match"

```zia
bind Zanna.Terminal;

// We don't know: depends on user input
var password = "";
while password != "secret" {
    Print("Password: ");
    password = TryReadLine().UnwrapOrStr("");
}
Say("Access granted!");
```

The user might guess correctly on the first try (1 iteration) or the hundredth (100 iterations). We can't know in advance, so `while` is the right choice.

**Mental shortcut**: If you're writing `for i in ...`, you probably know the iterations. If you're writing `while someCondition`, you probably don't.

---

## Breaking Out Early

Sometimes you want to stop a loop before its natural end. The `break` statement immediately exits the loop:

```zia
bind Zanna.Terminal;

for i in 1..100 {
    if i * i > 50 {
        Say("Stopping at " + i);
        break;
    }
    Say(i + " squared is " + (i * i));
}
Say("Loop finished");
```

Output:
```text
1 squared is 1
2 squared is 4
3 squared is 9
4 squared is 16
5 squared is 25
6 squared is 36
7 squared is 49
Stopping at 8
Loop finished
```

When `i` becomes 8, `8 * 8 = 64 > 50`, so we print the message and `break`. Execution jumps immediately past the loop's closing brace to "Loop finished".

### Why Break is Useful

**Searching**: Stop when you find what you're looking for.

```zia
bind Zanna.Terminal;

var numbers = [4, 8, 15, 16, 23, 42];
var target = 16;
var foundAt = -1;  // -1 means "not found"

for i in 0..numbers.Length {
    if numbers[i] == target {
        foundAt = i;
        break;  // Found it! No need to keep looking.
    }
}

if foundAt >= 0 {
    Say("Found " + target + " at index " + foundAt);
} else {
    Say("Not found");
}
```

Without `break`, the loop would continue checking every element even after finding the target. With thousands of elements, that wastes time.

**Early termination**: Stop when continuing is pointless.

```zia
bind Zanna.Terminal;

// Check if a number is prime
var n = 97;
var isPrime = true;

for i in 2..n {
    if n % i == 0 {
        isPrime = false;
        break;  // Found a divisor - no need to check more
    }
}

if isPrime {
    Say(n + " is prime");
} else {
    Say(n + " is not prime");
}
```

Once we find any divisor, we know the number isn't prime. No point checking more.

**User-controlled loops**:

```zia
bind Zanna.Terminal;

while true {
    Print("Enter a command (quit to exit): ");
    var command = TryReadLine().UnwrapOrStr("");

    if command == "quit" {
        Say("Goodbye!");
        break;
    }

    Say("Processing command: " + command);
}
```

The loop is infinite, but `break` provides an exit when the user types "quit".

---

## Skipping Iterations with continue

While `break` exits the loop entirely, `continue` skips the *rest of the current iteration* and moves to the next one:

```zia
bind Zanna.Terminal;

for i in 1..=10 {
    if i % 2 == 0 {
        continue;  // Skip even numbers
    }
    Say(i);
}
```

Output:
```text
1
3
5
7
9
```

Let's trace through:
- i = 1: Is 1 even? No. Print "1".
- i = 2: Is 2 even? Yes. `continue` - skip the Say, go to next iteration.
- i = 3: Is 3 even? No. Print "3".
- i = 4: Is 4 even? Yes. Skip.
- ... and so on

When `continue` executes, the program jumps back to the loop header, increments (for `for` loops), checks the condition, and starts the next iteration. Everything after `continue` in the current iteration is skipped.

### Continue Examples

**Processing only valid data:**

```zia
bind Zanna.Terminal;

var values = [10, -5, 20, -15, 30, 0, 40];

for i in 0..values.Length {
    if values[i] <= 0 {
        continue;  // Skip non-positive values
    }
    Say("Processing: " + values[i]);
}
```

Output:
```text
Processing: 10
Processing: 20
Processing: 30
Processing: 40
```

**Skipping specific cases:**

```zia
bind Zanna.Terminal;

// Print numbers 1-20, but skip multiples of 3
for i in 1..=20 {
    if i % 3 == 0 {
        continue;
    }
    Say(i);
}
```

**Avoiding deeply nested conditions:**

Without `continue`:
```zia
bind Zanna.Terminal;

var items = [10, -5, 25, 3];

for i in 0..items.Length {
    if items[i] > 0 {
        if items[i] < 20 {
            if items[i] % 2 == 0 {
                Say("Processing: " + items[i]);
            }
        }
    }
}
```

With `continue` (cleaner):
```zia
bind Zanna.Terminal;

var items = [10, -5, 25, 3];

for i in 0..items.Length {
    if items[i] <= 0 { continue; }
    if items[i] >= 20 { continue; }
    if items[i] % 2 != 0 { continue; }

    Say("Processing: " + items[i]);
}
```

The second version "guards" against invalid cases, letting the main logic stand out clearly.

### Break vs. Continue: A Summary

| Statement | Effect |
|-----------|--------|
| `break` | Exit the loop entirely. Done looping. |
| `continue` | Skip rest of current iteration. Go to next iteration. |

Think of a checkout line. `break` is leaving the store. `continue` is letting someone cut in front of you - you skip them, but you're still in line.

---

## Nested Loops: Loops Within Loops

Loops can contain other loops. When they do, the inner loop runs completely for each iteration of the outer loop.

```zia
bind Zanna.Terminal;

for row in 1..=3 {
    for col in 1..=4 {
        Print("*");
    }
    Say("");  // New line after each row
}
```

Output:
```text
****
****
****
```

Let's trace through this carefully:

**Outer iteration 1 (row = 1):**
- Inner loop runs: col = 1, print "*"
- Inner loop runs: col = 2, print "*"
- Inner loop runs: col = 3, print "*"
- Inner loop runs: col = 4, print "*"
- Inner loop ends
- Print newline: "****" now on its own line

**Outer iteration 2 (row = 2):**
- Inner loop runs completely again: prints "****"
- Print newline

**Outer iteration 3 (row = 3):**
- Inner loop runs completely: prints "****"
- Print newline

Total: The inner loop runs 4 iterations, but it does so 3 times (once per outer iteration). That's 4 x 3 = 12 stars total.

### The Pattern

With nested loops, iterations *multiply*. A 100-iteration outer loop with a 100-iteration inner loop means 10,000 total iterations. Be mindful of this - it's easy to accidentally create very slow code.

### Practical Example: Multiplication Table

```zia
bind Zanna.Terminal;

Say("    1   2   3   4   5");
Say("  +-------------------");

for row in 1..=5 {
    Print(row + " |");
    for col in 1..=5 {
        var product = row * col;
        if product < 10 {
            Print("  " + product + " ");
        } else {
            Print(" " + product + " ");
        }
    }
    Say("");
}
```

Output:
```text
    1   2   3   4   5
  +-------------------
1 |  1   2   3   4   5
2 |  2   4   6   8  10
3 |  3   6   9  12  15
4 |  4   8  12  16  20
5 |  5  10  15  20  25
```

### Nested Loops and Coordinates

Nested loops are natural for 2D structures - grids, tables, game boards:

```zia
bind Zanna.Terminal;

// Draw a 5x5 checkerboard pattern
for row in 0..5 {
    for col in 0..5 {
        if (row + col) % 2 == 0 {
            Print("#");
        } else {
            Print(".");
        }
    }
    Say("");
}
```

Output:
```text
#.#.#
.#.#.
#.#.#
.#.#.
#.#.#
```

### Break and Continue in Nested Loops

`break` and `continue` affect only the innermost loop they're in:

```zia
bind Zanna.Terminal;

for i in 1..=3 {
    for j in 1..=3 {
        if j == 2 {
            break;  // Only breaks inner loop
        }
        Say("i=" + i + ", j=" + j);
    }
    Say("Inner loop ended for i=" + i);
}
```

Output:
```text
i=1, j=1
Inner loop ended for i=1
i=2, j=1
Inner loop ended for i=2
i=3, j=1
Inner loop ended for i=3
```

When j reaches 2, we `break` out of the inner loop - but the outer loop continues with the next value of i.

---

## Common Loop Patterns

Certain loop structures appear again and again in programming. Learning to recognize these patterns makes coding faster and bugs rarer.

### Pattern 1: Counting

Count how many items match a condition.

```zia
bind Zanna.Terminal;

var numbers = [1, 5, 3, 8, 2, 9, 4, 7, 6];
var count = 0;

for i in 0..numbers.Length {
    if numbers[i] > 5 {
        count = count + 1;
    }
}

Say("Found " + count + " numbers greater than 5");  // 4
```

The pattern:
1. Start a counter at 0
2. Loop through items
3. Increment counter when condition matches
4. Counter holds the final count

### Pattern 2: Summing (Accumulating)

Add up values.

```zia
bind Zanna.Terminal;

var total = 0;
for i in 1..=100 {
    total = total + i;
}
Say("Sum of 1 to 100 is " + total);  // 5050
```

The pattern:
1. Start an accumulator at 0 (or 1 for products)
2. Loop through values
3. Add each value to the accumulator
4. Accumulator holds the total

Variations:
```zia
// Sum of squares
var sumOfSquares = 0;
for i in 1..=10 {
    sumOfSquares = sumOfSquares + (i * i);
}

// Product (use 1 as starting value!)
var factorial = 1;
for i in 1..=5 {
    factorial = factorial * i;  // 1*2*3*4*5 = 120
}
```

### Pattern 3: Finding Maximum/Minimum

Find the largest or smallest value.

```zia
bind Zanna.Terminal;

var values = [23, 7, 42, 15, 8, 31];
var max = values[0];  // Start with first element

for i in 1..values.Length {  // Start at 1, we already have values[0]
    if values[i] > max {
        max = values[i];
    }
}

Say("Maximum is " + max);  // 42
```

For minimum, just change `>` to `<`:
```zia
var values = [23, 7, 42, 15, 8, 31];
var min = values[0];
for i in 1..values.Length {
    if values[i] < min {
        min = values[i];
    }
}
```

### Pattern 4: Searching

Find an item that matches a condition.

```zia
bind Zanna.Terminal;

var names = ["Alice", "Bob", "Charlie", "Diana"];
var target = "Charlie";
var foundIndex = -1;  // -1 means "not found"

for i in 0..names.Length {
    if names[i] == target {
        foundIndex = i;
        break;  // Stop searching once found
    }
}

if foundIndex >= 0 {
    Say("Found at index " + foundIndex);
} else {
    Say("Not found");
}
```

### Pattern 5: Filtering

Collect items that match a condition.

```zia
var numbers = [1, 5, 3, 8, 2, 9, 4, 7, 6];
var evens = [];  // Empty array to hold results

for i in 0..numbers.Length {
    if numbers[i] % 2 == 0 {
        evens.Push(numbers[i]);
    }
}

// evens is now [8, 2, 4, 6]
```

### Pattern 6: Transformation

Create new values based on existing ones.

```zia
var numbers = [1, 2, 3, 4, 5];
var squares = [];

for i in 0..numbers.Length {
    squares.Push(numbers[i] * numbers[i]);
}

// squares is [1, 4, 9, 16, 25]
```

### Pattern 7: Building Strings

Construct a string piece by piece.

```zia
bind Zanna.Terminal;

var result = "";
for i in 1..=5 {
    result = result + i + " ";
}
Say(result);  // "1 2 3 4 5 "

// Building with condition
var message = "";
for i in 1..=10 {
    if i % 2 == 0 {
        message = message + i + ",";
    }
}
Say(message);  // "2,4,6,8,10,"
```

### Pattern 8: Validating Input

Keep asking until valid input is provided.

```zia
bind Zanna.Terminal;

var valid = false;
var age = 0;

while !valid {
    Print("Enter your age (0-120): ");
    age = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));

    if age >= 0 && age <= 120 {
        valid = true;
    } else {
        Say("Invalid age. Please try again.");
    }
}

Say("You entered: " + age);
```

This loop has no fixed number of iterations - it depends entirely on when the user provides valid input.

---

## Debugging Loops: Common Bugs and How to Fix Them

Loops are where many beginners encounter their first frustrating bugs. Here are the most common issues and how to diagnose them.

### Bug 1: Off-by-One Errors

The loop runs one too many or one too few times.

**Symptom**: Your loop processes 9 items instead of 10, or 11 instead of 10.

**Common causes**:
- Using `..` when you meant `..=` (or vice versa)
- Starting at 1 when you should start at 0
- Wrong comparison operator (`<` vs `<=`)

```zia
bind Zanna.Terminal;

// Intended: print 1-10
// Bug: prints 1-9
for i in 1..10 {
    Say(i);
}

// Fix: use ..= to include 10
for i in 1..=10 {
    Say(i);
}

// Alternative fix: use 11 as the end
for i in 1..11 {
    Say(i);
}
```

**How to debug**: Print the first and last values. Are they what you expected?

### Bug 2: Infinite Loops

The loop never stops.

**Symptom**: Program hangs or prints the same thing forever.

**Common causes**:
- Forgetting to update the loop variable
- Updating the wrong variable
- Condition that can never become false

The following examples are intentionally broken and expected to time out:

```zia
bind Zanna.Terminal;

// Bug: count never changes
var count = 0;
while count < 10 {
    Say(count);
    // Missing: count = count + 1;
}

// Bug: wrong variable updated
var i = 0;
var j = 0;
while i < 10 {
    j = j + 1;  // Should be i = i + 1
}

// Bug: condition always true
var x = 10;
while x > 0 {
    x = x + 1;  // x gets bigger, never reaches 0
}
```

**How to debug**:
1. Add print statements inside the loop to see what's happening
2. Check: Is the loop variable changing?
3. Check: Is the condition ever becoming false?
4. Trace through manually on paper

### Bug 3: Loop Doesn't Run At All

The loop body never executes.

**Symptom**: Nothing happens, or expected output is missing.

**Common causes**:
- Condition is false from the start
- Empty range
- Wrong direction range

```zia
bind Zanna.Terminal;

// Bug: 10 is not less than 5, so body never runs
var x = 10;
while x < 5 {
    Say("Never prints");
}

// Bug: empty range (5..5 has no elements)
for i in 5..5 {
    Say("Never prints");
}

// Bug: can't count up from 10 to 1 without .rev()
for i in 10..1 {
    Say(i);  // Never prints - empty range
}

// Fix: use reverse
for i in (1..=10).rev() {
    Say(i);
}
```

**How to debug**: Print something before the loop. Print the initial values. Check if the condition is what you think it is.

### Bug 4: Wrong Scope

Variable defined inside loop isn't available outside.

```zia
bind Zanna.Terminal;

for i in 1..=5 {
    var squared = i * i;
}
Say(squared);  // Error! squared doesn't exist here
```

The variable `squared` is created fresh each iteration and discarded at the end of each iteration. It doesn't exist outside the loop.

**Fix**: Declare the variable outside the loop.

```zia
bind Zanna.Terminal;

var lastSquared = 0;
for i in 1..=5 {
    lastSquared = i * i;
}
Say(lastSquared);  // 25
```

### Debugging Strategy

When a loop misbehaves:

1. **Add print statements** at the start of each iteration showing variable values
2. **Trace manually** with pencil and paper for a few iterations
3. **Check your range** - print the start and end values
4. **Verify the condition** - is it what you think?
5. **Check your updates** - is the right variable changing in the right direction?

```zia
bind Zanna.Terminal;

// Debugging version
var count = 0;
while count < 5 {
    Say("DEBUG: count = " + count);  // Add this
    // ... your code ...
    count = count + 1;
    Say("DEBUG: after increment, count = " + count);  // And this
}
```

---

## The Two Languages

**Zia**
```zia
bind Zanna.Terminal;

// while loop
var i = 0;
while i < 5 {
    Say(i);
    i = i + 1;
}

// for loop
for i in 0..5 {
    Say(i);
}

// for with step
for i in (0..10).step(2) {
    Say(i);  // 0, 2, 4, 6, 8
}
```

**BASIC**
```basic
' WHILE loop
DIM i AS INTEGER
i = 0
WHILE i < 5
    PRINT i
    i = i + 1
WEND

' FOR loop
FOR i = 0 TO 4
    PRINT i
NEXT i

' FOR with STEP
FOR i = 10 TO 0 STEP -2
    PRINT i
NEXT i
```

BASIC's `FOR` includes the end value (unlike Zia's `..`). `WEND` ends the `WHILE` block. `STEP` specifies the increment.

Both languages express the same ideas with different syntax. The concepts - checking conditions, iterating ranges, breaking and continuing - are universal.

---

## A Complete Example: Guessing Game

Let's improve our guessing game from Chapter 4 with loops:

```zia
module GuessGame;
bind Zanna.Terminal;

func start() {
    final SECRET = 7;
    final MAX_TRIES = 3;
    var tries = 0;
    var won = false;

    Say("=== Number Guessing Game ===");
    Say("I'm thinking of a number between 1 and 10.");
    Say("You have " + MAX_TRIES + " tries to guess it.");
    Say("");

    // Main game loop
    while tries < MAX_TRIES && !won {
        tries = tries + 1;
        var triesLeft = MAX_TRIES - tries;

        Print("Guess #" + tries + ": ");
        var guess = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));

        if guess == SECRET {
            Say("");
            Say("*** CORRECT! You win! ***");
            Say("You got it in " + tries + " tries!");
            won = true;
        } else if guess < SECRET {
            Say("Too low!");
            if triesLeft > 0 {
                Say("(" + triesLeft + " tries remaining)");
            }
        } else {
            Say("Too high!");
            if triesLeft > 0 {
                Say("(" + triesLeft + " tries remaining)");
            }
        }
        Say("");
    }

    if !won {
        Say("Game over! The number was " + SECRET);
        Say("Better luck next time!");
    }
}
```

Sample run (player wins):
```text
=== Number Guessing Game ===
I'm thinking of a number between 1 and 10.
You have 3 tries to guess it.

Guess #1: 5
Too low!
(2 tries remaining)

Guess #2: 8
Too high!
(1 tries remaining)

Guess #3: 7

*** CORRECT! You win! ***
You got it in 3 tries!
```

Sample run (player loses):
```text
=== Number Guessing Game ===
I'm thinking of a number between 1 and 10.
You have 3 tries to guess it.

Guess #1: 2
Too low!
(2 tries remaining)

Guess #2: 3
Too low!
(1 tries remaining)

Guess #3: 4
Too low!

Game over! The number was 7
Better luck next time!
```

The loop condition `tries < MAX_TRIES && !won` captures both exit conditions: we stop if we run out of tries OR if we win. The `!won` (not won) is true as long as we haven't won; it becomes false when we win, causing the loop to exit.

---

## Summary

- **`while` loops** repeat as long as a condition is true - use when you don't know how many iterations
- **`for` loops** iterate over a range of values - use when you know how many iterations
- **Ranges**: `..` excludes the end, `..=` includes it
- **`.rev()`** reverses a range for counting down
- **`.step(n)`** iterates by increments of n
- **`break`** exits a loop immediately
- **`continue`** skips to the next iteration
- **Nested loops**: iterations multiply - an inner loop runs fully for each outer iteration
- **Common patterns**: counting, summing, finding max/min, searching, filtering, transforming
- **Infinite loops** run forever - intentional for game loops, a bug otherwise
- **Off-by-one errors** are the most common loop bug - check your ranges carefully

---

## Exercises

**Exercise 5.1**: Write a program that prints the numbers 1 to 20 using a `while` loop, then again using a `for` loop. Verify both produce the same output.

**Exercise 5.2**: Write a program that prints all multiples of 3 between 1 and 50. Try two approaches: using `continue` to skip non-multiples, and using `.step(3)`.

**Exercise 5.3**: Write a program that asks for a number N and prints the first N numbers of the Fibonacci sequence (1, 1, 2, 3, 5, 8, 13, ...). Each number is the sum of the two before it. For example, if N is 7, print: 1 1 2 3 5 8 13

**Exercise 5.4**: Write a program that asks for a positive integer and prints whether it's prime. A prime number is only divisible by 1 and itself. (Hint: loop from 2 to n-1 and check if any number divides n evenly. Use `break` when you find a divisor.)

**Exercise 5.5**: Print this pattern using nested loops:
```text
*
**
***
****
*****
```

**Exercise 5.6**: Write a program that calculates the sum of all even numbers between 1 and 100. Then calculate the sum of all odd numbers. Print both sums.

**Exercise 5.7**: Write a program that asks for a number and prints its multiplication table (1x through 10x). For example, if the user enters 7:
```text
7 x 1 = 7
7 x 2 = 14
7 x 3 = 21
...
7 x 10 = 70
```

**Exercise 5.8**: Write a number guessing game where the computer has unlimited guesses. The computer should guess systematically (perhaps using a counter from 1 to 10) and stop when it guesses correctly. (This is the reverse of our example - the user picks a number, the computer guesses.)

**Exercise 5.9** (Challenge): Print this pattern:
```text
    *
   ***
  *****
 *******
*********
```
(Hint: For each row, first print some spaces, then print some stars. Both quantities change based on the row number.)

**Exercise 5.10** (Challenge): Write a program that finds all prime numbers between 1 and 100. You'll need nested loops - an outer loop for each number to test, and an inner loop to check if it's prime.

---

*We can now repeat actions efficiently. But we've been working with individual values - one number at a time, one string at a time. What if we have a hundred numbers? A thousand names? Next, we learn about collections - ways to organize and work with groups of values.*

*[Continue to Chapter 6: Collections](06-collections.md)*
