//! # nebula-modularity
//!
//! A thin C ABI over [Nebula](../../Nebula) so the Modularity editor (C++14,
//! OpenGL) can drive lightmap bakes without linking Rust into the engine.
//!
//! The engine loads the resulting shared library at runtime (`dlopen` /
//! `LoadLibrary`) and resolves the `nb_*` entry points. When the library is
//! absent the editor simply reports that baking is unavailable, so a Modularity
//! build never hard-depends on a Rust toolchain.
//!
//! ## Lifecycle
//!
//! ```text
//! nb_scene_create
//!   nb_scene_add_material / nb_scene_add_mesh / nb_scene_add_light / nb_scene_set_sky
//! nb_bake_lightmap_start   <- consumes the scene, spawns a worker thread
//!   nb_bake_poll           <- called once per editor frame, never blocks
//! nb_bake_write_result / nb_bake_copy_preview_rgba8
//! nb_bake_destroy
//! ```
//!
//! Every pointer handed across the boundary is an opaque `Box`-allocated
//! pointer owned by Rust. The C++ side must pair each `*_create` / `*_start`
//! with the matching `*_destroy`.
//!
//! Nebula itself is untouched by this crate: it is consumed purely as a path
//! dependency.

use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use nebula_core::{
    context::BakeContext,
    progress::ProgressReporter,
    scene::{BakeMesh, LightSource, LightSourceKind, MaterialDesc, SceneGeometry, Transform},
    traits::BakePass,
};
use nebula_light::{LightmapBaker, LightmapConfig, LightmapOutput};
use nebula_serialize::{
    chunk::{write_chunk, write_end_chunk, write_file_header},
    Compression,
};

// ─────────────────────────────────────────────────────────────────────────────
// ABI version
// ─────────────────────────────────────────────────────────────────────────────

/// Bumped whenever any struct layout or function signature below changes.
/// The engine refuses to use a library whose version it does not recognise.
pub const NB_ABI_VERSION: u32 = 1;

#[no_mangle]
pub extern "C" fn nb_abi_version() -> u32 {
    NB_ABI_VERSION
}

// ─────────────────────────────────────────────────────────────────────────────
// Error reporting
// ─────────────────────────────────────────────────────────────────────────────

thread_local! {
    static LAST_ERROR: std::cell::RefCell<Option<CString>> =
        const { std::cell::RefCell::new(None) };
}

fn set_last_error(msg: impl Into<String>) {
    let msg = msg.into();
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = CString::new(msg).ok();
    });
}

/// Returns the last error raised on the *calling* thread, or NULL.
/// The pointer stays valid until the next failing call on that thread.
#[no_mangle]
pub extern "C" fn nb_last_error() -> *const c_char {
    LAST_ERROR.with(|slot| {
        slot.borrow()
            .as_ref()
            .map(|s| s.as_ptr())
            .unwrap_or(std::ptr::null())
    })
}

// ─────────────────────────────────────────────────────────────────────────────
// Plain-old-data descriptors shared with the C++ side
// ─────────────────────────────────────────────────────────────────────────────

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NbMaterialDesc {
    pub albedo: [f32; 4],
    pub roughness: f32,
    pub metallic: f32,
    pub emissive: [f32; 3],
    /// Non-zero = surface blocks light.
    pub casts_shadows: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NbMeshDesc {
    /// `vertex_count * 3` floats. Required.
    pub positions: *const f32,
    /// `vertex_count * 3` floats, or NULL to derive area-weighted normals.
    pub normals: *const f32,
    /// `vertex_count * 2` floats, or NULL for zeroed UVs.
    pub uvs: *const f32,
    pub vertex_count: u32,
    /// `index_count` triangle-list indices. Required.
    pub indices: *const u32,
    pub index_count: u32,
    /// 16 floats, column-major, or NULL for identity.
    pub world_transform: *const f32,
    /// Index returned by `nb_scene_add_material`.
    pub material_id: u32,
}

/// Matches Modularity's `LightType` ordering.
pub const NB_LIGHT_DIRECTIONAL: i32 = 0;
pub const NB_LIGHT_POINT: i32 = 1;
pub const NB_LIGHT_SPOT: i32 = 2;
pub const NB_LIGHT_AREA: i32 = 3;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NbLightDesc {
    pub kind: i32,
    pub position: [f32; 3],
    pub direction: [f32; 3],
    pub color: [f32; 3],
    pub intensity: f32,
    pub range: f32,
    /// Radians.
    pub inner_angle: f32,
    /// Radians.
    pub outer_angle: f32,
    /// Area lights: half width / half height in world units.
    pub half_extents: [f32; 2],
    /// Area lights: the local right and up axes of the rectangle.
    pub right: [f32; 3],
    pub up: [f32; 3],
    pub casts_shadows: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NbLightmapConfig {
    pub resolution: u32,
    pub samples_per_texel: u32,
    pub bounce_count: u32,
    pub max_ray_distance: f32,
    pub denoise: i32,
    pub hdr_output: i32,
    pub area_light_samples: u32,
}

pub const NB_STATUS_RUNNING: i32 = 0;
pub const NB_STATUS_DONE: i32 = 1;
pub const NB_STATUS_FAILED: i32 = 2;
pub const NB_STATUS_CANCELLED: i32 = 3;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NbProgress {
    pub status: i32,
    /// 0..1, or negative when the current phase has no meaningful fraction.
    pub fraction: f32,
    pub step: u32,
    pub total_steps: u32,
    /// NUL-terminated UTF-8, truncated to fit.
    pub message: [c_char; 256],
}

impl Default for NbProgress {
    fn default() -> Self {
        Self {
            status: NB_STATUS_RUNNING,
            fraction: -1.0,
            step: 0,
            total_steps: 0,
            message: [0; 256],
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Scene building
// ─────────────────────────────────────────────────────────────────────────────

pub struct NbScene {
    geometry: SceneGeometry,
}

#[no_mangle]
pub extern "C" fn nb_scene_create() -> *mut NbScene {
    Box::into_raw(Box::new(NbScene {
        geometry: SceneGeometry::new(),
    }))
}

/// # Safety
/// `scene` must come from `nb_scene_create` and not have been consumed by
/// `nb_bake_lightmap_start`.
#[no_mangle]
pub unsafe extern "C" fn nb_scene_destroy(scene: *mut NbScene) {
    if !scene.is_null() {
        drop(Box::from_raw(scene));
    }
}

/// Appends a material and returns its index, or `u32::MAX` on error.
///
/// # Safety
/// Both pointers must be valid for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn nb_scene_add_material(
    scene: *mut NbScene,
    desc: *const NbMaterialDesc,
) -> u32 {
    let (Some(scene), Some(desc)) = (scene.as_mut(), desc.as_ref()) else {
        set_last_error("nb_scene_add_material: null argument");
        return u32::MAX;
    };

    scene.geometry.materials.push(MaterialDesc {
        albedo: desc.albedo,
        roughness: desc.roughness.clamp(0.0, 1.0),
        metallic: desc.metallic.clamp(0.0, 1.0),
        emissive: desc.emissive,
        casts_shadows: desc.casts_shadows != 0,
        ..MaterialDesc::default()
    });
    (scene.geometry.materials.len() - 1) as u32
}

/// Appends a mesh. Returns the mesh index, or -1 on error.
///
/// Vertex data is copied, so the caller may free its buffers immediately.
///
/// # Safety
/// The arrays in `desc` must each hold at least the element count implied by
/// `vertex_count` / `index_count`.
#[no_mangle]
pub unsafe extern "C" fn nb_scene_add_mesh(scene: *mut NbScene, desc: *const NbMeshDesc) -> i32 {
    let (Some(scene), Some(desc)) = (scene.as_mut(), desc.as_ref()) else {
        set_last_error("nb_scene_add_mesh: null argument");
        return -1;
    };

    let vcount = desc.vertex_count as usize;
    let icount = desc.index_count as usize;

    if vcount == 0 || icount < 3 || desc.positions.is_null() || desc.indices.is_null() {
        set_last_error("nb_scene_add_mesh: mesh has no triangles");
        return -1;
    }
    if icount % 3 != 0 {
        set_last_error("nb_scene_add_mesh: index_count is not a multiple of 3");
        return -1;
    }

    let positions: Vec<[f32; 3]> = std::slice::from_raw_parts(desc.positions, vcount * 3)
        .chunks_exact(3)
        .map(|c| [c[0], c[1], c[2]])
        .collect();

    let indices: Vec<u32> = std::slice::from_raw_parts(desc.indices, icount).to_vec();
    if let Some(bad) = indices.iter().find(|&&i| i as usize >= vcount) {
        set_last_error(format!(
            "nb_scene_add_mesh: index {bad} out of range for {vcount} vertices"
        ));
        return -1;
    }

    let normals: Vec<[f32; 3]> = if desc.normals.is_null() {
        derive_normals(&positions, &indices)
    } else {
        std::slice::from_raw_parts(desc.normals, vcount * 3)
            .chunks_exact(3)
            .map(|c| [c[0], c[1], c[2]])
            .collect()
    };

    let uvs: Vec<[f32; 2]> = if desc.uvs.is_null() {
        vec![[0.0, 0.0]; vcount]
    } else {
        std::slice::from_raw_parts(desc.uvs, vcount * 2)
            .chunks_exact(2)
            .map(|c| [c[0], c[1]])
            .collect()
    };

    let world_transform = if desc.world_transform.is_null() {
        Transform::IDENTITY
    } else {
        let m = std::slice::from_raw_parts(desc.world_transform, 16);
        Transform(glam::Mat4::from_cols_slice(m))
    };

    let material_ids = vec![desc.material_id; icount / 3];

    scene.geometry.add_mesh(BakeMesh {
        id: uuid::Uuid::new_v4(),
        positions,
        normals,
        uvs,
        // None asks Nebula to generate the lightmap parameterisation itself.
        lightmap_uvs: None,
        indices,
        material_ids,
        world_transform,
    }) as i32
}

/// Appends a light. Returns the light index, or -1 on error.
///
/// # Safety
/// `desc` must be valid for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn nb_scene_add_light(scene: *mut NbScene, desc: *const NbLightDesc) -> i32 {
    let (Some(scene), Some(desc)) = (scene.as_mut(), desc.as_ref()) else {
        set_last_error("nb_scene_add_light: null argument");
        return -1;
    };

    let kind = match desc.kind {
        NB_LIGHT_DIRECTIONAL => LightSourceKind::Directional {
            direction: normalize_or(desc.direction, [0.0, -1.0, 0.0]),
        },
        NB_LIGHT_POINT => LightSourceKind::Point {
            position: desc.position,
            range: desc.range.max(0.001),
        },
        NB_LIGHT_SPOT => LightSourceKind::Spot {
            position: desc.position,
            direction: normalize_or(desc.direction, [0.0, -1.0, 0.0]),
            range: desc.range.max(0.001),
            inner_angle: desc.inner_angle,
            outer_angle: desc.outer_angle,
        },
        NB_LIGHT_AREA => LightSourceKind::Area {
            center: desc.position,
            right: normalize_or(desc.right, [1.0, 0.0, 0.0]),
            up: normalize_or(desc.up, [0.0, 1.0, 0.0]),
            half_w: desc.half_extents[0].max(0.0001),
            half_h: desc.half_extents[1].max(0.0001),
        },
        other => {
            set_last_error(format!("nb_scene_add_light: unknown light kind {other}"));
            return -1;
        }
    };

    scene.geometry.add_light(LightSource {
        kind,
        color: desc.color,
        intensity: desc.intensity,
        bake_enabled: true,
        casts_shadows: desc.casts_shadows != 0,
    }) as i32
}

/// Sets a flat ambient sky by synthesising a 1x1 RGBE panorama.
///
/// Nebula consumes sky lighting as an RGBE HDR panorama; the editor's Bake tab
/// only exposes a colour and an intensity, so the smallest legal panorama that
/// encodes that constant radiance is generated here rather than teaching Nebula
/// about flat skies.
///
/// # Safety
/// `color` must point to 3 floats.
#[no_mangle]
pub unsafe extern "C" fn nb_scene_set_sky(
    scene: *mut NbScene,
    color: *const f32,
    intensity: f32,
) {
    let Some(scene) = scene.as_mut() else {
        set_last_error("nb_scene_set_sky: null scene");
        return;
    };
    if color.is_null() {
        set_last_error("nb_scene_set_sky: null colour");
        return;
    }
    let rgb = std::slice::from_raw_parts(color, 3);
    let scaled = [
        rgb[0] * intensity,
        rgb[1] * intensity,
        rgb[2] * intensity,
    ];

    if intensity <= 0.0 {
        scene.geometry.sky_hdr = None;
        scene.geometry.sky_hdr_width = 0;
        scene.geometry.sky_hdr_height = 0;
        return;
    }

    scene.geometry.sky_hdr = Some(encode_rgbe(scaled).to_vec());
    scene.geometry.sky_hdr_width = 1;
    scene.geometry.sky_hdr_height = 1;
}

/// Number of triangles currently in the scene (used by the editor's Bake tab
/// to show what a bake would cover before it is started).
///
/// # Safety
/// `scene` must be a live scene handle.
#[no_mangle]
pub unsafe extern "C" fn nb_scene_triangle_count(scene: *const NbScene) -> u32 {
    scene
        .as_ref()
        .map(|s| s.geometry.total_triangle_count() as u32)
        .unwrap_or(0)
}

// ─────────────────────────────────────────────────────────────────────────────
// Bake job
// ─────────────────────────────────────────────────────────────────────────────

struct ProgressState {
    status: i32,
    step: u32,
    total_steps: u32,
    message: String,
}

struct JobShared {
    progress: Mutex<ProgressState>,
    cancelled: AtomicBool,
    result: Mutex<Option<LightmapOutput>>,
}

/// Forwards Nebula's reporter callbacks into the shared job state that
/// `nb_bake_poll` reads from the editor thread.
struct SharedReporter(Arc<JobShared>);

impl ProgressReporter for SharedReporter {
    fn begin(&self, pass: &str, total_steps: u32) {
        if let Ok(mut p) = self.0.progress.lock() {
            p.total_steps = total_steps;
            p.step = 0;
            p.message = format!("{pass}: starting");
        }
    }

    fn step(&self, _pass: &str, step: u32, message: &str) {
        if let Ok(mut p) = self.0.progress.lock() {
            p.step = step;
            p.message = message.to_owned();
        }
    }

    fn finish(&self, _pass: &str, success: bool, message: &str) {
        if let Ok(mut p) = self.0.progress.lock() {
            if p.total_steps > 0 {
                p.step = p.total_steps;
            }
            p.message = message.to_owned();
            if !success {
                p.status = NB_STATUS_FAILED;
            }
        }
    }
}

pub struct NbBakeJob {
    shared: Arc<JobShared>,
    handle: Option<std::thread::JoinHandle<()>>,
}

/// Starts a lightmap bake on a worker thread. Consumes `scene` (the pointer is
/// invalid on return, whether or not the call succeeded).
///
/// Returns NULL on error; call `nb_last_error` for the reason.
///
/// # Safety
/// `scene` must be a live handle from `nb_scene_create`, and `config` must be
/// valid for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn nb_bake_lightmap_start(
    scene: *mut NbScene,
    config: *const NbLightmapConfig,
) -> *mut NbBakeJob {
    if scene.is_null() {
        set_last_error("nb_bake_lightmap_start: null scene");
        return std::ptr::null_mut();
    }
    let scene = Box::from_raw(scene);

    let Some(config) = config.as_ref() else {
        set_last_error("nb_bake_lightmap_start: null config");
        return std::ptr::null_mut();
    };

    if scene.geometry.meshes.is_empty() {
        set_last_error("nothing to bake: the scene has no static geometry");
        return std::ptr::null_mut();
    }

    let cfg = LightmapConfig {
        resolution: config.resolution.clamp(64, 8192),
        samples_per_texel: config.samples_per_texel,
        bounce_count: config.bounce_count,
        max_ray_distance: config.max_ray_distance.max(1.0),
        denoise: config.denoise != 0,
        hdr_output: config.hdr_output != 0,
        area_light_samples: config.area_light_samples.max(1),
        debug_normals: false,
    };

    let shared = Arc::new(JobShared {
        progress: Mutex::new(ProgressState {
            status: NB_STATUS_RUNNING,
            step: 0,
            total_steps: 0,
            message: "initialising GPU bake context".to_owned(),
        }),
        cancelled: AtomicBool::new(false),
        result: Mutex::new(None),
    });

    let worker_shared = Arc::clone(&shared);
    let geometry = scene.geometry;

    let handle = std::thread::Builder::new()
        .name("nebula-bake".to_owned())
        .spawn(move || {
            run_bake(worker_shared, geometry, cfg);
        });

    let handle = match handle {
        Ok(h) => h,
        Err(e) => {
            set_last_error(format!("could not spawn bake thread: {e}"));
            return std::ptr::null_mut();
        }
    };

    Box::into_raw(Box::new(NbBakeJob {
        shared,
        handle: Some(handle),
    }))
}

fn run_bake(shared: Arc<JobShared>, geometry: SceneGeometry, cfg: LightmapConfig) {
    let finish_err = |msg: String| {
        if let Ok(mut p) = shared.progress.lock() {
            p.status = NB_STATUS_FAILED;
            p.message = msg;
        }
    };

    let ctx = match pollster::block_on(BakeContext::new()) {
        Ok(ctx) => ctx,
        Err(e) => {
            finish_err(format!("no GPU available for baking: {e}"));
            return;
        }
    };

    if shared.cancelled.load(Ordering::Relaxed) {
        if let Ok(mut p) = shared.progress.lock() {
            p.status = NB_STATUS_CANCELLED;
            p.message = "cancelled".to_owned();
        }
        return;
    }

    let reporter = SharedReporter(Arc::clone(&shared));
    let baker = LightmapBaker;

    match pollster::block_on(baker.execute(&geometry, &cfg, &ctx, &reporter)) {
        Ok(output) => {
            // A cancel that lands after the GPU work completed still discards
            // the result: Nebula's BakePass has no interruption point, so
            // cancellation can only ever mean "throw the result away".
            if shared.cancelled.load(Ordering::Relaxed) {
                if let Ok(mut p) = shared.progress.lock() {
                    p.status = NB_STATUS_CANCELLED;
                    p.message = "cancelled".to_owned();
                }
                return;
            }
            if let Ok(mut slot) = shared.result.lock() {
                *slot = Some(output);
            }
            if let Ok(mut p) = shared.progress.lock() {
                p.status = NB_STATUS_DONE;
            }
        }
        Err(e) => finish_err(format!("bake failed: {e}")),
    }
}

/// Non-blocking progress read. Safe to call every frame.
///
/// # Safety
/// `job` must be live and `out` must point to a writable `NbProgress`.
#[no_mangle]
pub unsafe extern "C" fn nb_bake_poll(job: *const NbBakeJob, out: *mut NbProgress) -> i32 {
    let (Some(job), Some(out)) = (job.as_ref(), out.as_mut()) else {
        set_last_error("nb_bake_poll: null argument");
        return NB_STATUS_FAILED;
    };

    let Ok(p) = job.shared.progress.lock() else {
        set_last_error("nb_bake_poll: progress state poisoned");
        return NB_STATUS_FAILED;
    };

    out.status = p.status;
    out.step = p.step;
    out.total_steps = p.total_steps;
    out.fraction = if p.total_steps > 0 {
        (p.step as f32 / p.total_steps as f32).clamp(0.0, 1.0)
    } else {
        -1.0
    };

    write_c_string(&p.message, &mut out.message);
    p.status
}

/// Requests cancellation. The worker keeps running to completion (Nebula has no
/// mid-pass interruption point) but its result is discarded.
///
/// # Safety
/// `job` must be live.
#[no_mangle]
pub unsafe extern "C" fn nb_bake_cancel(job: *mut NbBakeJob) {
    if let Some(job) = job.as_mut() {
        job.shared.cancelled.store(true, Ordering::Relaxed);
    }
}

/// Joins the worker and frees the job.
///
/// # Safety
/// `job` must come from `nb_bake_lightmap_start` and must not be used after.
#[no_mangle]
pub unsafe extern "C" fn nb_bake_destroy(job: *mut NbBakeJob) {
    if job.is_null() {
        return;
    }
    let mut job = Box::from_raw(job);
    job.shared.cancelled.store(true, Ordering::Relaxed);
    if let Some(handle) = job.handle.take() {
        let _ = handle.join();
    }
}

/// Atlas dimensions of a finished bake. Returns 0 when no result is available.
///
/// # Safety
/// `job` must be live; `width`/`height` must be writable.
#[no_mangle]
pub unsafe extern "C" fn nb_bake_result_size(
    job: *const NbBakeJob,
    width: *mut u32,
    height: *mut u32,
) -> i32 {
    let Some(job) = job.as_ref() else { return 0 };
    let Ok(slot) = job.shared.result.lock() else {
        return 0;
    };
    let Some(output) = slot.as_ref() else { return 0 };

    if let Some(w) = width.as_mut() {
        *w = output.width;
    }
    if let Some(h) = height.as_mut() {
        *h = output.height;
    }
    1
}

/// Writes the finished bake as a chunked `.nebula` file. Returns 1 on success.
///
/// # Safety
/// `job` must be live and `path` a NUL-terminated UTF-8 path.
#[no_mangle]
pub unsafe extern "C" fn nb_bake_write_result(job: *const NbBakeJob, path: *const c_char) -> i32 {
    let (Some(job), false) = (job.as_ref(), path.is_null()) else {
        set_last_error("nb_bake_write_result: null argument");
        return 0;
    };

    let path = match CStr::from_ptr(path).to_str() {
        Ok(p) => p,
        Err(_) => {
            set_last_error("nb_bake_write_result: path is not valid UTF-8");
            return 0;
        }
    };

    let Ok(slot) = job.shared.result.lock() else {
        set_last_error("nb_bake_write_result: result state poisoned");
        return 0;
    };
    let Some(output) = slot.as_ref() else {
        set_last_error("nb_bake_write_result: no completed bake to write");
        return 0;
    };

    // nebula-serialize keeps its bincode chunk helper crate-private, so the
    // LMAP chunk is assembled here using the public chunk primitives. The
    // resulting file layout is identical to what an in-tree serializer emits.
    let encoded = match bincode::serde::encode_to_vec(output, bincode::config::standard()) {
        Ok(v) => v,
        Err(e) => {
            set_last_error(format!("could not encode lightmap: {e}"));
            return 0;
        }
    };

    let write = || -> std::io::Result<()> {
        if let Some(parent) = std::path::Path::new(path).parent() {
            std::fs::create_dir_all(parent)?;
        }
        let file = std::fs::File::create(path)?;
        let mut w = std::io::BufWriter::new(file);
        write_file_header(&mut w).map_err(std::io::Error::other)?;
        write_chunk(
            &mut w,
            nebula_light::CHUNK_TAG,
            &encoded,
            Compression::Balanced,
        )
        .map_err(std::io::Error::other)?;
        write_end_chunk(&mut w).map_err(std::io::Error::other)?;
        std::io::Write::flush(&mut w)?;
        Ok(())
    };

    match write() {
        Ok(()) => 1,
        Err(e) => {
            set_last_error(format!("could not write {path}: {e}"));
            0
        }
    }
}

/// Tone-maps the HDR atlas into a caller-provided RGBA8 buffer of
/// `width * height * 4` bytes, box-filtering when the requested size differs
/// from the atlas. Returns 1 on success.
///
/// This exists so the editor's Maps tab can show a preview without the C++ side
/// having to understand RGBA16F/RGBA32F storage.
///
/// # Safety
/// `dst` must be writable for `width * height * 4` bytes.
#[no_mangle]
pub unsafe extern "C" fn nb_bake_copy_preview_rgba8(
    job: *const NbBakeJob,
    dst: *mut c_void,
    width: u32,
    height: u32,
    exposure: f32,
) -> i32 {
    let (Some(job), false) = (job.as_ref(), dst.is_null()) else {
        set_last_error("nb_bake_copy_preview_rgba8: null argument");
        return 0;
    };
    if width == 0 || height == 0 {
        set_last_error("nb_bake_copy_preview_rgba8: zero-sized destination");
        return 0;
    }

    let Ok(slot) = job.shared.result.lock() else {
        return 0;
    };
    let Some(output) = slot.as_ref() else {
        set_last_error("nb_bake_copy_preview_rgba8: no completed bake");
        return 0;
    };

    let dst = std::slice::from_raw_parts_mut(dst as *mut u8, (width * height * 4) as usize);
    let scale = exposure.max(0.0001);

    for y in 0..height {
        for x in 0..width {
            let sx = (x as u64 * output.width as u64 / width as u64) as u32;
            let sy = (y as u64 * output.height as u64 / height as u64) as u32;
            let texel = read_texel(output, sx, sy).unwrap_or([0.0, 0.0, 0.0, 1.0]);

            let out = ((y * width + x) * 4) as usize;
            for c in 0..3 {
                dst[out + c] = encode_srgb(tonemap(texel[c] * scale));
            }
            dst[out + 3] = 255;
        }
    }
    1
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

fn read_texel(output: &LightmapOutput, x: u32, y: u32) -> Option<[f32; 4]> {
    let channels = output.channels.max(1) as usize;
    let index = (y as usize * output.width as usize + x as usize) * channels;

    if output.is_f32 {
        let floats: &[f32] = bytemuck_cast_f32(&output.texels)?;
        let mut out = [0.0f32, 0.0, 0.0, 1.0];
        for c in 0..channels.min(4) {
            out[c] = *floats.get(index + c)?;
        }
        Some(out)
    } else {
        let halves: &[half::f16] = bytemuck_cast_f16(&output.texels)?;
        let mut out = [0.0f32, 0.0, 0.0, 1.0];
        for c in 0..channels.min(4) {
            out[c] = halves.get(index + c)?.to_f32();
        }
        Some(out)
    }
}

fn bytemuck_cast_f32(bytes: &[u8]) -> Option<&[f32]> {
    if bytes.as_ptr() as usize % std::mem::align_of::<f32>() != 0 {
        return None;
    }
    Some(unsafe { std::slice::from_raw_parts(bytes.as_ptr() as *const f32, bytes.len() / 4) })
}

fn bytemuck_cast_f16(bytes: &[u8]) -> Option<&[half::f16]> {
    if bytes.as_ptr() as usize % std::mem::align_of::<half::f16>() != 0 {
        return None;
    }
    Some(unsafe {
        std::slice::from_raw_parts(bytes.as_ptr() as *const half::f16, bytes.len() / 2)
    })
}

/// Reinhard tone map, matching what the editor preview expects.
fn tonemap(v: f32) -> f32 {
    let v = v.max(0.0);
    v / (1.0 + v)
}

fn encode_srgb(linear: f32) -> u8 {
    let c = linear.clamp(0.0, 1.0);
    let s = if c <= 0.0031308 {
        c * 12.92
    } else {
        1.055 * c.powf(1.0 / 2.4) - 0.055
    };
    (s * 255.0 + 0.5) as u8
}

/// Radiance-style shared-exponent encoding for the synthetic 1x1 sky panorama.
fn encode_rgbe(rgb: [f32; 3]) -> [u8; 4] {
    let max = rgb[0].max(rgb[1]).max(rgb[2]);
    if max < 1e-32 {
        return [0, 0, 0, 0];
    }
    let (mantissa, exponent) = frexp(max);
    let scale = mantissa * 256.0 / max;
    [
        (rgb[0] * scale).clamp(0.0, 255.0) as u8,
        (rgb[1] * scale).clamp(0.0, 255.0) as u8,
        (rgb[2] * scale).clamp(0.0, 255.0) as u8,
        (exponent + 128) as u8,
    ]
}

fn frexp(x: f32) -> (f32, i32) {
    if x == 0.0 || !x.is_finite() {
        return (x, 0);
    }
    let exponent = x.abs().log2().floor() as i32 + 1;
    (x / (2.0f32).powi(exponent), exponent)
}

fn normalize_or(v: [f32; 3], fallback: [f32; 3]) -> [f32; 3] {
    let len = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt();
    if len < 1e-6 || !len.is_finite() {
        fallback
    } else {
        [v[0] / len, v[1] / len, v[2] / len]
    }
}

/// Area-weighted vertex normals, used when the engine hands over positions
/// without a normal stream.
fn derive_normals(positions: &[[f32; 3]], indices: &[u32]) -> Vec<[f32; 3]> {
    let mut normals = vec![[0.0f32; 3]; positions.len()];

    for tri in indices.chunks_exact(3) {
        let (a, b, c) = (
            positions[tri[0] as usize],
            positions[tri[1] as usize],
            positions[tri[2] as usize],
        );
        let e1 = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
        let e2 = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
        // Un-normalised cross product weights each face by twice its area.
        let n = [
            e1[1] * e2[2] - e1[2] * e2[1],
            e1[2] * e2[0] - e1[0] * e2[2],
            e1[0] * e2[1] - e1[1] * e2[0],
        ];
        for &i in tri {
            let slot = &mut normals[i as usize];
            slot[0] += n[0];
            slot[1] += n[1];
            slot[2] += n[2];
        }
    }

    for n in &mut normals {
        *n = normalize_or(*n, [0.0, 1.0, 0.0]);
    }
    normals
}

fn write_c_string(src: &str, dst: &mut [c_char]) {
    let bytes = src.as_bytes();
    let limit = dst.len().saturating_sub(1);
    // Truncate on a char boundary so the C side never sees a split UTF-8
    // sequence.
    let mut end = bytes.len().min(limit);
    while end > 0 && !src.is_char_boundary(end) {
        end -= 1;
    }
    for (i, &b) in bytes[..end].iter().enumerate() {
        dst[i] = b as c_char;
    }
    dst[end] = 0;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normals_from_a_single_triangle_face_up() {
        let positions = [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 0.0, -1.0]];
        let normals = derive_normals(&positions, &[0, 1, 2]);
        assert_eq!(normals.len(), 3);
        for n in normals {
            assert!((n[1] - 1.0).abs() < 1e-5, "expected +Y normal, got {n:?}");
        }
    }

    #[test]
    fn c_string_truncation_keeps_nul_and_char_boundary() {
        let mut buf = [0 as c_char; 8];
        write_c_string("ααααα", &mut buf);
        assert_eq!(buf[7], 0);
        // 3 two-byte chars fit in 7 usable bytes, the 4th must be dropped whole.
        let bytes: Vec<u8> = buf.iter().take_while(|&&c| c != 0).map(|&c| c as u8).collect();
        assert_eq!(std::str::from_utf8(&bytes).unwrap(), "ααα");
    }

    #[test]
    fn tonemap_is_monotonic_and_bounded() {
        assert_eq!(encode_srgb(tonemap(0.0)), 0);
        assert!(encode_srgb(tonemap(1.0)) < 255);
        assert!(encode_srgb(tonemap(1e6)) >= 254);
    }
}
