# Project material shader profiles

Custom material implementation belongs under a project's Assets directory. The engine ships no custom material profiles or implementations. Choose a project `.modushader` asset using the material shader field; its declared controls appear under Shader Controls.

A profile extends the existing shader-pack format:

```ini
name=My Surface
vertex=Shaders/surface.vert
fragment=Shaders/surface.frag
gbufferFragment=Shaders/surface_gbuffer.frag
shadowDirectionalVertex=Shaders/surface_shadow.vert
shadowCubeVertex=Shaders/surface_cube.vert
textureArrays=0
twoSided=0
cutout=0
uniform=0|float|surfaceGain|Gain|0.5|control|0,1
uniform=1|vec3|surfaceTint|Tint|1,1,1|control|0,1
```

Paths resolve relative to the profile file. Shader paths support the existing `?DEFINE,DEFINE` suffix. Each uniform declaration names a slot (0?31), type (`float`, `int`, `bool`, `vec2`, `vec3`, `vec4`), GLSL uniform, UI label, comma-separated defaults, reserved control tag, and minimum/maximum. Labels, defaults and shader programs are supplied by the project. Parameters remain trivially copyable for existing renderer caches and the native script ABI.

The renderer supplies `sceneTime`, `sceneWind` (direction xyz and strength w), `sceneWindTurbulence`, `meshHasAuxiliaryStreams` (color plus UV1 and UV2), and `meshHasAuxiliaryAxis` (three-component UV2). Existing standard material uniforms and texture slots remain available. Optional array inputs use `albedoArray` and `normalArray` on units 20/21 and require 22 fragment texture units. Custom profiles currently draw as opaque or binary cutouts, disable mesh merging/MDI, and use conservative expanded culling bounds. GPU-skinned objects keep the standard rendering path.

A custom profile should provide its matching G-buffer and both shadow vertex stages to keep animated geometry consistent. Runtime export stages the profile directory and its shaders only for projects that reference them. Keep profile shader files below that directory. Engine distribution packages contain only the generic interface.

Native `.mat` files store `shaderProfile` and `shaderValue0` through `shaderValue31`. Scenes persist the same data in their renderer component. Unity YAML source materials remain untouched: a neighboring `<name>.mat.modularity.mat` provides an optional project-authored native material for import.

Native script ABI: 70. Restart the editor and rebuild native scripts after updating.

The Shading Model row is shown only while a project shader profile is active. Its dropdown selects another profile from the current project or returns to Standard. Shader Controls appear immediately below it. Absolute picker paths and portable saved paths for the same profile preserve parameter edits.

Project script discovery also scans C++ files outside Assets/Scripts. Keep developer test and backup C++ sources outside the project to prevent them from compiling as gameplay scripts.


## GPU instancing

A profile can opt in with `instancing=1`. Its vertex stages must accept the generic
`MODU_INSTANCED` variant: a per-instance `mat4` at locations 5-8 replaces the `model`
uniform. Locations 9-11 continue to carry original auxiliary vertex streams.
The forward, G-buffer and both shadow vertex stages must implement the same transform.
Profiles without this opt-in retain individual draws. Shader compile failures fall
back to the ordinary path. The renderer groups only matching meshes, shader parameters,
textures, culling state and transform handedness. Reflection cube/intensity differences split batches;
skinned/dynamic meshes keep individual rendering.

Dense cutout profiles can optionally supply `depthFragment=Shaders/depth_frag.glsl`.
The renderer links it to the same instanced forward vertex stage and supplies the
usual wind, profile parameters, UV transform, albedo, opacity and cutoff uniforms.
The depth fragment must discard exactly the same pixels as the forward program.
Declare `invariant gl_Position` in the vertex shader to preserve identical positions
between programs. The full lighting pass then uses read-only equal depth, rejecting
hidden leaf layers before expensive shading. This currently applies to instanced,
opaque cutouts with 2D textures. Missing or failed depth programs retain ordinary
forward rendering. The depth stage remains a project asset and is exported with
the profile directory.
