---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 17: Polymorphism

You've seen pieces of this already: calling `draw()` on different shapes, processing different plugins through the same interface, treating Goblins and Orcs as Enemies. Now we give this powerful concept its proper name and explore it fully.

*Polymorphism* comes from Greek: *poly* meaning "many" and *morph* meaning "form." In programming, it means "many forms" — the ability for one piece of code to work with many different types. You write code that operates on a general interface, and at runtime, the specific type determines exactly what happens.

This is one of the most powerful ideas in programming. Once you truly understand polymorphism, you'll see it everywhere — and you'll wonder how you ever wrote code without it.

---

## The Everyday Polymorphism Around You

Before we dive into code, let's build intuition with everyday examples. You already understand polymorphism — you just didn't know it had a name.

### The Remote Control Analogy

Think about the "Play" button. You press Play on your TV remote, and the TV starts playing. You press Play on your music player, and music starts. You press Play on your video game, and the game resumes. Same action, same button, same concept — but completely different behaviors depending on what device receives the command.

The Play button is an *interface*. Every device that supports Play must respond to it, but each device responds in its own way. The TV shows video, the music player produces audio, the game renders graphics. You don't need a separate "Play TV" button, "Play Music" button, and "Play Game" button. One button, many forms of playing.

That's polymorphism.

### The Vehicle Controls Analogy

Consider a steering wheel. You turn it left, the vehicle goes left. Turn it right, the vehicle goes right. This works whether you're driving a car, a truck, a bus, or a forklift. The specific *mechanism* is different — a car has rack-and-pinion steering, a truck might have hydraulic assistance, a forklift steers its rear wheels — but the *interface* is the same: turn the wheel, change direction.

Now imagine you're writing software to control vehicles. Would you write separate code for each vehicle type?

```zia
// Without polymorphism - repetitive and fragile
if vehicleType == "car" {
    car.turnCarSteeringWheel(angle);
} else if vehicleType == "truck" {
    truck.turnTruckSteeringMechanism(angle);
} else if vehicleType == "forklift" {
    forklift.adjustForkliftDirection(angle);
}
```

Or would you write code that works with any steerable thing?

```zia
// With polymorphism - elegant and extensible
vehicle.steer(angle);
```

The second approach is polymorphism. One piece of code, many types of vehicles, each implementing `steer` in its own way.

### The Payment Analogy

When you buy something, the cashier says "How would you like to pay?" You might use cash, credit card, debit card, mobile payment, or gift card. The cashier doesn't need different training for each payment method. They just need to know: "Can this payment method complete the transaction?"

```zia
bind Zanna.Terminal;

interface PaymentMethod {
    func pay(amount: Number) -> Boolean;
}

class CashPayment implements PaymentMethod {
    expose func pay(amount: Number) -> Boolean {
        // Count bills, make change
        return amount >= 0.0;
    }
}

class CreditCard implements PaymentMethod {
    expose func pay(amount: Number) -> Boolean {
        // Contact bank, authorize transaction
        return amount >= 0.0;
    }
}

class MobilePayment implements PaymentMethod {
    expose func pay(amount: Number) -> Boolean {
        // Communicate with phone, verify fingerprint
        return amount >= 0.0;
    }
}
```

The cashier's checkout code doesn't care which specific payment method you use:

```zia
bind Zanna.Terminal;

func checkout(items: List[Item], payment: PaymentMethod) {
    var total = calculateTotal(items);
    if payment.pay(total) {
        Say("Transaction complete!");
    }
}
```

Add a new payment method (cryptocurrency, airline miles, whatever) and the checkout code doesn't change. That's the power of polymorphism.

---

## Why Polymorphism Matters

Polymorphism isn't just an academic concept — it solves real problems that you'll encounter in every significant program.

### Problem 1: The Endless If-Else Chain

Without polymorphism, your code becomes a cascade of type checks:

```text
// Without polymorphism - this gets ugly fast
func processEntity(class: Any, type: String) {
    if type == "player" {
        class.movePlayer();
        class.drawPlayer();
        class.handlePlayerInput();
    } else if type == "enemy" {
        class.moveEnemy();
        class.drawEnemy();
        class.runEnemyAI();
    } else if type == "projectile" {
        class.moveProjectile();
        class.drawProjectile();
        class.checkProjectileCollision();
    } else if type == "particle" {
        class.moveParticle();
        class.drawParticle();
        class.updateParticleLife();
    }
    // This goes on forever...
}
```

Every new class type requires modifying this function. Miss one place? Bug. Forget to add a case? Bug. Want to reorganize? Touch dozens of files.

With polymorphism:

```zia
// With polymorphism - clean and maintainable
func processEntity(entity: GameEntity) {
    entity.update();
    entity.draw();
}
```

The `GameEntity` interface (or base class) guarantees that every class knows how to update and draw itself. New class types just implement the interface — no modification to existing code.

### Problem 2: Adding New Types Should Be Easy

Imagine a graphics program with shapes. Version 1 supports circles and rectangles. Version 2 adds triangles. Version 3 adds polygons. Version 4 adds curves.

Without polymorphism, every feature that works with shapes must be updated for every new shape. Drawing code, selection code, resizing code, export code — all need new cases added.

With polymorphism, you add a new shape class that implements the `Shape` interface. Existing code that works with shapes automatically works with your new shape. No modifications needed.

This is called the *Open-Closed Principle*: code should be open for extension but closed for modification. Polymorphism makes this possible.

### Problem 3: Testing and Flexibility

Want to test your payment processing without actually charging credit cards? With polymorphism, create a `MockPaymentMethod` that always succeeds:

```text
bind Zanna.Terminal;

class MockPayment implements PaymentMethod {
    expose func pay(amount: Number) -> Boolean {
        Say("[Test] Would charge: " + amount);
        return true;  // Always succeeds for testing
    }
}

// In tests:
var testPayment = MockPayment();
checkout(items, testPayment);  // No real money charged
```

This flexibility — swapping implementations without changing calling code — is one of the most practical benefits of polymorphism.

---

## How Polymorphism Works: Runtime and Compile-Time

There are two flavors of polymorphism, and understanding both will deepen your mastery.

### Runtime Polymorphism (Dynamic Dispatch)

This is what we've been discussing: the decision about which method to call happens *at runtime*, based on the actual object type.

```zia
bind Zanna.Terminal;

class Animal {
    expose func speak() {
        Say("...");
    }
}

class Dog extends Animal {
    expose func speak() {
        Say("Woof!");
    }
}

class Cat extends Animal {
    expose func speak() {
        Say("Meow!");
    }
}

class Cow extends Animal {
    expose func speak() {
        Say("Moo!");
    }
}

// The key insight: variable type vs actual type
var animal: Animal = new Dog();  // Variable type is Animal, actual object is Dog
animal.speak();              // Output: "Woof!" - Dog's method is called!
```

Look carefully at that last example. The variable `animal` is declared as type `Animal`, but it holds a `Dog` object. When we call `speak()`, which version runs?

The answer: **Dog's version**. The actual object type determines the method called, not the variable type.

This is called *dynamic dispatch* or *late binding*. The decision is made at runtime because the compiler can't always know what type an object will be. Consider:

```zia
bind Zanna.Math.Random as Random;

func makeRandomAnimal() -> Animal {
    var r = Random.Range(0, 2);
    if r == 0 {
        return new Dog();
    } else if r == 1 {
        return new Cat();
    } else {
        return new Cow();
    }
}

var mystery: Animal = makeRandomAnimal();
mystery.speak();  // Which sound? We don't know until runtime!
```

The compiler has no idea which animal will be created — that depends on a random number at runtime. But polymorphism ensures the right `speak()` method is called regardless.

#### How Dynamic Dispatch Works (Mental Model)

Imagine each object carries a hidden table — a lookup of its methods. When you call `mystery.speak()`, the system:

1. Looks at the actual object (not the variable type)
2. Finds that object's method table
3. Looks up `speak` in that table
4. Calls the found method

This happens automatically. You just write `object.method()` and the right implementation runs.

### Compile-Time Polymorphism (Method Overloading)

The other form of polymorphism happens at compile time: *method overloading*.

```zia
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

class Printer {
    expose func print(text: String) {
        Say(text);
    }

    expose func print(number: Integer) {
        Say(Fmt.Int(number));
    }

    expose func print(number: Number) {
        Say(Fmt.Num(number));
    }

    expose func print(items: List[String]) {
        for item in items {
            Say(item);
        }
    }

    expose func print(item: String, times: Integer) {
        for i in 0..times {
            Say(item);
        }
    }
}
```

Same method name, different parameter types. The compiler chooses the right version based on the arguments:

```zia
var p = new Printer();
p.Print("hello");           // Calls print(text: String)
p.Print(42);                // Calls print(number: Integer)
p.Print(3.14);              // Calls print(number: Number)
p.Print(["a", "b", "c"]);   // Calls print(items: List[String])
p.Print("hi", 3);           // Calls print(item: String, times: Integer)
```

This is *static polymorphism* or *early binding* — the decision is made at compile time based on the types of arguments you provide.

### Generics Preview: Parametric Polymorphism

There's a third kind of polymorphism you'll encounter later: generics (parametric polymorphism). This lets you write code that works with any type:

```zia
func first[T](items: List[T]) -> T {
    return items[0];
}

var numbers = [1, 2, 3];
var names = ["Alice", "Bob"];

var firstNum = first(numbers);   // Returns Integer
var firstName = first(names);    // Returns String
```

The function works with *any* type — the type becomes a parameter. We'll cover generics fully in a later chapter.

---

## The Power of Polymorphic Collections

One of the most practical applications of polymorphism is creating collections of mixed types.

### The Game Loop Pattern

Consider a game with many different kinds of classes:

```zia
interface Updatable {
    func update(deltaTime: Number);
}

class Player implements Updatable {
    expose func update(deltaTime: Number) {
        // Read input, move character, check interactions
    }
}

class Enemy implements Updatable {
    expose func update(deltaTime: Number) {
        // Run AI, chase player, attack if in range
    }
}

class Projectile implements Updatable {
    expose func update(deltaTime: Number) {
        // Move forward, check for collisions
    }
}

class Particle implements Updatable {
    expose func update(deltaTime: Number) {
        // Fade out, drift, disappear when expired
    }
}

class AnimatedDecoration implements Updatable {
    expose func update(deltaTime: Number) {
        // Advance animation frame
    }
}
```

Now the game loop becomes beautifully simple:

```text
var classes: List[Updatable] = [];

// Add all kinds of things
classes.Push(Player());
classes.Push(Enemy());
classes.Push(Enemy());
classes.Push(Particle());
classes.Push(Projectile());
classes.Push(AnimatedDecoration());

// The game loop - handles everything uniformly
while running {
    var deltaTime = getDeltaTime();

    for class in classes {
        class.update(deltaTime);  // Each updates in its own way
    }

    render();
}
```

One loop handles everything. Players, enemies, projectiles, particles — all different types, all updated through the same interface. Adding a new class type means creating a new class that implements `Updatable`. The game loop never changes.

### Rendering Systems

The same pattern works for drawing:

```zia
interface Drawable {
    func draw();
    func getDepth() -> Integer;  // For sorting (draw back-to-front)
}

class Background implements Drawable {
    expose func draw() { /* Draw sky, mountains */ }
    expose func getDepth() -> Integer { return 0; }  // Furthest back
}

class Ground implements Drawable {
    expose func draw() { /* Draw terrain */ }
    expose func getDepth() -> Integer { return 1; }
}

class Character implements Drawable {
    expose func draw() { /* Draw sprite */ }
    expose func getDepth() -> Integer { return 2; }
}

class Foreground implements Drawable {
    expose func draw() { /* Draw trees, buildings in front */ }
    expose func getDepth() -> Integer { return 3; }
}

class UIOverlay implements Drawable {
    expose func draw() { /* Draw health bar, score */ }
    expose func getDepth() -> Integer { return 100; }  // Always on top
}
```

```text
var scene: List[Drawable] = [ui, foreground, player, enemy, ground, background];

// Sort by depth
scene.sortBy(func(a, b) { return a.getDepth() - b.getDepth(); });

// Draw everything in order
for item in scene {
    item.draw();
}
```

The rendering code doesn't know about backgrounds, characters, or UI. It just knows about Drawable things and depths. Perfect separation of concerns.

---

## Polymorphism Enables Extensibility

Let's see how polymorphism makes code genuinely extensible — able to grow without modification.

### A File Format Example

Imagine a document application that can export to different formats:

```text
interface Exporter {
    func export(document: Document) -> String;
    func getExtension() -> String;
}

class PDFExporter implements Exporter {
    expose func export(document: Document) -> String {
        // Convert to PDF format
    }
    expose func getExtension() -> String { return "pdf"; }
}

class HTMLExporter implements Exporter {
    expose func export(document: Document) -> String {
        // Convert to HTML
    }
    expose func getExtension() -> String { return "html"; }
}

class MarkdownExporter implements Exporter {
    expose func export(document: Document) -> String {
        // Convert to Markdown
    }
    expose func getExtension() -> String { return "md"; }
}
```

The export menu:

```text
bind Zanna.Terminal;

class ExportMenu {
    expose List[Exporter] exporters;

    expose func init() {
        self.exporters = [
            PDFExporter(),
            HTMLExporter(),
            MarkdownExporter()
        ];
    }

    expose func addExporter(exporter: Exporter) {
        self.exporters.Push(exporter);
    }

    expose func showOptions() {
        Say("Export as:");
        var i = 1;
        for exporter in self.exporters {
            Say(i + ". " + exporter.getExtension().ToUpper());
            i += 1;
        }
    }

    expose func exportAs(index: Integer, document: Document) -> String {
        var exporter = self.exporters[index];
        return exporter.export(document);
    }
}
```

Later, someone wants to add Word export. They create a `WordExporter` class:

```text
class WordExporter implements Exporter {
    expose func export(document: Document) -> String {
        // Convert to Word format
    }
    expose func getExtension() -> String { return "docx"; }
}
```

And register it:

```text
menu.addExporter(WordExporter());
```

No existing code was modified. The ExportMenu didn't change. The other exporters didn't change. The new functionality was added purely through extension.

### Plugin Architecture

This extensibility is the foundation of plugin systems:

```text
bind Zanna.Terminal;

interface Plugin {
    func getName() -> String;
    func getVersion() -> String;
    func initialize();
    func execute(context: PluginContext);
}

class PluginManager {
    expose List[Plugin] plugins;

    expose func loadPlugin(plugin: Plugin) {
        self.plugins.Push(plugin);
        Say("Loaded: " + plugin.getName() + " v" + plugin.getVersion());
        plugin.initialize();
    }

    expose func runAll(context: PluginContext) {
        for plugin in self.plugins {
            plugin.execute(context);
        }
    }
}
```

The PluginManager is completely decoupled from any specific plugin. It works with any class that implements the `Plugin` interface. Third-party developers can create plugins without access to your source code — they just need to know the interface.

---

## Practical Patterns Using Polymorphism

Let's explore common patterns that leverage polymorphism.

### The Strategy Pattern

Different algorithms, interchangeable at runtime:

```text
interface CompressionStrategy {
    func compress(data: String) -> String;
    func decompress(data: String) -> String;
}

class NoCompression implements CompressionStrategy {
    expose func compress(data: String) -> String { return data; }
    expose func decompress(data: String) -> String { return data; }
}

class GzipCompression implements CompressionStrategy {
    expose func compress(data: String) -> String {
        // Apply gzip algorithm
    }
    expose func decompress(data: String) -> String {
        // Reverse gzip
    }
}

class LZ4Compression implements CompressionStrategy {
    expose func compress(data: String) -> String {
        // Apply LZ4 - faster but less compression
    }
    expose func decompress(data: String) -> String {
        // Reverse LZ4
    }
}

class FileManager {
    compression: CompressionStrategy;

    expose func init(compression: CompressionStrategy) {
        self.compression = compression;
    }

    expose func save(filename: String, data: String) {
        var compressed = self.compression.compress(data);
        File.WriteAllText(filename, compressed);
    }

    expose func load(filename: String) -> String {
        var compressed = File.ReadAllText(filename);
        return self.compression.decompress(compressed);
    }
}
```

Usage:

```text
// Choose strategy based on needs
var fastManager = FileManager(LZ4Compression());      // Speed priority
var smallManager = FileManager(GzipCompression());    // Size priority
var simpleManager = FileManager(NoCompression());     // Debugging

// All three use the same interface
fastManager.save("data.bin", hugeData);
```

The Strategy pattern lets you swap algorithms without changing the code that uses them.

### Event Handlers

Polymorphism excels at event-driven systems:

```text
bind Zanna.Terminal;

interface EventHandler {
    func handle(event: Event);
    func canHandle(event: Event) -> Boolean;
}

class KeyPressHandler implements EventHandler {
    expose func canHandle(event: Event) -> Boolean {
        return event.type == "keypress";
    }

    expose func handle(event: Event) {
        Say("Key pressed: " + event.key);
    }
}

class MouseClickHandler implements EventHandler {
    expose func canHandle(event: Event) -> Boolean {
        return event.type == "click";
    }

    expose func handle(event: Event) {
        Say("Click at: " + event.x + ", " + event.y);
    }
}

class ResizeHandler implements EventHandler {
    expose func canHandle(event: Event) -> Boolean {
        return event.type == "resize";
    }

    expose func handle(event: Event) {
        Say("Window resized to: " + event.width + "x" + event.height);
    }
}

class EventDispatcher {
    expose List[EventHandler] handlers;

    expose func dispatch(event: Event) {
        for handler in self.handlers {
            if handler.canHandle(event) {
                handler.handle(event);
            }
        }
    }
}
```

New event types? New handlers? Just implement the interface and register.

### The Command Pattern

Encapsulate actions as objects for undo/redo:

```zia
interface Command {
    func execute();
    func undo();
    func getDescription() -> String;
}

class InsertTextCommand implements Command {
    document: Document;
    expose Integer position;
    expose String text;

    expose func execute() {
        self.document.insertAt(self.position, self.text);
    }

    expose func undo() {
        self.document.deleteRange(self.position, self.position + self.text.Length);
    }

    expose func getDescription() -> String {
        return "Insert '" + self.text + "'";
    }
}

class DeleteTextCommand implements Command {
    document: Document;
    expose Integer position;
    expose String deletedText;

    expose func execute() {
        self.deletedText = self.document.getRange(self.position, self.position + 1);
        self.document.deleteAt(self.position);
    }

    expose func undo() {
        self.document.insertAt(self.position, self.deletedText);
    }

    expose func getDescription() -> String {
        return "Delete '" + self.deletedText + "'";
    }
}

class CommandHistory {
    expose List[Command] executed;
    expose List[Command] undone;

    expose func execute(command: Command) {
        command.execute();
        self.executed.Push(command);
        self.undone = [];  // Clear redo stack
    }

    expose func undo() {
        if self.executed.Length > 0 {
            var command = self.executed.Pop();
            command.undo();
            self.undone.Push(command);
            Say("Undid: " + command.getDescription());
        }
    }

    expose func redo() {
        if self.undone.Length > 0 {
            var command = self.undone.Pop();
            command.execute();
            self.executed.Push(command);
            Say("Redid: " + command.getDescription());
        }
    }
}
```

The CommandHistory doesn't know about text insertion, deletion, or any specific operation. It just knows Commands can be executed and undone. Polymorphism makes undo/redo trivial to implement.

---

## Mental Models for Polymorphism

Understanding polymorphism deeply requires the right mental model. Here are several ways to think about it.

### Think "What Can It Do?" Not "What Is It?"

Traditional thinking focuses on what something *is*:
- This is a Dog
- This is a Cat
- This is a Bird

Polymorphic thinking focuses on what something *can do*:
- This can Speak
- This can Move
- This can be Drawn

When you design with polymorphism, ask: "What behaviors do I need?" not "What types do I have?"

```zia
// Type-focused (brittle)
func feedAnimal(animal: Animal, animalType: String) {
    if animalType == "dog" {
        feedDog(animal);
    } else if animalType == "cat" {
        feedCat(animal);
    }
}

// Behavior-focused (flexible)
func feedAnimal(animal: Feedable) {
    animal.feed();  // Each animal knows how to feed itself
}
```

### The Contract Mental Model

An interface is a contract. When a class implements an interface, it makes a promise: "I guarantee I can do these things."

When you write code that accepts an interface:
- You're saying "I need something that can do X"
- You don't care how it does X
- You trust that anything claiming to do X will actually do X

This is like hiring: "I need someone who can drive a truck." You don't care if they learned from their parent, a driving school, or the military. You just need someone who can drive a truck.

### The Substitution Mental Model

*Liskov Substitution Principle*: Anywhere you expect a base type, you can use any derived type.

If your code expects a `Shape`, you can give it a `Circle`, `Rectangle`, or `Triangle`. If your code expects a `PaymentMethod`, you can give it `Cash`, `CreditCard`, or `MobilePayment`.

Think of polymorphism as creating "slots" that can be filled by different things:

```zia
// This function has a "slot" for any Drawable
func render(item: Drawable) {
    item.draw();
}

// Any Drawable fits in that slot
render(new Circle());     // Circle fills the slot
render(new Rectangle());  // Rectangle fills the slot
render(new Text());       // Text fills the slot
render(new Group());      // Even a Group of Drawables fills the slot
```

---

## Combining Inheritance and Interfaces

Polymorphism is most powerful when you combine inheritance (shared code) with interfaces (shared contracts).

```text
// Interface: defines what things can do
interface Attackable {
    func takeDamage(amount: Integer);
    func isAlive() -> Boolean;
}

interface Drawable {
    func draw();
}

interface Movable {
    func move(dx: Number, dy: Number);
}

// Base class: provides shared implementation
class GameEntity {
    expose Number x;
    expose Number y;

    expose func getPosition() -> (Number, Number) {
        return (self.x, self.y);
    }
}

// Derived classes: inherit code AND implement interfaces
class Player extends GameEntity implements Attackable, Drawable, Movable {
    expose Integer health;
    sprite: Sprite;

    expose func takeDamage(amount: Integer) {
        self.health -= amount;
        self.playHurtAnimation();
    }

    expose func isAlive() -> Boolean {
        return self.health > 0;
    }

    expose func draw() {
        self.sprite.drawAt(self.x, self.y);
    }

    expose func move(dx: Number, dy: Number) {
        self.x += dx;
        self.y += dy;
        self.playWalkAnimation();
    }
}

class Tree extends GameEntity implements Drawable {
    // Trees don't move or take damage - just Drawable
    expose func draw() {
        drawTreeSprite(self.x, self.y);
    }
}

class Enemy extends GameEntity implements Attackable, Drawable, Movable {
    expose Integer health;

    expose func takeDamage(amount: Integer) {
        self.health -= amount;
        self.flashRed();
    }

    expose func isAlive() -> Boolean {
        return self.health > 0;
    }

    expose func draw() {
        drawEnemySprite(self.x, self.y);
    }

    expose func move(dx: Number, dy: Number) {
        // AI-controlled movement
        self.x += dx;
        self.y += dy;
    }
}
```

Now you have maximum flexibility:

```zia
var drawables: List[Drawable] = [player, tree1, tree2, enemy1, enemy2];
var attackables: List[Attackable] = [player, enemy1, enemy2];
var movables: List[Movable] = [player, enemy1, enemy2];

// Each collection serves a different purpose
for d in drawables { d.draw(); }        // Render everything
for a in attackables { a.takeDamage(1); }  // Area damage
for m in movables { m.move(wind.x, wind.y); }  // Wind pushes things
```

The Player can be in all three collections. The Tree only appears in drawables. Each class participates in exactly the systems that make sense for it.

---

## A Complete Example: Drawing System

Let's build a complete rendering system that demonstrates polymorphism's power:

```zia
module DrawingSystem;

bind Zanna.Terminal;

interface Drawable {
    func draw();
    func getBounds() -> Rect;
}

struct Rect {
    expose Number x;
    expose Number y;
    expose Number width;
    expose Number height;

    expose func init(x: Number, y: Number, width: Number, height: Number) {
        self.x = x;
        self.y = y;
        self.width = width;
        self.height = height;
    }

    expose func contains(px: Number, py: Number) -> Boolean {
        return px >= self.x && px <= self.x + self.width &&
               py >= self.y && py <= self.y + self.height;
    }
}

class Circle implements Drawable {
    expose Number x;
    expose Number y;
    expose Number radius;
    expose String color;

    expose func init(x: Number, y: Number, radius: Number, color: String) {
        self.x = x;
        self.y = y;
        self.radius = radius;
        self.color = color;
    }

    expose func draw() {
        Say("Drawing " + self.color + " circle at (" +
            self.x + ", " + self.y + ") radius " + self.radius);
    }

    expose func getBounds() -> Rect {
        return new Rect(self.x - self.radius, self.y - self.radius,
                        self.radius * 2, self.radius * 2);
    }
}

class Rectangle implements Drawable {
    expose Number x;
    expose Number y;
    expose Number width;
    expose Number height;
    expose String color;

    expose func init(x: Number, y: Number, w: Number, h: Number, color: String) {
        self.x = x;
        self.y = y;
        self.width = w;
        self.height = h;
        self.color = color;
    }

    expose func draw() {
        Say("Drawing " + self.color + " rectangle at (" +
            self.x + ", " + self.y + ") size " + self.width + "x" + self.height);
    }

    expose func getBounds() -> Rect {
        return new Rect(self.x, self.y, self.width, self.height);
    }
}

class Text implements Drawable {
    expose Number x;
    expose Number y;
    expose String content;
    expose Number fontSize;

    expose func init(x: Number, y: Number, content: String) {
        self.x = x;
        self.y = y;
        self.content = content;
        self.fontSize = 16.0;
    }

    expose func draw() {
        Say("Drawing text '" + self.content + "' at (" +
            self.x + ", " + self.y + ")");
    }

    expose func getBounds() -> Rect {
        var width = self.content.Length * self.fontSize * 0.6;  // Approximate
        return new Rect(self.x, self.y, width, self.fontSize);
    }
}

// Group of drawables - demonstrates the Composite Pattern
class Group implements Drawable {
    expose List[Drawable] children;
    expose String name;

    expose func init(name: String) {
        self.children = [];
        self.name = name;
    }

    expose func add(item: Drawable) {
        self.children.Push(item);
    }

    expose func draw() {
        Say("--- Drawing group: " + self.name + " ---");
        for child in self.children {
            child.draw();
        }
        Say("--- End group: " + self.name + " ---");
    }

    expose func getBounds() -> Rect {
        if self.children.Length == 0 {
            return new Rect(0.0, 0.0, 0.0, 0.0);
        }

        var first = self.children[0].getBounds();
        var minX = first.x;
        var minY = first.y;
        var maxX = first.x + first.width;
        var maxY = first.y + first.height;

        for i in 1..self.children.Length {
            var b = self.children[i].getBounds();
            if b.x < minX { minX = b.x; }
            if b.y < minY { minY = b.y; }
            if b.x + b.width > maxX { maxX = b.x + b.width; }
            if b.y + b.height > maxY { maxY = b.y + b.height; }
        }

        return new Rect(minX, minY, maxX - minX, maxY - minY);
    }
}

// Function that works with any Drawable - demonstrates polymorphism
func findItemAt(items: List[Drawable], x: Number, y: Number) -> Drawable? {
    for item in items {
        if item.getBounds().contains(x, y) {
            return item;
        }
    }
    return null;
}

func start() {
    // Create individual shapes
    var rect = new Rectangle(10.0, 10.0, 100.0, 50.0, "blue");
    var circle = new Circle(80.0, 35.0, 20.0, "red");
    var label = new Text(120.0, 30.0, "Hello!");

    // Create a group (polymorphism: Group is also Drawable)
    var icons = new Group("Icons");
    icons.add(new Circle(200.0, 100.0, 10.0, "green"));
    icons.add(new Circle(220.0, 100.0, 10.0, "yellow"));
    icons.add(new Rectangle(240.0, 95.0, 20.0, 10.0, "orange"));

    // Create main scene - mixing individual items and groups
    var scene = new Group("Main Scene");
    scene.add(rect);
    scene.add(circle);
    scene.add(label);
    scene.add(icons);  // Group inside group!

    // Draw everything with one call
    Say("=== Drawing Scene ===");
    scene.draw();

    // Get total bounds
    var bounds = scene.getBounds();
    Say("");
    Say("Scene bounds: " + bounds.width + "x" + bounds.height);

    // Find item at click position - works with any Drawable
    var allItems: List[Drawable] = [];
    allItems.Push(rect);
    allItems.Push(circle);
    allItems.Push(label);
    var clicked = findItemAt(allItems, 50.0, 30.0);
    if clicked != null {
        Say("Clicked on something!");
    }
}
```

Notice how `Group` is itself `Drawable`. This is the *Composite Pattern* — treating individuals and compositions uniformly through polymorphism. A scene can contain shapes, text, *and other groups*. The `draw()` method works the same way regardless of the complexity of what's inside.

---

## The Two Languages

**Zia**
```zia
bind Zanna.Terminal;

interface Animal {
    func speak();
}

class Dog implements Animal {
    expose func speak() {
        Say("Woof!");
    }
}

class Cat implements Animal {
    expose func speak() {
        Say("Meow!");
    }
}

var animals: List[Animal] = [];
animals.Push(new Dog());
animals.Push(new Cat());
for a in animals {
    a.speak();  // Polymorphism: right method called for each
}
```

**BASIC**
```text
INTERFACE Animal
    SUB Speak()
END INTERFACE

CLASS Dog IMPLEMENTS Animal
    SUB Speak()
        PRINT "Woof!"
    END SUB
END CLASS

CLASS Cat IMPLEMENTS Animal
    SUB Speak()
        PRINT "Meow!"
    END SUB
END CLASS

DIM animals() AS Animal = [NEW Dog(), NEW Cat()]
FOR EACH a IN animals
    a.Speak()   ' Polymorphism in action
NEXT
```

---

## Enums and Pattern Matching

Polymorphism isn't limited to classes and interfaces. Zanna also supports **enumerations** — fixed sets of named constants — with exhaustive pattern matching through `match`:

```zia
enum Direction { North, South, East, West }

func describe(dir: Direction) -> String {
    match dir {
        Direction.North => return "Going up";
        Direction.South => return "Going down";
        Direction.East  => return "Going right";
        Direction.West  => return "Going left";
    }
    return "Unknown";
}
```

The compiler verifies that every variant is handled (or that a `_` wildcard is present), catching missing cases at compile time rather than runtime. This is a form of *compile-time polymorphism* — one function handles multiple forms of a value, with the compiler ensuring completeness.

Enums are especially useful for state machines, command types, and any domain where a value must be exactly one of a known set. For full syntax details, see the canonical [Zia reference](../../languages/zia-reference.md#enums) and [BASIC reference](../../languages/basic-reference.md#enum).

---

## Common Mistakes and Misconceptions

### Mistake 1: Confusing Variable Type with Object Type

```zia
var animal: Animal = new Dog();
```

The variable type is `Animal`. The object type is `Dog`. For polymorphism, the *object type* determines behavior.

### Mistake 2: Forgetting That Interfaces Have No Code

Interfaces define *what*, not *how*. You can't put implementation in an interface:

```text
// Wrong!
interface Drawable {
    func draw() {
        Zanna.Terminal.Say("Drawing");  // Can't do this
    }
}

// Right
interface Drawable {
    func draw();  // Just the signature
}
```

### Mistake 3: Overusing Inheritance When Interfaces Would Be Better

Not everything needs to share code. Often, shared *behavior contracts* (interfaces) are more flexible than shared *implementation* (inheritance).

```zia
// Awkward: forcing inheritance for unrelated things
class Drawable { func draw() { } }
class DrawableCircle extends Drawable { ... }
class DrawableEnemy extends Drawable { ... }
class DrawableUI extends Drawable { ... }

// Better: interface lets unrelated things share behavior
interface Drawable { func draw(); }
class Circle implements Drawable { ... }
class Enemy implements Drawable { ... }
class UIElement implements Drawable { ... }
```

### Mistake 4: Not Using Polymorphism When You Should

If you see code like this:

```text
if type == "A" {
    doA();
} else if type == "B" {
    doB();
} else if type == "C" {
    doC();
}
```

That's often a sign you should use polymorphism instead.

---

## Benefits of Polymorphism: Summary

1. **Flexibility**: Add new types without changing existing code
2. **Simplicity**: Write one function that handles many types
3. **Extensibility**: Design systems that can grow organically
4. **Testability**: Swap real implementations for test doubles
5. **Loose coupling**: Code depends on abstractions, not concrete types
6. **Reduced duplication**: No need for repetitive type-checking code
7. **Better organization**: Each type handles its own behavior

---

## Summary

- *Polymorphism* means "many forms" — one interface, many implementations
- **Runtime polymorphism** (dynamic dispatch) determines methods at runtime based on actual object type
- **Compile-time polymorphism** (overloading) determines methods at compile time based on argument types
- Use base classes or interfaces to treat different types uniformly
- Collections can hold mixed types through common interfaces
- The **Strategy pattern** swaps algorithms through interfaces
- The **Command pattern** encapsulates actions for undo/redo
- The **Composite pattern** treats individuals and groups uniformly
- Think about *what things can do*, not *what things are*
- Combine inheritance (shared code) with interfaces (shared contracts) for maximum flexibility
- Polymorphism is the key to building extensible, maintainable systems

---

## Exercises

**Exercise 17.1**: Create a `Shape` hierarchy where you can store Circle, Rectangle, and Triangle in one array and call `area()` on each. Print the total area of all shapes.

**Exercise 17.2**: Create an audio system: `SoundSource` interface with `play()` and `stop()`, implemented by `MusicTrack`, `SoundEffect`, and `AmbientSound`. Write a mixer that can play and stop all sources.

**Exercise 17.3**: Create a notification system: `Notifier` interface with `send(message)`, implemented by `EmailNotifier`, `SMSNotifier`, `PushNotifier`, and `SlackNotifier`. Write code that sends a message through multiple notifiers.

**Exercise 17.4**: Implement the composite pattern: create a `FileSystemItem` interface with `getSize() -> Integer`, with `File` and `Folder` implementations. Folders contain items and report their total size.

**Exercise 17.5**: Create a validation system: `Validator` interface with `validate(input: String) -> Boolean` and `getError() -> String`. Create `LengthValidator`, `EmailValidator`, `NumberValidator`. Write code that applies multiple validators.

**Exercise 17.6**: Implement a simple calculator using the Command pattern with undo/redo. Commands: `AddCommand`, `SubtractCommand`, `MultiplyCommand`, `DivideCommand`.

**Exercise 17.7** (Challenge): Build a simple expression evaluator: `Expression` interface with `evaluate() -> Number`, implemented by `NumericLiteral`, `Add`, `Subtract`, `Multiply`, `Divide`. Create an expression tree like `Add(Multiply(2, 3), 4)` and evaluate it (should return 10).

**Exercise 17.8** (Challenge): Create a simple game with polymorphic classes. Define `GameEntity` interface with `update()` and `render()`. Create `Player`, `Enemy`, `Projectile`, and `PowerUp` classes. Write a game loop that updates and renders all classes.

---

*We've mastered the core of object-oriented programming. You now understand how inheritance provides shared implementation, interfaces provide shared contracts, and polymorphism ties them together into flexible, extensible systems. Next, we look at design patterns — recurring solutions to recurring problems that experienced programmers use every day.*

*[Continue to Chapter 18: Design Patterns](18-patterns.md)*
