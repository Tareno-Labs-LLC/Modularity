#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace Modularity {

enum class GraphicsBackend {
    OpenGL,
    Vulkan
};

inline const char* ToString(GraphicsBackend backend) {
    switch (backend) {
        case GraphicsBackend::Vulkan: return "Vulkan";
        case GraphicsBackend::OpenGL:
        default: return "OpenGL";
    }
}

inline GraphicsBackend GraphicsBackendFromString(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "vulkan" || value == "vk") {
        return GraphicsBackend::Vulkan;
    }
    return GraphicsBackend::OpenGL;
}

} // namespace Modularity
