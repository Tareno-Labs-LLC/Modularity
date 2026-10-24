#pragma once

#include "../../include/XR/XRInputTypes.h"
#include "XRMath.h"
#include "XRSession.h"
#include "XRSettings.h"

#include <array>
#include <string>
#include <vector>

#if MODULARITY_HAS_OPENXR
#include "XRPlatform.h"
#endif

namespace Modularity::XR {

struct XRControllerState {
    bool active = false;

    XRPose gripPose;
    XRPose aimPose;

    glm::vec3 velocity = glm::vec3(0.0f);
    glm::vec3 angularVelocity = glm::vec3(0.0f);
    bool hasVelocity = false;

    std::array<float, static_cast<size_t>(XRAxisKind::Count)> axes{};
    std::array<glm::vec2, static_cast<size_t>(XRAxis2DKind::Count)> axes2D{};

    std::array<bool, static_cast<size_t>(XRButton::Count)> buttons{};
    std::array<bool, static_cast<size_t>(XRButton::Count)> previousButtons{};

    std::array<bool, static_cast<size_t>(XRButton::Count)> buttonBound{};

    std::string interactionProfile;
#if !MODULARITY_RUNTIME_ONLY
    glm::vec2 editorTouchpad{0};
    bool editorTouchpadTouched = false;
    bool editorTouchpadClicked = false;
#endif

    XRTrackingState trackingState() const {
        if (!gripPose.isValid()) return XRTrackingState::NotTracked;
        return gripPose.isTracked() ? XRTrackingState::Tracked : XRTrackingState::Extrapolated;
    }

    const XRPose& pose(XRPoseKind kind) const {
        return (kind == XRPoseKind::Aim) ? aimPose : gripPose;
    }
};

struct XRInputState {
    bool sessionActive = false;
    bool hasFocus = false;

    XRPose headPose;
    XRControllerState controllers[2];

    const XRControllerState& controller(XRDevice device) const {
        const int index = (device == XRDevice::Right) ? 1 : 0;
        return controllers[index];
    }
    XRControllerState& controller(XRDevice device) {
        const int index = (device == XRDevice::Right) ? 1 : 0;
        return controllers[index];
    }
};

XRInputState& MutableInputState();
const XRInputState& InputState();
void ResetInputState();

struct XRHapticRequest {
    XRDevice device = XRDevice::Right;
    float amplitude = 0.5f;
    float duration = 0.08f;
    float frequency = 0.0f;
};

void RequestHapticPulse(const XRHapticRequest& request);

#if MODULARITY_HAS_OPENXR

class XRInput {
public:
    XRInput() = default;
    ~XRInput();

    XRInput(const XRInput&) = delete;
    XRInput& operator=(const XRInput&) = delete;

    bool initialize(const XRSession& session, const ProjectOpenXRSettings& settings,
                    std::string& outError);

    void shutdown();

    bool isInitialized() const { return initialized_; }

    void syncFrame(const XRSession& session, XrTime predictedDisplayTime);

    void refreshInteractionProfiles(const XRSession& session);

private:
    bool createActions(std::string& outError);
    bool suggestBindings(const ProjectOpenXRSettings& settings, std::string& outError);
    bool createActionSpaces(const XRSession& session, std::string& outError);
    void readController(const XRSession& session, XRDevice device, XrTime predictedDisplayTime);
    void applyPendingHaptics();

    XrPath stringToPath(const char* path) const;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    XrActionSet actionSet_ = XR_NULL_HANDLE;

    XrAction gripPoseAction_ = XR_NULL_HANDLE;
    XrAction aimPoseAction_ = XR_NULL_HANDLE;
    XrAction triggerValueAction_ = XR_NULL_HANDLE;
    XrAction gripValueAction_ = XR_NULL_HANDLE;
    XrAction thumbstickAction_ = XR_NULL_HANDLE;
    XrAction thumbstickClickAction_ = XR_NULL_HANDLE;
    XrAction primaryButtonAction_ = XR_NULL_HANDLE;
    XrAction secondaryButtonAction_ = XR_NULL_HANDLE;
    XrAction menuButtonAction_ = XR_NULL_HANDLE;
    XrAction hapticAction_ = XR_NULL_HANDLE;
#if !MODULARITY_RUNTIME_ONLY
    bool editorWorkspace_ = false;
    XrAction editorTouchpadAction_ = XR_NULL_HANDLE;
    XrAction editorTouchpadTouchAction_ = XR_NULL_HANDLE;
    XrAction editorTouchpadClickAction_ = XR_NULL_HANDLE;
#endif

    std::array<XrPath, 2> handPaths_{ XR_NULL_PATH, XR_NULL_PATH };
    std::array<XrSpace, 2> gripSpaces_{ XR_NULL_HANDLE, XR_NULL_HANDLE };
    std::array<XrSpace, 2> aimSpaces_{ XR_NULL_HANDLE, XR_NULL_HANDLE };

    bool initialized_ = false;
};

#endif // MODULARITY_HAS_OPENXR

}
