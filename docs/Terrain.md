# Terrain component

`hasTerrain` + `TerrainComponent` on any object (normally an Empty): a heightmap and a 4-layer paint map that
draw, shade, collide and answer height queries as chunked ground. Create it with **Create > 3D Primitives >
Terrain** or **Add Component > Rendering > Terrain**.

## Space and data

* Terrain-local space is centred: X and Z span `[-size/2, size/2]`. Sample `(x, z)` of a `resolution x resolution`
  grid sits at `-size/2 + index * size/(resolution-1)`. Local Y in metres is `heightOffset + height * heightScale`
  (`height` is normalized 0..1). The object's transform (position, rotation, scale) places the terrain in the world.
* Heights and paint live behind `std::shared_ptr` (copy-on-write), so undo snapshots stay cheap.
* Paint is RGBA8 at `splatResolution`: **R grass, G forest floor / dirt, B path, A rock**. A second optional
  channel stores a painted grass density (255 = full) for the grass pass.

## `.modterrain` sidecar

Heights and paint are not stored in the scene text. The scene keeps `terrainData="Assets/Terrain/<name>.modterrain"`
and the file is written when the scene is saved (only if the data changed; a terrain without a path gets
`Assets/Terrain/<object name>.modterrain`). Little endian:

| bytes | field |
|---|---|
| 4 | magic `MTER` |
| 4 | version (1) |
| 4 / 4 | resolution, splatResolution |
| 4 / 4 | size (f32), heightScale (f32) |
| res^2 * 4 | float32 heights, row-major, row = Z |
| sres^2 * 4 | RGBA8 paint, row = Z |
| sres^2 | optional grass density |

The codec is `TerrainRuntime::EncodeSidecar / DecodeSidecar`; `scratchpad/terrain_io.py` style tools can write the
same format. The file is referenced from the scene, so packaging (`SceneAssetReferences.h`, `ModuPak`) ships it, and
the player loads it through the asset source (project folder or bundle).

## Rendering

`RenderTerrain.cpp` splits the ground into 32x32-quad chunks with three LODs (every, 2nd, 4th sample), skirts that
hide LOD cracks, per-chunk bounds for frustum culling and lazy per-chunk mesh builds. The surface is the engine's own
`frag.glsl` compiled with `MODU_TERRAIN`, so shadows, image based lighting, fog and point lights are unchanged. Layers
without a texture get a procedural tile (grass, forest floor, path, rock); steep slopes turn to triplanar rock; layers
blend by height. The terrain casts shadows (directional and point light cubes) and renders in the deferred / SSR
prepass.

## Grass

`RenderGrass.cpp` + `Resources/Shaders/grass_vert.glsl`: every blade is generated in the vertex shader from
`gl_InstanceID` / `gl_VertexID` (no vertex data, nothing uploaded per frame, like the rain). The terrain supplies a
height texture (R32F, triangulated like the mesh, so roots sit on the surface), the splat map and the painted grass
density; grass grows on the Grass layer (a little on forest floor), and never on paths, rock or in ponds. Three LOD
rings around the camera (0.28 / 0.6 / 1.15 m cells; 3 / 2 / 1 segments per blade; blades get wider with distance),
cross-faded by density at the boundaries, shrinking into the ground at `Distance`. The cell grid is world-snapped, so
blades never swim. Each ring is drawn as runs of visible 3 m tiles (frustum / distance / hole culling on the CPU
using the pass frustum, which is the union of both eyes in XR) because the vertex shader costs the same for a blade
that ends up off screen. Lighting: `frag.glsl` compiled with `MODU_GRASS`: sun + a 4-tap shadow map lookup, nearby
point / spot lights, sky image based light (or the flat ambient), light transmission through the blade, bent normals,
fog; it renders in the scene views, game view and XR multiview path like any terrain item (skipped in reflection
captures and the SSR G-buffer). Wind comes from the scene wind (`sceneWind`, `sceneWindTurbulence`, `sceneTime`).
The ground under the grass is darkened within the grass distance (`terrainGrassShade` in the terrain shader).
Settings (Inspector > Terrain > Grass): density, blade height / width, height variation, bend, distance, wind, dry
patches, root / tip / dry colours. `MODU_GRASS_GPU_TIMING=1` prints the grass GPU time (ms) every 240 draws.

## Water

Every pond in the Ponds list gets a water surface (`water_vert.glsl` quad per pond + `frag.glsl` compiled with
`MODU_WATER`, a blended leader item drawn last). The quad only bounds the pond: texels where the terrain stands above
the pond's Water Height are discarded, so the visible shape is the carved basin. Depth below the surface (from the
terrain height texture) drives Beer-Lambert absorption between the shallow and deep colours, soft shore alpha and
ragged foam; procedural wave normals, Fresnel sky reflection from the environment cube, sun glints with the real
shadow map, fog. Settings: shallow / deep colour, clarity, wave size / strength, reflection, foam.

## Physics

At play start the Jolt backend builds a static `HeightFieldShape` per terrain with collision enabled (the actor id of
hits is the terrain object). PhysX is not implemented for terrain.

## Editor

Inspector > Terrain: shape (size, height scale / offset, resolution, paint resolution, LOD distance), **Sculpt &
Paint** (turn on *Edit Terrain*, pick Raise / Lower / Smooth / Flatten / Paint / Grass density; `[` `]` change the
radius, Shift inverts; each stroke is one undo step; the transform gizmo is hidden while the brush is armed), layers
(albedo, normal, tiling in metres, tint, normal strength), **Generate** (fBm noise recipe, clearings, ponds, slope
paint), **Ponds** and **Clearings**. Clicking the ground in the Scene view selects the terrain.

Clearings flatten the ground (`Flatten Clearings`) and, with *Keep Scatter Out*, stay empty for the Instancer.

## Queries

C++ (`TerrainRuntime.h`): `SampleScene / HeightAt / NormalAt / LayerWeightAt / KeepOutAt` in world space, `Raycast`
against the heightfield. `InstancerTools` uses them for **Scatter** (heights, keep-out, slope tilt) and **Snap To
Terrain** / **Remove Instances In Paths / Ponds**.

ModuCPP (`add ModuCPP;`):

```
float y = Terrain::HeightAt("Terrain", x, z);          // world Y, fallback 0 outside
vec3 n = Terrain::NormalAt("Terrain", x, z);
vec4 w = Terrain::LayerWeightAt("Terrain", x, z);      // grass, forest floor, path, rock
bool onPath = Terrain::IsPath("Terrain", x, z);
```

## Scene keys

Flat keys inside `NODE_Terrain(enabled=1) { ... }`: `terrainSize`, `terrainHeightScale`, `terrainHeightOffset`,
`terrainResolution`, `terrainSplatResolution`, `terrainCastShadows`, `terrainCollision`, `terrainLodDistance`,
`terrainData`, `terrainGen*`, `terrainLayer<N>Albedo|Normal|Tiling|Tint|NormalStrength`,
`terrainPondCount` + `terrainPond<N>Center|Radius|Height`, `terrainClearingCount` +
`terrainClearing<N>Center|Size|Rotation|Rect|Blend|UseHeight|Height|KeepOut`,
`terrainGrassEnabled|Density|BladeHeight|BladeWidth|HeightVariation|Bend|Distance|Wind|Dry|BaseColor|TipColor|DryColor`,
`terrainWaterEnabled|ShallowColor|DeepColor|Clarity|WaveScale|WaveStrength|Reflection|Foam`.
