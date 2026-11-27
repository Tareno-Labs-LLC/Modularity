#include "InstancerTools.h"
#include "InstancerRuntime.h"

#include "ThirdParty/glm/gtc/matrix_transform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace InstancerTools {

namespace {

GroundQuery& GroundQuerySlot() {
    static GroundQuery query;
    return query;
}

struct Rng {
    uint64_t state;
    explicit Rng(uint32_t seed) : state(0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(seed) * 0xBF58476D1CE4E5B9ull)) {
        next();
        next();
    }
    uint64_t next() {   // xorshift64*
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }
    float uniform() { return static_cast<float>((next() >> 40) & 0xFFFFFFull) / 16777216.0f; }
    float range(float lo, float hi) { return lo + (hi - lo) * uniform(); }
};

SceneObject* FindById(std::vector<SceneObject>& objects, int id) {
    for (SceneObject& object : objects) {
        if (object.id == id) return &object;
    }
    return nullptr;
}

std::unordered_map<int, size_t> BuildIndex(const std::vector<SceneObject>& objects) {
    std::unordered_map<int, size_t> index;
    index.reserve(objects.size());
    for (size_t i = 0; i < objects.size(); ++i) index[objects[i].id] = i;
    return index;
}

// Every descendant (root included), parents before children.
void CollectSubtree(const std::vector<SceneObject>& objects,
                    const std::unordered_map<int, size_t>& index, int rootId, std::vector<size_t>& out) {
    out.clear();
    const auto rootIt = index.find(rootId);
    if (rootIt == index.end()) return;
    std::vector<size_t> stack{rootIt->second};
    std::unordered_set<size_t> seen;
    while (!stack.empty() && out.size() < 4096) {
        const size_t current = stack.back();
        stack.pop_back();
        if (!seen.insert(current).second) continue;
        out.push_back(current);
        for (int childId : objects[current].childIds) {
            const auto childIt = index.find(childId);
            if (childIt != index.end()) stack.push_back(childIt->second);
        }
    }
}

// Identity of what a hierarchy draws: its sorted mesh references.
std::string MeshSignature(const std::vector<SceneObject>& objects, const std::vector<size_t>& subtree) {
    std::vector<std::string> parts;
    for (size_t index : subtree) {
        const SceneObject& o = objects[index];
        if (!HasRendererComponent(o) || o.renderType == RenderType::None) continue;
        parts.push_back(std::to_string(static_cast<int>(o.renderType)) + "|" + o.meshPath + "|" +
                        std::to_string(o.meshSourceIndex));
    }
    std::sort(parts.begin(), parts.end());
    std::string signature;
    for (const std::string& part : parts) {
        signature += part;
        signature += '\n';
    }
    return signature;
}

void EraseObjects(std::vector<SceneObject>& objects, const std::unordered_set<int>& doomed) {
    if (doomed.empty()) return;
    objects.erase(std::remove_if(objects.begin(), objects.end(),
                                 [&](const SceneObject& o) { return doomed.count(o.id) > 0; }),
                  objects.end());
    for (SceneObject& o : objects) {
        if (doomed.count(o.parentId) > 0) o.parentId = -1;
        o.childIds.erase(std::remove_if(o.childIds.begin(), o.childIds.end(),
                                        [&](int id) { return doomed.count(id) > 0; }),
                         o.childIds.end());
    }
}

} // namespace

void SetGroundQuery(GroundQuery query) { GroundQuerySlot() = std::move(query); }
const GroundQuery& GetGroundQuery() { return GroundQuerySlot(); }

int CountTemplateMeshes(const std::vector<SceneObject>& objects, int rootId) {
    const auto index = BuildIndex(objects);
    std::vector<size_t> subtree;
    CollectSubtree(objects, index, rootId, subtree);
    int count = 0;
    for (size_t i : subtree) {
        const SceneObject& o = objects[i];
        if (HasRendererComponent(o) && o.renderType != RenderType::None) ++count;
    }
    return count;
}

ConvertResult ConvertChildren(std::vector<SceneObject>& objects, int instancerId, bool nested) {
    ConvertResult result;
    SceneObject* instancer = FindById(objects, instancerId);
    if (!instancer || !instancer->hasInstancer) return result;
    InstancerComponent& comp = instancer->instancer;
    if (comp.prototypes.empty()) return result;

    const auto index = BuildIndex(objects);
    std::vector<size_t> subtree;

    // Template signatures, and every object that belongs to a template tree.
    std::vector<std::string> signatures(comp.prototypes.size());
    std::unordered_set<int> protected_ids;
    std::unordered_set<std::string> lodSignatures;
    for (size_t p = 0; p < comp.prototypes.size(); ++p) {
        const InstancerPrototype& proto = comp.prototypes[p];
        CollectSubtree(objects, index, proto.sourceObjectId, subtree);
        signatures[p] = MeshSignature(objects, subtree);
        for (size_t i : subtree) protected_ids.insert(objects[i].id);
        CollectSubtree(objects, index, proto.lodSourceObjectId, subtree);
        if (!subtree.empty()) {
            lodSignatures.insert(MeshSignature(objects, subtree));
            for (size_t i : subtree) protected_ids.insert(objects[i].id);
        }
    }

    const glm::mat4 world = InstancerRuntime::ObjectWorldMatrix(*instancer);
    const glm::mat4 inverseWorld = glm::inverse(world);
    std::vector<int> pending = instancer->childIds;   // the list changes while we erase
    std::vector<InstancerInstance> added;
    std::unordered_set<int> doomed;
    std::unordered_set<int> visited;

    while (!pending.empty()) {
        const int childId = pending.back();
        pending.pop_back();
        if (!visited.insert(childId).second) continue;
        const auto childIt = index.find(childId);
        if (childIt == index.end()) continue;
        const SceneObject& child = objects[childIt->second];
        if (child.hasInstancer) continue;
        CollectSubtree(objects, index, childId, subtree);
        bool touchesTemplate = false;
        for (size_t i : subtree) {
            if (protected_ids.count(objects[i].id) > 0) { touchesTemplate = true; break; }
        }
        if (touchesTemplate) {
            // A template parked under (or inside) this object stays put; its siblings may still match.
            if (nested && protected_ids.count(child.id) == 0) {
                for (int grandChild : child.childIds) pending.push_back(grandChild);
            }
            continue;
        }

        const std::string signature = MeshSignature(objects, subtree);
        int match = -1;
        if (!signature.empty()) {
            for (size_t p = 0; p < signatures.size(); ++p) {
                if (!signatures[p].empty() && signatures[p] == signature) { match = static_cast<int>(p); break; }
            }
        }
        if (match < 0) {
            if (!signature.empty()) ++result.unmatched;
            if (nested) {
                for (int grandChild : child.childIds) pending.push_back(grandChild);
            }
            continue;
        }
        const InstancerPrototype& proto = comp.prototypes[static_cast<size_t>(match)];
        const auto templateIt = index.find(proto.sourceObjectId);
        if (templateIt == index.end()) continue;
        const SceneObject& tmpl = objects[templateIt->second];

        InstancerInstance instance;
        instance.prototype = static_cast<uint16_t>(match);
        instance.position = glm::vec3(inverseWorld * glm::vec4(child.position, 1.0f));
        instance.yawDegrees = child.rotation.y - tmpl.rotation.y - instancer->rotation.y;
        const float templateScale = std::max(std::abs(tmpl.scale.y), 1e-4f);
        const float instancerScale = std::max(std::abs(instancer->scale.y), 1e-4f);
        instance.scale = std::max(std::abs(child.scale.y) / templateScale / instancerScale, 1e-3f);
        added.push_back(instance);
        for (size_t i : subtree) doomed.insert(objects[i].id);
        ++result.converted;
    }

    if (!added.empty()) {
        std::vector<InstancerInstance>& list = comp.mutableInstances();
        list.insert(list.end(), added.begin(), added.end());
    }
    result.removedObjects = static_cast<int>(doomed.size());
    EraseObjects(objects, doomed);
    return result;
}

ScatterResult Scatter(std::vector<SceneObject>& objects, int instancerId, const ScatterParams& params) {
    ScatterResult result;
    SceneObject* instancer = FindById(objects, instancerId);
    if (!instancer || !instancer->hasInstancer) return result;
    InstancerComponent& comp = instancer->instancer;

    // Prototype pick table.
    std::vector<int> usable;
    std::vector<float> cumulative;
    float total = 0.0f;
    for (size_t p = 0; p < comp.prototypes.size(); ++p) {
        if (comp.prototypes[p].sourceObjectId < 0) continue;
        if (params.prototypeOverride >= 0 && static_cast<int>(p) != params.prototypeOverride) continue;
        total += std::max(comp.prototypes[p].scatterWeight, 0.0f);
        usable.push_back(static_cast<int>(p));
        cumulative.push_back(total);
    }
    if (usable.empty() || total <= 0.0f || params.count <= 0) return result;

    const glm::mat4 world = InstancerRuntime::ObjectWorldMatrix(*instancer);
    const glm::mat4 inverseWorld = glm::inverse(world);
    const glm::mat3 normalToLocal = glm::mat3(glm::transpose(world));   // inverse-transpose of the inverse
    const GroundQuery& ground = GetGroundQuery();

    // Spacing grid in the instancer's local XZ plane.
    const float spacing = std::max(params.minSpacing, 0.0f);
    const float cell = std::max(spacing, 0.5f);
    std::unordered_map<int64_t, std::vector<glm::vec2>> grid;
    const auto cellKey = [&](int cx, int cz) {
        return (static_cast<int64_t>(cx) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(cz));
    };
    const auto cellOf = [&](float v) { return static_cast<int>(std::floor(v / cell)); };
    const auto addToGrid = [&](const glm::vec2& p) { grid[cellKey(cellOf(p.x), cellOf(p.y))].push_back(p); };
    const auto tooClose = [&](const glm::vec2& p) {
        if (spacing <= 0.0f) return false;
        const int cx = cellOf(p.x), cz = cellOf(p.y);
        const int reach = static_cast<int>(std::ceil(spacing / cell));
        for (int dx = -reach; dx <= reach; ++dx) {
            for (int dz = -reach; dz <= reach; ++dz) {
                const auto it = grid.find(cellKey(cx + dx, cz + dz));
                if (it == grid.end()) continue;
                for (const glm::vec2& q : it->second) {
                    const glm::vec2 d = q - p;
                    if (glm::dot(d, d) < spacing * spacing) return true;
                }
            }
        }
        return false;
    };
    if (params.avoidExisting && spacing > 0.0f && comp.instances) {
        for (const InstancerInstance& existing : *comp.instances) {
            addToGrid(glm::vec2(existing.position.x, existing.position.z));
        }
    }

    Rng rng(params.seed);
    std::vector<InstancerInstance> added;
    added.reserve(static_cast<size_t>(params.count));
    const int maxAttempts = params.count * 40 + 200;
    for (int attempt = 0; attempt < maxAttempts && result.placed < params.count; ++attempt) {
        glm::vec2 local;
        if (params.shape == ScatterParams::Shape::Circle) {
            const float angle = rng.range(0.0f, 6.28318530718f);
            const float distance = params.radius * std::sqrt(rng.uniform());
            local = glm::vec2(std::cos(angle), std::sin(angle)) * distance;
        } else {
            local = glm::vec2(rng.range(-0.5f, 0.5f) * params.boxSize.x, rng.range(-0.5f, 0.5f) * params.boxSize.y);
        }
        if (tooClose(local)) {
            ++result.rejectedSpacing;
            continue;
        }

        const glm::vec3 flatWorld = glm::vec3(world * glm::vec4(local.x, 0.0f, local.y, 1.0f));
        glm::vec3 position(local.x, params.yOffset, local.y);
        glm::vec3 worldNormal(0.0f, 1.0f, 0.0f);
        if (ground) {
            const GroundSample sample = ground(objects, flatWorld.x, flatWorld.z);
            if (!sample.valid) {
                ++result.rejectedNoGround;
                continue;
            }
            if (sample.keepOut >= params.keepOutThreshold) {
                ++result.rejectedKeepOut;
                continue;
            }
            const glm::vec3 snapped = glm::vec3(inverseWorld * glm::vec4(flatWorld.x, sample.height, flatWorld.z, 1.0f));
            position = glm::vec3(local.x, snapped.y + params.yOffset, local.y);
            worldNormal = sample.normal;
        }

        const float pick = rng.uniform() * total;
        size_t slot = 0;
        while (slot + 1 < cumulative.size() && pick > cumulative[slot]) ++slot;
        const int prototypeIndex = usable[slot];
        const InstancerPrototype& proto = comp.prototypes[static_cast<size_t>(prototypeIndex)];

        InstancerInstance instance;
        instance.prototype = static_cast<uint16_t>(prototypeIndex);
        instance.position = position;
        instance.yawDegrees = params.randomYaw ? rng.range(0.0f, 360.0f) : 0.0f;
        const float lo = std::max(std::min(proto.scaleMin, proto.scaleMax), 0.01f);
        const float hi = std::max(std::max(proto.scaleMin, proto.scaleMax), lo);
        instance.scale = rng.range(lo, hi);
        if (proto.alignToGround && ground) {
            InstancerRuntime::SetInstanceTilt(instance, glm::normalize(normalToLocal * worldNormal));
        }
        added.push_back(instance);
        addToGrid(local);
        ++result.placed;
    }

    if (!added.empty()) {
        std::vector<InstancerInstance>& list = comp.mutableInstances();
        list.insert(list.end(), added.begin(), added.end());
    }
    return result;
}

SnapResult SnapToGround(std::vector<SceneObject>& objects, int instancerId, bool removeKeepOut,
                        float yOffset, float keepOutThreshold) {
    SnapResult result;
    SceneObject* instancer = FindById(objects, instancerId);
    const GroundQuery& ground = GetGroundQuery();
    if (!instancer || !instancer->hasInstancer || !ground || instancer->instancer.instanceCount() == 0) return result;
    InstancerComponent& comp = instancer->instancer;
    const glm::mat4 world = InstancerRuntime::ObjectWorldMatrix(*instancer);
    const glm::mat4 inverseWorld = glm::inverse(world);
    const glm::mat3 normalToLocal = glm::mat3(glm::transpose(world));
    std::vector<InstancerInstance> next = *comp.instances;
    std::vector<InstancerInstance> kept;
    kept.reserve(next.size());
    for (InstancerInstance& instance : next) {
        const glm::vec3 flatWorld = glm::vec3(world * glm::vec4(instance.position, 1.0f));
        const GroundSample sample = ground(objects, flatWorld.x, flatWorld.z);
        if (!sample.valid) {
            ++result.noGround;
            kept.push_back(instance);
            continue;
        }
        if (removeKeepOut && sample.keepOut >= keepOutThreshold) {
            ++result.removedKeepOut;
            continue;
        }
        const glm::vec3 snapped = glm::vec3(inverseWorld * glm::vec4(flatWorld.x, sample.height, flatWorld.z, 1.0f));
        instance.position.y = snapped.y + yOffset;
        if (instance.prototype < comp.prototypes.size() && comp.prototypes[instance.prototype].alignToGround) {
            InstancerRuntime::SetInstanceTilt(instance, glm::normalize(normalToLocal * sample.normal));
        }
        ++result.snapped;
        kept.push_back(instance);
    }
    comp.setInstances(std::move(kept));
    return result;
}

int ClearInstances(SceneObject& instancer, int prototypeIndex) {
    if (!instancer.hasInstancer || instancer.instancer.instanceCount() == 0) return 0;
    std::vector<InstancerInstance>& list = instancer.instancer.mutableInstances();
    const size_t before = list.size();
    if (prototypeIndex < 0) {
        list.clear();
    } else {
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const InstancerInstance& i) { return i.prototype == prototypeIndex; }),
                   list.end());
    }
    return static_cast<int>(before - list.size());
}

void RemovePrototype(SceneObject& instancer, int prototypeIndex) {
    if (!instancer.hasInstancer) return;
    InstancerComponent& comp = instancer.instancer;
    if (prototypeIndex < 0 || prototypeIndex >= static_cast<int>(comp.prototypes.size())) return;
    if (comp.instanceCount() > 0) {
        std::vector<InstancerInstance>& list = comp.mutableInstances();
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const InstancerInstance& i) { return i.prototype == prototypeIndex; }),
                   list.end());
        for (InstancerInstance& i : list) {
            if (i.prototype > prototypeIndex) --i.prototype;
        }
    }
    comp.prototypes.erase(comp.prototypes.begin() + prototypeIndex);
    comp.touch();
}

} // namespace InstancerTools
