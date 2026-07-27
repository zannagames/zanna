---
status: active
audience: public
last-verified: 2026-07-26
---

# Specialized Structures
> F64Buffer, I64Buffer, StringSet, BloomFilter, Trie, UnionFind, BitSet, Bytes

**Part of [Zanna Runtime Library](../README.md) › [Collections](README.md)**

---

## Packed Numeric Buffers

`Zanna.Collections.F64Buffer` and `Zanna.Collections.I64Buffer` store fixed-width numeric payloads in contiguous runtime arrays. Use them for dense numeric batches such as samples, particles, vertices, metrics, and solver state. Use `List` or `Seq` when elements must be heterogeneous, reference-typed, or frequently inserted in the middle.

**Type:** Instance (obj)
**Constructors:** `F64Buffer.New(length)`, `I64Buffer.New(length)`, `F64Buffer.FromSeq(seq)`, `I64Buffer.FromSeq(seq)`

### Shared Properties

| Property | Type    | Description             |
|----------|---------|-------------------------|
| `Length` | Integer | Number of numeric slots |

### Shared Methods

| Method              | Signature                         | Description                                       |
|---------------------|-----------------------------------|---------------------------------------------------|
| `Get(index)`        | `Float(Integer)` or `Integer(Integer)` | Read one value                              |
| `Set(index, value)` | `Void(Integer, value)`            | Write one value                                   |
| `Fill(value)`       | `Void(value)`                     | Set every slot to the same value                  |
| `CopyFrom(other)`   | `Void(Buffer)`                    | Resize this buffer to `other.Length` and copy it  |
| `Slice(start, end)` | `Buffer(Integer, Integer)`        | Return an independent copy of `[start, end)`      |
| `AddScalar(value)`  | `Void(value)`                     | Add a scalar to every slot                        |
| `MulScalar(value)`  | `Void(value)`                     | Multiply every slot by a scalar                   |
| `AddBuffer(other)`  | `Void(Buffer)`                    | Add another same-length buffer element-by-element |
| `Sum()`             | `Float()` or `Integer()`          | Sum all values                                    |
| `Dot(other)`        | `Float(Buffer)` or `Integer(Buffer)` | Dot product with another same-length buffer    |
| `Min()`             | `Float()` or `Integer()`          | Minimum value                                     |
| `Max()`             | `Float()` or `Integer()`          | Maximum value                                     |
| `ToList()`          | `List()`                          | Box all values into a new List                    |
| `ToSeq()`           | `Seq()`                           | Box all values into a new Seq                     |

### Notes

- Values are not boxed while stored in the buffer.
- Negative constructor lengths trap. `Get` and `Set` also trap for negative or
  out-of-range indexes.
- `Slice` is an independent copy, not a view. Both bounds are clamped into
  `[0, Length]`; a reversed or empty range returns an empty buffer.
- `CopyFrom` changes the destination's `Length` to match the source before
  copying its values.
- `AddBuffer` and `Dot` trap on length mismatch.
- `Min` and `Max` trap on empty buffers; `Sum` of an empty buffer returns zero.
- `ToList` and `ToSeq` box values, and `FromSeq` reads boxed numeric values. `F64Buffer.FromSeq` accepts boxed floats and boxed integers; `I64Buffer.FromSeq` accepts boxed integers.
- `I64Buffer` arithmetic currently uses unchecked signed C arithmetic in
  `AddScalar`, `MulScalar`, `AddBuffer`, `Sum`, and `Dot`. Overflow does not
  produce a Zanna overflow trap and has no portable result; keep intermediate
  and final values within the signed 64-bit range.
- Buffers are not thread-safe. Synchronize access when any thread can mutate a
  buffer.

---

## Zanna.Collections.StringSet

A set data structure for storing unique strings. Efficiently handles membership testing, set operations (union,
intersection, difference), and enumeration.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Collections.StringSet()`

### Properties

| Property  | Type    | Description                     |
|-----------|---------|---------------------------------|
| `Count`   | Integer | Number of strings in the bag    |
| `IsEmpty` | Boolean | True if bag contains no strings |

### Methods

| Method          | Signature         | Description                                                  |
|-----------------|-------------------|--------------------------------------------------------------|
| `Add(str)`      | `Boolean(String)` | Add a string; returns true if new, false if already present  |
| `Remove(str)`   | `Boolean(String)` | Remove a string; returns true if removed, false if not found |
| `Has(str)`      | `Boolean(String)` | Check if string is in the bag                                |
| `Clear()`       | `Void()`          | Remove all strings from the bag                              |
| `Clone()`       | `StringSet()`           | Return an independent copy                                   |
| `Union(other)`     | `StringSet(StringSet)`        | Return new bag with union of both bags                       |
| `Intersect(other)` | `StringSet(StringSet)`        | Return new bag with intersection of both bags                |
| `Difference(other)`   | `StringSet(StringSet)`        | Return new bag with elements in this but not other           |
| `ToSeq()`        | `Seq()`           | Return all strings as a Seq (order undefined)                  |
| `ToSet()`        | `Set()`           | Return all strings as a new Set                                |

### Notes

- Strings are stored by value (copied into the bag). The full runtime byte
  length participates in hashing and equality; a null string argument is
  treated as the empty string.
- Order of strings returned by `ToSeq()` is not guaranteed (hash-table order).
- `ToSeq()` and `ToSet()` return independent snapshots containing copied strings.
- `Clone` and the set operations (`Union`, `Intersect`, `Difference`) return new bags;
  their inputs are unchanged.
- The implementation uses FNV-1a hashing and separate chaining for average
  O(1) membership, insertion, and removal. It grows after the next insertion
  would exceed a 75% load factor.
- Each node caches its hash and stores the copied key inline. A duplicate `Add`
  returns false without allocation; an allocation or growth failure traps and
  never masquerades as a duplicate result. Failed growth leaves the existing
  set unchanged.
- `ToSet()` boxes each string on conversion, so the resulting `Set` uses normal boxed-string
  value equality: `bag.ToSet().Has(Box.Str("apple"))` is true whenever the bag contains
  `"apple"`.
- Bags are not thread-safe. Concurrent reads are safe only while no thread can
  mutate the bag.

### Zia Example

```zia
module BagDemo;

bind Zanna.Terminal;
bind Zanna.Collections;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var fruits = new StringSet();
    fruits.Add("apple");
    fruits.Add("banana");
    fruits.Add("cherry");
    Say("Count: " + Fmt.Int(fruits.Count));              // 3

    // Membership testing
    Say("Has banana: " + Fmt.Bool(fruits.Has("banana")));  // true
    Say("Has grape: " + Fmt.Bool(fruits.Has("grape")));    // false

    // Duplicate returns false
    Say("Add apple: " + Fmt.Bool(fruits.Add("apple")));    // false

    // Remove
    fruits.Remove("banana");
    Say("After remove: " + Fmt.Int(fruits.Count));             // 2
}
```

### BASIC Example

```basic
' Create and populate a bag
DIM fruits AS Zanna.Collections.StringSet = NEW Zanna.Collections.StringSet()
fruits.Add("apple")
fruits.Add("banana")
fruits.Add("cherry")
PRINT fruits.Count           ' Output: 3

' Duplicate add returns false
DIM wasNew AS INTEGER = fruits.Add("apple")
PRINT wasNew               ' Output: 0 (already present)

' Membership testing
PRINT fruits.Has("banana") ' Output: 1 (true)
PRINT fruits.Has("grape")  ' Output: 0 (false)

' Remove an element
DIM removed AS INTEGER = fruits.Remove("banana")
PRINT removed              ' Output: 1 (was removed)
PRINT fruits.Has("banana") ' Output: 0 (no longer present)

' Set operations
DIM bagA AS Zanna.Collections.StringSet = NEW Zanna.Collections.StringSet()
bagA.Add("a")
bagA.Add("b")
bagA.Add("c")

DIM bagB AS Zanna.Collections.StringSet = NEW Zanna.Collections.StringSet()
bagB.Add("b")
bagB.Add("c")
bagB.Add("d")

' Union: elements in either bag
DIM merged AS Zanna.Collections.StringSet = bagA.Union(bagB)
PRINT merged.Count           ' Output: 4 (a, b, c, d)

' Intersection: elements in both bags
DIM common AS Zanna.Collections.StringSet = bagA.Intersect(bagB)
PRINT common.Count           ' Output: 2 (b, c)

' Difference: elements in A but not B
DIM diff AS Zanna.Collections.StringSet = bagA.Difference(bagB)
PRINT diff.Count             ' Output: 1 (a only)

' Enumerate all elements
DIM items AS Zanna.Collections.Seq = fruits.ToSeq()
FOR i = 0 TO items.Count - 1
    PRINT Zanna.Collections.Seq.GetStr(items, i)
NEXT
```

### Use Cases

- **Deduplication:** Track unique values encountered
- **Membership testing:** Fast O(1) lookup for string membership
- **Set mathematics:** Compute unions, intersections, and differences
- **Tag systems:** Manage collections of unique tags or labels
- **Visited tracking:** Track visited items in algorithms

---

## Zanna.Collections.BloomFilter

A space-efficient probabilistic data structure for membership testing. Can definitively say an element is *not* in
the set, but may produce false positives. Ideal for pre-filtering expensive lookups.

**Type:** Instance (obj)
**Constructor:** `Zanna.Collections.BloomFilter.New(capacity, falsePositiveRate)` - creates a filter sized for the
expected number of elements and desired false positive rate

`capacity = 0` is treated as one expected item. Negative capacity traps.

### Properties

| Property | Type    | Description                          |
|----------|---------|--------------------------------------|
| `Count`  | Integer | Number of non-null `Add` operations; duplicates count again |

### Methods

| Method              | Signature            | Description                                                    |
|---------------------|----------------------|----------------------------------------------------------------|
| `Add(str)`          | `Void(String)`       | Add a string to the filter                                     |
| `MightContain(str)` | `Boolean(String)`    | Check if string might be in the filter (false positives possible) |
| `Clear()`           | `Void()`             | Remove all elements from the filter                            |
| `FalsePositiveRate()` | `Float()`          | Get the estimated current false positive rate                  |
| `Merge(other)`      | `Integer(BloomFilter)` | OR in a compatible filter; return 1 on success or 0 if incompatible |

### Notes

- `MightContain` returning true means the element *may* be present; returning false means it is *definitely* not present
- `Count` is an operation count, not a distinct-element count. Adding the same
  non-null string again increments it, although the filter bits may not change.
- `FalsePositiveRate()` estimates the current false positive rate from the fraction of bits currently set, so duplicate additions do not inflate the estimate.
- `FalsePositiveRate()` remains available as a compatibility alias for
  `FalsePositiveRate()`.
- `Merge` requires matching *derived* bit and hash counts. Filters created with
  different inputs can therefore be compatible if those inputs round to the
  same configuration. A successful non-self merge sums both operation counts,
  including overlapping or duplicate additions; merging a filter with itself
  is a no-op that leaves `Count` unchanged.
- Strings are hashed over their full runtime byte length. A null string is
  ignored by `Add` and is always rejected by `MightContain`.
- After `Clear()`, `MightContain` returns false for all elements.
- A non-finite or non-positive false-positive rate becomes `0.01`; a rate of
  `1.0` or greater becomes `0.5`.
- Adding more than the expected number of items can raise the false-positive
  rate above the requested target. Sizing and item-count overflow trap instead
  of wrapping.
- Bloom filters are not thread-safe.

### Zia Example

```zia
module BloomFilterDemo;

bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Create filter for 100 items with 1% false positive rate
    var bf = BloomFilter.New(100, 0.01);

    // Add elements
    bf.Add("apple");
    bf.Add("banana");
    bf.Add("cherry");
    SayInt(bf.Count);                              // 3

    // Membership testing
    SayBool(bf.MightContain("apple"));             // 1 (true)
    SayBool(bf.MightContain("grape"));             // 0 (probably false)

    // Merge two filters
    var bf2 = BloomFilter.New(100, 0.01);
    bf2.Add("grape");
    bf2.Add("kiwi");
    SayInt(bf.Merge(bf2));                         // 1 (success)
    SayBool(bf.MightContain("grape"));             // 1 (now true)

    // Clear
    bf.Clear();
    SayInt(bf.Count);                              // 0
    SayBool(bf.MightContain("apple"));             // 0
}
```

### BASIC Example

```basic
' Create filter for 100 items with 1% false positive rate
DIM bf AS OBJECT
bf = Zanna.Collections.BloomFilter.New(100, 0.01)

' Add elements
bf.Add("apple")
bf.Add("banana")
bf.Add("cherry")
PRINT bf.Count                       ' 3

' Membership testing
PRINT bf.MightContain("apple")       ' 1 (true - definitely added)
PRINT bf.MightContain("grape")       ' 0 (probably not present)

' Current false positive rate
PRINT bf.FalsePositiveRate()         ' Very low (near 0.0)

' Merge two filters
DIM bf2 AS OBJECT
bf2 = Zanna.Collections.BloomFilter.New(100, 0.01)
bf2.Add("grape")
bf2.Add("kiwi")
PRINT bf.Merge(bf2)                  ' 1 (success)
PRINT bf.MightContain("grape")       ' 1 (now present)
PRINT bf.MightContain("kiwi")        ' 1 (now present)

' Clear the filter
bf.Clear()
PRINT bf.Count                       ' 0
PRINT bf.MightContain("apple")       ' 0 (no longer present)
```

### Use Cases

- **Database pre-filtering:** Avoid expensive disk lookups for absent keys
- **URL deduplication:** Quickly check if a URL has been visited in a web crawler
- **Spam detection:** Pre-filter email addresses against known spammers
- **Cache lookup:** Check if an item might be cached before querying the cache
- **Network routing:** Efficiently check membership in distributed systems

---

## Zanna.Collections.Trie

A prefix tree (trie) that maps string keys to values. Supports efficient prefix-based operations including prefix
existence checking, longest prefix matching, and retrieving all keys with a given prefix.

**Type:** Instance (obj)
**Constructor:** `Zanna.Collections.Trie.New()`

### Properties

| Property  | Type    | Description                             |
|-----------|---------|-----------------------------------------|
| `IsEmpty` | Boolean | True if the trie has no entries         |
| `Count`   | Integer | Number of key-value pairs in the trie   |

### Methods

| Method                | Signature            | Description                                                          |
|-----------------------|----------------------|----------------------------------------------------------------------|
| `Set(key, value)`     | `Void(String, Object)` | Add or update a key-value pair                                    |
| `Get(key)`            | `Object(String)`     | Get value for an exact key match (null if not found)                 |
| `Has(key)`            | `Boolean(String)`    | Check if an exact key exists                                         |
| `HasPrefix(prefix)`   | `Boolean(String)`    | Check if any key starts with the given prefix                        |
| `LongestPrefix(str)`  | `String(String)`     | Find the longest key that is a prefix of the given string            |
| `WithPrefix(prefix)`  | `Seq(String)`        | Get all keys that start with the given prefix                        |
| `Keys()`              | `Seq()`              | Get all keys as a Seq                                                |
| `Clone()`             | `Trie()`             | Copy the trie structure while sharing retained values                |
| `Remove(key)`         | `Boolean(String)`    | Remove a key-value pair; returns true if found                       |
| `Clear()`             | `Void()`             | Remove all entries                                                   |

### Notes

- `Has` checks for an exact key, not a prefix; use `HasPrefix` for prefix existence checks
- `LongestPrefix` finds the longest stored key that is a prefix of the input string (useful for routing)
- `WithPrefix` returns all keys that start with the given prefix, including exact matches
- Trie keys and prefixes use the full runtime string byte length; embedded NUL bytes are part of the key. A null key is treated as the empty key.
- `Get()` returns a borrowed value. A missing key and a present key storing null
  both return null, so use `Has()` when that distinction matters. The trie
  retains stored values until overwrite, removal, clear, or destruction.
- `LongestPrefix()` returns an owned copied string, or an owned empty string
  when no key matches. Because the empty key is valid, an empty-key match has
  the same return value as no match; use `Has("")` if that distinction matters.
- `WithPrefix()` and `Keys()` return owning snapshots of copied strings in
  ascending byte-lexicographic order, including when the requested prefix is
  longer than the default internal buffer size.
- `Clone()` deep-copies the node structure but shares and retains the stored
  values. Structural changes to either trie do not affect the other.
- Deep key traversal, cloning, clearing, and finalization use iterative walks rather than recursive C calls.
- Removing a key does not affect other keys that share the same prefix. Removed
  branches are not pruned, so their node memory remains until `Clear()` or trie
  destruction.
- Every node contains 256 child pointers, making this implementation fast for
  byte traversal but comparatively memory-heavy for sparse key sets.
- Tries are not thread-safe.

### Zia Example

```zia
module TrieDemo;

bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var t = Trie.New();

    // Insert keys
    t.Set("cat", Box.I64(1));
    t.Set("car", Box.I64(2));
    t.Set("card", Box.I64(3));
    t.Set("care", Box.I64(4));
    t.Set("dog", Box.I64(5));
    SayInt(t.Count);                                 // 5

    // Exact lookup
    SayInt(Box.ToI64(t.Get("cat")));               // 1
    SayBool(t.Has("cat"));                         // 1
    SayBool(t.Has("ca"));                          // 0 (prefix, not a key)

    // Prefix operations
    SayBool(t.HasPrefix("ca"));                    // 1
    SayBool(t.HasPrefix("x"));                     // 0

    // Longest prefix match
    Say(t.LongestPrefix("card_game"));             // card
    Say(t.LongestPrefix("caring"));                // care
    Say(t.LongestPrefix("catalog"));               // cat

    // All keys with prefix
    var carKeys = t.WithPrefix("car");
    SayInt(carKeys.Count);                           // 3 (car, card, care)

    // Remove
    t.Remove("card");
    SayBool(t.Has("card"));                        // 0
    SayBool(t.Has("car"));                         // 1 (not affected)
}
```

### BASIC Example

```basic
DIM t AS OBJECT
t = Zanna.Collections.Trie.New()

' Insert keys with values
t.Set("cat", Zanna.Core.Box.I64(1))
t.Set("car", Zanna.Core.Box.I64(2))
t.Set("card", Zanna.Core.Box.I64(3))
t.Set("care", Zanna.Core.Box.I64(4))
t.Set("dog", Zanna.Core.Box.I64(5))
PRINT t.Count                     ' 5

' Exact lookup
PRINT Zanna.Core.Box.ToI64(t.Get("cat"))   ' 1
PRINT t.Has("cat")              ' 1
PRINT t.Has("ca")               ' 0 (prefix, not a key)

' Prefix checks
PRINT t.HasPrefix("ca")         ' 1
PRINT t.HasPrefix("car")        ' 1
PRINT t.HasPrefix("x")          ' 0

' Longest prefix match
PRINT t.LongestPrefix("card_game")  ' card
PRINT t.LongestPrefix("caring")     ' care
PRINT t.LongestPrefix("catalog")    ' cat
PRINT t.LongestPrefix("dogs")       ' dog

' All keys with prefix
DIM carKeys AS OBJECT
carKeys = t.WithPrefix("car")
PRINT carKeys.Count               ' 3 (car, card, care)

DIM caKeys AS OBJECT
caKeys = t.WithPrefix("ca")
PRINT caKeys.Count                ' 4 (cat, car, card, care)

' Update existing key
t.Set("cat", Zanna.Core.Box.I64(100))
PRINT Zanna.Core.Box.ToI64(t.Get("cat"))  ' 100
PRINT t.Count                     ' 5 (no new entry)

' Remove a key (does not affect siblings)
PRINT t.Remove("card")          ' 1
PRINT t.Has("card")             ' 0
PRINT t.Has("car")              ' 1
PRINT t.Has("care")             ' 1

' Clear
t.Clear()
PRINT t.IsEmpty                 ' 1
```

### Use Cases

- **Autocomplete:** Find all words matching a typed prefix with `WithPrefix`
- **URL routing:** Match URL paths to handlers using `LongestPrefix`
- **Textual routing prefixes:** Match byte-string route prefixes (this is not a
  CIDR or bit-prefix routing implementation)
- **Spell checking:** Check if partial words exist as prefixes of known words
- **Dictionary lookup:** Efficient storage and retrieval of string-keyed data

---

## Zanna.Collections.UnionFind

A disjoint set (union-find) data structure with path compression and union by rank. Efficiently tracks which
elements belong to the same group and supports merging groups.

**Type:** Instance (obj)
**Constructor:** `Zanna.Collections.UnionFind.New(size)` - creates a structure with `size` elements, each in its own set

`size = 0` creates one element. Negative sizes trap.

### Properties

| Property | Type    | Description                                    |
|----------|---------|------------------------------------------------|
| `Count`  | Integer | Number of disjoint sets (decreases after Union) |

### Methods

| Method                | Signature                  | Description                                                     |
|-----------------------|----------------------------|-----------------------------------------------------------------|
| `FindRoot(x)`            | `Option[Integer](Integer)` | Representative root of element x's set as `Some(root)`, or `None` for an invalid element |
| `Union(x, y)`        | `Integer(Integer, Integer)`| Merge the sets containing x and y; returns 1 if merged, otherwise 0 |
| `IsConnected(x, y)`    | `Boolean(Integer, Integer)`| Check if x and y are in the same set                            |
| `ComponentSize(x)`         | `Integer(Integer)`         | Get the size of the set containing element x                    |
| `Clear()`            | `Void()`                   | Reset all elements to individual singleton sets                 |

### Notes

- Elements are identified by integers from 0 to effective-size minus one. The
  effective size is one when the constructor argument is zero.
- Uses path compression and union by rank for near-O(1) amortized operations.
- `FindRoot()` returns `Some(root)` for a valid element, or `None` for an out-of-range index.
- `Union` returns 0 both when the elements are already in the same set and when
  either index is invalid. `IsConnected` returns false for invalid indexes, and
  `ComponentSize` returns zero.
- `Clear` restores the structure to its initial state
  with `Count` equal to the effective size.
- Union-find objects are not thread-safe; even `Find` can mutate parent links
  through path compression.

### Zia Example

```zia
module UnionFindDemo;

bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var uf = UnionFind.New(6);
    SayInt(uf.Count);                            // 6 (each element is its own set)

    // Merge sets
    uf.Union(0, 1);
    uf.Union(2, 3);
    SayInt(uf.Count);                            // 4

    // Check connectivity
    SayBool(uf.IsConnected(0, 1));                 // 1
    SayBool(uf.IsConnected(0, 2));                 // 0

    // Merge across groups
    uf.Union(1, 3);
    SayBool(uf.IsConnected(0, 2));                 // 1 (now connected via 1-3)
    SayInt(uf.Count);                            // 3

    // Set sizes
    SayInt(uf.ComponentSize(0));                       // 4 ({0,1,2,3})
    SayInt(uf.ComponentSize(4));                       // 1 ({4} alone)

    var root = uf.FindRoot(3);
    if root.IsSome {
        SayInt(root.UnwrapI64());                // representative root
    }

    // Reset to individual sets
    uf.Clear();
    SayInt(uf.Count);                            // 6
    SayBool(uf.IsConnected(0, 1));                 // 0
}
```

### BASIC Example

```basic
DIM uf AS OBJECT
uf = Zanna.Collections.UnionFind.New(6)
PRINT uf.Count               ' 6 (each element is its own set)

' Find representative
DIM root AS OBJECT = uf.FindRoot(0)
IF root.IsSome THEN
    PRINT root.UnwrapI64()   ' 0 (own representative)
END IF

' Merge sets
PRINT uf.Union(0, 1)         ' 1 (merged)
PRINT uf.Count               ' 5
PRINT uf.Union(2, 3)         ' 1 (merged)
PRINT uf.Count               ' 4
PRINT uf.Union(0, 1)         ' 0 (already in same set)

' Check connectivity
PRINT uf.IsConnected(0, 1)     ' 1 (same set)
PRINT uf.IsConnected(0, 2)     ' 0 (different sets)

' Merge across groups
uf.Union(1, 3)
PRINT uf.IsConnected(0, 2)     ' 1 (now connected)
PRINT uf.Count               ' 3

' Set sizes
PRINT uf.ComponentSize(0)          ' 4 ({0,1,2,3})
PRINT uf.ComponentSize(4)          ' 1 ({4} alone)

' Merge all remaining
uf.Union(4, 5)
uf.Union(0, 4)
PRINT uf.Count               ' 1
PRINT uf.ComponentSize(0)          ' 6 (all connected)

' Reset to individual sets
uf.Clear()
PRINT uf.Count               ' 6
PRINT uf.IsConnected(0, 1)     ' 0
PRINT uf.ComponentSize(0)          ' 1
```

### Use Cases

- **Network connectivity:** Determine if two nodes are connected in a network
- **Kruskal's algorithm:** Build minimum spanning trees
- **Image segmentation:** Group connected pixels into regions
- **Equivalence classes:** Track which items are equivalent
- **Percolation:** Model physical systems to detect connected paths

---

## Zanna.Collections.BitSet

A growable set of bits supporting efficient bitwise operations. Useful for compact storage of boolean flags and
set operations on integer-indexed elements.

**Type:** Instance (obj)
**Constructor:** `Zanna.Collections.BitSet.New(size)` - creates a BitSet with the given number of bits

`size = 0` creates 64 logical bits. Negative sizes trap.

### Properties

| Property  | Type    | Description                              |
|-----------|---------|------------------------------------------|
| `Count`   | Integer | Number of bits currently set to 1        |
| `IsEmpty` | Boolean | True if no bits are set                  |
| `Length`  | Integer | Total number of bits (size of the set)   |

### Methods

| Method        | Signature              | Description                                            |
|---------------|------------------------|--------------------------------------------------------|
| `SetBit(index)`  | `Void(Integer)`     | Set bit to 1; auto-grow for a non-negative index       |
| `ClearBit(index)`| `Void(Integer)`     | Clear a bit; negative/out-of-range indexes are no-ops  |
| `Clear()`     | `Void()`               | Clear all bits to 0                                    |
| `GetBit(index)`  | `Boolean(Integer)`  | Get a bit; return false outside the logical length     |
| `ToggleBit(index)` | `Void(Integer)`   | Flip a bit; auto-grow for a non-negative index        |
| `SetAll()`    | `Void()`               | Set all bits to 1                                      |
| `And(other)`  | `BitSet(BitSet)`       | Return new BitSet with bitwise AND of both sets        |
| `Or(other)`   | `BitSet(BitSet)`       | Return new BitSet with bitwise OR of both sets         |
| `Xor(other)`  | `BitSet(BitSet)`       | Return new BitSet with bitwise XOR of both sets        |
| `Not()`       | `BitSet()`             | Return new BitSet with all bits inverted               |
| `ToString()`  | `String()`             | Return binary string representation of the bit pattern |

### Notes

- The bitset auto-grows when setting or toggling beyond the current logical
  `Length`; negative indexes are ignored. `GetBit` returns false outside the
  current length, and `ClearBit` is a no-op there.
- `And`, `Or`, and `Xor` return new BitSets whose length is the longer input's
  length; missing bits in the shorter input are zero. `Not` preserves the input
  length. The originals are unchanged.
- `ToString()` places the most significant set bit on the left, suppresses
  leading zeroes, and always returns at least `"0"`. It is not a fixed-width
  rendering of `Length`.
- Indices are zero-based; index 0 is the least significant bit.
- Auto-growth may allocate spare backing capacity, but it is never observable: `SetAll`,
  `Count`, and the binary operations work strictly within the logical `Length`, so
  `Count <= Length` always holds regardless of growth history.
- BitSets are not thread-safe.

### Zia Example

```zia
module BitSetDemo;

bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var bs = BitSet.New(8);

    // Set some bits
    bs.SetBit(0);
    bs.SetBit(2);
    bs.SetBit(7);
    Say("Count: " + Fmt.Int(bs.Count));         // 3
    Say("Get(2): " + Fmt.Bool(bs.GetBit(2)));       // true
    Say("Get(1): " + Fmt.Bool(bs.GetBit(1)));       // false

    // Toggle a bit
    bs.ToggleBit(2);
    Say("After toggle Get(2): " + Fmt.Bool(bs.GetBit(2)));  // false

    // Binary representation
    Say("Binary: " + bs.ToString());
    Say("Len: " + Fmt.Int(bs.Length));              // 8

    // Bitwise operations
    var a = BitSet.New(8);
    a.SetBit(0);
    a.SetBit(1);
    var b = BitSet.New(8);
    b.SetBit(1);
    b.SetBit(2);
    var result = a.And(b);
    SayInt(result.Count);                        // 1 (only bit 1)
}
```

### BASIC Example

```basic
DIM bs AS OBJECT
bs = Zanna.Collections.BitSet.New(8)

' Set some bits
bs.SetBit(0)
bs.SetBit(2)
bs.SetBit(7)
PRINT "Count: "; bs.Count       ' Count: 3
PRINT "Get(7): "; bs.GetBit(7)     ' Get(7): -1
PRINT "Get(1): "; bs.GetBit(1)     ' Get(1): 0

' Toggle a bit
bs.ToggleBit(7)
PRINT "After toggle Get(7): "; bs.GetBit(7)  ' After toggle Get(7): 0

PRINT "Len: "; bs.Length           ' Len: 8

' Binary representation
PRINT bs.ToString()

' Bitwise AND
DIM a AS OBJECT = Zanna.Collections.BitSet.New(8)
a.SetBit(0)
a.SetBit(1)
a.SetBit(2)
DIM b AS OBJECT = Zanna.Collections.BitSet.New(8)
b.SetBit(1)
b.SetBit(2)
b.SetBit(3)
DIM andResult AS OBJECT = a.And(b)
PRINT andResult.Count            ' 2 (bits 1 and 2)

' Bitwise NOT
DIM c AS OBJECT = Zanna.Collections.BitSet.New(4)
c.SetBit(0)
c.SetBit(2)
DIM notResult AS OBJECT = c.Not()
PRINT notResult.GetBit(1)           ' 1 (inverted)
```

### Use Cases

- **Permission flags:** Store and check multiple boolean permissions compactly
- **Feature toggles:** Track which features are enabled
- **Set intersection/union:** Efficient set operations on integer-indexed elements
- **Bloom filter backing:** Underlying storage for probabilistic data structures
- **Graph algorithms:** Track visited nodes in dense graphs

---

## Zanna.Collections.Bytes

An efficient byte array for binary data. More memory-efficient than Seq for byte manipulation.

**Type:** Instance (obj)
**Constructors:**

- `NEW Zanna.Collections.Bytes(length)` - Create zero-filled byte array
- `Zanna.Collections.Bytes.FromStr(str)` - Copy the runtime string's exact byte sequence
- `Zanna.Collections.Bytes.FromHex(hex)` - Decode a strict hexadecimal string
- `Zanna.Collections.Bytes.FromBase64(b64)` - Decode strict, padded RFC 4648 Base64 (traps on invalid input)

### Properties

| Property  | Type    | Description                              |
|-----------|---------|------------------------------------------|
| `Length`  | Integer | Number of bytes                          |
| `IsEmpty` | Boolean | Returns true if the byte array is empty  |

### Methods

| Method                                   | Signature                 | Description                                                           |
|------------------------------------------|---------------------------|-----------------------------------------------------------------------|
| `Get(index)`                             | `Integer(Integer)`        | Get byte value (0-255) at index                                       |
| `Set(index, value)`                      | `Void(Integer, Integer)`  | Store the low 8 bits of value at index                                |
| `Slice(start, end)`                      | `Bytes(Integer, Integer)` | Copy the clamped range `[start, end)`                                 |
| `Copy(dstOffset, src, srcOffset, count)` | `Void(Integer, Bytes, Integer, Integer)` | Copy into this array; overlap is supported              |
| `ToStr()`                                | `String()`                | Copy all bytes into a runtime string without validation               |
| `ToHex()`                                | `String()`                | Convert to lowercase hexadecimal string                               |
| `ToBase64()`                             | `String()`                | Convert to RFC 4648 Base64 string (A-Z a-z 0-9 + /, with '=' padding) |
| `Fill(value)`                            | `Void(Integer)`           | Set all bytes to the low 8 bits of value                              |
| `Find(value)`                            | `Option[Integer](Integer)` | First index of the low-8-bit value as `Some(index)`, or `None` if not found |
| `Clone()`                                | `Bytes()`                 | Create independent copy                                               |
| `ReadI16LittleEndian(offset)`                      | `Integer(Integer)`        | Read 16-bit signed integer at offset (little-endian)                  |
| `ReadI16BigEndian(offset)`                      | `Integer(Integer)`        | Read 16-bit signed integer at offset (big-endian)                     |
| `ReadI32LittleEndian(offset)`                      | `Integer(Integer)`        | Read 32-bit signed integer at offset (little-endian)                  |
| `ReadI32BigEndian(offset)`                      | `Integer(Integer)`        | Read 32-bit signed integer at offset (big-endian)                     |
| `ReadI64LittleEndian(offset)`                      | `Integer(Integer)`        | Read 64-bit signed integer at offset (little-endian)                  |
| `ReadI64BigEndian(offset)`                      | `Integer(Integer)`        | Read 64-bit signed integer at offset (big-endian)                     |
| `WriteI16LittleEndian(offset, value)`              | `Void(Integer, Integer)`  | Write 16-bit integer at offset (little-endian)                        |
| `WriteI16BigEndian(offset, value)`              | `Void(Integer, Integer)`  | Write 16-bit integer at offset (big-endian)                           |
| `WriteI32LittleEndian(offset, value)`              | `Void(Integer, Integer)`  | Write 32-bit integer at offset (little-endian)                        |
| `WriteI32BigEndian(offset, value)`              | `Void(Integer, Integer)`  | Write 32-bit integer at offset (big-endian)                           |
| `WriteI64LittleEndian(offset, value)`              | `Void(Integer, Integer)`  | Write 64-bit integer at offset (little-endian)                        |
| `WriteI64BigEndian(offset, value)`              | `Void(Integer, Integer)`  | Write 64-bit integer at offset (big-endian)                           |

### Notes

- `Length` is fixed after construction. `Get`, `Set`, and every binary
  read/write trap when their required bytes fall outside the array.
- Byte-valued operations truncate with `value & 255`; they do not saturate.
  For example, setting `256` stores `0`, and setting `-1` stores `255`.
- `Slice()` clamps both bounds into `[0, Length]` and returns an independent
  copy. Reversed or empty ranges return an empty Bytes object.
- `FromHex()` accepts uppercase or lowercase digits but requires an even number
  of digits and accepts no separators or whitespace. `FromBase64()` requires
  the standard alphabet, a multiple-of-four length, canonical padding bits,
  and `=` padding where needed; it does not accept whitespace or the URL-safe
  alphabet. Both parse the full runtime string byte length, so embedded NUL
  bytes do not truncate validation.
- `ToStr()` does not validate UTF-8. It creates a runtime string containing the
  exact bytes, including embedded NULs or malformed UTF-8.
- `Copy()` uses overlap-safe `memmove`. It traps for invalid Bytes operands,
  negative counts, arithmetic overflow, or out-of-bounds non-empty ranges. A
  zero-byte copy is a no-op and does not validate either offset.
- `ReadI16*()` and `ReadI32*()` sign-extend into the returned Integer. Values with the high bit set return negative numbers. The I64 readers return the corresponding signed 64-bit bit pattern.
- I16 and I32 writes keep only the low 16 or 32 bits respectively; I64 writes
  store all 64 bits.
- Negative byte-array lengths trap. Raw byte inputs larger than the maximum runtime `Bytes` length, or null raw inputs with non-zero length, are rejected before allocation.
- `Find()` returns `Some(index)` for the first matching byte, or `None` when the value is not present.
- Bytes objects are not thread-safe for concurrent mutation. Concurrent reads
  are safe only while no thread writes the object.

### Zia Example

```zia
module BytesDemo;

bind Zanna.Terminal;
bind Zanna.Collections;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Create from string
    var data = Bytes.FromStr("Hello");
    Say("Length: " + Fmt.Int(data.Length));        // 5
    Say("First byte: " + Fmt.Int(data.Get(0))); // 72 (ASCII 'H')
    Say("As string: " + data.ToStr());          // Hello

    // Hex and Base64 encoding
    Say("Hex: " + data.ToHex());                // 48656c6c6f
    Say("Base64: " + data.ToBase64());          // SGVsbG8=

    // Create from hex
    var hex = Bytes.FromHex("deadbeef");
    Say("From hex len: " + Fmt.Int(hex.Length));   // 4
}
```

### BASIC Example

```basic
' Create a 4-byte array and set values
DIM data AS Zanna.Collections.Bytes
data = NEW Zanna.Collections.Bytes(4)
data.Set(0, 222)
data.Set(1, 173)
data.Set(2, 190)
data.Set(3, 239)

PRINT data.ToHex()  ' Output: "deadbeef"
PRINT data.Length      ' Output: 4

' Create from hex string
DIM copy AS Zanna.Collections.Bytes
copy = Zanna.Collections.Bytes.FromHex("cafebabe")
PRINT copy.Get(0)   ' Output: 202 (0xCA)

' Create from string
DIM text AS Zanna.Collections.Bytes
text = Zanna.Collections.Bytes.FromStr("Hello")
PRINT text.Length      ' Output: 5
PRINT text.Get(0)   ' Output: 72 (ASCII 'H')

' Base64 encode/decode (RFC 4648)
PRINT text.ToBase64()  ' Output: "SGVsbG8="
DIM decoded AS Zanna.Collections.Bytes
decoded = Zanna.Collections.Bytes.FromBase64("SGVsbG8=")
PRINT decoded.ToStr()  ' Output: "Hello"

' Slice and copy
DIM slice AS Zanna.Collections.Bytes
slice = data.Slice(1, 3)  ' Bytes at indices 1 and 2
PRINT slice.Length           ' Output: 2

' Find a byte
DIM found AS OBJECT = data.Find(190)
IF found.IsSome THEN
    PRINT found.UnwrapI64()  ' Output: 2
END IF

' Fill with a value
data.Fill(0)
PRINT data.ToHex()        ' Output: "00000000"
```

### Use Cases

- **Binary file parsing:** Read and manipulate binary file formats
- **Network protocols:** Pack and unpack protocol messages
- **Cryptography:** Handle raw byte sequences
- **Image data:** Manipulate raw pixel data
- **Base encoding:** Convert between binary and text representations

---


## See Also

- [Sequential Collections](sequential.md)
- [Maps & Sets](maps-sets.md)
- [Specialized Maps](multi-maps.md)
- [Functional & Lazy](functional.md)
- [Collections Overview](README.md)
- [Zanna Runtime Library](../README.md)
