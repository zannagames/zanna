---
status: active
audience: contributors
last-verified: 2026-07-28
---

# ADR 0218: Bound Zanna Studio Source Modules and Functions

## Status

Accepted (2026-07-28)

## Context

Zanna Studio grew through many independently tested editor, scene, debugger,
workspace, and source-control features. Several Zia controllers consequently
exceeded one thousand lines, some probes placed an entire workflow in one
entry function, and the largest scene editors concentrated several thousand
lines behind a single class definition.

Those units still compiled, but they made ownership hard to see. A change to
one responsibility required navigating unrelated state and event paths,
debugging frequently crossed distant sections of the same file, and review
could not isolate a feature's dependencies from the rest of its controller.
The structure also discouraged focused probes because adding coverage enlarged
already monolithic entry functions.

Zia classes cannot be reopened across modules. Stateful controller splits
therefore require either delegation through additional owned objects or a
deliberate inheritance layer between an existing base and facade. That changes
internal cross-layer dependencies and must be an explicit architecture
decision. ADR 0217 provides enough bounded import-graph capacity for the
resulting wider, still finite module graph.

## Decision

Zanna Studio Zia source uses the following review ceilings:

- A source file is at most 1,000 physical lines.
- A function or method is at most 200 physical lines.

These are maintainability review limits, not language or build-system limits.
If a cohesive unit approaches either ceiling, it is split before additional
unrelated behavior is added.

Large stateful controllers are decomposed in one of two ways:

1. Stateless algorithms, parsing, formatting, and query preparation move to
   focused helper modules and receive all required state as arguments.
2. When behavior must directly access a broad inherited controller contract,
   a narrowly named implementation layer extends the preceding layer. The
   dependency remains single-directional, and the next consumer binds only the
   terminal layer.

The externally used facade module and class names remain stable. New
implementation layers do not add product dependencies, allocate duplicate
controller state, or change ownership and destruction rules. A layer may add
overrides for one coherent responsibility only; it must not become a second
miscellaneous controller.

Large probes are split into named scenario functions or scenario modules.
Shared state is carried in an explicit typed context. Each scenario retains
the original cleanup, failure, canonical-state, and history assertions, and
the facade probe preserves its registered test name.

Every new or modified source module keeps the full Zanna source header. The
Studio source tree is checked as one import graph, affected probes run after
each batch, and the official platform build remains the final validation.

## Consequences

- File navigation, debugging, review, and ownership discovery have bounded
  local scope.
- Public Studio module and controller identities remain compatible.
- The import graph becomes wider and some controller inheritance chains gain
  internal layers, while remaining acyclic and bounded by ADR 0217.
- A behavior that truly requires more than the review ceilings must receive a
  follow-up ADR explaining why its responsibility cannot be divided cleanly.
- The decision changes no IL grammar or opcode, verifier rule, runtime C ABI,
  product dependency, file format, or platform adapter.

## Alternatives Considered

- **Keep large files and rely on editor folding.** Rejected because folding
  hides text but does not clarify dependencies, ownership, or review scope.
- **Split at arbitrary line offsets.** Rejected because line-count compliance
  without responsibility boundaries merely distributes the same coupling.
- **Duplicate controller state in independent classes.** Rejected because
  synchronization and lifetime bugs would replace the navigation problem.
- **Rename every public module after its implementation role.** Rejected
  because source organization should not force broad caller churn.
- **Treat probes as exempt.** Rejected because monolithic probes are difficult
  to diagnose and are executable documentation of the same contracts.
