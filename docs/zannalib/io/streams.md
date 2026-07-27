---
status: active
audience: public
last-verified: 2026-07-26
---

# Streams & Buffers
> Stream, MemStream, LineReader, LineWriter, BinaryBuffer

**Part of [Zanna Runtime Library](../README.md) › [Input & Output](README.md)**

---

## Zanna.IO.Stream

Unified stream abstraction providing a common interface over file and memory streams.

**Type:** Instance class

**Constructors:**

- `Zanna.IO.Stream.OpenFile(path, mode)` - Opens a file stream (wraps BinFile)
- `Zanna.IO.Stream.OpenMemory()` - Opens a new in-memory stream (wraps MemStream)
- `Zanna.IO.Stream.OpenBytes(bytes)` - Opens an in-memory stream initialized with data from a Bytes object
- `Zanna.IO.Stream.FromBinFile(binFile)` - Wraps an existing BinFile object
- `Zanna.IO.Stream.FromMemStream(memStream)` - Wraps an existing MemStream object

### Properties

| Property | Type    | Description                                               |
|----------|---------|-----------------------------------------------------------|
| `Type`   | Integer | Stream backing type: 0 = file, 1 = memory (read-only)    |
| `Position` | Integer | Current stream position (read/write)                    |
| `Length`    | Integer | Current data length in bytes (read-only)                  |
| `Eof`    | Boolean | `Position >= Length` on both file and memory backings (position-based) |

### Methods

| Method              | Returns | Description                                      |
|---------------------|---------|--------------------------------------------------|
| `Read(count)`       | Bytes   | Read up to count bytes                           |
| `ReadAll()`         | Bytes   | Read all remaining bytes from current position   |
| `Write(bytes)`      | void    | Write bytes to stream                            |
| `ReadByte()`        | Integer | Read single byte (0-255) or -1 at EOF            |
| `WriteByte(value)`  | void    | Write single byte (0-255)                        |
| `Flush()`           | void    | Flush buffered writes                            |
| `Close()`           | void    | Close the stream and release resources           |
| `AsBinFile()`       | BinFile | Get the underlying BinFile (file streams only)   |
| `AsMemStream()`     | MemStream | Get the underlying MemStream (memory streams only) |
| `ToBytes()`         | Bytes   | Get all data as Bytes (memory streams only)      |

### Open Modes (for OpenFile)

| Mode   | Description                               |
|--------|-------------------------------------------|
| `"r"`  | Read only (file must exist)               |
| `"rb"` | Read only binary alias                    |
| `"w"`  | Write only (creates or truncates)         |
| `"wb"` | Write only binary alias                   |
| `"rw"` | Read and write (file must exist)          |
| `"r+"` | Read and write alias                      |
| `"rb+"`, `"r+b"` | Read and write binary aliases       |
| `"a"`  | Append (creates if needed)                |
| `"ab"` | Append binary alias                       |

### Ownership and Closed Streams

`OpenFile`, `OpenMemory`, and `OpenBytes` create streams that own their backing `BinFile` or `MemStream`; `Close()` releases that backing object. File streams created by `OpenFile` also close the underlying `BinFile`, surfacing delayed flush/close errors instead of leaving the file handle open. `FromBinFile` and `FromMemStream` retain the existing object for the wrapper's lifetime, so the wrapper remains valid even if another owner releases its reference. Closing a `From*` wrapper releases only the wrapper's retained reference; the original owner retains responsibility for the `BinFile` close or `MemStream` lifetime.

All operations except `Close()` trap on a null or already-closed stream. `FromBinFile` and `FromMemStream` require an object of the matching runtime class. `Write(bytes)` traps when `bytes` is null, `WriteByte(value)` traps outside `0..255`, and `Read(count)` traps when `count` is negative. Setting `Position` traps if the underlying file seek fails. This avoids silent reads from or writes to invalid backing objects.

`Read(count)` is a short-read API for both file-backed and memory-backed streams: if fewer than
`count` bytes remain, it returns a `Bytes` object containing only the available bytes instead of
trapping. The runtime measures the current position and length first and clamps the allocation to
that remainder. An arbitrarily large requested count near EOF therefore cannot force an equally
large speculative allocation. A file can still change after the size snapshot, so the returned
object is right-sized again to the host read's actual byte count.

All Stream, BinFile, and MemStream receivers are validated as complete managed objects before their private fields are accessed. Constructor ownership is transactional: if allocation fails after a native file or backing object has been created, that resource is closed or released before the trap propagates. File short reads also keep the temporary `Bytes` buffer owned across backing-read and right-sizing allocations. As with ordinary buffered I/O, a file cursor can already have advanced if a later result-allocation step fails.

`AsBinFile()` is valid only on file-backed streams. `AsMemStream()` and `ToBytes()` are valid only on memory-backed streams. Calling one of these conversion methods on the wrong backing type traps instead of returning null.

The two `As*()` methods return an OWNED, concretely typed backing object (`BinFile` / `MemStream`)
with a retained reference, so the result stays valid after the owning Stream is closed or
finalized and the frontends account for its ownership correctly. The caller owns the returned
object like any other constructor result.

`Eof` is fully polymorphic: on both file and memory backings it is true exactly when the current position is at or past the end (`Pos >= Size`). Reading exactly the remaining bytes — including `ReadAll()` — leaves `Eof` true on either backing. (BinFile's own `Eof` property, for direct BinFile use, keeps C-stdio sticky-flag semantics; see [Files](files.md).)

### Zia Example

`Stream` is available from both Zia and BASIC. Use `Close()` when you own the stream and are done with it so wrapped file or memory resources are released promptly.

### BASIC Example

```basic
' Open a file stream
DIM fs AS OBJECT = Zanna.IO.Stream.OpenFile("data.bin", "rw")

' Write some data
DIM data AS Zanna.Collections.Bytes = Zanna.Collections.Bytes.FromStr("Hello, Stream!")
fs.Write(data)

' Seek back and read (set Pos property to seek)
fs.Position = 0
DIM readData AS Zanna.Collections.Bytes = fs.Read(14)
PRINT readData.ToStr()  ' Output: Hello, Stream!

fs.Close()

' Open a memory stream
DIM ms AS OBJECT = Zanna.IO.Stream.OpenMemory()

' Write to memory
ms.Write(Zanna.Collections.Bytes.FromStr("In-memory data"))

' Get all data
DIM allData AS Zanna.Collections.Bytes = ms.ToBytes()
PRINT "Length:"; allData.Length

ms.Close()
```

### Polymorphic Processing Example

```basic
' Process data from any stream source
SUB ProcessStream(stream AS OBJECT)
    ' Read header
    DIM header AS OBJECT = stream.Read(4)
    PRINT "Header:"; header.ToHex()

    ' Read remaining data
    DO
        DIM chunk AS OBJECT = stream.Read(1024)
        IF chunk.Length = 0 THEN EXIT DO
        ProcessChunk(chunk)
    LOOP
END SUB

' Use with file
DIM fileStream AS OBJECT = Zanna.IO.Stream.OpenFile("input.bin", "r")
ProcessStream(fileStream)
fileStream.Close()

' Use with memory (e.g., from network)
DIM memStream AS OBJECT = Zanna.IO.Stream.OpenMemory()
memStream.Write(networkData)
memStream.Position = 0
ProcessStream(memStream)
memStream.Close()
```

### Wrapping Existing Streams

```basic
' Wrap an existing BinFile
DIM bf AS OBJECT = Zanna.IO.BinFile.Open("data.bin", "r")
DIM wrapped AS OBJECT = Zanna.IO.Stream.FromBinFile(bf)

' Use the unified interface
DIM data AS OBJECT = wrapped.Read(100)

' The wrapper retains the BinFile while it is open
wrapped.Close()  ' Releases the wrapper's retained reference
bf.Close()       ' Close the original owner when you are done with it

' Wrap an existing MemStream
DIM ms AS OBJECT = Zanna.IO.MemStream.New()
ms.WriteStr("test data")
ms.Seek(0)

DIM wrappedMem AS OBJECT = Zanna.IO.Stream.FromMemStream(ms)
DIM raw AS Zanna.Collections.Bytes = wrappedMem.Read(9)
DIM str AS STRING = raw.ToStr()
```

### Use Cases

- **Abstraction:** Write code that works with both files and memory
- **Testing:** Use memory streams in tests, files in production
- **Buffering:** Read network data into memory stream, process uniformly
- **Interoperability:** Wrap existing BinFile/MemStream for unified access

### Stream vs BinFile vs MemStream

| Feature           | Stream                         | BinFile        | MemStream           |
|-------------------|--------------------------------|----------------|---------------------|
| Unified interface | Yes                            | No             | No                  |
| File backing      | Via OpenFile/FromBinFile       | Yes            | No                  |
| Memory backing    | Via OpenMemory/OpenBytes/FromMemStream | No     | Yes                 |
| Polymorphic use   | Yes                            | No             | No                  |
| Ownership control | Yes (From* vs Open*)           | Always owns    | Always owns         |
| Best for          | Polymorphic I/O code           | Direct file I/O| In-memory buffering |

Use Stream when:
- You need code that works with both files and memory
- You want to abstract the data source from processing logic
- You're building libraries that accept stream-like inputs

Use BinFile/MemStream directly when:
- You know the specific backing type
- You need type-specific features
- Maximum performance is critical

---

## Zanna.IO.MemStream

In-memory binary stream for reading and writing raw bytes with auto-expanding buffer.

**Type:** Instance class

**Constructors:**

- `Zanna.IO.MemStream.New()` - Creates an empty stream with capacity 0; storage is allocated on the first write
- `Zanna.IO.MemStream.NewCapacity(capacity)` - Creates an empty stream with specified initial capacity
- `Zanna.IO.MemStream.FromBytes(bytes)` - Creates a stream initialized with data from a Bytes object

### Properties

| Property   | Type    | Access     | Description                      |
|------------|---------|------------|----------------------------------|
| `Position` | Integer | Read/Write | Current stream position          |
| `Length`      | Integer | Read-only  | Current data length in bytes     |
| `Capacity` | Integer | Read-only  | Current buffer capacity in bytes |

### Methods

| Method            | Returns | Description                                      |
|-------------------|---------|--------------------------------------------------|
| `ReadI8()`        | Integer | Read signed 8-bit integer                        |
| `WriteI8(value)`  | void    | Write signed 8-bit integer                       |
| `ReadU8()`        | Integer | Read unsigned 8-bit integer                      |
| `WriteU8(value)`  | void    | Write unsigned 8-bit integer                     |
| `ReadI16()`       | Integer | Read signed 16-bit integer (little-endian)       |
| `WriteI16(value)` | void    | Write signed 16-bit integer (little-endian)      |
| `ReadU16()`       | Integer | Read unsigned 16-bit integer (little-endian)     |
| `WriteU16(value)` | void    | Write unsigned 16-bit integer (little-endian)    |
| `ReadI32()`       | Integer | Read signed 32-bit integer (little-endian)       |
| `WriteI32(value)` | void    | Write signed 32-bit integer (little-endian)      |
| `ReadU32()`       | Integer | Read unsigned 32-bit integer (little-endian)     |
| `WriteU32(value)` | void    | Write unsigned 32-bit integer (little-endian)    |
| `ReadI64()`       | Integer | Read signed 64-bit integer (little-endian)       |
| `WriteI64(value)` | void    | Write signed 64-bit integer (little-endian)      |
| `ReadF32()`       | Float   | Read 32-bit float (IEEE 754, little-endian)      |
| `WriteF32(value)` | void    | Write 32-bit float (IEEE 754, little-endian)     |
| `ReadF64()`       | Float   | Read 64-bit float (IEEE 754, little-endian)      |
| `WriteF64(value)` | void    | Write 64-bit float (IEEE 754, little-endian)     |
| `ReadBytes(n)`    | Bytes   | Read n bytes into a new Bytes object             |
| `WriteBytes(b)`   | void    | Write all bytes from a Bytes object              |
| `ReadStr(n)`      | String  | Read exactly n raw bytes into a String (no prefix) |
| `WriteStr(s)`     | void    | Write the String's raw bytes (no prefix or terminator) |
| `ToBytes()`       | Bytes   | Copy all data to a new Bytes object              |
| `Clear()`         | void    | Reset Pos and Length to zero while retaining capacity for reuse |
| `Seek(pos)`       | void    | Set position absolutely                          |
| `Skip(n)`         | void    | Move position by n bytes; negative values move backward |

### Byte Order

All multi-byte integers and floats use **little-endian** byte order, independent of the host CPU's native byte order.

### Buffer Behavior

- **Auto-expansion:** Writing beyond current capacity automatically grows the buffer
- **Lazy default allocation:** `New()` reports `Capacity = 0`; its first non-empty write allocates
  at least 64 bytes. `NewCapacity(n)` preallocates at least `n` bytes and rounds positive requests
  below 64 up to the 64-byte minimum growth quantum.
- **Efficient reserve:** Capacity growth does not initialize bytes that remain outside `Length`;
  only a gap made observable by a sparse write is zero-filled.
- **Efficient clear:** `Clear()` retains the existing buffer and does not wipe bytes outside the
  new zero length. Those bytes are unobservable; a later sparse write zero-fills the newly exposed
  gap before it becomes part of `Length`.
- **Gap filling:** Writing past the current length fills the gap with zeros
- **Sparse cursor:** `Position`, `Seek`, and positive `Skip` may move beyond `Length`; the next write zero-fills the gap. Reads there trap.
- **Read traps:** Reading past the end of data traps with an error
- **Overflow traps:** `Seek`, `Skip`, and writes that would overflow the signed 64-bit position or addressable capacity trap instead of wrapping.
- **Input validation:** `NewCapacity()` traps on negative capacities, `FromBytes()` and `WriteBytes()` require a `Bytes` object, fixed-width integer writes trap outside their representable range, and `WriteStr()` requires a valid runtime string. Use an empty string to encode zero bytes.
- **Portable signed encoding:** Signed reads reconstruct two's-complement values mathematically,
  and signed writes shift unsigned bit patterns. Results do not depend on host signed-shift or
  unsigned-to-signed conversion behavior.
- **Concurrency:** A MemStream has one mutable cursor and is not internally synchronized. Do not
  read, write, seek, or close the same instance concurrently without external serialization.

### Zia Example

```zia
module MemStreamDemo;

bind Zanna.Terminal;
bind Zanna.IO.MemStream as MS;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Create a new memory stream
    var ms = MS.New();

    // Write various data types
    ms.WriteI32(12345);       // 4 bytes
    ms.WriteF64(3.14159);     // 8 bytes
    ms.WriteStr("Hello");     // 5 bytes

    Say("Length: " + Fmt.Int(ms.get_Length()));   // 17

    // Seek back to start and read
    ms.Seek(0);
    Say("Int: " + Fmt.Int(ms.ReadI32()));      // 12345
    Say("Float: " + Fmt.Num(ms.ReadF64()));    // 3.14159
    Say("Str: " + ms.ReadStr(5));              // Hello

    // Clear and reuse
    ms.Clear();
    Say("After clear: " + Fmt.Int(ms.get_Length()));  // 0
}
```

> **Note:** MemStream properties (`Position`, `Length`, `Capacity`) use the get_/set_ pattern; access them as `ms.get_Length()`, `ms.get_Position()`, `ms.set_Position(n)` in Zia.

### BASIC Example

```basic
' Create a new memory stream
DIM ms AS OBJECT = Zanna.IO.MemStream.New()

' Write various data types
ms.WriteI32(12345)       ' 4 bytes
ms.WriteF64(3.14159)     ' 8 bytes
ms.WriteStr("Hello")     ' 5 bytes

PRINT "Length:"; ms.Length  ' Output: 17

' Seek back to start to read
ms.Seek(0)

' Read the data back
DIM intVal AS INTEGER = ms.ReadI32()
DIM floatVal AS FLOAT = ms.ReadF64()
DIM strVal AS STRING = ms.ReadStr(5)

PRINT intVal     ' Output: 12345
PRINT floatVal   ' Output: 3.14159
PRINT strVal     ' Output: Hello
```

### Binary Protocol Example

```basic
' Create a packet with header and payload
DIM packet AS OBJECT = Zanna.IO.MemStream.New()

' Write packet header
packet.WriteU8(202)     ' Magic byte 1
packet.WriteU8(254)     ' Magic byte 2
packet.WriteU16(1)       ' Version
packet.WriteU32(0)       ' Payload length (placeholder)

' Remember header end position
DIM headerEnd AS INTEGER = packet.Position

' Write payload
packet.WriteStr("Hello, World!")

' Calculate and update payload length
DIM payloadLen AS INTEGER = packet.Length - headerEnd
packet.Seek(4)           ' Position of length field
packet.WriteU32(payloadLen)

' Get final packet as bytes
DIM data AS OBJECT = packet.ToBytes()
PRINT "Packet size:"; data.Length  ' Output: 21
```

### FromBytes Example

```basic
' Load binary data into a stream for parsing
DIM rawData AS OBJECT = Zanna.IO.File.ReadAllBytes("data.bin")
DIM ms AS OBJECT = Zanna.IO.MemStream.FromBytes(rawData)

' Parse the binary format
DIM magic AS INTEGER = ms.ReadU32()
DIM count AS INTEGER = ms.ReadI16()

DIM i AS INTEGER
FOR i = 0 TO count - 1
    DIM value AS FLOAT = ms.ReadF64()
    PRINT "Value"; i; ":"; value
NEXT i
```

### Preallocated Capacity Example

```basic
' Preallocate buffer for known size
DIM ms AS OBJECT = Zanna.IO.MemStream.NewCapacity(1024)

PRINT "Initial capacity:"; ms.Capacity  ' Output: 1024
PRINT "Initial length:"; ms.Length         ' Output: 0

' Write data - no reallocation needed if under capacity
DIM i AS INTEGER
FOR i = 0 TO 99
    ms.WriteI32(i)
NEXT i

PRINT "Final length:"; ms.Length  ' Output: 400
```

### Use Cases

- **Binary protocols:** Build and parse network packets, file formats
- **Serialization:** Convert structured data to/from bytes
- **Buffer management:** Accumulate binary data before writing to file
- **Testing:** Create test data for binary file parsers
- **Data transformation:** Convert between formats in memory

### Comparison with BinFile

| Feature           | MemStream            | BinFile                    |
|-------------------|----------------------|----------------------------|
| Storage           | In-memory buffer     | Disk file                  |
| Auto-expand       | Yes                  | No (file grows on write)   |
| Random access     | Yes (via Pos/Seek)   | Yes (via Seek)             |
| Persistence       | No                   | Yes                        |
| Max size          | Limited by memory    | Limited by disk            |
| Performance       | Very fast            | Slower (disk I/O)          |

Use MemStream when:
- Building binary data in memory before writing to file or network
- Parsing binary data already loaded in memory
- Performance is critical and data fits in memory

Use BinFile when:
- Working with files that are too large for memory
- Data must persist across program runs
- Random access to large files is needed

---

## Zanna.IO.LineReader

Line-by-line text file reader with support for multiple line ending conventions.

**Type:** Instance class

**Constructor:** `Zanna.IO.LineReader.Open(path)`

### Line Endings

LineReader automatically handles all common line ending formats:

| Format | Characters | Description      |
|--------|------------|------------------|
| LF     | `\n`       | Unix/Linux/macOS |
| CR     | `\r`       | Classic Mac      |
| CRLF   | `\r\n`     | Windows          |

### Properties

| Property | Type    | Description                        |
|----------|---------|------------------------------------|
| `Eof`    | Boolean | True if at end of file (read-only) |

### Methods

| Method       | Returns | Description                                                  |
|--------------|---------|--------------------------------------------------------------|
| `Close()`    | void    | Close the file and release resources                         |
| `Read()`     | String  | Read one line (without newline); returns empty string at EOF |
| `ReadChar()` | Integer | Read single character (0-255) or -1 at EOF                   |
| `PeekChar()` | Integer | View next character without consuming (0-255 or -1)          |
| `ReadAll()`  | String  | Read all remaining content as a string                       |

`Read()` marks `Eof` immediately after returning the final line when the file ends with a line terminator, so loops do not need one extra read to discover EOF. Because an empty line and EOF both produce `""`, use `PeekChar() >= 0` when that distinction matters. A single line is capped at 256 MiB. `ReadAll()` begins at the logical current cursor, includes a byte previously cached by `PeekChar()`, and advances to EOF; it never rewinds and rereads content already consumed. It traps if the remaining byte count cannot be represented by the host allocation size.

Open keeps cleanup ownership of the native file until the managed LineReader is published. `Read()` and `ReadAll()` likewise own their temporary byte buffers across runtime-string construction, so a recoverable allocation trap cannot leak either resource. A LineReader has one shared cursor/peek state and must be externally serialized if referenced from multiple threads.

### Zia Example

```zia
module LineReaderDemo;

bind Zanna.Terminal;
bind Zanna.IO.LineReader as LR;
bind Zanna.IO.File as File;

func start() {
    // Create a test file
    File.WriteAllText("/tmp/lr_test.txt", "Line 1\nLine 2\nLine 3\n");

    // Read line by line
    var reader = LR.Open("/tmp/lr_test.txt");
    Say(reader.Read());    // Line 1
    Say(reader.Read());    // Line 2
    Say(reader.Read());    // Line 3
    reader.Close();

    File.Delete("/tmp/lr_test.txt");
}
```

### BASIC Example

```basic
' Read a file line by line
DIM reader AS OBJECT = Zanna.IO.LineReader.Open("data.txt")

DO WHILE reader.PeekChar() >= 0
    DIM line AS STRING = reader.Read()
    PRINT line
LOOP

reader.Close()

' Character-by-character reading
reader = Zanna.IO.LineReader.Open("chars.txt")

DO WHILE NOT reader.Eof
    DIM ch AS INTEGER = reader.ReadChar()
    IF ch >= 0 THEN
        PRINT CHR(ch);
    END IF
LOOP

reader.Close()

' Peek at next character without consuming
reader = Zanna.IO.LineReader.Open("peek.txt")

' Peek and read
DIM nextChar AS INTEGER = reader.PeekChar()
PRINT "Next char will be: "; CHR(nextChar)

DIM actualChar AS INTEGER = reader.ReadChar()
PRINT "Read char: "; CHR(actualChar)   ' Same as peeked

reader.Close()

' Read entire remaining file content
reader = Zanna.IO.LineReader.Open("large.txt")

' Skip first line
DIM header AS STRING = reader.Read()

' Read everything else
DIM content AS STRING = reader.ReadAll()
PRINT "Remaining content length: "; LEN(content)

reader.Close()
```

### Use Cases

- **Text file processing:** Process files line by line
- **Log file reading:** Parse log files with various line endings
- **Configuration parsing:** Read config files line by line
- **Character-level parsing:** Build custom parsers with PeekChar/ReadChar
- **Cross-platform files:** Handle files with different line ending conventions

---

## Zanna.IO.LineWriter

Buffered text file writer with configurable line endings.

**Type:** Instance class

**Constructors:**

- `Zanna.IO.LineWriter.Open(path)` - Create or overwrite file
- `Zanna.IO.LineWriter.Append(path)` - Open for appending

### Properties

| Property  | Type   | Description                                           |
|-----------|--------|-------------------------------------------------------|
| `NewLine` | String | Line ending string (read/write, defaults to platform) |

### Methods

| Method          | Returns | Description                          |
|-----------------|---------|--------------------------------------|
| `Close()`       | void    | Close the file and release resources |
| `Write(text)`   | void    | Write string without newline; traps if text is invalid |
| `WriteLine(text)` | void    | Write string followed by newline; traps if text or `NewLine` is invalid |
| `WriteChar(ch)` | void    | Write single character (0-255); traps outside that range |
| `Flush()`       | void    | Flush buffered output to disk        |

### Platform Newlines

| Platform         | Default NewLine |
|------------------|-----------------|
| Windows          | `\r\n` (CRLF)   |
| Unix/Linux/macOS | `\n` (LF)       |

`LineWriter` writes the `NewLine` bytes exactly as configured. On Windows it opens files in binary mode, so `\r\n` is written once and custom newline strings are not altered by CRT text translation.

Open and Append close the native file and release the default newline if managed wrapper allocation fails. Assigning `NewLine` is transactional: the replacement is retained or allocated before the previous value is released, so an allocation trap preserves the old separator. String byte lengths are checked for host `size_t` representability before stdio calls, including 32-bit hosts. A LineWriter is not internally synchronized; concurrent writes, newline changes, flushes, and close require external serialization.

### Zia Example

```zia
module LineWriterDemo;

bind Zanna.Terminal;
bind Zanna.IO.LineWriter as LW;
bind Zanna.IO.File as File;

func start() {
    // Write lines to a file
    var writer = LW.Open("/tmp/lw_test.txt");
    writer.WriteLine("First line");
    writer.WriteLine("Second line");
    writer.Write("No newline here");
    writer.Close();

    // Verify contents
    Say(File.ReadAllText("/tmp/lw_test.txt"));

    File.Delete("/tmp/lw_test.txt");
}
```

`Append()` is registered for both Zia and BASIC. `Close()` and `Flush()` trap when the platform reports a delayed write failure.

### BASIC Example

```basic
' Write text to a file
DIM writer AS OBJECT = Zanna.IO.LineWriter.Open("output.txt")

' Write lines with automatic newline
writer.WriteLine("First line")
writer.WriteLine("Second line")

' Write without newline
writer.Write("No ")
writer.Write("newline ")
writer.Write("here")
writer.WriteLine("")  ' Add newline at end

writer.Close()

' Append to existing file
writer = Zanna.IO.LineWriter.Append("output.txt")
writer.WriteLine("Appended line")
writer.Close()

' Custom line endings (Windows-style)
writer = Zanna.IO.LineWriter.Open("windows.txt")
writer.NewLine = CHR(13) + CHR(10)  ' CRLF
writer.WriteLine("Windows line ending")
writer.Close()

' Unix-style line endings
writer = Zanna.IO.LineWriter.Open("unix.txt")
writer.NewLine = CHR(10)  ' LF only
writer.WriteLine("Unix line ending")
writer.Close()

' Write individual characters
writer = Zanna.IO.LineWriter.Open("chars.txt")
DIM i AS INTEGER
FOR i = 65 TO 90  ' A-Z
    writer.WriteChar(i)
NEXT
writer.Close()
```

### Use Cases

- **Text file generation:** Create configuration files, reports, logs
- **Cross-platform output:** Control line endings for target platform
- **Log writing:** Append entries to log files
- **Data export:** Write CSV, TSV, or other text formats
- **Code generation:** Generate source code with proper line endings

---

## Zanna.IO.BinaryBuffer

Positioned binary read/write buffer for constructing and parsing binary data in memory. Maintains an internal cursor position that advances automatically with each read or write.

**Type:** Instance class

**Constructors:**

- `Zanna.IO.BinaryBuffer.New()` - Creates an empty buffer with a 256-byte initial capacity
- `Zanna.IO.BinaryBuffer.NewCapacity(capacity)` - Creates an empty buffer with specified initial capacity
- `Zanna.IO.BinaryBuffer.FromBytes(data)` - Creates a buffer initialized with data from a Bytes object

### Properties

| Property | Type    | Access     | Description                   |
|----------|---------|------------|-------------------------------|
| `Position` | Integer | Read/Write | Current buffer position     |
| `Length`    | Integer | Read-only  | Current data length in bytes  |

### Write Methods

| Method              | Returns | Description                                      |
|---------------------|---------|--------------------------------------------------|
| `WriteByte(v)`      | void    | Write a single byte (0-255) at current position  |
| `WriteI16LittleEndian(v)`     | void    | Write signed 16-bit integer (little-endian)      |
| `WriteI16BigEndian(v)`     | void    | Write signed 16-bit integer (big-endian)         |
| `WriteU16LittleEndian(v)`     | void    | Write unsigned 16-bit integer (little-endian)    |
| `WriteU16BigEndian(v)`     | void    | Write unsigned 16-bit integer (big-endian)       |
| `WriteI32LittleEndian(v)`     | void    | Write signed 32-bit integer (little-endian)      |
| `WriteI32BigEndian(v)`     | void    | Write signed 32-bit integer (big-endian)         |
| `WriteU32LittleEndian(v)`     | void    | Write unsigned 32-bit integer (little-endian)    |
| `WriteU32BigEndian(v)`     | void    | Write unsigned 32-bit integer (big-endian)       |
| `WriteI64LittleEndian(v)`     | void    | Write 64-bit integer (little-endian)             |
| `WriteI64BigEndian(v)`     | void    | Write 64-bit integer (big-endian)                |
| `WriteStr(s)`       | void    | Write a little-endian signed 32-bit length prefix plus full string bytes |
| `WriteBytes(b)`     | void    | Write a little-endian signed 32-bit length prefix plus all Bytes data   |

### Read Methods

| Method              | Returns | Description                                      |
|---------------------|---------|--------------------------------------------------|
| `ReadByte()`        | Integer | Read a single byte (0-255) at current position   |
| `ReadI16LittleEndian()`       | Integer | Read signed 16-bit integer (little-endian)       |
| `ReadI16BigEndian()`       | Integer | Read signed 16-bit integer (big-endian)          |
| `ReadU16LittleEndian()`       | Integer | Read unsigned 16-bit integer (little-endian)     |
| `ReadU16BigEndian()`       | Integer | Read unsigned 16-bit integer (big-endian)        |
| `ReadI32LittleEndian()`       | Integer | Read signed 32-bit integer (little-endian)       |
| `ReadI32BigEndian()`       | Integer | Read signed 32-bit integer (big-endian)          |
| `ReadU32LittleEndian()`       | Integer | Read unsigned 32-bit integer (little-endian)     |
| `ReadU32BigEndian()`       | Integer | Read unsigned 32-bit integer (big-endian)        |
| `ReadI64LittleEndian()`       | Integer | Read 64-bit integer (little-endian)              |
| `ReadI64BigEndian()`       | Integer | Read 64-bit integer (big-endian)                 |
| `ReadStr()`         | String  | Read the little-endian length prefix and string payload |
| `ReadBytes(count)`  | Bytes   | Read count raw bytes into a new Bytes object (no prefix handling) |

### Control Methods

| Method       | Returns | Description                              |
|--------------|---------|------------------------------------------|
| `ToBytes()`  | Bytes   | Copy all data to a new Bytes object      |
| `Reset()`    | void    | Reset buffer to empty state (Pos=0, Len=0) |

### Notes

- `WriteStr()` uses the runtime string byte length, so embedded NUL bytes are preserved.
- `FromBytes()` and `WriteBytes()` require a `Bytes` object. `WriteStr()` requires a valid runtime string; use an empty string to encode zero bytes. `WriteByte()` traps outside `0..255`, and fixed-width integer writes trap outside their declared signed or unsigned range.
- `WriteStr()` and `WriteBytes()` trap if the payload length cannot fit in their signed 32-bit length prefix.
- `ReadStr()` is the direct counterpart to `WriteStr()`. `ReadBytes(count)` does not consume the prefix emitted by `WriteBytes()`; read that 32-bit little-endian length explicitly, then pass it as `count`.
- Setting `Position` traps for negative positions or positions beyond `Length`; it does not clamp.
- Signed integer readers sign-extend their declared width: `ReadI16*()` returns -32768..32767, `ReadI32*()` returns signed 32-bit values, and `ReadI64*()` preserves the full signed 64-bit bit pattern.
- Unsigned readers zero-extend their declared width: `ReadU16*()` returns 0..65535 and `ReadU32*()` returns 0..4294967295.
- Constructors and growth trap if the requested capacity exceeds the host platform's addressable allocation size.
- `Reset()` clears position and logical length but retains allocated capacity for reuse.
- A fixed-width write whose value is out of range traps *before* touching the buffer — no
  truncated value is ever appended — and every property getter (`Position`, `Length`, `ToBytes`,
  `Reset`) is safe on an invalid receiver, so a recoverable trap that resumes never leaves the
  buffer corrupted or dereferences a null receiver.
- `NewCap` remains available as a compatibility alias for `NewCapacity`.

### Zia Example

```zia
module BinaryBufferDemo;

bind Zanna.Terminal;
bind Zanna.IO.BinaryBuffer as BB;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Create a new buffer
    var buf = BB.New();

    // Write various data types
    buf.WriteByte(0xFF);
    buf.WriteI32LittleEndian(12345);
    buf.WriteI32BigEndian(67890);
    buf.WriteStr("Hello");

    Say("Length: " + Fmt.Int(buf.get_Length()));

    // Seek back to start and read
    buf.set_Position(0);
    Say("Byte: " + Fmt.Int(buf.ReadByte()));        // 255
    Say("I32LE: " + Fmt.Int(buf.ReadI32LittleEndian()));       // 12345
    Say("I32BE: " + Fmt.Int(buf.ReadI32BigEndian()));       // 67890

    // Export to Bytes
    var data = buf.ToBytes();
    Say("Exported len: " + Fmt.Int(data.Length));
}
```

> **Note:** BinaryBuffer properties (`Position`, `Length`) use the get_/set_ pattern in Zia; access them as `buf.get_Length()`, `buf.get_Position()`, `buf.set_Position(n)`.

### BASIC Example

```basic
' Create a new binary buffer
DIM buf AS OBJECT = Zanna.IO.BinaryBuffer.New()

' Write various data types
buf.WriteByte(202)
buf.WriteI16LittleEndian(1000)
buf.WriteI32BigEndian(123456)
buf.WriteI64LittleEndian(9876543210)
buf.WriteStr("Hello")
buf.WriteBytes(Zanna.Collections.Bytes.FromHex("deadbeef"))

PRINT "Length:"; buf.Length

' Seek back and read
buf.Position = 0
PRINT "Byte:"; buf.ReadByte()       ' Output: 202
PRINT "I16LE:"; buf.ReadI16LittleEndian()     ' Output: 1000
PRINT "I32BE:"; buf.ReadI32BigEndian()     ' Output: 123456
PRINT "I64LE:"; buf.ReadI64LittleEndian()     ' Output: 9876543210

' Export to Bytes
DIM data AS OBJECT = buf.ToBytes()
PRINT "Exported:"; data.Length

' Reset and reuse
buf.Reset()
PRINT "After reset:"; buf.Length       ' Output: 0
```

### Preallocated Capacity Example

```basic
' Preallocate buffer for a known packet size
DIM buf AS OBJECT = Zanna.IO.BinaryBuffer.NewCapacity(256)

' Build a binary protocol packet
buf.WriteU16BigEndian(51966)         ' Magic number
buf.WriteI32BigEndian(1)              ' Version
buf.WriteStr("payload data")

DIM packet AS OBJECT = buf.ToBytes()
```

### FromBytes Example

```basic
' Parse binary data received from network
DIM rawData AS OBJECT = Zanna.IO.File.ReadAllBytes("packet.bin")
DIM buf AS OBJECT = Zanna.IO.BinaryBuffer.FromBytes(rawData)

' Parse the binary format
DIM magic AS INTEGER = buf.ReadU16BigEndian()
DIM version AS INTEGER = buf.ReadI32BigEndian()
DIM payload AS STRING = buf.ReadStr()
```

### BinaryBuffer vs MemStream

| Feature           | BinaryBuffer            | MemStream                    |
|-------------------|-------------------------|------------------------------|
| Endianness        | Both LE and BE methods  | Little-endian only           |
| Float support     | No                      | Yes (F32, F64)               |
| Auto-expand       | Yes                     | Yes                          |
| Random access     | Yes (via Pos)           | Yes (via Pos/Seek)           |
| Best for          | Network protocols, mixed-endian formats | Structured binary data, IEEE floats |

Use BinaryBuffer when:
- You need both big-endian and little-endian operations
- Building network protocol packets (many protocols use big-endian)
- Parsing binary formats with mixed byte orders

Use MemStream when:
- Working with floating-point data (F32, F64)
- All data is little-endian
- You want cursor movement beyond the current logical end followed by zero-filled writes

### Use Cases

- **Network protocols:** Build and parse TCP/UDP packets with big-endian fields
- **File format parsing:** Read binary headers with mixed byte orders
- **Serialization:** Encode structured data for storage or transmission
- **Database pages:** Read and write fixed-format binary records

---


## See Also

- [Files & Directories](files.md)
- [Advanced IO](advanced.md)
- [Input & Output Overview](README.md)
- [Zanna Runtime Library](../README.md)
