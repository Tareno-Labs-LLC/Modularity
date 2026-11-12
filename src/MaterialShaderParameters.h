#pragma once
#include "ScriptSdkCommon.h"
#include <array>
#include <cstdint>

struct MaterialShaderParameters {
    char profilePath[768] = {};
    uint8_t profileId = 0; // transient registry handle; never serialized
    std::array<glm::vec4, 32> values{};
};
