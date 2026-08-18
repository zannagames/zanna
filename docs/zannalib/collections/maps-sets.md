---
status: active
audience: public
last-verified: 2026-08-17
---

# Maps & Sets
> Map, Set, OrderedMap, SortedSet, FrozenMap, FrozenSet, SortedMap

**Part of [Zanna Runtime Library](../README.md) › [Collections](README.md)**

---

## Zanna.Collections.Map

A key-value dictionary with string keys. Provides O(1) average-case lookup, insertion, and deletion.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Collections.Map()`

### Properties

| Property  | Type    | Description                            |
|-----------|---------|----------------------------------------|
| `Count`   | Integer | Number of key-value pairs in the map   |
| `IsEmpty` | Boolean | Returns true if the map has no entries |

### Methods

| Method                     | Signature                 | Description                                                              |
|----------------------------|---------------------------|--------------------------------------------------------------------------|
| `Set(key, value)`          | `Void(String, Object)`    | Add or update key-value pair                                             |
| `Get(key)`                 | `Object(String)`          | Get the borrowed value for a key (NULL if missing)                       |
| `GetOr(key, defaultValue)` | `Object(String, Object)`  | Get the borrowed value, or `defaultValue` if missing (does not insert)   |
| `Has(key)`                 | `Boolean(String)`         | Check if key exists                                                      |
| `SetIfMissing(key, value)` | `Boolean(String, Object)` | Insert key-value pair only when missing; returns true if inserted        |
| `Remove(key)`              | `Boolean(String)`         | Remove key-value pair; returns true if found                             |
| `Clear()`                  | `Void()`                  | Remove all entries                                                       |
| `Trim()`                   | `Boolean()`               | Return excess bucket capacity; true if already minimal or trim succeeds |
| `Keys()`                   | `Seq()`                   | Get sequence of all keys                                                 |
| `Values()`                 | `Seq()`                   | Get sequence of all values                                               |
| `Clone()`                  | `Map()`                   | Create a shallow copy of the map                                         |

### Typed Accessors

Convenience methods for storing and retrieving typed values without manual boxing/unboxing.

| Method                          | Signature                    | Description                                                       |
|---------------------------------|------------------------------|-------------------------------------------------------------------|
| `SetInt(key, value)`            | `Void(String, Integer)`      | Store an integer value                                            |
| `GetInt(key)`                   | `Integer(String)`            | Get/coerce a numeric value (0 if missing; traps if incompatible)  |
| `GetIntOr(key, default)`        | `Integer(String, Integer)`   | Get/coerce a numeric value, or `default` if missing/incompatible  |
| `SetFloat(key, value)`          | `Void(String, Number)`       | Store a floating-point value                                      |
| `GetFloat(key)`                 | `Number(String)`             | Get/coerce a numeric value (0.0 if missing; traps if incompatible)|
| `GetFloatOr(key, default)`      | `Number(String, Number)`     | Get/coerce a numeric value, or `default` if missing/incompatible  |
| `SetBool(key, value)`           | `Void(String, Boolean)`      | Store a boolean value                                             |
| `GetBool(key)`                  | `Boolean(String)`            | Get/coerce a numeric/boolean value (false if missing)             |
| `GetBoolOr(key, default)`       | `Boolean(String, Boolean)`   | Get/coerce a value, or `default` if missing/incompatible          |
| `SetStr(key, value)`            | `Void(String, String)`       | Store a string value                                              |
| `GetStr(key)`                   | `String(String)`             | Get a string (empty if missing; traps if incompatible)            |

### Notes

- String keys are compared by full byte length; embedded NUL bytes are part of the key.
- Values are retained while stored and released when overwritten, removed, cleared, or finalized.
- `Get()` and `GetOr()` return borrowed references. A present key may store NULL: in that case
  `Has()` is true and both getters return NULL rather than substituting the default.
- `Keys()` and `Values()` return independent snapshots. Key snapshots own copied strings; value snapshots retain the object values so they remain valid after the source map is cleared.
- `Keys()` and `Values()` use the same hash-table order, not insertion order; corresponding indices
  identify the same entry, but that order is otherwise unspecified.
- Integer, float, and boolean getters coerce among boxed integers, floats, and booleans. The plain
  getters trap on another stored type; their `Or` variants return the supplied default instead.
- Map growth and key-copy allocation paths trap on overflow instead of wrapping.
- Entries cache their hash and store node metadata with the copied key in one
  allocation. Growth allocates the complete replacement bucket array before
  relinking entries, so allocation failure preserves the old capacity, keys,
  values, and count.
- `Clear()` retains bucket capacity for fast reuse. `Trim()` explicitly reduces
  capacity to the smallest supported power-of-two table that keeps the current
  count below the normal 75-percent growth threshold. A failed trim traps,
  returns false to a returning trap hook, and leaves the map unchanged.

### Zia Example

```zia
module MapDemo;

bind Zanna.Terminal;
bind Zanna.Collections;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var scores = new Map[String, Integer]();

    // Add entries
    scores.set("Alice", 95);
    scores.set("Bob", 87);
    scores.set("Carol", 92);

    // Check and retrieve
    Say("Has Alice: " + Fmt.Bool(scores.has("Alice")));  // true
    Say("Alice: " + Fmt.Int(scores.get("Alice") ?? 0));  // 95

    // Update
    scores.set("Bob", 91);
    Say("Bob updated: " + Fmt.Int(scores.get("Bob") ?? 0)); // 91

    // Remove
    scores.remove("Carol");
    Say("Has Carol: " + Fmt.Bool(scores.has("Carol")));  // false
}
```

### BASIC Example

```basic
DIM scores AS Zanna.Collections.Map
scores = NEW Zanna.Collections.Map()

' Add typed entries
scores.SetInt("Alice", 95)
scores.SetInt("Bob", 87)
scores.SetInt("Carol", 92)

PRINT scores.Count      ' Output: 3
PRINT scores.IsEmpty  ' Output: False

' Check existence and get value
IF scores.Has("Alice") THEN
    PRINT "Alice's score: "; scores.GetInt("Alice")
END IF

' Get-or-default without inserting
PRINT scores.GetIntOr("Dave", 0)   ' Output: 0 (and "Dave" is still missing)

' Insert only if missing
IF scores.SetIfMissing("Bob", Zanna.Core.Box.I64(123)) THEN
    PRINT "Inserted Bob"
ELSE
    PRINT "Bob already exists"
END IF

' Update existing entry
scores.SetInt("Bob", 91)

' Remove an entry
IF scores.Remove("Carol") THEN
    PRINT "Removed Carol"
END IF

' Iterate over keys
DIM names AS Zanna.Collections.Seq
names = scores.Keys()
FOR i = 0 TO names.Count - 1
    PRINT Zanna.Collections.Seq.GetStr(names, i)
NEXT i

' Clear all
scores.Clear()
PRINT scores.IsEmpty  ' Output: True
```

### Use Cases

- **Configuration storage:** Store key-value settings
- **Caching:** Cache computed values by key
- **Lookup tables:** Map identifiers to objects
- **Counting:** Count occurrences by key

---

## Zanna.Collections.Set

A generic set data structure for storing unique objects. Efficiently handles membership testing, set operations (union,
intersection, difference), and subset/superset queries. Unlike `StringSet` which stores strings, `Set` stores arbitrary objects.

**Type:** Instance (obj)
**Constructor:** `Zanna.Collections.Set.New()`

### Properties

| Property  | Type    | Description                       |
|-----------|---------|-----------------------------------|
| `Count`   | Integer | Number of objects in the set      |
| `IsEmpty` | Boolean | True if set contains no objects   |

### Methods

| Method              | Signature         | Description                                                    |
|---------------------|-------------------|----------------------------------------------------------------|
| `Add(obj)`          | `Boolean(Object)` | Add an object; returns true if new, false if already present   |
| `Remove(obj)`       | `Boolean(Object)` | Remove an object; returns true if removed, false if not found  |
| `Has(obj)`          | `Boolean(Object)` | Check if object is in the set                                  |
| `Clear()`           | `Void()`          | Remove all objects from the set                                |
| `ToSeq()`           | `Seq()`           | Get all objects as a Seq (order undefined)                     |
| `Union(other)`      | `Set(Set)`        | Return new set with union of both sets                         |
| `Intersect(other)`  | `Set(Set)`        | Return new set with intersection of both sets                  |
| `Difference(other)`       | `Set(Set)`        | Return new set with elements in this but not other             |
| `IsSubset(other)`   | `Boolean(Set)`    | True if this set is a subset of other                          |
| `IsSuperset(other)` | `Boolean(Set)`    | True if this set is a superset of other                        |
| `IsDisjoint(other)` | `Boolean(Set)`    | True if sets have no elements in common                        |
| `Clone()`           | `Set()`           | Create a shallow copy of the set                               |
| `ToSeq()`           | `Seq()`           | Returns all elements as a new Seq (order undefined)            |
| `ToList()`          | `List()`          | Returns all elements as a new List (order undefined)           |

### Notes

- Separately boxed integers, booleans, floats, and strings compare by boxed value. Other objects
  compare by reference identity. Boxed values of different tags remain distinct (for example,
  boxed integer `1` and boxed boolean `true`).
- Order of objects returned by `ToSeq()` is not guaranteed (hash-table order).
- Elements are retained while stored. `ToSeq()`, `ToSeq()`, and `ToList()` return retained
  snapshots; elements remain valid after the source set is cleared.
- Set operations (`Union`, `Intersect`, `Diff`) return new sets; originals are unchanged.
- Hashing follows the same rule as equality: boxed scalars hash by value and other objects by
  identity. Membership operations are O(1) average-case and O(n) worst-case.
- The table automatically resizes when its load factor reaches the threshold.

### Zia Example

```zia
module SetDemo;

bind Zanna.Collections.Set as Set;
bind Zanna.Core.Box as Box;
bind Zanna.Terminal;

func start() {
    var items: Set = Set.New();
    var one = Box.I64(1);
    var anotherOne = Box.I64(1);
    var two = Box.I64(2);

    SayBool(items.Add(one));       // true
    SayBool(items.Add(anotherOne)); // false: equal boxed value, despite a different box
    SayBool(items.Add(two));       // true
    SayBool(items.Has(Box.I64(1))); // true: boxed-value lookup
    SayInt(items.Count);           // 2
}
```

### BASIC Example

```basic
' Create and populate a set
DIM items AS OBJECT = Zanna.Collections.Set.New()
DIM a AS OBJECT = Zanna.Core.Box.I64(1)
DIM b AS OBJECT = Zanna.Core.Box.I64(2)
DIM c AS OBJECT = Zanna.Core.Box.I64(3)

items.Add(a)
items.Add(b)
items.Add(c)
PRINT items.Count           ' Output: 3

' A separately boxed equal scalar is also a duplicate
DIM anotherA AS OBJECT = Zanna.Core.Box.I64(1)
DIM wasNew AS INTEGER = items.Add(anotherA)
PRINT wasNew              ' Output: 0 (already present)

' Membership testing
PRINT items.Has(b)        ' Output: 1 (true)
DIM d AS OBJECT = Zanna.Core.Box.I64(4)
PRINT items.Has(d)        ' Output: 0 (false)

' Remove an element
DIM removed AS INTEGER = items.Remove(b)
PRINT removed             ' Output: 1 (was removed)
PRINT items.Has(b)        ' Output: 0 (no longer present)

' Set operations
DIM setA AS OBJECT = Zanna.Collections.Set.New()
DIM x AS OBJECT = Zanna.Core.Box.Str("x")
DIM y AS OBJECT = Zanna.Core.Box.Str("y")
DIM z AS OBJECT = Zanna.Core.Box.Str("z")
DIM w AS OBJECT = Zanna.Core.Box.Str("w")
setA.Add(x)
setA.Add(y)
setA.Add(z)

DIM setB AS OBJECT = Zanna.Collections.Set.New()
setB.Add(y)
setB.Add(z)
setB.Add(w)

' Union: elements in either set
DIM merged AS OBJECT = setA.Union(setB)
PRINT merged.Count          ' Output: 4 (x, y, z, w)

' Intersection: elements in both sets
DIM common AS OBJECT = setA.Intersect(setB)
PRINT common.Count          ' Output: 2 (y, z)

' Difference: elements in A but not B
DIM diff AS OBJECT = setA.Difference(setB)
PRINT diff.Count            ' Output: 1 (x only)

' Subset/superset checks
DIM subset AS OBJECT = Zanna.Collections.Set.New()
subset.Add(y)
subset.Add(z)
PRINT subset.IsSubset(setA)     ' Output: 1 (true)
PRINT setA.IsSuperset(subset)   ' Output: 1 (true)

' Disjoint check
DIM disjoint AS OBJECT = Zanna.Collections.Set.New()
disjoint.Add(w)
PRINT setA.IsDisjoint(disjoint) ' Output: 1 (true - no common elements)
```

### Set vs StringSet

| Feature          | Set                        | StringSet                     |
|------------------|----------------------------|-------------------------|
| Element type     | Any object                 | Strings only            |
| Comparison       | Boxed-scalar value; otherwise identity | String value   |
| Subset/Superset  | Yes                        | No                      |
| Disjoint check   | Yes                        | No                      |

### Use Cases

- **Value/object deduplication:** Deduplicate boxed scalars by value or other objects by identity
- **Graph algorithms:** Track visited nodes
- **Relationship modeling:** Many-to-many relationships
- **Set mathematics:** Compute unions, intersections, and differences
- **Access control:** Track permissions or capabilities

---

## Zanna.Collections.OrderedMap

A key-value map that maintains insertion order. Keys are iterated in the order they were first inserted,
regardless of updates.

**Type:** Instance (obj)
**Constructor:** `Zanna.Collections.OrderedMap.New()`

### Properties

| Property  | Type    | Description                            |
|-----------|---------|----------------------------------------|
| `IsEmpty` | Boolean | True if the map has no entries         |
| `Count`   | Integer | Number of key-value pairs in the map   |

### Methods

| Method            | Signature              | Description                                                       |
|-------------------|------------------------|-------------------------------------------------------------------|
| `Set(key, value)` | `Void(String, Object)` | Add or update a key-value pair (preserves original insertion order)|
| `Get(key)`        | `Object(String)`       | Get the borrowed value for a key (null if not found)              |
| `Has(key)`        | `Boolean(String)`      | Check if key exists                                               |
| `KeyAt(index)`    | `String(Integer)`      | Get the key at the given position in insertion order              |
| `Keys()`          | `Seq()`                | Get all keys in insertion order                                   |
| `Values()`        | `Seq()`                | Get all values in insertion order                                 |
| `Remove(key)`     | `Boolean(String)`      | Remove a key-value pair; true if found                            |
| `Clear()`         | `Void()`               | Remove all entries                                                |

### Notes

- Updating an existing key's value does *not* change its position in the insertion order
- `KeyAt` walks the insertion-order list and is O(index), hence O(n) in the worst case. It returns
  null for a negative or out-of-range index.
- After removing a key, subsequent keys shift down in their positional indices
- String keys are compared by full byte length; embedded NUL bytes are part of the key
- Passing a null key through the runtime API is treated as the empty string key
- Values are retained while stored and released when overwritten, removed, cleared, or finalized
- `Get()` returns a borrowed value. Use `Has()` to distinguish a missing key from a present null.
- `KeyAt()` returns an owned copied string. `Keys()` and `Values()` return independent snapshots
  in insertion order; value snapshots retain object values.
- Values are boxed objects in Zia (use `Zanna.Core.Box`); BASIC auto-boxes string values

### Zia Example

```zia
module OrderedMapDemo;

bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var om = OrderedMap.New();

    // Insert in specific order
    om.Set("first", Box.Str("alpha"));
    om.Set("second", Box.Str("beta"));
    om.Set("third", Box.Str("gamma"));
    SayInt(om.Count);                             // 3

    // Access by key
    Say(Box.ToStr(om.Get("first")));            // alpha

    // Access by insertion position
    Say(om.KeyAt(0));                           // first
    Say(om.KeyAt(1));                           // second
    Say(om.KeyAt(2));                           // third

    // Update preserves insertion order
    om.Set("second", Box.Str("BETA"));
    Say(om.KeyAt(1));                           // second (still position 1)
    Say(Box.ToStr(om.Get("second")));           // BETA

    // Keys and values in insertion order
    var keys = om.Keys();
    Say(Box.ToStr(Seq.Get(keys, 0)));           // first
    Say(Box.ToStr(Seq.Get(keys, 1)));           // second
    Say(Box.ToStr(Seq.Get(keys, 2)));           // third
}
```

### BASIC Example

```basic
DIM om AS OBJECT
om = Zanna.Collections.OrderedMap.New()

' Insert in specific order
om.Set("first", "alpha")
om.Set("second", "beta")
om.Set("third", "gamma")
PRINT om.Count              ' 3

' Access by key
PRINT om.Get("first")     ' alpha
PRINT om.Get("second")    ' beta

' Access by insertion position
PRINT om.KeyAt(0)         ' first
PRINT om.KeyAt(1)         ' second
PRINT om.KeyAt(2)         ' third

' Update preserves order
om.Set("second", "BETA")
PRINT om.Get("second")    ' BETA
PRINT om.KeyAt(1)         ' second (still position 1)

' Keys in insertion order
DIM keys AS OBJECT
keys = om.Keys()
PRINT keys.Count            ' 3
PRINT keys.Get(0)         ' first
PRINT keys.Get(1)         ' second
PRINT keys.Get(2)         ' third

' Values in insertion order
DIM vals AS OBJECT
vals = om.Values()
PRINT vals.Get(0)         ' alpha
PRINT vals.Get(1)         ' BETA
PRINT vals.Get(2)         ' gamma

' Remove
PRINT om.Remove("second") ' 1
PRINT om.Count              ' 2
DIM keys2 AS OBJECT
keys2 = om.Keys()
PRINT keys2.Get(0)        ' first
PRINT keys2.Get(1)        ' third

' Clear
om.Clear()
PRINT om.IsEmpty          ' 1
```

### OrderedMap vs Map vs SortedMap

| Feature           | OrderedMap       | Map              | SortedMap          |
|-------------------|------------------|------------------|------------------|
| Key order         | Insertion order  | Unordered        | Sorted order     |
| Lookup            | O(1) average     | O(1) average     | O(log n)         |
| Insert            | O(1) average     | O(1) average     | O(n)             |
| Positional access | O(n) via KeyAt   | Not available    | Not available    |

### Use Cases

- **JSON-like serialization:** Preserve key order for deterministic output
- **Configuration files:** Maintain the order of settings as authored
- **Form processing:** Process form fields in submission order
- **Audit trails:** Track entries in the order they were created

---

## Zanna.Collections.SortedSet

A sorted set of unique strings maintained in sorted order. Unlike `StringSet` which uses hash-based storage, `SortedSet`
keeps elements sorted, enabling efficient range queries, ordered iteration, and floor/ceiling operations.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Collections.SortedSet()`

### Properties

| Property  | Type    | Description                             |
|-----------|---------|-----------------------------------------|
| `Count`   | Integer | Number of strings in the set            |
| `IsEmpty` | Boolean | True if set contains no strings         |

### Methods

| Method              | Signature                  | Description                                                        |
|---------------------|----------------------------|--------------------------------------------------------------------|
| `Add(str)`          | `Boolean(String)`          | Add a string; returns true if new, false if already present        |
| `Remove(str)`       | `Boolean(String)`          | Remove a string; returns true if removed, false if not found       |
| `Has(str)`          | `Boolean(String)`          | Check if string is in the set                                      |
| `Clear()`           | `Void()`                   | Remove all strings from the set                                    |
| `First()`           | `String()`                 | Get smallest (first) element; empty string if empty                |
| `Last()`            | `String()`                 | Get largest (last) element; empty string if empty                  |
| `Floor(str)`        | `String(String)`           | Greatest element <= given string; empty if none                    |
| `Ceiling(str)`         | `String(String)`           | Least element >= given string; empty if none                       |
| `Lower(str)`        | `String(String)`           | Greatest element < given string; empty if none                     |
| `Higher(str)`       | `String(String)`           | Least element > given string; empty if none                        |
| `At(index)`         | `String(Integer)`          | Get element at index; empty if out of range                        |
| `IndexOf(str)`      | `Integer(String)`          | Get index of element (-1 if not found)                             |
| `ToSeq()`           | `Seq()`                    | Get all elements as a Seq in sorted order                          |
| `Range(from, to)`   | `Seq(String, String)`      | Get elements in range [from, to]; null bounds are open-ended       |
| `Take(n)`           | `Seq(Integer)`             | Get first n elements                                               |
| `Skip(n)`           | `Seq(Integer)`             | Get all elements except first n                                    |
| `Union(other)`      | `SortedSet(SortedSet)`     | Return new set with union of both sets                             |
| `Intersect(other)`  | `SortedSet(SortedSet)`     | Return new set with intersection of both sets                      |
| `Difference(other)`       | `SortedSet(SortedSet)`     | Return new set with elements in this but not other                 |
| `IsSubset(other)`   | `Boolean(SortedSet)`       | True if this set is a subset of other                              |

### Notes

- Elements are compared by full byte length; embedded NUL bytes are part of element identity.
- `Range(from, to)` includes both bounds. Pass null for `from` or `to` through the runtime API to leave that side open.
- `First()`, `Last()`, `Floor()`, `Ceiling()`, `Lower()`, `Higher()`, and `At()` return owned copied strings.
- `ToSeq()`, `Range()`, `Take()`, and `Skip()` return independent snapshots containing copied strings.
- `Take(n)` returns empty for `n <= 0`; `Skip(n)` treats a negative `n` as zero and therefore
  returns all elements.
- The registry currently declares `Range()`, `Take()`, and `Skip()` as unqualified objects even
  though they return `Seq`. In Zia, assign these results to an explicitly typed `Seq` before using
  members such as `Count`; direct chaining can dispatch `SortedSet.Count` on the Seq and trap.
- Capacity growth traps on overflow instead of wrapping.

### Zia Example

```zia
module SortedSetDemo;

bind Zanna.Collections.SortedSet as SortedSet;
bind Zanna.Collections.Seq as Seq;
bind Zanna.Terminal;

func start() {
    var words: SortedSet = SortedSet.New();
    words.Add("cherry");
    words.Add("apple");
    words.Add("banana");

    Say(words.First());             // apple
    Say(words.Last());              // cherry
    var ordered: Seq = words.ToSeq();
    Say(Seq.GetStr(ordered, 1));    // banana
}
```

### BASIC Example

```basic
DIM words AS Zanna.Collections.SortedSet = NEW Zanna.Collections.SortedSet()

' Add words (stored in sorted order)
words.Add("cherry")
words.Add("apple")
words.Add("banana")
words.Add("date")

PRINT words.Count          ' Output: 4

' Ordered access
PRINT words.First()      ' Output: "apple"
PRINT words.Last()       ' Output: "date"
PRINT words.At(1)        ' Output: "banana" (second in sorted order)

' Find index
PRINT words.IndexOf("cherry")  ' Output: 2

' Range queries
PRINT words.Floor("cat")       ' Output: "banana" (largest <= "cat")
PRINT words.Ceiling("cat")        ' Output: "cherry" (smallest >= "cat")
PRINT words.Lower("cherry")    ' Output: "banana" (largest < "cherry")
PRINT words.Higher("cherry")   ' Output: "date" (smallest > "cherry")

' Get all items in sorted order
DIM all AS Zanna.Collections.Seq = words.ToSeq()
FOR i = 0 TO all.Count - 1
    PRINT Zanna.Collections.Seq.GetStr(all, i)  ' apple, banana, cherry, date
NEXT

' Get range [b, date], with both endpoints inclusive
DIM range AS Zanna.Collections.Seq = words.Range("b", "date")
FOR i = 0 TO range.Count - 1
    PRINT Zanna.Collections.Seq.GetStr(range, i)  ' banana, cherry, date
NEXT

' Set operations
DIM set1 AS Zanna.Collections.SortedSet = NEW Zanna.Collections.SortedSet()
set1.Add("a")
set1.Add("b")
set1.Add("c")

DIM set2 AS Zanna.Collections.SortedSet = NEW Zanna.Collections.SortedSet()
set2.Add("b")
set2.Add("c")
set2.Add("d")

' Union
DIM merged AS Zanna.Collections.SortedSet = set1.Union(set2)
PRINT merged.Count         ' Output: 4 (a, b, c, d)

' Intersection
DIM common AS Zanna.Collections.SortedSet = set1.Intersect(set2)
PRINT common.Count         ' Output: 2 (b, c)

' Difference
DIM diff AS Zanna.Collections.SortedSet = set1.Difference(set2)
PRINT diff.Count           ' Output: 1 (a only)

' Subset check
DIM subset AS OBJECT = NEW Zanna.Collections.SortedSet()
subset.Add("b")
subset.Add("c")
PRINT subset.IsSubset(set1)  ' Output: 1 (true)
```

### SortedSet vs StringSet vs Set

| Feature          | SortedSet        | StringSet              | Set              |
|------------------|------------------|------------------|------------------|
| Element type     | Strings          | Strings          | Objects          |
| Order            | Sorted           | Unordered        | Unordered        |
| Lookup           | O(log n)         | O(1) average     | O(1) average     |
| Insert           | O(n)             | O(1) average     | O(1) average     |
| First/Last       | O(1)             | No               | No               |
| Floor/Ceil       | O(log n)         | No               | No               |
| Range queries    | O(log n + k)     | No               | No               |
| Index access     | O(1)             | No               | No               |

### Use Cases

- **Autocomplete:** Find words in a range starting with a prefix
- **Leaderboards:** Maintain sorted rankings with efficient updates
- **Scheduling:** Find next available time slot with Floor/Ceil
- **Range queries:** Find all items in a lexicographic range
- **Ordered iteration:** When you need strings in sorted order
- **Nearest neighbor:** Find closest match using Floor/Ceil

---

## Zanna.Collections.FrozenMap

An immutable key-value map. Once created, entries cannot be added, removed, or modified. Supports efficient
lookup, merging (which returns a new FrozenMap), and equality comparison.

**Type:** Instance (obj)
**Constructors:**

- `Zanna.Collections.FrozenMap.FromSeqs(keys, values)` - Create from two parallel Seq objects
- `Zanna.Collections.FrozenMap.Empty()` - Create an empty immutable map

### Properties

| Property  | Type    | Description                            |
|-----------|---------|----------------------------------------|
| `IsEmpty` | Boolean | True if the map has no entries         |
| `Count`   | Integer | Number of key-value pairs in the map   |

### Methods

| Method                     | Signature                 | Description                                                       |
|----------------------------|---------------------------|-------------------------------------------------------------------|
| `Get(key)`                 | `Object(String)`          | Get the borrowed value for a key (null if missing)                |
| `GetOr(key, defaultValue)` | `Object(String, Object)`  | Get the borrowed value, or `defaultValue` if missing              |
| `Has(key)`                 | `Boolean(String)`         | Check if key exists                                               |
| `Keys()`                   | `Seq()`                   | Get all keys as a Seq                                             |
| `Values()`                 | `Seq()`                   | Get all values as a Seq                                           |
| `Merge(other)`             | `FrozenMap(FrozenMap)`    | Return new FrozenMap with entries from both; other's values win on conflict |
| `Equals(other)`            | `Boolean(FrozenMap)`      | Check if two maps have the same key-value pairs                   |

### Notes

- Keys in the `FromSeqs` constructor should be boxed strings (e.g., `Box.Str("key")`) in Zia;
  BASIC auto-boxes strings. Null keys are skipped and another non-string value traps while being
  unboxed.
- `FromSeqs` zips only through the shorter input. Repeated keys collapse, with the last paired
  value winning.
- FrozenMap is truly immutable: there are no `Set`, `Remove`, or `Clear` methods.
- `Get()` and `GetOr()` return borrowed references. A present null value remains null rather than
  being replaced by `GetOr`'s default; call `Has()` to distinguish it from a missing key.
- `Keys()` and `Values()` are retained snapshots in matching hash-slot order. The order is neither
  insertion order nor a stable cross-run order, but corresponding indices refer to the same entry.
- `Merge` returns a new FrozenMap; when both maps have the same key, the other map's value wins.
- `Equals` compares same-tag boxed scalar values by value and other objects by reference identity.
- Keys are compared by full byte length; embedded NUL bytes are part of the key.
- Keys and values are retained by the frozen map until the map is released.

### Zia Example

```zia
module FrozenMapDemo;

bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Build from parallel sequences
    var keys = Seq.New();
    keys.Push(Box.Str("name"));
    keys.Push(Box.Str("city"));

    var vals = Seq.New();
    vals.Push(Box.Str("Alice"));
    vals.Push(Box.Str("Boston"));

    var fm = FrozenMap.FromSeqs(keys, vals);
    SayInt(fm.Count);                                // 2
    Say(Box.ToStr(fm.Get("name")));                // Alice
    SayBool(fm.Has("name"));                       // 1
    Say(Box.ToStr(fm.GetOr("email", Box.Str("N/A")))); // N/A

    // Merge two frozen maps
    var keys2 = Seq.New();
    keys2.Push(Box.Str("city"));
    keys2.Push(Box.Str("email"));
    var vals2 = Seq.New();
    vals2.Push(Box.Str("NYC"));
    vals2.Push(Box.Str("a@b.com"));
    var fm2 = FrozenMap.FromSeqs(keys2, vals2);

    var merged = fm.Merge(fm2);
    SayInt(merged.Count);                            // 3
    Say(Box.ToStr(merged.Get("city")));            // NYC (fm2 wins)
}
```

### BASIC Example

```basic
' Create from parallel sequences
DIM keys AS Zanna.Collections.Seq
keys = Zanna.Collections.Seq.New()
keys.Push("name")
keys.Push("city")

DIM vals AS Zanna.Collections.Seq
vals = Zanna.Collections.Seq.New()
vals.Push("Alice")
vals.Push("Boston")

DIM fm AS OBJECT
fm = Zanna.Collections.FrozenMap.FromSeqs(keys, vals)
PRINT fm.Count                ' 2
PRINT fm.Get("name")        ' Alice
PRINT fm.Has("name")        ' 1

' GetOr for missing key
PRINT fm.GetOr("email", "N/A")  ' N/A

' Merge two frozen maps
DIM keys2 AS Zanna.Collections.Seq
keys2 = Zanna.Collections.Seq.New()
keys2.Push("city")
keys2.Push("email")
DIM vals2 AS Zanna.Collections.Seq
vals2 = Zanna.Collections.Seq.New()
vals2.Push("NYC")
vals2.Push("a@b.com")

DIM fm2 AS OBJECT
fm2 = Zanna.Collections.FrozenMap.FromSeqs(keys2, vals2)
DIM merged AS OBJECT
merged = fm.Merge(fm2)
PRINT merged.Count             ' 3
PRINT merged.Get("city")     ' NYC (fm2 wins)
PRINT merged.Get("name")     ' Alice (from fm)

' Equality
DIM keys3 AS Zanna.Collections.Seq
keys3 = Zanna.Collections.Seq.New()
keys3.Push("city")
keys3.Push("name")
DIM vals3 AS Zanna.Collections.Seq
vals3 = Zanna.Collections.Seq.New()
vals3.Push("Boston")
vals3.Push("Alice")
DIM fm3 AS OBJECT
fm3 = Zanna.Collections.FrozenMap.FromSeqs(keys3, vals3)
PRINT fm.Equals(fm3)         ' 1 (same key-value pairs)
PRINT fm.Equals(fm2)         ' 0
```

### Use Cases

- **Configuration snapshots:** Immutable configuration that cannot be accidentally modified
- **Thread-safe sharing:** Share data between threads without locking
- **Function return values:** Return read-only dictionaries from functions
- **Caching:** Store computed results as immutable maps

---

## Zanna.Collections.FrozenSet

An immutable set of unique strings. Once created, elements cannot be added or removed. Supports set operations
that return new FrozenSet instances.

**Type:** Instance (obj)
**Constructors:**

- `Zanna.Collections.FrozenSet.FromSeq(seq)` - Create from a Seq of boxed strings (duplicates are removed)
- `Zanna.Collections.FrozenSet.Empty()` - Create an empty immutable set

### Properties

| Property  | Type    | Description                             |
|-----------|---------|-----------------------------------------|
| `IsEmpty` | Boolean | True if the set has no elements         |
| `Count`   | Integer | Number of unique elements in the set    |

### Methods

| Method             | Signature              | Description                                                   |
|--------------------|------------------------|---------------------------------------------------------------|
| `Has(str)`         | `Boolean(String)`      | Check if string is in the set                                 |
| `ToSeq()`          | `Seq()`                | Get all elements as a Seq (order undefined)                   |
| `Union(other)`     | `FrozenSet(FrozenSet)` | Return new set with elements from either set                  |
| `Intersect(other)` | `FrozenSet(FrozenSet)` | Return new set with elements in both sets                     |
| `Difference(other)`      | `FrozenSet(FrozenSet)` | Return new set with elements in this but not other            |
| `IsSubset(other)`  | `Boolean(FrozenSet)`   | True if all elements of this set are in the other set         |
| `Equals(other)`    | `Boolean(FrozenSet)`   | True if both sets contain exactly the same elements           |

### Notes

- Elements in the `FromSeq` constructor should be boxed strings (e.g., `Box.Str("value")`) in
  Zia; BASIC auto-boxes. Null elements are skipped and another non-string value traps while being
  unboxed.
- Duplicate elements in the source Seq are automatically removed.
- Elements are retained by the frozen set. `ToSeq()` returns an independently retained snapshot
  in unspecified hash-slot order.
- All set operations return new FrozenSet instances; originals are unchanged.
- `Equals` compares strings by value regardless of insertion order.
- Elements are compared by full byte length; embedded NUL bytes are part of element identity.

### Zia Example

```zia
module FrozenSetDemo;

bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Build from a Seq (duplicates removed)
    var items = Seq.New();
    items.Push(Box.Str("apple"));
    items.Push(Box.Str("banana"));
    items.Push(Box.Str("cherry"));
    items.Push(Box.Str("apple"));  // duplicate

    var fs = FrozenSet.FromSeq(items);
    SayInt(fs.Count);                               // 3
    SayBool(fs.Has("apple"));                     // 1
    SayBool(fs.Has("grape"));                     // 0

    // Set operations
    var items2 = Seq.New();
    items2.Push(Box.Str("cherry"));
    items2.Push(Box.Str("date"));
    var fs2 = FrozenSet.FromSeq(items2);

    var united = fs.Union(fs2);
    SayInt(united.Count);                           // 4

    var inter = fs.Intersect(fs2);
    SayInt(inter.Count);                            // 1 (cherry)

    var diff = fs.Difference(fs2);
    SayInt(diff.Count);                             // 2 (apple, banana)
}
```

### BASIC Example

```basic
' Build from a Seq (duplicates removed)
DIM items AS Zanna.Collections.Seq
items = Zanna.Collections.Seq.New()
items.Push("apple")
items.Push("banana")
items.Push("cherry")
items.Push("apple")  ' duplicate

DIM fs AS OBJECT
fs = Zanna.Collections.FrozenSet.FromSeq(items)
PRINT fs.Count              ' 3
PRINT fs.Has("apple")     ' 1
PRINT fs.Has("grape")     ' 0

' Get all items
DIM all AS OBJECT
all = fs.ToSeq()
PRINT all.Count             ' 3

' Set operations
DIM items2 AS Zanna.Collections.Seq
items2 = Zanna.Collections.Seq.New()
items2.Push("cherry")
items2.Push("date")
items2.Push("elderberry")
DIM fs2 AS OBJECT
fs2 = Zanna.Collections.FrozenSet.FromSeq(items2)

DIM united AS OBJECT
united = fs.Union(fs2)
PRINT united.Count          ' 5

DIM inter AS OBJECT
inter = fs.Intersect(fs2)
PRINT inter.Count           ' 1 (cherry)

DIM diff AS OBJECT
diff = fs.Difference(fs2)
PRINT diff.Count            ' 2 (apple, banana)

' Subset check
DIM subItems AS Zanna.Collections.Seq
subItems = Zanna.Collections.Seq.New()
subItems.Push("apple")
subItems.Push("banana")
DIM subset AS OBJECT
subset = Zanna.Collections.FrozenSet.FromSeq(subItems)
PRINT subset.IsSubset(fs)   ' 1 (true)

' Equality (order independent)
DIM items3 AS Zanna.Collections.Seq
items3 = Zanna.Collections.Seq.New()
items3.Push("banana")
items3.Push("cherry")
items3.Push("apple")
DIM fs3 AS OBJECT
fs3 = Zanna.Collections.FrozenSet.FromSeq(items3)
PRINT fs.Equals(fs3)        ' 1 (same elements)
```

### Use Cases

- **Constant sets:** Define a fixed set of valid values (e.g., allowed file extensions)
- **Thread-safe sharing:** Share immutable sets between threads without locking
- **Snapshot comparisons:** Compare two snapshots of data for changes
- **Access control lists:** Define immutable permission sets

---

## Zanna.Collections.SortedMap

A sorted key-value map that maintains keys in sorted order. Uses a sorted array with binary search for O(log n) lookups.
Supports range queries via Floor/Ceil operations.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Collections.SortedMap()`

### Properties

| Property  | Type    | Description                            |
|-----------|---------|----------------------------------------|
| `Count`   | Integer | Number of key-value pairs in the map   |
| `IsEmpty` | Boolean | Returns true if the map has no entries |

### Methods

| Method            | Signature              | Description                                                     |
|-------------------|------------------------|-----------------------------------------------------------------|
| `Set(key, value)` | `Void(String, Object)` | Set or update a key-value pair                                  |
| `Get(key)`        | `Object(String)`       | Get the borrowed value for a key (null if not found)            |
| `Has(key)`        | `Boolean(String)`      | Check if a key exists                                           |
| `Remove(key)`     | `Boolean(String)`      | Remove a key-value pair; returns true if removed                |
| `Clear()`         | `Void()`               | Remove all entries                                              |
| `Keys()`          | `Seq()`                | Get all keys as a Seq in sorted order                           |
| `Values()`        | `Seq()`                | Get all values as a Seq in key-sorted order                     |
| `First()`         | `String()`             | Get the smallest (first) key; returns empty string if empty     |
| `Last()`          | `String()`             | Get the largest (last) key; returns empty string if empty       |
| `Floor(key)`      | `String(String)`       | Get the largest key <= given key; returns empty string if none  |
| `Ceiling(key)`       | `String(String)`       | Get the smallest key >= given key; returns empty string if none |

### Notes

- Keys are compared by full byte length in sorted byte order; embedded NUL bytes are part of the key.
- Values are retained while stored and released when overwritten, removed, cleared, or finalized.
- A null runtime key is treated as the empty string key. `Get()` returns a borrowed value; use
  `Has()` to distinguish a missing key from a present null value.
- `First()`, `Last()`, `Floor()`, and `Ceiling()` return owned copied strings, or an empty string when
  no matching key exists.
- `Keys()` and `Values()` are independent retained snapshots in the same key-sorted order. Key
  strings are copied and values are shared, not deep-cloned.
- SortedMap is not thread-safe; synchronize externally around concurrent access.

### Zia Example

```zia
module TreeMapDemo;

bind Zanna.Terminal;
bind Zanna.Collections;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var tm = new SortedMap();

    // Insert in any order — stored sorted
    tm.Set("cherry", Zanna.Core.Box.Str("red"));
    tm.Set("apple", Zanna.Core.Box.Str("green"));
    tm.Set("banana", Zanna.Core.Box.Str("yellow"));

    Say("Count: " + Fmt.Int(tm.Count));           // 3
    Say("First: " + tm.First());                  // apple
    Say("Last: " + tm.Last());                    // cherry

    // Range queries
    Say("Floor(cat): " + tm.Floor("cat"));        // banana
    Say("Ceil(cat): " + tm.Ceiling("cat"));          // cherry
}
```

### BASIC Example

```basic
DIM tm AS Zanna.Collections.SortedMap
tm = NEW Zanna.Collections.SortedMap()

' Insert in any order - stored sorted
tm.Set("cherry", "red")
tm.Set("apple", "green")
tm.Set("banana", "yellow")

' Keys are always in sorted order
DIM keys AS OBJECT = tm.Keys()
PRINT keys.Get(0)  ' Output: "apple"
PRINT keys.Get(1)  ' Output: "banana"
PRINT keys.Get(2)  ' Output: "cherry"

' First/Last access
PRINT tm.First()   ' Output: "apple"
PRINT tm.Last()    ' Output: "cherry"

' Range queries
PRINT tm.Floor("blueberry")  ' Output: "banana" (largest key <= "blueberry")
PRINT tm.Ceiling("blueberry")   ' Output: "cherry" (smallest key >= "blueberry")
```

### SortedMap vs Map

| Feature    | SortedMap  | Map           |
|------------|----------|---------------|
| Key order  | Sorted   | Unordered     |
| Lookup     | O(log n) | O(1) average  |
| Insert     | O(n)     | O(1) average  |
| First/Last | O(1)     | Not available |
| Floor/Ceil | O(log n) | Not available |

### Use Cases

- **Ordered iteration:** When you need keys in sorted order
- **Range queries:** Finding entries within a key range
- **Priority systems:** Using keys as priorities
- **Prefix matching:** Finding nearest matches for partial keys

---


## See Also

- [Sequential Collections](sequential.md)
- [Specialized Maps](multi-maps.md)
- [Functional & Lazy](functional.md)
- [Specialized Structures](specialized.md)
- [Collections Overview](README.md)
- [Zanna Runtime Library](../README.md)
