---
status: active
audience: public
last-verified: 2026-08-16
---

# Files & Directories
> File, BinFile, TempFile, Dir, Path, Glob

**Part of [Zanna Runtime Library](../README.md) › [Input & Output](README.md)**

---

## Zanna.IO.File

File system operations.

**Type:** Static utility class

### Methods

| Method                        | Signature              | Description                                                                               |
|-------------------------------|------------------------|-------------------------------------------------------------------------------------------|
| `Exists(path)`                | `Boolean(String)`      | Returns true only if the path exists and is a regular file                               |
| `SameFile(left, right)`       | `Boolean(String, String)` | Returns true when both paths resolve to the same existing regular file identity       |
| `ReadAllText(path)`           | `String(String)`       | Reads the entire file contents as a string; traps on I/O errors                           |
| `ReadAllTextBounded(path, maxBytes)` | `String(String, Integer)` | Opens once and reads the complete file only when its byte size does not exceed the nonnegative ceiling |
| `WriteAllText(path, content)` | `Void(String, String)` | Atomically replaces a text file with new contents                                          |
| `WriteAllTextNew(path, content)` | `Void(String, String)` | Atomically creates a durable text file and traps rather than replacing an existing destination |
| `CompareExchangeAllText(path, expected, desired)` | `Boolean(String, String, String)` | Atomically replaces a text file only when its complete current bytes match `expected` |
| `Delete(path)`                | `Void(String)`         | Deletes a file; missing files are ignored, other failures trap                            |
| `Copy(src, dst)`              | `Void(String, String)` | Copies a file from src to dst; traps if `dst` already exists or both paths name the same file |
| `Move(src, dst)`              | `Void(String, String)` | Moves or renames a file; traps if `dst` already exists                                    |
| `MoveOver(src, dst)`          | `Void(String, String)` | Moves or renames a file, replacing `dst` when supported by the platform                   |
| `SizeBytes(path)`             | `Integer(String)`      | Returns regular-file size in bytes, or -1 if not found or not a regular file              |
| `ReadAllBytes(path)`          | `Bytes(String)`        | Reads the entire file as binary data (traps on I/O errors)                                |
| `WriteAllBytes(path, bytes)`  | `Void(String, Bytes)`  | Atomically replaces a file with binary data (traps on I/O errors)                         |
| `ReadAllLines(path)`          | `Seq(String)`          | Reads the file as a runtime sequence of lines; traps on I/O errors                        |
| `WriteAllLines(path, lines)`  | `Void(String, Seq(String))` | Atomically writes a sequence of strings as lines; traps on I/O errors            |
| `Append(path, text)`          | `Void(String, String)` | Appends text to a file; traps on I/O errors                                               |
| `AppendLine(path, text)`      | `Void(String, String)` | Appends text followed by `\n` to a file in one atomic write (creates if missing)          |
| `ReadAllLines(path)`          | `Seq(String)`          | Reads file as a sequence of lines; strips `\n`, `\r`, or `\r\n` terminators (traps on I/O errors) |
| `Modified(path)`              | `Integer(String)`      | Returns regular-file modification time as Unix timestamp, or -1 if missing or not a file  |
| `Touch(path)`                 | `Void(String)`         | Creates a missing file or updates an existing regular file's modification time (traps on a directory) |

### Notes

- `AppendLine` always appends a single `\n` byte (no platform newline normalization).
- `AppendLine` writes the text and its `\n` as one buffer in a single `write()` on an
  `O_APPEND` file, so concurrent appenders do not split or interleave a line — as long as it
  fits in one OS write. Lines large enough to require a partial write can still interleave;
  synchronize externally when arbitrarily large records must remain indivisible.
- `Exists` returns false for directories; use `Dir.Exists` for directory checks.
- `SameFile` follows ordinary filesystem resolution and compares volume/file IDs on Windows
  or device/inode pairs on POSIX. Missing, inaccessible, malformed, and non-file paths return
  false rather than trapping. It therefore handles hard links, symlinks, and case-equivalent
  spellings without incorrectly folding distinct files on a case-sensitive filesystem.
- Path strings with embedded NUL bytes are rejected before reaching platform file APIs.
- `ReadAllText`, `ReadAllTextBounded`, `ReadAllBytes`, and `ReadAllLines` require a regular file and trap on directories, special files, I/O errors, or unexpected short reads if the file changes while being read. `ReadAllTextBounded` also rejects negative ceilings, sizes above the ceiling before allocation, and observed trailing data.
- `WriteAllText`, `WriteAllTextNew`, `WriteAllBytes`, `WriteBytes`, and `WriteLines` write to an exclusive temporary file in the destination directory, durably flush it, and atomically commit it. `WriteAllTextNew` uses a no-clobber commit and preserves any destination that appears before commit. Failed writes trap instead of silently leaving a partial file behind. Temporary sidecar names are unpredictable and do not include process memory addresses.
- `CompareExchangeAllText` serializes cooperating processes with a stable adjacent lock file,
  compares the destination byte-for-byte, and commits through the same durable atomic writer only
  on a match. A missing file matches an empty expected string. A mismatch returns `false`; I/O
  failure traps. Callers should use one normalized path spelling for a shared destination.
- `Copy` and `Move` never overwrite an existing destination. `MoveOver` is the explicit replacement operation; it first attempts an in-place replace/rename and only falls back to copy-plus-delete when the source and destination are on different filesystems or volumes.
- `Copy` preserves regular-file permission bits and modification/access times where the platform exposes them. Cross-device `Move` uses the same copy path before deleting the source.
- `Size` and `Modified` return `-1` for missing paths, directories, and special files. A real zero-byte file still reports size `0`.
- `Touch` creates a regular file when the path is missing and updates the modification time of an
  existing regular file. It is a File operation and traps on a directory operand, matching the
  rest of the `File` surface.
- `ReadAllLines` splits on `\n`, `\r`, and `\r\n` and does not include line endings in returned strings. Trailing empty lines are preserved.
- `WriteLines` / `WriteAllLines` append one LF byte after every sequence element, including the last. Their raw `seq<str>` argument is available to Zia but is currently rejected by safe BASIC; use `LineWriter` or whole-file text helpers from BASIC.
- `WriteAllText`, `ReadAllText`, `ReadBytes`, `ReadAllBytes`, `WriteAllBytes`, `ReadAllLines`, `WriteBytes`, `WriteLines`, `Append`, and `Touch` trap on I/O errors.
- POSIX file descriptors and Windows CRT handles opened by the runtime are created as close-on-exec/non-inheritable where the platform API supports it.
- `ReadBytes`/`WriteBytes` use the typed `Zanna.Collections.Bytes` runtime class. `ReadLines`/`WriteLines` use `Seq(String)`. The runtime keeps any raw buffer pointers internal.

### Zia Example

```zia
module FileDemo;

bind Zanna.Terminal;
bind Zanna.IO.File as File;
bind Zanna.IO.Path as Path;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Path utilities
    var p = Path.Join("/tmp", "test.txt");
    Say("Path: " + p);
    Say("Ext: " + Path.Extension(p));
    Say("Name: " + Path.Name(p));
    Say("Dir: " + Path.Directory(p));
    Say("Stem: " + Path.Stem(p));

    // Write and read a file
    File.WriteAllText("/tmp/zanna_test.txt", "Hello from Zia!");
    var content = File.ReadAllText("/tmp/zanna_test.txt");
    Say("Content: " + content);
    Say("Exists: " + Fmt.Bool(File.Exists("/tmp/zanna_test.txt")));

    // File size
    Say("Size: " + Fmt.Int(File.SizeBytes("/tmp/zanna_test.txt")));

    // Clean up
    File.Delete("/tmp/zanna_test.txt");
}
```

### BASIC Example

```basic
DIM filename AS STRING
filename = "data.txt"

' Write to file
Zanna.IO.File.WriteAllText(filename, "Hello, World!")

' Check if file exists
IF Zanna.IO.File.Exists(filename) THEN
    ' Read contents
    DIM content AS STRING
    content = Zanna.IO.File.ReadAllText(filename)
    PRINT content  ' Output: "Hello, World!"

    ' Delete file
    Zanna.IO.File.Delete(filename)
END IF
```

### Binary File Example

```basic
' Create binary data
DIM data AS Bytes
data = Zanna.Collections.Bytes.New(4)
data.Set(0, 72)  ' H
data.Set(1, 105)  ' i
data.Set(2, 33)  ' !
data.Set(3, 0)  ' null byte

' Write binary file
Zanna.IO.File.WriteAllBytes("test.bin", data)

' Read binary file
DIM loaded AS Bytes
loaded = Zanna.IO.File.ReadAllBytes("test.bin")
PRINT "Size:"; loaded.Length  ' Output: Size: 4
```

### Line-by-Line Example

```basic
' Safe BASIC cannot pass the raw seq<str> argument required by WriteLines.
' Write the initial text directly, or use Zanna.IO.LineWriter for generated lines.
Zanna.IO.File.WriteAllText("output.txt", "First line" + CHR(10) + "Second line" + CHR(10) + "Third line" + CHR(10))

' Read lines from file
DIM readLines AS Zanna.Collections.Seq
readLines = Zanna.IO.File.ReadAllLines("output.txt")
FOR i = 0 TO readLines.Count - 1
    PRINT readLines.Get(i)
NEXT i

' Append to file
Zanna.IO.File.Append("output.txt", "Appended text")
```

### File Management Example

```basic
' Copy file
Zanna.IO.File.Copy("source.txt", "backup.txt")

' Move/rename file
Zanna.IO.File.Move("old_name.txt", "new_name.txt")

' Replace an existing destination explicitly
Zanna.IO.File.MoveOver("new_data.txt", "data.txt")

' Use Zanna.IO.Dir.Move for directories; file moves require a regular-file source.

' Get file info
DIM size AS INTEGER
size = Zanna.IO.File.SizeBytes("data.txt")
PRINT "File size:"; size; "bytes"

DIM mtime AS INTEGER
mtime = Zanna.IO.File.Modified("data.txt")
PRINT "Modified:"; mtime

' Create empty file or update timestamp
Zanna.IO.File.Touch("marker.txt")
```

---

## Zanna.IO.BinFile

Binary file stream for reading and writing raw bytes with random access capabilities.

**Type:** Instance class

**Constructor:** `Zanna.IO.BinFile.Open(path, mode)`

### Open Modes

| Mode   | Description                               |
|--------|-------------------------------------------|
| `"r"`  | Read only (file must exist)               |
| `"rb"` | Read only binary alias                    |
| `"w"`  | Write only (creates or truncates)         |
| `"wb"` | Write only binary alias                   |
| `"rw"` | Read and write (file must exist)          |
| `"r+"` | Read and write alias                      |
| `"rb+"`, `"r+b"` | Read and write binary aliases       |
| `"a"`  | Append (creates if needed, writes at end) |
| `"ab"` | Append binary alias                       |

`Close()` and `Flush()` trap if the platform reports a delayed write or close failure.
`BinFile` also normalizes the required C stdio transition between reads and writes on `"rw"`/`"r+"` streams, so switching direction does not rely on undefined buffered-stdio state.

`BinFile.Eof` deliberately follows C stdio sticky-flag semantics rather than comparing `Position` with
`Size`: reading exactly the last byte leaves it false, and one more `Read`/`ReadByte` sets it. Use
`Pos >= Size` when the question is whether the cursor is at the logical end. Note this is the
low-level BinFile contract; the polymorphic `Stream.Eof` is position-based (`Pos >= Size`) on both
file and memory backings, so prefer `Stream` when you want uniform end-of-stream behavior.

### Properties

| Property | Type    | Description                                   |
|----------|---------|-----------------------------------------------|
| `Position` | Integer | Current file position (read-only)           |
| `SizeBytes` | Integer | Total file size in bytes (read-only)       |
| `Eof`    | Boolean | Sticky EOF flag, set after a read attempts to pass the end (read-only) |

### Methods

| Method                        | Returns | Description                                                            |
|-------------------------------|---------|------------------------------------------------------------------------|
| `Close()`                     | void    | Close the file and release resources                                   |
| `Read(bytes, offset, count)`  | Integer | Read up to count bytes into Bytes object at offset; returns bytes read |
| `Write(bytes, offset, count)` | void    | Write count bytes from Bytes object starting at offset                 |
| `ReadByte()`                  | Integer | Read single byte (0-255) or -1 at EOF                                  |
| `WriteByte(value)`            | void    | Write single byte (0-255)                                              |
| `Seek(offset, origin)`        | Integer | Seek to position; returns new position                                 |
| `Flush()`                     | void    | Flush buffered writes to disk                                          |

### Seek Origins

| Origin | Description                       |
|--------|-----------------------------------|
| `0`    | From beginning of file (SEEK_SET) |
| `1`    | From current position (SEEK_CUR)  |
| `2`    | From end of file (SEEK_END)       |

### Zia Example

```zia
module BinFileDemo;

bind Zanna.Terminal;
bind Zanna.IO.BinFile as BF;
bind Zanna.IO.File as File;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Write binary data
    var bf = BF.Open("/tmp/data.bin", "w");
    bf.WriteByte(0xCA);
    bf.WriteByte(0xFE);
    bf.WriteByte(0xBA);
    bf.WriteByte(0xBE);
    bf.Close();

    // Read binary data
    bf = BF.Open("/tmp/data.bin", "r");
    Say("Size: " + Fmt.Int(bf.get_SizeBytes()));

    var b1 = bf.ReadByte();
    var b2 = bf.ReadByte();
    Say("Byte 1: " + Fmt.Int(b1));   // 202 (0xCA)
    Say("Byte 2: " + Fmt.Int(b2));   // 254 (0xFE)

    // Seek back to start
    bf.Seek(0, 0);
    Say("After seek: " + Fmt.Int(bf.ReadByte()));
    bf.Close();

    File.Delete("/tmp/data.bin");
}
```

> **Note:** BinFile properties (`Position`, `SizeBytes`, `Eof`) use the get_/set_ pattern; access them as `bf.get_SizeBytes()`, `bf.get_Position()`, `bf.get_Eof()` in Zia.

### BASIC Example

```basic
' Write binary data
DIM bf AS OBJECT = Zanna.IO.BinFile.Open("data.bin", "w")

' Write individual bytes
bf.WriteByte(202)
bf.WriteByte(254)
bf.WriteByte(186)
bf.WriteByte(190)

' Write from a Bytes object
DIM data AS OBJECT = NEW Zanna.Collections.Bytes(4)
data.Set(0, 1)
data.Set(1, 2)
data.Set(2, 3)
data.Set(3, 4)
bf.Write(data, 0, 4)

bf.Close()

' Read binary data
bf = Zanna.IO.BinFile.Open("data.bin", "r")

' Check file size
PRINT bf.Size                 ' Output: 8

' Read byte by byte
PRINT HEX(bf.ReadByte())      ' Output: CA
PRINT HEX(bf.ReadByte())      ' Output: FE

' Seek to position
bf.Seek(0, 0)                 ' Back to start

' Read into a Bytes buffer
DIM buffer AS OBJECT = NEW Zanna.Collections.Bytes(8)
DIM bytesRead AS INTEGER = bf.Read(buffer, 0, 8)
PRINT bytesRead               ' Output: 8

' An exact read to the end does not set the sticky EOF flag
PRINT bf.Eof                  ' Output: 0
PRINT bf.ReadByte()           ' Output: -1
PRINT bf.Eof                  ' Output: 1

bf.Close()

' Read/write mode for random access
bf = Zanna.IO.BinFile.Open("data.bin", "rw")

' Seek to position 4 and overwrite
bf.Seek(4, 0)
bf.WriteByte(255)

bf.Close()
```

### Use Cases

- **Binary file formats:** Read/write structured binary data
- **Random access:** Seek to arbitrary positions in files
- **Large files:** Process files too large to load entirely into memory
- **Low-level I/O:** Direct byte-level file manipulation
- **Database files:** Read/write fixed-record binary databases

---

## Zanna.IO.TempFile

Temporary file and directory creation utilities. Generates unique paths in the system temporary directory with optional prefixes and extensions.

**Type:** Static utility class

### Methods

| Method                         | Signature              | Description                                                        |
|--------------------------------|------------------------|--------------------------------------------------------------------|
| `Dir()`                        | `String()`             | Returns the system temporary directory path                        |
| `Path()`                       | `String()`             | Generate a unique temporary file path (with `.tmp` extension)      |
| `PathWithPrefix(prefix)`       | `String(String)`       | Generate a unique temporary file path with a prefix                |
| `PathWithExt(prefix, ext)`     | `String(String, String)` | Generate a unique temporary file path with prefix and extension  |
| `Create()`                     | `String()`             | Create an empty temporary file and return its path                 |
| `CreateWithPrefix(prefix)`     | `String(String)`       | Create an empty temporary file with prefix and return its path     |
| `CreateDir()`                  | `String()`             | Create a temporary directory and return its path                   |
| `CreateDirWithPrefix(prefix)`  | `String(String)`       | Create a temporary directory with prefix and return its path       |

### Notes

- `Dir()` returns the platform temporary directory (e.g., `/tmp` on Unix, `%TEMP%` on Windows). On POSIX, an invalid relative, missing, or non-directory `TMPDIR` value is ignored and `/tmp` is used instead.
- `Path` and `PathWithPrefix` only generate unpredictable path candidates — they do not reserve or create files on disk
- `Create` and `CreateDir` actually create the file or directory on disk
- `PathWithExt("v_", ".txt")` produces a path like `/tmp/v_<unique>.txt`; prefixes are used verbatim, so include any desired separator yourself.
- Prefixes and extensions are filename fragments: `/`, `\`, and `:` are rejected, so they cannot add path components or drive prefixes.
- Prefixes and extensions also reject embedded NUL bytes; generated names are never truncated at a hidden NUL.
- On POSIX, created files use owner-only mode `0600` and created directories use `0700` (subject to platform/filesystem behavior).
- Temporary files and directories are not automatically cleaned up; the caller is responsible for deletion.

### Zia Example

```zia
module TempFileDemo;

bind Zanna.Terminal;
bind Zanna.IO.TempFile as TempFile;
bind Zanna.IO.File as File;

func start() {
    Say("Temp dir: " + TempFile.Dir());

    // Generate a unique path (does not create the file)
    var p = TempFile.Path();
    Say("Temp path: " + p);

    // Generate with prefix and extension
    var p2 = TempFile.PathWithExt("zanna_", ".txt");
    Say("Custom path: " + p2);

    // Create an actual temp file
    var created = TempFile.Create();
    Say("Created: " + created);
    File.Delete(created);
}
```

### BASIC Example

```basic
' Get the system temp directory
PRINT "Temp dir: "; Zanna.IO.TempFile.Dir()   ' Output: /tmp (or platform equivalent)

' Generate unique temp file paths (no file created)
DIM p AS STRING = Zanna.IO.TempFile.Path()
PRINT "Path: "; p   ' Output: /tmp/<unique>.tmp

DIM p2 AS STRING = Zanna.IO.TempFile.PathWithPrefix("myapp_")
PRINT "Prefixed: "; p2   ' Output: /tmp/myapp_<unique>.tmp

DIM p3 AS STRING = Zanna.IO.TempFile.PathWithExt("v_", ".txt")
PRINT "With ext: "; p3   ' Output: /tmp/v_<unique>.txt

' Create an actual temp file on disk
DIM f AS STRING = Zanna.IO.TempFile.Create()
PRINT "Created file: "; f
Zanna.IO.File.WriteAllText(f, "temp data")
Zanna.IO.File.Delete(f)

' Create a temp directory
DIM d AS STRING = Zanna.IO.TempFile.CreateDir()
PRINT "Created dir: "; d
Zanna.IO.Dir.RemoveAll(d)

' Create a temp directory with prefix
DIM d2 AS STRING = Zanna.IO.TempFile.CreateDirWithPrefix("build_")
PRINT "Build dir: "; d2
Zanna.IO.Dir.RemoveAll(d2)
```

---

## Zanna.IO.Dir

Cross-platform directory operations for creating, removing, listing, and navigating directories.

**Type:** Static utility class

### Methods

| Method             | Signature              | Description                                                                               |
|--------------------|------------------------|-------------------------------------------------------------------------------------------|
| `Exists(path)`     | `Boolean(String)`      | Returns true if the directory exists                                                      |
| `Make(path)`       | `Void(String)`         | Creates a single directory (parent must exist)                                            |
| `MakeAll(path)`    | `Void(String)`         | Creates a directory and all parent directories                                            |
| `Remove(path)`     | `Void(String)`         | Removes an empty directory                                                                |
| `RemoveAll(path)`  | `Void(String)`         | Recursively removes a directory and all its contents without following symlinks into targets |
| `Entries(path)`    | `Seq(String)`          | Returns directory entries (files + subdirectories); traps if the directory does not exist |
| `List(path)`       | `Seq(String)`          | Returns all entries in a directory (excluding `.` and `..`)                               |
| `Page(path, offset, limit)` | `Map(String, Integer, Integer)` | Returns one bounded page of immediate entries with kind metadata              |
| `Files(path)`      | `Seq(String)`          | Returns only files in a directory (no subdirectories)                                     |
| `Dirs(path)`       | `Seq(String)`          | Returns only subdirectories in a directory                                                |
| `Current()`        | `String()`             | Returns the current working directory                                                     |
| `SetCurrent(path)` | `Void(String)`         | Changes the current working directory                                                     |
| `Move(src, dst)`   | `Void(String, String)` | Moves/renames a directory                                                                 |

**Note:** `Entries()`, `List()`, `Files()`, and `Dirs()` return entry names (not full paths). Use
`Zanna.IO.Path.Join(dir, name)` to build full paths when needed.

`Page()` is the bounded alternative for frame-driven tools. Its result map contains
`valid`, `path`, `entries`, `offset`, `limit`, `emitted`, `nextOffset`, `done`, and
`diagnostics`. Each entry map contains `name`, `path`, `kind` (`directory`, `file`, or
`other`), and `isDirectory`. Pass `nextOffset` to the next call until `done` is true;
pages preserve filesystem enumeration order and are not sorted.

`Make()` and `MakeAll()` are idempotent for existing directories, but they trap if the target path or any intermediate path component already exists as a non-directory. `MakeAll()` follows host path semantics: `/` is the separator on POSIX, while both `/` and `\` are accepted on Windows.
`Files()` excludes symbolic links to files on POSIX, while `Dirs()` can include a symlink whose target is a directory. `RemoveAll()` removes a top-level symlink itself and does not recurse into the linked directory. On POSIX, recursive removal uses file-descriptor-relative traversal so a concurrently swapped symlink component cannot redirect deletion outside the requested tree.

### Zia Example

```zia
module DirDemo;

bind Zanna.Terminal;
bind Zanna.IO.Dir as Dir;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Current working directory
    Say("CWD: " + Dir.Current());

    // Check if directory exists
    Say("Exists /tmp: " + Fmt.Bool(Dir.Exists("/tmp")));

    // Create nested directories (like mkdir -p)
    Dir.MakeAll("/tmp/zanna_demo/sub/deep");
    Say("Created: " + Fmt.Bool(Dir.Exists("/tmp/zanna_demo/sub/deep")));

    // Clean up
    Dir.RemoveAll("/tmp/zanna_demo");
    Say("Removed: " + Fmt.Bool(!Dir.Exists("/tmp/zanna_demo")));
}
```

### BASIC Example

```basic
' Check if a directory exists
IF Zanna.IO.Dir.Exists("/home/user/documents") THEN
    PRINT "Documents folder exists"
END IF

' Create a new directory
Zanna.IO.Dir.Make("/home/user/newdir")

' Create nested directories (like mkdir -p)
Zanna.IO.Dir.MakeAll("/home/user/a/b/c/d")

' List all entries in a directory
DIM entries AS Zanna.Collections.Seq
entries = Zanna.IO.Dir.List("/home/user")
FOR i = 0 TO entries.Count - 1
    PRINT entries.Get(i)
NEXT i

' List directory entries (files + subdirectories); traps if the directory is missing
DIM all_entries AS Zanna.Collections.Seq
all_entries = Zanna.IO.Dir.Entries("/home/user")

' Consume one bounded immediate-directory page
DIM page AS Zanna.Collections.Map
page = Zanna.IO.Dir.Page("/home/user", 0, 128)

' List only files (no subdirectories)
DIM files AS Zanna.Collections.Seq
files = Zanna.IO.Dir.Files("/home/user")

' List only subdirectories
DIM subdirs AS Zanna.Collections.Seq
subdirs = Zanna.IO.Dir.Dirs("/home/user")

' Get and change current working directory
DIM cwd AS STRING
cwd = Zanna.IO.Dir.Current()
PRINT "Current directory: "; cwd

Zanna.IO.Dir.SetCurrent("/home/user/projects")
PRINT "New directory: "; Zanna.IO.Dir.Current()

' Restore original directory
Zanna.IO.Dir.SetCurrent(cwd)

' Move/rename a directory
Zanna.IO.Dir.Move("/home/user/oldname", "/home/user/newname")

' Remove an empty directory
Zanna.IO.Dir.Remove("/home/user/emptydir")

' Recursively remove a directory and all its contents
' WARNING: This permanently deletes files!
Zanna.IO.Dir.RemoveAll("/home/user/tempdir")
```

### Error Handling

Directory operations trap on errors:

- `Make()` traps if the parent directory doesn't exist or creation fails
- `Remove()` traps if the directory is not empty or doesn't exist
- `RemoveAll()` ignores an already-missing top-level directory, but traps if any existing child cannot be removed
- `RemoveAll()` refuses protected targets such as the filesystem root, `.`, `..`, the current
  working directory, and every ancestor of the current working directory
- `Move()` traps if the source directory is missing or the destination already exists
- `SetCurrent()` traps if the directory doesn't exist

Use `Exists()` to check before performing operations that may fail.

### Listing Functions

The three listing functions return `Seq` objects containing entry names (not full paths):

| Function      | Returns          | Includes                      |
|---------------|------------------|-------------------------------|
| `List(path)`  | All entries      | Files and subdirectories      |
| `Files(path)` | Files only       | Regular files, no directories |
| `Dirs(path)`  | Directories only | Subdirectories, no files      |

Use the `*Seq` forms when a frontend or toolchain stage wants the explicit suffix.

```basic
DIM names AS Zanna.Collections.Seq
names = Zanna.IO.Dir.List("/home/user")
PRINT names.Count
```

All listing functions exclude `.` and `..` entries. If the directory doesn't exist or can't be read, an empty sequence
is returned. `Entries()` is the strict variant: it traps on open, read, or close errors.

### Use Cases

- **File management:** List, copy, move, and delete directories
- **Build systems:** Create output directories with `MakeAll()`
- **Cleanup:** Remove temporary directories with `RemoveAll()`
- **Navigation:** Get and set the working directory
- **Filtering:** Separate files from subdirectories with `Files()` and `Dirs()`

---

## Zanna.IO.Path

Cross-platform path manipulation utilities. On Windows, both `/` and `\` are treated as separators. On POSIX, `/` is the only path separator and backslash is an ordinary filename byte in every path helper, including `Path.Join`.

**Type:** Static utility class

### Methods

| Method               | Signature                | Description                                                 |
|----------------------|--------------------------|-------------------------------------------------------------|
| `Join(a, b)`         | `String(String, String)` | Joins two path components with the platform separator       |
| `Directory(path)`    | `String(String)`         | Returns the directory portion of a path                     |
| `Name(path)`         | `String(String)`         | Returns the filename portion of a path                      |
| `Stem(path)`         | `String(String)`         | Returns the filename without extension                      |
| `Extension(path)`    | `String(String)`         | Returns the file extension (including the dot)              |
| `WithExtension(path, ext)` | `String(String, String)` | Replaces the extension of a path                      |
| `IsAbsolute(path)`   | `Boolean(String)`        | Returns true if the path is absolute                        |
| `IsLink(path)`       | `Boolean(String)`        | Returns true if the final component is a symlink/reparse point |
| `SameEntry(left, right)` | `Boolean(String, String)` | Compares stable identity for existing files or directories |
| `Absolute(path)`     | `String(String)`         | Converts a relative path to absolute                        |
| `ExeDir()`           | `String()`               | Returns the directory containing the running executable     |
| `DataDir(app)`       | `String(String)`         | Per-user writable data directory for `app` (created on demand) |
| `Normalize(path)`    | `String(String)`         | Normalizes a path (removes `.`, `..`, duplicate separators) |
| `Separator()`        | `String()`               | Returns the platform-specific path separator                |

`DataDir(app)` resolves the OS-conventional per-user location and creates the
directory (including parents) if needed, so settings and save files can be
written immediately:

- **Windows:** `%APPDATA%\<app>`
- **macOS:** `~/Library/Application Support/<app>`
- **Linux:** `$XDG_DATA_HOME/<app>` when `XDG_DATA_HOME` is absolute, else `~/.local/share/<app>`

The app name must be alphanumeric/dash/underscore (max 64 chars); anything
else traps. Validation covers the runtime String's full stored byte length, so an embedded NUL
(and any bytes after it) is rejected rather than silently truncated — distinct Strings cannot
alias one directory. With an unchanged environment, repeated calls return the same absolute path.
`Abs()` and `Norm()` are lexical operations: they do not require the path to
exist and do not resolve symbolic links.

`IsLink()` inspects the final component without following it. It recognizes POSIX symbolic
links and Windows reparse points (including directory junctions), and returns false rather than
trapping for invalid, missing, inaccessible, or ordinary paths.

`SameEntry()` follows normal filesystem resolution and compares stable entry
identity, including directories and link/case aliases supported by the mounted
filesystem. It returns false for invalid, missing, or inaccessible operands.
Use `File.SameFile()` when both operands must specifically be regular files.

### Zia Example

```zia
module PathDemo;

bind Zanna.Terminal;
bind Zanna.IO.Path as Path;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var p = "/home/user/documents/report.txt";

    // Extract path components
    Say("Dir:  " + Path.Directory(p));    // /home/user/documents
    Say("Name: " + Path.Name(p));   // report.txt
    Say("Stem: " + Path.Stem(p));   // report
    Say("Ext:  " + Path.Extension(p));    // .txt

    // Join paths
    Say("Join: " + Path.Join("/home/user", "downloads"));

    // Replace extension
    Say("WithExt: " + Path.WithExtension(p, ".md"));

    // Check if absolute
    Say("IsAbs: " + Fmt.Bool(Path.IsAbsolute(p)));
    Say("IsAbs relative: " + Fmt.Bool(Path.IsAbsolute("foo/bar")));

    // Normalize paths
    Say("Norm: " + Path.Normalize("/foo//bar/../baz"));

    // Platform separator
    Say("Sep: " + Path.Separator());
}
```

### BASIC Example

```basic
DIM path AS STRING
path = "/home/user/documents/report.txt"

' Extract path components
PRINT Zanna.IO.Path.Directory(path)   ' Output: "/home/user/documents"
PRINT Zanna.IO.Path.Name(path)  ' Output: "report.txt"
PRINT Zanna.IO.Path.Stem(path)  ' Output: "report"
PRINT Zanna.IO.Path.Extension(path)   ' Output: ".txt"

' Join paths
DIM newPath AS STRING
newPath = Zanna.IO.Path.Join("/home/user", "downloads")
PRINT newPath  ' Output: "/home/user/downloads"

' Replace extension
DIM mdPath AS STRING
mdPath = Zanna.IO.Path.WithExtension(path, ".md")
PRINT mdPath  ' Output: "/home/user/documents/report.md"

' Check if absolute
PRINT Zanna.IO.Path.IsAbsolute(path)      ' Output: true
PRINT Zanna.IO.Path.IsAbsolute("foo/bar") ' Output: false

' Normalize paths
PRINT Zanna.IO.Path.Normalize("/foo//bar/../baz")  ' Output: "/foo/baz"
PRINT Zanna.IO.Path.Normalize("./a/b/../c")        ' Output: "a/c"

' Get platform separator
PRINT Zanna.IO.Path.Separator()  ' Output: "/" on Unix, "\" on Windows
```

### Path Normalization

The `Norm()` function performs the following transformations:

- Removes redundant separators (`//` becomes `/`)
- Resolves `.` components (current directory)
- Resolves `..` components (parent directory) where possible
- Returns `.` for an empty result
- Preserves leading `..` in relative paths

### Platform Differences

| Behavior                | Unix            | Windows                   |
|-------------------------|-----------------|---------------------------|
| Path separator          | `/`             | `\`                       |
| Absolute path detection | Starts with `/` | Starts with `C:\`, `\\`, or root-relative `\` |
| Example absolute path   | `/home/user`    | `C:\Users\user`           |

Windows drive-relative paths such as `C:logs\app.txt` are not absolute. `Path.Normalize()` preserves the `C:` relative prefix instead of converting it to `C:\`, and `Path.Join()` treats drive-rooted, UNC, and root-relative right-hand paths as absolute.
`Path.Absolute()` resolves through the OS (`GetFullPathNameW`) on Windows, so a drive-relative
path like `C:logs` is anchored to drive C's current directory exactly like the shell, rather than
being joined under another drive's current directory.
`Path.ExeDir()` resolves the executable path with dynamically sized platform APIs and detects
truncation, so long or non-ASCII executable paths return the real directory; `"."` is returned
only on a genuine lookup failure.
On POSIX, `Path.Name("a\\b.txt")` returns `"a\\b.txt"` and `Path.Directory("a\\b.txt")` returns `"."`, because the backslash is part of the filename rather than a directory separator.
`Path.WithExtension("", "txt")` returns `.txt`, matching the behavior of applying an extension to an empty stem.

### Use Cases

- **Building file paths:** Use `Join()` to create paths safely
- **Extracting components:** Use `Dir()`, `Name()`, `Stem()`, `Ext()` to parse paths
- **Changing extensions:** Use `WithExt()` to replace file extensions
- **Cleaning paths:** Use `Norm()` to clean up user-provided paths
- **Portable code:** Use `Sep()` for platform-specific separators

---

## Zanna.IO.Glob

File globbing utilities for matching file paths against wildcard patterns and finding files by pattern.

**Type:** Static utility class

### Methods

| Method                        | Signature                  | Description                                                     |
|-------------------------------|----------------------------|-----------------------------------------------------------------|
| `Match(path, pattern)`        | `Boolean(String, String)`  | Returns true if the path matches the glob pattern               |
| `Files(dir, pattern)`         | `Seq(String, String)`      | Returns files in a directory matching the pattern               |
| `FilesRecursive(dir, pattern)` | `Seq(String, String)`     | Returns files in a directory and subdirectories matching pattern |
| `Entries(dir, pattern)`       | `Seq(String, String)`      | Returns all entries (files + dirs) matching the pattern          |

### Pattern Syntax

| Pattern | Description                        | Example                |
|---------|------------------------------------|------------------------|
| `*`     | Matches any sequence of characters | `*.txt` matches `a.txt` |
| `?`     | Matches a single character         | `?.c` matches `a.c`    |
| `[abc]` | Matches any character in brackets  | `[abc].txt`            |
| `[a-z]` | Matches a byte in a range           | `[0-9].txt`            |
| `[!abc]`, `[^abc]` | Matches a byte outside the class | `[!0-9]*`       |
| `**`    | Matches across path separators      | `src/**/*.zia`         |

### Notes

- **Parameter order is (path, pattern)** for `Match`, not (pattern, path)
- Matching is byte-oriented and case-sensitive on POSIX; Windows comparisons are ASCII case-insensitive.
- `*`, `?`, and character classes do not cross a path separator. `**` does; a backslash can escape a character inside a class.
- Null or embedded-NUL path/pattern inputs return false for `Match` and an empty sequence for
  listing helpers
- Listing helpers return an empty sequence for paths or patterns containing embedded NUL bytes.
- `Files` returns only regular files, not directories
- `FilesRecursive` descends into all subdirectories and traps if recursion exceeds the runtime depth guard.
- `Entries` returns both files and directories that match the pattern
- All listing methods return a `Seq` of full path strings
- Patterns are matched against the filename component for `Files` / `Entries`; `FilesRecursive` matches each file's path relative to the supplied root.
- On Windows, both `/` and `\` are treated as path separators for `*`, `?`, `**`, and literal separator matching.
- On POSIX, only `/` is a path separator for glob matching; backslash is matched as a normal character.
- `**` matching is memoized, so very deep path strings are handled without a fixed recursion attempt limit.
- `**` crosses separators itself; `*`, `?`, and character classes after an earlier `**` follow
  ordinary component rules and do not cross separators. For example, `Match("a/b", "**/a?b")`
  returns false (the trailing `?` cannot match the `/`), while `Match("a/b/xyz", "**/x?z")`
  returns true.
- Recursive traversal does not follow symbolic-link directories or Windows reparse points. Result order follows filesystem enumeration and is not sorted.

### Zia Example

```zia
module GlobDemo;

bind Zanna.Terminal;
bind Zanna.IO.Glob as Glob;
bind Zanna.Text.Fmt as Fmt;

func start() {
    Say("Match txt: " + Fmt.Bool(Glob.Match("hello.txt", "*.txt")));   // true
    Say("Match c: " + Fmt.Bool(Glob.Match("hello.c", "*.txt")));       // false
}
```

### BASIC Example

```basic
' Match file paths against patterns
PRINT Zanna.IO.Glob.Match("hello.txt", "*.txt")    ' Output: 1
PRINT Zanna.IO.Glob.Match("hello.c", "*.txt")      ' Output: 0
PRINT Zanna.IO.Glob.Match("test.bas", "*.bas")      ' Output: 1
PRINT Zanna.IO.Glob.Match("readme.md", "read*")     ' Output: 1

' Find all .txt files in a directory
DIM txtFiles AS OBJECT = Zanna.IO.Glob.Files("/home/user/docs", "*.txt")
FOR i = 0 TO txtFiles.Count - 1
    PRINT txtFiles.Get(i)
NEXT i

' Find all .bas files recursively
DIM basFiles AS OBJECT = Zanna.IO.Glob.FilesRecursive("/home/user/projects", "*.bas")
PRINT "Found "; basFiles.Count; " BASIC files"

' Find all entries (files + dirs) matching a pattern
DIM entries AS OBJECT = Zanna.IO.Glob.Entries("/home/user", "test*")
FOR i = 0 TO entries.Count - 1
    PRINT entries.Get(i)
NEXT i
```

---

## Zanna.Workspace.FileIndex

Workspace file inventory helper for IDEs and editor tools.

**Type:** Static utility class

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Enumerate(root, extensionsCsv, excludesCsv, includeDirs)` | `Seq(String, String, String, Boolean)` | Recursively enumerate workspace entries under `root` |
| `Page(root, extensionsCsv, excludesCsv, includeDirs, offset, limit)` | `Map(String, String, String, Boolean, Integer, Integer)` | Return one bounded page of workspace entries |
| `Status(root, extensionsCsv, excludesCsv, includeDirs)` | `Map(String, String, String, Boolean)` | Return traversal status without materializing every entry |
| `ShouldIgnore(root, relativePath, patternsCsv)` | `Boolean(String, String, String)` | Return whether a relative path is ignored by hard excludes, `.gitignore`, or explicit patterns |

`Enumerate` returns a `Seq` of `Map` records. Each record includes `path`, `relativePath`, `name`, `extension`, `kind`, `isDirectory`, `id`, `size`, and `modified`.

`Page` returns a `Map` with `valid`, `root`, `entries`, `offset`, `limit`, `emitted`, `nextOffset`, `scanned`, `done`, `truncated`, `maxEntries`, and `diagnostics`. Each entry has the same shape as `Enumerate`. Use `nextOffset` until `done` is true to process large workspaces without allocating every row at once. Non-positive limits default to 512 and larger limits are clamped to 4096.

`Status` returns a `Map` with `valid`, `root`, `entryCount`, `maxEntries`, `truncated`, `fingerprint`, and `diagnostics`. It uses the same filters and ignore rules as `Enumerate`, but it is intended for IDE progress/status surfaces and large-workspace guardrails where allocating every entry would be unnecessary. `fingerprint` hashes the filtered relative paths, kinds, sizes, and modification times for quick change detection.

### Notes

- Hard excludes include every dot-prefixed entry, plus `.git`, `.hg`, `.svn`, `.zanna`, `.zanna-cache`, `build`, `cmake-build-*`, `node_modules`, and `.DS_Store`.
- Root and nested `.gitignore` files are honored using an intentional subset: blank/comment lines, directory suffixes, `*`, `?`, `**`, and `!` negation are supported.
- `extensionsCsv` may contain values such as `.zia,.json,.png`; an empty list includes all regular files.
- Extension filtering lowercases both the filter and candidate extension on every platform, so it
  is ASCII case-insensitive even on case-sensitive filesystems.
- The `id` field is a stable 63-bit hash of the absolute, lexically normalized path for quick UI identity, not a persistent filesystem inode.
- Enumeration is capped at 100,000 matching entries. `Enumerate` stops at the cap; `Page` and `Status` expose `maxEntries` / `truncated` so callers can report it.
- The `**/` ignore matcher spans whole path components: it matches zero or more complete
  directory components, so `**/bar` ignores a component named exactly `bar` at any depth (`bar`,
  `a/b/bar`) but not a component that merely ends with it (`foobar` is kept).
- `.gitignore` cache invalidation is content-derived: the cache identity is a hash of the file's
  bytes, so any edit (including a same-second or coarse-clock rewrite that leaves the modification
  timestamp unchanged) takes effect on the next lookup.

---

## Zanna.Workspace.WorkspaceWatcher

Batch wrapper over `Zanna.IO.Watcher` for per-frame IDE polling.

**Type:** Static utility class

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `PollBatch(watcher, maxEvents)` | `Seq(Object, Integer)` | Drain up to `maxEvents` queued watcher events into structured maps |

Each event map includes `path`, `typeName`, `type`, `overflowCount`, and `requiresRescan`. `requiresRescan` is true for overflow events so an IDE can discard incremental state and re-enumerate the workspace. `overflowCount` is the exact dropped-event count for an internal ring overflow or `-1` when a native (kernel) overflow lost an unknown number of events — either way `requiresRescan` is the reliable signal. A non-positive `maxEvents` drains at most 64 events.

`Zanna.IO.Watcher` remains non-recursive; use one watcher per watched directory or pair this helper with a workspace index rescan policy.

---

## Zanna.Project.Manifest

Tooling-oriented subset parser for `zanna.project` and editor-expanded project manifests. It does not replace the build command's full project loader.

**Type:** Static utility class

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `ParseText(text)` | `Map(String)` | Parse manifest text into a structured map |
| `ParseFile(path)` | `Map(String)` | Read and parse a manifest file |

The returned map includes `valid`, `name`, `version`, `language`, `entry`, `defaultScene`, `sourceGlobs`, `excludes`, `assetRoots`, `sceneRoots`, `runConfigs`, `buildConfigs`, and `diagnostics`.

### Supported Directives

```text
project Demo
lang zia
entry src/main.zia
sources src
exclude build
asset-root assets
scene-root scenes
default-scene scenes/level.json

[run.play]
entry src/main.zia
args --dev, --scene=one
```

Unknown directives or sections add diagnostic records instead of trapping, and defaults are
retained. Build-only directives outside this tooling subset, such as `embed`, `pack`, and
`pack-compressed`, therefore appear as diagnostics rather than being expanded.
Directives inside an unknown `[section]` are each diagnosed and ignored until the next section
header — they do not fall through to mutate top-level fields — so a `valid=false` manifest never
carries values injected by an unsupported section.

---

## Zanna.Workspace.Edit

Transactional multi-file text edit validation and application.

**Type:** Static utility class

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Validate(edits)` | `Map(Seq)` | Check edit records without writing files |
| `Apply(edits)` | `Map(Seq)` | Revalidate, stage, and apply edits |
| `ValidateInRoot(edits, root)` | `Map(Seq, String)` | Validate while rejecting targets outside an existing workspace root |
| `ApplyInRoot(edits, root)` | `Map(Seq, String)` | Apply with the same workspace-root boundary |

Each edit record is a `Map` with `file`, `startLine`, `startColumn`, `endLine`, `endColumn`, `newText`, and optional `expectedMtime` / `expectedSize`. Line and column values are 1-based byte positions. Validation results contain `success`, `editCount`, and `diagnostics`; apply results add `appliedFiles`.

Validation rejects missing files, invalid ranges, overlapping edits in the same file, and stale expected metadata. Apply revalidates immediately before writing, stages every changed file beside its target, and commits each replacement with same-directory renames. If a later commit fails, rollback is best-effort; callers must still inspect `success` and diagnostics rather than treating a multi-file batch as a crash-proof filesystem transaction.
Immediately before replacing each live target the commit phase re-reads the file and confirms it
still matches the content validated earlier; if an external editor, formatter, or process changed
it during staging, the whole batch is aborted with an `edit target changed since validation`
diagnostic and rolled back, so newer content is not silently overwritten. (The recheck is
content-based; a rooted transaction whose path *component* is swapped for another during the
window is not yet fully guarded — full protection needs handle-relative commits.) Temp and backup sidecar names are unpredictable (a per-process keyed hash of an atomic counter),
and each backup is exclusively reserved (`O_EXCL` / `CREATE_NEW`) before the commit rename, so a
stale artifact or racing process cannot make the rename clobber unrelated data; a successful apply
leaves no sidecars behind.

---


## See Also

- [Streams & Buffers](streams.md)
- [Advanced IO](advanced.md)
- [Input & Output Overview](README.md)
- [Zanna Runtime Library](../README.md)
