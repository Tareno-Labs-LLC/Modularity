#pragma once

#include "../Common.h"
#include "../../include/Skybox/Skybox.h"
#include <array>
#include <unordered_map>

#ifndef __ANDROID__
#include "../ThirdParty/glfw/include/GLFW/glfw3.h"
#else
struct GLFWwindow;
#endif

namespace Modularity {

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    bool initialize(GLFWwindow* targetWindow);
    void shutdown();

    bool initImGuiBackend();
    void shutdownImGuiBackend();

    void notifyResize();
    bool prepareFrameResources();
    bool renderFrame(ImDrawData* drawData, const ImVec4& clearColor);
    void setViewportSceneSize(uint32_t width, uint32_t height);
    void setGameSceneSize(uint32_t width, uint32_t height);
    void setViewportSceneData(const std::vector<SceneObject>& sceneObjects,
                              const Camera& camera,
                              float fovDeg,
                              float nearPlane,
                              float farPlane);
    void setGameSceneData(const std::vector<SceneObject>& sceneObjects,
                          const Camera* camera,
                          float fovDeg,
                          float nearPlane,
                          float farPlane);
    void setSkyboxTimeOfDay(float timeOfDay);
    void setSkyboxSettings(const SkyboxSettings& settings);
    void clearViewportSceneData();
    void clearGameSceneData();
    ImTextureID getViewportSceneTextureID() const;
    ImTextureID getGameSceneTextureID() const;
    ImTextureID getOrCreateUIImage(const std::string& path, int* outWidth = nullptr, int* outHeight = nullptr);
    void invalidateImagePath(const std::string& path);

    bool isReady() const { return initialized; }
    bool isImGuiReady() const { return imguiInitialized; }
    const std::string& getLastError() const { return lastError; }

private:
#if MODULARITY_HAS_VULKAN
    struct QueueFamilyIndices {
        int graphicsFamily = -1;
        int presentFamily = -1;

        bool isComplete() const {
            return graphicsFamily >= 0 && presentFamily >= 0;
        }
    };

    struct SwapchainSupportDetails {
        VkSurfaceCapabilitiesKHR capabilities{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    struct FrameSync {
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkSemaphore renderFinished = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
    };

    struct SceneTarget {
        uint32_t requestedWidth = 0;
        uint32_t requestedHeight = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        bool resizePending = false;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView imageView = VK_NULL_HANDLE;
        VkImage depthImage = VK_NULL_HANDLE;
        VkDeviceMemory depthMemory = VK_NULL_HANDLE;
        VkImageView depthImageView = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    };

    struct SceneInstance {
        glm::mat4 model = glm::mat4(1.0f);
        glm::vec4 color = glm::vec4(1.0f);
        std::string albedoPath;
        std::string overlayPath;
        std::string normalPath;
        float mixAmount = 0.3f;
        glm::vec2 uvTiling = glm::vec2(1.0f);
        glm::vec2 uvOffset = glm::vec2(0.0f);
        float ambientStrength = 0.2f;
        float specularStrength = 0.5f;
        float shininess = 32.0f;
        bool unlit = false;
        bool scrollUv = false;
        bool hasOverlay = false;
        bool hasNormalMap = false;
        bool pointFilter = false;
    };

    struct SceneCameraData {
        bool valid = false;
        glm::vec3 position = glm::vec3(0.0f, 0.0f, 3.0f);
        glm::vec3 forward = glm::vec3(0.0f, 0.0f, -1.0f);
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
        float fovDeg = 60.0f;
        float nearPlane = 0.1f;
        float farPlane = 100.0f;
    };

    struct SceneLightInstance {
        int type = 0; // 0=dir, 1=point, 2=spot, 3=area(approx as point)
        glm::vec3 position = glm::vec3(0.0f);
        glm::vec3 direction = glm::vec3(0.0f, -1.0f, 0.0f);
        glm::vec3 color = glm::vec3(1.0f);
        float intensity = 1.0f;
        float range = 10.0f;
        float innerCos = 0.0f;
        float outerCos = 0.0f;
    };

    struct SceneLightingState {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    };

    struct UiImageTexture {
        int width = 0;
        int height = 0;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView imageView = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    };

    struct SceneTexture {
        int width = 0;
        int height = 0;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView imageView = VK_NULL_HANDLE;
    };

    struct SceneMaterialSet {
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    };

    static constexpr size_t kMaxFramesInFlight = 2;

    bool createInstance();
    bool createSurface();
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool createSwapchain();
    bool createImageViews();
    bool createRenderPass();
    bool createFramebuffers();
    bool createCommandPool();
    bool createCommandBuffers();
    bool createSyncObjects();
    bool createImGuiDescriptorPool();
    bool createScenePipelineResources();
    void destroyScenePipelineResources();
    bool createSceneTextureFromPixels(const unsigned char* pixels, int width, int height, SceneTexture& outTexture);
    bool createSceneTextureFromFile(const std::string& path, SceneTexture& outTexture);
    const SceneTexture* getOrCreateSceneTexture(const std::string& path);
    const SceneMaterialSet* getOrCreateSceneMaterialSet(const SceneInstance& instance);
    bool ensureSkyboxDescriptorSet();
    void destroySceneTexture(SceneTexture& texture);
    void destroySceneMaterialSet(SceneMaterialSet& materialSet);
    void destroySkyboxDescriptorSet();
    void clearSceneTextureCache();
    void clearSceneMaterialSetCache();
    bool ensureSceneTarget(SceneTarget& target);
    void destroySceneTarget(SceneTarget& target);
    bool createSceneTargetImage(SceneTarget& target, uint32_t width, uint32_t height);
    void renderSceneTarget(VkCommandBuffer cmd,
                           SceneTarget& target,
                           const SceneCameraData& cameraData,
                           const std::vector<SceneInstance>& instances,
                           const std::vector<SceneLightInstance>& lights,
                           SceneLightingState& lightingState);
    void setSceneDataForTarget(SceneCameraData& targetCamera,
                               std::vector<SceneInstance>& targetInstances,
                               std::vector<SceneLightInstance>& targetLights,
                               const std::vector<SceneObject>& sceneObjects,
                               const Camera* camera,
                               float fovDeg,
                               float nearPlane,
                               float farPlane);
    bool createSceneLightingState(SceneLightingState& lightingState);
    void destroySceneLightingState(SceneLightingState& lightingState);
    bool updateSceneLightingState(SceneLightingState& lightingState,
                                  const SceneCameraData& cameraData,
                                  const std::vector<SceneLightInstance>& lights);
    bool createUiSampler();
    bool createUiImageTexture(const std::string& path, UiImageTexture& outTexture);
    void destroyUiImageTexture(UiImageTexture& texture);
    void clearUiImageCache();
    VkCommandBuffer beginSingleUseCommands();
    bool endSingleUseCommands(VkCommandBuffer cmd);
    bool createBuffer(VkDeviceSize size,
                      VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer& buffer,
                      VkDeviceMemory& memory);
    void transitionImageLayout(VkCommandBuffer cmd,
                               VkImage image,
                               VkImageLayout oldLayout,
                               VkImageLayout newLayout);
    void copyBufferToImage(VkCommandBuffer cmd,
                           VkBuffer buffer,
                           VkImage image,
                           uint32_t width,
                           uint32_t height);
    bool compileGlslToSpirvIfNeeded(const fs::path& sourcePath, const fs::path& outputPath);
    bool loadShaderModuleFromFile(const fs::path& path, VkShaderModule& outModule);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    void cleanupSwapchain(bool destroyRenderPass);
    bool recreateSwapchain();

    bool recordCommandBuffer(VkCommandBuffer cmd,
                             uint32_t imageIndex,
                             ImDrawData* drawData,
                             const ImVec4& clearColor);

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
    bool checkDeviceExtensionSupport(VkPhysicalDevice device) const;
    SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device) const;
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes) const;
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;
#endif

    void setError(const std::string& message);

private:
    GLFWwindow* window = nullptr;
    bool initialized = false;
    bool imguiInitialized = false;
    bool framebufferResized = false;
    std::string lastError;

#if MODULARITY_HAS_VULKAN
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    uint32_t graphicsFamily = 0;
    uint32_t presentFamily = 0;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainImageFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    uint32_t minImageCount = 2;

    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<VkFramebuffer> swapchainFramebuffers;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;

    std::array<FrameSync, kMaxFramesInFlight> frames{};
    std::vector<VkFence> imagesInFlight;
    size_t currentFrame = 0;

    VkDescriptorPool imguiDescriptorPool = VK_NULL_HANDLE;

    VkRenderPass sceneRenderPass = VK_NULL_HANDLE;
    VkFormat sceneDepthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineLayout scenePipelineLayout = VK_NULL_HANDLE;
    VkSampler sceneSampler = VK_NULL_HANDLE;
    VkSampler sceneSamplerPoint = VK_NULL_HANDLE;
    VkSampler uiSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout sceneDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout sceneLightingDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout skyboxDescriptorSetLayout = VK_NULL_HANDLE;
    VkPipeline scenePipelineDefault = VK_NULL_HANDLE;
    VkPipeline scenePipelineScroll = VK_NULL_HANDLE;
    VkPipeline skyboxPipeline = VK_NULL_HANDLE;
    VkDescriptorSet skyboxDescriptorSet = VK_NULL_HANDLE;
    std::string skyboxDescriptorKey;
    bool skyboxHasScrollTexture = false;
    SceneTarget viewportSceneTarget{};
    SceneTarget gameSceneTarget{};
    SceneCameraData viewportCameraData{};
    SceneCameraData gameCameraData{};
    float skyboxTimeOfDay = 0.5f;
    SkyboxSettings skyboxSettings;
    std::vector<SceneInstance> viewportSceneInstances;
    std::vector<SceneInstance> gameSceneInstances;
    std::vector<SceneLightInstance> viewportSceneLights;
    std::vector<SceneLightInstance> gameSceneLights;
    SceneLightingState viewportSceneLighting{};
    SceneLightingState gameSceneLighting{};
    std::unordered_map<std::string, UiImageTexture> uiImageCache;
    std::unordered_map<std::string, SceneTexture> sceneTextureCache;
    std::unordered_map<std::string, SceneMaterialSet> sceneMaterialSetCache;
    SceneTexture whiteSceneTexture{};
    SceneTexture flatNormalSceneTexture{};
#endif
};

} // namespace Modularity
