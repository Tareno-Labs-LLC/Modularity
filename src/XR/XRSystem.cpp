#include "XRSystem.h"

#include "XRLoader.h"

#include <algorithm>

namespace Modularity::XR {

XRSystem::~XRSystem() { shutdown(); }

void XRSystem::setClipPlanes(float nearPlane, float farPlane) {
    nearPlane_ = std::max(0.001f, nearPlane);
    farPlane_ = std::max(nearPlane_ + 0.001f, farPlane);
}

glm::mat4 XRSystem::viewMatrix(uint32_t viewIndex) const {
    if (viewIndex >= views_.size()) return glm::mat4(1.0f);
    return XRViewMatrix(views_[viewIndex].view.pose, originToWorld_);
}

glm::mat4 XRSystem::projectionMatrix(uint32_t viewIndex) const {
    if (viewIndex >= views_.size()) return glm::mat4(1.0f);
    return XRProjectionMatrix(views_[viewIndex].view, nearPlane_, farPlane_);
}

#if !MODULARITY_HAS_OPENXR

bool XRSystem::startup(const ProjectOpenXRSettings&, const PlatformHandles&,
                       std::string& outError) {
    outError = "This build of Modularity was compiled without OpenXR support "
               "(MODULARITY_ENABLE_OPENXR=OFF).";
    return false;
}

void XRSystem::shutdown() {
    views_.clear();
    frameStarted_ = false;
    shouldRender_ = false;
}

bool XRSystem::update() { return false; }
bool XRSystem::beginFrame() { return false; }
bool XRSystem::beginView(uint32_t, XRRenderView&) { return false; }
void XRSystem::endView(uint32_t) {}
void XRSystem::endFrame() {}
bool XRSystem::createSwapchains(std::string&) { return false; }

#else

bool XRSystem::startup(const ProjectOpenXRSettings& settings, const PlatformHandles& platform,
                       std::string& outError) {
    outError.clear();
    shutdown();
    settings_ = settings;

    if (!settings.enabled) {
        outError = "OpenXR is disabled in Project Settings.";
        return false;
    }
    if (!settings.activeTarget().enabled) {
        outError = std::string("OpenXR is enabled for this project, but the ") +
                   DisplayName(ActivePlatformTarget()) +
                   " target is turned off in Project Settings > OpenXR Manager.";
        return false;
    }

    if (!runtime_.initialize(settings, platform.applicationVM, platform.applicationActivity,
                             outError)) {
        shutdown();
        return false;
    }
    if (!runtime_.hasSystem()) {
        outError = "No head-mounted display is available yet. XR will stay off until one is.";
        shutdown();
        return false;
    }

    if (!session_.create(runtime_, settings, platform.graphics, outError)) {
        shutdown();
        return false;
    }

    if (!createSwapchains(outError)) {
        shutdown();
        return false;
    }

    std::string inputError;
    if (!input_.initialize(session_, settings, inputError)) {
        XRLog(XRLogLevel::Warning, "OpenXR Input",
              "Input initialization failed, continuing without controllers: " + inputError);
    } else {
        XRLog(XRLogLevel::Info, "ModuInput.XR", "Input bindings initialized");
    }

    XRLogInfo("XR system ready");
    return true;
}

bool XRSystem::createSwapchains(std::string& outError) {
#if MODULARITY_XR_HAS_ANY_GL_TYPES
    if (!swapchains_.create(session_.handle(), runtime_.views(),
                            settings_.activeTarget().renderMode, outError)) {
        return false;
    }

    const uint32_t count = runtime_.viewCount();
    views_.assign(count, XRRenderView{});
    projectionViews_.assign(count, XrCompositionLayerProjectionView{});
    for (uint32_t i = 0; i < count; ++i) {
        projectionViews_[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        views_[i].width = swapchains_.viewWidth();
        views_[i].height = swapchains_.viewHeight();
        views_[i].arrayLayer =
            (swapchains_.resolvedRenderMode() == XRRenderMode::SinglePassMultiview) ? i : 0;
    }
    return true;
#else
    outError = "This build has no OpenGL support compiled in, so XR swapchains cannot be "
               "created. Modularity's OpenXR rendering path targets OpenGL ES on Android and "
               "desktop OpenGL on PC.";
    return false;
#endif
}

bool XRSystem::update() {
    if (!session_.isCreated()) return false;
    if (!session_.pollEvents()) return false;

    if (session_.consumeInteractionProfileChanged()) {
        input_.refreshInteractionProfiles(session_);
    }
    return true;
}

bool XRSystem::beginFrame() {
    shouldRender_ = false;
    frameStarted_ = false;
    if (!session_.isRunning()) return false;

    const XRSessionFunctions& fns = XRLoaderSessionFunctions();

    frameState_ = XrFrameState{};
    frameState_.type = XR_TYPE_FRAME_STATE;
    XrFrameWaitInfo waitInfo{};
    waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;
    XrResult result = fns.WaitFrame(session_.handle(), &waitInfo, &frameState_);
    if (XR_FAILED(result)) {
        XRLogResultFailure("xrWaitFrame", static_cast<int32_t>(result));
        return false;
    }

    XrFrameBeginInfo beginInfo{};
    beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO;
    result = fns.BeginFrame(session_.handle(), &beginInfo);
    if (XR_FAILED(result)) {
        XRLogResultFailure("xrBeginFrame", static_cast<int32_t>(result));
        return false;
    }
    frameStarted_ = true;

    input_.syncFrame(session_, frameState_.predictedDisplayTime);

    if (frameState_.shouldRender != XR_TRUE || !session_.shouldRender()) {
        return false;
    }

    XrViewLocateInfo locateInfo{};
    locateInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
    locateInfo.viewConfigurationType = session_.viewConfigurationType();
    locateInfo.displayTime = frameState_.predictedDisplayTime;
    locateInfo.space = session_.appSpace();

    XrViewState viewState{};
    viewState.type = XR_TYPE_VIEW_STATE;
    uint32_t located = 0;
    std::vector<XrView> xrViews(views_.size(), XrView{ XR_TYPE_VIEW, nullptr, {}, {} });
    result = fns.LocateViews(session_.handle(), &locateInfo, &viewState,
                             static_cast<uint32_t>(xrViews.size()), &located, xrViews.data());
    if (XR_FAILED(result)) {
        XRLogResultFailure("xrLocateViews", static_cast<int32_t>(result));
        return false;
    }
    if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 ||
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
        return false;
    }

    for (uint32_t i = 0; i < located && i < views_.size(); ++i) {
        views_[i].view = ToXRView(xrViews[i], viewState.viewStateFlags);
    }

    shouldRender_ = true;
    return true;
}

bool XRSystem::beginView(uint32_t viewIndex, XRRenderView& outView) {
#if MODULARITY_XR_HAS_ANY_GL_TYPES
    if (!shouldRender_ || viewIndex >= views_.size()) return false;

    XRSwapchainGL* swapchain = swapchains_.swapchainForView(viewIndex);
    if (!swapchain) return false;

    const bool multiview = swapchains_.resolvedRenderMode() == XRRenderMode::SinglePassMultiview;
    if (!multiview || viewIndex == 0) {
        uint32_t framebuffer = 0;
        std::string error;
        if (!swapchain->acquireAndWait(framebuffer, error)) {
            XRLogError(error);
            return false;
        }
        views_[viewIndex].framebuffer = framebuffer;
    }
    if (multiview && viewIndex > 0) {
        views_[viewIndex].framebuffer = views_[0].framebuffer;
    }

    views_[viewIndex].width = swapchains_.viewWidth();
    views_[viewIndex].height = swapchains_.viewHeight();
    outView = views_[viewIndex];

    XrCompositionLayerProjectionView& projection = projectionViews_[viewIndex];
    projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
    projection.pose = XrPosef{ ToXr(views_[viewIndex].view.pose.rotation),
                               ToXr(views_[viewIndex].view.pose.position) };
    projection.fov = XrFovf{ views_[viewIndex].view.angleLeft, views_[viewIndex].view.angleRight,
                             views_[viewIndex].view.angleUp, views_[viewIndex].view.angleDown };
    projection.subImage.swapchain = swapchain->handle();
    projection.subImage.imageRect.offset = { 0, 0 };
    projection.subImage.imageRect.extent = { static_cast<int32_t>(swapchains_.viewWidth()),
                                             static_cast<int32_t>(swapchains_.viewHeight()) };
    projection.subImage.imageArrayIndex = views_[viewIndex].arrayLayer;
    return true;
#else
    (void)viewIndex;
    (void)outView;
    return false;
#endif
}

void XRSystem::endView(uint32_t viewIndex) {
#if MODULARITY_XR_HAS_ANY_GL_TYPES
    if (viewIndex >= views_.size()) return;
    XRSwapchainGL* swapchain = swapchains_.swapchainForView(viewIndex);
    if (!swapchain) return;

    const bool multiview = swapchains_.resolvedRenderMode() == XRRenderMode::SinglePassMultiview;
    if (multiview && viewIndex + 1 < views_.size()) return;

    std::string error;
    if (!swapchain->release(error)) {
        XRLogError(error);
    }
#else
    (void)viewIndex;
#endif
}

void XRSystem::endFrame() {
    if (!frameStarted_) return;
    frameStarted_ = false;

    const XRSessionFunctions& fns = XRLoaderSessionFunctions();

    XrFrameEndInfo endInfo{};
    endInfo.type = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime = frameState_.predictedDisplayTime;
    endInfo.environmentBlendMode = session_.blendMode();

    const XrCompositionLayerBaseHeader* layers[1] = { nullptr };
    if (shouldRender_ && !projectionViews_.empty()) {
        projectionLayer_ = XrCompositionLayerProjection{};
        projectionLayer_.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        projectionLayer_.space = session_.appSpace();
        projectionLayer_.layerFlags = 0;
        projectionLayer_.viewCount = static_cast<uint32_t>(projectionViews_.size());
        projectionLayer_.views = projectionViews_.data();
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer_);
        endInfo.layerCount = 1;
        endInfo.layers = layers;
    } else {
        endInfo.layerCount = 0;
        endInfo.layers = nullptr;
    }

    const XrResult result = fns.EndFrame(session_.handle(), &endInfo);
    if (XR_FAILED(result)) {
        XRLogResultFailure("xrEndFrame", static_cast<int32_t>(result));
    }
    shouldRender_ = false;
}

void XRSystem::shutdown() {
    input_.shutdown();
#if MODULARITY_XR_HAS_ANY_GL_TYPES
    swapchains_.destroy();
#endif
    session_.destroy();
    runtime_.shutdown();
    projectionViews_.clear();
    views_.clear();
    frameStarted_ = false;
    shouldRender_ = false;
    originToWorld_ = glm::mat4(1.0f);
    ResetInputState();
}

#endif // MODULARITY_HAS_OPENXR

}
