#pragma once
#include "SceneObject.h"
#include "EditorUI.h"
#include <string>

// The component icon a SceneOBJ wears wherever it is listed: the Hierarchy tree, the drag
// ghost it leaves behind, and the Inspector's reference slots. Kept in one place so a
// reference to an object always shows the same icon its Hierarchy row does.
//
// `obj.editorIconPath` overrides this when the user picked a custom icon; callers fall back
// to this path when the custom one fails to load.
//
// Returns a reference to a static string: rows look their icon up every frame through
// Renderer::getTexture(const std::string&), and handing it a literal built a heap string
// for every row on every frame.
inline const std::string& SceneObjectComponentIconPath(const SceneObject& obj) {
    static const std::string kIconComponentCamera = "Resources/Engine-Root/Hierarchy/Component Camera.png";
    static const std::string kIconComponentLightBulb = "Resources/Engine-Root/Hierarchy/Component Light Bulb.png";
    static const std::string kIconComponentModuvolume = "Resources/Engine-Root/Hierarchy/Component ModuVolume.png";
    static const std::string kIconComponentCanvas = "Resources/Engine-Root/Hierarchy/Component Canvas.png";
    static const std::string kIconComponentText = "Resources/Engine-Root/Hierarchy/Component Text.png";
    static const std::string kIconComponentSprite = "Resources/Engine-Root/Hierarchy/Component Sprite.png";
    static const std::string kIconVideoPlayer = "Resources/Engine-Root/Hierarchy/Video Player.png";
    static const std::string kIconParticleSystemAnyType = "Resources/Engine-Root/Hierarchy/Particle System any type.png";
    static const std::string kIconComponentMesh = "Resources/Engine-Root/Hierarchy/Component Mesh.png";
    static const std::string kIconComponentAudioSource = "Resources/Engine-Root/Hierarchy/Component Audio Source.png";
    static const std::string kIconScript = "Resources/Engine-Root/Hierarchy/Script.png";
    static const std::string kIconRigidbody2DAnd3DType = "Resources/Engine-Root/Hierarchy/Rigidbody 2D and 3D type.png";
    static const std::string kIconColliderAnyType = "Resources/Engine-Root/Hierarchy/Collider any type.png";
    static const std::string kIconComponentDefaultOrUnknown = "Resources/Engine-Root/Hierarchy/Component Default or Unknown.png";
    if (obj.hasCamera) return kIconComponentCamera;
    if (obj.hasLight || obj.hasLight2D) return kIconComponentLightBulb;
    if (obj.hasPostFX) return kIconComponentModuvolume;
    if (obj.hasUI) {
        switch (obj.ui.type) {
            case UIElementType::Canvas: return kIconComponentCanvas;
            case UIElementType::Text: return kIconComponentText;
            case UIElementType::Image:
            case UIElementType::Sprite2D: return kIconComponentSprite;
            default: break;
        }
    }
    if (obj.hasVideoPlayer) return kIconVideoPlayer;
    if (obj.hasParticleSystem2D) return kIconParticleSystemAnyType;
    if (obj.hasRenderer) {
        switch (obj.renderType) {
            case RenderType::Sprite:
                return kIconComponentSprite;
            case RenderType::Cube:
            case RenderType::Sphere:
            case RenderType::Capsule:
            case RenderType::OBJMesh:
            case RenderType::Model:
            case RenderType::Mirror:
            case RenderType::Plane:
            case RenderType::Torus:
                return kIconComponentMesh;
            case RenderType::None:
            default:
                break;
        }
    }
    if (obj.hasAudioSource) return kIconComponentAudioSource;
    if (!obj.scripts.empty()) return kIconScript;
    if (obj.hasRigidbody || obj.hasRigidbody2D) return kIconRigidbody2DAnd3DType;
    if (obj.hasCollider || obj.hasCollider2D) return kIconColliderAnyType;
    return kIconComponentDefaultOrUnknown;
}

// The Project's file-type icon for an asset reference, matching what the File Explorer shows.
inline const char* AssetCategoryIconPath(FileCategory category) {
    switch (category) {
        case FileCategory::Folder:   return "Resources/Engine-Root/File Explorer/Folder Full.png";
        case FileCategory::Scene:    return "Resources/Engine-Root/File Explorer/File Icon Scenes.png";
        case FileCategory::Model:    return "Resources/Engine-Root/File Explorer/File Icon Model.png";
        case FileCategory::Material: return "Resources/Engine-Root/File Explorer/File Icon Material.png";
        case FileCategory::Video:    return "Resources/Engine-Root/File Explorer/File Icon Video File.png";
        case FileCategory::Audio:    return "Resources/Engine-Root/File Explorer/File Icon Audio File.png";
        case FileCategory::Script:   return "Resources/Engine-Root/File Explorer/File Icon Script.png";
        case FileCategory::Shader:
        case FileCategory::Text:     return "Resources/Engine-Root/File Explorer/File Icon Text.png";
        // Textures draw their own thumbnail; this is only the fallback when it cannot decode.
        case FileCategory::Texture:
        case FileCategory::Unknown:
        default:                     return "Resources/Engine-Root/File Explorer/File Icon Unknown or empty.png";
    }
}
