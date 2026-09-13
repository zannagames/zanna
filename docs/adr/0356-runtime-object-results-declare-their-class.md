---
status: accepted
audience: contributors
last-verified: 2026-09-13
---

# ADR 0356: Runtime Object Results Declare Their Class

## Status

Accepted. Extends the typed-return rows introduced with `obj<Class>` and `seq<T>` and replaces
the frontend inference that filled in undeclared object results.

## Context

A runtime row spells an object result as `obj`, `obj<Class>`, `seq<T>`, or `list<T>`. Before
this decision, about 1,050 functions returned a bare `obj`, and both frontends guessed the class.

- **Zia** (`Sema_Runtime.cpp`) typed a class method's bare `obj` as the owning class unless the
  name looked like an accessor (`Get*`, `Keys`, `Pop`, `Peek`, `First`, `Find`, and so on). A
  qualified function became the owning class when its prefix named a class or its name began
  with `New`, `Load`, `From`, `Parse`, `Open`, `Read`, `Decode`, or `Create`. A 30-entry override
  table corrected a few rows, but the class-method path overwrote it.
- **BASIC** (`Lowerer.cpp`, the OOP lowering helpers, and the semantic analyzer) used a
  cross-class target prefix, the owning class for any class without `Push`, `Set`, or `Enqueue`,
  and the owning class for any `New`.

The guess was right for constructors, fluent setters, and value arithmetic. It was wrong for
roughly 200 functions, for example:

- `Option.Unwrap` and `Result.Unwrap` were typed as `Option` and `Result`, so
  `opt.Unwrap() as String` failed with "Cannot cast 'Zanna.Option' to 'String'".
- `Menu.AddItem` returns a `MenuItem`, `TreeView.AddNode` a `TreeView.Node`, `TabBar.AddTab` a
  `Tab`; the toolbar and status bar builders return their item classes.
- Decryption and key derivation, `Tls.Recv`, `Tcp.Recv`, `Http.PostBytes`, `Compress.*`, and
  `Stream.Read*` return `Bytes`, not their static class.
- `Quat.RotateVec3`, `Mat4.TransformPoint`, `Collision3DEvent.Point`, and `Camera3D.ScreenToRay`
  return `Vec3`; `Game3D.Materials.*` return `Material3D`; `LocaleManager.Current` returns a
  `Locale`; `Channel.Recv` returns whatever was sent.

Two related defects surfaced during the review:

- `AnimStateMachine.StateName` returned a runtime string but was declared `obj`.
- `Zanna.Game.Entity`, `Behavior`, `Config`, `SceneManager`, and `Zanna.Game2D.LevelDocument`
  existed only as function namespaces. Zia kept a special rule that accepted such a namespace as
  a type name, and their constructors' results depended on the guess.

## Decision

### The row is the only source of a result's class

- `obj<Class>` makes the result that class, and `seq<T>` or `list<T>` a typed container.
- A bare `obj` result has no class: Zia types it `Any`, which needs `as` before use, and BASIC
  treats it as an untyped object.
- Neither frontend infers a class from the owning class, the method name, or the target
  function's namespace. The override table and every heuristic are removed.

### Every object result is declared

Each of the 1,047 affected functions was reviewed against its C implementation: what it
allocates, which constructor or typed function it returns, or which field it hands back. The
rows now declare:

- the concrete class (`obj<Zanna.Collections.Bytes>`, `obj<Zanna.GUI.MenuItem>`, ...);
- `seq<str>`, `seq<i64>`, or `seq<obj>` for sequences, because Zia iterates those and does not
  iterate a value typed `obj<Zanna.Collections.Seq>`;
- `obj<Zanna.Core.Object>` when the result is always a runtime object whose class depends on the
  value: XML nodes, message-bus callbacks, list-box items, material texture slots, terrain layer
  textures, physics joints, and the bitmap or sprite font behind an `SdfFont`;
- a bare `obj` only when the result can be any value, including a string or a boxed scalar:
  stored collection, channel, future, and lazy elements; `Option` and `Result` payloads; parsed
  JSON, YAML, and TOML documents; and the `Box` constructors.

`AnimStateMachine.StateName` becomes `str`, and its C function returns `rt_string`.

### Untyped results are an explicit list

`RUNTIME_SURFACE_UNTYPED_OBJECT_RESULT(canonical)` in `src/il/runtime/RuntimeSurfacePolicy.inc`
lists the 84 functions whose result stays a bare `obj`. `RuntimeSurfaceAudit.ObjectResultsDeclareTheirClass`
fails when:

- a function, a class method, or a readable property returns a bare `obj` and its function is
  not listed (methods and getters use the function they target);
- an `obj<Class>` result names a class that is not in the runtime catalog;
- a listed function no longer returns a bare `obj`.

### `Zanna.Core.Object` is the root class

Zia lets any runtime object flow into a `Zanna.Core.Object` slot, but a `Zanna.Core.Object`
value never flows into a concrete class implicitly; `as` narrows it, exactly like `Any`. Other
runtime classes keep their existing assignment rules because Zia does not yet model the runtime
class hierarchy.

### Every row is searchable by name

`RuntimeRegistry::findFunction` returns the declared signature of every public runtime row,
case-insensitively, rather than only catalog methods and properties. Constructors
(`Zanna.Text.CompiledPattern.New`), free functions, and property getters therefore report the
class they return. That is how a frontend learns that `pattern = Zanna.Text.CompiledPattern.New(...)`
holds a `CompiledPattern`, now that the name no longer implies it.

### Function-only namespaces become classes

`Zanna.Game.Entity`, `Zanna.Game.Behavior`, `Zanna.Game.Config`, `Zanna.Game.SceneManager`, and
`Zanna.Game2D.LevelDocument` gain `RT_CLASS` blocks over their existing functions, with
properties for their `get_`/`set_` pairs. Zia's rule that accepted a function namespace as a type
name is removed; only catalog classes are runtime types.

## Consequences

- Results have their real class, so their real members are available and wrong casts that Zia
  used to reject compile.
- Code that stored an untyped result in a typed variable, or passed it to a typed parameter,
  now narrows it with `as`. Code that relied on a guessed class was relying on a type the value
  never had. The repository's callers are updated.
- The generated runtime reference shows the declared classes, and the five new classes appear
  with their properties and methods.
- Adding a runtime function that returns an object means declaring its class, or listing it as
  untyped with a reason.
- Zia still allows assignment between unrelated runtime classes. Modeling the runtime class
  hierarchy (the catalog's optional base class) and checking those assignments is future work.

## Alternatives Considered

- **Fix only the wrong rows and keep inference.** An intentionally untyped row still needed its
  own spelling, the frontends would keep two sources of truth, and every new row would inherit a
  guess.
- **Extend the name heuristics.** Names do not determine classes: `AddItem` returns an item on a
  menu and nothing on a dropdown, and `Recv` returns `Bytes` on a socket but any value on a
  channel.
- **Type every untyped result as `Zanna.Core.Object`.** Collection elements, payloads, and parsed
  documents can be strings or boxed scalars, which `Zanna.Core.Object` cannot narrow to.
