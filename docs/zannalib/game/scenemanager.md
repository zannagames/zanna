---
status: active
audience: public
last-verified: 2026-09-13
---

# Zanna.Game.SceneManager

`SceneManager` is a bounded name/state timer. It does not own scene objects, invoke lifecycle
callbacks, or draw a fade; application code observes its flags and progress and performs those
actions itself.

## API behavior

- `Zanna.Game.SceneManager.New()` creates an empty manager. The first successful `Add(name)` makes
  that name current and raises `JustEntered`.
- `Add(name)` registers up to 64 scenes and ignores a duplicate or a 65th entry. A name is stored
  in a fixed 127-byte buffer; a name that does not fit (128+ bytes) or contains an embedded NUL is
  rejected outright rather than truncated, so two distinct long names sharing a 127-byte prefix can
  no longer alias to the same registered scene (VDOC-243). `Add` returns no status, so callers that
  need to detect the cap or a rejected name must check with `IsScene` afterward.
- `Switch(name)` immediately changes to a known, different name and cancels a pending transition.
  Unknown/current names are no-ops.
- `SwitchTransition(name, durationMs)` starts a timer for a known, different target. Non-positive
  duration becomes 1 ms. Re-requesting the current/pending target is a no-op.
- `Update(dt)` first clears `JustEntered`/`JustExited` and the one-tick completion state. Positive
  deltas advance a transition; completion changes the name and raises both edge flags.
- `Transitioning` reports an active timer. `TransProgress` is 0 at start, advances toward 1, is 1
  only on the completing update tick, then returns to 0 on the next update.
- `Current` and `Previous` return empty runtime strings when unavailable, not null. Their registry
  return type is now `str`, so the results assign to `String` locals and pass to `String` parameters
  in Zia directly (VDOC-237).

## Example

```zia
module SceneManagerExample;

func start() {
    var scenes = Zanna.Game.SceneManager.New();
    scenes.Add("menu");
    scenes.Add("playing");
    scenes.SwitchTransition("playing", 500);
    scenes.Update(500);
    Zanna.Terminal.SayBool(scenes.IsScene("playing"));
    Zanna.Terminal.SayBool(scenes.JustEntered);
}
```
