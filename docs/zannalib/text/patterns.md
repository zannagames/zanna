---
status: active
audience: public
last-verified: 2026-07-26
---

# Pattern Matching
> Pattern, CompiledPattern, Scanner, Diff, String.Like/LikeCI

**Part of [Zanna Runtime Library](../README.md) › [Text Processing](README.md)**

---

## Zanna.Text.Pattern

Byte-oriented regular-expression-subset matching for text search and manipulation.

**Type:** Static utility class

### Methods

| Method                                | Signature                   | Description                                        |
|---------------------------------------|-----------------------------|----------------------------------------------------|
| `IsMatch(text, pattern)`              | `Boolean(String, String)`   | Test if pattern matches anywhere in text           |
| `Find(text, pattern)`           | `Option[String](String, String)` | Find first match as `Some(match)`, including empty-string matches, or `None` |
| `FindFrom(text, pattern, start)`      | `Option[String](String, String, Integer)` | Find first match at or after start as `Some(match)`, or `None` |
| `FindPos(text, pattern)`              | `Option[Integer](String, String)` | Find position of first match as `Some(index)`, or `None` |
| `FindAll(text, pattern)`              | `Seq(String, String)`       | Find all non-overlapping matches                   |
| `Replace(text, pattern, replacement)` | `String(String, String, String)` | Replace all matches with replacement         |
| `ReplaceFirst(text, pattern, replacement)` | `String(String, String, String)` | Replace first match only                |
| `Split(text, pattern)`                | `Seq(String, String)`       | Split text by pattern matches                      |
| `Escape(text)`                        | `String(String)`            | Escape special regex characters for literal matching |

### Supported Regex Syntax

| Feature              | Syntax               | Description                                      |
|----------------------|----------------------|--------------------------------------------------|
| Literals             | `abc`                | Match literal bytes                              |
| Dot                  | `.`                  | Match any byte except `LF`                       |
| Anchors              | `^` `$`              | Start/end of string                              |
| Character class      | `[abc]` `[a-z]`      | Match any byte in the set                        |
| Negated class        | `[^abc]` `[^0-9]`    | Match any byte not in the set                    |
| Shorthand: digit     | `\d` `\D`            | Digit `[0-9]` / non-digit                        |
| Shorthand: word      | `\w` `\W`            | Word char `[a-zA-Z0-9_]` / non-word              |
| Shorthand: space     | `\s` `\S`            | Whitespace / non-whitespace                      |
| Quantifier: star     | `*` `*?`             | Zero or more (greedy / non-greedy)               |
| Quantifier: plus     | `+` `+?`             | One or more (greedy / non-greedy)                |
| Quantifier: optional | `?` `??`             | Zero or one (greedy / non-greedy)                |
| Grouping             | `(abc)`              | Group subexpressions                             |
| Alternation          | `a` &#124; `b`       | Match either alternative                         |
| Escape               | `\\` `\.` `\*` etc.  | Match literal special character                  |

### NOT Supported

The following advanced regex features are not implemented:

- Backreferences (`\1`, `\2`, etc.)
- Lookahead/lookbehind (`(?=...)`, `(?<=...)`, etc.)
- Named groups (`(?P<name>...)`)
- Unicode categories (`\p{L}`, etc.)
- Possessive quantifiers (`*+`, `++`, etc.)
- Bounded quantifiers (`{n}`, `{n,m}`)

### Traps

- Invalid pattern syntax traps with a descriptive error message
- Null pattern strings trap. Null text and null replacement strings are treated as empty strings.

### Byte Semantics

- Pattern matching uses runtime string byte length, so embedded `NUL` bytes in text can be matched
  and returned. Dot, classes, shorthands, literals, and quantifiers all operate on bytes; `\d` and
  `\w` are ASCII-only, and `\s` is the six-byte set space, tab, `LF`, `CR`, form feed, and vertical
  tab.
- Pattern source must not contain embedded `NUL` bytes: any `Pattern.*` call traps with
  `Pattern: pattern contains NUL byte` rather than silently compiling only the prefix.
- Match positions and `FindFrom` start values are byte offsets. Negative starts clamp to zero; a
  start beyond the text returns no match.
- Replacement text is literal; `$1`, `\1`, and similar capture-reference spellings are not
  expanded.
- Zero-width matches follow ECMAScript-style semantics: `Replace("abc", "", "-")` yields
  `-a-b-c-` (the stepped-over byte is preserved), and `Split("abc", "")` yields `a`, `b`, `c`
  (an empty match never splits at the current segment start or at end-of-text).
- Parentheses are semantics-neutral: `(a*)a` matches exactly what `a*a` matches, and
  `Captures` succeeds whenever `Find` succeeds. Capture groups are numbered lexically by
  opening parenthesis; a group in an untaken alternation branch reports an empty string, and
  a quantified group reports its last repetition.
- Uppercase complement shorthands (`\D`, `\W`, `\S`) may be combined freely with other
  members inside a character class; each shorthand contributes its own byte set to the union
  (`[a\D]` matches `a` and every non-digit).
- The regex cache is safe for concurrent users of cached patterns; in-use compiled entries are not evicted.
- `Find()`, `FindFrom()`, and `FindPos()` all return Options, so a valid empty-string match (`Some("")`) is distinguishable from no match (`None`).

### Zia Example

```zia
module PatternDemo;

bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Pattern functions take (text, pattern) order
    Say("Match: " + Fmt.Bool(Zanna.Text.Pattern.IsMatch("hello123", "[a-z]+[0-9]+")));
    var found = Zanna.Text.Pattern.Find("Price is $42.50", "[0-9]+");
    if found.IsSome {
        Say("Find: " + found.UnwrapStr());
    }
    Say("Replace: " + Zanna.Text.Pattern.Replace("foo 123 bar 456", "[0-9]+", "#"));
}
```

### BASIC Example

```basic
' Match test - argument order is (text, pattern)
PRINT "Match: "; Zanna.Text.Pattern.IsMatch("hello123", "[a-z]+[0-9]+")

' Find first match
DIM found AS OBJECT = Zanna.Text.Pattern.Find("Price is $42.50", "[0-9]+")
IF found.IsSome THEN
    PRINT "Find: "; found.UnwrapStr()
END IF

' Replace all matches
PRINT "Replace: "; Zanna.Text.Pattern.Replace("foo 123 bar 456", "[0-9]+", "#")
```

### Pattern Matching BASIC Example

```basic
' Basic matching
DIM text AS STRING = "Hello, World!"

' Check if pattern matches
IF Zanna.Text.Pattern.IsMatch(text, "\w+") THEN
    PRINT "Contains word characters"
END IF

' Find first match
DIM word AS OBJECT = Zanna.Text.Pattern.Find(text, "[A-Z][a-z]+")
IF word.IsSome THEN
    PRINT word.UnwrapStr()  ' Output: "Hello"
END IF

' Find position
DIM pos AS OBJECT = Zanna.Text.Pattern.FindPos(text, "World")
IF pos.IsSome THEN
    PRINT pos.UnwrapI64()  ' Output: 7
END IF

' Find all matches
DIM words AS Zanna.Collections.Seq = Zanna.Text.Pattern.FindAll(text, "\w+")
PRINT words.Count  ' Output: 2 (Hello, World)
```

### Replace Example

```basic
' Replace all digits with X
DIM result AS STRING = Zanna.Text.Pattern.Replace("abc123def456", "\d+", "X")
PRINT result  ' Output: "abcXdefX"

' Replace first match only
result = Zanna.Text.Pattern.ReplaceFirst("abc123def456", "\d+", "X")
PRINT result  ' Output: "abcXdef456"

' Remove all whitespace
result = Zanna.Text.Pattern.Replace("hello   world  test", "\s+", "")
PRINT result  ' Output: "helloworldtest"
```

### Split Example

```basic
' Split by whitespace
DIM parts AS Zanna.Collections.Seq = Zanna.Text.Pattern.Split("hello   world  test", "\s+")
PRINT parts.Count  ' Output: 3
PRINT Zanna.Collections.Seq.GetStr(parts, 0) ' Output: "hello"
PRINT Zanna.Collections.Seq.GetStr(parts, 1) ' Output: "world"
PRINT Zanna.Collections.Seq.GetStr(parts, 2) ' Output: "test"

' Split by comma
parts = Zanna.Text.Pattern.Split("a,b,c,d", ",")
PRINT parts.Count  ' Output: 4
```

### Escape Example

```basic
' Escape special characters for literal matching
DIM pattern AS STRING = Zanna.Text.Pattern.Escape("file.txt")
PRINT pattern  ' Output: "file\.txt"

' Use escaped pattern to match literal dot
DIM matches AS INTEGER = Zanna.Text.Pattern.IsMatch("file.txt", pattern)
PRINT matches  ' Output: 1 (true)

' Without escaping, dot matches any character
matches = Zanna.Text.Pattern.IsMatch("fileXtxt", "file.txt")
PRINT matches  ' Output: 1 (true - dot matched X)
```

### Email Validation Example

```basic
' Simple email pattern (not comprehensive)
DIM email_pattern AS STRING = "^\w+@\w+\.\w+$"

FUNCTION IsValidEmail(email AS STRING) AS BOOLEAN
    RETURN Zanna.Text.Pattern.IsMatch(email, email_pattern)
END FUNCTION

PRINT IsValidEmail("user@example.com")  ' Output: 1 (true)
PRINT IsValidEmail("invalid-email")     ' Output: 0 (false)
```

### Performance Notes

- Compiled patterns are cached internally (LRU cache, 16 entries)
- Frequently used patterns avoid recompilation overhead
- For maximum performance with many operations, use consistent pattern strings
- For repeated operations with the same pattern, consider `CompiledPattern`
- Matching is backtracking with a one-million-step cap shared across each search. Reaching the cap
  is reported as no match rather than as a separate timeout result.

---

## Zanna.Text.CompiledPattern

Pre-compiled regular expression for efficient repeated matching. Use this when applying the same pattern to multiple
strings to avoid recompilation overhead.

**Type:** Instance class
**Constructor:** `NEW Zanna.Text.CompiledPattern(pattern)`

### Properties

| Property  | Type   | Description                                   |
|-----------|--------|-----------------------------------------------|
| `Pattern` | String | The C-string prefix that was compiled          |

### Methods

| Method                             | Signature                    | Description                                        |
|------------------------------------|------------------------------|----------------------------------------------------|
| `IsMatch(text)`                    | `Boolean(String)`            | Test if pattern matches anywhere in text           |
| `Find(text)`                 | `Option[String](String)`     | Find first match as `Some(match)`, including empty-string matches, or `None` |
| `FindFrom(text, start)`            | `Option[String](String, Integer)` | Find first match at or after start as `Some(match)`, or `None` |
| `FindPos(text)`                    | `Option[Integer](String)`    | Find position of first match as `Some(index)`, or `None` |
| `FindAll(text)`                    | `Seq(String)`                | Find all non-overlapping matches                   |
| `Captures(text)`                   | `Seq(String)`                | Get capture groups from first match                |
| `CapturesFrom(text, start)`        | `Seq(String, Integer)`       | Get capture groups starting from position          |
| `Replace(text, replacement)`       | `String(String, String)`     | Replace all matches with replacement               |
| `ReplaceFirst(text, replacement)`  | `String(String, String)`     | Replace first match only                           |
| `Split(text)`                      | `Seq(String)`                | Split text by pattern matches                      |
| `Split(text, limit)`               | `Seq(String, Integer)`       | Split text with a maximum number of parts          |

### Capture Groups

For patterns that the capture matcher can represent correctly, `Captures` and `CapturesFrom`
return a Seq containing:
- Index 0: The full match
- Index 1+: Captured groups in order of opening parentheses

If there is no match, an empty Seq is returned. Alternation does not retain empty slots for
unmatched lexical groups, and the capture matcher can fail even when `Find` succeeds; see
[VDOC-057](../../../misc/reviews/documentation-review-findings.md#vdoc-057--compiledpatterncaptures-uses-a-different-matcher-and-group-numbering).

Prefer the Option-returning `Find*Option()` methods for branchable search code. The legacy
`Find()` and `FindFrom()` methods return an empty string for no match, which is ambiguous
when the pattern itself can match an empty string.

### Notes

- Null text and replacement arguments are treated as empty strings.
- Returned matches preserve runtime string byte length, including embedded `NUL` bytes.
- A null pattern passed to the constructor traps.
- A pattern containing an embedded `NUL` byte traps at construction
  (`CompiledPattern: pattern contains NUL byte`), so `Pattern` always round-trips the
  constructor input.
- `FindFrom` and `CapturesFrom` use byte offsets, clamp negative starts to zero, and return no
  result when the start is beyond the text.
- `Replace` and `ReplaceFirst` insert the replacement literally; they do not expand capture-group
  references. Zero-width matches preserve every source byte (see the static `Pattern` notes).
- The two-argument `Split(text, limit)` returns at most `limit` pieces when `limit > 0`; zero or a negative limit is unlimited.

### Zia Example

```zia
module CompiledPatternDemo;

bind Zanna.Terminal;
bind Zanna.Text.CompiledPattern as CompiledPattern;

func start() {
    var numbers = CompiledPattern.New("\\d+");
    Say(numbers.Find("abc123def"));  // 123
    Say(numbers.Replace("a1 b22", "#"));  // a# b#
}
```

### BASIC Example

```basic
' Compile a pattern once for multiple uses
DIM numberPattern AS Zanna.Text.CompiledPattern = NEW Zanna.Text.CompiledPattern("\d+")

' Process multiple strings efficiently
DIM texts AS Zanna.Collections.Seq = NEW Zanna.Collections.Seq()
texts.Push("abc123def")
texts.Push("foo456bar")
texts.Push("no digits here")

FOR i = 0 TO texts.Count - 1
    DIM text AS STRING = Zanna.Collections.Seq.GetStr(texts, i)
    IF numberPattern.IsMatch(text) THEN
        PRINT "Found number: "; numberPattern.Find(text)
    ELSE
        PRINT "No number in: "; text
    END IF
NEXT
' Output:
' Found number: 123
' Found number: 456
' No number in: no digits here
```

### Capture Groups Example

```basic
' Pattern with capture groups
DIM datePattern AS Zanna.Text.CompiledPattern = NEW Zanna.Text.CompiledPattern("(\d\d\d\d)-(\d\d)-(\d\d)")

DIM groups AS Zanna.Collections.Seq = datePattern.Captures("Today is 2024-01-15")
IF groups.Count > 0 THEN
    PRINT "Full match: "; Zanna.Collections.Seq.GetStr(groups, 0) ' Output: 2024-01-15
    PRINT "Year: "; Zanna.Collections.Seq.GetStr(groups, 1)       ' Output: 2024
    PRINT "Month: "; Zanna.Collections.Seq.GetStr(groups, 2)      ' Output: 01
    PRINT "Day: "; Zanna.Collections.Seq.GetStr(groups, 3)        ' Output: 15
END IF

' Email extraction
DIM emailPattern AS Zanna.Text.CompiledPattern = NEW Zanna.Text.CompiledPattern("(\w+)@(\w+)\.(\w+)")
groups = emailPattern.Captures("Contact: user@example.com")
IF groups.Count > 0 THEN
    PRINT "Local part: "; Zanna.Collections.Seq.GetStr(groups, 1) ' Output: user
    PRINT "Host: "; Zanna.Collections.Seq.GetStr(groups, 2)       ' Output: example
    PRINT "TLD: "; Zanna.Collections.Seq.GetStr(groups, 3)    ' Output: com
END IF
```

### Split with Limit Example

```basic
DIM commaPattern AS Zanna.Text.CompiledPattern = NEW Zanna.Text.CompiledPattern(",")

' Split all
DIM all AS Zanna.Collections.Seq = commaPattern.Split("a,b,c,d,e")
PRINT all.Count  ' Output: 5

' Split with limit (max 3 parts)
DIM limited AS Zanna.Collections.Seq = commaPattern.Split("a,b,c,d,e", 3)
PRINT limited.Count        ' Output: 3
PRINT Zanna.Collections.Seq.GetStr(limited, 0) ' Output: a
PRINT Zanna.Collections.Seq.GetStr(limited, 1) ' Output: b
PRINT Zanna.Collections.Seq.GetStr(limited, 2) ' Output: c,d,e (rest in last element)
```

### When to Use CompiledPattern vs Pattern

| Scenario                            | Recommendation             |
|-------------------------------------|----------------------------|
| One-time pattern match              | Use `Pattern` (simpler)    |
| Same pattern on multiple strings    | Use `CompiledPattern`      |
| Pattern in a loop                   | Use `CompiledPattern`      |
| Need capture groups                 | Use `CompiledPattern`      |
| Dynamic pattern from user input     | Use `Pattern` (compiled once) |

### Performance Notes

- Compilation is paid once per `CompiledPattern` instance, while matching still depends on both pattern and input complexity
- The matcher is a backtracking engine with a per-search step cap; pathological patterns can stop matching at that cap rather than backtrack indefinitely
- Static `Pattern` operations share a lock-protected 16-entry LRU compile cache. An explicit `CompiledPattern` is useful when a pattern must remain compiled independently of that cache.
- Capture extraction supports any number of groups (storage is sized from the compiled
  pattern), plus element 0 for the whole match. A quantified group captures its last
  repetition.

---

## Zanna.Text.Scanner

Stateful string scanner for lexing and parsing text. Maintains a position cursor and provides methods for peeking, reading, matching, and skipping characters and tokens.

**Type:** Instance class
**Constructor:** `New(text)` -- creates a scanner positioned at the start of the given text

### Properties

| Property    | Type    | Access     | Description                              |
|-------------|---------|------------|------------------------------------------|
| `Position`  | Integer | Read/Write | Current byte position (0-indexed)        |
| `IsEnd`     | Boolean | Read-only  | True if at end of string                 |
| `Remaining` | Integer | Read-only  | Number of bytes remaining                |
| `Length`    | Integer | Read-only  | Total byte length of the source string   |

### Methods

| Method                | Signature                  | Description                                                    |
|-----------------------|----------------------------|----------------------------------------------------------------|
| `Reset()`             | `Void()`                   | Reset position to beginning of string                          |
| `Peek()`              | `Integer()`                | Peek at current character without advancing (-1 if at end)     |
| `PeekAt(offset)`      | `Integer(Integer)`         | Peek at character at offset from current position              |
| `PeekStr(n)`          | `String(Integer)`          | Peek at up to the next n bytes without advancing                |
| `Read()`              | `Integer()`                | Read current character and advance (-1 if at end)              |
| `ReadStr(n)`          | `String(Integer)`          | Read up to the next n bytes and advance                         |
| `ReadUntil(delim)`    | `String(Integer)`          | Read until a delimiter byte, leaving it unconsumed              |
| `ReadUntilAny(chars)` | `String(String)`           | Read until any byte in the delimiter string                     |
| `Match(c)`            | `Boolean(Integer)`         | Check if current position matches character (no advance)       |
| `MatchStr(s)`         | `Boolean(String)`          | Check if current position matches string (no advance)          |
| `Accept(c)`           | `Boolean(Integer)`         | Match and consume character if it matches                      |
| `AcceptStr(s)`        | `Boolean(String)`          | Match and consume string if it matches                         |
| `AcceptAny(chars)`    | `Boolean(String)`          | Match and consume any one of the given characters              |
| `Skip(n)`             | `Void(Integer)`            | Skip up to n bytes                                              |
| `SkipWhitespace()`    | `Integer()`                | Skip space, tab, `LF`, and `CR`; return the byte count           |
| `ReadIdent()`         | `String()`                 | Read an identifier (letter/underscore start, then alnum/underscore) |
| `ReadIntToken()`           | `String()`                 | Read an integer (optional sign + digits)                       |
| `ReadNumberToken()`   | `String()`                 | Read a decimal number token with optional sign/exponent         |
| `ReadQuoted(quote)`   | `String(Integer)`          | Consume a quoted string and decode its simple escapes           |
| `ReadLine()`          | `String()`                 | Read a line and consume its `LF`, `CR`, or `CRLF` terminator    |

### Notes

- The scanner operates on byte positions; all character values are integers from 0 through 255
- The scanner retains the source string for its lifetime, so scanning remains valid even if the caller releases its reference
- A null source creates an empty scanner
- `Peek`, `PeekAt`, and `Read` return `-1` when their byte is out of range. `PeekAt` is relative to
  `Position` and accepts negative offsets.
- `Match` and `MatchStr` test without advancing; `Accept` and `AcceptStr` advance only if matched
- Null delimiter/character-set arguments fail safely; `ReadUntilAny(NULL)` reads the rest of the source
- `PeekStr` and `ReadStr` return empty for `n <= 0`; `Skip` is a no-op for `n <= 0`. Positive
  lengths clamp at the end of the source.
- `ReadIdent` recognizes ASCII letters/underscore followed by ASCII alphanumerics/underscore.
  `ReadIntToken` accepts an optional sign followed by at least one digit. `ReadNumber` additionally
  accepts forms such as `.5`, `1.`, and `1.5e-2`; an incomplete exponent is left unconsumed.
- `ReadIdent`, `ReadIntToken`, `ReadNumber`, and `ReadQuoted` return an empty string without advancing if
  the current position does not start the requested token.
- `ReadQuoted` decodes `\n`, `\t`, `\r`, `\\`, `\"`, and `\'`. For another escaped byte it drops
  the backslash and keeps that byte. An unterminated quoted string restores `Pos` and traps.
- Setting `Pos` to a value outside the valid range clamps it to the string boundaries

### Zia Example

```zia
module ScannerDemo;

bind Zanna.Terminal;
bind Zanna.Text.Scanner as Scanner;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var sc = Scanner.New("hello 42 world");

    var ident = sc.ReadIdent();
    Say("Ident: " + ident);                         // hello
    sc.SkipWhitespace();
    var num = sc.ReadIntToken();
    Say("Int: " + num);                              // 42
    sc.SkipWhitespace();
    var rest = sc.ReadIdent();
    Say("Rest: " + rest);                            // world
    Say("AtEnd: " + Fmt.Bool(sc.IsEnd));             // true
    Say("Pos: " + Fmt.Int(sc.Position));                  // 14
}
```

### BASIC Example

```basic
' Create a scanner for a string
DIM sc AS Zanna.Text.Scanner = Zanna.Text.Scanner.New("hello 42 world")

' Read an identifier
DIM ident AS STRING = sc.ReadIdent()
PRINT ident       ' Output: "hello"

' Skip whitespace
sc.SkipWhitespace()

' Read an integer
DIM num AS STRING = sc.ReadIntToken()
PRINT num          ' Output: "42"

' Skip whitespace and read another identifier
sc.SkipWhitespace()
DIM rest AS STRING = sc.ReadIdent()
PRINT rest         ' Output: "world"

' Check position and end state
PRINT sc.IsEnd     ' Output: 1 (true)
PRINT sc.Position       ' Output: 14
PRINT sc.Length       ' Output: 14

' Reset and scan again
sc.Reset()
PRINT sc.Position       ' Output: 0
PRINT sc.Remaining ' Output: 14

' Peek without advancing
DIM ch AS INTEGER = sc.Peek()
PRINT CHR$(ch)     ' Output: "h"
PRINT sc.Position       ' Output: 0 (unchanged)

' Read characters one at a time
ch = sc.Read()
PRINT CHR$(ch)     ' Output: "h"
PRINT sc.Position       ' Output: 1 (advanced)
```

### Parsing Example

```basic
' Parse a simple key=value format
DIM sc AS Zanna.Text.Scanner = Zanna.Text.Scanner.New("name=Alice age=30")

DO WHILE NOT sc.IsEnd
    sc.SkipWhitespace()
    IF sc.IsEnd THEN EXIT DO
    DIM key AS STRING = sc.ReadIdent()
    IF sc.Accept(61) THEN  ' 61 = ASCII '='
        DIM value AS STRING = sc.ReadUntilAny(" " + CHR$(10))
        PRINT key; " -> "; value
    END IF
LOOP
' Output:
' name -> Alice
' age -> 30
```

### Use Cases

- **Lexing:** Tokenize source code or structured text
- **Parsing:** Build simple parsers for custom data formats
- **Data extraction:** Pull structured fields from text
- **Protocol handling:** Parse simple text-based protocols

---

## Zanna.Text.Diff

Line-based text differencing using a longest-common-subsequence table. It computes a minimal insertion/deletion script for typical inputs, can render a compact diff, and can reconstruct the modified text.

**Type:** Static utility class

### Methods

| Method                       | Signature                    | Description                                              |
|------------------------------|------------------------------|----------------------------------------------------------|
| `CountChanges(a, b)`        | `Integer(String, String)`    | Count number of added + removed lines between two texts  |
| `Lines(a, b)`               | Registry: `Object(String, String)`; runtime: `Seq` | Compute a full line edit script |
| `Patch(original, diff)`     | `String(String, Object)`     | Reconstruct modified text from a full `Lines` result |
| `Unified(a, b, context)`    | `String(String, String, Integer)` | Produce compact unified-diff-style text with context lines |

### Notes

- Each entry in the `Lines` result is prefixed: `" "` (unchanged), `"+"` (added), `"-"` (removed)
- `Lines` is registered as `seq<str>`, so chained access such as `Diff.Lines(a, b).Count`
  resolves against the returned sequence.
- `Patch` reconstructs the modified text from the full `Lines` records and validates the diff
  against `original`: every context and removed line must match the corresponding original
  line, and the diff must consume the original exactly, or the call traps with
  `Diff.Patch: diff does not apply to the original text`.
- `Unified` emits `--- a` / `+++ b` headers and selected prefixed lines, but no `@@` hunk headers; it is diagnostic text, not a patch-applicable standard unified diff
- `Unified` treats a negative context as 3; zero includes only added and removed records.
- Lines split only at `LF`. A preceding `CR` remains part of line content, so LF and CRLF inputs
  compare byte-differently.
- The implementation uses O(n*m) time and space for n and m input lines and traps when the
  `(n + 1) * (m + 1)` table would exceed 16,777,216 cells.
- Diffing and patching use runtime string byte lengths, so lines containing embedded `NUL` bytes are compared and reconstructed without truncation
- `Lines` returns an owned sequence of owned line strings

### Zia Example

```zia
module DiffDemo;

bind Zanna.Terminal;
bind Zanna.Text.Diff as Diff;
bind Zanna.Text.Fmt as Fmt;
bind Zanna.Collections.Seq as Seq;

func start() {
    var a = "hello world";
    var b = "hello there";
    Say("Changes: " + Fmt.Int(Diff.CountChanges(a, b)));  // 2

    var orig = "hello\nworld\nfoo";
    var modified = "hello\nthere\nfoo";
    var diff = Diff.Lines(orig, modified);
    Say("Diff lines: " + Fmt.Int(Seq.get_Count(diff)));

    var unified = Diff.Unified(orig, modified, 1);
    Say(unified);

    // Round-trip: apply the diff to get modified text back
    var patched = Diff.Patch(orig, diff);
    Say("Patched: " + patched);  // hello\nthere\nfoo
}
```

### BASIC Example

```basic
' Count changes between two strings
DIM a AS STRING = "hello world"
DIM b AS STRING = "hello there"
DIM changes AS INTEGER = Zanna.Text.Diff.CountChanges(a, b)
PRINT changes  ' Output: 2

' Compute line-by-line diff
DIM orig AS STRING = "hello" + CHR$(10) + "world" + CHR$(10) + "foo"
DIM modified AS STRING = "hello" + CHR$(10) + "there" + CHR$(10) + "foo"
DIM diff AS Zanna.Collections.Seq = Zanna.Text.Diff.Lines(orig, modified)
PRINT diff.Count  ' Output: 4 (one unchanged, one removed, one added, one unchanged)

' Produce unified diff
DIM unified AS STRING = Zanna.Text.Diff.Unified(orig, modified, 1)
PRINT unified

' Apply patch to reconstruct modified text
DIM patched AS STRING = Zanna.Text.Diff.Patch(orig, diff)
PRINT patched = modified  ' Output: 1 (true)
```

### Use Cases

- **Code review:** Show differences between file versions
- **Configuration auditing:** Detect changes in config files
- **Testing:** Compare expected vs actual output
- **Patching:** Apply diffs to transform text programmatically

---

## String.Like / String.LikeCI

SQL-style LIKE pattern matching on strings. These are methods available on any String value, providing wildcard matching commonly used in database queries and filtering.

**Type:** String instance methods

### Methods

| Method              | Signature            | Description                                        |
|---------------------|----------------------|----------------------------------------------------|
| `Like(pattern)`     | `Boolean(String)`    | Case-sensitive SQL LIKE pattern matching            |
| `LikeIgnoreCase(pattern)`   | `Boolean(String)`    | Case-insensitive SQL LIKE pattern matching          |

### Pattern Syntax

| Pattern | Description                                     | Example                          |
|---------|-------------------------------------------------|----------------------------------|
| `%`     | Matches any sequence of zero or more UTF-8-shaped units | `"hello".Like("%llo")` is true |
| `_`     | Matches exactly one UTF-8-shaped unit           | `"hello".Like("h_llo")` is true  |
| `\`     | Escape character for literal `%`, `_`, or `\`   | `"100%".Like("100\%")` is true   |

Matching covers the entire string, rather than searching for a matching substring. `_` and `%`
advance by strictly validated UTF-8 code points (overlong encodings, UTF-16 surrogates, and
code points above U+10FFFF trap with `String.Like: invalid UTF-8 sequence`), while literal
matching remains bytewise.
`LikeIgnoreCase` folds ASCII letters only, independent of the process locale; it is not a
Unicode case algorithm.
A null receiver or pattern is treated as an empty string. A backslash quotes any following pattern
byte; a final backslash matches a literal backslash.

### Zia Example

```zia
module LikeDemo;

bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Wildcard matching
    Say(Fmt.Bool("hello".Like("h%")));          // true
    Say(Fmt.Bool("hello".Like("%llo")));         // true
    Say(Fmt.Bool("hello".Like("h_llo")));        // true
    Say(Fmt.Bool("hello".Like("world")));         // false

    // Case-insensitive matching
    Say(Fmt.Bool("Hello".LikeIgnoreCase("hello")));      // true
    Say(Fmt.Bool("Hello".LikeIgnoreCase("HELLO")));      // true
    Say(Fmt.Bool("Hello".LikeIgnoreCase("h%")));         // true
}
```

### BASIC Example

```basic
' Basic wildcard matching
PRINT "hello".Like("h%")         ' Output: 1 (starts with h)
PRINT "hello".Like("%llo")       ' Output: 1 (ends with llo)
PRINT "hello".Like("%ell%")      ' Output: 1 (contains ell)
PRINT "hello".Like("h_llo")      ' Output: 1 (_ matches e)
PRINT "hello".Like("h__lo")      ' Output: 1 (__ matches el)
PRINT "hello".Like("world")      ' Output: 0 (no match)

' Case-insensitive matching
PRINT "Hello World".LikeIgnoreCase("hello%")  ' Output: 1
PRINT "Hello World".LikeIgnoreCase("HELLO%")  ' Output: 1
PRINT "Hello World".LikeIgnoreCase("%WORLD")  ' Output: 1

' Escape special characters
PRINT "100%".Like("100\%")       ' Output: 1 (literal %)
PRINT "file_name".Like("file\_name")  ' Output: 1 (literal _)
```

### Use Cases

- **SQL query emulation:** Implement WHERE column LIKE pattern filtering
- **Filename matching:** Match files against user-specified patterns
- **Search filters:** Provide wildcard search in user interfaces
- **Data validation:** Check if strings match expected patterns

---

## Zanna.Text.FuzzyMatch

Reusable scoring and highlight ranges for command palettes, quick-open, and symbol search.

**Type:** Static utility class

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Score(query, candidate)` | `Integer(String, String)` | Return the raw subsequence score; an unmatched query returns `-1` |
| `Match(query, candidate)` | `Map(String, String)` | Return `matched`, `score`, `query`, `candidate`, and matched `ranges` |

`Match` returns a Map with Boolean `matched`, Integer `score`, String `query` and `candidate`, and
`ranges`. Ranges is a Seq of Maps with Integer `start` and `end` fields. The offsets are zero-based
byte offsets into the candidate, and `end` is exclusive.

### Notes

- Empty queries match with score `0`.
- Matching is a greedy, case-insensitive byte-subsequence search. Exact-case, separator,
  camel-case, and consecutive-byte hits score higher; gaps, a late first hit, and longer candidates
  lower the score.
- A successful match always scores `>= 0` (heavily penalized matches clamp to `0`), so `-1`
  from `Score` reliably means the query did not match.
- Case conversion and boundary detection are ASCII-only and locale-independent; bytes above
  0x7F never fold or count as letters, so results are identical on every host.
- Null query/candidate values are treated as empty strings. Returned query/candidate strings and
  byte ranges preserve embedded `NUL` bytes.

### Zia Example

```zia
module FuzzyDemo;

bind Zanna.Terminal;

func start() {
    var m = Zanna.Text.FuzzyMatch.Match("VS", "ZannaScene.zia");
    SayBool(m.GetBool("matched"));
    SayInt(m.GetInt("score"));
}
```

---


## See Also

- [Encoding & Identity](encoding.md)
- [Data Formats](formats.md)
- [Formatting & Generation](formatting.md)
- [Text Processing Overview](README.md)
- [Zanna Runtime Library](../README.md)
