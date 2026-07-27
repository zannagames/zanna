---
status: active
audience: contributors
last-verified: 2026-07-26
---

# Architecture Decision Records

Every ADR in the repository, grouped by area. ADRs record decisions that change
the IL specification, verifier rules, runtime C ABI surface, cross-layer
dependencies, or other contracts that must not drift silently. New ADRs start
from [0000-template.md](0000-template.md) and take the next unused number.

## Process & Governance

- [ADR 0006](0006-spec-currency-and-adr-triggers.md) — Spec Currency and ADR Triggers
- [ADR 0110](0110-project-rename-viper-to-zanna.md) — Rename the Project from Viper to Zanna
- [ADR 0118](0118-rename-zannaide-to-zanna-studio.md) — Rename ZannaIDE to Zanna Studio

## IL & Optimizer

- [ADR 0003](0003-il-linkage-and-module-linking.md) — IL Linkage and Module Linking
- [ADR 0005](0005-resume-token-provenance.md) — Resume Tokens Are Handler-Provenance Capabilities
- [ADR 0026](0026-range-analysis-demotion-proofs.md) — Whole-Function Range Analysis for Checked-Arithmetic Demotion Proofs
- [ADR 0063](0063-il-select-and-if-conversion.md) — IL `select` Opcode and If-Conversion
- [ADR 0064](0064-il-version-0-3-0.md) — IL Spec Version 0.2.0 → 0.3.0
- [ADR 0111](0111-il-text-resource-limits.md) — Bound Textual IL Parsing Resources
- [ADR 0147](0147-managed-reference-lowering-and-native-retain-elision.md) — Make Managed Reference Ownership Explicit Across Lowering and Native Codegen

## Concurrency & Threads

- [ADR 0002](0002-threads-monitor-safe.md) — Shared-Memory Threads with FIFO Monitors and Safe Variables
- [ADR 0020](0020-scheduler-generation-dimension.md) — Revision-Aware Scheduling (Zanna.Threads.Scheduler generations)

## Runtime API Design & Naming

- [ADR 0001](0001-builtin-signatures-from-registry.md) — Derive BASIC Builtin Signatures from Registry
- [ADR 0017](0017-string-lines-runtime-function.md) — CRLF-Aware Line Splitting (Zanna.String.Lines)
- [ADR 0027](0027-runtime-api-contract-metadata.md) — Add Contract Metadata to the Runtime API Dump
- [ADR 0028](0028-terminal-option-result-input-apis.md) — Terminal Option and Result Input APIs
- [ADR 0029](0029-diagnostics-current-trap-api.md) — Diagnostics Current Trap API
- [ADR 0030](0030-runtime-memory-and-gc-namespaces.md) — Runtime Memory and GC Namespaces
- [ADR 0031](0031-core-parse-double-aliases.md) — Core Parse Double Aliases
- [ADR 0032](0032-math-bits-full-name-aliases.md) — Math Bits Full-Name Aliases
- [ADR 0033](0033-core-convert-string-aliases.md) — Core Convert String Aliases
- [ADR 0034](0034-capacity-aliases.md) — Capacity Aliases
- [ADR 0035](0035-bloomfilter-false-positive-rate-alias.md) — BloomFilter False Positive Rate Alias
- [ADR 0036](0036-format-and-frame-abbreviation-aliases.md) — Format And Frame Abbreviation Aliases
- [ADR 0037](0037-collection-write-verb-aliases.md) — Collection Write Verb Aliases
- [ADR 0038](0038-graphics-factory-aliases.md) — Graphics Factory Aliases
- [ADR 0039](0039-option-failure-aliases.md) — Option Failure Aliases
- [ADR 0040](0040-input-key-namespace.md) — Input Key Namespace
- [ADR 0041](0041-crypto-result-and-legacy-apis.md) — Crypto Result APIs and Legacy Namespaces
- [ADR 0042](0042-http-tls-verification-bypass-api.md) — HTTP TLS Verification Bypass API
- [ADR 0043](0043-random-chance-boolean-api.md) — Random Chance Boolean API
- [ADR 0044](0044-crypto-module-process-policy-api.md) — Crypto Module Process Policy API
- [ADR 0045](0045-boxed-value-type-unsafe-api.md) — Boxed Value-Type Unsafe API
- [ADR 0047](0047-network-http-result-apis.md) — Network HTTP Result APIs
- [ADR 0048](0048-runtime-unsafe-trap-state-hooks.md) — Runtime Unsafe Trap-State Hooks
- [ADR 0049](0049-terminal-signature-nullability-cleanup.md) — Terminal Signature Nullability Cleanup
- [ADR 0050](0050-game-stateful-result-objects.md) — Game Stateful Result Objects
- [ADR 0051](0051-data-format-result-parse-apis.md) — Data Format Result Parse APIs
- [ADR 0052](0052-connect-open-load-result-apis.md) — Connect, Open, and Load Result APIs
- [ADR 0053](0053-search-option-apis.md) — Search Option APIs
- [ADR 0054](0054-unionfind-find-root-option-api.md) — UnionFind Root Lookup Option API
- [ADR 0055](0055-exec-command-result-api.md) — Exec CommandResult API
- [ADR 0056](0056-table-click-result-api.md) — Table ClickResult API
- [ADR 0057](0057-smtp-send-result-api.md) — SMTP Send Result API
- [ADR 0060](0060-sceneasset-load-result-apis.md) — SceneAsset Load Result APIs
- [ADR 0061](0061-zia-semantic-job-error-option-api.md) — Zia SemanticJob Error Option API
- [ADR 0062](0062-pathresult-step-count-api.md) — PathResult StepCount API
- [ADR 0066](0066-process-pty-read-result-apis.md) — Process And PTY Read Result APIs
- [ADR 0101](0101-modular-runtime-definitions-and-documentation.md) — Modular Runtime Definitions and Authored API Documentation
- [ADR 0104](0104-entity3d-position-accessor-properties.md) — Normalize Entity3D Position Accessors to Properties
- [ADR 0115](0115-retained-runtime-resource-snapshots.md) — Retain Runtime Resources Across Unlocked Work
- [ADR 0116](0116-gc-mutator-quiescence-and-array-cycles.md) — Coordinate Cycle Collection with Managed-Graph Mutators
- [ADR 0117](0117-archive-validation-resource-and-concurrency-policy.md) — Validate Archive Structure and Bound Serialized Writer Transactions
- [ADR 0119](0119-trap-safe-managed-io-ownership.md) — Make Managed I/O Construction and Result Ownership Trap-Safe
- [ADR 0120](0120-http-router-stable-publication-and-trap-safe-matching.md) — Keep HTTP Router Publication Stable and Matching Trap-Safe
- [ADR 0121](0121-network-policy-identity-and-atomic-retry-state.md) — Give Network Policies Stable Identity and Atomic Retry State
- [ADR 0122](0122-network-transport-identity-and-exclusive-pool-leases.md) — Validate Network Transports and Lease Pooled TCP Handles Exclusively
- [ADR 0123](0123-udp-managed-identity-and-datagram-integrity.md) — Preserve UDP Managed Identity and Atomic Datagram Integrity
- [ADR 0124](0124-trap-safe-native-promise-completion-and-async-socket-initialization.md) — Make Native Promise Completion and AsyncSocket Initialization Trap-Safe
- [ADR 0125](0125-multipart-stable-identity-and-atomic-native-staging.md) — Give Multipart Stable Identity and Atomic Native Staging
- [ADR 0126](0126-http-client-stable-identity-and-transactional-ownership.md) — Make HTTP Client Identity and Ownership Transactional
- [ADR 0127](0127-session-http-client-identity-and-synchronized-snapshots.md) — Give Session HTTP Clients Stable Identity and Synchronized Snapshots
- [ADR 0128](0128-http-server-stable-identity-and-serialized-lifecycle.md) — Give HTTP Servers Stable Identity and Serialized Lifecycles
- [ADR 0129](0129-tls-session-identity-and-native-socket-width.md) — Give TLS Sessions Stable Identity and Native-Width Socket Ownership
- [ADR 0130](0130-websocket-client-identity-strict-framing-and-transactional-ownership.md) — Give WebSocket Clients Stable Identity and Strict Transactional Framing
- [ADR 0131](0131-websocket-server-identity-strict-upgrades-and-generation-safe-lifecycle.md) — Give WebSocket Servers Stable Identity and Generation-Safe Lifecycles
- [ADR 0132](0132-sse-smtp-identity-strict-framing-and-cancellation-safe-lifecycles.md) — Give SSE and SMTP Strict Framing and Cancellation-Safe Lifecycles
- [ADR 0133](0133-runtime-concurrency-and-collection-hardening.md) — Make Runtime Concurrency Ownership Typed and Collection Capacity Explicit
- [ADR 0134](0134-zpak-v2-validation-and-entry-checksums.md) — Validate ZPAK Metadata and Checksum Every Version 2 Entry
- [ADR 0135](0135-runtime-cppcheck-build-and-ci-gate.md) — Gate Runtime Portability and Correctness Diagnostics with Cppcheck
- [ADR 0136](0136-runtime-context-binding-lifecycle-and-state-locks.md) — Serialize Runtime Context State and Reserve Child Bindings Before Native Start
- [ADR 0143](0143-generated-runtime-class-inheritance.md) — Generate Runtime Class Inheritance Metadata
- [ADR 0169](0169-super-modifier-keys-and-studio-viewport-picking.md) — Add Super Keys and Geometry-Aware Studio Viewport Interaction

## GUI & IDE

- [ADR 0007](0007-codeeditor-syntax-surface-expansion.md) — CodeEditor Syntax Surface Expansion Uses Registry-Only Semantics
- [ADR 0008](0008-semantic-token-overlay.md) — Semantic Token Overlay Uses Registry-Only Semantics
- [ADR 0009](0009-debug-evaluate-protocol.md) — Debug Adapter Evaluate Protocol Extension
- [ADR 0010](0010-workspace-file-index-status.md) — Workspace File Index Status Runtime API
- [ADR 0011](0011-codeeditor-editing-runtime-api.md) — CodeEditor Editing Runtime API
- [ADR 0012](0012-debug-conditional-breakpoints-logpoints.md) — Debug Conditional Breakpoints and Logpoints
- [ADR 0013](0013-editor-input-popup-runtime-surface.md) — Editor Input and Popup Runtime Surface
- [ADR 0014](0014-basic-language-service-runtime-bridge.md) — Zanna BASIC Language-Service Runtime Bridge
- [ADR 0015](0015-workspace-file-index-paging.md) — Workspace File Index Paging
- [ADR 0016](0016-pty-runtime-surface.md) — PTY Runtime Surface (Zanna.System.Pty) and Integrated Terminal
- [ADR 0018](0018-gui-command-binding-runtime-surface.md) — GUI Command Binding (Zanna.GUI.Command / CommandRegistry)
- [ADR 0019](0019-gui-text-cell-metrics-runtime-surface.md) — GUI Text/Cell Metrics (Zanna.GUI.OutputPane measurement)
- [ADR 0021](0021-gui-app-logical-unit-helpers.md) — HiDPI Logical-Unit Helpers (Zanna.GUI.App)
- [ADR 0022](0022-gui-layout-conveniences.md) — GUI Layout Conveniences (panel centering + Zanna.GUI.Grid)
- [ADR 0023](0023-gui-popuplist-widget.md) — Caret-Anchored Filtered Popup (Zanna.GUI.PopupList)
- [ADR 0024](0024-text-char-and-editor-insert-helpers.md) — Text/Editing Helpers (Zanna.Text.Char + CodeEditor.InsertAndPlaceCursor)
- [ADR 0058](0058-gui-lookup-option-apis.md) — GUI Lookup Option APIs
- [ADR 0067](0067-gui-toolbar-named-icons.md) — GUI Toolbar Named Icon Runtime API
- [ADR 0068](0068-gui-app-activation-api.md) — GUI App Activation API
- [ADR 0106](0106-gui-runtime-lifetime-contract-and-coordinate-policy.md) — Make GUI Lifetimes, Contracts, and Coordinates Explicit
- [ADR 0107](0107-gui-theme-accessibility-input-and-render-policy.md) — Unify GUI Theme, Accessibility, Input, and Rendering State
- [ADR 0108](0108-gui-control-layout-and-model-completeness.md) — Complete the GUI Control, Layout, and Virtual Model Surface
- [ADR 0109](0109-gui-dialog-media-scheduling-and-automation.md) — Make GUI Dialogs, Media, and Automation Frame-Driven
- [ADR 0137](0137-premium-rendering-surface.md) — Premium Rendering Surface for Zanna Studio
- [ADR 0138](0138-debug-class-layout-sidecar.md) — Debug Class-Layout Sidecar for VM Debugger Field Expansion
- [ADR 0148](0148-bounded-directory-paging.md) — Page Immediate Directory Entries Without Blocking GUI Work Loops
- [ADR 0150](0150-gui-native-minimum-window-size.md) — Enforce GUI Minimum Window Sizes Through Native Adapters
- [ADR 0151](0151-transactional-multi-root-workspace-edits.md) — Bound Transactional Workspace Edits to Explicit Multiple Roots
- [ADR 0152](0152-stable-file-identity-for-editor-documents.md) — Use Stable File Identity for Editor Document De-duplication
- [ADR 0153](0153-non-following-path-link-inspection.md) — Reject Linked Descendants at Explorer Boundaries
- [ADR 0154](0154-single-owner-split-editor-documents.md) — Give Split-Editor Documents One Live Buffer Owner
- [ADR 0156](0156-listbox-selected-row-data.md) — Expose Selected ListBox Row Data
- [ADR 0163](0163-stable-multiselect-and-row-aware-treeview-editing.md) — Add Stable Multi-Select and Row-Aware TreeView Editing
- [ADR 0165](0165-scrollview-descendant-reveal.md) — Expose ScrollView Descendant Reveal
- [ADR 0167](0167-spinner-mixed-value-state.md) — Expose Spinner Mixed-Value State

## Graphics3D & Game Systems

- [ADR 0004](0004-graphics3d-runtime-surface-expansion.md) — Graphics3D Runtime Surface Expansion Uses Registry-Only Semantics
- [ADR 0046](0046-game3d-prefab-loading-api.md) — Game3D Prefab Loading API
- [ADR 0059](0059-graphics3d-lookup-option-apis.md) — Graphics3D Lookup Option APIs
- [ADR 0065](0065-gameui-canvas-polymorphic-widgets.md) — Game.UI widgets are canvas-polymorphic (2D Canvas + Canvas3D)
- [ADR 0069](0069-canvas3d-diagnostics-api.md) — Canvas3D Diagnostics API
- [ADR 0070](0070-canvas3d-clustered-lighting-probe-api.md) — Canvas3D Clustered Lighting Probe API
- [ADR 0071](0071-mesh3d-retained-bytes-api.md) — Mesh3D Retained Bytes API
- [ADR 0072](0072-vegetation3d-scatter-seed-api.md) — Vegetation3D Scatter Seed API
- [ADR 0074](0074-game3d-thirdperson-controller-and-target-lock.md) — Game3D Third-Person Controller And Target Lock
- [ADR 0075](0075-character3d-dynamics-and-platforms.md) — Character3D Dynamic-Body Interaction, Crouch, And Moving Platforms
- [ADR 0076](0076-physics3d-traversal-probes.md) — Physics3D Traversal Probes
- [ADR 0077](0077-game3d-combat-volumes-and-health.md) — Game3D Combat Volumes And Health
- [ADR 0078](0078-graphics3d-ragdoll.md) — Graphics3D Ragdoll Pipeline
- [ADR 0079](0079-game3d-time-control.md) — Game3D Time Control (TimeScale, Pause, Hit-Stop)
- [ADR 0080](0080-game3d-cinematic-cameras.md) — Game3D Cinematic Cameras (RailCamera3D + Timeline3D)
- [ADR 0081](0081-game3d-dialogue.md) — Game3D Dialogue System (Dialogue3D + Camera3D.WorldToScreen)
- [ADR 0082](0082-game3d-facial-and-voice-metering.md) — Game3D Facial Animation (LipSync3D) and Per-Voice Metering
- [ADR 0083](0083-worldstream-async-staging.md) — Worker-Backed WorldStream3D Staging with Ordered Main-Thread Commits
- [ADR 0084](0084-worldstream-hlod-proxies-and-impostors.md) — Cell-Level HLOD Proxies and Automated Impostors
- [ADR 0085](0085-gpu-skinning-capability-and-telemetry.md) — GPU Skinning Capability, Routing Override, and Telemetry
- [ADR 0086](0086-terrain-holes-splats-autoblend.md) — Terrain Holes, 8 Splat Layers, and Slope/Height Auto-Blend
- [ADR 0087](0087-height-fog-sun-inscattering.md) — Height-Fog Sun Inscattering
- [ADR 0088](0088-baked-gi-lightmaps-and-probes.md) — Baked GI — LightBaker3D and LightProbeGrid3D
- [ADR 0089](0089-reflection-probes.md) — Local Reflection Probes (Capture Core)
- [ADR 0090](0090-procedural-sky-and-time-of-day.md) — Procedural Sky and Time-of-Day Clock
- [ADR 0091](0091-physics-materials-and-surfaces.md) — Per-Collider Physics Materials, Surface Tags, Body User Data
- [ADR 0092](0092-footsteps-surface-events.md) — Surface-Driven Footsteps (SurfaceTable3D + Footsteps3D)
- [ADR 0093](0093-game3d-interaction.md) — Focus-and-Use Interaction (Interactable3D + Interactor3D)
- [ADR 0094](0094-game3d-ai-perception-behavior-trees.md) — NPC AI — Perception3D and BehaviorTree3D
- [ADR 0095](0095-audio-immersion.md) — Audio Immersion — Reverb Zones, Occlusion, Ambient Beds, Dialogue Ducking
- [ADR 0096](0096-cloth3d-verlet.md) — Cloth3D — Verlet Chains and Patches
- [ADR 0097](0097-world-persistence.md) — Streamed-World Entity-State Persistence + Save Slots
- [ADR 0098](0098-game3d-minimap-markers.md) — Minimap3D, Compass, and World Markers
- [ADR 0099](0099-game-quests.md) — Zanna.Game.Quests — Objective Tracker with SaveData Integration
- [ADR 0100](0100-profiling-depth.md) — Profiling Depth — Per-Pass Draw Attribution + Hitch Tracer
- [ADR 0102](0102-graphics3d-runtime-boundary-and-contract-manifest.md) — Graphics3D Runtime Boundary and Contract Manifest
- [ADR 0105](0105-game3d-character-controller-gravity.md) — CharacterController3D Gravity Magnitude
- [ADR 0112](0112-linux-graphics-backend-selection.md) — Linux Graphics Backend Selection
- [ADR 0114](0114-ieee-floating-constant-folding.md) — Preserve IEEE-754 Results During IL Constant Folding
- [ADR 0139](0139-native-wayland-backend-and-linux-runtime-selection.md) — Native Wayland Backend and Linux Runtime Selection
- [ADR 0140](0140-tiled-map-and-scene-import.md) — Import Tiled Maps as Scene Documents and Tilemaps
- [ADR 0141](0141-vscn-v4-scene-asset-fidelity.md) — Preserve Complete Scene Assets in VSCN v4
- [ADR 0142](0142-complete-fbx-scene-animation-import.md) — Complete FBX Scene and Animation Import
- [ADR 0144](0144-complete-tiled-map-import.md) — Complete Tiled Map Import and Projected Tilemap Rendering
- [ADR 0145](0145-complete-fbx-evaluation-and-native-lights.md) — Complete FBX Evaluation and Native Authored Lights
- [ADR 0146](0146-vscn-v5-source-texture-containers.md) — Preserve Source Texture Containers in VSCN v5
- [ADR 0157](0157-material-texture-pixel-inspection.md) — Expose Read-Only Material Texture Pixels for Authoring Tools
- [ADR 0158](0158-scene-level-property-authoring.md) — Make Scene-Level Properties Fully Authorable
- [ADR 0159](0159-typed-scenenode-metadata-and-vscn-v6.md) — Add Typed SceneNode Metadata and VSCN v6
- [ADR 0160](0160-project-scene-component-schemas.md) — Add Project Scene Component Schemas
- [ADR 0161](0161-stable-scenenode-sibling-reordering.md) — Add Stable SceneNode Sibling Reordering
- [ADR 0162](0162-exact-preserve-world-scenenode-reparenting.md) — Add Exact Preserve-World SceneNode Reparenting
- [ADR 0164](0164-backward-compatible-2d-scene-object-hierarchy.md) — Add Backward-Compatible 2D Scene Object Hierarchy
- [ADR 0166](0166-exact-scenenode-world-matrix-assignment.md) — Add Exact SceneNode World-Matrix Assignment
- [ADR 0168](0168-windowless-canvas3d-rendering.md) — Add Windowless Canvas3D Rendering
- [ADR 0170](0170-studio-2d-canvas-marquee-selection.md) — Add Modifier and Marquee Selection to the Studio 2D Canvas
- [ADR 0171](0171-bounded-scene-flood-fill-and-studio-tile-tools.md) — Add Bounded Scene Flood Fill and Studio Tile Tools
- [ADR 0172](0172-public-scenenode-light-authoring-and-studio-light-inspector.md) — Expose SceneNode Lights and Add Studio Light Authoring
- [ADR 0173](0173-graphics3d-transactional-hardening-and-retained-work.md) — Make Graphics3D State Transactional and Retain Reusable Work
- [ADR 0174](0174-scene-object-authoring-metadata-and-duplication.md) — Make Scene Object Metadata and Duplication Authorable
- [ADR 0176](0176-typed-tile-behavior-sections.md) — Make Tile Behavior Sections Fully Authorable
- [ADR 0177](0177-scene-camera-and-lighting-sections.md) — Add Scene Camera and Lighting Section Contracts
- [ADR 0178](0178-scene-component-schema-v2-enum-asset-fields.md) — Add Enum and Asset Fields to Scene Component Schemas
- [ADR 0179](0179-2d-object-sprite-preview-and-route-conventions.md) — Add 2D Object Sprite Previews and Route Visualization Conventions
- [ADR 0180](0180-workspace-asset-library.md) — Add Workspace Asset Libraries
- [ADR 0181](0181-2d-scene-preview-and-run-scene.md) — Add 2D Scene Preview and Run Scene
- [ADR 0182](0182-canonical-scene2d-scene3d-extensions.md) — Adopt Canonical .scene2d and .scene3d Extensions
- [ADR 0183](0183-perspective-editor-viewport-and-fly-navigation.md) — Add a Perspective Editor Viewport with Fly Navigation
- [ADR 0184](0184-scene-camera-nodes-look-through-and-preview.md) — Author Scene Camera Nodes with Look-Through and Preview
- [ADR 0185](0185-collider-authoring-and-gameplay-overlays.md) — Author Colliders and Gameplay Overlays on Scene Nodes
- [ADR 0186](0186-batch-light-editing.md) — Add Mixed-Value Batch Light Editing
- [ADR 0187](0187-vscn-v7-prefab-reference-nodes.md) — Add VSCN v7 Prefab Reference Nodes
- [ADR 0188](0188-studio-bake-and-environment-workflow.md) — Add the Studio Bake and Environment Workflow
- [ADR 0189](0189-project-material-library.md) — Add a Project Material Library
- [ADR 0190](0190-scene3d-text-serialization.md) — In-Memory Scene3D Text Serialization
- [ADR 0191](0191-gpu-offscreen-editor-rendering.md) — GPU-Accelerated Offscreen Editor Rendering
- [ADR 0192](0192-2d-object-transforms.md) — Optional 2D Scene Object Transforms
- [ADR 0193](0193-scene3d-precise-raycast.md) — Triangle-Accurate Scene Raycast Queries
- [ADR 0194](0194-scene-watch-hot-reload.md) — The --scene-watch Hot-Reload Contract
- [ADR 0195](0195-2d-layer-opacity.md) — Optional 2D Layer Opacity

## Release & Packaging

- [ADR 0025](0025-windows-release-installer-workflow.md) — Windows Release Installer Workflow
- [ADR 0073](0073-cross-platform-installer-release-pipeline.md) — Cross-Platform Installer Release Pipeline
- [ADR 0103](0103-windows-developer-installer-v2.md) — Windows Developer Installer v2
- [ADR 0113](0113-windows-automation-powershell-entry-points.md) — Use PowerShell for Windows Automation Entry Points
- [ADR 0149](0149-macos-zanna-studio-application-identity.md) — Preserve Zanna Studio Identity in the macOS Application Menu
- [ADR 0155](0155-map-msvc-float-classification-to-ucrt.md) — Map MSVC Float Classification to UCRT
- [ADR 0175](0175-zanna-games-windows-installer-experience.md) — Give Windows Setup a Native Zanna Games Experience
- [ADR 0196](0196-map-windows-runtime-hardening-imports.md) — Map Windows Runtime Hardening Imports
- [ADR 0197](0197-project-owned-2d-object-preview-profiles.md) — Add Project-Owned 2D Object Preview Profiles
- [ADR 0198](0198-project-owned-3d-scene-preview-profiles.md) — Add Project-Owned 3D Scene Preview Profiles
- [ADR 0199](0199-project-owned-3d-node-prefab-previews.md) — Add Project-Owned 3D Node Prefab Previews
- [ADR 0200](0200-project-owned-3d-sky-and-light-preview-rigs.md) — Add Project-Owned 3D Sky and Light Preview Rigs
- [ADR 0201](0201-project-owned-3d-lens-and-atmosphere-previews.md) — Add Project-Owned 3D Lens and Atmosphere Previews
