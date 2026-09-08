#include "VulkanRenderer.h"
#include "../Camera.h"
#include "../SceneObject.h"

#if MODULARITY_HAS_VULKAN
#include "../ThirdParty/ModuGUI/backends/imgui_impl_vulkan.h"
#include "../../include/ThirdParty/stb_image.h"

#include <algorithm>
#include <array>
#include <set>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <numeric>

namespace {
constexpr const char* kRequiredDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

struct SceneDrawPushConstants {
    alignas(16) glm::mat4 mvp = glm::mat4(1.0f);
    alignas(16) glm::vec4 color = glm::vec4(1.0f);
    // xy = tiling, zw = offset
    alignas(16) glm::vec4 uvTransform = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    // x = mixAmount, y = unlit(0/1), z = hasOverlay(0/1), w = timeSeconds
    alignas(16) glm::vec4 params = glm::vec4(0.3f, 0.0f, 0.0f, 0.0f);
    // x = ambientStrength, y = specularStrength, z = shininess, w = hasNormalMap(0/1)
    alignas(16) glm::vec4 lighting = glm::vec4(0.2f, 0.5f, 32.0f, 0.0f);
    // xyz = object/world origin (used for preview lighting calculations)
    alignas(16) glm::vec4 objectPos = glm::vec4(0.0f);
};

constexpr uint32_t kMaxSceneLights = 16;

struct SceneLightGpu {
    // x = type(0/1/2/3), y = range, z = innerCos, w = outerCos
    alignas(16) glm::vec4 typeRangeAngles = glm::vec4(0.0f);
    alignas(16) glm::vec4 position = glm::vec4(0.0f);
    alignas(16) glm::vec4 direction = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
    alignas(16) glm::vec4 colorIntensity = glm::vec4(1.0f);
};

struct SceneLightingGpu {
    // xyz = camera position, w = light count
    alignas(16) glm::vec4 cameraAndCount = glm::vec4(0.0f, 0.0f, 3.0f, 0.0f);
    // xyz = ambient tint, w = global ambient multiplier
    alignas(16) glm::vec4 ambientAndStrength = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    std::array<SceneLightGpu, kMaxSceneLights> lights{};
};

struct SkyboxPushConstants {
    alignas(16) glm::mat4 viewProj = glm::mat4(1.0f);
    // x = timeOfDay, y = mode, z = hasScrollTexture, w = verticalInfluence
    alignas(16) glm::vec4 params = glm::vec4(0.5f, 0.0f, 0.0f, 0.18f);
    // x = scrollRepeatX, y = scrollRepeatY, z = viewportWidth, w = viewportHeight
    alignas(16) glm::vec4 scroll = glm::vec4(2.0f, 1.0f, 1.0f, 1.0f);
    // x = yawOffset, y = pitchRadians, z = lookSensitivity, w = animation time
    alignas(16) glm::vec4 camera = glm::vec4(0.0f);
};

std::string filenameOf(const std::string& path) {
    if (path.empty()) return std::string();
    fs::path p(path);
    return p.filename().string();
}

bool isSupportedVulkanMaterialShader(const SceneObject& obj) {
    const std::string vertName = filenameOf(obj.vertexShaderPath);
    const std::string fragName = filenameOf(obj.fragmentShaderPath);
    const bool vertSupported = vertName.empty() ||
                               vertName == "vert.glsl" ||
                               vertName == "scroll_texture_vert.glsl";
    const bool fragSupported = fragName.empty() ||
                               fragName == "frag.glsl" ||
                               fragName == "scroll_texture_frag.glsl";
    return vertSupported && fragSupported;
}

bool wantsScrollUvVariant(const SceneObject& obj) {
    const std::string vertName = filenameOf(obj.vertexShaderPath);
    const std::string fragName = filenameOf(obj.fragmentShaderPath);
    return vertName == "scroll_texture_vert.glsl" || fragName == "scroll_texture_frag.glsl";
}

SceneDrawPushConstants buildScenePushConstants(const glm::mat4& mvpMat,
                                               const glm::vec4& color,
                                               const glm::vec4& uvTransform,
                                               float mixAmount,
                                               float ambientStrength,
                                               float specularStrength,
                                               float shininess,
                                               bool unlit,
                                               bool hasOverlay,
                                               bool hasNormalMap,
                                               const glm::vec3& objectPos,
                                               float timeSeconds) {
    SceneDrawPushConstants push{};
    push.mvp = mvpMat;
    push.color = color;
    push.uvTransform = uvTransform;
    push.params = glm::vec4(std::clamp(mixAmount, 0.0f, 1.0f),
                            unlit ? 1.0f : 0.0f,
                            hasOverlay ? 1.0f : 0.0f,
                            timeSeconds);
    push.lighting = glm::vec4(std::clamp(ambientStrength, 0.0f, 1.0f),
                              std::clamp(specularStrength, 0.0f, 2.0f),
                              std::clamp(shininess, 1.0f, 256.0f),
                              hasNormalMap ? 1.0f : 0.0f);
    push.objectPos = glm::vec4(objectPos, 1.0f);
    return push;
}

void checkVkResult(VkResult err) {
    if (err == VK_SUCCESS) return;
    std::cerr << "Vulkan error: " << err << "\n";
}

std::string quotePath(const fs::path& path) {
#ifdef _WIN32
    return "\"" + path.string() + "\"";
#else
    return "'" + path.string() + "'";
#endif
}
} // namespace
#endif

namespace Modularity {

VulkanRenderer::~VulkanRenderer() {
    shutdown();
}

bool VulkanRenderer::initialize(GLFWwindow* targetWindow) {
    shutdown();
    window = targetWindow;
    lastError.clear();

#if !MODULARITY_HAS_VULKAN
    setError("Vulkan is not available in this build (MODULARITY_HAS_VULKAN=0).");
    return false;
#else
    if (!window) {
        setError("Cannot initialize Vulkan renderer: window is null.");
        return false;
    }
    if (!glfwVulkanSupported()) {
        setError("GLFW reports Vulkan is not supported on this system.");
        return false;
    }

    if (!createInstance() ||
        !createSurface() ||
        !pickPhysicalDevice() ||
        !createLogicalDevice() ||
        !createSwapchain() ||
        !createImageViews() ||
        !createRenderPass() ||
        !createFramebuffers() ||
        !createCommandPool() ||
        !createCommandBuffers() ||
        !createSyncObjects()) {
        shutdown();
        return false;
    }

    initialized = true;
    return true;
#endif
}

void VulkanRenderer::shutdown() {
#if MODULARITY_HAS_VULKAN
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }

    destroyScenePipelineResources();
    clearUiImageCache();
    shutdownImGuiBackend();

    for (auto& frame : frames) {
        if (frame.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, frame.imageAvailable, nullptr);
            frame.imageAvailable = VK_NULL_HANDLE;
        }
        if (frame.renderFinished != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, frame.renderFinished, nullptr);
            frame.renderFinished = VK_NULL_HANDLE;
        }
        if (frame.inFlight != VK_NULL_HANDLE) {
            vkDestroyFence(device, frame.inFlight, nullptr);
            frame.inFlight = VK_NULL_HANDLE;
        }
    }

    cleanupSwapchain(true);

    if (imguiDescriptorPool != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, imguiDescriptorPool, nullptr);
        imguiDescriptorPool = VK_NULL_HANDLE;
    }

    if (commandPool != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, commandPool, nullptr);
        commandPool = VK_NULL_HANDLE;
    }

    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }

    if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
    }

    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }

    imagesInFlight.clear();
    currentFrame = 0;
    framebufferResized = false;
    viewportCameraData = SceneCameraData{};
    gameCameraData = SceneCameraData{};
    viewportSceneInstances.clear();
    gameSceneInstances.clear();
#endif

    window = nullptr;
    initialized = false;
    imguiInitialized = false;
}

bool VulkanRenderer::initImGuiBackend() {
#if !MODULARITY_HAS_VULKAN
    setError("ImGui Vulkan backend unavailable in non-Vulkan build.");
    return false;
#else
    if (!initialized) {
        setError("Cannot initialize ImGui Vulkan backend before Vulkan renderer is ready.");
        return false;
    }
    if (imguiInitialized) {
        return true;
    }

    if (!createImGuiDescriptorPool()) {
        return false;
    }

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_1;
    initInfo.Instance = instance;
    initInfo.PhysicalDevice = physicalDevice;
    initInfo.Device = device;
    initInfo.QueueFamily = graphicsFamily;
    initInfo.Queue = graphicsQueue;
    initInfo.DescriptorPool = imguiDescriptorPool;
    initInfo.MinImageCount = minImageCount;
    initInfo.ImageCount = static_cast<uint32_t>(swapchainImages.size());
    initInfo.PipelineCache = VK_NULL_HANDLE;
    initInfo.UseDynamicRendering = false;
    initInfo.PipelineInfoMain.RenderPass = renderPass;
    initInfo.PipelineInfoMain.Subpass = 0;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.Allocator = nullptr;
    initInfo.CheckVkResultFn = checkVkResult;

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        setError("ImGui_ImplVulkan_Init failed.");
        return false;
    }

    imguiInitialized = true;
    return true;
#endif
}

void VulkanRenderer::shutdownImGuiBackend() {
#if MODULARITY_HAS_VULKAN
    if (imguiInitialized) {
        ImGui_ImplVulkan_Shutdown();
        imguiInitialized = false;
    }
#endif
}

void VulkanRenderer::notifyResize() {
    framebufferResized = true;
}

bool VulkanRenderer::prepareFrameResources() {
#if !MODULARITY_HAS_VULKAN
    return false;
#else
    if (!initialized || device == VK_NULL_HANDLE) {
        return false;
    }

    if (!createScenePipelineResources()) {
        return false;
    }
    if (!ensureSceneTarget(viewportSceneTarget) ||
        !ensureSceneTarget(gameSceneTarget)) {
        return false;
    }
    return true;
#endif
}

void VulkanRenderer::setViewportSceneSize(uint32_t width, uint32_t height) {
#if MODULARITY_HAS_VULKAN
    const uint32_t requestedWidth = std::max<uint32_t>(1, width);
    const uint32_t requestedHeight = std::max<uint32_t>(1, height);
    viewportSceneTarget.requestedWidth = requestedWidth;
    viewportSceneTarget.requestedHeight = requestedHeight;
    viewportSceneTarget.resizePending = (viewportSceneTarget.image == VK_NULL_HANDLE) ||
                                        viewportSceneTarget.width != requestedWidth ||
                                        viewportSceneTarget.height != requestedHeight;
#else
    (void)width;
    (void)height;
#endif
}

void VulkanRenderer::setGameSceneSize(uint32_t width, uint32_t height) {
#if MODULARITY_HAS_VULKAN
    const uint32_t requestedWidth = std::max<uint32_t>(1, width);
    const uint32_t requestedHeight = std::max<uint32_t>(1, height);
    gameSceneTarget.requestedWidth = requestedWidth;
    gameSceneTarget.requestedHeight = requestedHeight;
    gameSceneTarget.resizePending = (gameSceneTarget.image == VK_NULL_HANDLE) ||
                                    gameSceneTarget.width != requestedWidth ||
                                    gameSceneTarget.height != requestedHeight;
#else
    (void)width;
    (void)height;
#endif
}

void VulkanRenderer::setViewportSceneData(const std::vector<SceneObject>& sceneObjects,
                                          const Camera& camera,
                                          float fovDeg,
                                          float nearPlane,
                                          float farPlane) {
#if MODULARITY_HAS_VULKAN
    setSceneDataForTarget(viewportCameraData,
                          viewportSceneInstances,
                          viewportSceneLights,
                          sceneObjects,
                          &camera,
                          fovDeg,
                          nearPlane,
                          farPlane);
#else
    (void)sceneObjects;
    (void)camera;
    (void)fovDeg;
    (void)nearPlane;
    (void)farPlane;
#endif
}

void VulkanRenderer::setGameSceneData(const std::vector<SceneObject>& sceneObjects,
                                      const Camera* camera,
                                      float fovDeg,
                                      float nearPlane,
                                      float farPlane) {
#if MODULARITY_HAS_VULKAN
    setSceneDataForTarget(gameCameraData,
                          gameSceneInstances,
                          gameSceneLights,
                          sceneObjects,
                          camera,
                          fovDeg,
                          nearPlane,
                          farPlane);
#else
    (void)sceneObjects;
    (void)camera;
    (void)fovDeg;
    (void)nearPlane;
    (void)farPlane;
#endif
}

void VulkanRenderer::setSkyboxTimeOfDay(float timeOfDay) {
#if MODULARITY_HAS_VULKAN
    // Keep the wrap behavior aligned with OpenGL skybox timing semantics.
    skyboxTimeOfDay = std::fmod(timeOfDay, 1.0f);
    if (skyboxTimeOfDay < 0.0f) {
        skyboxTimeOfDay += 1.0f;
    }
#else
    (void)timeOfDay;
#endif
}

void VulkanRenderer::setSkyboxSettings(const SkyboxSettings& settings) {
#if MODULARITY_HAS_VULKAN
    skyboxSettings = settings;
    skyboxSettings.scrollingRepeatX = std::max(0.01f, skyboxSettings.scrollingRepeatX);
    skyboxSettings.scrollingRepeatY = std::max(0.01f, skyboxSettings.scrollingRepeatY);
    skyboxSettings.scrollingLookSensitivity = std::max(0.0f, skyboxSettings.scrollingLookSensitivity);
    skyboxSettings.scrollingVerticalInfluence = std::clamp(skyboxSettings.scrollingVerticalInfluence, 0.0f, 1.0f);
    destroySkyboxDescriptorSet();
#else
    (void)settings;
#endif
}

void VulkanRenderer::clearViewportSceneData() {
#if MODULARITY_HAS_VULKAN
    viewportCameraData.valid = false;
    viewportSceneInstances.clear();
    viewportSceneLights.clear();
#endif
}

void VulkanRenderer::clearGameSceneData() {
#if MODULARITY_HAS_VULKAN
    gameCameraData.valid = false;
    gameSceneInstances.clear();
    gameSceneLights.clear();
#endif
}

ImTextureID VulkanRenderer::getViewportSceneTextureID() const {
#if MODULARITY_HAS_VULKAN
    return (viewportSceneTarget.descriptorSet == VK_NULL_HANDLE)
        ? static_cast<ImTextureID>(0)
        : reinterpret_cast<ImTextureID>(viewportSceneTarget.descriptorSet);
#else
    return static_cast<ImTextureID>(0);
#endif
}

ImTextureID VulkanRenderer::getOrCreateUIImage(const std::string& path, int* outWidth, int* outHeight) {
#if !MODULARITY_HAS_VULKAN
    (void)path;
    (void)outWidth;
    (void)outHeight;
    return static_cast<ImTextureID>(0);
#else
    if (outWidth) *outWidth = 0;
    if (outHeight) *outHeight = 0;
    if (!initialized || !imguiInitialized || device == VK_NULL_HANDLE || path.empty()) {
        return static_cast<ImTextureID>(0);
    }

    auto it = uiImageCache.find(path);
    if (it != uiImageCache.end()) {
        if (outWidth) *outWidth = it->second.width;
        if (outHeight) *outHeight = it->second.height;
        return (it->second.descriptorSet == VK_NULL_HANDLE)
            ? static_cast<ImTextureID>(0)
            : reinterpret_cast<ImTextureID>(it->second.descriptorSet);
    }

    UiImageTexture texture{};
    if (!createUiImageTexture(path, texture)) {
        return static_cast<ImTextureID>(0);
    }
    if (outWidth) *outWidth = texture.width;
    if (outHeight) *outHeight = texture.height;
    ImTextureID id = reinterpret_cast<ImTextureID>(texture.descriptorSet);
    uiImageCache.emplace(path, std::move(texture));
    return id;
#endif
}

void VulkanRenderer::invalidateImagePath(const std::string& path) {
#if !MODULARITY_HAS_VULKAN
    (void)path;
#else
    if (path.empty()) {
        return;
    }
    auto uiIt = uiImageCache.find(path);
    if (uiIt != uiImageCache.end()) {
        destroyUiImageTexture(uiIt->second);
        uiImageCache.erase(uiIt);
    }
    auto sceneIt = sceneTextureCache.find(path);
    if (sceneIt != sceneTextureCache.end()) {
        destroySceneTexture(sceneIt->second);
        sceneTextureCache.erase(sceneIt);
    }
#endif
}

ImTextureID VulkanRenderer::getGameSceneTextureID() const {
#if MODULARITY_HAS_VULKAN
    return (gameSceneTarget.descriptorSet == VK_NULL_HANDLE)
        ? static_cast<ImTextureID>(0)
        : reinterpret_cast<ImTextureID>(gameSceneTarget.descriptorSet);
#else
    return static_cast<ImTextureID>(0);
#endif
}

bool VulkanRenderer::renderFrame(ImDrawData* drawData, const ImVec4& clearColor) {
#if !MODULARITY_HAS_VULKAN
    (void)drawData;
    (void)clearColor;
    return false;
#else
    if (!initialized || device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE) {
        return false;
    }

    FrameSync& frame = frames[currentFrame];
    vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(device,
                                             swapchain,
                                             UINT64_MAX,
                                             frame.imageAvailable,
                                             VK_NULL_HANDLE,
                                             &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        return recreateSwapchain();
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        setError("vkAcquireNextImageKHR failed.");
        return false;
    }

    if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(device, 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }
    imagesInFlight[imageIndex] = frame.inFlight;

    vkResetFences(device, 1, &frame.inFlight);
    vkResetCommandBuffer(commandBuffers[imageIndex], 0);

    if (!recordCommandBuffer(commandBuffers[imageIndex], imageIndex, drawData, clearColor)) {
        setError("Failed to record Vulkan command buffer.");
        return false;
    }

    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.imageAvailable;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffers[imageIndex];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &frame.renderFinished;

    if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, frame.inFlight) != VK_SUCCESS) {
        setError("vkQueueSubmit failed.");
        return false;
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &frame.renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;

    VkResult present = vkQueuePresentKHR(presentQueue, &presentInfo);
    bool needsRecreate = framebufferResized || present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR;
    if (needsRecreate) {
        framebufferResized = false;
        if (!recreateSwapchain()) {
            return false;
        }
    } else if (present != VK_SUCCESS) {
        setError("vkQueuePresentKHR failed.");
        return false;
    }

    currentFrame = (currentFrame + 1) % kMaxFramesInFlight;
    return true;
#endif
}

#if MODULARITY_HAS_VULKAN
bool VulkanRenderer::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Modularity";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Modularity";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    uint32_t extCount = 0;
    const char** extNames = glfwGetRequiredInstanceExtensions(&extCount);
    if (extCount == 0 || !extNames) {
        setError("glfwGetRequiredInstanceExtensions returned no Vulkan extensions.");
        return false;
    }

    std::vector<const char*> extensions(extNames, extNames + extCount);

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        setError("vkCreateInstance failed.");
        return false;
    }
    return true;
}

bool VulkanRenderer::createSurface() {
    if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
        setError("glfwCreateWindowSurface failed.");
        return false;
    }
    return true;
}

bool VulkanRenderer::pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        setError("No Vulkan physical device found.");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    for (VkPhysicalDevice candidate : devices) {
        QueueFamilyIndices indices = findQueueFamilies(candidate);
        if (!indices.isComplete()) continue;
        if (!checkDeviceExtensionSupport(candidate)) continue;

        SwapchainSupportDetails swap = querySwapchainSupport(candidate);
        if (swap.formats.empty() || swap.presentModes.empty()) continue;

        physicalDevice = candidate;
        graphicsFamily = static_cast<uint32_t>(indices.graphicsFamily);
        presentFamily = static_cast<uint32_t>(indices.presentFamily);
        return true;
    }

    setError("No suitable Vulkan GPU found (missing queues/swapchain support). ");
    return false;
}

bool VulkanRenderer::createLogicalDevice() {
    std::set<uint32_t> queueFamilies = { graphicsFamily, presentFamily };
    float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    queueInfos.reserve(queueFamilies.size());

    for (uint32_t family : queueFamilies) {
        VkDeviceQueueCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = family;
        info.queueCount = 1;
        info.pQueuePriorities = &queuePriority;
        queueInfos.push_back(info);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    createInfo.pQueueCreateInfos = queueInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(sizeof(kRequiredDeviceExtensions) / sizeof(kRequiredDeviceExtensions[0]));
    createInfo.ppEnabledExtensionNames = kRequiredDeviceExtensions;

    if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) {
        setError("vkCreateDevice failed.");
        return false;
    }

    vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
    vkGetDeviceQueue(device, presentFamily, 0, &presentQueue);
    return true;
}

bool VulkanRenderer::createSwapchain() {
    SwapchainSupportDetails details = querySwapchainSupport(physicalDevice);
    if (details.formats.empty() || details.presentModes.empty()) {
        setError("Swapchain is not supported by selected Vulkan device.");
        return false;
    }

    VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(details.formats);
    VkPresentModeKHR presentMode = choosePresentMode(details.presentModes);
    VkExtent2D extent = chooseSwapExtent(details.capabilities);

    uint32_t imageCount = details.capabilities.minImageCount + 1;
    if (details.capabilities.maxImageCount > 0 && imageCount > details.capabilities.maxImageCount) {
        imageCount = details.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t queueFamilyIndices[] = { graphicsFamily, presentFamily };
    if (graphicsFamily != presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = details.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain) != VK_SUCCESS) {
        setError("vkCreateSwapchainKHR failed.");
        return false;
    }

    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data());

    swapchainImageFormat = surfaceFormat.format;
    swapchainExtent = extent;
    minImageCount = details.capabilities.minImageCount;
    if (minImageCount < 2) minImageCount = 2;

    imagesInFlight.assign(swapchainImages.size(), VK_NULL_HANDLE);
    return true;
}

bool VulkanRenderer::createImageViews() {
    swapchainImageViews.resize(swapchainImages.size());

    for (size_t i = 0; i < swapchainImages.size(); ++i) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = swapchainImages[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = swapchainImageFormat;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &createInfo, nullptr, &swapchainImageViews[i]) != VK_SUCCESS) {
            setError("vkCreateImageView failed.");
            return false;
        }
    }

    return true;
}

bool VulkanRenderer::createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchainImageFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
        setError("vkCreateRenderPass failed.");
        return false;
    }
    return true;
}

bool VulkanRenderer::createFramebuffers() {
    swapchainFramebuffers.resize(swapchainImageViews.size());

    for (size_t i = 0; i < swapchainImageViews.size(); ++i) {
        VkImageView attachments[] = { swapchainImageViews[i] };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = swapchainExtent.width;
        framebufferInfo.height = swapchainExtent.height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &swapchainFramebuffers[i]) != VK_SUCCESS) {
            setError("vkCreateFramebuffer failed.");
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = graphicsFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        setError("vkCreateCommandPool failed.");
        return false;
    }
    return true;
}

bool VulkanRenderer::createCommandBuffers() {
    if (!commandBuffers.empty()) {
        vkFreeCommandBuffers(device,
                             commandPool,
                             static_cast<uint32_t>(commandBuffers.size()),
                             commandBuffers.data());
        commandBuffers.clear();
    }

    commandBuffers.resize(swapchainFramebuffers.size());

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

    if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
        setError("vkAllocateCommandBuffers failed.");
        return false;
    }
    return true;
}

bool VulkanRenderer::createSyncObjects() {
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(device, &semInfo, nullptr, &frames[i].imageAvailable) != VK_SUCCESS ||
            vkCreateSemaphore(device, &semInfo, nullptr, &frames[i].renderFinished) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &frames[i].inFlight) != VK_SUCCESS) {
            setError("Failed to create Vulkan sync objects.");
            return false;
        }
    }

    return true;
}

bool VulkanRenderer::createImGuiDescriptorPool() {
    if (imguiDescriptorPool != VK_NULL_HANDLE) {
        return true;
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 1024;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 128;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1152;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &imguiDescriptorPool) != VK_SUCCESS) {
        setError("Failed to create Vulkan descriptor pool for ImGui.");
        return false;
    }
    return true;
}

bool VulkanRenderer::compileGlslToSpirvIfNeeded(const fs::path& sourcePath, const fs::path& outputPath) {
    std::error_code ec;
    if (!fs::exists(sourcePath, ec)) {
        setError("Missing GLSL shader: " + sourcePath.string());
        return false;
    }

    bool needsCompile = true;
    if (fs::exists(outputPath, ec)) {
        auto srcTime = fs::last_write_time(sourcePath, ec);
        if (!ec) {
            auto outTime = fs::last_write_time(outputPath, ec);
            if (!ec && outTime >= srcTime) {
                needsCompile = false;
            }
        }
    }

    if (!needsCompile) {
        return true;
    }

    fs::create_directories(outputPath.parent_path(), ec);
    if (ec) {
        setError("Failed to create Vulkan shader output directory: " + outputPath.parent_path().string());
        return false;
    }

    std::string cmd = "glslc " + quotePath(sourcePath) + " -o " + quotePath(outputPath);
    int result = std::system(cmd.c_str());
    if (result != 0) {
        setError("glslc failed for shader: " + sourcePath.string());
        return false;
    }

    return true;
}

bool VulkanRenderer::loadShaderModuleFromFile(const fs::path& path, VkShaderModule& outModule) {
    outModule = VK_NULL_HANDLE;

    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        setError("Failed to open shader file: " + path.string());
        return false;
    }

    const std::streamsize fileSize = file.tellg();
    if (fileSize <= 0 || (fileSize % 4) != 0) {
        setError("Invalid SPIR-V shader file size: " + path.string());
        return false;
    }

    std::vector<char> buffer(static_cast<size_t>(fileSize));
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    if (!file) {
        setError("Failed to read shader file: " + path.string());
        return false;
    }

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = buffer.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());

    if (vkCreateShaderModule(device, &createInfo, nullptr, &outModule) != VK_SUCCESS) {
        setError("vkCreateShaderModule failed for: " + path.string());
        return false;
    }
    return true;
}

bool VulkanRenderer::createScenePipelineResources() {
    if (scenePipelineDefault != VK_NULL_HANDLE &&
        scenePipelineScroll != VK_NULL_HANDLE &&
        skyboxPipeline != VK_NULL_HANDLE &&
        sceneRenderPass != VK_NULL_HANDLE &&
        sceneDepthFormat != VK_FORMAT_UNDEFINED &&
        scenePipelineLayout != VK_NULL_HANDLE &&
        sceneDescriptorSetLayout != VK_NULL_HANDLE &&
        sceneLightingDescriptorSetLayout != VK_NULL_HANDLE &&
        skyboxDescriptorSetLayout != VK_NULL_HANDLE &&
        sceneSampler != VK_NULL_HANDLE &&
        sceneSamplerPoint != VK_NULL_HANDLE &&
        viewportSceneLighting.descriptorSet != VK_NULL_HANDLE &&
        gameSceneLighting.descriptorSet != VK_NULL_HANDLE &&
        whiteSceneTexture.imageView != VK_NULL_HANDLE &&
        flatNormalSceneTexture.imageView != VK_NULL_HANDLE) {
        return true;
    }

    fs::path shaderDir = fs::path("Resources") / "Shaders" / "Vulkan";
    fs::path materialVertSrc = shaderDir / "material.vert";
    fs::path materialFragSrc = shaderDir / "material.frag";
    fs::path materialScrollFragSrc = shaderDir / "material_scroll.frag";
    fs::path skyboxVertSrc = shaderDir / "skybox.vert";
    fs::path skyboxFragSrc = shaderDir / "skybox.frag";
    fs::path materialVertSpv = shaderDir / "material.vert.spv";
    fs::path materialFragSpv = shaderDir / "material.frag.spv";
    fs::path materialScrollFragSpv = shaderDir / "material_scroll.frag.spv";
    fs::path skyboxVertSpv = shaderDir / "skybox.vert.spv";
    fs::path skyboxFragSpv = shaderDir / "skybox.frag.spv";

    if (!compileGlslToSpirvIfNeeded(materialVertSrc, materialVertSpv) ||
        !compileGlslToSpirvIfNeeded(materialFragSrc, materialFragSpv) ||
        !compileGlslToSpirvIfNeeded(materialScrollFragSrc, materialScrollFragSpv) ||
        !compileGlslToSpirvIfNeeded(skyboxVertSrc, skyboxVertSpv) ||
        !compileGlslToSpirvIfNeeded(skyboxFragSrc, skyboxFragSpv)) {
        return false;
    }

    if (sceneDepthFormat == VK_FORMAT_UNDEFINED) {
        sceneDepthFormat = VK_FORMAT_D32_SFLOAT;
        VkFormatProperties depthProps{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, sceneDepthFormat, &depthProps);
        if ((depthProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
            sceneDepthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
            vkGetPhysicalDeviceFormatProperties(physicalDevice, sceneDepthFormat, &depthProps);
            if ((depthProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
                setError("No supported Vulkan depth format found for scene targets.");
                return false;
            }
        }
    }

    if (sceneRenderPass == VK_NULL_HANDLE) {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = sceneDepthFormat;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthAttachmentRef{};
        depthAttachmentRef.attachment = 1;
        depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;

        std::array<VkSubpassDependency, 2> dependencies{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = 0;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
        renderPassInfo.pDependencies = dependencies.data();

        if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &sceneRenderPass) != VK_SUCCESS) {
            setError("vkCreateRenderPass failed for scene pipeline.");
            return false;
        }
    }

    if (sceneDescriptorSetLayout == VK_NULL_HANDLE) {
        std::array<VkDescriptorSetLayoutBinding, 3> textureBindings{};
        for (uint32_t i = 0; i < textureBindings.size(); ++i) {
            textureBindings[i].binding = i;
            textureBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            textureBindings[i].descriptorCount = 1;
            textureBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(textureBindings.size());
        layoutInfo.pBindings = textureBindings.data();
        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &sceneDescriptorSetLayout) != VK_SUCCESS) {
            setError("vkCreateDescriptorSetLayout failed for scene material textures.");
            return false;
        }
    }

    if (sceneLightingDescriptorSetLayout == VK_NULL_HANDLE) {
        VkDescriptorSetLayoutBinding lightingBinding{};
        lightingBinding.binding = 0;
        lightingBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        lightingBinding.descriptorCount = 1;
        lightingBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &lightingBinding;
        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &sceneLightingDescriptorSetLayout) != VK_SUCCESS) {
            setError("vkCreateDescriptorSetLayout failed for scene lighting buffer.");
            return false;
        }
    }

    if (skyboxDescriptorSetLayout == VK_NULL_HANDLE) {
        std::array<VkDescriptorSetLayoutBinding, 3> textureBindings{};
        for (uint32_t i = 0; i < textureBindings.size(); ++i) {
            textureBindings[i].binding = i;
            textureBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            textureBindings[i].descriptorCount = 1;
            textureBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(textureBindings.size());
        layoutInfo.pBindings = textureBindings.data();
        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &skyboxDescriptorSetLayout) != VK_SUCCESS) {
            setError("vkCreateDescriptorSetLayout failed for skybox textures.");
            return false;
        }
    }

    VkShaderModule materialVertModule = VK_NULL_HANDLE;
    VkShaderModule materialFragModule = VK_NULL_HANDLE;
    VkShaderModule materialScrollFragModule = VK_NULL_HANDLE;
    VkShaderModule skyboxVertModule = VK_NULL_HANDLE;
    VkShaderModule skyboxFragModule = VK_NULL_HANDLE;
    if (!loadShaderModuleFromFile(materialVertSpv, materialVertModule) ||
        !loadShaderModuleFromFile(materialFragSpv, materialFragModule) ||
        !loadShaderModuleFromFile(materialScrollFragSpv, materialScrollFragModule) ||
        !loadShaderModuleFromFile(skyboxVertSpv, skyboxVertModule) ||
        !loadShaderModuleFromFile(skyboxFragSpv, skyboxFragModule)) {
        if (materialVertModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, materialVertModule, nullptr);
        if (materialFragModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, materialFragModule, nullptr);
        if (materialScrollFragModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, materialScrollFragModule, nullptr);
        if (skyboxVertModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, skyboxVertModule, nullptr);
        if (skyboxFragModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, skyboxFragModule, nullptr);
        return false;
    }

    if (scenePipelineLayout == VK_NULL_HANDLE) {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = static_cast<uint32_t>(std::max(sizeof(SceneDrawPushConstants),
                                                        sizeof(SkyboxPushConstants)));

        std::array<VkDescriptorSetLayout, 3> setLayouts = {
            sceneDescriptorSetLayout,
            sceneLightingDescriptorSetLayout,
            skyboxDescriptorSetLayout
        };
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
        layoutInfo.pSetLayouts = setLayouts.data();
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;

        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &scenePipelineLayout) != VK_SUCCESS) {
            vkDestroyShaderModule(device, materialVertModule, nullptr);
            vkDestroyShaderModule(device, materialFragModule, nullptr);
            vkDestroyShaderModule(device, materialScrollFragModule, nullptr);
            vkDestroyShaderModule(device, skyboxVertModule, nullptr);
            vkDestroyShaderModule(device, skyboxFragModule, nullptr);
            setError("vkCreatePipelineLayout failed for scene pipeline.");
            return false;
        }
    }

    auto createGraphicsPipeline = [&](VkShaderModule vertModule,
                                      VkShaderModule fragModule,
                                      VkCullModeFlags cullMode,
                                      bool depthTestEnable,
                                      bool depthWriteEnable,
                                      VkCompareOp depthCompareOp,
                                      VkPipeline& outPipeline) -> bool {
        VkPipelineShaderStageCreateInfo shaderStages[2]{};
        shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shaderStages[0].module = vertModule;
        shaderStages[0].pName = "main";
        shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shaderStages[1].module = fragModule;
        shaderStages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = cullMode;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = depthTestEnable ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = depthWriteEnable ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = depthCompareOp;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        std::array<VkDynamicState, 2> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = scenePipelineLayout;
        pipelineInfo.renderPass = sceneRenderPass;
        pipelineInfo.subpass = 0;

        if (outPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, outPipeline, nullptr);
            outPipeline = VK_NULL_HANDLE;
        }
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &outPipeline) != VK_SUCCESS) {
            return false;
        }
        return true;
    };

    if (!createGraphicsPipeline(materialVertModule, materialFragModule, VK_CULL_MODE_NONE, true, true, VK_COMPARE_OP_LESS, scenePipelineDefault) ||
        !createGraphicsPipeline(materialVertModule, materialScrollFragModule, VK_CULL_MODE_NONE, true, true, VK_COMPARE_OP_LESS, scenePipelineScroll) ||
        !createGraphicsPipeline(skyboxVertModule, skyboxFragModule, VK_CULL_MODE_NONE, true, false, VK_COMPARE_OP_LESS_OR_EQUAL, skyboxPipeline)) {
        vkDestroyShaderModule(device, materialVertModule, nullptr);
        vkDestroyShaderModule(device, materialFragModule, nullptr);
        vkDestroyShaderModule(device, materialScrollFragModule, nullptr);
        vkDestroyShaderModule(device, skyboxVertModule, nullptr);
        vkDestroyShaderModule(device, skyboxFragModule, nullptr);
        setError("vkCreateGraphicsPipelines failed for scene/skybox pipeline.");
        return false;
    }

    vkDestroyShaderModule(device, materialVertModule, nullptr);
    vkDestroyShaderModule(device, materialFragModule, nullptr);
    vkDestroyShaderModule(device, materialScrollFragModule, nullptr);
    vkDestroyShaderModule(device, skyboxVertModule, nullptr);
    vkDestroyShaderModule(device, skyboxFragModule, nullptr);

    if (sceneSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 0.0f;
        if (vkCreateSampler(device, &samplerInfo, nullptr, &sceneSampler) != VK_SUCCESS) {
            setError("vkCreateSampler failed for scene rendering.");
            return false;
        }
    }

    if (sceneSamplerPoint == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 0.0f;
        if (vkCreateSampler(device, &samplerInfo, nullptr, &sceneSamplerPoint) != VK_SUCCESS) {
            setError("vkCreateSampler failed for point-filtered scene rendering.");
            return false;
        }
    }

    if (!createSceneLightingState(viewportSceneLighting) ||
        !createSceneLightingState(gameSceneLighting)) {
        setError("Failed to create Vulkan scene lighting buffers.");
        return false;
    }

    if (whiteSceneTexture.imageView == VK_NULL_HANDLE) {
        const unsigned char whitePixel[4] = { 255, 255, 255, 255 };
        if (!createSceneTextureFromPixels(whitePixel, 1, 1, whiteSceneTexture)) {
            setError("Failed to create Vulkan fallback material texture.");
            return false;
        }
    }

    if (flatNormalSceneTexture.imageView == VK_NULL_HANDLE) {
        const unsigned char flatNormalPixel[4] = { 128, 128, 255, 255 };
        if (!createSceneTextureFromPixels(flatNormalPixel, 1, 1, flatNormalSceneTexture)) {
            setError("Failed to create Vulkan fallback normal-map texture.");
            return false;
        }
    }

    return true;
}

uint32_t VulkanRenderer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VulkanRenderer::createSceneTargetImage(SceneTarget& target, uint32_t width, uint32_t height) {
    target.width = width;
    target.height = height;
    target.resizePending = false;
    if (sceneDepthFormat == VK_FORMAT_UNDEFINED) {
        setError("Scene depth format is not initialized.");
        return false;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &target.image) != VK_SUCCESS) {
        setError("vkCreateImage failed for scene target.");
        return false;
    }

    VkMemoryRequirements memRequirements{};
    vkGetImageMemoryRequirements(device, target.image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
        setError("No suitable Vulkan memory type found for scene image.");
        return false;
    }

    if (vkAllocateMemory(device, &allocInfo, nullptr, &target.memory) != VK_SUCCESS) {
        setError("vkAllocateMemory failed for scene target.");
        return false;
    }
    if (vkBindImageMemory(device, target.image, target.memory, 0) != VK_SUCCESS) {
        setError("vkBindImageMemory failed for scene target.");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = target.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &target.imageView) != VK_SUCCESS) {
        setError("vkCreateImageView failed for scene target.");
        return false;
    }

    VkImageCreateInfo depthImageInfo{};
    depthImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthImageInfo.imageType = VK_IMAGE_TYPE_2D;
    depthImageInfo.extent.width = width;
    depthImageInfo.extent.height = height;
    depthImageInfo.extent.depth = 1;
    depthImageInfo.mipLevels = 1;
    depthImageInfo.arrayLayers = 1;
    depthImageInfo.format = sceneDepthFormat;
    depthImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &depthImageInfo, nullptr, &target.depthImage) != VK_SUCCESS) {
        setError("vkCreateImage failed for scene depth target.");
        return false;
    }

    VkMemoryRequirements depthMemRequirements{};
    vkGetImageMemoryRequirements(device, target.depthImage, &depthMemRequirements);

    VkMemoryAllocateInfo depthAllocInfo{};
    depthAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    depthAllocInfo.allocationSize = depthMemRequirements.size;
    depthAllocInfo.memoryTypeIndex = findMemoryType(depthMemRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (depthAllocInfo.memoryTypeIndex == UINT32_MAX) {
        setError("No suitable Vulkan memory type found for scene depth image.");
        return false;
    }
    if (vkAllocateMemory(device, &depthAllocInfo, nullptr, &target.depthMemory) != VK_SUCCESS) {
        setError("vkAllocateMemory failed for scene depth target.");
        return false;
    }
    if (vkBindImageMemory(device, target.depthImage, target.depthMemory, 0) != VK_SUCCESS) {
        setError("vkBindImageMemory failed for scene depth target.");
        return false;
    }

    VkImageViewCreateInfo depthViewInfo{};
    depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthViewInfo.image = target.depthImage;
    depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.format = sceneDepthFormat;
    depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (sceneDepthFormat == VK_FORMAT_D24_UNORM_S8_UINT || sceneDepthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        depthViewInfo.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    depthViewInfo.subresourceRange.baseMipLevel = 0;
    depthViewInfo.subresourceRange.levelCount = 1;
    depthViewInfo.subresourceRange.baseArrayLayer = 0;
    depthViewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &depthViewInfo, nullptr, &target.depthImageView) != VK_SUCCESS) {
        setError("vkCreateImageView failed for scene depth target.");
        return false;
    }

    VkImageView attachments[] = { target.imageView, target.depthImageView };
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = sceneRenderPass;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = attachments;
    framebufferInfo.width = width;
    framebufferInfo.height = height;
    framebufferInfo.layers = 1;
    if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &target.framebuffer) != VK_SUCCESS) {
        setError("vkCreateFramebuffer failed for scene target.");
        return false;
    }

    if (imguiInitialized && sceneSampler != VK_NULL_HANDLE) {
        const bool isGameTarget = (&target == &gameSceneTarget);
        const VkSampler sampler = (isGameTarget && sceneSamplerPoint != VK_NULL_HANDLE)
            ? sceneSamplerPoint
            : sceneSampler;
        target.descriptorSet = ImGui_ImplVulkan_AddTexture(sampler, target.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    return true;
}

void VulkanRenderer::destroySceneTarget(SceneTarget& target) {
    if (target.descriptorSet != VK_NULL_HANDLE && imguiInitialized) {
        ImGui_ImplVulkan_RemoveTexture(target.descriptorSet);
    }
    target.descriptorSet = VK_NULL_HANDLE;

    if (target.framebuffer != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, target.framebuffer, nullptr);
        target.framebuffer = VK_NULL_HANDLE;
    }
    if (target.imageView != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyImageView(device, target.imageView, nullptr);
        target.imageView = VK_NULL_HANDLE;
    }
    if (target.depthImageView != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyImageView(device, target.depthImageView, nullptr);
        target.depthImageView = VK_NULL_HANDLE;
    }
    if (target.depthImage != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyImage(device, target.depthImage, nullptr);
        target.depthImage = VK_NULL_HANDLE;
    }
    if (target.depthMemory != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkFreeMemory(device, target.depthMemory, nullptr);
        target.depthMemory = VK_NULL_HANDLE;
    }
    if (target.image != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyImage(device, target.image, nullptr);
        target.image = VK_NULL_HANDLE;
    }
    if (target.memory != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkFreeMemory(device, target.memory, nullptr);
        target.memory = VK_NULL_HANDLE;
    }
    target.width = 0;
    target.height = 0;
    target.resizePending = false;
}

bool VulkanRenderer::ensureSceneTarget(SceneTarget& target) {
    if (target.requestedWidth == 0 || target.requestedHeight == 0) {
        return true;
    }

    const uint32_t desiredWidth = std::clamp(target.requestedWidth, 1u, 8192u);
    const uint32_t desiredHeight = std::clamp(target.requestedHeight, 1u, 8192u);
    const bool needsResize = target.resizePending ||
                             target.image == VK_NULL_HANDLE ||
                             target.width != desiredWidth ||
                             target.height != desiredHeight;
    if (needsResize) {
        vkDeviceWaitIdle(device);
        destroySceneTarget(target);
        if (!createSceneTargetImage(target, desiredWidth, desiredHeight)) {
            destroySceneTarget(target);
            return false;
        }
        target.resizePending = false;
    } else if (target.descriptorSet == VK_NULL_HANDLE && imguiInitialized && sceneSampler != VK_NULL_HANDLE) {
        const bool isGameTarget = (&target == &gameSceneTarget);
        const VkSampler sampler = (isGameTarget && sceneSamplerPoint != VK_NULL_HANDLE)
            ? sceneSamplerPoint
            : sceneSampler;
        target.descriptorSet = ImGui_ImplVulkan_AddTexture(sampler, target.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    return true;
}

void VulkanRenderer::renderSceneTarget(VkCommandBuffer cmd,
                                       SceneTarget& target,
                                       const SceneCameraData& cameraData,
                                       const std::vector<SceneInstance>& instances,
                                       const std::vector<SceneLightInstance>& lights,
                                       SceneLightingState& lightingState) {
    if (target.framebuffer == VK_NULL_HANDLE ||
        sceneRenderPass == VK_NULL_HANDLE ||
        scenePipelineDefault == VK_NULL_HANDLE ||
        scenePipelineScroll == VK_NULL_HANDLE ||
        skyboxPipeline == VK_NULL_HANDLE ||
        scenePipelineLayout == VK_NULL_HANDLE) {
        return;
    }

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color.float32[0] = 0.06f;
    clearValues[0].color.float32[1] = 0.08f;
    clearValues[0].color.float32[2] = 0.13f;
    clearValues[0].color.float32[3] = 1.0f;
    clearValues[1].depthStencil.depth = 1.0f;
    clearValues[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = sceneRenderPass;
    passInfo.framebuffer = target.framebuffer;
    passInfo.renderArea.offset = { 0, 0 };
    passInfo.renderArea.extent = { target.width, target.height };
    passInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    passInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(cmd, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(target.width);
    viewport.height = static_cast<float>(target.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = { target.width, target.height };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (cameraData.valid) {
        updateSceneLightingState(lightingState, cameraData, lights);
        if (lightingState.descriptorSet != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(cmd,
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    scenePipelineLayout,
                                    1,
                                    1,
                                    &lightingState.descriptorSet,
                                    0,
                                    nullptr);
        }

        glm::vec3 forward = cameraData.forward;
        if (glm::length(forward) < 0.0001f) {
            forward = glm::vec3(0.0f, 0.0f, -1.0f);
        } else {
            forward = glm::normalize(forward);
        }
        glm::vec3 up = cameraData.up;
        if (glm::length(up) < 0.0001f) {
            up = glm::vec3(0.0f, 1.0f, 0.0f);
        } else {
            up = glm::normalize(up);
        }
        if (std::abs(glm::dot(forward, up)) > 0.995f) {
            up = glm::vec3(0.0f, 1.0f, 0.0f);
            if (std::abs(glm::dot(forward, up)) > 0.995f) {
                up = glm::vec3(0.0f, 0.0f, 1.0f);
            }
        }
        glm::vec3 right = glm::normalize(glm::cross(forward, up));
        up = glm::normalize(glm::cross(right, forward));

        float nearPlane = std::max(0.01f, cameraData.nearPlane);
        float farPlane = std::max(nearPlane + 0.01f, cameraData.farPlane);
        float aspect = static_cast<float>(std::max(1u, target.width)) /
                       static_cast<float>(std::max(1u, target.height));
        glm::mat4 view = glm::lookAt(cameraData.position, cameraData.position + forward, up);
        glm::mat4 proj = glm::perspective(glm::radians(cameraData.fovDeg), aspect, nearPlane, farPlane);
        proj[1][1] *= -1.0f;

        // Skybox pass (draw first).
        glm::mat4 skyView = glm::mat4(glm::mat3(view));
        glm::mat4 skyViewProj = proj * skyView;
        if (!ensureSkyboxDescriptorSet()) {
            vkCmdEndRenderPass(cmd);
            return;
        }
        SkyboxPushConstants skyPush{};
        skyPush.viewProj = skyViewProj;
        skyPush.params.x = skyboxTimeOfDay;
        skyPush.params.y = static_cast<float>(static_cast<int>(skyboxSettings.mode));
        skyPush.params.z = skyboxHasScrollTexture ? 1.0f : 0.0f;
        skyPush.params.w = skyboxSettings.scrollingVerticalInfluence;
        skyPush.scroll.x = skyboxSettings.scrollingRepeatX;
        skyPush.scroll.y = skyboxSettings.scrollingRepeatY;
        skyPush.scroll.z = static_cast<float>(std::max(1u, target.width));
        skyPush.scroll.w = static_cast<float>(std::max(1u, target.height));
        skyPush.camera.x = std::atan2(forward.x, -forward.z) / (2.0f * 3.14159265359f);
        skyPush.camera.y = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
        skyPush.camera.z = skyboxSettings.scrollingLookSensitivity;
        static const auto skyStartTime = std::chrono::steady_clock::now();
        skyPush.camera.w = std::chrono::duration<float>(std::chrono::steady_clock::now() - skyStartTime).count();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipeline);
        vkCmdBindDescriptorSets(cmd,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                scenePipelineLayout,
                                2,
                                1,
                                &skyboxDescriptorSet,
                                0,
                                nullptr);
        vkCmdPushConstants(cmd,
                           scenePipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0,
                           sizeof(SkyboxPushConstants),
                           &skyPush);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        const float timeSeconds = static_cast<float>(glfwGetTime());
        for (const SceneInstance& instance : instances) {
            VkPipeline pipeline = instance.scrollUv ? scenePipelineScroll : scenePipelineDefault;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

            const SceneMaterialSet* materialSet = getOrCreateSceneMaterialSet(instance);
            if (!materialSet || materialSet->descriptorSet == VK_NULL_HANDLE) {
                continue;
            }
            vkCmdBindDescriptorSets(cmd,
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    scenePipelineLayout,
                                    0,
                                    1,
                                    &materialSet->descriptorSet,
                                    0,
                                    nullptr);

            SceneDrawPushConstants push = buildScenePushConstants(proj * view * instance.model,
                                                                  instance.color,
                                                                  glm::vec4(instance.uvTiling, instance.uvOffset),
                                                                  instance.mixAmount,
                                                                  instance.ambientStrength,
                                                                  instance.specularStrength,
                                                                  instance.shininess,
                                                                  instance.unlit,
                                                                  instance.hasOverlay,
                                                                  instance.hasNormalMap,
                                                                  glm::vec3(instance.model[3]),
                                                                  timeSeconds);
            vkCmdPushConstants(cmd,
                               scenePipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0,
                               sizeof(SceneDrawPushConstants),
                               &push);
            vkCmdDraw(cmd, 36, 1, 0, 0);
        }
    }

    vkCmdEndRenderPass(cmd);
}

void VulkanRenderer::destroyScenePipelineResources() {
    if (device == VK_NULL_HANDLE) {
        return;
    }

    destroySceneTarget(viewportSceneTarget);
    destroySceneTarget(gameSceneTarget);
    clearSceneTextureCache();
    destroySceneLightingState(viewportSceneLighting);
    destroySceneLightingState(gameSceneLighting);
    viewportSceneLights.clear();
    gameSceneLights.clear();

    if (sceneSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, sceneSampler, nullptr);
        sceneSampler = VK_NULL_HANDLE;
    }
    if (sceneSamplerPoint != VK_NULL_HANDLE) {
        vkDestroySampler(device, sceneSamplerPoint, nullptr);
        sceneSamplerPoint = VK_NULL_HANDLE;
    }
    if (scenePipelineDefault != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, scenePipelineDefault, nullptr);
        scenePipelineDefault = VK_NULL_HANDLE;
    }
    if (scenePipelineScroll != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, scenePipelineScroll, nullptr);
        scenePipelineScroll = VK_NULL_HANDLE;
    }
    if (skyboxPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, skyboxPipeline, nullptr);
        skyboxPipeline = VK_NULL_HANDLE;
    }
    if (scenePipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, scenePipelineLayout, nullptr);
        scenePipelineLayout = VK_NULL_HANDLE;
    }
    if (sceneDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, sceneDescriptorSetLayout, nullptr);
        sceneDescriptorSetLayout = VK_NULL_HANDLE;
    }
    if (sceneLightingDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, sceneLightingDescriptorSetLayout, nullptr);
        sceneLightingDescriptorSetLayout = VK_NULL_HANDLE;
    }
    if (skyboxDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, skyboxDescriptorSetLayout, nullptr);
        skyboxDescriptorSetLayout = VK_NULL_HANDLE;
    }
    if (sceneRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, sceneRenderPass, nullptr);
        sceneRenderPass = VK_NULL_HANDLE;
    }
    sceneDepthFormat = VK_FORMAT_UNDEFINED;
}

void VulkanRenderer::setSceneDataForTarget(SceneCameraData& targetCamera,
                                           std::vector<SceneInstance>& targetInstances,
                                           std::vector<SceneLightInstance>& targetLights,
                                           const std::vector<SceneObject>& sceneObjects,
                                           const Camera* camera,
                                           float fovDeg,
                                           float nearPlane,
                                           float farPlane) {
    if (!camera) {
        targetCamera.valid = false;
        targetInstances.clear();
        targetLights.clear();
        return;
    }

    targetCamera.valid = true;
    targetCamera.position = camera->position;
    targetCamera.forward = camera->front;
    targetCamera.up = camera->up;
    targetCamera.fovDeg = std::clamp(fovDeg, 10.0f, 170.0f);
    targetCamera.nearPlane = std::max(0.01f, nearPlane);
    targetCamera.farPlane = std::max(targetCamera.nearPlane + 0.01f, farPlane);

    struct OrderedInstance {
        float distance2 = 0.0f;
        SceneInstance instance;
    };

    std::vector<OrderedInstance> ordered;
    ordered.reserve(sceneObjects.size());
    for (const SceneObject& obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasRenderer || obj.renderType == RenderType::None) {
            continue;
        }
        if (obj.renderType == RenderType::Sprite) {
            continue;
        }

        glm::vec3 scale = obj.scale;
        if (std::abs(scale.x) < 0.001f) scale.x = (scale.x >= 0.0f) ? 0.001f : -0.001f;
        if (std::abs(scale.y) < 0.001f) scale.y = (scale.y >= 0.0f) ? 0.001f : -0.001f;
        if (std::abs(scale.z) < 0.001f) scale.z = (scale.z >= 0.0f) ? 0.001f : -0.001f;
        if (obj.renderType == RenderType::Plane && std::abs(scale.y) < 0.02f) {
            scale.y = (scale.y >= 0.0f) ? 0.02f : -0.02f;
        }

        glm::mat4 model(1.0f);
        model = glm::translate(model, obj.position);
        model = glm::rotate(model, glm::radians(obj.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        model = glm::rotate(model, glm::radians(obj.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(obj.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::scale(model, scale);

        glm::vec3 c = glm::clamp(obj.material.color, glm::vec3(0.0f), glm::vec3(1.0f));
        OrderedInstance entry{};
        glm::vec3 toObj = obj.position - targetCamera.position;
        entry.distance2 = glm::dot(toObj, toObj);
        entry.instance.model = model;
        entry.instance.color = glm::vec4(c, 1.0f);
        entry.instance.albedoPath = obj.albedoTexturePath;
        entry.instance.hasOverlay = obj.useOverlay && !obj.overlayTexturePath.empty();
        entry.instance.overlayPath = entry.instance.hasOverlay ? obj.overlayTexturePath : std::string();
        entry.instance.hasNormalMap = !obj.normalMapPath.empty();
        entry.instance.normalPath = entry.instance.hasNormalMap ? obj.normalMapPath : std::string();
        entry.instance.mixAmount = obj.material.textureMix;
        entry.instance.uvTiling = obj.material.uvTiling;
        entry.instance.uvOffset = obj.material.uvOffset;
        entry.instance.ambientStrength = obj.material.ambientStrength;
        entry.instance.specularStrength = obj.material.specularStrength;
        entry.instance.shininess = obj.material.shininess;
        entry.instance.pointFilter = (obj.material.textureFilter == MaterialProperties::TextureFilter::Point);
        entry.instance.unlit = (obj.renderType == RenderType::Mirror || obj.renderType == RenderType::Sprite);
        entry.instance.scrollUv = isSupportedVulkanMaterialShader(obj) && wantsScrollUvVariant(obj);
        ordered.push_back(entry);
    }

    std::sort(ordered.begin(), ordered.end(), [](const OrderedInstance& a, const OrderedInstance& b) {
        return a.distance2 > b.distance2;
    });

    targetInstances.clear();
    targetInstances.reserve(ordered.size());
    for (const OrderedInstance& entry : ordered) {
        targetInstances.push_back(entry.instance);
    }

    auto forwardFromEuler = [](const glm::vec3& deg) {
        glm::mat4 rot(1.0f);
        rot = glm::rotate(rot, glm::radians(deg.x), glm::vec3(1.0f, 0.0f, 0.0f));
        rot = glm::rotate(rot, glm::radians(deg.y), glm::vec3(0.0f, 1.0f, 0.0f));
        rot = glm::rotate(rot, glm::radians(deg.z), glm::vec3(0.0f, 0.0f, 1.0f));
        glm::vec3 dir = glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
        if (glm::dot(dir, dir) < 1e-8f) {
            return glm::vec3(0.0f, -1.0f, 0.0f);
        }
        return glm::normalize(dir);
    };

    targetLights.clear();
    targetLights.reserve(std::min<size_t>(sceneObjects.size(), kMaxSceneLights));
    for (const SceneObject& obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasLight || !obj.light.enabled) {
            continue;
        }
        if (obj.light.intensity <= 0.0f) {
            continue;
        }

        SceneLightInstance light{};
        light.position = obj.position;
        light.direction = forwardFromEuler(obj.rotation);
        light.color = glm::clamp(obj.light.color, glm::vec3(0.0f), glm::vec3(1.0f));
        light.intensity = obj.light.intensity;
        light.range = std::max(0.01f, obj.light.range);

        switch (obj.light.type) {
            case LightType::Directional:
                light.type = 0;
                break;
            case LightType::Point:
                light.type = 1;
                break;
            case LightType::Spot:
                light.type = 2;
                break;
            case LightType::Area:
                light.type = 3;
                break;
        }

        if (light.type == 2) {
            float innerDeg = std::clamp(obj.light.innerAngle, 0.0f, 89.0f);
            float outerDeg = std::clamp(obj.light.outerAngle, innerDeg + 0.001f, 89.9f);
            light.innerCos = std::cos(glm::radians(innerDeg));
            light.outerCos = std::cos(glm::radians(outerDeg));
        } else {
            light.innerCos = 0.0f;
            light.outerCos = 0.0f;
        }

        targetLights.push_back(light);
        if (targetLights.size() >= kMaxSceneLights) {
            break;
        }
    }
}

bool VulkanRenderer::createSceneLightingState(SceneLightingState& lightingState) {
    if (device == VK_NULL_HANDLE ||
        imguiDescriptorPool == VK_NULL_HANDLE ||
        sceneLightingDescriptorSetLayout == VK_NULL_HANDLE) {
        return false;
    }

    if (lightingState.buffer == VK_NULL_HANDLE) {
        if (!createBuffer(sizeof(SceneLightingGpu),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          lightingState.buffer,
                          lightingState.memory)) {
            return false;
        }
    }

    if (lightingState.descriptorSet == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo allocDs{};
        allocDs.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocDs.descriptorPool = imguiDescriptorPool;
        allocDs.descriptorSetCount = 1;
        allocDs.pSetLayouts = &sceneLightingDescriptorSetLayout;
        if (vkAllocateDescriptorSets(device, &allocDs, &lightingState.descriptorSet) != VK_SUCCESS) {
            return false;
        }

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = lightingState.buffer;
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(SceneLightingGpu);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = lightingState.descriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    return true;
}

void VulkanRenderer::destroySceneLightingState(SceneLightingState& lightingState) {
    if (lightingState.descriptorSet != VK_NULL_HANDLE &&
        device != VK_NULL_HANDLE &&
        imguiDescriptorPool != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device, imguiDescriptorPool, 1, &lightingState.descriptorSet);
    }
    lightingState.descriptorSet = VK_NULL_HANDLE;

    if (lightingState.buffer != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, lightingState.buffer, nullptr);
        lightingState.buffer = VK_NULL_HANDLE;
    }
    if (lightingState.memory != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkFreeMemory(device, lightingState.memory, nullptr);
        lightingState.memory = VK_NULL_HANDLE;
    }
}

bool VulkanRenderer::updateSceneLightingState(SceneLightingState& lightingState,
                                              const SceneCameraData& cameraData,
                                              const std::vector<SceneLightInstance>& lights) {
    if (!cameraData.valid) {
        return false;
    }
    if (!createSceneLightingState(lightingState)) {
        return false;
    }
    if (lightingState.memory == VK_NULL_HANDLE) {
        return false;
    }

    SceneLightingGpu gpu{};
    const uint32_t count = static_cast<uint32_t>(std::min<size_t>(lights.size(), kMaxSceneLights));
    gpu.cameraAndCount = glm::vec4(cameraData.position, static_cast<float>(count));
    gpu.ambientAndStrength = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    for (uint32_t i = 0; i < count; ++i) {
        const SceneLightInstance& src = lights[i];
        SceneLightGpu& dst = gpu.lights[i];
        dst.typeRangeAngles = glm::vec4(static_cast<float>(src.type),
                                        src.range,
                                        src.innerCos,
                                        src.outerCos);
        dst.position = glm::vec4(src.position, 1.0f);
        glm::vec3 dir = src.direction;
        if (glm::dot(dir, dir) < 1e-8f) {
            dir = glm::vec3(0.0f, -1.0f, 0.0f);
        } else {
            dir = glm::normalize(dir);
        }
        dst.direction = glm::vec4(dir, 0.0f);
        dst.colorIntensity = glm::vec4(src.color, src.intensity);
    }

    void* mapped = nullptr;
    if (vkMapMemory(device, lightingState.memory, 0, sizeof(SceneLightingGpu), 0, &mapped) != VK_SUCCESS) {
        return false;
    }
    std::memcpy(mapped, &gpu, sizeof(SceneLightingGpu));
    vkUnmapMemory(device, lightingState.memory);
    return true;
}

bool VulkanRenderer::createUiSampler() {
    if (uiSampler != VK_NULL_HANDLE) {
        return true;
    }
    if (device == VK_NULL_HANDLE) {
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &uiSampler) != VK_SUCCESS) {
        setError("vkCreateSampler failed for Vulkan UI images.");
        return false;
    }
    return true;
}

VkCommandBuffer VulkanRenderer::beginSingleUseCommands() {
    if (device == VK_NULL_HANDLE || commandPool == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &allocInfo, &cmd) != VK_SUCCESS) {
        setError("Failed to allocate temporary Vulkan command buffer.");
        return VK_NULL_HANDLE;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        vkFreeCommandBuffers(device, commandPool, 1, &cmd);
        setError("Failed to begin temporary Vulkan command buffer.");
        return VK_NULL_HANDLE;
    }
    return cmd;
}

bool VulkanRenderer::endSingleUseCommands(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE || device == VK_NULL_HANDLE || commandPool == VK_NULL_HANDLE) {
        return false;
    }
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vkFreeCommandBuffers(device, commandPool, 1, &cmd);
        setError("Failed to end temporary Vulkan command buffer.");
        return false;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        vkFreeCommandBuffers(device, commandPool, 1, &cmd);
        setError("Failed to submit temporary Vulkan command buffer.");
        return false;
    }

    vkQueueWaitIdle(graphicsQueue);
    vkFreeCommandBuffers(device, commandPool, 1, &cmd);
    return true;
}

bool VulkanRenderer::createBuffer(VkDeviceSize size,
                                  VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags properties,
                                  VkBuffer& buffer,
                                  VkDeviceMemory& memory) {
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        setError("vkCreateBuffer failed.");
        return false;
    }

    VkMemoryRequirements memRequirements{};
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);
    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
        setError("No suitable Vulkan memory type found for buffer.");
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        setError("vkAllocateMemory failed for buffer.");
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        setError("vkBindBufferMemory failed.");
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void VulkanRenderer::transitionImageLayout(VkCommandBuffer cmd,
                                           VkImage image,
                                           VkImageLayout oldLayout,
                                           VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    vkCmdPipelineBarrier(cmd,
                         srcStage,
                         dstStage,
                         0,
                         0, nullptr,
                         0, nullptr,
                         1, &barrier);
}

void VulkanRenderer::copyBufferToImage(VkCommandBuffer cmd,
                                       VkBuffer buffer,
                                       VkImage image,
                                       uint32_t width,
                                       uint32_t height) {
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(cmd,
                           buffer,
                           image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &region);
}

bool VulkanRenderer::createSceneTextureFromPixels(const unsigned char* pixels,
                                                  int width,
                                                  int height,
                                                  SceneTexture& outTexture) {
    outTexture = SceneTexture{};
    if (!pixels || width <= 0 || height <= 0 || device == VK_NULL_HANDLE) {
        return false;
    }

    VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!createBuffer(imageSize,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      stagingBuffer,
                      stagingMemory)) {
        return false;
    }

    void* mapped = nullptr;
    if (vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mapped) != VK_SUCCESS) {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
        return false;
    }
    std::memcpy(mapped, pixels, static_cast<size_t>(imageSize));
    vkUnmapMemory(device, stagingMemory);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = static_cast<uint32_t>(width);
    imageInfo.extent.height = static_cast<uint32_t>(height);
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &imageInfo, nullptr, &outTexture.image) != VK_SUCCESS) {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
        return false;
    }

    VkMemoryRequirements memRequirements{};
    vkGetImageMemoryRequirements(device, outTexture.image, &memRequirements);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocInfo.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device, &allocInfo, nullptr, &outTexture.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, outTexture.image, outTexture.memory, 0) != VK_SUCCESS) {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
        destroySceneTexture(outTexture);
        return false;
    }

    VkCommandBuffer cmd = beginSingleUseCommands();
    if (cmd == VK_NULL_HANDLE) {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
        destroySceneTexture(outTexture);
        return false;
    }

    transitionImageLayout(cmd,
                          outTexture.image,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(cmd,
                      stagingBuffer,
                      outTexture.image,
                      static_cast<uint32_t>(width),
                      static_cast<uint32_t>(height));
    transitionImageLayout(cmd,
                          outTexture.image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bool submitted = endSingleUseCommands(cmd);
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);
    if (!submitted) {
        destroySceneTexture(outTexture);
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outTexture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &outTexture.imageView) != VK_SUCCESS) {
        destroySceneTexture(outTexture);
        return false;
    }

    outTexture.width = width;
    outTexture.height = height;
    return true;
}

bool VulkanRenderer::createSceneTextureFromFile(const std::string& path, SceneTexture& outTexture) {
    outTexture = SceneTexture{};
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return false;
    }
    bool ok = createSceneTextureFromPixels(pixels, width, height, outTexture);
    stbi_image_free(pixels);
    return ok;
}

const VulkanRenderer::SceneTexture* VulkanRenderer::getOrCreateSceneTexture(const std::string& path) {
    if (path.empty()) {
        return (whiteSceneTexture.imageView != VK_NULL_HANDLE) ? &whiteSceneTexture : nullptr;
    }
    auto it = sceneTextureCache.find(path);
    if (it != sceneTextureCache.end()) {
        return &it->second;
    }
    SceneTexture texture{};
    if (!createSceneTextureFromFile(path, texture)) {
        return (whiteSceneTexture.imageView != VK_NULL_HANDLE) ? &whiteSceneTexture : nullptr;
    }
    auto inserted = sceneTextureCache.emplace(path, std::move(texture));
    return &inserted.first->second;
}

const VulkanRenderer::SceneMaterialSet* VulkanRenderer::getOrCreateSceneMaterialSet(const SceneInstance& instance) {
    if (device == VK_NULL_HANDLE ||
        imguiDescriptorPool == VK_NULL_HANDLE ||
        sceneDescriptorSetLayout == VK_NULL_HANDLE ||
        sceneSampler == VK_NULL_HANDLE ||
        sceneSamplerPoint == VK_NULL_HANDLE) {
        return nullptr;
    }

    const char hasOverlay = instance.hasOverlay ? '1' : '0';
    const char hasNormal = instance.hasNormalMap ? '1' : '0';
    const char pointFilter = instance.pointFilter ? '1' : '0';
    std::string key;
    key.reserve(instance.albedoPath.size() + instance.overlayPath.size() + instance.normalPath.size() + 12);
    key += hasOverlay;
    key += '|';
    key += hasNormal;
    key += '|';
    key += pointFilter;
    key += '|';
    key += instance.albedoPath;
    key += '|';
    key += instance.overlayPath;
    key += '|';
    key += instance.normalPath;

    auto it = sceneMaterialSetCache.find(key);
    if (it != sceneMaterialSetCache.end()) {
        return &it->second;
    }

    const SceneTexture* albedoTexture = getOrCreateSceneTexture(instance.albedoPath);
    if (!albedoTexture || albedoTexture->imageView == VK_NULL_HANDLE) {
        albedoTexture = &whiteSceneTexture;
    }
    if (!albedoTexture || albedoTexture->imageView == VK_NULL_HANDLE) {
        return nullptr;
    }

    const SceneTexture* overlayTexture = &whiteSceneTexture;
    if (instance.hasOverlay && !instance.overlayPath.empty()) {
        overlayTexture = getOrCreateSceneTexture(instance.overlayPath);
        if (!overlayTexture || overlayTexture->imageView == VK_NULL_HANDLE) {
            overlayTexture = &whiteSceneTexture;
        }
    }
    if (!overlayTexture || overlayTexture->imageView == VK_NULL_HANDLE) {
        return nullptr;
    }

    const SceneTexture* normalTexture = &flatNormalSceneTexture;
    if (instance.hasNormalMap && !instance.normalPath.empty()) {
        normalTexture = getOrCreateSceneTexture(instance.normalPath);
        if (!normalTexture || normalTexture->imageView == VK_NULL_HANDLE) {
            normalTexture = &flatNormalSceneTexture;
        }
    }
    if (!normalTexture || normalTexture->imageView == VK_NULL_HANDLE) {
        return nullptr;
    }

    const VkSampler sampler = instance.pointFilter ? sceneSamplerPoint : sceneSampler;

    SceneMaterialSet materialSet{};
    VkDescriptorSetAllocateInfo allocDs{};
    allocDs.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocDs.descriptorPool = imguiDescriptorPool;
    allocDs.descriptorSetCount = 1;
    allocDs.pSetLayouts = &sceneDescriptorSetLayout;
    if (vkAllocateDescriptorSets(device, &allocDs, &materialSet.descriptorSet) != VK_SUCCESS) {
        return nullptr;
    }

    std::array<VkDescriptorImageInfo, 3> imageInfos{};
    imageInfos[0].sampler = sampler;
    imageInfos[0].imageView = albedoTexture->imageView;
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].sampler = sampler;
    imageInfos[1].imageView = overlayTexture->imageView;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[2].sampler = sampler;
    imageInfos[2].imageView = normalTexture->imageView;
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    std::array<VkWriteDescriptorSet, 3> descriptorWrites{};
    for (uint32_t i = 0; i < descriptorWrites.size(); ++i) {
        descriptorWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[i].dstSet = materialSet.descriptorSet;
        descriptorWrites[i].dstBinding = i;
        descriptorWrites[i].descriptorCount = 1;
        descriptorWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrites[i].pImageInfo = &imageInfos[i];
    }
    vkUpdateDescriptorSets(device,
                           static_cast<uint32_t>(descriptorWrites.size()),
                           descriptorWrites.data(),
                           0,
                           nullptr);

    auto inserted = sceneMaterialSetCache.emplace(std::move(key), materialSet);
    return &inserted.first->second;
}

bool VulkanRenderer::ensureSkyboxDescriptorSet() {
    if (device == VK_NULL_HANDLE ||
        imguiDescriptorPool == VK_NULL_HANDLE ||
        skyboxDescriptorSetLayout == VK_NULL_HANDLE ||
        sceneSampler == VK_NULL_HANDLE) {
        return false;
    }

    std::string key;
    key.reserve(skyboxSettings.sunTexturePath.size() +
                skyboxSettings.moonTexturePath.size() +
                skyboxSettings.scrollingTexturePath.size() + 3);
    key += skyboxSettings.sunTexturePath;
    key += '|';
    key += skyboxSettings.moonTexturePath;
    key += '|';
    key += skyboxSettings.scrollingTexturePath;
    if (skyboxDescriptorSet != VK_NULL_HANDLE && skyboxDescriptorKey == key) {
        return true;
    }

    destroySkyboxDescriptorSet();

    const SceneTexture* sunTexture = getOrCreateSceneTexture(skyboxSettings.sunTexturePath);
    if (!sunTexture || sunTexture->imageView == VK_NULL_HANDLE) {
        sunTexture = &whiteSceneTexture;
    }
    const SceneTexture* moonTexture = getOrCreateSceneTexture(skyboxSettings.moonTexturePath);
    if (!moonTexture || moonTexture->imageView == VK_NULL_HANDLE) {
        moonTexture = &whiteSceneTexture;
    }
    const SceneTexture* scrollTexture = getOrCreateSceneTexture(skyboxSettings.scrollingTexturePath);
    if (!scrollTexture || scrollTexture->imageView == VK_NULL_HANDLE) {
        scrollTexture = &whiteSceneTexture;
    }
    if (!sunTexture || !moonTexture || !scrollTexture ||
        sunTexture->imageView == VK_NULL_HANDLE ||
        moonTexture->imageView == VK_NULL_HANDLE ||
        scrollTexture->imageView == VK_NULL_HANDLE) {
        return false;
    }

    VkDescriptorSetAllocateInfo allocDs{};
    allocDs.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocDs.descriptorPool = imguiDescriptorPool;
    allocDs.descriptorSetCount = 1;
    allocDs.pSetLayouts = &skyboxDescriptorSetLayout;
    if (vkAllocateDescriptorSets(device, &allocDs, &skyboxDescriptorSet) != VK_SUCCESS) {
        skyboxDescriptorSet = VK_NULL_HANDLE;
        return false;
    }

    std::array<VkDescriptorImageInfo, 3> imageInfos{};
    imageInfos[0].sampler = sceneSampler;
    imageInfos[0].imageView = sunTexture->imageView;
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].sampler = sceneSampler;
    imageInfos[1].imageView = moonTexture->imageView;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[2].sampler = (sceneSamplerPoint != VK_NULL_HANDLE) ? sceneSamplerPoint : sceneSampler;
    imageInfos[2].imageView = scrollTexture->imageView;
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    std::array<VkWriteDescriptorSet, 3> descriptorWrites{};
    for (uint32_t i = 0; i < descriptorWrites.size(); ++i) {
        descriptorWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[i].dstSet = skyboxDescriptorSet;
        descriptorWrites[i].dstBinding = i;
        descriptorWrites[i].descriptorCount = 1;
        descriptorWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrites[i].pImageInfo = &imageInfos[i];
    }
    vkUpdateDescriptorSets(device,
                           static_cast<uint32_t>(descriptorWrites.size()),
                           descriptorWrites.data(),
                           0,
                           nullptr);

    skyboxDescriptorKey = std::move(key);
    skyboxHasScrollTexture = !skyboxSettings.scrollingTexturePath.empty() && scrollTexture != &whiteSceneTexture;
    return true;
}

void VulkanRenderer::destroySceneTexture(SceneTexture& texture) {
    if (texture.imageView != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyImageView(device, texture.imageView, nullptr);
        texture.imageView = VK_NULL_HANDLE;
    }
    if (texture.image != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyImage(device, texture.image, nullptr);
        texture.image = VK_NULL_HANDLE;
    }
    if (texture.memory != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkFreeMemory(device, texture.memory, nullptr);
        texture.memory = VK_NULL_HANDLE;
    }
    texture.width = 0;
    texture.height = 0;
}

void VulkanRenderer::destroySceneMaterialSet(SceneMaterialSet& materialSet) {
    if (materialSet.descriptorSet != VK_NULL_HANDLE &&
        device != VK_NULL_HANDLE &&
        imguiDescriptorPool != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device, imguiDescriptorPool, 1, &materialSet.descriptorSet);
    }
    materialSet.descriptorSet = VK_NULL_HANDLE;
}

void VulkanRenderer::destroySkyboxDescriptorSet() {
    if (skyboxDescriptorSet != VK_NULL_HANDLE &&
        device != VK_NULL_HANDLE &&
        imguiDescriptorPool != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device, imguiDescriptorPool, 1, &skyboxDescriptorSet);
    }
    skyboxDescriptorSet = VK_NULL_HANDLE;
    skyboxDescriptorKey.clear();
    skyboxHasScrollTexture = false;
}

void VulkanRenderer::clearSceneMaterialSetCache() {
    for (auto& entry : sceneMaterialSetCache) {
        destroySceneMaterialSet(entry.second);
    }
    sceneMaterialSetCache.clear();
}

void VulkanRenderer::clearSceneTextureCache() {
    destroySkyboxDescriptorSet();
    clearSceneMaterialSetCache();
    for (auto& entry : sceneTextureCache) {
        destroySceneTexture(entry.second);
    }
    sceneTextureCache.clear();
    destroySceneTexture(whiteSceneTexture);
    destroySceneTexture(flatNormalSceneTexture);
}

void VulkanRenderer::destroyUiImageTexture(UiImageTexture& texture) {
    if (texture.descriptorSet != VK_NULL_HANDLE && imguiInitialized) {
        ImGui_ImplVulkan_RemoveTexture(texture.descriptorSet);
    }
    texture.descriptorSet = VK_NULL_HANDLE;

    if (texture.imageView != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyImageView(device, texture.imageView, nullptr);
        texture.imageView = VK_NULL_HANDLE;
    }
    if (texture.image != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyImage(device, texture.image, nullptr);
        texture.image = VK_NULL_HANDLE;
    }
    if (texture.memory != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkFreeMemory(device, texture.memory, nullptr);
        texture.memory = VK_NULL_HANDLE;
    }
    texture.width = 0;
    texture.height = 0;
}

void VulkanRenderer::clearUiImageCache() {
    if (device == VK_NULL_HANDLE) {
        uiImageCache.clear();
        return;
    }

    for (auto& entry : uiImageCache) {
        destroyUiImageTexture(entry.second);
    }
    uiImageCache.clear();

    if (uiSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, uiSampler, nullptr);
        uiSampler = VK_NULL_HANDLE;
    }
}

bool VulkanRenderer::createUiImageTexture(const std::string& path, UiImageTexture& outTexture) {
    outTexture = UiImageTexture{};
    if (!imguiInitialized || device == VK_NULL_HANDLE) {
        return false;
    }
    if (!createUiSampler()) {
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return false;
    }

    VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!createBuffer(imageSize,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      stagingBuffer,
                      stagingMemory)) {
        stbi_image_free(pixels);
        return false;
    }

    void* mapped = nullptr;
    if (vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mapped) != VK_SUCCESS) {
        stbi_image_free(pixels);
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
        return false;
    }
    std::memcpy(mapped, pixels, static_cast<size_t>(imageSize));
    vkUnmapMemory(device, stagingMemory);
    stbi_image_free(pixels);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = static_cast<uint32_t>(width);
    imageInfo.extent.height = static_cast<uint32_t>(height);
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &imageInfo, nullptr, &outTexture.image) != VK_SUCCESS) {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
        return false;
    }

    VkMemoryRequirements memRequirements{};
    vkGetImageMemoryRequirements(device, outTexture.image, &memRequirements);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocInfo.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device, &allocInfo, nullptr, &outTexture.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, outTexture.image, outTexture.memory, 0) != VK_SUCCESS) {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
        destroyUiImageTexture(outTexture);
        return false;
    }

    VkCommandBuffer cmd = beginSingleUseCommands();
    if (cmd == VK_NULL_HANDLE) {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
        destroyUiImageTexture(outTexture);
        return false;
    }

    transitionImageLayout(cmd,
                          outTexture.image,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(cmd,
                      stagingBuffer,
                      outTexture.image,
                      static_cast<uint32_t>(width),
                      static_cast<uint32_t>(height));
    transitionImageLayout(cmd,
                          outTexture.image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bool submitted = endSingleUseCommands(cmd);
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);
    if (!submitted) {
        destroyUiImageTexture(outTexture);
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outTexture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &outTexture.imageView) != VK_SUCCESS) {
        destroyUiImageTexture(outTexture);
        return false;
    }

    outTexture.descriptorSet = ImGui_ImplVulkan_AddTexture(uiSampler,
                                                           outTexture.imageView,
                                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (outTexture.descriptorSet == VK_NULL_HANDLE) {
        destroyUiImageTexture(outTexture);
        return false;
    }

    outTexture.width = width;
    outTexture.height = height;
    return true;
}

void VulkanRenderer::cleanupSwapchain(bool destroyRenderPass) {
    if (device == VK_NULL_HANDLE) {
        return;
    }

    if (!commandBuffers.empty() && commandPool != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device,
                             commandPool,
                             static_cast<uint32_t>(commandBuffers.size()),
                             commandBuffers.data());
        commandBuffers.clear();
    }

    for (VkFramebuffer framebuffer : swapchainFramebuffers) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    swapchainFramebuffers.clear();

    if (destroyRenderPass && renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, renderPass, nullptr);
        renderPass = VK_NULL_HANDLE;
    }

    for (VkImageView imageView : swapchainImageViews) {
        vkDestroyImageView(device, imageView, nullptr);
    }
    swapchainImageViews.clear();

    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    swapchainImages.clear();
    imagesInFlight.clear();
}

bool VulkanRenderer::recreateSwapchain() {
    if (!window || !device) {
        return false;
    }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(window, &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(device);

    cleanupSwapchain(false);

    if (!createSwapchain() ||
        !createImageViews()) {
        return false;
    }

    if (renderPass == VK_NULL_HANDLE && !createRenderPass()) {
        return false;
    }

    if (!createFramebuffers() ||
        !createCommandBuffers()) {
        return false;
    }

    if (imguiInitialized) {
        ImGui_ImplVulkan_SetMinImageCount(minImageCount);
    }

    return true;
}

bool VulkanRenderer::recordCommandBuffer(VkCommandBuffer cmd,
                                         uint32_t imageIndex,
                                         ImDrawData* drawData,
                                         const ImVec4& clearColor) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        return false;
    }

    renderSceneTarget(cmd,
                      viewportSceneTarget,
                      viewportCameraData,
                      viewportSceneInstances,
                      viewportSceneLights,
                      viewportSceneLighting);
    renderSceneTarget(cmd,
                      gameSceneTarget,
                      gameCameraData,
                      gameSceneInstances,
                      gameSceneLights,
                      gameSceneLighting);

    VkClearValue clear{};
    clear.color.float32[0] = clearColor.x;
    clear.color.float32[1] = clearColor.y;
    clear.color.float32[2] = clearColor.z;
    clear.color.float32[3] = clearColor.w;

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = swapchainFramebuffers[imageIndex];
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = swapchainExtent;
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clear;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    if (imguiInitialized && drawData) {
        ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
    }

    vkCmdEndRenderPass(cmd);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        return false;
    }

    return true;
}

VulkanRenderer::QueueFamilyIndices VulkanRenderer::findQueueFamilies(VkPhysicalDevice gpu) const {
    QueueFamilyIndices indices;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queueFamilyCount, families.data());

    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphicsFamily = static_cast<int>(i);
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &presentSupport);
        if (presentSupport == VK_TRUE) {
            indices.presentFamily = static_cast<int>(i);
        }

        if (indices.isComplete()) break;
    }

    return indices;
}

bool VulkanRenderer::checkDeviceExtensionSupport(VkPhysicalDevice gpu) const {
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> available(extCount);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, available.data());

    std::set<std::string> required(std::begin(kRequiredDeviceExtensions), std::end(kRequiredDeviceExtensions));
    for (const auto& ext : available) {
        required.erase(ext.extensionName);
    }

    return required.empty();
}

VulkanRenderer::SwapchainSupportDetails VulkanRenderer::querySwapchainSupport(VkPhysicalDevice gpu) const {
    SwapchainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &details.capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, nullptr);
    if (formatCount > 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, details.formats.data());
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &presentModeCount, nullptr);
    if (presentModeCount > 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &presentModeCount, details.presentModes.data());
    }

    return details;
}

VkSurfaceFormatKHR VulkanRenderer::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
    // Match existing OpenGL presentation behavior (no implicit sRGB conversion)
    // so launcher/editor colors don't appear washed out in Vulkan.
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_R8G8B8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats.front();
}

VkPresentModeKHR VulkanRenderer::choosePresentMode(const std::vector<VkPresentModeKHR>& modes) const {
    for (const auto mode : modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return mode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanRenderer::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);

    VkExtent2D actualExtent = {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height)
    };

    actualExtent.width = std::clamp(actualExtent.width,
                                    capabilities.minImageExtent.width,
                                    capabilities.maxImageExtent.width);
    actualExtent.height = std::clamp(actualExtent.height,
                                     capabilities.minImageExtent.height,
                                     capabilities.maxImageExtent.height);

    return actualExtent;
}

#endif

void VulkanRenderer::setError(const std::string& message) {
    lastError = message;
    if (!message.empty()) {
        std::cerr << "[Vulkan] " << message << "\n";
    }
}

} // namespace Modularity
