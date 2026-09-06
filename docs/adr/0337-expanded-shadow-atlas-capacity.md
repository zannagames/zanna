# ADR 0337: Expanded fixture shadow capacity

Status: Implemented; native Metal/software verified, native OpenGL/D3D11 pending

## Context

Closed-roof Legacy Baseball rings request 16 spotlight shadows, but only 12
slots exist. Raising a constant alone would increase allocation for smaller
scenes and can erase earlier tile depth when a backend grows storage mid-frame.
OpenGL also currently ignores ADR 0336's smaller secondary resolution because
all maps occupy one grow-only array.

## Decision

Expand the general shadow capacity to 20 while retaining four cascade slots.
Before any shadow slot renders, the shared pass announces the required slot
extent and primary/secondary resolutions through a private backend preparation
hook. Preparation failure leaves the frame unshadowed and reports dropped
requests. Missing hooks retain the existing compatibility path.

Metal/D3D11 use four atlas columns and enough rows for the announced extent
(minimum two, maximum four). Texture dimensions determine sampling scale and
square tile size. Frame preparation precedes tile rendering; cached render-target
inheritance preserves the cached extent. Primary texture storage remains separate.
OpenGL separates four primary array layers from secondary layers at their own
resolution, using a spare fragment texture unit. Allocate each class before the
first slot, with sufficient layers for the entire pass, so neither grows mid-pass.
Software retains lazy individual CPU depth targets. Cascade split arrays remain
float4; general slot expansion cannot widen cascade semantics.

The authored ShadowBudget is a slot budget: cascades are capped to the available
slots and a point light requires six complete slots. The prior renderer limited
selected light count but could exceed the slot budget through cascades/cube faces;
this implementation enforces the documented meaning.

## Acceptance

Regress slot planning for cascades, spots and six-face points, prepare failures,
more than 12 requested lights, resize/reuse and primary/secondary sizes. Native
probes must demonstrate visible shadows from late slots, not only submission
counts. Native ring diagnostics must grant all 16 requested banks with zero drops.
Measure memory and CPU/GPU costs; retain all game image/ball gates. Canonical
build/tests, API/layout checks and platform policy/host smoke must pass. Native
Windows/Linux/OpenGL/D3D11 results remain pending until actually executed.

## Results

617 GPU-path assertions and canonical graphics 163/163 pass. Native Metal/software
16-light tests prove visible late-slot shadows and stable atlas resizing. Native
closed-roof ring grants all 16 requested bank shadows with no drops. Full game
livecheck passes 14 bands / 42 ball samples. Host syntax-only OpenGL compilation
passes; native Linux/Windows/GL/D3D11 remain pending. Production remains two-bank
because all-bank CPU cost is still above target. Evidence is retained in
baseball/analysis/plan96/shadow-capacity-16/README.md.
