---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 3: Values and Names

In the last chapter, we displayed text on the screen. But our program always showed the same message. Real programs work with data that changes -- numbers, text, true/false answers. They remember things. They calculate. They respond differently to different situations.

This chapter introduces the atoms of programming: *values* and the *names* we give them. These concepts are absolutely fundamental. Everything else you'll learn -- decisions, loops, functions, data structures -- builds on what we cover here. Take your time. Make sure you understand these ideas deeply before moving on.

---

## What ARE Values, Really?

Before we dive into code, let's think about what we're actually doing when we program.

A computer, at its core, is a machine that manipulates information. But "information" is a vague word. What does a computer actually work with? The answer is *values*.

A value is a single piece of data. A specific number. A specific piece of text. A specific answer to a yes/no question. When you write `42` in a program, you've created a value. When you write `"Hello"`, that's a value. When you write `true`, that's a value.

Values are the *nouns* of programming. If a program is a story, values are the things the story is about -- the characters, objects, and facts that the program manipulates.

**Why do computers need values?**

Think about what you want a computer to do. You might want it to:
- Calculate how much tax you owe (requires numbers: your income, the tax rate)
- Display a personalized message (requires text: your name)
- Decide whether you're old enough to vote (requires a number for your age, and produces a yes/no answer)
- Track your score in a game (requires numbers that change as you play)

Every one of these tasks involves working with specific pieces of data -- values. Without values, a program couldn't represent *anything*. It couldn't store your name, calculate your balance, or track whether you've won a game. Values are the raw material that programs process.

**Values are not the same as text on screen**

This is an important distinction. When we write:

```zia
bind Zanna.Terminal;

Say(42);
```

Two different things are happening:
1. The *value* `42` exists in the computer's memory
2. That value gets *displayed* on screen as the characters "4" and "2"

The value and its display are related but separate. The value `42` could be displayed as "42", or as "forty-two", or as "XLII" (Roman numerals), or not displayed at all -- it could just be used in a calculation. The value exists independently of how (or whether) we show it to the user.

---

## Types: Not All Values Are Created Equal

Here's a question: what's `5 + 3`?

You probably answered `8`. Now what's `"5" + "3"`?

If you said `"53"`, you're thinking like a programmer. If you said `8`, that's the natural human interpretation -- but computers see it differently.

The difference is *type*. In the first case, `5` and `3` are *numbers* -- the computer adds them mathematically. In the second case, `"5"` and `"3"` are *strings* (text) -- the computer joins them together like words.

**Why do types exist?**

Types exist because different kinds of data need different operations. Consider:

- You can divide 10 by 2 and get 5. Can you divide "Hello" by "World"? That's nonsense.
- You can make "Hello" uppercase to get "HELLO". Can you make the number 42 uppercase? That's nonsense.
- You can ask "is 5 less than 10?" and get a yes/no answer. Can you ask "is 'cat' less than 'dog'"? Well, actually you can (alphabetically), but the answer means something different than numerical comparison.

Types tell the computer what operations make sense for each value. They prevent nonsensical operations and enable meaningful ones.

**Zanna's basic types**

Zanna has three fundamental types:

**Numbers** are for math, counting, measuring, and anything quantitative.
```zia
42        // a whole number
-7        // negative numbers work too
3.14159   // numbers with decimal points
1000000   // big numbers
0         // zero is a number!
```

**Strings** are for text -- any sequence of characters. They're called "strings" because they're like beads on a string: a sequence of individual characters strung together.
```zia
"Hello"           // a word
"Enter your name:"// a prompt
""                // an empty string (zero characters)
"12345"           // digits AS TEXT, not as a number
"a"               // even a single character is a string
```

**Booleans** are for yes/no, true/false, on/off questions. Named after mathematician George Boole, who invented the algebra of logic. There are exactly two boolean values:
```zia
true    // yes, correct, on, 1
false   // no, incorrect, off, 0
```

These three types -- numbers, strings, booleans -- are surprisingly powerful. With just these, you can build calculators, games, text processors, quiz programs, and much more. Later we'll learn about collections (lists of values) and custom types (defining your own kinds of data), but these three are the foundation everything else builds on.

**What problems do types solve?**

Types prevent bugs by catching mistakes early. Imagine you accidentally wrote:

```zia
var price = "100";    // Oops, used quotes - this is text, not a number!
var tax = price * 0.08;
```

In some languages, this might fail silently or produce bizarre results. In Zanna, the type system catches the error: you can't multiply text by a number. This forces you to fix the bug before your program even runs.

Types also make code clearer. When you see `var age = 25`, you know `age` is a number. When you see `var name = "Alice"`, you know `name` is text. The types communicate meaning.

**What happens if you use the wrong type?**

Depending on the situation:

1. **Compile-time error**: Zanna refuses to run your program and tells you what's wrong. This is the best case -- you catch the bug immediately.

2. **Unexpected behavior**: Some operations work on multiple types but do different things. `5 + 3` gives `8` (addition), but `"5" + "3"` gives `"53"` (concatenation). If you meant to add numbers but accidentally used strings, you get the wrong answer.

3. **Runtime error**: Sometimes type mismatches only show up while the program is running -- for example, if user input can't be converted to a number. We'll learn how to handle these gracefully.

The key lesson: always know what type your values are, and make sure you're using operations appropriate for that type.

---

## Variables: Giving Names to Values

Here's a problem. You want to write a program that:
1. Asks the user for their name
2. Greets them by name
3. Asks for their age
4. Tells them what year they were born

You need to *remember* the name and age so you can use them later. But how do you remember things in a program?

This is what *variables* are for. A variable is a name attached to a value.

### The Labeled Box Analogy

Think of computer memory as a warehouse full of boxes. Each box can hold one thing. A variable is like putting a label on a box.

```zia
var age = 25;
```

This instruction says: "Get a box, put the value `25` inside it, and stick a label that says `age` on the front."

Now, whenever your program mentions `age`, the computer knows to go find the box labeled `age` and look inside to see what value is there.

```zia
bind Zanna.Terminal;

var age = 25;
Say(age);  // Goes to the 'age' box, finds 25, displays it
```

### The Name Tag Analogy

Another way to think about it: imagine values walking around a party. A variable is like a name tag stuck on a value.

When you write `var score = 100`, you're putting a name tag that says "score" on the value `100`. Later, when you need that value, you can just call out "score!" and the right value responds.

### The Nickname Analogy

Or think of variables like nicknames. The value `3.14159265358979` is unwieldy to write every time. So we give it a nickname:

```zia
var pi = 3.14159265358979;
```

Now we can just say `pi` instead of typing all those digits each time.

### Why do we need variables?

Variables serve several crucial purposes:

**1. Memory**: Programs need to remember things for later use.
```zia
bind Zanna.Terminal;

var name = TryReadLine().UnwrapOrStr("");  // remember what the user typed
// ... 50 lines of code later ...
Say("Goodbye, " + name);  // Still remember their name!
```

**2. Clarity**: Names make code readable.
```zia
bind Zanna.Terminal;

// What does this mean?
Say(100 * 0.08);

// Much clearer!
var price = 100;
var taxRate = 0.08;
Say(price * taxRate);
```

**3. Reusability**: Use the same value in multiple places without retyping it.
```zia
bind Zanna.Terminal;

var greeting = "Welcome to Zanna Programming!";
Say(greeting);
// ... later ...
Say(greeting);  // Same message, no retyping
```

**4. Change**: Update a value in one place, and everywhere using that variable sees the update.
```zia
var score = 0;
// ... player scores ...
score = score + 10;
// Now every place that uses 'score' sees the new value
```

### Variables in action

Let's see variables at work:

```zia
module Variables;
bind Zanna.Terminal;

func start() {
    var name = "Alice";
    var age = 30;

    Say("Name: " + name);
    Say("Age: " + age);
}
```

Output:
```text
Name: Alice
Age: 30
```

We created two variables: `name` holding the text `"Alice"`, and `age` holding the number `30`. Then we used them in our `Say` calls.

Notice the `+` between `"Name: "` and `name`. When you use `+` with strings, it joins them together. `"Name: " + "Alice"` becomes `"Name: Alice"`. This is called *concatenation* (from Latin *concatenare*, "to chain together").

### Common Confusion: Variables vs. Values

Variables and values are different things:
- A *value* is data: `42`, `"Hello"`, `true`
- A *variable* is a name that refers to a value: `age`, `name`, `isReady`

The variable `age` is not the same thing as the value `25`. The variable is the *name*; the value is the *contents*. You can change what's inside a variable, but the name stays the same (we'll explore this soon).

Think of it like a person's job title vs. the person. "The President" is a name/title. The actual person in that role can change, but the title remains "The President."

---

## Choosing Good Names

You can name variables almost anything, but the names you choose dramatically affect how easy your code is to understand. Consider:

```zia
// Mystery code
var x = 100;
var y = 5;
var z = x * y;
```

What does this program do? You can figure it out by studying it, but it takes mental effort.

Now the same program with good names:

```zia
// Clear code
var pricePerUnit = 100;
var quantity = 5;
var totalCost = pricePerUnit * quantity;
```

Immediately clear! You understand not just *what* the program does, but *why* -- it's calculating a total cost from a unit price and quantity.

Good naming is not about following rules -- it's about communicating with the next person who reads your code. That person might be a colleague, might be you six months from now, might be you tomorrow morning after a good night's sleep has made you forget what you were thinking.

**Rules for names** (what Zanna allows):
- Must start with a letter or underscore (`_`)
- Can contain letters, numbers, underscores
- Can't be a keyword (like `var`, `func`, `if`, `true`)
- Case matters: `age`, `Age`, and `AGE` are three different names

**Conventions** (what programmers do by tradition):
- Use `camelCase` for variables: `firstName`, `totalScore`, `isGameOver`
  - Start with lowercase, capitalize each new word
- Use descriptive names: `count` not `c`, `userName` not `un`
- Boolean variables often start with `is`, `has`, `can`: `isReady`, `hasWon`, `canJump`
  - This makes conditions read naturally: `if (isReady)` reads like English
- Avoid abbreviations unless universally understood: `max` is fine, `wndHndlCnt` is not

**The length guideline**: A variable's name length should roughly correspond to its scope (how much code can see it). Variables used across many lines of code deserve longer, more descriptive names. Variables used in just two adjacent lines can be shorter.

---

## Numbers in Detail

Numbers are probably the most intuitive type -- we all learned to count and do arithmetic long before we started programming. But computers handle numbers in ways that can surprise beginners.

### Two Kinds of Numbers

Zanna has two kinds of numbers:

**Integers** are whole numbers: `1`, `42`, `-7`, `1000000`, `0`. No decimal point. The word comes from Latin *integer*, meaning "whole" or "untouched."

**Floating-point numbers** (or "floats") have decimal points: `3.14`, `0.5`, `-273.15`, `2.0`. The name "floating-point" refers to how computers store these numbers internally (the decimal point can "float" to different positions).

```zia
var count = 10;        // integer - whole number
var price = 19.99;     // float - has decimal
var temperature = -5;  // integer - negative numbers work
var ratio = 0.75;      // float - fractions
var zero = 0;          // integer
var zeroFloat = 0.0;   // float (the .0 makes it a float)
```

**When to use which?**

Use integers for:
- Counting things (people, items, iterations)
- Indexing (the 1st item, the 5th character)
- Anything that can't meaningfully be fractional

Use floats for:
- Measurements (height, weight, temperature)
- Financial calculations (prices, percentages)
- Scientific calculations
- Anything that might involve fractions

### Arithmetic Operations

You can do math with numbers using operators:

```zia
bind Zanna.Terminal;

var a = 10;
var b = 3;

Say(a + b);   // 13 (addition)
Say(a - b);   // 7  (subtraction)
Say(a * b);   // 30 (multiplication)
Say(a / b);   // 3  (division - see below!)
Say(a % b);   // 1  (remainder/modulo)
```

These work exactly like arithmetic you learned in school -- mostly. Division has a surprise, which we'll address shortly.

### Real-World Arithmetic Examples

**Shopping calculation:**
```zia
var itemPrice = 29.99;
var quantity = 3;
var subtotal = itemPrice * quantity;  // 89.97

var taxRate = 0.08;  // 8% tax
var tax = subtotal * taxRate;  // 7.1976
var total = subtotal + tax;  // 97.1676
```

**Temperature conversion:**
```zia
var celsius = 25;
var fahrenheit = celsius * 9 / 5 + 32;  // 77
```

**Distance and speed:**
```zia
var distanceMiles = 150;
var hours = 2.5;
var averageSpeed = distanceMiles / hours;  // 60 mph
```

### The Remainder Operator (%)

The `%` operator gives the *remainder* after division. It's pronounced "mod" or "modulo."

```zia
10 % 3   // equals 1
```

Why? Because 10 divided by 3 is 3 with a remainder of 1. (3 times 3 is 9, and 10 minus 9 is 1.)

More examples:
```zia
10 % 5   // 0 (10 divides evenly by 5)
10 % 4   // 2 (4*2=8, remainder 2)
10 % 6   // 4 (6*1=6, remainder 4)
7 % 10   // 7 (10 doesn't go into 7 at all, so the whole number is the remainder)
```

**Why is this useful?**

The remainder operator is surprisingly handy:

**Checking if a number is even or odd:**
```zia
var n = 17;
var remainder = n % 2;  // If 0, n is even. If 1, n is odd.
// 17 % 2 = 1, so 17 is odd
```

**Wrapping around within a range:**
```zia
var hour = 25;
var normalizedHour = hour % 24;  // 1 (wraps around past 24)
```

**Extracting digits:**
```zia
var number = 1234;
var lastDigit = number % 10;  // 4
```

**Checking divisibility:**
```zia
var year = 2024;
var isDivisibleBy4 = year % 4 == 0;  // true (for leap year checking)
```

### Integer Division: A Common Source of Confusion

Here's something that surprises almost every beginner:

```zia
bind Zanna.Terminal;

var result = 10 / 3;
Say(result);  // Prints: 3
```

Wait -- 10 divided by 3 is 3.333..., isn't it? Why does Zanna say 3?

**The rule: when you divide two integers, you get an integer result.** The fractional part is simply discarded (not rounded -- truncated toward zero).

Think of it like this: if you have 10 cookies and 3 people, each person gets 3 cookies (integer division), with 1 cookie left over (that's where the remainder operator becomes useful).

More examples:
```zia
7 / 2    // 3, not 3.5
1 / 2    // 0, not 0.5 (this one really surprises people!)
99 / 100 // 0, not 0.99
-7 / 2   // -3, not -3.5 (truncates toward zero)
```

**How to get the fractional result:**

If you want the fractional part, use floats:

```zia
var result = 10.0 / 3.0;  // 3.333...
```

The `.0` makes these floating-point numbers, so you get a floating-point result.

Or, if you have integer variables and want a float result:

```zia
var a = 10;
var b = 3;
var result = a * 1.0 / b;  // Multiply by 1.0 to make it a float first
```

### Common Confusion: Order of Operations

Mathematical operations follow the standard order of operations (PEMDAS/BODMAS):
1. Parentheses/Brackets first
2. Multiplication and Division (left to right)
3. Addition and Subtraction (left to right)

```zia
var result = 2 + 3 * 4;    // 14, not 20 (multiplication first)
var result2 = (2 + 3) * 4; // 20 (parentheses force addition first)
```

When in doubt, use parentheses to make your intentions clear!

---

## Strings in Detail

Strings are text -- sequences of characters enclosed in double quotes. Despite seeming simple, strings have subtleties that trip up beginners.

### Creating Strings

```zia
var greeting = "Hello, World!";     // A typical string
var empty = "";                      // An empty string (zero characters)
var space = " ";                     // A single space character
var numbers = "12345";              // These are characters, not a number!
var single = "a";                   // Even one character is a string
var mixed = "The answer is 42.";   // Strings can contain digits
```

**Important**: `"42"` (in quotes) and `42` (no quotes) are completely different things:
- `"42"` is a string: two characters, the digit '4' and the digit '2'
- `42` is a number: the mathematical quantity forty-two

You can't do arithmetic with `"42"` (what's `"42" + 1`? Is that `"43"` or `"421"` or an error?). You can do arithmetic with `42`.

### Escape Characters: When Quotes Aren't Enough

What if you want to include a quote mark *inside* your string? This won't work:

```zia
var quote = "She said "Hello"";  // Error! Zanna thinks the string ends at said "
```

The computer sees the first `"`, starts the string, sees the second `"`, thinks the string ended, and then is confused by the remaining text.

The solution is the *escape character*: backslash (`\`). It tells Zanna "the next character is special -- don't interpret it normally."

```zia
var quote = "She said \"Hello\"";  // Works! \" means a literal quote
```

The `\"` doesn't print as `\"` -- it prints as `"`. The backslash "escapes" the quote from being interpreted as the end of the string.

**Common escape sequences:**

| Sequence | Meaning | Example Output |
|----------|---------|----------------|
| `\"` | Literal quote | `"` |
| `\\` | Literal backslash | `\` |
| `\n` | Newline (start new line) | *(moves to next line)* |
| `\t` | Tab (horizontal space) | *(wide space)* |
| `\r` | Carriage return | *(rare, Windows line endings)* |

**Examples in action:**

```zia
bind Zanna.Terminal;

// Newlines
Say("Line one\nLine two\nLine three");
```
Output:
```text
Line one
Line two
Line three
```

```zia
bind Zanna.Terminal;

// Tabs (for alignment)
Say("Name\tAge\tCity");
Say("Alice\t30\tNew York");
Say("Bob\t25\tChicago");
```
Output:
```text
Name    Age     City
Alice   30      New York
Bob     25      Chicago
```

```zia
bind Zanna.Terminal;

// Including backslashes (need to escape them)
Say("Path: C:\\Users\\Alice\\Documents");
```
Output:
```text
Path: C:\Users\Alice\Documents
```

```zia
bind Zanna.Terminal;

// Quotes in dialogue
Say("\"To be or not to be,\" he pondered.");
```
Output:
```text
"To be or not to be," he pondered.
```

### String Concatenation

When you use `+` with strings, it joins them together (concatenation):

```zia
var first = "Hello";
var second = "World";
var message = first + ", " + second + "!";  // "Hello, World!"
```

Think of it like gluing words together. `"Hello"` + `", "` + `"World"` + `"!"` becomes one long string: `"Hello, World!"`.

**Concatenating strings with numbers:**

```zia
bind Zanna.Terminal;

var name = "Alice";
var age = 30;
Say(name + " is " + age + " years old.");
```
Output:
```text
Alice is 30 years old.
```

When you concatenate a string with a number, Zanna automatically converts the number to its text representation. This is convenient but can lead to surprises:

```zia
var result = "5" + 3;  // "53" (concatenation, not addition!)
var math = 5 + 3;      // 8 (addition)
```

If either operand is a string, `+` does concatenation, not addition.

### String Length

Every string has a *length* -- the number of characters it contains.

```zia
"Hello"     // length 5
""          // length 0 (empty string)
"Hi there"  // length 8 (space counts!)
"Line\n"    // length 5 (the \n is one character - newline)
```

We'll learn how to get the length and work with individual characters in Chapter 8 when we explore collections.

### Common Confusion: Strings Are Not Numbers

A frequent beginner mistake is treating string-digits as numbers:

```zia
var userInput = "25";      // This is TEXT from user input
var nextYear = userInput + 1;  // Doesn't give 26!
```

The variable `userInput` contains the string `"25"` -- two characters that look like the number twenty-five but aren't. When you try to add `1`, Zanna converts `1` to `"1"` and concatenates, giving `"251"`.

To do math with user input, you must convert it first:

```zia
var userInput = "25";
var age = Zanna.Core.Convert.ToInt64(userInput);  // Convert to number
var nextYear = age + 1;  // Now this is 26
```

---

## Booleans: True and False

Boolean values represent truth. There are exactly two: `true` and `false`. That's it. No maybe, no sort-of, no probably. Just true and false.

### Why Booleans Matter

Before looking at code, let's think about why true/false values are useful.

Consider the questions you ask in everyday life:
- "Is it raining?" (yes/no)
- "Am I old enough to vote?" (yes/no)
- "Did the user enter the correct password?" (yes/no)
- "Is there milk in the fridge?" (yes/no)
- "Has the game ended?" (yes/no)

All of these questions have two possible answers. And crucially, the answers to these questions determine what we do next:
- If it's raining, I'll take an umbrella
- If I'm old enough, I can vote
- If the password is correct, grant access
- If there's no milk, I need to buy some
- If the game ended, show the final score

Booleans capture these yes/no questions in code. And in the next chapter, we'll see how to use booleans to make programs behave differently based on conditions.

### Creating Boolean Values

```zia
var isGameOver = false;
var hasPermission = true;
var isLoggedIn = true;
var isEmpty = false;
```

By convention, boolean variable names often start with `is`, `has`, `can`, `should`, or similar words that suggest a yes/no question. This makes code read more naturally.

### Comparison Operators: Questions That Produce Booleans

You usually don't write `true` or `false` directly. Instead, booleans are the *result* of comparisons:

```zia
var age = 25;
var isAdult = age >= 18;  // Is 25 >= 18? Yes! So isAdult is true.
```

The expression `age >= 18` is a *question* that produces a boolean *answer*.

**Comparison operators:**

| Operator | Meaning | Example | Result |
|----------|---------|---------|--------|
| `==` | equals | `5 == 5` | `true` |
| `!=` | not equals | `5 != 3` | `true` |
| `<` | less than | `3 < 5` | `true` |
| `>` | greater than | `5 > 3` | `true` |
| `<=` | less than or equal | `5 <= 5` | `true` |
| `>=` | greater than or equal | `5 >= 3` | `true` |

**Real-world examples:**

```zia
var temperature = 72;
var isHot = temperature > 90;        // false (72 is not greater than 90)
var isComfortable = temperature >= 65 && temperature <= 80;  // true

var password = "secret123";
var isCorrect = password == "secret123";  // true

var balance = 150;
var canAfford = balance >= 100;  // true (150 >= 100)
```

### Common Confusion: = vs ==

This is one of the most common bugs in programming:

```zia
// WRONG - this assigns 5 to x, doesn't compare!
if (x = 5)

// RIGHT - this checks if x equals 5
if (x == 5)
```

- Single `=` means "assignment" -- put this value into this variable
- Double `==` means "comparison" -- are these two values equal?

Some languages actually allow `x = 5` in conditions (with unintended results). Zanna tries to catch this as an error, but it's still a habit worth developing: always use `==` for comparison.

A memory trick: "One equals for setting, two equals for checking."

### Logical Operators: Combining Booleans

Often you need to combine multiple conditions. That's where logical operators come in.

**AND (&&)**: Both must be true

Real-world: "I'll go to the beach if it's sunny AND I have free time."

```zia
var isSunny = true;
var haveTime = false;
var goToBeach = isSunny && haveTime;  // false (both must be true)
```

Truth table for AND:
| A | B | A && B |
|---|---|--------|
| true | true | true |
| true | false | false |
| false | true | false |
| false | false | false |

**OR (||)**: At least one must be true

Real-world: "I'll be happy if I get a raise OR get promoted."

```zia
var gotRaise = false;
var gotPromotion = true;
var isHappy = gotRaise || gotPromotion;  // true (at least one is true)
```

Truth table for OR:
| A | B | A \|\| B |
|---|---|----------|
| true | true | true |
| true | false | true |
| false | true | true |
| false | false | false |

**NOT (!)**: Flip true to false, false to true

Real-world: "If I'm NOT tired, I'll go for a run."

```zia
var isTired = false;
var willRun = !isTired;  // true (opposite of false)
```

Truth table for NOT:
| A | !A |
|---|----|
| true | false |
| false | true |

### Combining Comparisons

Real programs often have complex conditions:

```zia
var age = 25;
var hasTicket = true;
var isMember = false;

// Can enter if: adult with ticket, OR any member
var canEnter = (age >= 18 && hasTicket) || isMember;

// Must be adult AND either have ticket or be member
var canEnterV2 = age >= 18 && (hasTicket || isMember);
```

Use parentheses to make the logic clear! Without parentheses, `&&` has higher precedence than `||`, which can lead to unexpected results.

### Boolean Logic in the Real World

Let's trace through a realistic example:

```zia
var userAge = 16;
var hasParentPermission = true;
var movieRating = "PG-13";

// Can watch if: adult, OR (teenager with permission)
var isAdult = userAge >= 18;           // false
var isTeenager = userAge >= 13;        // true
var canWatch = isAdult || (isTeenager && hasParentPermission);  // true

// Let's trace it:
// isAdult = false
// isTeenager = true
// hasParentPermission = true
// isTeenager && hasParentPermission = true && true = true
// isAdult || true = false || true = true
```

Breaking complex boolean expressions into named parts makes them much easier to understand and debug.

---

## Changing Values

Variables can change. That's why they're called "variables" -- they vary.

```zia
bind Zanna.Terminal;

var score = 0;
Say(score);  // 0

score = 10;
Say(score);  // 10

score = score + 5;
Say(score);  // 15
```

Notice that after creating a variable with `var`, we change it using just `=` (no `var`). Using `var` again would try to create a *new* variable with the same name, which is an error.

### Understanding score = score + 5

The line `score = score + 5` might look strange if you think of `=` as mathematical equality. In math, "x = x + 5" is a contradiction -- no number equals itself plus five!

But remember: `=` is not an equation. It's an *instruction*. Read it as:
1. Take the current value of `score` (which is 10)
2. Add 5 to it (getting 15)
3. Store that result back into `score`

After this line executes, `score` contains 15. The old value (10) is gone, replaced by the new value.

Think of it like updating a scoreboard. The old score was 10. Something happened, and now the score is 15. The scoreboard now shows 15 -- the 10 is history.

### Updating Variables

The pattern "variable = variable + something" appears constantly in real programs:

```zia
var score = 10;
score = score + 5;   // Add 5 to score
score = score - 3;   // Subtract 3 from score
score = score * 2;   // Double the score
score = score / 4;   // Quarter the score
score = score % 10;  // Take remainder after dividing by 10
```

This pattern is so common that Zia provides shorthand compound assignment operators: `+=`, `-=`, `*=`, `/=`, and `%=`. So `score = score + 5` can be written `score += 5`. The explicit long form shown above also works and may be clearer when you're starting out.

### Variable Lifetime

A variable exists from when it's created until the end of its *scope* (the block of code it belongs to). We'll explore scope more in Chapter 6, but for now, just know that variables created inside `start()` exist for the duration of that function.

---

## Constants: Values That Don't Change

Sometimes you have a value that should never change during the program:
- Mathematical constants like pi or e
- Configuration values like a maximum score or default settings
- Conversion factors like inches per foot

For these, use `final` instead of `var`:

```zia
final PI = 3.14159265358979;
final MAX_PLAYERS = 4;
final DAYS_IN_WEEK = 7;
final TAX_RATE = 0.08;
```

The intent is that `final` values should never be reassigned. Treating them as truly constant makes your code clearer and less error-prone:

```zia
final PI = 3.14159;
// PI = 3.0;  // Don't do this — PI is meant to be constant
```

**Why use constants?**

1. **Prevents bugs**: You can't accidentally change a value that shouldn't change.

2. **Communicates intent**: When another programmer (or future you) sees `final`, they know this value is fixed and can be relied upon.

3. **Enables optimization**: The compiler knows constants don't change, so it can make optimizations.

4. **Centralizes magic numbers**: Instead of writing `0.08` throughout your code, you write `TAX_RATE` everywhere and define `final TAX_RATE = 0.08` once. If the tax rate changes, you update one line.

**Naming convention**: Constants are traditionally named in `UPPER_CASE` with underscores between words. This makes them visually distinct from variables.

---

## Getting Input from the User

So far, our variables have been set by us in the code. But programs become interesting when users can provide input.

```zia
module Greeting;
bind Zanna.Terminal;

func start() {
    Print("What is your name? ");
    var name = TryReadLine().UnwrapOrStr("");

    Say("Hello, " + name + "!");
}
```

Running this:
```text
What is your name? Alice
Hello, Alice!
```

`TryReadLine().UnwrapOrStr("")` pauses the program, waits for the user to type something and press Enter, then returns what they typed as a string.

Note the difference between `Print()` and `Say()`:
- `Say()` adds a newline at the end (cursor moves to next line)
- `Print()` doesn't add a newline (cursor stays on same line)

We use `Print()` for the prompt so the user types on the same line as the question.

### Converting Input to Numbers

`TryReadLine().UnwrapOrStr("")` always returns a string. But what if you need a number? You must convert it:

```zia
module Age;
bind Zanna.Terminal;

func start() {
    Print("How old are you? ");
    var ageText = TryReadLine().UnwrapOrStr("");
    var age = Zanna.Core.Convert.ToInt64(ageText);        // Convert to integer

    var nextYear = age + 1;
    Say("Next year you'll be " + nextYear);
}
```

`Zanna.Core.Convert.ToInt64()` converts a string like `"25"` into the number `25`.
`Zanna.Core.Convert.ToDouble()` converts a string like `"3.14"` into the float `3.14`.

**What if the user types something that isn't a number?**

If you try to parse `"hello"` as a number, you'll get an error. In Chapter 7, we'll learn how to handle such errors gracefully. For now, we'll assume users type valid input.

### Common Confusion: Input Is Always a String

A very common beginner mistake:

```zia
bind Zanna.Terminal;

Print("Enter a number: ");
var number = TryReadLine().UnwrapOrStr("");
var doubled = number * 2;  // Error! Can't multiply a string
```

Always remember: `TryReadLine().UnwrapOrStr("")` returns a string, not a number. You must convert it with `Zanna.Core.Convert.ToInt64()` if you want to do math.

---

## The Two Languages

Zanna supports two syntax styles. The concepts -- values, variables, types, operations -- are identical in both. Only the spelling and punctuation differ.

**Zia**
```zia
bind Zanna.Terminal;

var name = "Alice";
var age = 30;
final PI = 3.14159;

age = age + 1;
Say(name + " is " + age);
```

**BASIC**
```basic
DIM name AS STRING
DIM age AS INTEGER
CONST PI = 3.14159

name = "Alice"
age = 30

age = age + 1
PRINT name; " is "; age
```

BASIC requires you to declare variables with `DIM` and specify their type explicitly. Some find this more verbose; others appreciate the explicitness. BASIC uses `;` to concatenate in `PRINT` statements rather than `+`.

**What's the same across both:**
- The concept of naming values
- Integers and floating-point numbers
- String text values
- Boolean true/false values
- The ability to change variables
- Constants that don't change
- Basic arithmetic operations
- Comparison operators

The syntax differs, but the ideas are identical. Once you understand these concepts, learning either syntax is straightforward.

---

## Common Confusions Summary

Let's address the most common beginner misunderstandings in one place:

### 1. Forgetting to initialize variables
```zia
var count;          // Error: what value does it have?
var count = 0;      // Good: starts at 0
```
Variables must be given a value when created.

### 2. Using a variable before creating it
```zia
bind Zanna.Terminal;

Say(score);  // Error: score doesn't exist yet!
var score = 100;
```
You must create a variable before you can use it.

### 3. Confusing = and ==
```zia
if (x = 5)   // WRONG: assigns 5 to x
if (x == 5)  // RIGHT: checks if x equals 5
```
One equals for setting, two equals for checking.

### 4. Expecting strings to behave like numbers
```zia
var result = "5" + 3;  // "53", not 8!
```
When either operand is a string, `+` concatenates. Convert strings to numbers first if you want arithmetic.

### 5. Integer division surprise
```zia
var half = 1 / 2;      // Result is 0, not 0.5!
var halfF = 1.0 / 2.0; // Result is 0.5
```
Integer divided by integer gives integer. Use floats for fractional results.

### 6. Forgetting to convert user input
```zia
bind Zanna.Terminal;

var age = TryReadLine().UnwrapOrStr("");  // This is a string, not a number!
var nextYear = age + 1;  // Wrong result: string concatenation, not arithmetic!
```
`TryReadLine().UnwrapOrStr("")` returns a string. Use `Zanna.Core.Convert.ToInt64()` or `Zanna.Core.Convert.ToDouble()` to convert it to a number.

### 7. Using var twice for the same variable
```zia
var score = 0;
var score = 10;  // Error: score already exists!
score = 10;      // Correct: change existing variable
```
Use `var` to create; use just `=` to change.

### 8. Thinking variables are linked
```zia
var a = 5;
var b = a;  // b is now 5
a = 10;     // a is now 10, but b is still 5!
```
`var b = a` copies the *value*, not a link to the variable. Changing `a` later doesn't affect `b`.

---

## Putting It Together

Here's a small program that uses everything we've learned:

```zia
module Calculator;
bind Zanna.Terminal;

func start() {
    Say("Simple Calculator");
    Say("================");

    Print("Enter first number: ");
    var first = Zanna.Core.Convert.ToDouble(TryReadLine().UnwrapOrStr(""));

    Print("Enter second number: ");
    var second = Zanna.Core.Convert.ToDouble(TryReadLine().UnwrapOrStr(""));

    var sum = first + second;
    var difference = first - second;
    var product = first * second;
    var quotient = first / second;

    Say("");
    Say("Results:");
    Say("" + first + " + " + second + " = " + sum);
    Say("" + first + " - " + second + " = " + difference);
    Say("" + first + " * " + second + " = " + product);
    Say("" + first + " / " + second + " = " + quotient);
}
```

Running it:
```text
Simple Calculator
================
Enter first number: 10
Enter second number: 3
Results:
10 + 3 = 13
10 - 3 = 7
10 * 3 = 30
10 / 3 = 3.33333333333333
```

Let's trace through what this program does:
1. Displays a title
2. Asks for and reads the first number (converting from string to float)
3. Asks for and reads the second number (converting from string to float)
4. Calculates all four arithmetic operations
5. Displays the results, concatenating strings and numbers for readable output

---

## Looking Ahead

The concepts in this chapter -- values, types, variables, and operations -- are the foundation of everything that follows.

In **Chapter 4: Making Decisions**, you'll learn to use booleans to make programs behave differently based on conditions. Instead of always doing the same thing, your programs will choose between different paths.

In **Chapter 5: Repeating Actions**, you'll learn loops -- doing the same thing many times. Combined with variables that change, this lets you process lists of data, count things, and accumulate results.

In **Chapter 6: Organizing Code with Functions**, you'll learn to package code into reusable pieces. Variables will take on new importance as we discuss parameters and return values.

In **Chapter 8: Collections**, you'll learn to work with lists of values -- not just a single `name`, but a list of names; not just a single `score`, but a list of scores.

Every one of these chapters builds on what you've learned here. Values, types, variables, and operations are the vocabulary of programming. Now you're ready to start constructing sentences.

---

## Summary

- **Values** are the data programs work with: individual pieces of information like numbers, text, or true/false answers.

- **Types** categorize values: integers (whole numbers), floats (decimals), strings (text), and booleans (true/false). Different types support different operations.

- **Variables** are names attached to values, created with `var`. Think of them as labeled boxes holding data.

- **Constants** are unchangeable values, created with `final`. Use them for values that should never change.

- **Integers** are whole numbers; **floats** have decimal points. Be aware that integer division discards the fractional part.

- **Strings** are text in double quotes. Use escape characters (`\n`, `\"`, `\\`) for special characters. Join strings with `+` (concatenation).

- **Booleans** are `true` or `false`, often produced by comparisons (`==`, `<`, `>=`). Combine them with logical operators (`&&`, `||`, `!`).

- Variables can change (that's why they're called variables). Use `=` to assign new values.

- Use `TryReadLine().UnwrapOrStr("")` to get text input from users. It returns a string. Use `Zanna.Core.Convert.ToInt64()` or `Zanna.Core.Convert.ToDouble()` to convert text to numbers.

- Choose descriptive variable names to make your code readable.

---

## Exercises

**Exercise 3.1**: Write a program that asks for your name and age, then prints "Hello, [name]! You are [age] years old."

**Exercise 3.2**: Write a program that asks for a temperature in Celsius and converts it to Fahrenheit. The formula is: F = C * 9/5 + 32. (Hint: Be careful about integer division!)

**Exercise 3.3**: Write a program that asks for the length and width of a rectangle, then prints its area (length * width) and perimeter (2 * length + 2 * width).

**Exercise 3.4**: Write a program that asks for three test scores and prints their average. (Hint: Average = sum / count. Watch out for integer division!)

**Exercise 3.5** (Challenge): Write a program that asks for a number of seconds and converts it to hours, minutes, and seconds. For example, 3665 seconds is 1 hour, 1 minute, and 5 seconds.

*Hint: Use integer division and the modulo operator.*
- How many total minutes? `seconds / 60`
- How many remaining seconds after extracting minutes? `seconds % 60`
- How many hours? `totalMinutes / 60`
- How many remaining minutes? `totalMinutes % 60`

**Exercise 3.6** (Challenge): Write a program that asks for a year and determines if it's a leap year. A year is a leap year if:
- It's divisible by 4, AND
- Either it's NOT divisible by 100, OR it IS divisible by 400

For example: 2020 is a leap year. 1900 is not (divisible by 100 but not 400). 2000 is (divisible by 400).

*Hint: Use the modulo operator to check divisibility: `year % 4 == 0` means year is divisible by 4.*

---

*Now we can store and calculate with data. But our programs still run the same way every time, regardless of input. Next, we'll learn how to make programs that choose different paths based on conditions -- bringing our programs to life with decision-making.*

*[Continue to Chapter 4: Making Decisions](04-decisions.md)*
