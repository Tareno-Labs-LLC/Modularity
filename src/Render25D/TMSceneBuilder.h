#pragma once

#include "Render25D/TMScene.h"
#include "SceneObject.h"

namespace Modularity::Render25D {

class TMSceneBuilder {
public:
    struct BuildStats {
        uint32_t segmentCount = 0;
        uint32_t modelCount = 0;
        bool hasExplicitFloor = false;
    };

    void buildFromSceneObjects(const std::vector<SceneObject>& sceneObjects,
                               TMScene& outScene,
                               BuildStats& outStats) const;
};

} // namespace Modularity::Render25D
