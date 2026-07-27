---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 14: Objects and Entities

In Chapter 11, we learned about structures -- grouping data together into cohesive units. A `Point` bundles x and y coordinates. A `Player` bundles name, health, and position. Structures solved the problem of scattered, disconnected data.

But structures have limitations that become apparent as your programs grow more complex. What if you want to ensure a player's health never goes negative? What if you want different types of game characters to share common behavior? What if you want to hide the internal representation of a data type so you can change it later without breaking code that uses it?

These questions lead us to *object-oriented programming* (OOP) -- a way of thinking about programs as collections of interacting *objects*. Objects combine data and behavior into unified entities that protect their internal state, expose controlled interfaces to the outside world, and can relate to each other in powerful ways.

This chapter introduces the fundamentals of object-oriented programming in Zia. By the end, you'll understand what objects are, why they matter, and how to design them well.

---

## Why Do We Need Objects?

Before diving into syntax, let's understand the problems that objects solve. This motivation is crucial -- without it, objects seem like unnecessary complexity.

### Problem 1: Data Without Protection

Consider a simple `BankAccount` structure:

```zia
struct BankAccount {
    expose String ownerName;
    expose Number balance;
}

var account = new BankAccount("Alice", 1000.0);
```

This works, but there's nothing stopping someone from writing:

```zia
account.balance = -500.0;  // Negative balance!
account.balance = account.balance * 1000000;  // Instant millionaire!
```

The structure has no way to enforce rules about its own data. It's just passive storage. Any code, anywhere in the program, can reach in and modify the balance to any value -- valid or not.

In a real banking system, this would be catastrophic. The balance should only change through deposits and withdrawals, and those operations should enforce business rules: you can't withdraw more than you have, deposits must be positive amounts, and so on.

### Problem 2: Behavior Scattered from Data

With structures, you write functions that operate on data:

```zia
func deposit(account: BankAccount, amount: Number) {
    if amount > 0 {
        account.balance += amount;
    }
}

func withdraw(account: BankAccount, amount: Number) -> Boolean {
    if amount > 0 && amount <= account.balance {
        account.balance -= amount;
        return true;
    }
    return false;
}
```

This is better -- now we have functions that enforce rules. But there are problems:

1. **Nothing forces their use.** Someone can still modify `account.balance` directly, bypassing your carefully written validation logic.

2. **The connection is invisible.** If you're looking at a `BankAccount`, there's no way to know what functions operate on it. You have to search through the codebase to find all the related functions.

3. **Organization breaks down.** As your program grows, you might have hundreds of functions operating on dozens of structures. Which functions go with which structures? The grouping exists only in your head and your file organization.

### Problem 3: No Guarantee of Consistency

A structure is just raw data. There's no built-in way to ensure it starts in a valid state:

```zia
struct Rectangle {
    expose Number width;
    expose Number height;
}

var rect = new Rectangle(0.0, 0.0);  // Created with default values
rect.width = -5.0;  // Negative width makes no sense!
```

What does a rectangle with negative dimensions even mean? The structure allows it because it has no concept of "valid" vs "invalid" state. It's up to every piece of code that creates or modifies a `Rectangle` to remember the rules.

### Problem 4: Changes Ripple Everywhere

Say you have a structure for representing colors:

```zia
struct Color {
    expose Integer red;    // 0-255
    expose Integer green;  // 0-255
    expose Integer blue;   // 0-255
}
```

Code throughout your program accesses `color.red`, `color.green`, and `color.blue` directly. Hundreds of lines use these fields.

Later, you realize you need transparency (alpha channel). Or you decide to represent colors as hue-saturation-lightness instead. Or you want to store colors as a single 32-bit integer for efficiency.

Every change to the internal representation breaks every piece of code that accesses the fields directly. You're trapped by your initial design decisions.

### The Object-Oriented Solution

Objects solve all these problems:

1. **Encapsulation**: Objects can hide their internal data and expose only controlled access through methods. The balance can only be modified through `deposit()` and `withdraw()`.

2. **Unified behavior**: Methods live inside the object definition, right alongside the data they operate on. When you look at a `BankAccount` class, you see everything it can do.

3. **Guaranteed initialization**: Objects are created through initializers that can validate and set up the object properly. You can't create an invalid object if the initializer checks its inputs.

4. **Stable interfaces**: External code interacts with objects through methods, not by touching fields directly. You can completely change the internal representation without affecting code that uses the object.

Objects don't just organize your code -- they make it possible to build systems that maintain their own integrity.

---

## From Structs to Classes

In Zia, the `struct` keyword creates a structure -- pure data without behavior. The `class` keyword creates something more powerful: a template for objects that combine data and behavior.

A struct groups data:

```zia
struct Rectangle {
    expose Number width;
    expose Number height;
}
```

A *class* groups data *and* behavior:

```zia
class Rectangle {
    expose Number width;
    expose Number height;

    expose func init(width: Number, height: Number) {
        self.width = width;
        self.height = height;
    }

    expose func area() -> Number {
        return self.width * self.height;
    }

    expose func perimeter() -> Number {
        return 2 * (self.width + self.height);
    }
}
```

The key differences:

- **`class` instead of `struct`**: Signals that this is an object template with behavior
- **An initializer**: The special `init` function that runs when creating new objects
- **Methods**: Functions that belong to the class and operate on its data
- **`self`**: A reference to the specific object the method is operating on

Let's explore each of these in depth.

---

## Classes and Structs

If you've learned about object-oriented programming in other languages, Zia's terminology will feel familiar. Like Java, Python, C++, and C#, Zia uses `class` to define object templates.

A class is:

- **A template for things.** A `Player` is a thing. A `BankAccount` is a thing. A `Rectangle` is a thing.
- **Concrete and tangible.** You can imagine pointing at a class instance and saying "that's a Player" or "that's a bank account."
- **Focused on identity and behavior.** Classes have state that changes over time and methods that operate on that state.

This contrasts with `struct`, which is for pure data:

- A `Point` is just coordinates -- data without behavior or identity
- A `Color` is just RGB values -- data you might copy and compare
- A `Config` is just settings -- passive information

The distinction helps you think clearly about your design: "Am I modeling a *thing* with behavior and identity (`class`)? Or am I just grouping *data* together (`struct`)?"

### Terminology Throughout This Book

Zia uses familiar OOP vocabulary:

| Zia Term | Common OOP Term |
|----------------|-----------------|
| class | class |
| struct | struct, record |
| expose | public |
| hide | private |
| init | constructor |

When you see code from other languages in "The Two Languages" sections, they use their own terminology -- BASIC uses `CLASS`. But these all compile to the same underlying system. The concepts are universal; only the words differ.

---

## The Template and Instance Distinction

This concept is fundamental, so let's make it crystal clear.

A **class** is a template -- a blueprint, a cookie cutter, a factory mold. It describes what objects of this type look like (their fields) and what they can do (their methods). The class itself is not a thing you can use; it's a description of things.

An **object** (also called an **instance**) is a specific thing created from that template. Each object has its own copy of the data fields, with its own values. You can have many objects created from the same class, each independent of the others.

Think of it like this:

- **Class `Dog`**: "A Dog has a name, an age, and can bark."
- **Object `fido`**: A specific dog named "Fido" who is 3 years old.
- **Object `rex`**: A different dog named "Rex" who is 7 years old.

Both `fido` and `rex` are Dogs -- they follow the same template. But they're separate objects with their own data. When Fido barks, Rex doesn't bark. When Rex's age increases, Fido's age stays the same.

```zia
bind Zanna.Terminal;

class Dog {
    expose String name;
    expose Integer age;

    expose func init(name: String, age: Integer) {
        self.name = name;
        self.age = age;
    }

    expose func bark() {
        Say(self.name + " says woof!");
    }

    expose func haveBirthday() {
        self.age += 1;
        Say(self.name + " is now " + self.age + "!");
    }
}

// Create two separate Dog objects
var fido = new Dog("Fido", 3);
var rex = new Dog("Rex", 7);

fido.bark();         // "Fido says woof!"
rex.bark();          // "Rex says woof!"

fido.haveBirthday(); // "Fido is now 4!"
Say(rex.age);  // Still 7 - Rex didn't age
```

This distinction matters because:

1. **Classes are defined once.** You write the `Dog` class definition one time. It describes all possible dogs.

2. **Objects are created many times.** Each `new Dog(...)` expression creates a new, independent object.

3. **Objects have independent state.** Changing one object doesn't affect others.

4. **Methods operate on specific objects.** When you call `fido.bark()`, the method runs in the context of `fido`, not all dogs everywhere.

---

## Creating Objects

You create an object with `new` followed by the class initializer arguments:

```zia
bind Zanna.Terminal;

var rect1 = new Rectangle(10.0, 5.0);
var rect2 = new Rectangle(3.0, 4.0);

Say(rect1.area());  // 50
Say(rect2.area());  // 12
```

Each call creates a fresh object. The arguments you pass go to the initializer, which sets up the object's initial state.

What happens when you write `new Rectangle(10.0, 5.0)`?

1. **Memory is allocated** for a new Rectangle object
2. **The initializer runs** with `width = 10.0` and `height = 5.0`
3. **The initializer assigns fields** using `self.width = width` and `self.height = height`
4. **The new object is returned** and assigned to `rect1`

Now `rect1` refers to a specific Rectangle object with its own width and height. When you create `rect2`, the same process happens, creating a completely separate object.

---

## The Initializer

The initializer is a special method that runs when you create a new object. Its job is to set up the object's initial state -- to make sure the object starts its life in a valid, usable condition.

```zia
class Person {
    expose String name;
    expose Integer age;

    expose func init(name: String, age: Integer) {
        self.name = name;
        self.age = age;
    }
}

var alice = new Person("Alice", 30);
```

### Why Initializers Matter

Without initializers, objects would start with uninitialized or default values. Consider:

```zia
// Hypothetically, if we could create objects without initializers:
var person = new Person();  // What is person.name? What is person.age?
```

The object would be in a meaningless state. The initializer ensures that every object starts with the data it needs.

### Multiple Initializers

You can define multiple initializers with different parameter lists. This is called *overloading* -- the same name (`init`) with different signatures:

```zia
class Person {
    expose String name;
    expose Integer age;
    expose String email;

    // Full initialization
    expose func init(name: String, age: Integer, email: String) {
        self.name = name;
        self.age = age;
        self.email = email;
    }

    // Without email
    expose func init(name: String, age: Integer) {
        self.name = name;
        self.age = age;
        self.email = "";  // Default to empty string
    }

    // Just a name
    expose func init(name: String) {
        self.name = name;
        self.age = 0;      // Default age
        self.email = "";   // Default email
    }
}

var alice = new Person("Alice", 30, "alice@example.com");
var bob = new Person("Bob", 25);        // No email
var baby = new Person("Baby");          // Just a name
```

Multiple initializers let callers provide different levels of detail. Some objects need full information; others can use sensible defaults.

### Validation in Initializers

Initializers are the perfect place to validate input and ensure objects start in valid states:

```zia
bind Zanna.Terminal;

class Rectangle {
    expose Number width;
    expose Number height;

    expose func init(width: Number, height: Number) {
        // Ensure dimensions are positive
        if width <= 0 {
            self.width = 1.0;  // Use minimum valid value
        } else {
            self.width = width;
        }

        if height <= 0 {
            self.height = 1.0;
        } else {
            self.height = height;
        }
    }
}

// Even with invalid input, the object is valid
var rect = new Rectangle(-5.0, 0.0);
Say(rect.width);   // 1.0 (corrected)
Say(rect.height);  // 1.0 (corrected)
```

You might also choose to report errors differently -- storing an error flag, logging a message, or using Zia's error handling mechanisms. The key insight is that the initializer controls how objects come into existence.

### Setup Logic in Initializers

Initializers can do more than just assign fields. They can perform any setup logic the object needs:

```zia
class GameCharacter {
    expose String name;
    expose Integer health;
    expose Integer maxHealth;
    expose Integer level;
    expose Integer experience;
    expose List[String] inventory;

    expose func init(name: String) {
        self.name = name;
        self.level = 1;
        self.experience = 0;

        // Calculate initial health based on level
        self.maxHealth = 100 + (self.level * 10);
        self.health = self.maxHealth;

        // Start with empty inventory
        self.inventory = [];

        // Could also: load saved data, connect to a server, initialize subsystems...
    }
}
```

### Default Values and Computed Fields

Some fields have values that depend on other fields. The initializer is where you establish these relationships:

```zia
bind Zanna.Math as Math;

class Circle {
    expose Number radius;
    expose Number diameter;
    expose Number circumference;

    expose func init(radius: Number) {
        self.radius = radius;
        self.diameter = radius * 2;
        self.circumference = 2 * PI * radius;
    }
}
```

However, this pattern can be dangerous -- if `radius` changes later, `diameter` and `circumference` will be out of sync. It's often better to compute derived values in methods:

```zia
bind Zanna.Math as Math;

class Circle {
    expose Number radius;

    expose func init(radius: Number) {
        self.radius = radius;
    }

    expose func diameter() -> Number {
        return self.radius * 2;  // Always computed fresh
    }

    expose func circumference() -> Number {
        return 2.0 * 3.14159265358979 * self.radius;  // Always accurate
    }
}
```

This leads us to an important principle: **initializers set up the essential state; methods compute derived information on demand.**

---

## Understanding `self`: The Current Object

Inside a method, you need a way to refer to the specific object the method was called on. That's what `self` provides.

```zia
class Counter {
    expose Integer count;

    expose func init() {
        self.count = 0;
    }

    expose func increment() {
        self.count += 1;
    }

    expose func getCount() -> Integer {
        return self.count;
    }
}
```

### What IS `self`?

`self` is a special variable that exists inside every method. It automatically refers to the object on which the method was called.

Think of it as the method's window into its own object's data. Without `self`, the method would have no way to know which object's `count` to read or modify.

### Step-by-Step: What Happens When You Call a Method

Let's trace through exactly what happens:

```zia
bind Zanna.Terminal;

var counter = new Counter();   // Step 1: Create object
counter.increment();           // Step 2: Call method
Say(counter.getCount());       // Step 3: Get value
```

**Step 1: `new Counter()` creates an object**
- Memory is allocated for a new Counter
- The initializer runs, with `self` pointing to this new object
- `self.count = 0` sets the new object's count field to 0
- The object is returned and assigned to `counter`

**Step 2: `counter.increment()` is called**
- Zia sees you're calling `increment` on the object stored in `counter`
- Inside `increment()`, `self` automatically refers to that object
- `self.count += 1` adds 1 to that specific object's count field
- The count is now 1

**Step 3: `counter.getCount()` is called**
- Again, `self` refers to the object stored in `counter`
- `return self.count` returns that object's count value (1)

### Why Methods Need `self`

Without `self`, methods would have no way to access their object's data:

```zia
func increment() {
    count += 1;  // Error! What count? count isn't defined here.
}
```

The variable `count` doesn't exist in the method's local scope. It's a field that belongs to an object -- but which object? There could be thousands of Counter objects in memory. The method needs `self` to know which one it's operating on.

### `self` with Multiple Objects

This becomes clearer with multiple objects:

```zia
bind Zanna.Terminal;

var counterA = new Counter();
var counterB = new Counter();

counterA.increment();  // Inside: self = counterA, self.count goes 0 -> 1
counterA.increment();  // Inside: self = counterA, self.count goes 1 -> 2
counterB.increment();  // Inside: self = counterB, self.count goes 0 -> 1

Say(counterA.getCount());  // 2
Say(counterB.getCount());  // 1
```

Each call binds `self` to the appropriate object. The method is the same code, but `self` changes based on which object the method was called on.

### `self` is Implicit in Some Languages

Some languages (Python, for example) require you to explicitly list `self` as the first parameter of every method. Zia doesn't -- `self` is automatically available inside methods.

This is cleaner and less repetitive, but it's important to understand that `self` is still there, working behind the scenes.

---

## Encapsulation: Protecting Object State

One of the most powerful ideas in object-oriented programming is *encapsulation* -- hiding an object's internal details and exposing only a controlled interface.

### The Problem: Unrestricted Access

Consider what happens when anyone can modify an object's fields directly:

```zia
class BankAccount {
    expose String ownerName;
    expose Number balance;  // Completely exposed!

    expose func init(owner: String, initial: Number) {
        self.ownerName = owner;
        self.balance = initial;
    }
}

var account = new BankAccount("Alice", 100.0);

// Anyone can do anything:
account.balance = -500.0;      // Negative balance
account.balance = 1000000.0;   // Instant millionaire
account.ownerName = "";        // Nameless account
```

There are no rules, no validation, no protection. The object is just passive data that any code can manipulate however it wants.

### The Solution: `hide` and `expose`

Zia lets you control what's visible from outside the class:

```zia
class BankAccount {
    hide balance: Number;
    expose String ownerName;

    expose func init(owner: String, initialDeposit: Number) {
        self.ownerName = owner;
        if initialDeposit < 0 {
            self.balance = 0.0;  // Don't allow negative initial balance
        } else {
            self.balance = initialDeposit;
        }
    }

    expose func deposit(amount: Number) {
        if amount > 0 {
            self.balance += amount;
        }
    }

    expose func withdraw(amount: Number) -> Boolean {
        if amount > 0 && amount <= self.balance {
            self.balance -= amount;
            return true;
        }
        return false;
    }

    expose func getBalance() -> Number {
        return self.balance;
    }
}
```

Now the balance is hidden -- external code cannot access it directly:

```zia
bind Zanna.Terminal;

var account = new BankAccount("Alice", 100.0);
account.deposit(50.0);
Say(account.getBalance());  // 150

// account.balance = 1000000;  // Error: balance is hidden
```

The only way to change the balance is through `deposit()` and `withdraw()`, which enforce the rules.

### What is an Invariant?

An *invariant* is a condition that must always be true for an object to be in a valid state. Encapsulation lets you protect invariants.

For `BankAccount`, the invariant might be: "Balance is always non-negative."

Without encapsulation, any code can violate this invariant by setting `balance` to a negative number. With encapsulation:

- The initializer ensures we start with a non-negative balance
- `deposit()` only adds positive amounts
- `withdraw()` refuses to go below zero

As long as all modifications go through these methods, the invariant is guaranteed. The object protects its own consistency.

### Common Invariants

Different classes have different invariants:

```zia
class Temperature {
    hide kelvin: Number;

    expose func init(kelvin: Number) {
        // Invariant: Temperature cannot be below absolute zero
        if kelvin < 0 {
            self.kelvin = 0;
        } else {
            self.kelvin = kelvin;
        }
    }

    expose func setKelvin(k: Number) {
        if k >= 0 {
            self.kelvin = k;
        }
    }
    // ...
}

class Password {
    hide hash: String;

    expose func init(plaintext: String) {
        // Invariant: Never store plaintext passwords
        self.hash = computeHash(plaintext);
    }

    expose func check(attempt: String) -> Boolean {
        return computeHash(attempt) == self.hash;
    }
    // The plaintext is never stored or retrievable
}

class OrderedPair {
    hide first: Integer;
    hide second: Integer;

    expose func init(a: Integer, b: Integer) {
        // Invariant: first <= second always
        if a <= b {
            self.first = a;
            self.second = b;
        } else {
            self.first = b;
            self.second = a;
        }
    }
    // ...
}
```

### What Problems Does Encapsulation Prevent?

1. **Invalid states**: Objects can't be put into states that violate their rules

2. **Uncontrolled access**: External code can't bypass validation logic

3. **Hidden implementation changes**: You can change internal representation without affecting code that uses the class

4. **Debugging complexity**: When something goes wrong with an object's state, you know the problem is in one of the class's own methods -- not scattered throughout the codebase

5. **Coupling**: Code that uses the class depends only on its public interface, not its internal structure

### Hide by Default

A good practice is to hide fields by default and only expose what's necessary. Ask yourself: "Does external code *need* to access this directly?"

Usually, the answer is no. External code needs to *do things* with the object (call methods), not *poke at its internals* (access fields).

```zia
class Player {
    hide name: String;
    hide health: Integer;
    hide maxHealth: Integer;
    hide x: Number;
    hide y: Number;

    // External code can get information...
    expose func getName() -> String { return self.name; }
    expose func getHealth() -> Integer { return self.health; }
    expose func getPosition() -> (Number, Number) { return (self.x, self.y); }

    // ...and perform actions that enforce game rules
    expose func takeDamage(amount: Integer) { ... }
    expose func heal(amount: Integer) { ... }
    expose func moveTo(x: Number, y: Number) { ... }
}
```

---

## Methods: Behavior That Belongs to Objects

Methods define what objects can *do*. They're functions that belong to a class and operate on its data through `self`.

```zia
bind Zanna.Math as Math;

class Circle {
    expose Number radius;

    expose func init(radius: Number) {
        self.radius = radius;
    }

    expose func area() -> Number {
        return 3.14159265358979 * self.radius * self.radius;
    }

    expose func circumference() -> Number {
        return 2.0 * 3.14159265358979 * self.radius;
    }

    expose func scale(factor: Number) {
        self.radius *= factor;
    }

    expose func diameter() -> Number {
        return self.radius * 2;
    }
}
```

### What Methods Can Do

Methods can:

- **Read the object's data**: `area()` uses `self.radius` to compute a value
- **Modify the object's data**: `scale()` changes `self.radius`
- **Take parameters**: `scale(factor)` takes a scaling factor
- **Return values**: `area()` returns the computed area
- **Call other methods on the same object**: A method could call `self.area()` or `self.diameter()`
- **Call methods on other objects**: If passed another object, methods can interact with it

### Types of Methods

Methods generally fall into a few categories:

**Accessors (Getters)** -- Return information about the object:

```zia
func getName() -> String {
    return self.name;
}

func getHealth() -> Integer {
    return self.health;
}
```

**Mutators (Setters/Modifiers)** -- Change the object's state:

```zia
func setName(newName: String) {
    self.name = newName;
}

func takeDamage(amount: Integer) {
    self.health -= amount;
    if self.health < 0 {
        self.health = 0;
    }
}
```

**Computed Properties** -- Calculate values from the object's state:

```zia
func area() -> Number {
    return self.width * self.height;
}

func isAlive() -> Boolean {
    return self.health > 0;
}
```

**Actions** -- Perform complex operations:

```zia
func attack(target: Enemy) {
    var damage = self.calculateDamage();
    target.takeDamage(damage);
    self.gainExperience(10);
}

func save() {
    // Write object state to a file
}
```

### Methods vs Functions: When to Use Which

Sometimes you have a choice: should this be a method on a class, or a standalone function?

**Use a method when:**

- The operation primarily works with one object's data
- The operation is conceptually "what this thing does"
- You want to take advantage of encapsulation (accessing hidden fields)

```zia
bind Zanna.Math as Math;

// Good as a method - operates on the circle's own data
class Circle {
    hide Number radius;

    expose func init(radius: Number) {
        self.radius = radius;
    }

    expose func area() -> Number {
        return 3.14159265358979 * self.radius * self.radius;
    }
}
```

**Use a function when:**

- The operation works with multiple objects equally (no clear "main" object)
- The operation is a general utility that doesn't belong to any particular class
- The operation doesn't need access to hidden state

```zia
bind Zanna.Math as Math;

// Good as a function - works with two objects equally
func distance(p1: Point, p2: Point) -> Number {
    var dx = p2.x - p1.x;
    var dy = p2.y - p1.y;
    return Math.Sqrt(dx*dx + dy*dy);
}
```

### Good Method Design

**Keep methods focused.** Each method should do one thing well:

```zia
// Bad: method does too many things
func processOrder() {
    // validate order
    // calculate shipping
    // charge credit card
    // update inventory
    // send email
    // log transaction
    // update customer loyalty points
}

// Good: split into focused methods
func validate() -> Boolean { ... }
func calculateShipping() -> Number { ... }
func chargePayment(amount: Number) -> Boolean { ... }
func updateInventory() { ... }
func sendConfirmationEmail() { ... }
```

**Name methods as verbs.** Methods do things, so their names should be action words:

```zia
// Good names - clear actions
func deposit(amount: Number) { ... }
func withdraw(amount: Number) { ... }
func save() { ... }
func display() { ... }

// Poor names - not clear what they do
func money(amount: Number) { ... }
func doIt() { ... }
func process() { ... }
```

**Be careful with methods that both read and modify.** If a method returns a value AND changes state, it can be confusing:

```zia
// Confusing: does this modify count, or just return it?
func getAndIncrement() -> Integer {
    var old = self.count;
    self.count += 1;
    return old;
}

// Clearer: separate operations
func getCount() -> Integer { return self.count; }
func increment() { self.count += 1; }
```

---

## Objects Have State That Changes Over Time

A key characteristic of objects is that they have *state* -- data that can change as the program runs. This is different from pure values, which are typically immutable.

### What is State?

State is the current condition of an object -- the values of all its fields at a particular moment. An object's state:

- Is established by the initializer
- Can change over time through method calls
- Determines how the object behaves

```zia
bind Zanna.Terminal;

class TrafficLight {
    hide color: String;

    expose func init() {
        self.color = "red";  // Initial state
    }

    expose func advance() {
        if self.color == "red" {
            self.color = "green";
        } else if self.color == "green" {
            self.color = "yellow";
        } else {
            self.color = "red";
        }
    }

    expose func getColor() -> String {
        return self.color;
    }
}

var light = new TrafficLight();
Say(light.getColor());  // "red"

light.advance();
Say(light.getColor());  // "green"

light.advance();
Say(light.getColor());  // "yellow"

light.advance();
Say(light.getColor());  // "red" again
```

The traffic light object transitions through states: red, green, yellow, red, green, yellow... Each call to `advance()` changes the state.

### State Diagrams

When designing classes with complex state, it helps to think in terms of state diagrams:

```text
    +---------+       +---------+       +----------+
    |   RED   | ----> |  GREEN  | ----> |  YELLOW  |
    +---------+       +---------+       +----------+
         ^                                    |
         +------------------------------------+
```

The class enforces valid transitions. You can't go directly from red to yellow -- the `advance()` method only allows red->green, green->yellow, yellow->red.

### Thinking About State

When designing a class, ask:

1. **What states can the object be in?** A player might be "alive", "dead", or "respawning". A connection might be "disconnected", "connecting", "connected", or "error".

2. **What are valid transitions?** Can you go from "dead" to "alive" directly, or only through "respawning"? Can a "disconnected" connection become "error" without going through "connecting"?

3. **What triggers transitions?** Method calls change state. The `takeDamage()` method might transition a player from "alive" to "dead" when health reaches zero.

4. **What behavior differs by state?** A "dead" player might ignore movement commands. A "disconnected" connection might queue messages instead of sending them.

```zia
class GameCharacter {
    hide health: Integer;
    hide state: String;  // "alive", "dead", "respawning"

    expose func init(name: String) {
        // ...
        self.health = 100;
        self.state = "alive";
    }

    expose func takeDamage(amount: Integer) {
        if self.state != "alive" {
            return;  // Can't damage dead/respawning characters
        }

        self.health -= amount;
        if self.health <= 0 {
            self.health = 0;
            self.state = "dead";
        }
    }

    expose func respawn() {
        if self.state == "dead" {
            self.state = "respawning";
            // Start respawn timer...
        }
    }

    expose func finishRespawn() {
        if self.state == "respawning" {
            self.health = 100;
            self.state = "alive";
        }
    }

    expose func move(dx: Number, dy: Number) {
        if self.state == "alive" {
            // Actually move
        }
        // Dead and respawning characters ignore movement
    }
}
```

### State and Encapsulation Work Together

Encapsulation is crucial for maintaining valid state. If external code could modify `state` directly, it could put the character in impossible states:

```zia
// If state were exposed:
character.state = "flying";  // Not a valid state!
character.state = "alive";   // Resurrected without going through respawn!
character.health = -50;      // Invalid health!
```

By hiding state and controlling access through methods, the class ensures it only ever enters valid states through valid transitions.

---

## Practical Design: Building an Entity from Scratch

Let's walk through the process of designing a class from the ground up. This will tie together everything we've learned.

### The Problem

We want to model a simple bank account for a personal finance application. Users can deposit money, withdraw money, and view their transaction history.

### Step 1: Identify the Core Data

What information does a bank account need to track?

- Account holder's name
- Current balance
- Transaction history (a list of past transactions)

Let's also track:
- Account number (for identification)
- Date opened

### Step 2: Identify the Invariants

What rules must always be true?

- Balance must never be negative
- Withdrawals cannot exceed the current balance
- Deposits must be positive amounts
- Account number cannot change after creation
- Transaction history should be append-only (you can't erase history)

### Step 3: Identify the Behaviors

What should a bank account be able to do?

- Accept deposits
- Process withdrawals
- Show current balance
- Show transaction history
- Display account information

### Step 4: Decide What to Hide

Following the principle of "hide by default":

- `balance`: Hidden -- must change through deposit/withdraw
- `transactions`: Hidden -- should only grow through deposits/withdrawals
- `accountNumber`: Hidden -- should never change
- `ownerName`: Could be exposed for reading, but changes should be controlled
- `dateOpened`: Hidden -- set once at creation

### Step 5: Write the Entity

```zia
bind Zanna.Terminal;
bind Zanna.Time.DateTime as DateTime;

class BankAccount {
    hide accountNumber: String;
    hide ownerName: String;
    hide balance: Number;
    hide transactions: List[String];
    hide dateOpened: String;

    // Main initializer with full information
    expose func init(accountNumber: String, ownerName: String, initialDeposit: Number) {
        self.accountNumber = accountNumber;
        self.ownerName = ownerName;
        var _dt = DateTime.Now();
        self.dateOpened = DateTime.Year(_dt) + "-" + DateTime.Month(_dt) + "-" + DateTime.Day(_dt);
        self.transactions = [];

        // Enforce non-negative initial balance
        if initialDeposit < 0 {
            self.balance = 0.0;
            self.addTransaction("Account opened with $0.00 (invalid initial deposit rejected)");
        } else {
            self.balance = initialDeposit;
            self.addTransaction("Account opened with $" + initialDeposit);
        }
    }

    // Simplified initializer for zero-balance accounts
    expose func init(accountNumber: String, ownerName: String) {
        self.accountNumber = accountNumber;
        self.ownerName = ownerName;
        var _dt = DateTime.Now();
        self.dateOpened = DateTime.Year(_dt) + "-" + DateTime.Month(_dt) + "-" + DateTime.Day(_dt);
        self.balance = 0.0;
        self.transactions = [];
        self.addTransaction("Account opened with $0.00");
    }

    // ===== Public Methods =====

    expose func deposit(amount: Number) -> Boolean {
        if amount <= 0 {
            return false;  // Invalid deposit amount
        }

        self.balance += amount;
        self.addTransaction("Deposit: +$" + amount + " (Balance: $" + self.balance + ")");
        return true;
    }

    expose func withdraw(amount: Number) -> Boolean {
        if amount <= 0 {
            return false;  // Invalid withdrawal amount
        }

        if amount > self.balance {
            self.addTransaction("Withdrawal DECLINED: $" + amount + " (Insufficient funds)");
            return false;  // Insufficient funds
        }

        self.balance -= amount;
        self.addTransaction("Withdrawal: -$" + amount + " (Balance: $" + self.balance + ")");
        return true;
    }

    expose func getBalance() -> Number {
        return self.balance;
    }

    expose func getAccountNumber() -> String {
        return self.accountNumber;
    }

    expose func getOwnerName() -> String {
        return self.ownerName;
    }

    expose func getTransactionHistory() -> List[String] {
        // Return a copy so external code can't modify our history
        return self.transactions.copy();
    }

    expose func printStatement() {
        Say("========================================");
        Say("Account Statement");
        Say("Account: " + self.accountNumber);
        Say("Owner: " + self.ownerName);
        Say("Opened: " + self.dateOpened);
        Say("Current Balance: $" + self.balance);
        Say("----------------------------------------");
        Say("Transaction History:");

        for transaction in self.transactions {
            Say("  " + transaction);
        }

        Say("========================================");
    }

    // ===== Private Helper Methods =====

    hide func addTransaction(description: String) {
        var _dt2 = DateTime.Now();
        var timestamp = DateTime.Hour(_dt2) + ":" + DateTime.Minute(_dt2) + ":" + DateTime.Second(_dt2);
        self.transactions.Push(timestamp + ": " + description);
    }
}
```

### Step 6: Use the Entity

```zia
bind Zanna.Terminal;

func start() {
    // Create an account
    var account = new BankAccount("1234-5678", "Alice Johnson", 500.0);

    // Make some transactions
    account.deposit(200.0);    // Balance: 700
    account.withdraw(50.0);    // Balance: 650
    account.deposit(100.0);    // Balance: 750
    account.withdraw(1000.0);  // Declined: insufficient funds

    // Check the balance
    Say("Current balance: $" + account.getBalance());

    // Print full statement
    account.printStatement();

    // Try to cheat (these would fail if balance were exposed):
    // account.balance = 1000000;  // Error: balance is hidden
    // account.transactions = [];  // Error: transactions is hidden
}
```

### What We Achieved

- **Invariants protected**: Balance can never go negative because `withdraw()` checks before subtracting
- **Controlled access**: All changes go through methods that enforce rules
- **Complete history**: Every significant action is logged
- **Clear interface**: Users of `BankAccount` know exactly what they can do
- **Hidden implementation**: We could change how we store transactions (database, file, etc.) without changing the public interface

---

## A Complete Example: Todo List Application

Let's put everything together with a more complete example:

```zia
module TodoApp;

bind Zanna.Terminal;
bind Zanna.Time;

class TodoItem {
    hide text: String;
    hide done: Boolean;
    hide createdAt: String;
    hide completedAt: String;

    expose func init(text: String) {
        self.text = text;
        self.done = false;
        var _dtc = DateTime.Now();
        self.createdAt = DateTime.Year(_dtc) + "-" + DateTime.Month(_dtc) + "-" + DateTime.Day(_dtc);
        self.completedAt = "";
    }

    expose func getText() -> String {
        return self.text;
    }

    expose func isDone() -> Boolean {
        return self.done;
    }

    expose func markDone() {
        if !self.done {
            self.done = true;
            var _dtx = DateTime.Now();
            self.completedAt = DateTime.Year(_dtx) + "-" + DateTime.Month(_dtx) + "-" + DateTime.Day(_dtx);
        }
    }

    expose func markUndone() {
        self.done = false;
        self.completedAt = "";
    }

    expose func toString() -> String {
        var status = "";
        if self.done {
            status = "[X]";
        } else {
            status = "[ ]";
        }
        return status + " " + self.text;
    }
}

class TodoList {
    hide items: List[TodoItem];
    hide name: String;

    expose func init(name: String) {
        self.name = name;
        self.items = [];
    }

    expose func add(text: String) {
        self.items.Push(new TodoItem(text));
    }

    expose func markDone(index: Integer) {
        if index >= 0 && index < self.items.Length {
            self.items[index].markDone();
        }
    }

    expose func markUndone(index: Integer) {
        if index >= 0 && index < self.items.Length {
            self.items[index].markUndone();
        }
    }

    expose func remove(index: Integer) {
        if index >= 0 && index < self.items.Length {
            self.items.RemoveAt(index);
        }
    }

    expose func display() {
        Say("=== " + self.name + " ===");
        if self.items.Length == 0 {
            Say("  (empty)");
            return;
        }

        for i in 0..self.items.Length {
            Say("  " + (i + 1) + ". " + self.items[i].toString());
        }

        Say("");
        Say("  " + self.countCompleted() + "/" + self.items.Length + " completed");
    }

    expose func countRemaining() -> Integer {
        var count = 0;
        for item in self.items {
            if !item.isDone() {
                count += 1;
            }
        }
        return count;
    }

    expose func countCompleted() -> Integer {
        var count = 0;
        for item in self.items {
            if item.isDone() {
                count += 1;
            }
        }
        return count;
    }

    expose func clear() {
        self.items = [];
    }

    expose func clearCompleted() {
        var remaining: List[TodoItem] = [];
        for item in self.items {
            if !item.isDone() {
                remaining.Push(item);
            }
        }
        self.items = remaining;
    }
}

func start() {
    var todos = new TodoList("My Tasks");

    todos.add("Learn Zia");
    todos.add("Build a project");
    todos.add("Read the documentation");

    todos.display();
    // === My Tasks ===
    //   1. [ ] Learn Zia
    //   2. [ ] Build a project
    //   3. [ ] Read the documentation
    //
    //   0/3 completed

    todos.markDone(0);
    todos.display();
    // === My Tasks ===
    //   1. [X] Learn Zia
    //   2. [ ] Build a project
    //   3. [ ] Read the documentation
    //
    //   1/3 completed

    Say("Remaining: " + todos.countRemaining());
    // Remaining: 2
}
```

Notice how:

- **`TodoItem` manages individual tasks**: Each item knows its text, status, and timestamps
- **`TodoList` manages the collection**: It handles adding, removing, and displaying items
- **Each class has one responsibility**: Items don't know about lists; lists don't know about item internals
- **Hidden fields protect state**: You can't directly mark an item done or modify the list's items array
- **Methods provide controlled access**: All operations go through validated methods

---

## The Two Languages

**Zia**

```zia
bind Zanna.Terminal;

class Dog {
    expose String name;

    expose func init(name: String) {
        self.name = name;
    }

    expose func bark() {
        Say(self.name + " says woof!");
    }
}

var dog = new Dog("Rex");
dog.bark();
```

**BASIC-style dialect comparison**

```text
CLASS Dog
    PUBLIC name AS STRING

    CONSTRUCTOR(n AS STRING)
        name = n
    END CONSTRUCTOR

    SUB Bark()
        PRINT name; " says woof!"
    END SUB
END CLASS

DIM dog AS Dog
dog = NEW Dog("Rex")
dog.Bark()
```

---

## Entity Design Guidelines

### Model Real Concepts

A class should represent a concrete thing you can describe and point to:

- A `Dog` class, a `BankAccount` class, a `Player` class -- things with identity
- A `ShoppingCart`, a `GameLevel`, a `UserSession` -- meaningful concepts

If you can't clearly describe what a class represents, it probably shouldn't be a class.

### Keep Entities Focused

A class should have one primary responsibility. If a class is doing too many things, split it:

```zia
// Too much responsibility
class Game {
    player: Player;
    expose List[Enemy] enemies;
    graphics: GraphicsSystem;
    sound: SoundSystem;
    input: InputHandler;
    network: NetworkConnection;
    // ... 50 methods for all these different things
}

// Better: separate classes for each concern
class Player { ... }
class EnemyManager { ... }
class Renderer { ... }
class SoundSystem { ... }
class InputHandler { ... }
class NetworkManager { ... }

class Game {
    player: Player;
    enemies: EnemyManager;
    renderer: Renderer;
    // ... orchestrates the others
}
```

### Hide Internals by Default

Make fields hidden unless there's a compelling reason to expose them. Hiding provides:

- Freedom to change implementation later
- Guaranteed enforcement of invariants
- Clear separation of interface from implementation

### Name Methods as Actions

Methods do things, so their names should be verbs:

- `deposit`, `withdraw`, `transfer` -- clear actions
- `save`, `load`, `delete` -- operations
- `display`, `render`, `print` -- output actions
- `calculate`, `compute`, `validate` -- computational actions

### Prefer Small, Focused Methods

Each method should do one thing well. If a method is long or does multiple distinct operations, consider splitting it:

```zia
// Too long -- doing multiple things
func processOrder() {
    // 50 lines of validation
    // 30 lines of payment processing
    // 40 lines of inventory update
    // 20 lines of email sending
}

// Better -- each method has one job
func validateOrder() -> Boolean { ... }
func processPayment() -> Boolean { ... }
func updateInventory() { ... }
func sendConfirmation() { ... }

func processOrder() {
    if !self.validateOrder() { return false; }
    if !self.processPayment() { return false; }
    self.updateInventory();
    self.sendConfirmation();
    return true;
}
```

---

## Common Mistakes

### Forgetting `self`

```zia
class Counter {
    expose Integer count;

    expose func increment() {
        count += 1;  // Error: should be self.count
    }
}
```

Inside a method, you must use `self` to access the object's fields. Without it, the compiler looks for a local variable named `count`, which doesn't exist.

### Exposing Fields That Should Be Hidden

```zia
class BankAccount {
    expose Number balance;  // Bad: anyone can modify directly
}

var account = new BankAccount();
account.balance = -1000.0;  // Oops, negative balance!
```

Exposed fields bypass all your carefully written validation logic. Hide them and provide controlled access through methods.

### Monster Entities

```zia
// Don't do this -- one class doing everything
class Game {
    player: ...;
    enemies: ...;
    graphics: ...;
    sound: ...;
    input: ...;
    network: ...;
    physics: ...;
    ui: ...;
    saves: ...;
    achievements: ...;
    // 100 methods covering all these different concerns
}
```

Such classes become impossible to understand, test, or modify. Split them into focused classes that each handle one concern.

### Methods That Do Too Much

```zia
func doEverything() {
    // Load data
    // Process data
    // Update UI
    // Save results
    // Send notifications
    // Log analytics
    // Update cache
    // ... 200 lines later ...
}
```

Long methods are hard to understand, test, and debug. Break them into smaller, focused methods.

### Ignoring Invalid Input

```zia
func setAge(age: Integer) {
    self.age = age;  // What if age is -5? Or 500?
}
```

Methods should validate input and handle invalid cases appropriately -- whether by rejecting, correcting, or reporting the error.

---

## Summary

This chapter introduced the fundamentals of object-oriented programming:

- **Objects solve problems** that values and functions alone cannot: protecting data integrity, unifying behavior with data, and providing stable interfaces
- **Entities are templates** that define what objects look like (fields) and can do (methods)
- **Objects are instances** created from classes, each with independent state
- **Initializers** set up objects in valid initial states
- **`self`** refers to the current object inside methods, connecting methods to their object's data
- **Encapsulation** (hide/expose) protects object state and enforces invariants
- **Methods** define object behavior -- what objects can do
- **State** changes over time through method calls, and good design ensures only valid states are reachable
- **Good class design** means one responsibility per class, hidden internals, clear interfaces, focused methods

Objects are the foundation of organizing complex programs. They let you build systems out of components that manage themselves, protect their own integrity, and interact through well-defined interfaces.

---

## Exercises

**Exercise 14.1**: Create a `Counter` class with `increment()`, `decrement()`, `reset()`, and `getValue()` methods. The count should never go below zero -- `decrement()` should do nothing if the count is already zero.

**Exercise 14.2**: Create a `Temperature` class that stores a temperature in Celsius and has methods `toFahrenheit()` and `toKelvin()`. Add validation to ensure the temperature never goes below absolute zero (-273.15 Celsius).

**Exercise 14.3**: Create a `Stopwatch` class with `start()`, `stop()`, `reset()`, and `elapsed()` methods. Think carefully about what states a stopwatch can be in (stopped, running) and what transitions are valid.

**Exercise 14.4**: Create a `Deck` class that represents a deck of playing cards with `shuffle()` and `draw()` methods. What should `draw()` return if the deck is empty?

**Exercise 14.5**: Create a `ShoppingCart` class that stores items with prices. Include methods to add items, remove items, get the item count, and calculate the total price. Consider: should you allow negative prices? Duplicate items?

**Exercise 14.6** (Challenge): Create a text-based RPG character: a `Character` class with name, health, maxHealth, attack power, and defense. Include methods for:
- Taking damage (reduced by defense)
- Healing (cannot exceed max health)
- Attacking another character
- Checking if alive
- Leveling up (increases max health and attack)

Think about invariants: health should be between 0 and maxHealth, attack and defense should be positive.

**Exercise 14.7** (Design Challenge): Design a `Thermostat` class for a home heating/cooling system. Consider:
- What data does it need? (current temperature, target temperature, mode, etc.)
- What are the valid states and transitions?
- What invariants must be maintained?
- What methods should it expose?

Write out your design first, then implement it.

---

*We can now create our own types with behavior and protected state. But what if we want specialized versions -- a `Cat` that's a kind of `Animal`, a `SavingsAccount` that's a kind of `BankAccount`? Next, we learn about inheritance.*

*[Continue to Chapter 15: Inheritance](15-inheritance.md)*
