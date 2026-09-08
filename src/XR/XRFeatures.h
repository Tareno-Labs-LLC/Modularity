#pragma once

#include "XRSettings.h"

#include <string>
#include <vector>

namespace Modularity::XR {

enum class XRFeature {
    OpenGLESGraphics,
    OpenGLGraphics,
    CompositionLayerDepth,
    AndroidLoaderInit,
    AndroidCreateInstance,

    HandTracking,
    VisibilityMask,
    PerformanceSettings,

    QuestDisplayRefreshRate,
    QuestPassthrough,
    QuestFoveatedRendering,
    QuestDisplayUtilities,

    Count
};

enum class XRFeatureGroup {
    Implicit = 0,
    BaseRuntime,
    MetaQuest,
    Count
};

const char* DisplayName(XRFeatureGroup group);
const char* Description(XRFeatureGroup group);
bool IsFeatureGroupUsedOn(XRFeatureGroup group, XRPlatformTarget target);

struct XRFeatureInfo {
    XRFeature feature;
    const char* extensionName;
    const char* displayName;
    const char* serializationId;
    XRFeatureGroup group;
    unsigned targetMask;
    bool implemented;
};

const XRFeatureInfo& GetFeatureInfo(XRFeature feature);
const char* FeatureExtensionName(XRFeature feature);
const char* FeatureDisplayName(XRFeature feature);
const char* FeatureSerializationId(XRFeature feature);
bool IsFeatureImplemented(XRFeature feature);
bool IsFeatureAvailableOn(XRFeature feature, XRPlatformTarget target);
bool XRFeatureFromString(const std::string& id, XRFeature& out);

const std::vector<XRFeatureInfo>& AllFeatures();

bool IsFeatureRequested(const ProjectOpenXRSettings& settings, XRFeature feature);

struct XRSkippedFeature {
    XRFeature feature;
    std::string reason;
};

std::vector<const char*> BuildRequestedExtensions(const ProjectOpenXRSettings& settings,
                                                  const std::vector<std::string>& available,
                                                  std::vector<XRSkippedFeature>& outSkipped);

}
