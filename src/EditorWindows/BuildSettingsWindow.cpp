#include "../EditorLocalization.h"
#include "Engine.h"

namespace Loc = Modularity::Loc;

namespace {
std::vector<fs::path> GatherBuildProfiles(const fs::path& projectRoot) {
    std::vector<fs::path> profiles;
    const fs::path profilesDir = projectRoot / "BuildProfiles";
    std::error_code ec;
    if (fs::exists(profilesDir, ec)) {
        for (const auto& entry : fs::directory_iterator(profilesDir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() == ".modubuild") {
                profiles.push_back(fs::relative(entry.path(), projectRoot, ec));
                if (ec) {
                    ec.clear();
                    profiles.push_back(entry.path());
                }
            }
        }
    }
    if (profiles.empty()) {
        profiles.push_back(fs::path("BuildProfiles") / "Default.modubuild");
    }
    std::sort(profiles.begin(), profiles.end());
    return profiles;
}

bool DrawModuleStateCombo(const char* label, std::string& value) {
    const char* states[] = {"Auto", "On", "Off"};
    int index = value == "on" ? 1 : (value == "off" ? 2 : 0);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::Combo(label, &index, states, 3)) {
        value = index == 1 ? "on" : (index == 2 ? "off" : "auto");
        return true;
    }
    return false;
}
} // namespace

void Engine::renderBuildSettingsWindow() {
    if (!showBuildSettings) return;

    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Loc::Window("WINDOW_BUILD_SETTINGS", "Build Settings"), &showBuildSettings)) {
        ImGui::End();
        return;
    }

    if (!projectManager.currentProject.isLoaded) {
        ImGui::TextDisabled("No project loaded.");
        ImGui::End();
        return;
    }

    bool changed = false;
    float buttonSpacing = ImGui::GetStyle().ItemSpacing.x;

    static char profileNameBuf[128] = "";
    static char duplicateNameBuf[128] = "";
    std::vector<fs::path> profiles = GatherBuildProfiles(projectManager.currentProject.projectPath);
    std::string activeProfile = buildSettings.activeProfilePath.empty()
        ? (fs::path("BuildProfiles") / (buildSettings.profileName + ".modubuild")).generic_string()
        : buildSettings.activeProfilePath;
    ImGui::Text("Build Profile");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::BeginCombo("##BuildProfileSelector", buildSettings.profileName.c_str())) {
        for (const fs::path& profile : profiles) {
            const bool selected = profile.generic_string() == activeProfile;
            std::string label = profile.stem().string();
            if (ImGui::Selectable(label.c_str(), selected)) {
                selectBuildSettingsProfile(profile);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        std::snprintf(profileNameBuf, sizeof(profileNameBuf), "Profile");
        ImGui::OpenPopup("New Build Profile");
    }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate")) {
        std::snprintf(duplicateNameBuf, sizeof(duplicateNameBuf), "%s_Copy", buildSettings.profileName.c_str());
        ImGui::OpenPopup("Duplicate Build Profile");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(buildSettings.profileName == "Default");
    if (ImGui::Button("Delete")) {
        deleteBuildSettingsProfile(buildSettings.profileName);
    }
    ImGui::EndDisabled();
    if (beginCardModal("New Build Profile")) {
        cardModalText("Name the new build profile.");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0f);
        const bool submitted = ImGui::InputText("##ProfileName", profileNameBuf, sizeof(profileNameBuf),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        if (cardModalButton("Cancel", CardButtonKind::Neutral, 0, 2)) ImGui::CloseCurrentPopup();
        if (cardModalButton("Create", CardButtonKind::Primary, 1, 2) || submitted) {
            saveBuildSettingsProfileAs(profileNameBuf);
            ImGui::CloseCurrentPopup();
        }
        endCardModal();
    }
    if (beginCardModal("Duplicate Build Profile")) {
        cardModalText("Name the duplicated profile.");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0f);
        const bool submitted = ImGui::InputText("##DuplicateName", duplicateNameBuf, sizeof(duplicateNameBuf),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        if (cardModalButton("Cancel", CardButtonKind::Neutral, 0, 2)) ImGui::CloseCurrentPopup();
        if (cardModalButton("Duplicate", CardButtonKind::Primary, 1, 2) || submitted) {
            duplicateBuildSettingsProfile(duplicateNameBuf);
            ImGui::CloseCurrentPopup();
        }
        endCardModal();
    }
    ImGui::Separator();

    const float footerReserve = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 8.0f;
    const float bottomPanelH  = 185.0f;
    const float sepSpace      = ImGui::GetStyle().ItemSpacing.y + 1.0f;

    ImGui::BeginChild("BuildTopSection", ImVec2(0.0f, -(bottomPanelH + sepSpace + footerReserve)), false);

    const float leftColW  = 270.0f;
    const float rightColW = ImGui::GetContentRegionAvail().x - leftColW - buttonSpacing;

    ImGui::BeginChild("BuildLeftCol", ImVec2(leftColW, 0.0f), false);

    ImGui::Text("Scenes In Build");
    ImGui::BeginChild("BuildScenesList", ImVec2(0, 160), true);
    for (int i = 0; i < static_cast<int>(buildSettings.scenes.size()); ++i) {
        BuildSceneEntry& entry = buildSettings.scenes[i];
        ImGui::PushID(i);
        bool enabled = entry.enabled;
        if (ImGui::Checkbox("##enabled", &enabled)) {
            entry.enabled = enabled;
            changed = true;
        }
        ImGui::SameLine();
        bool selected = (buildSettingsSelectedIndex == i);
        if (ImGui::Selectable(entry.name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
            buildSettingsSelectedIndex = i;
        }
        float rightX = ImGui::GetWindowContentRegionMax().x;
        ImGui::SameLine(rightX - 24.0f);
        ImGui::TextDisabled("%d", i);
        ImGui::PopID();
    }
    ImGui::EndChild();

    {
        float removeW = 85.0f;
        float addW    = 130.0f;
        float total   = removeW + addW + buttonSpacing;
        float startX  = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - total;
        if (startX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(startX);
        if (ImGui::Button("Remove", ImVec2(removeW, 0.0f))) {
            if (buildSettingsSelectedIndex >= 0 &&
                buildSettingsSelectedIndex < static_cast<int>(buildSettings.scenes.size())) {
                buildSettings.scenes.erase(buildSettings.scenes.begin() + buildSettingsSelectedIndex);
                if (buildSettingsSelectedIndex >= static_cast<int>(buildSettings.scenes.size()))
                    buildSettingsSelectedIndex = static_cast<int>(buildSettings.scenes.size()) - 1;
                changed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Open Scene", ImVec2(addW, 0.0f))) {
            if (addSceneToBuildSettings(projectManager.currentProject.currentSceneName, true))
                changed = true;
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::Text("Target Platform");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1);
    int targetIndex = buildPlatformIndex(buildSettings.platform);
    if (ImGui::Combo("##target-platform", &targetIndex,
                     kBuildPlatformLabels, kBuildPlatformCount)) {
        BuildPlatform newPlatform = buildPlatformFromIndex(targetIndex);
        if (newPlatform != buildSettings.platform) {
            buildSettings.platform = newPlatform;
            int archCount = 0;
            const BuildArchitectureInfo* archList = buildArchitecturesForPlatform(newPlatform, archCount);
            bool valid = false;
            for (int i = 0; i < archCount; ++i) {
                if (buildSettings.architecture == archList[i].serializedName) { valid = true; break; }
            }
            if (!valid) buildSettings.architecture = archList[0].serializedName;
            changed = true;
        }
    }
    if (buildPlatformIsExperimental(buildSettings.platform)) {
        ImGui::TextDisabled("Experimental target — runtime is not fully wired up yet.");
    }
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    int archCount = 0;
    const BuildArchitectureInfo* archList = buildArchitecturesForPlatform(buildSettings.platform, archCount);
    const char* archLabels[8];
    int archLabelsUsed = std::min(archCount, static_cast<int>(sizeof(archLabels) / sizeof(archLabels[0])));
    int archIndex = 0;
    bool archMatched = false;
    for (int i = 0; i < archLabelsUsed; ++i) {
        archLabels[i] = archList[i].label;
        if (buildSettings.architecture == archList[i].serializedName) {
            archIndex = i;
            archMatched = true;
        }
    }
    if (!archMatched && archLabelsUsed > 0) {
        buildSettings.architecture = archList[0].serializedName;
        changed = true;
    }
    if (ImGui::Combo("##architecture", &archIndex, archLabels, archLabelsUsed)) {
        buildSettings.architecture = archList[archIndex].serializedName;
        changed = true;
    }
    ImGui::EndChild(); // BuildLeftCol

    ImGui::SameLine();

    ImGui::BeginChild("BuildRightCol", ImVec2(rightColW, 0.0f), false);
    ImGui::Text("Build Options");
    const char* configurations[] = {"Release", "Debug"};
    int configurationIndex = buildSettings.configuration == "Debug" ? 1 : 0;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("Configuration", &configurationIndex, configurations, 2)) {
        buildSettings.configuration = configurations[configurationIndex];
        changed = true;
    }
    if (ImGui::Checkbox("Runtime Only", &buildSettings.runtimeOnly)) changed = true;
    if (ImGui::Checkbox("Include Editor Target", &buildSettings.includeEditor)) changed = true;
    if (ImGui::Checkbox("Lean Runtime Export", &buildSettings.leanRuntimeExport)) changed = true;
    if (ImGui::Checkbox("Ship ScriptSDK", &buildSettings.shipScriptSdk)) changed = true;
    if (ImGui::Checkbox("Compress Audio (Vorbis)", &buildSettings.compressAudio)) changed = true;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("WAV, AIFF and FLAC clips ship as Ogg Vorbis. MP3 and OGG are left as they are.\n"
                          "Scenes and scripts keep using the original file names.");
    }
    if (buildSettings.compressAudio) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##AudioVorbisQuality", &buildSettings.audioVorbisQuality, 0.0f, 10.0f,
                               "Vorbis quality %.1f")) {
            changed = true;
        }
    }
    if (ImGui::Checkbox("Development Build",  &buildSettings.developmentBuild))  changed = true;
    if (ImGui::Checkbox("Server Build",       &buildSettings.serverBuild))       changed = true;
    if (ImGui::Checkbox("Script Debugging",   &buildSettings.scriptDebugging))   changed = true;
    if (ImGui::Checkbox("Scripts Only",       &buildSettings.scriptsOnlyBuild))  changed = true;
    ImGui::Spacing();
    ImGui::Text("Profiling");
    ImGui::Separator();
    if (ImGui::Checkbox("Autoconnect Profiler", &buildSettings.autoConnectProfiler)) changed = true;
    if (ImGui::Checkbox("Deep Profiling",        &buildSettings.deepProfiling))       changed = true;
    ImGui::Spacing();
    ImGui::Text("Compression & Packaging");
    ImGui::Separator();
    const char* compressionOptions[] = {"Default", "None", "LZ4", "LZ4HC"};
    int compressionIndex = 0;
    for (int i = 0; i < 4; ++i) {
        if (buildSettings.compressionMethod == compressionOptions[i]) {
            compressionIndex = i;
            break;
        }
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##compression", &compressionIndex, compressionOptions, 4)) {
        buildSettings.compressionMethod = compressionOptions[compressionIndex];
        changed = true;
    }
    if (ImGui::Checkbox("Export as Archive (.tar.gz)", &buildSettings.packageStandaloneArchive))
        changed = true;
    ImGui::Spacing();
    ImGui::Text("Modules");
    ImGui::Separator();
    if (DrawModuleStateCombo("Assimp", buildSettings.moduleAssimp)) changed = true;
    ImGui::SameLine();
    if (DrawModuleStateCombo("Vulkan", buildSettings.moduleVulkan)) changed = true;
    if (DrawModuleStateCombo("PhysX", buildSettings.modulePhysX)) changed = true;
    ImGui::SameLine();
    if (DrawModuleStateCombo("Jolt", buildSettings.moduleJolt)) changed = true;
    if (DrawModuleStateCombo("sndfile", buildSettings.moduleSndfile)) changed = true;
    ImGui::SameLine();
    if (DrawModuleStateCombo("opusfile", buildSettings.moduleOpusfile)) changed = true;
    if (DrawModuleStateCombo("Mono", buildSettings.moduleMono)) changed = true;
    ImGui::SameLine();
    if (DrawModuleStateCombo("OpenGL ES", buildSettings.moduleOpenGLES)) changed = true;
    ImGui::EndChild(); // BuildRightCol
    ImGui::EndChild(); // BuildTopSection

    ImGui::BeginChild("BuildBottomSection", ImVec2(0.0f, -footerReserve), false);
    ImGui::Spacing();

    char companyBuf[256];
    char buildNameBuf[256];
    char versionBuf[128];
    char splashBuf[512];
    char outputFolderBuf[512];
    std::snprintf(companyBuf,   sizeof(companyBuf),   "%s", buildSettings.companyName.c_str());
    std::snprintf(buildNameBuf, sizeof(buildNameBuf), "%s", buildSettings.buildName.c_str());
    std::snprintf(versionBuf,   sizeof(versionBuf),   "%s", buildSettings.version.c_str());
    std::snprintf(splashBuf,    sizeof(splashBuf),    "%s", buildSettings.splashImagePath.c_str());
    std::snprintf(outputFolderBuf, sizeof(outputFolderBuf), "%s", buildSettings.outputFolder.c_str());

    // Three-column row: Company | Build Name | Version
    if (ImGui::BeginTable("OutputFieldsTable", 3, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("##col0", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##col1", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##col2", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Company");
        ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("Build Name");
        ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("Version");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##company", companyBuf, sizeof(companyBuf))) {
            buildSettings.companyName = companyBuf;
            changed = true;
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##buildname", buildNameBuf, sizeof(buildNameBuf))) {
            buildSettings.buildName = buildNameBuf;
            changed = true;
        }
        ImGui::TableSetColumnIndex(2);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##version", versionBuf, sizeof(versionBuf))) {
            buildSettings.version = versionBuf;
            changed = true;
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();

    ImGui::TextDisabled("Output Folder");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##outputFolder", outputFolderBuf, sizeof(outputFolderBuf))) {
        buildSettings.outputFolder = outputFolderBuf;
        changed = true;
    }
    ImGui::Spacing();

    // Splash settings
    if (ImGui::Checkbox("Enable Startup Splash", &buildSettings.splashEnabled)) changed = true;
    ImGui::BeginDisabled(!buildSettings.splashEnabled);
    {
        float availW  = ImGui::GetContentRegionAvail().x;
        float btn1W   = 112.0f;
        float btn2W   = 50.0f;
        float inputW  = availW - btn1W - btn2W - buttonSpacing * 2;
        ImGui::SetNextItemWidth(inputW > 40.0f ? inputW : 40.0f);
        if (ImGui::InputText("##splash", splashBuf, sizeof(splashBuf))) {
            buildSettings.splashImagePath = splashBuf;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Use Selected", ImVec2(btn1W, 0))) {
            if (!fileBrowser.selectedFile.empty() && fs::is_regular_file(fileBrowser.selectedFile)) {
                fs::path splashPath = fs::absolute(fileBrowser.selectedFile);
                std::error_code relEc;
                fs::path rel = fs::relative(splashPath, projectManager.currentProject.projectPath, relEc);
                bool outsideProject = static_cast<bool>(relEc);
                if (!outsideProject) {
                    for (const auto& part : rel) {
                        if (part == "..") { outsideProject = true; break; }
                    }
                }
                buildSettings.splashImagePath = outsideProject ? splashPath.string() : rel.generic_string();
                changed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear", ImVec2(btn2W, 0))) {
            buildSettings.splashImagePath.clear();
            changed = true;
        }
        float splashDuration = buildSettings.splashDurationSeconds;
        ImGui::Text("Splash Duration");
        if (ImGui::SliderFloat("##Splash Duration (sec)", &splashDuration, 0.5f, 10.0f, "%.1f")) {
            buildSettings.splashDurationSeconds = std::clamp(splashDuration, 0.5f, 10.0f);
            changed = true;
        }
    }
    ImGui::EndDisabled();

    ImGui::EndChild(); // BuildBottomSection

    ImGui::Separator();
    {
        float bakeW    = 70.0f;
        float bakeRunW = 100.0f;
        float total    = bakeW + bakeRunW + buttonSpacing;
        float startX   = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - total;
        if (startX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(startX);
        if (ImGui::Button("Bake", ImVec2(bakeW, 0.0f))) {
            exportRunAfter = false;
            if (exportOutputPath[0] == '\0') {
                fs::path defaultOut = projectManager.currentProject.projectPath /
                    (buildSettings.outputFolder.empty() ? fs::path("Builds") : fs::path(buildSettings.outputFolder));
                std::snprintf(exportOutputPath, sizeof(exportOutputPath), "%s", defaultOut.string().c_str());
            }
            showExportDialog = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Bake + Run", ImVec2(bakeRunW, 0.0f))) {
            exportRunAfter = true;
            if (exportOutputPath[0] == '\0') {
                fs::path defaultOut = projectManager.currentProject.projectPath /
                    (buildSettings.outputFolder.empty() ? fs::path("Builds") : fs::path(buildSettings.outputFolder));
                std::snprintf(exportOutputPath, sizeof(exportOutputPath), "%s", defaultOut.string().c_str());
            }
            showExportDialog = true;
        }
    }

    if (changed) saveBuildSettings();

    if (showExportDialog) {
        ImGui::SetNextWindowSize(ImVec2(720, 460), ImGuiCond_Appearing);
        ImGui::OpenPopup("Export Game");
        showExportDialog = false;
    }

    bool exportPopupOpen = true;
    ImGuiWindowFlags popupFlags = ImGuiWindowFlags_NoDocking;
    bool exportActive = false;
    bool exportDone = false;
    bool exportSuccess = false;
    float exportProgress = 0.0f;
    std::string exportStatus;
    std::string exportLog;
    fs::path exportDir;
    std::string exportExeName;
    fs::path exportArchivePath;
    {
        std::lock_guard<std::mutex> lock(exportMutex);
        exportActive      = exportJob.active;
        exportDone        = exportJob.done;
        exportSuccess     = exportJob.success;
        exportProgress    = exportJob.progress;
        exportStatus      = exportJob.status;
        exportLog         = exportJob.log;
        exportDir         = exportJob.outputDir;
        exportExeName     = exportJob.executableName;
        exportArchivePath = exportJob.archivePath;
    }
    bool allowClose = !exportActive;
    if (ImGui::BeginPopupModal("Export Game", allowClose ? &exportPopupOpen : nullptr, popupFlags)) {
        ImGui::Text("Output Folder");
        ImGui::SetNextItemWidth(-1);
        ImGui::BeginDisabled(exportActive);
        ImGui::InputText("##ExportOutput", exportOutputPath, sizeof(exportOutputPath));
        ImGui::EndDisabled();

        if (!exportActive) {
            if (ImGui::Button("Use Selected Folder")) {
                if (!fileBrowser.selectedFile.empty()) {
                    fs::path selected = fileBrowser.selectedFile;
                    fs::path folder   = fs::is_directory(selected) ? selected : selected.parent_path();
                    std::snprintf(exportOutputPath, sizeof(exportOutputPath), "%s", folder.string().c_str());
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Use Project Folder")) {
                fs::path folder = projectManager.currentProject.projectPath /
                    (buildSettings.outputFolder.empty() ? fs::path("Builds") : fs::path(buildSettings.outputFolder));
                std::snprintf(exportOutputPath, sizeof(exportOutputPath), "%s", folder.string().c_str());
            }
        }

        ImGui::Spacing();
        if (exportActive || exportDone) {
            const char* statusLabel = exportStatus.empty() ? "Working..." : exportStatus.c_str();
            float barValue = exportActive ? exportProgress : 1.0f;
            if (barValue <= 0.0f) barValue = 0.02f;
            ImGui::ProgressBar(barValue, ImVec2(-1, 0), statusLabel);
            if (exportActive) {
                ImGui::TextDisabled("Build can take a while (PhysX/assimp). Output updates after each step finishes.");
            }
            ImGui::BeginChild("ExportLog", ImVec2(0, 180), true);
            if (exportLog.empty()) {
                ImGui::TextUnformatted("Waiting for build output...");
            } else {
                ImGui::TextUnformatted(exportLog.c_str());
            }
            ImGui::EndChild();
        }

        ImGui::Separator();
        if (!exportActive && !exportDone) {
            if (ImGui::Button("Start Export", ImVec2(120, 0))) {
                if (!exportOutputPath[0]) {
                    addConsoleMessage("Please choose an export folder.", ConsoleMessageType::Warning);
                } else {
                    startExportBuild(fs::path(exportOutputPath), exportRunAfter);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                ImGui::CloseCurrentPopup();
            }
        } else if (!exportActive && exportDone) {
            if (exportSuccess && !exportDir.empty()) {
                ImGui::TextDisabled("Exported to: %s", exportDir.string().c_str());
                if (!exportExeName.empty())
                    ImGui::TextDisabled("Executable: %s", exportExeName.c_str());
                if (!exportArchivePath.empty())
                    ImGui::TextDisabled("Archive: %s", exportArchivePath.filename().string().c_str());
            }
            if (ImGui::Button("Close", ImVec2(100, 0))) {
                ImGui::CloseCurrentPopup();
                std::lock_guard<std::mutex> lock(exportMutex);
                exportJob = ExportJobState{};
            }
        } else {
            if (ImGui::Button("Cancel Export", ImVec2(140, 0))) {
                exportCancelRequested = true;
                std::lock_guard<std::mutex> lock(exportMutex);
                exportJob.status = "Cancelling...";
            }
        }

        ImGui::EndPopup();
    }

    ImGui::End();
}
