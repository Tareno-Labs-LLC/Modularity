
#include "XRDesktopGL.h"

#if MODULARITY_HAS_OPENXR && !defined(__ANDROID__) && !MODULARITY_OPENGL_ES && defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#include <unknwn.h>

#define MODULARITY_XR_PLATFORM_BINDING_TU 1
#include "XRPlatform.h"

#include "XRDiagnostics.h"

#include "../../include/Graphics/OpenGL.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WGL
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#define MODULARITY_XR_DESKTOP_GL_IMPLEMENTED 1

#else

#define MODULARITY_XR_DESKTOP_GL_IMPLEMENTED 0

#endif

namespace Modularity::XR {

#if MODULARITY_XR_DESKTOP_GL_IMPLEMENTED

namespace {

XrGraphicsBindingOpenGLWin32KHR& bindingStorage() {
    static XrGraphicsBindingOpenGLWin32KHR binding{};
    return binding;
}

} // namespace

bool XRDesktopGLBindingAvailable() { return true; }

const char* XRDesktopGLBindingName() { return "OpenGL (Win32)"; }

const void* XRDesktopGLBuildSessionBinding(void* glfwWindow, std::string& outError) {
    outError.clear();
    auto* window = static_cast<GLFWwindow*>(glfwWindow);
    if (!window) {
        outError = "Cannot bind an OpenXR session without the engine's window. The renderer "
                   "must be up before XR starts.";
        return nullptr;
    }

    const HWND hwnd = ::glfwGetWin32Window(window);
    const HGLRC context = ::glfwGetWGLContext(window);
    if (!hwnd || !context) {
        outError = "GLFW reported no native Win32 window or WGL context for this window, so "
                   "there is nothing for the OpenXR runtime to bind to. A Vulkan or headless "
                   "session cannot use the OpenGL binding.";
        return nullptr;
    }

    const HDC deviceContext = ::GetDC(hwnd);
    if (!deviceContext) {
        outError = "GetDC failed for the engine's window, so the OpenGL XR binding has no "
                   "device context.";
        return nullptr;
    }

    XrGraphicsBindingOpenGLWin32KHR& binding = bindingStorage();
    binding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR;
    binding.next = nullptr;
    binding.hDC = deviceContext;
    binding.hGLRC = context;

    XRLogInfo("WGL graphics binding initialized");
    return &binding;
}

std::string XRDesktopGLContextDescription() {
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (glGetError() != GL_NO_ERROR || !version) return {};
    std::string description(version);
    if (renderer && *renderer) {
        description += " - ";
        description += renderer;
    }
    return description;
}

#else

bool XRDesktopGLBindingAvailable() { return false; }

const char* XRDesktopGLBindingName() { return "none"; }

const void* XRDesktopGLBuildSessionBinding(void*, std::string& outError) {
#if !MODULARITY_HAS_OPENXR
    outError = "This build of Modularity was compiled without OpenXR support "
               "(MODULARITY_ENABLE_OPENXR=OFF).";
#elif defined(__ANDROID__)
    outError = "The desktop OpenGL binding does not apply on Android; Quest sessions use the "
               "OpenGL ES binding instead.";
#elif MODULARITY_OPENGL_ES
    outError = "This is a desktop OpenGL ES build. OpenXR defines no OpenGL ES graphics "
               "binding outside Android, so a desktop GLES build cannot create an XR session. "
               "Rebuild with MODULARITY_USE_OPENGL_ES=OFF for PCVR.";
#else
    outError = "PCVR is implemented for Windows (XrGraphicsBindingOpenGLWin32KHR). The Linux "
               "binding is XrGraphicsBindingOpenGLXlibKHR, which needs the GLX drawable and "
               "FBConfig that GLFW does not expose and that do not exist under Wayland, so it "
               "is not implemented rather than shipped half-working.";
#endif
    return nullptr;
}

std::string XRDesktopGLContextDescription() { return {}; }

#endif // MODULARITY_XR_DESKTOP_GL_IMPLEMENTED

}
