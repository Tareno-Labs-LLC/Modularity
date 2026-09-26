#include "../../EditorLocalization.h"
#include "Engine.h"

namespace Loc = Modularity::Loc;

namespace {

const char *const kLocalizedWindowIds[] = {
    "Viewport",           "Game Viewport",       "Hierarchy",
    "Inspector",          "Project",             "Project Settings",
    "Environment",        "Scripting",           "Animation",
    "Build Settings",     "Profiler",            "Camera",
    "AI Pathfinding",     "Sector Map",          "Modularity Doctor",
    "Modupak Manager",    "Pixel Sprite Editor", "Visual Script Graph",
    "Assemblage",
    "Console",           "Console##MiniLogPanel",
};

void loadWorkspaceIniWithLocalizedWindowIds(const fs::path &layoutPath) {
  std::ifstream in(layoutPath, std::ios::binary);
  if (!in.is_open()) {
    ImGui::LoadIniSettingsFromDisk(layoutPath.string().c_str());
    return;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  std::string text = buffer.str();
  in.close();

  bool migrated = false;
  for (const char *name : kLocalizedWindowIds) {
    const std::string legacy = std::string("[Window][") + name + "]";
    const std::string current = std::string("[Window][###") + name + "]";
    size_t pos = 0;
    while ((pos = text.find(legacy, pos)) != std::string::npos) {
      text.replace(pos, legacy.size(), current);
      pos += current.size();
      migrated = true;
    }
  }

  if (!migrated) {
    ImGui::LoadIniSettingsFromDisk(layoutPath.string().c_str());
    return;
  }
  ImGui::LoadIniSettingsFromMemory(text.c_str(), text.size());
}

bool layoutFileNeedsUtilityDockMigration(const fs::path &layoutPath,
                                         bool projectSettingsVisible,
                                         bool modupakVisible) {
  std::ifstream in(layoutPath);
  if (!in.is_open()) {
    return false;
  }

  std::string currentWindow;
  std::string projectDockId;
  std::string projectSettingsDockId;
  bool hasModupakWindow = false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("[Window][", 0) == 0) {
      const size_t close = line.find(']', 9);
      currentWindow = (close == std::string::npos) ? std::string()
                                                   : line.substr(9, close - 9);
      if (currentWindow == "Modupak Manager") {
        hasModupakWindow = true;
      }
      continue;
    }

    if (line.rfind("DockId=", 0) != 0) {
      continue;
    }

    const std::string dockId = line.substr(
        7, line.find(',') == std::string::npos ? std::string::npos
                                               : line.find(',') - 7);
    if (currentWindow == "Project" && projectDockId.empty()) {
      projectDockId = dockId;
    } else if (currentWindow == "Project Settings" &&
               projectSettingsDockId.empty()) {
      projectSettingsDockId = dockId;
    }
  }

  if (projectSettingsVisible && !projectDockId.empty() &&
      !projectSettingsDockId.empty() &&
      projectDockId == projectSettingsDockId) {
    return true;
  }

  if (modupakVisible && !hasModupakWindow) {
    return true;
  }

  return false;
}

struct EditorWindowIcon {
  const char *iconFile;
  const char *windowName;
  const char *rawName;
  const char *widgetKey;
};

const EditorWindowIcon kEditorWindowIcons[] = {
    {"Viewport", "Viewport", nullptr, nullptr},
    {"Game Viewport", "Game Viewport", nullptr, nullptr},
    {"Hierarchy", "Hierarchy", nullptr, nullptr},
    {"Inspector", "Inspector", nullptr, nullptr},
    {"Project", "Project", nullptr, "MENU_WORKFLOW_FILE_BROWSER"},
    {"Project Settings", "Project Settings", nullptr, nullptr},
    {"Environment", "Environment", nullptr, nullptr},
    {"Camera", "Camera", nullptr, nullptr},
    {"Animation", "Animation", nullptr, nullptr},
    {"Modularity Doctor", "Modularity Doctor", nullptr, nullptr},
    {"ModuPAK Manager", "Modupak Manager", nullptr,
     "MENU_PACKAGES_MODUPAK_MANAGER"},
    {"Style Editor", "Style Editor", "Style Editor", nullptr},
};
} // namespace

void Engine::refreshEditorWindowIcons() {
  const bool hasVulkanUiImages =
      usingVulkan() && vulkanRendererInitialized && (vulkanRenderer != nullptr);

  for (const EditorWindowIcon &binding : kEditorWindowIcons) {
    const std::string iconPath =
        std::string("Resources/ModuGUI Tabs and Window Icons/") +
        binding.iconFile + ".png";

    ImTextureID textureId = static_cast<ImTextureID>(0);
    ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
    if (rendererInitialized) {
      if (Texture *icon = renderer.getTexture(
              iconPath, MaterialProperties::TextureFilter::Bilinear);
          icon && icon->GetID()) {
        textureId = static_cast<ImTextureID>(icon->GetID());
        uv0 = ImVec2(0.0f, 1.0f);
        uv1 = ImVec2(1.0f, 0.0f);
      }
    }
    if (textureId == static_cast<ImTextureID>(0) && hasVulkanUiImages) {
      textureId = vulkanRenderer->getOrCreateUIImage(iconPath);
    }

    if (binding.windowName) {
      ImGui::SetNamedIcon(Loc::WindowRef(binding.windowName), textureId, uv0,
                          uv1);
    }
    if (binding.rawName) {
      ImGui::SetNamedIcon(binding.rawName, textureId, uv0, uv1);
    }
    if (binding.widgetKey) {
      ImGui::SetNamedIcon((std::string("###") + binding.widgetKey).c_str(),
                          textureId, uv0, uv1);
    }
  }
}

void Engine::renderMainMenuBar() {
  refreshScriptEditorWindows();
  pollEditorThemeFile();

  static bool triggerAboutModularityPopup = false;

  if (ImGui::BeginMainMenuBar()) {
    const EditorChromeMetrics &chrome = getEditorChromeMetrics(uiChromeScale);
    ImVec2 menuItemSpacing = chrome.menuItemSpacing;
    ImVec2 menuFramePadding = chrome.menuFramePadding;
    float menuFontScale = chrome.fontScale;
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_IsTouchScreen) {
      menuItemSpacing.x *= 1.6f;
      menuFramePadding.x *= 1.8f;
      menuFramePadding.y *= 2.6f;
      if (menuFontScale < 1.0f) menuFontScale = 1.0f;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, menuItemSpacing);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, menuFramePadding);
    ImGui::SetWindowFontScale(menuFontScale);

    if (ImGui::BeginMenu(Loc::T("MENU_ENGINE", "Engine"))) {
      if (ImGui::MenuItem(Loc::T("MENU_ENGINE_NEW_SCENE", "New Scene"), "Ctrl+N")) {
        showNewSceneDialog = true;
        memset(newSceneName, 0, sizeof(newSceneName));
      }
      if (ImGui::MenuItem(Loc::T("MENU_ENGINE_SAVE_SCENE", "Save Scene"), "Ctrl+S")) {
        saveCurrentScene();
      }
      if (ImGui::MenuItem(Loc::T("MENU_ENGINE_SAVE_SCENE_AS", "Save Scene As..."), "Ctrl+Shift+S")) {
        showSaveSceneAsDialog = true;
        strncpy(saveSceneAsName,
                projectManager.currentProject.currentSceneName.c_str(),
                sizeof(saveSceneAsName) - 1);
      }
      if (ImGui::BeginMenu(Loc::T("MENU_ENGINE_LOAD_SCENE", "Load Scene"),
                           projectManager.currentProject.isLoaded)) {
        const auto scenes = projectManager.currentProject.getSceneList();
        if (scenes.empty()) {
          ImGui::TextDisabled("%s", Loc::T("MENU_ENGINE_NO_SCENES", "No scenes"));
        }
        for (const auto &scene : scenes) {
          const bool isCurrentScene =
              scene == projectManager.currentProject.currentSceneName;
          if (ImGui::MenuItem(scene.c_str(), nullptr, isCurrentScene,
                              !isCurrentScene)) {
            loadScene(scene);
          }
        }
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem(Loc::T("MENU_ENGINE_BUILD_SETTINGS", "Build Settings..."))) {
        showBuildSettings = true;
      }
      ImGui::Separator();
      if (ImGui::MenuItem(Loc::T("MENU_ENGINE_RETURN_TO_PROJECT_MANAGER", "Return to Project Manager"))) {
        if (projectManager.currentProject.hasUnsavedChanges) {
          requestSceneSave(projectManager.currentProject.currentSceneName,
                           PendingScenePostAction::CloseProject,
                           "",
                           true);
        } else {
          performCloseProject();
        }
      }
      ImGui::Separator();
      if (ImGui::MenuItem(Loc::T("MENU_ENGINE_EXIT", "Exit"))) {
        glfwSetWindowShouldClose(editorWindow, true);
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(Loc::T("MENU_ACTIONS", "Actions"))) {
      if (ImGui::MenuItem(Loc::T("MENU_ACTIONS_UNDO", "Undo"), "Ctrl+Z", false, !undoStack.empty())) {
        undo();
      }
      if (ImGui::MenuItem(Loc::T("MENU_ACTIONS_REDO", "Redo"), "Ctrl+Y", false, !redoStack.empty())) {
        redo();
      }
      ImGui::Separator();
      const bool hasSelection =
          !selectedObjectIds.empty() || selectedObjectId >= 0;
      if (ImGui::MenuItem("Copy", "Ctrl+C", false, hasSelection)) {
        copySelected();
      }
      if (ImGui::MenuItem("Paste", "Ctrl+V", false, !objectClipboard.empty())) {
        pasteClipboard();
      }
      if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSelection)) {
        duplicateSelected();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Select All", "Ctrl+A", false,
                          !sceneObjects.empty())) {
        selectAllObjects();
      }
      if (ImGui::MenuItem("Clear Selection", nullptr, false, hasSelection)) {
        clearSelection();
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(Loc::T("MENU_WORKFLOW", "Workflow"))) {
      if (ImGui::BeginMenu(Loc::T("MENU_WORKFLOW_WORKSPACE", "Workspace"))) {
        const bool workspaceTransitionActive =
            pendingWorkspaceReload || workspaceLayoutDirty ||
            glfwGetTime() < workspaceLayoutStabilizeUntil;
        const bool workspaceSwitchLocked =
            glfwGetTime() < workspaceSwitchLockUntil;

        auto switchWorkspace = [&](WorkspaceMode mode) {
          if (currentWorkspace == mode) {
            return;
          }
          if (workspaceTransitionActive || workspaceSwitchLocked) {
            return;
          }
          saveWorkspaceLayout(currentWorkspace);
          applyWorkspacePreset(mode, true);
          workspaceSwitchLockUntil = glfwGetTime() + 0.22;
        };

        const bool canSwitchWorkspace =
            !workspaceTransitionActive && !workspaceSwitchLocked;
        if (ImGui::MenuItem(Loc::T("MENU_WORKFLOW_WORKSPACE_DEFAULT", "Default"), nullptr,
                            currentWorkspace == WorkspaceMode::Default,
                            canSwitchWorkspace)) {
          switchWorkspace(WorkspaceMode::Default);
        }
        if (ImGui::MenuItem(Loc::T("WINDOW_ANIMATION", "Animation"), nullptr,
                            currentWorkspace == WorkspaceMode::Animation,
                            canSwitchWorkspace)) {
          switchWorkspace(WorkspaceMode::Animation);
        }
        if (hasScriptingWindowPackage()) {
          if (ImGui::MenuItem(Loc::T("WINDOW_SCRIPTING", "Scripting"), nullptr,
                              currentWorkspace == WorkspaceMode::Scripting,
                              canSwitchWorkspace)) {
            switchWorkspace(WorkspaceMode::Scripting);
          }
        }
        ImGui::EndMenu();
      }

      bool prevViewport = showViewport;
      ImGui::MenuItem(Loc::Window("WINDOW_VIEWPORT", "Viewport"), nullptr, &showViewport);
      bool prevGameViewport = showGameViewport;
      ImGui::MenuItem(Loc::Window("WINDOW_GAME_VIEWPORT", "Game Viewport"), nullptr,
                      &showGameViewport);
      if (prevViewport != showViewport || prevGameViewport != showGameViewport) {
        saveEditorUserSettings();
      }
      ImGui::Separator();
      ImGui::MenuItem(Loc::Window("WINDOW_HIERARCHY", "Hierarchy"), "Ctrl+4", &showHierarchy);
      ImGui::MenuItem(Loc::Window("WINDOW_INSPECTOR", "Inspector"), "Ctrl+3", &showInspector);
      ImGui::MenuItem(Loc::Widget("MENU_WORKFLOW_FILE_BROWSER", "File Browser"), "Ctrl+5", &showFileBrowser);
      ImGui::MenuItem(Loc::T("WINDOW_CONSOLE", "Console"), nullptr, &showConsole);
      if (hasScriptingWindowPackage()) {
        ImGui::MenuItem(Loc::T("WINDOW_SCRIPTING", "Scripting"), nullptr, &showScriptingWindow);
      }
      bool prevProjectBrowser = showProjectBrowser;
      ImGui::MenuItem(Loc::Window("WINDOW_PROJECT_SETTINGS", "Project Settings"), nullptr, &showProjectBrowser);
      if (prevProjectBrowser != showProjectBrowser) {
        saveEditorUserSettings();
      }
      /*if (hasMeshBuilderPackage()) {
        ImGui::MenuItem("Mesh Builder (Legacy Window)", nullptr,
                        &showMeshBuilder);
      }*/
      ImGui::MenuItem(Loc::Window("WINDOW_ENVIRONMENT", "Environment"), nullptr, &showEnvironmentWindow);
      ImGui::MenuItem(Loc::Window("WINDOW_CAMERA", "Camera"), nullptr, &showCameraWindow);
      bool prevAnimationWindow = showAnimationWindow;
      ImGui::MenuItem(Loc::Window("WINDOW_ANIMATION", "Animation"), nullptr, &showAnimationWindow);
      if (prevAnimationWindow != showAnimationWindow) {
        saveEditorUserSettings();
      }
      bool prevAIPathWindow = showAIPathfindingWindow;
      ImGui::MenuItem(Loc::T("WINDOW_AI_PATHFINDING", "AI Pathfinding"), nullptr, &showAIPathfindingWindow);
      if (prevAIPathWindow != showAIPathfindingWindow) {
        saveEditorUserSettings();
      }
      if (hasSpriteEditorPackage()) {
        bool prevPixelSpriteEditor = showPixelSpriteEditorWindow;
        ImGui::MenuItem(Loc::T("WINDOW_PIXEL_SPRITE_EDITOR", "Pixel Sprite Editor"), nullptr,
                        &showPixelSpriteEditorWindow);
        if (prevPixelSpriteEditor != showPixelSpriteEditorWindow) {
          saveEditorUserSettings();
        }
      }
      {
        bool prevAssemblage = showAssemblageWindow;
        ImGui::MenuItem(Loc::T("WINDOW_ASSEMBLAGE", "Assemblage"), nullptr,
                        &showAssemblageWindow);
        if (prevAssemblage != showAssemblageWindow) {
          saveEditorUserSettings();
        }
      }
      ImGui::MenuItem(Loc::T("MENU_WORKFLOW_VIEW_OUTPUT", "View Output"), nullptr, &showViewOutput);
      ImGui::Separator();
      if (isProject2DPipeline()) {
        bool forced2DOverlay = true;
        ImGui::BeginDisabled();
        ImGui::MenuItem(Loc::T("MENU_WORKFLOW_UI_WORLD_OVERLAY", "UI World Overlay"), nullptr, &forced2DOverlay);
        ImGui::EndDisabled();
      } else {
        ImGui::MenuItem(Loc::T("MENU_WORKFLOW_UI_WORLD_OVERLAY", "UI World Overlay"), nullptr, &uiWorldMode);
      }
      ImGui::MenuItem(Loc::T("MENU_WORKFLOW_GRID_3D", "3D Grid"), nullptr, &showSceneGrid3D);
      ImGui::Separator();
      if (ImGui::MenuItem(Loc::T("MENU_WORKFLOW_FULLSCREEN_VIEWPORT", "Fullscreen Viewport"), "F11", viewportFullscreen)) {
        viewportFullscreen = !viewportFullscreen;
      }
      ImGui::Separator();
      if (ImGui::BeginMenu(Loc::T("MENU_WORKFLOW_STYLE", "Style"))) {
        ImGui::TextDisabled("%s", Loc::T("MENU_WORKFLOW_EDITOR_STYLES", "Editor Styles"));
        for (size_t i = 0; i < uiStylePresets.size(); ++i) {
          bool selected = static_cast<int>(i) == uiStylePresetIndex;
          if (ImGui::MenuItem(uiStylePresets[i].name.c_str(), nullptr,
                              selected)) {
            applyUIStylePresetByName(uiStylePresets[i].name);
            saveEditorUserSettings();
          }
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(Loc::T("MENU_WORKFLOW_UI_SCALE", "UI Scale"))) {
          const auto selectScale = [&](EditorChromeScale scale) {
            if (uiChromeScale == scale) {
              return;
            }
            uiChromeScale = scale;
            saveEditorUserSettings();
          };
          if (ImGui::MenuItem(Loc::T("MENU_WORKFLOW_UI_SCALE_BIG", "Big"), nullptr,
                              uiChromeScale == EditorChromeScale::Big)) {
            selectScale(EditorChromeScale::Big);
          }
          if (ImGui::MenuItem(Loc::T("MENU_WORKFLOW_UI_SCALE_DEFAULT", "Default"), nullptr,
                              uiChromeScale == EditorChromeScale::Default)) {
            selectScale(EditorChromeScale::Default);
          }
          if (ImGui::MenuItem(Loc::T("MENU_WORKFLOW_UI_SCALE_COMPACT", "Compact"), nullptr,
                              uiChromeScale == EditorChromeScale::Compact)) {
            selectScale(EditorChromeScale::Compact);
          }
          ImGui::EndMenu();
        }
        ImGui::TextDisabled("%s", Loc::T("MENU_WORKFLOW_UI_ANIMATIONS", "UI Animations"));
        if (ImGui::MenuItem(Loc::T("MENU_WORKFLOW_ANIM_FLUID", "Fluid"), nullptr,
                            uiAnimationMode == UIAnimationMode::Fluid)) {
          uiAnimationMode = UIAnimationMode::Fluid;
          saveEditorUserSettings();
        }
        if (ImGui::MenuItem(Loc::T("MENU_WORKFLOW_ANIM_SNAPPY", "Snappy"), nullptr,
                            uiAnimationMode == UIAnimationMode::Snappy)) {
          uiAnimationMode = UIAnimationMode::Snappy;
          saveEditorUserSettings();
        }
        if (ImGui::MenuItem(Loc::T("MENU_WORKFLOW_ANIM_OFF", "Off"), nullptr,
                            uiAnimationMode == UIAnimationMode::Off)) {
          uiAnimationMode = UIAnimationMode::Off;
          saveEditorUserSettings();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(Loc::T("MENU_WORKFLOW_FEEDBACK_SOUNDS", "Feedback Sounds"))) {
          ImGui::TextDisabled("%s", Loc::T("MENU_WORKFLOW_SOUND_THEME", "Sound Theme"));
          // Selecting rescans the theme folders, which frees the list being drawn, so the
          // pick is applied after the loop. Re-selecting the current theme reloads its theme.ini.
          std::string pickedSoundTheme;
          const std::string activeSoundTheme = editorSoundThemes.activeId();
          for (const auto& theme : editorSoundThemes.themes()) {
            if (ImGui::MenuItem(theme.displayName.c_str(), nullptr, theme.id == activeSoundTheme)) {
              pickedSoundTheme = theme.id;
            }
            if (!theme.description.empty() && ImGui::IsItemHovered()) {
              ImGui::SetTooltip("%s", theme.description.c_str());
            }
          }
          if (!pickedSoundTheme.empty()) {
            selectEditorSoundTheme(pickedSoundTheme);
          }
          ImGui::Separator();
          bool feedbackSettingsChanged = false;
          ImGui::SetNextItemWidth(140.0f);
          feedbackSettingsChanged |= ImGui::SliderFloat(Loc::T("MENU_WORKFLOW_SOUNDS_VOLUME", "UI Volume"), &feedbackSoundVolume, 0.0f, 1.0f, "%.2f");
          if (ImGui::IsItemDeactivatedAfterEdit()) {
            playEditorSound(EditorSoundCue::Confirm);  // hear the new level on release
          }
          feedbackSettingsChanged |= ImGui::MenuItem(Loc::T("MENU_WORKFLOW_SOUNDS_ENABLE_ALL", "Enable All Feedback Sounds"), nullptr, &feedbackSoundsEnabled);
          ImGui::Separator();
          feedbackSettingsChanged |= ImGui::MenuItem(Loc::T("MENU_WORKFLOW_SOUNDS_CLICK", "Click Sounds"), nullptr, &feedbackClickSoundsEnabled);
          feedbackSettingsChanged |= ImGui::MenuItem(Loc::T("MENU_WORKFLOW_SOUNDS_ERROR", "Error Sounds"), nullptr, &feedbackErrorSoundsEnabled);
          feedbackSettingsChanged |= ImGui::MenuItem(Loc::T("MENU_WORKFLOW_SOUNDS_OTHER", "Other Feedback Sounds"), nullptr, &feedbackOtherSoundsEnabled);
          ImGui::Separator();
          ImGui::TextDisabled("%s", Loc::T("MENU_WORKFLOW_SOUNDS_INTERFACE", "Interface Sounds"));
          feedbackSettingsChanged |= ImGui::MenuItem(Loc::T("MENU_WORKFLOW_SOUNDS_BUTTONS", "Button & Tab Sounds"), nullptr, &feedbackButtonSoundsEnabled);
          feedbackSettingsChanged |= ImGui::MenuItem(Loc::T("MENU_WORKFLOW_SOUNDS_TOGGLES", "Toggle Sounds"), nullptr, &feedbackToggleSoundsEnabled);
          feedbackSettingsChanged |= ImGui::MenuItem(Loc::T("MENU_WORKFLOW_SOUNDS_SLIDES", "Slide-out Sounds"), nullptr, &feedbackSlideSoundsEnabled);
          feedbackSettingsChanged |= ImGui::MenuItem(Loc::T("MENU_WORKFLOW_SOUNDS_TRANSITIONS", "Transition Sounds"), nullptr, &feedbackTransitionSoundsEnabled);
          feedbackSettingsChanged |= ImGui::MenuItem(Loc::T("MENU_WORKFLOW_SOUNDS_HOVER", "Hover Sounds"), nullptr, &feedbackHoverSoundsEnabled);
          ImGui::Separator();
          ImGui::TextDisabled("%s", Loc::T("MENU_WORKFLOW_SOUNDS_BOOT_NOTE", "Boot intro sound is always enabled."));
          if (feedbackSettingsChanged) {
            saveEditorUserSettings();
          }
          ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem(Loc::Window("WINDOW_STYLE_EDITOR", "Style Editor"), nullptr, &showStyleEditor);
        if (ImGui::MenuItem(Loc::T("MENU_WORKFLOW_EXPORT_THEME", "Export Theme + Layout"))) {
          exportEditorThemeLayout();
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(Loc::T("MENU_PACKAGES", "Packages"))) {
      bool prevRegistryPackages = showRegistryPackagesWindow;
      ImGui::MenuItem(Loc::Widget("MENU_PACKAGES_MODUPAK_MANAGER", "ModuPAK Manager"), nullptr, &showRegistryPackagesWindow);
      if (prevRegistryPackages != showRegistryPackagesWindow) {
        saveEditorUserSettings();
      }
      ImGui::Separator();
      if (ImGui::MenuItem(Loc::T("MENU_PACKAGES_EXPORT_MODUPAK", "Export Into ModuPAK..."))) {
        openModuPakExportDialog(fileBrowser.selectedFiles);
      }
      if (ImGui::MenuItem(Loc::T("MENU_PACKAGES_IMPORT_MODUPAK", "Import ModuPAK..."))) {
        openModuPakImportDialog();
      }
      if (ImGui::MenuItem(Loc::T("MENU_PACKAGES_IMPORT_MODUOBJ", "Import ModuOBJ..."))) {
        openModuObjImportDialog();
      }
      if (!scriptEditorWindows.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Scripted Windows");
        for (auto &window : scriptEditorWindows) {
          ImGui::MenuItem(window.label.c_str(), nullptr, &window.open);
        }
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(Loc::T("MENU_TOOLS", "Tools"))) {
      ImGui::MenuItem(Loc::Window("WINDOW_DOCTOR", "Modularity Doctor"), nullptr, &showModularityDoctorWindow);
#if !MODULARITY_RUNTIME_ONLY
      bool prevVersionControl = showVersionControlWindow;
      ImGui::MenuItem(Loc::Window("WINDOW_VERSION_CONTROL", "Version Control"), nullptr,
                      &showVersionControlWindow);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Back the project up with Git and open it on other computers");
      }
      if (prevVersionControl != showVersionControlWindow) {
        saveEditorUserSettings();
      }
#endif
      bool prevProfilerWindow = showGameProfilerWindow;
      ImGui::MenuItem("Profiler", nullptr, &showGameProfilerWindow);
      if (prevProfilerWindow != showGameProfilerWindow) {
        saveEditorUserSettings();
      }
      ImGui::MenuItem("Sector Map", nullptr, &showSectorMapWindow);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Visual map of sectors (rooms) and their transitions");
      }
      bool prevLightmapping = showLightmappingWindow;
      ImGui::MenuItem(Loc::Window("WINDOW_LIGHTMAPPING", "Lightmapping"), nullptr,
                      &showLightmappingWindow);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Bake static lighting with Nebula");
      }
      if (prevLightmapping != showLightmappingWindow) {
        saveEditorUserSettings();
      }
      ImGui::Separator();
#if !MODULARITY_RUNTIME_ONLY
      if (ImGui::MenuItem("Transcode EXR Textures")) {
        transcodeProjectExrTextures();
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Convert EXRs the bundled reader cannot decompress (Poly Haven\n"
                          "ships DWAA) into PNGs beside them, using Blender.\n"
                          "\n"
                          "One-off, and the result is part of the project - a shipped\n"
                          "build needs neither Blender nor the original EXR.");
      }
#endif
      if (ImGui::MenuItem("Auto-Assign Material Maps")) {
        autoAssignProjectMaterialMaps();
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Fill empty roughness / metallic / opacity / emissive /\n"
                          "occlusion slots on every material, by the same filename\n"
                          "conventions the model importer uses.\n"
                          "\n"
                          "Only fills slots that are empty - nothing you assigned\n"
                          "by hand is touched.");
      }
      ImGui::Separator();
      bool prevXRPreview = showXRPreviewWindow;
      ImGui::MenuItem(Loc::Window("WINDOW_XR_PREVIEW", "Stereo Preview"), nullptr,
                      &showXRPreviewWindow);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Render both eyes at headset resolution and time them,\n"
                          "with or without a headset attached");
      }
      if (prevXRPreview != showXRPreviewWindow) {
        if (!showXRPreviewWindow) {
          renderer.releaseXREyePreviews();
          xrPreview.eyeTexture[0] = 0;
          xrPreview.eyeTexture[1] = 0;
          xrPreview.eyeGpuMs[0] = -1.0;
          xrPreview.eyeGpuMs[1] = -1.0;
        }
        saveEditorUserSettings();
      }
      ImGui::Separator();
      ImGui::TextDisabled("Script Modes");
      auto toggleSpec = [&](bool enabled) {
        if (specMode == enabled)
          return;
        if (enabled && !physics->isReady() && !physics->init()) {
          addConsoleMessage("PhysX failed to initialize; spec mode disabled",
                            ConsoleMessageType::Warning);
          specMode = false;
          return;
        }
        deferInspectorRefresh = true;
        resetScriptRuntimeStateForReload(false);
        specMode = enabled;
        if (!isPlaying) {
          if (specMode) {
            physics->onPlayStart(sceneObjects);
            audio.setPrefer2DSpatialAudio(isProject2DPipeline() || uiWorldMode);
            audio.onPlayStart(sceneObjects);
          } else {
            physics->onPlayStop();
            audio.onPlayStop();
          }
        }
      };
      bool specValue = specMode;
      if (ImGui::MenuItem("Spec Mode (run Script_Spec)", nullptr, &specValue)) {
        toggleSpec(specValue);
      }
      bool testValue = testMode;
      if (ImGui::MenuItem("Test Mode (run Script_TestEditor)", nullptr,
                          &testValue)) {
        if (testMode != testValue) {
          deferInspectorRefresh = true;
          resetScriptRuntimeStateForReload(false);
        }
        testMode = testValue;
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(Loc::T("MENU_SCENE_OBJ", "SceneOBJ"))) {
      renderSceneObjectCreateMenu();
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(Loc::T("MENU_HELP", "Help"))) {
      if (ImGui::MenuItem("About")) {
        triggerAboutModularityPopup = true;
      }
      ImGui::EndMenu();
    }

    if (!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_IsTouchScreen) ||
        !mobileEditorLayout) {
      renderPlayControlsBar();
    }

    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleVar(2);
    ImGui::EndMainMenuBar();
  }

  if (triggerAboutModularityPopup) {
    playEditorSound(EditorSoundCue::DialogOpen, 0.95f);
    ImGui::OpenPopup("Modularity Engine##AboutModularity");
    triggerAboutModularityPopup = false;
  }
  if (ImGui::IsPopupOpen("Modularity Engine##AboutModularity")) {
    CardModalIcon moduLogo;
    if (rendererInitialized) {
      Texture *logo =
          renderer.getTexture("Resources/Engine-Root/Modu-Logo.png");
      if (logo && logo->GetID()) {
        moduLogo = {static_cast<ImTextureID>(logo->GetID()), true};
      }
    }
    if (moduLogo.id == static_cast<ImTextureID>(0) && usingVulkan() &&
        vulkanRendererInitialized && vulkanRenderer) {
      moduLogo = {vulkanRenderer->getOrCreateUIImage(
                      "Resources/Engine-Root/Modu-Logo.png"),
                  false};
    }
    if (beginCardModal("Modularity Engine##AboutModularity", 0.0f, nullptr,
                       moduLogo)) {
      ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.90f);
      cardModalText("aka ModuEngine");
      cardModalText("V7.0 Beta");
      ImGui::PopFont();
      ImGui::Spacing();
      cardModalText("Modularity™ is a trademark of Tareno Labs™");
      cardModalText("© 2025-2026 Tareno Labs™");
      if (cardModalButton("Okay", CardButtonKind::Primary, 0, 1)) {
        ImGui::CloseCurrentPopup();
      }
      endCardModal();
    }
  }

  auto layoutFileHasDockNodesForDockspace = [](const fs::path &layoutPath,
                                               ImGuiID dockspaceId) {
    if (dockspaceId == 0) {
      return false;
    }
    std::ifstream in(layoutPath);
    if (!in.is_open()) {
      return false;
    }

    char dockspaceIdHex[16];
    std::snprintf(dockspaceIdHex, sizeof(dockspaceIdHex), "0x%08X",
                  dockspaceId);

    bool hasDockingData = false;
    bool hasDockNodes = false;
    bool hasMatchingDockspace = false;
    std::string line;
    while (std::getline(in, line)) {
      if (line == "[Docking][Data]") {
        hasDockingData = true;
        continue;
      }
      if (!hasDockingData) {
        continue;
      }
      if (!line.empty() && line.front() == '[') {
        break;
      }
      if (line.find("DockNode") != std::string::npos) {
        hasDockNodes = true;
      }
      if (line.find("DockSpace") != std::string::npos &&
          line.find(dockspaceIdHex) != std::string::npos) {
        hasMatchingDockspace = true;
      }
    }
    return hasDockingData && hasDockNodes && hasMatchingDockspace;
  };

  if (pendingWorkspaceReload) {
    if (mainDockspaceId != 0) {
      const bool hasLayoutFile = !pendingWorkspaceIniPath.empty() &&
                                 fs::exists(pendingWorkspaceIniPath);
      const bool hasMatchingDockspace =
          hasLayoutFile && layoutFileHasDockNodesForDockspace(
                               pendingWorkspaceIniPath, mainDockspaceId);
      if (hasMatchingDockspace) {
        ImGui::ClearIniSettings();
        loadWorkspaceIniWithLocalizedWindowIds(pendingWorkspaceIniPath);
        workspaceLayoutSettlingFrame = true;
        if (ImGui::DockBuilderGetNode(mainDockspaceId) == nullptr) {
          ImGui::DockBuilderRemoveNode(mainDockspaceId);
          workspaceLayoutDirty = true;
          workspaceLayoutAutoRepairPending = true;
        } else {
          // A valid persisted layout should win over the default workspace
          // repair path instead of being rebuilt a frame later.
          workspaceLayoutAutoRepairPending = false;
        }
      } else {
        // No persisted layout to load (or stale DockSpace ID): force
        // deterministic rebuild.
        ImGui::ClearIniSettings();
        ImGui::DockBuilderRemoveNode(mainDockspaceId);
        workspaceLayoutDirty = true;
        workspaceLayoutAutoRepairPending = true;
        workspaceLayoutSettlingFrame = true;
      }
      pendingWorkspaceReload = false;
      workspaceLayoutSavePending = false;
      workspaceLayoutStabilizeUntil = glfwGetTime() + 0.75;
    }
  }

  if (!pendingWorkspaceReload && workspaceLayoutDirty) {
    ImGui::ClearIniSettings();
    buildWorkspaceLayout(currentWorkspace);
    workspaceLayoutSavePending = true;
    workspaceLayoutSettlingFrame = true;
  }

  if (showStyleEditor) {
    if (ImGui::Begin("Style Editor", &showStyleEditor)) {
      static char newPresetName[128] = "";
      const auto &fontCatalog = getUIFontCatalog();
      std::string currentFontLabel = "ImGui Default";
      const int currentFontIndex = findUIFontCatalogIndex(uiEditorFontAsset);
      if (currentFontIndex >= 0) {
        currentFontLabel = fontCatalog[static_cast<size_t>(currentFontIndex)].label;
      }

      if (ImGui::BeginCombo("Preset", uiStylePresetName.c_str())) {
        for (size_t i = 0; i < uiStylePresets.size(); ++i) {
          const bool selected = (uiStylePresets[i].name == uiStylePresetName);
          if (ImGui::Selectable(uiStylePresets[i].name.c_str(), selected)) {
            applyUIStylePresetByName(uiStylePresets[i].name);
          }
          if (selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
        ImGui::EndCombo();
      }
      if (ImGui::BeginCombo("Editor Font", currentFontLabel.c_str())) {
        for (size_t i = 0; i < fontCatalog.size(); ++i) {
          const bool selected = (fontCatalog[i].id == uiEditorFontAsset);
          if (ImGui::Selectable(fontCatalog[i].label.c_str(), selected)) {
            applyEditorUIFontById(fontCatalog[i].id);
          }
          if (selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
        ImGui::EndCombo();
      }

      {
        const bool glassAvailable =
            projectManager.preferences.effectiveTier() !=
            Modularity::HardwareProfile::Tier::Low;
        ImGui::BeginDisabled(!glassAvailable);
        ImGui::Checkbox("Glass Blur", &ImGui::GetStyle().GlassBlur);
        ImGui::EndDisabled();
        if (!glassAvailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          ImGui::SetTooltip("Off on Low performance mode: the frosted backdrop costs a "
                            "framebuffer capture and blur per translucent window.\n"
                            "Change it in the launcher's Settings > Performance tab.");
        }
      }
      ImGui::SameLine();
      ImGui::Checkbox("Pill Switches", &ImGui::GetStyle().CheckboxSwitch);
      ImGui::SameLine();
      ImGui::Checkbox("Pill Sliders", &ImGui::GetStyle().SliderPill);

      {
        ImGuiShadeTheme shade = ImGui::GetShadeTheme();
        bool shadeChanged = false;

        auto drawsSomething = [](const ImGuiShadeParams &p) {
          if ((p.Flags & ImGuiShadeFlags_Set) == 0) return false;
          if (p.GradientTop != 0.0f || p.GradientBottom != 0.0f) return true;
          if (p.BevelSize > 0.0f || p.BorderSize > 0.0f) return true;
          return (p.ColTopHighlight | p.ColBottomShadow | p.ColInnerShadow | p.ColInnerHighlight |
                  p.ColBorderTop | p.ColBorderBottom | p.ColBorderLeft | p.ColBorderRight |
                  p.ColBorderAll) != 0;
        };
        auto hasVisibleShading = [&](const ImGuiShadeTheme &theme) {
          for (int c = 0; c < ImGuiShadeClass_COUNT; ++c) {
            for (int s = 0; s < ImGuiShadeState_COUNT; ++s) {
              if (drawsSomething(theme.Params[c][s])) return true;
            }
          }
          return false;
        };

        if (ImGui::Checkbox("Shaded Widgets", &shade.Enabled)) {
          shadeChanged = true;
          if (shade.Enabled && !hasVisibleShading(shade)) {
            const float gradientScale = shade.GradientScale;
            const float bevelScale = shade.BevelScale;
            applyModularityShadeTheme(shade);
            shade.GradientScale = gradientScale;
            shade.BevelScale = bevelScale;
          }
        }
        if (shade.Enabled) {
          ImGui::SameLine();
          ImGui::SetNextItemWidth(120.0f);
          shadeChanged |= ImGui::SliderFloat("Gradients", &shade.GradientScale, 0.0f, 2.0f, "%.2f");
          ImGui::SameLine();
          ImGui::SetNextItemWidth(120.0f);
          shadeChanged |= ImGui::SliderFloat("Bevels", &shade.BevelScale, 0.0f, 2.0f, "%.2f");
        }
        ImGui::SameLine();
        if (ImGui::Button("Shading: Modularity")) {
          applyModularityShadeTheme(shade);
          shadeChanged = true;
        }
        if (shadeChanged) {
          shade.Scale = (uiDpiScale > 0.0f) ? uiDpiScale : 1.0f;
          ImGui::SetShadeTheme(shade);
        }
      }

      if (ImGui::Button("Save Current Preset")) {
        saveCurrentUIStyleToPreset(uiStylePresetName, true);
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(180.0f);
      ImGui::InputTextWithHint("##NewStylePresetName", "New preset name", newPresetName, sizeof(newPresetName));
      ImGui::SameLine();
      if (ImGui::Button("Save As New") && newPresetName[0] != '\0') {
        if (saveCurrentUIStyleToPreset(newPresetName, false)) {
          std::snprintf(newPresetName, sizeof(newPresetName), "%s", "");
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Save UI Settings")) {
        saveEditorUserSettings();
      }
      ImGui::SameLine();
      if (ImGui::Button("Export Theme + Layout")) {
        exportEditorThemeLayout();
      }
      ImGui::SameLine();
      if (ImGui::Button("Write Theme File")) {
        std::string themeError;
        if (saveEditorThemeFile(&themeError)) {
          addConsoleMessage("Wrote editor theme: " + getEditorThemeFilePath().string(),
                            ConsoleMessageType::Success);
        } else {
          addConsoleMessage("Could not write editor theme: " + themeError,
                            ConsoleMessageType::Error);
        }
      }
      ImGui::SetItemTooltip("Write the live theme to EditorTheme.modutheme (hand-editable, hot-reloaded)");
      ImGui::Separator();
      ImGuiStyle &style = ImGui::GetStyle();
      ImGui::ShowStyleEditor(&style);
    }
    ImGui::End();
  }
}

void Engine::applyWorkspacePreset(WorkspaceMode mode, bool rebuildLayout) {
  currentWorkspace = mode;
  workspaceLayoutSavePending = false;
  workspaceLayoutAutoRepairPending = true;
  workspaceLayoutStabilizeUntil = glfwGetTime() + 0.75;
  switch (mode) {
  case WorkspaceMode::Default:
    showHierarchy = true;
    showInspector = true;
    showFileBrowser = true;
    showConsole = true;
    showScriptingWindow = false;
    showAnimationWindow = false;
    showAIPathfindingWindow = false;
    showEnvironmentWindow = true;
    showCameraWindow = true;
    showViewport = true;
    showGameViewport = true;
    break;
  case WorkspaceMode::Animation:
    showHierarchy = true;
    showInspector = true;
    showFileBrowser = false;
    showConsole = true;
    showScriptingWindow = false;
    showAnimationWindow = true;
    showAIPathfindingWindow = true;
    showEnvironmentWindow = false;
    showCameraWindow = false;
    showViewport = true;
    showGameViewport = true;
    break;
  case WorkspaceMode::Scripting:
    showHierarchy = true;
    showInspector = true;
    showFileBrowser = true;
    showConsole = true;
    showScriptingWindow = hasScriptingWindowPackage();
    showAnimationWindow = false;
    showAIPathfindingWindow = false;
    showEnvironmentWindow = false;
    showCameraWindow = false;
    showViewport = true;
    showGameViewport = true;
    break;
  }

  if (!rebuildLayout && projectManager.currentProject.isLoaded) {
    const ProjectConsoleSettings& consoleSettings =
        projectManager.currentProject.consoleSettings;
    showConsole = consoleSettings.mode == ProjectConsoleMode::DockedMiniButton ||
                  consoleSettings.alwaysOpenOnLaunch || consolePanelExpanded;
  }

  if (rebuildLayout) {
    // Explicit workspace switches should rebuild from the preset instead of
    // reloading stale persisted dock data that can fight the new layout.
    pendingWorkspaceIniPath.clear();
    pendingWorkspaceReload = false;
    buildWorkspaceLayout(mode);
    if (workspaceLayoutDirty) {
      pendingWorkspaceReload = true;
    }
    workspaceLayoutStabilizeUntil = glfwGetTime() + 0.75;
    return;
  }

  auto layoutFileIsUsable = [&](const fs::path &layoutPath) {
    std::ifstream in(layoutPath);
    if (!in.is_open())
      return false;

    bool hasDockingData = false;
    bool hasDockNodes = false;
    bool hasDockspace = false;
    std::string line;
    while (std::getline(in, line)) {
      if (line == "[Docking][Data]") {
        hasDockingData = true;
        continue;
      }
      if (hasDockingData && line.find("DockNode") != std::string::npos) {
        hasDockNodes = true;
      }
      if (hasDockingData && line.find("DockSpace") != std::string::npos) {
        hasDockspace = true;
      }
    }
    if (!(hasDockingData && hasDockNodes && hasDockspace)) {
      return false;
    }

    if (loadedWorkspaceLayoutVersion < kWorkspaceLayoutVersion &&
        layoutFileNeedsUtilityDockMigration(layoutPath, showProjectBrowser,
                                            showRegistryPackagesWindow)) {
      return false;
    }

    return true;
  };

  fs::path layoutPath = getWorkspaceLayoutPath(mode);
  if (!layoutPath.empty() && fs::exists(layoutPath) &&
      layoutFileIsUsable(layoutPath)) {
    pendingWorkspaceIniPath = layoutPath;
    pendingWorkspaceReload = true;
    workspaceLayoutDirty = false;
    workspaceLayoutStabilizeUntil = glfwGetTime() + 0.75;
    return;
  }

  // applyWorkspacePreset() is also called during project/settings load, where
  // there may be no active ImGui window for ImGui::GetID() yet.
  // Defer dock layout rebuild to the normal UI frame path.
  pendingWorkspaceIniPath.clear();
  pendingWorkspaceReload = true;
  workspaceLayoutDirty = true;
  workspaceLayoutStabilizeUntil = glfwGetTime() + 0.75;
}

void Engine::buildWorkspaceLayout(WorkspaceMode mode) {
  if (!ImGui::GetCurrentContext()) {
    workspaceLayoutDirty = true;
    return;
  }

  if (mainDockspaceId == 0) {
    workspaceLayoutDirty = true;
    return;
  }
  const ImGuiID dockspaceId = mainDockspaceId;

  ImGuiViewport *viewport = ImGui::GetMainViewport();
  if (!viewport) {
    workspaceLayoutDirty = true;
    return;
  }

  ImGui::DockBuilderRemoveNode(dockspaceId);
  ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
  ImVec2 dockspaceSize = viewport->WorkSize;
  if (ImGuiWindow *dockHost = ImGui::FindWindowByName("DockSpace")) {
    dockspaceSize = dockHost->Size;
  }
  dockspaceSize.y =
      ImMax(0.0f, dockspaceSize.y - getEditorBottomStatusReserveHeight());
  ImGui::DockBuilderSetNodeSize(dockspaceId, dockspaceSize);

  ImGuiID dockMain = dockspaceId;
  if (mode == WorkspaceMode::Default) {
    ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left,
                                                   0.20f, nullptr, &dockMain);
    ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right,
                                                    0.28f, nullptr, &dockMain);
    ImGuiID dockBottom = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down,
                                                     0.28f, nullptr, &dockMain);
    ImGuiID dockUtility = ImGui::DockBuilderSplitNode(
        dockRight, ImGuiDir_Down, 0.42f, nullptr, &dockRight);

    ImGui::DockBuilderDockWindow(Loc::WindowRef("Hierarchy"), dockLeft);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Camera"), dockLeft);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Inspector"), dockRight);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Environment"), dockRight);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Project"), dockBottom);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Version Control"), dockBottom);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Project Settings"), dockUtility);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Modupak Manager"), dockUtility);
    if (hasSpriteEditorPackage()) {
      ImGui::DockBuilderDockWindow(Loc::WindowRef("Pixel Sprite Editor"), dockMain);
      ImGui::DockBuilderDockWindow(Loc::WindowRef("Assemblage"), dockMain);
    }
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Viewport"), dockMain);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Game Viewport"), dockMain);
  } else if (mode == WorkspaceMode::Animation) {
    ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left,
                                                   0.20f, nullptr, &dockMain);
    ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right,
                                                    0.27f, nullptr, &dockMain);
    ImGuiID dockBottom = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down,
                                                     0.35f, nullptr, &dockMain);
    ImGuiID dockUtility = ImGui::DockBuilderSplitNode(
        dockRight, ImGuiDir_Down, 0.42f, nullptr, &dockRight);

    ImGui::DockBuilderDockWindow(Loc::WindowRef("Hierarchy"), dockLeft);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Camera"), dockLeft);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Inspector"), dockRight);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Environment"), dockRight);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Animation"), dockBottom);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("AI Pathfinding"), dockBottom);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Project"), dockBottom);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Version Control"), dockBottom);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Project Settings"), dockUtility);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Modupak Manager"), dockUtility);
    if (hasSpriteEditorPackage()) {
      ImGui::DockBuilderDockWindow(Loc::WindowRef("Pixel Sprite Editor"), dockMain);
      ImGui::DockBuilderDockWindow(Loc::WindowRef("Assemblage"), dockMain);
    }
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Viewport"), dockMain);
  } else {
    ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left,
                                                   0.25f, nullptr, &dockMain);
    ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right,
                                                    0.35f, nullptr, &dockMain);
    ImGuiID dockUtility = ImGui::DockBuilderSplitNode(
        dockRight, ImGuiDir_Down, 0.42f, nullptr, &dockRight);

    ImGui::DockBuilderDockWindow(Loc::WindowRef("Project"), dockLeft);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Version Control"), dockLeft);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Hierarchy"), dockLeft);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Camera"), dockLeft);
    if (hasScriptingWindowPackage()) {
      ImGui::DockBuilderDockWindow(Loc::WindowRef("Scripting"), dockRight);
    }
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Inspector"), dockRight);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Environment"), dockRight);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Project Settings"), dockUtility);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Modupak Manager"), dockUtility);
    if (hasSpriteEditorPackage()) {
      ImGui::DockBuilderDockWindow(Loc::WindowRef("Pixel Sprite Editor"), dockMain);
      ImGui::DockBuilderDockWindow(Loc::WindowRef("Assemblage"), dockMain);
    }
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Viewport"), dockMain);
    ImGui::DockBuilderDockWindow(Loc::WindowRef("Game Viewport"), dockMain);
  }

  ImGui::DockBuilderFinish(dockspaceId);
  workspaceLayoutDirty = false;
  workspaceLayoutSavePending = false;
  workspaceLayoutStabilizeUntil = glfwGetTime() + 0.75;
}

void Engine::renderSceneObjectCreateMenu(int parentId) {
  // TODO: "Search SceneOBJ..." quick-create filter, once the editor has a
  const size_t objectCountBeforeMenu = sceneObjects.size();
  auto lastCreated = [&]() -> SceneObject * {
    return sceneObjects.empty() ? nullptr : &sceneObjects.back();
  };
  auto allocateRig25DNodeId = [&]() {
    int nextNodeId = 0;
    for (const SceneObject &obj : sceneObjects) {
      if (!obj.hasRig25DNode || obj.rig25DNode.nodeId < 0) {
        continue;
      }
      nextNodeId = std::max(nextNodeId, obj.rig25DNode.nodeId + 1);
    }
    return nextNodeId;
  };
  auto createRig25DObject = [&](bool isRoot) {
    addObject(ObjectType::Empty, isRoot ? "Rig Root" : "Rig Node");
    if (SceneObject *created = getSelectedObject()) {
      created->type = ObjectType::Empty;
      created->hasRenderer = false;
      created->renderType = RenderType::None;
      created->faceCamera = false;
      created->hasRig25DRoot = isRoot;
      created->rig25DRoot.enabled = isRoot;
      created->hasRig25DNode = !isRoot;
      created->rig25DNode.enabled = !isRoot;
      created->rig25DNode.nodeId = isRoot ? -1 : allocateRig25DNodeId();
      created->rig25DNode.nodeName = isRoot ? std::string() : created->name;
      created->localPosition = created->position;
      created->localRotation = NormalizeEulerDegrees(created->rotation);
      created->localScale = created->scale;
      created->localInitialized = true;
      EnsureInspectorComponentMetadata(*created);
      projectManager.currentProject.hasUnsavedChanges = true;
    }
  };
  auto createTaggedMarker = [&](const std::string &label,
                                const std::string &tag) {
    addObject(ObjectType::Empty, label);
    if (SceneObject *created = lastCreated()) {
      created->tag = tag;
    }
  };
  auto createUIWithCanvas = [&](ObjectType type, const std::string &baseName) {
    int canvasId = -1;
    // Right-clicking a Canvas should build into that Canvas, not into whichever one happens
    // to come first in the scene.
    if (parentId >= 0) {
      if (SceneObject *requestedParent = findObjectById(parentId)) {
        if (requestedParent->hasUI &&
            requestedParent->ui.type == UIElementType::Canvas) {
          canvasId = requestedParent->id;
        }
      }
    }
    for (const auto &obj : sceneObjects) {
      if (canvasId >= 0) {
        break;
      }
      if (obj.hasUI && obj.ui.type == UIElementType::Canvas) {
        canvasId = obj.id;
        break;
      }
    }
    if (canvasId < 0) {
      addObject(ObjectType::Canvas, "Canvas");
      if (SceneObject *created = lastCreated()) {
        canvasId = created->id;
      }
    }
    addObject(type, baseName);
    if (!sceneObjects.empty() && canvasId >= 0) {
      setParent(sceneObjects.back().id, canvasId);
    }
  };

  if (ImGui::MenuItem("Empty SceneOBJ"))
    addObject(ObjectType::Empty, "Empty");
  ImGui::Separator();

  if (ImGui::BeginMenu("3D Primitives")) {
    if (ImGui::MenuItem("Cube"))
      addObject(ObjectType::Cube, "Cube");
    if (ImGui::MenuItem("Sphere"))
      addObject(ObjectType::Sphere, "Sphere");
    if (ImGui::MenuItem("Capsule"))
      addObject(ObjectType::Capsule, "Capsule");
    if (ImGui::MenuItem("Plane"))
      addObject(ObjectType::Plane, "Plane");
    if (ImGui::MenuItem("Torus"))
      addObject(ObjectType::Torus, "Torus");
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("2D Objects")) {
    if (ImGui::MenuItem("Sprite (Quad)"))
      addObject(ObjectType::Sprite, "Sprite");
    if (ImGui::MenuItem("2.5D Sprite"))
      addObject(ObjectType::Sprite25D, "2.5D Sprite");
    if (ImGui::MenuItem("Particle System 2D"))
      addObject(ObjectType::ParticleSystem2D, "Particle System 2D");
    if (ImGui::MenuItem("Mirror"))
      addObject(ObjectType::Mirror, "Mirror");
    ImGui::EndMenu();
  }

  if (isProject25DPipeline() && ImGui::BeginMenu("2.5D Rig")) {
    ImGui::TextDisabled("Rig nodes are Empty objects.");
    ImGui::TextDisabled("Animate them with the normal transform tracks.");
    ImGui::Separator();
    if (ImGui::MenuItem("Create Rig Root (Empty)")) {
      createRig25DObject(true);
    }
    if (ImGui::MenuItem("Create Rig Node (Empty)")) {
      createRig25DObject(false);
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("MMesh")) {
    if (ImGui::MenuItem("Cube"))
      createMMeshPrimitive("Cube");
    if (ImGui::MenuItem("Sphere"))
      createMMeshPrimitive("Sphere");
    if (ImGui::MenuItem("Plane"))
      createMMeshPrimitive("Plane");
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("RMesh")) {
    if (ImGui::MenuItem("Cube"))
      createRMeshPrimitive("Cube");
    if (ImGui::MenuItem("Sphere"))
      createRMeshPrimitive("Sphere");
    if (ImGui::MenuItem("Plane"))
      createRMeshPrimitive("Plane");
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Map Maker")) {
    if (ImGui::MenuItem("Map Root")) {
      ensureMapRootObject(true);
      showSectorMapWindow = true;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Root object owning the sector graph (one per scene)");
    }
    ImGui::Separator();
    auto spawnRoomSector = [&](int preset) {
      glm::vec3 spawn = camera.position + camera.front * 8.0f;
      spawn.y = 0.0f;
      createMapRoomSectorAt(preset, spawn, 0.0f, glm::vec2(0.0f), true);
      showSectorMapWindow = true;
    };
    if (ImGui::MenuItem("Sector: Small Room")) spawnRoomSector(0);
    if (ImGui::MenuItem("Sector: Medium Room")) spawnRoomSector(1);
    if (ImGui::MenuItem("Sector: Large Room")) spawnRoomSector(2);
    if (ImGui::MenuItem("Sector: Corridor")) spawnRoomSector(3);
    if (ImGui::MenuItem("Sector: Vertical Shaft")) spawnRoomSector(5);
    if (ImGui::MenuItem("Sector: Stair Room")) spawnRoomSector(6);
    if (ImGui::MenuItem("Sector: Empty")) spawnRoomSector(4);
    ImGui::Separator();
    ImGui::MenuItem("Sector Map", nullptr, &showSectorMapWindow);
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Cameras")) {
    if (ImGui::MenuItem("Camera"))
      addObject(ObjectType::Camera, "Camera");
    if (ImGui::MenuItem("2D Camera")) {
      addObject(ObjectType::Camera, "2D Camera");
      if (SceneObject *created = lastCreated()) {
        created->camera.use2D = true;
      }
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Lights")) {
    if (ImGui::MenuItem("Directional Light"))
      addObject(ObjectType::DirectionalLight, "Directional Light");
    if (ImGui::MenuItem("Point Light"))
      addObject(ObjectType::PointLight, "Point Light");
    if (ImGui::MenuItem("Spot Light"))
      addObject(ObjectType::SpotLight, "Spot Light");
    if (ImGui::MenuItem("Area Light"))
      addObject(ObjectType::AreaLight, "Area Light");
    if (ImGui::MenuItem("Reflection Cast"))
      addObject(ObjectType::ReflectionCast, "Reflection Cast");
    if (has2DWorldPackage()) {
      ImGui::Separator();
      if (ImGui::MenuItem("2D Point Light"))
        addObject(ObjectType::Light2D, "2D Point Light");
      if (ImGui::MenuItem("2D Spot Light")) {
        addObject(ObjectType::Light2D, "2D Spot Light");
        if (SceneObject *created = lastCreated()) {
          created->light2D.type = Light2DType::Spot;
        }
      }
      if (ImGui::MenuItem("2D Freeform Light")) {
        addObject(ObjectType::Light2D, "2D Freeform Light");
        if (SceneObject *created = lastCreated()) {
          created->light2D.type = Light2DType::Freeform;
          created->light2D.shapePoints = {
              glm::vec2(-2.0f, -1.5f), glm::vec2(2.0f, -1.5f),
              glm::vec2(2.5f, 1.0f), glm::vec2(0.0f, 2.5f),
              glm::vec2(-2.5f, 1.0f)};
        }
      }
      if (ImGui::MenuItem("2D Global Light")) {
        addObject(ObjectType::Light2D, "2D Global Light");
        if (SceneObject *created = lastCreated()) {
          created->light2D.type = Light2DType::Global;
          created->light2D.intensity = 0.35f;
          created->light2D.color = glm::vec4(0.45f, 0.52f, 0.72f, 1.0f);
        }
      }
      if (ImGui::MenuItem("2D Shadow Caster"))
        addObject(ObjectType::ShadowCaster2D, "2D Shadow Caster");
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Audio")) {
    if (ImGui::MenuItem("Audio Source")) {
      addObject(ObjectType::Empty, "Audio Source");
      if (SceneObject *created = lastCreated()) {
        created->hasAudioSource = true;
        created->audioSource = AudioSourceComponent{};
      }
    }
    if (ImGui::MenuItem("Audio Reverb Zone")) {
      addObject(ObjectType::Empty, "Reverb Zone");
      if (SceneObject *created = lastCreated()) {
        created->hasReverbZone = true;
        created->reverbZone = ReverbZoneComponent{};
        created->reverbZone.boxSize = glm::max(created->scale, glm::vec3(1.0f));
      }
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("XR")) {
    if (ImGui::MenuItem("XR Origin (Player)")) {
      int originId = -1;

      addObject(ObjectType::Empty, "XR Origin (Player)");
      if (SceneObject *origin = lastCreated()) {
        origin->hasXROrigin = true;
        origin->xrOrigin = XROriginComponent{};
        originId = origin->id;
        EnsureInspectorComponentMetadata(*origin);
      }

      const auto addRigChild = [&](const char *name, const glm::vec3 &localOffset) -> SceneObject * {
        addObject(ObjectType::Empty, name);
        SceneObject *created = lastCreated();
        if (!created) return nullptr;
        if (originId >= 0) setParent(created->id, originId);
        created->localPosition = localOffset;
        created->localRotation = glm::vec3(0.0f);
        created->localScale = glm::vec3(1.0f);
        created->localInitialized = true;
        return created;
      };

      const auto addSphereMarker = [&](int parentId, const char *name, float diameter) {
        addObject(ObjectType::Sphere, name);
        SceneObject *marker = lastCreated();
        if (!marker) return;
        if (parentId >= 0) setParent(marker->id, parentId);
        marker->localPosition = glm::vec3(0.0f);
        marker->localRotation = glm::vec3(0.0f);
        marker->localScale = glm::vec3(diameter);
        marker->scale = glm::vec3(diameter);
        marker->localInitialized = true;
        EnsureInspectorComponentMetadata(*marker);
      };

      if (SceneObject *head = addRigChild("XR Camera", glm::vec3(0.0f, 1.6f, 0.0f))) {
        head->hasCamera = true;
        head->camera = CameraComponent{};
        head->camera.type = SceneCameraType::Player;
        head->hasXRCamera = true;
        head->xrCamera = XRCameraComponent{};
        EnsureInspectorComponentMetadata(*head);
        addSphereMarker(head->id, "Head Marker", 0.354f);
      }

      const auto addController = [&](const char *name, const char *rayName, XRHand hand,
                                     float xOffset) {
        int controllerId = -1;
        if (SceneObject *controller =
                addRigChild(name, glm::vec3(xOffset, 1.2f, -0.2f))) {
          controllerId = controller->id;
          controller->hasXRController = true;
          controller->xrController = XRControllerComponent{};
          controller->xrController.hand = hand;
          controller->xrController.poseSource = XRControllerPoseSource::Grip;

          controller->hasXRActionBasedController = true;
          controller->xrActionBasedController = XRActionBasedControllerComponent{};
          controller->xrActionBasedController.hand = hand;

          controller->hasXRDirectInteractor = true;
          controller->xrDirectInteractor = XRDirectInteractorComponent{};
          EnsureInspectorComponentMetadata(*controller);
        }
        if (controllerId >= 0) {
          addSphereMarker(controllerId, "Hand Marker", 0.116f);
        }

        if (SceneObject *ray = addRigChild(rayName, glm::vec3(xOffset, 1.2f, -0.2f))) {
          ray->hasXRController = true;
          ray->xrController = XRControllerComponent{};
          ray->xrController.hand = hand;
          ray->xrController.poseSource = XRControllerPoseSource::Aim;

          ray->hasXRActionBasedController = true;
          ray->xrActionBasedController = XRActionBasedControllerComponent{};
          ray->xrActionBasedController.hand = hand;

          ray->hasXRRayInteractor = true;
          ray->xrRayInteractor = XRRayInteractorComponent{};
          EnsureInspectorComponentMetadata(*ray);
        }
      };
      addController("Left Controller", "Left Ray Interactor", XRHand::Left, -0.2f);
      addController("Right Controller", "Right Ray Interactor", XRHand::Right, 0.2f);

      if (originId >= 0) {
        selectedObjectId = originId;
        projectManager.currentProject.hasUnsavedChanges = true;
        addConsoleMessage(
            "Created XR Origin (Player). Enable OpenXR in Project Settings to use it.",
            ConsoleMessageType::Success);
      }
    }
    if (ImGui::MenuItem("XR Origin (Empty)")) {
      addObject(ObjectType::Empty, "XR Origin");
      if (SceneObject *created = lastCreated()) {
        created->hasXROrigin = true;
        created->xrOrigin = XROriginComponent{};
        EnsureInspectorComponentMetadata(*created);
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("XR Controller")) {
      addObject(ObjectType::Empty, "XR Controller");
      if (SceneObject *created = lastCreated()) {
        created->hasXRController = true;
        created->xrController = XRControllerComponent{};
        created->hasXRActionBasedController = true;
        created->xrActionBasedController = XRActionBasedControllerComponent{};
        EnsureInspectorComponentMetadata(*created);
      }
    }
    if (ImGui::MenuItem("XR Grab Interactable (Cube)")) {
      addObject(ObjectType::Cube, "XR Grabbable");
      if (SceneObject *created = lastCreated()) {
        created->scale = glm::vec3(0.15f);
        syncLocalTransform(*created);
        created->hasXRGrabInteractable = true;
        created->xrGrabInteractable = XRGrabInteractableComponent{};
        created->hasRigidbody = true;
        created->rigidbody.enabled = true;
        created->rigidbody.useGravity = true;
        created->hasCollider = true;
        created->collider.type = ColliderType::Box;
        created->collider.boxSize = created->scale;
        EnsureInspectorComponentMetadata(*created);
      }
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Gameplay")) {
    if (ImGui::MenuItem("Player Controller")) {
      addObject(ObjectType::Capsule, "Player Controller");
      if (SceneObject *created = lastCreated()) {
        created->hasPlayerController = true;
        created->playerController = PlayerControllerComponent{};
        created->hasCollider = true;
        created->collider.type = ColliderType::Capsule;
        created->collider.boxSize =
            glm::vec3(created->playerController.radius * 2.0f,
                      created->playerController.height,
                      created->playerController.radius * 2.0f);
        created->collider.convex = true;
        created->hasRigidbody = true;
        created->rigidbody.enabled = true;
        created->rigidbody.useGravity = true;
        created->rigidbody.isKinematic = false;
        created->rigidbody.lockRotationX = true;
        created->rigidbody.lockRotationY = false;
        created->rigidbody.lockRotationZ = true;
        created->scale = glm::vec3(created->playerController.radius * 2.0f,
                                   created->playerController.height,
                                   created->playerController.radius * 2.0f);
        syncLocalTransform(*created);
      }
    }
    if (ImGui::MenuItem("Spawn Point"))
      createTaggedMarker("Spawn Point", "SpawnPoint");
    if (ImGui::MenuItem("Trigger Zone"))
      createTaggedMarker("Trigger Zone", "TriggerZone");
    if (ImGui::MenuItem("Checkpoint"))
      createTaggedMarker("Checkpoint", "Checkpoint");
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("AI")) {
    if (ImGui::MenuItem("AI Agent")) {
      addObject(ObjectType::Capsule, "AI Agent");
      if (SceneObject *created = lastCreated()) {
        created->hasAIAgent = true;
        created->aiAgent = AIAgentComponent{};
        created->aiAgent.destination = created->position;
      }
    }
    if (ImGui::MenuItem("Nav Area")) {
      addObject(ObjectType::Plane, "Nav Area");
      if (SceneObject *created = lastCreated()) {
        created->hasGroundBakedType = true;
        created->groundBakedType = GroundBakedTypeComponent{};
      }
    }
    if (ImGui::MenuItem("Off-Mesh Link")) {
      addObject(ObjectType::Empty, "Off-Mesh Link");
      if (SceneObject *created = lastCreated()) {
        created->hasOffMeshLink = true;
        created->offMeshLink = OffMeshLinkComponent{};
        created->offMeshLink.startPoint = created->position;
        created->offMeshLink.endPoint =
            created->position + glm::vec3(2.0f, 0.0f, 0.0f);
      }
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Video")) {
    if (ImGui::MenuItem("Video Player Plane")) {
      addObject(ObjectType::Plane, "Video Player Plane");
      if (SceneObject *created = lastCreated()) {
        created->hasVideoPlayer = true;
        created->videoPlayer = VideoPlayerComponent{};
      }
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Environment / Volumes")) {
    if (ImGui::MenuItem("ModuVolume"))
      addObject(ObjectType::PostFXNode, "ModuVolume");
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("UI")) {
    if (ImGui::MenuItem("Canvas"))
      addObject(ObjectType::Canvas, "Canvas");
    if (ImGui::MenuItem("UI Image"))
      createUIWithCanvas(ObjectType::UIImage, "UI Image");
    if (ImGui::MenuItem("UI Slider"))
      createUIWithCanvas(ObjectType::UISlider, "UI Slider");
    if (ImGui::MenuItem("UI Button"))
      createUIWithCanvas(ObjectType::UIButton, "UI Button");
    if (ImGui::MenuItem("UI Text"))
      createUIWithCanvas(ObjectType::UIText, "UI Text");
    if (has2DWorldPackage() && ImGui::MenuItem("Sprite2D"))
      createUIWithCanvas(ObjectType::Sprite2D, "Sprite2D");
    ImGui::EndMenu();
  }

  // Anything the menu just created that is still sitting at the root becomes a child of the
  // object the menu was opened on. Entries that build their own hierarchy - a UI element under
  // a Canvas, a sector under the map root - already parented themselves, so only their roots
  // move. ids are collected first because setParent() reorders sceneObjects.
  if (parentId >= 0 && sceneObjects.size() > objectCountBeforeMenu) {
    std::vector<int> createdRootIds;
    createdRootIds.reserve(sceneObjects.size() - objectCountBeforeMenu);
    for (size_t i = objectCountBeforeMenu; i < sceneObjects.size(); ++i) {
      if (sceneObjects[i].parentId == -1 && sceneObjects[i].id != parentId) {
        createdRootIds.push_back(sceneObjects[i].id);
      }
    }
    for (int createdId : createdRootIds) {
      setParent(createdId, parentId);
    }
  }
}

#pragma endregion
