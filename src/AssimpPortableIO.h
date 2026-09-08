#pragma once

#include "../include/Platform/AssetSource.h"

#include <assimp/DefaultIOSystem.h>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>

#include <cstring>
#include <string>

namespace Modularity {

class PortablePathIOSystem final : public Assimp::IOSystem {
public:
    bool Exists(const char* file) const override {
        return mBackend.Exists(Rescue(file).c_str());
    }

    char getOsSeparator() const override { return mBackend.getOsSeparator(); }

    Assimp::IOStream* Open(const char* file, const char* mode = "rb") override {
        if (mode != nullptr && std::strchr(mode, 'w') != nullptr) {
            return mBackend.Open(file, mode);
        }
        return mBackend.Open(Rescue(file).c_str(), mode);
    }

    void Close(Assimp::IOStream* file) override { mBackend.Close(file); }

    bool ComparePaths(const char* one, const char* second) const override {
        return mBackend.ComparePaths(one, second);
    }

private:
    static std::string Rescue(const char* file) {
        if (file == nullptr) return std::string();
        return Modularity::Platform::ResolveAssetPath(file);
    }

    Assimp::DefaultIOSystem mBackend;
};

inline void AttachPortableAssetIO(Assimp::Importer& importer) {
    importer.SetIOHandler(new PortablePathIOSystem());
}

} // namespace Modularity
