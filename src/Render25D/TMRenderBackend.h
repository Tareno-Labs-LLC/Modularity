#pragma once

#include "Render25D/MMesh.h"
#include "Render25D/TMScene.h"

namespace Modularity::Render25D {

struct M7FloorDrawCommand {
    uint32_t segmentIndex = kInvalidSegmentIndex;
    glm::vec3 boundsMin = glm::vec3(0.0f);
    glm::vec3 boundsMax = glm::vec3(0.0f);
    TMFloorSurface floorSurface;
};

struct SectorModelDrawCommand {
    uint32_t segmentIndex = kInvalidSegmentIndex;
    glm::mat4 modelMatrix = glm::mat4(1.0f);
    const MMeshRenderData* mesh = nullptr;
    std::string textureOverridePath;
    TMSurfacePresentation presentation;
    TMWobbleSettings wobble;
    float sortDepth = 0.0f;
    float worldBaseHeight = 0.0f;
};

struct Sprite25DDrawCommand {
    TMSprite25DInstance sprite;
    float sortDepth = 0.0f;
};

class ITMRenderBackend {
public:
    virtual ~ITMRenderBackend() = default;

    virtual void beginFrame(const TMRenderContext& context) = 0;
    virtual void drawM7Floor(const M7FloorDrawCommand& command) = 0;
    virtual void drawSectorModel(const SectorModelDrawCommand& command) = 0;
    virtual void drawSprite25D(const Sprite25DDrawCommand& command) = 0;
    virtual void endFrame() = 0;
};

class TMNullRenderBackend final : public ITMRenderBackend {
public:
    void beginFrame(const TMRenderContext& context) override { (void)context; }
    void drawM7Floor(const M7FloorDrawCommand& command) override { (void)command; }
    void drawSectorModel(const SectorModelDrawCommand& command) override { (void)command; }
    void drawSprite25D(const Sprite25DDrawCommand& command) override { (void)command; }
    void endFrame() override {}
};

} // namespace Modularity::Render25D
