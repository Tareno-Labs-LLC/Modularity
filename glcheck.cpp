// glcheck.cpp
#include "src/ThirdParty/glad/glad/glad.h"

#define GLFW_INCLUDE_NONE
#include "src/ThirdParty/glfw/include/GLFW/glfw3.h"
#include <iostream>

int main() {
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* win = glfwCreateWindow(1, 1, "check", nullptr, nullptr);
    glfwMakeContextCurrent(win);
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

    std::cout << "OpenGL: " << glGetString(GL_VERSION) << "\n";

    glfwTerminate();
    return 0;
}
