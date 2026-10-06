// Particle Systems in 3D views: the Renderer half of ParticleSystemRuntime.
//
// Particles used to be drawn as an ImGui overlay on top of the finished game image (the
// editor's Game view) or pushed through the 2D Light2D compositor (the player), so in a 3D
// view they were never part of the frame: no depth test, no fog, no post FX, and in the
// Game view they spilled past the edges of the image. Here they are geometry in the scene's
// own target, drawn from Renderer::renderSceneInternal.
//
// Quads are expanded on the CPU - a few thousand vertices at most, rebuilt per frame because
// every particle moves - and streamed through one buffer. Each system is one draw with its
// own texture and blend mode; systems go far to near, and within an alpha-blended system
// particles go back to front.

#include "Rendering.h"
#include "ParticleSystemRuntime.h"
#include "Profiler.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {

constexpr int kFloatsPerVertex = 9; // position xyz, uv, rgba

struct SystemDraw {
    const SceneObject* obj = nullptr;
    float distanceSq = 0.0f;
};

// The two directions a particle's quad spans: `axisX` across, `axisY` "up" in the texture.
struct QuadAxes {
    glm::vec3 axisX = glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 axisY = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 center = glm::vec3(0.0f);
    float halfX = 0.5f;
    float halfY = 0.5f;
};

// Spins a pair of axes in their own plane by the particle's rotation, matching the old
// overlay's convention so a rotated particle turns the same way it always did.
void Spin(glm::vec3& axisX, glm::vec3& axisY, float degrees) {
    if (degrees == 0.0f) return;
    const float angle = glm::radians(degrees);
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const glm::vec3 x = axisX;
    const glm::vec3 y = axisY;
    axisX = x * c + y * s;
    axisY = -x * s + y * c;
}

void ApplyBlendMode(int blendMode) {
    switch (blendMode) {
        case ParticleSystem2DComponent::BlendAdditive:
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
            break;
        case ParticleSystem2DComponent::BlendPremultiplied:
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case ParticleSystem2DComponent::BlendMultiply:
            glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
            break;
        default:
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            break;
    }
}

} // namespace

void Renderer::releaseParticleBuffers() {
    if (particleEBO) glDeleteBuffers(1, &particleEBO);
    if (particleVBO) glDeleteBuffers(1, &particleVBO);
    if (particleVAO) glDeleteVertexArrays(1, &particleVAO);
    particleEBO = 0;
    particleVBO = 0;
    particleVAO = 0;
    particleIndexQuads = 0;
}

void Renderer::renderParticleSystems(const std::vector<SceneObject>& sceneObjects,
                                     const glm::mat4& view, const glm::mat4& proj) {
    const glm::mat4 invView = glm::inverse(view);
    const glm::vec3 cameraPos = glm::vec3(invView[3]);
    const glm::vec3 cameraRight = glm::normalize(glm::vec3(invView[0]));
    const glm::vec3 cameraUp = glm::normalize(glm::vec3(invView[1]));
    const glm::vec3 cameraForward = -glm::normalize(glm::vec3(invView[2]));

    std::vector<SystemDraw> systems;
    for (const SceneObject& obj : sceneObjects) {
        if (!obj.hasParticleSystem2D || !IsObjectEnabledInHierarchy(obj)) continue;
        const ParticleSystem2DComponent& ps = obj.particleSystem2D;
        if (!ps.enabled || ps.particles.empty()) continue;
        const glm::vec3 offset = obj.position - cameraPos;
        systems.push_back({&obj, glm::dot(offset, offset)});
    }
    if (systems.empty()) return;
    std::stable_sort(systems.begin(), systems.end(),
                     [](const SystemDraw& a, const SystemDraw& b) { return a.distanceSq > b.distanceSq; });

    MODU_PROFILE_SCOPE("Particles", ProfilerSampleCategory::RenderDetail);
    static const std::string particleVert = "Resources/Shaders/particle_vert.glsl";
    static const std::string particleFrag = "Resources/Shaders/particle_frag.glsl";
    Shader* particleShader = getShader(particleVert, particleFrag);
    if (!particleShader || particleShader->ID == 0) return;

    if (particleVAO == 0) {
        glGenVertexArrays(1, &particleVAO);
        glGenBuffers(1, &particleVBO);
        glGenBuffers(1, &particleEBO);
        glBindVertexArray(particleVAO);
        glBindBuffer(GL_ARRAY_BUFFER, particleVBO);
        const GLsizei stride = kFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(5 * sizeof(float)));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, particleEBO); // recorded in the VAO
        glBindVertexArray(0);
    }

    // renderSceneInternal tracks its own blend/depth/cull/program/texture state in locals this
    // function cannot see, so put back exactly what was there instead of guessing.
    const GLboolean blendWas = glIsEnabled(GL_BLEND);
    const GLboolean depthTestWas = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cullWas = glIsEnabled(GL_CULL_FACE);
    GLboolean depthMaskWas = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMaskWas);
    GLint depthFuncWas = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &depthFuncWas);
    GLint blendSrcRgb = GL_ONE, blendDstRgb = GL_ZERO, blendSrcAlpha = GL_ONE, blendDstAlpha = GL_ZERO;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
    GLint programWas = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &programWas);
    GLint vaoWas = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vaoWas);
    GLint arrayBufferWas = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBufferWas);
    GLint activeTextureWas = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTextureWas);
    glActiveTexture(GL_TEXTURE0);
    GLint texture0Was = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0Was);

    glEnable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    particleShader->use();
    particleShader->setMat4("view", view);
    particleShader->setMat4("projection", proj);
    particleShader->setVec3("viewPos", cameraPos);
    particleShader->setInt("particleTexture", 0);
    if (skybox) {
        const SkyboxSettings& sky = skybox->getSettings();
        particleShader->setBool("fogEnabled", sky.fogEnabled);
        particleShader->setInt("fogMode", sky.fogMode);
        particleShader->setVec3("fogColor", sky.fogColor);
        particleShader->setFloat("fogStart", sky.fogStart);
        particleShader->setFloat("fogEnd", sky.fogEnd);
        particleShader->setFloat("fogDensity", sky.fogDensity);
        particleShader->setFloat("fogHeight", sky.fogHeight);
        particleShader->setFloat("fogHeightFalloff", sky.fogHeightFalloff);
    } else {
        particleShader->setBool("fogEnabled", false);
    }
    glBindVertexArray(particleVAO);
    glBindBuffer(GL_ARRAY_BUFFER, particleVBO);

    for (const SystemDraw& system : systems) {
        const SceneObject& obj = *system.obj;
        const ParticleSystem2DComponent& ps = obj.particleSystem2D;

        GLuint textureId = 0;
        int textureWidth = 0;
        int textureHeight = 0;
        const std::string& texturePath = ParticleRuntime::TexturePath(obj);
        if (!texturePath.empty()) {
            if (Texture* tex = getTexture(texturePath, ParticleRuntime::TextureFilter(obj))) {
                textureId = tex->GetID();
                textureWidth = tex->GetSourceWidth() > 0 ? tex->GetSourceWidth() : tex->GetWidth();
                textureHeight = tex->GetSourceHeight() > 0 ? tex->GetSourceHeight() : tex->GetHeight();
            }
        }
        if (textureId == 0) textureId = debugWhiteTexture;
        if (textureId == 0) continue;

        const ParticleRuntime::EmitterFrame emitter = ParticleRuntime::MakeEmitterFrame(obj);
        const size_t count = ps.particles.size();
        particleOrderScratch.resize(count);
        particleDepthScratch.resize(count);
        std::iota(particleOrderScratch.begin(), particleOrderScratch.end(), 0u);
        for (size_t i = 0; i < count; ++i) {
            const glm::vec3 world = ParticleRuntime::WorldPosition(emitter, ps.particles[i]);
            particleDepthScratch[i] = glm::dot(world - cameraPos, cameraForward);
        }
        // Additive and multiply give the same result in any order; the other two do not.
        const bool needsSort = ps.blendMode == ParticleSystem2DComponent::BlendAlpha ||
                               ps.blendMode == ParticleSystem2DComponent::BlendPremultiplied;
        if (needsSort) {
            std::sort(particleOrderScratch.begin(), particleOrderScratch.end(),
                      [&](uint32_t a, uint32_t b) { return particleDepthScratch[a] > particleDepthScratch[b]; });
        }

        const bool sheetAnimated = ParticleRuntime::SheetFrameCount(ps) > 0;
        particleVertexScratch.clear();
        particleVertexScratch.reserve(count * 4 * kFloatsPerVertex);
        size_t quads = 0;
        for (uint32_t index : particleOrderScratch) {
            const ParticleSystem2DComponent::Particle& p = ps.particles[index];
            if (!p.alive) continue;
            if (particleDepthScratch[index] < 0.0f) continue; // behind the camera
            const float t = ParticleRuntime::NormalizedAge(p);
            const float size = std::max(0.001f, ParticleRuntime::CurrentSize(ps, p, t));
            const ParticleRuntime::Frame frame =
                ParticleRuntime::CurrentFrame(ps, p, t, textureWidth, textureHeight);
            const glm::vec4 color = ParticleRuntime::CurrentColor(ps, p, t);

            QuadAxes quad;
            quad.center = ParticleRuntime::WorldPosition(emitter, p);
            quad.halfY = size * 0.5f;
            quad.halfX = quad.halfY * (sheetAnimated ? frame.aspect : 1.0f);
            switch (ps.renderAlignment) {
                case ParticleSystem2DComponent::AlignStretched: {
                    // Long axis along the motion, short axis turned toward the camera. A
                    // particle at rest, or moving straight at the lens, falls back to a
                    // plain billboard rather than collapsing to a line.
                    const glm::vec3 velocity = ParticleRuntime::WorldVelocity(emitter, p);
                    const float speed = glm::length(velocity);
                    glm::vec3 side(0.0f);
                    if (speed > 1e-4f) {
                        side = glm::cross(velocity / speed, cameraPos - quad.center);
                    }
                    if (speed > 1e-4f && glm::dot(side, side) > 1e-10f) {
                        quad.axisY = velocity / speed;
                        quad.axisX = glm::normalize(side);
                        quad.halfY = 0.5f * (size * std::max(0.0f, ps.stretchLengthScale) +
                                             speed * std::max(0.0f, ps.stretchSpeedScale));
                        quad.halfY = std::max(quad.halfY, 0.0005f);
                    } else {
                        quad.axisX = cameraRight;
                        quad.axisY = cameraUp;
                        Spin(quad.axisX, quad.axisY, p.rotation);
                    }
                    break;
                }
                case ParticleSystem2DComponent::AlignHorizontal:
                    quad.axisX = glm::vec3(1.0f, 0.0f, 0.0f);
                    quad.axisY = glm::vec3(0.0f, 0.0f, -1.0f);
                    Spin(quad.axisX, quad.axisY, p.rotation);
                    break;
                case ParticleSystem2DComponent::AlignVertical: {
                    // Upright like a tree card: turns about world Y only, and does not spin.
                    const glm::vec3 side =
                        glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), cameraPos - quad.center);
                    quad.axisX = glm::dot(side, side) > 1e-10f ? glm::normalize(side) : cameraRight;
                    quad.axisY = glm::vec3(0.0f, 1.0f, 0.0f);
                    break;
                }
                case ParticleSystem2DComponent::AlignLocal:
                    quad.axisX = emitter.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
                    quad.axisY = emitter.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
                    Spin(quad.axisX, quad.axisY, p.rotation);
                    break;
                default:
                    quad.axisX = cameraRight;
                    quad.axisY = cameraUp;
                    Spin(quad.axisX, quad.axisY, p.rotation);
                    break;
            }

            const glm::vec3 x = quad.axisX * quad.halfX;
            const glm::vec3 y = quad.axisY * quad.halfY;
            const glm::vec3 corners[4] = {
                quad.center - x - y, // bottom-left
                quad.center + x - y, // bottom-right
                quad.center + x + y, // top-right
                quad.center - x + y, // top-left
            };
            const glm::vec2 uvs[4] = {
                glm::vec2(frame.uvMin.x, frame.uvMin.y),
                glm::vec2(frame.uvMax.x, frame.uvMin.y),
                glm::vec2(frame.uvMax.x, frame.uvMax.y),
                glm::vec2(frame.uvMin.x, frame.uvMax.y),
            };
            for (int c = 0; c < 4; ++c) {
                particleVertexScratch.insert(particleVertexScratch.end(),
                                             {corners[c].x, corners[c].y, corners[c].z,
                                              uvs[c].x, uvs[c].y,
                                              color.r, color.g, color.b, color.a});
            }
            ++quads;
        }
        if (quads == 0) continue;

        if (quads > particleIndexQuads) {
            const size_t capacity = std::max(quads, particleIndexQuads * 2);
            std::vector<uint32_t> indices(capacity * 6);
            for (size_t q = 0; q < capacity; ++q) {
                const uint32_t base = static_cast<uint32_t>(q * 4);
                const size_t at = q * 6;
                indices[at + 0] = base;
                indices[at + 1] = base + 1;
                indices[at + 2] = base + 2;
                indices[at + 3] = base + 2;
                indices[at + 4] = base + 3;
                indices[at + 5] = base;
            }
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)),
                         indices.data(), GL_STATIC_DRAW);
            particleIndexQuads = capacity;
        }
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(particleVertexScratch.size() * sizeof(float)),
                     particleVertexScratch.data(), GL_STREAM_DRAW);

        ApplyBlendMode(ps.blendMode);
        particleShader->setInt("blendMode", ps.blendMode);
        particleShader->setVec3("emission", ps.runtimeMaterialEmission);
        glBindTexture(GL_TEXTURE_2D, textureId);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(quads * 6), GL_UNSIGNED_INT, nullptr);
        recordDrawCall();
    }

    glBindVertexArray(static_cast<GLuint>(vaoWas));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBufferWas));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture0Was));
    glActiveTexture(static_cast<GLenum>(activeTextureWas));
    glUseProgram(static_cast<GLuint>(programWas));
    glBlendFuncSeparate(blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha);
    glDepthMask(depthMaskWas);
    glDepthFunc(depthFuncWas);
    if (blendWas) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (depthTestWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (cullWas) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}
