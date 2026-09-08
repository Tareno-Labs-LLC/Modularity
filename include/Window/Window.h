#ifndef WINDOW_H
#define WINDOW_H
#include "../General/GlobalHeaders.h"
#include "../Graphics/GraphicsBackend.h"
#include "../Graphics/OpenGL.h"

// GLFW builds on Android with its null backend (see CMakeLists.txt), so the
// full header is available everywhere. The Android runtime still drives
// real windowing through ANativeWindow separately - these glfw* calls just
// no-op at runtime when the null backend is active.
#include "../../src/ThirdParty/glfw/include/GLFW/glfw3.h"

class Window {
public:
    GLFWwindow* makeWindow(Modularity::GraphicsBackend backend);
};

#endif
