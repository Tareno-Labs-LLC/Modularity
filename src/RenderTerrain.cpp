// Terrain rendering: the Renderer half of the Terrain component (TerrainRuntime.h has the data).
//
// The ground is split into 32x32-quad chunks. Each chunk keeps up to three non-indexed meshes
// (every sample, every 2nd, every 4th) built lazily the first time a view asks for them, with a
// skirt around the edge so neighbouring chunks at different LODs never show cracks. Meshes are in
// terrain-local space; the terrain object's world matrix is the model matrix, and the vertices
// carry the standard 8-float layout with TexCoord = position across the terrain (0..1), so the
// engine's own vert.glsl and frag.glsl (compiled with MODU_TERRAIN, see the shader) draw it with
// shadows, image based lighting, fog and point lights unchanged.
//
// The splat map is one RGBA8 texture; the four layers are 2D texture arrays resampled to a common
// size (layers without a texture get a procedural grass / forest floor / path / rock tile).

#include "RenderTerrainInternal.h"

#include "InstancerRuntime.h"
#include "Profiler.h"
#include "RenderOptimizationInternal.h"
#include "TerrainRuntime.h"
#include "../include/Platform/AssetSource.h"
#include "../include/Textures/StbFlipScope.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {

constexpr int kLayerSize = 1024;       // Texels per side of every layer in the arrays.
constexpr int kMaxChunkBuildsPerFrame = 40;
constexpr uint64_t kEntryRetireFrames = 512;

uint64_t Mix(uint64_t seed, uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    return seed;
}
uint64_t BitsOf(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
uint64_t HashString(uint64_t seed, const std::string& text) {
    for (char c : text) seed = Mix(seed, static_cast<unsigned char>(c));
    return Mix(seed, text.size());
}

float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }

// ---- Procedural layer tiles --------------------------------------------------------------------

// Tileable value noise: lattice of `period` cells that wraps.
float TileHash(int x, int y, int period, uint32_t seed) {
    x = ((x % period) + period) % period;
    y = ((y % period) + period) % period;
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000);
}
float TileNoise(float x, float y, int period, uint32_t seed) {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float fx = Smooth(x - static_cast<float>(xi));
    const float fy = Smooth(y - static_cast<float>(yi));
    const float a = TileHash(xi, yi, period, seed);
    const float b = TileHash(xi + 1, yi, period, seed);
    const float c = TileHash(xi, yi + 1, period, seed);
    const float d = TileHash(xi + 1, yi + 1, period, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}
// Tileable fBm over the unit square: `cells` lattice cells across at the first octave.
float TileFbm(float u, float v, int cells, int octaves, uint32_t seed) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    int period = cells;
    for (int i = 0; i < octaves; ++i) {
        sum += TileNoise(u * static_cast<float>(period), v * static_cast<float>(period), period, seed + static_cast<uint32_t>(i) * 31u) * amp;
        norm += amp;
        amp *= 0.5f;
        period *= 2;
    }
    return sum / norm;
}

struct LayerImage {
    std::vector<uint8_t> albedo;   // RGBA8, rgb gamma colour, a height proxy
    std::vector<uint8_t> normal;   // RGBA8, rg tangent normal xy, b roughness, a ambient occlusion
};

void ColorOf(float r, float g, float b, uint8_t* out) {
    out[0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
    out[1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
    out[2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Slot defaults: 0 grass, 1 forest floor, 2 packed path, 3 rock. `height` is 0..1 relief used for the
// height proxy and the normal map.
void ProceduralLayer(int layer, int size, LayerImage& out) {
    const size_t texels = static_cast<size_t>(size) * static_cast<size_t>(size);
    out.albedo.assign(texels * 4, 255);
    out.normal.assign(texels * 4, 255);
    std::vector<float> relief(texels, 0.0f);
    const float inv = 1.0f / static_cast<float>(size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) * inv;
            const float v = (static_cast<float>(y) + 0.5f) * inv;
            float color[3] = {0.5f, 0.5f, 0.5f};
            float h = 0.5f;
            switch (layer) {
                case 0: {   // grass: layered greens with fine vertical blade streaks and dry specks
                    const float broad = TileFbm(u, v, 4, 4, 11);
                    const float fine = TileFbm(u * 2.0f, v, 64, 3, 23);       // stretched along v: blade streaks
                    const float speck = TileFbm(u, v, 96, 2, 37);
                    const float dry = std::clamp((TileFbm(u, v, 3, 3, 53) - 0.5f) * 3.0f + 0.5f, 0.0f, 1.0f);
                    const float tone = 0.35f + broad * 0.5f + (fine - 0.5f) * 0.55f;
                    const float rgb[3] = {0.17f + tone * 0.22f, 0.30f + tone * 0.34f, 0.07f + tone * 0.10f};
                    const float dryRgb[3] = {0.42f, 0.40f, 0.14f};
                    for (int c = 0; c < 3; ++c) {
                        color[c] = rgb[c] + (dryRgb[c] - rgb[c]) * dry * 0.35f;
                        if (speck > 0.78f) color[c] = color[c] * 0.7f + (c == 0 ? 0.20f : c == 1 ? 0.17f : 0.05f) * 0.3f;
                    }
                    h = std::clamp(0.3f + fine * 0.6f + broad * 0.2f, 0.0f, 1.0f);
                    break;
                }
                case 1: {   // forest floor: dark soil with leaf litter and pebbles
                    const float broad = TileFbm(u, v, 5, 4, 71);
                    const float litter = TileFbm(u, v, 40, 3, 83);
                    const float pebble = std::clamp((TileFbm(u, v, 24, 2, 97) - 0.62f) * 6.0f, 0.0f, 1.0f);
                    const float tone = 0.45f + broad * 0.5f + (litter - 0.5f) * 0.5f;
                    color[0] = 0.26f + tone * 0.22f + pebble * 0.12f;
                    color[1] = 0.20f + tone * 0.16f + pebble * 0.10f;
                    color[2] = 0.13f + tone * 0.09f + pebble * 0.08f;
                    h = std::clamp(0.35f + litter * 0.45f + pebble * 0.3f, 0.0f, 1.0f);
                    break;
                }
                case 2: {   // path: compacted pale earth, fine grit and wheel-worn streaks
                    const float broad = TileFbm(u, v, 3, 4, 131);
                    const float grit = TileFbm(u, v, 80, 3, 149);
                    const float tone = 0.5f + (broad - 0.5f) * 0.6f + (grit - 0.5f) * 0.5f;
                    color[0] = 0.43f + tone * 0.22f;
                    color[1] = 0.34f + tone * 0.18f;
                    color[2] = 0.23f + tone * 0.14f;
                    h = std::clamp(0.4f + grit * 0.45f, 0.0f, 1.0f);
                    break;
                }
                default: {  // rock: grey stone with strata and cracks
                    const float broad = TileFbm(u, v, 4, 5, 181);
                    const float strata = TileFbm(u * 0.5f, v * 6.0f, 6, 2, 191);
                    const float crack = std::fabs(TileFbm(u, v, 6, 4, 211) - 0.5f) * 2.0f;
                    const float tone = 0.42f + (broad - 0.5f) * 0.55f + (strata - 0.5f) * 0.25f;
                    const float darker = crack < 0.07f ? 0.55f : 1.0f;
                    color[0] = (0.36f + tone * 0.30f) * darker;
                    color[1] = (0.35f + tone * 0.30f) * darker;
                    color[2] = (0.33f + tone * 0.28f) * darker;
                    h = std::clamp(0.35f + broad * 0.5f + (crack < 0.07f ? -0.25f : 0.0f), 0.0f, 1.0f);
                    break;
                }
            }
            const size_t index = static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x);
            ColorOf(color[0], color[1], color[2], &out.albedo[index * 4]);
            out.albedo[index * 4 + 3] = static_cast<uint8_t>(std::clamp(h, 0.0f, 1.0f) * 255.0f + 0.5f);
            relief[index] = h;
        }
    }
    // Normal map from the relief (wraps, so the tile stays seamless).
    static const float strength[4] = {2.5f, 3.5f, 2.5f, 6.0f};
    static const float rough[4] = {0.92f, 0.95f, 0.9f, 0.82f};
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const auto at = [&](int px, int py) {
                px = (px + size) % size;
                py = (py + size) % size;
                return relief[static_cast<size_t>(py) * static_cast<size_t>(size) + static_cast<size_t>(px)];
            };
            const float dx = (at(x + 1, y) - at(x - 1, y)) * strength[layer];
            const float dy = (at(x, y + 1) - at(x, y - 1)) * strength[layer];
            // Image-space +Y is toward row 0 (GL convention), so the row gradient flips sign.
            float nx = -dx, ny = dy, nz = 1.0f;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= len; ny /= len;
            uint8_t* out4 = &out.normal[(static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4];
            out4[0] = static_cast<uint8_t>((nx * 0.5f + 0.5f) * 255.0f + 0.5f);
            out4[1] = static_cast<uint8_t>((ny * 0.5f + 0.5f) * 255.0f + 0.5f);
            out4[2] = static_cast<uint8_t>(rough[layer] * 255.0f + 0.5f);
            const float ao = 0.78f + 0.22f * at(x, y);
            out4[3] = static_cast<uint8_t>(std::clamp(ao, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
}

// ---- Image loading ------------------------------------------------------------------------------

struct Rgba {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

bool DecodeImage(const std::string& path, Rgba& out) {
    if (path.empty()) return false;
    const std::vector<uint8_t> bytes = Modularity::Platform::GetAssetSource().ReadAll(path);
    if (bytes.empty()) return false;
    StbFlipScope noFlip(false);
    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (!data || w <= 0 || h <= 0) {
        if (data) stbi_image_free(data);
        return false;
    }
    out.width = w;
    out.height = h;
    out.pixels.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    stbi_image_free(data);
    return true;
}

// Box-halve until at most 2x the target, then bilinear (wrapping, since layers tile).
void ResampleTo(const Rgba& src, int size, std::vector<uint8_t>& dst) {
    std::vector<uint8_t> cur = src.pixels;
    int w = src.width;
    int h = src.height;
    while (w >= size * 2 && h >= size * 2) {
        const int nw = w / 2;
        const int nh = h / 2;
        std::vector<uint8_t> next(static_cast<size_t>(nw) * static_cast<size_t>(nh) * 4u);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const auto at = [&](int px, int py) {
                        return static_cast<int>(cur[(static_cast<size_t>(py) * static_cast<size_t>(w) + static_cast<size_t>(px)) * 4u + static_cast<size_t>(c)]);
                    };
                    next[(static_cast<size_t>(y) * static_cast<size_t>(nw) + static_cast<size_t>(x)) * 4u + static_cast<size_t>(c)] =
                        static_cast<uint8_t>((at(x * 2, y * 2) + at(x * 2 + 1, y * 2) + at(x * 2, y * 2 + 1) + at(x * 2 + 1, y * 2 + 1) + 2) / 4);
                }
            }
        }
        cur.swap(next);
        w = nw;
        h = nh;
    }
    dst.assign(static_cast<size_t>(size) * static_cast<size_t>(size) * 4u, 255);
    for (int y = 0; y < size; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * static_cast<float>(h) - 0.5f;
        const int y0 = static_cast<int>(std::floor(fy));
        const float ty = fy - static_cast<float>(y0);
        const int ya = ((y0 % h) + h) % h;
        const int yb = (ya + 1) % h;
        for (int x = 0; x < size; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * static_cast<float>(w) - 0.5f;
            const int x0 = static_cast<int>(std::floor(fx));
            const float tx = fx - static_cast<float>(x0);
            const int xa = ((x0 % w) + w) % w;
            const int xb = (xa + 1) % w;
            for (int c = 0; c < 4; ++c) {
                const auto at = [&](int px, int py) {
                    return static_cast<float>(cur[(static_cast<size_t>(py) * static_cast<size_t>(w) + static_cast<size_t>(px)) * 4u + static_cast<size_t>(c)]);
                };
                const float v = (at(xa, ya) * (1 - tx) + at(xb, ya) * tx) * (1 - ty) +
                                (at(xa, yb) * (1 - tx) + at(xb, yb) * tx) * ty;
                dst[(static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4u + static_cast<size_t>(c)] =
                    static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
            }
        }
    }
}

bool LooksDirectX(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("normaldx") != std::string::npos || lower.find("_dx.") != std::string::npos;
}

void BuildLayer(int layer, const TerrainLayer& settings, LayerImage& out, bool& usedAnyFile) {
    ProceduralLayer(layer, kLayerSize, out);
    Rgba image;
    if (DecodeImage(settings.albedoPath, image)) {
        std::vector<uint8_t> resized;
        ResampleTo(image, kLayerSize, resized);
        for (size_t i = 0; i < resized.size(); i += 4) {
            out.albedo[i] = resized[i];
            out.albedo[i + 1] = resized[i + 1];
            out.albedo[i + 2] = resized[i + 2];
            // Height proxy: brightness (dark crevices read low).
            const float luma = (0.299f * resized[i] + 0.587f * resized[i + 1] + 0.114f * resized[i + 2]) / 255.0f;
            out.albedo[i + 3] = static_cast<uint8_t>(std::clamp(luma, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        usedAnyFile = true;
        // The procedural normal no longer matches the picture; start flat and let the file override.
        for (size_t i = 0; i < out.normal.size(); i += 4) {
            out.normal[i] = 128; out.normal[i + 1] = 128;
        }
    } else if (!settings.albedoPath.empty()) {
        std::cerr << "[Terrain] layer " << layer << " albedo could not be read: " << settings.albedoPath
                  << " (using the procedural tile)\n";
    }
    if (DecodeImage(settings.normalPath, image)) {
        std::vector<uint8_t> resized;
        ResampleTo(image, kLayerSize, resized);
        const bool flipG = LooksDirectX(settings.normalPath);
        for (size_t i = 0; i < resized.size(); i += 4) {
            out.normal[i] = resized[i];
            out.normal[i + 1] = flipG ? static_cast<uint8_t>(255 - resized[i + 1]) : resized[i + 1];
        }
        usedAnyFile = true;
        const std::string roughPath = TerrainRuntime::SiblingMapPath(settings.normalPath, "Roughness");
        Rgba roughImage;
        if (!roughPath.empty() && DecodeImage(roughPath, roughImage)) {
            std::vector<uint8_t> r;
            ResampleTo(roughImage, kLayerSize, r);
            for (size_t i = 0; i < r.size(); i += 4) out.normal[i + 2] = r[i];
        }
        const std::string aoPath = TerrainRuntime::SiblingMapPath(settings.normalPath, "AmbientOcclusion");
        Rgba aoImage;
        if (!aoPath.empty() && DecodeImage(aoPath, aoImage)) {
            std::vector<uint8_t> a;
            ResampleTo(aoImage, kLayerSize, a);
            for (size_t i = 0; i < a.size(); i += 4) out.normal[i + 3] = static_cast<uint8_t>(128 + a[i] / 2);
        }
    } else if (!settings.normalPath.empty()) {
        std::cerr << "[Terrain] layer " << layer << " normal map could not be read: " << settings.normalPath << "\n";
    }
}

GLuint UploadArray(const std::vector<uint8_t>* layers[4]) {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D_ARRAY, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, kLayerSize, kLayerSize, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    for (int i = 0; i < 4; ++i) {
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, kLayerSize, kLayerSize, 1, GL_RGBA, GL_UNSIGNED_BYTE, layers[i]->data());
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    // Anisotropic filtering where the driver has it (GL_TEXTURE_MAX_ANISOTROPY, core in 4.6 and an
    // extension long before); ignore the error when it does not.
    GLfloat maxAniso = 0.0f;
    while (glGetError() != GL_NO_ERROR) {}
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
    if (glGetError() == GL_NO_ERROR && maxAniso > 1.0f) {
        glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY, std::min(maxAniso, 8.0f));
    }
    while (glGetError() != GL_NO_ERROR) {}
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    return id;
}

uint64_t LayerSignature(const TerrainComponent& t) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 4; ++i) {
        h = HashString(h, t.layers[i].albedoPath);
        h = HashString(h, t.layers[i].normalPath);
    }
    return h;
}

// ---- Chunk meshes ---------------------------------------------------------------------------------

struct MeshBuilder {
    const TerrainComponent& t;
    int res;
    float half;
    float cell;
    std::vector<float> data;

    explicit MeshBuilder(const TerrainComponent& terrain)
        : t(terrain), res(terrain.resolution), half(terrain.size * 0.5f), cell(TerrainRuntime::CellSize(terrain)) {}

    float H(int x, int z) const {
        x = std::clamp(x, 0, res - 1);
        z = std::clamp(z, 0, res - 1);
        return t.heightOffset + (*t.heights)[static_cast<size_t>(z) * static_cast<size_t>(res) + static_cast<size_t>(x)] * t.heightScale;
    }
    void vertex(int x, int z, float yDrop) {
        const float px = -half + static_cast<float>(x) * cell;
        const float pz = -half + static_cast<float>(z) * cell;
        // Central differences over the full-resolution grid: LODs share one smooth normal field.
        const int xl = std::max(x - 1, 0), xr = std::min(x + 1, res - 1);
        const int zl = std::max(z - 1, 0), zr = std::min(z + 1, res - 1);
        const float dhx = (H(xr, z) - H(xl, z)) / (static_cast<float>(xr - xl) * cell);
        const float dhz = (H(x, zr) - H(x, zl)) / (static_cast<float>(zr - zl) * cell);
        const float inv = 1.0f / std::sqrt(dhx * dhx + 1.0f + dhz * dhz);
        data.push_back(px);
        data.push_back(H(x, z) - yDrop);
        data.push_back(pz);
        data.push_back(-dhx * inv);
        data.push_back(inv);
        data.push_back(-dhz * inv);
        data.push_back(static_cast<float>(x) * cell / t.size);
        data.push_back(static_cast<float>(z) * cell / t.size);
    }
    void tri(int ax, int az, float ad, int bx, int bz, float bd, int cx, int cz, float cd) {
        vertex(ax, az, ad);
        vertex(bx, bz, bd);
        vertex(cx, cz, cd);
    }
};

std::unique_ptr<Mesh> BuildChunkMesh(const TerrainComponent& t, int chunkX, int chunkZ, int lod) {
    MeshBuilder b(t);
    const int step = 1 << lod;
    const int x0 = chunkX * TerrainRuntime::kChunkQuads;
    const int z0 = chunkZ * TerrainRuntime::kChunkQuads;
    const int x1 = std::min(x0 + TerrainRuntime::kChunkQuads, t.resolution - 1);
    const int z1 = std::min(z0 + TerrainRuntime::kChunkQuads, t.resolution - 1);
    if (x1 <= x0 || z1 <= z0) return nullptr;
    std::vector<int> xs, zs;
    for (int x = x0; x < x1; x += step) xs.push_back(x);
    xs.push_back(x1);
    for (int z = z0; z < z1; z += step) zs.push_back(z);
    zs.push_back(z1);
    const float skirt = b.cell * static_cast<float>(step) * 1.2f + 0.3f;
    b.data.reserve((xs.size() * zs.size() * 6 + (xs.size() + zs.size()) * 24) * 8);
    for (size_t j = 0; j + 1 < zs.size(); ++j) {
        for (size_t i = 0; i + 1 < xs.size(); ++i) {
            const int ax = xs[i], bx = xs[i + 1], az = zs[j], cz = zs[j + 1];
            // a=(x,z) b=(x+1,z) c=(x,z+1) d=(x+1,z+1); diagonal a-d, counter-clockwise from above.
            b.tri(ax, az, 0, ax, cz, 0, bx, cz, 0);
            b.tri(ax, az, 0, bx, cz, 0, bx, az, 0);
        }
    }
    // Skirts hide the cracks between chunks drawn at different LODs. Both windings: they are only
    // ever seen through a gap, from whichever side.
    const auto skirtQuad = [&](int px, int pz, int qx, int qz) {
        b.tri(px, pz, 0, px, pz, skirt, qx, qz, skirt);
        b.tri(px, pz, 0, qx, qz, skirt, qx, qz, 0);
        b.tri(px, pz, 0, qx, qz, skirt, px, pz, skirt);
        b.tri(px, pz, 0, qx, qz, 0, qx, qz, skirt);
    };
    for (size_t i = 0; i + 1 < xs.size(); ++i) {
        skirtQuad(xs[i], z0, xs[i + 1], z0);
        skirtQuad(xs[i], z1, xs[i + 1], z1);
    }
    for (size_t j = 0; j + 1 < zs.size(); ++j) {
        skirtQuad(x0, zs[j], x0, zs[j + 1]);
        skirtQuad(x1, zs[j], x1, zs[j + 1]);
    }
    return std::make_unique<Mesh>(b.data.data(), b.data.size() * sizeof(float));
}

void ComputeChunkBounds(const TerrainComponent& t, int chunkX, int chunkZ, float& minY, float& maxY) {
    const int res = t.resolution;
    const int x0 = chunkX * TerrainRuntime::kChunkQuads;
    const int z0 = chunkZ * TerrainRuntime::kChunkQuads;
    const int x1 = std::min(x0 + TerrainRuntime::kChunkQuads, res - 1);
    const int z1 = std::min(z0 + TerrainRuntime::kChunkQuads, res - 1);
    float lo = 1.0f, hi = 0.0f;
    for (int z = z0; z <= z1; ++z) {
        const float* row = t.heights->data() + static_cast<size_t>(z) * static_cast<size_t>(res);
        for (int x = x0; x <= x1; ++x) {
            lo = std::min(lo, row[x]);
            hi = std::max(hi, row[x]);
        }
    }
    // The skirt hangs below the lowest sample.
    minY = t.heightOffset + lo * t.heightScale - 2.0f;
    maxY = t.heightOffset + hi * t.heightScale;
}

} // namespace

// ---- TerrainState ---------------------------------------------------------------------------------

size_t Renderer::TerrainState::gather(Renderer& renderer, const std::vector<SceneObject>& objects,
                                      const glm::vec3& cameraPosition, const std::array<glm::vec4, 6>& planes,
                                      uint32_t cullingMask, const ShadowQuery* shadow,
                                      std::vector<Mesh*>& pool, std::vector<Batch>& batches, uint64_t* signature) {
    (void)renderer;
    pool.clear();
    batches.clear();
    if (signature) *signature = 0;
    if (!shadow) {
        ++epoch;
        buildBudget = kMaxChunkBuildsPerFrame;
    }
    for (size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const SceneObject& object = objects[objectIndex];
        if (!object.hasTerrain || !TerrainRuntime::IsActive(object)) continue;
        const TerrainComponent& t = object.terrain;
        if (shadow && !t.castShadows) continue;
        if (!shadow && cullingMask != 0xFFFFFFFFu &&
            (cullingMask & (1u << (static_cast<uint32_t>(object.layer) & 31u))) == 0u) continue;

        Entry& e = entries[object.id];
        e.lastUse = epoch;
        const int chunksPerSide = TerrainRuntime::ChunkCountPerSide(t);
        const bool layoutChanged = e.resolution != t.resolution || e.chunksPerSide != chunksPerSide ||
                                   e.size != t.size || e.heightScale != t.heightScale ||
                                   e.heightOffset != t.heightOffset || e.chunks.empty();
        if (layoutChanged) {
            e.chunks.clear();
            e.chunks.resize(static_cast<size_t>(chunksPerSide) * static_cast<size_t>(chunksPerSide));
            e.resolution = t.resolution;
            e.chunksPerSide = chunksPerSide;
            e.size = t.size;
            e.heightScale = t.heightScale;
            e.heightOffset = t.heightOffset;
            for (int cz = 0; cz < chunksPerSide; ++cz) {
                for (int cx = 0; cx < chunksPerSide; ++cx) {
                    Chunk& c = e.chunks[static_cast<size_t>(cz) * static_cast<size_t>(chunksPerSide) + static_cast<size_t>(cx)];
                    ComputeChunkBounds(t, cx, cz, c.minY, c.maxY);
                }
            }
            e.heightRevision = t.heightRevision;
            e.heightsBuffer = t.heights.get();
        } else if (e.heightRevision != t.heightRevision || e.heightsBuffer != t.heights.get()) {
            // Edited. Rebuild only what the dirty rectangle touches when this entry saw the revision
            // the rectangle started from; anything else (undo, load, generate) refreshes every chunk.
            const bool partial = !t.heightDirty.empty() && e.heightRevision >= t.heightDirtyBase &&
                                 e.heightRevision <= t.heightRevision && e.heightsBuffer == t.heights.get();
            const int minCx = partial ? std::max(0, (t.heightDirty.minX - 1) / TerrainRuntime::kChunkQuads) : 0;
            const int maxCx = partial ? std::min(chunksPerSide - 1, (t.heightDirty.maxX + 1) / TerrainRuntime::kChunkQuads) : chunksPerSide - 1;
            const int minCz = partial ? std::max(0, (t.heightDirty.minZ - 1) / TerrainRuntime::kChunkQuads) : 0;
            const int maxCz = partial ? std::min(chunksPerSide - 1, (t.heightDirty.maxZ + 1) / TerrainRuntime::kChunkQuads) : chunksPerSide - 1;
            for (int cz = minCz; cz <= maxCz; ++cz) {
                for (int cx = minCx; cx <= maxCx; ++cx) {
                    Chunk& c = e.chunks[static_cast<size_t>(cz) * static_cast<size_t>(chunksPerSide) + static_cast<size_t>(cx)];
                    for (int l = 0; l < kLodCount; ++l) c.stale[l] = c.mesh[l] != nullptr;
                    ComputeChunkBounds(t, cx, cz, c.minY, c.maxY);
                }
            }
            e.heightRevision = t.heightRevision;
            e.heightsBuffer = t.heights.get();
        }

        const glm::mat4 model = TerrainRuntime::WorldMatrix(object);
        const glm::mat3 absModel = glm::mat3(glm::abs(model[0]), glm::abs(model[1]), glm::abs(model[2]));
        const float half = t.size * 0.5f;
        const float chunkSize = TerrainRuntime::CellSize(t) * static_cast<float>(TerrainRuntime::kChunkQuads);
        const float lodNear = std::max(1.0f, t.lodDistance);

        Batch batch;
        batch.objectIndex = static_cast<int>(objectIndex);
        batch.entryId = object.id;
        batch.model = model;
        batch.first = pool.size();
        uint64_t sig = Mix(static_cast<uint64_t>(object.id), t.heightRevision);
        for (int cz = 0; cz < chunksPerSide; ++cz) {
            for (int cx = 0; cx < chunksPerSide; ++cx) {
                Chunk& c = e.chunks[static_cast<size_t>(cz) * static_cast<size_t>(chunksPerSide) + static_cast<size_t>(cx)];
                const float lx0 = -half + static_cast<float>(cx) * chunkSize;
                const float lz0 = -half + static_cast<float>(cz) * chunkSize;
                const float lx1 = std::min(lx0 + chunkSize, half);
                const float lz1 = std::min(lz0 + chunkSize, half);
                const glm::vec3 localCenter(0.5f * (lx0 + lx1), 0.5f * (c.minY + c.maxY), 0.5f * (lz0 + lz1));
                const glm::vec3 localExtents(0.5f * (lx1 - lx0), 0.5f * (c.maxY - c.minY), 0.5f * (lz1 - lz0));
                const glm::vec3 center = glm::vec3(model * glm::vec4(localCenter, 1.0f));
                const glm::vec3 extents = absModel * localExtents;
                if (!RenderOpt::AabbInsidePlanes(planes, center, extents)) continue;
                if (shadow && shadow->sphere) {
                    const glm::vec3 gap = glm::max(glm::abs(center - shadow->sphereCenter) - extents, glm::vec3(0.0f));
                    if (glm::dot(gap, gap) > shadow->sphereRadius * shadow->sphereRadius) continue;
                }
                const glm::vec3 gapToCamera = glm::max(glm::abs(center - cameraPosition) - extents, glm::vec3(0.0f));
                const float distance = glm::length(gapToCamera);
                int lod = distance < lodNear ? 0 : (distance < lodNear * 2.5f ? 1 : 2);
                // Shadow casters use the same LOD the camera pass draws: a coarser caster sits above
                // the visible surface wherever the ground is concave, and the terrain then shadows
                // itself in cell-sized blocks once the shadow map resolves that height difference.

                // Prefer the wanted LOD; build it within the per-frame budget, else fall back to any
                // built LOD (stale meshes still draw until their rebuild fits the budget).
                int use = -1;
                const bool wanted = c.mesh[lod] != nullptr && !c.stale[lod];
                if (wanted) {
                    use = lod;
                } else if (buildBudget > 0) {
                    --buildBudget;
                    std::unique_ptr<Mesh> built = BuildChunkMesh(t, cx, cz, lod);
                    if (built) {
                        c.mesh[lod] = std::move(built);
                        c.stale[lod] = false;
                        use = lod;
                    }
                }
                if (use < 0 && c.mesh[lod]) use = lod;   // stale but better than nothing
                if (use < 0) {
                    for (int d = 1; d < kLodCount && use < 0; ++d) {
                        const int a = lod - d, b2 = lod + d;
                        if (a >= 0 && c.mesh[a]) use = a;
                        else if (b2 < kLodCount && c.mesh[b2]) use = b2;
                    }
                }
                if (use < 0) continue;
                Mesh* mesh = c.mesh[use].get();
                pool.push_back(mesh);
                batch.triangles += mesh->getVertexCount() / 3;
                sig = Mix(sig, mesh->getSerial());
            }
        }
        batch.count = pool.size() - batch.first;
        if (batch.count == 0) continue;
        sig = Mix(sig, BitsOf(model[3].x));
        sig = Mix(sig, BitsOf(model[3].y));
        sig = Mix(sig, BitsOf(model[3].z));
        sig = Mix(sig, BitsOf(model[0].x));
        sig = Mix(sig, BitsOf(model[2].z));
        batch.signature = sig;
        if (signature) *signature = Mix(*signature, sig);
        batches.push_back(batch);
    }
    if (!shadow && (epoch & 63u) == 0) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (it->second.lastUse + kEntryRetireFrames < epoch) {
                Entry& dead = it->second;
                if (dead.splatTexture) glDeleteTextures(1, &dead.splatTexture);
                if (dead.albedoArray) glDeleteTextures(1, &dead.albedoArray);
                if (dead.normalArray) glDeleteTextures(1, &dead.normalArray);
                if (dead.heightTexture) glDeleteTextures(1, &dead.heightTexture);
                if (dead.densityTexture) glDeleteTextures(1, &dead.densityTexture);
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    }
    return batches.size();
}

void Renderer::TerrainState::prepareSurfaces(Renderer& renderer, const std::vector<SceneObject>& objects,
                                             const std::vector<Batch>& batches) {
    (void)renderer;
    for (const Batch& batch : batches) {
        const SceneObject& object = objects[static_cast<size_t>(batch.objectIndex)];
        const TerrainComponent& t = object.terrain;
        auto found = entries.find(batch.entryId);
        if (found == entries.end()) continue;
        Entry& e = found->second;

        // Splat map.
        const int res = t.splatResolution;
        const bool recreate = e.splatTexture == 0 || e.splatTextureResolution != res || e.splatBuffer != t.splat.get() ||
                              e.splatUploadedRevision == 0;
        if (e.splatTexture == 0) glGenTextures(1, &e.splatTexture);
        if (recreate || e.splatUploadedRevision != t.splatRevision) {
            glBindTexture(GL_TEXTURE_2D, e.splatTexture);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            const bool partial = !recreate && e.splatBuffer == t.splat.get() && !t.splatDirty.empty() &&
                                 e.splatUploadedRevision >= t.splatDirtyBase && e.splatUploadedRevision <= t.splatRevision &&
                                 e.splatTextureResolution == res;
            if (partial) {
                const int x0 = std::clamp(t.splatDirty.minX, 0, res - 1);
                const int x1 = std::clamp(t.splatDirty.maxX, 0, res - 1);
                const int z0 = std::clamp(t.splatDirty.minZ, 0, res - 1);
                const int z1 = std::clamp(t.splatDirty.maxZ, 0, res - 1);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, res);
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, x0);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, z0);
                glTexSubImage2D(GL_TEXTURE_2D, 0, x0, z0, x1 - x0 + 1, z1 - z0 + 1, GL_RGBA, GL_UNSIGNED_BYTE, t.splat->data());
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, res, res, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.splat->data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
            glBindTexture(GL_TEXTURE_2D, 0);
            e.splatTextureResolution = res;
            e.splatBuffer = t.splat.get();
            e.splatUploadedRevision = t.splatRevision;
        }

        // Layer arrays.
        const uint64_t signature = LayerSignature(t);
        if (e.albedoArray == 0 || e.layerSignature != signature) {
            ProfilerScope scope("Terrain::BuildLayerArrays", ProfilerSampleCategory::Asset);
            LayerImage layers[4];
            bool anyFile = false;
            for (int i = 0; i < 4; ++i) BuildLayer(i, t.layers[i], layers[i], anyFile);
            const std::vector<uint8_t>* albedo[4] = {&layers[0].albedo, &layers[1].albedo, &layers[2].albedo, &layers[3].albedo};
            const std::vector<uint8_t>* normal[4] = {&layers[0].normal, &layers[1].normal, &layers[2].normal, &layers[3].normal};
            if (e.albedoArray) glDeleteTextures(1, &e.albedoArray);
            if (e.normalArray) glDeleteTextures(1, &e.normalArray);
            e.albedoArray = UploadArray(albedo);
            e.normalArray = UploadArray(normal);
            e.layerSignature = signature;
        }
    }
}

size_t Renderer::TerrainState::appendItems(Renderer& renderer, const std::vector<SceneObject>& objects,
                                           const glm::mat4& view, const glm::vec3& cameraPosition,
                                           std::vector<SceneRenderItem>& items) {
    (void)renderer;
    size_t appended = 0;
    const size_t oldSize = items.size();
    static const std::string kVertPath = "Resources/Shaders/vert.glsl";
    static const std::string kFragPath = "Resources/Shaders/frag.glsl?MODU_TERRAIN";
    static const std::string kEmptyPath;
    for (size_t batchIndex = 0; batchIndex < viewBatches.size(); ++batchIndex) {
        const Batch& batch = viewBatches[batchIndex];
        const SceneObject& obj = objects[static_cast<size_t>(batch.objectIndex)];
        items.emplace_back();
        SceneRenderItem& item = items.back();
        item.obj = &obj;
        item.mesh = viewPool[batch.first];
        item.model = batch.model;
        item.sortCenter = glm::vec3(batch.model[3]);
        item.depthSortCenter = item.sortCenter;
        item.vertPath = &kVertPath;
        item.fragPath = &kFragPath;
        item.materialPath = &kEmptyPath;
        item.albedoTexturePath = &kEmptyPath;
        item.overlayTexturePath = &kEmptyPath;
        item.normalMapPath = &kEmptyPath;
        item.metallicRoughnessMapPath = &kEmptyPath;
        item.metallicMapPath = &kEmptyPath;
        item.roughnessMapPath = &kEmptyPath;
        item.opacityMapPath = &kEmptyPath;
        item.emissiveMapPath = &kEmptyPath;
        item.occlusionMapPath = &kEmptyPath;
        item.heightMapPath = &kEmptyPath;
        item.material = MaterialProperties{};
        item.material.pbrEnabled = true;
        item.material.metallic = 0.0f;
        item.material.roughness = 0.9f;
        item.material.alpha = 1.0f;
        item.material.alphaCutoff = 0.0f;
        item.material.color = glm::vec3(1.0f);
        item.doubleSided = false;
        item.requiresBlending = false;
        item.sortOpaque = true;
        item.reflectionResolved = true;
        item.terrainBatch = static_cast<int>(batchIndex);
        const glm::vec3 viewSpaceCenter = glm::vec3(view * glm::vec4(item.depthSortCenter, 1.0f));
        item.cameraDepth = -viewSpaceCenter.z;
        const glm::vec3 toCamera = item.depthSortCenter - cameraPosition;
        item.cameraDistanceSq = glm::dot(toCamera, toCamera);
        ++appended;
    }
    if (appended > 0) {
        // Opaque items come first in both queue builders; keep the leaders with them so the lazy
        // skybox still draws before the first transparent item.
        const auto firstTransparent = std::find_if(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(oldSize),
            [](const SceneRenderItem& it) { return !it.sortOpaque; });
        std::rotate(firstTransparent, items.begin() + static_cast<std::ptrdiff_t>(oldSize), items.end());
    }
    return appended;
}

Renderer::TerrainState::Surface Renderer::TerrainState::surface(const SceneObject& object) const {
    Surface s;
    const TerrainComponent& t = object.terrain;
    auto found = entries.find(object.id);
    if (found != entries.end()) {
        s.splat = found->second.splatTexture;
        s.albedoArray = found->second.albedoArray;
        s.normalArray = found->second.normalArray;
    }
    for (int i = 0; i < 4; ++i) {
        s.tiling[i] = std::max(0.05f, t.layers[i].tiling);
        s.normalStrength[i] = t.layers[i].normalStrength;
        s.tint[i] = t.layers[i].tint;
    }
    s.size = t.size;
    if (t.grassEnabled && t.grassDensityScale > 0.001f) s.grassShade = glm::vec2(t.grassDistance, 0.85f);
    return s;
}

void Renderer::TerrainState::release() {
    for (auto& kv : entries) {
        Entry& e = kv.second;
        if (e.splatTexture) glDeleteTextures(1, &e.splatTexture);
        if (e.albedoArray) glDeleteTextures(1, &e.albedoArray);
        if (e.normalArray) glDeleteTextures(1, &e.normalArray);
        if (e.heightTexture) glDeleteTextures(1, &e.heightTexture);
        if (e.densityTexture) glDeleteTextures(1, &e.densityTexture);
        e.splatTexture = e.albedoArray = e.normalArray = 0;
        e.heightTexture = e.densityTexture = 0;
    }
    if (vegetationVao) glDeleteVertexArrays(1, &vegetationVao);
    vegetationVao = 0;
    entries.clear();
    viewPool.clear();
    viewBatches.clear();
    shadowPool.clear();
    shadowBatches.clear();
}
