---
status: active
audience: contributors
last-verified: 2026-07-29
---

# ADR 0225: Run Profile, Build-Launch Chain, Scene Watch, and Embedded Play

- Status: Accepted
- Date: 2026-07-29
- Deciders: Zanna Studio maintainers
- Tags: zannastudio, runtime, graphics, play-loop

## Context

The Run Scene loop dead-ended in practice: ashfall refuses interactive VM
runs (its native profile is the real product), neither example game
implemented a scene watcher, and Studio could not say whether a save
would reach a running game. The program's chosen flagship — playing the
game inside the editor — additionally needs a frame/input transport that
works without modifying game code.

## Decision

### `run-profile` manifest directive

`zanna.project` gains `run-profile vm|native` (default `vm`), parsed by
all three consumers: the CLI project loader (validated, duplicate-
rejected), the runtime manifest parser (`runProfile` map key), and
Studio's project model including its legacy line parser. ashfall-scenes
declares `native`.

### Build-then-launch chains

`RunConfig` gains a `next` continuation and
`ForNativeScene(root, name, scene, binary, env)`: stage 1 runs
`zanna build <root> --build-profile release -o <binary>`, stage 2
launches the binary with `--scene <path> --scene-watch` (env merged for
embed). `BuildSystem.Update()` hands a zero-exit stage to its
continuation in the same job slot, streaming both stages' output. Run
Scene branches on the project's run profile; a bounded newest-source
mtime walk (≤6 levels, ≤2,000 files, `.`-prefixed and run-artifact
directories skipped) skips the build stage when the binary is fresh, so
scene-only iterations launch in about a second.

### Scene watch in both games

Both example games accept `--scene-watch` beside `--scene` (the ADR 0194
contract): a 250 ms-bounded mtime poller whose reload fires only after a
full parse preflight (`SceneDocument.LoadResult` / `SceneGraph.Load`), so
a torn editor write can never demote the run to the fallback level.
xenoscape reloads the launched region in place; ashfall re-arms its
one-shot `setSceneOverride` before reloading the campaign level, and
`--scene` launches default to windowed. Studio's save path reports
"Saved: … — the running game will reload it" only when the saved
document is the watched scene.

### Embedded play

- **Transport** (`vgfx_embed_channel.{h,c}`): a named shared-memory
  channel — POSIX `shm_open`/`mmap`, Windows `CreateFileMapping` — holding
  a flat C11-atomic header, a two-slot seqlock frame ring (RGBA8,
  latest-wins, torn reads retried, neither side blocks), and a 256-record
  drop-oldest input ring. The host creates and unlinks; games attach via
  `ZANNA_EMBED_CHANNEL`.
- **Game side — zero game changes:** the shared vgfx layer taps every
  successful present (framebuffer → channel) and injects channel input
  into the ordinary event queue during event pumping, translating the
  bounded embed vocabulary (mouse move/button, wheel, key up/down, text,
  resize→`vgfx_set_window_size`, close). This deliberately replaces the
  planned per-platform adapter fork: one shared-layer tap keeps all three
  platforms on identical code, at the cost that the game's native window
  still exists on screen in v1 (hidden-start is a small per-platform flag
  recorded as follow-up).
- **Host side:** `Zanna.System.EmbedHost` (`Create`/`Attach` returning an
  `EmbedChannel` with frame acquire/publish, event push/poll, size
  request, and truthful attached/exited state). `AcquireFrameToImage`
  presents straight into the widget's retained buffer through the
  ADR 0224 borrow/commit seam — one copy per frame, no conversions.
- **Studio:** the 3D editor's Play toolbar item builds the native chain
  with the channel env injected (`Process.StartWithEnv`), presents frames
  into the viewport image, forwards focused pane input (pointer mapped to
  frame pixels; a bounded gameplay key set edge-forwarded), and reports
  waiting/live/exited truthfully; pressing Play again stops the session
  and the existing Stop command kills the child.
- **AOT linker:** `shm_open`/`shm_unlink` (and `nextafter`/`nextafterf`)
  joined the dynamic-symbol allowlists all platforms share.

### Verification

`embed_channel_probe` proves the transport headlessly in one process:
create/attach, latest-wins frames with exact bytes, FIFO input round
trip, capacity-clamped resize requests, exit truthfulness, and close
semantics. Game smokes, parity gates, and the studio label stay green.

## Deferred (recorded)

Hidden-start window flag per platform; pause/frame-step (cooperative
control message); GPU shared-surface frames (IOSurface/DXGI/dmabuf, a
compositing-phase rider); clipboard/IME forwarding; wheel and text
forwarding from the Studio pane (the channel carries them; the pane's
key set is bounded to gameplay keys in v1); a 2D-editor Play pane
(xenoscape plays through Run Scene's VM profile with `--scene-watch`
today — the same controller drops in once the 2D game view exposes an
equivalent pane image).

## Links

- `docs/adr/0224-bounded-viewport-presentation-budget.md`
- `src/lib/graphics/src/vgfx_embed_channel.c`
- `src/zannastudio/src/ui/scene_play_controller.zia`
- `src/zannastudio/src/probes/embed_channel_probe.zia`
