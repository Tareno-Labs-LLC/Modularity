#pragma once

#include "ScriptSdkCommon.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace Assemblage {

constexpr int kFormatVersion = 1;

constexpr const char* kAssetExtension = ".moduasm";
constexpr const char* kTilesetExtension = ".modutile";

constexpr const char* kKindGrid2D = "grid2d";
constexpr const char* kLayerKindTile = "tile";

constexpr int kDefaultChunkSize = 32;
constexpr int kMinChunkSize = 4;
constexpr int kMaxChunkSize = 256;

using Cell = uint32_t;
using TileId = uint32_t;

constexpr TileId kEmptyTile = 0;
constexpr uint32_t kTileIdMask = 0x00FFFFFFu;
constexpr uint32_t kCellFlagShift = 24;

enum CellFlags : uint32_t {
    CellFlag_None   = 0,
    CellFlag_FlipX  = 1u << 0,
    CellFlag_FlipY  = 1u << 1,
    CellFlag_Rot90  = 1u << 2
};

inline Cell MakeCell(TileId tile, uint32_t flags = CellFlag_None) {
    return (tile & kTileIdMask) | ((flags & 0x7u) << kCellFlagShift);
}
inline TileId CellTile(Cell cell) { return cell & kTileIdMask; }
inline uint32_t CellFlagsOf(Cell cell) { return (cell >> kCellFlagShift) & 0x7u; }
inline bool CellIsEmpty(Cell cell) { return CellTile(cell) == kEmptyTile; }

enum class TileCollision {
    None = 0,
    Box = 1,
    Circle = 2,
    Polygon = 3,
    Line = 4
};

enum class TileAnimationMode {
    Loop = 0,
    PingPong = 1,
    Once = 2
};

struct TileFrame {
    glm::ivec4 rect = glm::ivec4(0);
};

struct TileAnimation {
    float fps = 0.0f;
    TileAnimationMode mode = TileAnimationMode::Loop;
    std::vector<TileFrame> frames;

    bool animated() const { return fps > 0.0f && frames.size() > 1; }
};

struct TileDef {
    TileId id = kEmptyTile;
    std::string name;
    std::string sheetPath;
    TileFrame frame;
    TileAnimation animation;

    std::vector<std::string> tags;

    TileCollision collision = TileCollision::None;
    glm::vec2 collisionSize = glm::vec2(1.0f);
    glm::vec2 collisionOffset = glm::vec2(0.0f);
    float collisionRadius = 0.5f;
    std::vector<glm::vec2> collisionPoints;

    int sortingOffset = 0;
    glm::vec4 tint = glm::vec4(1.0f);

    std::string autoRuleSet;
    std::vector<TileId> variants;

    std::map<std::string, std::string> meta;
};

struct Tileset {
    int formatVersion = kFormatVersion;
    std::string tilesetId;
    std::string name;
    TileId nextTileId = 1;
    std::vector<TileDef> tiles;

    const TileDef* find(TileId id) const;
    TileDef* find(TileId id);
    bool valid() const { return !tilesetId.empty(); }
};

int ResolveAnimationFrame(const TileDef& tile, double timeSeconds);

struct ChunkCoord {
    int x = 0;
    int y = 0;

    bool operator==(const ChunkCoord& other) const { return x == other.x && y == other.y; }
};

struct ChunkCoordHash {
    size_t operator()(const ChunkCoord& c) const {
        return static_cast<size_t>((static_cast<uint64_t>(static_cast<uint32_t>(c.x)) << 32) ^
                                   static_cast<uint32_t>(c.y));
    }
};

struct Chunk {
    ChunkCoord coord;
    std::vector<Cell> cells;

    bool empty() const;
};

using ChunkMap = std::unordered_map<ChunkCoord, Chunk, ChunkCoordHash>;

struct Layer {
    int id = -1;
    std::string name;
    std::string kind = kLayerKindTile;

    int order = 0;
    int sortingOrder = 0;
    float opacity = 1.0f;
    glm::vec4 tint = glm::vec4(1.0f);
    bool visible = true;
    bool locked = false;
    bool collisionEnabled = true;

    ChunkMap chunks;
};

struct Asset {
    int formatVersion = kFormatVersion;
    std::string assemblageId;
    std::string name;
    std::string kind = kKindGrid2D;

    int chunkSize = kDefaultChunkSize;
    glm::vec2 cellSize = glm::vec2(1.0f);
    glm::vec2 origin = glm::vec2(0.0f);

    int nextLayerId = 1;
    std::string tilesetPath;

    std::vector<Layer> layers;

    bool valid() const { return !assemblageId.empty() && kind == kKindGrid2D; }

    const Layer* findLayer(int layerId) const;
    Layer* findLayer(int layerId);

    Layer& addLayer(const std::string& layerName);
};

std::string GenerateAssemblageId();
std::string GenerateTilesetId();
bool IsWellFormedId(const std::string& id, const char* prefix);

ChunkCoord CellToChunk(int cellX, int cellY, int chunkSize);
int CellIndexInChunk(int cellX, int cellY, int chunkSize);

Cell GetCell(const Asset& asset, int layerId, int cellX, int cellY);

bool SetCell(Asset& asset, int layerId, int cellX, int cellY, Cell value,
             ChunkCoord* outChunk = nullptr);

glm::vec2 CellToWorld(const Asset& asset, int cellX, int cellY);
void WorldToCell(const Asset& asset, const glm::vec2& world, int& outCellX, int& outCellY);

bool LayerCellBounds(const Asset& asset, int layerId,
                     int& outMinX, int& outMinY, int& outMaxX, int& outMaxY);

bool SaveAsset(const fs::path& path, const Asset& asset, std::string& outError);
bool LoadAsset(const fs::path& path, Asset& outAsset, std::string& outError);

bool WriteAssetStream(std::ostream& out, const Asset& asset, std::string& outError);
bool ReadAssetStream(std::istream& in, Asset& outAsset, std::string& outError);

bool SaveTileset(const fs::path& path, const Tileset& tileset, std::string& outError);
bool LoadTileset(const fs::path& path, Tileset& outTileset, std::string& outError);

bool WriteTilesetStream(std::ostream& out, const Tileset& tileset, std::string& outError);
bool ReadTilesetStream(std::istream& in, Tileset& outTileset, std::string& outError);

template <typename T>
class ParsedCache {
public:
    using Loader = bool (*)(const fs::path&, T&, std::string&);

    explicit ParsedCache(Loader loader) : loader(loader) {}

    const T* get(const fs::path& path, std::string& outError) {
        outError.clear();
        const std::string key = path.string();
        std::error_code ec;
        const fs::file_time_type modified = fs::last_write_time(path, ec);
        if (ec) {
            outError = "Cannot stat " + key;
            return nullptr;
        }

        auto it = entries.find(key);
        if (it != entries.end() && it->second.modified == modified) {
            return &it->second.value;
        }

        Entry entry;
        if (!loader(path, entry.value, outError)) {
            return nullptr;
        }
        entry.modified = modified;
        Entry& slot = entries[key];
        slot = std::move(entry);
        return &slot.value;
    }

    void invalidate(const fs::path& path) { entries.erase(path.string()); }
    void clear() { entries.clear(); }
    size_t size() const { return entries.size(); }

private:
    struct Entry {
        T value;
        fs::file_time_type modified{};
    };
    Loader loader = nullptr;
    std::unordered_map<std::string, Entry> entries;
};

using AssetCache = ParsedCache<Asset>;
using TilesetCache = ParsedCache<Tileset>;

AssetCache MakeAssetCache();
TilesetCache MakeTilesetCache();

}  // namespace Assemblage
