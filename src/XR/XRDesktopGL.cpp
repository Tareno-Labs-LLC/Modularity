
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

#elif MODULARITY_HAS_OPENXR && !defined(__ANDROID__) && !MODULARITY_OPENGL_ES && \
    defined(__linux__)

// Engine headers first: <X11/Xlib.h> defines None, Bool, Status, Success, True and
// False as macros, so nothing of ours may be parsed after it.
#include "XRDiagnostics.h"

// glfw3native.h pulls in <X11/Xlib.h> and <GL/glx.h> (and through it <GL/gl.h>),
// which is exactly what openxr_platform.h's Xlib binding struct needs declared.
// glad is deliberately not included in this TU: <GL/gl.h> is already here, and the
// only GL calls below are glGetString/glGetError for the diagnostics string, which
// resolve against the GLVND libOpenGL the executables link anyway.
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_GLX
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#define MODULARITY_XR_PLATFORM_BINDING_TU 1
#include "XRPlatform.h"

#define MODULARITY_XR_DESKTOP_GL_IMPLEMENTED 1

#else

#define MODULARITY_XR_DESKTOP_GL_IMPLEMENTED 0

#endif

namespace Modularity::XR {

#if MODULARITY_XR_DESKTOP_GL_IMPLEMENTED

#if defined(_WIN32)

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

#else // Linux: Xlib + GLX

namespace {

XrGraphicsBindingOpenGLXlibKHR& bindingStorage() {
    static XrGraphicsBindingOpenGLXlibKHR binding{};
    return binding;
}

} // namespace

bool XRDesktopGLBindingAvailable() { return true; }

const char* XRDesktopGLBindingName() { return "OpenGL (Xlib/GLX)"; }

const void* XRDesktopGLBuildSessionBinding(void* glfwWindow, std::string& outError) {
    outError.clear();
    auto* window = static_cast<GLFWwindow*>(glfwWindow);
    if (!window) {
        outError = "Cannot bind an OpenXR session without the engine's window. The renderer "
                   "must be up before XR starts.";
        return nullptr;
    }

    // OpenXR's only desktop-GL binding on Linux is Xlib/GLX, so a native Wayland
    // window has nothing to hand the runtime. Under XWayland GLFW is on X11 and
    // this check passes.
    if (::glfwGetPlatform() != GLFW_PLATFORM_X11) {
        outError = "PCVR on Linux binds OpenGL through Xlib/GLX "
                   "(XrGraphicsBindingOpenGLXlibKHR), but this window is not an X11 window. "
                   "Run Modularity under XWayland (for example with WAYLAND_DISPLAY unset) "
                   "so GLFW picks its X11 platform.";
        return nullptr;
    }

    Display* display = ::glfwGetX11Display();
    const GLXContext context = ::glfwGetGLXContext(window);
    GLXDrawable drawable = ::glfwGetGLXWindow(window);
    if (!display || !context) {
        outError = "GLFW reported no X11 display or GLX context for this window, so there is "
                   "nothing for the OpenXR runtime to bind to. An EGL, Vulkan or headless "
                   "context cannot use the GLX binding.";
        return nullptr;
    }
    if (drawable == 0 && ::glXGetCurrentContext() == context) {
        drawable = ::glXGetCurrentDrawable();
    }
    if (drawable == 0) {
        outError = "The engine's GLX context has no drawable, so the OpenXR runtime has "
                   "nothing to make current alongside it.";
        return nullptr;
    }

    // GLFW does not expose the FBConfig it chose, but the context remembers it:
    // read its ID back and look the config up by that ID alone.
    int fbConfigId = 0;
    if (::glXQueryContext(display, context, GLX_FBCONFIG_ID, &fbConfigId) != Success) {
        outError = "glXQueryContext could not report the GLX_FBCONFIG_ID of the engine's "
                   "context, which the OpenXR GLX binding requires.";
        return nullptr;
    }
    int screen = DefaultScreen(display);
    ::glXQueryContext(display, context, GLX_SCREEN, &screen);

    const int configAttribs[] = {GLX_FBCONFIG_ID, fbConfigId, None};
    int configCount = 0;
    GLXFBConfig* configs = ::glXChooseFBConfig(display, screen, configAttribs, &configCount);
    if (!configs || configCount < 1) {
        if (configs) ::XFree(configs);
        outError = "No GLX FBConfig matches the engine context's GLX_FBCONFIG_ID " +
                   std::to_string(fbConfigId) + ", so the OpenXR GLX binding cannot be filled.";
        return nullptr;
    }
    // The array is ours to free; the FBConfig handles in it belong to the display.
    const GLXFBConfig config = configs[0];
    ::XFree(configs);

    uint32_t visualId = 0;
    if (XVisualInfo* visual = ::glXGetVisualFromFBConfig(display, config)) {
        visualId = static_cast<uint32_t>(visual->visualid);
        ::XFree(visual);
    }

    XrGraphicsBindingOpenGLXlibKHR& binding = bindingStorage();
    binding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR;
    binding.next = nullptr;
    binding.xDisplay = display;
    binding.visualid = visualId;
    binding.glxFBConfig = config;
    binding.glxDrawable = drawable;
    binding.glxContext = context;

    XRLogInfo("GLX graphics binding initialized");
    return &binding;
}

#endif // _WIN32

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
    outError = "PCVR is implemented for Windows (XrGraphicsBindingOpenGLWin32KHR) and Linux "
               "(XrGraphicsBindingOpenGLXlibKHR). This platform has no OpenXR desktop-OpenGL "
               "binding in Modularity.";
#endif
    return nullptr;
}

std::string XRDesktopGLContextDescription() { return {}; }

#endif // MODULARITY_XR_DESKTOP_GL_IMPLEMENTED

}
