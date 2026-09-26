#include "../../EditorLocalization.h"
#include "Engine.h"
#include "ViewportRenderHelpers.h"
#include "GizmoToolbar.h"
#include "ModelLoader.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <numeric>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <shlobj.h>
#endif

namespace Loc = Modularity::Loc;

using namespace ViewportRenderHelpers;

namespace {

ImVec2 MapRenderPixelToScreenPoint(const EmbeddedViewportLayout &layout,
                                   int renderWidth, int renderHeight,
                                   const glm::vec2 &renderPixel) {
  const float safeRenderWidth = static_cast<float>(std::max(1, renderWidth));
  const float safeRenderHeight = static_cast<float>(std::max(1, renderHeight));
  const float visibleUvWidth =
      std::max(0.0001f, layout.uvMax.x - layout.uvMin.x);
  const float visibleUvHeight =
      std::max(0.0001f, layout.uvMax.y - layout.uvMin.y);
  const float sourceU = renderPixel.x / safeRenderWidth;
  const float sourceV = renderPixel.y / safeRenderHeight;
  const float screenNormX = (sourceU - layout.uvMin.x) / visibleUvWidth;
  const float screenNormY = (sourceV - layout.uvMin.y) / visibleUvHeight;
  return ImVec2(layout.displayMin.x + screenNormX * layout.displaySize.x,
                layout.displayMin.y + screenNormY * layout.displaySize.y);
}

ImVec2 MapRenderDeltaToScreenDelta(const EmbeddedViewportLayout &layout,
                                   int renderWidth, int renderHeight,
                                   const glm::vec2 &renderDelta) {
  const float safeRenderWidth = static_cast<float>(std::max(1, renderWidth));
  const float safeRenderHeight = static_cast<float>(std::max(1, renderHeight));
  const float visibleUvWidth =
      std::max(0.0001f, layout.uvMax.x - layout.uvMin.x);
  const float visibleUvHeight =
      std::max(0.0001f, layout.uvMax.y - layout.uvMin.y);
  return ImVec2((renderDelta.x / safeRenderWidth) *
                    (layout.displaySize.x / visibleUvWidth),
                (renderDelta.y / safeRenderHeight) *
                    (layout.displaySize.y / visibleUvHeight));
}

void MapRenderRectToScreenRect(const EmbeddedViewportLayout &layout,
                               int renderWidth, int renderHeight,
                               const glm::vec2 &renderMin,
                               const glm::vec2 &renderMax, ImVec2 &outMin,
                               ImVec2 &outMax) {
  const ImVec2 p0 =
      MapRenderPixelToScreenPoint(layout, renderWidth, renderHeight, renderMin);
  const ImVec2 p1 =
      MapRenderPixelToScreenPoint(layout, renderWidth, renderHeight, renderMax);
  outMin = ImVec2(std::min(p0.x, p1.x), std::min(p0.y, p1.y));
  outMax = ImVec2(std::max(p0.x, p1.x), std::max(p0.y, p1.y));
}

template <typename EnumType, size_t N>
ObjectType MapEnumToObjectType(EnumType value,
                               const std::array<ObjectType, N> &mapping) {
  const size_t index = static_cast<size_t>(value);
  if (index >= mapping.size()) {
    return ObjectType::Empty;
  }
  return mapping[index];
}

static constexpr std::array<ObjectType,
                            static_cast<size_t>(RenderType::Sprite) + 1>
    kRenderTypeMainObjectMap = {{
        ObjectType::Empty,   // RenderType::None
        ObjectType::Cube,    // RenderType::Cube
        ObjectType::Sphere,  // RenderType::Sphere
        ObjectType::Capsule, // RenderType::Capsule
        ObjectType::OBJMesh, // RenderType::OBJMesh
        ObjectType::Model,   // RenderType::Model
        ObjectType::Mirror,  // RenderType::Mirror
        ObjectType::Plane,   // RenderType::Plane
        ObjectType::Torus,   // RenderType::Torus
        ObjectType::Sprite   // RenderType::Sprite
    }};

static constexpr std::array<ObjectType,
                            static_cast<size_t>(UIElementType::Sprite2D) + 1>
    kUiTypeMainObjectMap = {{
        ObjectType::Empty,    // UIElementType::None
        ObjectType::Canvas,   // UIElementType::Canvas
        ObjectType::UIImage,  // UIElementType::Image
        ObjectType::UISlider, // UIElementType::Slider
        ObjectType::UIButton, // UIElementType::Button
        ObjectType::UIText,   // UIElementType::Text
        ObjectType::Sprite2D  // UIElementType::Sprite2D
    }};

static constexpr std::array<ObjectType,
                            static_cast<size_t>(LightType::Area) + 1>
    kLightTypeMainObjectMap = {{
        ObjectType::DirectionalLight, // LightType::Directional
        ObjectType::PointLight,       // LightType::Point
        ObjectType::SpotLight,        // LightType::Spot
        ObjectType::AreaLight         // LightType::Area
    }};

} // namespace

#pragma region Game Viewport Window
void Engine::renderGameViewportWindow() {
  const auto runtimeUiStart = std::chrono::steady_clock::now();
  double runtimeSpriteBatchBuildMs = 0.0;
  uint32_t runtimeVisibleObjectCount = 0;
  Runtime2DWorldProfilerStats runtime2DProfilerStats;
  gameViewportFocused = false;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
  const bool windowVisible = ImGui::Begin(Loc::Window("WINDOW_GAME_VIEWPORT", "Game Viewport"), &showGameViewport,
                                          ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  if (!windowVisible) {
    ImGui::End();
    return;
  }
  {
    static bool dockTabWasHidden = false;
    const bool dockTabHidden = ViewportRenderHelpers::IsCurrentDockTabHidden();
    const bool skipHiddenTab = dockTabHidden && dockTabWasHidden;
    dockTabWasHidden = dockTabHidden;
    if (skipHiddenTab) {
      Modu2DStats::CountWindowSkipped();
      Modu2DStats::CountSkippedRedraw();
      ImGui::End();
      return;
    }
  }
  const bool showGameViewportToolbar = true;
  bool windowFocused =
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  struct GameResolutionOption {
    const char *label;
    int width;
    int height;
    bool useWindow;
    bool custom;
  };
  static const std::array<GameResolutionOption, 6> kGameResolutions = {
      {{"Default (1280x720)", 0, 0, false, false},
       {"1920x1080 (1080p)", 1920, 1080, false, false},
       {"1280x720 (720p)", 1280, 720, false, false},
       {"2560x1440 (1440p)", 2560, 1440, false, false},
       {"Custom", 0, 0, false, true},
       {"Native (Display Size)", 0, 0, true, false}}};
  if (gameViewportResolutionIndex < 0 ||
      gameViewportResolutionIndex >= (int)kGameResolutions.size()) {
    gameViewportResolutionIndex = 0;
  }
  gameViewportZoom = std::clamp(gameViewportZoom, 1.0f, 8.0f);

  static constexpr int kGameViewportPreviewSlot = 5001;

  SceneObject *playerCam = nullptr;
  for (auto &obj : sceneObjects) {
    if (IsObjectEnabledInHierarchy(obj) && obj.hasCamera &&
        obj.camera.type == SceneCameraType::Player) {
      playerCam = &obj;
      break;
    }
  }
  const bool hasVulkanSceneTexture =
      usingVulkan() && vulkanRendererInitialized && (vulkanRenderer != nullptr);
  const bool project2DPipeline = isProject2DPipeline();
  const bool project25DPipeline = isProject25DPipeline();
  const bool pure2DViewport =
      project2DPipeline || (playerCam && playerCam->camera.use2D);

  const GameResolutionOption &resOption =
      kGameResolutions[gameViewportResolutionIndex];
  const int activeCustomWidth = std::clamp(gameViewportCustomWidth, 64, 8192);
  const int activeCustomHeight = std::clamp(gameViewportCustomHeight, 64, 8192);
  const std::string resolutionComboLabel =
      resOption.custom ? ("Custom (" + std::to_string(activeCustomWidth) + "x" +
                          std::to_string(activeCustomHeight) + ")")
                       : std::string(resOption.label);
  auto viewportDisplayModeLabel = [](ViewportDisplayMode mode) {
    switch (mode) {
    case ViewportDisplayMode::Stretch:
      return "Stretch";
    case ViewportDisplayMode::Fill:
      return "Fill";
    case ViewportDisplayMode::IntegerScale:
      return "Integer";
    case ViewportDisplayMode::Fit:
    default:
      return "Fit";
    }
  };
  constexpr float kGameToolbarIconSource = 33.0f;
  const float gameToolbarIconSide =
      kGameToolbarIconSource *
      std::max(1.0f,
               std::floor(ImGui::GetFrameHeight() / kGameToolbarIconSource));
  const float gameToolbarSwitchSide = gameToolbarIconSide + 4.0f;
  const float gameToolbarRowHeight =
      std::max(gameToolbarSwitchSide, ImGui::GetFrameHeight());
  float gameToolbarRowY = 0.0f;
  auto alignToToolbarRow = [&](float itemHeight) {
    ImGui::SetCursorPosY(
        gameToolbarRowY +
        std::max(0.0f, (gameToolbarRowHeight - itemHeight) * 0.5f));
  };
  auto toolbarSeparator = [&]() {
    ImGui::SameLine();
    alignToToolbarRow(ImGui::GetTextLineHeight());
    ImGui::TextDisabled("|");
    ImGui::SameLine();
  };
  auto toolbarTooltip = [](const char *text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
      ImGui::SetTooltip("%s", text);
    }
  };

  struct GameToolbarIcon {
    ImTextureID id = static_cast<ImTextureID>(0);
    bool flipY = false;
  };
  auto resolveToolbarIcon = [&](const char *iconPath) -> GameToolbarIcon {
    if (!iconPath || !*iconPath) {
      return {};
    }
    if (rendererInitialized) {
      if (Texture *icon = renderer.getTexture(
              iconPath, MaterialProperties::TextureFilter::Point);
          icon && icon->GetID()) {
        return {static_cast<ImTextureID>(icon->GetID()), true};
      }
      return {};
    }
    if (hasVulkanSceneTexture && vulkanRenderer) {
      ImTextureID vkIcon = vulkanRenderer->getOrCreateUIImage(iconPath);
      if (vkIcon != static_cast<ImTextureID>(0)) {
        return {vkIcon, false};
      }
    }
    return {};
  };

  auto toolbarIconSwitch = [&](const char *id, const char *iconOnPath, const char *iconOffPath, const char *fallbackText, const char *tooltip, bool value) -> bool {
    alignToToolbarRow(gameToolbarSwitchSide);
    const ImVec2 slotSize(gameToolbarSwitchSide, gameToolbarSwitchSide);
    const ImVec2 slotPos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, slotSize);
    if (pressed) ImGui::ReportWidgetFeedback(value ? ImGuiWidgetFeedback_ToggleOff : ImGuiWidgetFeedback_ToggleOn);
    ImGui::ReportItemHoverFeedback();
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImVec2 slotMax(slotPos.x + slotSize.x, slotPos.y + slotSize.y);
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    if (value) {
      drawList->AddRectFilled(slotPos, slotMax, IM_COL32(122, 96, 200, held ? 122 : 82), 4.0f);
      drawList->AddRect(slotPos, slotMax, IM_COL32(198, 178, 255, 178), 4.0f, 0, 1.2f);
    } else if (hovered) {
      drawList->AddRectFilled(slotPos, slotMax, IM_COL32(255, 255, 255, held ? 42 : 24), 4.0f);
    }

    const GameToolbarIcon icon =
        resolveToolbarIcon(value ? iconOnPath : iconOffPath);
    const ImU32 tint =
        IM_COL32(255, 255, 255, value ? 255 : (hovered ? 226 : 198));
    if (icon.id != static_cast<ImTextureID>(0)) {
      const ImVec2 iconMin(
          std::floor(slotPos.x + (slotSize.x - gameToolbarIconSide) * 0.5f),
          std::floor(slotPos.y + (slotSize.y - gameToolbarIconSide) * 0.5f));
      const ImVec2 iconMax(iconMin.x + gameToolbarIconSide, iconMin.y + gameToolbarIconSide);
      const ImVec2 uvMin(0.0f, icon.flipY ? 1.0f : 0.0f);
      const ImVec2 uvMax(1.0f, icon.flipY ? 0.0f : 1.0f);
      drawList->AddImage(icon.id, iconMin, iconMax, uvMin, uvMax, tint);
    } else {
      const ImVec2 textSize = ImGui::CalcTextSize(fallbackText);
      drawList->AddText(
          ImVec2(slotPos.x + (slotSize.x - textSize.x) * 0.5f,
                 slotPos.y + (slotSize.y - textSize.y) * 0.5f),
          tint, fallbackText);
    }
    if (hovered && tooltip && *tooltip) {
      ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
  };

  if (!isPlaying && showGameViewportToolbar) {
    gameToolbarRowY = ImGui::GetCursorPosY();
    ImGui::Dummy(ImVec2(0.0f, gameToolbarRowHeight));
    ImGui::SameLine(0.0f, 0.0f);
    alignToToolbarRow(ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(210.0f);
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::BeginCombo("##GameViewportResolution", resolutionComboLabel.c_str())) {
      for (int i = 0; i < (int)kGameResolutions.size(); ++i) {
        bool selected = (i == gameViewportResolutionIndex);
        if (ImGui::Selectable(kGameResolutions[i].label, selected)) {
          gameViewportResolutionIndex = i;
          saveBuildSettings();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      if (kGameResolutions[gameViewportResolutionIndex].custom) {
        ImGui::Separator();
        ImGui::TextDisabled("Custom Resolution");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::DragInt("Width", &gameViewportCustomWidth, 1.0f, 64, 8192);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::DragInt("Height", &gameViewportCustomHeight, 1.0f, 64, 8192);
        gameViewportCustomWidth = std::clamp(gameViewportCustomWidth, 64, 8192);
        gameViewportCustomHeight =
            std::clamp(gameViewportCustomHeight, 64, 8192);
      }
      ImGui::EndCombo();
    }
    toolbarTooltip("Internal render resolution");

    toolbarSeparator();
    if (toolbarIconSwitch(
            "##GameViewportAutoFit",
            "Resources/Engine-Root/Game Viewport/Auto Fit ON.png",
            "Resources/Engine-Root/Game Viewport/Auto Fit OFF.png", "AF",
            "Auto Fit: scale the render target to the panel with the display "
            "mode instead of stretching it",
            gameViewportAutoFit)) {
      gameViewportAutoFit = !gameViewportAutoFit;
    }

    toolbarSeparator();
    alignToToolbarRow(ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("##GameViewportDisplayMode",
                          viewportDisplayModeLabel(gameViewportDisplayMode))) {
      const ViewportDisplayMode displayModes[] = {
          ViewportDisplayMode::Fit, ViewportDisplayMode::Stretch,
          ViewportDisplayMode::Fill, ViewportDisplayMode::IntegerScale};
      for (ViewportDisplayMode mode : displayModes) {
        bool selected = (mode == gameViewportDisplayMode);
        if (ImGui::Selectable(viewportDisplayModeLabel(mode), selected)) {
          gameViewportDisplayMode = mode;
          saveEditorUserSettings();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    toolbarTooltip("Viewport display mode");

    toolbarSeparator();
    float zoomPercent = gameViewportZoom * 100.0f;
    alignToToolbarRow(ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("##GameViewportZoom", &zoomPercent, 100.0f, 800.0f,
                           "%.0f%%", ImGuiSliderFlags_Logarithmic)) {
      gameViewportZoom = std::clamp(zoomPercent / 100.0f, 1.0f, 8.0f);
    }
    toolbarTooltip("Viewport zoom");

    const float toolbarSpacing = ImGui::GetStyle().ItemSpacing.x;
    bool gameViewportToolbarChanged = false;
    toolbarSeparator();
    if (toolbarIconSwitch("##GameViewportProfiler",
                          "Resources/Engine-Root/Game Viewport/Profiler ON.png",
                          "Resources/Engine-Root/Game Viewport/Profiler OFF.png",
                          "PR", "Profiler: show the game profiler overlay",
                          showGameProfiler)) {
      showGameProfiler = !showGameProfiler;
      gameViewportToolbarChanged = true;
    }
    ImGui::SameLine(0.0f, toolbarSpacing * 0.8f);
    if (toolbarIconSwitch(
            "##GameViewportUiPreview",
            "Resources/Engine-Root/Game Viewport/Canvas Preview ON.png",
            "Resources/Engine-Root/Game Viewport/Canvas Preview OFF.png", "UI",
            "UI Preview: show editor UI canvas previews while not playing",
            uiCanvasPreviewEnabled)) {
      uiCanvasPreviewEnabled = !uiCanvasPreviewEnabled;
      gameViewportToolbarChanged = true;
    }
    ImGui::SameLine(0.0f, toolbarSpacing * 0.8f);
    if (toolbarIconSwitch(
            "##GameViewportWorld2DPostFx",
            "Resources/Engine-Root/Game Viewport/2D Post FX ON.png",
            "Resources/Engine-Root/Game Viewport/2D Post FX OFF.png", "FX",
            "2D PostFX: apply 2D post FX after the world render and blend in "
            "active ModuVolume settings",
            world2DPostFx.enabled)) {
      world2DPostFx.enabled = !world2DPostFx.enabled;
      gameViewportToolbarChanged = true;
    }
    ImGui::SameLine(0.0f, toolbarSpacing * 0.35f);
    alignToToolbarRow(ImGui::GetFrameHeight());
    if (ImGui::ArrowButton("##game_world2d_postfx_settings_arrow",
                           ImGuiDir_Down)) {
      ImGui::OpenPopup("##game_world2d_postfx_settings_popup");
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("2D World Post FX settings");
    }
    if (ImGui::BeginPopup("##game_world2d_postfx_settings_popup")) {
      ImGui::TextUnformatted("2D World Post FX");
      ImGui::Separator();
      if (ImGui::SliderFloat("Dither Intensity", &world2DPostFx.ditherIntensity,
                             0.0f, 1.5f, "%.2f")) {
        gameViewportToolbarChanged = true;
      }
      if (ImGui::SliderInt("Color Bit Depth", &world2DPostFx.colorBits, 1, 8,
                           "%d bits")) {
        gameViewportToolbarChanged = true;
      }
      if (ImGui::SliderFloat("Dark Adjustment", &world2DPostFx.darkAdjustment,
                             0.0f, 1.0f, "%.2f")) {
        gameViewportToolbarChanged = true;
      }
      if (ImGui::SliderFloat("Pattern Scale", &world2DPostFx.ditherScale, 1.0f,
                             8.0f, "%.1fx")) {
        gameViewportToolbarChanged = true;
      }
      if (ImGui::SliderFloat("Pixelation", &world2DPostFx.pixelation, 0.0f,
                             64.0f, "%.1f px")) {
        gameViewportToolbarChanged = true;
      }
      ImGui::Separator();
      if (ImGui::SliderFloat("Exposure", &world2DPostFx.exposure, -4.0f, 4.0f,
                             "%.2f EV")) {
        gameViewportToolbarChanged = true;
      }
      if (ImGui::SliderFloat("Contrast", &world2DPostFx.contrast, 0.0f, 2.5f,
                             "%.2f")) {
        gameViewportToolbarChanged = true;
      }
      if (ImGui::SliderFloat("Saturation", &world2DPostFx.saturation, 0.0f,
                             2.5f, "%.2f")) {
        gameViewportToolbarChanged = true;
      }
      if (ImGui::ColorEdit3("Color Filter", &world2DPostFx.colorFilter.x,
                            ImGuiColorEditFlags_Float)) {
        gameViewportToolbarChanged = true;
      }
      ImGui::Separator();
      if (ImGui::SliderFloat("Vignette", &world2DPostFx.vignetteIntensity, 0.0f,
                             1.0f, "%.2f")) {
        gameViewportToolbarChanged = true;
      }
      if (ImGui::SliderFloat("Vignette Softness",
                             &world2DPostFx.vignetteSmoothness, 0.05f, 1.0f,
                             "%.2f")) {
        gameViewportToolbarChanged = true;
      }
      if (ImGui::SliderFloat("Chromatic Aberration",
                             &world2DPostFx.chromaticAmount, 0.0f, 0.05f,
                             "%.4f")) {
        gameViewportToolbarChanged = true;
      }
      if (ImGui::SliderFloat("Sharpen", &world2DPostFx.sharpenStrength, 0.0f,
                             2.0f, "%.2f")) {
        gameViewportToolbarChanged = true;
      }
      if (ImGui::SliderFloat("Film Grain", &world2DPostFx.grainAmount, 0.0f,
                             0.4f, "%.2f")) {
        gameViewportToolbarChanged = true;
      }
      if (ImGui::SliderFloat("Scanlines", &world2DPostFx.scanlineIntensity,
                             0.0f, 1.0f, "%.2f")) {
        gameViewportToolbarChanged = true;
      }
      ImGui::Separator();
      if (ImGui::Button("Reset Defaults")) {
        world2DPostFx = Light2DPostFXSettings{};
        gameViewportToolbarChanged = true;
      }
      ImGui::EndPopup();
    }
    if (gameViewportToolbarChanged) {
      saveEditorUserSettings();
    }
  }

  ImVec2 avail = ImGui::GetContentRegionAvail();
  int renderWidth = kRuntimeInternalWidth;
  int renderHeight = kRuntimeInternalHeight;
  getRuntimeInternalResolution(renderWidth, renderHeight);
  gameViewportLastRenderWidth = std::max(1, renderWidth);
  gameViewportLastRenderHeight = std::max(1, renderHeight);
  const ViewportDisplayMode activeDisplayMode =
      gameViewportAutoFit ? gameViewportDisplayMode
                          : ViewportDisplayMode::Stretch;
  const ImVec2 panelMin = ImGui::GetCursorScreenPos();
  const ImVec2 panelSize(std::max(1.0f, avail.x), std::max(1.0f, avail.y));
  const EmbeddedViewportLayout gameLayout = BuildEmbeddedViewportLayout(
      panelMin, panelSize, renderWidth, renderHeight, activeDisplayMode,
      gameViewportZoom);
  const ImVec2 frameSize = gameLayout.displaySize;

  if (!isPlaying) {
    gameViewCursorLocked = false;
  }

  if (!rendererInitialized && !hasVulkanSceneTexture) {
    ImGui::InvisibleButton("##GameViewportPanelEmpty", panelSize);
    ImVec2 imageSize = frameSize;
    ImVec2 imageMin = gameLayout.displayMin;
    ImVec2 imageMax = gameLayout.displayMax;
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(imageMin, imageMax, IM_COL32(14, 18, 30, 255),
                            8.0f);
    drawList->AddRect(imageMin, imageMax, IM_COL32(78, 96, 128, 210), 8.0f, 0,
                      1.5f);

    const char *title = usingVulkan() ? "Vulkan Game Viewport Unavailable"
                                      : "Game Viewport Unavailable";
    const char *line1 = usingVulkan()
                            ? "Vulkan game render target is not ready."
                            : "Renderer is not initialized for this session.";
    const char *line2 =
        usingVulkan()
            ? "Open a project scene or retry after renderer initialization."
            : "Open or create a project to initialize rendering.";

    ImVec2 titleSize = ImGui::CalcTextSize(title);
    ImVec2 line1Size = ImGui::CalcTextSize(line1);
    ImVec2 line2Size = ImGui::CalcTextSize(line2);
    float centerX = imageMin.x + imageSize.x * 0.5f;
    float baseY = imageMin.y + imageSize.y * 0.5f - 28.0f;
    drawList->AddText(ImVec2(centerX - titleSize.x * 0.5f, baseY),
                      IM_COL32(220, 228, 244, 255), title);
    drawList->AddText(ImVec2(centerX - line1Size.x * 0.5f, baseY + 24.0f),
                      IM_COL32(170, 184, 212, 255), line1);
    drawList->AddText(ImVec2(centerX - line2Size.x * 0.5f, baseY + 44.0f),
                      IM_COL32(170, 184, 212, 255), line2);

    gameViewportFocused = ImGui::IsWindowFocused();
  } else if (!playerCam && (rendererInitialized || hasVulkanSceneTexture)) {
    ImGui::InvisibleButton("##GameViewportPanelNoCamera", panelSize);
    ImVec2 imageSize = frameSize;
    ImVec2 imageMin = gameLayout.displayMin;
    ImVec2 imageMax = gameLayout.displayMax;
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(imageMin, imageMax, IM_COL32(14, 18, 30, 255),
                            8.0f);
    drawList->AddRect(imageMin, imageMax, IM_COL32(78, 96, 128, 210), 8.0f, 0,
                      1.5f);

    const char *title = "Game Viewport Camera Missing";
    const char *line1 = "No enabled Player camera was found in the scene.";
    const char *line2 = "Create a Camera and set its type to Player.";
    ImVec2 titleSize = ImGui::CalcTextSize(title);
    ImVec2 line1Size = ImGui::CalcTextSize(line1);
    ImVec2 line2Size = ImGui::CalcTextSize(line2);
    float centerX = imageMin.x + imageSize.x * 0.5f;
    float baseY = imageMin.y + imageSize.y * 0.5f - 28.0f;
    drawList->AddText(ImVec2(centerX - titleSize.x * 0.5f, baseY),
                      IM_COL32(220, 228, 244, 255), title);
    drawList->AddText(ImVec2(centerX - line1Size.x * 0.5f, baseY + 24.0f),
                      IM_COL32(170, 184, 212, 255), line1);
    drawList->AddText(ImVec2(centerX - line2Size.x * 0.5f, baseY + 44.0f),
                      IM_COL32(170, 184, 212, 255), line2);
    gameViewportFocused = ImGui::IsWindowFocused();
  } else if (playerCam && (rendererInitialized || hasVulkanSceneTexture)) {
    Modu2DStats::CountViewportRedraw();
    ImTextureID texId = static_cast<ImTextureID>(0);
    Modularity::Render25D::TMRenderer::RenderStats tmStats;
    std::string tmError;
    if (rendererInitialized) {
      MODU_PROFILE_SCOPE("Render", ProfilerSampleCategory::Render);
      const bool ownsGpuQuery = Profiler::instance().beginOpenGlGpuFrame();
      unsigned int tex = 0;
      const float gameViewFov = ResolveCameraVerticalFovDeg(
          playerCam->camera,
          static_cast<float>(renderWidth) /
              static_cast<float>(std::max(1, renderHeight)));
      if (project25DPipeline) {
        renderTMViewportPass(makeCameraFromObject(*playerCam), renderWidth,
                             renderHeight, gameViewFov,
                             playerCam->camera.nearClip,
                             playerCam->camera.farClip, &tex, &tmStats,
                             &tmError, kGameViewportPreviewSlot);
      } else if (!pure2DViewport) {
        tex = renderer.renderScenePreview(
            makeCameraFromObject(*playerCam), sceneObjects, renderWidth,
            renderHeight, gameViewFov, playerCam->camera.nearClip,
            playerCam->camera.farClip,
            false,
            kGameViewportPreviewSlot);
      }
      if (ownsGpuQuery) {
        Profiler::instance().endOpenGlGpuFrame();
      }
      texId = (ImTextureID)(intptr_t)tex;
    } else if (vulkanRenderer) {
      vulkanRenderer->setGameSceneSize(
          static_cast<uint32_t>(std::max(1, renderWidth)),
          static_cast<uint32_t>(std::max(1, renderHeight)));
      texId = vulkanRenderer->getGameSceneTextureID();
    }

    ImVec2 imageSize = frameSize;
    float effectiveOutputZoom = std::clamp(gameViewportZoom, 1.0f, 8.0f);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    ImGui::InvisibleButton("GameViewportRenderFrame", panelSize);
    ImGui::PopStyleColor(3);

    ImVec2 imageMin = gameLayout.displayMin;
    ImVec2 imageMax = gameLayout.displayMax;
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(gameLayout.panelMin, gameLayout.panelMax,
                            IM_COL32(12, 14, 20, 255), 6.0f);

    glm::vec2 hoveredPixel(0.0f);
    bool imageHovered =
        TryMapScreenPointToRenderPixel(gameLayout, ImGui::GetIO().MousePos,
                                       renderWidth, renderHeight, hoveredPixel);
    if (imageHovered) {
      ImGuiIO &io = ImGui::GetIO();
      if (io.MouseWheel != 0.0f) {
        const float factor = std::pow(1.12f, io.MouseWheel);
        gameViewportZoom = std::clamp(gameViewportZoom * factor, 1.0f, 8.0f);
        effectiveOutputZoom = std::clamp(gameViewportZoom, 1.0f, 8.0f);
      }
    }

    const EmbeddedViewportLayout outputLayout = BuildEmbeddedViewportLayout(
        panelMin, panelSize, renderWidth, renderHeight, activeDisplayMode,
        effectiveOutputZoom);
    imageSize = outputLayout.displaySize;
    imageMin = outputLayout.displayMin;
    imageMax = outputLayout.displayMax;

    if (texId != static_cast<ImTextureID>(0)) {
      if (rendererInitialized) {
        const GLuint tex = static_cast<GLuint>(texId);
        ApplyNearestTextureSampling(tex);
      }
      drawList->PushClipRect(outputLayout.panelMin, outputLayout.panelMax,
                             true);
      drawList->AddImage(
          texId, outputLayout.displayMin, outputLayout.displayMax,
          rendererInitialized
              ? ImVec2(outputLayout.uvMin.x, 1.0f - outputLayout.uvMin.y)
              : outputLayout.uvMin,
          rendererInitialized
              ? ImVec2(outputLayout.uvMax.x, 1.0f - outputLayout.uvMax.y)
              : outputLayout.uvMax);
      drawList->PopClipRect();
    } else if (pure2DViewport) {
      drawList->AddRectFilled(imageMin, imageMax, IM_COL32(0, 0, 0, 255));
    } else {
      drawList->AddRectFilled(imageMin, imageMax, IM_COL32(32, 36, 48, 255));
    }
    if (project25DPipeline) {
      const std::string label = buildTMOverlayLabel(tmStats, tmError);
      drawList->AddText(ImVec2(imageMin.x + 10.0f, imageMin.y + 10.0f),
                        IM_COL32(236, 240, 246, 255), label.c_str());
    }
    const ImVec2 renderToScreenScale = MapRenderDeltaToScreenDelta(
        outputLayout, renderWidth, renderHeight, glm::vec2(1.0f, 1.0f));
    float uiScaleX = renderToScreenScale.x;
    float uiScaleY = renderToScreenScale.y;
    if (showGameViewportToolbar && showCanvasOverlay) {
      ImVec2 pad(8.0f, 8.0f);
      ImVec2 tl(imageMin.x + pad.x, imageMin.y + pad.y);
      ImVec2 br(imageMax.x - pad.x, imageMax.y - pad.y);
      drawList->AddRect(tl, br, IM_COL32(110, 170, 255, 180), 8.0f, 0, 2.0f);
    }
    bool profilerOverlayPending = false;
    std::vector<std::string> profilerOverlayLines;
    ImVec2 profilerPanelMin(0.0f, 0.0f);

    if (showGameViewportToolbar && showGameProfiler) {
      float fps = ImGui::GetIO().Framerate;
      float frameMs = (fps > 0.0f) ? (1000.0f / fps) : 0.0f;
      int zoomPercent = (int)std::round(effectiveOutputZoom * 100.0f);
      static const Renderer::RenderStats zeroStats{};
      static const Renderer::PostProcessStats zeroPostStats{};
      const Renderer::RenderStats &stats =
          (rendererInitialized && !pure2DViewport)
              ? renderer.getLastPreviewStats()
              : zeroStats;
      const Renderer::PostProcessStats &postStats =
          (rendererInitialized && !pure2DViewport)
              ? renderer.getLastPreviewPostStats()
              : zeroPostStats;

      static ViewportRenderHelpers::ViewportFpsWindow fpsWindow;
      fpsWindow.accumulate(ImGui::GetIO().DeltaTime);

      char line1[128];
      char line2[128];
      char line3[128];
      char line4[128];
      char line5[160];
      char line6[160];
      char line7[192];
      std::snprintf(line1, sizeof(line1), "FPS: %.0f (%.1f ms)", fps, frameMs);
      if (fpsWindow.hasResult) {
        std::snprintf(line2, sizeof(line2), "1%% Low: %.0f | Max: %.0f",
                      fpsWindow.onePercentLowFps, fpsWindow.maxFps);
      } else {
        std::snprintf(line2, sizeof(line2), "1%% Low: -- | Max: --");
      }
      std::snprintf(line3, sizeof(line3), "Batches: %d", stats.drawCalls);
      std::snprintf(line4, sizeof(line4), "Meshes: %d", stats.meshDraws);
      std::snprintf(line5, sizeof(line5), "Render: %dx%d @ %d%%", renderWidth,
                    renderHeight, zoomPercent);
      std::snprintf(line6, sizeof(line6),
                    "PostFX: %.2f ms | Bloom: %.2f / %.2f", postStats.totalMs,
                    postStats.bloomExtractMs, postStats.bloomBlurMs);
      std::snprintf(line7, sizeof(line7), "ModuVolume: %s x%.2f (%d active)",
                    postStats.resolvedVolumeName.empty()
                        ? "None"
                        : postStats.resolvedVolumeName.c_str(),
                    postStats.resolvedBlend, postStats.activeVolumeCount);

      const char *lines[] = {line1, line2, line3, line4, line5, line6, line7};
      profilerOverlayLines.reserve(sizeof(lines) / sizeof(lines[0]) + 1);
      for (const char *line : lines) {
        profilerOverlayLines.emplace_back(line);
      }
      if (stats.triangles > 0) {
        char trianglesLine[96];
        std::snprintf(trianglesLine, sizeof(trianglesLine), "Triangles: %lld",
                      stats.triangles);
        profilerOverlayLines.emplace_back(trianglesLine);
      }
      if (stats.optimizationFeatures != 0) {
        // Project Settings > Graphics Manager > Optimization.
        char cullLine[192];
        std::snprintf(cullLine, sizeof(cullLine),
                      "Culling%s: %d / %d visible | %d culled | %d LOD | %.2f ms",
                      stats.optimizationAutomatic ? " (auto)" : "",
                      stats.visibleRenderables, stats.objectsSubmitted,
                      stats.objectsCulled, stats.objectsLodCulled,
                      stats.cpuCullingMs);
        profilerOverlayLines.emplace_back(cullLine);
        char queueLine[160];
        std::snprintf(queueLine, sizeof(queueLine),
                      "Queue: %.2f ms | Sort: %.2f ms", stats.queueBuildMs,
                      stats.sortingMs);
        profilerOverlayLines.emplace_back(queueLine);
        if (stats.mdiBatches > 0) {
          char mdiLine[192];
          std::snprintf(mdiLine, sizeof(mdiLine),
                        "Multi-draw: %d obj | %d batches | %d cmds",
                        stats.mdiObjects, stats.mdiBatches,
                        stats.indirectDrawCommands);
          profilerOverlayLines.emplace_back(mdiLine);
        }
        if (stats.gpuCullingMs >= 0.0f || stats.gpuVisible >= 0) {
          char gpuLine[160];
          std::snprintf(gpuLine, sizeof(gpuLine),
                        "GPU cull: %.3f ms | %d visible (async)",
                        stats.gpuCullingMs, stats.gpuVisible);
          profilerOverlayLines.emplace_back(gpuLine);
        }
      }
      const UiCanvas3DFrameStats &canvasStats = uiCanvas3DStatsLastFrame;
      if (canvasStats.active > 0) {
        char line8[160];
        std::snprintf(line8, sizeof(line8),
                      "3D Canvas: %d full | %d partial | %d cached | %.2f Mpx",
                      canvasStats.fullRedraws, canvasStats.partialRedraws,
                      canvasStats.cached, canvasStats.redrawnPixels / 1.0e6);
        profilerOverlayLines.emplace_back(line8);
      }
      profilerPanelMin = ImVec2(imageMin.x + 14.0f, imageMin.y + 14.0f);
      profilerOverlayPending = true;
    }
    const bool drawUiCanvasPreview = isPlaying || uiCanvasPreviewEnabled;
    if (drawUiCanvasPreview) {
    bool uiInteracting = false;
    UiSceneLookupCache uiSceneLookup(sceneObjects);
    auto find3DCanvasId = [&](const SceneObject &target) -> int {
      return uiSceneLookup.find3DCanvasId(target);
    };
    auto findPseudo3DCanvasId = [&](const SceneObject &target) -> int {
      return uiSceneLookup.findPseudo3DCanvasId(target);
    };
    [[maybe_unused]] auto isUiOn3DCanvas = [&](const SceneObject &target) {
      return find3DCanvasId(target) >= 0;
    };
    int editCanvas3DId = -1;
    if (SceneObject *selected = getSelectedObject()) {
      editCanvas3DId = find3DCanvasId(*selected);
    }
    auto isUIType = [&](const SceneObject &target) {
      if (target.type == ObjectType::Sprite25D)
        return false;
      if (!target.hasUI || target.ui.type == UIElementType::None)
        return false;
      int canvasId = find3DCanvasId(target);
      if (!((canvasId < 0) || (canvasId == editCanvas3DId))) {
        return false;
      }
      return findPseudo3DCanvasId(target) < 0;
    };
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::SetCursorScreenPos(imageMin);
    ImGui::BeginChild(
        "GameUIOverlay",
        ImVec2(imageMax.x - imageMin.x, imageMax.y - imageMin.y), false,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    auto anchorToPivot = [](UIAnchor anchor, const ImVec2 &size) {
      switch (anchor) {
      case UIAnchor::Center:
        return ImVec2(size.x * 0.5f, size.y * 0.5f);
      case UIAnchor::TopLeft:
        return ImVec2(0.0f, 0.0f);
      case UIAnchor::TopRight:
        return ImVec2(size.x, 0.0f);
      case UIAnchor::BottomLeft:
        return ImVec2(0.0f, size.y);
      case UIAnchor::BottomRight:
        return ImVec2(size.x, size.y);
      default:
        return ImVec2(size.x * 0.5f, size.y * 0.5f);
      }
    };
    auto anchorToPoint = [](UIAnchor anchor, const ImVec2 &min,
                            const ImVec2 &max) {
      switch (anchor) {
      case UIAnchor::Center:
        return ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
      case UIAnchor::TopLeft:
        return min;
      case UIAnchor::TopRight:
        return ImVec2(max.x, min.y);
      case UIAnchor::BottomLeft:
        return ImVec2(min.x, max.y);
      case UIAnchor::BottomRight:
        return max;
      default:
        return ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
      }
    };

    auto resolveUIRect = [&](const SceneObject &obj, ImVec2 &outMin,
                             ImVec2 &outMax, ImVec2 *parentMin = nullptr,
                             ImVec2 *parentMax = nullptr,
                             float *outRotRad = nullptr) {
      std::vector<const SceneObject *> chain;
      const SceneObject *current = &obj;
      while (current) {
        if (isUIType(*current)) {
          chain.push_back(current);
        }
        if (current->parentId < 0)
          break;
        auto pit = std::find_if(
            sceneObjects.begin(), sceneObjects.end(),
            [&](const SceneObject &o) { return o.id == current->parentId; });
        if (pit == sceneObjects.end())
          break;
        current = &(*pit);
      }
      std::reverse(chain.begin(), chain.end());

      glm::vec2 regionMinRender(0.0f, 0.0f);
      glm::vec2 regionMaxRender(static_cast<float>(renderWidth),
                                static_cast<float>(renderHeight));
      float accumRot = 0.0f;
      glm::vec2 parentCenter((regionMinRender.x + regionMaxRender.x) * 0.5f,
                             (regionMinRender.y + regionMaxRender.y) * 0.5f);
      auto rotateAbout = [](glm::vec2 p, glm::vec2 pivot, float rad) {
        return pivot + ViewportRenderHelpers::RotateUiVec2(p - pivot, rad);
      };
      for (size_t idx = 0; idx < chain.size(); ++idx) {
        const SceneObject *node = chain[idx];
        if (idx + 1 == chain.size() && parentMin && parentMax) {
          MapRenderRectToScreenRect(outputLayout, renderWidth, renderHeight,
                                    regionMinRender, regionMaxRender,
                                    *parentMin, *parentMax);
        }
        glm::vec2 nodeSizeWorld = getSpriteDisplaySize(*node);
        ImVec2 size = ImVec2(std::max(1.0f, nodeSizeWorld.x),
                             std::max(1.0f, nodeSizeWorld.y));
        ImVec2 anchorPoint = anchorToPoint(
            node->ui.anchor, ImVec2(regionMinRender.x, regionMinRender.y),
            ImVec2(regionMaxRender.x, regionMaxRender.y));
        glm::vec2 anchorWorld = rotateAbout(
            glm::vec2(anchorPoint.x, anchorPoint.y), parentCenter, accumRot);
        glm::vec2 off = ViewportRenderHelpers::RotateUiVec2(
            glm::vec2(node->ui.position.x, node->ui.position.y), accumRot);
        glm::vec2 pivot = anchorWorld + off;
        const float nodeRot = accumRot + glm::radians(node->ui.rotation);
        ImVec2 pivotOffset = anchorToPivot(node->ui.anchor, size);
        glm::vec2 centerOff = ViewportRenderHelpers::RotateUiVec2(
            glm::vec2(size.x * 0.5f - pivotOffset.x,
                      size.y * 0.5f - pivotOffset.y),
            nodeRot);
        glm::vec2 center = pivot + centerOff;
        regionMinRender =
            center - glm::vec2(size.x * 0.5f, size.y * 0.5f);
        regionMaxRender =
            center + glm::vec2(size.x * 0.5f, size.y * 0.5f);
        parentCenter = center;
        accumRot = nodeRot;
      }
      MapRenderRectToScreenRect(outputLayout, renderWidth, renderHeight,
                                regionMinRender, regionMaxRender, outMin,
                                outMax);
      if (outRotRad)
        *outRotRad = accumRot;
    };

    ImVec2 overlayPos = ImGui::GetWindowPos();
    ImVec2 overlaySize = ImGui::GetWindowSize();
    bool allowEditorUi = false;
    bool useWorldUi =
        project2DPipeline || (playerCam && playerCam->camera.use2D);
    UIWorldCamera2D uiWorldCameraBackup = uiWorldCamera;
    bool restoreUiWorldCamera = false;
    if (playerCam && useWorldUi) {
      useWorldUi = true;
      restoreUiWorldCamera = true;
      uiWorldCamera.position =
          glm::vec2(playerCam->position.x, playerCam->position.y);
      uiWorldCamera.zoom = std::max(1.0f, playerCam->camera.pixelsPerUnit);
    }
    uiWorldPanning = false;
    if (useWorldUi) {
      uiWorldCamera.viewportSize = glm::vec2(static_cast<float>(renderWidth),
                                             static_cast<float>(renderHeight));
    }
    runtime2DProfilerStats.useWorldUi = useWorldUi;
    Camera projectedUiCamera =
        playerCam ? makeCameraFromObject(*playerCam) : Camera{};
    glm::mat4 projectedUiView(1.0f);
    glm::mat4 projectedUiProj(1.0f);
    bool hasProjectedUiCamera = false;
    if (playerCam && !useWorldUi) {
      projectedUiView = projectedUiCamera.getViewMatrix();
      projectedUiProj = BuildCameraProjection(
          projectedUiCamera, renderWidth, std::max(1, renderHeight),
          ResolveCameraVerticalFovDeg(
              playerCam->camera,
              static_cast<float>(renderWidth) /
                  std::max(1.0f, static_cast<float>(renderHeight))),
          playerCam->camera.nearClip, playerCam->camera.farClip);
      hasProjectedUiCamera = true;
    }
    if (!useWorldUi && hasProjectedUiCamera) {
      BatchedSpriteEmitter particleBatch(drawList);
      const unsigned int fallbackParticleTextureId =
          rendererInitialized ? renderer.getDebugWhiteTextureId() : 0;
      const glm::mat4 invView = glm::inverse(projectedUiView);
      const glm::vec3 cameraRight = glm::normalize(glm::vec3(invView[0]));
      const glm::vec3 cameraUp = glm::normalize(glm::vec3(invView[1]));
      for (auto &obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasParticleSystem2D) {
          continue;
        }
        SimulateParticleSystem2D(obj, glfwGetTime());
        ParticleSystem2DComponent &ps = obj.particleSystem2D;
        if (!ps.enabled || ps.particles.empty()) continue;

        unsigned int texId = 0;
        const std::string texturePath =
            !ps.texturePath.empty() ? ps.texturePath : obj.albedoTexturePath;
        if (rendererInitialized && !texturePath.empty()) {
          if (Texture *particleTex =
                  renderer.getTexture(texturePath, obj.material.textureFilter)) {
            texId = particleTex->GetID();
          }
        }
        if (texId == 0) texId = fallbackParticleTextureId;

        glm::mat4 objectRotation(1.0f);
        objectRotation =
            glm::rotate(objectRotation, glm::radians(obj.rotation.x),
                        glm::vec3(1.0f, 0.0f, 0.0f));
        objectRotation =
            glm::rotate(objectRotation, glm::radians(obj.rotation.y),
                        glm::vec3(0.0f, 1.0f, 0.0f));
        objectRotation =
            glm::rotate(objectRotation, glm::radians(obj.rotation.z),
                        glm::vec3(0.0f, 0.0f, 1.0f));

        for (const auto &particle : ps.particles) {
          if (!particle.alive) continue;
          const float t = std::clamp(
              particle.age / std::max(0.01f, particle.lifetime), 0.0f, 1.0f);
          const float size = ps.sizeOverLifetimeEnabled
                                 ? glm::mix(particle.size,
                                            ps.sizeOverLifetime, t)
                                 : particle.size;
          const float halfSize = std::max(0.001f, size) * 0.5f;
          const float angle = glm::radians(particle.rotation);
          const float c = std::cos(angle);
          const float s = std::sin(angle);
          const glm::vec3 particleWorldCenter =
              obj.position +
              glm::vec3(objectRotation *
                        glm::vec4(particle.position.x * obj.scale.x,
                                  particle.position.y * obj.scale.y,
                                  particle.position.z * obj.scale.z,
                                  0.0f));
          auto localToWorld = [&](float x, float y) {
            const float rx = x * c - y * s;
            const float ry = x * s + y * c;
            return particleWorldCenter + cameraRight * rx + cameraUp * ry;
          };

          std::array<ImVec2, 4> quad;
          const std::array<glm::vec3, 4> corners = {
              localToWorld(-halfSize, -halfSize),
              localToWorld(halfSize, -halfSize),
              localToWorld(halfSize, halfSize),
              localToWorld(-halfSize, halfSize),
          };
          bool valid = true;
          for (size_t i = 0; i < corners.size(); ++i) {
            ImVec2 renderPoint;
            if (!ProjectWorldToOverlayPoint(
                    corners[i], projectedUiView, projectedUiProj,
                    ImVec2(0.0f, 0.0f),
                    ImVec2(static_cast<float>(renderWidth),
                           static_cast<float>(renderHeight)),
                    renderPoint)) {
              valid = false;
              break;
            }
            quad[i] = MapRenderPixelToScreenPoint(
                outputLayout, renderWidth, renderHeight,
                glm::vec2(renderPoint.x, renderPoint.y));
          }
          if (!valid) continue;

          ImVec2 rectMin(quad[0].x, quad[0].y);
          ImVec2 rectMax(quad[0].x, quad[0].y);
          for (const ImVec2 &point : quad) {
            rectMin.x = std::min(rectMin.x, point.x);
            rectMin.y = std::min(rectMin.y, point.y);
            rectMax.x = std::max(rectMax.x, point.x);
            rectMax.y = std::max(rectMax.y, point.y);
          }
          if (rectMax.x < outputLayout.panelMin.x ||
              rectMin.x > outputLayout.panelMax.x ||
              rectMax.y < outputLayout.panelMin.y ||
              rectMin.y > outputLayout.panelMax.y) {
            continue;
          }

          const glm::vec4 tint =
              ps.colorOverLifetimeEnabled
                  ? glm::mix(particle.startColor, ps.colorOverLifetime, t)
                  : particle.startColor;
          const ImU32 tintColor = ImGui::GetColorU32(
              ImVec4(tint.r, tint.g, tint.b, tint.a));
          if (texId != 0) {
            particleBatch.push((ImTextureID)(intptr_t)texId, quad[0], quad[1],
                               quad[2], quad[3], ImVec2(0.0f, 0.0f),
                               ImVec2(1.0f, 0.0f), ImVec2(1.0f, 1.0f),
                               ImVec2(0.0f, 1.0f), tintColor);
          } else {
            particleBatch.flush();
            drawList->AddQuadFilled(quad[0], quad[1], quad[2], quad[3],
                                    tintColor);
          }
        }
      }
      particleBatch.flush();
    }
    auto worldToScreen = [&](const glm::vec2 &world) {
      glm::vec2 renderLocal = uiWorldCamera.WorldToScreen(world);
      return MapRenderPixelToScreenPoint(outputLayout, renderWidth,
                                         renderHeight, renderLocal);
    };
    auto worldToRenderLocal = [&](const glm::vec2 &world) {
      return uiWorldCamera.WorldToScreen(world);
    };
    auto screenToWorld = [&](const ImVec2 &screen) {
      glm::vec2 renderLocal(0.0f);
      if (!TryMapScreenPointToRenderPixel(outputLayout, screen, renderWidth,
                                          renderHeight, renderLocal)) {
        const float normX =
            std::clamp((screen.x - outputLayout.displayMin.x) /
                           std::max(1.0f, outputLayout.displaySize.x),
                       0.0f, 1.0f);
        const float normY =
            std::clamp((screen.y - outputLayout.displayMin.y) /
                           std::max(1.0f, outputLayout.displaySize.y),
                       0.0f, 1.0f);
        const float sourceU =
            outputLayout.uvMin.x +
            (outputLayout.uvMax.x - outputLayout.uvMin.x) * normX;
        const float sourceV =
            outputLayout.uvMin.y +
            (outputLayout.uvMax.y - outputLayout.uvMin.y) * normY;
        renderLocal.x = sourceU * static_cast<float>(renderWidth);
        renderLocal.y = sourceV * static_cast<float>(renderHeight);
      }
      return uiWorldCamera.ScreenToWorld(renderLocal);
    };
    auto getWorldParentOffset = [&](const SceneObject &obj) {
      return uiSceneLookup.getWorldParentOffset(obj);
    };
    auto getWorldParentRotationRad = [&](const SceneObject &obj) {
      return uiSceneLookup.getWorldParentRotationRad(obj);
    };
    auto parallaxOffset = [&](const SceneObject &obj) {
      if (!obj.hasParallaxLayer2D || !obj.parallaxLayer2D.enabled)
        return glm::vec2(0.0f);
      float factor = std::clamp(obj.parallaxLayer2D.factor, 0.0f, 1.0f);
      return uiWorldCamera.position * (1.0f - factor);
    };
    auto resolveUIRectWorld = [&](const SceneObject &obj, ImVec2 &outMin,
                                  ImVec2 &outMax) {
      if (obj.type == ObjectType::Sprite25D && hasProjectedUiCamera) {
        ImVec2 renderMin;
        ImVec2 renderMax;
        if (!ResolveProjectedSprite25DRect(
                obj, projectedUiView, projectedUiProj, ImVec2(0.0f, 0.0f),
                ImVec2(static_cast<float>(renderWidth),
                       static_cast<float>(renderHeight)),
                renderMin, renderMax)) {
          return false;
        }
        MapRenderRectToScreenRect(outputLayout, renderWidth, renderHeight,
                                  glm::vec2(renderMin.x, renderMin.y),
                                  glm::vec2(renderMax.x, renderMax.y), outMin,
                                  outMax);
        return true;
      }
      glm::vec2 parentOffset = getWorldParentOffset(obj);
      float parentRot = getWorldParentRotationRad(obj);
      glm::vec2 worldPos =
          parentOffset +
          ViewportRenderHelpers::RotateUiVec2(
              glm::vec2(obj.ui.position.x, obj.ui.position.y), parentRot) +
          parallaxOffset(obj);
      glm::vec2 sizeWorld = getSpriteDisplaySize(obj);
      ImVec2 pivotOffset =
          anchorToPivot(obj.ui.anchor, ImVec2(sizeWorld.x, sizeWorld.y));
      glm::vec2 worldMin = worldPos - glm::vec2(pivotOffset.x, pivotOffset.y);
      glm::vec2 worldMax = worldMin + sizeWorld;
      ImVec2 s0 = worldToScreen(worldMin);
      ImVec2 s1 = worldToScreen(worldMax);
      outMin = ImVec2(std::min(s0.x, s1.x), std::min(s0.y, s1.y));
      outMax = ImVec2(std::max(s0.x, s1.x), std::max(s0.y, s1.y));
      return true;
    };
    auto resolveUIRectWorldRender = [&](const SceneObject &obj,
                                        glm::vec2 &outMin, glm::vec2 &outMax) {
      if (obj.type == ObjectType::Sprite25D && hasProjectedUiCamera) {
        ImVec2 renderMin;
        ImVec2 renderMax;
        if (!ResolveProjectedSprite25DRect(
                obj, projectedUiView, projectedUiProj, ImVec2(0.0f, 0.0f),
                ImVec2(static_cast<float>(renderWidth),
                       static_cast<float>(renderHeight)),
                renderMin, renderMax)) {
          return false;
        }
        outMin = glm::vec2(renderMin.x, renderMin.y);
        outMax = glm::vec2(renderMax.x, renderMax.y);
        return true;
      }
      glm::vec2 parentOffset = getWorldParentOffset(obj);
      float parentRot = getWorldParentRotationRad(obj);
      glm::vec2 worldPos =
          parentOffset +
          ViewportRenderHelpers::RotateUiVec2(
              glm::vec2(obj.ui.position.x, obj.ui.position.y), parentRot) +
          parallaxOffset(obj);
      glm::vec2 sizeWorld = getSpriteDisplaySize(obj);
      ImVec2 pivotOffset =
          anchorToPivot(obj.ui.anchor, ImVec2(sizeWorld.x, sizeWorld.y));
      glm::vec2 worldMin = worldPos - glm::vec2(pivotOffset.x, pivotOffset.y);
      glm::vec2 worldMax = worldMin + sizeWorld;
      glm::vec2 s0 = worldToRenderLocal(worldMin);
      glm::vec2 s1 = worldToRenderLocal(worldMax);
      outMin = glm::vec2(std::min(s0.x, s1.x), std::min(s0.y, s1.y));
      outMax = glm::vec2(std::max(s0.x, s1.x), std::max(s0.y, s1.y));
      return true;
    };
    struct CachedUiWorldRect {
      bool screenResolved = false;
      bool screenValid = false;
      ImVec2 screenMin = ImVec2(0.0f, 0.0f);
      ImVec2 screenMax = ImVec2(0.0f, 0.0f);
      bool renderResolved = false;
      bool renderValid = false;
      glm::vec2 renderMin = glm::vec2(0.0f);
      glm::vec2 renderMax = glm::vec2(0.0f);
    };
    std::unordered_map<int, CachedUiWorldRect> cachedUiWorldRects;
    cachedUiWorldRects.reserve(sceneObjects.size());
    auto resolveCachedUiWorldRect = [&](const SceneObject &obj, ImVec2 &outMin,
                                        ImVec2 &outMax) {
      CachedUiWorldRect &cached = cachedUiWorldRects[obj.id];
      if (!cached.screenResolved) {
        cached.screenResolved = true;
        cached.screenValid =
            resolveUIRectWorld(obj, cached.screenMin, cached.screenMax);
      }
      if (!cached.screenValid) {
        return false;
      }
      outMin = cached.screenMin;
      outMax = cached.screenMax;
      return true;
    };
    auto resolveCachedUiWorldRenderRect = [&](const SceneObject &obj,
                                              glm::vec2 &outMin,
                                              glm::vec2 &outMax) {
      CachedUiWorldRect &cached = cachedUiWorldRects[obj.id];
      if (!cached.renderResolved) {
        cached.renderResolved = true;
        cached.renderValid =
            resolveUIRectWorldRender(obj, cached.renderMin, cached.renderMax);
      }
      if (!cached.renderValid) {
        return false;
      }
      outMin = cached.renderMin;
      outMax = cached.renderMax;
      return true;
    };
    auto rectOutsideOverlay = [&](const ImVec2 &min, const ImVec2 &max) {
      return (max.x < overlayPos.x || min.x > overlayPos.x + overlaySize.x ||
              max.y < overlayPos.y || min.y > overlayPos.y + overlaySize.y);
    };

    bool uiWorldHover =
        imageHovered ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    bool uiWorldCameraActive = false;
    if (useWorldUi && allowEditorUi) {
      ImGuiIO &io = ImGui::GetIO();
      bool panHeld =
          uiWorldHover && (ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                           (ImGui::IsKeyDown(ImGuiKey_Space) &&
                            ImGui::IsMouseDown(ImGuiMouseButton_Left)));
      if (panHeld) {
        uiWorldPanning = true;
      } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
                 !(ImGui::IsKeyDown(ImGuiKey_Space) &&
                   ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        uiWorldPanning = false;
      }
      if (uiWorldPanning) {
        ImVec2 delta = io.MouseDelta;
        if (delta.x != 0.0f || delta.y != 0.0f) {
          uiWorldCamera.position.x -= delta.x / uiWorldCamera.zoom;
          uiWorldCamera.position.y += delta.y / uiWorldCamera.zoom;
        }
        uiWorldCameraActive = true;
      }
      if (uiWorldHover && io.MouseWheel != 0.0f) {
        glm::vec2 worldBefore = screenToWorld(io.MousePos);
        float zoomFactor = 1.0f + io.MouseWheel * 0.1f;
        float newZoom =
            std::clamp(uiWorldCamera.zoom * zoomFactor, 5.0f, 2000.0f);
        if (newZoom != uiWorldCamera.zoom) {
          uiWorldCamera.zoom = newZoom;
          glm::vec2 worldAfter = screenToWorld(io.MousePos);
          uiWorldCamera.position += (worldBefore - worldAfter);
          uiWorldCameraActive = true;
        }
      }
      if (uiWorldHover) {
        glm::vec2 panDir(0.0f);
        if (ImGui::IsKeyDown(ImGuiKey_A))
          panDir.x -= 1.0f;
        if (ImGui::IsKeyDown(ImGuiKey_D))
          panDir.x += 1.0f;
        if (ImGui::IsKeyDown(ImGuiKey_W))
          panDir.y += 1.0f;
        if (ImGui::IsKeyDown(ImGuiKey_S))
          panDir.y -= 1.0f;
        if (panDir.x != 0.0f || panDir.y != 0.0f) {
          float panSpeed = 6.0f;
          if (ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
              ImGui::IsKeyDown(ImGuiKey_RightShift)) {
            panSpeed *= 2.5f;
          }
          uiWorldCamera.position += panDir * (panSpeed * deltaTime);
          uiWorldCameraActive = true;
        }
      }
    }
    auto brighten = [](const ImVec4 &c, float k) {
      return ImVec4(std::clamp(c.x * k, 0.0f, 1.0f),
                    std::clamp(c.y * k, 0.0f, 1.0f),
                    std::clamp(c.z * k, 0.0f, 1.0f), c.w);
    };
    float animSpeed = 0.0f;
    if (uiAnimationMode == UIAnimationMode::Fluid) {
      animSpeed = 8.0f;
    } else if (uiAnimationMode == UIAnimationMode::Snappy) {
      animSpeed = 18.0f;
    }
    float animStep =
        (uiAnimationMode == UIAnimationMode::Off)
            ? 1.0f
            : (1.0f - std::exp(-animSpeed * ImGui::GetIO().DeltaTime));
    auto animateValue = [&](float &current, float target, bool immediate) {
      if (uiAnimationMode == UIAnimationMode::Off || immediate) {
        current = target;
      } else {
        current += (target - current) * animStep;
      }
      return current;
    };
    SpriteTextureResolver spriteTextureResolver(rendererInitialized ? &renderer
                                                                    : nullptr);

    std::vector<SceneObject *> uiDrawList;
    uiDrawList.reserve(sceneObjects.size());
    for (auto &obj : sceneObjects) {
      if (!IsObjectEnabledInHierarchy(obj))
        continue;
      if (!isUIType(obj) && !ViewportRenderHelpers::IsAssemblageLayerDrawable(obj))
        continue;
      uiDrawList.push_back(&obj);
    }
    if (uiDrawList.size() > 1) {
      StableSortRuntimeUiDrawList(uiDrawList);
    }
    glm::vec2 worldViewMin =
        useWorldUi ? uiWorldCamera.ScreenToWorld(
                         glm::vec2(0.0f, static_cast<float>(renderHeight)))
                   : glm::vec2(0.0f);
    glm::vec2 worldViewMax =
        useWorldUi ? uiWorldCamera.ScreenToWorld(
                         glm::vec2(static_cast<float>(renderWidth), 0.0f))
                   : glm::vec2(0.0f);
    const auto spriteBatchBuildStart = std::chrono::steady_clock::now();
    BatchedSpriteEmitter spriteBatch(ImGui::GetWindowDrawList());
    auto resolveCanvasMaskRectForObject =
        [&](const SceneObject &obj, ImVec2 &outMin, ImVec2 &outMax) -> bool {
      bool hasMask = false;
      ImVec2 maskMin(0.0f, 0.0f);
      ImVec2 maskMax(0.0f, 0.0f);
      const SceneObject *current = &obj;
      while (current && current->parentId >= 0) {
        current = uiSceneLookup.find(current->parentId);
        if (!current)
          break;
        if (!(current->hasUI && current->ui.type == UIElementType::Canvas &&
              current->ui.maskChildren)) {
          continue;
        }

        ImVec2 canvasMin, canvasMax;
        bool hasCanvasRect = true;
        if (useWorldUi || current->type == ObjectType::Sprite25D) {
          hasCanvasRect =
              resolveCachedUiWorldRect(*current, canvasMin, canvasMax);
        } else {
          resolveUIRect(*current, canvasMin, canvasMax);
        }
        if (!hasCanvasRect) {
          continue;
        }

        if (!hasMask) {
          maskMin = canvasMin;
          maskMax = canvasMax;
          hasMask = true;
        } else {
          maskMin.x = std::max(maskMin.x, canvasMin.x);
          maskMin.y = std::max(maskMin.y, canvasMin.y);
          maskMax.x = std::min(maskMax.x, canvasMax.x);
          maskMax.y = std::min(maskMax.y, canvasMax.y);
        }
      }

      if (!hasMask)
        return false;
      outMin = maskMin;
      outMax = maskMax;
      return (outMax.x > outMin.x) && (outMax.y > outMin.y);
    };

    std::unordered_set<int> light2DRenderedObjectIds;
    bool renderedLight2DComposite = false;
    Light2DDebugStats light2DStats;
    int activeLight2DCount = 0;
    int litSprite2DCount = 0;
    int litWorldImageCount = 0;
    bool lightBufferHadContent = false;
    std::unordered_map<int, std::string> light2DRoutingReasons;
    SceneObject *selectedForRoutingReasons =
        showInspector ? getSelectedObject() : nullptr;
    const bool captureLight2DRoutingReasons =
        selectedForRoutingReasons && selectedForRoutingReasons->hasUI;
    if (captureLight2DRoutingReasons) {
      light2DRoutingReasons.reserve(uiDrawList.size());
    }
    auto setLight2DRoutingReason = [&](int objectId, const char *reason) {
      if (captureLight2DRoutingReasons) {
        light2DRoutingReasons[objectId] = reason;
      }
    };
    if (rendererInitialized) {
      Light2DRenderRequest lightRequest;
      lightRequest.width = std::max(1, renderWidth);
      lightRequest.height = std::max(1, renderHeight);
      lightRequest.clearColor = glm::vec4(0.0f);
      lightRequest.baseAmbient = glm::vec3(0.0f);
      lightRequest.lightingBufferScale = buildSettings.light2DLightingResolution;
      lightRequest.lightingBufferScaleAuto = buildSettings.light2DLightingResolutionAuto;
      lightRequest.lightingPixelFilter = buildSettings.light2DLightingPixelFilter;
      lightRequest.lightingSDR = buildSettings.light2DLightingSDR;
      lightRequest.postFx =
          resolveWorld2DPostFx(makeCameraFromObject(*playerCam));
      lightRequest.blendStyles = light2DBlendStyles;
      auto computeFlickerMultiplier =
          [](const Light2DFlickerSettings &flicker) {
            if (!flicker.enabled || flicker.amount <= 0.0001f) {
              return 1.0f;
            }
            const float time = static_cast<float>(glfwGetTime());
            const float base =
                std::sin(time * std::max(0.01f, flicker.speed) + flicker.seed);
            const float jitter =
                std::sin(time * std::max(0.01f, flicker.speed * 2.173f) +
                         flicker.seed * 1.913f);
            const float noise = 0.5f + 0.35f * base + 0.15f * jitter;
            return glm::mix(1.0f, std::max(0.0f, noise),
                            std::clamp(flicker.amount, 0.0f, 1.0f));
          };

      int spriteDrawOrder = 0;
      assemblageTileQuadsLastFrame = 0;
      for (SceneObject *objPtr : uiDrawList) {
        SceneObject &obj = *objPtr;
        if (ViewportRenderHelpers::IsAssemblageLayerDrawable(obj)) {
          if (useWorldUi) {
            const int layerDrawOrder = spriteDrawOrder++;
            assemblageTileQuadsLastFrame += ViewportRenderHelpers::AppendAssemblageLayerQuads(
                sceneObjects, obj, assemblageRuntime, renderer, glfwGetTime(),
                worldViewMin, worldViewMax, worldToScreen,
                [&](const ViewportRenderHelpers::AssemblageScreenQuad &quad) {
                  Light2DScreenSprite sprite;
                  sprite.objectId = obj.id;
                  sprite.layer = obj.layer;
                  sprite.drawOrder = layerDrawOrder;
                  sprite.textureId = quad.textureId;
                  sprite.tint = quad.tint;
                  sprite.receiveLighting = obj.assemblageLayer.receiveLighting2D;
                  sprite.unlit = obj.assemblageLayer.unlitLighting2D;
                  sprite.emissiveIntensity = obj.assemblageLayer.emissiveLighting2D;
                  for (int corner = 0; corner < 4; ++corner) {
                    sprite.positions[corner] =
                        glm::vec2(quad.pos[corner].x - overlayPos.x,
                                  quad.pos[corner].y - overlayPos.y);
                    sprite.uvs[corner] = glm::vec2(quad.uv[corner].x, quad.uv[corner].y);
                  }
                  lightRequest.sprites.push_back(sprite);
                });
            light2DRenderedObjectIds.insert(obj.id);
          }
          continue;
        }
        if (!(obj.ui.type == UIElementType::Image ||
              obj.ui.type == UIElementType::Sprite2D)) {
          continue;
        }
        if (obj.ui.nineSliceEnabled) {
          setLight2DRoutingReason(obj.id, "Legacy path: nine-slice sprites are "
                                          "not routed through Light2D yet.");
          continue;
        }
        if (obj.ui.unlitLighting2D) {
          setLight2DRoutingReason(obj.id, "Legacy path: Force Unlit keeps this "
                                          "sprite on the legacy 2D renderer.");
          continue;
        }

        const bool repeatX = obj.hasParallaxLayer2D &&
                             obj.parallaxLayer2D.enabled &&
                             obj.parallaxLayer2D.repeatX;
        const bool repeatY = obj.hasParallaxLayer2D &&
                             obj.parallaxLayer2D.enabled &&
                             obj.parallaxLayer2D.repeatY;
        const bool disableCulling = obj.hasParallaxLayer2D &&
                                    obj.parallaxLayer2D.enabled &&
                                    obj.parallaxLayer2D.disableCulling;

        ImVec2 rectMin, rectMax;
        glm::vec2 renderRectMin(0.0f);
        glm::vec2 renderRectMax(0.0f);
        if (!resolveCachedUiWorldRect(obj, rectMin, rectMax) ||
            !resolveCachedUiWorldRenderRect(obj, renderRectMin,
                                            renderRectMax)) {
          setLight2DRoutingReason(
              obj.id, "Legacy path: failed to resolve a world-space sprite "
                      "rect for the active viewport.");
          continue;
        }
        if (!disableCulling && !repeatX && !repeatY &&
            rectOutsideOverlay(rectMin, rectMax)) {
          setLight2DRoutingReason(obj.id, "Skipped Light2D: object is outside "
                                          "the visible 2D world overlay.");
          continue;
        }

        Texture *spriteTex = spriteTextureResolver.resolveTexture(obj);
        if (!spriteTex || spriteTex->GetID() == 0) {
          setLight2DRoutingReason(
              obj.id,
              "Legacy path: no sprite texture is bound for this object.");
          continue;
        }

        std::array<ImVec2, 4> uvQuad = buildSpriteSheetUvs(obj);
        const float angle = getWorldParentRotationRad(obj) +
                            glm::radians(obj.ui.rotation);
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        ImVec2 maskMin, maskMax;
        const bool hasMaskRect =
            resolveCanvasMaskRectForObject(obj, maskMin, maskMax);
        auto appendSpriteQuad = [&](const ImVec2 &screenQuadMin,
                                    const ImVec2 &screenQuadMax,
                                    const glm::vec2 &renderQuadMin,
                                    const glm::vec2 &renderQuadMax) {
          if (!disableCulling &&
              rectOutsideOverlay(screenQuadMin, screenQuadMax)) {
            return false;
          }
          if (hasMaskRect) {
            const bool maskClipsSprite =
                screenQuadMin.x < maskMin.x || screenQuadMax.x > maskMax.x ||
                screenQuadMin.y < maskMin.y || screenQuadMax.y > maskMax.y;
            if (maskClipsSprite) {
              return false;
            }
          }

          Light2DScreenSprite sprite;
          sprite.objectId = obj.id;
          sprite.layer = obj.layer;
          sprite.drawOrder = spriteDrawOrder++;
          sprite.textureId = spriteTex->GetID();
          sprite.tint = obj.ui.color;
          sprite.receiveLighting = obj.ui.receiveLighting2D;
          sprite.unlit = obj.ui.unlitLighting2D;
          sprite.emissiveIntensity = obj.ui.emissiveLighting2D;

          const glm::vec2 center((renderQuadMin.x + renderQuadMax.x) * 0.5f,
                                 (renderQuadMin.y + renderQuadMax.y) * 0.5f);
          const glm::vec2 half(
              std::max(0.5f, (renderQuadMax.x - renderQuadMin.x) * 0.5f),
              std::max(0.5f, (renderQuadMax.y - renderQuadMin.y) * 0.5f));
          auto rotatePoint = [&](float x, float y) {
            return glm::vec2(center.x + x * c - y * s,
                             center.y + x * s + y * c);
          };
          sprite.positions[0] = rotatePoint(-half.x, -half.y);
          sprite.positions[1] = rotatePoint(half.x, -half.y);
          sprite.positions[2] = rotatePoint(half.x, half.y);
          sprite.positions[3] = rotatePoint(-half.x, half.y);
          sprite.uvs[0] = glm::vec2(uvQuad[0].x, uvQuad[0].y);
          sprite.uvs[1] = glm::vec2(uvQuad[1].x, uvQuad[1].y);
          sprite.uvs[2] = glm::vec2(uvQuad[2].x, uvQuad[2].y);
          sprite.uvs[3] = glm::vec2(uvQuad[3].x, uvQuad[3].y);
          lightRequest.sprites.push_back(sprite);
          return true;
        };

        bool addedAnySprite = false;
        if (repeatX || repeatY) {
          glm::vec2 spriteSizeWorld = getSpriteDisplaySize(obj);
          glm::vec2 spacing = obj.hasParallaxLayer2D
                                  ? obj.parallaxLayer2D.repeatSpacing
                                  : glm::vec2(0.0f);
          float stepX = spriteSizeWorld.x + spacing.x;
          float stepY = spriteSizeWorld.y + spacing.y;
          ImVec2 pivotOffset(spriteSizeWorld.x * 0.5f,
                             spriteSizeWorld.y * 0.5f);
          switch (obj.ui.anchor) {
          case UIAnchor::TopLeft:
            pivotOffset = ImVec2(0.0f, 0.0f);
            break;
          case UIAnchor::TopRight:
            pivotOffset = ImVec2(spriteSizeWorld.x, 0.0f);
            break;
          case UIAnchor::BottomLeft:
            pivotOffset = ImVec2(0.0f, spriteSizeWorld.y);
            break;
          case UIAnchor::BottomRight:
            pivotOffset = ImVec2(spriteSizeWorld.x, spriteSizeWorld.y);
            break;
          default:
            break;
          }
          glm::vec2 parentOffset = getWorldParentOffset(obj);
          glm::vec2 worldPos = parentOffset +
                               glm::vec2(obj.ui.position.x, obj.ui.position.y) +
                               parallaxOffset(obj);
          glm::vec2 baseWorldMin =
              worldPos - glm::vec2(pivotOffset.x, pivotOffset.y);
          int startX = repeatX
                           ? static_cast<int>(std::floor(
                                 (worldViewMin.x - baseWorldMin.x) / stepX)) -
                                 1
                           : 0;
          int endX = repeatX ? static_cast<int>(std::ceil(
                                   (worldViewMax.x - baseWorldMin.x) / stepX)) +
                                   1
                             : 0;
          int startY = repeatY
                           ? static_cast<int>(std::floor(
                                 (worldViewMin.y - baseWorldMin.y) / stepY)) -
                                 1
                           : 0;
          int endY = repeatY ? static_cast<int>(std::ceil(
                                   (worldViewMax.y - baseWorldMin.y) / stepY)) +
                                   1
                             : 0;
          for (int ix = startX; ix <= endX; ++ix) {
            for (int iy = startY; iy <= endY; ++iy) {
              float dx = repeatX ? static_cast<float>(ix) * stepX : 0.0f;
              float dy = repeatY ? static_cast<float>(iy) * stepY : 0.0f;
              glm::vec2 tileMin = baseWorldMin + glm::vec2(dx, dy);
              ImVec2 s0 = worldToScreen(tileMin);
              ImVec2 s1 = worldToScreen(
                  tileMin + glm::vec2(spriteSizeWorld.x, spriteSizeWorld.y));
              glm::vec2 r0 = worldToRenderLocal(tileMin);
              glm::vec2 r1 = worldToRenderLocal(
                  tileMin + glm::vec2(spriteSizeWorld.x, spriteSizeWorld.y));
              ImVec2 tileRectMin(std::min(s0.x, s1.x), std::min(s0.y, s1.y));
              ImVec2 tileRectMax(std::max(s0.x, s1.x), std::max(s0.y, s1.y));
              glm::vec2 renderTileRectMin(std::min(r0.x, r1.x),
                                          std::min(r0.y, r1.y));
              glm::vec2 renderTileRectMax(std::max(r0.x, r1.x),
                                          std::max(r0.y, r1.y));
              addedAnySprite =
                  appendSpriteQuad(tileRectMin, tileRectMax, renderTileRectMin,
                                   renderTileRectMax) ||
                  addedAnySprite;
            }
          }
        } else {
          addedAnySprite =
              appendSpriteQuad(rectMin, rectMax, renderRectMin, renderRectMax);
        }

        if (!addedAnySprite) {
          setLight2DRoutingReason(
              obj.id,
              hasMaskRect
                  ? "Legacy path: repeating or masked tiles still use legacy "
                    "rendering when the canvas clip cuts the visible tile."
                  : "Skipped Light2D: object has no visible tiles inside the "
                    "current 2D world overlay.");
          continue;
        }

        light2DRenderedObjectIds.insert(obj.id);
        if (obj.ui.type == UIElementType::Sprite2D) {
          if (obj.ui.receiveLighting2D && !obj.ui.unlitLighting2D) {
            ++litSprite2DCount;
          }
        } else if (obj.ui.receiveLighting2D && !obj.ui.unlitLighting2D) {
          ++litWorldImageCount;
        }
        if (obj.ui.receiveLighting2D && !obj.ui.unlitLighting2D) {
          setLight2DRoutingReason(
              obj.id, repeatX || repeatY
                          ? "Lit path: repeating parallax tiles are routed "
                            "through the Light2D compositor."
                          : "Lit path: routed through the Light2D compositor.");
        } else if (obj.ui.unlitLighting2D) {
          setLight2DRoutingReason(obj.id,
                                  "Lit compositor path: object is routed, but "
                                  "Force Unlit is enabled.");
        } else {
          setLight2DRoutingReason(obj.id,
                                  "Lit compositor path: object is routed, but "
                                  "Receive Lighting is disabled.");
        }
      }

      if (useWorldUi || !hasProjectedUiCamera) {
        AppendParticleSystem2DSprites(sceneObjects, renderer, glfwGetTime(),
                                      worldToScreen, worldToRenderLocal,
                                      getWorldParentOffset, rectOutsideOverlay,
                                      lightRequest, spriteDrawOrder);
      }

      for (const SceneObject &obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasLight2D ||
            !obj.light2D.enabled) {
          continue;
        }
        ++activeLight2DCount;

        if (obj.light2D.type == Light2DType::Global) {
          lightRequest.baseAmbient +=
              glm::vec3(obj.light2D.color) * obj.light2D.intensity;
          continue;
        }

        Light2DScreenLight light;
        light.objectId = obj.id;
        light.enabled = obj.light2D.enabled;
        light.type = obj.light2D.type;
        light.blendStyle = obj.light2D.blendStyle;
        light.lightOrder = obj.light2D.lightOrder;
        light.overlapOperation = obj.light2D.overlapOperation;
        light.targetAllLayers = obj.light2D.targetAllLayers;
        light.targetLayerMask = obj.light2D.targetLayerMask;
        light.cullWhenOffscreen = obj.light2D.cullWhenOffscreen;
        light.offscreenCullMarginPx =
            obj.light2D.offscreenCullMargin * uiWorldCamera.zoom;
        light.color = obj.light2D.color;
        light.intensity = obj.light2D.intensity *
                          computeFlickerMultiplier(obj.light2D.flicker);
        light.radius = std::max(obj.light2D.radius, obj.light2D.outerRadius) *
                       uiWorldCamera.zoom;
        light.innerRadius = obj.light2D.innerRadius * uiWorldCamera.zoom;
        light.outerRadius =
            std::max(obj.light2D.innerRadius, obj.light2D.outerRadius) *
            uiWorldCamera.zoom;
        light.falloffStrength = obj.light2D.falloffStrength;
        light.innerSpotAngle = obj.light2D.innerSpotAngle;
        light.outerSpotAngle = obj.light2D.outerSpotAngle;
        light.shadowStrength = obj.light2D.shadowStrength;
        light.volumetricEnabled = obj.light2D.volumetricEnabled;
        light.castsShadows = obj.light2D.castsShadows;
        light.rotationRad = glm::radians(obj.rotation.z);
        light.cookieScale = obj.light2D.cookieScale;
        light.cookieRotationRad = glm::radians(obj.light2D.cookieRotation);
        light.freeformFeatherPx =
            obj.light2D.freeformFeather * uiWorldCamera.zoom;
        light.freeformEdgeFalloff = obj.light2D.freeformEdgeFalloff;
        if (!obj.light2D.cookieTexturePath.empty()) {
          if (Texture *cookieTexture = renderer.getTexture(
                  obj.light2D.cookieTexturePath,
                  MaterialProperties::TextureFilter::Bilinear)) {
            light.cookieTextureId = cookieTexture->GetID();
          }
        }

        glm::vec2 lightPos =
            worldToRenderLocal(glm::vec2(obj.position.x, obj.position.y));
        light.position = lightPos;

        if (obj.light2D.type == Light2DType::Freeform ||
            obj.light2D.type == Light2DType::Sprite) {
          light.polygon.reserve(obj.light2D.shapePoints.size());
          for (const glm::vec2 &point : obj.light2D.shapePoints) {
            glm::vec2 renderPoint = worldToRenderLocal(
                glm::vec2(obj.position.x + point.x, obj.position.y + point.y));
            light.polygon.emplace_back(renderPoint.x, renderPoint.y);
          }
          if (!light.polygon.empty()) {
            glm::vec2 boundsMin(FLT_MAX);
            glm::vec2 boundsMax(-FLT_MAX);
            for (const glm::vec2 &point : light.polygon) {
              boundsMin.x = std::min(boundsMin.x, point.x);
              boundsMin.y = std::min(boundsMin.y, point.y);
              boundsMax.x = std::max(boundsMax.x, point.x);
              boundsMax.y = std::max(boundsMax.y, point.y);
            }
            light.boundsMin = boundsMin;
            light.boundsMax = boundsMax;
          }
        } else {
          const float extent = std::max(light.radius, light.outerRadius);
          light.boundsMin = light.position - glm::vec2(extent);
          light.boundsMax = light.position + glm::vec2(extent);
        }

        lightRequest.lights.push_back(light);
      }

      for (const SceneObject &obj : sceneObjects) {
        if (!IsObjectEnabledInHierarchy(obj) || !obj.hasShadowCaster2D ||
            !obj.shadowCaster2D.enabled) {
          continue;
        }

        Light2DScreenShadowCaster caster;
        caster.objectId = obj.id;
        caster.enabled = obj.shadowCaster2D.enabled;
        caster.targetAllLayers = obj.shadowCaster2D.targetAllLayers;
        caster.targetLayerMask = obj.shadowCaster2D.targetLayerMask;
        caster.shadowStrength = obj.shadowCaster2D.shadowStrength;
        caster.polygon.reserve(obj.shadowCaster2D.points.size());
        for (const glm::vec2 &point : obj.shadowCaster2D.points) {
          glm::vec2 renderPoint = worldToRenderLocal(
              glm::vec2(obj.position.x + point.x, obj.position.y + point.y));
          caster.polygon.emplace_back(renderPoint.x, renderPoint.y);
        }
        if (caster.polygon.size() >= 3) {
          lightRequest.shadowCasters.push_back(std::move(caster));
        }
      }

      const bool hasAmbientOnly =
          glm::length(lightRequest.baseAmbient) > 0.0001f;
      const bool wantsPostFxComposite =
          Light2DHasVisiblePostFx(lightRequest.postFx);
      lightBufferHadContent = hasAmbientOnly || !lightRequest.lights.empty();
      if (!lightRequest.sprites.empty() &&
          (hasAmbientOnly || !lightRequest.lights.empty() ||
           wantsPostFxComposite)) {
        unsigned int lightTexture =
            lighting2DRenderer.render(lightRequest, renderer);
        if (lightTexture != 0) {
          unsigned int presentedTexture = lightTexture;
          ImGui::GetWindowDrawList()->AddImage(
              (ImTextureID)(intptr_t)presentedTexture, outputLayout.displayMin,
              outputLayout.displayMax,
              ImVec2(outputLayout.uvMin.x, 1.0f - outputLayout.uvMin.y),
              ImVec2(outputLayout.uvMax.x, 1.0f - outputLayout.uvMax.y));
          renderedLight2DComposite = true;
          light2DStats = lighting2DRenderer.getLastStats();
        } else {
          for (int objectId : light2DRenderedObjectIds) {
            setLight2DRoutingReason(
                objectId, "Legacy path: Light2D compositor did not produce a "
                          "valid output texture this frame.");
          }
          light2DRenderedObjectIds.clear();
        }
      } else {
        for (int objectId : light2DRenderedObjectIds) {
          setLight2DRoutingReason(
              objectId,
              wantsPostFxComposite
                  ? "Legacy path: the Light2D compositor was expected to run "
                    "for 2D post FX, but no output was requested."
                  : "Legacy path: no active Light2D or Global Light2D affected "
                    "this frame.");
        }
        light2DRenderedObjectIds.clear();
      }
    }

    const ImVec2 postFxMin(
        std::max(outputLayout.displayMin.x, outputLayout.panelMin.x),
        std::max(outputLayout.displayMin.y, outputLayout.panelMin.y));
    const ImVec2 postFxMax(
        std::min(outputLayout.displayMax.x, outputLayout.panelMax.x),
        std::min(outputLayout.displayMax.y, outputLayout.panelMax.y));

    const Renderer::PostFX2DScope postFx2DScope =
        rendererInitialized
            ? renderer.resolvePostFX2DScope(makeCameraFromObject(*playerCam),
                                            sceneObjects)
            : Renderer::PostFX2DScope{};
    bool postFx2DBandOpen = false;
    bool postFx2DScopeResolved = false;
    auto emitPostFx2DScopeBoundary = [&](int sortOrder) {
      if (!postFx2DScope.active || postFx2DScopeResolved) {
        return;
      }
      ImDrawList *dl = ImGui::GetWindowDrawList();
      const Camera effectCamera = makeCameraFromObject(*playerCam);
      if (postFx2DScope.mode == PostFX2DScopeMode::Band) {
        if (!postFx2DBandOpen) {
          if (sortOrder >= postFx2DScope.minOrder &&
              sortOrder <= postFx2DScope.maxOrder) {
            spriteBatch.flush();
            queueOverlayPostFx(dl, effectCamera, postFxMin, postFxMax, false,
                               OverlayPostFxKind::BandBegin);
            postFx2DBandOpen = true;
          }
          return;
        }
        if (sortOrder > postFx2DScope.maxOrder) {
          spriteBatch.flush();
          queueOverlayPostFx(dl, effectCamera, postFxMin, postFxMax, false,
                             OverlayPostFxKind::BandEnd);
          postFx2DBandOpen = false;
          postFx2DScopeResolved = true;
        }
        return;
      }
      if (sortOrder > postFx2DScope.maxOrder) {
        spriteBatch.flush();
        queueOverlayPostFx(dl, effectCamera, postFxMin, postFxMax, false,
                           OverlayPostFxKind::FullRegion);
        postFx2DScopeResolved = true;
      }
    };

    for (SceneObject *objPtr : uiDrawList) {
      SceneObject &obj = *objPtr;
      emitPostFx2DScopeBoundary(RuntimeUiSortOrderOf(obj));
      if (ViewportRenderHelpers::IsAssemblageLayerDrawable(obj)) {
        if (useWorldUi && light2DRenderedObjectIds.count(obj.id) == 0) {
          assemblageTileQuadsLastFrame += ViewportRenderHelpers::AppendAssemblageLayerQuads(
              sceneObjects, obj, assemblageRuntime, renderer, glfwGetTime(), worldViewMin,
              worldViewMax, worldToScreen,
              [&](const ViewportRenderHelpers::AssemblageScreenQuad &quad) {
                spriteBatch.push(quad.textureId, quad.pos[0], quad.pos[1], quad.pos[2],
                                 quad.pos[3], quad.uv[0], quad.uv[1], quad.uv[2], quad.uv[3],
                                 ImGui::GetColorU32(ImVec4(quad.tint.r, quad.tint.g,
                                                           quad.tint.b, quad.tint.a)));
              });
        }
        continue;
      }
      ImVec2 rectMin, rectMax;
      if (useWorldUi || obj.type == ObjectType::Sprite25D) {
        if (!resolveCachedUiWorldRect(obj, rectMin, rectMax))
          continue;
      } else {
        resolveUIRect(obj, rectMin, rectMax);
      }
      const float elementRotRad =
          uiSceneLookup.getWorldParentRotationRad(obj) +
          glm::radians(obj.ui.rotation);
      ImVec2 rectSize(rectMax.x - rectMin.x, rectMax.y - rectMin.y);
      if (rectSize.x <= 1.0f || rectSize.y <= 1.0f)
        continue;
      ++runtimeVisibleObjectCount;
      const bool disableCulling = obj.hasParallaxLayer2D &&
                                  obj.parallaxLayer2D.enabled &&
                                  obj.parallaxLayer2D.disableCulling;
      if (!disableCulling && rectOutsideOverlay(rectMin, rectMax))
        continue;

      ImGuiStyle savedStyle = ImGui::GetStyle();
      bool styleApplied = false;
      bool fontApplied = false;
      if (!obj.ui.stylePreset.empty()) {
        if (const auto *preset = getUIStylePreset(obj.ui.stylePreset)) {
          ImGui::GetStyle() = preset->style;
          styleApplied = true;
          if (ImFont *presetFont =
                  getUIFontForContext(preset->fontAsset, ImGui::GetCurrentContext())) {
            ImGui::PushFont(presetFont, preset->style.FontSizeBase);
            fontApplied = true;
          }
        }
      }

      if (obj.ui.type == UIElementType::Canvas) {
        spriteBatch.flush();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImU32 edgeColor = obj.ui.maskChildren
                                    ? IM_COL32(74, 228, 255, 225)
                                    : IM_COL32(110, 170, 255, 140);
        const float thickness = obj.ui.maskChildren ? 2.4f : 1.5f;
        dl->AddRect(rectMin, rectMax, edgeColor, 6.0f, 0, thickness);
        if (obj.ui.maskChildren) {
          const float inset = 2.0f;
          if ((rectMax.x - rectMin.x) > inset * 2.0f &&
              (rectMax.y - rectMin.y) > inset * 2.0f) {
            dl->AddRect(ImVec2(rectMin.x + inset, rectMin.y + inset),
                        ImVec2(rectMax.x - inset, rectMax.y - inset),
                        IM_COL32(32, 190, 230, 175), 5.0f, 0, 1.0f);
          }
        }
        if (fontApplied)
          ImGui::PopFont();
        if (styleApplied)
          ImGui::GetStyle() = savedStyle;
        continue;
      }

      ImVec2 drawMin = rectMin;
      ImVec2 drawMax = rectMax;
      ImVec2 drawSize(drawMax.x - drawMin.x, drawMax.y - drawMin.y);
      ImVec2 localMin(drawMin.x - overlayPos.x, drawMin.y - overlayPos.y);
      bool pushedCanvasMask = false;
      if (obj.ui.type != UIElementType::Canvas) {
        ImVec2 maskMin, maskMax;
        if (resolveCanvasMaskRectForObject(obj, maskMin, maskMax)) {
          maskMin.x = std::max(maskMin.x, overlayPos.x);
          maskMin.y = std::max(maskMin.y, overlayPos.y);
          maskMax.x = std::min(maskMax.x, overlayPos.x + overlaySize.x);
          maskMax.y = std::min(maskMax.y, overlayPos.y + overlaySize.y);
          if (maskMax.x <= maskMin.x || maskMax.y <= maskMin.y) {
            if (fontApplied)
              ImGui::PopFont();
            if (styleApplied)
              ImGui::GetStyle() = savedStyle;
            continue;
          }
          if (drawMax.x <= maskMin.x || drawMin.x >= maskMax.x ||
              drawMax.y <= maskMin.y || drawMin.y >= maskMax.y) {
            if (fontApplied)
              ImGui::PopFont();
            if (styleApplied)
              ImGui::GetStyle() = savedStyle;
            continue;
          }
          spriteBatch.flush();
          ImGui::PushClipRect(maskMin, maskMax, true);
          pushedCanvasMask = true;
        }
      }
      ImGui::PushID(obj.id);
      UIAnimationState &animState = uiAnimationStates[obj.id];
      if (!animState.initialized) {
        animState.sliderValue = obj.ui.sliderValue;
        animState.initialized = true;
      }
      if (obj.ui.backdropBlur > 0.001f) {
        spriteBatch.flush();
        EmitRuntimeUiBackdropBlur(ImGui::GetWindowDrawList(), obj, drawMin, drawMax);
      }
      if (obj.ui.type == UIElementType::Image ||
          obj.ui.type == UIElementType::Sprite2D) {
        if (light2DRenderedObjectIds.find(obj.id) !=
            light2DRenderedObjectIds.end()) {
          if (pushedCanvasMask) {
            ImGui::PopClipRect();
          }
          ImGui::PopID();
          if (fontApplied)
            ImGui::PopFont();
          if (styleApplied)
            ImGui::GetStyle() = savedStyle;
          continue;
        }
        Texture *spriteTex = nullptr;
        unsigned int texId = 0;
        if (rendererInitialized && !obj.albedoTexturePath.empty()) {
          spriteTex = renderer.getTexture(
              obj.albedoTexturePath, MaterialProperties::TextureFilter::Point);
          if (spriteTex != nullptr) {
            texId = spriteTex->GetID();
          }
        }
        std::array<ImVec2, 4> uvQuad = buildSpriteSheetUvs(obj);
        const int frame = resolveSpriteSheetFrame(obj);
        const ImVec2 sourceFrameSizePx =
            ResolveUiSourceFrameSizePx(obj, frame, spriteTex);
        ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b,
                    obj.ui.color.a);
        const ImU32 tintColor = ImGui::GetColorU32(tint);
        bool repeatX = useWorldUi && obj.hasParallaxLayer2D &&
                       obj.parallaxLayer2D.enabled &&
                       obj.parallaxLayer2D.repeatX;
        bool repeatY = useWorldUi && obj.hasParallaxLayer2D &&
                       obj.parallaxLayer2D.enabled &&
                       obj.parallaxLayer2D.repeatY;
        glm::vec2 spacing = obj.hasParallaxLayer2D
                                ? obj.parallaxLayer2D.repeatSpacing
                                : glm::vec2(0.0f);
        glm::vec2 spriteSizeWorld = getSpriteDisplaySize(obj);
        float stepX = spriteSizeWorld.x + spacing.x;
        float stepY = spriteSizeWorld.y + spacing.y;
        glm::vec2 baseWorldMin = worldViewMin;
        if (repeatX || repeatY) {
          glm::vec2 sizeWorld = spriteSizeWorld;
          ImVec2 pivotOffset =
              anchorToPivot(obj.ui.anchor, ImVec2(sizeWorld.x, sizeWorld.y));
          glm::vec2 parentOffset = getWorldParentOffset(obj);
          glm::vec2 worldPos = parentOffset +
                               glm::vec2(obj.ui.position.x, obj.ui.position.y) +
                               parallaxOffset(obj);
          baseWorldMin = worldPos - glm::vec2(pivotOffset.x, pivotOffset.y);
        }
        float angle = elementRotRad;
        auto drawImageRect = [&](const ImVec2 &min, const ImVec2 &max) {
          ImVec2 size(max.x - min.x, max.y - min.y);
          if (size.x <= 1.0f || size.y <= 1.0f)
            return;
          if (DrawScrollingUiSprite(spriteBatch, (ImTextureID)(intptr_t)texId,
                                    obj, min, max, angle, tintColor,
                                    ImGui::GetTime())) {
            return;
          }
          if (DrawNineSliceSprite(spriteBatch, (ImTextureID)(intptr_t)texId,
                                  obj, min, max, uvQuad, sourceFrameSizePx,
                                  angle, tintColor)) {
            return;
          }
          if (std::abs(angle) > 1e-4f) {
            ImVec2 center =
                ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
            ImVec2 half = ImVec2(size.x * 0.5f, size.y * 0.5f);
            ImVec2 p0, p1, p2, p3;
            ViewportRenderHelpers::UiRotatedCornersAboutAnchor(
                obj.ui.anchor, center, half, angle, p0, p1, p2, p3);
            if (texId != 0) {
              spriteBatch.push((ImTextureID)(intptr_t)texId, p0, p1, p2, p3,
                               uvQuad[0], uvQuad[1], uvQuad[2], uvQuad[3],
                               tintColor);
            } else {
              spriteBatch.flush();
              ImDrawList *dl = ImGui::GetWindowDrawList();
              ImU32 fill = tintColor;
              ImU32 border = ImGui::GetColorU32(brighten(tint, 0.85f));
              dl->AddQuadFilled(p0, p1, p2, p3, fill);
              dl->AddQuad(p0, p1, p2, p3, border, 2.0f);
              ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
              ImVec2 textPos(center.x - textSize.x * 0.5f,
                             center.y - textSize.y * 0.5f);
              dl->AddText(textPos, IM_COL32(210, 210, 220, 220),
                          obj.ui.label.c_str());
            }
          } else {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (texId != 0) {
              spriteBatch.push((ImTextureID)(intptr_t)texId, min,
                               ImVec2(max.x, min.y), max, ImVec2(min.x, max.y),
                               uvQuad[0], ImVec2(uvQuad[2].x, uvQuad[0].y),
                               uvQuad[2], ImVec2(uvQuad[0].x, uvQuad[2].y),
                               tintColor);
            } else {
              spriteBatch.flush();
              ImU32 fill = tintColor;
              ImU32 border = ImGui::GetColorU32(brighten(tint, 0.85f));
              dl->AddRectFilled(min, max, fill, 6.0f);
              dl->AddRect(min, max, border, 6.0f);
              ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
              ImVec2 textPos(min.x + (size.x - textSize.x) * 0.5f,
                             min.y + (size.y - textSize.y) * 0.5f);
              dl->AddText(textPos, IM_COL32(210, 210, 220, 220),
                          obj.ui.label.c_str());
            }
          }
        };

        if (repeatX || repeatY) {
          int startX = repeatX
                           ? static_cast<int>(std::floor(
                                 (worldViewMin.x - baseWorldMin.x) / stepX)) -
                                 1
                           : 0;
          int endX = repeatX ? static_cast<int>(std::ceil(
                                   (worldViewMax.x - baseWorldMin.x) / stepX)) +
                                   1
                             : 0;
          int startY = repeatY
                           ? static_cast<int>(std::floor(
                                 (worldViewMin.y - baseWorldMin.y) / stepY)) -
                                 1
                           : 0;
          int endY = repeatY ? static_cast<int>(std::ceil(
                                   (worldViewMax.y - baseWorldMin.y) / stepY)) +
                                   1
                             : 0;
          for (int ix = startX; ix <= endX; ++ix) {
            for (int iy = startY; iy <= endY; ++iy) {
              float dx = repeatX ? (float)ix * stepX : 0.0f;
              float dy = repeatY ? (float)iy * stepY : 0.0f;
              glm::vec2 tileMin = baseWorldMin + glm::vec2(dx, dy);
              ImVec2 s0 = worldToScreen(tileMin);
              ImVec2 s1 = worldToScreen(
                  tileMin + glm::vec2(spriteSizeWorld.x, spriteSizeWorld.y));
              ImVec2 tMin(std::min(s0.x, s1.x), std::min(s0.y, s1.y));
              ImVec2 tMax(std::max(s0.x, s1.x), std::max(s0.y, s1.y));
              drawImageRect(tMin, tMax);
            }
          }
        } else {
          drawImageRect(drawMin, drawMax);
        }
      } else if (obj.ui.type == UIElementType::Slider) {
        spriteBatch.flush();
        const int sliderVtxStart = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b,
                    obj.ui.color.a);
        const bool uiWidgetInteractive =
            isPlaying && !uiWorldCameraActive && obj.ui.interactable;
        if (uiWidgetInteractive) {
          ImGui::SetCursorPos(localMin);
        }
        const ImU32 sliderBg     = (obj.ui.backgroundColor.a > 0.0f) ? ImGui::GetColorU32(ImVec4(obj.ui.backgroundColor.r, obj.ui.backgroundColor.g, obj.ui.backgroundColor.b, obj.ui.backgroundColor.a)) : ImGui::GetColorU32(ImVec4(tint.x * 0.2f, tint.y * 0.2f, tint.z * 0.2f, tint.w * 0.6f));
        const ImU32 sliderFill   = (obj.ui.fillColor.a > 0.0f)       ? ImGui::GetColorU32(ImVec4(obj.ui.fillColor.r, obj.ui.fillColor.g, obj.ui.fillColor.b, obj.ui.fillColor.a))             : ImGui::GetColorU32(tint);
        const ImU32 sliderBorder = (obj.ui.borderColor.a > 0.0f)     ? ImGui::GetColorU32(ImVec4(obj.ui.borderColor.r, obj.ui.borderColor.g, obj.ui.borderColor.b, obj.ui.borderColor.a))     : ImGui::GetColorU32(brighten(tint, 0.85f));
        const ImU32 sliderText   = (obj.ui.textColor.a > 0.0f)       ? ImGui::GetColorU32(ImVec4(obj.ui.textColor.r, obj.ui.textColor.g, obj.ui.textColor.b, obj.ui.textColor.a))             : IM_COL32(240, 240, 245, 220);
        if (obj.ui.sliderStyle == UISliderStyle::ImGui) {
          float minValue = obj.ui.sliderMin;
          float maxValue = obj.ui.sliderMax;
          float range = (maxValue - minValue);
          if (range <= 1e-6f)
            range = 1.0f;
          ImDrawList *dl = ImGui::GetWindowDrawList();
          if (uiWidgetInteractive) {
            ImGui::InvisibleButton("##UISliderImGui", drawSize);
            if (ImGui::IsItemActive() &&
                ImGui::IsMouseDown(ImGuiMouseButton_Left) && drawSize.x > 1.0f) {
              float mouseT = (ImGui::GetIO().MousePos.x - drawMin.x) / drawSize.x;
              mouseT = std::clamp(mouseT, 0.0f, 1.0f);
              float newValue = minValue + mouseT * range;
              if (newValue != obj.ui.sliderValue) {
                obj.ui.sliderValue = newValue;
                projectManager.currentProject.hasUnsavedChanges = true;
              }
            }
          }
          float t = (obj.ui.sliderValue - minValue) / range;
          t = std::clamp(t, 0.0f, 1.0f);
          float rounding = std::min(drawSize.x, drawSize.y) * 0.5f;
          ImVec2 fillMax(drawMin.x + drawSize.x * t, drawMax.y);
          dl->AddRectFilled(drawMin, drawMax, sliderBg, rounding);
          if (fillMax.x > drawMin.x) {
            dl->AddRectFilled(drawMin, fillMax, sliderFill, rounding);
          }
          dl->AddRect(drawMin, drawMax, sliderBorder, rounding);
          ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
          ImVec2 textPos(drawMin.x + (drawSize.x - textSize.x) * 0.5f,
                         drawMin.y + (drawSize.y - textSize.y) * 0.5f);
          dl->AddText(textPos, sliderText, obj.ui.label.c_str());
        } else {
          ImDrawList *dl = ImGui::GetWindowDrawList();
          ImU32 bg     = sliderBg;
          ImU32 fill   = sliderFill;
          ImU32 border = sliderBorder;
          float minValue = obj.ui.sliderMin;
          float maxValue = obj.ui.sliderMax;
          float range = (maxValue - minValue);
          if (range <= 1e-6f)
            range = 1.0f;
          bool held = false;
          if (uiWidgetInteractive) {
            ImGui::InvisibleButton("##UISlider", drawSize);
            held = ImGui::IsItemActive();
          }
          if (held && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
              drawSize.x > 1.0f) {
            float mouseT = (ImGui::GetIO().MousePos.x - drawMin.x) / drawSize.x;
            mouseT = std::clamp(mouseT, 0.0f, 1.0f);
            float newValue = minValue + mouseT * range;
            if (newValue != obj.ui.sliderValue) {
              obj.ui.sliderValue = newValue;
              projectManager.currentProject.hasUnsavedChanges = true;
            }
          }

          animateValue(animState.sliderValue, obj.ui.sliderValue, held);
          float displayValue = (uiAnimationMode == UIAnimationMode::Off)
                                   ? obj.ui.sliderValue
                                   : animState.sliderValue;
          float t = (displayValue - minValue) / range;
          t = std::clamp(t, 0.0f, 1.0f);

          ViewportRenderHelpers::RenderUISliderStyle(
              dl, obj.ui.sliderStyle, drawMin, drawMax, drawSize, bg, fill,
              border, sliderText, t, minValue, maxValue, obj.ui.label.c_str());
        }
        if (std::abs(elementRotRad) > 1e-4f) {
          const ImVec2 sliderPivotOffset = ViewportRenderHelpers::UiAnchorPivot(
              obj.ui.anchor,
              ImVec2(drawMax.x - drawMin.x, drawMax.y - drawMin.y));
          const ImVec2 sliderPivot(drawMin.x + sliderPivotOffset.x,
                                   drawMin.y + sliderPivotOffset.y);
          ViewportRenderHelpers::RotateDrawListVertices(
              ImGui::GetWindowDrawList(), sliderVtxStart,
              ImGui::GetWindowDrawList()->VtxBuffer.Size, sliderPivot,
              elementRotRad);
        }
      } else if (obj.ui.type == UIElementType::Button) {
        spriteBatch.flush();
        ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b,
                    obj.ui.color.a);
        obj.ui.buttonPressed = false;
        obj.ui.uiHovered = false;
        obj.ui.uiActive = false;
        const bool uiWidgetInteractive =
            isPlaying && !uiWorldCameraActive && obj.ui.interactable;
        if (uiWidgetInteractive) {
          ImGui::SetCursorPos(localMin);
        }
        const ImU32 uiTextCol = (obj.ui.textColor.a > 0.0f)
            ? ImGui::GetColorU32(ImVec4(obj.ui.textColor.r, obj.ui.textColor.g, obj.ui.textColor.b, obj.ui.textColor.a))
            : IM_COL32(240, 240, 245, 220);
        if (obj.ui.buttonStyle == UIButtonStyle::ImGui) {
          if (uiWidgetInteractive) {
            ImGui::PushStyleColor(ImGuiCol_Button, tint);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, brighten(tint, 1.1f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, brighten(tint, 1.2f));
            obj.ui.buttonPressed =
                ImGui::Button(obj.ui.label.c_str(), drawSize);
            obj.ui.uiHovered = ImGui::IsItemHovered();
            obj.ui.uiActive  = ImGui::IsItemActive();
            ImGui::PopStyleColor(3);
          } else {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImU32 fill   = (obj.ui.fillColor.a > 0.0f) ? ImGui::GetColorU32(ImVec4(obj.ui.fillColor.r, obj.ui.fillColor.g, obj.ui.fillColor.b, obj.ui.fillColor.a)) : ImGui::GetColorU32(tint);
            ImU32 border = (obj.ui.borderColor.a > 0.0f) ? ImGui::GetColorU32(ImVec4(obj.ui.borderColor.r, obj.ui.borderColor.g, obj.ui.borderColor.b, obj.ui.borderColor.a)) : ImGui::GetColorU32(brighten(tint, 0.85f));
            dl->AddRectFilled(drawMin, drawMax, fill, 6.0f);
            dl->AddRect(drawMin, drawMax, border, 6.0f);
            ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
            ImVec2 textPos(drawMin.x + (drawSize.x - textSize.x) * 0.5f,
                           drawMin.y + (drawSize.y - textSize.y) * 0.5f);
            dl->AddText(textPos, uiTextCol, obj.ui.label.c_str());
          }
        } else if (obj.ui.buttonStyle == UIButtonStyle::Outline) {
          ImDrawList *dl = ImGui::GetWindowDrawList();
          ImU32 border = (obj.ui.borderColor.a > 0.0f) ? ImGui::GetColorU32(ImVec4(obj.ui.borderColor.r, obj.ui.borderColor.g, obj.ui.borderColor.b, obj.ui.borderColor.a)) : ImGui::GetColorU32(tint);
          bool hovered = false;
          bool active = false;
          if (uiWidgetInteractive) {
            if (ImGui::InvisibleButton("##UIButton", drawSize)) {
              obj.ui.buttonPressed = true;
            }
            hovered = ImGui::IsItemHovered();
            active = ImGui::IsItemActive();
            obj.ui.uiHovered = hovered;
            obj.ui.uiActive  = active;
          }
          float hoverT =
              animateValue(animState.hover, hovered ? 1.0f : 0.0f, false);
          float activeT =
              animateValue(animState.active, active ? 1.0f : 0.0f, false);
          if (hoverT > 0.001f) {
            ImVec4 hoverCol = brighten(tint, 0.45f);
            hoverCol.w *= std::clamp(hoverT, 0.0f, 1.0f);
            dl->AddRectFilled(drawMin, drawMax, ImGui::GetColorU32(hoverCol),
                              6.0f);
          }
          if (activeT > 0.001f) {
            ImVec4 activeCol = brighten(tint, 0.65f);
            activeCol.w *= std::clamp(activeT, 0.0f, 1.0f);
            dl->AddRectFilled(drawMin, drawMax, ImGui::GetColorU32(activeCol),
                              6.0f);
          }
          dl->AddRect(drawMin, drawMax, border, 6.0f, 0, 2.0f);
          ImVec2 textSize = ImGui::CalcTextSize(obj.ui.label.c_str());
          ImVec2 textPos(drawMin.x + (drawSize.x - textSize.x) * 0.5f,
                         drawMin.y + (drawSize.y - textSize.y) * 0.5f);
          dl->AddText(textPos, uiTextCol, obj.ui.label.c_str());
        }
      } else if (obj.ui.type == UIElementType::Text) {
        spriteBatch.flush();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec4 tint(obj.ui.color.r, obj.ui.color.g, obj.ui.color.b,
                    obj.ui.color.a);
        ImFont *textFont =
            obj.ui.textFont.empty()
                ? ImGui::GetFont()
                : getUIFontForContext(obj.ui.textFont, ImGui::GetCurrentContext());
        if (!textFont) {
          textFont = ImGui::GetFont();
        }
        float scale = std::max(0.1f, obj.ui.textScale);
        const float authoredFontSize =
            ResolveViewportUITextAuthoredFontSize(
                ImGui::GetFontSize(), scale, obj.ui.fontSize);
        const float viewportRenderScale = std::max(0.01f, std::abs(uiScaleY));
        float fontSize = ResolveViewportUITextFontSize(
            ImGui::GetFontSize(), scale, obj.ui.fontSize, useWorldUi,
            uiWorldCamera.zoom, viewportRenderScale);
        const float textRotationRad = elementRotRad;
        const bool textIsRotated = std::abs(textRotationRad) > 1e-4f;
        if (!textIsRotated) {
          ImGui::PushClipRect(drawMin, drawMax, true);
        }
        const int textVtxStart = dl->VtxBuffer.Size;
        AddUITextWithFilter(dl, obj.material.textureFilter, textFont,
                            fontSize, drawMin, drawMax,
                            ImGui::GetColorU32(tint), obj.ui.label.c_str(),
                            obj.ui.textAutoWrap, obj.ui.textHAlign,
                            obj.ui.textVAlign, obj.ui.textEffectFlags,
                            obj.ui.textEffectSpeed, obj.ui.textEffectIntensity,
                            authoredFontSize,
                            ViewportRenderHelpers::UITextRevealParams{
                                obj.ui.textRevealChars,
                                obj.ui.textRevealPopScale,
                                obj.ui.textRevealSoftness});
        if (textIsRotated) {
          const ImVec2 textPivotOffset = ViewportRenderHelpers::UiAnchorPivot(
              obj.ui.anchor, ImVec2(drawMax.x - drawMin.x, drawMax.y - drawMin.y));
          const ImVec2 textPivot(drawMin.x + textPivotOffset.x,
                                 drawMin.y + textPivotOffset.y);
          ViewportRenderHelpers::RotateDrawListVertices(
              dl, textVtxStart, dl->VtxBuffer.Size, textPivot,
              textRotationRad);
        } else {
          ImGui::PopClipRect();
        }
      }
      if (pushedCanvasMask) {
        spriteBatch.flush();
        ImGui::PopClipRect();
      }
      ImGui::PopID();
      if (fontApplied)
        ImGui::PopFont();
      if (styleApplied)
        ImGui::GetStyle() = savedStyle;
    }
    spriteBatch.flush();
    if (postFx2DScope.active && !postFx2DScopeResolved) {
      ImDrawList *dl = ImGui::GetWindowDrawList();
      const Camera effectCamera = makeCameraFromObject(*playerCam);
      if (postFx2DScope.mode == PostFX2DScopeMode::Band) {
        if (postFx2DBandOpen) {
          queueOverlayPostFx(dl, effectCamera, postFxMin, postFxMax, false,
                             OverlayPostFxKind::BandEnd);
          postFx2DBandOpen = false;
        }
      } else {
        queueOverlayPostFx(dl, effectCamera, postFxMin, postFxMax, false,
                           OverlayPostFxKind::FullRegion);
      }
      postFx2DScopeResolved = true;
    }
    runtimeSpriteBatchBuildMs +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - spriteBatchBuildStart)
            .count();
    if (useWorldUi) {
      light2DCompositorRanLastFrame = renderedLight2DComposite;
      light2DLightBufferHadContentLastFrame = lightBufferHadContent;
      light2DActiveCountLastFrame = activeLight2DCount;
      light2DLitSprite2DCountLastFrame = litSprite2DCount;
      light2DLitWorldImageCountLastFrame = litWorldImageCount;
      runtime2DProfilerStats.light2DBuildMs = light2DStats.cpuBuildMs;
      runtime2DProfilerStats.light2DVisibleLights = light2DStats.visibleLights;
      runtime2DProfilerStats.light2DVisibleSprites =
          light2DStats.visibleSprites;
      if (captureLight2DRoutingReasons) {
        light2DObjectRoutingReasonsLastFrame = std::move(light2DRoutingReasons);
      }
    }

    bool pseudoPanelInteracting = false;
    struct PseudoPanelDrawEntry {
      int canvasId = -1;
      unsigned int textureId = 0;
      ImVec2 layoutSize = ImVec2(1.0f, 1.0f);
      std::array<ImVec2, 4> corners;
      int depthSort = 0;
      float distance = 0.0f;
      bool allowInteraction = false;
    };
    std::vector<PseudoPanelDrawEntry> pseudoPanels;
    pseudoPanels.reserve(sceneObjects.size());

    auto resolvePseudoAnchorScreen = [&](const SceneObject &canvas,
                                         ImVec2 &outScreen,
                                         float &outDistance) -> bool {
      outDistance = 1.0f;
      if (canvas.ui.pseudo3DAnchorTargetId < 0) {
        return false;
      }
      const SceneObject *anchorObj =
          uiSceneLookup.find(canvas.ui.pseudo3DAnchorTargetId);
      if (!anchorObj) {
        return false;
      }

      if (useWorldUi) {
        outScreen = worldToScreen(
            glm::vec2(anchorObj->position.x, anchorObj->position.y));
        outDistance = glm::length(
            glm::vec2(uiWorldCamera.position.x - anchorObj->position.x,
                      uiWorldCamera.position.y - anchorObj->position.y));
        return true;
      }

      if (hasProjectedUiCamera &&
          ProjectWorldToOverlayPoint(anchorObj->position, projectedUiView,
                                     projectedUiProj, overlayPos, overlaySize,
                                     outScreen)) {
        outDistance =
            glm::length(projectedUiCamera.position - anchorObj->position);
        return true;
      }

      return false;
    };
    auto resolvePseudoCanvasRect = [&](const SceneObject &canvas,
                                       const glm::vec2 &layoutSizePx,
                                       const ImVec2 &baseRegionMin,
                                       const ImVec2 &baseRegionMax,
                                       float scaleX, float scaleY,
                                       ImVec2 &outMin, ImVec2 &outMax) -> bool {
      std::vector<const SceneObject *> chain;
      chain.reserve(8);
      const SceneObject *current = &canvas;
      while (current) {
        if (current->hasUI && current->ui.type != UIElementType::None) {
          const int canvas3DId = find3DCanvasId(*current);
          if (canvas3DId < 0 || current->id == canvas.id) {
            chain.push_back(current);
          }
        }
        if (current->parentId < 0)
          break;
        current = uiSceneLookup.find(current->parentId);
        if (!current)
          break;
      }
      if (chain.empty()) {
        return false;
      }
      std::reverse(chain.begin(), chain.end());

      ImVec2 regionMin = baseRegionMin;
      ImVec2 regionMax = baseRegionMax;
      for (const SceneObject *node : chain) {
        ImVec2 size(1.0f, 1.0f);
        if (node->id == canvas.id) {
          size = ImVec2(std::max(1.0f, layoutSizePx.x * scaleX),
                        std::max(1.0f, layoutSizePx.y * scaleY));
        } else {
          const glm::vec2 nodeSize = getSpriteDisplaySize(*node);
          size = ImVec2(std::max(1.0f, nodeSize.x * scaleX),
                        std::max(1.0f, nodeSize.y * scaleY));
        }
        const ImVec2 anchorPoint =
            anchorToPoint(node->ui.anchor, regionMin, regionMax);
        const ImVec2 pivot(anchorPoint.x + node->ui.position.x * scaleX,
                           anchorPoint.y + node->ui.position.y * scaleY);
        const ImVec2 pivotOffset = anchorToPivot(node->ui.anchor, size);
        regionMin = ImVec2(pivot.x - pivotOffset.x, pivot.y - pivotOffset.y);
        regionMax = ImVec2(regionMin.x + size.x, regionMin.y + size.y);
      }
      outMin = regionMin;
      outMax = regionMax;
      return true;
    };

    for (auto &canvas : sceneObjects) {
      if (!IsObjectEnabledInHierarchy(canvas) || !canvas.hasUI ||
          canvas.ui.type != UIElementType::Canvas || canvas.ui.renderIn3D ||
          !canvas.ui.pseudo3DEnabled ||
          !canvas.ui.pseudo3DUseOffscreenSurface) {
        continue;
      }

      const glm::vec2 layoutSizePx = ResolvePseudo3DLayoutSize(canvas);
      ImVec2 rectMin;
      ImVec2 rectMax;
      const int targetWidth = std::clamp((canvas.ui.renderTargetSize.x > 0)
                                             ? canvas.ui.renderTargetSize.x
                                             : static_cast<int>(layoutSizePx.x),
                                         16, 4096);
      const int targetHeight = std::clamp(
          (canvas.ui.renderTargetSize.y > 0) ? canvas.ui.renderTargetSize.y
                                             : static_cast<int>(layoutSizePx.y),
          16, 4096);
	      Renderer::UiTargetInfo target =
	          renderer.ensureUiTarget(canvas.id, targetWidth, targetHeight,
	                                  canvas.layer,
	                                  canvas.ui.renderTargetFilter, false);
      if (target.texture == 0) {
        continue;
      }

      float distance = 1.0f;
      ImVec2 anchorScreen(0.0f, 0.0f);
      const bool anchored =
          resolvePseudoAnchorScreen(canvas, anchorScreen, distance);
      const bool transformDriven = !anchored;
      const bool stablePanelSpace = !useWorldUi && !anchored;
      if (!anchored) {
        if (useWorldUi) {
          distance = glm::length(
              glm::vec2(uiWorldCamera.position.x - canvas.position.x,
                        uiWorldCamera.position.y - canvas.position.y));
        } else if (hasProjectedUiCamera) {
          distance = glm::length(projectedUiCamera.position - canvas.position);
        }
      }

      std::array<ImVec2, 4> projectedCorners{};
      bool projectedCornersValid = false;
      if (transformDriven && !useWorldUi && hasProjectedUiCamera) {
        projectedCornersValid = ProjectTransformDrivenCanvasCorners(
            canvas, projectedUiView, projectedUiProj, overlayPos, overlaySize,
            projectedCorners);
      } else if (transformDriven && useWorldUi) {
        const float halfW = std::max(0.0001f, canvas.scale.x * 0.5f);
        const float halfH = std::max(0.0001f, canvas.scale.y * 0.5f);
        const float angle = glm::radians(canvas.rotation.z);
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        const std::array<glm::vec2, 4> localXY = {
            glm::vec2(-halfW, +halfH), glm::vec2(+halfW, +halfH),
            glm::vec2(+halfW, -halfH), glm::vec2(-halfW, -halfH)};
        for (int i = 0; i < 4; ++i) {
          const glm::vec2 rotated(localXY[i].x * c - localXY[i].y * s,
                                  localXY[i].x * s + localXY[i].y * c);
          const glm::vec2 worldXY(canvas.position.x + rotated.x,
                                  canvas.position.y + rotated.y);
          const ImVec2 screen = worldToScreen(worldXY);
          projectedCorners[i] = screen;
        }
        projectedCornersValid = true;
      }

      const ImVec2 pseudoRegionMin = stablePanelSpace ? panelMin : overlayPos;
      const ImVec2 pseudoRegionMax =
          stablePanelSpace
              ? ImVec2(panelMin.x + panelSize.x, panelMin.y + panelSize.y)
              : ImVec2(overlayPos.x + overlaySize.x,
                       overlayPos.y + overlaySize.y);
      const float pseudoScaleX = stablePanelSpace ? 1.0f : uiScaleX;
      const float pseudoScaleY = stablePanelSpace ? 1.0f : uiScaleY;
      if (!resolvePseudoCanvasRect(canvas, layoutSizePx, pseudoRegionMin,
                                   pseudoRegionMax, pseudoScaleX,
                                   pseudoScaleY, rectMin, rectMax)) {
        continue;
      }

      if (anchored) {
        const ImVec2 center((rectMin.x + rectMax.x) * 0.5f,
                            (rectMin.y + rectMax.y) * 0.5f);
        const ImVec2 shift(anchorScreen.x - center.x,
                           anchorScreen.y - center.y);
        rectMin = ImVec2(rectMin.x + shift.x, rectMin.y + shift.y);
        rectMax = ImVec2(rectMax.x + shift.x, rectMax.y + shift.y);
      }

      float distanceScale = 1.0f;
      float perspectiveFactor = 1.0f;
      bool allowInteraction = false;
      ResolvePseudo3DDistanceState(canvas.ui, distance, distanceScale,
                                   perspectiveFactor, allowInteraction);

      PseudoPanelDrawEntry entry;
      entry.canvasId = canvas.id;
      entry.textureId = target.texture;
      entry.layoutSize = ImVec2(layoutSizePx.x, layoutSizePx.y);
      entry.corners = BuildPseudo3DPanelCorners(
          rectMin, rectMax, canvas.ui, distanceScale, perspectiveFactor,
          projectedCornersValid ? &projectedCorners : nullptr);
      entry.depthSort = canvas.ui.pseudo3DDepthSort;
      entry.distance = distance;
      entry.allowInteraction = allowInteraction;
      pseudoPanels.push_back(entry);
    }

    if (!pseudoPanels.empty()) {
      std::stable_sort(
          pseudoPanels.begin(), pseudoPanels.end(),
          [](const PseudoPanelDrawEntry &a, const PseudoPanelDrawEntry &b) {
            if (std::abs(a.distance - b.distance) > 0.001f)
              return a.distance > b.distance;
            if (a.depthSort != b.depthSort)
              return a.depthSort < b.depthSort;
            return a.canvasId < b.canvasId;
          });

      ImDrawList *panelDrawList = ImGui::GetWindowDrawList();
      for (const PseudoPanelDrawEntry &panel : pseudoPanels) {
        panelDrawList->AddImageQuad(
            (ImTextureID)(intptr_t)panel.textureId, panel.corners[0],
            panel.corners[1], panel.corners[2], panel.corners[3],
            ImVec2(0.0f, 1.0f), ImVec2(1.0f, 1.0f), ImVec2(1.0f, 0.0f),
            ImVec2(0.0f, 0.0f), IM_COL32_WHITE);
      }

      if (imageHovered && !uiWorldCameraActive) {
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        bool inputAssigned = false;
        for (auto it = pseudoPanels.rbegin(); it != pseudoPanels.rend(); ++it) {
          ImVec2 uv(0.0f, 0.0f);
          if (!MapPointToPseudo3DQuadUV(it->corners, mousePos, uv)) {
            continue;
          }

          pseudoPanelInteracting =
              pseudoPanelInteracting ||
              ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
              ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
              std::abs(ImGui::GetIO().MouseWheel) > 0.0f;
          if (!it->allowInteraction || inputAssigned) {
            continue;
          }

          UiCanvas3DInput &input = uiCanvas3DInputs[it->canvasId];
          const float u = std::clamp(uv.x, 0.0f, 1.0f);
          const float v = std::clamp(uv.y, 0.0f, 1.0f);
          input.mousePos =
              ImVec2(u * std::max(1.0f, it->layoutSize.x),
                     (1.0f - v) * std::max(1.0f, it->layoutSize.y));
          input.mouseDown[0] = ImGui::GetIO().MouseDown[0];
          input.mouseDown[1] = ImGui::GetIO().MouseDown[1];
          input.mouseDown[2] = ImGui::GetIO().MouseDown[2];
          input.mouseWheel = ImGui::GetIO().MouseWheel;
          input.hasInput = true;
          input.hitT = -1000.0f - static_cast<float>(it->depthSort);
          inputAssigned = true;
        }
      }
    }

    bool gizmoUsed = false;
    if (allowEditorUi && !isPlaying) {
      SceneObject *selected = getSelectedObject();
      if (selected && isUIType(*selected) &&
          selected->ui.type != UIElementType::Canvas) {
        ImVec2 rectMin, rectMax;
        ImVec2 parentMin, parentMax;
        bool haveRect = true;
        if (useWorldUi || selected->type == ObjectType::Sprite25D) {
          haveRect = resolveCachedUiWorldRect(*selected, rectMin, rectMax);
        } else {
          resolveUIRect(*selected, rectMin, rectMax, &parentMin, &parentMax);
        }
        if (haveRect) {
          auto anchorToPivotUI = [](UIAnchor anchor, const ImVec2 &size) {
            switch (anchor) {
            case UIAnchor::TopLeft:
              return ImVec2(0.0f, 0.0f);
            case UIAnchor::TopRight:
              return ImVec2(size.x, 0.0f);
            case UIAnchor::BottomLeft:
              return ImVec2(0.0f, size.y);
            case UIAnchor::BottomRight:
              return ImVec2(size.x, size.y);
            default:
              return ImVec2(size.x * 0.5f, size.y * 0.5f);
            }
          };

          std::vector<int> worldUiRoots;
          ImVec2 worldUiBoundsMin = rectMin;
          ImVec2 worldUiBoundsMax = rectMax;
          if (useWorldUi) {
            std::vector<int> candidateIds;
            if (!selectedObjectIds.empty()) {
              candidateIds = selectedObjectIds;
            } else if (selectedObjectId >= 0) {
              candidateIds.push_back(selectedObjectId);
            }
            if (candidateIds.empty()) {
              candidateIds.push_back(selected->id);
            }

            std::vector<int> validIds;
            validIds.reserve(candidateIds.size());
            for (int id : candidateIds) {
              SceneObject *candidate = findObjectById(id);
              if (!candidate || !IsObjectEnabledInHierarchy(*candidate))
                continue;
              if (!isUIType(*candidate) ||
                  candidate->ui.type == UIElementType::Canvas)
                continue;
              ImVec2 candidateMin, candidateMax;
              if (!resolveCachedUiWorldRect(*candidate, candidateMin,
                                            candidateMax))
                continue;
              validIds.push_back(id);
            }
            if (validIds.empty()) {
              validIds.push_back(selected->id);
            }

            std::unordered_set<int> selectedSet(validIds.begin(),
                                                validIds.end());
            auto hasSelectedAncestor = [&](int id) {
              SceneObject *current = findObjectById(id);
              int parentId = current ? current->parentId : -1;
              while (parentId != -1) {
                if (selectedSet.count(parentId) > 0) {
                  return true;
                }
                SceneObject *parent = findObjectById(parentId);
                parentId = parent ? parent->parentId : -1;
              }
              return false;
            };

            worldUiRoots.reserve(validIds.size());
            for (int id : validIds) {
              if (!hasSelectedAncestor(id)) {
                worldUiRoots.push_back(id);
              }
            }
            if (worldUiRoots.empty()) {
              worldUiRoots = validIds;
            }

            ImVec2 boundsMin(FLT_MAX, FLT_MAX);
            ImVec2 boundsMax(-FLT_MAX, -FLT_MAX);
            for (int id : worldUiRoots) {
              SceneObject *target = findObjectById(id);
              if (!target)
                continue;
              ImVec2 targetMin, targetMax;
              if (!resolveCachedUiWorldRect(*target, targetMin, targetMax))
                continue;
              boundsMin.x = std::min(boundsMin.x, targetMin.x);
              boundsMin.y = std::min(boundsMin.y, targetMin.y);
              boundsMax.x = std::max(boundsMax.x, targetMax.x);
              boundsMax.y = std::max(boundsMax.y, targetMax.y);
            }
            if (boundsMin.x != FLT_MAX && boundsMin.y != FLT_MAX) {
              worldUiBoundsMin = boundsMin;
              worldUiBoundsMax = boundsMax;
              rectMin = boundsMin;
              rectMax = boundsMax;
            } else {
              worldUiRoots.clear();
              worldUiRoots.push_back(selected->id);
            }
          }

          ImVec2 rectSize(rectMax.x - rectMin.x, rectMax.y - rectMin.y);

          ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
          if (mCurrentGizmoOperation == ImGuizmo::SCALE) {
            op = ImGuizmo::SCALE;
          } else if (mCurrentGizmoOperation == ImGuizmo::BOUNDS) {
            op = ImGuizmo::BOUNDS;
          } else if (mCurrentGizmoOperation == ImGuizmo::ROTATE) {
            op = ImGuizmo::ROTATE;
          }
          glm::mat4 view(1.0f);
          glm::mat4 proj =
              glm::ortho(0.0f, (float)(imageMax.x - imageMin.x),
                         (float)(imageMax.y - imageMin.y), 0.0f, -1.0f, 1.0f);
          glm::vec2 parentOffset = getWorldParentOffset(*selected);
          const float selectedParentRot = getWorldParentRotationRad(*selected);
          glm::vec2 pivotWorld =
              parentOffset +
              ViewportRenderHelpers::RotateUiVec2(
                  glm::vec2(selected->ui.position.x, selected->ui.position.y),
                  selectedParentRot);
          ImVec2 pivotScreen;
          if (useWorldUi) {
            if (worldUiRoots.size() > 1) {
              pivotScreen =
                  ImVec2((worldUiBoundsMin.x + worldUiBoundsMax.x) * 0.5f,
                         (worldUiBoundsMin.y + worldUiBoundsMax.y) * 0.5f);
            } else {
              pivotScreen = worldToScreen(pivotWorld);
            }
          } else {
            ImVec2 anchorPoint =
                anchorToPoint(selected->ui.anchor, parentMin, parentMax);
            pivotScreen =
                ImVec2(anchorPoint.x + selected->ui.position.x * uiScaleX,
                       anchorPoint.y + selected->ui.position.y * uiScaleY);
          }
          ImVec2 rectCenter =
              (op == ImGuizmo::SCALE || op == ImGuizmo::BOUNDS)
                  ? ImVec2((rectMin.x + rectMax.x) * 0.5f - imageMin.x,
                           (rectMin.y + rectMax.y) * 0.5f - imageMin.y)
                  : ImVec2(pivotScreen.x - imageMin.x,
                           pivotScreen.y - imageMin.y);
          glm::vec3 gizmoScale(1.0f, 1.0f, 1.0f);
          if (op == ImGuizmo::SCALE || op == ImGuizmo::BOUNDS) {
            gizmoScale = glm::vec3(rectSize.x, rectSize.y, 1.0f);
          }
          glm::mat4 model(1.0f);
          model = glm::translate(model,
                                 glm::vec3(rectCenter.x, rectCenter.y, 0.0f));
          model = glm::rotate(model,
                              getWorldParentRotationRad(*selected) +
                                  glm::radians(selected->ui.rotation),
                              glm::vec3(0.0f, 0.0f, 1.0f));
          model = glm::scale(model, gizmoScale);
          const bool stableRectScale =
              (op == ImGuizmo::SCALE || op == ImGuizmo::BOUNDS);
          if (stableRectScale && gameUiGizmoHistoryCaptured &&
              gameUiRectGizmoOperation == op &&
              !gameUiRectGizmoSnapshots.empty()) {
            model = gameUiRectGizmoModel;
          }
          const glm::mat4 originalModel = model;

          ImGuizmo::BeginFrame();
          ImGuizmo::Enable(true);
          ImGuizmo::SetOrthographic(true);
          ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
          ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x,
                            imageMax.y - imageMin.y);
          glm::mat4 delta(1.0f);
          float bounds[6] = {-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 0.0f};
          const float *boundsPtr = (op == ImGuizmo::BOUNDS) ? bounds : nullptr;
          ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op,
                               ImGuizmo::LOCAL, glm::value_ptr(model),
                               glm::value_ptr(delta), nullptr, boundsPtr,
                               nullptr);
          if (ImGuizmo::IsUsing()) {
            if (!gameUiGizmoHistoryCaptured) {
              recordState("gameUiGizmo");
              gameUiRectGizmoOperation = op;
              gameUiRectGizmoModel = originalModel;
              gameUiRectGizmoStartMouse = ImGui::GetIO().MousePos;
              gameUiRectGizmoSnapshots.clear();
              for (int id : worldUiRoots) {
                SceneObject *target = findObjectById(id);
                if (!target)
                  continue;
                ImVec2 targetMin, targetMax;
                if (!resolveCachedUiWorldRect(*target, targetMin, targetMax))
                  continue;
                gameUiRectGizmoSnapshots.push_back(UiRectGizmoSnapshot{
                    id, target->ui.position, target->ui.size,
                    target->ui.rotation, targetMin, targetMax,
                    target->ui.textScale, target->ui.fontSize});
              }
              gameUiGizmoHistoryCaptured = true;
            }
            const float scaleDragDx =
                ImGui::GetIO().MousePos.x - gameUiRectGizmoStartMouse.x;
            const float scaleDragDy =
                ImGui::GetIO().MousePos.y - gameUiRectGizmoStartMouse.y;
            const bool allowScaleApply =
                !stableRectScale || ((scaleDragDx * scaleDragDx +
                                      scaleDragDy * scaleDragDy) >= 9.0f);
            const bool applyPixelSnap = pixelGridSnapEnabled;
            const float pixelStep =
                static_cast<float>(std::max(1, pixelGridSnapStep));
            auto snapScreenToPixel = [&](ImVec2 p) {
              p.x = imageMin.x +
                    std::round((p.x - imageMin.x) / pixelStep) * pixelStep;
              p.y = imageMin.y +
                    std::round((p.y - imageMin.y) / pixelStep) * pixelStep;
              return p;
            };
            auto findRectSnapshot = [&](int id) -> const UiRectGizmoSnapshot * {
              for (const UiRectGizmoSnapshot &snapshot :
                   gameUiRectGizmoSnapshots) {
                if (snapshot.objectId == id)
                  return &snapshot;
              }
              return nullptr;
            };
            auto extractRectFromModel = [](const glm::mat4 &rectModel,
                                           ImVec2 &outCenter, ImVec2 &outSize) {
              const glm::vec4 corners[4] = {glm::vec4(-0.5f, -0.5f, 0.0f, 1.0f),
                                            glm::vec4(0.5f, -0.5f, 0.0f, 1.0f),
                                            glm::vec4(0.5f, 0.5f, 0.0f, 1.0f),
                                            glm::vec4(-0.5f, 0.5f, 0.0f, 1.0f)};
              ImVec2 pts[4];
              for (int i = 0; i < 4; ++i) {
                const glm::vec4 p = rectModel * corners[i];
                pts[i] = ImVec2(p.x, p.y);
              }
              outCenter =
                  ImVec2((pts[0].x + pts[1].x + pts[2].x + pts[3].x) * 0.25f,
                         (pts[0].y + pts[1].y + pts[2].y + pts[3].y) * 0.25f);
              auto distance = [](const ImVec2 &a, const ImVec2 &b) {
                const float dx = b.x - a.x;
                const float dy = b.y - a.y;
                return std::sqrt(dx * dx + dy * dy);
              };
              outSize = ImVec2(std::max(0.01f, distance(pts[0], pts[1])),
                               std::max(0.01f, distance(pts[0], pts[3])));
            };

            if (useWorldUi && !worldUiRoots.empty()) {
              const glm::mat4 gizmoDelta =
                  model * glm::inverse(stableRectScale ? gameUiRectGizmoModel
                                                       : originalModel);
              const bool groupRotate =
                  (op == ImGuizmo::ROTATE && worldUiRoots.size() > 1);

              for (int id : worldUiRoots) {
                SceneObject *target = findObjectById(id);
                if (!target)
                  continue;

                ImVec2 targetMin, targetMax;
                float targetRotation = target->ui.rotation;
                if (stableRectScale) {
                  const UiRectGizmoSnapshot *snapshot = findRectSnapshot(id);
                  if (!snapshot)
                    continue;
                  targetMin = snapshot->rectMin;
                  targetMax = snapshot->rectMax;
                  targetRotation = snapshot->rotation;
                } else {
                  if (!resolveCachedUiWorldRect(*target, targetMin, targetMax))
                    continue;
                }
                ImVec2 targetSize(targetMax.x - targetMin.x,
                                  targetMax.y - targetMin.y);
                if (targetSize.x <= 0.01f || targetSize.y <= 0.01f)
                  continue;
                ImVec2 targetCenter(
                    (targetMin.x + targetMax.x) * 0.5f - imageMin.x,
                    (targetMin.y + targetMax.y) * 0.5f - imageMin.y);

                const float targetParentRot =
                    getWorldParentRotationRad(*target);
                glm::mat4 targetModel(1.0f);
                targetModel = glm::translate(
                    targetModel,
                    glm::vec3(targetCenter.x, targetCenter.y, 0.0f));
                targetModel = glm::rotate(
                    targetModel, targetParentRot + glm::radians(targetRotation),
                    glm::vec3(0.0f, 0.0f, 1.0f));
                targetModel = glm::scale(
                    targetModel, glm::vec3(targetSize.x, targetSize.y, 1.0f));

                glm::mat4 targetNewModel = gizmoDelta * targetModel;
                glm::vec3 pos, rot, scl;
                DecomposeMatrix(targetNewModel, pos, rot, scl);
                glm::vec3 euler = NormalizeEulerDegrees(glm::degrees(rot));
                ImVec2 targetNewCenter(imageMin.x + pos.x, imageMin.y + pos.y);
                ImVec2 targetNewScreenSize(targetSize.x, targetSize.y);
                if (stableRectScale) {
                  extractRectFromModel(targetNewModel, targetNewCenter,
                                       targetNewScreenSize);
                  targetNewCenter.x += imageMin.x;
                  targetNewCenter.y += imageMin.y;
                }
                if (applyPixelSnap && op == ImGuizmo::TRANSLATE) {
                  targetNewCenter = snapScreenToPixel(targetNewCenter);
                }

                glm::vec2 targetParentOffset = getWorldParentOffset(*target);
                glm::vec2 worldCenter = screenToWorld(targetNewCenter);

                if (op == ImGuizmo::ROTATE) {
                  target->ui.rotation =
                      euler.z - glm::degrees(targetParentRot);
                  if (groupRotate) {
                    glm::vec2 worldSize = getSpriteDisplaySize(*target);
                    ImVec2 pivotOffset = anchorToPivotUI(
                        target->ui.anchor, ImVec2(worldSize.x, worldSize.y));
                    glm::vec2 worldMin = worldCenter - worldSize * 0.5f;
                    glm::vec2 worldPivot =
                        worldMin + glm::vec2(pivotOffset.x, pivotOffset.y);
                    target->ui.position = ViewportRenderHelpers::RotateUiVec2(
                        worldPivot - targetParentOffset -
                            parallaxOffset(*target),
                        -targetParentRot);
                  }
                } else if (op == ImGuizmo::TRANSLATE) {
                  glm::vec2 worldSize = getSpriteDisplaySize(*target);
                  ImVec2 pivotOffset = anchorToPivotUI(
                      target->ui.anchor, ImVec2(worldSize.x, worldSize.y));
                  glm::vec2 worldMin = worldCenter - worldSize * 0.5f;
                  glm::vec2 worldPivot =
                      worldMin + glm::vec2(pivotOffset.x, pivotOffset.y);
                  target->ui.position = ViewportRenderHelpers::RotateUiVec2(
                      worldPivot - targetParentOffset - parallaxOffset(*target),
                      -targetParentRot);
                } else if (op == ImGuizmo::SCALE || op == ImGuizmo::BOUNDS) {
                  if (allowScaleApply) {
                    const float minUiSize =
                        (target->ui.type == UIElementType::Image ||
                         target->ui.type == UIElementType::Sprite2D)
                            ? 0.01f
                            : 1.0f;
                    ImVec2 newSize =
                        stableRectScale
                            ? ImVec2(std::max(minUiSize, targetNewScreenSize.x),
                                     std::max(minUiSize, targetNewScreenSize.y))
                            : ImVec2(std::max(minUiSize, scl.x),
                                     std::max(minUiSize, scl.y));
                    if (stableRectScale) {
                      constexpr float kRectScaleDeadZonePx = 0.1f;
                      if (std::abs(newSize.x - targetSize.x) <
                          kRectScaleDeadZonePx)
                        newSize.x = targetSize.x;
                      if (std::abs(newSize.y - targetSize.y) <
                          kRectScaleDeadZonePx)
                        newSize.y = targetSize.y;
                    }
                    if (applyPixelSnap) {
                      newSize.x = std::max(pixelStep,
                                           std::round(newSize.x / pixelStep) *
                                               pixelStep);
                      newSize.y = std::max(pixelStep,
                                           std::round(newSize.y / pixelStep) *
                                               pixelStep);
                    }
                    glm::vec2 worldSize =
                        glm::vec2(newSize.x, newSize.y) / uiWorldCamera.zoom;
                    ImVec2 pivotOffset = anchorToPivotUI(
                        target->ui.anchor, ImVec2(worldSize.x, worldSize.y));
                    glm::vec2 worldMin = worldCenter - worldSize * 0.5f;
                    glm::vec2 worldPivot =
                        worldMin + glm::vec2(pivotOffset.x, pivotOffset.y);
                    target->ui.position = ViewportRenderHelpers::RotateUiVec2(
                        worldPivot - targetParentOffset -
                            parallaxOffset(*target),
                        -targetParentRot);
                    glm::vec2 targetScale(
                        std::max(0.01f, std::abs(target->scale.x)),
                        std::max(0.01f, std::abs(target->scale.y)));
                    target->ui.size = worldSize / targetScale;
                    const UiRectGizmoSnapshot *snapshot = findRectSnapshot(id);
                    if (snapshot) {
                      ImVec2 startSize(
                          snapshot->rectMax.x - snapshot->rectMin.x,
                          snapshot->rectMax.y - snapshot->rectMin.y);
                      ApplyUITextScaleFromRectResize(
                          *target, snapshot->textScale, snapshot->fontSize,
                          startSize, newSize);
                    }
                  }
                }
              }
            } else {
              glm::vec3 pos, rot, scl;
              DecomposeMatrix(model, pos, rot, scl);
              glm::vec3 euler = NormalizeEulerDegrees(glm::degrees(rot));
              ImVec2 newPivot(imageMin.x + pos.x, imageMin.y + pos.y);
              ImVec2 newScaleCenter = newPivot;
              ImVec2 newScaleScreenSize(std::max(0.01f, scl.x),
                                        std::max(0.01f, scl.y));
              if (stableRectScale) {
                extractRectFromModel(model, newScaleCenter, newScaleScreenSize);
                newScaleCenter.x += imageMin.x;
                newScaleCenter.y += imageMin.y;
              }
              if (applyPixelSnap && op == ImGuizmo::TRANSLATE) {
                newPivot = snapScreenToPixel(newPivot);
              }
              if (op == ImGuizmo::ROTATE) {
                selected->ui.rotation =
                    euler.z - glm::degrees(selectedParentRot);
              } else if (op == ImGuizmo::TRANSLATE) {
                if (useWorldUi) {
                  glm::vec2 worldPivot = screenToWorld(newPivot);
                  selected->ui.position = ViewportRenderHelpers::RotateUiVec2(
                      worldPivot - parentOffset - parallaxOffset(*selected),
                      -selectedParentRot);
                } else {
                  ImVec2 anchorPoint =
                      anchorToPoint(selected->ui.anchor, parentMin, parentMax);
                  float invScaleX = (uiScaleX > 0.0f) ? 1.0f / uiScaleX : 1.0f;
                  float invScaleY = (uiScaleY > 0.0f) ? 1.0f / uiScaleY : 1.0f;
                  glm::vec2 local = ViewportRenderHelpers::RotateUiVec2(
                      glm::vec2(newPivot.x - anchorPoint.x,
                                newPivot.y - anchorPoint.y),
                      -selectedParentRot);
                  selected->ui.position =
                      glm::vec2(local.x * invScaleX, local.y * invScaleY);
                }
              } else if (op == ImGuizmo::SCALE || op == ImGuizmo::BOUNDS) {
                if (allowScaleApply) {
                  const float minUiSize =
                      (selected->ui.type == UIElementType::Image ||
                       selected->ui.type == UIElementType::Sprite2D)
                          ? 0.01f
                          : 1.0f;
                  ImVec2 newSize =
                      stableRectScale
                          ? ImVec2(std::max(minUiSize, newScaleScreenSize.x),
                                   std::max(minUiSize, newScaleScreenSize.y))
                          : ImVec2(std::max(minUiSize, scl.x),
                                   std::max(minUiSize, scl.y));
                  if (stableRectScale) {
                    constexpr float kRectScaleDeadZonePx = 0.1f;
                    if (std::abs(newSize.x - rectSize.x) < kRectScaleDeadZonePx)
                      newSize.x = rectSize.x;
                    if (std::abs(newSize.y - rectSize.y) < kRectScaleDeadZonePx)
                      newSize.y = rectSize.y;
                  }
                  if (applyPixelSnap) {
                    newSize.x =
                        std::max(pixelStep,
                                 std::round(newSize.x / pixelStep) * pixelStep);
                    newSize.y =
                        std::max(pixelStep,
                                 std::round(newSize.y / pixelStep) * pixelStep);
                  }
                  if (useWorldUi) {
                    glm::vec2 worldSize =
                        glm::vec2(newSize.x, newSize.y) / uiWorldCamera.zoom;
                    glm::vec2 worldCenter = screenToWorld(newScaleCenter);
                    ImVec2 pivotOffset = anchorToPivotUI(
                        selected->ui.anchor, ImVec2(worldSize.x, worldSize.y));
                    glm::vec2 worldMin = worldCenter - worldSize * 0.5f;
                    glm::vec2 worldPivot =
                        worldMin + glm::vec2(pivotOffset.x, pivotOffset.y);
                    selected->ui.position = ViewportRenderHelpers::RotateUiVec2(
                        worldPivot - parentOffset - parallaxOffset(*selected),
                        -selectedParentRot);
                    glm::vec2 selectedScale(
                        std::max(0.01f, std::abs(selected->scale.x)),
                        std::max(0.01f, std::abs(selected->scale.y)));
                    selected->ui.size = worldSize / selectedScale;
                    const UiRectGizmoSnapshot *snapshot =
                        findRectSnapshot(selected->id);
                    if (snapshot) {
                      ImVec2 startSize(
                          snapshot->rectMax.x - snapshot->rectMin.x,
                          snapshot->rectMax.y - snapshot->rectMin.y);
                      ApplyUITextScaleFromRectResize(
                          *selected, snapshot->textScale, snapshot->fontSize,
                          startSize, newSize);
                    }
                  } else {
                    float invScaleX =
                        (uiScaleX > 0.0f) ? 1.0f / uiScaleX : 1.0f;
                    float invScaleY =
                        (uiScaleY > 0.0f) ? 1.0f / uiScaleY : 1.0f;
                    glm::vec2 uiSize(newSize.x * invScaleX,
                                     newSize.y * invScaleY);
                    ImVec2 anchorPoint = anchorToPoint(selected->ui.anchor,
                                                       parentMin, parentMax);
                    ImVec2 pivotOffset = anchorToPivotUI(
                        selected->ui.anchor, ImVec2(uiSize.x, uiSize.y));
                    ImVec2 screenMin(newScaleCenter.x - newSize.x * 0.5f,
                                     newScaleCenter.y - newSize.y * 0.5f);
                    ImVec2 screenPivot(screenMin.x + pivotOffset.x * uiScaleX,
                                       screenMin.y + pivotOffset.y * uiScaleY);
                    glm::vec2 localPivot = ViewportRenderHelpers::RotateUiVec2(
                        glm::vec2(screenPivot.x - anchorPoint.x,
                                  screenPivot.y - anchorPoint.y),
                        -selectedParentRot);
                    selected->ui.position = glm::vec2(localPivot.x * invScaleX,
                                                      localPivot.y * invScaleY);
                    glm::vec2 selectedScale(
                        std::max(0.01f, std::abs(selected->scale.x)),
                        std::max(0.01f, std::abs(selected->scale.y)));
                    selected->ui.size = uiSize / selectedScale;
                    const UiRectGizmoSnapshot *snapshot =
                        findRectSnapshot(selected->id);
                    if (snapshot) {
                      ImVec2 startSize(
                          snapshot->rectMax.x - snapshot->rectMin.x,
                          snapshot->rectMax.y - snapshot->rectMin.y);
                      ApplyUITextScaleFromRectResize(
                          *selected, snapshot->textScale, snapshot->fontSize,
                          startSize, newSize);
                    }
                  }
                }
              }
            }
            projectManager.currentProject.hasUnsavedChanges = true;
            gizmoUsed = true;
          } else {
            gameUiGizmoHistoryCaptured = false;
            gameUiRectGizmoSnapshots.clear();
            gameUiRectGizmoModel = glm::mat4(1.0f);
            gameUiRectGizmoStartMouse = ImVec2(0.0f, 0.0f);
          }
        }
      } else {
        gameUiGizmoHistoryCaptured = false;
        gameUiRectGizmoSnapshots.clear();
        gameUiRectGizmoModel = glm::mat4(1.0f);
        gameUiRectGizmoStartMouse = ImVec2(0.0f, 0.0f);
      }
    } else {
      gameUiGizmoHistoryCaptured = false;
      gameUiRectGizmoSnapshots.clear();
      gameUiRectGizmoModel = glm::mat4(1.0f);
      gameUiRectGizmoStartMouse = ImVec2(0.0f, 0.0f);
    }

    uiInteracting = ImGui::IsAnyItemActive() || gizmoUsed ||
                    uiWorldCameraActive || pseudoPanelInteracting;

    if (!postFx2DScope.active &&
        (playerCam->camera.use2D || playerCam->camera.applyPostFX)) {
      queueOverlayPostFx(ImGui::GetWindowDrawList(),
                         makeCameraFromObject(*playerCam),
                         postFxMin, postFxMax, false);
    }

    if (profilerOverlayPending) {
      ViewportRenderHelpers::DrawViewportProfilerPanel(
          ImGui::GetWindowDrawList(), profilerPanelMin, profilerOverlayLines);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    bool clicked = imageHovered && isPlaying &&
                   ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                   !uiInteracting;

    if (clicked && !gameViewCursorLocked) {
      gameViewCursorLocked = true;
    }
    if (gameViewCursorLocked &&
        (!isPlaying || ImGui::IsKeyPressed(ImGuiKey_Escape))) {
      gameViewCursorLocked = false;
    }

    gameViewportFocused = windowFocused || gameViewCursorLocked;
    if (restoreUiWorldCamera) {
      uiWorldCamera = uiWorldCameraBackup;
    }
    } else {
      gameUiGizmoHistoryCaptured = false;
      gameUiRectGizmoSnapshots.clear();
      gameUiRectGizmoModel = glm::mat4(1.0f);
      gameUiRectGizmoStartMouse = ImVec2(0.0f, 0.0f);
      gameViewportFocused = windowFocused;
    }
  } else {
    ImGui::TextDisabled("No player camera found (Camera Type: Player).");
    gameViewportFocused = ImGui::IsWindowFocused();
  }

  const auto runtimeUiEnd = std::chrono::steady_clock::now();
  runtime2DProfilerStats.uiRuntimeMs =
      std::chrono::duration<double, std::milli>(runtimeUiEnd - runtimeUiStart)
          .count();
  runtime2DProfilerStats.spriteBatchBuildMs = runtimeSpriteBatchBuildMs;
  runtime2DProfilerStats.visibleObjectCount = runtimeVisibleObjectCount;
  SubmitRuntime2DWorldProfilerStats(runtime2DProfilerStats);

  ImGui::End();
}
#pragma endregion
