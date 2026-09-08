#include "Render25D/TMSceneBuilder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>

namespace Modularity::Render25D {

namespace {

std::string ToLowerCopy(const std::string& value) {
    std::string result = value;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool IsPseudo3DOverlayObject(const SceneObject& obj) {
    return obj.type == ObjectType::Sprite25D;
}

bool IsWorldRenderableObject(const SceneObject& obj) {
    if (!IsObjectEnabledInHierarchy(obj)) {
        return false;
    }
    if (HasUIComponent(obj) && !IsPseudo3DOverlayObject(obj)) {
        return false;
    }
    return true;
}

bool IsSprite25DImageObject(const SceneObject& obj) {
    return IsWorldRenderableObject(obj) &&
           obj.type == ObjectType::Sprite25D &&
           obj.hasUI &&
           (obj.ui.type == UIElementType::Image || obj.ui.type == UIElementType::Sprite2D);
}

bool IsMMeshObject(const SceneObject& obj) {
    if (!IsWorldRenderableObject(obj) || !obj.hasRenderer) {
        return false;
    }
    if (obj.renderType != RenderType::Model && obj.renderType != RenderType::OBJMesh) {
        return false;
    }
    return ToLowerCopy(fs::path(obj.meshPath).extension().string()) == ".mmesh";
}

bool IsFloorPlaneObject(const SceneObject& obj) {
    return IsWorldRenderableObject(obj) &&
           obj.hasRenderer &&
           obj.renderType == RenderType::Plane;
}

glm::vec3 ResolveObjectHalfExtents(const SceneObject& obj) {
    const glm::vec3 absoluteScale = glm::abs(obj.scale);
    return glm::max(absoluteScale * 0.5f, glm::vec3(0.5f));
}

glm::vec2 ResolvePlaneHalfExtents(const SceneObject& obj) {
    std::array<float, 3> components = {
        std::abs(obj.scale.x),
        std::abs(obj.scale.y),
        std::abs(obj.scale.z)
    };
    std::sort(components.begin(), components.end(), std::greater<float>());
    return glm::vec2(std::max(0.5f, components[0] * 0.5f),
                     std::max(0.5f, components[1] * 0.5f));
}

TMTextureFilter ToTMTextureFilter(MaterialProperties::TextureFilter filter) {
    return (filter == MaterialProperties::TextureFilter::Point)
               ? TMTextureFilter::Point
               : TMTextureFilter::Bilinear;
}

float BuildStableWobbleSeed(const SceneObject& obj) {
    const uint32_t id = static_cast<uint32_t>(std::max(0, obj.id));
    return static_cast<float>((id * 747796405u) ^ 2891336453u) * 0.0000001f;
}

bool HasMeaningfulSpriteFrameScales(const UIElementComponent& ui) {
    if (ui.spriteCustomFrameScales.size() != ui.spriteCustomFrames.size() ||
        ui.spriteCustomFrameScales.empty()) {
        return false;
    }
    for (const glm::vec2& scale : ui.spriteCustomFrameScales) {
        if (std::abs(scale.x - 1.0f) > 0.0001f ||
            std::abs(scale.y - 1.0f) > 0.0001f) {
            return true;
        }
    }
    return false;
}

glm::vec2 ResolveSpriteFrameScale(const UIElementComponent& ui) {
    if (!ui.spriteCustomFramesEnabled || ui.spriteCustomFrames.empty()) {
        return glm::vec2(1.0f);
    }

    const int frameCount = static_cast<int>(ui.spriteCustomFrames.size());
    const int frame = std::clamp(ui.spriteSheetFrame, 0, frameCount - 1);
    if (HasMeaningfulSpriteFrameScales(ui)) {
        const glm::vec2 authored = ui.spriteCustomFrameScales[static_cast<size_t>(frame)];
        return glm::vec2(std::max(0.01f, authored.x), std::max(0.01f, authored.y));
    }

    const glm::ivec4& referenceRect = ui.spriteCustomFrames.front();
    const glm::ivec4& frameRect = ui.spriteCustomFrames[static_cast<size_t>(frame)];
    const float referenceWidth = static_cast<float>(std::max(1, referenceRect.z));
    const float referenceHeight = static_cast<float>(std::max(1, referenceRect.w));
    return glm::vec2(static_cast<float>(std::max(1, frameRect.z)) / referenceWidth,
                     static_cast<float>(std::max(1, frameRect.w)) / referenceHeight);
}

glm::vec4 BuildSpriteUvRect(const UIElementComponent& ui) {
    if (ui.spriteCustomFramesEnabled &&
        !ui.spriteCustomFrames.empty() &&
        ui.spriteSourceWidth > 0 &&
        ui.spriteSourceHeight > 0) {
        const int frame = std::clamp(ui.spriteSheetFrame, 0,
                                     static_cast<int>(ui.spriteCustomFrames.size()) - 1);
        const glm::ivec4 rect = ui.spriteCustomFrames[static_cast<size_t>(frame)];
        const float invW = 1.0f / static_cast<float>(ui.spriteSourceWidth);
        const float invH = 1.0f / static_cast<float>(ui.spriteSourceHeight);
        const float u0 = rect.x * invW;
        const float vBottom = 1.0f - static_cast<float>(rect.y + rect.w) * invH;
        return glm::vec4(u0, vBottom, rect.z * invW, rect.w * invH);
    }

    if (!ui.spriteSheetEnabled) {
        return glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    }

    const int columns = std::max(1, ui.spriteSheetColumns);
    const int rows = std::max(1, ui.spriteSheetRows);
    const int total = std::max(1, columns * rows);
    const int frame = std::clamp(ui.spriteSheetFrame, 0, total - 1);
    const int col = frame % columns;
    const int row = frame / columns;
    return glm::vec4(static_cast<float>(col) / static_cast<float>(columns),
                     1.0f - static_cast<float>(row + 1) / static_cast<float>(rows),
                     1.0f / static_cast<float>(columns),
                     1.0f / static_cast<float>(rows));
}

} // namespace

void TMSceneBuilder::buildFromSceneObjects(const std::vector<SceneObject>& sceneObjects,
                                           TMScene& outScene,
                                           BuildStats& outStats) const {
    outScene = TMScene();
    outStats = BuildStats();

    TMSegment segment;
    segment.id = 0;
    segment.name = "DefaultSegment";
    segment.floorSurface.enabled = false;
    segment.floorSurface.height = 0.0f;
    segment.floorSurface.uvScale = glm::vec2(0.125f);
    segment.floorSurface.uvMode = TMFloorUvMode::TileRepeat;
    segment.floorSurface.tileWorldSize = glm::vec2(1.0f);
    segment.floorSurface.textureFilter = TMTextureFilter::Point;
    segment.floorSurface.extendToHorizon = true;
    segment.floorSurface.maxDistance = 128.0f;
    segment.floorSurface.perspectiveStrength = 1.0f;

    glm::vec3 boundsMin(FLT_MAX);
    glm::vec3 boundsMax(-FLT_MAX);
    bool hasBounds = false;

    for (const SceneObject& obj : sceneObjects) {
        if (!IsWorldRenderableObject(obj)) {
            continue;
        }

        const glm::vec3 halfExtents = ResolveObjectHalfExtents(obj);
        boundsMin = glm::min(boundsMin, obj.position - halfExtents - glm::vec3(2.0f, 0.0f, 2.0f));
        boundsMax = glm::max(boundsMax, obj.position + halfExtents + glm::vec3(2.0f, 2.0f, 2.0f));
        hasBounds = true;

        if (!IsFloorPlaneObject(obj)) {
            continue;
        }

        outStats.hasExplicitFloor = true;
        segment.floorSurface.height = obj.position.y;
        if (segment.floorSurface.texturePath.empty() && !obj.albedoTexturePath.empty()) {
            segment.floorSurface.texturePath = obj.albedoTexturePath;
            segment.floorSurface.textureFilter = ToTMTextureFilter(obj.material.textureFilter);
        }

        segment.floorSurface.enabled = true;
        const glm::vec2 planeHalfExtents = ResolvePlaneHalfExtents(obj);
        boundsMin.x = std::min(boundsMin.x, obj.position.x - planeHalfExtents.x);
        boundsMin.z = std::min(boundsMin.z, obj.position.z - planeHalfExtents.y);
        boundsMax.x = std::max(boundsMax.x, obj.position.x + planeHalfExtents.x);
        boundsMax.z = std::max(boundsMax.z, obj.position.z + planeHalfExtents.y);
        segment.floorSurface.uvScale = glm::vec2(
            1.0f / std::max(1.0f, planeHalfExtents.x * 2.0f),
            1.0f / std::max(1.0f, planeHalfExtents.y * 2.0f));
    }

    if (!hasBounds) {
        boundsMin = glm::vec3(-16.0f, -2.0f, -16.0f);
        boundsMax = glm::vec3(16.0f, 8.0f, 16.0f);
    }

    segment.boundsMin = glm::vec3(boundsMin.x, std::min(boundsMin.y, segment.floorSurface.height - 2.0f), boundsMin.z);
    segment.boundsMax = glm::vec3(boundsMax.x, std::max(boundsMax.y, segment.floorSurface.height + 8.0f), boundsMax.z);
    outScene.segments.push_back(segment);
    outStats.segmentCount = 1;

    for (const SceneObject& obj : sceneObjects) {
        if (!IsMMeshObject(obj)) {
            continue;
        }

        TMSectorModelInstance instance;
        instance.segmentIndex = 0;
        instance.name = obj.name.empty() ? "SectorModel" : obj.name;
        instance.meshAssetPath = obj.meshPath;
        instance.textureOverridePath = obj.albedoTexturePath;
        instance.position = obj.position;
        instance.rotationEuler = obj.rotation;
        instance.scale = glm::max(glm::abs(obj.scale), glm::vec3(0.0001f));
        instance.baseHeight = 0.0f;
        instance.enabled = true;
        instance.presentation.colorTint = glm::vec4(obj.material.color, obj.material.alpha);
        instance.presentation.textureFilter = ToTMTextureFilter(obj.material.textureFilter);
        instance.presentation.uvTiling = obj.material.uvTiling;
        instance.presentation.uvOffset = obj.material.uvOffset;
        instance.wobble.seed = BuildStableWobbleSeed(obj);
        instance.wobble.offset = obj.position;
        outScene.sectorModels.push_back(std::move(instance));
    }

    outStats.modelCount = static_cast<uint32_t>(outScene.sectorModels.size());

    for (const SceneObject& obj : sceneObjects) {
        if (!IsSprite25DImageObject(obj)) {
            continue;
        }

        const glm::vec2 baseSize =
            glm::max(obj.ui.size, glm::vec2(0.01f)) * ResolveSpriteFrameScale(obj.ui);
        const glm::vec3 objectScale = glm::max(glm::abs(obj.scale), glm::vec3(0.01f));

        TMSprite25DInstance sprite;
        sprite.segmentIndex = 0;
        sprite.objectId = obj.id;
        sprite.name = obj.name.empty() ? "Sprite25D" : obj.name;
        sprite.texturePath = obj.albedoTexturePath;
        sprite.position = obj.position;
        sprite.halfExtents = glm::max(glm::vec2(baseSize.x * objectScale.x,
                                                baseSize.y * objectScale.y) * 0.005f,
                                      glm::vec2(0.001f));
        sprite.rotationDegrees = obj.ui.rotation;
        sprite.colorTint = glm::clamp(obj.ui.color, glm::vec4(0.0f), glm::vec4(1.0f));
        sprite.uvRect = BuildSpriteUvRect(obj.ui);
        sprite.textureFilter = TMTextureFilter::Point;
        sprite.depthSort = obj.ui.pseudo3DDepthSort;
        outScene.sprites25D.push_back(std::move(sprite));
    }
}

} // namespace Modularity::Render25D
