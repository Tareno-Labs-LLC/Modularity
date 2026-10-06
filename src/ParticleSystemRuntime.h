#pragma once

// Particle System simulation, plus the per-particle maths every renderer shares: the 3D pass
// in the Renderer (RenderParticles.cpp) and the 2D / 2.5D sprite paths in the viewports. One
// copy, so a particle cannot look different depending on which view happens to draw it.
//
// Simulation runs once per frame from Engine::updateParticleSystems on game time. It used to
// run from inside each viewport's draw on wall-clock time, so two views drawing in one frame
// stepped it twice (a zero dt was bumped to 1/60), and a paused game kept emitting.

#include "SceneObject.h"

namespace ParticleRuntime {

using Component = ParticleSystem2DComponent;
using Particle = ParticleSystem2DComponent::Particle;

uint32_t Hash(uint32_t x);
float Unit(uint32_t& state);
float Range(const Component::MinMaxFloat& range, uint32_t& state);

// Rewinds a system to "never ran": the next Simulate restarts (and prewarms) it.
void ResetRuntime(Component& ps);
// Clears the live particles and starts the clock again from zero.
void Restart(Component& ps);
// Advances one system by `dt` seconds of game time.
void Simulate(SceneObject& obj, float dt);

// Where a system's particle positions live. Local particles are stored relative to the
// emitter and carried by its transform; world particles are stored finished.
struct EmitterFrame {
    glm::vec3 origin = glm::vec3(0.0f);
    glm::mat3 rotation = glm::mat3(1.0f); // Rx * Ry * Rz, the engine's Euler order
    glm::vec3 scale = glm::vec3(1.0f);
    bool world = false;
};
EmitterFrame MakeEmitterFrame(const SceneObject& obj);
glm::vec3 WorldPosition(const EmitterFrame& frame, const Particle& p);
glm::vec3 WorldVelocity(const EmitterFrame& frame, const Particle& p);

float NormalizedAge(const Particle& p);
float CurrentSize(const Component& ps, const Particle& p, float t);
// Start colour through colour over lifetime, times the material's colour and opacity.
glm::vec4 CurrentColor(const Component& ps, const Particle& p, float t);

// The texture a system draws with: its Texture slot, else its material's albedo, else the
// object's own albedo. Empty means untextured (renderers use their white texture).
const std::string& TexturePath(const SceneObject& obj);
MaterialProperties::TextureFilter TextureFilter(const SceneObject& obj);

// A particle's current texture sheet frame, in UVs for the engine's bottom-up textures:
// uvMin = (left, bottom), uvMax = (right, top). `aspect` is the frame's width / height in
// source pixels. With the sheet off it is the whole texture at aspect 1, as before.
struct Frame {
    glm::vec2 uvMin = glm::vec2(0.0f);
    glm::vec2 uvMax = glm::vec2(1.0f);
    float aspect = 1.0f;
};
// Frames the animation walks through; 0 when texture sheet animation is off or has none.
int SheetFrameCount(const Component& ps);
Frame CurrentFrame(const Component& ps, const Particle& p, float t, int textureWidth,
                   int textureHeight);

} // namespace ParticleRuntime
