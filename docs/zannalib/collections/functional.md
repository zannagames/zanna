---
status: active
audience: public
last-verified: 2026-09-01
---

# Functional & Lazy
> Seq, LazySeq, Iterator

**Part of [Zanna Runtime Library](../README.md) › [Collections](README.md)**

---

## Zanna.Collections.Seq

Dynamic sequence (growable array) with stack operations. Zanna's primary growable collection type, supporting
push/pop, insert/remove, and slicing operations.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Collections.Seq()`, `Zanna.Collections.Seq.NewSized(size)`, or `Zanna.Collections.Seq.WithCapacity(cap)`

### Properties

| Property  | Type    | Description                                    |
|-----------|---------|------------------------------------------------|
| `Count`   | Integer | Number of elements currently in the sequence   |
| `Capacity` | Integer | Current allocated capacity                   |
| `IsEmpty` | Boolean | Returns true if the sequence has zero elements |

### Methods

| Method                 | Signature               | Description                                                                           |
|------------------------|-------------------------|---------------------------------------------------------------------------------------|
| `Get(index)`           | `Object(Integer)`       | Returns the element at the specified index (0-based); traps out of bounds             |
| `Set(index, value)`    | `Void(Integer, Object)` | Sets the element at the specified index                                               |
| `Push(value)`          | `Void(Object)`          | Appends an element to the end                                                         |
| `PushAll(other)`       | `Void(Seq)`             | Appends all elements of `other` onto this sequence (self-appends double the sequence) |
| `Pop()`                | `Object()`              | Removes and returns the last element                                                  |
| `Peek()`               | `Object()`              | Returns the last element without removing it; traps when empty                        |
| `First()`              | `Object()`              | Returns the first element; traps when empty                                           |
| `Last()`               | `Object()`              | Returns the last element; traps when empty                                            |
| `Insert(index, value)` | `Void(Integer, Object)` | Inserts an element at the specified position                                          |
| `RemoveAt(index)`      | `Object(Integer)`       | Removes and returns the element at the specified position                             |
| `Clear()`              | `Void()`                | Removes all elements                                                                  |
| `FindOption(value)`    | `Option[Integer](Object)` | Returns `Some(index)` for a matching value, or `None` if not found                 |
| `Has(value)`           | `Boolean(Object)`       | Returns true if the sequence contains the value                                       |
| `Reverse()`            | `Void()`                | Reverses the elements in place                                                        |
| `Shuffle()`            | `Void()`                | Shuffles the elements in place (deterministic when `Zanna.Math.Random.Seed` is set)   |
| `Slice(start, end)`    | `Seq(Integer, Integer)` | Returns a new sequence with elements from start (inclusive) to end (exclusive)        |
| `Clone()`              | `Seq()`                 | Returns a shallow copy of the sequence                                                |
| `Sort()`               | `Void()`                | Stable ascending default sort; see the comparison notes below                         |
| `SortDesc()`           | `Void()`                | Stable descending default sort; see the comparison notes below                        |
| `Filter(pred)`         | `Seq(Function)`         | Returns new Seq with elements where predicate returns true                            |
| `Reject(pred)`         | `Seq(Function)`         | Returns new Seq excluding elements where predicate returns true                       |
| `Map(fn)`              | `Seq(Function)`         | Returns new Seq with each element transformed by function                             |
| `All(pred)`            | `Boolean(Function)`     | Returns true if all elements satisfy predicate (true for empty)                       |
| `Any(pred)`            | `Boolean(Function)`     | Returns true if any element satisfies predicate (false for empty)                     |
| `None(pred)`           | `Boolean(Function)`     | Returns true if no elements satisfy predicate (true for empty)                        |
| `CountWhere(pred)`     | `Integer(Function)`     | Returns count of elements satisfying predicate                                        |
| `FindWhereOption(pred)`| `Option[Object](Function)` | Returns `Some(value)` for the first matching element, including stored nulls, or `None` |
| `Take(n)`              | `Seq(Integer)`          | Returns new Seq with first n elements                                                 |
| `Drop(n)`              | `Seq(Integer)`          | Returns new Seq skipping first n elements                                             |
| `TakeWhile(pred)`      | `Seq(Function)`         | Returns new Seq with leading elements while predicate is true                         |
| `DropWhile(pred)`      | `Seq(Function)`         | Returns new Seq skipping leading elements while predicate is true                     |
| `Fold(init, fn)`       | `Object(Object, Function)` | Reduces sequence to single value using accumulator                                 |
| `GetStr(index)`        | `String(Integer)`         | Returns the element at index as a string (convenience)                                |
| `ToList()`             | `List()`                  | Returns elements as a new List                                                        |
| `ToSet()`              | `Set()`                   | Returns unique elements as a new Set                                                  |
| `ToStack()`            | `Stack()`                 | Returns elements as a new Stack                                                       |
| `ToQueue()`            | `Queue()`                 | Returns elements as a new Queue                                                       |
| `ToDeque()`            | `Deque()`                 | Returns elements as a new Deque                                                       |
| `ToStringSet()`        | `StringSet()`           | Returns raw or boxed string elements as a new StringSet                               |

### Notes

- Public `Seq` constructors (`new Seq()`, `Seq.New()`, `Seq.NewSized(size)`, and `Seq.WithCapacity(cap)`) create owning sequences, so pushed strings and objects remain valid until removed or the sequence is released.
- `Seq.NewSized(size)` creates a sequence with `Count == size` and null-initialized slots (`Seq.New()` takes no arguments). Use `Seq.WithCapacity(cap)` to reserve capacity without changing the count.
- Negative sizes trap; capacity values below 1 are clamped to one slot.
- The lower-level C helpers `rt_seq_new` and `rt_seq_with_capacity` still create borrowed-element sequences for internal runtime views; ownership mode must be selected while the sequence is empty.
- `Pop()` and `RemoveAt(index)` return an owned object reference. When the sequence owns elements, the removed element's retained reference is transferred to the caller.
- `Get()`, `Peek()`, `First()`, `Last()`, and `FindWhereOption()` return borrowed references. Keep the
  sequence (and the selected entry) alive while using them. `GetStr()` instead returns an owned
  string handle.
- `Has()` and `FindOption()` use the same boxed-value equality as List: boxed integers, booleans, floats,
  and strings compare by value; other objects compare by identity. Queue, Stack, Deque, and Ring
  membership uses this same relation.
- `Slice()`, `Filter()`, `Reject()`, `Take()`, and `TakeWhile()` preserve owned-element mode in the returned sequence when the source sequence owns its elements; `Map()` always returns an owning output sequence.
- `ToStringSet()` accepts raw runtime strings and boxed strings; any other element type traps.
- `Push`, `PushAll`, and capacity growth trap on length or allocation overflow instead of wrapping.
- The default Seq comparator matches List: raw and boxed strings order lexicographically and
  boxed integers/booleans/floats numerically, so ordinary Zia/BASIC sequences sort
  alphabetically. Elements rank by type class first (null < numeric < string < other), which
  keeps the order total even for mixed-type sequences.
- `FindOption()` and `FindWhereOption()` are the only lookup accessors; both report a miss as `None` rather than a sentinel value.

### Zia Example

```zia
module SeqDemo;

bind Zanna.Terminal;
bind Zanna.Collections;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var seq = new Seq();

    // Stack-like operations (stores boxed values)
    seq.Push("first");
    seq.Push("second");
    seq.Push("third");
    Say("Count: " + Fmt.Int(seq.Count));                     // 3

    // Peek and Pop (LIFO)
    Say("Peek: " + Zanna.Core.Box.ToStr(seq.Peek()));            // third
    Say("Pop: " + Zanna.Core.Box.ToStr(seq.Pop()));              // third
    Say("After pop: " + Fmt.Int(seq.Count));                   // 2

    // Reverse in place
    seq.Reverse();
    Say("Reversed first: " + Zanna.Core.Box.ToStr(seq.Get(0)));  // second
}
```

### BASIC Example

```basic
DIM seq AS Zanna.Collections.Seq
seq = NEW Zanna.Collections.Seq()

' Add elements (stack-like)
seq.Push(item1)
seq.Push(item2)
seq.Push(item3)

PRINT seq.Count      ' Output: 3
PRINT seq.IsEmpty  ' Output: False

' Access elements by index
DIM first AS Object
first = seq.Get(0)

' Modify by index
seq.Set(1, newItem)

' Stack operations
DIM last AS Object
last = seq.Pop()   ' Removes and returns item3
last = seq.Peek()  ' Returns item2 without removing

' Insert and remove at arbitrary positions
seq.Insert(0, itemAtStart)  ' Insert at beginning
seq.Remove(1)               ' Remove second element

' Search
IF seq.Has(someItem) THEN
    DIM found AS OBJECT = seq.FindOption(someItem)
    IF found.IsSome THEN
        PRINT "Found at index: "; found.UnwrapI64()
    END IF
END IF

' Slicing
DIM slice AS Zanna.Collections.Seq
slice = seq.Slice(1, 3)  ' Elements at indices 1 and 2

' Clone for independent copy
DIM copy AS Zanna.Collections.Seq
copy = seq.Clone()

' Reverse in place
seq.Reverse()

' Push all elements from another sequence
DIM other AS Zanna.Collections.Seq
other = NEW Zanna.Collections.Seq()
other.Push(item4)
other.Push(item5)
seq.PushAll(other)

' Deterministic shuffle (Random.Seed influences Shuffle)
Zanna.Math.Random.Seed(1)
seq.Shuffle()

' Clear all
seq.Clear()
```

### Creating with Initial Count or Capacity

Use `Seq.NewSized(size)` when you want indexed slots immediately, and `Seq.WithCapacity(cap)` when you only want to reserve append space:

```basic
DIM seq AS Zanna.Collections.Seq
seq = Zanna.Collections.Seq.NewSized(1000)
seq.Set(0, firstItem)

DIM buffered AS Zanna.Collections.Seq
buffered = Zanna.Collections.Seq.WithCapacity(1000)

' No reallocations needed for first 1000 pushes
FOR i = 1 TO 1000
    buffered.Push(items(i))
NEXT i
```

### Sorting

`Seq.Sort()` is suitable only when the sequence is known to contain raw runtime string handles or
when its pointer fallback is explicitly acceptable. For ordinary boxed strings, convert through
List, whose comparator understands boxed strings:

```basic
DIM names AS Zanna.Collections.Seq
names = NEW Zanna.Collections.Seq()
names.Push("Charlie")
names.Push("Alice")
names.Push("Bob")

DIM sortable AS Zanna.Collections.List
sortable = names.ToList()

' List recognizes the boxed string values.
sortable.Sort()
names = sortable.ToSeq()
' names is now: Alice, Bob, Charlie

sortable.SortDesc()
names = sortable.ToSeq()
' names is now: Charlie, Bob, Alice
```

### Functional Operations

Seq provides functional-style operations for filtering, transforming, and querying elements.

The callback-taking operations use C-ABI function-pointer slots. The Zia and BASIC frontends accept
`&FunctionName` and `ADDRESSOF FunctionName`, respectively, but the current `zanna run` VM cannot
dispatch those language callbacks through the C slots. Use explicit loops for code that must run on
the VM. The following shows the accepted BASIC callback shape; it is not a VM-runnable example.

```basic
FUNCTION IsEven(item AS OBJECT) AS BOOLEAN
    IsEven = (Zanna.Core.Box.ToI64(item) MOD 2 = 0)
END FUNCTION

FUNCTION DoubleValue(item AS OBJECT) AS OBJECT
    DoubleValue = Zanna.Core.Box.I64(Zanna.Core.Box.ToI64(item) * 2)
END FUNCTION

FUNCTION AddValues(accumulator AS OBJECT, item AS OBJECT) AS OBJECT
    AddValues = Zanna.Core.Box.I64(Zanna.Core.Box.ToI64(accumulator) + Zanna.Core.Box.ToI64(item))
END FUNCTION

DIM numbers AS Zanna.Collections.Seq
numbers = NEW Zanna.Collections.Seq()
' ...populate numbers with boxed integers...
FOR i = 1 TO 10
    numbers.Push(Zanna.Core.Box.I64(i))
NEXT i

DIM evens AS Zanna.Collections.Seq
evens = Zanna.Collections.Seq.Filter(numbers, ADDRESSOF IsEven)

DIM doubled AS Zanna.Collections.Seq
doubled = Zanna.Collections.Seq.Map(numbers, ADDRESSOF DoubleValue)

DIM evenCount AS INTEGER = Zanna.Collections.Seq.CountWhere(numbers, ADDRESSOF IsEven)

DIM firstEvenOpt AS OBJECT = Zanna.Collections.Seq.FindWhereOption(numbers, ADDRESSOF IsEven)
DIM firstEven AS INTEGER = Zanna.Core.Box.ToI64(Zanna.Option.Unwrap(firstEvenOpt))

DIM sumBox AS OBJECT
sumBox = Zanna.Collections.Seq.Fold(numbers, Zanna.Core.Box.I64(0), ADDRESSOF AddValues)
DIM sum AS INTEGER = Zanna.Core.Box.ToI64(sumBox)            ' 55
```

### Use Cases

- **Stack:** Use `Push()` and `Pop()` for LIFO operations
- **FIFO queues:** Use the dedicated `Zanna.Collections.Queue`; `Seq.Pop()` always removes the last element
- **Dynamic Array:** Use `Get()`/`Set()` for random access
- **Slicing:** Use `Slice()` to extract sub-sequences
- **Filtering:** Use `Filter()` and `Reject()` to filter by condition
- **Transformation:** Use `Map()` to transform all elements
- **Queries:** Use `All()`, `Any()`, `None()` for predicate checks
- **Aggregation:** Use `Fold()` to reduce to a single value

---

## Zanna.Functional.LazySeq

A lazy sequence that generates elements on demand rather than storing yielded history. Its public factories create
integer ranges and finite or infinite repetitions; pipelines can then bound, combine, transform, or collect those
sources.

> **Note:** The canonical namespace is `Zanna.Functional.LazySeq`, not `Zanna.Collections.LazySeq`.

**Type:** Instance (obj)
**Constructors:**

- `Zanna.Functional.LazySeq.Range(start, end, step)` - Generate integer range
- `Zanna.Functional.LazySeq.Repeat(value, count)` - Repeat value count times (-1 for infinite)

### Properties

| Property      | Type    | Description                                        |
|---------------|---------|----------------------------------------------------|
| `Index`       | Integer | Current position (number of elements consumed)     |
| `IsExhausted` | Boolean | True after a read has observed end-of-sequence      |

### Methods

#### Element Access

| Method    | Signature       | Description                                             |
|-----------|-----------------|---------------------------------------------------------|
| `Next()`  | `Object()`      | Get next borrowed element and advance; returns null if exhausted |
| `Peek()`  | `Object()`      | Get the next borrowed element without advancing              |
| `Reset()` | `Void()`        | Clear cursor/exhaustion state; does not rewind `Range` or finite `Repeat` |

#### Transformations (return new LazySeq)

| Method             | Signature                    | Description                                      |
|--------------------|------------------------------|--------------------------------------------------|
| `Map(fn)`          | `LazySeq(Function)`          | Transform each element with function             |
| `Filter(pred)`     | `LazySeq(Function)`          | Keep only elements where predicate returns true  |
| `Take(n)`          | `LazySeq(Integer)`           | Take first n elements; a negative limit returns null |
| `Drop(n)`          | `LazySeq(Integer)`           | Skip first n elements; a negative limit returns null |
| `TakeWhile(pred)`  | `LazySeq(Function)`          | Take elements while predicate is true            |
| `DropWhile(pred)`  | `LazySeq(Function)`          | Skip elements while predicate is true            |
| `Concat(other)`    | `LazySeq(LazySeq)`           | Concatenate with another lazy sequence           |

#### Collectors (consume sequence)

| Method         | Signature                    | Description                                            |
|----------------|------------------------------|--------------------------------------------------------|
| `ToSeq()`      | `Seq()`                      | Collect all elements into a Seq (may not terminate!)   |
| `Count()`      | `Integer()`                  | Count all elements (may not terminate!)                |
| `Find(pred)`   | `Object(Function)`           | Find first matching element; null if not found         |
| `Any(pred)`    | `Boolean(Function)`          | True if any element matches                            |
| `All(pred)`    | `Boolean(Function)`          | True if all elements match (may not terminate!)        |

### Zia Example

```zia
module LazySeqDemo;

bind Zanna.Functional.LazySeq as LS;
bind Zanna.Core.Box as Box;
bind Zanna.Terminal;

func start() {
    // Range is half-open: this counts 1 through 10.
    var numbers = LS.Range(1, 11, 1);
    SayInt(numbers.Count());

    // Repeat borrows the object supplied by the caller, so keep it live.
    var seven = Box.I64(7);
    var values = LS.Repeat(seven, 3);
    SayInt(Box.ToI64(values.Peek()));
    SayInt(values.Index);                    // 0: Peek did not consume it
    SayInt(Box.ToI64(values.Next()));
    SayInt(values.Index);                    // 1

    // Bound an infinite source before collecting it.
    var x = Box.Str("x");
    var xs = LS.Repeat(x, -1).Take(2).ToSeq();
    SayInt(xs.Count);                        // 2
}
```

### BASIC Example

```basic
DIM numbers AS Zanna.Functional.LazySeq
numbers = Zanna.Functional.LazySeq.Range(1, 11, 1)
PRINT numbers.Count()                                      ' 10

DIM values AS Zanna.Functional.LazySeq
DIM seven AS OBJECT = Zanna.Core.Box.I64(7)
values = Zanna.Functional.LazySeq.Repeat(seven, 3)
PRINT Zanna.Core.Box.ToI64(values.Peek())                  ' 7
PRINT values.Index                                         ' 0
PRINT Zanna.Core.Box.ToI64(values.Next())                  ' 7
PRINT values.Index                                         ' 1

DIM forever AS Zanna.Functional.LazySeq
DIM x AS OBJECT = Zanna.Core.Box.Str("x")
forever = Zanna.Functional.LazySeq.Repeat(x, -1)
DIM firstTwo AS Zanna.Collections.Seq
firstTwo = forever.Take(2).ToSeq()
PRINT firstTwo.Count                                       ' 2
```

### LazySeq vs Seq

| Feature               | LazySeq              | Seq                  |
|-----------------------|----------------------|----------------------|
| Retained elements     | None; collectors also borrow | All elements in public constructors |
| Element generation    | On demand            | Upfront              |
| Infinite sequences    | Supported            | Not possible         |
| Random access         | No (sequential only) | O(1)                 |
| Multiple iterations   | Recreate the source  | Automatic            |
| Chain transformations | Deferred             | Immediate            |

### Important Notes

- **Infinite sequences:** Methods like `ToSeq()`, `Count()`, and `All()` may never
  terminate on infinite sequences. Always use `Take(n)` to bound infinite sequences before
  collecting.
- **Single-pass:** LazySeq is consumed as you iterate. Recreate `Range` and finite `Repeat`
  sources for another pass. `ToSeq()` materializes the remaining values but does not take element
  ownership. `Reset()` clears cursor flags but does not restore either source's original range
  position or repeat count.
- **Shared cursors:** A transformation retains its source LazySeq object, but it does not clone its
  cursor. Consuming a derived pipeline also advances the original source and any sibling pipeline.
  Concatenating a source with itself therefore does not replay it a second time.
- **Ownership:** `Repeat` retains its value for the node's lifetime, and `ToSeq()`
  returns an owning Seq, so collected values are independently long-lived. `Next()` /
  `Peek()` still return borrowed references bounded by the source's lifetime.
- **Null ambiguity:** `Repeat(null, count)` is accepted, and `Next()` / `Peek()` also use null
  to report exhaustion. Use `NextOption()` / `PeekOption()` when null is a legitimate element:
  they return `Some(value)` for every yielded element (including null) and `None` only at
  end of stream.
- **Range values:** `Range` currently exposes each integer through transient internal storage rather
  than as a boxed runtime object. `Count()` and other operations that only count/skip values are
  safe, but do not retain, unbox, collect, or pass `Range` elements to object callbacks. Use
  `Repeat()` with explicitly boxed values when object-valued elements are required.
- **Transformation chaining:** Transformations like `Map()`, `Filter()`, `Take()` return new
  LazySeq instances and do no work until elements are requested.
- **Bounds and factory validation:** invalid bounds trap: a zero `Range` step, a negative
  `Take`/`Drop` limit, and any repeat count below `-1` (only `-1` means an infinite repeat).
- **Filtering infinite sources:** putting `Take(n)` after a filter does not bound how many source
  elements the filter may inspect. An infinite source whose predicate never matches still does not
  yield or terminate.
- **Language callbacks:** The same VM callback limitation described for `Seq` applies to
  `Map()`, `Filter()`, `TakeWhile()`, `DropWhile()`, `Find()`, `Any()`, and `All()`.
- **Explicit receiver safety:** Qualified calls such as
  `Zanna.Functional.LazySeq.Reset(value)` validate the receiver's class and trap with
  `LazySeq: invalid LazySeq object` when passed anything that is not a LazySeq.

### Use Cases

- **Large numeric ranges:** Count or skip values without storing the whole range
- **Infinite repeated values:** Create an unbounded source with `Repeat(value, -1)`, then bound it with `Take`
- **Early termination:** Stop an object-valued repeated source without collecting every value
- **Pipeline processing:** Chain transformations efficiently
- **Generator patterns:** Produce values on demand

---

## Zanna.Collections.Iterator

A sequential cursor over a collection that provides controlled traversal with peek-ahead, skip, and reset capabilities.

**Type:** Instance (obj)

### Factories

| Factory | Source behavior |
|---------|-----------------|
| `FromSeq(seq)` | Live indexed view; length is captured at creation |
| `FromList(list)` | Live indexed view; length is captured at creation |
| `FromRing(ring)` | Live indexed view; length is captured at creation |
| `FromDeque(deque)` | Snapshot |
| `FromMapKeys(map)` | Snapshot of keys in unspecified hash-table order |
| `FromMapValues(map)` | Snapshot of values in the corresponding hash-table order |
| `FromSet(set)` | Snapshot in unspecified hash order |
| `FromStack(stack)` | Snapshot in bottom-to-top order; source stack is unchanged |

### Properties

| Property | Type    | Description                                      |
|----------|---------|--------------------------------------------------|
| `Index`  | Integer | Current position (number of elements consumed)   |
| `Count`  | Integer | Number of elements captured when the iterator was created |

### Methods

| Method      | Signature        | Description                                                 |
|-------------|------------------|-------------------------------------------------------------|
| `HasNext()` | `Boolean()`      | Returns true if there are more elements to iterate          |
| `Next()`    | `Object()`       | Return the current element and advance the cursor           |
| `Peek()`    | `Object()`       | Return the current element without advancing                |
| `Reset()`   | `Void()`         | Reset the cursor to the beginning                           |
| `ToSeq()`   | `Seq()`          | Collect all remaining elements into a new Seq               |
| `Skip(n)`   | `Integer(Integer)` | Skip forward by n elements; returns actual number skipped |

### Notes

- `Next()` and `Peek()` return owned object references. In Zia these are usually boxed values; use `Box.ToStr()`, `Box.ToI64()`, etc. to unwrap
- In BASIC, `Next()` and `Peek()` return values directly
- `Skip(n)` returns the actual number of elements skipped, which may be less than n if the iterator is near the end
- `ToSeq()` collects only the *remaining* elements from the current position onward and advances the iterator to its end
- `ToSeq()` is registered as a typed sequence return, so chains such as `it.ToSeq().Count`
  resolve against the returned Seq.
- Stack iterators snapshot the stack in bottom-to-top order and restore the source stack unchanged
- Seq, List, and Ring iterators retain a live source reference but cache its length. Avoid changing a live source while
  iterating: inserted elements do not extend `Count`, and shrinking a source can invalidate a cached position.
- Do not store a live iterator back into its owning Seq/List/Ring. That creates a retained cycle
  which the current Iterator implementation does not register with cycle traversal (VDOC-094).

### Zia Example

```zia
module IteratorDemo;

bind Zanna.Collections;
bind Zanna.Core;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var seq = Seq.New();
    seq.Push(Box.Str("a"));
    seq.Push(Box.Str("b"));
    seq.Push(Box.Str("c"));
    seq.Push(Box.Str("d"));
    seq.Push(Box.Str("e"));

    var it: Zanna.Collections.Iterator = Iterator.FromSeq(seq);
    SayInt(it.Count);                         // 5

    // Peek without advancing
    Say(Box.ToStr(it.Peek()));                // a
    SayInt(it.Index);                         // 0

    // Next advances the cursor
    Say(Box.ToStr(it.Next()));                // a
    Say(Box.ToStr(it.Next()));                // b
    SayInt(it.Index);                         // 2

    // Skip forward
    SayInt(it.Skip(2));                       // 2 (skipped c, d)
    Say(Box.ToStr(it.Peek()));                // e

    // Collect remaining
    it.Reset();
    it.Next();   // consume a
    it.Next();   // consume b
    var rest: Seq = it.ToSeq();
    SayInt(rest.Count);                         // 3 (c, d, e)
}
```

### BASIC Example

```basic
DIM seq AS Zanna.Collections.Seq
seq = Zanna.Collections.Seq.New()
seq.Push("a")
seq.Push("b")
seq.Push("c")
seq.Push("d")
seq.Push("e")

DIM it AS Zanna.Collections.Iterator
it = Zanna.Collections.Iterator.FromSeq(seq)
PRINT it.Count          ' 5

' Peek without advancing
PRINT it.Peek()          ' a
PRINT it.Index           ' 0

' Next advances
PRINT it.Next()          ' a
PRINT it.Index           ' 1
PRINT it.Next()          ' b
PRINT it.Index           ' 2

' Peek after Next
PRINT it.Peek()          ' c

' Skip forward
PRINT it.Skip(2)         ' 2 (skipped c, d)
PRINT it.Index           ' 4
PRINT it.Peek()          ' e

' Iterate to exhaustion
PRINT it.Next()          ' e
PRINT it.HasNext()       ' 0

' Reset to beginning
it.Reset()
PRINT it.Index           ' 0
PRINT it.Peek()          ' a

' Collect remaining after consuming some
it.Next()   ' consume a
it.Next()   ' consume b
DIM rest AS Zanna.Collections.Seq
rest = it.ToSeq()
PRINT rest.Count           ' 3 (c, d, e)
PRINT rest.Get(0)        ' c
PRINT rest.Get(1)        ' d
PRINT rest.Get(2)        ' e
```

### Use Cases

- **Parser input:** Iterate over tokens with peek-ahead for lookahead parsing
- **Streaming processing:** Process elements one at a time with position tracking
- **Batch processing:** Skip forward to a specific position, then collect a batch with `ToSeq`
- **Repeated traversal:** Use `Reset()` to return an iterator cursor to position zero

---


## See Also

- [Sequential Collections](sequential.md)
- [Maps & Sets](maps-sets.md)
- [Specialized Maps](multi-maps.md)
- [Specialized Structures](specialized.md)
- [Collections Overview](README.md)
- [Zanna Runtime Library](../README.md)
