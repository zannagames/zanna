---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 25: How Zanna Works

Have you ever wondered what happens between pressing "run" and seeing your program's output? Most programmers treat this as a black box—code goes in, results come out, and the middle remains mysterious. But understanding how your code transforms into a running program is one of the most powerful skills you can develop.

This knowledge isn't just academic curiosity. When you understand how Zanna works internally, you gain practical superpowers. You'll read error messages and immediately know where the problem is. You'll write code that's faster because you understand what the computer actually does with it. You'll debug mysterious issues by reasoning about what's happening beneath the surface. And when something goes wrong, you won't feel helpless—you'll know exactly which layer to investigate.

Think of it like learning to drive. You can operate a car without understanding engines, but a driver who knows what happens when they press the accelerator can diagnose strange noises, drive more efficiently, and avoid damaging their vehicle. The same principle applies to programming. Let's pop the hood and see how Zanna really works.

---

## Mental Models for Compilation

Before diving into technical details, let's build some intuitions about what compilation really means.

### The Translation Metaphor

Imagine you've written a book in English and need to publish it in Japanese. You can't simply swap words one-by-one—the grammar differs, idioms don't translate literally, and some concepts need explanation. A skilled translator reads your book, understands what you meant, and expresses those same ideas naturally in Japanese.

Compilation works similarly. Your source code expresses ideas in a language comfortable for humans. The computer needs those same ideas expressed in a language comfortable for machines. The compiler is the translator, and like a good translator, it doesn't just swap syntax—it understands what you meant and finds the best way to express it in the target language.

### The Assembly Line Metaphor

Picture a car factory. Raw materials enter at one end and finished cars roll out the other. Between those points, the materials pass through many stations: the frame is built, the engine installed, the body painted, the interior fitted. Each station transforms the work-in-progress into something closer to the final product.

The Zanna compiler works like this assembly line:

```text
Source → Tokens → Tree → Checked Tree → IL → Executable
```

Each stage transforms your code into a form closer to what the computer needs. At every step, problems are caught and reported. A car with a faulty engine is caught before painting; code with type errors is caught before execution.

### The Recipe Metaphor

A recipe describes a cake, but it isn't a cake. You can't eat the recipe. To get a cake, someone must follow the instructions—measuring ingredients, mixing them, and baking. The recipe is a plan; the cake is the result of executing that plan.

Your source code is like a recipe. It describes what should happen, but it isn't the computation itself. The Zanna runtime is like a chef who reads your recipe (the IL) and performs the actual work. Understanding this distinction helps clarify why some errors happen when writing code (recipe errors) and others happen when running it (cooking errors).

---

## The Journey of Your Code

When you run a Zanna program, your code passes through several distinct stages. Here's the complete picture:

```text
Source Code → Lexer → Parser → Semantic Analyzer → IL Generator → Runtime
     ↓           ↓        ↓            ↓                ↓           ↓
  Characters   Tokens    AST      Checked AST      IL Code      Results
```

Let's trace a simple program through this entire journey. We'll use this example:

```zia
bind Zanna.Terminal;

func add(a: Integer, b: Integer) -> Integer {
    return a + b;
}

func start() {
    var result = add(3, 4);
    Terminal.Say(result);
}
```

This program defines a function that adds two numbers, calls it with 3 and 4, and prints the result. Simple enough—but behind the scenes, an remarkable transformation takes place.

---

## Stage 1: Lexical Analysis (Tokenization)

The first stage reads your source file character by character and groups those characters into meaningful chunks called *tokens*. This stage is called the *lexer* (or *tokenizer*).

Think of this like reading a sentence. You don't process individual letters—you see words. The sentence "The cat sat" isn't processed as `T-h-e-c-a-t-s-a-t` but as three words: "The", "cat", "sat". The lexer does the same thing for code.

### How Tokenization Works

The lexer scans through your source code and identifies patterns:

```text
func   → TOKEN_FUNC        (keyword)
add    → TOKEN_IDENTIFIER  (name: "add")
(      → TOKEN_LPAREN      (punctuation)
a      → TOKEN_IDENTIFIER  (name: "a")
:      → TOKEN_COLON       (punctuation)
i64    → TOKEN_TYPE        (name: "i64")
,      → TOKEN_COMMA       (punctuation)
b      → TOKEN_IDENTIFIER  (name: "b")
:      → TOKEN_COLON       (punctuation)
i64    → TOKEN_TYPE        (name: "i64")
)      → TOKEN_RPAREN      (punctuation)
->     → TOKEN_ARROW       (punctuation)
i64    → TOKEN_TYPE        (name: "i64")
{      → TOKEN_LBRACE      (punctuation)
return → TOKEN_RETURN      (keyword)
a      → TOKEN_IDENTIFIER  (name: "a")
+      → TOKEN_PLUS        (operator)
b      → TOKEN_IDENTIFIER  (name: "b")
;      → TOKEN_SEMICOLON   (punctuation)
}      → TOKEN_RBRACE      (punctuation)
```

The lexer also recognizes:
- **Numbers**: `42`, `3.14`, `0xFF`
- **Strings**: `"hello"`, `"line one\nline two"`
- **Comments**: `// ignored text`
- **Whitespace**: spaces, tabs, newlines (usually ignored)

### What the Lexer Catches

The lexer catches errors involving malformed individual tokens:

```zia
var x = 3.14.15;    // Error: invalid number literal (two decimal points)
var@name = 5;       // Error: unexpected character '@' in identifier
var s = "unclosed;  // Error: unterminated string literal
```

These errors happen at the character level—the lexer can't even figure out what tokens you meant to write.

### Viewing Tokens

You can see the tokens for any Zanna program:

```bash
zanna run --dump-tokens myprogram.zia
```

This prints every token with its location, kind, and text to stderr—useful when debugging mysterious syntax errors. Sometimes what looks correct to your eye isn't what the lexer sees.

---

## Stage 2: Parsing (Syntax Analysis)

Once the lexer produces tokens, the *parser* takes over. The parser's job is to understand the *structure* of your code—how tokens relate to each other.

Consider the difference between "Dog bites man" and "Man bites dog." Same words, different meanings, because the structure differs. The parser builds an *Abstract Syntax Tree* (AST) that captures this structure.

### Building the Tree

For our `add` function, the parser produces:

```text
FunctionDecl
├── name: "add"
├── params:
│   ├── Param
│   │   ├── name: "a"
│   │   └── type: i64
│   └── Param
│       ├── name: "b"
│       └── type: i64
├── returnType: i64
└── body:
    └── ReturnStmt
        └── BinaryExpr
            ├── op: ADD
            ├── left: Identifier("a")
            └── right: Identifier("b")
```

This tree shows that `add` is a function with two parameters, and its body contains a return statement that returns the result of adding `a` and `b`.

### Why a Tree?

Why not just a list of tokens? Because code has hierarchical structure. Consider this expression:

```zia
var result = (3 + 4) * 2;
```

The parentheses matter! This should compute 7 * 2 = 14, not 3 + 8 = 11. The tree captures this:

```text
BinaryExpr
├── op: MULTIPLY
├── left: BinaryExpr
│   ├── op: ADD
│   ├── left: Literal(3)
│   └── right: Literal(4)
└── right: Literal(2)
```

The tree shows that we multiply the result of an addition by 2—exactly what the parentheses specify.

### What the Parser Catches

The parser catches *structural* errors—violations of grammar:

```zia
func broken( {           // Error: expected parameter or ')'
    var x = ;            // Error: expected expression after '='
    if x < 5             // Error: expected '{' after condition
        Say("hi");
    }
}

func another)            // Error: unexpected ')'
```

These errors involve tokens that don't fit together properly. Each token might be valid on its own, but they don't form a valid program structure.

### Viewing the AST

```bash
zanna run --dump-ast myprogram.zia
```

The AST shows exactly how the parser interpreted your code's structure. You can also use `--dump-sema-ast` to see the AST after semantic analysis, which shows what the type checker annotated or transformed.

---

## Stage 3: Semantic Analysis

Parsing ensures your code is grammatically correct, but grammatically correct code can still be nonsense. "Colorless green ideas sleep furiously" is a grammatically correct English sentence, but it's meaningless.

The *semantic analyzer* checks that your code actually makes sense. This is where types, scopes, and logical constraints are verified.

### Type Checking

Every expression has a type. The semantic analyzer ensures types are used consistently:

```zia
var x: Integer = "hello";      // Error: cannot assign String to Integer
var y = add("a", "b");     // Error: 'add' expects Integer arguments, got String
var z = 5 + "three";       // Error: cannot add Integer and String
```

Type checking is your first line of defense against bugs. When the type checker complains, it's often catching a real problem with your logic.

### Scope Checking

Variables exist within scopes. The semantic analyzer ensures you only use variables that exist:

```zia
func test() {
    Terminal.Say(x);  // Error: 'x' not defined
    var x = 5;         // too late!
}

func another() {
    if true {
        var inner = 10;
    }
    Say(inner);              // Error: 'inner' not defined in this scope
}
```

### Return Checking

Functions that declare a return type must actually return a value:

```zia
func getValue() -> Integer {  // Error: may not return a value
    var x = 5;
}

func maybeReturn(flag: Boolean) -> Integer {  // Error: not all paths return a value
    if flag {
        return 42;
    }
}
```

### Entity and Value Checking

When working with entities and values, the semantic analyzer ensures you use them correctly:

```zia
class Player {
    expose name: String;
    hide health: Integer;       // hidden from outside
}

func test() {
    var p = Player();
    p.health = 50;          // Error: 'health' is hidden
}
```

### What Makes It Through

If your code passes semantic analysis, you've eliminated entire categories of bugs:
- Type mismatches
- Missing variables
- Missing returns
- Access violations (expose/hide)
- Arity mismatches (wrong number of arguments)

This is why Zanna catches so many errors before your program runs—the semantic analyzer is thorough.

---

## Stage 4: IL Generation

After semantic analysis, your code is structurally correct and type-safe. Now it's time to transform it into something closer to what the computer can execute. This is where *Intermediate Language* (IL) generation happens.

### What Is IL?

IL is a lower-level representation of your program. It's simpler than your source code but more abstract than machine code. Think of it as a universal assembly language that works on any computer.

Here's what our `add` function becomes in IL:

```il
.func add(i64, i64) -> i64
entry:
    %sum = add %0, %1        ; add parameters together
    ret %sum                 ; return the result

.func start() -> void
entry:
    %t0 = call @add(3, 4)    ; call add(3, 4), store result in %t0
    call @Zanna.Terminal.PrintI64(%t0)  ; print the result
    ret                      ; return from start
```

Let's break down the key elements:

- **Functions**: `.func add(i64, i64) -> i64` declares a function named `add` taking two 64-bit integers and returning one
- **Labels**: `entry:` marks a *basic block*—a sequence of instructions with one entry point
- **Registers**: `%sum`, `%t0`, `%0`, `%1` are virtual registers that hold values
- **Instructions**: `add`, `ret`, `call` are operations the VM understands
- **Comments**: Text after `;` explains what's happening

### Why Have an IL?

You might wonder: why not go straight from source code to machine code? IL exists for several powerful reasons:

**Multiple Languages, One Backend**

Zanna supports two frontend languages: Zia and BASIC. Both compile to the same IL:

```text
Zia ───┐
       ├──► IL ───► VM/Native
BASIC ─┘
```

This means:
- The VM only needs to understand IL, not two different languages
- Improvements to the VM benefit both languages
- You can mix languages in the same project—they both speak IL

**Platform Independence**

IL runs on any platform that has a Zanna VM or code generator. Write once, run anywhere.

**Optimization**

IL is a convenient place to optimize code. Transformations that would be complex on source code become simpler on IL's regular structure.

### The Same Logic, Three Syntaxes

Here's the same function in both Zanna languages:

**Zia**
```zia
func square(x: Integer) -> Integer {
    return x * x;
}
```

**BASIC**
```basic
FUNCTION Square(x AS INTEGER) AS INTEGER
    Square = x * x
END FUNCTION
```

Both produce identical IL:

```il
.func square(i64) -> i64
entry:
    %t0 = mul %0, %0         ; multiply x by itself
    ret %t0                  ; return the result
```

This is the power of a common IL—different syntaxes, same semantics.

---

## Understanding IL in Depth

IL is worth understanding in detail because it reveals exactly what your code does. Let's explore its key concepts.

### The Stack Machine Model

Zanna's IL uses a *stack-based* execution model. Operations push values onto a conceptual stack and pop values off:

```zia
var result = (3 + 4) * 2;
```

Becomes:

```il
iconst 3     ; push 3 onto stack         → stack: [3]
iconst 4     ; push 4 onto stack         → stack: [3, 4]
iadd         ; pop two, add, push result → stack: [7]
iconst 2     ; push 2 onto stack         → stack: [7, 2]
imul         ; pop two, multiply, push   → stack: [14]
store result ; pop and store in 'result' → stack: []
```

This model is elegant because:
- **Simple to generate**: Each operation is independent
- **Compact**: No need to name intermediate values
- **Easy to verify**: Stack discipline catches many errors

### Types in IL

IL has a small, explicit type system:

| Type  | Description                    |
|-------|--------------------------------|
| `i1`  | Boolean (0 or 1)               |
| `i16` | 16-bit integer                 |
| `i32` | 32-bit integer                 |
| `i64` | 64-bit integer (most common)   |
| `f64` | 64-bit floating point          |
| `ptr` | Memory pointer                 |
| `str` | String handle                  |
| `void`| No value (for procedures)      |

Every value has a known type. The IL verifier ensures types match everywhere.

### Reading IL Instructions

Here's a guide to common IL instructions:

**Arithmetic**
```il
add %a, %b    ; integer addition
sub %a, %b    ; integer subtraction
mul %a, %b    ; integer multiplication
sdiv %a, %b   ; signed integer division
fadd %a, %b   ; floating-point addition
```

**Comparisons** (produce `i1` boolean)
```il
icmp_eq %a, %b    ; equal?
icmp_ne %a, %b    ; not equal?
scmp_lt %a, %b    ; less than? (signed)
scmp_gt %a, %b    ; greater than? (signed)
```

**Control Flow**
```il
br label         ; unconditional jump
cbr %cond, then, else  ; conditional branch
ret %value       ; return from function
```

**Memory**
```il
alloca 8         ; allocate 8 bytes on stack
load i64, %ptr   ; read i64 from memory
store i64, %ptr, %val  ; write i64 to memory
```

**Calls**
```il
call @funcname(%arg1, %arg2)  ; call function
```

### A Complete IL Example

Let's trace a more complex function through IL:

```zia
func factorial(n: Integer) -> Integer {
    if n <= 1 {
        return 1;
    }
    return n * factorial(n - 1);
}
```

This becomes:

```il
.func factorial(i64) -> i64
entry:
    %cond = scmp_le %0, 1      ; n <= 1?
    cbr %cond, base, recurse   ; if true, go to base; else recurse

base:
    ret 1                      ; return 1

recurse:
    %n_minus_1 = sub %0, 1     ; compute n - 1
    %rec = call @factorial(%n_minus_1)  ; recursive call
    %result = mul %0, %rec     ; n * factorial(n-1)
    ret %result                ; return the product
```

Notice how the IL makes the control flow explicit. There's no "magic"—every branch and jump is visible.

### Viewing Your Program's IL

```bash
zia --emit-il myprogram.zia
```

This is incredibly useful for:
- Understanding what your code actually does
- Finding inefficiencies
- Debugging mysterious behavior
- Learning how language features are implemented

---

## Stage 5: The Runtime

IL by itself is just data. The *runtime* is what brings it to life. Zanna offers two execution modes: the Virtual Machine (VM) and native code generation.

### The Virtual Machine

The VM is an interpreter that reads IL instructions and executes them one by one. Think of it as a computer simulated in software.

Here's how the VM executes our add function call:

```text
Step 1: Execute `iconst 3`
        Action: Push 3 onto the value stack
        Stack:  [3]

Step 2: Execute `iconst 4`
        Action: Push 4 onto the value stack
        Stack:  [3, 4]

Step 3: Execute `call @add`
        Action: Pop arguments, create new frame, jump to add

        --- Inside add ---
Step 4: Execute `add %0, %1`
        Action: Add parameters (3 + 4)
        Result: 7

Step 5: Execute `ret %sum`
        Action: Return 7 to caller
        --- Back in start ---

Step 6: Execute `call @Zanna.Terminal.PrintI64`
        Action: Print 7 to console
        Output: 7
```

The VM maintains:
- **Value Stack**: Temporary values during computation
- **Call Stack**: Function frames (return addresses, local variables)
- **Heap**: Dynamically allocated memory (objects, arrays)
- **Program Counter**: Which instruction to execute next

### Native Code Generation

For maximum performance, Zanna can compile IL to native machine code:

```text
IL instruction:        Native x86-64:
iadd                   pop rbx       ; pop second operand
                       pop rax       ; pop first operand
                       add rax, rbx  ; add them
                       push rax      ; push result
```

Native code runs directly on the CPU with no interpretation overhead. This is significantly faster for computation-heavy programs.

### The Runtime Library

When your code calls `Zanna.Terminal.Say` or uses strings, it's invoking the *runtime library*—a set of functions written in C that provide system capabilities:

- **I/O**: Console input/output, file operations
- **Strings**: Concatenation, substrings, comparison
- **Memory**: Allocation, garbage collection
- **Math**: Square root, trigonometry, random numbers
- **Time**: Clocks, timers, delays
- **Graphics**: Windows, drawing, sprites

The runtime bridges the gap between IL and the operating system. Your IL code remains platform-independent; the runtime handles platform-specific details.

---

## Memory Management and Garbage Collection

One of the runtime's most important jobs is managing memory. In many languages, you must manually allocate and free memory—a tedious and error-prone process. Zanna handles this automatically through *garbage collection*.

### How Objects Live and Die

When you create a value or class, memory is allocated:

```zia
func createStuff() {
    var list = [1, 2, 3];      // Memory allocated for array
    var name = "Alice";         // Memory allocated for string
    var player = Player();      // Memory allocated for class
}
// When function returns, what happens to this memory?
```

The answer: garbage collection tracks what's still in use and frees the rest.

### The Mark-and-Sweep Algorithm

Zanna uses a mark-and-sweep garbage collector. Here's how it works:

**Phase 1: Mark**
Starting from "roots" (global variables, the stack), the collector follows every reference and marks objects as "reachable."

```text
Roots → [Player object] → [name: "Alice"]
                        → [inventory: [...]]

        [old_data]  ← nothing points here anymore
```

**Phase 2: Sweep**
Any object not marked is garbage—nothing can reach it, so nothing can use it. The collector frees these objects.

```text
Before:  [Player ✓] [name ✓] [inventory ✓] [old_data ✗]
After:   [Player ✓] [name ✓] [inventory ✓] [          ]
                                            ^ freed!
```

### Reference Types vs Value Types

Understanding value types and reference types helps you predict memory behavior:

**Value types** are stored directly:
```zia
var x: i64 = 42;     // 42 stored directly in x's memory slot
var y = x;           // y gets its own copy of 42
x = 100;             // x is now 100, y is still 42
```

**Reference types** store a pointer to heap memory:
```zia
var a = [1, 2, 3];   // Array lives on heap; 'a' holds a reference
var b = a;           // 'b' references the SAME array
a[0] = 999;          // Both a[0] and b[0] are now 999!
```

This distinction matters for performance and correctness. Modifying a reference type through one variable affects all variables that reference it.

### Writing GC-Friendly Code

While you don't manage memory directly, understanding GC helps you write efficient code:

```zia
// Less efficient: creates many temporary strings
func buildName(first: String, last: String) -> String {
    var result = "";
    result = result + first;  // temporary string created
    result = result + " ";    // another temporary
    result = result + last;   // yet another
    return result;
}

// More efficient: fewer allocations
func buildNameFast(first: String, last: String) -> String {
    return first + " " + last;  // compiler can optimize this
}
```

---

## How Errors Are Caught

One of the most practical benefits of understanding the compilation pipeline is knowing *when* errors are caught. Different errors appear at different stages.

### Compile-Time Errors

These are caught before your program runs:

**Lexical Errors** (Stage 1 - Tokenization)
```zia
var x = 3.14.15;     // Invalid number: two decimal points
var s = "unterminated  // String never closed
```

**Syntax Errors** (Stage 2 - Parsing)
```zia
func missing(         // Error: missing closing parenthesis
    var x = ;         // Error: missing expression
    if x { }          // Error: missing condition
```

**Semantic Errors** (Stage 3 - Analysis)
```zia
var x: Integer = "hello";     // Type mismatch
unknown_function();        // Undefined function
var y = z + 1;            // Undefined variable 'z'
```

Compile-time errors are the best kind—they're caught before anyone runs your program, so they can't cause problems in production.

### Runtime Errors

Some errors can only be detected when the program runs:

**Division by Zero**
```zia
func divide(a: Integer, b: Integer) -> Integer {
    return a / b;  // What if b is 0?
}

func start() {
    var x = getUserInput();
    var result = divide(100, x);  // Runtime error if x is 0
}
```

The compiler can't know what value `x` will have—the user provides it at runtime.

**Array Index Out of Bounds**
```zia
var arr = [1, 2, 3];
var i = getUserInput();
var value = arr[i];  // Runtime error if i >= 3 or i < 0
```

**Null Reference**
```zia
func findPlayer(name: String) -> Player? {
    // might return null if not found
}

func start() {
    var p = findPlayer("Bob");
    p.health = 100;  // Runtime error if p is null
}
```

### Error Messages and Where They Come From

When you see an error message, you can often tell which stage caught it:

**Lexer error**: Usually mentions "unexpected character" or "invalid literal"
```text
Error at line 5, column 12: unexpected character '@'
```

**Parser error**: Usually mentions "expected" something
```text
Error at line 10: expected ')' after function parameters
```

**Semantic error**: Usually mentions types or names
```text
Error at line 15: cannot assign value of type 'string' to variable of type 'i64'
Error at line 20: undefined function 'foo'
```

**Runtime error**: Happens when running, often with a stack trace
```text
Runtime error: division by zero
  at divide (myprogram.zia:3)
  at start (myprogram.zia:8)
```

---

## Step-by-Step Transformation Trace

Let's follow a complete program through every stage of compilation. This will solidify your understanding of the entire pipeline.

### The Source Code

```zia
func max(a: Integer, b: Integer) -> Integer {
    if a > b {
        return a;
    }
    return b;
}

func start() {
    var result = max(10, 25);
    Terminal.Say(result);
}
```

### Stage 1: Tokenization

The lexer produces this stream of tokens:

```text
FUNC IDENT("max") LPAREN IDENT("a") COLON TYPE("i64") COMMA
IDENT("b") COLON TYPE("i64") RPAREN ARROW TYPE("i64") LBRACE
IF IDENT("a") GT IDENT("b") LBRACE
RETURN IDENT("a") SEMICOLON
RBRACE
RETURN IDENT("b") SEMICOLON
RBRACE
FUNC IDENT("start") LPAREN RPAREN LBRACE
VAR IDENT("result") EQUALS IDENT("max") LPAREN INT(10) COMMA INT(25) RPAREN SEMICOLON
IDENT("Zanna") DOT IDENT("Terminal") DOT IDENT("Say") LPAREN IDENT("result") RPAREN SEMICOLON
RBRACE
EOF
```

### Stage 2: Parsing (AST)

The parser builds this tree:

```text
Module
├── FunctionDecl
│   ├── name: "max"
│   ├── params: [(a, i64), (b, i64)]
│   ├── returnType: i64
│   └── body:
│       ├── IfStmt
│       │   ├── condition: BinaryExpr(a > b)
│       │   └── thenBranch:
│       │       └── ReturnStmt(Identifier("a"))
│       └── ReturnStmt(Identifier("b"))
│
└── FunctionDecl
    ├── name: "start"
    ├── params: []
    ├── returnType: void
    └── body:
        ├── VarDecl
        │   ├── name: "result"
        │   └── init: CallExpr(max, [10, 25])
        └── ExprStmt
            └── CallExpr(Zanna.Terminal.Say, [result])
```

### Stage 3: Semantic Analysis

The analyzer walks the tree and checks:

1. `max` is defined before it's called (scope check)
2. `max(10, 25)` passes two `i64` values to a function expecting two `i64` parameters (type check)
3. Both branches of `max` return `i64` as declared (return check)
4. `result` is defined before being passed to `Say` (scope check)

No errors found—proceed to IL generation.

### Stage 4: IL Generation

```il
.func max(i64, i64) -> i64
entry:
    %cond = scmp_gt %0, %1     ; a > b?
    cbr %cond, return_a, return_b

return_a:
    ret %0                     ; return a

return_b:
    ret %1                     ; return b

.func start() -> void
entry:
    %result = call @max(10, 25)
    call @Zanna.Terminal.PrintI64(%result)
    ret
```

### Stage 5: Execution (VM)

```text
=== Executing start() ===

Instruction: call @max(10, 25)
  Create frame for 'max'
  %0 = 10, %1 = 25

  === Inside max() ===
  Instruction: scmp_gt %0, %1
    Compare: 10 > 25? → false (0)
    %cond = 0

  Instruction: cbr %cond, return_a, return_b
    %cond is 0 (false) → jump to return_b

  Instruction: ret %1
    Return value: 25
  === Exit max() ===

  %result = 25

Instruction: call @Zanna.Terminal.PrintI64(%result)
  Output: 25

Instruction: ret
  Exit program

=== Program Complete ===
Output: 25
```

---

## Debugging with Internal Knowledge

Understanding the compilation pipeline gives you powerful debugging strategies.

### When Your Code Won't Compile

1. **Read the error message carefully**. It tells you which stage failed.

2. **Lexer errors**: Look for typos, unclosed strings, invalid characters
   ```zia
   var name = "hello;    // Missing closing quote
   var x = 3..14;        // Extra dot
   ```

3. **Parser errors**: Check matching brackets, semicolons, statement structure
   ```zia
   if condition {        // Missing expression after 'if'
   func test( {          // Missing parameter list
   ```

4. **Semantic errors**: Check types, names, and logic
   ```zia
   var x: Integer = getInput();  // Does getInput() return Integer?
   process(x, y, z);         // Are x, y, z defined? Right types?
   ```

### When Your Code Runs but Misbehaves

1. **Print intermediate values** to narrow down where logic goes wrong
   ```zia
   bind Zanna.Terminal;

   func calculate(x: Integer) -> Integer {
       Terminal.Say("Input: " + x.toString());
       var step1 = x * 2;
       Terminal.Say("After multiply: " + step1.toString());
       var step2 = step1 + 10;
       Terminal.Say("After add: " + step2.toString());
       return step2;
   }
   ```

2. **Check the IL** if behavior seems impossible
   ```bash
   zia --emit-il myprogram.zia
   ```
   Sometimes the IL reveals that your code doesn't do what you think.

3. **Trace execution** with the VM's trace mode
   ```bash
   zia --trace myprogram.zia
   ```

### Understanding Performance

IL helps you understand why some code is slow:

```zia
bind Fmt = Zanna.Text.Fmt;

// This creates many temporary strings
func slow() -> String {
    var result = "";
    for i in 0..1000 {
        result = result + Fmt.Int(i);  // New string each iteration!
    }
    return result;
}
```

Looking at the IL, you'd see a `call @Zanna.String.Concat` in every loop iteration, each creating a new string.

---

## The Three Frontends Unified

Zanna's architecture allows three different languages to share one runtime. Let's see how this works in practice.

### Same Logic, Different Syntax

**Zia** (modern, curly-brace syntax)
```zia
bind Zanna.Terminal;

func greet(name: String) {
    Terminal.Say("Hello, " + name + "!");
}

func start() {
    greet("World");
}
```

**BASIC** (classic, keyword-based syntax)
```basic
SUB Greet(name AS STRING)
    PRINT "Hello, "; name; "!"
END SUB

SUB Main()
    Greet("World")
END SUB
```

### Both Produce the Same IL

```il
.func greet(str) -> void
entry:
    %t0 = const_str @.hello       ; "Hello, "
    %t1 = call @Zanna.String.Concat(%t0, %0)
    %t2 = const_str @.exclaim     ; "!"
    %t3 = call @Zanna.String.Concat(%t1, %t2)
    call @Zanna.Terminal.PrintStr(%t3)
    ret

.func start() -> void
entry:
    %t0 = const_str @.world       ; "World"
    call @greet(%t0)
    ret
```

### Mixing Languages

Because both compile to the same IL, you can:
- Call BASIC functions from Zia
- Call Zia functions from BASIC
- Share data structures across both languages

This interoperability is automatic—no special configuration needed.

---

## Common Misconceptions

### "The VM is Slow"

Modern VMs are highly optimized. For most programs—especially those that do I/O, process data, or interact with users—VM overhead is negligible. The bottleneck is usually I/O or algorithms, not interpretation.

For truly performance-critical code, you can use native compilation via the IL toolchain:
```bash
zia myprogram.zia --emit-il -o myprogram.il
zanna codegen arm64 myprogram.il -o myprogram
```

### "Garbage Collection Causes Pauses"

Early garbage collectors did cause noticeable pauses. Modern GC algorithms are much smarter:
- **Incremental collection**: Work is spread across many small steps
- **Generational collection**: Focus on new objects (most die young)
- **Concurrent collection**: GC runs alongside your program

For most programs, you won't notice GC at all.

### "I Need to Understand IL to Program"

Absolutely not! Most Zanna programmers never look at IL. But understanding it helps when:
- Debugging mysterious issues
- Optimizing performance-critical code
- Satisfying curiosity about how things work
- Becoming a better programmer overall

---

## Summary

We've taken a deep dive into how Zanna works. Here's what we covered:

**The Compilation Pipeline**
1. **Lexer**: Characters → Tokens (catches malformed literals)
2. **Parser**: Tokens → AST (catches syntax errors)
3. **Semantic Analyzer**: AST → Checked AST (catches type/scope errors)
4. **IL Generator**: Checked AST → IL (transforms to executable form)
5. **Runtime**: IL → Execution (runs your program)

**Intermediate Language**
- Platform-independent representation
- Stack-based execution model
- Shared by all three frontend languages
- Inspectable with `--dump-il`

**The Runtime**
- Virtual Machine interprets IL
- Native compilation for maximum performance
- Runtime library provides system services
- Garbage collection manages memory automatically

**Debugging Power**
- Error messages tell you which stage failed
- IL reveals what your code actually does
- Understanding stages helps fix problems faster

This knowledge makes you a more effective programmer. You're no longer working with a black box—you understand the machine.

---

## Exercises

**Exercise 25.1 - Token Exploration**
Write a simple program with a function that adds two numbers. Use `--dump-tokens` to see the token stream. Count how many different token types appear.

**Exercise 25.2 - AST Investigation**
Create a program with nested arithmetic: `var x = (1 + 2) * (3 + 4);`
Use `--dump-ast` to see the tree structure. Draw the tree on paper and verify the order of operations is correct.

**Exercise 25.3 - Error Classification**
Try to create one error of each type:
- A lexer error (invalid token)
- A parser error (bad syntax)
- A semantic error (type mismatch)
- A runtime error (division by zero)

Note how the error messages differ.

**Exercise 25.4 - IL Comparison**
Write the same function in Zia and BASIC. Use `--dump-il` on both and compare. What's the same? What's different?

**Exercise 25.5 - IL Reading**
Look at the IL for this function:
```zia
func sum_to_n(n: Integer) -> Integer {
    var total = 0;
    var i = 1;
    while i <= n {
        total = total + i;
        i = i + 1;
    }
    return total;
}
```
Trace the execution by hand for `sum_to_n(3)`. Track the values in each register through every instruction.

**Exercise 25.6 - Memory Investigation**
Create a program that makes many strings in a loop:
```zia
bind Fmt = Zanna.Text.Fmt;

func makeManyStrings() -> String {
    var last = "";
    for i in 0..10000 {
        last = "item " + Fmt.Int(i);
    }
    return last;
}
```
Think about what happens to memory. When are strings created? When might they be collected?

**Exercise 25.7 - Performance Thinking**
Consider these two functions:
```zia
func version1(n: Integer) -> Integer {
    var sum = 0;
    for i in 0..n {
        sum = sum + i;
    }
    return sum;
}

func version2(n: Integer) -> Integer {
    return (n * (n - 1)) / 2;
}
```
Both compute the sum 0 + 1 + 2 + ... + (n-1). Use `--dump-il` to see how different they are. Then try `--dump-il-opt` with `-O1` to see how the optimizer transforms each version. Which would be faster for large `n`? Why?

**Exercise 25.8 (Challenge) - Recursive IL Tracing**
Trace the complete IL execution of:
```zia
func fib(n: Integer) -> Integer {
    if n <= 1 {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}
```
For `fib(4)`, draw the call tree and show the stack state at each call and return.

**Exercise 25.9 (Challenge) - Two-Language Project**
Create a small project that uses both languages:
- A Zia main program
- A BASIC utility module

Verify they can call each other's functions. Use `--dump-il` on each to see how they share the same IL format.

**Exercise 25.10 (Challenge) - Build Your Mental Model**
Write a one-page explanation of how Zanna works, as if explaining to a friend who has never programmed. Use analogies that make sense to you. This exercise tests your understanding—if you can explain it simply, you truly understand it.

---

*Now that you understand how Zanna works internally, you're ready to make your code faster. Next chapter: Performance optimization.*

*[Continue to Chapter 26: Performance →](26-performance.md)*
