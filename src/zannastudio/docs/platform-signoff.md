# Zanna Studio Desktop Platform Sign-off

This is the release gate for behavior that source review and one-host CI cannot
establish. The machine-readable matrix is
[`tests/platform_signoff.tsv`](../tests/platform_signoff.tsv). It contains one
row for every required case on Windows, macOS, and Linux. No platform is signed
off merely because another platform passed or because a source-level adapter
exists.

## Status contract

Use only these values:

- `pending`: not run for the recorded release candidate.
- `pass`: completed successfully, with a dated evidence link/path.
- `fail`: completed and failed, with a dated evidence link/path.
- `blocked`: could not complete because of a named external condition, with
  evidence and a date.
- `not-applicable`: accepted only for the two platforms that cannot provide a
  backend-specific D3D11, EGL, or Metal leg.

`pass`, `fail`, and `blocked` require non-placeholder `evidence` and
`last_run`. Evidence should identify the build/commit, OS version, filesystem,
graphics adapter/driver where relevant, commands run, and retained logs or
screenshots. A new release candidate resets affected rows to `pending` when its
changes can invalidate the prior evidence.

The checked-in matrix starts honestly at 0/42 applicable passes. It defines the
gate; it does not claim that unavailable Windows or Linux machines were tested
from a macOS development session.

## Validation

Schema and coverage validation is an ordinary focused test:

```sh
ctest --test-dir build -R zia_zannastudio_platform_signoff_manifest --output-on-failure
```

Before release sign-off, run the same probe in strict mode. It exits nonzero
until every applicable row is `pass`:

```sh
./build/src/tools/zia/zia \
  src/zannastudio/src/probes/platform_signoff_manifest_probe.zia -- \
  --manifest src/zannastudio/tests/platform_signoff.tsv \
  --require-complete
```

Run only focused tests while another developer is changing the shared build
tree. For a release candidate, use the repository platform build scripts and
then execute the complete matrix on the produced Studio binary.

## Required scenarios

The manifest covers:

- Native file and folder pickers, including cancellation, Unicode, and long
  paths.
- Coarse-timestamp, same-size external rewrites.
- Malformed UTF-8 and oversized source admission.
- Real credentialed Git push through the platform's external Git credential
  broker or SSH agent, plus rename conflicts and multi-file conflicts. Studio
  must remain noninteractive and must not collect or persist the secret.
- Interactive vim, less, and htop (or the approved Windows equivalent).
- PTY/ConPTY process-tree, drain, handle, and shutdown lifecycle.
- Huge Build/Search/References/Debug output under filtering and resize.
- Debugger struct/class/container expansion and watch management.
- Keyboard focus, native-window return, and Narrator/VoiceOver/Orca coverage.
- D3D11 on Windows, EGL/OpenGL on Linux, and Metal on macOS for headless 3D
  scene rendering, with backend diagnostics and software-reference tolerances.

The procedure column is the minimum acceptance flow. Owners may attach more
focused automated evidence, but automation does not replace the explicitly
interactive portions of a row.
