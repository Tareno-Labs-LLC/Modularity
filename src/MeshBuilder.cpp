#include "MeshBuilder.h"
#pragma region State Reset
void MeshBuilder::clear() {
    mesh = RawMeshAsset();
    hasMesh = false;
    dirty = false;
    selectedVertex = -1;
    loadedPath.clear();
}
#pragma endregion
#pragma region File IO
bool MeshBuilder::load(const std::string& path, std::string& error) {
    auto& loader = getModelLoader();
    RawMeshAsset loaded;
    if (!loader.loadRawMesh(path, loaded, error)) return false;
    mesh = std::move(loaded);
    hasMesh = true;
    dirty = false;
    loadedPath = path;
    selectedVertex = mesh.positions.empty() ? -1 : 0;
    return true;
}
bool MeshBuilder::save(const std::string& path, std::string& error) {
    if (!hasMesh) {error = "No mesh loaded"; return false; }
    auto& loader = getModelLoader();
    if (!loader.saveRawMesh(mesh, path, error)) return false;
    loadedPath = path;
    dirty = false;
    return true;
}
#pragma endregion
#pragma region Geometry Editing
void MeshBuilder::recomputeNormals() {
    if (mesh.positions.empty()) return;
    mesh.normals.assign(mesh.positions.size(), glm::vec3(0.0f));
    for (const auto& face : mesh.faces) {
        if (face.x >= mesh.positions.size() ||
            face.y >= mesh.positions.size() ||
            face.z >= mesh.positions.size()) continue;
        const glm::vec3& a = mesh.positions[face.x];
        const glm::vec3& b = mesh.positions[face.y];
        const glm::vec3& c = mesh.positions[face.z];
        glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        mesh.normals[face.x] += n;
        mesh.normals[face.y] += n;
        mesh.normals[face.z] += n;
    }
    for (auto& n : mesh.normals) {if (glm::length(n) > 1e-6f) n = glm::normalize(n);}
    mesh.hasNormals = true;
    dirty = true;
}
void MeshBuilder::flipFaces() {
    if (!hasMesh || mesh.faces.empty()) return;
    for (glm::u32vec3& face : mesh.faces) std::swap(face.y, face.z);
    recomputeNormals();
}
bool MeshBuilder::addFace(const std::vector<uint32_t>& indices, std::string& error) {
    if (indices.size() < 3) {error = "Need at least 3 vertices to form a face"; return false;}
    for (uint32_t idx : indices) {if (idx >= mesh.positions.size()) {error = "Vertex index out of range"; return false;}}
    auto addTri = [&](uint32_t a, uint32_t b, uint32_t c) {mesh.faces.push_back(glm::u32vec3(a, b, c));};
    if (indices.size() == 3) addTri(indices[0], indices[1], indices[2]);
    else { // Context: Split polygon into triangles using vertex 0 as the anchor (WHAT DID I EVEN MEAN BY FAN TRIANGULATION IN THE FIRST PLACE???)
            /*Fan triangulation for n-gon*/ for (size_t i = 1; i + 1 < indices.size(); i++) addTri(indices[0], indices[i], indices[i + 1]);
    }
    /*Update bounds*/ mesh.boundsMin = glm::vec3(FLT_MAX);
    mesh.boundsMax = glm::vec3(-FLT_MAX);
    for (const auto& p : mesh.positions) {
        mesh.boundsMin.x = std::min(mesh.boundsMin.x, p.x);
        mesh.boundsMin.y = std::min(mesh.boundsMin.y, p.y);
        mesh.boundsMin.z = std::min(mesh.boundsMin.z, p.z);
        mesh.boundsMax.x = std::max(mesh.boundsMax.x, p.x);
        mesh.boundsMax.y = std::max(mesh.boundsMax.y, p.y);
        mesh.boundsMax.z = std::max(mesh.boundsMax.z, p.z);
    }
    recomputeNormals();
    return true;
}
#pragma endregion
