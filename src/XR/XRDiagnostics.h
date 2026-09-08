#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Modularity::XR {

enum class XRLogLevel { Info, Warning, Error };

void XRLog(XRLogLevel level, const std::string& subsystem, const std::string& message);

inline void XRLogInfo(const std::string& message) { XRLog(XRLogLevel::Info, "OpenXR", message); }
inline void XRLogWarn(const std::string& message) { XRLog(XRLogLevel::Warning, "OpenXR", message); }
inline void XRLogError(const std::string& message) { XRLog(XRLogLevel::Error, "OpenXR", message); }

using XRLogSink = void (*)(XRLogLevel level, const char* subsystem, const char* message);
void SetXRLogSink(XRLogSink sink);

std::string XRResultString(int32_t result);

void XRLogResultFailure(const char* call, int32_t result);

struct XRDiagnosticsSnapshot {
    bool compiledIn = false;
    bool loaderAvailable = false;
    bool instanceCreated = false;
    bool systemAcquired = false;
    bool sessionCreated = false;
    std::string lastError;
    std::string loaderPath;

    std::string runtimeName;
    std::string runtimeVersion;
    std::string openXRVersion;
    std::string systemName;
    std::string platformTarget;
    std::string runtimeOverridePath;

    std::string graphicsApi;
    std::string graphicsVersion;
    std::string swapchainFormat;

    uint32_t viewCount = 0;
    std::string viewConfiguration;
    uint32_t recommendedWidth = 0;
    uint32_t recommendedHeight = 0;
    uint32_t maxWidth = 0;
    uint32_t maxHeight = 0;
    uint32_t recommendedSwapchainSampleCount = 0;
    float refreshRateHz = 0.0f;
    std::vector<std::string> blendModes;

    bool multiviewSupported = false;
    bool depthSubmissionSupported = false;
    bool handTrackingSupported = false;
    bool passthroughSupported = false;
    bool displayRefreshRateSupported = false;
    bool foveatedRenderingSupported = false;

    std::string activeRenderMode;
    std::string activeDepthSubmission;
    std::string activeTrackingOrigin;

    std::vector<std::string> availableExtensions;
    std::vector<std::string> enabledExtensions;

    std::string activeInteractionProfileLeft;
    std::string activeInteractionProfileRight;
    bool leftControllerActive = false;
    bool rightControllerActive = false;

    bool hasExtension(const std::string& name) const;
    bool isExtensionEnabled(const std::string& name) const;
};

XRDiagnosticsSnapshot& MutableDiagnostics();
const XRDiagnosticsSnapshot& Diagnostics();
void ResetDiagnostics();

}
