# Tree, XR pointer and UI reflection rendering

Project shaders animate foliage in the vertex shader. Trunk/leaf vertex buffers and
auxiliary streams are uploaded when the mesh loads; they are not regenerated for wind.
A pre-change four-Oak fixture measured zero vertex-buffer uploads after warm-up.

Profiles can opt into generic GPU instancing (`instancing=1`, `MODU_INSTANCED` vertex
variant, model matrix attributes 5-8). Identical visible meshes/materials draw together
using their original VAO, including colors and auxiliary UV streams. Only changed
instance transforms upload. Model matrices remain separate, preserving the origin,
branch pivots, authored weights and independent phase of each tree. Opaque/cutout
forward, G-buffer and directional/cube shadows share these transforms. Transparent
sorting and skeletal/dynamic meshes retain individual draws. The cache is bounded
and recycles old material/mesh entries. Material/texture/parameter and reflection probe differences split groups.

Dense leaves additionally opt into a project-owned `depthFragment` stage. This
lightweight alpha mask writes the nearest leaf depth using the same animated vertex
program; the lighting pass then reads equal depth without writing it. Hidden leaf
layers therefore skip full material, light and shadow shading. Mesh buffers remain
unchanged, and the depth draw reuses the same instance transforms. Profiles without
this stage, single objects, array-textured profiles and failed depth programs retain
ordinary forward rendering. No mesh simplification or lower shadow quality is involved.

Point-light shadow cubemaps are reused between the views of one `setFrameSerial()`
frame, provided light settings, scene storage, culling mask and wind state agree.
Directional maps reuse only when their fitted light matrix also agrees; different
view coverage never reuses an unsuitable map. The scene is immutable between views
of one frame, matching the existing transform/material cache contract. Frame serial
0 disables reuse. Source mesh reuploads are not part of this optimization.

The controller ray uses the existing XR Ray Interactor's Show Line Visual and Line
Color controls. A depth-tested, camera-facing quad supplies the beam and a hit dot;
no native wide-line support or generated mesh is required. Untracked controllers,
disabled interactors, hidden layers and inactive XR sessions produce no beam. The
visual uses the same XYZ rotation convention and hit distance as interaction.

World-space UI contributes alpha-tested depth and a non-reflective surface marker
after the opaque SSR surface pass. SSR rays therefore hit the actual canvas plane;
the floor behind the canvas no longer inherits its color as if it were floor detail.
Transparent corners retain the depth/normal behind them. Both standalone and atlas
RTTs use their existing UV transform. This adds no new full-screen attachment.

## Performance targets and validation

Requested budgets: desktop 250 FPS = 4 ms; PC VR 144 FPS = 6.94 ms; standalone Quest
120 FPS = 8.33 ms. These are whole-frame targets, not guarantees from a desktop GPU
shader microbenchmark. Headset display cadence is runtime-paced through `xrWaitFrame`,
independently of the desktop frame cap. The headset/runtime must select the desired
supported refresh rate. No extra sleep limiter is inserted into XR rendering.

`python tools/tree-ui-selftest/run.py` runs the real GL renderer against UI depth,
transparent canvas corners, floor SSR, controller beam/reticle, visibility and tracking
loss. `--player` runs the same checks with the standalone runtime library.
Project-owned tree shader/performance fixtures remain in the private port tools,
with the paid shader implementation in the project. The engine package remains generic.

The native private fixture renders four original Oak meshes (four trunks and four
leaf meshes), one directional light and one point shadow, with one or three sequential
960x720 views, after warm-up. It counts real GL draws and uploads, and waits on the GPU
only inside the offline benchmark. It also supports loading saved scene geometry;
that measurement excludes the live ImGui canvas producer and full engine update loop.

The saved-scene fixture resolves local transforms through the full parent hierarchy
and uses a fixed in-room view at (0, 1.6, 5.5), looking toward (0, 1.3, -3.2), to show
the trees and kiosk. The old library and ABI-matching SDK were preserved privately
for comparison. Captures verify the representative view before reporting timings.

The additional leaf depth pass was compared on the same instancing-capable engine,
with only the optional depth stage disabled/enabled. At 1920x1025 on the local
RX 580 2048SP, one representative room view initially measured 6.11 ms without it
and 3.88 ms with it. An off/on/on/off repeat measured 6.24/3.97/4.01/6.21 ms,
about 36% less render time with the pass. Three sequential views averaged 19.12 ms
without it and 11.88 ms with it in that repeat. These are renderer
fixture measurements, not total game or headset FPS. Project light/shadow settings
are loaded unchanged, and post-processing is disabled for this comparison to match
the supplied screenshot's inactive volume. The live ImGui canvas producer, player
updates and headset compositor are excluded. A static avatar remains in this test view.

Depth and silhouette coverage match exactly in the four-tree comparison. Some
coincident leaf triangles can select a different color at equal depth: 65 pixels
out of 1,968,000 exceeded 0.002 in the HDR fixture (mean component error 0.00000342).
The representative room capture differed by more than one 8-bit level at seven
pixels. This is not a bit-identical color optimization; texture detail, lighting,
wind, mesh density and shadow filtering remain at their authored settings.

Physical Index/Quest frame-time and power measurements are still needed to validate
the requested rates, especially standalone Android. Desktop FPS is not a substitute
for mobile GPU/CPU performance. No foliage density, shadow quality, texture resolution
or user scene settings are reduced to improve the benchmark.

## Shadow and submission hitches

Instance groups can contain different objects in each cube face, camera view and
forward pass. When the matrix list changes, the renderer now discards the old
buffer storage before uploading the full replacement. Unchanged groups still
reuse their existing upload. This avoids overwriting matrices that earlier GPU
draws may still be reading. GPU culling also clears its counter on the GPU timeline
instead of updating that in-use buffer from the CPU.

Shader timestamp polling runs on one background worker per initialized renderer,
started only when polling is needed. Shader compilation remains on the GL thread
and only occurs for new programs or actual edits. Reload detection can take about
one second. File read errors leave existing programs alone.

Existing textures that fail to decode are retained as failures until their source
timestamp changes or the asset is explicitly invalidated. Missing files retain
the short retry policy. Failed EXRs also watch for creation/modification of their
transcoded PNG sidecar. This removes the previous 30-second cycle of re-reading
and decoding unchanged unsupported EXRs inside shadow/material submission.

The profiler breaks out `Shadows::Directional`, `Shadows::Point`,
`Shadows::ProjectCasters`, `Instances::Upload`, `Instances::Draw`,
`Submission::ReflectionLookup`, `Submission::InstanceGroups`,
`Submission::CutoutDepth`, `Submission::MaterialAndState`, `Submission::Draw`,
`Texture::LoadAndUpload` and `Shader::CompileAndLink`. These are CPU durations;
GL draw scopes can include driver backpressure. Parent scope durations include
their children and must not be added together.

`python tools/render-stall-selftest/run.py` checks that render-thread timestamp
polls complete while the file worker is blocked. The tree/UI integration check
also tests background shader reload, forced reload stability, unchanged failed
texture caching beyond the old 30-second deadline, and recovery after an edit.

The saved-scene check on RX 580 2048SP used 1920x1025 per view, 124 scene objects,
28 renderers including four trees (eight tree mesh objects), unchanged project
graphics/post-processing settings, and 1,200 frames per mode with 16 warm-up
frames. Before/after results from one paired run:

| Renderer fixture | Median before/after | 99th percentile before/after | Maximum before/after |
| --- | --- | --- | --- |
| One view | 12.47 / 8.81 ms | 16.89 / 10.19 ms | 31.63 / 26.38 ms |
| Three sequential views | 38.31 / 24.79 ms | 48.27 / 27.09 ms | 57.15 / 28.30 ms |

The buffer trace counted 7,198/7,200 writes to previously submitted instance
storage in the two old runs and zero in both replacement runs. These are potential
synchronization hazards, not measured wait counts. The saved-scene 8-bit capture
matched byte for byte. Three sequential views exercise the shared shadow and
instance code; this is not a single-pass headset FPS measurement. Live canvas
production, full game updates and headset submission/composition are excluded.
The supplied 604 ms spike was not reproduced in this fixture; the additional
profiler markers remain available to diagnose any remaining driver or asset stalls.
