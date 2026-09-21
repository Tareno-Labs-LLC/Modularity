// ImGui's OpenGL backend loads GL through its own imgl3w table rather than glad, so its state
// queries are wrapped here. This lives apart from GLSyncProbe.cpp because imgl3w's header and
// glad's cannot share a translation unit.

#include "GLSyncProbe.h"

#if defined(__ANDROID__) || (defined(MODULARITY_OPENGL_ES) && MODULARITY_OPENGL_ES)

namespace Modularity::GLSyncProbe {
void InstallImGuiLoaderWrappers() {}
} // namespace Modularity::GLSyncProbe

#else

#ifdef _WIN32
#include <intrin.h>
#define MODU_PROBE_CALLER() static_cast<void*>(_ReturnAddress())
#else
#define MODU_PROBE_CALLER() static_cast<void*>(__builtin_return_address(0))
#endif

#include "ThirdParty/ModuGUI/backends/imgui_impl_opengl3_loader.h"

namespace Modularity::GLSyncProbe {
namespace {

PFNGLGETINTEGERVPROC s_originalGetIntegerv = nullptr;
PFNGLISENABLEDPROC s_originalIsEnabled = nullptr;
PFNGLGETERRORPROC s_originalGetError = nullptr;
PFNGLGETPROGRAMIVPROC s_originalGetProgramiv = nullptr;
PFNGLGETSHADERIVPROC s_originalGetShaderiv = nullptr;
PFNGLGETUNIFORMLOCATIONPROC s_originalGetUniformLocation = nullptr;
PFNGLREADPIXELSPROC s_originalReadPixels = nullptr;
PFNGLBUFFERDATAPROC s_originalBufferData = nullptr;
PFNGLTEXIMAGE2DPROC s_originalTexImage2D = nullptr;
PFNGLGENVERTEXARRAYSPROC s_originalGenVertexArrays = nullptr;
PFNGLDELETEVERTEXARRAYSPROC s_originalDeleteVertexArrays = nullptr;
PFNGLDRAWELEMENTSPROC s_originalDrawElements = nullptr;
PFNGLDRAWELEMENTSBASEVERTEXPROC s_originalDrawElementsBaseVertex = nullptr;

void APIENTRY ProbeGetIntegerv(GLenum pname, GLint* data) {
    Record(SyncCall::GetIntegerv, pname, MODU_PROBE_CALLER());
    s_originalGetIntegerv(pname, data);
}
GLboolean APIENTRY ProbeIsEnabled(GLenum cap) {
    Record(SyncCall::IsEnabled, cap, MODU_PROBE_CALLER());
    return s_originalIsEnabled(cap);
}
GLenum APIENTRY ProbeGetError(void) {
    Record(SyncCall::GetError, 0, MODU_PROBE_CALLER());
    return s_originalGetError();
}
void APIENTRY ProbeGetProgramiv(GLuint program, GLenum pname, GLint* params) {
    Record(SyncCall::GetProgramOrShaderiv, 0, MODU_PROBE_CALLER());
    s_originalGetProgramiv(program, pname, params);
}
void APIENTRY ProbeGetShaderiv(GLuint shader, GLenum pname, GLint* params) {
    Record(SyncCall::GetProgramOrShaderiv, 0, MODU_PROBE_CALLER());
    s_originalGetShaderiv(shader, pname, params);
}
GLint APIENTRY ProbeGetUniformLocation(GLuint program, const GLchar* name) {
    Record(SyncCall::GetUniformLocation, 0, MODU_PROBE_CALLER());
    return s_originalGetUniformLocation(program, name);
}
void APIENTRY ProbeReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void* px) {
    Record(SyncCall::ReadPixels, 0, MODU_PROBE_CALLER());
    s_originalReadPixels(x, y, w, h, f, t, px);
}

void APIENTRY ProbeBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    Record(SyncCall::BufferData, static_cast<unsigned>(size), MODU_PROBE_CALLER());
    s_originalBufferData(target, size, data, usage);
}
void APIENTRY ProbeTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty,
                              const void* px) {
    Record(SyncCall::TexImage, 0, MODU_PROBE_CALLER());
    s_originalTexImage2D(t, l, i, w, h, b, f, ty, px);
}
void APIENTRY ProbeGenVertexArrays(GLsizei n, GLuint* arrays) {
    Record(SyncCall::CreateOrDeleteObject, 0, MODU_PROBE_CALLER());
    s_originalGenVertexArrays(n, arrays);
}
void APIENTRY ProbeDeleteVertexArrays(GLsizei n, const GLuint* arrays) {
    Record(SyncCall::CreateOrDeleteObject, 0, MODU_PROBE_CALLER());
    s_originalDeleteVertexArrays(n, arrays);
}
void APIENTRY ProbeDrawElements(GLenum m, GLsizei c, GLenum t, const void* i) {
    Record(SyncCall::Draw, 0, MODU_PROBE_CALLER());
    s_originalDrawElements(m, c, t, i);
}
void APIENTRY ProbeDrawElementsBaseVertex(GLenum m, GLsizei c, GLenum t, const void* i, GLint base) {
    Record(SyncCall::Draw, 0, MODU_PROBE_CALLER());
    s_originalDrawElementsBaseVertex(m, c, t, i, base);
}

template <typename Proc>
void Wrap(Proc& slot, Proc& original, Proc probe) {
    if (slot && slot != probe) {
        original = slot;
        slot = probe;
    }
}

} // namespace

void InstallImGuiLoaderWrappers() {
    Wrap(imgl3wProcs.gl.GetIntegerv, s_originalGetIntegerv, &ProbeGetIntegerv);
    Wrap(imgl3wProcs.gl.IsEnabled, s_originalIsEnabled, &ProbeIsEnabled);
    Wrap(imgl3wProcs.gl.GetError, s_originalGetError, &ProbeGetError);
    Wrap(imgl3wProcs.gl.GetProgramiv, s_originalGetProgramiv, &ProbeGetProgramiv);
    Wrap(imgl3wProcs.gl.GetShaderiv, s_originalGetShaderiv, &ProbeGetShaderiv);
    Wrap(imgl3wProcs.gl.GetUniformLocation, s_originalGetUniformLocation, &ProbeGetUniformLocation);
    Wrap(imgl3wProcs.gl.ReadPixels, s_originalReadPixels, &ProbeReadPixels);
    Wrap(imgl3wProcs.gl.BufferData, s_originalBufferData, &ProbeBufferData);
    Wrap(imgl3wProcs.gl.TexImage2D, s_originalTexImage2D, &ProbeTexImage2D);
    Wrap(imgl3wProcs.gl.GenVertexArrays, s_originalGenVertexArrays, &ProbeGenVertexArrays);
    Wrap(imgl3wProcs.gl.DeleteVertexArrays, s_originalDeleteVertexArrays, &ProbeDeleteVertexArrays);
    Wrap(imgl3wProcs.gl.DrawElements, s_originalDrawElements, &ProbeDrawElements);
    Wrap(imgl3wProcs.gl.DrawElementsBaseVertex, s_originalDrawElementsBaseVertex, &ProbeDrawElementsBaseVertex);
}

} // namespace Modularity::GLSyncProbe

#endif
