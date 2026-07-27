---
status: active
audience: public
last-verified: 2026-07-17
---

# Advanced IO
> Archive (ZIP), Compress (DEFLATE/GZIP), Watcher (filesystem events)

**Part of [Zanna Runtime Library](../README.md) › [Input & Output](README.md)**

---

## Zanna.IO.Archive

ZIP archive reader and writer for creating, reading, and extracting ZIP files.

**Type:** Instance class

**Constructors:**

- `Zanna.IO.Archive.Open(path)` - Opens an existing ZIP archive for reading
- `Zanna.IO.Archive.Create(path)` - Stages a new ZIP archive for writing; the destination is replaced on `Finish()`
- `Zanna.IO.Archive.FromBytes(data)` - Opens a ZIP archive from in-memory Bytes object

### Properties

| Property | Type   | Description                                        |
|----------|--------|----------------------------------------------------|
| `Path`   | String | Path to the archive file, or empty for `FromBytes` |
| `Count`  | Integer| Number of entries in the archive (read-only)       |
| `Names`  | Object | Runtime `Seq` of entry names (read-only)           |

### Reading Methods

| Method                  | Returns | Description                                               |
|-------------------------|---------|-----------------------------------------------------------|
| `Has(name)`             | Boolean | Returns true if the archive contains an entry with name   |
| `Read(name)`            | Bytes   | Reads entry content as binary data                        |
| `ReadStr(name)`         | String  | Copies entry bytes into a runtime String; does not validate UTF-8 |
| `Extract(name, path)`   | void    | Extracts a single entry to the specified path             |
| `ExtractAll(dir)`       | void    | Extracts all entries to the specified directory           |
| `Info(name)`            | Map     | Returns metadata for an entry, including size, compression, and directory flags |

### Writing Methods

| Method                    | Returns | Description                                           |
|---------------------------|---------|-------------------------------------------------------|
| `Add(name, data)`         | void    | Adds binary data as an entry (with compression)       |
| `AddStr(name, text)`      | void    | Adds a string as an entry (with compression)          |
| `AddFile(name, path)`     | void    | Adds a file from disk as an entry                     |
| `AddDir(name)`            | void    | Adds a directory entry (name should end with `/`)     |
| `Finish()`                | void    | Finalizes the archive (required after writing)        |

### Static Methods

| Method           | Returns | Description                                      |
|------------------|---------|--------------------------------------------------|
| `IsZip(path)`    | Boolean | Returns true if the file appears to be a ZIP    |
| `IsZipBytes(data)`| Boolean| Returns true if the bytes appear to be ZIP data |

### Compression

The archive uses DEFLATE compression (method 8) by default for added entries. Small entries or entries that don't compress well use stored mode (method 0). The implementation reads archives with any combination of stored and deflate-compressed entries. Deflate reads are bounded by each entry's declared uncompressed size, so oversized output traps without relying on the global compression safety cap.
Directory entries can be queried and read with a trailing slash, returning an empty `Bytes` object for `Read("dir/")`.

### Disk Write Semantics

`Create(path)` checks that the destination can be written, but it does not truncate or replace an existing file until `Finish()`. `Finish()` and `Extract()` write through an exclusive temporary file in the destination directory and then atomically replace the final path. `ExtractAll()` applies that replacement separately to each regular file; it is not a transaction across the archive, so a later failure leaves directories and files extracted earlier in the call. Failed individual writes trap and remove their temporary file.
Replacing an existing regular destination preserves POSIX permission bits. On Windows, replacement
uses the native metadata-preserving file-replacement operation so the destination's identity
metadata, ACLs, compression/encryption state, and existing named streams are retained. Windows
directory and reparse-point leaf destinations are rejected.
Archive path arguments reject embedded NUL bytes before calling platform file APIs.

`Open()` reads the complete archive file into memory. `FromBytes()` makes its own complete copy,
and a writer buffers the complete output until `Finish()`. Entry reads and extraction also
materialize each selected entry before returning or writing it.

### Resource Ceilings

Archive construction samples three optional positive-decimal environment settings. Invalid or
zero settings use the defaults; values above the audited hard ceiling are clamped. The sampled
limits remain fixed for that Archive object and apply symmetrically to parsing and writing.

| Setting | Default | Hard ceiling | Scope |
|---------|---------|--------------|-------|
| `ZANNA_ARCHIVE_MAX_FILE_BYTES` | 512 MiB | 2 GiB | Complete encoded ZIP image |
| `ZANNA_ARCHIVE_MAX_ENTRY_BYTES` | 256 MiB | 1 GiB | One entry's declared/uncompressed bytes |
| `ZANNA_ARCHIVE_MAX_TOTAL_ENTRY_BYTES` | 1 GiB | 4 GiB | Sum of all uncompressed entry sizes |

`Open()` checks the encoded file size before allocating its complete buffer. `FromBytes()` checks
before copying. The parser checks each entry and the running aggregate before copying entry names,
and reads recheck the selected entry before allocation or inflation. Writers reject an oversized
source before compression and bound every write-buffer growth, including the central directory.
`AddFile()` checks the source file size before creating a Bytes copy.

### Concurrency and Snapshots

Read-mode archives are immutable after their validated parse. A write-mode Archive owns a native
reader-writer lock: `Add`, `AddDir`, and `Finish` are exclusive transactions, while `Count` and
`Names` take shared snapshots. Concurrent calls therefore never observe a partially appended entry
or central directory. Each caller must still hold an owning reference to the Archive for the full
call. `Names` copies every name into an independently owned runtime String, so the returned Seq
remains valid after the Archive itself is released.

### Entry Name Rules

- Names must be relative (no leading `/` or drive letters)
- `..` path segments are rejected
- `.` segments are ignored
- Backslashes are normalized to `/`
- Embedded NUL bytes in archive entry names are rejected. Malformed ZIP central directories with NUL-containing names also fail to open.
- Duplicate entry names are rejected when writing, and malformed ZIP central directories with duplicate names fail to open.
- Every central-directory entry must agree with its referenced local header on name, flags,
  version, compression method, CRC, sizes, and supported extra fields. Local record ranges must be
  disjoint and end before the central directory.

Invalid names trap with `Archive: invalid entry name`.

### Extraction Safety

`ExtractAll(dir)` creates missing directories under `dir`, but it refuses to extract through existing symlinked or reparse-point directory components inside the destination tree. This prevents an archive entry such as `assets/config.json` from writing outside `dir` when `dir/assets` is already a symlink. The destination root itself must not be a symlink.
On POSIX, extraction resolves and writes entries through directory file descriptors and uses descriptor-relative temp-file replacement for regular files. This prevents a destination component from being swapped to a symlink between validation and the final write.

### Info Map Keys

| Key              | Type    | Description                           |
|------------------|---------|---------------------------------------|
| `size`           | Integer | Uncompressed size in bytes            |
| `compressedSize` | Integer | Compressed size in bytes              |
| `crc`            | Integer | CRC32 checksum                        |
| `method`         | Integer | Compression method (0=stored, 8=deflate) |
| `modifiedTime`   | Integer | Entry modification time as a Unix timestamp |
| `isDir`          | Boolean | True if entry is a directory          |
| `isDirectory`    | Boolean | Back-compat alias for `isDir`         |

ZIP timestamps have two-second precision. When writing, Zanna defaults every
entry to the reproducible timestamp `2001-01-01T00:00:00Z`; a valid
`SOURCE_DATE_EPOCH` changes that value, and `ZANNA_ARCHIVE_TIMESTAMP=now`
selects the current UTC time instead. `modifiedTime` interprets the stored DOS
calendar fields as UTC seconds since the Unix epoch.

### Zia Example

```zia
module ArchiveDemo;

bind Zanna.Terminal;
bind Zanna.IO.Archive as Arc;
bind Zanna.IO.File as File;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Create a new archive
    var arc = Arc.Create("/tmp/backup.zip");
    arc.AddStr("hello.txt", "Hello from Zia!");
    arc.AddStr("data.txt", "Some data content");
    arc.AddDir("subdir/");
    arc.Finish();
    Say("Archive created");

    // Fast signature probe; Open performs full validation
    Say("IsZip: " + Fmt.Bool(Arc.IsZip("/tmp/backup.zip")));

    // Open and read
    var reader = Arc.Open("/tmp/backup.zip");
    Say("Count: " + Fmt.Int(reader.get_Count()));
    Say("Has hello.txt: " + Fmt.Bool(reader.Has("hello.txt")));
    Say("Content: " + reader.ReadStr("hello.txt"));

    // Clean up
    File.Delete("/tmp/backup.zip");
}
```

### BASIC Example

```basic
' Create a new archive
DIM arc AS OBJECT = Zanna.IO.Archive.Create("backup.zip")

' Add files with different methods
arc.AddStr("readme.txt", "This is a readme file.")
arc.Add("data.bin", Zanna.IO.File.ReadAllBytes("data.bin"))
arc.AddFile("config.json", "config.json")
arc.AddDir("logs/")

' Finalize the archive
arc.Finish()

' Read an existing archive
arc = Zanna.IO.Archive.Open("backup.zip")

PRINT "Entries:"; arc.Count

' List all entries
DIM names AS OBJECT = arc.Names
FOR i = 0 TO Zanna.Collections.Seq.get_Count(names) - 1
    PRINT Zanna.Collections.Seq.GetStr(names, i)
NEXT i

' Read specific entry
IF arc.Has("readme.txt") THEN
    DIM content AS STRING = arc.ReadStr("readme.txt")
    PRINT content
END IF

' Get entry information
DIM info AS OBJECT = arc.Info("data.bin")
PRINT "Size:"; Zanna.Collections.Map.GetInt(info, "size")
PRINT "Compressed:"; Zanna.Collections.Map.GetInt(info, "compressedSize")
```

### Extraction Example

```basic
' Extract a single file
DIM arc AS OBJECT = Zanna.IO.Archive.Open("archive.zip")

' Extract one entry to a specific path
arc.Extract("docs/manual.pdf", "/home/user/manual.pdf")

' Extract all entries to a directory
arc.ExtractAll("/home/user/extracted")
```

### In-Memory Archive Example

```basic
' Work with ZIP data already loaded in memory
DIM zipData AS Zanna.Collections.Bytes = Zanna.IO.File.ReadAllBytes("download.zip")

' Check for a ZIP signature before the full FromBytes validation
IF Zanna.IO.Archive.IsZipBytes(zipData) THEN
    DIM arc AS OBJECT = Zanna.IO.Archive.FromBytes(zipData)

    ' Process entries
    DIM names AS OBJECT = arc.Names
    FOR i = 0 TO Zanna.Collections.Seq.get_Count(names) - 1
        DIM name AS STRING = Zanna.Collections.Seq.GetStr(names, i)
        DIM content AS Zanna.Collections.Bytes = arc.Read(name)
        PRINT name; ": "; content.Length; " bytes"
    NEXT i
END IF
```

### Signature Probe Example

```basic
' Check only the four-byte ZIP signature before opening
IF Zanna.IO.Archive.IsZip("download.zip") THEN
    DIM arc AS OBJECT = Zanna.IO.Archive.Open("download.zip")
    PRINT "Open validated a ZIP with"; arc.Count; "entries"
ELSE
    PRINT "No recognized ZIP signature"
END IF
```

`IsZip()` and `IsZipBytes()` inspect only the first four bytes. A true result is not structural
validation; `Open()` or `FromBytes()` still traps for a truncated or malformed archive.

### Error Handling

Archive operations trap on errors:

- `Open()` traps if file doesn't exist or isn't a valid ZIP
- `FromBytes()` traps if the buffer is not a valid ZIP archive
- `Read()`/`ReadStr()` trap if the entry doesn't exist or its data is corrupt. `ReadStr()` does not
  reject arbitrary byte sequences as invalid UTF-8.
- `Extract()` traps if entry doesn't exist or destination is unwritable
- `ExtractAll()` traps if an existing destination component under the extraction root is a symlink or reparse point
- `Add()` traps on null data, invalid names, or duplicate names
- `Finish()` must be called before a created archive is valid; until then an existing output path remains unchanged
- A failed `Finish()` removes its temporary sidecar, restores the staged writer buffer, and may be
  retried after the external filesystem problem is corrected
- Invalid or corrupted entries may trap during reading

### ZIP Format Support

- **Supported formats:** ZIP32 (standard ZIP format)
- **Compression methods:** Stored (0), Deflate (8)
- **Features:** Directory entries, file attributes, CRC32 validation
- **Limitations:** ZIP64, encryption, strong encryption, and data-descriptor entries are not supported
- Oversize entry counts or file sizes that require ZIP64 trap instead of producing a truncated archive
- Corrupt central directories, unsupported feature flags, ZIP64 marker fields or extra records,
  mismatched/overlapping local records, compressed data, CRC mismatches, size mismatches, resource
  ceiling violations, and internal buffer overflows trap instead of returning partial data

### Use Cases

- **File distribution:** Create ZIP archives for downloading
- **Data backup:** Archive multiple files into a single compressed file
- **In-memory processing:** Work with ZIP data without disk I/O
- **Extraction:** Unpack downloaded or received ZIP files
- **Integration:** Read/write ZIP files for interoperability with other tools

---

## Zanna.IO.Compress

DEFLATE and GZIP compression/decompression utilities with zero external dependencies.

**Type:** Static utility class

### Methods

| Method                  | Signature              | Description                                               |
|-------------------------|------------------------|-----------------------------------------------------------|
| `Deflate(data)`         | `Bytes(Bytes)`         | Compress bytes using DEFLATE (default level 6)            |
| `DeflateLvl(data, lvl)` | `Bytes(Bytes, Integer)`| Compress bytes using DEFLATE with specified level (1-9)   |
| `Inflate(data)`         | `Bytes(Bytes)`         | Decompress DEFLATE-compressed bytes                       |
| `Gzip(data)`            | `Bytes(Bytes)`         | Compress bytes using GZIP format (default level 6)        |
| `GzipLvl(data, lvl)`    | `Bytes(Bytes, Integer)`| Compress bytes using GZIP with specified level (1-9)      |
| `Gunzip(data)`          | `Bytes(Bytes)`         | Decompress GZIP-compressed bytes, including concatenated members |
| `DeflateStr(text)`      | `Bytes(String)`        | Compress string using DEFLATE                             |
| `InflateStr(data)`      | `String(Bytes)`        | Decompress DEFLATE-compressed bytes to string             |
| `GzipStr(text)`         | `Bytes(String)`        | Compress string using GZIP format                         |
| `GunzipStr(data)`       | `String(Bytes)`        | Decompress GZIP-compressed bytes to string, including concatenated members |

### Compression Levels

| Level | Speed     | Compression | Use Case                     |
|-------|-----------|-------------|------------------------------|
| 1     | Fastest   | Minimal     | Real-time compression        |
| 6     | Balanced  | Good        | General purpose (default)    |
| 9     | Slowest   | Maximum     | Archival, bandwidth-limited  |

### DEFLATE vs GZIP

| Format  | Description                                                        |
|---------|--------------------------------------------------------------------|
| DEFLATE | Raw compressed data stream (RFC 1951)                              |
| GZIP    | DEFLATE with header, CRC32, and size footer (RFC 1952)             |

Use GZIP when:
- Interoperating with `.gz` files or HTTP gzip encoding
- Data integrity verification is needed (CRC32)

Use DEFLATE when:
- Building custom formats with your own framing
- Minimal overhead is required
- Used as part of another format (ZIP, PNG, etc.)

### Zia Example

```zia
module CompressDemo;

bind Zanna.Terminal;
bind Zanna.IO.Compress as Comp;

func start() {
    // Compress a string with DEFLATE
    var compressed = Comp.DeflateStr("Hello, World! This is a test of compression.");
    Say("Compressed");

    // Decompress back to string
    var restored = Comp.InflateStr(compressed);
    Say("Restored: " + restored);

    // GZIP compression (compatible with gzip command-line tool)
    var gzipped = Comp.GzipStr("Gzip compressed data");
    var gunzipped = Comp.GunzipStr(gzipped);
    Say("Gzip roundtrip: " + gunzipped);
}
```

### BASIC Example

```basic
' Compress binary data
DIM original AS OBJECT = Zanna.Collections.Bytes.FromStr("Hello, World!")
DIM compressed AS OBJECT = Zanna.IO.Compress.Deflate(original)
DIM restored AS OBJECT = Zanna.IO.Compress.Inflate(compressed)

PRINT "Original:"; original.Length     ' Output: 13
PRINT "Compressed:"; compressed.Length ' Output: varies
PRINT "Restored:"; restored.Length     ' Output: 13

' Compress with higher compression level
DIM maxCompressed AS OBJECT = Zanna.IO.Compress.DeflateLvl(original, 9)

' GZIP format (compatible with gzip command-line tool)
DIM gzipped AS OBJECT = Zanna.IO.Compress.Gzip(original)
DIM gunzipped AS OBJECT = Zanna.IO.Compress.Gunzip(gzipped)

' Verify GZIP magic bytes
PRINT HEX(gzipped.Get(0)); HEX(gzipped.Get(1))  ' Output: 1F8B
```

### String Convenience Methods

```basic
' Compress a string directly
DIM text AS STRING = "The quick brown fox jumps over the lazy dog."
DIM compressed AS OBJECT = Zanna.IO.Compress.GzipStr(text)

' Decompress back to string
DIM restored AS STRING = Zanna.IO.Compress.GunzipStr(compressed)
PRINT restored  ' Output: The quick brown fox jumps over the lazy dog.

' DEFLATE string variants
DIM deflated AS OBJECT = Zanna.IO.Compress.DeflateStr(text)
DIM inflated AS STRING = Zanna.IO.Compress.InflateStr(deflated)
```

### File Compression Example

```basic
' Compress a file to .gz format
DIM content AS OBJECT = Zanna.IO.File.ReadAllBytes("data.txt")
DIM compressed AS OBJECT = Zanna.IO.Compress.Gzip(content)
Zanna.IO.File.WriteAllBytes("data.txt.gz", compressed)

' Decompress a .gz file
DIM gzData AS OBJECT = Zanna.IO.File.ReadAllBytes("archive.gz")
DIM original AS OBJECT = Zanna.IO.Compress.Gunzip(gzData)
Zanna.IO.File.WriteAllBytes("archive", original)
```

### Error Handling

Compression traps on:
- Null input data
- Invalid compression level (must be 1-9)
- Invalid or truncated compressed data
- Reserved GZIP flags, malformed optional headers, header CRC mismatches, trailer CRC32 mismatches, or trailer size mismatches
- Malformed later members in a concatenated GZIP stream
- Corrupted DEFLATE streams, including truncated Huffman symbols, oversubscribed or missing Huffman codes, malformed dynamic-code repeat runs, and dynamic literal trees without an end-of-block code
- Trailing data after the final raw DEFLATE block
- Inflated output exceeding the runtime safety cap (256 MiB)

### Implementation Notes

- Zero external dependencies (no zlib required)
- Implements RFC 1951 (DEFLATE) and RFC 1952 (GZIP)
- Level 1 emits stored blocks. Levels 2–9 use LZ77 matching plus fixed-Huffman blocks; higher levels search deeper match chains.
- The compressor does not currently emit dynamic-Huffman blocks.
- Decompression supports all DEFLATE block types (stored, fixed Huffman, dynamic Huffman)
- The `*Str` decompressors copy the inflated bytes into a runtime String without validating that
  they form UTF-8. Use the byte-returning methods plus an explicit codec when validation matters.

### Use Cases

- **File compression:** Create and read `.gz` files
- **HTTP compression:** Handle gzip-encoded HTTP responses
- **Data storage:** Reduce storage space for text and binary data
- **Network transfer:** Reduce bandwidth for data transmission
- **Archive formats:** Build custom compressed file formats

---

## Zanna.IO.Watcher

Cross-platform file system watcher for monitoring files and directories for changes.

**Type:** Instance class

**Constructor:** `Zanna.IO.Watcher.New(path)` - Creates a watcher for the specified file or directory

### Event Types

| Constant        | Value | Description                         |
|-----------------|-------|-------------------------------------|
| `EventNone`    | 0     | No event (returned by Poll timeout) |
| `EventCreated` | 1     | File or directory was created       |
| `EventModified`| 2     | File was modified                   |
| `EventDeleted` | 3     | File or directory was deleted       |
| `EventRenamed` | 4     | File or directory was renamed       |
| `EventOverflow`| 5     | Watcher event queue overflowed      |

### Properties

| Property      | Type    | Description                                    |
|---------------|---------|------------------------------------------------|
| `Path`        | String  | The path being watched (read-only)             |
| `IsWatching`  | Boolean | True if actively watching (read-only)          |
| `EventNone`  | Integer | Event constant: No event (static, read-only)   |
| `EventCreated` | Integer | Event constant: Created (static, read-only)  |
| `EventModified`| Integer | Event constant: Modified (static, read-only) |
| `EventDeleted` | Integer | Event constant: Deleted (static, read-only)  |
| `EventRenamed` | Integer | Event constant: Renamed (static, read-only)  |
| `EventOverflow`| Integer | Event constant: Queue overflow (static, read-only) |

### Methods

| Method          | Returns | Description                                              |
|-----------------|---------|----------------------------------------------------------|
| `Start()`       | void    | Begin watching for file system events                    |
| `Stop()`        | void    | Stop watching for events                                 |
| `Poll()`        | Integer | Check for event (non-blocking); returns event type or 0  |
| `PollFor(ms)`   | Integer | Wait using the requested platform timeout; returns event type |
| `EventPath()`   | String  | Get the path for the last event, rooted at the supplied watch path |
| `EventType()`   | Integer | Get the type of the last polled event                    |
| `EventOverflowCount()` | Integer | Exact dropped-event count for an internal ring overflow, `-1` for a native (kernel) overflow of unknown size, or 0 when the last event was not an overflow |

### Platform Implementation

| Platform | Backend API                |
|----------|----------------------------|
| Linux    | inotify                    |
| macOS    | kqueue                     |
| Windows  | ReadDirectoryChangesW      |

`Start()` traps as unsupported on stub platforms.

Each successful `Start()` begins a fresh event epoch. It clears stale queued and last-event state from a previously retired backend. Transient native resource failures (descriptor/handle exhaustion, a vanished path, or watch-registration exhaustion) leave `IsWatching` false on every supported platform so callers can fall back to a full rescan. `Stop()` is idempotent and clears the event epoch even if deletion, rename, unmount, revocation, or a backend error already made the watcher inactive.

The internal ring holds 64 events. If producers outrun polling, the newest slot becomes an overflow marker and `EventOverflowCount()` reports the exact coalesced loss, saturating at the signed 64-bit maximum. A kernel/backend overflow or malformed native batch reports `-1`, because no exact count exists. Treat every overflow as a request to rescan the watched scope.

One Watcher instance has mutable native handles, a shared event queue, and one “last event” slot.
It is bound to the thread that constructed it: every instance method and instance property traps
when called from another thread. Keep `New`, `Start`, polling, event inspection, and `Stop` on one
event-loop thread. In particular, cross-thread `Stop()` is not a cancellation mechanism for a
blocking `PollFor(-1)`. Finalization may release an otherwise unreachable watcher on a collector
thread because no public caller can still race that cleanup.

A native read failure, malformed native event batch, revoked/unmounted watch, or kernel change-
queue loss is surfaced as `EventOverflow` with an unknown loss count before the backend retires or
rearms. Callers therefore receive the same conservative rescan signal for backend errors as for a
kernel overflow instead of seeing only `EventNone`.

### Zia Example

Watcher is available from Zia and BASIC through the same poll-based API.

### BASIC Example

```basic
' Watch a directory for changes
DIM watcher AS OBJECT = Zanna.IO.Watcher.New("/home/user/documents")

' Start watching
watcher.Start()

' Check if we're watching
PRINT "Watching:"; watcher.IsWatching  ' Output: 1

' Poll once with a 1-second timeout; call repeatedly in the application's loop.
DIM event AS INTEGER = watcher.PollFor(1000)
IF event <> watcher.EventNone THEN
    DIM path AS STRING = watcher.EventPath()

    IF event = watcher.EventCreated THEN
        PRINT "Created: "; path
    ELSEIF event = watcher.EventModified THEN
        PRINT "Modified: "; path
    ELSEIF event = watcher.EventDeleted THEN
        PRINT "Deleted: "; path
    ELSEIF event = watcher.EventRenamed THEN
        PRINT "Renamed: "; path
    ELSEIF event = watcher.EventOverflow THEN
        PRINT "Watcher queue overflow; dropped:"; watcher.EventOverflowCount()
    END IF
END IF

' Stop watching
watcher.Stop()
```

### Non-Blocking Example

```basic
' Watch a single file
DIM watcher AS OBJECT = Zanna.IO.Watcher.New("/home/user/config.txt")
watcher.Start()

' Non-blocking poll in a game loop
DO
    ' Do other work...
    ProcessGame()

    ' Quick non-blocking check for file changes
    DIM event AS INTEGER = watcher.Poll()
    IF event = watcher.EventModified THEN
        PRINT "Config file changed, reloading..."
        ReloadConfig()
    END IF
LOOP

watcher.Stop()
```

### Use Cases

- **Hot reload:** Watch config files and reload when changed
- **Build systems:** Trigger rebuilds when source files change
- **File sync:** Monitor directories for new files to process
- **Development tools:** Auto-refresh on file changes
- **Backup software:** Detect changes to back up

### Notes

- Creating a watcher traps if the path does not exist
- All instance operations and properties must run on the thread that called `New()`; cross-thread
  access traps before reading or changing native handles or event state
- The watcher must be started with `Start()` before events can be received
- Construction retains the supplied immutable path and cleans partially derived file-watch paths if allocation traps
- POSIX `PollFor(ms)` retries interrupted waits against one monotonic deadline; repeated signals do not restart the full timeout
- On macOS, deletion, rename, revocation, or kqueue terminal failure retires the vnode watch; recreate and start a new Watcher after rescanning
- `Poll()` returns immediately with `EventNone` if no event is pending
- `PollFor(ms)` uses the specified milliseconds as its platform wait timeout; very large positive
  values are clamped to the largest supported wait value. A positive timeout is honored as a wall-
  clock bound: on Linux an interrupted `poll(2)` resumes with the remaining time against a single
  monotonic deadline, so repeated signals cannot extend the wait beyond the requested duration.
- A negative `PollFor` timeout waits indefinitely; zero is equivalent to non-blocking polling.
- After receiving an event, use `EventPath()` and `EventType()` to get details. Event paths are
  absolute only when the path passed to `New()` was absolute; relative watch paths yield relative
  event paths.
- Multiple events may be queued; call `Poll()` repeatedly to drain them
- The internal queue holds 64 events. If it overflows, a later `Poll()` returns `EventOverflow`;
  treat any overflow as a signal to rescan. `EventOverflowCount()` distinguishes the two overflow
  sources: for an **internal** ring overflow it returns the exact number of dropped events (and
  coalesces upward while the queue stays full); for a **native** kernel overflow — Linux
  `IN_Q_OVERFLOW` or a Windows change-buffer overflow — the OS does not report how many events
  were lost, so it returns `-1` (unknown). Both backends now translate native overflow into an
  `EventOverflow` so the rescan signal is never missed.
- `Stop()` clears queued events and the last-event state. After `Stop()`, `EventType()` returns `EventNone` and `EventPath()` traps until a later successful `Poll()` after `Start()`.
- Directory watches are non-recursive
- On Linux and macOS, a transient descriptor/watch-limit failure during `Start()` can leave `IsWatching` false without a trap; callers that need guaranteed monitoring should check the property and fall back to rescanning.
- On Linux and Windows, single-file watches monitor the parent directory and filter by filename, so deleting and recreating the file at the same path can still produce a later `EventCreated`
- macOS directory watches report the watched directory path for queued events because `kqueue` does not provide child entry names
- Platform-specific behavior may vary slightly for edge cases

---


## Zanna.Data.JsonStream

Pull-based token parser for one complete JSON String. It retains that complete input while callers
consume tokens; “streaming” refers to token-by-token traversal, not incremental input I/O. It
admits at most 255 simultaneously open object/array containers.

**Type:** Instance class

**Constructor:** `Zanna.Data.JsonStream.New(jsonText)`

The live API exposes `HasNext`, `Next`, `NextResult`, `Skip`, `Error`, typed
token-value accessors, and the read-only `Depth` and `TokenType` properties. It
does not generate JSON. See the
[generated API reference](../../generated/runtime/data.md#zanna-data-jsonstream) for the
complete interface.

---


## See Also

- [Files & Directories](files.md)
- [Streams & Buffers](streams.md)
- [Input & Output Overview](README.md)
- [Zanna Runtime Library](../README.md)
