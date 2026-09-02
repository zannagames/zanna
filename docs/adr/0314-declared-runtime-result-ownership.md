# ADR 0314: Declared result ownership for runtime functions

- Status: Accepted
- Date: 2026-09-01
- Scope: runtime definitions (`src/il/runtime/defs/**/*.def`), `rtgen`, runtime signature registry, Zia lowerer, runtime API manifest, a handful of C runtime functions
- Related: ADR 0313 (class destructors), plan 88 (probe memory)

## Context

After ADR 0313 made Zia objects release their fields, Legacy Baseball's shot
walk still retained about 110 MB per stage: every `Mesh3D.Box`,
`Material3D.PBR`, `World3D.WithCamera`, scene load, animation retarget and
screenshot the game created survived its owner. The lowered IL showed why: the
compiler retained the result of `Mesh3D.Box` on store and released it once,
while it transferred the result of `Entity3D.New` without a retain.

The Zia lowerer decides whether a runtime call's object result is owned (one
reference the caller must release) or borrowed (a view the caller must not
release) from `RuntimeOwnership.hpp`, a hand-maintained catalog of name
patterns: results are owned when the C symbol ends in `_new` or `_clone`, when
the Zanna name ends in `.New` or `.Clone` or contains `.From`, or when the
function is listed explicitly. Everything else is borrowed. Measured against the
C sources, 1048 of the 1758 object- and sequence-returning runtime functions
were treated as borrowed, and 502 of those return a reference the caller owns.
Those leaked on every call. The opposite error existed too: `.From` matched
`Entity3D.DetachFromBone`, which returns its receiver, so the compiler released
a reference it never owned.

String results had the mirror problem. Every `str` result was treated as
owned, but `Result.UnwrapStr`, `Option.UnwrapStr`, `Lazy.GetStr` and the clip
name accessors return a string another runtime object still owns. That double
release was masked while the owning `Result` leaked; with ADR 0313 the
`Result` dies and the string is freed under its user (Zanna Studio's regex
search and its 3D scene editor both failed this way).

## Decision

### Ownership is declared on the definition

Every `RT_FUNC` / `RT_INTERNAL_FUNC` row whose signature returns a managed
reference (`obj`, `obj<…>`, `seq<…>`, `str`) carries a trailing token:

```
RT_FUNC(Mesh3DBox, rt_mesh3d_new_box, "Zanna.Graphics3D.Mesh3D.Box", "obj(f64,f64,f64)", owned)
RT_FUNC(SceneAssetGetMesh, rt_model3d_get_mesh, "Zanna.Graphics3D.SceneAsset.GetMesh", "obj<Zanna.Graphics3D.Mesh3D>(obj,i64)", borrowed)
RT_FUNC(ResultUnwrapStr, rt_result_unwrap_str, "Zanna.Result.UnwrapStr", "str(obj)", borrowed)
```

`owned` means the callee hands the caller one reference; `borrowed` means the
result is a view the caller must never release. The token may precede or
follow the existing `always` lowering token.

`rtgen` refuses a reference-returning row without the token, and a token on a
row that returns no reference, so a new runtime function cannot be added
without stating its contract. The declaration is emitted into the generated
descriptor table (`DescriptorRow::resultOwnership`,
`RuntimeSignature::resultOwnership`) and is authoritative: it overrides the
name-pattern catalog in both directions. The catalog stays for argument
consumption masks and for optimizer facts about C symbols.

### Consumers

- **Zia lowerer**: an object or sequence result is scheduled for release only
  when declared `owned`; a string result is *not* scheduled for release when
  declared `borrowed` (the slot or field it lands in takes its own retain).
- **Runtime API manifest** (`zanna --dump-runtime-api`): the `ownership`
  field reports the declaration (`owned` / `borrowed`) and only falls back to
  the old heuristics for rows without a reference result.
- **Runtime surface audit** and generated docs are unchanged in shape; the
  def rows gained one token.

### Truth for the initial migration

The 2432 rows were decided from the C sources, not from names:

1. A per-function classifier resolved every `return` expression through the
   helper call graph (allocation, retain-before-return, catalog-known owned
   callees, parameter passthrough, field returns) and read the `@return`
   doc comment.
2. Rows where those disagreed, or where the classifier could not decide
   (about 175 object rows and 30 string rows), were read by hand.
3. The result was verified empirically: the compiler-effective decision was
   recomputed from the catalog for every function, and each family that the
   test suite or Zanna Studio exercises was bisected until the probes held.

Doc comments were not trusted blindly: the `Entity3D.get_Mesh` family
documents "the validated retained pointer" and returns the entity's retained
slot without a retain of its own. The definitions say `borrowed`.

### Runtime functions made uniform

Some functions returned a fresh object on one path and a borrowed one on
another; a declaration cannot describe that, so they now return one
caller-owned reference on every path:

- `Option.Map/Filter/AndThen/OrElse` and `Result.Map/MapErr/AndThen/OrElse`
  retain the original they pass through.
- `Lazy.Map/AndThen` retain the source they return unchanged.
- `ConcurrentMap.GetOr` retains the default it hands back on a miss.
- `Seq.Fold` and `Parallel.Reduce` retain the identity they return untouched.
- The GUI sub-handle wrappers (`Menu.AddItem`, `TreeView.GetSelected`, …)
  retain the stable wrapper when it already exists, exactly like a new one.

`Map.GetOr` and `FrozenMap.GetOr` stay borrowed on both paths.

## Consequences

- Every runtime constructor, loader, snapshot, retarget and screenshot result
  is released when its Zia owner dies, on the VM and in native binaries. The
  engine-only world create/destroy loop and the Legacy Baseball stage harness
  now retain nothing per cycle.
- Borrowed string accessors no longer double-release. Code that took a string
  out of a `Result` and outlived the `Result` used to survive by accident; it
  is correct now.
- A C caller of the GUI wrappers or the combinators receives one more
  reference than before on the passthrough paths and must release it (the
  runtime's own tests were updated).
- Adding a runtime function that returns a reference now requires deciding,
  and stating, who owns the result.

## Tests

- `rtgen_definition_manifest` fixtures declare ownership; `rtgen` rejects a
  missing or misplaced token.
- `test_runtime_result_ownership`: every registered descriptor that returns a
  managed reference declares ownership; spot checks pin `Mesh3D.Box` owned,
  `SceneAsset.GetMesh` / `Entity3D.get_Mesh` borrowed, `Result.UnwrapStr`
  borrowed.
- `zia_runtime_test_runtime_result_ownership` and its native lane: owned
  results die with their owner (weak reference goes dead), borrowed results
  do not disturb their owner, borrowed strings stay valid after the owning
  `Result` is gone, sequence elements outlive a temporary sequence.
- `test_rt_gui_runtime` releases the extra wrapper reference; the Zanna Studio
  probes and the full suite are green.
