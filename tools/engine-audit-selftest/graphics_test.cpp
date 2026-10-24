#include "Engine.h"
#include <cstdio>
#include <stdexcept>
static int failures = 0;
static int cleanups = 0;
struct CleanupProbe {
    GLuint buffer = 0;
    ~CleanupProbe() {
        if (!buffer) return;
        const bool liveContext = glfwGetCurrentContext() && glGetString(GL_VERSION);
        const bool meshReleased = liveContext && !glIsBuffer(buffer);
        const bool noError = liveContext && glGetError() == GL_NO_ERROR;
        if (!liveContext || !meshReleased || !noError) ++failures;
        ++cleanups;
        std::printf("GL member cleanup: context=%d released=%d errors=%d\n", liveContext, meshReleased, !noError);
    }
};
struct Fixture {
    Window window; // Same ownership order as Engine: context owner dies last.
    CleanupProbe probe;
    std::unique_ptr<Mesh> mesh;
    Fixture() {
        GLFWwindow* handle = window.makeWindow(Modularity::GraphicsBackend::OpenGL);
        if (!handle) throw std::runtime_error("GL context creation failed");
        glfwHideWindow(handle);
        const float vertices[] = {0,0,0, 0,0,1, 0,0, 1,0,0, 0,0,1, 1,0, 0,1,0, 0,0,1, 0,1};
        mesh = std::make_unique<Mesh>(vertices, sizeof(vertices));
        probe.buffer = mesh->getVertexBuffer();
        if (!glIsBuffer(probe.buffer)) throw std::runtime_error("GPU mesh not allocated");
    }
};
int main() {
    try {
        { Fixture normal; }
        try { Fixture unwinding; throw std::runtime_error("expected unwind"); }
        catch (const std::runtime_error& error) {
            if (std::string(error.what()) != "expected unwind") throw;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::printf("GL lifetime: %d cleanup paths, %d failures\n", cleanups, failures);
    return failures || cleanups != 2 ? 1 : 0;
}
