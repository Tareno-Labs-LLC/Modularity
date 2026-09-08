#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../ThirdParty/glm/glm.hpp"

namespace Nebula {

enum class LightmapMode {
    SingleLightmaps = 0,
    DualLightmaps = 1,
    Directional = 2
};

enum class LightmapQuality {
    Low = 0,
    Medium = 1,
    High = 2
};

struct LightmapSettings {
    LightmapMode mode = LightmapMode::DualLightmaps;
    bool useInForwardRendering = false;
    LightmapQuality quality = LightmapQuality::High;
    int bounces = 1;
    glm::vec3 skyLightColor = glm::vec3(0.83f, 0.89f, 0.98f);
    float skyLightIntensity = 0.0f;
    float bounceBoost = 1.0f;
    float bounceIntensity = 1.0f;
    int finalGatherRays = 1000;
    float contrastThreshold = 0.05f;
    float interpolation = 0.0f;
    int interpolationPoints = 15;
    float ambientOcclusion = 0.0f;
    float lodSurfaceDistance = 1.0f;
    bool lockAtlas = false;
    float resolution = 50.0f;
    float padding = 0.0f;
    int lockedAtlasSize = 1024;
};

struct BakeMaterialData {
    glm::vec4 albedo = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    float roughness = 0.5f;
    float metallic = 0.0f;
    glm::vec3 emissive = glm::vec3(0.0f);
    bool castsShadows = true;
};

struct BakeMeshData {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<uint32_t> indices;
    glm::mat4 worldTransform = glm::mat4(1.0f);
    uint32_t materialId = 0;
    int sceneObjectId = -1;
    std::string name;
};

struct BakeLightData {
    int kind = 1;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 direction = glm::vec3(0.0f, -1.0f, 0.0f);
    glm::vec3 color = glm::vec3(1.0f);
    float intensity = 1.0f;
    float range = 10.0f;
    float innerAngleRadians = 0.26f;
    float outerAngleRadians = 0.44f;
    glm::vec2 halfExtents = glm::vec2(0.5f, 0.5f);
    glm::vec3 right = glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    bool castsShadows = true;
};

struct BakeSceneData {
    std::vector<BakeMeshData> meshes;
    std::vector<BakeMaterialData> materials;
    std::vector<BakeLightData> lights;
    glm::vec3 boundsMin = glm::vec3(0.0f);
    glm::vec3 boundsMax = glm::vec3(0.0f);
    float surfaceArea = 0.0f;

    size_t triangleCount() const;
    bool empty() const { return meshes.empty(); }
};

enum class BakeStatus {
    Idle = 0,
    Running = 1,
    Done = 2,
    Failed = 3,
    Cancelled = 4
};

struct BakeProgress {
    BakeStatus status = BakeStatus::Idle;
    float fraction = -1.0f;
    uint32_t step = 0;
    uint32_t totalSteps = 0;
    std::string message;
};

class LightmapBaker {
public:
    static LightmapBaker& instance();

    bool available(std::string* reason = nullptr);

    const std::string& libraryPath() const { return libraryPath_; }

    bool start(const BakeSceneData& scene,
               const LightmapSettings& settings,
               const std::string& outputPath,
               std::string& error);

    void update();

    void cancel();

    void reset();

    bool isRunning() const { return progress_.status == BakeStatus::Running; }
    const BakeProgress& progress() const { return progress_; }
    const std::string& outputPath() const { return outputPath_; }

    uint32_t resultWidth() const { return resultWidth_; }
    uint32_t resultHeight() const { return resultHeight_; }
    bool hasResult() const { return resultWidth_ > 0 && resultHeight_ > 0; }

    bool copyPreviewRGBA8(std::vector<unsigned char>& dst, int width, int height,
                          float exposure);

    bool writeResult(std::string& error);

    static uint32_t resolveAtlasSize(const LightmapSettings& settings,
                                     float worldSurfaceArea);

    ~LightmapBaker();

private:
    LightmapBaker() = default;
    LightmapBaker(const LightmapBaker&) = delete;
    LightmapBaker& operator=(const LightmapBaker&) = delete;

    bool ensureLoaded();

    void* handle_ = nullptr;
    bool loadAttempted_ = false;
    std::string loadError_;
    std::string libraryPath_;

    void* job_ = nullptr;
    BakeProgress progress_;
    std::string outputPath_;
    uint32_t resultWidth_ = 0;
    uint32_t resultHeight_ = 0;
};

const char* toString(LightmapMode mode);
const char* toString(LightmapQuality quality);

}  // namespace Nebula
