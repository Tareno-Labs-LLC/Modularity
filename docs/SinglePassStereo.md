# Single-pass stereo

Choose **Project Settings > OpenXR > Rendering > Single Pass / Multiview**
for each headset target. Runtime Diagnostics reports the mode actually used.
The existing desktop/game viewport remains a separate camera render.

On a two-view OpenGL/OpenGL ES device supporting `GL_OVR_multiview2`, the
renderer submits scene geometry and the SSR surface prepass once for both
eyes. A layered color/depth target at the selected HDR or SDR precision receives the runtime's two distinct,
asymmetric projections. Object instancing, GPU wind, skinning, static caches,
indirect drawing and frustum culling retain their existing implementations.
Paid project shader implementations remain in their project.

The culling and directional-shadow camera encloses both eye frusta. Mono
Hi-Z occlusion and screen-size LOD rejection are bypassed for this shared
pass because they cannot safely reject geometry for both eye views. Saved
optimization settings are unchanged. Shadows and reflection captures run
outside the multiview geometry stage.

Each eye uses a 2D texture view of its array layer for SSAO, SSR tracing,
volumetric lighting and post-processing. These views share storage, avoiding
full-resolution color, depth and surface copies. Desktop GL 4.3 texture views
and GLES `EXT_texture_view` / `OES_texture_view` are supported; devices without
them use a copy-based resolve. Camera motion history remains separate for the
left and right eye. Sky backgrounds, CPU-projected 2.5D UI, particles and
controller beams retain their per-eye passes. World-space canvas geometry
and its alpha-hit reflection depth participate in the shared passes.
The sky draws after opaque depth and before transparent surfaces, preserving
depth rejection for indoor pixels and correct blending behind world UI.

Single-pass stereo reduces scene setup and geometry submissions. Both eyes
still require rasterization and shading; this does not promise double the
frame rate or a particular headset refresh rate.

Unsupported drivers, rejected array swapchains, incompatible material variants,
CPU-facing sprite models, and planar mirror objects use the existing multipass path. The VR editor
workspace also retains multipass for its overlays. A material fallback is
identified separately in Runtime Diagnostics. Array swapchain images are
acquired once, presented through separate array layers, and released once.

Material variants use the ordinary `view`, `projection` and `viewPos` uniforms.
The shader loader specializes these uniforms into two-view arrays when
compiling the internal `MODU_MULTIVIEW` variant. `model`, instance attributes,
bone transforms, texture interpretation, alpha masks and animation inputs
keep their existing contracts. Geometry-stage shaders remain mono capture
programs. Custom camera uniforms need their own stereo adaptation.

Whole-frame GPU timers and GPU-culling timer readbacks are skipped during
multiview because the extension leaves those query results undefined. Per-eye
post-processing timers remain available after resolving to ordinary targets.

Run `python tools/stereo-selftest/run.py` and its `--player` variant after
building Release. These use the production renderer and GPU without requiring
an OpenXR runtime. They compare asymmetric left/right output with multipass,
including world UI, SSR, optimized indirect/GPU culling, fallback and cleanup,
and count real geometry submissions. Headset compositor and standalone-device
performance must also be verified on the target devices.

Release validation on an AMD Radeon RX 580 2048SP covered both editor and player,
the texture-view path and the forced copy-based resolve. HDR/SDR scene color,
sky, UI, reflection surfaces, depth and animated instanced project-tree images
matched the multipass references exactly. SSAO matched; camera-motion blur
matched within timing tolerance with independently primed eye histories.
Controller-ray, canvas alpha/reflection, shadow-cache and post-processing
integration checks also passed.

The offscreen four-tree fixture at 1920 x 1025 **per eye**, with wind, alpha depth,
instancing and SSR, submitted 10 geometry draws in multipass and 5 in multiview.
Three alternating-order runs of 96 measured samples per path gave median
complete two-eye render times of 2.319 ms and 2.079 ms respectively (about 10%
less time). These measurements exclude headset submission, game updates and
the full game UI; they do not establish a headset refresh-rate guarantee.

An optional project mesh argument exercises imported material profiles without
bundling their implementation in the engine. Set `MODULARITY_STEREO_BENCHMARK=1`,
`MODULARITY_STEREO_WIDTH`, and `MODULARITY_STEREO_HEIGHT` for the alternating
benchmark; `MODULARITY_STEREO_COPY_RESOLVE=1` tests the compatibility resolve.

Implementation: [scene renderer](../src/Rendering.cpp),
[renderer API](../src/Rendering.h), [shader variants](../src/Shaders/Shader_Manager/Shader.cpp),
[XR submission](../src/XRRenderPath.cpp), [swapchain layers](../src/XR/XRSwapchainGL.cpp)
and [view lifecycle](../src/XR/XRSystem.cpp).
