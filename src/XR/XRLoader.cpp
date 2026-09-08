#include "XRLoader.h"

#if MODULARITY_HAS_OPENXR

#include "XRDiagnostics.h"
#include "XRRuntimeSelection.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace Modularity::XR {

namespace {

#if defined(_WIN32)
using LibraryHandle = HMODULE;
constexpr LibraryHandle kNoLibrary = nullptr;

LibraryHandle openLibrary(const char* name) { return ::LoadLibraryA(name); }
void* librarySymbol(LibraryHandle lib, const char* symbol) {
    return reinterpret_cast<void*>(::GetProcAddress(lib, symbol));
}
#else
using LibraryHandle = void*;
constexpr LibraryHandle kNoLibrary = nullptr;

LibraryHandle openLibrary(const char* name) {
    return ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
}
void* librarySymbol(LibraryHandle lib, const char* symbol) { return ::dlsym(lib, symbol); }
#endif

// Windows is the odd one out and worth spelling out, because "no OpenXR loader
const char* const kLoaderNames[] = {
#if defined(__ANDROID__)
    "libopenxr_loader.so",
#elif defined(_WIN32)
    "openxr_loader.dll",
    "redist/windows-x64/openxr_loader.dll",
    "redist/windows-x86/openxr_loader.dll",
#elif defined(__APPLE__)
    "libopenxr_loader.dylib",
    "libopenxr_loader.1.dylib",
#else
    "libopenxr_loader.so.1",
    "libopenxr_loader.so",
#endif
};

struct LoaderState {
    LibraryHandle library = kNoLibrary;
    std::string path;
    XRGlobalFunctions globals;
    XRInstanceFunctions instance;
    XRSessionFunctions session;
    XRInputFunctions input;
    XRExtensionFunctions extensions;
    XrInstance instanceHandle = XR_NULL_HANDLE;
    bool instanceResolved = false;
};

LoaderState& state() {
    static LoaderState s;
    return s;
}

template <typename Fn>
bool resolveGlobal(const char* name, Fn& out, bool required, std::string& outError) {
    PFN_xrVoidFunction fn = nullptr;
    const XrResult result = state().globals.GetInstanceProcAddr(XR_NULL_HANDLE, name, &fn);
    if (XR_FAILED(result) || fn == nullptr) {
        if (required) {
            outError = std::string("xrGetInstanceProcAddr could not resolve ") + name + ": " +
                       XRResultString(static_cast<int32_t>(result));
            return false;
        }
        out = nullptr;
        return true;
    }
    out = reinterpret_cast<Fn>(fn);
    return true;
}

template <typename Fn>
bool resolveInstance(XrInstance instance, const char* name, Fn& out, bool required,
                     std::string& outError) {
    PFN_xrVoidFunction fn = nullptr;
    const XrResult result = state().globals.GetInstanceProcAddr(instance, name, &fn);
    if (XR_FAILED(result) || fn == nullptr) {
        if (required) {
            outError = std::string("xrGetInstanceProcAddr could not resolve ") + name + ": " +
                       XRResultString(static_cast<int32_t>(result));
            return false;
        }
        out = nullptr;
        return true;
    }
    out = reinterpret_cast<Fn>(fn);
    return true;
}

} // namespace

bool XRLoaderInit(std::string& outError) {
    outError.clear();
    LoaderState& s = state();
    if (s.library != kNoLibrary && s.globals.GetInstanceProcAddr != nullptr) {
        return true;
    }

    for (const char* name : kLoaderNames) {
        s.library = openLibrary(name);
        if (s.library != kNoLibrary) {
            s.path = name;
            break;
        }
    }
    if (s.library == kNoLibrary) {
        outError = "No OpenXR loader found (tried ";
        for (size_t i = 0; i < sizeof(kLoaderNames) / sizeof(kLoaderNames[0]); ++i) {
            if (i) outError += ", ";
            outError += kLoaderNames[i];
        }
        outError += "). ";
#if defined(__ANDROID__)
        outError += "Package libopenxr_loader.so in the APK.";
#elif defined(_WIN32)
        outError += "Windows has no system-wide OpenXR loader: the DLL ships with the "
                    "application. Put openxr_loader.dll next to Modularity.exe (or in "
                    "redist/windows-x64/, which the build copies there) - see "
                    "tools/fetch-openxr-loader-windows.sh. Installing SteamVR or the Meta "
                    "app provides the runtime, not the loader.";
#else
        outError += "Install an OpenXR runtime; on Linux the loader normally comes with it "
                    "as libopenxr_loader.so.1.";
#endif
        return false;
    }

    auto getProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
        librarySymbol(s.library, "xrGetInstanceProcAddr"));
    if (!getProcAddr) {
        outError = "OpenXR loader at " + s.path + " does not export xrGetInstanceProcAddr.";
        s.library = kNoLibrary;
        s.path.clear();
        return false;
    }
    s.globals = XRGlobalFunctions{};
    s.globals.GetInstanceProcAddr = getProcAddr;

    if (!resolveGlobal("xrEnumerateInstanceExtensionProperties",
                       s.globals.EnumerateInstanceExtensionProperties, true, outError) ||
        !resolveGlobal("xrCreateInstance", s.globals.CreateInstance, true, outError)) {
        s.globals = XRGlobalFunctions{};
        s.library = kNoLibrary;
        s.path.clear();
        return false;
    }
    resolveGlobal("xrEnumerateApiLayerProperties", s.globals.EnumerateApiLayerProperties, false,
                  outError);
#ifdef XR_KHR_loader_init
    resolveGlobal("xrInitializeLoaderKHR", s.globals.InitializeLoaderKHR, false, outError);
#endif
    outError.clear();

    NoteRuntimeLoaded();

    const std::string& overridePath = ActiveRuntimeOverride();
    XRLogInfo("Loader initialized (" + s.path + ")" +
              (overridePath.empty() ? std::string() : ", XR_RUNTIME_JSON=" + overridePath));
    return true;
}

bool XRLoaderAvailable() {
    return state().library != kNoLibrary && state().globals.GetInstanceProcAddr != nullptr;
}

const std::string& XRLoaderPath() { return state().path; }

const XRGlobalFunctions& XRLoaderGlobals() { return state().globals; }

bool XRLoaderResolveInstanceFunctions(XrInstance instance,
                                      const std::vector<std::string>& enabledExtensions,
                                      std::string& outError) {
    outError.clear();
    LoaderState& s = state();
    if (!XRLoaderAvailable()) {
        outError = "OpenXR loader is not initialized.";
        return false;
    }
    if (instance == XR_NULL_HANDLE) {
        outError = "Cannot resolve instance functions from a null XrInstance.";
        return false;
    }

    XRInstanceFunctions fns{};
    const bool ok =
        resolveInstance(instance, "xrDestroyInstance", fns.DestroyInstance, true, outError) &&
        resolveInstance(instance, "xrGetInstanceProperties", fns.GetInstanceProperties, true, outError) &&
        resolveInstance(instance, "xrGetSystem", fns.GetSystem, true, outError) &&
        resolveInstance(instance, "xrGetSystemProperties", fns.GetSystemProperties, true, outError) &&
        resolveInstance(instance, "xrEnumerateViewConfigurations", fns.EnumerateViewConfigurations, true, outError) &&
        resolveInstance(instance, "xrGetViewConfigurationProperties", fns.GetViewConfigurationProperties, true, outError) &&
        resolveInstance(instance, "xrEnumerateViewConfigurationViews", fns.EnumerateViewConfigurationViews, true, outError) &&
        resolveInstance(instance, "xrEnumerateEnvironmentBlendModes", fns.EnumerateEnvironmentBlendModes, true, outError) &&
        resolveInstance(instance, "xrStringToPath", fns.StringToPath, true, outError) &&
        resolveInstance(instance, "xrPathToString", fns.PathToString, true, outError) &&
        resolveInstance(instance, "xrPollEvent", fns.PollEvent, true, outError);
    if (!ok) return false;

    resolveInstance(instance, "xrResultToString", fns.ResultToString, false, outError);
    resolveInstance(instance, "xrStructureTypeToString", fns.StructureTypeToString, false, outError);
    outError.clear();

    XRSessionFunctions session{};
    const bool sessionOk =
        resolveInstance(instance, "xrCreateSession", session.CreateSession, true, outError) &&
        resolveInstance(instance, "xrDestroySession", session.DestroySession, true, outError) &&
        resolveInstance(instance, "xrBeginSession", session.BeginSession, true, outError) &&
        resolveInstance(instance, "xrEndSession", session.EndSession, true, outError) &&
        resolveInstance(instance, "xrRequestExitSession", session.RequestExitSession, true, outError) &&
        resolveInstance(instance, "xrEnumerateReferenceSpaces", session.EnumerateReferenceSpaces, true, outError) &&
        resolveInstance(instance, "xrCreateReferenceSpace", session.CreateReferenceSpace, true, outError) &&
        resolveInstance(instance, "xrDestroySpace", session.DestroySpace, true, outError) &&
        resolveInstance(instance, "xrLocateSpace", session.LocateSpace, true, outError) &&
        resolveInstance(instance, "xrEnumerateSwapchainFormats", session.EnumerateSwapchainFormats, true, outError) &&
        resolveInstance(instance, "xrCreateSwapchain", session.CreateSwapchain, true, outError) &&
        resolveInstance(instance, "xrDestroySwapchain", session.DestroySwapchain, true, outError) &&
        resolveInstance(instance, "xrEnumerateSwapchainImages", session.EnumerateSwapchainImages, true, outError) &&
        resolveInstance(instance, "xrAcquireSwapchainImage", session.AcquireSwapchainImage, true, outError) &&
        resolveInstance(instance, "xrWaitSwapchainImage", session.WaitSwapchainImage, true, outError) &&
        resolveInstance(instance, "xrReleaseSwapchainImage", session.ReleaseSwapchainImage, true, outError) &&
        resolveInstance(instance, "xrWaitFrame", session.WaitFrame, true, outError) &&
        resolveInstance(instance, "xrBeginFrame", session.BeginFrame, true, outError) &&
        resolveInstance(instance, "xrEndFrame", session.EndFrame, true, outError) &&
        resolveInstance(instance, "xrLocateViews", session.LocateViews, true, outError);
    if (!sessionOk) return false;

    XRInputFunctions input{};
    const bool inputOk =
        resolveInstance(instance, "xrCreateActionSet", input.CreateActionSet, true, outError) &&
        resolveInstance(instance, "xrDestroyActionSet", input.DestroyActionSet, true, outError) &&
        resolveInstance(instance, "xrCreateAction", input.CreateAction, true, outError) &&
        resolveInstance(instance, "xrDestroyAction", input.DestroyAction, true, outError) &&
        resolveInstance(instance, "xrSuggestInteractionProfileBindings", input.SuggestInteractionProfileBindings, true, outError) &&
        resolveInstance(instance, "xrAttachSessionActionSets", input.AttachSessionActionSets, true, outError) &&
        resolveInstance(instance, "xrGetCurrentInteractionProfile", input.GetCurrentInteractionProfile, true, outError) &&
        resolveInstance(instance, "xrSyncActions", input.SyncActions, true, outError) &&
        resolveInstance(instance, "xrGetActionStateBoolean", input.GetActionStateBoolean, true, outError) &&
        resolveInstance(instance, "xrGetActionStateFloat", input.GetActionStateFloat, true, outError) &&
        resolveInstance(instance, "xrGetActionStateVector2f", input.GetActionStateVector2f, true, outError) &&
        resolveInstance(instance, "xrGetActionStatePose", input.GetActionStatePose, true, outError) &&
        resolveInstance(instance, "xrCreateActionSpace", input.CreateActionSpace, true, outError) &&
        resolveInstance(instance, "xrApplyHapticFeedback", input.ApplyHapticFeedback, true, outError) &&
        resolveInstance(instance, "xrStopHapticFeedback", input.StopHapticFeedback, true, outError);
    if (!inputOk) return false;

    XRExtensionFunctions extensions{};
    const auto extensionEnabled = [&enabledExtensions](const char* name) {
        return std::find(enabledExtensions.begin(), enabledExtensions.end(), std::string(name)) !=
               enabledExtensions.end();
    };
    (void)extensionEnabled;
#if defined(XR_USE_GRAPHICS_API_OPENGL_ES)
    if (extensionEnabled(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
        resolveInstance(instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                        extensions.GetOpenGLESGraphicsRequirementsKHR, false, outError);
        outError.clear();
    }
#endif
#if defined(XR_USE_GRAPHICS_API_OPENGL)
    if (extensionEnabled(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME)) {
        resolveInstance(instance, "xrGetOpenGLGraphicsRequirementsKHR",
                        extensions.GetOpenGLGraphicsRequirementsKHR, false, outError);
        outError.clear();
    }
#endif

    s.instance = fns;
    s.session = session;
    s.input = input;
    s.extensions = extensions;
    s.instanceHandle = instance;
    s.instanceResolved = true;
    return true;
}

const XRInstanceFunctions& XRLoaderInstanceFunctions() { return state().instance; }
const XRSessionFunctions& XRLoaderSessionFunctions() { return state().session; }
const XRInputFunctions& XRLoaderInputFunctions() { return state().input; }
const XRExtensionFunctions& XRLoaderExtensionFunctions() { return state().extensions; }

void XRLoaderReleaseInstanceFunctions() {
    LoaderState& s = state();
    s.instance = XRInstanceFunctions{};
    s.session = XRSessionFunctions{};
    s.input = XRInputFunctions{};
    s.extensions = XRExtensionFunctions{};
    s.instanceHandle = XR_NULL_HANDLE;
    s.instanceResolved = false;
}

void XRLoaderShutdown() {
    LoaderState& s = state();
    XRLoaderReleaseInstanceFunctions();
    s.globals = XRGlobalFunctions{};
    s.library = kNoLibrary;
    s.path.clear();
}

bool XRLoaderResultToString(int32_t result, std::string& out) {
    const LoaderState& s = state();
    if (!s.instanceResolved || s.instance.ResultToString == nullptr ||
        s.instanceHandle == XR_NULL_HANDLE) {
        return false;
    }
    char buffer[XR_MAX_RESULT_STRING_SIZE] = {};
    if (XR_FAILED(s.instance.ResultToString(s.instanceHandle, static_cast<XrResult>(result),
                                            buffer))) {
        return false;
    }
    if (buffer[0] == '\0') return false;
    out.assign(buffer);
    return true;
}

}

#endif // MODULARITY_HAS_OPENXR
