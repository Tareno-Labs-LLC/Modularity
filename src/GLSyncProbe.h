#pragma once

// MODULARITY_GL_SYNC_PROBE=1 counts, per presented frame, the GL calls that hand data back to the
// caller (glGet*, glIsEnabled, glGetError, glCheckFramebufferStatus, buffer maps, readbacks) and
// prints which engine functions make them every 5 seconds.
//
// Why those calls: NVIDIA's OpenGL driver ("Threaded optimization", on by default) queues
// commands to its own worker thread. A call that returns data cannot be queued, so the render
// thread stops and waits for that worker to drain everything queued so far. AMD's driver runs on
// the calling thread and answers from a cached value. The same frame can therefore cost several
// times more CPU on NVIDIA, and a GPU profiler shows nothing because the GPU is idle.
//
// Wraps glad's function pointers (engine code) and imgl3w's table (ImGui's GL backend). Inert
// unless the variable is set.

namespace Modularity::GLSyncProbe {

enum class SyncCall : int {
    // Round-trips: the caller waits for the driver.
    GetIntegerv,
    GetBooleanv,
    GetFloatv,
    IsEnabled,
    GetError,
    CheckFramebufferStatus,
    GetTexLevelParameteriv,
    GetQueryObject,
    GetUniformLocation,
    GetProgramOrShaderiv,
    MapBuffer,
    ReadPixels,
    GetTexImage,
    Finish,
    FirstNonSync,
    // Not round-trips, but storage (re)allocation and object churn are far costlier per call on
    // NVIDIA than on AMD, so a per-frame count of them is the other half of the picture.
    TexImage = FirstNonSync,
    BufferData,
    RenderbufferStorage,
    GenerateMipmap,
    CreateOrDeleteObject,
    CompileOrLink,
    Draw,
    Uniform,
    Count
};

bool Enabled();

// After gladLoadGL and ImGui_ImplOpenGL3_Init. Safe to repeat; it re-wraps a table that a loader
// refilled (every ImGui_ImplOpenGL3_Init reloads imgl3w).
void InstallIfRequested();

void OnFramePresented();

// Shared by the glad and imgl3w wrappers, which live in separate translation units because the
// two loaders' headers cannot be included together.
void Record(SyncCall call, unsigned pname, void* caller);
void InstallImGuiLoaderWrappers();

} // namespace Modularity::GLSyncProbe
