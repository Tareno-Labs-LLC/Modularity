#pragma once

// Reads Unity YAML mesh and texture-array data while preserving auxiliary streams.
// Material resolution supports generic Unity materials and project-owned native sidecars.

#include "SceneObject.h"

#include <cstdint>
#include <string>
#include <vector>

namespace UnityAssetImport {

enum class AssetKind {
    Unknown = 0,     // unreadable, or not a Unity YAML asset
    Mesh,            // !u!43
    Texture2DArray,  // !u!187
    Material,        // !u!21
    Prefab,          // a .prefab document
    Other            // another class, or binary serialization
};

// Classifies by the first "--- !u!<classID>" header, reading only the first few
// hundred bytes through the platform AssetSource. ".asset" alone says nothing:
// the same extension holds meshes, texture arrays, billboards and binary terrain.
AssetKind SniffAsset(const std::string& path);

// True when the file starts with "%YAML" - Unity's .mat shares Modularity's
// extension, and callers must never parse or overwrite one as a Modularity material.
bool IsUnityYamlFile(const std::string& path);

struct MeshData {
    struct Submesh {
        // De-indexed triangle list, 8 floats per corner (pos3, normal3, uv2), already
        // converted to Modularity's handedness: x mirrored and winding swapped.
        std::vector<float> vertices;
        std::vector<float> colors;  // 4 per corner, empty when the mesh has none
        std::vector<float> uv1;     // 2 per corner (TEXCOORD1), empty when absent
        // uv2Components per corner (TEXCOORD2), empty when absent. Left exactly as
        // stored: auxiliary packs pivots and branch axes into it in Unity's space, and the
        // packing does not survive negation, so the shader mirrors after decoding.
        std::vector<float> uv2;
        int uv2Components = 0;
        // Unique mirrored positions plus triangle indices into them, for physics.
        std::vector<glm::vec3> positions;
        std::vector<uint32_t> triangleIndices;
        glm::vec3 boundsMin = glm::vec3(0.0f);
        glm::vec3 boundsMax = glm::vec3(0.0f);
    };
    std::string name;
    std::vector<Submesh> submeshes;
};
bool LoadMesh(const std::string& path, MeshData& out, std::string& error);

struct TextureArrayData {
    int width = 0;
    int height = 0;
    int depth = 0;
    bool srgb = false;
    // Mip 0 of every slice, RGBA8, slice after slice, rows bottom-up (GL order).
    std::vector<uint8_t> rgba;
};
bool LoadTexture2DArray(const std::string& path, TextureArrayData& out, std::string& error);

struct ImportedMaterial {
    bool valid = false;
    std::string name;
    MaterialProperties props;
    // Paths in the form the rest of the engine stores texture paths.
    std::string albedoPath;
    std::string normalPath;
    std::string occlusionPath;
};

// Resolve prefab material slots. A <Unity material>.modularity.mat sidecar takes
// precedence; otherwise common Unity surface properties convert as Standard.
bool ResolveMeshMaterials(const std::string& meshPath,
                          std::vector<ImportedMaterial>& perSubmesh,
                          std::string& log);

} // namespace UnityAssetImport
