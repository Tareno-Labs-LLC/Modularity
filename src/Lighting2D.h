#pragma once
#include "Common.h"
#include "Lighting2DTypes.h"
#include "../include/Shaders/Shader.h"
#include <array>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>
class Renderer;
struct Light2DPolygonCache {
    uint64_t version = 0;
    bool valid = false;
    bool selfIntersecting = false;
    glm::vec2 boundsMin = glm::vec2(0.0f);
    glm::vec2 boundsMax = glm::vec2(0.0f);
    std::vector<glm::vec2> vertices;
    std::vector<unsigned int> indices;
    std::string error;
};
struct Light2DScreenSprite {
    int objectId = -1;
    int layer = 0;
    int drawOrder = 0;
    unsigned int textureId = 0;
    unsigned int normalMapTextureId = 0;
    glm::vec2 positions[4] = {};
    glm::vec2 uvs[4] = {};
    glm::vec4 tint = glm::vec4(1.0f);
    bool receiveLighting = true;
    bool unlit = false;
    float emissiveIntensity = 0.0f;
};
struct Light2DScreenLight {
    int objectId = -1;
    bool enabled = true;
    Light2DType type = Light2DType::Point;
    int blendStyle = 0;
    int lightOrder = 0;
    Light2DOverlapOperation overlapOperation = Light2DOverlapOperation::Additive;
    bool targetAllLayers = true;
    uint32_t targetLayerMask = 0xFFFFFFFFu;
    bool cullWhenOffscreen = true;
    float offscreenCullMarginPx = 0.0f;
    glm::vec4 color = glm::vec4(1.0f);
    float intensity = 1.0f;
    float radius = 5.0f;
    float innerRadius = 0.0f;
    float outerRadius = 5.0f;
    float falloffStrength = 1.0f;
    float innerSpotAngle = 24.0f;
    float outerSpotAngle = 42.0f;
    float shadowStrength = 0.75f;
    bool volumetricEnabled = false;
    bool castsShadows = false;
    glm::vec2 position = glm::vec2(0.0f);
    float rotationRad = 0.0f;
    glm::vec2 boundsMin = glm::vec2(0.0f);
    glm::vec2 boundsMax = glm::vec2(0.0f);
    float freeformFeatherPx = 24.0f;
    float freeformEdgeFalloff = 1.0f;
    unsigned int cookieTextureId = 0;
    glm::vec2 cookieScale = glm::vec2(1.0f);
    float cookieRotationRad = 0.0f;
    std::vector<glm::vec2> polygon;
};
struct Light2DScreenShadowCaster {
    int objectId = -1;
    bool enabled = true;
    bool targetAllLayers = true;
    uint32_t targetLayerMask = 0xFFFFFFFFu;
    float shadowStrength = 1.0f;
    std::vector<glm::vec2> polygon;
};
struct Light2DPostFXSettings {
    bool enabled = false;
    float ditherIntensity = 0.65f;
    int colorBits = 5;
    float darkAdjustment = 0.35f;
    float ditherScale = 1.0f;
    float pixelation = 0.0f;
    float exposure = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    glm::vec3 colorFilter = glm::vec3(1.0f);
    float vignetteIntensity = 0.0f;
    float vignetteSmoothness = 0.35f;
    float chromaticAmount = 0.0f;
    float sharpenStrength = 0.0f;
    float grainAmount = 0.0f;
    float scanlineIntensity = 0.0f;
};
bool Light2DHasVisiblePostFx(const Light2DPostFXSettings& settings);
struct Light2DRenderRequest {
    int width = 0;
    int height = 0;
    glm::vec4 clearColor = glm::vec4(0.0f);
    glm::vec3 baseAmbient = glm::vec3(0.15f);
    float lightingBufferScale = 0.0f;
    bool lightingBufferScaleAuto = true;
    bool lightingPixelFilter = false;
    bool lightingSDR = false;
    Light2DPostFXSettings postFx;
    std::array<Light2DBlendStyleDefinition, 4> blendStyles = {};
    std::vector<Light2DScreenSprite> sprites;
    std::vector<Light2DScreenLight> lights;
    std::vector<Light2DScreenShadowCaster> shadowCasters;
};
struct Light2DDebugStats {
    int renderedLayers = 0;
    int visibleSprites = 0;
    int visibleLights = 0;
    int freeformLights = 0;
    int shadowCasters = 0;
    int polygonCacheHits = 0;
    int polygonCacheMisses = 0;
    int triangulationFailures = 0;
    int drawCalls = 0;
    float cpuBuildMs = 0.0f;
};
uint32_t Light2DLayerBit(int layer);
bool Light2DLayerMaskContains(uint32_t mask, int layer);
uint64_t Light2DHashShape(const std::vector<glm::vec2>& points);
bool Light2DPolygonSelfIntersects(const std::vector<glm::vec2>& points);
bool Light2DValidatePolygon(const std::vector<glm::vec2>& points, std::string* outError = nullptr);
bool Light2DTriangulatePolygon(const std::vector<glm::vec2>& points, std::vector<unsigned int>& outIndices, std::string* outError = nullptr);
class Lighting2DRenderer {
    public:
        Lighting2DRenderer() = default;
        ~Lighting2DRenderer();
        void shutdown();
        unsigned int render(const Light2DRenderRequest& request, Renderer& renderer);
        void setLightingBufferScale(float scale) { lightingBufferScale = std::clamp(scale, 0.25f, 1.0f); }
        float getLightingBufferScale() const { return lightingBufferScale; }

        const Light2DDebugStats& getLastStats() const { return lastStats; }
        const Light2DPolygonCache* getPolygonCache(int objectId) const;
        const Light2DPolygonCache& updatePolygonCache(int objectId, const Light2DComponent& component);
        void clearPolygonCache(int objectId);
    private:
        struct RenderTarget {
            unsigned int fbo = 0;
            unsigned int texture = 0;
            int width = 0;
            int height = 0;
            int filter = 0;
            unsigned int internalFormat = 0;
        };
        struct LayerBatch {
            int layer = 0;
            size_t spriteBegin = 0;
            size_t spriteEnd = 0;
            uint8_t blendModesMask = 0;
            std::vector<const Light2DScreenLight*> lights;
        };
        struct SpriteGpuVertex {
            glm::vec2 position = glm::vec2(0.0f);
            glm::vec2 uv = glm::vec2(0.0f);
            glm::vec4 color = glm::vec4(1.0f);
            glm::vec4 params = glm::vec4(0.0f);
        };
        bool initialized = false;
        RenderTarget finalTarget;
        RenderTarget postTarget;
        RenderTarget additiveTarget;
        RenderTarget multiplyTarget;
        RenderTarget subtractiveTarget;
        unsigned int quadVao = 0;
        unsigned int quadVbo = 0;
        unsigned int spriteVao = 0;
        unsigned int spriteVbo = 0;
        unsigned int polygonVao = 0;
        unsigned int polygonVbo = 0;
        unsigned int polygonEbo = 0;
        size_t spriteVboCapacity = 0;
        std::unique_ptr<Shader> lightQuadShader;
        std::unique_ptr<Shader> lightFreeformShader;
        std::unique_ptr<Shader> spriteShader;
        std::unique_ptr<Shader> postFxShader;
        float lightingBufferScale = 1.0f;
        std::vector<const Light2DScreenSprite*> orderedSpritesScratch;
        std::vector<const Light2DScreenLight*> orderedLightsScratch;
        std::vector<LayerBatch> layerBatchesScratch;
        std::unordered_map<int, size_t> layerToBatchIndexScratch;
        std::vector<SpriteGpuVertex> spriteVertices;
        std::vector<glm::vec2> polygonVerticesScratch;
        std::unordered_map<int, Light2DPolygonCache> polygonCache;
        Light2DDebugStats lastStats;
        void initializeIfNeeded();
        void destroyTarget(RenderTarget& target);
        void ensureTarget(RenderTarget& target, int width, int height, int filter, unsigned int internalFormat);
        void ensureGeometry();
        void ensureShaders();
        void clearTarget(RenderTarget& target, const glm::vec4& color) const;
        float resolveLightingBufferScale(const Light2DRenderRequest& request) const;
        void renderLayer(const Light2DRenderRequest& request, Renderer& renderer, const LayerBatch& batch, float lightingScale, bool clearLightBuffers);
        void clearLightTargets(uint8_t blendModesMask);
        void renderLightPass(const Light2DRenderRequest& request, Renderer& renderer, int layer, const std::vector<const Light2DScreenLight*>& lights, float lightingScale);
        void renderSpritePass(const Light2DRenderRequest& request, int layer, size_t spriteBegin, size_t spriteEnd, uint8_t blendModesMask);
        unsigned int applyPostProcessing(const Light2DRenderRequest& request, unsigned int sourceTexture, int width, int height);
        void drawFullscreenQuad() const;
        void drawUnitQuad() const;
};
