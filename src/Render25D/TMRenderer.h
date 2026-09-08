#pragma once

#include "Render25D/M7FloorRenderer.h"
#include "Render25D/SectorRenderer.h"

namespace Modularity::Render25D {

class TMRenderer {
public:
    struct VisibilityDebugModel {
        uint32_t modelIndex = 0;
        bool usedPresentationBounds = false;
        bool skippedFrustumCulling = false;
        bool rejectedByFrustum = false;
        glm::vec3 originalBoundsMin = glm::vec3(0.0f);
        glm::vec3 originalBoundsMax = glm::vec3(0.0f);
        glm::vec3 presentationBoundsMin = glm::vec3(0.0f);
        glm::vec3 presentationBoundsMax = glm::vec3(0.0f);
    };

    struct VisibilityDebugState {
        uint32_t frustumRejectedModels = 0;
        uint32_t skippedFrustumCullingModels = 0;
        uint32_t presentationBoundsModels = 0;
        std::vector<VisibilityDebugModel> models;
    };

    struct RenderStats {
        uint32_t visibleSegments = 0;
        uint32_t visibleModels = 0;
        uint32_t floorCommands = 0;
        uint32_t modelCommands = 0;
        uint32_t rejectedModels = 0;
        uint32_t frustumRejectedModels = 0;
        uint32_t skippedFrustumCullingModels = 0;
        uint32_t presentationBoundsModels = 0;
    };

    TMRenderer();

    MMeshCache& getMeshCache();
    const MMeshCache& getMeshCache() const;
    const VisibilityDebugState& getLastVisibilityDebugState() const;

    bool render(const TMRenderContext& context,
                const TMScene& scene,
                ITMRenderBackend& backend,
                RenderStats& outStats,
                std::string& error);

private:
    TMVisibilityResult buildVisibility(const TMRenderContext& context, const TMScene& scene);

    MMeshCache meshCache;
    M7FloorRenderer floorRenderer;
    SectorRenderer sectorRenderer;
    VisibilityDebugState lastVisibilityDebugState;
};

} // namespace Modularity::Render25D
