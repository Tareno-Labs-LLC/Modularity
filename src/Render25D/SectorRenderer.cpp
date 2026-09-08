#include "Render25D/SectorRenderer.h"

#include <algorithm>

namespace Modularity::Render25D {

namespace {

glm::mat4 BuildModelMatrix(const TMSectorModelInstance& instance) {
    glm::vec3 worldPosition = instance.position;
    worldPosition += instance.placement.localOffset;
    worldPosition.y += instance.baseHeight + (static_cast<float>(instance.placement.vexel.y) * instance.placement.heightStep);

    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, worldPosition);
    model = glm::rotate(model, glm::radians(instance.rotationEuler.x), glm::vec3(1.0f, 0.0f, 0.0f));
    model = glm::rotate(model, glm::radians(instance.rotationEuler.y), glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, glm::radians(instance.rotationEuler.z), glm::vec3(0.0f, 0.0f, 1.0f));
    model = glm::scale(model, instance.scale);
    return model;
}

float ComputeSortDepth(const TMRenderContext& context, const TMSectorModelInstance& instance) {
    glm::vec3 worldPosition = instance.position;
    worldPosition += instance.placement.localOffset;
    worldPosition.y += instance.baseHeight + (static_cast<float>(instance.placement.vexel.y) * instance.placement.heightStep);
    return glm::dot(worldPosition - context.cameraPosition, context.cameraForward);
}

} // namespace

SectorRenderer::SectorRenderer(MMeshCache* meshCacheIn)
    : meshCache(meshCacheIn) {}

void SectorRenderer::setMeshCache(MMeshCache* meshCacheIn) {
    meshCache = meshCacheIn;
}

SectorRenderer::BuildStats SectorRenderer::buildDrawCommands(const TMRenderContext& context,
                                                             const TMScene& scene,
                                                             const TMVisibilityResult& visibility,
                                                             std::vector<SectorModelDrawCommand>& outCommands,
                                                             std::string& error) const {
    BuildStats stats;
    outCommands.clear();
    outCommands.reserve(visibility.visibleModelIndices.size());

    if (meshCache == nullptr) {
        error = "SectorRenderer has no MMesh cache";
        stats.rejectedModels = static_cast<uint32_t>(visibility.visibleModelIndices.size());
        return stats;
    }

    for (uint32_t modelIndex : visibility.visibleModelIndices) {
        if (modelIndex >= scene.sectorModels.size()) {
            ++stats.rejectedModels;
            continue;
        }

        const TMSectorModelInstance& instance = scene.sectorModels[modelIndex];
        if (!instance.enabled || instance.meshAssetPath.empty()) {
            ++stats.rejectedModels;
            continue;
        }

        std::string loadError;
        const MMeshRenderData* mesh = meshCache->getOrLoad(instance.meshAssetPath, loadError);
        if (mesh == nullptr) {
            if (!error.empty()) {
                error += '\n';
            }
            error += loadError;
            ++stats.rejectedModels;
            continue;
        }

        SectorModelDrawCommand command;
        command.segmentIndex = instance.segmentIndex;
        command.modelMatrix = BuildModelMatrix(instance);
        command.mesh = mesh;
        command.textureOverridePath = instance.textureOverridePath;
        command.presentation = instance.presentation;
        command.presentation.colorTint = glm::clamp(command.presentation.colorTint, glm::vec4(0.0f), glm::vec4(1.0f));
        command.presentation.affineTextureWarpStrength =
            std::clamp(command.presentation.affineTextureWarpStrength, 0.0f, 1.0f);
        command.wobble = instance.wobble;
        command.wobble.strength = std::max(0.0f, command.wobble.strength);
        command.wobble.speed = std::max(0.0f, command.wobble.speed);
        command.sortDepth = ComputeSortDepth(context, instance);
        command.worldBaseHeight = instance.position.y +
                                  instance.baseHeight +
                                  instance.placement.localOffset.y +
                                  (static_cast<float>(instance.placement.vexel.y) * instance.placement.heightStep);
        outCommands.push_back(std::move(command));
        ++stats.submittedModels;
    }

    std::sort(outCommands.begin(), outCommands.end(),
              [](const SectorModelDrawCommand& a, const SectorModelDrawCommand& b) {
                  if (a.segmentIndex != b.segmentIndex) {
                      return a.segmentIndex < b.segmentIndex;
                  }
                  if (std::abs(a.worldBaseHeight - b.worldBaseHeight) > 0.001f) {
                      return a.worldBaseHeight < b.worldBaseHeight;
                  }
                  return a.sortDepth < b.sortDepth;
              });

    return stats;
}

} // namespace Modularity::Render25D
