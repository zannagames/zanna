# Zanna Runtime API Consistency Review — Class-By-Class

Date: 2026-07-15
Scope: the full frontend-visible `Zanna.*` runtime surface as reported by
`./build/src/tools/zanna/zanna --dump-runtime-api` on this checkout
(**514 classes, 7,384 functions, 5,272 methods, 1,828 properties**), reviewed
class by class for intuitiveness, internal consistency, sibling consistency,
and naming quality, and checked against the conventions already decided in
`misc/plans/runtime_overhaul2/` (naming/namespace decisions, failure model,
class shape).

Relationship to prior work: `misc/reports/runtime_api_surface_review.md`
(2026-07-01) covered mechanical surface defects, and its cleanup items are
landed (no aliases, no `ptr`, no case collisions, PascalCase members, `i1`
booleans). This review is the next layer: developer-experience and
convention-conformance issues that survive a mechanically clean catalog.

Severity legend:

- **P0** — actively misleading or confusing; a developer will write wrong code
  or fail to find the API.
- **P1** — inconsistent with the platform's own decided conventions or with
  sibling classes; erodes predictability.
- **P2** — cosmetic or documentation-level; fix opportunistically during the
  same renames.

## Executive Summary

All 514 public classes were reviewed member-by-member against the live
catalog, the decided conventions in `misc/plans/runtime_overhaul2/`, and
each other. The review records **255 severity-tagged finding sites (25 P0,
124 P1, 106 P2)** — 25 cross-cutting systemic issues (Part 1) and
per-class findings for every namespace (Part 2), with consolidated
delete/rename/shape/policy/audit corrections (Part 3).

The surface is mechanically clean (the 2026-07-01 cleanups held) but not
yet *predictable*. Three root causes generate most findings:

1. **Migrations add canonical names without deleting the old ones.**
   176 C symbols have 2–3 public names, 58 more at the method level, 266
   rows are marked legacy but still published, six 2D classes are verified
   whole-class duplicates (one particle system ships as three classes),
   and `IO.File`/`IO.Dir`/`Url` have *parallel implementations* of the
   same operations. Every duplicate teaches a developer a name that is
   about to be wrong.
2. **Shape rules exist but aren't enforced, so each subsystem grew its
   own dialect.** Failure handling is 13:1 against the decided
   Result/Option model (4,609 infallible / 402 nullable / 40 sentinel /
   13 side-channel vs 35 result); GUI reads state through 129 zero-arg
   `Get*` methods while the rest of the platform uses properties; events
   are consumed via typed batches, indexed cursors, ambient `Last*`
   props, and polled flags depending on the class; the input stack names
   the same key states three ways; time units come in four spellings
   plus ~80 unmarked members.
3. **Catalog metadata is inferred, and wrong where it matters.**
   `IO.File.ReadAllText` is published as infallible; `Option.Unwrap`
   (which traps by design) likewise; ~24 classes have the wrong
   `class_kind` (`GUI.Widget` is a "static-module" with 37 instance
   methods); 220 of 514 class summaries are template boilerplate — and the
   doc generator lowercases acronym-initial summaries into "cSV parsing".

The ten worst individual offenders:

1. `Threads.Monitor.Pause/PauseAll` — **wakes** waiting threads
   (signal/notify named as its opposite).
2. `Option.Value()` — returns NULL on None; a null sentinel living on the
   type that exists to eliminate null sentinels (`Result.OkValue/ErrValue`
   too).
3. Graphics3D under graphics-off — every constructor silently returns
   null, the exact shape the capability decision bans.
4. `Collections.Bag` — a *unique-strings set* named after the standard
   term for a multiset.
5. `Seq.Remove(index)` vs `List.Remove(value)` — same verb, opposite
   semantics, on the platform's two primary sequences.
6. The half-executed Text→Data move — JSON lives in `Text`, XML in
   `Data`.
7. `Input.Key` vs `Input.Keyboard.Key*` (97 constants twice) plus three
   polling vocabularies across `Keyboard`/`Manager`/`Action`/`Input3D`.
8. `UnionFind.SetSize` — a getter that reads as a setter; with
   `DateTime.ToLocal -> str` (formatting named as conversion) and
   `Math.Deg/Rad` (direction-less conversions).
9. `*Result` means three shapes: `Zanna.Result` (`Pty.OpenResult`), a
   bare Map (`ProcessHandle.ReadStdoutResult`), and typed objects
   (`Pathfinder.FindPathResult`).
10. `Terminal` — three parallel typed print families (`PrintInt` ≠
    `PrintI64` only in spelling) and four read-a-line entry points.

What "good" looks like already exists in-tree: `Localization` (locale
handles, complete Try/Parse pairs, writable config), `Game`'s typed
result/event objects (`PathResult`, `TableClickResult`,
`AnimationEventBatch`), `World3D`'s sub-object decomposition,
`Diagnostics3D`'s telemetry module, `TimeOfDay3D`/`Stopwatch` unit naming,
`BiMap`/`RwLock`/`Tcp` explicit pairing, and `HttpReq`'s unsafe-naming.
The hardening job is to make the rest of the surface look like those.

Suggested execution order (each step is one mechanical sweep + audit):
(1) the delete sweep (3.1) — it removes ~600 rows of confusion at zero
design cost; (2) declared metadata + audit gates (3.5) so nothing
regresses; (3) the rename sweep (3.2); (4) property-ization and
failure-model adoption (3.3); namespace moves last (largest diffs, purely
mechanical). All are pre-alpha breaking changes by policy.

## Part 1 — Cross-Cutting Findings

These issues span namespaces. Counts were computed mechanically from the live
dump (scripts preserved alongside the review notes); every named member below
is re-greppable in `--dump-runtime-api` output.

### CC-1 (P0) — 176 C symbols are published under two or three public names

The rename migration added canonical spellings but kept the old ones as
live rows, so the catalog now teaches two names for one operation with no
signal (beyond `stability: legacy` metadata) about which one to write:

- Decided renames where both spellings remain: `Bits.LeadZ` /
  `Bits.CountLeadingZeros`, `Bits.Rotl` / `Bits.RotateLeft`, `Bits.Ushr` /
  `Bits.ShiftRightLogical`, `BloomFilter.Fpr` / `FalsePositiveRate`,
  `Fmt.BoolYN` / `Fmt.YesNo`, `Fmt.NumPct` / `Fmt.Percent`, `Fmt.NumSci` /
  `Fmt.Scientific`, `Canvas.SetDTMax` / `SetMaxDeltaTime` (both 2D and 3D
  canvases), `get_Cap` / `get_Capacity` (Deque, Ring, Seq, LruCache, Channel),
  `BinaryBuffer.NewCap` / `NewCapacity`, `BiMap.Put` / `Set`, `LruCache.Put` /
  `Set`, `MultiMap.Add` / `Put`, `Parse.TryNum` / `TryDouble`, `Parse.NumOr` /
  `DoubleOr`.
- Factory dual names (see CC-14): `Light3D.Spot`/`NewSpot` (×4 light kinds),
  `Mesh3D.Box`/`NewBox` (×4 shapes), `Collider3D.Box`/`NewBox` (×3),
  `Material3D.PBR`/`NewPBR`, `Textured`/`NewTextured`, `FromColor`/`NewColor`,
  `World3D.WithCamera`/`NewWithCamera`, `WithHorizontalCamera`/
  `NewWithHorizontalCamera`.
- Whole-class mirrors (see CC-2).
- One spelling contains an underscore, unique in the entire surface:
  `Core.Convert.ToString_Double` / `ToString_Int` (duplicates of
  `ToStringDouble` / `ToStringInt`, which are themselves questionable — see
  the Core notes in Part 2).
- The class-method layer adds **58 same-class duplicate pairs** on top of the
  function-level count (same `c_symbol`, same signature, two method names on
  one class). Beyond the groups above, the method-level-only pairs are:
  `String.Contains`/`Has`, `Text.Json.Format`/`Stringify`,
  `Math.Vec2/Vec3.Mul`/`Scale`, `Math.Easing.EaseInQuad`/`InQuad` (and the
  Out/InOut forms), `Math.Random.Next`/`NextDouble`,
  `Collections.UnionFind.Clear`/`Reset`, and
  `Crypto.Module.EnableApprovedMode`/`EnableApprovedModeForProcess` (the
  `ForProcess` spelling is not a different scope — it is the same symbol).

**Correction.** Pre-alpha policy (already adopted in the 2026-07-01 review) is
to delete rather than deprecate. Drop every non-canonical row in one sweep;
the canonical winner for each group is listed in Part 3. Add an `rtgen` audit
that fails when one `c_symbol` maps to more than one public name unless the
row is explicitly tagged as an intentional cross-publication.

### CC-2 (P0) — whole classes exist twice under different names

- `Zanna.Input.Key.get_*` (97 constants) ≡ `Zanna.Input.Keyboard.get_Key*`
  (97 constants). The decision (`02-naming`, Input) makes `Input.Key`
  canonical, and its spellings are better (`Digit0` vs `Key0`,
  `NumpadDivide` vs `KeyNumDiv`). `Keyboard` should keep state queries only.
- `Zanna.Memory.GC` ≡ `Zanna.Runtime.GC` (all 7 members, same symbols).
- `Zanna.Memory.Retain/Release/RetainStr/ReleaseStr` ≡
  `Zanna.Runtime.Unsafe.*` — refcount mutation is the definition of unsafe;
  the decision says `Zanna.Memory` is safe-only.
- `Zanna.Error.SetThrowMsg/ClearThrowMsg/SetTrapFields/RaiseKind` ≡
  `Zanna.Runtime.Unsafe.*` — already decided (`05-failure`): trap mutation
  belongs only under `Runtime.Unsafe`.
- `Zanna.Crypto.Hash.{MD5,SHA1,CRC32,HmacMD5,HmacSHA1}[Bytes]` ≡
  `Zanna.Crypto.Legacy.Hash.*` — the weak algorithms were copied into
  `Legacy` but not removed from the stable `Crypto.Hash` class, defeating the
  point of the `Legacy` namespace.
- `Zanna.Core.Box.ValueType*` ≡ `Zanna.Runtime.Unsafe.ValueType*`.

**Correction.** Keep exactly one home each: `Input.Key`, `Runtime.GC`
(delete `Memory.GC`; `Zanna.Memory` keeps only safe utilities per decision),
`Runtime.Unsafe` for retain/release/trap/valuetype rows (delete the `Memory`,
`Error`, and `Box` copies; delete the `Zanna.Error` class if nothing else
remains), `Crypto.Legacy.Hash` for MD5/SHA1/CRC32 (delete them from
`Crypto.Hash`; CRC32 is a checksum, not a security hash — consider
`Text.Checksum` or `IO.Checksum` instead of `Legacy`).

### CC-3 (P0) — the Text→Data namespace split is half-executed

`Zanna.Data` owns structured data per the decision, and `Xml`, `Yaml`,
`Serialize` already live there — but `Json`, `JsonStream`, `JsonPath`, `Csv`,
`Toml`, `Ini` are still `Zanna.Text.*`. Today a developer finds XML under
`Data` and JSON under `Text`. This is the single most disorienting namespace
fact in the surface.

**Correction.** Move `Text.{Json,JsonStream,JsonPath,Csv,Toml,Ini}` to
`Zanna.Data.*` in one commit. No facades (pre-alpha delete policy).

### CC-4 (P0) — the decided failure model has barely landed

Member fallibility census across all class methods: 4,609 `infallible`,
402 `nullable`, 99 `traps`, 66 `option`, 40 `sentinel`, 13 `side-channel`,
8 `status`, **35 `result`**. The `Result`/`Option` decision (`05-failure`)
is outnumbered ~13:1 by the shapes it replaced:

- Fallible persistence returns bare `i1` success flags: `IO.SaveData.Load/
  Save/Remove`, `Game.Quests.Load/Save`, `Game3D.World3D.LoadState/SaveState`,
  `Graphics2D.Tilemap.Save`, `Graphics3D.LightProbeGrid3D.Load/Save`,
  `Game2D.SceneDocument.Save`, `Graphics3D.NavMesh3D.Export/Import`.
- 402 members return nullable objects where the decision calls for
  `Option<T>`.
- The known side channels (`RestClient.LastStatus/LastOk`, `Tls.Error`,
  `JsonStream.Error`, `Xml.Error`, `Yaml.Error`, `Serialize.Error`,
  `SceneDocument.LastError`, `Pty.LastError`,
  `AssetDiagnostics3D.LastLoadError`) are all still present.

**Correction.** This is the hardening gate with the highest leverage. Either
execute `05-failure` before freezing (preferred), or explicitly re-scope it;
do not freeze a surface that is 13:1 against its own failure policy. Priority
order: (1) side channels → `Result<T, ErrorInfo>`; (2) `i1` save/load →
`Result`; (3) nullable → `Option` for the documented-absence cases.

### CC-5 (P0) — fallibility metadata contradicts reality

`contract_source` is `inferred` for effectively the whole catalog, and the
inference is wrong in load-bearing places: `IO.File.ReadAllText/ReadAllBytes/
ReadAllLines`, `IO.BinFile.Read`, `IO.Stream.Read`, `IO.Archive.Read`,
`System.Pty.PtySession.Read`, and even `ProcessHandle.ReadStdoutResult`
(a `Result`-returning function) are all published as `fallibility:
infallible`. Any tool, doc generator, or completion engine consuming the
catalog will tell users file reads cannot fail.

**Correction.** Declare fallibility in `runtime.def` rows (the schema already
carries the field); add an audit that rejects `infallible` on `Result`/
`Option` returns and on the IO verb list (`Read`, `Open`, `Load`, `Connect`,
`Parse`, `Decrypt`, `Fetch`) unless explicitly overridden with a rationale.

### CC-6 (P1) — seven teardown verbs; 164 handle classes with none

Instance-handle teardown today: `Destroy` (44, GUI/Game), `Close` (17,
IO/network/canvas), `Stop` (13, animation/servers), `Free`
(`Sound.Music`, `Sound.Sound`, `Memory.WeakRef`), `Release`
(pools), `Shutdown` (`Threads.Pool`, `Sound.Audio`), `Delete`
(`Network.HttpClient/HttpRouter/RestClient` — where `Delete` is *also* the
HTTP verb on the same classes, so `client.Delete()` tears down the client
while `client.Delete(url)` issues a request; arity is the only
disambiguator). 164 instance-handle classes expose no teardown at all
(GC-only).

**Correction.** Adopt a two-verb policy and rename the rest: `Close` for
things you open (files, streams, sockets, canvases, PTY), `Destroy` for
things you create in a scene/UI tree; GC handles everything without external
resources (document that explicitly per class kind). Rename `Sound.Music.
Free`/`Sound.Sound.Free` → `Destroy`, `Threads.Pool.Shutdown` stays (graceful
semantics differ from teardown — but then `Sound.Audio.Shutdown` must match),
and rename the HTTP client teardowns (`HttpClient.Delete()` →
`Close`) — a P0-grade collision inside a P1 category.

### CC-7 (P1) — boolean property naming splits bare vs `Is*` across sibling classes

Eleven stems ship both shapes simultaneously, including within one domain:
`Physics3DBody.Static/Kinematic/Trigger` (bare) vs `Game3D.BodyDef.IsStatic/
IsKinematic/IsTrigger` (prefixed) — a body and its own definition object
disagree. Others: `Active` (3 bare / 10 `IsActive`), `Playing` (2/9),
`Enabled` (5/1), `Finished` (1/5), `Paused` (1/3), `Emitting` (1/3),
`Expired` (2/1), `Ready` (1/1).

**Correction.** The decided rule allows "direct property" naming, which is
what produced the drift. Tighten it: boolean *properties* use `Is`/`Has`/
`Can` prefixes, no exceptions; rename the ~17 bare properties (list in
Part 3).

### CC-8 (P1) — membership is `Has` in collections, `Contains` / `Exists` / `Includes` elsewhere

`Has` (34 collection classes), `Contains` (8: `String`,
`Time.DateRange`, hitboxes/triggers), `Exists` (`IO.File/Dir/Assets`,
`Input.Action`), `Includes` (`Game3D.LayerMask`), plus `MightContain`
(`BloomFilter` — correct, it is probabilistic).

**Correction.** Policy line: `Has` = key/id membership on keyed containers;
`Contains` = value/geometric containment; `Exists` reserved for external
resources (filesystem); rename `LayerMask.Includes` → `Contains`. This
matches the existing majority usage, so the rename cost is one method.

### CC-9 (P1) — cardinality names still drift beyond the decided Count/Length policy

`Size` appears on `Game.Grid2D`, `Game.ParticleSnapshot`, `IO.BinFile`,
`Network.ConnectionPool`, `Threads.Pool` (and as methods on `IO.File/
Assets`); `Quadtree.ItemCount`; `CountMap.Total`. Byte sizes vs element
counts vs worker counts are all "Size" somewhere.

**Correction.** Extend the decided policy with one line — `Count` for
elements, `Length` for strings/bytes/fixed sequences/geometry, `SizeBytes`
for byte sizes, no other cardinality names — then rename: `BinFile.Size` →
`SizeBytes` (hmm: it is a byte length — `Length` also acceptable; pick one),
`ConnectionPool.Size`/`Threads.Pool.Size` → `Count` (of connections/workers —
or `WorkerCount`), `Quadtree.ItemCount` → `Count`, `Grid2D.Size` → `Count`
(cells) or `CellCount`.

### CC-10 (P1) — time units: three suffix spellings and ~80 unsuffixed members

`Ms` (14: `Game.Timer.ElapsedMs`, `Stopwatch.ElapsedMs`, …), `Millis`
(`Time.Duration.FromMillis/TotalMillis`), `Sec` (`Canvas.DeltaTimeSec`,
`Canvas3D.DeltaTimeSec`), `Seconds` (10: `TimeOfDay3D.DayLengthSeconds`, …),
plus ~80 time-ish members with no unit in the name (`Tween.Duration`,
`VideoPlayer.Duration`, `Network.*.SetTimeout`, `Threads.Debouncer.Delay`,
`GUI.Tooltip.SetDelay`, `Sound.Music.Duration`, …) whose units are only
discoverable from docs, when documented at all. `Game.Timer` is
milliseconds-based while `Game.Tween`/`Game3D` are seconds-based — adjacent
gameplay classes, different clocks.

**Correction.** Units policy for the freeze: gameplay/rendering time is `f64`
seconds and *unsuffixed*; wall-clock/integer milliseconds always carry `Ms`;
`Millis`/`Sec` spellings are renamed to `Ms`/`Seconds` forms; every
`Duration`/`Timeout`/`Delay`/`Elapsed` member's doc string states the unit.
Audit: name containing `Timeout|Duration|Delay|Elapsed` must either carry a
unit suffix or have a doc line matching `in (seconds|milliseconds)`.

### CC-11 (P1) — IL type names leak into user-facing member names

The typed-variant families use IL names (`I64`, `F64`, `I1`, `Str`) while the
Zia language calls these `Integer`, `Number`, `Boolean`, `String`:
`Option.UnwrapI1/UnwrapOrI1`, `Core.Box.EqF64/EqI64/EqStr/ToI1`,
`Map.GetStr/GetOptStr/SetStr`, `Seq.GetStr`, `Tween.LerpI64`,
`Input.Mouse.DeltaXF/WheelXF` (`F` = float variant). In binary-IO contexts
(`MemStream.ReadI64`, `Bytes.ReadI32BE`) the IL name *is* the wire format and
is correct.

**Correction.** Keep IL-typed names where the type is the wire format
(binary buffers, streams); rename language-facing accessors to language
names (`UnwrapBoolean`, `EqNumber`, `GetString`…) or — better — evaluate
whether the generic forms plus casts can replace the suffix families
entirely before freezing (see B1/B2 class notes).

### CC-12 (P1) — ALL-CAPS acronym blocks violate the decided word-casing policy

The decision explicitly lists `HmacSha256`, `Sha256`, `Pbkdf2Sha256`, `Gltf`,
`Fbx`, `Ktx2` as targets and names `SetAOMap`/`AddSSAO`/`AddDOF` as the
anti-pattern — yet the live surface has `Crypto.Hash.SHA256/HmacSHA256/
Pbkdf2SHA256`, `Graphics3D.GLTF`, `Graphics3D.FBX`,
`TextureAsset3D.LoadKTX2`, and post-decision additions
`Material3D.SetAOMap`, `PostFX3D.AddSSAO/AddDOF/AddTAA/AddSSR/AddFXAA`.
Full inventory (occurrence counts): SHA 16, BE/LE 32, MD 8, AAD 8, LOD 8,
RGBA 7, FX 5, CBC 4, CRC 4, AABB 4, ISO 3, RGB 3, IK 3, plus singletons
(HSL, CW/CCW, YN, CIDR, OBJ, STL, LUT, FPS, PBR, …).

**Correction.** Apply the decided policy in one rename sweep (Part 3 table).
Decisions needed for: `BE`/`LE` (recommend full words `BigEndian`/
`LittleEndian` — `ReadI16Be` is unreadable), `AABB` (recommend keep as
standardized token users type — add to the exception list explicitly),
`FABRIK` (standardized algorithm name — exception list), `RGBA`/`RGB`
(recommend `Rgba`/`Rgb` per policy). `LOD`→`Lod`, `IK`→`Ik`, `FX`→`Fx`
read oddly but match the decided examples (`Http`, `Tls`); apply uniformly
or amend the policy — do not leave it half-applied.

### CC-13 (P1) — decided-rename abbreviations still live as the only spelling in places

Beyond the dual-published names of CC-1, some abbreviations exist with *no*
canonical alternative: `Math.Vec2/Vec3/Quat.LenSq` (no `LengthSquared`),
`Vec3.ClampLen`, `Vec2/Vec3.Dist` (no `Distance`), `Vec2/Vec3/Quat/Path.Norm`
(no `Normalize`; `IO.Path.Norm` too), `String.MidLen`,
`TextWrapper.MaxLineLen`, 2D physics `SetPos`/`SetVel` (vs 3D
`SetPosition`/`SetVelocity`), `Game.Dialogue.SetPos`, `SetBgColor`,
`Playlist.Prev`, `Camera3D.FPSInit/FPSUpdate`, `BodyDef` (vs a spelled-out
`BodyDefinition`), `Game3D.EnvHandle`. Domain-standard short math verbs
(`Mul`, `Div`, `Sub`, `Add`, `Dot`, `Cross`, `Lerp`, `Slerp`, `Abs`, `Min`,
`Max`) should be *explicitly* excepted rather than left ambiguous.

**Correction.** Rename table in Part 3; add the explicit exception list to
the naming policy so future additions don't relitigate `Mul` vs `Multiply`.

### CC-14 (P1) — factory naming: dual-published spellings and three surviving styles

Every class constructor is `New` (good — 302 of them). Named factories are
dual-published (`Light3D.Spot`+`NewSpot`, `Mesh3D.Box`+`NewBox`,
`Collider3D.Sphere`+`NewSphere`, `Material3D.PBR`+`NewPBR`,
`World3D.WithCamera`+`NewWithCamera`), and the live stability marks show
the chosen canon: the `New*` spellings are [legacy], the bare product
nouns are canonical. Even after that deletion, **three factory styles
survive as canonical**: bare nouns (`Light3D.Spot`, `Mesh3D.Box`),
`From<Source>` (`Material3D.FromColor`, `Sprite.FromFile`,
`Mesh3D.FromOBJ`), and `With<Config>` (`World3D.WithCamera` — also the
capacity constructors `Seq.WithCapacity`). Outliers: the `Time` namespace
uses `Create(parts)` (three classes), and `Camera3D.NewHorizontalFov`
names a factory after a parameter.

**Correction.** Delete the [legacy] `New*` duals (CC-18 sweep). Write the
factory-style rule down: bare noun = named variant of the product;
`From<Source>` = conversion from existing data; `With<Config>` =
constructor with non-default configuration; `Create` is not used (rename
the Time trio to `FromParts` or `New` overloads);
`NewHorizontalFov` → `WithHorizontalFov`. One paragraph in the naming
policy ends the drift.

### CC-15 (P1) — GUI reads state through zero-arg `Get*` methods; the rest of the platform uses properties

129 zero-arg instance `Get*` methods exist, and they cluster almost entirely
in GUI: `GUI.CodeEditor` (22), `GUI.App` (16: `GetWidth`, `GetHeight`,
`GetScale`, `GetTitle`, …), `GUI.Widget` (7), `Graphics.Canvas` (7:
`GetFps`, `GetTitle`, `GetScale`, …), plus `GetSelected`-style reads across
FindBar/CommandPalette/TabBar/VirtualList. Meanwhile Collections, Graphics3D,
Game, and Sound expose equivalent state as catalog properties
(`Slider.Value`, `Light3D.Intensity`, `List.Count`). Same platform, two
idioms — and within GUI itself `CommandState.GetEnabled()` coexists with
`Widget.SetEnabled()` and property-shaped `Checkbox.Checked` elsewhere.

**Correction.** Convert zero-arg `Get*`/paired `Set*` members to catalog
properties (writable where a setter exists) across GUI and Canvas; keep
method form only for reads with side effects or nontrivial cost (`WeakRef.
Get`, `Breadcrumb.GetClickedIndex`-style event pulls — which should move to
the event-poll shape per `07-class-shape` anyway). Audit: no new zero-arg
`Get*` instance methods returning plain values.

### CC-16 (P1) — `class_kind` metadata is wrong for at least 24 classes

`GUI.Widget` is "static-module" with 37 instance methods; `System.Process.
ProcessHandle` and `System.Pty.PtySession` are "static-module" handles;
`Graphics3D.NavMesh3D` "static-module" with 19 instance methods; `Math` is
"value-object"; `String`, `Input.Keyboard`, `Sound.Audio`,
`Graphics3D.SceneAsset` are "namespace-facade" with rich member lists;
`Material3D` is "value-object" with 12 mutating setters; `GUI.Menu`,
`MenuItem`, `ToolbarItem`, `StatusBarItem`, `Tab`, `TreeView.Node`,
`Core.Object`, `Sound.Sound`, and the 3D event/handle classes are
similarly mislabeled. Overhaul2 already decided kinds should be declared,
not inferred; the inference is now visibly wrong in the shipped catalog.

**Correction.** Declare `class_kind` per class in `runtime.def`; audit
declared kind against shape (instance methods ⇒ not static-module; setters ⇒
not value-object unless builder semantics are documented).

### CC-17 (P1) — 73% of object-returning methods return bare `obj`

1,090 methods return untyped `obj` vs 403 typed `obj<...>`. Worst: the Math
types themselves (`BigInt` 24, `Mat4` 22, `Mat3` 19, `Vec3` 19, `Vec2` 13,
`Quat` 13 — every arithmetic op returns `obj`), `Graphics.Pixels` 20,
`Game3D.Entity3D` 19, `Result`/`Option` (17/16 — `Result.Unwrap` returns
`obj`, necessarily generic, fine). The typed-handle decision
(`obj<Zanna.Domain.Type>` whenever known) is unmet exactly where the types
are best known: `Vec3.Add` obviously returns `Vec3`.

**Correction.** Annotate return handle types in `runtime.def` for all
concrete-typed returns; keep bare `obj` only for genuinely dynamic values
(`List.Get`, `Result.Unwrap`). Audit new rows.

### CC-18 (P1) — ~150 `legacy`-stability members still published inside stable classes

60+ stable classes carry marked-legacy members (the CC-1 duplicates plus
retired spellings): `Crypto.Hash` is 10 legacy / 9 stable — majority-legacy.
Pre-alpha policy is deletion, decided twice already.

**Correction.** Delete all `stability: legacy` rows in the same sweep as
CC-1. Anything a demo still uses gets migrated in the same commit
(repo-wide grep is the verification step).

### CC-19 (P1) — Prefab vs Template vs SceneTemplate name the same concept

One loader symbol is published as `Assets3D.LoadPrefab`,
`Assets3D.LoadTemplate`, *and* `Prefab.Load` (same for the
`Asset`/`Async` variants — 4 symbols × 3 names); `AssetHandle3D` has both
`GetPrefab` and `GetTemplate`; a `Game3D.SceneTemplate` class coexists with
`Game3D.Prefab`.

**Correction.** Standardize on **Prefab** (industry term). Delete `*Template`
method spellings; rename or fold `SceneTemplate` (verify in B13 whether it is
a distinct concept — if it instantiates scenes, `ScenePrefab` or merging into
`Prefab` both work).

### CC-20 (P2) — positional-parameter hazards in the worst call shapes

88 methods take ≥6 positional parameters. Beyond the defensible matrix/rect
literals: `Game.PlatformerController.Update(i64, i1,i1,i1,i1,i1,i1)` — six
consecutive booleans; `Mesh3D.SetBoneWeights(i64, i64,f64, i64,f64, i64,f64,
i64,f64)` — interleaved pairs; `SceneNode.SetTransform(10×f64)`;
`Vehicle3D.AddWheel(7×f64, i1, i1)`; `Tilemap.SetAutoTileLo/Hi(9×i64)`;
`Crypto.Tls.ConnectOptions(str,i64,str,str,i1,i64)`.

**Correction.** For the hardened surface, provide struct/options-object forms
(`InputFrame` for `PlatformerController.Update`, `Vec3`-taking overloads
for transforms, an options object for TLS) and keep the flat forms only where
they are the established canvas-style idiom. At minimum, the doc generator
must emit parameter names — the catalog currently has none, so completions
show `f64, f64, f64, …`.

### CC-21 (P2) — dimensional suffix policy is incoherent inside the 3D namespaces

`Graphics3D`: 57 suffixed / 7 plain (`FBX`, `GLTF`, `SceneAsset`,
`SceneGraph`, `SceneNode`, `Physics3DBody`, `Physics3DWorld` — the last two
with `3D` *infixed*). `Game3D`: 34 suffixed / 28 plain. And the same event
concept is `Game3D.Collision3DEvent` but `Graphics3D.CollisionEvent3D`.
Meanwhile `Graphics2D`/`Game2D` are almost entirely unsuffixed, and
`Graphics.*2D` classes (52 of them) carry the `2D` suffix *outside* a
dimensioned namespace.

**Correction.** Rule: inside `*3D`/`*2D` namespaces the suffix is uniform —
recommend keeping `3D` suffixes (they survive Zia `bind` aliasing, the reason
they exist) and renaming the 35 plain classes (`Physics3DBody` →
`PhysicsBody3D`, `Collision3DEvent` → `CollisionEvent3D`, `FBX` → `Fbx`
(+CC-12), `SceneNode` → `SceneNode3D`, …). The `Graphics` namespace's own
2D story is CC-22's subject.

### CC-22 (P2) — `Graphics` vs `Graphics2D` split contradicts the decided ownership

Decision: `Graphics2D` owns 2D rendering; `Graphics` keeps shared primitives
only. Live surface: `Graphics` holds 52 classes, most explicitly 2D
(`Renderer2D`, `SpriteBatch`, `Tilemap`-adjacent, `Emitter2D`,
`PostProcess2D`, …) while `Graphics2D` holds 4 (`SceneGraph`, `SceneNode`,
`Tilemap`, `TilemapRenderer2D`). A developer cannot predict which of the two
namespaces a 2D type lives in.

**Correction.** Execute the decided ownership before freeze: move the `*2D`
rendering classes into `Graphics2D` (dropping now-redundant suffixes per
CC-21, or keeping them for bind-friendliness — one rule, applied once), leave
`Color`, `Pixels`, fonts, image codecs in `Graphics`. This is the largest
mechanical move in the review (~45 classes) but it is a rename-only change.

### CC-23 (P2) — three different `Diagnostics`, and root names that collide with namespaces

`Zanna.Diagnostics` is simultaneously a root *class* (1 member) and a
namespace (`Diagnostics.Log`, `Diagnostics.TrapInfo`); `Zanna.Core.
Diagnostics` is a third thing (assertion helpers). `Zanna.Memory` is a root
class *and* the `Memory.GC`/`Memory.WeakRef` namespace; `Zanna.Math` is a
class and the `Math.*` namespace. `Aes`/`Hash` leaf-collide between `Crypto`
and `Crypto.Legacy` (intentional); `SceneGraph`/`SceneNode` collide between
`Graphics2D` and `Graphics3D` (pre-uniqueness-rule debt).

**Correction.** Rename `Core.Diagnostics` → `Core.Assert` (it is an assert
module); fold the root `Diagnostics` class's single member into
`Diagnostics.*`; resolve `Memory` per CC-2 (root class dissolves into
`Runtime.Unsafe`, leaving `Memory.WeakRef` — then consider `Collections` or
`Core` as WeakRef's home and retire the namespace). Class-vs-namespace
name sharing for `Math` is conventional and can stay.

### CC-24 (P2) — one-way lifecycle verbs

`Bind*` with no way to unbind: `SceneNode.BindBody/BindAnimator`,
`Cloth3D.BindMesh/BindBoneChain`, `NavAgent3D.BindCharacter/BindNode`,
`SoundListener3D.BindCamera/BindNode`, `SoundSource3D.BindNode`,
`LipSync3D.BindMorph/BindHeadBone/BindMouthShape`, `Input3D.BindPad`,
`Hitbox3D.BindWindow`, `GUI.Command.BindMenuItem/BindToolbarItem`,
`CommandRegistry.BindPalette`, `HttpServer.BindHandler`. `Register*` with no
`Unregister`: `Sound.SoundBank.Register/RegisterSound`,
`Sound.Audio.RegisterGroup`, `Game3D.Surfaces.Register`,
`GUI.TestHarness.RegisterWidget`. `Pause` with no `Resume`:
`GUI.VideoWidget`, `Game.AnimTimeline`, `Game.PathFollower`,
`Sound.Playlist`, `Threads.Monitor.Pause/PauseAll` (only `Input.Action` has
proper `Unbind*` symmetry). `TargetLock3D.Acquire` has no `Release`.

**Correction.** For each: add the inverse, or document the binding as
permanent-by-design in the member docs. Batch notes flag which classes
actually need the inverse (e.g., `Playlist.Pause` resumes via `Play` —
document; `Threads.Monitor.Pause` with no resume looks like a real API gap).

### CC-25 (P2) — 220 of 514 class doc summaries are template boilerplate

"Provides X functionality for Y-oriented programs" tells a completion tooltip
nothing. Not a rename issue, but hardening freezes docs anchors too.

**Correction.** Rewrite summaries for at least the 100 most-used classes
before freeze; audit that new classes ship a non-template summary.

## Part 2 — Class-By-Class Findings

Classes with no findings are listed as clean at the end of each namespace
section. Severity tags refer to the legend above; cross-references like CC-7
mean the item is an instance of that systemic finding and is fixed by its
correction.

### 2.1 Root types

**`Zanna.String`**

- **(P0)** `Contains(str)` and `Has(str)` are the same C function
  (`rt_str_has`) published as two instance methods. Keep `Contains`
  (CC-8 canon for value containment), delete `Has`.
- **(P1)** Three substring vocabularies on one class: `Mid(start)`,
  `MidLen(start,len)`, `Substring(start,len)` — plus `Left(n)`/`Right(n)`.
  Propose: `Substring(start,len)` and `Slice(start,end)` as the canonical
  pair; keep `Left`/`Right` (readable, common); delete `Mid`/`MidLen`
  (`MidLen` also violates the `Len` rename rule).
- **(P1)** `Asc()`/`Chr(i64)` are BASIC-isms with opaque names →
  `CodePointAt(i64)` (subsuming `Asc` = `CodePointAt(0)`) and
  `FromCodePoint(i64)`.
- **(P1)** `Flip()` → `Reverse()` (no other class says Flip for reversal;
  `Pixels.FlipX/FlipY` is a different, axis-qualified concept).
- **(P1)** Case-conversion family is split across receiver conventions and
  one name drops its suffix: instance `CamelCase/PascalCase/KebabCase/
  SnakeCase/ScreamingSnake` vs static `Capitalize(str)`/`Title(str)`.
  Rename `ScreamingSnake` → `ScreamingSnakeCase`, `Title` → `TitleCase`,
  make all instance methods.
- **(P1)** `Cmp`/`CmpNoCase` → `Compare`/`CompareIgnoreCase`; `LikeCI` →
  `LikeIgnoreCase` (CC-12/CC-13).
- **(P1)** `FromSingle(f64)` is misnamed ("Single" means f32; the parameter
  is f64) and `FromStr(str) -> str` is a public identity function. Delete
  both; `FromI16`/`FromI32` leak narrow IL integers used nowhere else in the
  public surface — hide or fold into `From(i64)`.
- **(P2)** `IndexOfFrom(startIndex, needle)` puts the start index before the
  needle, reversing `IndexOf(needle)`'s shape; `LastIndexOf` is static while
  `IndexOf` is instance. Align as instance `IndexOf(needle)`,
  `IndexOf(needle, start)` (arity overload), `LastIndexOf(needle)`.
- **(P2)** `Levenshtein`/`Jaro`/`JaroWinkler`/`Hamming` live on `String`
  while `Text.FuzzyMatch` and `Text.Diff` own the same problem space —
  move to `Zanna.Text` (algorithms namespace per the decided Text charter).
- Clean-but-doc-critical: `Count(needle)` = occurrence count adjacent to
  `Length`; summary must say "occurrences of needle" (CC-9 allowlist entry).

**`Zanna.Option` / `Zanna.Result`**

- **(P0)** `Option.Value()` returns the payload or **NULL when None**
  (verified in `rt_option.c`) — a null sentinel on the very type that exists
  to eliminate null sentinels, distinguishable from `Unwrap()` only by
  reading the C source. Same shape: `Result.OkValue()`/`ErrValue()`.
  Delete all three (callers use `Unwrap*`/`UnwrapOr*` after `IsSome`/`IsOk`,
  or `Expect`).
- **(P0)** `Option.Unwrap`/`Result.Unwrap`/`Expect` are published with
  `fallibility: infallible`; they trap on None/Err by design (CC-5
  flagship example).
- **(P1)** Typed-variant asymmetry: `Option` has `Some/Unwrap/UnwrapOr` ×
  {obj, Str, I64, I1, F64}; `Result` lacks every `I1` variant (`OkI1`,
  `UnwrapI1`, `UnwrapOrI1` do not exist). Fill or trim to matching sets.
- **(P1)** The typed suffixes are IL names (CC-11): `UnwrapI1` for a language
  whose type is `Boolean`.
- **(P2)** Both classes are `class_kind: namespace-facade` (CC-16); they are
  value objects.

**`Zanna.Terminal`**

- **(P0)** Three parallel typed print families coexist:
  `Print/PrintInt/PrintNum/PrintBool`, `PrintStr/PrintI64/PrintF64`, and
  `Say/SayInt/SayNum/SayBool`. `PrintInt` and `PrintI64` are the same
  operation under two suffix conventions; nothing in the names says `Say`
  appends a newline and `Print` does not. Collapse to one family:
  `Print(...)` / `PrintLine(...)` with language-typed suffixes only where
  the runtime needs them.
- **(P1)** Four read-a-line entry points: `ReadLine` [legacy], `InputLine`
  [legacy], `ReadLineResult`, `TryReadLine`; `Ask` [legacy] /`AskResult`/
  `TryAsk` mirror it. Delete legacy rows (CC-18), keep `*Result` + `Try*`.
- **(P1)** `GetKey` (blocking) vs `InKey` (non-blocking BASIC INKEY$
  heritage): the pair encodes its behavioral difference in trivia. →
  `ReadKey()` / `TryReadKey()`; `GetKeyTimeout(i64)` unit unmarked (CC-10)
  → `ReadKeyTimeoutMs` or document.
- **(P2)** `SetColor(i64,i64)` and `SetPosition(i64,i64)` have undocumented
  parameter meaning/order (fg,bg? x,y or row,col?) (CC-20).

**`Zanna.Memory` / `Memory.GC` / `Memory.WeakRef` / `Runtime.GC` /
`Runtime.Unsafe` / `Zanna.Error` (function rows)** — the mirrors are CC-2.
Additional:

- **(P1)** `WeakRef` publishes each operation twice on one class: instance
  `Get/Alive/Free/Reset` *and* static `Get(obj)/Alive(obj)/Free(obj)/
  Reset(obj,obj)`. Drop the statics.
- **(P1)** `Alive` → `IsAlive` (CC-7); `Reset(obj)` → `SetTarget(obj)`
  (Reset reads as clear-to-empty); `Free()` on a GC-managed weak reference is
  surprising (CC-6) — document or remove.
- **(P2)** `Memory.Release(obj) -> i64` returns an undocumented value
  (refcount?) — declare or return void.

**`Zanna.Diagnostics` (root class) / `Diagnostics.Log` /
`Diagnostics.TrapInfo` / `Core.Diagnostics`**

- **(P1)** Three different things answer to "Diagnostics" (CC-23):
  root class (1 member), the namespace, and `Core.Diagnostics` (an assertion
  pack: `Assert*`, `Trap`). Rename `Core.Diagnostics` → `Core.Assert`; fold
  the root class's `CurrentTrap` into the `Diagnostics` namespace.
- **(P1)** `Diagnostics.TrapInfo` is a set of *global static properties*
  reading ambient trap state (`Kind`, `Code`, `Message`, …) — the ambient
  side-channel shape, duplicating `Diagnostics.CurrentTrap -> Option<obj>`.
  Make `CurrentTrap` return `Option<TrapInfo>` where `TrapInfo` is an
  immutable value object; remove the static-reader form.
- **(P2)** `Log` models its level constants as read-only properties on the
  same class that has the writable `Level` property (`LevelDebug`,
  `LevelInfo`, …) → enum-like `Diagnostics.LogLevel`; `Log.Enabled(i64)` →
  `IsEnabled(level)` (CC-7).

**`Core.Box`**

- **(P1)** `EqI64/EqF64/EqStr` (CC-11, plus `Eq` abbreviation) →
  `Equals` per-type or single `Equals(obj)`.
- **(P1)** `ToI64/ToF64/ToStr/ToI1` trap on type mismatch but are marked
  infallible (CC-5); the `To*Option` variants are the safe forms — good
  pattern, keep.
- **(P2)** `Type() -> i64` returns a bare type tag with no enum class and
  duplicates the concept of `Core.Object.TypeId`. One name, one domain
  class for tags.
- CC-2: `ValueType`/`ValueTypeAddField` belong only in `Runtime.Unsafe`;
  the 1-method `Core.ValueType` class dissolves.

**`Core.Convert` / `Core.Parse`**

- **(P0)** `Convert.ToDouble(str)`/`ToInt64(str)` overlap `Parse.*` with
  undeclared failure semantics (marked infallible; returns 0 on garbage?
  traps?). Decide: `Parse` owns string→number (`Try*` + `*Or` forms —
  already canonical); `Convert` keeps numeric↔numeric and to-string only.
- **(P1)** `NumToInt(f64)` mixes vocabularies (`Num`=f64, `Int`=i64) and
  hides rounding semantics (truncate? round?) → `TruncateToInt64` /
  `RoundToInt64` as behavior demands.
- **(P1)** `ToStringDouble/ToStringInt` — verb-first order with IL-flavored
  type words; underscore twins are CC-1. One consistent shape:
  `Convert.ToString(f64)` is impossible under arity-only overloads, so
  `DoubleToString`/`IntToString` (subject-first, like `NumToInt`) or fold
  into `Text.Fmt`.
- **(P2)** `Parse.TryNum`/`NumOr` are [legacy] but `IsNum` is not — the
  `Num` spelling is half-retired. Finish: `IsNum` → `IsDouble`.
- **(P2)** `IntRadix(str,i64,i64)` — name and second i64 opaque →
  `TryIntRadix(text, radix) -> Option` (or document the default parameter).

**`Core.Object`**

- **(P1)** Instance `IsNull()` on a possibly-null receiver is paradoxical —
  the case it exists to detect is the case where calling it is least
  well-defined. Keep only static `IsNull(obj)`.

**`Core.MessageBus`**

- **(P2)** Static `Callback(obj) -> obj` — noun-as-method with no hint it
  wraps a handler for `Subscribe`; `Publish -> i64` return meaning
  undocumented (receiver count?); `Topics() -> obj` bare (CC-17).

### 2.2 System

**`System.Environment` vs `System.Machine`**

- **(P0)** Four system facts are published twice under different names:
  `Environment.HomeDir()` ≡ `Machine.Home`, `Environment.CpuCount()` ≡
  `Machine.Cores`, `Environment.Platform()`/`PlatformVersion()` ≡
  `Machine.Os`/`OsVer`, `Environment.UserName()` ≡ `Machine.User`.
  A developer cannot know which is canonical. Split cleanly: `Machine` =
  host facts (properties); `Environment` = process state (args, env vars,
  cwd, exit). Delete the duplicated getters from `Environment`.
- **(P1)** `Environment.EndProgram(i64)` → `Exit(code)` (industry term;
  "EndProgram" is a BASIC-ism, and `System.Shutdown` nearby makes it read
  like graceful-shutdown machinery, which it is not).
- **(P1)** `Environment.Cwd()` vs `IO.Dir.Current`/`SetCurrent` — two homes
  for the working directory. Keep it in `IO.Dir`; drop `Cwd`.
- **(P2)** `Machine.Temp` is the temp *directory path* — ambiguous with
  temperature → `TempDir`. `MemFree`/`MemTotal` → `MemoryFree*`/
  `MemoryTotal*` with byte units per CC-9; `OsVer` → `OsVersion`;
  `Endian: str` ("little"/"big") — stringly-typed → `IsLittleEndian: i1` or
  an enum-like domain.
- **(P2)** `Environment.GetVariable(str)` missing-variable behavior
  undeclared (empty-string sentinel?) — `TryGetVariable -> Option` needed
  per failure policy.

**`System.Exec` / `System.Process` / `ProcessHandle` / `Pty` / `PtySession`**

- **(P0)** `ProcessHandle.ReadStdoutResult()` and `PtySession.ReadResult()`
  return a `Collections.Map`, while `Pty.OpenResult` returns
  `Zanna.Result` — the `*Result` suffix now denotes two unrelated shapes.
  Rename the Map-returning forms (or better, make them return
  `Result<T>`); reserve the `*Result` suffix exclusively for
  `Zanna.Result` returns platform-wide.
- **(P1)** `Exec` vs `Process` split (blocking capture vs streaming handles)
  is real but the names don't communicate it, and `Exec` piles four shapes
  (`Run` → exit code, `Capture` → stdout, `Shell`/`ShellCapture`,
  `ShellResult` → `CommandResult`) plus a [legacy] side-channel
  `LastExitCode`. Make `CommandResult`-returning forms canonical for all of
  Exec; document Run-vs-Shell (argv vs shell-interpreted) in both summaries.
- **(P1)** `ProcessHandle.Poll() -> i1` next to `IsRunning() -> i1` — what
  Poll returns (exited? running? reaped?) is unknowable from the name.
  Rename to what it answers (`HasExited()`?) or drop for `IsRunning`.
- **(P1)** `Kill() -> i1` success flag (CC-4); `Wait() -> i64` blocks forever
  with no timeout variant (`WaitFor(ms)` exists on Threads types — mirror
  it); `Destroy()` vs `Kill()` lifecycle distinction undocumented.
- **(P2)** Both handle classes are `class_kind: static-module` (CC-16).

**`System.Shutdown`** — constants-as-properties (`None`/`Interrupt`/
`Terminate`) → enum-like domain class; `Pending()` → `IsPending()` (CC-7).
Otherwise a clean poll-based design.

**`System.Clipboard`** — `Get()`/`Set(str)` vs `HasText()` asymmetry →
`GetText`/`SetText`/`HasText` (P2).

**`System.CommandResult`** — `Succeeded` → `IsSuccess` per CC-7 (P2);
otherwise clean.

### 2.3 Tooling namespaces (Zia, Basic, Workspace, Project, Assets) — all preview

- **(P1)** `Zia.ProjectIndex` is a static class whose every method takes the
  handle as a leading `obj` (`IsValid(obj)`, `UpdateFile(obj,…)`) while
  `ProjectIndexHandle` sits empty (0 members); same for `SemanticJob`/
  `SemanticJobHandle`. Draw the class boundary on the handle: instance
  methods on `ProjectIndexHandle`/`SemanticJobHandle` (or collapse the
  handle classes and declare the static form intentional — current split is
  the worst of both).
- **(P1)** Dual result formats: string-returning (`Check`, `Hover`,
  `Symbols` — JSON text) beside Map-returning (`HoverInfo`, `SignatureInfo`)
  for the same queries. Pick the structured form as canonical before
  hardening; string forms are transport conveniences.
- **(P1)** `Project.Manifest.ParseText/ParseFile` trap on failure with no
  `Result` counterpart — direct violation of the decided Parse rule
  (`05-failure`).
- **(P2)** `Workspace.WorkspaceWatcher` — namespace-stuttered name, one
  method, and its `PollBatch(obj, i64)` takes a watcher handle produced by
  a *different* namespace's factory (`IO.Watcher`). Fold into `IO.Watcher`
  or move properly.
- **(P2)** `Assets.Resolver.Resolve(str,str,str,str)` — four positional
  strings, meanings invisible (CC-20).
- **(P2)** `Zia.Document.SyncDelta -> i1` success flag (CC-4);
  `SemanticJob.Error` [legacy side-channel] correctly has an `ErrorOption`
  replacement — good migration example, finish by deleting the legacy row.
- Clean: `Basic.LanguageService` (mirrors Zia.Completion shapes),
  `Workspace.Edit`, `Workspace.FileIndex` (subject to the Map-shape note
  above).

### 2.4 Collections + Functional

**`Collections.Bag`**

- **(P0)** The class is documented and implemented as a *set of unique
  strings*, but a "bag" in every mainstream library is a **multiset**
  (duplicates counted) — the opposite uniqueness guarantee. Anyone reaching
  for `Bag` gets silently deduplicated data; the actual multiset here is
  `CountMap`. Rename `Bag` → `StringSet` (and see the string-typed family
  note under `SortedSet`).

**`Collections.Seq` vs `Collections.List`**

- **(P0)** `Seq.Remove(index) -> obj` removes **by index** and returns the
  element; `List.Remove(value) -> i1` removes **by value** and returns a
  flag; `List.RemoveAt(index) -> void` is the index form. The same verb on
  the platform's two primary sequences means different things —
  `seq.Remove(5)` and `list.Remove(five)` do unrelated work. Align on
  `Remove(value) -> i1` + `RemoveAt(index) -> obj|void` everywhere.
- **(P1)** `List.Push/Pop` are the primary append/remove on a *list*
  (decided verb table says `Add` for multi-value collections); `Seq` also
  uses `Push`. Either amend the verb table to bless `Push/Pop` for
  sequences, or rename to `Add`/`RemoveLast` — one decision, applied to
  both.
- **(P1)** Functional vocabulary is split inside the same domain:
  `Seq.Keep/Reject/Apply` vs `Functional.LazySeq.Filter/-/Map` vs
  `Option.Filter/Map/AndThen` vs `Functional.Lazy.Map/FlatMap`. Three names
  for map (`Apply`, `Map`), two for filter (`Keep`, `Filter`), two for
  monadic bind (`AndThen`, `FlatMap`). Canon: `Map`/`Filter`/`Reject`(keep —
  it has no standard rival)/`FlatMap`; rename `Seq.Apply` → `Map`,
  `Seq.Keep` → `Filter`, `Option.AndThen` → `FlatMap` (or keep `AndThen`
  everywhere — one name, platform-wide).
- **(P2)** Capacity-constructor zoo: `Seq.WithCapacity` *and* `Seq.NewSized`
  (both exist), `Deque.WithCapacity`, `Ring.New(cap)` + `Ring.NewDefault()`.
  Canon `WithCapacity(n)`; delete `NewSized`, rename `NewDefault` → `New`.
- **(P2)** Orphan statics: `Seq.GetStr(obj,i64)`, `Map.GetOptStr(obj,str)` —
  single static typed getters stranded on otherwise instance-shaped classes.
  Fold into the typed-accessor decision (CC-11).

**`Collections.Map`**

- **(P1)** Typed accessors use a *third* type vocabulary: `SetInt/GetInt/
  GetIntOr`, `SetFloat/GetFloat/GetFloatOr`, `SetBool/GetBool/GetBoolOr`,
  `SetStr/GetStr` — where `Option` says `I64/F64/I1/Str` and Zia says
  `Integer/Number/Boolean/String`. Pick the language vocabulary once
  (CC-11) and apply to both classes.
- **(P1)** The typed family is incomplete: `GetIntOr`/`GetFloatOr`/
  `GetBoolOr` exist but `GetStrOr` does not.
- **(P1)** `Get(key) -> obj` returns null on missing key (null sentinel,
  CC-4) while `GetOr` exists; per the decided model the absent-lookup form
  should be `Find(key) -> Option` (Collections vocabulary decision).
- Same applies to `IntMap`, `OrderedMap`, `TreeMap`, `SparseArray`,
  `DefaultMap` (whose `Get` is genuinely total — the one correct `Get`),
  `Trie`, `WeakMap`, `LruCache`.

**`Collections.Queue`** — **(P1)** `Push/Pop` here vs
`Threads.ConcurrentQueue.Enqueue/Dequeue`: the same abstract type uses
different verb pairs depending on thread-safety. Pick one pair (recommend
`Push/Pop` to match the platform-wide stack/queue family, renaming
ConcurrentQueue's — or `Enqueue/Dequeue` on both; the point is one pair).

**`Collections.BitSet`** — **(P1)** `Clear(i64)` clears *one bit* while
every other collection's `Clear()` empties the container; `Set(i64)` sets a
bit while every map's `Set(k,v)` inserts. Rename bit ops to `SetBit/
ClearBit/ToggleBit/GetBit` (keeping `ClearAll` → `Clear()`), removing the
only place where the platform's two most common verbs mean something else.
Also: `Length` (bit capacity) + `Count` (population count) both present —
allowed by the Count/Length policy but the summaries must define them.

**`Collections.UnionFind`**

- **(P0)** `SetSize(i64) -> i64` *reads as a setter* but returns the size of
  the set containing the element. Rename → `ComponentSize(element)`.
- **(P1)** `Connected(i64,i64)` → `IsConnected` (CC-7); `Find` [legacy
  sentinel] / `FindRootOption` — after CC-18 deletion, rename
  `FindRootOption` → `FindRoot` (returning `Option`).
- **(P2)** Both `Clear()` and `Reset()` published for the same symbol
  (CC-1 method-level list).

**`Collections.SortedSet` / `TreeMap` / `OrderedMap`**

- **(P1)** Naming pair mismatch: the sorted set is behavior-named
  (`SortedSet`) but the sorted map is implementation-named (`TreeMap`).
  Rename `TreeMap` → `SortedMap`.
- **(P1)** `SortedSet` and `Bag` are **string-only** while `Set` is
  object-typed — the element type is invisible in the names. With `Bag` →
  `StringSet`, consider `SortedSet` → `SortedStringSet` or (better) make
  both generic like `Set` before freeze.
- **(P2)** Sibling asymmetries: `SortedSet` has `At/IndexOf/Range/Take/Skip`
  which `TreeMap` lacks; `OrderedMap.KeyAt` vs `SortedSet.At` (same concept,
  two names); `OrderedMap` has `KeyAt` but no `ValueAt`.
- **(P2)** `SortedSet.Take/Skip(i64) -> obj` (a collection) vs
  `Iterator.Skip(i64) -> i64` (count actually skipped) — same verb, one
  returns data, the other telemetry. Rename `Iterator.Skip` return or name
  (`SkipCount`?); type the SortedSet returns (CC-17).
- **(P2)** `Floor/Ceil` — `Ceil` → `Ceiling` (full-words rule; `Floor` is
  already a full word).

**`Collections.Bytes`** — `ToStr`/`FromStr` hide encoding semantics
(UTF-8? lossy?) → `ToUtf8String`/`FromUtf8String` (P2); `Copy(i64,obj,i64,
i64)` parameter meaning opaque (CC-20); `Find` [legacy sentinel] +
`FindOption` (CC-18 then rename to `Find`); BE/LE suffixes → CC-12 decision.

**`Collections.Heap`** — **(P2)** `NewMax(i1)` is a constructor with a
boolean selector (`NewMax(false)` builds a **min**-heap). Replace with
`NewMin()`/`NewMax()` no-arg factories. `Push(priority, value)` order must
be documented (CC-20).

**`Collections.Ring`** — `Peek` (method) and `First` (property) expose the
same element (verified distinct symbols, same concept) — keep `First`;
`OwnsElements`/`SetOwnsElements` is an ownership toggle whose meaning in a
GC runtime is not discoverable from the name — document or remove (P2).

**`Collections.BloomFilter`** — `Merge -> i64` undocumented return;
`New(i64,f64)` params opaque (capacity, target false-positive rate) (P2,
CC-20). `MightContain` is exemplary honest naming.

**`Collections.CountMap`** — `Inc/Dec/IncBy` → `Increment/Decrement/
IncrementBy` per full-words rule (P2); `Count` (distinct keys) vs `Total`
(sum of counts) needs summary-level definition (CC-9).

**`Collections.Set`** — `Items()` and `ToSeq()` are distinct symbols with
identical signatures and behavior-overlapping names — keep one (P2);
`Diff` → `Difference` (P2, full-words; also `Bag`, `FrozenSet`,
`SortedSet`).

**`Collections.IntMap`** — `Keys -> seq<obj>` (boxed) while `Map.Keys ->
seq<str>`; type it `seq<i64>` if the descriptor grammar allows (P2).

**`Collections.Iterator`** — `Count` property on an iterator is ambiguous
(remaining? total?) — define or drop (P2); `class_kind` facade (CC-16).

**`Functional.Lazy` / `LazySeq`**

- **(P1)** `LazySeq.Filter/Map` vs `Seq.Keep/Apply` — see the Seq
  vocabulary finding; whichever canon wins must cover both.
- **(P2)** `Lazy.Of/OfStr/OfI64` — `Of` vs the platform's `From*`
  factories; rename `Of` → `From` family. `Force()` vs `Get()` overlap
  (evaluate-only vs evaluate-and-return) needs the summary to say so.
- **(P2)** `LazySeq.ToSeqN(i64)` — cryptic `N` suffix → `TakeToSeq(n)` or
  `ToSeq(limit)` arity overload; `IsExhausted` vs `Iterator.HasNext` — two
  idioms for iteration-done (align on one, recommend `HasNext`);
  `LazySeq.Count()` is consuming — name or doc must warn (`CountRemaining`).

Clean: `BiMap` (model of explicit naming — `GetByKey/GetByValue/HasKey/
HasValue/RemoveByKey/RemoveByValue`; only the `Put`[legacy] dup and
empty-string sentinel on misses), `DefaultMap` (the one collection whose
`Get` is honestly infallible), `F64Buffer`/`I64Buffer` (consistent pair;
only `CopyFrom(obj)` vs `Bytes.Copy(...)` direction naming), `FrozenMap`/
`FrozenSet` (good value-object design; `class_kind` wrong per CC-16),
`LruCache` (`Get` vs `Peek` recency distinction is good design — document
it), `MultiMap`, `SparseArray`, `Stack`, `Trie` (`WithPrefix` collides
mildly with `With*` constructor idiom), `WeakMap` (`Compact -> i64` return
undocumented).

### 2.5 Text

**Parse family (`Json`, `Csv`, `Toml`, `Ini`, `Html`, `Version`,
`Project.Manifest`)** — **(P0)** every text-format parser in the platform is
trap-only (`Parse`, `ParseLine`, `ParseArray`, `ParseObject`, `ParseMajor`…
all `!trap`, no `Result`/`Option` forms). The failure decision explicitly
lists parse failure as not trap-worthy. This is the largest single block of
CC-4 work: add `ParseResult` (or make `Parse` return `Result` directly,
pre-alpha) across all six classes.

**`Text.Fmt` vs `Text.InvariantNumberFormat`** — **(P1)** two static
formatting classes in one namespace with overlapping members under different
names: `Fmt.IntGrouped` vs `InvariantNumberFormat.Thousands`, `Fmt.Size` vs
`.Bytes`, `Fmt.NumFixed` vs `.Decimals`, `Fmt.IntPad` vs `.Pad`, plus both
have `Percent`, `Currency`, `Ordinal`, `ToWords`. A developer cannot know
which is canonical. Fold numeric formatting into `InvariantNumberFormat`
(mirrors `Localization.NumberFormat`'s vocabulary); leave `Fmt` with the
radix/bit helpers (`Bin`/`Oct`/`Hex` — spell out or bless as exceptions) —
or merge entirely. Also in `Fmt`: the `Num*` → full-word migration stopped
halfway (`NumPct`/`NumSci` are [legacy] but `Num`/`NumFixed` are canonical);
`Size(i64)` is too generic for byte-humanization.

**`Text.TextWrapper`** — **(P1)** `Left(str,width)`/`Right(str,width)`
*pad-align* text while `String.Left(n)`/`Right(n)` *take substrings* — the
same names doing unrelated things in the two text APIs every program uses.
Rename to `AlignLeft`/`AlignRight` (keeping `Center`). Also `MaxLineLen` →
`MaxLineLength` (CC-13); `Truncate` vs `Shorten` difference is undocumented;
class name stutters (`Text.TextWrapper` → `Text.Wrapper` or `Text.Wrap`).

**`Text.Codec`** — **(P1)** half the class abbreviates, half doesn't:
`Base64Enc/Base64Dec/HexEnc/HexDec` beside `UrlEncode/UrlDecode`. Rename to
`*Encode`/`*Decode`. Overlaps `Bytes.ToBase64/FromBase64` (str-vs-bytes
input; charter the split in summaries).

**`Text.Json` / `JsonPath` / `JsonStream`**

- **(P1)** `Json.Format` ≡ `Stringify` (same symbol; CC-1). Keep one
  (`Format` matches `Csv`/`Toml`/`Xml`; `Stringify` is the JS-ism).
- **(P1)** Typed accessors are incomplete *and* introduce yet another type
  vocabulary: `GetStr/GetInt/GetBool` (no `GetFloat` — JSON is
  floating-point-native), `JsonPath.GetStr/GetInt` (no bool/float),
  `JsonStream.BoolValue/NumberValue/StringValue` (a fifth spelling set).
  One vocabulary (CC-11), complete families.
- **(P2)** `JsonStream.TokenType -> i64` and `Next -> i64` return magic
  token-type integers with no enum-like domain class; `TypeOf(obj) -> str`
  is stringly. Add `Json.TokenType`/`ValueKind` domains.
- **(P2)** `Json.NewObject` ≡ `Map.New` (same symbol) — transparent
  Map representation is fine, but publish one name.

**`Text.Scanner`** — **(P1)** `ReadInt()` and `ReadNumber()` return **str**
(the token text, unparsed) — names promise parsed values. Rename
`ReadIntToken`/`ReadNumberToken` (or return parsed `Option<i64>`/
`Option<f64>`). `Read()`/`Peek()` return codepoints as `i64` where
`IO.LineReader` says `ReadChar` — align. `Pos` → `Position` (CC-13).
`Match`/`Accept` (test vs consume) is good lexer design — document the pair.

**`Text.Pattern` / `CompiledPattern`** — the static/compiled split is good
design. **(P2)** `CompiledPattern` has `Captures`/`CapturesFrom`/`SplitN`
which `Pattern` lacks — complete the static mirror or document why not;
`SplitN` → `SplitLimit(n)` (the `N` suffix, also in `LazySeq.ToSeqN`, names
an arity not a meaning); the `Find`/`FindOption`(+`From`/`Pos`) sentinel
pairs are CC-18 deletions then `FindOption` → `Find`.

**`Text.Uuid`** — **(P2)** `New() -> str`: `New` is reserved for object
allocation platform-wide; a UUID here is a string value → `Generate()`.

**`Text.Version`** — **(P2)** instance `Cmp(obj)` + static `Compare(str,
str)` → one `Compare`; `ParseMajor/ParseMinor/ParsePatch` trap-trio →
`Parse(...).Major` etc.

**`Text.Html` / `Markdown`** — **(P2)** `Html.ExtractLinks -> obj` vs
`Markdown.ExtractLinks -> seq<str>` — same name, different return shapes in
sibling classes; type both `seq<str>`.

**`Text.Pluralize`** — **(P2)** `Count(i64,str)` formats `"3 dogs"` —
`Count` collides with the platform-wide cardinality noun → `Format(n,
noun)`.

**`Text.Char`** — **(P2)** `IsAlnum` → `IsAlphanumeric`; three predicates
that take `str` (single-codepoint strings) — merge into `String` or grow a
real `Char` charter.

Clean: `StringBuilder` (chaining `Append` returns bare `obj` — CC-17),
`Diff`, `FuzzyMatch` (Map-shaped result), `Template` (`RenderWith` params
opaque — CC-20).

**Docs generator bug (affects every acronym-named class)** — **(P1)**
summaries are sentence-cased by lowercasing the first character, producing
"cSV parsing and formatting", "jSON parsing", "iNI config", "sAX-style",
"uUID generation", "tOML config", "hTML parsing", "uTF-8 system clipboard"
in the shipped catalog and generated docs. Fix the transform (lowercase only
when the second character is lowercase).

### 2.6 Crypto

**`Crypto.Aes` vs `Crypto.Cipher`** — **(P1)** two stable classes both do
authenticated encryption with no discoverable charter: `Cipher` is
"high-level authenticated encryption" (password/key + AAD variants);
`Aes.EncryptAuth/DecryptAuth` is the same job one class over, while
`Aes.Encrypt/Decrypt` (unauthenticated CBC) are [legacy] mirrors of
`Legacy.Aes.EncryptCBC` (CC-2). After deleting the legacy mirrors, `Aes`
retains only `*Auth` + `*Str` rows that duplicate `Cipher`'s purpose.
Recommend: `Cipher` is the only high-level API; `Aes` either disappears or
becomes the explicit raw-primitive class (`Aes.EncryptGcm(key, nonce, …)`),
documented as expert-level. (`EncryptStr/DecryptStr` verified safe:
password-based AES-GCM per `rt_aes.c` "VAG1" format — but `rt_aes_encrypt_str`
returns NULL on null inputs instead of trapping (sentinel, CC-4), and
`DecryptStr` silently accepts the legacy unauthenticated CBC format —
document the downgrade acceptance or gate it before hardening.)

**`Crypto.Hash`** — **(P1)** `Fast`/`FastBytes`/`FastInt` are
non-cryptographic hashes living in the *Crypto* namespace under a name that
invites misuse for security purposes. Move to `Text`/`Collections` (seed
hashing) or rename `NonCryptoFast*`; a hardened crypto namespace should
contain only cryptographic primitives. The MD5/SHA1/CRC32 [legacy] mirrors
are CC-2 deletions; `SHA256`/`HmacSHA256` → `Sha256`/`HmacSha256` (CC-12,
explicitly decided).

**`Crypto.KeyDerive`** — **(P1)** `Pbkdf2SHA256` → `Pbkdf2Sha256`
(explicitly decided example); `ScryptSHA256` → `Scrypt` (scrypt is not
parameterized by SHA-256 at the API level; the suffix is misleading). The
`*Str` suffix here means "returns encoded string" while everywhere else
`Str` means "string-typed parameter variant" — one suffix, two meanings
(CC-11); use `*Encoded` or return one type.

**`Crypto.Module`** — **(P1)** the class name says nothing (it is the
FIPS-style approved-mode switch) → `Crypto.Compliance` (or `ApprovedMode`).
Uniquely in the platform, the *longer* names are canonical
(`IsApprovedModeForProcess` stable, `IsApprovedMode` [legacy]) while the
Enable/Disable pairs publish both lengths for the same symbols (CC-1).
Pick the short names, delete the rest. `EnableApprovedMode -> i1` success
flag → `Result` (CC-4); `Status() -> str` stringly.

**`Crypto.Rand`** — **(P2)** abbreviated while its sibling is not
(`Math.Random`) → `Crypto.Random`; the security distinction between the two
random APIs exists only in docs — both summaries must cross-reference
("not for cryptographic use" / "cryptographically secure").

**`Crypto.Tls`** — `class_kind: namespace-facade` for a connection handle
(CC-16); `ConnectOptions(str,i64,str,str,i1,i64)` opaque positional options
(CC-20); `Error()` [legacy side-channel] (CC-4); the `*For` timeout suffix
(`ConnectFor`) matches `Threads.WaitFor` — bless "`For` = with-timeout" as a
documented platform convention (see Threads notes). Otherwise the
trap/Result dual-publication pattern here is the intended migration shape.

### 2.7 Localization

The best-executed namespace in the platform: locale-handle pattern with
`ForLocale` factories, complete `Parse`/`TryParse`(Option) pairs on
`NumberFormat`, writable configuration properties, CLDR vocabulary. Findings
are mostly cleanup:

- **(P1)** `LocaleManager.TryLoadFromJson/TryLoadFromAsset -> i1` — a third
  meaning for the `Try` prefix (success flag with side effects) alongside
  `Try* -> Option` (canonical) and [legacy] `Try*` sentinels. The failure
  decision allows `i1` only for pure probes; these load. Make them
  `LoadFromJsonResult -> Result` (or void + trap like `Load`), and tighten
  the policy line: `Try*` returns `Option` — nothing else.
- **(P1)** `LocaleInfo.TextDirection(locale)` duplicates
  `TextDirection.OfLocale(locale)` — two homes for one query; keep the
  `TextDirection` class as the owner.
- **(P2)** `Locale.New()` — what locale is a bare new locale? (`Invariant()`
  and `Parse` exist) — remove `New` or document it as invariant;
  `TryParse` [legacy sentinel] → CC-18 then `TryParseOption` → `TryParse`.
- **(P2)** `TextDirection.IsRTL/IsLTR` → `IsRtl/IsLtr` (CC-12);
  `Bidi(str) -> str` opaque name/return.
- **(P2)** Stringly enums: `NumberFormat.RoundingMode: str`,
  `RelativeTimeFormat.Style: str`, `Numeric(i64, unit: str)` — enum-like
  domain classes exist elsewhere; use them. `RelativeTimeFormat.Format(i64)`
  unit (seconds? ms?) unmarked (CC-10).
- **(P2)** `DateFormat.AmPm(i1)`, `MonthName(i64, i1)`, `DayName(i64, i1)` —
  boolean selectors (CC-20); `DateFormat.DateOnly(obj,str)` takes an object
  where every sibling takes epoch `i64`.
- **(P2)** `PluralRules` `class_kind` static-module but instance-shaped
  (CC-16); `MessageBundle.Get(key)` missing-key sentinel behavior
  undocumented; `Keys -> obj` untyped (CC-17).

### 2.8 IO

**`IO.File` / `IO.Dir` — duplicate implementations, not just duplicate
names** — **(P0)** `File.ReadBytes`/`ReadAllBytes`, `ReadLines`/
`ReadAllLines`, `WriteBytes`/`WriteAllBytes`, `WriteLines`/`WriteAllLines`
are pairs of *independent C implementations* (`rt_file_*` vs `rt_io_file_*`)
with identical public signatures, all stable, none marked legacy. `Dir` has
`Dirs`/`DirsSeq`, `Files`/`FilesSeq`, and **three** whole-directory
enumerations (`List`, `ListSeq`, `Entries` — `rt_dir_list`,
`rt_dir_list_seq`, `rt_dir_entries_seq`). Two generations of the IO runtime
are both published. Keep the `All`-forms (`ReadAllBytes` — industry term)
and `Entries`/`Files`/`Dirs`; delete the rest and their C implementations.
- **(P2)** `File.Modified -> i64` epoch unit/name (`LastModifiedMs`?
  CC-10); `MoveOver` (move-with-overwrite) is guessable-but-odd →
  `Replace(src,dst)` or `Move(src,dst,overwrite)`.

**`IO.BinaryBuffer` vs `IO.MemStream`** — **(P1)** two overlapping in-memory
binary IO classes: `BinaryBuffer` is endian-explicit (`ReadI16LE/BE`),
`MemStream` is endian-*unmarked* (`ReadI16` — which byte order?) and adds
`F32/F64`; `MemStream.Seek(i64)` is 1-arg while `BinFile.Seek(i64,i64)`
takes a whence flag (magic int, no enum). Merge or charter the pair, mark
MemStream's endianness in docs at minimum, and give `Seek` one shape.
- Also **(P2)**: `BinFile.Size` vs `BinaryBuffer/MemStream/Stream.Length`
  for the same concept (CC-9); `Pos` → `Position` (CC-13); `Eof` here vs
  `Scanner.IsEnd` vs `LazySeq.IsExhausted` — three end-of-input names.

**`IO.Stream`** — the unifying facade leaks its two backends' differences:
`Stream.Read(n) -> obj` vs `BinFile.Read(buf,off,len) -> i64` vs
`MemStream.ReadBytes(n)` — three read shapes among the three classes the
facade unifies (P1). `Type -> i64` magic constant, no domain class (P2);
`OpenMemory`/`OpenBytes` marked `!trap` — an in-memory open that traps needs
its trap condition documented or the metadata fixed (CC-5).

**`IO.Compress`** — **(P1)** the `Str` suffix flips meaning inside one
class: `DeflateStr(str) -> obj` (*takes* string) but `InflateStr(obj) ->
str` (*returns* string). Same flip exists in `Crypto.KeyDerive` (CC-11).
`DeflateLvl` → `DeflateWithLevel` or a level parameter (CC-13).

**`IO.SaveData`** — **(P1)** `Load() -> i1` *and* `!trap` — it both returns
a success flag and traps, the least predictable failure contract in the
surface; make it `Result` (CC-4). Typed accessors are vocabulary #6
(`SetInt/SetString` — `String` spelled out here, `Str` elsewhere) and the
family is partial (no float/bool — games save floats). `GetInt(key,
default)` duplicates `Map.GetIntOr(key, default)` shape under a different
name — align (CC-11).

**`IO.Watcher`** — **(P1)** stateful event cursor: `Poll()` advances then
`EventPath()`/`EventType()` read the "current" event — the ambient-state
shape the class-shape decision replaces with event objects
(`PollEvents() -> Seq<Event>`); inline `Event*` constant properties →
enum-like domain (`IO.WatchEventKind`). `PollFor(i64)` unit unmarked
(CC-10).

**`IO.Path`** — **(P2)** abbreviation set (`Abs`, `Dir`, `Ext`, `IsAbs`,
`Norm`, `Sep`, `WithExt`) violates the decided full-words rule; these are
muscle-memory names from other ecosystems, so either rename
(`Absolute/Directory/Extension/Normalize/Separator/WithExtension`) or add
them to the explicit exception list — decide, don't drift.

**`IO.Archive`** — **(P2)** writer teardown is `Finish()` — an eighth
teardown verb (CC-6; `Close` for an IO handle); `Read(name)`/`ReadStr(name)`
— `Str` suffix means returns-string here (CC-11 flip again); `Info -> obj`
Map-shaped; `Names -> obj` untyped (CC-17).

**`IO.Assets`** — **(P2)** `Mount/Unmount -> i64` undocumented returns;
`Load` traps with no Result form (CC-4); `List() -> obj` untyped.

**`IO.LineReader` / `LineWriter`** — **(P2)** `LineReader.Read()` reads a
*line* (name says nothing; `PtySession.Read` reads available bytes) →
`ReadLine`; `LineWriter.WriteLn` → `WriteLine` (the platform's only `Ln`
abbreviation; `StringBuilder.AppendLine`, `File.AppendLine` spell it out);
static `LineWriter.Append(path)` is an open-for-append *factory* while
`File.Append(path, text)` is a write *operation* — same name, different
category (rename factory `OpenAppend`).

**`IO.TempFile`** — **(P2)** `Path()`/`PathWithPrefix()` return
non-created temp paths (race-prone pattern) — document the hazard or drop in
favor of `Create*`; `Dir()` (the temp directory) vs `CreateDir()` (make one)
read as a pair but aren't.

Clean: `IO.Glob` (`Match/Files/FilesRecursive/Entries` — consistent),
`BinaryBuffer` internals (endian-explicit family is exemplary; only the
`NewCap` [legacy] dup and `Pos` spelling).

### 2.9 Time

**`Time.DateTime` vs `Time.DateOnly` — two paradigms in one namespace**
— **(P1)** `DateTime` is a static module of 23 functions over bare `i64`
epochs (`Day(ts)`, `AddDays(ts,n)`); `DateOnly` is a proper value object
with properties (`.Year`, `.AddDays()`); `Duration` mixes both (instance
properties *plus* static `TotalSeconds(i64)` over raw i64); `DateRange` is
an instance handle. Four shapes for time values. `DateOnly` proves the
value-object form works — converge on it (or explicitly document `DateTime`
as the low-level epoch layer under a value-object `Instant`).
- **(P0)** `DateTime.ToLocal(ts) -> str` and `ToZone(ts, zone) -> str`
  return *formatted strings* — the `To*` conversion names promise converted
  timestamps. Rename `FormatLocal`/`FormatInZone` (the latter exists —
  `ToZone` duplicates it minus the format argument).
- **(P1)** `Now()` returns epoch *seconds*, `NowMs()` milliseconds — the
  unmarked default (CC-10 flagship with `Clock.Ticks`, below).
  `ParseISO`/`ParseDate`/`ParseTime` trap-only (CC-4); `ParseISO` →
  `ParseIso8601` (CC-12); `TryParse` [legacy sentinel] → CC-18.
- **(P2)** `DateTime.Create`, `DateOnly.Create`, `Duration.Create` — the
  Time namespace consistently uses `Create(parts)` where the rest of the
  platform uses `New` (see CC-14 — bless "Create = value-from-parts" or
  rename to `FromParts`; three classes, one decision).

**`Time.Clock`** — **(P0)** `Ticks() -> i64`: "tick" is not a unit — the
name gives no way to know it is milliseconds (with `TicksUs()` for
microseconds — `Us` for µs). Rename `NowMs`/`NowMicros` or
`MonotonicMs`/`MonotonicMicros` (also states the clock source, which
`DateTime.Now` vs `Clock.Ticks` currently leaves ambiguous). `Sleep(i64)`
duplicates `Threads.Thread.Sleep(i64)` — one canonical home (Threads),
units unmarked in both (CC-10).

**`Time.TimeZone`** — **(P1)** `Find(name)` *traps* on unknown zone —
directly violates the decided `Find -> Option` rule. `OffsetAt(ts) -> i64`
unit unmarked (seconds? minutes?) (CC-10).

**`Time.Countdown`** — **(P2)** `Elapsed`/`Remaining`/`Interval` props
unmarked units while sibling `Stopwatch` marks everything (`ElapsedMs/Ns/
Us`) — copy Stopwatch's discipline; `Expired` → `IsExpired` (CC-7 — its own
sibling `Game.Timer` says `IsExpired`); `Wait()` blocking behavior
undocumented.

**`Time.RelativeTime`** — **(P2)** overlaps
`Localization.RelativeTimeFormat` (invariant vs locale-aware — the
`Fmt`/`NumberFormat` split again); `Format(i64)` — epoch or delta,
seconds or ms, unknowable (CC-10).

**`Time.Duration`** — **(P2)** `Neg` → `Negate`, `Cmp` → `Compare`
(CC-13); `TotalSecondsF` — `F` float-variant suffix (CC-11);
`Millis` prop vs platform `Ms` suffix (CC-10 spelling table).

Clean: `Stopwatch` (unit-marked elapsed family + `StartNew` — the model
citizen of Time), `DateRange` (minor: `Duration() -> i64` unit unmarked;
methods where `Duration` uses properties).

### 2.10 Threads

**`Threads.Monitor`** — **(P0)** `Pause(obj)` **wakes one waiting thread**
and `PauseAll(obj)` wakes all — the signal/notify operation named as its
opposite (the C doc comment itself says "signal/notify pattern"). A
developer pauses; the runtime notifies. Rename `Notify`/`NotifyAll` (or
`Pulse`/`PulseAll`). This is the most misleading name in the surface.

**Timeout-suffix convention** — **(P1)** `*For(…, timeoutMs)` is used
consistently across `Channel.SendFor/RecvFor`, `Future.GetFor/WaitFor`,
`Gate.TryEnterFor`, `Monitor.TryEnterFor/WaitFor`, `Thread.JoinFor`,
`Pool.WaitFor`, `Tcp/Tls.ConnectFor` — except
`ConcurrentQueue.DequeueTimeout`. Bless "`For` = with-timeout-ms" as a
documented platform convention and rename `DequeueTimeout` → `DequeueFor`.
All `For` parameters are unmarked ms (CC-10) — the convention doc must say
so.

**Timeout return asymmetry** — **(P1)** `SendFor -> i1` but
`RecvFor -> obj` (null on timeout — sentinel), `Future.GetFor -> obj`
(same). Timeout-miss on the receiving side should be `Option` (CC-4).

**`Threads.Thread` / `Async` — the `Safe`/`Owned` modifier matrix** —
**(P1)** `Start`, `StartOwned`, `StartSafe`, `StartSafeOwned` (+
`SafeJoin`, `SafeGetId`, `SafeIsAlive`, `Async.RunOwned`,
`RunCancellableOwned`, `MapOwned`, `Promise.SetOwned`): two undocumented
one-word modifiers combine into four-way start variants. What `Safe` guards
(error capture? invalid-handle tolerance?) and what `Owned` transfers
(argument ownership) are unknowable from names. Either collapse (make safe
behavior the only behavior) or document both modifiers as platform
conventions with exact semantics.

**`Threads.Future`** — **(P2)** errors surface as `IsError: i1` +
`Error: str` string properties (same on `Thread.HasError/Error`) — the
failure decision's `Result`-carrying shape for async joins applies; `TryGet`
[legacy sentinel] → CC-18 then `TryGetOption` → `TryGet`.

**`Threads.CancelToken`** — **(P2)** `Check() -> i1` duplicates
`IsCancelled` under an ambiguous verb — delete; `Reset()` on a cancellation
token makes cancellation two-way (footgun; standard tokens are one-shot) —
document or remove.

**`Threads.Throttler`** — **(P2)** bare `Try() -> i1` (try *what*) →
`TryAcquire`; `Interval` unmarked next to `RemainingMs` marked —
intra-class unit inconsistency (CC-10). `Debouncer.Delay` same.

**`Threads.SafeI64`** — **(P2)** "Safe" here means atomic, while
`Thread.StartSafe` means error-capturing — one word, two meanings →
`AtomicI64` (industry term).

**`Threads.Pool`** — **(P2)** `Size` → worker count (CC-9);
`Submit -> i1` success flag (CC-4).

**`Threads.Scheduler`** — **(P2)** `ScheduleGen`/`IsDueGen`/`GenerationOf`
— `Gen` → `Generation` (CC-13); `Poll() -> obj` untyped (CC-17).

**`Threads.Barrier`** — **(P2)** `Arrive() -> i64` undocumented return
(arrival index?).

Clean: `Channel` (best Try*/For* discipline once legacy rows drop),
`ConcurrentMap`, `Gate` (semaphore semantics documented; only arity-overload
`Leave`), `RwLock` (exemplary paired naming), `Parallel` (consistent family;
`Reduce` parameter order needs docs — CC-20), `Promise`.

### 2.11 Math

**Two object paradigms inside one namespace** — **(P1)** `Vec2`/`Vec3`/
`Quat` are instance-method value types (`a.Add(b)`), while `BigInt`/`Mat3`/
`Mat4`/`Spline`/`PerlinNoise` are static modules over opaque handles
(`Mat4.Mul(a, b)`). Same namespace, same kind of math value, two calling
conventions. Converge on instance methods (the Vec/Quat shape) for all
math values.

**`Vec2` vs `Vec3` paradigm and feature split** — **(P1)** `Vec2.X/Y` are
read-only; `Vec3.X/Y/Z` are *writable* and `Vec3` adds `Set`/`CopyFrom` —
one vector type is immutable-style, the other mutable. `Vec3` has
`Reflect`/`Project`/`ClampLen`/`MoveTowards`/`Min`/`Max`; `Vec2` has none of
them (reflection and clamping are meaningful in 2D). `Vec2.Angle()` is
heading, `Vec3.Angle(other)` is angle-between — same name, different
semantics and arity. Decide mutability once, mirror the member sets, and
split `Angle` into `Heading()`/`AngleBetween(other)`.

**`Math.Deg` / `Math.Rad`** — **(P0)** one-word unit names that do not say
which direction they convert (`Deg(x)` — to degrees or from degrees?).
Rename `ToDegrees`/`ToRadians`.

**`Mat3.Eq(a,b,eps)` / `Mat4.Eq(a,b,eps)`** — **(P1)** epsilon-tolerance
comparison published as equality → `ApproxEquals` (and `Eq` → `Equals`
everywhere per CC-13: `BigInt.Eq`, `Box.Eq*`).

**`Math.Random`** — **(P1)** determinism trap: `Next*`/`Range`/`Seed` are
seeded instance methods, but `Gaussian`/`Exponential`/`Dice`/`Chance`/
`Shuffle` are *statics* using global RNG state — mixing them silently
breaks reproducible simulations (a platform that guarantees VM/native
determinism should make the seeded path the obvious one). Move the
distributions onto the instance; keep statics only as documented
conveniences. Also: `Next() -> f64` ≡ `NextDouble` (CC-1 dup — and a bare
`Next` returning float surprises anyone from ecosystems where `Next()` is
int; keep `NextDouble`, delete `Next`); `List.Shuffle()`/`Seq.Shuffle()`
don't say which RNG they use — document.

**`Math.Easing`** — the `In*`/`EaseIn*` dual family is ~15 CC-1 pairs;
keep the self-documenting `Ease*` spellings.

**`Math.Bits`** — **(P2)** `Count` (population count) collides with the
platform cardinality noun → `CountOnes` (or `PopCount`); `Flip` (reverse
bit order?) vs `Not` (complement) vs `Swap` (byte swap) are three
near-synonyms whose distinction lives only in docs → `ReverseBits`,
`Not`, `SwapBytes`. Legacy rows (`LeadZ`, `Rotl`, …) are CC-1/CC-18.

**`Math` (root class)** — **(P2)** `Sgn`/`SgnInt` → `Sign` (BigInt already
says `Sign` — same namespace, two spellings); `Trunc` → `Truncate`; `Ceil`
→ `Ceiling` (with `SortedSet`/`TreeMap`); the `*Int` suffix family
(`AbsInt`, `ClampInt`, `MaxInt`, `MinInt`, `WrapInt`) is vocabulary #7 for
typed variants (CC-11); `Euler` names 2.71828… — "Euler's constant"
conventionally means γ (0.5772…) → rename `E`.

**`Math.Quat`** — **(P2)** has `Norm` (normalize) but never received the
`Normalize`/`Length` canonical names `Vec2`/`Vec3` got — the CC-1 rename
sweep skipped it; also `Norm` for a *verb* is mathematically misleading
(the norm of a quaternion is its length). `FromEuler(f64,f64,f64)` —
angle unit unmarked (CC-10), order (pitch/yaw/roll?) undocumented (CC-20).

**`Mat3`/`Mat4`** — **(P2)** `Det` → `Determinant`, `Ortho` →
`Orthographic` (CC-13); `Rotate*/Perspective` angle units unmarked (CC-10);
`TransformPoint` vs `TransformVec` w-semantics is good design — document
it.

**`Math.BigInt`** — **(P2)** `FromStr` has no radix parameter while
`ToStrBase(obj, base)` exists — asymmetric; `FromStr` parse failure
behavior undeclared (CC-4); `Neg` → `Negate`, `Cmp` → `Compare`.

**`Math.Spline`** — **(P2)** `Eval` → `Evaluate`;
`ArcLength(obj,f64,f64,i64)` opaque positional params (CC-20).

Missing: no `Vec4` (Mat4 exists; 3D pipelines routinely need homogeneous
vectors) — note for the gap list, not a rename.

### 2.12 Network

**Five ways to make an HTTP request** — **(P1)** `Http.Get(url) -> str`
(one-shot, body only), `HttpClient.Get(url) -> obj` (session/cookies),
`HttpReq.New("GET", url)…Send()` (builder), `RestClient.Get(path)` (+
`GetResult`/`GetJson` families), and `AsyncSocket.HttpGetAsync(url)`
(futures, hidden inside a *socket* class). The same verb returns four
different shapes. Charter the stack explicitly in every summary ("simple
one-shot → `Http`; sessions → `HttpClient`; full control → `HttpReq`; API
clients → `RestClient`"), move `HttpGetAsync/HttpPostAsync` to an
`Http.GetAsync` spelling, and make return shapes consistent (`HttpRes`
everywhere below `Http`).

**Ambient last-thing state (three more instances)** — **(P1)**
`Udp.RecvFrom()` + `SenderHost()`/`SenderPort()` side-props;
`SseClient.Recv()` + `LastEventType`/`LastEventId`; (with `IO.Watcher`'s
event cursor from 2.8). The class-shape decision's event/result objects
replace all three: `RecvFrom -> Datagram{Data,Host,Port}`,
`Recv -> SseEvent{Type,Id,Data}`.

**URL-encoding exists twice** — **(P1)** `Network.Url.Encode/Decode`
(`rt_url_*`) and `Text.Codec.UrlEncode/UrlDecode` (`rt_codec_url_*`) are
independent implementations of the same transform (the `IO.File` parallel-
implementation pattern). Keep one (Codec), delegate or delete the other.

**`HttpServer`/`HttpsServer`** — **(P1)** route handlers register by
*string name* (`Get(pattern, handlerName)`) while `BindHandler(name, obj)`
wires the callback — a two-step stringly indirection where every other
callback API takes the callable directly. Offer `Get(pattern, callback)`
and keep the registry as the advanced path. (`WsServer`/`WssServer` and the
`Http`/`Https` pairs are otherwise cleanly parallel.)

**`RestClient`** — the `X` + `XResult` × 6 verbs migration is the intended
CC-4 shape, but the `*Json` family (`GetJson`…`DeleteJson`) has **no**
Result twins — the JSON convenience layer can only trap (P1). `Last*`
side-channels are [legacy] (CC-4). `DelHeader` → `RemoveHeader` (also
`Url.DelQueryParam` → `RemoveQueryParam`) — `Del` is not a platform verb
(P2). No teardown on a pooled/session client (also `HttpClient`) — bless
GC-teardown in docs or add `Close` (P2, CC-6).

**`Dns`** — **(P2)** `Resolve4`/`Resolve6` digit suffixes beside spelled
`IsIPv4`/`IsIPv6` in the same class → `ResolveIPv4`/`ResolveIPv6` (and
`IsIP*` → `IsIpv4`… under CC-12 acronym policy — decide the IP casing once);
`Resolve` failure returns sentinel `""` (CC-4).

**`NetUtils`** — **(P2)** grab-bag name; `LocalIPv4` overlaps
`Dns.LocalHost/LocalAddrs`; `MatchCIDR` → CC-12; `IsPortOpen(host, port,
timeout?)` third param unmarked (CC-10/CC-20). Fold into `Dns`/`Tcp` or
rename `Net`.

**`ServerReq` / `ServerRes` / `HttpReq` / `HttpRes`** — **(P2)** `Req`/`Res`
class-name abbreviations (full-words rule says `Request`/`Response`; if the
Express-style short names are wanted, add them to the exception list
explicitly). `ServerRes.Json(str)` takes pre-serialized JSON — either take
`obj` and serialize, or the name oversells (P2). `HttpRes.IsOk()`/`Body()`
are methods where the platform shape is properties (P2).
`HttpReq.SetTlsVerify`/`AllowInsecureCertificatesForTesting` [unsafe] is
exemplary unsafe-naming — the pattern to copy.

**`WebSocket`** — **(P2)** four `Connect*` statics form a For×Protocol
matrix — an options object (or `ConnectWith(opts)`) collapses it; `Ping()`
with no visible pong story (documented auto-reply?).

**`RateLimiter` vs `Threads.Throttler`** — **(P2)** two rate limiters in
two namespaces (token-bucket vs interval); rate limiting is not
network-specific — co-locate (Threads) and cross-reference.
`TryAcquireN(i64)` → `TryAcquire(n)` arity overload (the `N`-suffix again).
`RetryPolicy.Exponential(i64,i64,i64)` opaque params; `NextDelay -> i64`
unmarked ms (CC-10); `CanRetry` + `IsExhausted` are mutual negations —
keep one.

**`SmtpClient`** — `Send(str,str,str,str)` — four positional strings
(CC-20); [legacy] `Send`/`SendHtml` + `LastError` → CC-18/CC-4.

Clean: `Tcp`/`TcpServer` (model client/listener pair: `Connect`/`Listen`
factories, `Send`/`SendAll` partial-vs-total, `RecvExact`, `AcceptFor` —
only `class_kind` facade mislabels and unmarked timeout units),
`ConnectionPool` (`Acquire`/`Release` proper pair), `Multipart`, `Url`
(writable-prop builder done right; `Pass` → `Password`, `Parse` traps
CC-4), `HttpRouter` (`Get(pattern)` route registration reads oddly beside
client `Get(url)` but is arguably idiomatic), `RouteMatch`.

### 2.13 Input

**Three polling vocabularies for one concept** — **(P0)** the input stack's
layers each name the same three states differently:
`Keyboard.IsDown/WasPressed/WasReleased`,
`Manager.KeyHeld/KeyPressed/KeyReleased`,
`Action.Held/Pressed/Released` (and `Game3D.Input3D.Pressed/Released/
MousePressed` — see 2.17). "Pressed" means edge-this-frame in all of them,
but a developer moving between layers must relearn the spelling each time —
in the most-used API a game has. Pick one triple (recommend
`IsDown`/`WasPressed`/`WasReleased` — unambiguous tense) and apply it to
all four surfaces.

**`Input.Mouse`** — **(P1)** `Left()`/`Right()`/`Middle()` are bare-noun
`i1` methods (`Mouse.Left()` reads like a position or a constant) →
delete in favor of `IsDown(ButtonLeft)`; `Keyboard.Shift()/Ctrl()/Alt()/
CapsLock()` same shape → `IsShiftDown()` etc. or a `Modifiers()` snapshot.
`DeltaXF/WheelXF` float variants (CC-11) vs
`Manager.ScrollHorizontalFloat` — the same float-variant concept spelled
two ways inside Input. `Show/Hide/IsHidden` overlaps `GUI.Cursor.
SetVisible` — two cursor-visibility owners.

**`Input.Action`** — the platform's best `Bind*`/`Unbind*` symmetry —
keep it. **(P2)** `Save() -> str` *returns* the serialized bindings
(`Save` promises persistence) → `Serialize()`; `Load(str)`/`LoadPreset`
trap (CC-4); `BindingsStr` Str-return-suffix (CC-11); the `Axis*` constant
properties belong in the decided `Input.GamepadAxis` companion domain, as
`Pad.Button*` belong in `Input.GamepadButton`.

**`Input.Pad`** — **(P2)** `SetDeadzone(f64)` is *global* while every
other member is per-pad (`IsDown(pad, button)`) — scoping surprise;
`Vibrate(pad, low, high)` has no duration and no visible stop-condition
other than `StopVibration` — document. Constants → `GamepadButton` domain
(decided).

**`Input.Manager`** — **(P2)** static `Destroy(obj)` beside instance
`New()` (the only static-destroy in Input); `KeyPressedDebounced` +
`DebounceDelay` unit unmarked (CC-10).

**`Input.KeyChord`** — **(P2)** `Active(str)`/`Triggered(str)` →
`IsActive`/`WasTriggered` (CC-7); `DefineCombo(name, keys, i64)` last
param unmarked (timeout ms?) (CC-10/20).

`Input.Key` (canonical constants) is clean — its `Keyboard.Key*` mirror
is the CC-2 deletion.

### 2.14 Sound

**Namespace decision unexecuted** — the 02-naming decision makes
`Zanna.Audio` the canonical root; all 9 classes still live under
`Zanna.Sound`, including `Zanna.Sound.Sound` (namespace-stuttered leaf) and
`Zanna.Sound.Audio` (the future root name as a *member* class). Executing
the rename dissolves both oddities (`Audio.Sound`, `Audio.System` or
`Audio.Mixer` for today's `Sound.Audio`).

**`Sound.Sound`** — **(P1)** `PlayEx(vol, pan)` and `PlayEx2(vol, pan,
pitch)` — versioned `Ex` suffixes (with `PlayExGroup`, `PlayLoopGroup`,
`PlayExGroup`…) → arity overloads `Play(vol, pan)`, `Play(vol, pan,
pitch)` or an options object; a hardened API cannot carry `Ex2`.
- **(P1)** `Play* -> i64` returns a **voice id** consumed by
  `Voice.SetVolume(id, …)` — the central audio contract is an untyped
  `i64` with no named type or doc thread connecting the two classes.
  Introduce a `Voice` handle (or at minimum document the id flow in both
  summaries).
- **(P2)** `Free()` teardown (CC-6 → `Destroy`); `class_kind`
  static-module with 8 instance methods (CC-16); `Load` vs `LoadAsset`
  (file vs embedded asset) needs both summaries to say which.

**`Sound.Audio`** — **(P2)** dual group addressing: `SetGroupVolume(id,…)`
vs `SetGroupVolumeNamed(name,…)` (a `Named` suffix family); `GroupAddReverb`
*and* `GroupSetReverb` (append vs configure? the pair reads as a typo);
`Init() -> i64` undocumented return (CC-4); `SetMasterVolume(i64)` scale
undocumented (0–100? 0–255?) — same for `Music.Volume`, `Playlist.Volume`
(CC-20); `Fx` casing (CC-12); `SetGroupDucking(str,str,f64,f64,f64)`
opaque (CC-20). The backend telemetry properties are good.

**`Sound.Voice`** — **(P2)** `EnableMetering(id, i1)` — an `Enable*` verb
that takes a disable flag → `SetMetering(id, on)`; `SetPitch` is a rate
multiplier, not an angle/frequency — summary must say so.

**`Sound.Music`** — **(P2)** `Play(i64)` param unmarked (fade ms?);
`Position`/`Duration` units unmarked (CC-10); `SetLoop` → writable `Loop`
property (S4 pair); `Free` (CC-6). `Pause`/`Resume` here while `Playlist`
has `Pause` + `Play`-as-resume — align the resume story.

**`Sound.MusicGen`** — **(P2)** `SetChannelVol` → `SetChannelVolume`
(CC-13); `AddNoteVel` → velocity variant naming (CC-13);
`SetEnvelope(i64×5)` opaque ADSR order (CC-20); `Length` rw — of what,
in what unit (CC-9/10).

**`Sound.SpatialAudio3D`** — **(P1)** spatial audio now has three homes:
this class, `Graphics3D.SoundSource3D`/`SoundListener3D`, and
`Game3D.Sound3D`. Which layer owns 3D audio must be chartered before
freeze (see 2.16/2.17 notes); `UpdateVoice`/`SyncBindings` are
engine-internal names published as API.

**`Sound.Playlist`** — **(P2)** `Prev` → `Previous` (CC-13); `Repeat: i64`
magic mode int (needs `RepeatMode` domain); `Crossfade: i64` unit
unmarked (CC-10).

Clean: `SoundBank` (`Register`/`RegisterSound` returns undocumented but
shape is sound), `Synth` (`Sfx(i64)` preset id opaque — CC-20).

### 2.15 Graphics + Graphics2D

**Duplicate classes (verified by shared C symbols)** — **(P0)** the 2D
stack ships five same-concept class pairs:

| Pair | Evidence | Keep |
|---|---|---|
| `Emitter2D` ≡ `ParticleSystem2D` ≡ `Game.ParticleEmitter` | all 15 method symbols shared (`rt_particle_emitter_*`) — one particle system published as **three classes in two namespaces** | one name, one namespace |
| `ScreenScaler` ≡ `Viewport2D` | 7 of 8 symbols are `rt_viewport2d_*` | `Viewport2D` |
| `Surface2D` ≡ `RenderTarget2D` | 4 of 5 symbols shared | `RenderTarget2D` |
| `SpriteFont` ≡ `BitmapFont` | doc says "game-facing alias"; `TextWidth` shared | `BitmapFont` |
| `Texture2D` ≈ `GpuTexture2D` | identical member sets; `ClonePixels` shared | charter or merge |

Near-duplicates by concept: `Material2D` vs `BlendState2D` (same three rw
props `Tint/Alpha/BlendMode`), `Shader2D` vs `PostProcess2D` (same state as
props vs setters — and neither is a programmable shader; `Shader2D.Effect:
i64` oversells), `SpriteSheet` vs `TextureAtlas` vs `TexturePackerAtlas`
(the first two *describe each other* in their summaries and differ only in
API shape), `TextRenderer2D` vs `TextLayout2D`, `Camera` vs `CameraRig2D`
(both own smoothing + deadzone). Each pair needs one survivor or a
documented charter.

**Canvas vs Pixels drawing verbs** — **(P0)** the two primary drawing
surfaces name the same operations differently: `Canvas.Box/Disc/Line/Text/
Blit` vs `Pixels.DrawBox/DrawDisc/DrawLine/DrawText`. And a third
vocabulary exists: `DebugDraw2D.Rect/Circle` and `ShapeRenderer2D.Rect/
Circle` vs Canvas's `Box/Disc`. One verb set (recommend `Draw`-prefixed
`Rect/Circle/Line/Text` — industry standard) across all four surfaces.

**`Graphics.Canvas` (86 members)** — beyond the CC-decided decomposition:
- **(P1)** input polling lives on the drawing surface (`KeyHeld(key) ->
  i64` — returning `i64` where `Input.Keyboard.IsDown` returns `i1`;
  `Poll()`) — the BASIC-era shim duplicating `Input.*` with different
  types. Hardening should route input through `Input` only.
- **(P1)** `BeginFrame() -> i64` pairs with `Flip()` (not `EndFrame`) and
  returns an undocumented i64; `DeltaTime: i64` (ms) beside
  `DeltaTimeSec: f64` — dual-unit twin props (CC-10).
- **(P2)** 12-variant `Text*` matrix (`TextFontCenteredScaledBg`-style
  combinations); `PreventClose(i64)` takes an int for a boolean;
  `SaveBmp/SavePng -> i64` status codes (CC-4); `Get*` zero-arg methods
  (CC-15).

**`Graphics.Pixels` (60 members)** — **(P1)** mutation semantics are mixed
with no naming signal: `Fill`/`Set`/`DrawLine` mutate in place while
`Blur`/`Scale`/`Rotate`/`Invert`/`Grayscale`/`Tint`/`Resize` return new
buffers — `p.Blur(3)` silently does nothing to `p`. Convention needed
(mutating verbs vs `Blurred()`-style past-participle returns, or
document + typed returns). `Scale(i64,i64)` and `Resize(i64,i64)` both
exist — indistinguishable by signature (P1). `Get/GetRGBA/GetColor` and
`Set/SetRGBA/SetColor/SetRGB` triples whose differences (packed vs
components?) are invisible (P2); `RotateCW/CCW` → CC-12; `Load*` traps
(CC-4).

**Integer-typed 2D transforms** — **(P1)** `Camera.Zoom/Rotation: i64`,
`Sprite.ScaleX/Rotation: i64`, `Transform2D.*: i64`,
`Graphics2D.SceneNode.ScaleX/Rotation: i64`, `CameraRig2D.SetSmoothing
(i64)` — scale/rotation/zoom as integers (percent? fixed-point?
degrees?) while every 3D equivalent is `f64`. The unit/scale is
undocumented in every case (CC-10/CC-20); decide the 2D numeric model
(f64 like 3D, or document the integer scale) before freeze.

**Chatty per-field getters** — **(P1)** `TextureAtlas.GetX/GetY/GetWidth/
GetHeight(name)`, `ObjectLayer2D.GetX/GetY/GetWidth/GetHeight/GetType(i)`,
`Path2D.GetX/GetY(i)`, `Tilemap.ToTileX/ToTileY/ToPixelX/ToPixelY` — four
calls to retrieve one rectangle. Return `Rect`/`Vec2`-style value objects
(the 3D layer already returns `Vec3`).

**Three sprite-animation classes** — **(P1)** `Sprite` (built-in frames +
delays), `AnimatedSprite2D` (clip-based), `SpriteAnimator` (named clips) —
with three different `Update` signatures (`Update()`, `Update(i64)`,
`Update(obj)`). Charter or merge; align `Update` (see the platform
`Update(dt)` note in 2.16).

**`Graphics2D.Tilemap` (55 members)** — layer-parameter duplication
(`SetTile`/`SetTileLayer`, `Fill`/`FillLayer`, `Clear`/`ClearLayer`,
`Draw`/`DrawLayer` — implicit layer-0 convention, undocumented);
`SetAutoTileLo/Hi(9×i64)` (CC-20 worst case); `CountDrawn*` telemetry
uses the `Count` cardinality noun (CC-9); `Save -> i1`/`Load` trap
(CC-4); `*Anim*` → `Animation` (CC-13); `HitTestScaled -> Map` (typed
result object instead). Candidate for the sub-object decomposition
(`Tilemap.Layers`, `Tilemap.AutoTile`, `Tilemap.Animation`).

**Misleading importer names** — **(P1)** `TiledMapLoader` has no load
method (it configures tile size and creates empty maps — actual TMX
loading is `Tilemap.Load`); `AsepriteImporter` has no import method
(`SetGrid` + `ToAtlas`). Rename to what they do or move the load/import
functions in.

**(P2) assorted:**
- `Gradient2D.Sample(f64 t)` vs `SamplePct(i64)` — dual parameter scales;
  `FillHorizontal/FillVertical` vs `Canvas.GradientH/GradientV` — full
  words vs abbreviations for the same concept in one namespace.
- `Palette2D.ApplyLegacy` — "Legacy" in a method name with no explanation.
- `Color`: `GetR/G/B/A/H/S/L` single-letter getters → `Red()`… (or
  properties); `RGB/RGBA/FromHSL` → CC-12; `Lerp(a,b,t: i64)` — integer
  t where `Math.Lerp` takes f64 0–1.
- Overlap-test verb split: `Sprite.Overlaps` / `CollisionMask2D.Overlaps`
  vs `Hitbox2D.Intersects` — pick one (platform majority: `Overlaps`).
- `SpriteBatch.DrawEx`/`DrawAtlasEx` — the `Ex` suffix again (with
  `Sound.PlayEx/PlayEx2`); replace with arity overloads.
- `Emitter2D.Draw/DrawAt/DrawToPixels -> i64` undocumented returns;
  `SetLifetime(i64,i64)` units unmarked.
- `Renderer2D.End(target)`/`FlushToTarget(target)` overlap; `Begin()`
  argless vs `End(obj)` — document the retained-stream contract.
- `Graphics2D.SceneNode.SetScale(i64)` uniform-only beside separate
  `ScaleX/ScaleY` props; `Find` [legacy sentinel] pairs → CC-18.

Clean: `AnimationClip2D` (`FrameDelayMs` — one of the few unit-marked
names in 2D), `Hitbox2D`, `TileLayer2D`, `TileSet2D`, `TileChunkCache2D`,
`CollisionMask2D` (`Overlaps` 5-param shape aside), `AutoTile2D`,
`DebugDraw2D`, `SdfFont`, `RenderGraph2D`/`RenderPass2D` (clean pass
pipeline), `SpriteRenderer2D`, `Sampler2D`.

### 2.16 GUI (both halves; all preview)

**The namespace-level shape gap** — GUI is method-only where the platform
is property-based: 129 zero-arg `Get*` readers (CC-15), 100+
`Set*`-without-property members (S4 data), and near-zero writable
properties. The single highest-leverage GUI change before freeze is
property-izing state (`Checkbox.Checked`, `App.Width`, `Widget.Visible`),
which dissolves most per-class notes below.

**Polled `Was*` event convention** — **(P1)** GUI events are per-frame edge
polls: `WasClicked`, `WasSelectionChanged`, `WasChanged`,
`WasCloseClicked`, `WasInvoked`, `WasAccepted`, `WasDismissed`,
`WasDropReceived`, `WasFileDropped`, `WasTriggered`. The convention is
real and workable but undocumented: consumption semantics differ silently
(most flags auto-clear on read; `TreeView` drag-drop needs explicit
`ClearDrop`; `OutputPane.TakeInput` consumes by name). Document the
convention (one read per frame, auto-clear) and make consumption uniform;
multi-field events (`GetDropSourceData` + `GetDropTargetData` +
`GetDropPosition`; `GetClickedIndex` + `GetClickedData`) should return
event objects per the class-shape decision.

**Boolean readers split `Is*` vs `Get*`** — **(P1)** `Command.IsEnabled/
IsChecked` but `CommandState.GetEnabled/GetChecked`; `Widget.IsVisible`
but `App.Get*` everywhere — adjacent classes disagree on the same
concept. With property-ization this disappears; otherwise standardize
`Is*`.

**`Poll` returns three different things** — **(P1)** `App.Poll() -> void`,
`Command.Poll() -> i1`, `CommandRegistry.Poll() -> str` (invoked command
id), `GUI.App.PollWait(i64) -> i1` — same verb, four contracts (add
`IO.Watcher.Poll -> i64` and `System.ProcessHandle.Poll -> i1`).
Platform-wide: `Poll` needs one meaning (drain/pump) with typed
`TakeEvent() -> Option<T>` for value-returning forms.

**Window management exists twice with different shapes** — **(P1)**
`GUI.App` vs `Graphics.Canvas`: `App.SetFullscreen(i1)/IsFullscreen()` vs
`Canvas.Fullscreen()/Windowed()`; both carry `Maximize/Minimize/Restore/
Focus/SetTitle/SetPosition/GetMonitor*`; `App` also has **both**
`SetSize(i64,i64)` and `SetWindowSize(i64,i64)`. One window-surface
vocabulary, one size setter.

**Coordinate/size types are mixed** — **(P1)** `Widget.SetSize(i64,i64)`
vs `SetPreferredSize(f64,f64)`/`SetMaxSize(f64,f64)`/`SetFlex(f64)` on the
*same class*; `Widget.SetPosition(i64,i64)` vs
`FloatingPanel.SetPosition(f64,f64)`/`PopupList.AnchorAt(f64,f64)`.
Logical-vs-physical pixels exist (`App.ToLogical/ToPhysical`) but no
naming rule says which methods take which. Decide (recommend f64 logical
everywhere) and mark exceptions.

**Two API generations inside one class** — **(P1)** `FileDialog`: blocking
statics (`Open`, `Save`, `SelectFolder`, plus the stringly
`PathListCount(str)`/`PathListGet(str, i)` decoder pair for multi-select
encoded in one string) *and* an instance builder (`NewOpen`/`NewSave`/
`SetFilter`/`Show`/`GetPathAt`). Keep the builder; return `seq<str>`;
delete the `PathList*` string protocol. `MessageBox` has the same static
+ builder split (acceptable if chartered) with magic `i64` button codes —
add a `DialogResult` domain.

**`VideoWidget` re-declares the Widget base surface** — **(P1)**
`SetVisible/SetEnabled/SetSize/SetPreferredSize/SetMaxSize/SetFlex/
SetMargin/SetPosition/AddChild` are re-published locally (the 2026-07-01
GUI dedup missed it). It also mirrors `Graphics.VideoPlayer`
(Play/Pause/Stop/Position/Duration) but lacks `Seek` — align the media
surface.

**(P2) assorted:**
- `GUI.Grid` is a *data table* (SetCell/SetHeader), not a layout —
  rename (`DataGrid`); `HBox`/`VBox` re-declare `Container`'s
  `SetSpacing/SetPadding`.
- `TabBar.PruneRetiredTabs`/`TreeView.PruneRetiredNodes` — internal
  lifecycle jargon ("retired"?) in public API; `TabBar.GetTabIndexAt(x,y)`
  is a hit test named like an index lookup (→ `HitTest`).
- `RadioButton.IsSelected/SetSelected` vs `Checkbox.IsChecked/SetChecked`
  — selected/checked split in the toggle family.
- `GUI.ClipboardText` duplicates `System.Clipboard` (`GetText/SetText/
  HasText` vs `Get/Set/HasText`) — one clipboard owner, one naming.
- `ProgressBar.ShowPercentage(i64)` and `Canvas.PreventClose(i64)` take
  ints for booleans.
- `VirtualTree.Expand(str) -> Map` vs `TreeView.Expand(obj) -> void` —
  same verb, different worlds; the virtual widgets return Map-shaped
  results (`VisibleRange -> Map`) — type them.
- `Toolbar.AddButton/AddButtonWithText/AddNamedButton/
  AddNamedButtonWithText` — a 2×2 matrix whose `Named` axis (icon
  registry vs path?) is opaque.
- `FileDialog.Show/MessageBox.Show -> i64`, `Image.LoadFile -> i64`,
  `GUI.Font.Load` traps — CC-4; `Cursor.Set(i64)`, `SplitPane.New(obj,
  i64)`, `Button.SetStyle(i64)` — magic ints needing domains.
- `Breadcrumb.SetItems(str)`/`SetPath(str,str)` — separator-encoded string
  lists → `seq<str>`.
- `GUI.CodeEditor` (100 members) — already decided for sub-object
  decomposition (`07-class-shape`); its 13-flag `Set*` matrix and 22
  `Get*` readers are the flagship case for both CC-15 and the
  decomposition.

Clean: `EditorBuffer`, `FindBar` (flag-matrix aside, its
`FindNext`/`FindNextOption` migration is textbook), `ContextMenu`,
`MenuBar`, `StatusBar` (zone+items dual model is fine), `OutputPane`
(`TakeInput` is the right consume verb — reuse it), `Accessibility`,
`TestHarness` (tooling), `Theme`, `Shortcuts`, `Toast` (global statics on
an instance class need a charter note), `Tooltip`, `GroupBox`,
`FloatingPanel`, `PopupList`, `VirtualList`.

### 2.17 Game + Game2D

This namespace contains the platform's best migration examples — and the
patterns that show what "done" looks like: `Pathfinder.FindPathResult ->
PathResult`, `Quadtree.QueryRectResult -> QueryResult`,
`AnimStateMachine.PollEvents -> AnimationEventBatch`,
`Game.UI.Table.HandleClickResult -> TableClickResult` (with
`RowOption`/`ColumnOption`). The findings below are mostly about the
classes that haven't caught up to their own namespace's standard.

**A third meaning for the `*Result` suffix** — **(P1)** `FindPathResult`
returns `Game.PathResult` (a typed result object), while `Pty.OpenResult`
returns `Zanna.Result` and `ProcessHandle.ReadStdoutResult` returns a
`Collections.Map`. Three shapes, one suffix (see 2.2). Platform rule
needed: `*Result` ⇒ `Zanna.Result<T>`; typed query objects use plain verbs
(`FindPath -> PathResult` once the legacy sentinel form is deleted — the
suffix becomes unnecessary).

**Static `Destroy(obj)` convention** — **(P1)** ~14 Game classes teardown
via *static* `Destroy(obj)` (`AchievementTracker`, `ButtonGroup`,
`CollisionRect`, `DebugOverlay`, `Grid2D`, `ObjectPool`, `PathFollower`,
`Quadtree`, `ScreenFX`, `SmoothValue`, `StateMachine`, `Timer`, `Tween`,
`Typewriter`) while GUI/Graphics use instance `Destroy()` — two shapes for
the platform's most common lifecycle call (CC-6 extension).

**`Game.Timer` dual clocks** — **(P1)** `Start/Update/Elapsed/Remaining`
(frame-based — nothing in the names says frames) beside
`StartMs/UpdateMs/ElapsedMs/RemainingMs` (milliseconds). The unmarked
family reads as time and is not. Suffix both (`*Frames`/`*Ms`) or make
seconds the unmarked default per the CC-10 policy.

**`Physics2D.CircleBody`** — **(P1)** a full duplicate of `Body`'s surface
(same 4 methods, 14 of 16 props) existing only as a differently-shaped
constructor — `Body` already has `IsCircle` and `Radius`. Replace with
`Body.NewCircle(x, y, radius, mass)`. Also `SetPos`/`SetVel` → `SetPosition`
/`SetVelocity` (CC-13; the 3D bodies already spell them out), and
`World.ContactBodyA/ContactNX/ContactNY/ContactDepth(index)` is the
indexed-cursor event shape (with cryptic `NX`/`NY`) where Graphics3D
already returns `ContactPoint3D` value objects — mirror the 3D shape.

**`Game.UI` prefix inconsistency** — **(P1)** half the namespace is
`Hud`-prefixed (`HudDropdown`, `HudLabel`, `HudSlider`, `HudTextInput`,
`HudTooltip`), half is not (`Bar`, `Modal`, `MenuList`, `Panel`, `Table`,
`NineSlice`), and one is `Game`-prefixed *inside* `Game.UI`
(`GameButton`). Drop all prefixes (the namespace scopes them) or prefix
all; `GameButton` → `Button` (collision with `GUI.Button` is what
namespaces are for). `Game.UI.NineSlice` also collides conceptually with
`Graphics.NineSlice2D` (two nine-slice implementations).

**`Handle*` input family returns drift** — **(P1)** `HudSlider.HandleKey
-> i1` (handled?) vs `Modal.HandleKey -> i64` (result code) vs
`Table.HandleKey -> void`; `MenuList.HandleInput(i1,i1,i1)` takes three
positional booleans. One contract for `Handle*` (recommend `i1` handled +
polled result objects for outcomes, per `TableClickResult`).

**`Game.Quests`** — **(P1)** two generations inside one class: the
indexed event cursor (`EventCount`/`EventQuestId(i)`/`EventKind(i)`/
`ClearEvents`) predates its sibling `AnimStateMachine.PollEvents` batch
shape; and displaying one quest takes seven per-field calls
(`QuestTitle`, `QuestState`, `CurrentStageText`, `ObjectiveText`,
`ObjectiveProgress`, `ObjectiveTarget`, `ObjectiveComplete`) — needs
`QuestInfo`/`ObjectiveInfo` objects and `PollEvents`.

**`Game2D.SceneDocument` (72 members)** — the `Load`/`LoadResult` quad is
the model migration; remaining: **(P2)** yet another typed-accessor
vocabulary (`ObjectGetInt/Str/Float/Bool` — CC-11's eighth spelling set),
`LastError` [legacy side-channel], `Save -> i1` (CC-4), `Diagnostics` +
`DiagnosticRecords` near-dup pair, ~20 per-field object accessors
(`ObjectType/ObjectId/ObjectX/ObjectY(id)`) that want an `ObjectRef`
value object.

**(P2) assorted:**
- `Tween.Start(from, to, duration, easing-i64)` — easing as magic int
  while `Math.Easing` functions exist; add an `Easing` domain or take the
  function. `ValueI64` twin props here and on `SmoothValue` (CC-11);
  `SmoothValue.AtTarget` → `IsAtTarget` (CC-7).
- `ScreenFX`: `Shake/Flash/FadeIn/Wipe/CircleIn(i64…)` opaque
  positional params + magic FX type ints (`CancelType(i64)`) (CC-20);
  `TransitionProgress` scale undocumented.
- `Lighting2D.SetPlayerLight` — player-specific concept in a general
  lighting class; `AddLight(5×i64)` opaque (CC-20).
- `ObjectPool.Acquire() -> i64` untyped id handles (the Sound voice-id
  pattern); `FirstActive/NextActive` manual iteration cursor.
- `PathResult.Steps` + `StepCount` + [legacy] `Length` — three
  cardinality props for one list (keep `StepCount` or `Count`).
- `Dialogue.SetBgColor/SetPos` (CC-13); `Speed: i64` unit unmarked
  (chars/sec?) (CC-10).
- `Game.UI.Bar.SetValue(value, max)` — two-arg `SetValue`;
  `Panel.SetColor(color, alpha)` — the name says one thing, takes two.
- `HudDropdown.Open()`/`Modal.Open()` marked `!trap` — a UI open that
  traps needs its trap condition documented (CC-5).
- `WorldToScreenProjection` — per-axis statics with trailing `i1` flips
  (CC-20); consider point-in/point-out.
- `AnimStateMachine.StateName -> obj` (should be `str`);
  `AddState(i64,i64,i64,i64,i1)` opaque (CC-20).
- `ButtonGroup.SelectionChanged` + `ClearChangedFlag`, `StateMachine.
  ClearFlags` — manual flag consumption vs GUI's auto-clear `Was*` —
  fold into the platform polled-event convention (2.16).

Clean: `Collision` (f64 predicates — note the i64-vs-f64 2D divide with
sprites), `CollisionRect` (near-model rect value type; static `Destroy`
aside), `Grid2D`, `Typewriter`, `DebugOverlay`, `ParticleEmitter`
(identical shape to the Graphics duplicate pair — a *third* copy of the
same particle API; include in the 2.15 dedup), `ParticleSnapshot`,
`QuestState`/`QuestEventKind` (proper enum-like domains — the pattern the
magic ints above should use), `SpriteAnimation`, `AnimTimeline`
(`TrackPayloadA/B/C` generic payload slots are a stretch but workable).

### 2.18 Graphics3D

**Graphics-off returns null everywhere** — **(P0)** virtually every
object-returning member (including every `New`) is `nullable` — with
graphics disabled the whole 3D surface silently returns nulls rather than
the structured unavailability the capability decision (`06-capabilities`)
requires ("disabled capabilities must not silently look successful"). This
is most of the 402-nullable census. Decide the capability story (trap with
a clear message, or `Result` constructors) before freeze.

**Boolean-setting shape roulette on `Canvas3D`** — **(P1)** one class,
three shapes for the same kind of toggle: `Wireframe` (rw property),
`VSync` (ro property + `SetVSync` method), `SetBackfaceCull` (method only,
nothing readable). Same class: `SetDTMax` [legacy] + `SetMaxDeltaTime`.
Pick the writable-property shape (per `07-class-shape`) and apply it to
all ~24 `Set*` config methods (S4 data).

**`Canvas3D` needs the decided decomposition more than ever** — 173
members: 30+ backend/quality/streaming telemetry props (`FrameStats`
sub-object), a synthetic-input/clock test surface (`PushSyntheticKey`,
`AdvanceSyntheticFrame`, `SetSyntheticDeltaTimeSec`, `SetClockSource`,
`SetInputSource` — move behind a `Canvas3D.Testing` object), four
screenshot variants, a full 2D overlay-drawing vocabulary (`DrawRect2D`,
`DrawText2DAA` — a third 2D drawing dialect; see 2.15), and
`Begin/Begin2D/BeginOverlay/BeginViewModel` vs `End/EndOverlay/
FinalizeFrame/Flip` — an asymmetric frame lifecycle that needs its state
machine documented (P1).

**`Try*` that mutates** — **(P1)** `TrySetClusteredLighting`,
`TrySetRenderScale`, `TryCopyScreenshotTo`, `SceneGraph.TryAdd`,
`Physics3DWorld.TryAdd`, `Character3D.TrySetHeight` — `Try` + side effect
+ `i1` (the `LocaleManager.TryLoad` shape). Fold into the platform `Try*`
rule: these become `Result`-returning or documented `Set… -> i1
(applied)` without the `Try` prefix.

**Mixed acronym casing within single classes** — **(P1)** `SceneNode` has
`AddLOD`/`ClearLOD` *and* `GetLodMesh`/`SetLodResident`/`LodCount`;
`Material3D` has `SetAOMap` beside `HasAmbientOcclusionMap` (the same
concept, abbreviated in the setter and spelled out in the reader);
`PostFX3D` carries the explicitly-banned `AddSSAO/AddDOF/AddTAA/AddSSR/
AddFXAA/AddColorLUT` set. CC-12's sweep resolves all of these — listed
here because they are post-decision additions.

**Asymmetric mutability on transforms** — **(P1)** `SceneNode.Rotation` is
*writable* while `Position` and `Scale` are read-only (+ `SetPosition`/
`SetScale` methods); `Transform3D` repeats the same split. Make all three
writable properties (S4 correction) — a transform whose rotation assigns
but whose position doesn't is a daily paper cut.

**Event/collision consumption is three idioms** — **(P1)**
`Physics3DWorld` exposes indexed cursors (`GetCollisionDepth(i)`,
`ClearCollisionEvents`) plus count props; `Trigger3D` exposes cumulative
`EnterCount`/`ExitCount`; typed `CollisionEvent3D`/`ContactPoint3D`
classes exist for batch consumption. `AnimController3D.PollEvent() -> str`
returns one *string* per call where 2D's `AnimStateMachine.PollEvents()`
returns a typed batch — the newer namespace uses the older shape.
Standardize on typed `PollEvents` batches.

**Model-import stack is five classes deep** — **(P1)** `GLTF`, `FBX`,
`SceneAsset` (plus `Game3D.Assets3D` and `Game3D.Prefab`, 2.19) all load
models; `GLTF` lacks `GetAnimationName` that `FBX` has (both formats have
animations); class names violate the decided casing (`Gltf`/`Fbx`).
Charter: `SceneAsset` is the format-neutral API; format classes become
thin probes or fold in.

**`Skeleton3D.FindBone`** — **(P1)** [legacy] sentinel with **no**
canonical `Option` replacement — the only `Find` in the surface whose
migration has no landing zone. Add `FindBone -> Option` before deleting.

**(P2) assorted:**
- `Camera3D.ScreenToRay` + `ScreenToRayOrigin` — two calls to build one
  ray → return a `Ray3D` value object; `FPSInit`/`FPSUpdate(7×f64)`
  (CC-12/CC-20); `Shake/SmoothFollow/Orbit` param meanings and angle
  units undocumented (CC-10).
- `Physics3DWorld`: ~18 telemetry props mixed with config (sub-object);
  `StepFixed(f64,f64,i64)` opaque; `AddJoint(obj, i64)` vs 2D
  `AddJoint(obj)`.
- `SceneGraph.RaycastNodes -> SceneNode` — plural name, singular return;
  `Save -> i64` status (CC-4); `SyncBindings(f64)` internal-sounding
  (also on `SpatialAudio3D`).
- Color models split: `Material3D.SetColor(f64,f64,f64)` and
  `Water3D.SetColor(4×f64)` vs `Sprite3D.SetColor(i64 packed)` — 3D uses
  float components except where it doesn't.
- `Mesh3D.Simplify` returns new; `Transform`/`RecalcNormals` mutate —
  the `Pixels` mutation ambiguity again; `RecalcNormals`/`CalcTangents`
  → `Recalculate*/Calculate*`; `ReleaseCpuScratch -> i64` internal name.
- `Material3D.Clone` vs `MakeInstance` — undocumented difference;
  `SetShininess` (Phong) beside PBR `Roughness`; `SetCustomParam(i64,
  f64)` magic slots.
- `InstanceBatch3D.Add(obj) -> void` but `Remove(i64)`/`Set(i64, obj)` —
  add returns no index, so the index-based API is unusable without
  external bookkeeping → `Add -> i64`.
- `Terrain3D.Last*` telemetry prefix vs `Canvas3D`'s bare names — one
  telemetry naming style; `AssetDiagnostics3D.GetLoadWarnings() -> str`
  (plural returning joined string, beside singular `GetLoadWarning(i)`).
- `Ragdoll3D.SetJointLimits(boneName, …)` vs `SetPowered(boneIndex, …)` —
  mixed bone addressing; `MorphTarget3D.SetWeight(i)` +
  `SetWeightByName(str)` — the `Named` dual again.
- `Vehicle3D.SetInput(f64,f64,f64)` opaque triple; `Wheel*` per-field
  getters → wheel state object; `Sky3D.Dirty` internal state exposed.
- `TimeOfDay3D` is the unit-naming model citizen (`DayLengthSeconds`,
  `LatitudeDegrees`) — copy it everywhere; its `Advance(f64, obj)` first
  param unit is still unlabeled.

Clean: `AnimPlayer3D`, `Animation3D`, `Path3D` (correct `Length` use),
`RenderTarget3D` (`NewHdr`/`IsHdr` — correct acronym casing to copy),
`TextureAsset3D` (`Format: str` stringly aside), `Decal3D` (`Expired` →
CC-7), `CubeMap3D` (`New(6×obj)` face order — CC-20), `LensFlare3D`,
`Water3D`/`Vegetation3D` (Set-soup → S4 properties; `Update(4×f64)`
opaque), `Cloth3D`, `Sprite3D`, `NavAgent3D` (good property-based
config), `Trigger3D`, `BlendTree3D` (`SetParam` opaque).

### 2.19 Game3D

**`World3D` (104 members)** — the sub-object decomposition works here
(`.Physics`, `.Scene`, `.Audio`, `.Input`, `.Effects`, `.Stream`,
`.Camera`, `.Canvas` properties exist — the pattern to replicate on
`Canvas3D`/`Tilemap`/`CodeEditor`). Remaining findings:

- **(P1)** six-variant run matrix: `Run`, `RunFixed`, `RunFixedWithOverlay`,
  `RunFrames`, `RunFramesOnly`, `RunWithOverlay` — plus five constructors
  (`New`, `NewFullscreen`, `NewWithCamera`, `NewWithHorizontalCamera`,
  `NewFullscreenWithHorizontalCamera`). Both need options objects.
- **(P1)** `Tick` — the platform's only `Tick` beside 57 `Update`s
  (S1); rename or bless as the world-step term and document against
  `StepSimulation`/`BeginFrame`/`Present`.
- **(P1)** three indexed event-cursor families as *methods*
  (`CollisionEvent(i)`/`CollisionEventCount()`, `HitEvent(i)`,
  `DamageEvent(i)` + `Clear*Events`) while typed event classes exist —
  the flagship for the platform event-idiom correction.
- **(P2)** `Dt`/`UnscaledDt` props vs `Canvas3D.DeltaTime` — same concept,
  abbreviated here (CC-13); `Hitch*` telemetry mixed in (belongs in
  `Diagnostics3D`, which is the clean telemetry model); `OnResize` — an
  `On*` name for an imperative notification method; `ReportSound` opaque;
  `LoadState/SaveState -> i1` (CC-4).

**`Animator3D` vs `AnimController3D`** — **(P1)** the Game3D wrapper
mirrors the Graphics3D controller but diverges: `SetSpeed(state, f64)` vs
`SetStateSpeed(state, f64)` (same op, different names), `PlayLayerAdditive`
without `PlayLayer` (the controller has both), `FindBone -> i64` as a
*live* sentinel (not even marked legacy — `Skeleton3D`'s is), and an
indexed `EventCount`/`EventName(i)` cursor vs the controller's
`PollEvent -> str`. Mirror pairs must be generated or audited against each
other.

**Two edge-flag conventions** — **(P2)** `Just*` (`Health3D.JustDied/
JustDamaged`, `TargetLock3D.JustAcquired/JustLost`, `Character3D.
JustLanded`, `Timeline3D.JustFinished`, `Quests.JustCompleted`) vs
`*Changed` (`Perception3D.SeenChanged`, `Interactor3D.FocusChanged`,
`ButtonGroup.SelectionChanged`). Pick `Just*` (it already dominates) and
document it as the frame-edge convention with its consumption rule.

**Duplicate constant domains** — **(P1)** `Game3D.MouseButtons` vs
`Input.Mouse.Button*` properties vs the decided-but-unbuilt
`Input.MouseButton` domain; `Game3D.Keys` vs `Input.Key` (CC-2). Input
constants get exactly one home (`Input.*` domains, per decision).

**(P2) assorted:**
- `DamageEvent3D` is `class_kind: enum-like` but carries event payload
  props (`Amount`, `WasLethal`); `HitEvent3D` "static-module" likewise
  (CC-16).
- `ShadingModel.Pbr` (correct casing) vs `Material3D.PBR` factory (banned
  casing) — the same word both ways in one subsystem (CC-12).
- `Game3D.PostFX.None(world)` — a preset method named `None` reads as a
  null-check; `Cinematic/Crisp` presets are good — add `Preset` to the
  names or an enum.
- `Sound3D.Volume: i64` beside `RefDistance/ReverbWet: f64` — the 2D
  integer-volume scale leaking into the f64 3D API (and `RefDistance` →
  CC-13); spatial-audio ownership spans three namespaces (2.14).
- `CharacterController3D.Grounded()` method vs `Character3D.Grounded`
  property — same concept, different shapes in the two character classes;
  `SetCrouching(i1) -> i1` setter-returning-success.
- `Health3D.Damage(f64, obj, i64) -> f64` — opaque positional params and
  return (CC-20); `LastDamage`/`LastTag` ambient side-props;
  `InvulnSeconds` → `Invulnerability*` (CC-13); `Invulnerable`/`Active`/
  `Ready`/`Driving` bare booleans (CC-7).
- `Perception3D.HeardTag(i)` indexed cursor; `Minimap3D.MapX/MapY`
  per-axis pair; `LipSync3D.Drive(i64)` consumes untyped voice ids
  (2.14's finding); `SetBlink(i1,str,f64,f64)` opaque (CC-20).
- `Assets3D.Preload` vs `PreloadAsset` — undocumented distinction;
  `EnvHandle` (abbreviation, static-module "handle") folds into
  `Environment3D`.
- `SceneTemplate` duplicates `SceneAsset` probes (`GetSceneName`,
  `GetCameraCount` on both) — CC-19's terminology fix should merge them.

Clean: `Diagnostics3D` (the telemetry-module pattern to copy),
`BodyDef` (correct `Is*` booleans — the model for CC-7), the enum-like
domains (`AlphaMode`, `BodyShape`, `CollisionPhase`, `QualityLevel`,
`RenderPass`, `ShadingModel`, `Layers`, `HitboxKind`, `HitchSource`),
the controller family (`FirstPerson`/`FreeFly`/`Follow`/`Orbit`/
`ThirdPerson`/`RailCamera3D` — consistent `Update`/`LateUpdate`
convention worth documenting), `Interactable3D`, `AmbientBed3D`
(`CrossfadeSeconds` unit naming), `Footsteps3D`, `ReverbZone3D`,
`Behavior3D`, `BehaviorTree3D`/`BehaviorTreeInstance3D` (builder DSL),
`LayerMask` (`Includes` → CC-8), `Lighting` (preset pattern),
`Quality`.

## Part 3 — Consolidated Corrections

### 3.1 Delete (pre-alpha policy: remove, don't deprecate)

| Group | Scope | Detail |
|---|---:|---|
| `stability: legacy` rows | 266 functions (147 class methods, 114 properties) | The CC-1/CC-18 sweep; every group's canonical survivor already exists |
| Duplicate C-symbol publications | 176 symbols | CC-1 function-level + 58 same-class method pairs; keep the canonical name per CC-1/Part 2 |
| Whole-class mirrors | ~10 classes | `Input.Keyboard.Key*` (97 props), `Memory.GC`, `Memory.Retain/Release*`, `Zanna.Error.*` rows, `Crypto.Hash` weak-algo mirrors, `Core.Box.ValueType*`, `Core.ValueType` |
| Duplicate classes (2D) | 6 classes | `Emitter2D`+`ParticleSystem2D` (keep one; `Game.ParticleEmitter` is the same symbols again), `ScreenScaler`, `Surface2D`, `SpriteFont`, `Physics2D.CircleBody` (→ `Body.NewCircle`), `GUI.ClipboardText` (→ `System.Clipboard`) |
| Parallel implementations | 2 families | `rt_file_*` vs `rt_io_file_*` (`File.ReadBytes`/`ReadLines`/`WriteBytes`/`WriteLines`), `Dir.Dirs/Files/List` vs `*Seq` vs `Entries` (keep `All`-forms and `Entries`/`Files`/`Dirs`); `Url.Encode/Decode` vs `Codec.UrlEncode/UrlDecode` (keep Codec) |
| Sentinel twins of Option forms | ~30 methods | `TryPop`/`Find`/`FindWhere`/`TryRecv`/`TryGet`/`TryParse`/`FindNext`… [legacy] rows; then rename `XOption` → `X` |
| Redundant members | ~15 | `Option.Value`, `Result.OkValue/ErrValue`, `Set.Items` or `ToSeq`, `Random.Next`, `Mouse.Left/Right/Middle`, `Keyboard.Shift/Ctrl/Alt/CapsLock` (→ modifier query), `CancelToken.Check`, `String.FromStr/FromSingle`, `Environment` duplicated host facts, `Environment.Cwd` |

### 3.2 Rename (high-signal canonical winners)

Grouped by rule; the long tail is in Part 2 per-namespace notes.

| Current | Canonical | Rule |
|---|---|---|
| `Threads.Monitor.Pause/PauseAll` | `Notify/NotifyAll` | says-what-it-does (P0) |
| `Collections.Bag` | `StringSet` | says-what-it-is (P0) |
| `UnionFind.SetSize` | `ComponentSize` | getter must not read as setter (P0) |
| `Math.Deg/Rad` | `ToDegrees/ToRadians` | conversion direction (P0) |
| `Time.Clock.Ticks/TicksUs` | `NowMs/NowMicros` (monotonic) | units in names (P0) |
| `DateTime.ToLocal/ToZone` | `FormatLocal/(delete; FormatInZone exists)` | `To*` = conversion, not formatting (P0) |
| `Seq.Remove(i)` | `RemoveAt(i)` | verb parity with `List` (P0) |
| `String.Has` | delete (`Contains` wins) | CC-8 |
| `TextWrapper.Left/Right` | `AlignLeft/AlignRight` | collision with `String.Left/Right` |
| `Seq.Apply/Keep` | `Map/Filter` | one functional vocabulary |
| `Option.AndThen` / `Lazy.FlatMap` | one of the two, both classes | one monadic-bind name |
| `Queue.Push/Pop` or `ConcurrentQueue.Enqueue/Dequeue` | one pair on both | same type, same verbs |
| `BitSet.Set/Clear(i)` | `SetBit/ClearBit(i)` | `Clear()` always means empty |
| `Bits.Count` | `CountOnes` | `Count` is cardinality |
| `TreeMap` | `SortedMap` | behavior-named like `SortedSet` |
| `Mat3/Mat4.Eq(a,b,eps)` | `ApproxEquals` | epsilon ≠ equality |
| `Sound.PlayEx/PlayEx2`, `SpriteBatch.DrawEx` | arity overloads / options | no versioned `Ex` |
| `Environment.EndProgram` | `Exit` | industry term |
| `Crypto.Rand` | `Crypto.Random` | full words (pair with `Math.Random`) |
| `Crypto.Module` | `Crypto.Compliance` | says-what-it-is |
| `Hash.Fast*` | move out of `Crypto` / `NonCrypto*` | misuse-resistant placement |
| `SHA256/HmacSHA256/Pbkdf2SHA256`, `MD5`, `CRC32`, `GLTF`, `FBX`, `LoadKTX2`, `SetAOMap`, `AddSSAO/DOF/TAA/SSR/FXAA`, `AddLOD/LOD*`, `RGBA/RGB`, `RotateCW/CCW`, `IsRTL/IsLTR`, `LikeCI`, `MatchCIDR`, `FromOBJ/FromSTL` | word-cased per decision (`Sha256`, `Gltf`, `Lod`, `Rgba`, …) | CC-12 (decided) |
| `Cap`, `Len/LenSq`, `Norm`, `Dist`, `Pos`, `SetPos/SetVel`, `Cmp`, `Eq`, `Neg`, `Trunc`, `Sgn`, `Ceil`, `Diff` (sets), `Prev`, `Inc/Dec`, `Bg/Fg`, `Dir` (paths), `Abs/Ext/Sep/WithExt` (Path), `Base64Enc/Dec`, `HexEnc/Dec`, `WriteLn`, `Del*`, `Dt`, `InvulnSeconds`, `RefDistance`, `Gen` (Scheduler), `Vol`, `Attr` (keep? Xml domain), `Pass` (Url) | full words (`Capacity`, `Length/LengthSquared`, `Normalize`, `Distance`, `Position`, `Compare`, `Equals`, `Negate`, `Truncate`, `Sign`, `Ceiling`, `Difference`, `Previous`, `Increment/Decrement`, `Background/Foreground`, `Directory`, …) | CC-13 with explicit exception list (`Mul/Div/Sub/Add/Dot/Cross/Lerp/Slerp/Min/Max/Abs/Sqrt/Id`) |
| `Text.{Json,JsonStream,JsonPath,Csv,Toml,Ini}` | `Zanna.Data.*` | CC-3 (decided) |
| `Zanna.Sound.*` | `Zanna.Audio.*` (`Sound.Sound` → `Audio.Sound`, `Sound.Audio` → `Audio.Mixer`) | decided namespace |
| `Game.UI.Hud*`, `Game.UI.GameButton` | drop prefixes (`Game.UI.Label`, `Game.UI.Button`) | namespace scopes |
| `Game3D.Collision3DEvent` | `CollisionEvent3D` | one suffix pattern (CC-21) |
| `Physics3DBody/Physics3DWorld` | `PhysicsBody3D/PhysicsWorld3D` | suffix, not infix (CC-21) |
| `Graphics.*2D` rendering classes | `Graphics2D.*` | CC-22 (decided ownership) |
| `Core.Diagnostics` | `Core.Assert` | three Diagnostics entities (CC-23) |
| `Scanner.ReadInt/ReadNumber` | `ReadIntToken/ReadNumberToken` | returns str, not numbers |
| `TiledMapLoader`, `AsepriteImporter` | rename or move the load in | names promise what they don't do |
| bare boolean props (`Static`, `Kinematic`, `Trigger`, `Active`, `Playing`, `Enabled`, `Finished`, `Paused`, `Emitting`, `Expired`, `Ready`, `Grounded`, `Alive`, `Succeeded`, `Pending`, `Driving`, `Invulnerable`) | `Is*` forms | CC-7 |

### 3.3 Shape changes

1. **Writable properties** replace the 37 ro-prop+`SetX` pairs and the GUI
   `Get*/Set*` method surface (129 zero-arg `Get*`, 343 `Set*`-without-
   property). One sweep, guided by S4 data; `Canvas3D`'s three-shape toggle
   roulette is the acceptance test.
2. **Failure model adoption** (CC-4), in order: (a) delete/replace the 13
   side-channels; (b) `i1`-returning `Save/Load/Export/Submit/Kill` →
   `Result`; (c) 402 nullable returns → `Option` where absence is normal;
   (d) trap-only `Parse/Load/Open/Find` gain `Result`/`Option` canonical
   forms (`Json/Csv/Toml/Ini/Html/Version/Manifest/TimeZone.Find`,
   `Pixels.Load*`, `Assets.Load`).
3. **Event objects** replace indexed cursors and ambient last-thing state:
   `World3D` (3 families), `Quests`, `Physics2D.World` contacts,
   `Physics3DWorld`, `IO.Watcher`, `Udp` sender props, `SseClient` `Last*`,
   `AnimController3D.PollEvent`, `Animator3D`, `Perception3D.HeardTag`,
   GUI multi-field `GetClicked*`/`GetDrop*`. Pattern already shipped:
   `AnimationEventBatch`, `TableClickResult`.
4. **Options objects / struct params** for the worst call shapes (CC-20):
   `PlatformerController.Update`, `Mesh3D.SetBoneWeights`,
   `SceneNode.SetTransform`, `Vehicle3D.AddWheel`, `Tls.ConnectOptions`,
   `World3D.Run*`/`New*` matrices, `WebSocket.Connect*`,
   `FileDialog`/`MessageBox` statics.
5. **Value objects for per-field getter families**: `Rect` (TextureAtlas,
   ObjectLayer2D, CollisionRect exists), `Ray3D` (Camera3D),
   `Datagram` (Udp), wheel state (Vehicle3D), `QuestInfo`, `TrapInfo` as
   value (not global reader).
6. **Declared metadata**: `class_kind` and `fallibility` become declared
   in `runtime.def` (CC-5/CC-16); `contract_source: inferred` is banned for
   stable rows.

### 3.4 Policy lines to add (one paragraph each ends the drift)

1. Teardown: `Close` for opened resources, `Destroy` for created scene/UI
   objects, instance method always; GC handles everything else (CC-6).
2. `Try*` returns `Option` (or `i1` for pure, side-effect-free probes) —
   never a success flag for a mutation (Localization/Canvas3D/SceneGraph).
3. `*For(…, timeoutMs)` is the timeout suffix; timeout-miss returns
   `Option`, not null (Threads).
4. `*Result` returns `Zanna.Result` — nothing else (System vs Game).
5. Factory styles: bare noun = variant, `From*` = conversion, `With*` =
   configured constructor, `New` = allocation; `Create` unused (CC-14).
6. Time: f64 seconds unmarked for gameplay; integer ms always `Ms`-suffixed;
   `Millis`/`Sec`/`Secs` spellings banned; angle units documented per
   member (CC-10; `TimeOfDay3D`/`Stopwatch` are the models).
7. Typed-variant suffixes: one language vocabulary (`Integer/Number/
   Boolean/String` or the IL names) — today there are eight spelling sets
   (CC-11); IL names only where the type is the wire format.
8. Booleans: `Is/Has/Can/Was` prefixes on properties and methods (CC-7);
   `Just*` is the frame-edge prefix; polled `Was*` flags auto-clear on
   read (document the GUI/Game convention).
9. `Update(dt)` is the per-frame verb (57 classes); `Tick`/`Step` reserved
   for world/physics stepping and documented as such; `Update`/`LateUpdate`
   controller convention documented.
10. Telemetry lives in diagnostics sub-objects/modules (`Diagnostics3D`
    pattern), not mixed into config surfaces; one naming style (no `Last*`
    vs bare split).
11. `Poll` pumps; `TakeEvent`/`PollEvents` return typed values;
    per-index event cursors banned.
12. Capability-off behavior: structured unavailability (`Result` or trap
    with code) — never silent null constructors (Graphics3D).
13. Enum-like domains for every magic `i64` mode/style/kind parameter;
    stringly enums (`RoundingMode: str`) migrate to domains.
14. Handle-returning ids are typed (voice ids, pool ids) or documented as
    opaque tokens with their consumer named.
15. Exception list for domain-standard short names (`Mul`, `Lerp`, `Sqrt`,
    `Http`, `Fov`, …) — additions require the list, not ad-hoc judgment.

### 3.5 Audit gates (mechanical, in `rtgen`)

1. One public name per `c_symbol` (function- and method-level) unless
   tagged intentional.
2. No `stability: legacy` rows (post-sweep).
3. No `fallibility: infallible` on `Result`/`Option` returns or IO-verb
   names without explicit override.
4. Declared `class_kind` matches shape (no static-module with instance
   methods; no enum-like with payload props).
5. Boolean-returning members carry an approved prefix; boolean properties
   are `Is/Has/Can*`.
6. `Timeout|Duration|Delay|Elapsed|Interval` members carry a unit suffix
   or a unit doc line.
7. No new zero-arg instance `Get*` returning plain values; no `Set*`
   method where a writable property fits.
8. `Try*` returns `Option`/`i1`-probe; `*Result` returns `Zanna.Result`.
9. No bare `obj` returns where the concrete type is known (CC-17
   ratchet: count must not increase).
10. Class summaries must not match the boilerplate template; acronym-initial
    summaries render correctly (the "cSV" casing bug).

## Appendix — Method

- Ground truth: `./build/src/tools/zanna/zanna --dump-runtime-api` from this
  checkout (v0.3.0-snapshot; 514 classes / 7,384 functions / 5,272 methods
  / 1,828 properties), saved and analyzed with ten sweep scripts
  (vocabulary matrix, lifecycle symmetry, boolean naming, accessor/property
  duplication, abbreviation/acronym scan, unit conventions, parameter
  shapes, metadata consistency, namespace overlap + duplicate publication,
  misc census). Scripts and raw outputs are preserved in the session
  scratchpad (`api_review/sweeps.py`, `s1`–`s10` outputs, `classdump.py`).
- Every class was read member-by-member from the dump (14 batches,
  foundation → largest namespace), judged against the decided conventions
  in `misc/plans/runtime_overhaul2/` and against sibling classes.
- Claims about behavior (not just names) were verified in source where
  load-bearing: `rt_option_value` (null-on-None), `rt_monitor_pause`
  (signal/notify), `rt_aes_encrypt_str` (GCM "VAG1" format, null-sentinel
  inputs, legacy-CBC decrypt acceptance), duplicate-symbol claims via
  `c_symbol` comparison, `String.Contains` ≡ `Has` via method targets.
- Excluded: the C ABI naming layer (`rt_*` symbols — internal per
  `runtime.def`), doc prose quality beyond catalog summaries, and behavior
  testing (no code was built or changed; the `zanna` binary was only read).
- Counts quoted in findings are recomputable from the saved dump with the
  sweep scripts; severity tags total 25 P0, 124 P1, 106 P2 sites (several
  tags cover multi-member groups).
