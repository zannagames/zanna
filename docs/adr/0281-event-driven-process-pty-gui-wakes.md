---
status: active
audience: contributors
last-verified: 2026-08-21
---

# ADR 0281: Wake GUI Event Loops for Process and PTY Activity

Date: 2026-08-20

Status: Accepted

## Context

Zanna Studio previously kept the GUI loop responsive to build, debugger, and
terminal output by selecting a zero- or four-millisecond polling interval while
any child process or PTY was active. That consumed CPU even when every stream
was idle, and it coupled frame cadence to process lifetime rather than actual
readiness. Fixed per-frame language-work item counts also made a nominal time
budget dependent on file size and machine speed.

The GUI backends already own the platform event wait, while Process and PTY own
their stream descriptors and lifecycle. Bridging those layers requires a
lifetime-safe callback, new public App methods, and one backend wake operation.
Those are cross-layer and public runtime C ABI changes, so an ADR is required.

## Decision

`Zanna.GUI.App` exposes `WatchProcess(process)` and `WatchPty(session)`. A
successful call associates the asynchronous source with the App's retained
activity-wake target. Output readiness, EOF, or process/session termination
signals that target. The target posts a harmless native event to the App's
window, causing an existing `PollWait` call to return without fabricating an
input event for Zia code.

The wake target is reference counted and serializes signal with invalidation.
App destruction first invalidates the target, waiting for an in-flight callback,
then releases its owner reference. Process and PTY registrations retain their
own references until unregistered. A late producer can therefore neither
dereference a destroyed App nor signal a recycled window.

Backend wake mechanisms are deliberately local to ZannaGFX:

- Win32 posts `WM_NULL` to the retained window.
- macOS posts an application-defined `NSEvent`.
- X11 sends a private client message and flushes the display.
- Wayland writes to a nonblocking self-pipe included in the display poll set.
- The mock backend records a lock-protected pending wake for deterministic tests.

A single process-wide activity worker multiplexes nonblocking readiness probes
for every watched Process and PTY. POSIX probes use zero-timeout `poll`; Windows
ConPTY and anonymous Process pipes use `PeekNamedPipe` plus a zero-timeout child
status check because pipe readability is not a waitable kernel object. The
worker is paced at 16 ms, never runs on the GUI thread, and posts at most one
wake per registration until Studio drains and rearms it. Unregistration is
serialized with probes before the owning handle closes descriptors. Failure to
create the shared worker or a registration is reported by `WatchProcess` or
`WatchPty`; Studio then uses a four-millisecond polling fallback for only that
source.

Studio's language-work lanes use a fair round-robin registry and an absolute
ten-millisecond deadline. Ready work requests an immediate frame; otherwise the
GUI wait deadline comes from the next scheduled job or normal idle policy.
Overlay visibility alone does not force polling because native input already
wakes the event loop.

## Consequences

- Idle builds, debuggers, and terminals no longer force the UI thread to spin.
- Stream monitoring has explicit register, rearm, unregister, and
  target-retention states; Process/PTy destruction unregisters before closing
  handles.
- Any number of concurrent child handles uses one background worker instead of
  one native thread and control pipe/event set per handle.
- A failed monitor allocation degrades to bounded Studio polling rather than
  making output permanently invisible.
- Platform GUI adapters gain `vgfx_wake_events`; it is a wake primitive, not a
  user-visible event and does not alter input ordering.
- Focused runtime and Studio policy probes cover wake delivery, fallback
  selection, absolute deadlines, terminal behavior, and clean teardown.

## Alternatives Considered

- **Keep four-millisecond polling while a process exists.** Rejected because
  process lifetime is not readiness and quiet long-running tools waste CPU.
- **Invoke Studio controllers directly from monitor threads.** Rejected because
  Zia UI/runtime state remains owned by the GUI thread.
- **Use one global wake callback.** Rejected because multiple Apps and teardown
  ordering require explicit per-owner lifetime and invalidation.
- **Use one monitor thread per child handle.** Rejected because build graphs and
  debugger helpers can create many concurrent processes; independently retained
  registrations preserve owner lifetime without linear native-thread growth.
- **Make the Windows monitor block only on the process handle.** Rejected because
  output can arrive long before process exit and anonymous pipe readability is
  not a waitable kernel object.
