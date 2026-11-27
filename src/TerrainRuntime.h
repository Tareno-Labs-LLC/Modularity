#pragma once
// Runtime-safe helpers for the Terrain component (compiled into the editor and the player):
// the `.modterrain` sidecar codec, height/normal/layer/keep-out queries, procedural generation
// and the brush kernels. No ImGui or GL in here; the editor UI, renderer and physics layers call in.
//
// Terrain-local space is centred: X and Z span [-size/2, size/2]. A height sample (x, z) sits at
// local X = -size/2 + x * cell, local Z = -size/2 + z * cell with cell = size / (resolution - 1);
// its local Y is heightOffset + heights[z * resolution + x] * heightScale. The splat texel
// (i, j) covers u = (i + 0.5) / splatResolution along +X and v = (j + 0.5) / splatResolution along +Z.
//
// `.modterrain` file (little endian):
//   char[4]   "MTER"
//   uint32    version (1)
//   uint32    resolution          (height samples per side)
//   uint32    splatResolution     (splat texels per side)
//   float32   size                (metres)
//   float32   heightScale         (metres)
//   float32   heights[resolution * resolution]       row-major, row = Z, 0..1 normalized
//   uint8     splat[splatResolution^2 * 4]           RGBA8, row = Z: R grass, G forest floor/dirt, B path, A rock
//   uint8     grassDensity[splatResolution^2]        OPTIONAL trailing block; absent = 255 everywhere
#include "SceneObject.h"

#include <string>
#include <vector>

namespace TerrainRuntime {

constexpr int kLayerCount = 4;
constexpr uint32_t kSidecarVersion = 1;

// ---- Geometry helpers -------------------------------------------------------------------------
inline float CellSize(const TerrainComponent& t) {
    return t.size / static_cast<float>(std::max(1, t.resolution - 1));
}
// The object's world matrix exactly as the renderer builds it (T * Rx * Ry * Rz * S).
glm::mat4 WorldMatrix(const SceneObject& object);
// True if the object draws / collides as a terrain right now (component present, enabled, has data).
bool IsActive(const SceneObject& object);

// ---- Data ---------------------------------------------------------------------------------------
// Allocates (or resizes, resampling) the buffers for the component's resolution settings.
// A fresh terrain is flat at local Y = baseHeightMetres and entirely grass.
void InitializeFlat(TerrainComponent& terrain, float baseHeightMetres = 0.0f);
// Resamples heights and splat to a new resolution (bilinear); keeps the shape.
void Resize(TerrainComponent& terrain, int newResolution, int newSplatResolution);
// Density painted by the Grass brush (255 = full). Allocated on first edit.
std::vector<uint8_t>& MutableGrassDensity(TerrainComponent& terrain);

// "..._NormalDX.jpg" -> "..._<suffix>.jpg" (Roughness / AmbientOcclusion sibling maps next to a layer's
// normal map); empty when the name has no "_Normal" part.
std::string SiblingMapPath(const std::string& normalPath, const char* suffix);

// ---- Sidecar ----------------------------------------------------------------------------------
std::vector<uint8_t> EncodeSidecar(const TerrainComponent& terrain);
bool DecodeSidecar(const uint8_t* bytes, size_t size, TerrainComponent& terrain, std::string* error);
// Reads terrain.dataPath through the asset source (project folder or bundle). Sets resolution,
// splatResolution, size and heightScale from the file header. False leaves a flat terrain behind.
bool LoadSidecar(TerrainComponent& terrain, std::string* error = nullptr);
struct SaveReport {
    int written = 0;
    std::vector<std::string> errors;
};
// Editor save pass (non-const: assigns Assets/Terrain/<name>_<id>.modterrain to terrains without
// a path or sharing one with another terrain, then writes every terrain whose data changed).
SaveReport SaveDirtySidecars(std::vector<SceneObject>& objects, const std::string& projectRoot);

// ---- Queries (terrain-local) ------------------------------------------------------------------
// Local Y of the surface at local XZ, matching the rendered/collided triangulation. Outside the
// terrain the edge height is returned and `inside` (when given) is false.
float LocalHeightAt(const TerrainComponent& terrain, float lx, float lz, bool* inside = nullptr);
glm::vec3 LocalNormalAt(const TerrainComponent& terrain, float lx, float lz);
// Layer weights (R grass, G forest floor/dirt, B path, A rock), normalized to sum 1.
glm::vec4 LayerWeightAtLocal(const TerrainComponent& terrain, float lx, float lz);
float GrassDensityAtLocal(const TerrainComponent& terrain, float lx, float lz);
// 0 free .. 1 blocked: path weight, ponds (+margin) and keep-out clearings.
float KeepOutAtLocal(const TerrainComponent& terrain, float lx, float lz);
// Signed distance (metres, negative inside) from a local point to a clearing's shape.
float ClearingDistance(const TerrainClearing& clearing, float lx, float lz);

// ---- Queries (world space, used by the Instancer scatter, grass and scripts) ------------------
struct SurfaceSample {
    bool valid = false;                              // a terrain covers this XZ
    float height = 0.0f;                             // world Y
    glm::vec3 normal = glm::vec3(0.0f, 1.0f, 0.0f);  // world space
    glm::vec4 layers = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    float keepOut = 0.0f;
    float grassDensity = 1.0f;
    int terrainId = -1;
};
bool SampleObject(const SceneObject& terrainObject, float worldX, float worldZ, SurfaceSample& out);
// First active terrain (in scene order) that covers the point.
bool SampleScene(const std::vector<SceneObject>& objects, float worldX, float worldZ, SurfaceSample& out);
float HeightAt(const std::vector<SceneObject>& objects, float worldX, float worldZ, float fallback = 0.0f);
glm::vec3 NormalAt(const std::vector<SceneObject>& objects, float worldX, float worldZ);
glm::vec4 LayerWeightAt(const std::vector<SceneObject>& objects, float worldX, float worldZ);
float KeepOutAt(const std::vector<SceneObject>& objects, float worldX, float worldZ);
// Registers SampleScene as InstancerTools' ground query (scatter snaps to terrain + keep-out).
void RegisterGroundQuery();

// World-space ray against the heightfield. Returns the hit distance along `direction`.
bool Raycast(const SceneObject& terrainObject, const glm::vec3& origin, const glm::vec3& direction,
             float maxDistance, float& hitDistance);

// ---- Chunk bounds (shared by renderer, picking and physics) ------------------------------------
constexpr int kChunkQuads = 32;
inline int ChunkCountPerSide(const TerrainComponent& t) {
    return std::max(1, (t.resolution - 1 + kChunkQuads - 1) / kChunkQuads);
}

// ---- Generation -------------------------------------------------------------------------------
struct GenerateOptions {
    float baseHeightMetres = 0.0f;   // Local Y of the noise zero level.
    bool applyClearings = true;      // Flatten each clearing.
    bool carvePonds = true;
    bool autoPaint = true;           // Rewrites the splat: grass, forest-floor patches, rock on slopes, dirt on shores.
    float rockSlopeDegrees = 38.0f;
    float pondDepth = 1.8f;
};
// Fills heights (and optionally splat) from the component's gen* recipe, then clearings and ponds.
void Generate(TerrainComponent& terrain, const GenerateOptions& options);
// Re-applies only clearings / ponds on top of the current heights.
void ApplyClearings(TerrainComponent& terrain);
void CarvePonds(TerrainComponent& terrain, float depth);
// Slope/height based splat repaint (see GenerateOptions::autoPaint).
void AutoPaint(TerrainComponent& terrain, float rockSlopeDegrees);
// Paints a soft dirt/path line segment (width in metres) into layer `layer`.
void PaintLine(TerrainComponent& terrain, glm::vec2 a, glm::vec2 b, float width, int layer, float strength = 1.0f);
// Slope in degrees at a sample.
float SlopeDegreesAt(const TerrainComponent& terrain, float lx, float lz);

// ---- Brushes (editor) ---------------------------------------------------------------------------
enum class BrushMode { Raise = 0, Lower, Smooth, Flatten, PaintLayer, GrassDensity };
struct BrushParams {
    BrushMode mode = BrushMode::Raise;
    float radius = 8.0f;        // metres
    float strength = 0.3f;      // 0..1
    float falloff = 0.6f;       // 0 hard edge .. 1 fully soft
    int layer = 0;              // PaintLayer target
    float flattenHeight = 0.0f; // local Y for Flatten (captured on stroke start)
    bool invert = false;        // GrassDensity: erase; PaintLayer: unused
};
// One stamp at a terrain-local position. Returns the touched sample rect (heights) or splat rect.
// `newStroke` starts a fresh dirty-rect accumulation (first stamp of a stroke).
void ApplyBrush(TerrainComponent& terrain, float lx, float lz, const BrushParams& params, bool newStroke);

// ---- Noise ---------------------------------------------------------------------------------------
float PerlinNoise(float x, float y, uint32_t seed);   // roughly -1..1
float Fbm(float x, float y, uint32_t seed, int octaves, float persistence, float ridged);

} // namespace TerrainRuntime
