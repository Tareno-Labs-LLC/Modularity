#include "NebulaLightmap.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "../../NebulaBindings/include/nebula_modularity.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace Nebula {

namespace {

struct NebulaApi {
    uint32_t (*abi_version)() = nullptr;
    const char* (*last_error)() = nullptr;

    NbScene* (*scene_create)() = nullptr;
    void (*scene_destroy)(NbScene*) = nullptr;
    uint32_t (*scene_add_material)(NbScene*, const NbMaterialDesc*) = nullptr;
    int32_t (*scene_add_mesh)(NbScene*, const NbMeshDesc*) = nullptr;
    int32_t (*scene_add_light)(NbScene*, const NbLightDesc*) = nullptr;
    void (*scene_set_sky)(NbScene*, const float*, float) = nullptr;

    NbBakeJob* (*bake_start)(NbScene*, const NbLightmapConfig*) = nullptr;
    int32_t (*bake_poll)(const NbBakeJob*, NbProgress*) = nullptr;
    void (*bake_cancel)(NbBakeJob*) = nullptr;
    void (*bake_destroy)(NbBakeJob*) = nullptr;
    int32_t (*bake_result_size)(const NbBakeJob*, uint32_t*, uint32_t*) = nullptr;
    int32_t (*bake_write_result)(const NbBakeJob*, const char*) = nullptr;
    int32_t (*bake_copy_preview)(const NbBakeJob*, void*, uint32_t, uint32_t, float) = nullptr;
};

NebulaApi g_api;

#if defined(_WIN32)
constexpr const char* kLibraryName = "nebula_modularity.dll";
void* openLibrary(const std::string& path) {
    return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
}
void* resolve(void* handle, const char* symbol) {
    return reinterpret_cast<void*>(
        GetProcAddress(reinterpret_cast<HMODULE>(handle), symbol));
}
void closeLibrary(void* handle) {
    if (handle) FreeLibrary(reinterpret_cast<HMODULE>(handle));
}
std::string libraryError() { return "LoadLibrary failed"; }
#else
constexpr const char* kLibraryName = "libnebula_modularity.so";
void* openLibrary(const std::string& path) {
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}
void* resolve(void* handle, const char* symbol) { return dlsym(handle, symbol); }
void closeLibrary(void* handle) {
    (void)handle;
}
std::string libraryError() {
    const char* err = dlerror();
    return err ? err : "dlopen failed";
}
#endif

std::vector<fs::path> candidatePaths() {
    std::vector<fs::path> out;

    if (const char* envOverride = std::getenv("MODULARITY_NEBULA_LIB")) {
        if (*envOverride) out.emplace_back(envOverride);
    }

    out.emplace_back(fs::path(".") / kLibraryName);
    out.emplace_back(fs::path("lib") / kLibraryName);
    out.emplace_back(fs::path("..") / "lib" / kLibraryName);
    out.emplace_back(fs::path("NebulaBindings") / "target" / "release" / kLibraryName);
    out.emplace_back(fs::path("..") / "NebulaBindings" / "target" / "release" / kLibraryName);
    out.emplace_back(fs::path("NebulaBindings") / "target" / "debug" / kLibraryName);
    out.emplace_back(fs::path("..") / "NebulaBindings" / "target" / "debug" / kLibraryName);
    return out;
}

uint32_t nextPowerOfTwo(uint32_t v) {
    if (v <= 1) return 1;
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

uint32_t samplesForQuality(LightmapQuality quality, int finalGatherRays) {
    const float rays = static_cast<float>(std::max(1, finalGatherRays));
    switch (quality) {
        case LightmapQuality::Low:
            return static_cast<uint32_t>(std::max(1.0f, rays * 0.03f));
        case LightmapQuality::Medium:
            return static_cast<uint32_t>(std::max(4.0f, rays * 0.10f));
        case LightmapQuality::High:
        default:
            return static_cast<uint32_t>(std::max(16.0f, rays * 0.25f));
    }
}

}  // namespace

size_t BakeSceneData::triangleCount() const {
    size_t total = 0;
    for (const BakeMeshData& mesh : meshes) {
        total += mesh.indices.size() / 3;
    }
    return total;
}

const char* toString(LightmapMode mode) {
    switch (mode) {
        case LightmapMode::SingleLightmaps: return "Single Lightmaps";
        case LightmapMode::DualLightmaps: return "Dual Lightmaps";
        case LightmapMode::Directional: return "Directional";
    }
    return "Dual Lightmaps";
}

const char* toString(LightmapQuality quality) {
    switch (quality) {
        case LightmapQuality::Low: return "Low";
        case LightmapQuality::Medium: return "Medium";
        case LightmapQuality::High: return "High";
    }
    return "High";
}

LightmapBaker& LightmapBaker::instance() {
    static LightmapBaker baker;
    return baker;
}

LightmapBaker::~LightmapBaker() {
    if (job_ && g_api.bake_destroy) {
        g_api.bake_destroy(static_cast<NbBakeJob*>(job_));
        job_ = nullptr;
    }
    closeLibrary(handle_);
}

bool LightmapBaker::ensureLoaded() {
    if (loadAttempted_) {
        return handle_ != nullptr;
    }
    loadAttempted_ = true;

    std::string tried;
    for (const fs::path& candidate : candidatePaths()) {
        std::error_code ec;
        if (!fs::exists(candidate, ec)) {
            continue;
        }
        void* handle = openLibrary(candidate.string());
        if (!handle) {
            if (!tried.empty()) tried += "; ";
            tried += candidate.string() + " (" + libraryError() + ")";
            continue;
        }
        handle_ = handle;
        libraryPath_ = candidate.string();
        break;
    }

    if (!handle_) {
        loadError_ = "Nebula bindings library (" + std::string(kLibraryName) +
                     ") was not found. Build it with: cargo build --release "
                     "--manifest-path NebulaBindings/Cargo.toml";
        if (!tried.empty()) {
            loadError_ += "  Failed candidates: " + tried;
        }
        return false;
    }

    struct Binding {
        const char* name;
        void** slot;
    };
    const Binding bindings[] = {
        {"nb_abi_version", reinterpret_cast<void**>(&g_api.abi_version)},
        {"nb_last_error", reinterpret_cast<void**>(&g_api.last_error)},
        {"nb_scene_create", reinterpret_cast<void**>(&g_api.scene_create)},
        {"nb_scene_destroy", reinterpret_cast<void**>(&g_api.scene_destroy)},
        {"nb_scene_add_material", reinterpret_cast<void**>(&g_api.scene_add_material)},
        {"nb_scene_add_mesh", reinterpret_cast<void**>(&g_api.scene_add_mesh)},
        {"nb_scene_add_light", reinterpret_cast<void**>(&g_api.scene_add_light)},
        {"nb_scene_set_sky", reinterpret_cast<void**>(&g_api.scene_set_sky)},
        {"nb_bake_lightmap_start", reinterpret_cast<void**>(&g_api.bake_start)},
        {"nb_bake_poll", reinterpret_cast<void**>(&g_api.bake_poll)},
        {"nb_bake_cancel", reinterpret_cast<void**>(&g_api.bake_cancel)},
        {"nb_bake_destroy", reinterpret_cast<void**>(&g_api.bake_destroy)},
        {"nb_bake_result_size", reinterpret_cast<void**>(&g_api.bake_result_size)},
        {"nb_bake_write_result", reinterpret_cast<void**>(&g_api.bake_write_result)},
        {"nb_bake_copy_preview_rgba8", reinterpret_cast<void**>(&g_api.bake_copy_preview)},
    };

    for (const Binding& binding : bindings) {
        void* symbol = resolve(handle_, binding.name);
        if (!symbol) {
            loadError_ = std::string("Nebula bindings library is missing symbol '") +
                         binding.name + "'. Rebuild NebulaBindings.";
            g_api = NebulaApi{};
            handle_ = nullptr;
            return false;
        }
        *binding.slot = symbol;
    }

    const uint32_t abi = g_api.abi_version();
    if (abi != NB_ABI_VERSION) {
        loadError_ = "Nebula bindings ABI mismatch: library reports " +
                     std::to_string(abi) + ", editor expects " +
                     std::to_string(NB_ABI_VERSION) + ". Rebuild NebulaBindings.";
        g_api = NebulaApi{};
        handle_ = nullptr;
        return false;
    }

    return true;
}

bool LightmapBaker::available(std::string* reason) {
    const bool ok = ensureLoaded();
    if (!ok && reason) {
        *reason = loadError_;
    }
    return ok;
}

uint32_t LightmapBaker::resolveAtlasSize(const LightmapSettings& settings,
                                         float worldSurfaceArea) {
    if (settings.lockAtlas) {
        return std::clamp<uint32_t>(nextPowerOfTwo(
                                        static_cast<uint32_t>(std::max(64, settings.lockedAtlasSize))),
                                    64u, 8192u);
    }

    const float area = std::max(0.0f, worldSurfaceArea);
    const float side = std::sqrt(area) * std::max(0.1f, settings.resolution);
    if (!(side > 0.0f)) {
        return 64u;
    }
    return std::clamp(nextPowerOfTwo(static_cast<uint32_t>(std::ceil(side))), 64u, 8192u);
}

bool LightmapBaker::start(const BakeSceneData& scene,
                          const LightmapSettings& settings,
                          const std::string& outputPath,
                          std::string& error) {
    if (isRunning()) {
        error = "A lightmap bake is already running.";
        return false;
    }
    if (!ensureLoaded()) {
        error = loadError_;
        return false;
    }
    if (scene.empty()) {
        error = "Nothing to bake: no static geometry is marked for lightmapping.";
        return false;
    }

    reset();

    NbScene* nbScene = g_api.scene_create();
    if (!nbScene) {
        error = "Nebula could not allocate a bake scene.";
        return false;
    }

    for (const BakeMaterialData& material : scene.materials) {
        NbMaterialDesc desc{};
        desc.albedo[0] = material.albedo.x;
        desc.albedo[1] = material.albedo.y;
        desc.albedo[2] = material.albedo.z;
        desc.albedo[3] = material.albedo.w;
        desc.roughness = material.roughness;
        desc.metallic = material.metallic;
        desc.emissive[0] = material.emissive.x;
        desc.emissive[1] = material.emissive.y;
        desc.emissive[2] = material.emissive.z;
        desc.casts_shadows = material.castsShadows ? 1 : 0;
        g_api.scene_add_material(nbScene, &desc);
    }

    for (const BakeMeshData& mesh : scene.meshes) {
        if (mesh.positions.size() < 9 || mesh.indices.size() < 3) {
            continue;
        }
        NbMeshDesc desc{};
        desc.positions = mesh.positions.data();
        desc.normals = mesh.normals.empty() ? nullptr : mesh.normals.data();
        desc.uvs = mesh.uvs.empty() ? nullptr : mesh.uvs.data();
        desc.vertex_count = static_cast<uint32_t>(mesh.positions.size() / 3);
        desc.indices = mesh.indices.data();
        desc.index_count = static_cast<uint32_t>(mesh.indices.size());
        desc.world_transform = &mesh.worldTransform[0][0];
        desc.material_id = mesh.materialId;

        if (g_api.scene_add_mesh(nbScene, &desc) < 0) {
            const char* nbError = g_api.last_error ? g_api.last_error() : nullptr;
            error = std::string("Failed to add mesh '") + mesh.name +
                    "' to the bake scene" + (nbError ? std::string(": ") + nbError : "");
            g_api.scene_destroy(nbScene);
            return false;
        }
    }

    for (const BakeLightData& light : scene.lights) {
        NbLightDesc desc{};
        desc.kind = light.kind;
        desc.position[0] = light.position.x;
        desc.position[1] = light.position.y;
        desc.position[2] = light.position.z;
        desc.direction[0] = light.direction.x;
        desc.direction[1] = light.direction.y;
        desc.direction[2] = light.direction.z;
        desc.color[0] = light.color.x;
        desc.color[1] = light.color.y;
        desc.color[2] = light.color.z;
        desc.intensity = light.intensity;
        desc.range = light.range;
        desc.inner_angle = light.innerAngleRadians;
        desc.outer_angle = light.outerAngleRadians;
        desc.half_extents[0] = light.halfExtents.x;
        desc.half_extents[1] = light.halfExtents.y;
        desc.right[0] = light.right.x;
        desc.right[1] = light.right.y;
        desc.right[2] = light.right.z;
        desc.up[0] = light.up.x;
        desc.up[1] = light.up.y;
        desc.up[2] = light.up.z;
        desc.casts_shadows = light.castsShadows ? 1 : 0;
        g_api.scene_add_light(nbScene, &desc);
    }

    if (settings.skyLightIntensity > 0.0f) {
        const float sky[3] = {settings.skyLightColor.x, settings.skyLightColor.y,
                              settings.skyLightColor.z};
        g_api.scene_set_sky(nbScene, sky, settings.skyLightIntensity);
    }

    NbLightmapConfig config{};
    config.resolution = resolveAtlasSize(settings, scene.surfaceArea);
    config.samples_per_texel = samplesForQuality(settings.quality, settings.finalGatherRays);
    config.bounce_count = static_cast<uint32_t>(std::max(0, settings.bounces));
    config.max_ray_distance =
        std::max(10.0f, glm::length(scene.boundsMax - scene.boundsMin) * 2.0f);
    config.denoise = (settings.interpolation > 0.0f ||
                      settings.quality != LightmapQuality::Low)
                         ? 1
                         : 0;
    config.hdr_output = 1;
    config.area_light_samples =
        static_cast<uint32_t>(std::max(1, settings.interpolationPoints));

    NbBakeJob* job = g_api.bake_start(nbScene, &config);
    if (!job) {
        const char* nbError = g_api.last_error ? g_api.last_error() : nullptr;
        error = nbError ? nbError : "Nebula could not start the bake.";
        return false;
    }

    job_ = job;
    outputPath_ = outputPath;
    progress_ = BakeProgress{};
    progress_.status = BakeStatus::Running;
    progress_.fraction = -1.0f;
    progress_.message = "Preparing bake";
    return true;
}

void LightmapBaker::update() {
    if (!job_ || !g_api.bake_poll) {
        return;
    }

    NbProgress raw{};
    const int32_t status = g_api.bake_poll(static_cast<const NbBakeJob*>(job_), &raw);

    progress_.fraction = raw.fraction;
    progress_.step = raw.step;
    progress_.totalSteps = raw.total_steps;
    raw.message[sizeof(raw.message) - 1] = '\0';
    progress_.message = raw.message;

    switch (status) {
        case NB_STATUS_RUNNING:
            progress_.status = BakeStatus::Running;
            break;
        case NB_STATUS_DONE:
            progress_.status = BakeStatus::Done;
            if (g_api.bake_result_size) {
                uint32_t width = 0;
                uint32_t height = 0;
                if (g_api.bake_result_size(static_cast<const NbBakeJob*>(job_), &width,
                                           &height)) {
                    resultWidth_ = width;
                    resultHeight_ = height;
                }
            }
            break;
        case NB_STATUS_CANCELLED:
            progress_.status = BakeStatus::Cancelled;
            break;
        case NB_STATUS_FAILED:
        default:
            progress_.status = BakeStatus::Failed;
            break;
    }
}

void LightmapBaker::cancel() {
    if (job_ && g_api.bake_cancel) {
        g_api.bake_cancel(static_cast<NbBakeJob*>(job_));
        progress_.message = "Cancelling (waiting for the current GPU pass)";
    }
}

void LightmapBaker::reset() {
    if (job_ && g_api.bake_destroy) {
        if (g_api.bake_cancel) {
            g_api.bake_cancel(static_cast<NbBakeJob*>(job_));
        }
        g_api.bake_destroy(static_cast<NbBakeJob*>(job_));
    }
    job_ = nullptr;
    progress_ = BakeProgress{};
    resultWidth_ = 0;
    resultHeight_ = 0;
}

bool LightmapBaker::copyPreviewRGBA8(std::vector<unsigned char>& dst, int width,
                                     int height, float exposure) {
    if (!job_ || !g_api.bake_copy_preview || width <= 0 || height <= 0) {
        return false;
    }
    dst.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
    return g_api.bake_copy_preview(static_cast<const NbBakeJob*>(job_), dst.data(),
                                   static_cast<uint32_t>(width),
                                   static_cast<uint32_t>(height), exposure) != 0;
}

bool LightmapBaker::writeResult(std::string& error) {
    if (!job_ || !g_api.bake_write_result) {
        error = "There is no completed bake to write.";
        return false;
    }
    if (outputPath_.empty()) {
        error = "No output path was set for this bake.";
        return false;
    }
    if (g_api.bake_write_result(static_cast<const NbBakeJob*>(job_),
                                outputPath_.c_str())) {
        return true;
    }
    const char* nbError = g_api.last_error ? g_api.last_error() : nullptr;
    error = nbError ? nbError : "Nebula could not write the lightmap file.";
    return false;
}

}  // namespace Nebula
