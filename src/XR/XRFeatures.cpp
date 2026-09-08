#include "XRFeatures.h"

#include <algorithm>

namespace Modularity::XR {

namespace {

constexpr unsigned kMaskPC = 1u << static_cast<unsigned>(XRPlatformTarget::PC);
constexpr unsigned kMaskQuest = 1u << static_cast<unsigned>(XRPlatformTarget::MetaQuest);
constexpr unsigned kMaskAndroid = 1u << static_cast<unsigned>(XRPlatformTarget::Android);
constexpr unsigned kMaskMobile = kMaskQuest | kMaskAndroid;
constexpr unsigned kMaskAll = kMaskPC | kMaskQuest | kMaskAndroid;

const std::vector<XRFeatureInfo>& featureTable() {
    static const std::vector<XRFeatureInfo> table = {
        { XRFeature::OpenGLESGraphics,        "XR_KHR_opengl_es_enable",
          "OpenGL ES Graphics Binding",       "openGLESGraphics",
          XRFeatureGroup::Implicit,           kMaskMobile,   true  },
        { XRFeature::OpenGLGraphics,          "XR_KHR_opengl_enable",
          "OpenGL Graphics Binding",          "openGLGraphics",
          XRFeatureGroup::Implicit,           kMaskPC,       true  },
        { XRFeature::CompositionLayerDepth,   "XR_KHR_composition_layer_depth",
          "Depth Submission",                 "compositionLayerDepth",
          XRFeatureGroup::Implicit,           kMaskAll,      true  },
        { XRFeature::AndroidLoaderInit,       "XR_KHR_loader_init_android",
          "Android Loader Init",              "androidLoaderInit",
          XRFeatureGroup::Implicit,           kMaskMobile,   true  },
        { XRFeature::AndroidCreateInstance,   "XR_KHR_android_create_instance",
          "Android Instance Create Info",     "androidCreateInstance",
          XRFeatureGroup::Implicit,           kMaskMobile,   true  },

        { XRFeature::HandTracking,            "XR_EXT_hand_tracking",
          "Hand Tracking",                    "handTracking",
          XRFeatureGroup::BaseRuntime,        kMaskAll,      false },
        { XRFeature::VisibilityMask,          "XR_KHR_visibility_mask",
          "Visibility Mask",                  "visibilityMask",
          XRFeatureGroup::BaseRuntime,        kMaskAll,      false },
        { XRFeature::PerformanceSettings,     "XR_EXT_performance_settings",
          "Performance Settings",             "performanceSettings",
          XRFeatureGroup::BaseRuntime,        kMaskAll,      false },

        { XRFeature::QuestDisplayRefreshRate, "XR_FB_display_refresh_rate",
          "Display Refresh Rate",             "displayRefreshRate",
          XRFeatureGroup::MetaQuest,          kMaskQuest,    true  },
        { XRFeature::QuestPassthrough,        "XR_FB_passthrough",
          "Passthrough",                      "passthrough",
          XRFeatureGroup::MetaQuest,          kMaskQuest,    false },
        { XRFeature::QuestFoveatedRendering,  "XR_FB_foveation",
          "Foveated Rendering",               "foveatedRendering",
          XRFeatureGroup::MetaQuest,          kMaskQuest,    false },
        { XRFeature::QuestDisplayUtilities,   "XR_META_performance_metrics",
          "Display Utilities",                "displayUtilities",
          XRFeatureGroup::MetaQuest,          kMaskQuest,    false },
    };
    return table;
}

struct FeatureGroupInfo {
    const char* displayName;
    const char* description;
};

constexpr FeatureGroupInfo kGroups[] = {
    { "Implicit", "Follows from the platform target; not a user choice." },
    { "Base Runtime", "Extensions any OpenXR runtime may offer, whatever the vendor." },
    { "Meta Quest", "Meta's own extensions. Only meaningful on Quest hardware." },
};

static_assert(sizeof(kGroups) / sizeof(kGroups[0]) ==
                  static_cast<size_t>(XRFeatureGroup::Count),
              "kGroups must have one row per XRFeatureGroup");

bool contains(const std::vector<std::string>& list, const char* name) {
    return std::find(list.begin(), list.end(), std::string(name)) != list.end();
}

} // namespace

const char* DisplayName(XRFeatureGroup group) {
    const size_t index = static_cast<size_t>(group);
    if (index >= static_cast<size_t>(XRFeatureGroup::Count)) return kGroups[0].displayName;
    return kGroups[index].displayName;
}

const char* Description(XRFeatureGroup group) {
    const size_t index = static_cast<size_t>(group);
    if (index >= static_cast<size_t>(XRFeatureGroup::Count)) return kGroups[0].description;
    return kGroups[index].description;
}

bool IsFeatureGroupUsedOn(XRFeatureGroup group, XRPlatformTarget target) {
    if (group == XRFeatureGroup::Implicit) return false;
    for (const XRFeatureInfo& info : featureTable()) {
        if (info.group != group) continue;
        if (IsFeatureAvailableOn(info.feature, target)) return true;
    }
    return false;
}

const std::vector<XRFeatureInfo>& AllFeatures() { return featureTable(); }

const XRFeatureInfo& GetFeatureInfo(XRFeature feature) {
    const std::vector<XRFeatureInfo>& table = featureTable();
    const size_t index = static_cast<size_t>(feature);
    if (index >= table.size()) return table[0];
    return table[index];
}

const char* FeatureExtensionName(XRFeature feature) { return GetFeatureInfo(feature).extensionName; }
const char* FeatureDisplayName(XRFeature feature) { return GetFeatureInfo(feature).displayName; }
const char* FeatureSerializationId(XRFeature feature) { return GetFeatureInfo(feature).serializationId; }
bool IsFeatureImplemented(XRFeature feature) { return GetFeatureInfo(feature).implemented; }

bool IsFeatureAvailableOn(XRFeature feature, XRPlatformTarget target) {
    const size_t index = static_cast<size_t>(target);
    if (index >= static_cast<size_t>(XRPlatformTarget::Count)) return false;
    return (GetFeatureInfo(feature).targetMask & (1u << index)) != 0u;
}

bool XRFeatureFromString(const std::string& id, XRFeature& out) {
    if (id.empty()) return false;
    for (const XRFeatureInfo& info : featureTable()) {
        if (id == info.serializationId) {
            out = info.feature;
            return true;
        }
    }
    return false;
}

bool IsFeatureRequested(const ProjectOpenXRSettings& settings, XRFeature feature) {
    const XRPlatformTarget active = ActivePlatformTarget();
    if (!IsFeatureAvailableOn(feature, active)) return false;

    const XRTargetSettings& target = settings.activeTarget();
    switch (feature) {
        case XRFeature::OpenGLESGraphics:
            return GraphicsBackendFor(active) == XRGraphicsBackend::OpenGLES;
        case XRFeature::OpenGLGraphics:
            return GraphicsBackendFor(active) == XRGraphicsBackend::OpenGL;
        case XRFeature::AndroidLoaderInit:
        case XRFeature::AndroidCreateInstance:
#if defined(__ANDROID__)
            return true;
#else
            return false;
#endif
        case XRFeature::CompositionLayerDepth:
            return target.depthSubmission == XRDepthSubmission::Depth;

        default:
            break;
    }
    return target.hasFeature(FeatureSerializationId(feature));
}

std::vector<const char*> BuildRequestedExtensions(const ProjectOpenXRSettings& settings,
                                                  const std::vector<std::string>& available,
                                                  std::vector<XRSkippedFeature>& outSkipped) {
    outSkipped.clear();
    std::vector<const char*> requested;
    requested.reserve(featureTable().size());

    for (const XRFeatureInfo& info : featureTable()) {
        if (!IsFeatureRequested(settings, info.feature)) continue;

        if (!info.implemented) {
            outSkipped.push_back({ info.feature,
                                   std::string(info.displayName) +
                                       " is not implemented in this build yet" });
            continue;
        }
        if (!contains(available, info.extensionName)) {
            outSkipped.push_back({ info.feature,
                                   std::string(info.extensionName) +
                                       " is not offered by this OpenXR runtime" });
            continue;
        }
        requested.push_back(info.extensionName);
    }
    return requested;
}

}
