
#include "Engine.h"
#include "EditorLocalization.h"
#include "Modu2DStats.h"
#include "SpritesheetFormat.h"

#include <algorithm>
#include <deque>

namespace Loc = Modularity::Loc;

namespace {

std::string LowerExtension(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

const char* ToolLabel(int tool) {
    switch (tool) {
        case 0: return "Pencil";
        case 1: return "Eraser";
        case 2: return "Rectangle";
        case 3: return "Fill";
        case 4: return "Line";
        case 5: return "Eyedropper";
        case 6: return "Select";
        default: return "Pencil";
    }
}

const char* ToolTooltip(int tool) {
    switch (tool) {
        case 0: return "Paint single cells. Drag to paint a stroke.";
        case 1: return "Erase cells. Drag to erase a stroke.";
        case 2: return "Drag to fill a rectangle of cells.";
        case 3: return "Flood fill the connected region of matching cells.";
        case 4: return "Drag to draw a straight line of cells.";
        case 5: return "Pick the tile under the cursor as the active tile.";
        case 6: return "Drag to select a region. Ctrl+C copies, Ctrl+V pastes.";
        default: return "";
    }
}

}  // namespace

SceneObject* Engine::getActiveAssemblageRoot() {
    if (assemblageActiveObjectId < 0) return nullptr;
    SceneObject* obj = findObjectById(assemblageActiveObjectId);
    if (obj == nullptr || !obj->hasAssemblage) {
        assemblageActiveObjectId = -1;
        return nullptr;
    }
    return obj;
}

bool Engine::resolveAssemblageEditTarget(std::string& outAssetPath,
                                         Assemblage::Asset*& outAsset,
                                         int& outLayerId) {
    outAsset = nullptr;
    outLayerId = -1;
    outAssetPath.clear();

    SceneObject* root = getActiveAssemblageRoot();
    if (root == nullptr) return false;
    if (root->assemblage.assetPath.empty()) return false;

    outAssetPath = root->assemblage.assetPath;
    outAsset = assemblageRuntime.asset(outAssetPath);
    if (outAsset == nullptr) return false;

    outLayerId = assemblageActiveLayerId;
    if (outAsset->findLayer(outLayerId) == nullptr) {
        outLayerId = outAsset->layers.empty() ? -1 : outAsset->layers.front().id;
        assemblageActiveLayerId = outLayerId;
    }
    return outLayerId >= 0;
}

void Engine::beginAssemblageStroke(const char* label) {
    std::string assetPath;
    Assemblage::Asset* asset = nullptr;
    int layerId = -1;
    if (!resolveAssemblageEditTarget(assetPath, asset, layerId)) return;

    assemblageStroke = AssemblageEditRecord{};
    assemblageStroke.assetPath = assetPath;
    assemblageStroke.label = label ? label : "Assemblage Edit";
    assemblageStrokeActive = true;
}

bool Engine::applyAssemblageCell(int cellX, int cellY, uint32_t value) {
    if (!assemblageStrokeActive) return false;

    std::string assetPath;
    Assemblage::Asset* asset = nullptr;
    int layerId = -1;
    if (!resolveAssemblageEditTarget(assetPath, asset, layerId)) return false;

    const Assemblage::Layer* layer = asset->findLayer(layerId);
    if (layer == nullptr || layer->locked) return false;

    const uint32_t before = Assemblage::GetCell(*asset, layerId, cellX, cellY);
    if (before == value) return false;

    Assemblage::ChunkCoord touched;
    if (!Assemblage::SetCell(*asset, layerId, cellX, cellY, value, &touched)) return false;

    AssemblageCellDelta delta;
    delta.layerId = layerId;
    delta.cellX = cellX;
    delta.cellY = cellY;
    delta.before = before;
    delta.after = value;
    assemblageStroke.cells.push_back(delta);

    assemblageRuntime.markCellDirty(assetPath, layerId, cellX, cellY);
    assemblageRuntime.markUnsaved(assetPath, true);
    return true;
}

void Engine::endAssemblageStroke() {
    if (!assemblageStrokeActive) return;
    assemblageStrokeActive = false;

    if (assemblageStroke.cells.empty()) {
        assemblageStroke = AssemblageEditRecord{};
        return;
    }

    SceneSnapshot snap;
    snap.kind = SceneSnapshot::Kind::AssemblageCells;
    snap.assemblageEdit = std::move(assemblageStroke);
    assemblageStroke = AssemblageEditRecord{};
    pushUndoSnapshot(std::move(snap), "assemblage");

    if (projectManager.currentProject.isLoaded) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }
}

void Engine::applyAssemblageEdit(const AssemblageEditRecord& record, bool forward) {
    Assemblage::Asset* asset = assemblageRuntime.asset(record.assetPath);
    if (asset == nullptr) {
        addConsoleMessage("Cannot undo the Assemblage edit: " + record.assetPath +
                              " is no longer loaded.",
                          ConsoleMessageType::Warning);
        return;
    }

    const size_t count = record.cells.size();
    for (size_t i = 0; i < count; ++i) {
        const AssemblageCellDelta& delta =
            forward ? record.cells[i] : record.cells[count - 1 - i];
        const uint32_t value = forward ? delta.after : delta.before;
        Assemblage::SetCell(*asset, delta.layerId, delta.cellX, delta.cellY, value);
        assemblageRuntime.markCellDirty(record.assetPath, delta.layerId, delta.cellX,
                                        delta.cellY);
    }
    assemblageRuntime.markUnsaved(record.assetPath, true);
}

uint32_t Engine::resolveAssemblageBrushCell() {
    if (assemblageTool == AssemblageTool::Erase) return 0u;

    uint32_t tileId = assemblageActiveTile;
    if (assemblageRandomVariants) {
        SceneObject* root = getActiveAssemblageRoot();
        if (root != nullptr) {
            if (const Assemblage::Tileset* set =
                    assemblageRuntime.tileset(root->assemblage.assetPath)) {
                if (const Assemblage::TileDef* tile = set->find(tileId)) {
                    if (!tile->variants.empty()) {
                        const size_t roll = static_cast<size_t>(std::rand()) %
                                            (tile->variants.size() + 1);
                        if (roll < tile->variants.size()) tileId = tile->variants[roll];
                    }
                }
            }
        }
    }
    return Assemblage::MakeCell(tileId);
}

void Engine::addAssemblageTilesFromImage(const std::string& droppedPath) {
    SceneObject* root = getActiveAssemblageRoot();
    if (root == nullptr) return;
    const std::string assetPath = root->assemblage.assetPath;

    AssemblageRuntime::Entry* entry = assemblageRuntime.acquire(assetPath);
    if (entry == nullptr) return;

    const fs::path imagePath(droppedPath);
    const std::string extension = LowerExtension(imagePath);
    if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" &&
        extension != ".bmp" && extension != ".tga") {
        addConsoleMessage("Assemblage tiles need an image file; ignored " + droppedPath,
                          ConsoleMessageType::Warning);
        return;
    }

    if (entry->asset.tilesetPath.empty()) {
        Assemblage::Tileset created;
        created.tilesetId = Assemblage::GenerateTilesetId();
        created.name = entry->asset.name;

        const fs::path tilesetDir =
            projectManager.currentProject.projectPath / "Assets" / "Assemblages";
        std::error_code ec;
        fs::create_directories(tilesetDir, ec);
        fs::path tilesetPath =
            tilesetDir / (entry->asset.name + Assemblage::kTilesetExtension);
        for (int suffix = 1; fs::exists(tilesetPath, ec) && suffix < 1000; ++suffix) {
            tilesetPath = tilesetDir / (entry->asset.name + " " + std::to_string(suffix) +
                                       Assemblage::kTilesetExtension);
        }

        std::string error;
        if (!Assemblage::SaveTileset(tilesetPath, created, error)) {
            addConsoleMessage("Failed to create the tileset: " + error,
                              ConsoleMessageType::Error);
            return;
        }
        entry->asset.tilesetPath = assemblageRuntime.makeProjectRelative(tilesetPath);
        entry->tileset = created;
        entry->tilesetSourcePath = entry->asset.tilesetPath;
        assemblageRuntime.markUnsaved(assetPath, true);
    }

    const std::string sheetRelative = assemblageRuntime.makeProjectRelative(imagePath);

    std::vector<glm::ivec4> rects;
    std::vector<std::string> names;
    const fs::path sidecar(imagePath.string() + ".spritesheet");
    std::error_code ec;
    if (fs::exists(sidecar, ec)) {
        std::ifstream in(sidecar);
        if (in.is_open()) {
            std::ostringstream buffer;
            buffer << in.rdbuf();
            const SpritesheetDocument doc = ParseSpritesheet(buffer.str()).document;
            rects = doc.rects;
            names = doc.names;
        }
    }
    if (rects.empty()) {
        rects.push_back(glm::ivec4(0));
        names.clear();
    }

    const std::string baseName = imagePath.stem().string();
    int added = 0;
    for (size_t i = 0; i < rects.size(); ++i) {
        Assemblage::TileDef tile;
        tile.id = entry->tileset.nextTileId++;
        tile.name = (i < names.size() && !names[i].empty())
                        ? names[i]
                        : (rects.size() > 1 ? baseName + " " + std::to_string(i) : baseName);
        tile.sheetPath = sheetRelative;
        tile.frame.rect = rects[i];
        entry->tileset.tiles.push_back(tile);
        if (added == 0) assemblageActiveTile = tile.id;
        ++added;
    }

    std::string error;
    if (!Assemblage::SaveTileset(assemblageRuntime.resolvePath(entry->asset.tilesetPath),
                                 entry->tileset, error)) {
        addConsoleMessage("Failed to save the tileset: " + error, ConsoleMessageType::Error);
        return;
    }
    assemblageRuntime.markAssetDirty(assetPath);
    addConsoleMessage("Added " + std::to_string(added) + " tile(s) from " + sheetRelative,
                      ConsoleMessageType::Success);
}

void Engine::assemblageFillRect(glm::ivec2 a, glm::ivec2 b, uint32_t value) {
    const int minX = std::min(a.x, b.x);
    const int maxX = std::max(a.x, b.x);
    const int minY = std::min(a.y, b.y);
    const int maxY = std::max(a.y, b.y);

    const long long area = static_cast<long long>(maxX - minX + 1) *
                           static_cast<long long>(maxY - minY + 1);
    if (area > 1000000ll) {
        addConsoleMessage("Assemblage rectangle is too large (" + std::to_string(area) +
                              " cells); ignored.",
                          ConsoleMessageType::Warning);
        return;
    }

    beginAssemblageStroke("Rectangle");
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            applyAssemblageCell(x, y, value);
        }
    }
    endAssemblageStroke();
}

void Engine::assemblageDrawLine(glm::ivec2 a, glm::ivec2 b, uint32_t value) {
    beginAssemblageStroke("Line");

    int x0 = a.x, y0 = a.y;
    const int x1 = b.x, y1 = b.y;
    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 100000; ++guard) {
        applyAssemblageCell(x0, y0, value);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
    endAssemblageStroke();
}

void Engine::assemblageFloodFill(int cellX, int cellY, uint32_t value) {
    std::string assetPath;
    Assemblage::Asset* asset = nullptr;
    int layerId = -1;
    if (!resolveAssemblageEditTarget(assetPath, asset, layerId)) return;

    const uint32_t target = Assemblage::GetCell(*asset, layerId, cellX, cellY);
    if (target == value) return;

    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    if (!Assemblage::LayerCellBounds(*asset, layerId, minX, minY, maxX, maxY)) {
        minX = maxX = cellX;
        minY = maxY = cellY;
    }
    minX = std::min(minX, cellX) - 1;
    minY = std::min(minY, cellY) - 1;
    maxX = std::max(maxX, cellX) + 1;
    maxY = std::max(maxY, cellY) + 1;

    const long long area = static_cast<long long>(maxX - minX + 1) *
                           static_cast<long long>(maxY - minY + 1);
    if (area > 4000000ll) {
        addConsoleMessage("Assemblage fill region is too large; ignored.",
                          ConsoleMessageType::Warning);
        return;
    }

    beginAssemblageStroke("Fill");

    const int width = maxX - minX + 1;
    const int height = maxY - minY + 1;
    std::vector<uint8_t> visited(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
    std::deque<glm::ivec2> queue;
    queue.push_back(glm::ivec2(cellX, cellY));

    while (!queue.empty()) {
        const glm::ivec2 cell = queue.front();
        queue.pop_front();
        if (cell.x < minX || cell.x > maxX || cell.y < minY || cell.y > maxY) continue;

        const size_t index = static_cast<size_t>(cell.y - minY) * static_cast<size_t>(width) +
                             static_cast<size_t>(cell.x - minX);
        if (visited[index]) continue;
        visited[index] = 1u;

        if (Assemblage::GetCell(*asset, layerId, cell.x, cell.y) != target) continue;
        if (!applyAssemblageCell(cell.x, cell.y, value)) continue;

        queue.push_back(glm::ivec2(cell.x + 1, cell.y));
        queue.push_back(glm::ivec2(cell.x - 1, cell.y));
        queue.push_back(glm::ivec2(cell.x, cell.y + 1));
        queue.push_back(glm::ivec2(cell.x, cell.y - 1));
    }

    endAssemblageStroke();
}

void Engine::assemblageCopySelection() {
    if (!assemblageHasSelection) return;

    std::string assetPath;
    Assemblage::Asset* asset = nullptr;
    int layerId = -1;
    if (!resolveAssemblageEditTarget(assetPath, asset, layerId)) return;

    assemblageClipboard.clear();
    const glm::ivec2 origin = assemblageSelectionMin;
    for (int y = assemblageSelectionMin.y; y <= assemblageSelectionMax.y; ++y) {
        for (int x = assemblageSelectionMin.x; x <= assemblageSelectionMax.x; ++x) {
            AssemblageClipboardCell entry;
            entry.dx = x - origin.x;
            entry.dy = y - origin.y;
            entry.cell = Assemblage::GetCell(*asset, layerId, x, y);
            assemblageClipboard.push_back(entry);
        }
    }
    assemblageClipboardSize =
        glm::ivec2(assemblageSelectionMax.x - assemblageSelectionMin.x + 1,
                   assemblageSelectionMax.y - assemblageSelectionMin.y + 1);
    addConsoleMessage("Copied " + std::to_string(assemblageClipboard.size()) +
                          " Assemblage cells.",
                      ConsoleMessageType::Info);
}

void Engine::assemblagePasteAt(glm::ivec2 origin) {
    if (assemblageClipboard.empty()) return;

    beginAssemblageStroke("Paste");
    for (const AssemblageClipboardCell& entry : assemblageClipboard) {
        applyAssemblageCell(origin.x + entry.dx, origin.y + entry.dy, entry.cell);
    }
    endAssemblageStroke();
}

int Engine::createAssemblageInScene(const std::string& name) {
    if (!projectManager.currentProject.isLoaded) {
        addConsoleMessage("Open a project before creating an Assemblage.",
                          ConsoleMessageType::Warning);
        return -1;
    }

    recordState("createAssemblage");

    Assemblage::Asset asset;
    asset.assemblageId = Assemblage::GenerateAssemblageId();
    asset.name = name;
    asset.addLayer("Ground");

    const fs::path assetDir = projectManager.currentProject.projectPath / "Assets" / "Assemblages";
    std::error_code ec;
    fs::create_directories(assetDir, ec);

    fs::path assetPath = assetDir / (name + Assemblage::kAssetExtension);
    for (int suffix = 1; fs::exists(assetPath, ec) && suffix < 1000; ++suffix) {
        assetPath = assetDir / (name + " " + std::to_string(suffix) + Assemblage::kAssetExtension);
    }

    std::string error;
    if (!Assemblage::SaveAsset(assetPath, asset, error)) {
        addConsoleMessage("Failed to create the Assemblage asset: " + error,
                          ConsoleMessageType::Error);
        return -1;
    }

    assemblageRuntime.setProjectRoot(projectManager.currentProject.projectPath);
    const std::string relativePath = assemblageRuntime.makeProjectRelative(assetPath);
    assemblageRuntime.adopt(relativePath, asset);
    assemblageRuntime.markUnsaved(relativePath, false);

    const int rootId = nextObjectId++;
    SceneObject root(name, ObjectType::Empty, rootId);
    root.hasAssemblage = true;
    root.assemblage.assemblageId = asset.assemblageId;
    root.assemblage.assetPath = relativePath;
    root.localPosition = root.position;
    root.localRotation = root.rotation;
    root.localScale = root.scale;
    root.localInitialized = true;
    EnsureInspectorComponentMetadata(root);
    sceneObjects.push_back(root);

    assemblageActiveObjectId = rootId;
    assemblageActiveLayerId = asset.layers.front().id;
    syncAssemblageLayerObjects(rootId);

    markRuntimeScriptBindingsDirty();
    setPrimarySelection(rootId);
    projectManager.currentProject.hasUnsavedChanges = true;
    addConsoleMessage("Created Assemblage: " + relativePath, ConsoleMessageType::Success);
    return rootId;
}

void Engine::syncAssemblageLayerObjects(int rootObjectId) {
    SceneObject* root = findObjectById(rootObjectId);
    if (root == nullptr || !root->hasAssemblage) return;

    const std::string assetPath = root->assemblage.assetPath;
    Assemblage::Asset* asset = assemblageRuntime.asset(assetPath);
    if (asset == nullptr) return;

    std::vector<int> existingLayerIds;
    for (const SceneObject& obj : sceneObjects) {
        if (obj.hasAssemblageLayer && obj.parentId == rootObjectId) {
            existingLayerIds.push_back(obj.assemblageLayer.layerId);
        }
    }

    for (const Assemblage::Layer& layer : asset->layers) {
        if (std::find(existingLayerIds.begin(), existingLayerIds.end(), layer.id) !=
            existingLayerIds.end()) {
            continue;
        }
        const int layerObjectId = nextObjectId++;
        SceneObject layerObj(layer.name, ObjectType::Empty, layerObjectId);
        layerObj.parentId = rootObjectId;
        layerObj.hasAssemblageLayer = true;
        layerObj.assemblageLayer.layerId = layer.id;
        layerObj.assemblageLayer.sortingOrder = layer.sortingOrder;
        layerObj.assemblageLayer.opacity = layer.opacity;
        layerObj.assemblageLayer.tint = layer.tint;
        layerObj.assemblageLayer.locked = layer.locked;
        layerObj.enabled = layer.visible;
        layerObj.localPosition = layerObj.position;
        layerObj.localRotation = layerObj.rotation;
        layerObj.localScale = layerObj.scale;
        layerObj.localInitialized = true;
        EnsureInspectorComponentMetadata(layerObj);
        sceneObjects.push_back(layerObj);

        if (SceneObject* rootAgain = findObjectById(rootObjectId)) {
            rootAgain->childIds.push_back(layerObjectId);
        }
    }
    markRuntimeScriptBindingsDirty();
}

int Engine::addAssemblageLayer(const std::string& name) {
    std::string assetPath;
    Assemblage::Asset* asset = nullptr;
    int layerId = -1;
    if (!resolveAssemblageEditTarget(assetPath, asset, layerId)) {
        SceneObject* root = getActiveAssemblageRoot();
        if (root == nullptr) return -1;
        asset = assemblageRuntime.asset(root->assemblage.assetPath);
        if (asset == nullptr) return -1;
        assetPath = root->assemblage.assetPath;
    }

    recordState("addAssemblageLayer");
    Assemblage::Layer& layer = asset->addLayer(name);
    int highest = 0;
    for (const Assemblage::Layer& existing : asset->layers) {
        highest = std::max(highest, existing.sortingOrder);
    }
    layer.sortingOrder = highest + 1;
    const int newLayerId = layer.id;

    assemblageRuntime.markUnsaved(assetPath, true);
    syncAssemblageLayerObjects(assemblageActiveObjectId);
    assemblageActiveLayerId = newLayerId;
    projectManager.currentProject.hasUnsavedChanges = true;
    return newLayerId;
}

void Engine::removeAssemblageLayer(int layerId) {
    SceneObject* root = getActiveAssemblageRoot();
    if (root == nullptr) return;
    const std::string assetPath = root->assemblage.assetPath;
    const int rootId = root->id;

    Assemblage::Asset* asset = assemblageRuntime.asset(assetPath);
    if (asset == nullptr) return;

    recordState("removeAssemblageLayer");

    asset->layers.erase(std::remove_if(asset->layers.begin(), asset->layers.end(),
                                       [layerId](const Assemblage::Layer& layer) {
                                           return layer.id == layerId;
                                       }),
                        asset->layers.end());

    std::vector<int> removedIds;
    for (const SceneObject& obj : sceneObjects) {
        if (obj.hasAssemblageLayer && obj.parentId == rootId &&
            obj.assemblageLayer.layerId == layerId) {
            removedIds.push_back(obj.id);
        }
    }
    for (int removedId : removedIds) {
        sceneObjects.erase(std::remove_if(sceneObjects.begin(), sceneObjects.end(),
                                          [removedId](const SceneObject& obj) {
                                              return obj.id == removedId;
                                          }),
                           sceneObjects.end());
    }
    if (SceneObject* rootAgain = findObjectById(rootId)) {
        rootAgain->childIds.erase(
            std::remove_if(rootAgain->childIds.begin(), rootAgain->childIds.end(),
                           [&removedIds](int childId) {
                               return std::find(removedIds.begin(), removedIds.end(), childId) !=
                                      removedIds.end();
                           }),
            rootAgain->childIds.end());
    }

    assemblageRuntime.markLayerDirty(assetPath, layerId);
    assemblageRuntime.markUnsaved(assetPath, true);
    if (assemblageActiveLayerId == layerId) {
        assemblageActiveLayerId = asset->layers.empty() ? -1 : asset->layers.front().id;
    }
    markRuntimeScriptBindingsDirty();
    projectManager.currentProject.hasUnsavedChanges = true;
}

bool Engine::saveDirtyAssemblages(std::string& outError) {
    return assemblageRuntime.saveAllDirty(outError);
}

void Engine::renderAssemblageWindow() {
    if (!ImGui::Begin(Loc::WindowRef("Assemblage"), &showAssemblageWindow)) {
        Modu2DStats::CountWindowSkipped();
        ImGui::End();
        return;
    }

    std::vector<std::pair<int, std::string>> available;
    for (const SceneObject& obj : sceneObjects) {
        if (obj.hasAssemblage) available.emplace_back(obj.id, obj.name);
    }

    if (available.empty()) {
        ImGui::TextWrapped(
            "No Assemblage in this scene yet. An Assemblage is a grid you paint tiles "
            "into; Freeform objects can sit between its layers.");
        ImGui::Spacing();
        if (ImGui::Button("Create Assemblage")) {
            createAssemblageInScene("Assemblage");
        }
        ImGui::End();
        return;
    }

    if (assemblageActiveObjectId < 0 ||
        std::find_if(available.begin(), available.end(), [this](const auto& entry) {
            return entry.first == assemblageActiveObjectId;
        }) == available.end()) {
        assemblageActiveObjectId = available.front().first;
    }

    std::string currentName = "(none)";
    for (const auto& entry : available) {
        if (entry.first == assemblageActiveObjectId) currentName = entry.second;
    }

    ImGui::SetNextItemWidth(-90.0f);
    if (ImGui::BeginCombo("##assemblage_pick", currentName.c_str())) {
        for (const auto& entry : available) {
            const bool selected = entry.first == assemblageActiveObjectId;
            if (ImGui::Selectable(entry.second.c_str(), selected)) {
                assemblageActiveObjectId = entry.first;
                assemblageActiveLayerId = -1;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("New##assemblage_new")) {
        createAssemblageInScene("Assemblage");
    }

    SceneObject* root = getActiveAssemblageRoot();
    if (root == nullptr) {
        ImGui::End();
        return;
    }
    const std::string assetPath = root->assemblage.assetPath;

    AssemblageRuntime::Entry* entry = assemblageRuntime.acquire(assetPath);
    if (entry == nullptr) {
        const AssemblageRuntime::Entry* failed = assemblageRuntime.find(assetPath);
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Could not load %s",
                           assetPath.c_str());
        if (failed != nullptr && !failed->loadError.empty()) {
            ImGui::TextWrapped("%s", failed->loadError.c_str());
        }
        ImGui::End();
        return;
    }

    if (assemblageRuntime.hasUnsavedChanges(assetPath)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "*");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unsaved tile changes");
    }

    ImGui::Separator();

    ImGui::TextUnformatted("Tools");
    for (int tool = 0; tool <= 6; ++tool) {
        if (tool % 4 != 0) ImGui::SameLine();
        const bool active = static_cast<int>(assemblageTool) == tool;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        if (ImGui::Button(ToolLabel(tool), ImVec2(78.0f, 0.0f))) {
            assemblageTool = static_cast<AssemblageTool>(tool);
        }
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ToolTooltip(tool));
    }

    ImGui::Checkbox("Random variants", &assemblageRandomVariants);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Paint a random pick among the active tile and its variants.\n"
            "Variants are defined on the tile, not the palette.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &assemblageShowGridOverlay);

    ImGui::Separator();

    ImGui::TextUnformatted("Layers");
    if (ImGui::Button("Add Layer")) {
        addAssemblageLayer("Layer");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(entry->asset.layers.size() <= 1 || assemblageActiveLayerId < 0);
    if (ImGui::Button("Remove Layer")) {
        removeAssemblageLayer(assemblageActiveLayerId);
    }
    ImGui::EndDisabled();

    if (ImGui::BeginChild("##assemblage_layers", ImVec2(0.0f, 150.0f), true)) {
        std::vector<const Assemblage::Layer*> ordered;
        ordered.reserve(entry->asset.layers.size());
        for (const Assemblage::Layer& layer : entry->asset.layers) ordered.push_back(&layer);
        std::sort(ordered.begin(), ordered.end(),
                  [](const Assemblage::Layer* a, const Assemblage::Layer* b) {
                      return a->sortingOrder > b->sortingOrder;
                  });

        for (const Assemblage::Layer* layerPtr : ordered) {
            const int layerId = layerPtr->id;
            ImGui::PushID(layerId);

            Assemblage::Layer* layer = entry->asset.findLayer(layerId);
            if (layer == nullptr) {
                ImGui::PopID();
                continue;
            }

            bool visible = layer->visible;
            if (ImGui::Checkbox("##visible", &visible)) {
                layer->visible = visible;
                for (SceneObject& obj : sceneObjects) {
                    if (obj.hasAssemblageLayer && obj.parentId == root->id &&
                        obj.assemblageLayer.layerId == layerId) {
                        obj.enabled = visible;
                    }
                }
                assemblageRuntime.markUnsaved(assetPath, true);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Visible");

            ImGui::SameLine();
            bool locked = layer->locked;
            if (ImGui::Checkbox("##locked", &locked)) {
                layer->locked = locked;
                for (SceneObject& obj : sceneObjects) {
                    if (obj.hasAssemblageLayer && obj.parentId == root->id &&
                        obj.assemblageLayer.layerId == layerId) {
                        obj.assemblageLayer.locked = locked;
                    }
                }
                assemblageRuntime.markUnsaved(assetPath, true);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Locked (blocks painting)");

            ImGui::SameLine();
            const bool selected = layerId == assemblageActiveLayerId;
            std::string label = layer->name + "##sel";
            if (ImGui::Selectable(label.c_str(), selected, 0, ImVec2(0.0f, 0.0f))) {
                assemblageActiveLayerId = layerId;
            }

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
            ImGui::SetNextItemWidth(60.0f);
            int sorting = layer->sortingOrder;
            if (ImGui::DragInt("##sort", &sorting, 0.2f, -4096, 4096)) {
                layer->sortingOrder = sorting;
                for (SceneObject& obj : sceneObjects) {
                    if (obj.hasAssemblageLayer && obj.parentId == root->id &&
                        obj.assemblageLayer.layerId == layerId) {
                        obj.assemblageLayer.sortingOrder = sorting;
                    }
                }
                assemblageRuntime.markUnsaved(assetPath, true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Sorting order. Freeform sprites with an order between two\n"
                    "layers draw between them.");
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Separator();

    ImGui::TextUnformatted("Tiles");
    ImGui::SameLine();
    ImGui::TextDisabled("(drag an image here)");

    const std::string tilesetLabel =
        entry->asset.tilesetPath.empty() ? std::string("(no tileset)") : entry->asset.tilesetPath;
    ImGui::TextWrapped("Tileset: %s", tilesetLabel.c_str());

    ImGui::InvisibleButton("##tileset_drop", ImVec2(-1.0f, 28.0f));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATH")) {
            if (payload->Data != nullptr && payload->DataSize > 0) {
                const std::string dropped(static_cast<const char*>(payload->Data));
                addAssemblageTilesFromImage(dropped);
            }
        }
        ImGui::EndDragDropTarget();
    }
    {
        const ImVec2 dropMin = ImGui::GetItemRectMin();
        const ImVec2 dropMax = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(dropMin, dropMax,
                                            ImGui::GetColorU32(ImGuiCol_Border), 4.0f);
        const ImVec2 textPos(dropMin.x + 8.0f, dropMin.y + 5.0f);
        ImGui::GetWindowDrawList()->AddText(textPos, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            "Drop a sprite or sheet to add tiles");
    }

    if (ImGui::BeginChild("##assemblage_palette", ImVec2(0.0f, 0.0f), true)) {
        if (entry->tileset.tiles.empty()) {
            ImGui::TextDisabled("No tiles yet.");
        } else {
            const float thumb = 44.0f;
            const float avail = ImGui::GetContentRegionAvail().x;
            const int perRow = std::max(1, static_cast<int>(avail / (thumb + 8.0f)));
            int column = 0;
            for (const Assemblage::TileDef& tile : entry->tileset.tiles) {
                ImGui::PushID(static_cast<int>(tile.id));
                if (column != 0) ImGui::SameLine();

                unsigned int textureId = 0;
                ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
                if (!tile.sheetPath.empty()) {
                    if (Texture* texture = renderer.getTexture(
                            tile.sheetPath, MaterialProperties::TextureFilter::Point)) {
                        textureId = texture->GetID();
                        glm::vec2 uvMin, uvMax;
                        if (ResolveTileUv(tile, texture->GetWidth(), texture->GetHeight(), 0.0,
                                          uvMin, uvMax)) {
                            uv0 = ImVec2(uvMin.x, uvMin.y);
                            uv1 = ImVec2(uvMax.x, uvMax.y);
                        }
                    }
                }

                const bool selected = tile.id == assemblageActiveTile;
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
                }
                bool clicked = false;
                if (textureId != 0) {
                    clicked = ImGui::ImageButton("##tile", textureId,
                                                 ImVec2(thumb, thumb), uv0, uv1);
                } else {
                    clicked = ImGui::Button("?", ImVec2(thumb + 8.0f, thumb + 8.0f));
                }
                if (selected) ImGui::PopStyleColor();
                if (clicked) {
                    assemblageActiveTile = tile.id;
                    if (assemblageTool == AssemblageTool::Erase) {
                        assemblageTool = AssemblageTool::Paint;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s (id %u)", tile.name.c_str(), tile.id);
                }
                ImGui::PopID();

                column = (column + 1) % perRow;
            }
        }
    }
    ImGui::EndChild();

    ImGui::End();
}
