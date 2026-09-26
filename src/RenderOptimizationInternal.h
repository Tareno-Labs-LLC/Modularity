#pragma once

// Internal to the renderer - included by Rendering.cpp and RenderOptimization.cpp only.
// The state behind Project Settings > Graphics Manager > Optimization.
//
// Rendering.cpp owns the scene-aware half (per-object caches, culling, the render queue,
// batch membership) because it needs that file's scene helpers. RenderOptimization.cpp
// owns the GL half (mesh arena, indirect buffers, compute culling, Hi-Z, async readback)
// and the Automatic strategy.

#include "Rendering.h"

#include <array>
#include <climits>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Camera;

namespace RenderOpt {

// The paths a draw reads, in the order CachedRenderable stores them.
enum CachedPath : int {
    kPathVert = 0,
    kPathFrag,
    kPathMaterial,
    kPathAlbedo,
    kPathOverlay,
    kPathNormal,
    kPathMetallicRoughness,
    kPathMetallic,
    kPathRoughness,
    kPathOpacity,
    kPathEmissive,
    kPathOcclusion,
    kPathHeight,
    kPathCount
};

// Frame-to-frame record for one scene object, indexed like the scene vector and
// validated by object id. Transform inputs are compared on every pass and material
// inputs once per frame; derived data is rebuilt only when an input changed. There is
// no dirty flag on SceneObject to hook (scripts, physics and the editor all write
// position/rotation/scale directly), so "dirty" means "differs from the snapshot".
struct CachedRenderable {
    int objectId = INT_MIN;

    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
    glm::vec3 scale{0.0f};
    glm::vec3 localCenter{0.0f};
    glm::vec3 localExtents{0.0f};
    int meshId = -2;
    RenderType renderType = RenderType::None;
    bool faceCamera = false;
    bool hasLocalBounds = false;
    bool transformValid = false;

    glm::mat4 model{1.0f};
    bool mirrored = false;        // odd number of negative scales: front face flips
    uint32_t transformVersion = 0;

    MaterialProperties material{};
    std::array<std::string, kPathCount> paths;
    bool useOverlay = false;
    bool materialValid = false;
    uint64_t materialCheckedFrame = UINT64_MAX;
    uint32_t materialVersion = 0;
    uint64_t materialKey = 0;     // every material input hashed: batch identity
    uint32_t shaderGroup = 0;     // interned (vert, frag) pair, for sort order
    uint32_t textureGroup = 0;    // interned texture set, for sort order

    // Albedo/overlay alpha blending, valid for (materialVersion, texture generation).
    uint32_t alphaMaterialVersion = 0;
    uint64_t alphaTextureGeneration = UINT64_MAX;
    bool albedoAlphaBlend = false;
    bool overlayAlphaBlend = false;

    // Merged into a static batch, valid for the static merge generation.
    bool staticMergeSource = false;
    uint64_t staticMergeFlagGeneration = UINT64_MAX;
};

// Culling inputs, kept apart from CachedRenderable so the culling loop streams 32 bytes
// per object rather than the whole record.
struct CullBounds {
    glm::vec3 center{0.0f};   // world AABB centre, which is also the cull-sphere centre
    float radius = 0.0f;      // sphere around the oriented box (the legacy cull sphere)
    glm::vec3 extents{0.0f};  // world AABB half extents
    uint32_t hasBounds = 0;
};

// GPU culling writes each pass's commands into its own region of the command buffer,
// rotating, so a pass never rewrites commands an earlier pass may still be drawing.
constexpr int kGpuCommandRegions = 4;
// Past this many batched objects the rest draw one at a time. Keeps every culling buffer
// well inside the 16 MB shader-storage block size GL 4.3 guarantees.
constexpr size_t kMaxMultiDrawSlots = 131072;

struct DrawArraysIndirectCommand {
    GLuint count = 0;
    GLuint instanceCount = 0;
    GLuint first = 0;
    GLuint baseInstance = 0;
};

// Plane-by-plane AABB test (the "p-vertex" form): the box is outside when its most
// positive corner along a plane normal is still behind that plane.
inline bool AabbInsidePlanes(const std::array<glm::vec4, 6>& planes,
                             const glm::vec3& center, const glm::vec3& extents) {
    for (const glm::vec4& plane : planes) {
        const float r = extents.x * std::abs(plane.x) +
                        extents.y * std::abs(plane.y) +
                        extents.z * std::abs(plane.z);
        const float s = plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
        if (s + r < 0.0f) {
            return false;
        }
    }
    return true;
}

inline bool SphereInsidePlanes(const std::array<glm::vec4, 6>& planes,
                               const glm::vec3& center, float radius) {
    for (const glm::vec4& plane : planes) {
        if (plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w < -radius) {
            return false;
        }
    }
    return true;
}

// Projected diameter of a bounding sphere in pixels. Must match cull_instances_comp.glsl
// so the CPU and GPU paths agree on what LOD rejects: a camera inside the sphere keeps it.
inline bool PassesLodScreenSize(const glm::vec4& viewDepthRow, bool perspective,
                                float pxPerUnit, float minScreenPx,
                                const glm::vec3& center, float radius) {
    if (!perspective) {
        return 2.0f * radius * pxPerUnit >= minScreenPx;
    }
    const float depth = viewDepthRow.x * center.x + viewDepthRow.y * center.y +
                        viewDepthRow.z * center.z + viewDepthRow.w;
    if (!(depth > radius)) {
        return true;
    }
    return 2.0f * radius * pxPerUnit / depth >= minScreenPx;
}

// World bounds of an object's local box under `model`: the sphere TryComputeObjectCullSphere
// builds, plus the world AABB of the oriented box (Arvo: |M| applied to the half extents).
inline void ComputeWorldBounds(const glm::mat4& model, const glm::vec3& localCenter,
                               const glm::vec3& localExtents, CullBounds& out) {
    const glm::mat3 axes(model);
    out.center = glm::vec3(model * glm::vec4(localCenter, 1.0f));
    const glm::vec3 axisLengths(glm::length(axes[0]), glm::length(axes[1]), glm::length(axes[2]));
    out.radius = glm::length(localExtents * axisLengths);
    out.extents = glm::vec3(
        std::abs(axes[0][0]) * localExtents.x + std::abs(axes[1][0]) * localExtents.y +
            std::abs(axes[2][0]) * localExtents.z,
        std::abs(axes[0][1]) * localExtents.x + std::abs(axes[1][1]) * localExtents.y +
            std::abs(axes[2][1]) * localExtents.z,
        std::abs(axes[0][2]) * localExtents.x + std::abs(axes[1][2]) * localExtents.y +
            std::abs(axes[2][2]) * localExtents.z);
    const bool finite = std::isfinite(out.radius) && out.radius > 0.0f &&
                        std::isfinite(out.center.x) && std::isfinite(out.center.y) &&
                        std::isfinite(out.center.z) && std::isfinite(out.extents.x) &&
                        std::isfinite(out.extents.y) && std::isfinite(out.extents.z);
    out.hasBounds = finite ? 1u : 0u;
}

// Stable LSD radix sort on a 64-bit `key` member, 8 bits a pass. Ties keep their input
// order, so entries gathered in scene order come out exactly as a stable comparison sort
// on the key would leave them. Bytes every key shares (usually the high ones) are
// skipped, which makes it a few linear passes over the list.
template <typename Entry>
void RadixSortByKey(std::vector<Entry>& entries, std::vector<Entry>& scratch) {
    const size_t count = entries.size();
    if (count < 2) {
        return;
    }
    scratch.resize(count);
    Entry* source = entries.data();
    Entry* target = scratch.data();
    for (int shift = 0; shift < 64; shift += 8) {
        size_t buckets[256] = {};
        for (size_t i = 0; i < count; ++i) {
            ++buckets[(source[i].key >> shift) & 0xFFu];
        }
        if (buckets[(source[0].key >> shift) & 0xFFu] == count) {
            continue;
        }
        size_t offset = 0;
        for (size_t& bucket : buckets) {
            const size_t size = bucket;
            bucket = offset;
            offset += size;
        }
        for (size_t i = 0; i < count; ++i) {
            target[buckets[(source[i].key >> shift) & 0xFFu]++] = source[i];
        }
        std::swap(source, target);
    }
    if (source != entries.data()) {
        std::copy(source, source + count, entries.data());
    }
}

} // namespace RenderOpt

struct Renderer::OptimizationState {
    // ----- What one pass runs. Manual copies the settings; Automatic narrows them.
    struct PassPlan {
        bool active = false;
        bool cpuCulling = false;
        bool staticCache = false;
        bool sorting = false;
        bool lod = false;
        bool multiDraw = false;
        bool gpuCulling = false;
        bool hiZ = false;
        bool automatic = false;
    };

    struct OpaqueEntry {
        uint64_t key = 0;
        uint32_t ref = 0;      // object index, or a tagged batch index (see kRef*)
        uint32_t order = 0;    // stable tie-break: gather order
    };
    struct TransparentEntry {
        float cameraDepth = 0.0f;
        float cameraDistanceSq = 0.0f;
        int objectId = -1;
        uint32_t ref = 0;
        int uiSortingOrder = 0;
        bool isUiCanvas3D = false;
        uint32_t order = 0;
    };
    static constexpr uint32_t kRefStaticBatch = 0x80000000u;
    static constexpr uint32_t kRefMultiDraw = 0x40000000u;
    static constexpr uint32_t kRefIndexMask = 0x3FFFFFFFu;

    struct ShaderSlot {
        const std::string* vert = nullptr;
        const std::string* frag = nullptr;
        Shader* shader = nullptr;
    };
    struct ProbeRef {
        glm::vec3 position{0.0f};
        float range = 0.0f;
        float intensity = 0.0f;
        unsigned int cube = 0;
    };

    // Per renderSceneInternal call. Indexed by recursion depth (a reflection capture
    // renders from inside the outer pass), and kept so its buffers are reused.
    struct Pass {
        PassPlan plan;
        uint64_t frameSerial = 0;
        glm::mat4 view{1.0f};
        glm::mat4 proj{1.0f};
        std::array<glm::vec4, 6> planes{};
        glm::vec3 cameraPosition{0.0f};
        glm::vec3 cameraForward{0.0f, 0.0f, -1.0f};
        glm::vec4 viewDepthRow{0.0f};
        float lodPxPerUnit = 0.0f;
        bool perspective = true;
        float farPlane = 1000.0f;
        int width = 0;
        int height = 0;
        uint32_t cullingMask = 0xFFFFFFFFu;
        bool drawMirrorObjects = true;
        bool reflectionCapture = false;
        unsigned int depthTexture = 0;   // sampleable scene depth, 0 when not published

        std::vector<uint32_t> candidates;          // renderable in this view, not batched
        std::vector<OpaqueEntry> opaque;
        std::vector<TransparentEntry> transparent;
        std::vector<OpaqueEntry> opaqueScratch;     // radix sort ping-pong buffer
        std::vector<uint32_t> canvasPositions;
        std::vector<TransparentEntry> canvasEntries;
        std::vector<SceneRenderItem> items;        // the draw list renderSceneInternal walks
        std::vector<ShaderSlot> shaders;
        std::vector<ProbeRef> probes;
        std::vector<std::pair<uint32_t, Mesh*>> shadowCasters;
        bool shadowCastersBuilt = false;

        int gpuCommandRegion = -1;                 // >= 0 once GPU culling was dispatched
        bool multiDrawUsed = false;
        float multiDrawCpuCullMs = 0.0f;           // CPU culling of batched objects only
    };

    // ----- Multi-draw indirect. Scene-level: membership is rebuilt only when the set of
    // eligible objects or their batch keys change, and object matrices upload only when
    // an object moved. Per pass, CPU culling rewrites the visible commands, or the GPU
    // culling shader writes them.
    struct MultiDrawBatch {
        uint64_t key = 0;
        uint32_t firstSlot = 0;
        uint32_t slotCount = 0;
        uint32_t representative = 0;  // object index whose material the batch draws with
        uint32_t visibleCount = 0;    // CPU culling: commands written this pass
        uint32_t shaderGroup = 0;
        long long triangles = 0;      // all members
    };
    // One mesh copied into the shared arena. Both the main batches and the shadow batches
    // draw from it; an entry neither uses any more is dead space until compaction.
    struct ArenaMesh {
        uint64_t serial = 0;
        GLint first = 0;
        GLsizei count = 0;
        bool usedByMain = false;
        bool usedByShadows = false;
    };
    struct MultiDrawState {
        GLuint vao = 0;
        GLuint arenaBuffer = 0;
        GLuint instanceBuffer = 0;     // one mat4 per slot, instanced attributes 5..8
        GLuint templateBuffer = 0;     // GPU culling: count/first/layer per slot
        GLuint boundsBuffer = 0;       // GPU culling: two vec4 per slot
        GLuint commandBuffer = 0;      // GPU culling: indirect commands, one region per pass
        GLuint cpuCommandBuffer = 0;   // CPU culling: the pass's packed visible commands
        GLuint counterBuffer = 0;      // GPU culling: visible count
        GLsizeiptr arenaCapacityVerts = 0;
        GLsizeiptr arenaUsedVerts = 0;
        GLsizeiptr arenaDeadVerts = 0;
        std::vector<ArenaMesh> arenaMeshes;
        std::unordered_map<uint64_t, uint32_t> arenaLookup;   // mesh serial -> arenaMeshes
        uint64_t arenaGeneration = 0;        // bumped when compaction moves every mesh
        uint64_t builtArenaGeneration = UINT64_MAX;
        size_t instanceCapacity = 0;
        size_t commandCapacity = 0;    // commands per region
        int commandRegions = 0;

        bool membershipValid = false;
        uint64_t membershipSignature = 0;
        uint64_t membershipFrame = UINT64_MAX;
        std::vector<MultiDrawBatch> batches;
        std::vector<uint32_t> slotObject;           // slot -> object index
        std::vector<uint32_t> slotVersion;          // transform version uploaded per slot
        std::vector<glm::mat4> slotMatrices;
        std::vector<glm::vec4> slotBounds;          // 2 per slot
        std::vector<glm::uvec4> slotTemplates;
        std::vector<GLint> slotFirst;               // arena first vertex per slot
        std::vector<GLsizei> slotCount;             // vertex count per slot
        std::vector<uint32_t> objectSlot;           // object index -> slot, or UINT32_MAX
        std::vector<std::pair<uint64_t, uint32_t>> eligible;  // (batch key, object index)
        std::vector<RenderOpt::DrawArraysIndirectCommand> passCommands;
        bool templatesDirty = true;
        int nextRegion = 0;
    };

    // ----- Shadow casters batched per light (and per cube face). The depth passes ignore
    // materials, so every static caster whose mesh is in the arena can share one draw.
    struct ShadowBatchState {
        GLuint vao = 0;
        GLuint instanceBuffer = 0;
        GLuint commandBuffer = 0;
        size_t instanceCapacity = 0;
        size_t commandCapacity = 0;
        bool membershipValid = false;
        uint64_t membershipSignature = 0;
        uint64_t membershipFrame = UINT64_MAX;
        uint64_t builtArenaGeneration = UINT64_MAX;
        std::vector<uint32_t> slotObject;           // slot -> object index
        std::vector<GLint> slotFirst;
        std::vector<GLsizei> slotCount;
        std::vector<uint32_t> slotVersion;
        std::vector<glm::mat4> slotMatrices;
        std::vector<uint32_t> objectSlot;           // object index -> slot, or UINT32_MAX
        std::vector<RenderOpt::DrawArraysIndirectCommand> commands;
    };

    // ----- Hi-Z (experimental): one pyramid per published depth texture.
    struct HiZPyramid {
        unsigned int depthTexture = 0;
        GLuint texture = 0;
        int depthWidth = 0;
        int depthHeight = 0;
        int width = 0;
        int height = 0;
        int levels = 0;
        glm::mat4 viewProj{1.0f};
        glm::vec3 cameraPosition{0.0f};
        glm::vec3 cameraForward{0.0f};
        bool valid = false;
        uint64_t builtFrame = 0;
    };

    // ----- Async GPU culling results. Polled, never waited on.
    struct GpuReadback {
        GLuint buffer = 0;
        GLuint queries[2] = {0, 0};
        GLsync fence = nullptr;
        const RenderStats* owner = nullptr;
        bool pending = false;
    };
    struct GpuResult {
        const RenderStats* owner = nullptr;
        float ms = -1.0f;
        int visible = -1;
    };

    // ----- Automatic strategy (RenderOptimization.cpp).
    struct AutoPolicy {
        int tier = 0;                 // 0 legacy, 1 CPU, 2 CPU + multi-draw, 3 GPU + multi-draw
        double tierSince = 0.0;
        double gpuBlockedUntil = 0.0;
        double multiDrawBlockedUntil = 0.0;
        float emaCpuCullMs = -1.0f;   // CPU cost of culling batched objects, per pass
        float emaGpuCullMs = -1.0f;
        float emaMembersPerBatch = -1.0f;
        int lastCandidates = 0;
    };

    // ----- Data.
    std::vector<RenderOpt::CachedRenderable> cache;
    std::vector<RenderOpt::CullBounds> bounds;
    uint32_t versionCounter = 0;
    std::unordered_map<uint64_t, uint32_t> shaderGroups;
    std::unordered_map<uint64_t, uint32_t> textureGroups;

    uint64_t staticMergeGeneration = 0;      // bumped whenever static batches rebuild or clear
    uint64_t staticMergeCheckedFrame = UINT64_MAX;
    uint64_t staticMergeCacheSignature = 0;
    bool staticMergeCacheValid = false;

    std::array<Pass, 4> passes;
    int passDepth = 0;

    RenderOptimizationSupport support;
    bool supportQueried = false;
    // GPU culling results are statistics and Automatic's cost input, so they are sampled,
    // not collected every frame: each poll and fetch is a driver round-trip, which costs
    // a sync with the driver thread under NVIDIA's threaded optimization.
    uint64_t readbackPollFrame = 0;
    uint64_t readbackIssueFrame = 0;

    MultiDrawState multiDraw;
    ShadowBatchState shadowBatch;
    std::vector<HiZPyramid> hiZ;
    std::array<GpuReadback, 6> readbacks;
    std::array<GpuResult, 4> gpuResults;
    AutoPolicy autoPolicy;

    GLuint cullProgram = 0;
    GLuint hiZProgram = 0;
    bool computeProgramsTried = false;
    std::string computeFailure;       // non-empty: GPU culling and Hi-Z fall back to the CPU
    bool multiDrawFailed = false;
    std::string multiDrawFailure;

    // ----- Scene-aware half (Rendering.cpp).
    static Pass* beginPass(Renderer& renderer, const Camera& camera,
                           const std::vector<SceneObject>& sceneObjects,
                           const glm::mat4& view, const glm::mat4& proj,
                           const std::array<glm::vec4, 6>& planes, int width, int height,
                           float farPlane, bool drawMirrorObjects);
    static void buildQueue(Renderer& renderer, Pass& pass,
                           const std::vector<SceneObject>& sceneObjects, RenderStats& passStats);
    static void collectShadowCasters(Renderer& renderer, Pass& pass,
                                     const std::vector<SceneObject>& sceneObjects);
    static void finishPass(Renderer& renderer, Pass& pass, RenderStats& passStats);
    static Mesh* meshFor(Renderer& renderer, const SceneObject& obj);
    void refreshTransforms(const std::vector<SceneObject>& sceneObjects, const Camera& camera,
                           bool forceRecompute);
    void refreshMaterials(const std::vector<SceneObject>& sceneObjects, uint64_t frameSerial,
                          bool forceRecompute);
    bool resolveAlphaBlend(Renderer& renderer, uint32_t objectIndex, const SceneObject& obj);
    void refreshStaticMerge(Renderer& renderer, const std::vector<SceneObject>& sceneObjects,
                            bool useCachedSignature);
    bool isMultiDrawEligible(Renderer& renderer, const Pass& pass, uint32_t objectIndex,
                             const SceneObject& obj, Mesh* mesh);
    void updateMultiDrawMembership(Renderer& renderer, const Pass& pass,
                                   const std::vector<SceneObject>& sceneObjects);
    // Shadow caster batching for this pass. False when casters draw one at a time.
    static bool prepareShadowBatch(Renderer& renderer, Pass& pass,
                                   const std::vector<SceneObject>& sceneObjects);

    // ----- GL half (RenderOptimization.cpp).
    static RenderOptimizationSupport querySupport();
    PassPlan resolvePlan(const RenderOptimizationSettings& settings,
                         const RenderOptimizationSupport& capabilities,
                         int candidateCount, bool reflectionCapture);
    void notePassCosts(const PassPlan& plan, int batchedObjects, int batches,
                       float batchedCpuCullMs, float gpuCullMs);
    bool ensureComputePrograms();
    void beginArenaRegistration(bool forShadows);
    bool ensureArenaMesh(Mesh* mesh, GLint& outFirst, GLsizei& outCount, bool forShadows);
    bool compactArenaIfWasteful();
    bool ensureMultiDrawCapacity(size_t slotCount, int commandRegions);
    void uploadMultiDrawSlots();
    void uploadPassCommands();
    int dispatchGpuCulling(const Pass& pass, const RenderOptimizationSettings& settings,
                           bool useHiZ, const RenderStats* statsOwner);
    void drawMultiDrawBatch(const Pass& pass, const MultiDrawBatch& batch) const;
    bool ensureShadowBatchCapacity(size_t slotCount);
    void uploadShadowSlots();
    void drawShadowBatch(size_t commandCount);
    const HiZPyramid* findUsableHiZ(const Pass& pass) const;
    void buildHiZ(const Pass& pass);
    void pollReadbacks();
    GpuResult latestGpuResult(const RenderStats* owner) const;
    void releaseGpuResources();
};
