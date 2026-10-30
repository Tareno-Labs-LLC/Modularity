#include "Rendering.h"
#include "RenderOptimizationInternal.h"
#include "Profiler.h"
#include "Modu2DStats.h"
#include "Camera.h"
#include "ModelLoader.h"
#include "../include/Platform/AssetSource.h"
#include "HardwareProfile.h"
#include "../include/Textures/TextureTranscode.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <future>
#include <optional>
#include <limits>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#define TINYOBJLOADER_IMPLEMENTATION
#include "../include/ThirdParty/tiny_obj_loader.h"

extern float vertices[288];
extern float mirrorPlaneVertices[48];

namespace {
struct Runtime2DRenderCounters {
    uint64_t textureBindCount = 0;
    uint64_t stateBindCount = 0;
};

Runtime2DRenderCounters gRuntime2DRenderCounters;

inline void Runtime2DCountTextureBind() {
    ++gRuntime2DRenderCounters.textureBindCount;
}

inline void Runtime2DCountStateBind() {
    ++gRuntime2DRenderCounters.stateBindCount;
}

constexpr int kUiAtlasPadding = 2;

uint64_t BuildUiAtlasKey(int layer, MaterialProperties::TextureFilter filter) {
    const uint64_t layerBits = static_cast<uint64_t>(static_cast<uint32_t>(layer));
    const uint64_t filterBits = static_cast<uint64_t>(filter == MaterialProperties::TextureFilter::Point ? 1u : 0u);
    return (layerBits << 32) | filterBits;
}

GLint ToGLTextureFilter(MaterialProperties::TextureFilter filter) {
    return filter == MaterialProperties::TextureFilter::Point ? GL_NEAREST : GL_LINEAR;
}

int NextPowerOfTwoAtLeast(int value) {
    int result = 1;
    while (result < value && result < 16384) {
        result <<= 1;
    }
    return result;
}

glm::vec4 BuildUiTargetUvTransform(int x, int y, int width, int height, int textureWidth, int textureHeight) {
    const float invW = 1.0f / static_cast<float>(std::max(1, textureWidth));
    const float invH = 1.0f / static_cast<float>(std::max(1, textureHeight));
    const float safeW = static_cast<float>(std::max(1, width));
    const float safeH = static_cast<float>(std::max(1, height));
    const float uOffset = (static_cast<float>(x) + 0.5f) * invW;
    const float vOffset = 1.0f - (static_cast<float>(y) + safeH - 0.5f) * invH;
    const float uScale = std::max(1.0f, safeW - 1.0f) * invW;
    const float vScale = std::max(1.0f, safeH - 1.0f) * invH;
    return glm::vec4(uScale, vScale, uOffset, vOffset);
}

inline void SetDepthOnlyFramebufferDrawBuffer()
{
#if MODULARITY_OPENGL_ES
    const GLenum noColorAttachments = GL_NONE;
    glDrawBuffers(1, &noColorAttachments);
#else
    glDrawBuffer(GL_NONE);
#endif
}

glm::vec4 BuildSpriteUvRect(const SceneObject& obj) {
    if (obj.ui.spriteCustomFramesEnabled &&
        !obj.ui.spriteCustomFrames.empty() &&
        obj.ui.spriteSourceWidth > 0 &&
        obj.ui.spriteSourceHeight > 0) {
        const int frame = std::clamp(obj.ui.spriteSheetFrame, 0, static_cast<int>(obj.ui.spriteCustomFrames.size()) - 1);
        const glm::ivec4 rect = obj.ui.spriteCustomFrames[frame];
        const float invW = 1.0f / static_cast<float>(obj.ui.spriteSourceWidth);
        const float invH = 1.0f / static_cast<float>(obj.ui.spriteSourceHeight);
        const float u0 = rect.x * invW;
        const float vBottom = 1.0f - static_cast<float>(rect.y + rect.w) * invH;
        return glm::vec4(u0, vBottom, rect.z * invW, rect.w * invH);
    }

    if (!obj.ui.spriteSheetEnabled) {
        return glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    }

    const int columns = std::max(1, obj.ui.spriteSheetColumns);
    const int rows = std::max(1, obj.ui.spriteSheetRows);
    const int total = std::max(1, columns * rows);
    const int frame = std::clamp(obj.ui.spriteSheetFrame, 0, total - 1);
    const int col = frame % columns;
    const int row = frame / columns;

    const float u0 = static_cast<float>(col) / static_cast<float>(columns);
    const float v0 = 1.0f - static_cast<float>(row + 1) / static_cast<float>(rows);
    const float uSize = 1.0f / static_cast<float>(columns);
    const float vSize = 1.0f / static_cast<float>(rows);
    return glm::vec4(u0, v0, uSize, vSize);
}

bool HasDefaultSpriteUvRect(const SceneObject& obj) {
    const glm::vec4 uvRect = BuildSpriteUvRect(obj);
    return std::abs(uvRect.x) <= 1e-5f &&
           std::abs(uvRect.y) <= 1e-5f &&
           std::abs(uvRect.z - 1.0f) <= 1e-5f &&
           std::abs(uvRect.w - 1.0f) <= 1e-5f;
}

bool HasMeaningfulSpriteFrameScales(const UIElementComponent& ui) {
    if (ui.spriteCustomFrameScales.size() != ui.spriteCustomFrames.size() ||
        ui.spriteCustomFrameScales.empty()) {
        return false;
    }
    for (const glm::vec2& scale : ui.spriteCustomFrameScales) {
        if (std::abs(scale.x - 1.0f) > 0.0001f ||
            std::abs(scale.y - 1.0f) > 0.0001f) {
            return true;
        }
    }
    return false;
}

glm::vec2 ResolveSpriteFrameScale(const UIElementComponent& ui) {
    if (!ui.spriteCustomFramesEnabled || ui.spriteCustomFrames.empty()) {
        return glm::vec2(1.0f);
    }

    const int frameCount = static_cast<int>(ui.spriteCustomFrames.size());
    const int frame = std::clamp(ui.spriteSheetFrame, 0, frameCount - 1);
    if (HasMeaningfulSpriteFrameScales(ui)) {
        const glm::vec2 authored = ui.spriteCustomFrameScales[static_cast<size_t>(frame)];
        return glm::vec2(std::max(0.01f, authored.x), std::max(0.01f, authored.y));
    }

    const glm::ivec4& referenceRect = ui.spriteCustomFrames.front();
    const glm::ivec4& frameRect = ui.spriteCustomFrames[static_cast<size_t>(frame)];
    const float referenceWidth = static_cast<float>(std::max(1, referenceRect.z));
    const float referenceHeight = static_cast<float>(std::max(1, referenceRect.w));
    return glm::vec2(static_cast<float>(std::max(1, frameRect.z)) / referenceWidth,
                     static_cast<float>(std::max(1, frameRect.w)) / referenceHeight);
}

bool IsMaskedSprite25DUiLayerObject(const SceneObject& obj) {
    return obj.type == ObjectType::Sprite25D &&
           obj.hasUI &&
           (obj.ui.type == UIElementType::Image || obj.ui.type == UIElementType::Sprite2D);
}

bool ProjectSprite25DProxyHalfSizeNdc(const SceneObject& obj,
                                      const glm::mat4& view,
                                      const glm::mat4& proj,
                                      float viewportWidth,
                                      float viewportHeight,
                                      glm::vec2& outHalfSizeNdc) {
    const glm::mat4 invView = glm::inverse(view);
    glm::vec3 cameraRight = glm::vec3(invView[0]);
    glm::vec3 cameraUp = glm::vec3(invView[1]);
    if (glm::dot(cameraRight, cameraRight) <= 1e-6f ||
        glm::dot(cameraUp, cameraUp) <= 1e-6f) {
        return false;
    }
    cameraRight = glm::normalize(cameraRight);
    cameraUp = glm::normalize(cameraUp);

    const glm::vec2 frameScale = ResolveSpriteFrameScale(obj.ui);
    const glm::vec2 objectScale = glm::max(glm::abs(glm::vec2(obj.scale.x, obj.scale.y)), glm::vec2(0.01f));
    const glm::vec2 baseSize = glm::max(obj.ui.size, glm::vec2(0.01f)) * frameScale * objectScale;
    const glm::vec2 worldHalfExtents = baseSize * 0.005f;

    auto projectNdc = [&](const glm::vec3& point, glm::vec3& outNdc) {
        const glm::vec4 clip = proj * view * glm::vec4(point, 1.0f);
        if (clip.w <= 0.0f || std::abs(clip.w) <= 1e-5f) {
            return false;
        }
        outNdc = glm::vec3(clip) / clip.w;
        return true;
    };

    glm::vec3 centerNdc(0.0f);
    glm::vec3 rightNdc(0.0f);
    glm::vec3 upNdc(0.0f);
    if (!projectNdc(obj.position, centerNdc) ||
        !projectNdc(obj.position + cameraRight * worldHalfExtents.x, rightNdc) ||
        !projectNdc(obj.position + cameraUp * worldHalfExtents.y, upNdc)) {
        return false;
    }

    const float minHalfWidthNdc = 1.0f / std::max(1.0f, viewportWidth);
    const float minHalfHeightNdc = 1.0f / std::max(1.0f, viewportHeight);
    outHalfSizeNdc = glm::vec2(std::max(minHalfWidthNdc, std::abs(rightNdc.x - centerNdc.x)),
                               std::max(minHalfHeightNdc, std::abs(upNdc.y - centerNdc.y)));
    return true;
}

glm::vec4 BuildSurfaceUvTransform(const SceneObject* obj, const MaterialProperties& material) {
    glm::vec2 tiling = material.uvTiling;
    glm::vec2 offset = material.uvOffset;

    if (obj != nullptr &&
        obj->runtimeHasAlbedoTextureOverride &&
        obj->runtimeAlbedoTextureOverrideId != 0 &&
        obj->runtimeAlbedoTextureFlipX) {
        offset.x += tiling.x;
        tiling.x = -tiling.x;
    }
    if (obj != nullptr &&
        obj->runtimeHasAlbedoTextureOverride &&
        obj->runtimeAlbedoTextureOverrideId != 0 &&
        obj->runtimeAlbedoTextureFlipY) {
        offset.y += tiling.y;
        tiling.y = -tiling.y;
    }

    return glm::vec4(tiling, offset);
}

glm::mat4 BuildSceneObjectModelMatrix(const SceneObject& obj, const glm::vec3* cameraPosition = nullptr, const glm::vec3* cameraUp = nullptr) {
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, obj.position);

    if (obj.renderType == RenderType::Sprite && obj.faceCamera && cameraPosition != nullptr) {
        glm::vec3 forward = *cameraPosition - obj.position;
        if (glm::dot(forward, forward) < 1e-6f) {
            forward = glm::vec3(0.0f, 0.0f, 1.0f);
        } else {
            forward = glm::normalize(forward);
        }

        glm::vec3 up = (cameraUp != nullptr && glm::dot(*cameraUp, *cameraUp) > 1e-6f)
            ? glm::normalize(*cameraUp)
            : glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 right = glm::cross(up, forward);
        if (glm::dot(right, right) < 1e-6f) {
            up = glm::vec3(0.0f, 0.0f, 1.0f);
            right = glm::cross(up, forward);
        }
        right = glm::normalize(right);
        up = glm::normalize(glm::cross(forward, right));

        const glm::vec3 signedScale = obj.scale;
        const glm::vec3 absScale = glm::max(glm::abs(signedScale), glm::vec3(0.0001f));
        model[0] = glm::vec4(right * absScale.x * (signedScale.x < 0.0f ? -1.0f : 1.0f), 0.0f);
        model[1] = glm::vec4(up * absScale.y * (signedScale.y < 0.0f ? -1.0f : 1.0f), 0.0f);
        model[2] = glm::vec4(forward * absScale.z * (signedScale.z < 0.0f ? -1.0f : 1.0f), 0.0f);
        return model;
    }

    model = glm::rotate(model, glm::radians(obj.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
    model = glm::rotate(model, glm::radians(obj.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, glm::radians(obj.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
    model = glm::scale(model, obj.scale);
    return model;
}

PostFXSettings MakeNeutralPostFXSettings() {
    PostFXSettings settings;
    settings.enabled = false;
    settings.isGlobal = true;
    settings.priority = 0.0f;
    settings.blendWeight = 1.0f;
    settings.blendRadius = 4.0f;
    settings.hdrEnabled = true;
    settings.toneMapper = PostFXToneMapper::ACES;
    settings.whitePoint = 4.0f;
    settings.gamma = 2.2f;
    settings.bloomEnabled = false;
    settings.bloomThreshold = 1.0f;
    settings.bloomSoftKnee = 0.25f;
    settings.bloomIntensity = 0.0f;
    settings.bloomRadius = 1.0f;
    settings.colorAdjustEnabled = false;
    settings.exposure = 0.0f;
    settings.contrast = 1.0f;
    settings.saturation = 1.0f;
    settings.colorFilter = glm::vec3(1.0f);
    settings.motionBlurEnabled = false;
    settings.motionBlurStrength = 0.0f;
    settings.motionBlurThreshold = 0.04f;
    settings.motionBlurClamp = 0.35f;
    settings.vignetteEnabled = false;
    settings.lensFlareEnabled = false;
    settings.vignetteIntensity = 0.0f;
    settings.vignetteSmoothness = 0.25f;
    settings.chromaticAberrationEnabled = false;
    settings.chromaticAmount = 0.0f;
    settings.sharpenEnabled = false;
    settings.sharpenStrength = 0.0f;
    settings.ambientOcclusionEnabled = false;
    settings.aoRadius = 0.0035f;
    settings.aoStrength = 0.0f;
    settings.ditherEnabled = false;
    settings.ditherIntensity = 0.0f;
    settings.ditherColorBits = 8;
    settings.ditherDarkAdjustment = 0.0f;
    settings.ditherPixelation = 0.0f;
    settings.ditherSize = 1.0f;
    settings.ditherContrast = 0.35f;
    settings.ditherOffset = 0.0f;
    settings.ditherPalette = PostFXDitherPalette::FullColor;
    settings.ditherPattern = PostFXDitherPattern::HybridPS1;
    settings.staticEnabled = false;
    settings.staticIntensity = 0.0f;
    settings.staticGrainScale = 1.0f;
    settings.staticDarkAreaInfluence = 0.0f;
    settings.staticSpeed = 0.0f;
    settings.staticMonochrome = false;
    settings.staticSparkle = 0.0f;
    settings.staticDistortionEnabled = false;
    settings.staticDistortionHorizontalJitterAmount = 0.0f;
    settings.staticDistortionLineDensity = 128.0f;
    settings.staticDistortionGlitchFrequency = 0.0f;
    settings.staticDistortionStrength = 0.0f;
    settings.lensDistortionEnabled = false;
    settings.lensDistortionAmount = 0.0f;
    settings.lensDistortionEdgeFalloff = 0.75f;
    settings.lensDistortionCenterOffset = glm::vec2(0.0f);
    settings.lensDistortionEdgeVignetteEnabled = false;
    settings.lensDistortionEdgeVignetteIntensity = 0.0f;
    settings.lensDistortionEdgeVignetteRadius = 0.9f;
    settings.lensDistortionEdgeVignetteSoftness = 0.25f;
    settings.lensDistortionEdgeVignetteColor = glm::vec3(0.0f);
    settings.pixelationEnabled = false;
    settings.pixelationSize = 1.0f;
    settings.posterizeEnabled = false;
    settings.posterizeLevels = 8;
    settings.scanlinesEnabled = false;
    settings.scanlinesIntensity = 0.0f;
    settings.scanlinesDensity = 1.0f;
    settings.scanlinesSpeed = 0.0f;
    settings.vhsOverlayEnabled = false;
    settings.vhsOverlayOpacity = 0.0f;
    settings.vhsOverlayScanlineStrength = 0.0f;
    settings.vhsOverlayTapeNoise = 0.0f;
    settings.vhsOverlayChromaBleed = 0.0f;
    settings.vhsOverlayBottomNoiseBandHeight = 0.0f;
    settings.vhsOverlayBottomNoiseBandIntensity = 0.0f;
    settings.vhsOverlayDistortionStrength = 0.0f;
    settings.vhsOverlayAnimationSpeed = 0.0f;
    settings.vhsOverlayColorBleed = 0.0f;
    settings.vhsOverlayBanding = 0.0f;
    settings.vhsOverlaySignalMode = PostFXVhsSignalMode::VhsSP;
    settings.vhsOverlayDropouts = 0.0f;
    settings.wavyEnabled = false;
    settings.wavyAmplitude = 0.0f;
    settings.wavyFrequency = 16.0f;
    settings.wavySpeed = 0.0f;
    settings.wavyVertical = false;
    return settings;
}

void AttenuatePostFXStrengths(PostFXSettings& settings, float weight) {
    const float t = glm::clamp(weight, 0.0f, 1.0f);
    settings.bloomIntensity *= t;
    settings.exposure *= t;
    settings.contrast = glm::mix(1.0f, settings.contrast, t);
    settings.saturation = glm::mix(1.0f, settings.saturation, t);
    settings.colorFilter = glm::mix(glm::vec3(1.0f), settings.colorFilter, t);
    settings.motionBlurStrength *= t;
    settings.vignetteIntensity *= t;
    settings.chromaticAmount *= t;
    settings.sharpenStrength *= t;
    settings.aoStrength *= t;

    settings.ditherIntensity *= t;
    settings.ditherColorBits = std::clamp(
        static_cast<int>(std::round(glm::mix(
            8.0f, static_cast<float>(settings.ditherColorBits), t))),
        1, 8);
    settings.ditherDarkAdjustment *= t;
    settings.ditherPixelation *= t;
    settings.ditherSize = glm::mix(1.0f, settings.ditherSize, t);
    settings.ditherContrast *= t;
    settings.ditherOffset *= t;

    settings.staticIntensity *= t;
    settings.staticDarkAreaInfluence *= t;
    settings.staticSparkle *= t;
    settings.staticDistortionHorizontalJitterAmount *= t;
    settings.staticDistortionGlitchFrequency *= t;
    settings.staticDistortionStrength *= t;

    settings.lensDistortionAmount *= t;
    settings.lensDistortionCenterOffset *= t;
    settings.lensDistortionEdgeVignetteIntensity *= t;
    settings.pixelationSize = glm::mix(1.0f, settings.pixelationSize, t);
    settings.posterizeLevels = std::clamp(
        static_cast<int>(std::round(glm::mix(
            64.0f, static_cast<float>(settings.posterizeLevels), t))),
        2, 64);
    settings.scanlinesIntensity *= t;

    settings.vhsOverlayOpacity *= t;
    settings.vhsOverlayScanlineStrength *= t;
    settings.vhsOverlayTapeNoise *= t;
    settings.vhsOverlayChromaBleed *= t;
    settings.vhsOverlayBottomNoiseBandHeight *= t;
    settings.vhsOverlayBottomNoiseBandIntensity *= t;
    settings.vhsOverlayDistortionStrength *= t;
    settings.vhsOverlayColorBleed *= t;
    settings.vhsOverlayBanding *= t;
    settings.vhsOverlayDropouts *= t;
    settings.wavyAmplitude *= t;
}

float ComputePostFXVolumeSpatialWeight(const SceneObject& obj, const glm::vec3& cameraPosition) {
    if (!obj.hasPostFX || !obj.postFx.enabled) return 0.0f;

    if (obj.postFx.isGlobal) return 1.0f;

    SceneObject safeObj = obj;
    safeObj.scale.x = (std::abs(safeObj.scale.x) < 0.001f) ? (safeObj.scale.x < 0.0f ? -0.001f : 0.001f) : safeObj.scale.x;
    safeObj.scale.y = (std::abs(safeObj.scale.y) < 0.001f) ? (safeObj.scale.y < 0.0f ? -0.001f : 0.001f) : safeObj.scale.y;
    safeObj.scale.z = (std::abs(safeObj.scale.z) < 0.001f) ? (safeObj.scale.z < 0.0f ? -0.001f : 0.001f) : safeObj.scale.z;
    glm::mat4 world = BuildSceneObjectModelMatrix(safeObj);
    glm::mat4 invWorld = glm::inverse(world);
    glm::vec3 local = glm::vec3(invWorld * glm::vec4(cameraPosition, 1.0f));
    glm::vec3 halfExtents(0.5f);
    glm::vec3 delta = glm::max(glm::abs(local) - halfExtents, glm::vec3(0.0f));
    float distance = glm::length(delta);
    if (!std::isfinite(distance)) return 0.0f;
    if (distance <= 0.0f) return 1.0f;

    float blendRadius = std::max(obj.postFx.blendRadius, 0.001f);
    float falloff = 1.0f - glm::clamp(distance / blendRadius, 0.0f, 1.0f);
    return falloff;
}

int CountEnabledPostEffects(const PostFXSettings& settings) {
    int count = 0;
    count += settings.hdrEnabled ? 1 : 0;
    count += settings.bloomEnabled ? 1 : 0;
    count += settings.colorAdjustEnabled ? 1 : 0;
    count += settings.motionBlurEnabled ? 1 : 0;
    count += settings.vignetteEnabled ? 1 : 0;
    count += settings.lensFlareEnabled ? 1 : 0;
    count += settings.chromaticAberrationEnabled ? 1 : 0;
    count += settings.sharpenEnabled ? 1 : 0;
    count += settings.ambientOcclusionEnabled ? 1 : 0;
    count += settings.ditherEnabled ? 1 : 0;
    count += settings.staticEnabled ? 1 : 0;
    count += settings.staticDistortionEnabled ? 1 : 0;
    count += settings.lensDistortionEnabled ? 1 : 0;
    count += settings.pixelationEnabled ? 1 : 0;
    count += settings.posterizeEnabled ? 1 : 0;
    count += settings.scanlinesEnabled ? 1 : 0;
    count += settings.vhsOverlayEnabled ? 1 : 0;
    count += settings.wavyEnabled ? 1 : 0;
    return count;
}

bool HasMeaningfulToneMapping(const PostFXSettings& settings) {
    constexpr float kWhitePointDefault = 4.0f;
    constexpr float kGammaDefault = 2.2f;
    if (settings.hdrEnabled) {
        return true;
    }
    return settings.toneMapper != PostFXToneMapper::ACES ||
           std::fabs(settings.whitePoint - kWhitePointDefault) > 0.0001f ||
           std::fabs(settings.gamma - kGammaDefault) > 0.0001f;
}

static inline bool moduRenderContextLive() {
#ifdef __ANDROID__
    return true;
#else
    return glfwGetCurrentContext() != nullptr;
#endif
}

struct FrustumPlanes {
    std::array<glm::vec4, 6> planes;
};

FrustumPlanes BuildFrustumPlanes(const glm::mat4& viewProjection) {
    FrustumPlanes frustum{};
    glm::mat4 rows = glm::transpose(viewProjection);
    frustum.planes[0] = rows[3] + rows[0];
    frustum.planes[1] = rows[3] - rows[0];
    frustum.planes[2] = rows[3] + rows[1];
    frustum.planes[3] = rows[3] - rows[1];
    frustum.planes[4] = rows[3] + rows[2];
    frustum.planes[5] = rows[3] - rows[2];

    for (glm::vec4& plane : frustum.planes) {
        float len = glm::length(glm::vec3(plane));
        if (len > 1e-5f) {
            plane /= len;
        }
    }

    return frustum;
}

bool SphereInsideFrustum(const FrustumPlanes& frustum, const glm::vec3& center, float radius) {
    for (const glm::vec4& plane : frustum.planes) {
        if (glm::dot(glm::vec3(plane), center) + plane.w < -radius) {
            return false;
        }
    }
    return true;
}

bool IsFiniteVec3(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

uint64_t HashStringFNV1a(const std::string& value) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : value) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool TryGetObjectLocalBounds(const SceneObject& obj, glm::vec3& outCenter, glm::vec3& outExtents) {
    outCenter = glm::vec3(0.0f);
    switch (obj.renderType) {
        case RenderType::Cube:
            outExtents = glm::vec3(0.5f);
            return true;
        case RenderType::Sphere:
            outExtents = glm::vec3(0.5f);
            return true;
        case RenderType::Capsule:
            outExtents = glm::vec3(0.25f, 0.75f, 0.25f);
            return true;
        case RenderType::Plane:
        case RenderType::Mirror:
        case RenderType::Sprite:
            outExtents = glm::vec3(0.5f, 0.5f, 0.05f);
            return true;
        case RenderType::Torus:
            outExtents = glm::vec3(0.5f, 0.15f, 0.5f);
            return true;
        case RenderType::OBJMesh:
            if (obj.meshId >= 0) {
                if (const auto* meshInfo = g_objLoader.getMeshInfo(obj.meshId)) {
                    if (IsFiniteVec3(meshInfo->boundsMin) && IsFiniteVec3(meshInfo->boundsMax)) {
                        outCenter = (meshInfo->boundsMin + meshInfo->boundsMax) * 0.5f;
                        outExtents = glm::max((meshInfo->boundsMax - meshInfo->boundsMin) * 0.5f,
                                              glm::vec3(0.001f));
                        return true;
                    }
                }
            }
            break;
        case RenderType::Model:
            if (obj.meshId >= 0) {
                if (const auto* meshInfo = getModelLoader().getMeshInfo(obj.meshId)) {
                    if (IsFiniteVec3(meshInfo->boundsMin) && IsFiniteVec3(meshInfo->boundsMax)) {
                        outCenter = (meshInfo->boundsMin + meshInfo->boundsMax) * 0.5f;
                        outExtents = glm::max((meshInfo->boundsMax - meshInfo->boundsMin) * 0.5f,
                                              glm::vec3(0.001f));
                        return true;
                    }
                }
            }
            break;
        case RenderType::None:
            break;
    }

    return false;
}

bool TryComputeObjectCullSphere(const SceneObject& obj, const glm::mat4& model,
                                glm::vec3& outCenter, float& outRadius) {
    glm::vec3 localCenter(0.0f);
    glm::vec3 localExtents(0.0f);
    if (!TryGetObjectLocalBounds(obj, localCenter, localExtents)) {
        return false;
    }

    outCenter = glm::vec3(model * glm::vec4(localCenter, 1.0f));
    glm::vec3 scaledExtents(
        localExtents.x * glm::length(glm::vec3(model[0])),
        localExtents.y * glm::length(glm::vec3(model[1])),
        localExtents.z * glm::length(glm::vec3(model[2])));
    outRadius = glm::length(scaledExtents);
    return std::isfinite(outRadius) && outRadius > 0.0f;
}

bool ProjectPlaneScreenRect(const SceneObject& obj, const glm::mat4& viewProjection,
                            int viewportWidth, int viewportHeight,
                            float& outMinX, float& outMinY, float& outMaxX, float& outMaxY) {
    const std::array<glm::vec3, 4> localCorners = {
        glm::vec3(-0.5f, -0.5f, 0.0f),
        glm::vec3( 0.5f, -0.5f, 0.0f),
        glm::vec3( 0.5f,  0.5f, 0.0f),
        glm::vec3(-0.5f,  0.5f, 0.0f)
    };

    glm::mat4 model = BuildSceneObjectModelMatrix(obj);
    bool anyProjected = false;
    outMinX = static_cast<float>(viewportWidth);
    outMinY = static_cast<float>(viewportHeight);
    outMaxX = 0.0f;
    outMaxY = 0.0f;

    for (const glm::vec3& corner : localCorners) {
        glm::vec4 clip = viewProjection * model * glm::vec4(corner, 1.0f);
        if (clip.w <= 0.0001f) {
            continue;
        }

        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        float screenX = (ndc.x * 0.5f + 0.5f) * static_cast<float>(viewportWidth);
        float screenY = (1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<float>(viewportHeight);
        outMinX = std::min(outMinX, screenX);
        outMinY = std::min(outMinY, screenY);
        outMaxX = std::max(outMaxX, screenX);
        outMaxY = std::max(outMaxY, screenY);
        anyProjected = true;
    }

    if (!anyProjected) {
        return false;
    }

    outMinX = std::clamp(outMinX, 0.0f, static_cast<float>(viewportWidth));
    outMinY = std::clamp(outMinY, 0.0f, static_cast<float>(viewportHeight));
    outMaxX = std::clamp(outMaxX, 0.0f, static_cast<float>(viewportWidth));
    outMaxY = std::clamp(outMaxY, 0.0f, static_cast<float>(viewportHeight));
    return (outMaxX - outMinX) >= 1.0f && (outMaxY - outMinY) >= 1.0f;
}

int QuantizeMirrorTargetDimension(int requested, int maxDimension) {
    constexpr int kMirrorTileSize = 64;
    int clamped = std::clamp(requested, 256, maxDimension);
    int quantized = ((clamped + kMirrorTileSize - 1) / kMirrorTileSize) * kMirrorTileSize;
    return std::clamp(quantized, 256, maxDimension);
}

uint64_t HashCombine64(uint64_t seed, uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    return seed;
}

uint64_t HashString64(const std::string& value) {
    return std::hash<std::string>{}(value);
}

uint64_t HashFloat64(float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<uint64_t>(bits);
}

const std::vector<float>& GetPrimitiveTriangleVertices(RenderType type) {
    static const std::vector<float> cubeVerts(std::begin(vertices), std::end(vertices));
    static const std::vector<float> planeVerts(std::begin(mirrorPlaneVertices), std::end(mirrorPlaneVertices));
    static const std::vector<float> sphereVerts = generateSphere();
    static const std::vector<float> capsuleVerts = generateCapsule();
    static const std::vector<float> torusVerts = generateTorus();

    switch (type) {
        case RenderType::Cube: return cubeVerts;
        case RenderType::Sphere: return sphereVerts;
        case RenderType::Capsule: return capsuleVerts;
        case RenderType::Plane: return planeVerts;
        case RenderType::Torus: return torusVerts;
        default: return planeVerts;
    }
}

// The MaterialProperties half of the static merge key, shared with the multi-draw batch key.
template <typename Visitor>
void VisitMaterialPropertiesKey(const MaterialProperties& m, Visitor& visitor) {
    auto num = [&](float v) { visitor.num(v); };
    auto integer = [&](int v) { visitor.integer(v); };
    auto flag = [&](bool v) { visitor.flag(v); };

    integer(static_cast<int>(m.textureFilter));
    num(m.color.r); num(m.color.g); num(m.color.b);
    num(m.alpha);
    num(m.ambientStrength);
    num(m.specularStrength);
    num(m.shininess);
    num(m.normalMapIntensity);
    num(m.textureMix);
    num(m.uvTiling.x); num(m.uvTiling.y);
    num(m.uvOffset.x); num(m.uvOffset.y);
    num(m.scrollSpeed);
    num(m.scrollDirection.x); num(m.scrollDirection.y);
    flag(m.uvScrollEnabled);
    num(m.cloudColor.r); num(m.cloudColor.g); num(m.cloudColor.b);
    num(m.cloudSkyColor.r); num(m.cloudSkyColor.g); num(m.cloudSkyColor.b);
    num(m.cloudScale);
    num(m.cloudCoverage);
    num(m.cloudSoftness);
    integer(m.cloudDetail);
    num(m.cloudSpeed);
    num(m.cloudWarp);
    num(m.cloudHighlight);
    num(m.cloudStars);
    num(m.cloudHorizon);
    flag(m.pbrEnabled);
    num(m.metallic);
    num(m.roughness);
    num(m.occlusionStrength);
    num(m.emissionColor.r); num(m.emissionColor.g); num(m.emissionColor.b);
    num(m.emissionStrength);
    num(m.heightScale);
    integer(m.parallaxSteps);
    flag(m.parallaxInvertHeight);
}

// Every field that decides whether two objects can share a static merge batch. The batch key
// string and the per-frame change signature both walk this list, so they cannot drift apart.
template <typename Visitor>
void VisitStaticMergeMaterialKey(const SceneObject& obj, Visitor& visitor) {
    auto integer = [&](int v) { visitor.integer(v); };
    auto flag = [&](bool v) { visitor.flag(v); };
    auto str = [&](const std::string& v) { visitor.str(v); };

    integer(static_cast<int>(obj.renderType));
    str(obj.vertexShaderPath);
    str(obj.fragmentShaderPath);
    str(obj.materialPath);
    str(obj.albedoTexturePath);
    str(obj.overlayTexturePath);
    str(obj.normalMapPath);
    str(obj.metallicRoughnessMapPath);
    str(obj.metallicMapPath);
    str(obj.roughnessMapPath);
    str(obj.opacityMapPath);
    str(obj.emissiveMapPath);
    str(obj.occlusionMapPath);
    str(obj.heightMapPath);
    flag(obj.useOverlay);

    VisitMaterialPropertiesKey(obj.material, visitor);
}

void AppendStaticMergeMaterialKey(const SceneObject& obj, std::string& out) {
    struct StringKeyVisitor {
        std::string& out;
        void num(float v) { out += std::to_string(v); out += '|'; }
        void integer(int v) { out += std::to_string(v); out += '|'; }
        void flag(bool v) { out += v ? '1' : '0'; out += '|'; }
        void str(const std::string& v) { out += v; out += '|'; }
    } visitor{out};
    VisitStaticMergeMaterialKey(obj, visitor);
}

// Hashes the same fields as AppendStaticMergeMaterialKey without formatting them. The signature
// is rebuilt every frame to notice scene edits, and std::to_string on ~45 floats per object was a
// measurable share of a frame on low-end CPUs.
struct HashKeyVisitor {
    uint64_t hash;
    void num(float v) { hash = HashCombine64(hash, HashFloat64(v == 0.0f ? 0.0f : v)); }
    void integer(int v) { hash = HashCombine64(hash, static_cast<uint64_t>(static_cast<int64_t>(v))); }
    void flag(bool v) { hash = HashCombine64(hash, v ? 2u : 1u); }
    void str(const std::string& v) { hash = HashCombine64(hash, HashString64(v)); }
};

uint64_t HashStaticMergeMaterialKey(const SceneObject& obj, uint64_t seed) {
    HashKeyVisitor visitor{seed};
    VisitStaticMergeMaterialKey(obj, visitor);
    return visitor.hash;
}

bool IsStaticMergeCandidate(const SceneObject& obj) {
    if (!IsObjectEnabledInHierarchy(obj) || !HasRendererComponent(obj)) return false;
    if (obj.hasUI || obj.hasLight || obj.hasCamera || obj.hasPostFX) return false;
    if (obj.hasRigidbody || obj.hasRigidbody2D || obj.hasPlayerController) return false;
    if (obj.hasVideoPlayer) return false;
    if (obj.hasAnimation || obj.hasSkeletalAnimation) return false;
    if (!obj.scripts.empty()) return false;
    if (obj.faceCamera) return false;

    switch (obj.renderType) {
        case RenderType::Cube:
        case RenderType::Sphere:
        case RenderType::Capsule:
        case RenderType::OBJMesh:
        case RenderType::Model:
        case RenderType::Plane:
        case RenderType::Sprite:
        case RenderType::Torus:
            return true;
        case RenderType::None:
        case RenderType::Mirror:
            return false;
    }

    return false;
}

bool TryGetObjectTriangleVertexStream(const SceneObject& obj, const std::vector<float>*& outVertices) {
    outVertices = nullptr;
    switch (obj.renderType) {
        case RenderType::Cube:
        case RenderType::Sphere:
        case RenderType::Capsule:
        case RenderType::Plane:
        case RenderType::Torus: {
            outVertices = &GetPrimitiveTriangleVertices(obj.renderType);
            return true;
        }
        case RenderType::OBJMesh:
            if (obj.meshId >= 0) {
                if (const auto* meshInfo = g_objLoader.getMeshInfo(obj.meshId)) {
                    if (!meshInfo->baseVertices.empty()) {
                        outVertices = &meshInfo->baseVertices;
                        return true;
                    }
                }
            }
            return false;
        case RenderType::Model:
            if (obj.meshId >= 0) {
                if (const auto* meshInfo = getModelLoader().getMeshInfo(obj.meshId)) {
                    if (!meshInfo->baseVertices.empty()) {
                        outVertices = &meshInfo->baseVertices;
                        return true;
                    }
                }
            }
            return false;
        case RenderType::None:
        case RenderType::Mirror:
        case RenderType::Sprite:
            return false;
    }

    return false;
}

void AppendTransformedTriangleVertices(const std::vector<float>& src, const glm::mat4& model, std::vector<float>& dst,
                                       glm::vec3& boundsMin, glm::vec3& boundsMax) {
    if (src.empty()) return;
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(model)));
    const size_t baseSize = dst.size();
    dst.resize(baseSize + src.size());

    for (size_t i = 0; i + 7 < src.size(); i += 8) {
        glm::vec3 pos(src[i + 0], src[i + 1], src[i + 2]);
        glm::vec3 normal(src[i + 3], src[i + 4], src[i + 5]);
        glm::vec2 uv(src[i + 6], src[i + 7]);

        glm::vec3 worldPos = glm::vec3(model * glm::vec4(pos, 1.0f));
        glm::vec3 worldNormal = glm::normalize(normalMatrix * normal);
        if (!std::isfinite(worldNormal.x) || glm::length(worldNormal) < 1e-6f) {
            worldNormal = glm::vec3(0.0f, 1.0f, 0.0f);
        }

        boundsMin = glm::min(boundsMin, worldPos);
        boundsMax = glm::max(boundsMax, worldPos);

        size_t out = baseSize + i;
        dst[out + 0] = worldPos.x;
        dst[out + 1] = worldPos.y;
        dst[out + 2] = worldPos.z;
        dst[out + 3] = worldNormal.x;
        dst[out + 4] = worldNormal.y;
        dst[out + 5] = worldNormal.z;
        dst[out + 6] = uv.x;
        dst[out + 7] = uv.y;
    }
}
} // namespace

const std::vector<float>* GetObjectTriangleVertexStream(const SceneObject& obj) {
    const std::vector<float>* stream = nullptr;
    if (!TryGetObjectTriangleVertexStream(obj, stream)) return nullptr;
    return stream;
}

glm::mat4 BuildCameraProjection(const Camera& camera, int width, int height,
                                float fovDeg, float nearPlane, float farPlane) {
    const float safeWidth = static_cast<float>(std::max(1, width));
    const float safeHeight = static_cast<float>(std::max(1, height));
    const float aspect = safeWidth / safeHeight;
    if (camera.orthographic) {
        const float halfHeight = (camera.orthoSize > 0.0f)
            ? camera.orthoSize
            : safeHeight / (2.0f * std::max(1.0f, camera.pixelsPerUnit));
        const float halfWidth = halfHeight * aspect;
        return glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, nearPlane, farPlane);
    }
    return glm::perspective(glm::radians(fovDeg), aspect, nearPlane, farPlane);
}

void ModuRuntime2DRenderCounters_Reset() {
    gRuntime2DRenderCounters = Runtime2DRenderCounters{};
}

void ModuRuntime2DRenderCounters_Read(uint64_t* outTextureBindCount,
                                      uint64_t* outStateBindCount) {
    if (outTextureBindCount) {
        *outTextureBindCount = gRuntime2DRenderCounters.textureBindCount;
    }
    if (outStateBindCount) {
        *outStateBindCount = gRuntime2DRenderCounters.stateBindCount;
    }
}

// Global OBJ loader instance
OBJLoader g_objLoader;

// Cube vertex data
float vertices[] = {
    // Back face (z = -0.5f)
     0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  1.0f, 0.0f,
    -0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  0.0f, 0.0f,
    -0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  0.0f, 1.0f,
     0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  1.0f, 0.0f,
    -0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  0.0f, 1.0f,
     0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,  1.0f, 1.0f,

    // Front face (z = 0.5f)
    -0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  0.0f, 0.0f,
     0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  1.0f, 0.0f,
     0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  1.0f, 1.0f,
     0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  1.0f, 1.0f,
    -0.5f,  0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  0.0f, 1.0f,
    -0.5f, -0.5f,  0.5f,  0.0f,  0.0f,  1.0f,  0.0f, 0.0f,

    // Left face (x = -0.5f)
    -0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,  1.0f, 0.0f,
    -0.5f,  0.5f, -0.5f, -1.0f,  0.0f,  0.0f,  1.0f, 1.0f,
    -0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,  0.0f, 1.0f,
    -0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,  0.0f, 1.0f,
    -0.5f, -0.5f,  0.5f, -1.0f,  0.0f,  0.0f,  0.0f, 0.0f,
    -0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,  1.0f, 0.0f,

    // Right face (x = 0.5f)
     0.5f, -0.5f, -0.5f,  1.0f,  0.0f,  0.0f,  0.0f, 1.0f,
     0.5f,  0.5f, -0.5f,  1.0f,  0.0f,  0.0f,  1.0f, 1.0f,
     0.5f,  0.5f,  0.5f,  1.0f,  0.0f,  0.0f,  1.0f, 0.0f,
     0.5f, -0.5f, -0.5f,  1.0f,  0.0f,  0.0f,  0.0f, 1.0f,
     0.5f,  0.5f,  0.5f,  1.0f,  0.0f,  0.0f,  1.0f, 0.0f,
     0.5f, -0.5f,  0.5f,  1.0f,  0.0f,  0.0f,  0.0f, 0.0f,

    // Bottom face (y = -0.5f)
    -0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,  0.0f, 1.0f,
     0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,  1.0f, 1.0f,
     0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,  1.0f, 0.0f,
     0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,  1.0f, 0.0f,
    -0.5f, -0.5f,  0.5f,  0.0f, -1.0f,  0.0f,  0.0f, 0.0f,
    -0.5f, -0.5f, -0.5f,  0.0f, -1.0f,  0.0f,  0.0f, 1.0f,

    // Top face (y = 0.5f)
    -0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,  0.0f, 1.0f,
    -0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,  0.0f, 0.0f,
     0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,  1.0f, 0.0f,
    -0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,  0.0f, 1.0f,
     0.5f,  0.5f,  0.5f,  0.0f,  1.0f,  0.0f,  1.0f, 0.0f,
     0.5f,  0.5f, -0.5f,  0.0f,  1.0f,  0.0f,  1.0f, 1.0f
};

float mirrorPlaneVertices[] = {
    // positions          // normals        // texcoords
    -0.5f, -0.5f, 0.0f,    0.0f, 0.0f, 1.0f,  0.0f, 0.0f,
     0.5f, -0.5f, 0.0f,    0.0f, 0.0f, 1.0f,  1.0f, 0.0f,
     0.5f,  0.5f, 0.0f,    0.0f, 0.0f, 1.0f,  1.0f, 1.0f,

    -0.5f, -0.5f, 0.0f,    0.0f, 0.0f, 1.0f,  0.0f, 0.0f,
     0.5f,  0.5f, 0.0f,    0.0f, 0.0f, 1.0f,  1.0f, 1.0f,
    -0.5f,  0.5f, 0.0f,    0.0f, 0.0f, 1.0f,  0.0f, 1.0f
};

std::vector<float> generateSphere(int segments, int rings) {
    std::vector<float> vertices;

    for (int ring = 0; ring <= rings; ring++) {
        float theta = ring * PI / rings;
        float sinTheta = sin(theta);
        float cosTheta = cos(theta);

        for (int seg = 0; seg <= segments; seg++) {
            float phi = seg * 2.0f * PI / segments;
            float sinPhi = sin(phi);
            float cosPhi = cos(phi);

            float x = cosPhi * sinTheta;
            float y = cosTheta;
            float z = sinPhi * sinTheta;

            // Position
            vertices.push_back(x * 0.5f);
            vertices.push_back(y * 0.5f);
            vertices.push_back(z * 0.5f);

            // Normal (same as position for unit sphere)
            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(z);

            // Texcoord
            vertices.push_back((float)seg / segments);
            vertices.push_back((float)ring / rings);
        }
    }

    std::vector<float> triangulated;
    int stride = segments + 1;
    for (int ring = 0; ring < rings; ring++) {
        for (int seg = 0; seg < segments; seg++) {
            int current = ring * stride + seg;
            int next = current + stride;

            for (int i = 0; i < 8; i++) triangulated.push_back(vertices[current * 8 + i]);
            for (int i = 0; i < 8; i++) triangulated.push_back(vertices[(current + 1) * 8 + i]);
            for (int i = 0; i < 8; i++) triangulated.push_back(vertices[next * 8 + i]);

            for (int i = 0; i < 8; i++) triangulated.push_back(vertices[(current + 1) * 8 + i]);
            for (int i = 0; i < 8; i++) triangulated.push_back(vertices[(next + 1) * 8 + i]);
            for (int i = 0; i < 8; i++) triangulated.push_back(vertices[next * 8 + i]);
        }
    }

    return triangulated;
}

std::vector<float> generateCapsule(int segments, int rings) {
    return generateCapsuleShape(0.25f, 0.5f, segments, rings);
}

std::vector<float> generateCapsuleShape(float radius, float cylinderHalfHeight,
                                        int segments, int rings) {
    std::vector<float> vertices;
    const float cylinderHeight = std::max(0.0f, cylinderHalfHeight);
    radius = std::max(0.0001f, radius);

    // Top hemisphere
    for (int ring = 0; ring <= rings / 2; ring++) {
        float theta = ring * PI / rings;
        float sinTheta = sin(theta);
        float cosTheta = cos(theta);

        for (int seg = 0; seg <= segments; seg++) {
            float phi = seg * 2.0f * PI / segments;
            float sinPhi = sin(phi);
            float cosPhi = cos(phi);

            float x = cosPhi * sinTheta * radius;
            float y = cosTheta * radius + cylinderHeight;
            float z = sinPhi * sinTheta * radius;

            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(z);

            glm::vec3 normal = glm::normalize(glm::vec3(x, y - cylinderHeight, z));
            vertices.push_back(normal.x);
            vertices.push_back(normal.y);
            vertices.push_back(normal.z);

            vertices.push_back((float)seg / segments);
            vertices.push_back((float)ring / (rings / 2));
        }
    }

    // Cylinder body
    for (int i = 0; i <= 1; i++) {
        float y = i == 0 ? cylinderHeight : -cylinderHeight;
        for (int seg = 0; seg <= segments; seg++) {
            float phi = seg * 2.0f * PI / segments;
            float x = cos(phi) * radius;
            float z = sin(phi) * radius;

            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(z);

            glm::vec3 normal = glm::normalize(glm::vec3(x, 0.0f, z));
            vertices.push_back(normal.x);
            vertices.push_back(normal.y);
            vertices.push_back(normal.z);

            vertices.push_back((float)seg / segments);
            vertices.push_back(0.5f);
        }
    }

    // Bottom hemisphere
    for (int ring = rings / 2; ring <= rings; ring++) {
        float theta = ring * PI / rings;
        float sinTheta = sin(theta);
        float cosTheta = cos(theta);

        for (int seg = 0; seg <= segments; seg++) {
            float phi = seg * 2.0f * PI / segments;
            float sinPhi = sin(phi);
            float cosPhi = cos(phi);

            float x = cosPhi * sinTheta * radius;
            float y = cosTheta * radius - cylinderHeight;
            float z = sinPhi * sinTheta * radius;

            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(z);

            glm::vec3 normal = glm::normalize(glm::vec3(x, y + cylinderHeight, z));
            vertices.push_back(normal.x);
            vertices.push_back(normal.y);
            vertices.push_back(normal.z);

            vertices.push_back((float)seg / segments);
            vertices.push_back((float)ring / rings);
        }
    }

    std::vector<float> triangulated;
    int stride = segments + 1;
    int totalRings = rings + 4;

    for (int ring = 0; ring < totalRings - 1; ring++) {
        for (int seg = 0; seg < segments; seg++) {
            int current = ring * stride + seg;
            int next = current + stride;

            for (int i = 0; i < 8; i++) triangulated.push_back(vertices[current * 8 + i]);
            for (int i = 0; i < 8; i++) triangulated.push_back(vertices[(current + 1) * 8 + i]);
            for (int i = 0; i < 8; i++) triangulated.push_back(vertices[next * 8 + i]);

            for (int i = 0; i < 8; i++) triangulated.push_back(vertices[(current + 1) * 8 + i]);
            for (int i = 0; i < 8; i++) triangulated.push_back(vertices[(next + 1) * 8 + i]);
            for (int i = 0; i < 8; i++) triangulated.push_back(vertices[next * 8 + i]);
        }
    }

    return triangulated;
}

std::vector<float> generateTorus(int segments, int sides) {
    std::vector<float> vertices;
    float majorRadius = 0.35f;
    float minorRadius = 0.15f;

    for (int seg = 0; seg <= segments; ++seg) {
        float u = seg * 2.0f * PI / segments;
        float cosU = cos(u);
        float sinU = sin(u);

        for (int side = 0; side <= sides; ++side) {
            float v = side * 2.0f * PI / sides;
            float cosV = cos(v);
            float sinV = sin(v);

            float x = (majorRadius + minorRadius * cosV) * cosU;
            float y = minorRadius * sinV;
            float z = (majorRadius + minorRadius * cosV) * sinU;

            glm::vec3 normal = glm::normalize(glm::vec3(cosU * cosV, sinV, sinU * cosV));

            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(z);
            vertices.push_back(normal.x);
            vertices.push_back(normal.y);
            vertices.push_back(normal.z);
            vertices.push_back((float)seg / segments);
            vertices.push_back((float)side / sides);
        }
    }

    std::vector<float> triangulated;
    int stride = sides + 1;
    for (int seg = 0; seg < segments; ++seg) {
        for (int side = 0; side < sides; ++side) {
            int current = seg * stride + side;
            int next = current + stride;

            for (int i = 0; i < 8; ++i) triangulated.push_back(vertices[current * 8 + i]);
            for (int i = 0; i < 8; ++i) triangulated.push_back(vertices[(current + 1) * 8 + i]);
            for (int i = 0; i < 8; ++i) triangulated.push_back(vertices[next * 8 + i]);

            for (int i = 0; i < 8; ++i) triangulated.push_back(vertices[(current + 1) * 8 + i]);
            for (int i = 0; i < 8; ++i) triangulated.push_back(vertices[(next + 1) * 8 + i]);
            for (int i = 0; i < 8; ++i) triangulated.push_back(vertices[next * 8 + i]);
        }
    }

    return triangulated;
}

// Mesh implementation
namespace {
std::atomic<uint64_t> gNextMeshSerial{1};
}

Mesh::Mesh(const float* vertexData, size_t dataSizeBytes) {
    vertexCount = dataSizeBytes / (8 * sizeof(float));
    strideFloats = 8;
    serial = gNextMeshSerial.fetch_add(1, std::memory_order_relaxed);

    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);

    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, dataSizeBytes, vertexData, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

Mesh::Mesh(const float* vertexData, size_t dataSizeBytes, bool dynamicUsage,
           const void* boneData, size_t boneDataBytes) {
    vertexCount = dataSizeBytes / (8 * sizeof(float));
    strideFloats = 8;
    serial = gNextMeshSerial.fetch_add(1, std::memory_order_relaxed);
    dynamic = dynamicUsage;
    hasBones = boneData && boneDataBytes > 0;

    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);

    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, dataSizeBytes, vertexData, dynamicUsage ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, strideFloats * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, strideFloats * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, strideFloats * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

    if (hasBones) {
        glGenBuffers(1, &boneVBO);
        glBindBuffer(GL_ARRAY_BUFFER, boneVBO);
        glBufferData(GL_ARRAY_BUFFER, boneDataBytes, boneData, GL_STATIC_DRAW);

        glVertexAttribIPointer(3, 4, GL_INT, sizeof(int) * 4 + sizeof(float) * 4, (void*)0);
        glEnableVertexAttribArray(3);

        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(int) * 4 + sizeof(float) * 4,
                              (void*)(sizeof(int) * 4));
        glEnableVertexAttribArray(4);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

Mesh::~Mesh() {
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    if (boneVBO) {
        glDeleteBuffers(1, &boneVBO);
    }
}

void Mesh::draw() const {
    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
    glBindVertexArray(0);
}

void Mesh::updateVertices(const float* vertexData, size_t dataSizeBytes) {
    if (!dynamic) return;
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, dataSizeBytes, vertexData);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    vertexCount = dataSizeBytes / (strideFloats * sizeof(float));
}

static void applyCpuSkinning(OBJLoader::LoadedMesh& meshInfo, const std::vector<glm::mat4>& bones, int maxBones) {
    if (!meshInfo.mesh || !meshInfo.isSkinned) return;
    if (meshInfo.baseVertices.empty() || meshInfo.boneIds.empty() || meshInfo.boneWeights.empty()) return;
    if (!meshInfo.mesh->isDynamic()) return;

    size_t vertexCount = meshInfo.baseVertices.size() / 8;
    if (vertexCount == 0 || meshInfo.boneIds.size() != vertexCount || meshInfo.boneWeights.size() != vertexCount) {
        return;
    }

    std::vector<float> skinned = meshInfo.baseVertices;
    int boneLimit = std::min<int>(static_cast<int>(bones.size()), maxBones);
    for (size_t i = 0; i < vertexCount; ++i) {
        glm::vec3 basePos(skinned[i * 8 + 0], skinned[i * 8 + 1], skinned[i * 8 + 2]);
        glm::vec3 baseNorm(skinned[i * 8 + 3], skinned[i * 8 + 4], skinned[i * 8 + 5]);
        glm::ivec4 ids = meshInfo.boneIds[i];
        glm::vec4 weights = meshInfo.boneWeights[i];

        glm::vec4 skinnedPos(0.0f);
        glm::vec3 skinnedNorm(0.0f);
        for (int k = 0; k < 4; ++k) {
            int id = ids[k];
            float w = weights[k];
            if (w <= 0.0f || id < 0 || id >= boneLimit) continue;
            const glm::mat4& m = bones[id];
            skinnedPos += w * (m * glm::vec4(basePos, 1.0f));
            skinnedNorm += w * glm::mat3(m) * baseNorm;
        }
        skinned[i * 8 + 0] = skinnedPos.x;
        skinned[i * 8 + 1] = skinnedPos.y;
        skinned[i * 8 + 2] = skinnedPos.z;
        if (glm::length(skinnedNorm) > 1e-6f) {
            skinnedNorm = glm::normalize(skinnedNorm);
        }
        skinned[i * 8 + 3] = skinnedNorm.x;
        skinned[i * 8 + 4] = skinnedNorm.y;
        skinned[i * 8 + 5] = skinnedNorm.z;
    }

    meshInfo.mesh->updateVertices(skinned.data(), skinned.size() * sizeof(float));
}

// OBJLoader implementation
int OBJLoader::loadOBJ(const std::string& filepath, std::string& errorMsg) {
    // Check if already loaded
    for (size_t i = 0; i < loadedMeshes.size(); i++) {
        if (loadedMeshes[i].path == filepath) {
            return static_cast<int>(i);
        }
    }

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    std::string baseDir = fs::path(filepath).parent_path().string();
    if (!baseDir.empty()) baseDir += "/";

    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
                                filepath.c_str(), baseDir.c_str());

    if (!warn.empty()) {
        errorMsg += "Warning: " + warn + "\n";
    }

    if (!err.empty()) {
        errorMsg += "Error: " + err + "\n";
    }

    if (!ret || shapes.empty()) {
        errorMsg += "Failed to load OBJ file: " + filepath;
        return -1;
    }

    std::vector<float> vertices;
    bool hasNormalsInFile = !attrib.normals.empty();

    int faceCount = 0;
    for (const auto& shape : shapes) {
        faceCount += static_cast<int>(shape.mesh.num_face_vertices.size());
    }

    glm::vec3 boundsMin(FLT_MAX);
    glm::vec3 boundsMax(-FLT_MAX);
    std::vector<glm::vec3> triPositions;
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> triangleIndices;

    positions.reserve(attrib.vertices.size() / 3);
    for (size_t i = 0; i + 2 < attrib.vertices.size(); i += 3) {
        positions.emplace_back(attrib.vertices[i], attrib.vertices[i + 1], attrib.vertices[i + 2]);
    }

    for (const auto& shape : shapes) {
        size_t indexOffset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++) {
            int fv = shape.mesh.num_face_vertices[f];

            struct TempVertex {
                glm::vec3 pos;
                glm::vec2 uv;
                glm::vec3 normal;
                bool hasNormal = false;
            };
            std::vector<TempVertex> faceVerts;
            std::vector<int> facePosIndices;
            facePosIndices.reserve(static_cast<size_t>(fv));

            for (int v = 0; v < fv; v++) {
                tinyobj::index_t idx = shape.mesh.indices[indexOffset + v];

                TempVertex tv;
                tv.pos.x = attrib.vertices[3 * size_t(idx.vertex_index) + 0];
                tv.pos.y = attrib.vertices[3 * size_t(idx.vertex_index) + 1];
                tv.pos.z = attrib.vertices[3 * size_t(idx.vertex_index) + 2];

                boundsMin.x = std::min(boundsMin.x, tv.pos.x);
                boundsMin.y = std::min(boundsMin.y, tv.pos.y);
                boundsMin.z = std::min(boundsMin.z, tv.pos.z);
                boundsMax.x = std::max(boundsMax.x, tv.pos.x);
                boundsMax.y = std::max(boundsMax.y, tv.pos.y);
                boundsMax.z = std::max(boundsMax.z, tv.pos.z);

                if (idx.texcoord_index >= 0 && !attrib.texcoords.empty()) {
                    tv.uv.x = attrib.texcoords[2 * size_t(idx.texcoord_index) + 0];
                    tv.uv.y = attrib.texcoords[2 * size_t(idx.texcoord_index) + 1];
                } else {
                    tv.uv = glm::vec2(0.0f);
                }

                if (idx.normal_index >= 0 && hasNormalsInFile) {
                    tv.normal.x = attrib.normals[3 * size_t(idx.normal_index) + 0];
                    tv.normal.y = attrib.normals[3 * size_t(idx.normal_index) + 1];
                    tv.normal.z = attrib.normals[3 * size_t(idx.normal_index) + 2];
                    tv.hasNormal = true;
                }

                faceVerts.push_back(tv);
                facePosIndices.push_back(idx.vertex_index);
            }

            if (!hasNormalsInFile && fv >= 3) {
                glm::vec3 v0 = faceVerts[0].pos;
                glm::vec3 v1 = faceVerts[1].pos;
                glm::vec3 v2 = faceVerts[2].pos;
                glm::vec3 faceNormal = glm::normalize(glm::cross(v1 - v0, v2 - v0));

                for (auto& tv : faceVerts) {
                    tv.normal = faceNormal;
                    tv.hasNormal = true;
                }
            }

            for (int v = 1; v < fv - 1; v++) {
                const TempVertex* tri[3] = { &faceVerts[0], &faceVerts[v], &faceVerts[v+1] };

                int idx0 = facePosIndices[0];
                int idx1 = facePosIndices[v];
                int idx2 = facePosIndices[v + 1];
                if (idx0 >= 0 && idx1 >= 0 && idx2 >= 0) {
                    triangleIndices.push_back(static_cast<uint32_t>(idx0));
                    triangleIndices.push_back(static_cast<uint32_t>(idx1));
                    triangleIndices.push_back(static_cast<uint32_t>(idx2));
                }

                for (int i = 0; i < 3; i++) {
                    triPositions.push_back(tri[i]->pos);
                    vertices.push_back(tri[i]->pos.x);
                    vertices.push_back(tri[i]->pos.y);
                    vertices.push_back(tri[i]->pos.z);
                    vertices.push_back(tri[i]->normal.x);
                    vertices.push_back(tri[i]->normal.y);
                    vertices.push_back(tri[i]->normal.z);
                    vertices.push_back(tri[i]->uv.x);
                    vertices.push_back(tri[i]->uv.y);
                }
            }

            indexOffset += fv;
        }
    }

    if (vertices.empty()) {
        errorMsg += "No vertices found in OBJ file";
        return -1;
    }

    LoadedMesh loaded;
    loaded.path = filepath;
    loaded.name = fs::path(filepath).stem().string();
    loaded.mesh = std::make_unique<Mesh>(vertices.data(), vertices.size() * sizeof(float));
    loaded.vertexCount = static_cast<int>(vertices.size() / 8);
    loaded.faceCount = faceCount;
    loaded.hasNormals = hasNormalsInFile;
    loaded.hasTexCoords = !attrib.texcoords.empty();
    loaded.boundsMin = boundsMin;
    loaded.boundsMax = boundsMax;
    loaded.triangleVertices = std::move(triPositions);
    loaded.positions = std::move(positions);
    loaded.triangleIndices = std::move(triangleIndices);
    loaded.baseVertices = vertices;

    loadedMeshes.push_back(std::move(loaded));
    return static_cast<int>(loadedMeshes.size() - 1);
}

Mesh* OBJLoader::getMesh(int index) {
    if (index < 0 || index >= static_cast<int>(loadedMeshes.size())) {
        return nullptr;
    }
    return loadedMeshes[index].mesh.get();
}

const OBJLoader::LoadedMesh* OBJLoader::getMeshInfo(int index) const {
    if (index < 0 || index >= static_cast<int>(loadedMeshes.size())) {
        return nullptr;
    }
    return &loadedMeshes[index];
}

// Renderer implementation
Renderer::Renderer() = default;

Renderer::~Renderer() {
    rendererShuttingDown = true;
    if (optState) {
        optState->releaseGpuResources();
#if !MODULARITY_OPENGL_ES
        if (optState->cullProgram) glDeleteProgram(optState->cullProgram);
        if (optState->hiZProgram) glDeleteProgram(optState->hiZProgram);
#endif
        optState.reset();
    }
    shaderCache.clear();
    shader = nullptr;
    defaultShader = nullptr;
    delete texture1;
    delete texture2;
    delete cubeMesh;
    delete sphereMesh;
    delete capsuleMesh;
    for (auto& [ratioKey, mesh] : capsuleColliderMeshes) delete mesh;
    capsuleColliderMeshes.clear();
    delete planeMesh;
    delete torusMesh;
    delete skybox;
    delete postShader;
    delete compositeShader;
    delete brightShader;
    delete blurShader;
    delete shadowDepthShader;
    delete directionalShadowDepthShader;
    if (previewTarget.fbo) glDeleteFramebuffers(1, &previewTarget.fbo);
    if (previewTarget.texture) glDeleteTextures(1, &previewTarget.texture);
    if (previewTarget.rbo) glDeleteRenderbuffers(1, &previewTarget.rbo);
    for (auto& entry : extraPreviewTargets) {
        releaseRenderTarget(entry.second);
    }
    extraPreviewTargets.clear();
    extraPreviewTargetLastUsed.clear();
    if (postTarget.fbo) glDeleteFramebuffers(1, &postTarget.fbo);
    if (postTarget.texture) glDeleteTextures(1, &postTarget.texture);
    if (postTarget.rbo) glDeleteRenderbuffers(1, &postTarget.rbo);
    releaseRenderTarget(volumetricTarget);
    releaseRenderTarget(volumetricBlurTarget);
    releaseRenderTarget(ssrTarget);
    releaseSurfaceTarget();
    releaseVoxelVolume();
    releaseXREyePreviews();
    releaseRenderTarget(polarizationTarget);
    releaseRenderTarget(externalSceneTarget);
    releaseRenderTarget(previewPostTarget);
    releaseRenderTarget(externalPostTarget);
    releaseRenderTarget(overlayCaptureTarget);
    releaseRenderTarget(bandLayerTarget);
    releaseRenderTarget(bandCaptureTarget);
    if (historyTarget.fbo) glDeleteFramebuffers(1, &historyTarget.fbo);
    if (historyTarget.texture) glDeleteTextures(1, &historyTarget.texture);
    if (historyTarget.rbo) glDeleteRenderbuffers(1, &historyTarget.rbo);
    if (bloomTargetA.fbo) glDeleteFramebuffers(1, &bloomTargetA.fbo);
    if (bloomTargetA.texture) glDeleteTextures(1, &bloomTargetA.texture);
    if (bloomTargetA.rbo) glDeleteRenderbuffers(1, &bloomTargetA.rbo);
    if (bloomTargetB.fbo) glDeleteFramebuffers(1, &bloomTargetB.fbo);
    if (bloomTargetB.texture) glDeleteTextures(1, &bloomTargetB.texture);
    if (bloomTargetB.rbo) glDeleteRenderbuffers(1, &bloomTargetB.rbo);
    if (selectionMaskTarget.fbo) glDeleteFramebuffers(1, &selectionMaskTarget.fbo);
    if (selectionMaskTarget.texture) glDeleteTextures(1, &selectionMaskTarget.texture);
    if (selectionMaskTarget.rbo) glDeleteRenderbuffers(1, &selectionMaskTarget.rbo);
    for (auto& entry : shadowCubeMaps) {
        if (entry.second.fbo) glDeleteFramebuffers(1, &entry.second.fbo);
        if (entry.second.depthCube) glDeleteTextures(1, &entry.second.depthCube);
    }
    shadowCubeMaps.clear();
    for (auto& entry : shadowDirectionalMaps) {
        if (entry.second.fbo) glDeleteFramebuffers(1, &entry.second.fbo);
        if (entry.second.depthTexture) glDeleteTextures(1, &entry.second.depthTexture);
    }
    shadowDirectionalMaps.clear();
    for (auto& entry : mirrorTargets) {
        releaseRenderTarget(entry.second);
    }
    mirrorTargets.clear();
    mirrorUpdateStates.clear();
    for (auto& entry : reflectionCastTargets) {
        releaseReflectionCastTarget(entry.second);
    }
    reflectionCastTargets.clear();
    releaseSkyboxReflectionTarget(skyboxReflectionTarget);
    for (auto& entry : uiTargets) {
        releaseRenderTarget(entry.second);
    }
    uiTargets.clear();
    uiTargetViews.clear();
    for (auto& entry : uiTargetAtlases) {
        releaseRenderTarget(entry.second.target);
    }
    uiTargetAtlases.clear();
    if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
    if (viewportTexture) glDeleteTextures(1, &viewportTexture);
    if (sceneDepthTexture) glDeleteTextures(1, &sceneDepthTexture);
    if (quadVBO) glDeleteBuffers(1, &quadVBO);
    if (quadVAO) glDeleteVertexArrays(1, &quadVAO);
    releaseParticleBuffers();
    if (debugWhiteTexture) glDeleteTextures(1, &debugWhiteTexture);
    if (missingMaterialFallbackTexture) glDeleteTextures(1, &missingMaterialFallbackTexture);
}

Texture* Renderer::getTexture(const std::string& path, MaterialProperties::TextureFilter filter,
                              bool preserveDataPrecision) {
    if (path.empty()) return nullptr;
    if (!moduRenderContextLive()) {
        static bool warnedNoContext = false;
        if (!warnedNoContext) {
            std::cerr << "[Renderer] Texture request without an OpenGL context. Returning null texture.\n";
            warnedNoContext = true;
        }
        return nullptr;
    }
    bool point = (filter == MaterialProperties::TextureFilter::Point);
    auto& cache = preserveDataPrecision
        ? (point ? normalTextureCachePoint : normalTextureCacheBilinear)
        : (point ? textureCachePoint : textureCacheBilinear);
    const double nowSec = glfwGetTime();
    auto missingIt = missingTextureRetryAfter.find(path);
    if (missingIt != missingTextureRetryAfter.end()) {
        if (nowSec < missingIt->second) {
            return nullptr;
        }
        missingTextureRetryAfter.erase(missingIt);
    }
    auto it = cache.find(path);
    if (it != cache.end()) {
        it->second.lastUsedTime = nowSec;
        it->second.lastUsedFrame = frameSerial;
        return it->second.texture.get();
    }

    GLenum minFilter = point ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
    GLenum magFilter = point ? GL_NEAREST : GL_LINEAR;
    TextureFormatPolicy policy = preserveDataPrecision
        ? TextureFormatPolicy::Full : getTextureFormatOverride(path);
    if (policy == TextureFormatPolicy::Auto) policy = defaultTextureFormatPolicy;
    const std::string loadPath = Modularity::ResolveLoadableTexturePath(path);
    auto tex = std::make_unique<Texture>(loadPath, GL_REPEAT, GL_REPEAT, minFilter, magFilter, policy);
    ++textureCacheGeneration;
    if (!tex->GetID()) {
        // A missing file is cheap to probe and may appear any moment. One that exists but
        // cannot be decoded (a DWAA EXR) fails the same way until it is edited, and retrying
        // it every second re-read tens of megabytes on the main thread. invalidateTexture,
        // called by the EXR transcoder and Version Control, clears the wait early.
        const bool unreadable = !Modularity::Platform::GetAssetSource().Exists(loadPath);
        missingTextureRetryAfter[path] = nowSec + (unreadable ? 1.0 : 30.0);
        textureLoadErrors[path] = tex->GetLoadError();
        return nullptr;
    }
    textureLoadErrors.erase(path);
    CachedTextureEntry entry;
    entry.approxBytes = tex->GetApproxMemoryBytes();
    entry.lastUsedTime = nowSec;
    entry.lastUsedFrame = frameSerial;
    entry.texture = std::move(tex);
    Texture* raw = entry.texture.get();
    textureCacheUsageBytes += entry.approxBytes;
    cache[path] = std::move(entry);
    purgeTextureCacheIfNeeded();
    return raw;
}

void Renderer::invalidateTexture(const std::string& path) {
    if (path.empty()) return;
    ++textureCacheGeneration;
    missingTextureRetryAfter.erase(path);
    auto eraseFromCache = [this, &path](auto& cache) {
        auto it = cache.find(path);
        if (it == cache.end()) return;
        textureCacheUsageBytes = (textureCacheUsageBytes > it->second.approxBytes)
            ? (textureCacheUsageBytes - it->second.approxBytes)
            : 0;
        cache.erase(it);
    };
    eraseFromCache(textureCacheBilinear);
    eraseFromCache(textureCachePoint);
    eraseFromCache(normalTextureCacheBilinear);
    eraseFromCache(normalTextureCachePoint);
}

std::string Renderer::normalizeTextureKey(const std::string& path) const {
    if (path.empty()) return path;
    std::error_code ec;
    fs::path p(path);
    if (p.is_relative() && !textureKeyRoot.empty()) {
        p = fs::path(textureKeyRoot) / p;
    }
    fs::path canon = fs::weakly_canonical(p, ec);
    if (ec) canon = p.lexically_normal();
    return canon.generic_string();
}

std::string Renderer::getTextureLoadError(const std::string& path) const {
    auto it = textureLoadErrors.find(path);
    return (it == textureLoadErrors.end()) ? std::string() : it->second;
}

void Renderer::setTextureFormatOverride(const std::string& path, TextureFormatPolicy policy) {
    if (path.empty()) return;
    const std::string key = normalizeTextureKey(path);
    if (policy == TextureFormatPolicy::Auto) {
        textureFormatOverrides.erase(key);
    } else {
        textureFormatOverrides[key] = policy;
    }
    auto evictMatching = [this, &key](auto& cache) {
        for (auto it = cache.begin(); it != cache.end();) {
            if (normalizeTextureKey(it->first) == key) {
                textureCacheUsageBytes = (textureCacheUsageBytes > it->second.approxBytes)
                    ? (textureCacheUsageBytes - it->second.approxBytes) : 0;
                it = cache.erase(it);
            } else {
                ++it;
            }
        }
    };
    evictMatching(textureCacheBilinear);
    evictMatching(textureCachePoint);
    evictMatching(normalTextureCacheBilinear);
    evictMatching(normalTextureCachePoint);
    ++textureCacheGeneration;
}

TextureFormatPolicy Renderer::getTextureFormatOverride(const std::string& path) const {
    auto it = textureFormatOverrides.find(normalizeTextureKey(path));
    return it == textureFormatOverrides.end() ? TextureFormatPolicy::Auto : it->second;
}

void Renderer::setMaxTextureDimension(int maxDimension) {
    if (Texture::GetMaxDimension() == maxDimension) return;
    Texture::SetMaxDimension(maxDimension);
    ++textureCacheGeneration;
    textureCacheBilinear.clear();
    textureCachePoint.clear();
    normalTextureCacheBilinear.clear();
    normalTextureCachePoint.clear();
    textureCacheUsageBytes = 0;
    missingTextureRetryAfter.clear();
    warnedTextureCacheThrashing = false;
    warnedTextureCacheOverCommitted = false;
}

int ComputeSingleTexelMipLevel(int width, int height) {
    int level = 0;
    while (((width >> level) > 1 || (height >> level) > 1) && level < 31) {
        ++level;
    }
    return level;
}

float ComputeAdaptedExposure(float measured, float& adapted, float dt,
                             const PostFXSettings& settings) {
    if (measured >= 0.0f) {
        measured = std::max(measured, 1e-4f);
        dt = std::clamp(dt, 0.0f, 0.25f);
        if (adapted <= 0.0f) {
            adapted = measured;
        } else {
            const float speed = (measured > adapted)
                                    ? std::max(settings.eyeAdaptationSpeedUp, 0.01f)
                                    : std::max(settings.eyeAdaptationSpeedDown, 0.01f);
            adapted += (measured - adapted) * (1.0f - std::exp(-dt * speed));
        }
    }

    if (adapted <= 0.0f) return 1.0f;

    const float exposure = std::max(settings.eyeAdaptationKeyValue, 0.001f) /
                           std::max(adapted, 1e-4f);
    const float ev = std::log2(std::max(exposure, 1e-6f));
    const float lo = std::min(settings.eyeAdaptationMinEv, settings.eyeAdaptationMaxEv);
    const float hi = std::max(settings.eyeAdaptationMinEv, settings.eyeAdaptationMaxEv);
    return std::exp2(std::clamp(ev, lo, hi));
}

void Renderer::setShadowQuality(int filterQuality, float lodDistance, bool contactHardening) {
    shadowFilterQuality = std::clamp(filterQuality, 0, 3);
    shadowLodDistance = std::max(0.0f, lodDistance);
    shadowContactHardening = contactHardening;
}

float Renderer::updateEyeAdaptation(unsigned int sourceTexture, int width, int height,
                                    const PostFXSettings& settings) {
#if MODULARITY_OPENGL_ES
    // This readback uses desktop glGetTexImage, which GLES does not provide.
    // Keep the configured manual exposure rather than reading an empty PBO.
    (void)sourceTexture; (void)width; (void)height; (void)settings;
    adaptedLuminance = -1.0f;
    luminancePboPrimed = false;
    return 1.0f;
#else
    if (!settings.eyeAdaptationEnabled || sourceTexture == 0 || width <= 0 || height <= 0) {
        adaptedLuminance = -1.0f;
        luminancePboPrimed = false;
        return 1.0f;
    }

    if (luminancePbo[0] == 0) {
        glGenBuffers(2, luminancePbo);
    }

    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);

    int texWidth = 0;
    int texHeight = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texWidth);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texHeight);
    if (texWidth <= 0 || texHeight <= 0) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        return ComputeAdaptedExposure(-1.0f, adaptedLuminance, 0.0f, settings);
    }
    const int maxLevel = ComputeSingleTexelMipLevel(texWidth, texHeight);

    int levelWidth = 0;
    int levelHeight = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, maxLevel, GL_TEXTURE_WIDTH, &levelWidth);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, maxLevel, GL_TEXTURE_HEIGHT, &levelHeight);
    if (levelWidth != 1 || levelHeight != 1) {
        static bool warnedNoMipChain = false;
        if (!warnedNoMipChain) {
            warnedNoMipChain = true;
            std::cerr << "[Renderer] Eye adaptation: scene texture " << texWidth << "x"
                      << texHeight << " has no 1x1 mip at level " << maxLevel
                      << " (got " << levelWidth << "x" << levelHeight
                      << "); skipping the luminance read.\n";
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        return ComputeAdaptedExposure(-1.0f, adaptedLuminance, 0.0f, settings);
    }

    const int writeIndex = luminancePboWriteIndex;
    const int readIndex = 1 - writeIndex;
    luminancePboWriteIndex = readIndex;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, luminancePbo[writeIndex]);
    glBufferData(GL_PIXEL_PACK_BUFFER, sizeof(float) * 4, nullptr, GL_STREAM_READ);
    glGetTexImage(GL_TEXTURE_2D, maxLevel, GL_RGBA, GL_FLOAT, nullptr);

    float measured = -1.0f;
    if (luminancePboPrimed) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, luminancePbo[readIndex]);
        if (const float* px = static_cast<const float*>(
                glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY))) {
            measured = 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    } else {
        luminancePboPrimed = true;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    float dt = 0.0f;
    if (measured >= 0.0f) {
        const double now = glfwGetTime();
        dt = (lastAdaptationTime > 0.0) ? static_cast<float>(now - lastAdaptationTime)
                                        : 1.0f / 60.0f;
        lastAdaptationTime = now;
    }
    return ComputeAdaptedExposure(measured, adaptedLuminance, dt, settings);
#endif
}

void Renderer::setDefaultTextureFormatPolicy(TextureFormatPolicy policy) {
    if (defaultTextureFormatPolicy == policy) return;
    defaultTextureFormatPolicy = policy;
    textureCacheBilinear.clear();
    textureCachePoint.clear();
    normalTextureCacheBilinear.clear();
    normalTextureCachePoint.clear();
    textureCacheUsageBytes = 0;
    missingTextureRetryAfter.clear();
}

void Renderer::setColorPrecision(RendererColorPrecision precision) {
    if (colorPrecision == precision) return;
    colorPrecision = precision;
    if (framebuffer != 0 && viewportTexture != 0 && moduRenderContextLive()) {
        const bool sdr = (colorPrecision == RendererColorPrecision::SDR8);
        glBindTexture(GL_TEXTURE_2D, viewportTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, sdr ? GL_RGBA8 : GL_RGBA16F, currentWidth, currentHeight,
                     0, GL_RGBA, sdr ? GL_UNSIGNED_BYTE : GL_FLOAT, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void Renderer::purgeTextureCacheIfNeeded() {
    if (textureCacheUsageBytes <= textureCacheBudgetBytes) {
        return;
    }

    struct EvictionCandidate {
        std::unordered_map<std::string, CachedTextureEntry>* cache = nullptr;
        double lastUsedTime = 0.0;
        size_t approxBytes = 0;
        std::string path;
    };

    std::vector<EvictionCandidate> candidates;
    candidates.reserve(getTextureCacheCount());

    const uint64_t currentFrame = frameSerial;
    auto collect = [&candidates, currentFrame](auto& cache) {
        for (const auto& [path, entry] : cache) {
            if (entry.lastUsedFrame == currentFrame) continue;
            candidates.push_back({&cache, entry.lastUsedTime, entry.approxBytes, path});
        }
    };
    collect(textureCacheBilinear);
    collect(textureCachePoint);
    collect(normalTextureCacheBilinear);
    collect(normalTextureCachePoint);

    std::sort(candidates.begin(), candidates.end(),
              [](const EvictionCandidate& a, const EvictionCandidate& b) {
                  if (a.lastUsedTime != b.lastUsedTime) return a.lastUsedTime < b.lastUsedTime;
                  return a.path < b.path;
              });

    const double nowSec = glfwGetTime();

    for (int pass = 0; pass < 2; ++pass) {
        const bool honorRecency = (pass == 0);
        for (const auto& candidate : candidates) {
            if (textureCacheUsageBytes <= textureCacheBudgetBytes) {
                break;
            }

            if (honorRecency && (nowSec - candidate.lastUsedTime) < 2.0) {
                continue;
            }

            auto& cache = *candidate.cache;
            auto it = cache.find(candidate.path);
            if (it == cache.end()) continue;

            textureCacheUsageBytes = (textureCacheUsageBytes > it->second.approxBytes)
                ? (textureCacheUsageBytes - it->second.approxBytes)
                : 0;
            cache.erase(it);
            ++textureCacheGeneration;

            if (!honorRecency && !warnedTextureCacheThrashing) {
                warnedTextureCacheThrashing = true;
                std::cerr << "[Renderer] Texture cache is over budget. Evicting textures that "
                             "were on screen recently, which costs a reload each. Lower the "
                             "texture size cap in Graphics settings (currently "
                          << Texture::GetMaxDimension() << ") to fix the stutter this causes.\n";
            }
        }
        if (textureCacheUsageBytes <= textureCacheBudgetBytes) break;
    }

    if (textureCacheUsageBytes > textureCacheBudgetBytes && !warnedTextureCacheOverCommitted) {
        warnedTextureCacheOverCommitted = true;
        std::cerr << "[Renderer] Texture working set is "
                  << (textureCacheUsageBytes / (1024 * 1024)) << " MB against a "
                  << (textureCacheBudgetBytes / (1024 * 1024))
                  << " MB budget and every texture is in use this frame, so none can be "
                     "evicted. Lower Graphics > Max Texture Size (currently "
                  << Texture::GetMaxDimension() << ").\n";
    }
}

void Renderer::purgePreviewTargetsIfNeeded(int keepSlot) {
    constexpr size_t kMaxExtraPreviewTargets = 192;
    if (extraPreviewTargets.size() <= kMaxExtraPreviewTargets) {
        return;
    }

    struct PreviewTargetCandidate {
        int slot = 0;
        uint64_t lastUsed = 0;
    };

    std::vector<PreviewTargetCandidate> candidates;
    candidates.reserve(extraPreviewTargets.size());
    for (const auto& [slot, target] : extraPreviewTargets) {
        (void)target;
        if (slot == keepSlot) continue;
        auto usedIt = extraPreviewTargetLastUsed.find(slot);
        candidates.push_back({slot, usedIt != extraPreviewTargetLastUsed.end() ? usedIt->second : 0});
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const PreviewTargetCandidate& a, const PreviewTargetCandidate& b) {
                  if (a.lastUsed != b.lastUsed) return a.lastUsed < b.lastUsed;
                  return a.slot < b.slot;
              });

    for (const PreviewTargetCandidate& candidate : candidates) {
        if (extraPreviewTargets.size() <= kMaxExtraPreviewTargets) {
            break;
        }
        auto targetIt = extraPreviewTargets.find(candidate.slot);
        if (targetIt == extraPreviewTargets.end()) continue;
        releaseRenderTarget(targetIt->second);
        extraPreviewTargets.erase(targetIt);
        extraPreviewTargetLastUsed.erase(candidate.slot);
    }
}

void Renderer::rebuildStaticMergeBatches(const std::vector<SceneObject>& sceneObjects) {
    if (lastStaticMergeCheckFrameSerial == frameSerial) {
        return;
    }
    lastStaticMergeCheckFrameSerial = frameSerial;

    // The optimized path caches which objects were merged; any change here invalidates it.
    const auto noteMergedSetChanged = [this]() {
        if (optState) ++optState->staticMergeGeneration;
    };

    constexpr size_t kMinSceneObjectsForStaticMerge = 8;
    if (sceneObjects.size() < kMinSceneObjectsForStaticMerge) {
        staticMergeSceneSignature = 0;
        staticMergeBatches.clear();
        staticMergeSourceIds.clear();
        noteMergedSetChanged();
        return;
    }

    auto isStaticMergeBatchable = [&](const SceneObject& obj) {
        if (!IsStaticMergeCandidate(obj)) return false;
        if (obj.material.alpha < 0.999f) return false;
        if (obj.renderType == RenderType::Sprite && !HasDefaultSpriteUvRect(obj)) {
            return false;
        }
        if (!obj.albedoTexturePath.empty()) {
            if (Texture* tex = getTexture(obj.albedoTexturePath, obj.material.textureFilter)) {
                if (tex->UsesAlphaBlending()) {
                    return false;
                }
            }
        }
        if (obj.useOverlay && !obj.overlayTexturePath.empty()) {
            if (Texture* tex = getTexture(obj.overlayTexturePath, obj.material.textureFilter)) {
                if (tex->UsesAlphaBlending()) {
                    return false;
                }
            }
        }
        return true;
    };

    uint64_t signature = 1469598103934665603ull;
    size_t candidateCount = 0;
    for (const auto& obj : sceneObjects) {
        if (!isStaticMergeBatchable(obj)) continue;
        ++candidateCount;
        signature = HashCombine64(signature, static_cast<uint64_t>(obj.id));
        signature = HashCombine64(signature, static_cast<uint64_t>(obj.meshId + 1));
        signature = HashCombine64(signature, HashFloat64(obj.position.x));
        signature = HashCombine64(signature, HashFloat64(obj.position.y));
        signature = HashCombine64(signature, HashFloat64(obj.position.z));
        signature = HashCombine64(signature, HashFloat64(obj.rotation.x));
        signature = HashCombine64(signature, HashFloat64(obj.rotation.y));
        signature = HashCombine64(signature, HashFloat64(obj.rotation.z));
        signature = HashCombine64(signature, HashFloat64(obj.scale.x));
        signature = HashCombine64(signature, HashFloat64(obj.scale.y));
        signature = HashCombine64(signature, HashFloat64(obj.scale.z));
        signature = HashStaticMergeMaterialKey(obj, signature);
    }
    signature = HashCombine64(signature, static_cast<uint64_t>(candidateCount));

    if (candidateCount < 2) {
        staticMergeSceneSignature = 0;
        staticMergeBatches.clear();
        staticMergeSourceIds.clear();
        noteMergedSetChanged();
        return;
    }

    if (signature == staticMergeSceneSignature) {
        return;
    }

    staticMergeSceneSignature = signature;
    staticMergeBatches.clear();
    staticMergeSourceIds.clear();
    noteMergedSetChanged();

    struct BatchAccumulator {
        Renderer::StaticMergeBatch batch;
        std::vector<float> vertices;
        glm::vec3 boundsMin = glm::vec3(FLT_MAX);
        glm::vec3 boundsMax = glm::vec3(-FLT_MAX);
    };

    std::unordered_map<std::string, BatchAccumulator> accumulators;
    accumulators.reserve(candidateCount);

    for (const auto& obj : sceneObjects) {
        if (!isStaticMergeBatchable(obj)) continue;

        const std::vector<float>* sourceVertices = nullptr;
        if (!TryGetObjectTriangleVertexStream(obj, sourceVertices) || !sourceVertices || sourceVertices->empty()) {
            continue;
        }

        std::string batchKey;
        batchKey.reserve(512);
        AppendStaticMergeMaterialKey(obj, batchKey);

        BatchAccumulator& acc = accumulators[batchKey];
        if (acc.batch.albedoTexturePath.empty() && acc.batch.normalMapPath.empty() &&
            acc.batch.overlayTexturePath.empty() && acc.batch.materialPath.empty() &&
            acc.batch.vertPath.empty() && acc.batch.fragPath.empty() && acc.vertices.empty()) {
            acc.batch.vertPath = obj.vertexShaderPath;
            acc.batch.fragPath = obj.fragmentShaderPath;
            acc.batch.material = obj.material;
            acc.batch.materialPath = obj.materialPath;
            acc.batch.albedoTexturePath = obj.albedoTexturePath;
            acc.batch.overlayTexturePath = obj.overlayTexturePath;
            acc.batch.normalMapPath = obj.normalMapPath;
            acc.batch.metallicRoughnessMapPath = obj.metallicRoughnessMapPath;
            acc.batch.metallicMapPath = obj.metallicMapPath;
            acc.batch.roughnessMapPath = obj.roughnessMapPath;
            acc.batch.opacityMapPath = obj.opacityMapPath;
            acc.batch.emissiveMapPath = obj.emissiveMapPath;
            acc.batch.occlusionMapPath = obj.occlusionMapPath;
            acc.batch.heightMapPath = obj.heightMapPath;
            acc.batch.useOverlay = obj.useOverlay;
            acc.batch.unlit = (obj.renderType == RenderType::Sprite);
            acc.batch.doubleSided = (obj.renderType == RenderType::Sprite);
        }

        AppendTransformedTriangleVertices(*sourceVertices, BuildSceneObjectModelMatrix(obj),
                                         acc.vertices, acc.boundsMin, acc.boundsMax);
        staticMergeSourceIds.insert(obj.id);
    }

    for (auto& [key, acc] : accumulators) {
        (void)key;
        if (acc.vertices.empty()) continue;
        StaticMergeBatch batch = std::move(acc.batch);
        batch.mesh = std::make_unique<Mesh>(acc.vertices.data(), acc.vertices.size() * sizeof(float));
        batch.boundsCenter = (acc.boundsMin + acc.boundsMax) * 0.5f;
        batch.boundsRadius = glm::length(glm::max(acc.boundsMax - batch.boundsCenter, glm::vec3(0.001f)));
        staticMergeBatches.push_back(std::move(batch));
    }
}

void Renderer::initialize() {
#if MODULARITY_RUNTIME_ONLY
    autoReloadShaders = false;
#endif
    auto requireFile = [](const std::string& path, const char* label) {
        if (!Modularity::Platform::GetAssetSource().Exists(path)) {
            throw std::runtime_error(std::string(label) + " not found: " + path);
        }
    };
    requireFile(defaultVertPath, "Default vertex shader");
    requireFile(defaultFragPath, "Default fragment shader");

    GLint maxTextureSize = 4096;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    uiAtlasMaxSize = std::clamp(static_cast<int>(maxTextureSize), 1024, 4096);

    shader = new Shader(defaultVertPath.c_str(), defaultFragPath.c_str());
    defaultShader = shader;
    if (shader->ID == 0) {
        std::cerr << "Shader compilation failed: " << defaultVertPath
                  << " + " << defaultFragPath << "\n";
        delete shader;
        shader = nullptr;
        defaultShader = nullptr;
        throw std::runtime_error("Default shader compilation failed: " +
                                 defaultVertPath + " + " + defaultFragPath);
    }
    postShader = new Shader(postVertPath.c_str(), postFragPath.c_str());
    if (!postShader || postShader->ID == 0) {
        std::cerr << "PostFX shader compilation failed!\n";
        delete postShader;
        postShader = nullptr;
    } else {
        postShader->use();
        postShader->setInt("sceneTex", 0);
        postShader->setInt("bloomTex", 1);
        postShader->setInt("historyTex", 2);
    }
    compositeShader = new Shader(postVertPath.c_str(), postCompositeFragPath.c_str());
    if (!compositeShader || compositeShader->ID == 0) {
        std::cerr << "PostFX composite shader compilation failed!\n";
        delete compositeShader;
        compositeShader = nullptr;
    } else {
        compositeShader->use();
        compositeShader->setInt("sourceTex", 0);
    }
    brightShader = new Shader(postVertPath.c_str(), postBrightFragPath.c_str());
    if (!brightShader || brightShader->ID == 0) {
        std::cerr << "Bright-pass shader compilation failed!\n";
        delete brightShader;
        brightShader = nullptr;
    } else {
        brightShader->use();
        brightShader->setInt("sceneTex", 0);
    }

    blurShader = new Shader(postVertPath.c_str(), postBlurFragPath.c_str());
    if (!blurShader || blurShader->ID == 0) {
        std::cerr << "Blur shader compilation failed!\n";
        delete blurShader;
        blurShader = nullptr;
    } else {
        blurShader->use();
        blurShader->setInt("image", 0);
    }
    shadowDepthShader = new Shader(shadowDepthVertPath.c_str(), shadowDepthFragPath.c_str());
    if (!shadowDepthShader || shadowDepthShader->ID == 0) {
        std::cerr << "Shadow depth shader compilation failed; shadows will be disabled.\n";
        delete shadowDepthShader;
        shadowDepthShader = nullptr;
    }
    directionalShadowDepthShader = new Shader(directionalShadowDepthVertPath.c_str(), directionalShadowDepthFragPath.c_str());
    if (!directionalShadowDepthShader || directionalShadowDepthShader->ID == 0) {
        std::cerr << "Directional shadow depth shader compilation failed; directional shadows will be disabled.\n";
        delete directionalShadowDepthShader;
        directionalShadowDepthShader = nullptr;
    }
    ShaderEntry entry;
    entry.shader.reset(defaultShader);
    entry.vertPath = defaultVertPath;
    entry.fragPath = defaultFragPath;
    entry.nextReloadCheckTime = glfwGetTime() + 0.5;
    if (fs::exists(defaultVertPath)) entry.vertTime = fs::last_write_time(defaultVertPath);
    if (fs::exists(defaultFragPath)) entry.fragTime = fs::last_write_time(defaultFragPath);
    shaderCache[defaultVertPath + "|" + defaultFragPath] = std::move(entry);

    texture1 = new Texture("Resources/Textures/container.jpg");
    texture2 = new Texture("Resources/Textures/awesomeface.png");
    if (debugWhiteTexture == 0) {
        unsigned char white[4] = { 255, 255, 255, 255 };
        glGenTextures(1, &debugWhiteTexture);
        glBindTexture(GL_TEXTURE_2D, debugWhiteTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    if (missingMaterialFallbackTexture == 0) {
        constexpr int kFallbackSize = 64;
        std::vector<unsigned char> pixels(kFallbackSize * kFallbackSize * 4, 255);
        const glm::vec3 centerColor(40.0f, 24.0f, 58.0f);
        const glm::vec3 edgeColor(70.0f, 46.0f, 102.0f);
        const float invHalfDiag = 1.0f / std::sqrt(0.5f * 0.5f + 0.5f * 0.5f);
        for (int y = 0; y < kFallbackSize; ++y) {
            for (int x = 0; x < kFallbackSize; ++x) {
                float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(kFallbackSize) - 0.5f;
                float fy = (static_cast<float>(y) + 0.5f) / static_cast<float>(kFallbackSize) - 0.5f;
                float dist = std::sqrt(fx * fx + fy * fy) * invHalfDiag;
                dist = std::clamp(dist, 0.0f, 1.0f);
                float t = dist * dist * (3.0f - 2.0f * dist);
                glm::vec3 rgb = centerColor * (1.0f - t) + edgeColor * t;
                size_t idx = static_cast<size_t>(y * kFallbackSize + x) * 4;
                pixels[idx + 0] = static_cast<unsigned char>(std::clamp(rgb.r, 0.0f, 255.0f));
                pixels[idx + 1] = static_cast<unsigned char>(std::clamp(rgb.g, 0.0f, 255.0f));
                pixels[idx + 2] = static_cast<unsigned char>(std::clamp(rgb.b, 0.0f, 255.0f));
                pixels[idx + 3] = 255;
            }
        }

        glGenTextures(1, &missingMaterialFallbackTexture);
        glBindTexture(GL_TEXTURE_2D, missingMaterialFallbackTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kFallbackSize, kFallbackSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    cubeMesh = new Mesh(vertices, sizeof(vertices));

    auto sphereVerts = generateSphere();
    sphereMesh = new Mesh(sphereVerts.data(), sphereVerts.size() * sizeof(float));

    auto capsuleVerts = generateCapsule();
    capsuleMesh = new Mesh(capsuleVerts.data(), capsuleVerts.size() * sizeof(float));
    planeMesh = new Mesh(mirrorPlaneVertices, sizeof(mirrorPlaneVertices));
    auto torusVerts = generateTorus();
    torusMesh = new Mesh(torusVerts.data(), torusVerts.size() * sizeof(float));

    skybox = new Skybox();

    setupFBO();
    ensureRenderTarget(postTarget, currentWidth, currentHeight, false, true);
    ensureRenderTarget(historyTarget, currentWidth, currentHeight, false, true);
    ensureRenderTarget(bloomTargetA, currentWidth, currentHeight, false, true);
    ensureRenderTarget(bloomTargetB, currentWidth, currentHeight, false, true);
    ensureRenderTarget(selectionMaskTarget, currentWidth, currentHeight);
    ensureQuad();
    clearHistory();
    glEnable(GL_DEPTH_TEST);
}

Shader* Renderer::getShader(const std::string& vert, const std::string& frag) {
    return getShader(vert, std::string(), frag);
}

Shader* Renderer::getShader(const std::string& vert, const std::string& geom,
                            const std::string& frag) {
    std::string vPath = vert.empty() ? defaultVertPath : vert;
    std::string fPath = frag.empty() ? defaultFragPath : frag;
    const std::string& gPath = geom;
    std::string key = vPath + "|" + gPath + "|" + fPath;

    auto makeShader = [&]() {
        return gPath.empty()
            ? std::make_unique<Shader>(vPath.c_str(), fPath.c_str())
            : std::make_unique<Shader>(vPath.c_str(), gPath.c_str(), fPath.c_str());
    };

    auto stampTimes = [&](ShaderEntry& e) {
        if (fs::exists(vPath)) e.vertTime = fs::last_write_time(vPath);
        if (fs::exists(fPath)) e.fragTime = fs::last_write_time(fPath);
        if (!gPath.empty() && fs::exists(gPath)) e.geomTime = fs::last_write_time(gPath);
    };

    auto reloadEntry = [&](ShaderEntry& e) -> Shader* {
        std::unique_ptr<Shader> newShader = makeShader();
        if (!newShader || newShader->ID == 0) {
            std::cerr << "Shader reload failed for " << key << ", falling back to default\n";
            return defaultShader;
        }
        e.shader = std::move(newShader);
        e.vertPath = vPath;
        e.fragPath = fPath;
        e.geomPath = gPath;
        stampTimes(e);
        return e.shader.get();
    };

    auto it = shaderCache.find(key);
    if (it != shaderCache.end()) {
        ShaderEntry& entry = it->second;
        if (autoReloadShaders) {
            const double nowSec = glfwGetTime();
            if (nowSec >= entry.nextReloadCheckTime) {
                entry.nextReloadCheckTime = nowSec + 0.5;
                bool changed = false;
                if (fs::exists(vPath)) {
                    auto t = fs::last_write_time(vPath);
                    if (t != entry.vertTime) { changed = true; entry.vertTime = t; }
                }
                if (fs::exists(fPath)) {
                    auto t = fs::last_write_time(fPath);
                    if (t != entry.fragTime) { changed = true; entry.fragTime = t; }
                }
                if (!gPath.empty() && fs::exists(gPath)) {
                    auto t = fs::last_write_time(gPath);
                    if (t != entry.geomTime) { changed = true; entry.geomTime = t; }
                }
                if (changed) {
                    return reloadEntry(entry);
                }
            }
        }
        return entry.shader ? entry.shader.get() : defaultShader;
    }

    ShaderEntry entry;
    entry.vertPath = vPath;
    entry.fragPath = fPath;
    entry.geomPath = gPath;
    entry.nextReloadCheckTime = glfwGetTime() + 0.5;
    stampTimes(entry);
    entry.shader = makeShader();
    if (!entry.shader || entry.shader->ID == 0) {
        std::cerr << "Shader compile failed for " << key << ", using default\n";
        shaderCache[key] = std::move(entry);
        return defaultShader;
    }
    Shader* ptr = entry.shader.get();
    shaderCache[key] = std::move(entry);
    return ptr;
}

bool Renderer::forceReloadShader(const std::string& vert, const std::string& frag) {
    std::string vPath = vert.empty() ? defaultVertPath : vert;
    std::string fPath = frag.empty() ? defaultFragPath : frag;
    std::string key = vPath + "|" + fPath;
    auto it = shaderCache.find(key);
    if (it != shaderCache.end()) {
        shaderCache.erase(it);
    }
    ShaderEntry entry;
    entry.vertPath = vPath;
    entry.fragPath = fPath;
    entry.nextReloadCheckTime = glfwGetTime() + 0.5;
    if (fs::exists(vPath)) entry.vertTime = fs::last_write_time(vPath);
    if (fs::exists(fPath)) entry.fragTime = fs::last_write_time(fPath);
    entry.shader = std::make_unique<Shader>(vPath.c_str(), fPath.c_str());
    if (!entry.shader || entry.shader->ID == 0) {
        std::cerr << "Shader force reload failed for " << key << "\n";
        return false;
    }
    if (vPath == defaultVertPath && fPath == defaultFragPath) {
        defaultShader = entry.shader.get();
    }
    shaderCache[key] = std::move(entry);
    return true;
}

namespace {

void allocateDepthStencilTexture(int width, int height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

} // namespace

void Renderer::setupFBO() {
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    glGenTextures(1, &viewportTexture);
    glBindTexture(GL_TEXTURE_2D, viewportTexture);
    {
        const bool sdr = (colorPrecision == RendererColorPrecision::SDR8);
        glTexImage2D(GL_TEXTURE_2D, 0, sdr ? GL_RGBA8 : GL_RGBA16F, currentWidth, currentHeight,
                     0, GL_RGBA, sdr ? GL_UNSIGNED_BYTE : GL_FLOAT, NULL);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, viewportTexture, 0);

    glGenTextures(1, &sceneDepthTexture);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTexture);
    allocateDepthStencilTexture(currentWidth, currentHeight);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                           sceneDepthTexture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Framebuffer setup failed!\n";
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    displayTexture = viewportTexture;
}

void Renderer::ensureRenderTarget(RenderTarget& target, int w, int h) {
    ensureRenderTarget(target, w, h, false, false);
}

void Renderer::ensureRenderTarget(RenderTarget& target, int w, int h, bool alpha) {
    ensureRenderTarget(target, w, h, alpha, false);
}

void Renderer::ensureRenderTarget(RenderTarget& target, int w, int h, bool alpha, bool hdr) {
    ensureRenderTarget(target, w, h, alpha, hdr, true);
}

void Renderer::ensureRenderTarget(RenderTarget& target, int w, int h, bool alpha, bool hdr, bool depth) {
    RenderTargetOptions options;
    options.alpha = alpha;
    options.hdr = hdr;
    options.depth = depth;
    ensureRenderTarget(target, w, h, options);
}

void Renderer::ensureRenderTarget(RenderTarget& target, int w, int h, const RenderTargetOptions& options) {
    bool alpha = options.alpha;
    bool hdr = options.hdr;
    const bool depth = options.depth;
    const bool depthSampleable = options.depthSampleable && depth;
    if (w <= 0 || h <= 0) return;

    if (colorPrecision == RendererColorPrecision::SDR8) hdr = false;

    if (target.fbo == 0) {
        glGenFramebuffers(1, &target.fbo);
        glGenTextures(1, &target.texture);
    }
    if (depth && !depthSampleable && target.rbo == 0) {
        glGenRenderbuffers(1, &target.rbo);
    }
    if (depthSampleable && target.depthTexture == 0) {
        glGenTextures(1, &target.depthTexture);
    }

    if (target.width == w && target.height == h && target.hasAlpha == alpha && target.hdr == hdr &&
        target.hasDepth == depth && target.depthSampleable == depthSampleable) return;

    target.width = w;
    target.height = h;
    target.hasAlpha = alpha;
    target.hdr = hdr;
    target.hasDepth = depth;
    target.depthSampleable = depthSampleable;

    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);

    glBindTexture(GL_TEXTURE_2D, target.texture);
    GLenum internalFormat = GL_RGB8;
    GLenum dataFormat = GL_RGB;
    GLenum dataType = GL_UNSIGNED_BYTE;
    if (hdr) {
        internalFormat = alpha ? GL_RGBA16F : GL_RGBA16F;
        dataFormat = alpha ? GL_RGBA : GL_RGBA;
        dataType = GL_FLOAT;
    } else if (alpha) {
        internalFormat = GL_RGBA8;
        dataFormat = GL_RGBA;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, target.width, target.height, 0, dataFormat, dataType, NULL);
    bool isSelectionMask = (&target == &selectionMaskTarget);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, isSelectionMask ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, isSelectionMask ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    target.textureFilter = isSelectionMask ? MaterialProperties::TextureFilter::Point
                                           : MaterialProperties::TextureFilter::Bilinear;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);

    if (depth && depthSampleable) {
        glBindTexture(GL_TEXTURE_2D, target.depthTexture);
        allocateDepthStencilTexture(target.width, target.height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                               target.depthTexture, 0);
        if (target.rbo != 0) {
            glDeleteRenderbuffers(1, &target.rbo);
            target.rbo = 0;
        }
    } else if (depth) {
        glBindRenderbuffer(GL_RENDERBUFFER, target.rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, target.width, target.height);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, target.rbo);
        if (target.depthTexture != 0) {
            glDeleteTextures(1, &target.depthTexture);
            target.depthTexture = 0;
        }
    } else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        if (target.rbo != 0) {
            glDeleteRenderbuffers(1, &target.rbo);
            target.rbo = 0;
        }
        if (target.depthTexture != 0) {
            glDeleteTextures(1, &target.depthTexture);
            target.depthTexture = 0;
        }
    }

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Preview framebuffer setup failed!\n";
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::releaseRenderTarget(RenderTarget& target) {
    if (target.texture) {
        glDeleteTextures(1, &target.texture);
    }
    if (target.rbo) {
        glDeleteRenderbuffers(1, &target.rbo);
    }
    if (target.depthTexture) {
        glDeleteTextures(1, &target.depthTexture);
    }
    if (target.fbo) {
        glDeleteFramebuffers(1, &target.fbo);
    }
    target = {};
}

bool Renderer::giSupported() const {
    return giUnsupportedReason().empty();
}

std::string Renderer::giUnsupportedReason() const {
#if MODULARITY_OPENGL_ES
    return "the OpenGL ES backend has no geometry stage and no image load/store";
#else
    const auto& info = Modularity::HardwareProfile::Static();
    if (info.glMajor <= 0) {
        return "no OpenGL context yet";
    }
    if (!Modularity::HardwareProfile::HasGL(4, 3)) {
        return "needs an OpenGL 4.3 context; this driver granted " +
               std::to_string(info.glMajor) + "." + std::to_string(info.glMinor);
    }
    if (info.maxTextureImageUnits < 17) {
        return "needs 17 fragment texture units; this driver reports " +
               std::to_string(info.maxTextureImageUnits);
    }
    return std::string();
#endif
}

bool Renderer::ensureVoxelVolume(int resolution, int volumeCount) {
#if MODULARITY_OPENGL_ES
    (void)resolution; (void)volumeCount;
    return false;
#else
    resolution = std::clamp(resolution, 32, 256);
    volumeCount = std::clamp(volumeCount, 1, 2);
    if (voxelVolume.texture[0] != 0 && voxelVolume.resolution == resolution &&
        voxelVolume.allocated == volumeCount) {
        return true;
    }
    releaseVoxelVolume();

    int levels = 1;
    for (int r = resolution; r > 1; r >>= 1) ++levels;

    for (int i = 0; i < volumeCount; ++i) {
        glGenTextures(1, &voxelVolume.texture[i]);
        glBindTexture(GL_TEXTURE_3D, voxelVolume.texture[i]);
        glTexStorage3D(GL_TEXTURE_3D, levels, GL_RGBA16F, resolution, resolution, resolution);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_3D, 0);
    voxelVolume.allocated = volumeCount;
    voxelVolume.resultIndex = 0;

    glGenFramebuffers(1, &voxelVolume.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, voxelVolume.fbo);
    glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_WIDTH, resolution);
    glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_HEIGHT, resolution);
    glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_LAYERS, 1);
    glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_SAMPLES, 0);
    glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_FIXED_SAMPLE_LOCATIONS, GL_FALSE);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (!complete || voxelVolume.texture[0] == 0) {
        std::cerr << "Voxel GI volume setup failed at resolution " << resolution << "\n";
        releaseVoxelVolume();
        return false;
    }

    voxelVolume.resolution = resolution;
    voxelVolume.hasData = false;
    voxelSceneSignature = 0;
    return true;
#endif
}

void Renderer::releaseVoxelVolume() {
    for (int i = 0; i < 2; ++i) {
        if (voxelVolume.texture[i]) glDeleteTextures(1, &voxelVolume.texture[i]);
    }
    if (voxelVolume.fbo) glDeleteFramebuffers(1, &voxelVolume.fbo);
    voxelVolume = VoxelVolume{};
}

void Renderer::ensureSurfaceTarget(int w, int h, unsigned int depthTexture) {
    if (w <= 0 || h <= 0 || depthTexture == 0) return;

    if (surfaceTarget.fbo == 0) {
        glGenFramebuffers(1, &surfaceTarget.fbo);
        glGenTextures(1, &surfaceTarget.texture);
    }
    const bool sizeChanged = (surfaceTarget.width != w || surfaceTarget.height != h);
    const bool depthChanged = (surfaceTarget.attachedDepth != depthTexture);
    if (!sizeChanged && !depthChanged) return;

    glBindFramebuffer(GL_FRAMEBUFFER, surfaceTarget.fbo);

    if (sizeChanged) {
        surfaceTarget.width = w;
        surfaceTarget.height = h;
        glBindTexture(GL_TEXTURE_2D, surfaceTarget.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               surfaceTarget.texture, 0);
    }

    surfaceTarget.attachedDepth = depthTexture;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                           depthTexture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Surface (G-buffer) framebuffer setup failed!\n";
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

Renderer::XREyePreviewResult Renderer::renderXREyePreview(
    const Camera& camera, const std::vector<SceneObject>& sceneObjects, int eyeIndex,
    int width, int height, const glm::mat4& projection, float nearPlane, float farPlane) {
    XREyePreviewResult result;
    if (width <= 0 || height <= 0) return result;
    eyeIndex = std::clamp(eyeIndex, 0, 1);
    XREyePreview& eye = xrEyePreview[eyeIndex];

    ensureRenderTarget(eye.target, width, height, false, true, true);
    if (eye.target.fbo == 0 || eye.target.texture == 0) return result;

#if !MODULARITY_OPENGL_ES
    const bool timersUsable = (GLAD_GL_VERSION_3_3 != 0);
    if (timersUsable && eye.startQuery[0] == 0) {
        glGenQueries(2, eye.startQuery);
        glGenQueries(2, eye.endQuery);
    }

    const int writeSlot = eye.writeSlot;
    const int readSlot = 1 - writeSlot;
    if (timersUsable && eye.pending[readSlot]) {
        GLint available = 0;
        glGetQueryObjectiv(eye.endQuery[readSlot], GL_QUERY_RESULT_AVAILABLE, &available);
        if (available) {
            GLuint64 began = 0;
            GLuint64 ended = 0;
            glGetQueryObjectui64v(eye.startQuery[readSlot], GL_QUERY_RESULT, &began);
            glGetQueryObjectui64v(eye.endQuery[readSlot], GL_QUERY_RESULT, &ended);
            eye.lastGpuMs = (ended >= began) ? static_cast<double>(ended - began) / 1.0e6 : -1.0;
            eye.pending[readSlot] = false;
        }
    }
    if (timersUsable && eye.startQuery[writeSlot] != 0) {
        glQueryCounter(eye.startQuery[writeSlot], GL_TIMESTAMP);
    }
#endif

    const RenderStats savedViewportStats = viewportStats;
    renderSceneToTarget(camera, sceneObjects, eye.target.fbo, width, height, projection,
                        nearPlane, farPlane, true);
    result.stats = viewportStats;
    viewportStats = savedViewportStats;

#if !MODULARITY_OPENGL_ES
    if (timersUsable && eye.endQuery[writeSlot] != 0) {
        glQueryCounter(eye.endQuery[writeSlot], GL_TIMESTAMP);
        eye.pending[writeSlot] = true;
        eye.writeSlot = readSlot;
    }
#endif

    result.texture = eye.target.texture;
    result.gpuMs = eye.lastGpuMs;
    return result;
}

void Renderer::releaseXREyePreviews() {
    for (XREyePreview& eye : xrEyePreview) {
        releaseRenderTarget(eye.target);
#if !MODULARITY_OPENGL_ES
        if (eye.startQuery[0] != 0) {
            glDeleteQueries(2, eye.startQuery);
            glDeleteQueries(2, eye.endQuery);
        }
#endif
        eye = XREyePreview{};
    }
}

void Renderer::releaseSurfaceTarget() {
    if (surfaceTarget.texture) glDeleteTextures(1, &surfaceTarget.texture);
    if (surfaceTarget.fbo) glDeleteFramebuffers(1, &surfaceTarget.fbo);
    surfaceTarget = SurfaceBufferTarget{};
}

void Renderer::releaseReflectionCastTarget(ReflectionCastTarget& target) {
    if (target.depth) {
        glDeleteRenderbuffers(1, &target.depth);
    }
    if (target.cube) {
        glDeleteTextures(1, &target.cube);
    }
    if (target.fbo) {
        glDeleteFramebuffers(1, &target.fbo);
    }
    target = {};
}

void Renderer::releaseSkyboxReflectionTarget(SkyboxReflectionTarget& target) {
    if (target.depth) {
        glDeleteRenderbuffers(1, &target.depth);
    }
    if (target.cube) {
        glDeleteTextures(1, &target.cube);
    }
    if (target.fbo) {
        glDeleteFramebuffers(1, &target.fbo);
    }
    target = {};
}

void Renderer::updateSkyboxReflectionTarget() {
    if (!skybox) return;
    const SkyboxSettings& settings = skybox->getSettings();
    if (!settings.environmentReflections || settings.environmentReflectionIntensity <= 0.001f) {
        if (skyboxReflectionTarget.fbo || skyboxReflectionTarget.cube || skyboxReflectionTarget.depth) {
            releaseSkyboxReflectionTarget(skyboxReflectionTarget);
        }
        return;
    }

    uint64_t signature = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        signature ^= v;
        signature *= 1099511628211ull;
    };
    auto mixFloat = [&](float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        mix(bits);
    };
    mix(static_cast<uint64_t>(settings.mode));
    mixFloat(skybox->getTimeOfDay());
    mixFloat(settings.scrollingRepeatX);
    mixFloat(settings.scrollingRepeatY);
    mixFloat(settings.scrollingLookSensitivity);
    mixFloat(settings.scrollingVerticalInfluence);
    for (char c : settings.sunTexturePath) mix(static_cast<unsigned char>(c));
    for (char c : settings.moonTexturePath) mix(static_cast<unsigned char>(c));
    for (char c : settings.scrollingTexturePath) mix(static_cast<unsigned char>(c));

    constexpr int resolution = 128;
    const bool targetMissing = skyboxReflectionTarget.fbo == 0 || skyboxReflectionTarget.cube == 0 || skyboxReflectionTarget.depth == 0;
    const bool mustCapture = targetMissing ||
        skyboxReflectionTarget.resolution != resolution ||
        !skyboxReflectionTarget.hasCapture;
    if (!mustCapture && skyboxReflectionTarget.signature == signature) return;

    constexpr double kMinRecaptureIntervalSec = 0.25;
    const double nowSec = glfwGetTime();
    if (!mustCapture &&
        skyboxReflectionTarget.lastCaptureSec >= 0.0 &&
        nowSec - skyboxReflectionTarget.lastCaptureSec < kMinRecaptureIntervalSec) {
        return;
    }

    if (targetMissing || skyboxReflectionTarget.resolution != resolution) {
        releaseSkyboxReflectionTarget(skyboxReflectionTarget);
        skyboxReflectionTarget.resolution = resolution;
        glGenFramebuffers(1, &skyboxReflectionTarget.fbo);
        glGenTextures(1, &skyboxReflectionTarget.cube);
        glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxReflectionTarget.cube);
        for (int face = 0; face < 6; ++face) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB16F,
                         resolution, resolution, 0, GL_RGB, GL_FLOAT, nullptr);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glGenRenderbuffers(1, &skyboxReflectionTarget.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, skyboxReflectionTarget.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, resolution, resolution);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }

    GLint prevFBO = 0;
    GLint prevViewport[4] = {0, 0, currentWidth, currentHeight};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    const glm::vec3 faceDirs[6] = {
        glm::vec3(1.0f, 0.0f, 0.0f),
        glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(0.0f, 0.0f, -1.0f),
    };
    const glm::vec3 faceUps[6] = {
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(0.0f, 0.0f, -1.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
    };
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
    for (int face = 0; face < 6; ++face) {
        glBindFramebuffer(GL_FRAMEBUFFER, skyboxReflectionTarget.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                               skyboxReflectionTarget.cube, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, skyboxReflectionTarget.depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            break;
        }
        glViewport(0, 0, resolution, resolution);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), faceDirs[face], faceUps[face]);
        renderSkybox(view, proj);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    skyboxReflectionTarget.signature = signature;
    skyboxReflectionTarget.hasCapture = true;
    skyboxReflectionTarget.lastCaptureSec = nowSec;
}

void Renderer::updateReflectionCastTargets(const std::vector<SceneObject>& sceneObjects, float nearPlane, float farPlane) {
    if (sceneObjects.empty() || reflectionCapturePass) return;

    std::unordered_set<int> active;
    GLint prevFBO = 0;
    GLint prevViewport[4] = {0, 0, currentWidth, currentHeight};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    const glm::vec3 faceDirs[6] = {
        glm::vec3(1.0f, 0.0f, 0.0f),
        glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(0.0f, 0.0f, -1.0f),
    };
    const glm::vec3 faceUps[6] = {
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(0.0f, 0.0f, -1.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, -1.0f, 0.0f),
    };

    for (const SceneObject& obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasReflectionCast || !obj.reflectionCast.enabled) continue;
        active.insert(obj.id);

        ReflectionCastTarget& target = reflectionCastTargets[obj.id];
        const int resolution = std::clamp(obj.reflectionCast.resolution, 32, 1024);
        const bool targetMissing = target.fbo == 0 || target.cube == 0 || target.depth == 0;
        const bool sizeChanged = target.resolution != resolution;
        const bool shouldCapture = targetMissing ||
            sizeChanged ||
            obj.reflectionCast.updateMode == ReflectionCastUpdateMode::EveryFrame ||
            !obj.reflectionCast.baked ||
            !target.hasCapture;
        if (!shouldCapture) continue;

        if (targetMissing || sizeChanged) {
            releaseReflectionCastTarget(target);
            target.resolution = resolution;
            glGenFramebuffers(1, &target.fbo);
            glGenTextures(1, &target.cube);
            glBindTexture(GL_TEXTURE_CUBE_MAP, target.cube);
            for (int face = 0; face < 6; ++face) {
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB16F,
                             resolution, resolution, 0, GL_RGB, GL_FLOAT, nullptr);
            }
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            glGenRenderbuffers(1, &target.depth);
            glBindRenderbuffer(GL_RENDERBUFFER, target.depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, resolution, resolution);
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
        }
        if (target.fbo == 0 || target.cube == 0 || target.depth == 0) continue;

        reflectionCapturePass = true;
        for (int face = 0; face < 6; ++face) {
            glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, target.cube, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, target.depth);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                break;
            }
            glViewport(0, 0, resolution, resolution);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            Camera captureCamera;
            captureCamera.position = obj.position;
            captureCamera.front = faceDirs[face];
            captureCamera.up = faceUps[face];
            renderSceneInternal(captureCamera, sceneObjects, resolution, resolution, false, 90.0f, nearPlane, farPlane, false, true);
        }
        reflectionCapturePass = false;
        target.hasCapture = true;
        const_cast<SceneObject&>(obj).reflectionCast.baked = true;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);

    for (auto it = reflectionCastTargets.begin(); it != reflectionCastTargets.end(); ) {
        if (active.find(it->first) == active.end()) {
            releaseReflectionCastTarget(it->second);
            it = reflectionCastTargets.erase(it);
        } else {
            ++it;
        }
    }
}

void Renderer::updateMirrorTargets(const Camera& camera, const std::vector<SceneObject>& sceneObjects, int width, int height, float fovDeg, float nearPlane, float farPlane) {
    if (camera.orthographic || sceneObjects.empty() || width <= 0 || height <= 0) return;

    bool hasEnabledMirror = false;
    for (const auto& obj : sceneObjects) {
        if (IsObjectEnabledInHierarchy(obj) && obj.hasRenderer && obj.renderType == RenderType::Mirror) {
            hasEnabledMirror = true;
            break;
        }
    }
    if (!hasEnabledMirror) {
        if (!mirrorTargets.empty()) {
            for (auto& entry : mirrorTargets) {
                releaseRenderTarget(entry.second);
            }
            mirrorTargets.clear();
            mirrorUpdateStates.clear();
        }
        return;
    }

    std::unordered_set<int> active;
    active.reserve(mirrorTargets.size() + 4);
    const double nowSec = glfwGetTime();
    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    const glm::mat4 viewProjection =
        BuildCameraProjection(camera, width, height, fovDeg, nearPlane, farPlane) *
        camera.getViewMatrix();
    const FrustumPlanes frustum = BuildFrustumPlanes(viewProjection);

    auto planeNormal = [](const SceneObject& obj) {
        glm::quat q = glm::quat(glm::radians(obj.rotation));
        glm::vec3 n = q * glm::vec3(0.0f, 0.0f, 1.0f);
        if (!std::isfinite(n.x) || glm::length(n) < 1e-3f) {
            n = glm::vec3(0.0f, 0.0f, 1.0f);
        }
        return glm::normalize(n);
    };

    for (const auto& obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasRenderer || obj.renderType != RenderType::Mirror) continue;
        active.insert(obj.id);

        glm::vec3 n = planeNormal(obj);
        glm::vec3 planePoint = obj.position;
        const glm::vec3 toCamera = camera.position - planePoint;
        if (glm::dot(n, toCamera) <= 0.001f) {
            continue;
        }

        glm::mat4 mirrorModel = BuildSceneObjectModelMatrix(obj);
        glm::vec3 boundsCenter(0.0f);
        float boundsRadius = 0.0f;
        if (TryComputeObjectCullSphere(obj, mirrorModel, boundsCenter, boundsRadius) &&
            !SphereInsideFrustum(frustum, boundsCenter, boundsRadius)) {
            continue;
        }

        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
        if (!ProjectPlaneScreenRect(obj, viewProjection, width, height, minX, minY, maxX, maxY)) {
            continue;
        }

        const int targetWidth = QuantizeMirrorTargetDimension(static_cast<int>(std::ceil(maxX - minX)), width);
        const int targetHeight = QuantizeMirrorTargetDimension(static_cast<int>(std::ceil(maxY - minY)), height);

        RenderTarget& target = mirrorTargets[obj.id];
        MirrorUpdateState& state = mirrorUpdateStates[obj.id];
        const glm::vec3 cameraDelta = camera.position - state.lastCameraPos;
        const glm::vec3 mirrorPosDelta = obj.position - state.lastMirrorPos;
        const glm::vec3 mirrorRotDelta = obj.rotation - state.lastMirrorRot;
        const glm::vec3 mirrorScaleDelta = obj.scale - state.lastMirrorScale;
        const bool sizeChanged = target.width != targetWidth || target.height != targetHeight;
        const bool targetMissing = target.fbo == 0 || target.texture == 0;
        const bool cameraMoved = !std::isfinite(state.lastCameraPos.x) ||
            glm::dot(cameraDelta, cameraDelta) > (0.02f * 0.02f);
        const bool cameraRotated = !std::isfinite(state.lastCameraFront.x) ||
            glm::dot(camera.front, state.lastCameraFront) < 0.9995f ||
            glm::dot(camera.up, state.lastCameraUp) < 0.9995f;
        const bool mirrorMoved =
            glm::dot(mirrorPosDelta, mirrorPosDelta) > (0.001f * 0.001f) ||
            glm::dot(mirrorRotDelta, mirrorRotDelta) > (0.05f * 0.05f) ||
            glm::dot(mirrorScaleDelta, mirrorScaleDelta) > (0.001f * 0.001f);
        const bool transformChanged = cameraMoved || cameraRotated || mirrorMoved;
        const double secondsSinceUpdate = (state.lastUpdateTime >= 0.0) ? (nowSec - state.lastUpdateTime) : DBL_MAX;
        const bool shouldRefresh =
            targetMissing ||
            sizeChanged ||
            state.lastUpdateTime < 0.0 ||
            (transformChanged && secondsSinceUpdate >= (1.0 / 45.0)) ||
            secondsSinceUpdate >= 0.25;

        if (!shouldRefresh) {
            continue;
        }

        ensureRenderTarget(target, targetWidth, targetHeight, false, true);
        if (target.fbo == 0 || target.texture == 0) continue;

        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        glViewport(0, 0, target.width, target.height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        auto reflectPoint = [&](const glm::vec3& p) {
            float dist = glm::dot(p - planePoint, n);
            return p - 2.0f * dist * n;
        };
        auto reflectDir = [&](const glm::vec3& v) {
            float dist = glm::dot(v, n);
            return v - 2.0f * dist * n;
        };

        Camera mirrorCam = camera;
        mirrorCam.position = reflectPoint(camera.position);
        mirrorCam.front = glm::normalize(reflectDir(camera.front));
        mirrorCam.up = glm::normalize(reflectDir(camera.up));
        if (!std::isfinite(mirrorCam.front.x) || glm::length(mirrorCam.front) < 1e-3f) {
            mirrorCam.front = glm::vec3(0.0f, 0.0f, -1.0f);
        }
        if (!std::isfinite(mirrorCam.up.x) || glm::length(mirrorCam.up) < 1e-3f) {
            mirrorCam.up = glm::vec3(0.0f, 1.0f, 0.0f);
        }

        renderSceneInternal(mirrorCam, sceneObjects, target.width, target.height, false, fovDeg, nearPlane, farPlane, false, true);

        state.lastCameraPos = camera.position;
        state.lastCameraFront = camera.front;
        state.lastCameraUp = camera.up;
        state.lastMirrorPos = obj.position;
        state.lastMirrorRot = obj.rotation;
        state.lastMirrorScale = obj.scale;
        state.lastUpdateTime = nowSec;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);

    for (auto it = mirrorTargets.begin(); it != mirrorTargets.end(); ) {
        if (active.find(it->first) == active.end()) {
            releaseRenderTarget(it->second);
            mirrorUpdateStates.erase(it->first);
            it = mirrorTargets.erase(it);
        } else {
            ++it;
        }
    }
}

Renderer::UiTargetInfo Renderer::ensureUiTarget(int id,
                                                int width,
                                                int height,
                                                int layer,
                                                MaterialProperties::TextureFilter filter,
                                                bool allowAtlas) {
    if (!moduRenderContextLive()) {
        return {};
    }

    width = std::clamp(width, 16, 4096);
    height = std::clamp(height, 16, 4096);
    const GLint glFilter = ToGLTextureFilter(filter);

    auto setTargetFilter = [&](RenderTarget& target, bool force = false) {
        if (target.texture == 0) return;
        if (!force && target.textureFilter == filter) return;
        glBindTexture(GL_TEXTURE_2D, target.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, glFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        target.textureFilter = filter;
    };

    auto makeView = [&](RenderTarget& target, bool invalidated) {
        UiTargetInfo view;
        view.fbo = target.fbo;
        view.texture = target.texture;
        view.width = width;
        view.height = height;
        view.framebufferWidth = target.width;
        view.framebufferHeight = target.height;
        view.clearWidth = width;
        view.clearHeight = height;
        view.uvTransform = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
        view.invalidated = invalidated;
        return view;
    };

    auto ensureDedicated = [&](bool preserveInvalidated) {
        RenderTarget& target = uiTargets[id];
        const bool targetInvalidated =
            target.fbo == 0 || target.texture == 0 || target.width != width ||
            target.height != height || !target.hasAlpha || target.hasDepth || target.hdr;
        ensureRenderTarget(target, width, height, true, false, false);
        setTargetFilter(target, targetInvalidated);

        UiTargetInfo view = makeView(target, targetInvalidated);
        view.framebufferWidth = width;
        view.framebufferHeight = height;
        view.clearX = 0;
        view.clearY = 0;
        view.usesAtlas = false;
        auto prev = uiTargetViews.find(id);
        if (prev != uiTargetViews.end()) {
            view.invalidated = view.invalidated || preserveInvalidated ||
                               prev->second.invalidated ||
                               prev->second.texture != view.texture ||
                               prev->second.usesAtlas;
        } else {
            view.invalidated = true;
        }
        uiTargetViews[id] = view;
        return view;
    };

    const int clearWidth = width + kUiAtlasPadding * 2;
    const int clearHeight = height + kUiAtlasPadding * 2;
    if (!allowAtlas || clearWidth > uiAtlasMaxSize || clearHeight > uiAtlasMaxSize) {
        return ensureDedicated(false);
    }

    const uint64_t atlasKey = BuildUiAtlasKey(layer, filter);
    UiAtlasTarget& atlas = uiTargetAtlases[atlasKey];
    bool atlasInvalidated = false;

    auto allocateSlot = [&](UiAtlasSlot& slot) -> bool {
        if (atlas.size <= 0) return false;
        if (slot.clearWidth > atlas.size || slot.clearHeight > atlas.size) return false;
        if (atlas.cursorX + slot.clearWidth > atlas.size) {
            atlas.cursorX = 0;
            atlas.cursorY += atlas.rowHeight;
            atlas.rowHeight = 0;
        }
        if (atlas.cursorY + slot.clearHeight > atlas.size) return false;
        slot.clearX = atlas.cursorX;
        slot.clearY = atlas.cursorY;
        slot.x = slot.clearX + kUiAtlasPadding;
        slot.y = slot.clearY + kUiAtlasPadding;
        atlas.cursorX += slot.clearWidth;
        atlas.rowHeight = std::max(atlas.rowHeight, slot.clearHeight);
        return true;
    };

    auto repackAtlas = [&](int requestedSize) -> bool {
        std::vector<std::pair<int, UiAtlasSlot>> slots;
        slots.reserve(atlas.slots.size());
        for (const auto& entry : atlas.slots) {
            slots.push_back(entry);
        }
        std::sort(slots.begin(), slots.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second.clearHeight != b.second.clearHeight) {
                          return a.second.clearHeight > b.second.clearHeight;
                      }
                      return a.first < b.first;
                  });

        int size = std::max(1024, NextPowerOfTwoAtLeast(requestedSize));
        size = std::max(size, atlas.size);
        while (size <= uiAtlasMaxSize) {
            UiAtlasTarget packed;
            packed.size = size;
            bool fits = true;
            for (auto& entry : slots) {
                UiAtlasSlot slot = entry.second;
                if (slot.clearWidth > packed.size || slot.clearHeight > packed.size) {
                    fits = false;
                    break;
                }
                if (packed.cursorX + slot.clearWidth > packed.size) {
                    packed.cursorX = 0;
                    packed.cursorY += packed.rowHeight;
                    packed.rowHeight = 0;
                }
                if (packed.cursorY + slot.clearHeight > packed.size) {
                    fits = false;
                    break;
                }
                slot.clearX = packed.cursorX;
                slot.clearY = packed.cursorY;
                slot.x = slot.clearX + kUiAtlasPadding;
                slot.y = slot.clearY + kUiAtlasPadding;
                packed.cursorX += slot.clearWidth;
                packed.rowHeight = std::max(packed.rowHeight, slot.clearHeight);
                packed.slots[entry.first] = slot;
            }
            if (fits) {
                atlas.size = packed.size;
                atlas.cursorX = packed.cursorX;
                atlas.cursorY = packed.cursorY;
                atlas.rowHeight = packed.rowHeight;
                atlas.slots = std::move(packed.slots);
                atlasInvalidated = true;
                return true;
            }
            if (size >= uiAtlasMaxSize) break;
            size = std::min(uiAtlasMaxSize, size * 2);
        }
        return false;
    };

    bool slotInvalidated = false;
    auto slotIt = atlas.slots.find(id);
    if (slotIt == atlas.slots.end()) {
        UiAtlasSlot slot;
        slot.contentWidth = width;
        slot.contentHeight = height;
        slot.clearWidth = clearWidth;
        slot.clearHeight = clearHeight;

        if (atlas.size <= 0) {
            atlas.size = std::min(uiAtlasMaxSize, std::max(1024, NextPowerOfTwoAtLeast(std::max(clearWidth, clearHeight))));
        }
        if (!allocateSlot(slot)) {
            auto oldSlots = atlas.slots;
            atlas.slots[id] = slot;
            if (!repackAtlas(std::max(atlas.size * 2, std::max(clearWidth, clearHeight)))) {
                atlas.slots = std::move(oldSlots);
                return ensureDedicated(false);
            }
        } else {
            atlas.slots[id] = slot;
        }
        slotInvalidated = true;
    } else if (slotIt->second.contentWidth != width ||
               slotIt->second.contentHeight != height) {
        auto oldSlots = atlas.slots;
        slotIt->second.contentWidth = width;
        slotIt->second.contentHeight = height;
        slotIt->second.clearWidth = clearWidth;
        slotIt->second.clearHeight = clearHeight;
        if (!repackAtlas(std::max(atlas.size, std::max(clearWidth, clearHeight)))) {
            atlas.slots = std::move(oldSlots);
            atlas.slots.erase(id);
            repackAtlas(std::max(1024, atlas.size));
            return ensureDedicated(false);
        }
        slotInvalidated = true;
    }

    const bool targetInvalidated =
        atlas.target.fbo == 0 || atlas.target.texture == 0 ||
        atlas.target.width != atlas.size || atlas.target.height != atlas.size ||
        !atlas.target.hasAlpha || atlas.target.hasDepth || atlas.target.hdr;
    ensureRenderTarget(atlas.target, atlas.size, atlas.size, true, false, false);
    setTargetFilter(atlas.target, targetInvalidated);
    atlasInvalidated = atlasInvalidated || targetInvalidated;

    if (atlasInvalidated) {
        for (const auto& entry : atlas.slots) {
            auto viewIt = uiTargetViews.find(entry.first);
            if (viewIt != uiTargetViews.end()) {
                viewIt->second.invalidated = true;
            }
        }
    }

    const UiAtlasSlot& slot = atlas.slots[id];
    UiTargetInfo view;
    view.fbo = atlas.target.fbo;
    view.texture = atlas.target.texture;
    view.width = width;
    view.height = height;
    view.framebufferWidth = atlas.target.width;
    view.framebufferHeight = atlas.target.height;
    view.x = slot.x;
    view.y = slot.y;
    view.clearX = slot.clearX;
    view.clearY = slot.clearY;
    view.clearWidth = slot.clearWidth;
    view.clearHeight = slot.clearHeight;
    view.uvTransform = BuildUiTargetUvTransform(slot.x, slot.y, width, height,
                                                atlas.target.width, atlas.target.height);
    view.usesAtlas = true;
    view.atlasKey = atlasKey;
    auto prev = uiTargetViews.find(id);
    view.invalidated = slotInvalidated || atlasInvalidated;
    if (prev != uiTargetViews.end()) {
        view.invalidated = view.invalidated || prev->second.invalidated ||
                           prev->second.texture != view.texture ||
                           !prev->second.usesAtlas ||
                           prev->second.atlasKey != atlasKey ||
                           prev->second.x != view.x ||
                           prev->second.y != view.y;
    } else {
        view.invalidated = true;
    }
    uiTargetViews[id] = view;
    return view;
}

Renderer::UiTargetInfo Renderer::getUiTargetInfo(int id) const {
    auto it = uiTargetViews.find(id);
    return (it != uiTargetViews.end()) ? it->second : UiTargetInfo{};
}

void Renderer::markUiTargetRendered(int id) {
    auto it = uiTargetViews.find(id);
    if (it != uiTargetViews.end()) {
        it->second.invalidated = false;
    }
}

void Renderer::cleanupUiTargets(const std::unordered_set<int>& active) {
    if (!moduRenderContextLive()) {
        uiTargets.clear();
        uiTargetViews.clear();
        uiTargetAtlases.clear();
        return;
    }

    for (auto it = uiTargetViews.begin(); it != uiTargetViews.end();) {
        if (active.find(it->first) == active.end()) {
            it = uiTargetViews.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = uiTargets.begin(); it != uiTargets.end();) {
        auto viewIt = uiTargetViews.find(it->first);
        if (active.find(it->first) == active.end() ||
            viewIt == uiTargetViews.end() ||
            viewIt->second.usesAtlas) {
            releaseRenderTarget(it->second);
            it = uiTargets.erase(it);
        } else {
            ++it;
        }
    }

    for (auto atlasIt = uiTargetAtlases.begin(); atlasIt != uiTargetAtlases.end();) {
        UiAtlasTarget& atlas = atlasIt->second;
        for (auto slotIt = atlas.slots.begin(); slotIt != atlas.slots.end();) {
            auto viewIt = uiTargetViews.find(slotIt->first);
            if (active.find(slotIt->first) == active.end() ||
                viewIt == uiTargetViews.end() ||
                !viewIt->second.usesAtlas ||
                viewIt->second.atlasKey != atlasIt->first) {
                slotIt = atlas.slots.erase(slotIt);
            } else {
                ++slotIt;
            }
        }
        if (atlas.slots.empty()) {
            releaseRenderTarget(atlas.target);
            atlasIt = uiTargetAtlases.erase(atlasIt);
        } else {
            ++atlasIt;
        }
    }
}

bool Renderer::isObjectVisibleInCapturedFrustums(const SceneObject& obj, float radiusMargin) const {
    if (capturedSceneFrustums.empty()) {
        return true;
    }
    glm::vec3 center(0.0f);
    float radius = 0.0f;
    if (!TryComputeObjectCullSphere(obj, BuildSceneObjectModelMatrix(obj), center, radius)) {
        return true;
    }
    radius *= std::max(1.0f, radiusMargin);
    for (const auto& planes : capturedSceneFrustums) {
        FrustumPlanes frustum{planes};
        if (SphereInsideFrustum(frustum, center, radius)) {
            return true;
        }
    }
    return false;
}

bool Renderer::measureObjectScreenSizePx(const SceneObject& obj, glm::vec2& outSizePx,
                                         bool& outUnbounded) const {
    outSizePx = glm::vec2(0.0f);
    outUnbounded = false;
    if (capturedSceneViews.empty()) {
        return false;
    }
    // Sprite quads span -0.5..0.5 in local X/Y, so the model axes are the
    // world-space edge lengths.
    const glm::mat4 model = BuildSceneObjectModelMatrix(obj);
    const glm::vec3 center(model[3]);
    const float worldWidth = glm::length(glm::vec3(model[0]));
    const float worldHeight = glm::length(glm::vec3(model[1]));
    glm::vec3 cullCenter(0.0f);
    float cullRadius = 0.0f;
    const bool hasCullSphere = TryComputeObjectCullSphere(obj, model, cullCenter, cullRadius);
    bool seen = false;
    for (size_t i = 0; i < capturedSceneViews.size(); ++i) {
        const CapturedSceneView& captured = capturedSceneViews[i];
        if (captured.width <= 0 || captured.height <= 0) {
            continue;
        }
        if (hasCullSphere && i < capturedSceneFrustums.size() &&
            !SphereInsideFrustum(FrustumPlanes{capturedSceneFrustums[i]}, cullCenter, cullRadius)) {
            continue;
        }
        seen = true;
        const float pxPerUnitAtUnitDepth =
            0.5f * std::max(std::abs(captured.proj[0][0]) * static_cast<float>(captured.width),
                            std::abs(captured.proj[1][1]) * static_cast<float>(captured.height));
        float pxPerUnit = pxPerUnitAtUnitDepth;
        const bool perspective = std::abs(captured.proj[3][3]) < 0.5f;
        if (perspective) {
            const float depth = -(captured.view * glm::vec4(center, 1.0f)).z;
            if (!(depth > 1e-3f)) {
                // Centre at or behind the eye while the quad is still in view.
                outUnbounded = true;
                return true;
            }
            pxPerUnit /= depth;
        }
        if (!std::isfinite(pxPerUnit)) {
            outUnbounded = true;
            return true;
        }
        outSizePx.x = std::max(outSizePx.x, worldWidth * pxPerUnit);
        outSizePx.y = std::max(outSizePx.y, worldHeight * pxPerUnit);
    }
    return seen;
}

void Renderer::ensureQuad() {
    if (quadVAO != 0) return;

    float quadVertices[] = {
        // positions   // texcoords
        -1.0f,  1.0f, 0.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,

        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f
    };

    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);
}

void Renderer::drawFullscreenQuad() {
    recordFullscreenDraw();
    if (quadVAO == 0) ensureQuad();
    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void Renderer::resetStats(RenderStats& stats) {
    stats = RenderStats{};
}

void Renderer::recordDrawCall() {
    if (!activeStats) return;
    activeStats->drawCalls += 1;
}

void Renderer::recordMeshDraw() {
    if (!activeStats) return;
    activeStats->drawCalls += 1;
    activeStats->meshDraws += 1;
}

void Renderer::recordFullscreenDraw() {
    if (!activeStats) return;
    activeStats->drawCalls += 1;
    activeStats->fullscreenDraws += 1;
}

void Renderer::clearHistory() {
    historyValid = false;
    if (historyTarget.fbo != 0 && historyTarget.width > 0 && historyTarget.height > 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, historyTarget.fbo);
        glViewport(0, 0, historyTarget.width, historyTarget.height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void Renderer::clearTarget(RenderTarget& target) {
    if (target.fbo == 0 || target.width <= 0 || target.height <= 0) return;
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
    glViewport(0, 0, target.width, target.height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

unsigned int Renderer::findFramebufferForTexture(unsigned int texture) const {
    if (texture == 0) return 0;
    if (texture == viewportTexture) return framebuffer;

    auto findTargetMatch = [texture](const RenderTarget& target) -> unsigned int {
        return (target.texture == texture) ? target.fbo : 0u;
    };

    if (unsigned int match = findTargetMatch(previewTarget)) return match;
    if (unsigned int match = findTargetMatch(externalSceneTarget)) return match;
    if (unsigned int match = findTargetMatch(postTarget)) return match;
    if (unsigned int match = findTargetMatch(previewPostTarget)) return match;
    if (unsigned int match = findTargetMatch(externalPostTarget)) return match;
    if (unsigned int match = findTargetMatch(historyTarget)) return match;
    if (unsigned int match = findTargetMatch(bloomTargetA)) return match;
    if (unsigned int match = findTargetMatch(bloomTargetB)) return match;
    if (unsigned int match = findTargetMatch(selectionMaskTarget)) return match;

    for (const auto& [id, target] : extraPreviewTargets) {
        (void)id;
        if (unsigned int match = findTargetMatch(target)) return match;
    }
    for (const auto& [id, target] : mirrorTargets) {
        (void)id;
        if (unsigned int match = findTargetMatch(target)) return match;
    }
    for (const auto& [id, target] : uiTargets) {
        (void)id;
        if (unsigned int match = findTargetMatch(target)) return match;
    }
    for (const auto& [key, atlas] : uiTargetAtlases) {
        (void)key;
        if (unsigned int match = findTargetMatch(atlas.target)) return match;
    }

    return 0;
}

void Renderer::logPostFxDebug(const PostProcessStats& stats, bool allowHistory) const {
    (void)stats;
    (void)allowHistory;
}

void Renderer::resize(int w, int h) {
    {
        ModuRenderer::ResizeRequest request;
        request.width = w;
        request.height = h;
        request.currentWidth = currentWidth;
        request.currentHeight = currentHeight;
        request.initialized = (framebuffer != 0 && viewportTexture != 0 && sceneDepthTexture != 0);
        request.resizeInProgress = resizeInProgress;
        request.shuttingDown = rendererShuttingDown;

        const ModuRenderer::ResizeDecision decision =
            ModuRenderer::EvaluateResizeRequest(request);
        if (decision != ModuRenderer::ResizeDecision::Accept) {
            if (decision == ModuRenderer::ResizeDecision::RejectNotInitialized &&
                framebuffer != 0) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    std::cerr << "Renderer::resize rejected as not-initialized while the "
                                 "framebuffer exists - the viewport will not follow its "
                                 "panel size.\n";
                }
            }
            return;
        }
    }

#ifndef __ANDROID__
    if (glfwGetCurrentContext() == nullptr) {
        return;
    }
#endif

    {
        GLint maxTexture = 0;
        GLint maxRenderbuffer = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
        glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRenderbuffer);
        const GLint limit = std::min(maxTexture > 0 ? maxTexture : 16384,
                                     maxRenderbuffer > 0 ? maxRenderbuffer : 16384);
        ModuRenderer::ClampResizeToLimit(w, h, static_cast<int>(limit));
        if (w == currentWidth && h == currentHeight) return;
    }

    struct ResizeGuard {
        bool& flag;
        explicit ResizeGuard(bool& f) : flag(f) { flag = true; }
        ~ResizeGuard() { flag = false; }
    } resizeGuard(resizeInProgress);

    currentWidth = w;
    currentHeight = h;

    GLint previousFramebuffer = 0;
    GLint previousTexture = 0;
    GLint previousRenderbuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    glBindTexture(GL_TEXTURE_2D, viewportTexture);
    {
        const bool sdr = (colorPrecision == RendererColorPrecision::SDR8);
        glTexImage2D(GL_TEXTURE_2D, 0, sdr ? GL_RGBA8 : GL_RGBA16F, currentWidth, currentHeight,
                     0, GL_RGBA, sdr ? GL_UNSIGNED_BYTE : GL_FLOAT, NULL);
    }
    glBindTexture(GL_TEXTURE_2D, sceneDepthTexture);
    allocateDepthStencilTexture(currentWidth, currentHeight);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Framebuffer incomplete after resize!\n";
    }

    ensureRenderTarget(postTarget, currentWidth, currentHeight, false, true);
    ensureRenderTarget(historyTarget, currentWidth, currentHeight, false, true);
    ensureRenderTarget(bloomTargetA, currentWidth, currentHeight, false, true);
    ensureRenderTarget(bloomTargetB, currentWidth, currentHeight, false, true);
    ensureRenderTarget(selectionMaskTarget, currentWidth, currentHeight);
    clearHistory();
    displayTexture = viewportTexture;

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previousRenderbuffer));
}

void Renderer::beginRender(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPos) {
    (void)view;
    (void)proj;
    (void)cameraPos;
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    Runtime2DCountStateBind();
    glViewport(0, 0, currentWidth, currentHeight);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    displayTexture = viewportTexture;
}

void Renderer::renderSkybox(const glm::mat4& view, const glm::mat4& proj) {
    if (skybox) {
        const glm::mat4 invView = glm::inverse(view);
        skybox->setCameraPosition(glm::vec3(invView[3]), frameSerial);
        glDepthFunc(GL_LEQUAL);
        skybox->draw(glm::value_ptr(view), glm::value_ptr(proj));
        glDepthFunc(GL_LESS);

        shader->use();
        shader->setMat4("view", view);
        shader->setMat4("projection", proj);
    }
}

void Renderer::renderObject(const SceneObject& obj) {
    glm::mat4 model = BuildSceneObjectModelMatrix(obj);

    const bool mirroredTransform = glm::determinant(glm::mat3(model)) < 0.0f;
    GLint restoreFrontFace = GL_CCW;
    if (mirroredTransform) {
        glGetIntegerv(GL_FRONT_FACE, &restoreFrontFace);
        glFrontFace(restoreFrontFace == GL_CCW ? GL_CW : GL_CCW);
    }
    struct FrontFaceRestore {
        bool active;
        GLint value;
        ~FrontFaceRestore() { if (active) glFrontFace(value); }
    } frontFaceRestore{mirroredTransform, restoreFrontFace};

    const bool hasRuntimeAlbedoOverride =
        obj.runtimeHasAlbedoTextureOverride && obj.runtimeAlbedoTextureOverrideId != 0;
    bool hasMaterialAsset = !obj.materialPath.empty();
    bool hasCustomShader = !obj.vertexShaderPath.empty() || !obj.fragmentShaderPath.empty();
    bool hasAnySurfaceInput = hasRuntimeAlbedoOverride ||
                              !obj.albedoTexturePath.empty() ||
                              !obj.overlayTexturePath.empty() ||
                              !obj.normalMapPath.empty();
    bool missingMaterialAndShader = !hasMaterialAsset && !hasCustomShader && !hasAnySurfaceInput;

    shader->setMat4("model", model);
    shader->setVec3("materialColor", missingMaterialAndShader ? glm::vec3(1.0f) : obj.material.color);
    shader->setFloat("materialAlpha", missingMaterialAndShader ? 1.0f : obj.material.alpha);
    shader->setFloat("ambientStrength", obj.material.ambientStrength);
    shader->setFloat("specularStrength", obj.material.specularStrength);
    shader->setBool("specularEnabled", specularEnabled);
    shader->setFloat("shininess", obj.material.shininess);
    shader->setFloat("normalMapIntensity", obj.material.normalMapIntensity);
    shader->setFloat("mixAmount", obj.material.textureMix);
    shader->setFloat("uScrollSpeed", obj.material.scrollSpeed);
    shader->setVec2("uScrollDir", obj.material.scrollDirection);
    shader->setVec3("uCloudColor", obj.material.cloudColor);
    shader->setVec3("uCloudSkyColor", obj.material.cloudSkyColor);
    shader->setFloat("uCloudScale", obj.material.cloudScale);
    shader->setFloat("uCloudCoverage", obj.material.cloudCoverage);
    shader->setFloat("uCloudSoftness", obj.material.cloudSoftness);
    shader->setInt("uCloudDetail", obj.material.cloudDetail);
    shader->setFloat("uCloudSpeed", obj.material.cloudSpeed);
    shader->setFloat("uCloudWarp", obj.material.cloudWarp);
    shader->setFloat("uCloudHighlight", obj.material.cloudHighlight);
    shader->setFloat("uCloudStars", obj.material.cloudStars);
    shader->setFloat("uCloudHorizon", obj.material.cloudHorizon);
    shader->setVec4("uvTransform", BuildSurfaceUvTransform(&obj, obj.material));
    shader->setVec4("uvRect", BuildSpriteUvRect(obj));
    shader->setBool("unlit", obj.renderType == RenderType::Mirror || obj.renderType == RenderType::Sprite || missingMaterialAndShader);

    Texture* baseTex = texture1;
    if (missingMaterialAndShader && missingMaterialFallbackTexture != 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, missingMaterialFallbackTexture);
    } else if (hasRuntimeAlbedoOverride) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, obj.runtimeAlbedoTextureOverrideId);
    } else {
        if (!obj.albedoTexturePath.empty()) {
            if (auto* t = getTexture(obj.albedoTexturePath, obj.material.textureFilter)) baseTex = t;
        }
        if (baseTex) baseTex->Bind(GL_TEXTURE0);
    }

    bool overlayUsed = false;
    if (obj.renderType == RenderType::Mirror) {
        auto it = mirrorTargets.find(obj.id);
        if (it != mirrorTargets.end() && it->second.texture != 0) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, it->second.texture);
            overlayUsed = true;
        }
    }
    if (!overlayUsed && obj.useOverlay && !obj.overlayTexturePath.empty()) {
        if (auto* t = getTexture(obj.overlayTexturePath, obj.material.textureFilter)) {
            t->Bind(GL_TEXTURE1);
            overlayUsed = true;
        }
    }
    if (!overlayUsed && texture2) {
        texture2->Bind(GL_TEXTURE1);
    }
    shader->setBool("hasOverlay", overlayUsed);

    bool normalUsed = false;
    if (!obj.normalMapPath.empty()) {
        if (auto* t = getTexture(obj.normalMapPath, obj.material.textureFilter, true)) {
            t->Bind(GL_TEXTURE2);
            normalUsed = true;
        }
    }
    shader->setBool("hasNormalMap", normalUsed);
    if (skybox) {
        const SkyboxSettings& skySettings = skybox->getSettings();
        shader->setFloat("reflectionFadeStart", skySettings.reflectionDistanceFadeStart);
        shader->setFloat("reflectionFadeEnd", skySettings.reflectionDistanceFadeEnd);
        shader->setBool("fogEnabled", skySettings.fogEnabled);
        shader->setInt("fogMode", skySettings.fogMode);
        shader->setVec3("fogColor", skySettings.fogColor);
        shader->setFloat("fogStart", skySettings.fogStart);
        shader->setFloat("fogEnd", skySettings.fogEnd);
        shader->setFloat("fogDensity", skySettings.fogDensity);
        shader->setFloat("fogHeight", skySettings.fogHeight);
        shader->setFloat("fogHeightFalloff", skySettings.fogHeightFalloff);
    } else {
        shader->setBool("fogEnabled", false);
    }

    switch (obj.renderType) {
        case RenderType::Cube:
            cubeMesh->draw();
            break;
        case RenderType::Sphere:
            sphereMesh->draw();
            break;
        case RenderType::Capsule:
            capsuleMesh->draw();
            break;
        case RenderType::Plane:
            if (planeMesh) planeMesh->draw();
            break;
        case RenderType::Mirror:
            if (planeMesh) planeMesh->draw();
            break;
        case RenderType::Sprite:
            if (planeMesh) planeMesh->draw();
            break;
        case RenderType::Torus:
            if (torusMesh) torusMesh->draw();
            break;
        case RenderType::OBJMesh:
            if (obj.meshId >= 0) {
                Mesh* objMesh = g_objLoader.getMesh(obj.meshId);
                if (objMesh) {
                    objMesh->draw();
                }
            }
            break;
        case RenderType::Model:
            if (obj.meshId >= 0) {
                Mesh* modelMesh = getModelLoader().getMesh(obj.meshId);
                if (modelMesh) {
                    modelMesh->draw();
                }
            }
            break;
        case RenderType::None:
        default:
            break;
    }
}

namespace {
bool MainDirectionalSunDirection(const std::vector<SceneObject>& sceneObjects,
                                 glm::vec3& outDirection) {
    for (const auto& obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj)) continue;
        if (!obj.hasLight || !obj.light.enabled) continue;
        if (obj.light.type != LightType::Directional) continue;

        glm::mat4 rotation(1.0f);
        rotation = glm::rotate(rotation, glm::radians(obj.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        rotation = glm::rotate(rotation, glm::radians(obj.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        rotation = glm::rotate(rotation, glm::radians(obj.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        const glm::vec3 forward = glm::vec3(rotation * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
        if (glm::dot(forward, forward) < 1e-6f) continue;
        if (!std::isfinite(forward.x) || !std::isfinite(forward.y) || !std::isfinite(forward.z)) continue;

        outDirection = -glm::normalize(forward);
        return true;
    }
    return false;
}
} // namespace

// =====================================================================================
// Optimized render path, scene-aware half (Project Settings > Graphics Manager >
// Optimization). The GL half is RenderOptimization.cpp and the shared state is in
// RenderOptimizationInternal.h. None of this runs unless an optimization is enabled.
// =====================================================================================

namespace {

const std::string kEmptyRenderPath;
const std::string kMultiDrawVertPath = "Resources/Shaders/vert_mdi.glsl";
const std::string kShadowDirectionalMultiDrawVertPath =
    "Resources/Shaders/shadow_directional_depth_mdi_vert.glsl";
const std::string kShadowCubeMultiDrawVertPath = "Resources/Shaders/shadow_depth_mdi_vert.glsl";

uint64_t ShaderPairHash(const std::string& vert, const std::string& frag) {
    return HashCombine64(HashString64(vert), HashString64(frag));
}

uint64_t TextureSetHash(const std::string* const* paths, size_t count,
                        MaterialProperties::TextureFilter filter, bool useOverlay) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < count; ++i) {
        hash = HashCombine64(hash, HashString64(*paths[i]));
    }
    hash = HashCombine64(hash, static_cast<uint64_t>(filter));
    return HashCombine64(hash, useOverlay ? 2u : 1u);
}

uint64_t HashCachedMaterial(const RenderOpt::CachedRenderable& entry) {
    HashKeyVisitor visitor{1469598103934665603ull};
    for (const std::string& path : entry.paths) {
        visitor.str(path);
    }
    visitor.flag(entry.useOverlay);
    VisitMaterialPropertiesKey(entry.material, visitor);
    // A per-draw uniform, so batches must split on it. (The static merge key predates
    // it and does not include it.)
    visitor.num(entry.material.alphaCutoff);
    return visitor.hash;
}

uint32_t InternGroup(std::unordered_map<uint64_t, uint32_t>& table, uint64_t key) {
    auto found = table.find(key);
    if (found != table.end()) {
        return found->second;
    }
    const uint32_t id = static_cast<uint32_t>(table.size() + 1);
    table.emplace(key, id);
    return id;
}

// Opaque draw order: program first (a switch re-sends every light uniform), then the
// texture set, then material values, then cull/front-face state, and last a coarse
// front-to-back bucket so draws that share all of that still get early depth rejection.
// The bucket is deliberately coarse: within one the sort is stable, so objects keep scene
// order, and the draw loop reads each SceneObject (kilobytes apiece) close to sequentially.
// A fine depth key scattered those reads across the whole scene array; in a 4900-object
// test the cache misses cost more than the state changes the sort saved.
uint64_t OpaqueSortKey(uint32_t shaderGroup, uint32_t textureGroup, uint64_t materialKey,
                       uint32_t stateBits, float depth, float farPlane) {
    float normalized = depth / std::max(farPlane, 1e-3f);
    if (!(normalized > 0.0f)) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;
    const uint64_t depthBits = static_cast<uint64_t>(normalized * 7.0f);
    return (static_cast<uint64_t>(shaderGroup & 0xFFFu) << 52) |
           (static_cast<uint64_t>(textureGroup & 0xFFFFFu) << 32) |
           ((materialKey & 0xFFFull) << 20) |
           (static_cast<uint64_t>(stateBits & 0xFu) << 16) |
           depthBits;
}

float FiniteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

bool IsUiCanvas3DObject(const SceneObject& obj) {
    return obj.hasUI && obj.ui.type == UIElementType::Canvas && obj.ui.renderIn3D;
}

float MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

Mesh* Renderer::OptimizationState::meshFor(Renderer& renderer, const SceneObject& obj) {
    switch (obj.renderType) {
        case RenderType::Cube: return renderer.cubeMesh;
        case RenderType::Sphere: return renderer.sphereMesh;
        case RenderType::Capsule: return renderer.capsuleMesh;
        case RenderType::Plane:
        case RenderType::Mirror:
        case RenderType::Sprite: return renderer.planeMesh;
        case RenderType::Torus: return renderer.torusMesh;
        case RenderType::OBJMesh:
            return obj.meshId != -1 ? g_objLoader.getMesh(obj.meshId) : nullptr;
        case RenderType::Model:
            return obj.meshId != -1 ? getModelLoader().getMesh(obj.meshId) : nullptr;
        case RenderType::None:
            break;
    }
    return nullptr;
}

void Renderer::OptimizationState::refreshTransforms(const std::vector<SceneObject>& sceneObjects,
                                                    const Camera& camera, bool forceRecompute) {
    if (cache.size() != sceneObjects.size()) {
        cache.resize(sceneObjects.size());
        bounds.resize(sceneObjects.size());
    }
    for (size_t i = 0; i < sceneObjects.size(); ++i) {
        const SceneObject& obj = sceneObjects[i];
        if (!IsObjectEnabledInHierarchy(obj) || !HasRendererComponent(obj)) {
            continue;
        }
        RenderOpt::CachedRenderable& entry = cache[i];
        if (entry.objectId != obj.id) {
            // A different object now sits at this index (insertions, deletions, a new
            // scene): nothing cached here describes it.
            entry.objectId = obj.id;
            entry.transformValid = false;
            entry.materialValid = false;
            entry.materialCheckedFrame = UINT64_MAX;
            entry.alphaTextureGeneration = UINT64_MAX;
            entry.staticMergeFlagGeneration = UINT64_MAX;
        }
        glm::vec3 localCenter(0.0f);
        glm::vec3 localExtents(0.0f);
        const bool hasLocalBounds = TryGetObjectLocalBounds(obj, localCenter, localExtents);
        // A camera-facing sprite's matrix depends on the camera, so it never stays valid.
        const bool cameraDependent = obj.renderType == RenderType::Sprite && obj.faceCamera;
        if (!forceRecompute && !cameraDependent && entry.transformValid &&
            entry.position == obj.position && entry.rotation == obj.rotation &&
            entry.scale == obj.scale && entry.renderType == obj.renderType &&
            entry.meshId == obj.meshId && entry.faceCamera == obj.faceCamera &&
            entry.hasLocalBounds == hasLocalBounds &&
            (!hasLocalBounds || (entry.localCenter == localCenter &&
                                 entry.localExtents == localExtents))) {
            continue;
        }
        entry.position = obj.position;
        entry.rotation = obj.rotation;
        entry.scale = obj.scale;
        entry.renderType = obj.renderType;
        entry.meshId = obj.meshId;
        entry.faceCamera = obj.faceCamera;
        entry.hasLocalBounds = hasLocalBounds;
        entry.localCenter = localCenter;
        entry.localExtents = localExtents;
        entry.model = BuildSceneObjectModelMatrix(obj, &camera.position, &camera.up);
        entry.mirrored = glm::determinant(glm::mat3(entry.model)) < 0.0f;
        entry.transformVersion = ++versionCounter;
        if (entry.transformVersion == 0) {
            entry.transformVersion = ++versionCounter;   // 0 means "never uploaded"
        }
        entry.transformValid = true;

        RenderOpt::CullBounds& b = bounds[i];
        b.hasBounds = 0;
        if (hasLocalBounds) {
            RenderOpt::ComputeWorldBounds(entry.model, localCenter, localExtents, b);
        }
    }
}

void Renderer::OptimizationState::refreshMaterials(const std::vector<SceneObject>& sceneObjects,
                                                   uint64_t frameSerial, bool forceRecompute) {
    for (size_t i = 0; i < sceneObjects.size() && i < cache.size(); ++i) {
        const SceneObject& obj = sceneObjects[i];
        if (!IsObjectEnabledInHierarchy(obj) || !HasRendererComponent(obj)) {
            continue;
        }
        RenderOpt::CachedRenderable& entry = cache[i];
        if (entry.objectId != obj.id) {
            continue;
        }
        if (!forceRecompute && entry.materialCheckedFrame == frameSerial) {
            continue;
        }
        entry.materialCheckedFrame = frameSerial;
        const std::string* current[RenderOpt::kPathCount] = {
            &obj.vertexShaderPath, &obj.fragmentShaderPath, &obj.materialPath,
            &obj.albedoTexturePath, &obj.overlayTexturePath, &obj.normalMapPath,
            &obj.metallicRoughnessMapPath, &obj.metallicMapPath, &obj.roughnessMapPath,
            &obj.opacityMapPath, &obj.emissiveMapPath, &obj.occlusionMapPath, &obj.heightMapPath};
        // Byte comparison can report a change that is not one (padding, -0.0), which only
        // costs a recompute; it can never miss a real change.
        bool unchanged = !forceRecompute && entry.materialValid &&
                         entry.useOverlay == obj.useOverlay &&
                         std::memcmp(&entry.material, &obj.material, sizeof(MaterialProperties)) == 0;
        for (int p = 0; unchanged && p < RenderOpt::kPathCount; ++p) {
            unchanged = entry.paths[static_cast<size_t>(p)] == *current[p];
        }
        if (unchanged) {
            continue;
        }
        entry.material = obj.material;
        entry.useOverlay = obj.useOverlay;
        for (int p = 0; p < RenderOpt::kPathCount; ++p) {
            entry.paths[static_cast<size_t>(p)] = *current[p];
        }
        entry.materialKey = HashCachedMaterial(entry);
        entry.shaderGroup = InternGroup(shaderGroups,
                                        ShaderPairHash(entry.paths[RenderOpt::kPathVert],
                                                       entry.paths[RenderOpt::kPathFrag]));
        entry.textureGroup = InternGroup(textureGroups,
                                         TextureSetHash(&current[RenderOpt::kPathAlbedo], 10,
                                                        obj.material.textureFilter, obj.useOverlay));
        entry.materialVersion = ++versionCounter;
        entry.materialValid = true;
    }
}

bool Renderer::OptimizationState::resolveAlphaBlend(Renderer& renderer, uint32_t objectIndex,
                                                    const SceneObject& obj) {
    RenderOpt::CachedRenderable& entry = cache[objectIndex];
    if (entry.alphaMaterialVersion == entry.materialVersion &&
        entry.alphaTextureGeneration == renderer.textureCacheGeneration) {
        return entry.albedoAlphaBlend;
    }
    entry.albedoAlphaBlend = false;
    entry.overlayAlphaBlend = false;
    if (!obj.albedoTexturePath.empty()) {
        if (Texture* tex = renderer.getTexture(obj.albedoTexturePath, obj.material.textureFilter)) {
            entry.albedoAlphaBlend = tex->UsesAlphaBlending();
        }
    }
    if (obj.useOverlay && !obj.overlayTexturePath.empty()) {
        if (Texture* tex = renderer.getTexture(obj.overlayTexturePath, obj.material.textureFilter)) {
            entry.overlayAlphaBlend = tex->UsesAlphaBlending();
        }
    }
    entry.alphaMaterialVersion = entry.materialVersion;
    // Read after the lookups: a lookup that loaded the texture bumped the generation.
    entry.alphaTextureGeneration = renderer.textureCacheGeneration;
    return entry.albedoAlphaBlend;
}

void Renderer::OptimizationState::refreshStaticMerge(Renderer& renderer,
                                                     const std::vector<SceneObject>& sceneObjects,
                                                     bool useCachedSignature) {
    if (!useCachedSignature) {
        renderer.rebuildStaticMergeBatches(sceneObjects);
        return;
    }
    if (staticMergeCheckedFrame == renderer.frameSerial) {
        return;
    }
    staticMergeCheckedFrame = renderer.frameSerial;
    // The legacy check hashes every material field and path of every candidate each
    // frame to notice edits. The cache already knows what changed, so combine its
    // versions instead, and only hand over to the legacy builder when this moves.
    uint64_t signature = 1469598103934665603ull;
    uint64_t candidates = 0;
    for (uint32_t i = 0; i < sceneObjects.size() && i < cache.size(); ++i) {
        const SceneObject& obj = sceneObjects[i];
        if (!IsStaticMergeCandidate(obj)) continue;
        if (obj.material.alpha < 0.999f) continue;
        if (obj.renderType == RenderType::Sprite && !HasDefaultSpriteUvRect(obj)) continue;
        const RenderOpt::CachedRenderable& entry = cache[i];
        if (entry.objectId != obj.id || !entry.transformValid || !entry.materialValid) {
            signature = HashCombine64(signature, ~static_cast<uint64_t>(i));
            continue;
        }
        resolveAlphaBlend(renderer, i, obj);
        if (entry.albedoAlphaBlend || entry.overlayAlphaBlend) continue;
        ++candidates;
        signature = HashCombine64(signature, static_cast<uint64_t>(static_cast<uint32_t>(obj.id)));
        signature = HashCombine64(signature, static_cast<uint64_t>(obj.meshId + 1));
        signature = HashCombine64(signature, entry.transformVersion);
        signature = HashCombine64(signature, entry.materialVersion);
    }
    signature = HashCombine64(signature, candidates);
    signature = HashCombine64(signature, static_cast<uint64_t>(sceneObjects.size() < 8 ? 1u : 2u));
    if (staticMergeCacheValid && signature == staticMergeCacheSignature) {
        return;
    }
    staticMergeCacheSignature = signature;
    staticMergeCacheValid = true;
    renderer.lastStaticMergeCheckFrameSerial = std::numeric_limits<uint64_t>::max();
    renderer.rebuildStaticMergeBatches(sceneObjects);
}

bool Renderer::OptimizationState::isMultiDrawEligible(Renderer& renderer, const Pass& pass,
                                                      uint32_t objectIndex, const SceneObject& obj,
                                                      Mesh* mesh) {
    // A batch shares every uniform but the model matrix, so anything that sets a
    // per-object uniform or texture the batch cannot vary keeps drawing on its own.
    if (!mesh || mesh->isDynamic() || mesh->usesBones()) return false;
    if (obj.renderType == RenderType::Mirror) return false;
    if (obj.renderType == RenderType::Sprite && (obj.faceCamera || !HasDefaultSpriteUvRect(obj))) {
        return false;
    }
    if (IsUiCanvas3DObject(obj)) return false;
    if (!obj.vertexShaderPath.empty()) return false;
    if (obj.hasSkeletalAnimation && obj.skeletal.enabled) return false;
    if (obj.runtimeHasAlbedoTextureOverride && obj.runtimeAlbedoTextureOverrideId != 0) return false;
    if (obj.material.alpha < 0.999f) return false;
    const RenderOpt::CachedRenderable& entry = cache[objectIndex];
    if (!entry.transformValid || !entry.materialValid) return false;
    if (!obj.albedoTexturePath.empty() && resolveAlphaBlend(renderer, objectIndex, obj)) return false;
    // Reflection probes blend by distance, so every object in range gets its own intensity.
    for (const ProbeRef& probe : pass.probes) {
        const glm::vec3 delta = obj.position - probe.position;
        if (glm::dot(delta, delta) <= probe.range * probe.range) return false;
    }
    return true;
}

void Renderer::OptimizationState::updateMultiDrawMembership(Renderer& renderer, const Pass& pass,
                                                            const std::vector<SceneObject>& sceneObjects) {
    MultiDrawState& md = multiDraw;
    const uint32_t objectCount = static_cast<uint32_t>(sceneObjects.size());
    if (md.membershipFrame == renderer.frameSerial && md.objectSlot.size() == objectCount &&
        md.builtArenaGeneration == md.arenaGeneration) {
        return;
    }
    md.membershipFrame = renderer.frameSerial;

    md.eligible.clear();
    uint64_t signature = 1469598103934665603ull;
    for (uint32_t i = 0; i < objectCount && i < cache.size(); ++i) {
        const SceneObject& obj = sceneObjects[i];
        if (!IsObjectEnabledInHierarchy(obj) || !HasRendererComponent(obj)) continue;
        if (cache[i].objectId != obj.id) continue;
        Mesh* mesh = meshFor(renderer, obj);
        if (!isMultiDrawEligible(renderer, pass, i, obj, mesh)) continue;
        const RenderOpt::CachedRenderable& entry = cache[i];
        uint64_t key = HashCombine64(entry.materialKey, obj.renderType == RenderType::Sprite ? 2u : 1u);
        key = HashCombine64(key, entry.mirrored ? 2u : 1u);
        md.eligible.emplace_back(key, i);
        signature = HashCombine64(signature, key);
        signature = HashCombine64(signature, i);
        signature = HashCombine64(signature, static_cast<uint64_t>(static_cast<uint32_t>(obj.id)));
        signature = HashCombine64(signature, mesh->getSerial());
        signature = HashCombine64(signature, static_cast<uint64_t>(static_cast<uint32_t>(obj.layer)));
    }
    signature = HashCombine64(signature, static_cast<uint64_t>(md.eligible.size()));
    if (md.membershipValid && signature == md.membershipSignature &&
        md.objectSlot.size() == objectCount && md.builtArenaGeneration == md.arenaGeneration) {
        return;
    }

    // Rebuild: equal keys form a batch, each member takes a slot, and a batch's slots are
    // contiguous so one multi-draw covers it. Only happens when membership changed.
    std::sort(md.eligible.begin(), md.eligible.end());
    for (int attempt = 0; attempt < 2; ++attempt) {
        beginArenaRegistration(false);
        md.batches.clear();
        md.slotObject.clear();
        md.slotFirst.clear();
        md.slotCount.clear();
        md.slotTemplates.clear();
        md.objectSlot.assign(objectCount, UINT32_MAX);
        for (const auto& [key, objectIndex] : md.eligible) {
            if (md.slotObject.size() >= RenderOpt::kMaxMultiDrawSlots) break;
            const SceneObject& obj = sceneObjects[objectIndex];
            GLint first = 0;
            GLsizei vertexCount = 0;
            if (!ensureArenaMesh(meshFor(renderer, obj), first, vertexCount, false)) continue;
            if (md.batches.empty() || md.batches.back().key != key) {
                MultiDrawBatch batch;
                batch.key = key;
                batch.firstSlot = static_cast<uint32_t>(md.slotObject.size());
                batch.representative = objectIndex;
                batch.shaderGroup = InternGroup(
                    shaderGroups,
                    ShaderPairHash(kMultiDrawVertPath, cache[objectIndex].paths[RenderOpt::kPathFrag]));
                md.batches.push_back(batch);
            }
            MultiDrawBatch& batch = md.batches.back();
            const uint32_t slot = static_cast<uint32_t>(md.slotObject.size());
            md.slotObject.push_back(objectIndex);
            md.slotFirst.push_back(first);
            md.slotCount.push_back(vertexCount);
            md.slotTemplates.emplace_back(static_cast<GLuint>(vertexCount), static_cast<GLuint>(first),
                                          1u << (static_cast<uint32_t>(obj.layer) & 31u), 0u);
            md.objectSlot[objectIndex] = slot;
            ++batch.slotCount;
            batch.triangles += vertexCount / 3;
        }
        if (!compactArenaIfWasteful()) break;   // emptied a wasteful arena: register again
    }

    const size_t slots = md.slotObject.size();
    md.slotVersion.assign(slots, 0u);
    md.slotMatrices.resize(slots);
    md.slotBounds.resize(slots * 2);
    md.passCommands.resize(slots);
    md.templatesDirty = true;
    const int regions = support.computeCulling ? RenderOpt::kGpuCommandRegions : 1;
    if (slots == 0 || !ensureMultiDrawCapacity(slots, regions)) {
        md.batches.clear();
        md.slotObject.clear();
        md.objectSlot.assign(objectCount, UINT32_MAX);
    }
    md.membershipSignature = signature;
    md.membershipValid = true;
    md.builtArenaGeneration = md.arenaGeneration;
}

bool Renderer::OptimizationState::prepareShadowBatch(Renderer& renderer, Pass& pass,
                                                     const std::vector<SceneObject>& sceneObjects) {
    OptimizationState& state = *renderer.optState;
    if (!pass.plan.multiDraw || state.multiDrawFailed) {
        return false;
    }
    collectShadowCasters(renderer, pass, sceneObjects);
    ShadowBatchState& sb = state.shadowBatch;
    MultiDrawState& md = state.multiDraw;
    const uint32_t objectCount = static_cast<uint32_t>(sceneObjects.size());
    const bool checkedThisFrame = sb.membershipFrame == renderer.frameSerial &&
                                  sb.objectSlot.size() == objectCount &&
                                  sb.builtArenaGeneration == md.arenaGeneration;
    if (!checkedThisFrame) {
        sb.membershipFrame = renderer.frameSerial;
        // Every caster with a static mesh can batch: the depth pass reads nothing but the
        // mesh and the model matrix. The set only changes when casters come and go.
        uint64_t signature = 1469598103934665603ull;
        for (const auto& caster : pass.shadowCasters) {
            if (caster.second->isDynamic() || caster.second->usesBones()) continue;
            signature = HashCombine64(signature, caster.first);
            signature = HashCombine64(signature,
                                      static_cast<uint64_t>(static_cast<uint32_t>(sceneObjects[caster.first].id)));
            signature = HashCombine64(signature, caster.second->getSerial());
        }
        signature = HashCombine64(signature, static_cast<uint64_t>(pass.shadowCasters.size()));
        if (!sb.membershipValid || signature != sb.membershipSignature ||
            sb.objectSlot.size() != objectCount || sb.builtArenaGeneration != md.arenaGeneration) {
            for (int attempt = 0; attempt < 2; ++attempt) {
                state.beginArenaRegistration(true);
                sb.slotObject.clear();
                sb.slotFirst.clear();
                sb.slotCount.clear();
                sb.objectSlot.assign(objectCount, UINT32_MAX);
                for (const auto& caster : pass.shadowCasters) {
                    if (caster.second->isDynamic() || caster.second->usesBones()) continue;
                    if (sb.slotObject.size() >= RenderOpt::kMaxMultiDrawSlots) break;
                    GLint first = 0;
                    GLsizei vertexCount = 0;
                    if (!state.ensureArenaMesh(caster.second, first, vertexCount, true)) continue;
                    sb.objectSlot[caster.first] = static_cast<uint32_t>(sb.slotObject.size());
                    sb.slotObject.push_back(caster.first);
                    sb.slotFirst.push_back(first);
                    sb.slotCount.push_back(vertexCount);
                }
                if (!state.compactArenaIfWasteful()) break;
            }
            const size_t slots = sb.slotObject.size();
            sb.slotVersion.assign(slots, 0u);
            sb.slotMatrices.resize(slots);
            sb.commands.resize(slots);
            if (slots == 0 || !state.ensureShadowBatchCapacity(slots)) {
                sb.slotObject.clear();
                sb.objectSlot.assign(objectCount, UINT32_MAX);
            }
            sb.membershipSignature = signature;
            sb.membershipValid = true;
            sb.builtArenaGeneration = md.arenaGeneration;
        }
    }
    if (sb.slotObject.empty()) {
        return false;
    }
    state.uploadShadowSlots();
    return true;
}

Renderer::OptimizationState::Pass* Renderer::OptimizationState::beginPass(
    Renderer& renderer, const Camera& camera, const std::vector<SceneObject>& sceneObjects,
    const glm::mat4& view, const glm::mat4& proj, const std::array<glm::vec4, 6>& planes,
    int width, int height, float farPlane, bool drawMirrorObjects) {
    const RenderOptimizationSettings& settings = renderer.optimizationSettings;
    const bool anyEnabled = settings.cpuFrustumCulling || settings.multiDrawIndirect ||
                            settings.gpuDrivenCulling || settings.lodOptimization ||
                            settings.hiZOcclusionCulling || settings.materialSorting ||
                            settings.staticObjectCaching;
    if (!anyEnabled) {
        return nullptr;
    }
    if (!renderer.optState) {
        renderer.optState = std::make_unique<OptimizationState>();
    }
    OptimizationState& state = *renderer.optState;
    if (state.passDepth >= static_cast<int>(state.passes.size())) {
        return nullptr;
    }
    if (!state.supportQueried) {
        state.support = querySupport();
        state.supportQueried = true;
    }
    if (renderer.frameSerial >= state.readbackPollFrame + 4) {
        state.readbackPollFrame = renderer.frameSerial;
        state.pollReadbacks();
    }

    int candidateCount = 0;
    if (settings.strategy == RenderOptimizationStrategy::Automatic) {
        for (const SceneObject& obj : sceneObjects) {
            if (IsObjectEnabledInHierarchy(obj) && HasRendererComponent(obj)) ++candidateCount;
        }
    }
    const PassPlan plan = state.resolvePlan(settings, state.support, candidateCount,
                                            renderer.reflectionCapturePass);
    if (!plan.active) {
        return nullptr;
    }

    Pass& pass = state.passes[static_cast<size_t>(state.passDepth++)];
    pass.plan = plan;
    pass.frameSerial = renderer.frameSerial;
    pass.view = view;
    pass.proj = proj;
    pass.planes = planes;
    pass.cameraPosition = camera.position;
    pass.cameraForward = -glm::vec3(view[0][2], view[1][2], view[2][2]);
    pass.viewDepthRow = -glm::vec4(view[0][2], view[1][2], view[2][2], view[3][2]);
    pass.perspective = std::abs(proj[3][3]) < 0.5f;
    pass.lodPxPerUnit = 0.5f * std::max(std::abs(proj[0][0]) * static_cast<float>(width),
                                        std::abs(proj[1][1]) * static_cast<float>(height));
    pass.farPlane = farPlane;
    pass.width = width;
    pass.height = height;
    pass.cullingMask = camera.cullingMask;
    pass.drawMirrorObjects = drawMirrorObjects;
    pass.reflectionCapture = renderer.reflectionCapturePass;
    pass.depthTexture = renderer.reflectionCapturePass ? 0 : renderer.sceneDepthTextureForPass;
    pass.shadowCastersBuilt = false;
    pass.gpuCommandRegion = -1;
    pass.multiDrawUsed = false;
    pass.multiDrawCpuCullMs = 0.0f;
    {
        MODU_PROFILE_SCOPE("Renderer::BuildRenderQueue", ProfilerSampleCategory::RenderDetail);
        state.refreshTransforms(sceneObjects, camera, !plan.staticCache);
    }
    return &pass;
}

void Renderer::OptimizationState::collectShadowCasters(Renderer& renderer, Pass& pass,
                                                       const std::vector<SceneObject>& sceneObjects) {
    if (pass.shadowCastersBuilt) {
        return;
    }
    pass.shadowCastersBuilt = true;
    pass.shadowCasters.clear();
    // The shadow loops' own filters, applied once per pass instead of per light and face.
    for (uint32_t i = 0; i < sceneObjects.size(); ++i) {
        const SceneObject& obj = sceneObjects[i];
        if (!IsObjectEnabledInHierarchy(obj) || !HasRendererComponent(obj)) continue;
        if (obj.renderType == RenderType::Mirror || obj.renderType == RenderType::Sprite) continue;
        if (IsUiCanvas3DObject(obj)) continue;
        if (obj.hasSkeletalAnimation && obj.skeletal.enabled) continue;
        Mesh* mesh = meshFor(renderer, obj);
        if (!mesh) continue;
        pass.shadowCasters.emplace_back(i, mesh);
    }
}

void Renderer::OptimizationState::buildQueue(Renderer& renderer, Pass& pass,
                                             const std::vector<SceneObject>& sceneObjects,
                                             RenderStats& passStats) {
    using Clock = std::chrono::steady_clock;
    OptimizationState& state = *renderer.optState;
    const PassPlan& plan = pass.plan;
    const RenderOptimizationSettings& settings = renderer.optimizationSettings;
    MultiDrawState& md = state.multiDraw;
    const auto queueStart = Clock::now();

    pass.candidates.clear();
    pass.opaque.clear();
    pass.transparent.clear();
    pass.items.clear();
    pass.shaders.clear();
    pass.probes.clear();

    bool multiDrawPlanned = plan.multiDraw;
    {
        MODU_PROFILE_SCOPE("Renderer::BuildRenderQueue", ProfilerSampleCategory::RenderDetail);
        state.refreshMaterials(sceneObjects, renderer.frameSerial, !plan.staticCache);
        if (!renderer.reflectionCastTargets.empty()) {
            for (const SceneObject& probeObj : sceneObjects) {
                if (!IsObjectEnabledInHierarchy(probeObj) || !probeObj.hasReflectionCast ||
                    !probeObj.reflectionCast.enabled) {
                    continue;
                }
                auto target = renderer.reflectionCastTargets.find(probeObj.id);
                if (target == renderer.reflectionCastTargets.end() || target->second.cube == 0 ||
                    !target->second.hasCapture) {
                    continue;
                }
                ProbeRef probe;
                probe.position = probeObj.position;
                probe.range = std::max(0.01f, probeObj.reflectionCast.blendDistance);
                probe.intensity = probeObj.reflectionCast.intensity;
                probe.cube = target->second.cube;
                pass.probes.push_back(probe);
            }
        }
        if (multiDrawPlanned) {
            // Batches need their own vertex stage. If it does not build, draw everything one
            // at a time rather than put every batched object at one object's matrix.
            Shader* batchProgram = renderer.getShader(kMultiDrawVertPath, renderer.defaultFragPath);
            if (!batchProgram || batchProgram == renderer.defaultShader || batchProgram->ID == 0) {
                if (!state.multiDrawFailed) {
                    std::cerr << "[Renderer] " << kMultiDrawVertPath
                              << " did not build; multi-draw batching is off.\n";
                }
                state.multiDrawFailed = true;
                state.multiDrawFailure =
                    "Resources/Shaders/vert_mdi.glsl did not build, so objects draw one at a "
                    "time. The log has the compiler error.";
                multiDrawPlanned = false;
            }
        }
        if (multiDrawPlanned) {
            state.updateMultiDrawMembership(renderer, pass, sceneObjects);
        } else {
            state.refreshStaticMerge(renderer, sceneObjects, plan.staticCache);
        }
    }

    const bool cullingMaskAllowsAll = (pass.cullingMask == 0xFFFFFFFFu);
    const bool multiDrawActive = multiDrawPlanned && !md.batches.empty() &&
                                 md.objectSlot.size() == sceneObjects.size();
    const bool useStaticMerge = !multiDrawPlanned;
    const bool skipStaticSources = useStaticMerge && cullingMaskAllowsAll &&
                                   !renderer.staticMergeSourceIds.empty();
    const bool gpuCulling = multiDrawActive && plan.gpuCulling && state.ensureComputePrograms();
    const float lodMinPx = settings.lodMinScreenSizePx;
    int submitted = 0;
    int culled = 0;
    int lodCulled = 0;
    int batchedVisible = 0;
    int indirectCommands = 0;
    long long batchedTriangles = 0;

    // False when the object is rejected, counting why.
    const auto passesCulling = [&](const RenderOpt::CullBounds& b) {
        if (!b.hasBounds) {
            return true;   // unknown extent: always drawn, as the legacy path does
        }
        if (plan.cpuCulling &&
            (!RenderOpt::SphereInsidePlanes(pass.planes, b.center, b.radius) ||
             !RenderOpt::AabbInsidePlanes(pass.planes, b.center, b.extents))) {
            ++culled;
            return false;
        }
        if (plan.lod && !RenderOpt::PassesLodScreenSize(pass.viewDepthRow, pass.perspective,
                                                        pass.lodPxPerUnit, lodMinPx, b.center,
                                                        b.radius)) {
            ++lodCulled;
            return false;
        }
        return true;
    };
    const auto layerVisible = [&](const SceneObject& obj) {
        return cullingMaskAllowsAll ||
               (pass.cullingMask & (1u << (static_cast<uint32_t>(obj.layer) & 31u))) != 0u;
    };

    const auto cullStart = Clock::now();
    {
        MODU_PROFILE_SCOPE("Renderer::FrustumCulling", ProfilerSampleCategory::RenderDetail);
        const uint32_t objectCount = static_cast<uint32_t>(sceneObjects.size());
        for (uint32_t i = 0; i < objectCount; ++i) {
            const SceneObject& obj = sceneObjects[i];
            if (!IsObjectEnabledInHierarchy(obj)) continue;
            if (!layerVisible(obj)) continue;
            if (!pass.drawMirrorObjects && obj.hasRenderer && obj.renderType == RenderType::Mirror) continue;
            if (!HasRendererComponent(obj)) continue;
            if (multiDrawActive && md.objectSlot[i] != UINT32_MAX) continue;   // its batch draws it
            RenderOpt::CachedRenderable& entry = state.cache[i];
            if (skipStaticSources) {
                if (entry.staticMergeFlagGeneration != state.staticMergeGeneration) {
                    entry.staticMergeSource = renderer.staticMergeSourceIds.count(obj.id) != 0;
                    entry.staticMergeFlagGeneration = state.staticMergeGeneration;
                }
                if (entry.staticMergeSource) continue;
            }
            if (!meshFor(renderer, obj)) continue;
            ++submitted;
            if (!passesCulling(state.bounds[i])) continue;
            pass.candidates.push_back(i);
        }

        if (multiDrawActive && !gpuCulling) {
            // CPU culling for batched objects: each batch's visible commands are packed at
            // the start of its slot range, so the batch draws exactly its visible count.
            const auto batchedStart = Clock::now();
            for (MultiDrawBatch& batch : md.batches) {
                batch.visibleCount = 0;
                const uint32_t endSlot = batch.firstSlot + batch.slotCount;
                for (uint32_t slot = batch.firstSlot; slot < endSlot; ++slot) {
                    const uint32_t objectIndex = md.slotObject[slot];
                    if (!layerVisible(sceneObjects[objectIndex])) continue;
                    ++submitted;
                    if (!passesCulling(state.bounds[objectIndex])) continue;
                    RenderOpt::DrawArraysIndirectCommand& command =
                        md.passCommands[batch.firstSlot + batch.visibleCount++];
                    command.count = static_cast<GLuint>(md.slotCount[slot]);
                    command.instanceCount = 1;
                    command.first = static_cast<GLuint>(md.slotFirst[slot]);
                    command.baseInstance = slot;
                    ++batchedVisible;
                    batchedTriangles += md.slotCount[slot] / 3;
                }
                indirectCommands += static_cast<int>(batch.visibleCount);
            }
            pass.multiDrawCpuCullMs = MillisecondsSince(batchedStart);
        } else if (gpuCulling) {
            for (const MultiDrawBatch& batch : md.batches) {
                submitted += static_cast<int>(batch.slotCount);
                indirectCommands += static_cast<int>(batch.slotCount);
                batchedTriangles += batch.triangles;   // an upper bound: the GPU decides
            }
        }
    }
    passStats.cpuCullingMs += MillisecondsSince(cullStart);

    if (multiDrawActive) {
        MODU_PROFILE_SCOPE("Renderer::IndirectCommandGeneration", ProfilerSampleCategory::RenderDetail);
        state.uploadMultiDrawSlots();
        if (!gpuCulling) {
            state.uploadPassCommands();
        }
    }

    uint32_t order = 0;
    {
        MODU_PROFILE_SCOPE("Renderer::BuildRenderQueue", ProfilerSampleCategory::RenderDetail);
        for (uint32_t objectIndex : pass.candidates) {
            const SceneObject& obj = sceneObjects[objectIndex];
            const RenderOpt::CachedRenderable& entry = state.cache[objectIndex];
            const RenderOpt::CullBounds& b = state.bounds[objectIndex];
            const bool isCanvas = IsUiCanvas3DObject(obj);
            bool requiresBlending = obj.material.alpha < 0.999f || isCanvas;
            if (!requiresBlending && !obj.albedoTexturePath.empty()) {
                requiresBlending = state.resolveAlphaBlend(renderer, objectIndex, obj);
            }
            const glm::vec3 depthCenter = b.hasBounds ? b.center : obj.position;
            const float cameraDepth = FiniteOr(-(pass.view * glm::vec4(depthCenter, 1.0f)).z, 0.0f);
            if (!requiresBlending) {
                OpaqueEntry opaque;
                opaque.ref = objectIndex;
                opaque.order = order++;
                if (plan.sorting) {
                    const bool doubleSided = obj.renderType == RenderType::Sprite ||
                                             obj.renderType == RenderType::Mirror;
                    const bool gpuSkinned = obj.hasSkeletalAnimation && obj.skeletal.enabled &&
                                            obj.skeletal.useGpuSkinning;
                    const uint32_t stateBits = (doubleSided ? 1u : 0u) | (entry.mirrored ? 2u : 0u) |
                                               (gpuSkinned ? 4u : 0u);
                    opaque.key = OpaqueSortKey(entry.shaderGroup, entry.textureGroup,
                                               entry.materialKey, stateBits, cameraDepth,
                                               pass.farPlane);
                }
                pass.opaque.push_back(opaque);
            } else {
                TransparentEntry transparent;
                transparent.cameraDepth = cameraDepth;
                const glm::vec3 toCamera = depthCenter - pass.cameraPosition;
                transparent.cameraDistanceSq = FiniteOr(glm::dot(toCamera, toCamera), 0.0f);
                transparent.objectId = obj.id;
                transparent.ref = objectIndex;
                transparent.isUiCanvas3D = isCanvas;
                transparent.uiSortingOrder = isCanvas ? obj.ui.sortingOrder : 0;
                transparent.order = order++;
                pass.transparent.push_back(transparent);
            }
        }

        if (useStaticMerge && cullingMaskAllowsAll) {
            for (uint32_t k = 0; k < renderer.staticMergeBatches.size(); ++k) {
                const StaticMergeBatch& batch = renderer.staticMergeBatches[k];
                if (!batch.mesh) continue;
                if (plan.cpuCulling && batch.boundsRadius > 0.0f &&
                    !RenderOpt::SphereInsidePlanes(pass.planes, batch.boundsCenter, batch.boundsRadius)) {
                    continue;
                }
                bool requiresBlending = batch.material.alpha < 0.999f;
                if (!requiresBlending && !batch.albedoTexturePath.empty()) {
                    if (Texture* tex = renderer.getTexture(batch.albedoTexturePath,
                                                           batch.material.textureFilter)) {
                        requiresBlending = tex->UsesAlphaBlending();
                    }
                }
                const float cameraDepth =
                    FiniteOr(-(pass.view * glm::vec4(batch.boundsCenter, 1.0f)).z, 0.0f);
                if (!requiresBlending) {
                    OpaqueEntry opaque;
                    opaque.ref = kRefStaticBatch | k;
                    opaque.order = order++;
                    if (plan.sorting) {
                        const std::string* texturePaths[10] = {
                            &batch.albedoTexturePath, &batch.overlayTexturePath, &batch.normalMapPath,
                            &batch.metallicRoughnessMapPath, &batch.metallicMapPath,
                            &batch.roughnessMapPath, &batch.opacityMapPath, &batch.emissiveMapPath,
                            &batch.occlusionMapPath, &batch.heightMapPath};
                        const uint32_t shaderGroup =
                            InternGroup(state.shaderGroups, ShaderPairHash(batch.vertPath, batch.fragPath));
                        const uint32_t textureGroup = InternGroup(
                            state.textureGroups,
                            TextureSetHash(texturePaths, 10, batch.material.textureFilter, batch.useOverlay));
                        opaque.key = OpaqueSortKey(shaderGroup, textureGroup, 0,
                                                   batch.doubleSided ? 1u : 0u, cameraDepth,
                                                   pass.farPlane);
                    }
                    pass.opaque.push_back(opaque);
                } else {
                    TransparentEntry transparent;
                    transparent.cameraDepth = cameraDepth;
                    const glm::vec3 toCamera = batch.boundsCenter - pass.cameraPosition;
                    transparent.cameraDistanceSq = FiniteOr(glm::dot(toCamera, toCamera), 0.0f);
                    transparent.objectId = -1;
                    transparent.ref = kRefStaticBatch | k;
                    transparent.order = order++;
                    pass.transparent.push_back(transparent);
                }
            }
        }

        if (multiDrawActive) {
            for (uint32_t k = 0; k < md.batches.size(); ++k) {
                const MultiDrawBatch& batch = md.batches[k];
                if (gpuCulling ? batch.slotCount == 0 : batch.visibleCount == 0) continue;
                OpaqueEntry opaque;
                opaque.ref = kRefMultiDraw | k;
                opaque.order = order++;
                if (plan.sorting) {
                    const RenderOpt::CachedRenderable& representative = state.cache[batch.representative];
                    const bool sprite = sceneObjects[batch.representative].renderType == RenderType::Sprite;
                    opaque.key = OpaqueSortKey(batch.shaderGroup, representative.textureGroup,
                                               representative.materialKey,
                                               (sprite ? 1u : 0u) | (representative.mirrored ? 2u : 0u) | 8u,
                                               0.0f, pass.farPlane);
                }
                pass.opaque.push_back(opaque);
            }
        }
    }

    const auto sortStart = Clock::now();
    {
        MODU_PROFILE_SCOPE("Renderer::MaterialSorting", ProfilerSampleCategory::RenderDetail);
        if (plan.sorting && pass.opaque.size() > 1 && pass.opaque.size() < 256) {
            std::sort(pass.opaque.begin(), pass.opaque.end(),
                      [](const OpaqueEntry& a, const OpaqueEntry& b) {
                          if (a.key != b.key) return a.key < b.key;
                          return a.order < b.order;
                      });
        } else if (plan.sorting && pass.opaque.size() > 1) {
            // Entries arrive in gather order, so the stable radix sort ties the same way
            // the comparison sort above does, in a few linear passes instead.
            RenderOpt::RadixSortByKey(pass.opaque, pass.opaqueScratch);
        }
        // Transparent order is about correctness, so it ignores the sorting setting: back
        // to front exactly as the legacy path does (its stable sort ties on gather order).
        if (pass.transparent.size() > 1) {
            std::sort(pass.transparent.begin(), pass.transparent.end(),
                      [](const TransparentEntry& a, const TransparentEntry& b) {
                          if (a.cameraDepth != b.cameraDepth) return a.cameraDepth > b.cameraDepth;
                          if (a.cameraDistanceSq != b.cameraDistanceSq) {
                              return a.cameraDistanceSq > b.cameraDistanceSq;
                          }
                          if (a.objectId != b.objectId) return a.objectId < b.objectId;
                          return a.order < b.order;
                      });
            // 3D UI canvases then reorder among the slots they occupy by sorting order.
            pass.canvasPositions.clear();
            pass.canvasEntries.clear();
            for (uint32_t k = 0; k < pass.transparent.size(); ++k) {
                if (!pass.transparent[k].isUiCanvas3D) continue;
                pass.canvasPositions.push_back(k);
                pass.canvasEntries.push_back(pass.transparent[k]);
                pass.canvasEntries.back().order = static_cast<uint32_t>(pass.canvasEntries.size() - 1);
            }
            if (pass.canvasEntries.size() > 1) {
                std::sort(pass.canvasEntries.begin(), pass.canvasEntries.end(),
                          [](const TransparentEntry& a, const TransparentEntry& b) {
                              if (a.uiSortingOrder != b.uiSortingOrder) {
                                  return a.uiSortingOrder < b.uiSortingOrder;
                              }
                              return a.order < b.order;
                          });
                for (size_t k = 0; k < pass.canvasPositions.size(); ++k) {
                    pass.transparent[pass.canvasPositions[k]] = pass.canvasEntries[k];
                }
            }
        }
    }
    passStats.sortingMs += MillisecondsSince(sortStart);

    {
        MODU_PROFILE_SCOPE("Renderer::BuildRenderQueue", ProfilerSampleCategory::RenderDetail);
        // Mirrors the legacy gather field for field, so the draw loop cannot tell the paths
        // apart except by what was culled and the order.
        const auto fillObjectItem = [&](SceneRenderItem& item, uint32_t objectIndex) {
            const SceneObject& obj = sceneObjects[objectIndex];
            const RenderOpt::CullBounds& b = state.bounds[objectIndex];
            item.obj = &obj;
            item.mesh = meshFor(renderer, obj);
            item.model = state.cache[objectIndex].model;
            item.sortCenter = obj.position;
            item.depthSortCenter = b.hasBounds ? b.center : obj.position;
            item.vertPath = &obj.vertexShaderPath;
            item.fragPath = &obj.fragmentShaderPath;
            item.material = obj.material;
            item.materialPath = &obj.materialPath;
            item.albedoTexturePath = &obj.albedoTexturePath;
            item.overlayTexturePath = &obj.overlayTexturePath;
            item.normalMapPath = &obj.normalMapPath;
            item.metallicRoughnessMapPath = &obj.metallicRoughnessMapPath;
            item.metallicMapPath = &obj.metallicMapPath;
            item.roughnessMapPath = &obj.roughnessMapPath;
            item.opacityMapPath = &obj.opacityMapPath;
            item.emissiveMapPath = &obj.emissiveMapPath;
            item.occlusionMapPath = &obj.occlusionMapPath;
            item.heightMapPath = &obj.heightMapPath;
            item.useOverlay = obj.useOverlay;
            item.boneLimit = obj.skeletal.maxBones;
            item.availableBones = static_cast<int>(obj.skeletal.finalMatrices.size());
            const bool needsFallback = obj.hasSkeletalAnimation && obj.skeletal.enabled &&
                                       obj.skeletal.allowCpuFallback &&
                                       item.boneLimit > 0 && item.availableBones > item.boneLimit;
            item.wantsGpuSkinning = obj.hasSkeletalAnimation && obj.skeletal.enabled &&
                                    obj.skeletal.useGpuSkinning && !needsFallback;
            if (item.wantsGpuSkinning && item.vertPath->empty()) {
                item.vertPath = &renderer.skinnedVertPath;
            }
            item.isUiCanvas3D = IsUiCanvas3DObject(obj);
            item.uiSortingOrder = item.isUiCanvas3D ? obj.ui.sortingOrder : 0;
            item.unlit = obj.renderType == RenderType::Mirror || obj.renderType == RenderType::Sprite ||
                         item.isUiCanvas3D;
            item.doubleSided = obj.renderType == RenderType::Sprite || obj.renderType == RenderType::Mirror;
            const glm::vec3 viewSpaceCenter = glm::vec3(pass.view * glm::vec4(item.depthSortCenter, 1.0f));
            item.cameraDepth = -viewSpaceCenter.z;
            const glm::vec3 toCamera = item.depthSortCenter - pass.cameraPosition;
            item.cameraDistanceSq = glm::dot(toCamera, toCamera);
        };
        const auto emit = [&](uint32_t ref, bool opaque) {
            pass.items.emplace_back();
            SceneRenderItem& item = pass.items.back();
            if (ref & kRefStaticBatch) {
                const StaticMergeBatch& batch = renderer.staticMergeBatches[ref & kRefIndexMask];
                item.staticBatch = &batch;
                item.mesh = batch.mesh.get();
                item.model = glm::mat4(1.0f);
                item.sortCenter = batch.boundsCenter;
                item.depthSortCenter = batch.boundsCenter;
                item.vertPath = &batch.vertPath;
                item.fragPath = &batch.fragPath;
                item.material = batch.material;
                item.materialPath = &batch.materialPath;
                item.albedoTexturePath = &batch.albedoTexturePath;
                item.overlayTexturePath = &batch.overlayTexturePath;
                item.normalMapPath = &batch.normalMapPath;
                item.metallicRoughnessMapPath = &batch.metallicRoughnessMapPath;
                item.metallicMapPath = &batch.metallicMapPath;
                item.roughnessMapPath = &batch.roughnessMapPath;
                item.opacityMapPath = &batch.opacityMapPath;
                item.emissiveMapPath = &batch.emissiveMapPath;
                item.occlusionMapPath = &batch.occlusionMapPath;
                item.heightMapPath = &batch.heightMapPath;
                item.useOverlay = batch.useOverlay;
                item.unlit = batch.unlit;
                item.doubleSided = batch.doubleSided;
                const glm::vec3 viewSpaceCenter = glm::vec3(pass.view * glm::vec4(item.depthSortCenter, 1.0f));
                item.cameraDepth = -viewSpaceCenter.z;
                const glm::vec3 toCamera = item.depthSortCenter - pass.cameraPosition;
                item.cameraDistanceSq = glm::dot(toCamera, toCamera);
            } else if (ref & kRefMultiDraw) {
                const uint32_t batchIndex = ref & kRefIndexMask;
                fillObjectItem(item, md.batches[batchIndex].representative);
                item.mesh = nullptr;
                item.multiDrawBatch = static_cast<int>(batchIndex);
            } else {
                fillObjectItem(item, ref);
            }
            item.requiresBlending = !opaque;
            item.sortOpaque = opaque;
        };
        pass.items.reserve(pass.opaque.size() + pass.transparent.size());
        for (const OpaqueEntry& entry : pass.opaque) {
            emit(entry.ref, true);
        }
        for (const TransparentEntry& entry : pass.transparent) {
            emit(entry.ref, false);
        }

        // Resolve each shader pair once per pass instead of building its cache key per draw,
        // and each item's reflection probe once instead of scanning the scene per draw.
        const auto resolveShader = [&](const std::string& vert, const std::string& frag) {
            for (const ShaderSlot& slot : pass.shaders) {
                if (*slot.vert == vert && *slot.frag == frag) return slot.shader;
            }
            Shader* shader = renderer.getShader(vert, frag);
            pass.shaders.push_back({&vert, &frag, shader});
            return shader;
        };
        for (SceneRenderItem& item : pass.items) {
            const std::string& frag = item.fragPath ? *item.fragPath : kEmptyRenderPath;
            if (item.multiDrawBatch >= 0) {
                Shader* shader = resolveShader(kMultiDrawVertPath, frag);
                // A material shader that fails to build falls back to the default program,
                // whose vertex stage reads the per-draw model uniform. Keep the batch vertex
                // stage and fall back only the fragment half.
                if (shader == renderer.defaultShader) {
                    shader = resolveShader(kMultiDrawVertPath, renderer.defaultFragPath);
                }
                item.shader = shader;
            } else {
                item.shader = resolveShader(item.vertPath ? *item.vertPath : kEmptyRenderPath, frag);
            }
            item.reflectionResolved = true;
            item.reflectionCube = 0;
            item.reflectionIntensity = 0.0f;
            float bestDistSq = std::numeric_limits<float>::max();
            for (const ProbeRef& probe : pass.probes) {
                const glm::vec3 delta = item.sortCenter - probe.position;
                const float distSq = glm::dot(delta, delta);
                if (distSq > probe.range * probe.range || distSq >= bestDistSq) continue;
                bestDistSq = distSq;
                item.reflectionCube = probe.cube;
                const float distance = std::sqrt(std::max(0.0f, distSq));
                item.reflectionIntensity =
                    probe.intensity * glm::clamp(1.0f - distance / probe.range, 0.0f, 1.0f);
            }
        }
    }

    if (gpuCulling) {
        MODU_PROFILE_SCOPE("Renderer::GPUCulling", ProfilerSampleCategory::RenderDetail);
        pass.gpuCommandRegion = state.dispatchGpuCulling(pass, settings, plan.hiZ, renderer.activeStats);
        if (pass.gpuCommandRegion < 0) {
            // The dispatch could not run; draw every batched object rather than none.
            for (MultiDrawBatch& batch : md.batches) {
                batch.visibleCount = batch.slotCount;
                for (uint32_t slot = batch.firstSlot; slot < batch.firstSlot + batch.slotCount; ++slot) {
                    RenderOpt::DrawArraysIndirectCommand& command = md.passCommands[slot];
                    command.count = static_cast<GLuint>(md.slotCount[slot]);
                    command.instanceCount = 1;
                    command.first = static_cast<GLuint>(md.slotFirst[slot]);
                    command.baseInstance = slot;
                }
            }
            state.uploadPassCommands();
        }
    }

    pass.multiDrawUsed = multiDrawActive;
    passStats.objectsSubmitted += submitted;
    passStats.objectsCulled += culled;
    passStats.objectsLodCulled += lodCulled;
    passStats.visibleRenderables += static_cast<int>(pass.candidates.size()) + batchedVisible;
    passStats.triangles += batchedTriangles;
    passStats.indirectDrawCommands += indirectCommands;
    if (multiDrawActive) {
        passStats.mdiObjects += static_cast<int>(md.slotObject.size());
        passStats.mdiBatches += static_cast<int>(md.batches.size());
    }
    passStats.queueBuildMs += MillisecondsSince(queueStart);
}

void Renderer::OptimizationState::finishPass(Renderer& renderer, Pass& pass, RenderStats& passStats) {
    OptimizationState& state = *renderer.optState;
    const PassPlan& plan = pass.plan;
    // Next frame's Hi-Z test reads this frame's finished depth.
    if (plan.hiZ && pass.depthTexture != 0 && !pass.reflectionCapture && state.ensureComputePrograms()) {
        MODU_PROFILE_SCOPE("Renderer::GPUCulling", ProfilerSampleCategory::RenderDetail);
        state.buildHiZ(pass);
    }
    const GpuResult gpu = state.latestGpuResult(renderer.activeStats);
    if (pass.gpuCommandRegion >= 0) {
        passStats.gpuCullingMs = gpu.ms;
        passStats.gpuVisible = gpu.visible;
    }
    uint32_t features = 0;
    if (plan.cpuCulling) features |= kRenderOptCpuCulling;
    if (plan.staticCache) features |= kRenderOptStaticCache;
    if (plan.sorting) features |= kRenderOptSorting;
    if (plan.lod) features |= kRenderOptLod;
    if (pass.multiDrawUsed) features |= kRenderOptMultiDraw;
    if (pass.gpuCommandRegion >= 0) features |= kRenderOptGpuCulling;
    if (pass.gpuCommandRegion >= 0 && plan.hiZ) features |= kRenderOptHiZ;
    passStats.optimizationFeatures = features;
    passStats.optimizationAutomatic = plan.automatic;
    state.notePassCosts(plan, passStats.mdiObjects, passStats.mdiBatches, pass.multiDrawCpuCullMs,
                        pass.gpuCommandRegion >= 0 ? gpu.ms : -1.0f);

    // MODULARITY_RENDER_OPT_LOG=1 prints one main view's pass every 5 s: the numbers the
    // stats overlay shows, for builds and players that have no overlay.
    static const bool logPasses = [] {
        const char* value = std::getenv("MODULARITY_RENDER_OPT_LOG");
        return value && value[0] == '1';
    }();
    static double lastLogSec[2] = {0.0, 0.0};
    const int logView = renderer.activeStats == &renderer.viewportStats ? 0
                      : renderer.activeStats == &renderer.previewStats ? 1 : -1;
    if (logPasses && logView >= 0 && !pass.reflectionCapture) {
        const double now = glfwGetTime();
        if (now - lastLogSec[logView] >= 5.0) {
            lastLogSec[logView] = now;
            std::fprintf(stderr,
                         "[RenderOpt] %s%s: features 0x%02x | submitted %d culled %d lod %d visible %d | "
                         "multi-draw %d obj %d batches %d cmds | queue %.3f ms "
                         "(cull %.3f, sort %.3f, batched cull %.3f) | gpu cull %.3f ms %d visible\n",
                         logView == 0 ? "viewport" : "preview", plan.automatic ? " (auto)" : "",
                         features, passStats.objectsSubmitted, passStats.objectsCulled,
                         passStats.objectsLodCulled, passStats.visibleRenderables,
                         passStats.mdiObjects, passStats.mdiBatches, passStats.indirectDrawCommands,
                         passStats.queueBuildMs, passStats.cpuCullingMs,
                         passStats.sortingMs, pass.multiDrawCpuCullMs, passStats.gpuCullingMs,
                         passStats.gpuVisible);
        }
    }

    if (RenderStats* stats = renderer.activeStats) {
        stats->objectsSubmitted += passStats.objectsSubmitted;
        stats->objectsCulled += passStats.objectsCulled;
        stats->objectsLodCulled += passStats.objectsLodCulled;
        stats->visibleRenderables += passStats.visibleRenderables;
        stats->mdiObjects += passStats.mdiObjects;
        stats->mdiBatches += passStats.mdiBatches;
        stats->indirectDrawCommands += passStats.indirectDrawCommands;
        stats->triangles += passStats.triangles;
        stats->cpuCullingMs += passStats.cpuCullingMs;
        stats->queueBuildMs += passStats.queueBuildMs;
        stats->sortingMs += passStats.sortingMs;
        stats->optimizationFeatures |= passStats.optimizationFeatures;
        stats->optimizationAutomatic = stats->optimizationAutomatic || passStats.optimizationAutomatic;
        if (pass.gpuCommandRegion >= 0) {
            stats->gpuCullingMs = passStats.gpuCullingMs;
            stats->gpuVisible = passStats.gpuVisible;
        }
    }
}

void Renderer::renderSceneInternal(const Camera& camera, const std::vector<SceneObject>& sceneObjects, int width, int height, bool unbindFramebuffer, float fovDeg, float nearPlane, float farPlane, bool drawMirrorObjects, bool drawSkybox) {
    if (!defaultShader || width <= 0 || height <= 0) return;

    if (skybox) {
        glm::vec3 sunDirection(0.0f, 1.0f, 0.0f);
        const bool haveDirectionalLight = MainDirectionalSunDirection(sceneObjects, sunDirection);
        skybox->setDirectionalLight(sunDirection, haveDirectionalLight);
    }
    if (camera.orthographic && camera.orthoSize <= 0.0f) {
        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (unbindFramebuffer) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        return;
    }
    if (!reflectionCapturePass) {
        updateSkyboxReflectionTarget();
        updateReflectionCastTargets(sceneObjects, nearPlane, farPlane);
    }

    struct LightUniform {
        int type = 0; // 0 dir,1 point,2 spot,3 area
        int sourceId = -1;
        glm::vec3 dir = glm::vec3(0.0f, -1.0f, 0.0f);
        glm::vec3 pos = glm::vec3(0.0f);
        glm::vec3 color = glm::vec3(1.0f);
        float intensity = 1.0f;
        float range = 10.0f;
        float inner = glm::cos(glm::radians(15.0f));
        float outer = glm::cos(glm::radians(25.0f));
        glm::vec2 areaSize = glm::vec2(1.0f); // width/height for area lights
        float areaFade = 0.0f; // 0 sharp, 1 fully softened
        bool castShadows = false;
        int shadowKind = 0; // 0 off, 1 cube, 2 directional
        int shadowMode = 0; // 0 off, 1 hard, 2 soft
        float shadowBias = 0.02f;
        float shadowSoftness = 0.04f;
        float shadowFar = 10.0f;
        int shadowResolution = 512;
        int shadowMapIndex = -1;
        glm::mat4 shadowMatrix = glm::mat4(1.0f);
    };
    auto forwardFromRotation = [](const SceneObject& obj) {
        glm::mat4 rotation(1.0f);
        rotation = glm::rotate(rotation, glm::radians(obj.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        rotation = glm::rotate(rotation, glm::radians(obj.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        rotation = glm::rotate(rotation, glm::radians(obj.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));

        glm::vec3 f = glm::normalize(glm::vec3(rotation * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));
        if (glm::length(f) < 1e-3f ||
            !std::isfinite(f.x) || !std::isfinite(f.y) || !std::isfinite(f.z)) {
            f = glm::vec3(0.0f, 0.0f, 1.0f);
        }
        return f;
    };
    auto buildModelMatrix = [&](const SceneObject& obj) {
        return BuildSceneObjectModelMatrix(obj, &camera.position, &camera.up);
    };
    auto selectMeshForObject = [&](const SceneObject& obj) -> Mesh* {
        if (obj.renderType == RenderType::Cube) return cubeMesh;
        if (obj.renderType == RenderType::Sphere) return sphereMesh;
        if (obj.renderType == RenderType::Capsule) return capsuleMesh;
        if (obj.renderType == RenderType::Plane) return planeMesh;
        if (obj.renderType == RenderType::Mirror) return planeMesh;
        if (obj.renderType == RenderType::Sprite) return planeMesh;
        if (obj.renderType == RenderType::Torus) return torusMesh;
        if (obj.renderType == RenderType::OBJMesh && obj.meshId != -1) {
            return g_objLoader.getMesh(obj.meshId);
        }
        if (obj.renderType == RenderType::Model && obj.meshId != -1) {
            return getModelLoader().getMesh(obj.meshId);
        }
        return nullptr;
    };

    constexpr size_t kMaxLights = static_cast<size_t>(kRendererMaxRealtimeLights);
    const size_t activeLightLimit = static_cast<size_t>(std::clamp(
        std::min(maxRealtimeLights, sceneLightBudget + directionalLightAllowance),
        1, kRendererMaxRealtimeLights));
    const size_t guaranteedDirectionals = static_cast<size_t>(std::clamp(directionalLightAllowance, 1, 4));
    std::vector<LightUniform> lights;
    lights.reserve(activeLightLimit);
    size_t directionalCount = 0;

    struct LightCandidate {
        LightUniform light;
        float distSq = 0.0f;
        int id = 0;
    };
    std::vector<LightCandidate> candidates;
    candidates.reserve(sceneObjects.size());

    for (const auto& obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasLight || !obj.light.enabled) continue;
        if (obj.light.type == LightType::Directional && !mainLightEnabled) continue;
        if (obj.light.type != LightType::Directional && !additionalLightsEnabled) continue;
        if (obj.light.type == LightType::Directional) {
            LightUniform l;
            l.type = 0;
            l.sourceId = obj.id;
            l.pos = obj.position;
            l.dir = forwardFromRotation(obj);
            l.color = obj.light.color;
            l.intensity = obj.light.intensity;
            l.castShadows = obj.light.castShadows;
            l.shadowMode = obj.light.castShadows ? (obj.light.softShadows ? 2 : 1) : 0;
            l.shadowBias = glm::clamp(obj.light.shadowBias, 0.0001f, 0.2f);
            l.shadowSoftness = glm::clamp(obj.light.shadowSoftness, 0.0f, 0.2f);
            l.shadowFar = glm::max(farPlane, nearPlane + 1.0f);
            l.shadowResolution = (obj.light.shadowResolution > 0)
                ? std::clamp(obj.light.shadowResolution, 128, 8192)
                : shadowMapResolution;
            if (directionalCount < guaranteedDirectionals) {
                lights.push_back(l);
                ++directionalCount;
                if (lights.size() >= activeLightLimit) break;
            } else {
                LightCandidate c;
                c.light = l;
                glm::vec3 delta = obj.position - camera.position;
                c.distSq = glm::dot(delta, delta);
                c.id = obj.id;
                candidates.push_back(c);
            }
        } else if (obj.light.type == LightType::Spot) {
            LightUniform l;
            l.type = 2;
            l.sourceId = obj.id;
            l.pos = obj.position;
            l.dir = forwardFromRotation(obj);
            l.color = obj.light.color;
            l.intensity = obj.light.intensity;
            l.range = glm::max(obj.light.range, 1.0f);
            l.inner = glm::cos(glm::radians(obj.light.innerAngle));
            l.outer = glm::cos(glm::radians(obj.light.outerAngle));
            l.castShadows = obj.light.castShadows;
            l.shadowMode = obj.light.castShadows ? (obj.light.softShadows ? 2 : 1) : 0;
            l.shadowBias = glm::clamp(obj.light.shadowBias, 0.0001f, 0.2f);
            l.shadowSoftness = glm::clamp(obj.light.shadowSoftness, 0.0f, 0.2f);
            l.shadowFar = l.range;
            l.shadowResolution = (obj.light.shadowResolution > 0)
                ? std::clamp(obj.light.shadowResolution, 128, 8192)
                : shadowMapResolution;
            LightCandidate c;
            c.light = l;
            glm::vec3 delta = obj.position - camera.position;
            c.distSq = glm::dot(delta, delta);
            c.id = obj.id;
            candidates.push_back(c);
        } else if (obj.light.type == LightType::Point) {
            LightUniform l;
            l.type = 1;
            l.sourceId = obj.id;
            l.pos = obj.position;
            l.color = obj.light.color;
            l.intensity = obj.light.intensity;
            l.range = glm::max(obj.light.range, 1.0f);
            l.castShadows = obj.light.castShadows;
            l.shadowMode = obj.light.castShadows ? (obj.light.softShadows ? 2 : 1) : 0;
            l.shadowBias = glm::clamp(obj.light.shadowBias, 0.0001f, 0.2f);
            l.shadowSoftness = glm::clamp(obj.light.shadowSoftness, 0.0f, 0.2f);
            l.shadowFar = l.range;
            l.shadowResolution = (obj.light.shadowResolution > 0)
                ? std::clamp(obj.light.shadowResolution, 128, 8192)
                : shadowMapResolution;
            LightCandidate c;
            c.light = l;
            glm::vec3 delta = obj.position - camera.position;
            c.distSq = glm::dot(delta, delta);
            c.id = obj.id;
            candidates.push_back(c);
        } else if (obj.light.type == LightType::Area) {
            LightUniform l;
            l.type = 3; // area
            l.sourceId = obj.id;
            l.pos = obj.position;
            l.dir = forwardFromRotation(obj); // plane normal
            l.color = obj.light.color;
            l.intensity = obj.light.intensity;
            float sizeHint = glm::max(obj.light.size.x, obj.light.size.y);
            l.range = (obj.light.range > 0.0f) ? obj.light.range : glm::max(sizeHint * 2.0f, 1.0f);
            l.areaSize = obj.light.size;
            l.areaFade = glm::clamp(obj.light.edgeFade, 0.0f, 1.0f);
            l.castShadows = obj.light.castShadows;
            l.shadowMode = obj.light.castShadows ? (obj.light.softShadows ? 2 : 1) : 0;
            l.shadowBias = glm::clamp(obj.light.shadowBias, 0.0001f, 0.2f);
            l.shadowSoftness = glm::clamp(obj.light.shadowSoftness, 0.0f, 0.2f);
            l.shadowFar = glm::max(l.range, 1.0f);
            l.shadowResolution = (obj.light.shadowResolution > 0)
                ? std::clamp(obj.light.shadowResolution, 128, 8192)
                : shadowMapResolution;
            LightCandidate c;
            c.light = l;
            glm::vec3 delta = obj.position - camera.position;
            c.distSq = glm::dot(delta, delta);
            c.id = obj.id;
            candidates.push_back(c);
        }
    }

    if (lights.size() < activeLightLimit && !candidates.empty()) {
        const auto candidateLess = [](const LightCandidate& a, const LightCandidate& b) {
            if (a.distSq != b.distSq) return a.distSq < b.distSq;
            return a.id < b.id;
        };
        const size_t remainingSlots = activeLightLimit - lights.size();
        if (candidates.size() > remainingSlots) {
            std::nth_element(candidates.begin(),
                             candidates.begin() + remainingSlots,
                             candidates.end(),
                             candidateLess);
            candidates.resize(remainingSlots);
        }
        std::sort(candidates.begin(), candidates.end(), candidateLess);
        for (const auto& c : candidates) {
            if (lights.size() >= activeLightLimit) break;
            lights.push_back(c.light);
        }
    }

    for (auto& l : lights) {
        const bool isDirectional = (l.type == 0);
        bool allowShadows = camera.renderShadows &&
                            (isDirectional ? mainLightShadows : additionalLightsShadows);
        if (!allowShadows) {
            l.castShadows = false;
            l.shadowMode = 0;
        } else if (!softShadowsAllowed && l.shadowMode == 2) {
            l.shadowMode = 1;
        }
    }

    glm::mat4 view = camera.getViewMatrix();
    glm::mat4 proj = sceneProjectionOverride
        ? *sceneProjectionOverride
        : BuildCameraProjection(camera, width, height, fovDeg, nearPlane, farPlane);
    {
        sunOnScreen = false;
        if (skybox) {
            const glm::vec3 sunDirection = skybox->resolvedSunDirection();
            const glm::vec4 clip = proj * view * glm::vec4(sunDirection, 0.0f);
            if (clip.w > 0.0001f) {
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                const glm::vec2 uv = glm::vec2(ndc) * 0.5f + 0.5f;
                if (std::isfinite(uv.x) && std::isfinite(uv.y)) {
                    sunScreenUv = uv;
                    sunOnScreen = uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f;
                }
            }
        }
    }

    FrustumPlanes frustum = BuildFrustumPlanes(proj * view);
    capturedSceneFrustums.push_back(frustum.planes);
    capturedSceneViews.push_back({view, proj, width, height});
    const float timeSeconds = static_cast<float>(glfwGetTime());

    // Project Settings > Optimization. Null when nothing is enabled, or when Automatic
    // picked the plain path for this scene: then everything below runs as it always has.
    OptimizationState::Pass* optPass = OptimizationState::beginPass(
        *this, camera, sceneObjects, view, proj, frustum.planes, width, height, farPlane,
        drawMirrorObjects);
    struct OptimizedPassEnd {
        Renderer* renderer = nullptr;
        ~OptimizedPassEnd() {
            if (renderer && renderer->optState) --renderer->optState->passDepth;
        }
    } optimizedPassEnd{optPass ? this : nullptr};
    RenderStats optPassStats;

    auto buildDirectionalShadowMatrix = [&](const glm::vec3& lightDir) {
        glm::mat4 shadowFitProj = proj;
        if (!sceneProjectionOverride && shadowMaxDistance > 0.0f && shadowMaxDistance < farPlane) {
            const float shadowFar = std::max(nearPlane + 0.01f, shadowMaxDistance);
            shadowFitProj = BuildCameraProjection(camera, width, height, fovDeg, nearPlane, shadowFar);
        }
        const glm::mat4 invViewProj = glm::inverse(shadowFitProj * view);
        std::array<glm::vec3, 8> corners;
        int cornerIndex = 0;
        for (int x = 0; x <= 1; ++x) {
            for (int y = 0; y <= 1; ++y) {
                for (int z = 0; z <= 1; ++z) {
                    glm::vec4 corner = invViewProj * glm::vec4(
                        x == 0 ? -1.0f : 1.0f,
                        y == 0 ? -1.0f : 1.0f,
                        z == 0 ? -1.0f : 1.0f,
                        1.0f);
                    corners[cornerIndex++] = glm::vec3(corner) / std::max(corner.w, 0.0001f);
                }
            }
        }

        glm::vec3 center(0.0f);
        for (const glm::vec3& corner : corners) {
            center += corner;
        }
        center /= static_cast<float>(corners.size());

        glm::vec3 dir = glm::normalize(lightDir);
        if (glm::length(dir) < 1e-4f || !std::isfinite(dir.x) || !std::isfinite(dir.y) || !std::isfinite(dir.z)) {
            dir = glm::vec3(0.0f, -1.0f, 0.0f);
        }

        float radius = 0.0f;
        for (const glm::vec3& corner : corners) {
            radius = glm::max(radius, glm::length(corner - center));
        }
        radius = glm::max(radius, 5.0f);

        glm::vec3 up = (std::abs(dir.y) > 0.95f) ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        glm::mat4 lightView = glm::lookAt(center - dir * (radius * 2.0f), center, up);

        glm::vec3 minBounds(std::numeric_limits<float>::max());
        glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
        for (const glm::vec3& corner : corners) {
            glm::vec3 lightSpaceCorner = glm::vec3(lightView * glm::vec4(corner, 1.0f));
            minBounds = glm::min(minBounds, lightSpaceCorner);
            maxBounds = glm::max(maxBounds, lightSpaceCorner);
        }

        const float xyPadding = glm::max(radius * 0.15f, 2.0f);
        const float zPadding = glm::max(radius * 0.5f, 10.0f);
        minBounds.x -= xyPadding;
        minBounds.y -= xyPadding;
        minBounds.z -= zPadding;
        maxBounds.x += xyPadding;
        maxBounds.y += xyPadding;
        maxBounds.z += zPadding;

        const float nearPlane = glm::max(0.1f, -maxBounds.z);
        const float farPlane = glm::max(nearPlane + 0.1f, -minBounds.z);
        glm::mat4 lightProj = glm::ortho(
            minBounds.x, maxBounds.x,
            minBounds.y, maxBounds.y,
            nearPlane, farPlane);
        return lightProj * lightView;
    };

    std::array<unsigned int, kMaxShadowMaps> shadowTextures = {0, 0, 0, 0};
    std::array<unsigned int, kMaxShadowMaps> shadowDirectionalTextures = {0, 0, 0, 0};
    std::unordered_set<int> activeCubeShadowIds;
    std::unordered_set<int> activeDirectionalShadowIds;
    auto releaseShadowCubeMap = [](ShadowCubeMap& map) {
        if (map.fbo) glDeleteFramebuffers(1, &map.fbo);
        if (map.depthCube) glDeleteTextures(1, &map.depthCube);
        map.fbo = 0;
        map.depthCube = 0;
        map.resolution = 0;
    };
    auto releaseDirectionalShadowMap = [](ShadowDirectionalMap& map) {
        if (map.fbo) glDeleteFramebuffers(1, &map.fbo);
        if (map.depthTexture) glDeleteTextures(1, &map.depthTexture);
        map.fbo = 0;
        map.depthTexture = 0;
        map.resolution = 0;
    };
    const bool hasShadowCasters = std::any_of(lights.begin(), lights.end(), [](const LightUniform& light) {
        return light.castShadows && light.sourceId >= 0;
    });
    const bool canRenderAnyShadowMaps =
        (shadowDepthShader && shadowDepthShader->ID != 0) ||
        (directionalShadowDepthShader && directionalShadowDepthShader->ID != 0);
    if (canRenderAnyShadowMaps && hasShadowCasters) {
        MODU_PROFILE_SCOPE("Shadow Pass", ProfilerSampleCategory::RenderDetail);
        GLint prevViewport[4] = {0, 0, width, height};
        GLint prevFbo = 0;
        GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
        GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
        GLboolean cullWasEnabled = glIsEnabled(GL_CULL_FACE);
        GLboolean polyOffsetWasEnabled = glIsEnabled(GL_POLYGON_OFFSET_FILL);
        GLint cullModeBefore = GL_BACK;
        glGetIntegerv(GL_VIEWPORT, prevViewport);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        glGetIntegerv(GL_CULL_FACE_MODE, &cullModeBefore);

        if (blendWasEnabled) glDisable(GL_BLEND);
        if (!depthWasEnabled) glEnable(GL_DEPTH_TEST);
        if (!polyOffsetWasEnabled) glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(2.0f, 4.0f);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);

        // Multi-draw: static casters whose meshes are in the arena draw as one batch per light
        // (per cube face for point lights); anything else still draws one at a time.
        Shader* directionalCasterBatch = nullptr;
        Shader* cubeCasterBatch = nullptr;
        if (optPass && OptimizationState::prepareShadowBatch(*this, *optPass, sceneObjects)) {
            const auto usable = [&](Shader* program) {
                return (program && program != defaultShader && program->ID != 0) ? program : nullptr;
            };
            directionalCasterBatch = usable(getShader(kShadowDirectionalMultiDrawVertPath,
                                                      directionalShadowDepthFragPath));
            cubeCasterBatch = usable(getShader(kShadowCubeMultiDrawVertPath, shadowDepthFragPath));
        }
        const auto batchedCasterSlot = [&](uint32_t objectIndex, Shader* batchProgram) {
            return batchProgram ? optState->shadowBatch.objectSlot[objectIndex] : UINT32_MAX;
        };
        const auto queueBatchedCaster = [&](size_t& queued, uint32_t slot) {
            auto& sb = optState->shadowBatch;
            RenderOpt::DrawArraysIndirectCommand& command = sb.commands[queued++];
            command.count = static_cast<GLuint>(sb.slotCount[slot]);
            command.instanceCount = 1;
            command.first = static_cast<GLuint>(sb.slotFirst[slot]);
            command.baseInstance = slot;
        };
        const auto noteBatchedCasters = [&](size_t queued) {
            if (activeStats) {
                activeStats->multiDrawCalls += 1;
                activeStats->indirectDrawCommands += static_cast<int>(queued);
            }
        };

        int shadowSlot = 0;
        for (auto& light : lights) {
            light.shadowMapIndex = -1;
            light.shadowKind = 0;
            light.shadowMatrix = glm::mat4(1.0f);
            if (shadowSlot >= kMaxShadowMaps) continue;
            if (!light.castShadows || light.sourceId < 0) continue;

            if (light.type == 0) {
                if (!directionalShadowDepthShader || directionalShadowDepthShader->ID == 0) {
                    continue;
                }

                ShadowDirectionalMap& shadowMap = shadowDirectionalMaps[light.sourceId];
                if (shadowMap.fbo == 0 || shadowMap.depthTexture == 0 || shadowMap.resolution != light.shadowResolution) {
                    releaseDirectionalShadowMap(shadowMap);

                    glGenFramebuffers(1, &shadowMap.fbo);
                    glGenTextures(1, &shadowMap.depthTexture);
                    shadowMap.resolution = light.shadowResolution;

                    glBindTexture(GL_TEXTURE_2D, shadowMap.depthTexture);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT,
                                 shadowMap.resolution, shadowMap.resolution, 0,
                                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
#if MODULARITY_OPENGL_ES
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#else
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
                    const float borderColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
#endif

                    glBindFramebuffer(GL_FRAMEBUFFER, shadowMap.fbo);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowMap.depthTexture, 0);
                    SetDepthOnlyFramebufferDrawBuffer();
                    glReadBuffer(GL_NONE);
                    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                        std::cerr << "Failed to create directional shadow framebuffer for light " << light.sourceId << "\n";
                        releaseDirectionalShadowMap(shadowMap);
                        glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
                        continue;
                    }
                    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }

                if (shadowMap.fbo == 0 || shadowMap.depthTexture == 0) continue;

                activeDirectionalShadowIds.insert(light.sourceId);
                light.shadowMapIndex = shadowSlot;
                light.shadowKind = 2;
                light.shadowFar = glm::max(light.shadowFar, 1.0f);
                light.shadowMatrix = buildDirectionalShadowMatrix(light.dir);
                shadowDirectionalTextures[shadowSlot] = shadowMap.depthTexture;
                ++shadowSlot;

                directionalShadowDepthShader->use();
                directionalShadowDepthShader->setMat4("lightSpaceMatrix", light.shadowMatrix);

                glViewport(0, 0, shadowMap.resolution, shadowMap.resolution);
                glBindFramebuffer(GL_FRAMEBUFFER, shadowMap.fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowMap.depthTexture, 0);
                SetDepthOnlyFramebufferDrawBuffer();
                glReadBuffer(GL_NONE);
                glClear(GL_DEPTH_BUFFER_BIT);

                const FrustumPlanes shadowFrustum = BuildFrustumPlanes(light.shadowMatrix);
                if (optPass) {
                    // Cached matrices and bounds instead of rebuilding every caster's matrix
                    // per light. LOD skips casters under one shadow texel across.
                    OptimizationState::collectShadowCasters(*this, *optPass, sceneObjects);
                    const glm::vec3 lightRow(light.shadowMatrix[0][0], light.shadowMatrix[1][0],
                                             light.shadowMatrix[2][0]);
                    const float texelsPerUnit =
                        0.5f * static_cast<float>(shadowMap.resolution) * glm::length(lightRow);
                    size_t queued = 0;
                    for (const auto& caster : optPass->shadowCasters) {
                        if (sceneObjects[caster.first].id == light.sourceId) continue;
                        const RenderOpt::CullBounds& b = optState->bounds[caster.first];
                        if (b.hasBounds) {
                            if (optPass->plan.cpuCulling &&
                                (!RenderOpt::SphereInsidePlanes(shadowFrustum.planes, b.center, b.radius) ||
                                 !RenderOpt::AabbInsidePlanes(shadowFrustum.planes, b.center, b.extents))) {
                                continue;
                            }
                            if (optPass->plan.lod && 2.0f * b.radius * texelsPerUnit < 1.0f) {
                                continue;
                            }
                        }
                        const uint32_t batchSlot = batchedCasterSlot(caster.first, directionalCasterBatch);
                        if (batchSlot != UINT32_MAX) {
                            queueBatchedCaster(queued, batchSlot);
                            continue;
                        }
                        directionalShadowDepthShader->setMat4("model", optState->cache[caster.first].model);
                        caster.second->draw();
                    }
                    if (queued > 0) {
                        directionalCasterBatch->use();
                        directionalCasterBatch->setMat4("lightSpaceMatrix", light.shadowMatrix);
                        optState->drawShadowBatch(queued);
                        noteBatchedCasters(queued);
                    }
                    continue;
                }
                for (const auto& obj : sceneObjects) {
                    if (!IsObjectEnabledInHierarchy(obj) || !HasRendererComponent(obj)) continue;
                    if (obj.renderType == RenderType::Mirror) continue;
                    if (obj.renderType == RenderType::Sprite) continue;
                    if (obj.id == light.sourceId) continue;
                    bool isUiCanvas3D = obj.hasUI && obj.ui.type == UIElementType::Canvas && obj.ui.renderIn3D;
                    if (isUiCanvas3D) continue;
                    if (obj.hasSkeletalAnimation && obj.skeletal.enabled) continue;

                    Mesh* shadowMesh = selectMeshForObject(obj);
                    if (!shadowMesh) continue;
                    glm::mat4 model = buildModelMatrix(obj);
                    glm::vec3 boundsCenter(0.0f);
                    float boundsRadius = 0.0f;
                    if (TryComputeObjectCullSphere(obj, model, boundsCenter, boundsRadius) &&
                        !SphereInsideFrustum(shadowFrustum, boundsCenter, boundsRadius)) {
                        continue;
                    }
                    directionalShadowDepthShader->setMat4("model", model);
                    shadowMesh->draw();
                }
                continue;
            }

            ShadowCubeMap& shadowMap = shadowCubeMaps[light.sourceId];
            if (shadowMap.fbo == 0 || shadowMap.depthCube == 0 || shadowMap.resolution != light.shadowResolution) {
                releaseShadowCubeMap(shadowMap);

                glGenFramebuffers(1, &shadowMap.fbo);
                glGenTextures(1, &shadowMap.depthCube);
                shadowMap.resolution = light.shadowResolution;

                glBindTexture(GL_TEXTURE_CUBE_MAP, shadowMap.depthCube);
                for (int face = 0; face < 6; ++face) {
                    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_DEPTH_COMPONENT,
                                 shadowMap.resolution, shadowMap.resolution, 0,
                                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
                }
                glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

                glBindFramebuffer(GL_FRAMEBUFFER, shadowMap.fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_CUBE_MAP_POSITIVE_X, shadowMap.depthCube, 0);
                SetDepthOnlyFramebufferDrawBuffer();
                glReadBuffer(GL_NONE);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                    std::cerr << "Failed to create shadow cubemap framebuffer for light " << light.sourceId << "\n";
                    releaseShadowCubeMap(shadowMap);
                    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
                    continue;
                }
                glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
                glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
            }

            if (shadowMap.fbo == 0 || shadowMap.depthCube == 0) continue;

            activeCubeShadowIds.insert(light.sourceId);
            light.shadowFar = glm::max(light.shadowFar, 1.0f);
            light.shadowMapIndex = shadowSlot;
            light.shadowKind = 1;
            shadowTextures[shadowSlot] = shadowMap.depthCube;
            ++shadowSlot;

            const float shadowNearPlane = 0.1f;
            glm::mat4 shadowProj = glm::perspective(glm::radians(90.0f), 1.0f, shadowNearPlane, light.shadowFar);
            std::array<glm::mat4, 6> shadowViews = {
                glm::lookAt(light.pos, light.pos + glm::vec3(1.0f, 0.0f, 0.0f),  glm::vec3(0.0f, -1.0f, 0.0f)),
                glm::lookAt(light.pos, light.pos + glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
                glm::lookAt(light.pos, light.pos + glm::vec3(0.0f, 1.0f, 0.0f),  glm::vec3(0.0f, 0.0f, 1.0f)),
                glm::lookAt(light.pos, light.pos + glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
                glm::lookAt(light.pos, light.pos + glm::vec3(0.0f, 0.0f, 1.0f),  glm::vec3(0.0f, -1.0f, 0.0f)),
                glm::lookAt(light.pos, light.pos + glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f))
            };

            shadowDepthShader->use();
            shadowDepthShader->setVec3("lightPos", light.pos);
            shadowDepthShader->setFloat("farPlane", light.shadowFar);

            glViewport(0, 0, shadowMap.resolution, shadowMap.resolution);
            glBindFramebuffer(GL_FRAMEBUFFER, shadowMap.fbo);

            for (int face = 0; face < 6; ++face) {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                       GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, shadowMap.depthCube, 0);
                SetDepthOnlyFramebufferDrawBuffer();
                glReadBuffer(GL_NONE);
                glClear(GL_DEPTH_BUFFER_BIT);
                shadowDepthShader->setMat4("lightSpaceMatrix", shadowProj * shadowViews[face]);

                if (optPass) {
                    // The legacy loop only range-tests casters, so every caster in range is
                    // drawn into all six faces. CPU culling also tests each face's frustum.
                    OptimizationState::collectShadowCasters(*this, *optPass, sceneObjects);
                    FrustumPlanes faceFrustum{};
                    if (optPass->plan.cpuCulling) {
                        faceFrustum = BuildFrustumPlanes(shadowProj * shadowViews[face]);
                    }
                    // A 90 degree face: one texel spans 2 * distance / resolution.
                    const float texelsAtUnitDistance = 0.5f * static_cast<float>(shadowMap.resolution);
                    size_t queued = 0;
                    for (const auto& caster : optPass->shadowCasters) {
                        if (sceneObjects[caster.first].id == light.sourceId) continue;
                        const RenderOpt::CullBounds& b = optState->bounds[caster.first];
                        if (b.hasBounds) {
                            const glm::vec3 lightDelta = b.center - light.pos;
                            const float distanceSq = glm::dot(lightDelta, lightDelta);
                            const float maxDistance = light.shadowFar + b.radius;
                            if (distanceSq > maxDistance * maxDistance) continue;
                            if (optPass->plan.cpuCulling &&
                                (!RenderOpt::SphereInsidePlanes(faceFrustum.planes, b.center, b.radius) ||
                                 !RenderOpt::AabbInsidePlanes(faceFrustum.planes, b.center, b.extents))) {
                                continue;
                            }
                            if (optPass->plan.lod) {
                                const float distance = std::sqrt(distanceSq);
                                if (distance > b.radius &&
                                    2.0f * b.radius * texelsAtUnitDistance / distance < 1.0f) {
                                    continue;
                                }
                            }
                        }
                        const uint32_t batchSlot = batchedCasterSlot(caster.first, cubeCasterBatch);
                        if (batchSlot != UINT32_MAX) {
                            queueBatchedCaster(queued, batchSlot);
                            continue;
                        }
                        shadowDepthShader->setMat4("model", optState->cache[caster.first].model);
                        caster.second->draw();
                    }
                    if (queued > 0) {
                        cubeCasterBatch->use();
                        cubeCasterBatch->setMat4("lightSpaceMatrix", shadowProj * shadowViews[face]);
                        cubeCasterBatch->setVec3("lightPos", light.pos);
                        cubeCasterBatch->setFloat("farPlane", light.shadowFar);
                        optState->drawShadowBatch(queued);
                        noteBatchedCasters(queued);
                        shadowDepthShader->use();   // the next face sets its matrix on this
                    }
                    continue;
                }

                for (const auto& obj : sceneObjects) {
                    if (!IsObjectEnabledInHierarchy(obj) || !HasRendererComponent(obj)) continue;
                    if (obj.renderType == RenderType::Mirror) continue;
                    if (obj.renderType == RenderType::Sprite) continue;
                    if (obj.id == light.sourceId) continue;
                    bool isUiCanvas3D = obj.hasUI && obj.ui.type == UIElementType::Canvas && obj.ui.renderIn3D;
                    if (isUiCanvas3D) continue;
                    if (obj.hasSkeletalAnimation && obj.skeletal.enabled) continue;

                    Mesh* shadowMesh = selectMeshForObject(obj);
                    if (!shadowMesh) continue;
                    glm::mat4 model = buildModelMatrix(obj);
                    glm::vec3 boundsCenter(0.0f);
                    float boundsRadius = 0.0f;
                    if (TryComputeObjectCullSphere(obj, model, boundsCenter, boundsRadius)) {
                        glm::vec3 lightDelta = boundsCenter - light.pos;
                        float maxDistance = light.shadowFar + boundsRadius;
                        if (glm::dot(lightDelta, lightDelta) > maxDistance * maxDistance) {
                            continue;
                        }
                    }

                    shadowDepthShader->setMat4("model", model);
                    shadowMesh->draw();
                }
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        if (blendWasEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        if (!depthWasEnabled) glDisable(GL_DEPTH_TEST); else glEnable(GL_DEPTH_TEST);
        if (!polyOffsetWasEnabled) glDisable(GL_POLYGON_OFFSET_FILL);
        if (!cullWasEnabled) glDisable(GL_CULL_FACE);
        else {
            glEnable(GL_CULL_FACE);
            glCullFace(cullModeBefore);
        }
    }

    for (auto it = shadowCubeMaps.begin(); it != shadowCubeMaps.end(); ) {
        if (activeCubeShadowIds.find(it->first) == activeCubeShadowIds.end()) {
            releaseShadowCubeMap(it->second);
            it = shadowCubeMaps.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = shadowDirectionalMaps.begin(); it != shadowDirectionalMaps.end(); ) {
        if (activeDirectionalShadowIds.find(it->first) == activeDirectionalShadowIds.end()) {
            releaseDirectionalShadowMap(it->second);
            it = shadowDirectionalMaps.erase(it);
        } else {
            ++it;
        }
    }

    GLboolean cullFace = glIsEnabled(GL_CULL_FACE);
    GLint prevCullMode = GL_BACK;
    glGetIntegerv(GL_CULL_FACE_MODE, &prevCullMode);
    GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    GLboolean depthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    bool currentBlendEnabled = (blendEnabled == GL_TRUE);
    bool currentDepthMask = (depthMask == GL_TRUE);
    GLint prevFrontFace = GL_CCW;
    glGetIntegerv(GL_FRONT_FACE, &prevFrontFace);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    bool currentCullFaceEnabled = true;
    // Mirrored transforms (negative scale on an odd number of axes) reverse triangle
    bool currentFrontFaceCCW = true;

    for (int slot = 0; slot < kMaxShadowMaps; ++slot) {
        glActiveTexture(GL_TEXTURE3 + slot);
        glBindTexture(GL_TEXTURE_CUBE_MAP, shadowTextures[slot]);
    }
    for (int slot = 0; slot < kMaxShadowMaps; ++slot) {
        glActiveTexture(GL_TEXTURE7 + slot);
        glBindTexture(GL_TEXTURE_2D, shadowDirectionalTextures[slot]);
    }
    glActiveTexture(GL_TEXTURE0);

    bool skyboxPending = false;
    if (drawSkybox && camera.solidBackground) {
        glClearColor(camera.backgroundColor.r, camera.backgroundColor.g,
                     camera.backgroundColor.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    } else if (drawSkybox) {
        skyboxPending = true;
    }
    const std::string emptyPath;
    using RenderItem = SceneRenderItem;
    // The optimized path keeps its draw list between frames; the legacy list is rebuilt
    // (and reallocated) every pass, exactly as it always was.
    std::vector<RenderItem> legacyDrawItems;
    std::vector<RenderItem>& drawItems = optPass ? optPass->items : legacyDrawItems;
    if (optPass) {
        OptimizationState::buildQueue(*this, *optPass, sceneObjects, optPassStats);
    } else {
        MODU_PROFILE_SCOPE("Renderer::BuildRenderQueue", ProfilerSampleCategory::RenderDetail);
        rebuildStaticMergeBatches(sceneObjects);

        auto combineHash = [](uint64_t seed, uint64_t value) {
            seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
            return seed;
        };
        auto buildOpaqueSortKey = [&](const std::string& vert,
                                      const std::string& frag,
                                      const std::string& material,
                                      const std::string& albedo,
                                      const std::string& overlay,
                                      const std::string& normal) {
            uint64_t key = 1469598103934665603ull;
            key = combineHash(key, HashStringFNV1a(vert));
            key = combineHash(key, HashStringFNV1a(frag));
            key = combineHash(key, HashStringFNV1a(material));
            key = combineHash(key, HashStringFNV1a(albedo));
            key = combineHash(key, HashStringFNV1a(overlay));
            key = combineHash(key, HashStringFNV1a(normal));
            return key;
        };

        drawItems.reserve(sceneObjects.size() + staticMergeBatches.size());

        const bool cullingMaskAllowsAll = (camera.cullingMask == 0xFFFFFFFFu);

        auto gatherRenderItemsRange = [&](size_t beginIndex, size_t endIndex) {
            std::vector<RenderItem> localItems;
            localItems.reserve(endIndex > beginIndex ? (endIndex - beginIndex) : 0);

            for (size_t objIndex = beginIndex; objIndex < endIndex; ++objIndex) {
                const auto& obj = sceneObjects[objIndex];
                if (!IsObjectEnabledInHierarchy(obj)) continue;
                if (!cullingMaskAllowsAll &&
                    (camera.cullingMask & (1u << (static_cast<uint32_t>(obj.layer) & 31u))) == 0u) continue;
                if (!drawMirrorObjects && obj.hasRenderer && obj.renderType == RenderType::Mirror) continue;
                if (!HasRendererComponent(obj)) continue;
                if (cullingMaskAllowsAll &&
                    staticMergeSourceIds.find(obj.id) != staticMergeSourceIds.end()) continue;

                glm::mat4 model = buildModelMatrix(obj);
                glm::vec3 boundsCenter(0.0f);
                float boundsRadius = 0.0f;
                const bool hasBounds = TryComputeObjectCullSphere(obj, model, boundsCenter, boundsRadius);
                if (hasBounds && !SphereInsideFrustum(frustum, boundsCenter, boundsRadius)) {
                    continue;
                }

                Mesh* mesh = selectMeshForObject(obj);
                if (!mesh) continue;

                RenderItem item;
                item.obj = &obj;
                item.mesh = mesh;
                item.model = model;
                item.sortCenter = obj.position;
                item.depthSortCenter = hasBounds ? boundsCenter : obj.position;
                item.vertPath = &obj.vertexShaderPath;
                item.fragPath = &obj.fragmentShaderPath;
                item.material = obj.material;
                item.materialPath = &obj.materialPath;
                item.albedoTexturePath = &obj.albedoTexturePath;
                item.overlayTexturePath = &obj.overlayTexturePath;
                item.normalMapPath = &obj.normalMapPath;
                item.metallicRoughnessMapPath = &obj.metallicRoughnessMapPath;
                item.metallicMapPath = &obj.metallicMapPath;
                item.roughnessMapPath = &obj.roughnessMapPath;
                item.opacityMapPath = &obj.opacityMapPath;
                item.emissiveMapPath = &obj.emissiveMapPath;
                item.occlusionMapPath = &obj.occlusionMapPath;
                item.heightMapPath = &obj.heightMapPath;
                item.useOverlay = obj.useOverlay;
                item.boneLimit = obj.skeletal.maxBones;
                item.availableBones = static_cast<int>(obj.skeletal.finalMatrices.size());
                bool needsFallback = obj.hasSkeletalAnimation && obj.skeletal.enabled &&
                                     obj.skeletal.allowCpuFallback &&
                                     item.boneLimit > 0 && item.availableBones > item.boneLimit;
                item.wantsGpuSkinning = obj.hasSkeletalAnimation && obj.skeletal.enabled &&
                                        obj.skeletal.useGpuSkinning && !needsFallback;
                if (item.wantsGpuSkinning && item.vertPath->empty()) {
                    item.vertPath = &skinnedVertPath;
                }
                item.isUiCanvas3D = obj.hasUI && obj.ui.type == UIElementType::Canvas && obj.ui.renderIn3D;
                item.uiSortingOrder = item.isUiCanvas3D ? obj.ui.sortingOrder : 0;
                item.requiresBlending = item.material.alpha < 0.999f || item.isUiCanvas3D;
                item.unlit = obj.renderType == RenderType::Mirror || obj.renderType == RenderType::Sprite || item.isUiCanvas3D;
                item.doubleSided =
                    obj.renderType == RenderType::Sprite ||
                    obj.renderType == RenderType::Mirror;
                glm::vec3 viewSpaceCenter = glm::vec3(view * glm::vec4(item.depthSortCenter, 1.0f));
                item.cameraDepth = -viewSpaceCenter.z;
                glm::vec3 toCamera = item.depthSortCenter - camera.position;
                item.cameraDistanceSq = glm::dot(toCamera, toCamera);
                localItems.push_back(std::move(item));
            }

            return localItems;
        };

        const unsigned int hardwareThreads = std::thread::hardware_concurrency();
        const size_t minItemsPerTask = 2048;
        const size_t maxTaskCountByWork = std::max<size_t>(1, sceneObjects.size() / minItemsPerTask);
        const size_t taskCount = (hardwareThreads > 4 && sceneObjects.size() >= minItemsPerTask * 2)
            ? std::min<size_t>(hardwareThreads, maxTaskCountByWork)
            : 1;

        if (taskCount <= 1) {
            drawItems = gatherRenderItemsRange(0, sceneObjects.size());
        } else {
            std::vector<std::future<std::vector<RenderItem>>> gatherFutures;
            gatherFutures.reserve(taskCount);
            const size_t chunkSize = (sceneObjects.size() + taskCount - 1) / taskCount;

            for (size_t taskIndex = 0; taskIndex < taskCount; ++taskIndex) {
                const size_t beginIndex = taskIndex * chunkSize;
                if (beginIndex >= sceneObjects.size()) {
                    break;
                }
                const size_t endIndex = std::min(sceneObjects.size(), beginIndex + chunkSize);
                gatherFutures.push_back(std::async(std::launch::async,
                    [&, beginIndex, endIndex]() {
                        return gatherRenderItemsRange(beginIndex, endIndex);
                    }));
            }

            for (auto& future : gatherFutures) {
                std::vector<RenderItem> chunkItems = future.get();
                drawItems.insert(drawItems.end(),
                                 std::make_move_iterator(chunkItems.begin()),
                                 std::make_move_iterator(chunkItems.end()));
            }
        }

        for (const auto& batch : staticMergeBatches) {
            if (!cullingMaskAllowsAll) break;
            if (!batch.mesh) continue;
            if (batch.boundsRadius > 0.0f &&
                !SphereInsideFrustum(frustum, batch.boundsCenter, batch.boundsRadius)) {
                continue;
            }

            RenderItem item;
            item.staticBatch = &batch;
            item.mesh = batch.mesh.get();
            item.model = glm::mat4(1.0f);
            item.sortCenter = batch.boundsCenter;
            item.depthSortCenter = batch.boundsCenter;
            item.vertPath = &batch.vertPath;
            item.fragPath = &batch.fragPath;
            item.material = batch.material;
            item.materialPath = &batch.materialPath;
            item.albedoTexturePath = &batch.albedoTexturePath;
            item.overlayTexturePath = &batch.overlayTexturePath;
            item.normalMapPath = &batch.normalMapPath;
            item.metallicRoughnessMapPath = &batch.metallicRoughnessMapPath;
            item.metallicMapPath = &batch.metallicMapPath;
            item.roughnessMapPath = &batch.roughnessMapPath;
            item.opacityMapPath = &batch.opacityMapPath;
            item.emissiveMapPath = &batch.emissiveMapPath;
            item.occlusionMapPath = &batch.occlusionMapPath;
            item.heightMapPath = &batch.heightMapPath;
            item.useOverlay = batch.useOverlay;
            item.requiresBlending = item.material.alpha < 0.999f;
            item.unlit = batch.unlit;
            item.doubleSided = batch.doubleSided;
            glm::vec3 viewSpaceCenter = glm::vec3(view * glm::vec4(item.depthSortCenter, 1.0f));
            item.cameraDepth = -viewSpaceCenter.z;
            glm::vec3 toCamera = item.depthSortCenter - camera.position;
            item.cameraDistanceSq = glm::dot(toCamera, toCamera);
            drawItems.push_back(std::move(item));
        }

        auto classifyAlphaBehavior = [&](RenderItem& item) {
            if (item.requiresBlending) {
                item.sortOpaque = false;
                return;
            }

            if (item.albedoTexturePath && !item.albedoTexturePath->empty()) {
                if (Texture* tex = getTexture(*item.albedoTexturePath, item.material.textureFilter)) {
                    if (tex->UsesAlphaBlending()) {
                        item.requiresBlending = true;
                    }
                }
            }

            item.sortOpaque = !item.requiresBlending;
            if (item.sortOpaque) {
                item.opaqueSortKey = buildOpaqueSortKey(
                    item.vertPath ? *item.vertPath : emptyPath,
                    item.fragPath ? *item.fragPath : emptyPath,
                    item.materialPath ? *item.materialPath : emptyPath,
                    item.albedoTexturePath ? *item.albedoTexturePath : emptyPath,
                    item.overlayTexturePath ? *item.overlayTexturePath : emptyPath,
                    item.normalMapPath ? *item.normalMapPath : emptyPath);
            }
        };

        for (RenderItem& item : drawItems) {
            classifyAlphaBehavior(item);
        }

        {
            MODU_PROFILE_SCOPE("Renderer::MaterialSorting", ProfilerSampleCategory::RenderDetail);
            if (drawItems.size() > 1) {
                std::stable_sort(drawItems.begin(), drawItems.end(),
                                 [](const RenderItem& a, const RenderItem& b) {
                                     if (a.sortOpaque != b.sortOpaque) return a.sortOpaque > b.sortOpaque;
                                     if (!a.sortOpaque) {
                                         if (a.cameraDepth != b.cameraDepth) {
                                             return a.cameraDepth > b.cameraDepth;
                                         }
                                         if (a.cameraDistanceSq != b.cameraDistanceSq) {
                                             return a.cameraDistanceSq > b.cameraDistanceSq;
                                         }
                                         int aId = a.obj ? a.obj->id : -1;
                                         int bId = b.obj ? b.obj->id : -1;
                                         return aId < bId;
                                     }
                                     if (a.opaqueSortKey != b.opaqueSortKey) return a.opaqueSortKey < b.opaqueSortKey;
                                     int aId = a.obj ? a.obj->id : -1;
                                     int bId = b.obj ? b.obj->id : -1;
                                     return aId < bId;
                                 });

                std::vector<size_t> canvasSlots;
                for (size_t i = 0; i < drawItems.size(); ++i) {
                    if (!drawItems[i].sortOpaque && drawItems[i].isUiCanvas3D) {
                        canvasSlots.push_back(i);
                    }
                }
                if (canvasSlots.size() > 1) {
                    std::vector<RenderItem> canvasItems;
                    canvasItems.reserve(canvasSlots.size());
                    for (size_t slot : canvasSlots) {
                        canvasItems.push_back(drawItems[slot]);
                    }
                    std::stable_sort(canvasItems.begin(), canvasItems.end(),
                                     [](const RenderItem& a, const RenderItem& b) {
                                         return a.uiSortingOrder < b.uiSortingOrder;
                                     });
                    for (size_t i = 0; i < canvasSlots.size(); ++i) {
                        drawItems[canvasSlots[i]] = canvasItems[i];
                    }
                }
            }
        }
        if (activeStats) {
            activeStats->visibleRenderables += static_cast<int>(drawItems.size());
        }
    }

    GLint currentActiveTexture = GL_TEXTURE0;
    std::array<GLuint, 16> boundTexture2D;
    boundTexture2D.fill(std::numeric_limits<GLuint>::max());
    auto bindTexture2D = [&](GLenum unit, GLuint textureId) {
        if (currentActiveTexture != static_cast<GLint>(unit)) {
            glActiveTexture(unit);
            currentActiveTexture = static_cast<GLint>(unit);
            Runtime2DCountStateBind();
        }
        int slot = static_cast<int>(unit - GL_TEXTURE0);
        if (slot >= 0 && slot < static_cast<int>(boundTexture2D.size()) &&
            boundTexture2D[slot] == textureId) {
            return;
        }
        glBindTexture(GL_TEXTURE_2D, textureId);
        Runtime2DCountTextureBind();
        if (slot >= 0 && slot < static_cast<int>(boundTexture2D.size())) {
            boundTexture2D[slot] = textureId;
        }
    };

    auto flushPendingSkybox = [&]() {
        if (!skyboxPending) return;
        skyboxPending = false;
        if (camera.orthographic) {
            const float aspect = static_cast<float>(std::max(1, width)) /
                                 static_cast<float>(std::max(1, height));
            renderSkybox(view, glm::perspective(glm::radians(glm::clamp(fovDeg, 1.0f, 179.0f)),
                                                aspect, nearPlane, farPlane));
        } else {
            renderSkybox(view, proj);
        }
        boundTexture2D[0] = 0;
        boundTexture2D[1] = 0;
        boundTexture2D[2] = 0;
        currentActiveTexture = GL_TEXTURE0;
    };

    struct LightUniformNameCache {
        std::array<std::string, kMaxLights> type;
        std::array<std::string, kMaxLights> dir;
        std::array<std::string, kMaxLights> pos;
        std::array<std::string, kMaxLights> color;
        std::array<std::string, kMaxLights> intensity;
        std::array<std::string, kMaxLights> range;
        std::array<std::string, kMaxLights> innerCos;
        std::array<std::string, kMaxLights> outerCos;
        std::array<std::string, kMaxLights> areaSize;
        std::array<std::string, kMaxLights> areaFade;
        std::array<std::string, kMaxLights> shadowMap;
        std::array<std::string, kMaxLights> shadowKind;
        std::array<std::string, kMaxLights> shadowMode;
        std::array<std::string, kMaxLights> shadowBias;
        std::array<std::string, kMaxLights> shadowSoftness;
        std::array<std::string, kMaxLights> shadowFar;
        std::array<std::string, kMaxLights> shadowMatrix;
    };
    static const LightUniformNameCache kLightNames = []() {
        LightUniformNameCache names;
        for (size_t i = 0; i < kMaxLights; ++i) {
            const std::string idx = "[" + std::to_string(i) + "]";
            names.type[i] = "lightTypeArr" + idx;
            names.dir[i] = "lightDirArr" + idx;
            names.pos[i] = "lightPosArr" + idx;
            names.color[i] = "lightColorArr" + idx;
            names.intensity[i] = "lightIntensityArr" + idx;
            names.range[i] = "lightRangeArr" + idx;
            names.innerCos[i] = "lightInnerCosArr" + idx;
            names.outerCos[i] = "lightOuterCosArr" + idx;
            names.areaSize[i] = "lightAreaSizeArr" + idx;
            names.areaFade[i] = "lightAreaFadeArr" + idx;
            names.shadowMap[i] = "lightShadowMapArr" + idx;
            names.shadowKind[i] = "lightShadowKindArr" + idx;
            names.shadowMode[i] = "lightShadowModeArr" + idx;
            names.shadowBias[i] = "lightShadowBiasArr" + idx;
            names.shadowSoftness[i] = "lightShadowSoftnessArr" + idx;
            names.shadowFar[i] = "lightShadowFarArr" + idx;
            names.shadowMatrix[i] = "lightShadowMatrixArr" + idx;
        }
        return names;
    }();

    Shader* currentShader = nullptr;
    auto findReflectionCastForItem = [&](const RenderItem& item, float& outIntensity) -> unsigned int {
        outIntensity = 0.0f;
        if (reflectionCastTargets.empty()) return 0;
        float bestDistSq = std::numeric_limits<float>::max();
        unsigned int bestCube = 0;
        float bestIntensity = 0.0f;
        for (const SceneObject& probeObj : sceneObjects) {
            if (!IsObjectEnabledInHierarchy(probeObj) || !probeObj.hasReflectionCast || !probeObj.reflectionCast.enabled) continue;
            auto targetIt = reflectionCastTargets.find(probeObj.id);
            if (targetIt == reflectionCastTargets.end() || targetIt->second.cube == 0 || !targetIt->second.hasCapture) continue;
            const glm::vec3 delta = item.sortCenter - probeObj.position;
            const float distSq = glm::dot(delta, delta);
            const float range = std::max(0.01f, probeObj.reflectionCast.blendDistance);
            if (distSq > range * range || distSq >= bestDistSq) continue;
            bestDistSq = distSq;
            bestCube = targetIt->second.cube;
            const float distance = std::sqrt(std::max(0.0f, distSq));
            bestIntensity = probeObj.reflectionCast.intensity * glm::clamp(1.0f - distance / range, 0.0f, 1.0f);
        }
        outIntensity = bestIntensity;
        return bestCube;
    };

    bool giActiveThisPass = false;
#if !MODULARITY_OPENGL_ES
    if (skybox && !reflectionCapturePass && giSupported()) {
        const SkyboxSettings& giSky = skybox->getSettings();
        if (giSky.giEnabled && giSky.giIntensity > 0.0f) {
            uint64_t giSignature = 1469598103934665603ull;
            glm::vec3 sceneMin(FLT_MAX);
            glm::vec3 sceneMax(-FLT_MAX);
            bool anyBounds = false;
            for (const auto& obj : sceneObjects) {
                if (!IsObjectEnabledInHierarchy(obj)) continue;
                if (obj.hasRenderer && obj.renderType != RenderType::None) {
                    const glm::vec3 reach = glm::abs(obj.scale) * 0.5f + glm::vec3(0.05f);
                    sceneMin = glm::min(sceneMin, obj.position - reach);
                    sceneMax = glm::max(sceneMax, obj.position + reach);
                    anyBounds = true;
                    giSignature = HashCombine64(giSignature, static_cast<uint64_t>(obj.id));
                    giSignature = HashCombine64(giSignature, HashFloat64(obj.position.x));
                    giSignature = HashCombine64(giSignature, HashFloat64(obj.position.y));
                    giSignature = HashCombine64(giSignature, HashFloat64(obj.position.z));
                    giSignature = HashCombine64(giSignature, HashFloat64(obj.rotation.x));
                    giSignature = HashCombine64(giSignature, HashFloat64(obj.rotation.y));
                    giSignature = HashCombine64(giSignature, HashFloat64(obj.rotation.z));
                    giSignature = HashCombine64(giSignature, HashFloat64(obj.scale.x));
                    giSignature = HashCombine64(giSignature, HashFloat64(obj.scale.y));
                    giSignature = HashCombine64(giSignature, HashFloat64(obj.scale.z));
                    giSignature = HashCombine64(giSignature, HashFloat64(obj.material.color.r));
                    giSignature = HashCombine64(giSignature, HashFloat64(obj.material.color.g));
                    giSignature = HashCombine64(giSignature, HashFloat64(obj.material.color.b));
                    giSignature = HashCombine64(giSignature, HashFloat64(obj.material.emissionStrength));
                    giSignature = HashCombine64(giSignature, HashString64(obj.albedoTexturePath));
                }
            }
            for (const auto& l : lights) {
                giSignature = HashCombine64(giSignature, static_cast<uint64_t>(l.type));
                giSignature = HashCombine64(giSignature, HashFloat64(l.pos.x));
                giSignature = HashCombine64(giSignature, HashFloat64(l.pos.y));
                giSignature = HashCombine64(giSignature, HashFloat64(l.pos.z));
                giSignature = HashCombine64(giSignature, HashFloat64(l.dir.x));
                giSignature = HashCombine64(giSignature, HashFloat64(l.dir.y));
                giSignature = HashCombine64(giSignature, HashFloat64(l.dir.z));
                giSignature = HashCombine64(giSignature, HashFloat64(l.intensity));
                giSignature = HashCombine64(giSignature, HashFloat64(l.color.r));
                giSignature = HashCombine64(giSignature, HashFloat64(l.color.g));
                giSignature = HashCombine64(giSignature, HashFloat64(l.color.b));
            }
            giSignature = HashCombine64(giSignature, static_cast<uint64_t>(giSky.giResolution));
            giSignature = HashCombine64(giSignature, static_cast<uint64_t>(giSky.giBounces));
            giSignature = HashCombine64(giSignature, HashFloat64(giSky.giMaxExtent));

            const int giBounces = std::clamp(giSky.giBounces, 1, 4);
            if (anyBounds && ensureVoxelVolume(giSky.giResolution, giBounces > 1 ? 2 : 1)) {
                const glm::vec3 center = (sceneMin + sceneMax) * 0.5f;
                const glm::vec3 size = sceneMax - sceneMin;
                const float extent = std::min(std::max(std::max(size.x, std::max(size.y, size.z)),
                                                       1.0f),
                                              std::max(1.0f, giSky.giMaxExtent));
                const bool boundsMoved =
                    std::abs(extent - voxelVolume.extent) > 0.001f ||
                    glm::length(voxelVolume.boundsMin - (center - glm::vec3(extent * 0.5f))) > 0.001f;

                if (giSignature != voxelSceneSignature || boundsMoved || !voxelVolume.hasData) {
                    Shader* voxelize = getShader(voxelizeVertPath, voxelizeGeomPath, voxelizeFragPath);
                    if (voxelize && voxelize->ID != 0 && voxelize != defaultShader) {
                        MODU_PROFILE_SCOPE("Voxelize GI", ProfilerSampleCategory::RenderDetail);
                        voxelizeShader = voxelize;
                        voxelVolume.extent = extent;
                        voxelVolume.boundsMin = center - glm::vec3(extent * 0.5f);

                        GLint previousFramebuffer = 0;
                        GLint previousViewport[4] = {0, 0, 0, 0};
                        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFramebuffer);
                        glGetIntegerv(GL_VIEWPORT, previousViewport);

                        const int res = voxelVolume.resolution;
                        std::vector<GLfloat> zeros;
                        auto clearVolume = [&](unsigned int tex) {
                            if (GLAD_GL_VERSION_4_4) {
                                const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                                glClearTexImage(tex, 0, GL_RGBA, GL_FLOAT, zero);
                                return;
                            }
                            if (zeros.empty()) {
                                zeros.assign(static_cast<size_t>(res) * res * res * 4, 0.0f);
                            }
                            glBindTexture(GL_TEXTURE_3D, tex);
                            glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, res, res, res,
                                            GL_RGBA, GL_FLOAT, zeros.data());
                        };

                        glBindFramebuffer(GL_FRAMEBUFFER, voxelVolume.fbo);
                        glViewport(0, 0, res, res);
                        glDisable(GL_DEPTH_TEST);
                        glDepthMask(GL_FALSE);
                        glDisable(GL_CULL_FACE);
                        glDisable(GL_BLEND);
                        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

                        voxelize->use();
                        voxelize->setVec3("volumeMin", voxelVolume.boundsMin);
                        voxelize->setFloat("volumeExtent", voxelVolume.extent);
                        voxelize->setInt("volumeResolution", res);
                        voxelize->setVec3("ambientColor", ambientColor);
                        voxelize->setInt("albedoTex", 0);
                        voxelize->setInt("previousVolume", 1);
                        voxelize->setInt("giConeSteps", std::clamp(giSky.giConeSteps, 4, 64));
                        voxelize->setInt("shadowCube0", 3);
                        voxelize->setInt("shadowCube1", 4);
                        voxelize->setInt("shadowCube2", 5);
                        voxelize->setInt("shadowCube3", 6);
                        voxelize->setInt("dirShadow0", 7);
                        voxelize->setInt("dirShadow1", 8);
                        voxelize->setInt("dirShadow2", 9);
                        voxelize->setInt("dirShadow3", 10);
                        voxelize->setInt("lightCount", static_cast<int>(lights.size()));
                        for (size_t i = 0; i < lights.size() && i < kMaxLights; ++i) {
                            const auto& l = lights[i];
                            voxelize->setInt(kLightNames.type[i], l.type);
                            voxelize->setVec3(kLightNames.dir[i], l.dir);
                            voxelize->setVec3(kLightNames.pos[i], l.pos);
                            voxelize->setVec3(kLightNames.color[i], l.color);
                            voxelize->setFloat(kLightNames.intensity[i], l.intensity);
                            voxelize->setFloat(kLightNames.range[i], l.range);
                            voxelize->setFloat(kLightNames.innerCos[i], l.inner);
                            voxelize->setFloat(kLightNames.outerCos[i], l.outer);
                            voxelize->setInt(kLightNames.shadowMap[i], l.shadowMapIndex);
                            voxelize->setInt(kLightNames.shadowKind[i], l.shadowKind);
                            voxelize->setFloat(kLightNames.shadowBias[i], l.shadowBias);
                            voxelize->setFloat(kLightNames.shadowFar[i], l.shadowFar);
                            voxelize->setMat4(kLightNames.shadowMatrix[i], l.shadowMatrix);
                        }

                        for (int bounce = 0; bounce < giBounces; ++bounce) {
                        const int writeIndex = (voxelVolume.allocated > 1) ? (bounce & 1) : 0;
                        const int readIndex = 1 - writeIndex;
                        const bool gatherPrevious =
                            bounce > 0 && voxelVolume.allocated > 1 &&
                            voxelVolume.texture[readIndex] != 0;

                        clearVolume(voxelVolume.texture[writeIndex]);
                        glBindImageTexture(0, voxelVolume.texture[writeIndex], 0, GL_TRUE, 0,
                                           GL_WRITE_ONLY, GL_RGBA16F);
                        voxelize->setBool("hasPreviousBounce", gatherPrevious);
                        glActiveTexture(GL_TEXTURE1);
                        glBindTexture(GL_TEXTURE_3D,
                                      gatherPrevious ? voxelVolume.texture[readIndex] : 0);
                        glActiveTexture(GL_TEXTURE0);

                        const auto voxelizeDraw = [&](const SceneObject* objPtr,
                                                      const glm::mat4& model,
                                                      const MaterialProperties& material,
                                                      const std::string& aPath, Mesh* mesh) {
                            voxelize->setMat4("model", model);
                            voxelize->setVec4("uvRect",
                                              objPtr ? BuildSpriteUvRect(*objPtr)
                                                     : glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
                            voxelize->setVec4("uvTransform",
                                              BuildSurfaceUvTransform(objPtr, material));
                            voxelize->setVec3("materialColor", material.color);
                            voxelize->setVec3("emissionColor", material.emissionColor);
                            voxelize->setFloat("emissionStrength", material.emissionStrength);
                            voxelize->setFloat("ambientStrength", material.ambientStrength);

                            bool albedoUsed = false;
                            if (!aPath.empty()) {
                                if (auto* t = getTexture(aPath, material.textureFilter)) {
                                    glActiveTexture(GL_TEXTURE0);
                                    glBindTexture(GL_TEXTURE_2D, t->GetID());
                                    albedoUsed = true;
                                }
                            }
                            voxelize->setBool("hasAlbedoTex", albedoUsed);

                            recordMeshDraw();
                            mesh->draw();
                        };
                        for (const RenderItem& item : drawItems) {
                            if (item.multiDrawBatch >= 0 && optState) {
                                // The voxelizer has its own vertex stage, so batched objects
                                // voxelize one at a time, limited to the view frustum the
                                // way the per-object items are.
                                const auto& md = optState->multiDraw;
                                const auto& batch = md.batches[static_cast<size_t>(item.multiDrawBatch)];
                                for (uint32_t slot = batch.firstSlot;
                                     slot < batch.firstSlot + batch.slotCount; ++slot) {
                                    const uint32_t objectIndex = md.slotObject[slot];
                                    const SceneObject& member = sceneObjects[objectIndex];
                                    const RenderOpt::CullBounds& b = optState->bounds[objectIndex];
                                    if (b.hasBounds &&
                                        !RenderOpt::SphereInsidePlanes(frustum.planes, b.center, b.radius)) {
                                        continue;
                                    }
                                    if (Mesh* mesh = OptimizationState::meshFor(*this, member)) {
                                        voxelizeDraw(&member, optState->cache[objectIndex].model,
                                                     member.material, member.albedoTexturePath, mesh);
                                    }
                                }
                                continue;
                            }
                            if (item.mesh == nullptr || !item.sortOpaque || item.isUiCanvas3D) {
                                continue;
                            }
                            voxelizeDraw(item.obj, item.model, item.material,
                                         item.albedoTexturePath ? *item.albedoTexturePath : emptyPath,
                                         item.mesh);
                        }

                        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                                        GL_TEXTURE_FETCH_BARRIER_BIT);
                        glBindTexture(GL_TEXTURE_3D, voxelVolume.texture[writeIndex]);
                        glGenerateMipmap(GL_TEXTURE_3D);
                        glBindTexture(GL_TEXTURE_3D, 0);
                        voxelVolume.resultIndex = writeIndex;
                        }

                        glActiveTexture(GL_TEXTURE1);
                        glBindTexture(GL_TEXTURE_3D, 0);
                        glActiveTexture(GL_TEXTURE0);

                        voxelVolume.hasData = true;
                        voxelSceneSignature = giSignature;

                        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
                        glViewport(previousViewport[0], previousViewport[1],
                                   previousViewport[2], previousViewport[3]);
                        glEnable(GL_DEPTH_TEST);
                        glDepthMask(GL_TRUE);
                        glEnable(GL_CULL_FACE);
                        glCullFace(GL_BACK);
                        glActiveTexture(GL_TEXTURE0);
                    }
                }

                if (voxelVolume.hasData && voxelVolume.texture[voxelVolume.resultIndex] != 0) {
                    glActiveTexture(GL_TEXTURE16);
                    glBindTexture(GL_TEXTURE_3D, voxelVolume.texture[voxelVolume.resultIndex]);
                    glActiveTexture(GL_TEXTURE0);
                    giActiveThisPass = true;
                }
            }
        }
    }

#endif
    struct MaterialUniformCache {
        bool valid = false;
        bool unlit = false;
        glm::vec3 materialColor{0.0f};
        float materialAlpha = 0.0f;
        float ambientStrength = 0.0f;
        float specularStrength = 0.0f;
        float shininess = 0.0f;
        float normalMapIntensity = 0.0f;
        float mixAmount = 0.0f;
        float scrollSpeed = -1.0f;
        glm::vec2 scrollDirection{0.0f};
        glm::vec3 cloudColor{-1.0f};
        glm::vec3 cloudSkyColor{-1.0f};
        float cloudScale = -1.0f;
        float cloudCoverage = -1.0f;
        float cloudSoftness = -1.0f;
        int cloudDetail = -1;
        float cloudSpeed = -1.0f;
        float cloudWarp = -1.0f;
        float cloudHighlight = -1.0f;
        float cloudStars = -1.0f;
        float cloudHorizon = -1.0f;
        glm::vec4 uvTransform{0.0f};
        glm::vec4 uvRect{0.0f};
        bool useSkinning = false;
        int boneCount = -1;
        bool hasOverlay = false;
        bool hasNormalMap = false;
        bool usePbr = false;
        float pbrMetallic = -1.0f;
        float pbrRoughness = -1.0f;
        float occlusionStrength = -1.0f;
        glm::vec3 emissionColor{-1.0f};
        float emissionStrength = -1.0f;
        bool hasMetallicRoughnessMap = false;
        bool hasMetallicMap = false;
        bool hasRoughnessMap = false;
        bool hasOpacityMap = false;
        float alphaCutoff = -1.0f;
        bool hasEmissiveMap = false;
        bool hasOcclusionMap = false;
        bool hasHeightMap = false;
        float heightScale = -1.0f;
        int parallaxSteps = -1;
        bool parallaxInvertHeight = false;
        bool hasReflectionCast = false;
        float reflectionIntensity = 0.0f;
        float reflectionFadeStart = -1.0f;
        float reflectionFadeEnd = -1.0f;
        bool fogEnabled = false;
        int fogMode = -1;
        glm::vec3 fogColor{0.0f};
        float fogStart = -1.0f;
        float fogEnd = -1.0f;
        float fogDensity = -1.0f;
        float fogHeight = 0.0f;
        float fogHeightFalloff = -1.0f;
    } matCache;
    auto invalidateMatCache = [&]() { matCache = MaterialUniformCache{}; };
    // A texture-transparent pixel at least this opaque goes into the depth buffer before
    // its item's colour is blended (see the depth prepass in the loop). Half coverage is
    // the cut that keeps the worst case smallest: hiding what is behind a pixel errs by
    // (1 - alpha), drawing it on top instead errs by alpha.
    constexpr float kTransparentDepthCutoff = 0.5f;
    // The prepass leans on the shader discarding below alphaCutoff; a shader that
    // ignores it would write depth for its whole quad, invisible parts included.
    // Asked once per shader per pass: glGetUniformLocation is a driver round-trip.
    std::array<std::pair<const Shader*, bool>, 8> cutoffShaders{};
    size_t cutoffShaderCount = 0;
    auto shaderHonorsAlphaCutoff = [&](const Shader* s) {
        for (size_t k = 0; k < cutoffShaderCount; ++k) {
            if (cutoffShaders[k].first == s) return cutoffShaders[k].second;
        }
        const bool honors = glGetUniformLocation(s->ID, "alphaCutoff") >= 0;
        if (cutoffShaderCount < cutoffShaders.size()) {
            cutoffShaders[cutoffShaderCount++] = {s, honors};
        }
        return honors;
    };
    // The legacy loop copies the sky settings (two heap strings) for every draw; the
    // optimized path reads them in place. Same values either way.
    static const SkyboxSettings kNoSkySettings{};
    const SkyboxSettings* passSkySettings =
        optPass ? (skybox ? &skybox->getSettings() : &kNoSkySettings) : nullptr;
    // Closed right after the loop rather than at the end of the function.
    std::optional<ProfilerScope> submissionScope;
    submissionScope.emplace("Renderer::Submission", ProfilerSampleCategory::RenderDetail);
    for (const RenderItem& item : drawItems) {
        if (skyboxPending && !item.sortOpaque) {
            flushPendingSkybox();
        }
        const SceneObject* objPtr = item.obj;
        const std::string& vertPath = item.vertPath ? *item.vertPath : emptyPath;
        const std::string& fragPath = item.fragPath ? *item.fragPath : emptyPath;
        const std::string& materialPath = item.materialPath ? *item.materialPath : emptyPath;
        const std::string& albedoTexturePath = item.albedoTexturePath ? *item.albedoTexturePath : emptyPath;
        const std::string& overlayTexturePath = item.overlayTexturePath ? *item.overlayTexturePath : emptyPath;
        const std::string& normalMapPath = item.normalMapPath ? *item.normalMapPath : emptyPath;

        Shader* active = item.shader ? item.shader : getShader(vertPath, fragPath);
        if (!active) continue;
        shader = active;
        if (currentShader != shader) {
            currentShader = shader;
            shader->use();
            invalidateMatCache();
            Runtime2DCountStateBind();
            shader->setMat4("view", view);
            shader->setMat4("projection", proj);
            shader->setVec3("viewPos", camera.position);
            shader->setFloat("uTime", timeSeconds);
            shader->setVec3("ambientColor", ambientColor);
            shader->setBool("specularEnabled", specularEnabled);
            shader->setInt("texture1", 0);
            shader->setInt("overlayTex", 1);
            shader->setInt("normalMap", 2);
            shader->setInt("shadowCube0", 3);
            shader->setInt("shadowCube1", 4);
            shader->setInt("shadowCube2", 5);
            shader->setInt("shadowCube3", 6);
            shader->setInt("dirShadow0", 7);
            shader->setInt("dirShadow1", 8);
            shader->setInt("dirShadow2", 9);
            shader->setInt("dirShadow3", 10);
            shader->setInt("reflectionCube", 11);
            shader->setInt("metallicRoughnessMap", 12);
            shader->setInt("emissiveMap", 13);
            shader->setInt("occlusionMap", 14);
            shader->setInt("heightMap", 15);
            shader->setInt("voxelVolume", 16);
            shader->setInt("shadowFilterQuality", shadowFilterQuality);
            shader->setFloat("shadowLodDistance", shadowLodDistance);
            shader->setBool("shadowContactHardening", shadowContactHardening);
            if (Modularity::HardwareProfile::Static().maxTextureImageUnits >= 20) {
                shader->setInt("metallicMap", 17);
                shader->setInt("roughnessMap", 18);
                shader->setInt("opacityMap", 19);
            }
            shader->setBool("giEnabled", giActiveThisPass);
            if (giActiveThisPass) {
                const SkyboxSettings giUniformSky = skybox ? skybox->getSettings() : SkyboxSettings{};
                shader->setVec3("giVolumeMin", voxelVolume.boundsMin);
                shader->setFloat("giVolumeExtent", voxelVolume.extent);
                shader->setFloat("giVolumeResolution", static_cast<float>(voxelVolume.resolution));
                shader->setFloat("giIntensity", giUniformSky.giIntensity);
                shader->setInt("giConeSteps", std::clamp(giUniformSky.giConeSteps, 4, 64));
            }
            const SkyboxSettings skySettings = skybox ? skybox->getSettings() : SkyboxSettings{};
            shader->setFloat("reflectionFadeStart", skySettings.reflectionDistanceFadeStart);
            shader->setFloat("reflectionFadeEnd", skySettings.reflectionDistanceFadeEnd);
            shader->setBool("fogEnabled", skybox != nullptr && skySettings.fogEnabled);
            shader->setInt("fogMode", skySettings.fogMode);
            shader->setVec3("fogColor", skySettings.fogColor);
            shader->setFloat("fogStart", skySettings.fogStart);
            shader->setFloat("fogEnd", skySettings.fogEnd);
            shader->setFloat("fogDensity", skySettings.fogDensity);
            shader->setFloat("fogHeight", skySettings.fogHeight);
            shader->setFloat("fogHeightFalloff", skySettings.fogHeightFalloff);
            shader->setInt("lightCount", static_cast<int>(lights.size()));
            for (size_t i = 0; i < lights.size() && i < kMaxLights; ++i) {
                const auto& l = lights[i];
                shader->setInt(kLightNames.type[i], l.type);
                shader->setVec3(kLightNames.dir[i], l.dir);
                shader->setVec3(kLightNames.pos[i], l.pos);
                shader->setVec3(kLightNames.color[i], l.color);
                shader->setFloat(kLightNames.intensity[i], l.intensity);
                shader->setFloat(kLightNames.range[i], l.range);
                shader->setFloat(kLightNames.innerCos[i], l.inner);
                shader->setFloat(kLightNames.outerCos[i], l.outer);
                shader->setVec2(kLightNames.areaSize[i], l.areaSize);
                shader->setFloat(kLightNames.areaFade[i], l.areaFade);
                shader->setInt(kLightNames.shadowMap[i], l.shadowMapIndex);
                shader->setInt(kLightNames.shadowKind[i], l.shadowKind);
                shader->setInt(kLightNames.shadowMode[i], (l.shadowMapIndex >= 0) ? l.shadowMode : 0);
                shader->setFloat(kLightNames.shadowBias[i], l.shadowBias);
                shader->setFloat(kLightNames.shadowSoftness[i], l.shadowSoftness);
                shader->setFloat(kLightNames.shadowFar[i], l.shadowFar);
                shader->setMat4(kLightNames.shadowMatrix[i], l.shadowMatrix);
            }
        }

        std::optional<SkyboxSettings> legacySkySettings;
        if (!passSkySettings) {
            legacySkySettings.emplace(skybox ? skybox->getSettings() : SkyboxSettings{});
        }
        const SkyboxSettings& skySettings = passSkySettings ? *passSkySettings : *legacySkySettings;
        const bool wantFogEnabled = skybox != nullptr && skySettings.fogEnabled;
        if (!matCache.valid || matCache.reflectionFadeStart != skySettings.reflectionDistanceFadeStart) {
            shader->setFloat("reflectionFadeStart", skySettings.reflectionDistanceFadeStart);
            matCache.reflectionFadeStart = skySettings.reflectionDistanceFadeStart;
        }
        if (!matCache.valid || matCache.reflectionFadeEnd != skySettings.reflectionDistanceFadeEnd) {
            shader->setFloat("reflectionFadeEnd", skySettings.reflectionDistanceFadeEnd);
            matCache.reflectionFadeEnd = skySettings.reflectionDistanceFadeEnd;
        }
        if (!matCache.valid || matCache.fogEnabled != wantFogEnabled) {
            shader->setBool("fogEnabled", wantFogEnabled);
            matCache.fogEnabled = wantFogEnabled;
        }
        if (!matCache.valid || matCache.fogMode != skySettings.fogMode) {
            shader->setInt("fogMode", skySettings.fogMode);
            matCache.fogMode = skySettings.fogMode;
        }
        if (!matCache.valid || matCache.fogColor != skySettings.fogColor) {
            shader->setVec3("fogColor", skySettings.fogColor);
            matCache.fogColor = skySettings.fogColor;
        }
        if (!matCache.valid || matCache.fogStart != skySettings.fogStart) {
            shader->setFloat("fogStart", skySettings.fogStart);
            matCache.fogStart = skySettings.fogStart;
        }
        if (!matCache.valid || matCache.fogEnd != skySettings.fogEnd) {
            shader->setFloat("fogEnd", skySettings.fogEnd);
            matCache.fogEnd = skySettings.fogEnd;
        }
        if (!matCache.valid || matCache.fogDensity != skySettings.fogDensity) {
            shader->setFloat("fogDensity", skySettings.fogDensity);
            matCache.fogDensity = skySettings.fogDensity;
        }
        if (!matCache.valid || matCache.fogHeight != skySettings.fogHeight) {
            shader->setFloat("fogHeight", skySettings.fogHeight);
            matCache.fogHeight = skySettings.fogHeight;
        }
        if (!matCache.valid || matCache.fogHeightFalloff != skySettings.fogHeightFalloff) {
            shader->setFloat("fogHeightFalloff", skySettings.fogHeightFalloff);
            matCache.fogHeightFalloff = skySettings.fogHeightFalloff;
        }

        const bool hasRuntimeAlbedoOverride =
            objPtr != nullptr &&
            objPtr->runtimeHasAlbedoTextureOverride &&
            objPtr->runtimeAlbedoTextureOverrideId != 0;
        const UiTargetInfo* uiTarget = nullptr;
        if (item.isUiCanvas3D && objPtr != nullptr) {
            auto uiIt = uiTargetViews.find(objPtr->id);
            if (uiIt != uiTargetViews.end() && uiIt->second.texture != 0) {
                uiTarget = &uiIt->second;
            }
        }
        bool hasMaterialAsset = !materialPath.empty();
        bool hasCustomShader = !vertPath.empty() || !fragPath.empty();
        bool hasAnySurfaceInput = hasRuntimeAlbedoOverride ||
                                  !albedoTexturePath.empty() ||
                                  !overlayTexturePath.empty() ||
                                  !normalMapPath.empty();
        bool missingMaterialAndShader = !hasMaterialAsset && !hasCustomShader && !hasAnySurfaceInput;

        bool wantUnlit = item.unlit || missingMaterialAndShader;
        if (!matCache.valid || matCache.unlit != wantUnlit) {
            shader->setBool("unlit", wantUnlit);
            matCache.unlit = wantUnlit;
        }

        shader->setMat4("model", item.model);

        glm::vec3 wantColor = missingMaterialAndShader ? glm::vec3(1.0f) : item.material.color;
        if (!matCache.valid || matCache.materialColor != wantColor) {
            shader->setVec3("materialColor", wantColor);
            matCache.materialColor = wantColor;
        }
        float wantAlpha = missingMaterialAndShader ? 1.0f : item.material.alpha;
        if (!matCache.valid || matCache.materialAlpha != wantAlpha) {
            shader->setFloat("materialAlpha", wantAlpha);
            matCache.materialAlpha = wantAlpha;
        }
        if (!matCache.valid || matCache.ambientStrength != item.material.ambientStrength) {
            shader->setFloat("ambientStrength", item.material.ambientStrength);
            matCache.ambientStrength = item.material.ambientStrength;
        }
        if (!matCache.valid || matCache.specularStrength != item.material.specularStrength) {
            shader->setFloat("specularStrength", item.material.specularStrength);
            matCache.specularStrength = item.material.specularStrength;
        }
        if (!matCache.valid || matCache.shininess != item.material.shininess) {
            shader->setFloat("shininess", item.material.shininess);
            matCache.shininess = item.material.shininess;
        }
        if (!matCache.valid || matCache.normalMapIntensity != item.material.normalMapIntensity) {
            shader->setFloat("normalMapIntensity", item.material.normalMapIntensity);
            matCache.normalMapIntensity = item.material.normalMapIntensity;
        }
        const bool wantPbr = item.material.pbrEnabled && !missingMaterialAndShader;
        const bool separateMapUnitsAvailable =
            Modularity::HardwareProfile::Static().maxTextureImageUnits >= 20;
        if (!matCache.valid || matCache.usePbr != wantPbr) {
            shader->setBool("usePbr", wantPbr);
            matCache.usePbr = wantPbr;
        }
        if (!matCache.valid || matCache.pbrMetallic != item.material.metallic) {
            shader->setFloat("pbrMetallic", item.material.metallic);
            matCache.pbrMetallic = item.material.metallic;
        }
        if (!matCache.valid || matCache.pbrRoughness != item.material.roughness) {
            shader->setFloat("pbrRoughness", item.material.roughness);
            matCache.pbrRoughness = item.material.roughness;
        }
        if (!matCache.valid || matCache.occlusionStrength != item.material.occlusionStrength) {
            shader->setFloat("occlusionStrength", item.material.occlusionStrength);
            matCache.occlusionStrength = item.material.occlusionStrength;
        }
        if (!matCache.valid || matCache.emissionColor != item.material.emissionColor) {
            shader->setVec3("emissionColor", item.material.emissionColor);
            matCache.emissionColor = item.material.emissionColor;
        }
        const float wantEmission = missingMaterialAndShader ? 0.0f : item.material.emissionStrength;
        if (!matCache.valid || matCache.emissionStrength != wantEmission) {
            shader->setFloat("emissionStrength", wantEmission);
            matCache.emissionStrength = wantEmission;
        }
        const float wantHeightScale = missingMaterialAndShader ? 0.0f : item.material.heightScale;
        if (!matCache.valid || matCache.heightScale != wantHeightScale) {
            shader->setFloat("heightScale", wantHeightScale);
            matCache.heightScale = wantHeightScale;
        }
        if (!matCache.valid || matCache.parallaxSteps != item.material.parallaxSteps) {
            shader->setInt("parallaxSteps", item.material.parallaxSteps);
            matCache.parallaxSteps = item.material.parallaxSteps;
        }
        if (!matCache.valid || matCache.parallaxInvertHeight != item.material.parallaxInvertHeight) {
            shader->setBool("parallaxInvertHeight", item.material.parallaxInvertHeight);
            matCache.parallaxInvertHeight = item.material.parallaxInvertHeight;
        }
        if (!matCache.valid || matCache.mixAmount != item.material.textureMix) {
            shader->setFloat("mixAmount", item.material.textureMix);
            matCache.mixAmount = item.material.textureMix;
        }
        if (!matCache.valid || matCache.scrollSpeed != item.material.scrollSpeed) {
            shader->setFloat("uScrollSpeed", item.material.scrollSpeed);
            matCache.scrollSpeed = item.material.scrollSpeed;
        }
        if (!matCache.valid || matCache.scrollDirection != item.material.scrollDirection) {
            shader->setVec2("uScrollDir", item.material.scrollDirection);
            matCache.scrollDirection = item.material.scrollDirection;
        }
        if (!matCache.valid || matCache.cloudColor != item.material.cloudColor) {
            shader->setVec3("uCloudColor", item.material.cloudColor);
            matCache.cloudColor = item.material.cloudColor;
        }
        if (!matCache.valid || matCache.cloudSkyColor != item.material.cloudSkyColor) {
            shader->setVec3("uCloudSkyColor", item.material.cloudSkyColor);
            matCache.cloudSkyColor = item.material.cloudSkyColor;
        }
        if (!matCache.valid || matCache.cloudScale != item.material.cloudScale) {
            shader->setFloat("uCloudScale", item.material.cloudScale);
            matCache.cloudScale = item.material.cloudScale;
        }
        if (!matCache.valid || matCache.cloudCoverage != item.material.cloudCoverage) {
            shader->setFloat("uCloudCoverage", item.material.cloudCoverage);
            matCache.cloudCoverage = item.material.cloudCoverage;
        }
        if (!matCache.valid || matCache.cloudSoftness != item.material.cloudSoftness) {
            shader->setFloat("uCloudSoftness", item.material.cloudSoftness);
            matCache.cloudSoftness = item.material.cloudSoftness;
        }
        if (!matCache.valid || matCache.cloudDetail != item.material.cloudDetail) {
            shader->setInt("uCloudDetail", item.material.cloudDetail);
            matCache.cloudDetail = item.material.cloudDetail;
        }
        if (!matCache.valid || matCache.cloudSpeed != item.material.cloudSpeed) {
            shader->setFloat("uCloudSpeed", item.material.cloudSpeed);
            matCache.cloudSpeed = item.material.cloudSpeed;
        }
        if (!matCache.valid || matCache.cloudWarp != item.material.cloudWarp) {
            shader->setFloat("uCloudWarp", item.material.cloudWarp);
            matCache.cloudWarp = item.material.cloudWarp;
        }
        if (!matCache.valid || matCache.cloudHighlight != item.material.cloudHighlight) {
            shader->setFloat("uCloudHighlight", item.material.cloudHighlight);
            matCache.cloudHighlight = item.material.cloudHighlight;
        }
        if (!matCache.valid || matCache.cloudStars != item.material.cloudStars) {
            shader->setFloat("uCloudStars", item.material.cloudStars);
            matCache.cloudStars = item.material.cloudStars;
        }
        if (!matCache.valid || matCache.cloudHorizon != item.material.cloudHorizon) {
            shader->setFloat("uCloudHorizon", item.material.cloudHorizon);
            matCache.cloudHorizon = item.material.cloudHorizon;
        }
        glm::vec4 wantUvT = uiTarget ? uiTarget->uvTransform
                                      : BuildSurfaceUvTransform(objPtr, item.material);
        if (!matCache.valid || matCache.uvTransform != wantUvT) {
            shader->setVec4("uvTransform", wantUvT);
            matCache.uvTransform = wantUvT;
        }
        glm::vec4 wantUvR = uiTarget ? glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)
                                     : (objPtr ? BuildSpriteUvRect(*objPtr) : glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
        if (!matCache.valid || matCache.uvRect != wantUvR) {
            shader->setVec4("uvRect", wantUvR);
            matCache.uvRect = wantUvR;
        }

        bool wantSkin = false;
        if (objPtr && objPtr->hasSkeletalAnimation && objPtr->skeletal.enabled) {
            int safeLimit = std::max(0, item.boneLimit);
            int boneCount = std::min<int>(item.availableBones, safeLimit);
            if (item.wantsGpuSkinning && boneCount > 0) {
                shader->setInt("boneCount", boneCount);
                shader->setMat4Array("bones", objPtr->skeletal.finalMatrices.data(), boneCount);
                matCache.boneCount = boneCount;
                wantSkin = true;
            }
        }
        if (!matCache.valid || matCache.useSkinning != wantSkin) {
            shader->setBool("useSkinning", wantSkin);
            matCache.useSkinning = wantSkin;
        }

        bool usingUiTargetTex = false;
        if (item.isUiCanvas3D && uiTarget != nullptr) {
            bindTexture2D(GL_TEXTURE0, uiTarget->texture);
            usingUiTargetTex = true;
        }
        Texture* baseTex = texture1;
        if (!usingUiTargetTex) {
            if (missingMaterialAndShader && missingMaterialFallbackTexture != 0) {
                bindTexture2D(GL_TEXTURE0, missingMaterialFallbackTexture);
            } else if (hasRuntimeAlbedoOverride) {
                bindTexture2D(GL_TEXTURE0, objPtr->runtimeAlbedoTextureOverrideId);
            } else {
                bool albedoResolved = false;
                if (!albedoTexturePath.empty()) {
                    if (auto* t = getTexture(albedoTexturePath, item.material.textureFilter)) {
                        baseTex = t;
                        albedoResolved = true;
                    }
                }
                if (albedoResolved || albedoTexturePath.empty()) {
                    if (baseTex) bindTexture2D(GL_TEXTURE0, baseTex->GetID());
                } else {
                    bindTexture2D(GL_TEXTURE0, debugWhiteTexture);
                }
            }
        }

        bool overlayUsed = false;
        if (objPtr && objPtr->renderType == RenderType::Mirror) {
            auto it = mirrorTargets.find(objPtr->id);
            if (it != mirrorTargets.end() && it->second.texture != 0) {
                bindTexture2D(GL_TEXTURE1, it->second.texture);
                overlayUsed = true;
            }
        }
        if (!overlayUsed && item.useOverlay && !overlayTexturePath.empty()) {
            if (auto* t = getTexture(overlayTexturePath, item.material.textureFilter)) {
                bindTexture2D(GL_TEXTURE1, t->GetID());
                overlayUsed = true;
            }
        }
        if (!overlayUsed && texture2) {
            bindTexture2D(GL_TEXTURE1, texture2->GetID());
        }
        if (!matCache.valid || matCache.hasOverlay != overlayUsed) {
            shader->setBool("hasOverlay", overlayUsed);
            matCache.hasOverlay = overlayUsed;
        }

        float reflectionIntensity = item.reflectionIntensity;
        unsigned int reflectionCube = item.reflectionResolved
            ? item.reflectionCube
            : findReflectionCastForItem(item, reflectionIntensity);
        if (reflectionCube == 0 && skybox && skyboxReflectionTarget.cube != 0 && skyboxReflectionTarget.hasCapture) {
            const SkyboxSettings& skySettings = skybox->getSettings();
            if (skySettings.environmentReflections && skySettings.environmentReflectionIntensity > 0.001f) {
                reflectionCube = skyboxReflectionTarget.cube;
                reflectionIntensity = skySettings.environmentReflectionIntensity;
            }
        }
        const bool reflectionUsed = reflectionCube != 0 && reflectionIntensity > 0.001f;
        if (reflectionUsed) {
            glActiveTexture(GL_TEXTURE11);
            glBindTexture(GL_TEXTURE_CUBE_MAP, reflectionCube);
            glActiveTexture(GL_TEXTURE0);
        }
        if (!matCache.valid || matCache.hasReflectionCast != reflectionUsed) {
            shader->setBool("hasReflectionCast", reflectionUsed);
            matCache.hasReflectionCast = reflectionUsed;
        }
        if (!matCache.valid || std::abs(matCache.reflectionIntensity - reflectionIntensity) > 0.0001f) {
            shader->setFloat("reflectionIntensity", reflectionIntensity);
            matCache.reflectionIntensity = reflectionIntensity;
        }

        bool normalUsed = false;
        if (!normalMapPath.empty()) {
            if (auto* t = getTexture(normalMapPath, item.material.textureFilter, true)) {
                bindTexture2D(GL_TEXTURE2, t->GetID());
                normalUsed = true;
            }
        }
        // map unit below already does this, and the odd one out kept a stale
        if (!normalUsed) bindTexture2D(GL_TEXTURE2, 0);
        if (!matCache.valid || matCache.hasNormalMap != normalUsed) {
            shader->setBool("hasNormalMap", normalUsed);
            matCache.hasNormalMap = normalUsed;
        }

        const std::string& metallicRoughnessPath =
            item.metallicRoughnessMapPath ? *item.metallicRoughnessMapPath : emptyPath;
        const std::string& emissivePath = item.emissiveMapPath ? *item.emissiveMapPath : emptyPath;
        const std::string& occlusionPath = item.occlusionMapPath ? *item.occlusionMapPath : emptyPath;

        bool metallicRoughnessUsed = false;
        if (wantPbr && !metallicRoughnessPath.empty()) {
            if (auto* t = getTexture(metallicRoughnessPath, item.material.textureFilter)) {
                bindTexture2D(GL_TEXTURE12, t->GetID());
                metallicRoughnessUsed = true;
            }
        }
        if (!metallicRoughnessUsed) bindTexture2D(GL_TEXTURE12, 0);
        if (!matCache.valid || matCache.hasMetallicRoughnessMap != metallicRoughnessUsed) {
            shader->setBool("hasMetallicRoughnessMap", metallicRoughnessUsed);
            matCache.hasMetallicRoughnessMap = metallicRoughnessUsed;
        }

        const std::string& metallicPath =
            item.metallicMapPath ? *item.metallicMapPath : emptyPath;
        const std::string& roughnessPath =
            item.roughnessMapPath ? *item.roughnessMapPath : emptyPath;
        const std::string& opacityPath =
            item.opacityMapPath ? *item.opacityMapPath : emptyPath;

        bool metallicUsed = false;
        if (separateMapUnitsAvailable && wantPbr && !metallicPath.empty()) {
            if (auto* t = getTexture(metallicPath, item.material.textureFilter)) {
                bindTexture2D(GL_TEXTURE17, t->GetID());
                metallicUsed = true;
            }
        }
        if (separateMapUnitsAvailable && !metallicUsed) bindTexture2D(GL_TEXTURE17, 0);
        if (!matCache.valid || matCache.hasMetallicMap != metallicUsed) {
            shader->setBool("hasMetallicMap", metallicUsed);
            matCache.hasMetallicMap = metallicUsed;
        }

        bool roughnessUsed = false;
        if (separateMapUnitsAvailable && wantPbr && !roughnessPath.empty()) {
            if (auto* t = getTexture(roughnessPath, item.material.textureFilter)) {
                bindTexture2D(GL_TEXTURE18, t->GetID());
                roughnessUsed = true;
            }
        }
        if (separateMapUnitsAvailable && !roughnessUsed) bindTexture2D(GL_TEXTURE18, 0);
        if (!matCache.valid || matCache.hasRoughnessMap != roughnessUsed) {
            shader->setBool("hasRoughnessMap", roughnessUsed);
            matCache.hasRoughnessMap = roughnessUsed;
        }

        bool opacityUsed = false;
        if (separateMapUnitsAvailable && !opacityPath.empty()) {
            if (auto* t = getTexture(opacityPath, item.material.textureFilter)) {
                bindTexture2D(GL_TEXTURE19, t->GetID());
                opacityUsed = true;
            }
        }
        if (separateMapUnitsAvailable && !opacityUsed) bindTexture2D(GL_TEXTURE19, 0);
        if (!matCache.valid || matCache.hasOpacityMap != opacityUsed) {
            shader->setBool("hasOpacityMap", opacityUsed);
            matCache.hasOpacityMap = opacityUsed;
        }

        const float wantCutoff = std::clamp(item.material.alphaCutoff, 0.0f, 1.0f);
        if (!matCache.valid || matCache.alphaCutoff != wantCutoff) {
            shader->setFloat("alphaCutoff", wantCutoff);
            matCache.alphaCutoff = wantCutoff;
        }

        bool emissiveUsed = false;
        if (wantEmission > 0.0f && !emissivePath.empty()) {
            if (auto* t = getTexture(emissivePath, item.material.textureFilter)) {
                bindTexture2D(GL_TEXTURE13, t->GetID());
                emissiveUsed = true;
            }
        }
        if (!emissiveUsed) bindTexture2D(GL_TEXTURE13, 0);
        if (!matCache.valid || matCache.hasEmissiveMap != emissiveUsed) {
            shader->setBool("hasEmissiveMap", emissiveUsed);
            matCache.hasEmissiveMap = emissiveUsed;
        }

        bool occlusionUsed = false;
        if (wantPbr && !occlusionPath.empty()) {
            if (auto* t = getTexture(occlusionPath, item.material.textureFilter)) {
                bindTexture2D(GL_TEXTURE14, t->GetID());
                occlusionUsed = true;
            }
        }
        if (!occlusionUsed) bindTexture2D(GL_TEXTURE14, 0);
        if (!matCache.valid || matCache.hasOcclusionMap != occlusionUsed) {
            shader->setBool("hasOcclusionMap", occlusionUsed);
            matCache.hasOcclusionMap = occlusionUsed;
        }

        const std::string& heightPath = item.heightMapPath ? *item.heightMapPath : emptyPath;
        bool heightUsed = false;
        if (wantHeightScale > 0.0f && !heightPath.empty()) {
            if (auto* t = getTexture(heightPath, item.material.textureFilter)) {
                bindTexture2D(GL_TEXTURE15, t->GetID());
                heightUsed = true;
            }
        }
        if (!heightUsed) bindTexture2D(GL_TEXTURE15, 0);
        if (!matCache.valid || matCache.hasHeightMap != heightUsed) {
            shader->setBool("hasHeightMap", heightUsed);
            matCache.hasHeightMap = heightUsed;
        }

        matCache.valid = true;

        if (objPtr && objPtr->renderType == RenderType::Model && objPtr->meshId != -1 &&
            objPtr->hasSkeletalAnimation && objPtr->skeletal.enabled && !item.wantsGpuSkinning) {
            const auto* meshInfo = getModelLoader().getMeshInfo(objPtr->meshId);
            if (meshInfo) {
                applyCpuSkinning(*const_cast<OBJLoader::LoadedMesh*>(meshInfo),
                                 objPtr->skeletal.finalMatrices,
                                 objPtr->skeletal.maxBones);
            }
        }

        const bool wantFrontFaceCCW = glm::determinant(glm::mat3(item.model)) >= 0.0f;
        if (wantFrontFaceCCW != currentFrontFaceCCW) {
            glFrontFace(wantFrontFaceCCW ? GL_CCW : GL_CW);
            currentFrontFaceCCW = wantFrontFaceCCW;
            Runtime2DCountStateBind();
        }

        if (item.doubleSided) {
            if (currentCullFaceEnabled) {
                glDisable(GL_CULL_FACE);
                currentCullFaceEnabled = false;
                Runtime2DCountStateBind();
            }
        } else if (!currentCullFaceEnabled) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            currentCullFaceEnabled = true;
            Runtime2DCountStateBind();
        }

        bool wantsTransparency = !item.sortOpaque;
        bool wantsBlend = wantsTransparency || item.isUiCanvas3D;
        if (wantsBlend) {
            if (!currentBlendEnabled) {
                glEnable(GL_BLEND);
                currentBlendEnabled = true;
                Runtime2DCountStateBind();
            }
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            Runtime2DCountStateBind();
        } else if (currentBlendEnabled) {
            glDisable(GL_BLEND);
            currentBlendEnabled = false;
            Runtime2DCountStateBind();
        }
        if ((wantsTransparency || item.isUiCanvas3D) && currentDepthMask) {
            glDepthMask(GL_FALSE);
            currentDepthMask = false;
            Runtime2DCountStateBind();
        } else if (!(wantsTransparency || item.isUiCanvas3D) && !currentDepthMask) {
            glDepthMask(GL_TRUE);
            currentDepthMask = true;
            Runtime2DCountStateBind();
        }
        if (item.multiDrawBatch >= 0 && optPass && optState) {
            const auto& batch = optState->multiDraw.batches[static_cast<size_t>(item.multiDrawBatch)];
            optState->drawMultiDrawBatch(*optPass, batch);
            if (activeStats) {
                activeStats->drawCalls += 1;
                activeStats->multiDrawCalls += 1;
                activeStats->meshDraws += static_cast<int>(
                    optPass->gpuCommandRegion >= 0 ? batch.slotCount : batch.visibleCount);
            }
            continue;
        }
        // Blended surfaces never write depth, so between themselves they are ordered only
        // by one sort key per object. That cannot order two that overlap in depth - a long
        // fence running past a wide grass card - nor the cards inside one foliage mesh.
        // A surface that is transparent because of its texture (soft-edged albedo alpha at
        // full material alpha: fences, foliage, cut-out sprites) first writes its solid
        // pixels to depth, the way a binary-alpha texture already does on the opaque path.
        // Per item, right before its own colour: a correctly sorted scene draws the same
        // colours, and only what was drawn out of order behind a solid pixel is rejected.
        // Material alpha below 1 is deliberate translucency (glass, fades) and stays as is.
        const bool depthPrepass = wantsTransparency && !item.isUiCanvas3D &&
                                  item.material.alpha >= 0.999f && shaderHonorsAlphaCutoff(shader);
        if (depthPrepass) {
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glDepthMask(GL_TRUE);
            shader->setFloat("alphaCutoff", std::max(wantCutoff, kTransparentDepthCutoff));
            shader->setBool("depthPrepass", true);
            recordMeshDraw();
            item.mesh->draw();
            shader->setBool("depthPrepass", false);
            shader->setFloat("alphaCutoff", wantCutoff);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_FALSE);
            // Same program and vertices, so the colour pass lands on exactly that depth.
            glDepthFunc(GL_LEQUAL);
            Runtime2DCountStateBind();
        }
        recordMeshDraw();
        if (activeStats) {
            activeStats->triangles += item.mesh->getVertexCount() / 3;
        }
        item.mesh->draw();
        if (depthPrepass) {
            glDepthFunc(GL_LESS);
        }
    }
    submissionScope.reset();
    flushPendingSkybox();

    auto drawMaskedSprite25DUiLayer = [&]() {
        ensureQuad();
        if (quadVAO == 0) {
            return;
        }

        const float viewportWidth = static_cast<float>(std::max(1, width));
        const float viewportHeight = static_cast<float>(std::max(1, height));

        std::vector<const SceneObject*> spriteItems;
        spriteItems.reserve(sceneObjects.size());
        for (const SceneObject& obj : sceneObjects) {
            if (!IsObjectEnabledInHierarchy(obj) || !IsMaskedSprite25DUiLayerObject(obj)) {
                continue;
            }

            const glm::vec4 clipCenter = proj * view * glm::vec4(obj.position, 1.0f);
            if (std::abs(clipCenter.w) <= 1e-5f) {
                continue;
            }
            const glm::vec3 ndcCenter = glm::vec3(clipCenter) / clipCenter.w;
            if (clipCenter.w <= 0.0f || ndcCenter.z < -1.0f || ndcCenter.z > 1.0f) {
                continue;
            }
            spriteItems.push_back(&obj);
        }
        if (spriteItems.empty()) {
            return;
        }

        std::stable_sort(spriteItems.begin(), spriteItems.end(),
                         [&](const SceneObject* a, const SceneObject* b) {
                             if (a->ui.pseudo3DDepthSort != b->ui.pseudo3DDepthSort) {
                                 return a->ui.pseudo3DDepthSort < b->ui.pseudo3DDepthSort;
                             }
                             const float aDepth = -glm::vec3(view * glm::vec4(a->position, 1.0f)).z;
                             const float bDepth = -glm::vec3(view * glm::vec4(b->position, 1.0f)).z;
                             if (std::abs(aDepth - bDepth) > 0.001f) {
                                 return aDepth > bDepth;
                             }
                             return a->id < b->id;
                         });

        static const std::string maskedSpriteVert = "Resources/Shaders/masked_sprite25d_ui_vert.glsl";
        static const std::string maskedSpriteFrag = "Resources/Shaders/masked_sprite25d_ui_frag.glsl";
        Shader* spriteShader = getShader(maskedSpriteVert, maskedSpriteFrag);
        if (spriteShader == nullptr) {
            return;
        }

        GLint previousDepthFunc = GL_LESS;
        glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunc);

        if (currentCullFaceEnabled) {
            glDisable(GL_CULL_FACE);
            currentCullFaceEnabled = false;
            Runtime2DCountStateBind();
        }
        if (!currentBlendEnabled) {
            glEnable(GL_BLEND);
            currentBlendEnabled = true;
            Runtime2DCountStateBind();
        }
        if (currentDepthMask) {
            glDepthMask(GL_FALSE);
            currentDepthMask = false;
            Runtime2DCountStateBind();
        }
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        shader = spriteShader;
        currentShader = spriteShader;
        shader->use();
        Runtime2DCountStateBind();
        shader->setInt("spriteTexture", 0);

        for (const SceneObject* obj : spriteItems) {
            const glm::vec4 clipCenter = proj * view * glm::vec4(obj->position, 1.0f);
            if (std::abs(clipCenter.w) <= 1e-5f) {
                continue;
            }
            const glm::vec3 ndcCenter = glm::vec3(clipCenter) / clipCenter.w;
            if (clipCenter.w <= 0.0f || ndcCenter.z < -1.0f || ndcCenter.z > 1.0f) {
                continue;
            }

            glm::vec2 halfSizeNdc(0.0f);
            if (!ProjectSprite25DProxyHalfSizeNdc(*obj, view, proj, viewportWidth, viewportHeight, halfSizeNdc)) {
                continue;
            }

            GLuint textureId = 0;
            if (obj->runtimeHasAlbedoTextureOverride && obj->runtimeAlbedoTextureOverrideId != 0) {
                textureId = obj->runtimeAlbedoTextureOverrideId;
            } else if (!obj->albedoTexturePath.empty()) {
                if (Texture* tex = getTexture(obj->albedoTexturePath, MaterialProperties::TextureFilter::Point)) {
                    textureId = tex->GetID();
                }
            }
            if (textureId == 0) {
                textureId = missingMaterialFallbackTexture;
            }
            if (textureId == 0) {
                continue;
            }

            shader->setVec2("centerNdc", glm::vec2(ndcCenter.x, ndcCenter.y));
            shader->setVec2("halfSizeNdc", halfSizeNdc);
            shader->setFloat("depthNdc", ndcCenter.z);
            shader->setFloat("rotationRadians", glm::radians(obj->ui.rotation));
            shader->setVec4("tint", obj->ui.color);
            shader->setVec4("uvRect", BuildSpriteUvRect(*obj));
            bindTexture2D(GL_TEXTURE0, textureId);

            recordDrawCall();
            glBindVertexArray(quadVAO);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        glBindVertexArray(0);

        glDepthFunc(previousDepthFunc);
    };
    drawMaskedSprite25DUiLayer();

    // Particles go last among the scene's blended surfaces, so they still land over 3D UI
    // canvases as they did when they were an overlay, but now inside the frame: depth-tested
    // against solid geometry, fogged, and through post FX. Reflection probes skip them.
    if (!reflectionCapturePass) {
        renderParticleSystems(sceneObjects, view, proj);
    }

    if (currentBlendEnabled != (blendEnabled == GL_TRUE)) {
        if (blendEnabled) glEnable(GL_BLEND);
        else glDisable(GL_BLEND);
    }
    if (currentDepthMask != (depthMask == GL_TRUE)) {
        glDepthMask(depthMask);
    }
    if (!cullFace) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(prevCullMode);
    }
    glFrontFace(prevFrontFace);

    if (skybox && !reflectionCapturePass && sceneDepthTextureForPass != 0 &&
        sceneColorTextureForPass != 0 && width > 0 && height > 0 && !drawItems.empty()) {
        const SkyboxSettings& ssrSky = skybox->getSettings();
        Shader* ssr = nullptr;
        Shader* gbufferStatic = nullptr;
        Shader* gbufferSkinned = nullptr;
        if (ssrSky.ssrEnabled && ssrSky.ssrIntensity > 0.0f) {
            gbufferStatic = getShader(defaultVertPath, gbufferFragPath);
            gbufferSkinned = getShader(skinnedVertPath, gbufferFragPath);
            ssr = getShader(postVertPath, ssrFragPath);
        }
        if (ssr && ssr->ID != 0 && gbufferStatic && gbufferStatic->ID != 0) {
            MODU_PROFILE_SCOPE("Screen Space Reflections", ProfilerSampleCategory::RenderDetail);
            ssrShader = ssr;
            gbufferShader = gbufferStatic;

            GLint previousFramebuffer = 0;
            GLint previousViewport[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFramebuffer);
            glGetIntegerv(GL_VIEWPORT, previousViewport);

            ensureSurfaceTarget(width, height, sceneDepthTextureForPass);

            if (surfaceTarget.fbo != 0 && surfaceTarget.texture != 0) {
                glBindFramebuffer(GL_FRAMEBUFFER, surfaceTarget.fbo);
                glViewport(0, 0, width, height);
                GLfloat previousClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClear);
                glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glClearColor(previousClear[0], previousClear[1],
                             previousClear[2], previousClear[3]);

                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
                glDepthMask(GL_FALSE);
                glDisable(GL_BLEND);
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                bool prepassFrontFaceCCW = true;
                glFrontFace(GL_CCW);
                bool prepassCullEnabled = true;

                // Batched objects write their surfaces through the same batches, with the
                // batch vertex stage in front of the G-buffer fragment stage.
                Shader* gbufferMultiDraw = nullptr;
                if (optPass && optPass->multiDrawUsed) {
                    gbufferMultiDraw = getShader(kMultiDrawVertPath, gbufferFragPath);
                    if (!gbufferMultiDraw || gbufferMultiDraw == defaultShader ||
                        gbufferMultiDraw->ID == 0) {
                        gbufferMultiDraw = nullptr;
                    }
                }
                Shader* active = nullptr;
                for (const RenderItem& item : drawItems) {
                    const bool drawsBatch = item.multiDrawBatch >= 0 && gbufferMultiDraw != nullptr;
                    if ((item.mesh == nullptr && !drawsBatch) || !item.sortOpaque || item.unlit ||
                        item.isUiCanvas3D) {
                        continue;
                    }
                    Shader* want = drawsBatch ? gbufferMultiDraw
                                 : (item.wantsGpuSkinning && gbufferSkinned &&
                                    gbufferSkinned->ID != 0) ? gbufferSkinned : gbufferStatic;
                    if (want != active) {
                        active = want;
                        active->use();
                        active->setMat4("view", view);
                        active->setMat4("projection", proj);
                        active->setVec3("viewPos", camera.position);
                        active->setInt("normalMap", 2);
                        active->setInt("metallicRoughnessMap", 12);
                        active->setInt("heightMap", 15);
                        if (Modularity::HardwareProfile::Static().maxTextureImageUnits >= 20) {
                            active->setInt("metallicMap", 17);
                            active->setInt("roughnessMap", 18);
                        }
                    }

                    const SceneObject* objPtr = item.obj;
                    active->setMat4("model", item.model);
                    active->setVec4("uvTransform", BuildSurfaceUvTransform(objPtr, item.material));
                    active->setVec4("uvRect", objPtr ? BuildSpriteUvRect(*objPtr)
                                                     : glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));

                    bool wantSkin = false;
                    if (active == gbufferSkinned && objPtr && objPtr->hasSkeletalAnimation &&
                        objPtr->skeletal.enabled) {
                        const int safeLimit = std::max(0, item.boneLimit);
                        const int boneCount = std::min<int>(item.availableBones, safeLimit);
                        if (item.wantsGpuSkinning && boneCount > 0) {
                            active->setInt("boneCount", boneCount);
                            active->setMat4Array("bones", objPtr->skeletal.finalMatrices.data(),
                                                 boneCount);
                            wantSkin = true;
                        }
                    }
                    active->setBool("useSkinning", wantSkin);

                    active->setBool("usePbr", item.material.pbrEnabled);
                    active->setFloat("pbrMetallic", item.material.metallic);
                    active->setFloat("pbrRoughness", item.material.roughness);
                    active->setFloat("specularStrength", item.material.specularStrength);
                    active->setFloat("shininess", item.material.shininess);
                    active->setFloat("normalMapIntensity", item.material.normalMapIntensity);
                    active->setFloat("heightScale", item.material.heightScale);
                    active->setInt("parallaxSteps", item.material.parallaxSteps);
                    active->setBool("parallaxInvertHeight", item.material.parallaxInvertHeight);

                    const std::string& nPath = item.normalMapPath ? *item.normalMapPath : emptyPath;
                    bool nUsed = false;
                    if (!nPath.empty()) {
                        if (auto* t = getTexture(nPath, item.material.textureFilter, true)) {
                            glActiveTexture(GL_TEXTURE2);
                            glBindTexture(GL_TEXTURE_2D, t->GetID());
                            nUsed = true;
                        }
                    }
                    active->setBool("hasNormalMap", nUsed);

                    const std::string& mrPath =
                        item.metallicRoughnessMapPath ? *item.metallicRoughnessMapPath : emptyPath;
                    bool mrUsed = false;
                    if (item.material.pbrEnabled && !mrPath.empty()) {
                        if (auto* t = getTexture(mrPath, item.material.textureFilter)) {
                            glActiveTexture(GL_TEXTURE12);
                            glBindTexture(GL_TEXTURE_2D, t->GetID());
                            mrUsed = true;
                        }
                    }
                    active->setBool("hasMetallicRoughnessMap", mrUsed);

                    const bool gbufSeparateUnits =
                        Modularity::HardwareProfile::Static().maxTextureImageUnits >= 20;
                    const std::string& mPath =
                        item.metallicMapPath ? *item.metallicMapPath : emptyPath;
                    bool mUsed = false;
                    if (gbufSeparateUnits && item.material.pbrEnabled && !mPath.empty()) {
                        if (auto* t = getTexture(mPath, item.material.textureFilter)) {
                            glActiveTexture(GL_TEXTURE17);
                            glBindTexture(GL_TEXTURE_2D, t->GetID());
                            mUsed = true;
                        }
                    }
                    active->setBool("hasMetallicMap", mUsed);

                    const std::string& rPath =
                        item.roughnessMapPath ? *item.roughnessMapPath : emptyPath;
                    bool rUsed = false;
                    if (gbufSeparateUnits && item.material.pbrEnabled && !rPath.empty()) {
                        if (auto* t = getTexture(rPath, item.material.textureFilter)) {
                            glActiveTexture(GL_TEXTURE18);
                            glBindTexture(GL_TEXTURE_2D, t->GetID());
                            rUsed = true;
                        }
                    }
                    active->setBool("hasRoughnessMap", rUsed);

                    const std::string& hPath = item.heightMapPath ? *item.heightMapPath : emptyPath;
                    bool hUsed = false;
                    if (item.material.heightScale > 0.0f && !hPath.empty()) {
                        if (auto* t = getTexture(hPath, item.material.textureFilter)) {
                            glActiveTexture(GL_TEXTURE15);
                            glBindTexture(GL_TEXTURE_2D, t->GetID());
                            hUsed = true;
                        }
                    }
                    active->setBool("hasHeightMap", hUsed);

                    const bool wantCCW = glm::determinant(glm::mat3(item.model)) >= 0.0f;
                    if (wantCCW != prepassFrontFaceCCW) {
                        glFrontFace(wantCCW ? GL_CCW : GL_CW);
                        prepassFrontFaceCCW = wantCCW;
                    }
                    if (item.doubleSided == prepassCullEnabled) {
                        if (item.doubleSided) glDisable(GL_CULL_FACE);
                        else glEnable(GL_CULL_FACE);
                        prepassCullEnabled = !item.doubleSided;
                    }

                    if (drawsBatch) {
                        recordDrawCall();
                        optState->drawMultiDrawBatch(
                            *optPass, optState->multiDraw.batches[static_cast<size_t>(item.multiDrawBatch)]);
                        continue;
                    }
                    recordMeshDraw();
                    item.mesh->draw();
                }
                glFrontFace(GL_CCW);

                const int ssrScale = ssrSky.ssrHalfResolution ? 2 : 1;
                const int ssrWidth = std::max(1, width / ssrScale);
                const int ssrHeight = std::max(1, height / ssrScale);
                ensureRenderTarget(ssrTarget, ssrWidth, ssrHeight, true, true, false);

                if (ssrTarget.fbo != 0 && ssrTarget.texture != 0) {
                    glBindFramebuffer(GL_FRAMEBUFFER, ssrTarget.fbo);
                    glViewport(0, 0, ssrWidth, ssrHeight);
                    glDisable(GL_DEPTH_TEST);
                    glDisable(GL_BLEND);
                    glDisable(GL_CULL_FACE);
                    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                    glClear(GL_COLOR_BUFFER_BIT);

                    ssr->use();
                    ssr->setMat4("viewProj", proj * view);
                    ssr->setMat4("invViewProj", glm::inverse(proj * view));
                    ssr->setVec3("viewPos", camera.position);
                    ssr->setFloat("nearPlane", nearPlane);
                    ssr->setFloat("farPlane", farPlane);
                    ssr->setFloat("intensity", ssrSky.ssrIntensity);
                    ssr->setInt("steps", std::clamp(ssrSky.ssrSteps, 8, 256));
                    ssr->setInt("refineSteps", std::clamp(ssrSky.ssrRefineSteps, 0, 16));
                    ssr->setFloat("maxDistance", std::max(0.01f, ssrSky.ssrMaxDistance));
                    ssr->setFloat("thickness", std::max(0.001f, ssrSky.ssrThickness));
                    ssr->setFloat("maxRoughness", std::clamp(ssrSky.ssrMaxRoughness, 0.0f, 1.0f));
                    ssr->setVec2("screenSize", glm::vec2(static_cast<float>(width),
                                                         static_cast<float>(height)));

                    ssr->setInt("sceneColor", 0);
                    ssr->setInt("sceneDepth", 1);
                    ssr->setInt("sceneSurface", 12);
                    ssr->setInt("fallbackCube", 13);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, sceneColorTextureForPass);
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, sceneDepthTextureForPass);
                    glActiveTexture(GL_TEXTURE12);
                    glBindTexture(GL_TEXTURE_2D, surfaceTarget.texture);

                    const bool forwardAlreadyReflectsSky =
                        ssrSky.environmentReflections &&
                        ssrSky.environmentReflectionIntensity > 0.001f;
                    const bool hasCube = !forwardAlreadyReflectsSky &&
                                         skyboxReflectionTarget.cube != 0 &&
                                         skyboxReflectionTarget.hasCapture;
                    if (hasCube) {
                        glActiveTexture(GL_TEXTURE13);
                        glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxReflectionTarget.cube);
                    }
                    ssr->setBool("hasFallbackCube", hasCube);

                    recordFullscreenDraw();
                    drawFullscreenQuad();

                    if (compositeShader && compositeShader->ID != 0) {
                        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
                        glViewport(previousViewport[0], previousViewport[1],
                                   previousViewport[2], previousViewport[3]);
                        glEnable(GL_BLEND);
                        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
                        compositeShader->use();
                        compositeShader->setInt("sourceTex", 0);
                        compositeShader->setFloat("opacity", 1.0f);
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, ssrTarget.texture);
                        recordFullscreenDraw();
                        drawFullscreenQuad();
                    }
                }

                glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
                glViewport(previousViewport[0], previousViewport[1],
                           previousViewport[2], previousViewport[3]);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glDepthFunc(GL_LESS);
                glDepthMask(depthMask);
                glEnable(GL_DEPTH_TEST);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                if (blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
                if (cullFace) { glEnable(GL_CULL_FACE); glCullFace(prevCullMode); }
                else glDisable(GL_CULL_FACE);
            }
        }
    }

    if (skybox && !reflectionCapturePass && sceneDepthTextureForPass != 0 && !lights.empty()) {
        const SkyboxSettings& volSky = skybox->getSettings();
        bool anyShadowedLight = false;
        for (const auto& l : lights) {
            if (l.shadowKind > 0 && l.shadowMapIndex >= 0 && l.intensity > 0.0f) {
                anyShadowedLight = true;
                break;
            }
        }
        Shader* volumetric = nullptr;
        if (volSky.volumetricEnabled && volSky.volumetricIntensity > 0.0f &&
            volSky.volumetricDensity > 0.0f && anyShadowedLight && width > 0 && height > 0) {
            volumetric = getShader(postVertPath, volumetricFragPath);
        }
        if (volumetric && volumetric->ID != 0) {
            MODU_PROFILE_SCOPE("Volumetric Lighting", ProfilerSampleCategory::RenderDetail);
            volumetricShader = volumetric;

            GLint previousFramebuffer = 0;
            GLint previousViewport[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFramebuffer);
            glGetIntegerv(GL_VIEWPORT, previousViewport);

            const int volScale = volSky.volumetricHalfResolution ? 2 : 1;
            const int volWidth = std::max(1, width / volScale);
            const int volHeight = std::max(1, height / volScale);
            ensureRenderTarget(volumetricTarget, volWidth, volHeight, false, true, false);

            if (volumetricTarget.fbo != 0 && volumetricTarget.texture != 0) {
                glBindFramebuffer(GL_FRAMEBUFFER, volumetricTarget.fbo);
                glViewport(0, 0, volWidth, volHeight);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                glDisable(GL_CULL_FACE);

                volumetric->use();
                volumetric->setMat4("invViewProj", glm::inverse(proj * view));
                volumetric->setVec3("viewPos", camera.position);
                volumetric->setFloat("nearPlane", nearPlane);
                volumetric->setFloat("farPlane", farPlane);
                volumetric->setFloat("intensity", volSky.volumetricIntensity);
                volumetric->setInt("steps", std::clamp(volSky.volumetricSteps, 4, 256));
                volumetric->setFloat("scattering",
                                     std::clamp(volSky.volumetricScattering, -0.95f, 0.95f));
                volumetric->setFloat("density", std::max(0.0f, volSky.volumetricDensity));
                volumetric->setFloat("maxDistance", std::max(0.01f, volSky.volumetricMaxDistance));
                volumetric->setFloat("maxIntensity",
                                     std::max(0.0f, volSky.volumetricMaxIntensity));
                volumetric->setFloat("frameJitter",
                                     static_cast<float>(frameSerial % 64) * (1.0f / 64.0f));

                volumetric->setInt("sceneDepth", 2);
                volumetric->setInt("shadowCube0", 3);
                volumetric->setInt("shadowCube1", 4);
                volumetric->setInt("shadowCube2", 5);
                volumetric->setInt("shadowCube3", 6);
                volumetric->setInt("dirShadow0", 7);
                volumetric->setInt("dirShadow1", 8);
                volumetric->setInt("dirShadow2", 9);
                volumetric->setInt("dirShadow3", 10);

                volumetric->setInt("lightCount", static_cast<int>(lights.size()));
                for (size_t i = 0; i < lights.size() && i < kMaxLights; ++i) {
                    const auto& l = lights[i];
                    volumetric->setInt(kLightNames.type[i], l.type);
                    volumetric->setVec3(kLightNames.dir[i], l.dir);
                    volumetric->setVec3(kLightNames.pos[i], l.pos);
                    volumetric->setVec3(kLightNames.color[i], l.color);
                    volumetric->setFloat(kLightNames.intensity[i], l.intensity);
                    volumetric->setFloat(kLightNames.range[i], l.range);
                    volumetric->setFloat(kLightNames.innerCos[i], l.inner);
                    volumetric->setFloat(kLightNames.outerCos[i], l.outer);
                    volumetric->setInt(kLightNames.shadowMap[i], l.shadowMapIndex);
                    volumetric->setInt(kLightNames.shadowKind[i], l.shadowKind);
                    volumetric->setFloat(kLightNames.shadowBias[i], l.shadowBias);
                    volumetric->setFloat(kLightNames.shadowFar[i], l.shadowFar);
                    volumetric->setMat4(kLightNames.shadowMatrix[i], l.shadowMatrix);
                }

                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, sceneDepthTextureForPass);
                recordFullscreenDraw();
                drawFullscreenQuad();

                const int denoiseRadius = std::clamp(volSky.volumetricDenoiseRadius, 0, 8);
                if (volSky.volumetricDenoise && denoiseRadius > 0) {
                    Shader* blur = getShader(postVertPath, volumetricBlurFragPath);
                    if (blur && blur->ID != 0) {
                        ensureRenderTarget(volumetricBlurTarget, volWidth, volHeight,
                                           false, true, false);
                        if (volumetricBlurTarget.fbo != 0 && volumetricBlurTarget.texture != 0) {
                            blur->use();
                            blur->setInt("sourceTex", 0);
                            blur->setInt("sceneDepth", 2);
                            blur->setInt("radius", denoiseRadius);
                            blur->setFloat("nearPlane", nearPlane);
                            blur->setFloat("farPlane", farPlane);

                            const float texelX = 1.0f / static_cast<float>(std::max(volWidth, 1));
                            const float texelY = 1.0f / static_cast<float>(std::max(volHeight, 1));

                            glBindFramebuffer(GL_FRAMEBUFFER, volumetricBlurTarget.fbo);
                            glViewport(0, 0, volWidth, volHeight);
                            blur->setVec2("blurDirection", glm::vec2(texelX, 0.0f));
                            glActiveTexture(GL_TEXTURE0);
                            glBindTexture(GL_TEXTURE_2D, volumetricTarget.texture);
                            recordFullscreenDraw();
                            drawFullscreenQuad();

                            glBindFramebuffer(GL_FRAMEBUFFER, volumetricTarget.fbo);
                            glViewport(0, 0, volWidth, volHeight);
                            blur->setVec2("blurDirection", glm::vec2(0.0f, texelY));
                            glActiveTexture(GL_TEXTURE0);
                            glBindTexture(GL_TEXTURE_2D, volumetricBlurTarget.texture);
                            recordFullscreenDraw();
                            drawFullscreenQuad();
                        }
                    }
                }

                if (compositeShader && compositeShader->ID != 0) {
                    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
                    glViewport(previousViewport[0], previousViewport[1],
                               previousViewport[2], previousViewport[3]);
                    glEnable(GL_BLEND);
                    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
                    compositeShader->use();
                    compositeShader->setInt("sourceTex", 0);
                    compositeShader->setFloat("opacity", 1.0f);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, volumetricTarget.texture);
                    recordFullscreenDraw();
                    drawFullscreenQuad();
                }

                glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
                glViewport(previousViewport[0], previousViewport[1],
                           previousViewport[2], previousViewport[3]);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glEnable(GL_DEPTH_TEST);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                if (blendEnabled) {
                    glEnable(GL_BLEND);
                } else {
                    glDisable(GL_BLEND);
                }
                if (cullFace) {
                    glEnable(GL_CULL_FACE);
                    glCullFace(prevCullMode);
                } else {
                    glDisable(GL_CULL_FACE);
                }
            }
        }
    }

    if (optPass) {
        OptimizationState::finishPass(*this, *optPass, optPassStats);
    }

    if (unbindFramebuffer) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

Renderer::ResolvedPostFX Renderer::gatherPostFX(const Camera& camera, const std::vector<SceneObject>& sceneObjects) const {
    ResolvedPostFX result;
    result.settings = MakeNeutralPostFXSettings();
    result.settings.hdrEnabled = false;

    const SceneObject* resolvedVolume = nullptr;
    float resolvedPriority = -FLT_MAX;
    float resolvedSpatialWeight = 0.0f;
    int resolvedIndex = -1;

    for (size_t i = 0; i < sceneObjects.size(); ++i) {
        const SceneObject& obj = sceneObjects[i];
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasPostFX || !obj.postFx.enabled) continue;

        const float spatialWeight =
            ComputePostFXVolumeSpatialWeight(obj, camera.position);
        const float blendWeight =
            glm::clamp(obj.postFx.blendWeight, 0.0f, 1.0f);
        const float polarization =
            glm::clamp(obj.postFx.distortionPolarization, 0.0f, 1.0f);
        const float polarizationVisibility =
            (polarization > 0.0001f && polarization < 0.9999f)
                ? 1.0f - polarization
                : 0.0f;
        if (spatialWeight *
                std::max(blendWeight, polarizationVisibility) <=
            0.0001f) {
            continue;
        }

        result.activeVolumeCount += 1;
        if (!resolvedVolume ||
            obj.postFx.priority > resolvedPriority ||
            (obj.postFx.priority == resolvedPriority && static_cast<int>(i) >= resolvedIndex)) {
            resolvedVolume = &obj;
            resolvedPriority = obj.postFx.priority;
            resolvedSpatialWeight = spatialWeight;
            resolvedIndex = static_cast<int>(i);
        }
    }

    if (!resolvedVolume) {
        return result;
    }

    PostFXSettings settings = MakeNeutralPostFXSettings();
    const float polarization =
        glm::clamp(resolvedVolume->postFx.distortionPolarization, 0.0f, 1.0f);
    const float weight =
        glm::clamp(resolvedSpatialWeight * polarization, 0.0f, 1.0f);
    settings.enabled = true;
    settings.isGlobal = resolvedVolume->postFx.isGlobal;
    settings.priority = resolvedVolume->postFx.priority;
    settings.blendWeight = weight;
    settings.blendRadius = resolvedVolume->postFx.blendRadius;
    settings.hdrEnabled = resolvedVolume->postFx.hdrEnabled;
    settings.toneMapper = resolvedVolume->postFx.toneMapper;
    settings.whitePoint = glm::mix(settings.whitePoint, resolvedVolume->postFx.whitePoint, weight);
    settings.gamma = glm::mix(settings.gamma, resolvedVolume->postFx.gamma, weight);

    if (resolvedVolume->postFx.bloomEnabled) {
        settings.bloomEnabled = true;
        settings.bloomThreshold = glm::mix(settings.bloomThreshold, resolvedVolume->postFx.bloomThreshold, weight);
        settings.bloomSoftKnee = glm::mix(settings.bloomSoftKnee, resolvedVolume->postFx.bloomSoftKnee, weight);
        settings.bloomIntensity = glm::mix(0.0f, resolvedVolume->postFx.bloomIntensity, weight);
        settings.bloomRadius = glm::mix(settings.bloomRadius, resolvedVolume->postFx.bloomRadius, weight);
    }
    if (resolvedVolume->postFx.colorAdjustEnabled) {
        settings.colorAdjustEnabled = true;
        settings.exposure = glm::mix(settings.exposure, resolvedVolume->postFx.exposure, weight);
        settings.contrast = glm::mix(settings.contrast, resolvedVolume->postFx.contrast, weight);
        settings.saturation = glm::mix(settings.saturation, resolvedVolume->postFx.saturation, weight);
        settings.colorFilter = glm::mix(settings.colorFilter, resolvedVolume->postFx.colorFilter, weight);
    }
    if (resolvedVolume->postFx.motionBlurEnabled) {
        settings.motionBlurEnabled = true;
        settings.motionBlurStrength = glm::mix(0.0f, resolvedVolume->postFx.motionBlurStrength, weight);
        settings.motionBlurThreshold = glm::mix(settings.motionBlurThreshold, resolvedVolume->postFx.motionBlurThreshold, weight);
        settings.motionBlurClamp = glm::mix(settings.motionBlurClamp, resolvedVolume->postFx.motionBlurClamp, weight);
    }
    if (resolvedVolume->postFx.lensFlareEnabled) {
        settings.lensFlareEnabled = true;
        settings.lensFlareIntensity = glm::mix(0.0f, resolvedVolume->postFx.lensFlareIntensity, weight);
        settings.lensFlareThreshold = glm::mix(settings.lensFlareThreshold, resolvedVolume->postFx.lensFlareThreshold, weight);
        settings.lensFlareGhosts = resolvedVolume->postFx.lensFlareGhosts;
        settings.lensFlareGhostSpacing = glm::mix(settings.lensFlareGhostSpacing, resolvedVolume->postFx.lensFlareGhostSpacing, weight);
        settings.lensFlareHaloRadius = glm::mix(settings.lensFlareHaloRadius, resolvedVolume->postFx.lensFlareHaloRadius, weight);
        settings.lensFlareStreak = glm::mix(settings.lensFlareStreak, resolvedVolume->postFx.lensFlareStreak, weight);
        settings.lensFlareTint = glm::mix(settings.lensFlareTint, resolvedVolume->postFx.lensFlareTint, weight);
        settings.lensFlareGhostsEnabled = resolvedVolume->postFx.lensFlareGhostsEnabled;
        settings.lensFlareHaloEnabled = resolvedVolume->postFx.lensFlareHaloEnabled;
        settings.lensFlareStreakEnabled = resolvedVolume->postFx.lensFlareStreakEnabled;
        settings.lensFlareGlowEnabled = resolvedVolume->postFx.lensFlareGlowEnabled;
        settings.lensFlareGlowSize = glm::mix(settings.lensFlareGlowSize, resolvedVolume->postFx.lensFlareGlowSize, weight);
        settings.lensFlareGlowIntensity = glm::mix(0.0f, resolvedVolume->postFx.lensFlareGlowIntensity, weight);
    }

    if (resolvedVolume->postFx.vignetteEnabled) {
        settings.vignetteEnabled = true;
        settings.vignetteIntensity = glm::mix(0.0f, resolvedVolume->postFx.vignetteIntensity, weight);
        settings.vignetteSmoothness = glm::mix(settings.vignetteSmoothness, resolvedVolume->postFx.vignetteSmoothness, weight);
    }
    if (resolvedVolume->postFx.chromaticAberrationEnabled) {
        settings.chromaticAberrationEnabled = true;
        settings.chromaticAmount = glm::mix(0.0f, resolvedVolume->postFx.chromaticAmount, weight);
    }
    if (resolvedVolume->postFx.sharpenEnabled) {
        settings.sharpenEnabled = true;
        settings.sharpenStrength = glm::mix(0.0f, resolvedVolume->postFx.sharpenStrength, weight);
    }
    if (resolvedVolume->postFx.ambientOcclusionEnabled) {
        settings.ambientOcclusionEnabled = true;
        settings.aoRadius = glm::mix(settings.aoRadius, resolvedVolume->postFx.aoRadius, weight);
        settings.aoStrength = glm::mix(0.0f, resolvedVolume->postFx.aoStrength, weight);
    }
    if (resolvedVolume->postFx.ditherEnabled) {
        settings.ditherEnabled = true;
        settings.ditherIntensity = glm::mix(0.0f, resolvedVolume->postFx.ditherIntensity, weight);
        settings.ditherColorBits = resolvedVolume->postFx.ditherColorBits;
        settings.ditherDarkAdjustment = glm::mix(0.0f, resolvedVolume->postFx.ditherDarkAdjustment, weight);
        settings.ditherPixelation = glm::mix(0.0f, resolvedVolume->postFx.ditherPixelation, weight);
        settings.ditherSize = glm::mix(1.0f, resolvedVolume->postFx.ditherSize, weight);
        settings.ditherContrast = glm::mix(0.0f, resolvedVolume->postFx.ditherContrast, weight);
        settings.ditherOffset = glm::mix(0.0f, resolvedVolume->postFx.ditherOffset, weight);
        settings.ditherPalette = resolvedVolume->postFx.ditherPalette;
        settings.ditherPattern = resolvedVolume->postFx.ditherPattern;
    }
    if (resolvedVolume->postFx.staticEnabled) {
        settings.staticEnabled = true;
        settings.staticIntensity = glm::mix(0.0f, resolvedVolume->postFx.staticIntensity, weight);
        settings.staticGrainScale = glm::mix(settings.staticGrainScale, resolvedVolume->postFx.staticGrainScale, weight);
        settings.staticDarkAreaInfluence = glm::mix(0.0f, resolvedVolume->postFx.staticDarkAreaInfluence, weight);
        settings.staticSpeed = glm::mix(0.0f, resolvedVolume->postFx.staticSpeed, weight);
        settings.staticMonochrome = resolvedVolume->postFx.staticMonochrome;
        settings.staticSparkle = glm::mix(0.0f, resolvedVolume->postFx.staticSparkle, weight);
    }
    if (resolvedVolume->postFx.staticDistortionEnabled) {
        settings.staticDistortionEnabled = true;
        settings.staticDistortionHorizontalJitterAmount =
            glm::mix(0.0f, resolvedVolume->postFx.staticDistortionHorizontalJitterAmount, weight);
        settings.staticDistortionLineDensity =
            glm::mix(settings.staticDistortionLineDensity, resolvedVolume->postFx.staticDistortionLineDensity, weight);
        settings.staticDistortionGlitchFrequency =
            glm::mix(0.0f, resolvedVolume->postFx.staticDistortionGlitchFrequency, weight);
        settings.staticDistortionStrength =
            glm::mix(0.0f, resolvedVolume->postFx.staticDistortionStrength, weight);
    }
    if (resolvedVolume->postFx.lensDistortionEnabled) {
        settings.lensDistortionEnabled = true;
        settings.lensDistortionAmount = glm::mix(0.0f, resolvedVolume->postFx.lensDistortionAmount, weight);
        settings.lensDistortionEdgeFalloff =
            glm::mix(settings.lensDistortionEdgeFalloff, resolvedVolume->postFx.lensDistortionEdgeFalloff, weight);
        settings.lensDistortionCenterOffset =
            glm::mix(settings.lensDistortionCenterOffset, resolvedVolume->postFx.lensDistortionCenterOffset, weight);
        settings.lensDistortionEdgeVignetteEnabled =
            resolvedVolume->postFx.lensDistortionEdgeVignetteEnabled;
        settings.lensDistortionEdgeVignetteIntensity =
            glm::mix(0.0f, resolvedVolume->postFx.lensDistortionEdgeVignetteIntensity, weight);
        settings.lensDistortionEdgeVignetteRadius =
            glm::mix(settings.lensDistortionEdgeVignetteRadius,
                     resolvedVolume->postFx.lensDistortionEdgeVignetteRadius, weight);
        settings.lensDistortionEdgeVignetteSoftness =
            glm::mix(settings.lensDistortionEdgeVignetteSoftness,
                     resolvedVolume->postFx.lensDistortionEdgeVignetteSoftness, weight);
        settings.lensDistortionEdgeVignetteColor =
            glm::mix(settings.lensDistortionEdgeVignetteColor,
                     resolvedVolume->postFx.lensDistortionEdgeVignetteColor, weight);
    }
    if (resolvedVolume->postFx.pixelationEnabled) {
        settings.pixelationEnabled = true;
        settings.pixelationSize =
            glm::mix(settings.pixelationSize, resolvedVolume->postFx.pixelationSize, weight);
    }
    if (resolvedVolume->postFx.posterizeEnabled) {
        settings.posterizeEnabled = true;
        settings.posterizeLevels = resolvedVolume->postFx.posterizeLevels;
    }
    if (resolvedVolume->postFx.scanlinesEnabled) {
        settings.scanlinesEnabled = true;
        settings.scanlinesIntensity =
            glm::mix(0.0f, resolvedVolume->postFx.scanlinesIntensity, weight);
        settings.scanlinesDensity =
            glm::mix(settings.scanlinesDensity, resolvedVolume->postFx.scanlinesDensity, weight);
        settings.scanlinesSpeed =
            glm::mix(0.0f, resolvedVolume->postFx.scanlinesSpeed, weight);
    }
    if (resolvedVolume->postFx.vhsOverlayEnabled) {
        settings.vhsOverlayEnabled = true;
        settings.vhsOverlayOpacity = glm::mix(0.0f, resolvedVolume->postFx.vhsOverlayOpacity, weight);
        settings.vhsOverlayScanlineStrength =
            glm::mix(0.0f, resolvedVolume->postFx.vhsOverlayScanlineStrength, weight);
        settings.vhsOverlayTapeNoise = glm::mix(0.0f, resolvedVolume->postFx.vhsOverlayTapeNoise, weight);
        settings.vhsOverlayChromaBleed = glm::mix(0.0f, resolvedVolume->postFx.vhsOverlayChromaBleed, weight);
        settings.vhsOverlayBottomNoiseBandHeight =
            glm::mix(0.0f, resolvedVolume->postFx.vhsOverlayBottomNoiseBandHeight, weight);
        settings.vhsOverlayBottomNoiseBandIntensity =
            glm::mix(0.0f, resolvedVolume->postFx.vhsOverlayBottomNoiseBandIntensity, weight);
        settings.vhsOverlayDistortionStrength =
            glm::mix(0.0f, resolvedVolume->postFx.vhsOverlayDistortionStrength, weight);
        settings.vhsOverlayAnimationSpeed =
            glm::mix(0.0f, resolvedVolume->postFx.vhsOverlayAnimationSpeed, weight);
        settings.vhsOverlayColorBleed =
            glm::mix(0.0f, resolvedVolume->postFx.vhsOverlayColorBleed, weight);
        settings.vhsOverlayBanding =
            glm::mix(0.0f, resolvedVolume->postFx.vhsOverlayBanding, weight);
        settings.vhsOverlaySignalMode = resolvedVolume->postFx.vhsOverlaySignalMode;
        settings.vhsOverlayDropouts =
            glm::mix(0.0f, resolvedVolume->postFx.vhsOverlayDropouts, weight);
    }
    if (resolvedVolume->postFx.wavyEnabled) {
        settings.wavyEnabled = true;
        settings.wavyAmplitude = glm::mix(0.0f, resolvedVolume->postFx.wavyAmplitude, weight);
        settings.wavyFrequency = glm::mix(settings.wavyFrequency, resolvedVolume->postFx.wavyFrequency, weight);
        settings.wavySpeed = glm::mix(0.0f, resolvedVolume->postFx.wavySpeed, weight);
        settings.wavyVertical = resolvedVolume->postFx.wavyVertical;
    }

    const float polarizationVisibility =
        (polarization > 0.0001f && polarization < 0.9999f)
            ? (1.0f - polarization) * resolvedSpatialWeight
            : 0.0f;
    settings.blendWeight =
        glm::clamp(polarizationVisibility, 0.0f, 1.0f);
    result.polarizationSettings = settings;
    result.hasPolarizationLayer = settings.blendWeight > 0.0001f;

    PostFXSettings fullSettings = resolvedVolume->postFx;
    fullSettings.blendWeight =
        glm::clamp(resolvedVolume->postFx.blendWeight *
                       resolvedSpatialWeight,
                   0.0f, 1.0f);
    AttenuatePostFXStrengths(fullSettings, fullSettings.blendWeight);
    result.settings = fullSettings;
    result.resolvedVolumeId = resolvedVolume->id;
    result.resolvedVolumeName = resolvedVolume->name;
    result.resolvedBlend = fullSettings.blendWeight;
    return result;
}

PostFXSettings Renderer::resolvePostFXSettings(const Camera& camera, const std::vector<SceneObject>& sceneObjects) const {
    return gatherPostFX(camera, sceneObjects).settings;
}

Renderer::PostFX2DScope Renderer::resolvePostFX2DScope(
    const Camera& camera, const std::vector<SceneObject>& sceneObjects) const {
    PostFX2DScope scope;
    const ResolvedPostFX resolved = gatherPostFX(camera, sceneObjects);
    if (!resolved.settings.enabled || resolved.activeVolumeCount <= 0 ||
        !resolved.settings.scope2DEnabled) {
        return scope;
    }
    scope.active = true;
    scope.mode = resolved.settings.scope2DMode;
    if (scope.mode == PostFX2DScopeMode::Band) {
        scope.minOrder = std::min(resolved.settings.scope2DMinOrder,
                                  resolved.settings.scope2DMaxOrder);
        scope.maxOrder = std::max(resolved.settings.scope2DMinOrder,
                                  resolved.settings.scope2DMaxOrder);
    } else {
        scope.minOrder = std::numeric_limits<int>::min();
        scope.maxOrder = resolved.settings.scope2DMaxOrder;
    }
    return scope;
}

unsigned int Renderer::postProcessTexture(const Camera& camera,
                                          const std::vector<SceneObject>& sceneObjects,
                                          unsigned int sourceTexture,
                                          int width,
                                          int height,
                                          bool allowHistory) {
    return applyPostProcessing(camera, sceneObjects, sourceTexture, width, height, allowHistory);
}

bool Renderer::postProcessFramebufferRegion(
    const Camera& camera, const std::vector<SceneObject>& sceneObjects,
    int x, int y, int width, int height, bool allowHistory) {
    if (!moduRenderContextLive() || width <= 0 || height <= 0) {
        return false;
    }

    const ResolvedPostFX resolved = gatherPostFX(camera, sceneObjects);
    if (!resolved.settings.enabled || resolved.activeVolumeCount <= 0) {
        return false;
    }

    GLint previousReadFramebuffer = 0;
    GLint previousDrawFramebuffer = 0;
    GLint previousViewport[4] = {0, 0, 0, 0};
    GLint previousScissorBox[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_SCISSOR_BOX, previousScissorBox);
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);

    ensureRenderTarget(overlayCaptureTarget, width, height, false, false, false);
    if (overlayCaptureTarget.fbo == 0 || overlayCaptureTarget.texture == 0) {
        return false;
    }

    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, overlayCaptureTarget.fbo);
    glBlitFramebuffer(x, y, x + width, y + height,
                      0, 0, width, height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    const unsigned int processed =
        applyPostProcessing(camera, sceneObjects, overlayCaptureTarget.texture,
                            width, height, allowHistory);
    const unsigned int presentedTexture =
        processed != 0 ? processed : overlayCaptureTarget.texture;
    const unsigned int presentedFramebuffer =
        findFramebufferForTexture(presentedTexture);
    if (presentedFramebuffer != 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, presentedFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFramebuffer);
        glBlitFramebuffer(0, 0, width, height,
                          x, y, x + width, y + height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFramebuffer);
    glViewport(previousViewport[0], previousViewport[1],
               previousViewport[2], previousViewport[3]);
    glScissor(previousScissorBox[0], previousScissorBox[1],
              previousScissorBox[2], previousScissorBox[3]);
    if (scissorEnabled) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    return presentedFramebuffer != 0;
}

bool Renderer::beginBandPostFxRegion(int x, int y, int width, int height,
                                     int framebufferWidth, int framebufferHeight) {
    if (!moduRenderContextLive() || bandRegionActive ||
        width <= 0 || height <= 0 ||
        framebufferWidth <= 0 || framebufferHeight <= 0) {
        return false;
    }

    GLint previousDrawFramebuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);

    ensureRenderTarget(bandLayerTarget, framebufferWidth, framebufferHeight,
                       true, false, false);
    if (bandLayerTarget.fbo == 0 || bandLayerTarget.texture == 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
        return false;
    }

    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    GLint previousScissorBox[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_SCISSOR_BOX, previousScissorBox);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bandLayerTarget.fbo);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, framebufferWidth, framebufferHeight);
    GLfloat previousClearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(previousClearColor[0], previousClearColor[1],
                 previousClearColor[2], previousClearColor[3]);
    glScissor(previousScissorBox[0], previousScissorBox[1],
              previousScissorBox[2], previousScissorBox[3]);
    if (scissorEnabled) {
        glEnable(GL_SCISSOR_TEST);
    }

    bandRegionX = x;
    bandRegionY = y;
    bandRegionWidth = width;
    bandRegionHeight = height;
    bandPreviousFramebuffer = static_cast<int>(previousDrawFramebuffer);
    bandRegionActive = true;
    return true;
}

bool Renderer::endBandPostFxRegion(const Camera& camera,
                                   const std::vector<SceneObject>& sceneObjects,
                                   bool allowHistory) {
    if (!bandRegionActive) {
        return false;
    }
    bandRegionActive = false;

    const int x = bandRegionX;
    const int y = bandRegionY;
    const int width = bandRegionWidth;
    const int height = bandRegionHeight;
    const GLuint previousFramebuffer = static_cast<GLuint>(bandPreviousFramebuffer);

    if (!moduRenderContextLive()) {
        return false;
    }

    GLint previousViewport[4] = {0, 0, 0, 0};
    GLint previousScissorBox[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_SCISSOR_BOX, previousScissorBox);
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    const GLboolean depthEnabled = glIsEnabled(GL_DEPTH_TEST);

    glDisable(GL_SCISSOR_TEST);

    ensureRenderTarget(bandCaptureTarget, width, height, true, false, false);
    if (bandCaptureTarget.fbo == 0 || bandCaptureTarget.texture == 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
        return false;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, bandLayerTarget.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bandCaptureTarget.fbo);
    glBlitFramebuffer(x, y, x + width, y + height,
                      0, 0, width, height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    postFxPreserveAlpha = true;
    const unsigned int processed =
        applyPostProcessing(camera, sceneObjects, bandCaptureTarget.texture,
                            width, height, allowHistory);
    postFxPreserveAlpha = false;
    const unsigned int presentedTexture =
        processed != 0 ? processed : bandCaptureTarget.texture;

    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
    bool composited = false;
    if (compositeShader && compositeShader->ID != 0) {
        glViewport(x, y, width, height);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        compositeShader->use();
        compositeShader->setInt("sourceTex", 0);
        compositeShader->setFloat("opacity", 1.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, presentedTexture);
        drawFullscreenQuad();
        composited = true;
    }

    glViewport(previousViewport[0], previousViewport[1],
               previousViewport[2], previousViewport[3]);
    glScissor(previousScissorBox[0], previousScissorBox[1],
              previousScissorBox[2], previousScissorBox[3]);
    if (scissorEnabled) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    if (blendEnabled) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
    if (depthEnabled) {
        glEnable(GL_DEPTH_TEST);
    }
    return composited;
}

unsigned int Renderer::applyPostProcessing(const Camera& camera, const std::vector<SceneObject>& sceneObjects, unsigned int sourceTexture, int width, int height, bool allowHistory) {
    MODU_PROFILE_SCOPE("Post Processing", ProfilerSampleCategory::RenderDetail);
    using Clock = std::chrono::steady_clock;

    const auto totalStart = Clock::now();
    ResolvedPostFX resolved;
    PostFXSettings settings;
    if (postFxSettingsOverrideActive) {
        settings = postFxSettingsOverride;
        resolved.settings = settings;
        resolved.activeVolumeCount = 1;
        resolved.resolvedBlend = settings.blendWeight;
    } else {
        MODU_PROFILE_SCOPE("PostFX Resolve", ProfilerSampleCategory::RenderDetail);
        resolved = gatherPostFX(camera, sceneObjects);
        settings = resolved.settings;
        if (resolved.hasPolarizationLayer && sourceTexture != 0 &&
            width > 0 && height > 0) {
            postFxSettingsOverrideActive = true;
            postFxSettingsOverride = resolved.polarizationSettings;
            postFxTargetOverride = &polarizationTarget;
            const unsigned int polarizedTexture =
                applyPostProcessing(camera, sceneObjects, sourceTexture,
                                    width, height, false);
            postFxTargetOverride = nullptr;
            postFxSettingsOverrideActive = false;
            if (polarizedTexture != 0) {
                sourceTexture = polarizedTexture;
            }
        }
    }

    PostProcessStats& postStats = allowHistory ? viewportPostStats : previewPostStats;
    postStats = {};
    postStats.sourceTextureId = sourceTexture;
    postStats.sourceFramebufferId = findFramebufferForTexture(sourceTexture);
    postStats.activeVolumeCount = resolved.activeVolumeCount;
    postStats.resolvedVolumeId = resolved.resolvedVolumeId;
    postStats.resolvedVolumeName = resolved.resolvedVolumeName;
    postStats.resolvedBlend = resolved.resolvedBlend;
    postStats.activeEffectCount = CountEnabledPostEffects(settings);
    postStats.hdrEnabled = settings.hdrEnabled;

    bool wantsEffects = settings.enabled &&
        (HasMeaningfulToneMapping(settings) || settings.bloomEnabled || settings.colorAdjustEnabled ||
         settings.motionBlurEnabled || settings.vignetteEnabled || settings.lensFlareEnabled ||
         settings.chromaticAberrationEnabled || settings.sharpenEnabled ||
         settings.ambientOcclusionEnabled || settings.ditherEnabled ||
         settings.staticEnabled || settings.staticDistortionEnabled ||
         settings.lensDistortionEnabled || settings.pixelationEnabled ||
         settings.posterizeEnabled || settings.scanlinesEnabled ||
         settings.vhsOverlayEnabled ||
         settings.wavyEnabled);

    if (wantsEffects) {
#if MODULARITY_OPENGL_ES
        bool wireframe = false;
#else
        GLint polygonMode[2] = { GL_FILL, GL_FILL };
#ifdef GL_POLYGON_MODE
        glGetIntegerv(GL_POLYGON_MODE, polygonMode);
#endif
        bool wireframe = (polygonMode[0] == GL_LINE || polygonMode[1] == GL_LINE);
#endif
        if (wireframe) {
            wantsEffects = false;
            postStats.activeEffectCount = 0;
            postStats.hdrEnabled = false;
            postStats.skipReason = "wireframe_or_line_mode";
        }
    }

    if (!wantsEffects || !postShader || width <= 0 || height <= 0 || sourceTexture == 0) {
        if (postStats.skipReason.empty()) {
            if (!wantsEffects) {
                postStats.skipReason = "no_visible_effects";
            } else if (!postShader) {
                postStats.skipReason = "post_shader_unavailable";
            } else if (width <= 0 || height <= 0) {
                postStats.skipReason = "invalid_target_size";
            } else {
                postStats.skipReason = "missing_source_texture";
            }
        }
        postStats.totalMs = std::chrono::duration<float, std::milli>(Clock::now() - totalStart).count();
        postStats.finalPresentedTextureId = sourceTexture;
        postStats.finalPresentedFramebufferId = postStats.sourceFramebufferId;
        postStats.finalTextureDiffersFromSource = false;
        if (allowHistory) {
            displayTexture = sourceTexture;
            clearHistory();
        }
        logPostFxDebug(postStats, allowHistory);
        return sourceTexture;
    }

    RenderTarget& target = postFxTargetOverride
                               ? *postFxTargetOverride
                               : (allowHistory ? postTarget
                                               : (postFxUseExternalTarget ? externalPostTarget
                                                                          : previewPostTarget));
    ensureRenderTarget(target, width, height, true, true);
    if (allowHistory) {
        ensureRenderTarget(historyTarget, width, height, false, true);
    }
    ensureRenderTarget(bloomTargetA, width, height, false, true);
    ensureRenderTarget(bloomTargetB, width, height, false, true);
    if (target.fbo == 0 || target.texture == 0) {
        postStats.skipReason = "post_target_unavailable";
        postStats.totalMs = std::chrono::duration<float, std::milli>(Clock::now() - totalStart).count();
        postStats.finalPresentedTextureId = sourceTexture;
        postStats.finalPresentedFramebufferId = postStats.sourceFramebufferId;
        postStats.finalTextureDiffersFromSource = false;
        if (allowHistory) {
            displayTexture = sourceTexture;
            clearHistory();
        }
        logPostFxDebug(postStats, allowHistory);
        return sourceTexture;
    }

    postStats.executionBegan = true;
    unsigned int bloomTex = 0;
    if (settings.bloomEnabled && brightShader && blurShader) {
        {
            MODU_PROFILE_SCOPE("Bloom Extract", ProfilerSampleCategory::RenderDetail);
            const auto bloomExtractStart = Clock::now();
            glDisable(GL_DEPTH_TEST);
            brightShader->use();
            brightShader->setFloat("threshold", settings.bloomThreshold);
            brightShader->setFloat("softKnee", settings.bloomSoftKnee);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, sourceTexture);
            glBindFramebuffer(GL_FRAMEBUFFER, bloomTargetA.fbo);
            glViewport(0, 0, width, height);
            glClear(GL_COLOR_BUFFER_BIT);
            drawFullscreenQuad();
            Modu2DStats::CountPostFxPass();
            postStats.bloomExtractDestinationTextureId = bloomTargetA.texture;
            postStats.bloomExtractDestinationFramebufferId = bloomTargetA.fbo;
            postStats.bloomExtractMs = std::chrono::duration<float, std::milli>(Clock::now() - bloomExtractStart).count();
        }

        unsigned int pingTex = bloomTargetA.texture;
        {
            MODU_PROFILE_SCOPE("Bloom Blur", ProfilerSampleCategory::RenderDetail);
            const auto bloomBlurStart = Clock::now();
            blurShader->use();
            float sigma = glm::max(settings.bloomRadius * 2.5f, 0.1f);
            int radius = static_cast<int>(glm::clamp(settings.bloomRadius * 4.0f, 2.0f, 12.0f));
            blurShader->setFloat("sigma", sigma);
            blurShader->setInt("radius", radius);

            bool horizontal = true;
            RenderTarget* writeTarget = &bloomTargetB;
            for (int i = 0; i < 4; ++i) {
                blurShader->setBool("horizontal", horizontal);
                blurShader->setVec2("texelSize", glm::vec2(1.0f / width, 1.0f / height));
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, pingTex);
                glBindFramebuffer(GL_FRAMEBUFFER, writeTarget->fbo);
                glViewport(0, 0, width, height);
                glClear(GL_COLOR_BUFFER_BIT);
                drawFullscreenQuad();
                Modu2DStats::CountPostFxPass();

                pingTex = writeTarget->texture;
                writeTarget = (writeTarget == &bloomTargetA) ? &bloomTargetB : &bloomTargetA;
                horizontal = !horizontal;
            }
            postStats.bloomBlurMs = std::chrono::duration<float, std::milli>(Clock::now() - bloomBlurStart).count();
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glEnable(GL_DEPTH_TEST);
        bloomTex = pingTex;
        postStats.bloomBlurTextureId = bloomTex;
        postStats.bloomBlurFramebufferId = findFramebufferForTexture(bloomTex);
        postStats.bloomUsed = true;
    } else {
        bloomTex = 0;
        clearTarget(bloomTargetA);
        clearTarget(bloomTargetB);
    }

    const bool hasHistory = allowHistory && historyValid;
    postStats.motionBlurUsed = settings.motionBlurEnabled && hasHistory;

    {
        MODU_PROFILE_SCOPE("PostFX Composite", ProfilerSampleCategory::RenderDetail);
        const auto compositeStart = Clock::now();
        glDisable(GL_DEPTH_TEST);
        postShader->use();
        postShader->setFloat("effectBlendWeight",
                             std::clamp(settings.blendWeight, 0.0f, 1.0f));
        postShader->setBool("preserveAlpha", postFxPreserveAlpha);
        postShader->setBool("enableHDR", settings.hdrEnabled);
        postShader->setInt("toneMapper", static_cast<int>(settings.toneMapper));
        postShader->setFloat("whitePoint", settings.whitePoint);
        postShader->setFloat("gamma", settings.gamma);
        postShader->setBool("enableBloom", settings.bloomEnabled && bloomTex != 0);
        postShader->setFloat("bloomIntensity", settings.bloomIntensity);
        postShader->setBool("enableColorAdjust", settings.colorAdjustEnabled);
        postShader->setFloat("exposure", settings.exposure);
        postShader->setFloat("autoExposure",
                             updateEyeAdaptation(sourceTexture, width, height, settings));
        const SkyboxSettings& weatherSky = skybox ? skybox->getSettings() : SkyboxSettings{};
        postShader->setFloat("rainAmount", weatherDrive.active ? weatherDrive.rainAmount : 0.0f);
        postShader->setFloat("rainOpacity", weatherSky.weather.rainOpacity);
        postShader->setFloat("rainSpeed", weatherSky.weather.rainSpeed);
        postShader->setFloat("rainScale", weatherSky.weather.rainScale);
        postShader->setFloat("rainTime", weatherTime);
        postShader->setFloat("lightningFlash", weatherDrive.active ? weatherDrive.flash : 0.0f);
        postShader->setBool("purkinjeEnabled", settings.purkinjeEnabled);
        postShader->setFloat("purkinjeStrength", settings.purkinjeStrength);
        postShader->setFloat("purkinjeThreshold", settings.purkinjeThreshold);
        postShader->setFloat("purkinjeRange", settings.purkinjeRange);
        postShader->setVec3("purkinjeTint", settings.purkinjeTint);
        postShader->setFloat("contrast", settings.contrast);
        postShader->setFloat("saturation", settings.saturation);
        postShader->setVec3("colorFilter", settings.colorFilter);
        postShader->setBool("enableMotionBlur", settings.motionBlurEnabled);
        postShader->setFloat("motionBlurStrength", settings.motionBlurStrength);
        postShader->setFloat("motionBlurThreshold", settings.motionBlurThreshold);
        postShader->setFloat("motionBlurClamp", settings.motionBlurClamp);
        postShader->setBool("hasHistory", hasHistory);
        postShader->setBool("enableVignette", settings.vignetteEnabled);
        postShader->setBool("enableLensFlare", settings.lensFlareEnabled);
        postShader->setFloat("lensFlareIntensity", std::max(0.0f, settings.lensFlareIntensity));
        postShader->setFloat("lensFlareThreshold", std::max(0.0f, settings.lensFlareThreshold));
        postShader->setInt("lensFlareGhosts", std::clamp(settings.lensFlareGhosts, 0, 8));
        postShader->setFloat("lensFlareGhostSpacing", settings.lensFlareGhostSpacing);
        postShader->setFloat("lensFlareHaloRadius", std::max(0.0f, settings.lensFlareHaloRadius));
        postShader->setFloat("lensFlareStreak", std::max(0.0f, settings.lensFlareStreak));
        postShader->setVec3("lensFlareTint", settings.lensFlareTint);
        postShader->setBool("lensFlareGhostsEnabled", settings.lensFlareGhostsEnabled);
        postShader->setBool("lensFlareHaloEnabled", settings.lensFlareHaloEnabled);
        postShader->setBool("lensFlareStreakEnabled", settings.lensFlareStreakEnabled);
        postShader->setBool("lensFlareGlowEnabled", settings.lensFlareGlowEnabled);
        postShader->setFloat("lensFlareGlowSize", std::max(0.0001f, settings.lensFlareGlowSize));
        postShader->setFloat("lensFlareGlowIntensity", std::max(0.0f, settings.lensFlareGlowIntensity));
        postShader->setVec2("lensFlareSunUv", sunScreenUv);
        postShader->setBool("lensFlareSunOnScreen", sunOnScreen);
        postShader->setFloat("lensFlareAspect",
                             (height > 0) ? (static_cast<float>(width) / static_cast<float>(height)) : 1.0f);
        postShader->setFloat("vignetteIntensity", settings.vignetteIntensity);
        postShader->setFloat("vignetteSmoothness", settings.vignetteSmoothness);
        postShader->setBool("enableChromatic", settings.chromaticAberrationEnabled);
        postShader->setFloat("chromaticAmount", settings.chromaticAmount);
        postShader->setBool("enableSharpen", settings.sharpenEnabled);
        postShader->setFloat("sharpenStrength", settings.sharpenStrength);
        postShader->setBool("enableAO", settings.ambientOcclusionEnabled);
        postShader->setFloat("aoRadius", settings.aoRadius);
        postShader->setFloat("aoStrength", settings.aoStrength);
        postShader->setBool("enableDither", settings.ditherEnabled);
        postShader->setFloat("ditherIntensity", settings.ditherIntensity);
        postShader->setInt("ditherColorBits", std::clamp(settings.ditherColorBits, 1, 8));
        postShader->setFloat("ditherDarkAdjustment", settings.ditherDarkAdjustment);
        postShader->setFloat("ditherPixelation", settings.ditherPixelation);
        postShader->setFloat("ditherSize", std::clamp(settings.ditherSize, 1.0f, 8.0f));
        postShader->setFloat("ditherContrast", std::clamp(settings.ditherContrast, -1.0f, 1.0f));
        postShader->setFloat("ditherOffset", std::clamp(settings.ditherOffset, -1.0f, 1.0f));
        postShader->setInt("ditherPalette", static_cast<int>(settings.ditherPalette));
        postShader->setInt("ditherPattern", static_cast<int>(settings.ditherPattern));
        postShader->setBool("enableStatic", settings.staticEnabled);
        postShader->setFloat("staticIntensity", settings.staticIntensity);
        postShader->setFloat("staticGrainScale", std::max(0.01f, settings.staticGrainScale));
        postShader->setFloat("staticDarkAreaInfluence", std::clamp(settings.staticDarkAreaInfluence, 0.0f, 4.0f));
        postShader->setFloat("staticSpeed", settings.staticSpeed);
        postShader->setBool("staticMonochrome", settings.staticMonochrome);
        postShader->setFloat("staticSparkle", std::clamp(settings.staticSparkle, 0.0f, 1.0f));
        postShader->setBool("enableStaticDistortion", settings.staticDistortionEnabled);
        postShader->setFloat("staticDistortionHorizontalJitterAmount",
                             std::max(0.0f, settings.staticDistortionHorizontalJitterAmount));
        postShader->setFloat("staticDistortionLineDensity",
                             std::max(1.0f, settings.staticDistortionLineDensity));
        postShader->setFloat("staticDistortionGlitchFrequency", std::max(0.0f, settings.staticDistortionGlitchFrequency));
        postShader->setFloat("staticDistortionStrength", std::max(0.0f, settings.staticDistortionStrength));
        postShader->setBool("enableLensDistortion", settings.lensDistortionEnabled);
        postShader->setFloat("lensDistortionAmount", settings.lensDistortionAmount);
        postShader->setFloat("lensDistortionEdgeFalloff", std::clamp(settings.lensDistortionEdgeFalloff, 0.0f, 1.0f));
        postShader->setVec2("lensDistortionCenterOffset",
                            glm::clamp(settings.lensDistortionCenterOffset, glm::vec2(-0.5f), glm::vec2(0.5f)));
        postShader->setBool("enableLensEdgeVignette",
                            settings.lensDistortionEdgeVignetteEnabled);
        postShader->setFloat("lensEdgeVignetteIntensity",
                             std::clamp(settings.lensDistortionEdgeVignetteIntensity, 0.0f, 1.0f));
        postShader->setFloat("lensEdgeVignetteRadius",
                             std::clamp(settings.lensDistortionEdgeVignetteRadius, 0.0f, 1.4142f));
        postShader->setFloat("lensEdgeVignetteSoftness",
                             std::clamp(settings.lensDistortionEdgeVignetteSoftness, 0.001f, 1.0f));
        postShader->setVec3("lensEdgeVignetteColor",
                            glm::clamp(settings.lensDistortionEdgeVignetteColor,
                                       glm::vec3(0.0f), glm::vec3(1.0f)));
        postShader->setBool("enablePixelation", settings.pixelationEnabled);
        postShader->setFloat("pixelationSize",
                             std::clamp(settings.pixelationSize, 1.0f, 64.0f));
        postShader->setBool("enablePosterize", settings.posterizeEnabled);
        postShader->setInt("posterizeLevels",
                           std::clamp(settings.posterizeLevels, 2, 64));
        postShader->setBool("enableScanlines", settings.scanlinesEnabled);
        postShader->setFloat("scanlinesIntensity",
                             std::clamp(settings.scanlinesIntensity, 0.0f, 1.0f));
        postShader->setFloat("scanlinesDensity",
                             std::clamp(settings.scanlinesDensity, 0.25f, 8.0f));
        postShader->setFloat("scanlinesSpeed", settings.scanlinesSpeed);
        postShader->setBool("enableVHSOverlay", settings.vhsOverlayEnabled);
        postShader->setFloat("vhsOverlayOpacity", std::clamp(settings.vhsOverlayOpacity, 0.0f, 1.0f));
        postShader->setFloat("vhsOverlayScanlineStrength", std::clamp(settings.vhsOverlayScanlineStrength, 0.0f, 1.0f));
        postShader->setFloat("vhsOverlayTapeNoise", std::clamp(settings.vhsOverlayTapeNoise, 0.0f, 1.0f));
        postShader->setFloat("vhsOverlayChromaBleed", std::clamp(settings.vhsOverlayChromaBleed, 0.0f, 1.0f));
        postShader->setFloat("vhsOverlayBottomNoiseBandHeight",
                             std::clamp(settings.vhsOverlayBottomNoiseBandHeight, 0.0f, 1.0f));
        postShader->setFloat("vhsOverlayBottomNoiseBandIntensity",
                             std::clamp(settings.vhsOverlayBottomNoiseBandIntensity, 0.0f, 2.0f));
        postShader->setFloat("vhsOverlayDistortionStrength",
                             std::clamp(settings.vhsOverlayDistortionStrength, 0.0f, 2.0f));
        postShader->setFloat("vhsOverlayAnimationSpeed",
                             std::max(0.0f, settings.vhsOverlayAnimationSpeed));
        postShader->setFloat("vhsOverlayColorBleed",
                             std::clamp(settings.vhsOverlayColorBleed, 0.0f, 1.0f));
        postShader->setFloat("vhsOverlayBanding",
                             std::clamp(settings.vhsOverlayBanding, 0.0f, 1.0f));
        postShader->setInt("vhsOverlaySignalMode",
                           std::clamp(static_cast<int>(settings.vhsOverlaySignalMode), 0, 5));
        postShader->setFloat("vhsOverlayDropouts",
                             std::clamp(settings.vhsOverlayDropouts, 0.0f, 1.0f));
        postShader->setBool("enableWavyEffect", settings.wavyEnabled);
        postShader->setFloat("wavyAmplitude", std::max(0.0f, settings.wavyAmplitude));
        postShader->setFloat("wavyFrequency", std::max(0.0f, settings.wavyFrequency));
        postShader->setFloat("wavySpeed", settings.wavySpeed);
        postShader->setBool("wavyVertical", settings.wavyVertical);
        postShader->setVec2("texelSize", glm::vec2(1.0f / width, 1.0f / height));
        postShader->setFloat("u_time", static_cast<float>(glfwGetTime()));

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sourceTexture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bloomTex ? bloomTex : sourceTexture);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, allowHistory ? historyTarget.texture : 0);

        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        drawFullscreenQuad();
        Modu2DStats::CountPostFxPass();
        postStats.compositeExecuted = true;
        postStats.compositeDestinationTextureId = target.texture;
        postStats.compositeDestinationFramebufferId = target.fbo;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glEnable(GL_DEPTH_TEST);
        postStats.compositeMs = std::chrono::duration<float, std::milli>(Clock::now() - compositeStart).count();
    }

    if (allowHistory) {
        displayTexture = target.texture;
    }

    if (settings.motionBlurEnabled && allowHistory && historyTarget.fbo != 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, target.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, historyTarget.fbo);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        historyValid = true;
    } else if (allowHistory) {
        clearHistory();
    }

    postStats.totalMs = std::chrono::duration<float, std::milli>(Clock::now() - totalStart).count();
    postStats.finalPresentedTextureId = target.texture;
    postStats.finalPresentedFramebufferId = target.fbo;
    postStats.finalTextureDiffersFromSource = (target.texture != sourceTexture);
    logPostFxDebug(postStats, allowHistory);
    return target.texture;
}

void Renderer::renderWireframeOverlay(
    const Camera &camera, const std::vector<SceneObject> &sceneObjects,
    float fovDeg, float nearPlane, float farPlane, bool shadedSurface) {
#if MODULARITY_OPENGL_ES
  (void)camera;
  (void)sceneObjects;
  (void)fovDeg;
  (void)nearPlane;
  (void)farPlane;
  (void)shadedSurface;
  return;
#else
  if (camera.orthographic || currentWidth <= 0 || currentHeight <= 0)
    return;

  struct WireframeDrawItem {
    const SceneObject *obj = nullptr;
    Mesh *mesh = nullptr;
    glm::mat4 model = glm::mat4(1.0f);
    bool wantsGpuSkinning = false;
    bool doubleSided = false;
  };

  auto resolveMesh = [this](const SceneObject &obj) -> Mesh * {
    if (obj.renderType == RenderType::Cube)
      return cubeMesh;
    if (obj.renderType == RenderType::Sphere)
      return sphereMesh;
    if (obj.renderType == RenderType::Capsule)
      return capsuleMesh;
    if (obj.renderType == RenderType::Plane)
      return planeMesh;
    if (obj.renderType == RenderType::Mirror)
      return planeMesh;
    if (obj.renderType == RenderType::Sprite)
      return planeMesh;
    if (obj.renderType == RenderType::Torus)
      return torusMesh;
    if (obj.renderType == RenderType::OBJMesh && obj.meshId >= 0) {
      return g_objLoader.getMesh(obj.meshId);
    }
    if (obj.renderType == RenderType::Model && obj.meshId >= 0) {
      return getModelLoader().getMesh(obj.meshId);
    }
    return nullptr;
  };

  rebuildStaticMergeBatches(sceneObjects);
  std::vector<WireframeDrawItem> drawItems;
  drawItems.reserve(sceneObjects.size() + staticMergeBatches.size());
  for (const SceneObject &obj : sceneObjects) {
    if (!IsObjectEnabledInHierarchy(obj) || !HasRendererComponent(obj))
      continue;
    if (staticMergeSourceIds.find(obj.id) != staticMergeSourceIds.end())
      continue;

    Mesh *mesh = resolveMesh(obj);
    if (!mesh)
      continue;

    bool wantsGpuSkinning = obj.hasSkeletalAnimation && obj.skeletal.enabled &&
                            obj.skeletal.useGpuSkinning;
    const int boneLimit = std::max(0, obj.skeletal.maxBones);
    const int availableBones =
        static_cast<int>(obj.skeletal.finalMatrices.size());
    if (obj.hasSkeletalAnimation && obj.skeletal.enabled &&
        obj.skeletal.allowCpuFallback && boneLimit > 0 &&
        availableBones > boneLimit) {
      wantsGpuSkinning = false;
    }

    if (obj.renderType == RenderType::Model && obj.meshId >= 0 &&
        obj.hasSkeletalAnimation && obj.skeletal.enabled && !wantsGpuSkinning) {
      const auto *meshInfo = getModelLoader().getMeshInfo(obj.meshId);
      if (meshInfo) {
        applyCpuSkinning(*const_cast<OBJLoader::LoadedMesh *>(meshInfo),
                         obj.skeletal.finalMatrices, obj.skeletal.maxBones);
      }
    }

    drawItems.push_back({
        &obj,
        mesh,
        BuildSceneObjectModelMatrix(obj, &camera.position, &camera.up),
        wantsGpuSkinning,
        obj.renderType == RenderType::Sprite ||
            obj.renderType == RenderType::Mirror,
    });
  }
  for (const StaticMergeBatch &batch : staticMergeBatches) {
    if (!batch.mesh)
      continue;
    drawItems.push_back(
        {nullptr, batch.mesh.get(), glm::mat4(1.0f), false, batch.doubleSided});
  }
  if (drawItems.empty())
    return;

  Shader *staticWireShader =
      getShader(selectionMaskVertPath, wireframeFragPath);
  Shader *skinnedWireShader = getShader(skinnedVertPath, wireframeFragPath);
  if (!staticWireShader || !skinnedWireShader)
    return;

  GLint previousFramebuffer = 0;
  GLint previousViewport[4] = {0, 0, currentWidth, currentHeight};
  GLint previousDepthFunc = GL_LESS;
  GLint previousCullMode = GL_BACK;
  GLint previousPolygonMode[2] = {GL_FILL, GL_FILL};
  GLfloat previousLineWidth = 1.0f;
  GLfloat previousPolygonOffsetFactor = 0.0f;
  GLfloat previousPolygonOffsetUnits = 0.0f;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
  glGetIntegerv(GL_VIEWPORT, previousViewport);
  glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunc);
  glGetIntegerv(GL_CULL_FACE_MODE, &previousCullMode);
  glGetIntegerv(GL_POLYGON_MODE, previousPolygonMode);
  glGetFloatv(GL_LINE_WIDTH, &previousLineWidth);
  glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &previousPolygonOffsetFactor);
  glGetFloatv(GL_POLYGON_OFFSET_UNITS, &previousPolygonOffsetUnits);

  const GLboolean previousDepthTest = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean previousCullFace = glIsEnabled(GL_CULL_FACE);
  const GLboolean previousBlend = glIsEnabled(GL_BLEND);
  const GLboolean previousStencil = glIsEnabled(GL_STENCIL_TEST);
  const GLboolean previousPolygonOffsetLine =
      glIsEnabled(GL_POLYGON_OFFSET_LINE);
  GLboolean previousDepthMask = GL_TRUE;
  GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
  glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
  glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);

  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glViewport(0, 0, currentWidth, currentHeight);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(shadedSurface ? GL_LEQUAL : GL_LESS);
  glDepthMask(shadedSurface ? GL_FALSE : GL_TRUE);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glLineWidth(1.0f);
  if (shadedSurface) {
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(-1.0f, -1.0f);
  } else {
    glDisable(GL_POLYGON_OFFSET_LINE);
  }

  const glm::mat4 view = camera.getViewMatrix();
  const glm::mat4 projection = BuildCameraProjection(
      camera, currentWidth, currentHeight, fovDeg, nearPlane, farPlane);
  const glm::vec4 wireColor = shadedSurface
                                  ? glm::vec4(0.035f, 0.045f, 0.065f, 1.0f)
                                  : glm::vec4(0.72f, 0.78f, 0.88f, 1.0f);

  Shader *activeShader = nullptr;
  for (const WireframeDrawItem &item : drawItems) {
    Shader *wireShader =
        item.wantsGpuSkinning ? skinnedWireShader : staticWireShader;
    if (wireShader != activeShader) {
      activeShader = wireShader;
      activeShader->use();
      activeShader->setMat4("view", view);
      activeShader->setMat4("projection", projection);
      activeShader->setVec4("wireColor", wireColor);
    }

    if (item.doubleSided) {
      glDisable(GL_CULL_FACE);
    } else {
      glEnable(GL_CULL_FACE);
      glCullFace(GL_BACK);
    }

    activeShader->setMat4("model", item.model);
    if (item.wantsGpuSkinning && item.obj) {
      const int boneCount = std::min<int>(
          static_cast<int>(item.obj->skeletal.finalMatrices.size()),
          std::max(0, item.obj->skeletal.maxBones));
      activeShader->setInt("boneCount", boneCount);
      activeShader->setBool("useSkinning", boneCount > 0);
      activeShader->setVec4("uvRect", glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
      if (boneCount > 0) {
        activeShader->setMat4Array(
            "bones", item.obj->skeletal.finalMatrices.data(), boneCount);
      }
    }

    recordMeshDraw();
    item.mesh->draw();
  }

  glColorMask(previousColorMask[0], previousColorMask[1], previousColorMask[2],
              previousColorMask[3]);
  if (previousDepthTest)
    glEnable(GL_DEPTH_TEST);
  else
    glDisable(GL_DEPTH_TEST);
  glDepthMask(previousDepthMask);
  glDepthFunc(previousDepthFunc);
  if (previousCullFace) {
    glEnable(GL_CULL_FACE);
    glCullFace(previousCullMode);
  } else {
    glDisable(GL_CULL_FACE);
  }
  if (previousBlend)
    glEnable(GL_BLEND);
  else
    glDisable(GL_BLEND);
  if (previousStencil)
    glEnable(GL_STENCIL_TEST);
  else
    glDisable(GL_STENCIL_TEST);
  if (previousPolygonOffsetLine) {
    glEnable(GL_POLYGON_OFFSET_LINE);
  } else {
    glDisable(GL_POLYGON_OFFSET_LINE);
  }
  glPolygonOffset(previousPolygonOffsetFactor, previousPolygonOffsetUnits);
  glPolygonMode(GL_FRONT, previousPolygonMode[0]);
  glPolygonMode(GL_BACK, previousPolygonMode[1]);
  glLineWidth(previousLineWidth);
  glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
  glViewport(previousViewport[0], previousViewport[1], previousViewport[2],
             previousViewport[3]);
#endif
}

void Renderer::renderScene(const Camera &camera,
                           const std::vector<SceneObject> &sceneObjects,
                           int selectedId, float fovDeg, float nearPlane,
                           float farPlane, bool drawColliders,
                           const std::vector<int> *selectedIds,
                           SceneRenderMode renderMode, bool applyPostFX) {
  resetStats(viewportStats);
  activeStats = &viewportStats;
#if MODULARITY_OPENGL_ES
  renderMode = SceneRenderMode::Normal;
#endif
  const bool drawShadedScene = renderMode != SceneRenderMode::Wireframe;
  if (drawShadedScene && !camera.orthographic) {
    MODU_PROFILE_SCOPE("Mirror Targets", ProfilerSampleCategory::RenderDetail);
    updateMirrorTargets(camera, sceneObjects, currentWidth, currentHeight,
                        fovDeg, nearPlane, farPlane);
  }
  if (drawShadedScene) {
    MODU_PROFILE_SCOPE("Scene Draw", ProfilerSampleCategory::RenderDetail);
    sceneDepthTextureForPass = sceneDepthTexture;
    sceneColorTextureForPass = viewportTexture;
    renderSceneInternal(camera, sceneObjects, currentWidth, currentHeight, true,
                        fovDeg, nearPlane, farPlane, true, true);
    sceneDepthTextureForPass = 0;
    sceneColorTextureForPass = 0;
#if !MODULARITY_OPENGL_ES
    // MODULARITY_RENDER_OPT_DUMP=<file.ppm> writes this view's raw scene colour once, on
    // its 300th frame, so a scene rendered with and without the optimizations can be
    // diffed pixel for pixel. A one-off blocking readback; nothing happens without it.
    static const char* dumpPath = std::getenv("MODULARITY_RENDER_OPT_DUMP");
    static int dumpCountdown = 300;
    if (dumpPath && dumpPath[0] != '\0' && dumpCountdown > 0 && --dumpCountdown == 0) {
      std::vector<unsigned char> pixels(static_cast<size_t>(currentWidth) * currentHeight * 3);
      glBindTexture(GL_TEXTURE_2D, viewportTexture);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
      glPixelStorei(GL_PACK_ALIGNMENT, 4);
      glBindTexture(GL_TEXTURE_2D, 0);
      std::ofstream dump(dumpPath, std::ios::binary);
      dump << "P6\n" << currentWidth << " " << currentHeight << "\n255\n";
      dump.write(reinterpret_cast<const char*>(pixels.data()),
                 static_cast<std::streamsize>(pixels.size()));
      std::cerr << "[RenderOpt] wrote " << dumpPath << "\n";
    }
#endif
  } else if (!camera.orthographic) {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, currentWidth, currentHeight);
    renderSkybox(camera.getViewMatrix(),
                 BuildCameraProjection(camera, currentWidth, currentHeight,
                                       fovDeg, nearPlane, farPlane));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  if (renderMode != SceneRenderMode::Normal) {
    MODU_PROFILE_SCOPE("Wireframe Overlay",
                       ProfilerSampleCategory::RenderDetail);
    renderWireframeOverlay(camera, sceneObjects, fovDeg, nearPlane, farPlane,
                           renderMode == SceneRenderMode::ShadedWireframe);
  }
  selectionOutlineSourceScratch.clear();
  if (selectedIds && !selectedIds->empty()) {
    selectionOutlineSourceScratch.insert(selectionOutlineSourceScratch.end(),
                                         selectedIds->begin(),
                                         selectedIds->end());
  } else if (selectedId >= 0) {
    selectionOutlineSourceScratch.push_back(selectedId);
  }
  if (!camera.orthographic && drawColliders) {
    MODU_PROFILE_SCOPE("Collision Overlay",
                       ProfilerSampleCategory::RenderDetail);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, currentWidth, currentHeight);
    renderCollisionOverlay(camera, sceneObjects, currentWidth, currentHeight,
                           fovDeg, nearPlane, farPlane,
                           selectionOutlineSourceScratch.empty()
                               ? nullptr
                               : &selectionOutlineSourceScratch);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  if (!camera.orthographic) {
    {
      MODU_PROFILE_SCOPE("Selection Outline",
                         ProfilerSampleCategory::RenderDetail);
      renderSelectionOutline(camera, sceneObjects,
                             selectionOutlineSourceScratch, fovDeg, nearPlane,
                             farPlane);
    }
  }
  unsigned int result = 0;
  if (renderMode == SceneRenderMode::Normal && applyPostFX) {
    result = applyPostProcessing(camera, sceneObjects, viewportTexture,
                                 currentWidth, currentHeight, true);
  } else {
    viewportPostStats = {};
    clearHistory();
  }
  if (result) {
    displayTexture = result;
  } else {
    displayTexture = viewportTexture;
    viewportPostStats = {};
    clearHistory();
  }
  activeStats = nullptr;}

void Renderer::renderSceneToTarget(const Camera& camera,
                                   const std::vector<SceneObject>& sceneObjects,
                                   unsigned int targetFramebuffer,
                                   int width, int height,
                                   const glm::mat4& projection,
                                   float nearPlane, float farPlane,
                                   bool clearTarget) {
    renderSceneToTarget(camera, sceneObjects, targetFramebuffer, width, height,
                        projection, nearPlane, farPlane, clearTarget, false);
}

void Renderer::renderSceneToTarget(const Camera& camera,
                                   const std::vector<SceneObject>& sceneObjects,
                                   unsigned int targetFramebuffer,
                                   int width, int height,
                                   const glm::mat4& projection,
                                   float nearPlane, float farPlane,
                                   bool clearTarget, bool applyPostFX) {
    if (!moduRenderContextLive() || width <= 0 || height <= 0) {
        return;
    }

    GLint previousFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    GLint previousViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);
    if (applyPostFX) glDisable(GL_SCISSOR_TEST);
    struct TargetRenderStateRestore {
        GLint framebuffer = 0;
        GLint readFramebuffer = 0;
        GLint viewport[4] = {0, 0, 0, 0};
        GLboolean scissor = GL_FALSE;
        Renderer* renderer = nullptr;

        ~TargetRenderStateRestore() {
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            if (scissor) glEnable(GL_SCISSOR_TEST);
            else glDisable(GL_SCISSOR_TEST);
            if (renderer) {
                renderer->sceneProjectionOverride = nullptr;
                renderer->activeStats = nullptr;
            }
        }
    } restore{previousFramebuffer,
              previousReadFramebuffer,
              {previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]},
              previousScissor, this};

    resetStats(viewportStats);
    activeStats = &viewportStats;
    sceneProjectionOverride = &projection;

    unsigned int sceneFramebuffer = targetFramebuffer;
    if (applyPostFX) {
        RenderTargetOptions options;
        options.hdr = true;
        options.depthSampleable = true;
        ensureRenderTarget(externalSceneTarget, width, height, options);
        if (externalSceneTarget.fbo == 0 || externalSceneTarget.texture == 0) return;
        sceneFramebuffer = externalSceneTarget.fbo;
        if (!clearTarget) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, targetFramebuffer);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sceneFramebuffer);
            glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                              GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer);
    glViewport(0, 0, width, height);
    if (clearTarget) {
        if (camera.solidBackground) {
            glClearColor(camera.backgroundColor.r, camera.backgroundColor.g,
                         camera.backgroundColor.b, 1.0f);
        } else {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        }
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    if (!camera.orthographic) {
        MODU_PROFILE_SCOPE("Mirror Targets", ProfilerSampleCategory::RenderDetail);
        updateMirrorTargets(camera, sceneObjects, width, height, FOV, nearPlane, farPlane);
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer);
        glViewport(0, 0, width, height);
    }
    {
        MODU_PROFILE_SCOPE("Scene Draw", ProfilerSampleCategory::RenderDetail);
        sceneDepthTextureForPass = applyPostFX ? externalSceneTarget.depthTexture : 0;
        sceneColorTextureForPass = applyPostFX ? externalSceneTarget.texture : 0;
        renderSceneInternal(camera, sceneObjects, width, height, false, FOV, nearPlane, farPlane,
                            true, true);
        sceneDepthTextureForPass = 0;
        sceneColorTextureForPass = 0;
    }
    if (applyPostFX) {
        // Do not share temporal history between the left eye, right eye and
        // desktop viewport. Each eye is processed before its swapchain releases.
        postFxUseExternalTarget = true;
        const unsigned int processed = applyPostProcessing(
            camera, sceneObjects, externalSceneTarget.texture, width, height, false);
        postFxUseExternalTarget = false;
        const unsigned int presented = processed ? processed : externalSceneTarget.texture;
        const unsigned int sourceFramebuffer = findFramebufferForTexture(presented);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFramebuffer ? sourceFramebuffer : sceneFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targetFramebuffer);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);

    }
}

unsigned int Renderer::renderScenePreview(const Camera& camera, const std::vector<SceneObject>& sceneObjects, int width, int height, float fovDeg, float nearPlane, float farPlane, bool applyPostFX, int previewSlot, bool transparentBackground) {
    if (!moduRenderContextLive()) {
        return 0;
    }
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return 0;
    }

    GLint previousFramebuffer = 0;
    GLint previousViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    struct PreviewRenderStateRestore {
        GLint framebuffer = 0;
        GLint viewport[4] = {0, 0, 0, 0};
        Renderer* renderer = nullptr;

        ~PreviewRenderStateRestore() {
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            if (renderer) {
                renderer->activeStats = nullptr;
            }
        }
    } restore{previousFramebuffer,
              {previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]},
              this};

    resetStats(previewStats);
    activeStats = &previewStats;
    RenderTarget& target = (previewSlot == 0) ? previewTarget : extraPreviewTargets[previewSlot];
    if (previewSlot != 0) {
        extraPreviewTargetLastUsed[previewSlot] = ++previewTargetUseSerial;
        purgePreviewTargetsIfNeeded(previewSlot);
    }
    {
        RenderTargetOptions previewOptions;
        previewOptions.alpha = transparentBackground;
        previewOptions.hdr = true;
        previewOptions.depthSampleable = true;
        ensureRenderTarget(target, width, height, previewOptions);
    }
    if (target.fbo == 0) {
        previewPostStats = {};
        return 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
    glViewport(0, 0, width, height);
    if (transparentBackground) {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    } else if (camera.solidBackground) {
        glClearColor(camera.backgroundColor.r, camera.backgroundColor.g,
                     camera.backgroundColor.b, 1.0f);
    } else {
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!camera.orthographic) {
        MODU_PROFILE_SCOPE("Mirror Targets", ProfilerSampleCategory::RenderDetail);
        updateMirrorTargets(camera, sceneObjects, width, height, fovDeg, nearPlane, farPlane);
    }
    {
        MODU_PROFILE_SCOPE("Scene Draw", ProfilerSampleCategory::RenderDetail);
        sceneDepthTextureForPass = target.depthTexture;
        sceneColorTextureForPass = target.texture;
        renderSceneInternal(camera, sceneObjects, width, height, true, fovDeg, nearPlane, farPlane, true, !transparentBackground);
        sceneDepthTextureForPass = 0;
        sceneColorTextureForPass = 0;
    }
    if (!applyPostFX) {
        previewPostStats = {};
        return target.texture;
    }
    unsigned int processed = applyPostProcessing(camera, sceneObjects, target.texture, width, height, false);
    return processed ? processed : target.texture;
}

Mesh* Renderer::capsuleColliderMesh(float radius, float cylinderHalfHeight) {
    if (radius <= 0.0001f) return capsuleMesh;
    const float ratio = std::clamp(cylinderHalfHeight / radius, 0.0f, 64.0f);
    const int key = static_cast<int>(std::lround(ratio * 64.0f));
    auto it = capsuleColliderMeshes.find(key);
    if (it != capsuleColliderMeshes.end()) return it->second;

    if (capsuleColliderMeshes.size() >= 64) {
        for (auto& [existingKey, mesh] : capsuleColliderMeshes) delete mesh;
        capsuleColliderMeshes.clear();
    }

    std::vector<float> verts = generateCapsuleShape(1.0f, static_cast<float>(key) / 64.0f);
    if (verts.empty()) return capsuleMesh;
    Mesh* mesh = new Mesh(verts.data(), verts.size() * sizeof(float));
    capsuleColliderMeshes[key] = mesh;
    return mesh;
}

void Renderer::renderCollisionOverlay(const Camera& camera, const std::vector<SceneObject>& sceneObjects, int width, int height, float fovDeg, float nearPlane, float farPlane, const std::vector<int>* previewIds) {
#if MODULARITY_OPENGL_ES
    (void)camera;
    (void)sceneObjects;
    (void)width;
    (void)height;
    (void)fovDeg;
    (void)nearPlane;
    (void)farPlane;
    (void)previewIds;
    return;
#else
    if (camera.orthographic || !defaultShader || width <= 0 || height <= 0) return;
    std::unordered_set<int> previewSet;
    if (previewIds && !previewIds->empty()) {
        previewSet.insert(previewIds->begin(), previewIds->end());
    }

    GLint prevPoly[2] = { GL_FILL, GL_FILL };
    glGetIntegerv(GL_POLYGON_MODE, prevPoly);
    GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean depthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    GLboolean cullFace = glIsEnabled(GL_CULL_FACE);
    GLint prevCullMode = GL_BACK;
    glGetIntegerv(GL_CULL_FACE_MODE, &prevCullMode);
    GLboolean polyOffsetLine = glIsEnabled(GL_POLYGON_OFFSET_LINE);

    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(-1.0f, -1.0f);

    Shader* active = defaultShader;
    active->use();
    active->setMat4("view", camera.getViewMatrix());
    active->setMat4("projection", BuildCameraProjection(camera, width, height, fovDeg, nearPlane, farPlane));
    active->setVec3("viewPos", camera.position);
    active->setBool("unlit", true);
    active->setBool("hasOverlay", false);
    active->setBool("hasNormalMap", false);
    active->setInt("lightCount", 0);
    active->setFloat("mixAmount", 0.0f);
    active->setVec3("materialColor", glm::vec3(0.2f, 1.0f, 0.2f));
    active->setFloat("ambientStrength", 1.0f);
    active->setFloat("specularStrength", 0.0f);
    active->setFloat("shininess", 1.0f);
    active->setInt("texture1", 0);
    active->setInt("overlayTex", 1);
    active->setInt("normalMap", 2);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, debugWhiteTexture ? debugWhiteTexture : (texture1 ? texture1->GetID() : 0));

    for (const auto& obj : sceneObjects) {
        if (!previewSet.empty() && previewSet.find(obj.id) == previewSet.end()) continue;
        if (!IsObjectEnabledInHierarchy(obj)) continue;
        if (!(obj.hasCollider && obj.collider.enabled) && !(obj.hasRigidbody && obj.rigidbody.enabled)) continue;

        Mesh* meshToDraw = nullptr;
        glm::vec3 scale = obj.scale;
        glm::vec3 colliderOffset(0.0f);

        if (obj.hasCollider && obj.collider.enabled) {
            colliderOffset = obj.collider.offset;
            switch (obj.collider.type) {
                case ColliderType::Box:
                    meshToDraw = cubeMesh;
                    scale = obj.collider.boxSize;
                    break;
                case ColliderType::Capsule: {
                    const float capsuleRadius = std::max(0.01f,
                        std::max(obj.collider.boxSize.x, obj.collider.boxSize.z) * 0.5f);
                    const float capsuleHalfHeight =
                        std::max(0.05f, obj.collider.boxSize.y - capsuleRadius * 2.0f) * 0.5f;
                    meshToDraw = capsuleColliderMesh(capsuleRadius, capsuleHalfHeight);
                    scale = glm::vec3(capsuleRadius);
                    break;
                }
                case ColliderType::Sphere:
                    meshToDraw = sphereMesh;
                    scale = glm::vec3(std::max({obj.collider.boxSize.x, obj.collider.boxSize.y, obj.collider.boxSize.z}));
                    break;
                case ColliderType::Mesh:
                case ColliderType::ConvexMesh:
                    if (obj.hasRenderer && obj.renderType == RenderType::OBJMesh && obj.meshId >= 0) {
                        meshToDraw = g_objLoader.getMesh(obj.meshId);
                    } else if (obj.hasRenderer && obj.renderType == RenderType::Model && obj.meshId >= 0) {
                        meshToDraw = getModelLoader().getMesh(obj.meshId);
                    } else {
                        meshToDraw = nullptr;
                    }
                    scale = obj.scale;
                    break;
            }
        } else {
            switch (obj.renderType) {
                case RenderType::Cube:
                    meshToDraw = cubeMesh;
                    break;
                case RenderType::Sphere:
                    meshToDraw = sphereMesh;
                    break;
                case RenderType::Capsule:
                    meshToDraw = capsuleMesh;
                    break;
                case RenderType::Plane:
                    meshToDraw = planeMesh;
                    break;
                case RenderType::Sprite:
                    meshToDraw = planeMesh;
                    break;
                case RenderType::Torus:
                    meshToDraw = sphereMesh;
                    break;
                case RenderType::OBJMesh:
                    if (obj.meshId >= 0) meshToDraw = g_objLoader.getMesh(obj.meshId);
                    break;
                case RenderType::Model:
                    if (obj.meshId >= 0) meshToDraw = getModelLoader().getMesh(obj.meshId);
                    break;
                default:
                    break;
            }
        }

        if (!meshToDraw) continue;

        glm::mat4 model(1.0f);
        model = glm::translate(model, obj.position);
        model = glm::rotate(model, glm::radians(obj.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        model = glm::rotate(model, glm::radians(obj.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(obj.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::translate(model, colliderOffset);
        model = glm::scale(model, scale);
        active->setMat4("model", model);

        meshToDraw->draw();
    }

    if (!polyOffsetLine) glDisable(GL_POLYGON_OFFSET_LINE);
    if (cullFace) glEnable(GL_CULL_FACE);
    if (depthTest) glEnable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, prevPoly[0]);
#endif
}

bool Renderer::getObjectLocalBounds(const SceneObject& object,glm::vec3& center,glm::vec3& extents) {
    return TryGetObjectLocalBounds(object,center,extents);
}

void Renderer::renderSelectionOutline(const Camera& camera, const std::vector<SceneObject>& objects,
    const std::vector<int>& ids, float fovDeg, float nearPlane, float farPlane) {
    renderSelectionOutlinePass(camera, objects, ids,
        BuildCameraProjection(camera,currentWidth,currentHeight,fovDeg,nearPlane,farPlane),
        framebuffer,currentWidth,currentHeight,nearPlane,2.7f,.95f,true,framebuffer);
}

#if !MODULARITY_RUNTIME_ONLY
void Renderer::renderSelectionOutlineToTarget(const Camera& camera, const std::vector<SceneObject>& objects,
    const std::vector<int>& ids, const glm::mat4& projection, unsigned int targetFramebuffer,
    int width, int height, float nearPlane, float radiusPx, float opacity, bool animateSelection) {
    renderSelectionOutlinePass(camera,objects,ids,projection,targetFramebuffer,width,height,nearPlane,
                              std::clamp(radiusPx,.5f,8.f),std::clamp(opacity,0.f,1.f),animateSelection,
                              externalSceneTarget.fbo ? externalSceneTarget.fbo : targetFramebuffer);
}
#endif

void Renderer::renderSelectionOutlinePass(const Camera& camera, const std::vector<SceneObject>& sceneObjects,
    const std::vector<int>& selectedIds, const glm::mat4& projection, unsigned int targetFramebuffer,
    int width, int height, float nearPlane, float radiusPx, float opacity, bool animateSelection,
    unsigned int depthFramebuffer) {
    if (camera.orthographic) {
        return;
    }
    if (animateSelection) {
    const double nowSec = glfwGetTime();
    if (selectionOutlineLastUpdateSec <= 0.0 || nowSec < selectionOutlineLastUpdateSec) {
        selectionOutlineLastUpdateSec = nowSec;
    }
    float dt = static_cast<float>(nowSec - selectionOutlineLastUpdateSec);
    selectionOutlineLastUpdateSec = nowSec;
    dt = std::clamp(dt, 0.0f, 0.25f);

    selectionOutlineInputScratch.clear();
    selectionOutlineInputScratch.reserve(selectedIds.size());
    for (int id : selectedIds) {
        if (id >= 0) selectionOutlineInputScratch.push_back(id);
    }
    std::sort(selectionOutlineInputScratch.begin(), selectionOutlineInputScratch.end());
    selectionOutlineInputScratch.erase(
        std::unique(selectionOutlineInputScratch.begin(), selectionOutlineInputScratch.end()),
        selectionOutlineInputScratch.end());

    constexpr float kFadeInSeconds = 0.05f;
    constexpr float kFadeOutSeconds = 0.05f;
    constexpr float kSwapSeconds = 0.10f;
    const bool hasSelection = !selectionOutlineInputScratch.empty();
    if (hasSelection) {
        if (selectionOutlineVisualIds.empty()) {
            selectionOutlineVisualIds = selectionOutlineInputScratch;
            selectionOutlinePrevIds.clear();
            selectionOutlineSwapBlend = 1.0f;
        } else if (selectionOutlineVisualIds != selectionOutlineInputScratch) {
            selectionOutlinePrevIds = selectionOutlineVisualIds;
            selectionOutlineVisualIds = selectionOutlineInputScratch;
            selectionOutlineSwapBlend = 0.0f;
        }
    }

    const float targetBlend = hasSelection ? 1.0f : 0.0f;
    if (selectionOutlineBlend < targetBlend) {
        selectionOutlineBlend = std::min(targetBlend, selectionOutlineBlend + dt / kFadeInSeconds);
    } else if (selectionOutlineBlend > targetBlend) {
        selectionOutlineBlend = std::max(targetBlend, selectionOutlineBlend - dt / kFadeOutSeconds);
    }

    if (!hasSelection) {
        selectionOutlinePrevIds.clear();
        selectionOutlineSwapBlend = 1.0f;
    } else if (!selectionOutlinePrevIds.empty()) {
        selectionOutlineSwapBlend = std::min(1.0f, selectionOutlineSwapBlend + dt / kSwapSeconds);
        if (selectionOutlineSwapBlend >= 0.999f) {
            selectionOutlineSwapBlend = 1.0f;
            selectionOutlinePrevIds.clear();
        }
    } else {
        selectionOutlineSwapBlend = 1.0f;
    }

    if (!hasSelection && selectionOutlineBlend <= 0.001f) {
        selectionOutlineBlend = 0.0f;
        selectionOutlineVisualIds.clear();
        selectionOutlinePrevIds.clear();
        selectionOutlineSwapBlend = 1.0f;
        return;
    }
    }
    if (!defaultShader || width <= 0 || height <= 0) return;
    if (animateSelection && (selectionOutlineVisualIds.empty() || selectionOutlineBlend <= 0.001f)) return;

    auto resolveMesh = [this](const SceneObject& obj) -> Mesh* {
        if (obj.renderType == RenderType::Cube) return cubeMesh;
        if (obj.renderType == RenderType::Sphere) return sphereMesh;
        if (obj.renderType == RenderType::Capsule) return capsuleMesh;
        if (obj.renderType == RenderType::Plane) return planeMesh;
        if (obj.renderType == RenderType::Mirror) return planeMesh;
        if (obj.renderType == RenderType::Sprite) return planeMesh;
        if (obj.renderType == RenderType::Torus) return torusMesh;
        if (obj.renderType == RenderType::OBJMesh && obj.meshId >= 0) return g_objLoader.getMesh(obj.meshId);
        if (obj.renderType == RenderType::Model && obj.meshId >= 0) return getModelLoader().getMesh(obj.meshId);
        return nullptr;
    };

    auto buildDrawItems = [&](const std::vector<int>& ids, std::vector<MaskDrawItem>& drawItems) {
        selectionOutlineSelectedScratch.clear();
        selectionOutlineSelectedScratch.reserve(ids.size());
        for (int id : ids) {
            selectionOutlineSelectedScratch.insert(id);
        }

        drawItems.clear();
        drawItems.reserve(selectionOutlineSelectedScratch.size());

        for (const auto& obj : sceneObjects) {
            if (!IsObjectEnabledInHierarchy(obj)) continue;
            if (selectionOutlineSelectedScratch.find(obj.id) == selectionOutlineSelectedScratch.end()) continue;
            if (!HasRendererComponent(obj)) continue;

            Mesh* meshToDraw = resolveMesh(obj);
            if (!meshToDraw) continue;

            bool wantsGpuSkinning = obj.hasSkeletalAnimation && obj.skeletal.enabled && obj.skeletal.useGpuSkinning;
            int boneLimit = obj.skeletal.maxBones;
            int availableBones = static_cast<int>(obj.skeletal.finalMatrices.size());
            if (obj.hasSkeletalAnimation && obj.skeletal.enabled &&
                obj.skeletal.allowCpuFallback && boneLimit > 0 && availableBones > boneLimit) {
                wantsGpuSkinning = false;
            }

            if (obj.renderType == RenderType::Model && obj.meshId >= 0 &&
                obj.hasSkeletalAnimation && obj.skeletal.enabled && !wantsGpuSkinning) {
                const auto* meshInfo = getModelLoader().getMeshInfo(obj.meshId);
                if (meshInfo) {
                    applyCpuSkinning(*const_cast<OBJLoader::LoadedMesh*>(meshInfo),
                                     obj.skeletal.finalMatrices,
                                     obj.skeletal.maxBones);
                }
            }

            drawItems.push_back({
                &obj,
                meshToDraw,
                wantsGpuSkinning,
                (obj.renderType == RenderType::Sprite || obj.renderType == RenderType::Mirror)
            });
        }
    };

    buildDrawItems(animateSelection ? selectionOutlineVisualIds : selectedIds, selectionOutlineCurrentDrawScratch);
    selectionOutlinePreviousDrawScratch.clear();
    if (animateSelection && !selectionOutlinePrevIds.empty() && selectionOutlineSwapBlend < 0.999f) {
        buildDrawItems(selectionOutlinePrevIds, selectionOutlinePreviousDrawScratch);
    }
    if (animateSelection && selectionOutlinePreviousDrawScratch.empty()) {
        selectionOutlinePrevIds.clear();
        selectionOutlineSwapBlend = 1.0f;
    }
    if (selectionOutlineCurrentDrawScratch.empty() && selectionOutlinePreviousDrawScratch.empty()) return;

    ensureRenderTarget(selectionMaskTarget, width, height);
    if (selectionMaskTarget.fbo == 0 || selectionMaskTarget.texture == 0) return;

    Shader* staticMaskShader = getShader(selectionMaskVertPath, selectionMaskFragPath);
    Shader* skinnedMaskShader = getShader(skinnedVertPath, selectionMaskFragPath);
    Shader* outlineShader = getShader(postVertPath, selectionOutlineFragPath);
    if (!staticMaskShader || !skinnedMaskShader || !outlineShader) return;
    const bool staticMaskShaderFallbackToDefault = (staticMaskShader == defaultShader);
    const bool skinnedMaskShaderFallbackToDefault = (skinnedMaskShader == defaultShader);
    const bool outlineShaderFallbackToDefault = (outlineShader == defaultShader);
    if (outlineShaderFallbackToDefault) return;

    // Optional coarse occupancy pre-pass (selection_outline_coarse_frag.glsl) that lets the outline
    // shader skip its neighbourhood search away from the selection. Without it the outline still
    // renders, it just searches every pixel.
    static const std::string kSelectionOutlineCoarseFragPath =
        "Resources/Shaders/selection_outline_coarse_frag.glsl";
    Shader* coarseMaskShader = getShader(postVertPath, kSelectionOutlineCoarseFragPath);
    if (coarseMaskShader == defaultShader || (coarseMaskShader && coarseMaskShader->ID == 0)) {
        coarseMaskShader = nullptr;
    }
    // A reserved slot in the renderer-owned auxiliary target map, so the coarse target is created,
    // resized and released with the renderer without widening Rendering.h.
    constexpr int kSelectionCoarseMaskSlot = -7101;
    constexpr int kCoarseBlockSize = 8;

    GLint prevDrawFbo = 0;
    GLint prevReadFbo = 0;
    GLint prevViewport[4] = { 0, 0, width, height };
    GLint prevActiveTex = GL_TEXTURE0;
    GLint prevBlendSrcRGB = GL_ONE;
    GLint prevBlendDstRGB = GL_ZERO;
    GLint prevBlendSrcAlpha = GL_ONE;
    GLint prevBlendDstAlpha = GL_ZERO;
    GLint prevDepthFunc = GL_LESS;
    GLfloat prevPolyOffsetFactor = 0.0f;
    GLfloat prevPolyOffsetUnits = 0.0f;
#if !MODULARITY_OPENGL_ES
    GLint prevPoly[2] = { GL_FILL, GL_FILL };
#endif
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevBlendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDstAlpha);
    glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
#if !MODULARITY_OPENGL_ES
    glGetIntegerv(GL_POLYGON_MODE, prevPoly);
#endif
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &prevPolyOffsetFactor);
    glGetFloatv(GL_POLYGON_OFFSET_UNITS, &prevPolyOffsetUnits);

    GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean depthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    GLboolean cullFace = glIsEnabled(GL_CULL_FACE);
    GLint prevCullMode = GL_BACK;
    glGetIntegerv(GL_CULL_FACE_MODE, &prevCullMode);
    GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    GLboolean stencilEnabled = glIsEnabled(GL_STENCIL_TEST);
    GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    GLint prevScissorBox[4] = { 0, 0, width, height };
    glGetIntegerv(GL_SCISSOR_BOX, prevScissorBox);
    GLboolean polyOffsetFill = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    GLboolean prevColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);

    const glm::mat4 view = camera.getViewMatrix();
    const glm::mat4 viewProjection = projection * view;

    // The outline shader searches a neighbourhood of the mask per pixel, so run over the whole
    // viewport it cost tens of millions of texture fetches per frame to draw a few-pixel border.
    // An outline pixel can only appear within the search radius of the selected object, and the
    // pass is alpha blended, so every pixel outside that box writes alpha 0 and leaves the target
    // untouched: restricting the pass to the box is the same image for a fraction of the fill.
    // These match the uniforms the pass sets below.
    const float kOutlineRadiusPx = radiusPx;
    constexpr float kOutlineSoftnessPx = 1.15f;
    const int outlineMarginPx =
        static_cast<int>(std::ceil(kOutlineRadiusPx + kOutlineSoftnessPx)) + 1;

    struct ScreenBox {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        bool valid = false;   // false: projection not trusted, keep the full viewport
        bool empty = false;   // wholly off screen, nothing to draw
    };

    // Conservative screen rect (GL pixels, bottom-left origin) of the objects being outlined: the
    // object's bounding box, clipped against the near plane so geometry that surrounds the camera
    // (a floor plane) still gets a partial rect instead of the whole viewport.
    auto computeScreenBox = [&](const std::vector<MaskDrawItem>& drawItems, int marginPx) -> ScreenBox {
        ScreenBox box;
        const float clipW = std::max(nearPlane, 0.0001f);
        float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
        bool anyInFront = false;
        auto accumulate = [&](const glm::vec4& clip) {
            const float sx = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(width);
            const float sy = (clip.y / clip.w * 0.5f + 0.5f) * static_cast<float>(height);
            if (!std::isfinite(sx) || !std::isfinite(sy)) return false;
            minX = std::min(minX, sx);
            minY = std::min(minY, sy);
            maxX = std::max(maxX, sx);
            maxY = std::max(maxY, sy);
            return true;
        };
        for (const auto& drawItem : drawItems) {
            if (!drawItem.obj) return box;
            // Skinned meshes can deform past their bind-pose bounds.
            if (drawItem.obj->hasSkeletalAnimation && drawItem.obj->skeletal.enabled) return box;
            glm::vec3 localCenter(0.0f);
            glm::vec3 localExtents(0.0f);
            if (!TryGetObjectLocalBounds(*drawItem.obj, localCenter, localExtents)) return box;
            const glm::mat4 mvp = viewProjection *
                BuildSceneObjectModelMatrix(*drawItem.obj, &camera.position, &camera.up);

            std::array<glm::vec4, 8> clip;
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec3 local(
                    localCenter.x + ((corner & 1) ? localExtents.x : -localExtents.x),
                    localCenter.y + ((corner & 2) ? localExtents.y : -localExtents.y),
                    localCenter.z + ((corner & 4) ? localExtents.z : -localExtents.z));
                clip[corner] = mvp * glm::vec4(local, 1.0f);
            }
            for (int corner = 0; corner < 8; ++corner) {
                if (clip[corner].w >= clipW) {
                    anyInFront = true;
                    if (!accumulate(clip[corner])) return box;
                }
                // Each box edge that crosses the near plane contributes its crossing point.
                for (int axisBit : {1, 2, 4}) {
                    if (corner & axisBit) continue;
                    const glm::vec4& a = clip[corner];
                    const glm::vec4& b = clip[corner | axisBit];
                    if ((a.w >= clipW) == (b.w >= clipW)) continue;
                    const float t = (clipW - a.w) / (b.w - a.w);
                    if (!accumulate(glm::mix(a, b, t))) return box;
                }
            }
        }
        box.valid = true;
        if (!anyInFront) {
            box.empty = true; // wholly behind the camera
            return box;
        }

        const float marginF = static_cast<float>(marginPx);
        const float fx0 = std::max(0.0f, std::floor(minX) - marginF);
        const float fy0 = std::max(0.0f, std::floor(minY) - marginF);
        const float fx1 = std::min(static_cast<float>(width), std::ceil(maxX) + marginF);
        const float fy1 = std::min(static_cast<float>(height), std::ceil(maxY) + marginF);
        box.empty = (fx1 <= fx0 || fy1 <= fy0);
        box.x = static_cast<int>(fx0);
        box.y = static_cast<int>(fy0);
        box.w = box.empty ? 0 : static_cast<int>(fx1 - fx0);
        box.h = box.empty ? 0 : static_cast<int>(fy1 - fy0);
        return box;
    };

    auto drawOutlinePass = [&](const std::vector<MaskDrawItem>& drawItems, float passWeight) {
        if (drawItems.empty() || passWeight <= 0.001f) return;

        // Box the outline can cover, and the mask region it reads: outline pixels at the edge of
        // their box still sample up to the search radius further out, so the mask has to be
        // cleared and filled over a box one margin larger again.
        const ScreenBox outlineBox = computeScreenBox(drawItems, outlineMarginPx);
        const ScreenBox maskBox = computeScreenBox(drawItems, outlineMarginPx * 2);
        const bool useScissor = outlineBox.valid && maskBox.valid;
        if (useScissor && (outlineBox.empty || maskBox.empty)) {
            return; // selection is off screen; the pass would be a no-op
        }
        if (useScissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(maskBox.x, maskBox.y, maskBox.w, maskBox.h);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, depthFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, selectionMaskTarget.fbo);
        glBlitFramebuffer(0, 0, width, height,
                          0, 0, width, height,
                          GL_DEPTH_BUFFER_BIT, GL_NEAREST);

        glBindFramebuffer(GL_FRAMEBUFFER, selectionMaskTarget.fbo);
        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_FALSE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
#if !MODULARITY_OPENGL_ES
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif
        glDisable(GL_BLEND);
        glDisable(GL_STENCIL_TEST);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);

        for (const auto& drawItem : drawItems) {
            Shader* maskShader = drawItem.wantsGpuSkinning ? skinnedMaskShader : staticMaskShader;
            if (!maskShader) continue;

            if (drawItem.doubleSided) {
                glDisable(GL_CULL_FACE);
            } else {
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
            }

            glm::mat4 model = BuildSceneObjectModelMatrix(*drawItem.obj, &camera.position, &camera.up);

            maskShader->use();
            maskShader->setMat4("view", view);
            maskShader->setMat4("projection", projection);
            maskShader->setMat4("model", model);

            const bool maskShaderFallbackToDefault =
                drawItem.wantsGpuSkinning ? skinnedMaskShaderFallbackToDefault : staticMaskShaderFallbackToDefault;
            if (maskShaderFallbackToDefault) {
                maskShader->setVec3("viewPos", camera.position);
                maskShader->setBool("unlit", true);
                maskShader->setBool("hasOverlay", false);
                maskShader->setBool("hasNormalMap", false);
                maskShader->setInt("lightCount", 0);
                maskShader->setFloat("mixAmount", 0.0f);
                maskShader->setVec3("materialColor", glm::vec3(1.0f));
                maskShader->setFloat("ambientStrength", 1.0f);
                maskShader->setFloat("specularStrength", 0.0f);
                maskShader->setFloat("shininess", 1.0f);
                maskShader->setInt("texture1", 0);
                maskShader->setInt("overlayTex", 1);
                maskShader->setInt("normalMap", 2);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, debugWhiteTexture ? debugWhiteTexture : (texture1 ? texture1->GetID() : 0));
            }

            if (drawItem.wantsGpuSkinning && drawItem.obj->hasSkeletalAnimation && drawItem.obj->skeletal.enabled) {
                int boneCount = std::min<int>(static_cast<int>(drawItem.obj->skeletal.finalMatrices.size()),
                                              std::max(0, drawItem.obj->skeletal.maxBones));
                if (boneCount > 0) {
                    maskShader->setInt("boneCount", boneCount);
                    maskShader->setMat4Array("bones", drawItem.obj->skeletal.finalMatrices.data(), boneCount);
                    maskShader->setBool("useSkinning", true);
                } else {
                    maskShader->setBool("useSkinning", false);
                }
            } else {
                maskShader->setBool("useSkinning", false);
            }

            drawItem.mesh->draw();
        }

        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);

        unsigned int coarseMaskTexture = 0;
        if (coarseMaskShader) {
            const int coarseWidth = (width + kCoarseBlockSize - 1) / kCoarseBlockSize;
            const int coarseHeight = (height + kCoarseBlockSize - 1) / kCoarseBlockSize;
            RenderTarget& coarseTarget = extraPreviewTargets[kSelectionCoarseMaskSlot];
            extraPreviewTargetLastUsed[kSelectionCoarseMaskSlot] = ++previewTargetUseSerial;
            RenderTargetOptions coarseOptions;
            coarseOptions.depth = false;
            ensureRenderTarget(coarseTarget, coarseWidth, coarseHeight, coarseOptions);
            if (coarseTarget.fbo != 0 && coarseTarget.texture != 0) {
                glBindFramebuffer(GL_FRAMEBUFFER, coarseTarget.fbo);
                glViewport(0, 0, coarseWidth, coarseHeight);
                glDisable(GL_BLEND);
                if (useScissor) {
                    // Blocks covering the mask box: every block the outline pass reads lies inside.
                    const int bx0 = maskBox.x / kCoarseBlockSize;
                    const int by0 = maskBox.y / kCoarseBlockSize;
                    const int bx1 = (maskBox.x + maskBox.w + kCoarseBlockSize - 1) / kCoarseBlockSize;
                    const int by1 = (maskBox.y + maskBox.h + kCoarseBlockSize - 1) / kCoarseBlockSize;
                    glEnable(GL_SCISSOR_TEST);
                    glScissor(bx0, by0, std::max(0, bx1 - bx0), std::max(0, by1 - by0));
                }
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                coarseMaskShader->use();
                coarseMaskShader->setInt("maskTex", 0);
                coarseMaskShader->setVec2("maskSize", glm::vec2(static_cast<float>(width),
                                                                static_cast<float>(height)));
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, selectionMaskTarget.texture);
                drawFullscreenQuad();
                coarseMaskTexture = coarseTarget.texture;
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer);
        glViewport(0, 0, width, height);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        outlineShader->use();
        outlineShader->setInt("maskTex", 0);
        outlineShader->setVec2("texelSize", glm::vec2(1.0f / (float)width, 1.0f / (float)height));
        outlineShader->setVec3("outlineColor", glm::vec3(0.48f, 0.43f, 1.0f));
        outlineShader->setFloat("outlineRadiusPx", radiusPx);
        outlineShader->setFloat("outlineSoftnessPx", 1.15f);
        outlineShader->setFloat("outlineOpacity", opacity * (animateSelection ? selectionOutlineBlend : 1.f) * passWeight);
        outlineShader->setInt("coarseMaskTex", 1);
        outlineShader->setBool("useCoarseMask", coarseMaskTexture != 0);
        outlineShader->setVec2("maskSize", glm::vec2(static_cast<float>(width),
                                                     static_cast<float>(height)));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, coarseMaskTexture);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, selectionMaskTarget.texture);
        if (useScissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(outlineBox.x, outlineBox.y, outlineBox.w, outlineBox.h);
        }
        drawFullscreenQuad();
        glDisable(GL_SCISSOR_TEST);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
    };

    float currentPassWeight = 1.0f;
    float previousPassWeight = 0.0f;
    if (!selectionOutlinePreviousDrawScratch.empty() && selectionOutlineSwapBlend < 0.999f) {
        previousPassWeight = 1.0f - selectionOutlineSwapBlend;
        currentPassWeight = selectionOutlineSwapBlend;
    }

    drawOutlinePass(selectionOutlinePreviousDrawScratch, previousPassWeight);
    drawOutlinePass(selectionOutlineCurrentDrawScratch, currentPassWeight);

    glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
    if (depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(depthMask);
    if (cullFace) {
        glEnable(GL_CULL_FACE);
        glCullFace(prevCullMode);
    } else {
        glDisable(GL_CULL_FACE);
    }
    if (blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFuncSeparate(prevBlendSrcRGB, prevBlendDstRGB, prevBlendSrcAlpha, prevBlendDstAlpha);
    if (stencilEnabled) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (scissorEnabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glScissor(prevScissorBox[0], prevScissorBox[1], prevScissorBox[2], prevScissorBox[3]);
    if (polyOffsetFill) glEnable(GL_POLYGON_OFFSET_FILL); else glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(prevPolyOffsetFactor, prevPolyOffsetUnits);
    glDepthFunc(prevDepthFunc);
#if !MODULARITY_OPENGL_ES
    glPolygonMode(GL_FRONT, prevPoly[0]);
    glPolygonMode(GL_BACK, prevPoly[1]);
#endif

    glActiveTexture(prevActiveTex);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, prevDrawFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
}

void Renderer::endRender() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    Runtime2DCountStateBind();
}
