#pragma once
#include "ModelLoader.h"
class MeshBuilder {
public:
    bool hasMesh = false;
    RawMeshAsset mesh;
    std::string loadedPath;
    bool dirty = false;
    int selectedVertex = -1;
    bool load(const std::string& path, std::string& error);
    bool save(const std::string& path, std::string& error);
    void clear();
    void recomputeNormals();
    void flipFaces();
    bool addFace(const std::vector<uint32_t>& indices, std::string& error);
};
