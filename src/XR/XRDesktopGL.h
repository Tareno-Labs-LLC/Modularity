#pragma once

#include <string>

namespace Modularity::XR {

bool XRDesktopGLBindingAvailable();

const char* XRDesktopGLBindingName();

const void* XRDesktopGLBuildSessionBinding(void* glfwWindow, std::string& outError);

std::string XRDesktopGLContextDescription();

}
