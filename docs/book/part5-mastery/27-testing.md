---
status: active
audience: public
last-verified: 2026-08-17
---

# Chapter 27: Testing

You finish writing a feature. It compiles. You run it once with some typical input, and it works. You run it again with slightly different input, and that works too. Confidence builds. You ship it.

Three weeks later, a user reports a crash. A particular combination of inputs you never considered causes the program to fail spectacularly. You spend four hours debugging, finally find the issue, fix it, and ship again. Two days later, another user finds a different edge case. More debugging, more fixing.

Now imagine a different scenario. You write the same feature, but this time you also write tests --- small programs that automatically verify your code works correctly. You test typical cases, boundary conditions, error situations. When you finish, you have a suite of tests that run in seconds and check dozens of scenarios. If future changes break anything, you'll know immediately.

This second approach takes more time upfront, but it saves far more time in the long run. Testing isn't just about finding bugs --- it's about building confidence, enabling safe changes, and documenting how code should behave. This chapter teaches you to test systematically, transforming testing from an afterthought into a fundamental part of how you write software.

> **Current Zanna note:** Zia does not currently have a built-in `test "name" { ... }` syntax, an `assert` statement, or a `Zanna.Test` module. The test blocks in this chapter are pseudocode that show test intent and structure. In the Zanna repository today, automated tests are written as C++ unit/e2e tests, BASIC/Zia sample programs, or shell-driven checks run through CTest and the scripts in `docs/internals/testing.md`.

---

## Why Testing Saves Time

This seems paradoxical: spending time writing tests saves time. But consider what happens without tests:

**The debugging tax**: Every bug found in production costs far more than one found during development. You must understand the user's report, reproduce the issue, find the cause, fix it, verify the fix, and deploy. This easily consumes hours for bugs that a test would have caught in seconds.

**The fear of change**: Without tests, changing code is terrifying. Will your improvement break something? Will your refactoring introduce subtle bugs? Developers become paralyzed, afraid to improve code because they can't verify it still works. Technical debt accumulates.

**The repetition cost**: Manual testing is tedious and error-prone. Testing the same feature for the hundredth time, humans get sloppy. Computers don't. A test suite runs the same checks with the same thoroughness every single time.

**The regression nightmare**: Software that worked last month doesn't work today. Something changed, somewhere, and broke it. Without tests, finding what broke is archaeology. With tests, you see exactly which test started failing and when.

**The documentation gap**: Code without tests is mysterious. How is this function supposed to be used? What inputs does it expect? What does it return? Tests answer these questions through working examples.

Let's do some rough math. Say you write a medium-sized feature in 10 hours. Without tests, you might spend:

- 10 hours writing code
- 2 hours manually testing during development
- 4 hours debugging issues found after release
- 3 hours fixing regressions over the next year
- 2 hours explaining the code to colleagues who don't understand it

Total: 21 hours

With tests:

- 10 hours writing code
- 4 hours writing tests
- 1 hour fixing issues caught by tests
- 0.5 hours fixing regressions (tests catch them immediately)
- 0.5 hours explaining code (tests serve as documentation)

Total: 16 hours

And this understates the difference. The tested code is more maintainable, gives you confidence to make improvements, and provides permanent documentation. The untested code becomes a liability that costs more with each passing month.

---

## Mental Models for Testing

To understand testing deeply, it helps to have mental models --- ways of thinking about what tests do and why they matter.

### The Safety Net

Imagine a trapeze artist performing without a net. Every move is terrifying because any mistake means catastrophe. With a net below, the artist can attempt more daring maneuvers, knowing that failure won't be fatal.

Tests are your safety net. When you refactor code, the tests catch you if you fall. When you add features, the tests verify you haven't broken existing functionality. You can be bold because you have protection.

Without tests, every code change is a high-wire act. With tests, you can experiment, improve, and refactor with confidence.

### The Quality Assurance Inspector

Picture a factory assembly line with an inspector who checks each product before it ships. The inspector follows a checklist: Does this work? Does that connect properly? Is this within tolerance?

Tests are your automated inspectors. They check your code against specifications continuously. Unlike human inspectors who get tired and make mistakes, tests apply the same rigorous standards every time, instantly, for free.

### Insurance Against Disaster

You buy insurance not because you expect disaster, but because disasters happen unexpectedly. The cost of insurance is far less than the cost of an uninsured catastrophe.

Tests are insurance for your code. You write them not because you expect bugs, but because bugs happen. The cost of writing tests is far less than the cost of shipping broken software.

### The Living Specification

Documentation gets out of date. Comments lie. But tests that pass are truthful by definition --- they demonstrate what the code actually does, not what someone thought it would do months ago.

Tests are executable specifications. They define correct behavior in a way that can be verified automatically. When requirements change, you update the tests, and they become the new source of truth.

### The Debugging Microscope

When something goes wrong, tests help you isolate the problem. Like a scientist using controls in an experiment, tests let you vary one thing while holding others constant. If a focused test fails, you know exactly where the bug lives.

---

## Types of Tests

Not all tests are the same. Different tests verify different aspects of your software, and each type has its place.

### Unit Tests

Unit tests verify that individual pieces of code work correctly in isolation. A "unit" is typically a single function, method, or small class --- the smallest testable piece of behavior.

```text
// The function we want to test
func isPrime(n: Integer) -> Boolean {
    if n < 2 { return false; }
    if n == 2 { return true; }
    if n % 2 == 0 { return false; }

    var i = 3;
    while i * i <= n {
        if n % i == 0 {
            return false;
        }
        i += 2;
    }
    return true;
}

// Unit tests for isPrime
test "isPrime returns false for numbers less than 2" {
    assert !isPrime(0);
    assert !isPrime(1);
    assert !isPrime(-5);
}

test "isPrime returns true for 2" {
    assert isPrime(2);
}

test "isPrime returns true for prime numbers" {
    assert isPrime(3);
    assert isPrime(5);
    assert isPrime(7);
    assert isPrime(11);
    assert isPrime(13);
    assert isPrime(97);
}

test "isPrime returns false for composite numbers" {
    assert !isPrime(4);
    assert !isPrime(6);
    assert !isPrime(9);
    assert !isPrime(15);
    assert !isPrime(100);
}
```

Unit tests are:
- **Fast**: They test small pieces, so they run in milliseconds
- **Focused**: When they fail, you know exactly what's broken
- **Independent**: Each test runs on its own, without depending on others
- **Numerous**: A typical project has hundreds or thousands of unit tests

Think of unit tests as verifying that each brick is solid before you build the wall.

### Integration Tests

Integration tests verify that multiple pieces work together correctly. While unit tests check individual functions, integration tests check that those functions collaborate properly.

```text
// Unit test: Just the validator
test "email validator rejects invalid format" {
    var validator = EmailValidator();
    assert !validator.isValid("notanemail");
    assert !validator.isValid("missing@domain");
    assert !validator.isValid("@nodomain.com");
}

// Integration test: Validator + User + Database working together
test "registration flow rejects invalid email" {
    var db = TestDatabase.create();
    var validator = EmailValidator();
    var userService = UserService(db, validator);

    var result = userService.register("notanemail", "password123");

    assert result.success == false;
    assert result.error == "Invalid email format";
    assert db.userCount() == 0;  // No user was created
}

test "registration flow creates user with valid email" {
    var db = TestDatabase.create();
    var validator = EmailValidator();
    var userService = UserService(db, validator);

    var result = userService.register("alice@example.com", "password123");

    assert result.success == true;
    assert db.userCount() == 1;
    assert db.findByEmail("alice@example.com") != null;
}
```

Integration tests are:
- **Broader**: They test multiple components working together
- **Slower**: More setup, more code executing, more to check
- **Realistic**: They catch problems that only appear when components interact
- **Fewer**: You have more unit tests than integration tests

Think of integration tests as verifying that the bricks fit together properly to form a sturdy wall.

### End-to-End Tests

End-to-end (E2E) tests verify complete workflows from the user's perspective. They simulate real user actions and check that the entire system behaves correctly.

```text
test "complete user journey: registration to purchase" {
    var app = Application.startForTesting();
    var browser = TestBrowser.create();

    // User visits homepage
    browser.navigate(app.url);
    assert browser.currentPage().Contains("Welcome");

    // User registers
    browser.click("Register");
    browser.fill("email", "alice@example.com");
    browser.fill("password", "securepassword123");
    browser.click("Submit");
    assert browser.currentPage().Contains("Registration successful");

    // User browses products
    browser.click("Products");
    assert browser.currentPage().Contains("Widget");

    // User adds item to cart
    browser.click("Add to Cart");
    assert browser.currentPage().Contains("Cart (1)");

    // User completes purchase
    browser.click("Checkout");
    browser.fill("cardNumber", "4111111111111111");
    browser.click("Complete Purchase");
    assert browser.currentPage().Contains("Thank you for your order");

    app.shutdown();
}
```

End-to-end tests are:
- **Comprehensive**: They test entire user workflows
- **Slowest**: They run the whole application, often with real browsers
- **Brittle**: Changes to UI can break them even if functionality is correct
- **Fewest**: You have few E2E tests compared to unit and integration tests

Think of end-to-end tests as verifying that the completed building is habitable --- that someone can walk in the front door and live there.

### The Testing Pyramid

A well-tested application has many more unit tests than integration tests, and many more integration tests than end-to-end tests. This is often visualized as a pyramid:

```text
        /\
       /  \        End-to-End Tests (few, slow, comprehensive)
      /----\
     /      \      Integration Tests (some, moderate speed)
    /--------\
   /          \    Unit Tests (many, fast, focused)
  /------------\
```

Why this shape?

**Unit tests** are fast and cheap to write, so you can have many. They give you quick feedback during development.

**Integration tests** take longer but catch problems unit tests miss. You need enough to verify components work together.

**End-to-end tests** are slowest and most expensive but verify the user experience. You need them, but not too many.

An inverted pyramid (many E2E tests, few unit tests) leads to slow test suites, fragile tests, and difficulty debugging failures.

---

## Your First Test

Let's start with the basics. Here's a simple function and a test for it:

```text
func add(a: Integer, b: Integer) -> Integer {
    return a + b;
}

test "add returns sum of two numbers" {
    assert add(2, 3) == 5;
    assert add(0, 0) == 0;
    assert add(-1, 1) == 0;
}
```

In the current toolchain, put runnable checks in a small `.zia` program and run it with:

```bash
zanna run myprogram.zia
```

Repository tests are run with CTest after a build:

```text
ctest --test-dir build --output-on-failure
```

Let's break down what's happening:

- A test should have a descriptive name
- Each check should verify one clear condition
- If all checks pass, the test passes
- If any check fails, report the failing behavior and enough context to debug it

The test name is important --- it should describe what you're testing, not how. "add returns sum of two numbers" tells you exactly what behavior is being verified.

---

## Assertions: The Language of Tests

Assertions are statements that must be true for a test to pass. They're how you express expectations about your code.

### Basic Assertions

```text
test "basic assertions" {
    // Equality
    assert 1 + 1 == 2;
    assert "hello" == "hello";

    // Inequality
    assert 5 != 3;

    // Comparisons
    assert 10 > 5;
    assert 3 <= 3;
    assert 7 >= 7;

    // Boolean
    assert true;
    assert !false;

    // Null checks
    var x: String? = "hello";
    assert x != null;

    var y: String? = null;
    assert y == null;
}
```

### Assertions with Messages

When assertions fail, messages help you understand what went wrong:

```text
test "with messages" {
    var result = calculateTax(100);
    assert result == 10, "Expected 10% tax on 100, got " + result;
}
```

Without a message, a failing assertion just tells you the condition was false. With a message, you see exactly what was expected and what happened.

### Common Assert Functions

Many test frameworks provide specialized assertion functions with better error messages. Zanna does not currently ship this source-level assertion module, but these examples show the behavior such helpers would provide:

```text
test "assert variants" {
    // Basic
    assert condition;
    assert condition, "custom message";

    // Equality (shows expected vs actual on failure)
    assertEqual(actual, expected);
    assertNotEqual(a, b);

    // Floating point (handles precision issues)
    assertClose(3.14159, 3.14, 0.01);  // Within tolerance

    // Null
    assertNull(value);
    assertNotNull(value);

    // Exceptions
    assertThrows(func() {
        riskyOperation();
    });

    // Collections
    assertContains(list, item);
    assertEmpty(list);
    assertLength(list, 5);
}
```

Why use `assertEqual(actual, expected)` instead of `assert actual == expected`? Consider a failing test:

```text
// With basic assert:
assert result == 42;
// Failure: assertion failed

// With assertEqual:
assertEqual(result, 42);
// Failure: expected 42, got 37
```

The specialized assertion tells you what went wrong, not just that something went wrong.

---

## Test Structure: Arrange, Act, Assert

Good tests follow a consistent pattern called "Arrange-Act-Assert" (or "Given-When-Then"):

```text
test "user can update email" {
    // Arrange: Set up the test conditions
    var user = User("alice", "alice@old.com");

    // Act: Perform the action being tested
    user.updateEmail("alice@new.com");

    // Assert: Verify the expected outcome
    assert user.email == "alice@new.com";
}
```

This three-part structure makes tests clear and consistent:

**Arrange** (Given): Create the objects, set up the initial state, prepare everything needed for the test. This is the "preconditions."

**Act** (When): Perform the single action being tested. This should usually be one line --- one function call, one method invocation, one operation.

**Assert** (Then): Verify that the action produced the expected results. Check return values, state changes, side effects.

### Why This Structure Matters

Consider a test without clear structure:

```text
test "confusing test" {
    var cart = ShoppingCart();
    cart.Add(Item("Book", 15.00));
    assert cart.itemCount() == 1;
    cart.Add(Item("Pen", 2.00));
    assert cart.itemCount() == 2;
    assert cart.total() == 17.00;
    cart.remove("Book");
    assert cart.itemCount() == 1;
    assert cart.total() == 2.00;
}
```

What is this testing? Adding? Removing? Totals? It's hard to tell because multiple actions and assertions are mixed together.

Compare with structured tests:

```text
test "adding item increases count" {
    // Arrange
    var cart = ShoppingCart();
    cart.Add(Item("Book", 15.00));

    // Act
    cart.Add(Item("Pen", 2.00));

    // Assert
    assertEqual(cart.itemCount(), 2);
}

test "total reflects all item prices" {
    // Arrange
    var cart = ShoppingCart();
    cart.Add(Item("Book", 15.00));
    cart.Add(Item("Pen", 2.00));

    // Act
    var total = cart.total();

    // Assert
    assertClose(total, 17.00, 0.01);
}

test "removing item decreases count and total" {
    // Arrange
    var cart = ShoppingCart();
    cart.Add(Item("Book", 15.00));
    cart.Add(Item("Pen", 2.00));

    // Act
    cart.remove("Book");

    // Assert
    assertEqual(cart.itemCount(), 1);
    assertClose(cart.total(), 2.00, 0.01);
}
```

Each test now verifies one specific behavior. When a test fails, you know exactly what's broken.

---

## What to Test: Choosing Your Cases

Writing tests is not about covering every possible input (there are infinitely many). It's about strategically choosing inputs that reveal bugs.

### Normal Cases (Happy Path)

Test typical usage --- the cases that should work:

```text
test "divide normal cases" {
    assertEqual(divide(10, 2), 5);
    assertEqual(divide(9, 3), 3);
    assertEqual(divide(100, 10), 10);
}
```

These verify the basic functionality works as expected.

### Edge Cases (Boundaries)

Test the boundaries and limits where bugs often hide:

```text
test "divide edge cases" {
    assertEqual(divide(0, 5), 0);       // Zero numerator
    assertEqual(divide(5, 1), 5);       // Divide by one
    assertEqual(divide(1, 1), 1);       // Same values
    assertEqual(divide(-10, 2), -5);    // Negative numerator
    assertEqual(divide(10, -2), -5);    // Negative divisor
    assertEqual(divide(-10, -2), 5);    // Both negative
}
```

Edge cases live at the boundaries: zero, one, negative numbers, maximum values, empty strings, empty collections. Bugs love these places.

### Error Cases

Test that errors are handled correctly:

```text
test "divide by zero throws" {
    assertThrows(func() {
        divide(5, 0);
    });
}

test "withdraw fails with insufficient funds" {
    var account = BankAccount(100);
    var result = account.withdraw(150);

    assert result == false;
    assertEqual(account.balance, 100);  // Balance unchanged
}
```

Error handling is as important as the happy path. Tests should verify that errors are detected and handled gracefully.

### The ZOMBIES Mnemonic

A helpful mnemonic for remembering what to test:

- **Z**ero: What happens with zero, empty, null, or none?
- **O**ne: What happens with exactly one item?
- **M**any: What happens with many items?
- **B**oundary: What happens at limits and edges?
- **I**nterface: What happens with different input combinations?
- **E**xception: What happens when things go wrong?
- **S**imple: Start with the simplest case that could work

Let's apply ZOMBIES to testing a stack:

```text
class Stack[T] {
    hide items: List[T];

    expose func init() {
        self.items = [];
    }

    func push(item: T) {
        self.items.Push(item);
    }

    func pop() -> T? {
        if self.items.Length == 0 {
            return null;
        }
        return self.items.Pop();
    }

    func isEmpty() -> Boolean {
        return self.items.Length == 0;
    }

    func size() -> Integer {
        return self.items.Length;
    }
}

// Zero: Empty stack behavior
test "new stack is empty" {
    var stack = new Stack[Integer]();
    assert stack.IsEmpty();
    assertEqual(stack.Length, 0);
}

test "pop on empty stack returns null" {
    var stack = new Stack[Integer]();
    assert stack.Pop() == null;
}

// One: Single element behavior
test "stack with one element is not empty" {
    var stack = new Stack[Integer]();
    stack.Push(42);
    assert !stack.IsEmpty();
    assertEqual(stack.Length, 1);
}

test "pop returns the single element" {
    var stack = new Stack[Integer]();
    stack.Push(42);
    assertEqual(stack.Pop(), 42);
    assert stack.IsEmpty();
}

// Many: Multiple elements behavior
test "stack maintains LIFO order" {
    var stack = new Stack[Integer]();
    stack.Push(1);
    stack.Push(2);
    stack.Push(3);

    assertEqual(stack.Pop(), 3);
    assertEqual(stack.Pop(), 2);
    assertEqual(stack.Pop(), 1);
}

// Boundary: Edge behavior
test "push then pop returns to empty" {
    var stack = new Stack[Integer]();
    stack.Push(1);
    stack.Pop();
    assert stack.IsEmpty();
}

// Exception/Error: Error behavior
test "multiple pops on empty stack all return null" {
    var stack = new Stack[Integer]();
    assert stack.Pop() == null;
    assert stack.Pop() == null;
}
```

---

## Writing Good Test Names

Test names are documentation. They should describe what behavior is being tested in a way that's clear to someone who hasn't seen the code.

### Bad Test Names

```text
test "test1" { ... }
test "user test" { ... }
test "it works" { ... }
test "bug fix" { ... }
```

These tell you nothing. When they fail, you have to read the test code to understand what broke.

### Good Test Names

```text
test "new user starts with zero balance" { ... }
test "deposit increases balance by deposit amount" { ... }
test "withdraw fails when amount exceeds balance" { ... }
test "transfer moves money from source to destination" { ... }
```

Good test names:
- Describe the scenario or behavior being tested
- Are specific enough to understand without reading the code
- Often follow a pattern: "class/function does something when condition"

### Test Names as Documentation

Imagine all your tests listed together:

```text
Pass: new user starts with zero balance
Pass: deposit increases balance by deposit amount
Pass: deposit with negative amount fails
Pass: withdraw reduces balance by withdrawal amount
Pass: withdraw fails when amount exceeds balance
Pass: transfer moves money from source to destination
Pass: transfer fails when source has insufficient funds
```

This list becomes living documentation of how your system behaves. Anyone can read it and understand the rules.

---

## Test Setup and Teardown

Many tests need common setup --- creating test data, initializing objects, connecting to test databases. Rather than repeating this in every test, you can use setup and teardown blocks.

```text
bind Zanna.Test;

var testDatabase: Database;
var testUser: User;

// Setup runs before each test
setup {
    testDatabase = Database.createTemporary();
    testDatabase.migrate();
    testUser = User("testuser", "test@example.com");
    testDatabase.Insert(testUser);
}

// Teardown runs after each test
teardown {
    testDatabase.destroy();
}

test "can find user by email" {
    var found = testDatabase.findUserByEmail("test@example.com");
    assertNotNull(found);
    assertEqual(found.name, "testuser");
}

test "can delete user" {
    testDatabase.deleteUser(testUser);
    var found = testDatabase.findUserByEmail("test@example.com");
    assertNull(found);
}

test "deleting user doesn't affect other users" {
    var other = User("other", "other@example.com");
    testDatabase.Insert(other);

    testDatabase.deleteUser(testUser);

    var found = testDatabase.findUserByEmail("other@example.com");
    assertNotNull(found);
}
```

Each test starts with a fresh database containing the test user. After each test, the database is destroyed, ensuring no test affects any other test.

### Why Teardown Matters

Without proper teardown, tests can interfere with each other:

- Files created by one test affect the next test
- Database records accumulate across tests
- Global state changes persist

This causes tests that pass alone but fail together, or tests that mysteriously fail on certain days but not others. Always clean up after yourself.

---

## Test Doubles: Mocks, Stubs, and Fakes

Real systems depend on external resources: databases, network services, the file system, the current time. Testing against real dependencies is slow, unreliable, and can have side effects (you don't want tests that send real emails!).

Test doubles are objects that stand in for real dependencies during testing. There are several types.

### Stubs: Provide Canned Responses

A stub returns predetermined values, ignoring its inputs:

```text
// Real implementation
class WeatherService {
    func getTemperature(city: String) -> Number {
        var response = Http.Get("https://api.weather.com/" + city);
        return parseTemperature(response);
    }
}

// Stub for testing
class StubWeatherService implements IWeatherService {
    hide temperature: Number;

    expose func init(temp: Number) {
        self.temperature = temp;
    }

    func getTemperature(city: String) -> Number {
        return self.temperature;  // Always returns the configured value
    }
}

test "thermostat turns on heat when cold" {
    // Arrange: Create a stub that always reports 30 degrees
    var weather = StubWeatherService(30.0);
    var thermostat = Thermostat(weather);

    // Act
    thermostat.check();

    // Assert
    assert thermostat.heatingOn == true;
}

test "thermostat turns on AC when hot" {
    var weather = StubWeatherService(95.0);
    var thermostat = Thermostat(weather);

    thermostat.check();

    assert thermostat.coolingOn == true;
}
```

The stub lets us test the thermostat's logic without depending on real weather data. We can simulate any temperature we want.

### Mocks: Verify Interactions

A mock records how it was called, letting you verify interactions:

```text
class MockEmailService implements IEmailService {
    hide sentEmails: List[Email];

    expose func init() {
        self.sentEmails = [];
    }

    func send(email: Email) {
        self.sentEmails.Push(email);
    }

    func wasSent(to: String, subject: String) -> Boolean {
        for email in self.sentEmails {
            if email.to == to && email.subject == subject {
                return true;
            }
        }
        return false;
    }

    func sendCount() -> Integer {
        return self.sentEmails.Length;
    }
}

test "registration sends welcome email" {
    // Arrange
    var emailService = MockEmailService();
    var registration = RegistrationService(emailService);

    // Act
    registration.register("alice@example.com", "password");

    // Assert
    assert emailService.wasSent("alice@example.com", "Welcome!");
}

test "failed registration sends no email" {
    var emailService = MockEmailService();
    var registration = RegistrationService(emailService);

    registration.register("invalid-email", "password");  // Should fail

    assertEqual(emailService.sendCount(), 0);
}
```

Mocks verify not just what your code returns, but how it interacts with its dependencies.

### Fakes: Simplified Implementations

A fake is a simplified but functional implementation:

```text
// Real database: connects to PostgreSQL, handles transactions, etc.
class ProductionDatabase implements IDatabase {
    // ... complex implementation
}

// Fake database: uses in-memory storage
class FakeDatabase implements IDatabase {
    hide data: Map[String, User];

    expose func init() {
        self.data = Map();
    }

    func save(user: User) {
        self.data.Set(user.id, user);
    }

    func findById(id: String) -> User? {
        return self.data.Get(id);
    }

    func delete(id: String) {
        self.data.remove(id);
    }
}

test "user repository can save and retrieve users" {
    var db = FakeDatabase();
    var repo = UserRepository(db);

    var user = User("1", "Alice");
    repo.save(user);

    var retrieved = repo.findById("1");
    assertEqual(retrieved.name, "Alice");
}
```

Fakes behave like the real thing but are simpler and faster. An in-memory database is much faster than a real database and doesn't require setup.

### When to Use Each Type

**Stubs** when you need to control what a dependency returns. "When the weather API says 30 degrees, the thermostat should..."

**Mocks** when you need to verify that your code interacts with a dependency correctly. "Registration should send exactly one welcome email."

**Fakes** when you need realistic behavior without the cost of the real implementation. "Test against an in-memory database instead of PostgreSQL."

---

## Test-Driven Development (TDD)

Test-Driven Development is a discipline where you write tests *before* writing the code they test. It sounds backwards, but it's a powerful technique for producing well-designed, well-tested code.

### The TDD Cycle: Red-Green-Refactor

1. **Red**: Write a failing test for the next piece of functionality
2. **Green**: Write the minimum code needed to make the test pass
3. **Refactor**: Improve the code while keeping tests passing

Then repeat.

### TDD Example: Building isPrime

Let's build an `isPrime` function using TDD.

**Step 1: Red --- Write a failing test**

```text
test "2 is prime" {
    assert isPrime(2);
}
```

This fails because `isPrime` doesn't exist yet.

**Step 2: Green --- Make it pass with minimum code**

```text
func isPrime(n: Integer) -> Boolean {
    return true;  // Minimum code to pass!
}
```

The test passes. Yes, this implementation is wrong, but it passes the test we have.

**Step 3: Red --- Write another failing test**

```text
test "4 is not prime" {
    assert !isPrime(4);
}
```

This fails because our function always returns true.

**Step 4: Green --- Make it pass**

```text
func isPrime(n: Integer) -> Boolean {
    if n == 4 {
        return false;
    }
    return true;
}
```

Both tests pass. Still a silly implementation, but we're being disciplined.

**Step 5: Red --- More tests to drive the design**

```text
test "1 is not prime" {
    assert !isPrime(1);
}

test "0 is not prime" {
    assert !isPrime(0);
}

test "negative numbers are not prime" {
    assert !isPrime(-5);
}
```

These fail because we haven't handled these cases.

**Step 6: Green --- Handle small numbers**

```text
func isPrime(n: Integer) -> Boolean {
    if n < 2 {
        return false;
    }
    if n == 4 {
        return false;
    }
    return true;
}
```

**Step 7: Red --- Test more composites**

```text
test "6 is not prime" {
    assert !isPrime(6);
}

test "9 is not prime" {
    assert !isPrime(9);
}
```

**Step 8: Green --- Generalize**

Time to write a real algorithm:

```text
func isPrime(n: Integer) -> Boolean {
    if n < 2 {
        return false;
    }
    for i in 2..n {
        if n % i == 0 {
            return false;
        }
    }
    return true;
}
```

All tests pass.

**Step 9: Refactor --- Improve without breaking tests**

The algorithm works but is slow. Let's optimize:

```text
bind Zanna.Math as Math;
bind Convert = Zanna.Core.Convert;

func isPrime(n: Integer) -> Boolean {
    if n < 2 { return false; }
    if n == 2 { return true; }
    if n % 2 == 0 { return false; }

    var limit = Convert.NumToInt(Math.Sqrt(n)) + 1;
    for i in 3..limit step 2 {
        if n % i == 0 {
            return false;
        }
    }
    return true;
}
```

Run the tests --- they still pass. The refactoring didn't break anything.

### Why TDD Works

**Forces you to think about design**: You have to know what you're building before you build it. Writing the test first clarifies requirements.

**Guarantees test coverage**: Every piece of code was written to make a test pass, so everything is tested.

**Produces testable code**: Code that's hard to test becomes obvious when you try to write the test first. This pressure leads to better designs.

**Provides instant feedback**: Small cycles mean you know immediately if something breaks.

**Documents intent**: The tests you write show what you intended the code to do, not just what it happens to do.

### TDD Is Not Always Appropriate

TDD works best when:
- Requirements are clear
- You're building well-understood functionality
- The code is logic-heavy (algorithms, business rules)

TDD is less helpful when:
- You're exploring or prototyping
- Requirements are fuzzy
- The code is mostly integration/wiring

Use TDD as a tool, not a religion. Apply it where it helps.

---

## Common Testing Mistakes

Learning to test well means learning what not to do. Here are common mistakes and how to avoid them.

### Testing Implementation, Not Behavior

```text
// Bad: Tests internal details
class Cache {
    hide storage: Map[String, String];
    // ...
}

test "cache uses hashmap internally" {
    var cache = Cache();
    // Trying to verify internal structure - fragile!
}

// Good: Tests observable behavior
test "cache returns stored value" {
    var cache = Cache();
    cache.Set("key", "value");
    assertEqual(cache.Get("key"), "value");
}
```

Test what the code *does*, not *how* it does it. Internal implementation should be free to change without breaking tests.

### Flaky Tests

Flaky tests pass sometimes and fail sometimes:

```text
// Bad: Depends on timing
test "async operation completes" {
    startAsyncOperation();
    Time.Clock.Sleep(100);  // Hope 100ms is enough...
    assert isComplete();  // Sometimes passes, sometimes fails!
}

// Good: Wait properly for completion
test "async operation completes" {
    var future = startAsyncOperation();
    var result = future.await(timeout: 5000);
    assert result.success;
}
```

Other sources of flakiness:
- Depending on test execution order
- Using real current time instead of fake time
- Race conditions in concurrent code
- Depending on external services

Flaky tests erode trust in your test suite. When tests sometimes fail for no reason, people start ignoring failures. Fix flaky tests immediately.

### Tests That Don't Test Anything

```text
// Bad: Doesn't actually verify anything useful
test "create user" {
    var user = User("alice");
    assert user != null;  // This will basically always pass
}

// Good: Verifies meaningful behavior
test "user starts with correct name" {
    var user = User("alice");
    assertEqual(user.name, "alice");
}
```

Every test should verify something that could plausibly fail. If a test can't fail, it's not testing anything.

### Too Many Assertions

```text
// Bad: What exactly failed?
test "everything about user" {
    var user = createUser("alice", 30, "alice@example.com");
    assertEqual(user.name, "alice");
    assertEqual(user.age, 30);
    assertEqual(user.email, "alice@example.com");
    assert user.isActive;
    assert user.createdAt != null;
    assertEqual(user.role, "member");
    assert user.permissions.Length > 0;
    // 15 more assertions...
}

// Good: Focused tests
test "new user has provided name" {
    var user = createUser("alice", 30, "alice@example.com");
    assertEqual(user.name, "alice");
}

test "new user has member role by default" {
    var user = createUser("alice", 30, "alice@example.com");
    assertEqual(user.role, "member");
}
```

When a test with many assertions fails, you only learn one thing failed --- you still have to figure out which one and why. Focused tests give focused feedback.

### Tests That Depend on Each Other

```text
// Bad: Tests depend on shared state
var globalUser: User?;

test "create user" {
    globalUser = User("alice");
}

test "user has name" {
    // Fails if previous test didn't run!
    assertEqual(globalUser.name, "alice");
}

// Good: Each test is independent
test "create user" {
    var user = User("alice");
    assertNotNull(user);
}

test "user has name" {
    var user = User("alice");
    assertEqual(user.name, "alice");
}
```

Tests should be independent --- each should work regardless of what other tests run or in what order. Shared state between tests causes mysterious failures.

### Testing Only the Happy Path

```text
// Incomplete: Only tests success cases
test "withdraw works" {
    var account = BankAccount(100);
    account.withdraw(50);
    assertEqual(account.balance, 50);
}

// Complete: Tests success AND failure cases
test "withdraw reduces balance" {
    var account = BankAccount(100);
    account.withdraw(50);
    assertEqual(account.balance, 50);
}

test "withdraw fails with insufficient funds" {
    var account = BankAccount(100);
    var result = account.withdraw(150);
    assert !result.success;
    assertEqual(account.balance, 100);  // Balance unchanged
}

test "withdraw fails with negative amount" {
    var account = BankAccount(100);
    var result = account.withdraw(-50);
    assert !result.success;
}
```

Error paths are just as important as success paths. Maybe more --- bugs in error handling cause data corruption and security issues.

---

## Code Coverage

Code coverage measures how much of your code is executed by your tests.

```bash
./scripts/coverage.sh
```

Output:

```text
File                  Statements    Branches    Coverage
--------------------------------------------------------------
math.zia            45/50         12/15       87%
user.zia            30/32         8/10        91%
utils.zia           20/40         5/12        52%
--------------------------------------------------------------
Total                 95/122        25/37       78%
```

For the Zanna repository itself, `./scripts/coverage.sh` configures a coverage build, runs CTest, and writes reports under `coverage/`.

### What Coverage Tells You

**Statements covered**: How many lines of code were executed during tests.

**Branches covered**: How many decision paths (if/else, match cases) were taken.

### What Coverage Doesn't Tell You

High coverage doesn't mean good tests. Consider:

```text
func divide(a: Integer, b: Integer) -> Integer {
    return a / b;
}

test "divide executes" {
    divide(10, 2);  // 100% coverage!
}
```

This test has 100% coverage but doesn't verify the result is correct. It doesn't test division by zero. Coverage measures what code ran, not whether the tests are meaningful.

### Healthy Coverage Targets

- **80-90%** is a reasonable target for most projects
- **100%** is often impractical and not worth the cost
- Below **60%** suggests significant untested code

More important than a specific number:
- Critical paths should be thoroughly tested
- Error handling should be tested
- Edge cases should be tested

Coverage is a tool for finding untested code, not a goal in itself.

### Using Coverage to Find Gaps

Low coverage in a file suggests tests are missing. Look at the uncovered lines:

```text
utils.zia: 52% coverage

Uncovered lines:
  45-52: Error handling for file not found
  67-71: Edge case for empty input
  89-95: Cleanup code in finally block
```

This tells you exactly what to test next.

---

## Debugging with Tests

Tests are powerful debugging tools. When you find a bug, write a test that reproduces it before fixing it.

### The Bug-First Testing Workflow

1. **Receive bug report**: "Dividing 7 by 3 returns 3 instead of 2.33"

2. **Write a test that fails**:
```text
test "divide returns correct result for 7/3" {
    assertClose(divide(7, 3), 2.33, 0.01);
}
```

3. **Run the test to confirm it fails**: This proves you've reproduced the bug.

4. **Fix the bug**: Make the code changes.

5. **Run the test to confirm it passes**: This proves the bug is fixed.

6. **Run all tests**: This proves you didn't break anything else.

### Why Write the Test First?

- **Proves you understand the bug**: If you can't write a failing test, do you really understand the problem?
- **Prevents regressions**: The test stays in your suite forever, preventing this bug from returning.
- **Documents the fix**: The test explains what went wrong and what the correct behavior is.

### Using Tests to Isolate Bugs

When you have a complex bug, write increasingly focused tests to narrow down the problem:

```text
// Start broad: Does the full workflow fail?
test "user registration fails" {
    var result = registerUser("alice@example.com", "password");
    assert result.success;  // FAILS
}

// Narrow down: Is it the email validation?
test "email validation accepts valid email" {
    assert isValidEmail("alice@example.com");  // PASSES
}

// Narrow down: Is it the password hashing?
test "password hashing works" {
    var hash = hashPassword("password");
    assert verifyPassword("password", hash);  // PASSES
}

// Narrow down: Is it the database insert?
test "database insert works" {
    var db = TestDatabase.create();
    var user = User("alice@example.com");
    db.Insert(user);  // FAILS - Found it!
}
```

Each test eliminates possibilities until you find the culprit.

---

## Property-Based Testing

Instead of testing specific examples, property-based testing checks that properties hold for many randomly generated inputs.

```text
bind Zanna.Test;

test "reversing twice returns original" {
    for i in 0..100 {
        var original = generateRandomString();
        var result = original.Reverse().Reverse();
        assertEqual(result, original);
    }
}

test "sorting is idempotent" {
    for i in 0..100 {
        var list = generateRandomIntList();
        var sortedOnce = sort(list);
        var sortedTwice = sort(sortedOnce);
        assertEqual(sortedOnce, sortedTwice);
    }
}

test "sorted list is ordered" {
    for i in 0..100 {
        var list = generateRandomIntList();
        var sorted = sort(list);

        for j in 0..(sorted.Length - 1) {
            assert sorted[j] <= sorted[j + 1];
        }
    }
}

test "sorted list has same elements" {
    for i in 0..100 {
        var list = generateRandomIntList();
        var sorted = sort(list);

        assertEqual(sorted.Length, list.Length);
        for item in list {
            assertContains(sorted, item);
        }
    }
}
```

Property-based testing often finds edge cases you wouldn't think to test manually.

---

## Step-by-Step Example: Building with Tests

Let's build a `StringCalculator` class using tests to guide our development.

### Requirements

Create a calculator that:
1. Takes a string of comma-separated numbers
2. Returns their sum
3. Handles empty strings (returns 0)
4. Handles newlines as separators too
5. Throws an error for negative numbers

### Step 1: Empty String Returns Zero

```text
test "empty string returns 0" {
    var calc = StringCalculator();
    assertEqual(calc.Add(""), 0);
}
```

Implementation:

```text
class StringCalculator {
    expose func init() {}

    func add(numbers: String) -> Integer {
        if numbers == "" {
            return 0;
        }
        return 0;  // Placeholder
    }
}
```

### Step 2: Single Number Returns That Number

```text
test "single number returns that number" {
    var calc = StringCalculator();
    assertEqual(calc.Add("5"), 5);
    assertEqual(calc.Add("42"), 42);
}
```

Implementation:

```text
func add(numbers: String) -> Integer {
    if numbers == "" {
        return 0;
    }
    return Convert.ToInt64(numbers);
}
```

### Step 3: Two Numbers Returns Sum

```text
test "two numbers returns sum" {
    var calc = StringCalculator();
    assertEqual(calc.Add("1,2"), 3);
    assertEqual(calc.Add("10,20"), 30);
}
```

Implementation:

```text
func add(numbers: String) -> Integer {
    if numbers == "" {
        return 0;
    }

    var parts = numbers.Split(",");
    var sum = 0;
    for part in parts {
        sum += Convert.ToInt64(part);
    }
    return sum;
}
```

### Step 4: Multiple Numbers

```text
test "multiple numbers returns sum" {
    var calc = StringCalculator();
    assertEqual(calc.Add("1,2,3"), 6);
    assertEqual(calc.Add("1,2,3,4,5"), 15);
}
```

The implementation already handles this! The test passes.

### Step 5: Newlines as Separators

```text
test "newlines work as separators" {
    var calc = StringCalculator();
    assertEqual(calc.Add("1\n2,3"), 6);
    assertEqual(calc.Add("1\n2\n3"), 6);
}
```

Implementation:

```text
func add(numbers: String) -> Integer {
    if numbers == "" {
        return 0;
    }

    // Replace newlines with commas
    var normalized = numbers.Replace("\n", ",");
    var parts = normalized.Split(",");
    var sum = 0;
    for part in parts {
        sum += Convert.ToInt64(part);
    }
    return sum;
}
```

### Step 6: Negative Numbers Throw

```text
test "negative numbers throw exception" {
    var calc = StringCalculator();

    assertThrows(func() {
        calc.Add("-1,2");
    });
}

test "exception message includes negative number" {
    var calc = StringCalculator();

    var caught = false;
    try {
        calc.Add("-1,2,-3");
    } catch(e) {
        caught = true;
        assertContains(e.message, "-1");
        assertContains(e.message, "-3");
    }
    assert caught;
}
```

Implementation:

```text
func add(numbers: String) -> Integer {
    if numbers == "" {
        return 0;
    }

    var normalized = numbers.Replace("\n", ",");
    var parts = normalized.Split(",");
    var negatives: List[Integer] = [];
    var sum = 0;

    for part in parts {
        var n = Convert.ToInt64(part);
        if n < 0 {
            negatives.Push(n);
        }
        sum += n;
    }

    if negatives.Length > 0 {
        var message = "Negatives not allowed: ";
        for i, neg in negatives {
            if i > 0 { message += ", "; }
            message += Fmt.Int(neg);
        }
        throw (message);
    }

    return sum;
}
```

### The Complete Solution

```text
class StringCalculator {
    expose func init() {}

    func add(numbers: String) -> Integer {
        if numbers == "" {
            return 0;
        }

        var normalized = numbers.Replace("\n", ",");
        var parts = normalized.Split(",");
        var negatives: List[Integer] = [];
        var sum = 0;

        for part in parts {
            var n = Convert.ToInt64(part.Trim());
            if n < 0 {
                negatives.Push(n);
            }
            sum += n;
        }

        if negatives.Length > 0 {
            var message = "Negatives not allowed: ";
            for i, neg in negatives {
                if i > 0 { message += ", "; }
                message += Fmt.Int(neg);
            }
            throw (message);
        }

        return sum;
    }
}
```

Notice how the tests guided us to a clean, well-designed implementation. Each test added a specific requirement, and we implemented just enough to satisfy it.

---

## Testing Patterns and Best Practices

### One Concept Per Test

Each test should verify one behavior:

```text
// Bad: Testing multiple concepts
test "user operations" {
    var user = User("alice");
    assertEqual(user.name, "alice");

    user.setEmail("alice@example.com");
    assertEqual(user.email, "alice@example.com");

    user.deactivate();
    assert !user.isActive;
}

// Good: Separate tests for separate concepts
test "user has provided name" {
    var user = User("alice");
    assertEqual(user.name, "alice");
}

test "setEmail updates email" {
    var user = User("alice");
    user.setEmail("alice@example.com");
    assertEqual(user.email, "alice@example.com");
}

test "deactivate makes user inactive" {
    var user = User("alice");
    user.deactivate();
    assert !user.isActive;
}
```

### Keep Tests Independent

Tests should not share state or depend on execution order:

```text
// Bad: Tests share state
var sharedList: List[Integer] = [];

test "add to list" {
    sharedList.Push(1);
    assertEqual(sharedList.Length, 1);
}

test "list has item" {
    // Depends on previous test!
    assertEqual(sharedList[0], 1);
}

// Good: Each test creates its own state
test "add to list" {
    var list: List[Integer] = [];
    list.Push(1);
    assertEqual(list.Length, 1);
}

test "list contains added item" {
    var list: List[Integer] = [];
    list.Push(1);
    assertEqual(list[0], 1);
}
```

### Test Behavior, Not Implementation

```text
// Bad: Tests how sorting works internally
test "sort uses quicksort" {
    // Trying to verify algorithm choice - fragile!
}

// Good: Tests that sorting produces correct result
test "sort puts elements in ascending order" {
    var list = [3, 1, 4, 1, 5, 9, 2, 6];
    var sorted = sort(list);

    for i in 0..(sorted.Length - 1) {
        assert sorted[i] <= sorted[i + 1];
    }
}
```

### Make Tests Readable

Tests are documentation. Make them clear:

```text
// Hard to understand
test "test1" {
    var x = f(1, 2, 3);
    assert x == 6;
}

// Clear and readable
test "sumOfList returns total of all elements" {
    var numbers = [1, 2, 3];
    var total = sumOfList(numbers);
    assertEqual(total, 6);
}
```

---

## The Two Languages

**Zia**

```text
bind Zanna.Test;

test "example test" {
    assert 1 + 1 == 2;
    assertEqual(actual, expected);
    assertThrows(func() {
        riskyCode();
    });
}
```

**BASIC**

```text
TEST "example test"
    ASSERT 1 + 1 = 2
    ASSERT_EQUAL actual, expected
    ASSERT_THROWS RiskyCode
END TEST
```

---

## Summary

Testing is an investment that pays dividends throughout your project's life:

- **Tests verify behavior**: Catch bugs before users do, automatically, every time
- **Tests enable change**: Refactor and improve code with confidence
- **Tests document intent**: Show how code should be used through working examples
- **Arrange-Act-Assert**: Structure tests clearly and consistently
- **Test the right things**: Normal cases, edge cases, error cases
- **Test doubles**: Stubs, mocks, and fakes isolate code from dependencies
- **TDD**: Writing tests first leads to better design and guaranteed coverage
- **Code coverage**: A tool for finding gaps, not a goal in itself
- **Property-based testing**: Check that properties hold across many inputs
- **Debug with tests**: Reproduce bugs as tests before fixing them

The time you spend writing tests saves far more time in debugging, maintenance, and confident development. Testing transforms programming from a scary exercise in hoping things work into an engineering discipline where you *know* things work.

---

## Exercises

**Exercise 27.1 (Mimic)**: Write tests for a `StringUtils` class with these methods:
- `reverse(s: String) -> String`: Returns the string reversed
- `capitalize(s: String) -> String`: Capitalizes the first letter
- `isPalindrome(s: String) -> Boolean`: Returns true if the string reads the same forwards and backwards

Test normal cases, edge cases (empty string, single character), and various inputs.

**Exercise 27.2 (Extend)**: Create a `BankAccount` class with `deposit`, `withdraw`, and `transfer` methods. Write comprehensive tests including:
- Normal deposits and withdrawals
- Edge cases (zero amounts, exact balance withdrawals)
- Error cases (negative amounts, overdrafts)
- Transfer between accounts (including insufficient funds)

**Exercise 27.3 (Create)**: Write tests for a sorting function using property-based testing. Your tests should verify:
- The result is sorted (each element <= the next)
- The result has the same length as the input
- The result contains exactly the same elements as the input
- Sorting twice gives the same result as sorting once

**Exercise 27.4 (Create)**: Create a mock for a file system and use it to test a `LogRotator` class. The LogRotator should:
- Check if a log file exceeds a size limit
- Rename the old file with a timestamp
- Create a new empty log file

Your mock file system should track which operations were called without actually creating files.

**Exercise 27.5 (TDD)**: Practice TDD by building a `ShoppingCart` class. Write tests first, then implement:
1. Cart starts empty
2. Adding an item increases the count
3. Removing an item decreases the count
4. Total price reflects all items
5. Cannot add negative quantities
6. Removing non-existent item does nothing
7. Clearing empties the cart

**Exercise 27.6 (Integration)**: Write integration tests for a simple user authentication system. Create:
- `UserRepository`: Stores users (use a fake in-memory database)
- `PasswordHasher`: Hashes and verifies passwords
- `AuthService`: Handles login/logout

Test that these components work together correctly for login success, login failure, and logout scenarios.

**Exercise 27.7 (Debug)**: Here's a buggy function. Write tests that expose the bugs, then fix them:

```text
func calculateGrade(score: Integer) -> String {
    if score >= 90 { return "A"; }
    if score >= 80 { return "B"; }
    if score >= 70 { return "C"; }
    if score >= 60 { return "D"; }
    return "F";
}
```

Hint: What happens with scores above 100? Below 0? Exactly on boundaries?

**Exercise 27.8 (Coverage)**: Take a project you've built in earlier chapters and add tests until you achieve 80% code coverage. Use coverage reports to identify untested code paths.

**Exercise 27.9 (Challenge)**: Build a simple test framework. It should:
- Allow defining tests with names
- Run all tests and collect results
- Report which tests passed and failed
- Show failure messages
- Support setup and teardown

**Exercise 27.10 (Challenge)**: Create a `MockClock` that you can control in tests. Use it to test a `SessionManager` that expires sessions after 30 minutes. You should be able to "advance time" in your tests without actually waiting.

---

*Your code works and is tested. You have confidence it's correct and will stay correct. Now let's think bigger --- how do you organize a large system into maintainable pieces? Next chapter: Architecture.*

*[Continue to Chapter 28: Architecture](28-architecture.md)*
