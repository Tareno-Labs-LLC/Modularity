# Depth ambient occlusion and camera motion blur

The ModuVolume Ambient Occlusion and Motion Blur sections work in the editor, game previews, standalone player and OpenXR eyes. Both effects consume the depth texture and exact projection of the view being processed; a 2D canvas or arbitrary color-only post-process has no geometry and skips them.

## Ambient occlusion

AO reconstructs view-space positions and geometric normals from depth instead of using image brightness. Six horizon directions with three distance samples run at half width and height. Two separable, depth-aware filters remove the spatial sampling pattern; the composite performs a four-tap joint bilateral upsample to keep contact shading off unrelated foreground/background surfaces. Sky is excluded. Normals use the nearer depth neighbor on each axis, reducing silhouette artifacts.

Two persistent RG16F targets store visibility and linear depth (about 4 MiB combined at 1920x1080). Their format remains floating point when scene color is SDR8. The renderer reuses these targets across sequential views and grows storage only when a larger view needs it. Smaller views use a bounded region, avoiding allocation when XR and desktop resolutions alternate. No normal prepass, random-noise texture or temporal accumulation is required. Disabled AO submits no AO passes. The projected sample radius is bounded at 128 full-resolution pixels.

The Inspector shows radius in meters; try 0.3?0.6 m and strength 0.6?1.0 for room-scale geometry. The existing serialized `aoRadius` field retains its historical scale: meters / 100. Existing scenes and script field names continue to load without a scene migration. AO darkens scene color before tone mapping; it does not separate ambient from direct lighting, and geometry outside the depth image cannot contribute.

## Motion blur

This is depth-reprojected **camera motion blur**. The previous view-projection transforms each current pixel into the previous view, giving its screen velocity. The shader samples the current HDR image along that velocity, with a motion-dependent sample count (maximum 12), a minimum pixel threshold, a screen-size clamp and depth rejection at silhouettes. It never blends a previously tone-mapped frame into the current one, and requires no full-frame history copy or history color texture.

- Strength is shutter duration relative to 1/60 second. Frame time normalization keeps exposure consistent across frame rates. Start with 0.15?0.4.
- Min Motion is the smallest trail in pixels. The stored `motionBlurThreshold` remains pixels / 10.
- Max Blur is the maximum trail as a percentage of image height. The stored `motionBlurClamp` remains percentage / 10.

Desktop, each preview slot and each XR view index own separate matrix histories. A stable view index is essential: swapchain framebuffer IDs rotate. `renderSceneToTarget(..., clearTarget, applyPostFX, temporalView)` accepts indices 0?3 for stable external views; its default -1 uses no temporal history. OpenXR supplies its actual view index. Each eye uses its own asymmetric projection.

First frames, resolution changes, large projection changes, camera cuts (more than 2 m or 60 degrees per rendered frame), and gaps of 250 ms invalidate the history. A stationary camera remains sharp. Moving objects, skeletal animation and shader-deformed foliage do not generate independent object velocity in this implementation. Existing motion blur remains opt-in, including in VR.

## Validation

`python tools/shader-selftest/postfx_test.py` exercises the shipping GL shaders on synthetic depth/color: contacts, radius response, unoccluded planes, sky, orthographic and asymmetric projections, stationary/first-frame behavior, camera motion, motion thresholds, blur clamps and silhouette rejection.

An offscreen 1920x1080 GPU benchmark on the local AMD Radeon RX 580 2048SP measured medians of 0.22 ms for the base composite, 1.14 ms with AO, 1.39 ms with maximum-sample camera blur, and 2.27 ms with both. These are synthetic per-view shader costs, not complete scene or headset frame times. Run `python tools/shader-selftest/postfx_test.py --benchmark` to reproduce.

`python tools/postfx-selftest/run.py` links the configured Release engine for real renderer integration checks, including independent external-eye/preview/desktop histories, camera cuts, resizing, floating-point AO under SDR color, and GL state/error checks. Pass `--player` to run against the standalone runtime library. A headset is still needed to assess comfort and final visual appearance in the actual scene.
