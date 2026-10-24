#pragma once
#if !MODULARITY_RUNTIME_ONLY
#include "SceneObject.h"
#include <functional>
#include <vector>

namespace EditorScenePicking {
// Shared by desktop image picking and the spatial controller pointer. The view
// determines camera-facing sprite geometry; the ray may start at a controller.
int PickRay(const std::vector<SceneObject>& objects, const glm::vec3& origin,
            const glm::vec3& direction, const glm::mat4& view,
            const std::function<bool(const SceneObject&)>& filter = {},
            float* hitDistance = nullptr);
}
#endif
