// ModuOBJ prefab edit mode.
//
// Opening a packaged object swaps the scene editor onto its contents: the same
// hierarchy, inspector, gizmos and viewport, pointed at the asset instead of the
// scene. Saving rebuilds the asset from whatever is in the scene and writes it
// back; closing restores the scene that was there before.
//
// Borrowing the whole scene editor rather than building a second one is the point.
// A packaged object IS a little scene - roots, children, components - so a
// dedicated editor would be the scene editor again with different variable names,
// and every feature added to one would have to be added to the other.
//
// The mechanism follows mesh edit mode, which already borrows the viewport this
// way: capture a SceneSnapshot on the way in, restore it on the way out.

#include "../Engine.h"
#include "../ModuObj.h"

#include <algorithm>

namespace {

// Roots of the scene as it stands: every object with no parent. In prefab mode
// these are exactly the asset's roots, because opening cleared everything else.
std::vector<int> CollectSceneRootIds(const std::vector<SceneObject>& objects) {
    std::vector<int> roots;
    for (const SceneObject& obj : objects) {
        if (obj.parentId == -1) roots.push_back(obj.id);
    }
    return roots;
}

}  // namespace

bool Engine::openModuObjEditor(const fs::path& assetPath) {
    if (moduObjEditMode) {
        addConsoleMessage("Already editing a packaged object. Close it first.",
                          ConsoleMessageType::Warning);
        return false;
    }
    if (meshEditMode) {
        addConsoleMessage("Leave mesh edit mode before opening a packaged object.",
                          ConsoleMessageType::Warning);
        return false;
    }

    std::string error;
    ModuObj::Asset asset;
    if (!ModuObj::LoadAsset(assetPath, asset, error)) {
        addConsoleMessage("Could not open " + assetPath.filename().string() + ": " + error,
                          ConsoleMessageType::Error);
        return false;
    }
    if (!asset.valid()) {
        addConsoleMessage("Packaged object " + assetPath.filename().string() + " is empty.",
                          ConsoleMessageType::Error);
        return false;
    }

    // The scene being left is kept whole, including selection and the id counter,
    // so closing puts the editor back exactly where it was rather than on a reloaded
    // approximation with different ids.
    moduObjEditReturnScene = captureSceneSnapshot();
    moduObjEditReturnValid = true;
    moduObjEditReturnUnsaved = projectManager.currentProject.hasUnsavedChanges;

    sceneObjects.clear();
    selectedObjectIds.clear();
    selectedObjectId = -1;
    nextObjectId = 1;

    ModuObj::InstanceResult placed = ModuObj::Instantiate(asset, nextObjectId);
    if (!placed.success) {
        addConsoleMessage("Could not lay out " + assetPath.filename().string() + ": " + placed.error,
                          ConsoleMessageType::Error);
        restoreSceneSnapshot(std::move(moduObjEditReturnScene));
        moduObjEditReturnValid = false;
        return false;
    }

    // Instantiate stamps every object with an instance ref so the scene can track a
    // spawned copy back to its source. Editing the source itself is the one case
    // where that linkage is wrong: leave it on and saving would bake an instance id
    // into the asset, making every future instance a copy of one particular
    // placement rather than of the asset.
    for (SceneObject& obj : placed.objects) {
        obj.moduObjInstance = ModuObjInstanceRef{};
    }

    sceneObjects = std::move(placed.objects);
    sceneObjectIndexById.clear();
    sceneObjectIndexData = nullptr;
    sceneObjectIndexCount = 0;
    markRuntimeScriptBindingsDirty();
    updateHierarchyWorldTransforms();

    moduObjEditMode = true;
    moduObjEditPath = assetPath;
    moduObjEditAssetId = asset.assetId;
    moduObjEditName = asset.name.empty() ? assetPath.stem().string() : asset.name;
    moduObjEditDirty = false;

    if (!placed.rootObjectIds.empty()) {
        selectedObjectId = placed.rootObjectIds.front();
        selectedObjectIds.assign(1, selectedObjectId);
    }

    addConsoleMessage("Editing packaged object \"" + moduObjEditName + "\" (" +
                          std::to_string(sceneObjects.size()) + " objects).",
                      ConsoleMessageType::Info);
    return true;
}

bool Engine::saveModuObjEdits() {
    if (!moduObjEditMode) return false;

    const std::vector<int> roots = CollectSceneRootIds(sceneObjects);
    if (roots.empty()) {
        addConsoleMessage("A packaged object needs at least one root object.",
                          ConsoleMessageType::Error);
        return false;
    }

    ModuObj::Asset rebuilt;
    std::vector<std::string> externalRefs;
    std::string error;
    if (!ModuObj::BuildAssetFromSelection(sceneObjects, roots, moduObjEditName, rebuilt,
                                          externalRefs, error)) {
        addConsoleMessage("Could not rebuild the packaged object: " + error,
                          ConsoleMessageType::Error);
        return false;
    }

    // Keep the original asset id. Building from a selection mints a new one, and a
    // new id would orphan every instance already placed in every scene - they
    // resolve by asset id, so the asset would silently become a different asset
    // that merely looks the same.
    if (!moduObjEditAssetId.empty()) rebuilt.assetId = moduObjEditAssetId;

    if (!ModuObj::SaveAsset(moduObjEditPath, rebuilt, error)) {
        addConsoleMessage("Could not save " + moduObjEditPath.filename().string() + ": " + error,
                          ConsoleMessageType::Error);
        return false;
    }

    // The cache hands out the previous contents by path, so an unflushed entry means
    // the next spawn uses the version from before this save.
    moduObjAssetCache.invalidate(moduObjEditPath);
    moduObjEditDirty = false;

    if (!externalRefs.empty()) {
        addConsoleMessage("Saved with " + std::to_string(externalRefs.size()) +
                              " reference(s) pointing outside the object; those will not resolve "
                              "when it is spawned elsewhere.",
                          ConsoleMessageType::Warning);
    }
    addConsoleMessage("Saved packaged object \"" + moduObjEditName + "\".",
                      ConsoleMessageType::Success);
    return true;
}

void Engine::closeModuObjEditor(bool saveFirst) {
    if (!moduObjEditMode) return;
    if (saveFirst && !saveModuObjEdits()) return;

    moduObjEditMode = false;
    moduObjEditDirty = false;
    moduObjEditPath.clear();
    moduObjEditAssetId.clear();
    moduObjEditName.clear();

    if (moduObjEditReturnValid) {
        restoreSceneSnapshot(std::move(moduObjEditReturnScene));
        moduObjEditReturnScene = SceneSnapshot{};
        moduObjEditReturnValid = false;
        // restoreSceneSnapshot does not know why the scene was swapped out, so the
        // scene's own dirty flag is put back by hand rather than inheriting whatever
        // editing the prefab set.
        projectManager.currentProject.hasUnsavedChanges = moduObjEditReturnUnsaved;
    } else {
        sceneObjects.clear();
        selectedObjectIds.clear();
        selectedObjectId = -1;
        sceneObjectIndexById.clear();
        sceneObjectIndexData = nullptr;
        sceneObjectIndexCount = 0;
        markRuntimeScriptBindingsDirty();
    }

    addConsoleMessage("Closed packaged object editor.", ConsoleMessageType::Info);
}

void Engine::renderModuObjEditorBar() {
    if (!moduObjEditMode) return;

    // A banner rather than a window: prefab mode replaces what the whole editor is
    // pointed at, and that has to be visible without hunting for a panel. The same
    // reason mesh edit mode announces itself.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 12.0f, viewport->WorkPos.y + 12.0f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##ModuObjEditBanner", nullptr, flags)) {
        ImGui::TextColored(ImVec4(0.45f, 0.78f, 1.0f, 1.0f), "Packaged Object");
        ImGui::SameLine();
        ImGui::TextUnformatted(moduObjEditName.c_str());
        if (moduObjEditDirty) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f), "*");
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::SmallButton("Save")) saveModuObjEdits();
        ImGui::SameLine();
        if (ImGui::SmallButton("Save & Close")) closeModuObjEditor(true);
        ImGui::SameLine();
        if (ImGui::SmallButton("Discard")) closeModuObjEditor(false);

        ImGui::TextDisabled("Editing the asset itself - changes apply to every instance of it.");
    }
    ImGui::End();
}

// Save the current selection as a packaged object asset - the prefab format the
// rest of the engine consumes.
//
// This is deliberately NOT the same thing as "Export As ModuOBJ". Two formats have
// always shared the .moduobj extension:
//
//   MODU_OBJASSET (text)  - what ModuObj::LoadAsset reads, and therefore what
//                           Instantiate, SpawnIntoScene, the asset cache, prefab
//                           edit mode and NetworkSession::spawn all resolve.
//   ZIP package           - what ModuObjExporter writes: a manifest, a scene and a
//                           Dependencies/ tree, for handing to someone else.
//
// Until now nothing in the editor wrote the first one, so the prefab system had no
// authoring entry point at all: a packaged object made through Export could not be
// opened for editing and could not be spawned over the network - the asset index
// skipped it as unreadable. This is that missing half.
bool Engine::createModuObjAssetFromSelection() {
    if (!projectManager.currentProject.isLoaded) {
        addConsoleMessage("Open a project before creating a packaged object.",
                          ConsoleMessageType::Warning);
        return false;
    }

    std::vector<int> ids = selectedObjectIds;
    if (ids.empty() && selectedObjectId >= 0) ids.push_back(selectedObjectId);
    if (ids.empty()) {
        addConsoleMessage("Select the object to package first.", ConsoleMessageType::Warning);
        return false;
    }

    std::string assetName = "PackagedObject";
    if (const SceneObject* primary = findObjectById(ids.front())) assetName = primary->name;

    ModuObj::Asset asset;
    std::vector<std::string> externalRefs;
    std::string error;
    if (!ModuObj::BuildAssetFromSelection(sceneObjects, ids, assetName, asset, externalRefs, error)) {
        addConsoleMessage("Could not package the selection: " + error, ConsoleMessageType::Error);
        return false;
    }

    fs::path outPath = projectManager.currentProject.assetsPath / (assetName + std::string(ModuObj::kAssetExtension));
    // Never silently replace a different asset that happens to share a name: a
    // packaged object is referenced by assetId, and overwriting one would break
    // every instance already placed.
    std::error_code ec;
    int suffix = 1;
    while (fs::exists(outPath, ec)) {
        outPath = projectManager.currentProject.assetsPath /
                  (assetName + " " + std::to_string(++suffix) + std::string(ModuObj::kAssetExtension));
    }

    if (!ModuObj::SaveAsset(outPath, asset, error)) {
        addConsoleMessage("Could not write " + outPath.filename().string() + ": " + error,
                          ConsoleMessageType::Error);
        return false;
    }

    moduObjAssetCache.invalidate(outPath);
    // The networking asset index is built by scanning for .moduobj files, so a new
    // one is invisible to Net.Spawn until it is rescanned.
    rebuildModuObjAssetIndex();

    if (!externalRefs.empty()) {
        addConsoleMessage("Packaged with " + std::to_string(externalRefs.size()) +
                              " reference(s) pointing outside the selection; those will not "
                              "resolve when it is spawned elsewhere.",
                          ConsoleMessageType::Warning);
    }
    addConsoleMessage("Created packaged object \"" + asset.name + "\" (assetId " + asset.assetId +
                          ") at " + outPath.filename().string(),
                      ConsoleMessageType::Success);
    return true;
}
