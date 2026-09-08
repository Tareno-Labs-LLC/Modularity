#pragma once

#include "ModelLoader.h"

#include <string>
#include <vector>

namespace MeshEditOperations {

enum class FlattenMode {
    X,
    Y,
    Z,
    AveragePlane,
};

bool FlipFaces(RawMeshAsset& mesh, const std::vector<int>& faceIndices,
               std::string& error);
bool RecalculateNormals(RawMeshAsset& mesh, bool pointInside,
                        std::string& error);
bool WeldVerticesByDistance(RawMeshAsset& mesh,
                            const std::vector<int>& vertexIndices,
                            float distance,
                            std::vector<int>& weldedSelection,
                            std::string& error);
bool DissolveVertices(RawMeshAsset& mesh,
                      const std::vector<int>& vertexIndices,
                      std::string& error);
bool SmoothVertices(RawMeshAsset& mesh,
                    const std::vector<int>& vertexIndices,
                    float factor,
                    int iterations,
                    std::string& error);
bool FlattenVertices(RawMeshAsset& mesh,
                     const std::vector<int>& vertexIndices,
                     FlattenMode mode,
                     std::string& error);

std::vector<glm::u32vec2> BuildCanonicalEdges(const RawMeshAsset& mesh);
bool SelectBoundaryEdges(const RawMeshAsset& mesh,
                         const std::vector<glm::u32vec2>& canonicalEdges,
                         std::vector<int>& edgeSelection,
                         std::string& error);
bool SelectSimilarFaces(const RawMeshAsset& mesh,
                        const std::vector<int>& seedFaces,
                        float maxAngleDegrees,
                        bool coplanarOnly,
                        std::vector<int>& faceSelection,
                        std::string& error);

} // namespace MeshEditOperations
