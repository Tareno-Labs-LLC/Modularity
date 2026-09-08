#pragma once

#include "Common.h"
#include "Render25D/TMPresentation.h"
#include "Skybox/Skybox.h"
#include <cstdint>
#include <limits>

namespace Modularity::Render25D {

constexpr uint32_t kInvalidSegmentIndex = std::numeric_limits<uint32_t>::max();

struct TMTileLayer {
    std::string name = "TileLayer";
    glm::ivec2 dimensions = glm::ivec2(0);
    glm::ivec2 tileSize = glm::ivec2(1);
    std::vector<int32_t> tileIds;
};

enum class TMFloorUvMode : uint32_t {
    Stretch = 0,
    TileRepeat = 1
};

struct TMFloorSurface {
    bool enabled = true;
    std::string texturePath;
    glm::vec2 uvScale = glm::vec2(1.0f);
    TMFloorUvMode uvMode = TMFloorUvMode::TileRepeat;
    glm::vec2 tileWorldSize = glm::vec2(1.0f);
    TMTextureFilter textureFilter = TMTextureFilter::Point;
    bool extendToHorizon = false;
    float height = 0.0f;
    float perspectiveStrength = 1.0f;
    float horizonOffset = 0.0f;
    float maxDistance = 96.0f;
};

struct TMSegmentPortal {
    uint32_t targetSegmentIndex = kInvalidSegmentIndex;
    glm::vec3 anchor = glm::vec3(0.0f);
    float activationRadius = 4.0f;
};

struct TMSegment {
    uint32_t id = 0;
    std::string name = "Segment";
    bool active = true;
    glm::vec3 boundsMin = glm::vec3(-1.0f);
    glm::vec3 boundsMax = glm::vec3(1.0f);
    float floorHeight = 0.0f;
    float ceilingHeight = 4.0f;
    std::vector<uint32_t> tileLayerIndices;
    std::vector<uint32_t> neighborSegmentIndices;
    std::vector<TMSegmentPortal> portals;
    TMFloorSurface floorSurface;
};

struct TMSectorPlacement {
    glm::ivec3 vexel = glm::ivec3(0);
    float heightStep = 1.0f;
    glm::vec3 localOffset = glm::vec3(0.0f);
};

struct TMSectorModelInstance {
    uint32_t segmentIndex = kInvalidSegmentIndex;
    std::string name = "SectorModel";
    std::string meshAssetPath;
    std::string textureOverridePath;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 rotationEuler = glm::vec3(0.0f);
    glm::vec3 scale = glm::vec3(1.0f);
    float baseHeight = 0.0f;
    bool enabled = true;
    TMSectorPlacement placement;
    TMSurfacePresentation presentation;
    TMWobbleSettings wobble;
};

struct TMSprite25DInstance {
    uint32_t segmentIndex = 0;
    int objectId = -1;
    std::string name = "Sprite25D";
    std::string texturePath;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec2 halfExtents = glm::vec2(0.5f);
    float rotationDegrees = 0.0f;
    glm::vec4 colorTint = glm::vec4(1.0f);
    glm::vec4 uvRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    TMTextureFilter textureFilter = TMTextureFilter::Point;
    int depthSort = 0;
    bool enabled = true;
};

struct TMScene {
    std::vector<TMTileLayer> tileLayers;
    std::vector<TMSegment> segments;
    std::vector<TMSectorModelInstance> sectorModels;
    std::vector<TMSprite25DInstance> sprites25D;
};

struct TMRenderContext {
    glm::mat4 view = glm::mat4(1.0f);
    glm::mat4 projection = glm::mat4(1.0f);
    glm::vec3 cameraPosition = glm::vec3(0.0f);
    glm::vec3 cameraForward = glm::vec3(0.0f, 0.0f, -1.0f);
    int viewportWidth = 0;
    int viewportHeight = 0;
    float maxVisibleDistance = 96.0f;
    bool allowPortalExpansion = true;
    float skyboxTimeOfDay = 0.5f;
    SkyboxSettings skyboxSettings;
    TMPresentationSettings presentationSettings;
};

struct TMVisibilityResult {
    std::vector<uint32_t> visibleSegmentIndices;
    std::vector<uint32_t> visibleModelIndices;
};

} // namespace Modularity::Render25D
