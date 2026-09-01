---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 28: Software Architecture

You have written a program. Maybe a few hundred lines. It works. You understand every piece because you wrote it all, and it fits in your head.

Then it grows.

A colleague adds a feature. You add another. Someone fixes a bug by copying code from elsewhere. The program becomes a thousand lines, then five thousand, then ten thousand. New developers join. Nobody remembers why certain decisions were made. Changing one thing breaks three others. Adding features takes longer and longer. Eventually, everyone dreads touching the code.

This is the story of almost every software project that succeeds long enough to become complex. And it does not have to end this way.

Software architecture is the art of organizing code so that large systems remain understandable, changeable, and reliable. It is perhaps the most important skill separating programmers who can write code from programmers who can build systems.

This chapter teaches you to think architecturally.

---

## Why Architecture Matters

When you write a small program, you can hold the entire thing in your head. You know where everything is. You understand how each piece connects to every other piece. Making changes is easy because you see the whole picture.

But human brains have limits. Studies suggest we can hold roughly seven things in working memory at once. A program with hundreds of functions and dozens of files exceeds this limit by orders of magnitude. Without structure, we cannot reason about the system.

Architecture provides that structure. It gives us a way to think about large systems by breaking them into manageable pieces with clear relationships. Instead of thinking about thousands of lines of code, you think about a handful of components, each with a defined responsibility.

**Good architecture buys you:**

- **Understanding**: New team members can learn the system piece by piece
- **Changeability**: Modifications stay local rather than rippling everywhere
- **Testability**: Components can be verified independently
- **Parallelism**: Different developers can work on different areas simultaneously
- **Longevity**: The system can evolve over years without collapsing

**Poor architecture costs you:**

- Every change requires understanding the whole system
- Bug fixes create new bugs elsewhere
- Testing requires running everything together
- New features take exponentially longer to add
- Developers become afraid to modify anything
- Eventually, rewriting from scratch seems easier than continuing

The difference between a program that lives for decades and one that becomes unmaintainable in two years is usually architecture.

---

## Mental Models for Architecture

Architecture is an abstract concept. It helps to have concrete analogies.

### The Building Blueprint

Imagine constructing a house without a plan. You start with the kitchen because you know you need one. You add a bedroom next to it. Then you realize you forgot the bathroom, so you squeeze it in wherever it fits. The living room goes wherever there is space left.

The result would be a functional disaster. Rooms in illogical places. Plumbing running bizarre routes. No natural flow through the space.

Real buildings start with blueprints. Architects think about how people will use the space before construction begins. They group related functions (all bathrooms near each other for plumbing efficiency). They plan traffic patterns (front door leads to common areas, not bedrooms). They ensure the structure can bear loads (foundation before walls).

Software architecture serves the same purpose. Before writing code, you think about how pieces will interact. You group related functionality. You plan how data flows through the system. You ensure the foundation can support what you will build on top.

### The City Plan

Cities offer another illuminating analogy. A well-planned city has distinct districts: residential neighborhoods, commercial zones, industrial areas. Roads connect districts in sensible ways. Utilities follow logical routes. Zoning laws prevent factories next to schools.

Now imagine a city that grew without planning. Houses mixed randomly with warehouses. No coherent street grid. Sewers connected haphazardly. Traffic jams everywhere because nobody thought about flow.

Many software systems resemble unplanned cities. They grow organically, adding whatever is needed wherever it fits at the moment. Over time, navigating them becomes nightmarish.

Architectural thinking brings urban planning to software. You designate districts (modules) with clear purposes. You plan the roads between them (interfaces and dependencies). You establish zoning laws (rules about what belongs where).

### The Restaurant Kitchen

Professional kitchens offer a smaller-scale analogy. A well-organized kitchen has stations: one for grilling, one for sauces, one for salads, one for plating. Each station has everything it needs. Chefs at different stations work simultaneously without collision. Food flows in one direction, from preparation through cooking to plating.

Contrast this with a home kitchen where one person does everything, grabbing ingredients from wherever, using the same counter for prep and plating, constantly walking back and forth.

The home kitchen works for simple meals. It fails completely for a restaurant serving hundreds of customers.

Software systems face the same scaling challenge. Approaches that work for small programs fail catastrophically for large ones. Architecture introduces the equivalent of kitchen stations: specialized areas with defined responsibilities and clear handoffs.

---

## Signs You Need Better Architecture

How do you know when architecture problems are holding you back? Here are warning signs:

### The Ripple Effect

You change one small thing, and it breaks something seemingly unrelated. Fixing the user display breaks the reporting system. Updating the email format crashes the dashboard. Everything seems connected to everything else.

This happens when code lacks clear boundaries. Components reach into each other's internals. A change anywhere can affect anything.

### The Fear Factor

Developers are afraid to modify certain areas. "Don't touch the order processing code" becomes tribal knowledge. People work around problems rather than fixing them because fixing seems too risky.

Fear indicates that code is fragile and poorly understood. Good architecture makes changes safe and localized.

### The Knowledge Silo

Only one person understands how certain parts work. When they are unavailable, those areas cannot be modified. The team dreads what happens when that person leaves.

This suggests the code lacks structure that communicates its organization. Well-architected systems are understandable from their structure, not just from tribal knowledge.

### The Feature Slowdown

Early features shipped quickly. Now every feature takes longer than the last. Simple additions require weeks of work. The system actively resists change.

Complexity compounds without architecture. Each new feature must navigate an increasingly tangled web of dependencies.

### The Testing Nightmare

Testing requires spinning up the entire system. You cannot test pieces in isolation. Test suites take hours to run because everything depends on everything.

Good architecture enables testing components independently. When testing requires the whole system, boundaries are insufficient.

### The Duplicate Code Explosion

Similar code appears throughout the system. Bug fixes must be applied in multiple places. Features are reimplemented because nobody knows similar code already exists.

This indicates poor organization. Related functionality should live together, easy to find and reuse.

---

## Core Architectural Principles

Several principles guide good architectural decisions. They are not rigid rules but lenses for evaluating designs.

### Separation of Concerns

Each piece of your system should do one thing. When you describe a component, you should be able to state its purpose in a single sentence without using "and."

Consider this problematic code:

```zia
// Bad: Everything mixed together
class UserManager {
    func registerUser(email: String, password: String) {
        // Validate email format
        if !email.Contains("@") || !email.Contains(".") {
            showError("Invalid email");
            return;
        }

        // Check password strength
        if password.Length < 8 {
            showError("Password too short");
            return;
        }

        // Hash password
        var hash = "";
        var i = 0;
        while i < password.Length {
            hash = hash + Str.Chr(Str.Asc(Str.MidLen(password, i, 1)) * 17 % 128);
            i = i + 1;
        }

        // Save to database
        var db = Database.connect("localhost:5432");
        db.query("INSERT INTO users (email, password_hash) VALUES (?, ?)",
                 [email, hash]);
        db.Close();

        // Send welcome email
        var smtp = SMTP.connect("mail.server.com");
        smtp.send(
            to: email,
            subject: "Welcome!",
            body: "Thanks for registering..."
        );
        smtp.Close();

        // Update UI
        userList.refresh();
        showSuccess("User created!");
    }
}
```

This function does six different things: validates, hashes passwords, accesses the database, sends email, and updates the UI. Changing any concern requires modifying this function. Testing requires all dependencies. The function is impossible to reuse.

Now consider the separated version:

```zia
// Good: Separate concerns
class EmailValidator {
    func validate(email: String) -> ValidationResult {
        if !email.Contains("@") {
            return ValidationResult.invalid("Missing @ symbol");
        }
        if !email.Contains(".") {
            return ValidationResult.invalid("Missing domain");
        }
        return ValidationResult.valid();
    }
}

class PasswordValidator {
    func validate(password: String) -> ValidationResult {
        if password.Length < 8 {
            return ValidationResult.invalid("Must be at least 8 characters");
        }
        return ValidationResult.valid();
    }
}

class PasswordHasher {
    func hash(password: String) -> String {
        return Crypto.sha256(password);
    }
}

class UserRepository {
    hide db: Database;

    expose func init(db: Database) {
        self.db = db;
    }

    func save(user: User) {
        self.db.query(
            "INSERT INTO users (email, password_hash) VALUES (?, ?)",
            [user.email, user.passwordHash]
        );
    }
}

class WelcomeEmailSender {
    hide smtp: SMTPClient;

    expose func init(smtp: SMTPClient) {
        self.smtp = smtp;
    }

    func send(email: String) {
        self.smtp.send(
            to: email,
            subject: "Welcome!",
            body: "Thanks for registering..."
        );
    }
}

class RegistrationService {
    hide emailValidator: EmailValidator;
    hide passwordValidator: PasswordValidator;
    hide hasher: PasswordHasher;
    hide repository: UserRepository;
    hide welcomeEmail: WelcomeEmailSender;

    func register(email: String, password: String) -> RegistrationResult {
        // Validate
        var emailResult = self.emailValidator.validate(email);
        if !emailResult.isValid {
            return RegistrationResult.failure(emailResult.message);
        }

        var passwordResult = self.passwordValidator.validate(password);
        if !passwordResult.isValid {
            return RegistrationResult.failure(passwordResult.message);
        }

        // Create user
        var hash = self.hasher.hash(password);
        var user = User(email: email, passwordHash: hash);

        // Persist
        self.repository.save(user);

        // Notify
        self.welcomeEmail.send(email);

        return RegistrationResult.success(user);
    }
}
```

Each class has one job. The `EmailValidator` only validates emails. The `PasswordHasher` only hashes passwords. The `UserRepository` only handles database operations.

This is more code, but consider the benefits:

- Change password hashing? Modify only `PasswordHasher`.
- Test email validation? No database needed.
- Reuse validation elsewhere? Just use the validators.
- Switch databases? Replace only the repository.

Separation of concerns is the foundation of all good architecture.

### Single Responsibility Principle

Closely related to separation of concerns, the single responsibility principle states that each component should have exactly one reason to change.

Ask yourself: "What could change in the future that would require modifying this code?"

If you can list multiple unrelated reasons, the component has too many responsibilities.

```zia
// Bad: Multiple reasons to change
class Report {
    func gatherData() { ... }      // Changes if data sources change
    func calculate() { ... }        // Changes if business rules change
    func formatHtml() { ... }       // Changes if HTML design changes
    func formatPdf() { ... }        // Changes if PDF design changes
    func sendEmail() { ... }        // Changes if email system changes
    func saveToFile() { ... }       // Changes if file storage changes
}
```

This class changes if data sources change, business rules change, HTML design changes, PDF design changes, email system changes, or file storage changes. Six unrelated reasons.

```zia
// Good: Single responsibility each
class ReportDataGatherer {
    func gather(sources: List[DataSource]) -> RawData { ... }
}

class ReportCalculator {
    func calculate(data: RawData) -> ReportResults { ... }
}

class HtmlReportFormatter {
    func format(results: ReportResults) -> String { ... }
}

class PdfReportFormatter {
    func format(results: ReportResults) -> PdfDocument { ... }
}

class ReportEmailer {
    func send(report: String, recipients: List[String]) { ... }
}

class ReportFileSaver {
    func save(report: String, path: String) { ... }
}
```

Now each class has exactly one reason to change.

### Dependency Inversion

High-level components should not depend on low-level components. Both should depend on abstractions.

This principle sounds abstract. Let us make it concrete.

```zia
// Bad: High-level depends on low-level
class OrderProcessor {
    hide database: MySQLDatabase;  // Concrete dependency
    hide paymentGateway: StripePayment;  // Concrete dependency
    hide emailer: SendGridEmail;  // Concrete dependency

    expose func init() {
        self.database = MySQLDatabase("localhost:3306");
        self.paymentGateway = StripePayment("sk_live_xxx");
        self.emailer = SendGridEmail("api_key_xxx");
    }

    func processOrder(order: Order) {
        self.database.save(order);
        self.paymentGateway.charge(order.total);
        self.emailer.sendConfirmation(order.customerEmail);
    }
}
```

The `OrderProcessor` is welded to MySQL, Stripe, and SendGrid. Testing requires those services. Switching databases requires rewriting `OrderProcessor`. The high-level policy (how to process orders) is entangled with low-level details (which database to use).

```zia
// Good: Depend on abstractions
interface IOrderRepository {
    func save(order: Order);
    func findById(id: String) -> Order?;
}

interface IPaymentGateway {
    func charge(amount: Number, customerId: String) -> PaymentResult;
}

interface IEmailService {
    func send(to: String, subject: String, body: String);
}

class OrderProcessor {
    hide repository: IOrderRepository;
    hide payment: IPaymentGateway;
    hide email: IEmailService;

    expose func init(
        repository: IOrderRepository,
        payment: IPaymentGateway,
        email: IEmailService
    ) {
        self.repository = repository;
        self.payment = payment;
        self.email = email;
    }

    func processOrder(order: Order) -> ProcessResult {
        self.repository.save(order);

        var paymentResult = self.payment.charge(order.total, order.customerId);
        if !paymentResult.success {
            return ProcessResult.failure(paymentResult.error);
        }

        self.email.send(
            order.customerEmail,
            "Order Confirmed",
            "Your order #" + order.id + " is confirmed."
        );

        return ProcessResult.success(order);
    }
}
```

Now `OrderProcessor` depends on interfaces, not concrete implementations. You can:

- Test with mock implementations
- Switch from MySQL to PostgreSQL without changing `OrderProcessor`
- Use different payment gateways for different regions
- Replace email service without touching business logic

The dependency arrow has been inverted. Instead of `OrderProcessor` depending on `MySQLDatabase`, both depend on `IOrderRepository`.

### Interface Segregation

Clients should not be forced to depend on methods they do not use. Prefer small, focused interfaces over large, general ones.

```zia
// Bad: Bloated interface
interface IEmployee {
    func getName() -> String;
    func getSalary() -> Number;
    func getOfficeLocation() -> String;
    func getRemoteWorkSchedule() -> Schedule;
    func getEquipmentAssigned() -> List[Equipment];
    func getParkingSpot() -> String;
    func getHealthBenefits() -> Benefits;
    func getStockOptions() -> List[StockGrant];
}
```

A payroll system only needs `getName()` and `getSalary()`. But it must accept an interface with eight methods. If `getStockOptions()` changes, the payroll system might need recompilation even though it never uses stock options.

```text
// Good: Segregated interfaces
interface IIdentifiable {
    func getName() -> String;
    func getId() -> String;
}

interface ICompensated {
    func getSalary() -> Number;
    func getBonus() -> Number;
}

interface ILocatable {
    func getOfficeLocation() -> String;
    func getParkingSpot() -> String;
}

interface IBenefited {
    func getHealthBenefits() -> Benefits;
    func getStockOptions() -> List[StockGrant];
}

// Payroll only needs what it uses
class PayrollCalculator {
    func calculatePay(employee: IIdentifiable & ICompensated) -> Number {
        return employee.getSalary() + employee.getBonus();
    }
}
```

Now the payroll system depends only on the interfaces it needs.

---

## Common Architectural Patterns

Patterns are proven solutions to recurring design problems. They give you vocabulary to discuss architecture and blueprints to follow.

### Layered Architecture

The most common pattern organizes code into horizontal layers, each with a distinct responsibility:

```text
┌─────────────────────────────────────────────────────────────┐
│                    Presentation Layer                        │
│            (User interface, API endpoints, CLI)              │
├─────────────────────────────────────────────────────────────┤
│                    Application Layer                         │
│           (Use cases, workflows, orchestration)              │
├─────────────────────────────────────────────────────────────┤
│                      Domain Layer                            │
│          (Business logic, classes, rules)                   │
├─────────────────────────────────────────────────────────────┤
│                   Infrastructure Layer                       │
│         (Database, file system, external APIs)               │
└─────────────────────────────────────────────────────────────┘
```

**The strict rule**: upper layers can use lower layers, but lower layers never use upper layers. The domain layer has no idea there is a user interface. The infrastructure layer does not know about use cases.

Why this rule? It protects your core business logic from changes in presentation or infrastructure. You can replace your web UI with a mobile app without touching business rules. You can switch databases without changing how orders are processed.

**Presentation Layer** handles all user interaction. It accepts input, displays output, and translates between user-facing formats and application formats. It contains no business logic.

**Application Layer** orchestrates use cases. It knows what steps to take but not how to perform them. "To place an order: validate the cart, process payment, create the order, send confirmation." The layer coordinates but delegates the actual work.

**Domain Layer** contains your business rules. What makes an order valid? How is pricing calculated? What constraints exist? This is the heart of your application, independent of how users interact with it or where data is stored.

**Infrastructure Layer** handles external systems. Databases, file systems, email services, third-party APIs. It implements the interfaces defined by upper layers.

Example folder structure:

```text
project/
├── presentation/
│   ├── web/
│   │   ├── OrderController.zia
│   │   └── ProductController.zia
│   ├── api/
│   │   └── RestApi.zia
│   └── cli/
│       └── AdminCommands.zia
│
├── application/
│   ├── PlaceOrderUseCase.zia
│   ├── CancelOrderUseCase.zia
│   └── SearchProductsUseCase.zia
│
├── domain/
│   ├── classes/
│   │   ├── Order.zia
│   │   ├── Product.zia
│   │   └── Customer.zia
│   ├── services/
│   │   ├── PricingService.zia
│   │   └── InventoryService.zia
│   └── repositories/
│       ├── IOrderRepository.zia
│       └── IProductRepository.zia
│
└── infrastructure/
    ├── persistence/
    │   ├── SqlOrderRepository.zia
    │   └── SqlProductRepository.zia
    ├── payment/
    │   └── StripePaymentGateway.zia
    └── email/
        └── SmtpEmailService.zia
```

### Model-View-Controller (MVC)

MVC separates user interface applications into three components:

```text
                    ┌───────────────┐
        User        │               │
       Interacts ──►│     View      │
                    │  (displays)   │
                    └───────┬───────┘
                            │ user events
                    ┌───────▼───────┐
                    │               │
                    │  Controller   │
                    │  (handles)    │
                    └───────┬───────┘
                            │ updates
                    ┌───────▼───────┐
                    │               │
                    │    Model      │◄─── state
                    │ (data/logic)  │     changes
                    └───────────────┘
```

**Model** contains data and business logic. It knows nothing about how it is displayed.

**View** displays the model to users. It knows how to render data but not what it means.

**Controller** handles user input and coordinates between model and view. It translates user actions into model updates and triggers view refreshes.

```text
bind Convert = Zanna.Core.Convert;
bind Zanna.Text.Fmt as Fmt;

// Model: Business logic and data
class TodoList {
    hide items: List[TodoItem];
    hide nextId: Integer;

    expose func init() {
        self.items = [];
        self.nextId = 1;
    }

    func addItem(text: String) -> TodoItem {
        var item = TodoItem(id: self.nextId, text: text, done: false);
        self.nextId = self.nextId + 1;
        self.items.Push(item);
        return item;
    }

    func toggleItem(id: Integer) {
        for item in self.items {
            if item.id == id {
                item.done = !item.done;
                return;
            }
        }
    }

    func removeItem(id: Integer) {
        self.items = self.items.Filter(item => item.id != id);
    }

    func getItems() -> List[TodoItem] {
        return self.items.clone();
    }

    func getActiveCount() -> Integer {
        return self.items.Filter(item => !item.done).Length;
    }
}

// View: Display only
class TodoView {
    hide listElement: HTMLElement;
    hide inputElement: HTMLInputElement;
    hide countElement: HTMLElement;

    func render(items: List[TodoItem], activeCount: Integer) {
        self.listElement.Clear();

        for item in items {
            var li = HTML.createElement("li");
            li.setText(item.text);
            if item.done {
                li.addClass("completed");
            }
            li.setAttribute("data-id", Fmt.Int(item.id));
            self.listElement.appendChild(li);
        }

        self.countElement.setText(Fmt.Int(activeCount) + " items left");
    }

    func bindAddItem(handler: func(String)) {
        self.inputElement.onEnter(func() {
            var text = self.inputElement.getValue();
            if text.Length > 0 {
                handler(text);
                self.inputElement.Clear();
            }
        });
    }

    func bindToggleItem(handler: func(Integer)) {
        self.listElement.onClick(func(event: Event) {
            var id = Convert.ToInt64(event.target.getAttribute("data-id"));
            handler(id);
        });
    }

    func bindDeleteItem(handler: func(Integer)) {
        // Similar binding for delete buttons
    }
}

// Controller: Coordinates model and view
class TodoController {
    hide model: TodoList;
    hide view: TodoView;

    expose func init(model: TodoList, view: TodoView) {
        self.model = model;
        self.view = view;

        // Bind user actions to model updates
        self.view.bindAddItem(self.handleAdd);
        self.view.bindToggleItem(self.handleToggle);
        self.view.bindDeleteItem(self.handleDelete);

        // Initial render
        self.updateView();
    }

    func handleAdd(text: String) {
        self.model.addItem(text);
        self.updateView();
    }

    func handleToggle(id: Integer) {
        self.model.toggleItem(id);
        self.updateView();
    }

    func handleDelete(id: Integer) {
        self.model.removeItem(id);
        self.updateView();
    }

    func updateView() {
        self.view.render(
            self.model.getItems(),
            self.model.getActiveCount()
        );
    }
}
```

MVC keeps each component focused. The model can be tested without a UI. The view can be redesigned without changing logic. The controller can be modified without touching either.

### Clean Architecture

Clean architecture extends layered architecture with a strict dependency rule: dependencies always point inward, toward the center.

```text
┌──────────────────────────────────────────────────────────────┐
│                      Frameworks & Drivers                     │
│              (Web, Database, UI, External APIs)               │
│    ┌────────────────────────────────────────────────────┐    │
│    │              Interface Adapters                     │    │
│    │        (Controllers, Gateways, Presenters)         │    │
│    │    ┌────────────────────────────────────────┐      │    │
│    │    │           Application Business Rules    │      │    │
│    │    │              (Use Cases)                │      │    │
│    │    │    ┌────────────────────────────┐      │      │    │
│    │    │    │   Enterprise Business Rules │      │      │    │
│    │    │    │        (Entities)           │      │      │    │
│    │    │    └────────────────────────────┘      │      │    │
│    │    └────────────────────────────────────────┘      │    │
│    └────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────┘
                    Dependencies point inward →
```

The innermost circle contains classes: core business objects and rules that would exist even without computers. A bank has accounts and transactions whether or not software exists.

The next circle contains use cases: application-specific business rules. "Transfer money between accounts" is a use case. It orchestrates classes.

Interface adapters convert between use case formats and external formats. A controller converts HTTP requests into use case inputs. A presenter converts use case outputs into view models.

The outermost circle contains frameworks and drivers: the web framework, database, UI toolkit. These are details that can be swapped without affecting inner circles.

The key insight: inner circles know nothing about outer circles. Your classes do not know they are stored in a database. Your use cases do not know they are called from a web controller. This makes the core of your application portable and testable.

### Repository Pattern

The repository pattern abstracts data access behind a collection-like interface:

```zia
interface IProductRepository {
    func findById(id: String) -> Product?;
    func findByCategory(category: String) -> List[Product];
    func findAll() -> List[Product];
    func save(product: Product);
    func delete(id: String);
}
```

The repository looks like an in-memory collection. Code using it does not know or care whether data comes from a database, file, API, or memory.

```zia
// Database implementation
class SqlProductRepository implements IProductRepository {
    hide db: Database;

    expose func init(db: Database) {
        self.db = db;
    }

    func findById(id: String) -> Product? {
        var row = self.db.queryOne(
            "SELECT * FROM products WHERE id = ?",
            [id]
        );
        if row == null {
            return null;
        }
        return self.mapRowToProduct(row);
    }

    func findByCategory(category: String) -> List[Product] {
        var rows = self.db.query(
            "SELECT * FROM products WHERE category = ?",
            [category]
        );
        return rows.Map(row => self.mapRowToProduct(row));
    }

    func save(product: Product) {
        if self.findById(product.id) != null {
            self.db.execute(
                "UPDATE products SET name = ?, price = ? WHERE id = ?",
                [product.name, product.price, product.id]
            );
        } else {
            self.db.execute(
                "INSERT INTO products (id, name, price) VALUES (?, ?, ?)",
                [product.id, product.name, product.price]
            );
        }
    }

    func delete(id: String) {
        self.db.execute("DELETE FROM products WHERE id = ?", [id]);
    }

    hide func mapRowToProduct(row: DatabaseRow) -> Product {
        return Product(
            id: row.getString("id"),
            name: row.getString("name"),
            price: row.getFloat("price")
        );
    }
}

// In-memory implementation for testing
class InMemoryProductRepository implements IProductRepository {
    hide products: Map[String, Product];

    expose func init() {
        self.products = new Map[String, Product]();
    }

    func findById(id: String) -> Product? {
        return self.products.Get(id);
    }

    func findByCategory(category: String) -> List[Product] {
        return self.products.Values()
            .Filter(p => p.category == category);
    }

    func findAll() -> List[Product] {
        return self.products.Values();
    }

    func save(product: Product) {
        self.products.Set(product.id, product);
    }

    func delete(id: String) {
        self.products.remove(id);
    }
}
```

With the repository pattern, you can:
- Test business logic with the fast in-memory implementation
- Switch databases by swapping implementations
- Add caching without changing calling code
- Mock data access for specific test scenarios

### Service Layer Pattern

Service layer provides an API for business operations, coordinating multiple repositories and domain objects:

```zia
class OrderService {
    hide orderRepo: IOrderRepository;
    hide productRepo: IProductRepository;
    hide customerRepo: ICustomerRepository;
    hide paymentGateway: IPaymentGateway;
    hide emailService: IEmailService;
    hide inventoryService: InventoryService;

    expose func init(
        orderRepo: IOrderRepository,
        productRepo: IProductRepository,
        customerRepo: ICustomerRepository,
        paymentGateway: IPaymentGateway,
        emailService: IEmailService,
        inventoryService: InventoryService
    ) {
        self.orderRepo = orderRepo;
        self.productRepo = productRepo;
        self.customerRepo = customerRepo;
        self.paymentGateway = paymentGateway;
        self.emailService = emailService;
        self.inventoryService = inventoryService;
    }

    func placeOrder(customerId: String, items: List[OrderItem]) -> OrderResult {
        // Load customer
        var customer = self.customerRepo.findById(customerId);
        if customer == null {
            return OrderResult.failure("Customer not found");
        }

        // Validate and calculate
        var total = 0.0;
        for item in items {
            var product = self.productRepo.findById(item.productId);
            if product == null {
                return OrderResult.failure("Product not found: " + item.productId);
            }

            if !self.inventoryService.isAvailable(product.id, item.quantity) {
                return OrderResult.failure("Insufficient stock: " + product.name);
            }

            total = total + (product.price * Convert.ToDouble(item.quantity));
        }

        // Process payment
        var payment = self.paymentGateway.charge(
            customer.paymentMethodId,
            total
        );
        if !payment.success {
            return OrderResult.failure("Payment failed: " + payment.error);
        }

        // Create order
        var order = Order(
            customerId: customerId,
            items: items,
            total: total,
            paymentId: payment.transactionId,
            status: OrderStatus.CONFIRMED
        );
        self.orderRepo.save(order);

        // Update inventory
        for item in items {
            self.inventoryService.reserve(item.productId, item.quantity);
        }

        // Notify customer
        self.emailService.send(
            customer.email,
            "Order Confirmed",
            self.buildConfirmationEmail(order)
        );

        return OrderResult.success(order);
    }

    hide func buildConfirmationEmail(order: Order) -> String {
        // Build email body
    }
}
```

The service layer keeps controllers thin and business logic organized. Controllers just translate HTTP to service calls; services contain the real logic.

---

## Making Architectural Decisions

Architecture involves tradeoffs. Every choice has costs and benefits. Learning to evaluate tradeoffs is essential.

### YAGNI: You Aren't Gonna Need It

YAGNI warns against building flexibility you do not currently need. Future requirements are unpredictable. The flexibility you build today may be useless or wrong tomorrow.

```zia
// Over-engineered: Supports five database types nobody uses
interface IDatabaseProvider {
    func getConnection() -> Connection;
}

class MySqlProvider implements IDatabaseProvider { ... }
class PostgresProvider implements IDatabaseProvider { ... }
class SqliteProvider implements IDatabaseProvider { ... }
class OracleProvider implements IDatabaseProvider { ... }
class MongoProvider implements IDatabaseProvider { ... }

class DatabaseFactory {
    func create(type: String) -> IDatabaseProvider {
        // Complex factory logic
    }
}

// Simpler: Just use what you need
class Database {
    hide connection: PostgresConnection;

    expose func init(connectionString: String) {
        self.connection = PostgresConnection(connectionString);
    }
}
```

If your company uses only PostgreSQL, do not build support for five databases. Build for PostgreSQL. If requirements change later, refactor then.

However, YAGNI does not mean ignore architecture entirely. It means build the simplest architecture that meets current needs, while keeping code clean enough to change later.

### When to Abstract

Abstraction has costs: more code, more indirection, harder to trace execution. When is it worth it?

**Abstract when**:
- You have multiple implementations now
- You need to test with different implementations
- The concrete implementation is complex and obscures intent
- Changes are likely and you want to isolate them

**Do not abstract when**:
- There is only one implementation with no testing need
- The abstraction would be more complex than the concrete code
- You are guessing about future requirements

```zia
// Unnecessary abstraction
interface ICalculator {
    func add(a: Integer, b: Integer) -> Integer;
}

class Calculator implements ICalculator {
    expose func add(a: Integer, b: Integer) -> Integer {
        return a + b;
    }
}

// Just write the function
func add(a: Integer, b: Integer) -> Integer {
    return a + b;
}
```

The interface adds nothing. `add` is not going to have multiple implementations. Testing does not need a mock. The abstraction is pure overhead.

```zia
// Worthwhile abstraction
interface IPaymentProcessor {
    func charge(amount: Number, customerId: String) -> PaymentResult;
    func refund(transactionId: String) -> RefundResult;
}

// Real implementation calls Stripe API
class StripePaymentProcessor implements IPaymentProcessor { ... }

// Test implementation records calls without real charges
class MockPaymentProcessor implements IPaymentProcessor { ... }
```

Here the abstraction pays for itself. You need different implementations for production and testing. The interface lets you swap them.

### The Rule of Three

Before extracting a pattern, wait until you see it three times. Two occurrences might be coincidental. Three suggest a real pattern worth abstracting.

```zia
// First occurrence: Just write it
func processOrder(order: Order) {
    log("Processing order " + order.id);
    // process...
    log("Order processed " + order.id);
}

// Second occurrence: Notice similarity but wait
func processRefund(refund: Refund) {
    log("Processing refund " + refund.id);
    // process...
    log("Refund processed " + refund.id);
}

// Third occurrence: Now abstract
func processShipment(shipment: Shipment) {
    log("Processing shipment " + shipment.id);
    // process...
    log("Shipment processed " + shipment.id);
}

// Now you have enough examples to abstract well
func withLogging[T](name: String, id: String, operation: func() -> T) -> T {
    log("Processing " + name + " " + id);
    var result = operation();
    log(name + " processed " + id);
    return result;
}
```

The third example shows the pattern clearly. You can now abstract confidently because you understand the variations.

### Balancing Consistency and Pragmatism

Architecture provides guidelines, not laws. Sometimes breaking the rules makes sense.

Imagine you have a cleanly layered application. A new urgent feature needs data that is easy to get with a direct database query but hard to get through your repository abstraction. Do you:

A) Build proper repository methods, taking a week
B) Write a direct query in the controller, taking an hour

Option B violates your architecture. But if the feature is critical and time-constrained, pragmatism wins. Document the shortcut. Plan to fix it later. Ship the feature.

The danger is letting shortcuts accumulate. One direct query is fine. Twenty direct queries scattered throughout means you have abandoned layered architecture. Set a threshold: "We fix architectural shortcuts when we have more than three in any area."

---

## Communication Between Components

Components must communicate. How they communicate affects coupling, testability, and flexibility.

### Direct Method Calls

The simplest approach: one component holds a reference to another and calls its methods.

```zia
class OrderProcessor {
    hide emailService: EmailService;

    expose func init(emailService: EmailService) {
        self.emailService = emailService;
    }

    func processOrder(order: Order) {
        // Process order...
        self.emailService.sendConfirmation(order.customerEmail, order);
    }
}
```

Pros: Simple, explicit, easy to trace.
Cons: Tight coupling. OrderProcessor knows about EmailService.

### Callbacks and Handlers

Instead of calling services directly, accept functions to call:

```zia
class OrderProcessor {
    hide onOrderProcessed: func(Order);

    expose func init(onOrderProcessed: func(Order)) {
        self.onOrderProcessed = onOrderProcessed;
    }

    func processOrder(order: Order) {
        // Process order...
        self.onOrderProcessed(order);
    }
}

// Usage
var emailService = EmailService();
var processor = OrderProcessor(order => {
    emailService.sendConfirmation(order.customerEmail, order);
});
```

Pros: OrderProcessor does not know about EmailService. Easy to change behavior.
Cons: Control flow is less obvious. Callbacks can nest deeply.

### Event Bus / Publish-Subscribe

Components publish events without knowing who listens. Other components subscribe to events they care about.

> **Note:** this pattern is not currently runnable in Zia. Invoking a handler
> through a `&function` reference segfaults
> ([audit #26](../../audit_09012026.md)), so the `publish` loop below cannot
> execute today. The structure is still the right shape to learn from.

```zia
class Event {
    expose String kind;
    expose func init(k: String) { kind = k; }
}

class EventBus {
    hide subscribers: Map[String, List[(Event) -> Void]];

    expose func init() {
        subscribers = new Map[String, List[(Event) -> Void]]();
    }

    expose func subscribe(eventType: String, handler: (Event) -> Void) {
        if !subscribers.has(eventType) {
            var fresh: List[(Event) -> Void] = [];
            subscribers.set(eventType, fresh);
        }
        var handlers = subscribers.get(eventType);
        handlers!.add(handler);
    }

    expose func publish(event: Event) {
        if subscribers.has(event.kind) {
            var handlers = subscribers.get(event.kind);
            for handler in handlers! {
                handler(event);
            }
        }
    }
}

// Events are simple data
struct OrderPlacedEvent {
    type: String = "order_placed";
    orderId: String;
    customerId: String;
    total: Number;
}

// Order service publishes
class OrderService {
    hide eventBus: EventBus;

    func placeOrder(order: Order) {
        // Process order...
        self.eventBus.publish(OrderPlacedEvent(
            orderId: order.id,
            customerId: order.customerId,
            total: order.total
        ));
    }
}

// Other services subscribe
class EmailService {
    func start(eventBus: EventBus) {
        eventBus.subscribe("order_placed", self.handleOrderPlaced);
    }

    func handleOrderPlaced(event: Event) {
        var orderEvent = event as OrderPlacedEvent;
        self.sendConfirmation(orderEvent.orderId);
    }
}

class InventoryService {
    func start(eventBus: EventBus) {
        eventBus.subscribe("order_placed", self.handleOrderPlaced);
    }

    func handleOrderPlaced(event: Event) {
        var orderEvent = event as OrderPlacedEvent;
        self.reserveInventory(orderEvent.orderId);
    }
}

class AnalyticsService {
    func start(eventBus: EventBus) {
        eventBus.subscribe("order_placed", self.handleOrderPlaced);
    }

    func handleOrderPlaced(event: Event) {
        var orderEvent = event as OrderPlacedEvent;
        self.trackPurchase(orderEvent.total);
    }
}
```

Pros: Maximum decoupling. Add new subscribers without modifying publishers. Components are independent.

Cons: Control flow is hard to trace. Events can create implicit dependencies. Debugging is harder.

### Dependency Injection Containers

For large systems, manually wiring dependencies becomes tedious. Dependency injection containers automate it:

```zia
class Container {
    hide registrations: Map[String, func() -> any];
    hide singletons: Map[String, any];

    expose func init() {
        self.registrations = new Map[String, func() -> any]();
        self.singletons = new Map[String, any]();
    }

    func register[T](name: String, factory: func() -> T) {
        self.registrations.Set(name, factory);
    }

    func registerSingleton[T](name: String, factory: func() -> T) {
        self.registrations.Set(name, func() -> T {
            if !self.singletons.Has(name) {
                self.singletons.Set(name, factory());
            }
            return self.singletons.Get(name) as T;
        });
    }

    func resolve[T](name: String) -> T {
        var factory = self.registrations.Get(name);
        if factory == null {
            throw ("No registration for: " + name);
        }
        return factory() as T;
    }
}

// Registration at startup
func configureContainer() -> Container {
    var container = Container();

    // Infrastructure
    container.registerSingleton("database", func() -> IDatabase {
        return PostgresDatabase(Config.dbUrl);
    });

    container.registerSingleton("emailService", func() -> IEmailService {
        return SmtpEmailService(Config.smtpHost);
    });

    // Repositories
    container.register("userRepository", func() -> IUserRepository {
        return SqlUserRepository(container.resolve("database"));
    });

    container.register("orderRepository", func() -> IOrderRepository {
        return SqlOrderRepository(container.resolve("database"));
    });

    // Services
    container.register("orderService", func() -> OrderService {
        return OrderService(
            container.resolve("orderRepository"),
            container.resolve("emailService")
        );
    });

    return container;
}

// Usage
var container = configureContainer();
var orderService = container.resolve[OrderService]("orderService");
```

The container manages object creation and lifetime. Changes to wiring happen in one place.

---

## Step-by-Step Refactoring Example

Let us walk through refactoring messy code into clean architecture. This is how architecture typically emerges: not designed perfectly upfront, but evolved from working code.

### Stage 1: The Monolith

Here is a realistic starting point, a command-line application for managing a library:

```zia
func main() {
    var db = Database.connect("library.db");

    while true {
        print("1. Add book");
        print("2. Borrow book");
        print("3. Return book");
        print("4. List books");
        print("5. Exit");

        var choice = Input.ReadLine();

        if choice == "1" {
            print("Enter title: ");
            var title = Input.ReadLine();
            print("Enter author: ");
            var author = Input.ReadLine();
            print("Enter ISBN: ");
            var isbn = Input.ReadLine();

            if title.Length == 0 || author.Length == 0 || isbn.Length == 0 {
                print("All fields required!");
                continue;
            }

            db.execute(
                "INSERT INTO books (title, author, isbn, available) VALUES (?, ?, ?, 1)",
                [title, author, isbn]
            );
            print("Book added!");

        } else if choice == "2" {
            print("Enter ISBN: ");
            var isbn = Input.ReadLine();
            print("Enter borrower name: ");
            var borrower = Input.ReadLine();

            var book = db.queryOne("SELECT * FROM books WHERE isbn = ?", [isbn]);
            if book == null {
                print("Book not found!");
                continue;
            }
            if book.getInt("available") == 0 {
                print("Book not available!");
                continue;
            }

            db.execute(
                "UPDATE books SET available = 0 WHERE isbn = ?",
                [isbn]
            );
            db.execute(
                "INSERT INTO loans (isbn, borrower, date) VALUES (?, ?, ?)",
                [isbn, borrower, Time.DateTime.ToIso8601(Time.DateTime.Now())]
            );
            print("Book borrowed!");

        } else if choice == "3" {
            print("Enter ISBN: ");
            var isbn = Input.ReadLine();

            var loan = db.queryOne(
                "SELECT * FROM loans WHERE isbn = ? ORDER BY date DESC LIMIT 1",
                [isbn]
            );
            if loan == null {
                print("No active loan for this book!");
                continue;
            }

            db.execute("DELETE FROM loans WHERE id = ?", [loan.getInt("id")]);
            db.execute("UPDATE books SET available = 1 WHERE isbn = ?", [isbn]);
            print("Book returned!");

        } else if choice == "4" {
            var books = db.query("SELECT * FROM books");
            for book in books {
                var status = book.getInt("available") == 1 ? "Available" : "Borrowed";
                print(book.getString("title") + " by " + book.getString("author") +
                      " [" + status + "]");
            }

        } else if choice == "5" {
            break;
        }
    }
}
```

This works but has problems:
- UI, business logic, and database access are tangled
- Cannot test without a database
- Cannot reuse logic (no API possible)
- Hard to add features

### Stage 2: Extract Domain Entities

First, create domain objects to represent our concepts:

```zia
// domain/Book.zia
struct Book {
    isbn: String;
    title: String;
    author: String;
    available: Boolean;
}

// domain/Loan.zia
struct Loan {
    id: Integer;
    isbn: String;
    borrowerName: String;
    borrowDate: DateTime;
}
```

Using `struct` instead of `class` because these are simple data holders without behavior yet.

### Stage 3: Extract Repository

Move database code to a repository:

```zia
// infrastructure/BookRepository.zia
class BookRepository {
    hide db: Database;

    expose func init(db: Database) {
        self.db = db;
    }

    func findByIsbn(isbn: String) -> Book? {
        var row = self.db.queryOne(
            "SELECT * FROM books WHERE isbn = ?",
            [isbn]
        );
        if row == null {
            return null;
        }
        return Book(
            isbn: row.getString("isbn"),
            title: row.getString("title"),
            author: row.getString("author"),
            available: row.getInt("available") == 1
        );
    }

    func findAll() -> List[Book] {
        var rows = self.db.query("SELECT * FROM books");
        return rows.Map(row => Book(
            isbn: row.getString("isbn"),
            title: row.getString("title"),
            author: row.getString("author"),
            available: row.getInt("available") == 1
        ));
    }

    func save(book: Book) {
        var existing = self.findByIsbn(book.isbn);
        if existing != null {
            self.db.execute(
                "UPDATE books SET title = ?, author = ?, available = ? WHERE isbn = ?",
                [book.title, book.author, book.available ? 1 : 0, book.isbn]
            );
        } else {
            self.db.execute(
                "INSERT INTO books (isbn, title, author, available) VALUES (?, ?, ?, ?)",
                [book.isbn, book.title, book.author, book.available ? 1 : 0]
            );
        }
    }
}

// infrastructure/LoanRepository.zia
class LoanRepository {
    hide db: Database;

    expose func init(db: Database) {
        self.db = db;
    }

    func findActiveByIsbn(isbn: String) -> Loan? {
        var row = self.db.queryOne(
            "SELECT * FROM loans WHERE isbn = ? ORDER BY date DESC LIMIT 1",
            [isbn]
        );
        if row == null {
            return null;
        }
        return Loan(
            id: row.getInt("id"),
            isbn: row.getString("isbn"),
            borrowerName: row.getString("borrower"),
            borrowDate: DateTime.ParseIso8601(row.getString("date"))
        );
    }

    func save(loan: Loan) {
        self.db.execute(
            "INSERT INTO loans (isbn, borrower, date) VALUES (?, ?, ?)",
            [loan.isbn, loan.borrowerName, loan.borrowDate.toString()]
        );
    }

    func delete(id: Integer) {
        self.db.execute("DELETE FROM loans WHERE id = ?", [id]);
    }
}
```

### Stage 4: Extract Business Logic to Service

Create a service for business operations:

```zia
// application/LibraryService.zia
class LibraryService {
    hide bookRepo: BookRepository;
    hide loanRepo: LoanRepository;

    expose func init(bookRepo: BookRepository, loanRepo: LoanRepository) {
        self.bookRepo = bookRepo;
        self.loanRepo = loanRepo;
    }

    func addBook(title: String, author: String, isbn: String) -> Result[Book] {
        // Validation
        if title.Trim().Length == 0 {
            return Result.ErrStr("Title is required");
        }
        if author.Trim().Length == 0 {
            return Result.ErrStr("Author is required");
        }
        if isbn.Trim().Length == 0 {
            return Result.ErrStr("ISBN is required");
        }

        // Check for duplicates
        if self.bookRepo.findByIsbn(isbn) != null {
            return Result.ErrStr("Book with ISBN already exists");
        }

        var book = Book(
            isbn: isbn,
            title: title,
            author: author,
            available: true
        );
        self.bookRepo.save(book);

        return Result.Ok(book);
    }

    func borrowBook(isbn: String, borrowerName: String) -> Result[Loan] {
        // Validation
        if borrowerName.Trim().Length == 0 {
            return Result.ErrStr("Borrower name is required");
        }

        // Find book
        var book = self.bookRepo.findByIsbn(isbn);
        if book == null {
            return Result.ErrStr("Book not found");
        }
        if !book.available {
            return Result.ErrStr("Book is not available");
        }

        // Create loan
        var loan = Loan(
            id: 0,  // Will be assigned by database
            isbn: isbn,
            borrowerName: borrowerName,
            borrowDate: Time.DateTime.Now()
        );
        self.loanRepo.save(loan);

        // Update book availability
        var updatedBook = Book(
            isbn: book.isbn,
            title: book.title,
            author: book.author,
            available: false
        );
        self.bookRepo.save(updatedBook);

        return Result.Ok(loan);
    }

    func returnBook(isbn: String) -> Result[Boolean] {
        // Find active loan
        var loan = self.loanRepo.findActiveByIsbn(isbn);
        if loan == null {
            return Result.ErrStr("No active loan for this book");
        }

        // Find book
        var book = self.bookRepo.findByIsbn(isbn);
        if book == null {
            return Result.ErrStr("Book not found");
        }

        // Delete loan and update book
        self.loanRepo.Delete(loan.id);
        var updatedBook = Book(
            isbn: book.isbn,
            title: book.title,
            author: book.author,
            available: true
        );
        self.bookRepo.save(updatedBook);

        return Result.Ok(true);
    }

    func listBooks() -> List[Book] {
        return self.bookRepo.findAll();
    }
}
```

### Stage 5: Clean Up the UI

Now the main function is just UI:

```zia
// presentation/cli/Main.zia
func main() {
    // Setup
    var db = Database.connect("library.db");
    var bookRepo = BookRepository(db);
    var loanRepo = LoanRepository(db);
    var library = LibraryService(bookRepo, loanRepo);

    // UI loop
    while true {
        print("\n=== Library System ===");
        print("1. Add book");
        print("2. Borrow book");
        print("3. Return book");
        print("4. List books");
        print("5. Exit");
        print("\nChoice: ");

        var choice = Input.ReadLine();

        if choice == "1" {
            handleAddBook(library);
        } else if choice == "2" {
            handleBorrowBook(library);
        } else if choice == "3" {
            handleReturnBook(library);
        } else if choice == "4" {
            handleListBooks(library);
        } else if choice == "5" {
            print("Goodbye!");
            break;
        } else {
            print("Invalid choice");
        }
    }
}

func handleAddBook(library: LibraryService) {
    print("Enter title: ");
    var title = Input.ReadLine();
    print("Enter author: ");
    var author = Input.ReadLine();
    print("Enter ISBN: ");
    var isbn = Input.ReadLine();

    var result = library.addBook(title, author, isbn);
    if result.isError {
        print("Error: " + result.error);
    } else {
        print("Book added successfully!");
    }
}

func handleBorrowBook(library: LibraryService) {
    print("Enter ISBN: ");
    var isbn = Input.ReadLine();
    print("Enter borrower name: ");
    var borrower = Input.ReadLine();

    var result = library.borrowBook(isbn, borrower);
    if result.isError {
        print("Error: " + result.error);
    } else {
        print("Book borrowed successfully!");
    }
}

func handleReturnBook(library: LibraryService) {
    print("Enter ISBN: ");
    var isbn = Input.ReadLine();

    var result = library.returnBook(isbn);
    if result.isError {
        print("Error: " + result.error);
    } else {
        print("Book returned successfully!");
    }
}

func handleListBooks(library: LibraryService) {
    var books = library.listBooks();
    if books.Length == 0 {
        print("No books in library");
        return;
    }

    print("\nBooks:");
    for book in books {
        var status = book.available ? "Available" : "Borrowed";
        print("  " + book.title + " by " + book.author + " [" + status + "]");
    }
}
```

### Stage 6: Add Interfaces for Testing

Finally, extract interfaces so we can test without a database:

```zia
// domain/IBookRepository.zia
interface IBookRepository {
    func findByIsbn(isbn: String) -> Book?;
    func findAll() -> List[Book];
    func save(book: Book);
}

// domain/ILoanRepository.zia
interface ILoanRepository {
    func findActiveByIsbn(isbn: String) -> Loan?;
    func save(loan: Loan);
    func delete(id: Integer);
}

// Update LibraryService to use interfaces
class LibraryService {
    hide bookRepo: IBookRepository;
    hide loanRepo: ILoanRepository;

    expose func init(bookRepo: IBookRepository, loanRepo: ILoanRepository) {
        self.bookRepo = bookRepo;
        self.loanRepo = loanRepo;
    }
    // ... rest unchanged
}
```

Now we can create in-memory implementations for testing. The final test case below is pseudocode; current Zia does not ship built-in `test` or `assert` syntax.

```text
// tests/InMemoryBookRepository.zia
class InMemoryBookRepository implements IBookRepository {
    hide books: Map[String, Book];

    expose func init() {
        self.books = Map();
    }

    func findByIsbn(isbn: String) -> Book? {
        return self.books.Get(isbn);
    }

    func findAll() -> List[Book] {
        return self.books.Values();
    }

    func save(book: Book) {
        self.books.Set(book.isbn, book);
    }
}

// tests/LibraryServiceTest.zia
test "cannot borrow unavailable book" {
    var bookRepo = InMemoryBookRepository();
    var loanRepo = InMemoryLoanRepository();
    var library = LibraryService(bookRepo, loanRepo);

    // Add a book and borrow it
    library.addBook("Test Book", "Author", "123");
    library.borrowBook("123", "Alice");

    // Try to borrow again
    var result = library.borrowBook("123", "Bob");

    assert result.isError;
    assert result.error == "Book is not available";
}
```

The refactoring is complete. We went from tangled spaghetti to clean, testable, maintainable code. Each step was small and safe.

---

## Documenting Architecture

Architecture lives in developers' heads. Without documentation, knowledge walks out the door when people leave.

### When to Document

Not everything needs documentation. Document:

- **System overview**: What does this system do? Who uses it?
- **Major components**: What are the key pieces and their responsibilities?
- **Key decisions**: Why was PostgreSQL chosen over MongoDB? Why this structure?
- **Boundaries and interfaces**: How do components communicate?
- **Deployment**: How is this deployed? What infrastructure exists?

Do not document:

- Implementation details that are obvious from code
- Decisions that are easily reversible
- Anything that will become stale immediately

### Documentation Formats

**Architecture Decision Records (ADRs)** capture why decisions were made:

```markdown
# ADR 001: Use PostgreSQL for Primary Database

## Status
Accepted

## Context
We need to choose a database for our order management system.
Expected volume is 10,000 orders per day initially, growing to 100,000.

## Decision
We will use PostgreSQL.

## Rationale
- Our team has PostgreSQL expertise
- Strong ACID compliance for financial data
- Good performance at our expected scale
- Rich querying capabilities we will need

## Consequences
- Need PostgreSQL hosting (AWS RDS selected)
- Team needs to learn PostgreSQL-specific features
- Some NoSQL flexibility sacrificed
```

**C4 Diagrams** visualize architecture at different zoom levels:
- Level 1: System context (your system and its users/external systems)
- Level 2: Containers (applications, databases, services)
- Level 3: Components (major pieces within a container)
- Level 4: Code (class/class diagrams for critical areas)

**README files** in each directory explain that area:

```markdown
# /application

This directory contains use cases - the application-specific business rules.

Each use case is a single class that:
- Accepts a request value
- Validates inputs
- Orchestrates domain classes
- Returns a result value

Use cases should not know about HTTP, databases, or UI.
They depend only on domain interfaces.

## Files

- CreateOrderUseCase.zia - Creates new orders
- CancelOrderUseCase.zia - Cancels existing orders
- GetOrderUseCase.zia - Retrieves order details
```

### Keeping Documentation Current

Documentation rots. Combat this:

- Review documentation during code reviews
- Link code to relevant documentation
- Delete documentation that is no longer true
- Keep documentation close to code (in the repo)
- Prefer diagrams that can be generated from code

---

## Common Architecture Mistakes

Learn from others' mistakes.

### Premature Abstraction

Building elaborate architectures before understanding the problem:

```zia
// Too much too soon
interface IUserDataAccessLayer { }
interface IUserBusinessLogicLayer { }
interface IUserPresentationLayer { }
interface IUserDataTransferObject { }
interface IUserViewModelMapper { }
// For a system with 10 users
```

Start simple. Add abstraction when you feel pain, not before.

### Over-Engineering

Flexibility nobody needs:

```text
// Supports 5 authentication methods, company uses 1
class AuthenticationStrategyFactory {
    func create(type: String) -> IAuthenticationStrategy {
        if type == "oauth" { return OAuthStrategy(); }
        if type == "saml" { return SamlStrategy(); }
        if type == "ldap" { return LdapStrategy(); }
        if type == "basic" { return BasicStrategy(); }
        if type == "apikey" { return ApiKeyStrategy(); }
    }
}
```

If you only need OAuth, just use OAuth. Add others when required.

### Under-Engineering

The opposite extreme, ignoring architecture entirely:

```zia
// Everything in one 5000-line file
func main() {
    // Database setup
    // HTTP server setup
    // All route handlers
    // All business logic
    // All data access
    // All utilities
}
```

Some structure is always better than none. Even basic separation helps.

### Mismatched Patterns

Using patterns that do not fit your problem:

```zia
// Using event sourcing for a simple CRUD app
class UserEventStore {
    events: List[UserEvent];

    func apply(event: UserEvent) {
        self.events.Push(event);
    }

    func getCurrentState() -> User {
        var user = User();
        for event in self.events {
            user = event.apply(user);
        }
        return user;
    }
}
// For a system that just needs to store and retrieve users
```

Patterns solve specific problems. Use them when you have those problems.

### Cargo Cult Architecture

Copying architecture from big tech companies without understanding why:

"Netflix uses microservices, so we should too!"

Netflix has thousands of engineers, millions of users, and specific scaling challenges. Your team of five building an internal tool does not need microservices. It needs a well-organized monolith.

Choose architecture based on your constraints, not others' solutions.

### Ignoring Team Capabilities

Architectural decisions must consider who will implement and maintain the system:

```zia
// Beautiful functional architecture
func processOrder(order: Order) -> IO<Either<OrderError, OrderResult>> {
    // Team has never seen functional programming
    // Nobody can maintain this
}
```

The best architecture is one your team can build and maintain. Sophistication your team cannot handle is not sophisticated.

---

## Evolving Architecture

Good architecture is not static. It evolves with the system.

### Start Simple

Every system should start as simple as possible:

```zia
// Initial version: Just make it work
class App {
    func handleRequest(request: Request) -> Response {
        if request.path == "/users" {
            return self.handleUsers(request);
        }
        if request.path == "/orders" {
            return self.handleOrders(request);
        }
        return Response.notFound();
    }
}
```

No layers, no patterns, no abstractions. Just working code.

### Add Structure When Needed

When code becomes hard to change, add structure:

```zia
// Growing: Extract services
class UserService {
    func create(data: UserData) -> User { ... }
    func find(id: String) -> User? { ... }
}

class OrderService {
    func create(data: OrderData) -> Order { ... }
    func find(id: String) -> Order? { ... }
}

class App {
    hide userService: UserService;
    hide orderService: OrderService;

    func handleRequest(request: Request) -> Response {
        if request.path.StartsWith("/users") {
            return self.handleUsers(request);
        }
        // ...
    }
}
```

### Refactor Continuously

Architecture emerges through continuous refactoring:

1. Notice duplication or coupling
2. Extract an abstraction
3. Move code to better locations
4. Repeat

Do this regularly in small steps, not occasionally in large rewrites.

### Plan for Change

Assume everything will change. Design to make changes easy:

- Keep components small
- Define clear interfaces
- Minimize dependencies
- Document decisions so future developers understand context

### Know When to Rewrite

Sometimes the right answer is to start over. Signs it is time:

- Architecture fundamentally mismatches current needs
- Changing anything requires understanding everything
- The system is more burden than value
- A rewrite would be faster than continued evolution

But rewriting is expensive and risky. Usually, incremental improvement is better.

---

## The Three Languages

Architecture concepts are language-independent. Here is how they appear in Zanna's three languages.

**Zia**
```zia
module Application;

interface IRepository[T] {
    func findById(id: String) -> T?;
    func save(item: T);
}

class UserService {
    hide repo: IRepository[User];

    expose func init(repo: IRepository[User]) {
        self.repo = repo;
    }

    func createUser(name: String) -> User {
        var user = User(name);
        self.repo.save(user);
        return user;
    }
}
```

**BASIC**
```basic
MODULE Application

INTERFACE IRepository
    FUNCTION FindById(id AS STRING) AS VARIANT
    SUB Save(item AS VARIANT)
END INTERFACE

CLASS UserService
    PRIVATE repo AS IRepository

    PUBLIC SUB New(r AS IRepository)
        repo = r
    END SUB

    PUBLIC FUNCTION CreateUser(name AS STRING) AS User
        DIM u AS NEW User(name)
        repo.Save(u)
        RETURN u
    END FUNCTION
END CLASS
```

---

## Summary

Architecture is about managing complexity. As systems grow, good architecture keeps them understandable, changeable, and maintainable.

**Core principles**:
- **Separation of concerns**: Each component does one thing
- **Single responsibility**: One reason to change
- **Dependency inversion**: Depend on abstractions
- **Interface segregation**: Small, focused interfaces

**Common patterns**:
- **Layered architecture**: Presentation, application, domain, infrastructure
- **MVC**: Separate model, view, controller
- **Repository**: Abstract data access
- **Service layer**: Encapsulate business operations
- **Event-driven**: Decouple with publish-subscribe

**Practical wisdom**:
- Start simple, add structure when needed
- Apply YAGNI, wait for three occurrences before abstracting
- Balance consistency with pragmatism
- Document decisions and boundaries
- Evolve architecture continuously

Architecture is not about following rules. It is about making thoughtful decisions that serve your system's needs. The best architecture is the simplest one that makes your system easy to understand, change, and maintain.

---

## Exercises

**Exercise 28.1 (Warm-up)**: Take this monolithic function and identify separate concerns:

```zia
func processPayment(userId: String, amount: Number) {
    // Load user from database
    var db = Database.connect("localhost:5432");
    var user = db.query("SELECT * FROM users WHERE id = ?", [userId]);

    // Validate
    if user == null { print("User not found"); return; }
    if amount <= 0 { print("Invalid amount"); return; }
    if user.balance < amount { print("Insufficient funds"); return; }

    // Process
    db.execute("UPDATE users SET balance = balance - ? WHERE id = ?", [amount, userId]);

    // Log
    File.AppendLine("transactions.log",
                    Time.DateTime.ToIso8601(Time.DateTime.Now()) + ": " + userId + " paid " + amount);

    // Notify
    var smtp = SMTP.connect("mail.server.com");
    smtp.send(user.email, "Payment processed", "You paid $" + amount);

    print("Payment successful!");
}
```

List at least five separate concerns in this function.

**Exercise 28.2 (Refactoring)**: Refactor the function from Exercise 28.1 using separation of concerns. Create appropriate classes and interfaces.

**Exercise 28.3 (Repository Pattern)**: Implement a `TaskRepository` interface with methods for finding, saving, and deleting tasks. Create both a `SqlTaskRepository` and an `InMemoryTaskRepository` implementation.

**Exercise 28.4 (Layered Architecture)**: Design a folder structure for a restaurant ordering system with:
- Menu management
- Order taking
- Kitchen display
- Payment processing

Show what files would exist in each layer.

**Exercise 28.5 (Event Bus)**: Build a simple event bus class that supports:
- Subscribing to event types
- Unsubscribing from event types
- Publishing events to all subscribers

Write tests for your implementation.

**Exercise 28.6 (Dependency Injection)**: You have a `NotificationService` that can send via email, SMS, or push notification. Design it using dependency injection so different notification methods can be swapped easily.

**Exercise 28.7 (MVC)**: Implement a simple counter application using MVC:
- Model: Stores count, supports increment/decrement
- View: Displays count (can be console output)
- Controller: Handles user input

**Exercise 28.8 (Refactoring Journey)**: Take this starter code and refactor it step by step:

```zia
bind Convert = Zanna.Core.Convert;

func main() {
    var items = [];
    while true {
        print("1. Add item  2. Remove item  3. Total  4. Exit: ");
        var choice = Input.ReadLine();
        if choice == "1" {
            print("Name: "); var name = Input.ReadLine();
            print("Price: "); var price = Convert.ToDouble(Input.ReadLine());
            items.Push({ name: name, price: price });
        } else if choice == "2" {
            print("Index: "); var idx = Convert.ToInt64(Input.ReadLine());
            items.RemoveAt(idx);
        } else if choice == "3" {
            var total = 0.0;
            for item in items { total = total + item.price; }
            print("Total: $" + total);
        } else if choice == "4" { break; }
    }
}
```

Create: a `ShoppingCart` class, an `Item` value, and separate UI handling functions.

**Exercise 28.9 (Architecture Decision)**: You are building a new feature that needs to send notifications. You could:
A) Add notification code directly in the existing service
B) Create a separate notification service
C) Use an event bus so services can publish events

For each option, list pros and cons. What would influence your decision?

**Exercise 28.10 (Challenge)**: Design and implement a plugin system for a text editor:
- Core editor with basic functionality
- Plugins can add features without modifying core
- Plugins can be loaded and unloaded at runtime
- Plugins can respond to events (file opened, text changed, etc.)

Document your architecture decisions and implement at least two sample plugins.

---

*Congratulations! You have completed the main chapters of The Zanna Book. You have learned programming from the ground up, from your first "Hello, World!" to architecting complex systems.*

*The appendices provide quick references for the languages and runtime. Use them as you continue your journey.*

*[Continue to Appendix A: Zia Reference](../appendices/a-zia-reference.md)*
