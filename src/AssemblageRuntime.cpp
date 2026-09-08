#include "AssemblageRuntime.h"

#include "Rendering.h"

#include <algorithm>

namespace {

glm::vec2 CellWorldMin(const Assemblage::Asset& asset, int cellX, int cellY) {
    return Assemblage::CellToWorld(asset, cellX, cellY);
}

}  // namespace

bool ResolveTileUv(const Assemblage::TileDef& tile, int textureWidth, int textureHeight,
                   double timeSeconds, glm::vec2& outUvMin, glm::vec2& outUvMax) {
    if (textureWidth <= 0 || textureHeight <= 0) return false;

    glm::ivec4 rect = tile.frame.rect;
    if (tile.animation.animated()) {
        const int frame = Assemblage::ResolveAnimationFrame(tile, timeSeconds);
        if (frame >= 0 && frame < static_cast<int>(tile.animation.frames.size())) {
            rect = tile.animation.frames[static_cast<size_t>(frame)].rect;
        }
    }

    if (rect.z <= 0 || rect.w <= 0) {
        outUvMin = glm::vec2(0.0f);
        outUvMax = glm::vec2(1.0f);
        return true;
    }

    const float tw = static_cast<float>(textureWidth);
    const float th = static_cast<float>(textureHeight);
    outUvMin = glm::vec2(static_cast<float>(rect.x) / tw, static_cast<float>(rect.y) / th);
    outUvMax = glm::vec2(static_cast<float>(rect.x + rect.z) / tw,
                         static_cast<float>(rect.y + rect.w) / th);
    return true;
}

void AssemblageRuntime::setProjectRoot(const fs::path& newRoot) {
    if (root == newRoot) return;
    root = newRoot;
    clear();
}

fs::path AssemblageRuntime::resolvePath(const std::string& assetPath) const {
    if (assetPath.empty()) return {};
    fs::path path(assetPath);
    if (path.is_absolute()) return path;
    if (root.empty()) return path;
    return root / path;
}

std::string AssemblageRuntime::makeProjectRelative(const fs::path& absolutePath) const {
    if (root.empty()) return absolutePath.generic_string();
    std::error_code ec;
    const fs::path relative = fs::relative(absolutePath, root, ec);
    if (ec || relative.empty()) return absolutePath.generic_string();
    return relative.generic_string();
}

void AssemblageRuntime::ensureTileset(Entry& entry) {
    const std::string& wanted = entry.asset.tilesetPath;
    if (wanted == entry.tilesetSourcePath) return;

    entry.tileset = Assemblage::Tileset{};
    entry.tilesetSourcePath = wanted;
    if (wanted.empty()) return;

    std::string error;
    if (!Assemblage::LoadTileset(resolvePath(wanted), entry.tileset, error)) {
        entry.loadError = error;
    }
}

AssemblageRuntime::Entry* AssemblageRuntime::acquire(const std::string& assetPath) {
    if (assetPath.empty()) return nullptr;

    auto it = entries.find(assetPath);
    if (it != entries.end()) {
        if (!it->second.loaded) return nullptr;
        ensureTileset(it->second);
        return &it->second;
    }

    Entry entry;
    std::string error;
    if (!Assemblage::LoadAsset(resolvePath(assetPath), entry.asset, error)) {
        entry.loaded = false;
        entry.loadError = error;
        entries.emplace(assetPath, std::move(entry));
        return nullptr;
    }
    entry.loaded = true;
    ensureTileset(entry);
    auto inserted = entries.emplace(assetPath, std::move(entry));
    return &inserted.first->second;
}

const AssemblageRuntime::Entry* AssemblageRuntime::find(const std::string& assetPath) const {
    auto it = entries.find(assetPath);
    return it == entries.end() ? nullptr : &it->second;
}

Assemblage::Asset* AssemblageRuntime::asset(const std::string& assetPath) {
    Entry* entry = acquire(assetPath);
    return entry ? &entry->asset : nullptr;
}

const Assemblage::Tileset* AssemblageRuntime::tileset(const std::string& assetPath) {
    Entry* entry = acquire(assetPath);
    return entry ? &entry->tileset : nullptr;
}

AssemblageRuntime::Entry& AssemblageRuntime::adopt(const std::string& assetPath,
                                                   Assemblage::Asset newAsset) {
    Entry entry;
    entry.asset = std::move(newAsset);
    entry.loaded = true;
    entry.hasUnsavedChanges = true;
    ensureTileset(entry);
    entries[assetPath] = std::move(entry);
    markAssetDirty(assetPath);
    return entries[assetPath];
}

void AssemblageRuntime::markChunkDirty(const std::string& assetPath, int layerId,
                                       Assemblage::ChunkCoord coord) {
    ChunkKey key;
    key.assetPath = assetPath;
    key.layerId = layerId;
    key.chunkX = coord.x;
    key.chunkY = coord.y;
    geometry.erase(key);
}

void AssemblageRuntime::markCellDirty(const std::string& assetPath, int layerId,
                                      int cellX, int cellY, bool includeNeighbours) {
    const Entry* entry = find(assetPath);
    const int chunkSize = (entry && entry->loaded) ? entry->asset.chunkSize
                                                   : Assemblage::kDefaultChunkSize;

    markChunkDirty(assetPath, layerId, Assemblage::CellToChunk(cellX, cellY, chunkSize));
    if (!includeNeighbours) return;

    const int localX = ((cellX % chunkSize) + chunkSize) % chunkSize;
    const int localY = ((cellY % chunkSize) + chunkSize) % chunkSize;
    const int stepX = (localX == 0) ? -1 : (localX == chunkSize - 1 ? 1 : 0);
    const int stepY = (localY == 0) ? -1 : (localY == chunkSize - 1 ? 1 : 0);
    if (stepX == 0 && stepY == 0) return;

    const Assemblage::ChunkCoord base = Assemblage::CellToChunk(cellX, cellY, chunkSize);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            if (dx != 0 && dx != stepX) continue;
            if (dy != 0 && dy != stepY) continue;
            Assemblage::ChunkCoord neighbour;
            neighbour.x = base.x + dx;
            neighbour.y = base.y + dy;
            markChunkDirty(assetPath, layerId, neighbour);
        }
    }
}

void AssemblageRuntime::markLayerDirty(const std::string& assetPath, int layerId) {
    for (auto it = geometry.begin(); it != geometry.end();) {
        if (it->first.assetPath == assetPath && it->first.layerId == layerId) {
            it = geometry.erase(it);
        } else {
            ++it;
        }
    }
}

void AssemblageRuntime::markAssetDirty(const std::string& assetPath) {
    for (auto it = geometry.begin(); it != geometry.end();) {
        if (it->first.assetPath == assetPath) {
            it = geometry.erase(it);
        } else {
            ++it;
        }
    }
}

void AssemblageRuntime::markUnsaved(const std::string& assetPath, bool unsaved) {
    auto it = entries.find(assetPath);
    if (it != entries.end()) it->second.hasUnsavedChanges = unsaved;
}

bool AssemblageRuntime::hasUnsavedChanges(const std::string& assetPath) const {
    const Entry* entry = find(assetPath);
    return entry && entry->hasUnsavedChanges;
}

bool AssemblageRuntime::anyUnsavedChanges() const {
    for (const auto& entry : entries) {
        if (entry.second.hasUnsavedChanges) return true;
    }
    return false;
}

void AssemblageRuntime::buildChunkGeometry(const Entry& entry, const Assemblage::Layer& layer,
                                           Assemblage::ChunkCoord coord, Renderer& renderer,
                                           ChunkGeometry& out) {
    out.quads.clear();
    out.hasAnimatedTiles = false;
    out.valid = true;

    const int chunkSize = std::max(Assemblage::kMinChunkSize, entry.asset.chunkSize);
    const int baseX = coord.x * chunkSize;
    const int baseY = coord.y * chunkSize;

    out.boundsMin = CellWorldMin(entry.asset, baseX, baseY);
    out.boundsMax = CellWorldMin(entry.asset, baseX + chunkSize, baseY + chunkSize);
    if (out.boundsMin.x > out.boundsMax.x) std::swap(out.boundsMin.x, out.boundsMax.x);
    if (out.boundsMin.y > out.boundsMax.y) std::swap(out.boundsMin.y, out.boundsMax.y);

    const auto chunkIt = layer.chunks.find(coord);
    if (chunkIt == layer.chunks.end()) return;
    const Assemblage::Chunk& chunk = chunkIt->second;

    out.quads.reserve(chunk.cells.size() / 4);

    struct SheetBinding {
        const std::string* path = nullptr;
        unsigned int textureId = 0;
        int width = 0;
        int height = 0;
    };
    std::vector<SheetBinding> sheets;
    auto bindSheet = [&](const std::string& path) -> const SheetBinding* {
        for (const SheetBinding& sheet : sheets) {
            if (*sheet.path == path) return sheet.textureId != 0 ? &sheet : nullptr;
        }
        SheetBinding binding;
        binding.path = &path;
        if (!path.empty()) {
            if (Texture* texture = renderer.getTexture(path, MaterialProperties::TextureFilter::Point)) {
                binding.textureId = texture->GetID();
                binding.width = texture->GetWidth();
                binding.height = texture->GetHeight();
            }
        }
        sheets.push_back(binding);
        return binding.textureId != 0 ? &sheets.back() : nullptr;
    };

    for (int localY = 0; localY < chunkSize; ++localY) {
        for (int localX = 0; localX < chunkSize; ++localX) {
            const size_t index = static_cast<size_t>(localY) * static_cast<size_t>(chunkSize) +
                                 static_cast<size_t>(localX);
            if (index >= chunk.cells.size()) continue;
            const Assemblage::Cell cell = chunk.cells[index];
            if (Assemblage::CellIsEmpty(cell)) continue;

            const Assemblage::TileDef* tile = entry.tileset.find(Assemblage::CellTile(cell));
            if (!tile) continue;

            const SheetBinding* sheet = bindSheet(tile->sheetPath);
            if (!sheet) continue;

            glm::vec2 uvMin(0.0f);
            glm::vec2 uvMax(1.0f);
            if (!ResolveTileUv(*tile, sheet->width, sheet->height, 0.0, uvMin, uvMax)) continue;

            TileQuad quad;
            quad.textureId = sheet->textureId;
            quad.worldMin = CellWorldMin(entry.asset, baseX + localX, baseY + localY);
            quad.worldMax = quad.worldMin + entry.asset.cellSize;
            quad.uvMin = uvMin;
            quad.uvMax = uvMax;
            quad.tint = tile->tint;
            quad.cellFlags = Assemblage::CellFlagsOf(cell);
            if (tile->animation.animated()) {
                quad.animatedTile = tile->id;
                quad.sheetWidth = sheet->width;
                quad.sheetHeight = sheet->height;
                out.hasAnimatedTiles = true;
            }

            if ((quad.cellFlags & Assemblage::CellFlag_FlipX) != 0) {
                std::swap(quad.uvMin.x, quad.uvMax.x);
            }
            if ((quad.cellFlags & Assemblage::CellFlag_FlipY) != 0) {
                std::swap(quad.uvMin.y, quad.uvMax.y);
            }
            out.quads.push_back(quad);
        }
    }
}

const AssemblageRuntime::ChunkGeometry* AssemblageRuntime::chunkGeometry(
    const std::string& assetPath, int layerId, Assemblage::ChunkCoord coord, Renderer& renderer) {
    Entry* entry = acquire(assetPath);
    if (!entry) return nullptr;

    const Assemblage::Layer* layer = entry->asset.findLayer(layerId);
    if (!layer) return nullptr;

    ChunkKey key;
    key.assetPath = assetPath;
    key.layerId = layerId;
    key.chunkX = coord.x;
    key.chunkY = coord.y;

    auto it = geometry.find(key);
    if (it != geometry.end() && it->second.valid) {
        return &it->second;
    }

    ChunkGeometry built;
    buildChunkGeometry(*entry, *layer, coord, renderer, built);
    auto inserted = geometry.emplace(std::move(key), std::move(built));
    return &inserted.first->second;
}

bool AssemblageRuntime::save(const std::string& assetPath, std::string& outError) {
    auto it = entries.find(assetPath);
    if (it == entries.end() || !it->second.loaded) {
        outError = "No loaded Assemblage at " + assetPath;
        return false;
    }
    if (!Assemblage::SaveAsset(resolvePath(assetPath), it->second.asset, outError)) {
        return false;
    }
    it->second.hasUnsavedChanges = false;
    return true;
}

bool AssemblageRuntime::saveAllDirty(std::string& outError) {
    bool ok = true;
    for (auto& entry : entries) {
        if (!entry.second.hasUnsavedChanges || !entry.second.loaded) continue;
        std::string error;
        if (!save(entry.first, error)) {
            ok = false;
            if (outError.empty()) outError = error;
        }
    }
    return ok;
}

void AssemblageRuntime::invalidateAllGeometry() { geometry.clear(); }

void AssemblageRuntime::clear() {
    geometry.clear();
    entries.clear();
}
