#include "MapMaker.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace MapMaker {

#pragma region Ids
std::string GenerateId(const char* prefix) {
    static std::mt19937_64 rng(std::random_device{}());
    const uint64_t value = rng();
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%s_%016llx",
                  (prefix != nullptr && *prefix != '\0') ? prefix : "map",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

const char* TransitionKindLabel(MapTransitionKind kind) {
    switch (kind) {
        case MapTransitionKind::Door: return "Door";
        case MapTransitionKind::OpenPassage: return "Open Passage";
        case MapTransitionKind::Elevator: return "Elevator";
        case MapTransitionKind::Ladder: return "Ladder";
        case MapTransitionKind::Teleport: return "Teleport";
        case MapTransitionKind::Loading: return "Loading Transition";
        case MapTransitionKind::Custom: return "Custom";
    }
    return "Door";
}
#pragma endregion

#pragma region Scene Queries
SceneObject* FindMapRoot(std::vector<SceneObject>& objects) {
    for (SceneObject& obj : objects) {
        if (obj.hasMapRoot) return &obj;
    }
    return nullptr;
}

const SceneObject* FindMapRoot(const std::vector<SceneObject>& objects) {
    for (const SceneObject& obj : objects) {
        if (obj.hasMapRoot) return &obj;
    }
    return nullptr;
}

SceneObject* FindSectorById(std::vector<SceneObject>& objects, const std::string& sectorId) {
    if (sectorId.empty()) return nullptr;
    for (SceneObject& obj : objects) {
        if (obj.hasMapSector && obj.mapSector.sectorId == sectorId) return &obj;
    }
    return nullptr;
}

const SceneObject* FindSectorById(const std::vector<SceneObject>& objects, const std::string& sectorId) {
    if (sectorId.empty()) return nullptr;
    for (const SceneObject& obj : objects) {
        if (obj.hasMapSector && obj.mapSector.sectorId == sectorId) return &obj;
    }
    return nullptr;
}

SceneObject* FindTransitionById(std::vector<SceneObject>& objects, const std::string& transitionId) {
    if (transitionId.empty()) return nullptr;
    for (SceneObject& obj : objects) {
        if (obj.hasMapTransition && obj.mapTransition.transitionId == transitionId) return &obj;
    }
    return nullptr;
}

SceneObject* FindPortalById(std::vector<SceneObject>& objects, const std::string& portalId) {
    if (portalId.empty()) return nullptr;
    for (SceneObject& obj : objects) {
        if (obj.hasMapPortal && obj.mapPortal.portalId == portalId) return &obj;
    }
    return nullptr;
}

const SceneObject* FindPortalById(const std::vector<SceneObject>& objects, const std::string& portalId) {
    if (portalId.empty()) return nullptr;
    for (const SceneObject& obj : objects) {
        if (obj.hasMapPortal && obj.mapPortal.portalId == portalId) return &obj;
    }
    return nullptr;
}

std::vector<SceneObject*> GatherSectors(std::vector<SceneObject>& objects) {
    std::vector<SceneObject*> result;
    for (SceneObject& obj : objects) {
        if (obj.hasMapSector) result.push_back(&obj);
    }
    return result;
}

std::vector<const SceneObject*> GatherSectors(const std::vector<SceneObject>& objects) {
    std::vector<const SceneObject*> result;
    for (const SceneObject& obj : objects) {
        if (obj.hasMapSector) result.push_back(&obj);
    }
    return result;
}

std::vector<SceneObject*> GatherTransitions(std::vector<SceneObject>& objects) {
    std::vector<SceneObject*> result;
    for (SceneObject& obj : objects) {
        if (obj.hasMapTransition) result.push_back(&obj);
    }
    return result;
}

std::vector<const SceneObject*> GatherTransitions(const std::vector<SceneObject>& objects) {
    std::vector<const SceneObject*> result;
    for (const SceneObject& obj : objects) {
        if (obj.hasMapTransition) result.push_back(&obj);
    }
    return result;
}

const SceneObject* FindOwningSector(const std::vector<SceneObject>& objects, const SceneObject& obj) {
    std::unordered_map<int, const SceneObject*> byId;
    byId.reserve(objects.size());
    for (const SceneObject& candidate : objects) {
        byId[candidate.id] = &candidate;
    }
    const SceneObject* current = &obj;
    for (size_t depth = 0; current != nullptr && depth <= objects.size(); ++depth) {
        if (current->hasMapSector) return current;
        if (current->parentId < 0) break;
        auto it = byId.find(current->parentId);
        current = (it != byId.end()) ? it->second : nullptr;
    }
    return nullptr;
}

std::vector<std::string> GatherAdjacentSectorIds(const std::vector<SceneObject>& objects,
                                                 const std::string& sectorId) {
    std::vector<std::string> result;
    if (sectorId.empty()) return result;
    std::unordered_set<std::string> seen;
    for (const SceneObject& obj : objects) {
        if (!obj.hasMapTransition || !obj.mapTransition.enabled) continue;
        const MapTransitionComponent& t = obj.mapTransition;
        std::string other;
        if (t.sourceSectorId == sectorId) {
            other = t.destinationSectorId;
        } else if (t.destinationSectorId == sectorId && t.bidirectional) {
            other = t.sourceSectorId;
        }
        if (other.empty() || other == sectorId) continue;
        if (seen.insert(other).second) result.push_back(other);
    }
    return result;
}
#pragma endregion

#pragma region Copy Support
void PrepareDuplicatedObjectMapComponents(SceneObject& obj) {
    if (obj.hasMapRoot) {
        obj.mapRoot.mapId = GenerateId("map");
    }
    if (obj.hasMapSector) {
        obj.mapSector.sectorId = GenerateId("sec");
        obj.mapSector.graphPosition += glm::vec2(40.0f, 24.0f);
    }
    if (obj.hasMapTransition) {
        obj.mapTransition.transitionId = GenerateId("trn");
        obj.mapTransition.sourcePortalId.clear();
        obj.mapTransition.destinationPortalId.clear();
    }
    if (obj.hasMapPortal) {
        obj.mapPortal.portalId = GenerateId("prt");
        obj.mapPortal.transitionId.clear();
    }
}

void RemapMapIdsForPastedObjects(std::vector<SceneObject*>& pastedObjects) {
    std::unordered_map<std::string, std::string> idRemap;
    for (SceneObject* obj : pastedObjects) {
        if (obj == nullptr) continue;
        if (obj->hasMapRoot && !obj->mapRoot.mapId.empty()) {
            const std::string next = GenerateId("map");
            idRemap[obj->mapRoot.mapId] = next;
            obj->mapRoot.mapId = next;
        }
        if (obj->hasMapSector && !obj->mapSector.sectorId.empty()) {
            const std::string next = GenerateId("sec");
            idRemap[obj->mapSector.sectorId] = next;
            obj->mapSector.sectorId = next;
            obj->mapSector.graphPosition += glm::vec2(40.0f, 24.0f);
        }
        if (obj->hasMapTransition && !obj->mapTransition.transitionId.empty()) {
            const std::string next = GenerateId("trn");
            idRemap[obj->mapTransition.transitionId] = next;
            obj->mapTransition.transitionId = next;
        }
        if (obj->hasMapPortal && !obj->mapPortal.portalId.empty()) {
            const std::string next = GenerateId("prt");
            idRemap[obj->mapPortal.portalId] = next;
            obj->mapPortal.portalId = next;
        }
    }
    if (idRemap.empty()) return;

    auto remap = [&](std::string& id) {
        if (id.empty()) return;
        auto it = idRemap.find(id);
        if (it != idRemap.end()) id = it->second;
    };
    for (SceneObject* obj : pastedObjects) {
        if (obj == nullptr) continue;
        if (obj->hasMapRoot) {
            remap(obj->mapRoot.startSectorId);
            remap(obj->mapRoot.activeSectorId);
        }
        if (obj->hasMapTransition) {
            remap(obj->mapTransition.sourceSectorId);
            remap(obj->mapTransition.destinationSectorId);
            remap(obj->mapTransition.sourcePortalId);
            remap(obj->mapTransition.destinationPortalId);
        }
        if (obj->hasMapPortal) {
            remap(obj->mapPortal.transitionId);
            remap(obj->mapPortal.sectorId);
        }
    }
}
#pragma endregion

#pragma region Validation
std::vector<MapValidationIssue> ValidateMap(const std::vector<SceneObject>& objects) {
    std::vector<MapValidationIssue> issues;
    auto add = [&](MapIssueSeverity severity, const std::string& message, int objectId) {
        issues.push_back({severity, message, objectId});
    };

    const SceneObject* mapRoot = FindMapRoot(objects);
    int mapRootCount = 0;
    for (const SceneObject& obj : objects) {
        if (obj.hasMapRoot) ++mapRootCount;
    }
    if (mapRootCount > 1) {
        add(MapIssueSeverity::Warning,
            "Scene has " + std::to_string(mapRootCount) +
                " Map Root components; only the first is used",
            mapRoot != nullptr ? mapRoot->id : -1);
    }

    std::unordered_map<std::string, const SceneObject*> sectorsById;
    std::unordered_map<std::string, const SceneObject*> portalsById;
    std::unordered_map<std::string, const SceneObject*> transitionsById;

    for (const SceneObject& obj : objects) {
        if (obj.hasMapSector) {
            const std::string& id = obj.mapSector.sectorId;
            if (id.empty()) {
                add(MapIssueSeverity::Error, "Sector \"" + obj.name + "\" has an empty sector id", obj.id);
            } else if (!sectorsById.emplace(id, &obj).second) {
                add(MapIssueSeverity::Error,
                    "Duplicate sector id \"" + id + "\" on \"" + obj.name + "\"", obj.id);
            }
        }
        if (obj.hasMapPortal) {
            const std::string& id = obj.mapPortal.portalId;
            if (id.empty()) {
                add(MapIssueSeverity::Error, "Portal \"" + obj.name + "\" has an empty portal id", obj.id);
            } else if (!portalsById.emplace(id, &obj).second) {
                add(MapIssueSeverity::Error,
                    "Duplicate portal id \"" + id + "\" on \"" + obj.name + "\"", obj.id);
            }
        }
        if (obj.hasMapTransition) {
            const std::string& id = obj.mapTransition.transitionId;
            if (id.empty()) {
                add(MapIssueSeverity::Error, "Transition \"" + obj.name + "\" has an empty transition id", obj.id);
            } else if (!transitionsById.emplace(id, &obj).second) {
                add(MapIssueSeverity::Error,
                    "Duplicate transition id \"" + id + "\" on \"" + obj.name + "\"", obj.id);
            }
        }
    }

    auto sectorExists = [&](const std::string& id) {
        return !id.empty() && sectorsById.find(id) != sectorsById.end();
    };
    auto portalExists = [&](const std::string& id) {
        return !id.empty() && portalsById.find(id) != portalsById.end();
    };

    std::unordered_set<std::string> referencedPortalIds;
    for (const SceneObject& obj : objects) {
        if (!obj.hasMapTransition) continue;
        const MapTransitionComponent& t = obj.mapTransition;
        if (t.sourceSectorId.empty()) {
            add(MapIssueSeverity::Error, "Transition \"" + obj.name + "\" has no source sector", obj.id);
        } else if (!sectorExists(t.sourceSectorId)) {
            add(MapIssueSeverity::Error,
                "Transition \"" + obj.name + "\" references missing source sector \"" + t.sourceSectorId + "\"",
                obj.id);
        }
        if (t.destinationSectorId.empty()) {
            add(MapIssueSeverity::Error, "Transition \"" + obj.name + "\" has no destination sector", obj.id);
        } else if (!sectorExists(t.destinationSectorId)) {
            add(MapIssueSeverity::Error,
                "Transition \"" + obj.name + "\" references missing destination sector \"" +
                    t.destinationSectorId + "\"",
                obj.id);
        }
        if (!t.sourcePortalId.empty() && !portalExists(t.sourcePortalId)) {
            add(MapIssueSeverity::Warning,
                "Transition \"" + obj.name + "\" references missing source portal \"" + t.sourcePortalId + "\"",
                obj.id);
        }
        if (!t.destinationPortalId.empty() && !portalExists(t.destinationPortalId)) {
            add(MapIssueSeverity::Warning,
                "Transition \"" + obj.name + "\" references missing destination portal \"" +
                    t.destinationPortalId + "\"",
                obj.id);
        }
        if (!t.sourcePortalId.empty()) referencedPortalIds.insert(t.sourcePortalId);
        if (!t.destinationPortalId.empty()) referencedPortalIds.insert(t.destinationPortalId);
        if (!t.bidirectional) {
            add(MapIssueSeverity::Info,
                "Transition \"" + obj.name + "\" is one-way (check this is intentional)", obj.id);
        }
    }

    for (const SceneObject& obj : objects) {
        if (!obj.hasMapPortal) continue;
        const MapPortalComponent& p = obj.mapPortal;
        if (p.transitionId.empty()) {
            add(MapIssueSeverity::Warning, "Portal \"" + obj.name + "\" is not linked to any transition", obj.id);
        } else if (transitionsById.find(p.transitionId) == transitionsById.end()) {
            add(MapIssueSeverity::Warning,
                "Portal \"" + obj.name + "\" references missing transition \"" + p.transitionId + "\"", obj.id);
        }
        if (!p.sectorId.empty() && !sectorExists(p.sectorId)) {
            add(MapIssueSeverity::Warning,
                "Portal \"" + obj.name + "\" is assigned to missing sector \"" + p.sectorId + "\"", obj.id);
        }
    }

    if (mapRoot != nullptr && !sectorsById.empty()) {
        std::string startId = mapRoot->mapRoot.startSectorId;
        if (startId.empty() || sectorsById.find(startId) == sectorsById.end()) {
            if (!startId.empty()) {
                add(MapIssueSeverity::Warning,
                    "Map Root start sector \"" + startId + "\" does not exist", mapRoot->id);
            }
            startId = sectorsById.begin()->first;
        }
        std::unordered_set<std::string> reachable;
        std::deque<std::string> queue;
        reachable.insert(startId);
        queue.push_back(startId);
        while (!queue.empty()) {
            const std::string current = queue.front();
            queue.pop_front();
            for (const SceneObject& obj : objects) {
                if (!obj.hasMapTransition || !obj.mapTransition.enabled) continue;
                const MapTransitionComponent& t = obj.mapTransition;
                std::string next;
                if (t.sourceSectorId == current) {
                    next = t.destinationSectorId;
                } else if (t.destinationSectorId == current && t.bidirectional) {
                    next = t.sourceSectorId;
                }
                if (!next.empty() && sectorExists(next) && reachable.insert(next).second) {
                    queue.push_back(next);
                }
            }
        }
        for (const auto& entry : sectorsById) {
            if (reachable.find(entry.first) == reachable.end()) {
                add(MapIssueSeverity::Warning,
                    "Sector \"" + entry.second->name + "\" is unreachable from the start sector",
                    entry.second->id);
            }
        }
    }

    for (const auto& entry : portalsById) {
        if (referencedPortalIds.find(entry.first) == referencedPortalIds.end() &&
            entry.second->mapPortal.transitionId.empty()) {
            add(MapIssueSeverity::Info,
                "Portal \"" + entry.second->name + "\" is not referenced by any transition",
                entry.second->id);
        }
    }

    return issues;
}

std::vector<std::string> ValidateMeshAsset(const RawMeshAsset& mesh) {
    std::vector<std::string> problems;
    const size_t vertexCount = mesh.positions.size();

    size_t nonFinite = 0;
    for (const glm::vec3& p : mesh.positions) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) ++nonFinite;
    }
    if (nonFinite > 0) {
        problems.push_back(std::to_string(nonFinite) + " vertex position(s) are non-finite (NaN/Inf)");
    }

    if (mesh.hasNormals && mesh.normals.size() != vertexCount) {
        problems.push_back("Normal count (" + std::to_string(mesh.normals.size()) +
                           ") does not match vertex count (" + std::to_string(vertexCount) + ")");
    }
    if (mesh.hasUVs && mesh.uvs.size() != vertexCount) {
        problems.push_back("UV count (" + std::to_string(mesh.uvs.size()) +
                           ") does not match vertex count (" + std::to_string(vertexCount) + ")");
    }
    if (mesh.hasNormals) {
        size_t badNormals = 0;
        for (const glm::vec3& n : mesh.normals) {
            if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z)) {
                ++badNormals;
            }
        }
        if (badNormals > 0) {
            problems.push_back(std::to_string(badNormals) + " normal(s) are non-finite");
        }
    }

    size_t outOfRange = 0;
    size_t degenerateIndex = 0;
    size_t zeroArea = 0;
    for (const glm::u32vec3& face : mesh.faces) {
        if (face.x >= vertexCount || face.y >= vertexCount || face.z >= vertexCount) {
            ++outOfRange;
            continue;
        }
        if (face.x == face.y || face.y == face.z || face.x == face.z) {
            ++degenerateIndex;
            continue;
        }
        const glm::vec3 cross = glm::cross(mesh.positions[face.y] - mesh.positions[face.x],
                                           mesh.positions[face.z] - mesh.positions[face.x]);
        if (!std::isfinite(cross.x) || !std::isfinite(cross.y) || !std::isfinite(cross.z) ||
            glm::length(cross) < 1e-10f) {
            ++zeroArea;
        }
    }
    if (outOfRange > 0) {
        problems.push_back(std::to_string(outOfRange) + " face(s) reference out-of-range vertex indices");
    }
    if (degenerateIndex > 0) {
        problems.push_back(std::to_string(degenerateIndex) + " face(s) reuse the same vertex twice (degenerate)");
    }
    if (zeroArea > 0) {
        problems.push_back(std::to_string(zeroArea) + " face(s) have zero area");
    }
    if (!mesh.faceMaterialIndices.empty() && mesh.faceMaterialIndices.size() != mesh.faces.size()) {
        problems.push_back("Face material index count (" + std::to_string(mesh.faceMaterialIndices.size()) +
                           ") does not match face count (" + std::to_string(mesh.faces.size()) + ")");
    }
    return problems;
}
#pragma endregion

} // namespace MapMaker
