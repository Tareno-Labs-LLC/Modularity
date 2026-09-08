#include "ModuObj.h"

#include "SceneSerializationInternal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <unordered_set>

namespace ModuObj {
namespace {

std::string Trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
}

bool ParseAssignment(const std::string& line, std::string& outKey, std::string& outValue) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    outKey = Trim(line.substr(0, eq));
    std::string value = Trim(line.substr(eq + 1));
    if (!value.empty() && value.back() == ';') value.pop_back();
    value = Trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    outValue = value;
    return !outKey.empty();
}

std::vector<int> ParseIntList(const std::string& value) {
    std::vector<int> out;
    std::stringstream ss(value);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token = Trim(token);
        if (token.empty()) continue;
        try {
            out.push_back(std::stoi(token));
        } catch (...) {
        }
    }
    return out;
}

std::string JoinIntList(const std::vector<int>& values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += ",";
        out += std::to_string(values[i]);
    }
    return out;
}

std::string EscapeString(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

constexpr const char* kObjectMarker = "[Object]";

std::string StripSceneGlobals(const std::string& payload, int objectCount, int nextLocalId) {
    const size_t firstObject = payload.find(kObjectMarker);

    std::ostringstream out;
    out << "# Scene File\n";
    out << "version=" << SceneSerializationInternal::kLegacySceneFormatVersion << "\n";
    out << "nextId=" << nextLocalId << "\n";
    out << "objectCount=" << objectCount << "\n\n";

    if (firstObject != std::string::npos) {
        out << payload.substr(firstObject);
    }
    return out.str();
}

}  // namespace

std::string GenerateAssetId() {
    static std::mt19937_64 rng([] {
        std::random_device rd;
        const uint64_t clock = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return static_cast<uint64_t>(rd()) ^ (clock * 0x9E3779B97F4A7C15ull);
    }());
    std::ostringstream out;
    out << "mobj-" << std::hex << std::setw(16) << std::setfill('0') << rng();
    return out.str();
}

bool IsWellFormedAssetId(const std::string& id) {
    if (id.rfind("mobj-", 0) != 0) return false;
    const std::string hex = id.substr(5);
    if (hex.size() != 16) return false;
    return std::all_of(hex.begin(), hex.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool WriteAssetStream(std::ostream& out, const Asset& asset, std::string& outError) {
    if (asset.assetId.empty()) {
        outError = "ModuOBJ has no asset id.";
        return false;
    }
    if (asset.objects.empty()) {
        outError = "ModuOBJ has no objects.";
        return false;
    }
    if (asset.roots.empty()) {
        outError = "ModuOBJ has no root objects.";
        return false;
    }

    out << "MODU_OBJASSET {\n";
    out << "    formatVersion=" << asset.formatVersion << ";\n";
    out << "    assetId=\"" << EscapeString(asset.assetId) << "\";\n";
    out << "    name=\"" << EscapeString(asset.name) << "\";\n";
    out << "    roots=" << JoinIntList(asset.roots) << ";\n";
    out << "    nextLocalId=" << asset.nextLocalId << ";\n";
    out << "}\n\n";

    for (const NestedRef& nested : asset.nested) {
        out << "MODU_OBJNESTED {\n";
        out << "    localId=" << nested.localId << ";\n";
        out << "    assetId=\"" << EscapeString(nested.assetId) << "\";\n";
        out << "    lastKnownPath=\"" << EscapeString(nested.lastKnownPath) << "\";\n";
        out << "}\n\n";
    }

    std::ostringstream payload;
    if (!SceneSerializationInternal::WriteLegacySceneStream(
            payload, asset.objects, asset.nextLocalId, 0.5f, SkyboxSettings{})) {
        outError = "Failed to serialize ModuOBJ object hierarchy.";
        return false;
    }
    out << StripSceneGlobals(payload.str(),
                             static_cast<int>(asset.objects.size()),
                             asset.nextLocalId);
    return true;
}

bool ReadAssetStream(std::istream& in, Asset& outAsset, std::string& outError) {
    outAsset = Asset{};
    outError.clear();

    std::ostringstream rest;
    std::string line;
    bool sawHeader = false;
    bool inHeader = false;
    bool inNested = false;
    NestedRef pendingNested;
    int headerVersion = -1;

    while (std::getline(in, line)) {
        const std::string trimmed = Trim(line);

        if (!inHeader && !inNested) {
            if (trimmed.rfind("MODU_OBJASSET", 0) == 0) {
                inHeader = true;
                sawHeader = true;
                continue;
            }
            if (trimmed.rfind("MODU_OBJNESTED", 0) == 0) {
                inNested = true;
                pendingNested = NestedRef{};
                continue;
            }
            rest << line << "\n";
            continue;
        }

        if (trimmed == "}" || trimmed == "};") {
            if (inNested) {
                if (pendingNested.localId >= 0 && !pendingNested.assetId.empty()) {
                    outAsset.nested.push_back(pendingNested);
                }
                inNested = false;
            } else {
                inHeader = false;
            }
            continue;
        }
        if (trimmed == "{") continue;

        std::string key, value;
        if (!ParseAssignment(trimmed, key, value)) continue;

        if (inHeader) {
            if (key == "formatVersion") {
                try { headerVersion = std::stoi(value); } catch (...) { headerVersion = -1; }
            } else if (key == "assetId") {
                outAsset.assetId = value;
            } else if (key == "name") {
                outAsset.name = value;
            } else if (key == "roots") {
                outAsset.roots = ParseIntList(value);
            } else if (key == "nextLocalId") {
                try { outAsset.nextLocalId = std::stoi(value); } catch (...) {}
            }
        } else {
            if (key == "localId") {
                try { pendingNested.localId = std::stoi(value); } catch (...) {}
            } else if (key == "assetId") {
                pendingNested.assetId = value;
            } else if (key == "lastKnownPath") {
                pendingNested.lastKnownPath = value;
            }
        }
    }

    if (!sawHeader) {
        outError = "Not a ModuOBJ asset: missing MODU_OBJASSET header.";
        return false;
    }
    if (headerVersion < 0) {
        outError = "ModuOBJ header has no formatVersion.";
        return false;
    }
    if (headerVersion > kFormatVersion) {
        outError = "ModuOBJ format version " + std::to_string(headerVersion) +
                   " is newer than this engine supports (" + std::to_string(kFormatVersion) +
                   "). Update Modularity to open this asset.";
        return false;
    }
    outAsset.formatVersion = headerVersion;
    if (!IsWellFormedAssetId(outAsset.assetId)) {
        outError = "ModuOBJ has a missing or malformed assetId.";
        return false;
    }

    std::istringstream payload(rest.str());
    int payloadNextId = 0;
    int payloadVersion = 0;
    if (!SceneSerializationInternal::LoadLegacySceneStream(
            payload, outAsset.objects, payloadNextId, payloadVersion,
            nullptr, nullptr, true, nullptr)) {
        outError = "Failed to parse the ModuOBJ object hierarchy.";
        return false;
    }
    if (outAsset.objects.empty()) {
        outError = "ModuOBJ contains no objects.";
        return false;
    }
    if (outAsset.nextLocalId <= 0) {
        outAsset.nextLocalId = payloadNextId > 0 ? payloadNextId : 1;
    }

    std::unordered_set<int> seen;
    for (const SceneObject& obj : outAsset.objects) {
        if (!seen.insert(obj.id).second) {
            outError = "ModuOBJ contains duplicate source-local object id " +
                       std::to_string(obj.id) + ".";
            return false;
        }
    }
    for (const SceneObject& obj : outAsset.objects) {
        if (obj.parentId != -1 && seen.count(obj.parentId) == 0) {
            outError = "ModuOBJ object " + std::to_string(obj.id) +
                       " references missing parent " + std::to_string(obj.parentId) + ".";
            return false;
        }
    }
    if (outAsset.roots.empty()) {
        for (const SceneObject& obj : outAsset.objects) {
            if (obj.parentId == -1) outAsset.roots.push_back(obj.id);
        }
    }
    for (int rootId : outAsset.roots) {
        if (seen.count(rootId) == 0) {
            outError = "ModuOBJ root list names missing object " + std::to_string(rootId) + ".";
            return false;
        }
    }
    if (outAsset.roots.empty()) {
        outError = "ModuOBJ has no root objects.";
        return false;
    }
    return true;
}

bool SaveAsset(const fs::path& path, const Asset& asset, std::string& outError) {
    outError.clear();

    std::string serialized;
    {
        std::ostringstream out;
        if (!WriteAssetStream(out, asset, outError)) return false;
        serialized = out.str();
    }

    {
        Asset roundTrip;
        std::istringstream check(serialized);
        std::string parseError;
        if (!ReadAssetStream(check, roundTrip, parseError)) {
            outError = "Refusing to save a ModuOBJ that does not parse back: " + parseError;
            return false;
        }
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    const fs::path tempPath = path.string() + ".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            outError = "Failed to open temporary file: " + tempPath.string();
            return false;
        }
        out << serialized;
        out.flush();
        if (!out) {
            out.close();
            fs::remove(tempPath, ec);
            outError = "Failed to write temporary ModuOBJ file.";
            return false;
        }
    }

    fs::rename(tempPath, path, ec);
    if (ec) {
        std::error_code copyEc;
        fs::copy_file(tempPath, path, fs::copy_options::overwrite_existing, copyEc);
        fs::remove(tempPath, ec);
        if (copyEc) {
            outError = "Failed to move the ModuOBJ into place: " + copyEc.message();
            return false;
        }
    }
    return true;
}

bool LoadAsset(const fs::path& path, Asset& outAsset, std::string& outError) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        outError = "ModuOBJ asset not found: " + path.string();
        return false;
    }
    if (!ReadAssetStream(in, outAsset, outError)) {
        if (!outError.empty()) outError += " (" + path.filename().string() + ")";
        return false;
    }
    return true;
}

bool BuildAssetFromSelection(const std::vector<SceneObject>& sceneObjects,
                             const std::vector<int>& selectedIds,
                             const std::string& assetName,
                             Asset& outAsset,
                             std::vector<std::string>& outExternalRefs,
                             std::string& outError) {
    outAsset = Asset{};
    outExternalRefs.clear();
    outError.clear();

    if (selectedIds.empty()) {
        outError = "No objects selected.";
        return false;
    }

    std::unordered_map<int, const SceneObject*> byId;
    for (const SceneObject& obj : sceneObjects) byId[obj.id] = &obj;

    const std::unordered_set<int> selectedSet(selectedIds.begin(), selectedIds.end());
    std::vector<int> selectedRoots;
    for (int id : selectedIds) {
        if (byId.count(id) == 0) continue;
        bool hasSelectedAncestor = false;
        int parent = byId[id]->parentId;
        while (parent != -1) {
            if (selectedSet.count(parent)) { hasSelectedAncestor = true; break; }
            auto it = byId.find(parent);
            if (it == byId.end()) break;
            parent = it->second->parentId;
        }
        if (!hasSelectedAncestor &&
            std::find(selectedRoots.begin(), selectedRoots.end(), id) == selectedRoots.end()) {
            selectedRoots.push_back(id);
        }
    }
    if (selectedRoots.empty()) {
        outError = "Selection contained no valid scene objects.";
        return false;
    }

    std::vector<int> captureOrder;
    std::unordered_set<int> captured;
    std::vector<int> stack(selectedRoots.rbegin(), selectedRoots.rend());
    while (!stack.empty()) {
        const int id = stack.back();
        stack.pop_back();
        if (!captured.insert(id).second) continue;
        auto it = byId.find(id);
        if (it == byId.end()) continue;
        captureOrder.push_back(id);
        const std::vector<int>& children = it->second->childIds;
        for (auto child = children.rbegin(); child != children.rend(); ++child) {
            stack.push_back(*child);
        }
    }

    std::unordered_map<int, int> sceneToLocal;
    int nextLocal = 1;
    for (int id : captureOrder) sceneToLocal[id] = nextLocal++;

    outAsset.assetId = GenerateAssetId();
    outAsset.name = assetName;
    outAsset.nextLocalId = nextLocal;
    outAsset.formatVersion = kFormatVersion;

    for (int id : captureOrder) {
        SceneObject copy = *byId[id];
        copy.id = sceneToLocal[id];

        if (copy.parentId != -1) {
            auto parentIt = sceneToLocal.find(copy.parentId);
            if (parentIt != sceneToLocal.end()) {
                copy.parentId = parentIt->second;
            } else {
                copy.parentId = -1;
            }
        }

        std::vector<int> mappedChildren;
        mappedChildren.reserve(copy.childIds.size());
        for (int childId : copy.childIds) {
            auto childIt = sceneToLocal.find(childId);
            if (childIt != sceneToLocal.end()) {
                mappedChildren.push_back(childIt->second);
            } else {
                outExternalRefs.push_back(
                    "Object \"" + copy.name + "\" had child object id " +
                    std::to_string(childId) + " outside the selection; it was not captured.");
            }
        }
        copy.childIds = std::move(mappedChildren);

        copy.hasModuObjInstance = false;
        copy.moduObjInstance = ModuObjInstanceRef{};

        if (copy.parentId == -1) {
            outAsset.roots.push_back(copy.id);
        }
        outAsset.objects.push_back(std::move(copy));
    }

    if (outAsset.roots.empty()) {
        outError = "Captured hierarchy has no root objects.";
        return false;
    }
    return true;
}

InstanceResult Instantiate(const Asset& asset,
                           int& nextObjectId,
                           int parentId,
                           const glm::vec3* position,
                           const glm::vec3* rotation,
                           const glm::vec3* scale) {
    InstanceResult result;
    result.assetId = asset.assetId;

    if (!asset.valid()) {
        result.error = "ModuOBJ asset is empty or invalid.";
        return result;
    }

    result.objects.reserve(asset.objects.size());
    for (const SceneObject& src : asset.objects) {
        result.localToScene[src.id] = nextObjectId++;
    }

    result.instanceId = GenerateAssetId();
    result.instanceId = "minst-" + result.instanceId.substr(5);

    for (const SceneObject& src : asset.objects) {
        SceneObject obj = src;
        obj.id = result.localToScene[src.id];

        if (src.parentId != -1) {
            auto it = result.localToScene.find(src.parentId);
            obj.parentId = (it != result.localToScene.end()) ? it->second : parentId;
        } else {
            obj.parentId = parentId;
        }

        std::vector<int> children;
        children.reserve(src.childIds.size());
        for (int childLocal : src.childIds) {
            auto it = result.localToScene.find(childLocal);
            if (it != result.localToScene.end()) children.push_back(it->second);
        }
        obj.childIds = std::move(children);

        const bool isRoot = std::find(asset.roots.begin(), asset.roots.end(), src.id) != asset.roots.end();
        obj.hasModuObjInstance = true;
        obj.moduObjInstance = ModuObjInstanceRef{};
        obj.moduObjInstance.assetId = asset.assetId;
        obj.moduObjInstance.instanceId = result.instanceId;
        obj.moduObjInstance.sourceLocalId = src.id;
        obj.moduObjInstance.isRoot = isRoot;

        if (isRoot) {
            result.rootObjectIds.push_back(obj.id);
            if (position) obj.position = *position;
            if (rotation) obj.rotation = *rotation;
            if (scale) obj.scale = *scale;
        }

        result.objects.push_back(std::move(obj));
    }

    if (result.rootObjectIds.empty()) {
        result.error = "ModuOBJ produced no root objects.";
        result.objects.clear();
        return result;
    }

    result.success = true;
    return result;
}

const Asset* AssetCache::get(const fs::path& path, std::string& outError) {
    outError.clear();
    std::error_code ec;
    const fs::file_time_type modified = fs::last_write_time(path, ec);
    if (ec) {
        outError = "ModuOBJ asset not found: " + path.string();
        entries.erase(path.string());
        return nullptr;
    }

    const std::string key = path.string();
    auto it = entries.find(key);
    if (it != entries.end() && it->second.modified == modified) {
        return &it->second.asset;
    }

    Asset parsed;
    if (!LoadAsset(path, parsed, outError)) {
        entries.erase(key);
        return nullptr;
    }

    Entry entry;
    entry.asset = std::move(parsed);
    entry.modified = modified;
    auto inserted = entries.insert_or_assign(key, std::move(entry));
    return &inserted.first->second.asset;
}

void AssetCache::invalidate(const fs::path& path) { entries.erase(path.string()); }

void AssetCache::clear() { entries.clear(); }

InstanceResult SpawnIntoScene(const Asset& asset,
                              std::vector<SceneObject>& sceneObjects,
                              int& nextObjectId,
                              int parentId,
                              const glm::vec3* position,
                              const glm::vec3* rotation,
                              const glm::vec3* scale,
                              bool spawnDisabled) {
    int candidateNextId = nextObjectId;
    InstanceResult result = Instantiate(asset, candidateNextId, parentId,
                                        position, rotation, scale);
    if (!result.success) {
        return result;
    }

    if (spawnDisabled) {
        for (SceneObject& obj : result.objects) {
            if (obj.moduObjInstance.isRoot) obj.enabled = false;
        }
    }

    if (parentId != -1) {
        auto parentIt = std::find_if(sceneObjects.begin(), sceneObjects.end(),
                                     [&](const SceneObject& o) { return o.id == parentId; });
        if (parentIt == sceneObjects.end()) {
            InstanceResult failed;
            failed.error = "Parent object " + std::to_string(parentId) + " does not exist.";
            return failed;
        }
        for (int rootId : result.rootObjectIds) {
            parentIt->childIds.push_back(rootId);
        }
    }

    sceneObjects.reserve(sceneObjects.size() + result.objects.size());
    for (SceneObject& obj : result.objects) {
        sceneObjects.push_back(obj);
    }
    nextObjectId = candidateNextId;
    return result;
}

int DestroyInstance(const std::string& instanceId, std::vector<SceneObject>& sceneObjects) {
    if (instanceId.empty()) return 0;

    std::unordered_set<int> doomed;
    for (const SceneObject& obj : sceneObjects) {
        if (obj.hasModuObjInstance && obj.moduObjInstance.instanceId == instanceId) {
            doomed.insert(obj.id);
        }
    }
    if (doomed.empty()) return 0;

    bool grew = true;
    while (grew) {
        grew = false;
        for (const SceneObject& obj : sceneObjects) {
            if (doomed.count(obj.id)) continue;
            if (obj.parentId != -1 && doomed.count(obj.parentId)) {
                doomed.insert(obj.id);
                grew = true;
            }
        }
    }

    for (SceneObject& obj : sceneObjects) {
        if (doomed.count(obj.id)) continue;
        obj.childIds.erase(std::remove_if(obj.childIds.begin(), obj.childIds.end(),
                                          [&](int id) { return doomed.count(id) != 0; }),
                           obj.childIds.end());
    }

    const size_t before = sceneObjects.size();
    sceneObjects.erase(std::remove_if(sceneObjects.begin(), sceneObjects.end(),
                                      [&](const SceneObject& o) { return doomed.count(o.id) != 0; }),
                       sceneObjects.end());
    return static_cast<int>(before - sceneObjects.size());
}

std::vector<int> GetInstanceRoots(const std::string& instanceId,
                                  const std::vector<SceneObject>& sceneObjects) {
    std::vector<std::pair<int, int>> found;
    for (const SceneObject& obj : sceneObjects) {
        if (obj.hasModuObjInstance && obj.moduObjInstance.instanceId == instanceId &&
            obj.moduObjInstance.isRoot) {
            found.emplace_back(obj.moduObjInstance.sourceLocalId, obj.id);
        }
    }
    std::sort(found.begin(), found.end());
    std::vector<int> roots;
    roots.reserve(found.size());
    for (const auto& entry : found) roots.push_back(entry.second);
    return roots;
}

int FindInstanceObject(const std::string& instanceId,
                       int sourceLocalId,
                       const std::vector<SceneObject>& sceneObjects) {
    for (const SceneObject& obj : sceneObjects) {
        if (obj.hasModuObjInstance && obj.moduObjInstance.instanceId == instanceId &&
            obj.moduObjInstance.sourceLocalId == sourceLocalId) {
            return obj.id;
        }
    }
    return -1;
}

int FindInstanceObjectByName(const std::string& instanceId,
                             const std::string& name,
                             const std::vector<SceneObject>& sceneObjects) {
    for (const SceneObject& obj : sceneObjects) {
        if (obj.hasModuObjInstance && obj.moduObjInstance.instanceId == instanceId &&
            obj.name == name) {
            return obj.id;
        }
    }
    return -1;
}

std::string GetInstanceAssetId(const std::string& instanceId,
                               const std::vector<SceneObject>& sceneObjects) {
    for (const SceneObject& obj : sceneObjects) {
        if (obj.hasModuObjInstance && obj.moduObjInstance.instanceId == instanceId) {
            return obj.moduObjInstance.assetId;
        }
    }
    return {};
}

bool ValidateInstance(const std::string& instanceId,
                      const std::vector<SceneObject>& sceneObjects,
                      std::string& outError) {
    outError.clear();

    std::unordered_set<int> present;
    std::unordered_set<int> localIds;
    int roots = 0;
    for (const SceneObject& obj : sceneObjects) {
        if (!obj.hasModuObjInstance || obj.moduObjInstance.instanceId != instanceId) continue;
        present.insert(obj.id);
        if (!localIds.insert(obj.moduObjInstance.sourceLocalId).second) {
            outError = "Instance " + instanceId + " has two objects claiming source-local id " +
                       std::to_string(obj.moduObjInstance.sourceLocalId) + ".";
            return false;
        }
        if (obj.moduObjInstance.isRoot) ++roots;
    }
    if (present.empty()) {
        outError = "Instance " + instanceId + " is not present in the scene.";
        return false;
    }
    if (roots == 0) {
        outError = "Instance " + instanceId + " has no root object.";
        return false;
    }

    for (const SceneObject& obj : sceneObjects) {
        if (!obj.hasModuObjInstance || obj.moduObjInstance.instanceId != instanceId) continue;
        if (obj.moduObjInstance.isRoot || obj.parentId == -1) continue;
        const bool parentExists =
            std::any_of(sceneObjects.begin(), sceneObjects.end(),
                        [&](const SceneObject& o) { return o.id == obj.parentId; });
        if (!parentExists) {
            outError = "Instance " + instanceId + " object " + std::to_string(obj.id) +
                       " references missing parent " + std::to_string(obj.parentId) + ".";
            return false;
        }
    }
    return true;
}

std::vector<std::string> CollectInstanceIds(const std::vector<SceneObject>& sceneObjects) {
    std::vector<std::string> ids;
    std::unordered_set<std::string> seen;
    for (const SceneObject& obj : sceneObjects) {
        if (!obj.hasModuObjInstance) continue;
        const std::string& id = obj.moduObjInstance.instanceId;
        if (id.empty()) continue;
        if (seen.insert(id).second) ids.push_back(id);
    }
    return ids;
}

bool DetectCycle(const std::string& rootAssetId,
                 const AssetResolver& resolver,
                 std::string& outChain) {
    outChain.clear();
    if (!resolver) return false;

    struct Frame {
        std::string assetId;
        std::vector<std::string> path;
    };
    std::vector<Frame> stack{ Frame{ rootAssetId, { rootAssetId } } };
    std::unordered_set<std::string> fullyExplored;

    while (!stack.empty()) {
        Frame frame = stack.back();
        stack.pop_back();

        if (frame.path.size() > static_cast<size_t>(kMaxNestingDepth)) {
            outChain = "exceeded maximum nesting depth of " + std::to_string(kMaxNestingDepth);
            for (const std::string& step : frame.path) outChain += "\n  " + step;
            return true;
        }
        if (fullyExplored.count(frame.assetId)) continue;

        fs::path assetPath;
        if (!resolver(frame.assetId, assetPath)) continue;

        Asset asset;
        std::string error;
        if (!LoadAsset(assetPath, asset, error)) continue;

        for (const NestedRef& nested : asset.nested) {
            if (nested.assetId.empty()) continue;

            const auto hit = std::find(frame.path.begin(), frame.path.end(), nested.assetId);
            if (hit != frame.path.end()) {
                outChain.clear();
                for (auto it = hit; it != frame.path.end(); ++it) {
                    outChain += *it + " -> ";
                }
                outChain += nested.assetId;
                return true;
            }

            Frame next;
            next.assetId = nested.assetId;
            next.path = frame.path;
            next.path.push_back(nested.assetId);
            stack.push_back(std::move(next));
        }
        fullyExplored.insert(frame.assetId);
    }
    return false;
}

}  // namespace ModuObj
