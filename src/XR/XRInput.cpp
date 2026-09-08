#include "XRInput.h"

#include "XRInputBindings.h"
#include "XRLoader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Modularity::XR {

namespace {

XRInputState g_inputState;

constexpr size_t kMaxQueuedHaptics = 16;
std::vector<XRHapticRequest>& hapticQueue() {
    static std::vector<XRHapticRequest> queue;
    return queue;
}

} // namespace

XRInputState& MutableInputState() { return g_inputState; }
const XRInputState& InputState() { return g_inputState; }

void ResetInputState() {
    g_inputState = XRInputState{};
    hapticQueue().clear();
}

void RequestHapticPulse(const XRHapticRequest& request) {
    std::vector<XRHapticRequest>& queue = hapticQueue();
    if (queue.size() >= kMaxQueuedHaptics) return;
    XRHapticRequest clamped = request;
    clamped.amplitude = std::clamp(clamped.amplitude, 0.0f, 1.0f);
    clamped.duration = std::clamp(clamped.duration, 0.0f, 5.0f);
    clamped.frequency = std::max(0.0f, clamped.frequency);
    queue.push_back(clamped);
}

#if MODULARITY_HAS_OPENXR

namespace {

int handIndex(XRDevice device) { return (device == XRDevice::Right) ? 1 : 0; }

const char* const kHandPathStrings[2] = { "/user/hand/left", "/user/hand/right" };

bool analogPressed(float value, bool previous) {
    if (previous) return value > kXRAnalogReleaseThreshold;
    return value >= kXRAnalogPressThreshold;
}

XrAction buttonActionFor(XRButton button, XrAction thumbstickClick, XrAction primary,
                         XrAction secondary, XrAction menu) {
    switch (button) {
        case XRButton::ThumbstickClick: return thumbstickClick;
        case XRButton::PrimaryButton:   return primary;
        case XRButton::SecondaryButton: return secondary;
        case XRButton::Menu:            return menu;
        default:                        return XR_NULL_HANDLE;
    }
}

XRActionSlot buttonSlotFor(XRButton button) {
    switch (button) {
        case XRButton::Trigger:         return XRActionSlot::TriggerValue;
        case XRButton::Grip:            return XRActionSlot::GripValue;
        case XRButton::ThumbstickClick: return XRActionSlot::ThumbstickClick;
        case XRButton::PrimaryButton:   return XRActionSlot::PrimaryButton;
        case XRButton::SecondaryButton: return XRActionSlot::SecondaryButton;
        case XRButton::Menu:            return XRActionSlot::MenuButton;
        default:                        return XRActionSlot::Count;
    }
}

} // namespace

XRInput::~XRInput() { shutdown(); }

XrPath XRInput::stringToPath(const char* path) const {
    if (!path || instance_ == XR_NULL_HANDLE) return XR_NULL_PATH;
    XrPath result = XR_NULL_PATH;
    const XrResult status = XRLoaderInstanceFunctions().StringToPath(instance_, path, &result);
    if (XR_FAILED(status)) {
        XRLog(XRLogLevel::Warning, "OpenXR Input",
              std::string("xrStringToPath failed for ") + path + ": " +
                  XRResultString(static_cast<int32_t>(status)));
        return XR_NULL_PATH;
    }
    return result;
}

bool XRInput::initialize(const XRSession& session, const ProjectOpenXRSettings& settings,
                         std::string& outError) {
    outError.clear();
    shutdown();

    if (!session.isCreated() || session.handle() == XR_NULL_HANDLE) {
        outError = "Cannot initialize XR input before the session exists.";
        return false;
    }

    instance_ = session.instance();
    session_ = session.handle();

    for (int i = 0; i < 2; ++i) {
        handPaths_[i] = stringToPath(kHandPathStrings[i]);
        if (handPaths_[i] == XR_NULL_PATH) {
            outError = std::string("Could not resolve the OpenXR path ") + kHandPathStrings[i] + ".";
            shutdown();
            return false;
        }
    }

    if (!createActions(outError) || !suggestBindings(settings, outError) ||
        !createActionSpaces(session, outError)) {
        shutdown();
        return false;
    }

    XrSessionActionSetsAttachInfo attachInfo{};
    attachInfo.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet_;
    const XrResult result = XRLoaderInputFunctions().AttachSessionActionSets(session_, &attachInfo);
    if (XR_FAILED(result)) {
        XRLogResultFailure("xrAttachSessionActionSets", static_cast<int32_t>(result));
        outError = "xrAttachSessionActionSets failed: " +
                   XRResultString(static_cast<int32_t>(result));
        shutdown();
        return false;
    }

    initialized_ = true;
    XRLog(XRLogLevel::Info, "OpenXR Input", "Action set created and attached");
    refreshInteractionProfiles(session);
    return true;
}

bool XRInput::createActions(std::string& outError) {
    const XRInputFunctions& fns = XRLoaderInputFunctions();

    XrActionSetCreateInfo setInfo{};
    setInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    std::snprintf(setInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "%s", "modularity_xr");
    std::snprintf(setInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "%s",
                  "Modularity XR");
    setInfo.priority = 0;

    XrResult result = fns.CreateActionSet(instance_, &setInfo, &actionSet_);
    if (XR_FAILED(result)) {
        XRLogResultFailure("xrCreateActionSet", static_cast<int32_t>(result));
        outError = "xrCreateActionSet failed: " + XRResultString(static_cast<int32_t>(result));
        return false;
    }

    const auto makeAction = [&](const char* name, const char* localized, XrActionType type,
                                XrAction& out) {
        XrActionCreateInfo info{};
        info.type = XR_TYPE_ACTION_CREATE_INFO;
        info.actionType = type;
        std::snprintf(info.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
        std::snprintf(info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", localized);
        info.countSubactionPaths = 2;
        info.subactionPaths = handPaths_.data();

        const XrResult status = fns.CreateAction(actionSet_, &info, &out);
        if (XR_FAILED(status)) {
            XRLogResultFailure("xrCreateAction", static_cast<int32_t>(status));
            outError = std::string("xrCreateAction failed for '") + name + "': " +
                       XRResultString(static_cast<int32_t>(status));
            return false;
        }
        return true;
    };

    if (!makeAction("grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT, gripPoseAction_) ||
        !makeAction("aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT, aimPoseAction_) ||
        !makeAction("trigger_value", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, triggerValueAction_) ||
        !makeAction("grip_value", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, gripValueAction_) ||
        !makeAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, thumbstickAction_) ||
        !makeAction("thumbstick_click", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT,
                    thumbstickClickAction_) ||
        !makeAction("primary_button", "Primary Button", XR_ACTION_TYPE_BOOLEAN_INPUT,
                    primaryButtonAction_) ||
        !makeAction("secondary_button", "Secondary Button", XR_ACTION_TYPE_BOOLEAN_INPUT,
                    secondaryButtonAction_) ||
        !makeAction("menu_button", "Menu Button", XR_ACTION_TYPE_BOOLEAN_INPUT, menuButtonAction_) ||
        !makeAction("haptic", "Haptic Feedback", XR_ACTION_TYPE_VIBRATION_OUTPUT, hapticAction_)) {
        return false;
    }
    return true;
}

bool XRInput::suggestBindings(const ProjectOpenXRSettings& settings, std::string& outError) {
    const XRInputFunctions& fns = XRLoaderInputFunctions();

    const auto actionForSlot = [&](XRActionSlot slot) -> XrAction {
        switch (slot) {
            case XRActionSlot::GripPose:        return gripPoseAction_;
            case XRActionSlot::AimPose:         return aimPoseAction_;
            case XRActionSlot::TriggerValue:    return triggerValueAction_;
            case XRActionSlot::GripValue:       return gripValueAction_;
            case XRActionSlot::Thumbstick:      return thumbstickAction_;
            case XRActionSlot::ThumbstickClick: return thumbstickClickAction_;
            case XRActionSlot::PrimaryButton:   return primaryButtonAction_;
            case XRActionSlot::SecondaryButton: return secondaryButtonAction_;
            case XRActionSlot::MenuButton:      return menuButtonAction_;
            case XRActionSlot::Haptic:          return hapticAction_;
            default:                            return XR_NULL_HANDLE;
        }
    };

    const std::vector<XRInteractionProfile> profiles =
        settings.activeTarget().effectiveInteractionProfiles();
    if (profiles.empty()) {
        XRLog(XRLogLevel::Warning, "OpenXR Input",
              "No interaction profiles are enabled; controllers will report no input.");
        return true;
    }

    int suggested = 0;
    for (XRInteractionProfile profile : profiles) {
        const XRProfileBindings* table = GetProfileBindings(profile);
        if (!table) continue;

        const XrPath profilePath = stringToPath(table->profilePath);
        if (profilePath == XR_NULL_PATH) continue;

        std::vector<XrActionSuggestedBinding> bindings;
        bindings.reserve(table->entryCount * 2);
        for (size_t i = 0; i < table->entryCount; ++i) {
            const XRBindingEntry& entry = table->entries[i];
            const XrAction action = actionForSlot(entry.slot);
            if (action == XR_NULL_HANDLE) continue;
            const char* paths[2] = { entry.leftPath, entry.rightPath };
            for (const char* path : paths) {
                if (!path) continue;
                const XrPath binding = stringToPath(path);
                if (binding == XR_NULL_PATH) continue;
                bindings.push_back(XrActionSuggestedBinding{ action, binding });
            }
        }
        if (bindings.empty()) continue;

        XrInteractionProfileSuggestedBinding suggestion{};
        suggestion.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
        suggestion.interactionProfile = profilePath;
        suggestion.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggestion.suggestedBindings = bindings.data();

        const XrResult result = fns.SuggestInteractionProfileBindings(instance_, &suggestion);
        if (XR_FAILED(result)) {
            XRLog(XRLogLevel::Warning, "OpenXR Input",
                  std::string("Runtime rejected bindings for ") + DisplayName(profile) + ": " +
                      XRResultString(static_cast<int32_t>(result)));
            continue;
        }
        ++suggested;
        XRLog(XRLogLevel::Info, "OpenXR Input",
              std::string(DisplayName(profile)) + " profile bindings suggested (" +
                  std::to_string(bindings.size()) + " bindings)");
    }

    if (suggested == 0) {
        outError = "The OpenXR runtime accepted no interaction profile bindings, so no "
                   "controller input is possible.";
        return false;
    }
    return true;
}

bool XRInput::createActionSpaces(const XRSession& session, std::string& outError) {
    const XRInputFunctions& fns = XRLoaderInputFunctions();

    const auto makeSpace = [&](XrAction action, int hand, XrSpace& out) {
        XrActionSpaceCreateInfo info{};
        info.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
        info.action = action;
        info.subactionPath = handPaths_[hand];
        info.poseInActionSpace.orientation.w = 1.0f;

        const XrResult status = fns.CreateActionSpace(session.handle(), &info, &out);
        if (XR_FAILED(status)) {
            XRLogResultFailure("xrCreateActionSpace", static_cast<int32_t>(status));
            outError = "xrCreateActionSpace failed: " +
                       XRResultString(static_cast<int32_t>(status));
            return false;
        }
        return true;
    };

    for (int hand = 0; hand < 2; ++hand) {
        if (!makeSpace(gripPoseAction_, hand, gripSpaces_[hand]) ||
            !makeSpace(aimPoseAction_, hand, aimSpaces_[hand])) {
            return false;
        }
    }
    return true;
}

void XRInput::refreshInteractionProfiles(const XRSession& session) {
    if (session.handle() == XR_NULL_HANDLE) return;
    const XRInputFunctions& fns = XRLoaderInputFunctions();
    const XRInstanceFunctions& instanceFns = XRLoaderInstanceFunctions();

    for (int hand = 0; hand < 2; ++hand) {
        XrInteractionProfileState profileState{};
        profileState.type = XR_TYPE_INTERACTION_PROFILE_STATE;
        const XrResult result =
            fns.GetCurrentInteractionProfile(session.handle(), handPaths_[hand], &profileState);

        XRControllerState& controller =
            g_inputState.controllers[hand];
        if (XR_FAILED(result) || profileState.interactionProfile == XR_NULL_PATH) {
            controller.interactionProfile.clear();
            controller.buttonBound.fill(false);
            continue;
        }

        char buffer[XR_MAX_PATH_LENGTH] = {};
        uint32_t written = 0;
        if (XR_SUCCEEDED(instanceFns.PathToString(instance_, profileState.interactionProfile,
                                                  sizeof(buffer), &written, buffer))) {
            controller.interactionProfile.assign(buffer,
                                                 written > 0 ? written - 1 : std::strlen(buffer));
        }

        controller.buttonBound.fill(false);
        for (int i = 0; i < static_cast<int>(XRButton::Count); ++i) {
            const XRActionSlot slot = buttonSlotFor(static_cast<XRButton>(i));
            if (slot == XRActionSlot::Count) continue;
            for (int p = 0; p < static_cast<int>(XRInteractionProfile::Count); ++p) {
                const XRProfileBindings* table =
                    GetProfileBindings(static_cast<XRInteractionProfile>(p));
                if (!table || controller.interactionProfile != table->profilePath) continue;
                for (size_t e = 0; e < table->entryCount; ++e) {
                    if (table->entries[e].slot != slot) continue;
                    const char* path =
                        (hand == 1) ? table->entries[e].rightPath : table->entries[e].leftPath;
                    if (path) controller.buttonBound[static_cast<size_t>(i)] = true;
                }
            }
        }

        XRLog(XRLogLevel::Info, "OpenXR Input",
              std::string(hand == 0 ? "Left" : "Right") + " controller profile: " +
                  (controller.interactionProfile.empty() ? "(none)"
                                                         : controller.interactionProfile));
    }
}

void XRInput::readController(const XRSession& session, XRDevice device,
                             XrTime predictedDisplayTime) {
    const int hand = handIndex(device);
    const XRInputFunctions& fns = XRLoaderInputFunctions();
    const XRSessionFunctions& sessionFns = XRLoaderSessionFunctions();
    XRControllerState& out = g_inputState.controller(device);

    out.previousButtons = out.buttons;

    XrActionStateGetInfo getInfo{};
    getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
    getInfo.subactionPath = handPaths_[hand];

    getInfo.action = gripPoseAction_;
    XrActionStatePose poseState{};
    poseState.type = XR_TYPE_ACTION_STATE_POSE;
    out.active = XR_SUCCEEDED(fns.GetActionStatePose(session_, &getInfo, &poseState)) &&
                 poseState.isActive == XR_TRUE;

    const auto readFloat = [&](XrAction action) {
        getInfo.action = action;
        XrActionStateFloat state{};
        state.type = XR_TYPE_ACTION_STATE_FLOAT;
        if (XR_FAILED(fns.GetActionStateFloat(session_, &getInfo, &state)) ||
            state.isActive != XR_TRUE) {
            return 0.0f;
        }
        return std::clamp(state.currentState, 0.0f, 1.0f);
    };
    out.axes[static_cast<size_t>(XRAxisKind::Trigger)] = readFloat(triggerValueAction_);
    out.axes[static_cast<size_t>(XRAxisKind::Grip)] = readFloat(gripValueAction_);

    getInfo.action = thumbstickAction_;
    XrActionStateVector2f stickState{};
    stickState.type = XR_TYPE_ACTION_STATE_VECTOR2F;
    if (XR_SUCCEEDED(fns.GetActionStateVector2f(session_, &getInfo, &stickState)) &&
        stickState.isActive == XR_TRUE) {
        out.axes2D[static_cast<size_t>(XRAxis2DKind::Thumbstick)] =
            glm::vec2(stickState.currentState.x, stickState.currentState.y);
    } else {
        out.axes2D[static_cast<size_t>(XRAxis2DKind::Thumbstick)] = glm::vec2(0.0f);
    }

    const auto readBool = [&](XrAction action) {
        if (action == XR_NULL_HANDLE) return false;
        getInfo.action = action;
        XrActionStateBoolean state{};
        state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
        if (XR_FAILED(fns.GetActionStateBoolean(session_, &getInfo, &state)) ||
            state.isActive != XR_TRUE) {
            return false;
        }
        return state.currentState == XR_TRUE;
    };

    for (int i = 0; i < static_cast<int>(XRButton::Count); ++i) {
        const auto button = static_cast<XRButton>(i);
        const size_t index = static_cast<size_t>(i);
        if (button == XRButton::Trigger) {
            out.buttons[index] = analogPressed(out.axes[static_cast<size_t>(XRAxisKind::Trigger)],
                                               out.previousButtons[index]);
        } else if (button == XRButton::Grip) {
            out.buttons[index] = analogPressed(out.axes[static_cast<size_t>(XRAxisKind::Grip)],
                                               out.previousButtons[index]);
        } else {
            out.buttons[index] = readBool(buttonActionFor(button, thumbstickClickAction_,
                                                          primaryButtonAction_,
                                                          secondaryButtonAction_,
                                                          menuButtonAction_));
        }
    }

    const auto locate = [&](XrSpace space, XRPose& pose, bool wantVelocity) {
        if (space == XR_NULL_HANDLE || session.appSpace() == XR_NULL_HANDLE) {
            pose = XRPose{};
            return;
        }
        XrSpaceVelocity velocity{};
        velocity.type = XR_TYPE_SPACE_VELOCITY;

        XrSpaceLocation location{};
        location.type = XR_TYPE_SPACE_LOCATION;
        if (wantVelocity) location.next = &velocity;

        if (XR_FAILED(sessionFns.LocateSpace(space, session.appSpace(), predictedDisplayTime,
                                             &location))) {
            pose = XRPose{};
            return;
        }
        pose = ToXRPose(location.pose, location.locationFlags);

        if (wantVelocity) {
            const bool linearValid =
                (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
            const bool angularValid =
                (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
            out.hasVelocity = linearValid || angularValid;
            out.velocity = linearValid ? ToGlm(velocity.linearVelocity) : glm::vec3(0.0f);
            out.angularVelocity =
                angularValid ? ToGlm(velocity.angularVelocity) : glm::vec3(0.0f);
        }
    };

    locate(gripSpaces_[hand], out.gripPose, true);
    locate(aimSpaces_[hand], out.aimPose, false);
}

void XRInput::applyPendingHaptics() {
    std::vector<XRHapticRequest>& queue = hapticQueue();
    if (queue.empty()) return;

    const XRInputFunctions& fns = XRLoaderInputFunctions();
    for (const XRHapticRequest& request : queue) {
        if (request.device == XRDevice::Head) continue;
        const int hand = handIndex(request.device);

        XrHapticVibration vibration{};
        vibration.type = XR_TYPE_HAPTIC_VIBRATION;
        vibration.amplitude = request.amplitude;
        vibration.duration = (request.duration <= 0.0f)
                                 ? XR_MIN_HAPTIC_DURATION
                                 : static_cast<XrDuration>(request.duration * 1e9);
        vibration.frequency =
            (request.frequency <= 0.0f) ? XR_FREQUENCY_UNSPECIFIED : request.frequency;

        XrHapticActionInfo info{};
        info.type = XR_TYPE_HAPTIC_ACTION_INFO;
        info.action = hapticAction_;
        info.subactionPath = handPaths_[hand];

        const XrResult result = fns.ApplyHapticFeedback(
            session_, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
        if (XR_FAILED(result)) {
            continue;
        }
    }
    queue.clear();
}

void XRInput::syncFrame(const XRSession& session, XrTime predictedDisplayTime) {
    g_inputState.sessionActive = session.isRunning();
    g_inputState.hasFocus = session.hasFocus();

    if (!initialized_ || !session.isRunning()) {
        hapticQueue().clear();
        return;
    }

    const XRInputFunctions& fns = XRLoaderInputFunctions();

    XrActiveActionSet activeSet{};
    activeSet.actionSet = actionSet_;
    activeSet.subactionPath = XR_NULL_PATH;

    XrActionsSyncInfo syncInfo{};
    syncInfo.type = XR_TYPE_ACTIONS_SYNC_INFO;
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;

    const XrResult result = fns.SyncActions(session_, &syncInfo);
    if (XR_FAILED(result)) {
        XRLogResultFailure("xrSyncActions", static_cast<int32_t>(result));
        return;
    }
    if (result == XR_SESSION_NOT_FOCUSED) return;

    if (session.viewSpace() != XR_NULL_HANDLE && session.appSpace() != XR_NULL_HANDLE) {
        XrSpaceLocation location{};
        location.type = XR_TYPE_SPACE_LOCATION;
        if (XR_SUCCEEDED(XRLoaderSessionFunctions().LocateSpace(
                session.viewSpace(), session.appSpace(), predictedDisplayTime, &location))) {
            g_inputState.headPose = ToXRPose(location.pose, location.locationFlags);
        }
    }

    readController(session, XRDevice::Left, predictedDisplayTime);
    readController(session, XRDevice::Right, predictedDisplayTime);
    applyPendingHaptics();

    XRDiagnosticsSnapshot& diag = MutableDiagnostics();
    diag.leftControllerActive = g_inputState.controller(XRDevice::Left).active;
    diag.rightControllerActive = g_inputState.controller(XRDevice::Right).active;
    diag.activeInteractionProfileLeft = g_inputState.controller(XRDevice::Left).interactionProfile;
    diag.activeInteractionProfileRight = g_inputState.controller(XRDevice::Right).interactionProfile;
}

void XRInput::shutdown() {
    const XRInputFunctions& fns = XRLoaderInputFunctions();
    const XRSessionFunctions& sessionFns = XRLoaderSessionFunctions();

    for (int hand = 0; hand < 2; ++hand) {
        if (gripSpaces_[hand] != XR_NULL_HANDLE && sessionFns.DestroySpace) {
            sessionFns.DestroySpace(gripSpaces_[hand]);
        }
        if (aimSpaces_[hand] != XR_NULL_HANDLE && sessionFns.DestroySpace) {
            sessionFns.DestroySpace(aimSpaces_[hand]);
        }
        gripSpaces_[hand] = XR_NULL_HANDLE;
        aimSpaces_[hand] = XR_NULL_HANDLE;
        handPaths_[hand] = XR_NULL_PATH;
    }

    if (actionSet_ != XR_NULL_HANDLE && fns.DestroyActionSet) {
        fns.DestroyActionSet(actionSet_);
    }
    actionSet_ = XR_NULL_HANDLE;
    gripPoseAction_ = XR_NULL_HANDLE;
    aimPoseAction_ = XR_NULL_HANDLE;
    triggerValueAction_ = XR_NULL_HANDLE;
    gripValueAction_ = XR_NULL_HANDLE;
    thumbstickAction_ = XR_NULL_HANDLE;
    thumbstickClickAction_ = XR_NULL_HANDLE;
    primaryButtonAction_ = XR_NULL_HANDLE;
    secondaryButtonAction_ = XR_NULL_HANDLE;
    menuButtonAction_ = XR_NULL_HANDLE;
    hapticAction_ = XR_NULL_HANDLE;

    instance_ = XR_NULL_HANDLE;
    session_ = XR_NULL_HANDLE;
    initialized_ = false;
    ResetInputState();
}

#endif // MODULARITY_HAS_OPENXR

}
