#pragma once

#include "Render25D/TMRenderer.h"
#include "Skybox/Skybox.h"
#include "Shaders/Shader.h"
#include "Textures/Texture.h"

namespace Modularity::Render25D {

class TMOpenGLRenderer final : public ITMRenderBackend {
public:
    using PresentationSettings = TMPresentationSettings;

    struct BackendStats {
        uint32_t floorDraws = 0;
        uint32_t modelDraws = 0;
        uint32_t submeshDraws = 0;
        uint32_t spriteDraws = 0;
    };

    TMOpenGLRenderer() = default;
    ~TMOpenGLRenderer();

    bool renderViewport(TMRenderer& renderer,
                        const TMRenderContext& context,
                        const TMScene& scene,
                        TMRenderer::RenderStats& outStats,
                        std::string& error);

    unsigned int renderPreview(TMRenderer& renderer,
                               const TMRenderContext& context,
                               const TMScene& scene,
                               int previewSlot,
                               TMRenderer::RenderStats& outStats,
                               std::string& error);

    unsigned int getViewportTexture() const;
    const BackendStats& getLastViewportBackendStats() const;
    PresentationSettings& getPresentationSettings();
    const PresentationSettings& getPresentationSettings() const;

    void invalidateCaches();

private:
    struct RenderTarget {
        unsigned int fbo = 0;
        unsigned int texture = 0;
        unsigned int depth = 0;
        int width = 0;
        int height = 0;
    };

    RenderTarget viewportTarget;
    RenderTarget viewportLowResTarget;
    std::unordered_map<int, RenderTarget> previewTargets;
    std::unordered_map<int, RenderTarget> previewLowResTargets;
    std::unordered_map<std::string, std::unique_ptr<Texture>> textureCache;
    std::unique_ptr<Shader> floorShader;
    std::unique_ptr<Shader> meshShader;
    std::unique_ptr<Shader> spriteShader;
    std::unique_ptr<Skybox> skybox;
    SkyboxSettings activeSkyboxSettings;
    bool activeSkyboxSettingsInitialized = false;
    PresentationSettings presentationSettings;
    unsigned int fallbackWhiteTexture = 0;
    unsigned int quadVAO = 0;
    unsigned int quadVBO = 0;
    unsigned int spriteVAO = 0;
    unsigned int spriteVBO = 0;
    const TMRenderContext* activeContext = nullptr;
    RenderTarget* activeTarget = nullptr;
    BackendStats viewportBackendStats;
    BackendStats previewBackendStats;
    BackendStats* activeBackendStats = nullptr;

    bool ensureInitialized(std::string& error);
    bool renderToTarget(RenderTarget& target,
                        RenderTarget& lowResTarget,
                        TMRenderer& renderer,
                        const TMRenderContext& context,
                        const TMScene& scene,
                        BackendStats& backendStats,
                        TMRenderer::RenderStats& outStats,
                        std::string& error);
    void ensureRenderTarget(RenderTarget& target, int width, int height);
    void destroyRenderTarget(RenderTarget& target);
    void ensureQuad();
    void destroyQuad();
    void ensureSpriteGeometry();
    void destroySpriteGeometry();
    bool ensureFallbackTexture();
    void destroyFallbackTexture();
    Texture* getTexture(const std::string& path, TMTextureFilter filter);
    unsigned int resolveTextureId(const std::string& texturePath,
                                  TMTextureFilter filter,
                                  const std::string& sourceMeshPath = std::string());
    TMTextureFilter applyTextureFilterPolicy(TMTextureFilter filter) const;
    float getPresentationPitchDegrees() const;

    void beginFrame(const TMRenderContext& context) override;
    void drawM7Floor(const M7FloorDrawCommand& command) override;
    void drawSectorModel(const SectorModelDrawCommand& command) override;
    void drawSprite25D(const Sprite25DDrawCommand& command) override;
    void endFrame() override;
};

} // namespace Modularity::Render25D
