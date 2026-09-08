#pragma once

#if defined(_WIN32)
    #pragma comment(linker, "/EXPORT:glfwGetCurrentContext")
    #pragma comment(linker, "/EXPORT:glfwGetKey")
    #pragma comment(linker, "/EXPORT:glfwGetGamepadState")
#endif
