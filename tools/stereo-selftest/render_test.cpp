#include "Engine.h"
#include "XR/XRSwapchainGL.h"
#include "UnityAssetImport.h"
#include "Platform/AssetSource.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <cfloat>
#include <fstream>

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
#include "swapchain_test.h"
static int W = 320, H = 240;
static PFNGLDRAWARRAYSPROC realDraw;
static PFNGLMULTIDRAWARRAYSINDIRECTPROC realMultiDraw;
static PFNGLDRAWARRAYSINSTANCEDPROC realInstancedDraw;
static int geometrySubmissions = 0;
static std::vector<std::vector<float>> surfaceCaptures, depthCaptures;
static void APIENTRY CountDraw(GLenum mode, GLint first, GLsizei count) {
    if (count >= 30) ++geometrySubmissions;
    GLint program = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (program && glGetUniformLocation(program, "sceneSurface") >= 0) {
        GLint unit = GL_TEXTURE0; glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
        surfaceCaptures.emplace_back(W * H * 4);
        glActiveTexture(GL_TEXTURE12);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, surfaceCaptures.back().data());
        depthCaptures.emplace_back(W * H);
        glActiveTexture(GL_TEXTURE1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depthCaptures.back().data());
        glActiveTexture(unit);
    }
    realDraw(mode, first, count);
}
static void APIENTRY CountMultiDraw(GLenum mode, const void* commands, GLsizei count, GLsizei stride) {
    ++geometrySubmissions;
    realMultiDraw(mode, commands, count, stride);
}
static void APIENTRY CountInstanced(GLenum mode, GLint first, GLsizei vertices, GLsizei instances) {
    ++geometrySubmissions;
    realInstancedDraw(mode, first, vertices, instances);
}
struct Target {
    GLuint fbo = 0, color = 0;
    Target() {
        glGenTextures(1, &color); glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, W, H, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Output target incomplete");
    }
    ~Target() { glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &color); }
    std::vector<float> pixels() const {
        std::vector<float> data(W * H * 4);
        glBindTexture(GL_TEXTURE_2D, color);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, data.data());
        for (float value : data) Require(std::isfinite(value), "Non-finite eye image");
        return data;
    }
};
static double Difference(const std::vector<float>& a, const std::vector<float>& b) {
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i) sum += std::abs(a[i] - b[i]);
    return sum / a.size();
}
static void Save(const char* label, const std::vector<float>& data) {
    std::ofstream out(std::string("build/stereo-selftest/") + label + ".ppm", std::ios::binary);
    out << "P6\n" << W << ' ' << H << "\n255\n";
    for (int y = H - 1; y >= 0; --y) for (int x = 0; x < W; ++x) for (int c = 0; c < 3; ++c) {
        uint8_t v = uint8_t(std::clamp(data[(y * W + x) * 4 + c], 0.f, 1.f) * 255);
        out.write(reinterpret_cast<char*>(&v), 1);
    }
}
static SceneObject Cube(int id, glm::vec3 position, glm::vec3 scale, glm::vec3 color) {
    SceneObject object("Stereo fixture", ObjectType::Cube, id);
    object.hasRenderer = true; object.renderType = RenderType::Cube;
    object.position = position; object.scale = scale;
    object.materialPath = "stereo-fixture.mat";
    object.material.color = color; object.material.pbrEnabled = true;
    object.material.metallic = .25f; object.material.roughness = .3f;
    return object;
}
int main(int argc, char** argv) {
    try {
        if (const char* w = std::getenv("MODULARITY_STEREO_WIDTH")) W = std::clamp(std::atoi(w), 64, 4096);
        if (const char* h = std::getenv("MODULARITY_STEREO_HEIGHT")) H = std::clamp(std::atoi(h), 64, 4096);
        Window window; auto* handle = window.makeWindow(Modularity::GraphicsBackend::OpenGL);
        Require(handle, "GL context"); glfwHideWindow(handle);
        Require(Modularity::XR::XRMultiviewSupported(), "GL_OVR_multiview2 unavailable on test GPU");
        std::cout << glGetString(GL_RENDERER) << '\n';
#if MODULARITY_HAS_OPENXR && MODULARITY_XR_HAS_ANY_GL_TYPES
        StereoSwapchainTest::Run();
#endif
        {
            Renderer renderer; renderer.initialize(); renderer.resize(W, H);
            renderer.setAmbientColor(glm::vec3(.6f));
            auto sky = renderer.getSkybox()->getSettings();
            sky.ssrEnabled = false; sky.environmentReflections = false;
            renderer.getSkybox()->setSettings(sky);
            Camera eyes[2]; glm::mat4 projections[2];
            for (int i = 0; i < 2; ++i) {
                eyes[i].position = glm::vec3(i == 0 ? -.12f : .12f, 1.2f, 3);
                eyes[i].front = glm::normalize(glm::vec3(.02f, -.1f, -1));
                eyes[i].solidBackground = true; eyes[i].backgroundColor = glm::vec3(.04f, .07f, .1f);
                projections[i] = BuildCameraProjection(eyes[i], W, H, 74, .1f, 35);
                projections[i][2][0] = i == 0 ? -.13f : .09f;
            }
            std::vector<SceneObject> scene{
                Cube(1, {0, -.15f, -1}, {12, .2f, 15}, {.3f, .3f, .3f}),
                Cube(2, {-.6f, .8f, -.7f}, {.5f, 1.6f, .5f}, {1, .05f, .01f}),
                Cube(3, {.9f, .5f, -2}, {.65f, 1, .6f}, {.02f, .9f, .1f}),
                Cube(4, {3.8f, .7f, -.8f}, {.2f, 1.4f, .5f}, {.02f, .1f, 1})};
            SceneObject canvas("World UI", ObjectType::Canvas, 8);
            canvas.hasRenderer = true; canvas.renderType = RenderType::Plane;
            canvas.hasUI = true; canvas.ui.type = UIElementType::Canvas;
            canvas.ui.renderIn3D = true; canvas.position = {0, 1.25f, 0};
            canvas.scale = {.6f, .6f, 1}; canvas.materialPath = "stereo-fixture.mat";
            scene.push_back(canvas);
            auto info = renderer.ensureUiTarget(8, 32, 32, 0, MaterialProperties::TextureFilter::Bilinear, false);
            std::vector<uint8_t> texels(32 * 32 * 4);
            for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
                const int n = (y * 32 + x) * 4;
                texels[n] = 240; texels[n + 1] = 18; texels[n + 2] = 220;
                texels[n + 3] = x > 4 && x < 27 && y > 4 && y < 27 ? 255 : 0;
            }
            glBindTexture(GL_TEXTURE_2D, info.texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
            renderer.markUiTargetRendered(8);
            Target mono[2], stereo[2];
            realDraw = glad_glDrawArrays; glad_glDrawArrays = CountDraw;
            realMultiDraw = glad_glMultiDrawArraysIndirect; glad_glMultiDrawArraysIndirect = CountMultiDraw;
            realInstancedDraw = glad_glDrawArraysInstanced; glad_glDrawArraysInstanced = CountInstanced;
            GLuint targets[2] = {stereo[0].fbo, stereo[1].fbo};
            uint64_t serial = 1;
            // Late sky must retain opaque depth rejection and sit behind UI.
            const auto originalSky = sky;
            sky.cloudCoverage = 0; sky.cloudWind = 0; renderer.getSkybox()->setSettings(sky);
            for (auto& eye : eyes) eye.solidBackground = false;
            for (int i = 0; i < 2; ++i) renderer.renderSceneToTarget(eyes[i], scene, mono[i].fbo,
                W, H, projections[i], .1f, 35, true, true, i);
            Require(renderer.renderStereoSceneToTargets(eyes, scene, targets, W, H, projections, .1f, 35), "Sky stereo fallback");
            for (int i = 0; i < 2; ++i) {
                const double error = Difference(mono[i].pixels(), stereo[i].pixels());
                std::cout << "Sky eye " << i << " mean error=" << error << '\n';
                Require(error < .00005, "Late stereo sky/UI differs from multipass");
            }
            Require(glGetError() == GL_NO_ERROR, "Late stereo sky GL error");
            for (auto& eye : eyes) eye.solidBackground = true;
            sky = originalSky; renderer.getSkybox()->setSettings(sky);
            for (auto precision : {RendererColorPrecision::HDR16F, RendererColorPrecision::SDR8})
            for (bool optimized : {false, true}) for (bool ssr : {false, true}) {
                renderer.setColorPrecision(precision);
                RenderOptimizationSettings opt;
                opt.cpuFrustumCulling = opt.materialSorting = opt.staticObjectCaching = optimized;
                opt.multiDrawIndirect = opt.gpuDrivenCulling = opt.hiZOcclusionCulling = optimized;
                renderer.setOptimizationSettings(opt);
                sky.ssrEnabled = ssr; sky.ssrIntensity = .8f;
                sky.ssrMaxDistance = 12; sky.ssrSteps = 64;
                renderer.getSkybox()->setSettings(sky);
                renderer.setFrameSerial(serial++);
                geometrySubmissions = 0;
                surfaceCaptures.clear(); depthCaptures.clear();
                for (int i = 0; i < 2; ++i) {
                    renderer.beginRender(eyes[i].getViewMatrix(), projections[i], eyes[i].position);
                    renderer.renderSceneToTarget(eyes[i], scene, mono[i].fbo, W, H, projections[i], .1f, 35, true, true, i);
                    renderer.endRender();
                }
                const int monoSubmissions = geometrySubmissions;
                renderer.setFrameSerial(serial++);
                geometrySubmissions = 0;
                renderer.beginRender(eyes[0].getViewMatrix(), projections[0], eyes[0].position);
                Require(renderer.renderStereoSceneToTargets(eyes, scene, targets, W, H, projections, .1f, 35), "Shared stereo rejected standard scene");
                renderer.endRender();
                Require(glGetError() == GL_NO_ERROR, "Stereo render GL error");
                std::cout << "Geometry submissions: multipass=" << monoSubmissions
                          << " shared=" << geometrySubmissions << '\n';
                // A union frustum may submit an edge object that one mono eye
                // culled. Sharing still removes most duplicate submissions.
                Require(geometrySubmissions > 0 && geometrySubmissions * 4 <= monoSubmissions * 3,
                        "Shared stereo did not reduce geometry submissions");
                for (int i = 0; i < 2; ++i) {
                    double difference = Difference(mono[i].pixels(), stereo[i].pixels());
                    std::cout << "Eye " << i << " SDR8=" << (precision == RendererColorPrecision::SDR8)
                              << " optimized=" << optimized << " SSR=" << ssr << " mean error=" << difference << '\n';
                    if (ssr && surfaceCaptures.size() == 4) {
                        std::cout << "Surface error=" << Difference(surfaceCaptures[i], surfaceCaptures[2+i])
                                  << " depth error=" << Difference(depthCaptures[i], depthCaptures[2+i]) << '\n';
                    }
                    if (difference >= .001) {
                        Save("mono", mono[i].pixels()); Save("stereo", stereo[i].pixels());
                        if (surfaceCaptures.size() == 4) { Save("mono-surface", surfaceCaptures[i]); Save("stereo-surface", surfaceCaptures[2+i]); }
                    }
                    Require(difference < .001, "Stereo eye differs from multipass reference");
                }
                Require(Difference(stereo[0].pixels(), stereo[1].pixels()) > .003, "Identical eyes: stereo disparity missing");
                Require(renderer.getOptimizationSettings().hiZOcclusionCulling == optimized, "Stereo modified saved optimization settings");
            }
            // Exercise effects against array-backed 2D views, including separate
            // eye histories. Prime the two reference paths independently.
            if (!std::getenv("MODULARITY_STEREO_BENCHMARK")) {
            renderer.setColorPrecision(RendererColorPrecision::HDR16F);
            const auto drawPair = [&](bool shared) {
                glfwSetTime(double(serial) * .04); renderer.setFrameSerial(serial++);
                if (shared) Require(renderer.renderStereoSceneToTargets(eyes, scene, targets,
                    W, H, projections, .1f, 35), "Effect stereo fallback");
                else for (int i = 0; i < 2; ++i) renderer.renderSceneToTarget(eyes[i], scene,
                    mono[i].fbo, W, H, projections[i], .1f, 35, true, true, i);
                Require(glGetError() == GL_NO_ERROR, "Stereo effects GL error");
            };
            drawPair(false); const auto sharpAOOff = mono[0].pixels();
            SceneObject volume("Stereo effects", ObjectType::PostFXNode, 50);
            volume.hasPostFX = true; volume.postFx.bloomEnabled = false;
            volume.postFx.hdrEnabled = false; volume.postFx.gamma = 1;
            volume.postFx.ambientOcclusionEnabled = true;
            volume.postFx.aoRadius = .0065f; volume.postFx.aoStrength = 1;
            scene.push_back(volume);
            drawPair(false); drawPair(true);
            Require(Difference(sharpAOOff, stereo[0].pixels()) > .00001, "Stereo AO has no effect");
            for (int i = 0; i < 2; ++i) Require(Difference(mono[i].pixels(), stereo[i].pixels()) < .00001,
                "Stereo AO differs from multipass");
            scene.back().postFx.motionBlurEnabled = true;
            scene.back().postFx.motionBlurStrength = 1;
            std::vector<float> movingMono[2], movingShared[2];
            for (bool shared : {false, true}) {
                for (auto& eye : eyes) eye.position.x += 3;
                drawPair(shared);
                for (auto& eye : eyes) eye.position.x -= 3;
                drawPair(shared); drawPair(shared);
                for (auto& eye : eyes) eye.position.x += .14f;
                drawPair(shared);
                Require(renderer.getLastPreviewPostStats().motionBlurUsed, "Stereo motion history missing");
                for (int i = 0; i < 2; ++i) {
                    if (shared) movingShared[i] = stereo[i].pixels();
                    else movingMono[i] = mono[i].pixels();
                }
                for (auto& eye : eyes) eye.position.x -= .14f;
            }
            for (auto& eye : eyes) eye.position.x += .14f;
            scene.back().postFx.motionBlurEnabled = false; drawPair(false);
            Require(Difference(movingMono[0], mono[0].pixels()) > .0001, "Stereo moving image is not blurred");
            drawPair(true);
            for (int i = 0; i < 2; ++i) {
                const double signal = Difference(movingShared[i], stereo[i].pixels());
                const double reference = Difference(movingMono[i], mono[i].pixels());
                const double error = Difference(movingShared[i], movingMono[i]);
                std::cout << "Motion eye " << i << " reference signal=" << reference << " shared signal="
                          << signal << " mean error=" << error << '\n';
                Require(signal > .0001, "Shared moving eye is not blurred");
                // Strength is normalized by wall-clock time between eye renders,
                // so independent reference runs have slightly different strengths.
                Require(error < .00005 + .3 * std::max(signal, reference), "Stereo motion differs from reference");
            }
            for (auto& eye : eyes) eye.position.x -= .14f;
            scene.pop_back();
            std::cout << "PASS: SSAO and independently primed left/right camera-motion blur match multipass\n";
            }

            if (argc > 2) {
                renderer.setColorPrecision(RendererColorPrecision::HDR16F);
                const std::filesystem::path meshPath = std::filesystem::absolute(argv[2]);
                auto project = meshPath.parent_path();
                while (!project.empty() && !std::filesystem::exists(project / "project.modu")) {
                    auto parent = project.parent_path(); if (parent == project) break; project = parent;
                }
                Require(std::filesystem::exists(project / "project.modu"), "Mesh has no project root");
                Modularity::Platform::SetPortableAssetRoot(project.string());
                ModelSceneData data; std::string error;
                Require(getModelLoader().loadModelScene(meshPath.string(), data, error), error.c_str());
                std::vector<UnityAssetImport::ImportedMaterial> materials;
                Require(UnityAssetImport::ResolveMeshMaterials(meshPath.string(), materials, error), error.c_str());
                std::vector<SceneObject> trees;
                glm::vec3 low(FLT_MAX), high(-FLT_MAX);
                for (size_t m = 0; m < data.meshIndices.size(); ++m) {
                    SceneObject tree("Project mesh", ObjectType::Model, int(m + 100));
                    tree.hasRenderer = true; tree.renderType = RenderType::Model;
                    tree.meshId = data.meshIndices[m]; tree.material = materials[m].props;
                    tree.albedoTexturePath = materials[m].albedoPath;
                    tree.normalMapPath = materials[m].normalPath;
                    tree.occlusionMapPath = materials[m].occlusionPath;
                    const auto* info = getModelLoader().getMeshInfo(tree.meshId);
                    Require(info && info->mesh, "Project mesh missing");
                    low = glm::min(low, info->boundsMin); high = glm::max(high, info->boundsMax);
                    trees.push_back(tree);
                }
                const glm::vec3 center = (low + high) * .5f;
                const float radius = glm::length(high - low) * .5f;
                const auto originals = trees;
                for (int copy = 1; copy < 4; ++copy) for (auto tree : originals) {
                    tree.id += copy * 100; tree.position.x += (copy - 1.5f) * radius * .4f;
                    trees.push_back(tree);
                }
                for (int i = 0; i < 2; ++i) {
                    eyes[i].position = center + glm::vec3((i == 0 ? -.015f : .015f) * radius, 0, radius * 3);
                    eyes[i].front = glm::vec3(0, 0, -1);
                    projections[i] = BuildCameraProjection(eyes[i], W, H, 60, .1f, radius * 10);
                    projections[i][2][0] = i == 0 ? -.04f : .04f;
                }
                SceneWindState wind; wind.strength = .45f; wind.time = 1.25f; wind.turbulence = .5f;
                renderer.setSceneWind(wind);
                sky.ssrEnabled = true; renderer.getSkybox()->setSettings(sky);
                renderer.setFrameSerial(serial++);
                geometrySubmissions = 0;
                for (int i = 0; i < 2; ++i) renderer.renderSceneToTarget(eyes[i], trees, mono[i].fbo,
                    W, H, projections[i], .1f, radius * 10, true, true, i);
                const int projectMonoSubmissions = geometrySubmissions;
                renderer.setFrameSerial(serial++);
                geometrySubmissions = 0;
                Require(renderer.renderStereoSceneToTargets(eyes, trees, targets, W, H, projections,
                    .1f, radius * 10), "Shared stereo rejected project mesh variants");
                std::cout << "Project geometry submissions: multipass=" << projectMonoSubmissions
                          << " shared=" << geometrySubmissions << '\n';
                Require(geometrySubmissions * 2 == projectMonoSubmissions, "Project stereo did not halve geometry submissions");
                for (int i = 0; i < 2; ++i) {
                    const double difference = Difference(mono[i].pixels(), stereo[i].pixels());
                    std::cout << "Project mesh eye " << i << " mean error=" << difference << '\n';
                    Require(difference < .001, "Animated instanced project mesh differs from multipass");
                }
                Require(renderer.getLastViewportStats().instancedDrawCalls >= 2, "Project instancing disabled in stereo");
                Require(glGetError() == GL_NO_ERROR, "Project stereo GL error");
                std::cout << "PASS: four project meshes, animated alpha depth, instancing and SSR stereo\n";
                if (std::getenv("MODULARITY_STEREO_BENCHMARK")) {
                    // Disable test readbacks/counters. Both paths include the same
                    // SSR/post chain; glFinish measures complete fixture work.
                    glad_glDrawArrays = realDraw; glad_glMultiDrawArraysIndirect = realMultiDraw;
                    glad_glDrawArraysInstanced = realInstancedDraw;
                    double totals[2]{};
                    for (int frame = 0; frame < 128; ++frame) {
                        // Alternate ordering to reduce clock/thermal bias.
                        for (int order = 0; order < 2; ++order) {
                            const int index = (frame + order) & 1;
                            const bool shared = index == 1;
                            renderer.setFrameSerial(serial++); wind.time = frame / 144.f; renderer.setSceneWind(wind);
                            glFinish(); const auto start = std::chrono::steady_clock::now();
                            if (shared) Require(renderer.renderStereoSceneToTargets(eyes, trees, targets,
                                W, H, projections, .1f, radius * 10), "Benchmark stereo fallback");
                            else for (int i = 0; i < 2; ++i) renderer.renderSceneToTarget(eyes[i], trees,
                                mono[i].fbo, W, H, projections[i], .1f, radius * 10, true, true, i);
                            glFinish();
                            if (frame >= 32) totals[index] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                        }
                    }
                    for (int i = 0; i < 2; ++i) std::cout << "BENCHMARK " << (i ? "shared" : "multipass")
                        << " " << W << 'x' << H << " per eye, four project meshes + SSR, mean ms=" << totals[i] / 96 << '\n';
                }
            }
            // CPU-facing billboards must take the complete per-eye fallback.
            scene[1].renderType = RenderType::Sprite; scene[1].faceCamera = true;
            Require(!renderer.renderStereoSceneToTargets(eyes, scene, targets, W, H, projections, .1f, 35), "CPU billboard did not fall back");
            Require(glGetError() == GL_NO_ERROR, "Stereo cleanup/fallback GL error");
            glad_glDrawArrays = realDraw; glad_glMultiDrawArraysIndirect = realMultiDraw;
            glad_glDrawArraysInstanced = realInstancedDraw;
        }
        Require(glGetError() == GL_NO_ERROR, "Resource cleanup GL error");
        std::cout << "PASS: asymmetric stereo, union culling, MDI/GPU culling, UI transparency, SSR, fallback and resource cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
