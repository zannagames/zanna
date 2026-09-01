---
status: active
audience: public
last-verified: 2026-07-26
---

# Chapter 21: Building a Game

There is something magical about building a game. When you write a calculator or a file browser, it works and that is satisfying. But when you build a game, something different happens. You see a character move across the screen because you told it to. You watch a player narrowly avoid danger and feel your heart race, even though you built the danger yourself. You create a small world that responds to input, that challenges and rewards, that tells a story without words.

This chapter is where everything comes together. We will build a complete game from scratch: Frogger, the classic arcade game where you guide a frog across a busy road and a dangerous river. This is not a toy example or a simplified demo. By the end of this chapter, you will have a playable game with multiple lives, scoring, progressive difficulty, and polish. More importantly, you will understand how games are structured and why they work the way they do.

Every skill you have learned converges here. Variables store game state. Functions organize behavior. Collections manage groups of enemies. Decisions determine what happens when objects collide. Loops drive the game forward frame by frame. Values represent entities in your world. This is the payoff for all those chapters of learning fundamentals. You are about to see how those fundamentals combine into something people actually want to play.

---

## Why Frogger?

Frogger is a brilliant learning project. Released by Konami in 1981, it became an arcade sensation because its rules are immediately understandable yet create genuine challenge and tension.

Here is what the player sees: a frog at the bottom of the screen, five empty spaces at the top, and a gauntlet in between. The lower half is a road filled with cars and trucks zooming past. The upper half is a river with logs and turtles floating by. Get hit by a car, you die. Fall in the water, you die. Hop on a log and ride it safely across. Reach one of the home spaces at the top, score points. Fill all five homes, advance to a faster level.

This design teaches us several things about what makes good games:

**Simple rules, emergent challenge.** The player has exactly four actions: hop up, down, left, right. The game has exactly two hazards: things that kill you on contact (cars) and empty space that kills you if you step on it (water). Yet these simple elements create complex situations. You might hop onto a log only to realize it is carrying you toward the edge of the screen. You might wait for the perfect gap in traffic, then panic when you realize a truck is barreling toward you from the other direction.

**Visible state.** Everything the player needs to know is on screen. You can see exactly where every car is, how fast the logs are moving, which homes are filled. There is no hidden information, no surprise deaths from something you could not see. This makes the game feel fair even when it is brutally hard.

**Meaningful progression.** Each completed frog represents tangible progress. Each cleared level brings faster traffic and more challenge. Players can feel themselves improving as patterns become recognizable and reflexes sharpen.

**Immediate feedback.** Every action has an instant, visible result. Hop and the frog moves. Get hit and you see the splat. The connection between input and outcome is never ambiguous.

These principles apply to all games, from simple arcade classics to sprawling modern adventures. Learning them now, through building something small and complete, prepares you to think like a game designer.

---

## Game Architecture: Thinking Before Coding

Before writing a single line of code, let us think about how games are structured. Beginning programmers often make the mistake of diving straight into code, adding features as they think of them. This leads to tangled, fragile programs that become harder to change as they grow. Professional game developers think about architecture first.

### The Core Loop

Every game has a heartbeat, a loop that runs continuously from the moment the game starts until it ends:

```text
while game is running:
    1. Handle input (read what the player is doing)
    2. Update state (move objects, check collisions, apply rules)
    3. Render (draw everything to the screen)
    4. Wait (control how fast the loop runs)
```

This is called the game loop, and it is the backbone of interactive software. Think of it like a flipbook animation. Each page shows a slightly different picture. Flip through fast enough and the static images appear to move. Your game loop draws one "page" per cycle, typically 60 times per second. Between draws, you update positions so the next frame shows things in new locations.

The separation of concerns here is crucial:

- **Input handling** reads the keyboard, mouse, or controller state but does not change the game world. It just records what the player wants to do.
- **Update** changes the game state based on time, input, and rules. This is where objects move, collisions are detected, and scores change.
- **Render** reads the current state and draws it to the screen. It never changes anything, just translates data into pictures.

This separation makes code easier to reason about. When something draws wrong, you know the bug is in rendering. When objects move incorrectly, you look at update logic. When controls feel wrong, you examine input handling.

### Entities: The Things in Your World

Games are full of things: players, enemies, obstacles, collectibles, projectiles, platforms. We call these entities. An class is anything that exists in your game world and needs to be tracked.

In Zia, we represent entities using values and sometimes class types. Think of a value as a filled-out form describing something:

```zia
struct Frog {
    x: Number;           // Position across the screen
    y: Number;           // Position up and down
    alive: Boolean;      // Is the frog currently alive?
    moveCooldown: Number; // Time until next move is allowed
}
```

This is like a census form for a frog. Every frog has these properties, and we can look them up whenever we need to know where a frog is or whether it is alive.

**Why separate data from behavior?** In Zia, we often keep values simple and put behavior in functions that operate on them. This makes data easy to serialize (save to files), easy to inspect for debugging, and easy to pass around. The frog does not know how to move itself; there is a `move` function that takes a frog and returns a new frog with an updated position.

### State Management: Knowing What Is True

A game's state is everything about the game world at a given moment: where every object is, how many lives remain, what level you are on, whether the game is paused. If you saved the state to a file and loaded it later, the game would resume exactly where it left off.

Good state management means:

1. **All state is explicit.** Nothing important lives in global variables scattered across files. There is a GameState value that contains everything.
2. **State changes are predictable.** Given the same state and the same inputs, the game always produces the same next state.
3. **State is separated from display.** The game state does not know about pixels or colors. It knows about positions, scores, and rules.

This is why we will create a `GameState` value that holds everything: the frog, all vehicles, all platforms, filled homes, score, lives, and level. When we need to know anything about the game, we look in one place.

### Collision Detection: When Things Touch

Games become interesting when objects interact. Collision detection answers the question: are these two objects touching?

For rectangular objects (which is what we will use), collision detection is beautifully simple. Two rectangles overlap if and only if they overlap on both axes. On the X axis, rectangle A overlaps rectangle B if A's left edge is before B's right edge AND A's right edge is after B's left edge. Same logic for the Y axis. If both are true, the rectangles collide.

```text
Rectangle A overlaps Rectangle B when:
  A.left < B.right AND A.right > B.left AND
  A.top < B.bottom AND A.bottom > B.top
```

Think of it like comparing two ranges of time. Two meetings overlap if the first starts before the second ends AND the first ends after the second starts. Same idea in two dimensions.

For our frog, we will simplify further: we treat the frog as a single point and check whether that point is inside any car or on any platform. This is less accurate but simpler to implement and works well when objects are large relative to the frog.

---

## Mental Models for Game Development

Before we build, let us develop some mental pictures that will help you think about game code.

### The World as a Spreadsheet

Imagine a spreadsheet where each row is an class and each column is a property. One row for the frog with columns for x, y, alive, cooldown. Many rows for vehicles, each with x, y, width, speed, color. Every frame, you read this spreadsheet, calculate new values, and write an updated spreadsheet. Rendering is looking at the spreadsheet and drawing what it describes.

This is why state and rendering are separate. The spreadsheet is the truth. The screen is just a visualization.

### Time as Slices

Games do not simulate continuous time. They simulate discrete moments, typically 60 per second. Between moments, the game world is frozen. Your code runs, updates state, renders, and the world jumps to a new configuration. Like a movie, games are an illusion of motion created by showing many still images rapidly.

This is why we use delta time. If your game runs slower on an old computer (say, 30 frames per second instead of 60), objects should still move the same distance per real-world second. You calculate how much time passed since the last frame and multiply speeds by that value. This way, a car moving at 100 pixels per second actually moves 100 pixels each real second, regardless of frame rate.

### Events vs. States

Some things in games are states: the frog is alive, the player is pressing right, the game is paused. States are continuously true or false. Other things are events: the player just pressed space, the frog just died, the level just completed. Events happen at a specific moment and then are over.

Treating these differently prevents bugs. If you check `Keyboard.IsDown(Keyboard.KeySpace)` for jumping, the frog will jump every frame the spacebar is held, bouncing constantly. You want `Keyboard.WasPressed(Keyboard.KeySpace)`, which is true only on the frame the key transitioned from up to down. That is an event check, not a state check.

### The Coordinate Dance

In Frogger, we work with two coordinate systems:

**Grid coordinates** divide the screen into tiles. The frog moves one tile at a time. Row 0 is the top (home row), row 14 is the bottom (starting row). This makes position checking simple: if the frog is in rows 8-13, it is on the road. If it is in rows 1-6, it is on the river.

**Pixel coordinates** are exact screen positions for drawing and precise collision detection. A tile might be 40x40 pixels, so grid position (5, 10) corresponds to pixel position (200, 400).

Converting between them keeps code clean. Game logic thinks in tiles (is the frog on the road?). Rendering thinks in pixels (draw the frog at this exact position).

---

## Project Structure

We will organize the code into modules, each responsible for one aspect of the game:

```text
frogger/
├── main.zia        # Entry point and game loop
├── config.zia      # Constants and settings
├── game.zia        # Game state and logic
├── frog.zia        # Player character
├── lane.zia        # Roads and rivers
├── vehicle.zia     # Cars and trucks
├── platform.zia    # Logs and turtles
├── renderer.zia    # Drawing code
└── input.zia       # Input handling
```

Why this structure? Each file has a single responsibility:

- **config.zia** holds all magic numbers in one place. Want to make the screen bigger? Change it there. Want to adjust scoring? One file.
- **frog.zia** knows everything about how a frog works but nothing about cars, logs, or scoring.
- **vehicle.zia** knows how cars and trucks move but nothing about frogs or platforms.
- **game.zia** orchestrates everything, knowing about all class types and how they interact.
- **renderer.zia** only draws. It never changes game state.

This separation means you can modify one aspect without breaking others. Adding a new vehicle type only touches vehicle.zia and game.zia, not the frog code or renderer.

---

## Step 1: Configuration

Let us start with the foundation. Configuration values are constants that control every aspect of the game. By collecting them in one place, we make the game easy to tune and customize.

```zia
// config.zia
module Config;

// Screen dimensions
expose final SCREEN_WIDTH = 800;
expose final SCREEN_HEIGHT = 600;

// Grid layout
// We divide the screen into tiles for easier positioning
expose final TILE_SIZE = 40;
expose final GRID_WIDTH = 20;   // 800 / 40
expose final GRID_HEIGHT = 15;  // 600 / 40

// Game zones (row numbers from top)
// Row 0 is the home zone where frogs must reach
expose final HOME_ROW = 0;
// Rows 1-6 are the river section with logs and turtles
expose final RIVER_START = 1;
expose final RIVER_END = 6;
// Row 7 is a safe zone in the middle
expose final SAFE_ZONE = 7;
// Rows 8-13 are the road section with cars and trucks
expose final ROAD_START = 8;
expose final ROAD_END = 13;
// Row 14 is where the frog starts
expose final START_ROW = 14;

// Timing
// Prevents the frog from moving too fast when keys are held
expose final FROG_MOVE_COOLDOWN = 0.15;  // seconds between hops

// Scoring
expose final SCORE_PER_STEP = 10;     // Points for each hop forward
expose final SCORE_PER_HOME = 200;    // Points for reaching a home
expose final SCORE_PER_LEVEL = 1000;  // Bonus for completing a level

// Lives
expose final STARTING_LIVES = 3;
```

Notice how every value has a comment explaining what it does. When you come back to this code in a month, you will thank yourself. Also notice the organization: screen setup, then zones, then timing, then scoring. Related values are grouped together.

The `expose` keyword makes these constants accessible from other modules. The `final` keyword means they cannot be changed after being set. Together, they create public constants that any part of the game can reference.

---

## Step 2: The Frog (Player Entity)

The frog is the heart of the game. It is what the player controls, what they identify with. Let us build it carefully.

```zia
// frog.zia
module Frog;

bind Config;

// The Frog value represents everything we need to know about the player
expose struct Frog {
    // Pixel coordinates for smooth movement and drawing
    x: Number;
    y: Number;

    // Grid coordinates for collision zones and logical position
    gridX: Integer;
    gridY: Integer;

    // If the frog is on a log or turtle, it moves with that platform
    // null means the frog is not riding anything
    ridingPlatform: Platform?;

    // Is the frog currently alive?
    alive: Boolean;

    // Time until the frog can move again (prevents too-fast hopping)
    moveCooldown: Number;
}
```

Why do we track both pixel and grid positions? The grid position determines which zone the frog is in (road, river, home) and makes movement calculations simple (move up = decrease gridY by 1). The pixel position is needed for smooth drawing and for precise collision detection with vehicles that move continuously rather than tile-by-tile.

Now let us add the functions that operate on frogs:

```zia
// Create a brand new frog at the starting position
expose func create() -> Frog {
    return spawn();
}

// Reset the frog to starting position (used after death or scoring)
expose func spawn() -> Frog {
    return Frog {
        x: Config.SCREEN_WIDTH / 2.0,
        y: Config.START_ROW * Config.TILE_SIZE,
        gridX: Config.GRID_WIDTH / 2,
        gridY: Config.START_ROW,
        ridingPlatform: null,
        alive: true,
        moveCooldown: 0.0
    };
}
```

The `spawn` function creates a frog at the starting position. Notice that `create` just calls `spawn`. Why have both? Semantic clarity. When the game first starts, we `create` a frog. When the frog dies and respawns, we `spawn` a new one. Same result, but the code reads more naturally.

```zia
// Update the frog each frame
expose func update(frog: Frog, dt: Number) -> Frog {
    var f = frog;

    // Decrease the movement cooldown
    if f.moveCooldown > 0 {
        f.moveCooldown -= dt;
    }

    // If riding a platform, move with it
    if f.ridingPlatform != null {
        f.x += f.ridingPlatform.speed * dt;

        // Did we drift off the edge of the screen?
        if f.x < 0 || f.x > Config.SCREEN_WIDTH {
            f.alive = false;
        }
    }

    return f;
}
```

This update function demonstrates a key pattern: we take the current state (`frog`), make a copy (`var f = frog`), modify the copy, and return it. This functional approach makes state changes explicit. The original frog is never modified; we always produce a new frog with updated values.

The platform-riding logic is interesting. When a frog hops onto a log, the log is stored in `ridingPlatform`. Every frame, if the frog is riding something, its x position changes by the platform's speed multiplied by delta time. This creates the effect of the frog being carried along.

But what if the log carries the frog off the screen? That is why we check the boundaries. A frog that drifts off-screen dies. This creates a classic Frogger tension: you hop onto a log for safety, but now you must keep an eye on where it is taking you.

```zia
// Check if the frog can move (cooldown expired)
expose func canMove(frog: Frog) -> Boolean {
    return frog.moveCooldown <= 0;
}

// Attempt to move the frog in a direction
expose func move(frog: Frog, dx: Integer, dy: Integer) -> Frog {
    var f = frog;

    var newX = f.gridX + dx;
    var newY = f.gridY + dy;

    // Bounds checking - cannot move off the grid
    if newX < 0 || newX >= Config.GRID_WIDTH {
        return f;  // Return unchanged if move is invalid
    }
    if newY < 0 || newY >= Config.GRID_HEIGHT {
        return f;
    }

    // Valid move - update both grid and pixel positions
    f.gridX = newX;
    f.gridY = newY;
    f.x = newX * Config.TILE_SIZE + Config.TILE_SIZE / 2.0;
    f.y = newY * Config.TILE_SIZE;

    // Reset cooldown so player cannot move again immediately
    f.moveCooldown = Config.FROG_MOVE_COOLDOWN;

    // Moving clears the riding platform (the frog jumps off)
    f.ridingPlatform = null;

    return f;
}

// Kill the frog
expose func die(frog: Frog) -> Frog {
    var f = frog;
    f.alive = false;
    return f;
}
```

The `move` function validates input before changing anything. If the player tries to move off the edge of the screen, we simply return the frog unchanged. No error, no crash, the frog just stays put. This defensive approach prevents bugs where invalid game states could occur.

Also notice that moving sets `ridingPlatform` to null. When you hop, you leave whatever platform you were on. If you hop off a log onto water (not onto another log), you will drown on the next collision check. If you hop to another log, the collision check will set a new riding platform.

---

## Step 3: Vehicles (Road Hazards)

Vehicles are the dangers on the road. They move continuously across the screen, wrapping around when they exit, creating an endless stream of traffic.

```zia
// vehicle.zia
module Vehicle;

bind Config;

expose struct Vehicle {
    x: Number;             // Horizontal position (pixels)
    y: Number;             // Vertical position (pixels)
    width: Number;         // How wide the vehicle is
    speed: Number;         // Pixels per second (negative = left, positive = right)
    color: Color;       // Color for drawing
}

expose func create(row: Integer, speed: Number, width: Number, color: Color) -> Vehicle {
    // Start position depends on direction
    // Vehicles moving right start off the left edge
    // Vehicles moving left start off the right edge
    var startX = 0.0;
    if speed < 0 {
        startX = Config.SCREEN_WIDTH;
    }

    return Vehicle {
        x: startX,
        y: row * Config.TILE_SIZE,
        width: width,
        speed: speed,
        color: color
    };
}
```

The starting position logic is clever: if a vehicle moves right (positive speed), it starts just off the left edge so it appears to drive onto the screen. If it moves left (negative speed), it starts at the right edge.

```zia
expose func update(vehicle: Vehicle, dt: Number) -> Vehicle {
    var v = vehicle;
    v.x += v.speed * dt;

    // Wrap around when the vehicle completely exits the screen
    if v.speed > 0 && v.x > Config.SCREEN_WIDTH + v.width {
        v.x = -v.width;
    }
    if v.speed < 0 && v.x < -v.width {
        v.x = Config.SCREEN_WIDTH;
    }

    return v;
}
```

The wrapping logic is essential for creating the feeling of endless traffic. When a vehicle moves off the right edge (x > screen width + vehicle width), it teleports to the left edge. When it moves off the left edge (x < negative vehicle width), it teleports to the right. This creates a seamless loop of vehicles.

Why `+ v.width` in the comparison? Because `x` represents the left edge of the vehicle. A vehicle at x = 800 on an 800-pixel wide screen still has its body visible. It only fully exits when its left edge is past the screen width by at least its own width.

```zia
expose func getBounds(vehicle: Vehicle) -> Rect {
    return Rect {
        x: vehicle.x,
        y: vehicle.y,
        width: vehicle.width,
        height: Config.TILE_SIZE - 4  // Slightly smaller than tile for visual fit
    };
}

expose func hitsPoint(v: Vehicle, px: Number, py: Number) -> Boolean {
    var bounds = getBounds(v);
    return px >= bounds.x && px < bounds.x + bounds.width &&
           py >= bounds.y && py < bounds.y + bounds.height;
}
```

The `hitsPoint` function checks if a point (the frog's position) is inside the vehicle's rectangle. This is point-in-rectangle collision detection: is the point's x between left and right edges? Is the point's y between top and bottom edges? If both are true, collision.

---

## Step 4: Platforms (River Objects)

Platforms are logs and turtles that float on the river. Unlike vehicles that kill on contact, platforms save the frog from drowning. The code is similar to vehicles with one crucial behavioral difference.

```zia
// platform.zia
module Platform;

bind Config;

expose struct Platform {
    x: Number;
    y: Number;
    width: Number;
    speed: Number;
    color: Color;
}

expose func create(row: Integer, speed: Number, width: Number, color: Color) -> Platform {
    var startX = 0.0;
    if speed < 0 {
        startX = Config.SCREEN_WIDTH;
    }

    return Platform {
        x: startX,
        y: row * Config.TILE_SIZE,
        width: width,
        speed: speed,
        color: color
    };
}

expose func update(platform: Platform, dt: Number) -> Platform {
    var p = platform;
    p.x += p.speed * dt;

    // Wrap around
    if p.speed > 0 && p.x > Config.SCREEN_WIDTH + p.width {
        p.x = -p.width;
    }
    if p.speed < 0 && p.x < -p.width {
        p.x = Config.SCREEN_WIDTH;
    }

    return p;
}

expose func containsPoint(p: Platform, px: Number, py: Number) -> Boolean {
    return px >= p.x && px < p.x + p.width &&
           py >= p.y && py < p.y + Config.TILE_SIZE;
}
```

The Platform code looks nearly identical to Vehicle. This might seem like unnecessary duplication, but it actually serves clarity. Vehicles and platforms have different semantics even if their data is similar. Keeping them separate makes code easier to read and allows them to diverge later if needed.

---

## Step 5: Game State

Now we bring everything together. The GameState value is the single source of truth for everything happening in the game.

```zia
// game.zia
module Game;

bind Config;
bind Frog;
bind Vehicle;
bind Platform;

expose struct GameState {
    frog: Frog.Frog;                    // The player
    vehicles: List[Vehicle.Vehicle];        // All cars and trucks
    platforms: List[Platform.Platform];     // All logs and turtles
    homesOccupied: List[Boolean];              // Which home slots are filled
    score: Integer;                         // Current score
    lives: Integer;                         // Remaining lives
    level: Integer;                         // Current level (affects speed)
    gameOver: Boolean;                     // Is the game over?
    won: Boolean;                          // Did the player win? (for future use)
}
```

Notice how GameState owns everything. The frog, all vehicles, all platforms. When you have the game state, you have the complete picture of the game world.

```zia
expose func create() -> GameState {
    var state = GameState {
        frog: Frog.create(),
        vehicles: [],
        platforms: [],
        homesOccupied: [false, false, false, false, false],
        score: 0,
        lives: Config.STARTING_LIVES,
        level: 1,
        gameOver: false,
        won: false
    };

    return setupLevel(state);
}
```

The `create` function initializes a fresh game, then calls `setupLevel` to populate the roads and rivers. This separation means the same `setupLevel` function can be used both when starting and when advancing to a new level.

```zia
func setupLevel(state: GameState) -> GameState {
    var s = state;

    // Speed increases with level
    // Level 1 = 1.0x speed, Level 2 = 1.2x speed, etc.
    var speedMod = 1.0 + (s.level - 1) * 0.2;

    // Clear and create vehicles for the road lanes
    s.vehicles = [];

    // Each row has different vehicles moving different directions at different speeds
    // This creates variety and challenge

    // Row 8: slow cars going right (easiest to dodge)
    s.vehicles.Push(Vehicle.create(8, 60 * speedMod, 60, Color.Red));
    s.vehicles.Push(Vehicle.create(8, 60 * speedMod, 60, Color.Red));

    // Row 9: faster cars going left
    s.vehicles.Push(Vehicle.create(9, -80 * speedMod, 50, Color.Blue));
    s.vehicles.Push(Vehicle.create(9, -80 * speedMod, 50, Color.Blue));

    // Row 10: slow trucks going right (wide, hard to squeeze past)
    s.vehicles.Push(Vehicle.create(10, 50 * speedMod, 120, Color.Yellow));

    // Row 11: fast cars going left (dangerous!)
    s.vehicles.Push(Vehicle.create(11, -120 * speedMod, 40, Color.Green));
    s.vehicles.Push(Vehicle.create(11, -120 * speedMod, 40, Color.Green));
    s.vehicles.Push(Vehicle.create(11, -120 * speedMod, 40, Color.Green));

    // Row 12: medium cars going right
    s.vehicles.Push(Vehicle.create(12, 70 * speedMod, 55, Color.Magenta));
    s.vehicles.Push(Vehicle.create(12, 70 * speedMod, 55, Color.Magenta));

    // Row 13: slow trucks going left (closest to starting area)
    s.vehicles.Push(Vehicle.create(13, -40 * speedMod, 100, Color.Cyan));
    s.vehicles.Push(Vehicle.create(13, -40 * speedMod, 100, Color.Cyan));

    // Spread vehicles out so they do not all start at the same place
    for i in 0..s.vehicles.Length {
        s.vehicles[i].x = (i * 170) % Config.SCREEN_WIDTH;
    }

    // Create platforms for the river lanes
    s.platforms = [];

    // Row 1-6: alternating logs (brown) and turtles (green)
    // Logs are long and safe, turtles are short and may submerge (future feature)
    s.platforms.Push(Platform.create(1, 50 * speedMod, 120, Color.Rgb(139, 69, 19)));
    s.platforms.Push(Platform.create(1, 50 * speedMod, 120, Color.Rgb(139, 69, 19)));
    s.platforms.Push(Platform.create(2, -40 * speedMod, 80, Color.Rgb(0, 100, 0)));
    s.platforms.Push(Platform.create(2, -40 * speedMod, 80, Color.Rgb(0, 100, 0)));
    s.platforms.Push(Platform.create(2, -40 * speedMod, 80, Color.Rgb(0, 100, 0)));
    s.platforms.Push(Platform.create(3, 60 * speedMod, 160, Color.Rgb(139, 69, 19)));
    s.platforms.Push(Platform.create(4, -70 * speedMod, 100, Color.Rgb(0, 100, 0)));
    s.platforms.Push(Platform.create(4, -70 * speedMod, 100, Color.Rgb(0, 100, 0)));
    s.platforms.Push(Platform.create(5, 45 * speedMod, 140, Color.Rgb(139, 69, 19)));
    s.platforms.Push(Platform.create(5, 45 * speedMod, 140, Color.Rgb(139, 69, 19)));
    s.platforms.Push(Platform.create(6, -55 * speedMod, 90, Color.Rgb(0, 100, 0)));
    s.platforms.Push(Platform.create(6, -55 * speedMod, 90, Color.Rgb(0, 100, 0)));

    // Spread platforms out
    for i in 0..s.platforms.Length {
        s.platforms[i].x = (i * 130) % Config.SCREEN_WIDTH;
    }

    return s;
}
```

The level setup demonstrates thoughtful design. Each lane has a different speed and direction, creating variety. Alternating directions means the player cannot just wait for gaps; they must actively watch both ways. The spreading logic ensures vehicles and platforms start at different positions rather than clumping together.

The `speedMod` multiplier creates progressive difficulty. Level 1 runs at 1.0x speed. Level 2 at 1.2x. Level 3 at 1.4x. The same level design becomes harder simply by making everything move faster.

### The Update Function

The heart of game logic happens in update:

```zia
expose func update(state: GameState, dt: Number) -> GameState {
    var s = state;

    // Nothing happens if the game is over
    if s.gameOver {
        return s;
    }

    // Update the frog (handles cooldown and platform riding)
    s.frog = Frog.update(s.frog, dt);

    // Update all vehicles (movement and wrapping)
    for i in 0..s.vehicles.Length {
        s.vehicles[i] = Vehicle.update(s.vehicles[i], dt);
    }

    // Update all platforms (movement and wrapping)
    for i in 0..s.platforms.Length {
        s.platforms[i] = Platform.update(s.platforms[i], dt);
    }

    // Check for collisions (death by car or drowning)
    s = checkCollisions(s);

    // Check if frog reached a home slot
    s = checkHome(s);

    // Handle frog death
    if !s.frog.alive {
        s.lives -= 1;
        if s.lives <= 0 {
            s.gameOver = true;
        } else {
            s.frog = Frog.spawn();  // Respawn at start
        }
    }

    // Check if all homes are filled (level complete)
    var allHome = true;
    for occupied in s.homesOccupied {
        if !occupied {
            allHome = false;
            break;
        }
    }

    if allHome {
        s.score += Config.SCORE_PER_LEVEL;
        s.level += 1;
        s.homesOccupied = [false, false, false, false, false];
        s.frog = Frog.spawn();
        s = setupLevel(s);
    }

    return s;
}
```

Notice the clear sequence: update entities, check interactions, handle consequences. Each step builds on the previous. We update positions first so collision checks use current positions. We check collisions before processing death so we have accurate death reasons. We handle level completion last because it resets so much state.

### Collision Detection

The collision system separates road and river logic:

```zia
func checkCollisions(state: GameState) -> GameState {
    var s = state;
    var frogX = s.frog.x;
    var frogY = s.frog.y;
    var gridY = s.frog.gridY;

    // Is the frog on the road?
    if gridY >= Config.ROAD_START && gridY <= Config.ROAD_END {
        // Check each vehicle for collision
        for vehicle in s.vehicles {
            if Vehicle.hitsPoint(vehicle, frogX, frogY) {
                s.frog = Frog.die(s.frog);
                return s;  // No need to check more, frog is dead
            }
        }
    }

    // Is the frog on the river?
    if gridY >= Config.RIVER_START && gridY <= Config.RIVER_END {
        var onPlatform = false;

        // Check each platform
        for platform in s.platforms {
            if Platform.containsPoint(platform, frogX, frogY) {
                s.frog.ridingPlatform = platform;
                onPlatform = true;
                break;  // Only ride one platform at a time
            }
        }

        // Not on any platform? Splash!
        if !onPlatform {
            s.frog = Frog.die(s.frog);
        }
    }

    return s;
}
```

The road and river have opposite logic:
- On the road, touching anything (a vehicle) is death
- On the river, NOT touching anything (a platform) is death

This inversion creates different play experiences. On the road, you dodge threats. On the river, you seek safety. Same character, same controls, but the player's mindset shifts completely.

### Home Detection

Getting home is the whole point:

```zia
func checkHome(state: GameState) -> GameState {
    var s = state;

    if s.frog.gridY == Config.HOME_ROW {
        // Determine which home slot the frog is at
        var homeIndex = getHomeIndex(s.frog.gridX);

        if homeIndex >= 0 && homeIndex < 5 {
            if !s.homesOccupied[homeIndex] {
                // Success! Fill this home and score points
                s.homesOccupied[homeIndex] = true;
                s.score += Config.SCORE_PER_HOME;
                s.frog = Frog.spawn();
            } else {
                // This home is already occupied, death by confusion!
                s.frog = Frog.die(s.frog);
            }
        } else {
            // Landed between home slots (on the barrier), death
            s.frog = Frog.die(s.frog);
        }
    }

    return s;
}

func getHomeIndex(gridX: Integer) -> Integer {
    // Five home slots spread across the top row
    var positions = [2, 6, 10, 14, 18];
    for i in 0..5 {
        // Each home spans two grid cells
        if gridX == positions[i] || gridX == positions[i] + 1 {
            return i;
        }
    }
    return -1;  // Not at a valid home position
}
```

The home slots are not the entire top row. There are barriers between them. Landing on a barrier kills the frog, adding precision to the challenge. Landing in an already-occupied home also kills, preventing the trivially easy strategy of going to the same spot repeatedly.

### Player Movement Interface

The game module also handles the interface between input and frog movement:

```zia
expose func moveFrog(state: GameState, dx: Integer, dy: Integer) -> GameState {
    var s = state;

    if Frog.canMove(s.frog) {
        var oldY = s.frog.gridY;
        s.frog = Frog.move(s.frog, dx, dy);

        // Score points for moving forward (toward home)
        if s.frog.gridY < oldY {
            s.score += Config.SCORE_PER_STEP;
        }
    }

    return s;
}
```

This wrapper adds scoring for forward progress. Moving backward does not lose points (you might be dodging a car), but only forward movement advances your score. This subtle mechanic encourages aggressive play.

---

## Step 6: Rendering

Rendering transforms game state into pictures. It reads but never modifies state.

```zia
// renderer.zia
module Renderer;

bind Config;
bind Game;
bind Zanna.Graphics;

expose func render(canvas: Canvas, state: Game.GameState) {
    // Clear the canvas
    canvas.Clear(Color.Black);

    // Draw the world zones (background)
    drawBackground(canvas);

    // Draw the home slots
    drawHomes(canvas, state.homesOccupied);

    // Draw all platforms (logs and turtles)
    for platform in state.platforms {
        canvas.Box(platform.x, platform.y, platform.width, Config.TILE_SIZE - 4, platform.color);
    }

    // Draw all vehicles
    for vehicle in state.vehicles {
        canvas.Box(vehicle.x, vehicle.y, vehicle.width, Config.TILE_SIZE - 4, vehicle.color);
    }

    // Draw the frog (only if alive)
    if state.frog.alive {
        canvas.Box(state.frog.x - 15, state.frog.y, 30, 35, Color.Rgb(0, 255, 0));
    }

    // Draw the user interface (score, lives, level)
    drawUI(canvas, state);
}
```

Notice the drawing order. Background first, then platforms, then vehicles, then frog, then UI. This layering ensures the frog appears on top of platforms (correct, since it is riding them) and the UI appears over everything.

```zia
func drawBackground(canvas: Canvas) {
    // Home zone at the top
    canvas.Box(0, 0, Config.SCREEN_WIDTH, Config.TILE_SIZE, Color.Rgb(50, 50, 100));

    // River (dangerous water)
    canvas.Box(0, Config.RIVER_START * Config.TILE_SIZE,
               Config.SCREEN_WIDTH,
               (Config.RIVER_END - Config.RIVER_START + 1) * Config.TILE_SIZE,
               Color.Rgb(0, 0, 150));

    // Safe zone in the middle (rest area)
    canvas.Box(0, Config.SAFE_ZONE * Config.TILE_SIZE,
               Config.SCREEN_WIDTH, Config.TILE_SIZE,
               Color.Rgb(100, 50, 150));

    // Road
    canvas.Box(0, Config.ROAD_START * Config.TILE_SIZE,
               Config.SCREEN_WIDTH,
               (Config.ROAD_END - Config.ROAD_START + 1) * Config.TILE_SIZE,
               Color.Rgb(50, 50, 50));

    // Draw lane divider lines on the road
    for row in Config.ROAD_START..Config.ROAD_END {
        var y = row * Config.TILE_SIZE + Config.TILE_SIZE / 2;
        // Dashed lines
        for x in 0..(Config.SCREEN_WIDTH / 50) {
            canvas.Box(x * 50, y - 2, 30, 4, Color.Yellow);
        }
    }

    // Starting area at the bottom
    canvas.Box(0, Config.START_ROW * Config.TILE_SIZE,
               Config.SCREEN_WIDTH, Config.TILE_SIZE,
               Color.Rgb(100, 50, 150));
}
```

The background creates the visual context. Blue water tells players "danger, stay on platforms." Gray road with yellow lines reads instantly as "traffic area." The safe zones (purple) provide visual breathing room.

```zia
func drawHomes(canvas: Canvas, occupied: List[Boolean]) {
    var positions = [2, 6, 10, 14, 18];

    for i in 0..5 {
        var x = positions[i] * Config.TILE_SIZE;

        // Green if filled, darker green if empty
        var homeColor = Color.Rgb(0, 100, 0);  // Dark green = target
        if occupied[i] {
            homeColor = Color.Rgb(0, 255, 0);  // Bright green = success
        }

        canvas.Box(x, 2, Config.TILE_SIZE * 2, Config.TILE_SIZE - 4, homeColor);
    }
}
```

Filled homes glow bright green, celebrating the player's progress. Empty homes are darker, beckoning targets. This visual feedback is crucial for quickly understanding game state.

```zia
func drawUI(canvas: Canvas, state: Game.GameState) {
    // Display current stats at the bottom
    canvas.Text(10, Config.SCREEN_HEIGHT - 10, "Score: " + state.score, Color.White);
    canvas.Text(200, Config.SCREEN_HEIGHT - 10, "Lives: " + state.lives, Color.White);
    canvas.Text(350, Config.SCREEN_HEIGHT - 10, "Level: " + state.level, Color.White);

    // Game over screen
    if state.gameOver {
        canvas.Text(300, 300, "GAME OVER", Color.Red);
        canvas.Text(280, 350, "Press ENTER to restart", Color.White);
    }
}
```

The UI provides at-a-glance status. Score, lives, and level are always visible. When the game ends, a large "GAME OVER" message appears with restart instructions.

---

## Step 7: Main Game Loop

The main file brings everything together:

```zia
// main.zia
module Main;

bind Config;
bind Game;
bind Renderer;
bind Zanna.Graphics;
bind Keyboard = Zanna.Input.Keyboard;
bind Zanna.Time as Time;

func start() {
    // Create the game window
    var canvas = Canvas.New("Frogger", Config.SCREEN_WIDTH, Config.SCREEN_HEIGHT);

    // Initialize game state
    var state = Game.create();

    // Track time for delta calculations
    var lastTime = Time.Clock.NowMs();

    // The game loop
    while !canvas.ShouldClose {
        canvas.Poll();

        // Calculate how much time passed since last frame
        var now = Time.Clock.NowMs();
        var dt = (now - lastTime) / 1000.0;  // Convert to seconds
        lastTime = now;

        // === INPUT PHASE ===
        if state.gameOver {
            // Only accept restart input when game is over
            if Keyboard.WasPressed(Keyboard.KeyEnter) {
                state = Game.create();  // Fresh game
            }
        } else {
            // Normal gameplay input
            if Keyboard.WasPressed(Keyboard.KeyUp) || Keyboard.WasPressed(Keyboard.KeyW) {
                state = Game.moveFrog(state, 0, -1);
            }
            if Keyboard.WasPressed(Keyboard.KeyDown) || Keyboard.WasPressed(Keyboard.KeyS) {
                state = Game.moveFrog(state, 0, 1);
            }
            if Keyboard.WasPressed(Keyboard.KeyLeft) || Keyboard.WasPressed(Keyboard.KeyA) {
                state = Game.moveFrog(state, -1, 0);
            }
            if Keyboard.WasPressed(Keyboard.KeyRight) || Keyboard.WasPressed(Keyboard.KeyD) {
                state = Game.moveFrog(state, 1, 0);
            }
        }

        // Escape always quits
        if Keyboard.WasPressed(Keyboard.KeyEscape) {
            break;
        }

        // === UPDATE PHASE ===
        state = Game.update(state, dt);

        // === RENDER PHASE ===
        Renderer.render(canvas, state);
        canvas.Flip();

        // === TIMING ===
        // Sleep to target approximately 60 FPS
        Time.Clock.Sleep(16);  // 16ms ≈ 60 frames per second
    }
}
```

This is the complete game loop in action:

1. **Calculate delta time** to ensure consistent speed
2. **Handle input** differently based on game state
3. **Update** the game world
4. **Render** the new state
5. **Sleep** to maintain frame rate

Notice the use of `Keyboard.WasPressed` rather than `Keyboard.IsDown` for movement. Frogger is a grid-based game where the frog moves one tile per button press. Using `IsDown` would make the frog zip across the screen while a key is held; `WasPressed` requires discrete presses.

Also notice the support for both arrow keys and WASD. Many players prefer WASD, especially for left-handed play or if they are used to modern games. Supporting both costs almost nothing and improves accessibility.

---

## What We Built

This complete game demonstrates professional game development concepts:

**Modular architecture.** Each file handles one responsibility. You can understand the frog without knowing about vehicles, or study rendering without understanding collision detection.

**Clear data structures.** Values represent entities with explicit fields. No magic, no hidden state. You can print any value and understand what it means.

**The game loop pattern.** Input, update, render, repeat. This pattern works for any interactive application, from games to simulations to creative tools.

**Collision detection.** Point-in-rectangle checks determine interactions. Simple but effective for tile-based games.

**State management.** A single GameState value holds everything. Changes are explicit and traceable.

**Progressive difficulty.** Speed scaling makes later levels harder without requiring new content.

**Player feedback.** Score display, visual home markers, game over screen. Players always know what is happening.

**Clean separation.** Logic does not know about pixels. Rendering does not change state. Input does not directly modify the frog.

---

## Common Mistakes and How to Avoid Them

Game development has unique pitfalls. Here are mistakes beginners commonly make and how to prevent them.

### Mistake 1: Frame-Rate Dependent Speed

**Wrong:**
```zia
// This moves different distances on fast vs slow computers
player.x += 5;
```

**Right:**
```zia
// This moves the same distance per real second
player.x += speed * dt;
```

Without delta time, your game runs twice as fast on a 120 FPS machine as on a 60 FPS machine. Always multiply speeds by dt (time since last frame).

### Mistake 2: Using Key State for One-Time Actions

**Wrong:**
```zia
// This fires every frame the key is held!
if Keyboard.IsDown(Keyboard.KeySpace) {
    fireBullet();
}
```

**Right:**
```zia
// This fires once per key press
if Keyboard.WasPressed(Keyboard.KeySpace) {
    fireBullet();
}
```

Use `Keyboard.WasPressed` for discrete actions (jump, fire, menu select) and `Keyboard.IsDown` for continuous actions (moving, aiming).

### Mistake 3: Order-Dependent Collision Bugs

**Wrong:**
```zia
// Moving then checking creates "tunneling" through thin walls
player.x += velocity * dt;
player.y += velocity * dt;
checkCollisions();
```

If velocity is high enough, the player can jump over obstacles between frames. For Frogger this is less critical because movement is grid-based, but in physics-heavy games, you need to either limit speed, use swept collision detection, or subdivide movement into smaller steps.

### Mistake 4: Modifying State During Iteration

**Wrong:**
```zia
for enemy in enemies {
    if enemy.dead {
        enemies.remove(enemy);  // Modifying while iterating!
    }
}
```

**Right:**
```zia
var survivors: List[Enemy] = [];
for enemy in enemies {
    if !enemy.dead {
        survivors.Push(enemy);
    }
}
enemies = survivors;
```

Or:
```zia
var toRemove: List[Integer] = [];
for i in 0..enemies.Length {
    if enemies[i].dead {
        toRemove.Push(i);
    }
}
// Remove in reverse order to preserve indices
var index = toRemove.Length - 1;
while index >= 0 {
    enemies.RemoveAt(toRemove[index]);
    index -= 1;
}
```

Never modify a collection while iterating over it. Either filter to create a new collection, or gather indices and remove afterward.

### Mistake 5: Forgetting Edge Cases

The frog can die in many ways:
- Hit by a car
- Falling in water
- Riding a log off the screen
- Landing on a filled home
- Landing on the barrier between homes

Each requires explicit handling. When adding new features, ask: "What are all the ways this can go wrong?"

### Mistake 6: Not Accounting for Boundaries

**Wrong:**
```zia
player.x += dx;  // What if this goes negative or off-screen?
```

**Right:**
```zia
var newX = player.x + dx;
if newX >= 0 && newX < screenWidth {
    player.x = newX;
}
```

Always validate that new positions are valid before applying them.

---

## Debugging Tips for Games

Game bugs are often harder to find than bugs in regular programs because the state changes 60 times per second. Here are strategies that help.

### Slow Down Time

Add a debug mode that runs at 1/10 speed:
```zia
var debugSlowMotion = false;

if Keyboard.WasPressed(Keyboard.KeyF1) {
    debugSlowMotion = !debugSlowMotion;
}

if debugSlowMotion {
    dt = dt * 0.1;
}
```

Now you can see exactly what happens during collisions or fast movements.

### Visualize Collision Boxes

Draw rectangles showing where collision detection thinks objects are:
```zia
func debugDrawBounds(canvas: Canvas, vehicles: List[Vehicle]) {
    for v in vehicles {
        var bounds = Vehicle.getBounds(v);
        canvas.Frame(bounds.x, bounds.y, bounds.width, bounds.height, Color.Rgb(255, 0, 0));
    }
}
```

If your frog dies when it looks like it should not, the collision boxes reveal whether positions are off.

### Log State Changes

When something unexpected happens, print the state before and after:
```zia
if frog.alive && newFrog.alive == false {
    print("Frog died! Position: " + frog.x + ", " + frog.y);
    print("Grid position: " + frog.gridX + ", " + frog.gridY);
    print("On road: " + (frog.gridY >= Config.ROAD_START));
    print("On river: " + (frog.gridY >= Config.RIVER_START));
}
```

### Pause and Step

Add the ability to pause the game and advance one frame at a time:
```zia
var paused = false;
var stepOneFrame = false;

if Keyboard.WasPressed(Keyboard.KeyP) {
    paused = !paused;
}

if Keyboard.WasPressed(Keyboard.KeyN) {  // Next frame
    stepOneFrame = true;
}

if !paused || stepOneFrame {
    state = Game.update(state, dt);
    stepOneFrame = false;
}
```

This lets you examine exactly what happens each frame.

### Check Your Assumptions

When a bug appears, question everything:
- Is dt actually being calculated correctly?
- Are coordinates in pixels or grid units?
- Is this checking greater-than or greater-than-or-equal?
- Are array indices zero-based or one-based?

Many game bugs come from off-by-one errors or unit mismatches.

---

## Extending the Game

A completed Frogger is a foundation, not an endpoint. Here are ways to make it your own.

### Add Sound

Sound brings games to life. Add a hop sound when the frog moves, a splash when it drowns, a splat when hit by a car, a cheerful jingle when reaching home:

```zia
bind Zanna.Audio;

var hopSound = Sound.Load("hop.wav");
var splatSound = Sound.Load("splat.wav");
var splashSound = Sound.Load("splash.wav");

// In movement code:
if moved {
    hopSound.Play();
}

// In death handling:
if deathByVehicle {
    splatSound.Play();
} else {
    splashSound.Play();
}
```

### Animated Sprites

Replace colored rectangles with proper artwork. Load sprite images and draw them instead of `Box`:

```zia
bind Sprite = Zanna.Graphics.Sprite;

var frogSprite = Sprite.FromFile("frog.png");
var frogLeft = Sprite.FromFile("frog_left.png");
var frogRight = Sprite.FromFile("frog_right.png");

// Track facing direction
struct Frog {
    // ... existing fields ...
    facing: Direction;
}

// In rendering
var sprite = frogSprite;
if frog.facing == Direction.LEFT { sprite = frogLeft; }
if frog.facing == Direction.RIGHT { sprite = frogRight; }
canvas.Blit(sprite, frog.x - 20, frog.y);
```

### High Score System

Save the highest score to a file so players can compete with themselves:

```zia
bind Convert = Zanna.Core.Convert;
bind Zanna.Text.Fmt as Fmt;

func loadHighScore() -> Integer {
    if !File.Exists("highscore.txt") {
        return 0;
    }
    return Convert.ToInt64(File.ReadAllText("highscore.txt"));
}

func saveHighScore(score: Integer) {
    File.WriteAllText("highscore.txt", Fmt.Int(score));
}

// At game over:
if state.score > highScore {
    highScore = state.score;
    saveHighScore(highScore);
}
```

### Time Limit

Add urgency with a countdown timer:

```zia
struct GameState {
    // ... existing fields ...
    timeRemaining: Number;
}

// In create:
timeRemaining: 60.0;  // 60 seconds per frog

// In update:
s.timeRemaining -= dt;
if s.timeRemaining <= 0 {
    s.frog = Frog.die(s.frog);
    s.timeRemaining = 60.0;
}

// In rendering:
canvas.Text(500, Config.SCREEN_HEIGHT - 10,
            "Time: " + Convert.NumToInt(Math.Floor(state.timeRemaining)), Color.White);
```

### Submerging Turtles

In the original Frogger, turtles periodically submerge, creating temporary danger:

```zia
struct Platform {
    // ... existing fields ...
    isTurtle: Boolean;
    submergeTimer: Number;
    submerged: Boolean;
}

// In update:
if p.isTurtle {
    p.submergeTimer -= dt;
    if p.submergeTimer <= 0 {
        p.submerged = !p.submerged;
        p.submergeTimer = 2.0;  // Toggle every 2 seconds
    }
}

// In collision check:
if Platform.containsPoint(platform, frogX, frogY) {
    if platform.submerged {
        // Turtle is underwater, frog drowns
        s.frog = Frog.die(s.frog);
    } else {
        // Normal platform behavior
        s.frog.ridingPlatform = platform;
        onPlatform = true;
    }
}
```

### Two-Player Mode

Add a second frog controlled by WASD, competing for score:

```zia
struct GameState {
    frog1: Frog;  // Arrow keys
    frog2: Frog;  // WASD keys
    score1: Integer;
    score2: Integer;
    // ...
}
```

Each frog fills different homes and earns its own score. First to fill all five wins, or highest score after a time limit.

### Power-Ups

Add collectible items that appear occasionally:

```text
struct PowerUp {
    x: Number;
    y: Number;
    kind: PowerUpKind;  // EXTRA_LIFE, SLOW_MOTION, INVINCIBILITY
    timer: Number;         // How long until it disappears
}

// Spawn power-ups occasionally
if randomChance(0.01) && state.powerUp == null {
    state.powerUp = PowerUp.create();
}

// Collect on touch
if powerUp.containsPoint(frog.x, frog.y) {
    applyPowerUp(powerUp.kind);
    state.powerUp = null;
}
```

---

## Exercises

These exercises progressively build on the Frogger project, from small modifications to significant new features.

**Exercise 21.1 - Sound Effects**: Add sound effects for hopping, dying, and scoring. Use the Zanna.Audio module to load and play WAV files. Make the death sound different for cars versus drowning.

**Exercise 21.2 - High Score**: Implement a high score system that persists between game sessions. Display the high score on the title screen and the game over screen. Show a special message when the player beats their previous best.

**Exercise 21.3 - Animated Sprites**: Replace the solid rectangles with sprite images. Create or find simple pixel art for the frog (ideally with different facing directions), cars, trucks, logs, and turtles. The frog should face the direction it last moved.

**Exercise 21.4 - Time Limit**: Add a time limit for each frog. Display the remaining time prominently. Award bonus points for reaching home quickly. The timer resets when the frog respawns.

**Exercise 21.5 - Pause Menu**: Add a pause feature (press P) that freezes the game and displays a simple menu with options to resume or quit. While paused, no updates should occur but the game should still render.

**Exercise 21.6 - Submerging Turtles**: Make some platforms turtles that periodically submerge. When submerged, they cannot be stood on. Add visual feedback (color change or transparency) when a turtle is about to submerge.

**Exercise 21.7 - Moving Gators**: Add alligator enemies that swim in the river lanes. They look like logs but if the frog stands on their head, it gets eaten. The player must hop onto the body, not the head.

**Exercise 21.8 - Multiple Lanes**: Make the road and river larger with more lanes. Adjust the difficulty curve so early levels use fewer lanes and later levels use all of them.

**Exercise 21.9 - Different Levels**: Instead of just increasing speed, create distinct level designs. Level 1 might have wide logs and slow cars. Level 5 might have narrow turtles and racing traffic. Define level data in configuration.

**Exercise 21.10 - Two-Player Mode**: Add a second frog controlled by WASD. Both frogs play simultaneously, competing for score. Handle what happens when both try to fill the same home. Declare a winner when all homes are filled.

**Exercise 21.11 - Title Screen**: Create a title screen that displays before the game starts, showing the game name, controls, and high score. Require the player to press Enter to begin.

**Exercise 21.12 (Challenge) - Level Editor**: Create a simple level editor that lets players place vehicles and platforms, set their speeds, and save/load level configurations. This teaches file I/O, UI design, and tool creation.

**Exercise 21.13 (Challenge) - Enemy AI**: Add a second mode where you control a car trying to hit player-controlled frogs. The frogs are AI-controlled, trying to reach home. This inverts the game and requires you to think about simple AI pathfinding.

**Exercise 21.14 (Challenge) - Network Multiplayer**: Using concepts from Chapter 22, make a two-player mode where players are on different computers. One hosts the game server, the other connects as a client. Handle synchronization and latency.

---

## Summary

You built a complete game. Not a demo, not a prototype, but a playable arcade game with multiple lives, scoring, progressive difficulty, and clear feedback.

Along the way, you learned:

- **Game architecture**: The loop of input, update, render that drives all interactive software
- **Entity design**: Representing game objects as values with clear properties
- **State management**: Keeping all game information in one place with explicit changes
- **Collision detection**: Determining when objects interact using geometric tests
- **Coordinate systems**: Working with both grid (logical) and pixel (visual) positions
- **Progressive difficulty**: Making games harder without creating new content
- **Code organization**: Separating concerns into focused modules
- **Debugging strategies**: Techniques specific to the challenges of game development
- **Extension possibilities**: How to build on a foundation to create something unique

These skills transfer far beyond Frogger. The game loop pattern appears in simulations, creative tools, and any real-time interactive software. Entity design applies to any domain with objects that have properties and behaviors. State management is essential for any complex application. Understanding these concepts makes you a better programmer regardless of what you build next.

---

*You built a game. You took abstract concepts and made them respond to your fingers, challenge your reflexes, and reward your progress. That is no small thing. The same skills that created this Frogger can create any game you can imagine, from simple puzzles to sprawling adventures.*

*Next, we explore networking, where programs communicate across the internet. Imagine if two Frogger players could compete from different computers. Networking makes that possible.*

*[Continue to Chapter 22: Networking](22-networking.md)*
