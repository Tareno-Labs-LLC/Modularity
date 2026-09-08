#pragma once

#include "XRRuntime.h"
#include "XRSettings.h"

#include <cstdint>
#include <string>
#include <vector>

#if MODULARITY_HAS_OPENXR
#include "XRPlatform.h"
#endif

namespace Modularity::XR {

bool XRMultiviewSupported();

std::string XRSwapchainFormatName(int64_t format);

#if MODULARITY_HAS_OPENXR && MODULARITY_XR_HAS_ANY_GL_TYPES

class XRSwapchainGL {
public:
    XRSwapchainGL() = default;
    ~XRSwapchainGL();

    XRSwapchainGL(const XRSwapchainGL&) = delete;
    XRSwapchainGL& operator=(const XRSwapchainGL&) = delete;
    XRSwapchainGL(XRSwapchainGL&& other) noexcept { moveFrom(other); }
    XRSwapchainGL& operator=(XRSwapchainGL&& other) noexcept {
        if (this != &other) {
            destroy();
            moveFrom(other);
        }
        return *this;
    }

    bool create(XrSession session, int64_t colorFormat, uint32_t width, uint32_t height,
                uint32_t arraySize, uint32_t sampleCount, std::string& outError);
    void destroy();

    bool isValid() const { return swapchain_ != XR_NULL_HANDLE; }

    bool acquireAndWait(uint32_t& outFramebuffer, std::string& outError);

    bool release(std::string& outError);

    XrSwapchain handle() const { return swapchain_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t arraySize() const { return arraySize_; }
    int64_t colorFormat() const { return colorFormat_; }

private:
    void moveFrom(XRSwapchainGL& other);
    bool buildFramebuffers(std::string& outError);

    struct ImageTarget {
        uint32_t colorTexture = 0;
        uint32_t depthTexture = 0;
        uint32_t framebuffer = 0;
    };

    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    std::vector<ImageTarget> images_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t arraySize_ = 1;
    int64_t colorFormat_ = 0;
    int acquiredIndex_ = -1;
};

class XRSwapchainSetGL {
public:
    bool create(XrSession session, const std::vector<XRViewInfo>& views,
                XRRenderMode requestedMode, std::string& outError);
    void destroy();

    bool isValid() const { return valid_; }
    XRRenderMode resolvedRenderMode() const { return resolvedMode_; }
    bool multiviewFellBack() const { return multiviewFellBack_; }

    XRSwapchainGL* swapchainForView(uint32_t viewIndex);
    size_t swapchainCount() const { return swapchains_.size(); }

    uint32_t viewWidth() const { return viewWidth_; }
    uint32_t viewHeight() const { return viewHeight_; }
    int64_t colorFormat() const { return colorFormat_; }

private:
    std::vector<XRSwapchainGL> swapchains_;
    XRRenderMode resolvedMode_ = XRRenderMode::MultiPass;
    bool multiviewFellBack_ = false;
    bool valid_ = false;
    uint32_t viewCount_ = 0;
    uint32_t viewWidth_ = 0;
    uint32_t viewHeight_ = 0;
    int64_t colorFormat_ = 0;
};

#endif // MODULARITY_HAS_OPENXR && MODULARITY_XR_HAS_ANY_GL_TYPES

}
