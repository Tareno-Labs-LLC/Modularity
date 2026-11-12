#pragma once
#include "XR/XRLoader.h"
#if MODULARITY_HAS_OPENXR && MODULARITY_XR_HAS_ANY_GL_TYPES
namespace StereoSwapchainTest {
using namespace Modularity::XR;
struct ImageSet { GLuint textures[2]{}; bool acquired = false; };
inline std::unordered_map<XrSwapchain, ImageSet> images;
inline uintptr_t nextHandle = 1;
inline int acquires = 0, waits = 0, releases = 0;
inline bool rejectArrays = false;
inline XrResult XRAPI_CALL Formats(XrSession, uint32_t capacity, uint32_t* count, int64_t* formats) {
    *count = 1; if (capacity) formats[0] = GL_RGBA8; return XR_SUCCESS;
}
inline XrResult XRAPI_CALL Create(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* handle) {
    if (rejectArrays && info->arraySize > 1) return XR_ERROR_FEATURE_UNSUPPORTED;
    Require((info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0, "Missing presentation transfer usage");
    *handle = reinterpret_cast<XrSwapchain>(nextHandle++);
    auto& image = images[*handle]; glGenTextures(2, image.textures);
    for (GLuint texture : image.textures) {
        const GLenum target = info->arraySize > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
        glBindTexture(target, texture);
        if (info->arraySize > 1) {
            Require(info->sampleCount == 1, "Array presentation is unexpectedly multisampled");
            glTexImage3D(target, 0, GL_RGBA8, info->width, info->height, info->arraySize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        } else glTexImage2D(target, 0, GL_RGBA8, info->width, info->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    return XR_SUCCESS;
}
inline XrResult XRAPI_CALL Destroy(XrSwapchain chain) {
    auto found = images.find(chain);
    Require(found != images.end(), "Destroyed unknown swapchain");
    Require(!found->second.acquired, "Destroyed an acquired swapchain");
    glDeleteTextures(2, found->second.textures); images.erase(found); return XR_SUCCESS;
}
inline XrResult XRAPI_CALL Enumerate(XrSwapchain chain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* data) {
    *count = 2;
    if (capacity) {
        auto* output = reinterpret_cast<XrSwapchainImageOpenGLKHR*>(data);
        for (int i = 0; i < 2; ++i) output[i].image = images.at(chain).textures[i];
    }
    return XR_SUCCESS;
}
inline XrResult XRAPI_CALL Acquire(XrSwapchain chain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    Require(!images.at(chain).acquired, "Duplicate swapchain acquisition");
    images.at(chain).acquired = true; ++acquires; *index = 0; return XR_SUCCESS;
}
inline XrResult XRAPI_CALL Wait(XrSwapchain chain, const XrSwapchainImageWaitInfo*) {
    Require(images.at(chain).acquired, "Wait before acquire"); ++waits; return XR_SUCCESS;
}
inline XrResult XRAPI_CALL Release(XrSwapchain chain, const XrSwapchainImageReleaseInfo*) {
    Require(images.at(chain).acquired, "Duplicate swapchain release");
    images.at(chain).acquired = false; ++releases; return XR_SUCCESS;
}
inline void Run() {
    auto& dispatch = const_cast<XRSessionFunctions&>(XRLoaderSessionFunctions());
    const auto saved = dispatch;
    struct Restore { XRSessionFunctions& target; XRSessionFunctions saved; ~Restore() { target = saved; } } restore{dispatch, saved};
    dispatch.EnumerateSwapchainFormats = Formats; dispatch.CreateSwapchain = Create;
    dispatch.DestroySwapchain = Destroy; dispatch.EnumerateSwapchainImages = Enumerate;
    dispatch.AcquireSwapchainImage = Acquire; dispatch.WaitSwapchainImage = Wait;
    dispatch.ReleaseSwapchainImage = Release;
    std::vector<XRViewInfo> views(2);
    for (auto& view : views) { view.recommendedWidth = 32; view.recommendedHeight = 32; view.recommendedSampleCount = 4; }
    XRSwapchainSetGL set; std::string error;
    const auto session = reinterpret_cast<XrSession>(uintptr_t(1));
    Require(set.create(session, views, XRRenderMode::SinglePassMultiview, error), "Array swapchain creation failed");
    Require(set.resolvedRenderMode() == XRRenderMode::SinglePassMultiview && set.swapchainCount() == 1, "Single pass allocated multiple swapchains");
    auto* chain = set.swapchainForView(0);
    Require(chain == set.swapchainForView(1), "Eyes do not share an image");
    GLuint first = 0; Require(chain->acquireAndWait(first, error), "Array image acquire failed");
    Require(first != 0 && first == chain->acquiredLayerFramebuffer(0) && first != chain->acquiredLayerFramebuffer(1), "Eye framebuffer aliases the wrong layer");
    glDisable(GL_SCISSOR_TEST);
    for (int eye = 0; eye < 2; ++eye) {
        glBindFramebuffer(GL_FRAMEBUFFER, chain->acquiredLayerFramebuffer(eye));
        glClearColor(eye == 0 ? 1 : 0, eye == 1 ? 1 : 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    }
    for (int eye = 0; eye < 2; ++eye) {
        glBindFramebuffer(GL_FRAMEBUFFER, chain->acquiredLayerFramebuffer(eye));
        uint8_t pixel[4]{}; glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        Require(pixel[eye] == 255 && pixel[1 - eye] == 0, "Array eye image bleed");
    }
    Require(chain->release(error), "Array image release failed");
    Require(acquires == 1 && waits == 1 && releases == 1, "Array image was not acquired/waited/released exactly once");
    Require(chain->acquiredLayerFramebuffer(1) == 0, "Released image remains accessible");
    set.destroy(); Require(images.empty(), "Array swapchain leaked images");
    rejectArrays = true;
    Require(set.create(session, views, XRRenderMode::SinglePassMultiview, error), "Array rejection did not recover");
    Require(error.empty() && set.multiviewFellBack() && set.resolvedRenderMode() == XRRenderMode::MultiPass && set.swapchainCount() == 2, "Rejected arrays did not fall back to two eye swapchains");
    set.destroy(); Require(images.empty(), "Fallback swapchain leaked images");
    rejectArrays = false;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    Require(glGetError() == GL_NO_ERROR, "Swapchain GL error");
    std::cout << "PASS: array image ownership, distinct eye layers, single acquire/wait/release and runtime rejection fallback\n";
}
}
#endif
