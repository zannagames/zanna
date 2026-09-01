---
status: active
audience: public
last-verified: 2026-09-01
---

# Specialized Maps
> BiMap, MultiMap, CountMap, IntMap, DefaultMap, LruCache, WeakMap, SparseArray

**Part of [Zanna Runtime Library](../README.md) › [Collections](README.md)**

---

## Zanna.Collections.BiMap

A bidirectional map that maintains a one-to-one mapping between keys and values, allowing efficient lookup in both
directions. Each key maps to exactly one value, and each value maps to exactly one key.

**Type:** Instance (obj)
**Constructor:** `Zanna.Collections.BiMap.New()`

### Properties

| Property  | Type    | Description                          |
|-----------|---------|--------------------------------------|
| `Count`      | Integer | Number of key-value pairs in the map |
| `IsEmpty` | Boolean | True if the map has no entries       |

### Methods

| Method                 | Signature              | Description                                                       |
|------------------------|------------------------|-------------------------------------------------------------------|
| `Set(key, value)`      | `Void(String, String)` | Add or update a bidirectional mapping; removes any old mapping    |
| `GetByKey(key)`        | `String(String)`       | Get an owned value copy; empty string if the key is missing       |
| `GetByValue(value)`    | `String(String)`       | Get an owned key copy; empty string if the value is missing       |
| `HasKey(key)`          | `Boolean(String)`      | Check if a key exists                                             |
| `HasValue(value)`      | `Boolean(String)`      | Check if a value exists                                           |
| `Keys()`               | `Seq()`                | Get all keys as a Seq                                             |
| `Values()`             | `Seq()`                | Get all values as a Seq                                           |
| `RemoveByKey(key)`     | `Boolean(String)`      | Remove a mapping by key; returns true if found                    |
| `RemoveByValue(value)` | `Boolean(String)`      | Remove a mapping by value; returns true if found                  |
| `Clear()`              | `Void()`               | Remove all mappings                                               |

### Notes

- Maintains strict one-to-one relationships: updating a key to a new value removes the old value's reverse mapping
- Both key-to-value and value-to-key lookups are O(1) average case and O(n) worst case
- `Set` with an existing key replaces the old value and removes the old value's reverse entry
- Keys and values are strings compared by full byte length; embedded NUL bytes are part of identity
- `Set` prepares the replacement entry before removing conflicting mappings, so allocation failures do not drop the previous entry
- `Put` remains available as a compatibility alias for `Set`.
- `Keys()` and `Values()` return owning snapshots of copied strings in matching forward
  hash-table order. Corresponding indices form a pair; the order itself is unspecified.
- The map is not thread-safe; synchronize externally around concurrent access.

### Zia Example

```zia
module BiMapDemo;

bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var bm = BiMap.New();

    // Add bidirectional mappings
    bm.Set("en", "English");
    bm.Set("fr", "French");
    Say("Count: " + Fmt.Int(bm.Count));                  // 2

    // Forward lookup (key -> value)
    Say("GetByKey en: " + bm.GetByKey("en"));         // English

    // Reverse lookup (value -> key)
    Say("GetByValue French: " + bm.GetByValue("French")); // fr

    // Membership checks
    Say(Fmt.Bool(bm.HasKey("en")));                   // true
    Say(Fmt.Bool(bm.HasValue("English")));            // true

    // Remove by key
    bm.RemoveByKey("en");
    SayInt(bm.Count);                                   // 1
}
```

### BASIC Example

```basic
DIM bm AS OBJECT
bm = Zanna.Collections.BiMap.New()

' Add bidirectional mappings
bm.Set("en", "English")
bm.Set("fr", "French")
PRINT "Count: "; bm.Count                       ' Count: 2

' Forward lookup (key -> value)
PRINT "GetByKey en: "; bm.GetByKey("en")     ' GetByKey en: English

' Reverse lookup (value -> key)
PRINT "GetByValue French: "; bm.GetByValue("French")  ' GetByValue French: fr

' Membership checks
PRINT "HasKey fr: "; bm.HasKey("fr")         ' HasKey fr: -1
PRINT "HasValue English: "; bm.HasValue("English")  ' HasValue English: -1

' Remove by key
bm.RemoveByKey("en")
PRINT "After remove: "; bm.Count              ' After remove: 1
```

### Use Cases

- **Locale/language maps:** Map language codes to names and vice versa
- **Encoding tables:** Bidirectional lookup between encoded and decoded values
- **Identifier mapping:** Two-way mapping between internal IDs and external names
- **Bijective transformations:** Any scenario requiring invertible mappings

---

## Zanna.Collections.MultiMap

A map that associates each string key with multiple values. Unlike Map which stores one value per key, MultiMap
stores a list of values for each key.

**Type:** Instance (obj)
**Constructor:** `Zanna.Collections.MultiMap.New()`

### Properties

| Property   | Type    | Description                                   |
|------------|---------|-----------------------------------------------|
| `IsEmpty`  | Boolean | True if the map has no entries                |
| `KeyCount` | Integer | Number of distinct keys                       |
| `Count`       | Integer | Total number of values across all keys        |

### Methods

| Method             | Signature              | Description                                                      |
|--------------------|------------------------|------------------------------------------------------------------|
| `Add(key, value)`  | `Void(String, Object)` | Add a value to the key's list (does not replace existing values) |
| `Get(key)`         | `Seq(String)`          | Get all values for key as a Seq (empty Seq if key not found)     |
| `GetFirst(key)`    | `Object(String)`       | Get the first value added for key                                |
| `Has(key)`         | `Boolean(String)`      | Check if key exists                                              |
| `CountFor(key)`    | `Integer(String)`      | Get the number of values for a key (0 if not found)              |
| `Keys()`           | `Seq()`                | Get all distinct keys as a Seq                                   |
| `RemoveAll(key)`   | `Boolean(String)`      | Remove a key and all its values; returns true if found           |
| `Clear()`          | `Void()`               | Remove all entries                                               |

### Notes

- `Count` is the *total* number of values across all keys, not the number of distinct keys
- `KeyCount` gives the number of distinct keys
- `Get` returns an empty Seq (not null) if the key does not exist
- Values for each key remain in addition order and duplicates are allowed.
- `Get` returns an owning snapshot Seq that can be mutated independently; values remain retained
  by the snapshot until it is released.
- `GetFirst` returns an owned object reference for the first value, or null when the key has no values
- String keys are compared by full byte length; embedded NUL bytes are part of the key
- `Keys()` returns an owning snapshot of copied keys in unspecified hash-table order.
- Values are boxed objects in Zia (use `Zanna.Core.Box`); BASIC auto-boxes string values
- `Put` remains available as a compatibility alias for `Add`.
- The current registry types `Get()` as `Any` even though the returned object is a `Seq`; in Zia,
  either annotate a `Seq` local or use static `Seq` operations rather than chaining instance
  properties directly.
- The map is not thread-safe; synchronize externally around concurrent access.

### Zia Example

```zia
module MultiMapDemo;

bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var mm = MultiMap.New();

    // Add multiple values per key
    mm.Add("color", Box.Str("red"));
    mm.Add("color", Box.Str("green"));
    mm.Add("color", Box.Str("blue"));
    mm.Add("size", Box.Str("small"));
    mm.Add("size", Box.Str("large"));

    SayInt(mm.Count);                              // 5 (total values)
    SayInt(mm.KeyCount);                         // 2 (distinct keys)

    // Get all values for a key.
    // MultiMap.Get returns a bare `obj`, so narrow it to Seq before use.
    var colors: Seq = mm.Get("color") as Seq;
    SayInt(Seq.get_Count(colors));                 // 3
    Say(Box.ToStr(Seq.Get(colors, 0)));          // red
    Say(Box.ToStr(Seq.Get(colors, 1)));          // green
    Say(Box.ToStr(Seq.Get(colors, 2)));          // blue

    // Get first value for a key
    Say(Box.ToStr(mm.GetFirst("color")));        // red

    // Count values for a key
    SayInt(mm.CountFor("color"));                // 3
    SayInt(mm.CountFor("shape"));                // 0

    // Remove all values for a key
    mm.RemoveAll("color");
    SayInt(mm.Count);                              // 2
    SayInt(mm.KeyCount);                         // 1
}
```

### BASIC Example

```basic
DIM mm AS OBJECT
mm = Zanna.Collections.MultiMap.New()

' Add multiple values per key
mm.Add("color", "red")
mm.Add("color", "green")
mm.Add("color", "blue")
mm.Add("size", "small")
mm.Add("size", "large")

PRINT mm.Count             ' 5 (total values)
PRINT mm.KeyCount        ' 2 (distinct keys)

' Get all values for a key
DIM colors AS Zanna.Collections.Seq
colors = mm.Get("color")
PRINT colors.Count         ' 3
PRINT colors.Get(0)      ' red
PRINT colors.Get(1)      ' green
PRINT colors.Get(2)      ' blue

' Get first value
PRINT mm.GetFirst("color")   ' red
PRINT mm.GetFirst("size")    ' small

' Count values for a key
PRINT mm.CountFor("color")   ' 3
PRINT mm.CountFor("shape")   ' 0

' Get for missing key returns empty Seq
DIM empty AS Zanna.Collections.Seq
empty = mm.Get("shape")
PRINT empty.Count              ' 0

' Remove all values for a key
PRINT mm.RemoveAll("color")  ' 1
PRINT mm.Count                 ' 2
PRINT mm.KeyCount            ' 1

' Clear all
mm.Clear()
PRINT mm.IsEmpty             ' 1
```

### Use Cases

- **HTTP headers:** Multiple values for the same header name
- **Tag systems:** Associate multiple tags with each item
- **Graph adjacency lists:** Store multiple edges per vertex
- **Index building:** Map each term to multiple document IDs
- **Event handlers:** Register multiple handlers for each event type

---

## Zanna.Collections.CountMap

A frequency counter that maps string keys to integer counts. Provides convenient increment, decrement, and
ranking operations for counting occurrences.

**Type:** Instance (obj)
**Constructor:** `Zanna.Collections.CountMap.New()`

### Properties

| Property  | Type    | Description                                |
|-----------|---------|--------------------------------------------|
| `IsEmpty` | Boolean | True if the map has no entries             |
| `Count`      | Integer | Number of distinct keys in the map         |
| `Total`   | Integer | Sum of all counts across all keys          |

### Methods

| Method              | Signature               | Description                                                         |
|---------------------|-------------------------|---------------------------------------------------------------------|
| `Increment(key)`          | `Integer(String)`       | Increment count for key by 1; returns new count                     |
| `Decrement(key)`          | `Integer(String)`       | Decrement count for key by 1; removes key if count reaches 0        |
| `IncrementBy(key, n)`     | `Integer(String, Integer)` | Add positive n; non-positive n returns 0 without changing the map|
| `Get(key)`          | `Integer(String)`       | Get current count for key (0 if not present)                        |
| `Set(key, count)`   | `Void(String, Integer)` | Set a positive count; zero or negative removes the key               |
| `Has(key)`          | `Boolean(String)`       | Check if key exists in the map                                      |
| `Remove(key)`       | `Boolean(String)`       | Remove a key entirely; returns true if found                        |
| `Keys()`            | `Seq()`                 | Get all keys as a Seq                                               |
| `MostCommon(n)`     | `Seq(Integer)`          | Get top n keys sorted by count descending                           |
| `Clear()`           | `Void()`                | Remove all entries                                                  |

### Notes

- `Dec` automatically removes a key when its count reaches zero and returns 0 for a missing key.
- `Get` returns 0 for keys that have never been added (does not insert)
- `IncrementBy` returns the post-operation count. Zero is a lookup no-op (returns the current
  count, or 0 for a missing key); a negative amount traps — use `Decrement` to count down.
- `MostCommon(n)` returns copied key strings ordered from highest count to lowest, all keys when
  `n` exceeds `Count`, and an empty Seq when `n <= 0`. Ties have unspecified order.
- `Keys()` returns copied strings in unspecified hash-table order.
- `Total` is the sum of all counts, not the number of distinct keys
- String keys are compared by full byte length; embedded NUL bytes are part of the key
- A null key passed through the runtime API is treated as the empty string key.
- Count and total overflow trap instead of wrapping.
- `MostCommon()` is registered with an unqualified object return even though it returns `Seq`.
  In Zia, assign it to an explicitly typed `Seq` before accessing `Count`; a direct chain can call
  `CountMap.Count` on the returned Seq and trap.
- The map is not thread-safe; synchronize externally around concurrent access.

### Zia Example

```zia
module CountMapDemo;

bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var cm: CountMap = CountMap.New();

    // Count occurrences
    cm.Increment("apple");
    cm.Increment("apple");
    cm.Increment("banana");
    cm.Increment("cherry");
    cm.Increment("cherry");
    cm.Increment("cherry");
    SayInt(cm.Count);                // 3 (distinct keys)

    // Bulk increment
    cm.IncrementBy("banana", 5);
    SayInt(cm.Get("banana"));      // 6

    // Total across all keys
    SayInt(cm.Total);              // 2 + 6 + 3 = 11

    // Most common
    var top: Seq = cm.MostCommon(2);
    SayInt(top.Count);               // 2

    // Decrement removes at zero
    cm.Decrement("apple");               // count -> 1
    cm.Decrement("apple");               // count -> 0, removed
    SayBool(cm.Has("apple"));      // 0
}
```

### BASIC Example

```basic
DIM cm AS OBJECT
cm = Zanna.Collections.CountMap.New()

' Count word occurrences
cm.Increment("apple")
cm.Increment("apple")
cm.Increment("banana")
cm.Increment("cherry")
cm.Increment("cherry")
cm.Increment("cherry")
PRINT cm.Count                ' 3 (distinct keys)

' Bulk increment
cm.IncrementBy("banana", 5)
PRINT cm.Get("banana")      ' 6

' Total of all counts
PRINT cm.Total               ' 11

' Set a count directly
cm.Set("date", 10)
PRINT cm.Get("date")        ' 10

' Decrement (removes key when count reaches 0)
cm.Decrement("apple")              ' 1
cm.Decrement("apple")              ' 0 (removed)
PRINT cm.Has("apple")        ' 0

' Get top entries
DIM top AS OBJECT
top = cm.MostCommon(2)
PRINT top.Count                ' 2

' Remove a key entirely
cm.Remove("date")
PRINT cm.Has("date")         ' 0

' Clear all
cm.Clear()
PRINT cm.IsEmpty             ' 1
```

### Use Cases

- **Word frequency analysis:** Count occurrences of words in text
- **Vote counting:** Tally votes for candidates
- **Event tracking:** Count how often each event type occurs
- **Histogram building:** Build frequency distributions
- **Top-N queries:** Find the most common items with `MostCommon`

---

## Zanna.Collections.IntMap

An integer-keyed dictionary for efficient mapping of integer keys to object values. Uses a hash table with O(1) average-case operations.

**Type:** Instance (obj)
**Constructor:** `Zanna.Collections.IntMap.New()`

### Properties

| Property  | Type    | Description                            |
|-----------|---------|----------------------------------------|
| `Count`      | Integer | Number of key-value pairs in the map   |
| `IsEmpty` | Boolean | True if the map has no entries         |

### Methods

| Method                        | Signature                  | Description                                                              |
|-------------------------------|----------------------------|--------------------------------------------------------------------------|
| `Set(key, value)`             | `Void(Integer, Object)`    | Add or update a key-value pair                                           |
| `Get(key)`                    | `Object(Integer)`          | Get the borrowed value (NULL if missing)                                 |
| `GetOr(key, default)`         | `Object(Integer, Object)`  | Get the borrowed value, or `default` if missing (does not insert)        |
| `Has(key)`                    | `Boolean(Integer)`         | Check if key exists                                                      |
| `Remove(key)`                 | `Boolean(Integer)`         | Remove key-value pair; returns true if found                             |
| `Clear()`                     | `Void()`                   | Remove all entries                                                       |
| `Trim()`                      | `Boolean()`                | Return excess bucket capacity; true if already minimal or trim succeeds |
| `Keys()`                      | `Seq()`                    | Get sequence of all keys as boxed i64 values                             |
| `Values()`                    | `Seq()`                    | Get sequence of all values                                               |

### Notes

- Keys are stored as signed 64-bit integers; no pointer casts are used for returned key snapshots.
- `Get()` and `GetOr()` return borrowed references. A present key may store null: `Has()` remains
  true and `GetOr()` returns null rather than its default.
- `Keys()` and `Values()` return retained snapshots in matching, unspecified hash-table order.
  Key elements are boxed `i64` values; corresponding indices identify the same entry.
- Values are retained while stored and released on overwrite, removal, clear, or finalization.
- Hashes are cached in entries, and growth is transactional: a replacement
  bucket table is allocated completely before any entry is relinked. Allocation
  failure leaves all keys, values, count, and capacity unchanged.
- `Clear()` retains buckets. `Trim()` explicitly returns excess bucket storage
  while preserving enough capacity for the current count below the 75-percent
  growth threshold. Failed trim traps and leaves the prior table intact.
- The map is not thread-safe; synchronize externally around concurrent access.

### Zia Example

```zia
module IntMapDemo;

bind Zanna.Terminal;
bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var m = IntMap.New();

    // Add entries with integer keys
    m.Set(1, Box.Str("one"));
    m.Set(2, Box.Str("two"));
    m.Set(3, Box.Str("three"));

    Say("Count: " + Fmt.Int(m.Count));                  // 3
    Say("Has 2: " + Fmt.Bool(m.Has(2)));             // true
    Say("Get 1: " + Box.ToStr(m.Get(1)));            // one

    // Remove
    m.Remove(2);
    Say("Has 2: " + Fmt.Bool(m.Has(2)));             // false
}
```

### BASIC Example

```basic
DIM m AS OBJECT = Zanna.Collections.IntMap.New()

' Add entries
m.Set(100, "apple")
m.Set(200, "banana")
m.Set(300, "cherry")

PRINT m.Count      ' Output: 3
PRINT m.IsEmpty  ' Output: 0

' Check existence and get value
IF m.Has(100) THEN
    PRINT m.Get(100)  ' Output: apple
END IF

' Get with default
PRINT m.GetOr(999, "unknown")  ' Output: unknown

' Remove
IF m.Remove(200) THEN
    PRINT "Removed 200"
END IF

' Iterate keys
DIM keys AS OBJECT = m.Keys()
FOR i = 0 TO keys.Count - 1
    PRINT keys.Get(i)
NEXT i

' Clear all
m.Clear()
PRINT m.IsEmpty  ' Output: 1
```

### Use Cases

- **Sparse arrays:** Map non-contiguous integer indices to values
- **ID-based lookup:** Look up objects by numeric identifier
- **Counting by ID:** Count occurrences keyed by integer values
- **Graph adjacency:** Map node IDs to neighbor lists

---

## Zanna.Collections.DefaultMap

A map that returns a configurable default value for missing keys instead of null. The default value is a boxed
value specified at construction time via `Zanna.Core.Box`.

**Type:** Instance (obj)
**Constructor:** `Zanna.Collections.DefaultMap.New(defaultValue)` - `defaultValue` is a boxed value (e.g., `Box.Str("N/A")`, `Box.I64(0)`)

### Properties

| Property  | Type    | Description                          |
|-----------|---------|--------------------------------------|
| `Count`   | Integer | Number of key-value pairs in the map |
| `IsEmpty` | Boolean | True if the map has no entries       |

### Methods

| Method            | Signature              | Description                                                   |
|-------------------|------------------------|---------------------------------------------------------------|
| `Get(key)`        | `Object(String)`       | Get a borrowed value, or the borrowed default when missing    |
| `Set(key, value)` | `Void(String, Object)` | Set a key-value pair                                          |
| `Has(key)`        | `Boolean(String)`      | Check if a key exists (Get does not insert missing keys)      |
| `Remove(key)`     | `Boolean(String)`      | Remove a key-value pair; true if found                        |
| `Keys()`          | `Seq()`                | Get all keys as a Seq                                         |
| `GetDefault()`    | `Object()`             | Get the borrowed default configured at construction           |
| `Clear()`         | `Void()`               | Remove all entries                                            |

### Notes

- `Get` for a missing key returns the same default object but does *not* insert the key into the map.
- The default reference is set at construction time and cannot be replaced. The referenced object
  is not cloned or frozen and may itself still be mutable. Null is permitted as the default.
- `Get()` and `GetDefault()` return borrowed references. A present key with a null value overrides
  a non-null default; use `Has()` to distinguish it from a missing key.
- Passing a null key through the runtime API is treated as the empty string key
- String keys are compared by full byte length; embedded NUL bytes are part of the key
- Values and the configured default value are retained while stored
- `Keys()` returns an owning snapshot of copied strings in unspecified keyed-hash order.
- Values are boxed objects; use `Zanna.Core.Box` to create and unwrap values
- In BASIC, string values are automatically boxed when passed to `Set`
- The map is not thread-safe; synchronize externally around concurrent access.

### Zia Example

```zia
module DefaultMapDemo;

bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Create with string default
    var dm = DefaultMap.New(Box.Str("N/A"));

    dm.Set("name", Box.Str("Alice"));
    dm.Set("city", Box.Str("Boston"));
    SayInt(dm.Count);                            // 2

    // Existing key returns stored value
    Say(Box.ToStr(dm.Get("name")));            // Alice

    // Missing key returns default
    Say(Box.ToStr(dm.Get("email")));           // N/A
    SayBool(dm.Has("email"));                  // 0 (not inserted)

    // Get the default value
    Say(Box.ToStr(dm.GetDefault()));           // N/A

    // Integer default
    var dm2 = DefaultMap.New(Box.I64(0));
    dm2.Set("count", Box.I64(42));
    SayInt(Box.ToI64(dm2.Get("count")));       // 42
    SayInt(Box.ToI64(dm2.Get("missing")));     // 0
}
```

### BASIC Example

```basic
' Create with string default
DIM dm AS OBJECT
dm = Zanna.Collections.DefaultMap.New(Zanna.Core.Box.Str("N/A"))

dm.Set("name", "Alice")
dm.Set("city", "Boston")
PRINT dm.Count                  ' 2

' Existing key returns stored value
PRINT dm.Get("name")          ' Alice

' Missing key returns default (does not insert)
PRINT dm.Get("email")         ' N/A
PRINT dm.Has("email")         ' 0

' Get the default value
PRINT dm.GetDefault()         ' N/A

' Remove a key
PRINT dm.Remove("city")       ' 1
PRINT dm.Get("city")          ' N/A (returns default now)

' Integer default
DIM dm2 AS OBJECT
dm2 = Zanna.Collections.DefaultMap.New(Zanna.Core.Box.I64(0))
dm2.Set("count", Zanna.Core.Box.I64(42))
PRINT Zanna.Core.Box.ToI64(dm2.Get("count"))     ' 42
PRINT Zanna.Core.Box.ToI64(dm2.Get("missing"))   ' 0

' Clear all
dm.Clear()
PRINT dm.Count                  ' 0
PRINT dm.Get("name")          ' N/A
```

### Use Cases

- **Configuration with fallbacks:** Access settings with sensible defaults for missing keys
- **Sparse data:** Represent data where most entries have the same default value
- **Counter initialization:** Use integer default of 0 to avoid checking for key existence
- **Template rendering:** Default to placeholder text for missing template variables

---

## Zanna.Collections.LruCache

A fixed-capacity cache that evicts the least recently used (LRU) entry when full. Supports O(1) get, put, and
eviction operations. Values are accessed by string keys.

**Type:** Instance (obj)
**Constructor:** `Zanna.Collections.LruCache.New(capacity)` - creates a cache with the given maximum capacity.
Use `capacity = 0` for an unbounded cache that keeps LRU ordering but does not evict automatically.
Negative capacities trap.

### Properties

| Property  | Type    | Description                               |
|-----------|---------|-------------------------------------------|
| `Capacity` | Integer | Maximum capacity (fixed at creation)    |
| `IsEmpty` | Boolean | True if the cache has no entries          |
| `Count`      | Integer | Number of entries currently in the cache  |

### Methods

| Method              | Signature              | Description                                                            |
|---------------------|------------------------|------------------------------------------------------------------------|
| `Set(key, value)`   | `Void(String, Object)` | Add or update an entry; evicts LRU entry if at capacity                |
| `Get(key)`          | `Object(String)`       | Borrow value and promote to most recently used; null if not found     |
| `Has(key)`          | `Boolean(String)`      | Check if key exists in the cache                                       |
| `Peek(key)`         | `Object(String)`       | Borrow value without promoting (does not affect LRU order)             |
| `Remove(key)`       | `Boolean(String)`      | Remove an entry; returns true if found                                 |
| `RemoveOldest()`    | `Boolean()`            | Remove the least recently used entry; returns true if cache was non-empty |
| `Keys()`            | `Seq()`                | Get all keys as a Seq (MRU to LRU order)                              |
| `Values()`          | `Seq()`                | Get all values as a Seq (MRU to LRU order)                            |
| `Clear()`           | `Void()`               | Remove all entries (capacity is preserved)                             |

### Notes

- `Get` promotes the accessed entry to most recently used; `Peek` does not
- `Has` also leaves recency unchanged.
- When `Set` is called at capacity, the least recently used entry is automatically evicted
- A capacity of `0` disables automatic eviction; entries remain until removed or cleared
- Updating an existing key with `Set` promotes it to most recently used without eviction
- String keys are compared by full byte length; embedded NUL bytes are part of the key
- Cached values are retained while stored and released on overwrite, eviction, remove, clear, or finalization
- `Get()` and `Peek()` return borrowed references. A present null value is distinguishable from a
  miss with `Has()`.
- `Keys()` and `Values()` return retained snapshots in matching MRU-to-LRU order. Keys are copied;
  values are shared, not deep-cloned.
- Values are boxed objects in Zia (use `Zanna.Core.Box`); BASIC auto-boxes string values
- `Put` remains available as a compatibility alias for `Set`.
- A null runtime key is treated as the empty string key. The cache is not thread-safe.

### Zia Example

```zia
module LruCacheDemo;

bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var cache = LruCache.New(3);
    SayInt(cache.Capacity);                        // 3

    // Add entries
    cache.Set("a", Box.Str("alpha"));
    cache.Set("b", Box.Str("beta"));
    cache.Set("c", Box.Str("gamma"));
    SayInt(cache.Count);                             // 3

    // Get promotes to MRU
    Say(Box.ToStr(cache.Get("a")));                // alpha
    Say(Box.ToStr(cache.Get("b")));                // beta

    // Peek does not promote
    Say(Box.ToStr(cache.Peek("c")));               // gamma

    // Adding a 4th entry evicts LRU (c, since Peek didn't promote it)
    cache.Set("d", Box.Str("delta"));
    SayBool(cache.Has("c"));                       // 0 (evicted)
    SayBool(cache.Has("d"));                       // 1

    // Remove specific entry
    cache.Remove("b");
    SayInt(cache.Count);                             // 2

    // Clear all
    cache.Clear();
    SayBool(cache.IsEmpty);                        // 1
    SayInt(cache.Capacity);                        // 3 (capacity preserved)
}
```

### BASIC Example

```basic
DIM cache AS OBJECT
cache = Zanna.Collections.LruCache.New(3)
PRINT cache.Capacity     ' 3

' Add entries
cache.Set("a", "alpha")
cache.Set("b", "beta")
cache.Set("c", "gamma")
PRINT cache.Count          ' 3

' Get promotes to most recently used
PRINT cache.Get("a")     ' alpha
PRINT cache.Get("b")     ' beta

' Peek does NOT promote
PRINT cache.Peek("c")    ' gamma

' Adding when full evicts LRU (c was not promoted by Peek)
cache.Set("d", "delta")
PRINT cache.Count          ' 3 (still at capacity)
PRINT cache.Has("c")     ' 0 (evicted)
PRINT cache.Has("d")     ' 1

' Update existing entry (no eviction)
cache.Set("a", "ALPHA")
PRINT cache.Get("a")     ' ALPHA
PRINT cache.Count          ' 3

' Remove specific entry
PRINT cache.Remove("b")  ' 1
PRINT cache.Count          ' 2

' Remove oldest (LRU) entry
cache.Set("e", "epsilon")
PRINT cache.RemoveOldest() ' 1
PRINT cache.Count          ' 2

' Clear all
cache.Clear()
PRINT cache.IsEmpty      ' 1
PRINT cache.Capacity     ' 3
```

### Use Cases

- **Database query caching:** Cache recent query results with automatic eviction
- **Web page caching:** Keep recently accessed pages in memory
- **DNS caching:** Cache recent DNS lookups with bounded memory
- **Session management:** Track active sessions with automatic expiration of idle ones
- **Memoization:** Cache function results with bounded memory usage

---

## Zanna.Collections.WeakMap

A map with weak value references. Values may become NULL when their referent is garbage collected. Uses string keys. Useful for caches and observer patterns where you don't want to prevent collection of values.

**Type:** Instance (obj)
**Constructor:** `Zanna.Collections.WeakMap.New()`

### Properties

| Property  | Type    | Description                                              |
|-----------|---------|----------------------------------------------------------|
| `Count`      | Integer | Number of entries whose weak values are still live       |
| `IsEmpty` | Boolean | True if map has no live entries                          |

### Methods

| Method           | Signature              | Description                                                |
|------------------|------------------------|------------------------------------------------------------|
| `Set(key, value)`| `Void(String, Object)` | Set a value (stored as weak reference)                     |
| `Get(key)`       | `Object(String)`       | Get a retained live value for key (NULL if not found or collected) |
| `Has(key)`       | `Boolean(String)`      | Check if key exists and its weak value is still live       |
| `Remove(key)`    | `Boolean(String)`      | Remove entry; returns true if found                        |
| `Keys()`         | `Seq()`                | Get all keys whose weak values are still live              |
| `Clear()`        | `Void()`               | Remove all entries                                         |
| `Compact()`      | `Integer()`            | Remove entries with collected values; returns count removed |

### Notes

- **Weak references:** Values are stored without preventing garbage collection. If the only reference to an object is through a WeakMap, it may be collected.
- **Stale entries:** After a value is collected, `Get()` returns NULL for that key. A live result is promoted to a retained strong reference for the caller. Use `Compact()` to clean up stale entries.
- **String keys:** Keys are regular strong string references and are compared by full byte length; embedded NUL bytes are part of the key.
- **Live views:** `Count`, `IsEmpty`, `Has`, and `Keys` expose live weak values only. `Compact()` removes the stale internal slots.
- **Snapshots:** `Keys()` returns an owning snapshot in unspecified hash-slot order.
- **Runtime values:** Non-null values must be live runtime-managed objects or strings; another
  pointer traps when the weak reference is created. Setting null creates a non-live entry that is
  omitted from the live views and removable with `Compact()`.
- **Concurrency:** WeakMap is not thread-safe; synchronize externally around concurrent access.

### Zia Example

```zia
module WeakMapDemo;

bind Zanna.Collections.WeakMap as WeakMap;
bind Zanna.Core.Box as Box;
bind Zanna.Terminal;

func start() {
    var cache: WeakMap = WeakMap.New();
    var value = Box.Str("cached value");
    cache.Set("key", value);
    SayBool(cache.Has("key"));
    SayInt(cache.Count);
    cache.Remove("key");
    SayBool(cache.IsEmpty);
}
```

### BASIC Example

```basic
' Create a weak map for caching
DIM cache AS Zanna.Collections.WeakMap = Zanna.Collections.WeakMap.New()

' Keep strong local references while these values are in use.
DIM first AS OBJECT = Zanna.Core.Box.Str("cached one")
DIM second AS OBJECT = Zanna.Core.Box.Str("cached two")
cache.Set("key1", first)
cache.Set("key2", second)

PRINT cache.Count      ' Output: 2
PRINT cache.Has("key1")  ' Output: 1 (true)

' Get value (may be NULL if collected)
DIM value AS OBJECT = cache.Get("key1")
IF NOT Zanna.Core.Object.RefEquals(value, NOTHING) THEN
    PRINT "Cache hit"
ELSE
    PRINT "Cache miss - object was collected"
END IF

' Remove specific entry
cache.Remove("key2")

' Clean up stale entries
DIM removed AS INTEGER = cache.Compact()
PRINT "Compacted "; removed; " stale entries"

' Get all current keys
DIM keys AS Zanna.Collections.Seq = cache.Keys()
FOR i = 0 TO keys.Count - 1
    PRINT Zanna.Collections.Seq.GetStr(keys, i)
NEXT

' Clear everything
cache.Clear()
```

### WeakMap vs Map

| Feature           | WeakMap                    | Map                       |
|-------------------|----------------------------|---------------------------|
| Value references  | Weak (may be collected)    | Strong (prevents collection) |
| Memory management | Values can be GC'd         | Values kept alive          |
| Stale entries     | Possible (use Compact)     | Never                      |
| Use case          | Caches, observers          | General key-value storage  |

### Use Cases

- **Object caches:** Cache computed results without preventing GC of the source objects
- **Observer patterns:** Track observers without preventing their collection
- **Metadata storage:** Associate metadata with objects without extending their lifetime
- **Memoization:** Cache function results that can be recomputed if evicted

---

## Zanna.Collections.SparseArray

An array-like data structure that efficiently stores values at arbitrary integer indices without allocating memory
for gaps. Only occupied indices consume storage.

**Type:** Instance (obj)
**Constructor:** `Zanna.Collections.SparseArray.New()`

### Properties

| Property | Type    | Description                                  |
|----------|---------|----------------------------------------------|
| `Count`     | Integer | Number of elements stored (occupied indices) |

### Methods

| Method             | Signature               | Description                                            |
|--------------------|-------------------------|--------------------------------------------------------|
| `Set(index, value)`| `Void(Integer, Object)` | Set value at the given index                           |
| `Get(index)`       | `Object(Integer)`       | Get the borrowed value (null if not set)               |
| `Has(index)`       | `Boolean(Integer)`      | Check if an index has a value                          |
| `Remove(index)`    | `Boolean(Integer)`      | Remove value at index; returns true if found           |
| `Indices()`        | `Seq()`                 | Get all occupied indices as boxed i64 values           |
| `Values()`         | `Seq()`                 | Get all values as a Seq                                |
| `Clear()`          | `Void()`                | Remove all entries                                     |

### Notes

- Indices can be any integer, including negative numbers (e.g., -5, 0, 1000)
- Only occupied indices consume memory; gaps between indices cost nothing
- Setting an index to null removes that entry, matching `Remove(index)`
- `Indices()` and `Values()` return retained snapshots in matching, unspecified hash-slot order.
  Indices are boxed `i64` values; corresponding indices identify the same entry.
- `Get()` returns a borrowed value. Null cannot be stored because it removes the entry.
- Stored values are retained until overwritten, removed, cleared, or finalized.
- Values are boxed objects in Zia (use `Zanna.Core.Box`); BASIC auto-boxes string values
- The array is not thread-safe; synchronize externally around concurrent access.

### Zia Example

```zia
module SparseArrayDemo;

bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var sa = SparseArray.New();

    // Set values at sparse indices
    sa.Set(0, Box.Str("zero"));
    sa.Set(100, Box.Str("hundred"));
    sa.Set(1000, Box.Str("thousand"));
    sa.Set(-5, Box.Str("negative"));
    SayInt(sa.Count);                               // 4

    // Retrieve values
    Say(Box.ToStr(sa.Get(0)));                    // zero
    Say(Box.ToStr(sa.Get(100)));                  // hundred
    Say(Box.ToStr(sa.Get(-5)));                   // negative

    // Check existence
    SayBool(sa.Has(100));                         // 1
    SayBool(sa.Has(50));                          // 0

    // Update existing index
    sa.Set(100, Box.Str("HUNDRED"));
    Say(Box.ToStr(sa.Get(100)));                  // HUNDRED
    SayInt(sa.Count);                               // 4 (no new entry)

    // Remove
    sa.Remove(1000);
    SayInt(sa.Count);                               // 3

    // Setting null also removes an entry
    sa.Set(100, null);
    SayBool(sa.Has(100));                         // 0

    // Get all indices and values
    var indices = sa.Indices();
    SayInt(indices.Count);                          // 2
}
```

### BASIC Example

```basic
DIM sa AS OBJECT
sa = Zanna.Collections.SparseArray.New()

' Set values at sparse indices
sa.Set(0, "zero")
sa.Set(100, "hundred")
sa.Set(1000, "thousand")
sa.Set(-5, "negative")
PRINT sa.Count             ' 4

' Retrieve values
PRINT sa.Get(0)          ' zero
PRINT sa.Get(100)        ' hundred
PRINT sa.Get(1000)       ' thousand
PRINT sa.Get(-5)         ' negative

' Check existence
PRINT sa.Has(100)        ' 1
PRINT sa.Has(50)         ' 0

' Update existing
sa.Set(100, "HUNDRED")
PRINT sa.Get(100)        ' HUNDRED
PRINT sa.Count             ' 4

' Remove
PRINT sa.Remove(1000)    ' 1
PRINT sa.Has(1000)       ' 0
PRINT sa.Count             ' 3

' Setting NULL also removes an entry
sa.Set(100, NOTHING)
PRINT sa.Has(100)        ' 0
PRINT sa.Count             ' 2

' Get all indices and values
DIM indices AS OBJECT
indices = sa.Indices()
PRINT indices.Count        ' 2

DIM vals AS OBJECT
vals = sa.Values()
PRINT vals.Count           ' 2

' Clear all
sa.Clear()
PRINT sa.Count             ' 0
```

### Use Cases

- **Game maps:** Store tile data at arbitrary 2D coordinates (using computed indices)
- **Sparse matrices:** Represent matrices where most entries are zero
- **Event scheduling:** Map time slots to events without allocating empty slots
- **Lookup tables:** Map non-contiguous IDs to objects efficiently
- **Dynamic arrays:** Store data at arbitrary positions without pre-allocation

---


## See Also

- [Sequential Collections](sequential.md)
- [Maps & Sets](maps-sets.md)
- [Functional & Lazy](functional.md)
- [Specialized Structures](specialized.md)
- [Collections Overview](README.md)
- [Zanna Runtime Library](../README.md)
