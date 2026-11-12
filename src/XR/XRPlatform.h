#pragma once

#if MODULARITY_HAS_OPENXR

#if MODULARITY_OPENGL_ES
    #define XR_USE_GRAPHICS_API_OPENGL_ES
    #include <EGL/egl.h>
#endif

#if !MODULARITY_OPENGL_ES && !defined(__ANDROID__)
    #define XR_USE_GRAPHICS_API_OPENGL
#endif

#if defined(__ANDROID__)
    #define XR_USE_PLATFORM_ANDROID
    #include <jni.h>
#endif

#if defined(MODULARITY_XR_PLATFORM_BINDING_TU) && defined(XR_USE_GRAPHICS_API_OPENGL) && \
    defined(_WIN32)
    #define XR_USE_PLATFORM_WIN32
#endif

// The Xlib binding struct names Display, GLXFBConfig, GLXDrawable and GLXContext,
// so the binding TU must have <X11/Xlib.h> and <GL/glx.h> included before this
// header (XRDesktopGL.cpp gets both from glfw3native.h). Every other TU stays free
// of X11's macro namespace (None, Bool, Status, Success...).
#if defined(MODULARITY_XR_PLATFORM_BINDING_TU) && defined(XR_USE_GRAPHICS_API_OPENGL) && \
    defined(__linux__) && !defined(__ANDROID__)
    #define XR_USE_PLATFORM_XLIB
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#if defined(XR_USE_GRAPHICS_API_OPENGL_ES)
    #define MODULARITY_XR_HAS_GLES_TYPES 1
#else
    #define MODULARITY_XR_HAS_GLES_TYPES 0
#endif

#if defined(XR_USE_GRAPHICS_API_OPENGL)
    #define MODULARITY_XR_HAS_GL_TYPES 1
#else
    #define MODULARITY_XR_HAS_GL_TYPES 0
#endif

#if MODULARITY_XR_HAS_GLES_TYPES || MODULARITY_XR_HAS_GL_TYPES
    #define MODULARITY_XR_HAS_ANY_GL_TYPES 1
#else
    #define MODULARITY_XR_HAS_ANY_GL_TYPES 0
#endif

#if defined(XR_USE_GRAPHICS_API_OPENGL_ES) && defined(XR_USE_PLATFORM_ANDROID)
    #define MODULARITY_XR_HAS_GRAPHICS_BINDING 1
#else
    #define MODULARITY_XR_HAS_GRAPHICS_BINDING 0
#endif

#if defined(XR_USE_GRAPHICS_API_OPENGL) && \
    (defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__)))
    #define MODULARITY_XR_HAS_DESKTOP_GL_BINDING 1
#else
    #define MODULARITY_XR_HAS_DESKTOP_GL_BINDING 0
#endif

#if MODULARITY_XR_HAS_GRAPHICS_BINDING || MODULARITY_XR_HAS_DESKTOP_GL_BINDING
    #define MODULARITY_XR_HAS_ANY_GRAPHICS_BINDING 1
#else
    #define MODULARITY_XR_HAS_ANY_GRAPHICS_BINDING 0
#endif

#endif // MODULARITY_HAS_OPENXR
