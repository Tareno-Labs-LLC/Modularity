// GL half of Project Settings > Graphics Manager > Optimization: capability checks,
// the multi-draw mesh arena and indirect buffers, compute-shader culling, the Hi-Z
// pyramid, non-blocking readback of GPU culling results, and the Automatic strategy.
// The scene-aware half (caches, culling, the render queue) is in Rendering.cpp.

#include "RenderOptimizationInternal.h"
#include "HardwareProfile.h"
#include "../include/Platform/AssetSource.h"

#include <cstring>
#include <fstream>
#include <sstream>

namespace {

constexpr const char* kCullComputePath = "Resources/Shaders/cull_instances_comp.glsl";
constexpr const char* kHiZComputePath = "Resources/Shaders/hiz_reduce_comp.glsl";

constexpr GLsizeiptr kArenaVertexBytes = 8 * sizeof(float);
// Duplicating batched meshes into one buffer costs their VRAM a second time; past this
// the remaining objects just keep drawing one at a time.
constexpr GLsizeiptr kArenaMaxVerts = (96ll * 1024 * 1024) / kArenaVertexBytes;

// Automatic strategy thresholds, in renderable objects. Each tier has a lower bound to
// enter and a lower one to leave, so a scene hovering at a boundary does not flap.
constexpr int kAutoOptimizedEnter = 48;
constexpr int kAutoOptimizedLeave = 32;
constexpr int kAutoMultiDrawEnter = 768;
constexpr int kAutoMultiDrawLeave = 512;
constexpr int kAutoGpuEnter = 4096;
constexpr int kAutoGpuLeave = 3072;

double NowSeconds() {
    return glfwGetTime();
}

std::string GlVersionLabel() {
    const auto& info = Modularity::HardwareProfile::Static();
    return std::to_string(info.glMajor) + "." + std::to_string(info.glMinor);
}

#if !MODULARITY_OPENGL_ES
std::string ReadShaderText(const char* path) {
    std::vector<uint8_t> bytes = Modularity::Platform::GetAssetSource().ReadAll(path);
    if (!bytes.empty()) {
        return std::string(bytes.begin(), bytes.end());
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::string();
    }
    std::stringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

GLuint CompileComputeProgram(const char* path, std::string& error) {
    const std::string source = ReadShaderText(path);
    if (source.empty()) {
        error = std::string(path) + " was not found";
        return 0;
    }
    GLuint stage = glCreateShader(GL_COMPUTE_SHADER);
    const char* text = source.c_str();
    glShaderSource(stage, 1, &text, nullptr);
    glCompileShader(stage);
    GLint ok = 0;
    glGetShaderiv(stage, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(stage, sizeof(log), nullptr, log);
        error = std::string(path) + ": " + log;
        glDeleteShader(stage);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, stage);
    glLinkProgram(program);
    glDeleteShader(stage);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        error = std::string(path) + " (link): " + log;
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

GLint Uniform(GLuint program, const char* name) {
    return glGetUniformLocation(program, name);
}

void ConfigureMultiDrawVao(GLuint vao, GLuint arenaBuffer, GLuint instanceBuffer) {
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, arenaBuffer);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(kArenaVertexBytes),
                          reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(kArenaVertexBytes),
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(kArenaVertexBytes),
                          reinterpret_cast<const void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindBuffer(GL_ARRAY_BUFFER, instanceBuffer);
    for (int column = 0; column < 4; ++column) {
        const GLuint location = static_cast<GLuint>(5 + column);
        glVertexAttribPointer(location, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4),
                              reinterpret_cast<const void*>(sizeof(glm::vec4) * column));
        glEnableVertexAttribArray(location);
        glVertexAttribDivisor(location, 1);
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// Grows a buffer to at least `bytes`, keeping its first `keepBytes`.
void GrowBuffer(GLuint& buffer, GLsizeiptr& capacityBytes, GLsizeiptr bytes, GLsizeiptr keepBytes,
                GLenum usage) {
    if (buffer != 0 && capacityBytes >= bytes) {
        return;
    }
    GLuint grown = 0;
    glGenBuffers(1, &grown);
    glBindBuffer(GL_COPY_WRITE_BUFFER, grown);
    glBufferData(GL_COPY_WRITE_BUFFER, bytes, nullptr, usage);
    if (buffer != 0 && keepBytes > 0) {
        glBindBuffer(GL_COPY_READ_BUFFER, buffer);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
                            std::min(keepBytes, capacityBytes));
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    if (buffer != 0) {
        glDeleteBuffers(1, &buffer);
    }
    buffer = grown;
    capacityBytes = bytes;
}

struct CullUniforms {
    GLint slotCount = -1, commandOffset = -1, cullingMask = -1, frustumPlanes = -1;
    GLint lodEnabled = -1, lodPerspective = -1, lodMinScreenPx = -1, lodPxPerUnit = -1;
    GLint viewDepthRow = -1;
    GLint hizEnabled = -1, hizTex = -1, hizViewProj = -1, hizDepthSize = -1, hizMaxLevel = -1;
};
struct HiZUniforms {
    GLint fromDepth = -1, depthTex = -1, srcSize = -1, dstSize = -1;
};
CullUniforms gCullUniforms;
HiZUniforms gHiZUniforms;
#endif

} // namespace

// ---------------------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------------------

RenderOptimizationSupport Renderer::OptimizationState::querySupport() {
    RenderOptimizationSupport support;
#if MODULARITY_OPENGL_ES
    support.multiDrawIndirectReason = "Needs desktop OpenGL 4.3; OpenGL ES has no multi-draw indirect.";
    support.computeCullingReason = "Needs desktop OpenGL 4.3 compute shaders.";
    support.hiZReason = support.computeCullingReason;
#else
    const auto& info = Modularity::HardwareProfile::Static();
    if (info.glMajor <= 0) {
        support.multiDrawIndirectReason = "The OpenGL renderer has not started yet.";
        support.computeCullingReason = support.multiDrawIndirectReason;
        support.hiZReason = support.multiDrawIndirectReason;
        return support;
    }
    const bool gl43 = Modularity::HardwareProfile::HasGL(4, 3);
    support.multiDrawIndirect = gl43 && glad_glMultiDrawArraysIndirect != nullptr &&
                                glad_glCopyBufferSubData != nullptr &&
                                glad_glVertexAttribDivisor != nullptr;
    if (!support.multiDrawIndirect) {
        support.multiDrawIndirectReason =
            "Needs OpenGL 4.3; this context is " + GlVersionLabel() + ".";
    }
    const bool computeEntryPoints = glad_glDispatchCompute != nullptr &&
                                    glad_glBindBufferBase != nullptr &&
                                    glad_glMemoryBarrier != nullptr;
    support.computeCulling = support.multiDrawIndirect && info.computeShaders && computeEntryPoints;
    if (!support.computeCulling) {
        support.computeCullingReason = (info.computeShaders && computeEntryPoints)
            ? support.multiDrawIndirectReason
            : "Needs compute shaders (OpenGL 4.3); this context is " + GlVersionLabel() + ".";
    }
    support.indirectCount = Modularity::HardwareProfile::HasGL(4, 6) &&
                            glad_glMultiDrawArraysIndirectCount != nullptr;
    support.hiZ = support.computeCulling && glad_glBindImageTexture != nullptr &&
                  glad_glTexStorage2D != nullptr;
    if (!support.hiZ) {
        support.hiZReason = support.computeCulling
            ? "Needs image load/store and immutable textures (OpenGL 4.3)."
            : support.computeCullingReason;
    }
#endif
    return support;
}

RenderOptimizationSupport Renderer::getOptimizationSupport() const {
    RenderOptimizationSupport support = OptimizationState::querySupport();
    if (optState) {
        if (support.multiDrawIndirect && optState->multiDrawFailed) {
            support.multiDrawIndirect = false;
            support.multiDrawIndirectReason = optState->multiDrawFailure;
        }
        if (support.computeCulling && !optState->computeFailure.empty()) {
            support.computeCulling = false;
            support.hiZ = false;
            support.computeCullingReason = optState->computeFailure;
            support.hiZReason = optState->computeFailure;
        }
        if (!support.multiDrawIndirect) {
            support.computeCulling = false;
            support.hiZ = false;
            if (support.computeCullingReason.empty()) {
                support.computeCullingReason = "Needs Multi-Draw Indirect.";
            }
            if (support.hiZReason.empty()) {
                support.hiZReason = support.computeCullingReason;
            }
        }
    }
    return support;
}

void Renderer::setOptimizationSettings(const RenderOptimizationSettings& settings) {
    optimizationSettings = settings;
    optimizationSettings.lodMinScreenSizePx =
        std::clamp(optimizationSettings.lodMinScreenSizePx, 0.1f, 64.0f);
    if (!optState) {
        return;
    }
    // Turning a GPU feature off hands its memory back rather than holding the arena and
    // pyramids for a path that no longer runs.
    const bool anyMultiDraw = settings.multiDrawIndirect;
    if (!anyMultiDraw || !settings.hiZOcclusionCulling) {
        for (auto& pyramid : optState->hiZ) {
#if !MODULARITY_OPENGL_ES
            if (pyramid.texture) glDeleteTextures(1, &pyramid.texture);
#endif
            pyramid.texture = 0;
        }
        optState->hiZ.clear();
    }
    if (!anyMultiDraw) {
        optState->releaseGpuResources();
    }
}

// ---------------------------------------------------------------------------------------
// Plan: what one pass runs
// ---------------------------------------------------------------------------------------

Renderer::OptimizationState::PassPlan Renderer::OptimizationState::resolvePlan(
    const RenderOptimizationSettings& settings, const RenderOptimizationSupport& capabilities,
    int candidateCount, bool reflectionCapture) {
    PassPlan plan;
    const bool multiDrawAvailable = capabilities.multiDrawIndirect && !multiDrawFailed;
    const bool gpuAvailable = multiDrawAvailable && capabilities.computeCulling && computeFailure.empty();

    if (settings.strategy == RenderOptimizationStrategy::Manual) {
        plan.cpuCulling = settings.cpuFrustumCulling;
        plan.staticCache = settings.staticObjectCaching;
        plan.sorting = settings.materialSorting;
        plan.lod = settings.lodOptimization;
        plan.multiDraw = settings.multiDrawIndirect && multiDrawAvailable;
        plan.gpuCulling = plan.multiDraw && settings.gpuDrivenCulling && gpuAvailable;
        plan.hiZ = plan.gpuCulling && settings.hiZOcclusionCulling && capabilities.hiZ;
    } else {
        plan.automatic = true;
        AutoPolicy& policy = autoPolicy;
        const double now = NowSeconds();
        // Reflection captures render the same scene six times from inside the outer
        // pass; they follow the tier the outer pass picked rather than voting.
        if (!reflectionCapture) {
            policy.lastCandidates = candidateCount;
            const int tier = policy.tier;
            // A tier is entered at one size and left at a smaller one, so a scene sitting
            // on a boundary does not flip paths every frame.
            const auto reaches = [&](int currentTier, int enterAt, int leaveBelow) {
                return candidateCount >= (tier >= currentTier ? leaveBelow : enterAt);
            };
            int wanted = 0;
            if (reaches(1, kAutoOptimizedEnter, kAutoOptimizedLeave)) {
                wanted = 1;
                if (reaches(2, kAutoMultiDrawEnter, kAutoMultiDrawLeave)) {
                    wanted = 2;
                    if (reaches(3, kAutoGpuEnter, kAutoGpuLeave)) {
                        wanted = 3;
                    }
                }
            }
            // Multi-draw only pays when batches actually merge objects. Measured from the
            // passes that ran it: too few objects per batch and it is overhead, so back off
            // and try again later with a fresh measurement.
            if (wanted >= 2 && (!multiDrawAvailable || !settings.multiDrawIndirect ||
                                now < policy.multiDrawBlockedUntil)) {
                wanted = 1;
            }
            if (wanted >= 2 && policy.emaMembersPerBatch >= 0.0f &&
                policy.emaMembersPerBatch < 2.0f) {
                policy.multiDrawBlockedUntil = now + 10.0;
                policy.emaMembersPerBatch = -1.0f;
                wanted = 1;
            }
            // GPU culling has to beat CPU culling on this machine, not just exist. A
            // software rasterizer never wins. Otherwise CPU culling runs first so its cost
            // is known, GPU culling gets a turn, and it is dropped for a while if it
            // measured slower.
            if (wanted >= 3) {
                const auto& info = Modularity::HardwareProfile::Static();
                if (!gpuAvailable || !settings.gpuDrivenCulling || info.softwareRasterizer ||
                    now < policy.gpuBlockedUntil || policy.emaCpuCullMs < 0.0f) {
                    wanted = 2;
                } else if (policy.emaGpuCullMs >= 0.0f &&
                           policy.emaGpuCullMs > policy.emaCpuCullMs * 1.25f + 0.02f) {
                    policy.gpuBlockedUntil = now + 15.0;
                    policy.emaGpuCullMs = -1.0f;
                    wanted = 2;
                }
            }
            if (wanted != tier) {
                policy.tier = wanted;
                policy.tierSince = now;
            }
        }
        const int tier = policy.tier;
        if (tier >= 1) {
            plan.cpuCulling = settings.cpuFrustumCulling;
            plan.staticCache = settings.staticObjectCaching;
            plan.sorting = settings.materialSorting;
            plan.lod = settings.lodOptimization;
        }
        if (tier >= 2) {
            plan.multiDraw = settings.multiDrawIndirect && multiDrawAvailable;
        }
        if (tier >= 3) {
            plan.gpuCulling = plan.multiDraw && settings.gpuDrivenCulling && gpuAvailable;
            plan.hiZ = plan.gpuCulling && settings.hiZOcclusionCulling && capabilities.hiZ;
        }
    }
    plan.active = plan.cpuCulling || plan.staticCache || plan.sorting || plan.lod ||
                  plan.multiDraw;
    return plan;
}

void Renderer::OptimizationState::notePassCosts(const PassPlan& plan, int batchedObjects,
                                                int batches, float batchedCpuCullMs,
                                                float gpuCullMs) {
    if (!plan.automatic) {
        return;
    }
    AutoPolicy& policy = autoPolicy;
    auto ema = [](float& value, float sample) {
        value = (value < 0.0f) ? sample : value * 0.9f + sample * 0.1f;
    };
    if (plan.multiDraw && batches > 0) {
        ema(policy.emaMembersPerBatch,
            static_cast<float>(batchedObjects) / static_cast<float>(batches));
    }
    if (plan.multiDraw && !plan.gpuCulling && batches > 0) {
        ema(policy.emaCpuCullMs, batchedCpuCullMs);
    }
    if (plan.gpuCulling && gpuCullMs >= 0.0f) {
        ema(policy.emaGpuCullMs, gpuCullMs);
    }
}

// ---------------------------------------------------------------------------------------
// Compute programs
// ---------------------------------------------------------------------------------------

bool Renderer::OptimizationState::ensureComputePrograms() {
#if MODULARITY_OPENGL_ES
    return false;
#else
    if (computeProgramsTried) {
        return cullProgram != 0 && hiZProgram != 0;
    }
    computeProgramsTried = true;
    std::string error;
    cullProgram = CompileComputeProgram(kCullComputePath, error);
    if (cullProgram != 0) {
        hiZProgram = CompileComputeProgram(kHiZComputePath, error);
    }
    if (cullProgram == 0 || hiZProgram == 0) {
        computeFailure = "GPU culling shaders failed to build, so culling stays on the CPU. " + error;
        std::cerr << "[Renderer] " << computeFailure << "\n";
        if (cullProgram) glDeleteProgram(cullProgram);
        if (hiZProgram) glDeleteProgram(hiZProgram);
        cullProgram = 0;
        hiZProgram = 0;
        return false;
    }
    CullUniforms& c = gCullUniforms;
    c.slotCount = Uniform(cullProgram, "slotCount");
    c.commandOffset = Uniform(cullProgram, "commandOffset");
    c.cullingMask = Uniform(cullProgram, "cullingMask");
    c.frustumPlanes = Uniform(cullProgram, "frustumPlanes");
    c.lodEnabled = Uniform(cullProgram, "lodEnabled");
    c.lodPerspective = Uniform(cullProgram, "lodPerspective");
    c.lodMinScreenPx = Uniform(cullProgram, "lodMinScreenPx");
    c.lodPxPerUnit = Uniform(cullProgram, "lodPxPerUnit");
    c.viewDepthRow = Uniform(cullProgram, "viewDepthRow");
    c.hizEnabled = Uniform(cullProgram, "hizEnabled");
    c.hizTex = Uniform(cullProgram, "hizTex");
    c.hizViewProj = Uniform(cullProgram, "hizViewProj");
    c.hizDepthSize = Uniform(cullProgram, "hizDepthSize");
    c.hizMaxLevel = Uniform(cullProgram, "hizMaxLevel");
    HiZUniforms& h = gHiZUniforms;
    h.fromDepth = Uniform(hiZProgram, "fromDepth");
    h.depthTex = Uniform(hiZProgram, "depthTex");
    h.srcSize = Uniform(hiZProgram, "srcSize");
    h.dstSize = Uniform(hiZProgram, "dstSize");
    return true;
#endif
}

// ---------------------------------------------------------------------------------------
// Mesh arena and multi-draw buffers
// ---------------------------------------------------------------------------------------

void Renderer::OptimizationState::beginArenaRegistration(bool forShadows) {
    for (ArenaMesh& entry : multiDraw.arenaMeshes) {
        (forShadows ? entry.usedByShadows : entry.usedByMain) = false;
    }
}

bool Renderer::OptimizationState::ensureArenaMesh(Mesh* mesh, GLint& outFirst, GLsizei& outCount,
                                                  bool forShadows) {
#if MODULARITY_OPENGL_ES
    (void)mesh; (void)outFirst; (void)outCount; (void)forShadows;
    return false;
#else
    if (!mesh || mesh->isDynamic() || mesh->usesBones() || mesh->getVertexBuffer() == 0) {
        return false;
    }
    MultiDrawState& md = multiDraw;
    auto found = md.arenaLookup.find(mesh->getSerial());
    if (found != md.arenaLookup.end()) {
        ArenaMesh& entry = md.arenaMeshes[found->second];
        (forShadows ? entry.usedByShadows : entry.usedByMain) = true;
        outFirst = entry.first;
        outCount = entry.count;
        return true;
    }
    const GLsizei vertexCount = mesh->getVertexCount();
    if (vertexCount <= 0 || md.arenaUsedVerts + vertexCount > kArenaMaxVerts) {
        return false;
    }
    if (md.arenaUsedVerts + vertexCount > md.arenaCapacityVerts || md.arenaBuffer == 0) {
        GLsizeiptr capacityBytes = md.arenaCapacityVerts * kArenaVertexBytes;
        const GLsizeiptr wantedVerts = std::min<GLsizeiptr>(
            kArenaMaxVerts,
            std::max<GLsizeiptr>({md.arenaCapacityVerts * 2, md.arenaUsedVerts + vertexCount,
                                  static_cast<GLsizeiptr>(64 * 1024)}));
        GrowBuffer(md.arenaBuffer, capacityBytes, wantedVerts * kArenaVertexBytes,
                   md.arenaUsedVerts * kArenaVertexBytes, GL_STATIC_DRAW);
        md.arenaCapacityVerts = wantedVerts;
        // Attribute bindings capture the buffer, so a grown arena needs both VAOs
        // re-pointed. Before an instance buffer exists there is nothing to point at yet;
        // the capacity functions configure each VAO once both buffers are there.
        if (md.vao != 0 && md.instanceBuffer != 0) {
            ConfigureMultiDrawVao(md.vao, md.arenaBuffer, md.instanceBuffer);
        }
        if (shadowBatch.vao != 0 && shadowBatch.instanceBuffer != 0) {
            ConfigureMultiDrawVao(shadowBatch.vao, md.arenaBuffer, shadowBatch.instanceBuffer);
        }
    }
    glBindBuffer(GL_COPY_READ_BUFFER, mesh->getVertexBuffer());
    glBindBuffer(GL_COPY_WRITE_BUFFER, md.arenaBuffer);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0,
                        md.arenaUsedVerts * kArenaVertexBytes,
                        static_cast<GLsizeiptr>(vertexCount) * kArenaVertexBytes);
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

    ArenaMesh entry;
    entry.serial = mesh->getSerial();
    entry.first = static_cast<GLint>(md.arenaUsedVerts);
    entry.count = vertexCount;
    (forShadows ? entry.usedByShadows : entry.usedByMain) = true;
    md.arenaLookup[entry.serial] = static_cast<uint32_t>(md.arenaMeshes.size());
    md.arenaMeshes.push_back(entry);
    md.arenaUsedVerts += vertexCount;
    outFirst = entry.first;
    outCount = entry.count;
    return true;
#endif
}

bool Renderer::OptimizationState::compactArenaIfWasteful() {
    // Called at a membership rebuild, after every member re-registered its mesh. Meshes
    // neither the main batches nor the shadow batches use any more (deleted, re-imported,
    // no longer eligible) are dead space; once they outweigh the live ones the arena is
    // emptied and its generation moves, so the caller re-registers its members and the
    // other set rebuilds before it next draws. Returns true then.
    MultiDrawState& md = multiDraw;
    GLsizeiptr dead = 0;
    for (const ArenaMesh& entry : md.arenaMeshes) {
        if (!entry.usedByMain && !entry.usedByShadows) dead += entry.count;
    }
    md.arenaDeadVerts = dead;
    const bool compact = dead > 256 * 1024 && dead * 2 > md.arenaUsedVerts;
    if (compact) {
        md.arenaMeshes.clear();
        md.arenaLookup.clear();
        md.arenaUsedVerts = 0;
        md.arenaDeadVerts = 0;
        ++md.arenaGeneration;
    }
    return compact;
}

bool Renderer::OptimizationState::ensureMultiDrawCapacity(size_t slotCount, int commandRegions) {
#if MODULARITY_OPENGL_ES
    (void)slotCount; (void)commandRegions;
    return false;
#else
    MultiDrawState& md = multiDraw;
    const size_t wanted = std::max<size_t>(slotCount, 1);
    commandRegions = std::max(1, commandRegions);
    bool vaoDirty = false;
    if (md.vao == 0) {
        glGenVertexArrays(1, &md.vao);
        vaoDirty = true;
    }
    if (md.counterBuffer == 0) {
        glGenBuffers(1, &md.counterBuffer);
        glBindBuffer(GL_COPY_WRITE_BUFFER, md.counterBuffer);
        const GLuint zero = 0;
        glBufferData(GL_COPY_WRITE_BUFFER, sizeof(GLuint), &zero, GL_DYNAMIC_COPY);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }
    if (wanted > md.instanceCapacity || md.instanceBuffer == 0) {
        const size_t capacity = std::max<size_t>(wanted + wanted / 2, 256);
        auto reallocate = [&](GLuint& buffer, GLsizeiptr bytes, GLenum usage) {
            if (buffer) glDeleteBuffers(1, &buffer);
            glGenBuffers(1, &buffer);
            glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
            glBufferData(GL_COPY_WRITE_BUFFER, bytes, nullptr, usage);
        };
        reallocate(md.instanceBuffer, static_cast<GLsizeiptr>(capacity * sizeof(glm::mat4)),
                   GL_DYNAMIC_DRAW);
        reallocate(md.templateBuffer, static_cast<GLsizeiptr>(capacity * sizeof(glm::uvec4)),
                   GL_DYNAMIC_DRAW);
        reallocate(md.boundsBuffer, static_cast<GLsizeiptr>(capacity * 2 * sizeof(glm::vec4)),
                   GL_DYNAMIC_DRAW);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        md.instanceCapacity = capacity;
        md.commandCapacity = 0;   // regions are sized from the slot capacity
        std::fill(md.slotVersion.begin(), md.slotVersion.end(), 0u);
        md.templatesDirty = true;
        vaoDirty = true;
    }
    if (md.commandCapacity != md.instanceCapacity || md.commandRegions != commandRegions ||
        md.commandBuffer == 0) {
        if (md.commandBuffer) glDeleteBuffers(1, &md.commandBuffer);
        glGenBuffers(1, &md.commandBuffer);
        glBindBuffer(GL_COPY_WRITE_BUFFER, md.commandBuffer);
        glBufferData(GL_COPY_WRITE_BUFFER,
                     static_cast<GLsizeiptr>(md.instanceCapacity * commandRegions *
                                             sizeof(RenderOpt::DrawArraysIndirectCommand)),
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        md.commandCapacity = md.instanceCapacity;
        md.commandRegions = commandRegions;
        md.nextRegion = 0;
    }
    if (vaoDirty && md.arenaBuffer != 0) {
        ConfigureMultiDrawVao(md.vao, md.arenaBuffer, md.instanceBuffer);
    }
    return md.arenaBuffer != 0;
#endif
}

void Renderer::OptimizationState::uploadMultiDrawSlots() {
#if !MODULARITY_OPENGL_ES
    MultiDrawState& md = multiDraw;
    const size_t slotCount = md.slotObject.size();
    if (slotCount == 0 || md.instanceBuffer == 0) {
        return;
    }
    size_t dirtyBegin = slotCount;
    size_t dirtyEnd = 0;
    for (size_t slot = 0; slot < slotCount; ++slot) {
        const uint32_t objectIndex = md.slotObject[slot];
        const RenderOpt::CachedRenderable& entry = cache[objectIndex];
        if (md.slotVersion[slot] == entry.transformVersion && md.slotVersion[slot] != 0) {
            continue;
        }
        md.slotVersion[slot] = entry.transformVersion;
        md.slotMatrices[slot] = entry.model;
        const RenderOpt::CullBounds& b = bounds[objectIndex];
        md.slotBounds[slot * 2] = glm::vec4(b.center, b.radius);
        md.slotBounds[slot * 2 + 1] = glm::vec4(b.extents, 0.0f);
        dirtyBegin = std::min(dirtyBegin, slot);
        dirtyEnd = std::max(dirtyEnd, slot + 1);
    }
    if (dirtyBegin < dirtyEnd) {
        const size_t count = dirtyEnd - dirtyBegin;
        glBindBuffer(GL_COPY_WRITE_BUFFER, md.instanceBuffer);
        glBufferSubData(GL_COPY_WRITE_BUFFER,
                        static_cast<GLintptr>(dirtyBegin * sizeof(glm::mat4)),
                        static_cast<GLsizeiptr>(count * sizeof(glm::mat4)),
                        &md.slotMatrices[dirtyBegin]);
        glBindBuffer(GL_COPY_WRITE_BUFFER, md.boundsBuffer);
        glBufferSubData(GL_COPY_WRITE_BUFFER,
                        static_cast<GLintptr>(dirtyBegin * 2 * sizeof(glm::vec4)),
                        static_cast<GLsizeiptr>(count * 2 * sizeof(glm::vec4)),
                        &md.slotBounds[dirtyBegin * 2]);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }
    if (md.templatesDirty) {
        glBindBuffer(GL_COPY_WRITE_BUFFER, md.templateBuffer);
        glBufferSubData(GL_COPY_WRITE_BUFFER, 0,
                        static_cast<GLsizeiptr>(slotCount * sizeof(glm::uvec4)),
                        md.slotTemplates.data());
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        md.templatesDirty = false;
    }
#endif
}

void Renderer::OptimizationState::uploadPassCommands() {
#if !MODULARITY_OPENGL_ES
    MultiDrawState& md = multiDraw;
    // CPU culling packs each batch's visible commands at the start of the batch's slot
    // range, so only up to the last batch's last visible command needs to go up.
    size_t used = 0;
    for (const MultiDrawBatch& batch : md.batches) {
        if (batch.visibleCount > 0) {
            used = std::max<size_t>(used, batch.firstSlot + batch.visibleCount);
        }
    }
    if (used == 0 || md.instanceCapacity == 0) {
        return;
    }
    if (md.cpuCommandBuffer == 0) {
        glGenBuffers(1, &md.cpuCommandBuffer);
    }
    // Orphaning hands the driver fresh storage (same size every time, so it recycles), and
    // a previous pass still drawing from the old contents never stalls this upload.
    glBindBuffer(GL_COPY_WRITE_BUFFER, md.cpuCommandBuffer);
    glBufferData(GL_COPY_WRITE_BUFFER,
                 static_cast<GLsizeiptr>(md.instanceCapacity * sizeof(RenderOpt::DrawArraysIndirectCommand)),
                 nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_COPY_WRITE_BUFFER, 0,
                    static_cast<GLsizeiptr>(used * sizeof(RenderOpt::DrawArraysIndirectCommand)),
                    md.passCommands.data());
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
#endif
}

// ---------------------------------------------------------------------------------------
// GPU culling
// ---------------------------------------------------------------------------------------

int Renderer::OptimizationState::dispatchGpuCulling(const Pass& pass,
                                                    const RenderOptimizationSettings& settings,
                                                    bool useHiZ, const RenderStats* statsOwner) {
#if MODULARITY_OPENGL_ES
    (void)pass; (void)settings; (void)useHiZ; (void)statsOwner;
    return -1;
#else
    MultiDrawState& md = multiDraw;
    const size_t slotCount = md.slotObject.size();
    if (slotCount == 0 || cullProgram == 0 || md.commandBuffer == 0 || md.commandRegions <= 0) {
        return -1;
    }
    const int region = md.nextRegion;
    md.nextRegion = (md.nextRegion + 1) % md.commandRegions;

    const HiZPyramid* pyramid = useHiZ ? findUsableHiZ(pass) : nullptr;

    glUseProgram(cullProgram);
    const CullUniforms& u = gCullUniforms;
    glUniform1ui(u.slotCount, static_cast<GLuint>(slotCount));
    glUniform1ui(u.commandOffset, static_cast<GLuint>(region * md.commandCapacity));
    glUniform1ui(u.cullingMask, pass.cullingMask);
    glUniform4fv(u.frustumPlanes, 6, &pass.planes[0].x);
    glUniform1i(u.lodEnabled, pass.plan.lod ? 1 : 0);
    glUniform1i(u.lodPerspective, pass.perspective ? 1 : 0);
    glUniform1f(u.lodMinScreenPx, settings.lodMinScreenSizePx);
    glUniform1f(u.lodPxPerUnit, pass.lodPxPerUnit);
    glUniform4fv(u.viewDepthRow, 1, &pass.viewDepthRow.x);
    glUniform1i(u.hizEnabled, pyramid ? 1 : 0);
    glUniform1i(u.hizTex, 0);
    if (pyramid) {
        glUniformMatrix4fv(u.hizViewProj, 1, GL_FALSE, glm::value_ptr(pyramid->viewProj));
        glUniform2f(u.hizDepthSize, static_cast<float>(pyramid->depthWidth),
                    static_cast<float>(pyramid->depthHeight));
        glUniform1f(u.hizMaxLevel, static_cast<float>(pyramid->levels - 1));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, pyramid->texture);
    }

    const GLuint zero = 0;
    glBindBuffer(GL_COPY_WRITE_BUFFER, md.counterBuffer);
    glBufferSubData(GL_COPY_WRITE_BUFFER, 0, sizeof(GLuint), &zero);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, md.boundsBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, md.templateBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, md.commandBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, md.counterBuffer);

    // Results come back through a fence a later frame polls; nothing here waits. One
    // sample in flight at a time, and only every so often (see readbackIssueFrame).
    GpuReadback* readback = nullptr;
    bool anyPending = false;
    for (const GpuReadback& candidate : readbacks) {
        anyPending = anyPending || candidate.pending;
    }
    if (!anyPending && pass.frameSerial >= readbackIssueFrame + 15) {
        readback = &readbacks[0];
        readbackIssueFrame = pass.frameSerial;
    }
    if (readback) {
        if (readback->queries[0] == 0) glGenQueries(2, readback->queries);
        glQueryCounter(readback->queries[0], GL_TIMESTAMP);
    }
    glDispatchCompute(static_cast<GLuint>((slotCount + 63) / 64), 1, 1);
    if (readback) {
        glQueryCounter(readback->queries[1], GL_TIMESTAMP);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        if (readback->buffer == 0) {
            glGenBuffers(1, &readback->buffer);
            glBindBuffer(GL_COPY_WRITE_BUFFER, readback->buffer);
            glBufferData(GL_COPY_WRITE_BUFFER, sizeof(GLuint), nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_COPY_READ_BUFFER, md.counterBuffer);
        glBindBuffer(GL_COPY_WRITE_BUFFER, readback->buffer);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, sizeof(GLuint));
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        readback->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        readback->owner = statsOwner;
        readback->pending = readback->fence != nullptr;
    }
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT);

    for (GLuint binding = 0; binding < 4; ++binding) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, 0);
    }
    if (pyramid) {
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glUseProgram(0);
    return region;
#endif
}

void Renderer::OptimizationState::drawMultiDrawBatch(const Pass& pass,
                                                     const MultiDrawBatch& batch) const {
#if MODULARITY_OPENGL_ES
    (void)pass; (void)batch;
#else
    const MultiDrawState& md = multiDraw;
    GLsizei drawCount = 0;
    size_t firstCommand = 0;
    GLuint commands = 0;
    if (pass.gpuCommandRegion >= 0) {
        firstCommand = static_cast<size_t>(pass.gpuCommandRegion) * md.commandCapacity + batch.firstSlot;
        drawCount = static_cast<GLsizei>(batch.slotCount);
        commands = md.commandBuffer;
    } else {
        firstCommand = batch.firstSlot;
        drawCount = static_cast<GLsizei>(batch.visibleCount);
        commands = md.cpuCommandBuffer;
    }
    if (drawCount <= 0 || md.vao == 0 || commands == 0) {
        return;
    }
    glBindVertexArray(md.vao);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, commands);
    glMultiDrawArraysIndirect(
        GL_TRIANGLES,
        reinterpret_cast<const void*>(firstCommand * sizeof(RenderOpt::DrawArraysIndirectCommand)),
        drawCount, sizeof(RenderOpt::DrawArraysIndirectCommand));
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindVertexArray(0);
#endif
}

bool Renderer::OptimizationState::ensureShadowBatchCapacity(size_t slotCount) {
#if MODULARITY_OPENGL_ES
    (void)slotCount;
    return false;
#else
    ShadowBatchState& sb = shadowBatch;
    const MultiDrawState& md = multiDraw;
    if (md.arenaBuffer == 0) {
        return false;
    }
    bool vaoDirty = false;
    if (sb.vao == 0) {
        glGenVertexArrays(1, &sb.vao);
        vaoDirty = true;
    }
    const size_t wanted = std::max<size_t>(slotCount, 1);
    if (wanted > sb.instanceCapacity || sb.instanceBuffer == 0) {
        const size_t capacity = std::max<size_t>(wanted + wanted / 2, 256);
        if (sb.instanceBuffer) glDeleteBuffers(1, &sb.instanceBuffer);
        if (sb.commandBuffer) glDeleteBuffers(1, &sb.commandBuffer);
        glGenBuffers(1, &sb.instanceBuffer);
        glBindBuffer(GL_COPY_WRITE_BUFFER, sb.instanceBuffer);
        glBufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(capacity * sizeof(glm::mat4)),
                     nullptr, GL_DYNAMIC_DRAW);
        glGenBuffers(1, &sb.commandBuffer);
        glBindBuffer(GL_COPY_WRITE_BUFFER, sb.commandBuffer);
        glBufferData(GL_COPY_WRITE_BUFFER,
                     static_cast<GLsizeiptr>(capacity * sizeof(RenderOpt::DrawArraysIndirectCommand)),
                     nullptr, GL_STREAM_DRAW);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        sb.instanceCapacity = capacity;
        sb.commandCapacity = capacity;
        std::fill(sb.slotVersion.begin(), sb.slotVersion.end(), 0u);
        vaoDirty = true;
    }
    if (vaoDirty) {
        ConfigureMultiDrawVao(sb.vao, md.arenaBuffer, sb.instanceBuffer);
    }
    return true;
#endif
}

void Renderer::OptimizationState::uploadShadowSlots() {
#if !MODULARITY_OPENGL_ES
    ShadowBatchState& sb = shadowBatch;
    const size_t slotCount = sb.slotObject.size();
    if (slotCount == 0 || sb.instanceBuffer == 0) {
        return;
    }
    size_t dirtyBegin = slotCount;
    size_t dirtyEnd = 0;
    for (size_t slot = 0; slot < slotCount; ++slot) {
        const RenderOpt::CachedRenderable& entry = cache[sb.slotObject[slot]];
        if (sb.slotVersion[slot] == entry.transformVersion && sb.slotVersion[slot] != 0) {
            continue;
        }
        sb.slotVersion[slot] = entry.transformVersion;
        sb.slotMatrices[slot] = entry.model;
        dirtyBegin = std::min(dirtyBegin, slot);
        dirtyEnd = std::max(dirtyEnd, slot + 1);
    }
    if (dirtyBegin < dirtyEnd) {
        glBindBuffer(GL_COPY_WRITE_BUFFER, sb.instanceBuffer);
        glBufferSubData(GL_COPY_WRITE_BUFFER,
                        static_cast<GLintptr>(dirtyBegin * sizeof(glm::mat4)),
                        static_cast<GLsizeiptr>((dirtyEnd - dirtyBegin) * sizeof(glm::mat4)),
                        &sb.slotMatrices[dirtyBegin]);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }
#endif
}

void Renderer::OptimizationState::drawShadowBatch(size_t commandCount) {
#if !MODULARITY_OPENGL_ES
    ShadowBatchState& sb = shadowBatch;
    if (commandCount == 0 || sb.vao == 0 || sb.commandBuffer == 0) {
        return;
    }
    // One upload per light (or cube face), orphaned so a draw still reading the previous
    // light's commands never stalls it.
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, sb.commandBuffer);
    glBufferData(GL_DRAW_INDIRECT_BUFFER,
                 static_cast<GLsizeiptr>(sb.commandCapacity * sizeof(RenderOpt::DrawArraysIndirectCommand)),
                 nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0,
                    static_cast<GLsizeiptr>(commandCount * sizeof(RenderOpt::DrawArraysIndirectCommand)),
                    sb.commands.data());
    glBindVertexArray(sb.vao);
    glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, static_cast<GLsizei>(commandCount),
                              sizeof(RenderOpt::DrawArraysIndirectCommand));
    glBindVertexArray(0);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
#else
    (void)commandCount;
#endif
}

// ---------------------------------------------------------------------------------------
// Hi-Z (experimental)
// ---------------------------------------------------------------------------------------

const Renderer::OptimizationState::HiZPyramid*
Renderer::OptimizationState::findUsableHiZ(const Pass& pass) const {
    if (pass.depthTexture == 0) {
        return nullptr;
    }
    for (const HiZPyramid& pyramid : hiZ) {
        if (pyramid.depthTexture != pass.depthTexture || !pyramid.valid || pyramid.texture == 0) {
            continue;
        }
        if (pyramid.depthWidth != pass.width || pyramid.depthHeight != pass.height) {
            return nullptr;
        }
        // Only last frame's depth: an older pyramid can hold occluders that have since
        // moved away (a view that was hidden for a while, say).
        if (pyramid.builtFrame + 1 < pass.frameSerial) {
            return nullptr;
        }
        // The pyramid is last frame's depth from last frame's camera. Reprojection
        // handles ordinary motion; a cut or a big jump would hide things that are now in
        // plain view, so skip the test for that frame instead.
        const glm::vec3 moved = pass.cameraPosition - pyramid.cameraPosition;
        const float turn = glm::dot(glm::normalize(pass.cameraForward),
                                    glm::normalize(pyramid.cameraForward));
        if (glm::dot(moved, moved) > 4.0f || !(turn > 0.94f)) {
            return nullptr;
        }
        return &pyramid;
    }
    return nullptr;
}

void Renderer::OptimizationState::buildHiZ(const Pass& pass) {
#if MODULARITY_OPENGL_ES
    (void)pass;
#else
    if (pass.depthTexture == 0 || pass.width <= 1 || pass.height <= 1 || hiZProgram == 0) {
        return;
    }
    HiZPyramid* pyramid = nullptr;
    for (HiZPyramid& candidate : hiZ) {
        if (candidate.depthTexture == pass.depthTexture) {
            pyramid = &candidate;
            break;
        }
    }
    if (!pyramid) {
        // Views come and go (previews, closed panels); keep a few and recycle the oldest.
        if (hiZ.size() >= 4) {
            auto oldest = std::min_element(hiZ.begin(), hiZ.end(),
                [](const HiZPyramid& a, const HiZPyramid& b) { return a.builtFrame < b.builtFrame; });
            if (oldest->texture) glDeleteTextures(1, &oldest->texture);
            *oldest = HiZPyramid{};
            pyramid = &*oldest;
        } else {
            hiZ.emplace_back();
            pyramid = &hiZ.back();
        }
        pyramid->depthTexture = pass.depthTexture;
    }
    const int width = std::max(1, pass.width / 2);
    const int height = std::max(1, pass.height / 2);
    if (pyramid->texture == 0 || pyramid->width != width || pyramid->height != height) {
        if (pyramid->texture) glDeleteTextures(1, &pyramid->texture);
        int levels = 1;
        while ((width >> levels) > 0 || (height >> levels) > 0) {
            ++levels;
        }
        glGenTextures(1, &pyramid->texture);
        glBindTexture(GL_TEXTURE_2D, pyramid->texture);
        glTexStorage2D(GL_TEXTURE_2D, levels, GL_R32F, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        pyramid->width = width;
        pyramid->height = height;
        pyramid->levels = levels;
    }
    pyramid->depthWidth = pass.width;
    pyramid->depthHeight = pass.height;

    const HiZUniforms& u = gHiZUniforms;
    glUseProgram(hiZProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pass.depthTexture);
    glUniform1i(u.depthTex, 0);

    int srcWidth = pass.width;
    int srcHeight = pass.height;
    for (int level = 0; level < pyramid->levels; ++level) {
        const int dstWidth = std::max(1, width >> level);
        const int dstHeight = std::max(1, height >> level);
        const bool fromDepth = (level == 0);
        glUniform1i(u.fromDepth, fromDepth ? 1 : 0);
        glUniform2i(u.srcSize, srcWidth, srcHeight);
        glUniform2i(u.dstSize, dstWidth, dstHeight);
        glBindImageTexture(0, pyramid->texture, fromDepth ? 0 : level - 1, GL_FALSE, 0,
                           GL_READ_ONLY, GL_R32F);
        glBindImageTexture(1, pyramid->texture, level, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
        glDispatchCompute(static_cast<GLuint>((dstWidth + 7) / 8),
                          static_cast<GLuint>((dstHeight + 7) / 8), 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        srcWidth = dstWidth;
        srcHeight = dstHeight;
    }
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    pyramid->viewProj = pass.proj * pass.view;
    pyramid->cameraPosition = pass.cameraPosition;
    pyramid->cameraForward = pass.cameraForward;
    pyramid->valid = true;
    pyramid->builtFrame = pass.frameSerial;
#endif
}

// ---------------------------------------------------------------------------------------
// Async readback
// ---------------------------------------------------------------------------------------

void Renderer::OptimizationState::pollReadbacks() {
#if !MODULARITY_OPENGL_ES
    for (GpuReadback& readback : readbacks) {
        if (!readback.pending || readback.fence == nullptr) {
            continue;
        }
        GLint status = GL_UNSIGNALED;
        glGetSynciv(readback.fence, GL_SYNC_STATUS, sizeof(status), nullptr, &status);
        if (status != GL_SIGNALED) {
            continue;
        }
        glDeleteSync(readback.fence);
        readback.fence = nullptr;
        readback.pending = false;

        GLuint visible = 0;
        glBindBuffer(GL_COPY_READ_BUFFER, readback.buffer);
        glGetBufferSubData(GL_COPY_READ_BUFFER, 0, sizeof(GLuint), &visible);
        glBindBuffer(GL_COPY_READ_BUFFER, 0);

        float ms = -1.0f;
        GLint available = 0;
        glGetQueryObjectiv(readback.queries[1], GL_QUERY_RESULT_AVAILABLE, &available);
        if (available) {
            GLuint64 start = 0;
            GLuint64 end = 0;
            glGetQueryObjectui64v(readback.queries[0], GL_QUERY_RESULT, &start);
            glGetQueryObjectui64v(readback.queries[1], GL_QUERY_RESULT, &end);
            if (end >= start) {
                ms = static_cast<float>(static_cast<double>(end - start) / 1.0e6);
            }
        }
        GpuResult* slot = nullptr;
        for (GpuResult& result : gpuResults) {
            if (result.owner == readback.owner) {
                slot = &result;
                break;
            }
        }
        if (!slot) {
            for (GpuResult& result : gpuResults) {
                if (result.owner == nullptr) {
                    slot = &result;
                    break;
                }
            }
        }
        if (!slot) {
            slot = &gpuResults[0];
        }
        slot->owner = readback.owner;
        slot->visible = static_cast<int>(visible);
        if (ms >= 0.0f) {
            slot->ms = ms;
        }
    }
#endif
}

Renderer::OptimizationState::GpuResult
Renderer::OptimizationState::latestGpuResult(const RenderStats* owner) const {
    for (const GpuResult& result : gpuResults) {
        if (result.owner == owner && owner != nullptr) {
            return result;
        }
    }
    return GpuResult{};
}

// ---------------------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------------------

void Renderer::OptimizationState::releaseGpuResources() {
#if !MODULARITY_OPENGL_ES
    MultiDrawState& md = multiDraw;
    ShadowBatchState& sb = shadowBatch;
    GLuint buffers[] = {md.arenaBuffer, md.instanceBuffer, md.templateBuffer, md.boundsBuffer,
                        md.commandBuffer, md.cpuCommandBuffer, md.counterBuffer,
                        sb.instanceBuffer, sb.commandBuffer};
    for (GLuint buffer : buffers) {
        if (buffer) glDeleteBuffers(1, &buffer);
    }
    if (md.vao) glDeleteVertexArrays(1, &md.vao);
    if (sb.vao) glDeleteVertexArrays(1, &sb.vao);
    for (GpuReadback& readback : readbacks) {
        if (readback.fence) glDeleteSync(readback.fence);
        if (readback.buffer) glDeleteBuffers(1, &readback.buffer);
        if (readback.queries[0]) glDeleteQueries(2, readback.queries);
        readback = GpuReadback{};
    }
    for (HiZPyramid& pyramid : hiZ) {
        if (pyramid.texture) glDeleteTextures(1, &pyramid.texture);
    }
#endif
    hiZ.clear();
    multiDraw = MultiDrawState{};
    shadowBatch = ShadowBatchState{};
    gpuResults = {};
}
