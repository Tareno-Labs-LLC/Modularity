#pragma once

#include "SceneObject.h"
#include "../include/Platform/AssetSource.h"

#include <filesystem>
#include <string>
#include <system_error>

// The asset paths on a scene object that runtime export stages into a build. Kept in one place so
// the exporter and the editor's missing-reference highlight agree on what "missing" means.
//
// Not exhaustive: text fonts, sprite sheets, particle textures, assemblage assets, PBR map paths
// and anything scripts load by path are not staged individually. They currently reach a build only
// because export copies each mesh's whole directory (see RuntimeExportStager).
enum class SceneAssetRefKind {
    File,
    DirectoryBacked,  // the mesh plus every file beside it
};

// Calls visit(pathString, kind, stagingCategory, label) for each non-optional reference; `Object`
// may be const. Stops and returns false as soon as a visit returns false.
template <typename Object, typename Visitor>
bool ForEachSceneObjectAssetReference(Object& obj, Visitor&& visit) {
    if (!visit(obj.materialPath, SceneAssetRefKind::File, "Materials", "material")) return false;
    if (!visit(obj.albedoTexturePath, SceneAssetRefKind::File, "Textures", "albedo texture")) return false;
    if (!visit(obj.overlayTexturePath, SceneAssetRefKind::File, "Textures", "overlay texture")) return false;
    if (!visit(obj.normalMapPath, SceneAssetRefKind::File, "Textures", "normal map")) return false;
    if (!visit(obj.shaderPackPath, SceneAssetRefKind::File, "Shaders", "shader pack")) return false;
    if (!visit(obj.vertexShaderPath, SceneAssetRefKind::File, "Shaders", "vertex shader")) return false;
    if (!visit(obj.fragmentShaderPath, SceneAssetRefKind::File, "Shaders", "fragment shader")) return false;
    if (!visit(obj.meshPath, SceneAssetRefKind::DirectoryBacked, "Models", "mesh")) return false;
    if (obj.hasAudioSource &&
        !visit(obj.audioSource.clipPath, SceneAssetRefKind::File, "Audio", "audio clip")) return false;
    if (obj.hasVideoPlayer &&
        !visit(obj.videoPlayer.videoPath, SceneAssetRefKind::File, "Video", "video")) return false;
    if (obj.hasLight2D &&
        !visit(obj.light2D.cookieTexturePath, SceneAssetRefKind::File, "Textures", "light cookie")) return false;
    if (obj.hasAnimation) {
        if (!visit(obj.animation.clipAssetPath, SceneAssetRefKind::File, "Animations", "animation clip")) return false;
        for (auto& clip : obj.animation.clips) {
            if (!visit(clip.assetPath, SceneAssetRefKind::File, "Animations", "animation clip")) return false;
        }
    }
    for (auto& materialRef : obj.additionalMaterialPaths) {
        if (!visit(materialRef, SceneAssetRefKind::File, "Materials", "extra material")) return false;
    }
    return true;
}

// True when a non-empty reference points at nothing, after the same foreign-project rescue the
// editor's loaders apply (so a path authored on another machine is not flagged if it resolves).
inline bool SceneAssetReferenceMissing(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    const std::string resolved = Modularity::Platform::ResolveAssetPath(path);
    return !std::filesystem::exists(std::filesystem::path(resolved), ec) || ec;
}
