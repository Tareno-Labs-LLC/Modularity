// 3D rain: the Renderer half of the weather system's rain.
//
// The weather used to draw rain only as screen-space streaks in the post pass, which
// cannot be occluded (it rained indoors, under bridges and inside the car) and cannot be
// lit (it looked the same at noon and next to a street lamp at midnight). This draws it
// as geometry in the scene's own target instead, from Renderer::renderSceneInternal:
//
//  - Streaks: instanced camera-facing quads stretched along each drop's velocity relative
//    to the camera. Positions are a pure function of instance id, a clock and the camera
//    position (rain_vert.glsl), so nothing is uploaded per frame and drops stay fixed in
//    the world as the camera moves. A dense near box plus a sparse far box is the LOD.
//  - Occlusion: an orthographic depth render of the scene from straight above the camera.
//    A drop below the first surface in that map is under a roof and is not drawn. The map
//    is re-rendered only when the camera has moved or after a handful of frames.
//  - Splashes: short-lived rings and crowns on whatever surface the occlusion map says
//    rain lands on near the camera (rain_splash_vert.glsl).
//
// Lighting is approximate but physically motivated: ambient + sun + up to eight nearby
// point/spot lights, with a strong forward-scattering lobe so drops flare when backlit.

#include "Rendering.h"
#include "Camera.h"
#include "Profiler.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

// Instance budgets at rain amount 1 (Rain mode at full intensity is 0.75, a full
// Thunderstorm 1.0) and rainDensity 1.
constexpr int kRainNearBase = 14000;
constexpr int kRainFarBase = 9000;
constexpr int kRainSplashBase = 1400;
constexpr float kRainDensityMax = 4.0f;

// Near layer: x half extent, y column height, z exposure seconds, w streak width (m).
const glm::vec4 kRainNearLayer(12.0f, 16.0f, 0.045f, 0.005f);
const glm::vec4 kRainFarLayer(55.0f, 42.0f, 0.070f, 0.020f);
constexpr float kRainFallSpeed = 9.0f;      // m/s, terminal velocity of a ~2 mm drop
constexpr float kRainWindResponse = 1.25f;  // m/s of drift per unit of scene wind strength

constexpr float kSplashRadius = 16.0f;
constexpr float kSplashPeriod = 0.42f;

// Occlusion map: 120 m square at 1024^2 is ~12 cm per texel, enough for eaves and a
// car roof. Depth covers from well above the camera to well below it.
constexpr int kOcclusionResolution = 1024;
constexpr float kOcclusionHalfExtent = 60.0f;
constexpr float kOcclusionAbove = 200.0f;
constexpr float kOcclusionDepthRange = 450.0f;
constexpr float kOcclusionMoveThreshold = 1.0f;
constexpr int kOcclusionMaxAge = 20;  // frames; also catches moving occluders

constexpr double kRainClockWrap = 900.0;

const char* const kRainLightPosNames[8] = {
    "rainLightPosRange[0]", "rainLightPosRange[1]", "rainLightPosRange[2]", "rainLightPosRange[3]",
    "rainLightPosRange[4]", "rainLightPosRange[5]", "rainLightPosRange[6]", "rainLightPosRange[7]"};
const char* const kRainLightColorNames[8] = {
    "rainLightColorType[0]", "rainLightColorType[1]", "rainLightColorType[2]", "rainLightColorType[3]",
    "rainLightColorType[4]", "rainLightColorType[5]", "rainLightColorType[6]", "rainLightColorType[7]"};
const char* const kRainLightDirNames[8] = {
    "rainLightDirCone[0]", "rainLightDirCone[1]", "rainLightDirCone[2]", "rainLightDirCone[3]",
    "rainLightDirCone[4]", "rainLightDirCone[5]", "rainLightDirCone[6]", "rainLightDirCone[7]"};
const char* const kRainLightOuterNames[8] = {
    "rainLightOuterCos[0]", "rainLightOuterCos[1]", "rainLightOuterCos[2]", "rainLightOuterCos[3]",
    "rainLightOuterCos[4]", "rainLightOuterCos[5]", "rainLightOuterCos[6]", "rainLightOuterCos[7]"};

double RainWallSeconds() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Everything the rain passes touch, put back exactly: renderSceneInternal tracks its own
// GL state in locals this file cannot see.
struct RainGlState {
    GLboolean blend = GL_FALSE, depthTest = GL_FALSE, cull = GL_FALSE, polygonOffset = GL_FALSE;
    GLboolean depthMask = GL_TRUE;
    GLint depthFunc = GL_LESS;
    GLint blendSrcRgb = GL_ONE, blendDstRgb = GL_ZERO, blendSrcAlpha = GL_ONE, blendDstAlpha = GL_ZERO;
    GLint program = 0, vao = 0, arrayBuffer = 0, activeTexture = GL_TEXTURE0, texture0 = 0;
    GLint framebuffer = 0;
    GLint viewport[4] = {0, 0, 0, 0};

    void capture() {
        blend = glIsEnabled(GL_BLEND);
        depthTest = glIsEnabled(GL_DEPTH_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        polygonOffset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_VIEWPORT, viewport);
    }

    void restore() const {
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glBindVertexArray(static_cast<GLuint>(vao));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture0));
        glActiveTexture(static_cast<GLenum>(activeTexture));
        glUseProgram(static_cast<GLuint>(program));
        glBlendFuncSeparate(blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha);
        glDepthMask(depthMask);
        glDepthFunc(depthFunc);
        if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        if (depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        if (polygonOffset) glEnable(GL_POLYGON_OFFSET_FILL); else glDisable(GL_POLYGON_OFFSET_FILL);
    }
};

} // namespace

bool Renderer::isRain3DActive() const {
    if (!skybox || !weatherDrive.active) return false;
    const WeatherSettings& weather = skybox->getSettings().weather;
    return weather.enabled && weather.rain3D;
}

void Renderer::releaseRainResources() {
    if (rainState.occlusionDepth) glDeleteTextures(1, &rainState.occlusionDepth);
    if (rainState.occlusionFbo) glDeleteFramebuffers(1, &rainState.occlusionFbo);
    if (rainState.vao) glDeleteVertexArrays(1, &rainState.vao);
    rainState.occlusionDepth = 0;
    rainState.occlusionFbo = 0;
    rainState.vao = 0;
    rainState.occlusionResolution = 0;
    rainState.occlusionValid = false;
}

void Renderer::updateRainOcclusion(const glm::vec3& cameraPos, const std::vector<SceneObject>& sceneObjects,
                                   RainMeshResolver resolveMesh, void* resolveContext) {
    if (!directionalShadowDepthShader || directionalShadowDepthShader->ID == 0 || !resolveMesh) {
        rainState.occlusionValid = false;
        return;
    }

    if (rainState.occlusionFbo == 0 || rainState.occlusionDepth == 0 ||
        rainState.occlusionResolution != kOcclusionResolution) {
        releaseRainResources();
        GLint prevFbo = 0;
        GLint prevTexture = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);
        glGenFramebuffers(1, &rainState.occlusionFbo);
        glGenTextures(1, &rainState.occlusionDepth);
        glBindTexture(GL_TEXTURE_2D, rainState.occlusionDepth);
        // Sized 24-bit depth with GL_UNSIGNED_INT is valid on both desktop GL and GLES 3.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kOcclusionResolution, kOcclusionResolution, 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        glBindFramebuffer(GL_FRAMEBUFFER, rainState.occlusionFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, rainState.occlusionDepth, 0);
#if MODULARITY_OPENGL_ES
        const GLenum none = GL_NONE;
        glDrawBuffers(1, &none);
#else
        glDrawBuffer(GL_NONE);
#endif
        glReadBuffer(GL_NONE);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTexture));
        if (!complete) {
            std::cerr << "[Rain] occlusion framebuffer incomplete; rain will not be occluded\n";
            releaseRainResources();
            return;
        }
        rainState.occlusionResolution = kOcclusionResolution;
    }

    const glm::vec3 delta = cameraPos - rainState.occlusionCenter;
    const bool moved = glm::length(glm::vec2(delta.x, delta.z)) > kOcclusionMoveThreshold ||
                       std::abs(delta.y) > kOcclusionMoveThreshold * 4.0f;
    const bool sceneChanged = rainState.occlusionSceneData != static_cast<const void*>(sceneObjects.data()) ||
                              rainState.occlusionSceneCount != sceneObjects.size();
    ++rainState.occlusionAge;
    if (rainState.occlusionValid && !moved && !sceneChanged && rainState.occlusionAge < kOcclusionMaxAge) {
        return;
    }

    MODU_PROFILE_SCOPE("Rain Occlusion", ProfilerSampleCategory::RenderDetail);
    // Snap the map to a multiple of its texels so static geometry does not shimmer as the
    // camera drifts.
    const float snap = (2.0f * kOcclusionHalfExtent / static_cast<float>(kOcclusionResolution)) * 4.0f;
    const glm::vec3 center(std::floor(cameraPos.x / snap) * snap, cameraPos.y,
                           std::floor(cameraPos.z / snap) * snap);
    const float top = center.y + kOcclusionAbove;
    const glm::mat4 occlusionView = glm::lookAt(glm::vec3(center.x, top, center.z),
                                                glm::vec3(center.x, top - 1.0f, center.z),
                                                glm::vec3(0.0f, 0.0f, -1.0f));
    const glm::mat4 occlusionProj = glm::ortho(-kOcclusionHalfExtent, kOcclusionHalfExtent,
                                               -kOcclusionHalfExtent, kOcclusionHalfExtent,
                                               0.0f, kOcclusionDepthRange);
    const glm::mat4 occlusionMatrix = occlusionProj * occlusionView;

    RainGlState saved;
    saved.capture();

    glBindFramebuffer(GL_FRAMEBUFFER, rainState.occlusionFbo);
    glViewport(0, 0, kOcclusionResolution, kOcclusionResolution);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    // Roofs are often single planes and imported meshes are not always wound the same way.
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);

    Shader* depthShader = directionalShadowDepthShader;
    depthShader->use();
    depthShader->setMat4("lightSpaceMatrix", occlusionMatrix);
    depthShader->setBool("hasAlbedoTex", false);
    depthShader->setBool("hasOpacityMap", false);
    depthShader->setFloat("materialAlpha", 1.0f);
    depthShader->setFloat("alphaCutoff", 0.0f);
    depthShader->setVec4("uvTransform", glm::vec4(1.0f, 1.0f, 0.0f, 0.0f));
    depthShader->setVec4("uvRect", glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));

    const float reach = kOcclusionHalfExtent * 1.4143f;
    for (const SceneObject& obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !HasRendererComponent(obj)) continue;
        if (obj.renderType == RenderType::Mirror || obj.renderType == RenderType::Sprite) continue;
        if (obj.hasUI && obj.ui.type == UIElementType::Canvas && obj.ui.renderIn3D) continue;
        if (obj.hasSkeletalAnimation && obj.skeletal.enabled) continue;
        glm::mat4 model(1.0f);
        glm::vec3 boundsCenter(0.0f);
        float boundsRadius = 0.0f;
        Mesh* mesh = resolveMesh(resolveContext, obj, model, boundsCenter, boundsRadius);
        if (!mesh) continue;
        if (boundsRadius > 0.0f) {
            const glm::vec2 offset(boundsCenter.x - center.x, boundsCenter.z - center.z);
            if (glm::length(offset) > reach + boundsRadius) continue;
            if (boundsCenter.y - boundsRadius > top) continue;
        }
        depthShader->setMat4("model", model);
        mesh->draw();
        recordDrawCall();
    }

    saved.restore();

    rainState.occlusionMatrix = occlusionMatrix;
    rainState.occlusionCenter = cameraPos;
    rainState.occlusionSceneData = sceneObjects.data();
    rainState.occlusionSceneCount = sceneObjects.size();
    rainState.occlusionAge = 0;
    rainState.occlusionValid = true;
}

void Renderer::renderWeatherRain(const Camera& camera, const std::vector<SceneObject>& sceneObjects,
                                 const glm::mat4& view, const glm::mat4& proj, int width, int height,
                                 RainMeshResolver resolveMesh, void* resolveContext) {
    static_assert(kRainMaxLights == 8, "rain light uniform name tables hold 8 entries");
    if (!skybox || !isRain3DActive() || width <= 0 || height <= 0) return;
    const SkyboxSettings& sky = skybox->getSettings();
    const WeatherSettings& weather = sky.weather;
    const float density = std::isfinite(weather.rainDensity)
                              ? std::clamp(weather.rainDensity, 0.0f, kRainDensityMax) : 1.0f;
    const float amount = std::clamp(weatherDrive.rainAmount, 0.0f, 1.0f) * density;
    if (amount <= 0.001f) return;

    static const std::string rainVert = "Resources/Shaders/rain_vert.glsl";
    static const std::string splashVert = "Resources/Shaders/rain_splash_vert.glsl";
    static const std::string rainFrag = "Resources/Shaders/rain_frag.glsl";
    Shader* streakShader = getShader(rainVert, rainFrag);
    Shader* splashShader = getShader(splashVert, rainFrag);
    if (!streakShader || streakShader == defaultShader || streakShader->ID == 0) return;
    if (splashShader == defaultShader || (splashShader && splashShader->ID == 0)) splashShader = nullptr;

    MODU_PROFILE_SCOPE("Rain 3D", ProfilerSampleCategory::RenderDetail);

    // Clock and camera velocity advance once per engine frame, however many views draw.
    const double now = RainWallSeconds();
    if (rainState.clockFrame != frameSerial || frameSerial == 0) {
        const double dt = rainState.lastWallTime >= 0.0
                              ? std::clamp(now - rainState.lastWallTime, 0.0, 0.1) : 0.0;
        if (frameSerial != 0 || dt > 0.001) {
            rainState.clock = std::fmod(rainState.clock + dt, kRainClockWrap);
            if (rainState.haveLastCamera && dt > 0.0005) {
                const glm::vec3 instant = (camera.position - rainState.lastCameraPos) / static_cast<float>(dt);
                // A jump (teleport, another view's camera) is not motion.
                if (glm::length(camera.position - rainState.lastCameraPos) < 5.0f &&
                    std::isfinite(instant.x) && std::isfinite(instant.y) && std::isfinite(instant.z)) {
                    rainState.cameraVelocity = glm::mix(rainState.cameraVelocity, instant, 0.25f);
                } else {
                    rainState.cameraVelocity = glm::vec3(0.0f);
                }
                const float speed = glm::length(rainState.cameraVelocity);
                if (speed > 60.0f) rainState.cameraVelocity *= 60.0f / speed;
            }
            rainState.lastCameraPos = camera.position;
            rainState.haveLastCamera = true;
            rainState.lastWallTime = now;
            rainState.clockFrame = frameSerial;
        }
    }

    updateRainOcclusion(camera.position, sceneObjects, resolveMesh, resolveContext);
    const bool occlusionOn = rainState.occlusionValid && rainState.occlusionDepth != 0;

    if (rainState.vao == 0) {
        // Core profiles need a VAO bound to draw even with no attributes.
        glGenVertexArrays(1, &rainState.vao);
    }
    if (rainState.vao == 0) return;

    const int nearCount = static_cast<int>(std::round(static_cast<float>(kRainNearBase) * amount));
    const int farCount = static_cast<int>(std::round(static_cast<float>(kRainFarBase) * amount));
    const int splashCount = splashShader ? static_cast<int>(std::round(static_cast<float>(kRainSplashBase) * amount)) : 0;

    glm::vec3 wind(sceneWind.direction.x, 0.0f, sceneWind.direction.z);
    wind *= std::isfinite(sceneWind.strength) ? std::min(sceneWind.strength, 12.0f) * kRainWindResponse : 0.0f;
    const glm::vec3 velocity = glm::vec3(0.0f, -kRainFallSpeed, 0.0f) + wind;
    const float pixelWorld = 2.0f / (std::max(0.001f, std::abs(proj[1][1])) * static_cast<float>(height));
    // rainOpacity keeps its meaning (0.55 default = 1x) for 3D rain.
    const float opacityScale = std::clamp(weather.rainOpacity, 0.0f, 2.0f) / 0.55f;
    const float rainTime = static_cast<float>(rainState.clock);
    const glm::vec3 skyTint = sky.fogEnabled ? glm::min(sky.fogColor, ambientColor * 3.0f + glm::vec3(0.02f))
                                             : ambientColor;
    const float flash = weatherDrive.active ? weatherDrive.flash : 0.0f;

    RainGlState saved;
    saved.capture();

    glEnable(GL_BLEND);
    // Premultiplied colour; destination alpha is left as it was.
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(rainState.vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, occlusionOn ? rainState.occlusionDepth : 0);

    const auto setShared = [&](Shader* program) {
        program->use();
        program->setMat4("view", view);
        program->setMat4("projection", proj);
        program->setVec3("cameraPos", camera.position);
        program->setFloat("rainTime", rainTime);
        program->setFloat("pixelWorld", pixelWorld);
        program->setInt("occlusionDepth", 0);
        program->setMat4("occlusionMatrix", rainState.occlusionMatrix);
        program->setVec3("ambientColor", ambientColor);
        program->setVec3("skyTint", skyTint);
        program->setVec3("sunDirection", rainState.sunDirection);
        program->setVec3("sunColor", rainState.sunColor);
        program->setFloat("lightningFlash", flash);
        const int lightCount = std::clamp(rainState.lightCount, 0, kRainMaxLights);
        program->setInt("rainLightCount", lightCount);
        for (int i = 0; i < lightCount; ++i) {
            program->setVec4(kRainLightPosNames[i], rainState.lightPosRange[i]);
            program->setVec4(kRainLightColorNames[i], rainState.lightColorType[i]);
            program->setVec4(kRainLightDirNames[i], rainState.lightDirCone[i]);
            program->setFloat(kRainLightOuterNames[i], rainState.lightOuterCos[i]);
        }
        program->setBool("fogEnabled", sky.fogEnabled);
        program->setInt("fogMode", sky.fogMode);
        program->setVec3("fogColor", sky.fogColor);
        program->setFloat("fogStart", sky.fogStart);
        program->setFloat("fogEnd", sky.fogEnd);
        program->setFloat("fogDensity", sky.fogDensity);
        program->setFloat("fogHeight", sky.fogHeight);
        program->setFloat("fogHeightFalloff", sky.fogHeightFalloff);
    };

    if (nearCount + farCount > 0) {
        setShared(streakShader);
        streakShader->setVec3("rainVelocity", velocity);
        streakShader->setVec3("cameraVelocity", rainState.cameraVelocity);
        streakShader->setInt("nearCount", nearCount);
        streakShader->setVec4("layerNear", kRainNearLayer);
        streakShader->setVec4("layerFar", kRainFarLayer);
        streakShader->setFloat("rainAlpha", 0.75f * opacityScale);
        streakShader->setBool("occlusionEnabled", occlusionOn);
        // ~25 cm of slack so a drop resting on a surface's own depth texel is not culled.
        streakShader->setFloat("occlusionBias", 0.25f / kOcclusionDepthRange);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, nearCount + farCount);
        recordDrawCall();
    }

    if (splashShader && occlusionOn && splashCount > 0) {
        setShared(splashShader);
        splashShader->setFloat("splashRadius", kSplashRadius);
        splashShader->setFloat("splashPeriod", kSplashPeriod);
        splashShader->setFloat("rainAlpha", 0.45f * opacityScale);
        splashShader->setMat4("occlusionInverse", glm::inverse(rainState.occlusionMatrix));
        glDrawArraysInstanced(GL_TRIANGLES, 0, 12, splashCount);
        recordDrawCall();
    }

    saved.restore();
}
