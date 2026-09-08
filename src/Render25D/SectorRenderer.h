#pragma once

#include "Render25D/MMeshLoader.h"
#include "Render25D/TMRenderBackend.h"

namespace Modularity::Render25D {

class SectorRenderer {
public:
    struct BuildStats {
        uint32_t submittedModels = 0;
        uint32_t rejectedModels = 0;
    };

    explicit SectorRenderer(MMeshCache* meshCache = nullptr);

    void setMeshCache(MMeshCache* meshCache);
    BuildStats buildDrawCommands(const TMRenderContext& context,
                                 const TMScene& scene,
                                 const TMVisibilityResult& visibility,
                                 std::vector<SectorModelDrawCommand>& outCommands,
                                 std::string& error) const;

private:
    MMeshCache* meshCache = nullptr;
};

} // namespace Modularity::Render25D
