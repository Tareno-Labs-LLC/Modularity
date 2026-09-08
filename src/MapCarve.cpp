#include "MapCarve.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace MapCarve {

namespace {

constexpr float kEpsilon = 1e-6f;

float Cross2(const glm::vec2& a, const glm::vec2& b) {
    return a.x * b.y - a.y * b.x;
}

float SignedArea(const std::vector<glm::vec2>& points, const std::vector<int>& loop) {
    float area = 0.0f;
    for (size_t i = 0; i < loop.size(); ++i) {
        const glm::vec2& a = points[loop[i]];
        const glm::vec2& b = points[loop[(i + 1) % loop.size()]];
        area += Cross2(a, b);
    }
    return area * 0.5f;
}

float ShapeSignedArea(const CarveShape& shape) {
    float area = 0.0f;
    for (size_t i = 0; i < shape.size(); ++i) {
        area += Cross2(shape[i], shape[(i + 1) % shape.size()]);
    }
    return area * 0.5f;
}

bool PointInTriangle2D(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b,
                       const glm::vec2& c, float eps) {
    const float d1 = Cross2(b - a, p - a);
    const float d2 = Cross2(c - b, p - b);
    const float d3 = Cross2(a - c, p - c);
    const bool hasNeg = (d1 < -eps) || (d2 < -eps) || (d3 < -eps);
    const bool hasPos = (d1 > eps) || (d2 > eps) || (d3 > eps);
    return !(hasNeg && hasPos);
}

bool PointStrictlyInPolygon(const glm::vec2& p, const CarveShape& shape) {
    // winding-agnostic even-odd test
    bool inside = false;
    for (size_t i = 0, j = shape.size() - 1; i < shape.size(); j = i++) {
        const glm::vec2& a = shape[i];
        const glm::vec2& b = shape[j];
        if ((a.y > p.y) != (b.y > p.y)) {
            const float x = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
            if (p.x < x) inside = !inside;
        }
    }
    return inside;
}

bool SegmentsIntersect(const glm::vec2& p1, const glm::vec2& p2, const glm::vec2& q1,
                       const glm::vec2& q2) {
    const float d1 = Cross2(p2 - p1, q1 - p1);
    const float d2 = Cross2(p2 - p1, q2 - p1);
    const float d3 = Cross2(q2 - q1, p1 - q1);
    const float d4 = Cross2(q2 - q1, p2 - q1);
    if (((d1 > kEpsilon && d2 < -kEpsilon) || (d1 < -kEpsilon && d2 > kEpsilon)) &&
        ((d3 > kEpsilon && d4 < -kEpsilon) || (d3 < -kEpsilon && d4 > kEpsilon))) {
        return true;
    }
    return false;
}

glm::vec3 FaceNormal(const RawMeshAsset& mesh, const glm::u32vec3& face) {
    const glm::vec3& a = mesh.positions[face.x];
    const glm::vec3& b = mesh.positions[face.y];
    const glm::vec3& c = mesh.positions[face.z];
    const glm::vec3 n = glm::cross(b - a, c - a);
    const float len = glm::length(n);
    return len > kEpsilon ? n / len : glm::vec3(0.0f);
}

bool FaceIndicesValid(const RawMeshAsset& mesh, const glm::u32vec3& face) {
    const size_t count = mesh.positions.size();
    return face.x < count && face.y < count && face.z < count &&
           face.x != face.y && face.y != face.z && face.x != face.z;
}

uint64_t UndirectedEdgeKey(uint32_t a, uint32_t b) {
    return (static_cast<uint64_t>(std::min(a, b)) << 32) | static_cast<uint64_t>(std::max(a, b));
}

} // namespace

bool OffsetConvexPolygon(const CarveShape& shape, float offset, CarveShape& out) {
    const size_t n = shape.size();
    if (n < 3) return false;
    out.clear();
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const glm::vec2& prev = shape[(i + n - 1) % n];
        const glm::vec2& cur = shape[i];
        const glm::vec2& next = shape[(i + 1) % n];
        glm::vec2 dirA = cur - prev;
        glm::vec2 dirB = next - cur;
        const float lenA = glm::length(dirA);
        const float lenB = glm::length(dirB);
        if (lenA < kEpsilon || lenB < kEpsilon) return false;
        dirA /= lenA;
        dirB /= lenB;
        const glm::vec2 normalA(dirA.y, -dirA.x);
        const glm::vec2 normalB(dirB.y, -dirB.x);
        const glm::vec2 pointA = prev + normalA * offset;
        const glm::vec2 pointB = cur + normalB * offset;
        const float denom = Cross2(dirA, dirB);
        if (std::fabs(denom) < 1e-8f) {
            out.push_back(cur + normalA * offset);
            continue;
        }
        const float t = Cross2(pointB - pointA, dirB) / denom;
        out.push_back(pointA + dirA * t);
    }
    if (ShapeSignedArea(out) * ShapeSignedArea(shape) <= 0.0f) return false;
    return true;
}

namespace {

bool EarClip(const std::vector<glm::vec2>& points, std::vector<int> polygon,
             std::vector<glm::ivec3>& outTris) {
    if (polygon.size() < 3) return false;
    if (SignedArea(points, polygon) < 0.0f) {
        std::reverse(polygon.begin(), polygon.end());
    }
    int guard = static_cast<int>(polygon.size()) * static_cast<int>(polygon.size()) + 16;
    while (polygon.size() > 3 && guard-- > 0) {
        bool clipped = false;
        const size_t n = polygon.size();
        for (size_t i = 0; i < n; ++i) {
            const int prev = polygon[(i + n - 1) % n];
            const int cur = polygon[i];
            const int next = polygon[(i + 1) % n];
            const glm::vec2& a = points[prev];
            const glm::vec2& b = points[cur];
            const glm::vec2& c = points[next];
            if (Cross2(b - a, c - a) <= kEpsilon) continue;
            bool contains = false;
            for (size_t k = 0; k < n; ++k) {
                const int idx = polygon[k];
                if (idx == prev || idx == cur || idx == next) continue;
                const glm::vec2& p = points[idx];
                if ((p == a) || (p == b) || (p == c)) continue;
                if (PointInTriangle2D(p, a, b, c, kEpsilon)) {
                    contains = true;
                    break;
                }
            }
            if (contains) continue;
            outTris.push_back(glm::ivec3(prev, cur, next));
            polygon.erase(polygon.begin() + static_cast<long>(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            const size_t n2 = polygon.size();
            float bestCross = -FLT_MAX;
            size_t bestIndex = 0;
            for (size_t i = 0; i < n2; ++i) {
                const glm::vec2& a = points[polygon[(i + n2 - 1) % n2]];
                const glm::vec2& b = points[polygon[i]];
                const glm::vec2& c = points[polygon[(i + 1) % n2]];
                const float cross = Cross2(b - a, c - a);
                if (cross > bestCross) {
                    bestCross = cross;
                    bestIndex = i;
                }
            }
            if (bestCross <= 0.0f) return false;
            const size_t n3 = polygon.size();
            outTris.push_back(glm::ivec3(polygon[(bestIndex + n3 - 1) % n3], polygon[bestIndex],
                                         polygon[(bestIndex + 1) % n3]));
            polygon.erase(polygon.begin() + static_cast<long>(bestIndex));
        }
    }
    if (polygon.size() == 3) {
        outTris.push_back(glm::ivec3(polygon[0], polygon[1], polygon[2]));
    }
    return guard > 0;
}

bool TriangulateWithHoles(const std::vector<glm::vec2>& points, std::vector<int> outer,
                          std::vector<std::vector<int>> holes,
                          std::vector<glm::ivec3>& outTris) {
    if (SignedArea(points, outer) < 0.0f) {
        std::reverse(outer.begin(), outer.end());
    }
    for (std::vector<int>& hole : holes) {
        if (SignedArea(points, hole) > 0.0f) {
            std::reverse(hole.begin(), hole.end());
        }
    }
    std::sort(holes.begin(), holes.end(),
              [&](const std::vector<int>& a, const std::vector<int>& b) {
                  float maxA = -FLT_MAX, maxB = -FLT_MAX;
                  for (int idx : a) maxA = std::max(maxA, points[idx].x);
                  for (int idx : b) maxB = std::max(maxB, points[idx].x);
                  return maxA > maxB;
              });
    for (const std::vector<int>& hole : holes) {
        size_t holeAnchor = 0;
        for (size_t i = 1; i < hole.size(); ++i) {
            if (points[hole[i]].x > points[hole[holeAnchor]].x) holeAnchor = i;
        }
        const glm::vec2 anchorPoint = points[hole[holeAnchor]];
        std::vector<size_t> candidates(outer.size());
        for (size_t i = 0; i < outer.size(); ++i) candidates[i] = i;
        std::sort(candidates.begin(), candidates.end(), [&](size_t a, size_t b) {
            const glm::vec2 da = points[outer[a]] - anchorPoint;
            const glm::vec2 db = points[outer[b]] - anchorPoint;
            return glm::dot(da, da) < glm::dot(db, db);
        });
        auto bridgeBlocked = [&](const glm::vec2& target) {
            for (size_t i = 0; i < outer.size(); ++i) {
                const glm::vec2& a = points[outer[i]];
                const glm::vec2& b = points[outer[(i + 1) % outer.size()]];
                if (a == anchorPoint || b == anchorPoint || a == target || b == target) continue;
                if (SegmentsIntersect(anchorPoint, target, a, b)) return true;
            }
            for (size_t i = 0; i < hole.size(); ++i) {
                const glm::vec2& a = points[hole[i]];
                const glm::vec2& b = points[hole[(i + 1) % hole.size()]];
                if (a == anchorPoint || b == anchorPoint || a == target || b == target) continue;
                if (SegmentsIntersect(anchorPoint, target, a, b)) return true;
            }
            return false;
        };
        bool bridged = false;
        for (size_t candidate : candidates) {
            const glm::vec2 target = points[outer[candidate]];
            if (bridgeBlocked(target)) continue;
            std::vector<int> merged;
            merged.reserve(outer.size() + hole.size() + 2);
            for (size_t i = 0; i <= candidate; ++i) merged.push_back(outer[i]);
            for (size_t i = 0; i <= hole.size(); ++i) {
                merged.push_back(hole[(holeAnchor + i) % hole.size()]);
            }
            merged.push_back(outer[candidate]);
            for (size_t i = candidate + 1; i < outer.size(); ++i) merged.push_back(outer[i]);
            outer = std::move(merged);
            bridged = true;
            break;
        }
        if (!bridged) return false;
    }
    return EarClip(points, outer, outTris);
}

bool ExtractBoundaryLoops(const RawMeshAsset& mesh, const std::vector<int>& islandFaces,
                          std::vector<std::vector<uint32_t>>& outLoops, std::string* outError) {
    std::unordered_map<uint64_t, int> edgeUse;
    for (int fi : islandFaces) {
        const glm::u32vec3& f = mesh.faces[fi];
        const uint32_t tri[3] = { f.x, f.y, f.z };
        for (int e = 0; e < 3; ++e) {
            edgeUse[UndirectedEdgeKey(tri[e], tri[(e + 1) % 3])]++;
        }
    }
    std::unordered_map<uint32_t, uint32_t> nextVert;
    for (int fi : islandFaces) {
        const glm::u32vec3& f = mesh.faces[fi];
        const uint32_t tri[3] = { f.x, f.y, f.z };
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = tri[e];
            const uint32_t b = tri[(e + 1) % 3];
            if (edgeUse[UndirectedEdgeKey(a, b)] == 1) {
                if (nextVert.count(a) != 0) {
                    if (outError) *outError = "Wall region has a non-manifold boundary";
                    return false;
                }
                nextVert[a] = b;
            }
        }
    }
    std::unordered_set<uint32_t> visited;
    for (const auto& entry : nextVert) {
        if (visited.count(entry.first) != 0) continue;
        std::vector<uint32_t> loop;
        uint32_t current = entry.first;
        while (visited.insert(current).second) {
            loop.push_back(current);
            auto it = nextVert.find(current);
            if (it == nextVert.end()) {
                if (outError) *outError = "Wall region boundary does not close";
                return false;
            }
            current = it->second;
        }
        if (current != loop.front()) {
            if (outError) *outError = "Wall region boundary does not close";
            return false;
        }
        if (loop.size() >= 3) outLoops.push_back(std::move(loop));
    }
    if (outLoops.empty()) {
        if (outError) *outError = "Wall region has no boundary";
        return false;
    }
    return true;
}

struct AffineUvMap {
    glm::vec2 colU = glm::vec2(0.25f, 0.0f);
    glm::vec2 colV = glm::vec2(0.0f, 0.25f);
    glm::vec2 offset = glm::vec2(0.0f);
    glm::vec2 apply(const glm::vec2& q) const { return colU * q.x + colV * q.y + offset; }
};

AffineUvMap FitAffineUvMap(const RawMeshAsset& mesh, const glm::u32vec3& face,
                           const PlaneBasis& basis) {
    AffineUvMap map;
    if (!mesh.hasUVs || mesh.uvs.size() != mesh.positions.size()) return map;
    const glm::vec2 q0 = basis.toPlane(mesh.positions[face.x]);
    const glm::vec2 q1 = basis.toPlane(mesh.positions[face.y]);
    const glm::vec2 q2 = basis.toPlane(mesh.positions[face.z]);
    const glm::vec2 t0 = mesh.uvs[face.x];
    const glm::vec2 t1 = mesh.uvs[face.y];
    const glm::vec2 t2 = mesh.uvs[face.z];
    const glm::vec2 dq1 = q1 - q0;
    const glm::vec2 dq2 = q2 - q0;
    const float det = Cross2(dq1, dq2);
    if (std::fabs(det) < 1e-9f) return map;
    const glm::vec2 dt1 = t1 - t0;
    const glm::vec2 dt2 = t2 - t0;
    map.colU = (dt1 * dq2.y - dt2 * dq1.y) / det;
    map.colV = (dt2 * dq1.x - dt1 * dq2.x) / det;
    map.offset = t0 - (map.colU * q0.x + map.colV * q0.y);
    return map;
}

} // namespace

#pragma region Shapes
CarveShape MakeRectShape(const glm::vec2& center, const glm::vec2& halfSize) {
    return {
        center + glm::vec2(-halfSize.x, -halfSize.y),
        center + glm::vec2(halfSize.x, -halfSize.y),
        center + glm::vec2(halfSize.x, halfSize.y),
        center + glm::vec2(-halfSize.x, halfSize.y),
    };
}

CarveShape MakeEllipseShape(const glm::vec2& center, const glm::vec2& halfSize, int segments) {
    CarveShape shape;
    const int count = std::max(3, segments);
    shape.reserve(count);
    for (int i = 0; i < count; ++i) {
        const float angle = (static_cast<float>(i) / count) * 6.2831853f;
        shape.push_back(center + glm::vec2(std::cos(angle) * halfSize.x,
                                           std::sin(angle) * halfSize.y));
    }
    return shape;
}
#pragma endregion

#pragma region Plane / Island Queries
bool BuildPlaneBasisFromFace(const RawMeshAsset& mesh, int faceIndex, PlaneBasis& outBasis,
                             std::string* outError) {
    if (faceIndex < 0 || faceIndex >= static_cast<int>(mesh.faces.size())) {
        if (outError) *outError = "Face index out of range";
        return false;
    }
    const glm::u32vec3& face = mesh.faces[faceIndex];
    if (!FaceIndicesValid(mesh, face)) {
        if (outError) *outError = "Face references invalid vertices";
        return false;
    }
    const glm::vec3 normal = FaceNormal(mesh, face);
    if (glm::length(normal) < 0.5f) {
        if (outError) *outError = "Face has zero area";
        return false;
    }
    outBasis.origin = mesh.positions[face.x];
    outBasis.normal = normal;
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::fabs(glm::dot(up, normal)) > 0.9f) {
        up = glm::vec3(0.0f, 0.0f, 1.0f);
    }
    outBasis.axisU = glm::normalize(glm::cross(up, normal));
    outBasis.axisV = glm::normalize(glm::cross(normal, outBasis.axisU));
    return true;
}

std::vector<int> GatherCoplanarIsland(const RawMeshAsset& mesh, int faceIndex,
                                      float maxNormalAngleDeg, float maxPlaneDistance) {
    std::vector<int> island;
    if (faceIndex < 0 || faceIndex >= static_cast<int>(mesh.faces.size())) return island;
    if (!FaceIndicesValid(mesh, mesh.faces[faceIndex])) return island;

    const glm::vec3 seedNormal = FaceNormal(mesh, mesh.faces[faceIndex]);
    const float planeOffset = glm::dot(seedNormal, mesh.positions[mesh.faces[faceIndex].x]);
    const float cosTolerance = std::cos(glm::radians(maxNormalAngleDeg));

    std::unordered_map<uint64_t, std::vector<int>> edgeFaces;
    for (size_t fi = 0; fi < mesh.faces.size(); ++fi) {
        const glm::u32vec3& f = mesh.faces[fi];
        if (!FaceIndicesValid(mesh, f)) continue;
        const uint32_t tri[3] = { f.x, f.y, f.z };
        for (int e = 0; e < 3; ++e) {
            edgeFaces[UndirectedEdgeKey(tri[e], tri[(e + 1) % 3])].push_back(static_cast<int>(fi));
        }
    }

    auto faceCoplanar = [&](int fi) {
        const glm::u32vec3& f = mesh.faces[fi];
        if (!FaceIndicesValid(mesh, f)) return false;
        const glm::vec3 n = FaceNormal(mesh, f);
        if (glm::dot(n, seedNormal) < cosTolerance) return false;
        const uint32_t tri[3] = { f.x, f.y, f.z };
        for (int k = 0; k < 3; ++k) {
            if (std::fabs(glm::dot(seedNormal, mesh.positions[tri[k]]) - planeOffset) >
                maxPlaneDistance) {
                return false;
            }
        }
        return true;
    };

    std::unordered_set<int> inIsland;
    std::deque<int> queue;
    inIsland.insert(faceIndex);
    queue.push_back(faceIndex);
    while (!queue.empty()) {
        const int current = queue.front();
        queue.pop_front();
        island.push_back(current);
        const glm::u32vec3& f = mesh.faces[current];
        const uint32_t tri[3] = { f.x, f.y, f.z };
        for (int e = 0; e < 3; ++e) {
            const auto it = edgeFaces.find(UndirectedEdgeKey(tri[e], tri[(e + 1) % 3]));
            if (it == edgeFaces.end()) continue;
            for (int neighbor : it->second) {
                if (inIsland.count(neighbor) != 0) continue;
                if (!faceCoplanar(neighbor)) continue;
                inIsland.insert(neighbor);
                queue.push_back(neighbor);
            }
        }
    }
    return island;
}

bool ShapeFitsIsland(const RawMeshAsset& mesh, const std::vector<int>& islandFaces,
                     const PlaneBasis& basis, const CarveShape& shape, std::string* outReason) {
    if (shape.size() < 3) {
        if (outReason) *outReason = "Opening needs at least 3 points";
        return false;
    }
    if (std::fabs(ShapeSignedArea(shape)) < 1e-6f) {
        if (outReason) *outReason = "Opening has no area";
        return false;
    }
    if (islandFaces.empty()) {
        if (outReason) *outReason = "No wall surface selected";
        return false;
    }

    std::vector<std::array<glm::vec2, 3>> tris;
    tris.reserve(islandFaces.size());
    for (int fi : islandFaces) {
        const glm::u32vec3& f = mesh.faces[fi];
        tris.push_back({ basis.toPlane(mesh.positions[f.x]), basis.toPlane(mesh.positions[f.y]),
                         basis.toPlane(mesh.positions[f.z]) });
    }
    auto insideIsland = [&](const glm::vec2& p) {
        for (const auto& tri : tris) {
            if (PointInTriangle2D(p, tri[0], tri[1], tri[2], 1e-5f)) return true;
        }
        return false;
    };

    const int samplesPerEdge = 6;
    for (size_t i = 0; i < shape.size(); ++i) {
        const glm::vec2& a = shape[i];
        const glm::vec2& b = shape[(i + 1) % shape.size()];
        for (int s = 0; s < samplesPerEdge; ++s) {
            const float t = static_cast<float>(s) / samplesPerEdge;
            if (!insideIsland(a + (b - a) * t)) {
                if (outReason) *outReason = "Opening reaches outside the wall surface";
                return false;
            }
        }
    }
    std::vector<std::vector<uint32_t>> loops;
    std::string loopError;
    if (!ExtractBoundaryLoops(mesh, islandFaces, loops, &loopError)) {
        if (outReason) *outReason = loopError;
        return false;
    }
    for (const auto& loop : loops) {
        for (uint32_t v : loop) {
            if (PointStrictlyInPolygon(basis.toPlane(mesh.positions[v]), shape)) {
                if (outReason) *outReason = "Opening overlaps the wall edge or an existing hole";
                return false;
            }
        }
    }
    return true;
}
#pragma endregion

#pragma region Carve
namespace {

uint32_t AppendVertex(RawMeshAsset& mesh, const glm::vec3& position, const glm::vec3& normal,
                      const glm::vec2& uv) {
    const uint32_t index = static_cast<uint32_t>(mesh.positions.size());
    mesh.positions.push_back(position);
    if (mesh.normals.size() == mesh.positions.size() - 1) {
        mesh.normals.push_back(normal);
    }
    if (mesh.uvs.size() == mesh.positions.size() - 1) {
        mesh.uvs.push_back(uv);
    }
    return index;
}

void AppendFace(RawMeshAsset& mesh, const glm::u32vec3& face, uint32_t material,
                std::vector<int>* newFaceIndices) {
    if (newFaceIndices != nullptr) {
        newFaceIndices->push_back(static_cast<int>(mesh.faces.size()));
    }
    mesh.faces.push_back(face);
    mesh.faceMaterialIndices.push_back(material);
    if (!mesh.faceIslandIds.empty()) {
        mesh.faceIslandIds.push_back(0u);
    }
}

void AppendOrientedQuad(RawMeshAsset& mesh, uint32_t a0, uint32_t a1, uint32_t b1, uint32_t b0,
                        const glm::vec3& desiredNormal, uint32_t material,
                        std::vector<int>* newFaceIndices) {
    const glm::vec3 n = glm::cross(mesh.positions[a1] - mesh.positions[a0],
                                   mesh.positions[b1] - mesh.positions[a0]);
    if (glm::dot(n, desiredNormal) >= 0.0f) {
        AppendFace(mesh, glm::u32vec3(a0, a1, b1), material, newFaceIndices);
        AppendFace(mesh, glm::u32vec3(a0, b1, b0), material, newFaceIndices);
    } else {
        AppendFace(mesh, glm::u32vec3(a1, a0, b0), material, newFaceIndices);
        AppendFace(mesh, glm::u32vec3(a1, b0, b1), material, newFaceIndices);
    }
}

struct WallCutResult {
    bool success = false;
    std::string error;
    std::vector<uint32_t> holeVerts;
};

WallCutResult CutHoleIntoIsland(RawMeshAsset& mesh, const std::vector<int>& islandFaces,
                                const PlaneBasis& basis, const CarveShape& shape,
                                std::vector<uint8_t>& removeFaces,
                                std::vector<int>* newFaceIndices) {
    WallCutResult result;
    if (islandFaces.empty()) {
        result.error = "No wall faces to cut";
        return result;
    }
    std::vector<std::vector<uint32_t>> loops;
    if (!ExtractBoundaryLoops(mesh, islandFaces, loops, &result.error)) {
        return result;
    }
    std::vector<glm::vec2> points;
    std::vector<uint32_t> pointToMeshVert;
    std::vector<std::vector<int>> loopIndexLists;
    for (const auto& loop : loops) {
        std::vector<int> indexList;
        indexList.reserve(loop.size());
        for (uint32_t v : loop) {
            indexList.push_back(static_cast<int>(points.size()));
            points.push_back(basis.toPlane(mesh.positions[v]));
            pointToMeshVert.push_back(v);
        }
        loopIndexLists.push_back(std::move(indexList));
    }
    size_t outerIndex = 0;
    float bestArea = -FLT_MAX;
    for (size_t i = 0; i < loopIndexLists.size(); ++i) {
        const float area = std::fabs(SignedArea(points, loopIndexLists[i]));
        if (area > bestArea) {
            bestArea = area;
            outerIndex = i;
        }
    }
    std::vector<int> outer = loopIndexLists[outerIndex];
    std::vector<std::vector<int>> holes;
    for (size_t i = 0; i < loopIndexLists.size(); ++i) {
        if (i != outerIndex) holes.push_back(loopIndexLists[i]);
    }

    const glm::u32vec3 seedFace = mesh.faces[islandFaces.front()];
    const uint32_t islandMaterial =
        islandFaces.front() < static_cast<int>(mesh.faceMaterialIndices.size())
            ? mesh.faceMaterialIndices[islandFaces.front()]
            : 0u;
    const AffineUvMap uvMap = FitAffineUvMap(mesh, seedFace, basis);
    std::vector<int> holeIndexList;
    holeIndexList.reserve(shape.size());
    for (const glm::vec2& p : shape) {
        const uint32_t meshVert =
            AppendVertex(mesh, basis.toWorld(p), basis.normal, uvMap.apply(p));
        result.holeVerts.push_back(meshVert);
        holeIndexList.push_back(static_cast<int>(points.size()));
        points.push_back(p);
        pointToMeshVert.push_back(meshVert);
    }
    holes.push_back(holeIndexList);

    std::vector<glm::ivec3> tris;
    if (!TriangulateWithHoles(points, outer, holes, tris)) {
        result.error = "Failed to retriangulate the wall around the opening";
        return result;
    }
    for (int fi : islandFaces) {
        if (fi >= 0 && fi < static_cast<int>(removeFaces.size())) removeFaces[fi] = 1u;
    }
    for (const glm::ivec3& tri : tris) {
        AppendFace(mesh,
                   glm::u32vec3(pointToMeshVert[tri.x], pointToMeshVert[tri.y],
                                pointToMeshVert[tri.z]),
                   islandMaterial, newFaceIndices);
    }
    result.success = true;
    return result;
}

float RayTriangle(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& a,
                  const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 e1 = b - a;
    const glm::vec3 e2 = c - a;
    const glm::vec3 p = glm::cross(dir, e2);
    const float det = glm::dot(e1, p);
    if (std::fabs(det) < 1e-9f) return -1.0f;
    const float invDet = 1.0f / det;
    const glm::vec3 tv = origin - a;
    const float u = glm::dot(tv, p) * invDet;
    if (u < -1e-4f || u > 1.0f + 1e-4f) return -1.0f;
    const glm::vec3 q = glm::cross(tv, e1);
    const float v = glm::dot(dir, q) * invDet;
    if (v < -1e-4f || u + v > 1.0f + 1e-4f) return -1.0f;
    const float t = glm::dot(e2, q) * invDet;
    return t > 1e-5f ? t : -1.0f;
}

} // namespace

CarveOutcome CarveShapeIntoMesh(RawMeshAsset& mesh, int frontFaceIndex,
                                const CarveShape& shapeInput, const CarveOptions& options) {
    CarveOutcome outcome;
    RawMeshAsset work = mesh;

    if (work.normals.size() != work.positions.size()) {
        work.normals.assign(work.positions.size(), glm::vec3(0.0f, 1.0f, 0.0f));
    }
    if (work.uvs.size() != work.positions.size()) {
        work.uvs.assign(work.positions.size(), glm::vec2(0.0f));
        work.hasUVs = true;
    }
    if (work.faceMaterialIndices.size() != work.faces.size()) {
        work.faceMaterialIndices.assign(work.faces.size(), 0u);
    }

    CarveShape shape = shapeInput;
    if (shape.size() < 3) {
        outcome.error = "Opening needs at least 3 points";
        return outcome;
    }
    if (ShapeSignedArea(shape) < 0.0f) {
        std::reverse(shape.begin(), shape.end());
    }

    PlaneBasis front;
    if (!BuildPlaneBasisFromFace(work, frontFaceIndex, front, &outcome.error)) {
        return outcome;
    }
    const std::vector<int> frontIsland = GatherCoplanarIsland(work, frontFaceIndex);
    if (!ShapeFitsIsland(work, frontIsland, front, shape, &outcome.error)) {
        return outcome;
    }

    CarveShape wallHole = shape;
    const bool useFrame = options.createFrame && options.frameThickness > 1e-4f;
    if (useFrame) {
        if (!OffsetConvexPolygon(shape, options.frameThickness, wallHole)) {
            outcome.error = "Frame thickness is invalid for this opening";
            return outcome;
        }
        if (!ShapeFitsIsland(work, frontIsland, front, wallHole, &outcome.error)) {
            outcome.error = "Opening plus frame does not fit the wall: " + outcome.error;
            return outcome;
        }
    }

    std::vector<int> backIsland;
    PlaneBasis back;
    float wallThickness = 0.0f;
    if (options.cutThrough) {
        glm::vec2 centroid(0.0f);
        for (const glm::vec2& p : shape) centroid += p;
        centroid /= static_cast<float>(shape.size());
        const glm::vec3 rayOrigin = front.toWorld(centroid) - front.normal * 1e-3f;
        const glm::vec3 rayDir = -front.normal;
        std::unordered_set<int> frontSet(frontIsland.begin(), frontIsland.end());
        float bestT = FLT_MAX;
        int backFace = -1;
        for (size_t fi = 0; fi < work.faces.size(); ++fi) {
            if (frontSet.count(static_cast<int>(fi)) != 0) continue;
            const glm::u32vec3& f = work.faces[fi];
            if (!FaceIndicesValid(work, f)) continue;
            const float t = RayTriangle(rayOrigin, rayDir, work.positions[f.x],
                                        work.positions[f.y], work.positions[f.z]);
            if (t > 0.0f && t < bestT && t <= options.maxThroughDistance) {
                const glm::vec3 n = FaceNormal(work, f);
                if (glm::dot(n, front.normal) < -0.985f) {
                    bestT = t;
                    backFace = static_cast<int>(fi);
                }
            }
        }
        if (backFace < 0) {
            outcome.error =
                "No parallel opposite wall found behind this surface; disable Cut Through to "
                "carve a pocket instead";
            return outcome;
        }
        wallThickness = bestT + 1e-3f;
        back.normal = -front.normal;
        back.axisU = front.axisU;
        back.axisV = -front.axisV;
        back.origin = front.origin - front.normal * wallThickness;
        backIsland = GatherCoplanarIsland(work, backFace);
        const glm::u32vec3& bf = work.faces[backFace];
        const float backOffset =
            glm::dot(front.normal, work.positions[bf.x] - front.origin);
        wallThickness = -backOffset;
        if (wallThickness < 1e-4f) {
            outcome.error = "Opposite wall is not behind the carved surface";
            return outcome;
        }
        back.origin = front.origin - front.normal * wallThickness;
    }

    auto projectToBack = [&](const glm::vec2& frontUV) {
        return back.toPlane(front.toWorld(frontUV) - front.normal * wallThickness);
    };

    CarveShape backShape;
    CarveShape backWallHole;
    if (options.cutThrough) {
        backShape.reserve(shape.size());
        for (const glm::vec2& p : shape) backShape.push_back(projectToBack(p));
        backWallHole.reserve(wallHole.size());
        for (const glm::vec2& p : wallHole) backWallHole.push_back(projectToBack(p));
        if (!ShapeFitsIsland(work, backIsland, back, backWallHole, &outcome.error)) {
            outcome.error = "Opening does not fit the opposite wall: " + outcome.error;
            return outcome;
        }
    }

    std::vector<uint8_t> removeFaces(work.faces.size(), 0u);
    std::vector<int> newFaceIndices;
    WallCutResult frontCut =
        CutHoleIntoIsland(work, frontIsland, front, wallHole, removeFaces, &newFaceIndices);
    if (!frontCut.success) {
        outcome.error = frontCut.error;
        return outcome;
    }
    WallCutResult backCut;
    if (options.cutThrough) {
        backCut = CutHoleIntoIsland(work, backIsland, back, backWallHole, removeFaces,
                                    &newFaceIndices);
        if (!backCut.success) {
            outcome.error = backCut.error;
            return outcome;
        }
    }

    const uint32_t carveMaterial = options.newFaceMaterial;
    const glm::vec3 axisCenter = front.toWorld([&]() {
        glm::vec2 c(0.0f);
        for (const glm::vec2& p : shape) c += p;
        return c / static_cast<float>(shape.size());
    }());

    std::vector<float> perimeter(shape.size() + 1, 0.0f);
    for (size_t i = 0; i < shape.size(); ++i) {
        perimeter[i + 1] =
            perimeter[i] + glm::length(shape[(i + 1) % shape.size()] - shape[i]);
    }

    std::vector<uint32_t> frontInnerRim;
    std::vector<uint32_t> backInnerRim;
    if (useFrame) {
        for (size_t i = 0; i < shape.size(); ++i) {
            frontInnerRim.push_back(AppendVertex(work, front.toWorld(shape[i]), front.normal,
                                                 glm::vec2(perimeter[i], 0.0f)));
        }
        if (options.cutThrough) {
            for (size_t i = 0; i < shape.size(); ++i) {
                backInnerRim.push_back(
                    AppendVertex(work, front.toWorld(shape[i]) - front.normal * wallThickness,
                                 back.normal, glm::vec2(perimeter[i], wallThickness)));
            }
        }
    } else {
        frontInnerRim = frontCut.holeVerts;
        backInnerRim = backCut.holeVerts;
    }

    const size_t rimCount = frontInnerRim.size();
    auto quadRing = [&](const std::vector<uint32_t>& loopA, const std::vector<uint32_t>& loopB,
                        const glm::vec3& desiredNormal, bool towardAxis) {
        for (size_t i = 0; i < rimCount; ++i) {
            const size_t j = (i + 1) % rimCount;
            glm::vec3 wanted = desiredNormal;
            if (towardAxis) {
                const glm::vec3 mid = (work.positions[loopA[i]] + work.positions[loopA[j]] +
                                       work.positions[loopB[i]] + work.positions[loopB[j]]) *
                                      0.25f;
                glm::vec3 toAxis = axisCenter - mid;
                toAxis -= front.normal * glm::dot(toAxis, front.normal);
                if (glm::length(toAxis) > kEpsilon) wanted = toAxis;
            }
            AppendOrientedQuad(work, loopA[i], loopA[j], loopB[j], loopB[i], wanted,
                               carveMaterial, &newFaceIndices);
        }
    };

    if (useFrame) {
        quadRing(frontCut.holeVerts, frontInnerRim, front.normal, false);
        if (options.cutThrough) {
            quadRing(backCut.holeVerts, backInnerRim, back.normal, false);
        }
    }

    if (options.cutThrough) {
        quadRing(frontInnerRim, backInnerRim, front.normal, true);
        outcome.backRimVerts = backInnerRim;
    } else {
        const float depth = std::max(0.01f, options.pocketDepth + options.surfaceOffset);
        std::vector<uint32_t> pocketRim;
        for (size_t i = 0; i < rimCount; ++i) {
            pocketRim.push_back(
                AppendVertex(work, work.positions[frontInnerRim[i]] - front.normal * depth,
                             front.normal, glm::vec2(perimeter[i], depth)));
        }
        quadRing(frontInnerRim, pocketRim, front.normal, true);
        std::vector<glm::vec2> floorPoints;
        std::vector<int> floorLoop;
        for (size_t i = 0; i < rimCount; ++i) {
            floorLoop.push_back(static_cast<int>(floorPoints.size()));
            floorPoints.push_back(shape[i]);
        }
        std::vector<glm::ivec3> floorTris;
        if (!EarClip(floorPoints, floorLoop, floorTris)) {
            outcome.error = "Failed to build the pocket floor";
            return outcome;
        }
        for (const glm::ivec3& tri : floorTris) {
            AppendFace(work,
                       glm::u32vec3(pocketRim[tri.x], pocketRim[tri.y], pocketRim[tri.z]),
                       carveMaterial, &newFaceIndices);
        }
        outcome.backRimVerts.clear();
    }
    outcome.frontRimVerts = frontInnerRim;

    {
        std::vector<glm::u32vec3> keptFaces;
        std::vector<uint32_t> keptMaterials;
        std::vector<uint32_t> keptIslands;
        keptFaces.reserve(work.faces.size());
        keptMaterials.reserve(work.faces.size());
        std::vector<int> faceRemap(work.faces.size(), -1);
        for (size_t fi = 0; fi < work.faces.size(); ++fi) {
            if (fi < removeFaces.size() && removeFaces[fi] != 0u) continue;
            faceRemap[fi] = static_cast<int>(keptFaces.size());
            keptFaces.push_back(work.faces[fi]);
            keptMaterials.push_back(fi < work.faceMaterialIndices.size()
                                        ? work.faceMaterialIndices[fi]
                                        : 0u);
            if (!work.faceIslandIds.empty()) {
                keptIslands.push_back(fi < work.faceIslandIds.size() ? work.faceIslandIds[fi]
                                                                     : 0u);
            }
        }
        for (int& idx : newFaceIndices) {
            idx = (idx >= 0 && idx < static_cast<int>(faceRemap.size())) ? faceRemap[idx] : -1;
        }
        newFaceIndices.erase(std::remove(newFaceIndices.begin(), newFaceIndices.end(), -1),
                             newFaceIndices.end());
        work.faces = std::move(keptFaces);
        work.faceMaterialIndices = std::move(keptMaterials);
        if (!work.faceIslandIds.empty()) {
            work.faceIslandIds = std::move(keptIslands);
        }
    }

    for (const glm::u32vec3& face : work.faces) {
        if (face.x >= work.positions.size() || face.y >= work.positions.size() ||
            face.z >= work.positions.size()) {
            outcome.error = "Carve produced invalid face indices";
            return outcome;
        }
    }
    work.boundsMin = glm::vec3(FLT_MAX);
    work.boundsMax = glm::vec3(-FLT_MAX);
    for (const glm::vec3& p : work.positions) {
        work.boundsMin = glm::min(work.boundsMin, p);
        work.boundsMax = glm::max(work.boundsMax, p);
    }
    work.hasNormals = true;
    work.hasUVs = true;

    outcome.newFaceIndices = std::move(newFaceIndices);
    outcome.success = true;
    mesh = std::move(work);
    return outcome;
}
#pragma endregion

} // namespace MapCarve
