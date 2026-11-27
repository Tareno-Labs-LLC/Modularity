#pragma once

#include "SceneObject.h"
#include <cctype>
#include <unordered_map>

namespace Modularity {

// Owned row data only: no pointers, references or iterators into sceneObjects.
struct HierarchyNodeSnapshot {
    int id = -1;
    int parentId = -1;
    std::string name;
    std::vector<int> childIds;
    // Exact copy of the object's own childIds, kept only to detect sibling-order edits.
    std::vector<int> sourceChildIds;
    bool matchesFilter = true;
    bool enabled = true;
    bool drawnLastFrame = false;
    uint32_t iconColor = 0;
    std::string editorIconPath;
    std::string componentIconPath;
    // The static string componentIconPath was copied from; a pointer compare replaces a 60-char one.
    const std::string* componentIconSource = nullptr;
    glm::vec4 editorIconTint = glm::vec4(1);
    std::string previewPath;
    MaterialProperties::TextureFilter previewFilter = MaterialProperties::TextureFilter::Bilinear;
    bool hasParallaxLayer2D = false;
    int parallaxOrder = 0;
    std::string firstMissingAsset;
    int missingAssetCount = 0;
};

class HierarchySnapshot {
public:
    std::vector<HierarchyNodeSnapshot> nodes;
    std::vector<int> rootIds;

    HierarchyNodeSnapshot* find(int id) {
        auto it = byId.find(id);
        return it == byId.end() ? nullptr : &nodes[it->second];
    }

    const HierarchyNodeSnapshot* find(int id) const {
        auto it = byId.find(id);
        return it == byId.end() ? nullptr : &nodes[it->second];
    }

    // The tree (childIds, rootIds, matchesFilter) is a pure function of the ids in vector order,
    // parentIds, each object's childIds, the filter and - only while filtering - the names. Every
    // one of those is compared exactly against the copy held here, so the hash-map and DFS passes
    // run only on frames where one actually changed. Row data is still synced every frame by
    // `populate`, which must leave a row untouched when its source fields are unchanged.
    template<class Populate>
    void refresh(const std::vector<SceneObject>& objects, const std::string& filter, Populate populate) {
        bool sameOrder = nodes.size() == objects.size();
        bool structureChanged = !sameOrder || filter != builtFilter;
        if (sameOrder) {
            for (size_t i = 0; i < objects.size(); ++i) {
                if (nodes[i].id != objects[i].id) { sameOrder = false; break; }
            }
        }
        nodes.resize(objects.size());
        if (!sameOrder) {
            byId.clear();
            byId.reserve(objects.size());
            for (size_t i = 0; i < objects.size(); ++i) byId.emplace(objects[i].id, i);
            structureChanged = true;
        }
        for (size_t i = 0; i < objects.size(); ++i) {
            const SceneObject& obj = objects[i];
            auto& node = nodes[i];
            if (node.id != obj.id) {
                node.drawnLastFrame = false;
                node.firstMissingAsset.clear();
                node.missingAssetCount = 0;
            }
            node.id = obj.id;
            if (node.parentId != obj.parentId) {
                node.parentId = obj.parentId;
                structureChanged = true;
            }
            if (node.name != obj.name) {
                node.name = obj.name;
                if (!filter.empty()) structureChanged = true;
            }
            if (node.sourceChildIds != obj.childIds) {
                node.sourceChildIds = obj.childIds;
                structureChanged = true;
            }
            populate(node, obj);
        }
        if (!structureChanged) return;
        builtFilter = filter;
        rootIds.clear();
        attached.assign(nodes.size(), false);
        visited.assign(nodes.size(), false);
        work.clear();
        for (size_t i = 0; i < nodes.size(); ++i) {
            nodes[i].childIds.clear();
            nodes[i].matchesFilter = nameMatches(nodes[i].name, filter);
        }
        // Respect explicit sibling ordering, but parentId is authoritative.
        // Missing/stale childIds must not hide otherwise valid scene objects.
        for (size_t i = 0; i < nodes.size(); ++i) {
            for (int childId : nodes[i].sourceChildIds) {
                auto child = byId.find(childId);
                if (child == byId.end() || child->second == i || attached[child->second] ||
                    nodes[child->second].parentId != nodes[i].id) continue;
                nodes[i].childIds.push_back(childId);
                attached[child->second] = true;
            }
        }
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto parent = byId.find(nodes[i].parentId);
            if (parent == byId.end() || parent->second == i) {
                rootIds.push_back(nodes[i].id);
            } else if (!attached[i]) {
                nodes[parent->second].childIds.push_back(nodes[i].id);
            }
        }
        // Include ancestors of matches so searching for "Grid" reveals a grid
        // below a Canvas rather than filtering its entire branch away.
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (!nodes[i].matchesFilter) continue;
            auto parent = byId.find(nodes[i].parentId);
            while (parent != byId.end() && !nodes[parent->second].matchesFilter) {
                auto& ancestor = nodes[parent->second];
                ancestor.matchesFilter = true;
                parent = byId.find(ancestor.parentId);
            }
        }
        auto visit = [&](int id) {
            work.push_back(id);
            while (!work.empty()) {
                auto it = byId.find(work.back()); work.pop_back();
                if (it == byId.end() || visited[it->second]) continue;
                visited[it->second] = true;
                for (int child : nodes[it->second].childIds) work.push_back(child);
            }
        };
        for (int id : rootIds) visit(id);
        // Malformed cycles should remain inspectable, not disappear forever.
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (!visited[i]) { rootIds.push_back(nodes[i].id); visit(nodes[i].id); }
        }
    }

private:
    std::unordered_map<int, size_t> byId;
    std::vector<bool> attached, visited;
    std::vector<int> work;
    std::string builtFilter;

    static bool nameMatches(const std::string& name, const std::string& filter) {
        if (filter.empty()) return true;
        if (filter.size() > name.size()) return false;
        for (size_t start = 0; start + filter.size() <= name.size(); ++start) {
            size_t i = 0;
            for (; i < filter.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(name[start + i])) !=
                    std::tolower(static_cast<unsigned char>(filter[i]))) break;
            }
            if (i == filter.size()) return true;
        }
        return false;
    }
};
}
