#include "../EditorLocalization.h"
#include "Engine.h"
#include "ModelLoader.h"
#include "ThirdParty/ModuGUI/imgui.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <unordered_set>

namespace Loc = Modularity::Loc;

namespace {
constexpr uint64_t kHashOffset = 1469598103934665603ull;
constexpr uint64_t kHashPrime = 1099511628211ull;

static void hashCombine(uint64_t& hash, uint64_t value) {
    hash ^= value;
    hash *= kHashPrime;
}

static uint64_t quantizedFloat(float value, float scale = 1000.0f) {
    const double v = static_cast<double>(value) * static_cast<double>(scale);
    const int64_t q = static_cast<int64_t>(std::llround(v));
    return static_cast<uint64_t>(q);
}

static uint64_t computeAIPathSourceHash(const std::vector<SceneObject>& objects,
                                        float cellSize,
                                        float obstaclePadding,
                                        bool allowDiagonal,
                                        int maxGridResolution) {
    uint64_t hash = kHashOffset;
    hashCombine(hash, quantizedFloat(cellSize));
    hashCombine(hash, quantizedFloat(obstaclePadding));
    hashCombine(hash, static_cast<uint64_t>(allowDiagonal ? 1 : 0));
    hashCombine(hash, static_cast<uint64_t>(std::max(1, maxGridResolution)));

    for (const auto& obj : objects) {
        if (!obj.enabled) continue;

        if (obj.hasGroundBakedType) {
            hashCombine(hash, 0xA11F00D1ull);
            hashCombine(hash, static_cast<uint64_t>(obj.id));
            hashCombine(hash, static_cast<uint64_t>(obj.groundBakedType.enabled ? 1 : 0));
            hashCombine(hash, static_cast<uint64_t>(obj.groundBakedType.includeInBake ? 1 : 0));
            hashCombine(hash, quantizedFloat(obj.groundBakedType.areaCost));
            hashCombine(hash, quantizedFloat(obj.position.x));
            hashCombine(hash, quantizedFloat(obj.position.y));
            hashCombine(hash, quantizedFloat(obj.position.z));
            hashCombine(hash, quantizedFloat(obj.rotation.x));
            hashCombine(hash, quantizedFloat(obj.rotation.y));
            hashCombine(hash, quantizedFloat(obj.rotation.z));
            hashCombine(hash, quantizedFloat(obj.scale.x));
            hashCombine(hash, quantizedFloat(obj.scale.y));
            hashCombine(hash, quantizedFloat(obj.scale.z));
            if (obj.hasCollider && obj.collider.enabled) {
                hashCombine(hash, quantizedFloat(obj.collider.boxSize.x));
                hashCombine(hash, quantizedFloat(obj.collider.boxSize.y));
                hashCombine(hash, quantizedFloat(obj.collider.boxSize.z));
                hashCombine(hash, quantizedFloat(obj.collider.offset.x));
                hashCombine(hash, quantizedFloat(obj.collider.offset.y));
                hashCombine(hash, quantizedFloat(obj.collider.offset.z));
            }
        }

        if (obj.hasObsticleObject) {
            hashCombine(hash, 0x0B57AC13ull);
            hashCombine(hash, static_cast<uint64_t>(obj.id));
            hashCombine(hash, static_cast<uint64_t>(obj.obsticleObject.enabled ? 1 : 0));
            hashCombine(hash, static_cast<uint64_t>(obj.obsticleObject.carve ? 1 : 0));
            hashCombine(hash, quantizedFloat(obj.obsticleObject.padding));
            hashCombine(hash, quantizedFloat(obj.position.x));
            hashCombine(hash, quantizedFloat(obj.position.y));
            hashCombine(hash, quantizedFloat(obj.position.z));
            hashCombine(hash, quantizedFloat(obj.rotation.x));
            hashCombine(hash, quantizedFloat(obj.rotation.y));
            hashCombine(hash, quantizedFloat(obj.rotation.z));
            hashCombine(hash, quantizedFloat(obj.scale.x));
            hashCombine(hash, quantizedFloat(obj.scale.y));
            hashCombine(hash, quantizedFloat(obj.scale.z));
            if (obj.hasCollider && obj.collider.enabled) {
                hashCombine(hash, quantizedFloat(obj.collider.boxSize.x));
                hashCombine(hash, quantizedFloat(obj.collider.boxSize.y));
                hashCombine(hash, quantizedFloat(obj.collider.boxSize.z));
                hashCombine(hash, quantizedFloat(obj.collider.offset.x));
                hashCombine(hash, quantizedFloat(obj.collider.offset.y));
                hashCombine(hash, quantizedFloat(obj.collider.offset.z));
            }
        }

        if (obj.hasOffMeshLink && obj.offMeshLink.enabled) {
            hashCombine(hash, 0x017DC0DEull);
            hashCombine(hash, static_cast<uint64_t>(obj.id));
            hashCombine(hash, quantizedFloat(obj.offMeshLink.startPoint.x));
            hashCombine(hash, quantizedFloat(obj.offMeshLink.startPoint.y));
            hashCombine(hash, quantizedFloat(obj.offMeshLink.startPoint.z));
            hashCombine(hash, quantizedFloat(obj.offMeshLink.endPoint.x));
            hashCombine(hash, quantizedFloat(obj.offMeshLink.endPoint.y));
            hashCombine(hash, quantizedFloat(obj.offMeshLink.endPoint.z));
            hashCombine(hash, static_cast<uint64_t>(obj.offMeshLink.bidirectional ? 1 : 0));
            hashCombine(hash, quantizedFloat(obj.offMeshLink.costOverride));
        }
    }

    return hash;
}

// Source footprint: full rotated OBB (mesh- or collider-derived) plus a precomputed world AABB
//   1) use the AABB for broad-phase cell range,
struct BakeFootprint {
    glm::vec3 worldCenter = glm::vec3(0.0f); // world position of OBB center (incl. collider/mesh offset)
    glm::vec3 halfLocal = glm::vec3(0.5f);   // half-extents in local frame
    glm::mat3 rotation = glm::mat3(1.0f);    // local → world rotation
    glm::vec3 worldMin = glm::vec3(0.0f);    // world AABB (includes XZ padding)
    glm::vec3 worldMax = glm::vec3(0.0f);
    float areaCost = 1.0f;
    int sourceId = -1;
};

// Attempt to retrieve true mesh bounds. RenderType picks the right cache; falls back to the
// other cache if the first miss-matches the meshId (handles older saves and cross-cache setups).
static bool tryGetMeshLocalBounds(const SceneObject& obj, glm::vec3& outCenter, glm::vec3& outHalf) {
    if (obj.meshId < 0 || !obj.hasRenderer) return false;

    const OBJLoader::LoadedMesh* info = nullptr;
    if (obj.renderType == RenderType::OBJMesh) {
        info = g_objLoader.getMeshInfo(obj.meshId);
    } else if (obj.renderType == RenderType::Model) {
        info = getModelLoader().getMeshInfo(obj.meshId);
    }
    if (!info) {
        info = g_objLoader.getMeshInfo(obj.meshId);
        if (!info) info = getModelLoader().getMeshInfo(obj.meshId);
    }
    if (!info) return false;

    const glm::vec3 mn = info->boundsMin;
    const glm::vec3 mx = info->boundsMax;
    if (!std::isfinite(mn.x) || !std::isfinite(mx.x) || mn.x >= mx.x) return false;

    outCenter = (mn + mx) * 0.5f;
    outHalf = glm::abs(mx - mn) * 0.5f;
    if (outHalf.x < 0.001f && outHalf.y < 0.001f && outHalf.z < 0.001f) return false;
    return true;
}

// Build the rotated OBB footprint. Priority: mesh bounds (× scale) > collider boxSize (× scale) > scale only.
static bool buildFootprint(const SceneObject& obj, float xzPadding, float yPadding, BakeFootprint& out) {
    glm::vec3 halfLocal;
    glm::vec3 localCenter(0.0f);

    glm::vec3 meshCenter, meshHalf;
    if (tryGetMeshLocalBounds(obj, meshCenter, meshHalf)) {
        // Mesh bounds are in local (pre-scale) coordinates; apply object scale.
        halfLocal = glm::vec3(
            std::max(0.005f, meshHalf.x * std::abs(obj.scale.x)),
            std::max(0.005f, meshHalf.y * std::abs(obj.scale.y)),
            std::max(0.005f, meshHalf.z * std::abs(obj.scale.z))
        );
        localCenter = meshCenter * obj.scale;
    } else if (obj.hasCollider && obj.collider.enabled) {
        halfLocal = glm::vec3(
            std::max(0.01f, std::abs(obj.collider.boxSize.x) * 0.5f * std::abs(obj.scale.x)),
            std::max(0.01f, std::abs(obj.collider.boxSize.y) * 0.5f * std::abs(obj.scale.y)),
            std::max(0.01f, std::abs(obj.collider.boxSize.z) * 0.5f * std::abs(obj.scale.z))
        );
        localCenter = obj.collider.offset * obj.scale;
    } else {
        halfLocal = glm::vec3(
            std::max(0.05f, std::abs(obj.scale.x) * 0.5f),
            std::max(0.05f, std::abs(obj.scale.y) * 0.5f),
            std::max(0.05f, std::abs(obj.scale.z) * 0.5f)
        );
    }

    // XYZ Euler matrix matches engine convention (QuatFromEulerXYZ).
    glm::mat4 m(1.0f);
    m = glm::rotate(m, glm::radians(obj.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
    m = glm::rotate(m, glm::radians(obj.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
    m = glm::rotate(m, glm::radians(obj.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::mat3 R(m);

    out.rotation = R;
    out.halfLocal = halfLocal;
    out.worldCenter = obj.position + R * localCenter;

    // World AABB of the rotated OBB: |R| * halfLocal summed across local axes.
    const glm::vec3 col0 = glm::abs(glm::vec3(R[0]));
    const glm::vec3 col1 = glm::abs(glm::vec3(R[1]));
    const glm::vec3 col2 = glm::abs(glm::vec3(R[2]));
    const glm::vec3 worldHalf = col0 * halfLocal.x + col1 * halfLocal.y + col2 * halfLocal.z;

    const float padXZ = std::max(0.0f, xzPadding);
    const float padY = std::max(0.0f, yPadding);

    out.worldMin = glm::vec3(
        out.worldCenter.x - worldHalf.x - padXZ,
        out.worldCenter.y - worldHalf.y - padY,
        out.worldCenter.z - worldHalf.z - padXZ
    );
    out.worldMax = glm::vec3(
        out.worldCenter.x + worldHalf.x + padXZ,
        out.worldCenter.y + worldHalf.y + padY,
        out.worldCenter.z + worldHalf.z + padXZ
    );

    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(out.worldMin[i]) || !std::isfinite(out.worldMax[i])) return false;
    }
    return true;
}

// XZ bounding rectangle (broad phase for grid-cell range queries).
static void footprintAABB(const BakeFootprint& fp, glm::vec2& outMin, glm::vec2& outMax) {
    outMin = glm::vec2(fp.worldMin.x, fp.worldMin.z);
    outMax = glm::vec2(fp.worldMax.x, fp.worldMax.z);
}

// True XZ-shadow containment for an arbitrarily rotated OBB.
static bool xzShadowContains(const BakeFootprint& fp, float wx, float wz, float xzPad = 0.0f) {
    const glm::vec3 worldRel(wx - fp.worldCenter.x, 0.0f, wz - fp.worldCenter.z);
    const glm::mat3 RT = glm::transpose(fp.rotation);
    const glm::vec3 localOrigin = RT * worldRel;
    // World +Y axis transformed into the box's local frame.
    const glm::vec3 localDir(fp.rotation[0].y, fp.rotation[1].y, fp.rotation[2].y);

    const float pad = std::max(0.0f, xzPad);
    float tMin = -std::numeric_limits<float>::infinity();
    float tMax = std::numeric_limits<float>::infinity();
    for (int i = 0; i < 3; ++i) {
        const float h = fp.halfLocal[i] + pad;
        const float d = localDir[i];
        const float o = localOrigin[i];
        if (std::abs(d) < 1e-6f) {
            if (o < -h - 1e-4f || o > h + 1e-4f) return false;
        } else {
            float t1 = (-h - o) / d;
            float t2 = ( h - o) / d;
            if (t1 > t2) std::swap(t1, t2);
            if (t1 > tMin) tMin = t1;
            if (t2 < tMax) tMax = t2;
            if (tMin > tMax) return false;
        }
    }
    return true;
}
} // namespace

#pragma region AI Pathfinding
bool Engine::bakeAIPathGrid(bool logResult) {
    aiPathGrid = AIPathGrid{};
    aiAgentRuntimeStates.clear();

    aiPathBakeSettings.cellSize = std::max(0.05f, aiPathBakeSettings.cellSize);
    aiPathBakeSettings.obstaclePadding = std::clamp(aiPathBakeSettings.obstaclePadding, 0.0f, 25.0f);
    aiPathBakeSettings.maxGridResolution = std::clamp(aiPathBakeSettings.maxGridResolution, 32, 4096);

    aiPathLastSourceHash = computeAIPathSourceHash(
        sceneObjects,
        aiPathBakeSettings.cellSize,
        aiPathBakeSettings.obstaclePadding,
        aiPathBakeSettings.allowDiagonal,
        aiPathBakeSettings.maxGridResolution
    );

    std::vector<BakeFootprint> groundFootprints;
    std::vector<BakeFootprint> obstacleFootprints;
    groundFootprints.reserve(sceneObjects.size());
    obstacleFootprints.reserve(sceneObjects.size());

    std::vector<glm::vec2> groundMins;
    std::vector<glm::vec2> groundMaxs;
    std::vector<glm::vec2> obstacleMins;
    std::vector<glm::vec2> obstacleMaxs;
    groundMins.reserve(sceneObjects.size());
    groundMaxs.reserve(sceneObjects.size());
    obstacleMins.reserve(sceneObjects.size());
    obstacleMaxs.reserve(sceneObjects.size());

    glm::vec2 boundsMin(FLT_MAX);
    glm::vec2 boundsMax(-FLT_MAX);

    for (const auto& obj : sceneObjects) {
        if (!obj.enabled) continue;

        if (obj.hasGroundBakedType && obj.groundBakedType.enabled && obj.groundBakedType.includeInBake) {
            BakeFootprint fp;
            if (buildFootprint(obj, 0.0f, 0.0f, fp)) {
                fp.areaCost = std::max(0.1f, obj.groundBakedType.areaCost);
                fp.sourceId = obj.id;
                glm::vec2 fmin, fmax;
                footprintAABB(fp, fmin, fmax);
                groundFootprints.push_back(fp);
                groundMins.push_back(fmin);
                groundMaxs.push_back(fmax);
                aiPathGrid.sourceGroundIds.push_back(obj.id);
                boundsMin.x = std::min(boundsMin.x, fmin.x);
                boundsMin.y = std::min(boundsMin.y, fmin.y);
                boundsMax.x = std::max(boundsMax.x, fmax.x);
                boundsMax.y = std::max(boundsMax.y, fmax.y);
            }
        }

        if (obj.hasObsticleObject && obj.obsticleObject.enabled && obj.obsticleObject.carve) {
            const float padXZ = aiPathBakeSettings.obstaclePadding + std::max(0.0f, obj.obsticleObject.padding);
            BakeFootprint fp;
            if (buildFootprint(obj, padXZ, 0.0f, fp)) {
                fp.sourceId = obj.id;
                glm::vec2 fmin, fmax;
                footprintAABB(fp, fmin, fmax);
                obstacleFootprints.push_back(fp);
                obstacleMins.push_back(fmin);
                obstacleMaxs.push_back(fmax);
                aiPathGrid.sourceObstacleIds.push_back(obj.id);
            }
        }
    }

    if (groundFootprints.empty()) {
        if (logResult) {
            addConsoleMessage("AI bake skipped: no enabled GroundBakedType surfaces found.", ConsoleMessageType::Warning);
        }
        return false;
    }

    glm::vec2 span = boundsMax - boundsMin;
    span.x = std::max(span.x, aiPathBakeSettings.cellSize);
    span.y = std::max(span.y, aiPathBakeSettings.cellSize);

    float bakeCellSize = aiPathBakeSettings.cellSize;
    int width = static_cast<int>(std::ceil(span.x / bakeCellSize));
    int height = static_cast<int>(std::ceil(span.y / bakeCellSize));
    const int maxRes = std::max(1, aiPathBakeSettings.maxGridResolution);

    // Clamp each axis independently so an oblong scene doesn't blow past maxRes on one axis.
    if (width > maxRes || height > maxRes) {
        const float largestSpan = std::max(span.x, span.y);
        bakeCellSize = std::max(bakeCellSize, largestSpan / static_cast<float>(maxRes));
        width = static_cast<int>(std::ceil(span.x / bakeCellSize));
        height = static_cast<int>(std::ceil(span.y / bakeCellSize));
    }

    width = std::clamp(width, 1, maxRes);
    height = std::clamp(height, 1, maxRes);

    aiPathGrid.baked = true;
    aiPathGrid.origin = boundsMin;
    aiPathGrid.width = width;
    aiPathGrid.height = height;
    aiPathGrid.cellSize = bakeCellSize;
    const size_t cellTotal = static_cast<size_t>(width) * static_cast<size_t>(height);
    aiPathGrid.walkable.assign(cellTotal, 0u);
    aiPathGrid.cellCost.assign(cellTotal, 1.0f);
    aiPathGrid.groundTop.assign(cellTotal, -std::numeric_limits<float>::infinity());

    auto worldToCell = [&](float wx, float wz, int& outX, int& outY) {
        outX = static_cast<int>(std::floor((wx - aiPathGrid.origin.x) / aiPathGrid.cellSize));
        outY = static_cast<int>(std::floor((wz - aiPathGrid.origin.y) / aiPathGrid.cellSize));
    };
    auto cellWorldXZ = [&](int cx, int cy) {
        return glm::vec2(
            aiPathGrid.origin.x + (static_cast<float>(cx) + 0.5f) * aiPathGrid.cellSize,
            aiPathGrid.origin.y + (static_cast<float>(cy) + 0.5f) * aiPathGrid.cellSize
        );
    };

    // 1) Rasterize ground footprints. Walkable cells get the highest groundTop and lowest areaCost.
    for (const BakeFootprint& fp : groundFootprints) {
        glm::vec2 fmin, fmax;
        footprintAABB(fp, fmin, fmax);
        int minX, minY, maxX, maxY;
        worldToCell(fmin.x, fmin.y, minX, minY);
        worldToCell(fmax.x, fmax.y, maxX, maxY);
        minX = std::clamp(minX, 0, width - 1);
        minY = std::clamp(minY, 0, height - 1);
        maxX = std::clamp(maxX, 0, width - 1);
        maxY = std::clamp(maxY, 0, height - 1);
        for (int y = minY; y <= maxY; ++y) {
            const int row = y * width;
            for (int x = minX; x <= maxX; ++x) {
                const glm::vec2 wp = cellWorldXZ(x, y);
                if (!xzShadowContains(fp, wp.x, wp.y, 0.0f)) continue;
                const size_t idx = static_cast<size_t>(row + x);
                aiPathGrid.walkable[idx] = 1u;
                // Highest ground wins (lets a higher platform supersede ground underneath).
                if (fp.worldMax.y > aiPathGrid.groundTop[idx]) {
                    aiPathGrid.groundTop[idx] = fp.worldMax.y;
                    aiPathGrid.cellCost[idx] = fp.areaCost;
                }
            }
        }
    }

    // 2) Carve obstacles, but only against ground that vertically intersects the obstacle.
    const float verticalSlack = std::max(0.05f, aiPathGrid.cellSize * 0.5f);
    for (const BakeFootprint& fp : obstacleFootprints) {
        glm::vec2 fmin, fmax;
        footprintAABB(fp, fmin, fmax);
        int minX, minY, maxX, maxY;
        worldToCell(fmin.x, fmin.y, minX, minY);
        worldToCell(fmax.x, fmax.y, maxX, maxY);
        minX = std::clamp(minX, 0, width - 1);
        minY = std::clamp(minY, 0, height - 1);
        maxX = std::clamp(maxX, 0, width - 1);
        maxY = std::clamp(maxY, 0, height - 1);
        for (int y = minY; y <= maxY; ++y) {
            const int row = y * width;
            for (int x = minX; x <= maxX; ++x) {
                const size_t idx = static_cast<size_t>(row + x);
                if (aiPathGrid.walkable[idx] == 0u) continue;
                const glm::vec2 wp = cellWorldXZ(x, y);
                if (!xzShadowContains(fp, wp.x, wp.y, 0.0f)) continue;
                const float groundY = aiPathGrid.groundTop[idx];
                // Treat any obstacle within agent-height envelope of the local ground as a carver.
                // Obstacles fully above or fully below the ground top are ignored.
                if (fp.worldMax.y < groundY - verticalSlack) continue;
                if (fp.worldMin.y > groundY + 5.0f) continue; // 5m headroom envelope
                aiPathGrid.walkable[idx] = 0u;
                aiPathGrid.cellCost[idx] = 1.0f;
            }
        }
    }

    size_t walkableCount = 0;
    for (uint8_t cell : aiPathGrid.walkable) {
        if (cell != 0u) ++walkableCount;
    }
    if (walkableCount == 0) {
        aiPathGrid.baked = false;
        if (logResult) {
            addConsoleMessage("AI bake failed: generated nav map has no walkable cells.", ConsoleMessageType::Warning);
        }
        return false;
    }

    // 3) Bake OffMeshLinks: snap each endpoint to the nearest walkable cell (BFS) so the link
    //    is reachable even if its anchor sits a few cells off the navigable area.
    auto cellIdx = [&](int x, int y) { return y * width + x; };
    auto inBounds = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height; };
    auto snapToWalkable = [&](const glm::vec3& worldP) -> int {
        int cx, cy;
        worldToCell(worldP.x, worldP.z, cx, cy);
        cx = std::clamp(cx, 0, width - 1);
        cy = std::clamp(cy, 0, height - 1);
        int seed = cellIdx(cx, cy);
        if (aiPathGrid.walkable[static_cast<size_t>(seed)] != 0u) return seed;
        std::queue<int> q;
        std::vector<uint8_t> seen(cellTotal, 0u);
        q.push(seed);
        seen[static_cast<size_t>(seed)] = 1u;
        constexpr std::array<std::array<int, 2>, 4> dirs = {{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
        const int maxSearch = static_cast<int>(std::min<size_t>(cellTotal, static_cast<size_t>(width + height) * 8u));
        int visited = 0;
        while (!q.empty() && visited < maxSearch) {
            int idx = q.front(); q.pop(); ++visited;
            const int x = idx % width;
            const int y = idx / width;
            for (const auto& d : dirs) {
                const int nx = x + d[0];
                const int ny = y + d[1];
                if (!inBounds(nx, ny)) continue;
                const int nidx = cellIdx(nx, ny);
                if (seen[static_cast<size_t>(nidx)]) continue;
                seen[static_cast<size_t>(nidx)] = 1u;
                if (aiPathGrid.walkable[static_cast<size_t>(nidx)] != 0u) return nidx;
                q.push(nidx);
            }
        }
        return -1;
    };

    for (const auto& obj : sceneObjects) {
        if (!obj.enabled || !obj.hasOffMeshLink || !obj.offMeshLink.enabled) continue;
        AIOffMeshLinkBaked link;
        link.sourceId = obj.id;
        link.startPoint = obj.offMeshLink.startPoint;
        link.endPoint = obj.offMeshLink.endPoint;
        link.bidirectional = obj.offMeshLink.bidirectional;
        link.fromCell = snapToWalkable(link.startPoint);
        link.toCell = snapToWalkable(link.endPoint);
        if (link.fromCell < 0 || link.toCell < 0 || link.fromCell == link.toCell) continue;
        const glm::vec2 delta(link.endPoint.x - link.startPoint.x, link.endPoint.z - link.startPoint.z);
        const float planar = std::sqrt(delta.x * delta.x + delta.y * delta.y);
        link.cost = obj.offMeshLink.costOverride > 0.0f
            ? obj.offMeshLink.costOverride
            : std::max(0.25f, planar / std::max(0.05f, aiPathGrid.cellSize));
        aiPathGrid.links.push_back(link);
    }

    if (logResult) {
        addConsoleMessage(
            "AI bake complete: " +
            std::to_string(aiPathGrid.width) + "x" + std::to_string(aiPathGrid.height) +
            " cells, walkable=" + std::to_string(walkableCount) +
            ", ground=" + std::to_string(aiPathGrid.sourceGroundIds.size()) +
            ", obstacles=" + std::to_string(aiPathGrid.sourceObstacleIds.size()) +
            ", offmesh links=" + std::to_string(aiPathGrid.links.size()),
            ConsoleMessageType::Success
        );
    }

    return true;
}

bool Engine::findAIPath(const glm::vec3& start, const glm::vec3& goal, std::vector<glm::vec3>& outPath, float clearancePadding) const {
    outPath.clear();
    if (!aiPathGrid.baked || aiPathGrid.width <= 0 || aiPathGrid.height <= 0 || aiPathGrid.walkable.empty()) {
        return false;
    }

    const int width = aiPathGrid.width;
    const int height = aiPathGrid.height;
    const int cellCount = width * height;
    const float pad = std::max(0.0f, clearancePadding);
    // Padding measured in grid-cell radii (any wall within this many cells = "too close").
    const int padCells = pad > 0.0f
        ? static_cast<int>(std::ceil(pad / std::max(0.05f, aiPathGrid.cellSize)))
        : 0;
    auto inBounds = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height; };
    auto toIndex = [&](int x, int y) { return y * width + x; };
    auto toCell = [&](const glm::vec3& p, int& outX, int& outY) {
        outX = static_cast<int>(std::floor((p.x - aiPathGrid.origin.x) / aiPathGrid.cellSize));
        outY = static_cast<int>(std::floor((p.z - aiPathGrid.origin.y) / aiPathGrid.cellSize));
        outX = std::clamp(outX, 0, width - 1);
        outY = std::clamp(outY, 0, height - 1);
    };
    auto cellCenterXZ = [&](int idx) {
        const int cx = idx % width;
        const int cy = idx / width;
        return glm::vec2(
            aiPathGrid.origin.x + (static_cast<float>(cx) + 0.5f) * aiPathGrid.cellSize,
            aiPathGrid.origin.y + (static_cast<float>(cy) + 0.5f) * aiPathGrid.cellSize
        );
    };
    auto cellGroundY = [&](int idx) {
        const float gy = aiPathGrid.groundTop[static_cast<size_t>(idx)];
        return std::isfinite(gy) ? gy : start.y;
    };
    auto isWalkable = [&](int x, int y) {
        if (!inBounds(x, y)) return false;
        return aiPathGrid.walkable[static_cast<size_t>(toIndex(x, y))] != 0u;
    };
    // A cell satisfies clearance if every cell within padCells is walkable. We collapse the
    // result to a boolean predicate that can stand in for plain isWalkable when padding > 0.
    auto hasClearance = [&](int x, int y) {
        if (!isWalkable(x, y)) return false;
        if (padCells <= 0) return true;
        for (int dy = -padCells; dy <= padCells; ++dy) {
            for (int dx = -padCells; dx <= padCells; ++dx) {
                if (dx == 0 && dy == 0) continue;
                if (!isWalkable(x + dx, y + dy)) return false;
            }
        }
        return true;
    };
    auto cellCost = [&](int idx) {
        return std::max(0.1f, aiPathGrid.cellCost[static_cast<size_t>(idx)]);
    };
    // Snap to nearest cell satisfying the (optional) clearance predicate. Falls back to plain
    auto nearestSatisfying = [&](int startIdx, bool requireClearance) {
        if (startIdx < 0 || startIdx >= cellCount) return -1;
        auto ok = [&](int idx) {
            const int x = idx % width;
            const int y = idx / width;
            return requireClearance ? hasClearance(x, y) : (aiPathGrid.walkable[static_cast<size_t>(idx)] != 0u);
        };
        if (ok(startIdx)) return startIdx;
        std::queue<int> open;
        std::vector<uint8_t> visited(static_cast<size_t>(cellCount), 0u);
        open.push(startIdx);
        visited[static_cast<size_t>(startIdx)] = 1u;
        constexpr std::array<std::array<int, 2>, 4> dirs = {{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
        while (!open.empty()) {
            const int idx = open.front();
            open.pop();
            const int x = idx % width;
            const int y = idx / width;
            for (const auto& d : dirs) {
                const int nx = x + d[0];
                const int ny = y + d[1];
                if (!inBounds(nx, ny)) continue;
                const int nidx = toIndex(nx, ny);
                if (visited[static_cast<size_t>(nidx)] != 0u) continue;
                visited[static_cast<size_t>(nidx)] = 1u;
                if (ok(nidx)) return nidx;
                open.push(nidx);
            }
        }
        return -1;
    };

    int sx = 0, sy = 0;
    int gx = 0, gy = 0;
    toCell(start, sx, sy);
    toCell(goal, gx, gy);

    int startIdx = nearestSatisfying(toIndex(sx, sy), padCells > 0);
    if (startIdx < 0) startIdx = nearestSatisfying(toIndex(sx, sy), false);
    int goalIdx = nearestSatisfying(toIndex(gx, gy), padCells > 0);
    if (goalIdx < 0) goalIdx = nearestSatisfying(toIndex(gx, gy), false);
    if (startIdx < 0 || goalIdx < 0) {
        return false;
    }

    if (startIdx == goalIdx) {
        outPath.push_back(start);
        outPath.push_back(goal);
        return true;
    }

    // Build outgoing OffMeshLink lookup keyed by fromCell.
    struct LinkEdge { int toCell; float cost; int linkIndex; };
    std::unordered_map<int, std::vector<LinkEdge>> linksByCell;
    linksByCell.reserve(aiPathGrid.links.size() * 2);
    for (size_t i = 0; i < aiPathGrid.links.size(); ++i) {
        const auto& link = aiPathGrid.links[i];
        if (link.fromCell < 0 || link.toCell < 0) continue;
        linksByCell[link.fromCell].push_back({link.toCell, link.cost, static_cast<int>(i)});
        if (link.bidirectional) {
            linksByCell[link.toCell].push_back({link.fromCell, link.cost, static_cast<int>(i)});
        }
    }

    struct OpenNode {
        int idx = -1;
        float f = 0.0f;
    };
    struct OpenNodeCmp {
        bool operator()(const OpenNode& a, const OpenNode& b) const { return a.f > b.f; }
    };

    std::priority_queue<OpenNode, std::vector<OpenNode>, OpenNodeCmp> open;
    std::vector<float> gScore(static_cast<size_t>(cellCount), std::numeric_limits<float>::infinity());
    std::vector<int> cameFrom(static_cast<size_t>(cellCount), -1);
    std::vector<int> cameFromLink(static_cast<size_t>(cellCount), -1); // link index used to reach this node (-1 = grid step)
    std::vector<uint8_t> closed(static_cast<size_t>(cellCount), 0u);

    auto heuristic = [&](int idx) {
        const int x = idx % width;
        const int y = idx / width;
        const int tx = goalIdx % width;
        const int ty = goalIdx / width;
        const int dx = std::abs(tx - x);
        const int dy = std::abs(ty - y);
        if (aiPathBakeSettings.allowDiagonal) {
            const int dmin = std::min(dx, dy);
            const int dmax = std::max(dx, dy);
            return static_cast<float>((dmax - dmin) + dmin * std::sqrt(2.0f));
        }
        return static_cast<float>(dx + dy);
    };

    gScore[static_cast<size_t>(startIdx)] = 0.0f;
    open.push({startIdx, heuristic(startIdx)});

    constexpr std::array<std::array<int, 2>, 8> allDirs = {{
        { 1, 0}, {-1, 0}, {0, 1}, {0,-1},
        { 1, 1}, {-1, 1}, {1,-1}, {-1,-1}
    }};

    bool found = false;
    while (!open.empty()) {
        const OpenNode current = open.top();
        open.pop();
        if (closed[static_cast<size_t>(current.idx)] != 0u) continue;
        if (current.idx == goalIdx) {
            found = true;
            break;
        }
        closed[static_cast<size_t>(current.idx)] = 1u;

        const int cx = current.idx % width;
        const int cy = current.idx / width;
        const float currentCost = cellCost(current.idx);
        for (const auto& d : allDirs) {
            const bool diagonal = (d[0] != 0 && d[1] != 0);
            if (diagonal && !aiPathBakeSettings.allowDiagonal) continue;

            const int nx = cx + d[0];
            const int ny = cy + d[1];
            if (!isWalkable(nx, ny)) continue;

            if (diagonal) {
                if (!isWalkable(cx + d[0], cy) || !isWalkable(cx, cy + d[1])) continue;
            }

            const int nidx = toIndex(nx, ny);
            if (closed[static_cast<size_t>(nidx)] != 0u) continue;

            const float baseStep = diagonal ? std::sqrt(2.0f) : 1.0f;
            // Average the two cells' area costs so traversing pricey terrain is symmetric.
            const float stepCost = baseStep * 0.5f * (currentCost + cellCost(nidx));
            const float tentative = gScore[static_cast<size_t>(current.idx)] + stepCost;
            if (tentative < gScore[static_cast<size_t>(nidx)]) {
                cameFrom[static_cast<size_t>(nidx)] = current.idx;
                cameFromLink[static_cast<size_t>(nidx)] = -1;
                gScore[static_cast<size_t>(nidx)] = tentative;
                open.push({nidx, tentative + heuristic(nidx)});
            }
        }

        // OffMeshLink edges out of this cell.
        auto itLinks = linksByCell.find(current.idx);
        if (itLinks != linksByCell.end()) {
            for (const LinkEdge& edge : itLinks->second) {
                if (closed[static_cast<size_t>(edge.toCell)] != 0u) continue;
                const float tentative = gScore[static_cast<size_t>(current.idx)] + edge.cost;
                if (tentative < gScore[static_cast<size_t>(edge.toCell)]) {
                    cameFrom[static_cast<size_t>(edge.toCell)] = current.idx;
                    cameFromLink[static_cast<size_t>(edge.toCell)] = edge.linkIndex;
                    gScore[static_cast<size_t>(edge.toCell)] = tentative;
                    open.push({edge.toCell, tentative + heuristic(edge.toCell)});
                }
            }
        }
    }

    if (!found) {
        return false;
    }

    // Reconstruct cell path with per-edge "via link" flag (-1 means grid step).
    struct PathStep { int idx; int viaLink; };
    std::vector<PathStep> steps;
    int cur = goalIdx;
    while (cur >= 0) {
        steps.push_back({cur, cameFromLink[static_cast<size_t>(cur)]});
        if (cur == startIdx) break;
        cur = cameFrom[static_cast<size_t>(cur)];
    }
    if (steps.empty() || steps.back().idx != startIdx) {
        return false;
    }
    std::reverse(steps.begin(), steps.end());

    // String-pull within each grid-only sub-run between link traversals (and at the start/end).
    auto cellCenter3 = [&](int idx) {
        const glm::vec2 xz = cellCenterXZ(idx);
        return glm::vec3(xz.x, cellGroundY(idx), xz.y);
    };

    // segments stay at least `padCells` away from any wall.
    auto pointClear = [&](float wx, float wz) {
        int ix = static_cast<int>(std::floor((wx - aiPathGrid.origin.x) / aiPathGrid.cellSize));
        int iy = static_cast<int>(std::floor((wz - aiPathGrid.origin.y) / aiPathGrid.cellSize));
        if (padCells <= 0) return isWalkable(ix, iy);
        return hasClearance(ix, iy);
    };
    // Sample many points along a segment; reject if any sample fails the clearance test.
    auto segmentClear = [&](const glm::vec3& a, const glm::vec3& b) {
        const float dx = b.x - a.x;
        const float dz = b.z - a.z;
        const float len = std::sqrt(dx * dx + dz * dz);
        const float step = std::max(0.05f, aiPathGrid.cellSize * 0.4f);
        const int samples = std::max(2, static_cast<int>(std::ceil(len / step)));
        for (int i = 1; i < samples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(samples);
            if (!pointClear(a.x + dx * t, a.z + dz * t)) return false;
        }
        return true;
    };
    // Greedy line-of-sight string-pull: anchor the current waypoint, advance while clear.
    auto stringPull = [&](const std::vector<glm::vec3>& run, std::vector<glm::vec3>& outRun) {
        outRun.clear();
        if (run.empty()) return;
        outRun.push_back(run.front());
        size_t anchor = 0;
        for (size_t i = 2; i < run.size(); ++i) {
            if (!segmentClear(run[anchor], run[i])) {
                outRun.push_back(run[i - 1]);
                anchor = i - 1;
            }
        }
        outRun.push_back(run.back());
    };

    outPath.reserve(steps.size() + 4);

    // Walk steps; flush each grid-only sub-run through string-pull, then append link teleports verbatim.
    std::vector<glm::vec3> currentRun;
    currentRun.push_back(start);
    for (size_t i = 1; i < steps.size(); ++i) {
        const PathStep& step = steps[i];
        if (step.viaLink >= 0) {
            // End the run at the link source (cell center of previous step) then teleport to link.toCell.
            const PathStep& prev = steps[i - 1];
            currentRun.push_back(cellCenter3(prev.idx));
            std::vector<glm::vec3> smoothed;
            stringPull(currentRun, smoothed);
            // Append smoothed run (skip duplicate of the start of next run if any).
            for (const auto& p : smoothed) outPath.push_back(p);

            const AIOffMeshLinkBaked& link = aiPathGrid.links[static_cast<size_t>(step.viaLink)];
            // Determine endpoint orientation: link may be traversed in reverse if bidirectional.
            const bool reverse = (link.toCell != step.idx);
            const glm::vec3 linkStart = reverse ? link.endPoint : link.startPoint;
            const glm::vec3 linkEnd = reverse ? link.startPoint : link.endPoint;
            outPath.push_back(linkStart);
            outPath.push_back(linkEnd);
            currentRun.clear();
            currentRun.push_back(linkEnd);
        } else {
            currentRun.push_back(cellCenter3(step.idx));
        }
    }
    // Final run: append goal so smoothing carries through to the actual target.
    currentRun.push_back(glm::vec3(goal.x, currentRun.back().y, goal.z));
    std::vector<glm::vec3> smoothedTail;
    stringPull(currentRun, smoothedTail);
    for (size_t i = 0; i < smoothedTail.size(); ++i) {
        // Avoid duplicating the join point with a prior link end.
        if (!outPath.empty() && i == 0) {
            const glm::vec3& last = outPath.back();
            const glm::vec3& cand = smoothedTail[0];
            if (std::abs(last.x - cand.x) < 1e-3f && std::abs(last.z - cand.z) < 1e-3f) continue;
        }
        outPath.push_back(smoothedTail[i]);
    }

    if (!outPath.empty()) {
        outPath.back().y = goal.y;
    }

    return outPath.size() >= 2;
}

void Engine::updateAIAgents(float delta) {
    if (delta <= 0.0f) return;

    std::unordered_map<int, SceneObject*> objectById;
    objectById.reserve(sceneObjects.size());
    for (auto& obj : sceneObjects) {
        objectById[obj.id] = &obj;
    }

    std::unordered_set<int> activeAgents;
    activeAgents.reserve(sceneObjects.size());
    for (const auto& obj : sceneObjects) {
        if (obj.enabled && obj.hasAIAgent && obj.aiAgent.enabled) {
            activeAgents.insert(obj.id);
        }
    }
    if (activeAgents.empty()) {
        aiAgentRuntimeStates.clear();
        return;
    }

    const uint64_t currentHash = computeAIPathSourceHash(
        sceneObjects,
        aiPathBakeSettings.cellSize,
        aiPathBakeSettings.obstaclePadding,
        aiPathBakeSettings.allowDiagonal,
        aiPathBakeSettings.maxGridResolution
    );

    if (aiPathBakeSettings.autoRebake && (!aiPathGrid.baked || currentHash != aiPathLastSourceHash)) {
        bakeAIPathGrid(false);
    }
    if (!aiPathGrid.baked) return;

    const float pathSlack = std::max(0.05f, aiPathGrid.cellSize * 0.3f);

    for (auto& obj : sceneObjects) {
        if (!obj.enabled || !obj.hasAIAgent || !obj.aiAgent.enabled) continue;

        auto& agent = obj.aiAgent;
        auto& state = aiAgentRuntimeStates[obj.id];
        state.repathTimer -= delta;

        glm::vec3 goal = agent.destination;
        if (agent.useTargetObject && agent.targetId >= 0) {
            auto itTarget = objectById.find(agent.targetId);
            if (itTarget != objectById.end() && itTarget->second && itTarget->second->enabled) {
                goal = itTarget->second->position;
            }
        }

        const float goalShiftThreshold = std::max(0.1f, aiPathGrid.cellSize * 0.5f);
        bool needsRepath = state.path.empty() || state.nextIndex >= static_cast<int>(state.path.size());
        glm::vec3 goalDelta = goal - state.lastGoal;
        if (!state.hasLastGoal || glm::dot(goalDelta, goalDelta) > goalShiftThreshold * goalShiftThreshold) {
            needsRepath = true;
        }
        if (agent.autoRepath && state.repathTimer <= 0.0f) {
            needsRepath = true;
        }

        if (needsRepath) {
            std::vector<glm::vec3> newPath;
            if (findAIPath(obj.position, goal, newPath, agent.avoidancePadding)) {
                state.path.swap(newPath);
                state.nextIndex = (state.path.size() > 1) ? 1 : 0;
            } else {
                state.path.clear();
                state.nextIndex = 0;
            }
            state.repathTimer = std::max(0.05f, agent.repathInterval);
            state.lastGoal = goal;
            state.hasLastGoal = true;
        }

        if (state.path.empty() || state.nextIndex >= static_cast<int>(state.path.size())) {
            continue;
        }

        auto planarDistance = [](const glm::vec3& a, const glm::vec3& b) {
            const glm::vec2 d(a.x - b.x, a.z - b.z);
            return glm::length(d);
        };

        const float stopDistance = std::max(0.0f, agent.stoppingDistance);
        while (state.nextIndex < static_cast<int>(state.path.size()) &&
               planarDistance(obj.position, state.path[static_cast<size_t>(state.nextIndex)]) <= (stopDistance + pathSlack)) {
            ++state.nextIndex;
        }
        if (state.nextIndex >= static_cast<int>(state.path.size())) {
            continue;
        }

        glm::vec3 targetPos = state.path[static_cast<size_t>(state.nextIndex)];
        glm::vec3 toTarget = targetPos - obj.position;
        toTarget.y = 0.0f;
        float distanceToTarget = glm::length(toTarget);
        glm::vec3 moveDir(0.0f);
        if (distanceToTarget > 1e-4f) {
            moveDir = toTarget / distanceToTarget;
        }

        if (agent.alignToPath && glm::length(moveDir) > 0.001f) {
            const float targetYaw = glm::degrees(std::atan2(moveDir.x, -moveDir.z));
            float yaw = targetYaw;
            if (agent.turnSpeed > 0.0f) {
                // Shortest-arc step toward the target yaw (wrap-aware via std::remainder).
                float diff = std::remainder(targetYaw - obj.rotation.y, 360.0f);
                const float maxStep = agent.turnSpeed * delta;
                if (std::abs(diff) > maxStep) {
                    diff = (diff > 0.0f) ? maxStep : -maxStep;
                }
                yaw = obj.rotation.y + diff;
            }
            obj.rotation.y = yaw;
            if (obj.hasRigidbody && obj.rigidbody.enabled && !obj.rigidbody.isKinematic) {
                physics->setActorYaw(obj.id, yaw);
            }
        }

        const float speed = std::max(0.05f, agent.speed);
        glm::vec3 desiredPlanarVelocity = moveDir * speed;
        if (distanceToTarget <= stopDistance) {
            desiredPlanarVelocity = glm::vec3(0.0f);
        }

        bool movedByPhysics = false;
        if (obj.hasRigidbody && obj.rigidbody.enabled && !obj.rigidbody.isKinematic) {
            glm::vec3 currentVelocity(0.0f);
            physics->getLinearVelocity(obj.id, currentVelocity);
            currentVelocity.x = desiredPlanarVelocity.x;
            currentVelocity.z = desiredPlanarVelocity.z;
            movedByPhysics = physics->setLinearVelocity(obj.id, currentVelocity);
        }

        if (!movedByPhysics) {
            if (glm::length(desiredPlanarVelocity) > 0.0f) {
                const float step = speed * delta;
                const float moveAmount = std::min(step, std::max(0.0f, distanceToTarget - stopDistance));
                obj.position += moveDir * moveAmount;
            }
            syncLocalTransform(obj);
            if (obj.hasRigidbody && obj.rigidbody.enabled && obj.rigidbody.isKinematic) {
                physics->setActorPose(obj.id, obj.position, obj.rotation);
            }
        }
    }

    for (auto it = aiAgentRuntimeStates.begin(); it != aiAgentRuntimeStates.end(); ) {
        if (activeAgents.find(it->first) == activeAgents.end()) {
            it = aiAgentRuntimeStates.erase(it);
        } else {
            ++it;
        }
    }
}

void Engine::renderAIPathfindingWindow() {
    if (!showAIPathfindingWindow) return;

    if (!ImGui::Begin(Loc::Window("WINDOW_AI_PATHFINDING", "AI Pathfinding"), &showAIPathfindingWindow)) {
        ImGui::End();
        return;
    }

    std::vector<SceneObject*> grounds;
    std::vector<SceneObject*> obstacles;
    std::vector<SceneObject*> agents;
    std::vector<SceneObject*> links;
    grounds.reserve(sceneObjects.size());
    obstacles.reserve(sceneObjects.size());
    agents.reserve(sceneObjects.size());
    links.reserve(sceneObjects.size());
    for (auto& obj : sceneObjects) {
        if (!obj.enabled) continue;
        if (obj.hasGroundBakedType && obj.groundBakedType.enabled && obj.groundBakedType.includeInBake) {
            grounds.push_back(&obj);
        }
        if (obj.hasObsticleObject && obj.obsticleObject.enabled && obj.obsticleObject.carve) {
            obstacles.push_back(&obj);
        }
        if (obj.hasAIAgent && obj.aiAgent.enabled) {
            agents.push_back(&obj);
        }
        if (obj.hasOffMeshLink && obj.offMeshLink.enabled) {
            links.push_back(&obj);
        }
    }

    const uint64_t sourceHash = computeAIPathSourceHash(
        sceneObjects,
        aiPathBakeSettings.cellSize,
        aiPathBakeSettings.obstaclePadding,
        aiPathBakeSettings.allowDiagonal,
        aiPathBakeSettings.maxGridResolution
    );
    const bool bakeStale = aiPathGrid.baked && (sourceHash != aiPathLastSourceHash);
    // If the bake has zero ground sources right now but a grid exists, it's a stale orphan.
    const bool bakeOrphaned = aiPathGrid.baked && grounds.empty();

    ImGui::TextDisabled("Sources");
    ImGui::Text("Ground: %zu  Obstacles: %zu  Agents: %zu  Links: %zu",
                grounds.size(), obstacles.size(), agents.size(), links.size());
    ImGui::Separator();

    bool settingsChanged = false;
    ImGui::TextDisabled("Bake Settings");
    if (ImGui::DragFloat("Cell Size", &aiPathBakeSettings.cellSize, 0.01f, 0.05f, 10.0f, "%.2f")) {
        aiPathBakeSettings.cellSize = std::clamp(aiPathBakeSettings.cellSize, 0.05f, 10.0f);
        settingsChanged = true;
    }
    if (ImGui::DragFloat("Obstacle Padding", &aiPathBakeSettings.obstaclePadding, 0.01f, 0.0f, 25.0f, "%.2f")) {
        aiPathBakeSettings.obstaclePadding = std::clamp(aiPathBakeSettings.obstaclePadding, 0.0f, 25.0f);
        settingsChanged = true;
    }
    if (ImGui::DragInt("Max Resolution", &aiPathBakeSettings.maxGridResolution, 1.0f, 32, 4096)) {
        aiPathBakeSettings.maxGridResolution = std::clamp(aiPathBakeSettings.maxGridResolution, 32, 4096);
        settingsChanged = true;
    }
    if (ImGui::Checkbox("Allow Diagonal", &aiPathBakeSettings.allowDiagonal)) {
        settingsChanged = true;
    }
    if (ImGui::Checkbox("Auto Rebake", &aiPathBakeSettings.autoRebake)) {
        settingsChanged = true;
    }
    if (settingsChanged) {
        projectManager.currentProject.hasUnsavedChanges = true;
    }

    if (aiPathBakeSettings.autoRebake && sourceHash != aiPathLastSourceHash) {
        bakeAIPathGrid(false);
    }
    // Auto-clear an orphaned bake (no ground sources remaining) when auto-rebake is on,
    // so the panel never lies about the current scene.
    if (aiPathBakeSettings.autoRebake && bakeOrphaned) {
        aiPathGrid = AIPathGrid{};
        aiAgentRuntimeStates.clear();
        aiPathLastSourceHash = sourceHash;
    }

    if (ImGui::Button("Bake Navigation")) {
        bakeAIPathGrid(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Bake")) {
        aiPathGrid = AIPathGrid{};
        aiAgentRuntimeStates.clear();
        aiPathLastSourceHash = sourceHash;
    }

    ImGui::SameLine();
    ImGui::Checkbox("Draw Grid", &aiPathDrawGrid);
    ImGui::SameLine();
    ImGui::Checkbox("Draw Path", &aiPathDrawPath);

    if (aiPathGrid.baked) {
        size_t walkableCount = 0;
        for (uint8_t cell : aiPathGrid.walkable) {
            if (cell != 0u) ++walkableCount;
        }
        ImGui::TextDisabled(
            "Baked: %dx%d (cell %.2f), walkable %zu/%zu, links %zu",
            aiPathGrid.width,
            aiPathGrid.height,
            aiPathGrid.cellSize,
            walkableCount,
            aiPathGrid.walkable.size(),
            aiPathGrid.links.size()
        );
    } else {
        ImGui::TextDisabled("No baked navigation grid.");
    }

    // Stale / orphan warnings.
    if (bakeOrphaned) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.3f, 1.0f));
        ImGui::TextWrapped("Bake is orphaned: scene has no enabled GroundBakedType sources. Clear or re-bake.");
        ImGui::PopStyleColor();
    } else if (bakeStale) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
        ImGui::TextWrapped("Bake is stale: scene changed since last bake. Re-bake to refresh.");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Preview");

    if (!agents.empty()) {
        auto findAgentById = [&](int id) -> SceneObject* {
            for (SceneObject* a : agents) {
                if (a && a->id == id) return a;
            }
            return nullptr;
        };

        if (!findAgentById(aiPreviewAgentId)) {
            aiPreviewAgentId = agents.front()->id;
        }

        SceneObject* previewAgent = findAgentById(aiPreviewAgentId);
        const char* selectedLabel = previewAgent ? previewAgent->name.c_str() : "<none>";
        if (ImGui::BeginCombo("Agent", selectedLabel)) {
            for (SceneObject* candidate : agents) {
                if (!candidate) continue;
                const bool selected = (candidate->id == aiPreviewAgentId);
                if (ImGui::Selectable(candidate->name.c_str(), selected)) {
                    aiPreviewAgentId = candidate->id;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (previewAgent) {
            bool overrideTarget = aiPreviewTargetId >= 0;
            if (ImGui::Checkbox("Override Preview Target", &overrideTarget)) {
                if (!overrideTarget) {
                    aiPreviewTargetId = -1;
                } else if (previewAgent->aiAgent.targetId >= 0) {
                    aiPreviewTargetId = previewAgent->aiAgent.targetId;
                }
            }

            if (overrideTarget) {
                SceneObject* selectedTarget = findObjectById(aiPreviewTargetId);
                const char* label = selectedTarget ? selectedTarget->name.c_str() : "<missing>";
                if (ImGui::BeginCombo("Target Object", label)) {
                    for (auto& obj : sceneObjects) {
                        if (!obj.enabled || obj.id == previewAgent->id) continue;
                        const bool selected = (obj.id == aiPreviewTargetId);
                        if (ImGui::Selectable(obj.name.c_str(), selected)) {
                            aiPreviewTargetId = obj.id;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
        }
    } else {
        aiPreviewAgentId = -1;
        aiPreviewTargetId = -1;
        ImGui::TextDisabled("No enabled AI Agent components in scene.");
    }

    std::vector<glm::vec3> previewPath;
    glm::vec3 previewStart(0.0f);
    glm::vec3 previewGoal(0.0f);
    bool hasPreviewPath = false;
    if (aiPathGrid.baked && aiPreviewAgentId >= 0) {
        SceneObject* previewAgent = findObjectById(aiPreviewAgentId);
        if (previewAgent && previewAgent->enabled && previewAgent->hasAIAgent && previewAgent->aiAgent.enabled) {
            previewStart = previewAgent->position;
            previewGoal = previewAgent->aiAgent.destination;

            if (aiPreviewTargetId >= 0) {
                if (SceneObject* target = findObjectById(aiPreviewTargetId)) {
                    if (target->enabled) {
                        previewGoal = target->position;
                    }
                }
            } else if (previewAgent->aiAgent.useTargetObject && previewAgent->aiAgent.targetId >= 0) {
                if (SceneObject* target = findObjectById(previewAgent->aiAgent.targetId)) {
                    if (target->enabled) {
                        previewGoal = target->position;
                    }
                }
            }

            hasPreviewPath = findAIPath(previewStart, previewGoal, previewPath, previewAgent->aiAgent.avoidancePadding);
        }
    }

    const float minCanvasHeight = 180.0f;
    const float maxCanvasHeight = 360.0f;
    float canvasHeight = std::clamp(ImGui::GetContentRegionAvail().y, minCanvasHeight, maxCanvasHeight);
    ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize(ImGui::GetContentRegionAvail().x, canvasHeight);
    if (canvasSize.x < 32.0f) canvasSize.x = 32.0f;

    ImGui::InvisibleButton("##AIPathPreview", canvasSize);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);
    draw->AddRectFilled(canvasMin, canvasMax, IM_COL32(20, 24, 32, 255), 6.0f);
    draw->AddRect(canvasMin, canvasMax, IM_COL32(60, 72, 84, 255), 6.0f);

    auto worldToCanvas = [&](float wx, float wz) {
        const float spanX = std::max(0.001f, aiPathGrid.cellSize * static_cast<float>(std::max(1, aiPathGrid.width)));
        const float spanY = std::max(0.001f, aiPathGrid.cellSize * static_cast<float>(std::max(1, aiPathGrid.height)));
        float u = (wx - aiPathGrid.origin.x) / spanX;
        float v = (wz - aiPathGrid.origin.y) / spanY;
        u = std::clamp(u, 0.0f, 1.0f);
        v = std::clamp(v, 0.0f, 1.0f);
        // Flip both axes to match the editor's top-down camera convention
        // (+X projected to canvas-left, +Z projected to canvas-top).
        return ImVec2(
            canvasMax.x - u * canvasSize.x,
            canvasMax.y - v * canvasSize.y
        );
    };

    // Unity-style preview: scale aggregation to canvas-pixel size so a wide/thin grid still reads
    // cleanly. Walkable cells = solid translucent blue. Non-walkable = near-black. High-cost
    // cells get a subtle orange tint only when there's actual variation (avoids misleading reds
    // when areaCost is uniformly default).
    if (aiPathGrid.baked && aiPathGrid.width > 0 && aiPathGrid.height > 0 && aiPathDrawGrid) {
        const int maxPreviewCells = 180;
        const int stepX = std::max(1, static_cast<int>(std::ceil(static_cast<float>(aiPathGrid.width) / static_cast<float>(maxPreviewCells))));
        const int stepY = std::max(1, static_cast<int>(std::ceil(static_cast<float>(aiPathGrid.height) / static_cast<float>(maxPreviewCells))));
        const int previewW = static_cast<int>(std::ceil(static_cast<float>(aiPathGrid.width) / static_cast<float>(stepX)));
        const int previewH = static_cast<int>(std::ceil(static_cast<float>(aiPathGrid.height) / static_cast<float>(stepY)));

        const float cellW = canvasSize.x / static_cast<float>(std::max(1, previewW));
        const float cellH = canvasSize.y / static_cast<float>(std::max(1, previewH));

        // First pass: detect whether area cost actually varies. If everything is ~1.0, skip the
        // orange tint entirely so the preview stays uniformly blue (matches Unity's default look).
        float minCost = std::numeric_limits<float>::infinity();
        float maxCost = -std::numeric_limits<float>::infinity();
        for (size_t i = 0; i < aiPathGrid.walkable.size(); ++i) {
            if (aiPathGrid.walkable[i] == 0u) continue;
            const float c = aiPathGrid.cellCost[i];
            if (c < minCost) minCost = c;
            if (c > maxCost) maxCost = c;
        }
        const bool costVaries = std::isfinite(minCost) && std::isfinite(maxCost) && (maxCost - minCost) > 0.25f;

        for (int py = 0; py < previewH; ++py) {
            for (int px = 0; px < previewW; ++px) {
                const int x0 = px * stepX;
                const int y0 = py * stepY;
                const int x1 = std::min(aiPathGrid.width, x0 + stepX);
                const int y1 = std::min(aiPathGrid.height, y0 + stepY);
                int walkableCount = 0;
                float costAccum = 0.0f;
                const int total = std::max(1, (x1 - x0) * (y1 - y0));
                for (int y = y0; y < y1; ++y) {
                    const int row = y * aiPathGrid.width;
                    for (int x = x0; x < x1; ++x) {
                        if (aiPathGrid.walkable[static_cast<size_t>(row + x)] != 0u) {
                            ++walkableCount;
                            costAccum += aiPathGrid.cellCost[static_cast<size_t>(row + x)];
                        }
                    }
                }

                // Flip both axes so canvas matches the editor's top-down view convention.
                const ImVec2 p0(canvasMax.x - (px + 1) * cellW, canvasMax.y - (py + 1) * cellH);
                const ImVec2 p1(canvasMax.x - px * cellW, canvasMax.y - py * cellH);

                if (walkableCount == 0) {
                    draw->AddRectFilled(p0, p1, IM_COL32(28, 32, 40, 255));
                } else {
                    const float coverage = static_cast<float>(walkableCount) / static_cast<float>(total);
                    int r = 60, g = 150, b = 220;
                    if (costVaries) {
                        const float avg = costAccum / static_cast<float>(walkableCount);
                        const float t = std::clamp((avg - minCost) / std::max(0.01f, (maxCost - minCost)), 0.0f, 1.0f);
                        r = static_cast<int>(60.0f + t * 140.0f);
                        g = static_cast<int>(150.0f - t * 50.0f);
                        b = static_cast<int>(220.0f - t * 120.0f);
                    }
                    const int a = static_cast<int>(160.0f + coverage * 80.0f);
                    draw->AddRectFilled(p0, p1, IM_COL32(r, g, b, a));
                }
            }
        }
    }

    // OffMeshLink arrows on the preview.
    if (aiPathGrid.baked && aiPathDrawGrid) {
        for (const auto& link : aiPathGrid.links) {
            const ImVec2 a = worldToCanvas(link.startPoint.x, link.startPoint.z);
            const ImVec2 b = worldToCanvas(link.endPoint.x, link.endPoint.z);
            const ImU32 col = IM_COL32(255, 200, 90, 230);
            draw->AddLine(a, b, col, 2.0f);
            draw->AddCircleFilled(a, 3.5f, col, 10);
            draw->AddCircleFilled(b, 3.5f, col, 10);
            // Small arrowhead at midpoint indicating direction.
            ImVec2 dir(b.x - a.x, b.y - a.y);
            const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (len > 0.001f) {
                dir.x /= len; dir.y /= len;
                ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                const float headSize = 5.0f;
                ImVec2 p1(mid.x - dir.y * headSize - dir.x * headSize, mid.y + dir.x * headSize - dir.y * headSize);
                ImVec2 p2(mid.x + dir.y * headSize - dir.x * headSize, mid.y - dir.x * headSize - dir.y * headSize);
                draw->AddTriangleFilled(ImVec2(mid.x + dir.x * headSize, mid.y + dir.y * headSize), p1, p2, col);
            }
        }
    }

    if (aiPathGrid.baked && aiPathDrawPath && hasPreviewPath && previewPath.size() >= 2) {
        std::vector<ImVec2> polyline;
        polyline.reserve(previewPath.size());
        for (const auto& p : previewPath) {
            polyline.push_back(worldToCanvas(p.x, p.z));
        }
        // Thin outline for legibility, then bright core.
        draw->AddPolyline(polyline.data(), static_cast<int>(polyline.size()), IM_COL32(20, 18, 8, 255), false, 4.5f);
        draw->AddPolyline(polyline.data(), static_cast<int>(polyline.size()), IM_COL32(255, 224, 120, 255), false, 2.5f);
        draw->AddCircleFilled(worldToCanvas(previewStart.x, previewStart.z), 4.5f, IM_COL32(104, 194, 255, 255), 14);
        draw->AddCircleFilled(worldToCanvas(previewGoal.x, previewGoal.z), 5.0f, IM_COL32(255, 122, 122, 255), 14);
    }

    if (!aiPathGrid.baked) {
        draw->AddText(ImVec2(canvasMin.x + 10.0f, canvasMin.y + 10.0f), IM_COL32(180, 190, 200, 255), "Bake navigation to preview cells.");
    } else if (aiPathDrawPath && !hasPreviewPath) {
        draw->AddText(ImVec2(canvasMin.x + 10.0f, canvasMin.y + 10.0f), IM_COL32(196, 170, 120, 255), "No path found for current preview agent/goal.");
    }

    if (ImGui::CollapsingHeader("Baked Sources")) {
        ImGui::TextDisabled("Ground IDs");
        if (aiPathGrid.sourceGroundIds.empty()) {
            ImGui::TextUnformatted("None");
        } else {
            for (int id : aiPathGrid.sourceGroundIds) {
                SceneObject* obj = findObjectById(id);
                ImGui::BulletText("%d  %s", id, obj ? obj->name.c_str() : "<missing>");
            }
        }

        ImGui::TextDisabled("Obstacle IDs");
        if (aiPathGrid.sourceObstacleIds.empty()) {
            ImGui::TextUnformatted("None");
        } else {
            for (int id : aiPathGrid.sourceObstacleIds) {
                SceneObject* obj = findObjectById(id);
                ImGui::BulletText("%d  %s", id, obj ? obj->name.c_str() : "<missing>");
            }
        }

        ImGui::TextDisabled("OffMesh Links");
        if (aiPathGrid.links.empty()) {
            ImGui::TextUnformatted("None");
        } else {
            for (const auto& link : aiPathGrid.links) {
                SceneObject* obj = findObjectById(link.sourceId);
                ImGui::BulletText("%d  %s  (%s, cost %.2f)",
                                  link.sourceId,
                                  obj ? obj->name.c_str() : "<missing>",
                                  link.bidirectional ? "two-way" : "one-way",
                                  link.cost);
            }
        }
    }

    ImGui::End();
}
#pragma endregion
