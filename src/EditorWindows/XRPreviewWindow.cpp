#include "../EditorLocalization.h"
#include "Engine.h"
#include "Panels/ViewportRenderHelpers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Loc = Modularity::Loc;

#pragma region XR Stereo Preview
void Engine::renderXRPreviewWindow() {
    if (!showXRPreviewWindow) return;

    ImGui::SetNextWindowSize(ImVec2(920.0f, 560.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(Loc::Window("WINDOW_XR_PREVIEW", "Stereo Preview"), &showXRPreviewWindow);

    if (usingVulkan()) {
        ImGui::TextWrapped("The stereo preview renders through the OpenGL path and is not "
                           "available while the Vulkan backend is active.");
        ImGui::End();
        return;
    }
    if (!projectManager.currentProject.isLoaded) {
        ImGui::TextDisabled("Open a project to preview stereo rendering.");
        ImGui::End();
        return;
    }

    const Modularity::XR::ProjectOpenXRSettings& xrSettings =
        projectManager.currentProject.openXRSettings;
    const bool xrEnabledAnywhere = [&]() {
        if (!xrSettings.enabled) return false;
        for (size_t i = 0; i < xrSettings.targets.size(); ++i) {
            if (xrSettings.targets[i].enabled) return true;
        }
        return false;
    }();

    if (!xrEnabledAnywhere) {
        ImGui::TextWrapped("OpenXR is off for every platform target. Turn it on in Project "
                           "Settings to preview what a headset frame costs.");
        ImGui::Separator();
    }

    struct EyePreset {
        const char* name;
        int width;
        int height;
        float defaultHz;
    };
    static const EyePreset kPresets[] = {
        {"Quest 2 (1832x1920)",      1832, 1920, 72.0f},
        {"Quest 3 (2064x2208)",      2064, 2208, 90.0f},
        {"Quest Pro (1800x1920)",    1800, 1920, 90.0f},
        {"Pico 4 (2160x2160)",       2160, 2160, 90.0f},
        {"Valve Index (1440x1600)",  1440, 1600, 120.0f},
        {"Custom",                   1600, 1600, 90.0f},
    };
    const int presetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
    xrPreview.presetIndex = std::clamp(xrPreview.presetIndex, 0, presetCount - 1);

    const bool sessionLive = isXRSessionRunning();
    if (sessionLive) {
        ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.6f, 1.0f),
                           "Live session: using the runtime's own eye poses and projections.");
    }

    if (ImGui::BeginTable("##XRPreviewSettings", 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        auto row = [&](const char* label) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0f);
        };

        ImGui::BeginDisabled(sessionLive);
        row("Headset");
        {
            const char* names[16];
            for (int i = 0; i < presetCount; ++i) names[i] = kPresets[i].name;
            if (ImGui::Combo("##XRPreset", &xrPreview.presetIndex, names, presetCount)) {
                xrPreview.targetHz = kPresets[xrPreview.presetIndex].defaultHz;
            }
        }
        if (xrPreview.presetIndex == presetCount - 1) {
            row("Per-eye size");
            int size[2] = {xrPreview.customWidth, xrPreview.customHeight};
            if (ImGui::DragInt2("##XRCustomSize", size, 8.0f, 256, 8192)) {
                xrPreview.customWidth = std::clamp(size[0], 256, 8192);
                xrPreview.customHeight = std::clamp(size[1], 256, 8192);
            }
        }
        row("Render scale");
        ImGui::SliderFloat("##XRRenderScale", &xrPreview.renderScale, 0.4f, 2.0f, "%.2fx");
        row("IPD (mm)");
        ImGui::SliderFloat("##XRIpd", &xrPreview.ipdMm, 50.0f, 76.0f, "%.1f");
        row("FOV");
        ImGui::SliderFloat("##XRFov", &xrPreview.fovDegrees, 60.0f, 130.0f, "%.0f deg");
        ImGui::EndDisabled();

        row("Target rate");
        {
            const char* hzNames[] = {"72 Hz", "80 Hz", "90 Hz", "120 Hz"};
            const float hzValues[] = {72.0f, 80.0f, 90.0f, 120.0f};
            int hzIndex = 2;
            for (int i = 0; i < 4; ++i) {
                if (std::abs(hzValues[i] - xrPreview.targetHz) < 0.5f) hzIndex = i;
            }
            if (ImGui::Combo("##XRTargetHz", &hzIndex, hzNames, 4)) {
                xrPreview.targetHz = hzValues[std::clamp(hzIndex, 0, 3)];
            }
        }
        row("Render");
        ImGui::Checkbox("##XRPreviewLive", &xrPreview.rendering);
        ImGui::EndTable();
    }

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Rendering the scene twice more per frame is itself expensive.\n"
                          "Untick to freeze the last pair and read the numbers.");
    }

    const EyePreset& preset = kPresets[xrPreview.presetIndex];
    const int baseWidth = (xrPreview.presetIndex == presetCount - 1) ? xrPreview.customWidth
                                                                    : preset.width;
    const int baseHeight = (xrPreview.presetIndex == presetCount - 1) ? xrPreview.customHeight
                                                                     : preset.height;
    int eyeWidth = std::clamp(static_cast<int>(baseWidth * xrPreview.renderScale), 64, 8192);
    int eyeHeight = std::clamp(static_cast<int>(baseHeight * xrPreview.renderScale), 64, 8192);

    if (xrPreview.rendering) {
        renderXRPreviewEyes(eyeWidth, eyeHeight);
    }

    ImGui::Separator();

    const double leftMs = xrPreview.eyeGpuMs[0];
    const double rightMs = xrPreview.eyeGpuMs[1];
    const bool haveTiming = leftMs >= 0.0 && rightMs >= 0.0;
    const double totalMs = haveTiming ? (leftMs + rightMs) : -1.0;
    const double budgetMs = 1000.0 / std::max(1.0f, xrPreview.targetHz);

    static ViewportRenderHelpers::ViewportFpsWindow fpsWindow;
    fpsWindow.accumulate(ImGui::GetIO().DeltaTime);

    const float fps = ImGui::GetIO().Framerate;
    const float frameMs = (fps > 0.0f) ? (1000.0f / fps) : 0.0f;
    const double used = haveTiming ? (totalMs / budgetMs) : 0.0;

    std::vector<std::string> overlay;
    overlay.reserve(8);
    {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "FPS: %.0f (%.1f ms)", fps, frameMs);
        overlay.emplace_back(buf);
        if (fpsWindow.hasResult) {
            std::snprintf(buf, sizeof(buf), "1%% Low: %.0f | Max: %.0f",
                          fpsWindow.onePercentLowFps, fpsWindow.maxFps);
        } else {
            std::snprintf(buf, sizeof(buf), "1%% Low: -- | Max: --");
        }
        overlay.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "Render: %dx%d per eye @ %d%%", eyeWidth, eyeHeight,
                      static_cast<int>(xrPreview.renderScale * 100.0f));
        overlay.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "Batches: %d + %d", xrPreview.eyeBatches[0],
                      xrPreview.eyeBatches[1]);
        overlay.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "Meshes: %d + %d", xrPreview.eyeMeshes[0],
                      xrPreview.eyeMeshes[1]);
        overlay.emplace_back(buf);
        if (haveTiming) {
            std::snprintf(buf, sizeof(buf), "GPU: L %.2f | R %.2f | Both %.2f ms",
                          leftMs, rightMs, totalMs);
            overlay.emplace_back(buf);
            std::snprintf(buf, sizeof(buf), "Budget: %.0f%% of %.2f ms @ %.0f Hz%s",
                          used * 100.0, budgetMs, xrPreview.targetHz,
                          used >= 1.0 ? "  OVER" : "");
            overlay.emplace_back(buf);
        } else {
            overlay.emplace_back("GPU: waiting for timer results...");
        }
        overlay.emplace_back("Scene only: no post FX, no runtime composition");
    }

    if (haveTiming) {
        const ImVec4 tint = used < 0.75 ? ImVec4(0.55f, 0.9f, 0.6f, 1.0f)
                          : used < 1.0  ? ImVec4(0.95f, 0.82f, 0.45f, 1.0f)
                                        : ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
        ImGui::TextColored(tint, "%.2f ms both eyes - %.0f%% of the %.2f ms budget at %.0f Hz%s",
                           totalMs, used * 100.0, budgetMs, xrPreview.targetHz,
                           used >= 1.0 ? "  (over budget)" : "");
        ImGui::ProgressBar(static_cast<float>(std::min(used, 1.0)), ImVec2(-1.0f, 0.0f), "");
    } else {
        ImGui::TextDisabled("Waiting for GPU timer results...");
    }

    ImGui::Spacing();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = 6.0f;
    const float paneWidth = std::max(32.0f, (avail.x - gap) * 0.5f);
    const float aspect = static_cast<float>(eyeHeight) / std::max(1, eyeWidth);
    const float paneHeight = std::max(32.0f, std::min(paneWidth * aspect, avail.y - 4.0f));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (int eye = 0; eye < 2; ++eye) {
        const ImVec2 min(origin.x + eye * (paneWidth + gap), origin.y);
        const ImVec2 max(min.x + paneWidth, min.y + paneHeight);
        drawList->AddRectFilled(min, max, IM_COL32(10, 12, 18, 255));
        if (xrPreview.eyeTexture[eye] != 0) {
            drawList->AddImage((void*)(intptr_t)xrPreview.eyeTexture[eye], min, max,
                               ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        }
        drawList->AddRect(min, max, IM_COL32(70, 76, 92, 255));
        drawList->AddText(ImVec2(min.x + 6.0f, min.y + 4.0f), IM_COL32(226, 232, 240, 220),
                          eye == 0 ? "Left" : "Right");
    }
    ViewportRenderHelpers::DrawViewportProfilerPanel(
        drawList, ImVec2(origin.x + 14.0f, origin.y + 24.0f), overlay);
    ImGui::Dummy(ImVec2(avail.x, paneHeight));
    ImGui::TextDisabled("%d x %d per eye (%.0f%% scale)", eyeWidth, eyeHeight,
                        xrPreview.renderScale * 100.0f);

    ImGui::End();
}
#pragma endregion

bool Engine::isXRSessionRunning() const {
#if MODULARITY_HAS_OPENXR
    return xrSystem.isRunning();
#else
    return false;
#endif
}

void Engine::renderXRPreviewEyes(int eyeWidth, int eyeHeight) {
    if (eyeWidth <= 0 || eyeHeight <= 0 || !rendererInitialized) return;

    Camera baseCamera = camera;
    float nearPlane = 0.05f;
    float farPlane = 1000.0f;
    const SceneObject* runtimeCam = findXRCameraObject();
    if (!runtimeCam) runtimeCam = findPlayerCameraObject();
    if (runtimeCam) {
        baseCamera = makeCameraFromObject(*runtimeCam);
        nearPlane = std::max(0.01f, runtimeCam->camera.nearClip);
        farPlane = std::max(nearPlane + 0.01f, runtimeCam->camera.farClip);
    }

    const bool sessionLive = isXRSessionRunning();
    const float halfIpd = (xrPreview.ipdMm * 0.001f) * 0.5f;
    const float aspect = static_cast<float>(eyeWidth) / static_cast<float>(std::max(1, eyeHeight));

    for (int eye = 0; eye < 2; ++eye) {
        Camera eyeCamera = baseCamera;
        glm::mat4 projection(1.0f);

#if MODULARITY_HAS_OPENXR
        if (sessionLive && static_cast<uint32_t>(eye) < xrSystem.viewCount()) {
            const glm::mat4 viewMatrix = xrSystem.viewMatrix(static_cast<uint32_t>(eye));
            projection = xrSystem.projectionMatrix(static_cast<uint32_t>(eye));
            const glm::mat4 eyeToWorld = glm::inverse(viewMatrix);
            eyeCamera.position = glm::vec3(eyeToWorld[3]);
            eyeCamera.front = glm::normalize(-glm::vec3(eyeToWorld[2]));
            eyeCamera.up = glm::normalize(glm::vec3(eyeToWorld[1]));
        } else
#endif
        {
            const glm::vec3 right =
                glm::normalize(glm::cross(baseCamera.front, baseCamera.up));
            const float offset = (eye == 0) ? -halfIpd : halfIpd;
            eyeCamera.position = baseCamera.position + right * offset;
            projection = glm::perspective(glm::radians(xrPreview.fovDegrees), aspect,
                                          nearPlane, farPlane);
        }

        const Renderer::XREyePreviewResult result = renderer.renderXREyePreview(
            eyeCamera, sceneObjects, eye, eyeWidth, eyeHeight, projection, nearPlane, farPlane);
        xrPreview.eyeTexture[eye] = result.texture;
        xrPreview.eyeGpuMs[eye] = result.gpuMs;
        xrPreview.eyeBatches[eye] = result.stats.drawCalls;
        xrPreview.eyeMeshes[eye] = result.stats.meshDraws;
    }
}
