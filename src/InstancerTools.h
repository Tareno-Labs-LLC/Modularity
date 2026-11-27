#pragma once
// Authoring operations for the Instancer component (convert, scatter, clear). Pure scene-data
// logic with no ImGui or GL, so the inspector, scripts and tooling all share one implementation.
#include "SceneObject.h"

#include <functional>
#include <string>
#include <vector>

namespace InstancerTools {

// What lies under a world-space XZ position. Registered by whatever owns ground data (the
// Terrain component) so scatter can snap to it and stay out of paths, ponds and clearings.
struct GroundSample {
    bool valid = false;                          // false: no ground here (outside the terrain)
    float height = 0.0f;                         // world Y of the surface
    glm::vec3 normal = glm::vec3(0.0f, 1.0f, 0.0f);
    float keepOut = 0.0f;                        // 0 = free, >= keepOutThreshold means "do not place"
};
using GroundQuery = std::function<GroundSample(const std::vector<SceneObject>& objects, float worldX, float worldZ)>;
void SetGroundQuery(GroundQuery query);          // pass an empty function to unregister
const GroundQuery& GetGroundQuery();

struct ConvertResult {
    int converted = 0;       // children turned into instances (and deleted)
    int unmatched = 0;       // children left alone: no prototype with the same meshes
    int removedObjects = 0;  // scene objects deleted with those children
};
// Every direct child of the instancer whose renderer meshes match a prototype template becomes
// an instance (position, yaw and scale carried over) and the child subtree is deleted. The
// templates themselves are never converted or deleted. With `nested`, children that do not match
// are searched too (their own children, recursively), so a forest grouped under folders converts.
ConvertResult ConvertChildren(std::vector<SceneObject>& objects, int instancerId, bool nested = false);

struct ScatterParams {
    enum class Shape { Box = 0, Circle = 1 };
    Shape shape = Shape::Box;
    glm::vec2 boxSize = glm::vec2(60.0f, 60.0f);   // X/Z extent, centred on the instancer
    float radius = 30.0f;
    int count = 200;
    float minSpacing = 2.0f;                       // metres between instance origins; 0 = none
    uint32_t seed = 1;
    bool randomYaw = true;
    bool avoidExisting = true;                     // keep spacing from instances already placed
    float yOffset = 0.0f;                          // added to the snapped height
    float keepOutThreshold = 0.5f;
    int prototypeOverride = -1;                    // >= 0: scatter only this prototype
};
struct ScatterResult {
    int placed = 0;
    int rejectedSpacing = 0;
    int rejectedKeepOut = 0;
    int rejectedNoGround = 0;
};
ScatterResult Scatter(std::vector<SceneObject>& objects, int instancerId, const ScatterParams& params);

struct SnapResult {
    int snapped = 0;          // instances moved onto the ground
    int removedKeepOut = 0;   // instances deleted because they sit on a path / pond / clearing
    int noGround = 0;         // instances outside every terrain (left where they are)
};
// Re-seats every instance on the registered ground (the Terrain component): Y follows the surface
// (+ yOffset), prototypes with alignToGround re-tilt to the slope. With `removeKeepOut`, instances
// standing in paths, ponds or keep-out clearings are deleted. Does nothing without a ground query.
SnapResult SnapToGround(std::vector<SceneObject>& objects, int instancerId, bool removeKeepOut,
                        float yOffset = 0.0f, float keepOutThreshold = 0.5f);

// Instances of one prototype (-1 = all). Returns how many were removed. Later prototype
// indices shift down when a prototype is removed, so use RemovePrototype for that.
int ClearInstances(SceneObject& instancer, int prototypeIndex = -1);
// Removes the prototype and its instances and renumbers the rest.
void RemovePrototype(SceneObject& instancer, int prototypeIndex);

// Number of mesh-bearing objects in a template hierarchy (root included); 0 = nothing to draw.
int CountTemplateMeshes(const std::vector<SceneObject>& objects, int rootId);

} // namespace InstancerTools
