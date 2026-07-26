---
status: active
audience: contributors
last-verified: 2026-07-26
---

# ADR 0195: Optional 2D Layer Opacity

## Status

Accepted (2026-07-26)

## Context

Scene layers carry `name`, `visible`, `asset`, and `tiles`. Bringing the
Studio layers panel to parity (ADR train of 0192) needs per-layer opacity
for authored translucency — parallax silhouettes, water overlays, and
editor onion-skinning workflows all fade whole layers. Editor-only
concerns (lock, solo) stay workspace state, but opacity affects how a
game renders the layer, so it belongs to the document model. The scene
JSON is golden/parity-pinned, so the field must be byte-invisible when
unused.

## Decision

Each layer gains an optional `opacity` float, default `1.0`, clamped to
`[0, 1]` on load and through the setter (non-finite input becomes the
default). It serializes only when it differs from `1.0`, keeping every
existing document byte-stable. Runtime surface follows the layer accessor
convention: `SceneDocument.LayerOpacity(layer)` and
`SetLayerOpacity(layer, opacity)`. Consumers (Studio's canvas now, game
spawn paths as they adopt it) multiply the layer's tile alpha by the
value; `0` renders nothing while remaining an authored value distinct
from `visible: false`.

Editor lock and solo toggles are explicitly **not** model fields: they
guard editing and preview inside Studio only and never serialize.

## Consequences

Legacy scenes are byte-stable by construction; goldens and parity suites
need no regeneration. The layer JSON namespace reserves one more key,
mirroring how `visible` already behaves. Games ignore the field until
their pipelines consume it — the model makes it available, nothing more.
