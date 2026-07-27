---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 23: Data Formats

Every program you have ever used handles data. Your web browser downloads pages. Your text editor saves documents. Your game remembers your progress. Your music player reads song files. But here is a question that might seem trivial until you really think about it: *how does data get from one place to another?*

When you save a document and close your word processor, the document does not vanish. It persists on your hard drive, waiting for you to open it again tomorrow, next week, or next year. When you share that document with a friend, they can open it on a completely different computer running completely different software. How is this possible?

The answer is **data formats** --- agreed-upon ways to represent information as bytes. A data format is like a language that different programs speak. If two programs understand JSON, they can exchange JSON data. If your game saves its state in a well-defined format, a completely different program could read that save file.

This chapter is about the most important skill in practical programming: moving data between the world inside your program (variables, objects, arrays) and the world outside (files, networks, databases, other programs). This process has names: **serialization** (turning program data into a storable format) and **parsing** (turning stored data back into program data). Master these concepts, and you can build programs that remember, communicate, and interoperate.

---

## Why Data Formats Matter

Imagine you are building a game. Your player has a name, a level, health points, an inventory of items, and a position on the map:

```zia
struct Player {
    name: String;
    level: Integer;
    health: Number;
    inventory: List[String];
    position: Position;
}

struct Position {
    x: Number;
    y: Number;
}
```

Now imagine the player saves their game and quits. Tomorrow, they want to continue where they left off. How do you make that work?

The `Player` value exists in your computer's memory --- it is a structured arrangement of bytes that your program understands. But when your program ends, that memory is released. The data is gone. To persist it, you must write it to a file. But you cannot just dump raw memory to disk and hope for the best. That memory layout is specific to your program, your programming language, your computer's architecture. A different program could not read it. Even a future version of your own program might not read it if the data structure changes.

Instead, you need to translate your data into a **portable, well-defined format** that any program can understand. You need to agree on how to represent "this player is named Hero, is level 5, has 87.5 health points, carries a sword and shield, and stands at coordinates (100, 200)."

This is the fundamental challenge: bridging the gap between the structured world inside your program and the unstructured world of bytes that can be stored and transmitted.

---

## Mental Models for Serialization

Before we dive into specific formats, let us build intuition about what serialization actually means.

### The Suitcase Metaphor

Imagine you are packing for a trip. At home, your clothes live in drawers, organized by type: socks in one drawer, shirts in another, pants in a third. This is like data in your program --- structured, organized, easy to access.

But you cannot bring your dresser on the plane. You need to pack everything into a suitcase. You fold clothes, arrange them efficiently, and close the suitcase. This is **serialization**: taking structured data and packing it into a portable format.

When you arrive at your destination, you unpack. You take clothes out of the suitcase and organize them again --- maybe in a hotel dresser, maybe in a different arrangement than at home. This is **deserialization** (or **parsing**): taking the portable format and recreating structured data from it.

Key insights from this metaphor:
- **Packing loses some structure.** Your suitcase does not have drawers. The organization is simpler.
- **Unpacking requires knowing how things were packed.** If you packed shirts on top of pants, you expect that layout when unpacking.
- **Different trips might need different packing.** A weekend trip and a month-long journey require different approaches.

### The Translation Metaphor

Think of serialization as translation between languages. Your program thinks in "Zia" --- it has entities, values, arrays, and functions. A JSON file thinks in "JSON" --- it has objects, arrays, strings, and numbers. An XML file thinks in "XML" --- it has elements, attributes, and text content.

To save your player to JSON, you must translate from Zia to JSON:
- A `struct` becomes a JSON object
- A `String` becomes a JSON string
- An `Integer` becomes a JSON number
- An array becomes a JSON array

To load a player from JSON, you translate back:
- A JSON object becomes values for constructing a `Player`
- JSON strings become Zia strings
- JSON numbers become integers or floats

Some translations are lossy. JSON does not distinguish between integers and floating-point numbers --- they are all just "numbers." XML represents everything as text, so numbers like `42` are actually the characters `'4'` and `'2'`. When translating back, you must interpret the text as a number.

### The Contract Metaphor

A data format is a contract between the writer and the reader. The writer promises to produce data in a specific structure. The reader promises to interpret that structure correctly. If either party breaks the contract, communication fails.

When you design a save file format, you are establishing a contract:
- "The first field will be the player's name, a string."
- "The second field will be the player's level, an integer."
- "The inventory will be an array of item names."

Anyone who wants to read your save files must honor this contract. If you change the contract (add new fields, rename existing ones, reorder things), old readers might break. This is why versioning data formats matters --- you can check "is this file version 1 or version 2?" and handle each appropriately.

---

## The Landscape of Structured Data

Before examining specific formats, let us understand what "structured data" means and the fundamental choices every format makes.

### What Is Structured Data?

Structured data has organization. It is not just a soup of bytes or a stream of text --- it has identifiable parts with relationships between them.

Consider a contact:
```text
Name: Alice Smith
Phone: 555-1234
Email: alice@example.com
```

This data has three fields: name, phone, and email. Each field has a label and a value. If we wanted to store multiple contacts, we would need a way to separate them. If contacts could have multiple phone numbers, we would need a way to represent lists.

Structured data typically involves:
- **Fields** (also called properties or attributes): named pieces of data
- **Values**: the actual data (strings, numbers, booleans)
- **Nesting**: data containing other data (a contact has an address, which has street, city, and zip)
- **Collections**: lists or sets of data (a person has multiple phone numbers)

### Text vs. Binary Formats

Every data format makes a fundamental choice: represent data as human-readable text or as compact binary.

**Text formats** (JSON, XML, CSV, INI) use printable characters. You can open them in a text editor and read them. They are easy to debug, easy to edit manually, and work across different computer systems because text is universal.

```json
{"name": "Hero", "level": 5, "health": 87.5}
```

You can see exactly what this says. If something is wrong, you can spot it.

**Binary formats** use raw bytes that do not correspond to printable characters. They are compact and fast but inscrutable to humans. Open a binary file in a text editor and you see gibberish.

```text
56 49 50 52 00 00 00 01 00 00 00 05 48 65 72 6F
```

This might encode the same information but requires documentation to decode.

The choice depends on your needs:
- **Text formats** when humans need to read, edit, or debug the data
- **Binary formats** when file size or parsing speed is critical

For most applications, start with text formats. Their human-readability is invaluable for debugging and development. Only switch to binary when you have measured a performance problem.

### Schemas: Defining Expected Structure

A **schema** defines what structure data should have. It is like a form template: the form specifies which fields exist, what types they should be, which are required, and what values are acceptable.

Some formats have built-in schema support:
- **XML** can use XSD (XML Schema Definition) to define valid structures
- **JSON** can use JSON Schema to define valid structures

Some formats have no built-in schema:
- **CSV** is just rows and columns; the meaning is up to interpretation
- **INI** files have sections and key-value pairs but no formal types

Even without formal schemas, you have implicit expectations about structure. When your code reads `data["player"]["name"]`, you expect `data` to be an object with a `player` field that is also an object with a `name` field. These expectations form an informal schema.

Why schemas matter:
- **Validation**: Check that data matches expectations before processing
- **Documentation**: The schema tells you what data should look like
- **Interoperability**: If two systems agree on a schema, they can exchange data confidently

---

## JSON: The Universal Data Language

JSON (JavaScript Object Notation) has become the de facto standard for data exchange. APIs use it. Configuration files use it. Databases store it. If you learn one data format well, make it JSON.

### The Philosophy of JSON

JSON was designed to be simple. It has only a handful of concepts, all borrowed from common programming constructs:
- Objects (like dictionaries or entities)
- Arrays (ordered lists)
- Strings, numbers, booleans, and null

This simplicity is JSON's superpower. Every programming language can represent these concepts, so every programming language can work with JSON. There is no impedance mismatch, no awkward translation.

JSON is also human-readable. You can open a JSON file, understand its structure, and edit it by hand. This makes debugging vastly easier compared to binary formats.

### JSON Syntax Deep Dive

Let us examine JSON syntax precisely, because small details matter.

**Objects** are enclosed in curly braces `{ }` and contain key-value pairs:

```json
{
    "name": "Hero",
    "level": 5
}
```

Rules for objects:
- Keys must be strings (always in double quotes)
- Values can be any JSON type
- Key-value pairs are separated by commas
- No trailing comma after the last pair (unlike some languages)
- Order of keys is not guaranteed to be preserved

**Arrays** are enclosed in square brackets `[ ]` and contain ordered values:

```json
["sword", "shield", "potion"]
```

Rules for arrays:
- Elements can be any JSON type
- Elements are separated by commas
- No trailing comma after the last element
- Order is preserved

**Strings** are enclosed in double quotes:

```json
"Hello, World!"
```

Rules for strings:
- Must use double quotes, not single quotes
- Special characters must be escaped: `\"` for quote, `\\` for backslash, `\n` for newline, `\t` for tab
- Unicode characters can be escaped as `\uXXXX`

**Numbers** are written directly, with no quotes:

```json
42
3.14159
-17
2.5e10
```

Rules for numbers:
- No leading zeros (except for `0` itself or before decimal point)
- Decimal point must have digits on both sides
- Exponents use `e` or `E`
- No distinction between integers and floats at the format level

**Booleans** are the literals `true` and `false` (lowercase):

```json
true
false
```

**Null** is the literal `null` (lowercase):

```json
null
```

### Nesting: The Power of Composition

JSON shines when structures contain other structures:

```json
{
    "name": "Hero",
    "level": 5,
    "health": 87.5,
    "inventory": ["sword", "shield", "potion"],
    "position": {
        "x": 100.0,
        "y": 200.0
    },
    "skills": [
        {"name": "Slash", "damage": 10, "cooldown": 1.5},
        {"name": "Block", "damage": 0, "cooldown": 3.0},
        {"name": "Heal", "damage": -20, "cooldown": 10.0}
    ],
    "isAlive": true,
    "guild": null
}
```

This single JSON document represents complex, deeply nested data:
- The player has a `position` object nested inside
- The `inventory` is an array of strings
- The `skills` array contains objects, each with their own fields
- The `guild` is null, indicating the player is not in a guild

### Parsing JSON in Zia

> **Note:** The examples in this chapter use `bind Json = Zanna.Data.Json;` for JSON operations. Use `Json.GetStr()`, `Json.GetInt()`, `Json.GetBool()`, and the matching `Json.Set*()` helpers for object fields. See Appendix D for the complete runtime reference.

Parsing means reading JSON text and creating program data structures from it:

```zia
bind Json = Zanna.Data.Json;
bind Zanna.Terminal as Terminal;
bind Fmt = Zanna.Text.Fmt;

func start() {
    var jsonText = "{\"name\": \"Alice\", \"score\": 100, \"active\": true}";

    // Parse the JSON text into a runtime object
    var data = Json.Parse(jsonText);

    // Extract values with type conversions
    var name = Json.GetStr(data, "name");
    var score = Json.GetInt(data, "score");
    var active = Json.GetBool(data, "active");

    Terminal.Say("Player: " + name);
    Terminal.Say("Score: " + Fmt.Int(score));
    Terminal.Say("Active: " + Fmt.Bool(active));
}
```

Let us trace through this step by step:

1. **`Json.Parse(jsonText)`**: Takes the raw text and builds a runtime object representing the JSON. Objects behave like maps, arrays behave like sequences, and scalar values stay boxed.

2. **`Json.GetStr(data, "name")`**: Reads the string field named `"name"` from the parsed object.

3. **`Json.GetInt()` / `Json.GetBool()`**: Read integer and boolean fields with the expected type.

For dynamic values, use the `Zanna.Core.Box` conversion helpers or parse string fields explicitly with `Zanna.Core.Parse`.

### Creating JSON in Zia

To save program data as JSON, you build a JSON structure programmatically:

```zia
bind Json = Zanna.Data.Json;
bind Zanna.Terminal as Terminal;

func start() {
    // Create a JSON object
    var player = Json.NewObject();

    // Add simple values
    Json.SetStr(player, "name", "Hero");
    Json.SetInt(player, "level", 5);
    Json.SetInt(player, "health", 88);
    Json.SetBool(player, "alive", true);

    // Convert to JSON text
    var jsonText = Json.Format(player);
    Terminal.Say(jsonText);
}
```

Output:
```json
{"health":88,"alive":true,"level":5,"name":"Hero"}
```

The output is compact --- no unnecessary whitespace. This is efficient for storage and transmission.

### Pretty-Printing for Readability

Compact JSON is hard to read. For debugging, logs, or configuration files that humans edit, use pretty-printing:

```zia
var jsonText = Json.FormatPretty(player, 2);
Terminal.Say(jsonText);
```

Output:
```json
{
    "name": "Hero",
    "level": 5,
    "health": 87.5,
    "alive": true,
    "inventory": [
        "sword",
        "shield",
        "potion"
    ],
    "position": {
        "x": 100.0,
        "y": 200.0
    }
}
```

This is the same data, just formatted with indentation and newlines for human eyes.

### Converting Between Entities and JSON

For clean code, add serialization methods to your entities:

```zia
bind Json = Zanna.Data.Json;
bind File = Zanna.IO.File;

class Player {
    expose name: String;
    expose level: Integer;
    expose health: Integer;

    expose func init(name: String, level: Integer, health: Integer) {
        self.name = name;
        self.level = level;
        self.health = health;
    }

    // Convert this Player to a JSON representation
    expose func toJson() -> Any {
        var obj = Json.NewObject();
        Json.SetStr(obj, "name", self.name);
        Json.SetInt(obj, "level", self.level);
        Json.SetInt(obj, "health", self.health);
        return obj;
    }
}

// Create a Player from a JSON representation
func playerFromJson(data: Any) -> Player {
    return new Player(
        Json.GetStr(data, "name"),
        Json.GetInt(data, "level"),
        Json.GetInt(data, "health")
    );
}

// Save a player
func saveGame(player: Player, filename: String) {
    var json = Json.FormatPretty(player.toJson(), 2);
    File.WriteAllText(filename, json);
}

// Load a player
func loadGame(filename: String) -> Player {
    var json = File.ReadAllText(filename);
    var data = Json.Parse(json);
    return playerFromJson(data);
}
```

Let us trace through saving and loading:

**Saving:**
1. `player.toJson()` creates a map-like runtime object representing the player
2. `Json.FormatPretty()` converts that to formatted text
3. `File.WriteAllText()` writes the text to disk

**Loading:**
1. `File.ReadAllText()` reads the raw text from disk
2. `Json.Parse()` parses the text into runtime maps, sequences, strings, numbers, and booleans
3. `playerFromJson()` extracts values and constructs a Player

### When to Use JSON

JSON excels in these situations:

- **APIs and web services**: JSON is the standard for REST APIs
- **Configuration files**: Human-readable and easy to edit
- **Data exchange between programs**: Nearly universal support
- **Logging structured data**: Easy to parse logs later
- **NoSQL databases**: Many store JSON documents directly

JSON struggles in these situations:

- **Tabular data**: Use CSV instead
- **Performance-critical applications**: Consider binary formats
- **When comments are needed**: JSON has no comment syntax
- **When exact numeric precision matters**: JSON numbers are inherently imprecise

---

## CSV: Simple Tabular Data

CSV (Comma-Separated Values) is the simplest widely-used format for tabular data --- data that fits in rows and columns like a spreadsheet.

### What CSV Looks Like

```csv
name,level,health,x,y
Hero,5,87.5,100.0,200.0
Wizard,3,65.0,150.0,180.0
Warrior,7,120.0,80.0,220.0
```

The first row typically contains column headers (the names of the fields). Each subsequent row is one record. Values are separated by commas.

### The Simplicity and Limits of CSV

CSV is popular because it is simple. You can create it in any text editor. Spreadsheet programs like Excel read and write it. It is easy to generate with a loop.

But CSV has significant limitations:

**No hierarchy**: CSV is flat. You cannot nest objects inside objects. There is no way to represent "a player has multiple skills, each with its own properties."

**No types**: Everything is text. The number `5` and the string `"5"` look identical. You must decide when parsing whether to interpret something as a number.

**No standardization**: Despite the name, CSV files might use tabs, semicolons, or other delimiters. They might or might not have headers. They might handle special characters differently.

**Escaping is tricky**: What if a value contains a comma? What if it contains a newline? The rules for escaping exist but vary between implementations.

### Reading CSV

```zia
bind Csv = Zanna.Data.Csv;
bind Zanna.Terminal as Terminal;
bind Convert = Zanna.Core.Convert;
bind Fmt = Zanna.Text.Fmt;

func start() {
    var row = Csv.ParseLine("Hero,5,88");
    var name = row.GetStr(0);
    var level = Convert.ToInt64(row.GetStr(1));
    var health = Convert.ToInt64(row.GetStr(2));

    Terminal.Say(name + " (Level " + Fmt.Int(level) + ", HP: " + Fmt.Int(health) + ")");
}
```

Step by step:
1. `Csv.ParseLine()` parses one CSV record and returns a sequence of fields
2. `GetStr(index)` reads a field by zero-based index
3. String values like `level` must be explicitly converted to integers

### Writing CSV

```zia
bind Csv = Zanna.Data.Csv;
bind Seq = Zanna.Collections.Seq;
bind Zanna.Terminal as Terminal;

func start() {
    var fields = Seq.New();
    fields.Push("Hero");
    fields.Push("5");
    fields.Push("88");

    var line = Csv.FormatLine(fields);
    Terminal.Say(line);
}
```

Notice that all values are written as strings. CSV does not have a concept of numbers --- they are just text that looks like numbers.

### Handling Edge Cases

Real-world CSV data is messy. What if a value contains a comma?

```text
"Doe, John",5,87.5
```

Values with commas must be quoted. What if a value contains a quote?

```text
"He said ""hello""",5,87.5
```

Quotes inside quoted strings are doubled. What if a value spans multiple lines?

```text
"This is a
multi-line note",5,87.5
```

The entire multi-line value is quoted.

The CSV library handles these automatically:

```zia
var row = csv.createRow();
row.Set("name", "Doe, John");       // Has comma, will be quoted
row.Set("quote", "He said \"hi\""); // Has quotes, will be escaped
row.Set("notes", "Line 1\nLine 2"); // Has newline, will be quoted
```

When writing CSV, trust the library to escape properly. When reading, the library unescapes automatically.

### When to Use CSV

CSV is ideal for:

- **Exporting data to spreadsheets**: Excel and Google Sheets open CSV natively
- **Simple tabular data**: Lists of records with uniform fields
- **Data migration**: Moving data between systems
- **Large datasets**: CSV parsers are fast and memory-efficient

Avoid CSV for:

- **Nested or hierarchical data**: Use JSON or XML instead
- **Data with complex types**: CSV only represents strings
- **Data interchange between programs**: JSON is more robust
- **Data that will be edited by non-technical users**: Spreadsheet formats are safer

---

## Step-by-Step Traces: Serialization and Parsing in Action

Understanding data formats deeply requires tracing through the process step by step. Let us do that for both serialization and parsing.

### Trace: Serializing a Player to JSON

We start with a Player class in memory:

```zia
var player = Player();
player.name = "Hero";
player.level = 5;
player.health = 87.5;
player.inventory = ["sword", "shield"];
player.x = 100.0;
player.y = 200.0;
```

Memory visualization:
```text
player:
  name -----> "Hero"
  level ----> 5
  health ---> 87.5
  inventory -> ["sword", "shield"]
  x ---------> 100.0
  y ---------> 200.0
```

Now we call `player.toJSON()`. Here is what happens:

**Step 1**: Create an empty JSON object
```text
obj: {}
```

**Step 2**: `obj.Set("name", self.name)` --- Add the name
```text
obj: {"name": "Hero"}
```

**Step 3**: `obj.Set("level", self.level)` --- Add the level
```text
obj: {"name": "Hero", "level": 5}
```

**Step 4**: `obj.Set("health", self.health)` --- Add the health
```text
obj: {"name": "Hero", "level": 5, "health": 87.5}
```

**Step 5**: Create an empty JSON array for inventory
```text
inv: []
```

**Step 6**: Loop through inventory, adding each item
```text
inv.Add("sword")  -> inv: ["sword"]
inv.Add("shield") -> inv: ["sword", "shield"]
```

**Step 7**: `obj.Set("inventory", inv)` --- Attach inventory to player
```text
obj: {"name": "Hero", "level": 5, "health": 87.5, "inventory": ["sword", "shield"]}
```

**Step 8**: Create position object
```text
pos: {}
pos.Set("x", 100.0) -> pos: {"x": 100.0}
pos.Set("y", 200.0) -> pos: {"x": 100.0, "y": 200.0}
```

**Step 9**: `obj.Set("position", pos)` --- Attach position
```text
obj: {"name": "Hero", "level": 5, "health": 87.5, "inventory": ["sword", "shield"], "position": {"x": 100.0, "y": 200.0}}
```

**Step 10**: `Json.FormatPretty(obj, 2)` --- Convert the structure to formatted text

The final result is a string containing the JSON representation.

### Trace: Parsing JSON into a Player

Now the reverse. We have this JSON text:

```json
{
    "name": "Hero",
    "level": 5,
    "health": 87.5,
    "inventory": ["sword", "shield"],
    "position": {"x": 100.0, "y": 200.0}
}
```

We call `Json.Parse(jsonText)`:

**Step 1**: The parser reads the opening `{` and knows this is an object. It creates a runtime map for the object.

**Step 2**: It reads `"name"` (a key), then `:`, then `"Hero"` (a string value). It stores `"name" -> "Hero"` in the object.

**Step 3**: It reads the comma, then `"level"`, then `:`, then `5` (a number). It stores `"level" -> 5`.

**Step 4**: Continue for `"health"`, storing `"health" -> 87.5`.

**Step 5**: It reads `"inventory"`, then `:`, then `[`. This starts an array. It reads `"sword"`, comma, `"shield"`, then `]`. It stores `"inventory" -> ["sword", "shield"]`.

**Step 6**: It reads `"position"`, then `:`, then `{`. This is a nested object. It parses `"x": 100.0` and `"y": 200.0`, then `}`. It stores `"position" -> {"x": 100.0, "y": 200.0}`.

**Step 7**: It reads the final `}`. Parsing is complete.

The result is a tree structure in memory:

```text
data:
  "name" -----> string "Hero"
  "level" ----> number 5
  "health" ---> number 87.5
  "inventory" -> Seq["sword", "shield"]
  "position" -> Map{
                   "x" -> number 100.0
                   "y" -> number 200.0
                 }
```

Now we call `playerFromJson(data)`:

**Step 1**: Create an empty Player
```text
player: Player()
```

**Step 2**: `Json.GetStr(data, "name")` extracts "Hero"
```text
player.name = "Hero"
```

**Step 3**: `Json.GetInt(data, "level")` extracts 5
```text
player.level = 5
```

**Step 4**: A numeric accessor extracts `87.5`; use integer helpers for whole-number fields and raw unboxing for floating-point values.
```text
player.health = 87.5
```

**Step 5**: The `"inventory"` field is parsed as a sequence we can iterate:
```text
for item in array:
    item = "sword"
    player.inventory.Push("sword")

    item = "shield"
    player.inventory.Push("shield")
```

**Step 6**: `JsonPath.GetInt(data, "$.position.x")` navigates nested objects:
```text
data -> "position" map -> "x" number -> 100

player.x = 100
player.y = 200
```

**Step 7**: Return the fully populated Player.

The data has completed its round trip: program data to text and back to program data.

---

## Comparing Data Formats

Different formats suit different needs. Here is a comprehensive comparison:

| Aspect | JSON | CSV | XML | INI | Binary |
|--------|------|-----|-----|-----|--------|
| Human-readable | Yes | Yes | Yes | Yes | No |
| Hierarchical | Yes | No | Yes | Limited | Yes |
| Types | String, Number, Bool, Null | Strings only | Strings only | Strings only | Any |
| Size | Medium | Small | Large | Small | Very small |
| Parse speed | Fast | Very fast | Slow | Fast | Fastest |
| Schema support | Via JSON Schema | No | Via XSD | No | Custom |
| Comments | No | No | Yes | Yes | N/A |
| Best for | APIs, configs | Spreadsheets | Documents | Simple config | Performance |

### Choosing the Right Format

**Use JSON when:**
- Exchanging data with web services
- Creating configuration files developers will edit
- Storing hierarchical data
- Interoperability across languages and platforms matters

**Use CSV when:**
- Data is inherently tabular (rows and columns)
- Users will open files in spreadsheets
- Simplicity outweighs flexibility
- Dealing with very large datasets where parsing speed matters

**Use XML when:**
- Working with enterprise systems that expect XML
- You need document-oriented data with mixed content
- Schema validation and namespaces are required
- Building configuration for tools that expect XML

**Use INI when:**
- Very simple key-value configuration
- Users need to edit files manually
- You want something simpler than JSON for basic settings

**Use binary when:**
- File size is critical (games with many assets)
- Parsing speed is critical (real-time systems)
- Security through obscurity is acceptable (though not robust security)
- You control both the writer and reader completely

---

## Practical Example: A Contact Manager

Let us build a complete application that manages contacts, demonstrating serialization and parsing in a real context:

```zia
module ContactManager;

bind Json = Zanna.Data.Json;
bind Seq = Zanna.Collections.Seq;
bind File = Zanna.IO.File;
bind Zanna.Terminal as Terminal;
bind Fmt = Zanna.Text.Fmt;

// ============================================
// Data Model
// ============================================

class Contact {
    expose name: String;
    expose phone: String;
    expose email: String;

    expose func init(name: String, phone: String, email: String) {
        self.name = name;
        self.phone = phone;
        self.email = email;
    }

    expose func toJson() -> Any {
        var obj = Json.NewObject();
        Json.SetStr(obj, "name", self.name);
        Json.SetStr(obj, "phone", self.phone);
        Json.SetStr(obj, "email", self.email);
        return obj;
    }
}

func contactFromJson(data: Any) -> Contact {
    return new Contact(
        Json.GetStr(data, "name"),
        Json.GetStr(data, "phone"),
        Json.GetStr(data, "email")
    );
}

class ContactBook {
    hide contacts: List[Contact];
    hide filename: String;

    expose func init(filename: String) {
        self.filename = filename;
        self.contacts = [];
        self.load();
    }

    // Add a new contact
    expose func add(contact: Contact) {
        self.contacts.Push(contact);
        self.save();
    }

    // List all contacts
    expose func all() -> List[Contact] {
        return self.contacts;
    }

    // Persist to disk
    hide func save() {
        var arr = Seq.New();
        for contact in self.contacts {
            arr.Push(contact.toJson());
        }
        File.WriteAllText(self.filename, Json.FormatPretty(arr, 2));
    }

    // Load from disk
    hide func load() {
        if !File.Exists(self.filename) {
            return;  // No file yet, start empty
        }

        var arr = Json.ParseArray(File.ReadAllText(self.filename));
        for i in 0..Seq.get_Count(arr) {
            self.contacts.Push(contactFromJson(Seq.Get(arr, i)));
        }
    }
}

// ============================================
// User Interface
// ============================================

func printContact(contact: Contact) {
    Terminal.Say("  Name:  " + contact.name);
    Terminal.Say("  Phone: " + contact.phone);
    Terminal.Say("  Email: " + contact.email);
}

func start() {
    var book = new ContactBook("contacts.json");
    book.add(new Contact("Alice", "555-0100", "alice@example.com"));

    var contacts = book.all();
    Terminal.Say("Contacts: " + Fmt.Int(contacts.Length));
    for contact in contacts {
        printContact(contact);
    }
}
```

This example demonstrates:
- **Entity-to-JSON serialization** with `toJSON()` methods
- **JSON-to-class parsing** with `fromJSON()` methods
- **Automatic persistence** that saves after every change
- **Graceful startup** that loads existing data if available
- **Clean separation** between data model and user interface

The resulting `contacts.json` file looks like:

```json
[
    {
        "name": "Alice Smith",
        "phone": "555-1234",
        "email": "alice@example.com"
    },
    {
        "name": "Bob Jones",
        "phone": "555-5678",
        "email": "bob@example.com"
    }
]
```

Users could even edit this file manually if needed --- that is the power of human-readable formats.

---

## Common Mistakes and How to Avoid Them

Data format handling is rife with subtle bugs. Here are the most common mistakes and how to prevent them.

### Mistake: Not Validating Input

```zia
// DANGEROUS: Assumes the structure exists
var name = JsonPath.GetStr(data, "$.player.stats.name");  // Empty if missing
```

If `data` does not have a `"player"` key, or if `player` does not have `"stats"`, silently using an empty default can hide bad input. Always validate:

```zia
bind JsonPath = Zanna.Data.JsonPath;

// SAFE: Check before using the nested value
if JsonPath.Has(data, "$.player.stats.name") {
    var name = JsonPath.GetStr(data, "$.player.stats.name");
    // Now safe to use name
}

// BETTER: Use a helper function with defaults
func getNestedString(data: Any, path: String, defaultValue: String) -> String {
    if JsonPath.Has(data, path) {
        return JsonPath.GetStr(data, path);
    }
    return defaultValue;
}

var name = getNestedString(data, "$.player.stats.name", "Unknown");
```

### Mistake: Ignoring Type Mismatches

JSON numbers can be integers or floats, but your code might expect one or the other:

```zia
// PROBLEM: What if level is "5" (string) instead of 5 (number)?
var level = Json.GetInt(data, "level");  // Returns 0 when the field is not numeric
```

Defensive approach:

```zia
bind Json = Zanna.Data.Json;
bind JsonPath = Zanna.Data.JsonPath;
bind Convert = Zanna.Core.Convert;

func safeGetInt(data: Any, key: String, defaultValue: Integer) -> Integer {
    if !Json.Has(data, key) {
        return defaultValue;
    }

    var valueType = Json.TypeOf(JsonPath.Get(data, "$." + key));
    if valueType == "int" || valueType == "float" {
        return Json.GetInt(data, key);
    }
    if valueType == "str" {
        return Convert.ToInt64(Json.GetStr(data, key));
    }

    return defaultValue;
}
```

### Mistake: Forgetting Character Encoding

Text files have encodings. The same bytes can mean different characters depending on the encoding:

```zia
// PROBLEM: Incorrectly converts raw bytes to string without encoding
var bytes = File.ReadAllBytes(filename);
// bytes.ToString() is not a valid conversion. Use File.ReadAllText for UTF-8 text.

// SOLUTION: Read text files through the text API
var text = File.ReadAllText(filename);
```

Modern systems generally use UTF-8, and Zanna's text file API reads UTF-8. Keep data as bytes until you know the encoding, especially for legacy formats like Latin-1 or Windows-1252.

### Mistake: Not Handling Missing Files

```zia
// CRASH: What if the file doesn't exist?
var json = File.ReadAllText("config.json");
var data = Json.Parse(json);
```

Always check:

```zia
if File.Exists("config.json") {
    var json = File.ReadAllText("config.json");
    var data = Json.Parse(json);
    // Use data
} else {
    // Use defaults
    var data = Json.NewObject();
}
```

### Mistake: Not Versioning Your Formats

Your data format will evolve. Today's save file format might not match tomorrow's:

```zia
// VERSION 1: Just name and level
{"name": "Hero", "level": 5}

// VERSION 2: Added health
{"name": "Hero", "level": 5, "health": 100}

// VERSION 3: Added skills
{"name": "Hero", "level": 5, "health": 100, "skills": [...]}
```

If version 3 code reads a version 1 file, it will fail when looking for `"health"` and `"skills"`.

Solution: Include a version number:

```json
{
    "version": 3,
    "name": "Hero",
    "level": 5,
    "health": 100,
    "skills": [...]
}
```

```zia
bind Json = Zanna.Data.Json;

func loadPlayer(data: Any) -> Player {
    var version = 1;  // Default for old files without version
    if Json.Has(data, "version") {
        version = Json.GetInt(data, "version");
    }

    var player = new Player();
    player.name = Json.GetStr(data, "name");
    player.level = Json.GetInt(data, "level");

    if version >= 2 {
        player.health = Json.GetInt(data, "health");
    } else {
        player.health = 100;  // Default for old saves
    }

    if version >= 3 {
        // Load skills
    } else {
        player.skills = new List[String]();  // Default for old saves
    }

    return player;
}
```

### Mistake: Malformed Input Crashes the Program

What if someone gives you invalid JSON?

```zia
// CRASH: Invalid JSON causes parse to fail
var data = Json.Parse("this is { not valid json");
```

Handle parse errors:

```zia
bind Json = Zanna.Data.Json;
bind Zanna.Terminal;

if !Json.IsValid(jsonText) {
    Terminal.Say("Invalid JSON");
    return;
}
var data = Json.Parse(jsonText);
// Now safe to use data
```

### Mistake: Trusting User-Provided Data

Never trust data from external sources:

```text
// DANGEROUS: User might provide negative level or huge numbers
var level = Json.GetInt(data, "level");
player.level = level;

// SAFE: Validate the data
var level = Json.GetInt(data, "level");
if level < 1 {
    level = 1;
}
if level > 100 {
    level = 100;
}
player.level = level;
```

This is especially critical for data from files users can edit, API responses, or network input.

---

## Debugging Data Format Issues

When data does not parse correctly, systematic debugging helps.

### Technique 1: Print the Raw Data

Before parsing, see exactly what you received:

```zia
bind Zanna.Terminal;

var jsonText = File.ReadAllText("data.json");
Terminal.Say("=== RAW JSON ===");
Terminal.Say(jsonText);
Terminal.Say("================");

var data = Json.Parse(jsonText);
```

Often you will spot the problem immediately: missing quotes, trailing commas, or completely unexpected content.

### Technique 2: Validate JSON Externally

Many tools validate JSON:
- Online validators like jsonlint.com
- Command-line tools like `jq`
- IDE plugins that highlight syntax errors

Paste your JSON into a validator to get precise error messages.

### Technique 3: Inspect the Parsed Structure

After parsing, inspect what you got:

```zia
bind Zanna.Terminal;
bind Json = Zanna.Data.Json;

func debugJSON(data: Any) {
    Terminal.Say("type: " + Json.TypeOf(data));
    Terminal.Say(Json.FormatPretty(data, 2));
}

debugJSON(data);
```

This shows you the structure and types of everything in the parsed data.

### Technique 4: Test Round-Trips

Create data, serialize it, parse it back, and compare:

```zia
var original = new Player();
original.name = "Test";
original.level = 5;
// ... set all fields

var json = Json.Format(original.toJson());
var parsed = Json.Parse(json);
var restored = playerFromJson(parsed);

// Compare
if original.name != restored.name {
    Terminal.Say("Name mismatch!");
}
if original.level != restored.level {
    Terminal.Say("Level mismatch!");
}
// ... check all fields
```

If round-trip works, your serialization is correct. If it fails, you know where to look.

### Technique 5: Handle Errors Gracefully

Instead of crashing on bad data, log the problem and continue:

```zia
bind Json = Zanna.Data.Json;
bind Zanna.Terminal;

func loadPlayerSafe(filename: String) -> Player? {
    if !File.Exists(filename) {
        Terminal.Say("Warning: Save file not found, using defaults");
        return null;
    }

    var json = File.ReadAllText(filename);
    if !Json.IsValid(json) {
        Terminal.Say("Warning: Invalid save file format");
        return null;
    }

    var data = Json.Parse(json);

    if !Json.Has(data, "name") || !Json.Has(data, "level") {
        Terminal.Say("Warning: Save file missing required fields");
        return null;
    }

    return playerFromJson(data);
}

func start() {
    var player = loadPlayerSafe("save.json");
    if player == null {
        player = new Player();  // Start fresh
        player.name = "New Player";
        player.level = 1;
    }
    // Continue with game
}
```

---

## The Two Languages

**Zia**
```zia
bind Json = Zanna.Data.Json;

var data = Json.Parse("{\"name\": \"test\", \"value\": 42}");
var name = Json.GetStr(data, "name");

var obj = Json.NewObject();
Json.SetInt(obj, "score", 100);
var json = Json.Format(obj);
```

**BASIC-style pseudocode**
```text
DIM data AS OBJECT
data = JSON_PARSE("{""name"": ""test"", ""value"": 42}")
DIM name AS STRING
name = JSON_GET_STRING(data, "name")

DIM obj AS OBJECT
obj = JSON_OBJECT()
JSON_SET obj, "score", 100
DIM json AS STRING
json = JSON_TOSTRING(obj)
```

The current JSON runtime is exposed through Zia under `Zanna.Data.Json`. The BASIC-style block is conceptual pseudocode.

---

## Binary Formats: When Text Is Not Enough

Sometimes text formats are too slow or too large. Binary formats pack data efficiently at the cost of human readability.

### When to Consider Binary

- **Games with large save files**: A player's complete world state might be megabytes
- **Real-time data**: Parsing text takes time that matters in performance-critical paths
- **Bandwidth-constrained networks**: Every byte counts on mobile or slow connections
- **Proprietary formats**: When you do not want users casually editing files

### Designing a Binary Format

A well-designed binary format includes:

**1. Magic number** --- A unique identifier that tells you "this is a ZannaSave file"
```zia
final MAGIC = 0x56535631;  // "VSV1" in ASCII
```

**2. Version number** --- Allows format evolution
```zia
final VERSION = 1;
```

**3. Predictable layout** --- Fields in a consistent order with known sizes

**4. Length prefixes** --- For variable-length data like strings and arrays

Here is a complete binary save example:

```zia
bind Buffer = Zanna.IO.BinaryBuffer;
bind File = Zanna.IO.File;
bind Zanna.Terminal;

final MAGIC = 0x56535631;  // "VSV1"
final VERSION = 1;

class GameSave {
    expose version: Integer;
    expose playerName: String;
    expose level: Integer;
    expose health: Integer;
    expose x: Integer;
    expose y: Integer;

    expose func init(playerName: String, level: Integer, health: Integer, x: Integer, y: Integer) {
        self.version = VERSION;
        self.playerName = playerName;
        self.level = level;
        self.health = health;
        self.x = x;
        self.y = y;
    }
}

func saveBinary(save: GameSave, filename: String) {
    var writer = Buffer.New();

    // Header
    writer.WriteU32LittleEndian(MAGIC);
    writer.WriteU32LittleEndian(VERSION);

    // Player data
    writer.WriteStr(save.playerName);
    writer.WriteI32LittleEndian(save.level);
    writer.WriteI32LittleEndian(save.health);
    writer.WriteI32LittleEndian(save.x);
    writer.WriteI32LittleEndian(save.y);

    File.WriteAllBytes(filename, writer.ToBytes());
}

func loadBinary(filename: String) -> GameSave? {
    var reader = Buffer.FromBytes(File.ReadAllBytes(filename));

    // Verify magic number
    var magic = reader.ReadU32LittleEndian();
    if magic != MAGIC {
        Terminal.Say("Error: Not a valid save file");
        return null;
    }

    // Check version
    var version = reader.ReadU32LittleEndian();
    if version > VERSION {
        Terminal.Say("Error: Save file is from a newer version");
        return null;
    }

    // Load data
    var save = new GameSave(
        reader.ReadStr(),
        reader.ReadI32LittleEndian(),
        reader.ReadI32LittleEndian(),
        reader.ReadI32LittleEndian(),
        reader.ReadI32LittleEndian()
    );
    save.version = version;
    return save;
}
```

The resulting file is much smaller than JSON and loads faster, but you cannot open it in a text editor to see what is inside. The example keeps the binary layout intentionally small; arrays use the same pattern as `WriteStr`: write a count, then each value in order.

---

## A Complete Example: Configuration System

Here is a production-quality configuration system that demonstrates everything we have learned:

```zia
module ConfigSystem;

bind Json = Zanna.Data.Json;
bind File = Zanna.IO.File;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

class Config {
    hide data: Any;
    hide filename: String;
    hide dirty: Boolean;

    // Initialize with a filename, loading existing data if available
    expose func init(filename: String) {
        self.filename = filename;
        self.dirty = false;
        self.data = self.loadObject();
    }

    hide func loadObject() -> Any {
        if File.Exists(self.filename) {
            var text = File.ReadAllText(self.filename);
            if Json.IsValid(text) {
                return Json.Parse(text);
            }

            Terminal.Say("Warning: Config file invalid, using defaults");
        }
        return Json.NewObject();
    }

    // Get values with type-safe defaults
    expose func getString(key: String, defaultValue: String) -> String {
        if Json.Has(self.data, key) {
            return Json.GetStr(self.data, key);
        }
        return defaultValue;
    }

    expose func getInt(key: String, defaultValue: Integer) -> Integer {
        if Json.Has(self.data, key) {
            return Json.GetInt(self.data, key);
        }
        return defaultValue;
    }

    expose func getBool(key: String, defaultValue: Boolean) -> Boolean {
        if Json.Has(self.data, key) {
            return Json.GetBool(self.data, key);
        }
        return defaultValue;
    }

    expose func setString(key: String, value: String) {
        Json.SetStr(self.data, key, value);
        self.dirty = true;
    }

    expose func setInt(key: String, value: Integer) {
        Json.SetInt(self.data, key, value);
        self.dirty = true;
    }

    expose func setBool(key: String, value: Boolean) {
        Json.SetBool(self.data, key, value);
        self.dirty = true;
    }

    // Save to disk only if there are unsaved changes
    expose func save() {
        if self.dirty {
            var text = Json.FormatPretty(self.data, 2);
            File.WriteAllText(self.filename, text);
            self.dirty = false;
        }
    }

    // Force reload from disk, discarding unsaved changes
    expose func reload() {
        self.data = self.loadObject();
        self.dirty = false;
    }
}

// Usage example
func start() {
    var config = new Config("game_settings.json");

    // Read with defaults (works even if file doesn't exist or keys are missing)
    var volumePercent = config.getInt("audio_volume_percent", 80);
    var fullscreen = config.getBool("graphics_fullscreen", false);
    var playerName = config.getString("player_name", "New Player");
    var difficulty = config.getInt("game_difficulty", 2);

    Terminal.Say("Current settings:");
    Terminal.Say("  Volume: " + Fmt.Int(volumePercent) + "%");
    Terminal.Say("  Fullscreen: " + Fmt.Bool(fullscreen));
    Terminal.Say("  Player: " + playerName);
    Terminal.Say("  Difficulty: " + Fmt.Int(difficulty));

    // Modify settings
    config.setInt("audio_volume_percent", 50);
    config.setBool("graphics_fullscreen", true);
    config.setString("player_name", "Hero");

    // Persist changes
    config.save();

    Terminal.Say("");
    Terminal.Say("Settings saved!");
}
```

The resulting configuration file:

```json
{
    "audio_volume_percent": 50,
    "graphics_fullscreen": true,
    "player_name": "Hero",
    "game_difficulty": 2
}
```

This system demonstrates:
- **Graceful handling of missing files and invalid data**
- **Typed key access with defaults**
- **Dirty tracking to avoid unnecessary writes**
- **Pretty JSON output for human-edited configuration**

---

## Summary

Data formats are the bridge between your program and the outside world. Here is what we learned:

**Core Concepts:**
- **Serialization** converts program data to a storable format
- **Parsing** converts stored data back to program data
- **Formats are contracts** between writers and readers

**JSON:**
- Universal format for structured data
- Objects, arrays, strings, numbers, booleans, null
- Human-readable and well-supported
- Use `.toJSON()` and `.fromJSON()` patterns for clean code

**CSV:**
- Simple format for tabular data
- Rows and columns, no nesting
- Great for spreadsheet interoperability
- Watch out for escaping special characters

**Binary Formats:**
- Compact and fast but not human-readable
- Include magic numbers and version fields
- Document your format carefully

**Best Practices:**
- Always validate input data
- Use versioning for format evolution
- Handle errors gracefully
- Test round-trips (serialize then parse)
- Choose the right format for your use case

---

## Exercises

**Exercise 23.1**: Write a program that reads a JSON file containing a list of books (title, author, year, pages) and prints them sorted by year. Handle the case where the file does not exist or is invalid JSON.

**Exercise 23.2**: Create a program that converts JSON to CSV and vice versa. For JSON-to-CSV, assume the JSON is an array of objects with uniform keys. For CSV-to-JSON, use the first row as headers and convert each subsequent row to an object.

**Exercise 23.3**: Build a contact manager (like the example in this chapter) but add support for contacts having multiple phone numbers and multiple email addresses. Design the JSON structure to support this.

**Exercise 23.4**: Create a settings system that supports inheritance: load `default.json` first, then override with values from `user.json`. Values in user.json should replace the same keys from default.json, but keys only in default.json should still appear.

**Exercise 23.5**: Design a binary save format for a simple RPG character (name, class, level, stats array, inventory list). Include a magic number and version. Write save and load functions with version checking.

**Exercise 23.6**: Write a JSON validator that checks whether a player save file has all required fields (`name`, `level`, `health`), whether numeric fields are within valid ranges, and reports all problems found (not just the first one).

**Exercise 23.7** (Challenge): Build a data migration tool. Given a "version 1" save file format and a "version 2" format (you define the differences), write code that reads version 1 files and converts them to version 2 format.

**Exercise 23.8** (Challenge): Create a CSV parser from scratch --- without using the CSV library. Handle quoted fields, escaped quotes, and fields containing newlines. This teaches you how complex "simple" formats really are.

**Exercise 23.9** (Challenge): Implement a simple JSON schema validator. Define a schema format (e.g., `{"name": {"type": "string", "required": true}, "age": {"type": "number", "min": 0}}`) and write code that validates JSON data against such schemas.

---

*We understand how to move data between our programs and the outside world. But what about doing multiple things at once? Next, we explore concurrency --- running tasks in parallel, handling events, and building responsive applications.*

*[Continue to Chapter 24: Concurrency](24-concurrency.md)*
