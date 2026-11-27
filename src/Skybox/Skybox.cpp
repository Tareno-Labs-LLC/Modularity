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
    refreshMatchedFogColor();
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
    bakedHasMips = false;
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

    // Mip chain allocated up front so glGenerateMipmap after a bake never
    // reallocates. Half-float first: alpha carries the cloud occlusion of the
    // sun, which RGB10_A2's two alpha bits can only do in quarter steps.
    int mipLevels = 1;
    for (int size = bakedResolution; size > 1; size >>= 1) ++mipLevels;
    const GLenum formats[2] = {GL_RGBA16F, GL_RGB10_A2};
    const GLenum types[2] = {GL_HALF_FLOAT, GL_UNSIGNED_INT_2_10_10_10_REV};
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    bool complete = false;
    for (int attempt = 0; attempt < 2 && !complete; ++attempt) {
        if (bakedCube) glDeleteTextures(1, &bakedCube);
        if (bakedFbo) glDeleteFramebuffers(1, &bakedFbo);
        bakedCube = 0;
        bakedFbo = 0;
        glGenTextures(1, &bakedCube);
        glBindTexture(GL_TEXTURE_CUBE_MAP, bakedCube);
        for (int level = 0; level < mipLevels; ++level) {
            const int size = std::max(1, bakedResolution >> level);
            for (int face = 0; face < 6; ++face) {
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, level, formats[attempt],
                             size, size, 0, GL_RGBA, types[attempt], nullptr);
            }
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, mipLevels - 1);
        // Plain linear until the first full bake has produced a mip chain.
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

        glGenFramebuffers(1, &bakedFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, bakedFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X, bakedCube, 0);
        complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        bakedFormat = formats[attempt];
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    bakedHasMips = false;
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
                         glm::vec2(0.0f), false, false);
        drawSkyTriangle();
        bakedDirtyMask &= ~(1 << face);
        ++baked;
    }
    if (bakedDirtyMask == 0) {
        bakedHasFullCapture = true;
        // Re-filter the chain once per full refresh, not per face: anything
        // sampling the cube rough (environment lighting) then sees one
        // consistent moment. Storage already exists, so this is filtering only.
        if (baked > 0) {
            glBindTexture(GL_TEXTURE_CUBE_MAP, bakedCube);
            glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
            if (!bakedHasMips) {
                glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                bakedHasMips = true;
            }
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        }
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
    refreshMatchedFogColor();
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
    settings.atmosphereModel = std::clamp(settings.atmosphereModel, 0, 1);
    settings.turbidity = std::clamp(settings.turbidity, 1.0f, 10.0f);
    settings.sunSize = std::clamp(settings.sunSize, 0.05f, 20.0f);
    settings.sunIntensity = std::clamp(settings.sunIntensity, 0.0f, 10.0f);
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
    refreshMatchedFogColor();
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
    refreshMatchedFogColor();
}

glm::vec3 Skybox::resolvedSunDirection() const {
    if (settings.sunFollowsDirectionalLight && sunDirectionValid) return sunDirection;
    const float t = timeOfDay - std::floor(timeOfDay);
    if (settings.atmosphereModel == 1) {
        // The documented schedule: 0.25 sunrise in the east (+X), 0.5 noon at
        // ~66 degrees, 0.75 sunset in the west, 0 / 1 midnight.
        const float angle = (t - 0.25f) * 2.0f * 3.14159265359f;
        return glm::normalize(glm::vec3(std::cos(angle), std::sin(angle), std::sin(angle) * 0.45f));
    }
    const float angle = t * 2.0f * 3.14159265359f;
    return glm::normalize(glm::vec3(std::cos(angle), std::sin(angle) * 0.55f, std::sin(angle)));
}

void Skybox::applySkyUniforms(const glm::mat4& viewMat, const glm::mat4& projMat,
                              float viewportW, float viewportH,
                              const glm::vec2& cameraAngles, bool sampleBaked,
                              bool drawSunDisk) {
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

    const bool physicalSky = settings.atmosphereModel == 1;
    skyboxShader->setInt("uAtmosphereModel", physicalSky ? 1 : 0);
    skyboxShader->setFloat("uTurbidity", settings.turbidity);
    skyboxShader->setFloat("uSunSize", settings.sunSize);
    skyboxShader->setFloat("uSunIntensity", settings.sunIntensity);
    skyboxShader->setBool("uDrawSunDisk", drawSunDisk);
    // The physical model takes the sun fully resolved from here, which keeps
    // its timeOfDay arc in one place (resolvedSunDirection).
    const bool useLightSun = physicalSky || (settings.sunFollowsDirectionalLight && sunDirectionValid);
    skyboxShader->setBool("uUseLightSunDirection", useLightSun);
    skyboxShader->setVec3("uLightSunDirection", physicalSky ? resolvedSunDirection() : sunDirection);
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

// ---------------------------------------------------------------------------
// CPU port of the physical atmosphere in skybox_frag.glsl (atmosphereRadiance,
// atmNightRadiance, the overcast veil and atmToDisplay), used only to find the
// sky's horizon colour for fog matching. Keep the constants and the step
// scheme in step with the shader, or the fog will stop meeting the sky.
// ---------------------------------------------------------------------------
namespace {
constexpr float kAtmR = 6360.0f;
constexpr float kAtmH = 100.0f;
constexpr float kAtmHR = 8.0f;
constexpr float kAtmHM = 1.2f;
constexpr float kAtmViewAlt = 0.5f;
constexpr float kAtmSunE = 20.0f;
constexpr float kAtmExposure = 1.3f;
constexpr float kAtmSaturation = 1.3f;
constexpr float kAtmMieG = 0.86f;
constexpr float kAtmPi = 3.14159265359f;
const glm::vec3 kAtmBetaR(5.802e-3f, 13.558e-3f, 33.1e-3f);
const glm::vec3 kAtmBetaOz(1.83e-3f, 5.30e-3f, 0.24e-3f);
constexpr float kAtmBetaM = 3.996e-3f;

float AtmSmoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float AtmChapman(float r, float mu, float H) {
    const float H50 = H * 0.693147f;
    const float c = std::sqrt(r / H50);
    const float h50 = (r - kAtmR) / H50;
    if (mu >= 0.0f) return H * c / (c * mu + 1.0f) * std::exp2(-h50);
    const float rMin = std::max(r * std::sqrt(std::max(1.0f - mu * mu, 0.0f)), kAtmR);
    const float x0 = rMin / H50;
    const float c0 = std::sqrt(x0);
    return H * (2.0f * c0 * std::exp2((kAtmR - rMin) / H50) - c / (1.0f - c * mu) * std::exp2(-h50));
}

float AtmSunVisibility(float r, float mu) {
    if (mu >= 0.0f) return 1.0f;
    const float rMin = r * std::sqrt(std::max(1.0f - mu * mu, 0.0f));
    return AtmSmoothstep(kAtmR - 8.0f, kAtmR + 1.0f, rMin);
}

glm::vec3 AtmExp(const glm::vec3& v) {
    return glm::vec3(std::exp(v.x), std::exp(v.y), std::exp(v.z));
}

glm::vec3 AtmTransmittanceToSun(float r, float mu, float mieScale) {
    const float mieE = kAtmBetaM * mieScale / 0.9f;
    const glm::vec3 tau = (kAtmBetaR + kAtmBetaOz) * AtmChapman(r, mu, kAtmHR) +
                          glm::vec3(mieE) * AtmChapman(r, mu, kAtmHM);
    return AtmExp(-tau) * AtmSunVisibility(r, mu);
}

glm::vec3 AtmRadiance(const glm::vec3& dir, const glm::vec3& sunDir, float mieScale, float sunIntensity) {
    const float r0 = kAtmR + kAtmViewAlt;
    const float mu = dir.y;
    const float b = r0 * mu;
    const float cTop = (kAtmViewAlt - kAtmH) * (2.0f * kAtmR + kAtmViewAlt + kAtmH);
    const float cGround = kAtmViewAlt * (2.0f * kAtmR + kAtmViewAlt);
    float tMax = -b + std::sqrt(std::max(b * b - cTop, 0.0f));
    const float discG = b * b - cGround;
    const bool hitsGround = mu < 0.0f && discG > 0.0f;
    if (hitsGround) tMax = -b - std::sqrt(discG);

    const glm::vec3 betaRE = kAtmBetaR + kAtmBetaOz;
    const float betaMS = kAtmBetaM * mieScale;
    const float betaME = betaMS / 0.9f;
    const float nu = glm::dot(dir, sunDir);
    const float phaseR = 3.0f / (16.0f * kAtmPi) * (1.0f + nu * nu);
    const float g2 = kAtmMieG * kAtmMieG;
    const float phaseM = 3.0f / (8.0f * kAtmPi) * ((1.0f - g2) * (1.0f + nu * nu)) /
                         ((2.0f + g2) * std::pow(std::max(1.0f + g2 - 2.0f * kAtmMieG * nu, 1e-4f), 1.5f));

    glm::vec3 sumR(0.0f);
    glm::vec3 sumM(0.0f);
    float odR = 0.0f;
    float odM = 0.0f;
    for (int i = 0; i < 12; ++i) {
        const float f0 = static_cast<float>(i) / 12.0f;
        const float f1 = static_cast<float>(i + 1) / 12.0f;
        const float t0 = tMax * f0 * f0;
        const float t1 = tMax * f1 * f1;
        const float ds = t1 - t0;
        const float t = 0.5f * (t0 + t1);
        const float r = std::sqrt(std::max(r0 * r0 + t * t + 2.0f * r0 * t * mu, kAtmR * kAtmR));
        const float alt = r - kAtmR;
        const float rhoR = std::exp(-alt / kAtmHR);
        const float rhoM = std::exp(-alt / kAtmHM);
        odR += rhoR * ds * 0.5f;
        odM += rhoM * ds * 0.5f;
        const float muS = std::clamp((r0 * sunDir.y + t * nu) / r, -1.0f, 1.0f);
        const glm::vec3 tau = betaRE * (odR + AtmChapman(r, muS, kAtmHR)) +
                              glm::vec3(betaME) * (odM + AtmChapman(r, muS, kAtmHM));
        const glm::vec3 T = AtmExp(-tau) * AtmSunVisibility(r, muS);
        sumR += T * rhoR * ds;
        sumM += T * rhoM * ds;
        odR += rhoR * ds * 0.5f;
        odM += rhoM * ds * 0.5f;
    }
    const glm::vec3 viewT = AtmExp(-(betaRE * odR + glm::vec3(betaME) * odM));
    const float E = kAtmSunE * std::max(sunIntensity, 0.0f);
    glm::vec3 L = E * (kAtmBetaR * sumR * (phaseR + 0.012f) + betaMS * sumM * (phaseM + 0.01f));
    if (hitsGround) {
        const float muG = std::clamp((r0 * sunDir.y + tMax * nu) / kAtmR, -1.0f, 1.0f);
        const glm::vec3 sunAtGround = AtmTransmittanceToSun(kAtmR, muG, mieScale);
        const glm::vec3 skyAmbient = glm::vec3(0.34f, 0.45f, 0.62f) * 0.05f * AtmSmoothstep(-0.15f, 0.25f, sunDir.y);
        const glm::vec3 groundAlbedo(0.056f, 0.052f, 0.049f);
        L += viewT * E * groundAlbedo * (sunAtGround * std::max(muG, 0.0f) / kAtmPi + skyAmbient);
    }
    return L;
}

glm::vec3 AtmNightRadiance(const glm::vec3& dir) {
    const glm::vec3 zenith(std::pow(0.020f, 2.2f), std::pow(0.032f, 2.2f), std::pow(0.080f, 2.2f));
    const glm::vec3 horizon(std::pow(0.055f, 2.2f), std::pow(0.068f, 2.2f), std::pow(0.115f, 2.2f));
    glm::vec3 col = glm::mix(horizon, zenith, AtmSmoothstep(-0.02f, 0.55f, dir.y));
    if (dir.y < 0.0f) col = horizon * glm::mix(1.0f, 0.4f, AtmSmoothstep(0.0f, -0.2f, dir.y));
    return col / kAtmExposure;
}

glm::vec3 AtmBlueHourRadiance(const glm::vec3& dir, const glm::vec3& sunDir) {
    const float amount = AtmSmoothstep(-0.22f, -0.03f, sunDir.y) * (1.0f - AtmSmoothstep(0.0f, 0.18f, sunDir.y));
    const glm::vec3 col = glm::vec3(std::pow(0.05f, 2.2f), std::pow(0.11f, 2.2f), std::pow(0.30f, 2.2f)) * 0.8f;
    const float shape = (0.55f + 0.45f * AtmSmoothstep(-0.05f, 0.6f, dir.y)) * (dir.y < 0.0f ? 0.4f : 1.0f);
    return col * shape * amount;
}

glm::vec3 AtmToDisplay(const glm::vec3& lin) {
    const glm::vec3 lumaW(0.2126f, 0.7152f, 0.0722f);
    glm::vec3 x = glm::max(lin, glm::vec3(0.0f)) * kAtmExposure;
    const float lum0 = glm::dot(x, lumaW);
    x = glm::max(glm::vec3(lum0) + (x - glm::vec3(lum0)) * kAtmSaturation, glm::vec3(0.0f));
    const glm::vec3 perChannel(1.0f - std::exp(-x.r), 1.0f - std::exp(-x.g), 1.0f - std::exp(-x.b));
    const float lum = glm::dot(x, lumaW);
    const glm::vec3 huePreserving = x * ((1.0f - std::exp(-lum)) / std::max(lum, 1e-5f));
    glm::vec3 c = glm::mix(perChannel, huePreserving, 0.6f);
    const float m = std::max(c.r, std::max(c.g, c.b));
    if (m > 1.0f) c = glm::mix(c / m, glm::vec3(1.0f), std::clamp((m - 1.0f) / m, 0.0f, 1.0f));
    glm::vec3 out;
    for (int i = 0; i < 3; ++i) out[i] = std::pow(std::clamp(c[i], 0.0f, 1.0f), 1.0f / 2.2f);
    return out;
}

// Display colour the physical sky draws toward dir, clouds left out (they fade
// out at the horizon anyway) but with the overcast veil applied.
glm::vec3 EvaluatePhysicalSkyDisplay(const glm::vec3& dir, const glm::vec3& sunDir,
                                     float turbidity, float sunIntensity,
                                     float coverage, float darkness) {
    const float mieScale = std::clamp((turbidity - 1.0f) / 1.5f, 0.05f, 8.0f) * 2.0f;
    const float nightAmount = AtmSmoothstep(0.03f, -0.16f, sunDir.y);
    glm::vec3 lin = AtmRadiance(dir, sunDir, mieScale, sunIntensity) + AtmNightRadiance(dir) * nightAmount +
                    AtmBlueHourRadiance(dir, sunDir);
    const float overcast = AtmSmoothstep(0.55f, 0.95f, std::clamp(coverage, 0.0f, 1.0f));
    const float dark = std::clamp(darkness, 0.0f, 1.0f);
    const float lum = glm::dot(lin, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    const glm::vec3 grey = glm::vec3(lum) * glm::vec3(0.96f, 0.99f, 1.04f) * glm::mix(0.95f, 0.38f, dark);
    lin = glm::mix(lin, grey, overcast * 0.82f);
    return AtmToDisplay(lin);
}

// The legacy gradient sky (proceduralSky + the sun halo in main), clouds and
// stars left out, through its own tonemap.
glm::vec3 EvaluateLegacySkyDisplay(const glm::vec3& dir, const glm::vec3& sunDir) {
    const float dayAmount = AtmSmoothstep(-0.12f, 0.16f, sunDir.y);
    const float height = std::clamp(dir.y, -1.0f, 1.0f);
    const float horizonMix = AtmSmoothstep(-0.18f, 0.72f, height);
    const float twilight = 1.0f - AtmSmoothstep(0.08f, 0.38f, std::fabs(sunDir.y));
    glm::vec3 horizonCol = glm::mix(glm::vec3(0.03f, 0.05f, 0.10f), glm::vec3(1.02f, 0.62f, 0.34f), twilight);
    horizonCol = glm::mix(horizonCol, glm::vec3(0.84f, 0.92f, 1.02f), dayAmount);
    glm::vec3 upperCol = glm::mix(glm::vec3(0.005f, 0.015f, 0.045f), glm::vec3(0.15f, 0.22f, 0.45f), twilight);
    upperCol = glm::mix(upperCol, glm::vec3(0.18f, 0.48f, 0.88f), dayAmount);
    glm::vec3 col = glm::mix(horizonCol, upperCol, horizonMix);
    const float sunDot = std::max(glm::dot(dir, sunDir), 0.0f);
    const glm::vec3 scatterColor = glm::mix(glm::vec3(1.05f, 0.62f, 0.30f), glm::vec3(1.0f, 0.97f, 0.92f), dayAmount);
    col += scatterColor * (std::pow(sunDot, 9.0f) * 0.22f + std::pow(sunDot, 40.0f) * 0.55f) * glm::mix(0.35f, 1.0f, dayAmount);
    const float haze = std::exp(-std::max(height, -0.1f) * 9.0f);
    col += glm::mix(glm::vec3(0.06f, 0.08f, 0.12f), glm::vec3(1.0f, 0.93f, 0.86f), dayAmount) * haze * glm::mix(0.05f, 0.18f, dayAmount);
    col += glm::vec3(1.0f, 0.97f, 0.9f) * (std::pow(sunDot, 18.0f) * 0.45f) * dayAmount;
    for (int c = 0; c < 3; ++c) col[c] = 1.0f - std::exp(-col[c] * 1.15f);
    return col;
}
} // namespace

void Skybox::refreshMatchedFogColor() {
    const glm::vec3 sun = resolvedSunDirection();
    const float coverage = weatherDrive.active ? weatherDrive.cloudCoverage : settings.cloudCoverage;
    const float darkness = weatherDrive.active ? weatherDrive.cloudDarkness : settings.cloudDarkness;
    const float key[12] = {
        sun.x, sun.y, sun.z,
        static_cast<float>(settings.atmosphereModel),
        settings.turbidity, settings.sunIntensity,
        coverage, darkness,
        settings.fogMatchSky ? 1.0f : 0.0f,
        0.0f, 0.0f, 0.0f,
    };
    if (matchedFogValid && std::equal(key, key + 12, matchedFogKey)) return;
    std::copy(key, key + 12, matchedFogKey);
    matchedFogValid = true;

    // Average a ring of directions ~2 degrees above the horizon: that is the
    // band distant fogged geometry sits against.
    constexpr int kSamples = 8;
    constexpr float kElevation = 0.035f;
    const float horizontal = std::sqrt(1.0f - kElevation * kElevation);
    glm::vec3 sum(0.0f);
    for (int i = 0; i < kSamples; ++i) {
        const float a = (static_cast<float>(i) + 0.5f) / kSamples * 2.0f * kAtmPi;
        const glm::vec3 dir(std::cos(a) * horizontal, kElevation, std::sin(a) * horizontal);
        sum += settings.atmosphereModel == 1
            ? EvaluatePhysicalSkyDisplay(dir, sun, settings.turbidity, settings.sunIntensity, coverage, darkness)
            : EvaluateLegacySkyDisplay(dir, sun);
    }
    matchedFogColor = glm::clamp(sum / static_cast<float>(kSamples), glm::vec3(0.0f), glm::vec3(1.0f));
}

glm::vec3 Skybox::sceneFogColor() const {
    if (!settings.fogMatchSky || !matchedFogValid) return settings.fogColor;
    const bool hasScrollTexture = scrollingTexture != nullptr && scrollingTexture->GetID() != 0;
    if (settings.mode == SkyboxMode::Scrolling && hasScrollTexture) return settings.fogColor;
    return matchedFogColor;
}

namespace {
struct WeatherProfile {
    float coverage;
    float darkness;
    float rain;
    bool lightning;
    float wind;  // foliage wind scale, 1 = calm baseline
};

WeatherProfile ProfileFor(WeatherMode mode) {
    switch (mode) {
        case WeatherMode::Overcast:     return {0.75f, 0.35f, 0.0f, false, 1.3f};
        case WeatherMode::DarkCloudy:   return {0.92f, 0.70f, 0.0f, false, 1.7f};
        case WeatherMode::Rain:         return {0.90f, 0.55f, 0.75f, false, 2.0f};
        case WeatherMode::Thunderstorm: return {0.98f, 0.85f, 1.0f, true, 2.5f};
        case WeatherMode::Clear:
        default:                        return {0.25f, 0.0f, 0.0f, false, 1.0f};
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
    out.windMultiplier = clear.wind + (target.wind - clear.wind) * t;

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
