# Engine bug audit — October 23–24, 2026

## Result

The seven original defects and all five follow-up findings now have source fixes.
The hierarchy draws owned cached rows, and an additional fix reveals nested UI/grid
objects. Changes cover editor actions, scripting, asset loading, asynchronous
requests/processes, GPU teardown, and networking. Native script ABI is now **60**;
existing native scripts must be rebuilt.

This is a risk-led review, not proof that the engine is free of bugs. Substantial
pre-existing local changes and the ModuGuizmo work were preserved. See the validation
record for build coverage and remaining manual/platform checks.

## Evidence and coverage

- Indexed 275 first-party C++ source/header files under `src` and `include`, excluding
  third-party trees. Searched scene mutation, pointer ownership, detached workers,
  callbacks, buffer copies, allocation counts, shutdown, and asynchronous work.
- Inspected the relevant code in Engine, scene hierarchy, native/managed scripting,
  collision dispatch, networking/serialization/loopback, MMesh/EXR asset loading,
  rendering/gather/cleanup, PhysX/Jolt object identification, audio/video ownership,
  and file-browser asynchronous refresh.
- Located 903 logs, 29 of which are dedicated text crash reports, and 10 minidumps
  under `build/release/CrashReports`. Counts are a point-in-time inventory; paired
  session/stderr logs do not represent additional independent crashes.
- Read all 29 dedicated text reports. The 10 minidumps were inventoried, not replayed
  in a debugger. Matching historical executable/PDB pairs were not verified.
- Root `CrashReports/Modularity-preview.log` explicitly contains mock UI data and
  was excluded from the crash evidence.

| Text-report signature | Reports | What it establishes |
| --- | ---: | --- |
| `0xc0000374` — heap corruption | 17 | Allocator metadata was damaged; the detecting stack generally does not identify the original writer. |
| `0xc0000005` — access violation | 10 | An invalid read/write occurred; null, invalid address arithmetic, stale storage, and other causes remain possible. |
| Synthetic code `0x0` through invalid-parameter handling | 2 | CRT/startup failure reports, not proof of a scene-object use-after-free. |

The September 8 report identifies `Engine::renderObjectNode` and recursive hierarchy
drawing while reading unmapped memory. That is relevant to the hierarchy lifetime
defects below, but it is **not** proof that these exact surviving paths caused that
historical crash. Several other reports contain only heap/ntdll frames, incomplete
unwinding, or questionable symbol attribution. Their root causes remain unproven.
The crash reporter's `DANGLING POINTER` interpretation of `MEM_FREE` is a heuristic,
not an allocation history.

## Fixed defects

P1 means a plausible crash, invalid memory access, or destruction-order error.
P2 means a narrower failure or potentially large resource misuse.

### F1 — P1: hierarchy root indices outlive scene edits

**Trigger:** Delete a root row or change root ordering from a row's context menu
while the hierarchy is being drawn.

**Before:** `renderHierarchyPanel` captured vector indices. Drawing one root can
shrink/reorder `sceneObjects`, so subsequent saved indices select the wrong object
or exceed the new vector size. The existing per-node context-menu guard did not
protect the outer root loop.

**Fix:** `HierarchySnapshot` owns row IDs, names, display metadata, and ordered
child/root IDs for a complete frame. Refresh reuses row/string/vector capacities
and reconstructs links from authoritative parent IDs while preserving valid sibling
ordering. Drawing and recursion retain no pointers into scene storage. Actions
resolve live IDs at the point of use; deleted queued rows are skipped. Refresh
removes deleted rows and stale links. Bounded path/ancestor vectors are retained
between frames instead of allocating traversal sets on every draw.

**Source:** [HierarchySnapshot.h](../src/EditorWindows/HierarchySnapshot.h) and
[SceneHierarchyWindow.cpp](../src/EditorWindows/Panels/SceneHierarchyWindow.cpp).

**Validation:** Production-cache regressions force live vector reallocation,
delete while retaining the cache, reorder/reparent, and cover stale child links,
orphans, cycles, filtering, and storage reuse. Live UI stress remains to run.

### F2 ? P1: drag/drop invalidates a hierarchy row before its guard

**Trigger:** Import a model or reparent/delete a row during hierarchy drawing.

**Before:** The row retained a live `SceneObject&`. Import could reallocate its
vector; root reordering could place a different object at that address before a
later mutation guard ran.

**Fix:** Display reads and recursion now use the owned snapshot above. Model,
texture, component, and context-menu actions resolve IDs when needed. Deleting a
row finishes its ImGui tree scope before returning. No traversal reference into
live scene storage crosses those actions.

**Validation:** Snapshot mutation regressions and editor compilation pass. A live
mouse-driven drag/import/delete stress pass is still required.

### F3 — P1: MMesh surface-range integer overflow bypasses validation

**Trigger:** An asset with six indices, `indexOffset = 3`, and
`indexCount = UINT32_MAX`. Both values satisfy the triangle-alignment test.

**Before:** The 32-bit sum `indexOffset + indexCount` wraps to `2`, so the validator
accepts the surface. `buildRenderData` can then read outside the index buffer.

**Fix:** After validating the offset, compare the count against
`indices.size() - indexOffset`; there is no overflowing addition.

**Source:** [MMesh.cpp](../src/Render25D/MMesh.cpp), `ValidateMMeshAsset`, line 117.

**Validation:** The new regression fails against `HEAD`'s original validator and
passes against the modified production validator. Ordinary and boundary ranges
are covered too. The unsafe render loop is not executed to demonstrate the crash.

### F4 — P2: malformed MMesh files request allocations before bounds checks

**Trigger:** Open a short/corrupt `.MMesh` whose header advertises huge vertex,
index, material, surface, or metadata counts, or a string length larger than the file.

**Before:** `resize` attempted those allocations before enough data was known to
exist, potentially causing a long stall, memory exhaustion, or an uncaught
allocation exception rather than an import error.

**Fix:** Compute the minimum payload size in 64-bit arithmetic and compare it with
the remaining file before resizing. Check each string's remaining bytes before
allocating it. No arbitrary valid-asset size cap was introduced.

**Source:** [MMeshLoader.cpp](../src/Render25D/MMeshLoader.cpp), `HasRemainingBytes`,
`ReadString`, and `MMeshLoader::loadAsset`.

**Validation:** Production loader tests reject each impossible count, oversized
strings, and truncation; preserve the caller's last valid asset on failure; and
read version 1/version 2 assets, including UVs, normals, and metadata. This does
not make every physically large but valid asset fit in available memory.

### F5 — P1, POSIX: detached HTTP jobs access an engine they do not own

**Trigger:** A non-streaming script HTTP request survives engine destruction; or a
streaming request needs the project-relative temp fallback during shutdown/project change.

**Before:** A detached lambda captured `this`, called `httpPostFromScript`, and/or
resolved a project path from the worker. The shared request state kept the request
alive but did not keep the engine alive or synchronize project changes.

**Fix:** Capture the fallback directory on the caller thread. A free transport
helper handles the synchronous/non-streaming operation, and the worker captures
only values and its shared request state. Removed an unused `this` capture from
the Windows process worker too; that unused capture alone was not a Windows UAF.

**Source:** [Engine.cpp](../src/Engine.cpp), `httpPostScriptRequest`,
`Engine::httpPostFromScript`, and `Engine::startHttpPostFromScript`.

**Validation:** Windows editor/player syntax checks pass. The POSIX lifetime path
was reviewed but has not been compiled or stress-tested on Linux/Android here.
HTTP status is now addressed in F10. Cancellation still does not immediately
interrupt a blocked HTTP transport.

### F6 — P1: export/compiler workers can outlive their engine mutexes

**Trigger:** Close the editor while an export runs, or destroy/unwind an Engine
while an export/compiler job is still using its captured engine pointer.

**Before:** `exportJob.future` is declared before `exportMutex`; similarly,
`compileWorkers` is declared before `compileMutex`. Reverse member destruction
destroys the mutex before a member future joins its worker. Normal shutdown waited
for compilation but did not drain exports before backend teardown.

**Fix:** An explicit engine destructor joins engine-dependent jobs before member
destruction. Normal `shutdown` uses the same helper before backend teardown. It
requests export cancellation, waits without holding the worker's mutex, and drains
compiler futures. Snapshot-only project-load/scan jobs retain their existing ownership.

**Source:** [Engine.h](../src/Engine.h), destructor and private helper declarations;
[Engine.cpp](../src/Engine.cpp), `Engine::~Engine`, `waitForBackgroundJobs`, `shutdown`.

**Validation:** Both editor and standalone-player configurations pass syntax
checking. Close-during-export/compile and exception-unwinding stress tests remain
to run. Shutdown may wait for an external build command to reach a cancellation
point; it does not forcibly kill build tools.

### F7 — P1: self-unregistering RPC destroys its executing callable

**Trigger:** A local or received RPC handler unregisters itself (or clears the RPC
table) and then continues using its captures.

**Before:** Dispatch invoked the `std::function` directly inside `rpcHandlers`.
Erasing that entry destroyed the active callable and its captured state.

**Fix:** Copy the handler to a local owning `std::function` before calling it in
both local and received-event paths. Its captures survive until dispatch returns.

**Source:** [NetworkSession.cpp](../src/Network/NetworkSession.cpp),
`NetworkSession::callRpc` and `handleEvent`, around lines 825 and 890.

**Validation:** All 163 networking self-test checks pass, including new lifetime
checks for synchronous local and loopback-delivered self-unregistering RPCs. This
fix protects the callable; backend ownership is addressed in F12.

### F8 (formerly R1) ? P1: script callbacks invalidate context and iterators

**Fix:** Stack-scoped `ScriptContextBinding` records object ID and script slot/path,
then refreshes nested dispatch contexts after create/delete/reparent/attach and
network scene mutations, before control returns to script code. Self-deletion
clears both pointers. Native/managed dispatch stops later phases when the instance
is gone. Engine update resolves each object afresh and avoids old references after
callbacks. Collision dispatch resolves both IDs for each script slot. Stopped or
removed IEnum tasks are checked before dispatch. Custom inspectors refresh their
context and finish UI scopes when storage changed. ABI bumped from 59 to 60.

**Source:** [ScriptContextBinding.h](../src/ScriptContextBinding.h), ScriptRuntime,
ManagedScriptRuntime, Engine, and InspectorWindow.

**Validation:** Identity tests cover vector/script reallocation, in-place reorder,
nested dispatch, slot replacement, and deletion. Integration tests call real Engine
mutation APIs and load a compiled native DLL: Begin grows the scene before Update,
Begin deletes itself before Update, and an IEnum deletes itself before a second
copied task. All pass.

**Limit:** Script-authored raw pointers/references from `FindObject*`, object lists,
collision `other`, or direct fields are borrowed. Scripts must retain IDs and
resolve again after structural edits; these fixes cannot repair arbitrary local
copies of raw pointers inside native code. Enabled-Mono execution and live
collision/custom-inspector UI stress remain to run.

### F9 (formerly R2) ? P1/P2: GPU owners outlive their context

**Fix:** `Window` owns GLFW termination. As Engine's first member it dies after
GPU-owning members, during normal destruction and unwinding. Engine makes its main
GL context current for cleanup and clears global OBJ/model GPU caches before they
can outlive the window. `shutdown` no longer terminates GLFW early. Headless Window
instances do not terminate GLFW they never initialized.

**Validation:** Real OpenGL 4.6 tests create a production GPU mesh and verify its
buffer is deleted with a live context and no GL error, both normally and during
exception unwinding. Integration tests also destroy a headless Engine. Full
initialized-editor exit, failed GL-loader initialization, multiple contexts/XR,
Vulkan, and Android remain manual/platform validation scenarios.

### F10 (formerly R3) ? P2: HTTP success guessed from response text

**Fix:** WinHTTP carries explicit read/transport success and numeric HTTP status.
The shared curl helper reads final response headers separately from the body and
checks curl completion and status. Both async modes use explicit completion status;
cancellation overrides success. No body substring is used. Streaming and buffered
requests share transport implementation.

**Validation:** Real loopback-only Windows HTTP tests cover a 200 body containing
`failed cancelled HTTP 500`, HTTP 500, empty HTTP 204, and invalid URLs. Streaming
and buffered modes pass. POSIX curl was reviewed but not compiled/executed here.

### F11 (formerly R4) ? P2: Windows cancellation leaves processes running

**Fix:** Script processes use hidden `CreateProcess` workers with output pipes and
owned job objects. Launch suspended, assign the job, then resume. Cancellation
terminates that job and descendants; cancellation before launch is remembered.
Only this worker's standard handles are inherited. Job publication, termination,
and closing share a mutex. Timeout messages distinguish confirmed completion from
termination still pending. Windows interactive stdin remains unsupported as before.

**Validation:** Production API tests cover captured output, nonzero exit, confirmed
descendant startup, cancellation, absence of its delayed marker, and blocking
timeout. All pass. POSIX process cancellation behavior was not changed.

### F12 (formerly R5) ? P1: backend destroyed in its own callback

**Fix:** Backend calls retain shared ownership until their call stack unwinds.
Session generations reject callbacks from stopped/replaced sessions; tick stops
synchronization after shutdown. Loopback drops the rest of its active work batch.
Initialization and synchronous forwarding retain ownership too. Ownership and
left-room loops iterate IDs and check callback shutdown instead of retaining map
iterators across callbacks.

**Validation:** Networking tests queue a shutdown RPC followed by another event,
re-register that second handler after shutdown to distinguish stale delivery from
an empty table, and shut down inside a synchronous connect-state notification.
All pass. Live Photon service/reconnect stress remains to run.

### F13 ? P2: nested UI/layout objects hidden in hierarchy

**Before:** Grids and other controls are created under a Canvas which may remain
collapsed. Search omitted matching children when their parent name failed the
filter; incomplete child-ID lists could hide valid scene objects too.

**Fix:** Reveal ancestors of newly selected objects, retain/open ancestor paths to
search matches, and reconstruct cached child links from parent IDs while respecting
valid sibling order. Layout objects remain ordinary scene objects; no duplicate
ModuOBJ asset is fabricated.

**Validation:** Grid-under-Canvas, missing/stale link, ancestor filtering, deletion,
and reparenting regressions pass. Menu creation was traced in
`renderSceneObjectCreateMenu`; live menu/UI stress remains to run.

## Validation record and limits

Executed on Windows/MSVC:

```powershell
python tools/engine-audit-selftest/run.py
python tools/engine-audit-selftest/run_hierarchy.py
python tools/engine-audit-selftest/run_network.py
python tools/engine-audit-selftest/run_engine.py
python tools/engine-audit-selftest/run_graphics.py
python tools/engine-audit-selftest/check_compile.py
```

- Mesh: **16 checks, zero failures** using production parser/validator. The original
  HEAD validator intentionally fails the wrapping regression with `run.py --before`.
- Hierarchy/context identity: **22 checks, zero failures**. Scope-helper unit tests
  use a test ID resolver; Engine integration below covers the production resolver.
- Networking: **169 checks, zero failures** with changed session/loopback sources
  and existing engine/dependency libraries.
- Engine/native-script/HTTP/process: **52 checks, zero failures**, compiling changed
  Engine/runtime/network/window/hierarchy units plus a native script DLL. HTTP
  fixtures serve only on `127.0.0.1`; process fixtures are local.
- Graphics: **two real GPU cleanup paths, zero failures** on the local GL driver.
- Changed Engine/runtime/window units pass editor/player syntax checks; hierarchy
  and networking are checked in the editor. Configured builds have Mono disabled.
- CMake/MSBuild hit `E_ACCESSDENIED` initializing Visual Studio's file tracker.
  Direct compiler/linker calls work. Full engine-source builds compiled all **134
  editor** and **112 player** translation units with configured Release
  definitions/includes and existing dependency libraries. Both executables linked.
  `run_engine.py --full-build` then passed all **52 checks** against the rebuilt
  optimized editor archive. Third-party dependency libraries were reused.
- The standard editor/player executables, resources/docs, and ABI-60 script SDK
  are staged by `build_direct.py --resume --install`; previous binaries/libraries
  are retained under `build/engine-audit-check/full-build/previous-build-*`.
  Live editor/player gameplay and mouse-driven hierarchy stress remain untested.
- No ASan/page-heap run, Linux/Android build, live mouse-driven editor stress pass,
  enabled-Mono runtime execution, or historical minidump replay was performed.

Native module retention in `ScriptRuntime::unloadAll` was preserved. Historical
crash reports/dumps were not removed or altered. These fixes address source defects
and do not establish the cause of every older crash.
