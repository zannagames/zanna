---
status: active
audience: public
last-verified: 2026-08-17
---

# Core Utilities
> Timer, StateMachine, SmoothValue, ObjectPool

**Part of [Zanna Runtime Library](../README.md) › [Game Utilities](README.md)**

---

## Zanna.Game.Timer

Countdown timer supporting both **frame-based** and **millisecond-based** modes. Frame mode
counts discrete frames (deterministic). Ms mode counts delta time (frame-rate independent).

**Type:** Instance class (requires `New()`)

### Constructor

| Method  | Signature  | Description                                 |
|---------|------------|---------------------------------------------|
| `New()` | `Timer()`  | Create a new timer (initially stopped)      |

### Properties

| Property      | Type                   | Description                                      |
|---------------|------------------------|--------------------------------------------------|
| `Duration`    | `Integer` (read/write) | Total timer units: frames after `Start`, ms after `StartMs` |
| `Elapsed`     | `Integer` (read-only)  | Elapsed timer units from the shared counter      |
| `Remaining`   | `Integer` (read-only)  | Remaining timer units from the shared counter    |
| `ElapsedMs`   | `Integer` (read-only)  | Elapsed counter, meaningful in ms mode (see `IsMs`) |
| `RemainingMs` | `Integer` (read-only)  | Remaining counter, meaningful in ms mode (see `IsMs`) |
| `Progress`    | `Integer` (read-only)  | Completion percentage 0-100 (both modes)         |
| `IsMs`        | `Boolean` (read-only)  | True if started in millisecond mode; false for frame mode (VDOC-264) |
| `IsRunning`   | `Boolean` (read-only)  | True if the timer is currently running           |
| `IsExpired`   | `Boolean` (read-only)  | True if the timer has finished                   |
| `IsRepeating` | `Boolean` (read-only)  | True if the timer auto-restarts when expired     |

### Frame-Based Methods

| Method                   | Signature       | Description                                               |
|--------------------------|-----------------|-----------------------------------------------------------|
| `Start(frames)`          | `Void(Integer)` | Start a one-shot countdown for specified frames           |
| `StartRepeating(frames)` | `Void(Integer)` | Start a repeating timer that auto-restarts                |
| `Update()`               | `Boolean()`     | Advance by one frame; returns true if just expired        |

### Millisecond-Based Methods

| Method                       | Signature          | Description                                            |
|------------------------------|--------------------|--------------------------------------------------------|
| `StartMs(durationMs)`        | `Void(Integer)`    | Start a one-shot countdown in milliseconds             |
| `StartRepeatingMs(intervalMs)` | `Void(Integer)`  | Start a repeating timer in milliseconds                |
| `UpdateMs(dt)`               | `Boolean(Integer)` | Advance by dt ms; returns true if just expired         |

### Common Methods

| Method   | Signature  | Description                                 |
|----------|------------|---------------------------------------------|
| `Stop()` | `Void()`   | Stop the timer                              |
| `Reset()`| `Void()`   | Reset elapsed to 0 without stopping         |

### Notes

- Frame mode: call `Update()` once per frame. Ms mode: call `UpdateMs(dt)` with delta time.
- The runtime enforces the mode: `Update()` is a no-op on a millisecond timer and
  `UpdateMs(dt)` is a no-op on a frame timer, so a mismatched call can no longer
  mix units and misfire a cooldown (VDOC-264). Read the `IsMs` property to inspect
  the active contract.
- `Update()` and `UpdateMs()` return true on an update that reaches an expiry.
  One-shot timers then stop. Repeating frame timers reset to zero; repeating ms
  timers preserve `dt` overshoot with modulo. If one `dt` spans several
  intervals, those expiries coalesce into one `true` result.
- Nonpositive start durations are ignored without changing existing state;
  nonpositive `UpdateMs` deltas are ignored. The `Duration` setter likewise
  ignores nonpositive values and does not start or reset the timer.
- `Stop()` preserves elapsed time and clears `IsExpired`; `Reset()` zeroes
  elapsed time but preserves running/repeating/mode state. `IsExpired` is true
  only for a completed one-shot, never a manually stopped or repeating timer.
- `Progress` works for both modes (0-100 based on elapsed/duration ratio).
- Ms mode is preferred for cooldowns, buffs, and effects that should be frame-rate independent.

### Zia Example

```zia
module TimerDemo;

bind Zanna.Terminal;
bind Zanna.Game.Timer as Timer;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var t = Timer.New();
    t.Start(60);

    var i = 0;
    while i < 5 {
        t.Update();
        i = i + 1;
    }
    Say("Elapsed: " + Fmt.Int(t.get_Elapsed()));
    Say("Remaining: " + Fmt.Int(t.get_Remaining()));
    Say("Progress: " + Fmt.Int(t.get_Progress()) + "%");

    // Repeating timer
    var r = Timer.New();
    r.StartRepeating(5);
    i = 0;
    while i < 12 {
        if r.Update() { Say("Expired at frame " + Fmt.Int(i)); }
        i = i + 1;
    }
}
```

### Example: Animation Timer

```basic
DIM canvas AS OBJECT = Zanna.Graphics.Canvas.New("Animation", 800, 600)

' Create a 60-frame animation timer (1 second at 60fps)
DIM anim AS OBJECT = Zanna.Game.Timer.New()
anim.Start(60)

DO WHILE NOT canvas.ShouldClose
    canvas.Poll()

    ' Update timer each frame
    IF anim.Update() THEN
        PRINT "Animation complete!"
    END IF

    ' Use progress for smooth interpolation
    DIM progress AS INTEGER = anim.Progress
    DIM x AS INTEGER = progress * 7  ' Move from 0 to 700

    canvas.Clear(0)
    canvas.Box(x, 280, 40, 40, 16711680)
    canvas.Flip()
LOOP
```

### Example: Repeating Timer (Game Events)

```basic
' Create a timer that fires every 180 frames (3 seconds at 60fps)
DIM spawnTimer AS OBJECT = Zanna.Game.Timer.New()
spawnTimer.StartRepeating(180)

' Game loop
DO WHILE running
    canvas.Poll()

    ' Check if it's time to spawn an enemy
    IF spawnTimer.Update() THEN
        SpawnEnemy()  ' Called every 3 seconds
    END IF

    ' Rest of game logic...
LOOP
```

### Example: Power-Up Duration

```basic
DIM powerUpTimer AS OBJECT = Zanna.Game.Timer.New()

' When player collects power-up
SUB OnPowerUpCollected()
    powerUpTimer.Start(600)  ' 10 seconds at 60fps
    playerSpeed = playerSpeed * 2
END SUB

' In game loop
IF powerUpTimer.IsRunning THEN
    ' Show remaining time
    IF powerUpTimer.Remaining < 120 THEN
        FlashPowerUpIndicator()  ' Warning: almost over
    END IF

    IF powerUpTimer.Update() THEN
        playerSpeed = playerSpeed / 2  ' Power-up expired
    END IF
END IF
```

### Comparison with Zanna.Time.Countdown

| Feature             | Timer                          | Countdown                       |
|---------------------|--------------------------------|---------------------------------|
| Time unit           | Frames or caller-supplied ms    | Host-clock milliseconds         |
| Determinism         | Frame mode is deterministic; ms mode follows supplied `dt` | Subject to host clock/fallback behavior |
| Use case            | Game logic, animations         | Real-world timeouts             |
| Update model        | Manual `Update()` per frame    | Automatic based on clock        |
| Progress tracking   | 0-100 integer percentage       | Elapsed/remaining in ms         |
| Repeating mode      | Yes (`StartRepeating` / `StartRepeatingMs`) | No (must manually restart) |

### Use Cases

- **Animations:** Smooth interpolation with `Progress` property
- **Cooldowns:** Weapon fire rate, ability cooldowns
- **Spawn timers:** Periodic enemy spawning
- **Power-ups:** Duration-limited effects
- **Mode transitions:** Ghost AI state changes
- **Delays:** Wait N frames before action

---

## Zanna.Game.StateMachine

A finite state machine for managing game/application states like menus, gameplay, pause screens, and transitions.

**Type:** Instance class (requires `New()`)

### Constructor

| Method  | Signature        | Description                           |
|---------|------------------|---------------------------------------|
| `New()` | `StateMachine()` | Create a new empty state machine      |

### Properties

| Property       | Type                  | Description                                     |
|----------------|-----------------------|-------------------------------------------------|
| `Current`      | `Integer` (read-only) | Current state ID (-1 if none set)               |
| `Previous`     | `Integer` (read-only) | Previous state ID (-1 if none)                  |
| `JustEntered`  | `Boolean` (read-only) | True after an initial state/transition until `ClearFlags` |
| `JustExited`   | `Boolean` (read-only) | True after leaving a previous state until `ClearFlags` |
| `FramesInState`| `Integer` (read-only) | Frames spent in current state                   |
| `StateCount`   | `Integer` (read-only) | Number of registered states                     |

### Methods

| Method           | Signature          | Description                                    |
|------------------|--------------------|------------------------------------------------|
| `AddState(id)`   | `Boolean(Integer)` | Register a state ID; returns false if exists   |
| `ClearFlags()`   | `Void()`           | Clear JustEntered/JustExited flags             |
| `HasState(id)`   | `Boolean(Integer)` | Check if state ID is registered                |
| `IsState(id)`    | `Boolean(Integer)` | Check if currently in specified state          |
| `SetInitial(id)` | `Boolean(Integer)` | Select a registered state, reset previous/counter/flags |
| `Transition(id)` | `Boolean(Integer)` | Transition to a registered state               |
| `Update()`       | `Void()`           | Increment frame counter (call once per frame)  |

### Notes

- State IDs are integers in `[0, 255]`; use constants for readability.
  `AddState` traps on an out-of-range ID, while `SetInitial`, `Transition`, and
  `HasState` return false for one.
- Edge flags do not clear automatically in `Update`; they remain true until
  `ClearFlags()` is called. `SetInitial` sets only `JustEntered`. A transition
  from an initialized state sets both flags.
- `SetInitial` is not restricted to initial setup: a later call replaces the
  current state, resets `Previous` to `-1`, and sets the frame counter to zero.
  Transitioning to the already-current state returns true but changes no flags
  or counters.
- Call `Update()` once per frame to track `FramesInState`; it saturates at the maximum
  integer value instead of overflowing
- Call `ClearFlags()` at end of frame if checking flags multiple times

### Zia Example

```zia
module StateMachineDemo;

bind Zanna.Terminal;
bind Zanna.Game.StateMachine as SM;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var sm = SM.New();
    sm.AddState(0);  // MENU
    sm.AddState(1);  // PLAYING
    sm.AddState(2);  // PAUSED
    sm.SetInitial(0);

    Say("State: " + Fmt.Int(sm.get_Current()));
    sm.Transition(1);
    Say("After transition: " + Fmt.Int(sm.get_Current()));
    Say("Previous: " + Fmt.Int(sm.get_Previous()));

    sm.Update();
    sm.Update();
    Say("Frames in state: " + Fmt.Int(sm.get_FramesInState()));
    Say("IsState(1): " + Fmt.Bool(sm.IsState(1)));
}
```

### Example: Game States

```basic
' Define state constants
CONST STATE_MENU = 0
CONST STATE_PLAYING = 1
CONST STATE_PAUSED = 2
CONST STATE_GAMEOVER = 3

' Create and configure state machine
DIM sm AS OBJECT = Zanna.Game.StateMachine.New()
sm.AddState(STATE_MENU)
sm.AddState(STATE_PLAYING)
sm.AddState(STATE_PAUSED)
sm.AddState(STATE_GAMEOVER)
sm.SetInitial(STATE_MENU)

' Game loop
DO WHILE running
    canvas.Poll()

    ' Handle state-specific logic
    SELECT CASE sm.Current
    CASE STATE_MENU
        IF sm.JustEntered THEN
            PRINT "Welcome! Press ENTER to start"
        END IF
        IF Zanna.Input.Keyboard.WasPressed(Zanna.Input.Key.Enter) THEN
            sm.Transition(STATE_PLAYING)
        END IF

    CASE STATE_PLAYING
        IF sm.JustEntered THEN
            InitGame()
        END IF
        UpdateGame()
        IF Zanna.Input.Keyboard.WasPressed(Zanna.Input.Key.Escape) THEN
            sm.Transition(STATE_PAUSED)
        END IF

    CASE STATE_PAUSED
        DrawPauseOverlay()
        IF Zanna.Input.Keyboard.WasPressed(Zanna.Input.Key.Escape) THEN
            sm.Transition(STATE_PLAYING)
        END IF

    CASE STATE_GAMEOVER
        IF sm.FramesInState > 180 THEN  ' 3 seconds at 60fps
            sm.Transition(STATE_MENU)
        END IF
    END SELECT

    sm.Update()
    sm.ClearFlags()
    canvas.Flip()
LOOP
```

---

## Zanna.Game.SmoothValue

Smooth value interpolation for camera follow, UI animations, and other cases where instant changes would be jarring. Uses exponential smoothing.

**Type:** Instance class (requires `New(initial, smoothing)`)

### Constructor

| Method                    | Signature              | Description                                    |
|---------------------------|------------------------|------------------------------------------------|
| `New(initial, smoothing)` | `SmoothValue(Dbl,Dbl)` | Create with initial value and smoothing factor |

### Properties

| Property    | Type                   | Description                                      |
|-------------|------------------------|--------------------------------------------------|
| `Value`     | `Double` (read-only)   | Current smoothed value                           |
| `ValueI64`  | `Integer` (read-only)  | Current value as rounded integer                 |
| `Target`    | `Double` (read/write)  | Target value to approach                         |
| `Smoothing` | `Double` (read/write)  | Per-update retention factor clamped to 0.0–0.999 |
| `AtTarget`  | `Boolean` (read-only)  | True when absolute error is below 0.001          |
| `Velocity`  | `Double` (read-only)   | Change produced by the most recent `Update`      |

### Methods

| Method              | Signature      | Description                              |
|---------------------|----------------|------------------------------------------|
| `Impulse(amount)`   | `Void(Double)` | Add instant displacement to current value|
| `SetImmediate(val)` | `Void(Double)` | Set value and target immediately         |
| `Update()`          | `Void()`       | Advance interpolation (call each frame)  |

### Notes

- Each update computes `current = current * smoothing + target * (1 - smoothing)`.
  This is update-count dependent, not delta-time compensated.
- Smoothing clamps to `[0.0, 0.999]`; a non-finite factor becomes `0.0`.
  `0.0` reaches the target on the next update, while higher values converge
  more slowly.
- Initial non-finite values and non-finite `SetImmediate` values become zero.
  A non-finite Target assignment or Impulse is ignored.
- `Impulse` offsets only the current value, leaving Target and the last
  reported Velocity unchanged. The next update eases back toward Target.
- `ValueI64` rounds halves away from zero and saturates to the signed 64-bit
  range. Update snaps to Target and zeroes Velocity once the error is below
  `0.001`.

### Zia Example

```zia
module SmoothValueDemo;

bind Zanna.Terminal;
bind Zanna.Game.SmoothValue as SV;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var sv = SV.New(0.0, 0.9);
    sv.set_Target(100.0);

    var i = 0;
    while i < 10 {
        sv.Update();
        Say("Frame " + Fmt.Int(i) + ": " + Fmt.Int(sv.get_ValueI64()));
        i = i + 1;
    }

    sv.SetImmediate(50.0);
    Say("After SetImmediate: " + Fmt.Int(sv.get_ValueI64()));
}
```

### Example: Camera Follow

```basic
' Create smooth camera position
DIM camX AS OBJECT = Zanna.Game.SmoothValue.New(400.0, 0.9)
DIM camY AS OBJECT = Zanna.Game.SmoothValue.New(300.0, 0.9)

' In game loop
camX.Target = playerX  ' Camera follows player
camY.Target = playerY
camX.Update()
camY.Update()

' Use smoothed values for drawing offset
DIM offsetX AS INTEGER = camX.ValueI64 - 400
DIM offsetY AS INTEGER = camY.ValueI64 - 300
```

---

## Zanna.Game.ObjectPool

Efficient object pool for reusing slot indices, avoiding allocation churn for frequently created/destroyed game objects like bullets, enemies, and particles.

**Type:** Instance class (requires `New(capacity)`)

### Constructor

| Method           | Signature          | Description                              |
|------------------|--------------------|------------------------------------------|
| `New(capacity)`  | `ObjectPool(Int)`  | Create a fixed pool; capacity clamps to 1–4096 |

### Properties

| Property      | Type                  | Description                              |
|---------------|-----------------------|------------------------------------------|
| `Capacity`    | `Integer` (read-only) | Maximum number of slots                  |
| `ActiveCount` | `Integer` (read-only) | Number of currently acquired slots       |
| `FreeCount`   | `Integer` (read-only) | Number of available slots                |
| `IsFull`      | `Boolean` (read-only) | True if all slots are in use             |
| `IsEmpty`     | `Boolean` (read-only) | True if no slots are in use              |

### Methods

| Method               | Signature           | Description                                    |
|----------------------|---------------------|------------------------------------------------|
| `Acquire()`          | `Integer()`         | Get a free slot index (-1 if full)             |
| `Clear()`            | `Void()`            | Release all slots                              |
| `FirstActive()`      | `Integer()`         | Get first active slot (-1 if none)             |
| `GetData(slot)`      | `Integer(Integer)`  | Get user data for active slot, or 0 if inactive |
| `IsActive(slot)`     | `Boolean(Integer)`  | Check if slot is currently acquired            |
| `NextActive(after)`  | `Integer(Integer)`  | Get next active slot after index               |
| `Release(slot)`      | `Boolean(Integer)`  | Return slot to pool; false if invalid          |
| `SetData(slot,data)` | `Boolean(Int,Int)`  | Associate user data with slot                  |

Releasing or clearing a slot clears its user data. `GetData` returns `0` for inactive,
released, or invalid slots.

`Acquire` takes the head of a free list, zeroes its data, and prepends it to the
active list. `FirstActive`/`NextActive` therefore visit newest acquisitions
first, not ascending indexes. Acquisition and iteration steps are O(1), but
releasing a non-head active slot scans for its predecessor and is O(active
count). A released slot is normally the next one reused.

When removing during iteration, read `NextActive(slot)` **before** calling
`Release(slot)`: release clears that slot's next-active link.

### Zia Example

```zia
module ObjectPoolDemo;

bind Zanna.Terminal;
bind Zanna.Game.ObjectPool as Pool;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var pool = Pool.New(10);
    Say("Capacity: " + Fmt.Int(pool.get_Capacity()));

    var s1 = pool.Acquire();
    var s2 = pool.Acquire();
    var s3 = pool.Acquire();
    Say("Active: " + Fmt.Int(pool.get_ActiveCount()));

    pool.SetData(s1, 100);
    Say("Data s1: " + Fmt.Int(pool.GetData(s1)));

    pool.Release(s2);
    Say("After release: " + Fmt.Int(pool.get_ActiveCount()));

    // Iterate active slots
    var slot = pool.FirstActive();
    while slot >= 0 {
        Say("Active: " + Fmt.Int(slot));
        slot = pool.NextActive(slot);
    }

    pool.Clear();
}
```

### Example: Bullet Pool

```basic
' Create bullet pool
DIM bullets AS Zanna.Game.ObjectPool = Zanna.Game.ObjectPool.New(100)
DIM bulletX(99) AS DOUBLE
DIM bulletY(99) AS DOUBLE
DIM bulletVX(99) AS DOUBLE
DIM bulletVY(99) AS DOUBLE

' Fire one bullet into an acquired slot
DIM fired AS INTEGER = bullets.Acquire()
IF fired >= 0 THEN
    bulletX(fired) = 790
    bulletY(fired) = 200
    bulletVX(fired) = 16
    bulletVY(fired) = 0
END IF

' Update all active bullets. Capture the link before a possible release.
DIM slot AS INTEGER = bullets.FirstActive()
DO WHILE slot >= 0
    DIM nextSlot AS INTEGER = bullets.NextActive(slot)
    bulletX(slot) = bulletX(slot) + bulletVX(slot)
    bulletY(slot) = bulletY(slot) + bulletVY(slot)

    IF bulletX(slot) < 0 OR bulletX(slot) > 800 THEN
        bullets.Release(slot)
    END IF

    slot = nextSlot
LOOP

PRINT "Active bullets: "; bullets.ActiveCount
```

---


## See Also

- [Physics & Collision](physics.md)
- [Animation & Movement](animation.md)
- [Visual Effects](effects.md)
- [Game Utilities Overview](README.md)
- [Zanna Runtime Library](../README.md)
