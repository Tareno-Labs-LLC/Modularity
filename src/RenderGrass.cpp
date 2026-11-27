// GPU grass and pond water for the Terrain component (the Renderer half; the shaders are
// Resources/Shaders/grass_vert.glsl, water_vert.glsl and frag.glsl compiled with MODU_GRASS / MODU_WATER).
//
// Grass is generated in the vertex shader around the camera, like the rain: no vertex buffers, no
// per-frame uploads. The terrain contributes three textures (heights as R32F, the splat map it already
// has, and the painted grass density as R8) which are rebuilt only when the terrain's revisions change.
// Three draws (rings) with different cell sizes / blade counts / segment counts form the LOD; blades
// cross-fade between rings by density near the boundaries (see grass_vert.glsl).
//
// Both the grass and the water are "leader items": ordinary entries of the main pass draw list that
// carry no mesh of their own, so every light, shadow map, image based light and fog uniform the
// engine uploads for a program reaches them with no extra plumbing, and XR stereo (multiview), scene
// views and the game view all draw them through the same path as the terrain itself.

#include "RenderTerrainInternal.h"

#include "Profiler.h"
#include "RenderOptimizationInternal.h"
#include "TerrainRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kHeightUnit = 20;     // same units as the terrain's layer arrays (never both in one program)
constexpr int kDensityUnit = 21;
constexpr int kMaxPonds = 8;        // grass shader ponds
constexpr int kMaxWaterPonds = 16;

struct GrassRing {
    float cellSize;
    int baseBlades;      // blades per cell at density 1
    int segments;        // segments per blade (1 = a single triangle)
    float widthScale;    // coarser rings use wider blades to keep the field looking full
    float inner;         // boundary radii as fractions of the grass distance (0 = none)
    float outer;         // (1 = the far edge)
};

constexpr GrassRing kRings[3] = {
    {0.28f, 5, 3, 1.0f, 0.00f, 0.20f},
    {0.60f, 9, 2, 2.2f, 0.20f, 0.50f},
    {1.15f, 12, 1, 3.8f, 0.50f, 1.00f},
};

float RingFade(float radius) { return std::max(1.5f, 0.12f * radius); }

const std::string kGrassVert = "Resources/Shaders/grass_vert.glsl";
const std::string kGrassFrag = "Resources/Shaders/frag.glsl?MODU_GRASS";
const std::string kWaterVert = "Resources/Shaders/water_vert.glsl";
const std::string kWaterFrag = "Resources/Shaders/frag.glsl?MODU_WATER";
const std::string kEmpty;

bool WantsGrass(const TerrainComponent& t) {
    return t.grassEnabled && t.hasData() && t.grassDensityScale > 0.001f && t.grassBladeHeight > 0.001f;
}
bool WantsWater(const TerrainComponent& t) {
    return t.waterEnabled && t.hasData() && !t.ponds.empty();
}

// Everything draw() reads for one ring.
struct RingDraw {
    int instances = 0;
    int vertices = 0;
    int blades = 0;
    float keepScale = 1.0f;
    glm::vec4 cells;
    glm::vec4 range;
    glm::vec4 ring;
};

} // namespace

void Renderer::TerrainState::InitLeaderItem(SceneRenderItem& item, const SceneObject& obj, Mesh* mesh, const glm::mat4& model,
                    const glm::mat4& view, const glm::vec3& cameraPosition, const std::string& vert,
                    const std::string& frag, bool blended) {
    item.obj = &obj;
    item.mesh = mesh;
    item.model = model;
    item.sortCenter = glm::vec3(model[3]);
    item.depthSortCenter = item.sortCenter;
    item.vertPath = &vert;
    item.fragPath = &frag;
    item.materialPath = &kEmpty;
    item.albedoTexturePath = &kEmpty;
    item.overlayTexturePath = &kEmpty;
    item.normalMapPath = &kEmpty;
    item.metallicRoughnessMapPath = &kEmpty;
    item.metallicMapPath = &kEmpty;
    item.roughnessMapPath = &kEmpty;
    item.opacityMapPath = &kEmpty;
    item.emissiveMapPath = &kEmpty;
    item.occlusionMapPath = &kEmpty;
    item.heightMapPath = &kEmpty;
    item.material = MaterialProperties{};
    item.material.pbrEnabled = false;
    // Slightly below 1 keeps the blended path from running its alpha-cutout depth pre-pass.
    item.material.alpha = blended ? 0.99f : 1.0f;
    item.material.alphaCutoff = 0.0f;
    item.material.color = glm::vec3(1.0f);
    item.doubleSided = true;
    item.requiresBlending = blended;
    item.sortOpaque = !blended;
    item.reflectionResolved = true;
    const glm::vec3 viewSpaceCenter = glm::vec3(view * glm::vec4(item.depthSortCenter, 1.0f));
    item.cameraDepth = -viewSpaceCenter.z;
    const glm::vec3 toCamera = item.depthSortCenter - cameraPosition;
    item.cameraDistanceSq = glm::dot(toCamera, toCamera);
}

namespace {

// Cells and vertices of a ring for the current terrain settings.
RingDraw PlanRing(const TerrainComponent& t, int ringIndex) {
    const GrassRing& ring = kRings[ringIndex];
    const float distance = std::max(5.0f, t.grassDistance);
    const float density = std::max(t.grassDensityScale, 0.0f);
    RingDraw plan;
    const float innerRadius = ring.inner * distance;
    const float outerRadius = ringIndex == 2 ? distance : ring.outer * distance;
    const float innerFade = ringIndex == 0 ? 0.0f : RingFade(innerRadius);
    const float outerFade = ringIndex == 2 ? 0.0f : RingFade(outerRadius);
    // Blade budget: blades per cell scale with the multiplier (unbounded; cost grows linearly with it).
    const float wanted = static_cast<float>(ring.baseBlades) * density;
    plan.blades = std::max(1, static_cast<int>(std::ceil(wanted - 1e-4f)));
    plan.keepScale = std::min(1.0f, wanted / static_cast<float>(plan.blades));

    const float reach = ringIndex == 2 ? distance + ring.cellSize : outerRadius + outerFade + ring.cellSize;
    const int n = std::max(1, static_cast<int>(std::ceil(reach / ring.cellSize)));
    int m = 0;
    if (ringIndex > 0) m = std::clamp(static_cast<int>(std::floor((innerRadius - innerFade) / ring.cellSize)) - 1, 0, n - 1);
    plan.instances = (2 * n) * (2 * n) - (2 * m) * (2 * m);
    const int vertsPerBlade = 3 * (2 * ring.segments - 1);
    plan.vertices = plan.blades * vertsPerBlade;
    plan.cells = glm::vec4(static_cast<float>(n), static_cast<float>(m), 0.0f, static_cast<float>(ringIndex));
    plan.range = glm::vec4(ringIndex == 0 ? 0.0f : innerRadius, ringIndex == 2 ? 0.0f : outerRadius, innerFade, outerFade);
    plan.ring = glm::vec4(ring.cellSize, static_cast<float>(plan.blades), static_cast<float>(ring.segments), ring.widthScale);
    return plan;
}

} // namespace

void Renderer::TerrainState::UploadHeights(Entry& e, const TerrainComponent& t) {
    const int res = t.resolution;
    const bool recreate = e.heightTexture == 0 || e.heightTextureResolution != res || e.heightTextureBuffer != t.heights.get() ||
                          e.heightTextureRevision == 0;
    if (e.heightTexture == 0) glGenTextures(1, &e.heightTexture);
    if (!recreate && e.heightTextureRevision == t.heightRevision) return;
    glBindTexture(GL_TEXTURE_2D, e.heightTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const bool partial = !recreate && !t.heightDirty.empty() && e.heightTextureRevision >= t.heightDirtyBase &&
                         e.heightTextureRevision <= t.heightRevision;
    if (partial) {
        const int x0 = std::clamp(t.heightDirty.minX, 0, res - 1);
        const int x1 = std::clamp(t.heightDirty.maxX, 0, res - 1);
        const int z0 = std::clamp(t.heightDirty.minZ, 0, res - 1);
        const int z1 = std::clamp(t.heightDirty.maxZ, 0, res - 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, res);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, x0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, z0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x0, z0, x1 - x0 + 1, z1 - z0 + 1, GL_RED, GL_FLOAT, t.heights->data());
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, res, res, 0, GL_RED, GL_FLOAT, t.heights->data());
        // Read with texelFetch; the filter state only has to be valid.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    {
        // Vertical bounds of the whole terrain, for the grass tile culling.
        float lo = 1.0f, hi = 0.0f;
        for (float h : *t.heights) {
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
        if (t.heights->empty()) lo = hi = 0.0f;
        e.heightMinY = t.heightOffset + lo * t.heightScale;
        e.heightMaxY = t.heightOffset + hi * t.heightScale;
    }
    e.heightTextureResolution = res;
    e.heightTextureBuffer = t.heights.get();
    e.heightTextureRevision = t.heightRevision;
}

void Renderer::TerrainState::prepareVegetation(const std::vector<SceneObject>& objects, const std::vector<Batch>& batches) {
    for (const Batch& batch : batches) {
        const SceneObject& object = objects[static_cast<size_t>(batch.objectIndex)];
        const TerrainComponent& t = object.terrain;
        if (!WantsGrass(t) && !WantsWater(t)) continue;
        auto found = entries.find(batch.entryId);
        if (found == entries.end()) continue;
        Entry& e = found->second;
        UploadHeights(e, t);

        // Painted grass density (255 = full). Absent = a 1x1 white texture.
        const int sres = t.splatResolution;
        const std::vector<uint8_t>* painted = t.grassDensity.get();
        const bool usable = painted && painted->size() >= static_cast<size_t>(sres) * static_cast<size_t>(sres);
        const int wantRes = usable ? sres : 1;
        const void* wantBuffer = usable ? static_cast<const void*>(painted) : nullptr;
        const bool recreate = e.densityTexture == 0 || e.densityTextureResolution != wantRes || e.densityTextureBuffer != wantBuffer;
        if (!recreate && (!usable || e.densityTextureRevision == t.splatRevision)) continue;
        if (e.densityTexture == 0) glGenTextures(1, &e.densityTexture);
        glBindTexture(GL_TEXTURE_2D, e.densityTexture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (usable) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, sres, sres, 0, GL_RED, GL_UNSIGNED_BYTE, painted->data());
        } else {
            const uint8_t white = 255;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, &white);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, 0);
        e.densityTextureResolution = wantRes;
        e.densityTextureBuffer = wantBuffer;
        e.densityTextureRevision = t.splatRevision;
    }
}

size_t Renderer::TerrainState::appendVegetationItems(const std::vector<SceneObject>& objects, const glm::mat4& view,
                                                     const glm::vec3& cameraPosition, bool allowGrass,
                                                     std::vector<SceneRenderItem>& items) {
    size_t appended = 0;
    const size_t oldSize = items.size();
    size_t grassItems = 0;
    for (size_t batchIndex = 0; batchIndex < viewBatches.size(); ++batchIndex) {
        const Batch& batch = viewBatches[batchIndex];
        const SceneObject& obj = objects[static_cast<size_t>(batch.objectIndex)];
        auto found = entries.find(batch.entryId);
        if (found == entries.end() || found->second.heightTexture == 0) continue;
        if (allowGrass && WantsGrass(obj.terrain)) {
            items.emplace_back();
            SceneRenderItem& item = items.back();
            InitLeaderItem(item, obj, viewPool[batch.first], batch.model, view, cameraPosition, kGrassVert, kGrassFrag, false);
            item.grassBatch = static_cast<int>(batchIndex);
            ++appended;
            ++grassItems;
        }
    }
    if (grassItems > 0) {
        // Opaque leaders stay with the opaque items (the lazy skybox still draws before the first
        // transparent one).
        const auto firstTransparent = std::find_if(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(oldSize),
            [](const SceneRenderItem& it) { return !it.sortOpaque; });
        std::rotate(firstTransparent, items.begin() + static_cast<std::ptrdiff_t>(oldSize), items.end());
    }
    for (size_t batchIndex = 0; batchIndex < viewBatches.size(); ++batchIndex) {
        const Batch& batch = viewBatches[batchIndex];
        const SceneObject& obj = objects[static_cast<size_t>(batch.objectIndex)];
        auto found = entries.find(batch.entryId);
        if (found == entries.end() || found->second.heightTexture == 0) continue;
        if (WantsWater(obj.terrain)) {
            items.emplace_back();
            SceneRenderItem& item = items.back();
            InitLeaderItem(item, obj, viewPool[batch.first], batch.model, view, cameraPosition, kWaterVert, kWaterFrag, true);
            item.waterBatch = static_cast<int>(batchIndex);
            ++appended;
        }
    }
    return appended;
}

void Renderer::TerrainState::bindVegetation(Renderer& renderer, const Batch& batch, const SceneObject& terrainObject,
                                            bool grass, Shader* shader, const glm::vec3& cameraPosition,
                                            const glm::mat4& projection, int viewportHeight) {
    const TerrainComponent& t = terrainObject.terrain;
    auto found = entries.find(batch.entryId);
    if (found == entries.end() || !shader) return;
    const Entry& e = found->second;

    glActiveTexture(GL_TEXTURE0 + kHeightUnit);
    glBindTexture(GL_TEXTURE_2D, e.heightTexture);
    glActiveTexture(GL_TEXTURE0 + kDensityUnit);
    glBindTexture(GL_TEXTURE_2D, e.densityTexture);
    glActiveTexture(GL_TEXTURE0);
    shader->setInt("grassHeightTex", kHeightUnit);
    shader->setInt("grassDensityTex", kDensityUnit);
    shader->setInt("grassSplat", 0);
    shader->setVec4("grassTerrain", glm::vec4(t.size, static_cast<float>(t.resolution), t.heightOffset, t.heightScale));

    const SceneWindState& wind = renderer.sceneWind;
    if (grass) {
        const glm::mat4 inverse = glm::inverse(batch.model);
        const glm::vec3 local = glm::vec3(inverse * glm::vec4(cameraPosition, 1.0f));
        shader->setVec3("grassCamera", local);
        const float pixelWorld = 2.0f / (std::max(0.001f, std::abs(projection[1][1])) * static_cast<float>(std::max(viewportHeight, 1)));
        shader->setVec4("grassBlade", glm::vec4(t.grassBladeHeight, t.grassBladeWidth, t.grassHeightVariation, t.grassBend));
        shader->setVec4("grassMisc", glm::vec4(1.0f, t.grassDryAmount, t.grassWindStrength, pixelWorld));
        shader->setFloat("grassMaxDist", std::max(5.0f, t.grassDistance));
        shader->setVec3("grassBaseColor", t.grassBaseColor);
        shader->setVec3("grassTipColor", t.grassTipColor);
        shader->setVec3("grassDryColor", t.grassDryColor);
        shader->setVec4("sceneWind", glm::vec4(wind.direction, wind.strength));
        shader->setFloat("sceneWindTurbulence", wind.turbulence);
        shader->setFloat("sceneTime", wind.time);
        int pondCount = 0;
        for (const TerrainPond& pond : t.ponds) {
            if (pondCount >= kMaxPonds) break;
            static const char* const kNames[kMaxPonds] = {"grassPonds[0]", "grassPonds[1]", "grassPonds[2]", "grassPonds[3]",
                                                         "grassPonds[4]", "grassPonds[5]", "grassPonds[6]", "grassPonds[7]"};
            shader->setVec4(kNames[pondCount], glm::vec4(pond.center.x, pond.center.y, pond.radius, pond.waterHeight));
            ++pondCount;
        }
        shader->setInt("grassPondCount", pondCount);
    } else {
        shader->setMat4("waterInverse", glm::inverse(batch.model));
        shader->setVec3("waterShallow", t.waterShallowColor);
        shader->setVec3("waterDeep", t.waterDeepColor);
        shader->setVec4("waterParams", glm::vec4(t.waterClarity, t.waterWaveScale, t.waterWaveStrength, t.waterReflection));
        shader->setVec4("waterParams2", glm::vec4(t.waterFoam, std::fmod(wind.time, 4000.0f), 0.0f, 0.0f));
        shader->setFloat("waterExtent", 1.3f);
        int count = 0;
        for (const TerrainPond& pond : t.ponds) {
            if (count >= kMaxWaterPonds) break;
            static const char* const kNames[kMaxWaterPonds] = {
                "waterPonds[0]", "waterPonds[1]", "waterPonds[2]", "waterPonds[3]", "waterPonds[4]", "waterPonds[5]",
                "waterPonds[6]", "waterPonds[7]", "waterPonds[8]", "waterPonds[9]", "waterPonds[10]", "waterPonds[11]",
                "waterPonds[12]", "waterPonds[13]", "waterPonds[14]", "waterPonds[15]"};
            shader->setVec4(kNames[count], glm::vec4(pond.center.x, pond.center.y, pond.radius, pond.waterHeight));
            ++count;
        }
    }
}

void Renderer::TerrainState::drawVegetation(const Batch& batch, const SceneObject& terrainObject, bool grass,
                                            Shader* shader, const glm::vec3& cameraPosition, long long& triangles) {
    if (!shader) return;
    const TerrainComponent& t = terrainObject.terrain;
    if (vegetationVao == 0) glGenVertexArrays(1, &vegetationVao);
    if (vegetationVao == 0) return;
    glBindVertexArray(vegetationVao);
    if (grass) {
        MODU_PROFILE_SCOPE("Grass", ProfilerSampleCategory::RenderDetail);
#if !MODULARITY_OPENGL_ES
        // MODU_GRASS_GPU_TIMING=1 prints the average GPU time of the grass draws every 240 frames
        // (verification aid; the queries are read four frames late, so nothing stalls).
        static const bool timing = std::getenv("MODU_GRASS_GPU_TIMING") != nullptr;
        static GLuint queries[4] = {0, 0, 0, 0};
        static bool issued[4] = {false, false, false, false};
        static unsigned int frame = 0;
        static double accumulatedMs = 0.0;
        static int samples = 0;
        const unsigned int slot = frame++ & 3u;
        if (timing) {
            if (queries[0] == 0) glGenQueries(4, queries);
            if (issued[slot]) {
                GLuint64 ns = 0;
                glGetQueryObjectui64v(queries[slot], GL_QUERY_RESULT, &ns);
                accumulatedMs += static_cast<double>(ns) * 1e-6;
                if (++samples >= 240) {
                    std::fprintf(stderr, "GRASS_GPU_MS avg=%.3f over %d draws\n", accumulatedMs / samples, samples);
                    accumulatedMs = 0.0;
                    samples = 0;
                }
            }
            glBeginQuery(GL_TIME_ELAPSED, queries[slot]);
            issued[slot] = true;
        }
#endif
        // Every ring is a square grid of cells around the camera, drawn as runs of visible 3 m tiles: the
        // vertex shader costs the same whether or not a blade ends up on screen, so what the camera cannot
        // see (outside the frustum, beyond the ring's circle, inside the inner ring's hole) never launches.
        float minY = -1.0e4f, maxY = 1.0e4f;
        if (auto found = entries.find(batch.entryId); found != entries.end()) {
            minY = found->second.heightMinY - 1.0f;
            maxY = found->second.heightMaxY + t.grassBladeHeight * (1.0f + t.grassHeightVariation) * 1.7f + 1.0f;
        }
        const glm::mat4 inverse = glm::inverse(batch.model);
        const glm::vec3 camLocal3 = glm::vec3(inverse * glm::vec4(cameraPosition, 1.0f));
        const glm::vec2 camLocal(camLocal3.x, camLocal3.z);
        const float distance = std::max(5.0f, t.grassDistance);
        for (int r = 0; r < 3; ++r) {
            const RingDraw plan = PlanRing(t, r);
            shader->setVec4("grassCells", plan.cells);
            shader->setVec4("grassRange", plan.range);
            shader->setVec4("grassRing", plan.ring);
            shader->setFloat("grassKeepScale", plan.keepScale);

            const float cell = kRings[r].cellSize;
            const int n = static_cast<int>(plan.cells.x + 0.5f);
            const int m = static_cast<int>(plan.cells.y + 0.5f);
            const int cellsPerSide = 2 * n;
            const int tileCells = std::clamp(static_cast<int>(std::lround(3.0f / cell)), 2, 16);
            const int tilesPerSide = (cellsPerSide + tileCells - 1) / tileCells;
            const glm::ivec2 camCell(static_cast<int>(std::floor(camLocal.x / cell)), static_cast<int>(std::floor(camLocal.y / cell)));
            const float margin = 0.8f + cell;
            const float outerReach = (plan.range.y > 0.0f ? plan.range.y + plan.range.w : distance) + margin;
            const float innerReach = plan.range.x > 0.0f ? plan.range.x - plan.range.z - margin : -1.0f;
            const int vertsPerBlade = 3 * (2 * kRings[r].segments - 1);
            (void)vertsPerBlade;

            const auto tileVisible = [&](int tx, int tz) {
                const int cx0 = tx * tileCells, cz0 = tz * tileCells;
                const int cx1 = std::min(cx0 + tileCells, cellsPerSide), cz1 = std::min(cz0 + tileCells, cellsPerSide);
                // Entirely inside the hole the inner ring owns.
                if (m > 0 && cx0 >= n - m && cx1 <= n + m && cz0 >= n - m && cz1 <= n + m) return false;
                const float x0 = static_cast<float>(camCell.x + cx0 - n) * cell;
                const float z0 = static_cast<float>(camCell.y + cz0 - n) * cell;
                const float x1 = static_cast<float>(camCell.x + cx1 - n) * cell;
                const float z1 = static_cast<float>(camCell.y + cz1 - n) * cell;
                const float nearestX = std::max({x0 - camLocal.x, 0.0f, camLocal.x - x1});
                const float nearestZ = std::max({z0 - camLocal.y, 0.0f, camLocal.y - z1});
                if (nearestX * nearestX + nearestZ * nearestZ > outerReach * outerReach) return false;
                if (innerReach > 0.0f) {
                    const float farX = std::max(std::abs(x0 - camLocal.x), std::abs(x1 - camLocal.x));
                    const float farZ = std::max(std::abs(z0 - camLocal.y), std::abs(z1 - camLocal.y));
                    if (farX * farX + farZ * farZ < innerReach * innerReach) return false;
                }
                if (!haveViewPlanes) return true;
                glm::vec3 lo(1.0e30f), hi(-1.0e30f);
                for (int corner = 0; corner < 8; ++corner) {
                    const glm::vec3 local((corner & 1) ? x1 + margin : x0 - margin, (corner & 2) ? maxY : minY,
                                          (corner & 4) ? z1 + margin : z0 - margin);
                    const glm::vec3 world = glm::vec3(batch.model * glm::vec4(local, 1.0f));
                    lo = glm::min(lo, world);
                    hi = glm::max(hi, world);
                }
                return RenderOpt::AabbInsidePlanes(viewPlanes, 0.5f * (lo + hi), 0.5f * (hi - lo));
            };

            for (int tz = 0; tz < tilesPerSide; ++tz) {
                const int rows = std::min(tileCells, cellsPerSide - tz * tileCells);
                int runStart = -1;
                for (int tx = 0; tx <= tilesPerSide; ++tx) {
                    const bool visible = tx < tilesPerSide && tileVisible(tx, tz);
                    if (visible && runStart < 0) runStart = tx;
                    if (!visible && runStart >= 0) {
                        const int firstCell = runStart * tileCells;
                        const int width = std::min(tx * tileCells, cellsPerSide) - firstCell;
                        shader->setVec4("grassRect", glm::vec4(static_cast<float>(firstCell), static_cast<float>(tz * tileCells),
                                                               static_cast<float>(width), 0.0f));
                        glDrawArraysInstanced(GL_TRIANGLES, 0, plan.vertices, width * rows);
                        triangles += static_cast<long long>(width) * rows * plan.blades * (2 * kRings[r].segments - 1);
                        runStart = -1;
                    }
                }
            }
        }
#if !MODULARITY_OPENGL_ES
        if (timing) glEndQuery(GL_TIME_ELAPSED);
#endif
    } else {
        const int count = std::min(static_cast<int>(t.ponds.size()), kMaxWaterPonds);
        if (count > 0) {
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, count);
            triangles += static_cast<long long>(count) * 2;
        }
    }
    glBindVertexArray(0);
}
