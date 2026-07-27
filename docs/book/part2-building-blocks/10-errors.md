---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 10: Errors and Recovery

Here's a secret that every experienced programmer knows but nobody talks about: we see error messages *constantly*. Not occasionally. Not just when we're learning. Every single day.

If you've been following along with this book and typing code, you've already seen errors. Maybe you forgot a semicolon. Maybe you misspelled a variable name. Maybe you tried to access an array element that didn't exist. And when that happened, you might have felt a flash of frustration, or embarrassment, or even wondered if programming is right for you.

Stop. Take a breath. Those errors are *completely normal*.

The difference between a beginner and an expert isn't that experts don't make errors — it's that experts have learned to work *with* errors instead of against them. They read error messages like helpful hints rather than accusations. They treat debugging as a puzzle to solve rather than a judgment on their intelligence tangentially, debugging can be fun once you develop the right mindset.

This chapter will teach you to embrace errors. You'll learn what causes them, how to read them, how to handle them gracefully in your programs, and how to find and fix bugs when they appear. By the end, you won't just tolerate errors — you'll see them as valuable tools for building better software.

---

## Errors Are Normal

Let's be absolutely clear about this: *every programmer gets errors*. Not just beginners. Not just on difficult problems. Everyone, everywhere, all the time.

Professional developers at major tech companies see error messages dozens or hundreds of times per day. The code they ship has bugs. The libraries they use have bugs. The compilers and interpreters have bugs. Bugs are a fundamental, unavoidable part of software development.

Why? Because programming is complex. You're translating human intentions into precise instructions for a machine that does exactly what you tell it — not what you meant. That translation is hard. Mistakes are inevitable.

The goal isn't to never make mistakes. The goal is to:
1. Catch mistakes quickly
2. Understand what went wrong
3. Fix the problem
4. Learn from it

Error messages exist to help you with step 2. They're not criticism — they're information. A good error message is like a friend tapping you on the shoulder and saying, "Hey, I think you meant to do this differently."

---

## The Three Types of Errors

Not all errors are the same. Understanding the different types helps you diagnose and fix them faster.

### Syntax Errors: Breaking the Rules

Syntax errors happen when your code violates the rules of the language. It's like writing a sentence with the words in the wrong order — even if your meaning is clear to a human, the compiler can't understand it.

```zia
bind Zanna.Terminal;

// Missing semicolon
var x = 10
var y = 20;

// Mismatched parentheses
if (x > 5 {
    Say("hello");
}

// Misspelled keyword
funktion add(a: Integer, b: Integer) -> Integer {
    return a + b;
}

// Unclosed String
var message = "Hello, world!;
```

Syntax errors are the easiest type to fix because:
- The compiler catches them before your program even runs
- The error message usually points to the exact line (or close to it)
- The fix is usually obvious once you see the problem

**How to spot them:** Your program won't even start. The compiler immediately complains.

**How to fix them:** Read the error message carefully. Look at the line number. Check for typos, missing punctuation, unclosed brackets, and mismatched quotes.

### Runtime Errors: Impossible Requests

Runtime errors happen while your program is running. The syntax is valid — the compiler accepts your code — but when the program executes, something goes wrong.

```zia
var x = 10 / 0;  // Division by zero — mathematically undefined

var arr = [1, 2, 3];
var y = arr[10];  // Index 10 doesn't exist

var text: String = null;
var len = text.Length;  // Can't access properties of null

bind Convert = Zanna.Core.Convert;
var num = Convert.ToInt64("hello");  // "hello" is not a number
```

Runtime errors occur because the code is syntactically correct but asks the computer to do something impossible. You can't divide by zero. You can't access an array element that doesn't exist. These operations make no sense.

**How to spot them:** Your program starts but crashes in the middle with an error message.

**How to fix them:** The error message tells you what went wrong. Think about *why* that operation was impossible. Usually, you need to add a check before the dangerous operation or handle the error with try-catch (which we'll cover soon).

### Logic Errors: Wrong but Valid

Logic errors are the sneakiest. Your code is syntactically correct. It runs without crashing. But it produces the wrong result.

```zia
// Trying to calculate the average
func average(a: Integer, b: Integer) -> Integer {
    return a + b / 2;  // Oops! Division happens before addition
    // Should be: return (a + b) / 2;
}

// Trying to check if a number is between 1 and 10
func inRange(n: Integer) -> Boolean {
    return n > 1 && n < 10;  // Should be >= 1 and <= 10
    // Numbers 1 and 10 are incorrectly excluded
}

// Off-by-one error in a loop
var sum = 0;
for i in 0..10 {  // This counts 0-9, not 1-10
    sum += i;
}
// sum is 45, not 55
```

Logic errors don't produce error messages. The computer happily does exactly what you told it to do — it's just not what you *meant*.

**How to spot them:** Your program runs but gives unexpected results. Tests fail. Users report bugs. You look at the output and think "that's not right."

**How to fix them:** These require detective work. Print out intermediate values. Step through the code manually. Compare what you *expected* to happen with what *actually* happened. We'll cover debugging strategies in detail later in this chapter.

---

## Reading Error Messages

Error messages often look intimidating. They're full of technical jargon, file paths, line numbers, and sometimes even unfamiliar symbols. But they're structured and, once you know how to read them, genuinely helpful.

Let's dissect a typical error message:

```text
Error: IndexOutOfBounds at main.zia:23:15
  Array index 10 is out of range for array of length 3

    21 |     var numbers = [1, 2, 3];
    22 |     var total = 0;
  > 23 |     var value = numbers[10];
       |                        ^^
    24 |     total += value;

  in function 'processData' at main.zia:23
  called from 'start' at main.zia:45
```

This message tells you:

1. **Error type:** `IndexOutOfBounds` — you tried to access an array position that doesn't exist.

2. **Location:** `main.zia:23:15` — file `main.zia`, line 23, column 15. This is exactly where the problem occurred.

3. **Description:** `Array index 10 is out of range for array of length 3` — you tried to access index 10, but the array only has indices 0, 1, and 2.

4. **Context:** The surrounding code is shown, with the problematic line highlighted and the specific part marked with `^^`.

5. **Call stack:** The error occurred in `processData`, which was called from `start`. This helps you understand how the program got here.

### Error Message Reading Strategy

When you see an error, follow this process:

1. **Read the type first.** `DivisionByZero`, `FileNotFound`, `TypeError`, `NullPointer` — the type immediately tells you the category of problem.

2. **Find the line number.** Go to that exact line in your code.

3. **Read the description.** It often tells you exactly what's wrong. "Array index 10 is out of range" is very specific.

4. **Look at the context.** Sometimes the bug is actually on a previous line. If line 23 tries to use a variable, but line 20 set it incorrectly, the error appears on 23 but the fix is on 20.

5. **Check the call stack.** If the error is in a function, who called it? Maybe the function is correct, but it's being called with wrong arguments.

### The Error Is Not Always Where It Appears

This is crucial: the line where an error occurs is not always the line where the bug lives.

```zia
bind Zanna.Terminal;

func processUser(user: User) {
    Say(user.name);  // ERROR: null pointer on line 5
}

func start() {
    var user: User = null;  // The actual bug is here on line 9
    // ... lots of code ...
    processUser(user);  // Passed a null to the function
}
```

The error message says "null pointer on line 5" because that's where the program crashes. But the real problem is line 9, where `user` was set to null. Always trace back through your code to find the *root cause*, not just the symptom.

---

## The try-catch Statement

Now that you understand what errors are and how to read error messages, let's learn to handle them in code. The `try-catch` statement lets your program respond to errors gracefully instead of crashing.

### Basic Structure

```zia
bind Zanna.Terminal;

try {
    // Code that might fail
    var x = 10 / y;
    Say("Result: " + x);
} catch {
    // Code that runs if an error occurred
    Say("Something went wrong: " + "an error occurred");
}

Say("Program continues...");
```

Here's how it works:

1. The program enters the `try` block and executes each statement in order
2. If all statements succeed, the `catch` block is skipped entirely
3. If any statement throws an error, execution *immediately* jumps to the `catch` block
4. After either path, the program continues with the code after try-catch

The `e` variable in `catch(e)` gives you access to information about the error — its type, message, and where it occurred.

### What Gets Tried, What Gets Caught

The `try` block should contain the "risky" code — the operations that might fail. When an error occurs, *nothing after that point in the try block runs*:

```zia
bind Zanna.Terminal;

try {
    Say("Step 1");
    var x = 10 / 0;                    // Error here!
    Say("Step 2");      // Never runs
    Say("Step 3");      // Never runs
} catch {
    Say("Caught: " + "an error occurred");
}
Say("Done");
```

Output:
```text
Step 1
Caught: Division by zero
Done
```

Step 2 and Step 3 never execute. The moment division by zero is attempted, execution jumps directly to the catch block.

### Multiple Things Can Fail

A single try block might have multiple things that could go wrong:

```zia
bind File = Zanna.IO.File;
bind Convert = Zanna.Core.Convert;
bind Zanna.Terminal;

try {
    var content = File.ReadAllText(filename);  // Could fail: file missing
    var value = Convert.ToInt64(content);        // Could fail: not a number
    var result = 1000 / value;         // Could fail: zero
    Say("Result: " + result);
} catch {
    Say("Error: " + "an error occurred");
}
```

If *any* of these three operations fails, the catch block runs. A plain
`catch` handles every runtime error:

```zia
bind File = Zanna.IO.File;
bind Convert = Zanna.Core.Convert;
bind Zanna.Terminal;

try {
    var content = File.ReadAllText(filename);
    var value = Convert.ToInt64(content);
    var result = 1000 / value;
    Say("Result: " + result);
} catch {
    Say("Something went wrong: " + "an error occurred");
}
```

When you need different handling for different failure modes, Zia also supports
typed catches for runtime error kinds such as `DivideByZero`, `Bounds`,
string error messages:

```zia
bind File = Zanna.IO.File;
bind Convert = Zanna.Core.Convert;
bind Zanna.Terminal;

try {
    var content = File.ReadAllText(filename);
    var value = Convert.ToInt64(content);
    var result = 1000 / value;
    Say("Result: " + result);
} catch(e: DivideByZero) {
    Say("The file contained zero.");
} catch(e: IOError) {
    Say("Could not read the file.");
} catch {
    Say("Something else went wrong.");
}
```

If you want fine-grained recovery without multiple typed catches, structuring
your code so that each risky operation is in its own try-catch is still a good
approach:

```zia
bind File = Zanna.IO.File;
bind Convert = Zanna.Core.Convert;
bind Zanna.Terminal;

var filename = "number.txt";
var content = "";
try {
    content = File.ReadAllText(filename);
} catch {
    Say("File doesn't exist: " + filename);
    return;
}

var value = 0;
try {
    value = Convert.ToInt64(content);
} catch {
    Say("File doesn't contain a valid number");
    return;
}

if value == 0 {
    Say("The file contained zero");
} else {
    var result = 1000 / value;
    Say("Result: " + result);
}
```

This pattern gives you fine-grained control over each failure point.

### When to Use try-catch

Use try-catch when:

1. **External resources might fail:** Files, networks, databases — anything outside your program's control
2. **User input might be invalid:** Parsing numbers, dates, or structured data
3. **You can meaningfully recover:** There's something useful you can do besides crash
4. **You want to log or report the error:** Even if you can't recover, you might want to record what happened

Don't use try-catch when:

1. **You can check beforehand:** If you can prevent the error with an `if` statement, that's usually cleaner
2. **You can't handle the error:** If there's nothing useful to do, let it propagate (more on this soon)
3. **For normal control flow:** Exceptions are for exceptional situations, not everyday logic

```zia
// Unnecessary try-catch
var arr = [10, 20, 30];
var index = 5;
var defaultValue = 0;
var value = 0;

try {
    value = arr[index];
} catch {
    value = defaultValue;
}

// Better: check first
if index >= 0 && index < arr.Length {
    value = arr[index];
} else {
    value = defaultValue;
}
```

---

## Understanding the Call Stack

When an error occurs, the error message often includes a *stack trace* or *call stack*. This shows the chain of function calls that led to the error.

```text
Error: DivisionByZero at calculations.zia:15:20
  Cannot divide by zero

  in function 'divide' at calculations.zia:15
  called from 'computeAverage' at calculations.zia:28
  called from 'processScores' at main.zia:42
  called from 'start' at main.zia:8
```

Read this from top to bottom: the error occurred in `divide`, which was called by `computeAverage`, which was called by `processScores`, which was called by `start`.

### Why the Stack Matters

The stack trace is your roadmap for debugging. It answers:

- **Where exactly did it fail?** In `divide`, line 15
- **What was the program doing?** Computing an average of some scores
- **How did it get there?** Through the chain of function calls

Often, the bug isn't in the function where the error occurred. Look back through the chain:

```zia
func divide(a: Integer, b: Integer) -> Integer {
    return a / b;  // Error: division by zero
}

func computeAverage(numbers: List[Integer]) -> Integer {
    var sum = 0;
    for n in numbers {
        sum += n;
    }
    return divide(sum, numbers.Length);  // BUG: empty array has length 0!
}

func processScores(data: String) {
    bind Zanna.Terminal;
    var scores = parseScores(data);
    var avg = computeAverage(scores);  // Called with empty array
    Say("Average: " + avg);
}
```

The error appears in `divide`, but the bug is in `computeAverage` (or maybe `processScores`, which should check for empty data). The stack trace helps you trace the path and find where things went wrong.

### Reading Stack Traces Effectively

1. **Start at the top** — that's where the error actually occurred
2. **Work your way down** — at each level, ask "what was passed to this function?"
3. **Find the level where the bug is** — usually it's not the top, but somewhere in the chain
4. **Look at the data** — what values were being processed when things went wrong?

---

## The finally Block

Sometimes you need to run cleanup code no matter what happens — whether the try block succeeds or fails:

```zia
bind Zanna.Terminal;

var connectionOpen = true;

try {
    Say("Reading users");
} catch {
    Say("Database error: " + "an error occurred");
} finally {
    connectionOpen = false;  // Always runs, error or not
    Say("Connection closed");
}
```

The `finally` block is guaranteed to run:
- If the try block completes successfully, finally runs after it
- If the try block throws an error that's caught, finally runs after the catch
- If the try block throws an error that's *not* caught, finally runs before the error propagates

Use `finally` for:
- Closing files and database connections
- Releasing locks
- Cleaning up temporary resources
- Any action that must happen regardless of success or failure

---

## Throwing Your Own Errors

You can signal errors from your own code using `throw`:

```zia
func validateAge(age: Integer) -> Integer {
    if age < 0 {
        throw ("Age cannot be negative: " + age);
    }
    if age > 150 {
        throw ("Age is unrealistically high: " + age);
    }
    return age;
}
```

When your code encounters a situation it can't handle, throw an error. This:
- Stops execution immediately
- Sends the error up the call stack until something catches it
- Provides information about what went wrong

### Descriptive Error Messages

Since Zia uses a single `Error` type, write clear, descriptive messages that explain what went wrong:

```zia
var input = "abc";

throw ("File not found: config.txt");  // Error: example throw
throw ("Expected number, got: " + input);  // Error: example throw
throw ("Email address is invalid");  // Error: example throw
```

Include relevant context in the message so the caller knows what happened:

```zia
bind Zanna.Terminal;

func loadConfiguration() {
    throw ("missing configuration");
}

func useDefaults() {
    Say("Defaults loaded");
}

func start() {
    try {
        loadConfiguration();
    } catch {
        Say("Configuration error: " + "an error occurred");
        Say("Using defaults instead");
        useDefaults();
    }
}
```

---

## Defensive Programming

The best error handling is *preventing* errors in the first place. Defensive programming means anticipating what could go wrong and addressing it before it becomes an error.

### Validate Inputs Early

Check that inputs are valid before using them:

```zia
func processOrder(quantity: Integer, price: Number) {
    // Validate at the start
    if quantity <= 0 {
        throw ("Quantity must be positive");
    }
    if price < 0 {
        throw ("Price cannot be negative");
    }
    if price > 1000000 {
        throw ("Price exceeds maximum allowed");
    }

    // Now we know the inputs are valid
    var total = quantity * price;
    // ...
}
```

This is called "fail fast" — if something's wrong, you want to know immediately, not after half the function has already run.

### Check Before Acting

Before performing dangerous operations, verify they'll succeed:

```zia
var arr = [10, 20, 30];
var index = 1;
var a = 12;
var b = 3;

// Instead of:
var value = arr[index];  // Might crash

// Do this:
if index >= 0 && index < arr.Length {
    var value = arr[index];
} else {
    // Handle the out-of-bounds case
}

// Instead of:
var result = a / b;  // Might crash

// Do this:
if b != 0 {
    var result = a / b;
} else {
    // Handle division by zero
}
```

### Provide Defaults

When missing values are acceptable, use defaults:

```zia
func getConfig(key: String, defaultValue: String) -> String {
    if key == "timeout" {
        return "60";
    }
    return defaultValue;
}

// Usage
var timeout = getConfig("timeout", "30");
var logLevel = getConfig("logLevel", "info");
```

### Use Guard Clauses

Handle error cases at the top of functions, then proceed with the main logic:

```zia
func processFile(filename: String) {
    bind File = Zanna.IO.File;
    // Guard clauses handle all the edge cases
    if filename.Length == 0 {
        throw "Filename cannot be empty";
    }

    if !File.Exists(filename) {
        throw ("File does not exist: " + filename);
    }

    if !filename.EndsWith(".txt") {
        throw ("Only .txt files are supported");
    }

    // Main logic — we know the file is valid
    var content = File.ReadAllText(filename);
    // ...
}
```

Guard clauses make code easier to read because all the error handling is at the top, and the "happy path" is clear.

---

## Error Propagation: Catch or Let It Bubble?

When an error occurs, you have two choices:
1. Catch it and handle it
2. Let it propagate up to the caller

### When to Catch

Catch an error when:

- You can recover meaningfully
- You need to translate it to a different error
- You need to log it but still continue
- You're at the top level and need to report to the user

```zia
bind File = Zanna.IO.File;

func queryUser(id: Integer) -> String {
    return "user-" + id;
}

func sendNotification(item: String) {
    throw ("notification service unavailable");
}

func log(message: String) {
    // Write to a log file in a real program.
}

// Recovery: try a backup file
func loadData() -> String {
    try {
        return File.ReadAllText("data.txt");
    } catch {
        return File.ReadAllText("data.backup.txt");
    }
}

// Translation: hide implementation details
func getUser(id: Integer) -> String {
    try {
        return queryUser(id);
    } catch {
        throw ("Unable to load user " + id + ": " + "an error occurred");
    }
}

// Logging but continuing
func processItem(item: String) {
    try {
        sendNotification(item);
    } catch {
        log("Failed to send notification: " + "an error occurred");
        // Continue anyway — notification failure isn't fatal
    }
}
```

### When to Propagate

Let errors propagate when:

- You can't do anything useful about it
- The caller is better positioned to handle it
- It's a programming error (bug) that should crash

```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

func parseConfig(content: String) -> String {
    return content.Trim();
}

func runApplication(config: String) {
    Say("Running with config: " + config);
}

// Can't handle it — let it propagate
func loadConfig() -> String {
    var content = File.ReadAllText("config.txt");  // Might fail
    return parseConfig(content);  // Might fail
    // If either fails, we can't proceed — let it bubble up
}

// Caller handles it
func start() {
    try {
        var config = loadConfig();
        runApplication(config);
    } catch {
        Say("Failed to start: " + "an error occurred");
    }
}
```

The principle: handle errors at the level that has enough context to deal with them properly.

---

## Debugging Strategies

Logic errors don't produce error messages — they produce wrong results. Finding and fixing them requires detective work. Here are proven strategies.

### Print Debugging

The simplest and most widely used technique: add print statements to see what's happening inside your code.

```zia
bind Zanna.Terminal;

func mysteriouslyWrongResult(data: List[Integer]) -> Integer {
    Say("DEBUG: Input data, length = " + data.Length);

    var sum = 0;
    for i in 0..data.Length {
        Say("DEBUG: i = " + i + ", data[i] = " + data[i]);
        sum += data[i];
        Say("DEBUG: sum is now " + sum);
    }

    var result = sum / data.Length;
    Say("DEBUG: final result = " + result);
    return result;
}
```

Print debugging works because it shows you exactly what your code is doing, step by step. Often, the bug becomes obvious when you see the actual values.

Tips for print debugging:
- Label your output so you know what each value represents
- Print at key decision points (before and after conditionals, at loop iterations)
- Print the values you're *assuming* are correct — often they're not
- Remove or comment out debug prints when you're done

### Rubber Duck Debugging

This sounds silly but works remarkably well: explain your code, line by line, to an inanimate object. A rubber duck. A stuffed animal. A plant. Anything.

The act of explaining forces you to think through each step carefully. You can't skip over parts when you're explaining — you have to articulate exactly what each line does.

"First, I get the array of numbers. Then I initialize sum to zero. Then for each number... wait. I'm starting at 1, not 0. Oh, that's the bug!"

This works because:
- You often make assumptions you're not aware of
- Explaining forces you to examine those assumptions
- The "audience" doesn't need to respond — the value is in your explanation

Many experienced programmers keep a rubber duck on their desk for exactly this purpose.

### Binary Search Debugging

When you have a lot of code and don't know where the bug is, use binary search: narrow down the location by halving the search space.

1. Find a point roughly in the middle of your code
2. Add a check: is the data correct at this point?
3. If yes, the bug is after this point
4. If no, the bug is before this point
5. Repeat, narrowing down each time

```text
bind Zanna.Terminal;

func complexProcessing(input: Data) -> Result {
    var a = stepOne(input);
    var b = stepTwo(a);
    var c = stepThree(b);

    // CHECKPOINT: Is the data correct here?
    Say("DEBUG mid-point: " + c);  // Print value for inspection

    var d = stepFour(c);
    var e = stepFive(d);
    var f = stepSix(e);
    return f;
}
```

If the data is wrong at the checkpoint, the bug is in steps 1-3. If it's correct, the bug is in steps 4-6. Then add another checkpoint to narrow further.

This is much faster than checking every line, especially in large codebases.

### The Scientific Method

Treat debugging like science:

1. **Observe:** What exactly is wrong? Get specific.
2. **Hypothesize:** What might cause this behavior?
3. **Experiment:** Add code to test your hypothesis
4. **Analyze:** Did the experiment confirm or refute your hypothesis?
5. **Repeat:** If refuted, form a new hypothesis

Example:
- **Observation:** The total is 45 but should be 55
- **Hypothesis:** Maybe the loop isn't including the last element
- **Experiment:** Print the loop variable at each iteration
- **Analysis:** The loop goes 0-9, but I expected 1-10. Confirmed!
- **Fix:** Change the loop bounds

### Simplify the Problem

If a bug is hard to find in complex code, create a simpler version that still shows the bug:

```text
// Complex original
func processTransactions(accounts: List[Account], transactions: List[Transaction]) -> Report {
    // 200 lines of code
    // Bug is somewhere in here
}

// Simplified test
func testSimple() {
    bind Zanna.Terminal;
    var accounts = [Account.new(100)];  // One account
    var transactions = [Transaction.new(50)];  // One transaction
    var report = processTransactions(accounts, transactions);
    Say(report);  // Report class should override toString() for display
}
```

With simple inputs, you can trace through the code manually and spot where things go wrong.

---

## Real-World Error Scenarios

Let's look at errors you'll encounter in real programs and how to handle them.

### Network Failures

Networks are inherently unreliable. Connections drop, servers go down, requests time out.

```zia
bind Zanna.Terminal;
bind Zanna.Time;

func fetchUserData(userId: Integer) -> UserData {
    var maxRetries = 3;
    var retryDelay = 1000;  // milliseconds

    for attempt in 1..maxRetries+1 {
        try {
            return HttpClient.Get("https://api.example.com/users/" + userId);
        } catch {
            if attempt < maxRetries {
                Say("Network error, retrying in " + retryDelay + "ms...");
                Time.Clock.Sleep(retryDelay);
                retryDelay *= 2;  // Exponential backoff
            } else {
                throw ("Failed to fetch user after " + maxRetries + " attempts");
            }
        }
    }
}
```

Key strategies for network errors:
- **Retry with backoff:** Try again, but wait longer between each attempt
- **Timeout:** Don't wait forever for a response
- **Fallback:** Use cached data or a default if the network is unavailable
- **Graceful degradation:** Continue with reduced functionality

### User Input Errors

Users will enter invalid data. Always. Count on it.

```zia
bind Zanna.Terminal;
bind Convert = Zanna.Core.Convert;
bind Parse = Zanna.Core.Parse;

func getValidAge() -> Integer {
    while true {
        Print("Enter your age: ");
        var input = TryReadLine().UnwrapOrStr("").Trim();

        if !Parse.IsInt(input) {
            Say("That's not a valid number. Please try again.");
            continue;
        }

        var age = Convert.ToInt64(input);
        if age < 0 || age > 150 {
            Say("Please enter a realistic age (0-150)");
            continue;
        }

        return age;
    }

    return 0;
}
```

Principles for user input:
- **Never trust input:** Always validate
- **Give clear feedback:** Tell users what went wrong and how to fix it
- **Allow retry:** Don't crash; let users try again
- **Sanitize:** Remove or escape dangerous characters

### Resource Exhaustion

Programs can run out of resources: memory, disk space, file handles, network connections.

```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

func writeReport(content: String) {
    var tempFile = "report.txt.tmp";

    try {
        File.WriteAllText(tempFile, content);
        File.MoveOver(tempFile, "report.txt");
    } catch {
        Say("Could not write report");
    } finally {
        if File.Exists(tempFile) {
            File.Delete(tempFile);
        }
    }
}
```

Resource management tips:
- **Process in chunks:** Don't load everything into memory
- **Release resources promptly:** Close files and connections when done
- **Monitor usage:** Log resource consumption in production
- **Set limits:** Cap the size of inputs, queues, caches

### Missing Files and Configuration

Programs often depend on files that might not exist:

```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

func loadConfiguration() -> Config {
    final CONFIG_FILE = "config.json";
    final DEFAULT_CONFIG = Config.new(
        timeout: 30,
        logLevel: "info",
        maxRetries: 3
    );

    if !File.Exists(CONFIG_FILE) {
        Say("Config file not found, creating default...");
        File.WriteAllText(CONFIG_FILE, DEFAULT_CONFIG.toJson());
        return DEFAULT_CONFIG;
    }

    try {
        var content = File.ReadAllText(CONFIG_FILE);
        return Config.fromJson(content);
    } catch {
        Say("Config file is corrupted, using defaults");
        return DEFAULT_CONFIG;
    }
}
```

File handling strategies:
- **Check existence:** Before reading, verify the file is there
- **Provide defaults:** Have sensible fallback values
- **Create if missing:** For config files, create a default version
- **Validate content:** Files can exist but contain garbage

---

## A Complete Example: Robust Data Processor

Let's put it all together with a program that demonstrates comprehensive error handling:

```zia
module DataProcessor;

bind File = Zanna.IO.File;
bind Zanna.Terminal;
bind Zanna.Time.DateTime as DateTime;
bind Convert = Zanna.Core.Convert;

final LOG_FILE = "processor.log";

func log(message: String) {
    var ts = DateTime.Now();
    var timestamp = Convert.ToStringInt(ts);
    var entry = "[" + timestamp + "] " + message + "\n";

    try {
        File.Append(LOG_FILE, entry);
    } catch {
        // If we can't log, print to console at least
        Say("LOG: " + message);
    }
}

func isInteger(text: String) -> Boolean {
    if text.Length == 0 {
        return false;
    }

    for i in 0..text.Length {
        var c = text[i];
        if c.Asc() < "0".Asc() || c.Asc() > "9".Asc() {
            return false;
        }
    }

    return true;
}

func readDataFile(filename: String) -> List[Integer] {
    if filename.Length == 0 {
        throw "Filename cannot be empty";
    }

    if !File.Exists(filename) {
        throw "Data file not found: " + filename;
    }

    var lines = File.ReadAllLines(filename);
    var numbers: List[Integer] = [];
    var lineNum = 0;

    for line in lines {
        lineNum += 1;
        var trimmed = line.Trim();

        // Skip empty lines and comments
        if trimmed.Length == 0 || trimmed.StartsWith("#") {
            continue;
        }

        if isInteger(trimmed) {
            var num = Convert.ToInt64(trimmed);
            numbers.add(num);
        } else {
            log("Warning: skipping invalid number on line " + lineNum + ": " + trimmed);
        }
    }

    if numbers.count() == 0 {
        throw "No valid numbers found in " + filename;
    }

    return numbers;
}

func printStats(filename: String, numbers: List[Integer]) {
    if numbers.count() == 0 {
        throw "Cannot calculate stats on an empty list";
    }

    var sum: Integer = 0;
    var min = numbers[0];
    var max = numbers[0];

    for n in numbers {
        sum += n;
        if n < min { min = n; }
        if n > max { max = n; }
    }

    var average = sum / numbers.count();

    Say("=== Statistics for " + filename + " ===");
    Say("Count:   " + numbers.count());
    Say("Sum:     " + sum);
    Say("Min:     " + min);
    Say("Max:     " + max);
    Say("Average: " + average);
}

func processFile(filename: String) {
    log("Processing file: " + filename);

    try {
        var numbers = readDataFile(filename);
        log("Loaded " + numbers.count() + " numbers");

        printStats(filename, numbers);

        log("Successfully processed " + filename);

    } catch {
        Say("Error: " + "an error occurred");
        Say("Please check the file and try again.");
        log("Error processing " + filename + ": " + "an error occurred");
    }
}

func start() {
    Say("Data Processor");
    Say("==============");
    Say("");

    while true {
        Print("Enter filename (or 'quit' to exit): ");
        var input = TryReadLine().UnwrapOrStr("").Trim();

        if input.ToLower() == "quit" {
            Say("Goodbye!");
            break;
        }

        processFile(input);
        Say("");
    }
}
```

This program demonstrates:
- **Input validation:** Checking for empty filenames, verifying files exist
- **Graceful degradation:** Skipping invalid lines instead of failing entirely
- **Logging:** Recording what happens for later analysis
- **Error handling:** Catching errors and reporting them clearly
- **User feedback:** Clear messages about what went wrong
- **Defensive programming:** Checking for empty arrays before processing

---

## The Two Languages

### Zia
```zia
bind Zanna.Terminal;

func riskyOperation() {
    throw ("example failure");
}

func cleanup() {
    Say("Cleaned up");
}

func demo() {
    try {
        riskyOperation();
    } catch {
        Say("Error: " + "an error occurred");
    } finally {
        cleanup();
    }
}

func failExample() {
    throw ("Something went wrong");
}
```

### BASIC
```basic
ON ERROR GOTO ErrorHandler

' Risky code here
OPEN "file.txt" FOR INPUT AS #1
' ...
GOTO Continue

ErrorHandler:
PRINT "Error: "; ERR; " - "; ERROR$
RESUME Continue

Continue:
' Continue execution
```

BASIC uses `ON ERROR GOTO` for older-style error handling. The `ERR` variable contains the error number, and `ERROR$` contains the message.

---

## Common Mistakes

### Empty Catch Blocks

The worst thing you can do with exceptions:

```zia
// TERRIBLE: Silently ignoring errors
func riskyOperation() {
    throw ("operation failed");
}

func start() {
    try {
        riskyOperation();
    } catch {
        // Nothing here — errors vanish without a trace
    }
}
```

This makes debugging nearly impossible. If something goes wrong, you'll never know. At minimum:

```zia
func riskyOperation() {
    throw ("operation failed");
}

func log(message: String) {
    // Write to a log file in a real program.
}

func start() {
    try {
        riskyOperation();
    } catch {
        log("Error ignored: " + "an error occurred");
    }
}
```

### Catching Too Broadly

Catching everything in one block can hide which operation failed:

```zia
bind Zanna.Terminal;

func complexOperation(data: String) -> String {
    return data.ToUpper();
}

func saveResult(result: String) {
    // Save in a real program.
}

func sendNotification(result: String) {
    // Notify in a real program.
}

func start() {
    var data = "example";
    try {
        var result = complexOperation(data);
        saveResult(result);
        sendNotification(result);
    } catch {
        Say("Something went wrong");  // Which operation? What error?
    }
}
```

Better -- separate the operations so you know which one failed:

```zia
bind Zanna.Terminal;

func complexOperation(data: String) -> String {
    return data.ToUpper();
}

func saveResult(result: String) {
    // Save in a real program.
}

func sendNotification(result: String) {
    // Notify in a real program.
}

func log(message: String) {
    // Write to a log file in a real program.
}

func start() {
    var data = "example";
    var result = complexOperation(data);  // Let validation errors propagate

    try {
        saveResult(result);
    } catch {
        Say("Could not save: " + "an error occurred");
        return;
    }

    try {
        sendNotification(result);
    } catch {
        log("Notification failed: " + "an error occurred");
        // Notification failure is not fatal, continue
    }
}
```

### Using Exceptions for Control Flow

Exceptions should be exceptional — not a normal part of logic:

```text
// BAD: Using exceptions as control flow
func findUser(name: String) -> User {
    for user in users {
        if user.name == name {
            throw ("found");  // Abuse!
        }
    }
    throw ("not found");
}

// GOOD: Normal control flow
func findUser(name: String) -> User? {
    for user in users {
        if user.name == name {
            return user;
        }
    }
    return null;
}
```

Exceptions are slow compared to normal returns. Use them for errors, not everyday logic.

### Losing Error Information

When catching and re-throwing, preserve the original error:

```zia
func loadData() {
    throw ("disk error");
}

// BAD: Original error is lost
func badExample() {
    try {
        loadData();
    } catch {
        throw ("Data loading failed");
    }
}

// GOOD: Chain the original error
func goodExample() {
    try {
        loadData();
    } catch {
        throw ("Data loading failed: " + "an error occurred");
    }
}
```

---

## Summary

- **Errors are normal** — every programmer encounters them constantly. Don't be discouraged.
- **Three types of errors:** Syntax (won't compile), runtime (crashes while running), logic (wrong results)
- **Read error messages carefully:** They contain type, location, description, and call stack
- **try-catch handles errors gracefully:** Prevents crashes, allows recovery
- **finally ensures cleanup:** Runs no matter what happens
- **throw signals errors from your code:** When your code encounters the impossible
- **Separate risky operations:** Use individual try-catch blocks when you need different responses for different failures
- **Propagate errors you can't handle:** Let the caller deal with it
- **Validate inputs early:** Fail fast with clear messages
- **Debug systematically:** Print statements, rubber duck, binary search, scientific method
- **Never silently ignore errors:** At minimum, log them

Errors are not your enemy — they're your ally in building robust software. They tell you when something is wrong so you can fix it. Embrace them.

---

## Exercises

**Exercise 10.1:** Write a program that asks for two numbers and divides them, handling division by zero with a friendly message. Include validation for non-numeric input.

**Exercise 10.2:** Write a function that reads a number from a file, with error handling for missing files, empty files, and invalid content. Return a default value if anything goes wrong.

**Exercise 10.3:** Modify the Note Keeper from Chapter 9 to handle file errors gracefully. If the notes file is corrupted, offer to start fresh.

**Exercise 10.4:** Write a function `safeGet(arr, index, default)` that returns `arr[index]` if valid, or `default` if the index is out of bounds. Do this *without* using try-catch (use conditional checks instead).

**Exercise 10.5:** Write a program that parses a date String like "2024-03-15" into year, month, and day variables. Validate that each component is in a sensible range. Handle all possible errors with helpful messages.

**Exercise 10.6 (Challenge):** Create a configuration file parser that reads key=value pairs from a file. Handle: missing files (create default), invalid lines (skip with warning), duplicate keys (use last value), and missing required keys (error with specific message). Log all warnings and errors.

**Exercise 10.7 (Challenge):** Write a retry wrapper function `withRetry(action, maxAttempts, delay)` that attempts an action up to maxAttempts times, waiting delay milliseconds between attempts. Use exponential backoff (double the delay each time). Return the successful result or throw the last error.

---

*Errors are now tools in your toolkit, not obstacles in your path. But our programs are still limited to built-in types. Next, we learn to create our own data structures.*

*[Continue to Chapter 11: Structures](11-structures.md)*
