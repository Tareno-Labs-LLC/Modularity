#pragma once
#include "SceneObject.h"
#include <string>
struct MaterialFileData {
    MaterialProperties props;
    std::string albedo;
    std::string overlay;
    std::string normal;
    std::string metallicRoughness;
    std::string metallicMap;
    std::string roughnessMap;
    std::string opacityMap;
    std::string emissive;
    std::string occlusion;
    std::string height;
    bool useOverlay = false;
    std::string shaderPack;
    std::string vertexShader;
    std::string fragmentShader;
};
bool readMaterialFile(const std::string& path, MaterialFileData& outData);
bool writeMaterialFile(const MaterialFileData& data, const std::string& path);
