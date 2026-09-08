#pragma once

#include "XRDiagnostics.h"
#include "XRRuntime.h"
#include "XRSettings.h"

#include <string>

#if MODULARITY_HAS_OPENXR
#include "XRPlatform.h"
#endif

namespace Modularity::XR {

struct XRGraphicsBindingGLES {
    void* eglDisplay = nullptr;
    void* eglConfig = nullptr;
    void* eglContext = nullptr;

    bool hasContext() const { return eglDisplay != nullptr && eglContext != nullptr; }
};

struct XRGraphicsBindingDesktopGL {
    void* window = nullptr;

    bool hasContext() const { return window != nullptr; }
};

struct XRGraphicsBinding {
    XRGraphicsBindingGLES gles;
    XRGraphicsBindingDesktopGL desktop;

    bool hasContext() const { return gles.hasContext() || desktop.hasContext(); }
};

enum class XRSessionStatus {
    None = 0,
    Idle,
    Ready,
    Synchronized,
    Visible,
    Focused,
    Stopping,
    LossPending,
    Exiting
};

const char* ToString(XRSessionStatus status);

class XRSession {
public:
    XRSession() = default;
    ~XRSession();

    XRSession(const XRSession&) = delete;
    XRSession& operator=(const XRSession&) = delete;

    bool create(const XRRuntime& runtime,
                const ProjectOpenXRSettings& settings,
                const XRGraphicsBinding& binding,
                std::string& outError);

    void destroy();

    bool isCreated() const { return created_; }

    bool isRunning() const { return running_; }

    bool shouldRender() const {
        return status_ == XRSessionStatus::Visible || status_ == XRSessionStatus::Focused;
    }

    bool hasFocus() const { return status_ == XRSessionStatus::Focused; }

    XRSessionStatus status() const { return status_; }

    bool wantsTeardown() const {
        return status_ == XRSessionStatus::Exiting || status_ == XRSessionStatus::LossPending;
    }

    bool pollEvents();

    void requestExit();

    bool usingFallbackReferenceSpace() const { return referenceSpaceFellBack_; }
    XRTrackingOrigin effectiveTrackingOrigin() const { return effectiveTrackingOrigin_; }

#if MODULARITY_HAS_OPENXR
    XrSession handle() const { return session_; }
    XrSpace appSpace() const { return appSpace_; }
    XrSpace viewSpace() const { return viewSpace_; }
    XrInstance instance() const { return instance_; }
    XrSystemId systemId() const { return systemId_; }
    XrViewConfigurationType viewConfigurationType() const { return viewConfigurationType_; }
    XrEnvironmentBlendMode blendMode() const { return blendMode_; }

    bool consumeInteractionProfileChanged();
#endif

private:
#if MODULARITY_HAS_OPENXR
    bool checkGraphicsRequirements(std::string& outError);
    bool createSession(const XRGraphicsBinding& binding, std::string& outError);
    bool createReferenceSpaces(const ProjectOpenXRSettings& settings, std::string& outError);
    bool beginSession(std::string& outError);
    void endSession();
    void handleStateChange(const XrEventDataSessionStateChanged& event);

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace appSpace_ = XR_NULL_HANDLE;
    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrViewConfigurationType viewConfigurationType_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XrEnvironmentBlendMode blendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool interactionProfileChanged_ = false;
#endif

    bool created_ = false;
    bool running_ = false;
    bool exitRequested_ = false;
    bool referenceSpaceFellBack_ = false;
    XRSessionStatus status_ = XRSessionStatus::None;
    XRTrackingOrigin effectiveTrackingOrigin_ = XRTrackingOrigin::Floor;
};

}
