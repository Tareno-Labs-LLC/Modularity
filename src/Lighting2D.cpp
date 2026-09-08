#include "Lighting2D.h"
#include "Rendering.h"
#include "../include/Graphics/OpenGL.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
namespace {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr int kMaxFreeformPoints = 64;
    constexpr float kNegligibleLightContribution = 0.0035f;
    constexpr float kMinVisibleLightExtent = 0.5f;
    const std::array<std::string, kMaxFreeformPoints>& FreeformPointUniformNames() {
        static const std::array<std::string, kMaxFreeformPoints> names = [] {
            std::array<std::string, kMaxFreeformPoints> generated{};
            for (int i = 0; i < kMaxFreeformPoints; ++i) {
                generated[static_cast<size_t>(i)] = "u_polygonPoints[" + std::to_string(i) + "]";
            }
            return generated;
        }();return names;
    }
    bool HasVisiblePostFxInternal(const Light2DPostFXSettings& settings) {
        const bool hasDither = settings.ditherIntensity > 0.0001f || settings.colorBits < 8 || settings.pixelation > 0.0001f;
        const bool hasColorAdjust = std::abs(settings.exposure) > 0.0001f || std::abs(settings.contrast - 1.0f) > 0.0001f || std::abs(settings.saturation - 1.0f) > 0.0001f || glm::length(settings.colorFilter - glm::vec3(1.0f)) > 0.0001f;
        const bool hasLensFx = settings.vignetteIntensity > 0.0001f || settings.chromaticAmount > 0.000001f || settings.sharpenStrength > 0.0001f || settings.grainAmount > 0.0001f || settings.scanlineIntensity > 0.0001f;
        return settings.enabled && (hasDither || hasColorAdjust || hasLensFx);
    }
    struct ScopedLighting2DState {
        GLint framebuffer = 0;
        GLint viewport[4] = {0, 0, 0, 0};
        GLint currentProgram = 0;
        GLint vertexArray = 0;
        GLint arrayBuffer = 0;
        GLint activeTexture = GL_TEXTURE0;
        GLint textureBindings[4] = {0, 0, 0, 0};
        GLint blendSrcRgb = GL_ONE;
        GLint blendDstRgb = GL_ZERO;
        GLint blendSrcAlpha = GL_ONE;
        GLint blendDstAlpha = GL_ZERO;
        GLint blendEquationRgb = GL_FUNC_ADD;
        GLint blendEquationAlpha = GL_FUNC_ADD;
        GLint depthFunc = GL_LESS;
        GLboolean blendEnabled = GL_FALSE;
        GLboolean depthTestEnabled = GL_FALSE;
        GLboolean cullFaceEnabled = GL_FALSE;
        GLboolean stencilTestEnabled = GL_FALSE;
        GLboolean scissorTestEnabled = GL_FALSE;
        GLboolean depthMask = GL_TRUE;
        ScopedLighting2DState() {
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
            glGetIntegerv(GL_VIEWPORT, viewport);
            glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
            for (int i = 0; i < 4; ++i) {
                glActiveTexture(GL_TEXTURE0 + i);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBindings[i]);
            }
            glActiveTexture(activeTexture);
            glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
            glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
            glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRgb);
            glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);
            glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
            blendEnabled = glIsEnabled(GL_BLEND);
            depthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
            cullFaceEnabled = glIsEnabled(GL_CULL_FACE);
            stencilTestEnabled = glIsEnabled(GL_STENCIL_TEST);
            scissorTestEnabled = glIsEnabled(GL_SCISSOR_TEST);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        }
        ~ScopedLighting2DState() {
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            if (blendEnabled) glEnable(GL_BLEND);
            else glDisable(GL_BLEND);
            if (depthTestEnabled) glEnable(GL_DEPTH_TEST);
            else glDisable(GL_DEPTH_TEST);
            if (cullFaceEnabled) glEnable(GL_CULL_FACE);
            else glDisable(GL_CULL_FACE);
            if (stencilTestEnabled) glEnable(GL_STENCIL_TEST);
            else glDisable(GL_STENCIL_TEST);
            if (scissorTestEnabled) glEnable(GL_SCISSOR_TEST);
            else glDisable(GL_SCISSOR_TEST);
            glDepthMask(depthMask);
            glDepthFunc(depthFunc);
            glBlendFuncSeparate(blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha);
            glBlendEquationSeparate(blendEquationRgb, blendEquationAlpha);
            for (int i = 0; i < 4; ++i) {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(textureBindings[i]));
            }
            glActiveTexture(activeTexture);
            glUseProgram(static_cast<GLuint>(currentProgram));
            glBindVertexArray(static_cast<GLuint>(vertexArray));
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
        }
    };
    float Cross2D(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
        const glm::vec2 ab = b - a;
        const glm::vec2 ac = c - a;
        return ab.x * ac.y - ab.y * ac.x;
    }
    float SignedArea(const std::vector<glm::vec2>& points) {
        float area = 0.0f;
        const size_t count = points.size();
        for (size_t i = 0; i < count; ++i) {
            const glm::vec2& a = points[i];
            const glm::vec2& b = points[(i + 1) % count];
            area += (a.x * b.y) - (b.x * a.y);
        }   return area * 0.5f;
    }
    bool PointInTriangle(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
        const float w0 = Cross2D(a, b, p);
        const float w1 = Cross2D(b, c, p);
        const float w2 = Cross2D(c, a, p);
        const bool hasNegative = (w0 < 0.0f) || (w1 < 0.0f) || (w2 < 0.0f);
        const bool hasPositive = (w0 > 0.0f) || (w1 > 0.0f) || (w2 > 0.0f);
        return !(hasNegative && hasPositive);
    }
    bool SegmentsIntersect(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& q0, const glm::vec2& q1) {
        auto orient = [](const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
            const float cross = Cross2D(a, b, c);
            if (cross > 1e-5f) return 1;
            if (cross < -1e-5f) return -1;
            return 0;
        };
        auto onSegment = [](const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
            return c.x >= std::min(a.x, b.x) - 1e-5f && c.x <= std::max(a.x, b.x) + 1e-5f && c.y >= std::min(a.y, b.y) - 1e-5f && c.y <= std::max(a.y, b.y) + 1e-5f;
        };
        const int o1 = orient(p0, p1, q0);
        const int o2 = orient(p0, p1, q1);
        const int o3 = orient(q0, q1, p0);
        const int o4 = orient(q0, q1, p1);
        if (o1 != o2 && o3 != o4) return true;
        if (o1 == 0 && onSegment(p0, p1, q0)) return true;
        if (o2 == 0 && onSegment(p0, p1, q1)) return true;
        if (o3 == 0 && onSegment(q0, q1, p0)) return true;
        if (o4 == 0 && onSegment(q0, q1, p1)) return true;
        return false;
    }
    glm::vec2 ComputeBoundsMin(const std::vector<glm::vec2>& points) {
        glm::vec2 value(std::numeric_limits<float>::max());
        for (const glm::vec2& point : points) {
            value.x = std::min(value.x, point.x);
            value.y = std::min(value.y, point.y);
        }   return value;
    }
    glm::vec2 ComputeBoundsMax(const std::vector<glm::vec2>& points) {
        glm::vec2 value(-std::numeric_limits<float>::max());
        for (const glm::vec2& point : points) {
            value.x = std::max(value.x, point.x);
            value.y = std::max(value.y, point.y);
        }   return value;
    }
    glm::vec2 ComputePolygonCentroid(const std::vector<glm::vec2>& points, const glm::vec2& fallback) {
        if (points.size() < 3) return fallback;
        float doubledArea = 0.0f;
        glm::vec2 centroid(0.0f);
        for (size_t i = 0; i < points.size(); ++i) {
            const glm::vec2& a = points[i];
            const glm::vec2& b = points[(i + 1) % points.size()];
            const float cross = (a.x * b.y) - (b.x * a.y);
            doubledArea += cross;
            centroid += (a + b) * cross;
        }
        if (std::abs(doubledArea) <= 1e-5f) {
            glm::vec2 average(0.0f);
            for (const glm::vec2& point : points) {
                average += point;
            }
            return average / static_cast<float>(points.size());
        }   return centroid / (3.0f * doubledArea);
    }
    void SetBlendStateForLight(Light2DBlendMode mode, Light2DOverlapOperation overlap) {
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        switch (mode) {
            case Light2DBlendMode::Additive:
            case Light2DBlendMode::Subtractive:
                if (overlap == Light2DOverlapOperation::Max) {
                    glBlendEquation(GL_MAX);
                    glBlendFunc(GL_ONE, GL_ONE);
                } else if (overlap == Light2DOverlapOperation::AlphaBlend) {
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                } else {
                    glBlendFunc(GL_ONE, GL_ONE);
                } break;
            case Light2DBlendMode::Multiply:
                glBlendFunc(GL_DST_COLOR, GL_ZERO); break;
        }
    }
    bool BoundsIntersectViewport(const glm::vec2& boundsMin, const glm::vec2& boundsMax, int width, int height, float margin = 0.0f) {
        const float viewportWidth = static_cast<float>(std::max(1, width));
        const float viewportHeight = static_cast<float>(std::max(1, height));
        const float m = std::max(0.0f, margin);
        return boundsMax.x >= -m && boundsMin.x <= viewportWidth + m && boundsMax.y >= -m && boundsMin.y <= viewportHeight + m;
    }
    float MaxLightComponent(const glm::vec4& color) {
        return std::max(std::max(color.r, color.g), std::max(color.b, color.a));
    }
    bool IsLightWorthRendering(const Light2DScreenLight& light, int width, int height) {
        if (!light.enabled) return false;
        const float peakContribution = light.intensity * MaxLightComponent(light.color);
        if (peakContribution <= kNegligibleLightContribution) return false;
        if (light.type == Light2DType::Global) return true;
        if (light.type == Light2DType::Freeform && light.polygon.size() < 3) return false;
        const glm::vec2 extent = glm::max(light.boundsMax - light.boundsMin, glm::vec2(0.0f));
        if (extent.x < kMinVisibleLightExtent && extent.y < kMinVisibleLightExtent) return false;
        if (!light.cullWhenOffscreen) return true;
        return BoundsIntersectViewport(light.boundsMin, light.boundsMax, width, height, light.offscreenCullMarginPx);
    }
    uint8_t BlendModeMaskForStyle(const Light2DBlendStyleDefinition& blendStyle) {
        switch (blendStyle.mode) {
            case Light2DBlendMode::Additive: return 1u;
            case Light2DBlendMode::Multiply: return 2u;
            case Light2DBlendMode::Subtractive: return 4u;
        }   return 0;
    }
    bool SameLightList(const std::vector<const Light2DScreenLight*>& a, const std::vector<const Light2DScreenLight*>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i] != b[i]) return false;
        }
        return true;
    }
}

bool Light2DHasVisiblePostFx(const Light2DPostFXSettings& settings) {return HasVisiblePostFxInternal(settings);}
uint32_t Light2DLayerBit(int layer) {
    const int clamped = std::clamp(layer, 0, 31);
    return (1u << static_cast<uint32_t>(clamped));
}
bool Light2DLayerMaskContains(uint32_t mask, int layer) {return (mask & Light2DLayerBit(layer)) != 0u;}
uint64_t Light2DHashShape(const std::vector<glm::vec2>& points) {
    uint64_t hash = 1469598103934665603ull;
    auto hashFloat = [&hash](float value) {
        uint32_t packed = 0;
        static_assert(sizeof(uint32_t) == sizeof(float), "Unexpected float packing size.");
        std::memcpy(&packed, &value, sizeof(uint32_t));
        hash ^= static_cast<uint64_t>(packed);
        hash *= 1099511628211ull;
    };
    hash ^= static_cast<uint64_t>(points.size());
    hash *= 1099511628211ull;
    for (const glm::vec2& point : points) {
        hashFloat(point.x);
        hashFloat(point.y);
    }   return hash;
}
bool Light2DPolygonSelfIntersects(const std::vector<glm::vec2>& points) {
    if (points.size() < 4) return false;
    const size_t count = points.size();
    for (size_t i = 0; i < count; ++i) {
        const glm::vec2& a0 = points[i];
        const glm::vec2& a1 = points[(i + 1) % count];
        for (size_t j = i + 1; j < count; ++j) {
            const size_t nextJ = (j + 1) % count;
            if (i == j || (i + 1) % count == j || i == nextJ) continue;
            const glm::vec2& b0 = points[j];
            const glm::vec2& b1 = points[nextJ];
            if (SegmentsIntersect(a0, a1, b0, b1)) return true;
        }
    }   return false;
}

// TODO: i should probably fix up that odd feather bug that causes it to render past the feather effects.
bool Light2DValidatePolygon(const std::vector<glm::vec2>& points, std::string* outError) {
    if (points.size() < 3)                                       {if (outError) *outError = "Hey! You need at least three points!"; return false;}
    if (points.size() > static_cast<size_t>(kMaxFreeformPoints)) {if (outError) *outError = "Hey! The shape exceeds Modularity's freeform point limit."; return false;}
    const float area = std::abs(SignedArea(points));
    if (area <= 1e-4f)                                           {if (outError) *outError = "The shape area is too small."; return false;}
    if (Light2DPolygonSelfIntersects(points))                    {if (outError) *outError = "The shape is self-intersecting."; return false;}
    if (outError) outError->clear();
    return true;
}
bool Light2DTriangulatePolygon(const std::vector<glm::vec2>& inputPoints, std::vector<unsigned int>& outIndices, std::string* outError) {
    outIndices.clear();
    if (!Light2DValidatePolygon(inputPoints, outError)) return false;
    std::vector<unsigned int> polygon(inputPoints.size());
    for (unsigned int i = 0; i < static_cast<unsigned int>(inputPoints.size()); ++i) polygon[i] = i;
    if (SignedArea(inputPoints) < 0.0f) std::reverse(polygon.begin(), polygon.end());
    int guard = 0;
    while (polygon.size() > 3 && guard < 4096) {
        bool clippedEar = false;
        for (size_t i = 0; i < polygon.size(); ++i) {
            const size_t prev = (i + polygon.size() - 1) % polygon.size();
            const size_t next = (i + 1) % polygon.size();
            const unsigned int ia = polygon[prev];
            const unsigned int ib = polygon[i];
            const unsigned int ic = polygon[next];
            const glm::vec2& a = inputPoints[ia];
            const glm::vec2& b = inputPoints[ib];
            const glm::vec2& c = inputPoints[ic];
            if (Cross2D(a, b, c) <= 1e-5f) continue;
            bool containsVertex = false;
            for (size_t p = 0; p < polygon.size(); ++p) {
                const unsigned int candidate = polygon[p];
                if (candidate == ia || candidate == ib || candidate == ic) continue;
                if (PointInTriangle(inputPoints[candidate], a, b, c)) {containsVertex = true; break;}
            }
            if (containsVertex) continue;
            outIndices.push_back(ia);
            outIndices.push_back(ib);
            outIndices.push_back(ic);
            polygon.erase(polygon.begin() + static_cast<std::ptrdiff_t>(i));
            clippedEar = true;
            break;
        }
        if (!clippedEar) {
            if (outError) *outError = "Triangulation failed.";
            outIndices.clear(); return false;
        }   ++guard;
    }
    if (polygon.size() == 3) {
        outIndices.push_back(polygon[0]);
        outIndices.push_back(polygon[1]);
        outIndices.push_back(polygon[2]);
    }
    if (outIndices.empty()) {
        if (outError) *outError = "Triangulation produced no triangles."; return false;
    }
    if (outError) outError->clear(); return true;
}
Lighting2DRenderer::~Lighting2DRenderer() {shutdown();}
void Lighting2DRenderer::shutdown() {
    destroyTarget(finalTarget);
    destroyTarget(postTarget);
    destroyTarget(additiveTarget);
    destroyTarget(multiplyTarget);
    destroyTarget(subtractiveTarget);
    if (quadVbo != 0) glDeleteBuffers(1, &quadVbo);
    if (quadVao != 0) glDeleteVertexArrays(1, &quadVao);
    if (spriteVbo != 0) glDeleteBuffers(1, &spriteVbo);
    if (spriteVao != 0) glDeleteVertexArrays(1, &spriteVao);
    if (polygonEbo != 0) glDeleteBuffers(1, &polygonEbo);
    if (polygonVbo != 0) glDeleteBuffers(1, &polygonVbo);
    if (polygonVao != 0) glDeleteVertexArrays(1, &polygonVao);
    quadVbo = 0;
    quadVao = 0;
    spriteVbo = 0;
    spriteVao = 0;
    polygonEbo = 0;
    polygonVbo = 0;
    polygonVao = 0;
    lightQuadShader.reset();
    lightFreeformShader.reset();
    spriteShader.reset();
    postFxShader.reset();
    initialized = false;
}
const Light2DPolygonCache* Lighting2DRenderer::getPolygonCache(int objectId) const {
    auto it = polygonCache.find(objectId);
    return (it != polygonCache.end()) ? &it->second : nullptr;
}
const Light2DPolygonCache& Lighting2DRenderer::updatePolygonCache(int objectId, const Light2DComponent& component) {
    Light2DPolygonCache& cache = polygonCache[objectId];
    const uint64_t version = Light2DHashShape(component.shapePoints) ^
                             (static_cast<uint64_t>(component.type) << 56);
    if (cache.version == version) {
        lastStats.polygonCacheHits += cache.valid ? 1 : 0; return cache;
    }
    cache = Light2DPolygonCache{};
    cache.version = version;
    cache.vertices = component.shapePoints;
    if (!cache.vertices.empty()) {
        cache.boundsMin = ComputeBoundsMin(cache.vertices);
        cache.boundsMax = ComputeBoundsMax(cache.vertices);
    }
    cache.selfIntersecting = Light2DPolygonSelfIntersects(component.shapePoints);
    cache.valid = Light2DTriangulatePolygon(component.shapePoints, cache.indices, &cache.error);
    if (!cache.valid) lastStats.triangulationFailures += 1;
    lastStats.polygonCacheMisses += 1;
    return cache;
}
void Lighting2DRenderer::clearPolygonCache(int objectId) {polygonCache.erase(objectId);}
void Lighting2DRenderer::initializeIfNeeded() {
    if (initialized) return;
    ensureGeometry();
    ensureShaders();
    initialized = true;
}
void Lighting2DRenderer::destroyTarget(RenderTarget& target) {
    if (target.texture != 0) glDeleteTextures(1, &target.texture);
    if (target.fbo != 0) glDeleteFramebuffers(1, &target.fbo);
    target = RenderTarget{};
}
void Lighting2DRenderer::ensureTarget(RenderTarget& target, int width, int height, int filter, unsigned int internalFormat) {
    const int safeWidth = std::max(1, width);
    const int safeHeight = std::max(1, height);
    if (target.fbo != 0 && target.width == safeWidth && target.height == safeHeight &&
        target.internalFormat == internalFormat) {
        if (target.filter != filter) {
            glBindTexture(GL_TEXTURE_2D, target.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
            glBindTexture(GL_TEXTURE_2D, 0);
            target.filter = filter;
        }
        return;
    }
    destroyTarget(target);
    const GLenum uploadType = (internalFormat == GL_RGBA16F) ? GL_FLOAT : GL_UNSIGNED_BYTE;
    glGenFramebuffers(1, &target.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
    glGenTextures(1, &target.texture);
    glBindTexture(GL_TEXTURE_2D, target.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), safeWidth, safeHeight, 0, GL_RGBA, uploadType, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        destroyTarget(target);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    target.width = safeWidth;
    target.height = safeHeight;
    target.filter = filter;
    target.internalFormat = internalFormat;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
void Lighting2DRenderer::ensureGeometry() {
    if (quadVao == 0) {
        const std::array<float, 24> quadVertices = {
            -1.0f, -1.0f, 0.0f, 0.0f,
             1.0f, -1.0f, 1.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 1.0f,
            -1.0f, -1.0f, 0.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 1.0f,
            -1.0f,  1.0f, 0.0f, 1.0f
        };
        glGenVertexArrays(1, &quadVao);
        glGenBuffers(1, &quadVbo);
        glBindVertexArray(quadVao);
        glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, reinterpret_cast<void*>(sizeof(float) * 2));
        glBindVertexArray(0);
    }
    if (spriteVao == 0) {
        glGenVertexArrays(1, &spriteVao);
        glGenBuffers(1, &spriteVbo);
        glBindVertexArray(spriteVao);
        glBindBuffer(GL_ARRAY_BUFFER, spriteVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(SpriteGpuVertex) * 1024, nullptr, GL_DYNAMIC_DRAW);
        spriteVboCapacity = 1024;
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteGpuVertex), reinterpret_cast<void*>(offsetof(SpriteGpuVertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteGpuVertex), reinterpret_cast<void*>(offsetof(SpriteGpuVertex, uv)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(SpriteGpuVertex), reinterpret_cast<void*>(offsetof(SpriteGpuVertex, color)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(SpriteGpuVertex), reinterpret_cast<void*>(offsetof(SpriteGpuVertex, params)));
        glBindVertexArray(0);
    }
    if (polygonVao == 0) {
        glGenVertexArrays(1, &polygonVao);
        glGenBuffers(1, &polygonVbo);
        glGenBuffers(1, &polygonEbo);
        glBindVertexArray(polygonVao);
        glBindBuffer(GL_ARRAY_BUFFER, polygonVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(glm::vec2) * kMaxFreeformPoints, nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), reinterpret_cast<void*>(0));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, polygonEbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned int) * kMaxFreeformPoints * 3, nullptr, GL_DYNAMIC_DRAW);
        glBindVertexArray(0);
    }
}
void Lighting2DRenderer::ensureShaders() {
    if (!lightQuadShader)     {lightQuadShader =     std::make_unique<Shader>("Resources/Shaders/light2d_light.vert", "Resources/Shaders/light2d_light.frag");}
    if (!lightFreeformShader) {lightFreeformShader = std::make_unique<Shader>("Resources/Shaders/light2d_freeform.vert", "Resources/Shaders/light2d_freeform.frag");}
    if (!spriteShader)        {spriteShader =        std::make_unique<Shader>("Resources/Shaders/light2d_sprite.vert", "Resources/Shaders/light2d_sprite.frag");}
    if (!postFxShader)        {postFxShader =        std::make_unique<Shader>("Resources/Shaders/postfx_vert.glsl", "Resources/Shaders/light2d_dither_frag.glsl");}
}
void Lighting2DRenderer::drawFullscreenQuad() const {
    glBindVertexArray(quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}
void Lighting2DRenderer::drawUnitQuad() const {drawFullscreenQuad();}
void Lighting2DRenderer::clearTarget(RenderTarget& target, const glm::vec4& color) const {
    if (target.fbo == 0 || target.width <= 0 || target.height <= 0) return;
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
    glViewport(0, 0, target.width, target.height);
    const GLfloat clearColor[4] = { color.r, color.g, color.b, color.a };
    glClearBufferfv(GL_COLOR, 0, clearColor);
}
float Lighting2DRenderer::resolveLightingBufferScale(const Light2DRenderRequest& request) const {
    float scale = (request.lightingBufferScale > 0.0f) ? std::clamp(request.lightingBufferScale, 0.25f, 1.0f) : std::clamp(lightingBufferScale, 0.25f, 1.0f);
    if (!request.lightingBufferScaleAuto) return scale;
    const int maxDimension = std::max(request.width, request.height);
    const size_t movingCosts = request.lights.size();
    if (maxDimension >= 2560) {scale *= 0.75f;}
    else if (maxDimension >= 1920) {scale *= 0.85f;}
    if (movingCosts > 24) {scale *= 0.80f;}
    else if (movingCosts > 12) {scale *= 0.90f;}
#ifdef MODULARITY_PLAYER
    if      (maxDimension >= 2560) {scale *= 0.80f;}
    else if (maxDimension >= 1920) {scale *= 0.88f;}
    if      (movingCosts > 32)     {scale *= 0.72f;}
    else if (movingCosts > 16)     {scale *= 0.82f;}
    else if (movingCosts > 8)      {scale *= 0.90f;}
    return std::clamp(scale, 0.375f, 1.0f);
#else
    return std::clamp(scale, 0.5f, 1.0f);
#endif
}
void Lighting2DRenderer::clearLightTargets(uint8_t blendModesMask) {
    glDisable(GL_BLEND);
    if ((blendModesMask & 1u) != 0u) {
        clearTarget(additiveTarget, glm::vec4(0.0f));
    }
    if ((blendModesMask & 2u) != 0u) {
        clearTarget(multiplyTarget, glm::vec4(1.0f));
    }
    if ((blendModesMask & 4u) != 0u) {
        clearTarget(subtractiveTarget, glm::vec4(0.0f));
    }
}
void Lighting2DRenderer::renderLightPass(const Light2DRenderRequest& request, Renderer& renderer, int layer, const std::vector<const Light2DScreenLight*>& lights, float lightingScale) {
    (void)renderer;
    if (lights.empty() || !lightQuadShader || !lightFreeformShader) return;
    const glm::vec2 scaleVec(lightingScale);
    Shader* activeShader = nullptr;
    RenderTarget* activeTarget = nullptr;
    GLuint activeVao = 0;
    unsigned int currentCookieTexture = 0;
    Light2DBlendMode lastBlendMode = static_cast<Light2DBlendMode>(-1);
    Light2DOverlapOperation lastOverlapOp = static_cast<Light2DOverlapOperation>(-1);
    bool blendStateInitialized = false;
    for (const Light2DScreenLight* light : lights) {
        if (!light || !light->enabled) continue;
        if (!light->targetAllLayers && !Light2DLayerMaskContains(light->targetLayerMask, layer)) continue;
        const int blendIndex = std::clamp(light->blendStyle, 0, static_cast<int>(request.blendStyles.size()) - 1);
        const Light2DBlendStyleDefinition& blendStyle = request.blendStyles[blendIndex];
        RenderTarget* target = &additiveTarget;
        switch (blendStyle.mode) {
            case Light2DBlendMode::Additive: target = &additiveTarget; break;
            case Light2DBlendMode::Multiply: target = &multiplyTarget; break;
            case Light2DBlendMode::Subtractive: target = &subtractiveTarget; break;
        }
        const bool targetChanged = (activeTarget != target);
        if (targetChanged) {
            activeTarget = target;
            glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
            glViewport(0, 0, target->width, target->height);
        }
        if (!blendStateInitialized || lastBlendMode != blendStyle.mode || lastOverlapOp != light->overlapOperation) {
            SetBlendStateForLight(blendStyle.mode, light->overlapOperation);
            lastBlendMode = blendStyle.mode;
            lastOverlapOp = light->overlapOperation;
            blendStateInitialized = true;
        }
        Shader* shader = (light->type == Light2DType::Freeform) ? lightFreeformShader.get() : lightQuadShader.get();
        const bool shaderChanged = (activeShader != shader);
        if (shaderChanged) {
            activeShader = shader;
            activeShader->use();
            shader->setInt("u_cookieTex", 0);
        }
        if (targetChanged || shaderChanged) {
            shader->setVec2("u_viewportSize", glm::vec2(static_cast<float>(target->width), static_cast<float>(target->height)));
        }
        shader->setVec4("u_lightColor", light->color * blendStyle.modulate);
        shader->setFloat("u_intensity", light->intensity * blendStyle.intensityScale);
        shader->setFloat("u_radius", light->radius * lightingScale);
        shader->setFloat("u_innerRadius", light->innerRadius * lightingScale);
        shader->setFloat("u_outerRadius", light->outerRadius * lightingScale);
        shader->setFloat("u_falloffStrength", light->falloffStrength);
        const float halfInnerAngle = glm::radians(std::clamp(light->innerSpotAngle, 0.0f, 360.0f) * 0.5f);
        const float halfOuterAngle = glm::radians(std::clamp(light->outerSpotAngle, 0.0f, 360.0f) * 0.5f);
        shader->setVec2("u_spotAngleCos", glm::vec2(std::cos(halfInnerAngle), std::cos(halfOuterAngle)));
        shader->setFloat("u_rotation", light->rotationRad);
        shader->setVec2("u_lightPos", light->position * scaleVec);
        shader->setVec2("u_boundsMin", light->boundsMin * scaleVec);
        shader->setVec2("u_boundsMax", light->boundsMax * scaleVec);
        shader->setFloat("u_freeformFeather", light->freeformFeatherPx * lightingScale);
        shader->setFloat("u_freeformEdgeFalloff", light->freeformEdgeFalloff);
        shader->setInt("u_blendMode", static_cast<int>(blendStyle.mode));
        shader->setInt("u_lightType", static_cast<int>(light->type));
        shader->setBool("u_useCookie", light->cookieTextureId != 0);
        shader->setVec2("u_cookieScale", light->cookieScale);
        shader->setFloat("u_cookieRotation", light->cookieRotationRad);
        shader->setBool("u_shadowEnabled", false);
        if (light->cookieTextureId != 0) {
            if (currentCookieTexture != light->cookieTextureId) {
                currentCookieTexture = light->cookieTextureId;
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, light->cookieTextureId);
            }
        }
        if (light->type == Light2DType::Freeform) {
            const int pointCount = std::min(static_cast<int>(light->polygon.size()), kMaxFreeformPoints);
            shader->setInt("u_polygonPointCount", pointCount);
            polygonVerticesScratch.resize(static_cast<size_t>(pointCount));
            const auto& polygonUniformNames = FreeformPointUniformNames();
            for (int i = 0; i < pointCount; ++i) {
                const glm::vec2 point = light->polygon[static_cast<size_t>(i)] * scaleVec;
                polygonVerticesScratch[static_cast<size_t>(i)] = point;
                shader->setVec2(polygonUniformNames[static_cast<size_t>(i)], point);
            }
            if (!Light2DValidatePolygon(polygonVerticesScratch, nullptr)) {
                lastStats.triangulationFailures += 1; continue;
            }
            const glm::vec2 polygonCenter = ComputePolygonCentroid(polygonVerticesScratch, light->position * scaleVec);
            shader->setVec2("u_lightPos", polygonCenter);
            const glm::vec2 scaledBoundsMin = light->boundsMin * scaleVec;
            const glm::vec2 scaledBoundsMax = light->boundsMax * scaleVec;
            const float outerExtent = std::max(light->outerRadius, light->radius) * lightingScale;
            const glm::vec2 expandedMin = scaledBoundsMin - glm::vec2(outerExtent);
            const glm::vec2 expandedMax = scaledBoundsMax + glm::vec2(outerExtent);
            const std::array<glm::vec2, 4> quadVertices = {{
                glm::vec2(expandedMin.x, expandedMin.y),
                glm::vec2(expandedMax.x, expandedMin.y),
                glm::vec2(expandedMax.x, expandedMax.y),
                glm::vec2(expandedMin.x, expandedMax.y)
            }};
            static constexpr std::array<unsigned int, 6> quadIndices = {{ 0u, 1u, 2u, 0u, 2u, 3u }};
            if (activeVao != polygonVao) {
                activeVao = polygonVao;
                glBindVertexArray(polygonVao);
            }
            glBindBuffer(GL_ARRAY_BUFFER, polygonVbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(glm::vec2) * quadVertices.size(), quadVertices.data());
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, polygonEbo);
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(unsigned int) * quadIndices.size(), quadIndices.data());
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(quadIndices.size()), GL_UNSIGNED_INT, nullptr);
        } else {
            if (activeVao != quadVao) {
                activeVao = quadVao;
                glBindVertexArray(quadVao);
            }   glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        lastStats.drawCalls += 1;
    }
}
void Lighting2DRenderer::renderSpritePass(const Light2DRenderRequest& request, int layer, size_t spriteBegin, size_t spriteEnd, uint8_t blendModesMask) {
    (void)request;
    (void)layer;
    if (!spriteShader || spriteBegin >= spriteEnd || spriteBegin >= orderedSpritesScratch.size()) return;
    spriteEnd = std::min(spriteEnd, orderedSpritesScratch.size());
    glBindFramebuffer(GL_FRAMEBUFFER, finalTarget.fbo);
    glViewport(0, 0, finalTarget.width, finalTarget.height);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    spriteShader->use();
    spriteShader->setBool("u_hasAdditiveLight", (blendModesMask & 1u) != 0u);
    spriteShader->setBool("u_hasMultiplyLight", (blendModesMask & 2u) != 0u);
    spriteShader->setBool("u_hasSubtractiveLight", (blendModesMask & 4u) != 0u);
    spriteVertices.clear();
    spriteVertices.reserve((spriteEnd - spriteBegin) * 6);
    unsigned int currentTexture = 0;
    glBindVertexArray(spriteVao);
    glBindBuffer(GL_ARRAY_BUFFER, spriteVbo);
    auto flush = [&]() {
        if (spriteVertices.empty() || currentTexture == 0) {spriteVertices.clear(); return;}
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, currentTexture);
        const size_t vertexCount = spriteVertices.size();
        if (vertexCount > spriteVboCapacity) {
            spriteVboCapacity = std::max(vertexCount, std::max<size_t>(spriteVboCapacity * 2, 1024));
            glBufferData(GL_ARRAY_BUFFER, sizeof(SpriteGpuVertex) * spriteVboCapacity, nullptr, GL_DYNAMIC_DRAW);
        }
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(SpriteGpuVertex) * vertexCount, spriteVertices.data());
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(spriteVertices.size()));
        lastStats.drawCalls += 1;
        spriteVertices.clear();
    };
    for (size_t index = spriteBegin; index < spriteEnd; ++index) {
        const Light2DScreenSprite* sprite = orderedSpritesScratch[index];
        if (!sprite || sprite->textureId == 0) {flush(); currentTexture = 0; continue;}
        if (currentTexture != 0 && currentTexture != sprite->textureId) flush();
        currentTexture = sprite->textureId;
        const glm::vec4 params(sprite->receiveLighting ? 1.0f : 0.0f, sprite->unlit ? 1.0f : 0.0f, sprite->emissiveIntensity, 0.0f);
        auto pushVertex = [&](int index) {
            SpriteGpuVertex vertex;
            vertex.position = sprite->positions[index];
            vertex.uv = sprite->uvs[index];
            vertex.color = sprite->tint;
            vertex.params = params;
            spriteVertices.push_back(vertex);
        };
        pushVertex(0);
        pushVertex(1);
        pushVertex(2);
        pushVertex(0);
        pushVertex(2);
        pushVertex(3);
    }   flush();
}
unsigned int Lighting2DRenderer::applyPostProcessing(const Light2DRenderRequest& request, unsigned int sourceTexture, int width, int height) {
    if (!Light2DHasVisiblePostFx(request.postFx) || sourceTexture == 0 || !postFxShader) return sourceTexture;
    const int safeWidth = std::max(1, width);
    const int safeHeight = std::max(1, height);
    const int targetFilter = request.lightingPixelFilter ? GL_NEAREST : GL_LINEAR;
    const unsigned int targetFormat = request.lightingSDR ? GL_RGBA8 : GL_RGBA16F;
    ensureTarget(postTarget, safeWidth, safeHeight, targetFilter, targetFormat);
    if (postTarget.fbo == 0) return sourceTexture;
    const int colorBits = std::clamp(request.postFx.colorBits, 1, 8);
    const float ditherIntensity = std::max(0.0f, request.postFx.ditherIntensity);
    const float darkAdjustment = std::clamp(request.postFx.darkAdjustment, 0.0f, 1.0f);
    const float ditherScale = std::clamp(request.postFx.ditherScale, 1.0f, 8.0f);
    const float pixelation = std::max(0.0f, request.postFx.pixelation);
    const float exposure = std::clamp(request.postFx.exposure, -8.0f, 8.0f);
    const float contrast = std::clamp(request.postFx.contrast, 0.0f, 2.5f);
    const float saturation = std::clamp(request.postFx.saturation, 0.0f, 2.5f);
    const glm::vec3 colorFilter = glm::clamp(request.postFx.colorFilter, glm::vec3(0.0f), glm::vec3(2.0f));
    const float vignetteIntensity = std::clamp(request.postFx.vignetteIntensity, 0.0f, 1.0f);
    const float vignetteSmoothness = std::clamp(request.postFx.vignetteSmoothness, 0.05f, 1.0f);
    const float chromaticAmount = std::clamp(request.postFx.chromaticAmount, 0.0f, 0.05f);
    const float sharpenStrength = std::clamp(request.postFx.sharpenStrength, 0.0f, 2.0f);
    const float grainAmount = std::clamp(request.postFx.grainAmount, 0.0f, 0.4f);
    const float scanlineIntensity = std::clamp(request.postFx.scanlineIntensity, 0.0f, 1.0f);
    const float timeSeconds = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
    glBindFramebuffer(GL_FRAMEBUFFER, postTarget.fbo);
    glViewport(0, 0, postTarget.width, postTarget.height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    postFxShader->use();
    postFxShader->setVec2("u_viewportSize", glm::vec2(static_cast<float>(postTarget.width), static_cast<float>(postTarget.height)));
    postFxShader->setInt("sceneTex", 0);
    postFxShader->setFloat("u_ditherIntensity", ditherIntensity);
    postFxShader->setInt("u_colorBits", colorBits);
    postFxShader->setFloat("u_darkAdjustment", darkAdjustment);
    postFxShader->setFloat("u_ditherScale", ditherScale);
    postFxShader->setFloat("u_pixelation", pixelation);
    postFxShader->setFloat("u_exposure", exposure);
    postFxShader->setFloat("u_contrast", contrast);
    postFxShader->setFloat("u_saturation", saturation);
    postFxShader->setVec3("u_colorFilter", colorFilter);
    postFxShader->setFloat("u_vignetteIntensity", vignetteIntensity);
    postFxShader->setFloat("u_vignetteSmoothness", vignetteSmoothness);
    postFxShader->setFloat("u_chromaticAmount", chromaticAmount);
    postFxShader->setFloat("u_sharpenStrength", sharpenStrength);
    postFxShader->setFloat("u_grainAmount", grainAmount);
    postFxShader->setFloat("u_scanlineIntensity", scanlineIntensity);
    postFxShader->setFloat("u_time", timeSeconds);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    drawFullscreenQuad();
    return postTarget.texture;
}
void Lighting2DRenderer::renderLayer(const Light2DRenderRequest& request, Renderer& renderer, const LayerBatch& batch, float lightingScale, bool clearLightBuffers) {
    if (clearLightBuffers) clearLightTargets(batch.blendModesMask);
    if (!batch.lights.empty()) renderLightPass(request, renderer, batch.layer, batch.lights, lightingScale);
    renderSpritePass(request, batch.layer, batch.spriteBegin, batch.spriteEnd, batch.blendModesMask);
}
unsigned int Lighting2DRenderer::render(const Light2DRenderRequest& request, Renderer& renderer) {
    initializeIfNeeded();
    lastStats = Light2DDebugStats{};
    if (!initialized || request.width <= 0 || request.height <= 0 || request.sprites.empty()) return 0;
    const ScopedLighting2DState restoreState;
    const auto start = std::chrono::steady_clock::now();
    const int targetFilter = request.lightingPixelFilter ? GL_NEAREST : GL_LINEAR;
    const unsigned int targetFormat = request.lightingSDR ? GL_RGBA8 : GL_RGBA16F;
    ensureTarget(finalTarget, request.width, request.height, targetFilter, targetFormat);
    if (finalTarget.fbo == 0) return 0;
    const float lightingScale = resolveLightingBufferScale(request);
    const int lightingWidth = std::max(1, static_cast<int>(std::ceil(static_cast<float>(request.width) * lightingScale)));
    const int lightingHeight = std::max(1, static_cast<int>(std::ceil(static_cast<float>(request.height) * lightingScale)));
    glBindFramebuffer(GL_FRAMEBUFFER, finalTarget.fbo);
    glViewport(0, 0, finalTarget.width, finalTarget.height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(request.clearColor.r, request.clearColor.g, request.clearColor.b, request.clearColor.a);
    glClear(GL_COLOR_BUFFER_BIT);
    ensureTarget(additiveTarget, lightingWidth, lightingHeight, targetFilter, targetFormat);
    ensureTarget(multiplyTarget, lightingWidth, lightingHeight, targetFilter, targetFormat);
    ensureTarget(subtractiveTarget, lightingWidth, lightingHeight, targetFilter, targetFormat);
    if (additiveTarget.fbo == 0 || multiplyTarget.fbo == 0 || subtractiveTarget.fbo == 0) return 0;
    orderedSpritesScratch.clear();
    orderedSpritesScratch.reserve(request.sprites.size());
    for (const Light2DScreenSprite& sprite : request.sprites) orderedSpritesScratch.push_back(&sprite);
    std::sort(orderedSpritesScratch.begin(), orderedSpritesScratch.end(), [](const Light2DScreenSprite* a, const Light2DScreenSprite* b) {
        if (a->layer != b->layer) return a->layer < b->layer;
        if (a->drawOrder != b->drawOrder) return a->drawOrder < b->drawOrder;
        return a->objectId < b->objectId;
    });

    orderedLightsScratch.clear();
    orderedLightsScratch.reserve(request.lights.size());
    for (const Light2DScreenLight& light : request.lights) {
        if (IsLightWorthRendering(light, request.width, request.height)) orderedLightsScratch.push_back(&light);
    }
    std::sort(orderedLightsScratch.begin(), orderedLightsScratch.end(), [](const Light2DScreenLight* a, const Light2DScreenLight* b) {if (a->lightOrder != b->lightOrder) return a->lightOrder < b->lightOrder; return a->objectId < b->objectId;});
    size_t activeLayerBatchCount = 0;
    if (layerBatchesScratch.capacity() < orderedSpritesScratch.size()) layerBatchesScratch.reserve(orderedSpritesScratch.size());
    layerToBatchIndexScratch.clear();
    layerToBatchIndexScratch.reserve(orderedSpritesScratch.size());
    size_t spriteBegin = 0;
    while (spriteBegin < orderedSpritesScratch.size()) {
        const int layer = orderedSpritesScratch[spriteBegin]->layer;
        size_t spriteEnd = spriteBegin + 1;
        while (spriteEnd < orderedSpritesScratch.size() && orderedSpritesScratch[spriteEnd]->layer == layer) {++spriteEnd;}
        if (activeLayerBatchCount >= layerBatchesScratch.size()) layerBatchesScratch.emplace_back();
        LayerBatch& batch = layerBatchesScratch[activeLayerBatchCount];
        batch.layer = layer;
        batch.spriteBegin = spriteBegin;
        batch.spriteEnd = spriteEnd;
        batch.blendModesMask = 0;
        batch.lights.clear();
        layerToBatchIndexScratch[layer] = activeLayerBatchCount;
        ++activeLayerBatchCount;
        spriteBegin = spriteEnd;
    }
    for (size_t i = activeLayerBatchCount; i < layerBatchesScratch.size(); ++i)  layerBatchesScratch[i].lights.clear();
    layerBatchesScratch.resize(activeLayerBatchCount);
    const size_t estimatedLightsPerBatch = layerBatchesScratch.empty() ? 0 : std::max<size_t>(1, orderedLightsScratch.size() / layerBatchesScratch.size());
    for (LayerBatch& batch : layerBatchesScratch) batch.lights.reserve(estimatedLightsPerBatch);
    lastStats.shadowCasters = static_cast<int>(request.shadowCasters.size());
    for (const Light2DScreenLight* light : orderedLightsScratch) {
        if (light && light->type == Light2DType::Freeform) lastStats.freeformLights += 1;
    }
    for (const Light2DScreenLight* light : orderedLightsScratch) {
        if (!light || !light->enabled) continue;
        if (light->targetAllLayers) {
            for (LayerBatch& batch : layerBatchesScratch) {
                batch.lights.push_back(light);
                const int blendIndex = std::clamp(light->blendStyle, 0, static_cast<int>(request.blendStyles.size()) - 1);
                batch.blendModesMask |= BlendModeMaskForStyle(request.blendStyles[blendIndex]);
            }   continue;
        }
        for (int bit = 0; bit < 32; ++bit) {
            if ((light->targetLayerMask & (1u << static_cast<uint32_t>(bit))) == 0u) continue;
            auto batchIt = layerToBatchIndexScratch.find(bit);
            if (batchIt != layerToBatchIndexScratch.end()) {
                LayerBatch& batch = layerBatchesScratch[batchIt->second];
                batch.lights.push_back(light);
                const int blendIndex = std::clamp(light->blendStyle, 0, static_cast<int>(request.blendStyles.size()) - 1);
                batch.blendModesMask |= BlendModeMaskForStyle(request.blendStyles[blendIndex]);
            }
        }
    }
    spriteShader->use();
    spriteShader->setVec2("u_viewportSize", glm::vec2(static_cast<float>(finalTarget.width), static_cast<float>(finalTarget.height)));
    spriteShader->setVec3("u_baseAmbient", request.baseAmbient);
    spriteShader->setInt("u_spriteTex", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, additiveTarget.texture);
    spriteShader->setInt("u_additiveLightTex", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, multiplyTarget.texture);
    spriteShader->setInt("u_multiplyLightTex", 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, subtractiveTarget.texture);
    spriteShader->setInt("u_subtractiveLightTex", 3);
    const std::vector<const Light2DScreenLight*>* activeLightList = nullptr;
    bool lightBuffersValid = false;
    for (const LayerBatch& batch : layerBatchesScratch) {
        const bool needsLightRefresh = !lightBuffersValid || !activeLightList || !SameLightList(*activeLightList, batch.lights);
        renderLayer(request, renderer, batch, lightingScale, needsLightRefresh);
        activeLightList = &batch.lights;
        lightBuffersValid = true;
        lastStats.renderedLayers += 1;
    }
    lastStats.visibleSprites = static_cast<int>(orderedSpritesScratch.size());
    lastStats.visibleLights = static_cast<int>(orderedLightsScratch.size());
    const auto end = std::chrono::steady_clock::now();
    lastStats.cpuBuildMs = std::chrono::duration<float, std::milli>(end - start).count();
    return applyPostProcessing(request, finalTarget.texture, finalTarget.width, finalTarget.height);
}
