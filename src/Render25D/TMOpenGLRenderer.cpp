#include "Render25D/TMOpenGLRenderer.h"

#include "Rendering.h"
#include "Skybox/Skybox.h"
#include "Platform/PlatformTime.h"

#include <cstddef>
#include <cmath>

namespace Modularity::Render25D {

namespace {

constexpr const char* kTMFloorVertexShader = "Resources/Shaders/tm_floor_vert.glsl";
constexpr const char* kTMFloorFragmentShader = "Resources/Shaders/tm_floor_frag.glsl";
constexpr const char* kTMMeshVertexShader = "Resources/Shaders/tm_mesh_vert.glsl";
constexpr const char* kTMMeshFragmentShader = "Resources/Shaders/tm_mesh_frag.glsl";
constexpr const char* kTMSpriteVertexShader = "Resources/Shaders/tm_sprite25d_vert.glsl";
constexpr const char* kTMSpriteFragmentShader = "Resources/Shaders/tm_sprite25d_frag.glsl";

struct SpriteVertex {
    glm::vec3 position;
    glm::vec2 uv;
    glm::vec4 color;
};

std::string ResolveTexturePath(const std::string& texturePath, const std::string& sourceMeshPath) {
    if (texturePath.empty()) {
        return texturePath;
    }

    fs::path candidate(texturePath);
    if (candidate.is_absolute() || sourceMeshPath.empty()) {
        return texturePath;
    }

    return (fs::path(sourceMeshPath).parent_path() / candidate).lexically_normal().string();
}

std::string BuildTextureCacheKey(const std::string& path, TMTextureFilter filter) {
    return path + "#" + std::to_string(static_cast<uint32_t>(filter));
}

GLenum ToGLMinFilter(TMTextureFilter filter) {
    return (filter == TMTextureFilter::Point) ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
}

GLenum ToGLMagFilter(TMTextureFilter filter) {
    return (filter == TMTextureFilter::Point) ? GL_NEAREST : GL_LINEAR;
}

glm::vec4 CombineColorTint(const glm::vec4& a, const glm::vec4& b) {
    return glm::clamp(a * b, glm::vec4(0.0f), glm::vec4(1.0f));
}

bool SkyboxSettingsEqual(const SkyboxSettings& a, const SkyboxSettings& b) {
    return a.mode == b.mode &&
           a.sunTexturePath == b.sunTexturePath &&
           a.moonTexturePath == b.moonTexturePath &&
           a.scrollingTexturePath == b.scrollingTexturePath &&
           a.scrollingRepeatX == b.scrollingRepeatX &&
           a.scrollingRepeatY == b.scrollingRepeatY &&
           a.scrollingLookSensitivity == b.scrollingLookSensitivity &&
           a.scrollingVerticalInfluence == b.scrollingVerticalInfluence;
}

} // namespace

TMOpenGLRenderer::~TMOpenGLRenderer() {
    invalidateCaches();
}

bool TMOpenGLRenderer::renderViewport(TMRenderer& renderer,
                                      const TMRenderContext& context,
                                      const TMScene& scene,
                                      TMRenderer::RenderStats& outStats,
                                      std::string& error) {
    return renderToTarget(viewportTarget, viewportLowResTarget, renderer, context, scene,
                          viewportBackendStats, outStats, error);
}

unsigned int TMOpenGLRenderer::renderPreview(TMRenderer& renderer,
                                             const TMRenderContext& context,
                                             const TMScene& scene,
                                             int previewSlot,
                                             TMRenderer::RenderStats& outStats,
                                             std::string& error) {
    RenderTarget& target = previewTargets[previewSlot];
    RenderTarget& lowResTarget = previewLowResTargets[previewSlot];
    if (!renderToTarget(target, lowResTarget, renderer, context, scene, previewBackendStats, outStats, error)) {
        return 0;
    }
    return target.texture;
}

unsigned int TMOpenGLRenderer::getViewportTexture() const {
    return viewportTarget.texture;
}

const TMOpenGLRenderer::BackendStats& TMOpenGLRenderer::getLastViewportBackendStats() const {
    return viewportBackendStats;
}

TMOpenGLRenderer::PresentationSettings& TMOpenGLRenderer::getPresentationSettings() {
    return presentationSettings;
}

const TMOpenGLRenderer::PresentationSettings& TMOpenGLRenderer::getPresentationSettings() const {
    return presentationSettings;
}

void TMOpenGLRenderer::invalidateCaches() {
    textureCache.clear();
    floorShader.reset();
    meshShader.reset();
    spriteShader.reset();
    skybox.reset();
    activeSkyboxSettingsInitialized = false;
    destroyQuad();
    destroySpriteGeometry();
    destroyFallbackTexture();
    destroyRenderTarget(viewportTarget);
    destroyRenderTarget(viewportLowResTarget);
    for (auto& [slot, target] : previewTargets) {
        (void)slot;
        destroyRenderTarget(target);
    }
    previewTargets.clear();
    for (auto& [slot, target] : previewLowResTargets) {
        (void)slot;
        destroyRenderTarget(target);
    }
    previewLowResTargets.clear();
}

bool TMOpenGLRenderer::ensureInitialized(std::string& error) {
    if (!Modularity::Platform::HasCurrentGLContext()) {
        error = "TM renderer requires an active OpenGL context";
        return false;
    }

    if (!ensureFallbackTexture()) {
        error = "TM renderer failed to create fallback texture";
        return false;
    }

    ensureQuad();
    if (quadVAO == 0 || quadVBO == 0) {
        error = "TM renderer failed to create fullscreen quad";
        return false;
    }

    if (!floorShader) {
        floorShader = std::make_unique<Shader>(kTMFloorVertexShader, kTMFloorFragmentShader);
    }
    if (!meshShader) {
        meshShader = std::make_unique<Shader>(kTMMeshVertexShader, kTMMeshFragmentShader);
    }
    if (!spriteShader) {
        spriteShader = std::make_unique<Shader>(kTMSpriteVertexShader, kTMSpriteFragmentShader);
    }
    if (!skybox) {
        skybox = std::make_unique<Skybox>();
    }
    ensureSpriteGeometry();
    if (!floorShader || floorShader->ID == 0 ||
        !meshShader || meshShader->ID == 0 ||
        !spriteShader || spriteShader->ID == 0 ||
        spriteVAO == 0 || spriteVBO == 0 ||
        !skybox) {
        error = "TM renderer failed to initialize shaders/resources";
        return false;
    }

    return true;
}

bool TMOpenGLRenderer::renderToTarget(RenderTarget& target,
                                      RenderTarget& lowResTarget,
                                      TMRenderer& renderer,
                                      const TMRenderContext& context,
                                      const TMScene& scene,
                                      BackendStats& backendStats,
                                      TMRenderer::RenderStats& outStats,
                                      std::string& error) {
    error.clear();
    outStats = TMRenderer::RenderStats();
    backendStats = BackendStats();

    if (context.viewportWidth <= 0 || context.viewportHeight <= 0) {
        error = "TM renderer received an invalid viewport size";
        return false;
    }

    if (!ensureInitialized(error)) {
        return false;
    }
    ensureRenderTarget(target, context.viewportWidth, context.viewportHeight);
    if (target.fbo == 0 || target.texture == 0) {
        error = "TM renderer failed to allocate render target";
        return false;
    }

    TMRenderContext drawContext = context;
    RenderTarget* drawTarget = &target;
    if (presentationSettings.fake3DEnabled) {
        const int internalHeight = std::clamp(presentationSettings.fake3DInternalHeight, 64,
                                              std::max(64, context.viewportHeight));
        const float aspect = static_cast<float>(context.viewportWidth) /
                             static_cast<float>(std::max(1, context.viewportHeight));
        const int internalWidth = std::max(64, static_cast<int>(std::lround(internalHeight * aspect)));
        ensureRenderTarget(lowResTarget, internalWidth, internalHeight);
        if (lowResTarget.fbo != 0 && lowResTarget.texture != 0) {
            drawTarget = &lowResTarget;
            drawContext.viewportWidth = lowResTarget.width;
            drawContext.viewportHeight = lowResTarget.height;
        }
    } else {
        destroyRenderTarget(lowResTarget);
    }

    activeTarget = drawTarget;
    activeContext = &drawContext;
    activeBackendStats = &backendStats;

    const bool ok = renderer.render(drawContext, scene, *this, outStats, error);

    activeTarget = nullptr;
    activeContext = nullptr;
    activeBackendStats = nullptr;

    if (drawTarget == &lowResTarget) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, lowResTarget.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target.fbo);
        glBlitFramebuffer(0, 0, lowResTarget.width, lowResTarget.height,
                          0, 0, target.width, target.height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    return ok;
}

void TMOpenGLRenderer::ensureRenderTarget(RenderTarget& target, int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (target.fbo != 0 && target.width == width && target.height == height) {
        return;
    }

    destroyRenderTarget(target);

    glGenFramebuffers(1, &target.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);

    glGenTextures(1, &target.texture);
    glBindTexture(GL_TEXTURE_2D, target.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);

    glGenRenderbuffers(1, &target.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, target.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, target.depth);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        destroyRenderTarget(target);
    } else {
        target.width = width;
        target.height = height;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

void TMOpenGLRenderer::destroyRenderTarget(RenderTarget& target) {
    if (target.depth != 0) {
        glDeleteRenderbuffers(1, &target.depth);
        target.depth = 0;
    }
    if (target.texture != 0) {
        glDeleteTextures(1, &target.texture);
        target.texture = 0;
    }
    if (target.fbo != 0) {
        glDeleteFramebuffers(1, &target.fbo);
        target.fbo = 0;
    }
    target.width = 0;
    target.height = 0;
}

void TMOpenGLRenderer::ensureQuad() {
    if (quadVAO != 0 && quadVBO != 0) {
        return;
    }

    static constexpr float kQuadVertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 1.0f
    };

    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVertices), kQuadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void TMOpenGLRenderer::destroyQuad() {
    if (quadVBO != 0) {
        glDeleteBuffers(1, &quadVBO);
        quadVBO = 0;
    }
    if (quadVAO != 0) {
        glDeleteVertexArrays(1, &quadVAO);
        quadVAO = 0;
    }
}

void TMOpenGLRenderer::ensureSpriteGeometry() {
    if (spriteVAO != 0 && spriteVBO != 0) {
        return;
    }

    glGenVertexArrays(1, &spriteVAO);
    glGenBuffers(1, &spriteVBO);
    glBindVertexArray(spriteVAO);
    glBindBuffer(GL_ARRAY_BUFFER, spriteVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(SpriteVertex) * 6, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SpriteVertex),
                          reinterpret_cast<void*>(offsetof(SpriteVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVertex),
                          reinterpret_cast<void*>(offsetof(SpriteVertex, uv)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(SpriteVertex),
                          reinterpret_cast<void*>(offsetof(SpriteVertex, color)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void TMOpenGLRenderer::destroySpriteGeometry() {
    if (spriteVBO != 0) {
        glDeleteBuffers(1, &spriteVBO);
        spriteVBO = 0;
    }
    if (spriteVAO != 0) {
        glDeleteVertexArrays(1, &spriteVAO);
        spriteVAO = 0;
    }
}

bool TMOpenGLRenderer::ensureFallbackTexture() {
    if (fallbackWhiteTexture != 0) {
        return true;
    }

    static constexpr unsigned char kWhitePixel[4] = {255, 255, 255, 255};
    glGenTextures(1, &fallbackWhiteTexture);
    glBindTexture(GL_TEXTURE_2D, fallbackWhiteTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kWhitePixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    return fallbackWhiteTexture != 0;
}

void TMOpenGLRenderer::destroyFallbackTexture() {
    if (fallbackWhiteTexture != 0) {
        glDeleteTextures(1, &fallbackWhiteTexture);
        fallbackWhiteTexture = 0;
    }
}

Texture* TMOpenGLRenderer::getTexture(const std::string& path, TMTextureFilter filter) {
    if (path.empty()) {
        return nullptr;
    }

    const std::string cacheKey = BuildTextureCacheKey(path, filter);
    const auto it = textureCache.find(cacheKey);
    if (it != textureCache.end()) {
        return it->second.get();
    }

    auto texture = std::make_unique<Texture>(path, GL_REPEAT, GL_REPEAT,
                                             ToGLMinFilter(filter),
                                             ToGLMagFilter(filter));
    if (!texture || texture->GetID() == 0) {
        return nullptr;
    }

    Texture* result = texture.get();
    textureCache.emplace(cacheKey, std::move(texture));
    return result;
}

unsigned int TMOpenGLRenderer::resolveTextureId(const std::string& texturePath,
                                                TMTextureFilter filter,
                                                const std::string& sourceMeshPath) {
    const std::string resolvedPath = ResolveTexturePath(texturePath, sourceMeshPath);
    Texture* texture = getTexture(resolvedPath, filter);
    if (texture && texture->GetID() != 0) {
        return texture->GetID();
    }
    return fallbackWhiteTexture;
}

TMTextureFilter TMOpenGLRenderer::applyTextureFilterPolicy(TMTextureFilter filter) const {
    if (presentationSettings.fake3DEnabled && presentationSettings.fake3DPointSampling) {
        return TMTextureFilter::Point;
    }
    return filter;
}

float TMOpenGLRenderer::getPresentationPitchDegrees() const {
    if (activeContext == nullptr) {
        return 0.0f;
    }

    const glm::vec3 forward = (glm::dot(activeContext->cameraForward, activeContext->cameraForward) > 1e-6f)
                                  ? glm::normalize(activeContext->cameraForward)
                                  : glm::vec3(0.0f, 0.0f, -1.0f);
    const float rawPitchDegrees = glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)));
    const float minPitch = std::min(presentationSettings.presentationPitchMinDegrees,
                                    presentationSettings.presentationPitchMaxDegrees);
    const float maxPitch = std::max(presentationSettings.presentationPitchMinDegrees,
                                    presentationSettings.presentationPitchMaxDegrees);
    return std::clamp(rawPitchDegrees, minPitch, maxPitch);
}

void TMOpenGLRenderer::beginFrame(const TMRenderContext& context) {
    if (activeTarget == nullptr) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, activeTarget->fbo);
    glViewport(0, 0, context.viewportWidth, context.viewportHeight);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(0.09f, 0.11f, 0.16f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (skybox) {
        skybox->setTimeOfDay(context.skyboxTimeOfDay);
        if (!activeSkyboxSettingsInitialized ||
            !SkyboxSettingsEqual(activeSkyboxSettings, context.skyboxSettings)) {
            activeSkyboxSettings = context.skyboxSettings;
            activeSkyboxSettingsInitialized = true;
            skybox->setSettings(activeSkyboxSettings);
        }
        skybox->draw(glm::value_ptr(context.view), glm::value_ptr(context.projection));
    }
}

void TMOpenGLRenderer::drawM7Floor(const M7FloorDrawCommand& command) {
    if (activeContext == nullptr || activeTarget == nullptr || floorShader == nullptr || floorShader->ID == 0) {
        return;
    }

    const glm::mat4 viewProjection = activeContext->projection * activeContext->view;
    const glm::mat4 inverseViewProjection = glm::inverse(viewProjection);
    const unsigned int textureId = resolveTextureId(command.floorSurface.texturePath,
                                                    applyTextureFilterPolicy(command.floorSurface.textureFilter));

    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    floorShader->use();
    floorShader->setMat4("u_inverseViewProjection", inverseViewProjection);
    floorShader->setMat4("u_viewProjection", viewProjection);
    floorShader->setVec3("u_cameraPosition", activeContext->cameraPosition);
    floorShader->setVec2("u_boundsMinXZ", glm::vec2(command.boundsMin.x, command.boundsMin.z));
    floorShader->setVec2("u_boundsMaxXZ", glm::vec2(command.boundsMax.x, command.boundsMax.z));
    floorShader->setVec2("u_uvScale", command.floorSurface.uvScale);
    floorShader->setInt("u_uvMode", static_cast<int>(command.floorSurface.uvMode));
    floorShader->setVec2("u_tileWorldSize", glm::max(command.floorSurface.tileWorldSize, glm::vec2(0.0001f)));
    floorShader->setBool("u_extendToHorizon", command.floorSurface.extendToHorizon);
    floorShader->setFloat("u_floorHeight", command.floorSurface.height);
    floorShader->setFloat("u_maxDistance", std::max(1.0f, command.floorSurface.maxDistance));
    floorShader->setFloat("u_perspectiveStrength", std::max(0.1f, command.floorSurface.perspectiveStrength));
    floorShader->setFloat("u_horizonOffset", command.floorSurface.horizonOffset);
    floorShader->setBool("u_hasTexture", !command.floorSurface.texturePath.empty() && textureId != fallbackWhiteTexture);
    floorShader->setInt("u_floorTexture", 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (activeBackendStats != nullptr) {
        ++activeBackendStats->floorDraws;
    }
}

void TMOpenGLRenderer::drawSectorModel(const SectorModelDrawCommand& command) {
    if (activeContext == nullptr || meshShader == nullptr || meshShader->ID == 0 || command.mesh == nullptr) {
        return;
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    meshShader->use();
    meshShader->setMat4("u_view", activeContext->view);
    meshShader->setMat4("u_projection", activeContext->projection);
    meshShader->setMat4("u_model", command.modelMatrix);
    meshShader->setVec3("u_cameraPosition", activeContext->cameraPosition);
    meshShader->setVec3("u_lightDirection", glm::normalize(glm::vec3(-0.45f, 0.85f, -0.30f)));
    meshShader->setFloat("u_time", static_cast<float>(Modularity::Platform::GetTimeSeconds()));
    meshShader->setFloat("u_presentationPitchDegrees", getPresentationPitchDegrees());
    meshShader->setFloat("u_pitchNormalizeDegrees",
                         std::max(1.0f, std::max(std::abs(presentationSettings.presentationPitchMinDegrees),
                                                 std::abs(presentationSettings.presentationPitchMaxDegrees))));
    meshShader->setFloat("u_pitchCurve", std::max(0.01f, presentationSettings.lookPitchCurve));
    meshShader->setFloat("u_pitchDepthRange", std::max(0.001f, presentationSettings.lookPitchDepthRange));
    meshShader->setFloat("u_pitchStretchStrength",
                         presentationSettings.lookPitchStretchEnabled
                             ? std::max(0.0f, presentationSettings.lookPitchStretchStrength)
                             : 0.0f);
    meshShader->setFloat("u_pitchCompressStrength",
                         presentationSettings.lookPitchStretchEnabled
                             ? std::max(0.0f, presentationSettings.lookPitchCompressStrength)
                             : 0.0f);
    meshShader->setFloat("u_pitchShearStrength",
                         presentationSettings.lookPitchStretchEnabled
                             ? std::max(0.0f, presentationSettings.lookPitchShearStrength)
                             : 0.0f);
    meshShader->setBool("u_presentationSnapEnabled", presentationSettings.presentationSnapEnabled);
    meshShader->setFloat("u_presentationSnapStep", std::max(0.0f, presentationSettings.presentationSnapStep));
    meshShader->setBool("u_cameraRelativeSnapEnabled", presentationSettings.cameraRelativeSnapEnabled);
    meshShader->setFloat("u_cameraRelativeSnapStep", std::max(0.0f, presentationSettings.cameraRelativeSnapStep));
    meshShader->setBool("u_vertexSnapEnabled", presentationSettings.vertexSnapEnabled);
    meshShader->setFloat("u_vertexSnapStep", std::max(0.0f, presentationSettings.vertexSnapStep));
    meshShader->setBool("u_screenSnapEnabled", presentationSettings.screenSnapEnabled);
    meshShader->setFloat("u_screenSnapStep", std::max(0.0f, presentationSettings.screenSnapStep));
    meshShader->setVec2("u_viewportSize", glm::vec2(static_cast<float>(std::max(1, activeContext->viewportWidth)),
                                                    static_cast<float>(std::max(1, activeContext->viewportHeight))));
    meshShader->setInt("u_albedoTexture", 0);
    const bool fake3D = presentationSettings.fake3DEnabled;
    meshShader->setBool("u_flatShadingEnabled", fake3D && presentationSettings.fake3DFlatShading);
    meshShader->setInt("u_shadeLevels",
                       fake3D ? std::clamp(presentationSettings.fake3DShadeLevels, 0, 16) : 0);

    for (const MMeshRenderSubmesh& submesh : command.mesh->submeshes) {
        if (!submesh.mesh) {
            continue;
        }

        const std::string& texturePath =
            command.textureOverridePath.empty() ? submesh.albedoTexturePath : command.textureOverridePath;
        const TMTextureFilter textureFilter = applyTextureFilterPolicy(
            (command.presentation.textureFilter == TMTextureFilter::Point ||
             submesh.presentation.textureFilter == TMTextureFilter::Point)
                ? TMTextureFilter::Point
                : TMTextureFilter::Bilinear);
        const unsigned int textureId = resolveTextureId(texturePath, textureFilter, command.mesh->sourcePath);
        const glm::vec4 tint = CombineColorTint(command.presentation.colorTint, submesh.presentation.colorTint);
        const bool forceAffine = fake3D && presentationSettings.fake3DAffineTextures;
        const bool affineWarpEnabled = forceAffine ||
            (presentationSettings.textureWarpEnabled &&
             (command.presentation.affineTextureWarpEnabled || submesh.presentation.affineTextureWarpEnabled));
        const float affineWarpStrength = forceAffine
            ? std::clamp(presentationSettings.fake3DAffineStrength, 0.0f, 1.0f)
            : std::clamp(
                  std::max(command.presentation.affineTextureWarpStrength,
                           submesh.presentation.affineTextureWarpStrength),
                  0.0f, 1.0f);
        const glm::vec2 uvTiling = command.presentation.uvTiling * submesh.presentation.uvTiling;
        const glm::vec2 uvOffset = command.presentation.uvOffset + submesh.presentation.uvOffset;
        const bool wobbleEnabled = presentationSettings.vertexWobbleEnabled && command.wobble.enabled;
        const float wobbleStrength = wobbleEnabled
                                         ? std::max(0.0f, command.wobble.strength) *
                                               std::max(0.0f, presentationSettings.wobbleStrength)
                                         : 0.0f;
        const float wobbleSpeed = wobbleEnabled
                                      ? std::max(0.0f, command.wobble.speed) *
                                            std::max(0.0f, presentationSettings.wobbleSpeed)
                                      : 0.0f;

        meshShader->setBool("u_hasTexture", textureId != fallbackWhiteTexture);
        meshShader->setVec4("u_colorTint", tint);
        meshShader->setVec2("u_uvTiling", uvTiling);
        meshShader->setVec2("u_uvOffset", uvOffset);
        meshShader->setBool("u_affineWarpEnabled", affineWarpEnabled);
        meshShader->setFloat("u_affineWarpStrength", affineWarpStrength);
        meshShader->setBool("u_wobbleEnabled", wobbleEnabled);
        meshShader->setFloat("u_wobbleStrength", wobbleStrength);
        meshShader->setFloat("u_wobbleSpeed", wobbleSpeed);
        meshShader->setFloat("u_wobbleSeed", command.wobble.seed);
        meshShader->setVec3("u_wobbleOffset", command.wobble.offset);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textureId);
        submesh.mesh->draw();
        if (activeBackendStats != nullptr) {
            ++activeBackendStats->submeshDraws;
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    if (activeBackendStats != nullptr) {
        ++activeBackendStats->modelDraws;
    }
}

void TMOpenGLRenderer::drawSprite25D(const Sprite25DDrawCommand& command) {
    if (activeContext == nullptr || spriteShader == nullptr || spriteShader->ID == 0 ||
        spriteVAO == 0 || spriteVBO == 0) {
        return;
    }

    const TMSprite25DInstance& sprite = command.sprite;
    const glm::mat4 inverseView = glm::inverse(activeContext->view);
    const glm::vec3 cameraRight = glm::normalize(glm::vec3(inverseView[0]));
    const glm::vec3 cameraUp = glm::normalize(glm::vec3(inverseView[1]));
    const glm::vec3 cameraForward =
        (glm::dot(activeContext->cameraForward, activeContext->cameraForward) > 1e-6f)
            ? glm::normalize(activeContext->cameraForward)
            : glm::vec3(0.0f, 0.0f, -1.0f);

    const float angle = glm::radians(sprite.rotationDegrees);
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    auto corner = [&](float x, float y) {
        const float rx = x * c - y * s;
        const float ry = x * s + y * c;
        return sprite.position + cameraRight * rx + cameraUp * ry;
    };

    const glm::vec4 uv = sprite.uvRect;
    const float u0 = uv.x;
    const float v0 = uv.y;
    const float u1 = uv.x + uv.z;
    const float v1 = uv.y + uv.w;
    const glm::vec3 p0 = corner(-sprite.halfExtents.x, -sprite.halfExtents.y);
    const glm::vec3 p1 = corner( sprite.halfExtents.x, -sprite.halfExtents.y);
    const glm::vec3 p2 = corner( sprite.halfExtents.x,  sprite.halfExtents.y);
    const glm::vec3 p3 = corner(-sprite.halfExtents.x,  sprite.halfExtents.y);
    const SpriteVertex vertices[6] = {
        {p0, glm::vec2(u0, v1), sprite.colorTint},
        {p1, glm::vec2(u1, v1), sprite.colorTint},
        {p2, glm::vec2(u1, v0), sprite.colorTint},
        {p0, glm::vec2(u0, v1), sprite.colorTint},
        {p2, glm::vec2(u1, v0), sprite.colorTint},
        {p3, glm::vec2(u0, v0), sprite.colorTint}
    };

    const unsigned int textureId = resolveTextureId(sprite.texturePath,
                                                    applyTextureFilterPolicy(sprite.textureFilter));

    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    spriteShader->use();
    spriteShader->setMat4("u_view", activeContext->view);
    spriteShader->setMat4("u_projection", activeContext->projection);
    spriteShader->setVec3("u_cameraForward", cameraForward);
    spriteShader->setInt("u_spriteTexture", 0);
    spriteShader->setBool("u_hasTexture", !sprite.texturePath.empty() && textureId != fallbackWhiteTexture);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glBindVertexArray(spriteVAO);
    glBindBuffer(GL_ARRAY_BUFFER, spriteVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    if (activeBackendStats != nullptr) {
        ++activeBackendStats->spriteDraws;
    }
}

void TMOpenGLRenderer::endFrame() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace Modularity::Render25D
