#include "XRDiagnostics.h"

#include <algorithm>
#include <cstdio>

#if MODULARITY_HAS_OPENXR
#include "XRLoader.h"
#endif

namespace Modularity::XR {

namespace {

XRLogSink g_logSink = nullptr;

const char* levelPrefix(XRLogLevel level) {
    switch (level) {
        case XRLogLevel::Error:   return "Error";
        case XRLogLevel::Warning: return "Warning";
        case XRLogLevel::Info:
        default:                  return "";
    }
}

XRDiagnosticsSnapshot g_diagnostics;

struct ResultName { int32_t value; const char* name; };
constexpr ResultName kFallbackResultNames[] = {
    {  0, "XR_SUCCESS" },
    {  1, "XR_TIMEOUT_EXPIRED" },
    {  3, "XR_SESSION_LOSS_PENDING" },
    {  4, "XR_EVENT_UNAVAILABLE" },
    {  7, "XR_SPACE_BOUNDS_UNAVAILABLE" },
    {  8, "XR_SESSION_NOT_FOCUSED" },
    {  9, "XR_FRAME_DISCARDED" },
    { -1, "XR_ERROR_VALIDATION_FAILURE" },
    { -2, "XR_ERROR_RUNTIME_FAILURE" },
    { -3, "XR_ERROR_OUT_OF_MEMORY" },
    { -4, "XR_ERROR_API_VERSION_UNSUPPORTED" },
    { -6, "XR_ERROR_INITIALIZATION_FAILED" },
    { -7, "XR_ERROR_FUNCTION_UNSUPPORTED" },
    { -8, "XR_ERROR_FEATURE_UNSUPPORTED" },
    { -9, "XR_ERROR_EXTENSION_NOT_PRESENT" },
    { -10, "XR_ERROR_LIMIT_REACHED" },
    { -11, "XR_ERROR_SIZE_INSUFFICIENT" },
    { -12, "XR_ERROR_HANDLE_INVALID" },
    { -13, "XR_ERROR_INSTANCE_LOST" },
    { -14, "XR_ERROR_SESSION_RUNNING" },
    { -16, "XR_ERROR_SESSION_NOT_RUNNING" },
    { -17, "XR_ERROR_SESSION_LOST" },
    { -18, "XR_ERROR_SYSTEM_INVALID" },
    { -19, "XR_ERROR_PATH_INVALID" },
    { -20, "XR_ERROR_PATH_COUNT_EXCEEDED" },
    { -21, "XR_ERROR_PATH_FORMAT_INVALID" },
    { -22, "XR_ERROR_PATH_UNSUPPORTED" },
    { -23, "XR_ERROR_LAYER_INVALID" },
    { -24, "XR_ERROR_LAYER_LIMIT_EXCEEDED" },
    { -25, "XR_ERROR_SWAPCHAIN_RECT_INVALID" },
    { -26, "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED" },
    { -27, "XR_ERROR_ACTION_TYPE_MISMATCH" },
    { -28, "XR_ERROR_SESSION_NOT_READY" },
    { -29, "XR_ERROR_SESSION_NOT_STOPPING" },
    { -30, "XR_ERROR_TIME_INVALID" },
    { -31, "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED" },
    { -32, "XR_ERROR_FILE_ACCESS_ERROR" },
    { -33, "XR_ERROR_FILE_CONTENTS_INVALID" },
    { -34, "XR_ERROR_FORM_FACTOR_UNSUPPORTED" },
    { -35, "XR_ERROR_FORM_FACTOR_UNAVAILABLE" },
    { -36, "XR_ERROR_API_LAYER_NOT_PRESENT" },
    { -37, "XR_ERROR_CALL_ORDER_INVALID" },
    { -38, "XR_ERROR_GRAPHICS_DEVICE_INVALID" },
    { -39, "XR_ERROR_POSE_INVALID" },
    { -40, "XR_ERROR_INDEX_OUT_OF_RANGE" },
    { -41, "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED" },
    { -42, "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED" },
    { -44, "XR_ERROR_NAME_DUPLICATED" },
    { -45, "XR_ERROR_NAME_INVALID" },
    { -46, "XR_ERROR_ACTIONSET_NOT_ATTACHED" },
    { -47, "XR_ERROR_ACTIONSETS_ALREADY_ATTACHED" },
    { -48, "XR_ERROR_LOCALIZED_NAME_DUPLICATED" },
    { -49, "XR_ERROR_LOCALIZED_NAME_INVALID" },
    { -50, "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING" },
    { -51, "XR_ERROR_RUNTIME_UNAVAILABLE" },
};

} // namespace

void SetXRLogSink(XRLogSink sink) { g_logSink = sink; }

void XRLog(XRLogLevel level, const std::string& subsystem, const std::string& message) {
    const char* suffix = levelPrefix(level);
    std::string tag = "[" + subsystem;
    if (*suffix) {
        tag += ' ';
        tag += suffix;
    }
    tag += ']';

    std::fprintf(stderr, "%s %s\n", tag.c_str(), message.c_str());
    if (g_logSink) g_logSink(level, subsystem.c_str(), message.c_str());
}

std::string XRResultString(int32_t result) {
#if MODULARITY_HAS_OPENXR
    std::string fromRuntime;
    if (XRLoaderResultToString(result, fromRuntime) && !fromRuntime.empty()) {
        return fromRuntime;
    }
#endif
    for (const ResultName& entry : kFallbackResultNames) {
        if (entry.value == result) return entry.name;
    }
    return "XR_UNKNOWN_RESULT(" + std::to_string(result) + ")";
}

void XRLogResultFailure(const char* call, int32_t result) {
    const std::string message =
        std::string(call ? call : "OpenXR call") + " failed: " + XRResultString(result);
    XRLog(XRLogLevel::Error, "OpenXR", message);
    g_diagnostics.lastError = message;
}

bool XRDiagnosticsSnapshot::hasExtension(const std::string& name) const {
    return std::find(availableExtensions.begin(), availableExtensions.end(), name) !=
           availableExtensions.end();
}

bool XRDiagnosticsSnapshot::isExtensionEnabled(const std::string& name) const {
    return std::find(enabledExtensions.begin(), enabledExtensions.end(), name) !=
           enabledExtensions.end();
}

XRDiagnosticsSnapshot& MutableDiagnostics() { return g_diagnostics; }
const XRDiagnosticsSnapshot& Diagnostics() { return g_diagnostics; }

void ResetDiagnostics() {
    g_diagnostics = XRDiagnosticsSnapshot{};
    g_diagnostics.compiledIn = (MODULARITY_HAS_OPENXR != 0);
}

}
