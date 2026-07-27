---
status: active
audience: public
last-verified: 2026-07-26
---

# Appendix E: Glossary

A comprehensive reference of programming terms for beginners. Terms that are specific to Zia are marked with **[Zia]**.

**How to use this glossary:**
- Look up any term you encounter in the book or in code
- Cross-references point you to chapters where concepts are explained in detail
- Related terms help you understand connected concepts
- Pronunciation guides help with unfamiliar words

---

## Symbols and Numbers

**!** (exclamation mark): The logical NOT operator. Flips `true` to `false` and vice versa. Example: `!true` equals `false`. See also *Boolean*, *Logical operator*.

**!=** (not equals): Comparison operator that checks if two values are different. Example: `5 != 3` is `true`. See also *Comparison operator*, *==*.

**%** (percent sign): The modulo or remainder operator. Returns what's left over after division. Example: `10 % 3` equals `1` because 10 divided by 3 is 3 with remainder 1. Pronounced "mod" or "modulo." See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Operator*, *Integer division*.

**&&** (double ampersand): The logical AND operator. Returns `true` only if both sides are true. Example: `true && false` equals `false`. See also *Boolean*, *Logical operator*, *||*.

**+** (plus sign): Addition operator for numbers; concatenation operator for strings. Example: `5 + 3` equals `8`; `"Hello" + "World"` equals `"HelloWorld"`. See also *Concatenation*, *Operator*.

**-** (minus sign): Subtraction operator or negation. Example: `10 - 3` equals `7`; `-5` is negative five.

**/** (forward slash): Division operator. Example: `10 / 3` equals `3` (integer division) or `3.333...` (with floats). See also *Integer division*.

**/** and `//`: Forward slashes are also used for comments. `//` starts a single-line comment; `/* */` encloses multi-line comments.

**32-bit/64-bit**: The size of values a processor or data type can handle. 32-bit means 32 binary digits (bits); 64-bit means 64. Larger bit sizes allow bigger numbers and more memory addressing. Modern computers are typically 64-bit.

**<, <=, >, >=**: Comparison operators. Less than, less than or equal, greater than, greater than or equal. Example: `5 < 10` is `true`. See also *Comparison operator*.

**=** (single equals): Assignment operator. Stores a value in a variable. Example: `x = 5` puts the value 5 into variable x. Not to be confused with `==`. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Assignment*, *==*.

**==** (double equals): Equality comparison operator. Checks if two values are the same. Example: `5 == 5` is `true`. Not to be confused with `=`. See also *Comparison operator*, *=*.

**=>**: Arrow syntax used in lambda expressions. Example: `(x: Integer) => x * 2` defines a function that doubles its input. See also *Lambda*.

**[ ]** (square brackets): Used to access array elements by index, or to define array types. Example: `numbers[0]` accesses the first element; `[Integer]` is an array of integers. See also *Array*, *Index*.

**{ }** (curly braces): Define code blocks, class bodies, or object literals. Everything between `{` and `}` is grouped together. See also *Block*.

**||** (double pipe): The logical OR operator. Returns `true` if at least one side is true. Example: `true || false` equals `true`. See also *Boolean*, *Logical operator*, *&&*.

---

## A

**Abstract** (AB-strakt): Something that represents a concept rather than a specific instance. In programming, abstract concepts are generalized so they can apply to many situations. An abstract class cannot be instantiated directly. See also *Concrete*.

**Accessor**: A method that retrieves (gets) a value from an object without modifying it. Often called a "getter." Example: `getName()` returns the name field. See [Chapter 14](../part3-objects/14-objects.md). See also *Mutator*, *Getter*.

**Algorithm** (AL-go-rith-um): A step-by-step procedure for solving a problem or performing a computation. Like a recipe in cooking - a precise sequence of instructions that produces a result. Example: a sorting algorithm arranges items in order; a search algorithm finds items in a collection.

**Allocate**: To reserve memory for storing data. When you create a variable or object, the system allocates space in memory to hold it. See also *Memory*, *Heap*, *Stack*.

**Anonymous function**: A function without a name. Created inline where needed. Also called a lambda. Example: `(x: Integer) => x * 2`. See also *Lambda*, *Closure*.

**API** (Application Programming Interface) (AY-pee-eye): A set of functions, methods, and protocols that define how software components communicate. Like a waiter in a restaurant - you don't need to know how the kitchen works, you just tell the waiter what you want. See also *Interface*, *Library*.

**Argument**: A value you pass to a function when calling it. The actual data sent to the function. In `greet("Alice")`, "Alice" is the argument. See [Chapter 7](../part1-foundations/07-functions.md). See also *Parameter* (the related but different concept).

**Arithmetic**: Mathematical operations: addition (+), subtraction (-), multiplication (*), division (/), and modulo (%). See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Operator*.

**Array**: An ordered collection of elements, accessed by numeric index starting at 0. Like a numbered list where each item has a position. Example: `var numbers = [10, 20, 30]` creates an array; `numbers[0]` is 10. See [Chapter 6](../part1-foundations/06-collections.md). See also *Index*, *Collection*, *List*.

**ASCII** (AS-kee): American Standard Code for Information Interchange. An older character encoding using numbers 0-127 to represent letters, digits, and symbols. Example: 'A' is 65. Largely superseded by Unicode but still commonly used for basic English text. See also *Unicode*, *UTF-8*.

**Assignment**: Storing a value in a variable using the `=` operator. Example: `age = 25` assigns 25 to the variable `age`. The variable is on the left; the value is on the right. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Variable*, *=*.

**Asynchronous** (ay-SIN-kron-us): Operations that don't block execution while waiting. The program can continue doing other work while waiting for the operation to complete. Opposite of synchronous. Example: downloading a file asynchronously lets your program continue running while the download happens. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Synchronous*, *Concurrency*.

**Atomic operation** (ah-TOM-ik): An operation that completes entirely or not at all, with no intermediate states visible to other threads. Like a single indivisible action - nothing can interrupt it halfway through. Important for thread safety. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Thread*, *Race condition*.

**Attribute**: A piece of metadata attached to code that provides additional information to the compiler or runtime. In Zia, attributes use the `@` symbol. Example: `@deprecated`, `@test`. See also *Annotation*, *Decorator*.

---

## B

**Base class**: In inheritance, the parent class that another class extends. Also called superclass or parent class. A `Dog` class that extends `Animal` has `Animal` as its base class. See [Chapter 15](../part3-objects/15-inheritance.md). See also *Inheritance*, *Derived class*, *Superclass*.

**Binary** (BY-nuh-ree): (1) The base-2 number system using only digits 0 and 1. Computers work in binary because electronic circuits are either on (1) or off (0). (2) A compiled executable file that can run directly on a computer. See also *Bit*, *Byte*, *Compiler*.

**Bit**: The smallest unit of data: a single binary digit, either 0 or 1. Eight bits make a byte. The word comes from "binary digit." See also *Byte*, *Binary*.

**Block**: A group of statements enclosed in braces `{ }`. Blocks define scope - variables created inside a block exist only within that block. Example: the body of an `if` statement or loop is a block. See also *Scope*, *Statement*.

**Body**: The code inside a function, loop, conditional, or other construct. The part between the curly braces that actually does the work. See also *Block*.

**Boolean** (BOO-lee-un): A data type with only two possible values: `true` or `false`. Named after mathematician George Boole (1815-1864), who developed Boolean algebra. Used for conditions and logical operations. Example: `var isReady = true`. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Condition*, *True*, *False*.

**Boolean expression**: An expression that evaluates to `true` or `false`. Example: `age >= 18`, `name == "Alice"`, `isReady && hasPermission`. See also *Boolean*, *Expression*.

**Break**: A statement that immediately exits the current loop. Useful when you find what you're looking for and don't need to continue iterating. See [Chapter 5](../part1-foundations/05-repetition.md). See also *Loop*, *Continue*.

**Bug**: An error in a program that causes incorrect or unexpected behavior. The term reportedly originated from an actual moth found in a computer in 1947. See also *Debug*, *Error*.

**Byte**: A unit of digital information consisting of 8 bits. Can represent values from 0 to 255 (or -128 to 127 if signed). One character in ASCII text is one byte. See also *Bit*, *Memory*.

---

## C

**Cache** (KASH): Temporary storage for frequently accessed data to improve performance. Like keeping often-used items on your desk instead of in a filing cabinet. The computer checks the cache first before going to slower main memory. See also *Memory*, *Performance*.

**Call**: To execute a function. When you write `greet("Alice")`, you are calling the `greet` function. Also called invoking. See [Chapter 7](../part1-foundations/07-functions.md). See also *Function*, *Invoke*.

**Call stack**: The stack of function calls that led to the current point of execution. When function A calls function B which calls function C, the call stack is C on top of B on top of A. Important for understanding errors and debugging. See also *Stack*, *Stack trace*.

**Camel case**: A naming convention where words are joined without spaces, with each word after the first capitalized. Example: `firstName`, `totalScore`, `isGameOver`. Called "camel case" because the capital letters look like humps. The standard convention for variable names in Zia. See also *Snake case*, *Pascal case*.

**Cast**: See *Type conversion*.

**Catch**: The part of try/catch error handling that handles an exception. When code in a `try` block throws an error, the `catch` block catches and handles it. See [Chapter 10](../part2-building-blocks/10-errors.md). See also *Try*, *Exception*, *Throw*.

**Channel**: A communication mechanism for sending data between threads or processes. Like a pipe that connects different parts of your program. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Thread*, *Concurrency*.

**Character**: A single letter, digit, symbol, or space. In Zia, the `char` type represents a single Unicode character. Example: `'a'`, `'5'`, `'!'`, `' '`. See also *String*, *Unicode*.

**Child class**: See *Derived class*.

**Class**: A blueprint for creating objects. Defines data (fields) and behavior (methods). **[Zia]** Declared with the `class` keyword. Example: `class Player { ... }`. See [Chapter 14](../part3-objects/14-objects.md). See also *Object*, *Struct*.

**Closure** (KLOH-zhur): A function that captures and remembers variables from the scope where it was created, even after that scope has ended. The function "closes over" its environment. Example: a function inside another function that uses the outer function's variables. See [Appendix A](a-zia-reference.md). See also *Lambda*, *Scope*.

**Code**: Instructions written in a programming language. Also called source code. The human-readable text that programmers write, which is then compiled or interpreted into something the computer can execute.

**Code block**: See *Block*.

**Collection**: A data structure that holds multiple values. Arrays, lists, maps, and sets are all collections. See [Chapter 6](../part1-foundations/06-collections.md). See also *Array*, *Map*, *Set*, *List*.

**Comment**: Text in code that the computer ignores. Used to explain what code does or why. In Zia: `// single line comment` or `/* multi-line comment */`. Good comments explain "why," not "what." See also *Documentation*.

**Comparison operator**: An operator that compares two values and returns a boolean. Includes `==` (equal), `!=` (not equal), `<`, `>`, `<=`, `>=`. Example: `5 > 3` returns `true`. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Boolean*, *Operator*.

**Compile**: To transform source code into machine code or intermediate code that can be executed. The compiler catches many errors before the program runs. See [Chapter 25](../part5-mastery/25-how-zanna-works.md). See also *Compiler*, *Runtime*.

**Compile-time**: The time when code is being compiled, before it runs. Errors caught at compile-time are easier to fix than runtime errors. See also *Runtime*, *Compile*.

**Compiler**: A program that translates source code into executable code. Takes the human-readable code you write and converts it to something the computer can run. See [Chapter 25](../part5-mastery/25-how-zanna-works.md). See also *Interpreter*, *Compile*.

**Concatenation** (kon-KAT-eh-NAY-shun): Joining strings together end-to-end. Like gluing words together. In Zia, use the `+` operator: `"Hello" + " " + "World"` produces `"Hello World"`. From Latin *concatenare*, "to chain together." See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *String*.

**Concrete**: The opposite of abstract. A concrete class can be instantiated directly; a concrete implementation provides actual code rather than just a definition. See also *Abstract*.

**Concurrency** (kon-KUR-en-see): Multiple tasks making progress over time, possibly interleaved on a single processor or running simultaneously on multiple processors. Like juggling - handling multiple things without necessarily doing them at the exact same instant. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Parallelism*, *Thread*, *Asynchronous*.

**Condition**: An expression that evaluates to `true` or `false`, used to make decisions. The test in an `if` statement or `while` loop. Example: `age >= 18`, `items.Length > 0`. See [Chapter 4](../part1-foundations/04-decisions.md). See also *Boolean*, *Conditional*.

**Conditional**: A statement that executes code based on whether a condition is true. `if`, `else if`, and `else` are conditional statements. Example: `if (age >= 18) { ... }`. See [Chapter 4](../part1-foundations/04-decisions.md). See also *Condition*, *If statement*.

**Constant**: A named value that cannot change after it's set. In Zia, declared with `final`. Example: `final PI = 3.14159`. Constants are typically named in UPPER_CASE. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Variable*, *Final*.

**Constructor**: A special method called when creating a new object. Sets up the object's initial state. **[Zia]** In Zia, constructors are called initializers and use the `init` function name. See [Chapter 14](../part3-objects/14-objects.md). See also *Initializer*, *Init*, *Object*.

**Continue**: A statement that skips the rest of the current loop iteration and moves to the next iteration. Useful when you want to skip certain cases without exiting the loop entirely. See [Chapter 5](../part1-foundations/05-repetition.md). See also *Loop*, *Break*.

**Control flow**: The order in which statements execute. Conditionals and loops change control flow - instead of just executing line by line, the program can branch or repeat. See also *Conditional*, *Loop*.

**Convention**: An agreed-upon way of doing things that isn't enforced by the language. Example: naming variables in camelCase is a convention, not a rule. Following conventions makes code more readable. See also *Syntax*.

**Conversion**: See *Type conversion*.

**CPU** (Central Processing Unit) (see-pee-YOO): The processor that executes program instructions. The "brain" of the computer. Modern CPUs have multiple cores for parallel processing. See [Chapter 1](../part1-foundations/01-the-machine.md). See also *Memory*, *Core*.

**Core**: A processing unit within a CPU. Modern processors have multiple cores (2, 4, 8, or more), allowing true parallel execution. A quad-core processor can run four threads simultaneously. See also *CPU*, *Thread*, *Parallelism*.

---

## D

**Data**: Information that a program works with. Numbers, text, true/false values - anything the program stores, processes, or outputs. See also *Value*, *Data type*.

**Data structure**: A way of organizing and storing data for efficient access and modification. Arrays, maps, trees, graphs, and linked lists are data structures. Different structures are suited for different tasks. See [Chapter 6](../part1-foundations/06-collections.md). See also *Array*, *Map*, *Collection*.

**Data type**: A classification that specifies what kind of values a variable can hold and what operations are valid. Example: `i64` (integer), `string` (text), `bool` (true/false). See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Type*, *Primitive type*.

**Deadlock**: A situation where two or more threads wait for each other indefinitely, unable to proceed. Thread A holds resource 1 and waits for resource 2; Thread B holds resource 2 and waits for resource 1. Neither can continue. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Thread*, *Race condition*, *Concurrency*.

**Debug**: To find and fix errors (bugs) in a program. Involves reading error messages, adding print statements, using debugging tools, and reasoning about code behavior. A crucial programming skill. See also *Bug*, *Debugger*.

**Debugger**: A tool that lets you run a program step by step, inspect variable values, and understand what the code is doing. Essential for finding tricky bugs.

**Declaration**: Introducing a new variable, function, or type to the program. Tells the compiler that something exists and what type it is. Example: `var age: Integer` declares an integer variable named age. See also *Definition*, *Variable*.

**Decrement**: To decrease a value by 1. Example: `count -= 1` or `count = count - 1`. See also *Increment*.

**Default parameter**: A parameter that has a value automatically used if no argument is provided. Example: `func greet(name: String = "Guest")` - calling `greet()` uses "Guest" as the name. See [Appendix A](a-zia-reference.md). See also *Parameter*, *Argument*.

**Definition**: Providing the implementation or value for something that was declared. A function definition includes the function body. See also *Declaration*.

**Dependency**: Code that your code relies on to function. If your program uses a library, that library is a dependency. Managing dependencies is important in larger projects. See also *Library*, *Module*.

**Dependency injection**: A design pattern where dependencies are passed to a component rather than having it create them internally. Makes code more flexible and testable. See [Chapter 18](../part3-objects/18-patterns.md). See also *Design pattern*.

**Derived class**: A class that inherits from another class (the base class). Also called subclass or child class. A `Dog` class that extends `Animal` is derived from `Animal`. See [Chapter 15](../part3-objects/15-inheritance.md). See also *Base class*, *Inheritance*, *Subclass*.

**Design pattern**: A reusable solution to a commonly occurring problem in software design. Examples: Singleton, Factory, Observer. Not code you copy directly, but templates for solving problems. See [Chapter 18](../part3-objects/18-patterns.md).

**Documentation**: Written explanations of what code does and how to use it. Includes comments in code, README files, API references, and tutorials. Good documentation helps others (and future you) understand your code. See also *Comment*.

**Domain**: The subject area or business context that software addresses. A banking domain deals with accounts and transactions; a gaming domain deals with players and scores. Understanding the domain helps you design better software.

**Double**: A double-precision floating-point number. Called "double" because it uses twice the memory of a single-precision float for greater precision. In Zia, `f64` is a double. See also *Float*, *Floating-point*.

**DRY**: "Don't Repeat Yourself." A principle that every piece of knowledge should have a single, unambiguous representation. If you find yourself copying and pasting code, consider making it a function. See also *Refactoring*.

**Dynamic dispatch**: Selecting which method implementation to call at runtime based on the actual object type, not the variable type. Enables polymorphism. See [Chapter 17](../part3-objects/17-polymorphism.md). See also *Polymorphism*, *Virtual method*.

**Dynamic typing**: Type checking done at runtime rather than compile time. Variables can hold different types at different times. Python and JavaScript use dynamic typing. See also *Static typing*, *Type*.

---

## E

**Edge case**: An unusual situation at the extreme boundaries of expected input or operation. Examples: empty strings, zero values, negative numbers, very large numbers. Good code handles edge cases gracefully. See also *Bug*, *Test*.

**Element**: A single item in a collection like an array or list. `numbers[0]` accesses the first element. See also *Array*, *Index*.

**Encapsulation** (en-kap-soo-LAY-shun): Hiding an object's internal details and exposing only a controlled public interface. Like a car - you use the steering wheel and pedals, but the engine's internals are hidden. **[Zia]** Controlled using `expose` (public) and `hide` (private) visibility modifiers. See [Chapter 14](../part3-objects/14-objects.md). See also *Expose*, *Hide*, *Information hiding*.

**Entity** (EN-ti-tee): A general term for an object with identity and behavior. **[Zia]** In older versions of Zia, `entity` was the keyword for class declarations; it has been replaced by `class`. See *Class*.

**Enumeration** (ee-noo-mer-AY-shun): A type consisting of a fixed set of named values. Also called an enum. In Zia: `enum Color { Red, Green, Blue }`. In BASIC: `ENUM Color / RED / GREEN / BLUE / END ENUM`. Variants are integer constants (auto-incrementing from 0 or explicitly assigned). Useful when a variable should only have certain specific values. See the canonical [Zia reference](../../languages/zia-reference.md#enums) and [BASIC reference](../../languages/basic-reference.md#enum). See also *Type*, *Match*.

**Environment**: (1) The runtime context where a program executes, including available variables and resources. (2) The development setup including operating system, tools, and configurations.

**Error**: A problem that prevents code from working correctly. May be caught at compile-time (syntax errors, type errors) or runtime (exceptions, crashes). See [Chapter 10](../part2-building-blocks/10-errors.md). See also *Bug*, *Exception*, *Syntax error*.

**Escape character**: A character preceded by backslash (`\`) that represents something special. `\n` = newline, `\"` = literal quote, `\\` = literal backslash, `\t` = tab. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *String*.

**Exception**: An error condition that disrupts normal program flow. Can be thrown when something goes wrong and caught to handle the error gracefully. See [Chapter 10](../part2-building-blocks/10-errors.md). See also *Try*, *Catch*, *Throw*.

**Execute**: To run code. When the computer executes a program, it carries out its instructions. See also *Run*, *Runtime*.

**Explicit**: Clearly stated, not implied. Explicit type declarations like `var x: Integer = 5` state the type directly. Opposite of implicit. See also *Implicit*.

**Export**: To make code available for use by other modules. Exported functions and entities can be imported elsewhere. See [Chapter 12](../part2-building-blocks/12-modules.md). See also *Import*, *Module*.

**Expose**: **[Zia]** The visibility modifier that makes a member accessible from outside the class (equivalent to "public" in other languages). This is the default visibility for methods in Zia. Example: `expose func init() { ... }`. See [Chapter 14](../part3-objects/14-objects.md). See also *Hide*, *Public*, *Encapsulation*.

**Expression**: Code that evaluates to a value. Can be used anywhere a value is expected. Examples: `2 + 3` (evaluates to 5), `x * y` (evaluates to the product), `age >= 18` (evaluates to boolean). See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Statement*, *Value*.

**Extends**: **[Zia]** Keyword used to indicate inheritance. `class Dog extends Animal` means Dog inherits from Animal. See [Chapter 15](../part3-objects/15-inheritance.md). See also *Inheritance*, *Base class*.

---

## F

**Factory pattern**: A design pattern where object creation is handled by a dedicated method or class, rather than calling constructors directly. Useful when the creation logic is complex or when the exact type to create is determined at runtime. See [Chapter 18](../part3-objects/18-patterns.md). See also *Design pattern*.

**False**: One of the two boolean values, representing "no," "off," or "not true." The opposite of `true`. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Boolean*, *True*.

**Field**: A variable that belongs to a class or struct type. Part of an object's data. Example: in `class Player { name: String; health: Integer; }`, `name` and `health` are fields. Also called member variable, attribute, or property. See [Chapter 14](../part3-objects/14-objects.md). See also *Class*, *Method*.

**FIFO**: First In, First Out. A queue behavior where the first item added is the first item removed. Like a line at a store. See also *Queue*, *LIFO*.

**File**: A named collection of data stored on disk. Programs can read from files (input) and write to files (output). See [Chapter 9](../part2-building-blocks/09-files.md). See also *Path*, *I/O*.

**Final**: **[Zia]** Keyword that declares an immutable variable (constant). Once set, the value cannot be changed. Example: `final PI = 3.14159`. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Constant*, *Immutable*, *Var*.

**Flag**: A boolean variable that signals a condition or state. Example: `var isGameOver = false`. Named because it's like raising or lowering a flag to signal something. See also *Boolean*.

**Float**: Short for floating-point number. A number with a decimal point. In Zia: `f32` (32-bit) or `f64` (64-bit). Examples: `3.14`, `-0.5`, `2.0`. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Floating-point*, *Integer*.

**Floating-point** (FLOH-ting-point): Numbers with decimal points. Called "floating-point" because the decimal point can "float" to different positions to represent very large or very small numbers. Subject to precision limitations. Examples: `3.14`, `0.001`, `1000000.0`. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Float*, *Integer*, *Double*.

**For loop**: A loop that iterates a specific number of times or over a collection. In Zia: `for i in 0..10 { ... }` or `for item in items { ... }`. See [Chapter 5](../part1-foundations/05-repetition.md). See also *Loop*, *While loop*, *Iteration*.

**Format string**: A string with placeholders that get replaced with values. Used for creating formatted output. In Zia, string interpolation uses `${}`: `"Hello, ${name}!"`. See also *String interpolation*.

**Framework**: A reusable structure that provides a foundation for building applications. More comprehensive than a library - a framework calls your code rather than you calling it. Examples: web frameworks, game frameworks. See also *Library*.

**Func**: **[Zia]** Keyword used to declare a function. Example: `func add(a: Integer, b: Integer) -> Integer { return a + b; }`. See [Chapter 7](../part1-foundations/07-functions.md). See also *Function*, *Method*.

**Function**: A reusable block of code that performs a specific task. Takes inputs (parameters), does work, and optionally returns an output (return value). Like a machine: put something in, get something out. Example: `func add(a: Integer, b: Integer) -> Integer { return a + b; }`. See [Chapter 7](../part1-foundations/07-functions.md). See also *Method*, *Parameter*, *Return value*.

---

## G

**Garbage collection** (GAR-bij kuh-LEK-shun): Automatic memory management that identifies and reclaims memory no longer being used. The programmer doesn't need to manually free memory. Like a cleaning service that removes items you're no longer using. See also *Memory*, *Heap*.

**Generic** (jeh-NAIR-ik): Code that works with multiple types, specified as type parameters. Write once, use with many types. Example: `func identity[T](value: T) -> T` works with any type T. See [Appendix A](a-zia-reference.md). See also *Type parameter*, *Polymorphism*.

**Getter**: See *Accessor*.

**Git**: A distributed version control system for tracking changes in code. Allows multiple programmers to collaborate and keeps history of all changes. Commands include `git commit`, `git push`, `git pull`. See also *Version control*, *Repository*.

**Global variable**: A variable accessible from anywhere in the program. Generally discouraged because they make code harder to understand and test. See also *Local variable*, *Scope*.

---

## H

**Hard-coded**: A value written directly in the code rather than being configurable or calculated. Example: using `3.14159` everywhere instead of a constant `PI`. Can make code harder to maintain.

**Hash** (HASH): A fixed-size value computed from data using a hash function. Used for quick lookups in hash maps and for verification. The same input always produces the same hash. See also *Hash map*.

**Hash map**: A data structure that maps keys to values using hashing for very fast lookup. Also called hash table, dictionary, or associative array. Example: looking up a person's phone number by their name. See [Chapter 6](../part1-foundations/06-collections.md). See also *Map*, *Key*, *Value*, *Hash*.

**Heap**: The memory region for dynamically allocated objects - things created at runtime whose size or lifetime isn't known at compile time. Objects, arrays, and other data structures typically live on the heap. See also *Stack*, *Memory*, *Allocate*.

**Helper method**: A private method that assists other methods in a class. Breaks down complex operations into smaller pieces. Example: a private `validateInput()` method called by public methods. See also *Method*, *Hide*.

**Hide**: **[Zia]** The visibility modifier that restricts a member's access to within the class only (equivalent to "private" in other languages). Hidden fields can only be accessed by the class's own methods. Example: `hide balance: Number`. See [Chapter 14](../part3-objects/14-objects.md). See also *Expose*, *Private*, *Encapsulation*.

**HTTP** (Hypertext Transfer Protocol) (aitch-tee-tee-PEE): The protocol for web communication. When you visit a website, your browser uses HTTP to request pages from servers. See [Chapter 22](../part4-applications/22-networking.md). See also *TCP*, *Protocol*.

---

## I

**IDE** (Integrated Development Environment) (eye-dee-EE): A software application that provides comprehensive facilities for software development: code editor, debugger, build tools, and more. Examples: VS Code, IntelliJ, Eclipse. See also *Debugger*, *Compiler*.

**Identifier**: A name given to a variable, function, class, or other program element. Must follow naming rules (start with letter or underscore, contain letters/numbers/underscores). Examples: `playerScore`, `calculateTotal`, `MAX_SIZE`. See also *Variable*, *Keyword*.

**If statement**: A conditional statement that executes code only when a condition is true. Example: `if (age >= 18) { allowVoting(); }`. See [Chapter 4](../part1-foundations/04-decisions.md). See also *Conditional*, *Else*, *Condition*.

**IL** (Intermediate Language): Zanna's internal representation between source code and execution. The compiler produces IL, which the runtime then executes. Allows code written in different Zanna languages to work together. See [Chapter 25](../part5-mastery/25-how-zanna-works.md). See also *Compiler*, *Runtime*.

**Immutable** (ih-MYOO-tuh-bul): Cannot be changed after creation. Once an immutable value is set, it stays that way forever. Strings are often immutable. `final` variables are immutable. Opposite of mutable. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Mutable*, *Final*, *Constant*.

**Implement**: To provide code that fulfills an interface or abstract definition. When a class "implements" an interface, it provides concrete methods for all the interface's requirements. See [Chapter 16](../part3-objects/16-interfaces.md). See also *Interface*, *Implements*.

**Implements**: **[Zia]** Keyword indicating that a class provides implementations for an interface's methods. Example: `class Circle implements Drawable`. See [Chapter 16](../part3-objects/16-interfaces.md). See also *Interface*, *Class*.

**Implicit**: Not explicitly stated; inferred or assumed. Type inference is implicit - the compiler figures out the type without you stating it. Opposite of explicit. Example: `var x = 5` implicitly has type `Integer`. See also *Explicit*, *Type inference*.

**Import**: To bring code from another module into the current file. Makes external functions, entities, and values available for use. Example: `import Zanna.Math`. See [Chapter 12](../part2-building-blocks/12-modules.md). See also *Export*, *Module*.

**Increment**: To increase a value by 1. Example: `count += 1` or `count = count + 1`. See [Chapter 5](../part1-foundations/05-repetition.md). See also *Decrement*.

**Index**: A number indicating position in an array or string. In most languages, indexing starts at 0: the first element is at index 0, the second at index 1, etc. Example: `names[0]` gets the first name. See [Chapter 6](../part1-foundations/06-collections.md). See also *Array*, *Zero-indexed*.

**Infinite loop**: A loop that never ends because its condition never becomes false. Sometimes intentional (game loops), often a bug. Example: `while true { ... }`. See [Chapter 5](../part1-foundations/05-repetition.md). See also *Loop*.

**Information hiding**: A design principle where implementation details are hidden and only a public interface is exposed. Prevents external code from depending on internal details that might change. See also *Encapsulation*, *Hide*.

**Inheritance** (in-HAIR-ih-tuns): A class adopting properties and methods from a parent class. The child "inherits" from the parent and can add or override functionality. Models "is-a" relationships: a Dog is an Animal. See [Chapter 15](../part3-objects/15-inheritance.md). See also *Class*, *Extends*, *Base class*, *Derived class*.

**Init**: **[Zia]** The special method name for initializers (constructors). Called automatically when creating new objects. Example: `expose func init(name: String) { self.name = name; }`. See [Chapter 14](../part3-objects/14-objects.md). See also *Initializer*, *Constructor*.

**Initialization**: Setting a variable's initial value when it's created. In Zia, variables must be initialized: `var count = 0`. See also *Declaration*, *Assignment*.

**Initializer**: A special method that sets up a new object's initial state. **[Zia]** In Zia, uses the `init` function name. Equivalent to constructor in other languages. See [Chapter 14](../part3-objects/14-objects.md). See also *Init*, *Constructor*, *Object*.

**Input**: Data that enters a program from outside - user keyboard input, file contents, network data, etc. See also *Output*, *I/O*.

**Instance**: A specific object created from a class. Each instance has its own copy of the fields. Example: `var fido = Dog("Fido")` creates an instance of Dog. See [Chapter 14](../part3-objects/14-objects.md). See also *Object*, *Class*, *Instantiate*.

**Instantiate** (in-STAN-shee-ate): To create an instance (object) from a class. Example: `var player = Player("Hero")` instantiates a Player. See also *Instance*, *Object*.

**Integer** (IN-tuh-jur): A whole number without a decimal point. Can be positive, negative, or zero. Examples: `42`, `-7`, `0`, `1000000`. In Zia: `i8`, `i16`, `i32`, `i64` (signed), `u8`, `u16`, `u32`, `u64` (unsigned). See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Float*, *Signed*, *Unsigned*.

**Integer division**: Division between two integers that produces an integer result by discarding the fractional part. Example: `10 / 3` equals `3`, not `3.333`. A common source of bugs for beginners. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Division*, *Modulo*.

**Interface** (IN-ter-fase): A contract specifying what methods a class must implement, without providing the implementation. Defines "what" but not "how." Example: a `Drawable` interface requires a `draw()` method. See [Chapter 16](../part3-objects/16-interfaces.md). See also *Implements*, *Abstract*, *Polymorphism*.

**Internal**: **[Zia]** A visibility modifier that makes a member accessible within the same module but not from outside. Between `expose` and `hide`. See [Appendix A](a-zia-reference.md). See also *Expose*, *Hide*, *Module*.

**Interpreter**: A program that executes code directly, line by line, without compiling to machine code first. Slower than compiled code but more flexible. Python and JavaScript are typically interpreted. See also *Compiler*.

**Invariant** (in-VAIR-ee-unt): A condition that must always be true for an object to be in a valid state. Example: a bank account's balance should never be negative. Encapsulation helps protect invariants. See [Chapter 14](../part3-objects/14-objects.md). See also *Encapsulation*.

**Invoke**: To call or execute a function or method. See also *Call*.

**I/O** (Input/Output) (eye-OH): The flow of data into and out of a program. Reading files, user input, network communication, and displaying output are all I/O operations. See [Chapter 9](../part2-building-blocks/09-files.md). See also *Input*, *Output*.

**Is-a relationship**: The relationship modeled by inheritance. If Dog extends Animal, we say "a dog is an animal." Use inheritance when this relationship genuinely holds. See [Chapter 15](../part3-objects/15-inheritance.md). See also *Inheritance*, *Has-a relationship*.

**Iteration** (it-er-AY-shun): (1) One pass through a loop. A loop with 5 iterations runs its body 5 times. (2) The process of repeating. See [Chapter 5](../part1-foundations/05-repetition.md). See also *Loop*, *Iterate*.

**Iterate**: To repeat; to go through items one at a time. "Iterate over an array" means process each element in turn. See also *Iteration*, *Loop*.

---

## J

**Join**: (1) To wait for a thread to complete: `thread.Join()`. (2) To combine strings with a separator: `Str.Join(",", items)` produces `"a,b,c"` from a list of strings. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Thread*, *String*.

**JSON** (JavaScript Object Notation) (JAY-son): A text format for structured data, widely used for configuration files and data exchange. Human-readable. Example: `{"name": "Alice", "age": 30}`. See [Chapter 23](../part4-applications/23-data-formats.md). See also *Serialization*, *XML*.

---

## K

**Key**: In a map/dictionary, the value used to look up an associated value. In `ages["Alice"] = 30`, "Alice" is the key and 30 is the value. See [Chapter 6](../part1-foundations/06-collections.md). See also *Map*, *Value*, *Key-value pair*.

**Key-value pair**: An association between a key and its corresponding value in a map. The key is used to retrieve the value. See also *Key*, *Map*.

**Keyword**: A reserved word with special meaning in the programming language. Cannot be used as variable names. Examples in Zia: `if`, `func`, `class`, `var`, `final`, `while`, `for`, `return`. See [Appendix A](a-zia-reference.md). See also *Identifier*, *Reserved word*.

---

## L

**Lambda** (LAM-duh): An anonymous (unnamed) function, often used inline. Short and concise, created on the fly. Example: `(x: Integer) => x * 2` is a lambda that doubles its input. From lambda calculus, a mathematical system. See [Appendix A](a-zia-reference.md). See also *Anonymous function*, *Closure*.

**Length**: The number of elements in a collection or characters in a string. `"Hello".length` is 5. `[1, 2, 3].length` is 3. See also *Array*, *String*.

**Library**: A collection of reusable code - functions, entities, and utilities - that you can use in your programs. Like a toolbox of pre-built components. See [Chapter 13](../part2-building-blocks/13-stdlib.md). See also *Module*, *Standard library*, *Framework*.

**LIFO**: Last In, First Out. A stack behavior where the last item added is the first item removed. Like a stack of plates. See also *Stack*, *FIFO*.

**List**: An ordered collection of elements, similar to an array. In some languages, lists can grow and shrink dynamically. See also *Array*, *Collection*.

**Literal**: A fixed value written directly in code, representing itself. Examples: `42` (number literal), `"hello"` (string literal), `true` (boolean literal), `[1, 2, 3]` (array literal). See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Value*, *Constant*.

**Local variable**: A variable declared inside a function or block, accessible only within that scope. Disappears when the scope ends. Opposite of global variable. See [Chapter 7](../part1-foundations/07-functions.md). See also *Scope*, *Global variable*.

**Logic**: The system of reasoning. In programming, logic determines program flow through conditions and boolean operations. See also *Boolean*, *Logical operator*.

**Logical operator**: An operator that works with boolean values. `&&` (AND), `||` (OR), `!` (NOT). Used to combine or modify conditions. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Boolean*, *Operator*.

**Loop**: Code that repeats until a condition is met. Includes `while` loops (repeat while condition is true) and `for` loops (iterate over a range or collection). See [Chapter 5](../part1-foundations/05-repetition.md). See also *While loop*, *For loop*, *Iteration*.

---

## M

**Magic number**: A numeric literal in code without explanation. Bad practice because its meaning is unclear. Example: `if (age > 17)` - what's special about 17? Better: `final VOTING_AGE = 18; if (age >= VOTING_AGE)`. See also *Constant*.

**Main function**: The entry point of a program - the function that runs first. In Zanna, typically called `start()`. See [Chapter 2](../part1-foundations/02-first-program.md).

**Map**: A collection of key-value pairs where each key is associated with a value. Also called dictionary, hash map, or associative array. Example: mapping names to phone numbers. See [Chapter 6](../part1-foundations/06-collections.md). See also *Key*, *Value*, *Hash map*.

**Match statement**: A control flow statement that compares a value against multiple patterns and executes the matching case. More powerful than multiple if/else statements. Example: `match color { RED => ..., GREEN => ..., _ => ... }`. See [Appendix A](a-zia-reference.md). See also *Pattern matching*.

**Member**: Something that belongs to a class - either a field (data) or a method (behavior). See also *Field*, *Method*, *Class*.

**Memory**: Storage where programs keep data while running. Includes registers, cache, RAM, and disk. Programs read from and write to memory constantly. See [Chapter 1](../part1-foundations/01-the-machine.md). See also *RAM*, *Stack*, *Heap*.

**Method**: A function that belongs to a class and operates on its data. Called on objects using dot notation: `player.takeDamage(10)`. Has access to `self` (the object it belongs to). See [Chapter 14](../part3-objects/14-objects.md). See also *Function*, *Self*, *Class*.

**Modular**: Organized into separate, independent modules. Modular code is easier to understand, test, and maintain. See also *Module*.

**Module**: A self-contained unit of code that can be imported by other code. Groups related functionality together. Like a chapter in a book. See [Chapter 12](../part2-building-blocks/12-modules.md). See also *Import*, *Export*, *Package*.

**Modulo**: See *%*.

**Mutator**: A method that modifies an object's state. Also called a "setter." Example: `setName(newName)` changes the name field. See [Chapter 14](../part3-objects/14-objects.md). See also *Accessor*, *Setter*.

**Mutable** (MYOO-tuh-bul): Can be changed after creation. A `var` variable is mutable - you can assign new values to it. Opposite of immutable. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Immutable*, *Var*.

**Mutex** (Mutual Exclusion) (MYOO-tex): A synchronization primitive ensuring only one thread can access a protected resource at a time. Like a lock on a door - only one person can enter at a time. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Thread*, *Deadlock*, *Race condition*.

---

## N

**Namespace**: A container that groups related code and prevents naming conflicts. Two functions can have the same name if they're in different namespaces. Example: `Zanna.Math.Sqrt()` is in the `Zanna.Math` namespace. See also *Module*, *Scope*.

**Native code**: Machine code that runs directly on the CPU, without interpretation. Compiled programs produce native code. Fastest execution but platform-specific. See also *Compiler*, *IL*.

**Nested**: Placed inside something else. Nested loops are loops inside loops. Nested conditionals are if statements inside if statements. Can become hard to read if too deeply nested.

**Newline**: A character that starts a new line. Written as `\n` in strings. When printed, moves the cursor to the beginning of the next line. See also *Escape character*.

**Null** (NUHL): A special value representing "no value" or "nothing." Indicates the absence of an object or meaningful value. Can cause errors if you try to use a null value. In Zia, nullable types use `?`: `var x: Integer? = null`. See [Appendix A](a-zia-reference.md). See also *Nullable*, *Nil*.

**Null coalescing**: Providing a default value when something is null. In Zia: `value ?? default` returns `value` if not null, otherwise `default`. See [Appendix A](a-zia-reference.md). See also *Null*, *Optional*.

**Nullable**: A type that can hold either a value or null. Written with `?` in Zia: `string?` can be a string or null. See [Appendix A](a-zia-reference.md). See also *Null*, *Optional*.

---

## O

**Object**: An instance created from a class, containing data (fields) and behavior (methods). Each object has its own state. Example: if `Dog` is a class, `fido` and `rex` are objects of type Dog. See [Chapter 14](../part3-objects/14-objects.md). See also *Class*, *Instance*.

**Object-oriented programming** (OOP): A programming paradigm organizing code around objects that combine data and behavior. Core concepts include encapsulation, inheritance, and polymorphism. See [Part III](../part3-objects/14-objects.md). See also *Encapsulation*, *Inheritance*, *Polymorphism*.

**Operand**: A value that an operator works on. In `5 + 3`, both 5 and 3 are operands of the `+` operator. See also *Operator*.

**Operator**: A symbol that performs an operation on values. Arithmetic operators (`+`, `-`, `*`, `/`), comparison operators (`==`, `<`), logical operators (`&&`, `||`), and more. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Operand*, *Expression*.

**Optional**: A type that may or may not have a value. Similar to nullable. Prevents null pointer errors by making the possibility of absence explicit. See [Appendix A](a-zia-reference.md). See also *Nullable*, *Null*.

**Optional chaining**: Safely accessing properties or methods that might be null. In Zia: `obj?.property` returns null instead of crashing if `obj` is null. See [Appendix A](a-zia-reference.md). See also *Null*, *Nullable*.

**Output**: Data that leaves a program - text displayed on screen, files written, network data sent, etc. See also *Input*, *I/O*.

**Overflow**: When a calculation produces a result too large to fit in the available storage. Example: adding 1 to the maximum integer value. Can cause unexpected behavior. See also *Underflow*.

**Overload**: Multiple functions or methods with the same name but different parameters. The correct version is chosen based on the arguments provided. Example: `init(name)` and `init(name, age)`. See also *Override*.

**Override**: Replacing a parent class's method with a new implementation in a child class. The child's version is used instead of the parent's. Marked with `override` keyword. See [Chapter 15](../part3-objects/15-inheritance.md). See also *Inheritance*, *Virtual method*.

---

## P

**Package**: A collection of related modules distributed together. Like a library but often larger and more comprehensive. See also *Module*, *Library*.

**Parallel**: Happening at the same time. Parallel execution means multiple operations running simultaneously on different CPU cores. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Parallelism*, *Sequential*.

**Parallelism**: Multiple tasks executing simultaneously on different CPU cores. True parallel execution requires multiple cores. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Concurrency*, *Thread*, *Core*.

**Parameter**: A variable in a function definition that receives an argument value. The placeholder that arguments fill. In `func greet(name: String)`, `name` is a parameter. See [Chapter 7](../part1-foundations/07-functions.md). See also *Argument* (the related but different concept).

**Parent class**: See *Base class*.

**Parse** (PARS): To analyze text and convert it to a structured format. Parsing `"42"` as an integer gives the number 42. Parsing JSON text creates a data structure. See [Chapter 23](../part4-applications/23-data-formats.md). See also *Serialize*.

**Pascal case**: A naming convention where each word starts with a capital letter, including the first. Example: `PlayerScore`, `BankAccount`. Often used for class names. Also called upper camel case. See also *Camel case*.

**Pass by reference**: Passing a reference to data rather than a copy. Changes to the parameter affect the original. See also *Pass by value*, *Reference*.

**Pass by value**: Passing a copy of data to a function. Changes to the parameter don't affect the original. See also *Pass by reference*.

**Path**: The location of a file or directory in the file system. Example: `/Users/alice/Documents/code.vpr`. See [Chapter 9](../part2-building-blocks/09-files.md). See also *File*.

**Pattern matching**: Checking data against patterns and extracting parts. More powerful than simple equality checking. Used in `match` statements. See [Appendix A](a-zia-reference.md). See also *Match statement*.

**Performance**: How fast and efficiently a program runs. Involves execution speed, memory usage, and resource consumption. See [Chapter 26](../part5-mastery/26-performance.md). See also *Optimization*.

**Pointer**: A value that stores a memory address - a reference to where data is located. Pointers allow indirect access to data. Some languages hide pointers; others make them explicit. See also *Reference*, *Memory*.

**Polymorphism** (pol-ee-MOR-fizm): The ability to treat different types uniformly through a common interface. A function that takes `Animal` can work with `Dog`, `Cat`, or any Animal subtype. From Greek "many forms." See [Chapter 17](../part3-objects/17-polymorphism.md). See also *Interface*, *Inheritance*, *Dynamic dispatch*.

**Precedence**: The order in which operators are evaluated. Multiplication has higher precedence than addition, so `2 + 3 * 4` equals `14`, not `20`. Use parentheses to override precedence. See [Appendix A](a-zia-reference.md). See also *Operator*.

**Primitive type**: A basic built-in type provided by the language. Integers, floats, booleans, and characters are primitives. Not composed of other types. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Data type*, *Value type*.

**Print**: To display text on the screen. In Zia: `Zanna.Terminal.Say()` prints with a newline; `Zanna.Terminal.Print()` prints without. See [Chapter 2](../part1-foundations/02-first-program.md).

**Private**: Accessible only within the class that defines it. Hidden from external code. **[Zia]** Use the `hide` keyword instead of "private." See [Chapter 14](../part3-objects/14-objects.md). See also *Hide*, *Public*, *Encapsulation*.

**Procedure**: A function that performs an action but doesn't return a value. Also called a subroutine or void function. In Zia, a function without a return type. See also *Function*, *Void*.

**Program**: A set of instructions that tells a computer what to do. Written in a programming language, then compiled or interpreted to run.

**Property**: A field with associated getter and/or setter methods. Provides controlled access to data. In some languages, properties look like fields but run code when accessed. See also *Field*, *Accessor*, *Mutator*.

**Protected**: **[Zia]** A visibility modifier that allows access within the class and its subclasses, but not from outside the inheritance hierarchy. Between `expose` and `hide`. See [Appendix A](a-zia-reference.md). See also *Expose*, *Hide*.

**Protocol**: A set of rules for communication or interaction. HTTP is a protocol for web communication. TCP is a network protocol. Interfaces can be seen as protocols between code components. See also *Interface*.

**Pseudocode** (SOO-doh-code): Informal description of an algorithm using natural language and programming-like structure. Helps plan code before writing it. Not meant to run, just to communicate ideas.

**Public**: Accessible from anywhere, with no restrictions. **[Zia]** Use the `expose` keyword (which is the default for methods). See [Chapter 14](../part3-objects/14-objects.md). See also *Expose*, *Private*.

---

## Q

**Query**: A request for data, often from a database or collection. Example: finding all users over age 18, or searching for items matching criteria. See also *Filter*.

**Queue** (KYOO): A data structure where elements are added at one end and removed from the other (FIFO - First In, First Out). Like a line of people waiting. See also *FIFO*, *Stack*.

---

## R

**Race condition**: A bug where program behavior depends on the unpredictable timing of concurrent operations. Two threads accessing the same data can produce different results depending on who gets there first. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Thread*, *Mutex*, *Deadlock*.

**RAM** (Random Access Memory) (RAM): Fast, temporary computer memory that holds running programs and their data. Contents are lost when power is off. See [Chapter 1](../part1-foundations/01-the-machine.md). See also *Memory*, *CPU*.

**Range**: A sequence of numbers from a start to an end. In Zia: `0..10` (exclusive, 0-9) or `0..=10` (inclusive, 0-10). Used in for loops. See [Appendix A](a-zia-reference.md). See also *For loop*.

**Recursion** (ree-KUR-zhun): A function calling itself to solve a problem by breaking it into smaller instances of the same problem. Must have a base case to stop. Example: calculating factorial. Can be elegant but watch for stack overflow. See [Chapter 7](../part1-foundations/07-functions.md). See also *Base case*, *Stack overflow*.

**Refactoring** (ree-FAK-tor-ing): Restructuring code without changing its behavior. Improving code quality, readability, or performance while keeping functionality identical. See also *DRY*.

**Reference**: A value that points to an object rather than containing the object directly. Multiple references can point to the same object. See also *Pointer*, *Value type*, *Reference type*.

**Reference type**: A type where variables hold references to data rather than the data itself. Objects are typically reference types. Contrast with value types. See also *Value type*, *Reference*.

**Regex** (Regular Expression) (REJ-ex): A pattern for matching text. Powerful but complex. Example: `\d+` matches one or more digits. Used for searching, validation, and text processing. See [Chapter 8](../part2-building-blocks/08-strings.md). See also *Pattern matching*, *String*.

**Repository**: A storage location for code, typically managed by version control. Contains all project files and their history. Also called a repo. See also *Git*, *Version control*.

**Reserved word**: See *Keyword*.

**Return**: (1) To send a value back from a function: `return result;` (2) The `return` statement that exits a function. See [Chapter 7](../part1-foundations/07-functions.md). See also *Return value*, *Function*.

**Return type**: The type of value a function returns. Declared after `->` in Zia: `func add(a: Integer, b: Integer) -> Integer`. See [Chapter 7](../part1-foundations/07-functions.md). See also *Function*, *Return value*.

**Return value**: The value a function sends back when it completes. The result of calling the function. `add(2, 3)` has return value 5. See [Chapter 7](../part1-foundations/07-functions.md). See also *Function*, *Return*.

**Run**: To execute a program. See also *Execute*, *Runtime*.

**Runtime**: (1) The period when a program is executing, as opposed to compile-time. (2) The supporting software that runs programs - memory management, standard library, etc. See [Chapter 25](../part5-mastery/25-how-zanna-works.md). See also *Compile-time*, *Runtime error*.

**Runtime error**: An error that occurs while a program is running, not during compilation. Examples: division by zero, file not found, null pointer access. See [Chapter 10](../part2-building-blocks/10-errors.md). See also *Compile-time*, *Exception*.

---

## S

**Scope**: The region of code where a variable is accessible. Variables exist within their scope and are invisible outside it. Creates isolation between different parts of code. See [Chapter 7](../part1-foundations/07-functions.md). See also *Local variable*, *Global variable*, *Block*.

**Self**: **[Zia]** A reference to the current object inside a method. Allows methods to access the object's fields and other methods. Example: `self.name = name`. See [Chapter 14](../part3-objects/14-objects.md). See also *This*, *Method*, *Object*.

**Semantic error**: Code that is syntactically valid but doesn't do what you intended. The hardest bugs to find because the program runs without complaints but produces wrong results. See also *Syntax error*, *Bug*.

**Semicolon**: The `;` character. In Zia, ends statements. Example: `var x = 5;`. Missing semicolons cause syntax errors.

**Sequential**: Happening one after another, in order. Sequential execution runs each line before the next. Opposite of parallel. See also *Parallel*.

**Serialization** (seer-ee-al-ih-ZAY-shun): Converting data to a format for storage or transmission. Turns objects into JSON, binary data, or other formats that can be saved or sent. See [Chapter 23](../part4-applications/23-data-formats.md). See also *Deserialization*, *JSON*, *Parse*.

**Set**: A collection of unique values with no duplicates. Adding the same value twice has no effect. Useful for tracking "have I seen this before?" See [Chapter 6](../part1-foundations/06-collections.md). See also *Collection*, *Map*.

**Setter**: See *Mutator*.

**Side effect**: Something a function does besides returning a value - modifying external state, printing output, writing files. Pure functions have no side effects. See also *Pure function*.

**Signature**: The "shape" of a function: its name, parameters, and return type. Two functions with the same signature are interchangeable. Example: `func add(a: Integer, b: Integer) -> Integer` has a specific signature.

**Signed**: A numeric type that can represent negative numbers. `i64` is signed (-9223372036854775808 to 9223372036854775807). See also *Unsigned*, *Integer*.

**Singleton**: A design pattern where a class allows only one instance to exist. Used for shared resources like configuration. See [Chapter 18](../part3-objects/18-patterns.md). See also *Design pattern*.

**Snake case**: A naming convention using lowercase letters with underscores between words. Example: `player_score`, `total_count`. Common in Python and for constants. See also *Camel case*.

**Socket**: An endpoint for network communication. A program creates a socket to send and receive data over a network. See [Chapter 22](../part4-applications/22-networking.md). See also *TCP*, *UDP*, *Networking*.

**Source code**: The human-readable text that programmers write. What you see in code files. Gets compiled or interpreted into something the computer executes. See also *Compiler*, *Code*.

**Spawn**: To create and start a new thread. `Thread.Start(func() { ... })` creates a thread that runs the given function. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Thread*.

**Stack**: (1) A region of memory for function calls and local variables. Fast but limited in size. (2) A LIFO (Last In, First Out) data structure. See also *Heap*, *LIFO*, *Call stack*.

**Stack overflow**: An error when the call stack runs out of space, usually from too much recursion. Each function call uses stack space; too many nested calls exhaust it. See also *Recursion*, *Stack*.

**Stack trace**: A list of function calls that led to an error. Shows where the program was and how it got there. Essential for debugging. See also *Call stack*, *Debug*.

**Standard library**: The library of functions, types, and utilities that comes with a programming language. Available without extra installation. In Zanna, includes `Zanna.Terminal`, `Zanna.Math`, `Zanna.Time`, etc. See [Chapter 13](../part2-building-blocks/13-stdlib.md). See also *Library*.

**State**: The current condition of a program or object - all the values of all variables at a particular moment. Objects have state (their field values) that can change over time. See [Chapter 14](../part3-objects/14-objects.md). See also *Mutable*, *Immutable*.

**Statement**: A complete instruction - a single step the program takes. Declarations, assignments, function calls, control flow are all statements. Statements do things; expressions produce values. See [Chapter 2](../part1-foundations/02-first-program.md). See also *Expression*.

**Static**: (1) Belonging to the class itself rather than to instances. A static method can be called without creating an object. (2) Known at compile-time rather than runtime. See also *Instance*, *Static typing*.

**Static typing**: Type checking done at compile time. Variables have fixed types that are known before the program runs. Zia uses static typing. Catches many errors early. See also *Dynamic typing*, *Type*.

**String**: A sequence of characters (text). Written in double quotes: `"Hello, World!"`. Can contain letters, numbers, spaces, punctuation. See [Chapter 3](../part1-foundations/03-values-and-names.md) and [Chapter 8](../part2-building-blocks/08-strings.md). See also *Character*, *Concatenation*.

**String interpolation**: Embedding expressions inside a string. In Zia: `"Hello, ${name}!"` replaces `${name}` with the value of `name`. Cleaner than concatenation. See [Appendix A](a-zia-reference.md). See also *Format string*.

**Struct/Structure**: A composite type grouping related values together, typically without behavior. **[Zia]** Declared with the `struct` keyword. See [Chapter 11](../part2-building-blocks/11-structures.md). See also *Class*.

**Subclass**: A class that inherits from another class (the superclass). The child in an inheritance relationship. `Dog extends Animal` makes `Dog` a subclass of `Animal`. See [Chapter 15](../part3-objects/15-inheritance.md). See also *Superclass*, *Inheritance*, *Derived class*.

**Subroutine**: See *Procedure*, *Function*.

**Super**: **[Zia]** Keyword to access the parent class's methods, especially in initializers. `super(name)` calls the parent's initializer with `name`. See [Chapter 15](../part3-objects/15-inheritance.md). See also *Inheritance*, *Base class*.

**Superclass**: The parent class that another class inherits from. `Animal` is the superclass of `Dog` if `Dog extends Animal`. See [Chapter 15](../part3-objects/15-inheritance.md). See also *Subclass*, *Base class*, *Inheritance*.

**Synchronization**: Coordinating concurrent access to shared resources. Ensuring threads don't interfere with each other. Uses mechanisms like mutexes and channels. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Mutex*, *Thread*, *Race condition*.

**Synchronous** (SIN-kron-us): Operations that block execution until complete. The program waits for each operation to finish before continuing. Opposite of asynchronous. See also *Asynchronous*, *Blocking*.

**Syntax**: The rules for valid code structure in a language - grammar, punctuation, structure. Syntax errors occur when code doesn't follow these rules. See also *Syntax error*, *Semantics*.

**Syntax error**: A violation of the language's grammar rules. The code is malformed and can't be understood. Example: missing semicolons, unmatched braces, typos in keywords. Caught by the compiler. See also *Syntax*, *Semantic error*.

---

## T

**Tab**: A whitespace character that creates horizontal space, typically moving to the next tab stop. Written as `\t` in strings. Used for indentation and alignment. See also *Escape character*.

**TCP** (Transmission Control Protocol) (tee-see-PEE): A reliable network protocol for ordered data delivery. Guarantees data arrives in order without loss. Used for web browsing, email, file transfer. See [Chapter 22](../part4-applications/22-networking.md). See also *UDP*, *Protocol*, *Socket*.

**Template**: See *Generic*.

**Terminal**: A text-based interface for interacting with a computer. Also called console or command line. Programs can print to and read from the terminal. See [Chapter 2](../part1-foundations/02-first-program.md).

**Ternary operator**: An operator with three operands. The conditional operator `condition ? valueIfTrue : valueIfFalse` is the most common example. See also *Operator*.

**Test**: Code that verifies other code works correctly. Tests call functions with known inputs and check for expected outputs. Essential for code quality. See [Chapter 27](../part5-mastery/27-testing.md). See also *Unit test*, *Bug*.

**This**: In many languages, refers to the current object inside a method. **[Zia]** Uses `self` instead. See also *Self*.

**Thread**: An independent sequence of execution within a program. Multiple threads can run concurrently. All threads in a process share memory. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Concurrency*, *Thread pool*, *Race condition*.

**Thread pool**: A collection of reusable threads for executing tasks. Instead of creating new threads for each task, tasks are queued and executed by pool threads. More efficient for many short tasks. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Thread*.

**Thread-safe**: Code that works correctly when called from multiple threads simultaneously. Properly synchronizes access to shared data. See [Chapter 24](../part4-applications/24-concurrency.md). See also *Thread*, *Race condition*, *Mutex*.

**Throw**: To raise an exception, signaling an error. Interrupts normal execution. Must be caught by a try/catch block somewhere up the call stack. Example: `throw ("Something went wrong")`. See [Chapter 10](../part2-building-blocks/10-errors.md). See also *Exception*, *Try*, *Catch*.

**Timeout**: A limit on how long to wait for an operation. If the operation doesn't complete in time, it fails or is cancelled. Prevents programs from waiting forever. See also *Asynchronous*.

**Token**: A basic unit of source code as recognized by the compiler. Keywords, identifiers, literals, and operators are tokens. The compiler first breaks source code into tokens (tokenization/lexing). See also *Compiler*, *Syntax*.

**True**: One of the two boolean values, representing "yes," "on," or "correct." The opposite of `false`. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Boolean*, *False*.

**Truncate**: To cut off part of a value. Integer division truncates the fractional part: `7 / 2` truncates to `3`. File truncation removes content after a certain point.

**Try**: The part of error handling that contains code that might throw an exception. If an exception occurs, execution jumps to the `catch` block. Example: `try { riskyCode(); } catch Error { handleError(); }`. See [Chapter 10](../part2-building-blocks/10-errors.md). See also *Catch*, *Exception*, *Throw*.

**Tuple** (TOO-pul or TUP-ul): A fixed-size ordered collection of values that can have different types. Like a small anonymous struct. Example: `(x, y)` is a tuple of two values. See also *Array*.

**Type**: A classification defining what values a variable can hold and what operations are valid. Every value and expression has a type. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Data type*, *Static typing*, *Type inference*.

**Type conversion**: Changing a value from one type to another. Also called casting. `Zanna.Core.Convert.ToInt64("42")` converts string to integer. Some conversions are automatic; others must be explicit. See also *Parse*.

**Type inference**: The compiler automatically determining types from context, so you don't have to write them explicitly. `var x = 5` infers that x is an integer. See [Appendix A](a-zia-reference.md). See also *Implicit*, *Static typing*.

**Type parameter**: In generics, a placeholder for a type that will be specified later. In `func identity[T](value: T) -> T`, `T` is a type parameter. See also *Generic*.

---

## U

**UDP** (User Datagram Protocol) (yoo-dee-PEE): A fast but unreliable network protocol. Doesn't guarantee delivery or order. Used for video streaming, gaming, and situations where speed matters more than perfect delivery. See [Chapter 22](../part4-applications/22-networking.md). See also *TCP*, *Protocol*.

**Underflow**: When a calculation produces a result too small to represent, typically going below the minimum value. Opposite of overflow. See also *Overflow*.

**Unicode** (YOO-ni-code): A universal standard for representing text characters from all writing systems. Includes Latin letters, Chinese characters, emojis, and more. Modern programs should use Unicode for text. See [Chapter 8](../part2-building-blocks/08-strings.md). See also *UTF-8*, *ASCII*, *Character*.

**Unit test**: A test for a small, isolated piece of code - typically a single function or method. Verifies that individual components work correctly. See [Chapter 27](../part5-mastery/27-testing.md). See also *Test*.

**Unsigned**: A numeric type that can only represent non-negative numbers (0 and positive). `u64` is unsigned (0 to 18446744073709551615). Can represent larger positive numbers than signed types of the same size. See also *Signed*, *Integer*.

**UTF-8** (yoo-tee-eff-ATE): The most common Unicode encoding. Variable-length: ASCII characters use 1 byte; others use more. Backward compatible with ASCII. See also *Unicode*, *ASCII*.

---

## V

**Value**: (1) A single piece of data - a specific number, text, or boolean. The actual data stored in a variable. (2) **[Zia]** In older versions of Zia, `value` was the keyword for struct declarations; it has been replaced by `struct`. See *Struct/Structure*. See [Chapter 11](../part2-building-blocks/11-structures.md). See also *Literal*, *Variable*, *Entity*.

**Value type**: A type where variables hold the actual data directly (not a reference). When assigned or passed, the data is copied. `struct` types in Zia are value types. See [Chapter 11](../part2-building-blocks/11-structures.md). See also *Reference type*, *Value*.

**Var**: **[Zia]** Keyword that declares a mutable variable. Example: `var count = 0`. The value can be changed after creation. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Final*, *Variable*, *Mutable*.

**Variable**: A named storage location for data. Has a name, a type, and a value. The value can change over time (if mutable). Example: `var score = 100`. See [Chapter 3](../part1-foundations/03-values-and-names.md). See also *Constant*, *Type*, *Assignment*.

**Variadic** (vair-ee-AD-ik): A function that accepts a variable number of arguments. In Zia, use `...` before the type: `func sum(numbers: ...Integer)`. See [Appendix A](a-zia-reference.md). See also *Parameter*.

**Version control**: A system for tracking changes to code over time. Allows reverting to previous versions, branching, and collaboration. Git is the most popular. See also *Git*, *Repository*.

**Virtual machine** (VM): Software that executes code in an isolated environment. Programs run on the VM rather than directly on hardware. The Java Virtual Machine and .NET CLR are examples. See also *Runtime*, *IL*.

**Virtual method**: A method that can be overridden in subclasses. When called, the actual implementation used depends on the runtime type of the object. See [Chapter 15](../part3-objects/15-inheritance.md). See also *Override*, *Dynamic dispatch*, *Polymorphism*.

**Visibility**: The accessibility of code elements - who can see and use them. Controlled by modifiers like `expose` (public), `hide` (private), `protected`, and `internal`. See [Chapter 14](../part3-objects/14-objects.md). See also *Expose*, *Hide*, *Encapsulation*.

**Void**: Indicating a function returns no value. A void function performs an action but doesn't produce a result. In Zia, simply omit the return type. See [Chapter 7](../part1-foundations/07-functions.md). See also *Function*, *Return value*.

---

## W

**WebSocket**: A protocol providing full-duplex communication over a single TCP connection. Unlike HTTP, keeps connection open for ongoing two-way communication. Used for real-time applications like chat. See [Chapter 22](../part4-applications/22-networking.md). See also *HTTP*, *TCP*.

**While loop**: A loop that repeats as long as a condition is true. Checks the condition before each iteration. Example: `while (count < 10) { ... }`. See [Chapter 5](../part1-foundations/05-repetition.md). See also *Loop*, *For loop*, *Condition*.

**Whitespace**: Characters that create space but don't display: spaces, tabs, newlines. Usually ignored in code structure but significant in strings. See also *Indentation*.

**Wrapper**: Code that surrounds other code to add functionality or change its interface. A wrapper function might add logging, error handling, or simplify a complex API.

---

## X

**XML** (Extensible Markup Language) (ex-em-ELL): A text format for structured data using tags. More verbose than JSON. Example: `<person><name>Alice</name></person>`. Common in configuration files and data exchange. See [Chapter 23](../part4-applications/23-data-formats.md). See also *JSON*, *Serialization*.

---

## Y

**Yield**: In some languages, a statement that pauses a function and returns a value, allowing the function to be resumed later. Used in generators and iterators. See also *Iterator*, *Generator*.

---

## Z

**Zero-indexed**: Counting that starts at 0 rather than 1. Arrays in most programming languages are zero-indexed: the first element is at index 0, the second at index 1, etc. A common source of "off-by-one" errors for beginners. See [Chapter 6](../part1-foundations/06-collections.md). See also *Index*, *Array*.

---

## Zia Quick Reference

Key Zia-specific terms and their equivalents in other languages:

| Zia | Other Languages | Meaning |
|-----------|-----------------|---------|
| `class` | `class` | Template for objects with data and behavior |
| `struct` | `struct`, `record` | Template for pure data without identity |
| `expose` | `public` | Accessible from anywhere |
| `hide` | `private` | Accessible only within the class |
| `protected` | `protected` | Accessible within class and subclasses |
| `internal` | `internal` | Accessible within the module |
| `init` | constructor | Method that initializes new objects |
| `self` | `this` | Reference to the current object |
| `final` | `const`, `readonly` | Immutable variable |
| `var` | `let`, `var` | Mutable variable |
| `func` | `function`, `def` | Function definition |
| `extends` | `extends`, `:` | Inheritance |
| `implements` | `implements`, `:` | Interface implementation |

See [Appendix A: Zia Reference](a-zia-reference.md) for complete language documentation.

---

*[Back to Table of Contents](../README.md) | [Prev: Appendix D](d-error-messages.md)*
