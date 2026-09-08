#pragma once
#include "Common.h"
#include "Rendering.h"
#include <cstdint>
#include <unordered_map>
#ifndef MODULARITY_ENABLE_ASSIMP
#define MODULARITY_ENABLE_ASSIMP 1
#endif
#if MODULARITY_ENABLE_ASSIMP
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#endif

// Supported file extensions for model import
struct ModelFormat {
    std::string extension;
    std::string description;
    bool supportsAnimation;
};

// Model loading result with detailed info
struct ModelLoadResult {
    bool success = false;
    int meshIndex = -1;
    std::vector<int> meshIndices;
    std::string errorMessage;
    int vertexCount = 0;
    int faceCount = 0;
    int meshCount = 0;
    bool hasNormals = false;
    bool hasTexCoords = false;
    bool hasTangents = false;
    std::vector<std::string> meshNames;
};

// Raw mesh asset for editable geometry (.rmesh)
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

struct ModelMaterialInfo {
    std::string name;
    MaterialProperties props;
    std::string albedoPath;
    std::string normalPath;
};

struct ModelNodeInfo {
    std::string name;
    int parentIndex = -1;
    std::vector<int> meshIndices;
    glm::vec3 localPosition = glm::vec3(0.0f);
    glm::vec3 localRotation = glm::vec3(0.0f);
    glm::vec3 localScale = glm::vec3(1.0f);
    bool isBone = false;
};

struct ModelSceneData {
    std::vector<ModelNodeInfo> nodes;
    std::vector<ModelMaterialInfo> materials;
    std::vector<int> meshIndices;
    std::vector<int> meshMaterialIndices;
    struct AnimVecKey {
        float time = 0.0f;
        glm::vec3 value = glm::vec3(0.0f);
    };
    struct AnimQuatKey {
        float time = 0.0f;
        glm::quat value = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    };
    struct AnimChannel {
        std::string nodeName;
        std::vector<AnimVecKey> positions;
        std::vector<AnimQuatKey> rotations;
        std::vector<AnimVecKey> scales;
    };
    struct AnimationClip {
        std::string name;
        double duration = 0.0;
        double ticksPerSecond = 0.0;
        std::vector<AnimChannel> channels;
    };
    std::vector<AnimationClip> animations;
};

class ModelLoader {
public:
    // Singleton access
    static ModelLoader& getInstance();

    // Load a model file (FBX, OBJ, GLTF, etc.)
    ModelLoadResult loadModel(const std::string& filepath);

    // Load a model scene with node hierarchy and per-mesh materials
    bool loadModelScene(const std::string& filepath, ModelSceneData& out, std::string& errorMsg);

    const ModelSceneData* loadModelSceneCached(const std::string& filepath, std::string& errorMsg);

    // Get mesh by index
    Mesh* getMesh(int index);

    // Get mesh info
    const OBJLoader::LoadedMesh* getMeshInfo(int index) const;

    // Get all loaded meshes
    const std::vector<OBJLoader::LoadedMesh>& getAllMeshes() const;

    // Check if file extension is supported
    bool isSupported(const std::string& filepath) const;

    // Export a model file into a raw editable mesh asset (.rmesh)
    bool exportRawMesh(const std::string& inputFile, const std::string& outputFile, std::string& errorMsg);

    // Load a raw mesh asset from disk
    bool loadRawMesh(const std::string& filepath, RawMeshAsset& out, std::string& errorMsg);

    // Save a raw mesh asset to disk
    bool saveRawMesh(const RawMeshAsset& asset, const std::string& filepath, std::string& errorMsg);

    // Update an already-loaded raw mesh in GPU memory
    bool updateRawMesh(int meshIndex, const RawMeshAsset& asset, std::string& errorMsg);

    // Build a raw mesh asset from a model scene without writing to disk
    bool buildRawMeshFromScene(const std::string& filepath, RawMeshAsset& out, std::string& errorMsg,
                               glm::vec3* outRootPos = nullptr,
                               glm::vec3* outRootRot = nullptr,
                               glm::vec3* outRootScale = nullptr);

    // Add a raw mesh asset to the GPU cache and return its mesh index
    int addRawMesh(const RawMeshAsset& asset, const std::string& sourcePath,
                   const std::string& name, std::string& errorMsg);

    // Get list of supported formats
    static std::vector<ModelFormat> getSupportedFormats();

    // Clear all loaded meshes
    void clear();

    // Get mesh count
    size_t getMeshCount() const;

private:
    ModelLoader() = default;
    ~ModelLoader() = default;
    ModelLoader(const ModelLoader&) = delete;
    ModelLoader& operator=(const ModelLoader&) = delete;

#if MODULARITY_ENABLE_ASSIMP
    // Process Assimp scene
    void processNode(aiNode* node, const aiScene* scene, const aiMatrix4x4& parentTransform,
                     std::vector<float>& vertices, std::vector<glm::vec3>& triPositions,
                     std::vector<glm::vec3>& positions, std::vector<uint32_t>& indices,
                     glm::vec3& boundsMin, glm::vec3& boundsMax);
    void processMesh(aiMesh* mesh, const aiMatrix4x4& transform,
                     std::vector<float>& vertices, std::vector<glm::vec3>& triPositions,
                     std::vector<glm::vec3>& positions, std::vector<uint32_t>& indices,
                     glm::vec3& boundsMin, glm::vec3& boundsMax);
    Assimp::Importer importer;
#endif

    // Storage for loaded meshes (reusing OBJLoader::LoadedMesh structure)
    std::vector<OBJLoader::LoadedMesh> loadedMeshes;
    std::unordered_map<std::string, ModelSceneData> cachedScenes;
    ModelSceneData sceneRefScratch;
};

// Global accessor
ModelLoader& getModelLoader();
