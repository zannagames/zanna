---
status: complete
audience: contributors
last-verified: 2026-07-24
---

# Linux platform review — July 2026

This ledger records the comprehensive Linux-specific platform review requested
for Zanna. An item is complete only when its implementation, focused regression
coverage, documentation (when user-visible), and applicable platform gates pass.
The review covers Linux-only adapters plus shared POSIX code exercised by Linux.

Status meanings:

- **Resolved** — implementation and focused verification are present.
- **Open** — evidence confirms an improvement is required; completion evidence
  has not yet been collected.

## Platform policy, build, and test infrastructure

1. **Resolved — remove raw host probes from network tests.** Replace the 15
   `_WIN32` branches in `RTHttpClientTests.cpp`, `RTHttpServerTests.cpp`, and
   `RTHttpsServerTests.cpp` with `RT_PLATFORM_WINDOWS`. Verification:
   `lint_platform_policy.sh --strict`.
2. **Resolved — publish Linux AUTO backend selection atomically.**
   `vgfx_platform_linux_auto.c` read `g_backend` without the mutex used by the
   first-window writer. Use release/acquire C11 atomics so dispatch cannot race
   publication.
3. **Resolved — exercise AUTO dispatch during first-window publication.** Run a
   display-query thread while `test_linux_auto_backend` creates the first live
   window; link the test explicitly with `Threads::Threads`.
4. **Resolved — use an exact Linux CMake predicate for Linux-only graphics tests.**
   Replace `UNIX AND NOT APPLE` with
   `CMAKE_SYSTEM_NAME STREQUAL "Linux"` so another Unix is not silently treated
   as Linux.
5. **Resolved — make strict platform-policy lint a CTest.** The build script runs the
   lint, but CTest only self-tests an empty-candidate case. Register a repository
   strict-policy test with a stable label.
6. **Resolved — audit Linux-only CTests for `requires_linux`.** Add a configuration
   test that rejects Linux-only test definitions lacking the capability label.
7. **Resolved — validate generated host capability exclusivity.** Add compile-time
   coverage proving exactly one of the generated Windows/macOS/Linux host
   capability values is true.
8. **Resolved — make the Linux build wrapper forward arbitrary paths safely.**
   Resolve its own directory without depending on the caller's working directory
   and cover invocation through a symlink/path containing spaces.
9. **Resolved — test empty display environment variables.** AUTO selection treats
   empty `WAYLAND_DISPLAY`/`DISPLAY` as absent; add a no-display diagnostic
   regression so this behavior cannot drift.
10. **Resolved — update Linux architecture documentation.** The platform-differences
    introduction still describes Linux as x86-64-only although the active build
    and native linker support AArch64.

## Entropy and sockets

11. **Resolved — preserve partial `getrandom()` progress on fallback.** If Linux
    returns bytes before reporting an unavailable syscall, continue
    `/dev/urandom` at the current offset rather than overwriting completed work.
12. **Resolved — support sandbox-denied `getrandom()`.** Treat Linux
    `EPERM`/`EACCES` like an unavailable syscall and use `/dev/urandom`, which is
    commonly still available in seccomp-constrained processes.
13. **Resolved — bound `/dev/urandom` reads to `SSIZE_MAX`.** Never pass a request
    that cannot be represented by the `read(2)` return type.
14. **Resolved — guarantee close-on-exec on the entropy fallback.** When
    `O_CLOEXEC` is unavailable at compile time, set `FD_CLOEXEC` and fail closed
    if that cannot be done.
15. **Resolved — preserve the entropy-source error across cleanup.** Save `errno`
    before closing a failed `/dev/urandom` descriptor and restore it for
    diagnostics.
16. **Resolved — validate the full `timespec` domain.**
    `rt_socket_monotonic_ms()` rejects negative nanoseconds but not values at or
    above one billion.
17. **Resolved — saturate monotonic millisecond conversion.** Prevent the
    seconds-to-milliseconds conversion from wrapping `uint64_t`.
18. **Resolved — make pending-socket-error length failures deterministic.** If
    `getsockopt(SO_ERROR)` returns an unexpected length, set `errno = EIO`
    instead of exposing stale state.
19. **Resolved — avoid redundant nonblocking `fcntl` writes.** Return success when
    the requested `O_NONBLOCK` state already matches, reducing failure surface
    and syscalls.
20. **Resolved — cover finite socket-wait clock failure.** Add an injectable clock
    seam or focused helper test proving a failed monotonic query returns `EINTR`
    rather than converting a finite wait into an unbounded one.

## Watcher and pseudo-terminal lifecycle

21. **Resolved — request directory-only inotify watches.** Add `IN_ONLYDIR` for
    directory watcher instances so a path-type race cannot attach incompatible
    semantics.
22. **Resolved — fail closed when inotify close-on-exec setup fails.** On systems
    without `IN_CLOEXEC`, do not publish an inotify descriptor when either
    `F_GETFD` or `F_SETFD` fails; otherwise child processes can inherit it.
23. **Resolved — retire self-moved or deleted inotify watches.** After reporting
    `IN_MOVE_SELF` or `IN_DELETE_SELF`, close the native watch because its
    retained path is stale and subsequent relative paths would be misreported.
24. **Resolved — suppress duplicate write notifications.** Coalesce an
    `IN_MODIFY` immediately followed by `IN_CLOSE_WRITE` for the same path and
    epoch.
25. **Resolved — bound each inotify drain pass.** A continuously mutating
    directory could keep the nonblocking read loop busy indefinitely and starve
    the caller; retain unread kernel events after 64 batches for the next poll.
26. **Resolved — make watcher deadline arithmetic overflow-safe.** Saturate the
    positive `PollFor` deadline instead of overflowing signed microseconds when
    the monotonic clock approaches `INT64_MAX`.
27. **Resolved — propagate PTY nonblocking setup failure.** Make
    `set_nonblocking()` return status and abort construction instead of
    publishing a session whose reads can block indefinitely.
28. **Resolved — retry blocking `waitpid` cleanup on `EINTR`.** Both PTY
    construction-failure paths currently discard an interrupted reap.
29. **Resolved — fail closed when PTY master close-on-exec setup fails.** Do
    not publish a live session if `F_GETFD` or `F_SETFD` fails, otherwise an
    unrelated exec can retain the terminal master indefinitely.
30. **Resolved — retain PTY teardown diagnostics.** Preserve unexpected
    `kill`/`waitpid` failures in the thread-local last-error channel while still
    completing best-effort cleanup.

## Linux machine and container reporting

31. **Resolved — discover the active cgroup v2 mount.** Do not assume every process
    is rooted at `/sys/fs/cgroup`; parse mount information for relocated
    hierarchies.
32. **Resolved — resolve the process's nested cgroup path.** Combine
    `/proc/self/cgroup` membership with the discovered mount before reading
    `cpu.max`, cpuset, and memory controls.
33. **Resolved — handle hybrid cgroup v1 controller mounts.** Resolve cpu, cpuset,
    and memory controls independently instead of assuming the conventional
    combined directory names.
34. **Resolved — reject overlapping or unordered cpuset ranges.** Malformed
    control data such as `0-3,2-4` previously double-counted CPUs; require the
    canonical strictly increasing, non-overlapping form.
35. **Resolved — reject trailing junk in numeric cgroup controls.** The prior
    parser accepted any suffix beginning with a space (for example
    `1024 corrupt`); allow only trailing horizontal whitespace.
36. **Resolved — test fractional CPU quotas.** Pin down the documented rounding
    policy for quotas below and between whole CPUs.
37. **Resolved — distinguish missing cgroup memory controls from zero.** A
    missing `memory.current` previously looked like a valid zero-byte usage and
    could incorrectly constrain host availability to the container limit.
38. **Resolved — tolerate missing `MemAvailable`.** Parse `MemFree`, buffers,
    page cache, reclaimable slab, and shared memory for older/minimal kernels,
    with saturation and fixture-backed coverage.

## Graphics, desktop integration, and accessibility

39. **Resolved — handle Wayland clipboard `poll()` errors.** Stop ignoring the
    result; retry `EINTR`, terminate on descriptor errors, and retain the
    one-second deadline.
40. **Resolved — make Wayland clipboard deadlines overflow-safe.** Compare elapsed
    monotonic time rather than adding one second to an absolute signed value.
41. **Resolved — cap outbound Wayland clipboard size.** Inbound transfers were
    already capped, but local clipboard text and compositor send requests could
    still retain and duplicate an unbounded payload; apply the same 8 MiB limit.
42. **Resolved — preserve transfer errors until consumed.** Distinguish an empty
    clipboard payload from timeout, allocation failure, and peer I/O failure in
    internal state and diagnostics.
43. **Resolved — yield during concurrent portal-loader initialization.** A
    second thread previously busy-spun at full CPU while the first thread ran
    `dlopen`/`dlsym`; yield until the immutable loader state is published.
44. **Resolved — validate all XSettings record arithmetic.** Add malformed-buffer
    tests for alignment overflow, oversized names, truncated values, and mixed
    byte order.
45. **Resolved — reject concurrent AT-SPI mutations.** The bridge has one
    bounded request slot; retain that backpressure contract and return failure
    immediately when it is occupied rather than allowing queue growth.
46. **Resolved — test AT-SPI detach during an in-flight request.** Prove the worker
    cannot dereference a detached bridge/root while a bounded GUI mutation is
    pending.

## Audio, packaging, and native linking

47. **Resolved — check ALSA condition-wait failures.** The audio thread currently
    ignores `pthread_cond_wait`; terminate or report a backend failure on
    unexpected errors.
48. **Resolved — check ALSA prepare/drop recovery results.** Pause, resume, and the
    write-failure backoff discard several ALSA return codes, hiding a permanently
    unusable device.
49. **Resolved — range-check `SOURCE_DATE_EPOCH`.** Linux packaging already rejects
    signs and non-digits but accepts an arbitrarily long digit string; parse and
    reject values outside the supported epoch range before passing them to RPM.
50. **Resolved — make unknown Linux-import diagnostics deterministic.**
   `planLinuxImports()` validates an `unordered_set` directly, so multiple
   unknown imports can report different first errors across library/hash
   implementations. Sort names before validation and test the chosen order.

## Post-review packaging verification

51. **Resolved — execute Linux package smoke tests on Linux.** Script-mode CMake
    does not populate the target-side `CMAKE_SYSTEM_NAME` variable, so the DEB
    and RPM smoke scripts incorrectly reported every Linux host as non-Linux
    and returned before generating an artifact. Use
    `CMAKE_HOST_SYSTEM_NAME` for the runtime host check.
52. **Resolved — register Linux packaging tests only for Linux targets.**
    Replace the broader `UNIX AND NOT APPLE` registration predicate with the
    exact `CMAKE_SYSTEM_NAME STREQUAL "Linux"` capability used by the tests.
    This prevents another Unix target from acquiring Linux-only package tests
    and labels.
53. **Resolved — keep clean Linux package staging compilable.** Once the DEB
    smoke reached its package-staging build, it exposed
    `rt_scene_editor.cpp`'s reliance on a transitive standard-library include
    for `std::array`. Include `<array>` directly so clean Linux builds do not
    depend on implementation-specific header inclusion.
54. **Resolved — give Linux package staging a realistic test budget.** The DEB
    and RPM smoke tests invoke `install-package --build-dir`, which may need to
    complete a broad warning-as-error rebuild before staging. The former
    420/540-second limits were below an observed AArch64 rebuild time. Use a
    shared 900-second ceiling while retaining serial execution.
55. **Resolved — validate actual toolchain dependencies.** The DEB smoke tried
    to infer whether X11 was unconditional by searching `dpkg-deb -I` metadata
    for internal payload library names, which that command never prints. The
    shipped toolchain includes Studio, graphics, and audio, so assert its X11
    and ALSA dependencies explicitly; focused builder tests retain conditional
    dependency coverage for synthetic manifests.
56. **Resolved — tolerate portable `dpkg-deb` listing paths.** Depending on the
    dpkg version, `dpkg-deb -c` prints archive members as either `usr/...` or
    `./usr/...`. Match required payload suffixes without requiring the optional
    dot prefix so a valid package is not rejected by host tool formatting.
57. **Resolved — report artifact smoke success as a CTest pass.** The normal,
    unprivileged DEB/RPM paths build, verify, and inspect complete artifacts,
    then optionally continue into a root-only install/upgrade/removal
    lifecycle. Do not trigger CTest's skip regex after artifact validation
    merely because the opt-in privileged extension was not requested.
58. **Resolved — classify the duplicated standalone asset adapter.** The
    Xenoscape Scenes sound generator contains the same single Windows/POSIX
    directory-creation boundary as the original standalone generator. Record
    that exact probe in the migration baseline so strict lint continues to
    reject any additional raw host checks elsewhere.

### Post-review verification record

- Clean warning-as-error AArch64 Linux build: passed.
- Full default CTest run: 1,936 of 1,936 tests passed; the audio-unavailable
  capability test was correctly skipped on the audio-enabled build.
- Linux DEB toolchain artifact generation, internal verification, dependency
  inspection, and payload inspection: passed.
- Linux self-extracting toolchain bundle generation and execution smoke:
  passed.
- Standalone application DEB, RPM-policy, and AppImage-style bundle unit
  coverage in `test_packaging`: passed.
- RPM toolchain artifact smoke: capability-skipped because this host does not
  provide `rpmbuild`/`rpm`.
- Strict platform-policy lint, runtime-surface audit, cross-platform smoke, and
  scripted non-privileged install: passed.

## Required final verification

When all entries are resolved:

```sh
./scripts/lint_platform_policy.sh --strict
./scripts/run_cross_platform_smoke.sh
./scripts/build_zanna_linux.sh
```

The full build must include all CTests, source-health audit, smoke, and install
stages with no skip variables.

### Verification record

- `./scripts/lint_platform_policy.sh --strict`: passed.
- Clean warning-as-error compilation through `./scripts/build_zanna_linux.sh`:
  passed.
- Linux-focused CTests, including machine/cgroup, watcher, POSIX socket and
  entropy faults, graphics selection, AT-SPI, capability generation, build
  wrapper, and label policy: passed.
- Runtime-surface audit: passed after classifying the cgroup resolver as an
  internal helper rather than exposing an `rt_` ABI symbol.
- Full CTest: 1,920 of 1,920 tests passed.
- `./scripts/run_cross_platform_smoke.sh`: all host-capability slices passed,
  including Zanna Studio and Linux graphics.
- Runtime-surface audit, source-health audit, and scripted install: passed.

During final verification, six pre-existing Studio failures exposed stale
Wayland/X11 resize publication, SplitPane ancestor invalidation, scaled
responsive-width, wrapped-toolbar, directory-watcher degradation, and
viewport-relative overlay issues. Those root causes were repaired and the
original behavioral tests now pass without relaxed layout assertions.

### Post-merge verification

Verification after merging the Linux review with the concurrent Windows and
Studio scene-authoring work found and repaired two additional integration
regressions:

- Windows-only checked path concatenation in `rt_savedata.c` is now scoped to
  the Windows adapter, keeping Linux warning-as-error builds clean.
- Beginning a 2D marquee now uses a bounded one-line status message. The prior
  message could reflow the retained toolbar after pointer capture, move the
  canvas beneath the physical pointer, and corrupt the selected Linux display
  cell.

The monolithic 3D Studio scene probe is classified `slow`: its source/compiler
graph exceeds the routine platform-test budget and its contracts are retained
by the focused rotation, scale-plane, material, viewport, picking, component,
and workspace probes. It remains available through
`ZANNA_RUN_SLOW_TESTS=1`.
