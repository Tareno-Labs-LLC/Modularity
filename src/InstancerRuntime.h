#pragma once
// Runtime-safe helpers for the Instancer component (compiled into the editor and the player):
// the scene blob codec, the per-instance transform, and collider placement. Anything that needs
// ImGui or editor state lives with the Inspector instead.
#include "SceneObject.h"

#include <string>
#include <vector>

namespace InstancerRuntime {

// "v1:" + base64 of 26 little-endian bytes per instance
// (f32 x, y, z, yaw, scale; u16 prototype; u8 tint; i8 tiltX; i8 tiltZ).
// The prefix keeps the value from ever looking like a path to the scene text portability pass.
std::string EncodeInstances(const std::vector<InstancerInstance>& instances);
// False on a malformed blob; `out` then holds whatever decoded before the damage.
bool DecodeInstances(const std::string& text, std::vector<InstancerInstance>& out);

// Ground-normal tilt stored per instance (normal.xz * 127, quantised).
void SetInstanceTilt(InstancerInstance& instance, const glm::vec3& groundNormal);
glm::vec3 InstanceUp(const InstancerInstance& instance);

// The object's world matrix exactly as the renderer builds it (T * Rx * Ry * Rz * S).
glm::mat4 ObjectWorldMatrix(const SceneObject& object);

// T(position) * tilt * Ry(yaw) * S(scale), in the instancer object's local space.
glm::mat4 InstanceLocalMatrix(const InstancerInstance& instance);

// World-space capsule for an instance of a prototype whose colliderMode is Capsule.
struct InstanceCapsule {
    glm::vec3 base;       // Bottom of the capsule (world).
    glm::quat rotation;   // Instance orientation (world).
    float radius = 0.0f;
    float height = 0.0f;  // Total height including both caps, always >= 2 * radius.
};
InstanceCapsule BuildInstanceCapsule(const InstancerPrototype& prototype,
                                     const InstancerInstance& instance,
                                     const glm::mat4& instancerWorld);

// A hash of everything about the prototype list that changes what is drawn or collided.
uint64_t HashPrototypes(const std::vector<InstancerPrototype>& prototypes);

} // namespace InstancerRuntime
