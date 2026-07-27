---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 12: Modules

Your programs are growing. The game demo in Chapter 11 was already getting long. If you continued adding features, the file would become a sprawling mess of hundreds of functions, dozens of structures, and tangled logic that no one could understand.

This isn't a hypothetical problem. Real programs are massive. A web browser has millions of lines of code. A video game might have ten million. An operating system can exceed a hundred million. How do teams of developers manage these vast codebases without going insane?

The answer is *modules* — self-contained units of code that can be developed, tested, and understood independently. Modules let you split a large program into manageable pieces, share code between projects, and hide internal details behind clean interfaces.

But modules aren't just about managing size. They're about managing complexity. They're about creating boundaries that let you think about one thing at a time. They're about building software that you (and others) can understand, modify, and trust.

Code organization is one of those skills that separates beginners from intermediate programmers. Anyone can write a hundred lines of code. Writing a thousand lines that stay maintainable requires discipline. This chapter teaches that discipline.

---

## The Problem: Code Chaos

Let's start with why this matters. Imagine you're building a simple game with a player, enemies, and items. Without any organization, you might write everything in one file:

```zia
// game.zia — 500 lines and growing...

// Player stuff
var playerX = 0.0;
var playerY = 0.0;
var playerHealth = 100;
var playerSpeed = 5.0;

func movePlayer(dx: Number, dy: Number) { ... }
func damagePlayer(amount: Integer) { ... }
func healPlayer(amount: Integer) { ... }
func renderPlayer() { ... }

// Enemy stuff
var enemies: List[Enemy] = [];

func spawnEnemy(x: Number, y: Number) { ... }
func updateEnemies() { ... }
func renderEnemies() { ... }

// Item stuff
var items: List[Item] = [];

func spawnItem(x: Number, y: Number, type: String) { ... }
func checkItemPickup() { ... }
func renderItems() { ... }

// Physics stuff
func checkCollision(a: Rect, b: Rect) -> Boolean { ... }
func applyGravity(class: Entity) { ... }

// Rendering stuff
func clearScreen() { ... }
func drawSprite(x: Number, y: Number, sprite: Sprite) { ... }

// Audio stuff
func playSound(name: String) { ... }
func playMusic(name: String) { ... }

// ... hundreds more lines
```

This file has several problems:

**It's hard to navigate.** Where's the function that handles enemy AI? Somewhere in there. You scroll, you search, you get lost. In a file with 50 functions, finding the one you need becomes a treasure hunt.

**It's hard to understand.** When everything is in one place, relationships are unclear. Does `damagePlayer` call `playSound`? Does `updateEnemies` use physics functions? You can't tell without reading every line.

**It's hard to change.** Touch one thing and you might break something else. Rename a variable and who knows what depends on it? Fear of breaking things makes developers reluctant to improve code.

**It's hard to reuse.** The physics code might be useful in another project. But it's tangled up with player code and rendering code. You can't extract it cleanly.

**It's hard to collaborate.** Two people can't work on the same file without conflicts. If Alice is adding player abilities while Bob is improving enemy AI, they'll step on each other's changes constantly.

Modules solve all these problems.

---

## What Is a Module?

A module is a file containing related code: functions, structures, constants. Every Zia file is a module.

Here's a simple module:

```zia
// file: MathUtils.zia
module MathUtils;

expose func square(x: Number) -> Number {
    return x * x;
}

expose func cube(x: Number) -> Number {
    return x * x * x;
}

expose final PI = 3.14159265358979;
```

This module provides math utilities. Other code can *bind* this module to use its functions and constants.

The `module MathUtils;` declaration at the top gives this module a name. This name becomes important when other files want to use this code.

Think of modules like chapters in a book. Each chapter covers one topic. You can read a chapter without reading the whole book. You can reference a specific chapter. And if you're working with others, different people can write different chapters.

---

## The Namespace Problem

Before we go further, let's understand a fundamental problem that modules solve: name collisions.

Suppose you have a function called `add`. Simple, right? But what does `add` do? It might add numbers. It might add an item to a list. It might add a user to a database. Different parts of your program might all want a function called `add`, but each wants it to do something different.

Without modules, you're forced into awkward solutions:

```zia
// Without modules: ugly prefixed names
func mathAdd(a: Integer, b: Integer) -> Integer { ... }
func listAdd(list: List[Integer], item: Integer) { ... }
func databaseAddUser(user: User) { ... }
```

This works, but it's tedious. Every function name becomes a small essay. And it doesn't actually prevent collisions — what if two libraries both define `mathAdd`?

Modules provide *namespaces*. A namespace is like a last name for your functions. Just as there can be many people named "Alice" but only one "Alice Smith" and one "Alice Jones," there can be many `add` functions as long as they're in different modules:

```text
// With modules: clean, qualified names
bind Math;
bind List;
bind Database;

Math.Add(5, 3);            // Adds numbers (a module you wrote)
myList.Push(10);           // Adds to a Zia list
Database.addUser(alice);   // Adds to database
```

Each module creates its own namespace. Functions inside `Math` don't conflict with functions inside `List`. This simple idea — that names are scoped to modules — prevents countless headaches.

---

## Binding Modules

To use code from another module, you bind it:

```zia
// file: Main.zia
module Main;

bind MathUtils;
bind Zanna.Terminal;

func start() {
    var x = 5.0;
    Say("Square: " + MathUtils.square(x));
    Say("Cube: " + MathUtils.cube(x));
    Say("Pi: " + MathUtils.PI);
}
```

The `bind MathUtils` statement makes that module's contents available. You access them with the module name prefix: `MathUtils.square`.

This prefix might seem annoying at first, but it's actually helpful. When you read `MathUtils.square(x)`, you know exactly where that function comes from. You don't have to wonder "which `square` function is this?" The module name tells you.

---

## Different Ways to Bind

Zia provides several bind styles for different situations.

### Bind the Module

The basic bind brings in the whole module:

```zia
bind MathUtils;

// Use with prefix
var area = MathUtils.PI * MathUtils.square(radius);
```

This is the safest approach. Names are always fully qualified, so there's no confusion.

### Bind Specific Items

If you only need certain items, bind them directly:

```zia
bind MathUtils { square, PI };
bind Zanna.Terminal;

func start() {
    Say(square(5.0));  // No prefix needed
    Say(PI);
}
```

This binds only `square` and `PI`, and you can use them without the module prefix. It's convenient when you use certain functions frequently, but be careful — if another module has a `square` function, you'll get a conflict.

### Bind with Aliases

Sometimes you want to rename a binding. Maybe the name is too long, or maybe it conflicts with something else:

```zia
bind MathUtils as M;
bind Zanna.Terminal;

func start() {
    Say(M.square(5.0));
    Say(M.PI);
}
```

This is useful when module names are long or when two modules have the same name from different packages.

You can also bind specific items from a module without the prefix:

```zia
bind MathUtils { square, PI };
bind Zanna.Terminal;

func start() {
    Say(square(5.0));  // No prefix needed
    Say(PI);
}
```

This binds only `square` and `PI`, and you can use them without the module prefix. It's convenient when you use certain functions frequently, but be careful — if another module has a `square` function, you'll get a conflict.

### Choosing the Right Style

Here's a practical guide:

**Use full module binds** when:
- You use many items from the module
- Clarity is more important than brevity
- The module name provides useful context

**Use specific binds** when:
- You use only one or two items
- You use them very frequently
- The item names are clear without context

**Use aliases** when:
- Names are too long
- You need to resolve conflicts
- You want shorter code in a specific area

Real-world example:

```zia
// Good: Bind with alias when using many functions
bind Zanna.Math as Math;
bind Zanna.Terminal { Say };

func start() {
    var a = 0.5;
    var b = 1.0;
    var c = 2.0;
    var x = Math.Sqrt(a) + Math.Sin(b) + Math.Cos(c);
    Say("Result: " + x);
}
```

---

## Public and Private: Controlling Visibility

Not everything in a module should be visible to outsiders. Some functions are internal helpers. Some variables are implementation details. Exposing them would let other code depend on things you might want to change.

Top-level declarations are exported by default unless marked `hide`. Use
`expose` to mark public API intentionally. `export` and `public` are accepted
aliases, but `expose` is the canonical spelling used elsewhere in the
reference:

```zia
// file: Counter.zia
module Counter;

hide var count = 0;  // Private: only this module can see it

expose func increment() {
    count += 1;
}

expose func decrement() {
    count -= 1;
}

expose func get() -> Integer {
    return count;
}

hide func reset() {  // Private: internal use only
    count = 0;
}
```

The `hide` modifier makes `count` and `reset` internal implementation details.
Other modules can use `increment`, `decrement`, and `get`.

```zia
// file: Main.zia
module Main;

bind Counter;
bind Zanna.Terminal;

func start() {
    Counter.increment();
    Counter.increment();
    Say(Counter.get());  // 2

    // Counter.count = 100;  // Error: count is private
    // Counter.reset();      // Error: reset is private
}
```

### Why Hide Things?

This might seem restrictive. Why not let people access everything? There are several good reasons:

**Freedom to change.** If `count` is private, you can change how the counter works internally without breaking anyone's code. Maybe you want to change it to use a database instead of a variable. Maybe you want to add logging. If `count` was public, changing it would break every program that uses it.

**Preventing misuse.** The counter maintains a rule: the count should never go negative. If people could access `count` directly, they could set it to -100, violating that rule. By exposing only `increment`, `decrement`, and `get`, you guarantee the invariant holds.

**Simplifying understanding.** A module with 100 functions but only 5 exported is much easier to learn than one with 100 exported. Users only need to understand the public interface. They can ignore the 95 internal helpers.

**Documenting intent.** Public functions are promises: "This exists, it works, you can rely on it." Private functions are implementation: "This might change, don't depend on it." The visibility itself communicates intent.

This principle is called *encapsulation*: hiding internal details and exposing only what's needed. It's one of the most important ideas in software engineering.

### What Should Be Public?

Here's a guideline: **expose the minimum necessary for your module to be useful.**

Ask yourself: "If I were using this module, what would I need?" Usually, that's:
- Functions that perform the module's core purpose
- Types that users need to interact with
- Constants that have meaning outside the module

Things that should usually be private:
- Helper functions used only internally
- Internal state variables
- Implementation details that might change

```zia
module EmailValidator;
bind Zanna.String as Str;
bind Zanna.Collections.Seq as Seq;

// Public: This is what the module is for
expose func isValid(email: String) -> Boolean {
    if email.Length == 0 {
        return false;
    }
    if !containsAt(email) {
        return false;
    }
    if !hasValidDomain(email) {
        return false;
    }
    return true;
}

// Private helpers: Users don't need these
hide func containsAt(email: String) -> Boolean {
    return Str.Contains(email, "@");
}

hide func hasValidDomain(email: String) -> Boolean {
    var parts = Str.Split(email, "@");
    if Seq.get_Count(parts) != 2 {
        return false;
    }
    return Str.Contains(Seq.GetStr(parts, 1), ".");
}
```

Users of `EmailValidator` only need `isValid`. The helper functions are implementation details that could change without affecting anyone.

---

## Organizing Files

A typical project structure:

```text
my_project/
├── main.zia       # Entry point
├── player.zia     # Player module
├── enemy.zia      # Enemy module
├── graphics.zia   # Graphics module
└── utils/
    ├── math.zia   # Math utilities
    └── random.zia # Random utilities
```

Modules in subdirectories use dot notation:

```zia
bind utils.math;     // Bind utils/math.zia
bind utils.random;   // Bind utils/random.zia

func start() {
    var x = utils.math.square(5.0);
    var r = utils.random.range(1, 10);
}
```

The file system structure mirrors the module hierarchy. This makes it easy to find where code lives: if you're looking for `utils.math.square`, you know to look in `utils/math.zia`.

### Project Structure Patterns

There are several common ways to organize a project:

**By feature** (recommended for most projects):
```text
my_game/
├── main.zia
├── player/
│   ├── player.zia
│   ├── inventory.zia
│   └── abilities.zia
├── enemy/
│   ├── enemy.zia
│   ├── ai.zia
│   └── spawner.zia
├── world/
│   ├── map.zia
│   └── tiles.zia
└── shared/
    ├── physics.zia
    └── math.zia
```

Each feature gets its own directory. Related code lives together.

**By layer** (common in business applications):
```text
my_app/
├── main.zia
├── ui/
│   ├── forms.zia
│   └── views.zia
├── logic/
│   ├── validation.zia
│   └── processing.zia
└── data/
    ├── database.zia
    └── models.zia
```

Code is organized by its role in the system.

**Flat** (fine for small projects):
```text
my_tool/
├── main.zia
├── parser.zia
├── formatter.zia
└── utils.zia
```

When you have fewer than ten files, directories might be overkill.

Choose the structure that makes sense for your project. The goal is that anyone (including future you) can find code quickly.

---

## The Standard Library

Zanna comes with a rich standard library organized into namespaces:

```zia
bind Zanna.Terminal;   // Terminal I/O (Say, Print, ReadLine, etc.)
bind Zanna.IO;         // File operations
bind Zanna.Math;       // Mathematical functions
bind Zanna.Math.Random as Random;  // Random numbers
bind Zanna.Text.Fmt;        // Formatting
bind Zanna.Time;       // Date and time
bind Zanna.Graphics;   // 2D graphics (Canvas, Sprite, etc.)
```

When you bind a Zanna namespace, its functions become available without the full prefix:

```zia
bind Zanna.Terminal;
bind Zanna.Math.Random as Random;

func start() {
    Say("Guess a number!");           // No Zanna.Terminal. prefix needed
    var secret = Random.NextInt(100) + 1;    // No Zanna.Math.Random prefix needed
}
```

Without `bind`, you'd write `Zanna.Terminal.Say("...")` and `Zanna.Math.Random.NextInt(100)`. Both work, but `bind` makes code cleaner.

You can also bind with aliases or selectively import specific items:

```zia
bind Zanna.Terminal as T;              // Alias: T.Say("Hello")
bind Zanna.Math { Sqrt, Sin, Cos };    // Selective: Sqrt(x) works, Tan(x) doesn't
```

Understanding that these are namespaces helps you know where to look for functionality. Need to work with time? Check `Zanna.Time`. Need to format numbers? Check `Zanna.Text.Fmt`.

---

## Module Dependencies

Modules can bind other modules, creating a dependency graph:

```text
Main.zia
    └── binds game.zia
        ├── binds player.zia
        │   └── binds physics.zia
        └── binds enemy.zia
            └── binds physics.zia
```

Both `player` and `enemy` bind `physics`. That's fine — each gets access to the same physics code. The physics module is only loaded once; both modules share it.

This is normal and expected. When multiple modules need the same functionality, they should bind the same shared module rather than duplicating code.

### Understanding Dependencies

Dependencies flow in one direction. In the graph above:
- `main` depends on `game`
- `game` depends on `player` and `enemy`
- `player` and `enemy` depend on `physics`

This means:
- You can understand `physics` without knowing about `player` or `enemy`
- You can understand `player` knowing only about `physics`, not about `enemy`
- To understand `game`, you need to know about `player`, `enemy`, and `physics`
- To understand `main`, you need to know about everything

This is called a *dependency hierarchy*, and it's what makes large programs manageable. You can work on `physics` in isolation. You can test `player` with just `physics`. The boundaries let you focus.

---

## Circular Dependencies: The Tangled Web

There's one pattern you must avoid: circular dependencies. This happens when A binds B and B binds A.

```zia
// player.zia
module Player;
bind Enemy;  // Player needs to know about enemies

expose func attackNearestEnemy() {
    var nearest = Enemy.findNearest(position);
    // ...
}

// enemy.zia
module Enemy;
bind Player;  // Enemy needs to know about player

expose func chasePlayer() {
    var target = Player.getPosition();  // Needs player position
    // ...
}
```

This looks reasonable. Players need to know about enemies to attack them. Enemies need to know about the player to chase them. But it creates a circle: Player binds Enemy, Enemy binds Player.

### Why Circular Dependencies Are Bad

**Compilation problems.** The compiler needs to know about `Enemy` before it can compile `Player`, but it needs to know about `Player` before it can compile `Enemy`. Which comes first? Some languages refuse to compile circular dependencies at all.

**Reasoning problems.** If A depends on B and B depends on A, you can't understand either in isolation. To understand `Player`, you need to understand `Enemy`. To understand `Enemy`, you need to understand `Player`. There's no starting point.

**Change problems.** Any change to `Player` might affect `Enemy`, and any change to `Enemy` might affect `Player`. The modules are entangled; they can't evolve independently.

### Breaking Circular Dependencies

There are several strategies:

**Extract shared code into a third module:**

```zia
// position.zia — Shared types
module Position;

expose struct Vec2 {
    x: Number;
    y: Number;
}

// player.zia
module Player;
bind Position;

var position: Position.Vec2;

expose func getPosition() -> Position.Vec2 {
    return position;
}

// enemy.zia
module Enemy;
bind Position;
bind Player;  // Now only enemy binds player, not vice versa

expose func chasePlayer() {
    var target = Player.getPosition();
    // ...
}
```

By extracting `Vec2` into its own module, we removed one direction of the dependency.

**Use interfaces (covered in Part III):**

```zia
// target.zia
module Target;

expose interface ITarget {
    func getPosition() -> Vec2;
}

// enemy.zia
module Enemy;
bind Target;

expose func chase(target: Target.ITarget) {
    var pos = target.getPosition();
    // ...
}
```

The enemy doesn't need to know about players specifically — it only needs something it can chase.

**Rethink the design:**

Sometimes circular dependencies reveal a design problem. Maybe `Player` and `Enemy` shouldn't be separate modules. Maybe they should both extend a common `Entity` module. Step back and ask: "What's the best way to organize this?"

---

## Module Design Principles

Good module design follows several principles:

### One Concept Per Module

A module should do one thing well. A `Player` module handles player logic. An `Enemy` module handles enemies. A `Physics` module handles physics.

Don't create a module called `Utilities` that contains everything from string manipulation to file handling to math. That's not a module — that's a junk drawer.

How do you know if a module is doing too much? If you can't describe what it does in one sentence, it's probably doing too much. "This module handles player movement, inventory, abilities, and saving" describes four modules masquerading as one.

### Minimize Dependencies

Every bind creates a coupling. The more modules you bind, the more you depend on, and the harder changes become.

Ask yourself: "Does this module really need that binding?" Sometimes you bind a module for one function that you could easily write yourself. Sometimes you bind a module for a type that could be passed in instead.

```zia
// High dependency: binds many modules
module Report;
bind Database;
bind Formatter;
bind Email;
bind Logger;
bind Config;

// Lower dependency: receives what it needs
module Report;

expose func generate(data: List[Record], format: Formatter) -> String {
    // No database binding — data is passed in
    // No email binding — report is returned, caller sends it
    // ...
}
```

The second version is more flexible. It can be used with any data source, any formatter, by anyone who wants to send reports anywhere.

### Clear Interfaces

Your public functions are your module's contract with the world. Make them clear:

**Good names.** A function called `process` tells you nothing. `validateEmail`, `calculateTax`, `renderSprite` — these names explain themselves.

**Clear parameters.** `doThing(a, b, c, d, e)` is mysterious. `createUser(name, email, password)` is obvious.

**Documented behavior.** What does the function do? What does it return? What errors might it throw? Comments on public functions are valuable.

**Stable over time.** Once you export something, people depend on it. Don't change public interfaces casually. If you need to change them, provide a migration path.

### Hide What Can Change

Export only what you're confident about. Everything else should be private.

Think of your module as having two parts:
- The **interface**: What you promise to provide, which should be stable
- The **implementation**: How you provide it, which should be flexible

```zia
module Cache;

// Interface (stable promise)
expose func get(key: String) -> String?;
expose func set(key: String, value: String);
expose func clear();

// Implementation (hidden, can change)
var data: Map[String, CacheEntry];  // Could change to use Redis
var maxSize = 1000;                 // Could become configurable

func evictOldest() { ... }          // Internal helper
func shouldEvict() -> Boolean { ... }  // Internal helper
```

Users see only `get`, `set`, and `clear`. You're free to completely rewrite the implementation — use a different data structure, add expiration, move to a database — without breaking anyone's code.

---

## A Complete Example: Modular Game

Let's refactor our game demo into proper modules:

**Vec2.zia** — Vector math
```zia
// file: Vec2.zia
module Vec2;

bind Zanna.Math as Math;

expose struct Vec2 {
    expose Number x;
    expose Number y;

    expose func init(x: Number, y: Number) {
        self.x = x;
        self.y = y;
    }
}

expose func create(x: Number, y: Number) -> Vec2 {
    return new Vec2(x, y);
}

expose func add(a: Vec2, b: Vec2) -> Vec2 {
    return new Vec2(a.x + b.x, a.y + b.y);
}

expose func subtract(a: Vec2, b: Vec2) -> Vec2 {
    return new Vec2(a.x - b.x, a.y - b.y);
}

expose func scale(v: Vec2, factor: Number) -> Vec2 {
    return new Vec2(v.x * factor, v.y * factor);
}

expose func distance(a: Vec2, b: Vec2) -> Number {
    var dx = b.x - a.x;
    var dy = b.y - a.y;
    return Math.Sqrt(dx * dx + dy * dy);
}

expose func zero() -> Vec2 {
    return new Vec2(0.0, 0.0);
}

expose func magnitude(v: Vec2) -> Number {
    return Math.Sqrt(v.x * v.x + v.y * v.y);
}

expose func normalize(v: Vec2) -> Vec2 {
    var mag = magnitude(v);
    if mag == 0.0 {
        return zero();
    }
    return new Vec2(v.x / mag, v.y / mag);
}
```

**Player.zia** — Player module
```zia
// file: Player.zia
module Player;

bind Vec2;

expose struct Player {
    expose Integer kind;
    expose Vec2.Vec2 position;
    expose Integer health;
    expose Integer maxHealth;
    expose Integer score;

    expose func init(kind: Integer, position: Vec2.Vec2, health: Integer, maxHealth: Integer, score: Integer) {
        self.kind = kind;
        self.position = position;
        self.health = health;
        self.maxHealth = maxHealth;
        self.score = score;
    }
}

expose func create() -> Player {
    return new Player(0, Vec2.zero(), 100, 100, 0);
}

expose func displayName(player: Player) -> String {
    return "Hero";
}

expose func isAlive(player: Player) -> Boolean {
    return player.health > 0;
}

expose func move(player: Player, direction: Vec2.Vec2) -> Player {
    return new Player(player.kind, Vec2.add(player.position, direction), player.health, player.maxHealth, player.score);
}

expose func takeDamage(player: Player, amount: Integer) -> Player {
    var newHealth = player.health - amount;
    if newHealth < 0 {
        newHealth = 0;
    }
    return new Player(player.kind, player.position, newHealth, player.maxHealth, player.score);
}

expose func heal(player: Player, amount: Integer) -> Player {
    var newHealth = player.health + amount;
    if newHealth > player.maxHealth {
        newHealth = player.maxHealth;
    }
    return new Player(player.kind, player.position, newHealth, player.maxHealth, player.score);
}

expose func addScore(player: Player, points: Integer) -> Player {
    return new Player(player.kind, player.position, player.health, player.maxHealth, player.score + points);
}
```

**Enemy.zia** — Enemy module
```zia
// file: Enemy.zia
module Enemy;

bind Vec2;

expose struct Enemy {
    expose Vec2.Vec2 position;
    expose Integer damage;
    expose Number speed;

    expose func init(position: Vec2.Vec2, damage: Integer, speed: Number) {
        self.position = position;
        self.damage = damage;
        self.speed = speed;
    }
}

expose func create(x: Number, y: Number, damage: Integer) -> Enemy {
    return new Enemy(Vec2.create(x, y), damage, 1.0);
}

expose func moveToward(enemy: Enemy, target: Vec2.Vec2) -> Enemy {
    var direction = Vec2.subtract(target, enemy.position);
    var normalized = Vec2.normalize(direction);
    var movement = Vec2.scale(normalized, enemy.speed);

    return new Enemy(Vec2.add(enemy.position, movement), enemy.damage, enemy.speed);
}
```

**Main.zia** — Game entry point
```zia
// file: Main.zia
module Main;

bind Vec2;
bind Player;
bind Enemy;
bind Zanna.Terminal;

func start() {
    Say("=== Modular Game Demo ===");
    Say("");

    var player = Player.create();
    var enemy = Enemy.create(5.0, 3.0, 10);

    Say("Player: " + Player.displayName(player));
    Say("Health: " + player.health + "/" + player.maxHealth);
    Say("Position: (" + player.position.x + ", " + player.position.y + ")");
    Say("");

    // Move player
    var direction = Vec2.create(1.0, 0.5);
    player = Player.move(player, direction);
    Say("Player moved to: (" + player.position.x + ", " + player.position.y + ")");

    // Enemy chases player
    enemy = Enemy.moveToward(enemy, player.position);
    Say("Enemy moved to: (" + enemy.position.x + ", " + enemy.position.y + ")");

    // Check combat
    var dist = Vec2.distance(player.position, enemy.position);
    Say("Distance to enemy: " + dist);

    if dist < 5.0 {
        player = Player.takeDamage(player, enemy.damage);
        Say("Hit! Health: " + player.health);
    }

    if Player.isAlive(player) {
        player = Player.addScore(player, 100);
        Say("Player survives! Score: " + player.score);
    } else {
        Say("Game Over!");
    }
}
```

Now each concept lives in its own file. The dependency graph is clean:

```text
main.zia
├── binds Vec2
├── binds Player
│   └── binds Vec2
└── binds Enemy
    └── binds Vec2
```

You can work on player logic without touching enemy code. You can test `Vec2` in isolation. The main file is short and focused on game flow. If you want to add items, you create a new `item.zia` module — no existing code needs to change.

---

## Practical Organization Examples

Let's look at how modules would organize different kinds of projects:

### A Utility Library

```text
string_utils/
├── string_utils.zia      # Main module, re-exports everything
├── manipulation.zia      # Transformations: reverse, capitalize, etc.
├── validation.zia        # Checking: isEmail, isNumeric, etc.
├── parsing.zia           # Extraction: extractNumbers, splitWords, etc.
└── formatting.zia        # Output: padLeft, truncate, etc.
```

Users bind `string_utils` and get a clean, organized API:

```zia
bind string_utils;

bind Zanna.Terminal;

var email = "test@example.com";
if string_utils.validation.isEmail(email) {
    var domain = string_utils.parsing.extractDomain(email);
    Say("Domain: " + domain);
}
```

### A Web Application

```text
web_app/
├── main.zia
├── routes/
│   ├── home.zia
│   ├── users.zia
│   └── products.zia
├── services/
│   ├── auth.zia
│   ├── email.zia
│   └── payment.zia
├── models/
│   ├── user.zia
│   ├── product.zia
│   └── order.zia
├── database/
│   ├── connection.zia
│   └── queries.zia
└── utils/
    ├── validation.zia
    └── formatting.zia
```

Routes handle HTTP requests. Services contain business logic. Models define data structures. Database handles persistence. Utils provide common helpers.

This separation means:
- A designer working on routes doesn't need to understand the database
- A database change doesn't require touching route code
- Services can be tested without an HTTP server
- Models can be reused in different contexts

### A Game

```text
platformer/
├── main.zia
├── core/
│   ├── game.zia          # Main game loop
│   ├── input.zia         # Input handling
│   └── time.zia          # Delta time, timers
├── entities/
│   ├── player.zia
│   ├── enemy.zia
│   ├── item.zia
│   └── projectile.zia
├── systems/
│   ├── physics.zia       # Movement, collision
│   ├── combat.zia        # Damage, health
│   └── scoring.zia       # Points, achievements
├── rendering/
│   ├── sprites.zia
│   ├── camera.zia
│   └── effects.zia
├── levels/
│   ├── loader.zia
│   ├── level1.zia
│   └── level2.zia
└── shared/
    ├── vec2.zia
    └── rect.zia
```

This structure lets the team work in parallel:
- One person improves physics
- Another adds new enemies
- A third designs levels
- All without conflicts

---

## Testing and Modules

One of the biggest benefits of good module design is testability. When code is properly modularized, it's much easier to test.

### Why Modular Code Is Easier to Test

**Isolation.** You can test a module without loading the entire application. Testing `Vec2` doesn't require creating a player or starting a game loop.

```zia
// file: Vec2Test.zia
module Vec2Test;

bind Vec2;
bind Zanna.Terminal;

func require(condition: Boolean, message: String) {
    if !condition {
        throw message;
    }
}

func testAddCombinesVectors() {
    var a = Vec2.create(1.0, 2.0);
    var b = Vec2.create(3.0, 4.0);
    var result = Vec2.add(a, b);

    require(result.x == 4.0, "x should be 4");
    require(result.y == 6.0, "y should be 6");
}

func testDistanceCalculatesCorrectly() {
    var a = Vec2.create(0.0, 0.0);
    var b = Vec2.create(3.0, 4.0);

    require(Vec2.distance(a, b) == 5.0, "3-4-5 distance should be 5");
}

func start() {
    testAddCombinesVectors();
    testDistanceCalculatesCorrectly();
    Say("Vec2 tests passed");
}
```

**Substitution.** You can replace modules with test versions. Instead of testing with a real database, use a fake one.

```text
// In tests: use a mock
var mockDatabase = MockDatabase.create();
var userService = UserService.create(mockDatabase);

// The UserService doesn't know (or care) that it's using a mock
userService.createUser("alice");
require(mockDatabase.hasUser("alice"), "user should be saved");
```

**Focused tests.** Each module has a clear responsibility, so tests are focused. `Player` tests check player behavior. `Enemy` tests check enemy behavior. You know where to look for tests and what they cover.

**Fast tests.** Testing a small module is fast. Testing a monolithic application is slow. Fast tests get run more often, catching bugs earlier.

### Testing Private Helpers

Sometimes you want to test internal functions that aren't exported. There are several approaches:

**Option 1: Don't test them directly.** Test them through the public interface. If `validateEmail` calls `containsAt` internally, testing `validateEmail` tests `containsAt` indirectly.

**Option 2: Export with a "testing" convention.** Some teams export testing helpers with a prefix:

```zia
module EmailValidator;

bind Zanna.String as Str;

hide func containsAt(email: String) -> Boolean {
    return Str.Contains(email, "@");
}

// Exported for testing, not for normal use
expose func test_containsAt(email: String) -> Boolean {
    return containsAt(email);
}
```

**Option 3: Create a test module.** Put tests in the same file as the code they test. Tests can access private functions because they're in the same module:

```zia
module EmailValidator;

bind Zanna.String as Str;

func require(condition: Boolean, message: String) {
    if !condition {
        throw message;
    }
}

hide func containsAt(email: String) -> Boolean {
    return Str.Contains(email, "@");
}

expose func isValid(email: String) -> Boolean {
    // Uses containsAt internally
    return containsAt(email);
}

// Tests in the same module can access private functions
func testContainsAt() {
    require(containsAt("test@example.com"), "email should contain @");
    require(!containsAt("invalid"), "plain text should not contain @");
}

func start() {
    testContainsAt();
}
```

---

## The Two Languages

**Zia**
```zia
// Defining
module MyModule;
expose func hello() { ... }

// Binding
bind MyModule;
bind MyModule { hello };
bind OtherModule as O;
```

**BASIC**
```basic
' BASIC uses NAMESPACE and USING
NAMESPACE MyModule
    PUBLIC SUB Hello()
        ...
    END SUB
END NAMESPACE

' Using
USING MyModule
CALL Hello()
```

---

## Common Mistakes

### Exposing Too Much

```zia
// Bad: Everything is public
module User;

expose var users: List[User] = [];  // Internal data exposed
expose var nextId: Integer = 1;     // Implementation detail exposed

expose func createUser(name: String) -> User { ... }
expose func validateName(name: String) -> Boolean { ... }  // Internal helper exposed
expose func generateId() -> Integer { ... }  // Internal helper exposed
```

```zia
// Good: Minimal public interface
module User;

var users: List[User] = [];  // Private
var nextId: Integer = 1;     // Private

expose func createUser(name: String) -> User { ... }
expose func findUser(id: Integer) -> User? { ... }
expose func deleteUser(id: Integer) -> Boolean { ... }
```

### God Modules

```zia
// Bad: One module that does everything
module App;

expose func handleLogin() { ... }
expose func handleLogout() { ... }
expose func renderDashboard() { ... }
expose func sendEmail() { ... }
expose func processPayment() { ... }
expose func generateReport() { ... }
// ... 200 more functions
```

Split it up! Authentication, UI, email, payments, and reporting are different concepts.

### Circular Dependencies

```text
// Bad: A binds B, B binds A
// order.zia
bind Customer;  // Order needs Customer
// customer.zia
bind Order;     // Customer needs Order
```

```text
// Good: Extract shared concept
// types.zia
expose struct OrderSummary { ... }
expose struct CustomerInfo { ... }

// order.zia
bind types;
// Uses CustomerInfo, doesn't need full Customer module

// customer.zia
bind types;
// Uses OrderSummary, doesn't need full Order module
```

### Bind Pollution

```text
// Bad: Binding everything unqualified
bind MathUtils { * };  // Brings ALL names into scope
bind StringUtils { * };
bind FileUtils { * };

// Now you have hundreds of unqualified names
// Which module does "format" come from? Who knows!
```

```text
// Good: Bind modules, use qualified names
bind MathUtils;
bind StringUtils;
bind FileUtils;

var x = MathUtils.sqrt(2.0);
var s = StringUtils.format("{}", x);
FileUtils.writeAllText("out.txt", s);
```

### Wrong Abstraction Level

```zia
// Bad: Module is too granular
// add.zia
module Add;
expose func add(a: Integer, b: Integer) -> Integer { return a + b; }

// subtract.zia
module Subtract;
expose func subtract(a: Integer, b: Integer) -> Integer { return a - b; }
```

```zia
// Good: Module has coherent purpose
// math.zia
module Math;
expose func add(a: Integer, b: Integer) -> Integer { return a + b; }
expose func subtract(a: Integer, b: Integer) -> Integer { return a - b; }
expose func multiply(a: Integer, b: Integer) -> Integer { return a * b; }
expose func divide(a: Integer, b: Integer) -> Integer { return a / b; }
```

---

## Summary

- Modules organize code into separate, reusable files
- `module Name;` declares a module
- `bind ModuleName;` brings in another module
- `bind ModuleName { item };` binds specific items
- `bind ModuleName as Alias;` binds with renaming
- Top-level Zia declarations are exported by default unless marked `hide`;
  `expose` marks public API intentionally
- `export` and `public` are accepted aliases for `expose`
- Colliding top-level functions, globals, aliases, classes, structs,
  interfaces, and enums are scoped under their declaring module
- Members without a visibility modifier follow their type defaults
  (class fields private, struct fields public)
- The standard library is organized into modules
- Dependencies flow one direction — avoid circular bindings
- Good module design means:
  - One concept per module
  - Minimal public interfaces
  - Clear, stable contracts
  - No circular dependencies
- Modular code is easier to understand, change, test, and reuse

---

## Exercises

**Exercise 12.1**: Create a `StringUtils` module with functions `repeat(s, n)` (repeat string n times) and `reverse(s)` (reverse a string). Expose only these functions, keeping any helpers private. Use it from another file.

**Exercise 12.2**: Split the calculator from Chapter 10 into modules: one for parsing input, one for calculations, one for display. Make sure the dependencies flow in one direction (display binds calculations, calculations binds parsing, but not the reverse).

**Exercise 12.3**: Create a `Constants` module with mathematical and physical constants (PI, E, SPEED_OF_LIGHT, GRAVITY, etc.). Mark them all as `final` and expose them.

**Exercise 12.4**: Create a `Stack` module that provides a stack data structure with `push`, `pop`, `peek`, and `isEmpty` functions. Keep the internal array private. Users should only interact through the exposed functions.

**Exercise 12.5**: Create a simple logging module with functions `info(msg)`, `warn(msg)`, `error(msg)` that format messages differently. Include a private helper function for timestamp generation. Add a private variable for log level that can be changed via an exposed `setLevel` function.

**Exercise 12.6** (Challenge): Create a multi-module address book with separate modules for:
- `Contact` (contact data structure and operations)
- `Storage` (file I/O for saving/loading contacts)
- `UI` (terminal interaction for adding, viewing, searching contacts)
- `Validation` (checking email formats, phone numbers)

Draw the dependency graph. Make sure there are no circular dependencies. Think carefully about what each module exports.

---

*We can now organize large programs into manageable pieces. Our code is cleaner, our dependencies are clear, and our modules can be understood, tested, and reused independently. Next, we survey the entire standard library — what Zanna gives you for free.*

*[Continue to Chapter 13: The Standard Library ->](13-stdlib.md)*
