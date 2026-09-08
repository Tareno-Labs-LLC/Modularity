#include "XRSession.h"

#include "XRDesktopGL.h"
#include "XRLoader.h"

#include <algorithm>
#include <vector>

namespace Modularity::XR {

const char* ToString(XRSessionStatus status) {
    switch (status) {
        case XRSessionStatus::Idle:         return "Idle";
        case XRSessionStatus::Ready:        return "Ready";
        case XRSessionStatus::Synchronized: return "Synchronized";
        case XRSessionStatus::Visible:      return "Visible";
        case XRSessionStatus::Focused:      return "Focused";
        case XRSessionStatus::Stopping:     return "Stopping";
        case XRSessionStatus::LossPending:  return "Loss Pending";
        case XRSessionStatus::Exiting:      return "Exiting";
        case XRSessionStatus::None:
        default:                            return "None";
    }
}

XRSession::~XRSession() { destroy(); }

#if !MODULARITY_HAS_OPENXR

bool XRSession::create(const XRRuntime&, const ProjectOpenXRSettings&,
                       const XRGraphicsBinding&, std::string& outError) {
    outError = "This build of Modularity was compiled without OpenXR support "
               "(MODULARITY_ENABLE_OPENXR=OFF).";
    return false;
}

void XRSession::destroy() {
    created_ = false;
    running_ = false;
    status_ = XRSessionStatus::None;
}

bool XRSession::pollEvents() { return false; }

void XRSession::requestExit() { exitRequested_ = true; }

#else

namespace {

XRSessionStatus fromXrState(XrSessionState state) {
    switch (state) {
        case XR_SESSION_STATE_IDLE:         return XRSessionStatus::Idle;
        case XR_SESSION_STATE_READY:        return XRSessionStatus::Ready;
        case XR_SESSION_STATE_SYNCHRONIZED: return XRSessionStatus::Synchronized;
        case XR_SESSION_STATE_VISIBLE:      return XRSessionStatus::Visible;
        case XR_SESSION_STATE_FOCUSED:      return XRSessionStatus::Focused;
        case XR_SESSION_STATE_STOPPING:     return XRSessionStatus::Stopping;
        case XR_SESSION_STATE_LOSS_PENDING: return XRSessionStatus::LossPending;
        case XR_SESSION_STATE_EXITING:      return XRSessionStatus::Exiting;
        case XR_SESSION_STATE_UNKNOWN:
        default:                            return XRSessionStatus::None;
    }
}

} // namespace

bool XRSession::create(const XRRuntime& runtime,
                       const ProjectOpenXRSettings& settings,
                       const XRGraphicsBinding& binding,
                       std::string& outError) {
    outError.clear();
    destroy();

    if (!runtime.isInitialized() || runtime.instance() == XR_NULL_HANDLE) {
        outError = "Cannot create an OpenXR session before the OpenXR instance exists.";
        return false;
    }
    if (!runtime.hasSystem()) {
        outError = "Cannot create an OpenXR session: no headset has been acquired yet.";
        return false;
    }

    instance_ = runtime.instance();
    systemId_ = runtime.systemId();
    viewConfigurationType_ = runtime.viewConfigurationType();

    blendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    const std::vector<XrEnvironmentBlendMode>& modes = runtime.blendModes();
    if (!modes.empty() &&
        std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == modes.end()) {
        blendMode_ = modes.front();
        XRLogWarn("Runtime does not support an opaque blend mode; using the first "
                  "advertised mode instead.");
    }

    if (!checkGraphicsRequirements(outError) ||
        !createSession(binding, outError) ||
        !createReferenceSpaces(settings, outError)) {
        destroy();
        MutableDiagnostics().lastError = outError;
        return false;
    }

    created_ = true;
    status_ = XRSessionStatus::Idle;
    MutableDiagnostics().sessionCreated = true;
    XRLogInfo("Session created");
    return true;
}

bool XRSession::checkGraphicsRequirements(std::string& outError) {
    const auto versionText = [](XrVersion v) {
        return std::to_string(XR_VERSION_MAJOR(v)) + "." + std::to_string(XR_VERSION_MINOR(v));
    };
    const XRExtensionFunctions& ext = XRLoaderExtensionFunctions();

#if MODULARITY_XR_HAS_GLES_TYPES
    if (!ext.GetOpenGLESGraphicsRequirementsKHR) {
        outError = "The OpenXR runtime did not provide xrGetOpenGLESGraphicsRequirementsKHR. "
                   "XR_KHR_opengl_es_enable is required for Modularity's OpenGL ES rendering path.";
        return false;
    }

    XrGraphicsRequirementsOpenGLESKHR requirements{};
    requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR;
    const XrResult result =
        ext.GetOpenGLESGraphicsRequirementsKHR(instance_, systemId_, &requirements);
    if (XR_FAILED(result)) {
        XRLogResultFailure("xrGetOpenGLESGraphicsRequirementsKHR", static_cast<int32_t>(result));
        outError = "xrGetOpenGLESGraphicsRequirementsKHR failed: " +
                   XRResultString(static_cast<int32_t>(result));
        return false;
    }

    XRLogInfo("OpenGL ES graphics requirements: " +
              versionText(requirements.minApiVersionSupported) + " to " +
              versionText(requirements.maxApiVersionSupported));
    return true;
#elif MODULARITY_XR_HAS_GL_TYPES
    if (!ext.GetOpenGLGraphicsRequirementsKHR) {
        outError = "This OpenXR runtime did not provide xrGetOpenGLGraphicsRequirementsKHR, "
                   "which means it does not support XR_KHR_opengl_enable. Modularity renders "
                   "with OpenGL, so pick a runtime that offers the OpenGL binding in the PC "
                   "tab of OpenXR settings.";
        return false;
    }

    XrGraphicsRequirementsOpenGLKHR requirements{};
    requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR;
    const XrResult result =
        ext.GetOpenGLGraphicsRequirementsKHR(instance_, systemId_, &requirements);
    if (XR_FAILED(result)) {
        XRLogResultFailure("xrGetOpenGLGraphicsRequirementsKHR", static_cast<int32_t>(result));
        outError = "xrGetOpenGLGraphicsRequirementsKHR failed: " +
                   XRResultString(static_cast<int32_t>(result));
        return false;
    }

    XRLogInfo("OpenGL graphics requirements: " +
              versionText(requirements.minApiVersionSupported) + " to " +
              versionText(requirements.maxApiVersionSupported));
    return true;
#else
    (void)ext;
    (void)versionText;
    outError = "This build has no OpenXR graphics binding compiled in. Modularity's OpenXR "
               "rendering path targets OpenGL ES on Android and desktop OpenGL on PC; a "
               "Vulkan binding is intentionally absent.";
    return false;
#endif
}

bool XRSession::createSession(const XRGraphicsBinding& binding, std::string& outError) {
#if MODULARITY_XR_HAS_ANY_GRAPHICS_BINDING
    const void* bindingChain = nullptr;
    const char* graphicsApiName = "OpenGL";

#if MODULARITY_XR_HAS_GRAPHICS_BINDING
    if (!binding.gles.hasContext()) {
        outError = "Cannot create an OpenXR session without a live EGL display and context. "
                   "The renderer must be up before XR starts.";
        return false;
    }

    XrGraphicsBindingOpenGLESAndroidKHR glesBinding{};
    glesBinding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR;
    glesBinding.next = nullptr;
    glesBinding.display = static_cast<EGLDisplay>(binding.gles.eglDisplay);
    glesBinding.config = static_cast<EGLConfig>(binding.gles.eglConfig);
    glesBinding.context = static_cast<EGLContext>(binding.gles.eglContext);
    bindingChain = &glesBinding;
    graphicsApiName = "OpenGL ES";
    XRLogInfo("EGL graphics binding initialized");
#else
    bindingChain = XRDesktopGLBuildSessionBinding(binding.desktop.window, outError);
    if (!bindingChain) return false;
#endif

    XrSessionCreateInfo createInfo{};
    createInfo.type = XR_TYPE_SESSION_CREATE_INFO;
    createInfo.next = bindingChain;
    createInfo.createFlags = 0;
    createInfo.systemId = systemId_;

    const XrResult result =
        XRLoaderSessionFunctions().CreateSession(instance_, &createInfo, &session_);
    if (XR_FAILED(result) || session_ == XR_NULL_HANDLE) {
        session_ = XR_NULL_HANDLE;
        XRLogResultFailure("xrCreateSession", static_cast<int32_t>(result));
        outError = "xrCreateSession failed: " + XRResultString(static_cast<int32_t>(result));
        return false;
    }

    XRDiagnosticsSnapshot& diag = MutableDiagnostics();
    diag.graphicsApi = graphicsApiName;
    if (diag.graphicsVersion.empty()) diag.graphicsVersion = XRDesktopGLContextDescription();
    return true;
#else
    (void)binding;
    std::string reason;
    XRDesktopGLBuildSessionBinding(nullptr, reason);
    outError = reason.empty()
                   ? std::string("No OpenXR graphics binding is implemented for this platform.")
                   : reason;
    return false;
#endif
}

bool XRSession::createReferenceSpaces(const ProjectOpenXRSettings& settings,
                                      std::string& outError) {
    const XRSessionFunctions& fns = XRLoaderSessionFunctions();

    std::vector<XrReferenceSpaceType> available;
    uint32_t spaceCount = 0;
    if (XR_SUCCEEDED(fns.EnumerateReferenceSpaces(session_, 0, &spaceCount, nullptr)) &&
        spaceCount > 0) {
        available.resize(spaceCount);
        if (XR_FAILED(fns.EnumerateReferenceSpaces(session_, spaceCount, &spaceCount,
                                                   available.data()))) {
            available.clear();
        }
    }
    const auto offers = [&available](XrReferenceSpaceType type) {
        return available.empty() ||
               std::find(available.begin(), available.end(), type) != available.end();
    };

    XrReferenceSpaceType wanted = (settings.trackingOrigin == XRTrackingOrigin::Floor)
                                      ? XR_REFERENCE_SPACE_TYPE_STAGE
                                      : XR_REFERENCE_SPACE_TYPE_LOCAL;
    effectiveTrackingOrigin_ = settings.trackingOrigin;
    referenceSpaceFellBack_ = false;
    if (wanted == XR_REFERENCE_SPACE_TYPE_STAGE && !offers(XR_REFERENCE_SPACE_TYPE_STAGE)) {
        XRLogWarn("Runtime does not offer a STAGE reference space; Floor tracking origin "
                  "falls back to Eye Level (LOCAL).");
        wanted = XR_REFERENCE_SPACE_TYPE_LOCAL;
        effectiveTrackingOrigin_ = XRTrackingOrigin::Eye;
        referenceSpaceFellBack_ = true;
    }

    XrReferenceSpaceCreateInfo spaceInfo{};
    spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    spaceInfo.referenceSpaceType = wanted;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;

    XrResult result = fns.CreateReferenceSpace(session_, &spaceInfo, &appSpace_);
    if (XR_FAILED(result) || appSpace_ == XR_NULL_HANDLE) {
        appSpace_ = XR_NULL_HANDLE;
        XRLogResultFailure("xrCreateReferenceSpace", static_cast<int32_t>(result));
        outError = "xrCreateReferenceSpace failed for the tracking space: " +
                   XRResultString(static_cast<int32_t>(result));
        return false;
    }

    XrReferenceSpaceCreateInfo viewInfo{};
    viewInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewInfo.poseInReferenceSpace.orientation.w = 1.0f;
    result = fns.CreateReferenceSpace(session_, &viewInfo, &viewSpace_);
    if (XR_FAILED(result) || viewSpace_ == XR_NULL_HANDLE) {
        viewSpace_ = XR_NULL_HANDLE;
        XRLogResultFailure("xrCreateReferenceSpace(VIEW)", static_cast<int32_t>(result));
        outError = "xrCreateReferenceSpace failed for the VIEW space: " +
                   XRResultString(static_cast<int32_t>(result));
        return false;
    }

    MutableDiagnostics().activeTrackingOrigin = ToString(effectiveTrackingOrigin_);
    XRLogInfo(std::string("Reference space: ") +
              (wanted == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE (Floor)" : "LOCAL (Eye Level)"));
    return true;
}

bool XRSession::beginSession(std::string& outError) {
    if (running_) return true;

    XrSessionBeginInfo beginInfo{};
    beginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
    beginInfo.primaryViewConfigurationType = viewConfigurationType_;

    const XrResult result = XRLoaderSessionFunctions().BeginSession(session_, &beginInfo);
    if (XR_FAILED(result)) {
        XRLogResultFailure("xrBeginSession", static_cast<int32_t>(result));
        outError = "xrBeginSession failed: " + XRResultString(static_cast<int32_t>(result));
        return false;
    }
    running_ = true;
    XRLogInfo("Session begun");
    return true;
}

void XRSession::endSession() {
    if (!running_ || session_ == XR_NULL_HANDLE) return;
    const XrResult result = XRLoaderSessionFunctions().EndSession(session_);
    if (XR_FAILED(result)) {
        XRLogResultFailure("xrEndSession", static_cast<int32_t>(result));
    }
    running_ = false;
    XRLogInfo("Session ended");
}

void XRSession::handleStateChange(const XrEventDataSessionStateChanged& event) {
    if (event.session != XR_NULL_HANDLE && event.session != session_) return;

    const XRSessionStatus previous = status_;
    status_ = fromXrState(event.state);
    if (previous != status_) {
        XRLogInfo(std::string("Session state: ") + ToString(previous) + " -> " + ToString(status_));
    }

    switch (event.state) {
        case XR_SESSION_STATE_READY: {
            std::string error;
            if (!beginSession(error)) {
                XRLogError(error);
                MutableDiagnostics().lastError = error;
                status_ = XRSessionStatus::LossPending;
            }
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            endSession();
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            running_ = false;
            break;
        default:
            break;
    }
}

bool XRSession::pollEvents() {
    if (!created_ || instance_ == XR_NULL_HANDLE) return false;

    const XRInstanceFunctions& fns = XRLoaderInstanceFunctions();
    bool keepRunning = true;

    for (;;) {
        XrEventDataBuffer event{};
        event.type = XR_TYPE_EVENT_DATA_BUFFER;

        const XrResult result = fns.PollEvent(instance_, &event);
        if (result == XR_EVENT_UNAVAILABLE) break;
        if (XR_FAILED(result)) {
            XRLogResultFailure("xrPollEvent", static_cast<int32_t>(result));
            return false;
        }

        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                handleStateChange(
                    *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event));
                break;

            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                XRLogWarn("Runtime reported instance loss pending; shutting XR down.");
                status_ = XRSessionStatus::LossPending;
                keepRunning = false;
                break;

            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                interactionProfileChanged_ = true;
                XRLogInfo("Interaction profile changed");
                break;

            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                XRLogInfo("Reference space change pending (recentre)");
                break;

            case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
                const auto& lost = *reinterpret_cast<const XrEventDataEventsLost*>(&event);
                XRLogWarn("Runtime dropped " + std::to_string(lost.lostEventCount) +
                          " OpenXR event(s); the app was not draining them fast enough.");
                break;
            }

            default:
                break;
        }
    }

    if (status_ == XRSessionStatus::Exiting || status_ == XRSessionStatus::LossPending) {
        keepRunning = false;
    }
    return keepRunning;
}

void XRSession::requestExit() {
    if (exitRequested_ || !created_ || session_ == XR_NULL_HANDLE) return;
    exitRequested_ = true;
    const XrResult result = XRLoaderSessionFunctions().RequestExitSession(session_);
    if (XR_FAILED(result)) {
        XRLogResultFailure("xrRequestExitSession", static_cast<int32_t>(result));
    }
}

bool XRSession::consumeInteractionProfileChanged() {
    const bool changed = interactionProfileChanged_;
    interactionProfileChanged_ = false;
    return changed;
}

void XRSession::destroy() {
    if (session_ != XR_NULL_HANDLE) {
        endSession();
        const XRSessionFunctions& fns = XRLoaderSessionFunctions();
        if (appSpace_ != XR_NULL_HANDLE && fns.DestroySpace) fns.DestroySpace(appSpace_);
        if (viewSpace_ != XR_NULL_HANDLE && fns.DestroySpace) fns.DestroySpace(viewSpace_);
        if (fns.DestroySession) fns.DestroySession(session_);
        XRLogInfo("Session destroyed");
    }
    appSpace_ = XR_NULL_HANDLE;
    viewSpace_ = XR_NULL_HANDLE;
    session_ = XR_NULL_HANDLE;
    instance_ = XR_NULL_HANDLE;
    systemId_ = XR_NULL_SYSTEM_ID;
    interactionProfileChanged_ = false;

    created_ = false;
    running_ = false;
    exitRequested_ = false;
    referenceSpaceFellBack_ = false;
    status_ = XRSessionStatus::None;
    MutableDiagnostics().sessionCreated = false;
}

#endif // MODULARITY_HAS_OPENXR

}
