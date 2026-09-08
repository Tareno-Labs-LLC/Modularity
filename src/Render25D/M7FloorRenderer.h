#pragma once

#include "Render25D/TMRenderBackend.h"

namespace Modularity::Render25D {

class M7FloorRenderer {
public:
    void buildDrawCommands(const TMScene& scene,
                           const TMVisibilityResult& visibility,
                           std::vector<M7FloorDrawCommand>& outCommands) const;
};

} // namespace Modularity::Render25D
