#include "GLSyncProbe.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#ifdef APIENTRY
#undef APIENTRY
#endif
#define MODU_PROBE_CALLER() static_cast<void*>(_ReturnAddress())
#else
#define MODU_PROBE_CALLER() static_cast<void*>(__builtin_return_address(0))
#endif

#include "../include/Graphics/OpenGL.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Modularity::GLSyncProbe {
namespace {

using Clock = std::chrono::steady_clock;

struct State {
    std::mutex mutex;
    uint64_t frames = 0;
    uint64_t byCall[static_cast<int>(SyncCall::Count)] = {};
    uint64_t bufferDataBytes = 0;
    std::unordered_map<uint64_t, uint64_t> byState;
    std::unordered_map<void*, uint64_t> syncCallers;
    std::unordered_map<void*, uint64_t> allocationCallers;
    Clock::time_point windowStart = Clock::now();
};

bool IsSync(SyncCall call) {
    return static_cast<int>(call) < static_cast<int>(SyncCall::FirstNonSync);
}

bool IsAllocation(SyncCall call) {
    return call == SyncCall::TexImage || call == SyncCall::BufferData ||
           call == SyncCall::RenderbufferStorage || call == SyncCall::GenerateMipmap ||
           call == SyncCall::CreateOrDeleteObject || call == SyncCall::CompileOrLink;
}

State& GetState() {
    static State state;
    return state;
}

const char* CallName(SyncCall call) {
    switch (call) {
        case SyncCall::GetIntegerv: return "glGetIntegerv";
        case SyncCall::GetBooleanv: return "glGetBooleanv";
        case SyncCall::GetFloatv: return "glGetFloatv";
        case SyncCall::IsEnabled: return "glIsEnabled";
        case SyncCall::GetError: return "glGetError";
        case SyncCall::CheckFramebufferStatus: return "glCheckFramebufferStatus";
        case SyncCall::GetTexLevelParameteriv: return "glGetTexLevelParameteriv";
        case SyncCall::GetQueryObject: return "glGetQueryObject*";
        case SyncCall::GetUniformLocation: return "glGetUniformLocation";
        case SyncCall::GetProgramOrShaderiv: return "glGetProgram/Shaderiv";
        case SyncCall::MapBuffer: return "glMapBuffer*";
        case SyncCall::ReadPixels: return "glReadPixels";
        case SyncCall::GetTexImage: return "glGetTexImage";
        case SyncCall::Finish: return "glFinish";
        case SyncCall::TexImage: return "glTexImage*/glTexStorage*";
        case SyncCall::BufferData: return "glBufferData";
        case SyncCall::RenderbufferStorage: return "glRenderbufferStorage*";
        case SyncCall::GenerateMipmap: return "glGenerateMipmap";
        case SyncCall::CreateOrDeleteObject: return "glGen*/glDelete* (textures, buffers, FBOs, VAOs)";
        case SyncCall::CompileOrLink: return "glCompileShader/glLinkProgram";
        case SyncCall::Draw: return "draw calls";
        case SyncCall::Uniform: return "glUniform*";
        default: return "?";
    }
}

std::string StateName(unsigned pname) {
    switch (pname) {
        case GL_VIEWPORT: return "VIEWPORT";
        case GL_SCISSOR_BOX: return "SCISSOR_BOX";
        case GL_DRAW_FRAMEBUFFER_BINDING: return "(DRAW_)FRAMEBUFFER_BINDING";
        case GL_READ_FRAMEBUFFER_BINDING: return "READ_FRAMEBUFFER_BINDING";
        case GL_RENDERBUFFER_BINDING: return "RENDERBUFFER_BINDING";
        case GL_CURRENT_PROGRAM: return "CURRENT_PROGRAM";
        case GL_ACTIVE_TEXTURE: return "ACTIVE_TEXTURE";
        case GL_TEXTURE_BINDING_2D: return "TEXTURE_BINDING_2D";
        case GL_SAMPLER_BINDING: return "SAMPLER_BINDING";
        case GL_ARRAY_BUFFER_BINDING: return "ARRAY_BUFFER_BINDING";
        case GL_ELEMENT_ARRAY_BUFFER_BINDING: return "ELEMENT_ARRAY_BUFFER_BINDING";
        case GL_VERTEX_ARRAY_BINDING: return "VERTEX_ARRAY_BINDING";
#if !MODULARITY_OPENGL_ES
        case GL_POLYGON_MODE: return "POLYGON_MODE";
#endif
        case GL_BLEND_SRC_RGB: return "BLEND_SRC_RGB";
        case GL_BLEND_DST_RGB: return "BLEND_DST_RGB";
        case GL_BLEND_SRC_ALPHA: return "BLEND_SRC_ALPHA";
        case GL_BLEND_DST_ALPHA: return "BLEND_DST_ALPHA";
        case GL_BLEND_EQUATION_RGB: return "BLEND_EQUATION_RGB";
        case GL_BLEND_EQUATION_ALPHA: return "BLEND_EQUATION_ALPHA";
        case GL_DEPTH_FUNC: return "DEPTH_FUNC";
        case GL_DEPTH_WRITEMASK: return "DEPTH_WRITEMASK";
        case GL_COLOR_WRITEMASK: return "COLOR_WRITEMASK";
        case GL_COLOR_CLEAR_VALUE: return "COLOR_CLEAR_VALUE";
        case GL_CULL_FACE_MODE: return "CULL_FACE_MODE";
        case GL_FRONT_FACE: return "FRONT_FACE";
        case GL_LINE_WIDTH: return "LINE_WIDTH";
        case GL_POLYGON_OFFSET_FACTOR: return "POLYGON_OFFSET_FACTOR";
        case GL_POLYGON_OFFSET_UNITS: return "POLYGON_OFFSET_UNITS";
        case GL_UNPACK_ALIGNMENT: return "UNPACK_ALIGNMENT";
        case GL_BLEND: return "BLEND";
        case GL_DEPTH_TEST: return "DEPTH_TEST";
        case GL_CULL_FACE: return "CULL_FACE";
        case GL_STENCIL_TEST: return "STENCIL_TEST";
        case GL_SCISSOR_TEST: return "SCISSOR_TEST";
        case GL_POLYGON_OFFSET_FILL: return "POLYGON_OFFSET_FILL";
#if !MODULARITY_OPENGL_ES
        case GL_POLYGON_OFFSET_LINE: return "POLYGON_OFFSET_LINE";
        case GL_PRIMITIVE_RESTART: return "PRIMITIVE_RESTART";
        case GL_FRAMEBUFFER_SRGB: return "FRAMEBUFFER_SRGB";
        case GL_MULTISAMPLE: return "MULTISAMPLE";
#endif
        default: {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "0x%04X", pname);
            return buffer;
        }
    }
}

bool StateIsKeyed(SyncCall call) {
    return call == SyncCall::GetIntegerv || call == SyncCall::GetBooleanv ||
           call == SyncCall::GetFloatv || call == SyncCall::IsEnabled;
}

#ifdef _WIN32
std::string SymbolFor(void* address) {
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    } else {
        // Script DLLs load after startup; without a refresh their callers (and sometimes the
        // exe's, once the module list is stale) resolve to bare addresses.
        SymRefreshModuleList(GetCurrentProcess());
    }
    char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(address), &displacement, symbol)) {
        return symbol->Name;
    }
    char fallback[32];
    std::snprintf(fallback, sizeof(fallback), "%p", address);
    return fallback;
}
#else
std::string SymbolFor(void* address) {
    char fallback[32];
    std::snprintf(fallback, sizeof(fallback), "%p", address);
    return fallback;
}
#endif

template <typename Map, typename KeyToName>
std::string TopEntries(const Map& map, double frames, size_t limit, KeyToName&& name) {
    std::unordered_map<std::string, uint64_t> merged;
    for (const auto& [key, count] : map) merged[name(key)] += count;
    std::vector<std::pair<std::string, uint64_t>> sorted(merged.begin(), merged.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string out;
    for (size_t i = 0; i < sorted.size() && i < limit; ++i) {
        char line[640];
        std::snprintf(line, sizeof(line), "    %9.2f  %s\n", static_cast<double>(sorted[i].second) / frames,
                      sorted[i].first.c_str());
        out += line;
    }
    return out;
}

#if !MODULARITY_OPENGL_ES

#define MODU_PROBE_WRAP(name, call, ret, params, args, pname)                  \
    decltype(glad_##name) s_original_##name = nullptr;                        \
    ret APIENTRY Probe_##name params {                                         \
        Record(call, static_cast<unsigned>(pname), MODU_PROBE_CALLER());       \
        return s_original_##name args;                                         \
    }

MODU_PROBE_WRAP(glGetIntegerv, SyncCall::GetIntegerv, void, (GLenum p, GLint* d), (p, d), p)
MODU_PROBE_WRAP(glGetBooleanv, SyncCall::GetBooleanv, void, (GLenum p, GLboolean* d), (p, d), p)
MODU_PROBE_WRAP(glGetFloatv, SyncCall::GetFloatv, void, (GLenum p, GLfloat* d), (p, d), p)
MODU_PROBE_WRAP(glIsEnabled, SyncCall::IsEnabled, GLboolean, (GLenum c), (c), c)
MODU_PROBE_WRAP(glGetError, SyncCall::GetError, GLenum, (void), (), 0)
MODU_PROBE_WRAP(glCheckFramebufferStatus, SyncCall::CheckFramebufferStatus, GLenum, (GLenum t), (t), 0)
MODU_PROBE_WRAP(glGetTexLevelParameteriv, SyncCall::GetTexLevelParameteriv, void,
                (GLenum t, GLint l, GLenum p, GLint* d), (t, l, p, d), 0)
MODU_PROBE_WRAP(glGetQueryObjectiv, SyncCall::GetQueryObject, void, (GLuint q, GLenum p, GLint* d), (q, p, d), 0)
MODU_PROBE_WRAP(glGetQueryObjectuiv, SyncCall::GetQueryObject, void, (GLuint q, GLenum p, GLuint* d), (q, p, d), 0)
MODU_PROBE_WRAP(glGetQueryObjectui64v, SyncCall::GetQueryObject, void, (GLuint q, GLenum p, GLuint64* d), (q, p, d), 0)
MODU_PROBE_WRAP(glGetUniformLocation, SyncCall::GetUniformLocation, GLint, (GLuint pr, const GLchar* n), (pr, n), 0)
MODU_PROBE_WRAP(glGetProgramiv, SyncCall::GetProgramOrShaderiv, void, (GLuint pr, GLenum p, GLint* d), (pr, p, d), 0)
MODU_PROBE_WRAP(glGetShaderiv, SyncCall::GetProgramOrShaderiv, void, (GLuint s, GLenum p, GLint* d), (s, p, d), 0)
MODU_PROBE_WRAP(glMapBuffer, SyncCall::MapBuffer, void*, (GLenum t, GLenum a), (t, a), 0)
MODU_PROBE_WRAP(glMapBufferRange, SyncCall::MapBuffer, void*, (GLenum t, GLintptr o, GLsizeiptr l, GLbitfield a), (t, o, l, a), 0)
MODU_PROBE_WRAP(glReadPixels, SyncCall::ReadPixels, void,
                (GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, void* px), (x, y, w, h, f, ty, px), 0)
MODU_PROBE_WRAP(glGetTexImage, SyncCall::GetTexImage, void,
                (GLenum t, GLint l, GLenum f, GLenum ty, void* px), (t, l, f, ty, px), 0)
MODU_PROBE_WRAP(glFinish, SyncCall::Finish, void, (void), (), 0)

MODU_PROBE_WRAP(glTexImage2D, SyncCall::TexImage, void,
                (GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void* px),
                (t, l, i, w, h, b, f, ty, px), 0)
MODU_PROBE_WRAP(glTexImage3D, SyncCall::TexImage, void,
                (GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLsizei d, GLint b, GLenum f, GLenum ty, const void* px),
                (t, l, i, w, h, d, b, f, ty, px), 0)
MODU_PROBE_WRAP(glTexStorage2D, SyncCall::TexImage, void,
                (GLenum t, GLsizei lv, GLenum i, GLsizei w, GLsizei h), (t, lv, i, w, h), 0)
MODU_PROBE_WRAP(glBufferData, SyncCall::BufferData, void,
                (GLenum t, GLsizeiptr s, const void* d, GLenum u), (t, s, d, u),
                static_cast<unsigned>(std::min<GLsizeiptr>(s, 0xffffffff)))
MODU_PROBE_WRAP(glRenderbufferStorage, SyncCall::RenderbufferStorage, void,
                (GLenum t, GLenum i, GLsizei w, GLsizei h), (t, i, w, h), 0)
MODU_PROBE_WRAP(glRenderbufferStorageMultisample, SyncCall::RenderbufferStorage, void,
                (GLenum t, GLsizei s, GLenum i, GLsizei w, GLsizei h), (t, s, i, w, h), 0)
MODU_PROBE_WRAP(glGenerateMipmap, SyncCall::GenerateMipmap, void, (GLenum t), (t), 0)
MODU_PROBE_WRAP(glGenTextures, SyncCall::CreateOrDeleteObject, void, (GLsizei n, GLuint* o), (n, o), 0)
MODU_PROBE_WRAP(glDeleteTextures, SyncCall::CreateOrDeleteObject, void, (GLsizei n, const GLuint* o), (n, o), 0)
MODU_PROBE_WRAP(glGenBuffers, SyncCall::CreateOrDeleteObject, void, (GLsizei n, GLuint* o), (n, o), 0)
MODU_PROBE_WRAP(glDeleteBuffers, SyncCall::CreateOrDeleteObject, void, (GLsizei n, const GLuint* o), (n, o), 0)
MODU_PROBE_WRAP(glGenFramebuffers, SyncCall::CreateOrDeleteObject, void, (GLsizei n, GLuint* o), (n, o), 0)
MODU_PROBE_WRAP(glDeleteFramebuffers, SyncCall::CreateOrDeleteObject, void, (GLsizei n, const GLuint* o), (n, o), 0)
MODU_PROBE_WRAP(glGenRenderbuffers, SyncCall::CreateOrDeleteObject, void, (GLsizei n, GLuint* o), (n, o), 0)
MODU_PROBE_WRAP(glDeleteRenderbuffers, SyncCall::CreateOrDeleteObject, void, (GLsizei n, const GLuint* o), (n, o), 0)
MODU_PROBE_WRAP(glGenVertexArrays, SyncCall::CreateOrDeleteObject, void, (GLsizei n, GLuint* o), (n, o), 0)
MODU_PROBE_WRAP(glDeleteVertexArrays, SyncCall::CreateOrDeleteObject, void, (GLsizei n, const GLuint* o), (n, o), 0)
MODU_PROBE_WRAP(glCompileShader, SyncCall::CompileOrLink, void, (GLuint s), (s), 0)
MODU_PROBE_WRAP(glLinkProgram, SyncCall::CompileOrLink, void, (GLuint p), (p), 0)

MODU_PROBE_WRAP(glDrawArrays, SyncCall::Draw, void, (GLenum m, GLint f, GLsizei c), (m, f, c), 0)
MODU_PROBE_WRAP(glDrawElements, SyncCall::Draw, void, (GLenum m, GLsizei c, GLenum t, const void* i), (m, c, t, i), 0)
MODU_PROBE_WRAP(glDrawArraysInstanced, SyncCall::Draw, void, (GLenum m, GLint f, GLsizei c, GLsizei n), (m, f, c, n), 0)
MODU_PROBE_WRAP(glDrawElementsInstanced, SyncCall::Draw, void,
                (GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n), (m, c, t, i, n), 0)

MODU_PROBE_WRAP(glUniform1i, SyncCall::Uniform, void, (GLint l, GLint v), (l, v), 0)
MODU_PROBE_WRAP(glUniform1f, SyncCall::Uniform, void, (GLint l, GLfloat v), (l, v), 0)
MODU_PROBE_WRAP(glUniform2f, SyncCall::Uniform, void, (GLint l, GLfloat a, GLfloat b), (l, a, b), 0)
MODU_PROBE_WRAP(glUniform3f, SyncCall::Uniform, void, (GLint l, GLfloat a, GLfloat b, GLfloat c), (l, a, b, c), 0)
MODU_PROBE_WRAP(glUniform4f, SyncCall::Uniform, void, (GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d), (l, a, b, c, d), 0)
MODU_PROBE_WRAP(glUniform1iv, SyncCall::Uniform, void, (GLint l, GLsizei n, const GLint* v), (l, n, v), 0)
MODU_PROBE_WRAP(glUniform1fv, SyncCall::Uniform, void, (GLint l, GLsizei n, const GLfloat* v), (l, n, v), 0)
MODU_PROBE_WRAP(glUniform2fv, SyncCall::Uniform, void, (GLint l, GLsizei n, const GLfloat* v), (l, n, v), 0)
MODU_PROBE_WRAP(glUniform3fv, SyncCall::Uniform, void, (GLint l, GLsizei n, const GLfloat* v), (l, n, v), 0)
MODU_PROBE_WRAP(glUniform4fv, SyncCall::Uniform, void, (GLint l, GLsizei n, const GLfloat* v), (l, n, v), 0)
MODU_PROBE_WRAP(glUniformMatrix3fv, SyncCall::Uniform, void,
                (GLint l, GLsizei n, GLboolean t, const GLfloat* v), (l, n, t, v), 0)
MODU_PROBE_WRAP(glUniformMatrix4fv, SyncCall::Uniform, void,
                (GLint l, GLsizei n, GLboolean t, const GLfloat* v), (l, n, t, v), 0)

#undef MODU_PROBE_WRAP

void InstallGladWrappers() {
#define MODU_PROBE_INSTALL(name)                                                \
    if (glad_##name && glad_##name != Probe_##name) {                          \
        s_original_##name = glad_##name;                                       \
        glad_##name = Probe_##name;                                            \
    }
    MODU_PROBE_INSTALL(glGetIntegerv)
    MODU_PROBE_INSTALL(glGetBooleanv)
    MODU_PROBE_INSTALL(glGetFloatv)
    MODU_PROBE_INSTALL(glIsEnabled)
    MODU_PROBE_INSTALL(glGetError)
    MODU_PROBE_INSTALL(glCheckFramebufferStatus)
    MODU_PROBE_INSTALL(glGetTexLevelParameteriv)
    MODU_PROBE_INSTALL(glGetQueryObjectiv)
    MODU_PROBE_INSTALL(glGetQueryObjectuiv)
    MODU_PROBE_INSTALL(glGetQueryObjectui64v)
    MODU_PROBE_INSTALL(glGetUniformLocation)
    MODU_PROBE_INSTALL(glGetProgramiv)
    MODU_PROBE_INSTALL(glGetShaderiv)
    MODU_PROBE_INSTALL(glMapBuffer)
    MODU_PROBE_INSTALL(glMapBufferRange)
    MODU_PROBE_INSTALL(glReadPixels)
    MODU_PROBE_INSTALL(glGetTexImage)
    MODU_PROBE_INSTALL(glFinish)
    MODU_PROBE_INSTALL(glTexImage2D)
    MODU_PROBE_INSTALL(glTexImage3D)
    MODU_PROBE_INSTALL(glTexStorage2D)
    MODU_PROBE_INSTALL(glBufferData)
    MODU_PROBE_INSTALL(glRenderbufferStorage)
    MODU_PROBE_INSTALL(glRenderbufferStorageMultisample)
    MODU_PROBE_INSTALL(glGenerateMipmap)
    MODU_PROBE_INSTALL(glGenTextures)
    MODU_PROBE_INSTALL(glDeleteTextures)
    MODU_PROBE_INSTALL(glGenBuffers)
    MODU_PROBE_INSTALL(glDeleteBuffers)
    MODU_PROBE_INSTALL(glGenFramebuffers)
    MODU_PROBE_INSTALL(glDeleteFramebuffers)
    MODU_PROBE_INSTALL(glGenRenderbuffers)
    MODU_PROBE_INSTALL(glDeleteRenderbuffers)
    MODU_PROBE_INSTALL(glGenVertexArrays)
    MODU_PROBE_INSTALL(glDeleteVertexArrays)
    MODU_PROBE_INSTALL(glCompileShader)
    MODU_PROBE_INSTALL(glLinkProgram)
    MODU_PROBE_INSTALL(glDrawArrays)
    MODU_PROBE_INSTALL(glDrawElements)
    MODU_PROBE_INSTALL(glDrawArraysInstanced)
    MODU_PROBE_INSTALL(glDrawElementsInstanced)
    MODU_PROBE_INSTALL(glUniform1i)
    MODU_PROBE_INSTALL(glUniform1f)
    MODU_PROBE_INSTALL(glUniform2f)
    MODU_PROBE_INSTALL(glUniform3f)
    MODU_PROBE_INSTALL(glUniform4f)
    MODU_PROBE_INSTALL(glUniform1iv)
    MODU_PROBE_INSTALL(glUniform1fv)
    MODU_PROBE_INSTALL(glUniform2fv)
    MODU_PROBE_INSTALL(glUniform3fv)
    MODU_PROBE_INSTALL(glUniform4fv)
    MODU_PROBE_INSTALL(glUniformMatrix3fv)
    MODU_PROBE_INSTALL(glUniformMatrix4fv)
#undef MODU_PROBE_INSTALL
}

#endif

} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("MODULARITY_GL_SYNC_PROBE");
        return v && *v && *v != '0';
    }();
    return enabled;
}

void Record(SyncCall call, unsigned pname, void* caller) {
    State& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.byCall[static_cast<int>(call)];
    if (StateIsKeyed(call)) {
        ++state.byState[(static_cast<uint64_t>(call) << 32) | pname];
    }
    if (call == SyncCall::BufferData) {
        state.bufferDataBytes += pname;
    }
    if (IsSync(call)) {
        ++state.syncCallers[caller];
    } else if (IsAllocation(call)) {
        ++state.allocationCallers[caller];
    }
}

void InstallIfRequested() {
    if (!Enabled()) return;
#if !MODULARITY_OPENGL_ES
    InstallGladWrappers();
    InstallImGuiLoaderWrappers();
#endif
}

void OnFramePresented() {
    if (!Enabled()) return;
    // ImGui_ImplOpenGL3_Init refills imgl3w's table (a new UI canvas context does this mid-run).
    InstallIfRequested();

    State& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.frames;
    const Clock::time_point now = Clock::now();
    if (now - state.windowStart < std::chrono::seconds(5) || state.frames == 0) return;

    const double frames = static_cast<double>(state.frames);
    const double seconds = std::chrono::duration<double>(now - state.windowStart).count();
    uint64_t syncTotal = 0;
    std::unordered_map<int, uint64_t> syncCalls;
    std::unordered_map<int, uint64_t> otherCalls;
    for (int i = 0; i < static_cast<int>(SyncCall::Count); ++i) {
        if (!state.byCall[i]) continue;
        if (IsSync(static_cast<SyncCall>(i))) {
            syncTotal += state.byCall[i];
            syncCalls[i] = state.byCall[i];
        } else {
            otherCalls[i] = state.byCall[i];
        }
    }
    const auto callName = [](int i) { return std::string(CallName(static_cast<SyncCall>(i))); };

    std::string report;
    char header[256];
    std::snprintf(header, sizeof(header),
                  "[GLSyncProbe] %llu frames (%.0f fps), %.1f driver round-trips per frame, "
                  "%.1f KB glBufferData per frame\n",
                  static_cast<unsigned long long>(state.frames), frames / seconds,
                  static_cast<double>(syncTotal) / frames,
                  static_cast<double>(state.bufferDataBytes) / frames / 1024.0);
    report += header;

    report += "  round-trips by call:\n";
    report += TopEntries(syncCalls, frames, 20, callName);
    report += "  round-trips by state queried:\n";
    report += TopEntries(state.byState, frames, 20, [](uint64_t key) {
        return std::string(CallName(static_cast<SyncCall>(key >> 32))) + " " +
               StateName(static_cast<unsigned>(key & 0xffffffffu));
    });
    report += "  round-trips by calling function:\n";
    report += TopEntries(state.syncCallers, frames, 25, [](void* address) { return SymbolFor(address); });

    report += "  other driver work per frame:\n";
    report += TopEntries(otherCalls, frames, 20, callName);
    report += "  allocation/churn by calling function:\n";
    report += TopEntries(state.allocationCallers, frames, 20, [](void* address) { return SymbolFor(address); });

    std::fputs(report.c_str(), stderr);
    std::fflush(stderr);

    state.frames = 0;
    std::fill(std::begin(state.byCall), std::end(state.byCall), 0);
    state.bufferDataBytes = 0;
    state.byState.clear();
    state.syncCallers.clear();
    state.allocationCallers.clear();
    state.windowStart = Clock::now();
}

} // namespace Modularity::GLSyncProbe
