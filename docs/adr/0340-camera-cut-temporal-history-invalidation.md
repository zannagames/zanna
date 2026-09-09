---
status: accepted
audience: contributors
last-verified: 2026-09-09
---

# ADR 0340: Invalidate Image History on Camera Cuts

## Context

`Canvas3D.NoteCameraCut` currently completes visible texture uploads,
clears per-object motion records, and restarts flare visibility. TAA retains
its color and previous camera even though a cut has no coherent previous
image. Depth validation cannot reject stale history when unrelated shots
happen to contain surfaces at similar depths.

## Decision

Extend the existing `Canvas3D.NoteCameraCut` / `rt_canvas3d_note_camera_cut`
contract to invalidate TAA color/depth validity and previous-camera validity
on software, Metal, D3D11, and OpenGL. Reset the GPU jitter sequence to its
first sample. Retain allocated storage, exposure adaptation, the selected
post-FX chain, upload-budget behavior, and flare behavior.

The next live resolve seeds matching color and depth without borrowing
history. Inset rendering keeps its existing history-ownership rules. No
new registered function, IL rule, dependency, or serialized format is added.
An internal PostFX helper lets Canvas3D invalidate CPU image history without
resetting exposure or transferring chain ownership.

## Validation

CPU regression: populate history, announce a cut with an unchanged camera
matrix and depth, and render contrasting current colors. The cut frame
must retain current RGB and alpha rather than blend the old image. Existing
camera-cut upload/flare tests remain green. GPU validation uses matched
native motion/cut captures and checks corresponding invalidation in each
backend's cut callback. A cut must not allocate fresh history textures.
