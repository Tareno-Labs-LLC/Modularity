#pragma once

// timing + GL-context queries the engine used to grab from GLFW, so the same source runs
// on desktop (GLFW) and Android (clock_gettime / EGL).

namespace Modularity::Platform {

// monotonic seconds since somewhere. good for shader uniforms + frame deltas.
// GLFW on desktop, CLOCK_MONOTONIC on Android.
double GetTimeSeconds();

// true iff a usable GL/GLES context is current on this thread. Android just assumes true
// once the runtime's EGL setup has succeeded.
bool HasCurrentGLContext();

} // namespace Modularity::Platform
