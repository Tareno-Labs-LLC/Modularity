#include "MeshEditOperations.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace MeshEditOperations {
namespace {

constexpr float kGeometryEpsilon = 1e-6f;

uint64_t edgeKey(uint32_t a, uint32_t b) {
    return (static_cast<uint64_t>(std::min(a, b)) << 32) |
           static_cast<uint64_t>(std::max(a, b));
}

bool faceIsValid(const RawMeshAsset& mesh, const glm::u32vec3& face) {
    return face.x < mesh.positions.size() &&
           face.y < mesh.positions.size() &&
           face.z < mesh.positions.size() &&
           face.x != face.y && face.y != face.z && face.z != face.x;
}

glm::vec3 unnormalizedFaceNormal(const RawMeshAsset& mesh,
                                const glm::u32vec3& face) {
    if (!faceIsValid(mesh, face)) {
        return glm::vec3(0.0f);
    }
    return glm::cross(mesh.positions[face.y] - mesh.positions[face.x],
                      mesh.positions[face.z] - mesh.positions[face.x]);
}

bool validateMesh(const RawMeshAsset& mesh, std::string& error) {
    if (mesh.positions.empty()) {
        error = "The mesh has no vertices.";
        return false;
    }
    if (mesh.faces.empty()) {
        error = "The mesh has no faces.";
        return false;
    }
    for (size_t fi = 0; fi < mesh.faces.size(); ++fi) {
        if (!faceIsValid(mesh, mesh.faces[fi])) {
            error = "Face " + std::to_string(fi) +
                    " has invalid or repeated vertex indices.";
            return false;
        }
        if (glm::length(unnormalizedFaceNormal(mesh, mesh.faces[fi])) <=
            kGeometryEpsilon) {
            error = "Face " + std::to_string(fi) + " is degenerate.";
            return false;
        }
    }
    return true;
}

void updateBounds(RawMeshAsset& mesh) {
    mesh.boundsMin = glm::vec3(FLT_MAX);
    mesh.boundsMax = glm::vec3(-FLT_MAX);
    for (const glm::vec3& position : mesh.positions) {
        mesh.boundsMin = glm::min(mesh.boundsMin, position);
        mesh.boundsMax = glm::max(mesh.boundsMax, position);
    }
}

void updateNormalsFromWinding(RawMeshAsset& mesh) {
    mesh.normals.assign(mesh.positions.size(), glm::vec3(0.0f));
    for (const glm::u32vec3& face : mesh.faces) {
        const glm::vec3 normal = unnormalizedFaceNormal(mesh, face);
        if (glm::length(normal) <= kGeometryEpsilon) {
            continue;
        }
        mesh.normals[face.x] += normal;
        mesh.normals[face.y] += normal;
        mesh.normals[face.z] += normal;
    }
    for (glm::vec3& normal : mesh.normals) {
        const float length = glm::length(normal);
        normal = length > kGeometryEpsilon ? normal / length
                                           : glm::vec3(0.0f);
    }
    mesh.hasNormals = true;
    updateBounds(mesh);
}

std::vector<int> validatedUniqueIndices(const std::vector<int>& indices,
                                        size_t limit) {
    std::vector<int> result;
    result.reserve(indices.size());
    for (int index : indices) {
        if (index < 0 || index >= static_cast<int>(limit)) {
            continue;
        }
        if (std::find(result.begin(), result.end(), index) == result.end()) {
            result.push_back(index);
        }
    }
    return result;
}

void compactUnusedVertices(RawMeshAsset& mesh) {
    std::vector<uint8_t> used(mesh.positions.size(), 0u);
    for (const glm::u32vec3& face : mesh.faces) {
        if (!faceIsValid(mesh, face)) {
            continue;
        }
        used[face.x] = used[face.y] = used[face.z] = 1u;
    }

    std::vector<uint32_t> remap(mesh.positions.size(), UINT32_MAX);
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    positions.reserve(mesh.positions.size());
    normals.reserve(mesh.positions.size());
    uvs.reserve(mesh.positions.size());
    for (size_t vi = 0; vi < mesh.positions.size(); ++vi) {
        if (!used[vi]) {
            continue;
        }
        remap[vi] = static_cast<uint32_t>(positions.size());
        positions.push_back(mesh.positions[vi]);
        normals.push_back(vi < mesh.normals.size() ? mesh.normals[vi]
                                                   : glm::vec3(0.0f));
        uvs.push_back(vi < mesh.uvs.size() ? mesh.uvs[vi]
                                           : glm::vec2(0.0f));
    }
    for (glm::u32vec3& face : mesh.faces) {
        face = glm::u32vec3(remap[face.x], remap[face.y], remap[face.z]);
    }
    mesh.positions = std::move(positions);
    mesh.normals = std::move(normals);
    mesh.uvs = std::move(uvs);
}

} // namespace

bool FlipFaces(RawMeshAsset& mesh, const std::vector<int>& faceIndices,
               std::string& error) {
    error.clear();
    if (!validateMesh(mesh, error)) {
        return false;
    }
    const std::vector<int> faces =
        validatedUniqueIndices(faceIndices, mesh.faces.size());
    if (faces.empty()) {
        error = "Select at least one valid face to flip.";
        return false;
    }

    RawMeshAsset result = mesh;
    for (int faceIndex : faces) {
        std::swap(result.faces[faceIndex].y, result.faces[faceIndex].z);
    }
    updateNormalsFromWinding(result);
    mesh = std::move(result);
    return true;
}

bool RecalculateNormals(RawMeshAsset& mesh, bool pointInside,
                        std::string& error) {
    error.clear();
    if (!validateMesh(mesh, error)) {
        return false;
    }

    struct EdgeUse {
        int face = -1;
        bool forward = false;
    };
    std::unordered_map<uint64_t, std::vector<EdgeUse>> edgeUses;
    edgeUses.reserve(mesh.faces.size() * 3);
    for (int fi = 0; fi < static_cast<int>(mesh.faces.size()); ++fi) {
        const glm::u32vec3& face = mesh.faces[fi];
        const uint32_t vertices[3] = {face.x, face.y, face.z};
        for (int edge = 0; edge < 3; ++edge) {
            const uint32_t a = vertices[edge];
            const uint32_t b = vertices[(edge + 1) % 3];
            edgeUses[edgeKey(a, b)].push_back({fi, a < b});
        }
    }
    for (const auto& entry : edgeUses) {
        if (entry.second.size() > 2) {
            error =
                "Cannot orient normals on non-manifold topology (an edge is "
                "shared by more than two faces).";
            return false;
        }
    }

    RawMeshAsset result = mesh;
    std::vector<uint8_t> visited(result.faces.size(), 0u);
    std::vector<uint8_t> flip(result.faces.size(), 0u);

    for (int start = 0; start < static_cast<int>(result.faces.size()); ++start) {
        if (visited[start]) {
            continue;
        }
        std::vector<int> component;
        std::queue<int> pending;
        pending.push(start);
        visited[start] = 1u;

        while (!pending.empty()) {
            const int current = pending.front();
            pending.pop();
            component.push_back(current);
            const glm::u32vec3& face = result.faces[current];
            const uint32_t vertices[3] = {face.x, face.y, face.z};
            for (int edge = 0; edge < 3; ++edge) {
                const uint32_t a = vertices[edge];
                const uint32_t b = vertices[(edge + 1) % 3];
                const auto& uses = edgeUses[edgeKey(a, b)];
                if (uses.size() != 2) {
                    continue;
                }
                const EdgeUse* currentUse =
                    uses[0].face == current ? &uses[0] : &uses[1];
                const EdgeUse* neighborUse =
                    uses[0].face == current ? &uses[1] : &uses[0];
                const bool neighborFlip =
                    flip[current] ^ (currentUse->forward ==
                                     neighborUse->forward);
                if (!visited[neighborUse->face]) {
                    visited[neighborUse->face] = 1u;
                    flip[neighborUse->face] = neighborFlip ? 1u : 0u;
                    pending.push(neighborUse->face);
                } else if (flip[neighborUse->face] !=
                           static_cast<uint8_t>(neighborFlip)) {
                    error =
                        "Cannot orient normals consistently on this topology.";
                    return false;
                }
            }
        }

        for (int fi : component) {
            if (flip[fi]) {
                std::swap(result.faces[fi].y, result.faces[fi].z);
            }
        }

        bool closed = true;
        for (int fi : component) {
            const glm::u32vec3& face = result.faces[fi];
            const uint32_t vertices[3] = {face.x, face.y, face.z};
            for (int edge = 0; edge < 3; ++edge) {
                if (edgeUses[edgeKey(vertices[edge],
                                     vertices[(edge + 1) % 3])].size() != 2) {
                    closed = false;
                    break;
                }
            }
            if (!closed) {
                break;
            }
        }

        bool currentlyOutside = true;
        if (closed) {
            double volume6 = 0.0;
            for (int fi : component) {
                const glm::u32vec3& face = result.faces[fi];
                volume6 += glm::dot(
                    result.positions[face.x],
                    glm::cross(result.positions[face.y],
                               result.positions[face.z]));
            }
            currentlyOutside = volume6 >= 0.0;
        } else {
            glm::vec3 center(0.0f);
            std::unordered_set<uint32_t> vertices;
            for (int fi : component) {
                const glm::u32vec3& face = result.faces[fi];
                vertices.insert(face.x);
                vertices.insert(face.y);
                vertices.insert(face.z);
            }
            for (uint32_t vertex : vertices) {
                center += result.positions[vertex];
            }
            center /= static_cast<float>(vertices.size());

            float score = 0.0f;
            for (int fi : component) {
                const glm::u32vec3& face = result.faces[fi];
                const glm::vec3 normal =
                    unnormalizedFaceNormal(result, face);
                const glm::vec3 faceCenter =
                    (result.positions[face.x] + result.positions[face.y] +
                     result.positions[face.z]) /
                    3.0f;
                score += glm::dot(normal, faceCenter - center);
            }
            if (std::abs(score) <= kGeometryEpsilon) {
                error =
                    "Inside/outside is ambiguous for a flat open surface. "
                    "Flip the desired faces directly instead.";
                return false;
            }
            currentlyOutside = score >= 0.0f;
        }

        const bool wantOutside = !pointInside;
        if (currentlyOutside != wantOutside) {
            for (int fi : component) {
                std::swap(result.faces[fi].y, result.faces[fi].z);
            }
        }
    }

    updateNormalsFromWinding(result);
    mesh = std::move(result);
    return true;
}

bool WeldVerticesByDistance(RawMeshAsset& mesh,
                            const std::vector<int>& vertexIndices,
                            float distance,
                            std::vector<int>& weldedSelection,
                            std::string& error) {
    error.clear();
    if (!validateMesh(mesh, error)) {
        return false;
    }
    if (!(distance > 0.0f) || !std::isfinite(distance)) {
        error = "Weld distance must be greater than zero.";
        return false;
    }
    const std::vector<int> vertices =
        validatedUniqueIndices(vertexIndices, mesh.positions.size());
    if (vertices.size() < 2) {
        error = "Select at least two valid vertices to weld.";
        return false;
    }

    std::vector<int> parent(vertices.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto findRoot = [&](int index) {
        int root = index;
        while (parent[root] != root) {
            root = parent[root];
        }
        while (parent[index] != index) {
            const int next = parent[index];
            parent[index] = root;
            index = next;
        }
        return root;
    };
    const float distanceSquared = distance * distance;
    int mergeCount = 0;
    for (size_t a = 0; a < vertices.size(); ++a) {
        for (size_t b = a + 1; b < vertices.size(); ++b) {
            const glm::vec3 delta =
                mesh.positions[vertices[a]] - mesh.positions[vertices[b]];
            if (glm::dot(delta, delta) > distanceSquared) {
                continue;
            }
            const int rootA = findRoot(static_cast<int>(a));
            const int rootB = findRoot(static_cast<int>(b));
            if (rootA != rootB) {
                parent[rootB] = rootA;
                ++mergeCount;
            }
        }
    }
    if (mergeCount == 0) {
        error = "No selected vertices are within the weld distance.";
        return false;
    }

    RawMeshAsset result = mesh;
    std::unordered_map<int, std::vector<int>> groups;
    for (size_t i = 0; i < vertices.size(); ++i) {
        groups[findRoot(static_cast<int>(i))].push_back(vertices[i]);
    }

    std::vector<uint32_t> remap(result.positions.size());
    std::iota(remap.begin(), remap.end(), 0u);
    std::vector<uint32_t> representatives;
    for (const auto& entry : groups) {
        const std::vector<int>& group = entry.second;
        if (group.size() < 2) {
            continue;
        }
        const uint32_t representative =
            static_cast<uint32_t>(*std::min_element(group.begin(), group.end()));
        glm::vec3 position(0.0f);
        glm::vec2 uv(0.0f);
        for (int vertex : group) {
            position += result.positions[vertex];
            if (vertex < static_cast<int>(result.uvs.size())) {
                uv += result.uvs[vertex];
            }
            remap[vertex] = representative;
        }
        position /= static_cast<float>(group.size());
        uv /= static_cast<float>(group.size());
        result.positions[representative] = position;
        if (representative < result.uvs.size()) {
            result.uvs[representative] = uv;
        }
        representatives.push_back(representative);
    }

    std::vector<glm::u32vec3> faces;
    std::vector<uint32_t> materials;
    std::vector<uint32_t> islands;
    faces.reserve(result.faces.size());
    materials.reserve(result.faces.size());
    islands.reserve(result.faces.size());
    std::unordered_set<std::string> uniqueFaces;
    for (size_t fi = 0; fi < result.faces.size(); ++fi) {
        glm::u32vec3 face = result.faces[fi];
        face = glm::u32vec3(remap[face.x], remap[face.y], remap[face.z]);
        if (face.x == face.y || face.y == face.z || face.z == face.x) {
            continue;
        }
        std::array<uint32_t, 3> sorted = {face.x, face.y, face.z};
        std::sort(sorted.begin(), sorted.end());
        const std::string key = std::to_string(sorted[0]) + ":" +
                                std::to_string(sorted[1]) + ":" +
                                std::to_string(sorted[2]);
        if (!uniqueFaces.insert(key).second) {
            continue;
        }
        faces.push_back(face);
        materials.push_back(
            fi < result.faceMaterialIndices.size()
                ? result.faceMaterialIndices[fi]
                : 0u);
        if (!result.faceIslandIds.empty()) {
            islands.push_back(fi < result.faceIslandIds.size()
                                  ? result.faceIslandIds[fi]
                                  : 0u);
        }
    }
    if (faces.empty()) {
        error = "Welding would remove every face in the mesh.";
        return false;
    }
    result.faces = std::move(faces);
    result.faceMaterialIndices = std::move(materials);
    if (!result.faceIslandIds.empty()) {
        result.faceIslandIds = std::move(islands);
    }
    compactUnusedVertices(result);
    updateNormalsFromWinding(result);

    weldedSelection.clear();
    for (uint32_t representative : representatives) {
        const glm::vec3 position = mesh.positions[representative];
        int closest = -1;
        float bestDistance = FLT_MAX;
        for (int vi = 0; vi < static_cast<int>(result.positions.size()); ++vi) {
            const glm::vec3 delta = result.positions[vi] - position;
            const float d = glm::dot(delta, delta);
            if (d < bestDistance) {
                bestDistance = d;
                closest = vi;
            }
        }
        if (closest >= 0 &&
            std::find(weldedSelection.begin(), weldedSelection.end(), closest) ==
                weldedSelection.end()) {
            weldedSelection.push_back(closest);
        }
    }
    mesh = std::move(result);
    return true;
}

bool DissolveVertices(RawMeshAsset& mesh,
                      const std::vector<int>& vertexIndices,
                      std::string& error) {
    error.clear();
    if (!validateMesh(mesh, error)) {
        return false;
    }
    const std::vector<int> selected =
        validatedUniqueIndices(vertexIndices, mesh.positions.size());
    if (selected.empty()) {
        error = "Select at least one valid vertex to dissolve.";
        return false;
    }

    RawMeshAsset result = mesh;
    std::vector<uint8_t> removeFace(result.faces.size(), 0u);
    struct Replacement {
        glm::u32vec3 face;
        uint32_t material = 0u;
        uint32_t island = 0u;
    };
    std::vector<Replacement> replacements;
    int dissolved = 0;

    for (int vertex : selected) {
        std::vector<int> incident;
        std::vector<uint32_t> neighbors;
        for (int fi = 0; fi < static_cast<int>(result.faces.size()); ++fi) {
            if (removeFace[fi]) {
                continue;
            }
            const glm::u32vec3& face = result.faces[fi];
            if (face.x != static_cast<uint32_t>(vertex) &&
                face.y != static_cast<uint32_t>(vertex) &&
                face.z != static_cast<uint32_t>(vertex)) {
                continue;
            }
            incident.push_back(fi);
            const uint32_t tri[3] = {face.x, face.y, face.z};
            for (uint32_t candidate : tri) {
                if (candidate != static_cast<uint32_t>(vertex) &&
                    std::find(neighbors.begin(), neighbors.end(), candidate) ==
                        neighbors.end()) {
                    neighbors.push_back(candidate);
                }
            }
        }
        if (incident.size() != 3 || neighbors.size() != 3) {
            continue;
        }

        const uint32_t material =
            incident[0] < static_cast<int>(result.faceMaterialIndices.size())
                ? result.faceMaterialIndices[incident[0]]
                : 0u;
        bool compatible = true;
        glm::vec3 referenceNormal(0.0f);
        for (int fi : incident) {
            const uint32_t faceMaterial =
                fi < static_cast<int>(result.faceMaterialIndices.size())
                    ? result.faceMaterialIndices[fi]
                    : 0u;
            if (faceMaterial != material) {
                compatible = false;
                break;
            }
            referenceNormal += unnormalizedFaceNormal(result, result.faces[fi]);
        }
        if (!compatible || glm::length(referenceNormal) <= kGeometryEpsilon) {
            continue;
        }

        glm::u32vec3 replacement(neighbors[0], neighbors[1], neighbors[2]);
        std::array<uint32_t, 3> replacementVertices = {
            replacement.x, replacement.y, replacement.z};
        std::sort(replacementVertices.begin(), replacementVertices.end());
        bool replacementAlreadyExists = false;
        for (int fi = 0; fi < static_cast<int>(result.faces.size()); ++fi) {
            if (std::find(incident.begin(), incident.end(), fi) !=
                incident.end()) {
                continue;
            }
            const glm::u32vec3& other = result.faces[fi];
            std::array<uint32_t, 3> otherVertices = {
                other.x, other.y, other.z};
            std::sort(otherVertices.begin(), otherVertices.end());
            if (otherVertices == replacementVertices) {
                replacementAlreadyExists = true;
                break;
            }
        }
        if (replacementAlreadyExists) {
            continue;
        }
        glm::vec3 replacementNormal =
            unnormalizedFaceNormal(result, replacement);
        if (glm::length(replacementNormal) <= kGeometryEpsilon) {
            continue;
        }
        if (glm::dot(replacementNormal, referenceNormal) < 0.0f) {
            std::swap(replacement.y, replacement.z);
        }
        for (int fi : incident) {
            removeFace[fi] = 1u;
        }
        replacements.push_back(
            {replacement, material,
             (!result.faceIslandIds.empty() &&
              incident[0] < static_cast<int>(result.faceIslandIds.size()))
                 ? result.faceIslandIds[incident[0]]
                 : 0u});
        ++dissolved;
    }

    if (dissolved == 0) {
        error =
            "No selected vertex has a supported manifold valence-3 one-ring. "
            "More complex vertex dissolves require polygon topology.";
        return false;
    }

    std::vector<glm::u32vec3> faces;
    std::vector<uint32_t> materials;
    std::vector<uint32_t> islands;
    for (size_t fi = 0; fi < result.faces.size(); ++fi) {
        if (removeFace[fi]) {
            continue;
        }
        faces.push_back(result.faces[fi]);
        materials.push_back(fi < result.faceMaterialIndices.size()
                                ? result.faceMaterialIndices[fi]
                                : 0u);
        if (!result.faceIslandIds.empty()) {
            islands.push_back(fi < result.faceIslandIds.size()
                                  ? result.faceIslandIds[fi]
                                  : 0u);
        }
    }
    for (const Replacement& replacement : replacements) {
        faces.push_back(replacement.face);
        materials.push_back(replacement.material);
        if (!result.faceIslandIds.empty()) {
            islands.push_back(replacement.island);
        }
    }
    result.faces = std::move(faces);
    result.faceMaterialIndices = std::move(materials);
    if (!result.faceIslandIds.empty()) {
        result.faceIslandIds = std::move(islands);
    }
    compactUnusedVertices(result);
    updateNormalsFromWinding(result);
    mesh = std::move(result);
    return true;
}

bool SmoothVertices(RawMeshAsset& mesh,
                    const std::vector<int>& vertexIndices,
                    float factor,
                    int iterations,
                    std::string& error) {
    error.clear();
    if (!validateMesh(mesh, error)) {
        return false;
    }
    const std::vector<int> selected =
        validatedUniqueIndices(vertexIndices, mesh.positions.size());
    if (selected.empty()) {
        error = "Select at least one valid vertex to smooth.";
        return false;
    }
    if (!std::isfinite(factor) || factor <= 0.0f || factor > 1.0f) {
        error = "Smooth factor must be in the range (0, 1].";
        return false;
    }
    iterations = std::clamp(iterations, 1, 100);

    std::vector<std::vector<uint32_t>> adjacency(mesh.positions.size());
    for (const glm::u32vec3& face : mesh.faces) {
        const uint32_t tri[3] = {face.x, face.y, face.z};
        for (int edge = 0; edge < 3; ++edge) {
            const uint32_t a = tri[edge];
            const uint32_t b = tri[(edge + 1) % 3];
            adjacency[a].push_back(b);
            adjacency[b].push_back(a);
        }
    }
    for (auto& neighbors : adjacency) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()),
                        neighbors.end());
    }

    RawMeshAsset result = mesh;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        std::vector<glm::vec3> next = result.positions;
        for (int vertex : selected) {
            if (adjacency[vertex].empty()) {
                continue;
            }
            glm::vec3 average(0.0f);
            for (uint32_t neighbor : adjacency[vertex]) {
                average += result.positions[neighbor];
            }
            average /= static_cast<float>(adjacency[vertex].size());
            next[vertex] =
                glm::mix(result.positions[vertex], average, factor);
        }
        result.positions = std::move(next);
    }
    updateNormalsFromWinding(result);
    mesh = std::move(result);
    return true;
}

bool FlattenVertices(RawMeshAsset& mesh,
                     const std::vector<int>& vertexIndices,
                     FlattenMode mode,
                     std::string& error) {
    error.clear();
    if (!validateMesh(mesh, error)) {
        return false;
    }
    const std::vector<int> selected =
        validatedUniqueIndices(vertexIndices, mesh.positions.size());
    if (selected.size() < 2) {
        error = "Select at least two valid vertices to flatten.";
        return false;
    }

    RawMeshAsset result = mesh;
    glm::vec3 center(0.0f);
    for (int vertex : selected) {
        center += result.positions[vertex];
    }
    center /= static_cast<float>(selected.size());

    if (mode == FlattenMode::X || mode == FlattenMode::Y ||
        mode == FlattenMode::Z) {
        const int axis = mode == FlattenMode::X ? 0
                         : mode == FlattenMode::Y ? 1
                                                  : 2;
        for (int vertex : selected) {
            result.positions[vertex][axis] = center[axis];
        }
    } else {
        std::unordered_set<int> selectedSet(selected.begin(), selected.end());
        glm::vec3 planeNormal(0.0f);
        for (const glm::u32vec3& face : result.faces) {
            const int selectedCorners =
                static_cast<int>(selectedSet.count(static_cast<int>(face.x))) +
                static_cast<int>(selectedSet.count(static_cast<int>(face.y))) +
                static_cast<int>(selectedSet.count(static_cast<int>(face.z)));
            if (selectedCorners >= 2) {
                planeNormal += unnormalizedFaceNormal(result, face);
            }
        }
        if (glm::length(planeNormal) <= kGeometryEpsilon) {
            error =
                "An average plane could not be determined from the selected "
                "topology.";
            return false;
        }
        planeNormal = glm::normalize(planeNormal);
        for (int vertex : selected) {
            glm::vec3& position = result.positions[vertex];
            position -= planeNormal * glm::dot(position - center, planeNormal);
        }
    }
    updateNormalsFromWinding(result);
    mesh = std::move(result);
    return true;
}

std::vector<glm::u32vec2> BuildCanonicalEdges(const RawMeshAsset& mesh) {
    std::vector<glm::u32vec2> edges;
    edges.reserve(mesh.faces.size() * 3);
    std::unordered_set<uint64_t> seen;
    for (const glm::u32vec3& face : mesh.faces) {
        if (!faceIsValid(mesh, face)) {
            continue;
        }
        const uint32_t tri[3] = {face.x, face.y, face.z};
        for (int edge = 0; edge < 3; ++edge) {
            const uint32_t a = tri[edge];
            const uint32_t b = tri[(edge + 1) % 3];
            if (seen.insert(edgeKey(a, b)).second) {
                edges.emplace_back(std::min(a, b), std::max(a, b));
            }
        }
    }
    return edges;
}

bool SelectBoundaryEdges(const RawMeshAsset& mesh,
                         const std::vector<glm::u32vec2>& canonicalEdges,
                         std::vector<int>& edgeSelection,
                         std::string& error) {
    error.clear();
    if (!validateMesh(mesh, error)) {
        return false;
    }
    std::unordered_map<uint64_t, int> useCount;
    useCount.reserve(mesh.faces.size() * 3);
    for (const glm::u32vec3& face : mesh.faces) {
        ++useCount[edgeKey(face.x, face.y)];
        ++useCount[edgeKey(face.y, face.z)];
        ++useCount[edgeKey(face.z, face.x)];
    }
    edgeSelection.clear();
    for (int edge = 0; edge < static_cast<int>(canonicalEdges.size()); ++edge) {
        const glm::u32vec2& value = canonicalEdges[edge];
        if (useCount[edgeKey(value.x, value.y)] == 1) {
            edgeSelection.push_back(edge);
        }
    }
    if (edgeSelection.empty()) {
        error = "The mesh has no open boundary edges.";
        return false;
    }
    return true;
}

bool SelectSimilarFaces(const RawMeshAsset& mesh,
                        const std::vector<int>& seedFaces,
                        float maxAngleDegrees,
                        bool coplanarOnly,
                        std::vector<int>& faceSelection,
                        std::string& error) {
    error.clear();
    if (!validateMesh(mesh, error)) {
        return false;
    }
    const std::vector<int> seeds =
        validatedUniqueIndices(seedFaces, mesh.faces.size());
    if (seeds.empty()) {
        error = "Select at least one valid seed face.";
        return false;
    }
    const float cosineThreshold =
        std::cos(glm::radians(std::clamp(maxAngleDegrees, 0.0f, 180.0f)));
    const float planeTolerance =
        std::max(1e-5f, glm::length(mesh.boundsMax - mesh.boundsMin) * 1e-4f);

    struct SeedPlane {
        glm::vec3 normal;
        float distance = 0.0f;
    };
    std::vector<SeedPlane> planes;
    for (int seed : seeds) {
        glm::vec3 normal =
            glm::normalize(unnormalizedFaceNormal(mesh, mesh.faces[seed]));
        planes.push_back(
            {normal, glm::dot(normal, mesh.positions[mesh.faces[seed].x])});
    }

    faceSelection.clear();
    for (int fi = 0; fi < static_cast<int>(mesh.faces.size()); ++fi) {
        const glm::u32vec3& face = mesh.faces[fi];
        const glm::vec3 normal =
            glm::normalize(unnormalizedFaceNormal(mesh, face));
        bool matches = false;
        for (const SeedPlane& plane : planes) {
            if (glm::dot(normal, plane.normal) < cosineThreshold) {
                continue;
            }
            if (coplanarOnly) {
                const float da =
                    std::abs(glm::dot(plane.normal, mesh.positions[face.x]) -
                             plane.distance);
                const float db =
                    std::abs(glm::dot(plane.normal, mesh.positions[face.y]) -
                             plane.distance);
                const float dc =
                    std::abs(glm::dot(plane.normal, mesh.positions[face.z]) -
                             plane.distance);
                if (da > planeTolerance || db > planeTolerance ||
                    dc > planeTolerance) {
                    continue;
                }
            }
            matches = true;
            break;
        }
        if (matches) {
            faceSelection.push_back(fi);
        }
    }
    if (faceSelection.empty()) {
        error = "No faces match the selection criteria.";
        return false;
    }
    return true;
}

} // namespace MeshEditOperations
