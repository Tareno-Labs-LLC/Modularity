#pragma once
// Internal to the renderer: the Terrain component's GPU state (RenderTerrain.cpp), shared with
// Rendering.cpp, which owns the main pass and the shadow passes that consume it.
#include "Rendering.h"

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

struct Renderer::TerrainState {
    static constexpr int kLodCount = 3;

    struct Chunk {
        std::unique_ptr<Mesh> mesh[kLodCount];
        bool stale[kLodCount] = {false, false, false};
        float minY = 0.0f;   // Terrain-local metres, over the chunk's full-resolution samples.
        float maxY = 0.0f;
    };

    // GPU + cache state of one Terrain object (keyed by object id).
    struct Entry {
        uint64_t lastUse = 0;
        int resolution = 0;
        int chunksPerSide = 0;
        float size = 0.0f;
        float heightScale = 0.0f;
        float heightOffset = 0.0f;
        const void* heightsBuffer = nullptr;
        uint64_t heightRevision = 0;
        uint64_t splatRevision = 0;
        std::vector<Chunk> chunks;
        GLuint splatTexture = 0;
        int splatTextureResolution = 0;
        uint64_t splatUploadedRevision = 0;
        const void* splatBuffer = nullptr;
        GLuint albedoArray = 0;
        GLuint normalArray = 0;
        uint64_t layerSignature = 0;
        bool layersFailed = false;
        // Grass / water inputs (RenderGrass.cpp): heights as R32F, painted grass density as R8.
        GLuint heightTexture = 0;
        int heightTextureResolution = 0;
        const void* heightTextureBuffer = nullptr;
        uint64_t heightTextureRevision = 0;
        GLuint densityTexture = 0;
        int densityTextureResolution = 0;
        const void* densityTextureBuffer = nullptr;
        uint64_t densityTextureRevision = 0;
        float heightMinY = 0.0f;   // terrain-local metres over all samples (grass tile culling)
        float heightMaxY = 0.0f;
    };

    // The chunk meshes one terrain contributes to one view / shadow pass.
    struct Batch {
        int objectIndex = -1;
        int entryId = -1;
        glm::mat4 model = glm::mat4(1.0f);
        size_t first = 0;
        size_t count = 0;
        uint64_t signature = 0;
        long long triangles = 0;
    };

    // What the terrain fragment stage needs bound.
    struct Surface {
        GLuint splat = 0;
        GLuint albedoArray = 0;
        GLuint normalArray = 0;
        glm::vec4 tiling = glm::vec4(4.0f);
        glm::vec4 normalStrength = glm::vec4(1.0f);
        glm::vec3 tint[4] = {glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f)};
        float size = 256.0f;
        glm::vec2 grassShade = glm::vec2(0.0f);   // x grass distance (0 = off), y ground darkening under it
    };

    struct ShadowQuery {
        bool sphere = false;               // point-light cube faces: also reject chunks outside the light range
        glm::vec3 sphereCenter = glm::vec3(0.0f);
        float sphereRadius = 0.0f;
    };

    std::unordered_map<int, Entry> entries;
    std::vector<Mesh*> viewPool;
    std::vector<Batch> viewBatches;
    std::vector<Mesh*> shadowPool;
    std::vector<Batch> shadowBatches;
    uint64_t epoch = 0;
    int buildBudget = 0;

    // Culls and LOD-selects every active terrain for one view (or one shadow frustum). Returns the
    // batch count. `signature` (shadow passes) receives a hash of exactly what will be drawn.
    size_t gather(Renderer& renderer, const std::vector<SceneObject>& objects, const glm::vec3& cameraPosition,
                  const std::array<glm::vec4, 6>& planes, uint32_t cullingMask, const ShadowQuery* shadow,
                  std::vector<Mesh*>& pool, std::vector<Batch>& batches, uint64_t* signature);
    // Uploads the splat map / layer arrays of every terrain in `batches` (main pass; call before the
    // pass starts tracking texture bindings).
    void prepareSurfaces(Renderer& renderer, const std::vector<SceneObject>& objects, const std::vector<Batch>& batches);
    // One leader item per batch, inserted with the opaque items.
    size_t appendItems(Renderer& renderer, const std::vector<SceneObject>& objects, const glm::mat4& view,
                       const glm::vec3& cameraPosition, std::vector<SceneRenderItem>& items);
    Surface surface(const SceneObject& object) const;
    void release();

    // ---- Grass and pond water (RenderGrass.cpp) --------------------------------------------------
    std::array<glm::vec4, 6> viewPlanes{};   // the main pass frustum (set by the caller of gather) for grass tile culling
    bool haveViewPlanes = false;
    GLuint vegetationVao = 0;    // attribute-less draws need a VAO bound in core profiles
    static void InitLeaderItem(SceneRenderItem& item, const SceneObject& obj, Mesh* mesh, const glm::mat4& model,
                               const glm::mat4& view, const glm::vec3& cameraPosition, const std::string& vert,
                               const std::string& frag, bool blended);
    static void UploadHeights(Entry& entry, const TerrainComponent& terrain);
    // Uploads the height / grass-density textures of every terrain in `batches` that grows grass or
    // holds water (main pass; before the pass starts tracking texture bindings).
    void prepareVegetation(const std::vector<SceneObject>& objects, const std::vector<Batch>& batches);
    // Leader items for the grass (opaque, after the terrain) and the pond water (blended, last).
    // `allowGrass` is false for reflection captures.
    size_t appendVegetationItems(const std::vector<SceneObject>& objects, const glm::mat4& view,
                                 const glm::vec3& cameraPosition, bool allowGrass,
                                 std::vector<SceneRenderItem>& items);
    // Per-draw uniforms + the height / density textures (units 20 and 21) for a grass or water item.
    void bindVegetation(Renderer& renderer, const Batch& batch, const SceneObject& terrainObject, bool grass,
                        Shader* shader, const glm::vec3& cameraPosition, const glm::mat4& projection, int viewportHeight);
    // The draw calls. `triangles` accumulates what was submitted.
    void drawVegetation(const Batch& batch, const SceneObject& terrainObject, bool grass, Shader* shader,
                        const glm::vec3& cameraPosition, long long& triangles);
};
