#include "Assemblage.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <unordered_set>

namespace Assemblage {
namespace {

std::string Trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
}

bool ParseAssignment(const std::string& line, std::string& outKey, std::string& outValue) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    outKey = Trim(line.substr(0, eq));
    std::string value = Trim(line.substr(eq + 1));
    if (!value.empty() && value.back() == ';') value.pop_back();
    value = Trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    outValue = value;
    return !outKey.empty();
}

std::string EscapeString(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::vector<std::string> SplitList(const std::string& value, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(value);
    std::string token;
    while (std::getline(ss, token, delim)) {
        token = Trim(token);
        if (!token.empty()) out.push_back(token);
    }
    return out;
}

int ParseIntOr(const std::string& value, int fallback) {
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

float ParseFloatOr(const std::string& value, float fallback) {
    try {
        return std::stof(value);
    } catch (...) {
        return fallback;
    }
}

bool ParseVec2(const std::string& value, glm::vec2& out) {
    const std::vector<std::string> parts = SplitList(value, ',');
    if (parts.size() < 2) return false;
    out.x = ParseFloatOr(parts[0], out.x);
    out.y = ParseFloatOr(parts[1], out.y);
    return true;
}

bool ParseVec4(const std::string& value, glm::vec4& out) {
    const std::vector<std::string> parts = SplitList(value, ',');
    if (parts.size() < 4) return false;
    out.x = ParseFloatOr(parts[0], out.x);
    out.y = ParseFloatOr(parts[1], out.y);
    out.z = ParseFloatOr(parts[2], out.z);
    out.w = ParseFloatOr(parts[3], out.w);
    return true;
}

bool ParseIVec4(const std::string& value, glm::ivec4& out) {
    const std::vector<std::string> parts = SplitList(value, ',');
    if (parts.size() < 4) return false;
    out.x = ParseIntOr(parts[0], out.x);
    out.y = ParseIntOr(parts[1], out.y);
    out.z = ParseIntOr(parts[2], out.z);
    out.w = ParseIntOr(parts[3], out.w);
    return true;
}

std::string WriteVec2(const glm::vec2& v) {
    std::ostringstream out;
    out << v.x << "," << v.y;
    return out.str();
}

std::string WriteVec4(const glm::vec4& v) {
    std::ostringstream out;
    out << v.x << "," << v.y << "," << v.z << "," << v.w;
    return out.str();
}

std::string WriteIVec4(const glm::ivec4& v) {
    std::ostringstream out;
    out << v.x << "," << v.y << "," << v.z << "," << v.w;
    return out.str();
}

bool ParseBlockHeader(const std::string& line, std::string& outIdentifier,
                      std::map<std::string, std::string>& outParams) {
    outParams.clear();
    const std::string trimmed = Trim(line);
    if (trimmed.rfind("MODU_", 0) != 0) return false;

    const size_t brace = trimmed.find('{');
    if (brace == std::string::npos) return false;

    std::string head = Trim(trimmed.substr(0, brace));
    const size_t open = head.find('(');
    if (open == std::string::npos) {
        const size_t eq = head.find('=');
        outIdentifier = Trim(eq == std::string::npos ? head : head.substr(0, eq));
        return !outIdentifier.empty();
    }

    const size_t close = head.rfind(')');
    if (close == std::string::npos || close < open) return false;

    std::string beforeParams = Trim(head.substr(0, open));
    const size_t eq = beforeParams.find('=');
    outIdentifier = Trim(eq == std::string::npos ? beforeParams : beforeParams.substr(0, eq));

    const std::string params = head.substr(open + 1, close - open - 1);
    std::vector<std::string> pieces;
    size_t segStart = 0;
    for (size_t i = 0; i < params.size(); ++i) {
        if (params[i] != ',') continue;
        size_t probe = i + 1;
        while (probe < params.size() && std::isspace(static_cast<unsigned char>(params[probe]))) ++probe;
        size_t nameEnd = probe;
        while (nameEnd < params.size() &&
               (std::isalnum(static_cast<unsigned char>(params[nameEnd])) || params[nameEnd] == '_')) {
            ++nameEnd;
        }
        if (nameEnd > probe && nameEnd < params.size() && params[nameEnd] == '=') {
            pieces.push_back(params.substr(segStart, i - segStart));
            segStart = i + 1;
        }
    }
    pieces.push_back(params.substr(segStart));

    for (const std::string& piece : pieces) {
        const size_t peq = piece.find('=');
        if (peq == std::string::npos) continue;
        std::string key = Trim(piece.substr(0, peq));
        std::string value = Trim(piece.substr(peq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (!key.empty()) outParams[key] = value;
    }
    return !outIdentifier.empty();
}

bool IsBlockClose(const std::string& line) {
    const std::string trimmed = Trim(line);
    return trimmed == "}" || trimmed == "};";
}

std::string EncodeCellsRLE32(const std::vector<Cell>& cells) {
    std::ostringstream out;
    size_t i = 0;
    bool first = true;
    while (i < cells.size()) {
        const Cell value = cells[i];
        size_t run = 1;
        while (i + run < cells.size() && cells[i + run] == value) ++run;
        if (!first) out << ",";
        if (run > 1) out << run << "*";
        out << value;
        first = false;
        i += run;
    }
    return out.str();
}

bool DecodeCellsRLE32(const std::string& text, size_t expected, std::vector<Cell>& out) {
    out.assign(expected, 0u);
    size_t written = 0;
    for (const std::string& token : SplitList(text, ',')) {
        size_t run = 1;
        std::string valuePart = token;
        const size_t star = token.find('*');
        if (star != std::string::npos) {
            long long parsed = 0;
            try {
                parsed = std::stoll(Trim(token.substr(0, star)));
            } catch (...) {
                return false;
            }
            if (parsed <= 0) return false;
            run = static_cast<size_t>(parsed);
            valuePart = token.substr(star + 1);
        }
        Cell value = 0;
        try {
            value = static_cast<Cell>(std::stoul(Trim(valuePart)));
        } catch (...) {
            return false;
        }
        if (written + run > expected) return false;
        for (size_t i = 0; i < run; ++i) out[written + i] = value;
        written += run;
    }
    return written <= expected;
}

std::string GenerateId(const char* prefix) {
    static std::mt19937_64 rng([] {
        std::random_device rd;
        const uint64_t clock = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return static_cast<uint64_t>(rd()) ^ (clock * 0x9E3779B97F4A7C15ull);
    }());
    std::ostringstream out;
    out << prefix << "-" << std::hex << std::setw(16) << std::setfill('0') << rng();
    return out.str();
}

const char* TileCollisionName(TileCollision collision) {
    switch (collision) {
        case TileCollision::Box: return "box";
        case TileCollision::Circle: return "circle";
        case TileCollision::Polygon: return "polygon";
        case TileCollision::Line: return "line";
        case TileCollision::None:
        default: return "none";
    }
}

TileCollision ParseTileCollision(const std::string& value) {
    if (value == "box") return TileCollision::Box;
    if (value == "circle") return TileCollision::Circle;
    if (value == "polygon") return TileCollision::Polygon;
    if (value == "line") return TileCollision::Line;
    return TileCollision::None;
}

const char* TileAnimationModeName(TileAnimationMode mode) {
    switch (mode) {
        case TileAnimationMode::PingPong: return "pingpong";
        case TileAnimationMode::Once: return "once";
        case TileAnimationMode::Loop:
        default: return "loop";
    }
}

TileAnimationMode ParseTileAnimationMode(const std::string& value) {
    if (value == "pingpong") return TileAnimationMode::PingPong;
    if (value == "once") return TileAnimationMode::Once;
    return TileAnimationMode::Loop;
}

std::string WriteFrameList(const std::vector<TileFrame>& frames) {
    std::string out;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (i) out += "|";
        out += WriteIVec4(frames[i].rect);
    }
    return out;
}

std::vector<TileFrame> ParseFrameList(const std::string& value) {
    std::vector<TileFrame> out;
    for (const std::string& piece : SplitList(value, '|')) {
        TileFrame frame;
        if (ParseIVec4(piece, frame.rect)) out.push_back(frame);
    }
    return out;
}

std::string WritePointList(const std::vector<glm::vec2>& points) {
    std::string out;
    for (size_t i = 0; i < points.size(); ++i) {
        if (i) out += "|";
        out += WriteVec2(points[i]);
    }
    return out;
}

std::vector<glm::vec2> ParsePointList(const std::string& value) {
    std::vector<glm::vec2> out;
    for (const std::string& piece : SplitList(value, '|')) {
        glm::vec2 point(0.0f);
        if (ParseVec2(piece, point)) out.push_back(point);
    }
    return out;
}

template <typename T, typename WriteFn, typename ReadFn>
bool SaveThroughTemp(const fs::path& path, const T& value, const char* label,
                     WriteFn write, ReadFn read, std::string& outError) {
    outError.clear();

    std::string serialized;
    {
        std::ostringstream out;
        if (!write(out, value, outError)) return false;
        serialized = out.str();
    }

    {
        T roundTrip;
        std::istringstream check(serialized);
        std::string parseError;
        if (!read(check, roundTrip, parseError)) {
            outError = std::string("Refusing to save a ") + label +
                       " that does not parse back: " + parseError;
            return false;
        }
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    const fs::path tempPath = path.string() + ".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            outError = "Failed to open temporary file: " + tempPath.string();
            return false;
        }
        out << serialized;
        out.flush();
        if (!out) {
            out.close();
            fs::remove(tempPath, ec);
            outError = std::string("Failed to write temporary ") + label + " file.";
            return false;
        }
    }

    fs::rename(tempPath, path, ec);
    if (ec) {
        std::error_code copyEc;
        fs::copy_file(tempPath, path, fs::copy_options::overwrite_existing, copyEc);
        fs::remove(tempPath, ec);
        if (copyEc) {
            outError = std::string("Failed to move the ") + label +
                       " into place: " + copyEc.message();
            return false;
        }
    }
    return true;
}

}  // namespace

std::string GenerateAssemblageId() { return GenerateId("asm"); }
std::string GenerateTilesetId() { return GenerateId("tls"); }

bool IsWellFormedId(const std::string& id, const char* prefix) {
    const std::string wanted = std::string(prefix) + "-";
    if (id.rfind(wanted, 0) != 0) return false;
    const std::string hex = id.substr(wanted.size());
    if (hex.size() != 16) return false;
    return std::all_of(hex.begin(), hex.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

const TileDef* Tileset::find(TileId id) const {
    for (const TileDef& tile : tiles) {
        if (tile.id == id) return &tile;
    }
    return nullptr;
}

TileDef* Tileset::find(TileId id) {
    return const_cast<TileDef*>(static_cast<const Tileset*>(this)->find(id));
}

int ResolveAnimationFrame(const TileDef& tile, double timeSeconds) {
    const TileAnimation& anim = tile.animation;
    if (!anim.animated()) return 0;

    const int count = static_cast<int>(anim.frames.size());
    const double step = timeSeconds * static_cast<double>(anim.fps);
    if (step <= 0.0) return 0;

    switch (anim.mode) {
        case TileAnimationMode::Once: {
            const long long index = static_cast<long long>(step);
            return static_cast<int>(std::min<long long>(index, count - 1));
        }
        case TileAnimationMode::PingPong: {
            const int period = count * 2 - 2;
            int index = static_cast<int>(static_cast<long long>(step) % period);
            if (index >= count) index = period - index;
            return index;
        }
        case TileAnimationMode::Loop:
        default:
            return static_cast<int>(static_cast<long long>(step) % count);
    }
}

bool Chunk::empty() const {
    for (Cell cell : cells) {
        if (!CellIsEmpty(cell)) return false;
    }
    return true;
}

const Layer* Asset::findLayer(int layerId) const {
    for (const Layer& layer : layers) {
        if (layer.id == layerId) return &layer;
    }
    return nullptr;
}

Layer* Asset::findLayer(int layerId) {
    return const_cast<Layer*>(static_cast<const Asset*>(this)->findLayer(layerId));
}

Layer& Asset::addLayer(const std::string& layerName) {
    Layer layer;
    layer.id = nextLayerId++;
    layer.name = layerName.empty() ? ("Layer " + std::to_string(layer.id)) : layerName;
    layer.order = static_cast<int>(layers.size());
    layer.sortingOrder = layer.order;
    layers.push_back(std::move(layer));
    return layers.back();
}

namespace {
int FloorDiv(int value, int divisor) {
    int quotient = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) --quotient;
    return quotient;
}

int FloorMod(int value, int divisor) {
    const int result = value % divisor;
    return (result != 0 && (result < 0) != (divisor < 0)) ? result + divisor : result;
}
}  // namespace

ChunkCoord CellToChunk(int cellX, int cellY, int chunkSize) {
    const int size = std::max(kMinChunkSize, chunkSize);
    ChunkCoord coord;
    coord.x = FloorDiv(cellX, size);
    coord.y = FloorDiv(cellY, size);
    return coord;
}

int CellIndexInChunk(int cellX, int cellY, int chunkSize) {
    const int size = std::max(kMinChunkSize, chunkSize);
    return FloorMod(cellY, size) * size + FloorMod(cellX, size);
}

Cell GetCell(const Asset& asset, int layerId, int cellX, int cellY) {
    const Layer* layer = asset.findLayer(layerId);
    if (!layer) return 0;

    const ChunkCoord coord = CellToChunk(cellX, cellY, asset.chunkSize);
    const auto it = layer->chunks.find(coord);
    if (it == layer->chunks.end()) return 0;

    const int index = CellIndexInChunk(cellX, cellY, asset.chunkSize);
    if (index < 0 || index >= static_cast<int>(it->second.cells.size())) return 0;
    return it->second.cells[static_cast<size_t>(index)];
}

bool SetCell(Asset& asset, int layerId, int cellX, int cellY, Cell value, ChunkCoord* outChunk) {
    Layer* layer = asset.findLayer(layerId);
    if (!layer) return false;

    const int size = std::max(kMinChunkSize, asset.chunkSize);
    const ChunkCoord coord = CellToChunk(cellX, cellY, size);
    const int index = CellIndexInChunk(cellX, cellY, size);
    if (outChunk) *outChunk = coord;

    auto it = layer->chunks.find(coord);
    if (it == layer->chunks.end()) {
        if (CellIsEmpty(value)) return false;
        Chunk chunk;
        chunk.coord = coord;
        chunk.cells.assign(static_cast<size_t>(size) * static_cast<size_t>(size), 0u);
        it = layer->chunks.emplace(coord, std::move(chunk)).first;
    }

    Chunk& chunk = it->second;
    if (index < 0 || index >= static_cast<int>(chunk.cells.size())) return false;
    if (chunk.cells[static_cast<size_t>(index)] == value) return false;

    chunk.cells[static_cast<size_t>(index)] = value;

    if (CellIsEmpty(value) && chunk.empty()) {
        layer->chunks.erase(it);
    }
    return true;
}

glm::vec2 CellToWorld(const Asset& asset, int cellX, int cellY) {
    return asset.origin + glm::vec2(static_cast<float>(cellX) * asset.cellSize.x,
                                    static_cast<float>(cellY) * asset.cellSize.y);
}

void WorldToCell(const Asset& asset, const glm::vec2& world, int& outCellX, int& outCellY) {
    const float sx = (asset.cellSize.x != 0.0f) ? asset.cellSize.x : 1.0f;
    const float sy = (asset.cellSize.y != 0.0f) ? asset.cellSize.y : 1.0f;
    outCellX = static_cast<int>(std::floor((world.x - asset.origin.x) / sx));
    outCellY = static_cast<int>(std::floor((world.y - asset.origin.y) / sy));
}

bool LayerCellBounds(const Asset& asset, int layerId,
                     int& outMinX, int& outMinY, int& outMaxX, int& outMaxY) {
    const Layer* layer = asset.findLayer(layerId);
    if (!layer || layer->chunks.empty()) return false;

    const int size = std::max(kMinChunkSize, asset.chunkSize);
    bool any = false;
    for (const auto& entry : layer->chunks) {
        const int baseX = entry.second.coord.x * size;
        const int baseY = entry.second.coord.y * size;
        if (!any) {
            outMinX = baseX;
            outMinY = baseY;
            outMaxX = baseX + size - 1;
            outMaxY = baseY + size - 1;
            any = true;
            continue;
        }
        outMinX = std::min(outMinX, baseX);
        outMinY = std::min(outMinY, baseY);
        outMaxX = std::max(outMaxX, baseX + size - 1);
        outMaxY = std::max(outMaxY, baseY + size - 1);
    }
    return any;
}

bool WriteAssetStream(std::ostream& out, const Asset& asset, std::string& outError) {
    outError.clear();

    if (asset.assemblageId.empty()) {
        outError = "Assemblage has no assemblageId.";
        return false;
    }
    if (asset.kind != kKindGrid2D) {
        outError = "Assemblage kind \"" + asset.kind + "\" cannot be written by this engine.";
        return false;
    }
    if (asset.chunkSize < kMinChunkSize || asset.chunkSize > kMaxChunkSize) {
        outError = "Assemblage chunkSize " + std::to_string(asset.chunkSize) +
                   " is outside the supported range.";
        return false;
    }

    out << "MODU_ASSEMBLAGE {\n";
    out << "    formatVersion=" << asset.formatVersion << ";\n";
    out << "    assemblageId=\"" << EscapeString(asset.assemblageId) << "\";\n";
    out << "    name=\"" << EscapeString(asset.name) << "\";\n";
    out << "    kind=\"" << EscapeString(asset.kind) << "\";\n";
    out << "    chunkSize=" << asset.chunkSize << ";\n";
    out << "    cellSize=" << WriteVec2(asset.cellSize) << ";\n";
    out << "    origin=" << WriteVec2(asset.origin) << ";\n";
    out << "    nextLayerId=" << asset.nextLayerId << ";\n";
    out << "    tileset=\"" << EscapeString(asset.tilesetPath) << "\";\n";
    out << "}\n\n";

    for (const Layer& layer : asset.layers) {
        out << "MODU_ASMLAYER = (id=" << layer.id
            << ", name=\"" << EscapeString(layer.name) << "\") {\n";
        out << "    kind=\"" << EscapeString(layer.kind) << "\";\n";
        out << "    order=" << layer.order << ";\n";
        out << "    sortingOrder=" << layer.sortingOrder << ";\n";
        out << "    opacity=" << layer.opacity << ";\n";
        out << "    tint=" << WriteVec4(layer.tint) << ";\n";
        out << "    visible=" << (layer.visible ? 1 : 0) << ";\n";
        out << "    locked=" << (layer.locked ? 1 : 0) << ";\n";
        out << "    collision=" << (layer.collisionEnabled ? 1 : 0) << ";\n";
        out << "}\n\n";

        std::vector<const Chunk*> ordered;
        ordered.reserve(layer.chunks.size());
        for (const auto& entry : layer.chunks) ordered.push_back(&entry.second);
        std::sort(ordered.begin(), ordered.end(), [](const Chunk* a, const Chunk* b) {
            if (a->coord.y != b->coord.y) return a->coord.y < b->coord.y;
            return a->coord.x < b->coord.x;
        });

        for (const Chunk* chunk : ordered) {
            if (chunk->empty()) continue;
            out << "MODU_ASMCHUNK = (layer=" << layer.id
                << ", coord=" << chunk->coord.x << "," << chunk->coord.y << ") {\n";
            out << "    encoding=\"rle32\";\n";
            out << "    cells=" << EncodeCellsRLE32(chunk->cells) << ";\n";
            out << "}\n\n";
        }
    }
    return true;
}

bool ReadAssetStream(std::istream& in, Asset& outAsset, std::string& outError) {
    outAsset = Asset{};
    outError.clear();

    bool sawHeader = false;
    int headerVersion = -1;

    std::string line;
    while (std::getline(in, line)) {
        std::string identifier;
        std::map<std::string, std::string> params;
        if (!ParseBlockHeader(line, identifier, params)) continue;

        if (identifier == "MODU_ASSEMBLAGE") {
            sawHeader = true;
            while (std::getline(in, line) && !IsBlockClose(line)) {
                std::string key, value;
                if (!ParseAssignment(line, key, value)) continue;
                if (key == "formatVersion") headerVersion = ParseIntOr(value, -1);
                else if (key == "assemblageId") outAsset.assemblageId = value;
                else if (key == "name") outAsset.name = value;
                else if (key == "kind") outAsset.kind = value;
                else if (key == "chunkSize") outAsset.chunkSize = ParseIntOr(value, kDefaultChunkSize);
                else if (key == "cellSize") ParseVec2(value, outAsset.cellSize);
                else if (key == "origin") ParseVec2(value, outAsset.origin);
                else if (key == "nextLayerId") outAsset.nextLayerId = ParseIntOr(value, 1);
                else if (key == "tileset") outAsset.tilesetPath = value;
            }

            if (headerVersion < 0) {
                outError = "Assemblage header has no formatVersion.";
                return false;
            }
            if (headerVersion > kFormatVersion) {
                outError = "Assemblage format version " + std::to_string(headerVersion) +
                           " is newer than this engine supports (" +
                           std::to_string(kFormatVersion) +
                           "). Update Modularity to open this asset.";
                return false;
            }
            if (outAsset.kind != kKindGrid2D) {
                outError = "Assemblage kind \"" + outAsset.kind +
                           "\" is not supported by this engine (expected \"" +
                           kKindGrid2D + "\").";
                return false;
            }
            if (outAsset.chunkSize < kMinChunkSize || outAsset.chunkSize > kMaxChunkSize) {
                outError = "Assemblage chunkSize " + std::to_string(outAsset.chunkSize) +
                           " is outside the supported range (" +
                           std::to_string(kMinChunkSize) + ".." +
                           std::to_string(kMaxChunkSize) + ").";
                return false;
            }
            outAsset.formatVersion = headerVersion;
            continue;
        }

        if (identifier == "MODU_ASMLAYER") {
            Layer layer;
            layer.id = ParseIntOr(params.count("id") ? params["id"] : "", -1);
            if (params.count("name")) layer.name = params["name"];

            while (std::getline(in, line) && !IsBlockClose(line)) {
                std::string key, value;
                if (!ParseAssignment(line, key, value)) continue;
                if (key == "kind") layer.kind = value;
                else if (key == "order") layer.order = ParseIntOr(value, 0);
                else if (key == "sortingOrder") layer.sortingOrder = ParseIntOr(value, 0);
                else if (key == "opacity") layer.opacity = ParseFloatOr(value, 1.0f);
                else if (key == "tint") ParseVec4(value, layer.tint);
                else if (key == "visible") layer.visible = ParseIntOr(value, 1) != 0;
                else if (key == "locked") layer.locked = ParseIntOr(value, 0) != 0;
                else if (key == "collision") layer.collisionEnabled = ParseIntOr(value, 1) != 0;
            }

            if (layer.id < 0) {
                outError = "Assemblage layer block has a missing or malformed id.";
                return false;
            }
            if (layer.kind != kLayerKindTile) {
                outError = "Assemblage layer \"" + layer.name + "\" has kind \"" + layer.kind +
                           "\", which is not supported by this engine.";
                return false;
            }
            if (outAsset.findLayer(layer.id) != nullptr) {
                outError = "Assemblage contains duplicate layer id " +
                           std::to_string(layer.id) + ".";
                return false;
            }
            outAsset.nextLayerId = std::max(outAsset.nextLayerId, layer.id + 1);
            outAsset.layers.push_back(std::move(layer));
            continue;
        }

        if (identifier == "MODU_ASMCHUNK") {
            const int layerId = ParseIntOr(params.count("layer") ? params["layer"] : "", -1);
            ChunkCoord coord;
            if (params.count("coord")) {
                const std::vector<std::string> parts = SplitList(params["coord"], ',');
                if (parts.size() >= 2) {
                    coord.x = ParseIntOr(parts[0], 0);
                    coord.y = ParseIntOr(parts[1], 0);
                }
            }

            std::string encoding = "rle32";
            std::string cellText;
            while (std::getline(in, line) && !IsBlockClose(line)) {
                std::string key, value;
                if (!ParseAssignment(line, key, value)) continue;
                if (key == "encoding") encoding = value;
                else if (key == "cells") cellText = value;
            }

            Layer* layer = outAsset.findLayer(layerId);
            if (!layer) {
                outError = "Assemblage chunk references unknown layer id " +
                           std::to_string(layerId) + ".";
                return false;
            }
            if (encoding != "rle32") {
                outError = "Assemblage chunk uses encoding \"" + encoding +
                           "\", which is not supported by this engine.";
                return false;
            }

            const size_t expected = static_cast<size_t>(outAsset.chunkSize) *
                                    static_cast<size_t>(outAsset.chunkSize);
            Chunk chunk;
            chunk.coord = coord;
            if (!DecodeCellsRLE32(cellText, expected, chunk.cells)) {
                outError = "Assemblage chunk at (" + std::to_string(coord.x) + "," +
                           std::to_string(coord.y) + ") has a malformed cell payload.";
                return false;
            }
            if (!chunk.empty()) {
                layer->chunks[coord] = std::move(chunk);
            }
            continue;
        }

        int depth = 1;
        while (depth > 0 && std::getline(in, line)) {
            std::string nestedId;
            std::map<std::string, std::string> nestedParams;
            if (ParseBlockHeader(line, nestedId, nestedParams)) ++depth;
            else if (IsBlockClose(line)) --depth;
        }
    }

    if (!sawHeader) {
        outError = "Not an Assemblage asset: missing MODU_ASSEMBLAGE header.";
        return false;
    }
    if (!IsWellFormedId(outAsset.assemblageId, "asm")) {
        outError = "Assemblage has a missing or malformed assemblageId.";
        return false;
    }
    return true;
}

bool SaveAsset(const fs::path& path, const Asset& asset, std::string& outError) {
    return SaveThroughTemp(path, asset, "Assemblage", WriteAssetStream, ReadAssetStream, outError);
}

bool LoadAsset(const fs::path& path, Asset& outAsset, std::string& outError) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        outError = "Failed to open Assemblage asset: " + path.string();
        return false;
    }
    return ReadAssetStream(in, outAsset, outError);
}

bool WriteTilesetStream(std::ostream& out, const Tileset& tileset, std::string& outError) {
    outError.clear();
    if (tileset.tilesetId.empty()) {
        outError = "Tileset has no tilesetId.";
        return false;
    }

    out << "MODU_TILESET {\n";
    out << "    formatVersion=" << tileset.formatVersion << ";\n";
    out << "    tilesetId=\"" << EscapeString(tileset.tilesetId) << "\";\n";
    out << "    name=\"" << EscapeString(tileset.name) << "\";\n";
    out << "    nextTileId=" << tileset.nextTileId << ";\n";
    out << "}\n\n";

    for (const TileDef& tile : tileset.tiles) {
        out << "MODU_TILE = (id=" << tile.id
            << ", name=\"" << EscapeString(tile.name) << "\") {\n";
        out << "    sheet=\"" << EscapeString(tile.sheetPath) << "\";\n";
        out << "    rect=" << WriteIVec4(tile.frame.rect) << ";\n";
        if (!tile.tags.empty()) {
            std::string joined;
            for (size_t i = 0; i < tile.tags.size(); ++i) {
                if (i) joined += ",";
                joined += tile.tags[i];
            }
            out << "    tags=\"" << EscapeString(joined) << "\";\n";
        }
        out << "    collision=\"" << TileCollisionName(tile.collision) << "\";\n";
        if (tile.collision != TileCollision::None) {
            out << "    collisionSize=" << WriteVec2(tile.collisionSize) << ";\n";
            out << "    collisionOffset=" << WriteVec2(tile.collisionOffset) << ";\n";
            out << "    collisionRadius=" << tile.collisionRadius << ";\n";
            if (!tile.collisionPoints.empty()) {
                out << "    collisionPoints=\"" << WritePointList(tile.collisionPoints) << "\";\n";
            }
        }
        if (tile.sortingOffset != 0) out << "    sortingOffset=" << tile.sortingOffset << ";\n";
        out << "    tint=" << WriteVec4(tile.tint) << ";\n";
        if (tile.animation.fps > 0.0f) {
            out << "    animFps=" << tile.animation.fps << ";\n";
            out << "    animMode=\"" << TileAnimationModeName(tile.animation.mode) << "\";\n";
            out << "    animFrames=\"" << WriteFrameList(tile.animation.frames) << "\";\n";
        }
        if (!tile.autoRuleSet.empty()) {
            out << "    autoRuleSet=\"" << EscapeString(tile.autoRuleSet) << "\";\n";
        }
        if (!tile.variants.empty()) {
            std::string joined;
            for (size_t i = 0; i < tile.variants.size(); ++i) {
                if (i) joined += ",";
                joined += std::to_string(tile.variants[i]);
            }
            out << "    variants=" << joined << ";\n";
        }
        for (const auto& entry : tile.meta) {
            out << "    meta." << entry.first << "=\"" << EscapeString(entry.second) << "\";\n";
        }
        out << "}\n\n";
    }
    return true;
}

bool ReadTilesetStream(std::istream& in, Tileset& outTileset, std::string& outError) {
    outTileset = Tileset{};
    outError.clear();

    bool sawHeader = false;
    int headerVersion = -1;
    std::unordered_set<TileId> seenIds;

    std::string line;
    while (std::getline(in, line)) {
        std::string identifier;
        std::map<std::string, std::string> params;
        if (!ParseBlockHeader(line, identifier, params)) continue;

        if (identifier == "MODU_TILESET") {
            sawHeader = true;
            while (std::getline(in, line) && !IsBlockClose(line)) {
                std::string key, value;
                if (!ParseAssignment(line, key, value)) continue;
                if (key == "formatVersion") headerVersion = ParseIntOr(value, -1);
                else if (key == "tilesetId") outTileset.tilesetId = value;
                else if (key == "name") outTileset.name = value;
                else if (key == "nextTileId") {
                    outTileset.nextTileId = static_cast<TileId>(std::max(1, ParseIntOr(value, 1)));
                }
            }
            if (headerVersion < 0) {
                outError = "Tileset header has no formatVersion.";
                return false;
            }
            if (headerVersion > kFormatVersion) {
                outError = "Tileset format version " + std::to_string(headerVersion) +
                           " is newer than this engine supports (" +
                           std::to_string(kFormatVersion) +
                           "). Update Modularity to open this asset.";
                return false;
            }
            outTileset.formatVersion = headerVersion;
            continue;
        }

        if (identifier == "MODU_TILE") {
            TileDef tile;
            tile.id = static_cast<TileId>(
                std::max(0, ParseIntOr(params.count("id") ? params["id"] : "", 0)));
            if (params.count("name")) tile.name = params["name"];

            while (std::getline(in, line) && !IsBlockClose(line)) {
                std::string key, value;
                if (!ParseAssignment(line, key, value)) continue;
                if (key.rfind("meta.", 0) == 0) {
                    tile.meta[key.substr(5)] = value;
                } else if (key == "sheet") tile.sheetPath = value;
                else if (key == "rect") ParseIVec4(value, tile.frame.rect);
                else if (key == "tags") tile.tags = SplitList(value, ',');
                else if (key == "collision") tile.collision = ParseTileCollision(value);
                else if (key == "collisionSize") ParseVec2(value, tile.collisionSize);
                else if (key == "collisionOffset") ParseVec2(value, tile.collisionOffset);
                else if (key == "collisionRadius") tile.collisionRadius = ParseFloatOr(value, 0.5f);
                else if (key == "collisionPoints") tile.collisionPoints = ParsePointList(value);
                else if (key == "sortingOffset") tile.sortingOffset = ParseIntOr(value, 0);
                else if (key == "tint") ParseVec4(value, tile.tint);
                else if (key == "animFps") tile.animation.fps = ParseFloatOr(value, 0.0f);
                else if (key == "animMode") tile.animation.mode = ParseTileAnimationMode(value);
                else if (key == "animFrames") tile.animation.frames = ParseFrameList(value);
                else if (key == "autoRuleSet") tile.autoRuleSet = value;
                else if (key == "variants") {
                    tile.variants.clear();
                    for (const std::string& piece : SplitList(value, ',')) {
                        tile.variants.push_back(static_cast<TileId>(std::max(0, ParseIntOr(piece, 0))));
                    }
                }
            }

            if (tile.id == kEmptyTile) {
                outError = "Tileset contains a tile with id 0, which is reserved for empty cells.";
                return false;
            }
            if (!seenIds.insert(tile.id).second) {
                outError = "Tileset contains duplicate tile id " + std::to_string(tile.id) + ".";
                return false;
            }
            outTileset.nextTileId = std::max(outTileset.nextTileId, tile.id + 1);
            outTileset.tiles.push_back(std::move(tile));
            continue;
        }

        int depth = 1;
        while (depth > 0 && std::getline(in, line)) {
            std::string nestedId;
            std::map<std::string, std::string> nestedParams;
            if (ParseBlockHeader(line, nestedId, nestedParams)) ++depth;
            else if (IsBlockClose(line)) --depth;
        }
    }

    if (!sawHeader) {
        outError = "Not a Tileset asset: missing MODU_TILESET header.";
        return false;
    }
    if (!IsWellFormedId(outTileset.tilesetId, "tls")) {
        outError = "Tileset has a missing or malformed tilesetId.";
        return false;
    }
    return true;
}

bool SaveTileset(const fs::path& path, const Tileset& tileset, std::string& outError) {
    return SaveThroughTemp(path, tileset, "Tileset", WriteTilesetStream, ReadTilesetStream, outError);
}

bool LoadTileset(const fs::path& path, Tileset& outTileset, std::string& outError) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        outError = "Failed to open Tileset asset: " + path.string();
        return false;
    }
    return ReadTilesetStream(in, outTileset, outError);
}

AssetCache MakeAssetCache() { return AssetCache(&LoadAsset); }
TilesetCache MakeTilesetCache() { return TilesetCache(&LoadTileset); }

}  // namespace Assemblage
