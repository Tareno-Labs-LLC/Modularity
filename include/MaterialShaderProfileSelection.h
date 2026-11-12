#pragma once
#include "MaterialShaderProfile.h"

// The Inspector may hold an absolute picker path while a material stores a
// portable one. Switching spellings must not reset edited parameter values.
inline bool SyncMaterialShaderProfileSelection(MaterialShaderParameters& parameters,
                                               const std::string& shaderPackPath) {
    if (shaderPackPath.empty() || shaderPackPath == parameters.profilePath) return false;
    if (Modularity::Platform::ToPortableAssetPath(shaderPackPath) ==
        Modularity::Platform::ToPortableAssetPath(parameters.profilePath)) return false;
    const uint8_t next = RegisterMaterialShaderProfile(shaderPackPath);
    if (next != 0 && next == parameters.profileId) return false;
    SetMaterialShaderProfile(parameters, shaderPackPath);
    return true;
}
