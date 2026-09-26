#include "Engine.h"
#include "HardwareProfile.h"
#include <chrono>
#include <cstdio>
#include <string>

void Engine::renderPlayControlsBar() {
  const EditorChromeMetrics &chrome = getEditorChromeMetrics(uiChromeScale);
  const char *playTooltip = isPlaying ? "Stop Play Mode" : "Play Mode";
  const char *specTooltip = specMode ? "Disable Spec Mode" : "Spec Mode";
  const char *pauseTooltip = isPaused ? "Resume" : "Pause";
  const bool hasVulkanSceneTexture =
      usingVulkan() && vulkanRendererInitialized && (vulkanRenderer != nullptr);
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

  struct ToolbarIcon {
    ImTextureID id = static_cast<ImTextureID>(0);
    bool flipY = false;
    float nativeW = 0.0f;
    float nativeH = 0.0f;
  };

  auto resolveToolbarIcon = [&](const char *iconPath) -> ToolbarIcon {
    if (!iconPath || !*iconPath) {
      return {};
    }
    if (rendererInitialized) {
      if (Texture *icon = renderer.getTexture(
              iconPath, MaterialProperties::TextureFilter::Point);
          icon && icon->GetID()) {
        return {static_cast<ImTextureID>(icon->GetID()), true,
                static_cast<float>(icon->GetWidth()),
                static_cast<float>(icon->GetHeight())};
      }
      return {};
    }
    if (hasVulkanSceneTexture && vulkanRenderer) {
      ImTextureID vkIcon = vulkanRenderer->getOrCreateUIImage(iconPath);
      if (vkIcon != static_cast<ImTextureID>(0)) {
        return {vkIcon, false, 0.0f, 0.0f};
      }
    }
    return {};
  };

  const float barHeight = ImGui::GetWindowHeight();
  const ToolbarIcon sizeProbe = resolveToolbarIcon(
      "Resources/Engine-Root/Editor/New Toolbar Play Type Buttons/Play Button.png");
  const float nativeIconSide =
      (sizeProbe.nativeW > 0.0f) ? sizeProbe.nativeW : 0.0f;
  const float slotSide =
      (nativeIconSide > 0.0f)
          ? nativeIconSide
          : std::max(chrome.buttonSize + 6.0f, std::floor(barHeight - 6.0f));
  const float spacing = std::max(2.0f, chrome.buttonSpacing - 1.0f);
  const float platePadX = 6.0f;
  const float plateRounding = std::floor(slotSide * 0.32f);

  const float timeIconSlot =
      std::max(nativeIconSide > 0.0f ? nativeIconSide : 0.0f,
               ImGui::GetFrameHeight());
  const float timeValueWidth =
      ImGui::CalcTextSize("8.0x").x + timeIconSlot + 14.0f;
  const float timeGroupWidth = timeValueWidth;
  const float separatorWidth = 9.0f;
  const float buttonsWidth = slotSide * 3.0f + spacing * 2.0f;
  const float plateWidth =
      platePadX * 2.0f + timeGroupWidth + separatorWidth + buttonsWidth;

  const float regionMinX = ImGui::GetWindowContentRegionMin().x;
  const float regionMaxX = ImGui::GetWindowContentRegionMax().x;
  float startX = regionMaxX - plateWidth;
  if (startX < regionMinX)
    startX = regionMinX;

  const ImVec2 cursor = ImGui::GetCursorPos();
  const float rowHeight = std::max(slotSide, ImGui::GetFrameHeight());
  const float plateY = std::max(0.0f, std::floor((barHeight - rowHeight) * 0.5f));
  const float buttonY = plateY + std::floor((rowHeight - slotSide) * 0.5f);
  ImGui::SetCursorPos(ImVec2(startX, plateY));

  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 plateMin = ImGui::GetCursorScreenPos();
  const ImVec2 plateMax(plateMin.x + plateWidth, plateMin.y + rowHeight);
  dl->AddRectFilled(plateMin, plateMax, IM_COL32(255, 255, 255, 12),
                    plateRounding);

  ImGui::SetCursorPos(ImVec2(startX + platePadX, plateY));

  auto iconButton = [&](const char *id, const std::string &iconPathOn,
                        const std::string &iconPathOff, const char *fallbackText,
                        const char *tooltip, bool toggled,
                        ImU32 toggledTint) -> bool {
    const ImVec2 slotSize(slotSide, slotSide);
    const ImVec2 slotPos = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, slotSize);
    if (pressed) ImGui::ReportWidgetFeedback(ImGuiWidgetFeedback_Button);
    ImGui::ReportItemHoverFeedback();
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImGuiID buttonId = ImGui::GetID(id);
    UIAnimationState &st = editorUiAnimationStates[buttonId];
    if (uiAnimationMode == UIAnimationMode::Off) {
      st.hover = hovered ? 1.0f : 0.0f;
      st.active = active ? 1.0f : 0.0f;
    } else {
      const float hoverTarget = hovered ? 1.0f : 0.0f;
      const float activeTarget = active ? 1.0f : 0.0f;
      st.hover += (hoverTarget - st.hover) * animStep;
      st.active += (activeTarget - st.active) * animStep;
    }

    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const ImVec2 slotMax(slotPos.x + slotSize.x, slotPos.y + slotSize.y);

    const std::string &iconFile = toggled ? iconPathOn : iconPathOff;
    ToolbarIcon icon = resolveToolbarIcon(iconFile.c_str());

    const float drawW = (icon.nativeW > 0.0f) ? icon.nativeW : slotSize.x;
    const float drawH = (icon.nativeH > 0.0f) ? icon.nativeH : slotSize.y;
    const ImVec2 iconMin(
        std::floor(slotPos.x + (slotSize.x - drawW) * 0.5f),
        std::floor(slotPos.y + (slotSize.y - drawH) * 0.5f));
    const ImVec2 iconMax(iconMin.x + drawW, iconMin.y + drawH);
    const ImVec2 iconCenter((iconMin.x + iconMax.x) * 0.5f,
                            (iconMin.y + iconMax.y) * 0.5f);

    const float slotRounding = std::floor(slotSide * 0.28f);
    const float highlightStrength = std::max(st.hover, st.active);
    if (toggled) {
      drawList->AddRectFilled(slotPos, slotMax, toggledTint, slotRounding);
    } else if (highlightStrength > 0.01f) {
      const int a = static_cast<int>(10.0f + 42.0f * highlightStrength);
      drawList->AddRectFilled(slotPos, slotMax, IM_COL32(255, 255, 255, a),
                              slotRounding);
    }

    int alpha = toggled ? 255 : 205;
    if (hovered && !toggled)
      alpha = 232;
    if (active && !toggled)
      alpha = 248;
    const ImU32 iconTint = IM_COL32(255, 255, 255, alpha);

    if (icon.id != static_cast<ImTextureID>(0)) {
      const ImVec2 uvMin = icon.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
      const ImVec2 uvMax = icon.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
      drawList->AddImage(icon.id, iconMin, iconMax, uvMin, uvMax, iconTint);
    } else {
      ImVec2 textSize = ImGui::CalcTextSize(fallbackText);
      ImVec2 textPos(iconCenter.x - textSize.x * 0.5f,
                     iconCenter.y - textSize.y * 0.5f);
      drawList->AddText(textPos, iconTint, fallbackText);
    }
    if (hovered && tooltip && *tooltip) {
      ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
  };

  static const char *kIconRoot =
      "Resources/Engine-Root/Editor/New Toolbar Play Type Buttons/";
  auto iconPath = [](const char *name) {
    return std::string(kIconRoot) + name;
  };

  {
    const float comboHeight = ImGui::GetFrameHeight();
    const float comboOffsetY =
        std::max(0.0f, std::floor((rowHeight - comboHeight) * 0.5f));
    const ImVec2 comboPos(ImGui::GetCursorScreenPos().x,
                          ImGui::GetCursorScreenPos().y + comboOffsetY);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + comboOffsetY);
    ImGui::SetNextItemWidth(timeValueWidth);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1.0f, 1.0f, 1.0f, 0.14f));

    char timeLabel[16];
    std::snprintf(timeLabel, sizeof(timeLabel), "%.1fx", gameTimeScale);
    const bool comboOpen = ImGui::BeginCombo("##TimeScaleValue", timeLabel,
                                             ImGuiComboFlags_NoArrowButton);
    const ImVec2 comboMin = comboPos;
    const ImVec2 comboMax(comboPos.x + timeValueWidth, comboPos.y + comboHeight);
    if (comboOpen) {
      const float presets[] = {0.0f, 0.1f, 0.2f, 0.5f, 1.0f,
                               1.5f, 2.0f, 3.0f, 4.0f, 8.0f};
      for (float preset : presets) {
        char label[32];
        if (preset <= 0.0f) {
          std::snprintf(label, sizeof(label), "Freeze (0.0x)");
        } else {
          std::snprintf(label, sizeof(label), "%.1fx", preset);
        }
        const bool selected = std::abs(gameTimeScale - preset) < 0.001f;
        if (ImGui::Selectable(label, selected)) {
          gameTimeScale = preset;
          saveEditorUserSettings();
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    const bool comboHovered =
        ImGui::IsMouseHoveringRect(comboMin, comboMax) && !comboOpen;
    if (comboHovered) {
      ImGui::SetTooltip("Time Scale: %.1fx\nSlows or speeds up gameplay, "
                        "animation, physics and video.\nThe editor UI keeps "
                        "running at full speed.",
                        gameTimeScale);
    }

    const std::string clockPath = iconPath("Time slider.png");
    ToolbarIcon clock = resolveToolbarIcon(clockPath.c_str());
    const float clockW = (clock.nativeW > 0.0f) ? clock.nativeW : comboHeight;
    const float clockH = (clock.nativeH > 0.0f) ? clock.nativeH : comboHeight;
    const ImVec2 clockMin(
        std::floor(comboMax.x - clockW - 3.0f),
        std::floor(comboMin.y + (comboHeight - clockH) * 0.5f));
    const ImVec2 clockMax(clockMin.x + clockW, clockMin.y + clockH);
    const bool scaled = std::abs(gameTimeScale - 1.0f) > 0.001f;
    const int clockAlpha = (comboHovered || comboOpen) ? 255 : (scaled ? 250 : 225);
    if (clock.id != static_cast<ImTextureID>(0)) {
      const ImVec2 uvMin = clock.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
      const ImVec2 uvMax = clock.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
      dl->AddImage(clock.id, clockMin, clockMax, uvMin, uvMax,
                   IM_COL32(255, 255, 255, clockAlpha));
    } else {
      dl->AddText(clockMin, IM_COL32(255, 255, 255, clockAlpha), "T");
    }

    ImGui::SetCursorPosY(plateY);
  }

  ImGui::SameLine(0.0f, 0.0f);
  {
    const ImVec2 sepPos = ImGui::GetCursorScreenPos();
    const float sepX = std::floor(sepPos.x + separatorWidth * 0.5f);
    const float sepInset = std::floor(rowHeight * 0.22f);
    dl->AddLine(ImVec2(sepX, sepPos.y + sepInset),
                ImVec2(sepX, sepPos.y + rowHeight - sepInset),
                IM_COL32(255, 255, 255, 32), 1.0f);
    ImGui::Dummy(ImVec2(separatorWidth, rowHeight));
  }
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::SetCursorPosY(buttonY);

  bool playPressed =
      iconButton("##PlayModeToolbarButton", iconPath("Play Button ON.png"),
                 iconPath("Play Button.png"), "P", playTooltip, isPlaying,
                 IM_COL32(86, 186, 122, 92));
  {
    static int autoplayCountdown = []() {
      const char *v = std::getenv("MODULARITY_DEBUG_AUTOPLAY");
      const int frames = v ? std::atoi(v) : 0;
      return frames > 0 ? frames : -1;
    }();
    if (autoplayCountdown > 0 && !isPlaying &&
        projectManager.currentProject.isLoaded) {
      if (--autoplayCountdown == 0) {
        playPressed = true;
        std::fprintf(stderr, "[Debug] MODULARITY_DEBUG_AUTOPLAY entering play mode\n");
      }
    }
  }
  ImGui::SameLine(0.0f, spacing);
  bool specPressed =
      iconButton("##SpecModeToolbarButton", iconPath("Spec Mode Button ON.png"),
                 iconPath("Spec Mode Button.png"), "S", specTooltip, specMode,
                 IM_COL32(214, 158, 74, 92));
  ImGui::SameLine(0.0f, spacing);
  bool pausePressed =
      iconButton("##PauseToolbarButton", iconPath("Pause Button ON.png"),
                 iconPath("Pause Button.png"), "||", pauseTooltip, isPaused,
                 IM_COL32(226, 96, 96, 92));

  ImGui::SetCursorPos(cursor);

  if (playPressed) {
    ImGui::ClearActiveID();
    togglePlayMode();
  }
  if (pausePressed) {
    togglePause();
  }
  if (specPressed) {
    ImGui::ClearActiveID();
    toggleSpecMode();
  }
}

void Engine::togglePlayMode() {
    {
      bool newState = !isPlaying;
      if (newState) {
        const auto __playStart = std::chrono::steady_clock::now();
        auto __markPlayStep = [&](const char *label, const auto &stepStart) {
          const double ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - stepStart)
                                .count();
          if (ms > Modularity::HardwareProfile::TimingBudget(10.0)) {
            std::fprintf(stderr, "[ModuTimer] play enter %s %.2f ms\n", label,
                         ms);
          }
        };
        auto __stepStart = std::chrono::steady_clock::now();
        clearVideoPlayers();
        videoAssetPreviewPlayer.reset();
        videoAssetPreviewPath.clear();
        __markPlayStep("clearPreviews", __stepStart);
        // Reset script module state so Begin/static script state is fresh each
        // play session.
        __stepStart = std::chrono::steady_clock::now();
        resetScriptRuntimeStateForReload(false);
        __markPlayStep("scriptReset", __stepStart);
        __stepStart = std::chrono::steady_clock::now();
        capturePlayModeSnapshot();
        __markPlayStep("snapshot", __stepStart);
        deferInspectorRefresh = true;
        for (SceneObject &obj : sceneObjects) {
          obj.editorSectorHidden = false;
        }
        __stepStart = std::chrono::steady_clock::now();
        for (SceneObject &obj : sceneObjects) {
          if (!obj.hasAnimation)
            continue;
          obj.animation.runtimePlaying = false;
          obj.animation.runtimePaused = false;
          obj.animation.runtimeTime = 0.0f;
          obj.animation.runtimeDirection = 1.0f;
          obj.animation.runtimeInitialized = false;
          obj.animation.runtimeClipPath.clear();
        }
        __markPlayStep("animations", __stepStart);
        __stepStart = std::chrono::steady_clock::now();
        if (physics->isReady() || physics->init()) {
          physics->onPlayStart(sceneObjects);
        } else {
          addConsoleMessage(
              "PhysX failed to initialize; physics disabled for play mode",
              ConsoleMessageType::Warning);
        }
        __markPlayStep("physics", __stepStart);
        __stepStart = std::chrono::steady_clock::now();
        audio.setPrefer2DSpatialAudio(isProject2DPipeline() || uiWorldMode);
        audio.onPlayStart(sceneObjects);
        __markPlayStep("audio", __stepStart);
        bool hasPlayerController = false;
        for (const auto &obj : sceneObjects) {
          if (IsObjectEnabledInHierarchy(obj) && obj.hasPlayerController &&
              obj.playerController.enabled) {
            hasPlayerController = true;
            break;
          }
        }
        if (hasPlayerController && showGameViewport) {
          gameViewCursorLocked = true;
          gameViewportFocused = true;
        }
        const double __playMs = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() -
                                    __playStart)
                                    .count();
        std::fprintf(stderr,
                     "[ModuTimer] play enter total %.2f ms objects=%zu scripts=%zu textures=%zu shaders=%zu previews=%zu reflections=%zu\n",
                     __playMs, sceneObjects.size(), runtimeScriptBindings.size(),
                     rendererInitialized ? renderer.getTextureCacheCount() : 0,
                     rendererInitialized ? renderer.getShaderCacheCount() : 0,
                     rendererInitialized ? renderer.getExtraPreviewTargetCount() : 0,
                     rendererInitialized ? renderer.getReflectionCastTargetCount() : 0);
      } else {
        const auto __playStop = std::chrono::steady_clock::now();
        clearVideoPlayers();
        videoAssetPreviewPlayer.reset();
        videoAssetPreviewPath.clear();
        physics->onPlayStop();
        audio.onPlayStop();
        restorePlayModeSnapshot();
        deferInspectorRefresh = true;
        resetScriptRuntimeStateForReload(false);
        isPaused = false;
        if (specMode && (physics->isReady() || physics->init())) {
          physics->onPlayStart(sceneObjects);
        }
        const double __stopMs = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() -
                                    __playStop)
                                    .count();
        std::fprintf(stderr, "[ModuTimer] play exit total %.2f ms\n", __stopMs);
      }
      isPlaying = newState;
    }
}

void Engine::togglePause() {
    isPaused = !isPaused;
    if (isPaused)
      isPlaying = true;
}

void Engine::toggleSpecMode() {
    bool enable = !specMode;
    if (enable && !physics->isReady() && !physics->init()) {
      addConsoleMessage("PhysX failed to initialize; spec mode disabled",
                        ConsoleMessageType::Warning);
      enable = false;
    }
    if (specMode != enable) {
      deferInspectorRefresh = true;
      resetScriptRuntimeStateForReload(false);
    }
    specMode = enable;
    if (!isPlaying) {
      if (specMode) {
        clearVideoPlayers();
        videoAssetPreviewPlayer.reset();
        videoAssetPreviewPath.clear();
        physics->onPlayStart(sceneObjects);
        audio.setPrefer2DSpatialAudio(isProject2DPipeline() || uiWorldMode);
        audio.onPlayStart(sceneObjects);
      } else {
        clearVideoPlayers();
        videoAssetPreviewPlayer.reset();
        videoAssetPreviewPath.clear();
        physics->onPlayStop();
        audio.onPlayStop();
      }
    }
}
