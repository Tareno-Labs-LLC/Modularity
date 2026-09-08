#include "Render25D/MMesh.h"

#include "Rendering.h"

#include <algorithm>
#include <utility>

namespace Modularity::Render25D {

namespace {

MMeshSurface MakeDefaultSurface(const MMeshAsset& asset) {
    MMeshSurface surface;
    surface.indexOffset = 0;
    surface.indexCount = static_cast<uint32_t>(asset.indices.size());
    surface.materialIndex = 0;
    return surface;
}

} // namespace

MMeshRenderSubmesh::MMeshRenderSubmesh() = default;
MMeshRenderSubmesh::~MMeshRenderSubmesh() = default;
MMeshRenderSubmesh::MMeshRenderSubmesh(MMeshRenderSubmesh&& other) noexcept = default;
MMeshRenderSubmesh& MMeshRenderSubmesh::operator=(MMeshRenderSubmesh&& other) noexcept = default;

void RecomputeMMeshBounds(MMeshAsset& asset) {
    asset.boundsMin = glm::vec3(FLT_MAX);
    asset.boundsMax = glm::vec3(-FLT_MAX);

    for (const glm::vec3& vertex : asset.vertices) {
        asset.boundsMin.x = std::min(asset.boundsMin.x, vertex.x);
        asset.boundsMin.y = std::min(asset.boundsMin.y, vertex.y);
        asset.boundsMin.z = std::min(asset.boundsMin.z, vertex.z);
        asset.boundsMax.x = std::max(asset.boundsMax.x, vertex.x);
        asset.boundsMax.y = std::max(asset.boundsMax.y, vertex.y);
        asset.boundsMax.z = std::max(asset.boundsMax.z, vertex.z);
    }
}

void NormalizeMMeshAsset(MMeshAsset& asset) {
    if (asset.materials.empty()) {
        asset.materials.push_back(MMeshMaterialRef());
    }

    for (MMeshMaterialRef& material : asset.materials) {
        material.presentation.colorTint = glm::clamp(material.presentation.colorTint, glm::vec4(0.0f), glm::vec4(1.0f));
        material.presentation.affineTextureWarpStrength =
            std::clamp(material.presentation.affineTextureWarpStrength, 0.0f, 1.0f);
    }

    if (asset.uvs.size() != asset.vertices.size()) {
        asset.uvs.resize(asset.vertices.size(), glm::vec2(0.0f));
    }

    if (asset.normals.size() != asset.vertices.size()) {
        asset.normals.resize(asset.vertices.size(), glm::vec3(0.0f));
    }

    asset.hasUVs = false;
    for (const glm::vec2& uv : asset.uvs) {
        if (std::abs(uv.x) > 1e-6f || std::abs(uv.y) > 1e-6f) {
            asset.hasUVs = true;
            break;
        }
    }

    asset.hasNormals = false;
    for (const glm::vec3& normal : asset.normals) {
        if (glm::length(normal) > 1e-6f) {
            asset.hasNormals = true;
            break;
        }
    }

    if (asset.surfaces.empty() && !asset.indices.empty()) {
        asset.surfaces.push_back(MakeDefaultSurface(asset));
    }

    if (!asset.vertices.empty()) {
        RecomputeMMeshBounds(asset);
    }
}

bool ValidateMMeshAsset(const MMeshAsset& inputAsset, std::string& error) {
    MMeshAsset asset = inputAsset;
    NormalizeMMeshAsset(asset);

    if (asset.vertices.empty()) {
        error = "MMesh asset has no vertices";
        return false;
    }

    if (asset.indices.empty()) {
        error = "MMesh asset has no indices";
        return false;
    }

    if ((asset.indices.size() % 3u) != 0u) {
        error = "MMesh indices must be a multiple of 3";
        return false;
    }

    for (uint32_t index : asset.indices) {
        if (index >= asset.vertices.size()) {
            error = "MMesh index references a vertex outside the vertex array";
            return false;
        }
    }

    for (const MMeshSurface& surface : asset.surfaces) {
        if ((surface.indexOffset % 3u) != 0u || (surface.indexCount % 3u) != 0u) {
            error = "MMesh surface ranges must be triangle-aligned";
            return false;
        }
        if (surface.indexOffset > asset.indices.size() ||
            surface.indexOffset + surface.indexCount > asset.indices.size()) {
            error = "MMesh surface range is outside the index buffer";
            return false;
        }
        if (surface.materialIndex < 0 ||
            surface.materialIndex >= static_cast<int>(asset.materials.size())) {
            error = "MMesh surface references an invalid material";
            return false;
        }
    }

    return true;
}

} // namespace Modularity::Render25D
