#include "../../include/Skybox/Skybox.h"
#include "../../include/Shaders/Shader.h"
#include "../../include/Textures/Texture.h"
#include "../../include/Graphics/OpenGL.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>
#include <string>
#include "../../src/ThirdParty/glm/glm.hpp"
#include "../../src/ThirdParty/glm/gtc/type_ptr.hpp"
#include "../../src/ThirdParty/glm/gtc/matrix_transform.hpp"

// Skybox cube vertices (positions only, no normals/UVs needed)
float skyboxVertices[] = {
    // positions
    -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,
     1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,

    -1.0f, -1.0f,  1.0f,
    -1.0f, -1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f,  1.0f,
    -1.0f, -1.0f,  1.0f,

     1.0f, -1.0f, -1.0f,
     1.0f, -1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,
     1.0f,  1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,

    -1.0f, -1.0f,  1.0f,
    -1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,
     1.0f, -1.0f,  1.0f,
    -1.0f, -1.0f,  1.0f,

    -1.0f,  1.0f, -1.0f,
     1.0f,  1.0f, -1.0f,
     1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,
    -1.0f,  1.0f,  1.0f,
    -1.0f,  1.0f, -1.0f,

    -1.0f, -1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,
     1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,
     1.0f, -1.0f,  1.0f
};

Skybox::Skybox() {
    skyboxShader = new Shader(vertPath.c_str(), fragPath.c_str());
    setupMesh();
    reloadTextures();
}

Skybox::~Skybox() {
    if (skyboxShader) delete skyboxShader;
    if (sunTexture) delete sunTexture;
    if (moonTexture) delete moonTexture;
    if (scrollingTexture) delete scrollingTexture;
    releaseBakedSky();
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

namespace {
constexpr int kBakedSkyFaceResolutionDefault = 256;
constexpr float kBakedSkyRefreshIntervalDefault = 0.1f;
constexpr float kBakedSkySunSettleSec = 0.12f;
constexpr int kBakedSkyTextureUnit = 12;
} // namespace

void Skybox::releaseBakedSky() {
    if (bakedFbo) {
        glDeleteFramebuffers(1, &bakedFbo);
        bakedFbo = 0;
    }
    if (bakedCube) {
        glDeleteTextures(1, &bakedCube);
        bakedCube = 0;
    }
    bakedDirtyMask = 0;
    bakedHasFullCapture = false;
    bakedLastRefreshSec = -1.0f;
}

bool Skybox::cachedSkyUsable() const {
    // Scrolling mode without a usable texture falls back to the procedural sky in the shader
    // (uSkyMode == 1 && uHasScrollTexture), so it is just as cacheable as Procedural mode.
    const bool hasScrollTexture = scrollingTexture != nullptr && scrollingTexture->GetID() != 0;
    const bool drawsProceduralSky =
        settings.mode == SkyboxMode::Procedural ||
        (settings.mode == SkyboxMode::Scrolling && !hasScrollTexture);
    return settings.cachedSky &&
           drawsProceduralSky &&
           fragPath == "Resources/Shaders/skybox_frag.glsl" &&
           skyboxShader && skyboxShader->ID != 0;
}

bool Skybox::ensureBakedSkyTargets() {
    if (bakedAllocFailed) return false;
    const int wantResolution = std::clamp(
        settings.skyResolution > 0 ? settings.skyResolution : kBakedSkyFaceResolutionDefault,
        64, 2048);
    if (bakedCube != 0 && bakedResolution != wantResolution) {
        releaseBakedSky();
    }
    if (bakedCube != 0 && bakedFbo != 0) return true;
    bakedResolution = wantResolution;

    glGenTextures(1, &bakedCube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, bakedCube);
    for (int face = 0; face < 6; ++face) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB10_A2,
                     bakedResolution, bakedResolution, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    glGenFramebuffers(1, &bakedFbo);
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, bakedFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X, bakedCube, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    if (!complete) {
        std::cerr << "[WARN] Baked sky framebuffer incomplete; falling back to live sky\n";
        releaseBakedSky();
        bakedAllocFailed = true;
        return false;
    }
    bakedDirtyMask = 0x3F;
    bakedHasFullCapture = false;
    return true;
}

void Skybox::bakeDirtyFaces(int maxFaces) {
    if (bakedDirtyMask == 0 || maxFaces <= 0) return;

    static const glm::vec3 faceDirs[6] = {
        glm::vec3(1.0f, 0.0f, 0.0f),  glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),  glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),  glm::vec3(0.0f, 0.0f, -1.0f),
    };
    static const glm::vec3 faceUps[6] = {
        glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),  glm::vec3(0.0f, 0.0f, -1.0f),
        glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
    };

    GLint prevFbo = 0;
    GLint prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    const GLboolean blendWas = glIsEnabled(GL_BLEND);
    const GLboolean scissorWas = glIsEnabled(GL_SCISSOR_TEST);
    if (blendWas) glDisable(GL_BLEND);
    if (scissorWas) glDisable(GL_SCISSOR_TEST);

    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, bakedFbo);
    glViewport(0, 0, bakedResolution, bakedResolution);
    int baked = 0;
    for (int face = 0; face < 6 && baked < maxFaces; ++face) {
        if ((bakedDirtyMask & (1 << face)) == 0) continue;
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, bakedCube, 0);
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), faceDirs[face], faceUps[face]);
        applySkyUniforms(view, proj,
                         static_cast<float>(bakedResolution),
                         static_cast<float>(bakedResolution),
                         glm::vec2(0.0f), false);
        drawSkyTriangle();
        bakedDirtyMask &= ~(1 << face);
        ++baked;
    }
    if (bakedDirtyMask == 0) {
        bakedHasFullCapture = true;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (blendWas) glEnable(GL_BLEND);
    if (scissorWas) glEnable(GL_SCISSOR_TEST);
}

void Skybox::setupMesh() {
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);

    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), &skyboxVertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    glBindVertexArray(0);
}

void Skybox::setTimeOfDay(float time) {
    timeOfDay = time;
}

bool Skybox::reloadShader() {
    if (skyboxShader) {
        delete skyboxShader;
        skyboxShader = nullptr;
    }
    skyboxShader = new Shader(vertPath.c_str(), fragPath.c_str());
    return skyboxShader && skyboxShader->ID != 0;
}

void Skybox::reloadTextures(bool sun, bool moon, bool scrolling) {
    if (sun && sunTexture) {
        delete sunTexture;
        sunTexture = nullptr;
    }
    if (moon && moonTexture) {
        delete moonTexture;
        moonTexture = nullptr;
    }
    if (scrolling && scrollingTexture) {
        delete scrollingTexture;
        scrollingTexture = nullptr;
    }

    if (sun && !settings.sunTexturePath.empty()) {
        sunTexture = new Texture(settings.sunTexturePath,
                                 GL_CLAMP_TO_EDGE,
                                 GL_CLAMP_TO_EDGE,
                                 GL_LINEAR_MIPMAP_LINEAR,
                                 GL_LINEAR);
        if (sunTexture && sunTexture->GetID() == 0) {
            delete sunTexture;
            sunTexture = nullptr;
        }
    }

    if (moon && !settings.moonTexturePath.empty()) {
        moonTexture = new Texture(settings.moonTexturePath,
                                  GL_CLAMP_TO_EDGE,
                                  GL_CLAMP_TO_EDGE,
                                  GL_LINEAR_MIPMAP_LINEAR,
                                  GL_LINEAR);
        if (moonTexture && moonTexture->GetID() == 0) {
            delete moonTexture;
            moonTexture = nullptr;
        }
    }

    if (scrolling && !settings.scrollingTexturePath.empty()) {
        scrollingTexture = new Texture(settings.scrollingTexturePath,
                                       GL_REPEAT,
                                       GL_REPEAT,
                                       GL_NEAREST,
                                       GL_NEAREST);
        if (scrollingTexture && scrollingTexture->GetID() == 0) {
            delete scrollingTexture;
            scrollingTexture = nullptr;
        } else if (scrollingTexture) {
            glBindTexture(GL_TEXTURE_2D, scrollingTexture->GetID());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }
}

void Skybox::setShaderPaths(const std::string& vert, const std::string& frag) {
    if (!vert.empty()) vertPath = vert;
    if (!frag.empty()) fragPath = frag;
    if (!reloadShader()) {
        std::cerr << "Failed to reload skybox shader, reverting to defaults\n";
        vertPath = "Resources/Shaders/skybox_vert.glsl";
        fragPath = "Resources/Shaders/skybox_frag.glsl";
        reloadShader();
    }
}

void Skybox::setSettings(const SkyboxSettings& newSettings) {
    // Every Environment window slider lands here, once per frame while it is dragged. This
    // used to delete and re-decode all three textures from disk on every call.
    const bool sunChanged = newSettings.sunTexturePath != settings.sunTexturePath;
    const bool moonChanged = newSettings.moonTexturePath != settings.moonTexturePath;
    const bool scrollingChanged = newSettings.scrollingTexturePath != settings.scrollingTexturePath;
    settings = newSettings;
    settings.scrollingRepeatX = std::max(0.01f, settings.scrollingRepeatX);
    settings.scrollingRepeatY = std::max(0.01f, settings.scrollingRepeatY);
    settings.scrollingLookSensitivity = std::max(0.0f, settings.scrollingLookSensitivity);
    settings.scrollingVerticalInfluence = std::clamp(settings.scrollingVerticalInfluence, 0.0f, 1.0f);
    settings.environmentReflectionIntensity = std::clamp(settings.environmentReflectionIntensity, 0.0f, 2.0f);
    settings.reflectionDistanceFadeStart = std::max(0.0f, settings.reflectionDistanceFadeStart);
    settings.reflectionDistanceFadeEnd = std::max(settings.reflectionDistanceFadeStart + 0.01f, settings.reflectionDistanceFadeEnd);
    settings.fogMode = std::clamp(settings.fogMode, 0, 2);
    settings.fogStart = std::max(0.0f, settings.fogStart);
    settings.fogEnd = std::max(settings.fogStart + 0.01f, settings.fogEnd);
    settings.fogDensity = std::clamp(settings.fogDensity, 0.0f, 1.0f);
    settings.fogHeightFalloff = std::clamp(settings.fogHeightFalloff, 0.0f, 1.0f);
    if (sunChanged || moonChanged || scrollingChanged) {
        reloadTextures(sunChanged, moonChanged, scrollingChanged);
    }
    if (!settings.cachedSky) {
        releaseBakedSky();
    } else if (bakedCube != 0) {
        bakedDirtyMask = 0x3F;
        bakedLastRefreshSec = animationTime;
    }
    bakedAllocFailed = false;
}

void Skybox::setDirectionalLight(const glm::vec3& direction, bool valid) {
    const float lengthSq = glm::dot(direction, direction);
    const bool usable = valid && lengthSq > 0.000001f;
    const glm::vec3 next = usable ? glm::normalize(direction) : glm::vec3(0.0f, 1.0f, 0.0f);

    if (usable != sunDirectionValid || glm::dot(next - sunDirection, next - sunDirection) > 0.000001f) {
        if (settings.sunFollowsDirectionalLight) {
            sunMotionSec = animationTime;
            bakedSunStale = true;
        }
    }
    sunDirection = next;
    sunDirectionValid = usable;
}

glm::vec3 Skybox::resolvedSunDirection() const {
    if (settings.sunFollowsDirectionalLight && sunDirectionValid) return sunDirection;
    const float t = timeOfDay - std::floor(timeOfDay);
    const float angle = t * 2.0f * 3.14159265359f;
    return glm::normalize(glm::vec3(std::cos(angle), std::sin(angle) * 0.55f, std::sin(angle)));
}

void Skybox::applySkyUniforms(const glm::mat4& viewMat, const glm::mat4& projMat,
                              float viewportW, float viewportH,
                              const glm::vec2& cameraAngles, bool sampleBaked) {
    skyboxShader->use();
    skyboxShader->setMat4("view", viewMat);
    skyboxShader->setMat4("projection", projMat);
    skyboxShader->setMat4("uInvProjection", glm::inverse(projMat));
    skyboxShader->setMat3("uInvViewRot", glm::inverse(glm::mat3(viewMat)));
    skyboxShader->setFloat("timeOfDay", timeOfDay);
    skyboxShader->setFloat("uSkyTime", animationTime);
    skyboxShader->setInt("uSunTex", 0);
    skyboxShader->setInt("uMoonTex", 1);
    skyboxShader->setInt("uScrollTex", 2);
    skyboxShader->setInt("uBakedSkyCube", kBakedSkyTextureUnit);
    skyboxShader->setBool("uUseBakedSky", sampleBaked);
    skyboxShader->setInt("uSkyMode", static_cast<int>(settings.mode));
    skyboxShader->setInt("uCloudMode", settings.cloudMode);
    skyboxShader->setFloat("uCloudCoverage", weatherDrive.active ? weatherDrive.cloudCoverage
                                                                 : settings.cloudCoverage);
    skyboxShader->setFloat("uCloudDensity", settings.cloudDensity);
    skyboxShader->setInt("uCloudSteps", settings.cloudSteps);
    skyboxShader->setInt("uCloudLightSteps", settings.cloudLightSteps);
    skyboxShader->setFloat("uCloudBottom", settings.cloudBottom);
    skyboxShader->setFloat("uCloudTop", settings.cloudTop);
    skyboxShader->setFloat("uCloudWind", settings.cloudWind);
    skyboxShader->setFloat("uCloudHorizonFade", settings.cloudHorizonFade);
    skyboxShader->setVec3("uCameraPos", bakedCameraValid ? bakedCameraPos : currentCameraPos);
    skyboxShader->setFloat("uCloudDarkness", weatherDrive.active ? weatherDrive.cloudDarkness
                                                                 : settings.cloudDarkness);

    const bool useLightSun = settings.sunFollowsDirectionalLight && sunDirectionValid;
    skyboxShader->setBool("uUseLightSunDirection", useLightSun);
    skyboxShader->setVec3("uLightSunDirection", sunDirection);
    skyboxShader->setVec2("uScrollRepeat", glm::vec2(settings.scrollingRepeatX, settings.scrollingRepeatY));
    skyboxShader->setFloat("uScrollLookSensitivity", settings.scrollingLookSensitivity);
    skyboxShader->setFloat("uScrollVerticalInfluence", settings.scrollingVerticalInfluence);
    skyboxShader->setBool("uHasScrollTexture", scrollingTexture != nullptr && scrollingTexture->GetID() != 0);
    skyboxShader->setVec2("uViewportSize", glm::vec2(std::max(1.0f, viewportW),
                                                     std::max(1.0f, viewportH)));
    skyboxShader->setVec2("uCameraAngles", cameraAngles);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (sunTexture && sunTexture->GetID() != 0) ? sunTexture->GetID() : 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (moonTexture && moonTexture->GetID() != 0) ? moonTexture->GetID() : 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, (scrollingTexture && scrollingTexture->GetID() != 0) ? scrollingTexture->GetID() : 0);
    glActiveTexture(GL_TEXTURE0);
}

void Skybox::drawSkyTriangle() {
    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void Skybox::draw(const float* view, const float* projection) {
    static const auto startTime = std::chrono::steady_clock::now();
    animationTime = std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime).count();

    glm::mat4 viewMat = glm::make_mat4(view);
    glm::mat4 projMat = glm::make_mat4(projection);

    viewMat = glm::mat4(glm::mat3(viewMat));
    glm::vec3 cameraForward = glm::normalize(glm::transpose(glm::mat3(viewMat)) * glm::vec3(0.0f, 0.0f, -1.0f));
    glm::vec2 cameraAngles(
        std::atan2(cameraForward.x, -cameraForward.z) / (2.0f * 3.14159265359f),
        std::asin(std::clamp(cameraForward.y, -1.0f, 1.0f))
    );
    GLint viewport[4] = {0, 0, 1, 1};
    glGetIntegerv(GL_VIEWPORT, viewport);

    bool sampleBaked = false;
    if (cachedSkyUsable() && ensureBakedSkyTargets()) {
        const bool sunStillMoving =
            bakedSunStale && sunMotionSec >= 0.0f &&
            (animationTime - sunMotionSec) < kBakedSkySunSettleSec;

        if (sunStillMoving) {
            sampleBaked = false;
        } else {
            if (bakedSunStale) {
                bakedDirtyMask = 0x3F;
                bakeDirtyFaces(6);
                bakedSunStale = false;
                bakedLastRefreshSec = animationTime;
            }
            const float refreshInterval = std::max(0.0f, settings.skyRefreshInterval > 0.0f
                                                        ? settings.skyRefreshInterval
                                                        : kBakedSkyRefreshIntervalDefault);
            const bool refreshDue = bakedLastRefreshSec < 0.0f ||
                                    animationTime - bakedLastRefreshSec >= refreshInterval;

            bool cameraMoved = false;
            if (settings.cloudMode == 1 && refreshDue) {
                const float moveThreshold = std::max(5.0f, settings.cloudBottom * 0.03f);
                cameraMoved = !bakedCameraValid ||
                              glm::distance(currentCameraPos, bakedCameraPos) > moveThreshold;
            }

            // A periodic refresh starts only once the previous one has rolled through every
            // face, and bakes one face per draw while a full capture exists: re-baking all six
            // at once made a 6x sky-cost frame every refresh interval, a visible hitch on
            // low-end GPUs. The first capture (and a sun change above) still bakes all faces.
            if ((refreshDue || cameraMoved) && bakedDirtyMask == 0) {
                bakedDirtyMask = 0x3F;
                bakedLastRefreshSec = animationTime;
                bakedCameraPos = currentCameraPos;
                bakedCameraValid = true;
            }

            bakeDirtyFaces(bakedHasFullCapture ? 1 : 6);
            sampleBaked = bakedHasFullCapture;
        }
    }

    glDepthFunc(GL_LEQUAL);
    applySkyUniforms(viewMat, projMat,
                     static_cast<float>(std::max(1, viewport[2])),
                     static_cast<float>(std::max(1, viewport[3])),
                     cameraAngles, sampleBaked);
    if (sampleBaked) {
        glActiveTexture(GL_TEXTURE0 + kBakedSkyTextureUnit);
        glBindTexture(GL_TEXTURE_CUBE_MAP, bakedCube);
        glActiveTexture(GL_TEXTURE0);
    }
    drawSkyTriangle();
    if (sampleBaked) {
        glActiveTexture(GL_TEXTURE0 + kBakedSkyTextureUnit);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDepthFunc(GL_LESS);
}

namespace {
struct WeatherProfile {
    float coverage;
    float darkness;
    float rain;
    bool lightning;
};

WeatherProfile ProfileFor(WeatherMode mode) {
    switch (mode) {
        case WeatherMode::Overcast:     return {0.75f, 0.35f, 0.0f, false};
        case WeatherMode::DarkCloudy:   return {0.92f, 0.70f, 0.0f, false};
        case WeatherMode::Rain:         return {0.90f, 0.55f, 0.75f, false};
        case WeatherMode::Thunderstorm: return {0.98f, 0.85f, 1.0f, true};
        case WeatherMode::Clear:
        default:                        return {0.25f, 0.0f, 0.0f, false};
    }
}

float WeatherRandom(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>((state >> 8) & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
}
}  // namespace

WeatherDrive EvaluateWeather(const WeatherSettings& weather, WeatherRuntime& runtime,
                             float deltaSeconds) {
    WeatherDrive out;
    if (!weather.enabled) {
        runtime.timeToNextStrike = -1.0f;
        runtime.flash = 0.0f;
        return out;
    }

    out.active = true;
    const WeatherProfile clear = ProfileFor(WeatherMode::Clear);
    const WeatherProfile target = ProfileFor(weather.mode);
    const float t = std::clamp(weather.intensity, 0.0f, 1.0f);

    out.cloudCoverage = clear.coverage + (target.coverage - clear.coverage) * t;
    out.cloudDarkness = clear.darkness + (target.darkness - clear.darkness) * t;
    out.rainAmount = clear.rain + (target.rain - clear.rain) * t;

    const float dt = std::clamp(deltaSeconds, 0.0f, 0.25f);
    if (target.lightning && t > 0.01f) {
        if (runtime.timeToNextStrike < 0.0f) {
            runtime.timeToNextStrike =
                std::max(0.5f, weather.lightningInterval) * (0.4f + WeatherRandom(runtime.rngState));
        }
        runtime.timeToNextStrike -= dt;
        if (runtime.timeToNextStrike <= 0.0f) {
            runtime.flash = std::max(0.0f, weather.lightningIntensity) * t;
            out.thunderTriggered = true;
            runtime.timeToNextStrike =
                std::max(0.5f, weather.lightningInterval) * (0.5f + WeatherRandom(runtime.rngState));
        }
    } else {
        runtime.timeToNextStrike = -1.0f;
    }

    if (runtime.flash > 0.0f) {
        runtime.flash *= std::exp(-dt * 6.0f);
        if (runtime.flash < 0.002f) runtime.flash = 0.0f;
    }
    out.flash = runtime.flash;

    const bool wantHeavy = t >= std::clamp(weather.heavyRainThreshold, 0.0f, 1.0f);
    switch (weather.mode) {
        case WeatherMode::Overcast:
        case WeatherMode::DarkCloudy:
            out.ambiencePath = weather.overcastSoundPath;
            break;
        case WeatherMode::Rain:
            out.ambiencePath = (wantHeavy && !weather.heavyRainSoundPath.empty())
                                   ? weather.heavyRainSoundPath
                                   : weather.rainSoundPath;
            break;
        case WeatherMode::Thunderstorm:
            out.ambiencePath = !weather.heavyRainSoundPath.empty()
                                   ? weather.heavyRainSoundPath
                                   : weather.rainSoundPath;
            break;
        case WeatherMode::Clear:
        default:
            out.ambiencePath = weather.clearSoundPath;
            break;
    }
    if (out.ambiencePath.empty() && weather.mode != WeatherMode::Clear) {
        if (!weather.rainSoundPath.empty() &&
            (weather.mode == WeatherMode::Rain || weather.mode == WeatherMode::Thunderstorm)) {
            out.ambiencePath = weather.rainSoundPath;
        } else if (!weather.overcastSoundPath.empty()) {
            out.ambiencePath = weather.overcastSoundPath;
        }
    }
    return out;
}
