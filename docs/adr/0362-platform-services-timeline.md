---
status: accepted
audience: contributors
last-verified: 2026-09-14
---

# ADR 0362: Platform Services Recording Timeline (Steam Timeline First)

## Status

Accepted. It extends the platform services program of
[ADR 0352](0352-platform-services-runtime-loaded-providers.md) and
[ADR 0353](0353-platform-services-player-features.md). Their policy, provider model, library
resolution, lifecycle, and argument rules apply unchanged.

## Context

Steam records gameplay in the background and shows a timeline of the session. Players scrub it
to save clips and screenshots. A game can mark that timeline with events such as a home run or a
walk-off. It can describe the state ("Top 9th, 3-2"), color the bar by what the player is doing,
and group long stretches (a game, a season) into phases that players find later. Legacy Baseball's
broadcast presentation has exactly these moments.

Steam exposes this through `ISteamTimeline`. The interface is `STEAMTIMELINE_INTERFACE_V004` in
SDK 1.61 and in SDK 1.65, and every flat function below has the same signature in both. The
feature is store-specific today, but "annotate a background recording" is not Steam-shaped. The
surface is neutral so other platforms with recording highlights can provide it.

Two needs fall outside the existing request object. A phase query returns four numbers
(recorded time, longest clip, clip count, screenshot count), and event ids are 64-bit handles.

## Decision

### Neutral surface

`Zanna.Services.Timeline` (static). Stateful members run on the main thread (ADR 0352 trap).

| Member | Signature | Contract |
|---|---|---|
| `SetGameMode(mode)` | `i1(i64)` | `TimelineMode` value |
| `SetTooltip(text, offsetSeconds)` | `i1(str,f64)` | Describe the current state |
| `ClearTooltip(offsetSeconds)` | `i1(f64)` | Remove the description |
| `AddEvent(title, description, icon, priority, offsetSeconds, clip)` | `str(str,str,str,i64,f64,i64)` owned | Instantaneous event; returns the event id or `""` |
| `AddRangeEvent(title, description, icon, priority, offsetSeconds, durationSeconds, clip)` | `str(str,str,str,i64,f64,f64,i64)` owned | Finished range; returns the event id or `""` |
| `StartRangeEvent(title, description, icon, priority, offsetSeconds, clip)` | `str(str,str,str,i64,f64,i64)` owned | Open range; returns the event id or `""` |
| `UpdateRangeEvent(eventId, title, description, icon, priority, clip)` | `i1(str,str,str,str,i64,i64)` | `priority` `-1` keeps the current priority |
| `EndRangeEvent(eventId, offsetSeconds)` | `i1(str,f64)` | Close an open range |
| `RemoveEvent(eventId)` | `i1(str)` | Delete an event added by this process |
| `RequestEventRecording(eventId)` | `obj<Zanna.Services.Request>(str)` owned | Completes with `Flag` = a recording covers the event, `Text` = event id |
| `StartPhase()`, `EndPhase()` | `i1()` | Phase boundaries |
| `SetPhaseId(phaseId)` | `i1(str)` | Persistent id for later queries |
| `AddPhaseTag(name, icon, group, priority)` | `i1(str,str,str,i64)` | Tag the current phase |
| `SetPhaseAttribute(group, value, priority)` | `i1(str,str,i64)` | Text attribute of the current phase |
| `RequestPhaseRecording(phaseId)` | `obj<Zanna.Services.Request>(str)` owned | Completes with `Value` = recorded ms, `Flag` = anything recorded, `Text` = phase id, details (below) |
| `OpenOverlayToPhase(phaseId)` | `i1(str)` | |
| `OpenOverlayToEvent(eventId)` | `i1(str)` | |

Offsets are seconds relative to now; negative values are in the past. Members that pass data to
the platform return `true` when the call reached it. Event ids are provider-defined text, like
user ids. On Steam they are decimal `TimelineEventHandle_t` values.

`Zanna.Services.Request` gains `DetailCount i64` and `Detail(index) i64(i64)`: up to eight
kind-specific integers (`RT_SERVICES_REQUEST_DETAIL_CAPACITY`). An index outside
`0..DetailCount-1` traps with `Services.Request.Detail: index <i> is outside 0..<n>`, or
`Services.Request.Detail: index <i> is out of range; the request holds no details`. For
`TimelinePhaseRecording` the details are 0 recorded milliseconds, 1 longest clip milliseconds, 2
clip count, and 3 screenshot count. Details are kept only for successful requests.

Constants (stable ordinals):

- `Feature.Timeline 13`.
- `RequestKind.TimelineEventRecording 6`, `RequestKind.TimelinePhaseRecording 7`.
- `TimelineMode`: `Playing 1`, `Staging 2`, `Menus 3`, `LoadingScreen 4` (the `ETimelineGameMode`
  ordinals).
- `TimelineClip`: `None 1`, `Standard 2`, `Featured 3` (the `ETimelineEventClipPriority`
  ordinals).

### Argument rules

Following ADR 0353:

- **Trap, with or without a provider:**
  - Empty title, tooltip text, event id, phase id, tag name, tag group, or attribute group:
    `Services.Timeline.<Member>: <what> must not be empty`.
  - `mode must be a TimelineMode value (got <n>)`.
  - `clip must be a TimelineClip value (got <n>)`.
  - `priority must be in 0..1000 (got <n>)`, or `priority must be in 0..1000 or -1 (got <n>)` for
    `UpdateRangeEvent`.
- **Values computed at run time return `false` or `""` with a diagnostic:**
  - `Services: Timeline.<Member> needs a finite time offset (got <o>)`.
  - `Services: Timeline.AddRangeEvent needs a finite duration of 0 seconds or more (got <d>)`.
- **Provider formats and limits:**
  - A malformed Steam event id traps only while Steam is active:
    `Services.Timeline.<Member>: Steam timeline event id '<id>' must be an integer in 1..18446744073709551615`.
  - Steam limits return `false`, `""`, or a failed request with a diagnostic:
    - `Steam: timeline range events last at most 600 seconds (got <d>)`
    - `Steam: timeline time <t> seconds is outside the float range`
    - `Steam: timeline phase id '<id>' is longer than 63 bytes`
    - `Steam: <AddInstantaneousTimelineEvent|AddRangeTimelineEvent|StartRangeTimelineEvent> returned no timeline event handle`

### Provider model extension

`rt_services_provider` gains `timeline`, a pointer to a static `rt_services_timeline_ops` table,
and the event description record `rt_services_timeline_event`. Recording queries go through
`begin_request` with the id in `rt_services_request_args.name`. `rt_services_request_result`
gains `details` and `detail_count`.

### Steam provider contract

Accessor `SteamAPI_SteamTimeline_v004`. One all-or-nothing group binds `SetTimelineTooltip`,
`ClearTimelineTooltip`, `SetTimelineGameMode`, `AddInstantaneousTimelineEvent`,
`AddRangeTimelineEvent`, `StartRangeTimelineEvent`, `UpdateRangeTimelineEvent`,
`EndRangeTimelineEvent`, `RemoveTimelineEvent`, `DoesEventRecordingExist`, `StartGamePhase`,
`EndGamePhase`, `SetGamePhaseID`, `DoesGamePhaseRecordingExist`, `AddGamePhaseTag`,
`SetGamePhaseAttribute`, `OpenOverlayToGamePhase`, and `OpenOverlayToTimelineEvent`. A missing
accessor or export records `Steam: <interface|export> <name> unavailable; timeline disabled`, and
queries fail with `Steam: the timeline is unavailable (see Platform.Diagnostics)`.

Mappings:

- Priority `-1` maps to `k_unTimelinePriority_KeepCurrentValue` (1000000).
- Times are passed as `float`.
- Constants pass through, because their ordinals equal the SDK enums.

Call results added to ADR 0352's table:

| Id | Call result | Payload bytes (pack 8 / pack 4) | Result |
|---|---|---|---|
| 6001 | `SteamTimelineGamePhaseRecordingExists_t` | 88 / 88 | completes `TimelinePhaseRecording` |
| 6002 | `SteamTimelineEventRecordingExists_t` | 16 / 12 | completes `TimelineEventRecording` |

Both layouts carry compile-time size and offset assertions and were checked against the SDK 1.61
headers. The flat functions were checked against the SDK 1.61 and 1.65 headers, and against the
exports of both redistributables.

## Consequences

- Games mark the Steam game recording timeline without a Steam-named class, and a later provider
  fills the same table.
- `Request.Detail` gives any later request kind a place for a few numbers without new classes.
- With a signed-in Steam client on macOS (app 480, SDK 1.61 and 1.65 libraries), every timeline
  member reached Steam and both queries completed; game recording was off, so they reported no
  recording.
- `RuntimeTypeId` gains `ServicesTimeline`, `ServicesTimelineMode`, and `ServicesTimelineClip`,
  appended so existing ordinals never shift.

## Alternatives Considered

- **Numeric (`i64`) event ids.** Rejected. Ids are provider-defined, and strings match user,
  DLC, and item ids elsewhere in the layer.
- **A `Zanna.Services.Steam.Timeline` extension.** Rejected. Annotating a background recording
  is a platform capability, not a Steam one.
- **A separate result object for phase queries.** Rejected in favor of `Request.Detail`, which
  keeps one request class for every asynchronous result.
- **Clamping out-of-range priorities.** Rejected. A priority outside the scale is a programming
  error, so it traps like other constant ranges.
