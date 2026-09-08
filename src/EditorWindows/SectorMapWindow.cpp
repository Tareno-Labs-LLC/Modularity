#include "../EditorLocalization.h"
#include "Engine.h"
#include "MapMaker.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace Loc = Modularity::Loc;

namespace {

constexpr float kNodeWidth = 156.0f;
constexpr float kNodeHeight = 56.0f;
constexpr float kMinZoom = 0.25f;
constexpr float kMaxZoom = 2.5f;

struct SectorMapViewState {
    ImVec2 pan = ImVec2(-200.0f, -150.0f);
    float zoom = 1.0f;
    bool viewInitialized = false;

    bool draggingNodes = false;
    bool dragUndoRecorded = false;

    bool panning = false;

    bool linkDragActive = false;
    int linkSourceObjectId = -1;

    bool boxSelectActive = false;
    ImVec2 boxSelectStartScreen = ImVec2(0.0f, 0.0f);

    int selectedTransitionObjectId = -1;

    char searchBuffer[96] = "";

    bool validationOpen = false;
    std::vector<MapMaker::MapValidationIssue> validationIssues;

    bool renamePopupOpen = false;
    int renameObjectId = -1;
    char renameBuffer[128] = "";
    bool deletePopupOpen = false;
    int deleteObjectId = -1;
};

SectorMapViewState g_state;

ImVec2 GraphToScreen(const ImVec2& canvasMin, const ImVec2& graphPos) {
    return ImVec2(canvasMin.x + (graphPos.x - g_state.pan.x) * g_state.zoom,
                  canvasMin.y + (graphPos.y - g_state.pan.y) * g_state.zoom);
}

ImVec2 ScreenToGraph(const ImVec2& canvasMin, const ImVec2& screenPos) {
    return ImVec2((screenPos.x - canvasMin.x) / g_state.zoom + g_state.pan.x,
                  (screenPos.y - canvasMin.y) / g_state.zoom + g_state.pan.y);
}

bool PointInRect(const ImVec2& p, const ImVec2& min, const ImVec2& max) {
    return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
}

float DistancePointSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    const ImVec2 ab(b.x - a.x, b.y - a.y);
    const float lenSq = ab.x * ab.x + ab.y * ab.y;
    if (lenSq < 1e-6f) {
        const ImVec2 d(p.x - a.x, p.y - a.y);
        return std::sqrt(d.x * d.x + d.y * d.y);
    }
    float t = ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / lenSq;
    t = std::clamp(t, 0.0f, 1.0f);
    const ImVec2 proj(a.x + ab.x * t, a.y + ab.y * t);
    const ImVec2 d(p.x - proj.x, p.y - proj.y);
    return std::sqrt(d.x * d.x + d.y * d.y);
}

ImVec2 EdgeBezierPoint(const ImVec2& a, const ImVec2& b, float t) {
    const float tangent = std::max(30.0f * g_state.zoom,
                                   std::fabs(b.x - a.x) * 0.4f);
    const ImVec2 c1(a.x + tangent, a.y);
    const ImVec2 c2(b.x - tangent, b.y);
    const float u = 1.0f - t;
    ImVec2 p;
    p.x = u * u * u * a.x + 3.0f * u * u * t * c1.x + 3.0f * u * t * t * c2.x + t * t * t * b.x;
    p.y = u * u * u * a.y + 3.0f * u * u * t * c1.y + 3.0f * u * t * t * c2.y + t * t * t * b.y;
    return p;
}

float DistanceToEdge(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    float best = FLT_MAX;
    ImVec2 prev = a;
    for (int i = 1; i <= 20; ++i) {
        const ImVec2 cur = EdgeBezierPoint(a, b, static_cast<float>(i) / 20.0f);
        best = std::min(best, DistancePointSegment(p, prev, cur));
        prev = cur;
    }
    return best;
}

bool NameMatchesFilter(const std::string& name, const char* filter) {
    if (filter == nullptr || filter[0] == '\0') return true;
    std::string lowerName = name;
    std::string lowerFilter = filter;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowerName.find(lowerFilter) != std::string::npos;
}

} // namespace

#pragma region Map Creation Helpers
SceneObject* Engine::ensureMapRootObject(bool recordUndo) {
    SceneObject* existing = MapMaker::FindMapRoot(sceneObjects);
    if (existing != nullptr) return existing;
    if (recordUndo) {
        recordState("createMapRoot");
    }
    const int id = nextObjectId++;
    SceneObject obj("Map Root", ObjectType::Empty, id);
    obj.hasMapRoot = true;
    obj.mapRoot.mapId = MapMaker::GenerateId("map");
    sceneObjects.push_back(obj);
    markRuntimeScriptBindingsDirty();
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    logToConsole("Created: Map Root");
    return &sceneObjects.back();
}

SceneObject* Engine::createMapSectorObject(const std::string& name, const glm::vec2& graphPos,
                                           bool recordUndo) {
    if (recordUndo) {
        recordState("createMapSector");
    }
    SceneObject* mapRootObj = ensureMapRootObject(false);
    const int mapRootId = mapRootObj != nullptr ? mapRootObj->id : -1;

    const int id = nextObjectId++;
    SceneObject obj(name, ObjectType::Empty, id);
    obj.hasMapSector = true;
    obj.mapSector.sectorId = MapMaker::GenerateId("sec");
    obj.mapSector.graphPosition = graphPos;
    obj.parentId = mapRootId;
    sceneObjects.push_back(obj);
    SceneObject* created = &sceneObjects.back();
    if (mapRootId >= 0) {
        if (SceneObject* parent = findObjectById(mapRootId)) {
            parent->childIds.push_back(id);
        }
    }
    if (SceneObject* root = MapMaker::FindMapRoot(sceneObjects)) {
        if (root->mapRoot.startSectorId.empty()) {
            root->mapRoot.startSectorId = created->mapSector.sectorId;
        }
    }
    created = findObjectById(id);
    updateHierarchyWorldTransforms();
    markRuntimeScriptBindingsDirty();
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    logToConsole("Created sector: " + name);
    return created;
}

SceneObject* Engine::createMapTransitionObject(const std::string& sourceSectorId,
                                               const std::string& destinationSectorId,
                                               bool recordUndo) {
    SceneObject* source = MapMaker::FindSectorById(sceneObjects, sourceSectorId);
    SceneObject* destination = MapMaker::FindSectorById(sceneObjects, destinationSectorId);
    if (source == nullptr || destination == nullptr || sourceSectorId == destinationSectorId) {
        return nullptr;
    }
    for (SceneObject& obj : sceneObjects) {
        if (!obj.hasMapTransition) continue;
        const MapTransitionComponent& t = obj.mapTransition;
        if ((t.sourceSectorId == sourceSectorId && t.destinationSectorId == destinationSectorId) ||
            (t.sourceSectorId == destinationSectorId && t.destinationSectorId == sourceSectorId)) {
            addConsoleMessage("Sectors are already connected: " + obj.name, ConsoleMessageType::Info);
            return &obj;
        }
    }
    if (recordUndo) {
        recordState("createMapTransition");
    }
    SceneObject* mapRootObj = ensureMapRootObject(false);
    const int mapRootId = mapRootObj != nullptr ? mapRootObj->id : -1;
    const std::string sourceName = source->name;
    const std::string destinationName = destination->name;
    const glm::vec3 midpoint = (source->position + destination->position) * 0.5f;

    const int id = nextObjectId++;
    SceneObject obj("Transition " + sourceName + " - " + destinationName, ObjectType::Empty, id);
    obj.hasMapTransition = true;
    obj.mapTransition.transitionId = MapMaker::GenerateId("trn");
    obj.mapTransition.sourceSectorId = sourceSectorId;
    obj.mapTransition.destinationSectorId = destinationSectorId;
    obj.position = midpoint;
    obj.parentId = mapRootId;
    sceneObjects.push_back(obj);
    if (mapRootId >= 0) {
        if (SceneObject* parent = findObjectById(mapRootId)) {
            parent->childIds.push_back(id);
        }
    }
    SceneObject* created = findObjectById(id);
    updateHierarchyWorldTransforms();
    markRuntimeScriptBindingsDirty();
    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
    logToConsole("Connected sectors: " + sourceName + " <-> " + destinationName);
    return created;
}

void Engine::focusMapSector(int sectorObjectId) {
    SceneObject* sector = findObjectById(sectorObjectId);
    if (sector == nullptr || !sector->hasMapSector) return;
    setPrimarySelection(sectorObjectId, false);
    focusViewportOnSelection();
    if (SceneObject* root = MapMaker::FindMapRoot(sceneObjects)) {
        root->mapRoot.activeSectorId = sector->mapSector.sectorId;
    }
    applyMapSectorVisibility();
}
#pragma endregion

#pragma region Sector Map Window
void Engine::renderSectorMapWindow() {
    if (!showSectorMapWindow) return;

    ImGui::SetNextWindowSize(ImVec2(820.0f, 540.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Loc::Window("WINDOW_SECTOR_MAP", "Sector Map"), &showSectorMapWindow)) {
        ImGui::End();
        return;
    }

    if (!projectManager.currentProject.isLoaded) {
        ImGui::TextDisabled("Load a project to edit its sector map.");
        ImGui::End();
        return;
    }

    SceneObject* mapRootObj = MapMaker::FindMapRoot(sceneObjects);

    if (mapRootObj == nullptr) {
        ImGui::TextWrapped("This scene has no Map Root yet. The Map Root owns the sector graph "
                           "and map-wide settings.");
        if (ImGui::Button("Create Map Root")) {
            ensureMapRootObject(true);
        }
        ImGui::End();
        return;
    }

    bool wantFrameAll = false;
    bool wantFrameSelection = false;
    if (ImGui::Button("Create Sector")) {
        int sectorNumber = 1;
        for (const SceneObject& candidate : sceneObjects) {
            if (candidate.hasMapSector) ++sectorNumber;
        }
        const ImVec2 viewCenterGraph(g_state.pan.x + (ImGui::GetContentRegionAvail().x * 0.5f) / g_state.zoom,
                                     g_state.pan.y + 200.0f / g_state.zoom);
        createMapSectorObject("Sector " + std::to_string(sectorNumber),
                              glm::vec2(viewCenterGraph.x, viewCenterGraph.y), true);
        mapRootObj = MapMaker::FindMapRoot(sceneObjects);
        if (mapRootObj == nullptr) {
            ImGui::End();
            return;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a new empty sector node");
    ImGui::SameLine();
    if (ImGui::Button("Frame All")) wantFrameAll = true;
    ImGui::SameLine();
    if (ImGui::Button("Frame Selection")) wantFrameSelection = true;
    ImGui::SameLine();
    if (ImGui::Button("Auto Layout")) {
        recordState("sectorMapAutoLayout");
        std::vector<SceneObject*> sectors = MapMaker::GatherSectors(sceneObjects);
        std::unordered_map<std::string, int> depth;
        std::deque<std::string> queue;
        std::string startId = mapRootObj->mapRoot.startSectorId;
        if (MapMaker::FindSectorById(sceneObjects, startId) == nullptr && !sectors.empty()) {
            startId = sectors.front()->mapSector.sectorId;
        }
        if (!startId.empty()) {
            depth[startId] = 0;
            queue.push_back(startId);
            while (!queue.empty()) {
                const std::string current = queue.front();
                queue.pop_front();
                for (const std::string& next : MapMaker::GatherAdjacentSectorIds(sceneObjects, current)) {
                    if (depth.find(next) == depth.end()) {
                        depth[next] = depth[current] + 1;
                        queue.push_back(next);
                    }
                }
            }
        }
        std::unordered_map<int, int> rowsPerColumn;
        int orphanRow = 0;
        for (SceneObject* sector : sectors) {
            auto it = depth.find(sector->mapSector.sectorId);
            if (it != depth.end()) {
                const int column = it->second;
                const int row = rowsPerColumn[column]++;
                sector->mapSector.graphPosition = glm::vec2(column * (kNodeWidth + 70.0f),
                                                            row * (kNodeHeight + 46.0f));
            } else {
                sector->mapSector.graphPosition = glm::vec2(orphanRow * (kNodeWidth + 70.0f), 420.0f);
                ++orphanRow;
            }
        }
        projectManager.currentProject.hasUnsavedChanges = true;
        wantFrameAll = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Arrange sectors by distance from the start sector");
    ImGui::SameLine();
    if (ImGui::Button("Validate")) {
        g_state.validationIssues = MapMaker::ValidateMap(sceneObjects);
        g_state.validationOpen = true;
        if (g_state.validationIssues.empty()) {
            addConsoleMessage("Map validation: no problems found", ConsoleMessageType::Success);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Check ids, references, portals, and reachability");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    const bool searchSubmitted = ImGui::InputTextWithHint("##SectorSearch", "Search sectors...",
                                                          g_state.searchBuffer, sizeof(g_state.searchBuffer),
                                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    {
        static const char* kVisibilityModes[] = { "Show All", "Current + Adjacent", "Current Only" };
        int visibilityMode = std::clamp(mapRootObj->mapRoot.sectorVisibilityMode, 0, 2);
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::Combo("##SectorVisibility", &visibilityMode, kVisibilityModes, 3)) {
            mapRootObj->mapRoot.sectorVisibilityMode = visibilityMode;
            projectManager.currentProject.hasUnsavedChanges = true;
            applyMapSectorVisibility();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Editor viewport visibility for sector content");
        }
    }

    int brokenTransitions = 0;
    int firstBrokenId = -1;
    for (const SceneObject& obj : sceneObjects) {
        if (!obj.hasMapTransition) continue;
        if (MapMaker::FindSectorById(sceneObjects, obj.mapTransition.sourceSectorId) == nullptr ||
            MapMaker::FindSectorById(sceneObjects, obj.mapTransition.destinationSectorId) == nullptr) {
            ++brokenTransitions;
            if (firstBrokenId < 0) firstBrokenId = obj.id;
        }
    }
    if (brokenTransitions > 0) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.35f, 1.0f));
        char warnLabel[64];
        std::snprintf(warnLabel, sizeof(warnLabel), "%d broken##BrokenTransitions", brokenTransitions);
        if (ImGui::SmallButton(warnLabel)) {
            setPrimarySelection(firstBrokenId, false);
            g_state.selectedTransitionObjectId = firstBrokenId;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Transitions referencing missing sectors. Click to select the first one.");
        }
    }

    if (g_state.validationOpen && !g_state.validationIssues.empty()) {
        const float listHeight =
            std::min(120.0f, 8.0f + g_state.validationIssues.size() * ImGui::GetTextLineHeightWithSpacing());
        ImGui::BeginChild("##SectorMapValidation", ImVec2(0.0f, listHeight), true);
        for (size_t i = 0; i < g_state.validationIssues.size(); ++i) {
            const MapMaker::MapValidationIssue& issue = g_state.validationIssues[i];
            ImGui::PushID(static_cast<int>(i));
            const ImVec4 color =
                issue.severity == MapMaker::MapIssueSeverity::Error
                    ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                    : issue.severity == MapMaker::MapIssueSeverity::Warning
                          ? ImVec4(1.0f, 0.8f, 0.4f, 1.0f)
                          : ImVec4(0.7f, 0.75f, 0.8f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            if (ImGui::Selectable(issue.message.c_str(), false) && issue.objectId >= 0) {
                if (SceneObject* target = findObjectById(issue.objectId)) {
                    setPrimarySelection(target->id, false);
                    if (target->hasMapSector) {
                        focusMapSector(target->id);
                    } else {
                        focusViewportOnSelection();
                    }
                }
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (ImGui::SmallButton("Dismiss")) {
            g_state.validationOpen = false;
            g_state.validationIssues.clear();
        }
    }

    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.x = std::max(canvasSize.x, 64.0f);
    canvasSize.y = std::max(canvasSize.y, 64.0f);
    const ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 bg = style.Colors[ImGuiCol_ChildBg].w > 0.01f ? style.Colors[ImGuiCol_ChildBg]
                                                               : style.Colors[ImGuiCol_WindowBg];
    drawList->AddRectFilled(canvasMin, canvasMax,
                            ImGui::GetColorU32(ImVec4(bg.x * 0.82f, bg.y * 0.82f, bg.z * 0.86f, 1.0f)), 6.0f);
    drawList->PushClipRect(canvasMin, canvasMax, true);

    {
        const float step = 48.0f * g_state.zoom;
        const ImU32 gridColor = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.045f));
        if (step > 6.0f) {
            float x = canvasMin.x - std::fmod(g_state.pan.x * g_state.zoom, step);
            for (; x < canvasMax.x; x += step) {
                drawList->AddLine(ImVec2(x, canvasMin.y), ImVec2(x, canvasMax.y), gridColor);
            }
            float y = canvasMin.y - std::fmod(g_state.pan.y * g_state.zoom, step);
            for (; y < canvasMax.y; y += step) {
                drawList->AddLine(ImVec2(canvasMin.x, y), ImVec2(canvasMax.x, y), gridColor);
            }
        }
    }

    ImGui::InvisibleButton("##SectorMapCanvas", canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool canvasHovered = ImGui::IsItemHovered();
    const bool canvasActive = ImGui::IsItemActive();
    const ImVec2 mousePos = ImGui::GetIO().MousePos;

    std::vector<SceneObject*> sectors = MapMaker::GatherSectors(sceneObjects);
    std::vector<SceneObject*> transitions = MapMaker::GatherTransitions(sceneObjects);
    std::unordered_map<std::string, SceneObject*> sectorsById;
    for (SceneObject* sector : sectors) {
        sectorsById[sector->mapSector.sectorId] = sector;
    }
    std::unordered_set<std::string> connectedSectorIds;
    for (SceneObject* transition : transitions) {
        connectedSectorIds.insert(transition->mapTransition.sourceSectorId);
        connectedSectorIds.insert(transition->mapTransition.destinationSectorId);
    }

    if (!g_state.viewInitialized) {
        g_state.viewInitialized = true;
        wantFrameAll = true;
    }

    auto nodeRect = [&](const SceneObject& sector, ImVec2& outMin, ImVec2& outMax) {
        const ImVec2 pos = GraphToScreen(canvasMin, ImVec2(sector.mapSector.graphPosition.x,
                                                           sector.mapSector.graphPosition.y));
        outMin = pos;
        outMax = ImVec2(pos.x + kNodeWidth * g_state.zoom, pos.y + kNodeHeight * g_state.zoom);
    };
    auto nodePortOut = [&](const SceneObject& sector) {
        ImVec2 mn, mx;
        nodeRect(sector, mn, mx);
        return ImVec2(mx.x, (mn.y + mx.y) * 0.5f);
    };
    auto nodePortIn = [&](const SceneObject& sector) {
        ImVec2 mn, mx;
        nodeRect(sector, mn, mx);
        return ImVec2(mn.x, (mn.y + mx.y) * 0.5f);
    };

    auto frameGraphBounds = [&](const ImVec2& boundsMin, const ImVec2& boundsMax) {
        const float width = std::max(boundsMax.x - boundsMin.x, 1.0f);
        const float height = std::max(boundsMax.y - boundsMin.y, 1.0f);
        const float zoomX = canvasSize.x / (width + 160.0f);
        const float zoomY = canvasSize.y / (height + 160.0f);
        g_state.zoom = std::clamp(std::min(zoomX, zoomY), kMinZoom, kMaxZoom);
        g_state.pan.x = boundsMin.x - (canvasSize.x / g_state.zoom - width) * 0.5f;
        g_state.pan.y = boundsMin.y - (canvasSize.y / g_state.zoom - height) * 0.5f;
    };
    if (wantFrameAll && !sectors.empty()) {
        ImVec2 boundsMin(FLT_MAX, FLT_MAX);
        ImVec2 boundsMax(-FLT_MAX, -FLT_MAX);
        for (const SceneObject* sector : sectors) {
            boundsMin.x = std::min(boundsMin.x, sector->mapSector.graphPosition.x);
            boundsMin.y = std::min(boundsMin.y, sector->mapSector.graphPosition.y);
            boundsMax.x = std::max(boundsMax.x, sector->mapSector.graphPosition.x + kNodeWidth);
            boundsMax.y = std::max(boundsMax.y, sector->mapSector.graphPosition.y + kNodeHeight);
        }
        frameGraphBounds(boundsMin, boundsMax);
    }
    if (wantFrameSelection) {
        ImVec2 boundsMin(FLT_MAX, FLT_MAX);
        ImVec2 boundsMax(-FLT_MAX, -FLT_MAX);
        bool any = false;
        for (const SceneObject* sector : sectors) {
            const bool isSelected = std::find(selectedObjectIds.begin(), selectedObjectIds.end(),
                                              sector->id) != selectedObjectIds.end() ||
                                    sector->id == selectedObjectId;
            if (!isSelected) continue;
            any = true;
            boundsMin.x = std::min(boundsMin.x, sector->mapSector.graphPosition.x);
            boundsMin.y = std::min(boundsMin.y, sector->mapSector.graphPosition.y);
            boundsMax.x = std::max(boundsMax.x, sector->mapSector.graphPosition.x + kNodeWidth);
            boundsMax.y = std::max(boundsMax.y, sector->mapSector.graphPosition.y + kNodeHeight);
        }
        if (any) frameGraphBounds(boundsMin, boundsMax);
    }

    if (searchSubmitted && g_state.searchBuffer[0] != '\0') {
        for (SceneObject* sector : sectors) {
            if (NameMatchesFilter(sector->name, g_state.searchBuffer)) {
                setPrimarySelection(sector->id, false);
                frameGraphBounds(ImVec2(sector->mapSector.graphPosition.x, sector->mapSector.graphPosition.y),
                                 ImVec2(sector->mapSector.graphPosition.x + kNodeWidth,
                                        sector->mapSector.graphPosition.y + kNodeHeight));
                g_state.zoom = std::min(g_state.zoom, 1.0f);
                break;
            }
        }
    }

    SceneObject* hoveredNode = nullptr;
    bool hoveredPortOut = false;
    for (auto it = sectors.rbegin(); it != sectors.rend(); ++it) {
        ImVec2 mn, mx;
        nodeRect(**it, mn, mx);
        const float portRadius = std::max(6.0f, 7.0f * g_state.zoom);
        const ImVec2 portOut = nodePortOut(**it);
        const ImVec2 dOut(mousePos.x - portOut.x, mousePos.y - portOut.y);
        if (dOut.x * dOut.x + dOut.y * dOut.y <= portRadius * portRadius * 2.25f) {
            hoveredNode = *it;
            hoveredPortOut = true;
            break;
        }
        if (PointInRect(mousePos, mn, mx)) {
            hoveredNode = *it;
            break;
        }
    }
    SceneObject* hoveredTransition = nullptr;
    if (hoveredNode == nullptr && canvasHovered) {
        float bestDistance = 8.0f;
        for (SceneObject* transition : transitions) {
            SceneObject* src = MapMaker::FindSectorById(sceneObjects, transition->mapTransition.sourceSectorId);
            SceneObject* dst = MapMaker::FindSectorById(sceneObjects, transition->mapTransition.destinationSectorId);
            if (src == nullptr || dst == nullptr) continue;
            const float distance = DistanceToEdge(mousePos, nodePortOut(*src), nodePortIn(*dst));
            if (distance < bestDistance) {
                bestDistance = distance;
                hoveredTransition = transition;
            }
        }
    }

    if (canvasHovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (std::fabs(wheel) > 0.001f) {
            const ImVec2 graphUnderMouse = ScreenToGraph(canvasMin, mousePos);
            g_state.zoom = std::clamp(g_state.zoom * (1.0f + wheel * 0.11f), kMinZoom, kMaxZoom);
            g_state.pan.x = graphUnderMouse.x - (mousePos.x - canvasMin.x) / g_state.zoom;
            g_state.pan.y = graphUnderMouse.y - (mousePos.y - canvasMin.y) / g_state.zoom;
        }
    }

    if (canvasActive && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        g_state.panning = true;
    }
    if (g_state.panning) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        g_state.pan.x -= delta.x / g_state.zoom;
        g_state.pan.y -= delta.y / g_state.zoom;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            g_state.panning = false;
        }
    }

    auto isNodeSelected = [&](int objectId) {
        return objectId == selectedObjectId ||
               std::find(selectedObjectIds.begin(), selectedObjectIds.end(), objectId) !=
                   selectedObjectIds.end();
    };

    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoveredNode != nullptr && hoveredPortOut) {
            g_state.linkDragActive = true;
            g_state.linkSourceObjectId = hoveredNode->id;
        } else if (hoveredNode != nullptr) {
            const bool additive = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
            if (!isNodeSelected(hoveredNode->id) || additive) {
                setPrimarySelection(hoveredNode->id, additive);
            } else {
                selectedObjectId = hoveredNode->id;
            }
            g_state.selectedTransitionObjectId = -1;
            g_state.draggingNodes = true;
            g_state.dragUndoRecorded = false;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                g_state.draggingNodes = false;
                focusMapSector(hoveredNode->id);
            }
        } else if (hoveredTransition != nullptr) {
            setPrimarySelection(hoveredTransition->id, false);
            g_state.selectedTransitionObjectId = hoveredTransition->id;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                SceneObject* portal = MapMaker::FindPortalById(
                    sceneObjects, hoveredTransition->mapTransition.sourcePortalId);
                if (portal == nullptr) {
                    portal = MapMaker::FindPortalById(sceneObjects,
                                                      hoveredTransition->mapTransition.destinationPortalId);
                }
                if (portal != nullptr) {
                    setPrimarySelection(portal->id, false);
                    focusViewportOnSelection();
                }
            }
        } else {
            g_state.boxSelectActive = true;
            g_state.boxSelectStartScreen = mousePos;
            g_state.selectedTransitionObjectId = -1;
            if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) {
                clearSelection();
            }
        }
    }

    if (g_state.draggingNodes) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            if (!g_state.dragUndoRecorded) {
                recordState("sectorMapMoveNodes");
                g_state.dragUndoRecorded = true;
            }
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            for (SceneObject* sector : sectors) {
                if (!isNodeSelected(sector->id)) continue;
                sector->mapSector.graphPosition.x += delta.x / g_state.zoom;
                sector->mapSector.graphPosition.y += delta.y / g_state.zoom;
            }
            if (g_state.dragUndoRecorded) {
                projectManager.currentProject.hasUnsavedChanges = true;
            }
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_state.draggingNodes = false;
        }
    }

    if (g_state.boxSelectActive) {
        const ImVec2 a = g_state.boxSelectStartScreen;
        const ImVec2 b = mousePos;
        const ImVec2 mn(std::min(a.x, b.x), std::min(a.y, b.y));
        const ImVec2 mx(std::max(a.x, b.x), std::max(a.y, b.y));
        drawList->AddRectFilled(mn, mx, ImGui::GetColorU32(ImVec4(0.35f, 0.6f, 0.9f, 0.15f)));
        drawList->AddRect(mn, mx, ImGui::GetColorU32(ImVec4(0.35f, 0.6f, 0.9f, 0.8f)));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            for (SceneObject* sector : sectors) {
                ImVec2 nodeMin, nodeMax;
                nodeRect(*sector, nodeMin, nodeMax);
                const bool overlaps = nodeMin.x <= mx.x && nodeMax.x >= mn.x &&
                                      nodeMin.y <= mx.y && nodeMax.y >= mn.y;
                if (overlaps) {
                    setPrimarySelection(sector->id, true);
                }
            }
            g_state.boxSelectActive = false;
        }
    }

    std::string pendingConnectSourceSectorId;
    std::string pendingConnectDestSectorId;
    int pendingDeleteTransitionObjectId = -1;
    if (g_state.linkDragActive) {
        SceneObject* sourceSector = findObjectById(g_state.linkSourceObjectId);
        if (sourceSector == nullptr || !sourceSector->hasMapSector) {
            g_state.linkDragActive = false;
        } else {
            drawList->AddBezierCubic(
                nodePortOut(*sourceSector),
                ImVec2(nodePortOut(*sourceSector).x + 40.0f * g_state.zoom, nodePortOut(*sourceSector).y),
                ImVec2(mousePos.x - 40.0f * g_state.zoom, mousePos.y), mousePos,
                ImGui::GetColorU32(ImVec4(0.85f, 0.85f, 0.5f, 0.9f)), 2.0f);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (hoveredNode != nullptr && hoveredNode->id != sourceSector->id) {
                    pendingConnectSourceSectorId = sourceSector->mapSector.sectorId;
                    pendingConnectDestSectorId = hoveredNode->mapSector.sectorId;
                }
                g_state.linkDragActive = false;
                g_state.linkSourceObjectId = -1;
            }
        }
    }

    if (canvasHovered && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            wantFrameSelection = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            if (g_state.selectedTransitionObjectId >= 0) {
                pendingDeleteTransitionObjectId = g_state.selectedTransitionObjectId;
            } else {
                for (SceneObject* sector : sectors) {
                    if (isNodeSelected(sector->id)) {
                        g_state.deletePopupOpen = true;
                        g_state.deleteObjectId = sector->id;
                        break;
                    }
                }
            }
        }
    }

    for (SceneObject* transition : transitions) {
        MapTransitionComponent& t = transition->mapTransition;
        SceneObject* src = sectorsById.count(t.sourceSectorId) ? sectorsById[t.sourceSectorId] : nullptr;
        SceneObject* dst = sectorsById.count(t.destinationSectorId) ? sectorsById[t.destinationSectorId] : nullptr;
        if (src == nullptr && dst == nullptr) continue;

        const bool isBroken = src == nullptr || dst == nullptr;
        const bool isSelected = transition->id == g_state.selectedTransitionObjectId ||
                                transition->id == selectedObjectId;
        const bool isHovered = transition == hoveredTransition;
        const bool isConditional = t.locked || !t.condition.empty();

        ImVec2 from = src != nullptr ? nodePortOut(*src) : ImVec2(0, 0);
        ImVec2 to = dst != nullptr ? nodePortIn(*dst) : ImVec2(0, 0);
        if (src == nullptr) from = ImVec2(to.x - 70.0f * g_state.zoom, to.y - 40.0f * g_state.zoom);
        if (dst == nullptr) to = ImVec2(from.x + 70.0f * g_state.zoom, from.y - 40.0f * g_state.zoom);

        ImVec4 edgeColor = isBroken ? ImVec4(0.95f, 0.35f, 0.3f, 0.95f)
                          : isConditional ? ImVec4(0.95f, 0.8f, 0.35f, 0.9f)
                                          : ImVec4(0.65f, 0.75f, 0.85f, 0.75f);
        if (isSelected) edgeColor = ImVec4(0.4f, 0.85f, 1.0f, 1.0f);
        else if (isHovered) edgeColor = ImVec4(0.8f, 0.9f, 1.0f, 1.0f);
        const ImU32 edgeCol32 = ImGui::GetColorU32(edgeColor);
        const float thickness = (isSelected || isHovered) ? 3.0f : 2.0f;

        if (isBroken) {
            const int dashes = 14;
            for (int i = 0; i < dashes; i += 2) {
                const float t0 = static_cast<float>(i) / dashes;
                const float t1 = static_cast<float>(i + 1) / dashes;
                drawList->AddLine(ImVec2(from.x + (to.x - from.x) * t0, from.y + (to.y - from.y) * t0),
                                  ImVec2(from.x + (to.x - from.x) * t1, from.y + (to.y - from.y) * t1),
                                  edgeCol32, thickness);
            }
        } else {
            const float tangent = std::max(30.0f * g_state.zoom, std::fabs(to.x - from.x) * 0.4f);
            drawList->AddBezierCubic(from, ImVec2(from.x + tangent, from.y),
                                     ImVec2(to.x - tangent, to.y), to, edgeCol32, thickness);
        }

        {
            const ImVec2 tip = EdgeBezierPoint(from, to, 0.92f);
            const ImVec2 back = EdgeBezierPoint(from, to, 0.84f);
            ImVec2 dir(tip.x - back.x, tip.y - back.y);
            const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (len > 0.5f) {
                dir.x /= len;
                dir.y /= len;
                const ImVec2 normal(-dir.y, dir.x);
                const float arrowSize = 6.0f * std::max(g_state.zoom, 0.6f);
                auto drawArrow = [&](const ImVec2& at, float sign) {
                    drawList->AddTriangleFilled(
                        ImVec2(at.x + dir.x * arrowSize * sign, at.y + dir.y * arrowSize * sign),
                        ImVec2(at.x + normal.x * arrowSize * 0.6f, at.y + normal.y * arrowSize * 0.6f),
                        ImVec2(at.x - normal.x * arrowSize * 0.6f, at.y - normal.y * arrowSize * 0.6f),
                        edgeCol32);
                };
                drawArrow(tip, 1.0f);
                if (t.bidirectional) {
                    const ImVec2 tail = EdgeBezierPoint(from, to, 0.08f);
                    drawArrow(tail, -1.0f);
                }
            }
        }

        const ImVec2 mid = EdgeBezierPoint(from, to, 0.5f);
        if (isConditional) {
            drawList->AddCircleFilled(mid, 6.5f * std::max(g_state.zoom, 0.6f),
                                      ImGui::GetColorU32(ImVec4(0.2f, 0.18f, 0.1f, 0.95f)));
            drawList->AddCircle(mid, 6.5f * std::max(g_state.zoom, 0.6f), edgeCol32, 0, 1.5f);
            const char* marker = t.locked ? "L" : "?";
            const ImVec2 markerSize = ImGui::CalcTextSize(marker);
            drawList->AddText(ImVec2(mid.x - markerSize.x * 0.5f, mid.y - markerSize.y * 0.5f),
                              edgeCol32, marker);
        }
        if (g_state.zoom > 0.55f) {
            std::string label = !t.editorLabel.empty() ? t.editorLabel
                                : (t.kind != MapTransitionKind::Door
                                       ? std::string(MapMaker::TransitionKindLabel(t.kind))
                                       : std::string());
            if (!label.empty()) {
                const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
                drawList->AddText(ImVec2(mid.x - labelSize.x * 0.5f, mid.y + 8.0f),
                                  ImGui::GetColorU32(ImVec4(0.8f, 0.85f, 0.9f, 0.8f)), label.c_str());
            }
        }
    }

    const std::string activeSectorId = mapRootObj->mapRoot.activeSectorId;
    const std::string startSectorId = mapRootObj->mapRoot.startSectorId;
    const bool filterActive = g_state.searchBuffer[0] != '\0';
    for (SceneObject* sector : sectors) {
        ImVec2 mn, mx;
        nodeRect(*sector, mn, mx);
        if (mx.x < canvasMin.x || mn.x > canvasMax.x || mx.y < canvasMin.y || mn.y > canvasMax.y) {
            continue;
        }
        const bool isSelected = isNodeSelected(sector->id);
        const bool isHovered = sector == hoveredNode;
        const bool matchesFilter = NameMatchesFilter(sector->name, g_state.searchBuffer);
        const float dim = (filterActive && !matchesFilter) ? 0.35f : 1.0f;

        const glm::vec3 c = sector->mapSector.color;
        const ImU32 fillColor = ImGui::GetColorU32(ImVec4(0.13f * dim, 0.15f * dim, 0.18f * dim, 0.97f));
        const ImU32 stripColor = ImGui::GetColorU32(ImVec4(c.x * dim, c.y * dim, c.z * dim, 1.0f));
        ImVec4 borderVec = isSelected ? ImVec4(0.45f, 0.85f, 1.0f, 1.0f)
                          : isHovered ? ImVec4(0.75f, 0.8f, 0.9f, 0.9f)
                                      : ImVec4(0.35f, 0.4f, 0.5f, 0.8f);
        borderVec.w *= dim;

        drawList->AddRectFilled(mn, mx, fillColor, 6.0f);
        drawList->AddRectFilled(mn, ImVec2(mx.x, mn.y + 5.0f * g_state.zoom), stripColor, 6.0f,
                                ImDrawFlags_RoundCornersTop);
        drawList->AddRect(mn, mx, ImGui::GetColorU32(borderVec), 6.0f, 0, isSelected ? 2.5f : 1.2f);

        const float portRadius = std::max(3.5f, 4.5f * g_state.zoom);
        drawList->AddCircleFilled(nodePortIn(*sector), portRadius,
                                  ImGui::GetColorU32(ImVec4(0.5f, 0.6f, 0.7f, 0.9f * dim)));
        const bool portHot = hoveredPortOut && hoveredNode == sector;
        drawList->AddCircleFilled(nodePortOut(*sector), portRadius * (portHot ? 1.5f : 1.0f),
                                  ImGui::GetColorU32(portHot ? ImVec4(0.95f, 0.9f, 0.5f, 1.0f)
                                                             : ImVec4(0.5f, 0.6f, 0.7f, 0.9f * dim)));

        if (g_state.zoom > 0.4f) {
            const ImVec2 textPos(mn.x + 8.0f * g_state.zoom, mn.y + 9.0f * g_state.zoom);
            std::string title = sector->name;
            if (sector->mapSector.sectorId == startSectorId) title += "  [start]";
            drawList->AddText(textPos, ImGui::GetColorU32(ImVec4(0.95f, 0.97f, 1.0f, 0.95f * dim)),
                              title.c_str());
            char statusLine[96];
            if (connectedSectorIds.find(sector->mapSector.sectorId) == connectedSectorIds.end()) {
                std::snprintf(statusLine, sizeof(statusLine), "unlinked");
            } else if (sector->mapSector.estimatedMemoryMB > 0.0f) {
                std::snprintf(statusLine, sizeof(statusLine), "%.2f MB", sector->mapSector.estimatedMemoryMB);
            } else {
                std::snprintf(statusLine, sizeof(statusLine), "0.00 MB");
            }
            drawList->AddText(ImVec2(textPos.x, textPos.y + ImGui::GetTextLineHeight() + 2.0f),
                              ImGui::GetColorU32(ImVec4(0.6f, 0.65f, 0.72f, 0.8f * dim)), statusLine);
            if (sector->mapSector.sectorId == activeSectorId) {
                drawList->AddCircleFilled(ImVec2(mx.x - 10.0f * g_state.zoom, mn.y + 12.0f * g_state.zoom),
                                          3.5f * g_state.zoom,
                                          ImGui::GetColorU32(ImVec4(0.4f, 0.95f, 0.55f, 1.0f)));
            }
        }
    }

    drawList->PopClipRect();

    if (!pendingConnectSourceSectorId.empty() && !pendingConnectDestSectorId.empty()) {
        SceneObject* created = createMapTransitionObject(pendingConnectSourceSectorId,
                                                         pendingConnectDestSectorId, true);
        if (created != nullptr) {
            setPrimarySelection(created->id, false);
            g_state.selectedTransitionObjectId = created->id;
        }
    }
    if (pendingDeleteTransitionObjectId >= 0) {
        SceneObject* transition = findObjectById(pendingDeleteTransitionObjectId);
        if (transition != nullptr && transition->hasMapTransition) {
            setPrimarySelection(transition->id, false);
            deleteSelected();
        }
        g_state.selectedTransitionObjectId = -1;
    }

    static int contextNodeId = -1;
    static int contextTransitionId = -1;
    static ImVec2 contextGraphPos;
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        contextNodeId = hoveredNode != nullptr ? hoveredNode->id : -1;
        contextTransitionId = (hoveredNode == nullptr && hoveredTransition != nullptr)
                                  ? hoveredTransition->id
                                  : -1;
        contextGraphPos = ScreenToGraph(canvasMin, mousePos);
        ImGui::OpenPopup("##SectorMapContext");
    }
    if (ImGui::BeginPopup("##SectorMapContext")) {
        SceneObject* contextNode = findObjectById(contextNodeId);
        SceneObject* contextTransition = findObjectById(contextTransitionId);
        if (contextNode != nullptr && contextNode->hasMapSector) {
            ImGui::TextDisabled("%s", contextNode->name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Focus In Viewport")) {
                focusMapSector(contextNode->id);
            }
            if (ImGui::MenuItem("Rename...")) {
                g_state.renamePopupOpen = true;
                g_state.renameObjectId = contextNode->id;
                std::snprintf(g_state.renameBuffer, sizeof(g_state.renameBuffer), "%s",
                              contextNode->name.c_str());
            }
            if (ImGui::MenuItem("Duplicate Sector")) {
                setPrimarySelection(contextNode->id, false);
                duplicateSelected();
            }
            if (ImGui::MenuItem("Set As Start Sector")) {
                if (SceneObject* freshRoot = MapMaker::FindMapRoot(sceneObjects)) {
                    recordState("sectorMapSetStart");
                    freshRoot->mapRoot.startSectorId = contextNode->mapSector.sectorId;
                    projectManager.currentProject.hasUnsavedChanges = true;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Sector...")) {
                g_state.deletePopupOpen = true;
                g_state.deleteObjectId = contextNode->id;
            }
        } else if (contextTransition != nullptr && contextTransition->hasMapTransition) {
            ImGui::TextDisabled("%s", contextTransition->name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Select Transition")) {
                setPrimarySelection(contextTransition->id, false);
                g_state.selectedTransitionObjectId = contextTransition->id;
            }
            if (ImGui::MenuItem("Swap Direction")) {
                recordState("sectorMapSwapTransition");
                std::swap(contextTransition->mapTransition.sourceSectorId,
                          contextTransition->mapTransition.destinationSectorId);
                std::swap(contextTransition->mapTransition.sourcePortalId,
                          contextTransition->mapTransition.destinationPortalId);
                projectManager.currentProject.hasUnsavedChanges = true;
            }
            bool bidirectional = contextTransition->mapTransition.bidirectional;
            if (ImGui::MenuItem("Bidirectional", nullptr, &bidirectional)) {
                recordState("sectorMapTransitionDirection");
                contextTransition->mapTransition.bidirectional = bidirectional;
                projectManager.currentProject.hasUnsavedChanges = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Disconnect (Delete Transition)")) {
                setPrimarySelection(contextTransition->id, false);
                deleteSelected();
                g_state.selectedTransitionObjectId = -1;
            }
        } else {
            if (ImGui::MenuItem("Create Sector Here")) {
                int sectorNumber = 1;
                for (const SceneObject& candidate : sceneObjects) {
                    if (candidate.hasMapSector) ++sectorNumber;
                }
                createMapSectorObject("Sector " + std::to_string(sectorNumber),
                                      glm::vec2(contextGraphPos.x, contextGraphPos.y), true);
            }
            if (ImGui::MenuItem("Frame All")) {
                g_state.viewInitialized = false;
            }
        }
        ImGui::EndPopup();
    }

    if (g_state.renamePopupOpen) {
        ImGui::OpenPopup("Rename Sector");
    }
    if (beginCardModal("Rename Sector", 0.0f, &g_state.renamePopupOpen)) {
        cardModalText("Choose a new name for this sector.");
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1.0f);
        const bool submitted = ImGui::InputText("##SectorRenameName", g_state.renameBuffer,
                                                sizeof(g_state.renameBuffer),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        const bool canRename = g_state.renameBuffer[0] != '\0';
        if (cardModalButton("Cancel", CardButtonKind::Neutral, 0, 2)) {
            g_state.renamePopupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        if ((cardModalButton("Rename", CardButtonKind::Primary, 1, 2) || submitted) && canRename) {
            if (SceneObject* target = findObjectById(g_state.renameObjectId)) {
                recordState("sectorMapRename");
                const std::string oldName = target->name;
                target->name = g_state.renameBuffer;
                propagateObjectRenameReferences(oldName, target->name, target->id);
                projectManager.currentProject.hasUnsavedChanges = true;
            }
            g_state.renamePopupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        endCardModal();
    }

    if (g_state.deletePopupOpen) {
        ImGui::OpenPopup("Delete Sector");
    }
    if (beginCardModal("Delete Sector", 0.0f, &g_state.deletePopupOpen)) {
        SceneObject* target = findObjectById(g_state.deleteObjectId);
        cardModalText(target != nullptr
                          ? ("Delete sector \"" + target->name +
                             "\" and everything inside it? Transitions pointing at it will report as broken.")
                                .c_str()
                          : "Sector no longer exists.");
        if (cardModalButton("Cancel", CardButtonKind::Neutral, 0, 2)) {
            g_state.deletePopupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        if (cardModalButton("Delete", CardButtonKind::Danger, 1, 2)) {
            if (target != nullptr) {
                setPrimarySelection(target->id, false);
                deleteSelected();
            }
            g_state.deletePopupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        endCardModal();
    }

    ImGui::End();
}
#pragma endregion
