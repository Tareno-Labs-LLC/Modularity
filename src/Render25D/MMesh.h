#pragma once

#include "Common.h"
#include "Render25D/TMPresentation.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace Modularity::Render25D {

struct MMeshMaterialRef {
    std::string name = "Default";
    std::string albedoTexturePath;
    TMSurfacePresentation presentation;
};

struct MMeshMetadataHook {
    std::string key;
    std::string value;
};

struct MMeshSurface {
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    int materialIndex = 0;
};

struct MMeshAsset {
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
    std::vector<glm::vec2> uvs;
    std::vector<glm::vec3> normals;
    std::vector<MMeshMaterialRef> materials;
    std::vector<MMeshSurface> surfaces;
    std::vector<MMeshMetadataHook> metadataHooks;
    glm::vec3 boundsMin = glm::vec3(FLT_MAX);
    glm::vec3 boundsMax = glm::vec3(-FLT_MAX);
    bool hasNormals = false;
    bool hasUVs = false;
};

struct MMeshRenderSubmesh {
    std::unique_ptr<Mesh> mesh;
    int materialIndex = 0;
    uint32_t triangleCount = 0;
    std::string materialName;
    std::string albedoTexturePath;
    TMSurfacePresentation presentation;

    MMeshRenderSubmesh();
    ~MMeshRenderSubmesh();
    MMeshRenderSubmesh(MMeshRenderSubmesh&& other) noexcept;
    MMeshRenderSubmesh& operator=(MMeshRenderSubmesh&& other) noexcept;

    MMeshRenderSubmesh(const MMeshRenderSubmesh&) = delete;
    MMeshRenderSubmesh& operator=(const MMeshRenderSubmesh&) = delete;
};

struct MMeshRenderData {
    std::string sourcePath;
    glm::vec3 boundsMin = glm::vec3(FLT_MAX);
    glm::vec3 boundsMax = glm::vec3(-FLT_MAX);
    uint32_t totalTriangleCount = 0;
    std::vector<MMeshRenderSubmesh> submeshes;
};

bool ValidateMMeshAsset(const MMeshAsset& asset, std::string& error);
void RecomputeMMeshBounds(MMeshAsset& asset);
void NormalizeMMeshAsset(MMeshAsset& asset);

} // namespace Modularity::Render25D
