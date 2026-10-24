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
    bool matchesFilter = true;
    bool enabled = true;
    bool drawnLastFrame = false;
    uint32_t iconColor = 0;
    std::string editorIconPath;
    std::string componentIconPath;
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

    template<class Populate>
    void refresh(const std::vector<SceneObject>& objects, const std::string& filter, Populate populate) {
        bool sameOrder = nodes.size() == objects.size();
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
        }
        rootIds.clear();
        attached.assign(objects.size(), false);
        visited.assign(objects.size(), false);
        work.clear();
        for (size_t i = 0; i < objects.size(); ++i) {
            const SceneObject& obj = objects[i];
            auto& node = nodes[i];
            if (node.id != obj.id) {
                node.drawnLastFrame = false;
                node.firstMissingAsset.clear();
                node.missingAssetCount = 0;
            }
            node.id = obj.id;
            node.parentId = obj.parentId;
            node.name = obj.name;
            node.childIds.clear();
            node.matchesFilter = nameMatches(obj.name, filter);
            populate(node, obj);
        }
        // Respect explicit sibling ordering, but parentId is authoritative.
        // Missing/stale childIds must not hide otherwise valid scene objects.
        for (size_t i = 0; i < objects.size(); ++i) {
            for (int childId : objects[i].childIds) {
                auto child = byId.find(childId);
                if (child == byId.end() || child->second == i || attached[child->second] ||
                    nodes[child->second].parentId != objects[i].id) continue;
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
