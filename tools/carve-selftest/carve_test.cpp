// Standalone test for MapCarve: builds a hollow-box room (inward-facing walls
// like a generated Map Maker room, 4x3x4, wall thickness via an outer shell),
// carves a doorway, and validates the result topology.
#include "MapCarve.h"
#include <cstdio>
#include <map>
#include <set>

static int failures = 0;
#define CHECK(cond, msg)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                  \
            ++failures;                                                                  \
        }                                                                                \
    } while (0)

// Two parallel quad walls facing each other (like the inner and outer skin of
// one room wall): front wall at z=0 facing +Z, back wall at z=-0.3 facing -Z.
static RawMeshAsset MakeDoubleWall(float width, float height, float thickness) {
    RawMeshAsset mesh;
    auto addQuad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
        const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
        mesh.positions.push_back(a);
        mesh.positions.push_back(b);
        mesh.positions.push_back(c);
        mesh.positions.push_back(d);
        mesh.faces.push_back(glm::u32vec3(base, base + 1, base + 2));
        mesh.faces.push_back(glm::u32vec3(base, base + 2, base + 3));
    };
    // front wall facing +Z (CCW seen from +Z)
    addQuad({ -width / 2, 0, 0 }, { width / 2, 0, 0 }, { width / 2, height, 0 },
            { -width / 2, height, 0 });
    // back wall facing -Z (CCW seen from -Z)
    addQuad({ width / 2, 0, -thickness }, { -width / 2, 0, -thickness },
            { -width / 2, height, -thickness }, { width / 2, height, -thickness });
    mesh.faceMaterialIndices.assign(mesh.faces.size(), 0u);
    for (const auto& p : mesh.positions) {
        mesh.boundsMin = glm::min(mesh.boundsMin, p);
        mesh.boundsMax = glm::max(mesh.boundsMax, p);
    }
    return mesh;
}

static int CountBoundaryEdges(const RawMeshAsset& mesh) {
    std::map<std::pair<uint32_t, uint32_t>, int> use;
    for (const auto& f : mesh.faces) {
        const uint32_t tri[3] = { f.x, f.y, f.z };
        for (int e = 0; e < 3; ++e) {
            uint32_t a = tri[e], b = tri[(e + 1) % 3];
            use[{ std::min(a, b), std::max(a, b) }]++;
        }
    }
    int boundary = 0;
    for (const auto& entry : use) {
        if (entry.second == 1) ++boundary;
    }
    return boundary;
}

static bool MeshBasicValid(const RawMeshAsset& mesh, const char* label) {
    bool ok = true;
    for (const auto& f : mesh.faces) {
        if (f.x >= mesh.positions.size() || f.y >= mesh.positions.size() ||
            f.z >= mesh.positions.size()) {
            std::printf("FAIL: %s: out-of-range face index\n", label);
            ok = false;
        } else if (f.x == f.y || f.y == f.z || f.x == f.z) {
            std::printf("FAIL: %s: degenerate face\n", label);
            ok = false;
        }
    }
    if (mesh.faceMaterialIndices.size() != mesh.faces.size()) {
        std::printf("FAIL: %s: material count mismatch\n", label);
        ok = false;
    }
    return ok;
}

static float TotalArea(const RawMeshAsset& mesh) {
    float area = 0.0f;
    for (const auto& f : mesh.faces) {
        area += 0.5f * glm::length(glm::cross(mesh.positions[f.y] - mesh.positions[f.x],
                                              mesh.positions[f.z] - mesh.positions[f.x]));
    }
    return area;
}

int main() {
    using namespace MapCarve;

    // ---- through-cut rectangle -------------------------------------------
    {
        RawMeshAsset mesh = MakeDoubleWall(4.0f, 3.0f, 0.3f);
        const float areaBefore = TotalArea(mesh);
        CarveOptions options;
        options.cutThrough = true;
        options.newFaceMaterial = 0;
        // front face 0's plane basis: normal +Z, U/V in-plane
        PlaneBasis basis;
        std::string err;
        CHECK(BuildPlaneBasisFromFace(mesh, 0, basis, &err), "basis from front face");
        // door 1.0 x 2.0 centered at bottom middle of the wall
        const glm::vec2 centerUV = basis.toPlane(glm::vec3(0.0f, 1.05f, 0.0f));
        const CarveShape door = MakeRectShape(centerUV, glm::vec2(0.5f, 1.0f));
        CHECK(ShapeFitsIsland(mesh, GatherCoplanarIsland(mesh, 0), basis, door, &err),
              "door fits wall");
        CarveOutcome result = CarveShapeIntoMesh(mesh, 0, door, options);
        if (!result.success) std::printf("carve error: %s\n", result.error.c_str());
        CHECK(result.success, "through carve succeeds");
        CHECK(MeshBasicValid(mesh, "through carve"), "valid mesh after through carve");
        CHECK(result.frontRimVerts.size() == 4, "4 front rim verts");
        CHECK(result.backRimVerts.size() == 4, "4 back rim verts");
        // both wall skins keep their outer boundary (2 quads = 8 edges) and the
        // tunnel seals the hole => boundary edge count unchanged (8)
        CHECK(CountBoundaryEdges(mesh) == 8, "tunnel seals the opening");
        const float areaAfter = TotalArea(mesh);
        // removed 2x door area from walls, added tunnel 2*(1+2)*0.3
        const float expected = areaBefore - 2.0f * (1.0f * 2.0f) + 2.0f * (1.0f + 2.0f) * 0.3f;
        CHECK(std::fabs(areaAfter - expected) < 0.05f, "surface area matches expectation");
        std::printf("through-cut: %zu verts, %zu faces, area %.3f (expected %.3f)\n",
                    mesh.positions.size(), mesh.faces.size(), areaAfter, expected);
    }

    // ---- pocket cut -------------------------------------------------------
    {
        RawMeshAsset mesh = MakeDoubleWall(4.0f, 3.0f, 0.3f);
        CarveOptions options;
        options.cutThrough = false;
        options.pocketDepth = 0.1f;
        PlaneBasis basis;
        std::string err;
        BuildPlaneBasisFromFace(mesh, 0, basis, &err);
        const glm::vec2 centerUV = basis.toPlane(glm::vec3(0.8f, 1.5f, 0.0f));
        const CarveShape niche = MakeRectShape(centerUV, glm::vec2(0.3f, 0.4f));
        CarveOutcome result = CarveShapeIntoMesh(mesh, 0, niche, options);
        if (!result.success) std::printf("carve error: %s\n", result.error.c_str());
        CHECK(result.success, "pocket carve succeeds");
        CHECK(MeshBasicValid(mesh, "pocket carve"), "valid mesh after pocket carve");
        CHECK(CountBoundaryEdges(mesh) == 8, "pocket is sealed");
        std::printf("pocket: %zu verts, %zu faces\n", mesh.positions.size(), mesh.faces.size());
    }

    // ---- framed through-cut ----------------------------------------------
    {
        RawMeshAsset mesh = MakeDoubleWall(4.0f, 3.0f, 0.3f);
        CarveOptions options;
        options.cutThrough = true;
        options.createFrame = true;
        options.frameThickness = 0.1f;
        PlaneBasis basis;
        std::string err;
        BuildPlaneBasisFromFace(mesh, 0, basis, &err);
        const glm::vec2 centerUV = basis.toPlane(glm::vec3(0.0f, 1.2f, 0.0f));
        const CarveShape door = MakeRectShape(centerUV, glm::vec2(0.45f, 0.9f));
        CarveOutcome result = CarveShapeIntoMesh(mesh, 0, door, options);
        if (!result.success) std::printf("carve error: %s\n", result.error.c_str());
        CHECK(result.success, "framed carve succeeds");
        CHECK(MeshBasicValid(mesh, "framed carve"), "valid mesh after framed carve");
        CHECK(CountBoundaryEdges(mesh) == 8, "framed opening is sealed");
        std::printf("framed: %zu verts, %zu faces\n", mesh.positions.size(), mesh.faces.size());
    }

    // ---- circular (polygon) through-cut ----------------------------------
    {
        RawMeshAsset mesh = MakeDoubleWall(4.0f, 3.0f, 0.3f);
        CarveOptions options;
        options.cutThrough = true;
        PlaneBasis basis;
        std::string err;
        BuildPlaneBasisFromFace(mesh, 0, basis, &err);
        const glm::vec2 centerUV = basis.toPlane(glm::vec3(-1.0f, 2.0f, 0.0f));
        const CarveShape window = MakeEllipseShape(centerUV, glm::vec2(0.35f, 0.35f), 16);
        CarveOutcome result = CarveShapeIntoMesh(mesh, 0, window, options);
        if (!result.success) std::printf("carve error: %s\n", result.error.c_str());
        CHECK(result.success, "circular carve succeeds");
        CHECK(MeshBasicValid(mesh, "circular carve"), "valid mesh after circular carve");
        CHECK(CountBoundaryEdges(mesh) == 8, "circular opening is sealed");
        std::printf("circle: %zu verts, %zu faces\n", mesh.positions.size(), mesh.faces.size());
    }

    // ---- two openings in the same wall (island with existing hole) --------
    {
        RawMeshAsset mesh = MakeDoubleWall(4.0f, 3.0f, 0.3f);
        CarveOptions options;
        options.cutThrough = true;
        PlaneBasis basis;
        std::string err;
        BuildPlaneBasisFromFace(mesh, 0, basis, &err);
        CarveOutcome first = CarveShapeIntoMesh(
            mesh, 0, MakeRectShape(basis.toPlane(glm::vec3(-0.9f, 1.05f, 0.0f)), glm::vec2(0.4f, 0.9f)),
            options);
        CHECK(first.success, "first carve on shared wall succeeds");
        // find a face still on the front wall plane (z == 0, normal +Z)
        int frontFace = -1;
        for (size_t fi = 0; fi < mesh.faces.size(); ++fi) {
            const auto& f = mesh.faces[fi];
            const glm::vec3 n = glm::normalize(glm::cross(
                mesh.positions[f.y] - mesh.positions[f.x], mesh.positions[f.z] - mesh.positions[f.x]));
            if (n.z > 0.99f && std::fabs(mesh.positions[f.x].z) < 1e-4f &&
                mesh.positions[f.x].x > 0.0f) {
                frontFace = static_cast<int>(fi);
                break;
            }
        }
        CHECK(frontFace >= 0, "found front wall face for second carve");
        if (frontFace >= 0) {
            PlaneBasis basis2;
            BuildPlaneBasisFromFace(mesh, frontFace, basis2, &err);
            CarveOutcome second = CarveShapeIntoMesh(
                mesh, frontFace,
                MakeRectShape(basis2.toPlane(glm::vec3(0.9f, 1.05f, 0.0f)), glm::vec2(0.4f, 0.9f)),
                options);
            if (!second.success) std::printf("carve error: %s\n", second.error.c_str());
            CHECK(second.success, "second carve on same wall succeeds");
            CHECK(MeshBasicValid(mesh, "second carve"), "valid mesh after second carve");
            CHECK(CountBoundaryEdges(mesh) == 8, "both openings sealed");
        }
        std::printf("double-door: %zu verts, %zu faces\n", mesh.positions.size(),
                    mesh.faces.size());
    }

    // ---- rejections -------------------------------------------------------
    {
        RawMeshAsset mesh = MakeDoubleWall(4.0f, 3.0f, 0.3f);
        CarveOptions options;
        options.cutThrough = true;
        PlaneBasis basis;
        std::string err;
        BuildPlaneBasisFromFace(mesh, 0, basis, &err);
        // hangs off the edge of the wall
        CarveOutcome offEdge = CarveShapeIntoMesh(
            mesh, 0, MakeRectShape(basis.toPlane(glm::vec3(1.9f, 1.0f, 0.0f)), glm::vec2(0.5f, 0.5f)),
            options);
        CHECK(!offEdge.success, "carve off the wall edge is rejected");
        CHECK(MeshBasicValid(mesh, "rejected carve"), "mesh untouched after rejection");
        CHECK(mesh.faces.size() == 4, "face count unchanged after rejection");
        // no opposite wall within range
        RawMeshAsset single;
        single.positions = { { -1, 0, 0 }, { 1, 0, 0 }, { 1, 2, 0 }, { -1, 2, 0 } };
        single.faces = { { 0, 1, 2 }, { 0, 2, 3 } };
        single.faceMaterialIndices = { 0, 0 };
        PlaneBasis singleBasis;
        BuildPlaneBasisFromFace(single, 0, singleBasis, &err);
        CarveOutcome noBack = CarveShapeIntoMesh(
            single, 0,
            MakeRectShape(singleBasis.toPlane(glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec2(0.3f, 0.3f)),
            options);
        CHECK(!noBack.success, "through-cut without back wall is rejected");
        std::printf("rejection messages:\n  off-edge: %s\n  no-back: %s\n",
                    offEdge.error.c_str(), noBack.error.c_str());
    }

    if (failures == 0) {
        std::printf("ALL CARVE TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", failures);
    return 1;
}
