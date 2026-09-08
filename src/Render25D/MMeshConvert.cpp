#include "Render25D/MMeshConvert.h"

#include <algorithm>

namespace Modularity::Render25D {

MMeshAsset BuildMMeshFromRawMesh(const RawMeshAsset& raw) {
    MMeshAsset asset;
    asset.vertices = raw.positions;
    asset.uvs = raw.uvs;
    asset.normals = raw.normals;
    asset.hasUVs = !asset.uvs.empty() && asset.uvs.size() == asset.vertices.size();
    asset.hasNormals = !asset.normals.empty() && asset.normals.size() == asset.vertices.size();
    asset.boundsMin = raw.boundsMin;
    asset.boundsMax = raw.boundsMax;

    const size_t materialCount = std::max<size_t>(1, raw.materialSlots.size());
    asset.materials.reserve(materialCount);
    for (size_t i = 0; i < materialCount; ++i) {
        MMeshMaterialRef material;
        material.name = (i < raw.materialSlots.size() && !raw.materialSlots[i].empty())
                            ? raw.materialSlots[i]
                            : ("Material_" + std::to_string(i));
        asset.materials.push_back(std::move(material));
    }

    asset.indices.reserve(raw.faces.size() * 3u);
    for (size_t materialIndex = 0; materialIndex < materialCount; ++materialIndex) {
        const uint32_t indexOffset = static_cast<uint32_t>(asset.indices.size());
        for (size_t faceIndex = 0; faceIndex < raw.faces.size(); ++faceIndex) {
            const uint32_t faceMaterial = (faceIndex < raw.faceMaterialIndices.size())
                                              ? raw.faceMaterialIndices[faceIndex]
                                              : 0u;
            const uint32_t clampedMaterial =
                (faceMaterial < materialCount) ? faceMaterial : 0u;
            if (clampedMaterial != static_cast<uint32_t>(materialIndex)) {
                continue;
            }
            const glm::u32vec3& face = raw.faces[faceIndex];
            asset.indices.push_back(face.x);
            asset.indices.push_back(face.y);
            asset.indices.push_back(face.z);
        }

        const uint32_t indexCount = static_cast<uint32_t>(asset.indices.size()) - indexOffset;
        if (indexCount == 0) {
            continue;
        }
        MMeshSurface surface;
        surface.indexOffset = indexOffset;
        surface.indexCount = indexCount;
        surface.materialIndex = static_cast<int>(materialIndex);
        asset.surfaces.push_back(surface);
    }

    if (asset.surfaces.empty() && !asset.indices.empty()) {
        MMeshSurface surface;
        surface.indexOffset = 0;
        surface.indexCount = static_cast<uint32_t>(asset.indices.size());
        surface.materialIndex = 0;
        asset.surfaces.push_back(surface);
    }

    return asset;
}

RawMeshAsset BuildRawMeshFromMMesh(const MMeshAsset& asset) {
    RawMeshAsset raw;
    raw.positions = asset.vertices;
    raw.uvs = asset.uvs;
    raw.normals = asset.normals;
    raw.hasUVs = asset.hasUVs;
    raw.hasNormals = asset.hasNormals;
    raw.boundsMin = asset.boundsMin;
    raw.boundsMax = asset.boundsMax;

    raw.materialSlots.reserve(asset.materials.size());
    for (const MMeshMaterialRef& material : asset.materials) {
        raw.materialSlots.push_back(material.name.empty()
                                        ? ("Material_" + std::to_string(raw.materialSlots.size()))
                                        : material.name);
    }
    if (raw.materialSlots.empty()) {
        raw.materialSlots.push_back("Default");
    }

    const size_t faceCount = asset.indices.size() / 3u;
    raw.faces.reserve(faceCount);
    raw.faceMaterialIndices.assign(faceCount, 0u);
    for (size_t i = 0; i + 2 < asset.indices.size(); i += 3) {
        raw.faces.push_back(glm::u32vec3(asset.indices[i],
                                         asset.indices[i + 1],
                                         asset.indices[i + 2]));
    }

    for (const MMeshSurface& surface : asset.surfaces) {
        const uint32_t materialIndex =
            (surface.materialIndex >= 0 &&
             surface.materialIndex < static_cast<int>(raw.materialSlots.size()))
                ? static_cast<uint32_t>(surface.materialIndex)
                : 0u;
        const size_t firstFace = surface.indexOffset / 3u;
        const size_t lastFace = std::min(faceCount, static_cast<size_t>(surface.indexOffset + surface.indexCount) / 3u);
        for (size_t face = firstFace; face < lastFace; ++face) {
            raw.faceMaterialIndices[face] = materialIndex;
        }
    }

    return raw;
}

}
