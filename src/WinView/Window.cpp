#include "../../include/Window/Window.h"
#include "../../include/ThirdParty/stb_image.h"
#include "../../include/Textures/StbFlipScope.h"

#include <cstdlib>
#include <iostream>

int width, height, channels;

namespace
{
    constexpr int kAnyProfile = 0;
    constexpr int kDefaultWindowWidth = 1000;
    constexpr int kDefaultWindowHeight = 800;

    void glfwErrorCallback(int code, const char* description)
    {
        std::cerr << "GLFW error (" << code << "): "
                  << (description ? description : "unknown")
                  << "\n";
    }

    void centerWindowOnPrimaryMonitor(GLFWwindow* window)
    {
        if (!window) return;
#if defined(__linux__) && !defined(__ANDROID__)
        // Wayland does not allow clients to set absolute window positions.
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            return;
        }
#endif

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        if (!monitor) return;

        int workX = 0;
        int workY = 0;
        int workW = 0;
        int workH = 0;
        glfwGetMonitorWorkarea(monitor, &workX, &workY, &workW, &workH);
        if (workW <= 0 || workH <= 0) {
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            if (!mode) return;
            workW = mode->width;
            workH = mode->height;
            workX = 0;
            workY = 0;
        }

        int windowW = 0;
        int windowH = 0;
        glfwGetWindowSize(window, &windowW, &windowH);
        if (windowW <= 0) windowW = kDefaultWindowWidth;
        if (windowH <= 0) windowH = kDefaultWindowHeight;

        const int centeredX = workX + (workW - windowW) / 2;
        const int centeredY = workY + (workH - windowH) / 2;
        glfwSetWindowPos(window, centeredX, centeredY);
    }

    GLFWwindow* tryCreateWindow(int major, int minor, int profile)
    {
        glfwDefaultWindowHints();
#if MODULARITY_OPENGL_ES
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#else
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
#endif
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, major);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, minor);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

        if (!MODULARITY_OPENGL_ES && profile != kAnyProfile)
        {
            glfwWindowHint(GLFW_OPENGL_PROFILE, profile);
        }

        return glfwCreateWindow(kDefaultWindowWidth, kDefaultWindowHeight, "Modularity", nullptr, nullptr);
    }

    GLFWwindow* tryCreateVulkanWindow()
    {
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

        return glfwCreateWindow(kDefaultWindowWidth, kDefaultWindowHeight, "Modularity", nullptr, nullptr);
    }

    bool loadOpenGLFunctions()
    {
#if MODULARITY_OPENGL_ES
        return true;
#else
        return gladLoadGLLoader((GLADloadproc)glfwGetProcAddress) != 0;
#endif
    }

} // namespace

GLFWwindow* Window::makeWindow(Modularity::GraphicsBackend backend)
{
    // GLFW window icons are top-row-first.
    const StbFlipScope flipForIcon(false);
    unsigned char* pixels = stbi_load(
        "Resources/Engine-Root/Modu-Logo.png",
        &width,
        &height,
        &channels,
        4
    );

#if defined(__ANDROID__)
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL);
#elif defined(__linux__)
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    const char* x11_display     = std::getenv("DISPLAY");

    if (wayland_display && *wayland_display &&
        glfwPlatformSupported(GLFW_PLATFORM_WAYLAND))
    {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    }
    else if (x11_display && *x11_display &&
             glfwPlatformSupported(GLFW_PLATFORM_X11))
    {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    }
    else
    {
        glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
    }
#endif

    glfwSetErrorCallback(glfwErrorCallback);

    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW\n";
        return nullptr;
    }

    GLFWwindow* window = nullptr;

    if (backend == Modularity::GraphicsBackend::Vulkan)
    {
        window = tryCreateVulkanWindow();

        if (!window)
        {
            std::cerr << "Failed to create GLFW window (Vulkan no-API)\n";
        }
    }
    else
    {
#ifdef __ANDROID__
        window = tryCreateVulkanWindow();
        if (!window)
        {
            std::cerr << "Failed to create null-backend GLFW window\n";
        }
#elif MODULARITY_OPENGL_ES
        window = tryCreateWindow(3, 0, kAnyProfile);

        if (!window)
        {
            std::cerr << "Failed to create GLFW window (OpenGL ES 3.0). Retrying...\n";
            window = tryCreateWindow(2, 0, kAnyProfile);
        }
#else
        {
            static const int kCoreLadder[][2] = { {4, 6}, {4, 5}, {4, 3}, {3, 3} };
            for (const auto& version : kCoreLadder)
            {
                window = tryCreateWindow(version[0], version[1], GLFW_OPENGL_CORE_PROFILE);
                if (window) break;
            }
        }

        if (!window)
        {
            std::cerr << "Failed to create GLFW window (OpenGL 3.3 core). Retrying...\n";
            window = tryCreateWindow(3, 3, kAnyProfile);
        }

        if (!window)
        {
            std::cerr << "Failed to create GLFW window (OpenGL 3.3 any). Retrying...\n";
            window = tryCreateWindow(3, 0, kAnyProfile);
        }

        if (!window)
        {
            std::cerr << "Failed to create GLFW window (OpenGL 3.0 any). Retrying...\n";
            window = tryCreateWindow(2, 1, kAnyProfile);
        }
#endif
    }

    if (!window)
    {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return nullptr;
    }

#ifndef __ANDROID__
    if (backend == Modularity::GraphicsBackend::OpenGL)
    {
        glfwMakeContextCurrent(window);
        glfwSwapInterval(0);

        if (!loadOpenGLFunctions())
        {
            std::cerr << "Failed to initialize " << Modularity::OpenGLApiName() << " functions\n";
            return nullptr;
        }

        std::cout << Modularity::OpenGLApiName() << ": " << glGetString(GL_VERSION) << "\n";
    }
#endif

    if (pixels)
    {
        GLFWimage icon;
        icon.width  = width;
        icon.height = height;
        icon.pixels = pixels;

        glfwSetWindowIcon(window, 1, &icon);
        stbi_image_free(pixels);
    }

    centerWindowOnPrimaryMonitor(window);
    glfwShowWindow(window);

    return window;
}
