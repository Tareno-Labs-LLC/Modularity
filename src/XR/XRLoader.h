#pragma once

#include <cstdint>
#include <string>
#include <vector>

#if MODULARITY_HAS_OPENXR

#include "XRPlatform.h"

namespace Modularity::XR {

struct XRGlobalFunctions {
    PFN_xrGetInstanceProcAddr                     GetInstanceProcAddr = nullptr;
    PFN_xrEnumerateApiLayerProperties             EnumerateApiLayerProperties = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties    EnumerateInstanceExtensionProperties = nullptr;
    PFN_xrCreateInstance                          CreateInstance = nullptr;
#ifdef XR_KHR_loader_init
    PFN_xrInitializeLoaderKHR                     InitializeLoaderKHR = nullptr;
#endif
};

struct XRInstanceFunctions {
    PFN_xrDestroyInstance                         DestroyInstance = nullptr;
    PFN_xrGetInstanceProperties                   GetInstanceProperties = nullptr;
    PFN_xrResultToString                          ResultToString = nullptr;
    PFN_xrStructureTypeToString                   StructureTypeToString = nullptr;
    PFN_xrGetSystem                               GetSystem = nullptr;
    PFN_xrGetSystemProperties                     GetSystemProperties = nullptr;
    PFN_xrEnumerateViewConfigurations             EnumerateViewConfigurations = nullptr;
    PFN_xrGetViewConfigurationProperties          GetViewConfigurationProperties = nullptr;
    PFN_xrEnumerateViewConfigurationViews         EnumerateViewConfigurationViews = nullptr;
    PFN_xrEnumerateEnvironmentBlendModes          EnumerateEnvironmentBlendModes = nullptr;
    PFN_xrStringToPath                            StringToPath = nullptr;
    PFN_xrPathToString                            PathToString = nullptr;
    PFN_xrPollEvent                               PollEvent = nullptr;
};

struct XRSessionFunctions {
    PFN_xrCreateSession                           CreateSession = nullptr;
    PFN_xrDestroySession                          DestroySession = nullptr;
    PFN_xrBeginSession                            BeginSession = nullptr;
    PFN_xrEndSession                              EndSession = nullptr;
    PFN_xrRequestExitSession                      RequestExitSession = nullptr;

    PFN_xrEnumerateReferenceSpaces                EnumerateReferenceSpaces = nullptr;
    PFN_xrCreateReferenceSpace                    CreateReferenceSpace = nullptr;
    PFN_xrDestroySpace                            DestroySpace = nullptr;
    PFN_xrLocateSpace                             LocateSpace = nullptr;

    PFN_xrEnumerateSwapchainFormats               EnumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain                         CreateSwapchain = nullptr;
    PFN_xrDestroySwapchain                        DestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages                EnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage                   AcquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage                      WaitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage                   ReleaseSwapchainImage = nullptr;

    PFN_xrWaitFrame                               WaitFrame = nullptr;
    PFN_xrBeginFrame                              BeginFrame = nullptr;
    PFN_xrEndFrame                                EndFrame = nullptr;
    PFN_xrLocateViews                             LocateViews = nullptr;
};

struct XRInputFunctions {
    PFN_xrCreateActionSet                         CreateActionSet = nullptr;
    PFN_xrDestroyActionSet                        DestroyActionSet = nullptr;
    PFN_xrCreateAction                            CreateAction = nullptr;
    PFN_xrDestroyAction                           DestroyAction = nullptr;
    PFN_xrSuggestInteractionProfileBindings       SuggestInteractionProfileBindings = nullptr;
    PFN_xrAttachSessionActionSets                 AttachSessionActionSets = nullptr;
    PFN_xrGetCurrentInteractionProfile            GetCurrentInteractionProfile = nullptr;
    PFN_xrSyncActions                             SyncActions = nullptr;
    PFN_xrGetActionStateBoolean                   GetActionStateBoolean = nullptr;
    PFN_xrGetActionStateFloat                     GetActionStateFloat = nullptr;
    PFN_xrGetActionStateVector2f                  GetActionStateVector2f = nullptr;
    PFN_xrGetActionStatePose                      GetActionStatePose = nullptr;
    PFN_xrCreateActionSpace                       CreateActionSpace = nullptr;
    PFN_xrApplyHapticFeedback                     ApplyHapticFeedback = nullptr;
    PFN_xrStopHapticFeedback                      StopHapticFeedback = nullptr;
};

struct XRExtensionFunctions {
#if defined(XR_USE_GRAPHICS_API_OPENGL_ES)
    PFN_xrGetOpenGLESGraphicsRequirementsKHR      GetOpenGLESGraphicsRequirementsKHR = nullptr;
#endif
#if defined(XR_USE_GRAPHICS_API_OPENGL)
    PFN_xrGetOpenGLGraphicsRequirementsKHR        GetOpenGLGraphicsRequirementsKHR = nullptr;
#endif
};

bool XRLoaderInit(std::string& outError);

bool XRLoaderAvailable();

const std::string& XRLoaderPath();

const XRGlobalFunctions& XRLoaderGlobals();

bool XRLoaderResolveInstanceFunctions(XrInstance instance,
                                      const std::vector<std::string>& enabledExtensions,
                                      std::string& outError);

const XRInstanceFunctions& XRLoaderInstanceFunctions();
const XRSessionFunctions& XRLoaderSessionFunctions();
const XRInputFunctions& XRLoaderInputFunctions();
const XRExtensionFunctions& XRLoaderExtensionFunctions();

void XRLoaderReleaseInstanceFunctions();

void XRLoaderShutdown();

bool XRLoaderResultToString(int32_t result, std::string& out);

}

#else

namespace Modularity::XR {
inline bool XRLoaderResultToString(int32_t, std::string&) { return false; }
}

#endif // MODULARITY_HAS_OPENXR
