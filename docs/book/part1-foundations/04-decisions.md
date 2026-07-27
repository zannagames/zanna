---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 4: Making Decisions

Until now, our programs have been like trains on a track — they go forward, line by line, never deviating. But real programs need to react to situations. Should we show an error message? Does the player have enough coins? Is the password correct?

This chapter teaches your programs to think: to examine conditions and choose what to do.

---

## Why Programs Need to Make Decisions

Think about how many decisions happen in a single minute of using your phone:

When you unlock your phone, it checks: Is the fingerprint correct? If yes, show the home screen. If no, ask for a PIN.

When you open a messaging app, it decides: Are there new messages? If yes, show a notification badge. How many unread? Display that number.

When you try to send a photo, it asks: Is the file too large? Is there an internet connection? Does the recipient exist?

Every single interaction involves dozens of invisible decisions. Programs aren't just calculators that crunch numbers — they're decision-makers that respond intelligently to ever-changing situations.

Let's look at some real-world examples to understand why decisions are so fundamental to programming.

### Video Games Are Decision Machines

Consider a simple platformer game. Every single frame (60 times per second!), the game asks:

- Is the player pressing the jump button?
- Is the player standing on solid ground? (You can't jump in mid-air!)
- Is the player touching an enemy?
- Does the player have any lives left?
- Has the player reached the goal?

Each of these questions has a yes/no answer, and the game does different things based on those answers. If the player is pressing jump AND standing on ground, make the character jump. If the player touches an enemy AND has no invincibility power-up, lose a life.

Without decisions, you couldn't have a game at all. The character would just stand there, unable to respond to anything.

### ATMs and Security

Think about withdrawing money from an ATM:

1. First, it asks: Is this card valid? If not, reject it immediately.
2. Then: Is the PIN correct? The ATM gives you three tries.
3. Next: How much do you want? Is that amount available in your account?
4. Also: Does the machine have enough cash to dispense?
5. Finally: Should we print a receipt?

Each step depends on the answer to the previous questions. If your PIN is wrong, you never get to the withdrawal screen. The ATM makes decisions on your behalf to keep your money safe.

### Smart Home Devices

A smart thermostat constantly makes decisions:

- Is anyone home? If not, save energy.
- What's the current temperature? What's the target?
- Is it heating season or cooling season?
- Is electricity cheaper right now? Maybe pre-cool the house during off-peak hours.

These decisions happen automatically, every few minutes, all day long.

### Apps You Use Daily

- **Email filters**: Is this message from my boss? Then mark it important. Does it contain "unsubscribe"? Probably promotional.
- **Autocorrect**: Did the user type "teh"? They probably meant "the."
- **Maps**: Is there traffic on the usual route? Suggest an alternative.
- **Music apps**: Has the user skipped this song three times? Stop recommending it.

Programming is, in many ways, the art of making decisions. Now let's learn how to write programs that can think.

---

## The Fork in the Road

Imagine giving someone directions: "Go straight until the fountain. If the gate is open, go through it; otherwise, take the path around."

That's exactly what conditional statements do in programming. They check a condition (is the gate open?) and do different things based on the answer.

Here's the mental picture you should hold in your mind: You're walking down a path, and suddenly it splits in two. One path goes left, one goes right. There's a signpost with a question on it. Based on your answer to that question, you'll take one path or the other.

You can't take both paths. You can't take neither. You must choose one.

```zia
bind Zanna.Terminal;

var gateOpen = true;

if gateOpen {
    Say("Going through the gate");
} else {
    Say("Taking the path around");
}
```

The `if` keyword introduces a condition. If it's true, the code inside the first `{ }` block runs. If it's false, the code inside the `else` block runs.

This is the fundamental pattern of decision-making in programs. Master this, and you understand something that appears in every programming language ever created.

---

## The if Statement: Your First Decision

### The Simplest Form

The simplest form has no `else`:

```zia
bind Zanna.Terminal;

var temperature = 35;

if temperature > 30 {
    Say("It's hot today!");
}

Say("Have a nice day.");
```

Let's trace through this program step by step, exactly as the computer does:

1. **Line 1**: Create a variable called `temperature` and store the number `35` in it.
2. **Line 3**: The `if` statement. The computer evaluates the expression `temperature > 30`. This means: "Is 35 greater than 30?" Yes, it is. The answer is `true`.
3. **Because the condition is true**: The computer enters the `{ }` block and executes line 4.
4. **Line 4**: Print "It's hot today!" to the screen.
5. **Line 5**: The closing brace `}` ends the if-block.
6. **Line 7**: This line is outside the `if` statement. It runs no matter what. Print "Have a nice day."

Output:
```text
It's hot today!
Have a nice day.
```

Now, what if we change the temperature to 25?

1. **Line 1**: Store `25` in `temperature`.
2. **Line 3**: Evaluate `temperature > 30`. Is 25 greater than 30? No. The answer is `false`.
3. **Because the condition is false**: Skip everything inside the `{ }` block. Jump directly to after the closing brace.
4. **Line 7**: Print "Have a nice day."

Output:
```text
Have a nice day.
```

See the difference? The "hot today" message only appears when the condition is true.

### The Structure Explained

```text
if condition {
    // code that runs when condition is true
}
```

Let's break down each part:

- **`if`**: This keyword tells the computer "I'm about to ask you a yes/no question."
- **`condition`**: The question itself. Must evaluate to either `true` or `false`.
- **`{`**: "If the answer is yes, start doing the following..."
- **code inside**: The instructions to follow when the condition is true.
- **`}`**: "...and stop here."

The condition must be something that evaluates to `true` or `false` — what programmers call a boolean expression. Usually this involves comparisons:

```zia
if age >= 18 { ... }           // is age at least 18?
if name == "Alice" { ... }     // is name exactly "Alice"?
if score != 0 { ... }          // is score something other than 0?
if password == secret { ... }  // do passwords match?
```

### Thinking Like the Computer

Here's a crucial skill for programmers: you need to be able to "run" code in your head, step by step, just like a computer would. Let's practice with another example:

```zia
bind Zanna.Terminal;

var balance = 50;
var price = 75;

if balance >= price {
    Say("Purchase complete!");
    balance = balance - price;
}

Say("Your balance is: " + balance);
```

Walk through it:

1. `balance` is set to `50`.
2. `price` is set to `75`.
3. Check: Is `balance >= price`? Is 50 >= 75? No. This is `false`.
4. Skip the entire block inside the braces.
5. Print "Your balance is: 50".

The purchase doesn't happen because you don't have enough money. The program protects you from overdrawing your account!

Now imagine `balance` was `100`:

1. `balance` is set to `100`.
2. `price` is set to `75`.
3. Check: Is `balance >= price`? Is 100 >= 75? Yes! This is `true`.
4. Enter the block. Print "Purchase complete!"
5. Update balance: `100 - 75 = 25`. Now `balance` is `25`.
6. Exit the block.
7. Print "Your balance is: 25".

---

## if-else: Two Paths, Always One Choice

When you need to do one thing OR another (but never both, and never neither), use `else`:

```zia
bind Zanna.Terminal;

var hour = 14;

if hour < 12 {
    Say("Good morning!");
} else {
    Say("Good afternoon!");
}
```

### The Two-Path Guarantee

With `if-else`, exactly one of these blocks will run. Always. No exceptions.

- If `hour < 12` is true, print "Good morning!" and skip the else.
- If `hour < 12` is false, skip the if and print "Good afternoon!"

Think of it as a fork in the road where you MUST take one path. You can't stand still, and you can't take both paths. The condition determines which way you go.

### Another Mental Model: The Light Switch

Think of a light switch. It's either ON or OFF. Never both, never neither.

```zia
if switchIsOn {
    // The light is on - room is bright
} else {
    // The light is off - room is dark
}
```

The room is always in exactly one of these states.

### The Bouncer Analogy

Imagine a bouncer at a club checking IDs:

```zia
bind Zanna.Terminal;

var age = 17;

if age >= 21 {
    Say("Welcome! Enjoy your evening.");
} else {
    Say("Sorry, you must be 21 or older.");
}
```

The bouncer doesn't have three or four responses. It's binary: you're either old enough or you're not. Every single person gets exactly one of these two responses.

---

## if-else if-else: Multiple Paths

Sometimes there are more than two possibilities:

```zia
bind Zanna.Terminal;

var hour = 20;

if hour < 12 {
    Say("Good morning!");
} else if hour < 17 {
    Say("Good afternoon!");
} else if hour < 21 {
    Say("Good evening!");
} else {
    Say("Good night!");
}
```

### How the Computer Processes This

The computer checks each condition in order, like going down a checklist:

1. Is `hour < 12`? Is 20 < 12? **No.** Move to the next condition.
2. Is `hour < 17`? Is 20 < 17? **No.** Move to the next condition.
3. Is `hour < 21`? Is 20 < 21? **Yes!** Run this block: print "Good evening!"
4. **Stop immediately.** Don't check any more conditions.

The final `else` catches anything that didn't match earlier conditions. It's the "everything else" case. It's optional — you can have a chain of `if-else if` without a final `else`.

### Critical Concept: Order Matters

**Once a condition matches, the rest are skipped.** This is crucial to understand.

Consider this buggy code:

```zia
bind Zanna.Terminal;

var score = 95;

if score >= 60 {
    Say("You passed!");
} else if score >= 90 {
    Say("Excellent work!");
}
```

What happens? Score is 95. Is `95 >= 60`? Yes! Print "You passed!" and stop.

Wait — 95 is also >= 90. Shouldn't it print "Excellent work!"? Nope. We already found a matching condition and stopped looking.

The fix: Put more specific conditions first.

```zia
bind Zanna.Terminal;

var score = 95;

if score >= 90 {
    Say("Excellent work!");
} else if score >= 60 {
    Say("You passed!");
}
```

Now 95 >= 90 matches first, and we get "Excellent work!"

### The Restaurant Menu Analogy

Think of `if-else if-else` like a waiter taking your order:

```zia
if wantsAppetizer {
    // Bring appetizer
} else if wantsMainCourse {
    // Bring main course
} else if wantsDessert {
    // Bring dessert
} else {
    // Bring the check, they're done
}
```

But wait — at a real restaurant, you might want appetizer AND main course AND dessert! This analogy shows why multiple separate `if` statements differ from an `if-else if` chain:

```zia
// Multiple separate ifs - customer can order multiple things
if wantsAppetizer {
    // Bring appetizer
}
if wantsMainCourse {
    // Bring main course
}
if wantsDessert {
    // Bring dessert
}
```

With separate `if` statements, each condition is checked independently. You could get all three, or any combination.

---

## Understanding Boolean Expressions

Many beginners struggle with boolean expressions, so let's dig deep here. A boolean expression is any expression that produces the value `true` or `false`. That's it — just two possible values.

### The Comparison Operators

These operators compare two values and return a boolean:

```zia
x == y    // Equal: Is x the same as y?
x != y    // Not equal: Is x different from y?
x < y     // Less than: Is x smaller than y?
x > y     // Greater than: Is x larger than y?
x <= y    // Less than or equal: Is x smaller than or the same as y?
x >= y    // Greater than or equal: Is x larger than or the same as y?
```

### Examples with Numbers

Let's evaluate some boolean expressions. I'll show you the expression, then work through it:

```zia
var a = 5;
var b = 10;
var c = 5;

a == b;    // Is 5 equal to 10? No. Result: false
a == c;    // Is 5 equal to 5? Yes. Result: true
a != b;    // Is 5 not equal to 10? Yes, they're different. Result: true
a != c;    // Is 5 not equal to 5? No, they're the same. Result: false

a < b;     // Is 5 less than 10? Yes. Result: true
a > b;     // Is 5 greater than 10? No. Result: false
a <= c;    // Is 5 less than or equal to 5? Yes (they're equal). Result: true
a >= c;    // Is 5 greater than or equal to 5? Yes (they're equal). Result: true

b > a;     // Is 10 greater than 5? Yes. Result: true
b <= a;    // Is 10 less than or equal to 5? No. Result: false
```

### Examples with Strings

String comparisons work too:

```zia
var name = "Alice";

name == "Alice";    // true - exact match
name == "alice";    // false - case matters!
name == "ALICE";    // false - case matters!
name != "Bob";      // true - they're different

name == "Alice ";   // false - trailing space makes it different!
```

**Critical warning**: String comparisons are case-sensitive and whitespace-sensitive. `"Yes"` is not equal to `"yes"`. `"Hello"` is not equal to `"Hello "` (with a space at the end).

### The Two-Equals Trap

This is the most common beginner mistake:

```zia
// WRONG - This is assignment, not comparison!
if score = 100 {
    ...
}

// RIGHT - Double equals for comparison
if score == 100 {
    ...
}
```

Remember:
- `=` means "put this value into that variable" (assignment)
- `==` means "are these two values the same?" (comparison)

In English, we use "equals" for both, which causes confusion. In code, they're completely different operations.

### Boolean Variables

You can store boolean values in variables:

```zia
bind Zanna.Terminal;

var isRaining = true;
var hasUmbrella = false;

if isRaining {
    Say("It's raining!");
}

if hasUmbrella {
    Say("Good thing you have an umbrella!");
}
```

Notice that when a variable IS a boolean, you don't need a comparison:

```zia
// Both of these work the same:
if isRaining == true { ... }
if isRaining { ... }           // This is cleaner

// For checking false:
if isRaining == false { ... }
if !isRaining { ... }          // This is cleaner (we'll learn ! soon)
```

### Storing Comparison Results

You can store the result of a comparison:

```zia
bind Zanna.Terminal;

var age = 25;
var canVote = age >= 18;    // canVote is now true

Say("Can vote: " + canVote);  // Prints "Can vote: true"

if canVote {
    Say("Remember to register!");
}
```

This is useful when you'll use the same condition multiple times, or when the condition is complex and giving it a name makes the code clearer.

---

## Combining Conditions: AND, OR, NOT

Often you need to check multiple things at once. The logical operators let you combine boolean expressions.

### AND: Both Must Be True (`&&`)

The AND operator (`&&`) returns `true` only when BOTH sides are true.

```zia
bind Zanna.Terminal;

var age = 25;
var hasTicket = true;

if age >= 18 && hasTicket {
    Say("Welcome to the show!");
}
```

To enter, you need BOTH conditions: be 18 or older AND have a ticket. If you're 18 but have no ticket: denied. If you have a ticket but you're 16: denied. You need both.

Think of it like a security checkpoint with two guards. Both guards must let you through.

#### The AND Truth Table (In Plain English)

Let me show you every possible combination:

| Left side | Right side | Result |
|-----------|------------|--------|
| true      | true       | **true** |
| true      | false      | false |
| false     | true       | false |
| false     | false      | false |

In plain language: AND is only true when EVERYTHING is true. One false makes the whole thing false.

Some real examples:

```zia
var age = 25;
var hasLicense = true;
var hasCar = false;

age >= 18 && hasLicense;          // true && true = true
age >= 18 && hasCar;              // true && false = false
age < 18 && hasLicense;           // false && true = false
age < 18 && hasCar;               // false && false = false
```

#### Multiple ANDs

You can chain multiple conditions:

```zia
bind Zanna.Terminal;

var hasTicket = true;
var age = 25;
var isBanned = false;

if hasTicket && age >= 18 && !isBanned {
    Say("You may enter");
}
```

ALL three conditions must be true. Think of it as: "Do you have a ticket? Are you 18+? Are you not banned?" All three must be yes.

### OR: At Least One Must Be True (`||`)

The OR operator (`||`) returns `true` when AT LEAST ONE side is true.

```zia
bind Zanna.Terminal;

var day = "Saturday";

if day == "Saturday" || day == "Sunday" {
    Say("It's the weekend!");
}
```

It's the weekend if it's Saturday OR if it's Sunday. Either one works.

#### The OR Truth Table (In Plain English)

| Left side | Right side | Result |
|-----------|------------|--------|
| true      | true       | **true** |
| true      | false      | **true** |
| false     | true       | **true** |
| false     | false      | false |

In plain language: OR is true when ANYTHING is true. Both must be false for OR to be false.

Think of OR as being generous or permissive. "Do you have ANY valid credential?"

```zia
var hasCash = false;
var hasCard = true;
var hasCoupon = false;

hasCash || hasCard;              // false || true = true (card works!)
hasCash || hasCoupon;            // false || false = false (nothing works)
hasCard || hasCoupon;            // true || false = true (card works!)
hasCash || hasCard || hasCoupon; // false || true || false = true
```

#### Real Example: Multiple Payment Methods

```zia
bind Zanna.Terminal;

var hasCash = false;
var hasCard = true;
var hasDigitalWallet = false;

if hasCash || hasCard || hasDigitalWallet {
    Say("Payment accepted!");
} else {
    Say("Sorry, no valid payment method.");
}
```

The customer can pay as long as they have at least one valid method.

### NOT: Flip the Boolean (`!`)

The NOT operator (`!`) reverses a boolean. True becomes false, false becomes true.

```zia
bind Zanna.Terminal;

var gameOver = false;

if !gameOver {
    Say("Keep playing!");
}
```

If `gameOver` is `false`, then `!gameOver` is `true`, so we enter the block.

#### The NOT Truth Table

| Original | After `!` |
|----------|-----------|
| true     | false |
| false    | true |

Simple as that. NOT just flips the value.

#### Common Uses of NOT

```zia
bind Zanna.Terminal;

// Check if something is NOT the case
if !isLoggedIn {
    Say("Please log in first");
}

// Check if a list is NOT empty
if !isEmpty {
    Say("Processing items...");
}

// Check if user did NOT agree
if !agreedToTerms {
    Say("You must agree to continue");
}
```

### Combining AND, OR, and NOT

You can build complex conditions:

```zia
bind Zanna.Terminal;

var age = 42;
var hasSpecialPass = false;

if (age >= 18 && age <= 65) || hasSpecialPass {
    Say("You qualify for the program.");
}
```

Let's break this down:
- `(age >= 18 && age <= 65)`: Age between 18 and 65 (inclusive)
- `|| hasSpecialPass`: OR has a special pass

So: You qualify if you're in the age range, OR if you have a special pass (regardless of age).

#### Parentheses Are Your Friend

Use parentheses to make your intentions clear:

```zia
// What does this mean?
if a || b && c { ... }

// Is it (a || b) && c?
// Or is it a || (b && c)?
```

In most languages, `&&` has higher precedence than `||`, so `a || b && c` means `a || (b && c)`. But don't make readers think about precedence rules — just use parentheses:

```zia
if a || (b && c) { ... }    // Clear: a, OR both b and c
if (a || b) && c { ... }    // Clear: either a or b, AND c
```

#### Complex Example: Movie Theater

```zia
bind Zanna.Terminal;

var age = 15;
var hasParent = true;
var movieRating = "R";

// Can watch if: movie is G or PG, OR you're 17+, OR you're 13+ with a parent
var canWatch = (movieRating == "G" || movieRating == "PG") ||
               age >= 17 ||
               (age >= 13 && hasParent);

if canWatch {
    Say("Enjoy the movie!");
} else {
    Say("Sorry, you cannot watch this movie.");
}
```

### Short-Circuit Evaluation

Here's something important: AND and OR are "short-circuit" operators. This means they stop evaluating as soon as they know the result.

For AND (`&&`):
- If the left side is `false`, the whole thing must be `false`.
- So the right side is never even checked!

```zia
if false && someExpensiveFunction() {
    // someExpensiveFunction() never runs!
}
```

For OR (`||`):
- If the left side is `true`, the whole thing must be `true`.
- So the right side is never even checked!

```zia
if true || someExpensiveFunction() {
    // someExpensiveFunction() never runs!
}
```

This is useful for safety checks:

```zia
bind Zanna.Terminal;

// Safe: if divisor is zero, the division never runs
var divisor = 0;
if divisor != 0 && 100 / divisor > 10 {
    Say("Large quotient");
}
```

If `divisor` is zero, the left side is false, so we never try to divide by zero.

---

## Nesting: Decisions Within Decisions

You can put `if` statements inside other `if` statements:

```zia
bind Zanna.Terminal;

var hasAccount = true;
var password = "secret123";
var inputPassword = "secret123";

if hasAccount {
    if password == inputPassword {
        Say("Login successful!");
    } else {
        Say("Wrong password.");
    }
} else {
    Say("Please create an account first.");
}
```

This creates a tree of decisions:
1. Do you have an account?
   - No: "Please create an account first."
   - Yes: Is the password correct?
     - No: "Wrong password."
     - Yes: "Login successful!"

### The Tree Mental Model

Think of nested conditions as a decision tree:

```text
                    hasAccount?
                    /         \
                  No           Yes
                  |             |
        "Create account"   password correct?
                              /        \
                            No          Yes
                            |            |
                   "Wrong password"  "Login successful!"
```

Each decision branches into more decisions. The program follows one path through the tree based on the conditions.

### When Nesting Gets Out of Hand

Nesting works, but deep nesting gets hard to read:

```zia
bind Zanna.Terminal;

var hasAccount = true;
var isVerified = true;
var isBanned = false;
var password = "secret123";
var inputPassword = "secret123";
var sessionExpired = false;

if hasAccount {
    if isVerified {
        if !isBanned {
            if password == inputPassword {
                if !sessionExpired {
                    Say("Login successful!");
                } else {
                    Say("Session expired.");
                }
            } else {
                Say("Wrong password.");
            }
        } else {
            Say("Account banned.");
        }
    } else {
        Say("Please verify your email.");
    }
} else {
    Say("Please create an account.");
}
```

This is sometimes called "pyramid of doom" or "arrow code" (because the indentation forms an arrow pointing right).

### The Guard Clause Pattern

Often you can flatten nested code using "guard clauses" — checking for problems first and handling the happy path at the end:

```zia
bind Zanna.Terminal;

var hasAccount = true;
var isVerified = true;
var isBanned = false;
var password = "secret123";
var inputPassword = "secret123";
var sessionExpired = false;

if !hasAccount {
    Say("Please create an account.");
    return;
}

if !isVerified {
    Say("Please verify your email.");
    return;
}

if isBanned {
    Say("Account banned.");
    return;
}

if password != inputPassword {
    Say("Wrong password.");
    return;
}

if sessionExpired {
    Say("Session expired.");
    return;
}

Say("Login successful!");
```

This reads like a checklist: "First, check if no account — if so, stop. Then check if not verified — if so, stop. Then..."

The happy path (successful login) is at the end, after all the checks have passed.

---

## Practical Examples

Let's build some real programs that use decisions.

### Example 1: Grade Calculator

```zia
module Grader;
bind Zanna.Terminal;

func start() {
    Print("Enter the score (0-100): ");
    var score = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));

    // First, validate the input
    if score < 0 || score > 100 {
        Say("Invalid score! Please enter a number between 0 and 100.");
        return;
    }

    // Determine the letter grade
    var grade = "";
    var message = "";

    if score >= 90 {
        grade = "A";
        message = "Excellent work! You've mastered this material.";
    } else if score >= 80 {
        grade = "B";
        message = "Good job! You have a solid understanding.";
    } else if score >= 70 {
        grade = "C";
        message = "You passed. Consider reviewing the material.";
    } else if score >= 60 {
        grade = "D";
        message = "You barely passed. Please seek extra help.";
    } else {
        grade = "F";
        message = "You did not pass. Please see the teacher.";
    }

    Say("Score: " + score);
    Say("Grade: " + grade);
    Say(message);

    // Add plus/minus for more precision
    var lastDigit = score % 10;
    if grade != "F" && grade != "A" {
        if lastDigit >= 7 {
            Say("(High " + grade + ", close to " + grade + "+)");
        } else if lastDigit <= 2 {
            Say("(Low " + grade + ", close to " + grade + "-)");
        }
    }
}
```

Notice how the conditions are ordered from highest to lowest. A score of 85 isn't >= 90, but it is >= 80, so we get "B" and stop checking.

### Example 2: Age-Based Access Control

```zia
module AgeChecker;
bind Zanna.Terminal;

func start() {
    Print("Enter your age: ");
    var age = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));

    Say("");  // Blank line for readability

    // Check various age-gated activities
    Say("Based on your age (" + age + "), here's what you can do:");
    Say("");

    // Voting
    if age >= 18 {
        Say("[X] Vote in elections");
    } else {
        var yearsUntilVoting = 18 - age;
        Say("[ ] Vote in elections (in " + yearsUntilVoting + " years)");
    }

    // Driving (varies by location, using 16 as example)
    if age >= 16 {
        Say("[X] Drive a car");
    } else if age >= 15 {
        Say("[ ] Drive a car (eligible for learner's permit)");
    } else {
        Say("[ ] Drive a car (not yet eligible)");
    }

    // Rent a car (usually 25)
    if age >= 25 {
        Say("[X] Rent a car (no young driver fee)");
    } else if age >= 21 {
        Say("[X] Rent a car (young driver fee applies)");
    } else {
        Say("[ ] Rent a car (must be 21+)");
    }

    // Senior discount
    if age >= 65 {
        Say("[X] Senior citizen discount eligible");
    }
}
```

### Example 3: Simple Game Logic

```zia
module BattleGame;
bind Zanna.Terminal;

func start() {
    // Player stats
    var playerHealth = 100;
    var playerMana = 50;
    var hasShield = true;
    var hasSword = true;

    // Enemy stats
    var enemyHealth = 80;
    var enemyAttack = 25;

    Say("=== BATTLE BEGINS ===");
    Say("Your health: " + playerHealth);
    Say("Enemy health: " + enemyHealth);
    Say("");

    // Player's turn - choose action
    Say("Choose your action:");
    if hasSword {
        Say("1. Sword Attack (30 damage)");
    }
    if playerMana >= 20 {
        Say("2. Magic Blast (40 damage, costs 20 mana)");
    }
    Say("3. Defend (reduce incoming damage by half)");

    Print("Your choice: ");
    var choice = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));

    var damage = 0;
    var defended = false;

    if choice == 1 && hasSword {
        damage = 30;
        Say("You swing your sword!");
    } else if choice == 2 && playerMana >= 20 {
        damage = 40;
        playerMana = playerMana - 20;
        Say("You cast a magic blast!");
    } else if choice == 3 {
        defended = true;
        Say("You raise your shield and brace for impact!");
    } else {
        Say("Invalid choice! You hesitate and lose your turn.");
    }

    // Apply damage to enemy
    enemyHealth = enemyHealth - damage;
    if damage > 0 {
        Say("Enemy takes " + damage + " damage!");
    }

    // Check if enemy defeated
    if enemyHealth <= 0 {
        Say("*** VICTORY! The enemy is defeated! ***");
        return;
    }

    // Enemy's turn
    Say("");
    Say("Enemy attacks!");

    var incomingDamage = enemyAttack;
    if defended && hasShield {
        incomingDamage = incomingDamage / 2;
        Say("Your shield blocks half the damage!");
    }

    playerHealth = playerHealth - incomingDamage;
    Say("You take " + incomingDamage + " damage!");

    // Check if player defeated
    if playerHealth <= 0 {
        Say("*** DEFEAT! You have fallen... ***");
    } else {
        Say("");
        Say("Your health: " + playerHealth);
        Say("Enemy health: " + enemyHealth);
        Say("The battle continues...");
    }
}
```

### Example 4: Input Validation

```zia
module FormValidator;
bind Zanna.Terminal;

func start() {
    Say("=== Account Registration ===");
    Say("");

    // Get username
    Print("Username (4-20 characters): ");
    var username = TryReadLine().UnwrapOrStr("");

    // Get email
    Print("Email address: ");
    var email = TryReadLine().UnwrapOrStr("");

    // Get age
    Print("Age: ");
    var age = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));

    // Validation
    var isValid = true;
    Say("");
    Say("Validation results:");

    // Check username length
    var usernameLength = Zanna.String.get_Length(username);
    if usernameLength < 4 {
        Say("[FAIL] Username too short (minimum 4 characters)");
        isValid = false;
    } else if usernameLength > 20 {
        Say("[FAIL] Username too long (maximum 20 characters)");
        isValid = false;
    } else {
        Say("[ OK ] Username length is valid");
    }

    // Check email contains @
    if Zanna.String.Contains(email, "@") && Zanna.String.Contains(email, ".") {
        Say("[ OK ] Email format appears valid");
    } else {
        Say("[FAIL] Email must contain @ and .");
        isValid = false;
    }

    // Check age
    if age < 13 {
        Say("[FAIL] Must be at least 13 years old");
        isValid = false;
    } else if age > 120 {
        Say("[FAIL] Please enter a realistic age");
        isValid = false;
    } else {
        Say("[ OK ] Age is valid");
    }

    // Final result
    Say("");
    if isValid {
        Say("Registration successful! Welcome, " + username + "!");
    } else {
        Say("Registration failed. Please fix the errors above.");
    }
}
```

---

## The match Statement

When you're comparing one value against many specific possibilities, `match` can be cleaner than a long `if-else if` chain:

```zia
bind Zanna.Terminal;

var day = 3;

match day {
    1 => Say("Monday"),
    2 => Say("Tuesday"),
    3 => Say("Wednesday"),
    4 => Say("Thursday"),
    5 => Say("Friday"),
    6 => Say("Saturday"),
    7 => Say("Sunday"),
    _ => Say("Invalid day")
}
```

The `_` is a wildcard that matches anything not already matched. It's like the `else` in an `if` chain.

### How match Works

The `match` statement takes a value and compares it against each pattern in order:

1. Does `day` equal `1`? No. Move on.
2. Does `day` equal `2`? No. Move on.
3. Does `day` equal `3`? Yes! Run the code after `=>`.
4. Stop. Don't check any more patterns.

### When to Use match vs if-else if

Use `match` when:
- You're comparing ONE value against MANY specific values
- You're checking for exact equality
- You have 3+ cases to handle

Use `if-else if` when:
- You need range comparisons (`< > <= >=`)
- You're checking multiple different variables
- Your conditions involve complex boolean expressions

```zia
bind Zanna.Terminal;

// GOOD use of match: checking one value against specific options
match menuChoice {
    1 => startGame(),
    2 => showOptions(),
    3 => viewHighScores(),
    4 => exitGame(),
    _ => Say("Invalid choice")
}

// BAD use of match: ranges don't work this way
// match score {
//     >= 90 => ...  // This won't work!
// }

// Use if-else if for ranges
if score >= 90 {
    grade = "A";
} else if score >= 80 {
    grade = "B";
} // etc.
```

### Match with Multiple Values

You can match multiple values with `|`:

```zia
bind Zanna.Terminal;

var day = 6;
match day {
    1 | 2 | 3 | 4 | 5 => Say("Weekday"),
    6 | 7 => Say("Weekend"),
    _ => Say("Invalid day")
}
```

This says: "If day is 1 OR 2 OR 3 OR 4 OR 5, it's a weekday."

### Match for Menu Systems

Match shines in menu-driven programs:

```zia
module MenuDemo;
bind Zanna.Terminal;

func newGame() {
    Say("Starting new game...");
    Say("Welcome, hero!");
}

func loadGame() {
    Say("Loading saved games...");
    Say("No saved games found.");
}

func showOptions() {
    Say("=== OPTIONS ===");
    Say("Sound: ON");
    Say("Difficulty: NORMAL");
}

func showCredits() {
    Say("=== CREDITS ===");
    Say("Created with Zia");
    Say("Thanks for playing!");
}

func start() {
    Say("=== MAIN MENU ===");
    Say("1. New Game");
    Say("2. Load Game");
    Say("3. Options");
    Say("4. Credits");
    Say("5. Quit");
    Print("Choose an option: ");

    var choice = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));

    match choice {
        1 => newGame(),
        2 => loadGame(),
        3 => showOptions(),
        4 => showCredits(),
        5 => Say("Goodbye!"),
        _ => Say("Invalid option. Please enter 1-5.")
    }
}
```

Notice that when you need multiple statements for a case, you use `{ }` braces.

### Match Returning Values

Match can be used as an expression that returns a value:

```zia
bind Zanna.Terminal;

var day = 3;

var dayName = match day {
    1 => "Monday",
    2 => "Tuesday",
    3 => "Wednesday",
    4 => "Thursday",
    5 => "Friday",
    6 => "Saturday",
    7 => "Sunday",
    _ => "Unknown"
};

Say("Today is " + dayName);
```

This is more concise than setting a variable in each branch.

---

## The Two Languages

**Zia**
```zia
bind Zanna.Terminal;

var score = 85;

if score >= 60 {
    Say("You passed!");
} else {
    Say("Try again.");
}

match score / 10 {
    10 | 9 => Say("A"),
    8 => Say("B"),
    7 => Say("C"),
    6 => Say("D"),
    _ => Say("F")
}
```

**BASIC**
```basic
DIM score AS INTEGER
score = 85

IF score >= 60 THEN
    PRINT "You passed!"
ELSE
    PRINT "Try again."
END IF

SELECT CASE score \ 10
    CASE 10, 9
        PRINT "A"
    CASE 8
        PRINT "B"
    CASE 7
        PRINT "C"
    CASE 6
        PRINT "D"
    CASE ELSE
        PRINT "F"
END SELECT
```

BASIC uses `THEN` and `END IF`. The `SELECT CASE` is its version of `match`.

Both express the same logic. The keywords differ, but the structure — condition, then branch, else branch — is universal.

---

## Truthiness and Falsiness

In Zia, conditions must be actual booleans. This is different from some languages where `0` counts as false and other numbers count as true.

```zia
var count = 5;

if count {           // Error in Zia: count is a number, not a boolean
    ...
}

if count != 0 {      // Correct: explicitly check if count is not zero
    ...
}

if count > 0 {       // Also correct: check if count is positive
    ...
}
```

This strictness helps prevent bugs. It forces you to be explicit about what you mean.

Consider this ambiguity in languages with truthiness:

```zia
var name = "";        // Empty string

if name { ... }       // Does this run? Is "" truthy or falsy?
```

Different languages disagree on whether empty strings are truthy or falsy. Zia avoids this confusion by requiring:

```zia
if name != "" { ... }       // Explicit: "if name is not empty"
if Zanna.String.get_Length(name) > 0 { ... }  // Also explicit
```

---

## Debugging Conditions: When Things Go Wrong

Conditions are a common source of bugs. Here's how to debug them.

### Symptom: Code Never Runs

Your code inside an `if` block never executes, even when you think it should.

**Diagnosis steps:**

1. **Print the actual values** before the condition:
```zia
bind Zanna.Terminal;

var score = 85;
Say("DEBUG: score = " + score);
Say("DEBUG: score >= 90 is " + (score >= 90));

if score >= 90 {
    Say("A grade!");
}
```

2. **Check for typos** in variable names. Did you accidentally check a different variable?

3. **Check your comparison operator**. Did you use `>` when you meant `>=`?

4. **Check for type mismatches**. Are you comparing a string to a number?
```zia
var input = "42";    // This is a STRING
if input == 42 {     // Comparing string to number - might not work as expected
    ...
}
```

### Symptom: Wrong Branch Executes

The code takes the `else` path when it should take the `if` path (or vice versa).

**Diagnosis steps:**

1. **Print the condition result**:
```zia
bind Zanna.Terminal;

var age = 17;
var result = age >= 18;
Say("DEBUG: age >= 18 evaluates to: " + result);
```

2. **Simplify complex conditions**. Break them into parts:
```zia
bind Zanna.Terminal;

// Instead of:
if age >= 18 && hasID && !isBanned { ... }

// Debug with:
var age = 17;
var hasID = true;
var isBanned = false;
var ageOK = age >= 18;
var hasIDOK = hasID;
var notBanned = !isBanned;
Say("age >= 18: " + ageOK);
Say("hasID: " + hasIDOK);
Say("!isBanned: " + notBanned);
Say("all together: " + (ageOK && hasIDOK && notBanned));
```

3. **Check operator precedence**. Use parentheses to be explicit:
```zia
// What does this mean?
if a || b && c

// Be explicit:
if a || (b && c)    // OR
if (a || b) && c    // Different meaning!
```

### Symptom: Multiple Branches Run (When They Shouldn't)

You expected only one action, but multiple things happened.

**Diagnosis:** You probably used multiple `if` statements instead of `if-else if`:

```zia
bind Zanna.Terminal;

// BUG: Both messages might print!
var score = 95;
if score >= 60 {
    Say("You passed!");
}
if score >= 90 {
    Say("Excellent!");
}

// FIX: Use else if if only one should print
if score >= 90 {
    Say("Excellent!");
} else if score >= 60 {
    Say("You passed!");
}
```

### Symptom: Condition Order Problems

Your conditions are in the wrong order, and an earlier condition catches cases meant for later ones.

```zia
bind Zanna.Terminal;

// BUG: score = 95 prints "You passed" instead of "Excellent"
var score = 95;
if score >= 60 {
    Say("You passed!");
} else if score >= 90 {
    Say("Excellent!");    // Never reached for 95!
}

// FIX: Check more specific conditions first
if score >= 90 {
    Say("Excellent!");
} else if score >= 60 {
    Say("You passed!");
}
```

**Rule of thumb:** In `if-else if` chains with ranges, go from most specific to least specific, or from highest to lowest (or lowest to highest, but be consistent).

### The Print Debugging Technique

When in doubt, print everything:

```zia
bind Zanna.Terminal;

var age = 17;
var hasAccount = true;
var password = "secret123";
var inputPassword = "guess";

Say("=== DEBUG START ===");
Say("age = " + age);
Say("hasAccount = " + hasAccount);
Say("password = " + password);
Say("inputPassword = " + inputPassword);
Say("password == inputPassword: " + (password == inputPassword));
Say("=== DEBUG END ===");

if hasAccount && password == inputPassword {
    Say("Login successful!");
} else {
    Say("Login failed.");
}
```

This reveals exactly what values you're working with and how the condition evaluates.

### Common Logic Errors

**Off-by-one errors:**
```zia
bind Zanna.Terminal;

// User enters 18, expecting to pass, but fails
var age = 18;
if age > 18 {    // Should be >= 18
    Say("You can vote!");
}
```

**Inverted logic:**
```zia
bind Zanna.Terminal;

// Meant to check if NOT empty, but checked if empty
var name = "";
if name == "" {
    Say("Processing " + name);  // Bug! Processing an empty name
}
```

**Wrong boolean operator:**
```zia
// Used OR when AND was needed
if username == "admin" || password == "secret" {
    // Bug! Someone can log in with just the right username
}

// Should be:
if username == "admin" && password == "secret" {
    // Correct: need both
}
```

---

## Common Mistakes

**Forgetting braces:**
```zia
bind Zanna.Terminal;

if score > 100
    Say("High score!");  // Error: missing braces
```
Zia requires `{ }` around conditional blocks.

**Using = instead of ==:**
```zia
if score = 100 {   // Wrong: this assigns 100 to score
    ...
}
if score == 100 {  // Right: this compares score to 100
    ...
}
```

**Checking impossible conditions:**
```zia
var score = 95;
var grade = "";
if score >= 90 {
    grade = "A";
} else if score >= 80 {
    grade = "B";
} else if score >= 90 {   // This will never be true! We already checked >= 90
    grade = "A+";
}
```

**Overlapping conditions with separate ifs:**
```zia
bind Zanna.Terminal;

// Both could be true for score = 85
var score = 85;
if score >= 80 {
    Say("B or better");
}
if score >= 70 {
    Say("C or better");
}
```

If you want only one to run, use `else if`:
```zia
bind Zanna.Terminal;

var score = 85;
if score >= 80 {
    Say("B or better");
} else if score >= 70 {
    Say("C or better");
}
```

**Confusing && and ||:**
```zia
bind Zanna.Terminal;

// Wrong: This is never true (nothing is both < 0 AND > 100)
var score = 150;
if score < 0 && score > 100 {
    Say("Invalid");
}

// Right: Invalid if EITHER condition is true
if score < 0 || score > 100 {
    Say("Invalid");
}
```

---

## Putting It Together

Here's a number guessing game that uses everything we've learned:

```zia
module GuessGame;
bind Zanna.Terminal;

func start() {
    final SECRET = 7;
    final MAX_GUESSES = 3;
    var guessesRemaining = MAX_GUESSES;

    Say("I'm thinking of a number between 1 and 10.");
    Say("You have " + MAX_GUESSES + " guesses.");
    Say("");

    Print("Your guess: ");
    var guess = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));
    guessesRemaining = guessesRemaining - 1;

    // Validate input
    if guess < 1 || guess > 10 {
        Say("That's not between 1 and 10! You wasted a guess.");
    } else if guess == SECRET {
        Say("Correct! You win!");
        Say("You guessed it in " + (MAX_GUESSES - guessesRemaining) + " tries!");
    } else {
        // Give a hint
        if guess < SECRET {
            Say("Too low!");
        } else {
            Say("Too high!");
        }

        // Show remaining guesses
        if guessesRemaining > 0 {
            Say("You have " + guessesRemaining + " guesses left.");

            // Extra hint if this was close
            var difference = guess - SECRET;
            if difference < 0 {
                difference = -difference;  // Make positive
            }

            if difference == 1 {
                Say("Hint: You were VERY close!");
            } else if difference <= 3 {
                Say("Hint: You were somewhat close.");
            }
        } else {
            Say("No guesses remaining. The answer was " + SECRET + ".");
            Say("Better luck next time!");
        }
    }
}
```

This isn't a great game yet — the number is always 7, and you only get one guess per run! In Chapter 5, we'll add loops so the player can guess multiple times. In Chapter 13, we'll learn to generate random numbers for real games.

---

## Summary

- `if` checks a condition and runs code only when it's true
- `else` provides an alternative when the condition is false
- `else if` chains let you check multiple conditions
- Comparison operators (`==`, `!=`, `<`, `>`, `<=`, `>=`) produce booleans
- Logical operators combine booleans:
  - `&&` (AND): Both must be true
  - `||` (OR): At least one must be true
  - `!` (NOT): Flips true to false and vice versa
- `match` compares one value against many specific possibilities
- Conditions are checked in order; once one matches, the rest are skipped
- Use clear, explicit conditions — don't rely on implicit conversions
- Debug by printing values and breaking complex conditions into parts

---

## Exercises

**Exercise 4.1**: Write a program that asks for a number and prints whether it's positive, negative, or zero.

**Exercise 4.2**: Write a program that asks for the user's age and prints whether they can vote (18+), will be able to vote soon (16-17), or are too young (under 16).

**Exercise 4.3**: Write a program that asks for two numbers and prints which one is larger, or if they're equal.

**Exercise 4.4**: Write a program that asks for a year and prints whether it's a leap year. A year is a leap year if:
- It's divisible by 4, AND
- Either it's not divisible by 100, OR it's divisible by 400

**Exercise 4.5** (Challenge): Write a simple calculator that asks for two numbers and an operation (+, -, *, /) and prints the result. Handle division by zero with an error message.

**Exercise 4.6**: Write a program that asks for three numbers and prints them in order from smallest to largest. (Hint: You'll need nested conditions to handle all possible orderings.)

**Exercise 4.7**: Write a rock-paper-scissors game. Ask the user for their choice (1=rock, 2=paper, 3=scissors), set the computer's choice to a fixed value, and determine the winner.

**Exercise 4.8** (Challenge): Build a simple text adventure with at least 3 decision points. For example: "You enter a dark cave. Do you (1) light a torch, (2) proceed carefully, or (3) go back?" Each choice leads to different outcomes.

---

*We can now make decisions. But what if we need to do something many times — print 100 lines, check every character in a name, run a game loop 60 times per second? Next, we learn about repetition.*

*[Continue to Chapter 5: Repetition ->](05-repetition.md)*
