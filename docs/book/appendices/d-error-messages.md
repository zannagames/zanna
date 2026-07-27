---
status: active
audience: public
last-verified: 2026-07-26
---

# Appendix D: Error Messages

A comprehensive guide to Zanna error messages, their causes, and solutions. When you encounter an error, find it here for a clear explanation and fix.

---

## How to Use This Reference

Error messages in Zanna follow a consistent format:

```text
Error: ErrorType at filename.zia:LINE:COLUMN
  Description of what went wrong

    LINE-1 | previous line of code
  > LINE   | the problematic line
           |     ^^^ indicator pointing to the issue
    LINE+1 | next line of code

  in function 'functionName' at filename.zia:LINE
  called from 'callerName' at filename.zia:LINE
```

**Reading strategy:**
1. Note the **error type** (e.g., `TypeError`, `SyntaxError`)
2. Check the **line and column** numbers
3. Read the **description** for specifics
4. Look at the **code context** shown
5. Trace the **call stack** if the error is in a function

---

## Quick Reference by Category

| Category | Common Errors |
|----------|---------------|
| [Syntax Errors](#syntax-errors) | Missing punctuation, invalid tokens, unclosed delimiters |
| [Type Errors](#type-errors) | Mismatched types, invalid operations, conversion failures |
| [Name Errors](#name-errors) | Undefined variables, duplicate definitions, scope issues |
| [Function Errors](#function-errors) | Wrong arguments, missing returns, signature mismatches |
| [Entity/Interface Errors](#entityinterface-errors) | Unimplemented methods, access violations, inheritance issues |
| [Runtime Errors](#runtime-errors) | Null access, array bounds, division by zero |
| [File/IO Errors](#fileio-errors) | Missing files, permission issues, format errors |
| [Memory Errors](#memory-errors) | Out of memory, stack overflow, resource exhaustion |
| [Concurrency Errors](#concurrency-errors) | Deadlocks, race conditions, synchronization issues |
| [Module Errors](#module-errors) | Import failures, circular dependencies, missing exports |

---

## Syntax Errors

Syntax errors occur when your code violates the grammar rules of Zia. The compiler catches these before your program runs.

> **Cross-reference:** See [Chapter 2: Your First Program](../part1-foundations/02-first-program.md) for basic syntax rules.

---

### "Unexpected token"

```text
Error: SyntaxError at main.zia:5:5
  Unexpected token 'else'
```

**What it means:** The compiler found a keyword or symbol where it wasn't expected, usually because something is missing earlier in the code.

**Common causes:**
- Missing braces `{}` around blocks
- Missing parentheses in conditions
- Missing semicolons
- Typos in keywords

**Fix examples:**

```zia
// Problem: Missing braces
if x > 0
    doSomething();
else              // Error: unexpected 'else'
    doOther();

// Solution 1: Add braces
if x > 0 {
    doSomething();
} else {
    doOther();
}

// Solution 2: Single-line format (for simple cases)
if x > 0 { doSomething(); } else { doOther(); }
```

```zia
// Problem: Missing parentheses
if x > 0 && y < 10 {  // OK in Zanna, but...

// Problem: Missing closing paren
if (x > 0 && y < 10 {  // Error: unexpected '{'
    doSomething();
}

// Solution: Balance parentheses
if (x > 0 && y < 10) {
    doSomething();
}
```

**Prevention:** Use an editor with syntax highlighting and bracket matching. When you type an opening brace, immediately type the closing one, then fill in the middle.

---

### "Expected ';'"

```text
Error: SyntaxError at main.zia:10:1
  Expected ';' after statement
```

**What it means:** A statement ended without the required semicolon.

**Common causes:**
- Simply forgetting the semicolon
- Breaking a statement across lines incorrectly
- Copy-pasting code and missing the trailing semicolon

**Fix examples:**

```zia
// Problem: Missing semicolon
var x = 10
var y = 20;    // Error reported here, but problem is line above

// Solution: Add semicolon
var x = 10;
var y = 20;
```

```zia
// Problem: Multi-line statement broken incorrectly
var result = someFunction(arg1, arg2)
    + anotherFunction(arg3);  // Error: unexpected '+'

// Solution: Keep operator at end of previous line
var result = someFunction(arg1, arg2) +
    anotherFunction(arg3);
```

**Prevention:** Configure your editor to highlight missing semicolons. Many editors can auto-insert them on save.

---

### "Expected '}'"

```text
Error: SyntaxError at main.zia:EOF
  Expected '}' at end of block
```

**What it means:** A block (function, if, loop, etc.) was opened with `{` but never closed with `}`.

**Common causes:**
- Forgetting to close a nested block
- Deleting a closing brace during editing
- Mismatched braces from copy-paste

**Fix examples:**

```zia
// Problem: Missing closing brace
func calculate(x: Integer) -> Integer {
    if x > 0 {
        return x * 2;
    // Missing } for the if
    return 0;
}  // This closes the function, but if is still open

// Solution: Add the missing brace
func calculate(x: Integer) -> Integer {
    if x > 0 {
        return x * 2;
    }  // Close the if
    return 0;
}
```

**Prevention:**
- Use an editor that highlights matching braces
- Keep consistent indentation so mismatches are visually obvious
- When you type `{`, immediately type `}` and then fill in between

---

### "Invalid character"

```text
Error: SyntaxError at main.zia:3:15
  Invalid character '@'
```

**What it means:** A character that isn't part of Zia syntax was found in your code.

**Common causes:**
- Copying code from websites (hidden special characters)
- Using smart quotes instead of straight quotes
- Typing symbols that aren't valid operators

**Fix examples:**

```zia
// Problem: Smart quotes from word processor
var message = "Hello";  // Uses curly quotes

// Solution: Use straight quotes
var message = "Hello";
```

```zia
// Problem: Invalid symbol
var email = user@domain.com;  // @ isn't valid here

// Solution: Make it a String
var email = "user@domain.com";
```

**Prevention:** Use a plain-text editor designed for code, not a word processor. If pasting from the web, use "Paste as plain text" or type it manually.

---

### "Unterminated String literal"

```text
Error: SyntaxError at main.zia:7:20
  Unterminated String literal
```

**What it means:** A String was opened with `"` but never closed.

**Common causes:**
- Forgetting the closing quote
- Having an unescaped quote inside the String
- Multi-line String without proper continuation

**Fix examples:**

```zia
// Problem: Missing closing quote
var message = "Hello, world!;

// Solution: Add closing quote
var message = "Hello, world!";
```

```zia
// Problem: Quote inside String
var quote = "She said "Hello"";  // Inner quotes end the String early

// Solution: Escape inner quotes
var quote = "She said \"Hello\"";
```

```zia
// Problem: Unintended line break
var text = "This is a very long message that
continues on the next line";  // Error on first line

// Solution 1: Keep on one line
var text = "This is a very long message that continues on the next line";

// Solution 2: Use concatenation
var text = "This is a very long message that " +
           "continues on the next line";

// Solution 3: Use \n for intentional line breaks
var text = "Line one\nLine two";
```

**Prevention:** When typing a quote, type both opening and closing, then fill in the middle.

---

### "Expected identifier"

```text
Error: SyntaxError at main.zia:5:5
  Expected identifier after 'var'
```

**What it means:** The compiler expected a name (for a variable, function, etc.) but found something else.

**Common causes:**
- Starting a name with a number
- Using a reserved keyword as a name
- Typo that resulted in invalid characters

**Fix examples:**

```zia
// Problem: Name starts with number
var 2ndPlace = "Silver";  // Error

// Solution: Start with letter or underscore
var secondPlace = "Silver";
var _2ndPlace = "Silver";
```

```zia
// Problem: Using keyword as name
var func = 10;    // Error: 'func' is a keyword
var if = true;    // Error: 'if' is a keyword

// Solution: Choose different names
var funcValue = 10;
var condition = true;
```

**Prevention:** Use descriptive names that start with a lowercase letter. Avoid single-letter names except for loop counters.

> **Cross-reference:** See [Chapter 3: Values and Names](../part1-foundations/03-values-and-names.md) for naming conventions.

---

## Type Errors

Type errors occur when you use a value in a way that doesn't match its type.

> **Cross-reference:** See [Chapter 3: Values and Names](../part1-foundations/03-values-and-names.md) for type fundamentals.

---

### "Type mismatch"

```text
Error: TypeError at main.zia:7:14
  Type mismatch: expected 'Integer', got 'String'
```

**What it means:** You used a value of one type where a different type was required.

**Common causes:**
- Assigning wrong type to a variable
- Passing wrong type to a function
- Returning wrong type from a function

**Fix examples:**

```zia
// Problem: Wrong type assignment
var count: Integer = "hello";  // Can't put String in Integer variable

// Solution 1: Use correct type
var count: Integer = 42;

// Solution 2: Change variable type
var count: String = "hello";

// Solution 3: Convert the value (if appropriate)
bind Convert = Zanna.Core.Convert;
var count: Integer = Convert.ToInt64("42");
```

```zia
// Problem: Wrong parameter type
bind Zanna.Terminal;

func greet(name: String) {
    Say("Hello, " + name);
}

greet(42);  // Error: expected String, got Integer

// Solution: Pass correct type
greet("Alice");
// Convert if needed: bind Zanna.Text.Fmt as Fmt; greet(Fmt.Int(42));
```

```zia
// Problem: Wrong return type
func getAge() -> Integer {
    return "twenty-five";  // Error: expected Integer
}

// Solution: Return correct type
func getAge() -> Integer {
    return 25;
}
```

**Prevention:** Be explicit about types when declaring variables. Use type annotations to catch mismatches early.

---

### "Cannot assign to immutable variable"

```text
Error: TypeError at main.zia:12:1
  Cannot assign to immutable variable 'PI'
```

**What it means:** You tried to change a value that was declared as constant.

**Common causes:**
- Trying to modify a `final` constant
- Modifying a function parameter (which may be immutable)

**Fix examples:**

```zia
// Problem: Modifying constant
final MAX_SCORE = 100;
MAX_SCORE = 200;  // Error: cannot assign to constant

// Solution 1: Use 'var' if it needs to change
var maxScore = 100;
maxScore = 200;  // OK

// Solution 2: Create new variable
final MAX_SCORE = 100;
var currentMax = MAX_SCORE + 100;  // OK: new variable
```

```zia
// Problem: Should be var, not final
final counter = 0;
counter = counter + 1;  // Error: counter is final

// Solution: Use var for values that change
var counter = 0;
counter = counter + 1;  // OK
```

**Prevention:** Use `final` only for true constants (values that should never change). Use `var` for variables that will be modified.

> **Cross-reference:** See [Chapter 3: Values and Names](../part1-foundations/03-values-and-names.md) for var vs final.

---

### "Incompatible types in binary operation"

```text
Error: TypeError at main.zia:8:20
  Cannot apply '+' to 'String' and 'Integer'
```

**What it means:** You tried to use an operator with types that don't support that operation together.

**Common causes:**
- Mixing strings and numbers in arithmetic
- Using comparison operators with incompatible types
- Attempting operations that don't make sense

**Fix examples:**

```zia
// Problem: Adding String and number
var result = "Value: " + 42;  // May error depending on context

// Solution 1: Convert number to String explicitly
bind Zanna.Text.Fmt as Fmt;
var result = "Value: " + Fmt.Int(42);

// Solution 2: Use String interpolation
var result = "Value: ${42}";
```

```zia
// Problem: Comparing incompatible types
var name = "Alice";
var age = 30;
if name > age {  // Error: can't compare String to number
    // ...
}

// Solution: Compare same types
if name.Length > age {  // Compare two numbers
    // ...
}
```

```zia
// Problem: Arithmetic on strings
var a = "5";
var b = "3";
var sum = a + b;  // Results in "53", not 8!

// Solution: Convert to numbers first
bind Convert = Zanna.Core.Convert;
var a = Convert.ToInt64("5");
var b = Convert.ToInt64("3");
var sum = a + b;  // 8
```

**Prevention:** Be aware of your variable types. When doing math, ensure all operands are numeric. When building strings, convert numbers explicitly.

---

### "Cannot convert type"

```text
Error: TypeError at main.zia:15:12
  Cannot convert 'String' to 'Integer': invalid format
```

**What it means:** A type conversion was attempted but failed because the value couldn't be converted.

**Common causes:**
- Parsing non-numeric String as number
- Invalid format for date/time parsing
- Casting between incompatible class types

**Fix examples:**

```zia
// Problem: Parsing non-number
bind Convert = Zanna.Core.Convert;
bind Zanna.Terminal;

var num = Convert.ToInt64("hello");  // Error: "hello" is not a number

// Solution: Use try-catch for values that may be invalid
var input = "hello";
try {
    var num = Convert.ToInt64(input);
} catch(e: ParseError) {
    Say("Please enter a valid number");
}
```

```zia
// Problem: Trailing characters
bind Convert = Zanna.Core.Convert;

var num = Convert.ToInt64("42abc");  // Error or unexpected result

// Solution: Clean input first
var input = "42abc";
var cleaned = input.Trim().Replace("abc", "");  // Remove non-numeric chars
var num = Convert.ToInt64(cleaned);
```

**Prevention:** Always validate user input before parsing. Use try-catch when conversion might fail.

> **Cross-reference:** See [Chapter 10: Errors and Recovery](../part2-building-blocks/10-errors.md) for handling parse errors.

---

### "Null pointer exception" / "Cannot access property of null"

```text
Error: NullPointerError at main.zia:15:18
  Cannot access property 'name' of null value
```

**What it means:** You tried to use a method or field on a value that is null.

**Common causes:**
- Not initializing an object
- Function returning null unexpectedly
- Array lookup returning null
- Optional value not checked

**Fix examples:**

```zia
// Problem: Using null value
bind Zanna.Terminal;

var user = findUser(id);
Say(user.name);  // Error if user is null!

// Solution 1: Check for null
var user = findUser(id);
if user != null {
    Say(user.name);
} else {
    Say("User not found");
}

// Solution 2: Use null-safe operator (if available)
var name = user?.name ?? "Unknown";

// Solution 3: Provide default
var user = findUser(id) ?? defaultUser;
```

```zia
// Problem: Array element might be null
var item = inventory[index];
item.use();  // Error if slot is empty (null)

// Solution: Check first
var item = inventory[index];
if item != null {
    item.use();
}
```

**Prevention:**
- Initialize all variables
- Check return values from functions that might return null
- Document which functions can return null
- Consider using optional types where appropriate

---

## Name Errors

Name errors occur when you use a name that doesn't exist or conflicts with another name.

---

### "Undefined variable"

```text
Error: NameError at main.zia:15:20
  Undefined variable 'count'
```

**What it means:** You used a variable name that hasn't been declared.

**Common causes:**
- Typo in variable name
- Using variable before declaring it
- Variable declared in different scope
- Forgetting to import a module

**Fix examples:**

```zia
// Problem: Typo
bind Zanna.Terminal;

var counter = 0;
Say(counte);  // Error: typo in name

// Solution: Fix spelling
Say(counter);
```

```zia
// Problem: Used before declaration
bind Zanna.Terminal;

Say(score);  // Error: score doesn't exist yet
var score = 100;

// Solution: Declare before use
var score = 100;
Say(score);
```

```zia
// Problem: Out of scope
bind Zanna.Terminal;

if someCondition {
    var temp = 42;
}
Say(temp);  // Error: temp not visible here

// Solution: Declare in outer scope
var temp = 0;
if someCondition {
    temp = 42;
}
Say(temp);
```

**Prevention:**
- Use consistent naming conventions
- Declare variables at the beginning of their scope
- Use an IDE with autocomplete to catch typos

---

### "Undefined function"

```text
Error: NameError at main.zia:20:5
  Undefined function 'calulate'
```

**What it means:** You called a function that doesn't exist.

**Common causes:**
- Typo in function name
- Function defined in unimported module
- Function defined after it's called (in some cases)
- Function is a method but called as standalone

**Fix examples:**

```zia
// Problem: Typo
var result = calulate(5, 3);  // Should be 'calculate'

// Solution: Fix spelling
var result = calculate(5, 3);
```

```zia
// Problem: Missing import
var data = Json.Parse(text);  // Error: Json not defined

// Solution: Import the module
bind Json = Zanna.Data.Json;
var data = Json.Parse(text);
```

```zia
// Problem: Using undefined function name
var length = length(myString);  // Error: 'length' is not a function

// Solution: Use the .Length property (no parentheses)
var length = myString.Length;
```

**Prevention:** Use IDE autocomplete. When a function isn't found, check: (1) spelling, (2) imports, (3) whether it's a method.

---

### "Duplicate definition"

```text
Error: NameError at main.zia:30:6
  Duplicate definition of 'processData'
```

**What it means:** The same name is defined twice in the same scope.

**Common causes:**
- Copy-paste error
- Importing two modules with same function name
- Declaring variable twice

**Fix examples:**

```zia
// Problem: Same function defined twice
func calculate(x: Integer) -> Integer {
    return x * 2;
}

func calculate(x: Integer) -> Integer {  // Error: duplicate
    return x * 3;
}

// Solution 1: Rename one
func calculateDouble(x: Integer) -> Integer {
    return x * 2;
}

func calculateTriple(x: Integer) -> Integer {
    return x * 3;
}

// Solution 2: Use overloading (different parameters)
func calculate(x: Integer) -> Integer {
    return x * 2;
}

func calculate(x: Integer, y: Integer) -> Integer {
    return x + y;
}
```

```zia
// Problem: Variable declared twice
var count = 0;
// ... later ...
var count = 10;  // Error: count already exists

// Solution: Just assign
var count = 0;
// ... later ...
count = 10;  // No 'var', just assignment
```

**Enum variant duplicates** are a special case of this error. Each variant name within an enum must be unique:

**Zia:**
```zia
// Problem: Duplicate variant
enum Color { Red, Green, Red }  // Error: duplicate variant 'Red'

// Fix: Remove duplicate
enum Color { Red, Green, Blue }
```

**BASIC:**
```basic
' Problem: Duplicate variant
ENUM Tint
  RED
  GREEN
  RED       ' Error B2120: duplicate enum variant
END ENUM

' Fix: Remove duplicate
ENUM Tint
  RED
  GREEN
  BLUE
END ENUM
```

**Prevention:** Use meaningful, specific names. Search your codebase before adding new names.

---

### "Variable used before declaration"

```text
Error: NameError at main.zia:5:12
  Variable 'total' used before declaration
```

**What it means:** You referenced a variable on a line before its `var` declaration.

**Fix:**

```zia
// Problem: Usage before declaration
bind Zanna.Terminal;

Say(total);  // Error
var total = 100;

// Solution: Move declaration before use
var total = 100;
Say(total);  // OK
```

---

### "Cannot shadow variable"

```text
Error: NameError at main.zia:12:9
  Cannot shadow variable 'x' from outer scope
```

**What it means:** You declared a variable with the same name as one in an outer scope, which can cause confusion.

**Fix examples:**

```zia
// Problem: Shadowing outer variable
var count = 0;
for i in 0..10 {
    var count = i;  // Shadows outer 'count' - confusing!
}

// Solution: Use different names
var totalCount = 0;
for i in 0..10 {
    var currentCount = i;
    totalCount = totalCount + currentCount;
}
```

**Prevention:** Use descriptive names that distinguish different variables. Avoid reusing names in nested scopes.

---

## Function Errors

Errors related to function definitions and calls.

> **Cross-reference:** See [Chapter 7: Functions](../part1-foundations/07-functions.md) for function basics.

---

### "Wrong number of arguments"

```text
Error: ArgumentError at main.zia:10:5
  Function 'add' expects 2 arguments, got 3
```

**What it means:** You called a function with more or fewer arguments than it accepts.

**Fix examples:**

```zia
// The function definition
func add(a: Integer, b: Integer) -> Integer {
    return a + b;
}

// Problem: Too many arguments
var result = add(1, 2, 3);  // Error: expects 2, got 3

// Solution: Match the signature
var result = add(1, 2);  // OK

// Problem: Too few arguments
var result = add(1);  // Error: expects 2, got 1

// Solution: Provide all required arguments
var result = add(1, 0);  // OK
```

```zia
// If you need variable arguments, use an array
func sum(numbers: List[Integer]) -> Integer {
    var total = 0;
    for n in numbers {
        total = total + n;
    }
    return total;
}

var result = sum([1, 2, 3, 4, 5]);  // Pass array
```

**Prevention:** Check the function signature before calling. Use IDE features to see parameter hints.

---

### "Missing return statement"

```text
Error: TypeError at main.zia:25:1
  Function 'getValue' must return a value of type 'Integer'
```

**What it means:** A function declares a return type but doesn't always return a value.

**Fix examples:**

```zia
// Problem: No return statement
func getValue() -> Integer {
    var x = 42;
    // Function ends without returning!
}

// Solution: Add return
func getValue() -> Integer {
    var x = 42;
    return x;
}
```

```zia
// Problem: Return only in some branches
func getStatus(code: Integer) -> String {
    if code == 0 {
        return "OK";
    } else if code == 1 {
        return "Warning";
    }
    // What if code is 2? No return!
}

// Solution: Ensure all paths return
func getStatus(code: Integer) -> String {
    if code == 0 {
        return "OK";
    } else if code == 1 {
        return "Warning";
    }
    return "Error";  // Default case
}
```

**Prevention:** When writing a function with a return type, ensure every possible execution path ends with a return statement.

---

### "Cannot return value from void function"

```text
Error: TypeError at main.zia:8:5
  Cannot return a value from function with no return type
```

**What it means:** You tried to return a value from a function that doesn't declare a return type.

**Fix examples:**

```zia
// Problem: Returning from void function
bind Zanna.Terminal;

func printMessage(msg: String) {
    Say(msg);
    return msg;  // Error: function doesn't return anything
}

// Solution 1: Remove the return value
func printMessage(msg: String) {
    Say(msg);
    return;  // OK: return without value
}

// Solution 2: Add return type if needed
func printMessage(msg: String) -> String {
    Say(msg);
    return msg;  // OK: function returns String
}
```

---

### "Argument type mismatch"

```text
Error: TypeError at main.zia:15:12
  Argument 1: expected 'String', got 'Integer'
```

**What it means:** A function was called with an argument of the wrong type.

**Fix examples:**

```zia
bind Zanna.Terminal;

func greet(name: String) {
    Say("Hello, " + name);
}

// Problem: Wrong argument type
greet(42);  // Error: expected String, got Integer

// Solution: Pass correct type
greet("Alice");  // OK
// bind Zanna.Text.Fmt as Fmt; greet(Fmt.Int(42));  // convert Integer to String if needed
```

---

## Entity/Interface Errors

Errors related to entities (Zanna's term for classes), interfaces, and object-oriented programming.

> **Cross-reference:** See [Chapter 14: Objects and Entities](../part3-objects/14-objects.md) and [Chapter 16: Interfaces](../part3-objects/16-interfaces.md).

---

### "Entity does not implement interface"

```text
Error: TypeError at main.zia:20:8
  Entity 'Circle' does not implement method 'draw' from interface 'Drawable'
```

**What it means:** An class claims to implement an interface but is missing one or more required methods.

**Fix examples:**

```zia
interface Drawable {
    func draw();
    func getColor() -> String;
}

// Problem: Missing method
bind Zanna.Terminal;

class Circle implements Drawable {
    radius: Number;

    func draw() {
        Say("Drawing circle");
    }
    // Missing getColor()!
}

// Solution: Implement all methods
class Circle implements Drawable {
    radius: Number;
    color: String;

    func draw() {
        Say("Drawing circle with radius " + self.radius);
    }

    func getColor() -> String {
        return self.color;
    }
}
```

**Prevention:** When implementing an interface, check its definition and implement every method it requires.

---

### "Cannot access hidden member"

```text
Error: AccessError at main.zia:30:15
  Cannot access hidden member 'balance' of class 'BankAccount'
```

**What it means:** You tried to access a field or method marked `hide` from outside the class.

**Fix examples:**

```zia
class BankAccount {
    hide balance: Number;  // Hidden field

    expose func getBalance() -> Number {
        return self.balance;
    }
}

// Problem: Accessing hidden field
bind Zanna.Terminal;

var account = BankAccount();
Say(account.balance);  // Error: balance is hidden

// Solution: Use exposed method
Say(account.getBalance());  // OK
```

**Prevention:** Design your entities with clear exposed/hidden boundaries. Use `expose` for the public interface and `hide` for internal implementation.

> **Cross-reference:** See [Chapter 14: Objects and Entities](../part3-objects/14-objects.md) for encapsulation concepts.

---

### "Method signature mismatch"

```text
Error: TypeError at main.zia:25:5
  Override of 'speak' has different signature than parent
```

**What it means:** When overriding a method from a parent class, the signature must match exactly.

**Fix examples:**

```zia
class Animal {
    func speak() -> String {
        return "...";
    }
}

// Problem: Different return type
class Dog extends Animal {
    func speak() -> Integer {  // Error: parent returns String
        return 1;
    }
}

// Problem: Different parameters
class Dog extends Animal {
    func speak(loudly: Boolean) -> String {  // Error: parent has no params
        return "Woof!";
    }
}

// Solution: Match signature exactly
class Dog extends Animal {
    func speak() -> String {
        return "Woof!";
    }
}
```

---

### "Missing initializer"

```text
Error: TypeError at main.zia:15:12
  Cannot create 'Player' without initializer
```

**What it means:** You tried to create a class instance without providing required initialization values.

**Fix examples:**

```zia
class Player {
    name: String;
    health: Integer;

    expose func init(name: String, health: Integer) {
        self.name = name;
        self.health = health;
    }
}

// Problem: No arguments
var player = Player();  // Error: needs name and health

// Solution: Provide required arguments
var player = Player("Alice", 100);  // OK
```

---

### "Cannot access 'self' in static context"

```text
Error: ContextError at main.zia:12:16
  Cannot access 'self' outside of method context
```

**What it means:** You used `self` in a context where there's no object instance.

**Fix examples:**

```zia
bind Zanna.Terminal;

class Counter {
    count: Integer;

    // Problem: Using self in static function
    static func printCount() {
        Say(self.count);  // Error: no self in static
    }
}

// Solution 1: Make it a regular method
class Counter {
    count: Integer;

    func printCount() {
        Say(self.count);  // OK: self exists in methods
    }
}

// Solution 2: Pass instance as parameter
class Counter {
    count: Integer;

    static func printCount(counter: Counter) {
        Say(counter.count);  // Access via parameter
    }
}
```

---

## Runtime Errors

Runtime errors occur while your program is running, typically when an operation is impossible to perform.

> **Cross-reference:** See [Chapter 10: Errors and Recovery](../part2-building-blocks/10-errors.md) for handling runtime errors.

---

### "Index out of bounds"

```text
Error: IndexError at main.zia:20:15
  Array index 10 is out of bounds for array of length 5
```

**What it means:** You tried to access an array element at a position that doesn't exist.

**Common causes:**
- Off-by-one errors (arrays are 0-indexed)
- Loop going too far
- Not checking array length
- Empty array access

**Fix examples:**

```zia
// Problem: Index too high
var items = [1, 2, 3, 4, 5];  // Indices 0-4
var x = items[5];  // Error: index 5 doesn't exist

// Solution: Use valid index
var x = items[4];  // OK: last valid index
```

```zia
// Problem: Off-by-one in loop
bind Zanna.Terminal;

var items = ["a", "b", "c"];
for i in 0..items.Length + 1 {  // Goes to 3, but max index is 2
    Say(items[i]);  // Error on last iteration
}

// Solution: Correct loop bounds
for i in 0..items.Length {  // 0 to 2 (exclusive)
    Say(items[i]);  // OK
}

// Better: Use for-each
for item in items {
    Say(item);  // No index needed
}
```

```zia
// Problem: Not checking for empty array
var scores: List[Integer] = [];
var first = scores[0];  // Error: array is empty

// Solution: Check length first
if scores.Length > 0 {
    var first = scores[0];  // OK
}
```

**Prevention:**
- Use for-each loops when possible
- Always validate indices before use
- Check array length before accessing elements
- Remember: first index is 0, last index is length - 1

---

### "Division by zero"

```text
Error: ArithmeticError at main.zia:12:16
  Division by zero
```

**What it means:** You divided a number by zero, which is mathematically undefined.

**Fix examples:**

```zia
// Problem: Literal division by zero
var result = 10 / 0;  // Error

// Problem: Variable could be zero
var total = 100;
var count = 0;
var average = total / count;  // Error: dividing by 0

// Solution: Check before dividing
bind Zanna.Terminal;

if count != 0 {
    var average = total / count;
} else {
    Say("Cannot calculate average: no items");
}
```

```zia
// Solution: Provide default for zero divisor
func safeDivide(a: Integer, b: Integer, default: Integer) -> Integer {
    if b == 0 {
        return default;
    }
    return a / b;
}

var average = safeDivide(total, count, 0);
```

**Prevention:** Always validate divisors before division operations, especially when the divisor comes from user input or calculation.

---

### "Integer overflow"

```text
Error: OverflowError at main.zia:8:12
  Integer overflow: result exceeds Integer range
```

**What it means:** A calculation produced a number too large (or too small) to fit in the integer type.

**Fix examples:**

```zia
// Problem: Result too large
var big = 9223372036854775807;  // Max Integer
var bigger = big + 1;  // Error: overflow

// Solution 1: Use checked arithmetic
var result = big.CheckedAdd(1);  // Returns null on overflow

// Solution 2: Use larger type or arbitrary precision
var big = BigInt("9223372036854775807");
var bigger = big + 1;  // OK with BigInt

// Solution 3: Validate before operation
if big < Integer.MAX {
    var bigger = big + 1;  // Safe
}
```

**Prevention:** Be aware of integer limits when working with large numbers. Use appropriate data types for your range of values.

---

### "Type cast failed"

```text
Error: CastError at main.zia:30:12
  Cannot cast 'Dog' to 'Cat'
```

**What it means:** You tried to cast an object to a type it isn't compatible with.

**Fix examples:**

```zia
// Problem: Invalid cast
var animal: Animal = Dog("Rex");
var cat = animal as Cat;  // Error: it's a Dog, not a Cat!

// Solution: Check type first
if animal is Cat {
    var cat = animal as Cat;  // Safe: we know it's a Cat
    cat.meow();
} else if animal is Dog {
    var dog = animal as Dog;
    dog.bark();
}
```

```zia
// Solution: Use pattern matching
bind Zanna.Terminal;

match animal {
    case cat: Cat {
        cat.meow();
    }
    case dog: Dog {
        dog.bark();
    }
    else {
        Say("Unknown animal");
    }
}
```

**Prevention:** Always use `is` to check types before casting, or use pattern matching.

---

### "Stack overflow"

```text
Error: StackOverflowError at main.zia:15:5
  Stack overflow: too many nested function calls
```

**What it means:** Your program ran out of stack space, usually from too-deep recursion.

**Common causes:**
- Infinite recursion (no base case)
- Missing base case in recursive function
- Very deep recursion even with base case

**Fix examples:**

```zia
// Problem: Infinite recursion (no base case)
bind Zanna.Terminal;

func countdown(n: Integer) {
    Say(n);
    countdown(n - 1);  // Never stops!
}

// Solution: Add base case
func countdown(n: Integer) {
    if n < 0 {
        return;  // Base case: stop recursion
    }
    Say(n);
    countdown(n - 1);
}
```

```zia
// Problem: Base case never reached
func factorial(n: Integer) -> Integer {
    return n * factorial(n - 1);  // n keeps decreasing forever!
}

// Solution: Proper base case
func factorial(n: Integer) -> Integer {
    if n <= 1 {
        return 1;  // Base case
    }
    return n * factorial(n - 1);
}
```

```zia
// Problem: Recursion too deep even with base case
func processDeep(depth: Integer) {
    if depth == 0 { return; }
    processDeep(depth - 1);  // With depth = 1000000, stack overflow
}

// Solution: Convert to iteration
func processDeep(depth: Integer) {
    for i in 0..depth {
        // Process iteration
    }
}
```

**Prevention:**
- Always define a base case for recursive functions
- Consider iterative solutions for deep recursion
- Use tail recursion when possible (if optimizer supports it)

> **Cross-reference:** See [Chapter 7: Functions](../part1-foundations/07-functions.md) for recursion.

---

## File/IO Errors

Errors related to file operations and input/output.

> **Cross-reference:** See [Chapter 9: Files](../part2-building-blocks/09-files.md) for file operations.

---

### "File not found"

```text
Error: FileError at main.zia:5:20
  File not found: 'data.txt'
```

**What it means:** You tried to read a file that doesn't exist.

**Fix examples:**

```zia
// Problem: File doesn't exist
bind Zanna.IO;
bind Zanna.Terminal;

var content = IO.File.ReadAllText("data.txt");  // Error if missing

// Solution 1: Check existence first
if IO.File.Exists("data.txt") {
    var content = IO.File.ReadAllText("data.txt");
} else {
    Say("File not found");
}

// Solution 2: Use try-catch
try {
    var content = IO.File.ReadAllText("data.txt");
} catch(e: FileNotFound) {
    Say("File not found, creating default...");
    IO.File.WriteAllText("data.txt", "default content");
}

// Solution 3: Provide path at runtime
Terminal.Print("Enter filename: ");
var filename = Terminal.ReadLine();
if filename != null and IO.File.Exists(filename!) {
    var content = IO.File.ReadAllText(filename!);
}
```

**Prevention:**
- Check if files exist before reading
- Handle missing files gracefully
- Use relative paths carefully (know your working directory)
- Provide fallback defaults

---

### "Permission denied"

```text
Error: FileError at main.zia:10:5
  Permission denied: '/etc/passwd'
```

**What it means:** Your program doesn't have permission to access the file.

**Fix examples:**

```zia
// Problem: Writing to protected location
bind File = Zanna.IO.File;
bind Zanna.System.Machine as Machine;

File.WriteAllText("/etc/config.txt", data);  // Permission denied

// Solution: Write to allowed location
File.WriteAllText("./config.txt", data);  // Local directory
File.WriteAllText(Machine.Home + "/config.txt", data);  // User's home
```

**Prevention:**
- Write files to appropriate locations (user directories, temp directories)
- Don't assume write access to system directories
- Handle permission errors gracefully

---

### "File already exists"

```text
Error: FileError at main.zia:15:5
  File already exists: 'output.txt' (exclusive create mode)
```

**What it means:** You tried to create a file that already exists when using exclusive create mode.

**Fix examples:**

```zia
// Problem: You want to create a file only when it does not already exist
bind File = Zanna.IO.File;
bind Zanna.Terminal;
bind Zanna.Time;
bind Convert = Zanna.Core.Convert;

// Solution 1: Check and decide
if File.Exists("output.txt") {
    Print("File exists. Overwrite? (y/n): ");
    if (ReadLine() ?? "") == "y" {
        File.WriteAllText("output.txt", data);
    }
} else {
    File.WriteAllText("output.txt", data);
}

// Solution 2: Generate unique name
// Generate unique name using a tick counter
var filename = "output_" + Convert.ToStringInt(Time.Clock.NowMs()) + ".txt";
File.WriteAllText(filename, data);
```

---

### "Parse error"

```text
Error: ParseError at main.zia:8:25
  Invalid JSON at position 42: unexpected '}'
```

**What it means:** File content couldn't be parsed in the expected format.

**Fix examples:**

```zia
// Problem: Invalid JSON
bind Json = Zanna.Data.Json;
bind Zanna.Terminal;

var data = Json.Parse("{invalid json}");  // Parse error

// Solution: Validate and handle errors
try {
    var data = Json.Parse(content);
    processData(data);
} catch(e: ParseError) {
    Say("Invalid file format: " + e.message);
    // Use default or prompt user
}
```

**Prevention:**
- Validate file contents before parsing
- Provide helpful error messages
- Consider using schema validation for complex formats

---

## Memory Errors

Errors related to memory allocation and management.

---

### "Out of memory"

```text
Error: MemoryError at main.zia:50:5
  Out of memory: failed to allocate 1073741824 bytes
```

**What it means:** Your program requested more memory than is available.

**Common causes:**
- Creating extremely large arrays
- Loading huge files into memory
- Memory leak from accumulating data
- Infinite loop creating objects

**Fix examples:**

```zia
// Problem: Array too large
var huge = Array.new(1000000000);  // 1 billion elements

// Solution: Process in chunks
func processLargeData(source: DataSource) {
    while source.hasMore() {
        var chunk = source.readChunk(10000);  // Process 10K at a time
        processChunk(chunk);
        // chunk is freed after each iteration
    }
}
```

```zia
// Problem: Loading huge file into memory
bind File = Zanna.IO.File;

var content = File.ReadAllText("huge_file.txt");  // Out of memory

// Solution: Read line by line
var reader = openReader("huge_file.txt");
while reader.hasNextLine() {
    var line = reader.ReadLine();
    processLine(line);
}
reader.Close();
```

**Prevention:**
- Process large data in chunks
- Release resources when done
- Monitor memory usage in development
- Set reasonable limits on data sizes

---

## Concurrency Errors

Errors related to multi-threaded and concurrent programming.

> **Cross-reference:** See [Chapter 24: Concurrency](../part4-applications/24-concurrency.md) for threading.

---

### "Deadlock detected"

```text
Error: DeadlockError at runtime
  Deadlock detected: threads waiting on each other
```

**What it means:** Two or more threads are each waiting for the other to release a lock, so neither can proceed.

**Common causes:**
- Acquiring locks in different orders
- Holding one lock while waiting for another
- Circular dependency between locks

**Fix examples:**

```zia
// Problem: Lock ordering causes deadlock
// Thread 1: lock(A) then lock(B)
// Thread 2: lock(B) then lock(A)
// If Thread 1 holds A and Thread 2 holds B, deadlock!

// Solution: Always acquire locks in same order
var lockA = Mutex.create();
var lockB = Mutex.create();

func operation1() {
    lockA.lock();
    lockB.lock();  // Always A before B
    // ... work ...
    lockB.unlock();
    lockA.unlock();
}

func operation2() {
    lockA.lock();  // Same order: A before B
    lockB.lock();
    // ... work ...
    lockB.unlock();
    lockA.unlock();
}
```

**Prevention:**
- Always acquire multiple locks in the same order
- Use lock timeouts
- Consider lock-free data structures
- Keep critical sections small

---

### "Race condition" / "Data race detected"

```text
Warning: DataRaceWarning at main.zia:45:12
  Potential data race accessing 'counter' from multiple threads
```

**What it means:** Multiple threads are accessing shared data without proper synchronization.

**Fix examples:**

```zia
bind Thread = Zanna.Threads.Thread;
bind SafeI64 = Zanna.Threads.SafeI64;

// Problem: Unsynchronized access
var counter = 0;

var t1 = Thread.Start(func() {
    for i in 0..1000 {
        counter = counter + 1;  // Race condition!
    }
});

var t2 = Thread.Start(func() {
    for i in 0..1000 {
        counter = counter + 1;  // Race condition!
    }
});

// Solution 1: Use mutex
var counter = 0;
var mutex = Mutex.create();

var t1 = Thread.Start(func() {
    for i in 0..1000 {
        mutex.lock();
        counter = counter + 1;
        mutex.unlock();
    }
});

// Solution 2: Use atomic operations
var counter = SafeI64.New(0);

var t1 = Thread.Start(func() {
    for i in 0..1000 {
        counter.Add(1);  // Atomic, thread-safe
    }
});
```

**Prevention:**
- Protect shared data with mutexes
- Use atomic operations for simple counters
- Minimize shared mutable state
- Prefer message passing between threads

---

## Module Errors

Errors related to module imports and organization.

> **Cross-reference:** See [Chapter 12: Modules](../part2-building-blocks/12-modules.md) for module system.

---

### "Module not found"

```text
Error: ImportError at main.zia:3:8
  Module not found: 'Utils'
```

**What it means:** An imported module couldn't be located.

**Fix examples:**

```zia
// Problem: Module name doesn't match file
bind Utils;  // Looking for Utils.zia, but file is utilities.zia

// Solution 1: Match name to file
bind Utilities;  // Matches utilities.zia

// Solution 2: Use correct path
bind src.utils.Utils;  // For Utils.zia in src/utils/
```

**Prevention:**
- Ensure module names match file names
- Check import paths
- Use consistent naming conventions

---

### "Circular import"

```text
Error: ImportError at moduleA.zia:2:8
  Circular import detected: ModuleA -> ModuleB -> ModuleA
```

**What it means:** Two modules import each other, creating a circular dependency.

**Fix examples:**

```zia
// Problem: A imports B, B imports A
// moduleA.zia
bind ModuleB;
// moduleB.zia
bind ModuleA;  // Circular!

// Solution: Extract shared code to third module
// common.zia - shared types/functions
// moduleA.zia - imports Common
// moduleB.zia - imports Common
```

**Prevention:**
- Design module dependencies as a tree, not a graph
- Extract shared functionality to common modules
- Consider whether modules are properly separated

---

### "Symbol not exported"

```text
Error: ImportError at main.zia:5:12
  Symbol 'internalFunc' is not exported from module 'Utils'
```

**What it means:** You tried to use something from a module that isn't marked for export.

**Fix examples:**

```zia
// In Utils module:
func internalFunc() { ... }  // Not exported (hidden by default)
expose func publicFunc() { ... }  // Exported

// Problem: Accessing non-exported symbol
bind Utils;
Utils.internalFunc();  // Error: not exported

// Solution 1: Use exported symbol
Utils.publicFunc();  // OK

// Solution 2: Export the symbol (if you control the module)
expose func internalFunc() { ... }  // Now exported
```

---

## Debugging Tips

When you encounter an error:

1. **Read the full message carefully.** Error messages contain the type, location, and often a description of what's wrong.

2. **Check the line BEFORE the error.** Many errors (like missing semicolons) are reported on the line after where the actual problem is.

3. **Trace back through the call stack.** The error might occur in a function, but the bug might be in the code that called it with wrong arguments.

4. **Print intermediate values.** Add `Say()` calls (after `bind Zanna.Terminal;`) to see what values variables have at different points.

5. **Simplify the problem.** Create a minimal example that reproduces the error. Often, the bug becomes obvious.

6. **Check types carefully.** Many errors stem from type mismatches. Verify your variable types.

7. **Look for null values.** Null pointer errors are extremely common. Check if any value might be null.

8. **Watch array bounds.** Off-by-one errors with array indices are frequent. Remember arrays start at 0.

9. **Use the debugger.** Step through code line by line to see exactly what's happening.

10. **Search for the error message.** Others have likely encountered the same error. Search documentation and forums.

---

## Error Handling Best Practices

### Validate Early

```zia
func processOrder(quantity: Integer, price: Number) {
    // Validate at the start
    if quantity <= 0 {
        throw ("Quantity must be positive");
    }
    if price < 0 {
        throw ("Price cannot be negative");
    }

    // Now we know inputs are valid
    var total = quantity * price;
    // ...
}
```

### Provide Helpful Messages

```zia
// Unhelpful
throw ("Invalid input");

// Helpful
throw ("Invalid age: " + age + ". Age must be between 0 and 150.");
```

### Handle Specific Errors

```zia
bind Zanna.Terminal;

try {
    var config = loadConfig();
} catch(e: FileNotFound) {
    Say("Config file missing, using defaults");
    config = defaultConfig;
} catch(e: ParseError) {
    Say("Config file corrupted: " + e.message);
    throw e;  // Re-throw - can't recover from this
} catch(e) {
    Say("Unexpected error: " + e.message);
    log(e);
}
```

### Don't Ignore Errors

```zia
// WRONG: Silent failure
try {
    riskyOperation();
} catch(e) {
    // Empty catch - errors disappear!
}

// RIGHT: At least log errors
try {
    riskyOperation();
} catch(e) {
    log("Operation failed: " + e.message);
    // Handle appropriately
}
```

---

*[Back to Table of Contents](../README.md) | [Prev: Appendix C](c-runtime-reference.md) | [Next: Appendix E: Glossary](e-glossary.md)*
