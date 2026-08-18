---
status: complete
audience: contributors
last-verified: 2026-08-17
---

# Windows Runtime Reliability Audit

This audit covers the Direct3D 11 backend and the Windows-specific runtime adapters for sockets,
entropy, TLS verification, process/ConPTY launch, paths and assets, locale detection, file watching,
large-file I/O, environment access, concurrency, stack safety, graphics, audio, native dialogs,
native builds, UI Automation, installer lifecycle, signing, and demo automation. It is a robustness
pass only: no IL opcode, grammar, verifier rule, or runtime C ABI changed. ADRs 0155, 0196, and
0232 record the native-link cross-layer dependencies required by current MSVC object code.

## Repaired findings

The 2026-07-23 passes added WR-199 through WR-450: 252 concrete repairs, with installer and
Zanna Studio packaging intentionally receiving the largest share. The 2026-07-24 pass adds
WR-451 through WR-502: 52 further Direct3D, native-installer, Windows build, and demo-automation
repairs aimed at alpha-quality failure behavior. The 2026-07-25 pass adds WR-503 through WR-560:
58 remaining Unicode-I/O, persistence, synchronization, Direct3D cache, installer, and
demo-automation repairs. The 2026-07-26 pass adds WR-561 through WR-657: 97 Direct3D resource,
PE/installer, Windows storage, WASAPI, native-dialog, build/publication, and native-link hardening
repairs. A second 2026-07-26 pass adds WR-658 through WR-715: 58 checked-synchronization,
Direct3D object-identity/state, TLS contract, installer-cleanup, and native-link repairs. The
2026-08-01 alpha-hardening pass adds WR-716 through WR-805: 90 shared-preview IPC, Direct3D
frame-transaction, installer/update, Win32 runtime, audio, window-adapter, and validation repairs.
The 2026-08-05 alpha-hardening pass adds WR-806 through WR-880: 75 demo/build confinement,
Direct3D telemetry, process/ConPTY teardown, Win32 input, installer, and WASAPI lifecycle repairs.
The 2026-08-11 alpha-hardening pass adds WR-881 through WR-930: 50 Direct3D state-submission,
Win32 GDI/clipboard cleanup, synchronous execution, shared-preview IPC, installer staging, and
maintenance-handoff repairs.
The 2026-08-14 alpha-hardening pass adds WR-931 through WR-980: 50 Direct3D target-binding,
parallel-batch event ownership, and native-installer UI cleanup repairs.

| ID | Area | Finding and repair |
|----|------|--------------------|
| WR-01 | WinSock startup | A failed `WSAStartup` attempt left concurrent callers spinning on the in-progress state forever. Failed owners now reset the state so waiters retry. |
| WR-02 | WinSock startup | Plain reads of the volatile startup state did not provide the atomic visibility used by its writes. All state observations now use interlocked operations. |
| WR-03 | WinSock startup | A successful call did not verify that WinSock 2.2 was actually negotiated. The adapter now validates `wVersion` and cleans up a mismatched startup. |
| WR-04 | WinSock startup | Failure to register `WSACleanup` with `atexit` was ignored. That failure path was initially made explicit; WR-344 later removes the unsafe Windows registration entirely for CRT-less native executables. |
| WR-05 | WinSock connect | Non-blocking connect classification recognized only `WSAEWOULDBLOCK`. It now also recognizes `WSAEINPROGRESS` and `WSAEALREADY`. |
| WR-06 | WinSock accept | Listener shutdown could surface `WSAESHUTDOWN` as an unexpected accept failure. It is now classified as the normal close race. |
| WR-07 | WinSock readiness | The Windows `select` call derived `nfds` from the pointer-sized socket handle and narrowed it to `int`, although WinSock ignores that argument. It now passes zero. |
| WR-08 | WinSock readiness | Invalid readiness arguments returned `-1` without a deterministic last-error value. They now set `WSAEINVAL` or `WSAENOTSOCK`. |
| WR-09 | OS entropy | A chunked `BCryptGenRandom` failure could leave a fresh prefix followed by stale caller bytes. The complete destination is now securely erased on failure. |
| WR-10 | OS entropy | The 64-bit entropy helper could preserve an old scalar after failure. It now clears the output before requesting entropy. |
| WR-11 | `rt_rand.c` | The text CSPRNG module's independent BCrypt path had the same partial-output exposure. It now securely erases the entire destination on failure. |
| WR-12 | hash seeding | The BCrypt byte count narrowed `size_t` to `ULONG` in one call. Requests are now chunked to the native 32-bit limit. |
| WR-13 | hash seeding | A later hash-seed entropy chunk could fail after earlier chunks had populated the buffer. Failure now securely clears the whole buffer. |
| WR-14 | locale detection | POSIX-style fallback precedence was `LC_ALL`, `LANG`, `LC_MESSAGES`; category-specific `LC_MESSAGES` must precede `LANG`. All adapters now share the corrected order. |
| WR-15 | locale detection | `C.UTF-8`, `C@...`, and suffixed `POSIX` values were treated as real language tags. Sentinel recognition now occurs case-insensitively before encoding/modifier suffixes. |
| WR-16 | locale detection | Malformed, non-ASCII, empty, or overlong subtags passed through to later parsers. A shared helper now validates a strict near-BCP-47 ASCII shape. |
| WR-17 | locale detection | Detection failure could leave stale bytes in the caller's buffer. Every adapter and shared normalizer now clears writable output on failure. |
| WR-18 | TLS CertificateVerify | Null session/data arguments could be dereferenced by the platform verifier. Both Windows and portable implementations now reject them before parsing. |
| WR-19 | TLS CertificateVerify | A declared signature shorter than the remaining handshake payload silently accepted trailing bytes. The framing check now requires an exact length. |
| WR-20 | TLS CertificateVerify | An obsolete CryptoAPI fallback contained a fixed 4096-byte signature buffer and could verify using uninitialized/out-of-bounds data for a larger declared signature. Supported schemes already use CNG, so the dead fallback was removed. |
| WR-21 | process launch | `StartWithEnv` passed a UTF-16 block without `CREATE_UNICODE_ENVIRONMENT`, allowing ANSI reinterpretation. The flag is now always present for the Windows launch path. |
| WR-22 | process launch | Explicit Windows environment entries were emitted in caller order, contrary to the sorted-block contract. Entries are now sorted case-insensitively with deterministic tie-breaking. |
| WR-23 | process launch | Case-insensitive duplicate variable names produced an ambiguous block. The builder now rejects duplicates after sorting. |
| WR-24 | finite waits | Three Future timed operations could map a long finite duration to Win32's `INFINITE` sentinel. They now use a shared saturating absolute deadline and finite wait slices. |
| WR-25 | finite waits | ConcurrentQueue timeout could become indefinite at the sentinel and treated an intermediate maximum-size slice as final timeout. It now recomputes against the absolute deadline and traps on non-timeout wait failures. |
| WR-26 | finite waits | Monitor `TryEnterFor` and `WaitFor` clamped large finite durations to `INFINITE`. Both now retain the full absolute deadline across finite slices. |
| WR-27 | monitor errors | Monitor condition-variable failures were ignored in wait/reacquire loops, risking a spin or corrupt queued stack waiter. Failure paths now remove/requeue fairly, clean up, and trap. |
| WR-28 | thread joins | Timed joins narrowed long durations and could become indefinite; join condition failures were also ignored. Joins now use absolute finite slices and report native wait failure. |
| WR-29 | thread yield | `SwitchToThread` can report that no processor yielded. The adapter now falls back to `Sleep(0)` in that case. |
| WR-30 | file watcher | Packed `FILE_NOTIFY_INFORMATION` offsets and filename extents were trusted. The decoder now validates every record, stride, alignment, and in-buffer next-record target. |
| WR-31 | file watcher | Invalid UTF-16 names and conversion/allocation failures silently dropped events. Strict conversion now emits an overflow/rescan marker, whose dropped count is at least one. |
| WR-32 | file watcher | Wait, overlapped-result, reset, and rearm failures could leave a watcher apparently active with no pending read. Failures now emit overflow and retire invalid handles. |
| WR-33 | file watcher | `CancelIo` only cancels operations issued by the calling thread, and the event/OVERLAPPED storage could be closed before cancellation completed. Teardown now uses `CancelIoEx`, waits for completion, then closes the directory and event handles. |
| WR-34 | D3D11 shaders | Compiler diagnostics were formatted as an unbounded NUL-terminated string even though an `ID3DBlob` is a byte buffer. Logging now honors the blob size and caps diagnostic output. |
| WR-35 | D3D11 shaders | Invalid/empty compile arguments could leave a stale output blob; successful warning blobs were discarded, and success did not require bytecode. The helper now clears output first, validates non-empty inputs, logs warnings, and requires a blob. |
| WR-36 | D3D11 resources | Device creation, compact input layouts, timing-query recreation, and lazy material samplers had partial-COM-output leak or stale-cache paths. They now use centralized/defensive release, reset timing state, and publish sampler cache entries only after complete success with HRESULT diagnostics. |
| WR-37 | Windows native link | TOML float formatting pulled in `strpbrk`, which has no DLL mapping in the fixed native-runtime import surface. A small in-tree marker scan now preserves the zero-dependency native link. |
| WR-38 | D3D11 swapchain | Back-buffer RTV and depth resources were published piecemeal, so a late allocation failure left a partially valid context. Creation is now transactional and publishes only a complete target set. |
| WR-39 | D3D11 swapchain | A successful `GetBuffer`/view call was assumed to return a non-null resource, and the back-buffer descriptor was not checked against the backend contract. Null outputs and incompatible dimensions, format, subresources, or sampling are now rejected with complete cleanup. |
| WR-40 | D3D11 resize | A same-size resize returned immediately even when a previous failure had removed one of the main targets. It now detects and reconstructs incomplete target sets. |
| WR-41 | D3D11 depth probes | A perpetually busy non-blocking staging map kept one probe batch pending forever and starved every later request. Polling is now bounded and stale batches are discarded. |
| WR-42 | D3D11 depth probes | Failed maps and malformed mapped payloads cleared counts but retained the poisoned staging texture. Failure now unmaps when needed, evicts the staging resource, and resets all probe state. |
| WR-43 | D3D11 telemetry | Abandoned or failed timestamp reads cleared flags and immediately reused the same potentially active query objects. The complete query set is now recreated before telemetry resumes. |
| WR-44 | D3D11 readback | Several early failures preserved old caller pixels, while an invalid cached staging descriptor remained cached for every later call. Valid destinations are cleared up front and invalid/failed staging resources are evicted. |
| WR-45 | D3D11 buffers | Dynamic-buffer and staging-texture creation trusted success with a null output and did not release a partial output on failure. Both helpers now require a resource, release partial COM outputs, and preserve the old dynamic buffer until replacement succeeds. |
| WR-46 | D3D11 device loss | Query, buffer, staging, RTV, depth, and map failures often logged only the local HRESULT. These paths now also report the device-removed reason when applicable. |
| WR-47 | Windows path input | Directory conversion retried malformed UTF-8 without `MB_ERR_INVALID_CHARS`, silently redirecting operations to a replacement-character path. All filesystem boundary conversions now reject malformed input. |
| WR-48 | Windows path output | Directory, file, asset-decoder, and executable-path adapters accepted unpaired UTF-16 surrogates and produced lossy UTF-8. They now use `WC_ERR_INVALID_CHARS` and fail deterministically. |
| WR-49 | asset canonicalization | Mounted pack paths used ANSI `GetFullPathNameA`, so names outside the process code page did not round-trip. Canonicalization now uses a growing UTF-16 `GetFullPathNameW` buffer. |
| WR-50 | asset identity | Mounted pack equality used `_stricmp` on UTF-8 bytes, which is neither Unicode-aware nor Windows ordinal comparison. Paths are now converted strictly and compared with `CompareStringOrdinal` semantics. |
| WR-51 | executable path | The second UTF-16-to-UTF-8 conversion result was ignored and the source contract still described the retired ANSI/fixed-buffer implementation. The result is now checked and the documented contract matches the growing Unicode path. |
| WR-52 | ConPTY discovery | Concurrent first use raced on a plain loaded flag and partially initialized function pointers. Optional exports are now resolved through `InitOnceExecuteOnce`. |
| WR-53 | ConPTY text | Malformed UTF-8 in a command, argument, working directory, or environment entry could be replaced or truncated at the Win32 boundary. Launch now fails transactionally on any strict conversion failure. |
| WR-54 | ConPTY environment | Per-entry allocation/conversion failures silently omitted variables, and block allocation failure silently inherited the parent environment. Explicit environments are now all-or-nothing. |
| WR-55 | ConPTY environment | Explicit blocks were emitted in caller order, allowed case-insensitive duplicate names, and had unchecked aggregate-size arithmetic. Entries are now ordinal-case sorted, duplicate-rejected, and overflow-checked. |
| WR-56 | ConPTY startup | Failure after an attribute list was initialized freed its storage without `DeleteProcThreadAttributeList`. Initialization/update are now separate lifecycle stages with matching cleanup. |
| WR-57 | ConPTY handles | HRESULT-returning APIs reported unrelated `GetLastError` state, and a partial pseudo-console output on failed creation was not closed. Diagnostics now preserve HRESULTs and every partial handle is retired. |
| WR-58 | ConPTY lifecycle | Process waits, exit-code reads, and resize failures were incompletely checked; child-allocation failure also closed a terminated process without waiting. The adapter now publishes deterministic failure state, reports resize HRESULTs, and waits for successful termination. |
| WR-59 | captured execution | Failure to remove inheritance from the capture pipe's read end was ignored, allowing a child to keep the reader alive and prevent EOF. Launch now aborts and closes both ends when `SetHandleInformation` fails. |
| WR-60 | process execution | `WaitForSingleObject` and `GetExitCodeProcess` failures could return an uninitialized code or leave an asynchronous process permanently marked running. Wait/exit results are checked and failures become deterministic errors. |
| WR-61 | process environment | Environment pointer/block sizing could overflow, and locale-sensitive `_wcsnicmp` did not match Windows ordinal name semantics. Sizing is checked and ordering/duplicate detection use dynamically resolved ordinal comparison. |
| WR-62 | parallel workers | Failed completion-event signals or waits were ignored, after which caller-owned task arrays and locks could be freed while workers still borrowed them. Such unrecoverable synchronization failures now abort before cleanup. |
| WR-63 | scheduler clocks | Scheduler/debouncer code ignored QPF/QPC failure and could use an uninitialized counter. Results are checked and the Windows path falls back to monotonic `GetTickCount64`. |
| WR-64 | stack safety | `SetThreadStackGuarantee` is per-thread, but only the initializing thread received a reserve. Every thread created through the Windows Threads adapter now establishes its own emergency stack reserve. |
| WR-65 | stack diagnostics | The low-stack exception path dereferenced exception pointers and wrote to stderr without validation, and called `strlen` while stack was exhausted. It now validates inputs/handles and writes a compile-time-sized static message. |
| WR-66 | host integration | Stack-safety initialization replaced the process error mode, clobbering flags chosen by an embedding host. It now preserves existing flags and only adds the two required suppression modes. |
| WR-67 | UI Automation startup | Lazy `uiautomationcore.dll` initialization used an unsynchronized attempted flag and exposed partially filled function pointers. The API table is now initialized once atomically. |
| WR-68 | UI Automation lifetime | Providers borrowed a reusable fixed bridge slot, so a provider retained across detach could resolve against a different window/root attached in that slot. Per-slot generations now invalidate every detached provider. |
| WR-69 | UI Automation tree | Provider construction did not prove that a widget belonged to the bridge root, and later reparenting could leave it representing another tree. Construction and every resolution now validate ancestry and immutable IDs. |
| WR-70 | UI Automation IDs | Runtime IDs retained only 31 bits of a 64-bit widget ID, allowing collisions. The SafeArray now carries the complete low/high 32-bit identity after `UiaAppendRuntimeId`. |
| WR-71 | UI Automation arrays | `SafeArrayPutElement` failures were ignored and a partially populated array could be returned. Every write is checked and failed arrays are destroyed through the dynamically resolved OleAut export. |
| WR-72 | UI Automation text | Accessible UTF-8 and `IValueProvider::SetValue` UTF-16 were converted leniently, and the second conversion result was unchecked. Both directions are strict; malformed provider input returns `E_INVALIDARG`. |
| WR-73 | UI Automation allocation | BSTR or child-provider allocation failure was frequently reported as `S_OK` with a null value. Property, navigation, hit-test, focus, selection-container, and Value paths now return `E_OUTOFMEMORY`. |
| WR-74 | UI Automation geometry | Non-finite/out-of-range hit-test coordinates were narrowed to `LONG`, invalid bounds entered comparisons, and failed client/screen transforms were ignored. Inputs, bounds, and Win32 transforms are now validated. |
| WR-75 | UI Automation ranges | Corrupt non-finite, inverted, or out-of-range slider/progress/spinner state leaked through RangeValue. Published ranges are now finite, ordered, and clamped. |
| WR-76 | UI Automation outputs | Toggle and RangeValue getters could leave caller output unchanged on stale-provider failure. Outputs are initialized to deterministic safe values before resolution. |
| WR-77 | UI Automation events | Live-region announcement allocation failure could pass null BSTRs into UIA. Both strings must now exist before an event is raised. |
| WR-78 | Windows compile | FBX camera-channel locals named `NEAR` and `FAR` collided with Win32 header macros and stopped the MSVC build. Backend-private prefixed names now avoid the global macro namespace. |
| WR-79 | Windows native link | The software wireframe rasterizer called CRT `llabs`, which is absent from the fixed CRT-less native import surface. Its already-widened 32-bit coordinate deltas now use in-tree signed magnitude arithmetic. |
| WR-80 | MSVC portability | Tiled artwork bounds and software texture indexes relied on implicit narrowing/conversion that produced C4244 diagnostics under the Windows warning policy. Range checks now precede explicit conversions at the exact assignment boundaries. |
| WR-81 | D3D11 readback | Resizing the reusable readback staging texture released the last usable surface before allocation. A replacement now stages locally and is published only after a non-null successful creation. |
| WR-82 | D3D11 snapshots | Presented-backbuffer snapshot replacement also destroyed the cached texture before `CreateTexture2D` succeeded. Creation now preserves the old snapshot resource and metadata on failure. |
| WR-83 | D3D11 scene targets | Scene color, motion, and depth attachments were rebuilt directly in the live context; a late depth/view failure discarded the previous complete scene. All nine COM resources now stage and publish as one transaction. |
| WR-84 | D3D11 overlay | Overlay resize evicted its prior texture/RTV/SRV before replacement allocation. The complete replacement is now created before bound state is retired and the old set released. |
| WR-85 | D3D11 post-FX | Primary and scratch post-processing targets had the same release-before-create failure mode. Each target set now stages independently and keeps the last complete resource authoritative on allocation failure. |
| WR-86 | D3D11 bloom | Bloom resize published mip resources one at a time and destroyed both the old chain and a partial new chain after a later allocation failure. The complete mip chain and dimensions now stage in local arrays before one commit. |
| WR-87 | D3D11 TAA | TAA history resize could leave no usable history pair after the second target failed. Both history textures/RTVs/SRVs now stage before the previous pair is replaced. |
| WR-88 | D3D11 SSR | SSR resize released its cached target before replacement creation. Texture, RTV, and SRV now stage as a complete set before publication. |
| WR-89 | D3D11 RGBA cache | Starting a changed RGBA texture upload evicted a known-good cache entry before texture/SRV allocation. Allocation now stages locally, so allocation failure preserves the resident generation. |
| WR-90 | D3D11 native texture cache | Compressed native-texture replacement had the same early eviction window. The replacement texture/SRV must now both exist before cache metadata and ownership change. |
| WR-91 | D3D11 cubemap cache | Cubemap replacement likewise discarded a usable cube before allocation. Cube texture/SRV creation is now staged before entry release. |
| WR-92 | Windows file metadata | High-level file APIs used `_stat64i32`, whose name means 64-bit time but only a 32-bit file size. They now use `_stat64`/`_fstat64`/`_wstat64`, preserving sizes and metadata beyond 2 GiB. |
| WR-93 | Windows file seek | Low-level seek called `_lseeki64` but rejected offsets using Windows' unrelated 32-bit `off_t` range first. Windows now accepts the full `int64_t` offset contract of the actual adapter. |
| WR-94 | Windows IDE files | Workspace modification-time lookup used `_wstat64i32`, which can fail solely because an otherwise valid file exceeds its 32-bit size field. It now uses `_wstat64`. |
| WR-95 | Windows environment | A variable that grew between `GetEnvironmentVariableW`'s size probe and read returned a new required capacity that was misused as the undersized buffer's string length, enabling an out-of-bounds read. The adapter now retries boundedly until one snapshot fits. |
| WR-96 | Windows environment | UTF-8/UTF-16 conversion allocation and output-size arithmetic were unchecked. Both conversion directions now reject `size_t` overflow before allocating or accumulating encoded bytes. |
| WR-97 | Windows environment | `HasVariable` treated every query error except `ERROR_ENVVAR_NOT_FOUND` as proof that the variable existed. Unexpected Win32 errors now trap and return a deterministic false fallback. |
| WR-98 | argument initialization | The Windows legacy-argument once wait ignored a failed `SwitchToThread`, unlike the Threads adapter. It now falls back to `Sleep(0)` so a contended initializer yields deterministically. |
| WR-99 | recursive directory removal | A malformed UTF-16 child name or allocation failure became the canonical empty runtime string; recursion could then resolve `""` as the process cwd. Recursive deletion now requires an explicit successful conversion and rejects empty child paths. |
| WR-100 | directory enumeration | Windows list/files/dirs/entries silently inserted empty names after UTF-16 conversion failure. Non-trapping enumerators now clear the partial result; the trapping `Entries` API reports the read failure. |
| WR-101 | current directory | `Dir.Current` had a size/read race when another thread changed cwd and silently returned empty after UTF-16 conversion failure. A growing snapshot helper retries the Win32 call and conversion failure now traps. |
| WR-102 | deletion protection | Full-path/cwd sizing races could produce an unchecked buffer or make the recursive-delete guard fail open after resolution/allocation failure. Both paths now use growing buffers and every inability to prove safety refuses deletion. |
| WR-103 | deletion identity | Recursive-delete protection compared Windows paths with locale-sensitive `_wcsnicmp`. It now uses dynamically resolved `CompareStringOrdinal` case folding, matching Windows path identity. |
| WR-104 | WinSock shutdown | Shutting down the invalid-socket sentinel returned `SOCKET_ERROR` while preserving an unrelated thread-local error. It now sets `WSAENOTSOCK`. |
| WR-105 | WinSock pending error | `SO_ERROR` was read directly into caller storage, so a failed or short `getsockopt` could partially modify the output. The adapter now stages locally and publishes only an exact successful result. |
| WR-106 | WinSock startup wait | Startup waiters used only `Sleep(0)` while the rest of the runtime prefers `SwitchToThread` with a zero-sleep fallback. The once wait now follows the shared scheduling policy. |
| WR-107 | Windows native imports | The fixed PE import planner recognized only the old 32-bit-size stat variants, so native programs using the corrected `_stat64`, `_fstat64`, or `_wstat64` calls failed to link. The exact Windows-only exports now map to UCRT and remain rejected on other targets. |
| WR-108 | Windows native math | `remainder` was accepted by the shared dynamic-symbol policy but missing from the Windows UCRT planner, breaking the complete native Studio link. Both double and float UCRT exports are now planned and regression-tested. |
| WR-109 | Windows demo validation | The Windows demo driver could build and stage demos but had no launch-smoke mode, unlike its Unix counterparts. `--run` now launches each host-architecture binary with bounded diagnostics and removes only newly created run artifacts. |
| WR-110 | D3D11 shaders | FXC's DXBC validator rejected the shared shadow/light pixel shader because early-return control flow left a temporary component apparently uninitialized. The helpers now initialize one result and return it after structured control flow; real D3D11 probes verify hardware initialization. |
| WR-111 | D3D11 diagnostics | Shader initialization failures shared the short success-warning diagnostic cap, which truncated the validator's actionable error and obscured software fallback. Failures now retain a bounded extended diagnostic while successful compilation keeps the smaller cap. |
| WR-112 | tiled runtime import | Boxed `INT64_MAX` values were converted through `double`, rounded to the exclusive positive limit, and then cast back to `int64_t`. Exact boxed integers now bypass floating-point conversion, and floating values use an exclusive `2^63` upper bound. |
| WR-113 | BASIC Windows input | Horizontal-whitespace skipping consumed carriage returns before the cursor could atomically normalize CRLF. Windows-authored examples consequently lost end-of-line tokens; CR/LF pairs and lone CR characters now each produce one EOL. |
| WR-114 | model-loader tests | A runtime test depended on an untracked website JPEG, making a clean Windows checkout fail independently of product behavior. The test now decodes a tiny known-good embedded JPEG fixture and removes the external tree dependency. |
| WR-115 | Windows demo processes | Windows PowerShell could return a blank `Process.ExitCode` for a fast redirected child because its native process handle was not materialized before the wait. The launch driver now acquires and validates the handle immediately, preserving exact failure codes. |
| WR-116 | Windows native demos | Four more curated demos exposed the known Windows checked-integer optimizer miscompile during launch validation: invalid string handles in 3dbowling/Crackman, invalid pixels in Chess, and early Ridgebound termination. The Windows driver now uses its existing conservative `-O0` policy for every affected demo until that separately tracked compiler defect is resolved. |
| WR-117 | Windows demo cleanup | Launch cleanup snapshotted only top-level names, so a new run artifact nested beneath an existing staged asset directory could survive. Snapshots now track validated relative paths recursively and remove new entries from deepest to shallowest. |
| WR-118 | D3D11 mesh cache | Replacing a static mesh evicted its usable vertex/index pair before both immutable buffers existed. Both buffers now stage locally and commit together. |
| WR-119 | D3D11 morph cache | Position deltas were published before optional normal deltas, so a late allocation failure destroyed a complete morph entry. A full replacement entry now stages and publishes as one transaction. |
| WR-120 | D3D11 opaque depth | Opaque-depth resize released the old texture/SRV before replacement allocation. The pair now stages locally and preserves the previous target on failure. |
| WR-121 | D3D11 RTT | Render-to-texture replacement destroyed color, depth, and readback resources before the new set was complete. All five resources now stage before one commit. |
| WR-122 | D3D11 shadow slots | Per-light shadow resize discarded a valid depth texture/DSV/SRV before replacement creation. The complete slot now stages before eviction. |
| WR-123 | D3D11 shadow atlas | Atlas resize released its usable texture and views before the replacement was complete. The new atlas now stages transactionally and resets completeness only at commit. |
| WR-124 | D3D11 shadow binding | Atlas replacement could release a DSV still bound as the active output-merger target. Active atlas passes now unbind output targets before release. |
| WR-125 | D3D11 target factories | Color, depth, and staging helpers trusted successful HRESULTs with null COM outputs and leaked partial outputs. Every required resource/view is now validated and partial state is released. |
| WR-126 | D3D11 snapshots | Presented-backbuffer capture trusted a successful `GetBuffer` with a null texture. The path now normalizes this broken COM contract to `E_POINTER` and cleans up. |
| WR-127 | D3D11 resource factories | Static buffers, float SRV buffers, RGBA textures, native compressed textures, and cubemaps could accept missing successful outputs. The shared allocation boundaries now reject null resources/views without disturbing live cache entries. |
| WR-128 | Win32 allocation | The aligned allocator accepted non-power-of-two alignments after silently clamping small values. Invalid zero/non-power-of-two requests are now rejected before normalization. |
| WR-129 | Win32 framebuffer | DIB recreation destroyed the current bitmap before replacement creation and ignored partial handles or `SelectObject` failure. It now stages and selects a complete DIB before retiring the old one. |
| WR-130 | Win32 input | Raw-input parsing accepted short packets, and file-drop path allocation failure silently lost an event. Packet size/header framing is exact and allocation failure now emits the overflow contract. |
| WR-131 | Win32 window state | `SetWindowLongPtrW` failure while attaching runtime state was ignored, leaving a live HWND with no safe WndProc context. Creation now checks the zero-result/last-error contract and tears down on failure. |
| WR-132 | Win32 relative input | Destroying a relative-mouse window left raw-input registration and cursor clipping active. Teardown now unregisters the mouse device, releases the clip, and clears both mode flags. |
| WR-133 | Win32 event dispatch | A failed message wait was reported as an available event, while lazy DWM resolution raced between presentation threads. Wait failures are explicit and `DwmFlush` resolution now uses `INIT_ONCE`. |
| WR-134 | Win32 graphics timing | QPF/QPC and waitable-timer wait failures were unchecked. Frequency is initialized once, clock failure falls back to `GetTickCount64`, and failed timer waits fall back to `Sleep`. |
| WR-135 | Win32 clipboard | The fallback text and owner HWND were unsynchronized, and fallback allocation failure erased the last usable text. An SRW lock protects shared state and replacement publishes only after allocation. |
| WR-136 | Win32 clipboard | External `CF_UNICODETEXT` was treated as unbounded NUL-terminated memory. Reads now honor `GlobalSize`, use an in-tree bounded terminator scan, and convert only the validated UTF-16 span; this also avoids the unavailable MSVC `_Avx2WmemEnabled` dependency, which the native planner now rejects. |
| WR-137 | Win32 clipboard | Clipboard contents were cleared before UTF-8 validation, conversion, or allocation succeeded. The complete HGLOBAL is now prepared first and `EmptyClipboard` failure is checked. |
| WR-138 | WASAPI rendering | Format conversion wrote only logical samples, leaving channel padding or unsupported channel bytes stale; size arithmetic was also unchecked. The complete acquired frame span is overflow-checked and zeroed before mixing. |
| WR-139 | WASAPI worker | Unexpected wait results, impossible padding, and successful null render buffers could underflow counts or publish garbage. Each native contract is validated and failures become deterministic silent/error paths. |
| WR-140 | WASAPI startup | Initialization returned success before the worker established COM, successful COM calls could return null interfaces, a zero buffer size was accepted, and timer calls were unchecked. A readiness handshake, output validation, and monotonic fallback close those gaps. |
| WR-141 | native file dialogs | Win32 dialog paths and labels used lenient UTF conversion and ignored the second conversion result. Both directions now reject malformed text and require exact conversion. |
| WR-142 | native file dialogs | A process-global unsynchronized availability/COM latch was reused across apartments, while option and result HRESULTs or partial outputs were ignored. Every public call now owns a balanced thread-local COM scope and cleans every partial interface/string. |
| WR-143 | drawn file dialog | Narrow borrowed environment pointers, first-logical-drive fallback, and pre-epoch FILETIME subtraction produced races, floppy-drive roots, or timestamp underflow. Retrying UTF-16 snapshots, once-only system-drive discovery, saturation, and the matching `GetWindowsDirectoryW` native import now define the behavior. |
| WR-144 | machine runtime | Fixed buffers and borrowed `_wgetenv` pointers truncated/raced user, home, and temp values; root `C:\\` became drive-relative `C:`, and processor counts ignored other groups. Growing UTF-16 snapshots preserve roots, `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)` reports the host, and its Kernel32 import is available to native output. |
| WR-145 | installer defaults | Environment-folder reads raced size changes and failed known-folder calls could leak a partial COM allocation. Reads now retry boundedly and every returned `PWSTR` is released. |
| WR-146 | installer clipboard | Diagnostic-copy size arithmetic could overflow and the current clipboard was cleared before allocation succeeded. The payload is overflow-checked and fully prepared before opening/emptying the clipboard. |
| WR-147 | installer folder picker | Destination text reads and folder-picker mutations/results were incompletely checked, including successful null or failed partial outputs. Exact text results, HRESULTs, and every COM/PWSTR lifetime are now validated. |
| WR-148 | installer registry | General string queries trusted a stale size/type and an external NUL, so concurrent changes or malformed values could truncate or overread. Reads now retry `ERROR_MORE_DATA`, cap and align bytes, revalidate type, and bound the terminator. |
| WR-149 | installer PATH rollback | PATH snapshots had the same size race and could restore the wrong registry type. Snapshot reads now retry and preserve the type returned with the successful payload. |
| WR-150 | installer elevation | Elevated launch assumed a process handle and ignored wait failure. The lifecycle now requires the handle and accepts only a signaled wait before reading the exact exit code. |
| WR-151 | installer cleanup launch | Detached-helper inspection ignored wait/exit failures and unchecked termination, which could let a helper outlive cleanup state. Inspection records exact errors and unsuccessful self-delete requires confirmed termination. |
| WR-152 | cleanup helper | Parent-open failure was conflated with an already-exited parent, timeout hid other wait errors, and executable-path growth mishandled exact-fit buffers. Exact Win32 status is returned and path discovery is bounded with correct truncation rules in both helper and host. |
| WR-153 | installer signing | Timestamp URLs accepted embedded credentials or fragments. Signing now permits only credential-free absolute HTTPS endpoints with a host. |
| WR-154 | installer signing | In-place signing and direct metadata writes could corrupt or replace a known-good release after signer/verifier failure. Artifact and metadata are staged in the destination directory, verified, published in order, and always cleaned. |
| WR-155 | Windows demo architecture | Build type was unchecked and host detection missed native ARM64 when invoked from an emulated shell. The driver validates configurations and honors `PROCESSOR_ARCHITEW6432`. |
| WR-156 | Windows demo tool discovery | The driver assumed a multi-config executable path and reused CMake trees without checking declared architecture. It now resolves multi- and single-config layouts and rejects incompatible caches before reuse. |
| WR-157 | Windows demo manifests | Asset/project paths could escape their roots and duplicate or unsafe manifest names could overwrite outputs. Lexical confinement, safe-name rules, and case-insensitive duplicate rejection now protect staging. |
| WR-158 | Windows CTest scheduling | The editor hot-path probe enforced 250 ms wall-clock budgets while sharing the host with up to seven unrelated tests, producing a clean-build-only false failure. The absolute-timing probe now runs serially. |
| WR-159 | D3D11 telemetry queries | Frame-timing query creation trusted successful HRESULTs with null objects. Every required query now passes through the shared successful-null normalizer before it can be published. |
| WR-160 | D3D11 resource factories | Rasterizer and constant-buffer factories could return nominal success without a usable object, allowing a null cache entry or later dereference. Both factory boundaries now require their output. |
| WR-161 | D3D11 fallback resources | White 2D/cube textures, their SRVs, and the BRDF lookup texture/SRV all trusted successful-null creation. Each mandatory fallback resource is now validated before initialization continues. |
| WR-162 | D3D11 pipeline state | Required depth-stencil, blend, and sampler states accepted successful-null driver outputs. Context creation now rejects every missing state at the call that created it. |
| WR-163 | D3D11 startup assets | Shader objects, required and optional input layouts, and the skybox vertex buffer could publish null outputs after successful HRESULTs. All startup assets now normalize that contract to `E_POINTER`; optional compact layouts remain optional only after an actual failure is handled. |
| WR-164 | D3D11 device creation | `D3D11CreateDeviceAndSwapChain` success was accepted without proving that the swapchain, device, and immediate context trio existed. Initialization now requires all three outputs before continuing. |
| WR-165 | D3D11 DXGI outputs | Factory-parent lookup and the dedicated readback `GetBuffer` path trusted successful-null outputs and did not defensively release a failed partial output. Both paths now validate and retire partial interfaces. |
| WR-166 | D3D11 presentation | Non-failing DXGI status codes such as occlusion were treated as proof that a captured backbuffer reached the display. Only `S_OK` now publishes a presented-frame snapshot; all other statuses invalidate it. |
| WR-167 | D3D11 scene replacement | Publishing a newly staged scene called the full teardown routine, unnecessarily discarding the valid native-size overlay before the route was complete. Scene-only teardown is now separate and preserves the overlay transactionally. |
| WR-168 | D3D11 render scale | Scale changes destroyed the last usable scene route before replacement allocation and committed the requested scale before resources existed. Overlay and scene resources now become ready first, and only then is the scale committed. |
| WR-169 | D3D11 route repair | Same-scale requests returned success even when route resources were missing, while returning to native scale could retain an unused offscreen route and stale temporal/present state. Calls now repair incomplete routes, retire unnecessary targets, and invalidate dependent state. |
| WR-170 | Windows TLS CA paths | Custom CA files were opened through the narrow CRT path boundary, failing for names outside the active ANSI code page. Paths now convert strictly from UTF-8 and open with `_wfopen`. |
| WR-171 | Windows TLS CA sizing | CA loading used 32-bit `long` seek/tell results and had no file-size limit. It now uses 64-bit CRT offsets and rejects bundles larger than 16 MiB before allocation. |
| WR-172 | Windows TLS CA snapshots | A CA file that grew after sizing was silently accepted as a verified prefix. The loader now requires exact content and rejects any trailing byte or read error after the snapshot. |
| WR-173 | Windows TLS CA framing | Null stores/paths and embedded NUL bytes in PEM input were not rejected at the parser boundary, permitting leaked allocations or truncated interpretation. Inputs are validated before reading and PEM must be NUL-free. |
| WR-174 | Windows TLS PEM parsing | A valid certificate followed by a truncated, undecodable, unallocatable, or unaddable PEM block still accepted the earlier prefix. Bundle parsing is now all-or-nothing. |
| WR-175 | Windows TLS CA count | PEM bundles had no certificate-count ceiling. Custom stores now reject more than 1,024 certificates before unbounded decode/store work. |
| WR-176 | Windows TLS DER input | Raw DER bundle length was narrowed to CryptoAPI's `DWORD` without a boundary check. Oversized input is now rejected before the cast. |
| WR-177 | Windows TLS chain input | `tls_verify_chain(NULL)` could dereference its session, and an oversized leaf length narrowed into `CertCreateCertificateContext`. Null sessions and leaf sizes beyond the CryptoAPI contract now fail deterministically. |
| WR-178 | Windows TLS chain construction | Intermediate lengths could narrow, and failed chain-engine/chain-context APIs could leave partial outputs allocated. Lengths are checked and all failed partial outputs are released. |
| WR-179 | Windows TLS CertificateVerify | Oversized leaf lengths and successful-null CNG key imports could reach CertificateVerify processing; failed imports could also leak a partial key. Bounds and required key outputs are now enforced with cleanup. |
| WR-180 | installer update configuration | Supplying only part of the manifest URL/modulus/exponent tuple silently disabled update verification. Only a wholly absent tuple is unconfigured; partial security configuration is now an error. |
| WR-181 | installer update key modulus | The pinned RSA modulus had no public-boundary size or canonical-hex validation. Verification now requires a lowercase, non-zero-leading 2,048- to 4,096-bit modulus. |
| WR-182 | installer update key exponent | The pinned exponent could be oversized, non-canonical, even, or otherwise invalid before CNG import. It is now bounded to 32 bits, minimally encoded, odd, and at least three. |
| WR-183 | installer update signatures | Signature text was decoded and allocated before proving its encoding or expected RSA width. It must now be lowercase hex with exactly the pinned modulus length. |
| WR-184 | installer update digests | Download SHA-256 text was decoded before enforcing its exact representation. Manifests now require exactly 64 lowercase hexadecimal characters. |
| WR-185 | installer update URLs | Raw backslashes and spaces were left for WinHTTP canonicalization, allowing the signed URL text and requested resource to differ ambiguously. Such characters are now rejected before parsing. |
| WR-186 | installer update origins | Same-origin comparison lowercased hosts with the process locale. Host identity now uses Windows ordinal case-insensitive comparison without mutation. |
| WR-187 | installer update transport | The update session did not explicitly require the supported TLS floor or enable certificate revocation checks. WinHTTP now requires TLS 1.2 and enables SSL revocation before sending. |
| WR-188 | installer update reads | A corrupted or shimmed `WinHttpReadData` byte count could exceed the supplied buffer yet still be appended. Returned counts are now bounded by both the buffer and manifest cap. |
| WR-189 | installer path identity | Lifecycle path comparisons and mutex/cache hashes depended on `towlower` and the process locale. Comparisons now use ordinal Windows semantics and hashes use invariant case folding over preferred separators. |
| WR-190 | installer protected roots | Windows-directory and protected-known-folder lookup failures made destination protection fail open, and partial known-folder outputs could leak. Resolution is now mandatory and every returned allocation is released. |
| WR-191 | installer registry boundary | Registry opens could report success with no key, malformed string queries were conflated with missing values, and string-write size/type/NUL constraints were unchecked. The adapter now validates handles, exact reads, and bounded writes. |
| WR-192 | installer reparse defense | Unexpected `GetFileAttributesW` errors on a destination ancestor were treated like a nonexistent path, so safety could not actually be proven. Only file/path-not-found is skippable; every other error fails closed. |
| WR-193 | installer metadata reads | Transaction and ownership text files were read without a bound, and unreadable files were conflated with absent files. Reads now require a regular file, cap it at 32 MiB, and consume one exact snapshot. |
| WR-194 | installer atomic writes | Staged metadata used stream flush only and could leave temporary files after write/flush exceptions. Native writes now loop exactly, call `FlushFileBuffers`, publish with write-through, and clean failed staging. |
| WR-195 | installer recovery journal | Journal state was selected by substring, so corrupt or contradictory text could drive destructive recovery; a missing journal could also discard a preserved old tree. Parsing now requires the exact schema and retains ambiguous transactions. |
| WR-196 | installer PATH updates | PATH mutation used a lossy optional query, overwrote malformed reads with an empty value, forced `REG_EXPAND_SZ`, and compared entries with locale folding. It now uses the exact snapshot reader, preserves the existing type, and compares ordinally. |
| WR-197 | installer Shell Links | Successful-null Shell Link/persistence interfaces could be dereferenced, and getter buffers were assumed to contain a terminator. Required COM outputs and bounded NUL termination are now verified before ownership matching. |
| WR-198 | installer shortcut cleanup | Missing shortcut records still triggered parent cleanup, directories/reparse points could be removed as links, and a desktop shortcut could cause the Desktop root itself to be removed if empty. Cleanup now skips absent links, requires plain files, and protects all shell roots. |
| WR-199 | installer metadata text | Unbounded or malformed UTF-8 product, component, and UI strings could reach native conversion and allocation. Metadata now requires canonical UTF-8 and practical byte limits at the public package boundary. |
| WR-200 | installer metadata identity | Identifier, channel, and path folding depended on the process C locale. The schema now uses explicit ASCII classification and folding for its ASCII-defined fields. |
| WR-201 | installer metadata paths | Backslash/slash aliases bypassed duplicate detection, while reserved devices, invalid leaf characters, and trailing dots/spaces were accepted below the first segment. Every segment now follows Windows leaf rules and duplicate keys normalize separators. |
| WR-202 | installer metadata cardinality | The generic record ceiling still allowed impractical component, payload, outer-file, shortcut, and association counts to drive UI/native work. Each typed collection now has a purpose-sized ceiling. |
| WR-203 | installer metadata sizing | Per-component installed-size accumulation could wrap before comparison with the declared total. Every addition is now checked before committing the sum. |
| WR-204 | installer metadata URLs | Public/update URLs accepted credentials, fragments, XML-breaking characters, backslashes, whitespace, and ambiguous non-ASCII authority text after a prefix-only HTTPS test. A bounded credential-free absolute HTTPS shape is now required. |
| WR-205 | installer update key metadata | A nominally 2,048-bit modulus could have its high bit clear or be even, yielding a shorter or invalid RSA key. Metadata now requires the declared bit width and an odd modulus. |
| WR-206 | installer integration metadata | PATH or shortcut defaults could be enabled without the records needed to implement them. Enabled integrations now require their matching path or shortcut inventory. |
| WR-207 | installer payload ownership | Main, association, display-icon, shortcut-target, and shortcut-icon paths could name files absent from the signed payload. Every executable/icon integration reference must now resolve to a payload record. |
| WR-208 | installer lifecycle metadata | State and ownership-manifest paths could collide with each other or a payload file. These installer-owned control files now require distinct, unowned destinations. |
| WR-209 | installer shortcut metadata | Shortcut records allowed non-`.lnk` destinations, unowned targets/icons, and oversized UI text. The schema now constrains the link suffix, payload ownership, and display fields. |
| WR-210 | installer association metadata | Extensions and ProgIDs could inject registry separators, while MIME/description/argument strings were effectively unbounded and shell metacharacters were accepted. Registry names and command arguments now use strict bounded grammars. |
| WR-211 | installer host snapshot | The setup executable was read without a practical package ceiling or a final growth check. Loading now caps the executable at 2 GiB and proves the read consumed one exact snapshot. |
| WR-212 | installer ZIP boundary | EOCD search accepted a valid-looking record before trailing appended bytes. The selected EOCD must now terminate the embedded archive exactly. |
| WR-213 | installer ZIP structure | Multi-disk, ZIP64-sentinel, contradictory entry-count, or central-directory ranges were not rejected at the outer package boundary. The host now requires one supported disk and an exact bounded central directory. |
| WR-214 | installer outer inventory | Archive iteration stopped once required hashes were found, so extra files or missing late control files could escape the inventory contract. The complete archive is now consumed; only declared files, their ancestors, and the writer-owned empty `app/` marker are allowed, while every mandatory record must appear. |
| WR-215 | installer host narrowing | UTF conversion sizes and command-line quote expansion could overflow native `int`/`size_t` fields. Both boundaries now reject unrepresentable lengths before allocation or conversion. |
| WR-216 | installer logging | Existing-log size, BOM writes, UTF-8 write widths, and presentation callbacks were incompletely checked; a log failure could also suppress wizard progress. Logging now validates exact native results and progress remains best-effort and exception-contained. |
| WR-217 | installer startup | COM and common-controls initialization failures were ignored before the wizard used them. Operational modes now fail closed while help and the launch self-test remain available for diagnostics. |
| WR-218 | installer automation paths | `/output` and `/log` could alias each other or the running installer through normalization or hard links, risking self-overwrite and corrupt mixed output. Lexical and existing-file identity checks now reject those collisions. |
| WR-219 | installer fatal reporting | Fatal UTF conversion, dialog display, or stderr writes could throw or accept impossible native byte counts while already handling an exception. Diagnostics now have a noexcept fallback and exact bounded write semantics. |
| WR-220 | installer update startup | A partial update URL/key tuple was validated only after networking began. The complete pinned configuration is now proven before any request is opened. |
| WR-221 | installer update CNG | Runtime key import repeated the metadata modulus weakness, trusted successful-null CNG outputs, and reusable wrappers could overwrite live handles. Full-width odd keys and reset-before-output RAII are now enforced. |
| WR-222 | installer update manifest | Manifest text fields did not all pass strict UTF-8 conversion, HTTP status-query failure was conflated with a bad status, and JSON inspection omitted the signed release-notes URL. These outputs are now distinct, strict, and complete. |
| WR-223 | installer wizard actions | Failed hyperlink launches were silent and button-vector size narrowed unchecked to `UINT`. The wizard reports shell launch failure and bounds native action counts. |
| WR-224 | installer wizard commit | Custom choices were published before `DestroyWindow` succeeded, and a failed default-folder scope switch left radio, path, and elevation shield inconsistent. Choices now stage until close succeeds and scope changes roll back visibly. |
| WR-225 | installer wizard lifetime | User-data publication, DPI window adjustment, control construction, and the message loop ignored native failures and could leak the custom window/font on exceptions. Exact API checks and a scope guard now own the entire dialog. |
| WR-226 | installer wizard integrations | Disabled PATH, association, or shortcut controls could retain a checked value and be committed through stale options. Capability-disabled integrations are now forcibly unchecked. |
| WR-227 | installer wizard progress | Thread construction/callback setup exceptions could escape a native callback, and unchecked posted completion messages could leave the modal progress dialog hung forever. Failures are captured and synchronous completion closes the dialog deterministically. |
| WR-228 | installer wizard finish | Throwing filesystem status checks could turn an already successful installation into a fatal finish-page error. Optional launch/sample actions now use non-throwing status queries. |
| WR-229 | installer registry settings | Missing, unreadable, and malformed `REG_DWORD` values all became the same default, potentially enabling a destructive maintenance plan from corrupt state. Only a genuinely absent value is optional; type, size, and query errors fail closed. |
| WR-230 | installer elevation query | Token API failure was reported as “not elevated,” causing an unnecessary or misleading relaunch. Elevation inspection now distinguishes native failure from a confirmed non-elevated token. |
| WR-231 | installer destination probe | The writable-parent probe used one predictable name, so a coincidental/stale file falsely made a writable destination fail. A bounded high-resolution nonce loop now retries only name collisions. |
| WR-232 | installer component upgrades | Retired component IDs in an older install record made every later upgrade fail, while explicit component casing was not normalized consistently. Stored selections are intersected with the new package; explicit unknown choices still fail. |
| WR-233 | installer integration upgrades | Persisted PATH/association/shortcut settings could remain enabled after the new package removed the corresponding capability or selected executable component. Plans now clamp settings to the actual selected payload and metadata. |
| WR-234 | installer lifecycle paths | Recovery-owned paths did not reject trailing dots/spaces or device names, and semantic-version identifiers used locale-sensitive alphanumeric classification. Lifecycle validation now mirrors Windows path rules and ASCII SemVer grammar. |
| WR-235 | installer disk preflight | Failure to read an existing entry's attributes was treated like a non-reparse file, so disk preflight could proceed without proving tree safety. Attribute failure now aborts before traversal or sizing. |
| WR-236 | installer build wrapper | `--help` triggered a build, and equals-form or explicit `--build-dir`/existing-input options could accidentally build/package the default tree too. Argument classification now occurs before build work and recognizes both supported spellings. |
| WR-237 | installer build configuration | Rooted build directories retained ambiguous `..` forms and unsupported Debug-like configurations failed only after expensive packaging work. Paths are canonicalized and the wrapper accepts only Release or RelWithDebInfo. |
| WR-238 | Zanna Studio packaging | Quoted Studio CMake settings could be misdetected and a supposedly Studio-enabled build was not checked for its executable/build identity. The wrapper parses the last explicit setting and requires both outputs when Studio is enabled by default. |
| WR-239 | Windows demo entry point | The established `build_demos_win.cmd` contract was absent. A tested logic-free shim now forwards every argument and exact exit status to canonical PowerShell; ADR 0113 records the narrow compatibility exception. |
| WR-240 | D3D11 mip validation | Invalid texture dimensions returned one mip, allowing bad callers to look superficially valid. The D3D11 helper now returns zero so invalid dimensions fail closed. |
| WR-241 | D3D11 cache growth | One frame with unique texture/cubemap identities could grow CPU cache tables toward `INT_MAX` before age pruning ran. Hard entry ceilings now divert excess one-frame entries through existing temporary/fallback resources. |
| WR-242 | D3D11 frame protocol | Nested begin, duplicate end, and depth probes outside an active frame could corrupt timing, history, or probe batches. Begin/end/probe hooks now enforce the active-frame state machine. |
| WR-243 | D3D11 presentation protocol | Present or split post-FX could run during drawing, replay an already-presented frame, or consume stale targets. Both paths now require an ended frame with one pending presentation. |
| WR-244 | D3D11 resize | Scene/overlay targets were destroyed before `ResizeBuffers`, so a rejected DXGI resize discarded a valid independent route; resize could also run mid-frame. Scene teardown now follows successful resize, active frames reject mutation, and an accepted size change explicitly retires any superseded pending frame. |
| WR-245 | D3D11 post-FX planning | A bad final target was discovered only after intermediate passes, and single-effect offscreen capture allocated an unnecessary scratch texture. Destination validation is upfront and scratch allocation follows the actual ping-pong count. |
| WR-246 | D3D11 mutable state | Post-FX routing, render-target changes, and shadow operations could mutate the pipeline mid-frame or outside a frame, while frame-serial wrap broke age ordering. Hooks now honor frame ownership and serials saturate instead of wrapping. |
| WR-247 | Win32 fullscreen | Style/rect/monitor calls and both `SetWindowLong`/`SetWindowPos` phases were unchecked, and fullscreen state was published before success. Entry/exit now snapshot exactly, validate monitor bounds, roll back partial writes, and commit state last. |
| WR-248 | Win32 window/input | Older hosts had no DPI-awareness fallback, while unchecked size/coordinate transforms, drifting cursor visibility, and non-transactional raw-input clipping produced virtualized geometry or stuck-pointer states. A legacy system-DPI fallback, bounded dimensions, checked transforms, convergent visibility, and clip rollback now define the adapter. |
| WR-249 | Windows native link | Current MSVC toolsets can emit the UCRT `_fdtest` helper for Studio float classification, but the fixed import planner had no owner mapping and rejected the native link. The helper now maps to the selected system UCRT under ADR 0155 and remains excluded from non-Windows planners. |
| WR-250 | Windows runtime test portability | The stable-file-identity regression called the retired `_link` CRT spelling, which current MSVC headers do not declare and which blocked the warning-as-error suite before exercising the runtime. The fixture now uses the portable non-throwing `std::filesystem::create_hard_link` contract on every host. |
| WR-251 | Studio asynchronous paths | Windows `FileIndex` pages publish forward-slash paths while open documents use native separators, so completed bind/reference queries could no longer match their originating files. Both workers now canonicalize every stored and published path before identity checks, reads, and UI handoff. |
| WR-252 | Studio high-DPI welcome | Responsive welcome breakpoints compared framebuffer widget dimensions with logical layout thresholds, leaving compact secondary content clipped on a 200% Windows display. Width and height policy now use the widgets' effective logical dimensions. |
| WR-253 | Studio panel regression | The compact Search probe asserted immediately after its controller changed the splitter, before a layout frame could arrange the reserved results viewport. It now renders the controller-authored split before inspecting geometry. |
| WR-254 | Studio zoom regression | The wide-at-200%-zoom fixture required a window wider than Win32's native maximum tracking width on a 200%-DPI monitor, so it could never reach the layout it purported to test. A 150% fixture still detects double scaling while remaining achievable on high-DPI Windows. |
| WR-255 | Studio index concurrency test | The snapshot reader could remain unscheduled until after the mutator destroyed its index, making a concurrency regression fail without executing one query. A condition-variable handoff proves reader progress before mutation and destruction. |
| WR-256 | Studio phase regression | One 880-line probe function generated a verifier workload that exceeded 270 CPU-seconds in Debug and timed out before `main` ran. Focused helper functions preserve every assertion while reducing the same test to about 15 seconds. |
| WR-257 | Studio file-tree regression | The comprehensive file-tree probe creates hundreds of files and exercises paging, mutation, trash, and multi-root behavior under a display lock, but its 30-second budget was shorter than its full-suite runtime under CPU and filesystem contention. Its normal-suite timeout is now 90 seconds while retaining the same assertions and coverage. |
| WR-258 | Windows Release native link | Release optimization can emit UCRT's `_fdclass` helper even when a Debug Studio link only needs `_fdtest`. The exact helper is now accepted only on Windows, mapped to the selected UCRT, and covered beside `_fdtest` by platform import tests and ADR 0155. |
| WR-259 | D3D11 frame begin | Failed target validation unconditionally cleared pending-present state, discarding an earlier completed pass. Begin now blocks nesting, permits the documented multi-pass Begin/End sequence before Flip, and restores prior pending state when a continuation cannot start. |
| WR-260 | D3D11 frame targets | Frame setup could proceed without an immediate context or usable color target and leave active/pending bits latched. It now validates the complete draw route and rolls the protocol back on failure. |
| WR-261 | D3D11 draw submission | Regular and instanced draws accepted calls outside `BeginFrame`/`EndFrame` or without a complete color target. A shared readiness policy now rejects those submissions. |
| WR-262 | D3D11 shadow isolation | Ordinary color draws could execute while the shadow pass owned the output stage. The shared submission policy now reserves shadow rendering for its dedicated entry point. |
| WR-263 | D3D11 skybox submission | Skybox rendering checked target geometry but not active-frame, device-context, or shadow-pass ownership. It now uses the same full color-draw contract. |
| WR-264 | D3D11 same-size repair | Repairing an incomplete same-size swapchain route released the surviving targets before replacement was known to succeed. Rebuild now stages a complete target set before publication. |
| WR-265 | D3D11 RTT selection | Failure to allocate a newly selected render target destroyed the previously usable RTT route. Failed selection now releases only staged resources and preserves the old route. |
| WR-266 | D3D11 RTT ownership | Switching or unbinding a dirty render target could discard its only GPU color copy. Selection now requires a successful readback before retiring the old target. |
| WR-267 | D3D11 RTT staging | A failed or successful-null RTT map left the same poisoned staging texture cached for every retry. Failure now evicts it and attempts a validated replacement. |
| WR-268 | D3D11 asynchronous copies | Snapshot and depth-probe copies were marked valid without checking device removal, because copy commands return no HRESULT. Device health is now checked before publishing either result. |
| WR-269 | WASAPI block layout | Negotiated block alignment was accepted when merely large enough, allowing per-frame channel addressing to disagree with the endpoint format. It must now equal channels times bytes per sample exactly. |
| WR-270 | WASAPI byte rate | Negotiated average bytes per second was trusted even when inconsistent with sample rate and block alignment. The exact checked product is now required. |
| WR-271 | WASAPI CRT lifetime | The audio worker used `CreateThread` despite executing CRT allocation and conversion code. It now uses `_beginthreadex` with the matching calling convention. |
| WR-272 | WASAPI padding failures | Repeated `GetCurrentPadding` errors could spin forever. Eight consecutive failures now stop the worker with deterministic diagnostics. |
| WR-273 | WASAPI buffer failures | A successful padding query reset the shared retry counter, so `GetBuffer` could still fail forever. Padding and buffer acquisition now have independent bounded counters. |
| WR-274 | WASAPI null buffers | Successful acquisition with a null pointer released silence and continued, repeatedly accepting an impossible contract. The worker attempts the required release, records failure, and stops. |
| WR-275 | WASAPI buffer release | `ReleaseBuffer` failure incremented telemetry but continued as though endpoint ownership had been returned. It now stops the worker immediately. |
| WR-276 | WASAPI worker state | Several loop exits left `running` set until an external shutdown. Every worker exit now clears the published state. |
| WR-277 | WASAPI pause state | A failed `IAudioClient::Stop` left the software pause flag set although audio could still be running. Pause state now rolls back under its lock. |
| WR-278 | native file-dialog COM | `RPC_E_CHANGED_MODE` was treated as usable apartment initialization, after which STA file-dialog calls ran from an incompatible COM apartment. Native dialogs now fail closed on every failed initialization. |
| WR-279 | native file-dialog filters | A pattern with an empty display name produced an invalid filter record. Windows dialogs now use a stable `Files` label fallback. |
| WR-280 | native open dialog | Single-file open omitted filesystem, existing-file/path, no-directory-change, and no-recent-list constraints. The complete safe option set is now explicit. |
| WR-281 | native multi/folder dialogs | Multi-select and folder selection inherited permissive shell-item behavior and could change process state. Both modes now require filesystem paths and preserve directory/recent-list state. |
| WR-282 | native save dialog | Save omitted overwrite confirmation, filesystem/path constraints, and state-preservation flags. The native save contract now sets all of them. |
| WR-283 | widget file enumeration | Entry-vector growth multiplied capacity before proving the multiplication safe. Capacity doubling and byte sizing are now checked in separate steps. |
| WR-284 | widget home lookup | `%USERPROFILE%` was returned without proving that it existed as a directory. Invalid values now fall through to the next supported source. |
| WR-285 | widget legacy home lookup | `%HOMEDRIVE%%HOMEPATH%` could be relative, nonexistent, or a file. The combined value must now be an absolute existing directory. |
| WR-286 | widget parent navigation | Parent calculation could trim ordinary UNC, extended drive, or extended UNC share roots into invalid paths. A root parser now preserves every supported root boundary. |
| WR-287 | Windows locale environment | Locale fallback borrowed `getenv` storage that concurrent environment mutation could invalidate. It now takes an owned `GetEnvironmentVariableW` snapshot with bounded growth-race retries. |
| WR-288 | Windows locale conversion | Fallback environment text was converted through process narrow storage, and system-locale validation wrote incrementally. Strict UTF-16/UTF-8 conversion and validate-before-write preserve deterministic output. |
| WR-289 | Win32 title conversion | UTF-8 window-title allocation multiplied a native length without an explicit `size_t` bound. The byte allocation is now overflow-checked. |
| WR-290 | Win32 thread yield regression | The documented `Sleep(0)` fallback after an unsuccessful `SwitchToThread` was absent from the adapter. The implementation now matches the reliability contract already recorded by WR-29. |
| WR-291 | cleanup path namespaces | The detached helper accepted any `\\?\` prefix, including arbitrary device namespaces. Parsing now recognizes only drive, UNC, extended-drive, and extended-UNC forms. |
| WR-292 | cleanup root deletion | Drive and share roots could pass broad absolute-path checks. Every accepted cleanup target must contain a child below its parsed root. |
| WR-293 | cleanup traversal aliases | Dot components, repeated separators, and empty components admitted ambiguous normalized targets. The lexical policy now rejects all three before filesystem access. |
| WR-294 | cleanup component aliases | Alternate streams, control/illegal characters, and trailing dots/spaces could change Win32 target interpretation. Every component now has one permitted interpretation. |
| WR-295 | cleanup device names | Reserved DOS devices, including extension-bearing and superscript-digit COM/LPT aliases, could name devices instead of files. The complete reserved set is rejected case-insensitively. |
| WR-296 | cleanup duplicate targets | Case or separator aliases could schedule one target more than once. A Windows-ordinal-equivalent comparison now rejects duplicate cleanup requests. |
| WR-297 | cleanup command line | Options were case-sensitive and duplicate parents or unbounded file/directory lists were accepted. Parsing is now case-insensitive, duplicate-aware, and capped at 64 files plus 64 directories. |
| WR-298 | cleanup file indirection | A reparse-point leaf passed the old file check and could redirect deletion. File cleanup now refuses every reparse point. |
| WR-299 | cleanup directory indirection | Directory retries did not revalidate type or reparse state. Every attempt now proves it is still an ordinary directory. |
| WR-300 | cleanup read-only handling | Failure to clear a read-only file attribute was ignored before deletion retries. It now fails deterministically instead of masking the native boundary failure. |
| WR-301 | cleanup exit status | Adding raw Win32 errors to 1,000-based offsets could exceed stable process exit-code semantics. The helper now exposes a small documented class of failure codes. |
| WR-302 | cleanup parent wait | Raw wait/open errors leaked as inconsistent exit statuses, including timeout values. Parent-wait failure now maps to one stable helper result while nonexistent exited parents remain the supported race. |
| WR-303 | Studio build arguments | Space-splitting CMake options, unchecked job counts, and weak architecture aliases made Windows Studio builds ambiguous. A quote-aware tokenizer, bounded jobs, and native `PROCESSOR_ARCHITEW6432` detection now define the inputs. |
| WR-304 | Studio cross-architecture trees | Host compiler and target runtime builds could share a CMake tree or silently reuse a tree for the wrong platform. Cross builds require distinct trees and every existing cache is architecture-checked. |
| WR-305 | Studio tool discovery | Multi-config executables and case-varied standard build configurations were not handled consistently. Discovery now supports both tree layouts and canonicalizes the four CMake configurations. |
| WR-306 | Studio output safety | Binary, metadata, compiler, compatibility, and protected source paths could collide or traverse reparse/hard-link destinations. Outputs are canonicalized, collision-checked, and indirection-rejected before mutation. |
| WR-307 | Studio failed compilation | Direct compilation to the published path could destroy the last good executable on a late failure. Compilation now targets a same-directory unique stage first. |
| WR-308 | Studio PE validation | A produced file was accepted without proving PE32+, target machine, and bounded headers. Host, target, staged, and compatibility executables now pass strict PE validation. |
| WR-309 | Studio build provenance | The old buildinfo did not bind schema, architecture, byte size, hash, or an exact toolchain version. Schema 1 records and validates all of them with strict UTF-8 and a bounded exact field set. |
| WR-310 | Studio pair publication | Binary and buildinfo replacement could leave a mixed old/new pair. Publication now backs up both, publishes both staged files, and rolls both back on failure. |
| WR-311 | Studio compatibility cleanup | `--clean` removed compatibility artifacts even when compatibility copying was explicitly disabled, and temporary/environment state could leak after failure. Disabled outputs are now untouched and all invocation state is restored. |
| WR-312 | CMake Studio generation | Repository builds compiled directly to the final Studio name and emitted unbound metadata. CMake now compiles to a staged executable and writes architecture/size/SHA-256 schema-1 metadata before final publication. |
| WR-313 | Studio package completeness | Any Studio-owned asset enabled the optional component even when the canonical executable or buildinfo was absent. Presence of any asset now requires the complete canonical pair. |
| WR-314 | Studio package file type | The canonical executable/buildinfo could be symlinks, have reversed executable flags, disagree with manifest sizes, or carry the wrong PE machine. All properties are now verified before packaging. |
| WR-315 | Studio package metadata shape | Empty, oversized, NUL-bearing, duplicate, missing, or unknown buildinfo fields were accepted. Packaging now enforces a bounded exact schema. |
| WR-316 | Studio package provenance | A stale Studio from another toolchain version or architecture could be packaged when its bytes otherwise looked valid. Version, architecture, size, and lowercase SHA-256 must all match. |
| WR-317 | Studio nested signing | The Studio executable could be signed once during validation and again during payload iteration, making ordering and metadata binding signer-dependent. It is now signed exactly once before payload assembly. |
| WR-318 | Studio signed buildinfo | Authenticode changes PE bytes after buildinfo generation, leaving installed hash/size metadata stale. The packager rebinds only validated size/hash fields to the exact signed payload bytes. |
| WR-319 | installer wrapper inputs | Equals-form input options, empty values, or multiple build/stage/verify modes were classified inconsistently. The wrapper now normalizes supported spellings and requires exactly one caller-owned mode at most. |
| WR-320 | installer wrapper CMake options | Quoted, separated `-D`, typed BOOL, duplicate, and conflicting Studio definitions could bypass the default. Tokenized parsing accepts only one consistent supported meaning. |
| WR-321 | installer wrapper Studio gate | A fresh build checked only that Studio files existed. It now proves PE32+, exact repository version, exact metadata schema, architecture, size, and SHA-256 before invoking the packager. |
| WR-322 | signing/update URLs | Timestamp and update URLs accepted whitespace, backslashes, credentials, fragments, or impractical lengths. Authoring now requires bounded printable credential-free absolute HTTPS URLs. |
| WR-323 | Authenticode staging | Input/output/PFX/metadata aliases, path indirection, and source mutation during signing could redirect or race publication. All identities are preflighted and the source hash is checked across staging and publication. |
| WR-324 | Authenticode publication | A successful signed executable could replace the old file before metadata creation or publication failed. Signed bytes and metadata now publish as one rollback-protected pair, including in-place signing. |
| WR-325 | update-manifest outputs | Manifest, public key, and PFX paths could alias or traverse unsafe destinations, and certificate-store matches outlived their store scope. Paths are distinct/validated and every certificate/store object is disposed deterministically. |
| WR-326 | update-manifest publication | Line/signature sizes and multi-file publication were incompletely constrained. UTF-8 lines, RSA signature length, total manifest size, and manifest/public-key pair replacement are now exact and failure-atomic. |
| WR-327 | installer validator capture | Child stdout/stderr were read completely into memory before the nominal limit was checked, and child execution had no universal finite bound. Captures now stream to unique files, enforce byte/time ceilings while running, and kill over-budget processes. |
| WR-328 | installer validator inspection | `/inspect` output could be oversized, malformed UTF-8/JSON, or a partial identity. The validator caps the file and requires the complete schema-3 toolchain identity and bounded component set. |
| WR-329 | installer validator paths | Upgrade sentinels and cleanup roots relied on broad joins and clobbering writes. Relative paths are confined component-by-component, sentinels use `CreateNew`, and Windows/reparse roots are rejected. |
| WR-330 | installer validator architecture | WOW64 could report the emulated process machine, and baseline/current identity was underconstrained. Native host architecture is used and baseline identifier, architecture, and differing version are required. |
| WR-331 | installer validator replacement | `-ReplaceExisting` trusted any registered command and did not wait for detached cache removal. Replacement is restricted to a verified same-identity maintenance image below the owned cache root and waits for cleanup. |
| WR-332 | installer validator payload | Installed checks proved names but not executable format, while Apps & Features identity was only partially compared. ARP display/version are exact and every required tool must be an architecture-matched PE32+ image. |
| WR-333 | installer validator Studio | Installed Studio validation checked only four metadata fields and only once. The full exact versioned schema/hash is now checked after install and again after Minimal-to-Complete component restoration. |
| WR-334 | installer validator environment | Validation could leak `ZANNA_LIB_PATH`, mishandle `%` in generated batch paths, or verify only the outer release signature. Environment is restored, batch paths are escaped, and release mode recursively verifies maintenance and installed PE signatures. |
| WR-335 | installer validator cleanup | Failed installs and temporary workspaces were recursively removed without rejecting newly introduced reparse points. Cleanup is path-confined and refuses any reparse-bearing tree. |
| WR-336 | installer internal options | Duplicate, unpaired, or contradictory elevated/handoff worker switches could enter internal modes from an ambiguous command line. Parsing now rejects all such combinations. |
| WR-337 | installer worker trust | Handoff waiting occurred before proving that the process was the cached maintenance image or a genuinely elevated machine-scope worker. Both proofs now run before trusting the supplied parent PID. |
| WR-338 | widget home ownership | Rejecting an invalid `%HOMEDRIVE%%HOMEPATH%` directory freed both converted components before falling through and then freed them a second time. Fallback now has one consolidated release point. |
| WR-339 | Studio buildinfo classification | The generic `bin/` rule marked buildinfo executable, allowed metadata to satisfy the required-Studio binary lookup, and exposed it as a macOS command. `.buildinfo` is now data and is excluded from tool links. |
| WR-340 | cross-target package tests | Linux toolchain archive fixtures inherited `.exe`, `.lib`, PE identity, and synthetic NTFS permission bits on Windows, causing their exact payload and mode assertions to depend on the host. The fixtures now normalize Linux names, archive extensions, ELF identity, and the intended POSIX mode before packaging. |
| WR-341 | Studio phase probe deadlines | Process and debugger waits treated `Sleep(1)` poll counts as elapsed milliseconds, making the Windows end-to-end probe depend on scheduler granularity and cold executable scanning. The waits now use bounded monotonic deadlines, terminate timed-out children, preserve diagnostic output, and have matching serial CTest headroom. |
| WR-342 | Release boundary assertions | Release installer builds inherited `NDEBUG` in two boundary suites that deliberately use assertions as executable checks, erasing filesystem, Result, and COM calls; the scene-editor and UI Automation tests consequently failed or crashed after their setup disappeared. Both translation units now enable their checks before any header can cache the disabled assertion macro, without changing product or unrelated test flags. |
| WR-343 | Studio version provenance | The packager compared Studio’s full configured version (for example `0.2.99-snapshot`) with the deliberately numeric CMake package version (`0.2.99`), so a correctly built prerelease Studio could never ship. Install manifests now preserve package and exact product versions separately; Studio provenance binds the staged header’s full version while installer naming and upgrade metadata retain the package-compatible version. |
| WR-344 | native WinSock teardown | The first `TcpServer.Listen` in a native Windows executable registered `WSACleanup` through CRT `atexit`, but Zanna PE files can enter through a deliberately CRT-less startup shim. The call corrupted the uninitialized CRT exit table (`0xC0000374` in ZannaSQL) before the listener was returned. WinSock now remains process-lifetime state, which Windows reclaims at process teardown, and the native Windows runtime probe opens, inspects, and closes an ephemeral listener through that exact entry path. |
| WR-345 | Studio lifecycle provenance | The lifecycle validator still compared installed Studio’s full configured version with the installer’s deliberately numeric package/upgrade version, so the correctly repaired prerelease package would fail its own Complete install and restore checks. Validation now obtains the bounded canonical product version from the installed, package-owned `zanna --version` result and uses it for both Studio provenance checks while retaining numeric package identity for Apps & Features. |
| WR-346 | installer validator version parsing | The Studio provenance helper anchored its version expression to all of `zanna --version`, even though the CLI contract intentionally follows the canonical product-version line with snapshot, source, IL, and feature details. A valid installer therefore failed lifecycle validation after installation. The helper now parses the strict first line from the still-bounded, NUL-free capture and accepts the documented diagnostic lines that follow. |
| WR-347 | D3D11 device creation | The device was created without `D3D11_CREATE_DEVICE_BGRA_SUPPORT`, preventing reliable Direct2D/DXGI BGRA interoperation. Creation now requests the BGRA capability explicitly. |
| WR-348 | D3D11 feature level | Device creation requested 11.0 but discarded the feature level actually returned by the driver. The backend now captures it and rejects any result other than the required 11.0 contract. |
| WR-349 | D3D11 void commands | `UpdateSubresource`, `GenerateMips`, and `CopyResource` return no HRESULT, so a removed device could discard work while CPU state claimed success. A shared post-command device-health check now gates every affected publication. |
| WR-350 | D3D11 float buffers | Float-SRV updates advanced as successful without checking device removal after `UpdateSubresource`. The upload now returns the post-command device status. |
| WR-351 | D3D11 temporary textures | A partial texture/SRV allocation failure could leak the COM object because cleanup still classified the record as non-temporary. Failure now marks ownership before releasing the partial allocation. |
| WR-352 | D3D11 temporary textures | Temporary texture upload and mip generation published a usable SRV even if the device was removed. The complete object is now discarded unless both void commands leave the device healthy. |
| WR-353 | D3D11 temporary cubemaps | Partial cubemap allocation had the same ownership-classification leak. Failure now marks and releases every partial cube resource. |
| WR-354 | D3D11 temporary cubemaps | Temporary cube uploads and mip generation could publish after device loss. Device health is checked before upload telemetry or the SRV escapes. |
| WR-355 | D3D11 compressed uploads | Native compressed texture streaming advanced its block-row cursor after an unconfirmed `UpdateSubresource`. Cursor publication now requires a healthy device. |
| WR-356 | D3D11 streamed textures | Row-sliced texture uploads advanced progress and byte telemetry even if the upload was discarded. Failed device-health checks now fail the upload before any cursor mutation. |
| WR-357 | D3D11 streamed texture mips | The final texture generation became current immediately after an unchecked `GenerateMips`. Generation and pending-state publication now occur only after the mip command is confirmed. |
| WR-358 | D3D11 IBL uploads | Cubemap IBL identity was cached after a series of unchecked subresource uploads. The identity is now committed only after one final device-health check. |
| WR-359 | D3D11 streamed cubemaps | Face/row cursors and telemetry advanced after unchecked cubemap slice uploads. Each slice now fails before publication when device removal is reported. |
| WR-360 | D3D11 cubemap mips | A streamed cube generation was committed after an unchecked `GenerateMips`. Pending generation remains uncommitted unless device health is confirmed. |
| WR-361 | D3D11 opaque depth | Opaque-depth resolve marked the snapshot valid immediately after `CopyResource`. The validity bit now remains clear when the copy coincides with device loss. |
| WR-362 | Win32 class registration | `ERROR_CLASS_ALREADY_EXISTS` was accepted without proving who registered the class. The adapter now verifies the module, window procedure, and `CS_OWNDC` contract before reusing it. |
| WR-363 | Win32 IME attributes | IME attribute byte counts could exceed the composition unit count and drive selection scans beyond valid text. Attribute reads are now bounded by both the text and event capacity. |
| WR-364 | Win32 IME text | An arbitrarily large composition/result payload could allocate far beyond the fixed event contract. Oversized payloads now record overflow and are rejected before allocation. |
| WR-365 | Win32 surrogate input | A second high surrogate silently replaced the first pending value. The abandoned scalar now emits U+FFFD before the new surrogate is retained. |
| WR-366 | Win32 surrogate boundaries | A pending high surrogate could disappear when a BMP character or new key event arrived. Boundary transitions now emit U+FFFD and clear the pending state deterministically. |
| WR-367 | Win32 `WM_UNICHAR` | Supplementary-plane input delivered through `WM_UNICHAR` was ignored. The window now implements the capability handshake and validates every scalar before enqueueing it. |
| WR-368 | Win32 system keys | `WM_SYSKEYDOWN`/`WM_SYSKEYUP` bypassed Zanna key events or suppressed normal Alt/system behavior. They now share key-state delivery while still delegating native system handling. |
| WR-369 | Win32 mouse capture | Drags outside the client area could lose button-up events because button-down did not acquire capture. Supported button presses now attempt capture and publish ownership only when Win32 confirms it. |
| WR-370 | Win32 capture release | Capture could be released while another supported mouse button remained down. It is now released only after the final left/right/middle button is up. |
| WR-371 | Win32 capture loss | External capture transfer left Zanna button state stuck down. `WM_CAPTURECHANGED` now clears ownership and all supported button states. |
| WR-372 | Win32 cancellation/focus | `WM_CANCELMODE` and focus loss could preserve capture and pressed-button state. Both paths release owned capture and reset the logical buttons. |
| WR-373 | Win32 destruction | Window destruction did not retire an outstanding mouse capture. Teardown now releases proven ownership before destroying platform state. |
| WR-374 | Win32 file drops | A shell drop containing an unbounded number of paths could monopolize the event pump. Drop enumeration is capped to event-queue capacity and records overflow. |
| WR-375 | Win32 file-drop paths | Zero-length and over-capacity shell path lengths reached allocation/conversion logic that could never fit the event. They are rejected before allocation with overflow accounting. |
| WR-376 | Win32 paint validation | `EndPaint` was called even when `BeginPaint` failed. The adapter now ends only a successfully begun paint and falls back to native processing otherwise. |
| WR-377 | Win32 module discovery | A failed `GetModuleHandleW` flowed into registration and window creation as if a valid instance existed. Initialization now fails with a platform diagnostic. |
| WR-378 | Win32 window bounds | Failure of the DPI-aware `AdjustWindowRect` path was ignored. Window creation now stops before using uninitialized or stale adjusted bounds. |
| WR-379 | Win32 window dimensions | Adjusted rectangle subtraction narrowed directly to `int`, allowing inverted or overflowing dimensions. Width and height are checked in 64 bits before narrowing. |
| WR-380 | WASAPI valid bits | Extensible formats accepted zero valid bits or a value larger than the sample container. Negotiation now rejects both malformed cases. |
| WR-381 | WASAPI float format | A 32-bit float container was accepted even when valid-bits metadata described a different representation. Float playback now requires exactly 32 valid bits. |
| WR-382 | WASAPI PCM format | PCM formats below the mixer’s 16-bit conversion floor could reach unsupported conversion logic. Negotiation now rejects them up front. |
| WR-383 | WASAPI thread waits | Timeout and wait failure were conflated, and the follow-up infinite wait result was ignored. Join now distinguishes both states and reports a failed recovery wait. |
| WR-384 | WASAPI thread handles | Failure to close the worker handle was silently discarded. Shutdown now records a backend failure and diagnostic. |
| WR-385 | WASAPI worker COM | `RPC_E_CHANGED_MODE` was treated as a usable worker apartment. Every failed worker `CoInitializeEx` now fails startup rather than running COM in an incompatible apartment. |
| WR-386 | WASAPI owner COM | Context creation similarly tolerated incompatible COM state and omitted the OLE1 DDE suppression flag. Initialization is strict and uses `COINIT_DISABLE_OLE1DDE`. |
| WR-387 | WASAPI pause serialization | The worker sampled `paused` under a lock and then rendered after releasing it, racing `Stop`/`Reset`. The pause lock now covers the complete endpoint buffer transaction. |
| WR-388 | WASAPI control thread | Pause and resume could call apartment-affine endpoint controls from an arbitrary thread. The context records its owner and rejects cross-thread control. |
| WR-389 | WASAPI destruction thread | Cross-thread destruction called `CoUninitialize` for an apartment owned by another thread. It now reports misuse and leaves that apartment reference to its owner. |
| WR-390 | WASAPI pause idempotence | Repeated pause calls issued repeated endpoint stops. An already-paused context now returns without touching the client. |
| WR-391 | WASAPI pause reset | Pausing stopped the endpoint but retained queued audio, so resume could replay stale samples. A successful stop is now followed by `IAudioClient::Reset`. |
| WR-392 | WASAPI pause rollback | Stop/reset failure could leave software and endpoint state contradictory. The adapter attempts a safe restart and rolls back the pause bit when recovery succeeds. |
| WR-393 | WASAPI resume state | Resume could start an inactive/null client or repeatedly start an already-running one. It is now owner-thread-only, idempotent, and rejects inactive contexts before clearing pause. |
| WR-394 | HTTP server threads | The Windows HTTP accept loop used `CreateThread` despite executing CRT code. It now uses `_beginthreadex` with the matching calling convention. |
| WR-395 | HTTPS server threads | The HTTPS accept loop had the same CRT initialization hazard. It now uses `_beginthreadex`. |
| WR-396 | WebSocket server threads | The WebSocket accept loop had the same CRT initialization hazard. It now uses `_beginthreadex`. |
| WR-397 | secure WebSocket threads | The secure WebSocket accept loop had the same CRT initialization hazard. It now uses `_beginthreadex`. |
| WR-398 | HTTP download staging | Temporary path formatting assumed `snprintf` succeeded and fit its computed buffer. Negative/truncated results now fail before the path is used. |
| WR-399 | HTTP download paths | Download filesystem operations consumed process-ACP bytes on Windows. A strict owned UTF-8-to-UTF-16 conversion now fronts every native path operation. |
| WR-400 | HTTP download creation | Staged downloads used narrow `_open` and left the descriptor inheritable. Windows now uses `_wopen` with `_O_NOINHERIT` and exclusive creation. |
| WR-401 | HTTP download publication | Atomic replacement used `MoveFileExA`, corrupting destinations outside the active code page. Publication now uses `MoveFileExW` for both paths. |
| WR-402 | HTTP download permissions | Mode preservation used narrow `stat`/`chmod` and 32-bit metadata. It now uses `_wstat64` and `_wchmod` on strict native paths. |
| WR-403 | HTTP download cleanup | Failed/cancelled stage cleanup used narrow `remove`, potentially leaving sensitive partial files. Cleanup now uses `_wremove`. |
| WR-404 | HTTP path failure | A malformed path could partly convert and continue through mode or replacement logic. Every multi-path operation now frees all temporaries and fails closed unless every conversion succeeds. |
| WR-405 | SaveData environment | `%APPDATA%`, `%USERPROFILE%`, `%HOMEDRIVE%`, and `%HOMEPATH%` were read as borrowed process-ACP `getenv` values. Windows now snapshots each value through `GetEnvironmentVariableW`. |
| WR-406 | SaveData environment races | Environment sizing/read races could truncate or reuse stale storage. Native snapshots retry a bounded number of times and convert strict UTF-16 into owned UTF-8. |
| WR-407 | SaveData path assembly | Multiple save/data path concatenations added lengths without overflow checks. A shared checked concatenator now fails allocation before wraparound. |
| WR-408 | SaveData home fallback | Partial home-variable fallbacks leaked or reused ownership ambiguously. Every temporary is now independently owned and released along all branches. |
| WR-409 | SaveData parent roots | Parent extraction turned `C:\file` into `C:` and `/file` into an empty parent. Drive and separator roots are now preserved. |
| WR-410 | SaveData base selection | APPDATA/home selection allocated home unnecessarily and mixed borrowed/owned lifetimes. The chosen absolute base is now built once, used only while owned, and released deterministically. |
| WR-411 | TempFile directory API | The runtime always used legacy `GetTempPathW`. It now prefers dynamically resolved `GetTempPath2W` and retains the legacy API only as compatibility fallback. |
| WR-412 | TempFile sizing | A temp/current-directory sizing race could truncate the path or trust stale capacity. A shared provider loop retries boundedly with overflow-checked allocation. |
| WR-413 | TempFile directory validity | Environment-backed temp results were returned without proving they still named directories. Native attributes are now checked before publication. |
| WR-414 | TempFile roots | Trailing-separator removal turned drive, UNC, extended-drive, extended-UNC, or volume roots into different paths. Root recognition now preserves every supported namespace. |
| WR-415 | TempFile fallback | Failure invented `C:\Temp`, which might not exist or be writable. The existing process current directory is now the final native fallback, otherwise the API traps. |
| WR-416 | TempFile creation | Created files lacked temporary/not-indexed attributes, and `CloseHandle` failure still reported success. Attributes are explicit; close failure deletes the stage and traps. |
| WR-417 | TempFile conversion | Empty or malformed native directory text could escape as a plausible runtime string. Strict conversion must now produce a non-empty result or the provider is rejected. |
| WR-418 | child handle inheritance | `CreateProcessW(..., TRUE, ...)` allowed unrelated inheritable process handles to leak into tools and signers. `STARTUPINFOEX` now allow-lists only stdin/stdout/stderr. |
| WR-419 | child startup flags | The handle list could not take effect without the extended-startup creation flag. Windows launches now set `EXTENDED_STARTUPINFO_PRESENT` and retire the attribute list after creation. |
| WR-420 | child capture threads | Output capture used raw Win32 threads while appending through the C++ runtime. Capture workers now use `_beginthreadex`. |
| WR-421 | child pipe errors | Unexpected `ReadFile` failures were treated like ordinary EOF. Each capture worker retains its native error and the parent reports the affected stream. |
| WR-422 | child process waits | A failed process wait was ignored before exit-code inspection. The launcher now terminates/reaps the child and emits the native wait error. |
| WR-423 | child exit queries | `GetExitCodeProcess` failure could publish a zero/default success. It now emits a diagnostic and a deterministic saturated failure status. |
| WR-424 | child working directories | Working-directory validation reconstructed UTF-8 through the active code page. Validation now uses the shared native-path decoder, with a Unicode-directory regression. |
| WR-425 | tool filesystem contract | Tools repeatedly relied on implementation-defined narrow `std::filesystem` conversion. A shared adapter now defines owned UTF-8-to-native and native-to-UTF-8 boundaries. |
| WR-426 | tool environment contract | Compiler/linker/package configuration read borrowed ACP `getenv` values on Windows. A shared environment snapshot helper now uses strict, race-aware native reads. |
| WR-427 | tool command lines | Eleven installed tools accepted ACP-decoded CRT `argv`, corrupting non-ASCII paths before validation. Their mains now rebuild strict UTF-8 arguments from `GetCommandLineW`. |
| WR-428 | SourceManager keys | A remaining `generic_string()` conversion threw during registration of a Unicode Windows source path, aborting tools with exit code 3. Lookup keys now use explicit UTF-8 encoding. |
| WR-429 | frontend source loading | Shared source loading and Zia `compileFile` opened UTF-8 strings through narrow streams. Both now open native paths, allowing `zanna`, `zia`, and `vbasic` to consume Unicode files. |
| WR-430 | Zia imports | Import normalization, cache metadata, and file reads reconstructed paths through ACP and emitted ACP cache keys. Every operation now converts at the filesystem boundary and retains UTF-8 keys. |
| WR-431 | BASIC `ADDFILE` | Include resolution, canonicalization, reads, and size checks used narrow paths. Included BASIC sources now remain UTF-8 in diagnostics and native for disk operations. |
| WR-432 | Zia editor services | Project-root normalization and language-server workspace discovery used narrow filesystem construction/output. Completion and symbol indexes now preserve Unicode project paths. |
| WR-433 | project discovery | Project manifests, source lists, script discovery, and diagnostic paths mixed native and narrow representations. The loader now keeps native paths internally and explicit UTF-8 at tool interfaces. |
| WR-434 | native compilation paths | Codegen, object/archive readers, linker support, temporary artifacts, and output writers used ACP-sensitive conversions. The complete Windows native-build pipeline now consumes UTF-8 paths explicitly. |
| WR-435 | asset compiler paths | Asset cache keys, hashing, pack destinations, diagnostics, and archive logical names mixed native and narrow paths. Native I/O and UTF-8 logical identity are now separated, with Unicode asset/output coverage. |
| WR-436 | package file utilities | Atomic reads/writes, PNG loading, staging, and error messages used narrow paths or could strand partial output. Common packaging utilities now accept native paths and publish Unicode destinations atomically. |
| WR-437 | Windows package inventory | Toolchain manifests and Windows package assembly narrowed staged/source paths, breaking Unicode installer payloads. Inventory identity stays UTF-8 while every disk operation stays native. |
| WR-438 | command path plumbing | `package`, `install-package`, `build`, `build-many`, `init`, and `rtgen` still narrowed user paths after parsing. Output, signing, cache, project, and generator paths now remain Unicode end to end. |
| WR-439 | installer PE identity | The host accepted short/non-MZ images and unknown metadata architectures after checking only the PE signature and machine. It now requires bounded DOS/PE headers and an explicit x64/arm64 match. |
| WR-440 | installer PE shape | Zero-entrypoint, DLL, non-PE32+, excessive-section, invalid-alignment, or unsupported-subsystem images could pass inspection. Optional-header and image characteristics are now validated exactly. |
| WR-441 | installer PE sections | Truncated, header-overlapping, file-out-of-range, virtual-out-of-range, or mutually overlapping sections were accepted. Every section table and raw/virtual extent is now bounded and overlap-checked. |
| WR-442 | installer CLI parsing | Repeated operations/UI/integration/scope/preset/flag options, empty equals values, and signed/spaced handoff PIDs were accepted or reinterpreted. Parsing is duplicate-intolerant, empty-aware, and digit-exact. |
| WR-443 | installer help semantics | `/help` silently overrode an explicitly requested lifecycle operation. Help and lifecycle modes are now mutually exclusive and covered by the fail-closed CLI suite. |
| WR-444 | installer log text | Invalid surrogates, controls, bidi controls, separators, and overlong records could forge or confuse installer logs. Records are scalar-safe, single-line, bounded, and visibly truncated. |
| WR-445 | installer logging/control | Partial log writes, BOM durability, prior-handle close failure, and exceptions from cancellation callbacks were ignored. Writes loop and flush, handle failures surface, and callback exceptions request safe cancellation. |
| WR-446 | installer signing paths | Missing output parents were created before ancestry validation, and stage/backup paths were not preflighted immediately before publication. Signing now validates before and after creation and rechecks every stage, backup, and publish destination. |
| WR-447 | child handle-list lifetime | `UpdateProcThreadAttribute` retained a pointer to a helper-local handle vector that expired before `CreateProcessW`, making every hardened child launch fail. The exact allow-list storage now remains caller-owned until process creation returns. |
| WR-448 | native execution diagnostics | Native-run launch failures replaced the process runner's actionable Win32 diagnostic with a generic executable-path message. The detailed launch error is now retained beneath the operation context. |
| WR-449 | Studio responsive regression | The bottom-panel wide-layout fixture depended on obtaining a window larger than the host work area, so low-resolution or fractionally scaled Windows desktops could never cross the responsive threshold. The fixture temporarily collapses the primary sidebar and then restores it, exercising both layouts within the available viewport. |
| WR-450 | Studio CTest contention | Scene-editor and multi-root probes completed well within their isolated budgets but exceeded 60/60/30-second ceilings while seven unrelated Debug workers saturated the Windows host. Their bounded ceilings now retain measured headroom, and the especially heavy hidden 3D fixture reserves the runner while the display resource lock preserves graphical isolation. |
| WR-451 | Windows MSVC build | Scene-editor code used `std::array` without including `<array>`, so a clean MSVC build failed before Windows runtime validation began. The owning translation unit now includes its direct standard-library dependency. |
| WR-452 | D3D11 device availability | Device creation tried only the hardware driver, excluding Remote Desktop, virtual machines, CI hosts, and machines whose installed display driver could not create a feature-level-11 device. A failed hardware attempt now retries the in-box WARP driver before falling back to Zanna's software backend. |
| WR-453 | D3D11 device retry | Adding a second driver attempt without a transaction boundary would let partial swapchain, device, or immediate-context outputs from the first attempt contaminate the retry. A shared attempt helper clears outputs before creation, validates the complete feature-level-11 result, and releases every partial interface on failure. |
| WR-454 | D3D11 timing begin | `Begin` and the starting timestamp `End` return no HRESULT, yet `frame_time_active` was published immediately even if device removal discarded the commands. Device health now gates active-query publication and a failed set is retired. |
| WR-455 | D3D11 timing end | The ending timestamp and disjoint `End` calls similarly published a pending timing sample after unobservable command failure. Pending state is now committed only after the device remains healthy. |
| WR-456 | D3D11 fallback completeness | Fallback creation returned success once the white 2D and cube SRVs existed even if the BRDF LUT was absent. Readiness now requires both textures, both SRVs, and the complete LUT pair. |
| WR-457 | D3D11 fallback transaction | White textures, cube resources, and the BRDF LUT were written into context fields incrementally, so a late failure exposed a partial resource set to retry and cleanup paths. All six COM objects are staged locally and published together only after complete success. |
| WR-458 | D3D11 RTT staging descriptor | A cached readback surface with an invalid size, format, usage, bind, access, or subresource descriptor returned failure forever while remaining cached. Invalid staging descriptors are diagnosed, evicted, and replaced before a later retry. |
| WR-459 | D3D11 RTT mapped payload | A successful map with an impossible row pitch failed the current read but retained the same poisoned staging resource. Invalid mapped staging payloads now follow the same unmap, eviction, and validated-recreation path as failed and null maps. |
| WR-460 | D3D11 diagnostics | HRESULT and shader diagnostics assumed `snprintf` succeeded, so a CRT formatting failure could send uninitialized stack bytes to the debugger and stderr. One bounded formatter initializes every destination and emits a deterministic fallback or terminated truncation. |
| WR-461 | branded installer models | Null module instances and malformed or embedded-NUL UTF-16 could reach native registration and control creation with a different visible meaning from the model. Page and progress models now validate the instance and every UTF-16 code unit before allocating Win32 resources. |
| WR-462 | branded installer text bounds | Window, heading, body, metadata, details, verification, cancel, action, and progress strings had no practical limits. Per-field bounds now cap allocation, native message, accessibility, and owner-draw work. |
| WR-463 | branded action IDs | IDs outside the unsigned 16-bit `WM_COMMAND` control range were narrowed through `HMENU`/`LOWORD`, allowing one action to activate another. Every action ID must now be positive and exactly representable. |
| WR-464 | branded action ownership | Page action IDs could collide with cancel, status, verification, details, heading, or other internal controls. The complete reserved control set is rejected before window creation. |
| WR-465 | branded action identity | Duplicate action IDs made focus/default state and click lookup ambiguous. Validation now requires a unique ID for every action. |
| WR-466 | branded accessibility | Empty action titles, visible cancel text, or details labels produced nameless controls or unlabeled read-only content. Every visible actionable/auxiliary surface now requires an accessible name. |
| WR-467 | branded default action | A default ID that did not name a page action silently focused the first action while retaining contradictory model state. The default must exist, and the native window now exposes it through the dialog default-button protocol so Enter activates it. |
| WR-468 | branded close action | The title-bar close result could name no action and no cancellation result. Close now resolves only to `IDCANCEL` or a declared action. |
| WR-469 | branded verification | `requiresVerification` silently did nothing when the page omitted verification text. Gated actions now require a real accessible checkbox before the page can be shown. |
| WR-470 | branded progress work | An empty `std::function` created the progress window and worker before failing with `bad_function_call`. Progress work and its required presentation strings are validated before registration, timers, callbacks, or threads. |
| WR-471 | branded page class | `ERROR_CLASS_ALREADY_EXISTS` trusted any process-local class with the branded page name. Reuse now verifies module, window procedure, style, and class/window extra storage. |
| WR-472 | branded progress class | The progress class had the same foreign-registration and concurrent first-use ambiguity. It now uses the shared verified registration boundary. |
| WR-473 | custom-options class | The custom options window also accepted an unverified existing class. All three installer window classes now share collision-intolerant registration with no racy atomic sentinel. |
| WR-474 | branded keyboard default | Owner-drawn page buttons had no parent default-ID protocol, so `IsDialogMessageW` could not reliably activate the model's default action with Enter. `DM_GETDEFID`/`DM_SETDEFID` now preserve native keyboard semantics. |
| WR-475 | custom-options keyboard default | The custom installer accepted only a pointer click on its owner-drawn install button on hosts where the raw window supplied no default dialog ID. Its parent now exposes and maintains `IDOK` as the native default. |
| WR-476 | branded page painting | The page drew and called `EndPaint` even when `BeginPaint` returned no device context. It now delegates safely on begin failure and draws only after client bounds are available. |
| WR-477 | branded progress painting | The progress surface repeated the same invalid paint lifecycle. Its backdrop, compact mark, and track are now guarded by successful paint and bounds acquisition. |
| WR-478 | custom-options painting | The custom page also used a failed `BeginPaint` result and assumed `SaveDC` succeeded. Both native boundaries are checked before drawing or restoring state. |
| WR-479 | installer GDI state | Shared drawing helpers restored DC state with a zero save level, risking selected-object lifetime and subsequent paint corruption. Every helper now stops before mutating the DC when `SaveDC` fails. |
| WR-480 | branded page quit handling | The modal page consumed `WM_QUIT` and returned without restoring it, allowing one nested setup surface to cancel process shutdown. It reposts the exact quit status before unwinding. |
| WR-481 | branded progress quit handling | The progress loop similarly swallowed the thread quit request while joining its worker. It now preserves the quit message after cooperative cancellation and cleanup. |
| WR-482 | custom-options quit handling | The custom-options loop had the same nested-message-loop defect. All installer modal loops now leave the outer application shutdown request intact. |
| WR-483 | installer per-monitor DPI | Raw pages used an unchecked system DPI, so API failure produced zero-sized geometry and moving a PerMonitorV2 window retained stale fonts, controls, scroll extents, and brand geometry. DPI is normalized to a bounded fallback; all page types handle `WM_DPICHANGED`, rescale children/state, rebuild GDI resources, apply the suggested bounds, and repaint. |
| WR-484 | installer progress queue | Every log update allocated a new string and posted a distinct window message, allowing a fast worker to grow heap and queue usage without bound. One bounded pending string is protected by a mutex and updates are coalesced behind a single posted message. |
| WR-485 | demo host architecture | Any native host architecture other than ARM64 was assumed to be x64, so an x86 or unknown environment could reuse and run the wrong tools. Only explicit AMD64 and ARM64 host identities are accepted. |
| WR-486 | demo CMake cache proof | An existing tree with neither generator-platform nor system-processor identity was accepted. Reuse now fails closed when the target architecture cannot be proven. |
| WR-487 | demo CMake architecture | An unrecognized non-empty cache architecture was ignored as though compatible. Unknown architecture values are rejected rather than bypassing the requested-target comparison. |
| WR-488 | demo asset parsing | Whitespace splitting only appeared to support quotes and corrupted project asset paths containing spaces. Asset directives now use the same quote-aware native argument tokenizer as CMake options. |
| WR-489 | demo asset grammar | Missing sources, extra fields, and empty quoted operands could be silently skipped or reinterpreted. Every line beginning with `asset` now has an exact two- or three-field grammar and fails with project context when malformed. |
| WR-490 | demo asset source ancestry | Lexical containment did not stop a project asset path from traversing an existing junction or symbolic link outside the project. Every existing source ancestor is checked for reparse indirection. |
| WR-491 | demo asset source trees | A contained source directory could hide a nested reparse point that recursive copy followed. The complete declared source tree is rejected if any entry is indirect. |
| WR-492 | demo asset destinations | Output directories and parents were created or reused without proving that an existing component was not a reparse point. Destination ancestry is checked before each copy. |
| WR-493 | demo asset collisions | Two projects could silently overwrite the same shared asset path with different content, making results depend on manifest order; 3D Bowling and Xenoscape already had different title-art bytes at one path. Each published demo now owns a sidecar directory, and per-demo ownership still rejects conflicting directives while allowing byte-identical aliases. |
| WR-494 | demo executable preservation | Compilation wrote directly over the published `.exe`, so a late compiler/linker failure could destroy the last good demo. Every build now targets a same-directory unique stage. |
| WR-495 | demo stale output | Exit zero was trusted without proving that the requested output existed, allowing a stale prior executable to be reported and run. The exact staged file must exist and pass validation before smoke or publication. |
| WR-496 | demo PE shape | Arbitrary/truncated/MZ-only output was accepted as a Windows demo. Stages now require bounded DOS/PE offsets, a PE signature, a sane section count, a complete PE32+ optional header, and executable/non-DLL characteristics. |
| WR-497 | demo PE architecture | A host or compiler mix-up could publish an x64 image for ARM64 or vice versa. The COFF machine must exactly match the requested demo architecture and the image must have an entry point. |
| WR-498 | demo smoke isolation | Demos ran in shared `examples/bin`, so a smoke launch could modify or delete published executables and assets. Each launch now receives a unique private directory containing only its executable and declared assets. |
| WR-499 | demo timeout process trees | Timeout killed only the immediate process, allowing descendants to survive with inherited handles or locks. The Windows tree terminator now stops the complete PID tree. |
| WR-500 | demo timeout wait | After kill, the driver performed an unbounded `WaitForExit()`, so inherited redirected handles or failed termination could hang all demo automation. Reaping uses one finite five-second bound and reports a deterministic failure. |
| WR-501 | Xenoscape asset generator | A newly added scene-source copy retained a raw `_WIN32` branch that was absent from the platform-policy debt baseline, making the complete Windows CTest gate fail after every compiled test passed. Both generator copies now select the available directory API through header capability detection, and directory-creation failures other than an existing output directory are fatal. |
| WR-502 | installer build self-lock | Explicit `--build-dir` packaging launched that tree's `zanna.exe` and then asked it to relink the same image, which Windows denied with `LNK1104`. The wrapper now executes a unique, byte-exact staged driver outside the relink target, verifies its shape and SHA-256 before launch, and removes only the exact file and empty private directory afterward. |
| WR-503 | runtime audio detection | Signature detection opened a UTF-8 runtime path through narrow `fopen`, so audio under names outside the active ANSI code page was reported as an unknown format. It now uses the strict UTF-8/native-path stdio adapter. |
| WR-504 | runtime MP3 decode | Eager MP3-to-WAV conversion used the same narrow path boundary and failed before decoding non-ASCII filenames. The complete file read now starts from a strict UTF-16 Windows open. |
| WR-505 | runtime MP3 streaming | The streaming MP3 reader independently used narrow `fopen`, so long-form playback disagreed with eager decoding on Unicode paths. Both paths now share the same non-inheritable UTF-8 adapter. |
| WR-506 | runtime Ogg streaming | Ogg file readers also interpreted managed UTF-8 paths through the process ANSI code page. Ogg streams now open through the shared strict Windows filesystem boundary. |
| WR-507 | runtime TLS files | Custom CA certificates, certificate chains, and private keys could not be read from Unicode paths. The common TLS text loader now performs strict UTF-8-to-UTF-16 opening. |
| WR-508 | runtime HDR assets | Cubemap panorama loading used narrow stdio even though the runtime asset contract is UTF-8. HDR files now use the shared Unicode/non-inheritable adapter. |
| WR-509 | light-probe persistence | Probe-grid load and save paths were ANSI-only on Windows. Both directions now honor the runtime's UTF-8 path contract. |
| WR-510 | game-state persistence | World save slots could be constructed as UTF-8 beneath a Unicode profile but were then reopened through narrow stdio. State loads and temporary saves now stay Unicode end to end. |
| WR-511 | native audio sound files | ZannaAUD eager WAV loading bypassed runtime path adapters and could not open non-ACP filenames. A library-local strict UTF-8 reader now opens them through UTF-16. |
| WR-512 | native audio music files | ZannaAUD's streaming WAV path had a second independent narrow open. Music streams now use the same Unicode reader as sound effects. |
| WR-513 | GUI bitmap files | The GUI BMP widget loaded image paths with narrow stdio. It now shares a strict UTF-8 GUI asset reader with the font loader. |
| WR-514 | GUI/font handle inheritance | The prior Unicode font helper used `_wfopen`, whose descriptor could remain inheritable across a later child launch. GUI asset descriptors are now created explicitly with `_O_NOINHERIT`. |
| WR-515 | audio handle inheritance | ZannaAUD file descriptors could likewise escape into spawned processes and retain asset locks. Its Windows reader now creates non-inheritable descriptors before converting them to `FILE *`. |
| WR-516 | malformed asset paths | The new GUI and audio boundaries could have preserved Unicode support while still accepting lossy conversion. Both helpers use `MB_ERR_INVALID_CHARS` and fail without opening a replacement-character path. |
| WR-517 | light-probe save atomicity | Probe-grid saves wrote directly into the destination, so disk-full or short-write failure destroyed the prior valid bake. They now write an exclusive same-directory temporary file and atomically replace only after a successful close. |
| WR-518 | game-state temp collisions | World saves reused one predictable `<slot>.tmp` name, allowing concurrent saves or stale files to truncate each other's work. A bounded exclusive-create sequence now gives each attempt collision-safe staging. |
| WR-519 | game-state replacement gap | Windows `rename` was preceded by unconditional destination deletion, so replacement failure lost the last good save. `MoveFileExW(REPLACE_EXISTING\|WRITE_THROUGH)` now publishes without a delete gap and failed stages are removed. |
| WR-520 | Ogg CRC initialization | Concurrent first Ogg readers raced on a plain initialized flag and could calculate page CRCs through a partially populated table. A release/acquire three-state latch publishes the table only after all 256 entries exist. |
| WR-521 | MSVC 32-bit atomics | `fetch_sub(ptr, INT_MIN)` evaluated `-INT_MIN`, which is signed overflow before reaching the interlocked intrinsic. The subtraction delta is now formed with defined unsigned modulo arithmetic. |
| WR-522 | MSVC 64-bit atomics | The 64-bit interlocked subtraction repeated the same undefined negation for `INT64_MIN`. It now forms and passes the exact two's-complement bit delta without signed arithmetic. |
| WR-523 | MSVC size atomics | `size_t` subtraction first narrowed an unsigned decrement to a signed native integer and then negated it, making large decrements implementation-dependent or undefined. The modulo delta is computed in `size_t` before the width-specific intrinsic call. |
| WR-524 | D3D11 float-SRV growth | A corrupted current morph-buffer capacity above the typed-buffer limit was used as the growth seed and could force an impossible or overflowing allocation. Invalid capacities now reset to the bounded initial growth policy. |
| WR-525 | D3D11 dynamic-buffer cache | Vertex/index buffer reuse trusted the backend's byte-capacity field without checking the live resource. `ByteWidth`, usage, exact bind class, CPU access, misc flags, and stride must now match before mapped reuse. |
| WR-526 | D3D11 dynamic bind input | The resize helper accepted arbitrary or combined bind flags even though its mapped-update contract supports only vertex or index buffers. Unsupported bind classes now fail before resource creation. |
| WR-527 | D3D11 poisoned growth | An invalid cached buffer could still seed the next geometric allocation with stale, attacker-sized metadata. Only a descriptor-validated cache contributes its prior capacity. |
| WR-528 | D3D11 dynamic publication | A successful driver/proxy buffer creation was published without checking that its native descriptor matched the requested capacity and update mode. Replacement resources are now re-read and validated before the old cache is released. |
| WR-529 | D3D11 snapshot cache | Presented-color snapshots were reused from width/height metadata alone. The cached texture must now also match exact format, subresources, sampling, usage, bind, CPU-access, and misc fields. |
| WR-530 | D3D11 snapshot publication | A newly created snapshot was trusted solely from HRESULT and non-null output. Its complete descriptor is now validated before it can replace the last usable snapshot. |
| WR-531 | D3D11 morph backing buffer | Morph uploads trusted a float capacity that could overstate the backing buffer and authorize an out-of-range `UpdateSubresource` box. The native byte width must now equal exactly `capacity * sizeof(float)`. |
| WR-532 | D3D11 morph SRV range | A cached morph SRV could expose the wrong type, an offset, or fewer elements while still being rebound. Reuse now requires an exact R32_FLOAT full-buffer view starting at element zero. |
| WR-533 | D3D11 morph resource pairing | A structurally valid SRV could name a different buffer from the cached upload target. `GetResource` identity is checked and its temporary COM reference is released on every path. |
| WR-534 | D3D11 morph publication | New morph buffer/SRV pairs were published immediately after creation. The complete pair, its descriptors, capacity, and resource identity are now validated transactionally first. |
| WR-535 | D3D11 rasterizer output | Base rasterizer creation returned a failed HRESULT without releasing a faulty driver's partial output. The helper now clears every failed or successful-null output before returning. |
| WR-536 | D3D11 biased rasterizer output | Per-draw biased rasterizer creation had the same direct-return leak and could retain a partial state outside the cache. It now enforces the owned-output cleanup contract. |
| WR-537 | D3D11 constant-buffer output | Constant-buffer creation was the third direct helper that leaked a partial COM object on failure. It now releases partial output and requires a non-null successful buffer. |
| WR-538 | Win32 thread joins | Four server teardown paths treated every return from `WaitForSingleObject` as a completed join and then freed borrowed state. A shared helper accepts only `WAIT_OBJECT_0` before cleanup. |
| WR-539 | Win32 self joins | A server stopped from its own accept thread could wait forever. The helper recognizes the current thread by native thread ID, closes the owned handle, and reports the self-stop case distinctly. |
| WR-540 | Win32 join ownership | Invalid non-thread handles and wait failures could leak the supposedly owned handle. Failure paths now preserve a deterministic error while attempting the matching close. |
| WR-541 | HTTP teardown | Plain HTTP retained its own unchecked wait/close sequence. It now uses the checked join boundary and aborts before unsafe state cleanup if synchronization fails. |
| WR-542 | HTTPS teardown | HTTPS duplicated the same unchecked lifecycle and now follows the identical checked ownership rule. |
| WR-543 | WebSocket teardown | Plain WebSocket teardown could deadlock on self-stop or continue after a failed native wait. It now uses the shared exact join result. |
| WR-544 | secure WebSocket teardown | WSS had the fourth raw wait/close sequence and now shares the checked self/failure handling. |
| WR-545 | installer package snapshot | The host read its running installer through a sharing mode that allowed another process to modify or replace it between hashing, overlay parsing, and extraction. `CreateFileW` now denies write/delete sharing while the complete package snapshot is retained. |
| WR-546 | installer package reads | Stream positioning did not provide one exact native read contract for truncation, growth, short reads, and close failure. The host now bounds `GetFileSizeEx`, loops exact `ReadFile` chunks, probes EOF, and checks `CloseHandle`. |
| WR-547 | installer log short writes | A successful zero/oversized `WriteFile` result returned failure without a reliable error, so diagnostics could report stale `GetLastError` state. Short writes now publish `ERROR_WRITE_FAULT`. |
| WR-548 | installer log failure recovery | Throwing on a log append failure while retaining the bad handle made rollback logging throw repeatedly and could interrupt recovery. The failed log is closed and disabled before the first error escapes. |
| WR-549 | installer BOM diagnostics | A successful short BOM write could reuse an unrelated prior error, while flush failure was folded into the same ambiguous condition. Exact write and flush results now select deterministic diagnostics and close the failed log. |
| WR-550 | installer validator clock | Child timeouts used adjustable wall-clock UTC, so a clock correction could extend or prematurely expire validation. Process budgets now use a monotonic `Stopwatch`. |
| WR-551 | installer validator descendants | Timeout and output-limit cleanup killed only the root validator child, allowing installer/compiler descendants to retain files, pipes, or mutations. Cleanup now invokes the Windows process-tree terminator for the exact root PID. |
| WR-552 | installer validator termination | The tree terminator itself could hang or fail silently. Its launch, ten-second exit, and exit code are checked before validation continues. |
| WR-553 | installer validator reap | Root reaping after termination was ignored, allowing the next lifecycle phase to race a still-live process. A second bounded wait now proves the root handle is signaled. |
| WR-554 | demo diagnostics | A failed demo compilation reran the complete compiler solely to recover stderr, doubling time and potentially changing artifacts or diagnostics. One combined captured invocation is now authoritative. |
| WR-555 | demo publication unit | Executables and assets were updated separately, so a failure could expose a new executable with old/partial assets. A complete private directory is smoke-tested and moved into place as one generation. |
| WR-556 | demo publication rollback | Replacing a published demo did not preserve one complete prior directory across a failed move. Publication moves the old generation to a unique sibling backup and restores it on any commit failure. |
| WR-557 | demo stale assets | Rebuilding after removing an asset directive left the old file live because staging copied into the existing output tree. Whole-directory replacement makes the manifest's current asset set exact. |
| WR-558 | demo smoke generation | Smoke runs independently recopied project assets, so the tested generation could differ from the one later published. The runner now copies only the already completed staged directory. |
| WR-559 | demo smoke resources | A child could emit redirected output without bound, exhausting disk while the driver waited. Separate stdout/stderr limits are enforced during and after execution with a configurable 1 MiB default. |
| WR-560 | demo cleanup and deadlines | Smoke timeouts used one blocking wait and clean used recursive deletion that could traverse a reparse point. Monotonic polling now bounds output and process-tree termination, while root/child reparse validation removes each owned entry without following indirection. |
| WR-561 | D3D11 textures | Successful `CreateTexture2D` calls were trusted without proving that the returned native object retained the requested dimensions, mip count, format, usage, bind flags, CPU access, and misc flags. A shared exact descriptor check now gates publication and reuse. |
| WR-562 | D3D11 views | RTV, DSV, and SRV creation checked only HRESULT and pointer presence, so a faulty proxy could return a view of another resource. Every accepted view now proves backing-resource identity through `GetResource`. |
| WR-563 | D3D11 color targets | Scene/post-FX color helpers could publish a malformed texture, wrong RTV mip/format, or narrowed SRV. Texture, RTV, and optional SRV descriptors are now validated as one creation transaction. |
| WR-564 | D3D11 depth targets | Depth allocation did not confirm typeless/typed storage, the D32 view shape, or the optional R32 sampling view. All three native contracts are checked before a depth target can become live. |
| WR-565 | D3D11 staging | A successful staging allocation could return a texture that was not single-subresource, CPU-readable, or unbound. The exact staging descriptor is now required. |
| WR-566 | D3D11 readback cache | Width/height/format bookkeeping alone authorized staging reuse even if the COM resource no longer matched that metadata. Cache hits now re-read and validate the native descriptor. |
| WR-567 | D3D11 swapchain | The backbuffer was released before its new RTV could be checked against that exact resource. It remains retained through view identity/format/mip validation, then releases only after the pair is proven. |
| WR-568 | D3D11 RGBA uploads | Streamed RGBA texture allocation trusted the created mip-chain texture and full-chain SRV. Both descriptors and their pairing now gate the upload cache entry. |
| WR-569 | D3D11 native textures | BC-family upload allocation had the same unchecked texture/SRV publication path. Exact compressed format, mip range, usage, and resource identity are now required. |
| WR-570 | D3D11 cubemaps | Cubemap creation did not confirm six-slice cube storage or a full-range `TEXTURECUBE` view. Both contracts are now checked before upload starts. |
| WR-571 | D3D11 depth copies | Opaque-depth resolve trusted CPU-tracked target dimensions before `CopyResource`. It now checks the source texture's native extent, subresources, format family, sampling, usage, and CPU-access policy first. |
| WR-572 | D3D11 opaque-depth cache | Cached opaque-depth texture/SRV reuse trusted width/height fields and non-null pointers. The exact texture descriptor, SRV range, and backing identity are revalidated; a bad pair is replaced transactionally. |
| WR-573 | D3D11 timing | Timestamp and disjoint query factories could return the wrong query object behind a successful result. `GetDesc` now proves all three query types and zero misc flags before telemetry begins. |
| WR-574 | D3D11 constant buffers | Constant-buffer creation checked size before the call but not the returned object's native usage/bind/access descriptor. Exact descriptor validation now precedes publication. |
| WR-575 | D3D11 mesh buffers | Immutable cached vertex/index buffers were accepted without confirming immutable usage, byte width, or bind kind. The returned descriptor must now equal the request. |
| WR-576 | D3D11 skybox buffer | The standalone skybox buffer bypassed the shared immutable-buffer helper and retained the same unchecked-output risk. Its descriptor is now independently validated and failed resources are released. |
| WR-577 | D3D11 fixed samplers | Linear-wrap, linear-clamp, and shadow-comparison samplers trusted factory outputs. Filtering, addressing, LOD, and every mode-dependent comparison, border, and anisotropy field are now validated before publication. |
| WR-578 | D3D11 material samplers | Lazy sampler-cache hits trusted only the derived cache indices, so corrupted or proxy-normalized state could persist. Each hit is checked against the reconstructed descriptor and evicted on mismatch; new state is validated too. |
| WR-579 | D3D11 fallback resources | White 2D, white cube, and BRDF LUT resources were published after pointer checks alone. All three textures and three SRVs now prove descriptor and backing-resource identity before the fallback set replaces live state. |
| WR-580 | D3D11 telemetry | `texture_fallback_binds` wrapped to zero after `UINT64_MAX`, making long-running diagnostics non-monotonic. Both 2D and cubemap fallback paths now use saturating addition. |
| WR-581 | PE validation ownership | Packaging verification and the installer host maintained separate, drifting PE parsers. One zero-dependency `WindowsPEValidation` implementation now owns the native loader-facing policy. |
| WR-582 | PE header bounds | Earlier parsers performed only shallow `MZ`/`PE` checks and left several widened offset/length relationships implicit. Every DOS, COFF, optional-header, section-table, and field read is now range-checked before access. |
| WR-583 | PE image kind | A file with an unexpected subsystem, missing executable characteristic, DLL flag, PE32 magic, or unsupported machine could reach later installer logic. Only non-DLL x64/ARM64 PE32+ GUI/console executables are accepted. |
| WR-584 | PE section inventory | Zero or pathological section counts and a section table not covered by `SizeOfHeaders` were not uniformly rejected. Counts are capped at 96 and both file and header coverage are proven. |
| WR-585 | PE loader alignment | Section/file alignment was only partially checked. Power-of-two, ordering, sub-page equality, and documented 512–65536 file-alignment rules are now enforced. |
| WR-586 | PE image extents | Zero, misaligned, or undersized `SizeOfImage`/`SizeOfHeaders` values could survive shallow verification. Header and aligned virtual extents now have to fit their declared loader image. |
| WR-587 | PE raw sections | Raw section offsets/sizes could be misaligned, overlap headers, or escape the snapshot in some consumers. Each nonempty range is aligned and bounded, while empty sections must use a zero raw offset. |
| WR-588 | PE raw overlap | One parser compared raw ranges, but others did not and none shared a single policy. Sorted nonempty raw ranges are now proven disjoint everywhere. |
| WR-589 | PE virtual overlap | Overlapping loader RVAs were not rejected. Mapped ranges use `max(VirtualSize, SizeOfRawData)`, remain within `SizeOfImage`, align safely, and cannot overlap. |
| WR-590 | PE entry point | A nonzero entry point was considered sufficient even if it fell outside all sections or into data. It must now belong to an executable section. |
| WR-591 | PE directories | `NumberOfRvaAndSizes` and directory pairs were inconsistently bounded. Counts must fit the optional header, zero address/size pairs must be complete, and every non-security directory must fit one mapped range. |
| WR-592 | Authenticode | The security directory uses file offsets rather than RVAs and was either treated generically or barely checked. Its offset/size must be 8-byte aligned, in-file, outside headers, and disjoint from raw sections. |
| WR-593 | package verifier | `verifyPE` retained its weaker local parser after installer-host hardening. It now delegates to the shared validator while preserving package-oriented diagnostics. |
| WR-594 | installer host | Embedded host and cleanup architecture checks duplicated structural parsing and could drift from package verification. Host extraction now validates the complete shared PE contract with the metadata-required machine. |
| WR-595 | native installer verifier | Its final machine helper defaulted every unknown architecture string to x64 and reread offsets shallowly. Unsupported metadata is explicit and the shared validator now performs the complete expected-machine check. |
| WR-596 | script PE snapshots | Automation parsers opened mutable aliases and inspected only a few header fields. The shared PowerShell validator rejects reparse/hard-link aliases and holds one write-denying read handle for its complete bounded inspection. |
| WR-597 | script PE policy | Demo, Studio, package-wrapper, and lifecycle-validator scripts carried four divergent architecture readers. All four now dot-source the same PowerShell mirror of the native loader policy. |
| WR-598 | Windows stdio sharing | Runtime read, write, and replacement-temp streams used `_wfopen`, permitting conflicting opens during persistence. `_wsopen_s` now denies writers for read snapshots and denies all sharing for mutable outputs. |
| WR-599 | Windows stdio errors | `_fdopen` failure closed the descriptor and could overwrite the useful conversion error. Runtime, GUI, audio, and TLS helpers now preserve `errno` across cleanup. |
| WR-600 | atomic replace errors | Failed `MoveFileExW` calls exposed inconsistent or stale CRT error values. Documented Win32 failures now map deterministically to `errno`. |
| WR-601 | durable saves | Atomic writers used `fflush` plus `fclose`, which did not request a durable OS flush and sometimes lost the first failure. A shared helper performs flush, `_commit`/`fsync`, and close while preserving the earliest error. |
| WR-602 | file-mode parsing | `rt_file_mode_to_flags` cleared caller output on invalid mode even though its header promised output preservation. Failure now leaves the caller's prior flags untouched. |
| WR-603 | persistence writers | Pixels binary/PNG, tile maps, game persistence, light-baker caches, and VSCN scenes each had a nondurable close path. All six now use the durable close helper before atomic replacement. |
| WR-604 | VSCN save cleanup | Its `fflush(...) != 0 \|\| fclose(...) != 0` expression short-circuited, leaking the stream whenever flush failed. The shared close transaction always attempts every required cleanup step. |
| WR-605 | TLS custom roots | The Windows custom-CA loader could read a file while another process rewrote it between length and payload checks. It now opens the Unicode path through `_wsopen_s` with write sharing denied. |
| WR-606 | library file reads | GUI and audio decoders used wide CRT opens without a stable snapshot policy. Their Windows adapters now use non-inheritable descriptors and deny concurrent writers. |
| WR-607 | WASAPI channel masks | Extensible format negotiation did not require the channel-mask bit count to equal `nChannels`. Cardinality is now validated before accepting the mix format. |
| WR-608 | WASAPI multichannel | Legacy or maskless formats with more than two channels could be accepted even though routing was undefined. Multichannel output now requires `WAVEFORMATEXTENSIBLE` with a nonzero valid mask. |
| WR-609 | WASAPI stereo routing | Mixing always wrote channels zero and one, which is wrong when FL/FR occupy other slots in a surround mask. Their exact channel indexes are derived from the mask and used during render. |
| WR-610 | WASAPI legacy formats | Non-extensible formats above stereo could reach the render loop with an assumed layout. They are now rejected during negotiation. |
| WR-611 | WASAPI zero-copy | The fast path assumed left/right were the first two interleaved samples. It is enabled only when the validated FL/FR indices are exactly zero and one. |
| WR-612 | WASAPI initialization | `InitializeCriticalSection` provided no failure result and could terminate the process on allocation failure. `InitializeCriticalSectionEx` is checked and initialization unwinds cleanly. |
| WR-613 | WASAPI events | Generic `CreateEvent` spellings depended on build-wide character macros. Explicit `CreateEventW` calls now make the Unicode ABI invariant local and testable. |
| WR-614 | WASAPI clock | Converting the complete long-running QPC counter through MSVC's double-width `long double` lost sub-millisecond precision over time. Whole seconds are split first and only the remainder uses floating-point conversion. |
| WR-615 | WASAPI self-join | Teardown could wait on its own render thread and deadlock. The captured worker ID is compared against the current thread before any join. |
| WR-616 | WASAPI join failure | A failed thread-handle wait still allowed platform memory to be released while the worker could be live. Teardown now requests stop and waits for an independently published exit flag before freeing. |
| WR-617 | WASAPI exit publication | Not every worker return path announced completion. All exits now set the shared `thread_exited` state. |
| WR-618 | WASAPI shutdown ownership | Non-owner shutdown could mutate COM and synchronization state from the wrong apartment. Shutdown rejects the call before teardown and reports failure. |
| WR-619 | audio context lifetime | `vaud_destroy` freed the public context even when platform shutdown could not safely complete. It now clears the destroy-in-progress flag and retains ownership for a valid later owner-thread retry. |
| WR-620 | Win32 window text | Two window-long calls still used `GetWindowLongA`/`SetWindowLongA` inside an otherwise Unicode window adapter. They now use the wide API consistently. |
| WR-621 | machine memory | `GlobalMemoryStatusEx` unsigned 64-bit byte counts narrowed directly to signed runtime values. Physical and available memory now saturate at `INT64_MAX`. |
| WR-622 | Windows version fallback | The compatibility fallback used `GetVersionExA` despite carrying no narrow text. It now uses `OSVERSIONINFOW` and `GetVersionExW`. |
| WR-623 | native file dialogs | UTF-8-to-UTF-16 allocation multiplication and multi-select result count were unbounded. Conversion checks `SIZE_MAX`, and hostile shell extensions cannot request more than 65,536 result pointers. |
| WR-624 | widget file dialog namespaces | Absolute-path classification accepted incomplete UNC roots and Win32 device namespaces such as `\\.\PhysicalDrive0` or unsupported `\\?\GLOBALROOT`. Only complete drive, UNC, extended-drive, and extended-UNC roots are accepted. |
| WR-625 | widget home directory | An existing relative `USERPROFILE` value was accepted relative to the process working directory. The environment candidate must now be a recognized absolute Windows path. |
| WR-626 | widget directory listing | A hostile or enormous directory could grow the dialog entry array until exhaustion. Enumeration now fails closed after a one-million-entry ceiling. |
| WR-627 | demo confinement | Trimming trailing separators turned drive/UNC roots into malformed bases and broke ancestry checks. Root paths are preserved and prefix construction handles an existing separator. |
| WR-628 | demo metadata reads | Manifests, CMake cache/system files, and project files were read without byte or line ceilings and with permissive decoding. Bounded strict-UTF-8 readers now reject oversized, unstable, or malformed automation input. |
| WR-629 | demo asset paths | Asset source/target components could contain rooted forms, control characters, alternate data streams, illegal characters, dot segments, or trailing dots/spaces. One sanitizer rejects every unsafe Windows component before path resolution. |
| WR-630 | demo reserved names | DOS device aliases such as `CON.txt`, `NUL`, and `COM1` could pass ordinary path confinement. Component validation now rejects reserved base names case-insensitively. |
| WR-631 | CMake cache reads | Architecture lookup streamed an unbounded cache. The cache must now be a regular bounded UTF-8 file with a single usable value. |
| WR-632 | CMake generated metadata | Recursive discovery could traverse an enormous build tree and trusted conflicting system descriptions. An iterative, count/size-bounded scan rejects reparse points and conflicting processors. |
| WR-633 | source-tree indirection | Demo and Studio source validation used recursive enumeration that could follow reparse points. Iterative bounded tree walks reject indirection before compilation. |
| WR-634 | architecture proof | Empty or unknown CMake processor metadata silently passed, allowing a wrong-architecture tree to be reused. Both drivers now require a recognized, exact target architecture. |
| WR-635 | CMake generator selection | Explicit fresh build directories omitted `-A`, leaving Visual Studio architecture to ambient defaults. Default and explicitly selected Visual Studio generators now receive x64/ARM64 explicitly. |
| WR-636 | build revalidation | Architecture was checked only before reuse, not after configure/build could alter the tree or executable. Cache metadata and the complete PE are revalidated after every tool build. |
| WR-637 | executable provenance | A discovered `zanna.exe` without a corresponding CMake cache was accepted. Both demo and Studio drivers now reject binaries whose build-tree architecture cannot be proven. |
| WR-638 | demo manifests | Manifest names and project ownership were not fully canonicalized, so case aliases or one project under multiple executable names could collide. Case-insensitive name/output and unique-project ownership are enforced. |
| WR-639 | demo asset ownership | Assets could case-alias one another or overwrite the newly generated executable. A case-insensitive ownership map reserves the executable destination before staging. |
| WR-640 | asset snapshot copy | Ordinary copy operations could consume a changing source or overwrite a staged destination. Each source is held with write sharing denied and each destination uses `CreateNew` plus an explicit durable flush. |
| WR-641 | post-asset executable | Asset staging was trusted not to change the generated PE. The complete architecture/loader validation runs again after every asset is staged. |
| WR-642 | demo staging names | Random staging paths were assumed absent. Every path is confined, reparse-checked, and required not to exist before use. |
| WR-643 | demo rollback paths | Publication generated a sibling backup but did not preflight its complete ancestry and no-clobber contract. The private rollback path is confined and validated before moving the live generation. |
| WR-644 | precommit demo validation | A staged tree could change after smoke but before its directory move. The tree and executable are validated again immediately before publication. |
| WR-645 | postcommit demo validation | Backup deletion occurred without proving the moved generation survived intact. Published tree/PE validation now completes first, and any failure restores the prior directory. |
| WR-646 | demo output discovery | Final reporting recursively traversed the entire shared `examples/bin` tree. Output listing is now bounded to the manifest-owned published executable paths. |
| WR-647 | Studio provenance reads | Version/build/CMake metadata used unbounded default decoding and tolerated unknown generated processors. Strict byte ceilings, UTF-8 decoding, bounded traversal, and exact architecture proof now apply. |
| WR-648 | Studio buildinfo | Staged metadata used overwrite semantics and no durable flush. A private `CreateNew` stream now writes and flushes the complete provenance record. |
| WR-649 | Studio publication | Binary/buildinfo publication did not hash the exact staged pair or revalidate the published PE before deleting backups. Staged hashes/architecture are captured, destinations are rechecked byte-for-byte, and failure rolls back both files. |
| WR-650 | Studio compatibility output | The compatibility copy had weaker staging and rollback guarantees than the primary output. It now uses the same no-clobber staging, architecture, hash, paired publication, and postcommit validation transaction. |
| WR-651 | native UCRT imports | The stable-snapshot file helpers introduced `_wsopen_s`, but the fixed Windows native-link policy did not map that UCRT export. The planner now maps it to the selected release/debug UCRT and pins its Windows-only scope. |
| WR-652 | native event imports | Explicit `CreateEventW` removed character-macro ambiguity in WASAPI but was absent from the native Kernel32 inventory. The Unicode export is now mapped and regression-tested. |
| WR-653 | native window reads | Replacing `GetWindowLongA` with the Unicode API compiled but left native Studio with an unmapped import. `GetWindowLongW` now maps explicitly to User32. |
| WR-654 | native window writes | `SetWindowLongW` had the matching native-link gap. It now maps explicitly to User32, and the planner suite rejects all four new imports on non-Windows targets. |
| WR-655 | audio teardown tests | The core audio test double retained the old `void` shutdown contract, so the full warning-as-error build caught a late signature mismatch that focused runtime targets missed. The stub now returns status, and a behavioral regression proves failed destroy retains a retryable context before successful teardown. |
| WR-656 | D3D11 sampler canonicalization | Real D3D11 hardware reports `MaxAnisotropy == 0` after creating a non-anisotropic sampler even when the valid input descriptor uses one, so byte-for-byte validation rejected working devices. Sampler checks now remain exact for behavior-bearing state but condition anisotropy, comparison, and border equality on the modes that use those fields; bounded mismatch diagnostics preserve debuggability. |
| WR-657 | Windows CTest reliability | The strict full-tree platform-policy CTest had only a 60-second timeout and flaked while the complete parallel Windows Debug suite saturated process startup and storage, despite passing standalone in 10.55 seconds and again in the wrapper. Its bound now preserves three minutes of contention headroom without weakening the lint or pass expression. |
| WR-658 | GC synchronization | The process-global garbage-collector lock used `InitializeCriticalSection`, whose allocation failure raises an exception instead of producing a checkable result. One-time initialization now uses checked `InitializeCriticalSectionEx` and aborts before publishing an unusable lock. |
| WR-659 | string interning synchronization | The process-global intern-table lock had the same exception-only initialization contract. Its one-time callback now returns the checked result and first use aborts deterministically on failure. |
| WR-660 | Game3D model cache synchronization | Model-cache startup assumed its critical section could not fail. It now checks `InitializeCriticalSectionEx` before reporting successful one-time initialization. |
| WR-661 | Game3D surface synchronization | The global surface registry could publish an uninitialized critical section. Its `InitOnce` callback now propagates the checked native result and callers abort on initialization failure. |
| WR-662 | asset registry synchronization | Asset-cache one-time initialization used the exception-only critical-section API. It now returns the checked `InitializeCriticalSectionEx` result through the already fail-closed caller contract. |
| WR-663 | ZPAK synchronization | Reader-lock allocation could return a live wrapper after exception-only native initialization. The checked API now frees the wrapper and reports allocation failure without publishing it. |
| WR-664 | HTTP client synchronization | HTTP-client mutex construction always reported success even when Windows could not allocate critical-section state. The platform macro now returns the checked initialization result to existing cleanup paths. |
| WR-665 | HTTP server synchronization | HTTP-server mutex construction had the same unconditional-success macro. It now propagates `InitializeCriticalSectionEx` failure instead of entering an invalid lock. |
| WR-666 | HTTPS server synchronization | HTTPS-server mutex construction likewise converted an exception-only initialization into a checked success value. Allocation failure now follows the server's ordinary initialization unwind. |
| WR-667 | HTTP pool synchronization | An unused `HTTP_POOL_MUTEX_INIT` macro still encoded exception-only initialization and could be copied into a future call site. The dead, unsafe abstraction was removed. |
| WR-668 | HTTP connection-pool synchronization | The live pool-mutex wrapper always returned success after `InitializeCriticalSection`. It now returns false when checked native initialization fails. |
| WR-669 | REST client synchronization | REST-client mutex initialization could not report allocation failure to its callers. It now uses the checked Windows API and preserves the existing portable boolean contract. |
| WR-670 | SMTP synchronization | SMTP mutex initialization had the same unconditional-success behavior. A failed Windows critical-section allocation now fails client construction cleanly. |
| WR-671 | SSE synchronization | Server-sent-event mutex initialization could expose an unusable lock. It now propagates the checked native result. |
| WR-672 | WebSocket server synchronization | The WebSocket server macro always returned zero after exception-only initialization. It now maps `InitializeCriticalSectionEx` success/failure to the server's zero/negative contract. |
| WR-673 | secure WebSocket synchronization | The WSS server carried the same unchecked initialization path. It now returns a deterministic initialization error that existing teardown handles. |
| WR-674 | regex cache synchronization | The process-global regular-expression cache could publish a failed critical section from its one-time callback. The callback now returns the checked result and first use aborts before accessing the cache. |
| WR-675 | promise synchronization | Promise allocation initialized its mutex without a recoverable failure path. A failed checked initialization now frees the promise and reports allocation failure. |
| WR-676 | parallel-pool synchronization | Process-global parallel-pool initialization could report `InitOnce` success with an unusable lock. The callback propagates `InitializeCriticalSectionEx`, and pool use aborts if the one-time operation fails. |
| WR-677 | parallel `ForEach` synchronization | `ForEach` created its shared error lock with the exception-only API after allocating an event and task array. It now checks initialization and releases both prior allocations on failure. |
| WR-678 | parallel `Map` synchronization | `Map` had the same uncheckable error-lock construction. Its failure path now closes the event and frees every staged allocation before trapping. |
| WR-679 | parallel `Invoke` synchronization | `Invoke` could proceed with an invalid shared error lock. Checked construction now unwinds its event/task state transactionally. |
| WR-680 | parallel `Reduce` synchronization | `Reduce` used exception-only error-lock initialization after allocating reduction state. It now cleans all staged state and fails deterministically if the lock cannot be created. |
| WR-681 | parallel `For` synchronization | Indexed parallel iteration carried the fifth unchecked local error lock. Checked initialization now releases the event and task array before reporting failure. |
| WR-682 | scheduler synchronization | Scheduler task allocation could publish a task whose mutex initialization failed. It now frees the task and returns null after a checked native failure. |
| WR-683 | thread synchronization | The Windows thread wrapper used exception-only critical-section initialization after allocating its native object. It now frees the object and returns null when checked initialization fails. |
| WR-684 | watcher events | The file watcher used generic `CreateEvent`, leaving its ABI spelling dependent on translation-unit character macros. Explicit `CreateEventW` makes the Unicode import invariant local. |
| WR-685 | parallel `ForEach` events | `ForEach` used the generic event macro for its native completion event. It now calls `CreateEventW` explicitly. |
| WR-686 | parallel `Map` events | `Map` had the same character-macro-dependent event call. Its completion event now uses the explicit wide entry point. |
| WR-687 | parallel `Invoke` events | `Invoke` now uses `CreateEventW` instead of relying on ambient `UNICODE` state. |
| WR-688 | parallel `Reduce` events | `Reduce` now pins the Unicode event ABI explicitly. |
| WR-689 | parallel `For` events | Indexed parallel iteration now uses the same explicit `CreateEventW` contract as the other combinators. |
| WR-690 | parallel `ForEach` completion | Its worker callback ignored `SetEvent` failure, allowing the owner to wait forever or later free task state while completion was not observable. The callback now routes through the shared checked signal helper and aborts before unsafe cleanup. |
| WR-691 | parallel `Map` completion | The map worker carried an independent unchecked completion signal. It now uses the checked helper. |
| WR-692 | parallel `Invoke` completion | Invoke workers could silently lose their last completion notification. Signal failure is now treated as an unrecoverable synchronization error. |
| WR-693 | parallel `Reduce` completion | Reduction workers now check event publication through the shared helper instead of discarding the native result. |
| WR-694 | parallel `For` completion | Indexed-loop workers carried the fifth unchecked `SetEvent` path. It now shares the fail-fast completion contract. |
| WR-695 | TLS custom-root contract | The Windows TLS verifier's source contract still claimed `_wfopen` even though WR-605 moved the implementation to deny-write `_wsopen_s`. The stale documentation made the regression test reject the corrected implementation; it now describes the actual stable-snapshot API and sharing policy. |
| WR-696 | D3D11 COM identity | Backend owner and view/resource validation could not safely compare arbitrary COM interface pointers, because one object may expose different interface addresses. A shared helper now compares controlling `IUnknown` identities and balances both temporary references. |
| WR-697 | D3D11 device creation | A successful core-device call required non-null device/context/swapchain outputs but did not prove that the immediate context and swapchain belonged to that device. Both owner identities are now verified before a device attempt can be published. |
| WR-698 | D3D11 swapchain buffers | Resize, presented-snapshot, and readback `GetBuffer` paths validated pointer/descriptor shape but not device ownership. Every accepted backbuffer now proves it is a child of the live backend device. |
| WR-699 | D3D11 depth states | Opaque, transparent, disabled, skybox, and shadow depth-state factories trusted successful outputs. They now validate device ownership and every behavior-bearing depth/stencil descriptor field, while tolerating only documented canonicalization of disabled fields. |
| WR-700 | D3D11 blend states | Opaque, alpha, additive, and premultiplied blend states were not checked against their requested native behavior. Publication now requires owner identity plus alpha-coverage, independence, write-mask, and enabled blend-operation equality for every active render-target slot. |
| WR-701 | D3D11 rasterizer states | Fixed and depth-biased rasterizer creation did not prove device ownership or returned fill, cull, winding, bias, clipping, scissor, multisample, and line behavior. One descriptor builder and exact validator now gate every state. |
| WR-702 | D3D11 biased-state cache | A depth-biased rasterizer cache hit trusted only scalar cache keys and could retain a foreign or malformed COM state. Hits now revalidate owner and descriptor, evict bad state, and recreate transactionally. |
| WR-703 | D3D11 samplers | Fixed samplers checked descriptors but not owning device, and material-cache hits/new objects had the same ownership gap. Both creation paths and every lazy cache hit now require the live device identity. |
| WR-704 | D3D11 executable pipeline objects | Vertex/pixel shaders and six input-layout variants required only successful non-null outputs. All shader and layout objects now prove they were created by the backend device before initialization continues. |
| WR-705 | D3D11 queries and buffers | Timing queries and the standalone skybox vertex buffer validated type/descriptor but not native owner. Their factory transactions now reject foreign-device children before publishing telemetry or draw state. |
| WR-706 | D3D11 fallback resources | White 2D/cube textures and views plus the BRDF LUT validated shapes and resource/view pairing but not device ownership. Every fallback child now also proves it belongs to the current device before the set is published. |
| WR-707 | installer cleanup-helper sharing | The installer reopened its detached helper with delete access while sharing writes and deletes, so another process could mutate or replace the executable between materialization and launch. The held launch snapshot is now read-only and shares only reads. |
| WR-708 | installer cleanup-helper snapshot | Closing and reopening the staged helper created a window in which its bytes could change without detection. The reopened, mutation-denying handle now verifies exact size and contents against the trusted embedded helper bytes before launch. |
| WR-709 | installer cleanup-helper indirection | The reopened helper path did not explicitly reject a directory or reparse-point substitution. It now opens the entry itself with `FILE_FLAG_OPEN_REPARSE_POINT` and validates handle attributes before execution. |
| WR-710 | installer cleanup-helper activation | A failed `ResumeThread` issued an unchecked termination and immediately entered exception cleanup, potentially deleting state while a suspended process still existed. Termination and a bounded reap are both required before cleanup is allowed. |
| WR-711 | installer cleanup-helper lifetime | Later launch failures could enter the catch block without proving that the helper process had exited, while an already-exited process could receive a misleading termination attempt. Explicit running/exited state now gates termination and filesystem cleanup. |
| WR-712 | installer read-only cleanup | A failed deletion permanently removed the target's read-only attribute, mutating a file that cleanup did not own successfully. The original attributes are restored after each failed attempt, with concurrent disappearance treated as success. |
| WR-713 | installer reboot cleanup | `MoveFileExW(..., MOVEFILE_DELAY_UNTIL_REBOOT)` was unchecked, yet the log always claimed cleanup had been scheduled. Scheduling failure is now reported with its native error and the cache remains available for later repair. |
| WR-714 | installer cleanup idempotence | A file that disappeared after attribute inspection but before `DeleteFileW` was treated as a cleanup failure unless it had originally been read-only. Both post-inspection not-found results are now idempotent success. |
| WR-715 | D3D11 native GUID linkage | The new COM identity checks compiled under MSVC but referenced `IID_IUnknown` and `IID_ID3D11Device` as external `dxguid` data, which the zero-dependency native linker intentionally does not import. SDK-identical file-local GUID constants now preserve the checks without expanding the native import surface. |
| WR-716 | embed IPC layout | Frame-slot and complete mapping sizes were derived with unchecked multiplication/addition. A single checked layout helper now rejects invalid dimensions and every `size_t` overflow before mapping or copying. |
| WR-717 | embed IPC Windows ownership | `CreateFileMappingA` could open an existing named section and let a second host reinitialize live shared memory. `ERROR_ALREADY_EXISTS` now fails creation without touching the established channel. |
| WR-718 | embed IPC POSIX ownership | Host creation unconditionally unlinked the name before `O_EXCL`, allowing a second host to hide a live channel and create a split-brain replacement. Creation now leaves existing objects intact and fails closed. |
| WR-719 | embed IPC atomics | `memset` was treated as initialization for C11 atomic objects. Every shared atomic now receives `atomic_init` before either process may access it. |
| WR-720 | embed IPC portability | The protocol assumed process-shared atomics were lock-free; a library-backed atomic may contain process-local synchronization state. Creation and attachment now require every shared atomic type to be lock-free. |
| WR-721 | embed IPC publication | The magic sentinel was written before version, bounds, and atomic state, so a concurrent attach could accept a partially initialized header. The sentinel is now published last after initialization and a release fence. |
| WR-722 | embed IPC POSIX probing | Attachment mapped a complete header without proving the shared-memory object was that large, permitting a truncated object to fault on access. `fstat` now verifies the probe extent first. |
| WR-723 | embed IPC POSIX extent | After trusting dimensions from the probe, attachment did not prove the object matched the derived complete layout. It now requires the exact mapping size before the full `mmap`. |
| WR-724 | embed IPC mapping cleanup | Probe-view unmap failures were ignored before remapping the full channel. Both Win32 and POSIX attachment now fail and release handles when the probe cannot be unmapped cleanly. |
| WR-725 | embed IPC remap race | Header metadata was validated only through the temporary probe, leaving a size/replacement race before the full view was used. The established view is revalidated against the captured layout before publication. |
| WR-726 | embed IPC metadata trust | Copy and allocation bounds were repeatedly read from mutable shared header fields. Each handle now retains validated immutable dimensions and rejects later header tampering. |
| WR-727 | embed IPC producer ownership | Any number of game processes could attach and write the same frame slots and input tail. An atomic claim now permits exactly one producer handle. |
| WR-728 | embed IPC attach publication | A new producer could become visible while the prior producer's exited flag was still set. Attachment now reserves an intermediate state, clears stale exit state, then release-publishes the live producer. |
| WR-729 | embed IPC restart state | Reattaching after an orderly producer exit retained `producer_exited`, making a healthy restarted preview look dead. Successful exclusive attachment now resets it. |
| WR-730 | embed IPC detach state | Producer close marked exit but left `producer_attached` true forever. Close now publishes exited, clears attached, and relinquishes the local claim. |
| WR-731 | embed IPC explicit exit | `mark_exited` left the same handle authorized to continue publishing. It now atomically exposes exit, revokes attachment, and invalidates the producer claim. |
| WR-732 | embed IPC frame roles | Host handles could publish and producer handles could acquire frames. Frame operations now enforce their protocol roles and require a live producer claim. |
| WR-733 | embed IPC event roles | Either side could push or consume host-to-game input, violating the SPSC assumptions. Push is host-only and poll is exclusive-producer-only. |
| WR-734 | embed IPC control roles | Producer handles could issue host resize requests and producer-side callers could query host-only frame state. Control and inspection operations now validate roles; resize state is published only after its event is queued. |
| WR-735 | embed IPC abandoned frame | A producer interrupted while the sequence was odd left every future frame odd as well. Publication now advances an abandoned odd value to the next valid write transaction. |
| WR-736 | embed IPC sequence wrap | Incrementing a near-`UINT64_MAX` frame sequence could wrap through an invalid completion value. The publisher now restarts through a valid odd/even pair and readers detect the new frame. |
| WR-737 | embed IPC failure outputs | Failed frame acquisition could expose stale width/height values supplied by the caller. Outputs are cleared before validation and remain zero on every failure path. |
| WR-738 | embed IPC dimension snapshot | `frame_size` read width and height without checking the frame sequence, allowing dimensions from different publications. It now performs a bounded stable-even seqlock read with range validation. |
| WR-739 | embed IPC event validation | Invalid event kinds entered the ring, failed polls preserved stale records, and corrupted producer/consumer distance was not detected. Kinds are bounded, outputs clear first, and impossible ring distance fails closed. |
| WR-740 | embed IPC full ring | The host advanced the consumer-owned tail and overwrote a slot the game could be copying, creating cross-process data races. A full ring now rejects the newest event while preserving all published records. |
| WR-741 | D3D11 frame clear | Render-target and depth clears are void D3D11 commands whose device-loss result was never checked. Clear now returns a checked frame status before submission continues. |
| WR-742 | D3D11 begin transaction | Per-frame counters and history could claim a new frame after target clear had already failed. Clear succeeds before those CPU-side completion states are published, and early failure restores pending-present state. |
| WR-743 | D3D11 indexed draws | Main `DrawIndexed` submission could be discarded by a removed device while the frame remained publishable. Device health is checked and latched after the command. |
| WR-744 | D3D11 instanced draws | `DrawIndexedInstanced` had the same unchecked void-command failure. It now participates in the frame failure latch. |
| WR-745 | D3D11 skybox draw | Skybox health checks logged device removal but did not reliably poison the active frame transaction. The draw now records failure through the common frame-status helper. |
| WR-746 | D3D11 constant buffers | Invalid constant-buffer arguments or incompatible native descriptors returned errors without invalidating an active frame, allowing stale constants to be presented. Both contract failures now latch submission failure. |
| WR-747 | D3D11 constant maps | A failed dynamic-constant `Map`, or success with null mapped storage, could leave stale constants while the frame completed. Both paths now poison the frame after safe unmap where required. |
| WR-748 | D3D11 dynamic buffers | Dynamic buffer creation failure or an impossible post-create capacity mismatch merely dropped one draw. The complete frame is now invalidated so partial geometry is never presented as successful. |
| WR-749 | D3D11 dynamic maps | Failed or null dynamic vertex/index/instance mappings could publish a frame missing only some geometry. These failures now latch the frame transaction before returning. |
| WR-750 | D3D11 failed-frame continuation | Later main, instanced, skybox, shadow, and reuse calls continued encoding after a fatal submission error. Every affected entry point now short-circuits once the frame latch is set. |
| WR-751 | D3D11 failed depth probes | `end_frame` issued and published depth-probe work even after rendering failed. Failed frames now discard queued probes without copying or exposing invalid depth. |
| WR-752 | D3D11 failed RTT publication | A failed render-to-texture frame still marked host readback dirty and could replace good pixels with incomplete GPU output. Dirty/HDR state is no longer published for failed frames. |
| WR-753 | D3D11 failed presentation | A failed on-screen frame remained pending for post-processing or Present. `end_frame` clears the pending transaction and presented-color validity before returning. |
| WR-754 | D3D11 shadow entry | Shadow passes could begin after the active frame had already failed, mutating targets and cache state that would never be valid. Begin and reuse now reject failed-frame state. |
| WR-755 | D3D11 shadow atlas | The atlas-cleared flag was set immediately after the void clear command, even if the device discarded it. The flag is published only after checked pass setup succeeds. |
| WR-756 | D3D11 shadow setup | Per-slot and atlas render-target/shader setup used only void calls and could fail through device removal without marking the pass. Both setup families now record device status and shadow failure. |
| WR-757 | D3D11 shadow completion | A removed-device shadow draw could still make `shadow_end` mark its slot complete and reusable. Draw health now latches both frame and shadow-pass failure; later shadow work is suppressed. |
| WR-758 | D3D11 IBL telemetry | A cubemap identity whose IBL upload had failed remained reported as pending forever and was reconsidered on every refresh. Failed identities now clear pending bytes and are excluded until identity changes. |
| WR-759 | installer cleanup UTF-16 | Cleanup accepted unpaired surrogates in deletion targets, creating strings with no stable Win32 interpretation. The policy now validates every UTF-16 pair before parsing a path. |
| WR-760 | installer cleanup extended paths | Extended-length paths accepted forward-slash aliases even though the `\\?\` namespace is interpreted literally. Extended drive and UNC spellings now require backslashes throughout. |
| WR-761 | installer cleanup devices | `CONIN$` and `CONOUT$` were not rejected as reserved console devices. Both aliases now fail the same component check as `CON`, `NUL`, and `CLOCK$`. |
| WR-762 | installer cleanup equality | Deduplication compared only ASCII case, so Unicode-equivalent Windows path spellings could execute cleanup twice. Components now use locale-independent `CompareStringOrdinal` case-insensitive equality. |
| WR-763 | installer cleanup namespaces | Ordinary and extended drive/UNC spellings of the same target compared different. Safe namespace prefixes are normalized before component-wise equality. |
| WR-764 | installer cleanup PID grammar | `wcstoul` accepted leading whitespace and a plus sign in the trusted parent-process option. A digits-only, nonzero decimal parser now defines the command-line contract. |
| WR-765 | installer cleanup PID range | PID conversion depended on CRT `errno`/width behavior and did not deterministically clear output. Checked `uint32_t` accumulation rejects overflow and leaves zero on failure. |
| WR-766 | Windows package versions | The PE resource builder stripped any `-`/`+` suffix without validating the complete package version, allowing packages the installer could not consume. It now validates the full SemVer-compatible text first. |
| WR-767 | Windows VERSIONINFO | Zero-padded numeric core components produced noncanonical package identities. Resource-version parsing now rejects them before emitting PE metadata. |
| WR-768 | Windows package version bounds | Package version parsing was unbounded and used locale-sensitive digit classification. Input is capped at 128 bytes and every core/suffix character is validated as explicit ASCII. |
| WR-769 | Windows prerelease metadata | Empty/double-dot identifiers and zero-padded numeric prereleases were accepted or silently truncated by the resource path. Complete prerelease and build grammar is now enforced. |
| WR-770 | installer version identifiers | `std::isalnum` made accepted prerelease/build bytes depend on the process locale. Installer precedence now recognizes only ASCII letters, digits, hyphen, and period. |
| WR-771 | installer version resource limits | Arbitrarily long versions could drive allocations and parsing in package load/update paths. Installer versions now share the package builder's 128-byte bound. |
| WR-772 | installer version identity | The installer accepted more than four numeric components and values larger than PE `VERSIONINFO`'s 16-bit fields. Both limits now match the package identity exactly. |
| WR-773 | installer host validation | A package with an unusable version could load and reach UI/lifecycle work before a later comparison rejected it. Package load now self-compares the version immediately after metadata parsing. |
| WR-774 | installer minimum OS parser | Minimum-Windows checks reused the package SemVer parser and therefore accepted prerelease/build syntax that has no OS-version meaning. A dedicated one-to-three-part dotted parser now rejects suffixes. |
| WR-775 | installer OS-version arithmetic | OS components were narrowed from native `DWORD` values to signed `int`, and test/metadata spellings allowed zero padding. Comparison now uses canonical `uint32_t` components without narrowing. |
| WR-776 | installer metadata devices | Package leaf-name validation missed `CLOCK$`, console aliases, and the Win32 superscript COM/LPT device forms. All are rejected before archive or shortcut metadata is accepted. |
| WR-777 | installer update signed bytes | CRLF manifests were normalized to LF before signature verification, so accepted transport bytes differed from authenticated bytes. Signed manifests now require canonical LF exclusively. |
| WR-778 | installer update framing | A manifest without a final newline was reconstructed with one for signature verification. The exact downloaded message must now end with LF. |
| WR-779 | installer update JSON | Unknown `UpdateStatus` enum values serialized as `unconfigured`, concealing memory/caller corruption. JSON serialization now uses an exhaustive switch and rejects invalid state. |
| WR-780 | installer update UI | Update presentation treated every non-current/non-unconfigured value as available and offered an open-link action. The dialog path now rejects unknown enum values before constructing controls. |
| WR-781 | installer update result identity | The UI trusted a mutable result record after verification. It now rechecks package version, channel, architecture, and status/version precedence immediately before presentation. |
| WR-782 | installer update result digest | A forged result could reach presentation with an invalid download digest. The final boundary requires exactly 64 lowercase hexadecimal SHA-256 characters. |
| WR-783 | installer update launch origin | Download and release-note URLs were not revalidated at the `ShellExecuteW` boundary. Both are reparsed as HTTPS and required to match the pinned manifest origin. |
| WR-784 | installer update TLS | Update discovery requested only TLS 1.2 even where WinHTTP supports TLS 1.3. It now offers TLS 1.3 with a TLS-1.2 floor and safely retries the floor on older WinHTTP versions. |
| WR-785 | installer update unconfigured state | A forged unconfigured result could carry a different identity or be paired with a package that was actually configured. Presentation now verifies empty configuration and exact package identity. |
| WR-786 | Windows FILETIME epoch | Millisecond and microsecond conversion subtracted the Unix offset in unsigned arithmetic, wrapping any pre-1970 value far into the future. A signed-direction helper now handles both sides of the epoch. |
| WR-787 | Windows FILETIME range | The general conversion helper could narrow a large positive quotient beyond `int64_t`. It now saturates at `INT64_MAX` for every documented divisor. |
| WR-788 | Windows sleep duration | Positive 64-bit sleeps narrowed directly to `DWORD`, could wrap short, or become Win32's infinite sentinel. Large waits are now split into finite `INFINITE - 1` chunks. |
| WR-789 | terminal cursor wrapper | A nonzero i64 such as `2^32` narrowed to zero and hid the cursor. The wrapper canonicalizes boolean truth before crossing the i32 ABI. |
| WR-790 | terminal alternate screen | The alternate-screen wrapper had the same nonzero-to-zero narrowing. It now passes an explicit boolean. |
| WR-791 | terminal sleep wrapper | Negative or large i64 durations could wrap through the i32 adapter. The wrapper now clamps to `[0, INT32_MAX]` deterministically. |
| WR-792 | Windows threadpool build | The threadpool called `rt_win32_deadline_from_now_ms` without including its declaration, producing an implicit declaration and unresolved MSVC link. The Windows wait header is now included in the owning translation unit. |
| WR-793 | Windows audio refill wait | Destruction could wait forever if a refill event signal was lost after the in-progress flag cleared. Windows now polls the event for one millisecond and rechecks protected state. |
| WR-794 | Windows audio event errors | Refill-event reset, signal, wait, and close failures were silently ignored. Every failed operation now publishes a bounded platform diagnostic. |
| WR-795 | WASAPI shutdown signal | Failure to signal the render thread's stop event was ignored. Shutdown now records a backend write failure and reports the platform error before the bounded join. |
| WR-796 | WASAPI client stop | `IAudioClient::Stop` failure disappeared during teardown. The HRESULT is now checked and reflected in diagnostics/statistics. |
| WR-797 | WASAPI handle cleanup | Render, stop, and ready event `CloseHandle` results were discarded. Each failed close is now observable rather than silently leaking native state. |
| WR-798 | Win32 DPI transition | `WM_DPICHANGED` ignored failure to apply the suggested bounds, leaving logical and native sizing out of sync without explanation. `SetWindowPos` failure is now diagnosed. |
| WR-799 | Win32 frame pacing | A waitable timer used an infinite wait, ignored wait/close failures, and slept the full interval again after a timeout. It now uses a finite grace deadline, reports native failures, returns after timeout, and falls back only for immediate failures. |
| WR-800 | Win32 resize constraints | Programmatic resize ignored `GetClientRect` failure, and minimum-size refresh ignored `SetWindowPos` failure. Both operations now fail visibly and avoid publishing guessed backing-store dimensions. |
| WR-801 | Windows native preview imports | The standalone native Studio reached four embed-channel file-mapping symbols that the fixed import planner did not map. `CreateFileMappingA`, `OpenFileMappingA`, `MapViewOfFile`, and `UnmapViewOfFile` now resolve to Kernel32 under ADR 0232. |
| WR-802 | Windows native physics imports | Range-safe 2D physics arithmetic reached `expm1`, `fma`, `frexp`, `log1p`, and `scalbn`, but the Windows planner's maintained UCRT math set omitted them. Release and debug native links now map all five to the appropriate UCRT. |
| WR-803 | Windows CTest scenario table | The VM/native parity test declared a three-element `std::array` after growing to five scenarios, so clean MSVC all-target builds failed with excess initializers. The scenario table now uses a size-deduced native array and cannot drift from its initializer count. |
| WR-804 | Windows warning-as-error fixture | The action-mapping malformed UTF-8 fixture cast `0xC0` and `0xAF` directly to signed `char`; MSVC reports constant truncation and the Windows build treats it as an error. Unsigned bytes are now reinterpreted only at the byte-string API boundary. |
| WR-805 | Windows CTest Python discovery | Git Bash's `command -v python3` accepted a nonfunctional Microsoft Store application alias, causing `benchmark_compare_self_test` to fail despite an installed interpreter. Discovery now executes a probe, honors only a working `PYTHON` override, and falls back through `python3`, `python`, and the standard `py -3` launcher. |
| WR-806 | demo clean authorization | The expected clean directory was assigned from the configured output itself, making the destructive-path equality check tautological. `--clean` now requires the exact conventional `<demo-root>\bin` directory. |
| WR-807 | demo override resolution | Relative external demo-root, output, and manifest overrides depended on the caller's current directory. Every override is now resolved deterministically from the repository root. |
| WR-808 | demo root indirection | External root validation assumed ancestry beneath the repository and could not inspect absolute paths for junctions or symlinks. A volume-rooted ancestor walk now rejects reparse points wherever the configured roots live. |
| WR-809 | demo protected paths | A malicious output root could contain the repository, script directory, or demo source root and make later cleanup own protected content. Configuration now rejects every such containment relationship. |
| WR-810 | demo target build overlap | Demo publication could overlap the target CMake build tree, allowing clean or rollback operations to damage compiler artifacts. Source and output roots must now be disjoint from the target build. |
| WR-811 | demo host-build metadata | External output could overlap the host-tool build or own the manifest file. Both relationships are rejected before any tool build or directory creation. |
| WR-812 | demo source/build overlap | An external demo source root could contain either build tree, letting recursive asset inspection traverse generated compiler content. Source and host/target build roots must now be disjoint. |
| WR-813 | demo cross-architecture trees | Cross-compilation could reuse one CMake tree as both the host compiler and target runtime, invalidating executable provenance. Non-native targets now require distinct canonical trees. |
| WR-814 | demo source identity | A missing source root or a top-level reparse-point directory reached later project discovery with ambiguous errors. The complete configuration now requires one existing ordinary directory. |
| WR-815 | demo manifest encoding | BOM-driven decoder switching allowed UTF-16/UTF-32 or BOM-prefixed UTF-8 inventory files despite the shared manifest's UTF-8 contract. The Windows reader now requires strict BOM-free UTF-8. |
| WR-816 | demo project encoding | `zanna.project` asset and language metadata had the same permissive decoder path. Project metadata now uses the same bounded, strict, BOM-free UTF-8 reader. |
| WR-817 | demo manifest grammar | Windows trimmed rows and accepted uppercase/dotted names that the Unix manifest gate rejected, creating platform-dependent inventories. Rows now forbid surrounding whitespace and require canonical lowercase `[a-z0-9_-]` names. |
| WR-818 | demo category grammar | Category matching was case-insensitive only on Windows. The driver now accepts exactly `games`, `apps`, or `3d`, matching the shared manifest contract. |
| WR-819 | demo project language | A manifest entry could point at a non-Zia project and fail deep in native generation. Preflight now requires an exact `lang zia` declaration before staging. |
| WR-820 | demo Windows path grammar | DEL plus `CLOCK$`, `CONIN$`, `CONOUT$`, and superscript COM/LPT device aliases bypassed the asset/path sanitizer. All are now rejected component-wise. |
| WR-821 | demo nested assets | Top-level asset directives were checked, but recursively discovered child names bypassed the Windows component sanitizer. Every copied child is now validated before destination construction. |
| WR-822 | demo worker bounds | An arbitrarily large `JOBS` value could exhaust process or handle resources. The demo driver now accepts only 1 through 1024 workers. |
| WR-823 | demo host architecture | Missing native-architecture variables produced an indirect null-method failure, and `X86_64` aliases were rejected. Detection now fails explicitly when absent and canonicalizes the common alias to x64. |
| WR-824 | demo configuration names | Build-type validation was unnecessarily case-sensitive even though CMake configuration names are canonicalizable. Accepted spellings are normalized to the four supported names. |
| WR-825 | demo staging rejection | Failure immediately after private staging-directory creation could leave the rejected directory behind. Creation and reparse validation now unwind the private path on failure. |
| WR-826 | demo failure isolation | A project-specific preflight exception aborted the entire inventory rather than contributing one failed demo. Each project now has a guarded build transaction and later entries continue. |
| WR-827 | demo tree termination | A timed-out `taskkill` helper was killed without proving that helper itself was reaped. Both normal and forced helper shutdown now have bounded, checked waits. |
| WR-828 | demo smoke truthfulness | Process-tree termination or smoke-directory deletion could fail while the demo was still reported successful. Either cleanup failure now fails that demo's smoke result. |
| WR-829 | demo live-workspace cleanup | The runner removed its smoke directory even when the child tree might still be alive and using it. Uncertain process teardown now preserves the directory and reports why. |
| WR-830 | demo preflight ordering | Root/manifest failures could occur only after a lengthy Zanna tool build or output mutation. Complete path validation and manifest decoding now precede build discovery and directory creation. |
| WR-831 | Windows build toggles | Nonbinary values such as `true`, `yes`, or `2` silently took inconsistent branches across ten build controls. Every canonical toggle now accepts exactly `0` or `1`. |
| WR-832 | Windows build workers | The canonical build accepted unbounded job counts. Build and CTest concurrency are now limited to the same explicit 1-through-1024 range. |
| WR-833 | Windows Studio policy | Generic extra CMake arguments could override the driver's Studio ON/OFF choice and leave a stale skipped configuration cached into a later full build. The canonical policy flag is now appended last. |
| WR-834 | D3D11 Present status | Any non-failing DXGI status could be treated as proof that a frame reached the display. A shared classifier now accepts only exact `S_OK`, excluding informational statuses such as occlusion. |
| WR-835 | D3D11 present telemetry | On-screen/off-screen present counters advanced even when `Present` failed or returned a non-display status. Counters now advance only after confirmed display-path completion. |
| WR-836 | D3D11 present path | A failed or occluded present retained the prior frame's path classification. The live statistic is now reset to unknown on every unconfirmed presentation. |
| WR-837 | D3D11 stream uploads | `mesh_stream_uploads` advanced before either dynamic vertex or index upload, counting partial/failed work. Publication now follows successful completion of both buffers. |
| WR-838 | D3D11 indexed draws | Main draw-call telemetry advanced even when the post-command device-health check reported removal. It now counts only a healthy `DrawIndexed` submission. |
| WR-839 | D3D11 instanced draws | Instanced draw telemetry had the same post-device-loss overcount. `DrawIndexedInstanced` is counted only after the frame-status check succeeds. |
| WR-840 | D3D11 framebuffer telemetry | `default_framebuffer_writable` was a stale copied field rather than current backend state. Stats snapshots now derive it from the live swapchain/RTV pair. |
| WR-841 | Win32 process termination | Process and ConPTY teardown used unbounded waits after forced termination. A shared helper rejects `INFINITE` and enforces a finite post-termination deadline. |
| WR-842 | Win32 termination races | A child exiting between poll and `TerminateProcess` could turn successful teardown into a false failure. The helper probes before termination and rechecks after a lost race. |
| WR-843 | Win32 exit confirmation | Forced termination assumed the requested code became final without querying the process. Success now requires a signaled object, a readable exit code, and a value other than `STILL_ACTIVE`. |
| WR-844 | Process allocation unwind | Failure to allocate the runtime Process object terminated the already-created child and then waited forever. This rare initialization unwind now uses bounded confirmed teardown. |
| WR-845 | Process finalization | Close always replaced a naturally completed child's real status with synthetic code 1. It now polls first and preserves the exact confirmed exit code. |
| WR-846 | Process wait state | `WAIT_FAILED` marked a possibly live child stopped, preventing finalization from attempting termination. The running flag now remains true until exit is proven. |
| WR-847 | Process exit query | `GetExitCodeProcess` failure silently became `-1` with no runtime diagnostic. The adapter now traps while retaining deterministic output state. |
| WR-848 | ConPTY allocation unwind | ConPTY object-allocation failure used the same potentially infinite post-termination wait. It now shares bounded confirmed process teardown. |
| WR-849 | ConPTY finalization | ConPTY close discarded the native result and reported synthetic status after an unchecked kill. It now polls first, bounds termination, and preserves the confirmed exit code. |
| WR-850 | ConPTY blocking waits | Waiting indefinitely before draining redirected terminal output could deadlock when the pipe filled; wait/query failures also falsely cleared liveness. The loop now drains between short waits and retires state only after proven exit. |
| WR-851 | Win32 mouse capture | Failure to acquire capture left only a false flag with no diagnostic. The adapter now verifies `GetCapture` and publishes a platform error. |
| WR-852 | Win32 capture release | Focus loss, cancel mode, button release, and destruction cleared ownership even when `ReleaseCapture` failed. One checked helper retains truthful state and diagnoses failure. |
| WR-853 | Win32 raw-input teardown | Window destruction ignored failure to unregister the raw mouse device. The result is now checked and reported before state retirement. |
| WR-854 | Win32 cursor teardown | Destruction ignored failure to release process-wide cursor confinement. `ClipCursor(NULL)` is now checked and diagnosed. |
| WR-855 | Win32 cursor confinement | Client-rect query, screen mapping, clip release, and clip application failures collapsed into an unexplained false result. Each native stage now has an exact diagnostic. |
| WR-856 | Win32 relative-input rollback | If clipping failed after raw-input registration, unregister rollback failure disappeared. Rollback is now checked and reported independently. |
| WR-857 | Win32 fullscreen rollback | Failed enter/leave placement ignored both style restores and the rollback `SetWindowPos`. A transactional helper checks both styles and complete bounds and distinguishes rollback failure. |
| WR-858 | installer cleanup parent handle | The detached cleaner could report successful parent synchronization even if its process handle failed to close. Parent-handle close now has a stable failure code. |
| WR-859 | installer cleanup self handles | Both self-rename and self-delete handles were closed without checking the result. Each close is now required for success and has a distinct stable code. |
| WR-860 | installer cleanup rename storage | A byte vector did not guarantee the alignment required by `FILE_RENAME_INFO`. Fixed-size explicitly aligned storage now backs the variable-length record. |
| WR-861 | installer cleanup attributes | Clearing the only `READONLY` bit could pass zero attributes instead of the documented normal-file value. The helper now substitutes `FILE_ATTRIBUTE_NORMAL`. |
| WR-862 | installer cleanup boundary | Command-line storage release failures and C++ allocation/unexpected exceptions could escape or look successful. They now map to observable stable exit codes at `wWinMain`. |
| WR-863 | installer automation output | Failed atomic publication ignored failure to remove its private temporary file. The diagnostic now reports both the publication error and any retained staging path. |
| WR-864 | installer session aliases | Path comparison/open/identity/close failures were interpreted as “not equal,” allowing inaccessible aliases to bypass executable/log/output protection. Every non-missing native failure now fails closed. |
| WR-865 | installer default logs | A timestamp/PID default path was opened with `OPEN_ALWAYS`, so a pre-created temp file could capture the session. Tick-qualified names are now created exclusively; explicit user paths retain append behavior. |
| WR-866 | installer fatal diagnostics | Empty exception strings suppressed the stable message, and allocation failure in diagnostic conversion had no allocation-free stderr path. Empty text now falls back and a fixed UTF-8 record remains available. |
| WR-867 | installer startup boundary | DPI setup, command-line release, help delivery, and pre-package exceptions could fail silently or escape the OS callback. Startup now checks each result and contains all C++ failures while honoring quiet-mode policy. |
| WR-868 | WASAPI join retry stop | Thread-join timeout/error recovery discarded `IAudioClient::Stop` HRESULTs. Both retry paths now record stop failures in diagnostics and backend statistics. |
| WR-869 | WASAPI join retry signal | The same recovery paths ignored stop-event signaling failure. A checked event helper now covers timeout and wait-error retries. |
| WR-870 | WASAPI worker handle | Join returned success and cleared its worker handle even when `CloseHandle` failed. Handle retirement is now conditional and failed close makes the join fail. |
| WR-871 | WASAPI startup failure handshake | Worker COM initialization failure ignored inability to signal the readiness event, forcing an unexplained creator timeout. The signal is now checked with a stage-specific diagnostic. |
| WR-872 | WASAPI initialization unwind | Four event-creation/service/thread failure paths closed render, stop, and ready handles unchecked. A common cleanup helper attempts every close and records each failure. |
| WR-873 | WASAPI readiness wait | Timeout, native wait failure, and a worker-reported startup failure all collapsed into one message. Startup now preserves the exact failure class after safe worker teardown. |
| WR-874 | WASAPI readiness handle | Successful startup discarded `CloseHandle` failure and continued with a leaked readiness handle. Close failure now aborts initialization after proving worker exit and unwinding resources. |
| WR-875 | WASAPI start and shutdown | Audio-client start failure and normal shutdown used separate partially checked signal/stop/close sequences. Both now use the same checked lifecycle helpers and deterministic diagnostics. |
| WR-876 | MSVC animation masks | Three inverse-mask casts in the former per-channel retarget copy path emitted C4310 constant-truncation warnings in the Windows all-target build. The superseding pose-based retarget solver removes that obsolete copy path and its warning-producing casts entirely. |
| WR-877 | D3D11 absent-swapchain telemetry | Classified presentation returned before clearing `present_path` when the swap chain had already been retired, so a previous direct/offscreen classification could survive teardown. Every classified attempt now resets the live path before checking swapchain availability or status. |
| WR-878 | Windows embedded demo assets | MSVC emits the runtime's empty `__declspec(selectany)` asset fallbacks as `COMDAT ANY`, but the native linker treated them as hard duplicates of generated strong asset blobs. Windows COFF resolution now lets an ordinary definition replace a selectany fallback in either object order and strips the losing COMDAT group. |
| WR-879 | Win32 asynchronous termination | `Process.Kill()` could successfully begin asynchronous termination and an immediate `Destroy()` could issue a second `TerminateProcess` before the child became signaled. The second request then failed and teardown trapped without using its finite confirmation window. The shared helper now waits within that existing bound even when termination is already in flight and reports success only after exact exit confirmation. |
| WR-880 | Windows application runtime packaging | Package-generated release executables imported app-local MSVC runtime DLLs from a private temporary path, while the build-tree compiler kept redistributables only for installation. Packaging therefore failed closed even on a complete developer build. The native support-host directory now mirrors the installed runtime closure, and the application builder may source only recognized numbered compiler runtimes from that trusted directory while ordinary DLLs remain application-adjacent. |
| WR-881 | D3D11 biased scene state | Main mesh draws ignored failure to create their requested depth-biased rasterizer and silently rendered with an unbiased fallback. Pipeline binding now returns the exact failure and the draw is dropped. |
| WR-882 | D3D11 biased instanced state | Instanced and particle submissions inherited the same silent biased-state fallback through the shared binder. They now abort before `DrawIndexedInstanced` when biased state cannot be created. |
| WR-883 | D3D11 scene rasterizer completeness | A missing prebuilt rasterizer state could leave the prior command's state active and still submit a main draw. The binder now requires a concrete selected state and reports `E_UNEXPECTED` otherwise. |
| WR-884 | D3D11 instanced rasterizer completeness | Instanced submissions had the same stale-state exposure when an ordinary cached rasterizer was absent. The common binding result now rejects that submission too. |
| WR-885 | D3D11 main bind health | The main binder returned `void`, so device removal during its many void state-setting calls was not observed until after a draw. Binding now checks and latches device health before submission. |
| WR-886 | D3D11 main bind propagation | Non-instanced callers could not distinguish a completed bind from a failed one. They now unwind temporary SRVs, unbind resources, and return before drawing on failure. |
| WR-887 | D3D11 instanced bind propagation | Instanced callers likewise now unwind their draw resources and return before overriding particle state or submitting work after a failed bind. |
| WR-888 | D3D11 main dropped telemetry | Device loss after a main indexed draw suppressed `draw_calls` but did not count the attempted submission as dropped. The failure branch now advances `dropped_draws`. |
| WR-889 | D3D11 instanced dropped telemetry | Device loss after an indexed-instanced draw had the same undercount. The failure now contributes to dropped-draw telemetry. |
| WR-890 | D3D11 biased shadow state | Shadow draws ignored depth-biased rasterizer creation failure and rendered with the wrong bias. The pass is now marked failed and exits before touching later pipeline state. |
| WR-891 | D3D11 shadow rasterizer completeness | A missing ordinary shadow rasterizer could reuse stale state. Shadow submission now requires a concrete rasterizer and leaves the shadow slot incomplete on failure. |
| WR-892 | D3D11 shadow/skybox health | Shadow rasterizer binding was not checked before buffer work, and skybox device loss did not affect dropped telemetry. Both void-command boundaries now record device health; failed skyboxes count as dropped. |
| WR-893 | Win32 initial DIB cleanup | A partially created DIB whose pixel pointer was missing was deleted without checking `DeleteObject`. A common checked GDI cleanup helper now reports the native failure. |
| WR-894 | Win32 unselectable DIB cleanup | Failure to select a new DIB also discarded its delete result. That candidate cleanup is now checked and diagnosed. |
| WR-895 | Win32 replaced DIB cleanup | Successful DIB replacement silently ignored failure to release the old bitmap. The live replacement remains valid while the leak/failure becomes observable. |
| WR-896 | Win32 resize candidate cleanup | Incomplete or unselectable resize DIBs had independent unchecked delete paths. Both now use the checked GDI cleanup boundary. |
| WR-897 | Win32 resize rollback selection | Framebuffer-resize failure assumed the previous bitmap was reselected successfully. Rollback now validates `SelectObject` before deleting the rejected candidate. |
| WR-898 | Win32 resize rollback release | The rejected resize bitmap's deletion was unchecked after rollback. It now produces a stable platform diagnostic. |
| WR-899 | Win32 resize commit release | Commit of a new backing DIB ignored failure to release the prior unselected bitmap. The cleanup helper now reports it without corrupting the committed dimensions. |
| WR-900 | Win32 paint lifecycle | `EndPaint` failure was explicitly discarded, potentially hiding a broken paint-validation loop. The window procedure now reports that failure. |
| WR-901 | Win32 display DC | Display-scale probing discarded the `ReleaseDC` result. Display DC release now uses the checked DC helper. |
| WR-902 | Win32 window-init unwind | Four creation failures destroyed the native window without checking success. Every unwind now diagnoses a retained native window instead of silently claiming complete cleanup. |
| WR-903 | Win32 DC-init unwind | Memory-DC and DIB failures ignored `ReleaseDC`/`DeleteDC` results. Both DC classes now use checked cleanup helpers throughout initialization rollback. |
| WR-904 | Win32 bitmap teardown | Final destruction assumed the original memory-DC object was restored before deleting the DIB. The restore result is now validated. |
| WR-905 | Win32 window teardown | Final DIB, memory DC, window DC, and native-window destruction results were all ignored. Each cleanup stage now has a stable diagnostic. |
| WR-906 | Win32 window state lifetime | Native user data still pointed at soon-to-be-freed Zanna window state during final destruction. The pointer is explicitly detached and the detach result checked before `DestroyWindow`. |
| WR-907 | Win32 clipboard ownership | Read/write/clear paths ignored real `GlobalUnlock`, `CloseClipboard`, `EmptyClipboard`, and publication failures. They now distinguish successful final unlocks, discard untrustworthy reads, and report every lost clipboard transition. |
| WR-908 | Exec startup attribute sizing | The required-size `InitializeProcThreadAttributeList` probe ignored both its error class and a zero returned size. It now accepts only the documented insufficient-buffer result with nonzero storage. |
| WR-909 | Exec inherited-handle list | The generic allow-list builder did not validate aggregate byte size or reject null/invalid entries. It now checks multiplication and every handle before native attribute construction. |
| WR-910 | Exec handle ownership | Direct execution and capture scattered unchecked `CloseHandle` calls. One helper now clears ownership only after successful close so failures cannot masquerade as retirement. |
| WR-911 | Exec.Run thread handle | Synchronous direct execution retained the primary-thread handle until after process completion and ignored close failure. It now retires the thread handle immediately and includes that result in success. |
| WR-912 | Exec.Run exit proof | A signaled process whose queried status remained `STILL_ACTIVE` could be returned as the large unsigned value 259. Exact completion now rejects that inconsistent state. |
| WR-913 | Exec.Run process handle | Process-handle close failure did not affect the direct-execution result. A successful exit code is now returned only when both native handles retire successfully. |
| WR-914 | Exec.Capture writer ownership | Failure to close the parent's pipe writer could keep the reader from ever observing EOF. The child is now terminated with bounded confirmation and all remaining handles are retired. |
| WR-915 | Exec.Capture allocation pressure | Initial allocation failure stopped consuming the pipe, and growth failure eventually issued a zero-byte read then waited behind a full child pipe. Initial failure uses bounded teardown; growth failure switches to a drain buffer. |
| WR-916 | Exec.Capture bounded output | Reaching 16 MiB stopped reading and could deadlock before raising the truncation trap; non-broken-pipe read errors were also indistinguishable from EOF. Oversized output is fully drained, and real read failures now fail capture. |
| WR-917 | Exec.Capture completion proof | Read/process/thread closes, process wait, exit query, and `STILL_ACTIVE` consistency were incompletely checked. Capture publishes bytes only after every close and exact process completion succeeds. |
| WR-918 | Win32 embed cleanup diagnostics | Shared-preview mapping cleanup failures had no error surface. A bounded debugger/stderr diagnostic now records the operation and native error without changing the channel ABI. |
| WR-919 | Win32 embed host setup | Duplicate-name and host-map failure paths closed their section handle unchecked. Both now use the checked mapping owner helper. |
| WR-920 | Win32 embed client probe | Failure to map the client header probe discarded mapping-handle close failure. The handle remains truthfully owned until a successful close and failure is reported. |
| WR-921 | Win32 embed invalid metadata | Invalid header layout discarded both probe-unmap and mapping-close results. Both cleanup stages are now independently checked. |
| WR-922 | Win32 embed client publication | Valid-probe transition, full-map failure, header revalidation, and producer-claim rejection all had unchecked unmap/close paths. Each ownership transition now uses the common helpers. |
| WR-923 | Win32 embed finalization | Public channel close ignored final `UnmapViewOfFile` and `CloseHandle` failure. Both are now diagnosed, and the mapping-handle slot clears only after native success. |
| WR-924 | Installer helper create diagnostics | Cleanup-helper creation read `GetLastError` only after attempting directory rollback, so the original cause could be lost. The create error is now captured first and retained in diagnostics. |
| WR-925 | Installer helper-directory rollback | Failure to create the cleanup executable ignored inability to remove its private directory. The combined create/cleanup error now identifies retained staging. |
| WR-926 | Installer helper-file unwind | Later detached-helper failure deleted the staged executable unchecked. Non-missing failures are now written to the installer log. |
| WR-927 | Installer helper-directory unwind | The matching directory removal was also unchecked. Non-missing failures are logged independently so support can locate residue. |
| WR-928 | Installer atomic file rollback | Failed staged installer writes and commits discarded temporary-file deletion failure. Rollback now reports cleanup errors together with commit context. |
| WR-929 | Installer shortcut rollback | Shell Link save and atomic commit failures similarly ignored temporary `.lnk` cleanup. Both paths now report retained shortcut staging and preserve commit context. |
| WR-930 | Installer maintenance handoff | A successfully launched verified maintenance executable was reported cleanly even when its thread or process handle could not close. Handoff remains successful to avoid duplicate mutation, but logs the exact launcher-handle failure. |
| WR-931 | D3D11 target count | The ordinary-draw readiness gate accepted a render-target count larger than the backend's two-entry RTV mirror. It now rejects counts above two before native submission. |
| WR-932 | D3D11 primary target | The native target binder trusted a positive count even when slot zero was null. A shared validator now requires the primary view for every nonempty color binding. |
| WR-933 | D3D11 secondary target | Count-two bindings could expose a null second view, while count-one bindings could retain an uncounted second view in the CPU mirror. Both forms now fail validation. |
| WR-934 | D3D11 empty viewport | A zero-target binding could retain nonzero viewport dimensions and leave CPU/native state contradictory. Only the canonical zero-count, zero-view, zero-extent state is accepted. |
| WR-935 | D3D11 target-bind health | `OMSetRenderTargets` and `RSSetViewports` return no status, and the binder returned `void`. It now checks device health and returns the exact removal/reset reason. |
| WR-936 | D3D11 invalid-bind containment | An invalid target mirror previously returned without retiring the old output and viewport. It now explicitly unbinds both and marks an active frame failed. |
| WR-937 | D3D11 target selection | RTT, scene, overlay, and swapchain selection discarded native bind failure. Selection now propagates an `HRESULT` to every lifecycle caller. |
| WR-938 | D3D11 swapchain binding | Forced swapchain binding during resize and route changes also discarded native failure. It now returns device/validation status. |
| WR-939 | D3D11 BeginFrame target rollback | BeginFrame published an active/pending frame even when target binding was discarded by a removed device. It now clears tracked targets and restores the prior frame protocol state. |
| WR-940 | D3D11 BeginFrame sampler health | Initial common sampler binding was not checked before timing and cache maintenance began. Device loss now cancels the frame transaction at that boundary. |
| WR-941 | D3D11 presented snapshot restore | Backbuffer snapshot capture could mark a copy valid after restoration of the render target failed. Snapshot validity now requires a successful restore. |
| WR-942 | D3D11 depth-probe restore | Probe copies could publish a pending batch even when the original output target could not be restored. The failed batch and staging resource are now discarded. |
| WR-943 | D3D11 RTT readback restore | Render-target readback could clear `color_dirty` after its target restore failed. Readback success now includes restoration, retaining dirty state for retry. |
| WR-944 | D3D11 opaque-depth restore | The soft-particle depth snapshot became valid even when restoring the draw target failed. Validity now requires both restoration and healthy copy completion. |
| WR-945 | D3D11 same-size resize | Repairing an incomplete same-size swapchain could retain a complete resource set but claim a stale current binding after bind failure. The mirror is now cleared. |
| WR-946 | D3D11 resize rollback | Failed `ResizeBuffers` recovery recreated old targets but ignored failure to bind them. Rollback now clears the current-target mirror when rebinding fails. |
| WR-947 | D3D11 resize commit | Successful swapchain resize likewise ignored final bind failure. The committed resources remain owned, but no false current binding survives. |
| WR-948 | D3D11 render-scale publication | `SetRenderScale` returned success after its final swapchain rebind failed. It now returns false and leaves device loss visible to the caller. |
| WR-949 | D3D11 shadow entry | Starting a new shadow pass ignored failure to restore targets from a prior pass. Shadow setup now aborts before mutating the next slot. |
| WR-950 | D3D11 shadow completion | Ending a shadow pass could advertise its slot complete even when the main target could not be rebound. The slot is now invalidated, shadow count recomputed, and the frame failed. |
| WR-951 | Parallel.ForEach event unwind | Critical-section, task-size, and task-allocation failures closed the batch event unchecked. Every pre-submission unwind now uses fatal checked retirement. |
| WR-952 | Parallel.ForEach event completion | The successful drain path also discarded `CloseHandle` failure before freeing synchronization state. It now enforces the same ownership invariant. |
| WR-953 | Parallel.Map event unwind | Three Map setup failures could hide an invalid/leaked completion event. They now use the shared checked closer. |
| WR-954 | Parallel.Map event completion | Map's drained event is now proven closed before result collection and synchronization storage are released. |
| WR-955 | Parallel.Invoke event unwind | Invoke setup discarded completion-event close results on all allocation/synchronization failures. Those paths now fail fast on ownership corruption. |
| WR-956 | Parallel.Invoke event completion | Invoke completion now proves the event retired before freeing worker-borrowed state. |
| WR-957 | Parallel.Reduce event unwind | Reduce's critical-section and task-allocation unwind paths silently ignored event close failure. All now use the checked adapter. |
| WR-958 | Parallel.Reduce event completion | Reduce no longer publishes an accumulator after an unobservable event-retirement failure. |
| WR-959 | Parallel.For event unwind | Integer-range parallel setup had the same three unchecked completion-event cleanup paths. They now share the fatal ownership boundary. |
| WR-960 | Parallel.For event completion | The successful range drain now checks event retirement before releasing tasks and counters. |
| WR-961 | Installer font-probe early release | Invalid font-family input released its display DC unchecked. The common DC adapter now diagnoses failure on that early exit. |
| WR-962 | Installer font-probe normal release | Successful font enumeration also discarded `ReleaseDC` status. Normal completion now uses the checked adapter. |
| WR-963 | Installer font ownership | Theme destruction and replacement cleared owned font slots regardless of `DeleteObject` failure. Every font retirement now records native failure. |
| WR-964 | Installer brush ownership | Long-lived theme brush retirement had the same silent GDI leak path. It now uses checked deletion. |
| WR-965 | Installer circuit pens | Per-line decorative pens were deleted unchecked after selection restoration. Each retirement now carries a stable diagnostic label. |
| WR-966 | Installer circuit ring | The separately allocated ring pen had an independent silent cleanup path. It now uses checked GDI deletion. |
| WR-967 | Installer brand-panel brush | A temporary backdrop brush could leak without any support signal. Its cleanup is now diagnosed. |
| WR-968 | Installer focus resources | Owner-drawn focus pen and brush cleanup results were discarded. Both are now checked independently. |
| WR-969 | Installer action-button resources | Border pen, fill brush, and accent brush retirement could each fail silently. All three use labeled checked deletion. |
| WR-970 | Installer page completion | Programmatic branded-page completion destroyed the native window unchecked. Failure now reaches the shared bounded debugger diagnostic. |
| WR-971 | Installer page action | Action-button closure discarded `DestroyWindow` failure and could leave a modal page alive. The failure is now observable. |
| WR-972 | Installer page close | The branded page's `WM_CLOSE` path had the same unreported failure. It now uses checked destruction. |
| WR-973 | Installer progress brushes | Indeterminate track and pulse brushes were deleted unchecked every animation frame. Both transient cleanup paths now report failure. |
| WR-974 | Installer progress completion | Cancellation and worker-completion messages could fail to destroy the progress window silently. Both transitions now use checked destruction. |
| WR-975 | Installer progress unwind | Exception and normal stack unwinds discarded progress-window destruction failure. Surviving native state is now diagnosed. |
| WR-976 | Installer clipboard unlock | Clipboard text publication ignored `GlobalUnlock`, so a still-locked allocation could proceed into clipboard ownership. Exact final unlock is now required. |
| WR-977 | Installer clipboard rollback | Allocation, open, clear, and publication failures discarded `GlobalFree` status. Every still-owned block is now freed through a diagnostic adapter. |
| WR-978 | Installer clipboard close | RAII cleanup ignored `CloseClipboard`, allowing apparent success with clipboard ownership retained. Success now requires an exact close; unwind retries and diagnoses. |
| WR-979 | Installer options closure | Accepted, cancelled, and `WM_CLOSE` options-window paths used inconsistent checked/unchecked destruction. All now share the checked window adapter. |
| WR-980 | Installer options unwind | Stack unwinding could silently leave the custom-options window alive. The surviving window is now reported through the same bounded diagnostic path. |

## Regression coverage

- `test_rt_windows_runtime` exercises finite wait slicing, concurrent process-lifetime WinSock
  initialization, the CRT-exit-table exclusion, deterministic WinSock error/output contracts,
  entropy argument handling, strict Windows path transcoding, checked directory conversion,
  Unicode/non-inheritable stdio, deny-write snapshot sharing, durable flush/replace behavior,
  failure-atomic save and custom-TLS-root source contracts, signed-minimum atomic subtraction,
  exact thread-handle joining, bounded asynchronous process-termination confirmation, ordinal
  comparison, fail-closed deletion guards, processor-count
  validity, drive-root temp preservation, long environment-backed home paths, CRT-aware network
  workers, restricted child handle inheritance, checked capture/wait failures, explicit Unicode
  event creation, checked critical-section construction and parallel completion signaling,
  signed pre-epoch FILETIME conversion, terminal-wrapper narrowing, finite window pacing, and the
  bounded/checked WASAPI thread, event, format, routing, buffer, and control-thread contracts.
  `native_run_windows_environment` additionally compiles and runs an ephemeral `TcpServer` through
  the CRT-less native PE startup path.
- `test_rt_file_ext` creates a 3 GiB sparse file and verifies 64-bit seek, stat, visibility, and
  modification-time behavior without allocating the file's logical size.
- `test_rt_args` races small and 16 KiB environment values across the Win32 size/read boundary.
- `test_rt_locale` exercises sentinel, malformed-tag, normalization, and cleared-output behavior.
- `test_rt_tls_cert` exercises exact CertificateVerify framing, null-session rejection, non-ASCII
  custom-CA paths, malformed trailing PEM blocks, and the 16 MiB Windows bundle limit.
- `test_rt_exec` relaunches itself through both Process and ConPTY with intentionally unsorted,
  non-ASCII explicit environments; it also rejects duplicate variables and malformed UTF-8.
- `test_rt_asset` mounts a non-ASCII pack path and verifies Windows ordinal case identity.
- `test_rt_uia_provider` covers full-width runtime IDs, strict text conversion, invalid geometry,
  stale bridge generations, deterministic failure outputs, and normalized range values.
- `test_rt_watcher`, `test_rt_future`, `test_rt_concqueue`, `test_rt_threads_monitor`,
  `test_rt_threads_thread`, and `test_rt_threads_primitives` cover the affected runtime contracts.
- `test_embed_channel` exercises name collision, exclusive attachment, producer restart/exit,
  strict host/producer roles, stable frame publication and dimensions, cleared failure outputs,
  full-ring rejection and FIFO order, and resize clamping. `test_input` is also enabled on Windows
  again, and the graphics test aggregate now depends on both tests on every platform.
- `test_vgfx3d_backend_d3d11_shared` covers timestamp/depth-probe poll budgets and source contracts
  requiring stage-before-publish ordering for all repaired D3D11 replacement paths, required
  startup-object validation, exact presentation status, bounded texture caches, render-scale
  mutation guards across the pending-present interval, multi-pass continuation recovery,
  draw/shadow ownership, transactional RTT
  switching, staging recovery, resize ordering, post-FX target planning, overlay-preserving scene
  replacement, BGRA/feature-level device creation, device-health checks after void GPU commands,
  hardware-to-WARP retry cleanup, complete fallback-resource publication, timing-query health
  checks, malformed RTT staging eviction, native dynamic/morph-buffer descriptor validation,
  exact texture/buffer/query/sampler descriptors, RTV/DSV/SRV backing-resource identity and
  subresource ranges, controlling-`IUnknown` device identity for core interfaces and every repaired
  device child, exact depth/blend/rasterizer state behavior, biased-state cache revalidation, exact
  morph SRV/resource pairing, partial-output cleanup, presented-snapshot validation, failed-frame
  clear/draw/shadow/RTT/present suppression, retired failed-IBL telemetry, and initialized
  diagnostic formatting;
  `zia_smoke_d3d11_rtt_readback`,
  `g3d_test_canvas3d_viewmodel_sprite`, `g3d_test_canvas3d_point_shadows_d3d11`, and the Ridgebound
  D3D11 smoke exercise the hardware backend.
- `test_linker_platform_import_planners` verifies that the 64-bit stat and floating-remainder
  exports plus `_fdclass`/`_fdtest` map to UCRT, the reliability APIs map to Kernel32, and
  Windows-only names stay excluded from Linux and macOS.
- `test_linker_symbol_resolver` verifies that an ordinary Windows COFF asset definition overrides
  a `COMDAT ANY` selectany fallback in either object order without weakening other duplicate-symbol
  or COMDAT-selection rules.
- `test_packaging_WindowsPackageBuilder_all` verifies that package-generated applications can
  bundle a numbered MSVC runtime from the validated native-host support directory while missing
  ordinary or compiler DLL dependencies still fail closed. The opt-in
  `windows_installer_user_smoke` and `windows_installer_crackman_smoke` tests exercise the complete
  package/install/launch/uninstall lifecycle with those app-local runtime bytes.
- `test_rt_scene_editor` preserves the full boxed signed-64-bit range in tiled properties;
  `test_basic_lexer` covers CRLF and lone-CR EOL normalization; and `test_rt_model3d` supplies its
  own strict-decoded JPEG fixture.
- `windows_automation_script_contracts` exercises failure-atomic signing, timestamp-URL rejection,
  input-race detection, paired metadata publication, staging cleanup, single/multi-config Studio
  discovery, PE/provenance checks, path confinement, bounded installer validation, duplicate
  demo-output rejection, transactional demo PE publication, quoted asset parsing, reparse
  rejection, architecture proof, whole-generation demo publication and rollback, private smoke
  directories, bounded output, monotonic deadlines, process-tree termination, and checked reap
  waits. Behavioral fixtures exercise the shared write-denying PE snapshot against valid,
  wrong-architecture, and zero-entry-point images. The suite also launches the logic-free `.cmd`
  compatibility shim and pins installer-wrapper help, equals-form input detection, required
  Studio output checks, and the distinct package/product version domains used during lifecycle
  validation.
- `test_windows_installer_cleanup_policy` covers supported drive/UNC namespaces, strict process
  identifiers, well-formed UTF-16, extended-path separator rules, namespace-alias and Unicode
  ordinal identity, root/traversal refusal, reserved devices, alternate streams, and illegal or
  trailing characters. `test_vg_filedialog_platform_win32` covers root-preserving parent navigation, strict
  UTF-8 path handling, path joining, complete extended roots, device-namespace rejection,
  absolute-only home fallback, and bounded enumeration.
- `test_packaging_WindowsInstallerMetadata_all` covers strict UTF-8 and bounded collections,
  Windows path aliases/devices, control-entry collisions, payload ownership, URL/key structure,
  shortcut and association grammars, integration consistency, and checked size accounting.
  `test_packaging_PE_all` and `test_packaging_Verify_PEall` mutate loader alignments, section
  extents/overlaps, entry-point ownership, data directories, Authenticode storage, and image kind
  to prove that every native packaging consumer shares the same bounded PE32+ policy.
  `ToolchainWindowsPackageBuilder` coverage rejects partial, stale, wrong-architecture,
  non-executable, and hash-mismatched Studio pairs and proves buildinfo rebinding after nested
  signing. `test_packaging_WindowsPackageBuilder_all` and `test_windows_installer_version` pin the
  shared ASCII, length, four-component, 16-bit, and complete prerelease/build version contract.
- `test_windows_installer_update` covers partial pinned configurations, key/digest/signature bounds,
  canonical LF-terminated signed bytes, strict signed and presentation result fields, invalid enum
  states, ambiguous URLs, and ordinal origin matching at the final launch boundary.
  `test_windows_installer_lifecycle_contract` protects exact recovery schemas, durable staging,
  validated PATH mutation, required Shell Link outputs, typed registry/elevation handling, bounded
  destination probes, upgrade filtering, fail-closed destination/shortcut cleanup, paired internal
  worker arguments, cache/elevation proof before handoff waiting, verified installer class reuse,
  guarded painting, per-monitor DPI transitions, quit preservation, bounded/coalesced progress,
  exact non-reparse cleanup-helper snapshots, checked helper termination/reaping, truthful reboot
  scheduling, read-only attribute restoration after failed cleanup, canonical unsigned Windows OS
  versions, and early package-version validation in the host.
  `test_windows_installer_brand_validation` directly exercises page/progress UTF-16 and size
  bounds, action identity, accessible labels, default/close results, verification gates, and work
  presence. Host source contracts additionally pin mutation-denying package snapshots, exact
  native reads, deterministic log short-write errors, and fail-closed log initialization.
- `windows_installer_host_cli_contracts` exercises duplicate, empty, ambiguous-help, and malformed
  internal-handoff options without entering an installer mutation path.
- `windows_utf8_tool_command_line` exercises Unicode source input through `zanna`, `zia`, and
  `vbasic`; Unicode IL output and project creation through the driver; and Unicode generated output
  through `rtgen`. `test_support`, `test_run_process_quotes`,
  `test_tools_frontend_native_compiler`, and `test_tools_asset_compiler` cover the corresponding
  SourceManager, child-process, native-build, and asset filesystem boundaries. The process tests
  also prove that the restricted inherited-handle array remains live through `CreateProcessW`.
- `benchmark_compare_self_test` verifies the benchmark regression classifier through the same
  probed-interpreter path used by Windows Git Bash, including fallback around nonfunctional
  application-execution aliases.
- The Studio phase, welcome, bottom-panel, diagnostic-action, BASIC workspace-query, file-tree, and
  project-index regressions cover bounded verifier work, high-DPI logical layouts, canonical
  Windows query paths, monitor-feasible zoom, deterministic concurrent snapshot startup, and
  monotonic subprocess/debug-adapter deadlines under cold Windows process startup. The
  bottom-panel fixture reaches its wide branch without assuming a monitor-specific maximum window
  size.

The required end-to-end gates are `scripts/build_zanna_win.ps1` and
`powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_demos_win.ps1 --clean --run`.
The `.cmd` demo shim delegates to that canonical PowerShell implementation under
[ADR 0113](adr/0113-windows-automation-powershell-entry-points.md). The platform-policy lint
remains mandatory for future changes in these adapters.

## Validation record

Final Windows x64/MSVC alpha-hardening validation on 2026-08-11:

- A no-skip clean `scripts/build_zanna_win.ps1` run rebuilt the complete warning-as-error Debug
  tree, including Zanna Studio, and completed installation. One parallel graphics3d test failed
  transiently; its immediate isolated replay passed in 7.24 seconds. An exact-source incremental
  replay of the complete canonical script then passed all 1,861 non-slow CTests under the same
  eight-way concurrency and continued through changed-tree platform-policy lint, runtime-surface
  audit, every cross-platform host smoke, and installation with exit zero.
- The focused D3D11, Windows runtime, installer-lifecycle, and shared-preview IPC regressions
  passed 4/4. The complete `graphics3d` label passed 151/151, including hardware-backed D3D11,
  GPU smoke, and soak coverage. The installer selection passed 23 tests with its expected
  non-elevated all-users skip; the installed-configuration smoke passed separately after the
  canonical build restored the Studio target.
- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_demos_win.ps1 --clean
  --run` rebuilt and privately launch-smoked all seven curated native x64 demos in 59.1 seconds:
  Crackman, Chess, Action Slice, Game3D Starter, Game3D Scenes, Overhaul Showcase, and Paint.
- `clang-format --dry-run --Werror` passed for every changed native source. The source-header
  audit reported zero missing headers, `scripts/check_docs.sh`, strict changed-only platform
  lint, and `git diff --check` passed. The documentation auditor's 48 existing undocumented
  prototypes remain informational debt and were not increased by this pass.

Final Windows x64/MSVC alpha-hardening validation on 2026-08-05:

- A no-skip clean `scripts/build_zanna_win.ps1` run rebuilt the complete warning-as-error Debug
  tree, including the native Zanna Studio target. The default non-slow selection passed
  1,853/1,853 CTests, and the run continued through strict platform-policy lint, runtime-surface
  audit, every cross-platform host smoke, and installation. An independent replay of the
  post-build gates passed all eight focused runtime-surface tests and accounted for 7,852 runtime
  functions, 531 classes, and 9,198 header declarations; the install manifest contains 1,219
  files.
- Full integration exposed three defects that focused review did not: generated demo assets
  conflicting with MSVC `selectany` fallbacks (WR-878), an asynchronous process-termination race
  in the Studio phase-2/phase-3 test (WR-879), and missing app-local MSVC runtime closure in
  generated application installers (WR-880). Each now has a focused regression, and the clean
  canonical run passed from the corrected source.
- The opt-in slow Windows installer lifecycle selection passed the per-user synthetic-package and
  Crackman application tests in 6.16 and 8.40 seconds. The all-users test reported its expected
  explicit skip because this test session is not elevated.
- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_demos_win.ps1 --clean
  --run` rebuilt, PE-validated, privately launch-smoked, and atomically published all seven
  curated native x64 demos in 57.2 seconds: Crackman, Chess, Action Slice, Game3D Starter,
  Game3D Scenes, Overhaul Showcase, and Paint.
- `clang-format --dry-run --Werror` passed for all 22 textually changed native files. The full
  source-header audit passed for all 19 changed C/C++ sources, all three changed PowerShell files
  parsed cleanly, `scripts/check_docs.sh` passed, strict changed-only platform lint passed, and
  `git diff --check` exited zero. Rebuilding the three header-comment-touched translation units
  and their dependents completed warning-free, and all six affected linker, installer-host,
  lifecycle, and packaging CTests passed.

Final alpha-hardening revalidation on Windows x64/MSVC on 2026-08-01:

- A frozen-source, no-skip clean `scripts/build_zanna_win.ps1` run rebuilt the complete
  warning-as-error Debug tree and exited zero in 2,395.1 seconds with empty stderr. It passed
  1,876/1,876 CTests in 521.01 seconds, strict platform-policy lint, the runtime-surface audit,
  every cross-platform host smoke, and installation. The audit accounted for 7,723 runtime
  functions, 530 classes, and 9,068 header declarations, and its eight focused tests passed.
- Before the clean proof, a frozen-source incremental canonical run exited zero in 665.0 seconds,
  passed the same 1,876/1,876 CTests in 498.90 seconds, and repeated every downstream gate with
  empty stderr. The preceding clean integration iterations exposed the missing standalone-native
  imports in WR-801/WR-802 and warning-as-error test drift in WR-803/WR-804; the complete
  incremental rehearsal then exposed the Windows application-alias defect in WR-805. Each was
  repaired and covered before the final clean run.
- `powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File
  scripts/build_demos_win.ps1 --clean --run` rebuilt, PE-validated, privately launch-smoked, and
  atomically published all ten curated native x64 demos in 328.0 seconds with empty stderr:
  Ashfall, Ashfall Scenes, 3D Bowling, Ridgebound, Xenoscape, Crackman, Chess, Baseball, Paint,
  and ZannaSQL.
- `clang-format --dry-run --Werror` passed for all 37 changed/new native source files. The
  source-header audit found zero missing file headers; `scripts/check_docs.sh`, strict
  changed-only platform lint, shell syntax validation, `git diff --check`, and explicit untracked
  whitespace/final-newline checks passed. The documentation auditor's 66 existing undocumented
  runtime prototypes remain informational debt.

Alpha-hardening revalidation on Windows x64/MSVC on 2026-08-14:

- A final frozen-source, no-skip clean `scripts/build_zanna_win.ps1` run rebuilt the
  warning-as-error Debug tree with no compiler diagnostics and exited zero in 2,512.3 seconds. It
  selected all 1,866 configured non-slow CTests with zero failures (12 entries are disabled by
  policy), then completed platform-policy lint, the runtime-surface audit, every cross-platform
  host smoke, and installation.
- An earlier clean integration pass exposed one pre-existing MSVC `C4310` warning in the Ogg
  reserved-page-bit mask. The behavior-preserving exact `0xF8` mask removed the truncating
  complement cast; `test_ogg_vorbis` and the final clean canonical run passed without a warning.
- The complete `graphics3d` label passed 155/155 tests in 962.86 seconds, including the shared
  D3D11 contracts, four D3D11 integration tests, three GPU smoke tests, and soak coverage. The
  final affected Windows/D3D/installer/parallel selection passed 18 applicable tests; its
  administrator-only all-users installer smoke was correctly skipped in the non-elevated session.
- `powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File
  scripts/build_demos_win.ps1 --clean --run` rebuilt, staged, and privately launch-smoked all eight
  curated native x64 demos in 65.9 seconds: Crackman, Chess, Dumbsnake, Action Slice, Game3D
  Starter, Game3D Scenes, Overhaul Showcase, and Paint. Every executable remained healthy through
  its bounded launch window.
- `clang-format --dry-run --Werror`, the source-header audit, `scripts/check_docs.sh`, strict
  changed-tree platform lint, and `git diff --check` passed for the final change set. The
  documentation auditor reported zero missing file headers; its 49 existing undocumented runtime
  prototypes remain informational debt and were not increased by this pass.

Final alpha-hardening revalidation on Windows x64/MSVC on 2026-07-26:

- A frozen-source, no-skip clean `scripts/build_zanna_win.ps1` run rebuilt the warning-as-error
  Debug tree and exited zero in 2,269.6 seconds with an empty stderr log. It passed 1,862/1,862
  CTests in 500.01 seconds, strict platform-policy lint, the runtime-surface audit, every
  cross-platform host smoke, and installation. The audit accounted for 7,613 runtime functions,
  524 classes, and 8,969 header declarations; its eight focused tests passed.
- The first clean integration attempt exposed one issue hidden by focused MSVC targets: the new
  COM-identity checks referenced external `dxguid` data that the zero-dependency native Studio
  link does not import. WR-715 replaced those references with SDK-identical file-local GUID
  constants. The standalone native Studio target then linked successfully, and the complete
  clean gate above passed from the corrected source.
- Focused warning-as-error builds and runtime, network, thread, installer, and D3D11 regressions
  passed. Hardware-backed coverage included Ridgebound, render-target readback, viewmodel
  sprites, point shadows, render scaling, and the shared D3D11 contract test.
- `powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File
  scripts/build_demos_win.ps1 --clean --run` rebuilt, structurally PE-validated, privately
  launch-smoked, and atomically published all nine curated native x64 demos in 302.7 seconds:
  Ashfall, 3D Bowling, Ridgebound, Xenoscape, Crackman, Chess, Baseball, Paint, and ZannaSQL. The
  demo gate produced no stderr output.
- `scripts/build_installer.ps1 --build-dir build --config Release --target windows` completed the
  real Release package path in 3,063.3 seconds. Its manifest marked the 307,005,574-byte unsigned
  x64 `zanna-0.2.99-win-x64.exe` payload verified with SHA-256
  `fcbc41db89314dc843ec756ea6716b03ffcd4182372fcc5a0775990fdbdc7795`. The standalone
  real-artifact validator then passed complete install, integration registration, component
  modify, byte-exact repair, installed CLI/native-code generation, installed SDK CMake-consumer
  build, uninstall, and detached-cache cleanup in 102.1 seconds with no stderr output.
- The user-application and Xenoscape installer CTests passed independently in 6.01 and 30.56
  seconds, respectively, covering package creation, per-user installation, launch/runtime
  metadata, and uninstall cleanup. The separate all-users smoke remains correctly inapplicable
  to this non-administrator test session.
- `clang-format --dry-run --Werror`, the source-header audit, `scripts/check_docs.sh`, strict
  changed-tree platform lint, and `git diff --check` passed for the final change set.

Revalidated on Windows x64/MSVC on 2026-07-26:

- A no-skip clean `scripts/build_zanna_win.ps1` run rebuilt the warning-as-error Debug tree from
  scratch and exited zero in 2,377.9 seconds with an empty stderr log. It passed 1,861/1,861
  CTests in 513.11 seconds, strict platform-policy lint, the runtime-surface audit, every
  cross-platform host smoke, and installation. The audit accounted for 7,598 runtime functions,
  524 classes, and 8,954 header declarations; its eight focused tests passed.
- The clean pipeline exposed and drove closure of three integration-only defects before that
  final pass: missing fixed native-import mappings, an outdated audio shutdown test double, and
  legal D3D11 sampler-state canonicalization on real hardware. Focused import-planner, audio
  lifetime, shared D3D11, readback, viewmodel-sprite, PE mutation, Win32 runtime, native-dialog,
  installer-lifecycle, and automation regressions all passed after repair.
- `powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File
  scripts/build_demos_win.ps1 --clean --run` rebuilt, structurally PE-validated, privately
  launch-smoked, and atomically published all nine curated native x64 demos in 305.0 seconds:
  Ashfall, 3D Bowling, Ridgebound, Xenoscape, Crackman, Chess, Baseball, Paint, and ZannaSQL. The
  demo gate produced no stderr output.
- `scripts/build_installer.ps1 --build-dir build --config Release --target windows` completed the
  real Release package path in 2,614.1 seconds. Its manifest marked the 306,436,348-byte unsigned
  x64 `zanna-0.2.99-win-x64.exe` payload verified with SHA-256
  `8fd2206002fe627a79df632a966472531e676571ff6991971106d4886f476f0d`. The non-elevated toolchain
  and Xenoscape install/uninstall lifecycle CTests then passed in 6.04 and 30.27 seconds. The
  standalone real-artifact validator also completed its complete install, CMake-consumer build,
  modify, repair, uninstall, and detached-cleanup checks in 100.1 seconds.
- `clang-format --dry-run --Werror` passed for all 40 changed native source files. Windows
  PowerShell parser checks passed for all six changed/new scripts, the source-header audit found
  zero missing file headers, `scripts/check_docs.sh`, strict full-tree platform lint, and
  `git diff --check` passed. The documentation auditor's 350 existing undocumented runtime
  prototypes remain informational debt and were not increased by this pass.

Revalidated on Windows x64/MSVC on 2026-07-25:

- A clean canonical `scripts/build_zanna_win.ps1` run passed 1,858/1,858 CTests in
  486.58 seconds, then completed strict platform-policy lint, the runtime-surface audit,
  every cross-platform host smoke, and installation. The audit accounted for 7,586 runtime
  functions, 524 classes, and 8,940 header declarations, and its eight focused tests passed.
- After the last native and automation changes, a frozen-source incremental canonical run passed
  the same 1,858/1,858 CTests in 502.56 seconds and repeated the lint, runtime audit, host-smoke,
  and install gates. Both canonical runs produced empty stderr logs. Targeted Windows runtime,
  Direct3D shared-backend, audio, installer-lifecycle, and automation regression CTests also
  passed during development.
- `powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File
  scripts/build_demos_win.ps1 --clean --run` rebuilt, PE-validated, privately launch-smoked, and
  atomically published all nine curated native x64 demos in 307.1 seconds: Ashfall, 3D Bowling,
  Ridgebound, Xenoscape, Crackman, Chess, Baseball, Paint, and ZannaSQL. The demo gate produced
  no stderr output.
- `clang-format --dry-run --Werror` passed for all 26 changed native source files. PowerShell
  parser checks passed for all three changed scripts, the source-header audit found zero missing
  file headers, `scripts/check_docs.sh`, strict changed-only platform lint, and
  `git diff --check` passed. The documentation auditor's 540 existing undocumented runtime
  prototypes remain informational debt and were not increased by this pass.

Revalidated on Windows x64/MSVC on 2026-07-24:

- The warning-as-error incremental Debug `scripts/build_zanna_win.ps1` pipeline passed
  1,891/1,891 CTests and completed strict platform lint, the runtime-surface audit, every
  cross-platform host smoke, and the install stage in 576.2 seconds. The first complete run
  exposed the tracked Xenoscape scene-generator policy drift recorded as WR-501; its focused
  strict-policy rerun and the complete canonical rerun both passed after repair.
- The Direct3D shared-backend, native installer validation/lifecycle/update/cleanup, and Windows
  automation regression set passed 7/7 focused CTests. The automation contract passed again after
  pinning generated CMake architecture proof, per-demo publication isolation, and the staged
  installer package driver.
- `powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File
  scripts/build_demos_win.ps1 --clean --run` built, PE-validated, privately launch-smoked, and
  published all nine curated native x64 demos in 308.1 seconds: Ashfall, 3D Bowling, Ridgebound,
  Xenoscape, Crackman, Chess, Baseball, Paint, and ZannaSQL. Each executable and its assets now
  live below a demo-owned `examples/bin/<demo>/` directory.
- `scripts/build_installer.ps1 --build-dir build --config Release --target windows` completed in
  1,864.3 seconds after its first invocation exposed and repaired the Windows self-relink lock in
  WR-502. It produced the verified, unsigned development installer
  `zanna-0.2.99-win-x64.exe`: 298,583,630 bytes with SHA-256
  `1570f9cf5a848a3113da0cb25d1c7871f36ac5598df88b2d408074f6191369ed`.
  Independent checksum-required verification, the bounded installer-host self-test, and schema-3
  inspection all returned zero. Inspection reports 1,984 payload files, 642,324,370 installed
  bytes, and the `core`, `zannastudio`, `sdk`, and `samples` components.
- The opt-in slow application-installer lifecycle pair passed 2/2 in 37.7 seconds. It generated
  both the synthetic user package and the real Xenoscape package, completed quiet user-scope
  installs, launched the installed Xenoscape executable outside its source working directory,
  validated assets, shortcuts, registry identity, and maintenance-cache ownership, then completed
  residue-free uninstalls.
- `scripts/audit_doc_comments.sh` found zero missing source-file headers; strict changed-only
  platform lint, PowerShell parser checks, `clang-format --dry-run --Werror`,
  `scripts/check_docs.sh`, and `git diff --check` passed. The documentation auditor's 1,024
  existing undocumented runtime prototypes remain informational debt and were not increased by
  this pass.

Revalidated on Windows x64/MSVC on 2026-07-23:

- The current canonical warning-as-error Debug `scripts/build_zanna_win.ps1` pipeline passed
  1,832/1,832 CTests in 466.98 seconds and completed strict platform lint, the focused
  runtime-surface audit, every cross-platform host smoke, and the install stage. An earlier
  canonical Release pipeline rebuilt the product and standalone native Studio, passed
  1,819/1,819 CTests in 243.98 seconds, and completed the same downstream gates. The
  contention-sensitive Studio file-tree probe passed in 48.95 seconds in Debug and 28.09 seconds
  in Release; the bounded phase-2/3 probe passed in 9.90 and 4.15 seconds respectively.
- The native Windows environment probe opens and closes an ephemeral listener through a generated
  CRT-less PE, and both it and `test_rt_windows_runtime` passed after the ZannaSQL failure was
  isolated. The final Windows automation contract passed again after lifecycle validation exposed
  and repaired the multiline `zanna --version` parser.
- The focused hardware/backend D3D11 set passed 5/5 tests in 108.83 seconds: Ridgebound,
  render-to-texture readback, viewmodel sprites, point shadows, and the shared backend contracts.
  Those contracts cover the bounded caches, frame/present protocol, resize ordering, post-FX
  route validation, and active-frame mutation guards added by this audit.
- `scripts/build_installer.ps1 --build-dir build --config Release --target windows` produced the
  286,072,746-byte development installer `zanna-0.2.99-win-x64.exe` with SHA-256
  `1ca60c5ec9715a2ed00be3633c3218649db2c429c0b8472b98c276f04156a853`.
  Checksum-required verification, the waited installer-host self-test, and schema-3 inspection all
  returned zero. Inspection reports 1,870 payload files, 623,540,847 installed bytes, and the
  `core`, `zannastudio`, `sdk`, and `samples` components.
- `scripts/validate-windows-toolchain-installer.ps1` passed a real user-scope Complete install,
  exact Studio buildinfo/version/hash validation, Minimal-to-Complete component round trip,
  byte-exact repair, PATH/association/shortcut checks, interpreted and native execution, external
  installed-SDK CMake consumer build/run, and uninstall. Independent residue checks found no
  product registry key, install root, PATH entry, Start Menu directory, validator workspace, or
  maintenance-cache file.
- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_demos_win.ps1 --clean --run`
  built and launch-smoked all nine curated native x64 demos successfully: Ashfall, 3dbowling,
  Ridgebound, Xenoscape, Crackman, Chess, Baseball, Paint, and ZannaSQL.
- `scripts/lint_platform_policy.sh --strict --changed-only`, the PowerShell parser checks, the
  changed-source header audit, `clang-format --dry-run --Werror`, and `git diff --check` passed.
