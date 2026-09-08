#include "../include/Platform/PlatformTime.h"

#ifdef __ANDROID__
#include <ctime>
#else
#include "ThirdParty/glfw/include/GLFW/glfw3.h"
#endif

namespace Modularity::Platform {

double GetTimeSeconds() {
#ifdef __ANDROID__
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
#else
    return glfwGetTime();
#endif
}

bool HasCurrentGLContext() {
#ifdef __ANDROID__
    return true;
#else
    return glfwGetCurrentContext() != nullptr;
#endif
}

}
