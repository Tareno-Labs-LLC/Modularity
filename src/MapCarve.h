#pragma once

#include "ModelLoader.h"
#include <string>
#include <vector>

namespace MapCarve {

struct PlaneBasis {
    glm::vec3 origin = glm::vec3(0.0f);
    glm::vec3 normal = glm::vec3(0.0f, 0.0f, 1.0f);
    glm::vec3 axisU = glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 axisV = glm::vec3(0.0f, 1.0f, 0.0f);

    glm::vec2 toPlane(const glm::vec3& p) const {
        const glm::vec3 d = p - origin;
        return glm::vec2(glm::dot(d, axisU), glm::dot(d, axisV));
    }
    glm::vec3 toWorld(const glm::vec2& uv) const {
        return origin + axisU * uv.x + axisV * uv.y;
    }
};

using CarveShape = std::vector<glm::vec2>;

CarveShape MakeRectShape(const glm::vec2& center, const glm::vec2& halfSize);
CarveShape MakeEllipseShape(const glm::vec2& center, const glm::vec2& halfSize, int segments);

bool OffsetConvexPolygon(const CarveShape& shape, float offset, CarveShape& out);

bool BuildPlaneBasisFromFace(const RawMeshAsset& mesh, int faceIndex,
                             PlaneBasis& outBasis, std::string* outError = nullptr);

std::vector<int> GatherCoplanarIsland(const RawMeshAsset& mesh, int faceIndex,
                                      float maxNormalAngleDeg = 4.0f,
                                      float maxPlaneDistance = 0.005f);

bool ShapeFitsIsland(const RawMeshAsset& mesh, const std::vector<int>& islandFaces,
                     const PlaneBasis& basis, const CarveShape& shape,
                     std::string* outReason = nullptr);

struct CarveOptions {
    bool cutThrough = true;
    float pocketDepth = 0.2f;
    float maxThroughDistance = 8.0f;
    bool createFrame = false;
    float frameThickness = 0.08f;
    float surfaceOffset = 0.0f;
    uint32_t newFaceMaterial = 0;
};

struct CarveOutcome {
    bool success = false;
    std::string error;
    std::vector<uint32_t> frontRimVerts;
    std::vector<uint32_t> backRimVerts;
    std::vector<int> newFaceIndices;
};

CarveOutcome CarveShapeIntoMesh(RawMeshAsset& mesh, int frontFaceIndex,
                                const CarveShape& shapeUV, const CarveOptions& options);

} // namespace MapCarve
