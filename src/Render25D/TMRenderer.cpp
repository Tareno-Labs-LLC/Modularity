#include "Render25D/TMRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>

namespace Modularity::Render25D {

namespace {

struct WorldBounds {
    glm::vec3 min = glm::vec3(FLT_MAX);
    glm::vec3 max = glm::vec3(-FLT_MAX);
};

std::array<glm::vec3, 8> BuildAABBCorners(const glm::vec3& minBounds, const glm::vec3& maxBounds) {
    return {
        glm::vec3(minBounds.x, minBounds.y, minBounds.z),
        glm::vec3(maxBounds.x, minBounds.y, minBounds.z),
        glm::vec3(minBounds.x, maxBounds.y, minBounds.z),
        glm::vec3(maxBounds.x, maxBounds.y, minBounds.z),
        glm::vec3(minBounds.x, minBounds.y, maxBounds.z),
        glm::vec3(maxBounds.x, minBounds.y, maxBounds.z),
        glm::vec3(minBounds.x, maxBounds.y, maxBounds.z),
        glm::vec3(maxBounds.x, maxBounds.y, maxBounds.z)
    };
}

glm::vec3 SnapVec3(const glm::vec3& value, float step) {
    const float safeStep = std::max(step, 0.0001f);
    return glm::floor((value / safeStep) + glm::vec3(0.5f)) * safeStep;
}

glm::vec3 ComputeBoundsCenter(const TMSegment& segment) {
    return (segment.boundsMin + segment.boundsMax) * 0.5f;
}

float ComputeBoundsRadius(const TMSegment& segment) {
    return glm::length(segment.boundsMax - segment.boundsMin) * 0.5f;
}

bool PassesDistanceVisibility(const TMRenderContext& context, const glm::vec3& center, float radius) {
    const float distance = glm::length(center - context.cameraPosition);
    return distance <= (context.maxVisibleDistance + radius);
}

bool HasPresentationDeformation(const TMRenderContext& context, const TMSectorModelInstance& model) {
    const TMPresentationSettings& settings = context.presentationSettings;
    if (settings.lookPitchStretchEnabled) {
        return true;
    }
    if (settings.presentationSnapEnabled || settings.cameraRelativeSnapEnabled ||
        settings.vertexSnapEnabled || settings.screenSnapEnabled) {
        return true;
    }
    if (settings.vertexWobbleEnabled && model.wobble.enabled &&
        model.wobble.strength > 0.0f && model.wobble.speed >= 0.0f) {
        return true;
    }
    return false;
}

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

WorldBounds ComputeWorldBounds(const glm::mat4& transform,
                               const glm::vec3& localMin,
                               const glm::vec3& localMax) {
    WorldBounds bounds;
    for (const glm::vec3& corner : BuildAABBCorners(localMin, localMax)) {
        const glm::vec3 world = glm::vec3(transform * glm::vec4(corner, 1.0f));
        bounds.min = glm::min(bounds.min, world);
        bounds.max = glm::max(bounds.max, world);
    }
    return bounds;
}

glm::vec4 BuildClipSlack(const TMRenderContext& context) {
    if (!context.presentationSettings.screenSnapEnabled || context.presentationSettings.screenSnapStep <= 0.0f) {
        return glm::vec4(0.0f);
    }

    const float safeWidth = static_cast<float>(std::max(1, context.viewportWidth));
    const float safeHeight = static_cast<float>(std::max(1, context.viewportHeight));
    const float ndcSlackX = (2.0f * context.presentationSettings.screenSnapStep) / safeWidth;
    const float ndcSlackY = (2.0f * context.presentationSettings.screenSnapStep) / safeHeight;
    return glm::vec4(ndcSlackX, ndcSlackX, ndcSlackY, ndcSlackY);
}

bool ClipCornersIntersectFrustum(const std::array<glm::vec4, 8>& clipCorners,
                                 const glm::vec4& planeSlack) {
    bool allOutsideLeft = true;
    bool allOutsideRight = true;
    bool allOutsideBottom = true;
    bool allOutsideTop = true;
    bool allOutsideNear = true;
    bool allOutsideFar = true;

    for (const glm::vec4& clip : clipCorners) {
        const float absW = std::max(std::abs(clip.w), 0.0001f);
        const float slackX = planeSlack.x * absW;
        const float slackY = planeSlack.z * absW;

        allOutsideLeft   &= (clip.x < (-clip.w - slackX));
        allOutsideRight  &= (clip.x > ( clip.w + slackX));
        allOutsideBottom &= (clip.y < (-clip.w - slackY));
        allOutsideTop    &= (clip.y > ( clip.w + slackY));
        allOutsideNear   &= (clip.z < -clip.w);
        allOutsideFar    &= (clip.z >  clip.w);
    }

    return !(allOutsideLeft || allOutsideRight || allOutsideBottom ||
             allOutsideTop || allOutsideNear || allOutsideFar);
}

bool SegmentIntersectsFrustum(const TMRenderContext& context, const TMSegment& segment) {
    std::array<glm::vec4, 8> clipCorners{};
    const glm::mat4 viewProjection = context.projection * context.view;
    const auto corners = BuildAABBCorners(segment.boundsMin, segment.boundsMax);
    for (size_t i = 0; i < corners.size(); ++i) {
        clipCorners[i] = viewProjection * glm::vec4(corners[i], 1.0f);
    }
    return ClipCornersIntersectFrustum(clipCorners, glm::vec4(0.0f));
}

struct ModelVisibilityResult {
    bool visible = true;
    bool usedPresentationBounds = false;
    bool skippedFrustumCulling = false;
    WorldBounds originalBounds;
    WorldBounds presentationBounds;
};

ModelVisibilityResult EvaluateModelVisibility(const TMRenderContext& context,
                                              const TMSectorModelInstance& model,
                                              const MMeshRenderData& mesh) {
    ModelVisibilityResult result;
    const glm::mat4 modelMatrix = BuildModelMatrix(model);
    result.originalBounds = ComputeWorldBounds(modelMatrix, mesh.boundsMin, mesh.boundsMax);

    const glm::vec3 originalCenter = (result.originalBounds.min + result.originalBounds.max) * 0.5f;
    const float originalRadius = glm::length(result.originalBounds.max - originalCenter);
    if (!PassesDistanceVisibility(context, originalCenter, std::max(originalRadius, 0.001f))) {
        result.visible = false;
        return result;
    }

    const bool deformed = HasPresentationDeformation(context, model);
    const TMPresentationSettings& settings = context.presentationSettings;
    if (!settings.presentationAwareCullingEnabled || !deformed) {
        result.presentationBounds = result.originalBounds;
        return result;
    }

    result.usedPresentationBounds = true;

    if (settings.skipFrustumCullingForDeformedObjects) {
        result.presentationBounds = result.originalBounds;
        result.skippedFrustumCulling = true;
        return result;
    }

    glm::vec3 localMin = mesh.boundsMin;
    glm::vec3 localMax = mesh.boundsMax;

    if (settings.vertexSnapEnabled && settings.vertexSnapStep > 0.0f) {
        const glm::vec3 snapPad(settings.vertexSnapStep * 0.5f);
        localMin -= snapPad;
        localMax += snapPad;
    }

    if (settings.vertexWobbleEnabled && model.wobble.enabled && model.wobble.strength > 0.0f) {
        const float wobbleStrength = model.wobble.strength * std::max(0.0f, settings.wobbleStrength);
        const glm::vec3 wobblePad(wobbleStrength * 0.45f,
                                  wobbleStrength * 0.65f,
                                  wobbleStrength * 0.45f);
        localMin -= wobblePad;
        localMax += wobblePad;
    }

    const glm::mat4 inverseView = glm::inverse(context.view);
    const glm::vec3 safeForward = (glm::dot(context.cameraForward, context.cameraForward) > 1e-6f)
                                      ? glm::normalize(context.cameraForward)
                                      : glm::vec3(0.0f, 0.0f, -1.0f);
    const float pitchNormalizeDegrees =
        std::max(1.0f, std::max(std::abs(settings.presentationPitchMinDegrees),
                                std::abs(settings.presentationPitchMaxDegrees)));
    const float rawPitchDegrees = glm::degrees(std::asin(glm::clamp(safeForward.y, -1.0f, 1.0f)));
    const float clampedPitchDegrees = glm::clamp(
        rawPitchDegrees,
        std::min(settings.presentationPitchMinDegrees, settings.presentationPitchMaxDegrees),
        std::max(settings.presentationPitchMinDegrees, settings.presentationPitchMaxDegrees));
    float pitchNormalized = glm::clamp(clampedPitchDegrees / pitchNormalizeDegrees, -1.0f, 1.0f);
    pitchNormalized = (pitchNormalized >= 0.0f ? 1.0f : -1.0f) *
                      std::pow(std::abs(pitchNormalized), std::max(0.01f, settings.lookPitchCurve));
    const float pitchUp = std::max(pitchNormalized, 0.0f);
    const float pitchDown = std::max(-pitchNormalized, 0.0f);
    const glm::vec4 clipSlack = BuildClipSlack(context);

    std::array<glm::vec4, 8> clipCorners{};
    size_t cornerIndex = 0;
    for (const glm::vec3& localCorner : BuildAABBCorners(localMin, localMax)) {
        glm::vec3 worldCorner = glm::vec3(modelMatrix * glm::vec4(localCorner, 1.0f));
        if (settings.presentationSnapEnabled && settings.presentationSnapStep > 0.0f) {
            worldCorner = SnapVec3(worldCorner, settings.presentationSnapStep);
        }

        glm::vec4 viewCorner = context.view * glm::vec4(worldCorner, 1.0f);
        if (settings.cameraRelativeSnapEnabled && settings.cameraRelativeSnapStep > 0.0f) {
            viewCorner = glm::vec4(SnapVec3(glm::vec3(viewCorner), settings.cameraRelativeSnapStep), viewCorner.w);
        }

        if (settings.lookPitchStretchEnabled) {
            const float depthWeight =
                glm::clamp((-viewCorner.z) / std::max(0.001f, settings.lookPitchDepthRange), 0.0f, 1.0f);
            const float verticalScale = 1.0f +
                                        (pitchUp * std::max(0.0f, settings.lookPitchStretchStrength)) -
                                        (pitchDown * std::max(0.0f, settings.lookPitchCompressStrength));
            const float shearAmount = pitchNormalized *
                                      std::max(0.0f, settings.lookPitchShearStrength) *
                                      (0.45f + depthWeight * 0.55f);
            viewCorner.y *= verticalScale;
            viewCorner.y += viewCorner.x * shearAmount;
        }

        const glm::vec3 presentedWorldCorner = glm::vec3(inverseView * viewCorner);
        result.presentationBounds.min = glm::min(result.presentationBounds.min, presentedWorldCorner);
        result.presentationBounds.max = glm::max(result.presentationBounds.max, presentedWorldCorner);
        clipCorners[cornerIndex++] = context.projection * viewCorner;
    }

    if (settings.inflatePresentationBounds) {
        const glm::vec3 center = (result.presentationBounds.min + result.presentationBounds.max) * 0.5f;
        glm::vec3 extents = (result.presentationBounds.max - result.presentationBounds.min) * 0.5f;
        extents *= std::max(1.0f, settings.presentationBoundsScale);
        extents += glm::vec3(std::max(0.0f, settings.presentationBoundsPadding));
        result.presentationBounds.min = center - extents;
        result.presentationBounds.max = center + extents;
    }

    result.visible = ClipCornersIntersectFrustum(clipCorners, clipSlack);
    return result;
}

} // namespace

TMRenderer::TMRenderer()
    : sectorRenderer(&meshCache) {}

MMeshCache& TMRenderer::getMeshCache() {
    return meshCache;
}

const MMeshCache& TMRenderer::getMeshCache() const {
    return meshCache;
}

const TMRenderer::VisibilityDebugState& TMRenderer::getLastVisibilityDebugState() const {
    return lastVisibilityDebugState;
}

TMVisibilityResult TMRenderer::buildVisibility(const TMRenderContext& context, const TMScene& scene) {
    TMVisibilityResult visibility;
    lastVisibilityDebugState = VisibilityDebugState();

    std::unordered_set<uint32_t> visibleSegmentSet;
    for (uint32_t segmentIndex = 0; segmentIndex < static_cast<uint32_t>(scene.segments.size()); ++segmentIndex) {
        const TMSegment& segment = scene.segments[segmentIndex];
        if (!segment.active) {
            continue;
        }

        const glm::vec3 center = ComputeBoundsCenter(segment);
        const float radius = ComputeBoundsRadius(segment);
        if (!PassesDistanceVisibility(context, center, radius)) {
            continue;
        }
        if (!SegmentIntersectsFrustum(context, segment)) {
            continue;
        }
        visibleSegmentSet.insert(segmentIndex);
    }

    for (uint32_t modelIndex = 0; modelIndex < static_cast<uint32_t>(scene.sectorModels.size()); ++modelIndex) {
        if (modelIndex >= scene.sectorModels.size()) {
            continue;
        }

        const TMSectorModelInstance& model = scene.sectorModels[modelIndex];
        if (!model.enabled || model.segmentIndex >= scene.segments.size()) {
            continue;
        }
        if (!scene.segments[model.segmentIndex].active) {
            continue;
        }

        const bool isPresentationDeformed = HasPresentationDeformation(context, model);
        if (!isPresentationDeformed &&
            visibleSegmentSet.find(model.segmentIndex) == visibleSegmentSet.end()) {
            continue;
        }

        std::string loadError;
        const MMeshRenderData* mesh = meshCache.getOrLoad(model.meshAssetPath, loadError);
        if (mesh == nullptr) {
            if (visibleSegmentSet.find(model.segmentIndex) != visibleSegmentSet.end()) {
                visibility.visibleModelIndices.push_back(modelIndex);
            }
            continue;
        }

        const ModelVisibilityResult modelVisibility = EvaluateModelVisibility(context, model, *mesh);

        VisibilityDebugModel debugModel;
        debugModel.modelIndex = modelIndex;
        debugModel.usedPresentationBounds = modelVisibility.usedPresentationBounds;
        debugModel.skippedFrustumCulling = modelVisibility.skippedFrustumCulling;
        debugModel.rejectedByFrustum = !modelVisibility.visible;
        debugModel.originalBoundsMin = modelVisibility.originalBounds.min;
        debugModel.originalBoundsMax = modelVisibility.originalBounds.max;
        debugModel.presentationBoundsMin = modelVisibility.presentationBounds.min;
        debugModel.presentationBoundsMax = modelVisibility.presentationBounds.max;
        lastVisibilityDebugState.models.push_back(std::move(debugModel));

        if (modelVisibility.usedPresentationBounds) {
            ++lastVisibilityDebugState.presentationBoundsModels;
        }
        if (modelVisibility.skippedFrustumCulling) {
            ++lastVisibilityDebugState.skippedFrustumCullingModels;
        }
        if (!modelVisibility.visible) {
            ++lastVisibilityDebugState.frustumRejectedModels;
            continue;
        }

        visibility.visibleModelIndices.push_back(modelIndex);
        visibleSegmentSet.insert(model.segmentIndex);
    }

    visibility.visibleSegmentIndices.assign(visibleSegmentSet.begin(), visibleSegmentSet.end());
    std::sort(visibility.visibleSegmentIndices.begin(), visibility.visibleSegmentIndices.end(),
              [&scene, &context](uint32_t a, uint32_t b) {
                  const float distA = glm::length(ComputeBoundsCenter(scene.segments[a]) - context.cameraPosition);
                  const float distB = glm::length(ComputeBoundsCenter(scene.segments[b]) - context.cameraPosition);
                  return distA < distB;
              });

    return visibility;
}

bool TMRenderer::render(const TMRenderContext& context,
                        const TMScene& scene,
                        ITMRenderBackend& backend,
                        RenderStats& outStats,
                        std::string& error) {
    outStats = RenderStats();
    error.clear();

    const TMVisibilityResult visibility = buildVisibility(context, scene);
    outStats.visibleSegments = static_cast<uint32_t>(visibility.visibleSegmentIndices.size());
    outStats.visibleModels = static_cast<uint32_t>(visibility.visibleModelIndices.size());
    outStats.frustumRejectedModels = lastVisibilityDebugState.frustumRejectedModels;
    outStats.skippedFrustumCullingModels = lastVisibilityDebugState.skippedFrustumCullingModels;
    outStats.presentationBoundsModels = lastVisibilityDebugState.presentationBoundsModels;

    std::vector<M7FloorDrawCommand> floorCommands;
    floorRenderer.buildDrawCommands(scene, visibility, floorCommands);
    outStats.floorCommands = static_cast<uint32_t>(floorCommands.size());

    std::vector<SectorModelDrawCommand> modelCommands;
    const SectorRenderer::BuildStats modelStats =
        sectorRenderer.buildDrawCommands(context, scene, visibility, modelCommands, error);
    outStats.modelCommands = static_cast<uint32_t>(modelCommands.size());
    outStats.rejectedModels = modelStats.rejectedModels;

    std::vector<Sprite25DDrawCommand> spriteCommands;
    spriteCommands.reserve(scene.sprites25D.size());
    for (const TMSprite25DInstance& sprite : scene.sprites25D) {
        if (!sprite.enabled) {
            continue;
        }
        const float depth = glm::dot(sprite.position - context.cameraPosition, context.cameraForward);
        if (depth <= 0.0f || depth > context.maxVisibleDistance) {
            continue;
        }
        Sprite25DDrawCommand command;
        command.sprite = sprite;
        command.sortDepth = depth;
        spriteCommands.push_back(std::move(command));
    }
    std::stable_sort(spriteCommands.begin(), spriteCommands.end(),
                     [](const Sprite25DDrawCommand& a, const Sprite25DDrawCommand& b) {
                         if (a.sprite.depthSort != b.sprite.depthSort) {
                             return a.sprite.depthSort < b.sprite.depthSort;
                         }
                         if (std::abs(a.sortDepth - b.sortDepth) > 0.001f) {
                             return a.sortDepth > b.sortDepth;
                         }
                         return a.sprite.objectId < b.sprite.objectId;
                     });

    backend.beginFrame(context);
    for (const M7FloorDrawCommand& command : floorCommands) {
        backend.drawM7Floor(command);
    }
    for (const SectorModelDrawCommand& command : modelCommands) {
        backend.drawSectorModel(command);
    }
    for (const Sprite25DDrawCommand& command : spriteCommands) {
        backend.drawSprite25D(command);
    }
    backend.endFrame();

    return error.empty();
}

} // namespace Modularity::Render25D
