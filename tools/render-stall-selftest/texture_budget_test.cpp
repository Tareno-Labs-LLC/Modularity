#include "Engine.h"

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
enum class MemoryExtension { None, Ati, Nvidia };

PFNGLGETSTRINGIPROC originalGetStringi = nullptr;
PFNGLGETINTEGERVPROC originalGetIntegerv = nullptr;
MemoryExtension extension = MemoryExtension::None;
GLint availableKiB = 0;
int memoryQueries = 0;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

const GLubyte* APIENTRY GetStringi(GLenum name, GLuint index) {
    const GLubyte* actual = originalGetStringi(name, index);
    if (name != GL_EXTENSIONS) return actual;
    if (index == 0 && extension == MemoryExtension::Ati)
        return reinterpret_cast<const GLubyte*>("GL_ATI_meminfo");
    if (index == 0 && extension == MemoryExtension::Nvidia)
        return reinterpret_cast<const GLubyte*>("GL_NVX_gpu_memory_info");
    if (actual && (std::strcmp(reinterpret_cast<const char*>(actual), "GL_ATI_meminfo") == 0 ||
                   std::strcmp(reinterpret_cast<const char*>(actual), "GL_NVX_gpu_memory_info") == 0))
        return reinterpret_cast<const GLubyte*>("GL_MODULARITY_test_hidden");
    return actual;
}

void APIENTRY GetIntegerv(GLenum name, GLint* values) {
    if (name == 0x87FC) {
        Require(extension == MemoryExtension::Ati, "Queried unsupported ATI extension");
        ++memoryQueries;
        values[0] = availableKiB;
        values[1] = 0;
        values[2] = 16 * 1024 * 1024; // Auxiliary/system memory must be ignored.
        values[3] = 0;
    } else if (name == 0x9049) {
        Require(extension == MemoryExtension::Nvidia, "Queried unsupported NVX extension");
        ++memoryQueries;
        values[0] = availableKiB;
    } else {
        originalGetIntegerv(name, values);
    }
}

struct RestoreQueries {
    ~RestoreQueries() {
        glad_glGetStringi = originalGetStringi;
        glad_glGetIntegerv = originalGetIntegerv;
    }
};

void Check(MemoryExtension kind, GLint freeKiB, uint64_t expectedMiB) {
    extension = kind;
    availableKiB = freeKiB;
    memoryQueries = 0;
    Renderer renderer;
    renderer.initialize();
    Require(renderer.getTextureCacheBudgetBytes() == expectedMiB * 1024 * 1024,
            "Unexpected texture cache budget");
    Require(memoryQueries == (kind == MemoryExtension::None ? 0 : 1),
            "Memory query count or extension gating changed");
    Require(glGetError() == GL_NO_ERROR, "GL error selecting texture cache budget");
}
void CheckDynamicBudget() {
    extension = MemoryExtension::Ati;
    availableKiB = 4 * 1024 * 1024;
    memoryQueries = 0;
    const double savedTime = glfwGetTime();
    glfwSetTime(100.0);
    {
        Renderer renderer;
        renderer.initialize();
        const auto budgetMiB = [&] { return renderer.getTextureCacheBudgetBytes() / (1024 * 1024); };
        Require(budgetMiB() == 512, "Initial dynamic budget");
        availableKiB = 16 * 1024 * 1024;
        renderer.setFrameSerial(1);
        Require(memoryQueries == 1 && budgetMiB() == 512, "Queried memory on every frame");
        glfwSetTime(106.0);
        renderer.setFrameSerial(2);
        Require(memoryQueries == 2 && budgetMiB() == 2048, "Budget did not grow with free GPU memory");
        availableKiB = 4 * 1024 * 1024;
        glfwSetTime(112.0);
        renderer.setFrameSerial(3);
        Require(budgetMiB() == 512, "Budget did not shrink under memory pressure");
        availableKiB += 128 * 1024;
        glfwSetTime(118.0);
        renderer.setFrameSerial(4);
        Require(budgetMiB() == 512, "Small memory fluctuation changed the budget");
        availableKiB = -1;
        glfwSetTime(124.0);
        renderer.setFrameSerial(5);
        Require(budgetMiB() == 512, "Failed query discarded the last valid budget");
        Require(glGetError() == GL_NO_ERROR, "Dynamic budget GL error");
    }
    glfwSetTime(savedTime);
}
} // namespace

int main() {
    try {
        Window window;
        auto* handle = window.makeWindow(Modularity::GraphicsBackend::OpenGL);
        Require(handle != nullptr, "Create OpenGL context");
        glfwHideWindow(handle);
        {
            Renderer renderer;
            renderer.initialize();
            std::cout << "Native texture cache budget: "
                      << renderer.getTextureCacheBudgetBytes() / (1024 * 1024) << " MiB\n";
            Require(glGetError() == GL_NO_ERROR, "Native memory query produced a GL error");
        }
        originalGetStringi = glad_glGetStringi;
        originalGetIntegerv = glad_glGetIntegerv;
        RestoreQueries restore;
        glad_glGetStringi = GetStringi;
        glad_glGetIntegerv = GetIntegerv;
        Check(MemoryExtension::None, 0, 384);
        Check(MemoryExtension::Ati, 4 * 1024 * 1024, 512);
        Check(MemoryExtension::Ati, 16 * 1024 * 1024, 2048);
        Check(MemoryExtension::Ati, -1, 384);
        Check(MemoryExtension::Ati, 128 * 1024, 384);
        Check(MemoryExtension::Nvidia, 4 * 1024 * 1024, 512);
        Check(MemoryExtension::Nvidia, 16 * 1024 * 1024, 2048);
        Check(MemoryExtension::Nvidia, 0, 384);
        CheckDynamicBudget();
        std::cout << "PASS: extension gating, free-pool selection, fallback, runtime growth/shrink and hysteresis\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
