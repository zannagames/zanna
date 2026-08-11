---
status: complete
audience: contributors
last-verified: 2026-08-11
---

# Graphics3D Runtime Hardening Program, Round Four (2026-08-11)

## 1. Scope

This live ledger records a fourth non-duplicative review of the C runtime under
`src/runtime/graphics/3d`. The target is 100 concrete correctness, memory-safety,
resource-safety, determinism, and performance corrections. Earlier July and
August Graphics3D audit ledgers are the baseline and their closed findings are
not recounted here.

The work does not change the runtime C ABI, IL opcode set, language grammar,
serialized scene format, platform policy, external dependencies, or CI
workflows. Any later finding that requires one of those changes must be split
behind the repository's ADR process before implementation.

## 2. Corrected issue ledger

Evidence keys:

- `MAT`: adversarial retained-state cases in `test_rt_material3d`.
- `PHY`: adversarial joint ownership/numeric cases in `test_rt_physics3d`.
- `RT`: adversarial RenderTarget3D allocation-identity and readback cases in
  `test_rt_canvas3d`.
- `CUBE`: adversarial CubeMap3D source/IBL identity, numeric, sampling, and
  lifetime cases in `test_rt_cubemap3d_ibl` and the related Canvas3D tests.
- `STATIC`: focused source review plus clean `cppcheck` warning, performance,
  and portability analysis over the compiled 3D runtime.
- `BUILD`: warning-as-error macOS build through `build_zanna_mac.sh`.
- `G3D`: complete `graphics3d` CTest label.
- `SAN`: AddressSanitizer and UndefinedBehaviorSanitizer validation.
- `PLATFORM`: repository platform-policy lint and cross-platform smoke.

| ID | Area | Class | Finding and resolution | Evidence |
|---|---|---|---|---|
| G3H4-001 | Material texture | Type safety | The raw base-color getter returned a wrong-class private pointer. Validate the retained source and clear borrowed corruption before readback. | MAT |
| G3H4-002 | Material texture | Type safety | The raw normal-map getter exposed the same wrong-class state. Repair the slot before returning it. | MAT |
| G3H4-003 | Material texture | Type safety | The raw specular-map getter exposed the same wrong-class state. Repair the slot before returning it. | MAT |
| G3H4-004 | Material texture | Type safety | The raw emissive-map getter exposed the same wrong-class state. Repair the slot before returning it. | MAT |
| G3H4-005 | Material texture | Type safety | The raw metallic-roughness getter exposed the same wrong-class state. Repair the slot before returning it. | MAT |
| G3H4-006 | Material texture | Type safety | The raw ambient-occlusion getter exposed the same wrong-class state. Repair the slot before returning it. | MAT |
| G3H4-007 | Material texture | Type safety | The raw lightmap getter exposed the same wrong-class state. Repair the slot before returning it. | MAT |
| G3H4-008 | Material environment | Type safety | The raw environment getter returned wrong-class or incomplete cubemaps. Reuse the retained cubemap repair boundary before readback. | MAT |
| G3H4-009 | Material emissive | Numeric | Emissive-color readback copied NaN, infinity, and negative channels into a new Vec3. Sanitize and persist all three lanes first. | MAT |
| G3H4-010 | Material shininess | Numeric | Shininess readback returned non-finite or out-of-range retained state. Clamp and persist the scalar before returning it. | MAT |
| G3H4-011 | Material depth bias | Numeric | Constant depth-bias readback returned corrupt non-finite state. Restore the neutral bounded value before returning it. | MAT |
| G3H4-012 | Material depth bias | Numeric | Slope-scaled depth-bias readback had the same exposure. Restore its neutral bounded value independently. | MAT |
| G3H4-013 | Material custom data | Numeric | Custom-parameter readback returned NaN or infinity. Repair the selected retained lane before returning it. | MAT |
| G3H4-014 | Material custom data | Correctness | The generic signed clamp mapped a non-finite custom parameter to the negative magnitude limit. Use a symmetric clamp with neutral zero fallback. | MAT |
| G3H4-015 | Material depth bias | Correctness | Non-finite constant bias likewise became the strongest supported negative bias. Use the neutral symmetric clamp. | MAT |
| G3H4-016 | Material depth bias | Correctness | Non-finite slope bias had the same maximum-negative fallback. Normalize it independently to zero. | MAT |
| G3H4-017 | Material clone | Correctness | Clone and MakeInstance silently discarded the soft-particle fade distance. Copy it into the independent shell and sanitize it. | MAT |
| G3H4-018 | Material clone | Correctness | Clone and MakeInstance silently disabled the retained SSR opt-in. Copy and canonicalize it. | MAT |
| G3H4-019 | Material state | Correctness | Retained SSR bytes could remain noncanonical when copied through private state. Normalize the copied flag to zero or one. | MAT |
| G3H4-020 | Material alpha | Correctness | The copied explicit-alpha-mode byte could remain noncanonical, changing automatic texture-alpha promotion. Normalize it to zero or one. | MAT |
| G3H4-021 | Distance joint | Ownership | `BodyA` returned a mutable mirror that could be replaced with an unrelated object. Restore it from the retained owner before readback. | PHY |
| G3H4-022 | Distance joint | Ownership | `BodyB` exposed the same mutable-mirror substitution independently. Repair the second slot before readback. | PHY |
| G3H4-023 | Spring joint | Ownership | `BodyA` returned an unvalidated substituted mirror. Make the retained identity authoritative. | PHY |
| G3H4-024 | Spring joint | Ownership | `BodyB` returned an unvalidated substituted mirror. Repair it independently. | PHY |
| G3H4-025 | Hinge joint | Ownership | `BodyA` exposed a corrupted private reference rather than the retained body. Restore the mirror before use. | PHY |
| G3H4-026 | Hinge joint | Ownership | `BodyB` had the same readback exposure. Restore the second mirror independently. | PHY |
| G3H4-027 | Rope joint | Ownership | `BodyA` trusted mutable storage rather than owned identity. Repair before returning it. | PHY |
| G3H4-028 | Rope joint | Ownership | `BodyB` trusted the same mutable storage independently. Repair before returning it. | PHY |
| G3H4-029 | SixDof joint | Ownership | `BodyA` could return a wrong-class substituted handle. Resolve it from retained ownership first. | PHY |
| G3H4-030 | SixDof joint | Ownership | `BodyB` could return a wrong-class substituted handle. Repair the second reference independently. | PHY |
| G3H4-031 | Distance joint | Lifetime | Finalization followed corrupted public body mirrors, leaking the two real retains. Release only private ownership identities. | PHY |
| G3H4-032 | Spring joint | Lifetime | Spring finalization likewise lacked authoritative body ownership. Add and release immutable retained identities. | STATIC |
| G3H4-033 | Hinge joint | Lifetime | Hinge finalization could lose its retained bodies after mirror corruption. Finalize only the owners. | STATIC |
| G3H4-034 | Rope joint | Lifetime | Rope finalization followed mutable body pointers. Separate ownership from public solver mirrors. | STATIC |
| G3H4-035 | SixDof joint | Lifetime | SixDof finalization could leak both retained bodies after private mirror substitution. Release the owner slots. | STATIC |
| G3H4-036 | Distance solve | Correctness | The constraint solver could move an unrelated substituted Body3D while leaving its owned body untouched. Repair the pair before solving. | PHY |
| G3H4-037 | Spring solve | Correctness | Spring impulses trusted mutable body mirrors. Gate solving on repaired owned bodies. | STATIC |
| G3H4-038 | Hinge solve | Correctness | Hinge corrections and motor impulses trusted mutable body mirrors. Repair and validate both owners at dispatch. | STATIC |
| G3H4-039 | Rope solve | Correctness | Rope projection trusted mutable body mirrors. Restore the owned pair before any kinematic read. | STATIC |
| G3H4-040 | SixDof solve | Memory safety | Failed pair validation could fall through to a solver that dereferenced cleared body mirrors. Stop dispatch when repair fails. | STATIC |
| G3H4-041 | Distance readback | Numeric | `Distance` returned corrupt non-finite retained state. Sanitize and persist it before readback. | PHY |
| G3H4-042 | Spring readback | Numeric | `RestLength` returned corrupt non-finite retained state. Restore the neutral bounded value. | PHY |
| G3H4-043 | Spring readback | Numeric | `Stiffness` returned infinity verbatim. Enforce the solver's non-negative parameter bound. | PHY |
| G3H4-044 | Spring readback | Numeric | `Damping` returned negative infinity verbatim. Sanitize and persist it independently. | PHY |
| G3H4-045 | Rope readback | Numeric | `MaxLength` returned corrupt non-finite retained state. Apply the rope parameter sanitizer on read. | PHY |
| G3H4-046 | Hinge motor | Correctness | The retained motor-enable byte remained noncanonical, complicating deterministic snapshots. Normalize and persist it. | PHY |
| G3H4-047 | Hinge motor | Numeric | Motor target-velocity readback returned infinity. Clamp it to the signed solver envelope. | PHY |
| G3H4-048 | Hinge motor | Numeric | Motor maximum-impulse readback returned non-finite state. Restore a neutral non-negative value. | PHY |
| G3H4-049 | Hinge limits | Correctness | The retained limit-enable byte remained noncanonical. Normalize it before readback or solving. | PHY |
| G3H4-050 | Hinge limits | Numeric | Active limits with either non-finite endpoint poisoned angle comparisons. Disable and clear the invalid pair atomically. | PHY |
| G3H4-051 | Hinge limits | Numeric | `LimitMin` could expose a NaN endpoint from corrupt retained state. Repair the pair before returning the lower bound. | PHY |
| G3H4-052 | Hinge limits | Numeric | `LimitMax` could expose infinity independently. Return only the repaired upper bound. | PHY |
| G3H4-053 | SixDof limits | Numeric | `LinearLimitMin` boxed non-finite and reversed retained lanes. Sanitize and canonicalize all axes first. | PHY |
| G3H4-054 | SixDof limits | Numeric | `LinearLimitMax` exposed the same malformed limit pair independently. Return the repaired upper lanes. | PHY |
| G3H4-055 | SixDof limits | Numeric | `AngularLimitMin` boxed non-finite and reversed retained lanes. Repair the angular pair before allocation. | PHY |
| G3H4-056 | SixDof limits | Numeric | `AngularLimitMax` exposed malformed upper lanes independently. Return the canonicalized values. | PHY |
| G3H4-057 | SixDof motor | Correctness | The retained linear-motor enable byte remained noncanonical. Normalize and persist it. | PHY |
| G3H4-058 | SixDof motor | Numeric | Linear-motor velocity readback boxed three non-finite lanes. Clamp every lane before creating the Vec3. | PHY |
| G3H4-059 | SixDof motor | Numeric | Linear-motor maximum-impulse readback returned infinity. Apply the non-negative solver bound. | PHY |
| G3H4-060 | Physics test | Test integrity | `EXPECT_NEAR` treated NaN as a passing comparison because `fabs(NaN) > eps` is false. Reject non-finite actual and expected values explicitly. | PHY |
| G3H4-061 | Render target shell | Ownership | The public backing-shell pointer could be substituted, redirecting reads and finalization. Preserve an authoritative allocation identity and restore the mirror. | RT |
| G3H4-062 | Render target width | Correctness | The wrapper width could be corrupted independently of its allocation. Restore it from immutable construction metadata. | RT |
| G3H4-063 | Render target height | Correctness | The wrapper height had the same independent corruption path. Restore it before every public operation. | RT |
| G3H4-064 | Backing width | Memory safety | A corrupt shell width could make buffer-size calculations disagree with the allocation. Repair it from the allocation dimensions. | RT |
| G3H4-065 | Backing height | Memory safety | A corrupt shell height could likewise invalidate row and buffer calculations. Restore it before backend or CPU access. | RT |
| G3H4-066 | Backing stride | Memory safety | A too-small retained stride caused valid wrapper-owned readback storage to be rejected or misinterpreted. Restore the exact allocation stride. | RT |
| G3H4-067 | Backing format | Correctness | A corrupt color-format tag could switch UNORM/HDR behavior without changing the allocation. Restore the construction format. | RT |
| G3H4-068 | Reservation telemetry | Accounting | Corrupt estimated-byte metadata broke render-target reservation accounting. Preserve and restore the allocation estimate. | RT |
| G3H4-069 | Backend cache key | Correctness | A zero or substituted cache identity could alias or strand backend resources. Preserve and restore the allocated identity. | RT |
| G3H4-070 | Material cache | Ownership | The cached Pixels mirror could be replaced by an unrelated object while the real retained cache leaked. Separate its ownership identity from the mutable mirror. | RT |
| G3H4-071 | Material cache | Memory safety | A malformed owned Pixels payload could be reused using target dimensions, risking invalid data access. Validate its class, payload, dimensions, and embedded data pointer, then rebuild it. | RT |
| G3H4-072 | Target finalizer | Lifetime | Finalization freed the mutable shell mirror, permitting an invalid free and leaking the actual allocation after substitution. Free only the authoritative allocation. | STATIC |
| G3H4-073 | Cache finalizer | Lifetime | Finalization released the mutable Pixels mirror, permitting a wrong-object release and leaking the real cache. Release only the authoritative retained cache. | STATIC |
| G3H4-074 | Width readback | Correctness | `Width` merely clamped corrupt wrapper state to zero. Repair the wrapper and return the allocated width. | RT |
| G3H4-075 | Height readback | Correctness | `Height` merely clamped corrupt wrapper state to zero. Repair it independently and return the allocated height. | RT |
| G3H4-076 | HDR readback | Correctness | `IsHdr` trusted a mutable backing pointer and format. Route it through allocation-state repair. | RT |
| G3H4-077 | Pixels copy | Memory safety | `AsPixels` trusted mutable target identity and layout metadata before copying. Repair the authoritative shell before validating and reading it. | RT |
| G3H4-078 | RGBA readback | Memory safety | `TryReadRgba` shared the same corrupt-shell exposure on its allocation-free path. Repair before its exact-size copy. | RT |
| G3H4-079 | Existing Pixels copy | Memory safety | `CopyTo` could consume corrupt wrapper dimensions and target layout. Route it through the common repair boundary. | STATIC |
| G3H4-080 | Canvas binding | Ownership | `SetRenderTarget` retained and bound a mutable substituted shell. Repair the RenderTarget3D before adopting its owned allocation. | STATIC |
| G3H4-081 | Offscreen canvas | Ownership | Offscreen construction accepted a valid wrapper whose backing pointer had been substituted. Repair and validate its owned shell before backend initialization. | STATIC |
| G3H4-082 | Cubemap +X face | Ownership | The positive-X face mirror could be replaced with an unrelated Pixels object. Restore it from its retained owner before validation, serialization, upload, or sampling. | CUBE |
| G3H4-083 | Cubemap -X face | Ownership | The negative-X face had the same independent substitution path. Preserve and restore its retained identity. | CUBE |
| G3H4-084 | Cubemap +Y face | Ownership | The positive-Y mirror could redirect skybox and irradiance reads. Restore it from ownership state. | CUBE |
| G3H4-085 | Cubemap -Y face | Ownership | The negative-Y mirror independently trusted mutable payload state. Repair it before use. | CUBE |
| G3H4-086 | Cubemap +Z face | Ownership | The positive-Z mirror could substitute different content without changing the cubemap identity. Restore its owner. | CUBE |
| G3H4-087 | Cubemap -Z face | Ownership | The negative-Z mirror exposed the same substitution bug. Repair the sixth slot independently. | CUBE |
| G3H4-088 | Cubemap extent | Memory safety | Corrupt `face_size` metadata could invalidate sampling and backend extent calculations. Preserve and restore the construction extent. | CUBE |
| G3H4-089 | Cubemap cache key | Correctness | A corrupt zero/substituted cache identity could alias backend resources or force false invalidation. Restore the allocation identity. | CUBE |
| G3H4-090 | Cubemap finalizer | Lifetime | Finalization followed mutable face mirrors, permitting wrong-object releases and leaking all six real retains. Release only authoritative source owners. | CUBE |
| G3H4-091 | Cubemap sampling | Correctness | Sharp, unit-direction, and roughness sampling consumed mutable face mirrors. Build their shared validated view only after owner-state repair. | CUBE |
| G3H4-092 | VSCN cubemap save | Correctness | Scene serialization interned mutable substituted faces. Repair the cubemap before collecting or emitting its six texture references. | STATIC |
| G3H4-093 | IBL ready flag | Memory safety | A corrupt nonzero ready byte bypassed lazy preparation and exposed uninitialized metadata. Normalize it from authoritative generated state. | CUBE |
| G3H4-094 | IBL mip count | Memory safety | An unbounded retained mip count drove `SampleIbl` and backend loops beyond the six-level array. Restore the exact generated count before indexing. | CUBE |
| G3H4-095 | IBL base extent | Correctness | A corrupt base-size field made generated mip extents disagree with upload validation. Restore the deterministic prefilter extent. | CUBE |
| G3H4-096 | IBL cache key | Correctness | A zero/substituted IBL identity could alias generated GPU chains. Restore the generation identity from owner state. | CUBE |
| G3H4-097 | IBL irradiance | Numeric | Non-finite or substituted public SH coefficients poisoned per-frame ambient lighting. Restore all 27 finite generated coefficients before frame upload. | CUBE |
| G3H4-098 | IBL face mirrors | Ownership | Generated mip mirrors could be substituted while the real Pixels chain leaked or the backend uploaded unrelated images. Separate all 36 owner slots and restore the mirrors. | CUBE |
| G3H4-099 | IBL generation | Resource safety | Prefiltering published partial output directly into live state and trusted malformed cached owners. Generate transactionally, validate every owned extent/coefficient, discard bad chains, and finalize only owners. | CUBE |
| G3H4-100 | Cubemap test | Test integrity | The IBL test's near-comparison helper accepted NaN just like the physics helper had. Require finite actual and expected operands. | CUBE |

## 3. Validation record

Completed batch validation:

```text
ctest --test-dir build -R '^test_rt_material3d$' --output-on-failure
1/1 passed; 97/97 assertions passed

ctest --test-dir build -R '^test_rt_physics3d$' --output-on-failure
1/1 passed; 979/979 assertions passed

ctest --test-dir build -R '^test_rt_canvas3d$' --output-on-failure
1/1 passed; 299/299 cases passed

ctest --test-dir build -R '^test_rt_cubemap3d_ibl$' --output-on-failure
1/1 passed; 8/8 cases passed

ctest --test-dir build -L graphics3d --output-on-failure -j8
151/151 passed

ctest --test-dir build-g3h4-asan_full -L graphics3d \
  -LE 'slow|native_run' --output-on-failure -j8
147/147 passed under AddressSanitizer

ctest --test-dir build-g3h4-ubsan -L graphics3d \
  -LE 'slow|native_run' --output-on-failure -j8
147/147 passed under UndefinedBehaviorSanitizer

cppcheck --project=build/compile_commands.json \
  --file-filter='*/src/runtime/graphics/3d/*' \
  --enable=warning,performance,portability --error-exitcode=1 \
  --inline-suppr --suppress=missingIncludeSystem --check-level=exhaustive
106/106 compiled Graphics3D translation units clean

./scripts/lint_platform_policy.sh
passed

./scripts/run_cross_platform_smoke.sh --build-dir build
all selected host and cross-platform smokes passed

./scripts/build_zanna_mac.sh
clean warning-as-error build; 1968/1968 default tests passed; platform lint,
runtime-surface audit, smoke, Zanna Studio, and install stages passed

git diff --check
passed
```

Before the Material3D runtime changes, the expanded test reported 20 failures
(77/97 assertions passed). The same executable passes after the repairs.

Before the joint runtime changes, the expanded physics executable reported 32
joint failures once the NaN-safe comparison helper was active. It now passes
all 979 assertions. The complete compiled 3D runtime also produced no cppcheck
warning, performance, or portability diagnostics.

The initial full AddressSanitizer Graphics3D run passed every test except
`native_run_game3d_runfixed_callback_probe`. That probe asks Zanna's Mach-O
object writer to consume sanitizer-generated `SUBTRACTOR` relocation pairs in
`__eh_frame`, which that writer does not support. This is an existing native
object-writer limitation outside the C runtime changed here. The compatible
ASan rerun excluded `native_run` (and the separately exercised `slow` label)
and passed all 147 selected tests. The equivalent UBSan selection also passed
all 147 tests. The normal unsanitized native-run probe passed in the final
1,968-test repository gate.

The ledger contains exactly 100 unique rows, `G3H4-001` through `G3H4-100`,
with no gaps or duplicates. No public runtime C ABI, IL, language, serialized
scene format, dependency, platform-policy, or workflow surface changed, so no
ADR was required.
