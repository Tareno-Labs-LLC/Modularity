#include "XRSettings.h"

#include <algorithm>
#include <cctype>

namespace Modularity::XR {

namespace {

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

struct PlatformTargetInfo {
    const char* id;
    const char* displayName;
    const char* description;
    const char* iconPath;
    XRGraphicsBackend backend;
};

constexpr PlatformTargetInfo kTargets[] = {
    { "pc", "PC",
      "Windows and Linux desktop headsets driven by a PC OpenXR runtime - SteamVR, "
      "Meta Quest Link / Air Link, Windows Mixed Reality, Virtual Desktop.",
      "Resources/Engine-Root/Project Settings/OpenXR Tab/PCTab.png",
      XRGraphicsBackend::OpenGL },
    { "metaQuest", "Meta Quest",
      "Quest 2, Quest 3 and Quest 3S running the build standalone on the headset.",
      "Resources/Engine-Root/Project Settings/OpenXR Tab/MetaQuestTab.png",
      XRGraphicsBackend::OpenGLES },
    { "android", "Android",
      "Other Android XR devices. Shares the Quest code path but none of the Meta "
      "extensions, and has not been brought up on hardware.",
      "Resources/Engine-Root/Project Settings/OpenXR Tab/AndroidTab.png",
      XRGraphicsBackend::OpenGLES },
};

static_assert(sizeof(kTargets) / sizeof(kTargets[0]) ==
                  static_cast<size_t>(XRPlatformTarget::Count),
              "kTargets must have one row per XRPlatformTarget");

const PlatformTargetInfo& targetInfo(XRPlatformTarget target) {
    const size_t index = static_cast<size_t>(target);
    if (index >= static_cast<size_t>(XRPlatformTarget::Count)) return kTargets[0];
    return kTargets[index];
}

struct InteractionProfileInfo {
    const char* id;
    const char* displayName;
    const char* path;
    bool implemented;
    unsigned targetMask;
};

constexpr unsigned kMaskPC = 1u << static_cast<unsigned>(XRPlatformTarget::PC);
constexpr unsigned kMaskQuest = 1u << static_cast<unsigned>(XRPlatformTarget::MetaQuest);
constexpr unsigned kMaskAndroid = 1u << static_cast<unsigned>(XRPlatformTarget::Android);
constexpr unsigned kMaskAll = kMaskPC | kMaskQuest | kMaskAndroid;

constexpr InteractionProfileInfo kProfiles[] = {
    { "metaQuestTouch", "Meta Quest Touch Controller",
      "/interaction_profiles/oculus/touch_controller", true, kMaskAll },
    { "valveIndex", "Valve Index Controller",
      "/interaction_profiles/valve/index_controller", true, kMaskPC },
    { "htcVive", "HTC Vive Controller",
      "/interaction_profiles/htc/vive_controller", true, kMaskPC },
    { "microsoftMotionController", "Microsoft Motion Controller",
      "/interaction_profiles/microsoft/motion_controller", true, kMaskPC },
    { "khronosSimple", "Khronos Simple Controller",
      "/interaction_profiles/khr/simple_controller", true, kMaskAll },
};

static_assert(sizeof(kProfiles) / sizeof(kProfiles[0]) ==
                  static_cast<size_t>(XRInteractionProfile::Count),
              "kProfiles must have one row per XRInteractionProfile");

const InteractionProfileInfo& info(XRInteractionProfile profile) {
    const size_t index = static_cast<size_t>(profile);
    if (index >= static_cast<size_t>(XRInteractionProfile::Count)) {
        return kProfiles[0];
    }
    return kProfiles[index];
}

} // namespace

const char* ToString(XRGraphicsBackend backend) {
    switch (backend) {
        case XRGraphicsBackend::OpenGL: return "OpenGL";
        case XRGraphicsBackend::OpenGLES:
        default: return "OpenGL ES";
    }
}

const char* ToString(XRRenderMode mode) {
    switch (mode) {
        case XRRenderMode::MultiPass: return "Multi Pass";
        case XRRenderMode::SinglePassMultiview:
        default: return "Single Pass / Multiview";
    }
}

const char* ToString(XRDepthSubmission depth) {
    switch (depth) {
        case XRDepthSubmission::Depth: return "Depth";
        case XRDepthSubmission::None:
        default: return "None";
    }
}

const char* ToString(XRTrackingOrigin origin) {
    switch (origin) {
        case XRTrackingOrigin::Eye: return "Eye Level";
        case XRTrackingOrigin::Floor:
        default: return "Floor";
    }
}

const char* SerializationId(XRPlatformTarget target) { return targetInfo(target).id; }
const char* DisplayName(XRPlatformTarget target) { return targetInfo(target).displayName; }
const char* Description(XRPlatformTarget target) { return targetInfo(target).description; }
const char* TabIconPath(XRPlatformTarget target) { return targetInfo(target).iconPath; }
XRGraphicsBackend GraphicsBackendFor(XRPlatformTarget target) { return targetInfo(target).backend; }

bool XRPlatformTargetFromString(const std::string& value, XRPlatformTarget& out) {
    const std::string v = toLower(value);
    for (int i = 0; i < static_cast<int>(XRPlatformTarget::Count); ++i) {
        if (toLower(kTargets[i].id) == v) {
            out = static_cast<XRPlatformTarget>(i);
            return true;
        }
    }
    return false;
}

XRPlatformTarget ActivePlatformTarget() {
#if defined(__ANDROID__)
    return XRPlatformTarget::MetaQuest;
#else
    return XRPlatformTarget::PC;
#endif
}

bool IsPlatformTargetSupported(XRPlatformTarget target) {
#if !MODULARITY_HAS_OPENXR
    (void)target;
    return false;
#else
    switch (target) {
        case XRPlatformTarget::PC:
#if defined(_WIN32) && !MODULARITY_OPENGL_ES
            return true;
#else
            return false;
#endif
        case XRPlatformTarget::MetaQuest:
        case XRPlatformTarget::Android:
#if defined(__ANDROID__)
            return true;
#else
            return false;
#endif
        default:
            return false;
    }
#endif
}

const char* SerializationId(XRInteractionProfile profile) { return info(profile).id; }
const char* DisplayName(XRInteractionProfile profile) { return info(profile).displayName; }
const char* InteractionProfilePath(XRInteractionProfile profile) { return info(profile).path; }
bool IsInteractionProfileImplemented(XRInteractionProfile profile) { return info(profile).implemented; }

bool IsInteractionProfileAvailableOn(XRInteractionProfile profile, XRPlatformTarget target) {
    const size_t index = static_cast<size_t>(target);
    if (index >= static_cast<size_t>(XRPlatformTarget::Count)) return false;
    return (info(profile).targetMask & (1u << index)) != 0u;
}

std::vector<XRInteractionProfile> DefaultInteractionProfiles(XRPlatformTarget target) {
    switch (target) {
        case XRPlatformTarget::PC:
            return { XRInteractionProfile::MetaQuestTouch,
                     XRInteractionProfile::ValveIndex,
                     XRInteractionProfile::HtcVive,
                     XRInteractionProfile::MicrosoftMotionController,
                     XRInteractionProfile::KhronosSimpleController };
        case XRPlatformTarget::MetaQuest:
            return { XRInteractionProfile::MetaQuestTouch,
                     XRInteractionProfile::KhronosSimpleController };
        case XRPlatformTarget::Android:
        default:
            return { XRInteractionProfile::KhronosSimpleController };
    }
}

XRRenderMode XRRenderModeFromString(const std::string& value) {
    const std::string v = toLower(value);
    if (v == "multipass" || v == "multi pass" || v == "1") return XRRenderMode::MultiPass;
    return XRRenderMode::SinglePassMultiview;
}

XRDepthSubmission XRDepthSubmissionFromString(const std::string& value) {
    const std::string v = toLower(value);
    if (v == "depth" || v == "1") return XRDepthSubmission::Depth;
    return XRDepthSubmission::None;
}

XRTrackingOrigin XRTrackingOriginFromString(const std::string& value) {
    const std::string v = toLower(value);
    if (v == "eye" || v == "eyelevel" || v == "eye level" || v == "local" || v == "1") {
        return XRTrackingOrigin::Eye;
    }
    return XRTrackingOrigin::Floor;
}

bool XRInteractionProfileFromString(const std::string& value, XRInteractionProfile& out) {
    const std::string v = toLower(value);
    for (int i = 0; i < static_cast<int>(XRInteractionProfile::Count); ++i) {
        if (toLower(kProfiles[i].id) == v) {
            out = static_cast<XRInteractionProfile>(i);
            return true;
        }
    }
    return false;
}

bool XRTargetSettings::hasInteractionProfile(XRInteractionProfile profile) const {
    return std::find(interactionProfiles.begin(), interactionProfiles.end(), profile) !=
           interactionProfiles.end();
}

void XRTargetSettings::setInteractionProfile(XRInteractionProfile profile, bool on) {
    const auto it = std::find(interactionProfiles.begin(), interactionProfiles.end(), profile);
    if (on && it == interactionProfiles.end()) {
        interactionProfiles.push_back(profile);
    } else if (!on && it != interactionProfiles.end()) {
        interactionProfiles.erase(it);
    }
}

std::vector<XRInteractionProfile> XRTargetSettings::effectiveInteractionProfiles() const {
    std::vector<XRInteractionProfile> result;
    result.reserve(interactionProfiles.size());
    for (XRInteractionProfile profile : interactionProfiles) {
        if (IsInteractionProfileImplemented(profile)) result.push_back(profile);
    }
    return result;
}

bool XRTargetSettings::hasFeature(const char* featureId) const {
    if (!featureId || !*featureId) return false;
    return std::find(enabledFeatures.begin(), enabledFeatures.end(), std::string(featureId)) !=
           enabledFeatures.end();
}

void XRTargetSettings::setFeature(const char* featureId, bool on) {
    if (!featureId || !*featureId) return;
    const std::string id(featureId);
    const auto it = std::find(enabledFeatures.begin(), enabledFeatures.end(), id);
    if (on && it == enabledFeatures.end()) {
        enabledFeatures.push_back(id);
    } else if (!on && it != enabledFeatures.end()) {
        enabledFeatures.erase(it);
    }
}

std::array<XRTargetSettings, static_cast<size_t>(XRPlatformTarget::Count)> DefaultTargetSettings() {
    std::array<XRTargetSettings, static_cast<size_t>(XRPlatformTarget::Count)> defaults{};
    for (int i = 0; i < static_cast<int>(XRPlatformTarget::Count); ++i) {
        const auto which = static_cast<XRPlatformTarget>(i);
        XRTargetSettings& target = defaults[static_cast<size_t>(i)];
        target.enabled = false;
        target.renderMode = (which == XRPlatformTarget::PC) ? XRRenderMode::MultiPass
                                                            : XRRenderMode::SinglePassMultiview;
        target.depthSubmission = XRDepthSubmission::None;
        target.interactionProfiles = DefaultInteractionProfiles(which);
        target.enabledFeatures.clear();
    }
    return defaults;
}

XRTargetSettings& ProjectOpenXRSettings::target(XRPlatformTarget which) {
    const size_t index = static_cast<size_t>(which);
    if (index >= targets.size()) return targets[0];
    return targets[index];
}

const XRTargetSettings& ProjectOpenXRSettings::target(XRPlatformTarget which) const {
    const size_t index = static_cast<size_t>(which);
    if (index >= targets.size()) return targets[0];
    return targets[index];
}

}
