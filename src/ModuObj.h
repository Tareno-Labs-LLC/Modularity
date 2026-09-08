#pragma once

#include "Common.h"
#include "SceneObject.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace ModuObj {

constexpr int kFormatVersion = 1;

constexpr int kMaxNestingDepth = 16;

constexpr const char* kAssetExtension = ".moduobj";

struct NestedRef {
    int localId = -1;
    std::string assetId;
    std::string lastKnownPath;
};

struct Asset {
    int formatVersion = kFormatVersion;
    std::string assetId;
    std::string name;
    std::vector<int> roots;
    int nextLocalId = 1;
    std::vector<NestedRef> nested;

    std::vector<SceneObject> objects;

    bool valid() const { return !assetId.empty() && !objects.empty() && !roots.empty(); }
};

std::string GenerateAssetId();

bool IsWellFormedAssetId(const std::string& id);

bool SaveAsset(const fs::path& path, const Asset& asset, std::string& outError);

bool LoadAsset(const fs::path& path, Asset& outAsset, std::string& outError);

bool WriteAssetStream(std::ostream& out, const Asset& asset, std::string& outError);
bool ReadAssetStream(std::istream& in, Asset& outAsset, std::string& outError);

bool BuildAssetFromSelection(const std::vector<SceneObject>& sceneObjects,
                             const std::vector<int>& selectedIds,
                             const std::string& assetName,
                             Asset& outAsset,
                             std::vector<std::string>& outExternalRefs,
                             std::string& outError);

struct InstanceResult {
    bool success = false;
    std::string error;

    std::string instanceId;
    std::string assetId;
    std::vector<int> rootObjectIds;
    std::unordered_map<int, int> localToScene;
    std::vector<SceneObject> objects;
};

InstanceResult Instantiate(const Asset& asset,
                           int& nextObjectId,
                           int parentId = -1,
                           const glm::vec3* position = nullptr,
                           const glm::vec3* rotation = nullptr,
                           const glm::vec3* scale = nullptr);

class AssetCache {
public:
    const Asset* get(const fs::path& path, std::string& outError);

    void invalidate(const fs::path& path);
    void clear();
    size_t size() const { return entries.size(); }

private:
    struct Entry {
        Asset asset;
        fs::file_time_type modified{};
    };
    std::unordered_map<std::string, Entry> entries;
};

InstanceResult SpawnIntoScene(const Asset& asset,
                              std::vector<SceneObject>& sceneObjects,
                              int& nextObjectId,
                              int parentId = -1,
                              const glm::vec3* position = nullptr,
                              const glm::vec3* rotation = nullptr,
                              const glm::vec3* scale = nullptr,
                              bool spawnDisabled = false);

int DestroyInstance(const std::string& instanceId, std::vector<SceneObject>& sceneObjects);

std::vector<int> GetInstanceRoots(const std::string& instanceId,
                                  const std::vector<SceneObject>& sceneObjects);

int FindInstanceObject(const std::string& instanceId,
                       int sourceLocalId,
                       const std::vector<SceneObject>& sceneObjects);

int FindInstanceObjectByName(const std::string& instanceId,
                             const std::string& name,
                             const std::vector<SceneObject>& sceneObjects);

std::string GetInstanceAssetId(const std::string& instanceId,
                               const std::vector<SceneObject>& sceneObjects);

bool ValidateInstance(const std::string& instanceId,
                      const std::vector<SceneObject>& sceneObjects,
                      std::string& outError);

std::vector<std::string> CollectInstanceIds(const std::vector<SceneObject>& sceneObjects);

using AssetResolver = std::function<bool(const std::string& assetId, fs::path& outPath)>;

bool DetectCycle(const std::string& rootAssetId,
                 const AssetResolver& resolver,
                 std::string& outChain);

}  // namespace ModuObj
