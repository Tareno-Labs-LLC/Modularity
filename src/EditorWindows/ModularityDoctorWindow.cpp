#include "../EditorLocalization.h"
#include "Engine.h"

namespace Loc = Modularity::Loc;

namespace {
enum class DoctorSeverity {
    Pass,
    Info,
    Warning,
    Error
};

struct DoctorCheck {
    DoctorSeverity severity = DoctorSeverity::Info;
    std::string category;
    std::string name;
    std::string detail;
    std::string suggestion;
};

const char* SeverityLabel(DoctorSeverity severity) {
    switch (severity) {
        case DoctorSeverity::Pass: return "Pass";
        case DoctorSeverity::Warning: return "Warning";
        case DoctorSeverity::Error: return "Error";
        case DoctorSeverity::Info:
        default: return "Info";
    }
}

ImVec4 SeverityColor(DoctorSeverity severity) {
    switch (severity) {
        case DoctorSeverity::Pass: return ImVec4(0.35f, 0.82f, 0.48f, 1.0f);
        case DoctorSeverity::Warning: return ImVec4(0.95f, 0.72f, 0.28f, 1.0f);
        case DoctorSeverity::Error: return ImVec4(0.95f, 0.35f, 0.32f, 1.0f);
        case DoctorSeverity::Info:
        default: return ImVec4(0.58f, 0.72f, 0.95f, 1.0f);
    }
}

bool PathExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec) && !ec;
}

std::string EnvValue(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}
} // namespace

void Engine::renderModularityDoctorWindow() {
    if (!showModularityDoctorWindow) return;

    static std::vector<DoctorCheck> checks;
    static double lastRunTime = -1.0;
    static int selectedIndex = -1;

    auto addCheck = [&](DoctorSeverity severity,
                        const std::string& category,
                        const std::string& name,
                        const std::string& detail,
                        const std::string& suggestion = {}) {
        checks.push_back({severity, category, name, detail, suggestion});
    };

    auto runChecks = [&]() {
        checks.clear();
        selectedIndex = -1;
        if (!projectManager.currentProject.isLoaded) {
            addCheck(DoctorSeverity::Error, "Project", "Project loaded", "No project is currently loaded.", "Open or create a project.");
            lastRunTime = ImGui::GetTime();
            return;
        }

        const Project& project = projectManager.currentProject;
        addCheck(DoctorSeverity::Pass, "Project", "Project loaded", project.projectPath.string());
        addCheck(PathExists(project.projectPath / "project.modu") ? DoctorSeverity::Pass : DoctorSeverity::Error,
                 "Project", "project.modu", (project.projectPath / "project.modu").string(),
                 "Restore or recreate the project manifest.");
        addCheck(PathExists(project.assetsPath) ? DoctorSeverity::Pass : DoctorSeverity::Error,
                 "Project", "Assets folder", project.assetsPath.string());
        addCheck(PathExists(project.scenesPath) ? DoctorSeverity::Pass : DoctorSeverity::Warning,
                 "Project", "Scenes folder", project.scenesPath.string());
        addCheck(PathExists(project.scriptsConfigPath) ? DoctorSeverity::Pass : DoctorSeverity::Warning,
                 "Scripts", "scripts.modu", project.scriptsConfigPath.string(),
                 "Create scripts.modu or use the default project template.");
        addCheck(PathExists(project.projectPath / "packages.modu") ? DoctorSeverity::Pass : DoctorSeverity::Info,
                 "Packages", "packages.modu", (project.projectPath / "packages.modu").string());

        fs::path activeProfile = project.projectPath / buildSettings.activeProfilePath;
        addCheck(PathExists(activeProfile) ? DoctorSeverity::Pass : DoctorSeverity::Warning,
                 "Build", "Active build profile", activeProfile.string(),
                 "Save Build Settings to create the active .modubuild profile.");

        if (buildSettings.buildName.empty()) {
            addCheck(DoctorSeverity::Warning, "Build", "Build name", "Build name is empty.", "Set a build name in Build Settings.");
        }
        if (buildSettings.version.empty()) {
            addCheck(DoctorSeverity::Warning, "Build", "Version", "Version is empty.", "Set a version in Build Settings.");
        }
        if (buildSettings.scenes.empty()) {
            addCheck(DoctorSeverity::Warning, "Build", "Scenes In Build", "No scenes are listed.", "Add the open scene to Build Settings.");
        } else {
            for (const auto& scene : buildSettings.scenes) {
                fs::path scenePath = project.scenesPath / (scene.name + ".scene");
                addCheck(PathExists(scenePath) ? DoctorSeverity::Pass : DoctorSeverity::Error,
                         "Build", "Scene: " + scene.name, scenePath.string(),
                         "Remove the missing scene or restore it under Assets/Scenes.");
            }
        }
        if (!buildSettings.splashImagePath.empty()) {
            fs::path splash = buildSettings.splashImagePath;
            if (splash.is_relative()) splash = project.projectPath / splash;
            addCheck(PathExists(splash) ? DoctorSeverity::Pass : DoctorSeverity::Warning,
                     "Build", "Splash image", splash.string(), "Clear or replace the splash image path.");
        }

        ScriptBuildConfig scriptConfig;
        std::string configError;
        if (scriptCompiler.loadConfig(project.scriptsConfigPath, scriptConfig, configError)) {
            addCheck(DoctorSeverity::Pass, "Scripts", "Script config loads", project.scriptsConfigPath.string());
            fs::path scriptsDir = scriptConfig.scriptsDir;
            if (scriptsDir.is_relative()) scriptsDir = project.projectPath / scriptsDir;
            addCheck(PathExists(scriptsDir) ? DoctorSeverity::Pass : DoctorSeverity::Warning,
                     "Scripts", "scriptsDir", scriptsDir.string(), "Create the scripts directory or update scripts.modu.");
            fs::path outDir = scriptConfig.outDir;
            if (outDir.is_relative()) outDir = project.projectPath / outDir;
            addCheck(PathExists(outDir) ? DoctorSeverity::Pass : DoctorSeverity::Info,
                     "Scripts", "outDir", outDir.string(), "The compiler will create this directory when needed.");
        } else {
            addCheck(DoctorSeverity::Error, "Scripts", "Script config loads", configError, "Fix scripts.modu.");
        }

        for (const auto& packageId : packageManager.getInstalled()) {
            const bool hasPayload = packageManager.hasProjectInstallPayload(packageId) ||
                                    packageManager.isGloballyInstalled(packageId);
            addCheck(hasPayload ? DoctorSeverity::Pass : DoctorSeverity::Warning,
                     "Packages", "Package: " + packageId,
                     hasPayload ? "Installed payload found." : "Package manifest entry has no local/global payload.",
                     "Reinstall or remove the package.");
        }

        if (buildSettings.platform == BuildPlatform::Android) {
            std::string ndk = resolveAndroidNdkPath();
            addCheck(!ndk.empty() ? DoctorSeverity::Pass : DoctorSeverity::Error,
                     "Android", "Android NDK", ndk.empty() ? "ANDROID_NDK_ROOT / ANDROID_NDK_HOME / ANDROID_NDK not found." : ndk,
                     "Install the Android NDK and set ANDROID_NDK_ROOT.");
            std::string sdk = EnvValue("ANDROID_SDK_ROOT");
            if (sdk.empty()) sdk = EnvValue("ANDROID_HOME");
            addCheck(!sdk.empty() && PathExists(sdk) ? DoctorSeverity::Pass : DoctorSeverity::Error,
                     "Android", "Android SDK", sdk.empty() ? "ANDROID_SDK_ROOT / ANDROID_HOME not set." : sdk,
                     "Install the Android SDK and set ANDROID_SDK_ROOT.");
        } else {
            addCheck(DoctorSeverity::Info, "Android", "Android checks", "Current profile does not target Android.");
        }

        fs::path outputRoot = project.projectPath / (buildSettings.outputFolder.empty() ? fs::path("Builds") : fs::path(buildSettings.outputFolder));
        std::error_code ec;
        fs::create_directories(outputRoot, ec);
        addCheck(!ec ? DoctorSeverity::Pass : DoctorSeverity::Error,
                 "Export", "Output folder", outputRoot.string(), "Choose a writable output folder.");

        auto checkAssetPath = [&](const std::string& category, const std::string& objectName, const std::string& rawPath) {
            if (rawPath.empty()) return;
            fs::path path = rawPath;
            if (path.is_relative()) path = project.projectPath / path;
            addCheck(PathExists(path) ? DoctorSeverity::Pass : DoctorSeverity::Warning,
                     "Assets", category + ": " + objectName, path.string(),
                     "Restore the missing asset or clear the reference.");
        };
        for (const SceneObject& obj : sceneObjects) {
            checkAssetPath("Mesh", obj.name, obj.meshPath);
            checkAssetPath("Material", obj.name, obj.materialPath);
            checkAssetPath("Albedo Texture", obj.name, obj.albedoTexturePath);
            checkAssetPath("Overlay Texture", obj.name, obj.overlayTexturePath);
            checkAssetPath("Normal Map", obj.name, obj.normalMapPath);
            checkAssetPath("Vertex Shader", obj.name, obj.vertexShaderPath);
            checkAssetPath("Fragment Shader", obj.name, obj.fragmentShaderPath);
            checkAssetPath("Particle Texture", obj.name, obj.particleSystem2D.texturePath);
            checkAssetPath("Particle Material", obj.name, obj.particleSystem2D.materialPath);
            for (const auto& script : obj.scripts) {
                checkAssetPath("Script", obj.name, script.path);
            }
        }

        lastRunTime = ImGui::GetTime();
    };

    ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Loc::Window("WINDOW_DOCTOR", "Modularity Doctor"), &showModularityDoctorWindow)) {
        ImGui::End();
        return;
    }

    if (checks.empty()) {
        runChecks();
    }

    int passCount = 0, infoCount = 0, warningCount = 0, errorCount = 0;
    for (const DoctorCheck& check : checks) {
        if (check.severity == DoctorSeverity::Pass) ++passCount;
        else if (check.severity == DoctorSeverity::Info) ++infoCount;
        else if (check.severity == DoctorSeverity::Warning) ++warningCount;
        else if (check.severity == DoctorSeverity::Error) ++errorCount;
    }

    if (ImGui::Button("Run All", ImVec2(100, 0))) {
        runChecks();
    }
    ImGui::SameLine();
    if (ImGui::Button("Send to Console", ImVec2(130, 0))) {
        addConsoleMessage("Modularity Doctor: " + std::to_string(errorCount) + " errors, " +
                          std::to_string(warningCount) + " warnings.", ConsoleMessageType::Info);
    }
    ImGui::SameLine();
    if (lastRunTime >= 0.0) {
        ImGui::TextDisabled("Last run %.1fs ago", ImGui::GetTime() - lastRunTime);
    }
    ImGui::TextColored(SeverityColor(DoctorSeverity::Pass), "Pass %d", passCount);
    ImGui::SameLine();
    ImGui::TextColored(SeverityColor(DoctorSeverity::Info), "Info %d", infoCount);
    ImGui::SameLine();
    ImGui::TextColored(SeverityColor(DoctorSeverity::Warning), "Warnings %d", warningCount);
    ImGui::SameLine();
    ImGui::TextColored(SeverityColor(DoctorSeverity::Error), "Errors %d", errorCount);
    ImGui::Separator();

    const float detailHeight = 120.0f;
    if (ImGui::BeginTable("DoctorChecks", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable,
                          ImVec2(0, -detailHeight))) {
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Check", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(checks.size()); ++i) {
            const DoctorCheck& check = checks[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(SeverityColor(check.severity), "%s", SeverityLabel(check.severity));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(check.category.c_str());
            ImGui::TableSetColumnIndex(2);
            bool selected = selectedIndex == i;
            if (ImGui::Selectable(check.name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                selectedIndex = i;
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::TextWrapped("%s", check.detail.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(checks.size())) {
        const DoctorCheck& check = checks[selectedIndex];
        ImGui::TextColored(SeverityColor(check.severity), "%s", SeverityLabel(check.severity));
        ImGui::SameLine();
        ImGui::TextUnformatted(check.name.c_str());
        ImGui::TextWrapped("%s", check.detail.c_str());
        if (!check.suggestion.empty()) {
            ImGui::TextDisabled("%s", check.suggestion.c_str());
        }
    } else {
        ImGui::TextDisabled("Select a check for details.");
    }

    ImGui::End();
}
