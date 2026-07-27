---
status: active
audience: public
last-verified: 2026-07-26
---

# Mathematics

> Mathematical functions, vectors, matrices, bit operations, and random numbers.

**Part of the [Zanna Runtime Library](README.md)**

## Contents

- [Zanna.Math](#zannamath)
- [Zanna.Math.BigInt](#zannamathbigint)
- [Zanna.Math.Bits](#zannamathbits)
- [Zanna.Math.Easing](#zannamatheasing)
- [Zanna.Math.Mat3](#zannamathmat3)
- [Zanna.Math.Mat4](#zannamathmat4)
- [Zanna.Math.PerlinNoise](#zannamathperlinnoise)
- [Zanna.Math.Quat](#zannamathquat)
- [Zanna.Math.Random](#zannamathrandom)
- [Zanna.Math.Spline](#zannamathspline)
- [Zanna.Math.Vec2](#zannamathvec2)
- [Zanna.Math.Vec3](#zannamathvec3)

---

## Zanna.Math.Bits

Bit manipulation utilities for working with 64-bit integers at the bit level.

**Type:** Static (no instantiation required)

### Methods

| Method             | Signature       | Description                            |
|--------------------|-----------------|----------------------------------------|
| `And(a, b)`        | `Integer(Integer, Integer)` | Bitwise AND                            |
| `Or(a, b)`         | `Integer(Integer, Integer)` | Bitwise OR                             |
| `Xor(a, b)`        | `Integer(Integer, Integer)` | Bitwise XOR                            |
| `Not(val)`         | `Integer(Integer)`      | Bitwise NOT (complement)               |
| `Shl(val, count)`  | `Integer(Integer, Integer)` | Logical shift left                     |
| `Shr(val, count)`  | `Integer(Integer, Integer)` | Arithmetic shift right (sign-extended) |
| `ShiftRightLogical(val, count)` | `Integer(Integer, Integer)` | Logical shift right (zero-fill) |
| `RotateLeft(val, count)` | `Integer(Integer, Integer)` | Rotate left                            |
| `RotateRight(val, count)` | `Integer(Integer, Integer)` | Rotate right                           |
| `CountOnes(val)`   | `Integer(Integer)`      | Population count (number of 1 bits)    |
| `CountLeadingZeros(val)` | `Integer(Integer)` | Count leading zeros                    |
| `CountTrailingZeros(val)` | `Integer(Integer)` | Count trailing zeros                   |
| `Flip(val)`        | `Integer(Integer)`      | Reverse all 64 bits                    |
| `Swap(val)`        | `Integer(Integer)`      | Byte swap (endian swap)                |
| `Get(val, bit)`    | `Boolean(Integer, Integer)`  | Get bit at position (0-63)             |
| `Set(val, bit)`    | `Integer(Integer, Integer)` | Set bit at position                    |
| `Clear(val, bit)`  | `Integer(Integer, Integer)` | Clear bit at position                  |
| `Toggle(val, bit)` | `Integer(Integer, Integer)` | Toggle bit at position                 |

### Method Details

#### Shift Operations

- **Shl** — Logical shift left. Shifts bits left, filling with zeros on the right.
- **Shr** — Arithmetic shift right. Shifts bits right, preserving the sign bit (sign-extended).
- **ShiftRightLogical** — Logical shift right. Shifts bits right, filling with zeros on the left.

For `Shl` and `ShiftRightLogical`, negative counts and counts of 64 or more return 0.
For `Shr`, a negative count returns the input unchanged; counts of 64 or more
return -1 for a negative input and 0 otherwise.

#### Rotate Operations

- **RotateLeft** — Rotate left. Bits shifted out on the left wrap around to the right.
- **RotateRight** — Rotate right. Bits shifted out on the right wrap around to the left.

Rotate counts are normalized to 0-63 (count MOD 64).

#### Bit Counting

- **CountOnes** — Population count (popcount). Returns the number of 1 bits.
- **CountLeadingZeros** — Count leading zeros. Returns 64 for zero, 0 for negative values.
- **CountTrailingZeros** — Count trailing zeros. Returns 64 for zero.

Compatibility aliases remain available for existing code: `ShiftRightLogical`, `RotateLeft`,
`RotateRight`, `CountLeadingZeros`, and `CountTrailingZeros`.

#### Single Bit Operations

All single-bit operations accept bit positions 0-63. Out-of-range positions return the input unchanged (for
Set/Clear/Toggle) or false (for Get).

### Zia Example

```zia
module BitsDemo;

bind Zanna.Terminal;
bind Zanna.Math.Bits as Bits;
bind Zanna.Text.Fmt as Fmt;

func start() {
    Say("And: " + Fmt.Int(Bits.And(12, 10)));     // 8
    Say("Or: " + Fmt.Int(Bits.Or(12, 10)));        // 14
    Say("Xor: " + Fmt.Int(Bits.Xor(12, 10)));     // 6
    Say("Shl: " + Fmt.Int(Bits.Shl(1, 4)));       // 16
    Say("Count: " + Fmt.Int(Bits.CountOnes(255)));     // 8
    Say("Leading zeros: " + Fmt.Int(Bits.CountLeadingZeros(1))); // 63
}
```

### BASIC Example

```basic
' Basic bitwise operations
DIM a AS INTEGER = 255
DIM b AS INTEGER = 15
PRINT Zanna.Math.Bits.And(a, b)  ' 15 (15)
PRINT Zanna.Math.Bits.Or(a, b)   ' 255 (255)
PRINT Zanna.Math.Bits.Xor(a, b)  ' 240 (240)

' Shift operations
DIM val AS INTEGER = 1
PRINT Zanna.Math.Bits.Shl(val, 4)   ' 16
PRINT Zanna.Math.Bits.Shr(16, 2)    ' 4

' Count set bits
DIM mask AS INTEGER = 255
PRINT Zanna.Math.Bits.CountOnes(mask)   ' 8

' Work with individual bits
DIM flags AS INTEGER = 0
flags = Zanna.Math.Bits.Set(flags, 0)    ' Set bit 0
flags = Zanna.Math.Bits.Set(flags, 3)    ' Set bit 3
PRINT Zanna.Math.Bits.Get(flags, 0)      ' True
PRINT Zanna.Math.Bits.Get(flags, 1)      ' False
flags = Zanna.Math.Bits.Toggle(flags, 3) ' Toggle bit 3 off
PRINT flags                          ' 1

' Endian conversion
DIM big AS INTEGER = 72623859790382856
DIM little AS INTEGER = Zanna.Math.Bits.Swap(big)
' little = 578437695752307201
```

---

## Zanna.Math

Mathematical functions and constants.

**Type:** Static utility class

### Constants

| Property | Type     | Description                          |
|----------|----------|--------------------------------------|
| `Pi`     | `Double` | π (3.14159265358979...)              |
| `Euler`  | `Double` | Euler's number (2.71828182845904...) |
| `Tau`    | `Double` | τ = 2π (6.28318530717958...)         |

### Basic Functions

| Method           | Signature                | Description                               |
|------------------|--------------------------|-------------------------------------------|
| `Abs(x)`         | `Double(Double)`         | Absolute value of a floating-point number |
| `AbsInt(x)`      | `Integer(Integer)`       | Absolute value of an integer              |
| `Sqrt(x)`        | `Double(Double)`         | Square root                               |
| `Pow(base, exp)` | `Double(Double, Double)` | Raises base to the power of exp           |
| `Exp(x)`         | `Double(Double)`         | e raised to the power x                   |
| `Sign(x)`        | `Double(Double)`         | Sign of x: -1, 0, or 1                    |
| `SignInt(x)`     | `Integer(Integer)`       | Sign of integer x: -1, 0, or 1            |

### Trigonometric Functions

| Method        | Signature                | Description                                            |
|---------------|--------------------------|--------------------------------------------------------|
| `Sin(x)`      | `Double(Double)`         | Sine (radians)                                         |
| `Cos(x)`      | `Double(Double)`         | Cosine (radians)                                       |
| `Tan(x)`      | `Double(Double)`         | Tangent (radians)                                      |
| `Atan(x)`     | `Double(Double)`         | Arctangent (returns radians)                           |
| `Atan2(y, x)` | `Double(Double, Double)` | Arctangent of y/x (returns radians, respects quadrant) |
| `Asin(x)`     | `Double(Double)`         | Arc sine (returns radians)                             |
| `Acos(x)`     | `Double(Double)`         | Arc cosine (returns radians)                           |

### Hyperbolic Functions

| Method    | Signature        | Description        |
|-----------|------------------|--------------------|
| `Sinh(x)` | `Double(Double)` | Hyperbolic sine    |
| `Cosh(x)` | `Double(Double)` | Hyperbolic cosine  |
| `Tanh(x)` | `Double(Double)` | Hyperbolic tangent |

### Logarithmic Functions

| Method     | Signature        | Description                |
|------------|------------------|----------------------------|
| `Log(x)`   | `Double(Double)` | Natural logarithm (base e) |
| `Log10(x)` | `Double(Double)` | Base-10 logarithm          |
| `Log2(x)`  | `Double(Double)` | Base-2 logarithm           |

### Rounding Functions

| Method     | Signature        | Description                                 |
|------------|------------------|---------------------------------------------|
| `Floor(x)` | `Double(Double)` | Largest integer less than or equal to x     |
| `Ceil(x)`  | `Double(Double)` | Smallest integer greater than or equal to x |
| `Round(x)` | `Double(Double)` | Round to nearest integer                    |
| `Truncate(x)` | `Double(Double)` | Truncate toward zero                     |

### Min/Max Functions

| Method                  | Signature                            | Description                          |
|-------------------------|--------------------------------------|--------------------------------------|
| `Min(a, b)`             | `Double(Double, Double)`             | Smaller of two floating-point values |
| `Max(a, b)`             | `Double(Double, Double)`             | Larger of two floating-point values  |
| `MinInt(a, b)`          | `Integer(Integer, Integer)`          | Smaller of two integers              |
| `MaxInt(a, b)`          | `Integer(Integer, Integer)`          | Larger of two integers               |
| `Clamp(val, lo, hi)`    | `Double(Double, Double, Double)`     | Constrain value to range [lo, hi]    |
| `ClampInt(val, lo, hi)` | `Integer(Integer, Integer, Integer)` | Constrain integer to range [lo, hi]  |

### Utility Functions

| Method                 | Signature                            | Description                       |
|------------------------|--------------------------------------|-----------------------------------|
| `FMod(x, y)`           | `Double(Double, Double)`             | Floating-point remainder of x/y   |
| `Lerp(a, b, t)`        | `Double(Double, Double, Double)`     | Linear interpolation; exact at t=0/t=1 and overflow-safe for opposite-sign endpoints |
| `Wrap(val, lo, hi)`    | `Double(Double, Double, Double)`     | Wrap value to range [lo, hi)      |
| `WrapInt(val, lo, hi)` | `Integer(Integer, Integer, Integer)` | Wrap integer to range [lo, hi), including full i64 ranges |
| `Hypot(x, y)`          | `Double(Double, Double)`             | Hypotenuse: sqrt(x² + y²)         |

### Angle Conversion

| Method         | Signature        | Description                |
|----------------|------------------|----------------------------|
| `ToDegrees(radians)` | `Double(Double)` | Convert radians to degrees |
| `ToRadians(degrees)` | `Double(Double)` | Convert degrees to radians |

### Behavior Notes

- Floating-point functions use the host C math-library semantics. Domain errors such as
  `Sqrt(-1.0)` and `Acos(2.0)` return NaN rather than trapping; infinities are handled according
  to the corresponding library operation.
- `AbsInt(INT64_MIN)` traps because its positive magnitude is not representable. `Round` rounds
  halfway cases away from zero.
- `Sign(NaN)` returns NaN. Both positive and negative zero produce positive zero. `Min` and `Max`
  propagate a NaN operand and preserve the expected signed zero (`Min(-0,+0) = -0`,
  `Max(-0,+0) = +0`).
- `Clamp` and `ClampInt` swap inverted bounds. `Lerp` does not clamp `t`, so values outside
  `[0,1]` extrapolate.
- `Wrap` and `WrapInt` do not swap bounds: if `hi <= lo`, they return `lo`. Floating-point
  `Wrap` also follows normal NaN behavior for non-finite inputs.

### Zia Example

```zia
module MathDemo;

bind Zanna.Terminal;
bind Zanna.Math as Math;
bind Zanna.Text.Fmt as Fmt;

func start() {
    Say("Sqrt(144): " + Fmt.NumFixed(Math.Sqrt(144.0), 1));          // 12.0
    Say("Pow(2,10): " + Fmt.NumFixed(Math.Pow(2.0, 10.0), 0));      // 1024
    Say("AbsInt(-42): " + Fmt.Int(Math.AbsInt(-42)));                 // 42
    Say("MinInt(3,7): " + Fmt.Int(Math.MinInt(3, 7)));                // 3
    Say("MaxInt(3,7): " + Fmt.Int(Math.MaxInt(3, 7)));                // 7
    Say("ClampInt(15,0,10): " + Fmt.Int(Math.ClampInt(15, 0, 10)));   // 10
    Say("Floor(3.7): " + Fmt.NumFixed(Math.Floor(3.7), 0));           // 3
    Say("Ceil(3.2): " + Fmt.NumFixed(Math.Ceil(3.2), 0));             // 4
}
```

### BASIC Example

```basic
' Using constants
PRINT Zanna.Math.Pi              ' Output: 3.14159265358979
PRINT Zanna.Math.Euler               ' Output: 2.71828182845905

' Basic math
PRINT Zanna.Math.Sqrt(16)        ' Output: 4.0
PRINT Zanna.Math.Pow(2, 10)      ' Output: 1024.0
PRINT Zanna.Math.Abs(-42.5)      ' Output: 42.5

' Rounding
PRINT Zanna.Math.Floor(2.7)      ' Output: 2.0
PRINT Zanna.Math.Ceil(2.1)       ' Output: 3.0
PRINT Zanna.Math.Round(2.5)      ' Output: 3.0
PRINT Zanna.Math.Truncate(-2.7)     ' Output: -2.0

' Trigonometry (radians)
PRINT Zanna.Math.Sin(Zanna.Math.Pi / 2)  ' Output: 1.0
PRINT Zanna.Math.Cos(0)                   ' Output: 1.0

' Angle conversion
PRINT Zanna.Math.ToDegrees(Zanna.Math.Pi)      ' Output: 180.0
PRINT Zanna.Math.ToRadians(90)                  ' Output: 1.5707963...

' Min/Max/Clamp
PRINT Zanna.Math.MaxInt(10, 20)          ' Output: 20
PRINT Zanna.Math.Clamp(15, 0, 10)        ' Output: 10.0

' Lerp and Wrap
PRINT Zanna.Math.Lerp(0, 100, 0.5)       ' Output: 50.0
PRINT Zanna.Math.Wrap(370, 0, 360)       ' Output: 10.0

' Geometry
PRINT Zanna.Math.Hypot(3, 4)             ' Output: 5.0
```

---

## Zanna.Math.Random

Random number generation with uniform and distribution-based functions.

**Type:** Static utility class plus seeded instances

**Constructor:** `Zanna.Math.Random.New(seed)` or `new Zanna.Math.Random(seed)` creates a seeded random generator instance with state independent from the active runtime context's generator.

### Core Methods

| Method         | Signature          | Description                                     |
|----------------|--------------------|-------------------------------------------------|
| `Seed(value)`  | `Void(Integer)`    | Seeds the random number generator               |
| `NextDouble()` | `Double()`         | Returns a random double in the range [0.0, 1.0) |
| `NextInt(max)` | `Integer(Integer)` | Returns a random integer in the range [0, max)  |

### Distribution Methods

| Method                     | Signature              | Description                                           |
|----------------------------|------------------------|-------------------------------------------------------|
| `Range(min, max)`          | `Integer(Integer, Integer)` | Returns a random integer in [min, max] inclusive |
| `Gaussian(mean, stddev)`   | `Double(Double, Double)` | Returns a normally distributed random value         |
| `Exponential(lambda)`      | `Double(Double)`       | Returns an exponentially distributed random value     |
| `Dice(sides)`              | `Integer(Integer)`     | Simulates a dice roll, returns [1, sides]             |
| `Chance(probability)`      | `Boolean(Double)`      | Returns true with probability p, otherwise false      |
| `Shuffle(seq)`             | `Void(Seq)`            | Randomly shuffles a sequence in place                 |

### Zia Example

```zia
module RandomDemo;

bind Zanna.Terminal;
bind Zanna.Math.Random as Random;
bind Zanna.Text.Fmt as Fmt;

func start() {
    Say("Range(1,100): " + Fmt.Int(Random.Range(1, 100)));
    Say("Float: " + Fmt.NumFixed(Random.NextDouble(), 4));
    Say("Dice d6: " + Fmt.Int(Random.Dice(6)));
}
```

### BASIC Example

```basic
' Seed for reproducible sequences
Zanna.Math.Random.Seed(12345)

' Random float between 0 and 1
DIM r AS DOUBLE
r = Zanna.Math.Random.NextDouble()
PRINT r  ' Output: 0.123... (varies)

' Random integer 0-99
DIM n AS INTEGER
n = Zanna.Math.Random.NextInt(100)
PRINT n  ' Output: 0-99 (varies)

' Random integer in range [1, 10] inclusive
DIM x AS INTEGER
x = Zanna.Math.Random.Range(1, 10)
PRINT x  ' Output: 1-10 (varies)

' Simulate dice roll (1-6)
DIM die AS INTEGER
die = Zanna.Math.Random.Dice(6)
PRINT "You rolled: "; die

' Gaussian distribution (bell curve) with mean=100, stddev=15
DIM iq AS DOUBLE
iq = Zanna.Math.Random.Gaussian(100.0, 15.0)
PRINT "IQ Score: "; INT(iq)

' Exponential distribution (for waiting times, etc.)
DIM waitTime AS DOUBLE
waitTime = Zanna.Math.Random.Exponential(0.5)  ' mean = 2.0
PRINT "Wait: "; waitTime

' 70% chance of success
IF Zanna.Math.Random.Chance(0.7) THEN
    PRINT "Success!"
ELSE
    PRINT "Failed."
END IF

' Shuffle a sequence
DIM seq AS Zanna.Collections.Seq
seq = Zanna.Collections.Seq.New()
FOR i = 1 TO 5
    seq.Push(Zanna.Core.Box.I64(i))
NEXT i
Zanna.Math.Random.Shuffle(seq)  ' Now shuffled: e.g., [3, 1, 5, 2, 4]
```

### Notes

- Static calls use the active runtime context's LCG state; separate runtime contexts have separate
  sequences. A constructed `Random` has independent state and exposes `NextDouble()`,
  `NextInt(max)`, `Range(min,max)`, and `Seed` as instance methods.
- `NextDouble()`, `NextInt(max)`, and `Range(min,max)` are available both as static calls and as
  instance methods. There is no two-argument `NextInt` overload — use `Range(min,max)` for a bounded
  range.
- Sequences are deterministic for a given seed. `NextInt(max)` and `Range(min,max)` use rejection
  sampling to avoid modulo bias; `Range` swaps inverted bounds and supports the full signed i64
  range.
- `NextInt(max <= 0)` returns 0. `Dice(sides <= 0)` returns 1, non-positive standard deviations
  make `Gaussian` return its mean, and non-positive exponential rates return 0.
- `Chance` returns false at probabilities at or below 0 and true at or above 1. `Gaussian` uses
  Box-Muller, `Exponential(lambda)` has mean `1/lambda` for positive finite lambda, and `Shuffle`
  is an in-place Fisher-Yates shuffle using the context generator.

---

## Zanna.Math.Vec2

2D vector math for positions, directions, velocities, and physics calculations.

**Type:** Instance (obj)
**Constructor:** `Zanna.Math.Vec2.New(x, y)` or `Zanna.Math.Vec2.Zero()` or `Zanna.Math.Vec2.One()`

### Static Constructors

| Method      | Signature       | Description                               |
|-------------|-----------------|-------------------------------------------|
| `New(x, y)` | `Object(Double, Double)` | Create a new vector with given components |
| `Zero()`    | `Object()`         | Create a vector at origin (0, 0)          |
| `One()`     | `Object()`         | Create a vector (1, 1)                    |

### Properties

| Property | Type  | Description             |
|----------|-------|-------------------------|
| `X`      | `f64` | X component (read-only) |
| `Y`      | `f64` | Y component (read-only) |

### Methods

| Method           | Signature       | Description                                                |
|------------------|-----------------|------------------------------------------------------------|
| `Add(other)`     | `Object(Object)`      | Add two vectors: self + other                              |
| `Sub(other)`     | `Object(Object)`      | Subtract vectors: self - other                             |
| `Mul(scalar)`    | `Object(Double)`      | Multiply by scalar: self * s                               |
| `Div(scalar)`    | `Object(Double)`      | Divide by scalar: self / s                                 |
| `Negate()`       | `Object()`         | Negate vector: -self                                       |
| `Dot(other)`     | `Double(Object)`      | Dot product of two vectors                                 |
| `Cross(other)`   | `Double(Object)`      | 2D cross product (scalar z-component)                      |
| `Length()`       | `Double()`         | Length (magnitude)                                          |
| `LengthSquared()`        | `Double()`         | Squared length (avoids sqrt)                               |
| `Norm()`         | `Object()`         | Normalize to unit length                                   |
| `Normalize()`    | `Object()`         | Alias for `Norm()`                                         |
| `Dist(other)`    | `Double(Object)`      | Distance to another point                                  |
| `Distance(other)`| `Double(Object)`      | Alias for `Dist(other)`                                    |
| `Lerp(other, t)` | `Object(Object, Double)` | Linear interpolation (t=0 returns self, t=1 returns other) |
| `Heading()`      | `Double()`         | Direction angle in radians (atan2(y, x))                   |
| `Rotate(angle)`  | `Object(Double)`      | Rotate by angle in radians                                 |

### Notes

- Vectors are immutable - all operations return new vectors
- `Norm()` returns the zero vector for a zero-length vector or any non-finite component.
- `Div()` traps for zero, NaN, or infinite divisors.
- `Lerp()` does not clamp `t`; values outside `[0,1]` extrapolate.
- `Cross()` returns the scalar z-component of the 3D cross product (treating 2D vectors as 3D with z=0)
- Angles are in radians; use `Zanna.Math.ToRadians()` and `Zanna.Math.ToDegrees()` for conversion

### Zia Example

```zia
module Vec2Demo;

bind Zanna.Terminal;
bind Zanna.Math.Vec2 as V2;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var a = V2.New(3.0, 4.0);
    Say("Length: " + Fmt.NumFixed(V2.Len(a), 2));               // 5.00

    var b = V2.New(1.0, 0.0);
    var c = V2.Add(a, b);
    Say("Add length: " + Fmt.NumFixed(V2.Len(c), 4));           // 5.6569

    var n = V2.Norm(a);
    Say("Normalized: " + Fmt.NumFixed(V2.Len(n), 2));            // 1.00
}
```

### BASIC Example

```basic
' Create vectors
DIM pos AS OBJECT = Zanna.Math.Vec2.New(100.0, 200.0)
DIM vel AS OBJECT = Zanna.Math.Vec2.New(5.0, -3.0)

' Move position by velocity
pos = pos.Add(vel)
PRINT "Position: ("; pos.X; ", "; pos.Y; ")"

' Calculate distance
DIM target AS OBJECT = Zanna.Math.Vec2.New(150.0, 180.0)
DIM dist AS DOUBLE = pos.Dist(target)
PRINT "Distance to target: "; dist

' Normalize to get direction
DIM dir AS OBJECT = vel.Norm()
PRINT "Direction: ("; dir.X; ", "; dir.Y; ")"
PRINT "Direction length: "; Zanna.Math.Vec2.Len(dir)  ' Should be 1.0

' Rotate a vector 90 degrees
DIM right AS OBJECT = Zanna.Math.Vec2.New(1.0, 0.0)
DIM up AS OBJECT = right.Rotate(3.14159265 / 2.0)
PRINT "Rotated: ("; up.X; ", "; up.Y; ")"  ' (0, 1)

' Linear interpolation for smooth movement
DIM start AS OBJECT = Zanna.Math.Vec2.Zero()
DIM endpoint AS OBJECT = Zanna.Math.Vec2.New(100.0, 100.0)
DIM midpoint AS OBJECT = start.Lerp(endpoint, 0.5)
PRINT "Midpoint: ("; midpoint.X; ", "; midpoint.Y; ")"  ' (50, 50)

' Dot product to check perpendicularity
DIM a AS OBJECT = Zanna.Math.Vec2.New(1.0, 0.0)
DIM b AS OBJECT = Zanna.Math.Vec2.New(0.0, 1.0)
IF a.Dot(b) = 0.0 THEN
    PRINT "Vectors are perpendicular"
END IF
```

---

## Zanna.Math.Vec3

3D vector math for positions, directions, velocities, and physics calculations in 3D space.

**Type:** Instance (obj)
**Constructor:** `Zanna.Math.Vec3.New(x, y, z)` or `Zanna.Math.Vec3.Zero()` or `Zanna.Math.Vec3.One()`

### Static Constructors

| Method         | Signature            | Description                               |
|----------------|----------------------|-------------------------------------------|
| `New(x, y, z)` | `Object(Double, Double, Double)` | Create a new vector with given components |
| `Zero()`       | `Object()`              | Create a vector at origin (0, 0, 0)       |
| `One()`        | `Object()`              | Create a vector (1, 1, 1)                 |

### Properties

| Property | Type  | Description          |
|----------|-------|----------------------|
| `X`      | `f64` | X component (read/write) |
| `Y`      | `f64` | Y component (read/write) |
| `Z`      | `f64` | Z component (read/write) |

### Methods

| Method           | Signature       | Description                                                |
|------------------|-----------------|------------------------------------------------------------|
| `Add(other)`     | `Object(Object)`      | Add two vectors: self + other                              |
| `Sub(other)`     | `Object(Object)`      | Subtract vectors: self - other                             |
| `Mul(scalar)`    | `Object(Double)`      | Multiply by scalar: self * s                               |
| `Div(scalar)`    | `Object(Double)`      | Divide by scalar: self / s                                 |
| `Negate()`       | `Object()`         | Negate vector: -self                                       |
| `Dot(other)`     | `Double(Object)`      | Dot product of two vectors                                 |
| `Cross(other)`   | `Object(Object)`      | Cross product (returns Vec3)                               |
| `Length()`       | `Double()`         | Length (magnitude)                                          |
| `LengthSquared()`        | `Double()`         | Squared length (avoids sqrt)                               |
| `Norm()`         | `Object()`         | Normalize to unit length                                   |
| `Normalize()`    | `Object()`         | Alias for `Norm()`                                         |
| `Dist(other)`    | `Double(Object)`      | Distance to another point                                  |
| `Distance(other)`| `Double(Object)`      | Alias for `Dist(other)`                                    |
| `Lerp(other, t)` | `Object(Object, Double)` | Linear interpolation (t=0 returns self, t=1 returns other) |
| `Reflect(normal)`| `Object(Object)`      | Reflect across a normal, which is normalized internally   |
| `Project(onto)`  | `Object(Object)`      | Project onto the line spanned by another vector            |
| `ClampLength(max)`  | `Object(Double)`      | Limit magnitude to at most `max`                           |
| `MoveTowards(target, delta)` | `Object(Object, Double)` | Move by at most `delta`, snapping when in reach |
| `AngleBetween(other)` | `Double(Object)` | Unsigned angle to another vector in `[0, pi]`              |
| `Min(other)`     | `Object(Object)`      | Component-wise minimum                                     |
| `Max(other)`     | `Object(Object)`      | Component-wise maximum                                     |
| `Set(x, y, z)`   | `Void(Double, Double, Double)` | Replace all components in place                      |
| `CopyFrom(other)` | `Void(Object)`    | Copy another Vec3's components in place                    |

### Notes

- Arithmetic operations are pure and return new vectors.
- Use `Set`, `CopyFrom`, or writable `X`/`Y`/`Z` properties when a per-frame script path
  needs to reuse an existing vector.
- `Norm()` returns the zero vector for zero-length or non-finite input. `Div()` traps for zero,
  NaN, or infinite divisors, while `Lerp()` extrapolates when `t` is outside `[0,1]`.
- `Cross()` returns a Vec3 perpendicular to both input vectors (right-hand rule)
- The cross product formula: a × b = (ay*bz - az*by, az*bx - ax*bz, ax*by - ay*bx)
- `Reflect` and `Project` return zero for degenerate/non-finite directions. `ClampLength` returns zero
  for a non-positive or non-finite limit. `MoveTowards` leaves the current value unchanged for a
  negative or non-finite delta. `Angle` returns 0 for degenerate/non-finite vectors.

### Zia Example

```zia
module Vec3Demo;

bind Zanna.Terminal;
bind Zanna.Math.Vec3 as V3;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var v = V3.New(1.0, 2.0, 3.0);
    Say("Length: " + Fmt.NumFixed(V3.Len(v), 4));                // 3.7417

    var n = V3.Norm(v);
    Say("Normalized: " + Fmt.NumFixed(V3.Len(n), 2));            // 1.00

    var a = V3.New(1.0, 0.0, 0.0);
    var b = V3.New(0.0, 1.0, 0.0);
    var cross = V3.Cross(a, b);
    Say("Cross len: " + Fmt.NumFixed(V3.Len(cross), 2));         // 1.00
}
```

### BASIC Example

```basic
' Create 3D vectors
DIM pos AS OBJECT = Zanna.Math.Vec3.New(100.0, 200.0, 50.0)
DIM vel AS OBJECT = Zanna.Math.Vec3.New(5.0, -3.0, 2.0)

' Move position by velocity
pos = pos.Add(vel)
PRINT "Position: ("; pos.X; ", "; pos.Y; ", "; pos.Z; ")"

' Calculate distance in 3D
DIM target AS OBJECT = Zanna.Math.Vec3.New(150.0, 180.0, 60.0)
DIM dist AS DOUBLE = pos.Dist(target)
PRINT "Distance to target: "; dist

' Normalize to get direction
DIM dir AS OBJECT = vel.Norm()
PRINT "Direction length: "; Zanna.Math.Vec3.Len(dir)  ' Should be 1.0

' Cross product for surface normals
DIM edge1 AS OBJECT = Zanna.Math.Vec3.New(1.0, 0.0, 0.0)
DIM edge2 AS OBJECT = Zanna.Math.Vec3.New(0.0, 1.0, 0.0)
DIM normal AS OBJECT = edge1.Cross(edge2)
PRINT "Normal: ("; normal.X; ", "; normal.Y; ", "; normal.Z; ")"  ' (0, 0, 1)

' Verify cross product is perpendicular
PRINT "Dot with edge1: "; normal.Dot(edge1)  ' 0
PRINT "Dot with edge2: "; normal.Dot(edge2)  ' 0

' Linear interpolation for smooth 3D movement
DIM start AS OBJECT = Zanna.Math.Vec3.Zero()
DIM endpoint AS OBJECT = Zanna.Math.Vec3.New(100.0, 100.0, 100.0)
DIM midpoint AS OBJECT = start.Lerp(endpoint, 0.5)
PRINT "Midpoint: ("; midpoint.X; ", "; midpoint.Y; ", "; midpoint.Z; ")"  ' (50, 50, 50)
```

---

## Zanna.Math.Quat

Quaternion math for 3D rotations, avoiding gimbal lock. Quaternions represent orientations in 3D space and support
smooth interpolation via SLERP.

**Type:** Instance (obj)
**Constructor:** `Quat.New(x, y, z, w)` or `Quat.Identity()`

### Static Constructors

| Method                        | Signature             | Description                                              |
|-------------------------------|-----------------------|----------------------------------------------------------|
| `New(x, y, z, w)`             | `Object(Double,Double,Double,Double)` | Create quaternion from components (x, y, z, w order)    |
| `Identity()`                  | `Object()`               | Create identity quaternion (0, 0, 0, 1)                 |
| `FromAxisAngle(axis, angle)`  | `Object(Object, Double)`       | Create from a Vec3 axis and angle in radians            |
| `FromEuler(pitch, yaw, roll)` | `Object(Double, Double, Double)`  | Create from Euler angles in radians: pitch about X, yaw about Y, roll about Z (ZYX intrinsic order, matching `Transform3D.SetEuler`) |

### Properties

| Property | Type  | Description              |
|----------|-------|--------------------------|
| `W`      | `f64` | W (scalar) component     |
| `X`      | `f64` | X (vector i) component   |
| `Y`      | `f64` | Y (vector j) component   |
| `Z`      | `f64` | Z (vector k) component   |

### Methods

| Method            | Signature       | Description                                                    |
|-------------------|-----------------|----------------------------------------------------------------|
| `Angle()`         | `Double()`         | Return the rotation angle in radians                           |
| `Axis()`          | `Vec3()`        | Return the normalized rotation axis as a Vec3                  |
| `Conjugate()`     | `Object()`         | Return conjugate (inverse for unit quaternions)                |
| `Dot(other)`      | `Double(Object)`      | Dot product with another quaternion                            |
| `Inverse()`       | `Object()`         | Return the inverse quaternion                                  |
| `LengthSquared()`         | `Double()`         | Squared magnitude (avoids sqrt)                                |
| `Lerp(other, t)`  | `Object(Object, Double)` | Normalized linear interpolation (nlerp); `t` is not clamped    |
| `Mul(other)`      | `Object(Object)`      | Multiply (compose) two quaternion rotations                    |
| `Normalize()`     | `Object()`         | Normalize to unit length                                       |
| `RotateVec3(v)`   | `Object(Object)`      | Rotate a Vec3 by this quaternion, returns Vec3                 |
| `Slerp(other, t)` | `Object(Object, Double)` | Spherical linear interpolation (t=0 returns self)              |
| `ToMat4()`        | `Object()`         | Convert to a 4x4 rotation matrix                               |

### Notes

- Quaternions are immutable — all operations return new quaternions.
- `Mul` composes rotations: `a.Mul(b)` applies rotation `b` then rotation `a`.
- `Slerp` assumes unit inputs, chooses the shortest arc, and clamps `t` to `[0,1]`; a non-finite
  `t` traps. `Lerp` normalizes its result but extrapolates for `t` outside that range.
- `Norm()` returns the zero quaternion for a zero-length or non-finite quaternion.
- `Inverse()` traps only for a zero-length or non-finite quaternion; for any finite nonzero
  quaternion — including extreme magnitudes near the `double` range limits — it returns a finite,
  nonzero inverse (the squared-length reciprocal is computed with scaling to avoid overflow to
  zero or underflow to infinity).
- `FromAxisAngle` takes a `Vec3` axis and radians. A zero/non-finite axis or non-finite angle
  produces identity. `FromEuler` likewise returns identity for non-finite input.
- `RotateVec3` and `ToMat4` assume a unit quaternion; call `Norm()` first for raw-component
  quaternions. `Axis()` returns `(0,0,1)` when no meaningful axis exists, and `Angle()` returns a
  value in `[0,2*pi]` (0 for a degenerate quaternion).

### Zia Example

```zia
module QuaternionDemo;

bind Zanna.Math;
bind Zanna.Terminal;

func start() {
    // Identity quaternion
    var id = Quat.Identity();
    SayNum(Quat.get_W(id));  // 1.0

    // From axis-angle (90° around Y)
    var yAxis = Vec3.New(0.0, 1.0, 0.0);
    var q90 = Quat.FromAxisAngle(yAxis, 1.5707963);

    // Rotate a vector
    var v = Vec3.New(1.0, 0.0, 0.0);
    var rotated = Quat.RotateVec3(q90, v);
    SayNum(Vec3.get_Z(rotated));  // ~-1.0

    // Compose rotations (90° + 90° = 180°)
    var combined = Quat.Mul(q90, q90);

    // Interpolate (slerp)
    var halfway = Quat.Slerp(id, q90, 0.5);
    SayNum(Quat.Length(halfway));     // 1.0

    // Inverse (q * q^-1 = identity)
    var inv = Quat.Inverse(q90);
    var check = Quat.Mul(q90, inv);
    SayNum(Quat.get_W(check));  // ~1.0

    // Euler angles
    var qe = Quat.FromEuler(0.0, 1.5707963, 0.0);
    SayNum(Quat.Angle(qe));  // ~1.5707963
}
```

### BASIC Example

```basic
' Create quaternion from axis-angle (90 degrees around Y axis)
DIM axis AS Zanna.Math.Vec3 = Zanna.Math.Vec3.New(0.0, 1.0, 0.0)
DIM q AS OBJECT = Zanna.Math.Quat.FromAxisAngle(axis, Zanna.Math.ToRadians(90.0))

' Rotate a vector
DIM v AS Zanna.Math.Vec3 = Zanna.Math.Vec3.New(1.0, 0.0, 0.0)
DIM rotated AS OBJECT = Zanna.Math.Quat.RotateVec3(q, v)
PRINT "Rotated: ("; Zanna.Math.Vec3.get_X(rotated); ", "; Zanna.Math.Vec3.get_Y(rotated); ", "; Zanna.Math.Vec3.get_Z(rotated); ")"
' Approximately (0, 0, -1) for 90-degree Y rotation

' Compose rotations
DIM axis2 AS Zanna.Math.Vec3 = Zanna.Math.Vec3.New(1.0, 0.0, 0.0)
DIM q2 AS OBJECT = Zanna.Math.Quat.FromAxisAngle(axis2, Zanna.Math.ToRadians(45.0))
DIM combined AS OBJECT = Zanna.Math.Quat.Mul(q, q2)

' Smooth interpolation between orientations
DIM identity AS OBJECT = Zanna.Math.Quat.Identity()
DIM halfway AS OBJECT = Zanna.Math.Quat.Slerp(identity, q, 0.5)
```

---

## Zanna.Math.Easing

Standard easing functions for smooth animation and interpolation. Each function is intended for a normalized time
value `t` in `[0.0,1.0]`. Inputs are not uniformly clamped: polynomial, sine, back, and bounce
functions extrapolate, exponential and elastic functions pin their endpoints, and circular
functions can return NaN outside their square-root domain. Back and elastic curves intentionally
overshoot `[0,1]` even for valid input.

**Type:** Static utility class

### Methods

| Method           | Signature    | Description                                  |
|------------------|-------------|----------------------------------------------|
| `Linear(t)`      | `Double(Double)` | Linear (no easing)                       |
| `EaseInQuad(t)`      | `Double(Double)` | Quadratic ease in (accelerate)           |
| `EaseOutQuad(t)`     | `Double(Double)` | Quadratic ease out (decelerate)          |
| `EaseInOutQuad(t)`   | `Double(Double)` | Quadratic ease in-out                    |
| `EaseInCubic(t)`     | `Double(Double)` | Cubic ease in                            |
| `EaseOutCubic(t)`    | `Double(Double)` | Cubic ease out                           |
| `EaseInOutCubic(t)`  | `Double(Double)` | Cubic ease in-out                        |
| `EaseInQuart(t)`     | `Double(Double)` | Quartic ease in                          |
| `EaseOutQuart(t)`    | `Double(Double)` | Quartic ease out                         |
| `EaseInSine(t)`      | `Double(Double)` | Sinusoidal ease in                       |
| `EaseOutSine(t)`     | `Double(Double)` | Sinusoidal ease out                      |
| `EaseInOutSine(t)`   | `Double(Double)` | Sinusoidal ease in-out                   |
| `EaseInExpo(t)`      | `Double(Double)` | Exponential ease in                      |
| `EaseOutExpo(t)`     | `Double(Double)` | Exponential ease out                     |
| `EaseInOutExpo(t)`   | `Double(Double)` | Exponential ease in-out                  |
| `EaseInCirc(t)`      | `Double(Double)` | Circular ease in                         |
| `EaseOutCirc(t)`     | `Double(Double)` | Circular ease out                        |
| `EaseInOutCirc(t)`   | `Double(Double)` | Circular ease in-out                     |
| `EaseInBack(t)`      | `Double(Double)` | Back ease in (overshoots start)          |
| `EaseOutBack(t)`     | `Double(Double)` | Back ease out (overshoots end)           |
| `EaseInOutBack(t)`   | `Double(Double)` | Back ease in-out                         |
| `EaseInElastic(t)`   | `Double(Double)` | Elastic ease in (spring-like)            |
| `EaseOutElastic(t)`  | `Double(Double)` | Elastic ease out                         |
| `EaseInOutElastic(t)`| `Double(Double)` | Elastic ease in-out                      |
| `EaseInBounce(t)`    | `Double(Double)` | Bounce ease in                           |
| `EaseOutBounce(t)`   | `Double(Double)` | Bounce ease out                          |
| `EaseInOutBounce(t)` | `Double(Double)` | Bounce ease in-out                       |

### Zia Example

```zia
module EasingDemo;

bind Zanna.Math;
bind Zanna.Terminal;

func start() {
    // Linear
    SayNum(Easing.Linear(0.0));   // 0.0
    SayNum(Easing.Linear(0.5));   // 0.5
    SayNum(Easing.Linear(1.0));   // 1.0

    // Quadratic
    SayNum(Easing.EaseInQuad(0.5));      // 0.25
    SayNum(Easing.EaseOutQuad(0.5));     // 0.75
    SayNum(Easing.EaseInOutQuad(0.5));   // 0.5

    // Cubic
    SayNum(Easing.EaseInCubic(0.5));     // 0.125
    SayNum(Easing.EaseOutCubic(0.5));    // 0.875

    // Sine
    SayNum(Easing.EaseInSine(0.0));      // 0.0
    SayNum(Easing.EaseOutSine(1.0));     // 1.0

    // Elastic and Bounce
    SayNum(Easing.EaseInElastic(0.0));   // 0.0
    SayNum(Easing.EaseOutBounce(1.0));   // 1.0

    // Back (overshoots slightly)
    SayNum(Easing.EaseInBack(0.0));      // 0.0
    SayNum(Easing.EaseOutBack(1.0));     // 1.0

    // Quart
    SayNum(Easing.EaseInQuart(0.5));     // 0.0625
    SayNum(Easing.EaseOutQuart(0.5));    // 0.9375
}
```

### BASIC Example

```basic
' Smooth animation using easing functions
DIM t AS DOUBLE
FOR i = 0 TO 10
    t = i / 10.0
    DIM eased AS DOUBLE = Zanna.Math.Easing.EaseOutCubic(t)
    PRINT "t="; t; " eased="; eased
NEXT

' Use with Lerp for smooth movement
DIM startX AS DOUBLE = 0.0
DIM endX AS DOUBLE = 100.0
DIM progress AS DOUBLE = 0.5
DIM smoothX AS DOUBLE = Zanna.Math.Lerp(startX, endX, Zanna.Math.Easing.EaseInOutQuad(progress))
```

---

## Zanna.Math.Spline

Curve interpolation for smooth paths. Supports Catmull-Rom, Bezier, and linear splines.

**Type:** Instance (obj)

### Static Constructors

| Method                              | Signature                              | Description                                         |
|-------------------------------------|----------------------------------------|-----------------------------------------------------|
| `CatmullRom(points)`               | `Spline(Seq)`                          | Create Catmull-Rom spline from a sequence of Vec2   |
| `Bezier(p0, p1, p2, p3)`           | `Spline(Vec2, Vec2, Vec2, Vec2)`       | Create cubic Bezier curve from 4 control points     |
| `Linear(points)`                    | `Spline(Seq)`                          | Create linear interpolation between points          |

### Properties

| Property     | Type    | Description                     |
|--------------|---------|---------------------------------|
| `PointCount` | Integer | Number of control points        |

### Methods

| Method                           | Signature                        | Description                                              |
|----------------------------------|----------------------------------|----------------------------------------------------------|
| `Eval(t)`                        | `Vec2(Double)`                   | Evaluate position at parameter t (0.0 to 1.0)           |
| `Tangent(t)`                     | `Vec2(Double)`                   | Evaluate tangent vector at parameter t                   |
| `PointAt(index)`                 | `Vec2(Integer)`                  | Get control point at index                               |
| `ArcLength(t0, t1, segments)`    | `Double(Double, Double, Integer)`| Approximate arc length between t0 and t1                 |
| `Sample(count)`                  | `Seq(Integer)`                   | Sample count evenly-spaced points along the spline       |

The current runtime registry includes the spline object as an explicit first parameter for these operations. Use the
static forms shown below (`Spline.Eval(spline, t)`, `Spline.Sample(spline, count)`, and so on); instance-call syntax is
not accepted by the current BASIC frontend.

- `CatmullRom` and `Linear` require at least two points and copy each input Vec2's coordinates.
  The current Catmull-Rom implementation is the uniform `0.5`-tangent form with duplicated endpoint
  neighbors; it is not centripetal.
- `t` is clamped to `[0,1]` for every spline kind (Linear, Bezier, and Catmull-Rom): values below
  0 or `-Inf` map to the curve start, values above 1 or `+Inf` map to the curve end, and `NaN`
  maps to the curve start. `Eval`, `Tangent`, and `ArcLength` apply this rule uniformly, so a
  non-finite parameter is always well-defined and never extrapolates.
- `Tangent` is not normalized: it is the active segment delta for a linear spline, the analytic
  derivative for Bezier, and a finite-difference derivative for Catmull-Rom.
- `PointAt` traps outside `0..PointCount-1`. `ArcLength` uses at least one equal-parameter segment,
  and `Sample(count)` raises counts below two to two.

### Zia Example

```zia
module SplineDemo;

bind Zanna.Math;
bind Zanna.Terminal;
bind Zanna.Collections;

func start() {
    // Linear spline from points
    var pts = Seq.New();
    pts.Push(Vec2.New(0.0, 0.0));
    pts.Push(Vec2.New(100.0, 0.0));
    pts.Push(Vec2.New(100.0, 100.0));

    var lin = Spline.Linear(pts);
    SayInt(Spline.get_PointCount(lin));  // 3

    // Evaluate at t=0, 0.5, 1.0
    var p0 = Spline.Eval(lin, 0.0);
    SayNum(Vec2.get_X(p0));  // 0.0

    var pm = Spline.Eval(lin, 0.5);
    SayNum(Vec2.get_X(pm));  // 100.0

    // Arc length and sampling
    var len = Spline.ArcLength(lin, 0.0, 1.0, 100);
    SayNum(len);  // 200.0

    var samples = Spline.Sample(lin, 5);
    SayInt(Seq.get_Count(samples));  // 5

    // Bezier curve
    var bez = Spline.Bezier(
        Vec2.New(0.0, 0.0),
        Vec2.New(33.0, 100.0),
        Vec2.New(66.0, 100.0),
        Vec2.New(100.0, 0.0)
    );
    var bm = Spline.Eval(bez, 0.5);
    SayNum(Vec2.get_Y(bm));  // 75.0

    // Catmull-Rom
    var cpts = Seq.New();
    cpts.Push(Vec2.New(0.0, 0.0));
    cpts.Push(Vec2.New(50.0, 100.0));
    cpts.Push(Vec2.New(100.0, 50.0));
    cpts.Push(Vec2.New(150.0, 100.0));
    var cr = Spline.CatmullRom(cpts);
    SayInt(Spline.get_PointCount(cr));  // 4
}
```

### BASIC Example

```basic
' Create a Catmull-Rom spline
DIM points AS Zanna.Collections.Seq = NEW Zanna.Collections.Seq()
points.Push(Zanna.Math.Vec2.New(0.0, 0.0))
points.Push(Zanna.Math.Vec2.New(50.0, 100.0))
points.Push(Zanna.Math.Vec2.New(100.0, 50.0))
points.Push(Zanna.Math.Vec2.New(150.0, 100.0))

DIM spline AS OBJECT = Zanna.Math.Spline.CatmullRom(points)

' Sample points along the spline
DIM samples AS Zanna.Collections.Seq = Zanna.Math.Spline.Sample(spline, 20)
FOR i = 0 TO samples.Count - 1
    DIM p AS OBJECT = samples.Get(i)
    PRINT "x="; Zanna.Math.Vec2.get_X(p); " y="; Zanna.Math.Vec2.get_Y(p)
NEXT

' Evaluate at specific parameter
DIM midpoint AS OBJECT = Zanna.Math.Spline.Eval(spline, 0.5)
PRINT "Mid: ("; Zanna.Math.Vec2.get_X(midpoint); ", "; Zanna.Math.Vec2.get_Y(midpoint); ")"
```

---

## Zanna.Math.PerlinNoise

Perlin noise generator for procedural content generation. Produces smooth, continuous pseudo-random values suitable for terrain generation, texture synthesis, and organic-looking randomness.

**Type:** Instance class (requires `New(seed)`)

### Constructor

| Method       | Signature            | Description                                        |
|--------------|----------------------|----------------------------------------------------|
| `New(seed)`  | `PerlinNoise(Integer)` | Create a new Perlin noise generator with a seed  |

### Methods

| Method                               | Signature                               | Description                                                  |
|--------------------------------------|-----------------------------------------|--------------------------------------------------------------|
| `Noise2D(noise, x, y)`              | `Double(Object, Double, Double)`        | Sample 2D Perlin noise at coordinates (x, y)                |
| `Noise3D(noise, x, y, z)`           | `Double(Object, Double, Double, Double)` | Sample 3D Perlin noise at coordinates (x, y, z)            |
| `Octave2D(noise, x, y, oct, pers)`  | `Double(Object, Double, Double, Integer, Double)` | Sample 2D fractal noise with octaves and persistence |
| `Octave3D(noise, x, y, z, oct, pers)` | `Double(Object, Double, Double, Double, Integer, Double)` | Sample 3D fractal noise with octaves and persistence |

### Parameters

| Parameter     | Type    | Description                                                |
|---------------|---------|------------------------------------------------------------|
| `noise`       | Object  | The PerlinNoise instance (passed explicitly)               |
| `x`, `y`, `z` | Double  | Sampling coordinates                                       |
| `oct`         | Integer | Number of octaves (layers of detail)                       |
| `pers`        | Double  | Persistence (amplitude reduction per octave, typically 0.5)|

### Notes

- All methods are called in a static style, passing the noise object as the first parameter
- Single-octave output is approximately bounded by `[-1,1]` and is not clamped (2D output is
  normally narrower). The field repeats every 256 integer cells on each axis.
- The same seed always produces the same noise field (deterministic)
- `Octave2D`/`Octave3D` layer multiple frequencies for more natural-looking noise (fractal Brownian motion)
- Octave counts are clamped to 16; non-positive counts return 0. Non-finite persistence also
  returns 0. Persistence is accepted outside the usual `[0,1]` range, including negative values.
- Coordinates must be finite and their floored cell index must fit a 32-bit C `int`; otherwise the
  sample returns 0. Higher octave counts add finer detail but increase computation cost.
- The runtime validates the explicit receiver's heap kind, class, and size before reading its
  permutation table: a non-`PerlinNoise` object (or null) traps rather than being misread as
  noise state. `PerlinNoise.New` returns a concretely typed `obj<Zanna.Math.PerlinNoise>`.

### Zia Example

```zia
module PerlinDemo;

bind Zanna.Terminal;
bind Zanna.Math.PerlinNoise as PerlinNoise;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var p = PerlinNoise.New(42);

    var n2d = PerlinNoise.Noise2D(p, 0.5, 0.5);
    Say("Noise2D(0.5, 0.5): " + Fmt.NumFixed(n2d, 4));   // -0.5000

    var n3d = PerlinNoise.Noise3D(p, 1.0, 2.0, 3.0);
    Say("Noise3D(1.0, 2.0, 3.0): " + Fmt.NumFixed(n3d, 4)); // 0.0000

    // Fractal noise with 4 octaves
    var oct = PerlinNoise.Octave2D(p, 0.5, 0.5, 4, 0.5);
    Say("Octave2D: " + Fmt.NumFixed(oct, 4));
}
```

### BASIC Example

```basic
' Create a Perlin noise generator with seed 42
DIM p AS OBJECT = Zanna.Math.PerlinNoise.New(42)

' Sample 2D noise
DIM n2d AS DOUBLE = Zanna.Math.PerlinNoise.Noise2D(p, 0.5, 0.5)
PRINT "Noise2D(0.5, 0.5): "; n2d   ' Output: -0.5

' Sample 3D noise
DIM n3d AS DOUBLE = Zanna.Math.PerlinNoise.Noise3D(p, 1.0, 2.0, 3.0)
PRINT "Noise3D(1.0, 2.0, 3.0): "; n3d   ' Output: 0

' Fractal noise with octaves for terrain-like output
DIM oct AS DOUBLE = Zanna.Math.PerlinNoise.Octave2D(p, 0.5, 0.5, 4, 0.5)
PRINT "Octave2D: "; oct

' Generate a simple height map
FOR y = 0 TO 9
    FOR x = 0 TO 9
        DIM h AS DOUBLE = Zanna.Math.PerlinNoise.Noise2D(p, x * 0.1, y * 0.1)
        ' Map noise from [-1,1] to [0,255]
        DIM height AS INTEGER = INT((h + 1.0) * 127.5)
    NEXT x
NEXT y
```

### Use Cases

- **Terrain generation:** Generate height maps for landscapes
- **Texture synthesis:** Create natural-looking procedural textures (clouds, marble, wood)
- **Animation:** Add organic movement to objects
- **Game worlds:** Procedurally generate caves, forests, and biomes
- **Particle effects:** Add natural variation to particle systems

---

## Zanna.Math.BigInt

Arbitrary-precision integer arithmetic. All methods are static; instance values are opaque objects returned by the
constructors. Pass the bigint object explicitly as the first argument to instance-style methods.

**Type:** Static utility class

### Constructors and Constants

| Method / Property  | Signature         | Description                                    |
|--------------------|-------------------|------------------------------------------------|
| `FromInt(n)`       | `Object(Integer)` | Create a BigInt from a 64-bit integer          |
| `FromStr(s)`       | `Object(String)`  | Parse decimal or `0x`/`0b`/`0o` text           |
| `FromBytes(b)`     | `Object(Bytes)`   | Parse big-endian two's-complement Bytes        |
| `Zero`             | `Object`          | The constant 0                                 |
| `One`              | `Object`          | The constant 1                                 |

### Conversion Methods

| Method             | Signature                | Description                                      |
|--------------------|--------------------------|--------------------------------------------------|
| `ToInt(n)`         | `Integer(Object)`        | Convert to i64, saturating out-of-range values |
| `ToString(n)`      | `String(Object)`         | Convert to decimal string                        |
| `ToStringBase(n, base)` | `String(Object, Integer)` | Convert to string in given base (2–36)        |
| `ToBytes(n)`       | `Bytes(Object)`          | Convert to big-endian two's-complement Bytes     |
| `FitsInt(n)`       | `Boolean(Object)`        | True if value fits in a 64-bit signed integer    |

### Arithmetic Methods

| Method           | Signature                    | Description              |
|------------------|------------------------------|--------------------------|
| `Add(a, b)`      | `Object(Object, Object)`     | a + b                    |
| `Sub(a, b)`      | `Object(Object, Object)`     | a − b                    |
| `Mul(a, b)`      | `Object(Object, Object)`     | a × b                    |
| `Div(a, b)`      | `Object(Object, Object)`     | Truncated division a ÷ b |
| `Mod(a, b)`      | `Object(Object, Object)`     | Remainder of a ÷ b       |
| `Negate(n)`      | `Object(Object)`             | Negate: −n               |
| `Abs(n)`         | `Object(Object)`             | Absolute value           |
| `Sqrt(n)`        | `Object(Object)`             | Integer square root (floor) |
| `Pow(n, exp)`    | `Object(Object, Integer)`    | n raised to exp          |
| `PowMod(n, exp, mod)` | `Object(Object, Object, Object)` | Modular exponentiation |
| `Gcd(a, b)`      | `Object(Object, Object)`     | Greatest common divisor  |
| `Lcm(a, b)`      | `Object(Object, Object)`     | Least common multiple    |

### Comparison and Inspection

| Method           | Signature                    | Description                           |
|------------------|------------------------------|---------------------------------------|
| `Compare(a, b)`      | `Integer(Object, Object)`    | -1 if a < b, 0 if equal, 1 if a > b  |
| `Equals(a, b)`       | `Boolean(Object, Object)`    | True if a equals b                    |
| `IsZero(n)`      | `Boolean(Object)`            | True if n == 0                        |
| `IsNegative(n)`  | `Boolean(Object)`            | True if n < 0                         |
| `Sign(n)`        | `Integer(Object)`            | -1, 0, or 1                           |

### Bitwise Methods

| Method              | Signature                    | Description                           |
|---------------------|------------------------------|---------------------------------------|
| `And(a, b)`         | `Object(Object, Object)`     | Bitwise AND                           |
| `Or(a, b)`          | `Object(Object, Object)`     | Bitwise OR                            |
| `Xor(a, b)`         | `Object(Object, Object)`     | Bitwise XOR                           |
| `Not(n)`            | `Object(Object)`             | Bitwise NOT (one's complement)        |
| `Shl(n, bits)`      | `Object(Object, Integer)`    | Shift left by bits                    |
| `Shr(n, bits)`      | `Object(Object, Integer)`    | Arithmetic shift right by bits        |
| `BitLength(n)`      | `Integer(Object)`            | Number of bits in binary representation |
| `TestBit(n, i)`     | `Boolean(Object, Integer)`   | True if bit i is set                  |
| `SetBit(n, i)`      | `Object(Object, Integer)`    | Return n with bit i set               |
| `ClearBit(n, i)`    | `Object(Object, Integer)`    | Return n with bit i cleared           |

### Notes

- BigInt values are immutable — all operations return new objects.
- `FromStr` accepts an optional sign, leading spaces/tabs, trailing whitespace, underscores, and
  `0x`, `0b`, or `0o` prefixes. Invalid text returns null rather than trapping. Separators are
  permissive and may appear at the edges or repeatedly.
- `ToInt` saturates to `INT64_MIN`/`INT64_MAX`; call `FitsInt` first when loss must be rejected.
- Byte conversion uses signed big-endian two's-complement form and is minimal: a leading `0xff`
  (negative) or `0x00` (positive) sign byte is added exactly when the encoding would otherwise
  read as the wrong sign. `ToBytes` / `FromBytes` round-trip every value in both signs, including
  across the `0x80` magnitude boundaries (e.g. `-129` encodes as `ff 7f`).
- `Div(a,b)` truncates toward zero and `Mod(a,b)` gives the remainder the dividend's sign; both
  trap on a zero divisor. `PowMod`, however, returns the least non-negative residue in `[0, |mod|)`
  regardless of the base's sign (e.g. `PowMod(-2, 3, 5)` is `2`, not `-3`), so it is safe for
  number-theory and modular-arithmetic use.
- `Sqrt(n)` traps if n is negative.
- `Pow(n,exp)` and `PowMod(n,exp,mod)` require non-negative exponents; `PowMod` also requires a
  nonzero modulus.
- `ToStringBase` supports bases 2 through 36; digits above 9 use lowercase letters.
- Bitwise methods use arbitrary-width two's-complement semantics. `BitLength` measures magnitude;
  negative `TestBit` values have infinitely extended sign bits. Negative bit indexes return false
  or an unchanged clone, and negative shift counts leave the value unchanged.
- BigInt operands are exposed as generic `Object` values, but every operation validates each
  operand's heap kind, class, and size at the boundary: passing a non-BigInt object traps with
  `BigInt: invalid BigInt object` rather than being dereferenced as bigint state. (A null operand
  keeps each operation's documented identity behavior — e.g. `Add(null, x)` returns a copy of x.)

### Zia Example

```zia
module BigIntDemo;

bind Zanna.Terminal;
bind Zanna.Math.BigInt as BigInt;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var a = BigInt.FromStr("123456789012345678901234567890");
    var b = BigInt.FromInt(999999999);

    var sum  = BigInt.Add(a, b);
    var prod = BigInt.Mul(b, b);

    Say("Sum: " + BigInt.ToString(sum));
    Say("Product: " + BigInt.ToString(prod));
    Say("FitsInt(b): " + Fmt.Bool(BigInt.FitsInt(b)));  // true
    Say("FitsInt(a): " + Fmt.Bool(BigInt.FitsInt(a)));  // false

    var g = BigInt.Gcd(BigInt.FromInt(48), BigInt.FromInt(36));
    Say("Gcd(48,36): " + BigInt.ToString(g));           // 12

    var pm = BigInt.PowMod(BigInt.FromInt(2), BigInt.FromInt(10), BigInt.FromInt(1000));
    Say("2^10 mod 1000: " + BigInt.ToString(pm));       // 24

    Say("BitLength: " + Fmt.Int(BigInt.BitLength(BigInt.FromInt(255))));  // 8
}
```

### BASIC Example

```basic
' Arbitrary-precision arithmetic
DIM a AS OBJECT = Zanna.Math.BigInt.FromStr("999999999999999999999999")
DIM b AS OBJECT = Zanna.Math.BigInt.FromInt(2)

DIM doubled AS OBJECT = Zanna.Math.BigInt.Mul(a, b)
PRINT Zanna.Math.BigInt.ToString(doubled)
' Output: 1999999999999999999999998

' Modular exponentiation (useful in cryptography)
DIM base AS OBJECT = Zanna.Math.BigInt.FromInt(65537)
DIM exp  AS OBJECT = Zanna.Math.BigInt.FromInt(65537)
DIM modulus AS OBJECT = Zanna.Math.BigInt.FromStr("340282366920938463463374607431768211457")
DIM result AS OBJECT = Zanna.Math.BigInt.PowMod(base, exp, modulus)
PRINT Zanna.Math.BigInt.ToString(result)

' GCD and LCM
DIM g AS OBJECT = Zanna.Math.BigInt.Gcd(Zanna.Math.BigInt.FromInt(48), Zanna.Math.BigInt.FromInt(36))
PRINT Zanna.Math.BigInt.ToString(g)   ' 12

' Comparison
DIM x AS OBJECT = Zanna.Math.BigInt.FromStr("100000000000000000000")
DIM y AS OBJECT = Zanna.Math.BigInt.FromStr("99999999999999999999")
PRINT Zanna.Math.BigInt.Compare(x, y)    ' 1 (x > y)

' Bitwise ops
DIM n AS OBJECT = Zanna.Math.BigInt.FromInt(255)
PRINT Zanna.Math.BigInt.BitLength(n)  ' 8
PRINT Zanna.Math.BigInt.TestBit(n, 7) ' True
DIM shifted AS OBJECT = Zanna.Math.BigInt.Shl(n, 8)
PRINT Zanna.Math.BigInt.ToString(shifted)  ' 65280

' Check if big number fits in 64-bit integer
DIM big AS OBJECT = Zanna.Math.BigInt.FromStr("99999999999999999999")
IF NOT Zanna.Math.BigInt.FitsInt(big) THEN
    PRINT "Too large for native integer"
END IF
```

---

## Zanna.Math.Mat3

3×3 matrix for 2D affine transformations (translation, rotation, scale, shear). All methods are static; matrix
values are opaque objects. Pass the matrix as the first argument to instance-style methods.

**Type:** Static utility class

### Constructors

| Method                        | Signature                                    | Description                                    |
|-------------------------------|----------------------------------------------|------------------------------------------------|
| `New(m00..m22)`               | `Object(f64×9)`                              | Create from 9 row-major floats                 |
| `Identity()`                  | `Object()`                                   | 3×3 identity matrix                            |
| `Zero()`                      | `Object()`                                   | 3×3 zero matrix                               |

### 2D Transform Factories

| Method                | Signature                 | Description                                     |
|-----------------------|---------------------------|-------------------------------------------------|
| `Translate(x, y)`     | `Object(f64, f64)`        | Translation matrix                              |
| `Scale(x, y)`         | `Object(f64, f64)`        | Non-uniform scale matrix                        |
| `ScaleUniform(s)`     | `Object(f64)`             | Uniform scale matrix                            |
| `Rotate(angle)`       | `Object(f64)`             | Counter-clockwise rotation matrix (radians)     |
| `Shear(x, y)`         | `Object(f64, f64)`        | Shear matrix (x shear in X direction, etc.)     |

### Element Access

| Method           | Signature                    | Description                         |
|------------------|------------------------------|-------------------------------------|
| `Get(m, row, col)` | `Double(Object, Integer, Integer)` | Get element at (row, col) 0-indexed |
| `Row(m, i)`      | `Object(Object, Integer)`    | Return row i as a Vec3              |
| `Col(m, i)`      | `Object(Object, Integer)`    | Return column i as a Vec3           |

### Math Operations

| Method              | Signature                    | Description                           |
|---------------------|------------------------------|---------------------------------------|
| `Add(a, b)`         | `Object(Object, Object)`     | Component-wise addition               |
| `Sub(a, b)`         | `Object(Object, Object)`     | Component-wise subtraction            |
| `Mul(a, b)`         | `Object(Object, Object)`     | Matrix multiplication                 |
| `MulScalar(m, s)`   | `Object(Object, f64)`        | Multiply every element by scalar      |
| `Negate(m)`         | `Object(Object)`             | Negate every element                  |
| `Transpose(m)`      | `Object(Object)`             | Transpose rows and columns            |
| `Inverse(m)`        | `Object(Object)`             | Matrix inverse (traps if singular)    |
| `Determinant(m)`    | `Double(Object)`                | Determinant                           |

### Transform Application

| Method                 | Signature                | Description                                      |
|------------------------|--------------------------|--------------------------------------------------|
| `TransformPoint(m, v)` | `Object(Object, Object)` | Transform a Vec2 point (applies translation)     |
| `TransformVector(m, v)`   | `Object(Object, Object)` | Transform a Vec2 direction (ignores translation) |
| `ApproxEquals(a, b, eps)`        | `Boolean(Object, Object, f64)` | True if every absolute difference is at most the effective tolerance |

### Notes

- Matrices are stored in row-major order.
- `TransformPoint` treats the Vec2 as a homogeneous point (w=1); translation is applied.
- `TransformVector` treats the Vec2 as a direction (w=0); translation is not applied.
- `Get` returns 0 for an out-of-range index; `Row` and `Col` return a zero Vec3.
- `Rotate` returns identity for a non-finite angle. `Inverse` traps for a non-finite determinant or
  when `abs(det) < 1e-15`, not just for exact zero.
- `Eq` substitutes `1e-9` when `eps <= 0` or when `eps` is non-finite (NaN/Inf), and accepts
  differences equal to the tolerance. A NaN component makes a matrix unequal to any matrix
  (including itself), matching IEEE semantics; signed zeros (`-0.0` vs `0.0`) compare equal.
- All factory methods return a new matrix; matrices are immutable.

### Zia Example

```zia
module Mat3Demo;

bind Zanna.Terminal;
bind Zanna.Math.Mat3 as Mat3;
bind Zanna.Math.Vec2 as Vec2;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Build a combined 2D transform: scale then translate
    var s  = Mat3.Scale(2.0, 2.0);
    var t  = Mat3.Translate(10.0, 5.0);
    var m  = Mat3.Mul(t, s);   // apply scale first, then translate

    // Transform a point
    var p      = Vec2.New(1.0, 1.0);
    var result = Mat3.TransformPoint(m, p);
    // Scaled: (2, 2) then translated: (12, 7)
    Say("X: " + Fmt.NumFixed(Vec2.get_X(result), 1));  // 12.0
    Say("Y: " + Fmt.NumFixed(Vec2.get_Y(result), 1));  // 7.0

    // Rotation by 90 degrees
    var r   = Mat3.Rotate(1.5707963);
    var dir = Vec2.New(1.0, 0.0);
    var rd  = Mat3.TransformVector(r, dir);
    Say("Rotated X: " + Fmt.NumFixed(Vec2.get_X(rd), 1));  // ~0.0
    Say("Rotated Y: " + Fmt.NumFixed(Vec2.get_Y(rd), 1));  // ~1.0

    // Determinant and inverse
    Say("Det: " + Fmt.NumFixed(Mat3.Determinant(m), 1));   // 4.0 (scale factor²)
    var inv = Mat3.Inverse(m);
    var chk = Mat3.Mul(m, inv);
    Say("[0,0] should be 1: " + Fmt.NumFixed(Mat3.Get(chk, 0, 0), 4));
}
```

### BASIC Example

```basic
' 2D transform pipeline
DIM scale AS OBJECT = Zanna.Math.Mat3.Scale(3.0, 3.0)
DIM rot   AS OBJECT = Zanna.Math.Mat3.Rotate(Zanna.Math.ToRadians(45.0))
DIM trans AS OBJECT = Zanna.Math.Mat3.Translate(100.0, 50.0)

' Compose: scale → rotate → translate (right to left application order)
DIM m AS OBJECT = Zanna.Math.Mat3.Mul(trans, Zanna.Math.Mat3.Mul(rot, scale))

' Transform a point
DIM pt AS OBJECT = Zanna.Math.Vec2.New(1.0, 0.0)
DIM out AS OBJECT = Zanna.Math.Mat3.TransformPoint(m, pt)
PRINT "x="; Zanna.Math.Vec2.get_X(out); " y="; Zanna.Math.Vec2.get_Y(out)

' Identity check
DIM id AS OBJECT = Zanna.Math.Mat3.Identity()
PRINT "Det(I): "; Zanna.Math.Mat3.Determinant(id)   ' 1.0

' Epsilon comparison
DIM a AS OBJECT = Zanna.Math.Mat3.Identity()
DIM b AS OBJECT = Zanna.Math.Mat3.Mul(a, Zanna.Math.Mat3.Identity())
PRINT Zanna.Math.Mat3.ApproxEquals(a, b, 0.0001)   ' True

' Row and column extraction
DIM row0 AS OBJECT = Zanna.Math.Mat3.Row(id, 0)
PRINT Zanna.Math.Vec3.get_X(row0)   ' 1.0 (first row of identity)
```

---

## Zanna.Math.Mat4

4×4 matrix for 3D transformations, including translation, rotation, scale, and projection. All methods are static;
matrix values are opaque objects. Pass the matrix as the first argument to instance-style methods.

**Type:** Static utility class

### Constructors

| Method         | Signature    | Description                              |
|----------------|--------------|------------------------------------------|
| `New(m00..m33)` | `Object(f64×16)` | Create from 16 row-major floats      |
| `Identity()`   | `Object()`   | 4×4 identity matrix                      |
| `Zero()`       | `Object()`   | 4×4 zero matrix                          |

### 3D Transform Factories

| Method                      | Signature                        | Description                                    |
|-----------------------------|----------------------------------|------------------------------------------------|
| `Translate(x, y, z)`        | `Object(f64, f64, f64)`          | Translation matrix                             |
| `Scale(x, y, z)`            | `Object(f64, f64, f64)`          | Non-uniform scale matrix                       |
| `ScaleUniform(s)`           | `Object(f64)`                    | Uniform scale matrix                           |
| `RotateX(angle)`            | `Object(f64)`                    | Rotation around X axis (radians)               |
| `RotateY(angle)`            | `Object(f64)`                    | Rotation around Y axis (radians)               |
| `RotateZ(angle)`            | `Object(f64)`                    | Rotation around Z axis (radians)               |
| `RotateAxis(axis, angle)`   | `Object(Object, f64)`            | Rotation around an arbitrary Vec3 axis         |

### Projection Factories

| Method                              | Signature                               | Description                              |
|-------------------------------------|-----------------------------------------|------------------------------------------|
| `Perspective(fov, aspect, near, far)` | `Object(f64, f64, f64, f64)`          | Right-handed perspective projection (vertical fov in radians) |
| `Orthographic(l, r, b, t, near, far)`      | `Object(f64, f64, f64, f64, f64, f64)` | Orthographic projection matrix           |
| `LookAt(eye, center, up)`           | `Object(Object, Object, Object)`        | View matrix from eye/center/up (Vec3)    |

### Element Access and Math

| Method              | Signature                          | Description                           |
|---------------------|------------------------------------|---------------------------------------|
| `Get(m, row, col)`  | `Double(Object, Integer, Integer)`    | Get element at (row, col) 0-indexed   |
| `Add(a, b)`         | `Object(Object, Object)`           | Component-wise addition               |
| `Sub(a, b)`         | `Object(Object, Object)`           | Component-wise subtraction            |
| `Mul(a, b)`         | `Object(Object, Object)`           | Matrix multiplication                 |
| `MulScalar(m, s)`   | `Object(Object, f64)`              | Multiply every element by scalar      |
| `Negate(m)`         | `Object(Object)`                   | Negate every element                  |
| `Transpose(m)`      | `Object(Object)`                   | Transpose rows and columns            |
| `Inverse(m)`        | `Object(Object)`                   | Matrix inverse (traps if singular)             |
| `Determinant(m)`    | `Double(Object)`                      | Determinant                           |
| `ApproxEquals(a, b, eps)`     | `Boolean(Object, Object, f64)`     | True if every absolute difference is at most the effective tolerance |

### Transform Application

| Method                 | Signature                | Description                                       |
|------------------------|--------------------------|---------------------------------------------------|
| `TransformPoint(m, v)` | `Object(Object, Object)` | Transform a Vec3 point and perform perspective divide |
| `TransformVector(m, v)`   | `Object(Object, Object)` | Transform a Vec3 direction (ignores translation)  |

### Notes

- Matrices are stored in row-major order.
- Matrices multiply column vectors; storage order does not change the projection convention.
- `LookAt` expects right-handed coordinate system (OpenGL convention).
- `Perspective` uses the standard OpenGL depth range [-1, 1].
- Invalid/non-finite projection parameters, a degenerate `LookAt`, and a zero/non-finite
  `RotateAxis` input return identity rather than trapping. Perspective requires `0 < fov < pi`,
  positive aspect and near plane, and `near < far`.
- `TransformPoint` returns normalized-device coordinates after dividing by homogeneous `w`; it
  returns the zero vector for non-finite input/output or `abs(w) <= 1e-15`.
- `Get` returns 0 for invalid indices. `Inverse` traps (`singular matrix`) for a null/invalid
  receiver or a non-finite / near-zero determinant (`abs(det) < 1e-15`) — it never returns
  identity as an error sentinel, so its failure contract matches `Mat3.Inverse`.
- `Eq` uses `1e-9` when `eps <= 0` or non-finite, accepts equality at the tolerance, and treats a
  NaN component as unequal (never equal, including to itself), the same NaN-safe contract as
  `Mat3.ApproxEquals`.
- Composing transforms: `Mul(B, A)` applies A first, then B (right-to-left).

### Zia Example

```zia
module Mat4Demo;

bind Zanna.Terminal;
bind Zanna.Math.Mat4 as Mat4;
bind Zanna.Math.Vec3 as Vec3;
bind Zanna.Math as Math;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Simple model matrix: scale → rotateY → translate
    var s = Mat4.Scale(2.0, 2.0, 2.0);
    var r = Mat4.RotateY(Math.ToRadians(45.0));
    var t = Mat4.Translate(0.0, 0.0, -5.0);
    var model = Mat4.Mul(t, Mat4.Mul(r, s));

    // Camera view matrix
    var eye    = Vec3.New(0.0, 3.0, 8.0);
    var center = Vec3.New(0.0, 0.0, 0.0);
    var up     = Vec3.New(0.0, 1.0, 0.0);
    var view   = Mat4.LookAt(eye, center, up);

    // Perspective projection
    var proj = Mat4.Perspective(Math.ToRadians(60.0), 16.0 / 9.0, 0.1, 1000.0);

    // MVP matrix
    var mvp = Mat4.Mul(proj, Mat4.Mul(view, model));

    // Transform a point
    var pt  = Vec3.New(1.0, 0.0, 0.0);
    var out = Mat4.TransformPoint(mvp, pt);
    Say("Transformed X: " + Fmt.NumFixed(Vec3.get_X(out), 4));

    // Verify identity round-trip
    var id  = Mat4.Identity();
    var inv = Mat4.Inverse(id);
    var chk = Mat4.Mul(id, inv);
    Say("[0,0]: " + Fmt.NumFixed(Mat4.Get(chk, 0, 0), 2));  // 1.00
}
```

### BASIC Example

```basic
' Build a 3D model-view-projection matrix
DIM scale AS OBJECT = Zanna.Math.Mat4.Scale(1.0, 1.0, 1.0)
DIM rotY  AS OBJECT = Zanna.Math.Mat4.RotateY(Zanna.Math.ToRadians(30.0))
DIM trans AS OBJECT = Zanna.Math.Mat4.Translate(0.0, 0.0, -10.0)

DIM model AS OBJECT = Zanna.Math.Mat4.Mul(trans, Zanna.Math.Mat4.Mul(rotY, scale))

' Camera
DIM eye    AS OBJECT = Zanna.Math.Vec3.New(0.0, 5.0, 10.0)
DIM center AS OBJECT = Zanna.Math.Vec3.New(0.0, 0.0, 0.0)
DIM up     AS OBJECT = Zanna.Math.Vec3.New(0.0, 1.0, 0.0)
DIM view   AS OBJECT = Zanna.Math.Mat4.LookAt(eye, center, up)

' Projection
DIM proj AS OBJECT = Zanna.Math.Mat4.Perspective(Zanna.Math.ToRadians(60.0), 1.777, 0.1, 1000.0)

' Combined MVP
DIM mvp AS OBJECT = Zanna.Math.Mat4.Mul(proj, Zanna.Math.Mat4.Mul(view, model))

' Transform a world-space point into normalized-device coordinates
DIM pt  AS OBJECT = Zanna.Math.Vec3.New(0.0, 0.0, 0.0)
DIM out AS OBJECT = Zanna.Math.Mat4.TransformPoint(mvp, pt)
PRINT "NDC X: "; Zanna.Math.Vec3.get_X(out)
PRINT "NDC Y: "; Zanna.Math.Vec3.get_Y(out)

' Orthographic projection for 2D/UI overlay
DIM ortho AS OBJECT = Zanna.Math.Mat4.Orthographic(0.0, 1920.0, 0.0, 1080.0, -1.0, 1.0)
PRINT "Det(ortho): "; Zanna.Math.Mat4.Determinant(ortho)

' Rotate around arbitrary axis
DIM axis    AS OBJECT = Zanna.Math.Vec3.New(1.0, 1.0, 0.0)
DIM rotAxis AS OBJECT = Zanna.Math.Mat4.RotateAxis(axis, Zanna.Math.ToRadians(45.0))
```

---

## See Also

- [Graphics](graphics/README.md) - Use `Vec2`, `Vec3`, `Mat3`, `Mat4`, and `Quat` with `Canvas` and `Pixels` for 2D/3D graphics
- [Cryptography](crypto.md) - `Rand` for cryptographically secure randomness
