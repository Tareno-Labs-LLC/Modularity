#pragma once

#include "Assemblage.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class Renderer;

class AssemblageRuntime {
public:
    struct TileQuad {
        unsigned int textureId = 0;
        glm::vec2 worldMin = glm::vec2(0.0f);
        glm::vec2 worldMax = glm::vec2(0.0f);
        glm::vec2 uvMin = glm::vec2(0.0f);
        glm::vec2 uvMax = glm::vec2(1.0f);
        glm::vec4 tint = glm::vec4(1.0f);
        Assemblage::TileId animatedTile = Assemblage::kEmptyTile;
        int sheetWidth = 0;
        int sheetHeight = 0;
        uint32_t cellFlags = 0;
    };

    struct ChunkGeometry {
        std::vector<TileQuad> quads;
        glm::vec2 boundsMin = glm::vec2(0.0f);
        glm::vec2 boundsMax = glm::vec2(0.0f);
        bool hasAnimatedTiles = false;
        bool valid = false;
    };

    struct Entry {
        Assemblage::Asset asset;
        Assemblage::Tileset tileset;
        std::string tilesetSourcePath;
        bool loaded = false;
        bool hasUnsavedChanges = false;
        std::string loadError;
    };

    void setProjectRoot(const fs::path& root);
    const fs::path& projectRoot() const { return root; }

    fs::path resolvePath(const std::string& assetPath) const;
    std::string makeProjectRelative(const fs::path& absolutePath) const;

    Entry* acquire(const std::string& assetPath);
    const Entry* find(const std::string& assetPath) const;

    Assemblage::Asset* asset(const std::string& assetPath);
    const Assemblage::Tileset* tileset(const std::string& assetPath);

    Entry& adopt(const std::string& assetPath, Assemblage::Asset asset);

    void markChunkDirty(const std::string& assetPath, int layerId, Assemblage::ChunkCoord coord);
    void markCellDirty(const std::string& assetPath, int layerId, int cellX, int cellY,
                       bool includeNeighbours = true);
    void markLayerDirty(const std::string& assetPath, int layerId);
    void markAssetDirty(const std::string& assetPath);

    void markUnsaved(const std::string& assetPath, bool unsaved = true);
    bool hasUnsavedChanges(const std::string& assetPath) const;
    bool anyUnsavedChanges() const;

    const ChunkGeometry* chunkGeometry(const std::string& assetPath, int layerId,
                                       Assemblage::ChunkCoord coord, Renderer& renderer);

    bool save(const std::string& assetPath, std::string& outError);
    bool saveAllDirty(std::string& outError);

    void invalidateAllGeometry();
    void clear();

    size_t loadedAssetCount() const { return entries.size(); }
    size_t cachedChunkCount() const { return geometry.size(); }

private:
    struct ChunkKey {
        std::string assetPath;
        int layerId = -1;
        int chunkX = 0;
        int chunkY = 0;

        bool operator==(const ChunkKey& other) const {
            return layerId == other.layerId && chunkX == other.chunkX &&
                   chunkY == other.chunkY && assetPath == other.assetPath;
        }
    };
    struct ChunkKeyHash {
        size_t operator()(const ChunkKey& key) const {
            size_t h = std::hash<std::string>()(key.assetPath);
            h ^= static_cast<size_t>(key.layerId) * 0x9E3779B9u + (h << 6) + (h >> 2);
            h ^= static_cast<size_t>(key.chunkX) * 0x85EBCA6Bu + (h << 6) + (h >> 2);
            h ^= static_cast<size_t>(key.chunkY) * 0xC2B2AE35u + (h << 6) + (h >> 2);
            return h;
        }
    };

    void ensureTileset(Entry& entry);
    void buildChunkGeometry(const Entry& entry, const Assemblage::Layer& layer,
                            Assemblage::ChunkCoord coord, Renderer& renderer,
                            ChunkGeometry& out);

    fs::path root;
    std::unordered_map<std::string, Entry> entries;
    std::unordered_map<ChunkKey, ChunkGeometry, ChunkKeyHash> geometry;
};

bool ResolveTileUv(const Assemblage::TileDef& tile, int textureWidth, int textureHeight,
                   double timeSeconds, glm::vec2& outUvMin, glm::vec2& outUvMax);
