#include "Engine.h"
#include "MapCarve.h"
#include "MapMaker.h"
#include "ModelLoader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace {

constexpr float kRoomWallThickness = 0.2f;

void AppendRoomQuad(RawMeshAsset& mesh, const glm::vec3& a, const glm::vec3& b,
                    const glm::vec3& c, const glm::vec3& d, const glm::vec3& normal,
                    float uvScale = 0.5f) {
    const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
    const glm::vec3 corners[4] = { a, b, c, d };
    glm::vec3 axisU = b - a;
    glm::vec3 axisV = d - a;
    const float lenU = glm::length(axisU);
    const float lenV = glm::length(axisV);
    axisU = lenU > 1e-6f ? axisU / lenU : glm::vec3(1.0f, 0.0f, 0.0f);
    axisV = lenV > 1e-6f ? axisV / lenV : glm::vec3(0.0f, 1.0f, 0.0f);
    for (int i = 0; i < 4; ++i) {
        mesh.positions.push_back(corners[i]);
        mesh.normals.push_back(normal);
        const glm::vec3 rel = corners[i] - a;
        mesh.uvs.push_back(glm::vec2(glm::dot(rel, axisU) * uvScale,
                                     glm::dot(rel, axisV) * uvScale));
    }
    mesh.faces.push_back(glm::u32vec3(base, base + 1, base + 2));
    mesh.faces.push_back(glm::u32vec3(base, base + 2, base + 3));
    mesh.faceMaterialIndices.push_back(0u);
    mesh.faceMaterialIndices.push_back(0u);
}

void AppendRoomBox(RawMeshAsset& mesh, const glm::vec3& mn, const glm::vec3& mx,
                   bool flipInward) {
    struct QuadDef {
        glm::vec3 a, b, c, d, n;
    };
    const QuadDef quads[6] = {
        { { mx.x, mn.y, mx.z }, { mx.x, mn.y, mn.z }, { mx.x, mx.y, mn.z }, { mx.x, mx.y, mx.z }, { 1, 0, 0 } },
        { { mn.x, mn.y, mn.z }, { mn.x, mn.y, mx.z }, { mn.x, mx.y, mx.z }, { mn.x, mx.y, mn.z }, { -1, 0, 0 } },
        { { mn.x, mx.y, mx.z }, { mx.x, mx.y, mx.z }, { mx.x, mx.y, mn.z }, { mn.x, mx.y, mn.z }, { 0, 1, 0 } },
        { { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z }, { mx.x, mn.y, mx.z }, { mn.x, mn.y, mx.z }, { 0, -1, 0 } },
        { { mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z }, { mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z }, { 0, 0, 1 } },
        { { mx.x, mn.y, mn.z }, { mn.x, mn.y, mn.z }, { mn.x, mx.y, mn.z }, { mx.x, mx.y, mn.z }, { 0, 0, -1 } },
    };
    for (const QuadDef& q : quads) {
        if (flipInward) {
            AppendRoomQuad(mesh, q.b, q.a, q.d, q.c, -q.n);
        } else {
            AppendRoomQuad(mesh, q.a, q.b, q.c, q.d, q.n);
        }
    }
}

RawMeshAsset BuildRoomShellRMesh(const glm::vec3& innerSize, bool withStairs) {
    RawMeshAsset mesh;
    const float t = kRoomWallThickness;
    const glm::vec3 innerMin(-innerSize.x * 0.5f, 0.0f, -innerSize.z * 0.5f);
    const glm::vec3 innerMax(innerSize.x * 0.5f, innerSize.y, innerSize.z * 0.5f);
    AppendRoomBox(mesh, innerMin, innerMax, true);
    AppendRoomBox(mesh, innerMin - glm::vec3(t), innerMax + glm::vec3(t), false);
    if (withStairs) {
        const int stepCount = 6;
        const float stepHeight = std::min(0.4f, innerSize.y * 0.6f / stepCount);
        const float stepDepth = innerSize.z * 0.7f / stepCount;
        const float stairWidth = std::min(1.6f, innerSize.x * 0.45f);
        for (int i = 0; i < stepCount; ++i) {
            const glm::vec3 mn(innerMax.x - stairWidth, 0.0f,
                               innerMin.z + 0.4f + i * stepDepth);
            const glm::vec3 mx(innerMax.x, (i + 1) * stepHeight, mn.z + stepDepth);
            AppendRoomBox(mesh, mn, mx, false);
        }
    }
    mesh.materialSlots.push_back("Default");
    mesh.hasNormals = true;
    mesh.hasUVs = true;
    mesh.boundsMin = glm::vec3(FLT_MAX);
    mesh.boundsMax = glm::vec3(-FLT_MAX);
    for (const glm::vec3& p : mesh.positions) {
        mesh.boundsMin = glm::min(mesh.boundsMin, p);
        mesh.boundsMax = glm::max(mesh.boundsMax, p);
    }
    return mesh;
}

const char* MapRoomPresetName(int preset) {
    switch (preset) {
        case 0: return "Small Room";
        case 1: return "Medium Room";
        case 2: return "Large Room";
        case 3: return "Corridor";
        case 4: return "Empty Sector";
        case 5: return "Vertical Shaft";
        case 6: return "Stair Room";
    }
    return "Room";
}

} // namespace

#pragma region Room Generation
bool Engine::mapRoomPresetDimensions(int preset, glm::vec3& outInnerSize) const {
    switch (preset) {
        case 0: outInnerSize = glm::vec3(4.0f, 2.6f, 4.0f); return true;
        case 1: outInnerSize = glm::vec3(8.0f, 3.0f, 8.0f); return true;
        case 2: outInnerSize = glm::vec3(14.0f, 4.5f, 14.0f); return true;
        case 3: outInnerSize = glm::vec3(2.4f, 2.6f, 8.0f); return true;
        case 4: return false;
        case 5: outInnerSize = glm::vec3(2.4f, 9.0f, 2.4f); return true;
        case 6: outInnerSize = glm::vec3(6.0f, 4.5f, 8.0f); return true;
    }
    return false;
}

SceneObject* Engine::createRoomMeshObject(RawMeshAsset&& roomMesh, const std::string& name,
                                          const glm::vec3& position, float yawDeg,
                                          std::string& error) {
    if (!projectManager.currentProject.isLoaded) {
        error = "No project loaded";
        return nullptr;
    }
    fs::path root = projectManager.currentProject.assetsPath / "Models" / "RMeshes" / "MapRooms";
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        error = "Failed to create room mesh folder: " + root.string();
        return nullptr;
    }
    std::string safeName = name;
    for (char& c : safeName) {
        if (c == '/' || c == '\\' || c == ':') c = '_';
    }
    fs::path filePath = root / (safeName + ".rmesh");
    int suffix = 1;
    while (fs::exists(filePath)) {
        filePath = root / (safeName + "_" + std::to_string(suffix++) + ".rmesh");
    }
    if (!getModelLoader().saveRawMesh(roomMesh, filePath.string(), error)) {
        return nullptr;
    }
    fileBrowser.needsRefresh = true;

    ModelLoadResult loaded = getModelLoader().loadModel(filePath.string());
    const int id = nextObjectId++;
    SceneObject obj(name, ObjectType::Empty, id);
    obj.hasRenderer = true;
    obj.renderType = RenderType::Model;
    obj.type = ObjectType::Model;
    obj.meshPath = filePath.string();
    obj.meshId = loaded.success ? loaded.meshIndex : -1;
    obj.position = position;
    obj.rotation = glm::vec3(0.0f, yawDeg, 0.0f);
    obj.hasMapMesh = true;
    obj.mapMesh = MapMeshComponent{};
    obj.hasCollider = true;
    obj.collider.enabled = true;
    obj.collider.type = ColliderType::Mesh;
    obj.collider.convex = false;
    sceneObjects.push_back(obj);
    SceneObject* created = &sceneObjects.back();
    syncLocalTransform(*created);
    markRuntimeScriptBindingsDirty();
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    return findObjectById(id);
}

SceneObject* Engine::createMapRoomSectorAt(int preset, const glm::vec3& floorPosition,
                                           float yawDeg, const glm::vec2& graphPos,
                                           bool recordUndo) {
    if (recordUndo) {
        recordState("createMapRoomSector");
    }
    int sectorNumber = 1;
    for (const SceneObject& candidate : sceneObjects) {
        if (candidate.hasMapSector) ++sectorNumber;
    }
    const std::string sectorName =
        std::string(MapRoomPresetName(preset)) + " " + std::to_string(sectorNumber);
    SceneObject* sector = createMapSectorObject(sectorName, graphPos, false);
    if (sector == nullptr) return nullptr;
    const int sectorId = sector->id;
    sector->position = floorPosition;
    syncLocalTransform(*sector);

    glm::vec3 innerSize(0.0f);
    if (mapRoomPresetDimensions(preset, innerSize)) {
        RawMeshAsset roomMesh = BuildRoomShellRMesh(innerSize, preset == 6);
        std::string error;
        SceneObject* room = createRoomMeshObject(std::move(roomMesh), sectorName + " Shell",
                                                 floorPosition, yawDeg, error);
        sector = findObjectById(sectorId);
        if (room == nullptr) {
            addConsoleMessage("Room shell generation failed: " + error,
                              ConsoleMessageType::Error);
        } else if (sector != nullptr) {
            room->parentId = sector->id;
            sector->childIds.push_back(room->id);
            if (sector->hasMapSector) {
                sector->mapSector.useCustomBounds = true;
                sector->mapSector.boundsCenter = glm::vec3(0.0f, innerSize.y * 0.5f, 0.0f);
                sector->mapSector.boundsSize = innerSize + glm::vec3(kRoomWallThickness * 2.0f);
            }
        }
    }
    updateHierarchyWorldTransforms();
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    return findObjectById(sectorId);
}
#pragma endregion

#pragma region Sector Visibility
void Engine::applyMapSectorVisibility() {
    SceneObject* mapRootObj = MapMaker::FindMapRoot(sceneObjects);
    const int mode =
        mapRootObj != nullptr ? std::clamp(mapRootObj->mapRoot.sectorVisibilityMode, 0, 2) : 0;
    const std::string activeSectorId =
        mapRootObj != nullptr ? mapRootObj->mapRoot.activeSectorId : std::string();
    const bool filterActive = !isPlaying && mapRootObj != nullptr && mode != 0 &&
                              MapMaker::FindSectorById(sceneObjects, activeSectorId) != nullptr;
    if (!filterActive) {
        for (SceneObject& obj : sceneObjects) {
            obj.editorSectorHidden = false;
        }
        return;
    }
    std::unordered_set<std::string> visibleSectorIds;
    visibleSectorIds.insert(activeSectorId);
    if (mode == 1) {
        for (const std::string& adjacent :
             MapMaker::GatherAdjacentSectorIds(sceneObjects, activeSectorId)) {
            visibleSectorIds.insert(adjacent);
        }
    }
    std::unordered_map<int, const SceneObject*> byId;
    byId.reserve(sceneObjects.size());
    for (const SceneObject& obj : sceneObjects) {
        byId[obj.id] = &obj;
    }
    for (SceneObject& obj : sceneObjects) {
        const SceneObject* current = &obj;
        const SceneObject* owningSector = nullptr;
        for (size_t depth = 0; current != nullptr && depth <= sceneObjects.size(); ++depth) {
            if (current->hasMapSector) {
                owningSector = current;
                break;
            }
            if (current->parentId < 0) break;
            auto it = byId.find(current->parentId);
            current = it != byId.end() ? it->second : nullptr;
        }
        obj.editorSectorHidden =
            owningSector != nullptr &&
            visibleSectorIds.count(owningSector->mapSector.sectorId) == 0;
    }
}
#pragma endregion

#pragma region Carve Connect Workflow
void Engine::createConnectedSectorThroughCarve(SceneObject* wallObject,
                                               const MapCarve::CarveOutcome& carve,
                                               const MapCarve::PlaneBasis& basisLocal,
                                               const glm::mat4& wallModelMatrix) {
    if (wallObject == nullptr || carve.backRimVerts.empty() || !meshEditLoaded) {
        addConsoleMessage("Connect Sector: carve did not produce a through opening",
                          ConsoleMessageType::Warning);
        return;
    }
    for (uint32_t v : carve.backRimVerts) {
        if (v >= meshEditAsset.positions.size()) {
            addConsoleMessage("Connect Sector: carve rim is out of range",
                              ConsoleMessageType::Warning);
            return;
        }
    }
    recordState("carveConnectSector");
    const int wallObjectId = wallObject->id;

    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(wallModelMatrix)));
    glm::vec3 intoDir = -glm::normalize(normalMatrix * basisLocal.normal);
    glm::vec3 axisUWorld = glm::normalize(glm::mat3(wallModelMatrix) * basisLocal.axisU);
    glm::vec3 axisVWorld = glm::normalize(glm::mat3(wallModelMatrix) * basisLocal.axisV);

    glm::vec3 backCenter(0.0f);
    for (uint32_t v : carve.backRimVerts) {
        backCenter += glm::vec3(wallModelMatrix * glm::vec4(meshEditAsset.positions[v], 1.0f));
    }
    backCenter /= static_cast<float>(carve.backRimVerts.size());
    glm::vec3 frontCenter = backCenter;
    if (!carve.frontRimVerts.empty()) {
        frontCenter = glm::vec3(0.0f);
        for (uint32_t v : carve.frontRimVerts) {
            if (v >= meshEditAsset.positions.size()) continue;
            frontCenter +=
                glm::vec3(wallModelMatrix * glm::vec4(meshEditAsset.positions[v], 1.0f));
        }
        frontCenter /= static_cast<float>(carve.frontRimVerts.size());
    }
    float doorWidth = 0.0f;
    float doorHeight = 0.0f;
    for (uint32_t v : carve.backRimVerts) {
        const glm::vec3 world =
            glm::vec3(wallModelMatrix * glm::vec4(meshEditAsset.positions[v], 1.0f));
        doorWidth = std::max(doorWidth, 2.0f * std::fabs(glm::dot(world - backCenter, axisUWorld)));
        doorHeight = std::max(doorHeight, 2.0f * std::fabs(glm::dot(world - backCenter, axisVWorld)));
    }
    doorWidth = std::max(doorWidth, 0.4f);
    doorHeight = std::max(doorHeight, 0.4f);
    const float doorFloorY = backCenter.y - doorHeight * 0.5f;

    const int preset = std::clamp(carveConnectedPreset, 0, 6);
    glm::vec3 innerSize(4.0f, 2.6f, 4.0f);
    const bool hasRoomMesh = mapRoomPresetDimensions(preset, innerSize);
    glm::vec3 flatInto = intoDir;
    flatInto.y = 0.0f;
    if (glm::length(flatInto) < 1e-4f) {
        flatInto = glm::vec3(0.0f, 0.0f, 1.0f);
    } else {
        flatInto = glm::normalize(flatInto);
    }
    const float yawDeg = glm::degrees(std::atan2(-flatInto.x, -flatInto.z));
    const glm::vec3 roomCenterFloor =
        glm::vec3(backCenter.x, doorFloorY, backCenter.z) +
        flatInto * (kRoomWallThickness + innerSize.z * 0.5f);

    const SceneObject* sourceSector = MapMaker::FindOwningSector(sceneObjects, *wallObject);
    const std::string sourceSectorId =
        sourceSector != nullptr ? sourceSector->mapSector.sectorId : std::string();
    const int sourceSectorObjectId = sourceSector != nullptr ? sourceSector->id : -1;
    glm::vec2 graphPos(0.0f);
    if (sourceSector != nullptr) {
        graphPos = sourceSector->mapSector.graphPosition +
                   glm::vec2(flatInto.x, flatInto.z) * 230.0f;
    }
    sourceSector = nullptr;
    wallObject = nullptr;

    SceneObject* newSector = createMapRoomSectorAt(preset, roomCenterFloor, yawDeg, graphPos,
                                                   /*recordUndo=*/false);
    if (newSector == nullptr) {
        addConsoleMessage("Connect Sector: failed to create the destination sector",
                          ConsoleMessageType::Error);
        return;
    }
    const int newSectorObjectId = newSector->id;
    const std::string newSectorId = newSector->mapSector.sectorId;

    if (hasRoomMesh) {
        SceneObject* room = nullptr;
        for (SceneObject& candidate : sceneObjects) {
            if (candidate.parentId == newSectorObjectId && candidate.hasMapMesh) {
                room = &candidate;
                break;
            }
        }
        if (room != nullptr && IsMeshEditablePath(room->meshPath)) {
            RawMeshAsset roomMesh;
            std::string loadError;
            if (getModelLoader().loadRawMesh(room->meshPath, roomMesh, loadError)) {
                glm::mat4 roomModel = glm::translate(glm::mat4(1.0f), room->position);
                roomModel = glm::rotate(roomModel, glm::radians(yawDeg), glm::vec3(0, 1, 0));
                const glm::vec3 doorLocal =
                    glm::vec3(glm::inverse(roomModel) * glm::vec4(backCenter, 1.0f));
                const float wallPlaneZ = innerSize.z * 0.5f + kRoomWallThickness;
                int doorFace = -1;
                for (size_t fi = 0; fi < roomMesh.faces.size(); ++fi) {
                    const glm::u32vec3& f = roomMesh.faces[fi];
                    const glm::vec3 n = glm::normalize(glm::cross(
                        roomMesh.positions[f.y] - roomMesh.positions[f.x],
                        roomMesh.positions[f.z] - roomMesh.positions[f.x]));
                    if (n.z > 0.99f &&
                        std::fabs(roomMesh.positions[f.x].z - wallPlaneZ) < 1e-3f) {
                        doorFace = static_cast<int>(fi);
                        break;
                    }
                }
                bool carvedRoom = false;
                if (doorFace >= 0) {
                    MapCarve::PlaneBasis roomBasis;
                    if (MapCarve::BuildPlaneBasisFromFace(roomMesh, doorFace, roomBasis)) {
                        MapCarve::CarveOptions roomOptions;
                        roomOptions.cutThrough = true;
                        roomOptions.maxThroughDistance = kRoomWallThickness * 6.0f;
                        MapCarve::CarveOutcome roomCarve = MapCarve::CarveShapeIntoMesh(
                            roomMesh, doorFace,
                            MapCarve::MakeRectShape(
                                roomBasis.toPlane(glm::vec3(doorLocal.x, doorLocal.y, wallPlaneZ)),
                                glm::vec2(doorWidth * 0.5f, doorHeight * 0.5f)),
                            roomOptions);
                        if (roomCarve.success) {
                            std::string saveError;
                            if (getModelLoader().saveRawMesh(roomMesh, room->meshPath, saveError)) {
                                std::string reloadError;
                                getModelLoader().updateRawMesh(room->meshId, roomMesh, reloadError);
                                carvedRoom = true;
                            } else {
                                addConsoleMessage("Connect Sector: failed to save room mesh: " +
                                                      saveError,
                                                  ConsoleMessageType::Warning);
                            }
                        } else {
                            addConsoleMessage("Connect Sector: room opening carve failed: " +
                                                  roomCarve.error,
                                              ConsoleMessageType::Warning);
                        }
                    }
                }
                if (!carvedRoom) {
                    addConsoleMessage(
                        "Connect Sector: the room was created but its doorway was not carved; "
                        "carve it manually",
                        ConsoleMessageType::Warning);
                }
            }
        }
    }

    auto createPortal = [&](const std::string& name, const glm::vec3& position,
                            const std::string& owningSectorId, int parentId) -> std::string {
        const int id = nextObjectId++;
        SceneObject portal(name, ObjectType::Empty, id);
        portal.hasMapPortal = true;
        portal.mapPortal.portalId = MapMaker::GenerateId("prt");
        portal.mapPortal.sectorId = owningSectorId;
        portal.mapPortal.openingSize = glm::vec2(doorWidth, doorHeight);
        portal.position = position;
        portal.rotation = glm::vec3(0.0f, yawDeg, 0.0f);
        portal.parentId = parentId;
        const std::string portalId = portal.mapPortal.portalId;
        sceneObjects.push_back(portal);
        if (SceneObject* parent = findObjectById(parentId)) {
            parent->childIds.push_back(id);
        }
        if (SceneObject* created = findObjectById(id)) {
            syncLocalTransform(*created);
        }
        return portalId;
    };
    const int sourceParentId =
        sourceSectorObjectId >= 0 ? sourceSectorObjectId : wallObjectId;
    const std::string sourcePortalId =
        createPortal("Doorway (source)", frontCenter, sourceSectorId, sourceParentId);
    const std::string destPortalId = createPortal(
        "Doorway (destination)", backCenter + flatInto * kRoomWallThickness, newSectorId,
        newSectorObjectId);

    if (!sourceSectorId.empty()) {
        SceneObject* transition =
            createMapTransitionObject(sourceSectorId, newSectorId, /*recordUndo=*/false);
        if (transition != nullptr) {
            transition->mapTransition.sourcePortalId = sourcePortalId;
            transition->mapTransition.destinationPortalId = destPortalId;
            transition->mapTransition.kind = MapTransitionKind::Door;
            transition->mapTransition.hasEntryTransform = true;
            transition->mapTransition.entryPosition =
                backCenter + flatInto * (kRoomWallThickness + 0.8f);
            transition->mapTransition.entryYawDeg = yawDeg;
            if (SceneObject* portal = MapMaker::FindPortalById(sceneObjects, sourcePortalId)) {
                portal->mapPortal.transitionId = transition->mapTransition.transitionId;
            }
            if (SceneObject* portal = MapMaker::FindPortalById(sceneObjects, destPortalId)) {
                portal->mapPortal.transitionId = transition->mapTransition.transitionId;
            }
        }
    } else {
        addConsoleMessage(
            "Connect Sector: the carved wall is not inside a sector, so no transition was "
            "created. Add a Sector component to the source room's root object.",
            ConsoleMessageType::Warning);
    }

    updateHierarchyWorldTransforms();
    markRuntimeScriptBindingsDirty();
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    addConsoleMessage("Created connected sector through the carved doorway",
                      ConsoleMessageType::Success);
    showSectorMapWindow = true;
    focusMapSector(newSectorObjectId);
}
#pragma endregion
