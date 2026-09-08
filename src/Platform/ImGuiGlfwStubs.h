#pragma once

struct GLFWwindow;
struct GLFWmonitor;

bool  ImGui_ImplGlfw_InitForOpenGL(GLFWwindow* window, bool install_callbacks);
bool  ImGui_ImplGlfw_InitForVulkan(GLFWwindow* window, bool install_callbacks);
void  ImGui_ImplGlfw_Shutdown();
void  ImGui_ImplGlfw_NewFrame();
void  ImGui_ImplGlfw_Sleep(int milliseconds);
float ImGui_ImplGlfw_GetContentScaleForWindow(GLFWwindow* window);
float ImGui_ImplGlfw_GetContentScaleForMonitor(GLFWmonitor* monitor);
