---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 16: Interfaces

Inheritance says "this thing *is* a specific type." But sometimes you care about what something *can do*, not what it *is*.

A dog can be drawn on screen. So can a spaceship. So can a health bar. They're completely different things, but they share an ability: they can be drawn. How do you write code that works with "anything drawable"?

*Interfaces* solve this. An interface defines a set of methods that classes must implement. It's a contract: "I promise I can do these things."

---

## What Is an Interface?

Before we dive into code, let's understand what interfaces really are at a conceptual level.

### The Concept of a Contract

In everyday life, contracts are everywhere. When you sign a lease, you're agreeing to pay rent on time, keep the property in good condition, and follow certain rules. The landlord doesn't care *how* you earn the money for rent. They don't care if you're a doctor, teacher, or freelancer. They only care that you fulfill the contract: pay rent on the first of each month.

An interface in programming works the same way. It's a contract that says: "If you claim to be this type of thing, you must be able to do these specific things." The interface doesn't care *how* you do them. It only cares *that* you can.

Think of it this way:
- **Inheritance** says what something *is*: "A Dog is an Animal"
- **Interfaces** say what something *can do*: "A Dog can be Drawn, can be Saved, can be Compared"

### Why Do We Need Interfaces?

Imagine you're building a game engine. You want to draw things on screen. Without interfaces, you might write code like this:

```zia
func drawEverything(dogs: List[Dog], spaceships: List[Spaceship], trees: List[Tree]) {
    for dog in dogs {
        dog.draw();
    }
    for ship in spaceships {
        ship.draw();
    }
    for tree in trees {
        tree.draw();
    }
}
```

This is terrible! Every time you add a new drawable type, you need to modify this function. What if you have 50 types of things that can be drawn? What about explosions, particles, UI elements, characters, items...?

What you really want to say is: "Give me anything that can be drawn, and I'll draw it." Interfaces let you say exactly that.

---

## Real-World Analogies

Interfaces are everywhere in the physical world. Understanding these analogies will help you think about when and how to use interfaces in code.

### The USB Port

A USB port is an interface. It defines a contract: "If you have the right shape and follow the USB protocol, you can plug in here." The computer doesn't care if you're plugging in a mouse, keyboard, flash drive, phone charger, or external hard drive. It only cares that you conform to the USB interface.

This is incredibly powerful. The same USB port can work with devices that didn't exist when the computer was manufactured. As long as new devices follow the USB contract, they work.

In code terms:
```zia
interface USBDevice {
    func connect();
    func transfer(data: bytes);
    func disconnect();
}
```

Any device that implements these three methods can work with any USB port.

### The Electrical Outlet

Electrical outlets are interfaces too. A standard wall outlet promises to provide 120V AC power (in the US). Your lamp doesn't know or care how the electricity is generated. It might come from a coal plant, nuclear reactor, solar farm, or wind turbine. The outlet is the interface. Anything that needs electricity can plug in, and anything that generates electricity can feed into the system.

```zia
interface PowerSource {
    func getVoltage() -> Number;
    func draw(watts: Number) -> Boolean;
}
```

### Job Requirements

When companies hire, job postings list requirements: "Must know Python, SQL, and have 3 years of experience." This is an interface! The company doesn't care where you learned Python or which bootcamp you attended. They care that you *can do* certain things.

```zia
interface SoftwareEngineer {
    func writeCode(language: String) -> String;
    func reviewCode(pr: PullRequest) -> List[Comment];
    func debugIssue(bug: Bug) -> Fix;
}
```

Anyone who can do these things is a SoftwareEngineer, regardless of their background.

### The Restaurant Menu

A restaurant menu is an interface between you and the kitchen. You don't need to know how the chef prepares each dish. You don't need to know their techniques, training, or equipment. You just need to know: "I can order these items, and I'll receive food." The menu is the contract.

---

## Defining Interfaces in Code

An interface declares methods without implementing them:

```zia
interface Drawable {
    func draw();
}
```

This says: "Anything that is Drawable must have a `draw()` method." It doesn't say *how* to draw. That's up to each class.

The interface contains only *method signatures*:
- The method name: `draw`
- The parameters: none in this case
- The return type: none (void)

There's no method body. No curly braces with code inside. Just the promise that this method will exist.

You can have multiple methods:

```zia
interface Saveable {
    func save(filename: String) -> Boolean;
    func load(filename: String) -> Boolean;
    func getLastSaved() -> timestamp;
}
```

And methods can have any signature:

```zia
interface Calculator {
    func add(a: Number, b: Number) -> Number;
    func subtract(a: Number, b: Number) -> Number;
    func multiply(a: Number, b: Number) -> Number;
    func divide(a: Number, b: Number) -> Number;
}
```

---

## Implementing Interfaces

When a class *implements* an interface, it's making a promise. It's saying: "I guarantee I can do everything this interface requires."

### What Does "Implement" Mean?

To implement an interface, a class must:
1. Declare that it implements the interface using the `implements` keyword
2. Provide working code for *every* method the interface declares
3. Match the exact signatures (names, parameters, return types)

```zia
bind Zanna.Terminal;

interface Drawable {
    func draw();
}

class Circle implements Drawable {
    expose Number radius;

    expose func draw() {
        Say("Drawing a circle with radius " + self.radius);
    }
}

class Rectangle implements Drawable {
    expose Number width;
    expose Number height;

    expose func draw() {
        Say("Drawing a rectangle " + self.width + "x" + self.height);
    }
}
```

Both classes are Drawable. They each provide their own implementation of `draw()`. The Circle draws itself as a circle. The Rectangle draws itself as a rectangle. But both fulfill the Drawable contract.

### The Compiler Enforces the Contract

If you claim to implement an interface but forget a method, the compiler will complain:

```zia
bind Zanna.Terminal;

interface Drawable {
    func draw();
    func getColor() -> String;
}

class Circle implements Drawable {  // Error: missing getColor()
    expose func draw() {
        Say("Drawing circle");
    }
    // Error: Circle claims to implement Drawable but doesn't implement getColor()!
}
```

This is a feature, not a bug! The compiler is protecting you. If code expects a Drawable and calls `getColor()`, it better work. The compiler ensures you keep your promises.

### Implementation Freedom

The interface only specifies *what* must be done, not *how*. This gives implementers complete freedom:

```text
interface DataStore {
    func save(key: String, value: String);
    func load(key: String) -> String;
}

// Store in memory
class MemoryStore implements DataStore {
    data: {String: String};

    expose func save(key: String, value: String) {
        self.data[key] = value;
    }

    expose func load(key: String) -> String {
        return self.data[key];
    }
}

// Store in a file
class FileStore implements DataStore {
    expose String basePath;

    expose func save(key: String, value: String) {
        var path = self.basePath + "/" + key + ".txt";
        File.WriteAllText(path, value);
    }

    expose func load(key: String) -> String {
        var path = self.basePath + "/" + key + ".txt";
        return File.ReadAllText(path);
    }
}

// Store in a database
class DatabaseStore implements DataStore {
    connection: DBConnection;

    expose func save(key: String, value: String) {
        self.connection.execute(
            "INSERT INTO store (key, value) VALUES (?, ?)",
            [key, value]
        );
    }

    expose func load(key: String) -> String {
        var result = self.connection.query(
            "SELECT value FROM store WHERE key = ?",
            [key]
        );
        return result[0]["value"];
    }
}
```

Three completely different implementations, one interface. Code that uses DataStore doesn't know or care which one it's working with.

---

## Using Interfaces

The power comes from treating different classes uniformly:

```zia
func renderScene(items: List[Drawable]) {
    for item in items {
        item.draw();
    }
}

var circle = new Circle(5.0);
var rect = new Rectangle(10.0, 3.0);

var scene: List[Drawable] = [circle, rect];
renderScene(scene);
// Drawing a circle with radius 5
// Drawing a rectangle 10x3
```

The `renderScene` function doesn't know or care about the specific types. It just knows they're Drawable, so it can call `draw()` on each one.

### Interface Types in Variables and Parameters

You can use an interface as a type anywhere you'd use a class type:

```zia
// As a variable type
var drawable: Drawable = new Circle(3.0);

// As a parameter type
func processDrawable(d: Drawable) {
    d.draw();
}

// As a return type
func createDrawable(kind: String) -> Drawable {
    if kind == "circle" {
        return new Circle(5.0);
    }
    return new Rectangle(10.0, 5.0);
}

// In collections
var items: List[Drawable] = [];
items.Push(new Circle(2.0));
items.Push(new Rectangle(4.0, 3.0));
```

When you use the interface type, you can only call methods that the interface defines. You can't call Circle-specific methods on a Drawable variable, even if the actual object is a Circle:

```zia
var drawable: Drawable = new Circle(5.0);
drawable.draw();  // OK: Drawable has draw()
drawable.radius;  // ERROR: Drawable doesn't define radius!
```

---

## Interface vs. Inheritance

This is a crucial distinction that often confuses beginners. Let's be crystal clear.

### Inheritance: The "Is-A" Relationship

**Inheritance** creates an "is-a" relationship and shares implementation:

```zia
class Dog extends Animal { ... }
// A dog IS an animal, inherits animal's code
```

When you extend a class:
- You get all the parent's fields automatically
- You get all the parent's method implementations
- You can only extend ONE class
- You're creating a tight coupling. Changes to the parent affect you.

### Interface: The "Can-Do" Relationship

**Interface** creates a "can-do" relationship and shares behavior contract:

```zia
class Dog implements Drawable { ... }
// A dog CAN BE drawn, must implement draw()
```

When you implement an interface:
- You get nothing for free. You must write all the code.
- You're promising certain capabilities
- You can implement MANY interfaces
- You're loosely coupled. The interface rarely changes.

### Key Differences Summarized

| Aspect | Inheritance | Interface |
|--------|-------------|-----------|
| Relationship | "is-a" | "can-do" |
| Code sharing | Yes, inherits implementation | No, only contract |
| How many | One parent only | Many interfaces |
| Coupling | Tight (shares internals) | Loose (only contract) |
| When to change | Often (implementation evolves) | Rarely (contracts are stable) |

### When to Use Each

**Use inheritance when:**
- There's a clear "is-a" relationship
- You want to share actual code between classes
- The classes are fundamentally the same kind of thing
- You need to reuse and extend parent behavior

**Use interfaces when:**
- Different things share a capability
- You want to define what something can do, not what it is
- You need maximum flexibility
- You're designing for extensibility

**Example decision:**

```zia
// These ARE accounts. They share implementation. Use inheritance.
class BankAccount { ... }
class SavingsAccount extends BankAccount { ... }
class CheckingAccount extends BankAccount { ... }

// These CAN BE printed. They're different things. Use interface.
interface Printable { func print(); }
class Document implements Printable { ... }
class Photo implements Printable { ... }
class Report implements Printable { ... }
```

---

## Multiple Interfaces

One of the most powerful aspects of interfaces is that a class can implement many of them. This is something inheritance can't do.

### Why Multiple Interfaces?

In the real world, things have multiple capabilities. A smartphone:
- Can make calls (Phone interface)
- Can take photos (Camera interface)
- Can play music (MediaPlayer interface)
- Can browse the web (WebBrowser interface)
- Can run apps (AppPlatform interface)

You can't express this with single inheritance. A smartphone isn't "a type of phone that happens to also be a camera." It's something that *can do* all these things.

### Implementing Multiple Interfaces

```zia
bind Zanna.Terminal;

interface Drawable {
    func draw();
}

interface Movable {
    func move(dx: Number, dy: Number);
}

interface Clickable {
    func onClick();
}

class Button implements Drawable, Movable, Clickable {
    expose Number x;
    expose Number y;
    expose String label;

    expose func draw() {
        Say("Drawing button: " + self.label);
    }

    expose func move(dx: Number, dy: Number) {
        self.x += dx;
        self.y += dy;
    }

    expose func onClick() {
        Say("Button clicked: " + self.label);
    }
}
```

The Button is Drawable AND Movable AND Clickable. It can be used anywhere any of those interfaces is expected:

```zia
func drawAll(items: List[Drawable]) { ... }
func moveAll(items: List[Movable]) { ... }
func handleClick(item: Clickable) { ... }

var btn = Button { x: 0.0, y: 0.0, label: "OK" };
drawAll([btn]);      // Works: Button is Drawable
moveAll([btn]);      // Works: Button is Movable
handleClick(btn);    // Works: Button is Clickable
```

### Flexible Combinations

Different classes can implement different combinations:

```zia
// A tree can be drawn but not moved or clicked
class Tree implements Drawable {
    expose func draw() { ... }
}

// A player can be drawn and moved, but not clicked
class Player implements Drawable, Movable {
    expose func draw() { ... }
    expose func move(dx: Number, dy: Number) { ... }
}

// An invisible trigger can be moved and clicked, but not drawn
class InvisibleTrigger implements Movable, Clickable {
    expose func move(dx: Number, dy: Number) { ... }
    expose func onClick() { ... }
}
```

Each class implements exactly what it needs. No more, no less.

---

## Interfaces with Inheritance

You can combine inheritance and interfaces. This is common and powerful.

```zia
bind Zanna.Terminal;

class Enemy {
    expose Integer health;

    expose func takeDamage(amount: Integer) {
        self.health -= amount;
    }
}

interface Drawable {
    func draw();
}

interface Attackable {
    func attack() -> Integer;
}

class Goblin extends Enemy implements Drawable, Attackable {
    expose func draw() {
        Say("Drawing a goblin");
    }

    expose func attack() -> Integer {
        return 5;
    }
}
```

The Goblin:
- **IS** an Enemy (inheritance). Gets `health` field and `takeDamage` method.
- **CAN BE** drawn (interface). Must implement `draw()`.
- **CAN** attack (interface). Must implement `attack()`.

### Order Matters

When combining, `extends` comes before `implements`:

```zia
// Correct
class Goblin extends Enemy implements Drawable, Attackable { ... }

// Wrong order (syntax error)
class Goblin implements Drawable extends Enemy { ... }
```

### A Richer Example

```zia
bind Zanna.Terminal;

class GameObject {
    expose Integer id;
    expose Number x;
    expose Number y;
    expose Boolean active;

    expose func getPosition() -> (Number, Number) {
        return (self.x, self.y);
    }

    expose func setPosition(x: Number, y: Number) {
        self.x = x;
        self.y = y;
    }
}

interface Renderable {
    func render(screen: Screen);
    func getLayer() -> Integer;
}

interface Collidable {
    func getBounds() -> Rectangle;
    func onCollision(other: Collidable);
}

interface Updatable {
    func update(deltaTime: Number);
}

// A player has everything
class Player extends GameObject implements Renderable, Collidable, Updatable {
    sprite: Sprite;
    velocity: Vector2;
    hitbox: Rectangle;

    expose func render(screen: Screen) {
        screen.draw(self.sprite, self.x, self.y);
    }

    expose func getLayer() -> Integer {
        return 5;  // Player renders on layer 5
    }

    expose func getBounds() -> Rectangle {
        return self.hitbox;
    }

    expose func onCollision(other: Collidable) {
        Say("Player hit something!");
    }

    expose func update(deltaTime: Number) {
        self.x += self.velocity.x * deltaTime;
        self.y += self.velocity.y * deltaTime;
    }
}

// A background image just renders, nothing else
class Background extends GameObject implements Renderable {
    image: Image;

    expose func render(screen: Screen) {
        screen.draw(self.image, 0, 0);
    }

    expose func getLayer() -> Integer {
        return 0;  // Background is layer 0 (behind everything)
    }
}
```

---

## Dependency Injection: Programming to Interfaces

This is where interfaces truly shine. *Dependency injection* is a fancy term for a simple idea: don't create your dependencies inside your code; receive them from outside.

### The Problem: Hardcoded Dependencies

Consider a UserService that needs to store user data:

```zia
class UserService {
    expose func saveUser(user: User) {
        var db = MySQLDatabase();  // Hardcoded!
        db.save("users", user);
    }
}
```

This is problematic:
- Can't use a different database without changing this code
- Can't test without a real MySQL database
- UserService and MySQLDatabase are tightly coupled

### The Solution: Depend on Interfaces

```zia
interface Database {
    func save(table: String, data: any);
    func load(table: String, id: Integer) -> any;
}

class UserService {
    db: Database;  // Depends on interface, not implementation

    expose func init(database: Database) {
        self.db = database;  // Receive dependency from outside
    }

    expose func saveUser(user: User) {
        self.db.save("users", user);
    }

    expose func getUser(id: Integer) -> User {
        return self.db.load("users", id);
    }
}
```

Now you can use UserService with any database:

```zia
// In production
var mysqlDb = MySQLDatabase();
var userService = UserService(mysqlDb);

// In development
var sqliteDb = SQLiteDatabase();
var userService = UserService(sqliteDb);

// In tests
var fakeDb = FakeDatabase();
var userService = UserService(fakeDb);
```

### "Program to an Interface, Not an Implementation"

This is one of the most important principles in software design. It means:

1. **Define interfaces** for the capabilities you need
2. **Write code that uses interfaces**, not concrete types
3. **Inject implementations** at runtime

The benefit? Your code becomes:
- **Flexible**: Swap implementations without changing code
- **Testable**: Use test doubles easily
- **Maintainable**: Changes to one implementation don't ripple through

### A Practical Example

```zia
interface Logger {
    func log(message: String);
    func error(message: String);
}

interface PaymentProcessor {
    func charge(amount: Number, cardNumber: String) -> Boolean;
    func refund(transactionId: String) -> Boolean;
}

interface EmailSender {
    func send(to: String, subject: String, body: String);
}

class OrderService {
    logger: Logger;
    payments: PaymentProcessor;
    email: EmailSender;

    expose func init(logger: Logger, payments: PaymentProcessor, email: EmailSender) {
        self.logger = logger;
        self.payments = payments;
        self.email = email;
    }

    expose func processOrder(order: Order) {
        self.logger.log("Processing order " + order.id);

        var success = self.payments.charge(order.total, order.cardNumber);

        if success {
            self.logger.log("Payment successful");
            self.email.send(order.customerEmail, "Order Confirmed", "Thank you!");
        } else {
            self.logger.error("Payment failed for order " + order.id);
        }
    }
}
```

OrderService doesn't know:
- Whether logs go to console, file, or a logging service
- Whether payments go through Stripe, PayPal, or a test processor
- Whether emails are sent via SMTP, SendGrid, or just printed in tests

It just knows these capabilities exist. The actual implementations are provided when the service is created.

---

## Testing Benefits

Interfaces make testing dramatically easier. This alone justifies their use in serious software.

### The Testing Problem

Imagine testing code that sends real emails, charges real credit cards, or talks to real databases. You'd need:
- Actual payment processor accounts
- Real email servers
- Running databases
- And tests would be slow, flaky, and expensive!

### Mocking: Fake Implementations for Tests

With interfaces, you can create *test doubles* (also called mocks or fakes):

The following sketches are conceptual pseudocode. Current Zia does not ship a built-in `assert` statement or `test` block syntax.

```text
interface EmailSender {
    func send(to: String, subject: String, body: String);
}

// Real implementation for production
class SmtpEmailSender implements EmailSender {
    expose func send(to: String, subject: String, body: String) {
        // Actually send email via the chosen SMTP provider
        sendViaSmtp(to, subject, body);
    }
}

// Fake implementation for testing
class FakeEmailSender implements EmailSender {
    expose List[Email] sentEmails;

    expose func init() {
        self.sentEmails = [];
    }

    expose func send(to: String, subject: String, body: String) {
        // Don't actually send. Just record that we would have.
        self.sentEmails.Push(Email { to: to, subject: subject, body: body });
    }

    // Helper method for tests
    expose func getSentEmails() -> List[Email] {
        return self.sentEmails;
    }
}
```

Now you can test email-sending code without sending real emails:

```text
func testOrderConfirmationEmail() {
    var fakeEmail = FakeEmailSender();
    var fakePayments = FakePaymentProcessor();  // Always succeeds
    var fakeLogger = FakeLogger();

    var orderService = OrderService(fakeLogger, fakePayments, fakeEmail);

    var order = Order {
        id: "123",
        total: 99.99,
        customerEmail: "customer@example.com"
    };

    orderService.processOrder(order);

    // Verify the right email was "sent"
    var emails = fakeEmail.getSentEmails();
    assert(emails.Length == 1);
    assert(emails[0].to == "customer@example.com");
    assert(emails[0].subject == "Order Confirmed");
}
```

### Testing Edge Cases

Fakes let you simulate scenarios that are hard to create with real systems:

```text
class AlwaysFailingPaymentProcessor implements PaymentProcessor {
    expose func charge(amount: Number, cardNumber: String) -> Boolean {
        return false;  // Simulate payment failure
    }

    expose func refund(transactionId: String) -> Boolean {
        return false;
    }
}

func testHandlesPaymentFailure() {
    var fakeEmail = FakeEmailSender();
    var failingPayments = AlwaysFailingPaymentProcessor();
    var fakeLogger = FakeLogger();

    var orderService = OrderService(fakeLogger, failingPayments, fakeEmail);

    orderService.processOrder(someOrder);

    // Should not send confirmation email when payment fails
    assert(fakeEmail.getSentEmails().Length == 0);

    // Should log the error
    assert(fakeLogger.hasError("Payment failed"));
}
```

### Substituting Implementations

Beyond testing, interfaces let you swap implementations for other reasons:

```zia
// Development: fast local storage
var store = InMemoryStore();

// Staging: local file storage
var store = FileStore("/tmp/data");

// Production: distributed database
var store = CassandraStore(clusterConfig);

// All work identically because they implement the same interface
var userService = UserService(store);
```

---

## Interface Design Principles

Designing good interfaces is a skill. Here are principles to guide you.

### Keep Interfaces Small

Prefer small, focused interfaces over large ones. This is called the *Interface Segregation Principle*.

**Bad: One massive interface**
```zia
interface GameEntity {
    func draw();
    func move(dx: Number, dy: Number);
    func attack() -> Integer;
    func takeDamage(amount: Integer);
    func save();
    func load();
    func playSound(sound: String);
    func getTooltip() -> String;
}
```

Problems:
- A tree needs to implement `attack()` even though trees don't attack
- A sound effect needs to implement `draw()` even though it's invisible
- Everyone implements everything, even what doesn't apply

**Good: Small focused interfaces**
```zia
interface Drawable {
    func draw();
}

interface Movable {
    func move(dx: Number, dy: Number);
}

interface Combatant {
    func attack() -> Integer;
    func takeDamage(amount: Integer);
}

interface Saveable {
    func save();
    func load();
}

interface Audible {
    func playSound(sound: String);
}
```

Benefits:
- A tree implements only Drawable
- A player implements Drawable, Movable, Combatant, Saveable
- A sound effect implements only Audible
- Entities implement only what they need

### The Newspaper Analogy

Think of small interfaces like sections of a newspaper. The sports fan reads the sports section. The investor reads the business section. Nobody is forced to read the entire paper to get what they want.

Large interfaces force everyone to deal with everything. Small interfaces let each consumer focus on what they care about.

### Name Interfaces for Capabilities

Interface names should describe *what something can do*, often using adjectives or "-able" suffixes:

- `Drawable` - can be drawn
- `Serializable` - can be serialized
- `Comparable` - can be compared
- `Iterable` - can be iterated
- `Clickable` - can be clicked
- `Runnable` - can be run

Or use nouns that describe a role:
- `Logger` - something that logs
- `Reader` - something that reads
- `Writer` - something that writes
- `Handler` - something that handles events

### Design for Clients, Not Implementers

When designing an interface, think about the code that will *use* it, not the code that will *implement* it.

**Bad: Designed around implementation details**
```zia
interface UserRepository {
    func openConnection();
    func executeQuery(sql: String) -> ResultSet;
    func closeConnection();
}
```

This interface leaks implementation details. What if the implementation isn't SQL-based?

**Good: Designed around what clients need**
```zia
interface UserRepository {
    func getUser(id: Integer) -> User;
    func saveUser(user: User);
    func deleteUser(id: Integer);
    func findUsersByName(name: String) -> List[User];
}
```

Now clients say what they want, not how to get it.

### Avoid Changing Interfaces

Once an interface is published and used, changing it breaks all implementations. This is why interfaces should be:
- Thoughtfully designed upfront
- Small (less to get wrong)
- Stable (change rarely)

If you need to add capabilities, consider creating a new interface:

```zia
// Original interface
interface Drawable {
    func draw();
}

// Later, need animation support.
// Don't add to Drawable! Create a second interface for the combined contract.
interface AnimatableDrawable {
    func draw();
    func animate(frame: Integer);
}
```

Existing code still uses Drawable. New code can use AnimatableDrawable when it
needs both drawing and animation.

---

## A Complete Example: Plugin System

Interfaces are perfect for plugin systems where you don't know what implementations will exist:

```zia
module PluginSystem;

bind Zanna.Terminal;
bind Zanna.String as Str;

// Interface that all plugins must implement
interface Plugin {
    func getName() -> String;
    func execute(input: String) -> String;
}

// Some plugins
class UppercasePlugin implements Plugin {
    expose func getName() -> String {
        return "Uppercase";
    }

    expose func execute(input: String) -> String {
        return Str.ToUpper(input);
    }
}

class ReversePlugin implements Plugin {
    expose func getName() -> String {
        return "Reverse";
    }

    expose func execute(input: String) -> String {
        return Str.Reverse(input);
    }
}

class RepeatPlugin implements Plugin {
    expose Integer times;

    expose func init(times: Integer) {
        self.times = times;
    }

    expose func getName() -> String {
        return "Repeat x" + self.times;
    }

    expose func execute(input: String) -> String {
        return Str.Repeat(input, self.times);
    }
}

// Plugin manager doesn't need to know about specific plugins
class PluginManager {
    expose List[Plugin] plugins;

    expose func init() {
        self.plugins = [];
    }

    expose func register(plugin: Plugin) {
        self.plugins.Push(plugin);
        Say("Registered: " + plugin.getName());
    }

    expose func process(input: String) -> String {
        var result = input;
        for plugin in self.plugins {
            result = plugin.execute(result);
        }
        return result;
    }

    expose func listPlugins() {
        Say("Installed plugins:");
        for plugin in self.plugins {
            Say("  - " + plugin.getName());
        }
    }
}

func start() {
    var manager = new PluginManager();

    // Register some plugins
    manager.register(new UppercasePlugin());
    manager.register(new ReversePlugin());
    manager.register(new RepeatPlugin(2));

    manager.listPlugins();

    // Process text through all plugins
    var input = "hello";
    var output = manager.process(input);

    Say("Input: " + input);
    Say("Output: " + output);
    // Output: OLLEHOLLEH (uppercased, reversed, repeated twice)
}
```

The PluginManager works with any Plugin without knowing the specific types. You could add new plugins without modifying the manager. Someone could write plugins in a separate codebase, and they'd still work.

This is the power of interfaces: *designing for extension without modification*.

---

## The Two Languages

**Zia**
```zia
bind Zanna.Terminal;

interface Printable {
    func print();
}

class Document implements Printable {
    expose func print() {
        Say("Printing document");
    }
}
```

**BASIC**
```text
INTERFACE Printable
    SUB Print()
END INTERFACE

CLASS Document IMPLEMENTS Printable
    SUB Print()
        PRINT "Printing document"
    END SUB
END CLASS
```

---

## Common Patterns

### Strategy Pattern

The strategy pattern lets you swap algorithms at runtime:

```zia
interface SortStrategy {
    func sort(items: List[Integer]) -> List[Integer];
}

class QuickSort implements SortStrategy {
    expose func sort(items: List[Integer]) -> List[Integer] {
        // Quick sort implementation
        ...
    }
}

class MergeSort implements SortStrategy {
    expose func sort(items: List[Integer]) -> List[Integer] {
        // Merge sort implementation
        ...
    }
}

class BubbleSort implements SortStrategy {
    expose func sort(items: List[Integer]) -> List[Integer] {
        // Bubble sort implementation (slow but simple)
        ...
    }
}

// Use any sorting strategy
func processData(data: List[Integer], strategy: SortStrategy) {
    var sorted = strategy.Sort(data);
    ...
}

// Choose strategy based on data size
var data = getNumbers();
if data.Length < 10 {
    processData(data, BubbleSort());  // Simple for small data
} else {
    processData(data, QuickSort());   // Fast for large data
}
```

### Observer Pattern

The observer pattern notifies interested parties when something happens:

```zia
bind Zanna.Terminal;

interface Observer {
    func onEvent(event: String);
}

class Subject {
    expose List[Observer] observers;

    expose func init() {
        self.observers = [];
    }

    expose func subscribe(obs: Observer) {
        self.observers.Push(obs);
    }

    expose func unsubscribe(obs: Observer) {
        self.observers.Remove(obs);
    }

    expose func notify(event: String) {
        for obs in self.observers {
            obs.onEvent(event);
        }
    }
}

// Different observers respond differently
class LoggingObserver implements Observer {
    expose func onEvent(event: String) {
        Say("LOG: " + event);
    }
}

class EmailObserver implements Observer {
    expose String adminEmail;

    expose func init(adminEmail: String) {
        self.adminEmail = adminEmail;
    }

    expose func onEvent(event: String) {
        Say("EMAIL to " + self.adminEmail + ": " + event);
    }
}

// Usage
var subject = new Subject();
subject.subscribe(new LoggingObserver());
subject.subscribe(new EmailObserver("admin@example.com"));

subject.notify("User signed up");  // Both observers notified
```

### Repository Pattern

The repository pattern abstracts data access:

```zia
interface UserRepository {
    func findById(id: Integer) -> User;
    func findByEmail(email: String) -> User;
    func save(user: User);
    func delete(id: Integer);
    func findAll() -> List[User];
}

// In-memory implementation for development/testing
class InMemoryUserRepository implements UserRepository {
    users: {Integer: User};
    expose Integer nextId;

    expose func findById(id: Integer) -> User {
        return self.users[id];
    }

    expose func findByEmail(email: String) -> User {
        for user in self.users.Values() {
            if user.email == email {
                return user;
            }
        }
        return null;
    }

    expose func save(user: User) {
        if user.id == 0 {
            user.id = self.nextId;
            self.nextId += 1;
        }
        self.users[user.id] = user;
    }

    expose func delete(id: Integer) {
        self.users.Remove(id);
    }

    expose func findAll() -> List[User] {
        return self.users.Values();
    }
}

// PostgreSQL implementation for production
class PostgresUserRepository implements UserRepository {
    connection: PGConnection;

    expose func findById(id: Integer) -> User {
        var row = self.connection.queryOne(
            "SELECT * FROM users WHERE id = ?", [id]
        );
        return self.rowToUser(row);
    }

    // ... other methods
}
```

---

## When to Use Interfaces

**Use interfaces when:**
- Different classes need to be treated uniformly (all Drawables can be drawn)
- You're designing a plugin or extension system
- You want to define contracts without dictating implementation
- You need multiple inheritance of behavior (one class, many capabilities)
- You want to enable testing with mocks/fakes
- You're building for flexibility and future extension
- You want loose coupling between parts of your system

**Use inheritance when:**
- Entities share actual implementation code
- There's a clear "is-a" relationship
- You want to reuse and extend parent code
- The classes are fundamentally the same kind of thing

Often you'll use both together: inheritance for shared code, interfaces for shared contracts. A `Goblin extends Enemy implements Drawable, Attackable` gets code from Enemy and implements the Drawable and Attackable contracts.

---

## Summary

- *Interfaces* define method contracts without implementation
- Think of interfaces as contracts or promises: "I can do these things"
- Entities *implement* interfaces by providing the methods
- A class can implement multiple interfaces (unlike single inheritance)
- Interfaces enable polymorphism: treating different classes uniformly
- **Program to interfaces, not implementations** for flexible, testable code
- Use small, focused interfaces (Interface Segregation Principle)
- Interfaces make testing easy through mocking and fakes
- Interfaces are great for plugin systems and loose coupling
- Combine with inheritance as needed

Interfaces are one of the most powerful tools for writing maintainable software. They let you:
- Change implementations without changing the code that uses them
- Test code in isolation
- Design for extension without modification
- Write flexible, loosely-coupled systems

Master interfaces, and you'll write code that adapts to change instead of fighting it.

---

## Exercises

**Exercise 16.1**: Create a `Comparable` interface with a `compareTo(other) -> Integer` method. Implement it for a `Person` class (compare by age). Write a function that finds the oldest person in a list.

**Exercise 16.2**: Create `Readable` and `Writable` interfaces. Create a `File` class that implements both. Create a `ReadOnlyFile` that implements only `Readable`.

**Exercise 16.3**: Create a simple command pattern: `Command` interface with `execute()` and `undo()`. Create `AddCommand`, `DeleteCommand`, `MoveCommand` classes. Build a simple undo system.

**Exercise 16.4**: Create a `Serializable` interface with `toJson() -> String` and `fromJson(s: String)`. Implement for a simple data class. Think about how you'd test this.

**Exercise 16.5**: Create a filter system: `Filter` interface with `matches(item) -> Boolean`. Create `AgeFilter`, `NameFilter`, `ActiveFilter`. Write code that applies multiple filters to a list of users.

**Exercise 16.6**: Create a `Logger` interface. Implement `ConsoleLogger`, `FileLogger`, and `NullLogger` (does nothing). Use dependency injection in a service that logs.

**Exercise 16.7**: Create `PaymentProcessor` interface. Implement `StripeProcessor` (real) and `FakeProcessor` (for tests). Write tests for a checkout service using the fake.

**Exercise 16.8** (Challenge): Build a complete event system: `EventListener` interface, `EventDispatcher` class that manages listeners and dispatches events. Support multiple listeners for the same event type.

**Exercise 16.9** (Challenge): Design interfaces for a document editor that supports multiple file formats. Think about: `Loadable`, `Saveable`, `Renderable`, `Editable`. How would you handle format conversion?

---

*We've seen interfaces as contracts. Next, we put it all together with polymorphism. Writing code that works with many types through common interfaces.*

*[Continue to Chapter 17: Polymorphism ->](17-polymorphism.md)*
