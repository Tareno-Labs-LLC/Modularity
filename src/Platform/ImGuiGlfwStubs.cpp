#ifdef __ANDROID__

#include "ImGuiGlfwStubs.h"

bool ImGui_ImplGlfw_InitForOpenGL(GLFWwindow*, bool) { return true; }
bool ImGui_ImplGlfw_InitForVulkan(GLFWwindow*, bool) { return true; }
void ImGui_ImplGlfw_Shutdown() {}
void ImGui_ImplGlfw_NewFrame() {}
void ImGui_ImplGlfw_Sleep(int) {}
float ImGui_ImplGlfw_GetContentScaleForWindow(GLFWwindow*) { return 1.0f; }
float ImGui_ImplGlfw_GetContentScaleForMonitor(GLFWmonitor*) { return 1.0f; }

#endif // __ANDROID__
