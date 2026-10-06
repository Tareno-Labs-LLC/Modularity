#include "ParticleSystemRuntime.h"

#include "ThirdParty/glm/gtc/matrix_transform.hpp"

#include <algorithm>
#include <cmath>

namespace ParticleRuntime {

namespace {

constexpr float kTwoPi = 6.28318530718f;

int SheetTotalFrames(const Component& ps) {
    if (!ps.textureSheetEnabled) return 0;
    if (ps.textureSheetMode == Component::SheetSpriteClips) {
        return static_cast<int>(ps.runtimeSheetRects.size());
    }
    return std::max(1, ps.textureSheetTilesX) * std::max(1, ps.textureSheetTilesY);
}

void Emit(SceneObject& obj, uint32_t emissionIndex) {
    Component& ps = obj.particleSystem2D;
    if (static_cast<int>(ps.particles.size()) >= std::max(1, ps.maxParticles)) return;
    // The salt is 0 until a script replays the system, so unscripted systems seed as before.
    uint32_t seed = ps.autoRandomSeed
                        ? Hash(static_cast<uint32_t>(obj.id) * 747796405u + emissionIndex +
                               static_cast<uint32_t>(ps.runtimeTime * 1000.0f) +
                               ps.runtimeEmitSalt * 2654435761u)
                        : Hash(ps.randomSeed + emissionIndex);
    Particle particle;
    particle.alive = true;
    particle.seed = seed;
    particle.lifetime = std::max(0.01f, Range(ps.startLifetime, seed));
    particle.size = std::max(0.0f, Range(ps.startSize, seed));
    particle.rotation = Range(ps.startRotation, seed);
    particle.startColor = ps.startColor;

    // The draws from `seed` below keep their original order for the first three shapes, so
    // a fixed-seed system emits exactly the particles it always did.
    const bool in3D = ps.emitIn3D;
    const float angle = Unit(seed) * kTwoPi;
    glm::vec3 direction(std::cos(angle), std::sin(angle), 0.0f);
    if (in3D) {
        const float z = Unit(seed) * 2.0f - 1.0f;
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        direction = glm::vec3(std::cos(angle) * r, std::sin(angle) * r, z);
    }

    glm::vec3 position(0.0f);
    switch (ps.shape) {
        case Component::ShapeCircle: {
            const float u = Unit(seed);
            const float radius = ps.shapeRadius * (in3D ? std::cbrt(u) : std::sqrt(u));
            position = direction * radius;
            break;
        }
        case Component::ShapeBox: {
            position = glm::vec3((Unit(seed) - 0.5f) * ps.shapeBox.x,
                                 (Unit(seed) - 0.5f) * ps.shapeBox.y,
                                 in3D ? (Unit(seed) - 0.5f) * ps.shapeBox.z : 0.0f);
            if (glm::length(position) > 0.0001f) direction = glm::normalize(position);
            break;
        }
        case Component::ShapeCone: {
            // A disc across the emitter's XZ plane (a line along X in 2D) firing along +Y. The
            // spread comes from where on the disc a particle starts - the rim leaves at
            // shapeAngle - so a zero-radius cone still sprays, just from a single point.
            glm::vec2 disc(0.0f);
            if (in3D) {
                const float a = Unit(seed) * kTwoPi;
                const float r = std::sqrt(Unit(seed));
                disc = glm::vec2(std::cos(a) * r, std::sin(a) * r);
            } else {
                disc = glm::vec2(Unit(seed) * 2.0f - 1.0f, 0.0f);
            }
            position = glm::vec3(disc.x, 0.0f, disc.y) * ps.shapeRadius;
            const float spread =
                std::tan(glm::radians(std::clamp(ps.shapeAngle, 0.0f, 89.0f)));
            direction = glm::normalize(glm::vec3(disc.x * spread, 1.0f, disc.y * spread));
            break;
        }
        case Component::ShapeHemisphere: {
            direction.y = std::abs(direction.y);
            const float u = Unit(seed);
            const float radius = ps.shapeRadius * (in3D ? std::cbrt(u) : std::sqrt(u));
            position = direction * radius;
            break;
        }
        case Component::ShapeEdge: {
            position = glm::vec3((Unit(seed) * 2.0f - 1.0f) * ps.shapeRadius, 0.0f, 0.0f);
            direction = glm::vec3(0.0f, 1.0f, 0.0f);
            break;
        }
        default:
            break;
    }
    glm::vec3 velocity = direction * Range(ps.startSpeed, seed);
    particle.angularVelocity = ps.rotationOverLifetimeEnabled ? ps.rotationOverLifetime : 0.0f;
    // Rolled even when random start is off, so toggling it never reshuffles other draws.
    particle.sheetFrameOffset = static_cast<int>(Unit(seed) * 65536.0f);

    if (ps.simulationSpace == Component::SpaceWorld) {
        const EmitterFrame frame = MakeEmitterFrame(obj);
        particle.position = frame.origin + frame.rotation * (position * frame.scale);
        particle.velocity = frame.rotation * velocity;
    } else {
        particle.position = position;
        particle.velocity = velocity;
    }
    ps.particles.push_back(particle);
}

void Step(SceneObject& obj, float dt) {
    Component& ps = obj.particleSystem2D;
    if (ps.paused) return;
    dt *= std::max(0.0f, ps.simulationSpeed);
    if (dt <= 0.0f) return;

    // Particles.Emit from a script fires whether or not the system is playing.
    int emitCount = std::max(0, ps.runtimePendingEmit);
    ps.runtimePendingEmit = 0;

    // Only emission stops with the system. Particles already out keep moving and dying - they
    // used to freeze where they were the moment a one-shot finished, never to be removed.
    if (ps.playing) {
        ps.runtimeTime += dt;
        if (ps.runtimeTime >= ps.startDelay) {
            ps.runtimeAccumulator += std::max(0.0f, ps.emissionRate) * dt;
            const int rateCount = static_cast<int>(ps.runtimeAccumulator);
            ps.runtimeAccumulator -= static_cast<float>(rateCount);
            emitCount += rateCount;
            if (ps.burstCount > 0) {
                if (ps.burstLoop && ps.burstTime > 0.0f) {
                    const int previousBurst =
                        static_cast<int>(std::floor((ps.runtimeTime - dt) / ps.burstTime));
                    const int currentBurst =
                        static_cast<int>(std::floor(ps.runtimeTime / ps.burstTime));
                    if (currentBurst > previousBurst) {
                        emitCount += (currentBurst - previousBurst) * ps.burstCount;
                    }
                } else if (ps.runtimeTime - dt <= ps.burstTime && ps.runtimeTime >= ps.burstTime) {
                    emitCount += ps.burstCount;
                }
            }
        }
    }
    for (int i = 0; i < emitCount; ++i) {
        Emit(obj, static_cast<uint32_t>(ps.particles.size() + i));
    }

    for (Particle& particle : ps.particles) {
        if (!particle.alive) continue;
        particle.age += dt;
        if (particle.age >= particle.lifetime) {
            particle.alive = false;
            continue;
        }
        particle.velocity.y -= ps.gravityModifier * dt;
        // Velocity over lifetime and noise ride on top of the particle's own velocity. They
        // used to be added INTO it every frame, which turned them into accelerations whose
        // strength depended on the frame rate.
        glm::vec3 velocity = particle.velocity;
        if (ps.velocityOverLifetimeEnabled) velocity += ps.velocityOverLifetime;
        if (ps.noiseEnabled && ps.noiseStrength > 0.0f) {
            const float frequency = std::max(0.01f, ps.noiseFrequency);
            const float n = std::sin((ps.runtimeTime + static_cast<float>(particle.seed % 997u)) * frequency);
            const float m = std::cos((ps.runtimeTime * 0.7f + static_cast<float>(particle.seed % 613u)) * frequency);
            const glm::vec3 drift(std::cos(n * kTwoPi), std::sin(n * kTwoPi),
                                  ps.emitIn3D ? std::sin(m * kTwoPi) : 0.0f);
            velocity += drift * ps.noiseStrength;
        }
        particle.position += velocity * dt;
        particle.rotation += particle.angularVelocity * dt;
    }
    ps.particles.erase(std::remove_if(ps.particles.begin(), ps.particles.end(),
                                      [](const Particle& p) { return !p.alive; }),
                       ps.particles.end());
    if (ps.playing && !ps.looping && ps.runtimeTime > ps.startDelay + ps.startLifetime.max) {
        ps.playing = false;
    }
}

} // namespace

uint32_t Hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float Unit(uint32_t& state) {
    state = Hash(state + 0x9e3779b9u);
    return static_cast<float>(state & 0x00ffffffu) / static_cast<float>(0x01000000u);
}

float Range(const Component::MinMaxFloat& range, uint32_t& state) {
    if (!range.random) return range.min;
    return glm::mix(range.min, range.max, Unit(state));
}

void ResetRuntime(Component& ps) {
    ps.particles.clear();
    ps.runtimeAccumulator = 0.0f;
    ps.runtimeTime = 0.0f;
    ps.runtimeLastUpdateTime = 0.0;
    ps.runtimeInitialized = false;
    ps.runtimePendingEmit = 0;
    ps.playing = ps.playOnAwake;
    ps.paused = false;
}

void Restart(Component& ps) {
    ps.particles.clear();
    ps.runtimeAccumulator = 0.0f;
    ps.runtimeTime = 0.0f;
    ps.runtimeInitialized = true;
    ps.playing = ps.playOnAwake;
    ps.paused = false;
}

void Simulate(SceneObject& obj, float dt) {
    if (!obj.hasParticleSystem2D) return;
    Component& ps = obj.particleSystem2D;
    if (!ps.enabled) return;
    if (!ps.runtimeInitialized) {
        Restart(ps);
        if (ps.prewarm) {
            const float step = 1.0f / 30.0f;
            const float prewarmTime = std::max(0.0f, ps.startLifetime.max);
            for (float t = 0.0f; t < prewarmTime; t += step) {
                Step(obj, step);
            }
        }
    }
    if (dt > 0.0f) Step(obj, dt);
}

EmitterFrame MakeEmitterFrame(const SceneObject& obj) {
    EmitterFrame frame;
    frame.origin = obj.position;
    glm::mat4 rotation(1.0f);
    rotation = glm::rotate(rotation, glm::radians(obj.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
    rotation = glm::rotate(rotation, glm::radians(obj.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
    rotation = glm::rotate(rotation, glm::radians(obj.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
    frame.rotation = glm::mat3(rotation);
    frame.scale = obj.scale;
    frame.world = obj.particleSystem2D.simulationSpace == Component::SpaceWorld;
    return frame;
}

glm::vec3 WorldPosition(const EmitterFrame& frame, const Particle& p) {
    if (frame.world) return p.position;
    return frame.origin + frame.rotation * (p.position * frame.scale);
}

glm::vec3 WorldVelocity(const EmitterFrame& frame, const Particle& p) {
    if (frame.world) return p.velocity;
    return frame.rotation * p.velocity;
}

float NormalizedAge(const Particle& p) {
    return std::clamp(p.age / std::max(0.01f, p.lifetime), 0.0f, 1.0f);
}

float CurrentSize(const Component& ps, const Particle& p, float t) {
    return ps.sizeOverLifetimeEnabled ? glm::mix(p.size, ps.sizeOverLifetime, t) : p.size;
}

glm::vec4 CurrentColor(const Component& ps, const Particle& p, float t) {
    const glm::vec4 color = ps.colorOverLifetimeEnabled
                                ? glm::mix(p.startColor, ps.colorOverLifetime, t)
                                : p.startColor;
    return color * ps.runtimeMaterialTint;
}

const std::string& TexturePath(const SceneObject& obj) {
    const Component& ps = obj.particleSystem2D;
    if (!ps.texturePath.empty()) return ps.texturePath;
    if (!ps.runtimeMaterialTexture.empty()) return ps.runtimeMaterialTexture;
    return obj.albedoTexturePath;
}

MaterialProperties::TextureFilter TextureFilter(const SceneObject& obj) {
    const Component& ps = obj.particleSystem2D;
    if (ps.runtimeMaterialFilter >= 0) {
        return static_cast<MaterialProperties::TextureFilter>(ps.runtimeMaterialFilter);
    }
    return obj.material.textureFilter;
}

int SheetFrameCount(const Component& ps) {
    const int total = SheetTotalFrames(ps);
    if (total <= 0) return 0;
    const int start = std::clamp(ps.textureSheetStartFrame, 0, total - 1);
    int count = total - start;
    if (ps.textureSheetFrameCount > 0) count = std::min(count, ps.textureSheetFrameCount);
    return std::max(1, count);
}

Frame CurrentFrame(const Component& ps, const Particle& p, float t, int textureWidth,
                   int textureHeight) {
    Frame frame;
    const int count = SheetFrameCount(ps);
    if (count <= 0) return frame;
    const int total = SheetTotalFrames(ps);
    const int start = std::clamp(ps.textureSheetStartFrame, 0, total - 1);

    const bool byFps = ps.textureSheetTiming == Component::SheetFps;
    const float position = byFps ? p.age * std::max(0.0f, ps.textureSheetFps)
                                 : t * std::max(0.0f, ps.textureSheetCycles) * static_cast<float>(count);
    int index = static_cast<int>(std::floor(position));
    if (ps.textureSheetRandomStart) index += p.sheetFrameOffset % count;
    if (byFps && !ps.textureSheetLoop) {
        index = std::min(index, count - 1);
    } else {
        index = ((index % count) + count) % count;
    }
    const int sheetFrame = start + index;

    if (ps.textureSheetMode == Component::SheetSpriteClips) {
        if (textureWidth <= 0 || textureHeight <= 0 ||
            sheetFrame >= static_cast<int>(ps.runtimeSheetRects.size())) {
            return frame;
        }
        // Clip rects are top-left-origin source pixels, the same as a UI Image's clips.
        const glm::ivec4 rect = ps.runtimeSheetRects[sheetFrame];
        const float invW = 1.0f / static_cast<float>(textureWidth);
        const float invH = 1.0f / static_cast<float>(textureHeight);
        frame.uvMin = glm::vec2(static_cast<float>(rect.x) * invW,
                                1.0f - static_cast<float>(rect.y + rect.w) * invH);
        frame.uvMax = glm::vec2(static_cast<float>(rect.x + rect.z) * invW,
                                1.0f - static_cast<float>(rect.y) * invH);
        frame.aspect = rect.w > 0 ? static_cast<float>(rect.z) / static_cast<float>(rect.w) : 1.0f;
        return frame;
    }

    const int columns = std::max(1, ps.textureSheetTilesX);
    const int rows = std::max(1, ps.textureSheetTilesY);
    const int column = sheetFrame % columns;
    const int row = sheetFrame / columns;
    frame.uvMin = glm::vec2(static_cast<float>(column) / static_cast<float>(columns),
                            1.0f - static_cast<float>(row + 1) / static_cast<float>(rows));
    frame.uvMax = glm::vec2(static_cast<float>(column + 1) / static_cast<float>(columns),
                            1.0f - static_cast<float>(row) / static_cast<float>(rows));
    if (textureWidth > 0 && textureHeight > 0) {
        frame.aspect = (static_cast<float>(textureWidth) / static_cast<float>(columns)) /
                       (static_cast<float>(textureHeight) / static_cast<float>(rows));
    }
    return frame;
}

} // namespace ParticleRuntime
