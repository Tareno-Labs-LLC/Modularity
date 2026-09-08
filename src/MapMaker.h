#pragma once

#include "SceneObject.h"
#include "ModelLoader.h"
#include <vector>

namespace MapMaker {

std::string GenerateId(const char* prefix);

const char* TransitionKindLabel(MapTransitionKind kind);
constexpr int kMapTransitionKindCount = 7;

SceneObject* FindMapRoot(std::vector<SceneObject>& objects);
const SceneObject* FindMapRoot(const std::vector<SceneObject>& objects);
SceneObject* FindSectorById(std::vector<SceneObject>& objects, const std::string& sectorId);
const SceneObject* FindSectorById(const std::vector<SceneObject>& objects, const std::string& sectorId);
SceneObject* FindTransitionById(std::vector<SceneObject>& objects, const std::string& transitionId);
SceneObject* FindPortalById(std::vector<SceneObject>& objects, const std::string& portalId);
const SceneObject* FindPortalById(const std::vector<SceneObject>& objects, const std::string& portalId);
std::vector<SceneObject*> GatherSectors(std::vector<SceneObject>& objects);
std::vector<const SceneObject*> GatherSectors(const std::vector<SceneObject>& objects);
std::vector<SceneObject*> GatherTransitions(std::vector<SceneObject>& objects);
std::vector<const SceneObject*> GatherTransitions(const std::vector<SceneObject>& objects);

const SceneObject* FindOwningSector(const std::vector<SceneObject>& objects, const SceneObject& obj);

inline bool TransitionTouchesSector(const MapTransitionComponent& t, const std::string& sectorId) {
    return !sectorId.empty() &&
           (t.sourceSectorId == sectorId || t.destinationSectorId == sectorId);
}

std::vector<std::string> GatherAdjacentSectorIds(const std::vector<SceneObject>& objects,
                                                 const std::string& sectorId);

void PrepareDuplicatedObjectMapComponents(SceneObject& obj);

void RemapMapIdsForPastedObjects(std::vector<SceneObject*>& pastedObjects);

enum class MapIssueSeverity {
    Info = 0,
    Warning = 1,
    Error = 2
};

struct MapValidationIssue {
    MapIssueSeverity severity = MapIssueSeverity::Warning;
    std::string message;
    int objectId = -1;
};

std::vector<MapValidationIssue> ValidateMap(const std::vector<SceneObject>& objects);

std::vector<std::string> ValidateMeshAsset(const RawMeshAsset& mesh);

} // namespace MapMaker
