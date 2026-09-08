// Test shim: mirrors the RawMeshAsset layout from src/ModelLoader.h without
// pulling in the renderer, so MapCarve.cpp can be unit-tested standalone.
#pragma once
#include <glm/glm.hpp>
#include <cfloat>
#include <cstdint>
#include <string>
#include <vector>

struct RawMeshAsset {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<glm::u32vec2> edges;
    std::vector<glm::u32vec3> faces;
    std::vector<uint32_t> faceMaterialIndices;
    std::vector<uint32_t> faceIslandIds;
    std::vector<std::string> materialSlots;
    glm::vec3 boundsMin = glm::vec3(FLT_MAX);
    glm::vec3 boundsMax = glm::vec3(-FLT_MAX);
    bool hasNormals = false;
    bool hasUVs = false;
};
