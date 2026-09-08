// nebula_modularity.h — C ABI exposed by the `nebula-modularity` Rust crate.
//
// This header is hand-maintained and must stay in lockstep with
// NebulaBindings/src/lib.rs. Bump NB_ABI_VERSION on both sides together when
// any signature or struct layout below changes.
//
// The Modularity editor never links this library; it resolves the symbols at
// runtime (see src/Nebula/NebulaLightmap.cpp), so an engine build without a
// Rust toolchain still compiles and runs with baking reported as unavailable.

#ifndef NEBULA_MODULARITY_H
#define NEBULA_MODULARITY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_ABI_VERSION 1u

typedef struct NbScene NbScene;
typedef struct NbBakeJob NbBakeJob;

// ── Light kinds (mirrors Modularity's LightType) ─────────────────────────────
#define NB_LIGHT_DIRECTIONAL 0
#define NB_LIGHT_POINT       1
#define NB_LIGHT_SPOT        2
#define NB_LIGHT_AREA        3

// ── Bake status ──────────────────────────────────────────────────────────────
#define NB_STATUS_RUNNING   0
#define NB_STATUS_DONE      1
#define NB_STATUS_FAILED    2
#define NB_STATUS_CANCELLED 3

typedef struct NbMaterialDesc {
    float   albedo[4];
    float   roughness;
    float   metallic;
    float   emissive[3];
    int32_t casts_shadows;
} NbMaterialDesc;

typedef struct NbMeshDesc {
    const float*    positions;        // vertex_count * 3, required
    const float*    normals;          // vertex_count * 3, or NULL to derive
    const float*    uvs;              // vertex_count * 2, or NULL for zeroes
    uint32_t        vertex_count;
    const uint32_t* indices;          // triangle list, required
    uint32_t        index_count;
    const float*    world_transform;  // 16 floats column-major, or NULL
    uint32_t        material_id;
} NbMeshDesc;

typedef struct NbLightDesc {
    int32_t kind;
    float   position[3];
    float   direction[3];
    float   color[3];
    float   intensity;
    float   range;
    float   inner_angle;      // radians
    float   outer_angle;      // radians
    float   half_extents[2];  // area lights
    float   right[3];         // area lights
    float   up[3];            // area lights
    int32_t casts_shadows;
} NbLightDesc;

typedef struct NbLightmapConfig {
    uint32_t resolution;
    uint32_t samples_per_texel;
    uint32_t bounce_count;
    float    max_ray_distance;
    int32_t  denoise;
    int32_t  hdr_output;
    uint32_t area_light_samples;
} NbLightmapConfig;

typedef struct NbProgress {
    int32_t  status;
    float    fraction;     // 0..1, negative when indeterminate
    uint32_t step;
    uint32_t total_steps;
    char     message[256];
} NbProgress;

// ── Entry points ─────────────────────────────────────────────────────────────

uint32_t     nb_abi_version(void);
const char*  nb_last_error(void);

NbScene* nb_scene_create(void);
void     nb_scene_destroy(NbScene* scene);
uint32_t nb_scene_add_material(NbScene* scene, const NbMaterialDesc* desc);
int32_t  nb_scene_add_mesh(NbScene* scene, const NbMeshDesc* desc);
int32_t  nb_scene_add_light(NbScene* scene, const NbLightDesc* desc);
void     nb_scene_set_sky(NbScene* scene, const float* color_rgb, float intensity);
uint32_t nb_scene_triangle_count(const NbScene* scene);

// Consumes `scene` (valid or not on return). NULL on failure.
NbBakeJob* nb_bake_lightmap_start(NbScene* scene, const NbLightmapConfig* config);
int32_t    nb_bake_poll(const NbBakeJob* job, NbProgress* out);
void       nb_bake_cancel(NbBakeJob* job);
void       nb_bake_destroy(NbBakeJob* job);
int32_t    nb_bake_result_size(const NbBakeJob* job, uint32_t* width, uint32_t* height);
int32_t    nb_bake_write_result(const NbBakeJob* job, const char* path);
int32_t    nb_bake_copy_preview_rgba8(const NbBakeJob* job, void* dst,
                                      uint32_t width, uint32_t height, float exposure);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // NEBULA_MODULARITY_H
