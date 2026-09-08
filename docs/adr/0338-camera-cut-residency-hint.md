# ADR 0338: Camera-cut residency hint

Status: Implemented; native Metal verified, native OpenGL/D3D11 pending

## Context

A hard camera cut relocates the whole visible set in one frame. Texture
uploads are paced by `Canvas3D.SetTextureUploadBudget` (12-24 MiB a frame on
the broadcast tiers), and a texture's FIRST upload has no fallback entry: the
draw binds the backend's 1x1 white default and the material renders flat
white until the upload publishes. The GPU texture caches were capped at 512
entries with LRU eviction, which an authored ballpark (crowd cards,
population, LODs, signage) exceeds, so textures behind the camera were
evicted and every cut re-uploaded them from nothing. Legacy Baseball showed
a white frame or two on every cut. Night masts made it worse: a lens flare
seen for the first time after a gap latched its raw visibility (the depth
probe has no result yet and counts as visible), so the near-white core popped
at full gain on the cut frame.

## Decision

Add `Canvas3D.NoteCameraCut()` (`rt_canvas3d_note_camera_cut`, backend vtable
`note_camera_cut`). Semantics: the frame that follows the hint completes every
texture upload its draws demand (the backend lifts its budget to unlimited
until its next present, then restores the value last set through
`set_texture_upload_budget`, including one set during the override); the
canvas drops its motion-blur history (a cut has no coherent previous frame);
every lens flare drawn on that frame fades in from zero visibility instead of
latching its first raw value. Metal, D3D11 and OpenGL implement the hook; the
software backend leaves it NULL and the canvas-side effects still apply.

Raise the resident 2D texture cache cap from 512 to 2048 entries on all three
GPU backends so a stadium's working set stays resident across cuts.

The hint is a runtime C ABI surface addition (`rendering.def`
`Canvas3D.NoteCameraCut`, `void()`); the generated runtime docs carry it.

## Acceptance

Unit: the canvas forwards the hint to the backend, clears the motion history
and arms exactly the next frame; a backend without the hook still takes the
canvas-side effects; the ABI surface test pins the PascalCase name and the
method row. Game: Legacy Baseball's stage calls the hint on every hard/wipe
cut and every >40 ft rig jump and keeps the upload budget open until the
backend reports nothing pending (at most 4 frames); the shotcheck probe's
cut burst samples at frame rate and fails on any near-white frame after a cut.
