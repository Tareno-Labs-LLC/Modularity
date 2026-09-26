#pragma once

#include "Common.h"
#include "SceneObject.h"
#include "RendererResizePolicy.h"
#include "../include/Shaders/Shader.h"
#include "../include/Textures/Texture.h"
#include "../include/Skybox/Skybox.h"
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <limits>

// Cube vertex data (position + normal + texcoord)
extern float vertices[288];
extern float mirrorPlaneVertices[48];

constexpr int kRendererMaxRealtimeLights = 32;

enum class SceneRenderMode { Normal = 0, ShadedWireframe = 1, Wireframe = 2 };

enum class RendererColorPrecision { Auto = 0, SDR8 = 1, HDR16F = 2 };

// Primitive generation functions
std::vector<float> generateSphere(int segments = 32, int rings = 16);
std::vector<float> generateCapsule(int segments = 16, int rings = 8);
std::vector<float> generateCapsuleShape(float radius, float cylinderHalfHeight,
                                        int segments = 16, int rings = 8);
std::vector<float> generateTorus(int segments = 32, int sides = 16);

class Mesh {
private:
    unsigned int VAO, VBO;
    unsigned int boneVBO = 0;
    int vertexCount;
    int strideFloats = 8;
    bool dynamic = false;
    bool hasBones = false;
    // Unique per constructed mesh and never reused, so a cache keyed on a mesh
    // can tell a re-imported mesh from the one it replaced, even at the same address.
    uint64_t serial = 0;

public:
    Mesh(const float* vertexData, size_t dataSizeBytes);
    Mesh(const float* vertexData, size_t dataSizeBytes, bool dynamicUsage,
         const void* boneData, size_t boneDataBytes);
    ~Mesh();

    void draw() const;
    void updateVertices(const float* vertexData, size_t dataSizeBytes);
    int getVertexCount() const { return vertexCount; }
    bool isDynamic() const { return dynamic; }
    bool usesBones() const { return hasBones; }
    unsigned int getVertexBuffer() const { return VBO; }
    uint64_t getSerial() const { return serial; }
};

class OBJLoader {
public:
    struct LoadedMesh {
        struct SubMesh {
            std::unique_ptr<Mesh> mesh;
            int materialIndex = 0;
            int vertexCount = 0;
            int faceCount = 0;
        };

        std::string path;
        std::unique_ptr<Mesh> mesh;
        std::string name;
        int vertexCount = 0;
        int faceCount = 0;
        bool hasNormals = false;
        bool hasTexCoords = false;
        glm::vec3 boundsMin = glm::vec3(FLT_MAX);
        glm::vec3 boundsMax = glm::vec3(-FLT_MAX);
        std::vector<glm::vec3> triangleVertices; // positions duplicated per-triangle for picking
        std::vector<glm::vec3> positions; // unique vertex positions for physics
        std::vector<uint32_t> triangleIndices; // triangle indices into positions
        bool isSkinned = false;
        std::vector<std::string> boneNames;
        std::vector<std::string> boneNodePaths;
        std::vector<glm::mat4> inverseBindMatrices;
        std::vector<glm::ivec4> boneIds;
        std::vector<glm::vec4> boneWeights;
        std::vector<float> baseVertices;
        std::vector<SubMesh> subMeshes;
        std::vector<std::string> materialSlots;
    };

private:
    std::vector<LoadedMesh> loadedMeshes;

public:
    int loadOBJ(const std::string& filepath, std::string& errorMsg);
    Mesh* getMesh(int index);
    const LoadedMesh* getMeshInfo(int index) const;
    const std::vector<LoadedMesh>& getAllMeshes() const { return loadedMeshes; }
    void clear() { loadedMeshes.clear(); }
    size_t getMeshCount() const { return loadedMeshes.size(); }
};

class Camera;

glm::mat4 BuildCameraProjection(const Camera& camera, int width, int height,
                                float fovDeg, float nearPlane, float farPlane);

const std::vector<float>* GetObjectTriangleVertexStream(const SceneObject& obj);

// Project Settings > Graphics Manager > Optimization. Everything defaults to off, and
// with nothing enabled renderSceneInternal takes the path it always took, so a project
// can A/B these against the renderer it shipped with.
enum class RenderOptimizationStrategy { Manual = 0, Automatic = 1 };

struct RenderOptimizationSettings {
    bool cpuFrustumCulling = false;
    bool multiDrawIndirect = false;
    bool gpuDrivenCulling = false;
    bool lodOptimization = false;
    bool hiZOcclusionCulling = false;
    bool materialSorting = false;
    bool staticObjectCaching = false;
    // Manual runs exactly the flags above. Automatic treats them as the allowed set
    // and picks which to run from the scene's size and measured cost.
    RenderOptimizationStrategy strategy = RenderOptimizationStrategy::Manual;
    // LOD: objects whose bounding sphere projects smaller than this are skipped.
    float lodMinScreenSizePx = 1.5f;
};

// What the current GL context can run. A reason is empty when the feature is supported.
struct RenderOptimizationSupport {
    bool multiDrawIndirect = false;
    bool computeCulling = false;
    bool indirectCount = false;
    bool hiZ = false;
    std::string multiDrawIndirectReason;
    std::string computeCullingReason;
    std::string hiZReason;
};

// RenderStats::optimizationFeatures: which optimizations actually ran in a pass.
enum RenderOptimizationFeature : uint32_t {
    kRenderOptCpuCulling = 1u << 0,
    kRenderOptStaticCache = 1u << 1,
    kRenderOptSorting = 1u << 2,
    kRenderOptLod = 1u << 3,
    kRenderOptMultiDraw = 1u << 4,
    kRenderOptGpuCulling = 1u << 5,
    kRenderOptHiZ = 1u << 6,
};

class Renderer {
public:
    struct RenderStats {
        int drawCalls = 0;
        int meshDraws = 0;
        int fullscreenDraws = 0;
        long long triangles = 0;
        // Filled by the optimized path. The legacy path only fills visibleRenderables.
        int objectsSubmitted = 0;    // renderables that reached culling
        int objectsCulled = 0;       // rejected by the CPU frustum test
        int objectsLodCulled = 0;    // rejected by LOD screen size on the CPU
        int visibleRenderables = 0;  // drawn per object or as a CPU-visible MDI command
        int mdiObjects = 0;          // objects owned by multi-draw batches
        int mdiBatches = 0;
        int multiDrawCalls = 0;
        int indirectDrawCommands = 0;
        float cpuCullingMs = 0.0f;
        float queueBuildMs = 0.0f;
        float sortingMs = 0.0f;
        // GPU culling results come back asynchronously, a few frames old, and are
        // -1 until the first one lands. Nothing waits on the GPU for them.
        float gpuCullingMs = -1.0f;
        int gpuVisible = -1;
        uint32_t optimizationFeatures = 0;  // RenderOptimizationFeature bits
        bool optimizationAutomatic = false;
    };
    struct StaticMergeBatch {
        std::unique_ptr<Mesh> mesh;
        std::string vertPath;
        std::string fragPath;
        MaterialProperties material;
        std::string materialPath;
        std::string albedoTexturePath;
        std::string overlayTexturePath;
        std::string normalMapPath;
        std::string metallicRoughnessMapPath;
        std::string metallicMapPath;
        std::string roughnessMapPath;
        std::string opacityMapPath;
        std::string emissiveMapPath;
        std::string occlusionMapPath;
        std::string heightMapPath;
        bool useOverlay = false;
        bool unlit = false;
        bool doubleSided = false;
        glm::vec3 boundsCenter = glm::vec3(0.0f);
        float boundsRadius = 0.0f;
    };

    struct PostProcessStats {
        float totalMs = 0.0f;
        float resolveMs = 0.0f;
        float bloomExtractMs = 0.0f;
        float bloomBlurMs = 0.0f;
        float compositeMs = 0.0f;
        int activeVolumeCount = 0;
        int resolvedVolumeId = -1;
        std::string resolvedVolumeName;
        float resolvedBlend = 0.0f;
        int activeEffectCount = 0;
        bool hdrEnabled = false;
        bool bloomUsed = false;
        bool motionBlurUsed = false;
        bool executionBegan = false;
        bool compositeExecuted = false;
        bool finalTextureDiffersFromSource = false;
        unsigned int sourceTextureId = 0;
        unsigned int sourceFramebufferId = 0;
        unsigned int bloomExtractDestinationTextureId = 0;
        unsigned int bloomExtractDestinationFramebufferId = 0;
        unsigned int bloomBlurTextureId = 0;
        unsigned int bloomBlurFramebufferId = 0;
        unsigned int compositeDestinationTextureId = 0;
        unsigned int compositeDestinationFramebufferId = 0;
        unsigned int finalPresentedTextureId = 0;
        unsigned int finalPresentedFramebufferId = 0;
        std::string skipReason;
    };

    struct UiTargetInfo {
        unsigned int fbo = 0;
        unsigned int texture = 0;
        int width = 0;
        int height = 0;
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        int x = 0;
        int y = 0;
        int clearX = 0;
        int clearY = 0;
        int clearWidth = 0;
        int clearHeight = 0;
        glm::vec4 uvTransform = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
        bool usesAtlas = false;
        bool invalidated = false;
        uint64_t atlasKey = 0;
    };

private:
    unsigned int framebuffer = 0, viewportTexture = 0;
    unsigned int sceneDepthTexture = 0;
    unsigned int sceneDepthTextureForPass = 0;
    unsigned int sceneColorTextureForPass = 0;
    int currentWidth = 800, currentHeight = 600;
    bool resizeInProgress = false;
    bool rendererShuttingDown = false;
    struct RenderTarget {
        unsigned int fbo = 0;
        unsigned int texture = 0;
        unsigned int rbo = 0;
        unsigned int depthTexture = 0;
        int width = 0;
        int height = 0;
        bool hasAlpha = false;
        bool hdr = false;
        bool hasDepth = true;
        bool depthSampleable = false;
        MaterialProperties::TextureFilter textureFilter = MaterialProperties::TextureFilter::Bilinear;
    };
    struct RenderTargetOptions {
        bool alpha = false;
        bool hdr = false;
        bool depth = true;
        bool depthSampleable = false;
    };
    struct ResolvedPostFX {
        PostFXSettings settings;
        PostFXSettings polarizationSettings;
        bool hasPolarizationLayer = false;
        int activeVolumeCount = 0;
        int resolvedVolumeId = -1;
        std::string resolvedVolumeName;
        float resolvedBlend = 0.0f;
    };
    RenderTarget previewTarget;
    std::unordered_map<int, RenderTarget> extraPreviewTargets;
    std::unordered_map<int, uint64_t> extraPreviewTargetLastUsed;
    RenderTarget postTarget;
    RenderTarget previewPostTarget;
    RenderTarget polarizationTarget;
    RenderTarget overlayCaptureTarget;
    RenderTarget bandLayerTarget;
    RenderTarget bandCaptureTarget;
    int bandRegionX = 0;
    int bandRegionY = 0;
    int bandRegionWidth = 0;
    int bandRegionHeight = 0;
    int bandPreviousFramebuffer = 0;
    bool bandRegionActive = false;
    RenderTarget historyTarget;
    RenderTarget volumetricTarget;
    RenderTarget volumetricBlurTarget;
    RenderTarget ssrTarget;
    struct SurfaceBufferTarget {
        unsigned int fbo = 0;
        unsigned int texture = 0;
        unsigned int attachedDepth = 0;
        int width = 0;
        int height = 0;
    };
    SurfaceBufferTarget surfaceTarget;
    struct VoxelVolume {
        unsigned int texture[2] = {0, 0};
        unsigned int fbo = 0;
        int resolution = 0;
        int allocated = 0;
        int resultIndex = 0;
        glm::vec3 boundsMin = glm::vec3(0.0f);
        float extent = 0.0f;
        bool hasData = false;
    };
    VoxelVolume voxelVolume;
    struct XREyePreview {
        RenderTarget target;
        unsigned int startQuery[2] = {0, 0};
        unsigned int endQuery[2] = {0, 0};
        bool pending[2] = {false, false};
        int writeSlot = 0;
        double lastGpuMs = -1.0;
    };
    XREyePreview xrEyePreview[2];
    uint64_t voxelSceneSignature = 0;
    RenderTarget bloomTargetA;
    RenderTarget bloomTargetB;
    bool postFxSettingsOverrideActive = false;
    PostFXSettings postFxSettingsOverride;
    RenderTarget* postFxTargetOverride = nullptr;
    RenderTarget selectionMaskTarget;
    static constexpr int kMaxShadowMaps = 4;
    struct ShadowCubeMap {
        unsigned int fbo = 0;
        unsigned int depthCube = 0;
        int resolution = 0;
    };
    struct ShadowDirectionalMap {
        unsigned int fbo = 0;
        unsigned int depthTexture = 0;
        int resolution = 0;
    };
    struct MirrorUpdateState {
        glm::vec3 lastCameraPos = glm::vec3(FLT_MAX);
        glm::vec3 lastCameraFront = glm::vec3(0.0f);
        glm::vec3 lastCameraUp = glm::vec3(0.0f);
        glm::vec3 lastMirrorPos = glm::vec3(FLT_MAX);
        glm::vec3 lastMirrorRot = glm::vec3(FLT_MAX);
        glm::vec3 lastMirrorScale = glm::vec3(FLT_MAX);
        double lastUpdateTime = -1.0;
    };
    struct ReflectionCastTarget {
        unsigned int fbo = 0;
        unsigned int cube = 0;
        unsigned int depth = 0;
        int resolution = 0;
        bool hasCapture = false;
    };
    struct SkyboxReflectionTarget {
        unsigned int fbo = 0;
        unsigned int cube = 0;
        unsigned int depth = 0;
        int resolution = 0;
        uint64_t signature = 0;
        bool hasCapture = false;
        double lastCaptureSec = -1.0;
    };
    struct MaskDrawItem {
        const SceneObject* obj = nullptr;
        Mesh* mesh = nullptr;
        bool wantsGpuSkinning = false;
        bool doubleSided = false;
    };
    // One draw in renderSceneInternal. It used to be local to that function; it lives
    // here so the optimized path can keep its item buffers between frames.
    struct SceneRenderItem {
        const SceneObject* obj = nullptr;
        const StaticMergeBatch* staticBatch = nullptr;
        Mesh* mesh = nullptr;
        glm::mat4 model = glm::mat4(1.0f);
        glm::vec3 sortCenter = glm::vec3(0.0f);
        glm::vec3 depthSortCenter = glm::vec3(0.0f);
        const std::string* vertPath = nullptr;
        const std::string* fragPath = nullptr;
        MaterialProperties material = MaterialProperties{};
        const std::string* materialPath = nullptr;
        const std::string* albedoTexturePath = nullptr;
        const std::string* overlayTexturePath = nullptr;
        const std::string* normalMapPath = nullptr;
        const std::string* metallicRoughnessMapPath = nullptr;
        const std::string* metallicMapPath = nullptr;
        const std::string* roughnessMapPath = nullptr;
        const std::string* opacityMapPath = nullptr;
        const std::string* emissiveMapPath = nullptr;
        const std::string* occlusionMapPath = nullptr;
        const std::string* heightMapPath = nullptr;
        bool useOverlay = false;
        int boneLimit = 0;
        int availableBones = 0;
        bool wantsGpuSkinning = false;
        bool isUiCanvas3D = false;
        int uiSortingOrder = 0;
        bool requiresBlending = false;
        bool sortOpaque = false;
        bool unlit = false;
        bool doubleSided = false;
        float cameraDepth = 0.0f;
        float cameraDistanceSq = 0.0f;
        uint64_t opaqueSortKey = 0;
        // Set only by the optimized path; the legacy path leaves the defaults.
        Shader* shader = nullptr;         // resolved once per shader pair per pass
        int multiDrawBatch = -1;          // >= 0: this item draws a whole multi-draw batch
        bool reflectionResolved = false;  // reflectionCube/Intensity already looked up
        unsigned int reflectionCube = 0;
        float reflectionIntensity = 0.0f;
    };
    // Everything behind Project Settings > Optimization: per-object caches, pass
    // buffers, multi-draw and GPU culling resources. RenderOptimizationInternal.h.
    struct OptimizationState;
    std::unique_ptr<OptimizationState> optState;
    RenderOptimizationSettings optimizationSettings;
    struct UiAtlasSlot {
        int contentWidth = 0;
        int contentHeight = 0;
        int x = 0;
        int y = 0;
        int clearX = 0;
        int clearY = 0;
        int clearWidth = 0;
        int clearHeight = 0;
    };
    struct UiAtlasTarget {
        RenderTarget target;
        int size = 0;
        int cursorX = 0;
        int cursorY = 0;
        int rowHeight = 0;
        std::unordered_map<int, UiAtlasSlot> slots;
    };
    Shader* shader = nullptr;
    Shader* defaultShader = nullptr;
    Shader* postShader = nullptr;
    Shader* compositeShader = nullptr;
    Shader* brightShader = nullptr;
    Shader* blurShader = nullptr;
    Shader* volumetricShader = nullptr;
    Shader* ssrShader = nullptr;
    Shader* gbufferShader = nullptr;
    Shader* voxelizeShader = nullptr;
    Shader* shadowDepthShader = nullptr;
    Shader* directionalShadowDepthShader = nullptr;
    Texture* texture1 = nullptr;
    Texture* texture2 = nullptr;
    unsigned int debugWhiteTexture = 0;
    unsigned int missingMaterialFallbackTexture = 0;
    struct CachedTextureEntry {
        std::unique_ptr<Texture> texture;
        size_t approxBytes = 0;
        double lastUsedTime = 0.0;
        uint64_t lastUsedFrame = 0;
    };
    std::unordered_map<std::string, CachedTextureEntry> textureCacheBilinear;
    std::unordered_map<std::string, CachedTextureEntry> textureCachePoint;
    std::unordered_map<std::string, double> missingTextureRetryAfter;
    std::unordered_map<std::string, std::string> textureLoadErrors;
    std::unordered_map<std::string, TextureFormatPolicy> textureFormatOverrides;
    std::string textureKeyRoot;
    struct ShaderEntry {
        std::unique_ptr<Shader> shader;
        fs::file_time_type vertTime;
        fs::file_time_type fragTime;
        fs::file_time_type geomTime;
        std::string vertPath;
        std::string fragPath;
        std::string geomPath;
        double nextReloadCheckTime = 0.0;
    };
    std::unordered_map<std::string, ShaderEntry> shaderCache;
    std::string defaultVertPath = "Resources/Shaders/vert.glsl";
    std::string skinnedVertPath = "Resources/Shaders/skinned_vert.glsl";
    std::string defaultFragPath = "Resources/Shaders/frag.glsl";
    std::string postVertPath = "Resources/Shaders/postfx_vert.glsl";
    std::string postFragPath = "Resources/Shaders/postfx_frag.glsl";
    std::string postCompositeFragPath = "Resources/Shaders/postfx_composite_frag.glsl";
    std::string postBrightFragPath = "Resources/Shaders/postfx_bright_frag.glsl";
    std::string postBlurFragPath = "Resources/Shaders/postfx_blur_frag.glsl";
    std::string selectionMaskVertPath = "Resources/Shaders/selection_mask_vert.glsl";
    std::string selectionMaskFragPath = "Resources/Shaders/selection_mask_frag.glsl";
    std::string wireframeFragPath = "Resources/Shaders/wireframe_frag.glsl";
    std::string selectionOutlineFragPath = "Resources/Shaders/selection_outline_frag.glsl";
    std::string shadowDepthVertPath = "Resources/Shaders/shadow_depth_vert.glsl";
    std::string shadowDepthFragPath = "Resources/Shaders/shadow_depth_frag.glsl";
    std::string directionalShadowDepthVertPath = "Resources/Shaders/shadow_directional_depth_vert.glsl";
    std::string directionalShadowDepthFragPath = "Resources/Shaders/shadow_directional_depth_frag.glsl";
    std::string volumetricFragPath = "Resources/Shaders/volumetric_frag.glsl";
    std::string volumetricBlurFragPath = "Resources/Shaders/volumetric_blur_frag.glsl";
    std::string ssrFragPath = "Resources/Shaders/ssr_frag.glsl";
    std::string gbufferFragPath = "Resources/Shaders/gbuffer_frag.glsl";
    std::string voxelizeVertPath = "Resources/Shaders/voxelize_vert.glsl";
    std::string voxelizeGeomPath = "Resources/Shaders/voxelize_geom.glsl";
    std::string voxelizeFragPath = "Resources/Shaders/voxelize_frag.glsl";
    bool autoReloadShaders = true;
    int maxRealtimeLights = 10;
    int shadowMapResolution = 512;
    glm::vec3 ambientColor = glm::vec3(0.2f, 0.2f, 0.2f);
    int sceneLightBudget = 20;
    int directionalLightAllowance = 1;
    bool mainLightEnabled = true;
    bool mainLightShadows = true;
    bool additionalLightsEnabled = true;
    bool additionalLightsShadows = true;
    bool specularEnabled = true;
    bool softShadowsAllowed = true;
    float shadowMaxDistance = 0.0f;
    RendererColorPrecision colorPrecision = RendererColorPrecision::Auto;
    TextureFormatPolicy defaultTextureFormatPolicy = TextureFormatPolicy::Auto;
    Mesh* cubeMesh = nullptr;
    Mesh* sphereMesh = nullptr;
    Mesh* capsuleMesh = nullptr;
    std::unordered_map<int, Mesh*> capsuleColliderMeshes;
    Mesh* capsuleColliderMesh(float radius, float cylinderHalfHeight);
    Mesh* planeMesh = nullptr;
    Mesh* torusMesh = nullptr;
    Skybox* skybox = nullptr;
    glm::vec2 sunScreenUv = glm::vec2(0.5f);
    bool sunOnScreen = false;
    unsigned int quadVAO = 0;
    unsigned int quadVBO = 0;
    unsigned int displayTexture = 0;
    bool historyValid = false;
    size_t textureCacheBudgetBytes = 384ull * 1024ull * 1024ull;
    size_t textureCacheUsageBytes = 0;
    // Bumped whenever a texture is loaded, fails, or leaves the cache, so answers
    // derived from texture contents (alpha blending) can be cached until it moves.
    uint64_t textureCacheGeneration = 0;
    WeatherDrive weatherDrive;
    float weatherTime = 0.0f;

    bool warnedTextureCacheThrashing = false;
    bool warnedTextureCacheOverCommitted = false;

    unsigned int luminancePbo[2] = {0, 0};
    int luminancePboWriteIndex = 0;
    bool luminancePboPrimed = false;
    float adaptedLuminance = -1.0f;
    double lastAdaptationTime = 0.0;

    int shadowFilterQuality = 2;
    float shadowLodDistance = 30.0f;
    bool shadowContactHardening = true;
    uint64_t staticMergeSceneSignature = 0;
    uint64_t frameSerial = 0;
    uint64_t previewTargetUseSerial = 0;
    uint64_t lastStaticMergeCheckFrameSerial = std::numeric_limits<uint64_t>::max();
    std::unordered_map<int, ShadowCubeMap> shadowCubeMaps;
    std::unordered_map<int, ShadowDirectionalMap> shadowDirectionalMaps;
    std::unordered_map<int, RenderTarget> mirrorTargets;
    std::unordered_map<int, MirrorUpdateState> mirrorUpdateStates;
    std::unordered_map<int, ReflectionCastTarget> reflectionCastTargets;
    SkyboxReflectionTarget skyboxReflectionTarget;
    std::unordered_map<int, RenderTarget> uiTargets;
    std::unordered_map<int, UiTargetInfo> uiTargetViews;
    std::unordered_map<uint64_t, UiAtlasTarget> uiTargetAtlases;
    int uiAtlasMaxSize = 4096;
    std::vector<std::array<glm::vec4, 6>> capturedSceneFrustums;
    // Parallel to capturedSceneFrustums: the camera and pixel size of each view.
    struct CapturedSceneView {
        glm::mat4 view{1.0f};
        glm::mat4 proj{1.0f};
        int width = 0;
        int height = 0;
    };
    std::vector<CapturedSceneView> capturedSceneViews;
    std::vector<StaticMergeBatch> staticMergeBatches;
    std::unordered_set<int> staticMergeSourceIds;
    RenderStats viewportStats;
    RenderStats previewStats;
    PostProcessStats viewportPostStats;
    PostProcessStats previewPostStats;
    bool reflectionCapturePass = false;
    const glm::mat4* sceneProjectionOverride = nullptr;
    RenderStats* activeStats = nullptr;
    float selectionOutlineBlend = 0.0f;
    double selectionOutlineLastUpdateSec = 0.0;
    std::vector<int> selectionOutlineVisualIds;
    std::vector<int> selectionOutlinePrevIds;
    std::vector<int> selectionOutlineSourceScratch;
    std::vector<int> selectionOutlineInputScratch;
    std::unordered_set<int> selectionOutlineSelectedScratch;
    std::vector<MaskDrawItem> selectionOutlineCurrentDrawScratch;
    std::vector<MaskDrawItem> selectionOutlinePreviousDrawScratch;
    float selectionOutlineSwapBlend = 1.0f;

    void setupFBO();
    void ensureRenderTarget(RenderTarget& target, int w, int h);
    void ensureRenderTarget(RenderTarget& target, int w, int h, bool alpha);
    void ensureRenderTarget(RenderTarget& target, int w, int h, bool alpha, bool hdr);
    void ensureRenderTarget(RenderTarget& target, int w, int h, bool alpha, bool hdr, bool depth);
    void ensureRenderTarget(RenderTarget& target, int w, int h, const RenderTargetOptions& options);
    void releaseRenderTarget(RenderTarget& target);
    void ensureSurfaceTarget(int w, int h, unsigned int depthTexture);
    void releaseSurfaceTarget();
    bool ensureVoxelVolume(int resolution, int volumeCount);
    void releaseVoxelVolume();
    void releaseReflectionCastTarget(ReflectionCastTarget& target);
    void releaseSkyboxReflectionTarget(SkyboxReflectionTarget& target);
    void updateMirrorTargets(const Camera& camera, const std::vector<SceneObject>& sceneObjects, int width, int height, float fovDeg, float nearPlane, float farPlane);
    void updateReflectionCastTargets(const std::vector<SceneObject>& sceneObjects, float nearPlane, float farPlane);
    void updateSkyboxReflectionTarget();
    void ensureQuad();
    void drawFullscreenQuad();
    void purgeTextureCacheIfNeeded();
    float updateEyeAdaptation(unsigned int sourceTexture, int width, int height,
                              const PostFXSettings& settings);
    void purgePreviewTargetsIfNeeded(int keepSlot);
    void rebuildStaticMergeBatches(const std::vector<SceneObject>& sceneObjects);
    void resetStats(RenderStats& stats);
    void recordDrawCall();
    void recordMeshDraw();
    void recordFullscreenDraw();
    void clearHistory();
    void clearTarget(RenderTarget& target);
    void renderSceneInternal(const Camera& camera, const std::vector<SceneObject>& sceneObjects, int width, int height, bool unbindFramebuffer, float fovDeg, float nearPlane, float farPlane, bool drawMirrorObjects, bool drawSkybox);
    void renderWireframeOverlay(const Camera& camera,
                                const std::vector<SceneObject>& sceneObjects,
                                float fovDeg, float nearPlane, float farPlane,
                                bool shadedSurface);
    unsigned int applyPostProcessing(const Camera& camera, const std::vector<SceneObject>& sceneObjects, unsigned int sourceTexture, int width, int height, bool allowHistory);
    bool postFxPreserveAlpha = false;
    ResolvedPostFX gatherPostFX(const Camera& camera, const std::vector<SceneObject>& sceneObjects) const;
    unsigned int findFramebufferForTexture(unsigned int texture) const;
    void logPostFxDebug(const PostProcessStats& stats, bool allowHistory) const;

public:
    // Out of line: optState's type is only complete in the renderer's own sources.
    Renderer();
    ~Renderer();

    void initialize();
    Texture* getTexture(const std::string& path, MaterialProperties::TextureFilter filter = MaterialProperties::TextureFilter::Bilinear);
    unsigned int getDebugWhiteTextureId() const { return debugWhiteTexture; }
    void invalidateTexture(const std::string& path);
    std::string getTextureLoadError(const std::string& path) const;

    void setTextureFormatOverride(const std::string& path, TextureFormatPolicy policy);
    TextureFormatPolicy getTextureFormatOverride(const std::string& path) const;
    const std::unordered_map<std::string, TextureFormatPolicy>& getTextureFormatOverrides() const { return textureFormatOverrides; }
    void clearTextureFormatOverrides() { textureFormatOverrides.clear(); }
    void setTextureKeyRoot(const std::string& root) { textureKeyRoot = root; }
    std::string normalizeTextureKey(const std::string& path) const;
    Shader* getShader(const std::string& vert, const std::string& frag);
    Shader* getShader(const std::string& vert, const std::string& geom, const std::string& frag);
    bool forceReloadShader(const std::string& vert, const std::string& frag);
    void setAmbientColor(const glm::vec3& color) { ambientColor = color; }
    glm::vec3 getAmbientColor() const { return ambientColor; }
    void setShadowMapResolution(int resolution) { shadowMapResolution = std::clamp(resolution, 128, 4096); }
    int getShadowMapResolution() const { return shadowMapResolution; }
    void setMaxRealtimeLights(int count) { maxRealtimeLights = std::clamp(count, 1, kRendererMaxRealtimeLights); }
    int getMaxRealtimeLights() const { return maxRealtimeLights; }
    void setSceneLightBudget(int budget, int directionalAllowance) {
        sceneLightBudget = std::max(1, budget);
        directionalLightAllowance = std::clamp(directionalAllowance, 1, 4);
    }
    int getSceneLightBudget() const { return sceneLightBudget; }
    int getDirectionalLightAllowance() const { return directionalLightAllowance; }
    void setMainLightEnabled(bool enabled) { mainLightEnabled = enabled; }
    bool isMainLightEnabled() const { return mainLightEnabled; }
    void setMainLightShadows(bool enabled) { mainLightShadows = enabled; }
    bool areMainLightShadowsEnabled() const { return mainLightShadows; }
    void setAdditionalLightsEnabled(bool enabled) { additionalLightsEnabled = enabled; }
    bool areAdditionalLightsEnabled() const { return additionalLightsEnabled; }
    void setAdditionalLightsShadows(bool enabled) { additionalLightsShadows = enabled; }
    bool areAdditionalLightShadowsEnabled() const { return additionalLightsShadows; }
    void setSpecularEnabled(bool enabled) { specularEnabled = enabled; }
    bool isSpecularEnabled() const { return specularEnabled; }
    void setSoftShadowsAllowed(bool allowed) { softShadowsAllowed = allowed; }
    bool areSoftShadowsAllowed() const { return softShadowsAllowed; }
    void setShadowMaxDistance(float distance) { shadowMaxDistance = std::max(0.0f, distance); }
    float getShadowMaxDistance() const { return shadowMaxDistance; }
    void setColorPrecision(RendererColorPrecision precision);
    RendererColorPrecision getColorPrecision() const { return colorPrecision; }
    void setDefaultTextureFormatPolicy(TextureFormatPolicy policy);
    TextureFormatPolicy getDefaultTextureFormatPolicy() const { return defaultTextureFormatPolicy; }
    void setMaxTextureDimension(int maxDimension);
    void setWeatherDrive(const WeatherDrive& drive, float weatherTimeSeconds) {
        weatherDrive = drive;
        weatherTime = weatherTimeSeconds;
    }
    void setShadowQuality(int filterQuality, float lodDistance, bool contactHardening);
    void setShaderAutoReload(bool enabled) { autoReloadShaders = enabled; }
    bool isShaderAutoReloadEnabled() const { return autoReloadShaders; }
    void setOptimizationSettings(const RenderOptimizationSettings& settings);
    const RenderOptimizationSettings& getOptimizationSettings() const { return optimizationSettings; }
    RenderOptimizationSupport getOptimizationSupport() const;
    void resize(int w, int h);
    int getWidth() const { return currentWidth; }
    int getHeight() const { return currentHeight; }
    void setFrameSerial(uint64_t serial) {
        frameSerial = serial;
        capturedSceneFrustums.clear();
        capturedSceneViews.clear();
    }
    bool hasCapturedSceneFrustums() const { return !capturedSceneFrustums.empty(); }
    bool isObjectVisibleInCapturedFrustums(const SceneObject& obj, float radiusMargin = 1.0f) const;
    // Largest face-on size, in pixels, of a sprite-quad object across this
    // frame's scene views (oblique views over-estimate, never under). False when
    // no view saw it. outUnbounded when a view sits too close to measure.
    bool measureObjectScreenSizePx(const SceneObject& obj, glm::vec2& outSizePx,
                                   bool& outUnbounded) const;

    void beginRender(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPos);
    void renderSkybox(const glm::mat4& view, const glm::mat4& proj);
    void renderObject(const SceneObject& obj);
    void renderScene(const Camera& camera, const std::vector<SceneObject>& sceneObjects, int selectedId = -1, float fovDeg = FOV, float nearPlane = NEAR_PLANE, float farPlane = FAR_PLANE, bool drawColliders = false, const std::vector<int>* selectedIds = nullptr, SceneRenderMode renderMode = SceneRenderMode::Normal, bool applyPostFX = true);
    void renderSelectionOutline(const Camera& camera, const std::vector<SceneObject>& sceneObjects, const std::vector<int>& selectedIds, float fovDeg, float nearPlane, float farPlane);
    unsigned int renderScenePreview(const Camera& camera, const std::vector<SceneObject>& sceneObjects, int width, int height, float fovDeg, float nearPlane, float farPlane, bool applyPostFX = false, int previewSlot = 0, bool transparentBackground = false);
    void renderSceneToTarget(const Camera& camera,
                             const std::vector<SceneObject>& sceneObjects,
                             unsigned int targetFramebuffer,
                             int width, int height,
                             const glm::mat4& projection,
                             float nearPlane, float farPlane,
                             bool clearTarget = true);
    void renderCollisionOverlay(const Camera& camera, const std::vector<SceneObject>& sceneObjects, int width, int height, float fovDeg, float nearPlane, float farPlane, const std::vector<int>* previewIds = nullptr);
    void endRender();
    PostFXSettings resolvePostFXSettings(const Camera& camera, const std::vector<SceneObject>& sceneObjects) const;
    unsigned int postProcessTexture(const Camera& camera, const std::vector<SceneObject>& sceneObjects, unsigned int sourceTexture, int width, int height, bool allowHistory = false);
    bool postProcessFramebufferRegion(const Camera& camera,
                                      const std::vector<SceneObject>& sceneObjects,
                                      int x, int y, int width, int height,
                                      bool allowHistory = false);

    struct PostFX2DScope {
        bool active = false;
        PostFX2DScopeMode mode = PostFX2DScopeMode::AtOrBelow;
        int minOrder = 0;
        int maxOrder = 0;
    };
    PostFX2DScope resolvePostFX2DScope(const Camera& camera,
                                       const std::vector<SceneObject>& sceneObjects) const;

    bool beginBandPostFxRegion(int x, int y, int width, int height,
                               int framebufferWidth, int framebufferHeight);
    bool endBandPostFxRegion(const Camera& camera,
                             const std::vector<SceneObject>& sceneObjects,
                             bool allowHistory = false);
    bool isBandPostFxRegionActive() const { return bandRegionActive; }

    Skybox* getSkybox() { return skybox; }
    unsigned int getViewportTexture() const { return displayTexture ? displayTexture : viewportTexture; }
    unsigned int getSceneDepthTexture() const { return sceneDepthTexture; }
    bool giSupported() const;
    std::string giUnsupportedReason() const;
    unsigned int getRawViewportTexture() const { return viewportTexture; }
    const RenderStats& getLastViewportStats() const { return viewportStats; }
    const RenderStats& getLastPreviewStats() const { return previewStats; }
    const PostProcessStats& getLastViewportPostStats() const { return viewportPostStats; }
    const PostProcessStats& getLastPreviewPostStats() const { return previewPostStats; }
    uint64_t getTextureCacheUsageBytes() const { return static_cast<uint64_t>(textureCacheUsageBytes); }
    uint64_t getTextureCacheBudgetBytes() const { return static_cast<uint64_t>(textureCacheBudgetBytes); }
    size_t getTextureCacheCount() const { return textureCacheBilinear.size() + textureCachePoint.size(); }
    size_t getShaderCacheCount() const { return shaderCache.size(); }
    size_t getExtraPreviewTargetCount() const { return extraPreviewTargets.size(); }
    size_t getReflectionCastTargetCount() const { return reflectionCastTargets.size(); }

    struct XREyePreviewResult {
        unsigned int texture = 0;
        double gpuMs = -1.0;
        RenderStats stats;
    };
    XREyePreviewResult renderXREyePreview(const Camera& camera,
                                          const std::vector<SceneObject>& sceneObjects,
                                          int eyeIndex, int width, int height,
                                          const glm::mat4& projection,
                                          float nearPlane, float farPlane);
    void releaseXREyePreviews();

    UiTargetInfo ensureUiTarget(int id,
                                int width,
                                int height,
                                int layer,
                                MaterialProperties::TextureFilter filter,
                                bool allowAtlas);
    UiTargetInfo getUiTargetInfo(int id) const;
    void markUiTargetRendered(int id);
    void cleanupUiTargets(const std::unordered_set<int>& active);
};
