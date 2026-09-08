#pragma once

#include "Render25D/MMesh.h"

namespace Modularity::Render25D {

class MMeshLoader {
public:
    bool loadAsset(const std::string& path, MMeshAsset& outAsset, std::string& error) const;
    bool saveAsset(const MMeshAsset& asset, const std::string& path, std::string& error) const;
    bool buildRenderData(const MMeshAsset& asset, MMeshRenderData& outRenderData, std::string& error) const;
    bool loadRenderData(const std::string& path, MMeshRenderData& outRenderData, std::string& error) const;
};

class MMeshCache {
public:
    const MMeshRenderData* getOrLoad(const std::string& path, std::string& error);
    const MMeshRenderData* store(const std::string& path, const MMeshAsset& asset, std::string& error);
    void invalidate(const std::string& path);
    void clear();

private:
    MMeshLoader loader;
    std::unordered_map<std::string, std::shared_ptr<MMeshRenderData>> cache;
};

} // namespace Modularity::Render25D
