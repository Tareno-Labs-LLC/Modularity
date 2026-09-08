#include "Render25D/MMeshLoader.h"

#include "Rendering.h"
#include <cstring>

namespace Modularity::Render25D {

namespace {

constexpr uint32_t kMMeshVersion = 2;
constexpr uint32_t kMMeshFlagNormals = 1u << 0;
constexpr uint32_t kMMeshFlagUVs = 1u << 1;

struct MMeshFileHeader {
    char magic[4];
    uint32_t version = 0;
    uint32_t flags = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t materialCount = 0;
    uint32_t surfaceCount = 0;
    uint32_t metadataCount = 0;
};

template <typename T>
bool WriteValue(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return static_cast<bool>(out);
}

template <typename T>
bool ReadValue(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

bool WriteString(std::ofstream& out, const std::string& value) {
    const uint32_t length = static_cast<uint32_t>(value.size());
    return WriteValue(out, length) &&
           (length == 0u || static_cast<bool>(out.write(value.data(), static_cast<std::streamsize>(length))));
}

bool ReadString(std::ifstream& in, std::string& value) {
    uint32_t length = 0;
    if (!ReadValue(in, length)) {
        return false;
    }

    value.resize(length);
    if (length == 0u) {
        return true;
    }

    in.read(value.data(), static_cast<std::streamsize>(length));
    return static_cast<bool>(in);
}

template <typename TEnum>
bool WriteEnum(std::ofstream& out, TEnum value) {
    const uint32_t stored = static_cast<uint32_t>(value);
    return WriteValue(out, stored);
}

template <typename TEnum>
bool ReadEnum(std::ifstream& in, TEnum& value) {
    uint32_t stored = 0;
    if (!ReadValue(in, stored)) {
        return false;
    }
    value = static_cast<TEnum>(stored);
    return true;
}

glm::vec3 ComputeTriangleNormal(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    glm::vec3 normal = glm::cross(b - a, c - a);
    const float length = glm::length(normal);
    if (length <= 1e-6f) {
        return glm::vec3(0.0f, 1.0f, 0.0f);
    }
    return normal / length;
}

} // namespace

bool MMeshLoader::saveAsset(const MMeshAsset& inputAsset, const std::string& path, std::string& error) const {
    MMeshAsset asset = inputAsset;
    NormalizeMMeshAsset(asset);

    if (!ValidateMMeshAsset(asset, error)) {
        return false;
    }

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        error = "Failed to open .MMesh for writing: " + path;
        return false;
    }

    MMeshFileHeader header{};
    std::memcpy(header.magic, "MMSH", 4);
    header.version = kMMeshVersion;
    if (asset.hasNormals) header.flags |= kMMeshFlagNormals;
    if (asset.hasUVs) header.flags |= kMMeshFlagUVs;
    header.vertexCount = static_cast<uint32_t>(asset.vertices.size());
    header.indexCount = static_cast<uint32_t>(asset.indices.size());
    header.materialCount = static_cast<uint32_t>(asset.materials.size());
    header.surfaceCount = static_cast<uint32_t>(asset.surfaces.size());
    header.metadataCount = static_cast<uint32_t>(asset.metadataHooks.size());

    if (!WriteValue(out, header) ||
        !WriteValue(out, asset.boundsMin) ||
        !WriteValue(out, asset.boundsMax)) {
        error = "Failed to write .MMesh header: " + path;
        return false;
    }

    for (const glm::vec3& vertex : asset.vertices) {
        if (!WriteValue(out, vertex)) {
            error = "Failed to write .MMesh vertices: " + path;
            return false;
        }
    }

    for (uint32_t index : asset.indices) {
        if (!WriteValue(out, index)) {
            error = "Failed to write .MMesh indices: " + path;
            return false;
        }
    }

    if (asset.hasUVs) {
        for (const glm::vec2& uv : asset.uvs) {
            if (!WriteValue(out, uv)) {
                error = "Failed to write .MMesh uvs: " + path;
                return false;
            }
        }
    }

    if (asset.hasNormals) {
        for (const glm::vec3& normal : asset.normals) {
            if (!WriteValue(out, normal)) {
                error = "Failed to write .MMesh normals: " + path;
                return false;
            }
        }
    }

    for (const MMeshMaterialRef& material : asset.materials) {
        if (!WriteString(out, material.name) ||
            !WriteString(out, material.albedoTexturePath) ||
            !WriteValue(out, material.presentation.colorTint) ||
            !WriteEnum(out, material.presentation.textureFilter) ||
            !WriteValue(out, material.presentation.affineTextureWarpEnabled) ||
            !WriteValue(out, material.presentation.affineTextureWarpStrength)) {
            error = "Failed to write .MMesh materials: " + path;
            return false;
        }
    }

    for (const MMeshSurface& surface : asset.surfaces) {
        if (!WriteValue(out, surface.indexOffset) ||
            !WriteValue(out, surface.indexCount) ||
            !WriteValue(out, surface.materialIndex)) {
            error = "Failed to write .MMesh surfaces: " + path;
            return false;
        }
    }

    for (const MMeshMetadataHook& hook : asset.metadataHooks) {
        if (!WriteString(out, hook.key) ||
            !WriteString(out, hook.value)) {
            error = "Failed to write .MMesh metadata hooks: " + path;
            return false;
        }
    }

    return true;
}

bool MMeshLoader::loadAsset(const std::string& path, MMeshAsset& outAsset, std::string& error) const {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        error = "Failed to open .MMesh for reading: " + path;
        return false;
    }

    MMeshFileHeader header{};
    if (!ReadValue(in, header)) {
        error = "Failed to read .MMesh header: " + path;
        return false;
    }

    if (std::strncmp(header.magic, "MMSH", 4) != 0) {
        error = "Invalid .MMesh magic header: " + path;
        return false;
    }

    if (header.version == 0u || header.version > kMMeshVersion) {
        error = "Unsupported .MMesh version: " + std::to_string(header.version);
        return false;
    }

    MMeshAsset asset;
    asset.hasNormals = (header.flags & kMMeshFlagNormals) != 0u;
    asset.hasUVs = (header.flags & kMMeshFlagUVs) != 0u;
    asset.vertices.resize(header.vertexCount);
    asset.indices.resize(header.indexCount);
    asset.uvs.resize(asset.hasUVs ? header.vertexCount : 0u);
    asset.normals.resize(asset.hasNormals ? header.vertexCount : 0u);
    asset.materials.resize(header.materialCount);
    asset.surfaces.resize(header.surfaceCount);
    asset.metadataHooks.resize(header.metadataCount);

    if (!ReadValue(in, asset.boundsMin) ||
        !ReadValue(in, asset.boundsMax)) {
        error = "Failed to read .MMesh bounds: " + path;
        return false;
    }

    for (glm::vec3& vertex : asset.vertices) {
        if (!ReadValue(in, vertex)) {
            error = "Failed to read .MMesh vertices: " + path;
            return false;
        }
    }

    for (uint32_t& index : asset.indices) {
        if (!ReadValue(in, index)) {
            error = "Failed to read .MMesh indices: " + path;
            return false;
        }
    }

    for (glm::vec2& uv : asset.uvs) {
        if (!ReadValue(in, uv)) {
            error = "Failed to read .MMesh uvs: " + path;
            return false;
        }
    }

    for (glm::vec3& normal : asset.normals) {
        if (!ReadValue(in, normal)) {
            error = "Failed to read .MMesh normals: " + path;
            return false;
        }
    }

    for (MMeshMaterialRef& material : asset.materials) {
        if (!ReadString(in, material.name) ||
            !ReadString(in, material.albedoTexturePath)) {
            error = "Failed to read .MMesh materials: " + path;
            return false;
        }

        if (header.version >= 2u) {
            if (!ReadValue(in, material.presentation.colorTint) ||
                !ReadEnum(in, material.presentation.textureFilter) ||
                !ReadValue(in, material.presentation.affineTextureWarpEnabled) ||
                !ReadValue(in, material.presentation.affineTextureWarpStrength)) {
                error = "Failed to read .MMesh material presentation data: " + path;
                return false;
            }
        }
    }

    for (MMeshSurface& surface : asset.surfaces) {
        if (!ReadValue(in, surface.indexOffset) ||
            !ReadValue(in, surface.indexCount) ||
            !ReadValue(in, surface.materialIndex)) {
            error = "Failed to read .MMesh surfaces: " + path;
            return false;
        }
    }

    for (MMeshMetadataHook& hook : asset.metadataHooks) {
        if (!ReadString(in, hook.key) ||
            !ReadString(in, hook.value)) {
            error = "Failed to read .MMesh metadata hooks: " + path;
            return false;
        }
    }

    NormalizeMMeshAsset(asset);
    if (!ValidateMMeshAsset(asset, error)) {
        return false;
    }

    outAsset = std::move(asset);
    return true;
}

bool MMeshLoader::buildRenderData(const MMeshAsset& inputAsset, MMeshRenderData& outRenderData, std::string& error) const {
    MMeshAsset asset = inputAsset;
    NormalizeMMeshAsset(asset);

    if (!ValidateMMeshAsset(asset, error)) {
        return false;
    }

    MMeshRenderData renderData;
    renderData.boundsMin = asset.boundsMin;
    renderData.boundsMax = asset.boundsMax;

    for (const MMeshSurface& surface : asset.surfaces) {
        if (surface.indexCount == 0u) {
            continue;
        }

        std::vector<float> vertices;
        vertices.reserve((surface.indexCount / 3u) * 24u);

        MMeshRenderSubmesh submesh;
        submesh.materialIndex = surface.materialIndex;
        submesh.triangleCount = surface.indexCount / 3u;

        if (surface.materialIndex >= 0 &&
            surface.materialIndex < static_cast<int>(asset.materials.size())) {
            const MMeshMaterialRef& material = asset.materials[static_cast<size_t>(surface.materialIndex)];
            submesh.materialName = material.name;
            submesh.albedoTexturePath = material.albedoTexturePath;
            submesh.presentation = material.presentation;
        }

        for (uint32_t offset = 0; offset < surface.indexCount; offset += 3u) {
            const uint32_t i0 = asset.indices[surface.indexOffset + offset + 0u];
            const uint32_t i1 = asset.indices[surface.indexOffset + offset + 1u];
            const uint32_t i2 = asset.indices[surface.indexOffset + offset + 2u];

            const glm::vec3 faceNormal = ComputeTriangleNormal(asset.vertices[i0], asset.vertices[i1], asset.vertices[i2]);
            const uint32_t triIndices[3] = { i0, i1, i2 };

            for (uint32_t vertexIndex : triIndices) {
                const glm::vec3& position = asset.vertices[vertexIndex];
                const glm::vec3 normal = asset.hasNormals ? asset.normals[vertexIndex] : faceNormal;
                const glm::vec2 uv = asset.hasUVs ? asset.uvs[vertexIndex] : glm::vec2(0.0f);

                vertices.push_back(position.x);
                vertices.push_back(position.y);
                vertices.push_back(position.z);
                vertices.push_back(normal.x);
                vertices.push_back(normal.y);
                vertices.push_back(normal.z);
                vertices.push_back(uv.x);
                vertices.push_back(uv.y);
            }
        }

        if (vertices.empty()) {
            continue;
        }

        submesh.mesh = std::make_unique<Mesh>(vertices.data(), vertices.size() * sizeof(float));
        renderData.totalTriangleCount += submesh.triangleCount;
        renderData.submeshes.push_back(std::move(submesh));
    }

    if (renderData.submeshes.empty()) {
        error = "MMesh asset produced no drawable surfaces";
        return false;
    }

    outRenderData = std::move(renderData);
    return true;
}

bool MMeshLoader::loadRenderData(const std::string& path, MMeshRenderData& outRenderData, std::string& error) const {
    MMeshAsset asset;
    if (!loadAsset(path, asset, error)) {
        return false;
    }

    outRenderData.sourcePath = path;
    return buildRenderData(asset, outRenderData, error);
}

const MMeshRenderData* MMeshCache::getOrLoad(const std::string& path, std::string& error) {
    const auto it = cache.find(path);
    if (it != cache.end()) {
        return it->second.get();
    }

    auto renderData = std::make_shared<MMeshRenderData>();
    if (!loader.loadRenderData(path, *renderData, error)) {
        return nullptr;
    }

    renderData->sourcePath = path;
    const auto [insertedIt, inserted] = cache.emplace(path, std::move(renderData));
    (void)inserted;
    return insertedIt->second.get();
}

const MMeshRenderData* MMeshCache::store(const std::string& path,
                                         const MMeshAsset& asset,
                                         std::string& error) {
    auto renderData = std::make_shared<MMeshRenderData>();
    if (!loader.buildRenderData(asset, *renderData, error)) {
        return nullptr;
    }

    renderData->sourcePath = path;
    cache[path] = std::move(renderData);
    return cache[path].get();
}

void MMeshCache::invalidate(const std::string& path) {
    cache.erase(path);
}

void MMeshCache::clear() {
    cache.clear();
}

} // namespace Modularity::Render25D
