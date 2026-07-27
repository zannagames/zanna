---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 9: Files and Persistence

Everything we've built so far disappears when the program ends. Run it again, and it starts fresh — no memory of what happened before.

Real programs need to remember things: your documents, game saves, settings, databases. They read data created by other programs. They produce files that outlive them.

This chapter teaches you to work with the file system — the persistent storage that survives when your program stops. File operations are fundamental to almost every real-world application, yet they're also where many things can go wrong. We'll learn not just how to read and write files, but how to do so safely and correctly.

---

## What IS a File?

Before we dive into code, let's understand what files actually are. This conceptual foundation will help you make better decisions about when and how to use them.

### The Two Types of Computer Memory

Your computer has two fundamentally different types of memory:

**RAM (Random Access Memory)** — This is where your program lives while it's running. When you create a variable like `var name = "Alice"`, that string is stored in RAM. RAM is:
- **Fast** — accessing data takes nanoseconds (billionths of a second)
- **Temporary** — everything in RAM disappears when power is lost
- **Limited** — a typical computer has 8-32 GB of RAM
- **Expensive** — costs roughly $5 per gigabyte

**Storage (Hard Drive or SSD)** — This is where files live. Your documents, photos, programs, and operating system are all stored here. Storage is:
- **Slower** — accessing data takes microseconds to milliseconds (thousands of times slower than RAM)
- **Permanent** — data survives power loss, restarts, even moving the drive to another computer
- **Abundant** — a typical computer has 256 GB to 2+ TB of storage
- **Cheap** — costs roughly $0.05-0.10 per gigabyte

### Files Bridge the Gap

A file is simply a named collection of data stored on permanent storage. When you save a document in a word processor, it writes the document's contents to a file. When you open that document later (even days or years later), it reads the contents back from that file.

Think of it like this:
- RAM is like your desk — you can work with things quickly, but when you leave for the day, you need to put things away
- Storage is like a filing cabinet — slower to access, but things stay there until you deliberately remove them
- Files are like the folders in that cabinet — each has a name, and contains your actual data

### Why We Need Files

Programs need files for several reasons:

**Persistence** — The most obvious reason. When a user creates something (a document, game save, drawing), they expect it to exist tomorrow. Without files, every time you closed Notepad, your text would vanish. Every time you quit a game, you'd have to start over.

**Data sharing** — Files allow different programs to exchange data. You might export data from a spreadsheet as a CSV file, then import it into a database program. You might download a PDF that was created by software you've never heard of.

**Configuration** — Programs need to remember your preferences. Dark mode. Font size. Last opened file. These settings are stored in configuration files so they persist between runs.

**Large data sets** — Sometimes you need to work with more data than fits in RAM. A video editing program doesn't load your entire 50 GB movie into RAM — it reads pieces from the file as needed.

**Logging and debugging** — Programs write log files to record what they did. When something goes wrong, you can examine the log to understand what happened.

---

## Understanding File Paths

Before you can read or write a file, you need to tell the computer which file you mean. This is done through *paths* — addresses that specify where a file lives in the file system.

### The File System Hierarchy

Files are organized in a tree structure of directories (also called folders). At the top is the *root* directory, and everything branches down from there.

On Linux and Mac, the root is simply `/`:

```text
/
├── home/
│   └── alice/
│       ├── Documents/
│       │   ├── report.txt
│       │   └── notes.txt
│       └── Pictures/
│           └── vacation.jpg
├── etc/
│   └── config.ini
└── usr/
    └── bin/
        └── zanna
```

On Windows, each drive has its own root (C:\, D:\, etc.):

```text
C:\
├── Users\
│   └── Alice\
│       ├── Documents\
│       │   ├── report.txt
│       │   └── notes.txt
│       └── Pictures\
│           └── vacation.jpg
└── Program Files\
    └── Zanna\
        └── zanna.exe
```

### Absolute Paths

An *absolute path* specifies the complete location of a file, starting from the root. It works no matter what directory you're currently "in."

**Linux/Mac examples:**
```zia
"/home/alice/Documents/report.txt"
"/etc/config.ini"
"/usr/bin/zanna"
```

**Windows examples:**
```zia
"C:\\Users\\Alice\\Documents\\report.txt"
"C:\\Program Files\\Zanna\\zanna.exe"
```

Notice Windows uses backslashes (`\`), but since backslash is also the escape character in strings, you need to write `\\` to get a single backslash.

**Good news:** Zanna handles this automatically. You can use forward slashes everywhere, and Zanna will convert them appropriately for the operating system:

```zia
bind File = Zanna.IO.File;

// This works on ALL operating systems
var path = "C:/Users/Alice/Documents/report.txt";
var content = File.ReadAllText(path);
```

### Relative Paths

A *relative path* specifies a location relative to the "current working directory" — typically the directory where your program is running.

```zia
"data.txt"              // In the current directory
"data/scores.txt"       // In a 'data' subdirectory
"../config.txt"         // In the parent directory
"../shared/data.txt"    // Up one level, then into 'shared'
```

The special names have meanings:
- `.` means "current directory"
- `..` means "parent directory"

If your program is running from `/home/alice/myprogram/`, then:
- `"data.txt"` refers to `/home/alice/myprogram/data.txt`
- `"data/scores.txt"` refers to `/home/alice/myprogram/data/scores.txt`
- `"../config.txt"` refers to `/home/alice/config.txt`

### When to Use Which

**Use relative paths when:**
- Files belong to your program (data files shipped with your app)
- You want your program to be portable (work in different locations)
- You're working with files the user provides in the current location

**Use absolute paths when:**
- You need to access a specific system location
- Working with user-specified paths (which users typically enter as absolute)
- You need certainty about which file you're accessing

**A warning about relative paths:** They depend on the current working directory, which can vary based on how the program was launched. A program started from the command line might have a different working directory than one started by double-clicking an icon.

### Path Manipulation

The `Path` module helps work with paths safely:

```zia
bind Zanna.IO.Path as Path;

var path = "/home/alice/documents/report.txt";

Path.Name(path);     // "report.txt"
Path.Extension(path);      // ".txt"
Path.Directory(path);      // "/home/alice/documents"
Path.Stem(path);     // "report" (name without extension)

// Building paths safely
var dir = "/home/alice";
var file = "documents/report.txt";
var full = Path.Join(dir, file);  // "/home/alice/documents/report.txt"
```

Always use `Path.Join` instead of string concatenation:

```zia
bind Zanna.IO.Path as Path;

// Bad: might produce "/home/alice//documents" or wrong separators
var directory = "/home/alice";
var filename = "documents/report.txt";
var badPath = directory + "/" + filename;

// Good: handles edge cases correctly
var goodPath = Path.Join(directory, filename);
```

### Cross-Platform Considerations

If you're writing software that might run on different operating systems, keep these differences in mind:

| Aspect | Windows | Linux/Mac |
|--------|---------|-----------|
| Path separator | `\` (backslash) | `/` (forward slash) |
| Root | `C:\`, `D:\`, etc. | `/` |
| Case sensitivity | Not case-sensitive | Case-sensitive |
| Home directory | `C:\Users\Name` | `/home/name` (Linux) or `/Users/Name` (Mac) |

**Case sensitivity matters!** On Linux, `Report.txt` and `report.txt` are different files. On Windows, they're the same. If you develop on Windows and deploy on Linux (common for web servers), this can cause surprising bugs.

To get standard locations portably:

```zia
bind Zanna.System.Machine as Machine;
bind Zanna.IO.Dir as Dir;

var home = Machine.Home;        // User's home directory
var temp = Machine.Temp;        // Temporary file directory
var cwd = Dir.Current();        // Current working directory
```

---

## How File Operations Work

When you read or write a file, several things happen behind the scenes. Understanding this helps you write more efficient and correct code.

### Opening a File

Before you can read or write, the file must be *opened*. Opening a file:

1. Tells the operating system you want to access this file
2. Checks that the file exists (for reading) or can be created (for writing)
3. Checks that you have permission to perform the operation
4. Creates a *file handle* — a connection between your program and the file
5. Sets up a buffer for efficient reading/writing

### Reading Data

When you read from a file:

1. Your program requests data from the operating system
2. The OS checks if the data is already in its cache (frequently accessed data is kept in RAM)
3. If not cached, the OS tells the hard drive to retrieve the data
4. The data travels from the drive, through the operating system, into your program's memory
5. Your program processes the data

### Writing Data

When you write to a file:

1. Your program sends data to the operating system
2. The OS typically buffers this data (holds it temporarily in RAM)
3. At some point — when the buffer fills up, when you close the file, or when you explicitly request it — the OS writes the buffered data to the actual storage device
4. The drive physically stores the data

**Important:** Because of buffering, data might not be on disk immediately after you write it. If power is lost before the buffer is flushed, that data can be lost.

### Closing a File

When an API exposes a long-lived file handle, closing it is crucial:

1. Flushes any buffered writes to disk
2. Releases the file handle back to the operating system
3. Allows other programs to access the file
4. Frees up system resources

Zanna's current `Zanna.IO.File` helpers (`ReadAllText`, `WriteAllText`, `Append`, and related calls) open and close the file for each operation. If you use an API that gives you an explicit handle, close it when you're done. Failing to close file handles can cause:
- Data loss (unflushed buffers)
- Resource exhaustion (too many open files)
- Files being locked (other programs can't access them)
- Memory leaks

---

## File Modes: Read, Write, Append

When you open a file, you specify what you intend to do with it. This is called the file *mode*.

### Read Mode

Read mode opens an existing file for reading. The file must already exist — if it doesn't, you'll get an error.

```zia
bind File = Zanna.IO.File;

var content = File.ReadAllText("data.txt");
```

The file is opened, read from beginning to end, and then closed automatically. Your program receives the contents as a string.

### Write Mode

Write mode creates a new file or overwrites an existing one. This is destructive — if the file exists, its contents are erased and replaced.

```zia
bind File = Zanna.IO.File;

File.WriteAllText("output.txt", "Hello, World!");
```

**Be careful!** Write mode doesn't ask for confirmation. If `output.txt` contained your life's work, it's now gone forever (unless you have backups).

### Append Mode

Append mode adds data to the end of an existing file, or creates a new file if it doesn't exist. Existing content is preserved.

```zia
bind File = Zanna.IO.File;

File.Append("log.txt", "New entry\n");
```

Each call adds to the end. The file grows over time. This is perfect for logs, history files, and accumulating data.

### Choosing the Right Mode

| Scenario | Mode | Function |
|----------|------|----------|
| Reading a configuration file | Read | `File.ReadAllText` |
| Saving user data | Write | `File.WriteAllText` |
| Adding to a log | Append | `File.Append` |
| Replacing a corrupted file | Write | `File.WriteAllText` |
| Recording each transaction | Append | `File.Append` |

---

## Reading Files: Step by Step

Let's explore file reading in detail, from simple to advanced techniques.

### Simple Reading: Load Everything at Once

The simplest approach loads the entire file into memory:

```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

var content = File.ReadAllText("message.txt");
Say(content);
```

**What happens:**
1. Zanna opens `message.txt` for reading
2. Reads all bytes from beginning to end
3. Converts bytes to a string
4. Closes the file
5. Returns the string to your program

**When to use:** Small files (under a few megabytes). This is simple and convenient.

**When to avoid:** Large files. If you load a 4 GB file, your program needs 4+ GB of RAM just for that string. Your computer might slow down or crash.

### Reading Lines into a Sequence

Often you want to process a file line by line. `ReadAllLines` returns a typed sequence of strings:

```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

var lines = File.ReadAllLines("data.txt");

for line in lines {
    Say("Line: " + line);
}
```

**What happens:**
1. The entire file is loaded
2. Split at line break characters (`\n` or `\r\n`)
3. Each line becomes an element in the array

This is still loading everything at once, but having lines as separate strings is often more convenient.

### Processing a Line Sequence

When line-by-line processing is clearer than working with one large string, use `ReadAllLines` and process the returned sequence:

```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

var lines = File.ReadAllLines("huge.txt");

for line in lines {
    // Process this one line
    Say(line);
}
```

**What happens:**
1. File is opened and read
2. Contents are split into a sequence of lines
3. You process each line in order
4. The file is closed automatically

**When to use:** Files that fit comfortably in memory, but are naturally processed as records or lines. The current `Zanna.IO.File` API is whole-file oriented; it does not expose explicit stream reader handles.

### Reading with a Position

Sometimes you need to jump to a specific location:

```zia
bind File = Zanna.IO.File;

var bytes = File.ReadAllBytes("data.bin");

// Copy 50 bytes starting at position 100
var data = bytes.Slice(100, 50);
```

This is more common with binary files (like reading specific parts of an image or database file).

---

## Writing Files: Step by Step

Writing is where things can go wrong in ways that lose data. Let's understand how to write safely.

### Simple Writing

```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

var content = "Hello, File!\nThis is line two.\nAnd line three.";
File.WriteAllText("output.txt", content);
Say("File written!");
```

**What happens:**
1. Zanna checks if the file exists (if so, it will be overwritten!)
2. Creates or opens the file for writing
3. Writes all the bytes
4. Closes the file
5. The file now contains exactly what you wrote

The `\n` creates line breaks. Without them, everything would be on one line.

### Appending Data

```zia
bind File = Zanna.IO.File;

File.Append("log.txt", "Event 1 occurred\n");
File.Append("log.txt", "Event 2 occurred\n");
File.Append("log.txt", "Event 3 occurred\n");
```

After these three calls, `log.txt` contains:
```text
Event 1 occurred
Event 2 occurred
Event 3 occurred
```

Append is safer than write because you can't accidentally erase data.

### Building Output Before Writing

For generated output, build the content first and then write it in one call:

```zia
bind File = Zanna.IO.File;
bind Zanna.Text.StringBuilder as SB;

var builder = new SB();

builder.AppendLine("Header line");

for i in 0..1000 {
    builder.AppendLine("Data line " + i);
}

File.WriteAllText("output.txt", builder.ToString());
```

**Why this pattern matters:**
- You avoid repeated `result = result + ...` copying
- `StringBuilder` avoids repeated string copying while you assemble the content
- `WriteAllText` handles open/write/close for you

### Ensuring Data Is Written

`WriteAllText`, `WriteAllBytes`, and `Append` open the file, perform the write, and close it before returning:

```zia
bind File = Zanna.IO.File;

File.WriteAllText("critical.txt", "Important data\n");
File.Append("critical.txt", "More data\n");
```

There is no exposed `Flush` method in the current whole-file API. If the write call reports success, the runtime has closed the file handle for that operation.

---

## Text Files vs Binary Files

Not all files are text. Understanding the difference helps you choose the right approach.

### What Are Text Files?

Text files contain human-readable characters. When you open them in Notepad, you see... text.

Examples: `.txt`, `.csv`, `.json`, `.html`, `.py`, `.md`

A text file containing "Hello" looks like this in bytes:
```text
72  101  108  108  111
H   e    l    l    o
```

Each byte represents a character code (usually ASCII or UTF-8).

**Line endings matter:** Different operating systems use different characters:
- Linux/Mac: `\n` (line feed, byte 10)
- Windows: `\r\n` (carriage return + line feed, bytes 13 and 10)

Zanna handles common line-ending differences when you use `ReadAllLines` and `WriteAllText`.

### What Are Binary Files?

Binary files contain raw bytes that aren't meant to be read as text. They might be images, audio, compiled programs, or custom data formats.

Examples: `.jpg`, `.png`, `.mp3`, `.exe`, `.zip`, `.dat`

If you open a binary file in Notepad, you see gibberish — because the bytes aren't character codes, they're data in some other format.

A simple example — storing numbers efficiently:

```zia
bind File = Zanna.IO.File;
bind Zanna.Collections.Bytes as Bytes;

// Store the number 1000000 as text: needs 7 bytes ("1000000")
// Store the number 1000000 as binary: needs 4 bytes (the actual bits)

// Write binary data
var data = new Bytes(4);
data.Set(0, 0x40);
data.Set(1, 0x42);
data.Set(2, 0x0F);
data.Set(3, 0x00);  // 1000000 in little-endian
File.WriteAllBytes("number.bin", data);

// Read binary data
var bytes = File.ReadAllBytes("number.bin");
```

### When to Use Each

**Use text files when:**
- Humans need to read/edit the file (configuration, logs, source code)
- You need to work with many different programs (CSV, JSON, XML)
- Exact byte-level precision isn't critical
- Files are relatively small

**Use binary files when:**
- Storing non-text data (images, audio, video)
- Efficiency matters (binary is more compact)
- You need exact byte-level control
- Working with existing binary formats (ZIP files, databases)

### Working with Binary Files

```zia
bind File = Zanna.IO.File;
bind Zanna.Collections.Bytes as Bytes;
bind Zanna.Terminal;

// Writing binary data
var data = new Bytes(4);
data.Set(0, 0x89);
data.Set(1, 0x50);
data.Set(2, 0x4E);
data.Set(3, 0x47);  // PNG header bytes
File.WriteAllBytes("header.bin", data);

// Reading binary data
var bytes = File.ReadAllBytes("header.bin");
Say("File size: " + File.SizeBytes("header.bin") + " bytes");

// Check for PNG signature
if bytes.Get(0) == 0x89 && bytes.Get(1) == 0x50 && bytes.Get(2) == 0x4E && bytes.Get(3) == 0x47 {
    Say("This is a PNG file!");
}
```

Binary file handling is more complex and usually requires understanding the specific file format. We'll use binary files more in later chapters when working with graphics and game data.

---

## Checking If Files and Directories Exist

Before reading, you might want to check if a file exists:

```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

if File.Exists("config.txt") {
    var config = File.ReadAllText("config.txt");
    Say("Loaded config");
} else {
    Say("No config file found, using defaults");
}
```

### The Race Condition Problem

There's a subtle issue with check-then-read:

```zia
bind File = Zanna.IO.File;

if File.Exists("data.txt") {
    // Another program could delete the file RIGHT HERE
    var content = File.ReadAllText("data.txt");  // Might fail!
}
```

Between checking and reading, another program (or user) could delete the file. This is called a *race condition*.

For critical code, consider using try-catch instead:

```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

try {
    var content = File.ReadAllText("data.txt");
    // Process content...
} catch(e: FileNotFound) {
    Say("File not found, using defaults");
}
```

This is atomic — there's no gap where the file could disappear.

For most programs, the check-then-read approach is fine. Race conditions mainly matter for server software or when multiple programs access the same files.

---

## Working with Directories

Files are organized in directories. Zanna provides tools to work with them.

### Checking and Creating Directories

```zia
bind Zanna.IO.Dir as Dir;
bind Zanna.Terminal;

// Check if directory exists
if Dir.Exists("saves") {
    Say("Saves directory found");
}

// Create a directory
Dir.Make("output");

// Create nested directories (creates parent directories as needed)
Dir.MakeAll("output/data/processed");
```

### Listing Directory Contents

```zia
bind Zanna.IO.Dir as Dir;
bind Zanna.Terminal;

// List all files in a directory
var files = Dir.Files("data");
for file in files {
    Say(file);
}

// List subdirectories
var dirs = Dir.Dirs("projects");
for dir in dirs {
    Say("Directory: " + dir);
}

// List everything (files and directories)
var all = Dir.List("documents");
```

### Practical Example: Finding All Text Files

```zia
bind Zanna.IO.Dir as Dir;
bind Zanna.IO.Path as Path;
bind Zanna.Terminal;

func findTextFiles(directory: String) {
    for entry in Dir.List(directory) {
        var path = Path.Join(directory, entry);

        if Dir.Exists(path) {
            // It's a directory, recurse into it
            findTextFiles(path);
        } else if entry.EndsWith(".txt") {
            Say("Found: " + path);
        }
    }
}

func start() {
    findTextFiles("documents");
}
```

---

## Error Handling for File Operations

Files are a major source of errors. The file might not exist. You might not have permission. The disk might be full. The path might be invalid.

### Common File Errors

**File not found:**
```zia
bind File = Zanna.IO.File;

// The file doesn't exist
var content = File.ReadAllText("nonexistent.txt");
// Error: File not found: nonexistent.txt
```

**Permission denied:**
```zia
bind File = Zanna.IO.File;

// You don't have permission to read/write this file
var content = File.ReadAllText("/etc/shadow");
// Error: Permission denied: /etc/shadow
```

**Disk full:**
```zia
bind File = Zanna.IO.File;

// No space left on the storage device
var massiveData = "very large content";
File.WriteAllText("huge.txt", massiveData);
// Error: No space left on device
```

**Path is a directory:**
```zia
bind File = Zanna.IO.File;

// Trying to read a directory as a file
var content = File.ReadAllText("my_folder");
// Error: Is a directory: my_folder
```

**Invalid path:**
```zia
bind File = Zanna.IO.File;

// Path contains invalid characters or is malformed
var content = File.ReadAllText("file\0name.txt");
// Error: Invalid path
```

### Handling Errors Gracefully

Never let file errors crash your program unexpectedly. Handle them:

```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

func loadConfig() -> String {
    try {
        return File.ReadAllText("config.txt");
    } catch(e: FileNotFound) {
        Say("Config file not found, creating default...");
        var defaultConfig = "theme=light\nvolume=50";
        File.WriteAllText("config.txt", defaultConfig);
        return defaultConfig;
    } catch(e: IOError) {
        Say("Cannot read config file");
        Say("Using built-in defaults");
        return "theme=light\nvolume=50";
    } catch(e) {
        Say("Unexpected error reading config: " + e.message);
        return "theme=light\nvolume=50";
    }
}
```

### User-Friendly Error Messages

Don't show raw error messages to users. Translate them:

```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

func openUserFile(path: String) -> String {
    try {
        return File.ReadAllText(path);
    } catch(e: FileNotFound) {
        Say("Sorry, the file '" + path + "' doesn't exist.");
        Say("Please check the filename and try again.");
        return "";
    } catch(e: IOError) {
        Say("Sorry, the file could not be opened.");
        Say("Check the path and your permissions, then try again.");
        return "";
    } catch(e) {
        Say("Sorry, couldn't open the file.");
        Say("Technical details: " + e.message);
        return "";
    }
}
```

---

## Safe File Writing Practices

Writing files is risky. If something goes wrong mid-write, you might end up with a corrupted file — half old data, half new data, completely unusable.

### The Danger of Direct Overwriting

```zia
bind File = Zanna.IO.File;

// Dangerous: if this fails mid-write, data.txt is corrupted
var newData = "replacement content";
File.WriteAllText("data.txt", newData);
```

If power fails, or your program crashes, or the disk has an error during the write, `data.txt` might be left in a broken state.

### Safe Writing with Temporary Files

The professional approach:

```zia
bind File = Zanna.IO.File;

func safeWrite(filename: String, content: String) {
    var tempFile = filename + ".tmp";

    // Step 1: Write to a temporary file
    File.WriteAllText(tempFile, content);

    // Step 2: Delete the old file (if it exists)
    if File.Exists(filename) {
        File.Delete(filename);
    }

    // Step 3: Rename temp file to real name
    File.Move(tempFile, filename);
}
```

Why is this safer?
- If step 1 fails, the original file is untouched
- Rename (step 3) is usually atomic on most systems — it either happens completely or not at all
- You never have a half-written file

### Backup Before Modifying

For critical data, keep a backup:

```zia
bind File = Zanna.IO.File;

func writeWithBackup(filename: String, content: String) {
    // Create backup of existing file
    if File.Exists(filename) {
        var backupName = filename + ".backup";
        File.Copy(filename, backupName);
    }

    // Now write the new content
    File.WriteAllText(filename, content);
}
```

If something goes wrong, the user still has their `.backup` file.

### Avoiding Accidental Overwrites

**Confirm before overwriting:**
```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

func saveFile(filename: String, content: String) {
    if File.Exists(filename) {
        Print("File exists. Overwrite? (y/n): ");
        var response = TryReadLine().UnwrapOrStr("").Trim().ToLower();
        if response != "y" {
            Say("Save cancelled.");
            return;
        }
    }
    File.WriteAllText(filename, content);
    Say("File saved.");
}
```

**Generate unique names:**
```zia
bind File = Zanna.IO.File;

func uniqueFilename(base: String, ext: String) -> String {
    var filename = base + ext;
    var counter = 1;

    while File.Exists(filename) {
        filename = base + "_" + counter + ext;
        counter += 1;
    }

    return filename;
}

// "report.txt", "report_1.txt", "report_2.txt", ...
var name = uniqueFilename("report", ".txt");
```

---

## Practical File Patterns

Let's look at real-world examples of how files are used.

### Configuration Files

Programs often store settings in configuration files:

```zia
module Config;

bind File = Zanna.IO.File;
bind Convert = Zanna.Core.Convert;
bind Zanna.Terminal;

final CONFIG_FILE = "settings.cfg";

// Simple key=value format
func loadConfig() -> Map[String, String] {
    var config = new Map[String, String]();

    if !File.Exists(CONFIG_FILE) {
        return config;  // Empty config if file doesn't exist
    }

    var lines = File.ReadAllLines(CONFIG_FILE);

    for line in lines {
        var trimmed = line.Trim();

        // Skip empty lines and comments
        if trimmed.Length == 0 || trimmed.StartsWith("#") {
            continue;
        }

        var separator = trimmed.IndexOf("=");
        if separator >= 0 {
            var key = trimmed.Substring(0, separator).Trim();
            var value = trimmed.Substring(separator + 1, trimmed.Length - separator - 1).Trim();
            config.set(key, value);
        }
    }

    return config;
}

func saveConfig(config: Map[String, String], keys: List[String]) {
    var content = "# Application settings\n# Edit with care!\n\n";

    for key in keys {
        content = content + key + "=" + config.get(key) + "\n";
    }

    File.WriteAllText(CONFIG_FILE, content);
}

// Usage
func start() {
    var config = loadConfig();

    // Get setting with default
    var theme = config.getOr("theme", "light");
    var volume = Convert.ToInt64(config.getOr("volume", "50"));

    Say("Theme: " + theme);
    Say("Volume: " + volume);

    // Change a setting
    config.set("theme", "dark");
    saveConfig(config, ["theme", "volume", "lastFile"]);
}
```

Example `settings.cfg`:
```text
# Application settings
# Edit with care!

theme=dark
volume=75
lastFile=/home/alice/documents/report.txt
```

### Log Files

Recording events for debugging and auditing:

```zia
module Logger;

bind Zanna.Time.DateTime as DateTime;
bind File = Zanna.IO.File;

final LOG_FILE = "application.log";

func log(level: String, message: String) {
    var ts = DateTime.Now();
    var timestamp = DateTime.Format(ts, "%Y-%m-%d %H:%M:%S");
    var entry = "[" + timestamp + "] [" + level + "] " + message + "\n";
    File.Append(LOG_FILE, entry);
}

func info(message: String) {
    log("INFO", message);
}

func warning(message: String) {
    log("WARNING", message);
}

func error(message: String) {
    log("ERROR", message);
}

// Usage
func processData() {
    info("Starting data processing");

    try {
        // ... do work ...
        info("Processed 1000 records");
    } catch {
        error("Processing failed");
    }
}
```

Example log output:
```text
[2024-03-15 10:23:45] [INFO] Application started
[2024-03-15 10:23:45] [INFO] Loading configuration
[2024-03-15 10:23:46] [INFO] Starting data processing
[2024-03-15 10:23:47] [INFO] Processed 1000 records
[2024-03-15 10:25:12] [WARNING] Disk space low
[2024-03-15 10:30:00] [ERROR] Connection timeout
```

### Save Games

Games need to persist player progress:

```zia
module GameSave;

bind Zanna.IO.Dir as Dir;
bind Zanna.IO.Path as Path;
bind File = Zanna.IO.File;
bind Zanna.Terminal;
bind Convert = Zanna.Core.Convert;

final SAVE_DIR = "saves";

func save(playerName: String, level: Integer, score: Integer,
          health: Integer, inventory: String,
          posX: Number, posY: Number, slot: Integer) {
    // Ensure save directory exists
    if !Dir.Exists(SAVE_DIR) {
        Dir.Make(SAVE_DIR);
    }

    var filename = Path.Join(SAVE_DIR, "save_" + slot + ".dat");

    var content =
        "name=" + playerName + "\n" +
        "level=" + level + "\n" +
        "score=" + score + "\n" +
        "health=" + health + "\n" +
        "inventory=" + inventory + "\n" +
        "position=" + posX + "," + posY + "\n";

    File.WriteAllText(filename, content);
    Say("Game saved to slot " + slot);
}

func load(slot: Integer) -> String {
    var filename = Path.Join(SAVE_DIR, "save_" + slot + ".dat");

    if !File.Exists(filename) {
        Say("No save found in slot " + slot);
        return "";
    }

    Say("Game loaded from slot " + slot);
    return File.ReadAllText(filename);
}

func listSaves() -> List[Integer] {
    var saves: List[Integer] = [];

    if !Dir.Exists(SAVE_DIR) {
        return saves;
    }

    for file in Dir.Files(SAVE_DIR) {
        if file.StartsWith("save_") && file.EndsWith(".dat") {
            var numStr = file.Substring(5, file.Length - 9);
            saves.Push(Convert.ToInt64(numStr));
        }
    }

    return saves;
}
```

### Data Export (CSV)

Exporting data for spreadsheets or other programs:

```zia
bind File = Zanna.IO.File;
bind Zanna.String as Str;
bind Zanna.Terminal;
bind Seq = Zanna.Collections.Seq;

func escapeCSV(value: String) -> String {
    if value.Contains(",") || value.Contains("\"") {
        return "\"" + value.Replace("\"", "\"\"") + "\"";
    }
    return value;
}

func joinCSV(values: Seq) -> String {
    var line = "";

    for i in 0..values.Count {
        if i > 0 {
            line = line + ",";
        }
        line = line + escapeCSV(values.GetStr(i));
    }

    return line;
}

func exportToCSV(filename: String, headers: Seq, rows: Seq) {
    var content = joinCSV(headers) + "\n";

    for i in 0..rows.Count {
        var row = rows.GetStr(i).Split("|");
        content = content + joinCSV(row) + "\n";
    }

    File.WriteAllText(filename, content);
    Say("Exported " + rows.Count + " rows to " + filename);
}

func start() {
    var headers = "Name,Age,City".Split(",");
    var rows = "Alice|25|New York\nBob|30|Los Angeles\nCharlie|35|Chicago".Split("\n");

    exportToCSV("people.csv", headers, rows);
}
```

### Data Import (CSV)

Reading data from CSV files:

```zia
bind File = Zanna.IO.File;
bind Zanna.String as Str;
bind Zanna.Terminal;

func importFromCSV(filename: String) -> Zanna.Collections.Seq {
    return File.ReadAllLines(filename);
}

func start() {
    var rows = importFromCSV("people.csv");
    if rows.Count == 0 {
        Say("No data");
        return;
    }

    var headers = rows.GetStr(0).Split(",");
    Say("Columns: " + Str.Join(", ", headers));
    Say("Data rows: " + (rows.Count - 1));
}
```

---

## A Complete Example: Note Keeper

Let's build a full application that demonstrates proper file handling:

```zia
module NoteKeeper;

bind File = Zanna.IO.File;
bind Convert = Zanna.Core.Convert;
bind Zanna.Terminal;

final NOTES_FILE = "notes.txt";
final BACKUP_FILE = "notes.txt.backup";

func joinNotes(notes: List[String]) -> String {
    var content = "";

    for i in 0..notes.count() {
        if i > 0 {
            content = content + "\n";
        }
        content = content + notes.get(i);
    }

    return content;
}

// Load notes from file
func loadNotes() -> List[String] {
    if !File.Exists(NOTES_FILE) {
        return [];
    }

    var loaded: List[String] = [];
    try {
        var lines = File.ReadAllLines(NOTES_FILE);
        // Filter out empty lines
        for line in lines {
            if line.Trim().Length > 0 {
                loaded.add(line);
            }
        }
    } catch {
        Say("Warning: Could not load notes");

        // Try backup
        if File.Exists(BACKUP_FILE) {
            Say("Attempting to load from backup...");
            try {
                var backupLines = File.ReadAllLines(BACKUP_FILE);
                var backupNotes: List[String] = [];
                for backupLine in backupLines {
                    backupNotes.add(backupLine);
                }
                return backupNotes;
            } catch {
                Say("Backup also failed.");
            }
        }

        return [];
    }

    return loaded;
}

// Save notes to file (with backup)
func saveNotes(notes: List[String]) -> Boolean {
    // Create backup of existing file
    if File.Exists(NOTES_FILE) {
        try {
            File.Copy(NOTES_FILE, BACKUP_FILE);
        } catch {
            Say("Warning: Could not create backup");
        }
    }

    // Write to temporary file first
    var tempFile = NOTES_FILE + ".tmp";
    var content = joinNotes(notes);
    File.WriteAllText(tempFile, content);

    // Delete old file and rename temp to real
    if File.Exists(NOTES_FILE) {
        File.Delete(NOTES_FILE);
    }
    File.Move(tempFile, NOTES_FILE);

    return true;
}

// Display all notes
func displayNotes(notes: List[String]) {
    if notes.count() == 0 {
        Say("No notes yet.");
        Say("Use 'add' to create your first note!");
        return;
    }

    Say("");
    Say("=== Your Notes ===");
    for i in 0..notes.count() {
        Say((i + 1) + ". " + notes.get(i));
    }
    Say("==================");
}

// Show help
func showHelp() {
    Say("");
    Say("Available commands:");
    Say("  list   - Show all notes");
    Say("  add    - Add a new note");
    Say("  delete - Delete a note by number");
    Say("  edit   - Edit an existing note");
    Say("  clear  - Delete all notes");
    Say("  help   - Show this help");
    Say("  quit   - Exit the program");
    Say("");
}

func start() {
    var notes = loadNotes();

    Say("===================");
    Say("   Note Keeper");
    Say("===================");
    Say("Type 'help' for commands");
    Say("");

    if notes.count() > 0 {
        Say("Loaded " + notes.count() + " existing notes.");
    }

    while true {
        Print("> ");
        var command = TryReadLine().UnwrapOrStr("").Trim().ToLower();

        if command == "quit" || command == "exit" {
            Say("Goodbye!");
            break;

        } else if command == "help" {
            showHelp();

        } else if command == "list" {
            displayNotes(notes);

        } else if command == "add" {
            Print("Enter note: ");
            var note = TryReadLine().UnwrapOrStr("").Trim();

            if note.Length == 0 {
                Say("Note cannot be empty.");
            } else {
                notes.add(note);
                if saveNotes(notes) {
                    Say("Note added!");
                }
            }

        } else if command == "delete" {
            if notes.count() == 0 {
                Say("No notes to delete.");
            } else {
                displayNotes(notes);
                Print("Delete which number? (0 to cancel): ");
                var input = TryReadLine().UnwrapOrStr("").Trim();

                try {
                    var num = Convert.ToInt64(input);
                    if num == 0 {
                        Say("Cancelled.");
                    } else if num >= 1 && num <= notes.count() {
                        var deleted = notes.get(num - 1);
                        notes.removeAt(num - 1);
                        if saveNotes(notes) {
                            Say("Deleted: " + deleted);
                        }
                    } else {
                        Say("Invalid number. Enter 1-" + notes.count());
                    }
                } catch {
                    Say("Please enter a number.");
                }
            }

        } else if command == "edit" {
            if notes.count() == 0 {
                Say("No notes to edit.");
            } else {
                displayNotes(notes);
                Print("Edit which number? (0 to cancel): ");
                var input = TryReadLine().UnwrapOrStr("").Trim();

                try {
                    var num = Convert.ToInt64(input);
                    if num == 0 {
                        Say("Cancelled.");
                    } else if num >= 1 && num <= notes.count() {
                        Say("Current: " + notes.get(num - 1));
                        Print("New text: ");
                        var newNote = TryReadLine().UnwrapOrStr("").Trim();

                        if newNote.Length == 0 {
                            Say("Note cannot be empty. Use 'delete' to remove.");
                        } else {
                            notes.set(num - 1, newNote);
                            if saveNotes(notes) {
                                Say("Note updated!");
                            }
                        }
                    } else {
                        Say("Invalid number. Enter 1-" + notes.count());
                    }
                } catch {
                    Say("Please enter a number.");
                }
            }

        } else if command == "clear" {
            if notes.count() == 0 {
                Say("Already empty.");
            } else {
                Print("Delete ALL " + notes.count() + " notes? (yes/no): ");
                var confirm = TryReadLine().UnwrapOrStr("").Trim().ToLower();

                if confirm == "yes" {
                    notes = [];
                    if saveNotes(notes) {
                        Say("All notes deleted.");
                    }
                } else {
                    Say("Cancelled.");
                }
            }

        } else if command.Length == 0 {
            // Empty input, just show prompt again
            continue;

        } else {
            Say("Unknown command: '" + command + "'");
            Say("Type 'help' for available commands.");
        }
    }
}
```

This program demonstrates:
- Loading data from files with error handling
- Saving with backups for safety
- Using temporary files to prevent corruption
- User-friendly error messages
- Input validation
- Confirmation for destructive operations

---

## The Two Languages

**Zia**
```zia
bind File = Zanna.IO.File;

// Read
var content = File.ReadAllText("file.txt");

// Write
File.WriteAllText("file.txt", "Hello!");

// Append
File.Append("file.txt", "More text\n");

// Check existence
if File.Exists("file.txt") {
    var existing = File.ReadAllText("file.txt");
}

// Read lines
var lines = File.ReadAllLines("file.txt");

// Process lines
for line in lines {
    // Work with each line
}
```

**BASIC**
```basic
' Write
OPEN "file.txt" FOR OUTPUT AS #1
PRINT #1, "Hello!"
CLOSE #1

' Read
OPEN "file.txt" FOR INPUT AS #1
DIM content AS STRING
LINE INPUT #1, content
CLOSE #1

' Append
OPEN "file.txt" FOR APPEND AS #1
PRINT #1, "More text"
CLOSE #1

' Missing files are reported when OPEN FOR INPUT fails.
```

BASIC uses file numbers (#1, #2, etc.) and requires explicit OPEN/CLOSE. The FOR clause specifies the mode: INPUT (read), OUTPUT (write), APPEND (append).

---

## Common Mistakes

**Forgetting the file might not exist:**
```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

// Crashes if file doesn't exist
var content = File.ReadAllText("maybe.txt");

// Better: check first
if File.Exists("maybe.txt") {
    var content = File.ReadAllText("maybe.txt");
}

// Or: use try-catch
try {
    var content = File.ReadAllText("maybe.txt");
} catch(e: FileNotFound) {
    Say("File not found");
}
```

**Overwriting when you meant to append:**
```zia
bind File = Zanna.IO.File;

File.WriteAllText("log.txt", "Entry 1\n");
File.WriteAllText("log.txt", "Entry 2\n");  // Oops! Entry 1 is gone

// Correct: use append for logs
File.WriteAllText("log.txt", "Entry 1\n");
File.Append("log.txt", "Entry 2\n");  // Both entries preserved
```

**Hardcoding paths:**
```zia
bind Zanna.System.Machine as Machine;
bind Zanna.IO.Path as Path;

// Bad: only works on your machine
var absoluteFile = "C:\\Users\\Alice\\Documents\\data.txt";

// Better: use relative paths
var relativeFile = "data/scores.txt";

// Or build paths dynamically
var home = Machine.Home;
var documentsFile = Path.Join(home, "Documents/data.txt");
```

**Expecting stream handles from whole-file APIs:**
```zia
bind File = Zanna.IO.File;
bind Zanna.Terminal;

// The current File API does not expose openRead/openWrite stream handles.
// Use whole-file helpers instead.
var lines = File.ReadAllLines("file.txt");
for line in lines {
    Say(line);
}
```

**Looking for a Flush method on whole-file writes:**
```zia
bind File = Zanna.IO.File;

File.WriteAllText("important.txt", "Critical data\n");
File.Append("important.txt", "More data\n");
// Each call opens, writes, and closes the file before returning.
```

**Not handling paths cross-platform:**
```zia
bind Zanna.IO.Path as Path;

// Bad: backslash doesn't work on Mac/Linux
var windowsPath = "data\\files\\scores.txt";

// Good: forward slash works everywhere (Zanna converts as needed)
var portablePath = "data/files/scores.txt";

// Best: use Path.Join
var joinedPath = Path.Join("data/files", "scores.txt");
```

---

## Summary

- **Files provide persistence** — data that survives program termination
- **RAM is fast but temporary; storage is slow but permanent**
- **Paths specify file locations** — absolute (full path) or relative (from current directory)
- **File modes determine behavior** — read, write (overwrite), append (add to end)
- **Whole-file helpers close files for you**; explicit file handles must be closed by the code that opens them
- **Handle errors gracefully** — files can fail in many ways
- **Use temporary files and backups** for safe writing
- **Text files are human-readable; binary files store raw data**
- `File.ReadAllText/File.WriteAllText/File.Append` for simple text operations
- `File.ReadAllLines/File.ReadAllBytes` for line and binary data
- `Dir` module for working with directories
- `Path` module for manipulating paths safely and portably

---

## Exercises

**Exercise 9.1**: Write a program that saves your name to a file, then reads it back and greets you. If the file exists, it should greet you; if not, it should ask for your name and save it.

**Exercise 9.2**: Write a program that reads a file and reports statistics: number of lines, number of words, number of characters, and the longest line.

**Exercise 9.3**: Write a program that copies one file to another. It should ask for the source filename, check that it exists, ask for the destination filename, warn if it already exists, and then perform the copy.

**Exercise 9.4**: Extend the Note Keeper to save timestamps with each note (e.g., "[2024-03-15 10:30] Buy groceries").

**Exercise 9.5**: Write a program that lists all `.txt` files in the current directory and its subdirectories, showing the file path and size for each.

**Exercise 9.6**: Write a simple "find and replace" program that reads a file, replaces all occurrences of one string with another, and writes the result. It should create a backup of the original file first.

**Exercise 9.7** (Challenge): Write a simple address book that stores names, phone numbers, and emails in a file. Include commands to add, search (by any field), list, edit, and delete contacts. Handle all file errors gracefully.

**Exercise 9.8** (Challenge): Write a program that can merge two sorted files. Each input file contains one number per line in ascending order. The output file should contain all numbers from both files, still in ascending order. Do this efficiently without loading both entire files into memory.

---

*We can now save and load data. But what happens when things go wrong — the file doesn't exist, the network fails, the input is invalid? Chapter 10 dives deeper into handling errors gracefully, building on the error handling we've introduced here.*

*[Continue to Chapter 10: Errors and Recovery](10-errors.md)*
