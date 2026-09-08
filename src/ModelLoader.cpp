#include "ModelLoader.h"
#include "../include/Platform/AssetSource.h"
#include "AssimpPortableIO.h"
#include "Render25D/MMeshConvert.h"
#include "Render25D/MMeshLoader.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include "ThirdParty/zstd/zstd.h"
#include <iostream>
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_set>
#if MODULARITY_ENABLE_ASSIMP
#include <assimp/material.h>
#endif
#include "ThirdParty/glm/gtc/quaternion.hpp"
#include <system_error>

ModelLoader& ModelLoader::getInstance() {
    static ModelLoader instance;
    return instance;
}

#if MODULARITY_ENABLE_ASSIMP

static void collectRawMeshData(aiNode* node, const aiScene* scene, const aiMatrix4x4& parentTransform, RawMeshAsset& out);
static bool buildSceneMeshes(const std::string& filepath, const aiScene* scene,
                             std::vector<OBJLoader::LoadedMesh>& loadedMeshes,
                             ModelSceneData& out, std::string& errorMsg);
static void buildSceneNodes(const aiScene* scene,
                            const std::vector<int>& meshIndices,
                            ModelSceneData& out);
static glm::mat4 aiToGlm(const aiMatrix4x4& m);

#endif

ModelLoader& getModelLoader() {
    return ModelLoader::getInstance();
}

#if MODULARITY_ENABLE_ASSIMP

namespace {
constexpr uint32_t kRMeshFormatVersion1 = 1;
constexpr uint32_t kRMeshFormatVersion2 = 2;
constexpr uint32_t kRMeshFlagNormals = 1u << 0;
constexpr uint32_t kRMeshFlagUVs = 1u << 1;
constexpr uint32_t kRMeshFlagFaceMaterials = 1u << 2;
constexpr uint32_t kRMeshFlagFaceIslands = 1u << 3;
constexpr uint32_t kRMeshFlagEdges = 1u << 4;

enum class BlendCompressionKind {
    Unknown,
    Native,
    GZip,
    ZStd
};

enum class BlendContainerFormat {
    Unknown,
    Legacy,
    ExtendedV5Plus
};

struct ScopedImportedTempFile {
    fs::path path;

    ~ScopedImportedTempFile() {
        if (path.empty()) {
            return;
        }

        std::error_code ec;
        fs::remove(path, ec);
    }
};

bool IsUsableScene(const aiScene* scene) {
    return scene != nullptr &&
           (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) == 0 &&
           scene->mRootNode != nullptr;
}

bool HasBlendExtension(const std::string& filepath) {
    std::string ext = fs::path(filepath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".blend";
}

BlendCompressionKind DetectBlendCompression(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return BlendCompressionKind::Unknown;
    }

    std::array<unsigned char, 8> bytes{};
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    const std::streamsize readCount = file.gcount();
    if (readCount < 4) {
        return BlendCompressionKind::Unknown;
    }

    if (readCount >= 7 && std::memcmp(bytes.data(), "BLENDER", 7) == 0) {
        return BlendCompressionKind::Native;
    }
    if (bytes[0] == 0x1f && bytes[1] == 0x8b) {
        return BlendCompressionKind::GZip;
    }
    if (bytes[0] == 0x28 && bytes[1] == 0xB5 && bytes[2] == 0x2F && bytes[3] == 0xFD) {
        return BlendCompressionKind::ZStd;
    }
    return BlendCompressionKind::Unknown;
}

BlendContainerFormat DetectBlendContainerFormat(const fs::path& path, std::string* detail = nullptr) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return BlendContainerFormat::Unknown;
    }

    std::array<char, 20> bytes{};
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const std::streamsize readCount = file.gcount();
    if (readCount < 12) {
        return BlendContainerFormat::Unknown;
    }

    if (std::memcmp(bytes.data(), "BLENDER", 7) != 0) {
        return BlendContainerFormat::Unknown;
    }

    const char pointerCode = bytes[7];
    if (pointerCode == '-' || pointerCode == '_') {
        return BlendContainerFormat::Legacy;
    }

    if (readCount >= 17 && detail) {
        *detail = std::string(bytes.data() + 7, 10);
    }
    return BlendContainerFormat::ExtendedV5Plus;
}

std::string QuoteShellArg(const std::string& value) {
#ifdef _WIN32
    std::string quoted = "\"";
    for (char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted += ch;
        }
    }
    quoted += "\"";
    return quoted;
#else
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
#endif
}

bool RunCommandCapture(const std::string& command, std::string& output) {
    std::array<char, 256> buffer{};
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) {
        output = "Failed to spawn process: " + command;
        return false;
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }

#ifdef _WIN32
    const int returnCode = _pclose(pipe);
#else
    const int returnCode = pclose(pipe);
#endif
    return returnCode == 0;
}

fs::path MakeTempBlendPath(const fs::path& sourcePath) {
    std::error_code ec;
    fs::path tempDir = fs::temp_directory_path(ec);
    if (ec || tempDir.empty()) {
        tempDir = sourcePath.parent_path();
    }

    std::string stem = sourcePath.stem().string();
    if (stem.empty()) {
        stem = "blend";
    }
    for (char& ch : stem) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) {
            ch = '_';
        }
    }

    const auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 32; ++attempt) {
        fs::path candidate = tempDir /
            ("modularity_" + stem + "_" + std::to_string(timestamp) + "_" + std::to_string(attempt) + ".blend");
        if (!fs::exists(candidate, ec)) {
            return candidate;
        }
    }

    return tempDir / ("modularity_" + stem + ".blend");
}

bool DecompressZstdBlend(const std::string& filepath, fs::path& outPath, std::string& errorMsg) {
    outPath = MakeTempBlendPath(fs::path(filepath));
    std::error_code ec;
    fs::remove(outPath, ec);

    std::ifstream source(filepath, std::ios::binary);
    if (!source) {
        errorMsg = "Blender file uses Zstandard compression, but the file could not be opened for reading.";
        return false;
    }

    ZSTD_DStream* stream = ZSTD_createDStream();
    if (stream == nullptr) {
        errorMsg = "Blender file uses Zstandard compression, but the decompressor could not be created.";
        return false;
    }
    const size_t initResult = ZSTD_initDStream(stream);
    if (ZSTD_isError(initResult)) {
        errorMsg = std::string("Failed to start Zstandard decompression: ") +
                   ZSTD_getErrorName(initResult);
        ZSTD_freeDStream(stream);
        return false;
    }

    std::ofstream destination(outPath, std::ios::binary | std::ios::trunc);
    if (!destination) {
        errorMsg = "Blender file uses Zstandard compression, but a temporary file for the "
                   "decompressed data could not be created.";
        ZSTD_freeDStream(stream);
        return false;
    }

    std::vector<char> inBuffer(ZSTD_DStreamInSize());
    std::vector<char> outBuffer(ZSTD_DStreamOutSize());
    bool failed = false;
    unsigned long long totalWritten = 0;

    while (!failed) {
        source.read(inBuffer.data(), static_cast<std::streamsize>(inBuffer.size()));
        const std::streamsize readCount = source.gcount();
        if (readCount <= 0) {
            break;
        }

        ZSTD_inBuffer input{inBuffer.data(), static_cast<size_t>(readCount), 0};
        while (input.pos < input.size) {
            ZSTD_outBuffer output{outBuffer.data(), outBuffer.size(), 0};
            const size_t status = ZSTD_decompressStream(stream, &output, &input);
            if (ZSTD_isError(status)) {
                errorMsg = std::string("Failed to decompress Zstandard-compressed .blend: ") +
                           ZSTD_getErrorName(status);
                failed = true;
                break;
            }
            if (output.pos > 0) {
                destination.write(outBuffer.data(), static_cast<std::streamsize>(output.pos));
                if (!destination) {
                    errorMsg = "Failed to write the decompressed .blend to a temporary file "
                               "(out of disk space?).";
                    failed = true;
                    break;
                }
                totalWritten += output.pos;
            }
        }
    }

    ZSTD_freeDStream(stream);
    destination.close();

    if (!failed && totalWritten == 0) {
        errorMsg = "Blender file uses Zstandard compression, but decompression produced no data.";
        failed = true;
    }

    if (failed) {
        std::error_code removeEc;
        fs::remove(outPath, removeEc);
        return false;
    }

    if (!fs::exists(outPath, ec) || ec) {
        errorMsg = "Blender file uses Zstandard compression, but the temporary decompressed "
                   ".blend was not created.";
        return false;
    }

    return true;
}

bool DescribeUnsupportedBlendFormat(const fs::path& path, std::string& errorMsg) {
    std::string detail;
    const BlendContainerFormat format = DetectBlendContainerFormat(path, &detail);
    if (format != BlendContainerFormat::ExtendedV5Plus) {
        return false;
    }

    errorMsg =
        "Unsupported .blend format: this file uses the newer Blender 5+ container/header format";
    if (!detail.empty()) {
        errorMsg += " (" + detail + ")";
    }
    errorMsg +=
        ". The bundled Assimp importer only supports legacy .blend files. "
        "Export to glTF/FBX/OBJ or save/export from an older Blender version.";
    return true;
}

const aiScene* ReadSceneWithBlendFallback(Assimp::Importer& importer,
                                          const std::string& rawFilepath,
                                          unsigned int importFlags,
                                          ScopedImportedTempFile& tempFile,
                                          std::string& errorMsg) {
    Modularity::AttachPortableAssetIO(importer);

    const std::string filepath = Modularity::Platform::ResolveAssetPath(rawFilepath);

    const aiScene* scene = importer.ReadFile(filepath, importFlags);
    if (IsUsableScene(scene) || !HasBlendExtension(filepath)) {
        return scene;
    }

    if (DescribeUnsupportedBlendFormat(fs::path(filepath), errorMsg)) {
        const std::string assimpError = importer.GetErrorString();
        if (!assimpError.empty()) {
            errorMsg += " (tried anyway - Assimp reported: " + assimpError + ")";
        }
        return nullptr;
    }

    const BlendCompressionKind compression = DetectBlendCompression(filepath);
    if (compression == BlendCompressionKind::ZStd) {
        if (!DecompressZstdBlend(filepath, tempFile.path, errorMsg)) {
            return nullptr;
        }

        scene = importer.ReadFile(tempFile.path.string(), importFlags);
        if (!IsUsableScene(scene)) {
            std::string formatNote;
            if (DescribeUnsupportedBlendFormat(tempFile.path, formatNote)) {
                errorMsg = formatNote + " (tried anyway - Assimp reported: " +
                           std::string(importer.GetErrorString()) + ")";
            } else {
                errorMsg = "Assimp error after decompressing Zstandard-compressed .blend: " +
                           std::string(importer.GetErrorString());
            }
        }
        return scene;
    }

    if (compression == BlendCompressionKind::Unknown) {
        errorMsg =
            "Invalid or unsupported .blend container. Expected native Blender data, gzip, or Zstandard-compressed data.";
    }
    return scene;
}

void recomputeRawBounds(RawMeshAsset& mesh) {
    mesh.boundsMin = glm::vec3(FLT_MAX);
    mesh.boundsMax = glm::vec3(-FLT_MAX);
    for (const auto& p : mesh.positions) {
        mesh.boundsMin.x = std::min(mesh.boundsMin.x, p.x);
        mesh.boundsMin.y = std::min(mesh.boundsMin.y, p.y);
        mesh.boundsMin.z = std::min(mesh.boundsMin.z, p.z);
        mesh.boundsMax.x = std::max(mesh.boundsMax.x, p.x);
        mesh.boundsMax.y = std::max(mesh.boundsMax.y, p.y);
        mesh.boundsMax.z = std::max(mesh.boundsMax.z, p.z);
    }
}

void ensureRawNormals(RawMeshAsset& mesh) {
    mesh.normals.assign(mesh.positions.size(), glm::vec3(0.0f));
    for (const auto& face : mesh.faces) {
        if (face.x >= mesh.positions.size() ||
            face.y >= mesh.positions.size() ||
            face.z >= mesh.positions.size()) {
            continue;
        }
        const glm::vec3& a = mesh.positions[face.x];
        const glm::vec3& b = mesh.positions[face.y];
        const glm::vec3& c = mesh.positions[face.z];
        glm::vec3 n = glm::cross(b - a, c - a);
        if (glm::length(n) < 1e-8f) continue;
        n = glm::normalize(n);
        mesh.normals[face.x] += n;
        mesh.normals[face.y] += n;
        mesh.normals[face.z] += n;
    }
    for (auto& n : mesh.normals) {
        if (glm::length(n) > 1e-6f) {
            n = glm::normalize(n);
        }
    }
    mesh.hasNormals = true;
}

void ensureRawTopology(RawMeshAsset& mesh) {
    std::unordered_map<uint64_t, glm::u32vec2> uniqueEdges;
    uniqueEdges.reserve(mesh.faces.size() * 3);
    auto makeEdgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
        const uint32_t lo = std::min(a, b);
        const uint32_t hi = std::max(a, b);
        return (static_cast<uint64_t>(lo) << 32) | static_cast<uint64_t>(hi);
    };

    std::vector<std::vector<uint32_t>> vertexToFaces(mesh.positions.size());
    for (uint32_t fi = 0; fi < static_cast<uint32_t>(mesh.faces.size()); ++fi) {
        const auto& f = mesh.faces[fi];
        if (f.x < mesh.positions.size()) vertexToFaces[f.x].push_back(fi);
        if (f.y < mesh.positions.size()) vertexToFaces[f.y].push_back(fi);
        if (f.z < mesh.positions.size()) vertexToFaces[f.z].push_back(fi);

        const uint32_t tri[3] = { f.x, f.y, f.z };
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = tri[e];
            const uint32_t b = tri[(e + 1) % 3];
            if (a >= mesh.positions.size() || b >= mesh.positions.size()) continue;
            const uint64_t key = makeEdgeKey(a, b);
            uniqueEdges.emplace(key, glm::u32vec2(std::min(a, b), std::max(a, b)));
        }
    }

    mesh.edges.clear();
    mesh.edges.reserve(uniqueEdges.size());
    for (const auto& it : uniqueEdges) {
        mesh.edges.push_back(it.second);
    }
    std::sort(mesh.edges.begin(), mesh.edges.end(), [](const glm::u32vec2& a, const glm::u32vec2& b) {
        if (a.x != b.x) return a.x < b.x;
        return a.y < b.y;
    });

    mesh.faceIslandIds.assign(mesh.faces.size(), 0u);
    std::vector<uint8_t> visited(mesh.faces.size(), 0u);
    uint32_t islandId = 0;

    for (uint32_t start = 0; start < static_cast<uint32_t>(mesh.faces.size()); ++start) {
        if (visited[start]) continue;
        std::vector<uint32_t> stack;
        stack.push_back(start);
        visited[start] = 1u;
        mesh.faceIslandIds[start] = islandId;

        while (!stack.empty()) {
            uint32_t current = stack.back();
            stack.pop_back();
            const auto& f = mesh.faces[current];
            const uint32_t tri[3] = { f.x, f.y, f.z };

            for (int i = 0; i < 3; ++i) {
                const uint32_t v = tri[i];
                if (v >= vertexToFaces.size()) continue;
                for (uint32_t neighbor : vertexToFaces[v]) {
                    if (neighbor >= visited.size() || visited[neighbor]) continue;
                    visited[neighbor] = 1u;
                    mesh.faceIslandIds[neighbor] = islandId;
                    stack.push_back(neighbor);
                }
            }
        }
        ++islandId;
    }
}

void sanitizeRawMeshAsset(RawMeshAsset& mesh) {
    if (mesh.materialSlots.empty()) {
        mesh.materialSlots.push_back("Default");
    }

    if (mesh.uvs.size() != mesh.positions.size()) {
        mesh.uvs.resize(mesh.positions.size(), glm::vec2(0.0f));
    }

    if (mesh.normals.size() != mesh.positions.size()) {
        mesh.normals.resize(mesh.positions.size(), glm::vec3(0.0f));
    }

    if (mesh.faceMaterialIndices.size() != mesh.faces.size()) {
        mesh.faceMaterialIndices.resize(mesh.faces.size(), 0u);
    }
    const uint32_t maxMaterialIndex = static_cast<uint32_t>(std::max<size_t>(1, mesh.materialSlots.size()) - 1);
    for (auto& matIdx : mesh.faceMaterialIndices) {
        matIdx = std::min(matIdx, maxMaterialIndex);
    }

    if (mesh.positions.empty() || mesh.faces.empty()) {
        mesh.hasNormals = false;
        mesh.hasUVs = false;
        mesh.edges.clear();
        mesh.faceIslandIds.clear();
        mesh.boundsMin = glm::vec3(FLT_MAX);
        mesh.boundsMax = glm::vec3(-FLT_MAX);
        return;
    }

    recomputeRawBounds(mesh);
    ensureRawTopology(mesh);

    bool hasAnyNormal = false;
    for (const auto& n : mesh.normals) {
        if (glm::length(n) > 1e-4f) {
            hasAnyNormal = true;
            break;
        }
    }
    if (!hasAnyNormal) {
        ensureRawNormals(mesh);
    } else {
        mesh.hasNormals = true;
    }

    mesh.hasUVs = false;
    for (const auto& uv : mesh.uvs) {
        if (std::abs(uv.x) > 1e-6f || std::abs(uv.y) > 1e-6f) {
            mesh.hasUVs = true;
            break;
        }
    }
}

struct RawGpuBuildResult {
    std::vector<float> fullVertices;
    std::vector<glm::vec3> triPositions;
    std::vector<uint32_t> triIndices;
    std::vector<std::vector<float>> submeshVertices;
    std::vector<int> submeshFaceCounts;
};

bool buildRawGpuData(const RawMeshAsset& inMesh, RawGpuBuildResult& out, std::string& errorMsg) {
    RawMeshAsset mesh = inMesh;
    sanitizeRawMeshAsset(mesh);
    if (mesh.positions.empty() || mesh.faces.empty()) {
        errorMsg = "Raw mesh has no geometry";
        return false;
    }

    const size_t materialCount = std::max<size_t>(1, mesh.materialSlots.size());
    out = RawGpuBuildResult();
    out.submeshVertices.resize(materialCount);
    out.submeshFaceCounts.assign(materialCount, 0);

    out.fullVertices.reserve(mesh.faces.size() * 3 * 8);
    out.triPositions.reserve(mesh.faces.size() * 3);
    out.triIndices.reserve(mesh.faces.size() * 3);

    auto getNorm = [&](uint32_t idx) -> glm::vec3 {
        if (idx < mesh.normals.size()) return mesh.normals[idx];
        return glm::vec3(0.0f);
    };
    auto getUV = [&](uint32_t idx) -> glm::vec2 {
        if (idx < mesh.uvs.size()) return mesh.uvs[idx];
        return glm::vec2(0.0f);
    };

    for (size_t fi = 0; fi < mesh.faces.size(); ++fi) {
        const auto& face = mesh.faces[fi];
        if (face.x >= mesh.positions.size() ||
            face.y >= mesh.positions.size() ||
            face.z >= mesh.positions.size()) {
            continue;
        }

        uint32_t materialIdx = 0u;
        if (fi < mesh.faceMaterialIndices.size()) {
            materialIdx = std::min<uint32_t>(mesh.faceMaterialIndices[fi], static_cast<uint32_t>(materialCount - 1));
        }
        out.submeshFaceCounts[materialIdx] += 1;

        const uint32_t idx[3] = { face.x, face.y, face.z };
        glm::vec3 faceNormal(0.0f);
        if (!mesh.hasNormals) {
            const glm::vec3& a = mesh.positions[idx[0]];
            const glm::vec3& b = mesh.positions[idx[1]];
            const glm::vec3& c = mesh.positions[idx[2]];
            faceNormal = glm::normalize(glm::cross(b - a, c - a));
        }

        out.triIndices.push_back(idx[0]);
        out.triIndices.push_back(idx[1]);
        out.triIndices.push_back(idx[2]);

        for (int i = 0; i < 3; ++i) {
            const glm::vec3 pos = mesh.positions[idx[i]];
            const glm::vec3 n = mesh.hasNormals ? getNorm(idx[i]) : faceNormal;
            const glm::vec2 uv = mesh.hasUVs ? getUV(idx[i]) : glm::vec2(0.0f);

            out.triPositions.push_back(pos);

            auto pushVertex = [&](std::vector<float>& verts) {
                verts.push_back(pos.x);
                verts.push_back(pos.y);
                verts.push_back(pos.z);
                verts.push_back(n.x);
                verts.push_back(n.y);
                verts.push_back(n.z);
                verts.push_back(uv.x);
                verts.push_back(uv.y);
            };
            pushVertex(out.fullVertices);
            pushVertex(out.submeshVertices[materialIdx]);
        }
    }

    if (out.fullVertices.empty()) {
        errorMsg = "No valid triangles were produced from raw mesh";
        return false;
    }
    return true;
}
} // namespace

std::vector<ModelFormat> ModelLoader::getSupportedFormats() {
    return {
        {".fbx", "Autodesk FBX", true},
        {".obj", "Wavefront OBJ", false},
        {".gltf", "glTF 2.0", true},
        {".glb", "glTF Binary", true},
        {".dae", "Collada", true},
        {".blend", "Blender", true},
        {".3ds", "3D Studio Max", false},
        {".ase", "3D Studio ASE", false},
        {".ifc", "IFC-STEP", false},
        {".xgl", "XGL", false},
        {".zgl", "ZGL", false},
        {".ply", "Stanford PLY", false},
        {".dxf", "AutoCAD DXF", false},
        {".lwo", "LightWave", false},
        {".lws", "LightWave Scene", false},
        {".lxo", "Modo", false},
        {".stl", "Stereolithography", false},
        {".x", "DirectX X", true},
        {".ac", "AC3D", false},
        {".ms3d", "Milkshape 3D", true},
        {".cob", "TrueSpace", false},
        {".scn", "TrueSpace Scene", false},
        {".bvh", "Biovision BVH", true},
        {".csm", "CharacterStudio Motion", true},
        {".irrmesh", "Irrlicht Mesh", false},
        {".rmesh", "Modularity Raw Mesh", false},
        {".mmesh", "Modularity 2.5D Mesh", false},
        {".irr", "Irrlicht Scene", false},
        {".mdl", "Quake MDL", true},
        {".md2", "Quake II MD2", true},
        {".md3", "Quake III MD3", true},
        {".pk3", "Quake III BSP", false},
        {".mdc", "RtCW MDC", true},
        {".md5mesh", "Doom 3 MD5", true},
        {".md5anim", "Doom 3 MD5 Anim", true},
        {".smd", "Valve SMD", true},
        {".vta", "Valve VTA", false},
        {".ogex", "Open Game Engine Exchange", true},
        {".3d", "Unreal 3D", false},
        {".b3d", "BlitzBasic 3D", true},
        {".q3d", "Quick3D", false},
        {".q3s", "Quick3D Scene", false},
        {".nff", "Neutral File Format", false},
        {".off", "Object File Format", false},
        {".raw", "Raw Triangles", false},
        {".ter", "Terragen Terrain", false},
        {".hmp", "3D GameStudio HMP", false},
        {".ndo", "Nendo", false},
    };
}

bool ModelLoader::isSupported(const std::string& filepath) const {
    std::string ext = fs::path(filepath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    auto formats = getSupportedFormats();
    for (const auto& format : formats) {
        if (format.extension == ext) {
            return true;
        }
    }
    return false;
}

ModelLoadResult ModelLoader::loadModel(const std::string& filepath) {
    ModelLoadResult result;
    std::string extLower = fs::path(filepath).extension().string();
    std::transform(extLower.begin(), extLower.end(), extLower.begin(), ::tolower);

    // Check if already loaded
    for (size_t i = 0; i < loadedMeshes.size(); i++) {
        if (loadedMeshes[i].path == filepath) {
            result.success = true;
            result.meshIndex = static_cast<int>(i);
            result.meshIndices.push_back(result.meshIndex);
            const auto& mesh = loadedMeshes[i];
            result.vertexCount = mesh.vertexCount;
            result.faceCount = mesh.faceCount;
            result.hasNormals = mesh.hasNormals;
            result.hasTexCoords = mesh.hasTexCoords;
            return result;
        }
    }

    // Check if format is supported
    if (!isSupported(filepath)) {
        result.errorMessage = "Unsupported file format: " + fs::path(filepath).extension().string();
        return result;
    }

    if (extLower == ".rmesh" || extLower == ".mmesh") {
        RawMeshAsset raw;
        std::string error;
        if (!loadRawMesh(filepath, raw, error)) {
            result.errorMessage = error;
            return result;
        }

        RawGpuBuildResult gpu;
        if (!buildRawGpuData(raw, gpu, result.errorMessage)) {
            return result;
        }

        OBJLoader::LoadedMesh loaded;
        loaded.path = filepath;
        loaded.name = fs::path(filepath).stem().string();
        loaded.mesh = std::make_unique<Mesh>(gpu.fullVertices.data(), gpu.fullVertices.size() * sizeof(float));
        loaded.vertexCount = static_cast<int>(gpu.fullVertices.size() / 8);
        loaded.faceCount = static_cast<int>(raw.faces.size());
        loaded.hasNormals = raw.hasNormals;
        loaded.hasTexCoords = raw.hasUVs;
        loaded.boundsMin = raw.boundsMin;
        loaded.boundsMax = raw.boundsMax;
        loaded.triangleVertices = std::move(gpu.triPositions);
        loaded.positions = raw.positions;
        loaded.triangleIndices = std::move(gpu.triIndices);
        loaded.materialSlots = raw.materialSlots;
        loaded.subMeshes.clear();
        loaded.subMeshes.reserve(gpu.submeshVertices.size());
        for (size_t matIdx = 0; matIdx < gpu.submeshVertices.size(); ++matIdx) {
            if (gpu.submeshVertices[matIdx].empty()) continue;
            OBJLoader::LoadedMesh::SubMesh sub;
            sub.materialIndex = static_cast<int>(matIdx);
            sub.faceCount = gpu.submeshFaceCounts[matIdx];
            sub.vertexCount = static_cast<int>(gpu.submeshVertices[matIdx].size() / 8);
            sub.mesh = std::make_unique<Mesh>(
                gpu.submeshVertices[matIdx].data(),
                gpu.submeshVertices[matIdx].size() * sizeof(float));
            loaded.subMeshes.push_back(std::move(sub));
        }

        loadedMeshes.push_back(std::move(loaded));

        result.success = true;
        result.meshIndex = static_cast<int>(loadedMeshes.size() - 1);
        result.vertexCount = static_cast<int>(raw.positions.size());
        result.faceCount = static_cast<int>(raw.faces.size());
        result.meshCount = 1;
        result.hasNormals = raw.hasNormals;
        result.hasTexCoords = raw.hasUVs;
        result.meshNames.push_back(loadedMeshes.back().name);
        return result;
    }

    // Configure import flags
    unsigned int importFlags =
        aiProcess_Triangulate |           // Convert all faces to triangles
        aiProcess_GenSmoothNormals |      // Generate smooth normals if missing
        //aiProcess_FlipUVs |               // Flip UV coordinates for OpenGL
        aiProcess_CalcTangentSpace |      // Calculate tangents and bitangents
        aiProcess_SortByPType |           // Sort by primitive type
        aiProcess_OptimizeMeshes |        // Reduce number of meshes
        aiProcess_ValidateDataStructure;  // Validate the imported data

    // Load the model
    ScopedImportedTempFile tempFile;
    std::string importError;
    const aiScene* scene = ReadSceneWithBlendFallback(importer, filepath, importFlags, tempFile, importError);

    if (!IsUsableScene(scene)) {
        result.errorMessage = importError.empty()
            ? "Assimp error: " + std::string(importer.GetErrorString())
            : importError;
        return result;
    }

    glm::vec3 boundsMin(FLT_MAX);
    glm::vec3 boundsMax(-FLT_MAX);
    std::vector<glm::vec3> triPositions;
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;

    // Process all meshes in the scene
    std::vector<float> vertices;
    result.meshCount = scene->mNumMeshes;
    result.hasNormals = true;
    result.hasTexCoords = false;
    result.hasTangents = false;

    // Process the root node recursively
    processNode(scene->mRootNode, scene, aiMatrix4x4(), vertices, triPositions, positions, indices, boundsMin, boundsMax);

    // Check mesh properties
    for (unsigned int i = 0; i < scene->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[i];
        result.meshNames.push_back(mesh->mName.C_Str());
        result.vertexCount += mesh->mNumVertices;
        result.faceCount += mesh->mNumFaces;
        if (mesh->mTextureCoords[0]) result.hasTexCoords = true;
        if (mesh->mTangents) result.hasTangents = true;
    }

    if (vertices.empty()) {
        result.errorMessage = "No vertices found in model file";
        return result;
    }

    // Create the mesh
    OBJLoader::LoadedMesh loaded;
    loaded.path = filepath;
    loaded.name = fs::path(filepath).stem().string();
    loaded.mesh = std::make_unique<Mesh>(vertices.data(), vertices.size() * sizeof(float));
    loaded.vertexCount = result.vertexCount;
    loaded.faceCount = result.faceCount;
    loaded.hasNormals = result.hasNormals;
    loaded.hasTexCoords = result.hasTexCoords;

    loaded.boundsMin = boundsMin;
    loaded.boundsMax = boundsMax;
    loaded.triangleVertices = std::move(triPositions);
    loaded.positions = std::move(positions);
    loaded.triangleIndices = std::move(indices);

    loadedMeshes.push_back(std::move(loaded));

    result.success = true;
    result.meshIndex = static_cast<int>(loadedMeshes.size() - 1);

    std::cerr << "[ModelLoader] Loaded " << filepath << " with "
              << result.vertexCount << " vertices, "
              << result.faceCount << " faces, "
              << result.meshCount << " meshes" << std::endl;

    return result;
}

bool ModelLoader::loadModelScene(const std::string& filepath, ModelSceneData& out, std::string& errorMsg) {
    out = ModelSceneData();
    std::string extLower = fs::path(filepath).extension().string();
    std::transform(extLower.begin(), extLower.end(), extLower.begin(), ::tolower);

    if (!isSupported(filepath)) {
        errorMsg = "Unsupported file format: " + fs::path(filepath).extension().string();
        return false;
    }

    if (extLower != ".rmesh") {
        auto cached = cachedScenes.find(filepath);
        if (cached != cachedScenes.end()) {
            out = cached->second;
            return true;
        }
    }

    if (extLower == ".rmesh") {
        RawMeshAsset raw;
        if (!loadRawMesh(filepath, raw, errorMsg)) {
            return false;
        }

        int meshIndex = -1;
        for (size_t i = 0; i < loadedMeshes.size(); ++i) {
            if (loadedMeshes[i].path == filepath) {
                meshIndex = static_cast<int>(i);
                break;
            }
        }

        if (meshIndex >= 0) {
            std::string updateError;
            if (!updateRawMesh(meshIndex, raw, updateError)) {
                errorMsg = updateError.empty()
                    ? "Failed to refresh loaded .rmesh mesh data"
                    : updateError;
                return false;
            }
        } else {
            ModelLoadResult loadResult = loadModel(filepath);
            if (!loadResult.success || loadResult.meshIndex < 0) {
                errorMsg = loadResult.errorMessage.empty()
                    ? "Failed to load .rmesh as scene mesh"
                    : loadResult.errorMessage;
                return false;
            }
            meshIndex = loadResult.meshIndex;
        }

        out.materials.clear();
        out.materials.reserve(std::max<size_t>(1, raw.materialSlots.size()));
        const size_t slotCount = std::max<size_t>(1, raw.materialSlots.size());
        for (size_t i = 0; i < slotCount; ++i) {
            ModelMaterialInfo info;
            if (i < raw.materialSlots.size() && !raw.materialSlots[i].empty()) {
                info.name = raw.materialSlots[i];
            } else {
                info.name = "Material_" + std::to_string(i);
            }
            out.materials.push_back(std::move(info));
        }

        out.meshIndices = { meshIndex };
        out.meshMaterialIndices = { 0 };
        out.nodes.clear();
        ModelNodeInfo root;
        root.name = fs::path(filepath).stem().string();
        root.parentIndex = -1;
        root.meshIndices = { 0 };
        out.nodes.push_back(std::move(root));
        out.animations.clear();
        return true;
    }

    unsigned int importFlags =
        aiProcess_Triangulate |
        aiProcess_GenSmoothNormals |
        //aiProcess_FlipUVs |
        aiProcess_CalcTangentSpace |
        aiProcess_LimitBoneWeights |
        aiProcess_PopulateArmatureData |
        aiProcess_SortByPType |
        aiProcess_ValidateDataStructure;

    importer.SetPropertyBool("IMPORT_FBX_PRESERVE_PIVOTS", true);
    importer.SetPropertyBool("IMPORT_FBX_OPTIMIZE_EMPTY_ANIMATION_CURVES", false);

    ScopedImportedTempFile tempFile;
    std::string importError;
    const aiScene* scene = ReadSceneWithBlendFallback(importer, filepath, importFlags, tempFile, importError);
    if (!IsUsableScene(scene)) {
        errorMsg = importError.empty()
            ? "Assimp error: " + std::string(importer.GetErrorString())
            : importError;
        return false;
    }

    if (!buildSceneMeshes(filepath, scene, loadedMeshes, out, errorMsg)) {
        return false;
    }

    buildSceneNodes(scene, out.meshIndices, out);

    out.animations.clear();
    if (scene->mNumAnimations > 0) {
        out.animations.reserve(scene->mNumAnimations);
        for (unsigned int i = 0; i < scene->mNumAnimations; ++i) {
            aiAnimation* anim = scene->mAnimations[i];
            ModelSceneData::AnimationClip clip;
            clip.name = anim->mName.C_Str();
            if (clip.name.empty()) {
                clip.name = "Clip_" + std::to_string(i);
            }
            clip.duration = anim->mDuration;
            clip.ticksPerSecond = anim->mTicksPerSecond != 0.0 ? anim->mTicksPerSecond : 25.0;
            clip.channels.reserve(anim->mNumChannels);
            for (unsigned int c = 0; c < anim->mNumChannels; ++c) {
                aiNodeAnim* ch = anim->mChannels[c];
                ModelSceneData::AnimChannel channel;
                channel.nodeName = ch->mNodeName.C_Str();
                channel.positions.reserve(ch->mNumPositionKeys);
                for (unsigned int k = 0; k < ch->mNumPositionKeys; ++k) {
                    const auto& key = ch->mPositionKeys[k];
                    ModelSceneData::AnimVecKey vk;
                    vk.time = static_cast<float>(key.mTime);
                    vk.value = glm::vec3(key.mValue.x, key.mValue.y, key.mValue.z);
                    channel.positions.push_back(vk);
                }
                channel.rotations.reserve(ch->mNumRotationKeys);
                for (unsigned int k = 0; k < ch->mNumRotationKeys; ++k) {
                    const auto& key = ch->mRotationKeys[k];
                    ModelSceneData::AnimQuatKey qk;
                    qk.time = static_cast<float>(key.mTime);
                    qk.value = glm::quat(key.mValue.w, key.mValue.x, key.mValue.y, key.mValue.z);
                    channel.rotations.push_back(qk);
                }
                channel.scales.reserve(ch->mNumScalingKeys);
                for (unsigned int k = 0; k < ch->mNumScalingKeys; ++k) {
                    const auto& key = ch->mScalingKeys[k];
                    ModelSceneData::AnimVecKey sk;
                    sk.time = static_cast<float>(key.mTime);
                    sk.value = glm::vec3(key.mValue.x, key.mValue.y, key.mValue.z);
                    channel.scales.push_back(sk);
                }
                clip.channels.push_back(std::move(channel));
            }
            out.animations.push_back(std::move(clip));
        }
    }

    cachedScenes[filepath] = out;
    return true;
}

const ModelSceneData* ModelLoader::loadModelSceneCached(const std::string& filepath, std::string& errorMsg) {
    std::string extLower = fs::path(filepath).extension().string();
    std::transform(extLower.begin(), extLower.end(), extLower.begin(), ::tolower);
    if (extLower != ".rmesh") {
        auto cached = cachedScenes.find(filepath);
        if (cached != cachedScenes.end()) {
            return &cached->second;
        }
    }

    sceneRefScratch = ModelSceneData();
    if (!loadModelScene(filepath, sceneRefScratch, errorMsg)) {
        return nullptr;
    }
    auto cached = cachedScenes.find(filepath);
    if (cached != cachedScenes.end()) {
        return &cached->second;
    }
    return &sceneRefScratch;
}

bool ModelLoader::exportRawMesh(const std::string& inputFile, const std::string& outputFile, std::string& errorMsg) {
    fs::path inPath(inputFile);
    if (!fs::exists(inPath)) {
        errorMsg = "File not found: " + inputFile;
        return false;
    }
    if (!isSupported(inputFile)) {
        errorMsg = "Unsupported file format for raw export";
        return false;
    }

    if (IsMMeshPath(inPath)) {
        RawMeshAsset raw;
        if (!loadRawMesh(inputFile, raw, errorMsg)) {
            return false;
        }
        fs::path outPath(outputFile.empty() ? inPath : fs::path(outputFile));
        if (outPath.extension().empty() || IsMMeshPath(outPath)) {
            outPath.replace_extension(".rmesh");
        }
        return saveRawMesh(raw, outPath.string(), errorMsg);
    }

    Assimp::Importer localImporter;
    unsigned int importFlags =
        aiProcess_Triangulate |
        aiProcess_GenSmoothNormals;
        //aiProcess_FlipUVs;

    ScopedImportedTempFile tempFile;
    std::string importError;
    const aiScene* scene = ReadSceneWithBlendFallback(localImporter, inputFile, importFlags, tempFile, importError);
    if (!IsUsableScene(scene)) {
        errorMsg = importError.empty()
            ? "Assimp error: " + std::string(localImporter.GetErrorString())
            : importError;
        return false;
    }

    RawMeshAsset raw;
    collectRawMeshData(scene->mRootNode, scene, aiMatrix4x4(), raw);

    if (raw.positions.empty() || raw.faces.empty()) {
        errorMsg = "No geometry found to export";
        return false;
    }

    fs::path outPath(outputFile.empty() ? inPath : fs::path(outputFile));
    if (outPath.extension().empty()) {
        outPath.replace_extension(".rmesh");
    }

    if (!saveRawMesh(raw, outPath.string(), errorMsg)) {
        return false;
    }

    std::cerr << "[ModelLoader] Exported raw mesh to " << outPath << " ("
              << raw.positions.size() << " verts, " << raw.faces.size() << " faces)" << std::endl;
    return true;
}

bool ModelLoader::loadRawMesh(const std::string& filepath, RawMeshAsset& out, std::string& errorMsg) {
    out = RawMeshAsset();

    if (IsMMeshPath(filepath)) {
        Modularity::Render25D::MMeshLoader mmeshLoader;
        Modularity::Render25D::MMeshAsset mmeshAsset;
        if (!mmeshLoader.loadAsset(filepath, mmeshAsset, errorMsg)) {
            return false;
        }
        out = Modularity::Render25D::BuildRawMeshFromMMesh(mmeshAsset);
        if (out.positions.empty() || out.faces.empty()) {
            errorMsg = "MMesh contains no editable geometry: " + filepath;
            return false;
        }
        return true;
    }

    std::ifstream in(filepath, std::ios::binary);
    if (!in) {
        errorMsg = "Unable to open raw mesh: " + filepath;
        return false;
    }

    struct HeaderV1 {
        char magic[6];
        uint32_t version;
        uint32_t vertexCount;
        uint32_t faceCount;
    } headerV1{};

    in.read(reinterpret_cast<char*>(&headerV1), sizeof(headerV1));
    if (!in.good()) {
        errorMsg = "Raw mesh header read failed";
        return false;
    }
    if (std::strncmp(headerV1.magic, "RMESH", 5) != 0) {
        errorMsg = "Invalid raw mesh header";
        return false;
    }

    if (headerV1.vertexCount == 0 || headerV1.faceCount == 0) {
        errorMsg = "Raw mesh contains no geometry";
        return false;
    }

    if (headerV1.version == kRMeshFormatVersion2) {
        struct HeaderV2 {
            char magic[6];
            uint32_t version;
            uint32_t vertexCount;
            uint32_t faceCount;
            uint32_t materialSlotCount;
            uint32_t edgeCount;
            uint32_t flags;
        } headerV2{};

        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(&headerV2), sizeof(headerV2));
        if (!in.good()) {
            errorMsg = "Raw mesh v2 header read failed";
            return false;
        }

        out.positions.resize(headerV2.vertexCount);
        out.faces.resize(headerV2.faceCount);
        out.materialSlots.resize(headerV2.materialSlotCount);

        in.read(reinterpret_cast<char*>(&out.boundsMin.x), sizeof(float) * 3);
        in.read(reinterpret_cast<char*>(&out.boundsMax.x), sizeof(float) * 3);
        in.read(reinterpret_cast<char*>(out.positions.data()), sizeof(glm::vec3) * out.positions.size());

        if ((headerV2.flags & kRMeshFlagNormals) != 0u) {
            out.normals.resize(out.positions.size());
            in.read(reinterpret_cast<char*>(out.normals.data()), sizeof(glm::vec3) * out.normals.size());
        } else {
            out.normals.assign(out.positions.size(), glm::vec3(0.0f));
        }

        if ((headerV2.flags & kRMeshFlagUVs) != 0u) {
            out.uvs.resize(out.positions.size());
            in.read(reinterpret_cast<char*>(out.uvs.data()), sizeof(glm::vec2) * out.uvs.size());
        } else {
            out.uvs.assign(out.positions.size(), glm::vec2(0.0f));
        }

        in.read(reinterpret_cast<char*>(out.faces.data()), sizeof(glm::u32vec3) * out.faces.size());

        if ((headerV2.flags & kRMeshFlagFaceMaterials) != 0u) {
            out.faceMaterialIndices.resize(out.faces.size(), 0u);
            in.read(reinterpret_cast<char*>(out.faceMaterialIndices.data()), sizeof(uint32_t) * out.faceMaterialIndices.size());
        } else {
            out.faceMaterialIndices.assign(out.faces.size(), 0u);
        }

        if ((headerV2.flags & kRMeshFlagFaceIslands) != 0u) {
            out.faceIslandIds.resize(out.faces.size(), 0u);
            in.read(reinterpret_cast<char*>(out.faceIslandIds.data()), sizeof(uint32_t) * out.faceIslandIds.size());
        }

        if ((headerV2.flags & kRMeshFlagEdges) != 0u) {
            out.edges.resize(headerV2.edgeCount);
            in.read(reinterpret_cast<char*>(out.edges.data()), sizeof(glm::u32vec2) * out.edges.size());
        }

        for (uint32_t i = 0; i < headerV2.materialSlotCount; ++i) {
            uint32_t len = 0u;
            in.read(reinterpret_cast<char*>(&len), sizeof(uint32_t));
            if (!in.good()) {
                errorMsg = "Raw mesh material slot table is truncated";
                return false;
            }
            if (len == 0u) {
                out.materialSlots[i] = "Material_" + std::to_string(i);
                continue;
            }
            std::string name(len, '\0');
            in.read(name.data(), static_cast<std::streamsize>(len));
            if (!in.good()) {
                errorMsg = "Raw mesh material slot string is truncated";
                return false;
            }
            out.materialSlots[i] = std::move(name);
        }
    } else if (headerV1.version == kRMeshFormatVersion1) {
        in.seekg(0, std::ios::end);
        std::streamoff fileSize = in.tellg();
        in.seekg(sizeof(headerV1), std::ios::beg);

        in.read(reinterpret_cast<char*>(&out.boundsMin.x), sizeof(float) * 3);
        in.read(reinterpret_cast<char*>(&out.boundsMax.x), sizeof(float) * 3);

        const std::streamoff payloadSize = fileSize - sizeof(headerV1) - sizeof(float) * 6;
        const std::streamoff positionsSize = static_cast<std::streamoff>(sizeof(glm::vec3)) * headerV1.vertexCount;
        const std::streamoff normalsSize = static_cast<std::streamoff>(sizeof(glm::vec3)) * headerV1.vertexCount;
        const std::streamoff uvsSize = static_cast<std::streamoff>(sizeof(glm::vec2)) * headerV1.vertexCount;
        const std::streamoff facesSize = static_cast<std::streamoff>(sizeof(glm::u32vec3)) * headerV1.faceCount;

        bool hasNormals = false;
        bool hasUVs = false;
        if (payloadSize == positionsSize + normalsSize + uvsSize + facesSize) {
            hasNormals = true;
            hasUVs = true;
        } else if (payloadSize == positionsSize + normalsSize + facesSize) {
            hasNormals = true;
        } else if (payloadSize == positionsSize + uvsSize + facesSize) {
            hasUVs = true;
        } else if (payloadSize == positionsSize + facesSize) {
            // legacy v1 layout without normals/uvs
        } else if (payloadSize < positionsSize + facesSize) {
            errorMsg = "Raw mesh data is truncated";
            return false;
        }

        out.positions.resize(headerV1.vertexCount);
        out.faces.resize(headerV1.faceCount);
        out.materialSlots = { "Default" };
        out.faceMaterialIndices.assign(out.faces.size(), 0u);

        in.read(reinterpret_cast<char*>(out.positions.data()), sizeof(glm::vec3) * out.positions.size());
        if (hasNormals) {
            out.normals.resize(headerV1.vertexCount);
            in.read(reinterpret_cast<char*>(out.normals.data()), sizeof(glm::vec3) * out.normals.size());
        } else {
            out.normals.assign(headerV1.vertexCount, glm::vec3(0.0f));
        }
        if (hasUVs) {
            out.uvs.resize(headerV1.vertexCount);
            in.read(reinterpret_cast<char*>(out.uvs.data()), sizeof(glm::vec2) * out.uvs.size());
        } else {
            out.uvs.assign(headerV1.vertexCount, glm::vec2(0.0f));
        }
        in.read(reinterpret_cast<char*>(out.faces.data()), sizeof(glm::u32vec3) * out.faces.size());
    } else {
        errorMsg = "Unsupported raw mesh version";
        return false;
    }

    if (!in.good()) {
        errorMsg = "Unexpected EOF while reading raw mesh";
        return false;
    }

    for (const auto& face : out.faces) {
        if (face.x >= out.positions.size() ||
            face.y >= out.positions.size() ||
            face.z >= out.positions.size()) {
            errorMsg = "Raw mesh contains invalid face indices";
            return false;
        }
    }

    sanitizeRawMeshAsset(out);
    return true;
}

bool ModelLoader::saveRawMesh(const RawMeshAsset& asset, const std::string& filepath, std::string& errorMsg) {
    if (asset.positions.empty() || asset.faces.empty()) {
        errorMsg = "Raw mesh is empty";
        return false;
    }

    if (IsMMeshPath(filepath)) {
        using namespace Modularity::Render25D;
        MMeshAsset mmeshAsset = BuildMMeshFromRawMesh(asset);

        MMeshLoader mmeshLoader;
        MMeshAsset previous;
        std::string previousError;
        if (fs::exists(filepath) && mmeshLoader.loadAsset(filepath, previous, previousError)) {
            for (size_t i = 0; i < mmeshAsset.materials.size(); ++i) {
                const MMeshMaterialRef* match = nullptr;
                for (const MMeshMaterialRef& old : previous.materials) {
                    if (old.name == mmeshAsset.materials[i].name) {
                        match = &old;
                        break;
                    }
                }
                if (match == nullptr && i < previous.materials.size()) {
                    match = &previous.materials[i];
                }
                if (match != nullptr) {
                    mmeshAsset.materials[i].albedoTexturePath = match->albedoTexturePath;
                    mmeshAsset.materials[i].presentation = match->presentation;
                }
            }
            mmeshAsset.metadataHooks = previous.metadataHooks;
        }

        return mmeshLoader.saveAsset(mmeshAsset, filepath, errorMsg);
    }

    fs::path outPath(filepath);
    if (outPath.extension().empty()) {
        outPath.replace_extension(".rmesh");
    }

    RawMeshAsset normalized = asset;
    sanitizeRawMeshAsset(normalized);

    struct HeaderV2 {
        char magic[6] = { 'R','M','E','S','H','\0' };
        uint32_t version = kRMeshFormatVersion2;
        uint32_t vertexCount = 0;
        uint32_t faceCount = 0;
        uint32_t materialSlotCount = 0;
        uint32_t edgeCount = 0;
        uint32_t flags = 0;
    } header;

    header.vertexCount = static_cast<uint32_t>(normalized.positions.size());
    header.faceCount = static_cast<uint32_t>(normalized.faces.size());
    header.materialSlotCount = static_cast<uint32_t>(normalized.materialSlots.size());
    header.edgeCount = static_cast<uint32_t>(normalized.edges.size());
    header.flags = kRMeshFlagNormals | kRMeshFlagUVs | kRMeshFlagFaceMaterials |
                   kRMeshFlagFaceIslands | kRMeshFlagEdges;

    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        errorMsg = "Unable to open file for writing: " + outPath.string();
        return false;
    }

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(&normalized.boundsMin.x), sizeof(float) * 3);
    out.write(reinterpret_cast<const char*>(&normalized.boundsMax.x), sizeof(float) * 3);
    out.write(reinterpret_cast<const char*>(normalized.positions.data()), sizeof(glm::vec3) * normalized.positions.size());
    out.write(reinterpret_cast<const char*>(normalized.normals.data()), sizeof(glm::vec3) * normalized.normals.size());
    out.write(reinterpret_cast<const char*>(normalized.uvs.data()), sizeof(glm::vec2) * normalized.uvs.size());
    out.write(reinterpret_cast<const char*>(normalized.faces.data()), sizeof(glm::u32vec3) * normalized.faces.size());
    out.write(reinterpret_cast<const char*>(normalized.faceMaterialIndices.data()), sizeof(uint32_t) * normalized.faceMaterialIndices.size());
    out.write(reinterpret_cast<const char*>(normalized.faceIslandIds.data()), sizeof(uint32_t) * normalized.faceIslandIds.size());
    out.write(reinterpret_cast<const char*>(normalized.edges.data()), sizeof(glm::u32vec2) * normalized.edges.size());

    for (const auto& slotName : normalized.materialSlots) {
        uint32_t len = static_cast<uint32_t>(slotName.size());
        out.write(reinterpret_cast<const char*>(&len), sizeof(uint32_t));
        if (len > 0u) {
            out.write(slotName.data(), static_cast<std::streamsize>(len));
        }
    }

    if (!out.good()) {
        errorMsg = "Failed while writing raw mesh file";
        return false;
    }

    cachedScenes.erase(outPath.string());

    return true;
}

bool ModelLoader::updateRawMesh(int meshIndex, const RawMeshAsset& asset, std::string& errorMsg) {
    if (meshIndex < 0 || meshIndex >= static_cast<int>(loadedMeshes.size())) {
        errorMsg = "Invalid mesh index";
        return false;
    }
    if (asset.positions.empty() || asset.faces.empty()) {
        errorMsg = "Raw mesh is empty";
        return false;
    }

    RawMeshAsset normalized = asset;
    sanitizeRawMeshAsset(normalized);

    RawGpuBuildResult gpu;
    if (!buildRawGpuData(normalized, gpu, errorMsg)) {
        return false;
    }

    OBJLoader::LoadedMesh& loaded = loadedMeshes[meshIndex];
    loaded.mesh = std::make_unique<Mesh>(gpu.fullVertices.data(), gpu.fullVertices.size() * sizeof(float));
    loaded.vertexCount = static_cast<int>(gpu.fullVertices.size() / 8);
    loaded.faceCount = static_cast<int>(normalized.faces.size());
    loaded.hasNormals = normalized.hasNormals;
    loaded.hasTexCoords = normalized.hasUVs;
    loaded.boundsMin = normalized.boundsMin;
    loaded.boundsMax = normalized.boundsMax;
    loaded.triangleVertices = std::move(gpu.triPositions);
    loaded.positions = normalized.positions;
    loaded.triangleIndices = std::move(gpu.triIndices);
    loaded.materialSlots = normalized.materialSlots;
    loaded.subMeshes.clear();
    loaded.subMeshes.reserve(gpu.submeshVertices.size());
    for (size_t matIdx = 0; matIdx < gpu.submeshVertices.size(); ++matIdx) {
        if (gpu.submeshVertices[matIdx].empty()) continue;
        OBJLoader::LoadedMesh::SubMesh sub;
        sub.materialIndex = static_cast<int>(matIdx);
        sub.faceCount = gpu.submeshFaceCounts[matIdx];
        sub.vertexCount = static_cast<int>(gpu.submeshVertices[matIdx].size() / 8);
        sub.mesh = std::make_unique<Mesh>(
            gpu.submeshVertices[matIdx].data(),
            gpu.submeshVertices[matIdx].size() * sizeof(float));
        loaded.subMeshes.push_back(std::move(sub));
    }

    cachedScenes.erase(loaded.path);

    return true;
}

int ModelLoader::addRawMesh(const RawMeshAsset& asset, const std::string& sourcePath,
                            const std::string& name, std::string& errorMsg) {
    if (asset.positions.empty() || asset.faces.empty()) {
        errorMsg = "Raw mesh is empty";
        return -1;
    }

    RawMeshAsset normalized = asset;
    sanitizeRawMeshAsset(normalized);

    RawGpuBuildResult gpu;
    if (!buildRawGpuData(normalized, gpu, errorMsg)) {
        return -1;
    }

    OBJLoader::LoadedMesh loaded;
    loaded.path = sourcePath;
    loaded.name = name.empty() ? "StaticBatch" : name;
    loaded.mesh = std::make_unique<Mesh>(gpu.fullVertices.data(), gpu.fullVertices.size() * sizeof(float));
    loaded.vertexCount = static_cast<int>(gpu.fullVertices.size() / 8);
    loaded.faceCount = static_cast<int>(normalized.faces.size());
    loaded.hasNormals = normalized.hasNormals;
    loaded.hasTexCoords = normalized.hasUVs;
    loaded.boundsMin = normalized.boundsMin;
    loaded.boundsMax = normalized.boundsMax;
    loaded.triangleVertices = std::move(gpu.triPositions);
    loaded.positions = normalized.positions;
    loaded.triangleIndices = std::move(gpu.triIndices);
    loaded.materialSlots = normalized.materialSlots;
    loaded.subMeshes.clear();
    loaded.subMeshes.reserve(gpu.submeshVertices.size());
    for (size_t matIdx = 0; matIdx < gpu.submeshVertices.size(); ++matIdx) {
        if (gpu.submeshVertices[matIdx].empty()) continue;
        OBJLoader::LoadedMesh::SubMesh sub;
        sub.materialIndex = static_cast<int>(matIdx);
        sub.faceCount = gpu.submeshFaceCounts[matIdx];
        sub.vertexCount = static_cast<int>(gpu.submeshVertices[matIdx].size() / 8);
        sub.mesh = std::make_unique<Mesh>(
            gpu.submeshVertices[matIdx].data(),
            gpu.submeshVertices[matIdx].size() * sizeof(float));
        loaded.subMeshes.push_back(std::move(sub));
    }

    int newIndex = static_cast<int>(loadedMeshes.size());
    loadedMeshes.push_back(std::move(loaded));
    return newIndex;
}

static glm::mat4 aiToGlm(const aiMatrix4x4& m) {
    return glm::mat4(
        m.a1, m.b1, m.c1, m.d1,
        m.a2, m.b2, m.c2, m.d2,
        m.a3, m.b3, m.c3, m.d3,
        m.a4, m.b4, m.c4, m.d4
    );
}

static glm::vec3 extractEulerXYZDegrees(const glm::quat& q) {
    const glm::mat3 m = glm::mat3_cast(glm::normalize(q));
    float t1 = std::atan2(m[2][1], m[2][2]);
    float c2 = std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0]);
    float t2 = std::atan2(-m[2][0], c2);
    float s1 = std::sin(t1);
    float c1 = std::cos(t1);
    float t3 = std::atan2(s1 * m[0][2] - c1 * m[0][1], c1 * m[1][1] - s1 * m[1][2]);
    return glm::degrees(glm::vec3(-t1, -t2, -t3));
}

static glm::vec3 quatToEulerDegrees(const aiQuaternion& q) {
    glm::quat gq(q.w, q.x, q.y, q.z);
    return extractEulerXYZDegrees(gq);
}

static std::string buildNodePathFromRoot(const aiNode* node, const aiNode* root) {
    if (!node || node == root) return {};

    std::vector<std::string> segments;
    const aiNode* current = node;
    while (current && current != root) {
        std::string name = current->mName.C_Str();
        if (!name.empty()) {
            segments.push_back(std::move(name));
        }
        current = current->mParent;
    }
    if (current != root) {
        return {};
    }

    std::string path;
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
        if (!path.empty()) path += '/';
        path += *it;
    }
    return path;
}

static bool buildSceneMeshes(const std::string& filepath, const aiScene* scene,
                             std::vector<OBJLoader::LoadedMesh>& loadedMeshes,
                             ModelSceneData& out, std::string& errorMsg) {
    out.meshIndices.assign(scene->mNumMeshes, -1);
    out.meshMaterialIndices.assign(scene->mNumMeshes, -1);

    out.materials.clear();
    out.materials.reserve(scene->mNumMaterials);
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
        aiMaterial* mat = scene->mMaterials[i];
        ModelMaterialInfo info;
        info.name = mat->GetName().C_Str();
        if (info.name.empty()) {
            info.name = "Material_" + std::to_string(i);
        }

        aiColor3D diffuse(1.0f, 1.0f, 1.0f);
        if (AI_SUCCESS == mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse)) {
            info.props.color = glm::vec3(diffuse.r, diffuse.g, diffuse.b);
        }

        aiColor3D specular(0.0f, 0.0f, 0.0f);
        if (AI_SUCCESS == mat->Get(AI_MATKEY_COLOR_SPECULAR, specular)) {
            float avg = (specular.r + specular.g + specular.b) / 3.0f;
            info.props.specularStrength = avg;
        }

        float shininess = info.props.shininess;
        if (AI_SUCCESS == mat->Get(AI_MATKEY_SHININESS, shininess)) {
            info.props.shininess = shininess;
        }

        aiString tex;
        if (AI_SUCCESS == mat->GetTexture(aiTextureType_DIFFUSE, 0, &tex)) {
            info.albedoPath = tex.C_Str();
        }
        if (AI_SUCCESS == mat->GetTexture(aiTextureType_NORMALS, 0, &tex)) {
            info.normalPath = tex.C_Str();
        } else if (AI_SUCCESS == mat->GetTexture(aiTextureType_HEIGHT, 0, &tex)) {
            info.normalPath = tex.C_Str();
        }

        if (!info.albedoPath.empty()) {
            info.props.textureMix = 1.0f;
        }

        out.materials.push_back(info);
    }

    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        aiMesh* mesh = scene->mMeshes[i];
        if (!mesh || mesh->mNumVertices == 0 || mesh->mNumFaces == 0) {
            continue;
        }

        std::vector<float> vertices;
        struct BoneVertex {
            int ids[4];
            float weights[4];
        };
        std::vector<BoneVertex> boneVertices;
        std::vector<glm::ivec4> vertexBoneIds(mesh->mNumVertices, glm::ivec4(0));
        std::vector<glm::vec4> vertexBoneWeights(mesh->mNumVertices, glm::vec4(0.0f));
        std::vector<glm::vec3> triPositions;
        std::vector<glm::vec3> positions;
        std::vector<uint32_t> triangleIndices;
        vertices.reserve(mesh->mNumFaces * 3 * 8);
        boneVertices.reserve(mesh->mNumFaces * 3);
        triPositions.reserve(mesh->mNumFaces * 3);
        positions.reserve(mesh->mNumVertices);
        triangleIndices.reserve(mesh->mNumFaces * 3);

        glm::vec3 boundsMin(FLT_MAX);
        glm::vec3 boundsMax(-FLT_MAX);

        std::vector<std::string> boneNames;
        std::vector<std::string> boneNodePaths;
        std::vector<glm::mat4> inverseBindMatrices;
        if (mesh->mNumBones > 0) {
            boneNames.reserve(mesh->mNumBones);
            boneNodePaths.reserve(mesh->mNumBones);
            inverseBindMatrices.reserve(mesh->mNumBones);
            for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
                aiBone* bone = mesh->mBones[b];
                int boneIndex = static_cast<int>(boneNames.size());
                boneNames.push_back(bone->mName.C_Str());
#ifndef ASSIMP_BUILD_NO_ARMATUREPOPULATE_PROCESS
                boneNodePaths.push_back(buildNodePathFromRoot(bone->mNode, scene->mRootNode));
#else
                boneNodePaths.emplace_back();
#endif
                inverseBindMatrices.push_back(aiToGlm(bone->mOffsetMatrix));

                for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
                    unsigned int vId = bone->mWeights[w].mVertexId;
                    float weight = bone->mWeights[w].mWeight;
                    if (vId >= vertexBoneWeights.size()) continue;

                    glm::vec4& weights = vertexBoneWeights[vId];
                    glm::ivec4& ids = vertexBoneIds[vId];
                    int replaceIndex = -1;
                    float minWeight = weight;
                    for (int k = 0; k < 4; ++k) {
                        if (weights[k] == 0.0f) {
                            replaceIndex = k;
                            break;
                        }
                        if (weights[k] < minWeight) {
                            minWeight = weights[k];
                            replaceIndex = k;
                        }
                    }
                    if (replaceIndex >= 0) {
                        weights[replaceIndex] = weight;
                        ids[replaceIndex] = boneIndex;
                    }
                }
            }
        }

        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            glm::vec3 pos(mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z);
            positions.push_back(pos);
            boundsMin.x = std::min(boundsMin.x, pos.x);
            boundsMin.y = std::min(boundsMin.y, pos.y);
            boundsMin.z = std::min(boundsMin.z, pos.z);
            boundsMax.x = std::max(boundsMax.x, pos.x);
            boundsMax.y = std::max(boundsMax.y, pos.y);
            boundsMax.z = std::max(boundsMax.z, pos.z);
        }

        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices != 3) continue;

            triangleIndices.push_back(static_cast<uint32_t>(face.mIndices[0]));
            triangleIndices.push_back(static_cast<uint32_t>(face.mIndices[1]));
            triangleIndices.push_back(static_cast<uint32_t>(face.mIndices[2]));

            for (unsigned int j = 0; j < 3; ++j) {
                unsigned int index = face.mIndices[j];
                glm::vec3 pos(mesh->mVertices[index].x,
                              mesh->mVertices[index].y,
                              mesh->mVertices[index].z);
                vertices.push_back(pos.x);
                vertices.push_back(pos.y);
                vertices.push_back(pos.z);
                triPositions.push_back(pos);

                if (mesh->mNormals) {
                    glm::vec3 n(mesh->mNormals[index].x,
                                mesh->mNormals[index].y,
                                mesh->mNormals[index].z);
                    vertices.push_back(n.x);
                    vertices.push_back(n.y);
                    vertices.push_back(n.z);
                } else {
                    vertices.push_back(0.0f);
                    vertices.push_back(1.0f);
                    vertices.push_back(0.0f);
                }

                if (mesh->mTextureCoords[0]) {
                    vertices.push_back(mesh->mTextureCoords[0][index].x);
                    vertices.push_back(mesh->mTextureCoords[0][index].y);
                } else {
                    vertices.push_back(0.0f);
                    vertices.push_back(0.0f);
                }

                BoneVertex bv{};
                glm::ivec4 ids = vertexBoneIds[index];
                glm::vec4 weights = vertexBoneWeights[index];
                float weightSum = weights.x + weights.y + weights.z + weights.w;
                if (weightSum > 0.0f) {
                    weights /= weightSum;
                }
                bv.ids[0] = ids.x;
                bv.ids[1] = ids.y;
                bv.ids[2] = ids.z;
                bv.ids[3] = ids.w;
                bv.weights[0] = weights.x;
                bv.weights[1] = weights.y;
                bv.weights[2] = weights.z;
                bv.weights[3] = weights.w;
                boneVertices.push_back(bv);
            }
        }

        if (vertices.empty()) {
            continue;
        }

        OBJLoader::LoadedMesh loaded;
        loaded.path = filepath;
        loaded.name = mesh->mName.C_Str();
        if (loaded.name.empty()) {
            loaded.name = fs::path(filepath).stem().string() + "_mesh" + std::to_string(i);
        }
        bool isSkinned = mesh->mNumBones > 0 && boneVertices.size() == vertices.size() / 8;
        if (isSkinned) {
            loaded.mesh = std::make_unique<Mesh>(vertices.data(), vertices.size() * sizeof(float), true,
                                                 boneVertices.data(), boneVertices.size() * sizeof(BoneVertex));
        } else {
            loaded.mesh = std::make_unique<Mesh>(vertices.data(), vertices.size() * sizeof(float));
        }
        loaded.vertexCount = static_cast<int>(vertices.size() / 8);
        loaded.faceCount = static_cast<int>(mesh->mNumFaces);
        loaded.hasNormals = mesh->mNormals != nullptr;
        loaded.hasTexCoords = mesh->mTextureCoords[0] != nullptr;
        loaded.boundsMin = boundsMin;
        loaded.boundsMax = boundsMax;
        loaded.triangleVertices = std::move(triPositions);
        loaded.positions = std::move(positions);
        loaded.triangleIndices = std::move(triangleIndices);
        loaded.isSkinned = isSkinned;
        loaded.boneNames = std::move(boneNames);
        loaded.boneNodePaths = std::move(boneNodePaths);
        loaded.inverseBindMatrices = std::move(inverseBindMatrices);
        loaded.baseVertices = vertices;
        if (isSkinned) {
            loaded.boneIds.reserve(boneVertices.size());
            loaded.boneWeights.reserve(boneVertices.size());
            for (const auto& bv : boneVertices) {
                loaded.boneIds.emplace_back(bv.ids[0], bv.ids[1], bv.ids[2], bv.ids[3]);
                loaded.boneWeights.emplace_back(bv.weights[0], bv.weights[1], bv.weights[2], bv.weights[3]);
            }
        }

        out.meshMaterialIndices[i] = static_cast<size_t>(mesh->mMaterialIndex) < out.materials.size()
            ? static_cast<int>(mesh->mMaterialIndex)
            : -1;

        out.meshIndices[i] = static_cast<int>(loadedMeshes.size());
        loadedMeshes.push_back(std::move(loaded));
    }

    bool anyMesh = false;
    for (int idx : out.meshIndices) {
        if (idx >= 0) { anyMesh = true; break; }
    }
    if (!anyMesh) {
        errorMsg = "No meshes found in model file";
        return false;
    }

    return true;
}

static void buildSceneNodes(const aiScene* scene,
                            const std::vector<int>& meshIndices,
                            ModelSceneData& out) {
    std::unordered_set<std::string> boneNames;
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        aiMesh* mesh = scene->mMeshes[i];
        for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
            boneNames.insert(mesh->mBones[b]->mName.C_Str());
        }
    }

    std::function<void(aiNode*, int)> walk = [&](aiNode* node, int parentIndex) {
        ModelNodeInfo info;
        info.name = node->mName.C_Str();
        if (info.name.empty()) {
            info.name = "Node_" + std::to_string(out.nodes.size());
        }
        info.parentIndex = parentIndex;
        info.isBone = boneNames.find(info.name) != boneNames.end();

        aiVector3D scaling(1.0f, 1.0f, 1.0f);
        aiVector3D position(0.0f, 0.0f, 0.0f);
        aiQuaternion rotation;
        node->mTransformation.Decompose(scaling, rotation, position);

        info.localPosition = glm::vec3(position.x, position.y, position.z);
        info.localScale = glm::vec3(scaling.x, scaling.y, scaling.z);
        info.localRotation = quatToEulerDegrees(rotation);

        for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
            unsigned int meshIndex = node->mMeshes[i];
            if (meshIndex < meshIndices.size()) {
                info.meshIndices.push_back(static_cast<int>(meshIndex));
            }
        }

        int thisIndex = static_cast<int>(out.nodes.size());
        out.nodes.push_back(info);

        for (unsigned int c = 0; c < node->mNumChildren; ++c) {
            walk(node->mChildren[c], thisIndex);
        }
    };

    out.nodes.clear();
    if (scene->mRootNode) {
        walk(scene->mRootNode, -1);
    }
}

static void collectRawMeshData(aiNode* node, const aiScene* scene, const aiMatrix4x4& parentTransform, RawMeshAsset& out) {
    aiMatrix4x4 current = parentTransform * node->mTransformation;
    glm::mat4 gTransform = aiToGlm(current);
    glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(gTransform)));

    for (unsigned int i = 0; i < node->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        size_t baseIndex = out.positions.size();

        // Vertices
        for (unsigned int v = 0; v < mesh->mNumVertices; v++) {
            glm::vec3 pos(mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z);
            glm::vec4 transformed = gTransform * glm::vec4(pos, 1.0f);
            glm::vec3 finalPos = glm::vec3(transformed) / (transformed.w == 0.0f ? 1.0f : transformed.w);
            out.positions.push_back(finalPos);

            out.boundsMin.x = std::min(out.boundsMin.x, finalPos.x);
            out.boundsMin.y = std::min(out.boundsMin.y, finalPos.y);
            out.boundsMin.z = std::min(out.boundsMin.z, finalPos.z);
            out.boundsMax.x = std::max(out.boundsMax.x, finalPos.x);
            out.boundsMax.y = std::max(out.boundsMax.y, finalPos.y);
            out.boundsMax.z = std::max(out.boundsMax.z, finalPos.z);

            glm::vec3 normal(0.0f);
            if (mesh->mNormals) {
                normal = glm::normalize(normalMat * glm::vec3(mesh->mNormals[v].x, mesh->mNormals[v].y, mesh->mNormals[v].z));
            }
            out.normals.push_back(normal);

            glm::vec2 uv(0.0f);
            if (mesh->mTextureCoords[0]) {
                uv.x = mesh->mTextureCoords[0][v].x;
                uv.y = mesh->mTextureCoords[0][v].y;
            }
            out.uvs.push_back(uv);
        }

        // Faces (triangles)
        uint32_t meshMaterialIndex = mesh->mMaterialIndex;
        if (out.materialSlots.size() <= meshMaterialIndex) {
            out.materialSlots.resize(static_cast<size_t>(meshMaterialIndex) + 1u);
        }
        if (meshMaterialIndex < scene->mNumMaterials && scene->mMaterials[meshMaterialIndex]) {
            std::string matName = scene->mMaterials[meshMaterialIndex]->GetName().C_Str();
            if (matName.empty()) {
                matName = "Material_" + std::to_string(meshMaterialIndex);
            }
            out.materialSlots[meshMaterialIndex] = matName;
        } else if (out.materialSlots[meshMaterialIndex].empty()) {
            out.materialSlots[meshMaterialIndex] = "Material_" + std::to_string(meshMaterialIndex);
        }

        for (unsigned int f = 0; f < mesh->mNumFaces; f++) {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices != 3) continue;
            glm::u32vec3 tri(
                static_cast<uint32_t>(baseIndex + face.mIndices[0]),
                static_cast<uint32_t>(baseIndex + face.mIndices[1]),
                static_cast<uint32_t>(baseIndex + face.mIndices[2])
            );
            out.faces.push_back(tri);
            out.faceMaterialIndices.push_back(meshMaterialIndex);
        }
    }

    for (unsigned int c = 0; c < node->mNumChildren; c++) {
        collectRawMeshData(node->mChildren[c], scene, current, out);
    }
}

bool ModelLoader::buildRawMeshFromScene(const std::string& filepath, RawMeshAsset& out, std::string& errorMsg,
                                        glm::vec3* outRootPos, glm::vec3* outRootRot, glm::vec3* outRootScale) {
    out = RawMeshAsset();

    fs::path inPath(filepath);
    if (!fs::exists(inPath)) {
        errorMsg = "File not found: " + filepath;
        return false;
    }
    if (!isSupported(filepath)) {
        errorMsg = "Unsupported file format for raw mesh build";
        return false;
    }

    Assimp::Importer localImporter;
    unsigned int importFlags =
        aiProcess_Triangulate |
        aiProcess_GenSmoothNormals;
        //aiProcess_FlipUVs;

    ScopedImportedTempFile tempFile;
    std::string importError;
    const aiScene* scene = ReadSceneWithBlendFallback(localImporter, filepath, importFlags, tempFile, importError);
    if (!IsUsableScene(scene)) {
        errorMsg = importError.empty()
            ? "Assimp error: " + std::string(localImporter.GetErrorString())
            : importError;
        return false;
    }

    aiMatrix4x4 parent;
    parent = aiMatrix4x4();
    if (scene->mRootNode && (outRootPos || outRootRot || outRootScale)) {
        aiVector3D scaling(1.0f, 1.0f, 1.0f);
        aiVector3D position(0.0f, 0.0f, 0.0f);
        aiQuaternion rotation;
        scene->mRootNode->mTransformation.Decompose(scaling, rotation, position);
        if (outRootPos) *outRootPos = glm::vec3(position.x, position.y, position.z);
        if (outRootScale) *outRootScale = glm::vec3(scaling.x, scaling.y, scaling.z);
        if (outRootRot) *outRootRot = quatToEulerDegrees(rotation);

        aiMatrix4x4 rootTransform = scene->mRootNode->mTransformation;
        rootTransform.Inverse();
        parent = rootTransform;
    }

    collectRawMeshData(scene->mRootNode, scene, parent, out);

    if (out.positions.empty() || out.faces.empty()) {
        errorMsg = "No geometry found to build raw mesh";
        return false;
    }

    out.hasNormals = false;
    for (const auto& n : out.normals) {
        if (glm::length(n) > 1e-4f) { out.hasNormals = true; break; }
    }

    out.hasUVs = false;
    for (const auto& uv : out.uvs) {
        if (std::abs(uv.x) > 1e-6f || std::abs(uv.y) > 1e-6f) { out.hasUVs = true; break; }
    }

    if (!out.hasNormals) {
        out.normals.assign(out.positions.size(), glm::vec3(0.0f));
        std::vector<glm::vec3> accum(out.positions.size(), glm::vec3(0.0f));
        for (const auto& face : out.faces) {
            const glm::vec3& a = out.positions[face.x];
            const glm::vec3& b = out.positions[face.y];
            const glm::vec3& c = out.positions[face.z];
            glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
            accum[face.x] += n;
            accum[face.y] += n;
            accum[face.z] += n;
        }
        for (size_t i = 0; i < accum.size(); i++) {
            if (glm::length(accum[i]) > 1e-6f) {
                out.normals[i] = glm::normalize(accum[i]);
            }
        }
        out.hasNormals = true;
    }

    sanitizeRawMeshAsset(out);

    return true;
}

void ModelLoader::processNode(aiNode* node, const aiScene* scene, const aiMatrix4x4& parentTransform,
                              std::vector<float>& vertices, std::vector<glm::vec3>& triPositions,
                              std::vector<glm::vec3>& positions, std::vector<uint32_t>& indices,
                              glm::vec3& boundsMin, glm::vec3& boundsMax) {
    aiMatrix4x4 currentTransform = parentTransform * node->mTransformation;
    // Process all meshes in this node
    for (unsigned int i = 0; i < node->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        processMesh(mesh, currentTransform, vertices, triPositions, positions, indices, boundsMin, boundsMax);
    }

    // Process children nodes
    for (unsigned int i = 0; i < node->mNumChildren; i++) {
        processNode(node->mChildren[i], scene, currentTransform, vertices, triPositions, positions, indices, boundsMin, boundsMax);
    }
}

void ModelLoader::processMesh(aiMesh* mesh, const aiMatrix4x4& transform,
                              std::vector<float>& vertices, std::vector<glm::vec3>& triPositions,
                              std::vector<glm::vec3>& positions, std::vector<uint32_t>& indices,
                              glm::vec3& boundsMin, glm::vec3& boundsMax) {
    glm::mat4 gTransform = aiToGlm(transform);
    glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(gTransform)));

    size_t baseIndex = positions.size();
    positions.reserve(baseIndex + mesh->mNumVertices);
    for (unsigned int v = 0; v < mesh->mNumVertices; v++) {
        glm::vec3 pos(mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z);
        glm::vec4 transformed = gTransform * glm::vec4(pos, 1.0f);
        glm::vec3 finalPos = glm::vec3(transformed) / (transformed.w == 0.0f ? 1.0f : transformed.w);
        positions.push_back(finalPos);

        boundsMin.x = std::min(boundsMin.x, finalPos.x);
        boundsMin.y = std::min(boundsMin.y, finalPos.y);
        boundsMin.z = std::min(boundsMin.z, finalPos.z);
        boundsMax.x = std::max(boundsMax.x, finalPos.x);
        boundsMax.y = std::max(boundsMax.y, finalPos.y);
        boundsMax.z = std::max(boundsMax.z, finalPos.z);
    }

    // Process each face
    for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
        aiFace face = mesh->mFaces[i];
        if (face.mNumIndices == 3) {
            indices.push_back(static_cast<uint32_t>(baseIndex + face.mIndices[0]));
            indices.push_back(static_cast<uint32_t>(baseIndex + face.mIndices[1]));
            indices.push_back(static_cast<uint32_t>(baseIndex + face.mIndices[2]));
        }

        // Process each vertex of the face
        for (unsigned int j = 0; j < face.mNumIndices; j++) {
            unsigned int index = face.mIndices[j];

            glm::vec3 pos(mesh->mVertices[index].x,
                          mesh->mVertices[index].y,
                          mesh->mVertices[index].z);
            glm::vec4 transformed = gTransform * glm::vec4(pos, 1.0f);
            glm::vec3 finalPos = glm::vec3(transformed) / (transformed.w == 0.0f ? 1.0f : transformed.w);

            vertices.push_back(finalPos.x);
            vertices.push_back(finalPos.y);
            vertices.push_back(finalPos.z);

            triPositions.push_back(finalPos);

            // Normal
            if (mesh->mNormals) {
                glm::vec3 n(mesh->mNormals[index].x,
                            mesh->mNormals[index].y,
                            mesh->mNormals[index].z);
                n = glm::normalize(normalMat * n);
                vertices.push_back(n.x);
                vertices.push_back(n.y);
                vertices.push_back(n.z);
            } else {
                vertices.push_back(0.0f);
                vertices.push_back(1.0f);
                vertices.push_back(0.0f);
            }

            // Texture coordinates
            if (mesh->mTextureCoords[0]) {
                vertices.push_back(mesh->mTextureCoords[0][index].x);
                vertices.push_back(mesh->mTextureCoords[0][index].y);
            } else {
                vertices.push_back(0.0f);
                vertices.push_back(0.0f);
            }
        }
    }
}

#else

std::vector<ModelFormat> ModelLoader::getSupportedFormats() {
    return {};
}

bool ModelLoader::isSupported(const std::string&) const {
    return false;
}

ModelLoadResult ModelLoader::loadModel(const std::string&) {
    ModelLoadResult result;
    result.errorMessage = "Model loading is not available in this runtime build.";
    return result;
}

bool ModelLoader::loadModelScene(const std::string&, ModelSceneData&, std::string& errorMsg) {
    errorMsg = "Model loading is not available in this runtime build.";
    return false;
}

const ModelSceneData* ModelLoader::loadModelSceneCached(const std::string&, std::string& errorMsg) {
    errorMsg = "Model loading is not available in this runtime build.";
    return nullptr;
}

bool ModelLoader::exportRawMesh(const std::string&, const std::string&, std::string& errorMsg) {
    errorMsg = "Model import is not available in this runtime build.";
    return false;
}

bool ModelLoader::loadRawMesh(const std::string&, RawMeshAsset&, std::string& errorMsg) {
    errorMsg = "Raw mesh loading is not available in this runtime build.";
    return false;
}

bool ModelLoader::saveRawMesh(const RawMeshAsset&, const std::string&, std::string& errorMsg) {
    errorMsg = "Raw mesh saving is not available in this runtime build.";
    return false;
}

bool ModelLoader::updateRawMesh(int, const RawMeshAsset&, std::string& errorMsg) {
    errorMsg = "Raw mesh editing is not available in this runtime build.";
    return false;
}

int ModelLoader::addRawMesh(const RawMeshAsset&, const std::string&, const std::string&, std::string& errorMsg) {
    errorMsg = "Raw mesh loading is not available in this runtime build.";
    return -1;
}

bool ModelLoader::buildRawMeshFromScene(const std::string&, RawMeshAsset&, std::string& errorMsg,
                                        glm::vec3*, glm::vec3*, glm::vec3*) {
    errorMsg = "Model import is not available in this runtime build.";
    return false;
}

#endif

Mesh* ModelLoader::getMesh(int index) {
    if (index < 0 || index >= static_cast<int>(loadedMeshes.size())) {
        return nullptr;
    }
    return loadedMeshes[index].mesh.get();
}

const OBJLoader::LoadedMesh* ModelLoader::getMeshInfo(int index) const {
    if (index < 0 || index >= static_cast<int>(loadedMeshes.size())) {
        return nullptr;
    }
    return &loadedMeshes[index];
}

const std::vector<OBJLoader::LoadedMesh>& ModelLoader::getAllMeshes() const {
    return loadedMeshes;
}

void ModelLoader::clear() {
    loadedMeshes.clear();
    cachedScenes.clear();
}

size_t ModelLoader::getMeshCount() const {
    return loadedMeshes.size();
}
