#include "../EditorLocalization.h"
#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Loc = Modularity::Loc;

namespace {

constexpr const char* kUnwiredMarker = " *";
constexpr const char* kUnwiredTooltip =
    "Stored with the bake settings, but not consumed by Nebula yet.\n"
    "Changing it will not alter the baked result.";

void unwiredTooltip() {
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", kUnwiredTooltip);
    }
}

void fieldLabel(const char* text, bool unwired = false) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    if (unwired) {
        ImGui::TextUnformatted(text);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextDisabled("%s", kUnwiredMarker);
        unwiredTooltip();
    } else {
        ImGui::TextUnformatted(text);
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

std::string formatByteSize(uint64_t bytes) {
    char buffer[64];
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.2f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

bool isLightmapCandidate(const SceneObject& obj) {
    if (!IsObjectEnabledInHierarchy(obj)) return false;
    if (!HasRendererComponent(obj)) return false;
    if (obj.hasUI || obj.hasCamera || obj.hasPostFX) return false;
    if (obj.hasRigidbody || obj.hasRigidbody2D || obj.hasPlayerController) return false;
    if (obj.hasAnimation || obj.hasSkeletalAnimation) return false;
    if (obj.faceCamera) return false;

    switch (obj.renderType) {
        case RenderType::Cube:
        case RenderType::Sphere:
        case RenderType::Capsule:
        case RenderType::Plane:
        case RenderType::Torus:
        case RenderType::OBJMesh:
        case RenderType::Model:
            return true;
        default:
            return false;
    }
}

float worldTriangleArea(const std::vector<float>& stream, const glm::mat4& model) {
    float area = 0.0f;
    const size_t stride = 8;
    const size_t triStride = stride * 3;
    for (size_t i = 0; i + triStride - 1 < stream.size(); i += triStride) {
        const glm::vec3 a(glm::vec3(model * glm::vec4(stream[i + 0], stream[i + 1],
                                                      stream[i + 2], 1.0f)));
        const glm::vec3 b(glm::vec3(model * glm::vec4(stream[i + 8], stream[i + 9],
                                                      stream[i + 10], 1.0f)));
        const glm::vec3 c(glm::vec3(model * glm::vec4(stream[i + 16], stream[i + 17],
                                                      stream[i + 18], 1.0f)));
        area += 0.5f * glm::length(glm::cross(b - a, c - a));
    }
    return area;
}

int lightKindFor(const SceneObject& obj) {
    switch (obj.light.type) {
        case LightType::Directional: return 0;
        case LightType::Point: return 1;
        case LightType::Spot: return 2;
        case LightType::Area: return 3;
    }
    return 1;
}

}  // namespace

Nebula::BakeSceneData Engine::collectLightmapBakeScene() const {
    Nebula::BakeSceneData scene;

    glm::vec3 boundsMin(FLT_MAX);
    glm::vec3 boundsMax(-FLT_MAX);
    bool hasBounds = false;

    for (const SceneObject& obj : sceneObjects) {
        if (!isLightmapCandidate(obj)) continue;
        if (lightmapping.excludedObjectIds.count(obj.id) != 0) continue;
        if (lightmapping.bakeSelectedOnly &&
            std::find(selectedObjectIds.begin(), selectedObjectIds.end(), obj.id) ==
                selectedObjectIds.end()) {
            continue;
        }

        const std::vector<float>* stream = GetObjectTriangleVertexStream(obj);
        if (!stream || stream->size() < 24) {
            continue;
        }

        Nebula::BakeMeshData mesh;
        mesh.sceneObjectId = obj.id;
        mesh.name = obj.name;
        mesh.worldTransform = ComposeTransform(obj.position, obj.rotation, obj.scale);

        const size_t vertexCount = stream->size() / 8;
        mesh.positions.reserve(vertexCount * 3);
        mesh.normals.reserve(vertexCount * 3);
        mesh.uvs.reserve(vertexCount * 2);
        mesh.indices.reserve(vertexCount);

        for (size_t i = 0; i + 7 < stream->size(); i += 8) {
            mesh.positions.push_back((*stream)[i + 0]);
            mesh.positions.push_back((*stream)[i + 1]);
            mesh.positions.push_back((*stream)[i + 2]);
            mesh.normals.push_back((*stream)[i + 3]);
            mesh.normals.push_back((*stream)[i + 4]);
            mesh.normals.push_back((*stream)[i + 5]);
            mesh.uvs.push_back((*stream)[i + 6]);
            mesh.uvs.push_back((*stream)[i + 7]);
        }

        const uint32_t triVertexCount = static_cast<uint32_t>(mesh.positions.size() / 3);
        const uint32_t usableVertices = triVertexCount - (triVertexCount % 3);
        for (uint32_t i = 0; i < usableVertices; ++i) {
            mesh.indices.push_back(i);
        }
        if (mesh.indices.size() < 3) continue;

        Nebula::BakeMaterialData material;
        material.albedo = glm::vec4(obj.material.color, obj.material.alpha);
        material.roughness = std::clamp(1.0f - obj.material.shininess / 128.0f, 0.02f, 1.0f);
        material.metallic = 0.0f;
        material.emissive = glm::vec3(0.0f);
        material.castsShadows = true;
        mesh.materialId = static_cast<uint32_t>(scene.materials.size());
        scene.materials.push_back(material);

        scene.surfaceArea += worldTriangleArea(*stream, mesh.worldTransform);

        for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
            const glm::vec4 world = mesh.worldTransform * glm::vec4(mesh.positions[i + 0],
                                                                    mesh.positions[i + 1],
                                                                    mesh.positions[i + 2], 1.0f);
            boundsMin = glm::min(boundsMin, glm::vec3(world));
            boundsMax = glm::max(boundsMax, glm::vec3(world));
            hasBounds = true;
        }

        scene.meshes.push_back(std::move(mesh));
    }

    for (const SceneObject& obj : sceneObjects) {
        if (!obj.hasLight || !obj.light.enabled) continue;
        if (!IsObjectEnabledInHierarchy(obj)) continue;

        Nebula::BakeLightData light;
        light.kind = lightKindFor(obj);
        light.position = obj.position;
        light.color = obj.light.color;
        light.intensity = obj.light.intensity;
        light.range = obj.light.range;
        light.innerAngleRadians = glm::radians(obj.light.innerAngle);
        light.outerAngleRadians = glm::radians(obj.light.outerAngle);
        light.halfExtents = obj.light.size * 0.5f;
        light.castsShadows = obj.light.castShadows;

        const glm::mat4 orientation = ComposeTransform(glm::vec3(0.0f), obj.rotation,
                                                       glm::vec3(1.0f));
        light.direction = glm::normalize(glm::vec3(orientation * glm::vec4(0.0f, -1.0f, 0.0f, 0.0f)));
        light.right = glm::normalize(glm::vec3(orientation * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)));
        light.up = glm::normalize(glm::vec3(orientation * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));

        scene.lights.push_back(light);
    }

    if (hasBounds) {
        scene.boundsMin = boundsMin;
        scene.boundsMax = boundsMax;
    }
    return scene;
}

void Engine::refreshLightmapStats() {
    lightmapping.statsMeshCount = 0;
    lightmapping.statsTriangleCount = 0;
    lightmapping.statsSurfaceArea = 0.0f;

    glm::vec3 boundsMin(FLT_MAX);
    glm::vec3 boundsMax(-FLT_MAX);
    bool hasBounds = false;

    for (const SceneObject& obj : sceneObjects) {
        if (!isLightmapCandidate(obj)) continue;
        if (lightmapping.excludedObjectIds.count(obj.id) != 0) continue;
        if (lightmapping.bakeSelectedOnly &&
            std::find(selectedObjectIds.begin(), selectedObjectIds.end(), obj.id) ==
                selectedObjectIds.end()) {
            continue;
        }

        const std::vector<float>* stream = GetObjectTriangleVertexStream(obj);
        if (!stream || stream->size() < 24) continue;

        ++lightmapping.statsMeshCount;
        lightmapping.statsTriangleCount += (stream->size() / 8) / 3;

        const glm::mat4 model = ComposeTransform(obj.position, obj.rotation, obj.scale);
        lightmapping.statsSurfaceArea += worldTriangleArea(*stream, model);

        glm::vec3 localMin(FLT_MAX);
        glm::vec3 localMax(-FLT_MAX);
        for (size_t i = 0; i + 7 < stream->size(); i += 8) {
            const glm::vec3 p((*stream)[i + 0], (*stream)[i + 1], (*stream)[i + 2]);
            localMin = glm::min(localMin, p);
            localMax = glm::max(localMax, p);
        }

        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 p((corner & 1) ? localMax.x : localMin.x,
                              (corner & 2) ? localMax.y : localMin.y,
                              (corner & 4) ? localMax.z : localMin.z);
            const glm::vec3 world = glm::vec3(model * glm::vec4(p, 1.0f));
            boundsMin = glm::min(boundsMin, world);
            boundsMax = glm::max(boundsMax, world);
            hasBounds = true;
        }
    }

    lightmapping.statsBoundsMin = hasBounds ? boundsMin : glm::vec3(0.0f);
    lightmapping.statsBoundsMax = hasBounds ? boundsMax : glm::vec3(0.0f);
    lightmapping.statsRefreshTime = glfwGetTime();
}

void Engine::releaseLightmapPreviewTexture() {
    if (lightmapping.previewTexture != 0) {
        glDeleteTextures(1, &lightmapping.previewTexture);
        lightmapping.previewTexture = 0;
    }
    lightmapping.previewWidth = 0;
    lightmapping.previewHeight = 0;
}

void Engine::startLightmapBake() {
    Nebula::LightmapBaker& baker = Nebula::LightmapBaker::instance();

    lightmapping.lastBakeError.clear();

    const Nebula::BakeSceneData scene = collectLightmapBakeScene();

    const Project& project = projectManager.currentProject;
    const fs::path outputDir =
        project.isLoaded ? (project.projectPath / "Baked") : fs::path("Baked");
    const std::string sceneStem =
        project.currentSceneName.empty() ? std::string("Untitled") : project.currentSceneName;
    const std::string outputPath = (outputDir / (sceneStem + ".lightmap.nebula")).string();

    std::string error;
    if (!baker.start(scene, lightmapping.settings, outputPath, error)) {
        lightmapping.lastBakeError = error;
        addConsoleMessage("Lightmap bake failed to start: " + error, ConsoleMessageType::Error);
        showEditorToast("Lightmap bake failed to start", ConsoleMessageType::Error, 3.0);
        return;
    }

    releaseLightmapPreviewTexture();
    lightmapping.pendingOutputPath = outputPath;
    lightmapping.lastBakeTriangles = scene.triangleCount();
    lightmapping.bakeStartTime = glfwGetTime();
    lightmapping.lastBakeSummary.clear();

    addConsoleMessage("Baking lightmaps: " + std::to_string(scene.meshes.size()) +
                          " meshes, " + std::to_string(scene.triangleCount()) +
                          " triangles, " + std::to_string(scene.lights.size()) + " lights",
                      ConsoleMessageType::Info);
    showEditorProgressToast("Baking Lightmaps", "Preparing bake", -1.0f);
}

void Engine::updateLightmapBake() {
    Nebula::LightmapBaker& baker = Nebula::LightmapBaker::instance();
    const Nebula::BakeStatus previous = baker.progress().status;
    if (previous == Nebula::BakeStatus::Idle) {
        return;
    }

    baker.update();
    const Nebula::BakeProgress& progress = baker.progress();

    if (progress.status == Nebula::BakeStatus::Running) {
        std::string detail = progress.message;
        if (progress.totalSteps > 0) {
            detail += "  (" + std::to_string(progress.step + 1) + "/" +
                      std::to_string(progress.totalSteps) + ")";
        }
        showEditorProgressToast("Baking Lightmaps", detail, progress.fraction);
        return;
    }

    if (previous != Nebula::BakeStatus::Running) {
        return;
    }

    lightmapping.lastBakeSeconds = glfwGetTime() - lightmapping.bakeStartTime;

    if (progress.status == Nebula::BakeStatus::Done) {
        std::string writeError;
        if (baker.writeResult(writeError)) {
            char summary[256];
            std::snprintf(summary, sizeof(summary), "%ux%u atlas, %zu triangles, %.1fs",
                          baker.resultWidth(), baker.resultHeight(),
                          lightmapping.lastBakeTriangles, lightmapping.lastBakeSeconds);
            lightmapping.lastBakeSummary = summary;
            lightmapping.previewDirty = true;
            lightmapping.requestedTab = 2;

            addConsoleMessage("Lightmap bake complete: " + std::string(summary) + " -> " +
                                  lightmapping.pendingOutputPath,
                              ConsoleMessageType::Success);
            finishEditorProgressToast("Lightmap bake complete", ConsoleMessageType::Success);
        } else {
            lightmapping.lastBakeError = writeError;
            addConsoleMessage("Lightmap bake finished but could not be written: " + writeError,
                              ConsoleMessageType::Error);
            finishEditorProgressToast("Lightmap could not be saved", ConsoleMessageType::Error, 3.5);
        }
    } else if (progress.status == Nebula::BakeStatus::Cancelled) {
        addConsoleMessage("Lightmap bake cancelled", ConsoleMessageType::Warning);
        finishEditorProgressToast("Lightmap bake cancelled", ConsoleMessageType::Warning);
    } else {
        lightmapping.lastBakeError = progress.message;
        addConsoleMessage("Lightmap bake failed: " + progress.message, ConsoleMessageType::Error);
        finishEditorProgressToast("Lightmap bake failed", ConsoleMessageType::Error, 3.5);
    }
}

void Engine::renderLightmappingWindow() {
    Nebula::LightmapBaker& baker = Nebula::LightmapBaker::instance();
    Nebula::LightmapSettings& settings = lightmapping.settings;

    ImGui::SetNextWindowSize(ImVec2(380.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Loc::Window("WINDOW_LIGHTMAPPING", "Lightmapping"),
                      &showLightmappingWindow)) {
        ImGui::End();
        return;
    }

    std::string unavailableReason;
    const bool nebulaReady = baker.available(&unavailableReason);
    const bool baking = baker.isRunning();

    if (ImGui::BeginTabBar("##LightmappingTabs", ImGuiTabBarFlags_None)) {
        const char* tabNames[3] = {"Object", "Bake", "Maps"};
        for (int i = 0; i < 3; ++i) {
            ImGuiTabItemFlags flags = (lightmapping.requestedTab == i)
                                          ? ImGuiTabItemFlags_SetSelected
                                          : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(tabNames[i], nullptr, flags)) {
                lightmapping.activeTab = i;
                ImGui::EndTabItem();
            }
        }
        lightmapping.requestedTab = -1;
        ImGui::EndTabBar();
    }
    ImGui::Separator();

    if (!nebulaReady) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.35f, 1.0f));
        ImGui::TextWrapped("Nebula baking backend unavailable");
        ImGui::PopStyleColor();
        ImGui::TextWrapped("%s", unavailableReason.c_str());
        ImGui::Spacing();
        ImGui::Separator();
    }

    const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 3.4f;
    ImGui::BeginChild("##LightmappingBody", ImVec2(0.0f, -footerHeight), false);

    const ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit;

    if (lightmapping.activeTab == 0) {
        ImGui::TextDisabled("Geometry included in the next bake");
        ImGui::Spacing();

        ImGui::Checkbox("Bake selected objects only", &lightmapping.bakeSelectedOnly);

        size_t candidateCount = 0;
        size_t includedCount = 0;
        for (const SceneObject& obj : sceneObjects) {
            if (!isLightmapCandidate(obj)) continue;
            ++candidateCount;
            if (lightmapping.excludedObjectIds.count(obj.id) == 0) ++includedCount;
        }

        ImGui::Text("%zu of %zu static renderers included", includedCount, candidateCount);
        ImGui::Spacing();

        if (ImGui::SmallButton("Include All")) {
            lightmapping.excludedObjectIds.clear();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Exclude All")) {
            for (const SceneObject& obj : sceneObjects) {
                if (isLightmapCandidate(obj)) lightmapping.excludedObjectIds.insert(obj.id);
            }
        }

        ImGui::Separator();

        if (candidateCount == 0) {
            ImGui::TextWrapped(
                "No static renderers in this scene. Objects are eligible when they are "
                "enabled, have a mesh renderer, and are not driven by physics, animation "
                "or camera facing.");
        }

        for (const SceneObject& obj : sceneObjects) {
            if (!isLightmapCandidate(obj)) continue;

            bool included = lightmapping.excludedObjectIds.count(obj.id) == 0;
            ImGui::PushID(obj.id);
            if (ImGui::Checkbox("##include", &included)) {
                if (included) {
                    lightmapping.excludedObjectIds.erase(obj.id);
                } else {
                    lightmapping.excludedObjectIds.insert(obj.id);
                }
            }
            ImGui::SameLine();

            const bool isSelected =
                std::find(selectedObjectIds.begin(), selectedObjectIds.end(), obj.id) !=
                selectedObjectIds.end();
            if (ImGui::Selectable(obj.name.c_str(), isSelected)) {
                setPrimarySelection(obj.id, ImGui::GetIO().KeyCtrl);
            }
            ImGui::PopID();
        }
    } else if (lightmapping.activeTab == 1) {
        ImGui::BeginDisabled(baking);
        if (ImGui::BeginTable("##BakeSettings", 2, tableFlags)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

            fieldLabel("Mode", true);
            int mode = static_cast<int>(settings.mode);
            const char* modeNames[] = {"Single Lightmaps", "Dual Lightmaps", "Directional"};
            if (ImGui::Combo("##Mode", &mode, modeNames, IM_ARRAYSIZE(modeNames))) {
                settings.mode = static_cast<Nebula::LightmapMode>(mode);
            }

            fieldLabel("Use in forward rend.", true);
            ImGui::Checkbox("##UseInForward", &settings.useInForwardRendering);

            fieldLabel("Quality");
            int quality = static_cast<int>(settings.quality);
            const char* qualityNames[] = {"Low", "Medium", "High"};
            if (ImGui::Combo("##Quality", &quality, qualityNames, IM_ARRAYSIZE(qualityNames))) {
                settings.quality = static_cast<Nebula::LightmapQuality>(quality);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Scales Final Gather Rays into Nebula's samples per texel.");
            }

            fieldLabel("Bounces");
            ImGui::SliderInt("##Bounces", &settings.bounces, 0, 4);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Indirect bounces. 0 bakes direct lighting only.");
            }

            fieldLabel("Sky Light Color");
            ImGui::ColorEdit3("##SkyLightColor", &settings.skyLightColor.x,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);

            fieldLabel("Sky Light Intensity");
            ImGui::DragFloat("##SkyLightIntensity", &settings.skyLightIntensity, 0.01f, 0.0f,
                             32.0f, "%.2f");

            fieldLabel("Bounce Boost", true);
            ImGui::SliderFloat("##BounceBoost", &settings.bounceBoost, 0.0f, 4.0f, "%.2f");
            unwiredTooltip();

            fieldLabel("Bounce Intensity", true);
            ImGui::SliderFloat("##BounceIntensity", &settings.bounceIntensity, 0.0f, 4.0f, "%.2f");
            unwiredTooltip();

            fieldLabel("Final Gather Rays");
            ImGui::DragInt("##FinalGatherRays", &settings.finalGatherRays, 8.0f, 1, 8192);

            fieldLabel("Contrast Threshold", true);
            ImGui::SliderFloat("##ContrastThreshold", &settings.contrastThreshold, 0.0f, 1.0f,
                               "%.3f");
            unwiredTooltip();

            fieldLabel("Interpolation");
            ImGui::SliderFloat("##Interpolation", &settings.interpolation, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Any non-zero value enables Nebula's spatial denoise pass.");
            }

            fieldLabel("Interpolation Points");
            ImGui::SliderInt("##InterpolationPoints", &settings.interpolationPoints, 1, 64);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Sample count used to approximate area lights.");
            }

            fieldLabel("Ambient Occlusion", true);
            ImGui::SliderFloat("##AmbientOcclusion", &settings.ambientOcclusion, 0.0f, 1.0f,
                               "%.2f");
            unwiredTooltip();

            fieldLabel("LOD Surface Distance", true);
            ImGui::DragFloat("##LodSurfaceDistance", &settings.lodSurfaceDistance, 0.05f, 0.0f,
                             100.0f, "%.2f");
            unwiredTooltip();

            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginTable("##AtlasSettings", 2, tableFlags)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

            fieldLabel("Lock Atlas");
            ImGui::Checkbox("##LockAtlas", &settings.lockAtlas);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Pin the atlas size instead of deriving it from scene extent.");
            }

            if (settings.lockAtlas) {
                fieldLabel("Atlas Size");
                ImGui::DragInt("##AtlasSize", &settings.lockedAtlasSize, 32.0f, 64, 8192);
            } else {
                fieldLabel("Resolution");
                ImGui::DragFloat("##Resolution", &settings.resolution, 0.5f, 0.1f, 4096.0f,
                                 "%.1f texels per world unit");
            }

            fieldLabel("Padding", true);
            ImGui::DragFloat("##Padding", &settings.padding, 0.1f, 0.0f, 64.0f, "%.1f texels");
            unwiredTooltip();

            ImGui::EndTable();
        }
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::TextDisabled("* stored with the scene, not consumed by Nebula yet");
    } else {
        if (!baker.hasResult()) {
            ImGui::TextWrapped("No lightmaps have been baked in this session.");
            ImGui::Spacing();
            ImGui::TextDisabled(
                "Bake the scene from the Bake tab; the resulting atlas appears here and is "
                "written next to the project as a .nebula file.");
        } else {
            if (lightmapping.previewDirty) {
                const int previewSide = 256;
                std::vector<unsigned char> pixels;
                if (baker.copyPreviewRGBA8(pixels, previewSide, previewSide,
                                           lightmapping.previewExposure)) {
                    if (lightmapping.previewTexture == 0) {
                        glGenTextures(1, &lightmapping.previewTexture);
                    }
                    glBindTexture(GL_TEXTURE_2D, lightmapping.previewTexture);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, previewSide, previewSide, 0, GL_RGBA,
                                 GL_UNSIGNED_BYTE, pixels.data());
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    lightmapping.previewWidth = previewSide;
                    lightmapping.previewHeight = previewSide;
                }
                lightmapping.previewDirty = false;
            }

            ImGui::Text("Atlas: %u x %u", baker.resultWidth(), baker.resultHeight());
            ImGui::TextDisabled("%s", lightmapping.pendingOutputPath.c_str());
            ImGui::Spacing();

            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderFloat("Exposure", &lightmapping.previewExposure, 0.05f, 8.0f,
                                   "%.2f")) {
                lightmapping.previewDirty = true;
            }
            ImGui::Spacing();

            if (lightmapping.previewTexture != 0) {
                const float available = ImGui::GetContentRegionAvail().x;
                const float side = std::max(64.0f, std::min(available, 320.0f));
                ImGui::Image(static_cast<ImTextureID>(lightmapping.previewTexture),
                             ImVec2(side, side));
            } else {
                ImGui::TextDisabled("Preview could not be generated.");
            }
        }
    }

    ImGui::EndChild();

    ImGui::Separator();

    if (!baking) {
        const double now = glfwGetTime();
        if (lightmapping.statsRefreshTime < 0.0 || now - lightmapping.statsRefreshTime > 0.25) {
            refreshLightmapStats();
        }
    }
    const uint32_t atlasSize =
        baking ? baker.resultWidth()
               : Nebula::LightmapBaker::resolveAtlasSize(settings, lightmapping.statsSurfaceArea);

    const float buttonWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) *
                              0.5f;

    ImGui::BeginDisabled(baking);
    if (ImGui::Button("Clear", ImVec2(buttonWidth, 0.0f))) {
        baker.reset();
        releaseLightmapPreviewTexture();
        lightmapping.lastBakeSummary.clear();
        lightmapping.lastBakeError.clear();
        lightmapping.pendingOutputPath.clear();
        addConsoleMessage("Cleared baked lightmaps", ConsoleMessageType::Info);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    if (baking) {
        if (ImGui::Button("Cancel Bake", ImVec2(buttonWidth, 0.0f))) {
            baker.cancel();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Nebula has no mid-pass interruption point, so the current GPU pass "
                "finishes before the bake stops and its result is discarded.");
        }
    } else {
        ImGui::BeginDisabled(!nebulaReady || lightmapping.statsMeshCount == 0);
        if (ImGui::Button("Bake Scene", ImVec2(buttonWidth, 0.0f))) {
            startLightmapBake();
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing();

    if (baking) {
        const Nebula::BakeProgress& progress = baker.progress();
        if (progress.fraction >= 0.0f) {
            ImGui::ProgressBar(progress.fraction, ImVec2(-FLT_MIN, 0.0f));
        } else {
            ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()),
                               ImVec2(-FLT_MIN, 0.0f), "Working");
        }
        ImGui::TextDisabled("%s", progress.message.c_str());
    } else if (!lightmapping.lastBakeError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.48f, 0.48f, 1.0f));
        ImGui::TextWrapped("%s", lightmapping.lastBakeError.c_str());
        ImGui::PopStyleColor();
    } else if (baker.hasResult()) {
        const uint64_t bytes = static_cast<uint64_t>(baker.resultWidth()) *
                               baker.resultHeight() * 4ull * 4ull;
        ImGui::Text("1 lightmap%s%s", "   ", formatByteSize(bytes).c_str());
        ImGui::TextDisabled("%s", lightmapping.lastBakeSummary.c_str());
    } else {
        ImGui::Text("0 lightmaps   0 B");
        ImGui::TextDisabled("%zu triangles, %.0f u^2  |  atlas would be %u x %u",
                            lightmapping.statsTriangleCount, lightmapping.statsSurfaceArea,
                            atlasSize, atlasSize);
    }

    ImGui::End();
}
