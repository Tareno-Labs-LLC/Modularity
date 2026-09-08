#pragma once

#include <array>
#include <string>
#include <vector>

namespace Modularity::XR {

enum class XRGraphicsBackend {
    OpenGLES = 0,
    OpenGL = 1,
};

enum class XRRenderMode {
    SinglePassMultiview = 0,
    MultiPass = 1,
};

enum class XRDepthSubmission {
    None = 0,
    Depth = 1,
};

enum class XRTrackingOrigin {
    Floor = 0,
    Eye = 1,
};

enum class XRPlatformTarget {
    PC = 0,
    MetaQuest = 1,
    Android = 2,
    Count
};

enum class XRInteractionProfile {
    MetaQuestTouch = 0,
    ValveIndex = 1,
    HtcVive = 2,
    MicrosoftMotionController = 3,
    KhronosSimpleController = 4,
    Count
};

const char* ToString(XRGraphicsBackend backend);
const char* ToString(XRRenderMode mode);
const char* ToString(XRDepthSubmission depth);
const char* ToString(XRTrackingOrigin origin);

const char* SerializationId(XRPlatformTarget target);
const char* DisplayName(XRPlatformTarget target);
const char* Description(XRPlatformTarget target);
const char* TabIconPath(XRPlatformTarget target);
XRGraphicsBackend GraphicsBackendFor(XRPlatformTarget target);
bool XRPlatformTargetFromString(const std::string& value, XRPlatformTarget& out);

XRPlatformTarget ActivePlatformTarget();

bool IsPlatformTargetSupported(XRPlatformTarget target);

const char* SerializationId(XRInteractionProfile profile);
const char* DisplayName(XRInteractionProfile profile);
const char* InteractionProfilePath(XRInteractionProfile profile);
bool IsInteractionProfileImplemented(XRInteractionProfile profile);
bool IsInteractionProfileAvailableOn(XRInteractionProfile profile, XRPlatformTarget target);

std::vector<XRInteractionProfile> DefaultInteractionProfiles(XRPlatformTarget target);

XRRenderMode XRRenderModeFromString(const std::string& value);
XRDepthSubmission XRDepthSubmissionFromString(const std::string& value);
XRTrackingOrigin XRTrackingOriginFromString(const std::string& value);
bool XRInteractionProfileFromString(const std::string& value, XRInteractionProfile& out);

struct XRTargetSettings {
    bool enabled = false;

    XRRenderMode renderMode = XRRenderMode::SinglePassMultiview;
    XRDepthSubmission depthSubmission = XRDepthSubmission::None;

    std::vector<XRInteractionProfile> interactionProfiles;

    std::vector<std::string> enabledFeatures;

    bool hasInteractionProfile(XRInteractionProfile profile) const;
    void setInteractionProfile(XRInteractionProfile profile, bool on);
    std::vector<XRInteractionProfile> effectiveInteractionProfiles() const;

    bool hasFeature(const char* featureId) const;
    void setFeature(const char* featureId, bool on);
};

struct XRPCRuntimeSettings {
    std::string runtimeManifestPath;
    std::string runtimeDisplayName;
    bool startOnPlay = true;
};

struct XRInteractionSettings {
    bool enabled = true;
    std::string defaultActionSet;
};

std::array<XRTargetSettings, static_cast<size_t>(XRPlatformTarget::Count)> DefaultTargetSettings();

struct ProjectOpenXRSettings {
    bool enabled = false;

    XRTrackingOrigin trackingOrigin = XRTrackingOrigin::Floor;

    std::array<XRTargetSettings, static_cast<size_t>(XRPlatformTarget::Count)> targets =
        DefaultTargetSettings();

    XRPCRuntimeSettings pc;
    XRInteractionSettings interaction;

    XRTargetSettings& target(XRPlatformTarget which);
    const XRTargetSettings& target(XRPlatformTarget which) const;

    XRTargetSettings& activeTarget() { return target(ActivePlatformTarget()); }
    const XRTargetSettings& activeTarget() const { return target(ActivePlatformTarget()); }

    XRGraphicsBackend activeGraphicsBackend() const {
        return GraphicsBackendFor(ActivePlatformTarget());
    }

    bool isEnabledForActiveTarget() const { return enabled && activeTarget().enabled; }
};

}
