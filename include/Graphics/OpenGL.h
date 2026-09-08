#pragma once

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif

#if defined(__ANDROID__) || (defined(MODULARITY_OPENGL_ES) && MODULARITY_OPENGL_ES)
#ifndef MODULARITY_OPENGL_ES
#define MODULARITY_OPENGL_ES 1
#endif
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#else
#ifndef MODULARITY_OPENGL_ES
#define MODULARITY_OPENGL_ES 0
#endif
#include <glad/glad.h>
#endif

namespace Modularity {

inline const char* OpenGLApiName()
{
#if MODULARITY_OPENGL_ES
    return "OpenGL ES";
#else
    return "OpenGL";
#endif
}

inline const char* OpenGLImGuiGlslVersion()
{
#if MODULARITY_OPENGL_ES
    return "#version 300 es";
#else
    return "#version 330";
#endif
}

} // namespace Modularity
