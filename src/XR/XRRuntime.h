#pragma once

#include "XRDiagnostics.h"
#include "XRFeatures.h"
#include "XRSettings.h"

#include <cstdint>
#include <string>
#include <vector>

#if MODULARITY_HAS_OPENXR
#include "XRPlatform.h"
#endif

namespace Modularity::XR {

struct XRViewInfo {
    uint32_t recommendedWidth = 0;
    uint32_t recommendedHeight = 0;
    uint32_t maxWidth = 0;
    uint32_t maxHeight = 0;
    uint32_t recommendedSampleCount = 1;
    uint32_t maxSampleCount = 1;
};

class XRRuntime {
public:
    XRRuntime() = default;
    ~XRRuntime();

    XRRuntime(const XRRuntime&) = delete;
    XRRuntime& operator=(const XRRuntime&) = delete;

    bool initialize(const ProjectOpenXRSettings& settings,
                    void* applicationVM,
                    void* applicationActivity,
                    std::string& outError);

    void shutdown();

    bool isInitialized() const { return initialized_; }

    bool hasSystem() const { return hasSystem_; }

    const std::string& runtimeName() const { return runtimeName_; }
    const std::string& systemName() const { return systemName_; }
    const std::vector<std::string>& availableExtensions() const { return availableExtensions_; }
    const std::vector<std::string>& enabledExtensions() const { return enabledExtensions_; }

    bool isExtensionEnabled(const char* name) const;
    bool isFeatureActive(XRFeature feature) const;

    const std::vector<XRViewInfo>& views() const { return views_; }
    uint32_t viewCount() const { return static_cast<uint32_t>(views_.size()); }

#if MODULARITY_HAS_OPENXR
    XrInstance instance() const { return instance_; }
    XrSystemId systemId() const { return systemId_; }
    XrViewConfigurationType viewConfigurationType() const { return viewConfigurationType_; }
    const std::vector<XrEnvironmentBlendMode>& blendModes() const { return blendModes_; }
#endif

private:
#if MODULARITY_HAS_OPENXR
    bool initializeAndroidLoader(void* applicationVM, void* applicationActivity,
                                 std::string& outError);
    bool enumerateExtensions(std::string& outError);
    bool createInstance(const ProjectOpenXRSettings& settings, void* applicationVM,
                        void* applicationActivity, std::string& outError);
    bool queryInstanceProperties(std::string& outError);
    bool acquireSystem(std::string& outError);
    bool queryViewConfiguration(std::string& outError);
    void publishDiagnostics(const ProjectOpenXRSettings& settings);

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrViewConfigurationType viewConfigurationType_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrEnvironmentBlendMode> blendModes_;
#endif

    bool initialized_ = false;
    bool hasSystem_ = false;
    std::string runtimeName_;
    std::string systemName_;
    std::vector<std::string> availableExtensions_;
    std::vector<std::string> enabledExtensions_;
    std::vector<XRViewInfo> views_;
};

}
