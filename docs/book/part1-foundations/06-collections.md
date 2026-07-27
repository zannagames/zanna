---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 6: Collections

You're building a student grade tracker. You need to store grades for 30 students. With what we know so far, you'd create 30 variables:

```zia
var grade1 = 85;
var grade2 = 92;
var grade3 = 78;
// ... 27 more variables ...
var grade30 = 91;
```

Then to find the average, you'd add them all up manually:

```zia
var sum = grade1 + grade2 + grade3 + /* ... */ grade30;
var average = sum / 30;
```

What if you have 100 students? 1000? What if you don't know how many students until the program runs?

There must be a better way.

There is. *Collections* let you store multiple values under a single name. The most fundamental collection is the *array* — and understanding arrays is one of the most important steps in learning to program.

---

## Why Do We Need Collections?

Think about the data you encounter in everyday life. It's rarely a single, isolated value. It's usually a *list* of things:

**A shopping list:**
- Milk
- Bread
- Eggs
- Butter
- Apples

**High scores in a game:**
1. 15,000 points
2. 12,500 points
3. 11,200 points
4. 10,800 points
5. 9,500 points

**Your contacts:**
- Mom: 555-1234
- Dad: 555-5678
- Best Friend: 555-9999
- Doctor: 555-4321

**Temperatures for the week:**
- Monday: 72F
- Tuesday: 68F
- Wednesday: 75F
- Thursday: 71F
- Friday: 69F
- Saturday: 73F
- Sunday: 70F

**Inventory in a game:**
- Sword
- Shield
- Health Potion
- Health Potion
- Gold Key

In every case, you have *multiple related values* that belong together. You could create separate variables for each:

```zia
var item1 = "Sword";
var item2 = "Shield";
var item3 = "Health Potion";
var item4 = "Health Potion";
var item5 = "Gold Key";
```

But this approach has serious problems:

1. **It's tedious.** Imagine typing out 100 variable names.

2. **You can't easily process all values.** Want to print every item? You need to write a print statement for each one.

3. **You can't add items dynamically.** What happens when the player picks up a sixth item? You'd need to create `item6`, but that name doesn't even exist in your code.

4. **You can't loop through them.** There's no way to say "do this for every item" because `item1`, `item2`, etc. are completely separate variables with no connection.

5. **The number is fixed.** You have to decide how many variables to create when you write the code, not when the program runs.

Collections solve all of these problems. An array lets you store any number of values under a single name, access them by position, and process them all with a simple loop.

---

## What Is an Array?

An array is a numbered list of values stored under a single name. Instead of having separate boxes labeled `score1`, `score2`, `score3`, you have one big row of boxes labeled `scores`, and each box has a number.

```zia
var scores = [85, 92, 78, 95, 88];
```

This single line creates an array named `scores` containing five numbers. The square brackets `[ ]` define the array, and the values are separated by commas.

### Mental Model: The Mailbox Metaphor

Imagine an apartment building with a row of mailboxes in the lobby. Each mailbox is identical in size and shape, but each has a different number. The mailboxes are numbered 0, 1, 2, 3, 4 (we'll explain why they start at 0 shortly).

```text
   scores
   +-----+-----+-----+-----+-----+
   |  85 |  92 |  78 |  95 |  88 |
   +-----+-----+-----+-----+-----+
     [0]   [1]   [2]   [3]   [4]
```

The array name `scores` refers to the entire row. The number in brackets (the *index*) tells you which specific box to look in. If you want the value in box 2, you say `scores[2]` and get 78.

### Mental Model: Train Cars

Think of an array as a train. The entire train has one name (like "Express 42"), but each car has a number. Car 0 is at the front, then car 1, car 2, and so on. Each car holds one piece of cargo (one value).

```text
    Engine -> [Car 0] -> [Car 1] -> [Car 2] -> [Car 3] -> [Car 4]
               (85)       (92)       (78)       (95)       (88)
```

To access cargo, you specify which car: "Get me the cargo from car 2 of the Express 42 train."

### Mental Model: School Lockers

Picture a hallway of lockers in a school. The lockers are all in a row, numbered 0, 1, 2, 3, and so on. Each locker can hold whatever the student puts in it. When you want to find something, you go to a specific locker number.

```text
   +--------+--------+--------+--------+--------+
   |   85   |   92   |   78   |   95   |   88   |
   | Locker | Locker | Locker | Locker | Locker |
   |   #0   |   #1   |   #2   |   #3   |   #4   |
   +--------+--------+--------+--------+--------+
```

### Mental Model: Spreadsheet Row

If you've used a spreadsheet like Excel or Google Sheets, think of an array as a single row. Each cell in the row is identified by its column number (starting from 0).

```text
     A     B     C     D     E
   +-----+-----+-----+-----+-----+
1  |  85 |  92 |  78 |  95 |  88 |
   +-----+-----+-----+-----+-----+
```

Column A is index 0, column B is index 1, and so on. The array is the row; the index is the column number.

---

## Accessing Array Elements

To access an individual element, use the array name followed by the index in square brackets:

```zia
bind Zanna.Terminal;

var scores = [85, 92, 78, 95, 88];

Say(scores[0]);  // 85 (first element)
Say(scores[1]);  // 92 (second element)
Say(scores[2]);  // 78 (third element)
Say(scores[3]);  // 95 (fourth element)
Say(scores[4]);  // 88 (fifth element)
```

Notice something important: **indices start at 0, not 1.** The first element is at index 0, the second at index 1, and so on. For an array of 5 elements, valid indices are 0, 1, 2, 3, and 4. There is no index 5 — that would be asking for a sixth element that doesn't exist.

---

## Why Zero-Based Indexing?

This is one of the most confusing aspects of programming for beginners. Why not just start at 1 like we count in everyday life?

The answer lies in how computers actually work with memory. When you create an array, the computer sets aside a contiguous block of memory — a sequence of adjacent storage locations. Each element takes up some amount of space (say, 8 bytes for an integer).

To find where an element is stored, the computer needs to calculate:

```text
element address = starting address + (index x element size)
```

If the array starts at memory address 1000, and each element is 8 bytes:
- Element 0 is at address 1000 + (0 x 8) = 1000
- Element 1 is at address 1000 + (1 x 8) = 1008
- Element 2 is at address 1000 + (2 x 8) = 1016

The index is an *offset* — how far to jump from the start. The first element is zero steps away from the start, so its index is 0. The second element is one step away, so its index is 1.

Think of it this way: if you're standing at the start of a row of lockers and someone says "go forward 0 steps," you're already at the first locker. "Go forward 1 step" puts you at the second locker. The number tells you how many positions to move, not which position you end up at.

### The Fence Post Analogy

Imagine a fence with posts and the spaces between them:

```text
   |   |   |   |   |   |
   0   1   2   3   4   5   <- post numbers
     0   1   2   3   4     <- space numbers
```

If you number the posts starting at 0, then the space between post N and post N+1 is space N. The numbers align perfectly. If you started numbering posts at 1, things get complicated.

In programming, we often need to count both items and the gaps between them, or convert between positions and offsets. Starting at 0 makes these calculations clean and consistent.

### Just Memorize It

Honestly, the historical and mathematical reasons matter less than the practical reality: **nearly every programming language uses zero-based indexing.** C, C++, Java, JavaScript, Python, Rust, Go, Swift — they all start at 0.

The sooner you internalize this, the easier programming becomes. Here's the key formula:

> For an array of N elements, valid indices go from 0 to N-1.

An array of 5 elements has indices 0, 1, 2, 3, 4.
An array of 100 elements has indices 0, 1, 2, ..., 98, 99.
An array of 1 element has only index 0.

### Practice Makes Perfect

The zero-based indexing confusion fades with practice. After you've written a few programs with arrays, you'll start thinking in zero-based terms naturally. Until then, just pause and think:

- **First** element = index 0
- **Second** element = index 1
- **Third** element = index 2
- **Nth** element = index N-1
- **Last** element = index (length - 1)

---

## When to Use Arrays vs. Individual Variables

Now you might wonder: when should I use an array versus individual variables?

**Use individual variables when:**

1. **You have a fixed, small number of distinct things.** A game character might have `health`, `mana`, and `stamina` — three different attributes that serve different purposes. Using an array `[100, 50, 75]` would be confusing because you'd have to remember that index 0 is health, index 1 is mana, and so on.

2. **The values represent different concepts.** A `width` and a `height` are both numbers, but they mean different things. Separate variables make the code clearer:
   ```zia
   var width = 800;
   var height = 600;
   // Much clearer than dimensions[0] and dimensions[1]
   ```

3. **You need different types.** A person might have a `name` (String), `age` (number), and `isStudent` (boolean). Arrays typically hold items of the same type.

**Use arrays when:**

1. **You have multiple values of the same kind.** Test scores, prices, names, coordinates — whenever you have a list of similar items.

2. **The number of values might change.** Users might add or remove items. An inventory might grow as players collect items.

3. **You want to process all values the same way.** If you need to find the total, average, maximum, or print every value, arrays let you do that with a loop.

4. **You don't know how many values until runtime.** The user might enter 5 numbers or 500. An array can grow as needed.

5. **Position matters but names don't.** The 5th score in a list doesn't need its own name — its position identifies it.

Here's a good rule of thumb: if you find yourself creating `thing1`, `thing2`, `thing3`, stop and use an array instead.

---

## Creating Arrays

There are several ways to create arrays, depending on what you need.

### With Initial Values

The most common way is to list the values inside square brackets:

```zia
var names = ["Alice", "Bob", "Carol"];
var primes = [2, 3, 5, 7, 11, 13];
var flags = [true, false, true];
var temperatures = [72.5, 68.3, 75.1, 71.8];
```

The array's type is inferred from the values. An array of strings, an array of integers, an array of booleans, an array of floats.

### Empty Array

Sometimes you want to start with nothing and add items later:

```zia
var numbers: List[Integer] = [];  // Empty array of integers
var words: List[String] = []; // Empty array of strings
```

When creating an empty array, you must specify the type because Zanna can't infer it from non-existent values. The `: List[Integer]` part says "this will hold 64-bit integers."

### Repeating a Value

If you need many copies of the same value, build the array with a loop:

```zia
var zeros: List[Integer] = [];
for i in 0..10 {
    zeros.Push(0);           // Ten zeros
}
```

This is especially useful for initializing game boards, buffers, or any structure where you need a known size filled with default values.

---

## Array Length

Every array knows how many elements it contains. Access this with the `.Length` property:

```zia
bind Zanna.Terminal;

var names = ["Alice", "Bob", "Carol"];
Say(names.Length);  // 3

var primes = [2, 3, 5, 7, 11, 13, 17, 19];
Say(primes.Length);  // 8

var empty: List[Integer] = [];
Say(empty.Length);   // 0
```

The length is crucial for working with arrays safely. The valid indices always go from 0 to `length - 1`. Trying to access any index outside this range is an error.

```zia
var scores = [85, 92, 78, 95, 88];
// Valid indices: 0, 1, 2, 3, 4
// scores.Length is 5
// Last valid index is scores.Length - 1 = 4
```

---

## Modifying Array Elements

Arrays are mutable — you can change individual elements after creation:

```zia
bind Zanna.Terminal;

var scores = [85, 92, 78, 95, 88];

// Change the third element (index 2) from 78 to 82
scores[2] = 82;

Say(scores[2]);  // 82

// Change multiple elements
scores[0] = 90;
scores[4] = 91;
```

You can also use an element's current value in the calculation:

```zia
bind Zanna.Terminal;

var counts = [10, 20, 30];

// Add 5 to the second element
counts[1] = counts[1] + 5;

Say(counts[1]);  // 25
```

---

## Adding and Removing Elements

Unlike the fixed arrays in some languages, Zanna arrays can grow and shrink dynamically.

### Adding Elements with push()

The `push` operation adds an element to the end of an array:

```zia
bind Zanna.Terminal;

var names = ["Alice", "Bob"];
Say(names.Length);  // 2

names.Push("Carol");
Say(names.Length);  // 3
Say(names[2]);      // Carol

names.Push("Dave");
names.Push("Eve");
Say(names.Length);  // 5
```

Think of `push` like adding a new car to the end of a train — the train gets longer, and the new car has the next number in sequence.

### Removing Elements with pop()

The `pop` operation removes the last element and returns it:

```zia
bind Zanna.Terminal;

var numbers = [1, 2, 3, 4, 5];
Say(numbers.Length);  // 5

var last = numbers.Pop();  // Removes and returns 5
Say(last);            // 5
Say(numbers.Length);  // 4

var another = numbers.Pop();  // Removes and returns 4
Say(another);         // 4
Say(numbers.Length);  // 3
// numbers is now [1, 2, 3]
```

Pop is like detaching the last car from a train — the train gets shorter, and you get to keep whatever was in that car.

**Warning:** Calling `pop` on an empty array is an error — there's nothing to remove!

```zia
var empty: List[Integer] = [];
var x = empty.Pop();  // Error! Can't pop from empty array
```

Always check that the array has elements before popping:

```zia
var numbers = [1, 2, 3];
if numbers.Length > 0 {
    var last = numbers.Pop();
    // Safe to use last
}
```

### Building Arrays Dynamically

These operations let you build arrays piece by piece:

```zia
bind Zanna.Terminal;

var userInputs: List[Integer] = [];

Say("Enter numbers (0 to stop):");

while true {
    var num = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));
    if num == 0 {
        break;
    }
    userInputs.Push(num);
}

Say("You entered " + userInputs.Length + " numbers.");
```

You start with an empty array and grow it as the user provides input. You couldn't do this with fixed variables like `num1`, `num2`, etc.

---

## Finding Elements

A common task is searching for a value in an array.

### Linear Search

The simplest approach is to check each element one by one:

```zia
bind Zanna.Terminal;

var names = ["Alice", "Bob", "Carol", "Dave", "Eve"];
var target = "Carol";
var foundIndex = -1;  // -1 means "not found"

for i in 0..names.Length {
    if names[i] == target {
        foundIndex = i;
        break;  // Found it, no need to keep looking
    }
}

if foundIndex >= 0 {
    Say("Found '" + target + "' at index " + foundIndex);
} else {
    Say("'" + target + "' not found");
}
```

Output: `Found 'Carol' at index 2`

This is called *linear search* because you go through the array in a line, one element at a time. It's simple and works for any array, but for very large arrays (millions of elements), it can be slow.

### Checking If an Element Exists

Sometimes you just need to know yes or no, not where:

```zia
bind Zanna.Terminal;

var numbers = [10, 25, 30, 45, 50];
var lookingFor = 30;
var found = false;

for n in numbers {
    if n == lookingFor {
        found = true;
        break;
    }
}

if found {
    Say("Found it!");
} else {
    Say("Not in the list.");
}
```

### Counting Occurrences

How many times does a value appear?

```zia
bind Zanna.Terminal;

var grades = ["A", "B", "A", "C", "A", "B", "A", "D"];
var countA = 0;

for grade in grades {
    if grade == "A" {
        countA = countA + 1;
    }
}

Say("Number of A grades: " + countA);  // 4
```

---

## Sorting Arrays

Sorting puts array elements in order — smallest to largest (ascending) or largest to smallest (descending).

### Using Built-in Sort

Zanna provides a built-in sort method:

```zia
bind Zanna.Terminal;

var numbers = [64, 34, 25, 12, 22, 11, 90];
numbers.Sort();

for n in numbers {
    Print(n + " ");
}
// Output: 11 12 22 25 34 64 90
```

For strings, sort orders alphabetically:

```zia
bind Zanna.Terminal;

var names = ["Charlie", "Alice", "Bob", "Diana"];
names.Sort();

for name in names {
    Say(name);
}
// Alice, Bob, Charlie, Diana
```

### Understanding How Sorting Works (Conceptually)

While you can just use `sort()`, understanding the concept helps you appreciate what's happening.

Imagine you have a hand of playing cards and want to sort them. One approach:

1. Find the smallest card, put it first.
2. Find the smallest of what's left, put it second.
3. Repeat until done.

This is called *selection sort*. Here's what it looks like:

```zia
// Selection sort (for understanding — use built-in sort in practice)
var arr = [64, 34, 25, 12, 22];

for i in 0..arr.Length {
    // Find minimum in unsorted portion
    var minIndex = i;
    for j in (i + 1)..arr.Length {
        if arr[j] < arr[minIndex] {
            minIndex = j;
        }
    }
    // Swap it with position i
    var temp = arr[i];
    arr[i] = arr[minIndex];
    arr[minIndex] = temp;
}
```

This is educational but inefficient for large arrays. Always use the built-in `sort()` in real code — it uses much faster algorithms.

---

## Iterating Over Arrays

Processing every element in an array is so common that there are several patterns for it.

### Pattern 1: For-Each Loop (When You Only Need Values)

The cleanest way when you just need each value:

```zia
bind Zanna.Terminal;

var fruits = ["apple", "banana", "cherry", "date"];

for fruit in fruits {
    Say("I enjoy eating " + fruit);
}
```

The variable `fruit` takes on each value in sequence: first "apple", then "banana", then "cherry", then "date".

**Use this when:** You only need to read the values, not modify them or know their positions.

### Pattern 2: Index Loop (When You Need Position)

When you need to know *where* each element is:

```zia
bind Zanna.Terminal;

var scores = [85, 92, 78, 95, 88];

for i in 0..scores.Length {
    Say("Student " + (i + 1) + " scored " + scores[i]);
}
```

Output:
```text
Student 1 scored 85
Student 2 scored 92
Student 3 scored 78
Student 4 scored 95
Student 5 scored 88
```

Notice `i + 1` to display human-friendly numbering (1, 2, 3) while using programmer indexing (0, 1, 2) internally.

**Use this when:**
- You need to display or use the position
- You need to modify elements: `arr[i] = newValue`
- You need to compare adjacent elements: `arr[i]` vs `arr[i+1]`
- You need to access elements from multiple arrays at the same position

### Pattern 3: While Loop with Index

Sometimes you need more control than a for loop provides:

```zia
bind Zanna.Terminal;

var numbers = [10, 20, 30, 40, 50];
var i = 0;

while i < numbers.Length {
    Say("Index " + i + ": " + numbers[i]);
    i = i + 1;
}
```

**Use this when:** You need to skip elements, process in unusual order, or have complex stopping conditions.

### Pattern 4: Reverse Iteration

To process elements from last to first:

```zia
bind Zanna.Terminal;

var countdown = [5, 4, 3, 2, 1];

// Using while loop for reverse
var i = countdown.Length - 1;
while i >= 0 {
    Say(countdown[i]);
    i = i - 1;
}
// Prints: 1, 2, 3, 4, 5 (but we access 5 first, then 4...)
// Wait, that prints the values which happen to be a countdown.
// Let me show a clearer example:

var letters = ["a", "b", "c", "d", "e"];
var j = letters.Length - 1;
while j >= 0 {
    Say(letters[j]);
    j = j - 1;
}
// Prints: e, d, c, b, a
```

### Pattern 5: Processing Every Other Element

Skip elements as needed:

```zia
bind Zanna.Terminal;

var numbers = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];

// Even indices only (0, 2, 4, 6, 8)
var i = 0;
while i < numbers.Length {
    Print(numbers[i] + " ");
    i = i + 2;
}
// Output: 0 2 4 6 8

Say("");

// Odd indices only (1, 3, 5, 7, 9)
i = 1;
while i < numbers.Length {
    Print(numbers[i] + " ");
    i = i + 2;
}
// Output: 1 3 5 7 9
```

### Pattern 6: Process Until Condition

Stop early when you find what you need:

```zia
bind Zanna.Terminal;

var values = [10, 20, -5, 30, 40];

for v in values {
    if v < 0 {
        Say("Found negative number, stopping.");
        break;
    }
    Say("Processing: " + v);
}
// Processes 10, 20, then stops at -5
```

Or skip certain elements:

```zia
bind Zanna.Terminal;

var numbers = [1, -2, 3, -4, 5, -6];

for n in numbers {
    if n < 0 {
        continue;  // Skip negative numbers
    }
    Say("Positive: " + n);
}
// Prints: 1, 3, 5
```

---

## Bounds Checking: What Happens With Invalid Indices?

What happens if you try to access an index that doesn't exist?

```zia
var arr = [10, 20, 30, 40, 50];  // Valid indices: 0-4

// These are all errors:
var x = arr[5];    // Error: index 5 doesn't exist
var y = arr[100];  // Error: way out of bounds
var z = arr[-1];   // Error: negative indices don't exist
```

Zanna performs *bounds checking* — it verifies that every array access is within valid range. If you try to read or write an invalid index, the program stops with a clear error message:

```text
Runtime Error: Array index out of bounds
  Index: 5
  Valid range: 0 to 4
  at line 3 in main.zia
```

This is a safety feature. Without bounds checking, accessing invalid indices would read or write random memory locations, causing mysterious bugs, crashes, or security vulnerabilities.

### Why This Matters

In languages without bounds checking (like C), accessing `arr[100]` when the array has only 5 elements doesn't give an error — it just accesses whatever happens to be in that memory location. This leads to:

1. **Undefined behavior:** The program might work, crash, or produce wrong results randomly.
2. **Security exploits:** Attackers can use out-of-bounds access to hack programs.
3. **Incredibly hard-to-find bugs:** The symptom might appear far from the cause.

Zanna's bounds checking catches these mistakes immediately, making bugs much easier to find and fix.

### Common Causes of Index Errors

**Off-by-one with length:**
```zia
var arr = [1, 2, 3, 4, 5];
var last = arr[arr.Length];  // Error! length is 5, but valid indices are 0-4
var last = arr[arr.Length - 1];  // Correct: gets arr[4] = 5
```

**Empty array access:**
```zia
var empty: List[Integer] = [];
var x = empty[0];  // Error! Array is empty, no index 0
```

**Loop going too far:**

This version is expected to trap at runtime:

```zia
bind Zanna.Terminal;

var arr = [1, 2, 3];
for i in 0..=arr.Length {  // Wrong! 0..= includes the endpoint
    Say(arr[i]);  // Crashes when i=3
}
```

This version uses the half-open range and should run:

```zia
bind Zanna.Terminal;

var arr = [1, 2, 3];
for i in 0..arr.Length {   // Correct! 0.. excludes the endpoint
    Say(arr[i]);  // Works: i goes 0, 1, 2
}
```

**Calculated index going wrong:**
```zia
bind Zanna.Terminal;

var arr = [10, 20, 30];
var userInput = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));
var value = arr[userInput];  // Dangerous! User might enter 999
```

To prevent this, validate indices before using them:

```zia
bind Zanna.Terminal;

var arr = [10, 20, 30];
var userInput = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));

if userInput >= 0 && userInput < arr.Length {
    var value = arr[userInput];
    // Safe to use value
} else {
    Say("Invalid selection");
}
```

---

## Arrays of Strings

Arrays can hold any type. Strings are especially common:

```zia
bind Zanna.Terminal;

var fruits = ["apple", "banana", "cherry", "date", "elderberry"];

Say("Menu:");
for i in 0..fruits.Length {
    Say((i + 1) + ". " + fruits[i]);
}
```

Output:
```text
Menu:
1. apple
2. banana
3. cherry
4. date
5. elderberry
```

String arrays are how you'd store:
- Menu options
- Lines of a file
- Names in a contact list
- Words in a sentence
- Commands in a history

### Working with String Arrays

```zia
bind Zanna.Terminal;

var words = ["The", "quick", "brown", "fox"];

// Join into a sentence
var sentence = "";
for word in words {
    sentence = sentence + word + " ";
}
Say(sentence);  // "The quick brown fox "

// Find longest word
var longest = words[0];
for word in words {
    if word.Length > longest.Length {
        longest = word;
    }
}
Say("Longest word: " + longest);  // "quick" or "brown" (both length 5)
```

---

## Multidimensional Arrays

Sometimes you need more than a simple list. You need a *grid* — rows and columns. Think of:

- A chessboard (8 rows x 8 columns)
- A spreadsheet (many rows x many columns)
- Pixels in an image (height x width)
- A tic-tac-toe board (3 x 3)
- A seating chart (rows of seats)

You can create these using *arrays of arrays* — an array where each element is itself an array.

### Creating a 2D Array

```zia
var grid = [
    [1, 2, 3],
    [4, 5, 6],
    [7, 8, 9]
];
```

Visualize it as a grid:

```text
         Column 0  Column 1  Column 2
        +--------+--------+--------+
Row 0   |   1    |   2    |   3    |
        +--------+--------+--------+
Row 1   |   4    |   5    |   6    |
        +--------+--------+--------+
Row 2   |   7    |   8    |   9    |
        +--------+--------+--------+
```

### Accessing Elements

Use two indices: the first selects the row (which inner array), the second selects the column (which element within that row):

```zia
var grid = [
    [1, 2, 3],
    [4, 5, 6],
    [7, 8, 9]
];

var value = grid[1][2];  // Row 1, Column 2 = 6
```

Let's break down `grid[1][2]`:
1. `grid[1]` gets the second row: `[4, 5, 6]`
2. `[2]` gets the third element of that row: `6`

```zia
bind Zanna.Terminal;

var grid = [
    [1, 2, 3],
    [4, 5, 6],
    [7, 8, 9]
];

Say(grid[0][0]);  // 1 (top-left)
Say(grid[0][2]);  // 3 (top-right)
Say(grid[2][0]);  // 7 (bottom-left)
Say(grid[2][2]);  // 9 (bottom-right)
Say(grid[1][1]);  // 5 (center)
```

### Modifying Elements

```zia
bind Zanna.Terminal;

var grid = [
    [1, 2, 3],
    [4, 5, 6],
    [7, 8, 9]
];

grid[1][1] = 50;  // Change center from 5 to 50

Say(grid[1][1]);  // 50
```

### Iterating Over a 2D Array

To visit every element, you need nested loops — one for rows, one for columns:

```zia
bind Zanna.Terminal;

var grid = [
    [1, 2, 3],
    [4, 5, 6],
    [7, 8, 9]
];

for row in 0..grid.Length {
    for col in 0..grid[row].Length {
        Print(grid[row][col] + " ");
    }
    Say("");  // New line after each row
}
```

Output:
```text
1 2 3
4 5 6
7 8 9
```

### Example: Tic-Tac-Toe Board

```zia
bind Zanna.Terminal;

// Print the board nicely
func printBoard(board: List[List[String]]) {
    for row in 0..board.Length {
        Print(" ");
        for col in 0..board[row].Length {
            Print(board[row][col]);
            if col < board[row].Length - 1 {
                Print(" | ");
            }
        }
        Say("");
        if row < board.Length - 1 {
            Say("-----------");
        }
    }
}

func start() {
    var board = [
        ["X", "O", "X"],
        [" ", "X", "O"],
        ["O", " ", "X"]
    ];

    printBoard(board);
}
```

Output:
```text
 X | O | X
-----------
   | X | O
-----------
 O |   | X
```

### Example: Multiplication Table

```zia
bind Zanna.Terminal;

// Create a 10x10 multiplication table
var table: List[List[Integer]] = [];

for i in 1..11 {
    var row: List[Integer] = [];
    for j in 1..11 {
        row.Push(i * j);
    }
    table.Push(row);
}

// Print header
Print("    ");
for i in 1..11 {
    Print(i + "\t");
}
Say("");
Say("--------------------------------------------");

// Print table
for i in 0..table.Length {
    Print((i + 1) + " | ");
    for j in 0..table[i].Length {
        Print(table[i][j] + "\t");
    }
    Say("");
}
```

### Example: Image as Pixels

A grayscale image is just a 2D array of brightness values (0 = black, 255 = white):

```zia
bind Zanna.Terminal;

// A tiny 5x5 "image" of a plus sign
var image = [
    [0, 0, 255, 0, 0],
    [0, 0, 255, 0, 0],
    [255, 255, 255, 255, 255],
    [0, 0, 255, 0, 0],
    [0, 0, 255, 0, 0]
];

// Display as ASCII art
for row in 0..image.Length {
    for col in 0..image[row].Length {
        if image[row][col] > 128 {
            Print("#");  // Bright
        } else {
            Print(".");  // Dark
        }
    }
    Say("");
}
```

Output:
```text
..#..
..#..
#####
..#..
..#..
```

---

## Practical Example: Todo List

Let's build a functional todo list manager:

```zia
module TodoList;
bind Zanna.Terminal;

func start() {
    var tasks: List[String] = [];
    var completed: List[Boolean] = [];  // Parallel array tracking completion

    Say("=== Todo List Manager ===");
    Say("Commands: add, done, list, remove, quit");
    Say("");

    while true {
        Print("> ");
        var input = TryReadLine().UnwrapOrStr("");

        if input == "quit" {
            Say("Goodbye!");
            break;
        } else if input == "add" {
            Print("Task: ");
            var task = TryReadLine().UnwrapOrStr("");
            tasks.Push(task);
            completed.Push(false);
            Say("Added: " + task);
        } else if input == "list" {
            if tasks.Length == 0 {
                Say("No tasks yet!");
            } else {
                Say("Your tasks:");
                for i in 0..tasks.Length {
                    var status = "[ ]";
                    if completed[i] {
                        status = "[X]";
                    }
                    Say((i + 1) + ". " + status + " " + tasks[i]);
                }
            }
        } else if input == "done" {
            Print("Task number to mark done: ");
            var num = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));
            if num >= 1 && num <= tasks.Length {
                completed[num - 1] = true;  // Convert to zero-based index
                Say("Marked done: " + tasks[num - 1]);
            } else {
                Say("Invalid task number.");
            }
        } else if input == "remove" {
            Print("Task number to remove: ");
            var num = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));
            if num >= 1 && num <= tasks.Length {
                // Rebuild arrays without this task
                var newTasks: List[String] = [];
                var newCompleted: List[Boolean] = [];
                for i in 0..tasks.Length {
                    if i != num - 1 {
                        newTasks.Push(tasks[i]);
                        newCompleted.Push(completed[i]);
                    }
                }
                Say("Removed: " + tasks[num - 1]);
                tasks = newTasks;
                completed = newCompleted;
            } else {
                Say("Invalid task number.");
            }
        } else {
            Say("Unknown command. Try: add, done, list, remove, quit");
        }

        Say("");
    }
}
```

Sample session:
```text
=== Todo List Manager ===
Commands: add, done, list, remove, quit

> add
Task: Buy groceries
Added: Buy groceries

> add
Task: Finish homework
Added: Finish homework

> add
Task: Call mom
Added: Call mom

> list
Your tasks:
1. [ ] Buy groceries
2. [ ] Finish homework
3. [ ] Call mom

> done
Task number to mark done: 2
Marked done: Finish homework

> list
Your tasks:
1. [ ] Buy groceries
2. [X] Finish homework
3. [ ] Call mom

> quit
Goodbye!
```

---

## Practical Example: Inventory System

A simple game inventory:

```zia
module Inventory;
bind Zanna.Terminal;

func start() {
    var items: List[String] = [];
    var quantities: List[Integer] = [];
    var maxSlots = 10;

    Say("=== Inventory System ===");
    Say("Commands: pickup, drop, use, show, quit");
    Say("Max slots: " + maxSlots);
    Say("");

    while true {
        Print("Command> ");
        var cmd = TryReadLine().UnwrapOrStr("");

        if cmd == "quit" {
            break;
        } else if cmd == "show" {
            showInventory(items, quantities);
        } else if cmd == "pickup" {
            Print("Item name: ");
            var item = TryReadLine().UnwrapOrStr("");
            Print("Quantity: ");
            var qty = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));

            pickupItem(items, quantities, item, qty, maxSlots);
        } else if cmd == "drop" {
            Print("Item name: ");
            var item = TryReadLine().UnwrapOrStr("");
            Print("Quantity: ");
            var qty = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));

            dropItem(items, quantities, item, qty);
        } else if cmd == "use" {
            Print("Item name: ");
            var item = TryReadLine().UnwrapOrStr("");

            useItem(items, quantities, item);
        } else {
            Say("Unknown command.");
        }

        Say("");
    }
}

func findItem(items: List[String], name: String) -> Integer {
    for i in 0..items.Length {
        if items[i] == name {
            return i;
        }
    }
    return -1;  // Not found
}

func showInventory(items: List[String], quantities: List[Integer]) {
    if items.Length == 0 {
        Say("Inventory is empty.");
        return;
    }

    Say("=== Inventory ===");
    for i in 0..items.Length {
        Say("- " + items[i] + " x" + quantities[i]);
    }
    Say("Slots used: " + items.Length);
}

func pickupItem(items: List[String], quantities: List[Integer], name: String, qty: Integer, maxSlots: Integer) {
    var existing = findItem(items, name);

    if existing >= 0 {
        // Already have this item, add to quantity
        quantities[existing] = quantities[existing] + qty;
        Say("Added " + qty + " " + name + " (now have " + quantities[existing] + ")");
    } else {
        // New item
        if items.Length >= maxSlots {
            Say("Inventory full! Drop something first.");
            return;
        }
        items.Push(name);
        quantities.Push(qty);
        Say("Picked up " + qty + " " + name);
    }
}

func dropItem(items: List[String], quantities: List[Integer], name: String, qty: Integer) {
    var idx = findItem(items, name);

    if idx < 0 {
        Say("You don't have any " + name);
        return;
    }

    if qty >= quantities[idx] {
        // Drop all - remove from inventory
        Say("Dropped all " + name);
        // Remove by shifting elements (simplified)
        var newItems: List[String] = [];
        var newQty: List[Integer] = [];
        for i in 0..items.Length {
            if i != idx {
                newItems.Push(items[i]);
                newQty.Push(quantities[i]);
            }
        }
        // Note: In real code, you'd modify the original arrays
    } else {
        quantities[idx] = quantities[idx] - qty;
        Say("Dropped " + qty + " " + name + " (have " + quantities[idx] + " left)");
    }
}

func useItem(items: List[String], quantities: List[Integer], name: String) {
    var idx = findItem(items, name);

    if idx < 0 {
        Say("You don't have any " + name);
        return;
    }

    Say("Used " + name + "!");
    quantities[idx] = quantities[idx] - 1;

    if quantities[idx] <= 0 {
        Say("No more " + name + " left.");
        // Would remove from inventory here
    }
}
```

---

## Practical Example: High Score Table

```zia
module HighScores;
bind Zanna.Terminal;

func start() {
    // Start with some default scores
    var names = ["Alice", "Bob", "Carol", "Dave", "Eve"];
    var scores = [15000, 12500, 11200, 10800, 9500];
    var maxScores = 5;

    Say("=== High Score Table ===");
    Say("");

    displayScores(names, scores);

    // Add a new score
    Say("");
    Print("Enter your name: ");
    var playerName = TryReadLine().UnwrapOrStr("");
    Print("Enter your score: ");
    var playerScore = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));

    // Find where to insert
    var insertPos = -1;
    for i in 0..scores.Length {
        if playerScore > scores[i] {
            insertPos = i;
            break;
        }
    }

    if insertPos >= 0 {
        // Insert new score
        var newNames: List[String] = [];
        var newScores: List[Integer] = [];

        for i in 0..insertPos {
            newNames.Push(names[i]);
            newScores.Push(scores[i]);
        }

        newNames.Push(playerName);
        newScores.Push(playerScore);

        for i in insertPos..(scores.Length - 1) {  // One less since we're adding one
            newNames.Push(names[i]);
            newScores.Push(scores[i]);
        }

        names = newNames;
        scores = newScores;

        Say("");
        Say("Congratulations! You made the high score list!");
        Say("");
    } else if scores.Length < maxScores {
        // Append to end
        names.Push(playerName);
        scores.Push(playerScore);
        Say("");
        Say("You made the list!");
        Say("");
    } else {
        Say("");
        Say("Score not high enough for the leaderboard.");
        Say("");
    }

    displayScores(names, scores);
}

func displayScores(names: List[String], scores: List[Integer]) {
    Say("  RANK  NAME          SCORE");
    Say("  ----  ----          -----");

    for i in 0..names.Length {
        Say("  " + (i + 1) + ".    " + names[i] + "         " + scores[i]);
    }
}
```

---

## The Two Languages

Different languages handle arrays with different syntax, but the concepts are identical.

**Zia**
```zia
bind Zanna.Terminal;

var numbers = [10, 20, 30];

// Access
var first = numbers[0];

// Modify
numbers[1] = 25;

// Iterate
for n in numbers {
    Say(n);
}

// Add
numbers.Push(40);

// Length
Say(numbers.Length);
```

**BASIC**
```basic
DIM numbers(2) AS INTEGER  ' Array with indices 0, 1, 2
numbers(0) = 10
numbers(1) = 20
numbers(2) = 30

' Access
DIM first AS INTEGER
first = numbers(0)

' Modify
numbers(1) = 25

' Iterate
FOR i = 0 TO 2
    PRINT numbers(i)
NEXT i

' Length (BASIC uses UBOUND for upper bound)
PRINT UBOUND(numbers) + 1
```

BASIC uses parentheses `()` instead of brackets `[]` for array access. Arrays must be declared with a fixed size using `DIM`.

---

## Common Mistakes (Expanded)

### Off-by-One with Length

This is the most common array bug:

```zia
var arr = [1, 2, 3, 4, 5];  // Length is 5

// WRONG: arr[5] doesn't exist
var last = arr[arr.Length];  // Error!

// CORRECT: Last element is at length - 1
var last = arr[arr.Length - 1];  // Gets arr[4] = 5
```

Remember: Length tells you *how many* elements. The highest index is always one less.

### Forgetting Zero-Based Indexing

```zia
bind Zanna.Terminal;

var scores = [85, 92, 78];

// "I want the second score"
Say(scores[2]);  // WRONG: This is the THIRD element (78)
Say(scores[1]);  // CORRECT: This is the SECOND element (92)
```

When counting elements, humans say "first, second, third..."
When using indices, programmers say "0, 1, 2..."

### Empty Array Access

```zia
var arr: List[Integer] = [];
var x = arr[0];  // Error! No elements exist

// Always check first
if arr.Length > 0 {
    var x = arr[0];  // Safe
}
```

### Using Wrong Loop Range

This version is expected to trap at runtime:

```zia
bind Zanna.Terminal;

var arr = [1, 2, 3, 4, 5];

// WRONG: 0..=length includes length itself
for i in 0..=arr.Length {
    Say(arr[i]);  // Crashes on last iteration (i=5)
}
```

This version uses the half-open range and should run:

```zia
bind Zanna.Terminal;

var arr = [1, 2, 3, 4, 5];

// CORRECT: 0..Length excludes the endpoint
for i in 0..arr.Length {
    Say(arr[i]);  // Works perfectly (i goes 0,1,2,3,4)
}
```

### Modifying Array While Iterating

```zia
var nums = [1, 2, 3, 4, 5];

// Dangerous: adding while iterating can cause infinite loops
for n in nums {
    if n < 3 {
        nums.Push(n + 10);  // Don't do this!
    }
}

// Safer: iterate over a copy or use indices carefully
```

### Index From User Input Without Validation

```zia
bind Zanna.Terminal;

var items = ["Sword", "Shield", "Potion"];
Print("Choose item (1-3): ");
var choice = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));

// WRONG: User might enter 0, 99, or -5
var item = items[choice - 1];

// CORRECT: Validate first
if choice >= 1 && choice <= items.Length {
    var item = items[choice - 1];
    Say("You selected: " + item);
} else {
    Say("Invalid choice.");
}
```

---

## A Complete Example: Student Grade Tracker

Let's build a comprehensive grade tracker that brings everything together:

```zia
module GradeTracker;
bind Zanna.Terminal;

func start() {
    var grades: List[Integer] = [];

    Say("=== Student Grade Tracker ===");
    Say("Enter grades (enter -1 to finish):");
    Say("");

    // Collect grades with validation
    while true {
        Print("Grade: ");
        var input = Zanna.Core.Convert.ToInt64(TryReadLine().UnwrapOrStr(""));

        if input == -1 {
            break;
        }

        if input >= 0 && input <= 100 {
            grades.Push(input);
            Say("  Added. Total grades: " + grades.Length);
        } else {
            Say("  Please enter 0-100, or -1 to finish.");
        }
    }

    // Handle empty input
    if grades.Length == 0 {
        Say("");
        Say("No grades entered.");
        return;
    }

    // Calculate statistics
    var sum = 0;
    var min = grades[0];
    var max = grades[0];
    var countA = 0;
    var countB = 0;
    var countC = 0;
    var countD = 0;
    var countF = 0;

    for grade in grades {
        sum = sum + grade;

        if grade < min {
            min = grade;
        }
        if grade > max {
            max = grade;
        }

        // Count letter grades
        if grade >= 90 {
            countA = countA + 1;
        } else if grade >= 80 {
            countB = countB + 1;
        } else if grade >= 70 {
            countC = countC + 1;
        } else if grade >= 60 {
            countD = countD + 1;
        } else {
            countF = countF + 1;
        }
    }

    var average = sum / grades.Length;

    // Generate report
    Say("");
    Say("========== GRADE REPORT ==========");
    Say("");
    Say("All grades entered:");

    for i in 0..grades.Length {
        var letterGrade = "F";
        if grades[i] >= 90 {
            letterGrade = "A";
        } else if grades[i] >= 80 {
            letterGrade = "B";
        } else if grades[i] >= 70 {
            letterGrade = "C";
        } else if grades[i] >= 60 {
            letterGrade = "D";
        }
        Say("  Student " + (i + 1) + ": " + grades[i] + " (" + letterGrade + ")");
    }

    Say("");
    Say("Statistics:");
    Say("  Number of students: " + grades.Length);
    Say("  Lowest grade:       " + min);
    Say("  Highest grade:      " + max);
    Say("  Average:            " + average);
    Say("");
    Say("Grade Distribution:");
    Say("  A (90-100): " + countA + " students");
    Say("  B (80-89):  " + countB + " students");
    Say("  C (70-79):  " + countC + " students");
    Say("  D (60-69):  " + countD + " students");
    Say("  F (0-59):   " + countF + " students");
    Say("");
    Say("===================================");
}
```

Sample run:
```text
=== Student Grade Tracker ===
Enter grades (enter -1 to finish):

Grade: 85
  Added. Total grades: 1
Grade: 92
  Added. Total grades: 2
Grade: 78
  Added. Total grades: 3
Grade: 95
  Added. Total grades: 4
Grade: 67
  Added. Total grades: 5
Grade: 88
  Added. Total grades: 6
Grade: 73
  Added. Total grades: 7
Grade: -1

========== GRADE REPORT ==========

All grades entered:
  Student 1: 85 (B)
  Student 2: 92 (A)
  Student 3: 78 (C)
  Student 4: 95 (A)
  Student 5: 67 (D)
  Student 6: 88 (B)
  Student 7: 73 (C)

Statistics:
  Number of students: 7
  Lowest grade:       67
  Highest grade:      95
  Average:            82

Grade Distribution:
  A (90-100): 2 students
  B (80-89):  2 students
  C (70-79):  2 students
  D (60-69):  1 students
  F (0-59):   0 students

===================================
```

This example demonstrates:
- Dynamic array building with `push`
- Input validation
- Finding minimum and maximum values
- Calculating sum and average
- Counting items by category
- Iterating with both for-each and index-based loops
- Converting between user-friendly numbers (1, 2, 3) and zero-based indices

---

## Summary

Arrays are one of the most fundamental and widely-used concepts in programming. Here's what you've learned:

**Core Concepts:**
- Arrays store multiple values under a single name
- Each element has an index (position number)
- Indices start at 0, not 1
- For N elements, valid indices are 0 to N-1

**Creating Arrays:**
- With values: `[1, 2, 3]`
- Empty: `var arr: List[Type] = []`
- Repeated value: `[0; 10]` (ten zeros)

**Accessing and Modifying:**
- Read: `array[index]`
- Write: `array[index] = value`
- Length: `array.Length`

**Growing and Shrinking:**
- Add to end: `array.Push(value)`
- Remove from end: `array.Pop()`

**Iteration Patterns:**
- For-each: `for item in array` (when you need values)
- Index-based: `for i in 0..array.Length` (when you need positions)

**Multidimensional Arrays:**
- Create: `[[1,2,3], [4,5,6], [7,8,9]]`
- Access: `grid[row][col]`
- Use nested loops to iterate

**Safety:**
- Zanna checks bounds and prevents invalid access
- Always validate user input before using as an index
- Check `array.Length > 0` before accessing elements

**When to Use Arrays:**
- Multiple values of the same kind
- Unknown or variable number of items
- Need to process all values uniformly
- Position matters but individual names don't

---

## Exercises

**Exercise 6.1**: Create an array of your five favorite foods and print them all using a for-each loop. Then print them with numbers (1, 2, 3, 4, 5) using an index-based loop.

**Exercise 6.2**: Write a program that creates an array of 10 numbers (you choose), then prints them in reverse order without creating a new array.

**Exercise 6.3**: Write a program that asks the user to enter 5 numbers, stores them in an array, then prints the smallest, largest, and average.

**Exercise 6.4**: Write a program that asks for numbers until -1 is entered, then prints:
- How many numbers were entered
- How many were positive
- How many were negative
- How many were even
- How many were odd

**Exercise 6.5**: Create a 3x3 tic-tac-toe board using a 2D array of strings ("X", "O", and " " for empty). Write code to:
- Print the board in a nice format
- Check if there's a winner (three in a row horizontally, vertically, or diagonally)

```text
 X | O | X
-----------
   | X | O
-----------
 O |   | X
```

**Exercise 6.6**: Write a program that checks if an array is *sorted* (each element >= the one before it). Test it with:
- `[1, 2, 3, 4, 5]` (sorted)
- `[1, 3, 2, 4, 5]` (not sorted)
- `[5, 4, 3, 2, 1]` (not sorted - this is reverse sorted)
- `[1, 1, 2, 2, 3]` (sorted - equal adjacent values are allowed)

**Exercise 6.7** (Challenge): Write a simple quiz program. Store questions in one array and answers in another. Ask each question, check the user's answer, and report the final score.

**Exercise 6.8** (Challenge): Implement a simple shopping cart:
- Store item names and prices in parallel arrays
- Allow adding items
- Allow removing items by name
- Display the cart with a total price

---

*We can now work with groups of data efficiently. But our programs are getting longer and more complex. We're starting to repeat ourselves — the same code appearing in multiple places. How do we organize our code better? How do we avoid repetition? How do we break complex problems into manageable pieces?*

*Next, we learn about **functions** — reusable blocks of code that you can call by name. Functions are how programmers tame complexity.*

*[Continue to Chapter 7: Breaking It Down ->](07-functions.md)*
