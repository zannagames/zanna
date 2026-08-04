# Zanna Intro Video — Demo Runbook (verified commands)

> Every command below was run against `zanna v0.2.7-snapshot` on macOS (arm64) and
> produces the output shown. Do a dry run of the **Games** section before the shoot
> (they open real windows). Record terminal segments in a dark theme, large font.

## 0. Setup (off camera)
```sh
# Confirm the binary you're filming is the real one (not a stale fixture)
which zanna            # -> /usr/local/bin/zanna
zanna --version        # -> zanna v0.2.7-snapshot
clear
```

## 1. "It's a real, friendly language" — REPL
```sh
zanna repl
```
```
zia> Say("Hello from Zanna")
Hello from Zanna
zia> Say(Fmt.Int(2 + 3))
5
```
Exit with Ctrl-D. Keep this short — it's just to establish approachability.

## 2. The program we'll trace
Show `misc/video/demo.zia` on screen (source is short on purpose):
```
func square(n: Integer) -> Integer { return n * n; }
func start() {
    Say("Zanna, compiled from scratch.");
    var total = 0;
    for i in 1..6 { total = total + square(i); }
    Say("Sum of squares 1..5 = " + Fmt.Int(total));
}
```

## 3. Run it on the VM
```sh
zanna run misc/video/demo.zia
```
```
Zanna, compiled from scratch.
Sum of squares 1..5 = 55
```

## 4. The magic trick — show the IL (the "peek behind the curtain")
First the **naive** lowering (-O0): source translated 1:1, memory traffic everywhere.
```sh
zanna build misc/video/demo.zia -O0 -o /tmp/demo_O0.il
```
On screen, scroll to `func @square` — point at the alloca/store/load/load/imul:
```llvm
func @square(i64 %t0) -> i64 {
entry_0(%t1:i64):
  %n$2 = alloca 8
  store i64, %n$2, %t1
  %t3 = load i64, %n$2
  %t4 = load i64, %n$2
  %t5 = imul.ovf %t3, %t4
  ret %t5
}
```

Then the **optimized** build (default pipeline, 24 passes):
```sh
zanna build misc/video/demo.zia -o /tmp/demo.il
```
`square` collapses to two instructions...
```llvm
func @square(i64 %t0) -> i64 {
entry_0(%t1:i64):
  %t5 = imul.ovf %t1, %t1
  ret %t5
}
```
...and in `@main` it's been **inlined into the loop** — search for the block named
`forin_body_1.inline.square.entry_0`. That label literally shows the inliner at work.
(`imul.ovf` = checked multiply; Zia is overflow-safe by default. Good aside.)

## 5. The payoff — a native binary, no LLVM
```sh
zanna build misc/video/demo.zia -o /tmp/demo_native
file /tmp/demo_native
/tmp/demo_native
```
```
/tmp/demo_native: Mach-O 64-bit executable arm64
Zanna, compiled from scratch.
Sum of squares 1..5 = 55
```
Punchline on camera: "No LLVM, no gcc, no clang, no system linker — Zanna's *own*
assembler and linker produced that Mach-O. And it printed the exact same thing the
VM did: same program, two engines, identical output."

## 6. The spectacle — real programs (DRY-RUN THESE FIRST)
All are runnable projects (`zanna.project` present). The showcase titles live
in the zannademos clone at `<zanna>/zannademos/`:
```sh
zanna run zannademos/games/ridgebound   # Ridgebound: terrain, water, skybox, PBR, post-FX
zanna run zannademos/games/xenoscape        # Metroid-style sidescroller
zanna run zannademos/games/3dbowling        # physics-driven 3D
zanna run examples/games/chess              # alpha-beta AI + drag-drop GUI
zanna run examples/apps/paint               # layers, undo/redo
zanna run zannademos/apps/zannasql          # SQL engine + client
```
Capture gameplay separately at high frame rate; cut to the best 4–6 seconds of each.

## 7. The scale reveal (B-roll / slides)
- Architecture diagram: `README.md` (Source → Frontend → IL → VM/Native → Runtime).
- Zero-dependency line + components table: `README.md`.
- Cross-platform badge: macOS · Linux · Windows.
- Optional: a fast scroll through the source tree to convey size.

## Recording checklist
- [ ] Dark terminal theme, font ≥ 20pt, window cropped tight.
- [ ] `which zanna` / `--version` confirmed before filming.
- [ ] Speed-ramp every build wait to ~2s in the edit.
- [ ] Games captured at 60fps separately from terminal segments.
