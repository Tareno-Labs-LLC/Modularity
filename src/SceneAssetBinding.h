#pragma once

#include "ModelLoader.h"
#include "SceneObject.h"

#include <string>
#include <vector>

// Helpers for binding serialized asset paths to the runtime mesh ids used by the
// renderer. A scene load does this while streaming in scene objects, but any object
// that enters the scene later - most importantly a ModuOBJ spawned through
// Net.Spawn() - arrives with meshPath/meshSourceIndex set and meshId still at its
// default of -1. Without this pass those objects are invisible until the user
// presses Reload in the Inspector.
namespace SceneAssetBinding {

// Resolve one object's mesh from its stored path and submesh index. Out-of-range
// indices fall back to the model's first mesh, matching scene-load and Inspector
// reload behaviour.
inline bool ResolveMesh(SceneObject& obj, std::string* outError = nullptr) {
    if (outError) outError->clear();
    obj.meshId = -1;
    if (!obj.hasRenderer || obj.meshPath.empty()) return false;

    if (obj.renderType == RenderType::OBJMesh) {
        std::string error;
        obj.meshId = g_objLoader.loadOBJ(obj.meshPath, error);
        if (obj.meshId < 0 && outError) *outError = std::move(error);
        return obj.meshId >= 0;
    }

    if (obj.renderType != RenderType::Model) return false;

    ModelSceneData sceneData;
    std::string error;
    if (!getModelLoader().loadModelScene(obj.meshPath, sceneData, error)) {
        if (outError) *outError = std::move(error);
        return false;
    }

    int sourceIndex = obj.meshSourceIndex;
    if (sourceIndex < 0 || sourceIndex >= static_cast<int>(sceneData.meshIndices.size())) {
        sourceIndex = 0;
    }
    if (!sceneData.meshIndices.empty()) {
        obj.meshId = sceneData.meshIndices[static_cast<size_t>(sourceIndex)];
    }
    return obj.meshId >= 0;
}

// Resolve every model/mesh renderer in a freshly instantiated hierarchy. Returns
// the number of meshes resolved; the first error is reported through firstError so
// callers can log it without flooding the console once per submesh.
inline int ResolveMeshes(std::vector<SceneObject>& objects, std::string* firstError = nullptr) {
    if (firstError) firstError->clear();
    int resolved = 0;
    for (SceneObject& obj : objects) {
        if (!obj.hasRenderer) continue;
        if (obj.renderType != RenderType::OBJMesh && obj.renderType != RenderType::Model) continue;
        if (obj.meshPath.empty()) continue;

        std::string error;
        if (ResolveMesh(obj, &error)) {
            ++resolved;
        } else if (firstError && firstError->empty() && !error.empty()) {
            *firstError = std::move(error);
        }
    }
    return resolved;
}

}  // namespace SceneAssetBinding
