---
status: active
audience: public
last-verified: 2026-07-26
---

# Time & Timing

> Date/time operations, timing, and performance measurement.

**Part of the [Zanna Runtime Library](README.md)**

## Contents

- [Zanna.Time.Clock](#zannatimeclock)
- [Zanna.Time.Countdown](#zannatimecountdown)
- [Zanna.Time.DateOnly](#zannatimedateonly)
- [Zanna.Time.DateRange](#zannatimedaterange)
- [Zanna.Time.DateTime](#zannatimedatetime)
- [Zanna.Time.Duration](#zannatimeduration)
- [Zanna.Time.RelativeTime](#zannatimerelativetime)
- [Zanna.Time.Stopwatch](#zannatimestopwatch)
- [Zanna.Time.TimeZone](#zannatimetimezone)

---

## Zanna.Time.Clock

Basic timing utilities for sleeping and measuring elapsed time.

**Type:** Static utility class

### Methods

| Method      | Signature       | Description                                                       |
|-------------|-----------------|-------------------------------------------------------------------|
| `Sleep(ms)` | `Void(Integer)` | Pause execution for the specified number of milliseconds          |
| `NowMs()`   | `Integer()`     | Returns monotonic time in milliseconds since an unspecified epoch |
| `NowMicros()` | `Integer()`     | Returns monotonic time in microseconds since an unspecified epoch |

### Notes

- `NowMs()` and `NowMicros()` use the platform's monotonic clock when it is available. On POSIX,
  failure of `CLOCK_MONOTONIC` falls back to `CLOCK_REALTIME`. That fallback reading is passed
  through a process-local floor (a lock-free compare-and-swap maximum), so the values these helpers
  return stay non-decreasing even if the wall clock is adjusted backward (VDOC-223). If all usable
  clock queries fail, these helpers return `0`.
- The epoch (starting point) is unspecified. Use tick values for elapsed durations, not absolute
  dates or serialization.
- `NowMicros()` reports microsecond units; neither method promises that much physical clock
  resolution.
- Tick conversions trap on signed 64-bit overflow instead of wrapping.
- Negative sleep values are treated as `0`. Values above `2,147,483,647` ms are clamped to that
  limit (about 24.85 days) for the platform sleep primitive. A zero-duration call does not block,
  although Windows may yield the caller's remaining time slice.

### Zia Example

```zia
module ClockDemo;

bind Zanna.Terminal;
bind Zanna.Time.Clock as Clock;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var t1 = Clock.NowMs();
    // Perform the work being measured here.
    var t2 = Clock.NowMs();
    Say("Elapsed: " + Fmt.Int(t2 - t1) + " ms");
    Say("Ticks (us): " + Fmt.Int(Clock.NowMicros()));
}
```

### BASIC Example

```basic
' Measure execution time
DIM startMs AS INTEGER = Zanna.Time.Clock.NowMs()

' Do a small unit of work
FOR i = 1 TO 1000000
    ' busy loop
NEXT

DIM endMs AS INTEGER = Zanna.Time.Clock.NowMs()
PRINT "Elapsed time: "; endMs - startMs; " ms"

' High-precision timing with microseconds
DIM startUs AS INTEGER = Zanna.Time.Clock.NowMicros()
' Perform the fast operation here
DIM endUs AS INTEGER = Zanna.Time.Clock.NowMicros()
PRINT "Elapsed: "; endUs - startUs; " microseconds"

' Sleep for a short delay
Zanna.Time.Clock.Sleep(100)  ' Sleep for 100ms
```

---

## Zanna.Time.Countdown

A countdown timer for implementing timeouts, delays, and timed operations. Tracks remaining time and signals when the
interval has expired.

**Type:** Instance class (requires `New(interval)`)

### Constructor

| Method          | Signature            | Description                                                      |
|-----------------|----------------------|------------------------------------------------------------------|
| `New(interval)` | `Countdown(Integer)` | Create a countdown timer with specified interval in milliseconds |

### Properties

| Property    | Type                   | Description                                            |
|-------------|------------------------|--------------------------------------------------------|
| `Elapsed`   | `Integer` (read-only)  | Milliseconds elapsed since start                       |
| `Remaining` | `Integer` (read-only)  | Milliseconds remaining until expiration (0 if expired) |
| `IsExpired` | `Boolean` (read-only)  | True if the countdown has finished                     |
| `Interval`  | `Integer` (read/write) | The countdown duration in milliseconds                 |
| `IsRunning` | `Boolean` (read-only)  | True if the countdown is currently running             |

### Methods

| Method    | Signature | Description                                    |
|-----------|-----------|------------------------------------------------|
| `Start()` | `Void()`  | Start or resume the countdown                  |
| `Stop()`  | `Void()`  | Pause the countdown, preserving remaining time |
| `Reset()` | `Void()`  | Stop and reset to full interval                |
| `Wait()`  | `Void()`  | Block until the countdown expires              |

### Notes

- Unlike Stopwatch (which counts up), Countdown counts down from an interval
- `Remaining` returns 0 once expired (never negative)
- `Expired` becomes true when elapsed time reaches or exceeds the interval
- `Wait()` blocks the current thread until expiration; returns immediately if already expired
- `Wait()` starts a stopped countdown before waiting
- `Wait()` handles intervals larger than the platform's single-sleep limit by sleeping in chunks until expired
- Changing `Interval` while running affects when `Expired` becomes true
- Constructor and assigned interval values less than or equal to zero are clamped to zero; such a countdown reports expired immediately
- Elapsed time continues increasing after expiration while the countdown remains running. `Wait()`
  does not stop it when it returns; call `Stop()` if the final elapsed value must remain fixed.
- Countdown instances are not thread-safe; synchronize externally if an instance is shared
- Instance methods and properties require a valid Countdown object; a null receiver, or an
  explicit receiver of another runtime class, traps rather than being reinterpreted (VDOC-229)
- Countdown timing inherits `Clock`'s wall-clock fallback if the platform monotonic query fails;
  that fallback is ratcheted to stay non-decreasing so the deadline cannot recede (VDOC-223). On
  Windows, the performance-counter frequency is cached through an acquire/release atomic slot, so
  concurrent first use is well-defined (VDOC-224).

### Zia Example

```zia
module CountdownDemo;

bind Zanna.Terminal;
bind Zanna.Time.Countdown as Countdown;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var timer = Countdown.New(5000);
    Say("Interval: " + Fmt.Int(timer.get_Interval()));  // 5000
    Say("Running: " + Fmt.Bool(timer.get_IsRunning())); // false
    timer.Start();
    timer.Stop();
}
```

### BASIC Example

```basic
' Create a 5-second countdown
DIM timer AS Zanna.Time.Countdown = Zanna.Time.Countdown.New(5000)
timer.Start()

' Game loop with timeout
WHILE NOT timer.IsExpired
    PRINT "Time remaining: "; timer.Remaining; " ms"
    Zanna.Time.Clock.Sleep(500)
WEND
PRINT "Time's up!"

' Use for operation timeout
DIM timeout AS Zanna.Time.Countdown = Zanna.Time.Countdown.New(1000)
timeout.Start()

WHILE NOT operationComplete AND NOT timeout.IsExpired
    ' Poll the surrounding operation here
WEND

IF timeout.IsExpired THEN
    PRINT "Operation timed out"
END IF

' Blocking wait
DIM delay AS Zanna.Time.Countdown = Zanna.Time.Countdown.New(2000)
delay.Start()
PRINT "Waiting 2 seconds..."
delay.Wait()
PRINT "Done!"
```

### Comparison with Clock.Sleep

| Use Case                      | Recommended       |
|-------------------------------|-------------------|
| Simple fixed delay            | `Clock.Sleep(ms)` |
| Timeout with early exit       | `Countdown`       |
| Progress tracking during wait | `Countdown`       |
| Pause/resume timing           | `Countdown`       |

---

## Zanna.Time.DateTime

Date and time operations. Timestamps are Unix timestamps (seconds since January 1, 1970 UTC).

**Type:** Static utility class

### Methods

| Method                           | Signature                   | Description                                              |
|----------------------------------|-----------------------------|----------------------------------------------------------|
| `Now()`                          | `Integer()`                 | Returns the current Unix timestamp (seconds)             |
| `NowMs()`                        | `Integer()`                 | Returns the current timestamp in milliseconds            |
| `Year(timestamp)`                | `Integer(Integer)`          | Extracts the year from a timestamp                       |
| `Month(timestamp)`               | `Integer(Integer)`          | Extracts the month (1-12) from a timestamp               |
| `Day(timestamp)`                 | `Integer(Integer)`          | Extracts the day of month (1-31) from a timestamp        |
| `Hour(timestamp)`                | `Integer(Integer)`          | Extracts the hour (0-23) from a timestamp                |
| `Minute(timestamp)`              | `Integer(Integer)`          | Extracts the minute (0-59) from a timestamp              |
| `Second(timestamp)`              | `Integer(Integer)`          | Extracts the second (0-59) from a timestamp              |
| `DayOfWeek(timestamp)`           | `Integer(Integer)`          | Returns day of week (0=Sunday, 6=Saturday)               |
| `Format(timestamp, format)`      | `String(Integer, String)`   | Formats a timestamp using strftime-style format          |
| `FormatInZone(timestamp, zone, format)` | `String(Integer, TimeZone, String)` | Formats a UTC instant in a named time zone |
| `ToIso8601(timestamp)`               | `String(Integer)`           | Returns ISO 8601 formatted string in UTC (with Z suffix) |
| `FormatLocal(timestamp)`             | `String(Integer)`           | Returns ISO 8601 formatted string in local time (no Z)   |
| `ToZone(timestamp, zone)`        | `String(Integer, TimeZone)` | Returns ISO 8601 local wall time plus numeric zone offset |
| `FromParts(y, m, d, h, min, s)`     | `Integer(Integer...)`       | Creates a timestamp from local time components           |
| `AddSeconds(timestamp, seconds)` | `Integer(Integer, Integer)` | Adds seconds to a timestamp                              |
| `AddDays(timestamp, days)`       | `Integer(Integer, Integer)` | Adds days to a timestamp                                 |
| `Diff(t1, t2)`                   | `Integer(Integer, Integer)` | Returns the difference in seconds between two timestamps |
| `ParseIso8601(str)`                  | `Integer(String)`           | Parse an ISO 8601 datetime string to Unix timestamp      |
| `ParseDate(str)`                 | `Integer(String)`           | Parse a "YYYY-MM-DD" string to Unix timestamp            |
| `ParseTime(str)`                 | `Integer(String)`           | Parse a "HH:MM:SS" string to seconds since midnight      |
| `TryParse(str)`                  | `Option[Integer](String)`   | Auto-detect format and parse; returns `None` on failure  |
| `TryFromParts(y, m, d, h, min, s)` | `Option[Integer](Integer...)` | Like `FromParts` but returns `Some` on success and `None` on failure, so pre-epoch `-1` is unambiguous |

### Notes

- `Create` interprets components in the **local time zone**
- `Create` returns `-1` if components are out of range, normalize to a different value (for example month 13 or day 32), or the resulting local timestamp cannot be represented safely by the platform time APIs
- `Year`, `Month`, `Day`, `Hour`, `Minute`, `Second` extract components in the **local time zone**
- `ToIso8601` formats in **UTC** (appends `Z` suffix)
- `FormatLocal` formats in the **local time zone** (no `Z` suffix) — use this for consistent round-trips with `Create`
- `Format` uses the **local time zone** with strftime-style format strings
- `Format` is host-dependent: supported conversions, localized names, and the active C locale come
  from the platform `strftime`. The fixed buffer permits at most 255 output bytes; an empty,
  embedded-NUL, unsupported/oversized, or otherwise failing format returns `""`.
- `ToZone` and `FormatInZone` use Zanna's embedded named-zone table, not host OS zoneinfo, so output is deterministic across macOS, Windows, and Linux for covered zones
- `FormatInZone` currently supports `%Y`, `%m`, `%d`, `%H`, `%M`, `%S`, `%F`, `%T`, `%z`, `%Z`, and `%%`
- An unsupported or dangling conversion in `FormatInZone`, an empty/embedded-NUL format, or output
  beyond its 512-byte buffer returns `""`.
- Timestamp accessors and formatters return 0 or an empty string when a timestamp cannot be represented by the platform `time_t`
- `Diff(t1, t2)` returns the signed difference `t1 - t2` in seconds
- `AddSeconds`, `AddDays`, and `Diff` trap on signed 64-bit overflow
- `AddDays` adds exactly 86,400 seconds per day; it is elapsed-time arithmetic and does not preserve
  a local wall-clock hour across daylight-saving transitions.
- `Now` and `NowMs` read the adjustable wall clock, not a monotonic timer. `NowMs` returns `0` when
  its platform query fails. `Now` reads the clock through a checked helper that distinguishes a
  genuine `time(NULL)` failure (detected via `errno`) from the valid pre-epoch instant `-1`; on a
  real clock failure it traps rather than returning an ambiguous `-1`, so a failure can never
  masquerade as a 1969 timestamp (VDOC-230). `DateOnly.Today` returns `Nothing` and RelativeTime's
  implicit current-time helpers trap on the same failure.
- `FromParts` (the sentinel construction form) uses `-1` for rejected/unrepresentable input, but
  `-1` is also the valid Unix instant immediately before the epoch. Use `TryFromParts`, which
  returns `Some(Integer)` for any valid instant — including that pre-epoch `-1` — and `None` on
  failure, when construction failure must be distinguished from a genuine pre-epoch result
  (VDOC-225).
- Local civil-time creation (`FromParts`/`TryFromParts` and local ISO/date parsing) rejects a
  skipped daylight-saving hour. A repeated ("fall back") hour resolves **deterministically to the
  earlier occurrence** (the one still in daylight time) on every host, rather than whichever the
  host `mktime` would pick — the converter evaluates both DST interpretations, keeps only those
  whose fields round-trip, and selects the earliest instant (VDOC-226). Use an explicit numeric
  offset or a named-zone UTC instant when you need the later (standard-time) occurrence instead.

### Parsing Methods

| Method       | Input Format                          | Returns                                |
|--------------|---------------------------------------|----------------------------------------|
| `ParseIso8601`   | `"2025-06-15T14:30:00Z"`, `"2025-06-15T14:30:00.123Z"`, or `"2025-06-15T14:30:00+02:00"` | Unix timestamp (seconds) |
| `ParseDate`  | `"2025-06-15"`                        | Unix timestamp (seconds, midnight)     |
| `ParseTime`  | `"14:30"`, `"14:30:00"`, or `"14:30:00.123"` | Seconds since midnight (0-86399) |
| `TryParse`   | Any of the above formats              | `Some(Integer)` on success, `None` on failure |

- `ParseIso8601` accepts exactly four year digits, `T`, `t`, or a space as the date/time separator,
  optional fractional seconds (discarded because timestamps store whole seconds), and `Z`/`z` or
  numeric `+HH:MM`/`-HH:MM` offsets. Numeric offsets are limited to `-14:00` through `+14:00`.
  `Z` and numeric offsets identify UTC instants; no suffix means local time and inherits the
  skipped/repeated-hour behavior above.
- `ParseDate` parses exact `YYYY-MM-DD` strings and returns the timestamp at local midnight
- `ParseTime` returns seconds since midnight (not a Unix timestamp) for exact `HH:MM`, `HH:MM:SS`, or `HH:MM:SS.fraction` strings
- Invalid calendar values, out-of-range times, and trailing characters are rejected
- On failure, `ParseIso8601` and `ParseDate` return `0`, while `ParseTime` returns `-1`; use `TryParse` when the Unix epoch must be distinguishable from failure
- `TryParse` preserves a valid Unix epoch or midnight parse as `Some(0)`. A time-only input
  produces seconds since midnight; a date or datetime input produces a Unix timestamp. The former
  `TryParse` numeric-sentinel target remains implemented in C for binary/internal compatibility
  but is no longer registered in the current public runtime surface.

### Zia Example

```zia
module DateTimeDemo;

bind Zanna.Terminal;
bind Zanna.Time.DateTime as DateTime;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var now = DateTime.Now();
    Say("Year: " + Fmt.Int(DateTime.Year(now)));
    Say("Month: " + Fmt.Int(DateTime.Month(now)));
    Say("Day: " + Fmt.Int(DateTime.Day(now)));
    Say("Hour: " + Fmt.Int(DateTime.Hour(now)));

    // Local ISO format (consistent with Create)
    Say("Local: " + DateTime.FormatLocal(now));

    // UTC ISO format
    Say("UTC: " + DateTime.ToIso8601(now));

    // Create a specific date and format it
    var christmas = DateTime.FromParts(2025, 12, 25, 12, 0, 0);
    Say("Christmas: " + DateTime.FormatLocal(christmas));
}
```

### BASIC Example

```basic
' Get current time
DIM now AS INTEGER
now = Zanna.Time.DateTime.Now()

' Extract components
PRINT "Year: "; Zanna.Time.DateTime.Year(now)
PRINT "Month: "; Zanna.Time.DateTime.Month(now)
PRINT "Day: "; Zanna.Time.DateTime.Day(now)
PRINT "Hour: "; Zanna.Time.DateTime.Hour(now)

' Format as a local-time string
PRINT Zanna.Time.DateTime.Format(now, "%Y-%m-%d %H:%M:%S")

' Local ISO (no Z suffix, consistent with Create)
PRINT Zanna.Time.DateTime.FormatLocal(now)

' UTC ISO (with Z suffix)
PRINT Zanna.Time.DateTime.ToIso8601(now)

' Create a specific local date
DIM birthday AS INTEGER
birthday = Zanna.Time.DateTime.FromParts(1990, 6, 15, 0, 0, 0)

' Elapsed-time arithmetic
DIM tomorrow AS INTEGER
tomorrow = Zanna.Time.DateTime.AddDays(now, 1)

DIM age_seconds AS INTEGER
age_seconds = Zanna.Time.DateTime.Diff(now, birthday)
```

### Local Format Specifiers

`DateTime.Format`, unlike `FormatInZone`, delegates to the host `strftime`. Common conversions
include:

| Specifier | Description             | Example  |
|-----------|-------------------------|----------|
| `%Y`      | Year, minimum four digits | 2025   |
| `%m`      | Two-digit month         | 01-12    |
| `%d`      | Two-digit day           | 01-31    |
| `%H`      | 24-hour hour            | 00-23    |
| `%I`      | 12-hour hour            | 01-12    |
| `%M`      | Minute                  | 00-59    |
| `%S`      | Second                  | 00-60    |
| `%p`      | Locale AM/PM marker     | PM       |
| `%A`, `%a`| Full/abbreviated weekday| Monday / Mon |
| `%B`, `%b`| Full/abbreviated month  | January / Jan |
| `%%`      | Literal percent sign    | %        |

The platform may support additional conversions; portable code should not assume the same
extensions or localized text across hosts.

### Named-zone Example

```zia
module TimeZoneDemo;

bind Zanna.Terminal;
bind Zanna.Time.DateTime as DateTime;
bind Zanna.Time.TimeZone as TimeZone;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var ny = TimeZone.Find("America/New_York");
    var instant = DateTime.ParseIso8601("2025-03-09T07:00:00Z");
    Say(DateTime.ToZone(instant, ny));                         // 2025-03-09T03:00:00-04:00
    Say(DateTime.FormatInZone(instant, ny, "%F %T %z %Z"));    // 2025-03-09 03:00:00 -0400 EDT
    Say(TimeZone.get_Name(ny));                               // America/New_York
    Say(Fmt.Int(TimeZone.OffsetAt(ny, instant)));             // -14400
}
```

---

## Zanna.Time.TimeZone

Deterministic IANA named-zone lookup backed by an embedded subset. TimeZone handles are used with `DateTime.ToZone` and `DateTime.FormatInZone`.

**Type:** Static lookup plus opaque zone handles

### Methods and Properties

| Method / Property | Signature | Description |
|-------------------|-----------|-------------|
| `Find(name)` | `TimeZone(String)` | Resolve a supported IANA zone name; traps for unknown names |
| `Name` | `String` | Canonical zone name |
| `OffsetAt(timestamp)` | `Integer(Integer)` | UTC offset in seconds east of UTC at the instant |
| `IsDstAt(timestamp)` | `Boolean(Integer)` | True when daylight saving time is active |

### Embedded Subset

The initial deterministic subset covers:

| Zone | Coverage |
|------|----------|
| `UTC`, `Etc/UTC` | Fixed UTC |
| `Asia/Tokyo` | Fixed UTC+09:00 |
| `America/New_York` | 2025-2026 EST/EDT transitions |
| `Australia/Sydney` | 2025-2026 AEST/AEDT transitions |

`Find` is exact and case-sensitive. Unknown zones trap with a runtime error rather than falling back to local time.
The New York and Sydney rules are authoritative only for the listed 2025-2026 window. Before the first embedded
transition the table uses its configured base rule; after the last transition it keeps the final rule indefinitely,
so dates outside the window are deterministic but may not match historical or future IANA data.

Empty, null, and embedded-NUL names also trap. TimeZone handles point at process-lifetime static
data and must not be manually freed — the runtime reports them as `borrowed`, so tools never
schedule a release. `Find` now returns a concrete `Zanna.Time.TimeZone`, so Zia can chain
`TimeZone.Find("UTC").OffsetAt(...)` and assign the result to a TimeZone-typed local directly
(VDOC-228). The explicit-receiver forms `TimeZone.get_Name(zone)`, `TimeZone.OffsetAt(zone, ts)`,
and `TimeZone.IsDstAt(zone, ts)` remain available as compatibility entry points.

### DST Policy

`TimeZone` converts UTC instants to wall time, so skipped and repeated local hours are resolved by the instant itself. For example, `2025-03-09T07:00:00Z` in `America/New_York` formats as `2025-03-09T03:00:00-04:00`, skipping the nonexistent `02:00` hour.

### BASIC Handle Example

```basic
DIM zone AS OBJECT = Zanna.Time.TimeZone.Find("UTC")
PRINT Zanna.Time.TimeZone.get_Name(zone)       ' UTC
PRINT Zanna.Time.TimeZone.OffsetAt(zone, 0)    ' 0
PRINT Zanna.Time.TimeZone.IsDstAt(zone, 0)     ' 0

DIM instant AS INTEGER = Zanna.Time.DateTime.ParseIso8601("2025-03-09T07:00:00Z")
PRINT Zanna.Time.DateTime.ToZone(instant, zone)
```

---

## Zanna.Time.Stopwatch

High-precision stopwatch for benchmarking and performance measurement. Supports pause/resume timing and reports
elapsed values down to nanosecond units.

**Type:** Instance class (requires `New()` or `StartNew()`)

### Constructors

| Method       | Signature     | Description                                  |
|--------------|---------------|----------------------------------------------|
| `New()`      | `Stopwatch()` | Create a new stopped stopwatch               |
| `StartNew()` | `Stopwatch()` | Create and immediately start a new stopwatch |

### Properties

| Property    | Type                  | Description                            |
|-------------|-----------------------|----------------------------------------|
| `ElapsedMs` | `Integer` (read-only) | Total elapsed time in milliseconds     |
| `ElapsedUs` | `Integer` (read-only) | Total elapsed time in microseconds     |
| `ElapsedNs` | `Integer` (read-only) | Total elapsed time in nanoseconds      |
| `IsRunning` | `Boolean` (read-only) | True if stopwatch is currently running |

### Methods

| Method      | Signature | Description                                           |
|-------------|-----------|-------------------------------------------------------|
| `Start()`   | `Void()`  | Start or resume timing (no effect if already running) |
| `Stop()`    | `Void()`  | Pause timing, preserving accumulated time             |
| `Reset()`   | `Void()`  | Stop and clear all accumulated time                   |
| `Restart()` | `Void()`  | Reset and start in one convenience operation          |

### Notes

- `ElapsedNs` reports nanosecond units; actual clock resolution is platform-dependent
- Time accumulates across multiple Start/Stop cycles until Reset
- Reading `ElapsedMs`/`ElapsedUs`/`ElapsedNs` while running returns current elapsed time
- Stopwatch timestamp conversions trap on signed 64-bit overflow instead of wrapping
- `Start()` has no effect if already running (doesn't reset)
- `Stop()` has no effect if already stopped
- Instance methods and properties require a valid Stopwatch object; a null receiver, or an
  explicit receiver of another runtime class, traps rather than being reinterpreted (VDOC-229)
- Stopwatch instances are not thread-safe; synchronize externally if an instance is shared
- `Restart()` is not a synchronization primitive or an atomic operation between threads. It is a
  single API call on an otherwise non-thread-safe object.
- On POSIX, failure of the monotonic clock falls back to the realtime clock; that fallback reading
  is ratcheted through a process-local floor so total elapsed time stays non-decreasing even if the
  wall clock is adjusted backward. If both queries fail, the internal timestamp is `0` (VDOC-223).
  On Windows the performance-counter frequency is cached through an acquire/release atomic slot, so
  concurrent first use is well-defined (VDOC-224).

### Zia Example

```zia
module StopwatchDemo;

bind Zanna.Terminal;
bind Zanna.Time.Stopwatch as Stopwatch;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var sw = Stopwatch.New();
    sw.Start();
    // Perform the work to measure here.
    sw.Stop();
    Say("Elapsed: " + Fmt.Int(sw.get_ElapsedUs()) + " us");
}
```

### BASIC Example

```basic
' Create and start a stopwatch
DIM sw AS Zanna.Time.Stopwatch = Zanna.Time.Stopwatch.StartNew()

' Code to benchmark
FOR i = 1 TO 1000000
    DIM x AS INTEGER = i * i
NEXT

sw.Stop()
PRINT "Elapsed: "; sw.ElapsedMs; " ms"
PRINT "Elapsed: "; sw.ElapsedUs; " us"
PRINT "Elapsed: "; sw.ElapsedNs; " ns"

' Resume timing for additional work
sw.Start()
FOR i = 1 TO 500000
    DIM x AS INTEGER = i * i
NEXT
sw.Stop()
PRINT "Total: "; sw.ElapsedMs; " ms"

' Reset and restart
sw.Restart()
' Perform another benchmark here
sw.Stop()
PRINT "New timing: "; sw.ElapsedMs; " ms"
```

---

## Zanna.Time.DateOnly

Date-only type for working with calendar dates without time components. Represents a year, month, and day.

**Type:** Instance class (requires `FromParts(year, month, day)`, `Today()`, `Parse(str)`, or `FromDays(i64)`)

### Constructors

| Method                   | Signature                       | Description                                            |
|--------------------------|---------------------------------|--------------------------------------------------------|
| `FromParts(year, month, day)` | `DateOnly(Integer, Integer, Integer)` | Create a date, or `Nothing` for a year outside `[0,9999]` or an invalid month/day |
| `Today()`                | `DateOnly()`                    | Return today's local date, or `Nothing` if conversion fails |
| `Parse(str)`             | `DateOnly(String)`              | Parse exact `YYYY-MM-DD`, or return `Nothing` on failure |
| `FromDays(days)`         | `DateOnly(Integer)`             | Create a date from a day count (days since epoch)      |

### Properties

| Property     | Type                   | Description                                     |
|--------------|------------------------|-------------------------------------------------|
| `Year`       | `Integer` (read-only)  | The year component                              |
| `Month`      | `Integer` (read-only)  | The month component (1-12)                      |
| `Day`        | `Integer` (read-only)  | The day of month component (1-31)               |
| `DayOfWeek`  | `Integer` (read-only)  | Day of week (0=Sunday, 6=Saturday)              |
| `DayOfYear`  | `Integer` (read-only)  | Day of year (1-366)                             |
| `IsLeapYear` | `Boolean` (read-only)  | True if the year is a leap year                 |
| `DaysInMonth`| `Integer` (read-only)  | Number of days in the current month             |

### Methods

| Method              | Signature              | Description                                              |
|---------------------|------------------------|----------------------------------------------------------|
| `ToDays()`          | `Integer()`            | Convert to day count (days since epoch)                  |
| `AddDays(n)`        | `DateOnly(Integer)`    | Returns a new date with n days added                     |
| `AddMonths(n)`      | `DateOnly(Integer)`    | Returns a new date with n months added                   |
| `AddYears(n)`       | `DateOnly(Integer)`    | Returns a new date with n years added                    |
| `DiffDays(other)`   | `Integer(DateOnly)`    | Returns the number of days between this date and other   |
| `StartOfMonth()`    | `DateOnly()`           | Returns the first day of this date's month               |
| `EndOfMonth()`      | `DateOnly()`           | Returns the last day of this date's month                |
| `StartOfYear()`     | `DateOnly()`           | Returns January 1 of this date's year                    |
| `EndOfYear()`       | `DateOnly()`           | Returns December 31 of this date's year                  |
| `Compare(other)`    | `Integer(DateOnly)`    | Compares dates, returns -1, 0, or 1                      |
| `Equals(other)`     | `Boolean(DateOnly)`    | Returns true if the two dates are equal                  |
| `ToString()`        | `String()`             | Returns date as "YYYY-MM-DD" string                      |
| `Format(fmt)`       | `String(String)`       | Formats the date using a format string                   |

### Notes

- DateOnly is immutable -- all mutation methods return new DateOnly instances
- Calendar arithmetic uses the proleptic Gregorian calendar and is independent of the host time
  zone. Only `Today()` consults local time.
- `DiffDays(other)` returns a negative value if `other` is after this date
- `AddMonths` and `AddYears` clamp the day to the last valid day of the resulting month
- `Create` validates month and day but accepts any signed 64-bit year. `Parse` accepts only exactly
  four ASCII year digits and ten total bytes; non-padded fields, signs, embedded NULs, and trailing
  characters are rejected by returning `Nothing`.
- Construction, formatting, and parsing share one year domain: `Create` (and `Today`, `Parse`,
  `FromDays`) accept only years `0` through `9999`, and `ToString` emits exactly four year digits.
  Every constructible `DateOnly` therefore serializes to a fixed ten-byte string that `Parse` reads
  back — there are no out-of-domain years that `ToString` could render un-parseably (VDOC-231).
  `FromDays` and the `Add*` operations return `Nothing` when a result would leave that domain.
- `Format` supports `%Y`, `%y`, `%m`, `%d`, `%B`, `%b`, `%A`, `%a`, `%j`, and `%%`. Month and
  weekday names are hard-coded English, unknown conversions are preserved literally, and the
  input/output are length-aware (embedded NUL bytes are retained).
- `FromDays`, `ToDays`, `DayOfWeek`, `AddDays`, `AddMonths`, `AddYears`, and `DiffDays` trap on signed 64-bit overflow
- The low-level helpers return neutral values for null receivers and compare two null dates as
  equal. Ordinary source should null-check a failed `Create`, `Today`, `Parse`, `FromDays`, or an
  `Add*` result (which returns `Nothing` when the new date leaves the `[0,9999]` domain) before
  invoking instance members.

### Zia Example

```zia
module DateOnlyDemo;

bind Zanna.Terminal;
bind Zanna.Time.DateOnly as DateOnly;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var d = DateOnly.FromParts(2025, 6, 15);
    Say("Year: " + Fmt.Int(d.get_Year()));           // 2025
    Say("Month: " + Fmt.Int(d.get_Month()));         // 6
    Say("Day: " + Fmt.Int(d.get_Day()));             // 15
    Say("ToString: " + d.ToString());                // 2025-06-15
    Say("IsLeapYear: " + Fmt.Bool(d.get_IsLeapYear())); // false

    var d2 = d.AddDays(10);
    Say("AddDays(10): " + d2.ToString());            // 2025-06-25

    var diff = d.DiffDays(d2);
    Say("DiffDays: " + Fmt.Int(diff));               // -10
}
```

### BASIC Example

```basic
' Create a specific date
DIM d AS OBJECT = Zanna.Time.DateOnly.FromParts(2025, 6, 15)

PRINT "Year: "; d.Year           ' Output: 2025
PRINT "Month: "; d.Month         ' Output: 6
PRINT "Day: "; d.Day             ' Output: 15
PRINT "ToString: "; d.ToString() ' Output: 2025-06-15
PRINT "IsLeapYear: "; d.IsLeapYear ' Output: 0

' Date arithmetic
DIM d2 AS OBJECT = d.AddDays(10)
PRINT "AddDays(10): "; d2.ToString() ' Output: 2025-06-25

DIM diff AS INTEGER = d.DiffDays(d2)
PRINT "DiffDays: "; diff             ' Output: -10

' Start/end of month
DIM som AS OBJECT = d.StartOfMonth()
DIM eom AS OBJECT = d.EndOfMonth()
PRINT "Start of month: "; som.ToString()  ' Output: 2025-06-01
PRINT "End of month: "; eom.ToString()    ' Output: 2025-06-30

' Parse from string
DIM parsed AS OBJECT = Zanna.Time.DateOnly.Parse("2025-12-25")
PRINT "Parsed: "; parsed.ToString()  ' Output: 2025-12-25
' DateOnly.Parse accepts exactly YYYY-MM-DD with no hidden suffix bytes.

' Get today's date
DIM today AS OBJECT = Zanna.Time.DateOnly.Today()
PRINT "Today: "; today.ToString()
```

---

## Zanna.Time.Duration

Duration type for representing and manipulating time spans. Duration is a static value type -- it operates on `i64` millisecond values rather than heap-allocated objects.

**Type:** Static utility class (operates on i64 millisecond values)

### Factory Methods

| Method                              | Signature                                       | Description                                  |
|-------------------------------------|-------------------------------------------------|----------------------------------------------|
| `FromMillis(ms)`                    | `Integer(Integer)`                              | Create duration from milliseconds            |
| `FromSeconds(s)`                    | `Integer(Integer)`                              | Create duration from seconds                 |
| `FromMinutes(m)`                    | `Integer(Integer)`                              | Create duration from minutes                 |
| `FromHours(h)`                      | `Integer(Integer)`                              | Create duration from hours                   |
| `FromDays(d)`                       | `Integer(Integer)`                              | Create duration from days                    |
| `FromParts(days, hours, min, sec, ms)` | `Integer(Integer, Integer, Integer, Integer, Integer)` | Create from individual components    |
| `Zero()`                            | `Integer()`                                     | Returns zero duration (0)                    |

### Total Extraction Methods

| Method              | Signature          | Description                                       |
|---------------------|--------------------|---------------------------------------------------|
| `TotalMillis(dur)`  | `Integer(Integer)` | Returns total milliseconds (identity)             |
| `TotalSeconds(dur)` | `Integer(Integer)` | Returns total whole seconds                       |
| `TotalMinutes(dur)` | `Integer(Integer)` | Returns total whole minutes                       |
| `TotalHours(dur)`   | `Integer(Integer)` | Returns total whole hours                         |
| `TotalDays(dur)`    | `Integer(Integer)` | Returns total whole days                          |
| `TotalSecondsF(dur)`| `Double(Integer)`  | Returns total seconds as floating-point           |

### Component Extraction Methods

| Method          | Signature          | Description                                            |
|-----------------|--------------------|--------------------------------------------------------|
| `Days(dur)`     | `Integer(Integer)` | Whole days in the duration's magnitude                  |
| `Hours(dur)`    | `Integer(Integer)` | Hours component (0-23)                                 |
| `Minutes(dur)`  | `Integer(Integer)` | Minutes component (0-59)                               |
| `Seconds(dur)`  | `Integer(Integer)` | Seconds component (0-59)                               |
| `Millis(dur)`   | `Integer(Integer)` | Milliseconds component (0-999)                         |

### Arithmetic Methods

| Method            | Signature                   | Description                          |
|-------------------|-----------------------------|--------------------------------------|
| `Add(a, b)`       | `Integer(Integer, Integer)` | Add two durations                    |
| `Sub(a, b)`       | `Integer(Integer, Integer)` | Subtract duration b from a           |
| `Mul(dur, factor)` | `Integer(Integer, Integer)` | Multiply duration by integer factor  |
| `Div(dur, divisor)`| `Integer(Integer, Integer)` | Divide duration by integer divisor   |
| `Abs(dur)`        | `Integer(Integer)`          | Absolute value of a duration         |
| `Negate(dur)`     | `Integer(Integer)`          | Negate a duration                    |
| `Compare(a, b)`   | `Integer(Integer, Integer)` | Compare two durations (-1, 0, or 1)  |

### Formatting Methods

| Method          | Signature          | Description                                      |
|-----------------|--------------------|--------------------------------------------------|
| `ToString(dur)` | `String(Integer)` | Format as "d.HH:MM:SS" or "HH:MM:SS" (e.g., "02:00:00", "1.02:30:00") |
| `ToIso8601(dur)`    | `String(Integer)` | Format as ISO 8601 duration (e.g., "PT2H")       |

### Notes

- Duration is a value type -- all methods take and return `i64` millisecond values
- Duration values themselves require no heap object; `ToString` and `ToIso8601` allocate their returned strings
- Negative durations are supported and represent time spans in the past
- Component accessors (`Days`, `Hours`, `Minutes`, `Seconds`, and `Millis`) use the duration's magnitude; use total accessors when the sign must be preserved
- Whole-unit total accessors use signed integer division and truncate toward zero. `Create` does
  not require normalized components: each supplied value is multiplied by its unit and summed, so
  values such as 25 hours or mixed signs are accepted when the calculation does not overflow.
- `TotalSecondsF` returns a `f64` for sub-second precision
- `TotalSecondsF` first converts the 64-bit millisecond value to `f64`; sufficiently large values
  lose millisecond precision.
- Factories and arithmetic methods trap on signed 64-bit overflow
- `Abs(INT64_MIN)` and `Neg(INT64_MIN)` trap because the positive magnitude cannot be represented as a signed 64-bit value
- `Div(dur, divisor)` traps when `divisor` is 0 and when dividing `INT64_MIN` by `-1`

### Zia Example

```zia
module DurationDemo;

bind Zanna.Terminal;
bind Zanna.Time.Duration as Duration;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var d = Duration.FromHours(2);
    Say("Millis: " + Fmt.Int(d));                            // 7200000
    Say("TotalMinutes: " + Fmt.Int(Duration.TotalMinutes(d))); // 120
    Say("ToString: " + Duration.ToString(d));                // 02:00:00
    Say("ToISO: " + Duration.ToIso8601(d));                      // PT2H
}
```

### BASIC Example

```basic
' Create a duration of 2 hours
DIM d AS INTEGER = Zanna.Time.Duration.FromHours(2)

PRINT "Millis: "; d                                    ' Output: 7200000
PRINT "TotalMinutes: "; Zanna.Time.Duration.TotalMinutes(d) ' Output: 120
PRINT "ToString: "; Zanna.Time.Duration.ToString(d)    ' Output: 02:00:00
PRINT "ToISO: "; Zanna.Time.Duration.ToIso8601(d)          ' Output: PT2H

' Create from components
DIM d2 AS INTEGER = Zanna.Time.Duration.FromParts(1, 2, 30, 0, 0)
PRINT "Complex: "; Zanna.Time.Duration.ToString(d2)    ' Output: 1.02:30:00

' Arithmetic
DIM sum AS INTEGER = Zanna.Time.Duration.Add(d, d2)
DIM diff AS INTEGER = Zanna.Time.Duration.Sub(d2, d)
PRINT "Sum: "; Zanna.Time.Duration.ToString(sum)
PRINT "Diff: "; Zanna.Time.Duration.ToString(diff)

' Component extraction
PRINT "Hours: "; Zanna.Time.Duration.get_Hours(d2)         ' Output: 2
PRINT "Minutes: "; Zanna.Time.Duration.get_Minutes(d2)     ' Output: 30
```

---

## Zanna.Time.DateRange

A time range defined by start and end timestamps (in seconds). Useful for representing time intervals, scheduling windows, and date-based queries.

**Type:** Instance class (requires `New(startTimestamp, endTimestamp)`)

### Constructor

| Method              | Signature                   | Description                                               |
|---------------------|-----------------------------|-----------------------------------------------------------|
| `New(start, end)`   | `DateRange(Integer, Integer)` | Create a date range from start and end Unix timestamps  |

### Properties

| Property | Type                  | Description                                   |
|----------|-----------------------|-----------------------------------------------|
| `Start`  | `Integer` (read-only) | Start timestamp in seconds                    |
| `End`    | `Integer` (read-only) | End timestamp in seconds                      |

### Methods

| Method              | Signature                | Description                                                  |
|---------------------|--------------------------|--------------------------------------------------------------|
| `Contains(ts)`      | `Boolean(Integer)`       | Returns true if timestamp is within the range                |
| `Days()`            | `Integer()`              | Returns the number of whole days in the range                |
| `Hours()`           | `Integer()`              | Returns the number of whole hours in the range               |
| `Duration()`        | `Integer()`              | Returns the duration in seconds                              |
| `Overlaps(other)`   | `Boolean(DateRange)`     | Returns true if this range overlaps with another             |
| `Intersection(other)`| `DateRange or Nothing(DateRange)` | Return the overlap, or `Nothing` when disjoint        |
| `Union(other)`      | `DateRange or Nothing(DateRange)` | Return a contiguous union, or `Nothing` across a gap   |
| `ToString()`        | `String()`               | Returns a string representation of the range                 |

### Notes

- Timestamps are integer Unix timestamps in seconds (not milliseconds), and each range is a closed
  interval over those integer instants.
- The constructor normalizes reversed endpoints by swapping them, so stored ranges always satisfy `Start <= End`
- `Contains` checks if `Start <= ts <= End`
- `Intersection` returns `null` if the ranges do not overlap (does not trap)
- `Union` returns `null` if the ranges are neither overlapping nor contiguous; endpoints one second apart are considered contiguous
- `Days`, `Hours`, and `Duration` use `End - Start`; they do not add one for the inclusive endpoint.
  `Days` and `Hours` truncate the elapsed span to whole units. All three trap if the subtraction
  cannot be represented as a signed 64-bit integer.
- `ToString` formats both endpoints in UTC as `YYYY-MM-DD HH:MM - YYYY-MM-DD HH:MM`, without a zone suffix, and returns an empty string if either endpoint cannot be represented by the platform time APIs
- `ToString` omits seconds, so it is a display form rather than a lossless serialization.
- The live runtime metadata does not mark `Intersection` or `Union` nullable even though both
  return `Nothing` for ordinary inputs (VDOC-232); callers must test the result before chaining.

### Zia Example

```zia
module DateRangeDemo;

bind Zanna.Terminal;
bind Zanna.Time.DateRange as DateRange;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var r = DateRange.New(1000, 5000);
    Say("Start: " + Fmt.Int(r.get_Start()));        // 1000
    Say("End: " + Fmt.Int(r.get_End()));             // 5000
    Say("Contains 3000: " + Fmt.Bool(r.Contains(3000))); // true
    Say("Duration: " + Fmt.Int(r.Duration()));       // 4000
}
```

### BASIC Example

```basic
' Create a date range
DIM r AS OBJECT = Zanna.Time.DateRange.New(1000, 5000)

PRINT "Start: "; r.Start        ' Output: 1000
PRINT "End: "; r.End            ' Output: 5000
PRINT "Contains 3000: "; r.Contains(3000)  ' Output: 1
PRINT "Contains 6000: "; r.Contains(6000)  ' Output: 0
PRINT "Duration: "; r.Duration()           ' Output: 4000
PRINT "Days: "; r.Days()

' Check overlap with another range
DIM r2 AS OBJECT = Zanna.Time.DateRange.New(4000, 8000)
PRINT "Overlaps: "; r.Overlaps(r2)  ' Output: 1

' Get intersection
DIM inter AS OBJECT = r.Intersection(r2)
PRINT "Intersection start: "; inter.Start  ' Output: 4000
PRINT "Intersection end: "; inter.End      ' Output: 5000

' Get union
DIM u AS OBJECT = r.Union(r2)
PRINT "Union start: "; u.Start  ' Output: 1000
PRINT "Union end: "; u.End      ' Output: 8000

PRINT r.ToString()
```

---

## Zanna.Time.RelativeTime

Formats time durations and timestamps into human-readable relative descriptions.

**Type:** Static utility class

### Methods

| Method                  | Signature                   | Description                                                        |
|-------------------------|-----------------------------|--------------------------------------------------------------------|
| `Format(timestamp)`     | `String(Integer)`           | Format a timestamp relative to now (e.g., "2 hours ago", "in 3 days", "just now") |
| `FormatFrom(ts, base)`  | `String(Integer, Integer)`  | Format a timestamp relative to a given base timestamp              |
| `FormatDuration(ms)`    | `String(Integer)`           | Format a duration in milliseconds (e.g., "45s", "2h 30m", "1d 5h 20m") |
| `FormatShort(timestamp)`| `String(Integer)`           | Format a timestamp relative to now in short form (e.g., "2h ago", "in 3d", "5m ago") |

### Notes

- `Format` produces hard-coded English strings like "2 hours ago", "in 3 days", and "just now"; it is not locale-aware
- `FormatShort` produces compact single-unit strings with direction, like "2h ago", "in 3d", "5m ago", "now"
- `FormatDuration` produces compact multi-unit strings like "45s", "2h 30m", "1d 5h 20m"
- `Format` and `FormatShort` compare against the current system time
- Relative month and year buckets use fixed 30-day and 365-day lengths, and displayed units are truncated rather than rounded
- Differences below ten seconds render as `"just now"` (`Format`/`FormatFrom`) or `"now"`
  (`FormatShort`).
- `FormatDuration` discards the millisecond remainder and prefixes negative values with `-` only
  when the displayed whole-second magnitude is nonzero. A negative duration between `-999` and `-1`
  ms truncates to zero whole seconds and therefore renders as `"0s"`, symmetric with the positive
  sub-second case (VDOC-227).

### Zia Example

```zia
module RelativeTimeDemo;

bind Zanna.Terminal;
bind Zanna.Time.RelativeTime as RT;

func start() {
    Say(RT.FormatDuration(3600000));   // 1h
    Say(RT.FormatDuration(86400000));  // 1d
    Say(RT.FormatDuration(60000));     // 1m
}
```

### BASIC Example

```basic
' Format durations as human-readable strings
PRINT Zanna.Time.RelativeTime.FormatDuration(3600000)   ' Output: 1h
PRINT Zanna.Time.RelativeTime.FormatDuration(86400000)  ' Output: 1d
PRINT Zanna.Time.RelativeTime.FormatDuration(60000)     ' Output: 1m
PRINT Zanna.Time.RelativeTime.FormatDuration(5000)      ' Output: 5s

' Format relative to a base timestamp
DIM now AS INTEGER = Zanna.Time.DateTime.Now()
DIM past AS INTEGER = now - 3600  ' 1 hour ago
PRINT Zanna.Time.RelativeTime.FormatFrom(past, now)
```

---

## See Also

- [Threads](threads.md) - `Thread.Sleep()` and synchronization with timeouts
- [Graphics](graphics/README.md) - Frame timing with `Canvas` game loops
