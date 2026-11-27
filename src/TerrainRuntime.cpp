#include "TerrainRuntime.h"

#include "DurableFile.h"
#include "InstancerRuntime.h"
#include "InstancerTools.h"
#include "../include/Platform/AssetSource.h"

#include "ThirdParty/glm/gtc/matrix_transform.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>

namespace fs = std::filesystem;

namespace TerrainRuntime {

namespace {

constexpr float kPi = 3.14159265358979f;

float Smoothstep(float a, float b, float x) {
    if (b == a) return x >= b ? 1.0f : 0.0f;
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

uint32_t HashU32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float HashFloat(int x, int y, uint32_t seed) {
    const uint32_t h = HashU32(static_cast<uint32_t>(x) * 0x1f1f1f1fu ^ HashU32(static_cast<uint32_t>(y) + seed * 0x9e3779b9u));
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000);
}

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}
void PutF32(std::vector<uint8_t>& out, float f) {
    uint32_t v = 0;
    std::memcpy(&v, &f, sizeof(v));
    PutU32(out, v);
}
uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
float GetF32(const uint8_t* p) {
    const uint32_t v = GetU32(p);
    float f = 0.0f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

inline float HeightAtSample(const TerrainComponent& t, int x, int z) {
    const int res = t.resolution;
    x = std::clamp(x, 0, res - 1);
    z = std::clamp(z, 0, res - 1);
    return (*t.heights)[static_cast<size_t>(z) * static_cast<size_t>(res) + static_cast<size_t>(x)];
}

// Local Y in metres of a normalized sample.
inline float MetresOf(const TerrainComponent& t, float normalized) {
    return t.heightOffset + normalized * t.heightScale;
}
inline float NormalizedOf(const TerrainComponent& t, float metres) {
    return t.heightScale > 1e-5f ? std::clamp((metres - t.heightOffset) / t.heightScale, 0.0f, 1.0f) : 0.0f;
}

// Brush falloff: 1 inside the hard core, smooth to 0 at the radius.
float BrushWeight(float distance, float radius, float falloff) {
    if (radius <= 1e-4f) return 0.0f;
    const float t = distance / radius;
    if (t >= 1.0f) return 0.0f;
    const float hard = std::clamp(1.0f - falloff, 0.0f, 0.999f);
    if (t <= hard) return 1.0f;
    const float k = 1.0f - (t - hard) / (1.0f - hard);
    return k * k * (3.0f - 2.0f * k);
}

// Moves weight toward `layer` by `amount` (0..1) and rescales the others so the texel still sums to 255.
void PushLayerWeight(uint8_t* texel, int layer, float amount) {
    if (amount <= 0.0f) return;
    float w[4];
    float sum = 0.0f;
    for (int i = 0; i < 4; ++i) { w[i] = static_cast<float>(texel[i]); sum += w[i]; }
    if (sum < 1.0f) { w[0] = 255.0f; sum = 255.0f; }
    for (int i = 0; i < 4; ++i) w[i] = w[i] / sum * 255.0f;
    const float old = w[layer];
    const float neu = std::min(255.0f, old + amount * 255.0f);
    const float others = 255.0f - old;
    const float remaining = 255.0f - neu;
    for (int i = 0; i < 4; ++i) {
        if (i == layer) continue;
        w[i] = others > 1e-4f ? w[i] * (remaining / others) : 0.0f;
    }
    w[layer] = neu;
    for (int i = 0; i < 4; ++i) texel[i] = static_cast<uint8_t>(std::clamp(w[i] + 0.5f, 0.0f, 255.0f));
}

} // namespace

glm::mat4 WorldMatrix(const SceneObject& object) {
    return InstancerRuntime::ObjectWorldMatrix(object);
}

bool IsActive(const SceneObject& object) {
    return object.hasTerrain && object.terrain.enabled && object.terrain.hasData() &&
           object.enabled && IsObjectEnabledInHierarchy(object);
}

// ---- Noise ---------------------------------------------------------------------------------------

float PerlinNoise(float x, float y, uint32_t seed) {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float xf = x - static_cast<float>(xi);
    const float yf = y - static_cast<float>(yi);
    const auto grad = [&](int ix, int iy, float dx, float dy) {
        const float angle = HashFloat(ix, iy, seed) * 2.0f * kPi;
        return std::cos(angle) * dx + std::sin(angle) * dy;
    };
    const float u = xf * xf * xf * (xf * (xf * 6.0f - 15.0f) + 10.0f);
    const float v = yf * yf * yf * (yf * (yf * 6.0f - 15.0f) + 10.0f);
    const float n00 = grad(xi, yi, xf, yf);
    const float n10 = grad(xi + 1, yi, xf - 1.0f, yf);
    const float n01 = grad(xi, yi + 1, xf, yf - 1.0f);
    const float n11 = grad(xi + 1, yi + 1, xf - 1.0f, yf - 1.0f);
    const float a = n00 + (n10 - n00) * u;
    const float b = n01 + (n11 - n01) * u;
    return (a + (b - a) * v) * 1.4142f;
}

float Fbm(float x, float y, uint32_t seed, int octaves, float persistence, float ridged) {
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    for (int i = 0; i < std::max(1, octaves); ++i) {
        float n = PerlinNoise(x * frequency, y * frequency, seed + static_cast<uint32_t>(i) * 7919u);
        if (ridged > 0.0f) {
            const float r = 1.0f - 2.0f * std::fabs(n);   // ridges up
            n = n * (1.0f - ridged) + r * ridged;
        }
        sum += n * amplitude;
        norm += amplitude;
        amplitude *= persistence;
        frequency *= 2.0f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

// ---- Data ---------------------------------------------------------------------------------------

void InitializeFlat(TerrainComponent& terrain, float baseHeightMetres) {
    terrain.resolution = std::clamp(terrain.resolution, 3, 4097);
    terrain.splatResolution = std::clamp(terrain.splatResolution, 16, 4096);
    const float norm = NormalizedOf(terrain, baseHeightMetres);
    terrain.heights = std::make_shared<std::vector<float>>(
        static_cast<size_t>(terrain.resolution) * static_cast<size_t>(terrain.resolution), norm);
    std::vector<uint8_t> splat(static_cast<size_t>(terrain.splatResolution) *
                               static_cast<size_t>(terrain.splatResolution) * 4u, 0);
    for (size_t i = 0; i < splat.size(); i += 4) splat[i] = 255;
    terrain.splat = std::make_shared<std::vector<uint8_t>>(std::move(splat));
    terrain.grassDensity.reset();
    terrain.markAllChanged();
}

void Resize(TerrainComponent& terrain, int newResolution, int newSplatResolution) {
    newResolution = std::clamp(newResolution, 3, 4097);
    newSplatResolution = std::clamp(newSplatResolution, 16, 4096);
    if (!terrain.hasData()) {
        terrain.resolution = newResolution;
        terrain.splatResolution = newSplatResolution;
        InitializeFlat(terrain, 0.0f);
        return;
    }
    if (newResolution != terrain.resolution) {
        const int oldRes = terrain.resolution;
        std::vector<float> resized(static_cast<size_t>(newResolution) * static_cast<size_t>(newResolution));
        const std::vector<float>& src = *terrain.heights;
        for (int z = 0; z < newResolution; ++z) {
            const float fz = static_cast<float>(z) / static_cast<float>(newResolution - 1) * static_cast<float>(oldRes - 1);
            const int z0 = std::min(static_cast<int>(fz), oldRes - 2);
            const float tz = fz - static_cast<float>(z0);
            for (int x = 0; x < newResolution; ++x) {
                const float fx = static_cast<float>(x) / static_cast<float>(newResolution - 1) * static_cast<float>(oldRes - 1);
                const int x0 = std::min(static_cast<int>(fx), oldRes - 2);
                const float tx = fx - static_cast<float>(x0);
                const auto at = [&](int ix, int iz) { return src[static_cast<size_t>(iz) * static_cast<size_t>(oldRes) + static_cast<size_t>(ix)]; };
                const float h = (at(x0, z0) * (1 - tx) + at(x0 + 1, z0) * tx) * (1 - tz) +
                                (at(x0, z0 + 1) * (1 - tx) + at(x0 + 1, z0 + 1) * tx) * tz;
                resized[static_cast<size_t>(z) * static_cast<size_t>(newResolution) + static_cast<size_t>(x)] = h;
            }
        }
        terrain.heights = std::make_shared<std::vector<float>>(std::move(resized));
        terrain.resolution = newResolution;
    }
    if (newSplatResolution != terrain.splatResolution) {
        const int oldRes = terrain.splatResolution;
        const auto resample = [&](const std::vector<uint8_t>& src, int channels, std::vector<uint8_t>& dst) {
            dst.assign(static_cast<size_t>(newSplatResolution) * static_cast<size_t>(newSplatResolution) * static_cast<size_t>(channels), 0);
            for (int z = 0; z < newSplatResolution; ++z) {
                const float fz = (static_cast<float>(z) + 0.5f) / static_cast<float>(newSplatResolution) * static_cast<float>(oldRes) - 0.5f;
                const int z0 = std::clamp(static_cast<int>(std::floor(fz)), 0, oldRes - 1);
                const int z1 = std::min(z0 + 1, oldRes - 1);
                const float tz = std::clamp(fz - static_cast<float>(z0), 0.0f, 1.0f);
                for (int x = 0; x < newSplatResolution; ++x) {
                    const float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(newSplatResolution) * static_cast<float>(oldRes) - 0.5f;
                    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, oldRes - 1);
                    const int x1 = std::min(x0 + 1, oldRes - 1);
                    const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
                    for (int c = 0; c < channels; ++c) {
                        const auto at = [&](int ix, int iz) {
                            return static_cast<float>(src[(static_cast<size_t>(iz) * static_cast<size_t>(oldRes) + static_cast<size_t>(ix)) * static_cast<size_t>(channels) + static_cast<size_t>(c)]);
                        };
                        const float v = (at(x0, z0) * (1 - tx) + at(x1, z0) * tx) * (1 - tz) +
                                        (at(x0, z1) * (1 - tx) + at(x1, z1) * tx) * tz;
                        dst[(static_cast<size_t>(z) * static_cast<size_t>(newSplatResolution) + static_cast<size_t>(x)) * static_cast<size_t>(channels) + static_cast<size_t>(c)] =
                            static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
                    }
                }
            }
        };
        std::vector<uint8_t> newSplat;
        resample(*terrain.splat, 4, newSplat);
        terrain.splat = std::make_shared<std::vector<uint8_t>>(std::move(newSplat));
        if (terrain.grassDensity && terrain.grassDensity->size() ==
                static_cast<size_t>(oldRes) * static_cast<size_t>(oldRes)) {
            std::vector<uint8_t> newDensity;
            resample(*terrain.grassDensity, 1, newDensity);
            terrain.grassDensity = std::make_shared<std::vector<uint8_t>>(std::move(newDensity));
        } else {
            terrain.grassDensity.reset();
        }
        terrain.splatResolution = newSplatResolution;
    }
    terrain.markAllChanged();
}

std::vector<uint8_t>& MutableGrassDensity(TerrainComponent& terrain) {
    const size_t count = static_cast<size_t>(terrain.splatResolution) * static_cast<size_t>(terrain.splatResolution);
    if (!terrain.grassDensity || terrain.grassDensity->size() != count) {
        terrain.grassDensity = std::make_shared<std::vector<uint8_t>>(count, 255);
    } else if (terrain.grassDensity.use_count() > 1) {
        terrain.grassDensity = std::make_shared<std::vector<uint8_t>>(*terrain.grassDensity);
    }
    return *terrain.grassDensity;
}

// ---- Sidecar ----------------------------------------------------------------------------------

std::vector<uint8_t> EncodeSidecar(const TerrainComponent& terrain) {
    std::vector<uint8_t> out;
    if (!terrain.hasData()) return out;
    const size_t heightCount = static_cast<size_t>(terrain.resolution) * static_cast<size_t>(terrain.resolution);
    const size_t splatTexels = static_cast<size_t>(terrain.splatResolution) * static_cast<size_t>(terrain.splatResolution);
    out.reserve(24 + heightCount * 4 + splatTexels * 5);
    out.insert(out.end(), {'M', 'T', 'E', 'R'});
    PutU32(out, kSidecarVersion);
    PutU32(out, static_cast<uint32_t>(terrain.resolution));
    PutU32(out, static_cast<uint32_t>(terrain.splatResolution));
    PutF32(out, terrain.size);
    PutF32(out, terrain.heightScale);
    const size_t headerEnd = out.size();
    out.resize(headerEnd + heightCount * 4);
    // Heights are stored as little-endian float32; every supported target is little endian.
    std::memcpy(out.data() + headerEnd, terrain.heights->data(), heightCount * 4);
    out.insert(out.end(), terrain.splat->begin(), terrain.splat->end());
    if (terrain.grassDensity && terrain.grassDensity->size() == splatTexels) {
        out.insert(out.end(), terrain.grassDensity->begin(), terrain.grassDensity->end());
    }
    return out;
}

bool DecodeSidecar(const uint8_t* bytes, size_t size, TerrainComponent& terrain, std::string* error) {
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (!bytes || size < 24 || std::memcmp(bytes, "MTER", 4) != 0) return fail("not a .modterrain file");
    const uint32_t version = GetU32(bytes + 4);
    if (version < 1 || version > kSidecarVersion) return fail("unsupported .modterrain version");
    const uint32_t resolution = GetU32(bytes + 8);
    const uint32_t splatResolution = GetU32(bytes + 12);
    const float fileSize = GetF32(bytes + 16);
    const float fileHeightScale = GetF32(bytes + 20);
    if (resolution < 2 || resolution > 8193 || splatResolution < 1 || splatResolution > 8192) {
        return fail("invalid .modterrain resolution");
    }
    const size_t heightCount = static_cast<size_t>(resolution) * resolution;
    const size_t splatTexels = static_cast<size_t>(splatResolution) * splatResolution;
    const size_t needed = 24 + heightCount * 4 + splatTexels * 4;
    if (size < needed) return fail("truncated .modterrain file");
    std::vector<float> heights(heightCount);
    std::memcpy(heights.data(), bytes + 24, heightCount * 4);
    for (float& h : heights) {
        if (!std::isfinite(h)) h = 0.0f;
        h = std::clamp(h, 0.0f, 1.0f);
    }
    terrain.resolution = static_cast<int>(resolution);
    terrain.splatResolution = static_cast<int>(splatResolution);
    if (std::isfinite(fileSize) && fileSize > 0.0f) terrain.size = fileSize;
    if (std::isfinite(fileHeightScale) && fileHeightScale > 0.0f) terrain.heightScale = fileHeightScale;
    terrain.heights = std::make_shared<std::vector<float>>(std::move(heights));
    const uint8_t* splatBegin = bytes + 24 + heightCount * 4;
    terrain.splat = std::make_shared<std::vector<uint8_t>>(splatBegin, splatBegin + splatTexels * 4);
    if (size >= needed + splatTexels) {
        const uint8_t* densityBegin = splatBegin + splatTexels * 4;
        terrain.grassDensity = std::make_shared<std::vector<uint8_t>>(densityBegin, densityBegin + splatTexels);
    } else {
        terrain.grassDensity.reset();
    }
    terrain.markAllChanged();
    terrain.savedHeightRevision = terrain.heightRevision;
    terrain.savedSplatRevision = terrain.splatRevision;
    return true;
}

bool LoadSidecar(TerrainComponent& terrain, std::string* error) {
    std::string message;
    bool ok = false;
    if (!terrain.dataPath.empty()) {
        const std::vector<uint8_t> bytes = Modularity::Platform::GetAssetSource().ReadAll(terrain.dataPath);
        if (bytes.empty()) {
            message = "could not read " + terrain.dataPath;
        } else {
            ok = DecodeSidecar(bytes.data(), bytes.size(), terrain, &message);
        }
    } else {
        message = "no data path";
    }
    if (!ok) {
        if (error) *error = message;
        // Keep the object usable: a flat terrain, marked clean so a failed read never overwrites
        // whatever sits at dataPath until the user actually edits it.
        InitializeFlat(terrain, 0.0f);
        terrain.savedHeightRevision = terrain.heightRevision;
        terrain.savedSplatRevision = terrain.splatRevision;
    }
    return ok;
}

std::string SiblingMapPath(const std::string& normalPath, const char* suffix) {
    const size_t at = normalPath.find("_Normal");
    if (at == std::string::npos) return std::string();
    const size_t dot = normalPath.find_last_of('.');
    if (dot == std::string::npos || dot < at) return std::string();
    return normalPath.substr(0, at) + "_" + suffix + normalPath.substr(dot);
}

namespace {
std::string SanitizeFileName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_' || c == '-') out.push_back(c);
        else if (c == ' ') out.push_back('_');
    }
    return out.empty() ? std::string("Terrain") : out;
}
} // namespace

SaveReport SaveDirtySidecars(std::vector<SceneObject>& objects, const std::string& projectRoot) {
    SaveReport report;
    std::set<std::string> usedPaths;
    for (SceneObject& object : objects) {
        if (!object.hasTerrain) continue;
        TerrainComponent& t = object.terrain;
        std::string path = t.dataPath;
        if (path.empty() || usedPaths.count(path)) {
            const std::string base = "Assets/Terrain/" + SanitizeFileName(object.name);
            path = base + ".modterrain";
            if (usedPaths.count(path)) path = base + "_" + std::to_string(object.id) + ".modterrain";
            // A fresh path starts unsaved (copies of a terrain share data until the first save).
            t.dataPath = path;
            t.savedHeightRevision = 0;
            t.savedSplatRevision = 0;
        }
        usedPaths.insert(path);
        if (!t.hasData()) continue;
        const fs::path absolute = projectRoot.empty() ? fs::path(path) : fs::path(projectRoot) / fs::path(path);
        std::error_code ec;
        if (!t.dataDirty() && fs::exists(absolute, ec)) continue;
        const std::vector<uint8_t> bytes = EncodeSidecar(t);
        std::string error;
        fs::create_directories(absolute.parent_path(), ec);
        if (!Modularity::WriteFileDurably(absolute,
                std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), error,
                Modularity::DurableFileMode::Binary)) {
            report.errors.push_back("Terrain '" + object.name + "': " + error);
            continue;
        }
        t.savedHeightRevision = t.heightRevision;
        t.savedSplatRevision = t.splatRevision;
        ++report.written;
    }
    return report;
}

// ---- Queries (local) --------------------------------------------------------------------------

float LocalHeightAt(const TerrainComponent& terrain, float lx, float lz, bool* inside) {
    if (!terrain.hasData()) {
        if (inside) *inside = false;
        return terrain.heightOffset;
    }
    const float half = terrain.size * 0.5f;
    const float cell = CellSize(terrain);
    if (inside) *inside = lx >= -half && lx <= half && lz >= -half && lz <= half;
    float fx = (lx + half) / cell;
    float fz = (lz + half) / cell;
    const float maxIndex = static_cast<float>(terrain.resolution - 1);
    fx = std::clamp(fx, 0.0f, maxIndex);
    fz = std::clamp(fz, 0.0f, maxIndex);
    const int x0 = std::min(static_cast<int>(fx), terrain.resolution - 2);
    const int z0 = std::min(static_cast<int>(fz), terrain.resolution - 2);
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);
    const float ha = HeightAtSample(terrain, x0, z0);
    const float hb = HeightAtSample(terrain, x0 + 1, z0);
    const float hc = HeightAtSample(terrain, x0, z0 + 1);
    const float hd = HeightAtSample(terrain, x0 + 1, z0 + 1);
    // Same diagonal (a-d) as the renderer's mesh and Jolt's heightfield.
    const float h = tz <= tx ? ha + (hb - ha) * tx + (hd - hb) * tz
                             : ha + (hd - hc) * tx + (hc - ha) * tz;
    return MetresOf(terrain, h);
}

glm::vec3 LocalNormalAt(const TerrainComponent& terrain, float lx, float lz) {
    const float d = std::max(0.05f, CellSize(terrain));
    const float hx0 = LocalHeightAt(terrain, lx - d, lz);
    const float hx1 = LocalHeightAt(terrain, lx + d, lz);
    const float hz0 = LocalHeightAt(terrain, lx, lz - d);
    const float hz1 = LocalHeightAt(terrain, lx, lz + d);
    return glm::normalize(glm::vec3(hx0 - hx1, 2.0f * d, hz0 - hz1));
}

float SlopeDegreesAt(const TerrainComponent& terrain, float lx, float lz) {
    const glm::vec3 n = LocalNormalAt(terrain, lx, lz);
    return std::acos(std::clamp(n.y, -1.0f, 1.0f)) * 180.0f / kPi;
}

namespace {
// Bilinear fetch from an interleaved byte map at terrain-local XZ.
void SampleSplatBilinear(const std::vector<uint8_t>& data, int channels, int res, float half, float size,
                         float lx, float lz, float* out) {
    float u = (lx + half) / size * static_cast<float>(res) - 0.5f;
    float v = (lz + half) / size * static_cast<float>(res) - 0.5f;
    u = std::clamp(u, 0.0f, static_cast<float>(res - 1));
    v = std::clamp(v, 0.0f, static_cast<float>(res - 1));
    const int x0 = std::min(static_cast<int>(u), res - 1);
    const int z0 = std::min(static_cast<int>(v), res - 1);
    const int x1 = std::min(x0 + 1, res - 1);
    const int z1 = std::min(z0 + 1, res - 1);
    const float tx = u - static_cast<float>(x0);
    const float tz = v - static_cast<float>(z0);
    for (int c = 0; c < channels; ++c) {
        const auto at = [&](int x, int z) {
            return static_cast<float>(data[(static_cast<size_t>(z) * static_cast<size_t>(res) + static_cast<size_t>(x)) * static_cast<size_t>(channels) + static_cast<size_t>(c)]);
        };
        out[c] = ((at(x0, z0) * (1 - tx) + at(x1, z0) * tx) * (1 - tz) +
                  (at(x0, z1) * (1 - tx) + at(x1, z1) * tx) * tz) / 255.0f;
    }
}
} // namespace

glm::vec4 LayerWeightAtLocal(const TerrainComponent& terrain, float lx, float lz) {
    if (!terrain.splat || terrain.splat->size() < static_cast<size_t>(terrain.splatResolution) * terrain.splatResolution * 4u) {
        return glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    }
    float w[4];
    SampleSplatBilinear(*terrain.splat, 4, terrain.splatResolution, terrain.size * 0.5f, terrain.size, lx, lz, w);
    const float sum = w[0] + w[1] + w[2] + w[3];
    if (sum < 1e-4f) return glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    return glm::vec4(w[0], w[1], w[2], w[3]) / sum;
}

float GrassDensityAtLocal(const TerrainComponent& terrain, float lx, float lz) {
    const size_t count = static_cast<size_t>(terrain.splatResolution) * static_cast<size_t>(terrain.splatResolution);
    if (!terrain.grassDensity || terrain.grassDensity->size() != count) return 1.0f;
    float d = 1.0f;
    SampleSplatBilinear(*terrain.grassDensity, 1, terrain.splatResolution, terrain.size * 0.5f, terrain.size, lx, lz, &d);
    return d;
}

float ClearingDistance(const TerrainClearing& c, float lx, float lz) {
    const glm::vec2 p(lx - c.center.x, lz - c.center.y);
    if (!c.rectangle) return glm::length(p) - std::max(0.0f, c.size.x);
    const float angle = -c.rotationDegrees * kPi / 180.0f;
    const float cs = std::cos(angle);
    const float sn = std::sin(angle);
    const glm::vec2 q(p.x * cs - p.y * sn, p.x * sn + p.y * cs);
    const glm::vec2 d = glm::abs(q) - glm::vec2(std::max(0.0f, c.size.x), std::max(0.0f, c.size.y));
    return glm::length(glm::max(d, glm::vec2(0.0f))) + std::min(std::max(d.x, d.y), 0.0f);
}

float KeepOutAtLocal(const TerrainComponent& terrain, float lx, float lz) {
    float keep = 0.0f;
    const glm::vec4 w = LayerWeightAtLocal(terrain, lx, lz);
    keep = std::max(keep, Smoothstep(0.3f, 0.65f, w.z));
    for (const TerrainPond& pond : terrain.ponds) {
        const float d = glm::length(glm::vec2(lx - pond.center.x, lz - pond.center.y)) - pond.radius;
        keep = std::max(keep, 1.0f - Smoothstep(0.0f, 3.0f, d));
    }
    for (const TerrainClearing& clearing : terrain.clearings) {
        if (!clearing.keepOut) continue;
        const float sd = ClearingDistance(clearing, lx, lz);
        keep = std::max(keep, 1.0f - Smoothstep(0.0f, std::max(clearing.blend, 0.001f), sd));
    }
    return keep;
}

// ---- Queries (world) --------------------------------------------------------------------------

bool SampleObject(const SceneObject& obj, float wx, float wz, SurfaceSample& out) {
    if (!IsActive(obj)) return false;
    const glm::mat4 world = WorldMatrix(obj);
    const glm::mat4 inverse = glm::inverse(world);
    // Intersect the vertical line through (wx, wz) with the local heightfield; for terrains that
    // are only translated, yawed and scaled the local XZ is independent of the height we pick.
    const glm::vec3 local = glm::vec3(inverse * glm::vec4(wx, obj.position.y, wz, 1.0f));
    const TerrainComponent& t = obj.terrain;
    bool inside = false;
    float ly = LocalHeightAt(t, local.x, local.z, &inside);
    if (!inside) return false;
    // One refinement for tilted terrains: re-project using the found height.
    const glm::vec3 worldPoint = glm::vec3(world * glm::vec4(local.x, ly, local.z, 1.0f));
    glm::vec3 refined = glm::vec3(inverse * glm::vec4(wx, worldPoint.y, wz, 1.0f));
    ly = LocalHeightAt(t, refined.x, refined.z, &inside);
    if (!inside) return false;
    const glm::vec3 surface = glm::vec3(world * glm::vec4(refined.x, ly, refined.z, 1.0f));
    out.valid = true;
    out.height = surface.y;
    const glm::vec3 localNormal = LocalNormalAt(t, refined.x, refined.z);
    out.normal = glm::normalize(glm::vec3(glm::transpose(inverse) * glm::vec4(localNormal, 0.0f)));
    out.layers = LayerWeightAtLocal(t, refined.x, refined.z);
    out.keepOut = KeepOutAtLocal(t, refined.x, refined.z);
    out.grassDensity = GrassDensityAtLocal(t, refined.x, refined.z);
    out.terrainId = obj.id;
    return true;
}

bool SampleScene(const std::vector<SceneObject>& objects, float wx, float wz, SurfaceSample& out) {
    for (const SceneObject& obj : objects) {
        if (!obj.hasTerrain) continue;
        if (SampleObject(obj, wx, wz, out)) return true;
    }
    return false;
}

float HeightAt(const std::vector<SceneObject>& objects, float wx, float wz, float fallback) {
    SurfaceSample s;
    return SampleScene(objects, wx, wz, s) ? s.height : fallback;
}
glm::vec3 NormalAt(const std::vector<SceneObject>& objects, float wx, float wz) {
    SurfaceSample s;
    return SampleScene(objects, wx, wz, s) ? s.normal : glm::vec3(0.0f, 1.0f, 0.0f);
}
glm::vec4 LayerWeightAt(const std::vector<SceneObject>& objects, float wx, float wz) {
    SurfaceSample s;
    return SampleScene(objects, wx, wz, s) ? s.layers : glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
}
float KeepOutAt(const std::vector<SceneObject>& objects, float wx, float wz) {
    SurfaceSample s;
    return SampleScene(objects, wx, wz, s) ? s.keepOut : 0.0f;
}

void RegisterGroundQuery() {
    InstancerTools::SetGroundQuery([](const std::vector<SceneObject>& objects, float wx, float wz) {
        InstancerTools::GroundSample ground;
        SurfaceSample s;
        if (SampleScene(objects, wx, wz, s)) {
            ground.valid = true;
            ground.height = s.height;
            ground.normal = s.normal;
            ground.keepOut = s.keepOut;
        }
        return ground;
    });
}

bool Raycast(const SceneObject& obj, const glm::vec3& origin, const glm::vec3& direction,
             float maxDistance, float& hitDistance) {
    if (!IsActive(obj)) return false;
    const TerrainComponent& t = obj.terrain;
    const glm::mat4 inverse = glm::inverse(WorldMatrix(obj));
    const glm::vec3 o = glm::vec3(inverse * glm::vec4(origin, 1.0f));
    const glm::vec3 d = glm::vec3(inverse * glm::vec4(direction, 0.0f));
    const float half = t.size * 0.5f;
    const float minY = t.heightOffset;
    const float maxY = t.heightOffset + t.heightScale;
    // Slab clip against the terrain's local bounding box.
    float t0 = 0.0f;
    float t1 = maxDistance;
    const float lo[3] = {-half, minY, -half};
    const float hi[3] = {half, maxY, half};
    const float oo[3] = {o.x, o.y, o.z};
    const float dd[3] = {d.x, d.y, d.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(dd[axis]) < 1e-9f) {
            if (oo[axis] < lo[axis] || oo[axis] > hi[axis]) return false;
        } else {
            float a = (lo[axis] - oo[axis]) / dd[axis];
            float b = (hi[axis] - oo[axis]) / dd[axis];
            if (a > b) std::swap(a, b);
            t0 = std::max(t0, a);
            t1 = std::min(t1, b);
            if (t0 > t1) return false;
        }
    }
    const float horizontal = std::max(1e-6f, std::sqrt(d.x * d.x + d.z * d.z));
    const float step = std::max(0.02f, CellSize(t) * 0.5f) / std::max(horizontal, 1e-6f);
    const auto above = [&](float s) {
        const glm::vec3 p = o + d * s;
        return p.y - LocalHeightAt(t, p.x, p.z);
    };
    float prevT = t0;
    float prevF = above(prevT);
    if (prevF < 0.0f) {          // starts below the surface (camera under the terrain)
        hitDistance = t0;
        return false;
    }
    const int maxSteps = 20000;
    for (int i = 0; i < maxSteps && prevT < t1; ++i) {
        const float s = std::min(prevT + step, t1);
        const float f = above(s);
        if (f < 0.0f) {
            float a = prevT;
            float b = s;
            for (int k = 0; k < 14; ++k) {
                const float m = 0.5f * (a + b);
                if (above(m) < 0.0f) b = m; else a = m;
            }
            hitDistance = 0.5f * (a + b);
            return hitDistance <= maxDistance;
        }
        prevT = s;
        prevF = f;
    }
    (void)prevF;
    return false;
}

// ---- Generation -------------------------------------------------------------------------------

void ApplyClearings(TerrainComponent& terrain) {
    if (!terrain.hasData() || terrain.clearings.empty()) return;
    std::vector<float>& heights = terrain.mutableHeights();
    const int res = terrain.resolution;
    const float half = terrain.size * 0.5f;
    const float cell = CellSize(terrain);
    for (const TerrainClearing& c : terrain.clearings) {
        const float target = c.useHeight ? c.height : LocalHeightAt(terrain, c.center.x, c.center.y);
        const float reach = std::max(c.size.x, c.rectangle ? c.size.y : c.size.x) * 1.5f + c.blend + cell * 2.0f;
        const int x0 = std::max(0, static_cast<int>(std::floor((c.center.x - reach + half) / cell)));
        const int x1 = std::min(res - 1, static_cast<int>(std::ceil((c.center.x + reach + half) / cell)));
        const int z0 = std::max(0, static_cast<int>(std::floor((c.center.y - reach + half) / cell)));
        const int z1 = std::min(res - 1, static_cast<int>(std::ceil((c.center.y + reach + half) / cell)));
        const float targetNorm = NormalizedOf(terrain, target);
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const float lx = -half + static_cast<float>(x) * cell;
                const float lz = -half + static_cast<float>(z) * cell;
                const float sd = ClearingDistance(c, lx, lz);
                if (sd >= c.blend) continue;
                const float w = sd <= 0.0f ? 1.0f : 1.0f - Smoothstep(0.0f, std::max(c.blend, 0.001f), sd);
                float& h = heights[static_cast<size_t>(z) * static_cast<size_t>(res) + static_cast<size_t>(x)];
                h = h + (targetNorm - h) * w;
            }
        }
    }
    terrain.markAllChanged();
}

void CarvePonds(TerrainComponent& terrain, float depth) {
    if (!terrain.hasData() || terrain.ponds.empty()) return;
    std::vector<float>& heights = terrain.mutableHeights();
    const int res = terrain.resolution;
    const float half = terrain.size * 0.5f;
    const float cell = CellSize(terrain);
    for (const TerrainPond& pond : terrain.ponds) {
        const float reach = pond.radius * 1.6f + cell * 2.0f;
        const int x0 = std::max(0, static_cast<int>(std::floor((pond.center.x - reach + half) / cell)));
        const int x1 = std::min(res - 1, static_cast<int>(std::ceil((pond.center.x + reach + half) / cell)));
        const int z0 = std::max(0, static_cast<int>(std::floor((pond.center.y - reach + half) / cell)));
        const int z1 = std::min(res - 1, static_cast<int>(std::ceil((pond.center.y + reach + half) / cell)));
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const float lx = -half + static_cast<float>(x) * cell;
                const float lz = -half + static_cast<float>(z) * cell;
                const float dist = glm::length(glm::vec2(lx - pond.center.x, lz - pond.center.y));
                const float blend = Smoothstep(pond.radius * 0.35f, pond.radius * 1.35f, dist);
                if (blend >= 1.0f) continue;
                float& h = heights[static_cast<size_t>(z) * static_cast<size_t>(res) + static_cast<size_t>(x)];
                const float current = MetresOf(terrain, h);
                const float bank = std::max(current, pond.waterHeight + 0.18f);
                const float bottom = pond.waterHeight - depth * (1.0f - 0.35f * Smoothstep(0.0f, pond.radius, dist));
                const float carved = bottom + (bank - bottom) * blend;
                // Only ever dig: a bank already lower than the bowl keeps its height.
                if (dist < pond.radius * 1.35f && carved < current) h = NormalizedOf(terrain, carved);
            }
        }
    }
    terrain.markAllChanged();
}

void AutoPaint(TerrainComponent& terrain, float rockSlopeDegrees) {
    if (!terrain.hasData()) return;
    std::vector<uint8_t>& splat = terrain.mutableSplat();
    const int res = terrain.splatResolution;
    const float half = terrain.size * 0.5f;
    for (int j = 0; j < res; ++j) {
        for (int i = 0; i < res; ++i) {
            const float lx = (static_cast<float>(i) + 0.5f) / static_cast<float>(res) * terrain.size - half;
            const float lz = (static_cast<float>(j) + 0.5f) / static_cast<float>(res) * terrain.size - half;
            const float slope = SlopeDegreesAt(terrain, lx, lz);
            const float rock = Smoothstep(rockSlopeDegrees - 6.0f, rockSlopeDegrees + 6.0f, slope);
            float forest = Smoothstep(-0.05f, 0.35f,
                                      Fbm(lx * 0.025f, lz * 0.025f, static_cast<uint32_t>(terrain.genSeed) + 101u, 3, 0.5f, 0.0f));
            float shore = 0.0f;
            for (const TerrainPond& pond : terrain.ponds) {
                const float d = glm::length(glm::vec2(lx - pond.center.x, lz - pond.center.y)) - pond.radius;
                shore = std::max(shore, 1.0f - Smoothstep(0.0f, 4.0f, d));
            }
            forest = std::max(forest * 0.85f, shore);
            float w[4];
            w[3] = rock;
            w[2] = 0.0f;
            w[1] = (1.0f - rock) * forest;
            w[0] = (1.0f - rock) * (1.0f - forest);
            uint8_t* texel = &splat[(static_cast<size_t>(j) * static_cast<size_t>(res) + static_cast<size_t>(i)) * 4u];
            for (int c = 0; c < 4; ++c) texel[c] = static_cast<uint8_t>(std::clamp(w[c] * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }
    TerrainRect all;
    all.include(0, 0, res - 1, res - 1);
    terrain.markSplatChanged(all, true);
}

void Generate(TerrainComponent& terrain, const GenerateOptions& options) {
    if (!terrain.hasData()) InitializeFlat(terrain, options.baseHeightMetres);
    std::vector<float>& heights = terrain.mutableHeights();
    const int res = terrain.resolution;
    const float half = terrain.size * 0.5f;
    const float cell = CellSize(terrain);
    const float scale = std::max(1.0f, terrain.genScale);
    for (int z = 0; z < res; ++z) {
        for (int x = 0; x < res; ++x) {
            const float lx = -half + static_cast<float>(x) * cell;
            const float lz = -half + static_cast<float>(z) * cell;
            const float n = Fbm(lx / scale, lz / scale, static_cast<uint32_t>(terrain.genSeed), terrain.genOctaves,
                                std::clamp(terrain.genPersistence, 0.05f, 0.95f), std::clamp(terrain.genRidged, 0.0f, 1.0f));
            heights[static_cast<size_t>(z) * static_cast<size_t>(res) + static_cast<size_t>(x)] =
                NormalizedOf(terrain, options.baseHeightMetres + n * terrain.genAmplitude);
        }
    }
    terrain.markAllChanged();
    if (options.applyClearings) ApplyClearings(terrain);
    if (options.carvePonds) CarvePonds(terrain, options.pondDepth);
    if (options.autoPaint) AutoPaint(terrain, options.rockSlopeDegrees);
    terrain.markAllChanged();
}

void PaintLine(TerrainComponent& terrain, glm::vec2 a, glm::vec2 b, float width, int layer, float strength) {
    if (!terrain.hasData() || layer < 0 || layer > 3) return;
    std::vector<uint8_t>& splat = terrain.mutableSplat();
    const int res = terrain.splatResolution;
    const float half = terrain.size * 0.5f;
    const float texel = terrain.size / static_cast<float>(res);
    const float reach = width * 0.5f + width * 0.35f + texel;
    const float minX = std::min(a.x, b.x) - reach;
    const float maxX = std::max(a.x, b.x) + reach;
    const float minZ = std::min(a.y, b.y) - reach;
    const float maxZ = std::max(a.y, b.y) + reach;
    const int i0 = std::max(0, static_cast<int>(std::floor((minX + half) / texel)));
    const int i1 = std::min(res - 1, static_cast<int>(std::ceil((maxX + half) / texel)));
    const int j0 = std::max(0, static_cast<int>(std::floor((minZ + half) / texel)));
    const int j1 = std::min(res - 1, static_cast<int>(std::ceil((maxZ + half) / texel)));
    const glm::vec2 ab = b - a;
    const float len2 = std::max(glm::dot(ab, ab), 1e-6f);
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            const glm::vec2 p((static_cast<float>(i) + 0.5f) * texel - half, (static_cast<float>(j) + 0.5f) * texel - half);
            const float s = std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
            const float d = glm::length(p - (a + ab * s));
            const float w = 1.0f - Smoothstep(width * 0.5f, width * 0.5f + width * 0.35f, d);
            if (w <= 0.0f) continue;
            PushLayerWeight(&splat[(static_cast<size_t>(j) * static_cast<size_t>(res) + static_cast<size_t>(i)) * 4u],
                            layer, w * strength);
        }
    }
    terrain.markAllChanged();
}

// ---- Brushes ----------------------------------------------------------------------------------

void ApplyBrush(TerrainComponent& terrain, float lx, float lz, const BrushParams& params, bool newStroke) {
    if (!terrain.hasData()) return;
    const float half = terrain.size * 0.5f;
    const float radius = std::max(0.25f, params.radius);
    if (params.mode == BrushMode::PaintLayer || params.mode == BrushMode::GrassDensity) {
        const int res = terrain.splatResolution;
        const float texel = terrain.size / static_cast<float>(res);
        const int i0 = std::max(0, static_cast<int>(std::floor((lx - radius + half) / texel)));
        const int i1 = std::min(res - 1, static_cast<int>(std::ceil((lx + radius + half) / texel)));
        const int j0 = std::max(0, static_cast<int>(std::floor((lz - radius + half) / texel)));
        const int j1 = std::min(res - 1, static_cast<int>(std::ceil((lz + radius + half) / texel)));
        if (i1 < i0 || j1 < j0) return;
        const bool density = params.mode == BrushMode::GrassDensity;
        std::vector<uint8_t>& data = density ? MutableGrassDensity(terrain) : terrain.mutableSplat();
        const int layer = std::clamp(params.layer, 0, 3);
        for (int j = j0; j <= j1; ++j) {
            for (int i = i0; i <= i1; ++i) {
                const float px = (static_cast<float>(i) + 0.5f) * texel - half;
                const float pz = (static_cast<float>(j) + 0.5f) * texel - half;
                const float w = BrushWeight(glm::length(glm::vec2(px - lx, pz - lz)), radius, params.falloff);
                if (w <= 0.0f) continue;
                const size_t index = static_cast<size_t>(j) * static_cast<size_t>(res) + static_cast<size_t>(i);
                if (density) {
                    const float delta = (params.invert ? -1.0f : 1.0f) * params.strength * 0.6f * w * 255.0f;
                    data[index] = static_cast<uint8_t>(std::clamp(static_cast<float>(data[index]) + delta, 0.0f, 255.0f));
                } else {
                    PushLayerWeight(&data[index * 4u], layer, params.strength * 0.5f * w);
                }
            }
        }
        TerrainRect rect;
        rect.include(i0, j0, i1, j1);
        terrain.markSplatChanged(rect, newStroke);
        return;
    }

    const int res = terrain.resolution;
    const float cell = CellSize(terrain);
    const int x0 = std::max(0, static_cast<int>(std::floor((lx - radius + half) / cell)));
    const int x1 = std::min(res - 1, static_cast<int>(std::ceil((lx + radius + half) / cell)));
    const int z0 = std::max(0, static_cast<int>(std::floor((lz - radius + half) / cell)));
    const int z1 = std::min(res - 1, static_cast<int>(std::ceil((lz + radius + half) / cell)));
    if (x1 < x0 || z1 < z0) return;
    std::vector<float>& heights = terrain.mutableHeights();
    // Smooth reads a copy of the neighbourhood so the blur has no directional bias.
    std::vector<float> source;
    const int sx0 = std::max(0, x0 - 1);
    const int sz0 = std::max(0, z0 - 1);
    const int sx1 = std::min(res - 1, x1 + 1);
    const int sz1 = std::min(res - 1, z1 + 1);
    const int sw = sx1 - sx0 + 1;
    if (params.mode == BrushMode::Smooth) {
        source.resize(static_cast<size_t>(sw) * static_cast<size_t>(sz1 - sz0 + 1));
        for (int z = sz0; z <= sz1; ++z) {
            for (int x = sx0; x <= sx1; ++x) {
                source[static_cast<size_t>(z - sz0) * static_cast<size_t>(sw) + static_cast<size_t>(x - sx0)] =
                    heights[static_cast<size_t>(z) * static_cast<size_t>(res) + static_cast<size_t>(x)];
            }
        }
    }
    const float perStampMetres = 0.25f;
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            const float px = -half + static_cast<float>(x) * cell;
            const float pz = -half + static_cast<float>(z) * cell;
            const float w = BrushWeight(glm::length(glm::vec2(px - lx, pz - lz)), radius, params.falloff);
            if (w <= 0.0f) continue;
            float& h = heights[static_cast<size_t>(z) * static_cast<size_t>(res) + static_cast<size_t>(x)];
            switch (params.mode) {
                case BrushMode::Raise:
                    h += params.strength * perStampMetres * w / std::max(terrain.heightScale, 1e-3f);
                    break;
                case BrushMode::Lower:
                    h -= params.strength * perStampMetres * w / std::max(terrain.heightScale, 1e-3f);
                    break;
                case BrushMode::Flatten: {
                    const float target = NormalizedOf(terrain, params.flattenHeight);
                    h += (target - h) * std::clamp(params.strength * 0.5f * w * 2.0f, 0.0f, 1.0f);
                    break;
                }
                case BrushMode::Smooth: {
                    float sum = 0.0f;
                    int count = 0;
                    for (int dz = -1; dz <= 1; ++dz) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = std::clamp(x + dx, sx0, sx1);
                            const int nz = std::clamp(z + dz, sz0, sz1);
                            sum += source[static_cast<size_t>(nz - sz0) * static_cast<size_t>(sw) + static_cast<size_t>(nx - sx0)];
                            ++count;
                        }
                    }
                    h += (sum / static_cast<float>(count) - h) * std::clamp(params.strength * w * 1.2f, 0.0f, 1.0f);
                    break;
                }
                default:
                    break;
            }
            h = std::clamp(h, 0.0f, 1.0f);
        }
    }
    TerrainRect rect;
    rect.include(x0, z0, x1, z1);
    terrain.markHeightsChanged(rect, newStroke);
}

} // namespace TerrainRuntime
