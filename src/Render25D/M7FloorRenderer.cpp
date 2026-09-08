#include "Render25D/M7FloorRenderer.h"

namespace Modularity::Render25D {

void M7FloorRenderer::buildDrawCommands(const TMScene& scene,
                                        const TMVisibilityResult& visibility,
                                        std::vector<M7FloorDrawCommand>& outCommands) const {
    outCommands.clear();
    outCommands.reserve(visibility.visibleSegmentIndices.size());

    for (uint32_t segmentIndex : visibility.visibleSegmentIndices) {
        if (segmentIndex >= scene.segments.size()) {
            continue;
        }

        const TMSegment& segment = scene.segments[segmentIndex];
        if (!segment.floorSurface.enabled) {
            continue;
        }

        M7FloorDrawCommand command;
        command.segmentIndex = segmentIndex;
        command.boundsMin = segment.boundsMin;
        command.boundsMax = segment.boundsMax;
        command.floorSurface = segment.floorSurface;
        outCommands.push_back(std::move(command));
    }
}

} // namespace Modularity::Render25D
