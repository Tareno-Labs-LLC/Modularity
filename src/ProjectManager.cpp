#include "ProjectManager.h"
#include "../include/Platform/AssetSource.h"
#include "SceneSerializationInternal.h"
#include "EditorLocalization.h"
#include "ModuCPPLanguagePack.h"
#include "Rendering.h"
#include "ModelLoader.h"
#ifdef __ANDROID__
#include "AndroidRuntime/AndroidRuntime.h"
#endif
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <system_error>
#include <unordered_map>

ObjectType GetLegacyTypeFromComponents(const SceneObject& obj);

namespace {
std::string TrimCopy(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string GetPlatformDefaultProjectsPath() {
#if defined(__ANDROID__)
    {
        const std::string docs = Modularity::AndroidRuntime::GetExternalDocumentsPath();
        if (!docs.empty() && Modularity::AndroidRuntime::HasAllFilesAccess()) {
            return (fs::path(docs) / "ModularityProjects").string();
        }
    }
    if (const char* dataPath = Modularity::AndroidRuntime::GetInternalDataPath()) {
        return (fs::path(dataPath) / "ModularityProjects").string();
    }
    return std::string("/data/local/tmp/ModularityProjects");
#elif defined(_WIN32)
    // The user root, matching the Linux branch below rather than burying projects
    // under Documents. Windows sat under Documents only because reading a project
    // straight from the user root used to crash; that was a path-handling bug, since
    // fixed, not a reason to keep the two platforms disagreeing.
    //
    // Only a fallback: a stored defaultProjectLocation always wins, so existing
    // installs keep pointing wherever they already pointed.
    const char* userProfile = std::getenv("USERPROFILE");
    if (userProfile && *userProfile) {
        return (fs::path(userProfile) / "ModularityProjects").string();
    }
#else
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return (fs::path(home) / "ModularityProjects").string();
    }
#endif
    return (fs::current_path() / "Projects").string();
}

bool ParseBoolToken(const std::string& value) {
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

void ApplyGlobalPreferenceKey(EditorGlobalPreferences& prefs,
                              const std::string& key,
                              const std::string& value) {
    auto readFloat = [&](float& target, float low, float high) {
        try {
            target = std::min(high, std::max(low, std::stof(value)));
        } catch (...) {
        }
    };
    auto readInt = [&](int& target, int low, int high) {
        try {
            target = std::min(high, std::max(low, std::stoi(value)));
        } catch (...) {
        }
    };

    if (key == "themePreset")                    prefs.themePreset = value;
    else if (key == "uiFontAsset")               prefs.uiFontAsset = value;
    else if (key == "chromeScale")               prefs.chromeScale = value;
    else if (key == "animationMode")             prefs.animationMode = value;
    else if (key == "uiScale")                   readFloat(prefs.uiScale, 0.6f, 2.0f);
    else if (key == "launcherIntroAnimation")    prefs.launcherIntroAnimation = ParseBoolToken(value);
    else if (key == "launcherProjectThumbnails") prefs.launcherProjectThumbnails = ParseBoolToken(value);
    else if (key == "launcherRememberLastTab")   prefs.launcherRememberLastTab = ParseBoolToken(value);
    else if (key == "launcherLastTab")           readInt(prefs.launcherLastTab, 0, 2);
    else if (key == "launcherShowFullPaths")     prefs.launcherShowFullPaths = ParseBoolToken(value);
    else if (key == "launcherProjectSort")       readInt(prefs.launcherProjectSort, 0, 2);
    else if (key == "feedbackSounds")            prefs.feedbackSounds = ParseBoolToken(value);
    else if (key == "feedbackClickSounds")       prefs.feedbackClickSounds = ParseBoolToken(value);
    else if (key == "feedbackErrorSounds")       prefs.feedbackErrorSounds = ParseBoolToken(value);
    else if (key == "feedbackOtherSounds")       prefs.feedbackOtherSounds = ParseBoolToken(value);
    else if (key == "reopenLastProject")         prefs.reopenLastProject = ParseBoolToken(value);
    else if (key == "hierarchyTexturePreviews")  prefs.hierarchyTexturePreviews = ParseBoolToken(value);
    else if (key == "sceneGizmos")               prefs.sceneGizmos = ParseBoolToken(value);
    else if (key == "gizmoCameraOverlays")       prefs.gizmoCameraOverlays = ParseBoolToken(value);
    else if (key == "consoleWrapText")           prefs.consoleWrapText = ParseBoolToken(value);
    else if (key == "quickToolsPinned")          prefs.quickToolsPinned = ParseBoolToken(value);
    else if (key == "editorVSync")               prefs.editorVSync = ParseBoolToken(value);
    else if (key == "editorFpsCapEnabled")       prefs.editorFpsCapEnabled = ParseBoolToken(value);
    else if (key == "editorFpsCap")              readFloat(prefs.editorFpsCap, 10.0f, 1000.0f);
    else if (key == "performanceMode")           prefs.performanceMode = value;
    else if (key == "hardwareTier")              prefs.hardwareTier = value;
    else if (key == "benchFullscreenPassMs")     readFloat(prefs.benchFullscreenPassMs, 0.0f, 10000.0f);
    else if (key == "benchBlurPassMs")           readFloat(prefs.benchBlurPassMs, 0.0f, 10000.0f);
    else if (key == "benchDrawCallBatchMs")      readFloat(prefs.benchDrawCallBatchMs, 0.0f, 100000.0f);
    else if (key == "lowSpecPromptAnswered")     prefs.lowSpecPromptAnswered = ParseBoolToken(value);
    else if (key == "scriptAutoCompile")         prefs.scriptAutoCompile = ParseBoolToken(value);
    else if (key == "scriptAutoCompileInterval") readFloat(prefs.scriptAutoCompileInterval, 0.1f, 10.0f);
    else if (key == "scriptDirectoryScanInterval") readFloat(prefs.scriptDirectoryScanInterval, 1.0f, 60.0f);
    else if (key == "defaultPipeline")           readInt(prefs.defaultPipeline, 0, 2);
    else if (key == "defaultRenderer")           readInt(prefs.defaultRenderer, 0, 1);
    else if (key == "defaultCompanyName")        prefs.defaultCompanyName = value;
    else if (key == "applyDefaultPackages")      prefs.applyDefaultPackages = ParseBoolToken(value);
}

ProjectMassUnit ParseProjectMassUnit(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "grams" || value == "gram" || value == "g" || value == "1") {
        return ProjectMassUnit::Grams;
    }
    if (value == "pounds" || value == "pound" || value == "lb" || value == "lbs" || value == "2") {
        return ProjectMassUnit::Pounds;
    }
    if (value == "ounces" || value == "ounce" || value == "oz" || value == "3") {
        return ProjectMassUnit::Ounces;
    }
    return ProjectMassUnit::Kilograms;
}

const char* SerializeProjectMassUnit(ProjectMassUnit unit) {
    switch (unit) {
        case ProjectMassUnit::Grams: return "Grams";
        case ProjectMassUnit::Pounds: return "Pounds";
        case ProjectMassUnit::Ounces: return "Ounces";
        case ProjectMassUnit::Kilograms:
        default:
            return "Kilograms";
    }
}

ProjectTextureFiltering ParseProjectTextureFiltering(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "point" || value == "nearest" || value == "1") return ProjectTextureFiltering::Point;
    if (value == "trilinear" || value == "2") return ProjectTextureFiltering::Trilinear;
    return ProjectTextureFiltering::Bilinear;
}

const char* SerializeProjectTextureFiltering(ProjectTextureFiltering value) {
    switch (value) {
        case ProjectTextureFiltering::Point: return "Point";
        case ProjectTextureFiltering::Trilinear: return "Trilinear";
        case ProjectTextureFiltering::Bilinear:
        default: return "Bilinear";
    }
}

ProjectAntiAliasing ParseProjectAntiAliasing(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "off" || value == "none" || value == "0") return ProjectAntiAliasing::Off;
    if (value == "2x" || value == "msaa2x" || value == "1") return ProjectAntiAliasing::MSAA2x;
    if (value == "8x" || value == "msaa8x" || value == "3") return ProjectAntiAliasing::MSAA8x;
    return ProjectAntiAliasing::MSAA4x;
}

const char* SerializeProjectAntiAliasing(ProjectAntiAliasing value) {
    switch (value) {
        case ProjectAntiAliasing::Off: return "Off";
        case ProjectAntiAliasing::MSAA2x: return "MSAA2x";
        case ProjectAntiAliasing::MSAA8x: return "MSAA8x";
        case ProjectAntiAliasing::MSAA4x:
        default: return "MSAA4x";
    }
}

ProjectConsoleMode ParseProjectConsoleMode(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "floatingwindow" || value == "floating" || value == "1") return ProjectConsoleMode::FloatingWindow;
    return ProjectConsoleMode::DockedMiniButton;
}

const char* SerializeProjectConsoleMode(ProjectConsoleMode value) {
    return value == ProjectConsoleMode::FloatingWindow ? "FloatingWindow" : "DockedMiniButton";
}

ProjectConsoleTone ParseProjectConsoleTone(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "concise" || value == "professional" || value == "1") return ProjectConsoleTone::Concise;
    return ProjectConsoleTone::Fun;
}

const char* SerializeProjectConsoleTone(ProjectConsoleTone value) {
    return value == ProjectConsoleTone::Concise ? "Concise" : "Fun";
}

ProjectImportTranslatePolicy ParseProjectImportTranslatePolicy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "always" || value == "translate" || value == "1") return ProjectImportTranslatePolicy::Always;
    if (value == "never" || value == "keep" || value == "2") return ProjectImportTranslatePolicy::Never;
    return ProjectImportTranslatePolicy::Ask;
}

const char* SerializeProjectImportTranslatePolicy(ProjectImportTranslatePolicy value) {
    switch (value) {
        case ProjectImportTranslatePolicy::Always: return "Always";
        case ProjectImportTranslatePolicy::Never: return "Never";
        case ProjectImportTranslatePolicy::Ask:
        default: return "Ask";
    }
}

bool ParseProjectBool(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool ScriptPathContains(const ScriptComponent& script, const char* token) {
    if (!token || !*token) return false;
    std::string pathLower = script.path;
    std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::string tokenLower = token;
    std::transform(tokenLower.begin(), tokenLower.end(), tokenLower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return pathLower.find(tokenLower) != std::string::npos;
}

ScriptSetting* FindScriptSetting(ScriptComponent& script, const std::string& key) {
    for (ScriptSetting& setting : script.settings) {
        if (setting.key == key) {
            return &setting;
        }
    }
    return nullptr;
}

void UpgradeLegacyScriptSettings(int sceneVersion, std::vector<SceneObject>& objects) {
    if (sceneVersion >= SceneSerializationInternal::kModularSceneFormatVersion) {
        return;
    }

    for (SceneObject& obj : objects) {
        for (ScriptComponent& script : obj.scripts) {
            if (!ScriptPathContains(script, "interactableobject")) {
                continue;
            }

            ScriptSetting* optionsBlob = FindScriptSetting(script, "optionsBlob");
            ScriptSetting* options = FindScriptSetting(script, "options");
            if (!optionsBlob || optionsBlob->value.empty() || (options && !options->value.empty())) {
                continue;
            }

            if (options) {
                options->value = optionsBlob->value;
            } else {
                script.settings.push_back(ScriptSetting{"options", optionsBlob->value});
            }
            optionsBlob->value.clear();
        }
    }
}

bool ExistsWithExactName(const fs::path& candidate) {
    const fs::path parent = candidate.parent_path();
    const std::string wanted = candidate.filename().string();
    if (parent.empty() || wanted.empty()) return false;
    std::error_code ec;
    if (!fs::exists(parent, ec) || ec) return false;
    fs::directory_iterator it(parent, ec);
    if (ec) return false;
    for (const fs::directory_entry& entry : it) {
        if (entry.path().filename().string() == wanted) return true;
    }
    return false;
}

} // namespace

// Project implementation
Project::Project(const std::string& projectName, const fs::path& basePath)
    : name(projectName) {
    projectPath = basePath / projectName;
    assetsPath = projectPath / "Assets";
    scenesPath = assetsPath / "Scenes";
    scriptsPath = assetsPath / "Scripts";
    scriptsConfigPath = projectPath / "scripts.modu";
    usesNewLayout = true;
    pipeline = ProjectPipeline::Pipeline3D;
    rendererBackend = Modularity::GraphicsBackend::OpenGL;
    playerSettings.productName = projectName;
    playerSettings.defaultScene = "Main";
}

bool Project::create() {
    try {
        fs::create_directories(projectPath);
        fs::create_directories(assetsPath);
        fs::create_directories(assetsPath / "Scenes");
        fs::create_directories(assetsPath / "Scripts" / "Runtime");
        fs::create_directories(assetsPath / "Scripts" / "Editor");
        fs::create_directories(assetsPath / "Models");
        fs::create_directories(assetsPath / "Shaders");
        fs::create_directories(assetsPath / "Materials");
        fs::create_directories(projectPath / "Library" / "CompiledScripts");
        fs::create_directories(projectPath / "Library" / "InstalledPackages");
        fs::create_directories(projectPath / "Library" / "ScriptTemp");
        fs::create_directories(projectPath / "Library" / "Temp");
        fs::create_directories(projectPath / "ProjectUserSettings" / "ProjectLayout");
        fs::create_directories(projectPath / "ProjectUserSettings" / "ScriptSettings");

        saveProjectFile();

        // Initialize a default scripting build file
        std::ofstream scriptCfg(scriptsConfigPath);
        scriptCfg << "# scripts.modu\n";
        scriptCfg << "# Default native script target is C++23. Use c++17, c++20, or c++26 only when a project needs a different target.\n";
        scriptCfg << "cppStandard=c++23\n";
        scriptCfg << "scriptsDir=Assets/Scripts\n";
        scriptCfg << "outDir=Library/CompiledScripts\n";
        scriptCfg << "define=MODU_SCRIPTING=1\n";
        scriptCfg << "define=MODU_PROJECT_NAME=\"" << name << "\"\n";
        scriptCfg << "linux.linkLib=pthread\n";
        scriptCfg << "linux.linkLib=dl\n";
        scriptCfg << "win.linkLib=User32.lib\n";
        scriptCfg << "win.linkLib=Advapi32.lib\n";
        scriptCfg.close();

        std::ofstream packageManifest(projectPath / "packages.modu");
        packageManifest << "# Modularity package manifest\n";
        packageManifest << "# package=<id>\n";
        packageManifest << "# git=<id>|<name>|<url>|<path>|<includeDirs>|<defines>|<linuxLibs>|<windowsLibs>|<description>\n";
        packageManifest << "# modupak=<id>|<name>|<bundlePath>|<path>|<includeDirs>|<defines>|<linuxLibs>|<windowsLibs>|<description>\n";
        packageManifest.close();

        currentSceneName = "Main";
        isLoaded = true;
        usesNewLayout = true;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to create project: " << e.what() << std::endl;
        return false;
    }
}

bool Project::load(const fs::path& projectFilePath) {
    try {
        projectPath = projectFilePath.parent_path();
        assetsPath = projectPath / "Assets";
        Modularity::Platform::SetPortableAssetRoot(projectPath.string());

        fs::path oldScenes = projectPath / "Scenes";
        fs::path oldScripts = projectPath / "Scripts";
        fs::path oldConfig = projectPath / "Scripts.modu";
        fs::path newScenes = assetsPath / "Scenes";
        fs::path newScripts = assetsPath / "Scripts";
        fs::path newConfig = projectPath / "scripts.modu";

        bool hasOldScenes = ExistsWithExactName(oldScenes);
        bool hasOldScripts = ExistsWithExactName(oldScripts);
        bool hasOldConfig = ExistsWithExactName(oldConfig);
        bool hasNewScenes = fs::exists(newScenes);
        bool hasNewScripts = fs::exists(newScripts);
        bool hasNewConfig = fs::exists(newConfig);

        bool useNewLayout = false;
        if (hasOldScenes || hasOldScripts || hasOldConfig) {
            useNewLayout = false;
        } else if (hasNewScenes || hasNewScripts || hasNewConfig) {
            useNewLayout = true;
        }

        if (useNewLayout) {
            scenesPath = newScenes;
            scriptsPath = newScripts;
            scriptsConfigPath = hasNewConfig ? newConfig : (hasOldConfig ? oldConfig : newConfig);
        } else {
            scenesPath = oldScenes;
            scriptsPath = oldScripts;
            scriptsConfigPath = hasOldConfig ? oldConfig : (hasNewConfig ? newConfig : oldConfig);
        }
        usesNewLayout = useNewLayout;

        std::ifstream file(projectFilePath);
        if (!file.is_open()) return false;

        struct LegacyOpenXRSettings {
            bool sawAnyLegacyKey = false;
            bool sawTargetBlock = false;
            bool sawProfileList = false;
            Modularity::XR::XRRenderMode renderMode =
                Modularity::XR::XRRenderMode::SinglePassMultiview;
            Modularity::XR::XRDepthSubmission depthSubmission =
                Modularity::XR::XRDepthSubmission::None;
            std::vector<Modularity::XR::XRInteractionProfile> profiles;
            bool questEnabled = true;
            std::vector<std::string> questFeatures;
        } legacyXR;
        Modularity::XR::XRPlatformTarget xrTargetCursor = Modularity::XR::XRPlatformTarget::PC;

        std::string line;
        while (std::getline(file, line)) {
            const size_t equals = line.find('=');
            const std::string key = equals == std::string::npos ? line : line.substr(0, equals);
            const std::string value = equals == std::string::npos ? std::string() : line.substr(equals + 1);
            if (line.find("name=") == 0) {
                name = value;
            } else if (line.find("lastScene=") == 0) {
                currentSceneName = value;
            } else if (line.find("pipeline=") == 0) {
                pipeline = ParseProjectPipeline(value);
            } else if (line.find("renderer=") == 0) {
                rendererBackend = Modularity::GraphicsBackendFromString(value);
            } else if (key == "physicsBackend") {
                if (value == "PhysX") physicsSettings.backend = PhysicsBackendType::PhysX;
                else physicsSettings.backend = PhysicsBackendType::Jolt;
            } else if (line.find("physicsMassUnit=") == 0) {
                physicsSettings.massUnit = ParseProjectMassUnit(value);
            } else if (line.find("physicsGlobalGravityScale=") == 0) {
                physicsSettings.globalGravityScale = std::max(0.0f, std::stof(value));
            } else if (key == "physicsFixedTimestep") {
                physicsSettings.fixedTimestep = std::clamp(std::stof(value), 0.001f, 0.1f);
            } else if (key == "physicsSolverIterations") {
                physicsSettings.solverIterations = std::clamp(std::stoi(value), 1, 64);
            } else if (key == "physicsEnable3D") {
                physicsSettings.enable3DPhysics = ParseProjectBool(value);
            } else if (key == "physicsEnable2D") {
                physicsSettings.enable2DPhysics = ParseProjectBool(value);
            } else if (key == "physicsDefaultRaycastDistance") {
                physicsSettings.defaultRaycastDistance = std::max(0.01f, std::stof(value));
            } else if (key == "physicsRaycastHitTriggers") {
                physicsSettings.raycastHitTriggers = ParseProjectBool(value);
            } else if (key == "physicsDefaultRigidbodyMass") {
                physicsSettings.defaultRigidbodyMass = std::max(0.0001f, std::stof(value));
            } else if (key == "physicsDefaultRigidbodyDrag") {
                physicsSettings.defaultRigidbodyDrag = std::max(0.0f, std::stof(value));
            } else if (key == "physicsDefaultMaterialFriction") {
                physicsSettings.defaultMaterialFriction = std::clamp(std::stof(value), 0.0f, 1.0f);
            } else if (key == "physicsDefaultMaterialBounciness") {
                physicsSettings.defaultMaterialBounciness = std::clamp(std::stof(value), 0.0f, 1.0f);
            } else if (key.rfind("physicsLayer", 0) == 0 && key.size() > 12 && key.substr(key.size() - 5) == "_name") {
                int index = std::stoi(key.substr(12, key.size() - 17));
                if (index >= 0 && index < 32) {
                    if (physicsSettings.collisionLayers.size() <= static_cast<size_t>(index)) {
                        physicsSettings.collisionLayers.resize(static_cast<size_t>(index) + 1);
                    }
                    physicsSettings.collisionLayers[static_cast<size_t>(index)] = value;
                }
            } else if (key == "textureFormatOverride") {
                const size_t bar = value.rfind('|');
                if (bar != std::string::npos && bar > 0 && bar + 1 < value.size()) {
                    textureFormatOverrides[value.substr(0, bar)] = value.substr(bar + 1);
                }
            } else if (key.rfind("tag", 0) == 0 && key.size() > 5 && key.substr(key.size() - 5) == "_name") {
                int index = std::stoi(key.substr(3, key.size() - 8));
                if (index >= 0 && index < 256) {
                    if (tags.size() <= static_cast<size_t>(index)) {
                        tags.resize(static_cast<size_t>(index) + 1);
                    }
                    tags[static_cast<size_t>(index)] = value;
                }
            } else if (key == "graphicsVSync") {
                graphicsSettings.vsync = ParseProjectBool(value);
            } else if (key == "graphicsTargetFps") {
                graphicsSettings.targetFps = std::clamp(std::stoi(value), 1, 500);
            } else if (key == "graphicsShadowQuality") {
                graphicsSettings.shadowQuality = std::clamp(std::stoi(value), 0, 3);
            } else if (key == "graphicsRenderResolutionScale") {
                graphicsSettings.renderResolutionScale = std::clamp(std::stof(value), 0.25f, 2.0f);
            } else if (key == "graphicsTextureFiltering") {
                graphicsSettings.textureFiltering = ParseProjectTextureFiltering(value);
            } else if (key == "graphicsAntiAliasing") {
                graphicsSettings.antiAliasing = ParseProjectAntiAliasing(value);
            } else if (key == "graphicsFullscreenStartup") {
                graphicsSettings.fullscreenStartup = ParseProjectBool(value);
            } else if (key == "graphicsEditorPreviewOverrides") {
                graphicsSettings.editorPreviewOverrides = ParseProjectBool(value);
            } else if (key == "graphicsGamePreviewOverrides") {
                graphicsSettings.gamePreviewOverrides = ParseProjectBool(value);
            } else if (key == "graphicsRenderingPath") {
                graphicsSettings.renderingPath = static_cast<ProjectRenderingPath>(std::clamp(std::stoi(value), 0, 3));
            } else if (key == "graphicsHdr") {
                graphicsSettings.hdr = ParseProjectBool(value);
            } else if (key == "graphicsColorResolution") {
                graphicsSettings.colorResolution = static_cast<ProjectColorResolution>(std::clamp(std::stoi(value), 0, 2));
            } else if (key == "graphicsDefaultTextureFormat") {
                graphicsSettings.defaultTextureFormat = value;
            } else if (key == "graphicsShadowFilterQuality") {
                graphicsSettings.shadowFilterQuality = std::clamp(std::stoi(value), 0, 3);
            } else if (key == "graphicsShadowLodDistance") {
                graphicsSettings.shadowLodDistance = std::max(0.0f, std::stof(value));
            } else if (key == "graphicsShadowContactHardening") {
                graphicsSettings.shadowContactHardening = ParseProjectBool(value);
            } else if (key == "graphicsMaxTextureSize") {
                const int parsed = std::stoi(value);
                graphicsSettings.maxTextureSize = (parsed <= 0) ? 0 : std::clamp(parsed, 64, 16384);
            } else if (key == "lightingMainLightEnabled") {
                graphicsSettings.mainLightEnabled = ParseProjectBool(value);
            } else if (key == "lightingMainLightCastShadows") {
                graphicsSettings.mainLightCastShadows = ParseProjectBool(value);
            } else if (key == "lightingAdditionalLightsEnabled") {
                graphicsSettings.additionalLightsEnabled = ParseProjectBool(value);
            } else if (key == "lightingAdditionalLightsCastShadows") {
                graphicsSettings.additionalLightsCastShadows = ParseProjectBool(value);
            } else if (key == "lightingSpecularEnabled") {
                graphicsSettings.specularEnabled = ParseProjectBool(value);
            } else if (key == "lightingSoftShadows") {
                graphicsSettings.softShadows = ParseProjectBool(value);
            } else if (key == "lightingShadowMaxDistance") {
                graphicsSettings.shadowMaxDistance = std::max(0.0f, std::stof(value));
            } else if (key == "graphicsQualityPreset") {
                graphicsSettings.qualityPreset =
                    static_cast<ProjectQualityPreset>(std::clamp(std::stoi(value), 0, 3));
            } else if (key == "consoleMode") {
                consoleSettings.mode = ParseProjectConsoleMode(value);
            } else if (key == "consoleAlwaysOpenOnLaunch") {
                consoleSettings.alwaysOpenOnLaunch = ParseProjectBool(value);
            } else if (key == "consoleOpenOnlyOnErrors") {
                consoleSettings.openOnlyOnErrors = ParseProjectBool(value);
            } else if (key == "consoleTone") {
                consoleSettings.tone = ParseProjectConsoleTone(value);
            } else if (key == "moducppSyntaxLanguage") {
                const std::string lowered = TrimCopy(value);
                languageSettings.moduCppSyntaxLanguage =
                    (lowered.empty() || lowered == "global" || lowered == "default")
                        ? std::string()
                        : ModuCPPLang::NormalizeLanguageId(lowered);
            } else if (key == "moducppImportTranslatePolicy") {
                languageSettings.importTranslatePolicy = ParseProjectImportTranslatePolicy(value);
            } else if (key == "playerProductName") {
                playerSettings.productName = value;
            } else if (key == "playerCompanyName") {
                playerSettings.companyName = value;
            } else if (key == "playerDefaultScene") {
                playerSettings.defaultScene = value;
            } else if (key == "playerStartupWidth") {
                playerSettings.startupWidth = std::clamp(std::stoi(value), 64, 8192);
            } else if (key == "playerStartupHeight") {
                playerSettings.startupHeight = std::clamp(std::stoi(value), 64, 8192);
            } else if (key == "playerFullscreenStartup") {
                playerSettings.fullscreenStartup = ParseProjectBool(value);
            } else if (key == "playerNativeDisplayResolution") {
                playerSettings.nativeDisplayResolution = ParseProjectBool(value);
            } else if (key == "playerCursorLocked") {
                playerSettings.cursorLocked = ParseProjectBool(value);
            } else if (key == "playerCursorVisible") {
                playerSettings.cursorVisible = ParseProjectBool(value);
            } else if (key == "playerBuildTarget") {
                playerSettings.buildTarget = value;
            } else if (key == "playerApplicationIcon") {
                playerSettings.applicationIconPath = value;
            } else if (key == "playerSaveDataPathBehavior") {
                playerSettings.saveDataPathBehavior = value;
            } else if (key == "openXREnabled") {
                openXRSettings.enabled = ParseProjectBool(value);
            } else if (key == "openXRTrackingOrigin") {
                openXRSettings.trackingOrigin = Modularity::XR::XRTrackingOriginFromString(value);
            } else if (key == "openXRInteractionEnabled") {
                openXRSettings.interaction.enabled = ParseProjectBool(value);
            } else if (key == "openXRInteractionDefaultActions") {
                openXRSettings.interaction.defaultActionSet = value;
            } else if (key == "openXRPCRuntimeManifest") {
                openXRSettings.pc.runtimeManifestPath = value;
            } else if (key == "openXRPCRuntimeName") {
                openXRSettings.pc.runtimeDisplayName = value;
            } else if (key == "openXRPCStartOnPlay") {
                openXRSettings.pc.startOnPlay = ParseProjectBool(value);

            } else if (key == "openXRTarget") {
                Modularity::XR::XRPlatformTarget parsed;
                if (Modularity::XR::XRPlatformTargetFromString(value, parsed)) {
                    xrTargetCursor = parsed;
                    if (!legacyXR.sawTargetBlock) {
                        for (Modularity::XR::XRTargetSettings& target : openXRSettings.targets) {
                            target = Modularity::XR::XRTargetSettings{};
                        }
                    }
                    legacyXR.sawTargetBlock = true;
                }
            } else if (key == "openXRTargetEnabled") {
                openXRSettings.target(xrTargetCursor).enabled = ParseProjectBool(value);
            } else if (key == "openXRTargetRenderMode") {
                openXRSettings.target(xrTargetCursor).renderMode =
                    Modularity::XR::XRRenderModeFromString(value);
            } else if (key == "openXRTargetDepthSubmission") {
                openXRSettings.target(xrTargetCursor).depthSubmission =
                    Modularity::XR::XRDepthSubmissionFromString(value);
            } else if (key == "openXRTargetInteractionProfileCount") {
                openXRSettings.target(xrTargetCursor).interactionProfiles.clear();
            } else if (key == "openXRTargetInteractionProfile") {
                Modularity::XR::XRInteractionProfile profile;
                if (Modularity::XR::XRInteractionProfileFromString(value, profile)) {
                    openXRSettings.target(xrTargetCursor).setInteractionProfile(profile, true);
                }
            } else if (key == "openXRTargetFeatureCount") {
                openXRSettings.target(xrTargetCursor).enabledFeatures.clear();
            } else if (key == "openXRTargetFeature") {
                if (!value.empty()) {
                    openXRSettings.target(xrTargetCursor).setFeature(value.c_str(), true);
                }

            } else if (key == "openXRRenderMode") {
                legacyXR.sawAnyLegacyKey = true;
                legacyXR.renderMode = Modularity::XR::XRRenderModeFromString(value);
            } else if (key == "openXRDepthSubmission") {
                legacyXR.sawAnyLegacyKey = true;
                legacyXR.depthSubmission = Modularity::XR::XRDepthSubmissionFromString(value);
            } else if (key == "openXRInteractionProfileCount") {
                legacyXR.sawAnyLegacyKey = true;
                legacyXR.sawProfileList = true;
                legacyXR.profiles.clear();
            } else if (key == "openXRInteractionProfile") {
                Modularity::XR::XRInteractionProfile profile;
                if (Modularity::XR::XRInteractionProfileFromString(value, profile)) {
                    legacyXR.sawAnyLegacyKey = true;
                    legacyXR.profiles.push_back(profile);
                }
            } else if (key == "openXRQuestEnabled") {
                legacyXR.sawAnyLegacyKey = true;
                legacyXR.questEnabled = ParseProjectBool(value);
            } else if (key == "openXRQuestHandTracking" || key == "openXRQuestPassthrough" ||
                       key == "openXRQuestDisplayRefreshRate" ||
                       key == "openXRQuestDisplayUtilities" ||
                       key == "openXRQuestFoveatedRendering" ||
                       key == "openXRQuestPerformanceSettings") {
                legacyXR.sawAnyLegacyKey = true;
                if (ParseProjectBool(value)) {
                    std::string featureId = key.substr(std::strlen("openXRQuest"));
                    if (!featureId.empty()) {
                        featureId[0] = static_cast<char>(
                            std::tolower(static_cast<unsigned char>(featureId[0])));
                        legacyXR.questFeatures.push_back(featureId);
                    }
                }
            }
        }
        file.close();

        if (legacyXR.sawAnyLegacyKey && !legacyXR.sawTargetBlock) {
            using namespace Modularity::XR;
            for (int i = 0; i < static_cast<int>(XRPlatformTarget::Count); ++i) {
                const auto which = static_cast<XRPlatformTarget>(i);
                XRTargetSettings& target = openXRSettings.target(which);
                target.renderMode = legacyXR.renderMode;
                target.depthSubmission = legacyXR.depthSubmission;

                target.enabled = (which != XRPlatformTarget::PC) && legacyXR.questEnabled;

                if (legacyXR.sawProfileList) {
                    target.interactionProfiles.clear();
                    for (XRInteractionProfile profile : legacyXR.profiles) {
                        if (IsInteractionProfileAvailableOn(profile, which)) {
                            target.setInteractionProfile(profile, true);
                        }
                    }
                } else {
                    target.interactionProfiles = DefaultInteractionProfiles(which);
                }

                target.enabledFeatures.clear();
                if (which != XRPlatformTarget::PC) {
                    for (const std::string& featureId : legacyXR.questFeatures) {
                        target.setFeature(featureId.c_str(), true);
                    }
                }
            }
        }

        if (currentSceneName.empty()) {
            currentSceneName = "Main";
        }
        if (physicsSettings.collisionLayers.empty() || physicsSettings.collisionLayers[0].empty()) {
            if (physicsSettings.collisionLayers.empty()) physicsSettings.collisionLayers.resize(1);
            physicsSettings.collisionLayers[0] = "Default";
        }
        if (tags.empty() || tags[0].empty()) {
            if (tags.empty()) tags.resize(1);
            tags[0] = "Untagged";
        }
        if (playerSettings.productName.empty()) playerSettings.productName = name;
        if (playerSettings.defaultScene.empty()) playerSettings.defaultScene = currentSceneName;

        isLoaded = true;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to load project: " << e.what() << std::endl;
        return false;
    }
}

void Project::saveProjectFile() const {
    std::ofstream file(projectPath / "project.modu");
    file << "name=" << name << "\n";
    file << "lastScene=" << currentSceneName << "\n";
    file << "pipeline=" << SerializeProjectPipeline(pipeline) << "\n";
    file << "renderer=" << Modularity::ToString(rendererBackend) << "\n";
    file << "physicsBackend=" << PhysicsBackendLabel(physicsSettings.backend) << "\n";
    file << "physicsMassUnit=" << SerializeProjectMassUnit(physicsSettings.massUnit) << "\n";
    file << "physicsGlobalGravityScale=" << std::max(0.0f, physicsSettings.globalGravityScale) << "\n";
    file << "physicsFixedTimestep=" << std::clamp(physicsSettings.fixedTimestep, 0.001f, 0.1f) << "\n";
    file << "physicsSolverIterations=" << std::clamp(physicsSettings.solverIterations, 1, 64) << "\n";
    file << "physicsEnable3D=" << (physicsSettings.enable3DPhysics ? 1 : 0) << "\n";
    file << "physicsEnable2D=" << (physicsSettings.enable2DPhysics ? 1 : 0) << "\n";
    file << "physicsDefaultRaycastDistance=" << std::max(0.01f, physicsSettings.defaultRaycastDistance) << "\n";
    file << "physicsRaycastHitTriggers=" << (physicsSettings.raycastHitTriggers ? 1 : 0) << "\n";
    file << "physicsDefaultRigidbodyMass=" << std::max(0.0001f, physicsSettings.defaultRigidbodyMass) << "\n";
    file << "physicsDefaultRigidbodyDrag=" << std::max(0.0f, physicsSettings.defaultRigidbodyDrag) << "\n";
    file << "physicsDefaultMaterialFriction=" << std::clamp(physicsSettings.defaultMaterialFriction, 0.0f, 1.0f) << "\n";
    file << "physicsDefaultMaterialBounciness=" << std::clamp(physicsSettings.defaultMaterialBounciness, 0.0f, 1.0f) << "\n";
    file << "physicsLayerCount=" << physicsSettings.collisionLayers.size() << "\n";
    for (size_t i = 0; i < physicsSettings.collisionLayers.size(); ++i) {
        file << "physicsLayer" << i << "_name=" << physicsSettings.collisionLayers[i] << "\n";
    }
    file << "tagCount=" << tags.size() << "\n";
    for (size_t i = 0; i < tags.size(); ++i) {
        file << "tag" << i << "_name=" << tags[i] << "\n";
    }
    for (const auto& [texPath, format] : textureFormatOverrides) {
        if (texPath.empty() || format.empty() || format == "Auto") continue;
        file << "textureFormatOverride=" << texPath << "|" << format << "\n";
    }
    file << "graphicsVSync=" << (graphicsSettings.vsync ? 1 : 0) << "\n";
    file << "graphicsTargetFps=" << std::clamp(graphicsSettings.targetFps, 1, 500) << "\n";
    file << "graphicsShadowQuality=" << std::clamp(graphicsSettings.shadowQuality, 0, 3) << "\n";
    file << "graphicsRenderResolutionScale=" << std::clamp(graphicsSettings.renderResolutionScale, 0.25f, 2.0f) << "\n";
    file << "graphicsTextureFiltering=" << SerializeProjectTextureFiltering(graphicsSettings.textureFiltering) << "\n";
    file << "graphicsAntiAliasing=" << SerializeProjectAntiAliasing(graphicsSettings.antiAliasing) << "\n";
    file << "graphicsFullscreenStartup=" << (graphicsSettings.fullscreenStartup ? 1 : 0) << "\n";
    file << "graphicsEditorPreviewOverrides=" << (graphicsSettings.editorPreviewOverrides ? 1 : 0) << "\n";
    file << "graphicsGamePreviewOverrides=" << (graphicsSettings.gamePreviewOverrides ? 1 : 0) << "\n";
    file << "graphicsRenderingPath=" << static_cast<int>(graphicsSettings.renderingPath) << "\n";
    file << "graphicsHdr=" << (graphicsSettings.hdr ? 1 : 0) << "\n";
    file << "graphicsColorResolution=" << static_cast<int>(graphicsSettings.colorResolution) << "\n";
    file << "graphicsDefaultTextureFormat=" << (graphicsSettings.defaultTextureFormat.empty() ? "Auto" : graphicsSettings.defaultTextureFormat) << "\n";
    file << "graphicsMaxTextureSize=" << graphicsSettings.maxTextureSize << "\n";
    file << "graphicsShadowFilterQuality=" << graphicsSettings.shadowFilterQuality << "\n";
    file << "graphicsShadowLodDistance=" << graphicsSettings.shadowLodDistance << "\n";
    file << "graphicsShadowContactHardening=" << (graphicsSettings.shadowContactHardening ? 1 : 0) << "\n";
    file << "lightingMainLightEnabled=" << (graphicsSettings.mainLightEnabled ? 1 : 0) << "\n";
    file << "lightingMainLightCastShadows=" << (graphicsSettings.mainLightCastShadows ? 1 : 0) << "\n";
    file << "lightingAdditionalLightsEnabled=" << (graphicsSettings.additionalLightsEnabled ? 1 : 0) << "\n";
    file << "lightingAdditionalLightsCastShadows=" << (graphicsSettings.additionalLightsCastShadows ? 1 : 0) << "\n";
    file << "lightingSpecularEnabled=" << (graphicsSettings.specularEnabled ? 1 : 0) << "\n";
    file << "lightingSoftShadows=" << (graphicsSettings.softShadows ? 1 : 0) << "\n";
    file << "lightingShadowMaxDistance=" << std::max(0.0f, graphicsSettings.shadowMaxDistance) << "\n";
    file << "graphicsQualityPreset=" << static_cast<int>(graphicsSettings.qualityPreset) << "\n";
    file << "consoleMode=" << SerializeProjectConsoleMode(consoleSettings.mode) << "\n";
    file << "consoleAlwaysOpenOnLaunch=" << (consoleSettings.alwaysOpenOnLaunch ? 1 : 0) << "\n";
    file << "consoleOpenOnlyOnErrors=" << (consoleSettings.openOnlyOnErrors ? 1 : 0) << "\n";
    file << "consoleTone=" << SerializeProjectConsoleTone(consoleSettings.tone) << "\n";
    file << "moducppSyntaxLanguage="
         << (languageSettings.usesGlobalModuCppDefault()
                 ? std::string()
                 : ModuCPPLang::NormalizeLanguageId(languageSettings.moduCppSyntaxLanguage))
         << "\n";
    file << "moducppImportTranslatePolicy="
         << SerializeProjectImportTranslatePolicy(languageSettings.importTranslatePolicy) << "\n";
    file << "playerProductName=" << (playerSettings.productName.empty() ? name : playerSettings.productName) << "\n";
    file << "playerCompanyName=" << playerSettings.companyName << "\n";
    file << "playerDefaultScene=" << (playerSettings.defaultScene.empty() ? currentSceneName : playerSettings.defaultScene) << "\n";
    file << "playerStartupWidth=" << std::clamp(playerSettings.startupWidth, 64, 8192) << "\n";
    file << "playerStartupHeight=" << std::clamp(playerSettings.startupHeight, 64, 8192) << "\n";
    file << "playerFullscreenStartup=" << (playerSettings.fullscreenStartup ? 1 : 0) << "\n";
    file << "playerNativeDisplayResolution=" << (playerSettings.nativeDisplayResolution ? 1 : 0) << "\n";
    file << "playerCursorLocked=" << (playerSettings.cursorLocked ? 1 : 0) << "\n";
    file << "playerCursorVisible=" << (playerSettings.cursorVisible ? 1 : 0) << "\n";
    file << "playerBuildTarget=" << playerSettings.buildTarget << "\n";
    file << "playerApplicationIcon=" << playerSettings.applicationIconPath << "\n";
    file << "playerSaveDataPathBehavior=" << playerSettings.saveDataPathBehavior << "\n";

    {
        const Modularity::XR::ProjectOpenXRSettings& xr = openXRSettings;
        file << "openXREnabled=" << (xr.enabled ? 1 : 0) << "\n";
        file << "openXRTrackingOrigin="
             << (xr.trackingOrigin == Modularity::XR::XRTrackingOrigin::Eye ? "eye" : "floor")
             << "\n";
        file << "openXRInteractionEnabled=" << (xr.interaction.enabled ? 1 : 0) << "\n";
        file << "openXRInteractionDefaultActions=" << xr.interaction.defaultActionSet << "\n";
        file << "openXRPCRuntimeManifest=" << xr.pc.runtimeManifestPath << "\n";
        file << "openXRPCRuntimeName=" << xr.pc.runtimeDisplayName << "\n";
        file << "openXRPCStartOnPlay=" << (xr.pc.startOnPlay ? 1 : 0) << "\n";

        for (int i = 0; i < static_cast<int>(Modularity::XR::XRPlatformTarget::Count); ++i) {
            const auto which = static_cast<Modularity::XR::XRPlatformTarget>(i);
            const Modularity::XR::XRTargetSettings& target = xr.target(which);
            file << "openXRTarget=" << Modularity::XR::SerializationId(which) << "\n";
            file << "openXRTargetEnabled=" << (target.enabled ? 1 : 0) << "\n";
            file << "openXRTargetRenderMode="
                 << (target.renderMode == Modularity::XR::XRRenderMode::MultiPass
                         ? "multiPass"
                         : "singlePassMultiview")
                 << "\n";
            file << "openXRTargetDepthSubmission="
                 << (target.depthSubmission == Modularity::XR::XRDepthSubmission::Depth ? "depth"
                                                                                        : "none")
                 << "\n";
            file << "openXRTargetInteractionProfileCount=" << target.interactionProfiles.size()
                 << "\n";
            for (Modularity::XR::XRInteractionProfile profile : target.interactionProfiles) {
                file << "openXRTargetInteractionProfile="
                     << Modularity::XR::SerializationId(profile) << "\n";
            }
            file << "openXRTargetFeatureCount=" << target.enabledFeatures.size() << "\n";
            for (const std::string& featureId : target.enabledFeatures) {
                file << "openXRTargetFeature=" << featureId << "\n";
            }
        }
    }
    file.close();
}

std::vector<std::string> Project::getSceneList() const {
    std::vector<std::string> scenes;
    try {
        for (const auto& entry : fs::directory_iterator(scenesPath)) {
            if (entry.path().extension() == ".scene") {
                scenes.push_back(entry.path().stem().string());
            }
        }
    } catch (...) {}
    return scenes;
}

fs::path Project::getSceneFilePath(const std::string& sceneName) const {
    return scenesPath / (sceneName + ".scene");
}

// ProjectManager implementation
ProjectManager::ProjectManager() {
    #if defined(__ANDROID__)
    if (const char* dataPath = Modularity::AndroidRuntime::GetInternalDataPath()) {
        appDataPath = fs::path(dataPath) / ".Modularity";
    } else {
        appDataPath = fs::path("/data/local/tmp/.Modularity");
    }
    #elif defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata) {
        appDataPath = fs::path(appdata) / ".Modularity";
    } else {
        appDataPath = fs::current_path() / "AppData";
    }
    #else
    const char* home = std::getenv("HOME");
    if (home) {
        appDataPath = fs::path(home) / ".Modularity";
    } else {
        appDataPath = fs::current_path() / ".Modularity";
    }
    #endif

    fs::create_directories(appDataPath);
#if !MODULARITY_RUNTIME_ONLY
    loadRecentProjects();
    loadLauncherSettings();
#else
    const std::string fallback = GetPlatformDefaultProjectsPath();
    std::snprintf(defaultProjectLocation, sizeof(defaultProjectLocation), "%s", fallback.c_str());
#endif

    std::snprintf(newProjectLocation, sizeof(newProjectLocation), "%s", defaultProjectLocation);
}

void ProjectManager::loadRecentProjects() {
    recentProjects.clear();
    fs::path recentFile = appDataPath / "recent_projects.txt";

    if (!fs::exists(recentFile)) {
        return;
    }

    std::ifstream file(recentFile);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (line.empty()) continue;

        RecentProject rp;
        size_t pos1 = line.find('|');
        size_t pos2 = line.find('|', pos1 + 1);

        if (pos1 != std::string::npos && pos2 != std::string::npos) {
            rp.name = line.substr(0, pos1);
            rp.path = line.substr(pos1 + 1, pos2 - pos1 - 1);
            rp.lastOpened = line.substr(pos2 + 1);

            rp.path.erase(0, rp.path.find_first_not_of(" \t\r\n"));
            rp.path.erase(rp.path.find_last_not_of(" \t\r\n") + 1);

            if (fs::exists(rp.path)) {
                recentProjects.push_back(rp);
            }
        }
    }
    file.close();
}

void ProjectManager::saveRecentProjects() {
    fs::path recentFile = appDataPath / "recent_projects.txt";
    std::ofstream file(recentFile);

    for (const auto& rp : recentProjects) {
        std::string absolutePath = rp.path;
        try {
            if (fs::exists(rp.path)) {
                absolutePath = fs::canonical(rp.path).string();
            }
        } catch (...) {
            // Keep original path if canonical fails
        }
        file << rp.name << "|" << absolutePath << "|" << rp.lastOpened << "\n";
    }
    file.close();
}

void ProjectManager::loadLauncherSettings() {
    defaultProjectLocation[0] = '\0';
    acceptedTermsVersion.clear();
    windowsDisclaimerAcknowledgedV68 = false;
    moduCppNvimWarningDismissedV1 = false;
    androidStoragePromptDismissedV1 = false;
    editorLanguage = "english";
    moduCppDefaultLanguage = "english";
    osLanguagePromptAnswered = false;
    preferences = EditorGlobalPreferences{};
    fs::path settingsFile = appDataPath / "launcher_settings.modu";

    if (fs::exists(settingsFile)) {
        std::ifstream file(settingsFile);
        std::string line;
        while (std::getline(file, line)) {
            const std::string cleaned = TrimCopy(line);
            if (cleaned.empty() || cleaned[0] == '#') continue;

            const size_t eq = cleaned.find('=');
            if (eq == std::string::npos) continue;

            const std::string key = TrimCopy(cleaned.substr(0, eq));
            const std::string value = TrimCopy(cleaned.substr(eq + 1));
            if (key == "defaultProjectLocation" && !value.empty()) {
                std::snprintf(defaultProjectLocation, sizeof(defaultProjectLocation), "%s", value.c_str());
            } else if (key == "acceptedTermsVersion") {
                acceptedTermsVersion = value;
            } else if (key == "WindowsDisclaimerAcknowledged_V6_8") {
                windowsDisclaimerAcknowledgedV68 = (value == "1" || value == "true" || value == "yes");
            } else if (key == "ModuCppNvimWarningDismissed_V1") {
                moduCppNvimWarningDismissedV1 = (value == "1" || value == "true" || value == "yes");
            } else if (key == "AndroidStoragePromptDismissed_V1") {
                androidStoragePromptDismissedV1 = (value == "1" || value == "true" || value == "yes");
            } else if (key == "editorLanguage") {
                editorLanguage = value;
            } else if (key == "moducppDefaultLanguage") {
                moduCppDefaultLanguage = ModuCPPLang::NormalizeLanguageId(value);
            } else if (key == "osLanguagePromptAnswered") {
                osLanguagePromptAnswered = (value == "1" || value == "true" || value == "yes");
            } else if (key == "defaultPackage") {
                if (!value.empty()) {
                    preferences.setDefaultPackage(value, true);
                }
            } else if (key.rfind("pref.", 0) == 0) {
                ApplyGlobalPreferenceKey(preferences, key.substr(5), value);
            }
        }
    }

    if (defaultProjectLocation[0] == '\0') {
        const std::string fallback = GetPlatformDefaultProjectsPath();
        std::snprintf(defaultProjectLocation, sizeof(defaultProjectLocation), "%s", fallback.c_str());
    }

    editorLanguage = Modularity::Loc::NormalizeLanguageId(editorLanguage);
    Modularity::Loc::SetLanguage(editorLanguage);

#if defined(__ANDROID__)
    {
        std::error_code ec;
        bool persistedUsable = false;
        if (defaultProjectLocation[0] != '\0') {
            fs::create_directories(defaultProjectLocation, ec);
            persistedUsable = !ec && fs::exists(defaultProjectLocation);
        }
        if (!persistedUsable) {
            const std::string androidDefault = GetPlatformDefaultProjectsPath();
            std::snprintf(defaultProjectLocation, sizeof(defaultProjectLocation), "%s",
                          androidDefault.c_str());
            ec.clear();
            fs::create_directories(defaultProjectLocation, ec);
        }
    }
#endif
}

void ProjectManager::saveLauncherSettings() const {
    fs::path settingsFile = appDataPath / "launcher_settings.modu";
    std::ofstream file(settingsFile);
    if (!file.is_open()) {
        return;
    }

    file << "# Modularity launcher settings\n";
    if (!acceptedTermsVersion.empty()) {
        file << "acceptedTermsVersion=" << acceptedTermsVersion << "\n";
    }
    file << "WindowsDisclaimerAcknowledged_V6_8=" << (windowsDisclaimerAcknowledgedV68 ? "1" : "0") << "\n";
    file << "ModuCppNvimWarningDismissed_V1=" << (moduCppNvimWarningDismissedV1 ? "1" : "0") << "\n";
    file << "AndroidStoragePromptDismissed_V1=" << (androidStoragePromptDismissedV1 ? "1" : "0") << "\n";
    file << "defaultProjectLocation=" << defaultProjectLocation << "\n";
    file << "editorLanguage=" << editorLanguage << "\n";
    file << "moducppDefaultLanguage=" << ModuCPPLang::NormalizeLanguageId(moduCppDefaultLanguage) << "\n";
    file << "osLanguagePromptAnswered=" << (osLanguagePromptAnswered ? "1" : "0") << "\n";

    const EditorGlobalPreferences& p = preferences;
    auto boolToken = [](bool v) { return v ? "1" : "0"; };
    file << "\n# Global editor preferences (launcher Settings tab)\n";
    file << "pref.themePreset=" << p.themePreset << "\n";
    file << "pref.uiFontAsset=" << p.uiFontAsset << "\n";
    file << "pref.chromeScale=" << p.chromeScale << "\n";
    file << "pref.animationMode=" << p.animationMode << "\n";
    file << "pref.uiScale=" << p.uiScale << "\n";
    file << "pref.launcherIntroAnimation=" << boolToken(p.launcherIntroAnimation) << "\n";
    file << "pref.launcherProjectThumbnails=" << boolToken(p.launcherProjectThumbnails) << "\n";
    file << "pref.launcherRememberLastTab=" << boolToken(p.launcherRememberLastTab) << "\n";
    file << "pref.launcherLastTab=" << p.launcherLastTab << "\n";
    file << "pref.launcherShowFullPaths=" << boolToken(p.launcherShowFullPaths) << "\n";
    file << "pref.launcherProjectSort=" << p.launcherProjectSort << "\n";
    file << "pref.feedbackSounds=" << boolToken(p.feedbackSounds) << "\n";
    file << "pref.feedbackClickSounds=" << boolToken(p.feedbackClickSounds) << "\n";
    file << "pref.feedbackErrorSounds=" << boolToken(p.feedbackErrorSounds) << "\n";
    file << "pref.feedbackOtherSounds=" << boolToken(p.feedbackOtherSounds) << "\n";
    file << "pref.reopenLastProject=" << boolToken(p.reopenLastProject) << "\n";
    file << "pref.hierarchyTexturePreviews=" << boolToken(p.hierarchyTexturePreviews) << "\n";
    file << "pref.sceneGizmos=" << boolToken(p.sceneGizmos) << "\n";
    file << "pref.gizmoCameraOverlays=" << boolToken(p.gizmoCameraOverlays) << "\n";
    file << "pref.consoleWrapText=" << boolToken(p.consoleWrapText) << "\n";
    file << "pref.quickToolsPinned=" << boolToken(p.quickToolsPinned) << "\n";
    file << "pref.editorVSync=" << boolToken(p.editorVSync) << "\n";
    file << "pref.editorFpsCapEnabled=" << boolToken(p.editorFpsCapEnabled) << "\n";
    file << "pref.editorFpsCap=" << p.editorFpsCap << "\n";
    file << "pref.performanceMode=" << (p.performanceMode.empty() ? "Auto" : p.performanceMode) << "\n";
    file << "pref.hardwareTier=" << p.hardwareTier << "\n";
    file << "pref.benchFullscreenPassMs=" << p.benchFullscreenPassMs << "\n";
    file << "pref.benchBlurPassMs=" << p.benchBlurPassMs << "\n";
    file << "pref.benchDrawCallBatchMs=" << p.benchDrawCallBatchMs << "\n";
    file << "pref.lowSpecPromptAnswered=" << boolToken(p.lowSpecPromptAnswered) << "\n";
    file << "pref.scriptAutoCompile=" << boolToken(p.scriptAutoCompile) << "\n";
    file << "pref.scriptAutoCompileInterval=" << p.scriptAutoCompileInterval << "\n";
    file << "pref.scriptDirectoryScanInterval=" << p.scriptDirectoryScanInterval << "\n";
    file << "pref.defaultPipeline=" << p.defaultPipeline << "\n";
    file << "pref.defaultRenderer=" << p.defaultRenderer << "\n";
    file << "pref.defaultCompanyName=" << p.defaultCompanyName << "\n";
    file << "pref.applyDefaultPackages=" << boolToken(p.applyDefaultPackages) << "\n";
    file << "\n# ModuPAKs added to every newly created project\n";
    for (const auto& id : p.defaultPackageIds) {
        if (!id.empty()) {
            file << "defaultPackage=" << id << "\n";
        }
    }
}

std::string ProjectManager::effectiveModuCppLanguage() const {
    if (!currentProject.languageSettings.usesGlobalModuCppDefault()) {
        return ModuCPPLang::NormalizeLanguageId(currentProject.languageSettings.moduCppSyntaxLanguage);
    }
    return ModuCPPLang::NormalizeLanguageId(moduCppDefaultLanguage);
}

void ProjectManager::addToRecentProjects(const std::string& name, const std::string& path) {
#if MODULARITY_RUNTIME_ONLY
    (void)name;
    (void)path;
    return;
#else
    std::string absolutePath = path;
    try {
        if (fs::exists(path)) {
            absolutePath = fs::canonical(path).string();
        } else {
            // For new projects, the file might not exist yet - use absolute()
            absolutePath = fs::absolute(path).string();
        }
    } catch (...) {
        // Keep original path if conversion fails
    }

    recentProjects.erase(
        std::remove_if(recentProjects.begin(), recentProjects.end(),
            [&absolutePath](const RecentProject& rp) { return rp.path == absolutePath; }),
        recentProjects.end()
    );

    std::time_t now = std::time(nullptr);
    char timeStr[64];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M", std::localtime(&now));

    RecentProject rp;
    rp.name = name;
    rp.path = absolutePath;  // Use absolute path
    rp.lastOpened = timeStr;
    recentProjects.insert(recentProjects.begin(), rp);

    saveRecentProjects();
#endif
}

bool ProjectManager::loadProject(const std::string& path) {
    if (currentProject.load(path)) {
        addToRecentProjects(currentProject.name, path);
        return true;
    }
    errorMessage = "Failed to load project file";
    return false;
}

bool SceneSerializationInternal::WriteLegacySceneStream(std::ostream& file,
                                                        const std::vector<SceneObject>& objects,
                                                        int nextId,
                                                        float timeOfDay,
                                                        const SkyboxSettings& skyboxSettings) {
    try {
        file << "# Scene File\n";
        file << "version=" << SceneSerializationInternal::kLegacySceneFormatVersion << "\n";
        file << "nextId=" << nextId << "\n";
        file << "timeOfDay=" << timeOfDay << "\n";
        file << "skyboxMode=" << static_cast<int>(skyboxSettings.mode) << "\n";
        file << "skyboxCachedSky=" << (skyboxSettings.cachedSky ? 1 : 0) << "\n";
        file << "skyboxSunFollowsLight=" << (skyboxSettings.sunFollowsDirectionalLight ? 1 : 0) << "\n";
        file << "skyboxVolumetric=" << (skyboxSettings.volumetricEnabled ? 1 : 0) << "\n";
        file << "skyboxVolumetricIntensity=" << skyboxSettings.volumetricIntensity << "\n";
        file << "skyboxVolumetricSteps=" << skyboxSettings.volumetricSteps << "\n";
        file << "skyboxVolumetricScattering=" << skyboxSettings.volumetricScattering << "\n";
        file << "skyboxVolumetricDensity=" << skyboxSettings.volumetricDensity << "\n";
        file << "skyboxVolumetricMaxDistance=" << skyboxSettings.volumetricMaxDistance << "\n";
        file << "skyboxVolumetricHalfRes=" << (skyboxSettings.volumetricHalfResolution ? 1 : 0) << "\n";
        file << "skyboxWeatherEnabled=" << (skyboxSettings.weather.enabled ? 1 : 0) << "\n";
        file << "skyboxWeatherMode=" << static_cast<int>(skyboxSettings.weather.mode) << "\n";
        file << "skyboxWeatherIntensity=" << skyboxSettings.weather.intensity << "\n";
        file << "skyboxWeatherWindSpeed=" << skyboxSettings.weather.windSpeed << "\n";
        file << "skyboxWeatherRainOpacity=" << skyboxSettings.weather.rainOpacity << "\n";
        file << "skyboxWeatherRainSpeed=" << skyboxSettings.weather.rainSpeed << "\n";
        file << "skyboxWeatherRainScale=" << skyboxSettings.weather.rainScale << "\n";
        file << "skyboxWeatherLightningInterval=" << skyboxSettings.weather.lightningInterval << "\n";
        file << "skyboxWeatherLightningIntensity=" << skyboxSettings.weather.lightningIntensity << "\n";
        file << "skyboxWeatherAmbienceVolume=" << skyboxSettings.weather.ambienceVolume << "\n";
        file << "skyboxWeatherHeavyThreshold=" << skyboxSettings.weather.heavyRainThreshold << "\n";
        file << "skyboxWeatherClearSound=" << skyboxSettings.weather.clearSoundPath << "\n";
        file << "skyboxWeatherOvercastSound=" << skyboxSettings.weather.overcastSoundPath << "\n";
        file << "skyboxWeatherRainSound=" << skyboxSettings.weather.rainSoundPath << "\n";
        file << "skyboxWeatherHeavyRainSound=" << skyboxSettings.weather.heavyRainSoundPath << "\n";
        file << "skyboxWeatherThunderSound=" << skyboxSettings.weather.thunderSoundPath << "\n";
        file << "skyboxCloudHorizonFade=" << skyboxSettings.cloudHorizonFade << "\n";
        file << "skyboxSkyResolution=" << skyboxSettings.skyResolution << "\n";
        file << "skyboxSkyRefreshInterval=" << skyboxSettings.skyRefreshInterval << "\n";
        file << "skyboxCloudMode=" << skyboxSettings.cloudMode << "\n";
        file << "skyboxCloudCoverage=" << skyboxSettings.cloudCoverage << "\n";
        file << "skyboxCloudDensity=" << skyboxSettings.cloudDensity << "\n";
        file << "skyboxCloudSteps=" << skyboxSettings.cloudSteps << "\n";
        file << "skyboxCloudLightSteps=" << skyboxSettings.cloudLightSteps << "\n";
        file << "skyboxCloudBottom=" << skyboxSettings.cloudBottom << "\n";
        file << "skyboxCloudTop=" << skyboxSettings.cloudTop << "\n";
        file << "skyboxCloudWind=" << skyboxSettings.cloudWind << "\n";
        file << "skyboxCloudDarkness=" << skyboxSettings.cloudDarkness << "\n";
        file << "skyboxVolumetricMaxIntensity=" << skyboxSettings.volumetricMaxIntensity << "\n";
        file << "skyboxVolumetricDenoise=" << (skyboxSettings.volumetricDenoise ? 1 : 0) << "\n";
        file << "skyboxVolumetricDenoiseRadius=" << skyboxSettings.volumetricDenoiseRadius << "\n";
        file << "skyboxSsr=" << (skyboxSettings.ssrEnabled ? 1 : 0) << "\n";
        file << "skyboxSsrIntensity=" << skyboxSettings.ssrIntensity << "\n";
        file << "skyboxSsrSteps=" << skyboxSettings.ssrSteps << "\n";
        file << "skyboxSsrRefineSteps=" << skyboxSettings.ssrRefineSteps << "\n";
        file << "skyboxSsrMaxDistance=" << skyboxSettings.ssrMaxDistance << "\n";
        file << "skyboxSsrThickness=" << skyboxSettings.ssrThickness << "\n";
        file << "skyboxSsrMaxRoughness=" << skyboxSettings.ssrMaxRoughness << "\n";
        file << "skyboxSsrHalfRes=" << (skyboxSettings.ssrHalfResolution ? 1 : 0) << "\n";
        file << "skyboxGi=" << (skyboxSettings.giEnabled ? 1 : 0) << "\n";
        file << "skyboxGiIntensity=" << skyboxSettings.giIntensity << "\n";
        file << "skyboxGiResolution=" << skyboxSettings.giResolution << "\n";
        file << "skyboxGiMaxExtent=" << skyboxSettings.giMaxExtent << "\n";
        file << "skyboxGiConeSteps=" << skyboxSettings.giConeSteps << "\n";
        file << "skyboxGiBounces=" << skyboxSettings.giBounces << "\n";
        file << "skyboxSunTexture=" << skyboxSettings.sunTexturePath << "\n";
        file << "skyboxMoonTexture=" << skyboxSettings.moonTexturePath << "\n";
        file << "skyboxScrollTexture=" << skyboxSettings.scrollingTexturePath << "\n";
        file << "skyboxScrollRepeat=" << skyboxSettings.scrollingRepeatX << "," << skyboxSettings.scrollingRepeatY << "\n";
        file << "skyboxScrollLookSensitivity=" << skyboxSettings.scrollingLookSensitivity << "\n";
        file << "skyboxScrollVerticalInfluence=" << skyboxSettings.scrollingVerticalInfluence << "\n";
        file << "skyboxEnvironmentReflections=" << (skyboxSettings.environmentReflections ? 1 : 0) << "\n";
        file << "skyboxEnvironmentReflectionIntensity=" << skyboxSettings.environmentReflectionIntensity << "\n";
        file << "skyboxReflectionDistanceFadeStart=" << skyboxSettings.reflectionDistanceFadeStart << "\n";
        file << "skyboxReflectionDistanceFadeEnd=" << skyboxSettings.reflectionDistanceFadeEnd << "\n";
        file << "fogEnabled=" << (skyboxSettings.fogEnabled ? 1 : 0) << "\n";
        file << "fogMode=" << skyboxSettings.fogMode << "\n";
        file << "fogColor=" << skyboxSettings.fogColor.r << "," << skyboxSettings.fogColor.g << "," << skyboxSettings.fogColor.b << "\n";
        file << "fogStart=" << skyboxSettings.fogStart << "\n";
        file << "fogEnd=" << skyboxSettings.fogEnd << "\n";
        file << "fogDensity=" << skyboxSettings.fogDensity << "\n";
        file << "fogHeight=" << skyboxSettings.fogHeight << "\n";
        file << "fogHeightFalloff=" << skyboxSettings.fogHeightFalloff << "\n";
        file << "objectCount=" << objects.size() << "\n";
        file << "\n";

        for (const auto& sourceObj : objects) {
            SceneObject obj = sourceObj;
            EnsureInspectorComponentMetadata(obj);
            file << "[Object]\n";
            file << "id=" << obj.id << "\n";
            file << "name=" << obj.name << "\n";
            ObjectType legacyType = GetLegacyTypeFromComponents(obj);
            file << "type=" << static_cast<int>(legacyType) << "\n";
            file << "enabled=" << (obj.enabled ? 1 : 0) << "\n";
            file << "invariable=" << (obj.IsInvariable ? 1 : 0) << "\n";
            file << "layer=" << obj.layer << "\n";
            file << "tag=" << obj.tag << "\n";
            file << "editorIconTint=" << obj.editorIconTint.r << "," << obj.editorIconTint.g << ","
                 << obj.editorIconTint.b << "," << obj.editorIconTint.a << "\n";
            file << "editorIcon=" << obj.editorIconPath << "\n";
            file << "editorIconGizmo=" << (obj.editorIconShowInViewport ? 1 : 0) << "\n";
            file << "hasRenderer=" << (obj.hasRenderer ? 1 : 0) << "\n";
            file << "renderType=" << static_cast<int>(obj.renderType) << "\n";
            file << "faceCamera=" << (obj.faceCamera ? 1 : 0) << "\n";
            file << "hasLight=" << (obj.hasLight ? 1 : 0) << "\n";
            file << "hasLight2D=" << (obj.hasLight2D ? 1 : 0) << "\n";
            file << "hasReflectionCast=" << (obj.hasReflectionCast ? 1 : 0) << "\n";
            file << "hasCamera=" << (obj.hasCamera ? 1 : 0) << "\n";
            file << "hasPostFX=" << (obj.hasPostFX ? 1 : 0) << "\n";
            file << "hasUI=" << (obj.hasUI ? 1 : 0) << "\n";
            file << "hasShadowCaster2D=" << (obj.hasShadowCaster2D ? 1 : 0) << "\n";
            file << "uiType=" << static_cast<int>(obj.ui.type) << "\n";
            file << "parentId=" << obj.parentId << "\n";
            file << "position=" << obj.localPosition.x << "," << obj.localPosition.y << "," << obj.localPosition.z << "\n";
            file << "rotation=" << obj.localRotation.x << "," << obj.localRotation.y << "," << obj.localRotation.z << "\n";
            file << "scale=" << obj.localScale.x << "," << obj.localScale.y << "," << obj.localScale.z << "\n";
            file << "hasRigidbody=" << (obj.hasRigidbody ? 1 : 0) << "\n";
            if (obj.hasRigidbody) {
                file << "rbEnabled=" << (obj.rigidbody.enabled ? 1 : 0) << "\n";
                file << "rbMass=" << obj.rigidbody.mass << "\n";
                file << "rbUseCustomCenterOfMass=" << (obj.rigidbody.useCustomCenterOfMass ? 1 : 0) << "\n";
                file << "rbCenterOfMass=" << obj.rigidbody.centerOfMass.x << "," << obj.rigidbody.centerOfMass.y << "," << obj.rigidbody.centerOfMass.z << "\n";
                file << "rbUseGravity=" << (obj.rigidbody.useGravity ? 1 : 0) << "\n";
                file << "rbKinematic=" << (obj.rigidbody.isKinematic ? 1 : 0) << "\n";
                file << "rbLinearDamping=" << obj.rigidbody.linearDamping << "\n";
                file << "rbAngularDamping=" << obj.rigidbody.angularDamping << "\n";
                file << "rbInterpolation=" << std::clamp(obj.rigidbody.interpolationMode, 0, 2) << "\n";
                file << "rbMaxExtrapolation=" << std::max(0.0f, obj.rigidbody.maxExtrapolationTime) << "\n";
                file << "rbLockRotX=" << (obj.rigidbody.lockRotationX ? 1 : 0) << "\n";
                file << "rbLockRotY=" << (obj.rigidbody.lockRotationY ? 1 : 0) << "\n";
                file << "rbLockRotZ=" << (obj.rigidbody.lockRotationZ ? 1 : 0) << "\n";
            }
            file << "hasRigidbody2D=" << (obj.hasRigidbody2D ? 1 : 0) << "\n";
            if (obj.hasRigidbody2D) {
                file << "rb2dEnabled=" << (obj.rigidbody2D.enabled ? 1 : 0) << "\n";
                file << "rb2dUseGravity=" << (obj.rigidbody2D.useGravity ? 1 : 0) << "\n";
                file << "rb2dLockRotation=" << (obj.rigidbody2D.lockRotation ? 1 : 0) << "\n";
                file << "rb2dGravityScale=" << obj.rigidbody2D.gravityScale << "\n";
                file << "rb2dLinearDamping=" << obj.rigidbody2D.linearDamping << "\n";
                file << "rb2dVelocity=" << obj.rigidbody2D.velocity.x << "," << obj.rigidbody2D.velocity.y << "\n";
            }
            file << "hasCollider2D=" << (obj.hasCollider2D ? 1 : 0) << "\n";
            if (obj.hasCollider2D) {
                file << "collider2dEnabled=" << (obj.collider2D.enabled ? 1 : 0) << "\n";
                file << "collider2dType=" << static_cast<int>(obj.collider2D.type) << "\n";
                file << "collider2dBox=" << obj.collider2D.boxSize.x << "," << obj.collider2D.boxSize.y << "\n";
                file << "collider2dOffset=" << obj.collider2D.offset.x << "," << obj.collider2D.offset.y << "\n";
                file << "collider2dClosed=" << (obj.collider2D.closed ? 1 : 0) << "\n";
                file << "collider2dEdgeThickness=" << obj.collider2D.edgeThickness << "\n";
                file << "collider2dPoints=";
                for (size_t i = 0; i < obj.collider2D.points.size(); ++i) {
                    if (i > 0) file << ";";
                    file << obj.collider2D.points[i].x << "," << obj.collider2D.points[i].y;
                }
                file << "\n";
                if (obj.collider2D.type == Collider2DType::Circle) {
                    file << "collider2dRadius=" << obj.collider2D.radius << "\n";
                }
                const Collider2DComponent collider2DDefaults;
                const bool outlineTouched =
                    !obj.collider2D.outlineSourcePath.empty() ||
                    obj.collider2D.outlineAlphaThreshold != collider2DDefaults.outlineAlphaThreshold ||
                    obj.collider2D.outlineTolerance != collider2DDefaults.outlineTolerance ||
                    obj.collider2D.outlineMaxVertices != collider2DDefaults.outlineMaxVertices ||
                    obj.collider2D.outlineClosed != collider2DDefaults.outlineClosed;
                if (outlineTouched) {
                    file << "collider2dOutlineAlpha=" << obj.collider2D.outlineAlphaThreshold << "\n";
                    file << "collider2dOutlineTolerance=" << obj.collider2D.outlineTolerance << "\n";
                    file << "collider2dOutlineMaxVerts=" << obj.collider2D.outlineMaxVertices << "\n";
                    file << "collider2dOutlineClosed=" << (obj.collider2D.outlineClosed ? 1 : 0) << "\n";
                    file << "collider2dOutlineSource=" << obj.collider2D.outlineSourcePath << "\n";
                }
            }
            file << "hasParallaxLayer2D=" << (obj.hasParallaxLayer2D ? 1 : 0) << "\n";
            if (obj.hasParallaxLayer2D) {
                file << "parallax2dEnabled=" << (obj.parallaxLayer2D.enabled ? 1 : 0) << "\n";
                file << "parallax2dOrder=" << obj.parallaxLayer2D.order << "\n";
                file << "parallax2dFactor=" << obj.parallaxLayer2D.factor << "\n";
                file << "parallax2dRepeatX=" << (obj.parallaxLayer2D.repeatX ? 1 : 0) << "\n";
                file << "parallax2dRepeatY=" << (obj.parallaxLayer2D.repeatY ? 1 : 0) << "\n";
                file << "parallax2dDisableCulling=" << (obj.parallaxLayer2D.disableCulling ? 1 : 0) << "\n";
                file << "parallax2dSpacing=" << obj.parallaxLayer2D.repeatSpacing.x << "," << obj.parallaxLayer2D.repeatSpacing.y << "\n";
            }
            file << "hasCameraFollow2D=" << (obj.hasCameraFollow2D ? 1 : 0) << "\n";
            if (obj.hasCameraFollow2D) {
                file << "cameraFollow2dEnabled=" << (obj.cameraFollow2D.enabled ? 1 : 0) << "\n";
                file << "cameraFollow2dTarget=" << obj.cameraFollow2D.targetId << "\n";
                file << "cameraFollow2dOffset=" << obj.cameraFollow2D.offset.x << "," << obj.cameraFollow2D.offset.y << "\n";
                file << "cameraFollow2dSmoothTime=" << obj.cameraFollow2D.smoothTime << "\n";
            }
            file << "hasCollider=" << (obj.hasCollider ? 1 : 0) << "\n";
            if (obj.hasCollider) {
                file << "colliderEnabled=" << (obj.collider.enabled ? 1 : 0) << "\n";
                file << "colliderType=" << static_cast<int>(obj.collider.type) << "\n";
                file << "colliderBox=" << obj.collider.boxSize.x << "," << obj.collider.boxSize.y << "," << obj.collider.boxSize.z << "\n";
                file << "colliderOffset=" << obj.collider.offset.x << "," << obj.collider.offset.y << "," << obj.collider.offset.z << "\n";
                file << "colliderConvex=" << (obj.collider.convex ? 1 : 0) << "\n";
                file << "colliderIsTrigger=" << (obj.collider.isTrigger ? 1 : 0) << "\n";
                file << "colliderStaticFriction=" << obj.collider.staticFriction << "\n";
                file << "colliderDynamicFriction=" << obj.collider.dynamicFriction << "\n";
                file << "colliderRestitution=" << obj.collider.restitution << "\n";
            }
            file << "hasPlayerController=" << (obj.hasPlayerController ? 1 : 0) << "\n";
            if (obj.hasPlayerController) {
                file << "pcEnabled=" << (obj.playerController.enabled ? 1 : 0) << "\n";
                file << "pcMoveSpeed=" << obj.playerController.moveSpeed << "\n";
                file << "pcRunSpeed=" << obj.playerController.runSpeed << "\n";
                file << "pcLookSensitivity=" << obj.playerController.lookSensitivity << "\n";
                file << "pcGroundAcceleration=" << obj.playerController.groundAcceleration << "\n";
                file << "pcAirAcceleration=" << obj.playerController.airAcceleration << "\n";
                file << "pcBraking=" << obj.playerController.braking << "\n";
                file << "pcMinSurfaceControl=" << obj.playerController.minSurfaceControl << "\n";
                file << "pcSlideGravity=" << obj.playerController.slideGravity << "\n";
                file << "pcPlatformCarry=" << obj.playerController.platformCarry << "\n";
                file << "pcHeight=" << obj.playerController.height << "\n";
                file << "pcRadius=" << obj.playerController.radius << "\n";
                file << "pcJumpStrength=" << obj.playerController.jumpStrength << "\n";
                file << "pcVmEnabled=" << (obj.playerViewMotion.enabled ? 1 : 0) << "\n";
                file << "pcVmBobFrequency=" << obj.playerViewMotion.bobFrequency << "\n";
                file << "pcVmBobVertical=" << obj.playerViewMotion.bobVertical << "\n";
                file << "pcVmBobHorizontal=" << obj.playerViewMotion.bobHorizontal << "\n";
                file << "pcVmBobRoll=" << obj.playerViewMotion.bobRoll << "\n";
                file << "pcVmRunMultiplier=" << obj.playerViewMotion.runMultiplier << "\n";
                file << "pcVmLookSway=" << obj.playerViewMotion.lookSway << "\n";
                file << "pcVmLookSwayStiffness=" << obj.playerViewMotion.lookSwayStiffness << "\n";
                file << "pcVmLookSwayDamping=" << obj.playerViewMotion.lookSwayDamping << "\n";
                file << "pcVmLookSwayMax=" << obj.playerViewMotion.lookSwayMax << "\n";
                file << "pcVmStrafeRoll=" << obj.playerViewMotion.strafeRoll << "\n";
                file << "pcVmTurnRoll=" << obj.playerViewMotion.turnRoll << "\n";
                file << "pcVmRollSmoothing=" << obj.playerViewMotion.rollSmoothing << "\n";
                file << "pcVmIdleAmount=" << obj.playerViewMotion.idleAmount << "\n";
                file << "pcVmIdleFrequency=" << obj.playerViewMotion.idleFrequency << "\n";
                file << "pcVmLandingDip=" << obj.playerViewMotion.landingDip << "\n";
                file << "pcVmLandingStiffness=" << obj.playerViewMotion.landingStiffness << "\n";
                file << "pcVmLandingDamping=" << obj.playerViewMotion.landingDamping << "\n";
                file << "pcVmAttachedEnabled=" << (obj.playerViewMotion.attachedSwayEnabled ? 1 : 0) << "\n";
                file << "pcVmAttachedSway=" << obj.playerViewMotion.attachedSway << "\n";
                file << "pcVmAttachedStiffness=" << obj.playerViewMotion.attachedStiffness << "\n";
                file << "pcVmAttachedDamping=" << obj.playerViewMotion.attachedDamping << "\n";
                file << "pcVmAttachedMax=" << obj.playerViewMotion.attachedMax << "\n";
                file << "pcVmJumpCrouchDip=" << obj.playerViewMotion.jumpCrouchDip << "\n";
                file << "pcFeelLookSmoothing=" << obj.playerControlFeel.lookSmoothing << "\n";
                file << "pcFeelChargedJump=" << (obj.playerControlFeel.chargedJump ? 1 : 0) << "\n";
                file << "pcFeelJumpChargeTime=" << obj.playerControlFeel.jumpChargeTime << "\n";
                file << "pcFeelJumpMinScale=" << obj.playerControlFeel.jumpChargeMinScale << "\n";
                file << "pcVmGaitBlend=" << obj.playerViewMotion.gaitBlend << "\n";
                file << "pcVmRunLean=" << obj.playerViewMotion.runLean << "\n";
                file << "pcVmIdleBlend=" << obj.playerViewMotion.idleBlend << "\n";
                file << "pcAudioEnabled=" << (obj.playerMovementAudio.enabled ? 1 : 0) << "\n";
                file << "pcAudioJumpClip=" << obj.playerMovementAudio.jumpClip << "\n";
                file << "pcAudioLandClip=" << obj.playerMovementAudio.landClip << "\n";
                file << "pcAudioVolume=" << obj.playerMovementAudio.volume << "\n";
                file << "pcAudioRunVolumeScale=" << obj.playerMovementAudio.runVolumeScale << "\n";
                file << "pcAudioLandVolumeScale=" << obj.playerMovementAudio.landVolumeScale << "\n";
                file << "pcAudioPitchVariance=" << obj.playerMovementAudio.pitchVariance << "\n";
                file << "pcAudioStepCount=" << obj.playerMovementAudio.footstepClips.size() << "\n";
                for (size_t si = 0; si < obj.playerMovementAudio.footstepClips.size(); ++si) {
                    file << "pcAudioStep" << si << "=" << obj.playerMovementAudio.footstepClips[si] << "\n";
                }
            }
            file << "hasAudioSource=" << (obj.hasAudioSource ? 1 : 0) << "\n";
            if (obj.hasAudioSource) {
                file << "audioEnabled=" << (obj.audioSource.enabled ? 1 : 0) << "\n";
                file << "audioClip=" << obj.audioSource.clipPath << "\n";
                file << "audioVolume=" << obj.audioSource.volume << "\n";
                file << "audioLoop=" << (obj.audioSource.loop ? 1 : 0) << "\n";
                file << "audioPlayOnStart=" << (obj.audioSource.playOnStart ? 1 : 0) << "\n";
                file << "audioSpatial=" << (AudioSourceUsesSpatialization(obj.audioSource) ? 1 : 0) << "\n";
                file << "audioSpatialBlend=" << GetAudioSpatialBlend(obj.audioSource) << "\n";
                file << "audioMinDistance=" << obj.audioSource.minDistance << "\n";
                file << "audioMaxDistance=" << obj.audioSource.maxDistance << "\n";
                file << "audioRolloffMode=" << static_cast<int>(obj.audioSource.rolloffMode) << "\n";
                file << "audioRolloff=" << obj.audioSource.rolloff << "\n";
                file << "audioCustomMidDistance=" << obj.audioSource.customMidDistance << "\n";
                file << "audioCustomMidGain=" << obj.audioSource.customMidGain << "\n";
                file << "audioCustomEndGain=" << obj.audioSource.customEndGain << "\n";
            }
            file << "hasAudioFX=" << (obj.hasAudioFX ? 1 : 0) << "\n";
            if (obj.hasAudioFX) {
                file << "afxEnabled=" << (obj.audioFX.enabled ? 1 : 0) << "\n";
                file << "afxGlobal=" << (obj.audioFX.global ? 1 : 0) << "\n";
                file << "afxChain=" << AudioFXSerialize(obj.audioFX.chain) << "\n";
            }
            file << "hasVideoPlayer=" << (obj.hasVideoPlayer ? 1 : 0) << "\n";
            if (obj.hasVideoPlayer) {
                file << "videoEnabled=" << (obj.videoPlayer.enabled ? 1 : 0) << "\n";
                file << "videoPath=" << obj.videoPlayer.videoPath << "\n";
                file << "videoPlayOnAwake=" << (obj.videoPlayer.playOnAwake ? 1 : 0) << "\n";
                file << "videoLoop=" << (obj.videoPlayer.loop ? 1 : 0) << "\n";
                file << "videoFlipX=" << (obj.videoPlayer.flipX ? 1 : 0) << "\n";
                file << "videoFlipY=" << (obj.videoPlayer.flipY ? 1 : 0) << "\n";
                file << "videoPlaybackSpeed=" << obj.videoPlayer.playbackSpeed << "\n";
                file << "videoPlayAudioFromVideo=" << (obj.videoPlayer.playAudioFromVideo ? 1 : 0) << "\n";
                file << "videoRouteAudioToSource=" << (obj.videoPlayer.routeAudioToSource ? 1 : 0) << "\n";
                file << "videoOutputAudioSourceObjectId=" << obj.videoPlayer.outputAudioSourceObjectId << "\n";
                file << "videoAudioVolume=" << obj.videoPlayer.videoAudioVolume << "\n";
                file << "videoAudioMuted=" << (obj.videoPlayer.videoAudioMuted ? 1 : 0) << "\n";
                file << "videoSyncAudioToVideo=" << (obj.videoPlayer.syncAudioToVideo ? 1 : 0) << "\n";
                file << "videoAudioSyncTolerance=" << obj.videoPlayer.audioSyncTolerance << "\n";
            }
            file << "hasParticleSystem2D=" << (obj.hasParticleSystem2D ? 1 : 0) << "\n";
            if (obj.hasParticleSystem2D) {
                const auto& ps = obj.particleSystem2D;
                file << "ps2dEnabled=" << (ps.enabled ? 1 : 0) << "\n";
                file << "ps2dLooping=" << (ps.looping ? 1 : 0) << "\n";
                file << "ps2dPrewarm=" << (ps.prewarm ? 1 : 0) << "\n";
                file << "ps2dPlayOnAwake=" << (ps.playOnAwake ? 1 : 0) << "\n";
                file << "ps2dAutoRandomSeed=" << (ps.autoRandomSeed ? 1 : 0) << "\n";
                file << "ps2dRandomSeed=" << ps.randomSeed << "\n";
                file << "ps2dStartDelay=" << ps.startDelay << "\n";
                file << "ps2dStartLifetime=" << ps.startLifetime.min << "," << ps.startLifetime.max << "," << (ps.startLifetime.random ? 1 : 0) << "\n";
                file << "ps2dStartSpeed=" << ps.startSpeed.min << "," << ps.startSpeed.max << "," << (ps.startSpeed.random ? 1 : 0) << "\n";
                file << "ps2dStartSize=" << ps.startSize.min << "," << ps.startSize.max << "," << (ps.startSize.random ? 1 : 0) << "\n";
                file << "ps2dStartRotation=" << ps.startRotation.min << "," << ps.startRotation.max << "," << (ps.startRotation.random ? 1 : 0) << "\n";
                file << "ps2dStartColor=" << ps.startColor.r << "," << ps.startColor.g << "," << ps.startColor.b << "," << ps.startColor.a << "\n";
                file << "ps2dGravity=" << ps.gravityModifier << "\n";
                file << "ps2dSimulationSpeed=" << ps.simulationSpeed << "\n";
                file << "ps2dMaxParticles=" << ps.maxParticles << "\n";
                file << "ps2dEmissionRate=" << ps.emissionRate << "\n";
                file << "ps2dBurstCount=" << ps.burstCount << "\n";
                file << "ps2dBurstTime=" << ps.burstTime << "\n";
                file << "ps2dBurstLoop=" << (ps.burstLoop ? 1 : 0) << "\n";
                file << "ps2dShape=" << ps.shape << "\n";
                file << "ps2dShapeRadius=" << ps.shapeRadius << "\n";
                file << "ps2dShapeBox=" << ps.shapeBox.x << "," << ps.shapeBox.y << "," << ps.shapeBox.z << "\n";
                file << "ps2dEmitIn3D=" << (ps.emitIn3D ? 1 : 0) << "\n";
                file << "ps2dVelocityOverLifetimeEnabled=" << (ps.velocityOverLifetimeEnabled ? 1 : 0) << "\n";
                file << "ps2dVelocityOverLifetime=" << ps.velocityOverLifetime.x << "," << ps.velocityOverLifetime.y << "," << ps.velocityOverLifetime.z << "\n";
                file << "ps2dColorOverLifetimeEnabled=" << (ps.colorOverLifetimeEnabled ? 1 : 0) << "\n";
                file << "ps2dColorOverLifetime=" << ps.colorOverLifetime.r << "," << ps.colorOverLifetime.g << "," << ps.colorOverLifetime.b << "," << ps.colorOverLifetime.a << "\n";
                file << "ps2dSizeOverLifetimeEnabled=" << (ps.sizeOverLifetimeEnabled ? 1 : 0) << "\n";
                file << "ps2dSizeOverLifetime=" << ps.sizeOverLifetime << "\n";
                file << "ps2dRotationOverLifetimeEnabled=" << (ps.rotationOverLifetimeEnabled ? 1 : 0) << "\n";
                file << "ps2dRotationOverLifetime=" << ps.rotationOverLifetime << "\n";
                file << "ps2dNoiseEnabled=" << (ps.noiseEnabled ? 1 : 0) << "\n";
                file << "ps2dNoiseStrength=" << ps.noiseStrength << "\n";
                file << "ps2dNoiseFrequency=" << ps.noiseFrequency << "\n";
                file << "ps2dTexture=" << ps.texturePath << "\n";
                file << "ps2dMaterial=" << ps.materialPath << "\n";
                file << "ps2dReceiveLighting2D=" << (ps.receiveLighting2D ? 1 : 0) << "\n";
                file << "ps2dUnlitLighting2D=" << (ps.unlitLighting2D ? 1 : 0) << "\n";
                file << "ps2dEmissiveLighting2D=" << ps.emissiveLighting2D << "\n";
            }
            file << "hasReverbZone=" << (obj.hasReverbZone ? 1 : 0) << "\n";
            if (obj.hasReverbZone) {
                file << "reverbEnabled=" << (obj.reverbZone.enabled ? 1 : 0) << "\n";
                file << "reverbPreset=" << static_cast<int>(obj.reverbZone.preset) << "\n";
                file << "reverbShape=" << static_cast<int>(obj.reverbZone.shape) << "\n";
                file << "reverbBox=" << obj.reverbZone.boxSize.x << "," << obj.reverbZone.boxSize.y << "," << obj.reverbZone.boxSize.z << "\n";
                file << "reverbRadius=" << obj.reverbZone.radius << "\n";
                file << "reverbBlend=" << obj.reverbZone.blendDistance << "\n";
                file << "reverbMinDistance=" << obj.reverbZone.minDistance << "\n";
                file << "reverbMaxDistance=" << obj.reverbZone.maxDistance << "\n";
                file << "reverbRoom=" << obj.reverbZone.room << "\n";
                file << "reverbRoomHF=" << obj.reverbZone.roomHF << "\n";
                file << "reverbRoomLF=" << obj.reverbZone.roomLF << "\n";
                file << "reverbDecayTime=" << obj.reverbZone.decayTime << "\n";
                file << "reverbDecayHFRatio=" << obj.reverbZone.decayHFRatio << "\n";
                file << "reverbReflections=" << obj.reverbZone.reflections << "\n";
                file << "reverbReflectionsDelay=" << obj.reverbZone.reflectionsDelay << "\n";
                file << "reverbReverb=" << obj.reverbZone.reverb << "\n";
                file << "reverbReverbDelay=" << obj.reverbZone.reverbDelay << "\n";
                file << "reverbHFReference=" << obj.reverbZone.hfReference << "\n";
                file << "reverbLFReference=" << obj.reverbZone.lfReference << "\n";
                file << "reverbRoomRolloffFactor=" << obj.reverbZone.roomRolloffFactor << "\n";
                file << "reverbDiffusion=" << obj.reverbZone.diffusion << "\n";
                file << "reverbDensity=" << obj.reverbZone.density << "\n";
            }
            file << "hasXROrigin=" << (obj.hasXROrigin ? 1 : 0) << "\n";
            if (obj.hasXROrigin) {
                file << "xrOriginEnabled=" << (obj.xrOrigin.enabled ? 1 : 0) << "\n";
                file << "xrOriginTrackingMode=" << static_cast<int>(obj.xrOrigin.trackingOriginMode) << "\n";
                file << "xrOriginRigScale=" << obj.xrOrigin.rigScale << "\n";
                file << "xrOriginCameraYOffset=" << obj.xrOrigin.cameraYOffset << "\n";
            }
            file << "hasXRCamera=" << (obj.hasXRCamera ? 1 : 0) << "\n";
            if (obj.hasXRCamera) {
                file << "xrCameraEnabled=" << (obj.xrCamera.enabled ? 1 : 0) << "\n";
                file << "xrCameraTrackPosition=" << (obj.xrCamera.trackPosition ? 1 : 0) << "\n";
                file << "xrCameraTrackRotation=" << (obj.xrCamera.trackRotation ? 1 : 0) << "\n";
                file << "xrCameraApplyTracking=" << (obj.xrCamera.applyTracking ? 1 : 0) << "\n";
            }
            file << "hasXRController=" << (obj.hasXRController ? 1 : 0) << "\n";
            if (obj.hasXRController) {
                file << "xrControllerEnabled=" << (obj.xrController.enabled ? 1 : 0) << "\n";
                file << "xrControllerHand=" << static_cast<int>(obj.xrController.hand) << "\n";
                file << "xrControllerPoseSource=" << static_cast<int>(obj.xrController.poseSource) << "\n";
                file << "xrControllerTrackPosition=" << (obj.xrController.trackPosition ? 1 : 0) << "\n";
                file << "xrControllerTrackRotation=" << (obj.xrController.trackRotation ? 1 : 0) << "\n";
                file << "xrControllerHideWhenNotTracked=" << (obj.xrController.hideWhenNotTracked ? 1 : 0) << "\n";
            }
            file << "hasXRActionBasedController=" << (obj.hasXRActionBasedController ? 1 : 0) << "\n";
            if (obj.hasXRActionBasedController) {
                const XRActionBasedControllerComponent& c = obj.xrActionBasedController;
                file << "xrActionControllerEnabled=" << (c.enabled ? 1 : 0) << "\n";
                file << "xrActionControllerHand=" << static_cast<int>(c.hand) << "\n";
                file << "xrActionControllerSelect=" << c.selectButton << "\n";
                file << "xrActionControllerActivate=" << c.activateButton << "\n";
                file << "xrActionControllerUiPress=" << c.uiPressButton << "\n";
                file << "xrActionControllerHaptics=" << (c.enableHaptics ? 1 : 0) << "\n";
                file << "xrActionControllerHapticAmplitude=" << c.hapticAmplitude << "\n";
                file << "xrActionControllerHapticDuration=" << c.hapticDuration << "\n";
            }
            file << "hasXRRayInteractor=" << (obj.hasXRRayInteractor ? 1 : 0) << "\n";
            if (obj.hasXRRayInteractor) {
                const XRRayInteractorComponent& r = obj.xrRayInteractor;
                file << "xrRayEnabled=" << (r.enabled ? 1 : 0) << "\n";
                file << "xrRayType=" << static_cast<int>(r.rayType) << "\n";
                file << "xrRayMaxDistance=" << r.maxDistance << "\n";
                file << "xrRayInteractionMask=" << r.interactionMask << "\n";
                file << "xrRayUiInteraction=" << (r.uiInteraction ? 1 : 0) << "\n";
                file << "xrRayLineVisual=" << (r.showLineVisual ? 1 : 0) << "\n";
                file << "xrRayLineColor=" << r.lineColor.r << "," << r.lineColor.g << ","
                     << r.lineColor.b << "," << r.lineColor.a << "\n";
            }
            file << "hasXRDirectInteractor=" << (obj.hasXRDirectInteractor ? 1 : 0) << "\n";
            if (obj.hasXRDirectInteractor) {
                file << "xrDirectEnabled=" << (obj.xrDirectInteractor.enabled ? 1 : 0) << "\n";
                file << "xrDirectRadius=" << obj.xrDirectInteractor.interactionRadius << "\n";
                file << "xrDirectInteractionMask=" << obj.xrDirectInteractor.interactionMask << "\n";
            }
            file << "hasXRGrabInteractable=" << (obj.hasXRGrabInteractable ? 1 : 0) << "\n";
            if (obj.hasXRGrabInteractable) {
                const XRGrabInteractableComponent& g = obj.xrGrabInteractable;
                file << "xrGrabEnabled=" << (g.enabled ? 1 : 0) << "\n";
                file << "xrGrabMovementType=" << static_cast<int>(g.movementType) << "\n";
                file << "xrGrabAllowLeft=" << (g.allowLeftHand ? 1 : 0) << "\n";
                file << "xrGrabAllowRight=" << (g.allowRightHand ? 1 : 0) << "\n";
                file << "xrGrabAttachTransformId=" << g.attachTransformId << "\n";
                file << "xrGrabTrackPosition=" << (g.trackPosition ? 1 : 0) << "\n";
                file << "xrGrabTrackRotation=" << (g.trackRotation ? 1 : 0) << "\n";
                file << "xrGrabThrowOnDetach=" << (g.throwOnDetach ? 1 : 0) << "\n";
                file << "xrGrabThrowVelocityScale=" << g.throwVelocityScale << "\n";
                file << "xrGrabThrowAngularScale=" << g.throwAngularVelocityScale << "\n";
            }
            file << "hasGroundBakedType=" << (obj.hasGroundBakedType ? 1 : 0) << "\n";
            if (obj.hasGroundBakedType) {
                file << "groundBakedEnabled=" << (obj.groundBakedType.enabled ? 1 : 0) << "\n";
                file << "groundBakedInclude=" << (obj.groundBakedType.includeInBake ? 1 : 0) << "\n";
                file << "groundBakedAreaCost=" << obj.groundBakedType.areaCost << "\n";
            }
            file << "hasObsticleObject=" << (obj.hasObsticleObject ? 1 : 0) << "\n";
            if (obj.hasObsticleObject) {
                file << "obsticleEnabled=" << (obj.obsticleObject.enabled ? 1 : 0) << "\n";
                file << "obsticleCarve=" << (obj.obsticleObject.carve ? 1 : 0) << "\n";
                file << "obsticlePadding=" << obj.obsticleObject.padding << "\n";
            }
            file << "hasAIAgent=" << (obj.hasAIAgent ? 1 : 0) << "\n";
            if (obj.hasAIAgent) {
                file << "aiAgentEnabled=" << (obj.aiAgent.enabled ? 1 : 0) << "\n";
                file << "aiAgentUseTargetObject=" << (obj.aiAgent.useTargetObject ? 1 : 0) << "\n";
                file << "aiAgentTargetId=" << obj.aiAgent.targetId << "\n";
                file << "aiAgentDestination=" << obj.aiAgent.destination.x << "," << obj.aiAgent.destination.y << "," << obj.aiAgent.destination.z << "\n";
                file << "aiAgentSpeed=" << obj.aiAgent.speed << "\n";
                file << "aiAgentStoppingDistance=" << obj.aiAgent.stoppingDistance << "\n";
                file << "aiAgentRepathInterval=" << obj.aiAgent.repathInterval << "\n";
                file << "aiAgentAutoRepath=" << (obj.aiAgent.autoRepath ? 1 : 0) << "\n";
                file << "aiAgentAlignToPath=" << (obj.aiAgent.alignToPath ? 1 : 0) << "\n";
                file << "aiAgentDebugDrawPath=" << (obj.aiAgent.debugDrawPath ? 1 : 0) << "\n";
                file << "aiAgentTurnSpeed=" << obj.aiAgent.turnSpeed << "\n";
                file << "aiAgentAvoidancePadding=" << obj.aiAgent.avoidancePadding << "\n";
            }
            file << "hasOffMeshLink=" << (obj.hasOffMeshLink ? 1 : 0) << "\n";
            if (obj.hasOffMeshLink) {
                file << "offMeshLinkEnabled=" << (obj.offMeshLink.enabled ? 1 : 0) << "\n";
                file << "offMeshLinkStart=" << obj.offMeshLink.startPoint.x << "," << obj.offMeshLink.startPoint.y << "," << obj.offMeshLink.startPoint.z << "\n";
                file << "offMeshLinkEnd=" << obj.offMeshLink.endPoint.x << "," << obj.offMeshLink.endPoint.y << "," << obj.offMeshLink.endPoint.z << "\n";
                file << "offMeshLinkBidirectional=" << (obj.offMeshLink.bidirectional ? 1 : 0) << "\n";
                file << "offMeshLinkCostOverride=" << obj.offMeshLink.costOverride << "\n";
            }
            file << "hasRig25DRoot=" << (obj.hasRig25DRoot ? 1 : 0) << "\n";
            if (obj.hasRig25DRoot) {
                file << "rig25dRootEnabled=" << (obj.rig25DRoot.enabled ? 1 : 0) << "\n";
            }
            file << "hasRig25DNode=" << (obj.hasRig25DNode ? 1 : 0) << "\n";
            if (obj.hasRig25DNode) {
                file << "rig25dNodeEnabled=" << (obj.rig25DNode.enabled ? 1 : 0) << "\n";
                file << "rig25dNodeId=" << obj.rig25DNode.nodeId << "\n";
                file << "rig25dNodeName=" << obj.rig25DNode.nodeName << "\n";
            }
            auto mapSingleLine = [](std::string value) {
                for (char& c : value) {
                    if (c == '\n' || c == '\r') c = ' ';
                }
                return value;
            };
            file << "hasMapRoot=" << (obj.hasMapRoot ? 1 : 0) << "\n";
            if (obj.hasMapRoot) {
                file << "mapRootEnabled=" << (obj.mapRoot.enabled ? 1 : 0) << "\n";
                file << "mapRootId=" << obj.mapRoot.mapId << "\n";
                file << "mapRootStartSector=" << obj.mapRoot.startSectorId << "\n";
                file << "mapRootActiveSector=" << obj.mapRoot.activeSectorId << "\n";
                file << "mapRootVisibilityMode=" << obj.mapRoot.sectorVisibilityMode << "\n";
                file << "mapRootNotes=" << mapSingleLine(obj.mapRoot.notes) << "\n";
            }
            file << "hasMapSector=" << (obj.hasMapSector ? 1 : 0) << "\n";
            if (obj.hasMapSector) {
                file << "mapSectorEnabled=" << (obj.mapSector.enabled ? 1 : 0) << "\n";
                file << "mapSectorId=" << obj.mapSector.sectorId << "\n";
                file << "mapSectorGraphPos=" << obj.mapSector.graphPosition.x << "," << obj.mapSector.graphPosition.y << "\n";
                file << "mapSectorColor=" << obj.mapSector.color.x << "," << obj.mapSector.color.y << "," << obj.mapSector.color.z << "\n";
                file << "mapSectorUseCustomBounds=" << (obj.mapSector.useCustomBounds ? 1 : 0) << "\n";
                file << "mapSectorBoundsCenter=" << obj.mapSector.boundsCenter.x << "," << obj.mapSector.boundsCenter.y << "," << obj.mapSector.boundsCenter.z << "\n";
                file << "mapSectorBoundsSize=" << obj.mapSector.boundsSize.x << "," << obj.mapSector.boundsSize.y << "," << obj.mapSector.boundsSize.z << "\n";
                file << "mapSectorStreamingTag=" << mapSingleLine(obj.mapSector.streamingTag) << "\n";
                file << "mapSectorNotes=" << mapSingleLine(obj.mapSector.notes) << "\n";
                file << "mapSectorEstimatedMemoryMB=" << obj.mapSector.estimatedMemoryMB << "\n";
            }
            file << "hasMapTransition=" << (obj.hasMapTransition ? 1 : 0) << "\n";
            if (obj.hasMapTransition) {
                file << "mapTransitionEnabled=" << (obj.mapTransition.enabled ? 1 : 0) << "\n";
                file << "mapTransitionId=" << obj.mapTransition.transitionId << "\n";
                file << "mapTransitionSourceSector=" << obj.mapTransition.sourceSectorId << "\n";
                file << "mapTransitionDestSector=" << obj.mapTransition.destinationSectorId << "\n";
                file << "mapTransitionSourcePortal=" << obj.mapTransition.sourcePortalId << "\n";
                file << "mapTransitionDestPortal=" << obj.mapTransition.destinationPortalId << "\n";
                file << "mapTransitionBidirectional=" << (obj.mapTransition.bidirectional ? 1 : 0) << "\n";
                file << "mapTransitionLocked=" << (obj.mapTransition.locked ? 1 : 0) << "\n";
                file << "mapTransitionCondition=" << mapSingleLine(obj.mapTransition.condition) << "\n";
                file << "mapTransitionKind=" << static_cast<int>(obj.mapTransition.kind) << "\n";
                file << "mapTransitionEditorLabel=" << mapSingleLine(obj.mapTransition.editorLabel) << "\n";
                file << "mapTransitionHasEntry=" << (obj.mapTransition.hasEntryTransform ? 1 : 0) << "\n";
                file << "mapTransitionEntryPos=" << obj.mapTransition.entryPosition.x << "," << obj.mapTransition.entryPosition.y << "," << obj.mapTransition.entryPosition.z << "\n";
                file << "mapTransitionEntryYaw=" << obj.mapTransition.entryYawDeg << "\n";
            }
            file << "hasMapPortal=" << (obj.hasMapPortal ? 1 : 0) << "\n";
            if (obj.hasMapPortal) {
                file << "mapPortalEnabled=" << (obj.mapPortal.enabled ? 1 : 0) << "\n";
                file << "mapPortalId=" << obj.mapPortal.portalId << "\n";
                file << "mapPortalTransition=" << obj.mapPortal.transitionId << "\n";
                file << "mapPortalSector=" << obj.mapPortal.sectorId << "\n";
                file << "mapPortalOpeningSize=" << obj.mapPortal.openingSize.x << "," << obj.mapPortal.openingSize.y << "\n";
            }
            file << "hasMapMesh=" << (obj.hasMapMesh ? 1 : 0) << "\n";
            if (obj.hasMapMesh) {
                file << "mapMeshEnabled=" << (obj.mapMesh.enabled ? 1 : 0) << "\n";
                file << "mapMeshGridSize=" << obj.mapMesh.gridSize << "\n";
                file << "mapMeshSnapToGrid=" << (obj.mapMesh.snapToGrid ? 1 : 0) << "\n";
                file << "mapMeshVertexSnapping=" << (obj.mapMesh.vertexSnapping ? 1 : 0) << "\n";
                file << "mapMeshSurfaceSnapping=" << (obj.mapMesh.surfaceSnapping ? 1 : 0) << "\n";
                file << "mapMeshAutoCollision=" << (obj.mapMesh.autoCollision ? 1 : 0) << "\n";
            }
            file << "hasAnimation=" << (obj.hasAnimation ? 1 : 0) << "\n";
            if (obj.hasAnimation) {
                AnimationComponent animation = obj.animation;
                NormalizeAnimationClipSlots(animation);
                file << "animEnabled=" << (animation.enabled ? 1 : 0) << "\n";
                file << "animClipAsset=" << animation.clipAssetPath << "\n";
                file << "animClipCount=" << animation.clips.size() << "\n";
                file << "animActiveClipIndex=" << animation.activeClipIndex << "\n";
                for (size_t ci = 0; ci < animation.clips.size(); ++ci) {
                    const auto& clip = animation.clips[ci];
                    file << "animClip" << ci << "_name=" << clip.name << "\n";
                    file << "animClip" << ci << "_asset=" << clip.assetPath << "\n";
                }
                file << "animClipLength=" << animation.clipLength << "\n";
                file << "animPlaySpeed=" << animation.playSpeed << "\n";
                file << "animLoop=" << (animation.loop ? 1 : 0) << "\n";
                file << "animPlayOnAwake=" << (animation.playOnAwake ? 1 : 0) << "\n";
                file << "animApplyOnScrub=" << (animation.applyOnScrub ? 1 : 0) << "\n";
                file << "animKeyCount=" << animation.keyframes.size() << "\n";
                for (size_t ki = 0; ki < animation.keyframes.size(); ++ki) {
                    const auto& key = animation.keyframes[ki];
                    file << "animKey" << ki << "_time=" << key.time << "\n";
                    file << "animKey" << ki << "_pos=" << key.position.x << "," << key.position.y << "," << key.position.z << "\n";
                    file << "animKey" << ki << "_rot=" << key.rotation.x << "," << key.rotation.y << "," << key.rotation.z << "\n";
                    file << "animKey" << ki << "_scale=" << key.scale.x << "," << key.scale.y << "," << key.scale.z << "\n";
                    file << "animKey" << ki << "_interp=" << static_cast<int>(key.interpolation) << "\n";
                    file << "animKey" << ki << "_curve=" << static_cast<int>(key.curveMode) << "\n";
                    file << "animKey" << ki << "_in=" << key.bezierIn.x << "," << key.bezierIn.y << "\n";
                    file << "animKey" << ki << "_out=" << key.bezierOut.x << "," << key.bezierOut.y << "\n";
                }
                file << "animEventCount=" << animation.events.size() << "\n";
                for (size_t ei = 0; ei < animation.events.size(); ++ei) {
                    const auto& evt = animation.events[ei];
                    file << "animEvent" << ei << "_time=" << evt.time << "\n";
                    file << "animEvent" << ei << "_id=" << evt.eventId << "\n";
                    file << "animEvent" << ei << "_payload=" << evt.payload << "\n";
                }
                file << "animTrackCount=" << animation.tracks.size() << "\n";
                for (size_t ti = 0; ti < animation.tracks.size(); ++ti) {
                    const auto& track = animation.tracks[ti];
                    file << "animTrack" << ti << "_enabled=" << (track.enabled ? 1 : 0) << "\n";
                    file << "animTrack" << ti << "_path=" << track.path << "\n";
                    file << "animTrack" << ti << "_label=" << track.label << "\n";
                    file << "animTrack" << ti << "_default=" << track.defaultValue << "\n";
                    file << "animTrack" << ti << "_keyCount=" << track.keyframes.size() << "\n";
                    for (size_t ki = 0; ki < track.keyframes.size(); ++ki) {
                        const auto& key = track.keyframes[ki];
                        file << "animTrack" << ti << "_key" << ki << "_time=" << key.time << "\n";
                        file << "animTrack" << ti << "_key" << ki << "_value=" << key.value << "\n";
                        file << "animTrack" << ti << "_key" << ki << "_interp=" << static_cast<int>(key.interpolation) << "\n";
                        file << "animTrack" << ti << "_key" << ki << "_curve=" << static_cast<int>(key.curveMode) << "\n";
                        file << "animTrack" << ti << "_key" << ki << "_in=" << key.bezierIn.x << "," << key.bezierIn.y << "\n";
                        file << "animTrack" << ti << "_key" << ki << "_out=" << key.bezierOut.x << "," << key.bezierOut.y << "\n";
                    }
                }
            }
            file << "hasSkeletalAnimation=" << (obj.hasSkeletalAnimation ? 1 : 0) << "\n";
            if (obj.hasSkeletalAnimation) {
                file << "skelEnabled=" << (obj.skeletal.enabled ? 1 : 0) << "\n";
                file << "skelUseGpu=" << (obj.skeletal.useGpuSkinning ? 1 : 0) << "\n";
                file << "skelAllowCpuFallback=" << (obj.skeletal.allowCpuFallback ? 1 : 0) << "\n";
                file << "skelUseAnimation=" << (obj.skeletal.useAnimation ? 1 : 0) << "\n";
                file << "skelClipIndex=" << obj.skeletal.clipIndex << "\n";
                file << "skelPlaySpeed=" << obj.skeletal.playSpeed << "\n";
                file << "skelLoop=" << (obj.skeletal.loop ? 1 : 0) << "\n";
                file << "skelMaxBones=" << obj.skeletal.maxBones << "\n";
            }
            file << "materialColor=" << obj.material.color.r << "," << obj.material.color.g << "," << obj.material.color.b << "\n";
            file << "materialAlpha=" << obj.material.alpha << "\n";
            file << "materialAmbient=" << obj.material.ambientStrength << "\n";
            file << "materialSpecular=" << obj.material.specularStrength << "\n";
            file << "materialShininess=" << obj.material.shininess << "\n";
            file << "materialNormalMapIntensity=" << obj.material.normalMapIntensity << "\n";
            file << "materialTextureMix=" << obj.material.textureMix << "\n";
            file << "materialUvTiling=" << obj.material.uvTiling.x << "," << obj.material.uvTiling.y << "\n";
            file << "materialUvOffset=" << obj.material.uvOffset.x << "," << obj.material.uvOffset.y << "\n";
            file << "materialScrollSpeed=" << obj.material.scrollSpeed << "\n";
            file << "materialScrollDirection=" << obj.material.scrollDirection.x << "," << obj.material.scrollDirection.y << "\n";
            file << "materialUvScrollEnabled=" << (obj.material.uvScrollEnabled ? 1 : 0) << "\n";
            file << "materialCloudColor=" << obj.material.cloudColor.r << "," << obj.material.cloudColor.g << "," << obj.material.cloudColor.b << "\n";
            file << "materialCloudSkyColor=" << obj.material.cloudSkyColor.r << "," << obj.material.cloudSkyColor.g << "," << obj.material.cloudSkyColor.b << "\n";
            file << "materialCloudScale=" << obj.material.cloudScale << "\n";
            file << "materialCloudCoverage=" << obj.material.cloudCoverage << "\n";
            file << "materialCloudSoftness=" << obj.material.cloudSoftness << "\n";
            file << "materialCloudDetail=" << obj.material.cloudDetail << "\n";
            file << "materialCloudSpeed=" << obj.material.cloudSpeed << "\n";
            file << "materialCloudWarp=" << obj.material.cloudWarp << "\n";
            file << "materialCloudHighlight=" << obj.material.cloudHighlight << "\n";
            file << "materialCloudStars=" << obj.material.cloudStars << "\n";
            file << "materialCloudHorizon=" << obj.material.cloudHorizon << "\n";
            file << "materialTextureFilter=" << static_cast<int>(obj.material.textureFilter) << "\n";
            file << "materialPbrEnabled=" << (obj.material.pbrEnabled ? 1 : 0) << "\n";
            file << "materialMetallic=" << obj.material.metallic << "\n";
            file << "materialRoughness=" << obj.material.roughness << "\n";
            file << "materialOcclusionStrength=" << obj.material.occlusionStrength << "\n";
            file << "materialEmissionColor=" << obj.material.emissionColor.r << "," << obj.material.emissionColor.g << "," << obj.material.emissionColor.b << "\n";
            file << "materialEmissionStrength=" << obj.material.emissionStrength << "\n";
            file << "materialAlphaCutoff=" << obj.material.alphaCutoff << "\n";
            file << "materialHeightScale=" << obj.material.heightScale << "\n";
            file << "materialParallaxSteps=" << obj.material.parallaxSteps << "\n";
            file << "materialParallaxInvertHeight=" << (obj.material.parallaxInvertHeight ? 1 : 0) << "\n";
            file << "materialPath=" << obj.materialPath << "\n";
            file << "albedoTex=" << obj.albedoTexturePath << "\n";
            file << "overlayTex=" << obj.overlayTexturePath << "\n";
            file << "normalMap=" << obj.normalMapPath << "\n";
            file << "metallicRoughnessMap=" << obj.metallicRoughnessMapPath << "\n";
            file << "metallicMap=" << obj.metallicMapPath << "\n";
            file << "roughnessMap=" << obj.roughnessMapPath << "\n";
            file << "opacityMap=" << obj.opacityMapPath << "\n";
            file << "emissiveMap=" << obj.emissiveMapPath << "\n";
            file << "occlusionMap=" << obj.occlusionMapPath << "\n";
            file << "heightMap=" << obj.heightMapPath << "\n";
            file << "shaderPack=" << obj.shaderPackPath << "\n";
            file << "vertexShader=" << obj.vertexShaderPath << "\n";
            file << "fragmentShader=" << obj.fragmentShaderPath << "\n";
            file << "useOverlay=" << (obj.useOverlay ? 1 : 0) << "\n";
            file << "additionalMaterialCount=" << obj.additionalMaterialPaths.size() << "\n";
            for (size_t mi = 0; mi < obj.additionalMaterialPaths.size(); ++mi) {
                file << "additionalMaterial" << mi << "=" << obj.additionalMaterialPaths[mi] << "\n";
            }
            file << "nextInspectorScriptId=" << obj.nextInspectorScriptId << "\n";
            file << "componentOrder=";
            for (size_t oi = 0; oi < obj.inspectorComponentOrder.size(); ++oi) {
                if (oi > 0) file << ";";
                file << obj.inspectorComponentOrder[oi];
            }
            file << "\n";
            file << "scripts=" << obj.scripts.size() << "\n";
            for (size_t si = 0; si < obj.scripts.size(); ++si) {
                const auto& sc = obj.scripts[si];
                file << "script" << si << "_id=" << sc.inspectorId << "\n";
                file << "script" << si << "_path=" << sc.path << "\n";
                file << "script" << si << "_lang=" << static_cast<int>(sc.language) << "\n";
                file << "script" << si << "_type=" << sc.managedType << "\n";
                file << "script" << si << "_enabled=" << (sc.enabled ? 1 : 0) << "\n";
                file << "script" << si << "_settings=" << sc.settings.size() << "\n";
                for (size_t k = 0; k < sc.settings.size(); ++k) {
                    file << "script" << si << "_setting" << k << "=" << sc.settings[k].key << ":" << sc.settings[k].value << "\n";
                }
            }
            file << "lightColor=" << obj.light.color.r << "," << obj.light.color.g << "," << obj.light.color.b << "\n";
            if (obj.hasLight) {
                file << "lightType=" << static_cast<int>(obj.light.type) << "\n";
            }
            file << "lightIntensity=" << obj.light.intensity << "\n";
            file << "lightRange=" << obj.light.range << "\n";
            file << "lightEdgeFade=" << obj.light.edgeFade << "\n";
            file << "lightInner=" << obj.light.innerAngle << "\n";
            file << "lightOuter=" << obj.light.outerAngle << "\n";
            file << "lightSize=" << obj.light.size.x << "," << obj.light.size.y << "\n";
            file << "lightCastShadows=" << (obj.light.castShadows ? 1 : 0) << "\n";
            file << "lightSoftShadows=" << (obj.light.softShadows ? 1 : 0) << "\n";
            file << "lightShadowBias=" << obj.light.shadowBias << "\n";
            file << "lightShadowSoftness=" << obj.light.shadowSoftness << "\n";
            file << "lightShadowResolution=" << obj.light.shadowResolution << "\n";
            file << "lightEnabled=" << (obj.light.enabled ? 1 : 0) << "\n";
            if (obj.hasReflectionCast) {
                file << "reflectionCastEnabled=" << (obj.reflectionCast.enabled ? 1 : 0) << "\n";
                file << "reflectionCastUpdateMode=" << static_cast<int>(obj.reflectionCast.updateMode) << "\n";
                file << "reflectionCastBox=" << obj.reflectionCast.boxSize.x << "," << obj.reflectionCast.boxSize.y << "," << obj.reflectionCast.boxSize.z << "\n";
                file << "reflectionCastBlend=" << obj.reflectionCast.blendDistance << "\n";
                file << "reflectionCastIntensity=" << obj.reflectionCast.intensity << "\n";
                file << "reflectionCastResolution=" << obj.reflectionCast.resolution << "\n";
            }
            if (obj.hasLight2D) {
                file << "light2dEnabled=" << (obj.light2D.enabled ? 1 : 0) << "\n";
                file << "light2dType=" << static_cast<int>(obj.light2D.type) << "\n";
                file << "light2dColor="
                     << obj.light2D.color.r << ","
                     << obj.light2D.color.g << ","
                     << obj.light2D.color.b << ","
                     << obj.light2D.color.a << "\n";
                file << "light2dIntensity=" << obj.light2D.intensity << "\n";
                file << "light2dRadius=" << obj.light2D.radius << "\n";
                file << "light2dInnerRadius=" << obj.light2D.innerRadius << "\n";
                file << "light2dOuterRadius=" << obj.light2D.outerRadius << "\n";
                file << "light2dFalloffStrength=" << obj.light2D.falloffStrength << "\n";
                file << "light2dInnerSpotAngle=" << obj.light2D.innerSpotAngle << "\n";
                file << "light2dOuterSpotAngle=" << obj.light2D.outerSpotAngle << "\n";
                file << "light2dBlendStyle=" << obj.light2D.blendStyle << "\n";
                file << "light2dOrder=" << obj.light2D.lightOrder << "\n";
                file << "light2dOverlap=" << static_cast<int>(obj.light2D.overlapOperation) << "\n";
                file << "light2dShadowStrength=" << obj.light2D.shadowStrength << "\n";
                file << "light2dVolumetric=" << (obj.light2D.volumetricEnabled ? 1 : 0) << "\n";
                file << "light2dCastShadows=" << (obj.light2D.castsShadows ? 1 : 0) << "\n";
                file << "light2dTargetAllLayers=" << (obj.light2D.targetAllLayers ? 1 : 0) << "\n";
                file << "light2dTargetLayerMask=" << obj.light2D.targetLayerMask << "\n";
                file << "light2dCullOffscreen=" << (obj.light2D.cullWhenOffscreen ? 1 : 0) << "\n";
                file << "light2dOffscreenMargin=" << obj.light2D.offscreenCullMargin << "\n";
                file << "light2dNormalQuality=" << static_cast<int>(obj.light2D.normalMapQuality) << "\n";
                file << "light2dNormalDistance=" << obj.light2D.normalMapDistance << "\n";
                file << "light2dUseDistanceExponent=" << (obj.light2D.useDistanceExponent ? 1 : 0) << "\n";
                file << "light2dDistanceExponent=" << obj.light2D.distanceExponent << "\n";
                file << "light2dCookie=" << obj.light2D.cookieTexturePath << "\n";
                file << "light2dCookieScale=" << obj.light2D.cookieScale.x << "," << obj.light2D.cookieScale.y << "\n";
                file << "light2dCookieRotation=" << obj.light2D.cookieRotation << "\n";
                file << "light2dFreeformFeather=" << obj.light2D.freeformFeather << "\n";
                file << "light2dFreeformEdgeFalloff=" << obj.light2D.freeformEdgeFalloff << "\n";
                file << "light2dFlickerEnabled=" << (obj.light2D.flicker.enabled ? 1 : 0) << "\n";
                file << "light2dFlickerSpeed=" << obj.light2D.flicker.speed << "\n";
                file << "light2dFlickerAmount=" << obj.light2D.flicker.amount << "\n";
                file << "light2dFlickerSeed=" << obj.light2D.flicker.seed << "\n";
                if (!obj.light2D.shapePoints.empty()) {
                    file << "light2dShape=";
                    for (size_t i = 0; i < obj.light2D.shapePoints.size(); ++i) {
                        if (i > 0) file << ";";
                        file << obj.light2D.shapePoints[i].x << "," << obj.light2D.shapePoints[i].y;
                    }
                    file << "\n";
                }
            }
            if (obj.hasShadowCaster2D) {
                file << "shadowCaster2dEnabled=" << (obj.shadowCaster2D.enabled ? 1 : 0) << "\n";
                file << "shadowCaster2dSelfShadow=" << (obj.shadowCaster2D.castsSelfShadow ? 1 : 0) << "\n";
                file << "shadowCaster2dTargetAllLayers=" << (obj.shadowCaster2D.targetAllLayers ? 1 : 0) << "\n";
                file << "shadowCaster2dTargetLayerMask=" << obj.shadowCaster2D.targetLayerMask << "\n";
                file << "shadowCaster2dStrength=" << obj.shadowCaster2D.shadowStrength << "\n";
                if (!obj.shadowCaster2D.points.empty()) {
                    file << "shadowCaster2dShape=";
                    for (size_t i = 0; i < obj.shadowCaster2D.points.size(); ++i) {
                        if (i > 0) file << ";";
                        file << obj.shadowCaster2D.points[i].x << "," << obj.shadowCaster2D.points[i].y;
                    }
                    file << "\n";
                }
            }
            if (obj.hasModuObjInstance) {
                file << "moduObjInstance=1\n";
                file << "moduObjVersion=1\n";
                file << "moduObjAssetId=" << obj.moduObjInstance.assetId << "\n";
                file << "moduObjInstanceId=" << obj.moduObjInstance.instanceId << "\n";
                file << "moduObjSourceLocalId=" << obj.moduObjInstance.sourceLocalId << "\n";
                file << "moduObjIsRoot=" << (obj.moduObjInstance.isRoot ? 1 : 0) << "\n";
                file << "moduObjSourceMissing=" << (obj.moduObjInstance.sourceMissing ? 1 : 0) << "\n";
                file << "moduObjLastKnownPath=" << obj.moduObjInstance.lastKnownPath << "\n";
            }
            if (obj.hasNetworkIdentity) {
                const NetworkIdentityComponent& net = obj.networkIdentity;
                file << "netIdentity=1\n";
                file << "netIdentityVersion=1\n";
                file << "netIdentityEnabled=" << (net.enabled ? 1 : 0) << "\n";
                file << "netIdentityAssetId=" << net.assetId << "\n";
                file << "netIdentitySyncPosition=" << (net.syncPosition ? 1 : 0) << "\n";
                file << "netIdentitySyncRotation=" << (net.syncRotation ? 1 : 0) << "\n";
                file << "netIdentitySyncScale=" << (net.syncScale ? 1 : 0) << "\n";
                file << "netIdentitySyncVelocity=" << (net.syncVelocity ? 1 : 0) << "\n";
                file << "netIdentitySyncMode=" << net.syncMode << "\n";
                file << "netIdentitySendRate=" << net.sendRateHz << "\n";
                file << "netIdentityInterpDelay=" << net.interpolationDelay << "\n";
                file << "netIdentityMaxExtrapolation=" << net.maxExtrapolation << "\n";
                file << "netIdentityOwnerOnlyWrites=" << (net.ownerOnlyWrites ? 1 : 0) << "\n";
                file << "netIdentitySpawnerOwns=" << (net.spawnerOwns ? 1 : 0) << "\n";
            }
            if (obj.hasNetworkManager) {
                const NetworkManagerComponent& mgr = obj.networkManager;
                file << "netManager=1\n";
                file << "netManagerVersion=1\n";
                file << "netManagerEnabled=" << (mgr.enabled ? 1 : 0) << "\n";
                file << "netManagerAppId=" << mgr.appId << "\n";
                file << "netManagerAppVersion=" << mgr.appVersion << "\n";
                file << "netManagerRegion=" << mgr.region << "\n";
                file << "netManagerNickname=" << mgr.nickname << "\n";
                file << "netManagerAutoConnect=" << (mgr.autoConnect ? 1 : 0) << "\n";
                file << "netManagerOfflineMode=" << (mgr.offlineMode ? 1 : 0) << "\n";
                file << "netManagerMaxPlayers=" << mgr.maxPlayers << "\n";
                file << "netManagerAutoJoinLobby=" << (mgr.autoJoinLobby ? 1 : 0) << "\n";
                file << "netManagerDefaultRoom=" << mgr.defaultRoomName << "\n";
                file << "netManagerSendRate=" << mgr.sendRateHz << "\n";
                file << "netManagerSerializationRate=" << mgr.serializationRateHz << "\n";
                file << "netManagerMaxOutBps=" << mgr.maxOutboundBytesPerSecond << "\n";
            }
            if (obj.hasAssemblage) {
                const AssemblageComponent& assemblage = obj.assemblage;
                file << "assemblage=1\n";
                file << "assemblageVersion=1\n";
                file << "assemblageEnabled=" << (assemblage.enabled ? 1 : 0) << "\n";
                file << "assemblageId=" << assemblage.assemblageId << "\n";
                file << "assemblageAsset=" << assemblage.assetPath << "\n";
                file << "assemblageShowGrid=" << (assemblage.showGrid ? 1 : 0) << "\n";
                file << "assemblageSnapToGrid=" << (assemblage.snapToGrid ? 1 : 0) << "\n";
                file << "assemblageGridColor=" << assemblage.gridColor.r << "," << assemblage.gridColor.g
                     << "," << assemblage.gridColor.b << "," << assemblage.gridColor.a << "\n";
            }
            if (obj.hasAssemblageLayer) {
                const AssemblageLayerComponent& assemblageLayer = obj.assemblageLayer;
                file << "assemblageLayer=1\n";
                file << "assemblageLayerVersion=1\n";
                file << "assemblageLayerEnabled=" << (assemblageLayer.enabled ? 1 : 0) << "\n";
                file << "assemblageLayerId=" << assemblageLayer.layerId << "\n";
                file << "assemblageLayerSorting=" << assemblageLayer.sortingOrder << "\n";
                file << "assemblageLayerLocked=" << (assemblageLayer.locked ? 1 : 0) << "\n";
                file << "assemblageLayerOpacity=" << assemblageLayer.opacity << "\n";
                file << "assemblageLayerTint=" << assemblageLayer.tint.r << "," << assemblageLayer.tint.g
                     << "," << assemblageLayer.tint.b << "," << assemblageLayer.tint.a << "\n";
                file << "assemblageLayerReceiveLighting=" << (assemblageLayer.receiveLighting2D ? 1 : 0) << "\n";
                file << "assemblageLayerUnlit=" << (assemblageLayer.unlitLighting2D ? 1 : 0) << "\n";
                file << "assemblageLayerEmissive=" << assemblageLayer.emissiveLighting2D << "\n";
                file << "assemblageLayerCollision=" << (assemblageLayer.collisionEnabled ? 1 : 0) << "\n";
            }
            file << "cameraType=" << static_cast<int>(obj.camera.type) << "\n";
            file << "cameraFov=" << obj.camera.fov << "\n";
            file << "cameraNear=" << obj.camera.nearClip << "\n";
            file << "cameraFar=" << obj.camera.farClip << "\n";
            file << "cameraPostFX=" << (obj.camera.applyPostFX ? 1 : 0) << "\n";
            file << "cameraUse2D=" << (obj.camera.use2D ? 1 : 0) << "\n";
            file << "cameraPixelsPerUnit=" << obj.camera.pixelsPerUnit << "\n";
            file << "cameraProjection=" << static_cast<int>(obj.camera.projection) << "\n";
            file << "cameraFovAxis=" << static_cast<int>(obj.camera.fovAxis) << "\n";
            file << "cameraOrthoSize=" << obj.camera.orthoSize << "\n";
            file << "cameraRenderShadows=" << (obj.camera.renderShadows ? 1 : 0) << "\n";
            file << "cameraBackground=" << static_cast<int>(obj.camera.background) << "\n";
            file << "cameraBackgroundColor=" << obj.camera.backgroundColor.r << "," << obj.camera.backgroundColor.g << "," << obj.camera.backgroundColor.b << "\n";
            file << "cameraCullingMask=" << obj.camera.cullingMask << "\n";
            file << "uiAnchor=" << static_cast<int>(obj.ui.anchor) << "\n";
            file << "uiPosition=" << obj.ui.position.x << "," << obj.ui.position.y << "\n";
            file << "uiRotation=" << obj.ui.rotation << "\n";
            file << "uiSize=" << obj.ui.size.x << "," << obj.ui.size.y << "\n";
            file << "uiMaskChildren=" << (obj.ui.maskChildren ? 1 : 0) << "\n";
            file << "uiSliderValue=" << obj.ui.sliderValue << "\n";
            file << "uiSliderMin=" << obj.ui.sliderMin << "\n";
            file << "uiSliderMax=" << obj.ui.sliderMax << "\n";
            file << "uiLabel=" << obj.ui.label << "\n";
            file << "uiColor=" << obj.ui.color.r << "," << obj.ui.color.g << "," << obj.ui.color.b << "," << obj.ui.color.a << "\n";
            file << "uiInteractable=" << (obj.ui.interactable ? 1 : 0) << "\n";
            file << "uiSliderStyle=" << static_cast<int>(obj.ui.sliderStyle) << "\n";
            file << "uiButtonStyle=" << static_cast<int>(obj.ui.buttonStyle) << "\n";
            file << "uiStylePreset=" << obj.ui.stylePreset << "\n";
            file << "uiTextScale=" << obj.ui.textScale << "\n";
            file << "uiTextFont=" << obj.ui.textFont << "\n";
            file << "uiTextWrap=" << (obj.ui.textAutoWrap ? 1 : 0) << "\n";
            file << "uiTextHAlign=" << static_cast<int>(obj.ui.textHAlign) << "\n";
            file << "uiTextVAlign=" << static_cast<int>(obj.ui.textVAlign) << "\n";
            file << "uiTextEffectFlags=" << obj.ui.textEffectFlags << "\n";
            file << "uiTextEffectSpeed=" << obj.ui.textEffectSpeed << "\n";
            file << "uiTextEffectIntensity=" << obj.ui.textEffectIntensity << "\n";
            file << "uiRenderIn3D=" << (obj.ui.renderIn3D ? 1 : 0) << "\n";
            file << "uiRenderTargetSize=" << obj.ui.renderTargetSize.x << "," << obj.ui.renderTargetSize.y << "\n";
            file << "uiRenderTargetFilter=" << static_cast<int>(obj.ui.renderTargetFilter) << "\n";
            file << "uiPseudo3DEnabled=" << (obj.ui.pseudo3DEnabled ? 1 : 0) << "\n";
            file << "uiPseudo3DUseOffscreen=" << (obj.ui.pseudo3DUseOffscreenSurface ? 1 : 0) << "\n";
            file << "uiPseudo3DPanelSize=" << obj.ui.pseudo3DPanelSize.x << "," << obj.ui.pseudo3DPanelSize.y << "\n";
            file << "uiPseudo3DTopLeftOffset=" << obj.ui.pseudo3DTopLeftOffset.x << "," << obj.ui.pseudo3DTopLeftOffset.y << "\n";
            file << "uiPseudo3DTopRightOffset=" << obj.ui.pseudo3DTopRightOffset.x << "," << obj.ui.pseudo3DTopRightOffset.y << "\n";
            file << "uiPseudo3DBottomRightOffset=" << obj.ui.pseudo3DBottomRightOffset.x << "," << obj.ui.pseudo3DBottomRightOffset.y << "\n";
            file << "uiPseudo3DBottomLeftOffset=" << obj.ui.pseudo3DBottomLeftOffset.x << "," << obj.ui.pseudo3DBottomLeftOffset.y << "\n";
            file << "uiPseudo3DPivot=" << obj.ui.pseudo3DPivot.x << "," << obj.ui.pseudo3DPivot.y << "\n";
            file << "uiPseudo3DPerspectiveIntensity=" << obj.ui.pseudo3DPerspectiveIntensity << "\n";
            file << "uiPseudo3DSkewAmount=" << obj.ui.pseudo3DSkewAmount << "\n";
            file << "uiPseudo3DCurvatureAmount=" << obj.ui.pseudo3DCurvatureAmount << "\n";
            file << "uiPseudo3DAnchorTargetId=" << obj.ui.pseudo3DAnchorTargetId << "\n";
            file << "uiPseudo3DDistanceScaling=" << (obj.ui.pseudo3DDistanceScalingEnabled ? 1 : 0) << "\n";
            file << "uiPseudo3DAdjustPerspectiveDistance=" << (obj.ui.pseudo3DAdjustPerspectiveWithDistance ? 1 : 0) << "\n";
            file << "uiPseudo3DMinDistance=" << obj.ui.pseudo3DMinDistance << "\n";
            file << "uiPseudo3DMaxDistance=" << obj.ui.pseudo3DMaxDistance << "\n";
            file << "uiPseudo3DInteractionDistance=" << obj.ui.pseudo3DInteractionDistance << "\n";
            file << "uiPseudo3DDepthSort=" << obj.ui.pseudo3DDepthSort << "\n";
            file << "uiPseudo3DAllowInteraction=" << (obj.ui.pseudo3DAllowInteraction ? 1 : 0) << "\n";
            file << "uiSpriteSheetEnabled=" << (obj.ui.spriteSheetEnabled ? 1 : 0) << "\n";
            file << "uiSpriteSheetGrid=" << obj.ui.spriteSheetColumns << "," << obj.ui.spriteSheetRows << "\n";
            file << "uiSpriteSheetFrame=" << obj.ui.spriteSheetFrame << "\n";
            file << "uiSpriteSheetFps=" << obj.ui.spriteSheetFps << "\n";
            file << "uiSpriteSheetLoop=" << (obj.ui.spriteSheetLoop ? 1 : 0) << "\n";
            file << "uiSpriteCustomFramesEnabled=" << (obj.ui.spriteCustomFramesEnabled ? 1 : 0) << "\n";
            file << "uiSpriteSourceSize=" << obj.ui.spriteSourceWidth << "," << obj.ui.spriteSourceHeight << "\n";
            file << "uiNineSliceEnabled=" << (obj.ui.nineSliceEnabled ? 1 : 0) << "\n";
            file << "uiNineSliceBorder="
                 << obj.ui.nineSliceBorder.x << ","
                 << obj.ui.nineSliceBorder.y << ","
                 << obj.ui.nineSliceBorder.z << ","
                 << obj.ui.nineSliceBorder.w << "\n";
            file << "uiNineSliceTileEdges=" << (obj.ui.nineSliceTileEdges ? 1 : 0) << "\n";
            file << "uiNineSliceTileCenter=" << (obj.ui.nineSliceTileCenter ? 1 : 0) << "\n";
            file << "uiReceiveLighting2D=" << (obj.ui.receiveLighting2D ? 1 : 0) << "\n";
            file << "uiUnlitLighting2D=" << (obj.ui.unlitLighting2D ? 1 : 0) << "\n";
            file << "uiEmissiveLighting2D=" << obj.ui.emissiveLighting2D << "\n";
            file << "uiFillColor=" << obj.ui.fillColor.r << "," << obj.ui.fillColor.g << "," << obj.ui.fillColor.b << "," << obj.ui.fillColor.a << "\n";
            file << "uiBackgroundColor=" << obj.ui.backgroundColor.r << "," << obj.ui.backgroundColor.g << "," << obj.ui.backgroundColor.b << "," << obj.ui.backgroundColor.a << "\n";
            file << "uiBorderColor=" << obj.ui.borderColor.r << "," << obj.ui.borderColor.g << "," << obj.ui.borderColor.b << "," << obj.ui.borderColor.a << "\n";
            file << "uiTextColor=" << obj.ui.textColor.r << "," << obj.ui.textColor.g << "," << obj.ui.textColor.b << "," << obj.ui.textColor.a << "\n";
            file << "uiFontSize=" << obj.ui.fontSize << "\n";
            file << "uiSortingOrder=" << obj.ui.sortingOrder << "\n";
            file << "uiBackdropBlur=" << obj.ui.backdropBlur << "\n";
            file << "uiBackdropRounding=" << obj.ui.backdropRounding << "\n";
            if (!obj.ui.spriteCustomFrames.empty()) {
                file << "uiSpriteCustomFrames=";
                for (size_t i = 0; i < obj.ui.spriteCustomFrames.size(); ++i) {
                    const glm::ivec4& frame = obj.ui.spriteCustomFrames[i];
                    if (i > 0) file << ";";
                    file << frame.x << "," << frame.y << "," << frame.z << "," << frame.w;
                }
                file << "\n";
            }
            if (!obj.ui.spriteCustomFrameNames.empty()) {
                file << "uiSpriteCustomFrameNames=";
                for (size_t i = 0; i < obj.ui.spriteCustomFrameNames.size(); ++i) {
                    if (i > 0) file << ";";
                    file << obj.ui.spriteCustomFrameNames[i];
                }
                file << "\n";
            }
            if (!obj.ui.spriteCustomFrameScales.empty()) {
                file << "uiSpriteCustomFrameScales=";
                for (size_t i = 0; i < obj.ui.spriteCustomFrameScales.size(); ++i) {
                    if (i > 0) file << ";";
                    const glm::vec2& scale = obj.ui.spriteCustomFrameScales[i];
                    file << scale.x << "," << scale.y;
                }
                file << "\n";
            }
            if (obj.hasPostFX) {
                file << "postEnabled=" << (obj.postFx.enabled ? 1 : 0) << "\n";
                file << "postVolumeGlobal=" << (obj.postFx.isGlobal ? 1 : 0) << "\n";
                file << "postVolumePriority=" << obj.postFx.priority << "\n";
                file << "postVolumeWeight=" << obj.postFx.distortionPolarization << "\n";
                file << "postBlendWeight=" << obj.postFx.blendWeight << "\n";
                file << "postVolumeBlendRadius=" << obj.postFx.blendRadius << "\n";
                file << "postHDREnabled=" << (obj.postFx.hdrEnabled ? 1 : 0) << "\n";
                file << "postToneMapper=" << static_cast<int>(obj.postFx.toneMapper) << "\n";
                file << "postWhitePoint=" << obj.postFx.whitePoint << "\n";
                file << "postGamma=" << obj.postFx.gamma << "\n";
                file << "postBloomEnabled=" << (obj.postFx.bloomEnabled ? 1 : 0) << "\n";
                file << "postBloomThreshold=" << obj.postFx.bloomThreshold << "\n";
                file << "postBloomSoftKnee=" << obj.postFx.bloomSoftKnee << "\n";
                file << "postBloomIntensity=" << obj.postFx.bloomIntensity << "\n";
                file << "postBloomRadius=" << obj.postFx.bloomRadius << "\n";
                file << "postColorAdjustEnabled=" << (obj.postFx.colorAdjustEnabled ? 1 : 0) << "\n";
                file << "postExposure=" << obj.postFx.exposure << "\n";
                file << "postEyeAdaptationEnabled=" << (obj.postFx.eyeAdaptationEnabled ? 1 : 0) << "\n";
                file << "postEyeAdaptationMinEv=" << obj.postFx.eyeAdaptationMinEv << "\n";
                file << "postEyeAdaptationMaxEv=" << obj.postFx.eyeAdaptationMaxEv << "\n";
                file << "postEyeAdaptationSpeedUp=" << obj.postFx.eyeAdaptationSpeedUp << "\n";
                file << "postEyeAdaptationSpeedDown=" << obj.postFx.eyeAdaptationSpeedDown << "\n";
                file << "postEyeAdaptationKeyValue=" << obj.postFx.eyeAdaptationKeyValue << "\n";
                file << "postPurkinjeEnabled=" << (obj.postFx.purkinjeEnabled ? 1 : 0) << "\n";
                file << "postPurkinjeStrength=" << obj.postFx.purkinjeStrength << "\n";
                file << "postPurkinjeThreshold=" << obj.postFx.purkinjeThreshold << "\n";
                file << "postPurkinjeRange=" << obj.postFx.purkinjeRange << "\n";
                file << "postPurkinjeTint=" << obj.postFx.purkinjeTint.r << "," << obj.postFx.purkinjeTint.g << "," << obj.postFx.purkinjeTint.b << "\n";
                file << "postContrast=" << obj.postFx.contrast << "\n";
                file << "postSaturation=" << obj.postFx.saturation << "\n";
                file << "postColorFilter=" << obj.postFx.colorFilter.r << "," << obj.postFx.colorFilter.g << "," << obj.postFx.colorFilter.b << "\n";
                file << "postMotionBlurEnabled=" << (obj.postFx.motionBlurEnabled ? 1 : 0) << "\n";
                file << "postMotionBlurStrength=" << obj.postFx.motionBlurStrength << "\n";
                file << "postMotionBlurThreshold=" << obj.postFx.motionBlurThreshold << "\n";
                file << "postMotionBlurClamp=" << obj.postFx.motionBlurClamp << "\n";
                file << "postVignetteEnabled=" << (obj.postFx.vignetteEnabled ? 1 : 0) << "\n";
                file << "postLensFlareEnabled=" << (obj.postFx.lensFlareEnabled ? 1 : 0) << "\n";
                file << "postLensFlareIntensity=" << obj.postFx.lensFlareIntensity << "\n";
                file << "postLensFlareThreshold=" << obj.postFx.lensFlareThreshold << "\n";
                file << "postLensFlareGhosts=" << obj.postFx.lensFlareGhosts << "\n";
                file << "postLensFlareGhostSpacing=" << obj.postFx.lensFlareGhostSpacing << "\n";
                file << "postLensFlareHaloRadius=" << obj.postFx.lensFlareHaloRadius << "\n";
                file << "postLensFlareStreak=" << obj.postFx.lensFlareStreak << "\n";
                file << "postLensFlareTint=" << obj.postFx.lensFlareTint.x << "," << obj.postFx.lensFlareTint.y << "," << obj.postFx.lensFlareTint.z << "\n";
                file << "postLensFlareGhostsEnabled=" << (obj.postFx.lensFlareGhostsEnabled ? 1 : 0) << "\n";
                file << "postLensFlareHaloEnabled=" << (obj.postFx.lensFlareHaloEnabled ? 1 : 0) << "\n";
                file << "postLensFlareStreakEnabled=" << (obj.postFx.lensFlareStreakEnabled ? 1 : 0) << "\n";
                file << "postLensFlareGlowEnabled=" << (obj.postFx.lensFlareGlowEnabled ? 1 : 0) << "\n";
                file << "postLensFlareGlowSize=" << obj.postFx.lensFlareGlowSize << "\n";
                file << "postLensFlareGlowIntensity=" << obj.postFx.lensFlareGlowIntensity << "\n";
                file << "postVignetteIntensity=" << obj.postFx.vignetteIntensity << "\n";
                file << "postVignetteSmoothness=" << obj.postFx.vignetteSmoothness << "\n";
                file << "postChromaticEnabled=" << (obj.postFx.chromaticAberrationEnabled ? 1 : 0) << "\n";
                file << "postChromaticAmount=" << obj.postFx.chromaticAmount << "\n";
                file << "postSharpenEnabled=" << (obj.postFx.sharpenEnabled ? 1 : 0) << "\n";
                file << "postSharpenStrength=" << obj.postFx.sharpenStrength << "\n";
                file << "postAOEnabled=" << (obj.postFx.ambientOcclusionEnabled ? 1 : 0) << "\n";
                file << "postAORadius=" << obj.postFx.aoRadius << "\n";
                file << "postAOStrength=" << obj.postFx.aoStrength << "\n";
                file << "postDitherEnabled=" << (obj.postFx.ditherEnabled ? 1 : 0) << "\n";
                file << "postDitherIntensity=" << obj.postFx.ditherIntensity << "\n";
                file << "postDitherColorBits=" << obj.postFx.ditherColorBits << "\n";
                file << "postDitherDarkAdjustment=" << obj.postFx.ditherDarkAdjustment << "\n";
                file << "postDitherPixelation=" << obj.postFx.ditherPixelation << "\n";
                file << "postDitherSize=" << obj.postFx.ditherSize << "\n";
                file << "postDitherContrast=" << obj.postFx.ditherContrast << "\n";
                file << "postDitherOffset=" << obj.postFx.ditherOffset << "\n";
                file << "postDitherPalette=" << static_cast<int>(obj.postFx.ditherPalette) << "\n";
                file << "postDitherPattern=" << static_cast<int>(obj.postFx.ditherPattern) << "\n";
                file << "postStaticEnabled=" << (obj.postFx.staticEnabled ? 1 : 0) << "\n";
                file << "postStaticIntensity=" << obj.postFx.staticIntensity << "\n";
                file << "postStaticGrainScale=" << obj.postFx.staticGrainScale << "\n";
                file << "postStaticDarkAreaInfluence=" << obj.postFx.staticDarkAreaInfluence << "\n";
                file << "postStaticSpeed=" << obj.postFx.staticSpeed << "\n";
                file << "postStaticMonochrome=" << (obj.postFx.staticMonochrome ? 1 : 0) << "\n";
                file << "postStaticSparkle=" << obj.postFx.staticSparkle << "\n";
                file << "postStaticDistortionEnabled=" << (obj.postFx.staticDistortionEnabled ? 1 : 0) << "\n";
                file << "postStaticDistortionHorizontalJitterAmount="
                     << obj.postFx.staticDistortionHorizontalJitterAmount << "\n";
                file << "postStaticDistortionLineDensity=" << obj.postFx.staticDistortionLineDensity << "\n";
                file << "postStaticDistortionGlitchFrequency=" << obj.postFx.staticDistortionGlitchFrequency << "\n";
                file << "postStaticDistortionStrength=" << obj.postFx.staticDistortionStrength << "\n";
                file << "postLensDistortionEnabled=" << (obj.postFx.lensDistortionEnabled ? 1 : 0) << "\n";
                file << "postLensDistortionAmount=" << obj.postFx.lensDistortionAmount << "\n";
                file << "postLensDistortionEdgeFalloff=" << obj.postFx.lensDistortionEdgeFalloff << "\n";
                file << "postLensDistortionCenterOffset=" << obj.postFx.lensDistortionCenterOffset.x << ","
                     << obj.postFx.lensDistortionCenterOffset.y << "\n";
                file << "postLensDistortionEdgeVignetteEnabled="
                     << (obj.postFx.lensDistortionEdgeVignetteEnabled ? 1 : 0) << "\n";
                file << "postLensDistortionEdgeVignetteIntensity="
                     << obj.postFx.lensDistortionEdgeVignetteIntensity << "\n";
                file << "postLensDistortionEdgeVignetteRadius="
                     << obj.postFx.lensDistortionEdgeVignetteRadius << "\n";
                file << "postLensDistortionEdgeVignetteSoftness="
                     << obj.postFx.lensDistortionEdgeVignetteSoftness << "\n";
                file << "postLensDistortionEdgeVignetteColor="
                     << obj.postFx.lensDistortionEdgeVignetteColor.x << ","
                     << obj.postFx.lensDistortionEdgeVignetteColor.y << ","
                     << obj.postFx.lensDistortionEdgeVignetteColor.z << "\n";
                file << "postPixelationEnabled=" << (obj.postFx.pixelationEnabled ? 1 : 0) << "\n";
                file << "postPixelationSize=" << obj.postFx.pixelationSize << "\n";
                file << "postPosterizeEnabled=" << (obj.postFx.posterizeEnabled ? 1 : 0) << "\n";
                file << "postPosterizeLevels=" << obj.postFx.posterizeLevels << "\n";
                file << "postScanlinesEnabled=" << (obj.postFx.scanlinesEnabled ? 1 : 0) << "\n";
                file << "postScanlinesIntensity=" << obj.postFx.scanlinesIntensity << "\n";
                file << "postScanlinesDensity=" << obj.postFx.scanlinesDensity << "\n";
                file << "postScanlinesSpeed=" << obj.postFx.scanlinesSpeed << "\n";
                file << "postVHSOverlayEnabled=" << (obj.postFx.vhsOverlayEnabled ? 1 : 0) << "\n";
                file << "postVHSOverlayOpacity=" << obj.postFx.vhsOverlayOpacity << "\n";
                file << "postVHSOverlayScanlineStrength=" << obj.postFx.vhsOverlayScanlineStrength << "\n";
                file << "postVHSOverlayTapeNoise=" << obj.postFx.vhsOverlayTapeNoise << "\n";
                file << "postVHSOverlayChromaBleed=" << obj.postFx.vhsOverlayChromaBleed << "\n";
                file << "postVHSOverlayBottomNoiseBandHeight=" << obj.postFx.vhsOverlayBottomNoiseBandHeight << "\n";
                file << "postVHSOverlayBottomNoiseBandIntensity=" << obj.postFx.vhsOverlayBottomNoiseBandIntensity << "\n";
                file << "postVHSOverlayDistortionStrength=" << obj.postFx.vhsOverlayDistortionStrength << "\n";
                file << "postVHSOverlayAnimationSpeed=" << obj.postFx.vhsOverlayAnimationSpeed << "\n";
                file << "postVHSOverlayColorBleed=" << obj.postFx.vhsOverlayColorBleed << "\n";
                file << "postVHSOverlayBanding=" << obj.postFx.vhsOverlayBanding << "\n";
                file << "postVHSOverlaySignalMode=" << static_cast<int>(obj.postFx.vhsOverlaySignalMode) << "\n";
                file << "postVHSOverlayDropouts=" << obj.postFx.vhsOverlayDropouts << "\n";
                file << "postWavyEnabled=" << (obj.postFx.wavyEnabled ? 1 : 0) << "\n";
                file << "postWavyAmplitude=" << obj.postFx.wavyAmplitude << "\n";
                file << "postWavyFrequency=" << obj.postFx.wavyFrequency << "\n";
                file << "postWavySpeed=" << obj.postFx.wavySpeed << "\n";
                file << "postWavyVertical=" << (obj.postFx.wavyVertical ? 1 : 0) << "\n";
                file << "postScope2DEnabled=" << (obj.postFx.scope2DEnabled ? 1 : 0) << "\n";
                file << "postScope2DMode=" << static_cast<int>(obj.postFx.scope2DMode) << "\n";
                file << "postScope2DMinOrder=" << obj.postFx.scope2DMinOrder << "\n";
                file << "postScope2DMaxOrder=" << obj.postFx.scope2DMaxOrder << "\n";
            }

            file << "scriptCount=" << obj.scripts.size() << "\n";
            for (size_t s = 0; s < obj.scripts.size(); ++s) {
                const auto& sc = obj.scripts[s];
                file << "script" << s << "_path=" << sc.path << "\n";
                file << "script" << s << "_lang=" << static_cast<int>(sc.language) << "\n";
                file << "script" << s << "_type=" << sc.managedType << "\n";
                file << "script" << s << "_enabled=" << (sc.enabled ? 1 : 0) << "\n";
                file << "script" << s << "_settingCount=" << sc.settings.size() << "\n";
                for (size_t si = 0; si < sc.settings.size(); ++si) {
                    file << "script" << s << "_setting" << si << "=" << sc.settings[si].key << ":" << sc.settings[si].value << "\n";
                }
            }

            if (obj.hasRenderer &&
                (obj.renderType == RenderType::OBJMesh || obj.renderType == RenderType::Model) &&
                !obj.meshPath.empty()) {
                file << "meshPath=" << obj.meshPath << "\n";
                if (obj.renderType == RenderType::Model && obj.meshSourceIndex >= 0) {
                    file << "meshSourceIndex=" << obj.meshSourceIndex << "\n";
                }
            }

            file << "children=";
            for (size_t i = 0; i < obj.childIds.size(); i++) {
                if (i > 0) file << ",";
                file << obj.childIds[i];
            }
            file << "\n\n";
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to write legacy scene stream: " << e.what() << std::endl;
        return false;
    }
}

bool SceneSerializationInternal::SceneValueLooksLikeAbsolutePath(const std::string& value) {
    if (value.size() < 2) return false;
    if (value[0] == '/' || value[0] == '\\') return true;
    const char c = value[0];
    const bool isLetter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return isLetter && value[1] == ':';
}

std::string SceneSerializationInternal::MakeSceneTextPathsPortable(const std::string& sceneText) {
    std::istringstream in(sceneText);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string value = line.substr(eq + 1);
            std::string carriage;
            if (!value.empty() && value.back() == '\r') {
                carriage = "\r";
                value.pop_back();
            }
            if (SceneValueLooksLikeAbsolutePath(value)) {
                line = line.substr(0, eq + 1) +
                       Modularity::Platform::ToPortableAssetPath(value) + carriage;
            }
        }
        out << line << '\n';
    }
    return out.str();
}

bool SceneSerializationInternal::SaveLegacyScene(const fs::path& filePath,
                                                 const std::vector<SceneObject>& objects,
                                                 int nextId,
                                                 float timeOfDay,
                                                 const SkyboxSettings& skyboxSettings,
                                                 SceneSerializer::Metadata* metadata) {
    try {
        std::ofstream file(filePath);
        if (!file.is_open()) return false;
        std::ostringstream buffer;
        if (!WriteLegacySceneStream(buffer, objects, nextId, timeOfDay, skyboxSettings)) {
            return false;
        }
        file << MakeSceneTextPathsPortable(buffer.str());
        file.close();
        if (metadata) {
            metadata->version = SceneSerializationInternal::kLegacySceneFormatVersion;
            metadata->fileFormat = SceneSerializer::FileFormat::LegacyFlat;
            metadata->loadedFromLegacyLayout = true;
            metadata->upgradedToModularLayout = false;
            metadata->sourcePath = filePath;
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to save legacy scene: " << e.what() << std::endl;
        return false;
    }
}

namespace {
template <typename Vec2T>
void ParseVec2(const std::string& value, Vec2T& out) {
    sscanf(value.c_str(), "%f,%f", &out.x, &out.y);
}

void ParseIVec2(const std::string& value, glm::ivec2& out) {
    int x = 0;
    int y = 0;
    if (sscanf(value.c_str(), "%d,%d", &x, &y) == 2) {
        out.x = x;
        out.y = y;
    }
}

void ParseVec2List(const std::string& value, std::vector<glm::vec2>& out) {
    out.clear();
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ';')) {
        if (item.empty()) continue;
        glm::vec2 v(0.0f);
        ParseVec2(item, v);
        out.push_back(v);
    }
}

template <typename Vec3T>
void ParseVec3(const std::string& value, Vec3T& out) {
    sscanf(value.c_str(), "%f,%f,%f", &out.x, &out.y, &out.z);
}

template <typename Vec4T>
void ParseVec4(const std::string& value, Vec4T& out) {
    sscanf(value.c_str(), "%f,%f,%f,%f", &out.x, &out.y, &out.z, &out.w);
}

void ParseMinMaxFloat(const std::string& value, ParticleSystem2DComponent::MinMaxFloat& out) {
    int random = out.random ? 1 : 0;
    if (sscanf(value.c_str(), "%f,%f,%d", &out.min, &out.max, &random) >= 2) {
        out.random = random != 0;
        if (out.max < out.min) std::swap(out.min, out.max);
    }
}

bool g_deferSceneAssetLoading = false;

bool IsDefaultTransform(const SceneObject& obj) {
    auto nearZero = [](float v) { return std::abs(v) < 1e-4f; };
    auto nearOne = [](float v) { return std::abs(v - 1.0f) < 1e-4f; };
    return nearZero(obj.localPosition.x) &&
           nearZero(obj.localPosition.y) &&
           nearZero(obj.localPosition.z) &&
           nearZero(obj.localRotation.x) &&
           nearZero(obj.localRotation.y) &&
           nearZero(obj.localRotation.z) &&
           nearOne(obj.localScale.x) &&
           nearOne(obj.localScale.y) &&
           nearOne(obj.localScale.z);
}

void ApplyModelRootTransform(SceneObject& obj, const ModelSceneData& sceneData) {
    if (sceneData.nodes.empty()) return;
    if (obj.localInitialized && !IsDefaultTransform(obj)) return;
    const auto& root = sceneData.nodes.front();
    obj.localPosition = root.localPosition;
    obj.localRotation = root.localRotation;
    obj.localScale = root.localScale;
    obj.localInitialized = true;
    obj.position = obj.localPosition;
    obj.rotation = obj.localRotation;
    obj.scale = obj.localScale;
}

using KeyHandler = void (*)(SceneObject&, const std::string&);

const std::unordered_map<std::string, KeyHandler>& GetSceneObjectKeyHandlers() {
    static const std::unordered_map<std::string, KeyHandler> handlers = {
        {"id", +[](SceneObject& obj, const std::string& value) { obj.id = std::stoi(value); }},
        {"name", +[](SceneObject& obj, const std::string& value) { obj.name = value; }},
        {"type", +[](SceneObject& obj, const std::string& value) {
             obj.type = static_cast<ObjectType>(std::stoi(value));
         }},
        {"enabled", +[](SceneObject& obj, const std::string& value) { obj.enabled = (std::stoi(value) != 0); }},
        {"invariable", +[](SceneObject& obj, const std::string& value) { obj.IsInvariable = (std::stoi(value) != 0); }},
        {"layer", +[](SceneObject& obj, const std::string& value) { obj.layer = std::stoi(value); }},
        {"tag", +[](SceneObject& obj, const std::string& value) { obj.tag = value; }},
        {"editorIconTint", +[](SceneObject& obj, const std::string& value) { ParseVec4(value, obj.editorIconTint); }},
        {"editorIcon", +[](SceneObject& obj, const std::string& value) { obj.editorIconPath = value; }},
        {"editorIconGizmo", +[](SceneObject& obj, const std::string& value) { obj.editorIconShowInViewport = std::stoi(value) != 0; }},
        {"hasRenderer", +[](SceneObject& obj, const std::string& value) { obj.hasRenderer = std::stoi(value) != 0; }},
        {"renderType", +[](SceneObject& obj, const std::string& value) {
             obj.renderType = static_cast<RenderType>(std::stoi(value));
             if (obj.renderType != RenderType::None) {
                 obj.hasRenderer = true;
             }
         }},
        {"faceCamera", +[](SceneObject& obj, const std::string& value) { obj.faceCamera = std::stoi(value) != 0; }},
        {"hasLight", +[](SceneObject& obj, const std::string& value) { obj.hasLight = std::stoi(value) != 0; }},
        {"hasLight2D", +[](SceneObject& obj, const std::string& value) { obj.hasLight2D = std::stoi(value) != 0; }},
        {"hasReflectionCast", +[](SceneObject& obj, const std::string& value) { obj.hasReflectionCast = std::stoi(value) != 0; }},
        {"hasCamera", +[](SceneObject& obj, const std::string& value) { obj.hasCamera = std::stoi(value) != 0; }},
        {"hasPostFX", +[](SceneObject& obj, const std::string& value) { obj.hasPostFX = std::stoi(value) != 0; }},
        {"hasUI", +[](SceneObject& obj, const std::string& value) { obj.hasUI = std::stoi(value) != 0; }},
        {"hasShadowCaster2D", +[](SceneObject& obj, const std::string& value) { obj.hasShadowCaster2D = std::stoi(value) != 0; }},
        {"moduObjInstance", +[](SceneObject& obj, const std::string& value) { obj.hasModuObjInstance = std::stoi(value) != 0; }},
        {"moduObjVersion", +[](SceneObject& obj, const std::string& value) { (void)obj; (void)value; }},
        {"moduObjAssetId", +[](SceneObject& obj, const std::string& value) { obj.hasModuObjInstance = true; obj.moduObjInstance.assetId = value; }},
        {"moduObjInstanceId", +[](SceneObject& obj, const std::string& value) { obj.hasModuObjInstance = true; obj.moduObjInstance.instanceId = value; }},
        {"moduObjSourceLocalId", +[](SceneObject& obj, const std::string& value) { obj.hasModuObjInstance = true; obj.moduObjInstance.sourceLocalId = std::stoi(value); }},
        {"moduObjIsRoot", +[](SceneObject& obj, const std::string& value) { obj.hasModuObjInstance = true; obj.moduObjInstance.isRoot = std::stoi(value) != 0; }},
        {"moduObjSourceMissing", +[](SceneObject& obj, const std::string& value) { obj.hasModuObjInstance = true; obj.moduObjInstance.sourceMissing = std::stoi(value) != 0; }},
        {"moduObjLastKnownPath", +[](SceneObject& obj, const std::string& value) { obj.hasModuObjInstance = true; obj.moduObjInstance.lastKnownPath = value; }},
        {"netIdentity", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkIdentity = std::stoi(value) != 0; }},
        {"netIdentityVersion", +[](SceneObject& obj, const std::string& value) { (void)obj; (void)value; }},
        {"netIdentityEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkIdentity = true; obj.networkIdentity.enabled = std::stoi(value) != 0; }},
        {"netIdentityAssetId", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkIdentity = true; obj.networkIdentity.assetId = value; }},
        {"netIdentitySyncPosition", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkIdentity = true; obj.networkIdentity.syncPosition = std::stoi(value) != 0; }},
        {"netIdentitySyncRotation", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkIdentity = true; obj.networkIdentity.syncRotation = std::stoi(value) != 0; }},
        {"netIdentitySyncScale", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkIdentity = true; obj.networkIdentity.syncScale = std::stoi(value) != 0; }},
        {"netIdentitySyncVelocity", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkIdentity = true; obj.networkIdentity.syncVelocity = std::stoi(value) != 0; }},
        {"netIdentitySyncMode", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkIdentity = true; obj.networkIdentity.syncMode = std::stoi(value); }},
        {"netIdentitySendRate", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkIdentity = true; obj.networkIdentity.sendRateHz = std::stoi(value); }},
        {"netIdentityInterpDelay", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkIdentity = true; obj.networkIdentity.interpolationDelay = std::stof(value); }},
        {"netIdentityMaxExtrapolation", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkIdentity = true; obj.networkIdentity.maxExtrapolation = std::stof(value); }},
        {"netIdentityOwnerOnlyWrites", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkIdentity = true; obj.networkIdentity.ownerOnlyWrites = std::stoi(value) != 0; }},
        {"netIdentitySpawnerOwns", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkIdentity = true; obj.networkIdentity.spawnerOwns = std::stoi(value) != 0; }},
        {"netManager", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkManager = std::stoi(value) != 0; }},
        {"netManagerVersion", +[](SceneObject& obj, const std::string& value) { (void)obj; (void)value; }},
        {"netManagerEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkManager = true; obj.networkManager.enabled = std::stoi(value) != 0; }},
        {"netManagerAppId", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkManager = true; obj.networkManager.appId = value; }},
        {"netManagerAppVersion", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkManager = true; obj.networkManager.appVersion = value; }},
        {"netManagerRegion", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkManager = true; obj.networkManager.region = value; }},
        {"netManagerNickname", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkManager = true; obj.networkManager.nickname = value; }},
        {"netManagerAutoConnect", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkManager = true; obj.networkManager.autoConnect = std::stoi(value) != 0; }},
        {"netManagerOfflineMode", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkManager = true; obj.networkManager.offlineMode = std::stoi(value) != 0; }},
        {"netManagerMaxPlayers", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkManager = true; obj.networkManager.maxPlayers = std::stoi(value); }},
        {"netManagerAutoJoinLobby", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkManager = true; obj.networkManager.autoJoinLobby = std::stoi(value) != 0; }},
        {"netManagerDefaultRoom", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkManager = true; obj.networkManager.defaultRoomName = value; }},
        {"netManagerSendRate", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkManager = true; obj.networkManager.sendRateHz = std::stoi(value); }},
        {"netManagerSerializationRate", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkManager = true; obj.networkManager.serializationRateHz = std::stoi(value); }},
        {"netManagerMaxOutBps", +[](SceneObject& obj, const std::string& value) { obj.hasNetworkManager = true; obj.networkManager.maxOutboundBytesPerSecond = std::stoi(value); }},
        {"uiType", +[](SceneObject& obj, const std::string& value) {
             obj.ui.type = static_cast<UIElementType>(std::stoi(value));
             if (obj.ui.type != UIElementType::None) {
                 obj.hasUI = true;
             }
         }},
        {"parentId", +[](SceneObject& obj, const std::string& value) { obj.parentId = std::stoi(value); }},
        {"position", +[](SceneObject& obj, const std::string& value) {
             ParseVec3(value, obj.position);
             obj.localPosition = obj.position;
             obj.localInitialized = true;
         }},
        {"rotation", +[](SceneObject& obj, const std::string& value) {
             ParseVec3(value, obj.rotation);
             obj.rotation = NormalizeEulerDegrees(obj.rotation);
             obj.localRotation = obj.rotation;
             obj.localInitialized = true;
         }},
        {"scale", +[](SceneObject& obj, const std::string& value) {
             ParseVec3(value, obj.scale);
             obj.localScale = obj.scale;
             obj.localInitialized = true;
         }},
        {"hasRigidbody", +[](SceneObject& obj, const std::string& value) { obj.hasRigidbody = std::stoi(value) != 0; }},
        {"rbEnabled", +[](SceneObject& obj, const std::string& value) { obj.rigidbody.enabled = std::stoi(value) != 0; }},
        {"rbMass", +[](SceneObject& obj, const std::string& value) { obj.rigidbody.mass = std::stof(value); }},
        {"rbUseCustomCenterOfMass", +[](SceneObject& obj, const std::string& value) { obj.rigidbody.useCustomCenterOfMass = std::stoi(value) != 0; }},
        {"rbCenterOfMass", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.rigidbody.centerOfMass); }},
        {"rbUseGravity", +[](SceneObject& obj, const std::string& value) { obj.rigidbody.useGravity = std::stoi(value) != 0; }},
        {"rbKinematic", +[](SceneObject& obj, const std::string& value) { obj.rigidbody.isKinematic = std::stoi(value) != 0; }},
        {"rbLinearDamping", +[](SceneObject& obj, const std::string& value) { obj.rigidbody.linearDamping = std::stof(value); }},
        {"rbAngularDamping", +[](SceneObject& obj, const std::string& value) { obj.rigidbody.angularDamping = std::stof(value); }},
        {"rbInterpolation", +[](SceneObject& obj, const std::string& value) { obj.rigidbody.interpolationMode = std::clamp(std::stoi(value), 0, 2); }},
        {"rbMaxExtrapolation", +[](SceneObject& obj, const std::string& value) { obj.rigidbody.maxExtrapolationTime = std::max(0.0f, std::stof(value)); }},
        {"rbLockRotX", +[](SceneObject& obj, const std::string& value) { obj.rigidbody.lockRotationX = std::stoi(value) != 0; }},
        {"rbLockRotY", +[](SceneObject& obj, const std::string& value) { obj.rigidbody.lockRotationY = std::stoi(value) != 0; }},
        {"rbLockRotZ", +[](SceneObject& obj, const std::string& value) { obj.rigidbody.lockRotationZ = std::stoi(value) != 0; }},
        {"hasRigidbody2D", +[](SceneObject& obj, const std::string& value) { obj.hasRigidbody2D = std::stoi(value) != 0; }},
        {"rb2dEnabled", +[](SceneObject& obj, const std::string& value) { obj.rigidbody2D.enabled = std::stoi(value) != 0; }},
        {"rb2dUseGravity", +[](SceneObject& obj, const std::string& value) { obj.rigidbody2D.useGravity = std::stoi(value) != 0; }},
        {"rb2dLockRotation", +[](SceneObject& obj, const std::string& value) { obj.rigidbody2D.lockRotation = std::stoi(value) != 0; }},
        {"rb2dGravityScale", +[](SceneObject& obj, const std::string& value) { obj.rigidbody2D.gravityScale = std::stof(value); }},
        {"rb2dLinearDamping", +[](SceneObject& obj, const std::string& value) { obj.rigidbody2D.linearDamping = std::stof(value); }},
        {"rb2dVelocity", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.rigidbody2D.velocity); }},
        {"hasCollider2D", +[](SceneObject& obj, const std::string& value) { obj.hasCollider2D = std::stoi(value) != 0; }},
        {"collider2dEnabled", +[](SceneObject& obj, const std::string& value) { obj.collider2D.enabled = std::stoi(value) != 0; }},
        {"collider2dType", +[](SceneObject& obj, const std::string& value) { obj.collider2D.type = static_cast<Collider2DType>(std::stoi(value)); }},
        {"collider2dBox", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.collider2D.boxSize); }},
        {"collider2dOffset", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.collider2D.offset); }},
        {"collider2dClosed", +[](SceneObject& obj, const std::string& value) { obj.collider2D.closed = std::stoi(value) != 0; }},
        {"collider2dEdgeThickness", +[](SceneObject& obj, const std::string& value) { obj.collider2D.edgeThickness = std::stof(value); }},
        {"collider2dPoints", +[](SceneObject& obj, const std::string& value) { ParseVec2List(value, obj.collider2D.points); }},
        {"collider2dRadius", +[](SceneObject& obj, const std::string& value) { obj.collider2D.radius = std::stof(value); }},
        {"collider2dOutlineAlpha", +[](SceneObject& obj, const std::string& value) { obj.collider2D.outlineAlphaThreshold = std::stof(value); }},
        {"collider2dOutlineTolerance", +[](SceneObject& obj, const std::string& value) { obj.collider2D.outlineTolerance = std::stof(value); }},
        {"collider2dOutlineMaxVerts", +[](SceneObject& obj, const std::string& value) { obj.collider2D.outlineMaxVertices = std::stoi(value); }},
        {"collider2dOutlineClosed", +[](SceneObject& obj, const std::string& value) { obj.collider2D.outlineClosed = std::stoi(value) != 0; }},
        {"collider2dOutlineSource", +[](SceneObject& obj, const std::string& value) { obj.collider2D.outlineSourcePath = value; }},
        {"assemblage", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblage = std::stoi(value) != 0; }},
        {"assemblageVersion", +[](SceneObject&, const std::string&) {}},
        {"assemblageEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblage = true; obj.assemblage.enabled = std::stoi(value) != 0; }},
        {"assemblageId", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblage = true; obj.assemblage.assemblageId = value; }},
        {"assemblageAsset", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblage = true; obj.assemblage.assetPath = value; }},
        {"assemblageShowGrid", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblage = true; obj.assemblage.showGrid = std::stoi(value) != 0; }},
        {"assemblageSnapToGrid", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblage = true; obj.assemblage.snapToGrid = std::stoi(value) != 0; }},
        {"assemblageGridColor", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblage = true; ParseVec4(value, obj.assemblage.gridColor); }},
        {"assemblageLayer", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblageLayer = std::stoi(value) != 0; }},
        {"assemblageLayerVersion", +[](SceneObject&, const std::string&) {}},
        {"assemblageLayerEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblageLayer = true; obj.assemblageLayer.enabled = std::stoi(value) != 0; }},
        {"assemblageLayerId", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblageLayer = true; obj.assemblageLayer.layerId = std::stoi(value); }},
        {"assemblageLayerSorting", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblageLayer = true; obj.assemblageLayer.sortingOrder = std::stoi(value); }},
        {"assemblageLayerLocked", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblageLayer = true; obj.assemblageLayer.locked = std::stoi(value) != 0; }},
        {"assemblageLayerOpacity", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblageLayer = true; obj.assemblageLayer.opacity = std::stof(value); }},
        {"assemblageLayerTint", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblageLayer = true; ParseVec4(value, obj.assemblageLayer.tint); }},
        {"assemblageLayerReceiveLighting", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblageLayer = true; obj.assemblageLayer.receiveLighting2D = std::stoi(value) != 0; }},
        {"assemblageLayerUnlit", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblageLayer = true; obj.assemblageLayer.unlitLighting2D = std::stoi(value) != 0; }},
        {"assemblageLayerEmissive", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblageLayer = true; obj.assemblageLayer.emissiveLighting2D = std::stof(value); }},
        {"assemblageLayerCollision", +[](SceneObject& obj, const std::string& value) { obj.hasAssemblageLayer = true; obj.assemblageLayer.collisionEnabled = std::stoi(value) != 0; }},
        {"hasParallaxLayer2D", +[](SceneObject& obj, const std::string& value) { obj.hasParallaxLayer2D = std::stoi(value) != 0; }},
        {"parallax2dEnabled", +[](SceneObject& obj, const std::string& value) { obj.parallaxLayer2D.enabled = std::stoi(value) != 0; }},
        {"parallax2dOrder", +[](SceneObject& obj, const std::string& value) { obj.parallaxLayer2D.order = std::stoi(value); }},
        {"parallax2dFactor", +[](SceneObject& obj, const std::string& value) { obj.parallaxLayer2D.factor = std::stof(value); }},
        {"parallax2dRepeatX", +[](SceneObject& obj, const std::string& value) { obj.parallaxLayer2D.repeatX = std::stoi(value) != 0; }},
        {"parallax2dRepeatY", +[](SceneObject& obj, const std::string& value) { obj.parallaxLayer2D.repeatY = std::stoi(value) != 0; }},
        {"parallax2dDisableCulling", +[](SceneObject& obj, const std::string& value) { obj.parallaxLayer2D.disableCulling = std::stoi(value) != 0; }},
        {"parallax2dSpacing", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.parallaxLayer2D.repeatSpacing); }},
        {"hasCameraFollow2D", +[](SceneObject& obj, const std::string& value) { obj.hasCameraFollow2D = std::stoi(value) != 0; }},
        {"cameraFollow2dEnabled", +[](SceneObject& obj, const std::string& value) { obj.cameraFollow2D.enabled = std::stoi(value) != 0; }},
        {"cameraFollow2dTarget", +[](SceneObject& obj, const std::string& value) { obj.cameraFollow2D.targetId = std::stoi(value); }},
        {"cameraFollow2dOffset", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.cameraFollow2D.offset); }},
        {"cameraFollow2dSmoothTime", +[](SceneObject& obj, const std::string& value) { obj.cameraFollow2D.smoothTime = std::stof(value); }},
        {"hasCollider", +[](SceneObject& obj, const std::string& value) { obj.hasCollider = std::stoi(value) != 0; }},
        {"colliderEnabled", +[](SceneObject& obj, const std::string& value) { obj.collider.enabled = std::stoi(value) != 0; }},
        {"colliderType", +[](SceneObject& obj, const std::string& value) { obj.collider.type = static_cast<ColliderType>(std::stoi(value)); }},
        {"colliderBox", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.collider.boxSize); }},
        {"colliderOffset", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.collider.offset); }},
        {"colliderConvex", +[](SceneObject& obj, const std::string& value) { obj.collider.convex = std::stoi(value) != 0; }},
        {"colliderIsTrigger", +[](SceneObject& obj, const std::string& value) { obj.collider.isTrigger = std::stoi(value) != 0; }},
        {"colliderStaticFriction", +[](SceneObject& obj, const std::string& value) { obj.collider.staticFriction = std::stof(value); }},
        {"colliderDynamicFriction", +[](SceneObject& obj, const std::string& value) { obj.collider.dynamicFriction = std::stof(value); }},
        {"colliderRestitution", +[](SceneObject& obj, const std::string& value) { obj.collider.restitution = std::stof(value); }},
        {"hasPlayerController", +[](SceneObject& obj, const std::string& value) { obj.hasPlayerController = std::stoi(value) != 0; }},
        {"pcEnabled", +[](SceneObject& obj, const std::string& value) { obj.playerController.enabled = std::stoi(value) != 0; }},
        {"pcMoveSpeed", +[](SceneObject& obj, const std::string& value) { obj.playerController.moveSpeed = std::stof(value); }},
        {"pcRunSpeed", +[](SceneObject& obj, const std::string& value) { obj.playerController.runSpeed = std::stof(value); }},
        {"pcLookSensitivity", +[](SceneObject& obj, const std::string& value) { obj.playerController.lookSensitivity = std::stof(value); }},
        {"pcGroundAcceleration", +[](SceneObject& obj, const std::string& value) { obj.playerController.groundAcceleration = std::stof(value); }},
        {"pcAirAcceleration", +[](SceneObject& obj, const std::string& value) { obj.playerController.airAcceleration = std::stof(value); }},
        {"pcBraking", +[](SceneObject& obj, const std::string& value) { obj.playerController.braking = std::stof(value); }},
        {"pcMinSurfaceControl", +[](SceneObject& obj, const std::string& value) { obj.playerController.minSurfaceControl = std::stof(value); }},
        {"pcSlideGravity", +[](SceneObject& obj, const std::string& value) { obj.playerController.slideGravity = std::stof(value); }},
        {"pcPlatformCarry", +[](SceneObject& obj, const std::string& value) { obj.playerController.platformCarry = std::stof(value); }},
        {"pcHeight", +[](SceneObject& obj, const std::string& value) { obj.playerController.height = std::stof(value); }},
        {"pcRadius", +[](SceneObject& obj, const std::string& value) { obj.playerController.radius = std::stof(value); }},
        {"pcJumpStrength", +[](SceneObject& obj, const std::string& value) { obj.playerController.jumpStrength = std::stof(value); }},
        {"pcVmEnabled", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.enabled = std::stoi(value) != 0; }},
        {"pcVmBobFrequency", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.bobFrequency = std::stof(value); }},
        {"pcVmBobVertical", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.bobVertical = std::stof(value); }},
        {"pcVmBobHorizontal", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.bobHorizontal = std::stof(value); }},
        {"pcVmBobRoll", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.bobRoll = std::stof(value); }},
        {"pcVmRunMultiplier", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.runMultiplier = std::stof(value); }},
        {"pcVmLookSway", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.lookSway = std::stof(value); }},
        {"pcVmLookSwayStiffness", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.lookSwayStiffness = std::stof(value); }},
        {"pcVmLookSwayDamping", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.lookSwayDamping = std::stof(value); }},
        {"pcVmLookSwayMax", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.lookSwayMax = std::stof(value); }},
        {"pcVmStrafeRoll", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.strafeRoll = std::stof(value); }},
        {"pcVmTurnRoll", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.turnRoll = std::stof(value); }},
        {"pcVmRollSmoothing", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.rollSmoothing = std::stof(value); }},
        {"pcVmIdleAmount", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.idleAmount = std::stof(value); }},
        {"pcVmIdleFrequency", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.idleFrequency = std::stof(value); }},
        {"pcVmLandingDip", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.landingDip = std::stof(value); }},
        {"pcVmLandingStiffness", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.landingStiffness = std::stof(value); }},
        {"pcVmLandingDamping", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.landingDamping = std::stof(value); }},
        {"pcVmAttachedEnabled", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.attachedSwayEnabled = std::stoi(value) != 0; }},
        {"pcVmAttachedSway", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.attachedSway = std::stof(value); }},
        {"pcVmAttachedStiffness", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.attachedStiffness = std::stof(value); }},
        {"pcVmAttachedDamping", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.attachedDamping = std::stof(value); }},
        {"pcVmAttachedMax", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.attachedMax = std::stof(value); }},
        {"pcVmJumpCrouchDip", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.jumpCrouchDip = std::stof(value); }},
        {"pcFeelLookSmoothing", +[](SceneObject& obj, const std::string& value) { obj.playerControlFeel.lookSmoothing = std::stof(value); }},
        {"pcFeelChargedJump", +[](SceneObject& obj, const std::string& value) { obj.playerControlFeel.chargedJump = std::stoi(value) != 0; }},
        {"pcFeelJumpChargeTime", +[](SceneObject& obj, const std::string& value) { obj.playerControlFeel.jumpChargeTime = std::stof(value); }},
        {"pcFeelJumpMinScale", +[](SceneObject& obj, const std::string& value) { obj.playerControlFeel.jumpChargeMinScale = std::stof(value); }},
        {"pcVmGaitBlend", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.gaitBlend = std::stof(value); }},
        {"pcVmRunLean", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.runLean = std::stof(value); }},
        {"pcVmIdleBlend", +[](SceneObject& obj, const std::string& value) { obj.playerViewMotion.idleBlend = std::stof(value); }},
        {"pcAudioEnabled", +[](SceneObject& obj, const std::string& value) { obj.playerMovementAudio.enabled = std::stoi(value) != 0; }},
        {"pcAudioJumpClip", +[](SceneObject& obj, const std::string& value) { obj.playerMovementAudio.jumpClip = value; }},
        {"pcAudioLandClip", +[](SceneObject& obj, const std::string& value) { obj.playerMovementAudio.landClip = value; }},
        {"pcAudioVolume", +[](SceneObject& obj, const std::string& value) { obj.playerMovementAudio.volume = std::stof(value); }},
        {"pcAudioRunVolumeScale", +[](SceneObject& obj, const std::string& value) { obj.playerMovementAudio.runVolumeScale = std::stof(value); }},
        {"pcAudioLandVolumeScale", +[](SceneObject& obj, const std::string& value) { obj.playerMovementAudio.landVolumeScale = std::stof(value); }},
        {"pcAudioPitchVariance", +[](SceneObject& obj, const std::string& value) { obj.playerMovementAudio.pitchVariance = std::stof(value); }},
        {"pcAudioStepCount", +[](SceneObject& obj, const std::string& value) {
             obj.playerMovementAudio.footstepClips.resize(std::max(0, std::stoi(value)));
         }},
        {"hasAudioSource", +[](SceneObject& obj, const std::string& value) { obj.hasAudioSource = std::stoi(value) != 0; }},
        {"audioEnabled", +[](SceneObject& obj, const std::string& value) { obj.audioSource.enabled = std::stoi(value) != 0; }},
        {"audioClip", +[](SceneObject& obj, const std::string& value) { obj.audioSource.clipPath = value; }},
        {"audioVolume", +[](SceneObject& obj, const std::string& value) { obj.audioSource.volume = std::stof(value); }},
        {"audioLoop", +[](SceneObject& obj, const std::string& value) { obj.audioSource.loop = std::stoi(value) != 0; }},
        {"audioPlayOnStart", +[](SceneObject& obj, const std::string& value) { obj.audioSource.playOnStart = std::stoi(value) != 0; }},
        {"audioSpatial", +[](SceneObject& obj, const std::string& value) {
            obj.audioSource.spatial = std::stoi(value) != 0;
            obj.audioSource.spatialBlend = obj.audioSource.spatial ? 1.0f : 0.0f;
        }},
        {"audioSpatialBlend", +[](SceneObject& obj, const std::string& value) {
            obj.audioSource.spatialBlend = std::clamp(std::stof(value), 0.0f, 1.0f);
            obj.audioSource.spatial = obj.audioSource.spatialBlend > 0.001f;
        }},
        {"audioMinDistance", +[](SceneObject& obj, const std::string& value) { obj.audioSource.minDistance = std::stof(value); }},
        {"audioMaxDistance", +[](SceneObject& obj, const std::string& value) { obj.audioSource.maxDistance = std::stof(value); }},
        {"audioRolloffMode", +[](SceneObject& obj, const std::string& value) { obj.audioSource.rolloffMode = static_cast<AudioRolloffMode>(std::stoi(value)); }},
        {"audioRolloff", +[](SceneObject& obj, const std::string& value) { obj.audioSource.rolloff = std::stof(value); }},
        {"audioCustomMidDistance", +[](SceneObject& obj, const std::string& value) { obj.audioSource.customMidDistance = std::stof(value); }},
        {"audioCustomMidGain", +[](SceneObject& obj, const std::string& value) { obj.audioSource.customMidGain = std::stof(value); }},
        {"audioCustomEndGain", +[](SceneObject& obj, const std::string& value) { obj.audioSource.customEndGain = std::stof(value); }},
        {"hasAudioFX", +[](SceneObject& obj, const std::string& value) { obj.hasAudioFX = std::stoi(value) != 0; }},
        {"afxEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasAudioFX = true; obj.audioFX.enabled = std::stoi(value) != 0; }},
        {"afxGlobal", +[](SceneObject& obj, const std::string& value) { obj.hasAudioFX = true; obj.audioFX.global = std::stoi(value) != 0; }},
        {"afxChain", +[](SceneObject& obj, const std::string& value) { obj.hasAudioFX = true; obj.audioFX.chain = AudioFXParse(value); }},
        {"hasVideoPlayer", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = std::stoi(value) != 0; }},
        {"videoEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = true; obj.videoPlayer.enabled = std::stoi(value) != 0; }},
        {"videoPath", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = true; obj.videoPlayer.videoPath = value; }},
        {"videoPlayOnAwake", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = true; obj.videoPlayer.playOnAwake = std::stoi(value) != 0; }},
        {"videoLoop", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = true; obj.videoPlayer.loop = std::stoi(value) != 0; }},
        {"videoFlipX", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = true; obj.videoPlayer.flipX = std::stoi(value) != 0; }},
        {"videoFlipY", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = true; obj.videoPlayer.flipY = std::stoi(value) != 0; }},
        {"videoPlaybackSpeed", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = true; obj.videoPlayer.playbackSpeed = std::stof(value); }},
        {"videoPlayAudioFromVideo", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = true; obj.videoPlayer.playAudioFromVideo = std::stoi(value) != 0; }},
        {"videoRouteAudioToSource", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = true; obj.videoPlayer.routeAudioToSource = std::stoi(value) != 0; }},
        {"videoOutputAudioSourceObjectId", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = true; obj.videoPlayer.outputAudioSourceObjectId = std::stoi(value); }},
        {"videoAudioVolume", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = true; obj.videoPlayer.videoAudioVolume = std::stof(value); }},
        {"videoAudioMuted", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = true; obj.videoPlayer.videoAudioMuted = std::stoi(value) != 0; }},
        {"videoSyncAudioToVideo", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = true; obj.videoPlayer.syncAudioToVideo = std::stoi(value) != 0; }},
        {"videoAudioSyncTolerance", +[](SceneObject& obj, const std::string& value) { obj.hasVideoPlayer = true; obj.videoPlayer.audioSyncTolerance = std::stof(value); }},
        {"hasParticleSystem2D", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = std::stoi(value) != 0; }},
        {"ps2dEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.enabled = std::stoi(value) != 0; }},
        {"ps2dLooping", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.looping = std::stoi(value) != 0; }},
        {"ps2dPrewarm", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.prewarm = std::stoi(value) != 0; }},
        {"ps2dPlayOnAwake", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.playOnAwake = std::stoi(value) != 0; obj.particleSystem2D.playing = obj.particleSystem2D.playOnAwake; }},
        {"ps2dAutoRandomSeed", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.autoRandomSeed = std::stoi(value) != 0; }},
        {"ps2dRandomSeed", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.randomSeed = static_cast<uint32_t>(std::stoul(value)); }},
        {"ps2dStartDelay", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.startDelay = std::stof(value); }},
        {"ps2dStartLifetime", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; ParseMinMaxFloat(value, obj.particleSystem2D.startLifetime); }},
        {"ps2dStartSpeed", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; ParseMinMaxFloat(value, obj.particleSystem2D.startSpeed); }},
        {"ps2dStartSize", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; ParseMinMaxFloat(value, obj.particleSystem2D.startSize); }},
        {"ps2dStartRotation", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; ParseMinMaxFloat(value, obj.particleSystem2D.startRotation); }},
        {"ps2dStartColor", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; ParseVec4(value, obj.particleSystem2D.startColor); }},
        {"ps2dGravity", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.gravityModifier = std::stof(value); }},
        {"ps2dSimulationSpeed", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.simulationSpeed = std::stof(value); }},
        {"ps2dMaxParticles", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.maxParticles = std::max(1, std::stoi(value)); }},
        {"ps2dEmissionRate", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.emissionRate = std::max(0.0f, std::stof(value)); }},
        {"ps2dBurstCount", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.burstCount = std::max(0, std::stoi(value)); }},
        {"ps2dBurstTime", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.burstTime = std::max(0.0f, std::stof(value)); }},
        {"ps2dBurstLoop", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.burstLoop = std::stoi(value) != 0; }},
        {"ps2dShape", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.shape = std::clamp(std::stoi(value), 0, 2); }},
        {"ps2dShapeRadius", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.shapeRadius = std::max(0.0f, std::stof(value)); }},
        {"ps2dShapeBox", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; ParseVec3(value, obj.particleSystem2D.shapeBox); }},
        {"ps2dEmitIn3D", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.emitIn3D = std::stoi(value) != 0; }},
        {"ps2dVelocityOverLifetimeEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.velocityOverLifetimeEnabled = std::stoi(value) != 0; }},
        {"ps2dVelocityOverLifetime", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; ParseVec3(value, obj.particleSystem2D.velocityOverLifetime); }},
        {"ps2dColorOverLifetimeEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.colorOverLifetimeEnabled = std::stoi(value) != 0; }},
        {"ps2dColorOverLifetime", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; ParseVec4(value, obj.particleSystem2D.colorOverLifetime); }},
        {"ps2dSizeOverLifetimeEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.sizeOverLifetimeEnabled = std::stoi(value) != 0; }},
        {"ps2dSizeOverLifetime", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.sizeOverLifetime = std::stof(value); }},
        {"ps2dRotationOverLifetimeEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.rotationOverLifetimeEnabled = std::stoi(value) != 0; }},
        {"ps2dRotationOverLifetime", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.rotationOverLifetime = std::stof(value); }},
        {"ps2dNoiseEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.noiseEnabled = std::stoi(value) != 0; }},
        {"ps2dNoiseStrength", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.noiseStrength = std::stof(value); }},
        {"ps2dNoiseFrequency", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.noiseFrequency = std::stof(value); }},
        {"ps2dTexture", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.texturePath = value; }},
        {"ps2dMaterial", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.materialPath = value; }},
        {"ps2dReceiveLighting2D", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.receiveLighting2D = std::stoi(value) != 0; }},
        {"ps2dUnlitLighting2D", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.unlitLighting2D = std::stoi(value) != 0; }},
        {"ps2dEmissiveLighting2D", +[](SceneObject& obj, const std::string& value) { obj.hasParticleSystem2D = true; obj.particleSystem2D.emissiveLighting2D = std::stof(value); }},
        {"hasReverbZone", +[](SceneObject& obj, const std::string& value) { obj.hasReverbZone = std::stoi(value) != 0; }},
        {"reverbEnabled", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.enabled = std::stoi(value) != 0; }},
        {"reverbPreset", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.preset = static_cast<ReverbPreset>(std::stoi(value)); }},
        {"reverbShape", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.shape = static_cast<ReverbZoneShape>(std::stoi(value)); }},
        {"reverbBox", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.reverbZone.boxSize); }},
        {"reverbRadius", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.radius = std::stof(value); }},
        {"reverbBlend", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.blendDistance = std::stof(value); }},
        {"reverbMinDistance", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.minDistance = std::stof(value); }},
        {"reverbMaxDistance", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.maxDistance = std::stof(value); }},
        {"reverbRoom", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.room = std::stof(value); }},
        {"reverbRoomHF", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.roomHF = std::stof(value); }},
        {"reverbRoomLF", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.roomLF = std::stof(value); }},
        {"reverbDecayTime", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.decayTime = std::stof(value); }},
        {"reverbDecayHFRatio", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.decayHFRatio = std::stof(value); }},
        {"reverbReflections", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.reflections = std::stof(value); }},
        {"reverbReflectionsDelay", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.reflectionsDelay = std::stof(value); }},
        {"reverbReverb", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.reverb = std::stof(value); }},
        {"reverbReverbDelay", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.reverbDelay = std::stof(value); }},
        {"reverbHFReference", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.hfReference = std::stof(value); }},
        {"reverbLFReference", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.lfReference = std::stof(value); }},
        {"reverbRoomRolloffFactor", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.roomRolloffFactor = std::stof(value); }},
        {"reverbDiffusion", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.diffusion = std::stof(value); }},
        {"reverbDensity", +[](SceneObject& obj, const std::string& value) { obj.reverbZone.density = std::stof(value); }},
        {"hasGroundBakedType", +[](SceneObject& obj, const std::string& value) { obj.hasGroundBakedType = std::stoi(value) != 0; }},
        {"groundBakedEnabled", +[](SceneObject& obj, const std::string& value) { obj.groundBakedType.enabled = std::stoi(value) != 0; }},
        {"groundBakedInclude", +[](SceneObject& obj, const std::string& value) { obj.groundBakedType.includeInBake = std::stoi(value) != 0; }},
        {"groundBakedAreaCost", +[](SceneObject& obj, const std::string& value) { obj.groundBakedType.areaCost = std::stof(value); }},
        {"hasObsticleObject", +[](SceneObject& obj, const std::string& value) { obj.hasObsticleObject = std::stoi(value) != 0; }},
        {"obsticleEnabled", +[](SceneObject& obj, const std::string& value) { obj.obsticleObject.enabled = std::stoi(value) != 0; }},
        {"obsticleCarve", +[](SceneObject& obj, const std::string& value) { obj.obsticleObject.carve = std::stoi(value) != 0; }},
        {"obsticlePadding", +[](SceneObject& obj, const std::string& value) { obj.obsticleObject.padding = std::stof(value); }},
        {"hasAIAgent", +[](SceneObject& obj, const std::string& value) { obj.hasAIAgent = std::stoi(value) != 0; }},
        {"aiAgentEnabled", +[](SceneObject& obj, const std::string& value) { obj.aiAgent.enabled = std::stoi(value) != 0; }},
        {"aiAgentUseTargetObject", +[](SceneObject& obj, const std::string& value) { obj.aiAgent.useTargetObject = std::stoi(value) != 0; }},
        {"aiAgentTargetId", +[](SceneObject& obj, const std::string& value) { obj.aiAgent.targetId = std::stoi(value); }},
        {"aiAgentDestination", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.aiAgent.destination); }},
        {"aiAgentSpeed", +[](SceneObject& obj, const std::string& value) { obj.aiAgent.speed = std::stof(value); }},
        {"aiAgentStoppingDistance", +[](SceneObject& obj, const std::string& value) { obj.aiAgent.stoppingDistance = std::stof(value); }},
        {"aiAgentRepathInterval", +[](SceneObject& obj, const std::string& value) { obj.aiAgent.repathInterval = std::stof(value); }},
        {"aiAgentAutoRepath", +[](SceneObject& obj, const std::string& value) { obj.aiAgent.autoRepath = std::stoi(value) != 0; }},
        {"aiAgentAlignToPath", +[](SceneObject& obj, const std::string& value) { obj.aiAgent.alignToPath = std::stoi(value) != 0; }},
        {"aiAgentDebugDrawPath", +[](SceneObject& obj, const std::string& value) { obj.aiAgent.debugDrawPath = std::stoi(value) != 0; }},
        {"aiAgentTurnSpeed", +[](SceneObject& obj, const std::string& value) { obj.aiAgent.turnSpeed = std::stof(value); }},
        {"aiAgentAvoidancePadding", +[](SceneObject& obj, const std::string& value) { obj.aiAgent.avoidancePadding = std::stof(value); }},
        {"hasOffMeshLink", +[](SceneObject& obj, const std::string& value) { obj.hasOffMeshLink = std::stoi(value) != 0; }},
        {"offMeshLinkEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasOffMeshLink = true; obj.offMeshLink.enabled = std::stoi(value) != 0; }},
        {"offMeshLinkStart", +[](SceneObject& obj, const std::string& value) { obj.hasOffMeshLink = true; ParseVec3(value, obj.offMeshLink.startPoint); }},
        {"offMeshLinkEnd", +[](SceneObject& obj, const std::string& value) { obj.hasOffMeshLink = true; ParseVec3(value, obj.offMeshLink.endPoint); }},
        {"offMeshLinkBidirectional", +[](SceneObject& obj, const std::string& value) { obj.hasOffMeshLink = true; obj.offMeshLink.bidirectional = std::stoi(value) != 0; }},
        {"offMeshLinkCostOverride", +[](SceneObject& obj, const std::string& value) { obj.hasOffMeshLink = true; obj.offMeshLink.costOverride = std::stof(value); }},
        {"hasRig25DRoot", +[](SceneObject& obj, const std::string& value) { obj.hasRig25DRoot = std::stoi(value) != 0; }},
        {"rig25dRootEnabled", +[](SceneObject& obj, const std::string& value) { obj.rig25DRoot.enabled = std::stoi(value) != 0; obj.hasRig25DRoot = true; }},
        {"hasRig25DNode", +[](SceneObject& obj, const std::string& value) { obj.hasRig25DNode = std::stoi(value) != 0; }},
        {"rig25dNodeEnabled", +[](SceneObject& obj, const std::string& value) { obj.rig25DNode.enabled = std::stoi(value) != 0; obj.hasRig25DNode = true; }},
        {"rig25dNodeId", +[](SceneObject& obj, const std::string& value) { obj.rig25DNode.nodeId = std::stoi(value); obj.hasRig25DNode = true; }},
        {"rig25dNodeName", +[](SceneObject& obj, const std::string& value) { obj.rig25DNode.nodeName = value; obj.hasRig25DNode = true; }},
        {"hasMapRoot", +[](SceneObject& obj, const std::string& value) { obj.hasMapRoot = std::stoi(value) != 0; }},
        {"mapRootEnabled", +[](SceneObject& obj, const std::string& value) { obj.mapRoot.enabled = std::stoi(value) != 0; obj.hasMapRoot = true; }},
        {"mapRootId", +[](SceneObject& obj, const std::string& value) { obj.mapRoot.mapId = value; obj.hasMapRoot = true; }},
        {"mapRootStartSector", +[](SceneObject& obj, const std::string& value) { obj.mapRoot.startSectorId = value; obj.hasMapRoot = true; }},
        {"mapRootActiveSector", +[](SceneObject& obj, const std::string& value) { obj.mapRoot.activeSectorId = value; obj.hasMapRoot = true; }},
        {"mapRootVisibilityMode", +[](SceneObject& obj, const std::string& value) { obj.mapRoot.sectorVisibilityMode = std::clamp(std::stoi(value), 0, 2); obj.hasMapRoot = true; }},
        {"mapRootNotes", +[](SceneObject& obj, const std::string& value) { obj.mapRoot.notes = value; obj.hasMapRoot = true; }},
        {"hasMapSector", +[](SceneObject& obj, const std::string& value) { obj.hasMapSector = std::stoi(value) != 0; }},
        {"mapSectorEnabled", +[](SceneObject& obj, const std::string& value) { obj.mapSector.enabled = std::stoi(value) != 0; obj.hasMapSector = true; }},
        {"mapSectorId", +[](SceneObject& obj, const std::string& value) { obj.mapSector.sectorId = value; obj.hasMapSector = true; }},
        {"mapSectorGraphPos", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.mapSector.graphPosition); obj.hasMapSector = true; }},
        {"mapSectorColor", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.mapSector.color); obj.hasMapSector = true; }},
        {"mapSectorUseCustomBounds", +[](SceneObject& obj, const std::string& value) { obj.mapSector.useCustomBounds = std::stoi(value) != 0; obj.hasMapSector = true; }},
        {"mapSectorBoundsCenter", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.mapSector.boundsCenter); obj.hasMapSector = true; }},
        {"mapSectorBoundsSize", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.mapSector.boundsSize); obj.hasMapSector = true; }},
        {"mapSectorStreamingTag", +[](SceneObject& obj, const std::string& value) { obj.mapSector.streamingTag = value; obj.hasMapSector = true; }},
        {"mapSectorNotes", +[](SceneObject& obj, const std::string& value) { obj.mapSector.notes = value; obj.hasMapSector = true; }},
        {"mapSectorEstimatedMemoryMB", +[](SceneObject& obj, const std::string& value) { obj.mapSector.estimatedMemoryMB = std::stof(value); obj.hasMapSector = true; }},
        {"hasMapTransition", +[](SceneObject& obj, const std::string& value) { obj.hasMapTransition = std::stoi(value) != 0; }},
        {"mapTransitionEnabled", +[](SceneObject& obj, const std::string& value) { obj.mapTransition.enabled = std::stoi(value) != 0; obj.hasMapTransition = true; }},
        {"mapTransitionId", +[](SceneObject& obj, const std::string& value) { obj.mapTransition.transitionId = value; obj.hasMapTransition = true; }},
        {"mapTransitionSourceSector", +[](SceneObject& obj, const std::string& value) { obj.mapTransition.sourceSectorId = value; obj.hasMapTransition = true; }},
        {"mapTransitionDestSector", +[](SceneObject& obj, const std::string& value) { obj.mapTransition.destinationSectorId = value; obj.hasMapTransition = true; }},
        {"mapTransitionSourcePortal", +[](SceneObject& obj, const std::string& value) { obj.mapTransition.sourcePortalId = value; obj.hasMapTransition = true; }},
        {"mapTransitionDestPortal", +[](SceneObject& obj, const std::string& value) { obj.mapTransition.destinationPortalId = value; obj.hasMapTransition = true; }},
        {"mapTransitionBidirectional", +[](SceneObject& obj, const std::string& value) { obj.mapTransition.bidirectional = std::stoi(value) != 0; obj.hasMapTransition = true; }},
        {"mapTransitionLocked", +[](SceneObject& obj, const std::string& value) { obj.mapTransition.locked = std::stoi(value) != 0; obj.hasMapTransition = true; }},
        {"mapTransitionCondition", +[](SceneObject& obj, const std::string& value) { obj.mapTransition.condition = value; obj.hasMapTransition = true; }},
        {"mapTransitionKind", +[](SceneObject& obj, const std::string& value) { obj.mapTransition.kind = static_cast<MapTransitionKind>(std::clamp(std::stoi(value), 0, 6)); obj.hasMapTransition = true; }},
        {"mapTransitionEditorLabel", +[](SceneObject& obj, const std::string& value) { obj.mapTransition.editorLabel = value; obj.hasMapTransition = true; }},
        {"mapTransitionHasEntry", +[](SceneObject& obj, const std::string& value) { obj.mapTransition.hasEntryTransform = std::stoi(value) != 0; obj.hasMapTransition = true; }},
        {"mapTransitionEntryPos", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.mapTransition.entryPosition); obj.hasMapTransition = true; }},
        {"mapTransitionEntryYaw", +[](SceneObject& obj, const std::string& value) { obj.mapTransition.entryYawDeg = std::stof(value); obj.hasMapTransition = true; }},
        {"hasXROrigin", +[](SceneObject& obj, const std::string& value) { obj.hasXROrigin = std::stoi(value) != 0; }},
        {"xrOriginEnabled", +[](SceneObject& obj, const std::string& value) { obj.xrOrigin.enabled = std::stoi(value) != 0; obj.hasXROrigin = true; }},
        {"xrOriginTrackingMode", +[](SceneObject& obj, const std::string& value) {
             obj.xrOrigin.trackingOriginMode = static_cast<XROriginComponent::Mode>(
                 std::clamp(std::stoi(value), 0, 2));
             obj.hasXROrigin = true;
         }},
        {"xrOriginRigScale", +[](SceneObject& obj, const std::string& value) { obj.xrOrigin.rigScale = std::clamp(std::stof(value), 0.01f, 100.0f); obj.hasXROrigin = true; }},
        {"xrOriginCameraYOffset", +[](SceneObject& obj, const std::string& value) { obj.xrOrigin.cameraYOffset = std::stof(value); obj.hasXROrigin = true; }},

        {"hasXRCamera", +[](SceneObject& obj, const std::string& value) { obj.hasXRCamera = std::stoi(value) != 0; }},
        {"xrCameraEnabled", +[](SceneObject& obj, const std::string& value) { obj.xrCamera.enabled = std::stoi(value) != 0; obj.hasXRCamera = true; }},
        {"xrCameraTrackPosition", +[](SceneObject& obj, const std::string& value) { obj.xrCamera.trackPosition = std::stoi(value) != 0; obj.hasXRCamera = true; }},
        {"xrCameraTrackRotation", +[](SceneObject& obj, const std::string& value) { obj.xrCamera.trackRotation = std::stoi(value) != 0; obj.hasXRCamera = true; }},
        {"xrCameraApplyTracking", +[](SceneObject& obj, const std::string& value) { obj.xrCamera.applyTracking = std::stoi(value) != 0; obj.hasXRCamera = true; }},

        {"hasXRController", +[](SceneObject& obj, const std::string& value) { obj.hasXRController = std::stoi(value) != 0; }},
        {"xrControllerEnabled", +[](SceneObject& obj, const std::string& value) { obj.xrController.enabled = std::stoi(value) != 0; obj.hasXRController = true; }},
        {"xrControllerHand", +[](SceneObject& obj, const std::string& value) { obj.xrController.hand = (std::stoi(value) == 1) ? XRHand::Right : XRHand::Left; obj.hasXRController = true; }},
        {"xrControllerPoseSource", +[](SceneObject& obj, const std::string& value) { obj.xrController.poseSource = (std::stoi(value) == 1) ? XRControllerPoseSource::Aim : XRControllerPoseSource::Grip; obj.hasXRController = true; }},
        {"xrControllerTrackPosition", +[](SceneObject& obj, const std::string& value) { obj.xrController.trackPosition = std::stoi(value) != 0; obj.hasXRController = true; }},
        {"xrControllerTrackRotation", +[](SceneObject& obj, const std::string& value) { obj.xrController.trackRotation = std::stoi(value) != 0; obj.hasXRController = true; }},
        {"xrControllerHideWhenNotTracked", +[](SceneObject& obj, const std::string& value) { obj.xrController.hideWhenNotTracked = std::stoi(value) != 0; obj.hasXRController = true; }},

        {"hasXRActionBasedController", +[](SceneObject& obj, const std::string& value) { obj.hasXRActionBasedController = std::stoi(value) != 0; }},
        {"xrActionControllerEnabled", +[](SceneObject& obj, const std::string& value) { obj.xrActionBasedController.enabled = std::stoi(value) != 0; obj.hasXRActionBasedController = true; }},
        {"xrActionControllerHand", +[](SceneObject& obj, const std::string& value) { obj.xrActionBasedController.hand = (std::stoi(value) == 1) ? XRHand::Right : XRHand::Left; obj.hasXRActionBasedController = true; }},
        {"xrActionControllerSelect", +[](SceneObject& obj, const std::string& value) { obj.xrActionBasedController.selectButton = std::stoi(value); obj.hasXRActionBasedController = true; }},
        {"xrActionControllerActivate", +[](SceneObject& obj, const std::string& value) { obj.xrActionBasedController.activateButton = std::stoi(value); obj.hasXRActionBasedController = true; }},
        {"xrActionControllerUiPress", +[](SceneObject& obj, const std::string& value) { obj.xrActionBasedController.uiPressButton = std::stoi(value); obj.hasXRActionBasedController = true; }},
        {"xrActionControllerHaptics", +[](SceneObject& obj, const std::string& value) { obj.xrActionBasedController.enableHaptics = std::stoi(value) != 0; obj.hasXRActionBasedController = true; }},
        {"xrActionControllerHapticAmplitude", +[](SceneObject& obj, const std::string& value) { obj.xrActionBasedController.hapticAmplitude = std::clamp(std::stof(value), 0.0f, 1.0f); obj.hasXRActionBasedController = true; }},
        {"xrActionControllerHapticDuration", +[](SceneObject& obj, const std::string& value) { obj.xrActionBasedController.hapticDuration = std::clamp(std::stof(value), 0.0f, 5.0f); obj.hasXRActionBasedController = true; }},

        {"hasXRRayInteractor", +[](SceneObject& obj, const std::string& value) { obj.hasXRRayInteractor = std::stoi(value) != 0; }},
        {"xrRayEnabled", +[](SceneObject& obj, const std::string& value) { obj.xrRayInteractor.enabled = std::stoi(value) != 0; obj.hasXRRayInteractor = true; }},
        {"xrRayType", +[](SceneObject& obj, const std::string& value) { obj.xrRayInteractor.rayType = static_cast<XRRayInteractorComponent::RayType>(std::max(0, std::stoi(value))); obj.hasXRRayInteractor = true; }},
        {"xrRayMaxDistance", +[](SceneObject& obj, const std::string& value) { obj.xrRayInteractor.maxDistance = std::max(0.01f, std::stof(value)); obj.hasXRRayInteractor = true; }},
        {"xrRayInteractionMask", +[](SceneObject& obj, const std::string& value) { obj.xrRayInteractor.interactionMask = static_cast<uint32_t>(std::stoul(value)); obj.hasXRRayInteractor = true; }},
        {"xrRayUiInteraction", +[](SceneObject& obj, const std::string& value) { obj.xrRayInteractor.uiInteraction = std::stoi(value) != 0; obj.hasXRRayInteractor = true; }},
        {"xrRayLineVisual", +[](SceneObject& obj, const std::string& value) { obj.xrRayInteractor.showLineVisual = std::stoi(value) != 0; obj.hasXRRayInteractor = true; }},
        {"xrRayLineColor", +[](SceneObject& obj, const std::string& value) { ParseVec4(value, obj.xrRayInteractor.lineColor); obj.hasXRRayInteractor = true; }},

        {"hasXRDirectInteractor", +[](SceneObject& obj, const std::string& value) { obj.hasXRDirectInteractor = std::stoi(value) != 0; }},
        {"xrDirectEnabled", +[](SceneObject& obj, const std::string& value) { obj.xrDirectInteractor.enabled = std::stoi(value) != 0; obj.hasXRDirectInteractor = true; }},
        {"xrDirectRadius", +[](SceneObject& obj, const std::string& value) { obj.xrDirectInteractor.interactionRadius = std::max(0.001f, std::stof(value)); obj.hasXRDirectInteractor = true; }},
        {"xrDirectInteractionMask", +[](SceneObject& obj, const std::string& value) { obj.xrDirectInteractor.interactionMask = static_cast<uint32_t>(std::stoul(value)); obj.hasXRDirectInteractor = true; }},

        {"hasXRGrabInteractable", +[](SceneObject& obj, const std::string& value) { obj.hasXRGrabInteractable = std::stoi(value) != 0; }},
        {"xrGrabEnabled", +[](SceneObject& obj, const std::string& value) { obj.xrGrabInteractable.enabled = std::stoi(value) != 0; obj.hasXRGrabInteractable = true; }},
        {"xrGrabMovementType", +[](SceneObject& obj, const std::string& value) {
             obj.xrGrabInteractable.movementType = static_cast<XRGrabInteractableComponent::MovementType>(
                 std::clamp(std::stoi(value), 0, 2));
             obj.hasXRGrabInteractable = true;
         }},
        {"xrGrabAllowLeft", +[](SceneObject& obj, const std::string& value) { obj.xrGrabInteractable.allowLeftHand = std::stoi(value) != 0; obj.hasXRGrabInteractable = true; }},
        {"xrGrabAllowRight", +[](SceneObject& obj, const std::string& value) { obj.xrGrabInteractable.allowRightHand = std::stoi(value) != 0; obj.hasXRGrabInteractable = true; }},
        {"xrGrabAttachTransformId", +[](SceneObject& obj, const std::string& value) { obj.xrGrabInteractable.attachTransformId = std::stoi(value); obj.hasXRGrabInteractable = true; }},
        {"xrGrabTrackPosition", +[](SceneObject& obj, const std::string& value) { obj.xrGrabInteractable.trackPosition = std::stoi(value) != 0; obj.hasXRGrabInteractable = true; }},
        {"xrGrabTrackRotation", +[](SceneObject& obj, const std::string& value) { obj.xrGrabInteractable.trackRotation = std::stoi(value) != 0; obj.hasXRGrabInteractable = true; }},
        {"xrGrabThrowOnDetach", +[](SceneObject& obj, const std::string& value) { obj.xrGrabInteractable.throwOnDetach = std::stoi(value) != 0; obj.hasXRGrabInteractable = true; }},
        {"xrGrabThrowVelocityScale", +[](SceneObject& obj, const std::string& value) { obj.xrGrabInteractable.throwVelocityScale = std::stof(value); obj.hasXRGrabInteractable = true; }},
        {"xrGrabThrowAngularScale", +[](SceneObject& obj, const std::string& value) { obj.xrGrabInteractable.throwAngularVelocityScale = std::stof(value); obj.hasXRGrabInteractable = true; }},

        {"hasMapPortal", +[](SceneObject& obj, const std::string& value) { obj.hasMapPortal = std::stoi(value) != 0; }},
        {"mapPortalEnabled", +[](SceneObject& obj, const std::string& value) { obj.mapPortal.enabled = std::stoi(value) != 0; obj.hasMapPortal = true; }},
        {"mapPortalId", +[](SceneObject& obj, const std::string& value) { obj.mapPortal.portalId = value; obj.hasMapPortal = true; }},
        {"mapPortalTransition", +[](SceneObject& obj, const std::string& value) { obj.mapPortal.transitionId = value; obj.hasMapPortal = true; }},
        {"mapPortalSector", +[](SceneObject& obj, const std::string& value) { obj.mapPortal.sectorId = value; obj.hasMapPortal = true; }},
        {"mapPortalOpeningSize", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.mapPortal.openingSize); obj.hasMapPortal = true; }},
        {"hasMapMesh", +[](SceneObject& obj, const std::string& value) { obj.hasMapMesh = std::stoi(value) != 0; }},
        {"mapMeshEnabled", +[](SceneObject& obj, const std::string& value) { obj.mapMesh.enabled = std::stoi(value) != 0; obj.hasMapMesh = true; }},
        {"mapMeshGridSize", +[](SceneObject& obj, const std::string& value) { obj.mapMesh.gridSize = std::max(0.01f, std::stof(value)); obj.hasMapMesh = true; }},
        {"mapMeshSnapToGrid", +[](SceneObject& obj, const std::string& value) { obj.mapMesh.snapToGrid = std::stoi(value) != 0; obj.hasMapMesh = true; }},
        {"mapMeshVertexSnapping", +[](SceneObject& obj, const std::string& value) { obj.mapMesh.vertexSnapping = std::stoi(value) != 0; obj.hasMapMesh = true; }},
        {"mapMeshSurfaceSnapping", +[](SceneObject& obj, const std::string& value) { obj.mapMesh.surfaceSnapping = std::stoi(value) != 0; obj.hasMapMesh = true; }},
        {"mapMeshAutoCollision", +[](SceneObject& obj, const std::string& value) { obj.mapMesh.autoCollision = std::stoi(value) != 0; obj.hasMapMesh = true; }},
        {"hasAnimation", +[](SceneObject& obj, const std::string& value) { obj.hasAnimation = std::stoi(value) != 0; }},
        {"animEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasAnimation = true; obj.animation.enabled = std::stoi(value) != 0; }},
        {"animClipAsset", +[](SceneObject& obj, const std::string& value) { obj.hasAnimation = true; obj.animation.clipAssetPath = value; }},
        {"animClipCount", +[](SceneObject& obj, const std::string& value) {
             obj.hasAnimation = true;
             int count = std::stoi(value);
             obj.animation.clips.resize(std::max(0, count));
         }},
        {"animActiveClipIndex", +[](SceneObject& obj, const std::string& value) {
             obj.hasAnimation = true;
             obj.animation.activeClipIndex = std::stoi(value);
         }},
        {"animClipLength", +[](SceneObject& obj, const std::string& value) { obj.hasAnimation = true; obj.animation.clipLength = std::stof(value); }},
        {"animPlaySpeed", +[](SceneObject& obj, const std::string& value) { obj.hasAnimation = true; obj.animation.playSpeed = std::stof(value); }},
        {"animLoop", +[](SceneObject& obj, const std::string& value) { obj.hasAnimation = true; obj.animation.loop = std::stoi(value) != 0; }},
        {"animPlayOnAwake", +[](SceneObject& obj, const std::string& value) { obj.hasAnimation = true; obj.animation.playOnAwake = std::stoi(value) != 0; }},
        {"animApplyOnScrub", +[](SceneObject& obj, const std::string& value) { obj.hasAnimation = true; obj.animation.applyOnScrub = std::stoi(value) != 0; }},
        {"animKeyCount", +[](SceneObject& obj, const std::string& value) {
             obj.hasAnimation = true;
             int count = std::stoi(value);
             obj.animation.keyframes.resize(std::max(0, count));
         }},
        {"animEventCount", +[](SceneObject& obj, const std::string& value) {
             obj.hasAnimation = true;
             int count = std::stoi(value);
             obj.animation.events.resize(std::max(0, count));
         }},
        {"animTrackCount", +[](SceneObject& obj, const std::string& value) {
             obj.hasAnimation = true;
             int count = std::stoi(value);
             obj.animation.tracks.resize(std::max(0, count));
         }},
        {"hasSkeletalAnimation", +[](SceneObject& obj, const std::string& value) { obj.hasSkeletalAnimation = std::stoi(value) != 0; }},
        {"skelEnabled", +[](SceneObject& obj, const std::string& value) { obj.skeletal.enabled = std::stoi(value) != 0; }},
        {"skelUseGpu", +[](SceneObject& obj, const std::string& value) { obj.skeletal.useGpuSkinning = std::stoi(value) != 0; }},
        {"skelAllowCpuFallback", +[](SceneObject& obj, const std::string& value) { obj.skeletal.allowCpuFallback = std::stoi(value) != 0; }},
        {"skelUseAnimation", +[](SceneObject& obj, const std::string& value) { obj.skeletal.useAnimation = std::stoi(value) != 0; }},
        {"skelClipIndex", +[](SceneObject& obj, const std::string& value) { obj.skeletal.clipIndex = std::stoi(value); }},
        {"skelPlaySpeed", +[](SceneObject& obj, const std::string& value) { obj.skeletal.playSpeed = std::stof(value); }},
        {"skelLoop", +[](SceneObject& obj, const std::string& value) { obj.skeletal.loop = std::stoi(value) != 0; }},
        {"skelMaxBones", +[](SceneObject& obj, const std::string& value) { obj.skeletal.maxBones = std::stoi(value); }},
        {"materialColor", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.material.color); }},
        {"materialAlpha", +[](SceneObject& obj, const std::string& value) { obj.material.alpha = std::clamp(std::stof(value), 0.0f, 1.0f); }},
        {"materialAmbient", +[](SceneObject& obj, const std::string& value) { obj.material.ambientStrength = std::stof(value); }},
        {"materialSpecular", +[](SceneObject& obj, const std::string& value) { obj.material.specularStrength = std::stof(value); }},
        {"materialShininess", +[](SceneObject& obj, const std::string& value) { obj.material.shininess = std::stof(value); }},
        {"materialNormalMapIntensity", +[](SceneObject& obj, const std::string& value) { obj.material.normalMapIntensity = std::clamp(std::stof(value), 0.0f, 2.0f); }},
        {"materialTextureMix", +[](SceneObject& obj, const std::string& value) { obj.material.textureMix = std::stof(value); }},
        {"materialUvTiling", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.material.uvTiling); }},
        {"materialUvOffset", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.material.uvOffset); }},
        {"materialScrollSpeed", +[](SceneObject& obj, const std::string& value) { obj.material.scrollSpeed = std::stof(value); }},
        {"materialScrollDirection", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.material.scrollDirection); }},
        {"materialUvScrollEnabled", +[](SceneObject& obj, const std::string& value) { obj.material.uvScrollEnabled = ParseProjectBool(value); }},
        {"materialCloudColor", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.material.cloudColor); }},
        {"materialCloudSkyColor", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.material.cloudSkyColor); }},
        {"materialCloudScale", +[](SceneObject& obj, const std::string& value) { obj.material.cloudScale = std::max(0.0001f, std::stof(value)); }},
        {"materialCloudCoverage", +[](SceneObject& obj, const std::string& value) { obj.material.cloudCoverage = std::clamp(std::stof(value), 0.0f, 1.0f); }},
        {"materialCloudSoftness", +[](SceneObject& obj, const std::string& value) { obj.material.cloudSoftness = std::clamp(std::stof(value), 0.001f, 1.0f); }},
        {"materialCloudDetail", +[](SceneObject& obj, const std::string& value) { obj.material.cloudDetail = std::clamp(std::stoi(value), 1, 8); }},
        {"materialCloudSpeed", +[](SceneObject& obj, const std::string& value) { obj.material.cloudSpeed = std::stof(value); }},
        {"materialCloudWarp", +[](SceneObject& obj, const std::string& value) { obj.material.cloudWarp = std::max(0.0f, std::stof(value)); }},
        {"materialCloudHighlight", +[](SceneObject& obj, const std::string& value) { obj.material.cloudHighlight = std::max(0.0f, std::stof(value)); }},
        {"materialCloudStars", +[](SceneObject& obj, const std::string& value) { obj.material.cloudStars = std::clamp(std::stof(value), 0.0f, 1.0f); }},
        {"materialCloudHorizon", +[](SceneObject& obj, const std::string& value) { obj.material.cloudHorizon = std::clamp(std::stof(value), 0.0f, 1.0f); }},
        {"materialPbrEnabled", +[](SceneObject& obj, const std::string& value) { obj.material.pbrEnabled = ParseProjectBool(value); }},
        {"materialMetallic", +[](SceneObject& obj, const std::string& value) { obj.material.metallic = std::clamp(std::stof(value), 0.0f, 1.0f); }},
        {"materialRoughness", +[](SceneObject& obj, const std::string& value) { obj.material.roughness = std::clamp(std::stof(value), 0.0f, 1.0f); }},
        {"materialOcclusionStrength", +[](SceneObject& obj, const std::string& value) { obj.material.occlusionStrength = std::clamp(std::stof(value), 0.0f, 1.0f); }},
        {"materialEmissionColor", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.material.emissionColor); }},
        {"materialEmissionStrength", +[](SceneObject& obj, const std::string& value) { obj.material.emissionStrength = std::max(0.0f, std::stof(value)); }},
        {"materialAlphaCutoff", +[](SceneObject& obj, const std::string& value) { obj.material.alphaCutoff = std::clamp(std::stof(value), 0.0f, 1.0f); }},
        {"materialHeightScale", +[](SceneObject& obj, const std::string& value) { obj.material.heightScale = std::clamp(std::stof(value), 0.0f, 1.0f); }},
        {"materialParallaxSteps", +[](SceneObject& obj, const std::string& value) { obj.material.parallaxSteps = std::clamp(std::stoi(value), 4, 256); }},
        {"materialParallaxInvertHeight", +[](SceneObject& obj, const std::string& value) { obj.material.parallaxInvertHeight = ParseProjectBool(value); }},
        {"heightMap", +[](SceneObject& obj, const std::string& value) { obj.heightMapPath = value; }},
        {"metallicRoughnessMap", +[](SceneObject& obj, const std::string& value) { obj.metallicRoughnessMapPath = value; }},
        {"metallicMap", +[](SceneObject& obj, const std::string& value) { obj.metallicMapPath = value; }},
        {"roughnessMap", +[](SceneObject& obj, const std::string& value) { obj.roughnessMapPath = value; }},
        {"opacityMap", +[](SceneObject& obj, const std::string& value) { obj.opacityMapPath = value; }},
        {"emissiveMap", +[](SceneObject& obj, const std::string& value) { obj.emissiveMapPath = value; }},
        {"occlusionMap", +[](SceneObject& obj, const std::string& value) { obj.occlusionMapPath = value; }},
        {"materialTextureFilter", +[](SceneObject& obj, const std::string& value) {
             int filterValue = std::stoi(value);
             obj.material.textureFilter = (filterValue == 1)
                 ? MaterialProperties::TextureFilter::Point
                 : MaterialProperties::TextureFilter::Bilinear;
         }},
        {"materialPath", +[](SceneObject& obj, const std::string& value) { obj.materialPath = value; }},
        {"albedoTex", +[](SceneObject& obj, const std::string& value) { obj.albedoTexturePath = value; }},
        {"overlayTex", +[](SceneObject& obj, const std::string& value) { obj.overlayTexturePath = value; }},
        {"normalMap", +[](SceneObject& obj, const std::string& value) { obj.normalMapPath = value; }},
        {"shaderPack", +[](SceneObject& obj, const std::string& value) { obj.shaderPackPath = value; }},
        {"vertexShader", +[](SceneObject& obj, const std::string& value) { obj.vertexShaderPath = value; }},
        {"fragmentShader", +[](SceneObject& obj, const std::string& value) { obj.fragmentShaderPath = value; }},
        {"useOverlay", +[](SceneObject& obj, const std::string& value) { obj.useOverlay = (std::stoi(value) != 0); }},
        {"additionalMaterialCount", +[](SceneObject& obj, const std::string& value) {
             int count = std::stoi(value);
             obj.additionalMaterialPaths.resize(std::max(0, count));
         }},
        {"nextInspectorScriptId", +[](SceneObject& obj, const std::string& value) {
             obj.nextInspectorScriptId = std::max(1, std::stoi(value));
         }},
        {"componentOrder", +[](SceneObject& obj, const std::string& value) {
             obj.inspectorComponentOrder.clear();
             std::stringstream ss(value);
             std::string item;
             while (std::getline(ss, item, ';')) {
                 if (!item.empty()) {
                     obj.inspectorComponentOrder.push_back(item);
                 }
             }
         }},
        {"scripts", +[](SceneObject& obj, const std::string& value) {
             int count = std::stoi(value);
             obj.scripts.resize(std::max(0, count));
         }},
        {"scriptCount", +[](SceneObject& obj, const std::string& value) {
             int count = std::stoi(value);
             obj.scripts.resize(std::max(0, count));
         }},
        {"lightColor", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.light.color); }},
        {"lightType", +[](SceneObject& obj, const std::string& value) {
             obj.light.type = static_cast<LightType>(std::stoi(value));
         }},
        {"lightIntensity", +[](SceneObject& obj, const std::string& value) { obj.light.intensity = std::stof(value); }},
        {"lightRange", +[](SceneObject& obj, const std::string& value) { obj.light.range = std::stof(value); }},
        {"lightEdgeFade", +[](SceneObject& obj, const std::string& value) { obj.light.edgeFade = std::stof(value); }},
        {"lightInner", +[](SceneObject& obj, const std::string& value) { obj.light.innerAngle = std::stof(value); }},
        {"lightOuter", +[](SceneObject& obj, const std::string& value) { obj.light.outerAngle = std::stof(value); }},
        {"lightSize", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.light.size); }},
        {"lightCastShadows", +[](SceneObject& obj, const std::string& value) { obj.light.castShadows = (std::stoi(value) != 0); }},
        {"lightSoftShadows", +[](SceneObject& obj, const std::string& value) { obj.light.softShadows = (std::stoi(value) != 0); }},
        {"lightShadowBias", +[](SceneObject& obj, const std::string& value) { obj.light.shadowBias = std::stof(value); }},
        {"lightShadowSoftness", +[](SceneObject& obj, const std::string& value) { obj.light.shadowSoftness = std::stof(value); }},
        {"lightShadowResolution", +[](SceneObject& obj, const std::string& value) { obj.light.shadowResolution = std::clamp(std::stoi(value), 0, 8192); }},
        {"lightEnabled", +[](SceneObject& obj, const std::string& value) { obj.light.enabled = (std::stoi(value) != 0); }},
        {"reflectionCastEnabled", +[](SceneObject& obj, const std::string& value) { obj.hasReflectionCast = true; obj.reflectionCast.enabled = std::stoi(value) != 0; }},
        {"reflectionCastUpdateMode", +[](SceneObject& obj, const std::string& value) {
             obj.hasReflectionCast = true;
             obj.reflectionCast.updateMode = std::stoi(value) == 0
                 ? ReflectionCastUpdateMode::EveryFrame
                 : ReflectionCastUpdateMode::FirstFrame;
         }},
        {"reflectionCastBox", +[](SceneObject& obj, const std::string& value) { obj.hasReflectionCast = true; ParseVec3(value, obj.reflectionCast.boxSize); }},
        {"reflectionCastBlend", +[](SceneObject& obj, const std::string& value) { obj.hasReflectionCast = true; obj.reflectionCast.blendDistance = std::max(0.0f, std::stof(value)); }},
        {"reflectionCastIntensity", +[](SceneObject& obj, const std::string& value) { obj.hasReflectionCast = true; obj.reflectionCast.intensity = std::clamp(std::stof(value), 0.0f, 2.0f); }},
        {"reflectionCastResolution", +[](SceneObject& obj, const std::string& value) { obj.hasReflectionCast = true; obj.reflectionCast.resolution = std::clamp(std::stoi(value), 32, 1024); }},
        {"light2dEnabled", +[](SceneObject& obj, const std::string& value) { obj.light2D.enabled = (std::stoi(value) != 0); obj.hasLight2D = true; }},
        {"light2dType", +[](SceneObject& obj, const std::string& value) { obj.light2D.type = static_cast<Light2DType>(std::stoi(value)); obj.hasLight2D = true; }},
        {"light2dColor", +[](SceneObject& obj, const std::string& value) { ParseVec4(value, obj.light2D.color); obj.hasLight2D = true; }},
        {"light2dIntensity", +[](SceneObject& obj, const std::string& value) { obj.light2D.intensity = std::stof(value); obj.hasLight2D = true; }},
        {"light2dRadius", +[](SceneObject& obj, const std::string& value) { obj.light2D.radius = std::max(0.0f, std::stof(value)); obj.hasLight2D = true; }},
        {"light2dInnerRadius", +[](SceneObject& obj, const std::string& value) { obj.light2D.innerRadius = std::max(0.0f, std::stof(value)); obj.hasLight2D = true; }},
        {"light2dOuterRadius", +[](SceneObject& obj, const std::string& value) { obj.light2D.outerRadius = std::max(0.0f, std::stof(value)); obj.hasLight2D = true; }},
        {"light2dFalloffStrength", +[](SceneObject& obj, const std::string& value) { obj.light2D.falloffStrength = std::max(0.0f, std::stof(value)); obj.hasLight2D = true; }},
        {"light2dInnerSpotAngle", +[](SceneObject& obj, const std::string& value) { obj.light2D.innerSpotAngle = std::clamp(std::stof(value), 0.0f, 360.0f); obj.hasLight2D = true; }},
        {"light2dOuterSpotAngle", +[](SceneObject& obj, const std::string& value) { obj.light2D.outerSpotAngle = std::clamp(std::stof(value), 0.0f, 360.0f); obj.hasLight2D = true; }},
        {"light2dBlendStyle", +[](SceneObject& obj, const std::string& value) { obj.light2D.blendStyle = std::stoi(value); obj.hasLight2D = true; }},
        {"light2dOrder", +[](SceneObject& obj, const std::string& value) { obj.light2D.lightOrder = std::stoi(value); obj.hasLight2D = true; }},
        {"light2dOverlap", +[](SceneObject& obj, const std::string& value) { obj.light2D.overlapOperation = static_cast<Light2DOverlapOperation>(std::stoi(value)); obj.hasLight2D = true; }},
        {"light2dShadowStrength", +[](SceneObject& obj, const std::string& value) { obj.light2D.shadowStrength = std::clamp(std::stof(value), 0.0f, 1.0f); obj.hasLight2D = true; }},
        {"light2dVolumetric", +[](SceneObject& obj, const std::string& value) { obj.light2D.volumetricEnabled = (std::stoi(value) != 0); obj.hasLight2D = true; }},
        {"light2dCastShadows", +[](SceneObject& obj, const std::string& value) { obj.light2D.castsShadows = (std::stoi(value) != 0); obj.hasLight2D = true; }},
        {"light2dTargetAllLayers", +[](SceneObject& obj, const std::string& value) { obj.light2D.targetAllLayers = (std::stoi(value) != 0); obj.hasLight2D = true; }},
        {"light2dTargetLayerMask", +[](SceneObject& obj, const std::string& value) { obj.light2D.targetLayerMask = static_cast<uint32_t>(std::stoul(value)); obj.hasLight2D = true; }},
        {"light2dCullOffscreen", +[](SceneObject& obj, const std::string& value) { obj.light2D.cullWhenOffscreen = (std::stoi(value) != 0); obj.hasLight2D = true; }},
        {"light2dOffscreenMargin", +[](SceneObject& obj, const std::string& value) { obj.light2D.offscreenCullMargin = std::max(0.0f, std::stof(value)); obj.hasLight2D = true; }},
        {"light2dNormalQuality", +[](SceneObject& obj, const std::string& value) { obj.light2D.normalMapQuality = static_cast<Light2DNormalMapQuality>(std::stoi(value)); obj.hasLight2D = true; }},
        {"light2dNormalDistance", +[](SceneObject& obj, const std::string& value) { obj.light2D.normalMapDistance = std::max(0.0f, std::stof(value)); obj.hasLight2D = true; }},
        {"light2dUseDistanceExponent", +[](SceneObject& obj, const std::string& value) { obj.light2D.useDistanceExponent = (std::stoi(value) != 0); obj.hasLight2D = true; }},
        {"light2dDistanceExponent", +[](SceneObject& obj, const std::string& value) { obj.light2D.distanceExponent = std::max(0.01f, std::stof(value)); obj.hasLight2D = true; }},
        {"light2dCookie", +[](SceneObject& obj, const std::string& value) { obj.light2D.cookieTexturePath = value; obj.hasLight2D = true; }},
        {"light2dCookieScale", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.light2D.cookieScale); obj.hasLight2D = true; }},
        {"light2dCookieRotation", +[](SceneObject& obj, const std::string& value) { obj.light2D.cookieRotation = std::stof(value); obj.hasLight2D = true; }},
        {"light2dFreeformFeather", +[](SceneObject& obj, const std::string& value) { obj.light2D.freeformFeather = std::max(0.0f, std::stof(value)); obj.hasLight2D = true; }},
        {"light2dFreeformEdgeFalloff", +[](SceneObject& obj, const std::string& value) { obj.light2D.freeformEdgeFalloff = std::max(0.01f, std::stof(value)); obj.hasLight2D = true; }},
        {"light2dFlickerEnabled", +[](SceneObject& obj, const std::string& value) { obj.light2D.flicker.enabled = (std::stoi(value) != 0); obj.hasLight2D = true; }},
        {"light2dFlickerSpeed", +[](SceneObject& obj, const std::string& value) { obj.light2D.flicker.speed = std::max(0.01f, std::stof(value)); obj.hasLight2D = true; }},
        {"light2dFlickerAmount", +[](SceneObject& obj, const std::string& value) { obj.light2D.flicker.amount = std::clamp(std::stof(value), 0.0f, 1.0f); obj.hasLight2D = true; }},
        {"light2dFlickerSeed", +[](SceneObject& obj, const std::string& value) { obj.light2D.flicker.seed = std::stof(value); obj.hasLight2D = true; }},
        {"light2dShape", +[](SceneObject& obj, const std::string& value) { ParseVec2List(value, obj.light2D.shapePoints); obj.hasLight2D = true; }},
        {"shadowCaster2dEnabled", +[](SceneObject& obj, const std::string& value) { obj.shadowCaster2D.enabled = (std::stoi(value) != 0); obj.hasShadowCaster2D = true; }},
        {"shadowCaster2dSelfShadow", +[](SceneObject& obj, const std::string& value) { obj.shadowCaster2D.castsSelfShadow = (std::stoi(value) != 0); obj.hasShadowCaster2D = true; }},
        {"shadowCaster2dTargetAllLayers", +[](SceneObject& obj, const std::string& value) { obj.shadowCaster2D.targetAllLayers = (std::stoi(value) != 0); obj.hasShadowCaster2D = true; }},
        {"shadowCaster2dTargetLayerMask", +[](SceneObject& obj, const std::string& value) { obj.shadowCaster2D.targetLayerMask = static_cast<uint32_t>(std::stoul(value)); obj.hasShadowCaster2D = true; }},
        {"shadowCaster2dStrength", +[](SceneObject& obj, const std::string& value) { obj.shadowCaster2D.shadowStrength = std::clamp(std::stof(value), 0.0f, 1.0f); obj.hasShadowCaster2D = true; }},
        {"shadowCaster2dShape", +[](SceneObject& obj, const std::string& value) { ParseVec2List(value, obj.shadowCaster2D.points); obj.hasShadowCaster2D = true; }},
        {"cameraType", +[](SceneObject& obj, const std::string& value) { obj.camera.type = static_cast<SceneCameraType>(std::stoi(value)); }},
        {"cameraFov", +[](SceneObject& obj, const std::string& value) { obj.camera.fov = std::stof(value); }},
        {"cameraNear", +[](SceneObject& obj, const std::string& value) { obj.camera.nearClip = std::stof(value); }},
        {"cameraFar", +[](SceneObject& obj, const std::string& value) { obj.camera.farClip = std::stof(value); }},
        {"cameraPostFX", +[](SceneObject& obj, const std::string& value) { obj.camera.applyPostFX = (std::stoi(value) != 0); }},
        {"cameraUse2D", +[](SceneObject& obj, const std::string& value) { obj.camera.use2D = (std::stoi(value) != 0); }},
        {"cameraPixelsPerUnit", +[](SceneObject& obj, const std::string& value) { obj.camera.pixelsPerUnit = std::stof(value); }},
        {"cameraProjection", +[](SceneObject& obj, const std::string& value) { obj.camera.projection = static_cast<SceneCameraProjection>(std::clamp(std::stoi(value), 0, 1)); }},
        {"cameraFovAxis", +[](SceneObject& obj, const std::string& value) { obj.camera.fovAxis = static_cast<SceneCameraFovAxis>(std::clamp(std::stoi(value), 0, 1)); }},
        {"cameraOrthoSize", +[](SceneObject& obj, const std::string& value) { obj.camera.orthoSize = std::max(0.01f, std::stof(value)); }},
        {"cameraRenderShadows", +[](SceneObject& obj, const std::string& value) { obj.camera.renderShadows = (std::stoi(value) != 0); }},
        {"cameraBackground", +[](SceneObject& obj, const std::string& value) { obj.camera.background = static_cast<SceneCameraBackground>(std::clamp(std::stoi(value), 0, 1)); }},
        {"cameraBackgroundColor", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.camera.backgroundColor); }},
        {"cameraCullingMask", +[](SceneObject& obj, const std::string& value) { obj.camera.cullingMask = static_cast<uint32_t>(std::stoul(value)); }},
        {"uiAnchor", +[](SceneObject& obj, const std::string& value) { obj.ui.anchor = static_cast<UIAnchor>(std::stoi(value)); }},
        {"uiPosition", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.ui.position); }},
        {"uiRotation", +[](SceneObject& obj, const std::string& value) { obj.ui.rotation = std::stof(value); }},
        {"uiSize", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.ui.size); }},
        {"uiMaskChildren", +[](SceneObject& obj, const std::string& value) { obj.ui.maskChildren = (std::stoi(value) != 0); }},
        {"uiSliderValue", +[](SceneObject& obj, const std::string& value) { obj.ui.sliderValue = std::stof(value); }},
        {"uiSliderMin", +[](SceneObject& obj, const std::string& value) { obj.ui.sliderMin = std::stof(value); }},
        {"uiSliderMax", +[](SceneObject& obj, const std::string& value) { obj.ui.sliderMax = std::stof(value); }},
        {"uiLabel", +[](SceneObject& obj, const std::string& value) { obj.ui.label = value; }},
        {"uiColor", +[](SceneObject& obj, const std::string& value) { ParseVec4(value, obj.ui.color); }},
        {"uiInteractable", +[](SceneObject& obj, const std::string& value) { obj.ui.interactable = (std::stoi(value) != 0); }},
        {"uiSliderStyle", +[](SceneObject& obj, const std::string& value) { obj.ui.sliderStyle = static_cast<UISliderStyle>(std::stoi(value)); }},
        {"uiButtonStyle", +[](SceneObject& obj, const std::string& value) { obj.ui.buttonStyle = static_cast<UIButtonStyle>(std::stoi(value)); }},
        {"uiStylePreset", +[](SceneObject& obj, const std::string& value) { obj.ui.stylePreset = value; }},
        {"uiTextScale", +[](SceneObject& obj, const std::string& value) { obj.ui.textScale = std::stof(value); }},
        {"uiTextFont", +[](SceneObject& obj, const std::string& value) { obj.ui.textFont = value; }},
        {"uiTextWrap", +[](SceneObject& obj, const std::string& value) { obj.ui.textAutoWrap = (std::stoi(value) != 0); }},
        {"uiTextHAlign", +[](SceneObject& obj, const std::string& value) {
             obj.ui.textHAlign = static_cast<UITextHAlign>(std::clamp(std::stoi(value), 0, 2));
         }},
        {"uiTextVAlign", +[](SceneObject& obj, const std::string& value) {
             obj.ui.textVAlign = static_cast<UITextVAlign>(std::clamp(std::stoi(value), 0, 2));
         }},
        {"uiTextEffectFlags", +[](SceneObject& obj, const std::string& value) { obj.ui.textEffectFlags = std::stoi(value); }},
        {"uiTextEffectSpeed", +[](SceneObject& obj, const std::string& value) { obj.ui.textEffectSpeed = std::max(0.01f, std::stof(value)); }},
        {"uiTextEffectIntensity", +[](SceneObject& obj, const std::string& value) { obj.ui.textEffectIntensity = std::max(0.0f, std::stof(value)); }},
        {"uiRenderIn3D", +[](SceneObject& obj, const std::string& value) { obj.ui.renderIn3D = (std::stoi(value) != 0); }},
        {"uiRenderTargetSize", +[](SceneObject& obj, const std::string& value) { ParseIVec2(value, obj.ui.renderTargetSize); }},
        {"uiRenderTargetFilter", +[](SceneObject& obj, const std::string& value) {
             obj.ui.renderTargetFilter = (std::stoi(value) == 1)
                 ? MaterialProperties::TextureFilter::Point
                 : MaterialProperties::TextureFilter::Bilinear;
         }},
        {"uiPseudo3DEnabled", +[](SceneObject& obj, const std::string& value) { obj.ui.pseudo3DEnabled = (std::stoi(value) != 0); }},
        {"uiPseudo3DUseOffscreen", +[](SceneObject& obj, const std::string& value) { obj.ui.pseudo3DUseOffscreenSurface = (std::stoi(value) != 0); }},
        {"uiPseudo3DPanelSize", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.ui.pseudo3DPanelSize); }},
        {"uiPseudo3DTopLeftOffset", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.ui.pseudo3DTopLeftOffset); }},
        {"uiPseudo3DTopRightOffset", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.ui.pseudo3DTopRightOffset); }},
        {"uiPseudo3DBottomRightOffset", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.ui.pseudo3DBottomRightOffset); }},
        {"uiPseudo3DBottomLeftOffset", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.ui.pseudo3DBottomLeftOffset); }},
        {"uiPseudo3DPivot", +[](SceneObject& obj, const std::string& value) {
             ParseVec2(value, obj.ui.pseudo3DPivot);
             obj.ui.pseudo3DPivot.x = std::clamp(obj.ui.pseudo3DPivot.x, 0.0f, 1.0f);
             obj.ui.pseudo3DPivot.y = std::clamp(obj.ui.pseudo3DPivot.y, 0.0f, 1.0f);
         }},
        {"uiPseudo3DPerspectiveIntensity", +[](SceneObject& obj, const std::string& value) { obj.ui.pseudo3DPerspectiveIntensity = std::stof(value); }},
        {"uiPseudo3DSkewAmount", +[](SceneObject& obj, const std::string& value) { obj.ui.pseudo3DSkewAmount = std::stof(value); }},
        {"uiPseudo3DCurvatureAmount", +[](SceneObject& obj, const std::string& value) { obj.ui.pseudo3DCurvatureAmount = std::stof(value); }},
        {"uiPseudo3DAnchorTargetId", +[](SceneObject& obj, const std::string& value) { obj.ui.pseudo3DAnchorTargetId = std::stoi(value); }},
        {"uiPseudo3DDistanceScaling", +[](SceneObject& obj, const std::string& value) { obj.ui.pseudo3DDistanceScalingEnabled = (std::stoi(value) != 0); }},
        {"uiPseudo3DAdjustPerspectiveDistance", +[](SceneObject& obj, const std::string& value) { obj.ui.pseudo3DAdjustPerspectiveWithDistance = (std::stoi(value) != 0); }},
        {"uiPseudo3DMinDistance", +[](SceneObject& obj, const std::string& value) { obj.ui.pseudo3DMinDistance = std::max(0.01f, std::stof(value)); }},
        {"uiPseudo3DMaxDistance", +[](SceneObject& obj, const std::string& value) { obj.ui.pseudo3DMaxDistance = std::max(0.02f, std::stof(value)); }},
        {"uiPseudo3DInteractionDistance", +[](SceneObject& obj, const std::string& value) { obj.ui.pseudo3DInteractionDistance = std::max(0.0f, std::stof(value)); }},
        {"uiPseudo3DDepthSort", +[](SceneObject& obj, const std::string& value) { obj.ui.pseudo3DDepthSort = std::stoi(value); }},
        {"uiPseudo3DAllowInteraction", +[](SceneObject& obj, const std::string& value) { obj.ui.pseudo3DAllowInteraction = (std::stoi(value) != 0); }},
        {"uiSpriteSheetEnabled", +[](SceneObject& obj, const std::string& value) { obj.ui.spriteSheetEnabled = (std::stoi(value) != 0); }},
        {"uiSpriteSheetGrid", +[](SceneObject& obj, const std::string& value) {
             glm::ivec2 grid(1, 1);
             ParseIVec2(value, grid);
             obj.ui.spriteSheetColumns = std::max(1, grid.x);
             obj.ui.spriteSheetRows = std::max(1, grid.y);
         }},
        {"uiSpriteSheetFrame", +[](SceneObject& obj, const std::string& value) { obj.ui.spriteSheetFrame = std::max(0, std::stoi(value)); }},
        {"uiSpriteSheetFps", +[](SceneObject& obj, const std::string& value) { obj.ui.spriteSheetFps = std::max(1.0f, std::stof(value)); }},
        {"uiSpriteSheetLoop", +[](SceneObject& obj, const std::string& value) { obj.ui.spriteSheetLoop = (std::stoi(value) != 0); }},
        {"uiSpriteCustomFramesEnabled", +[](SceneObject& obj, const std::string& value) { obj.ui.spriteCustomFramesEnabled = (std::stoi(value) != 0); }},
        {"uiSpriteSourceSize", +[](SceneObject& obj, const std::string& value) {
             glm::ivec2 size(0, 0);
             ParseIVec2(value, size);
             obj.ui.spriteSourceWidth = std::max(0, size.x);
             obj.ui.spriteSourceHeight = std::max(0, size.y);
         }},
        {"uiNineSliceEnabled", +[](SceneObject& obj, const std::string& value) {
             obj.ui.nineSliceEnabled = (std::stoi(value) != 0);
         }},
        {"uiNineSliceBorder", +[](SceneObject& obj, const std::string& value) {
             ParseVec4(value, obj.ui.nineSliceBorder);
             obj.ui.nineSliceBorder.x = std::max(0.0f, obj.ui.nineSliceBorder.x);
             obj.ui.nineSliceBorder.y = std::max(0.0f, obj.ui.nineSliceBorder.y);
             obj.ui.nineSliceBorder.z = std::max(0.0f, obj.ui.nineSliceBorder.z);
             obj.ui.nineSliceBorder.w = std::max(0.0f, obj.ui.nineSliceBorder.w);
         }},
        {"uiNineSliceTileEdges", +[](SceneObject& obj, const std::string& value) {
             obj.ui.nineSliceTileEdges = (std::stoi(value) != 0);
         }},
        {"uiNineSliceTileCenter", +[](SceneObject& obj, const std::string& value) {
             obj.ui.nineSliceTileCenter = (std::stoi(value) != 0);
         }},
        {"uiReceiveLighting2D", +[](SceneObject& obj, const std::string& value) {
             obj.ui.receiveLighting2D = (std::stoi(value) != 0);
         }},
        {"uiUnlitLighting2D", +[](SceneObject& obj, const std::string& value) {
             obj.ui.unlitLighting2D = (std::stoi(value) != 0);
         }},
        {"uiEmissiveLighting2D", +[](SceneObject& obj, const std::string& value) {
             obj.ui.emissiveLighting2D = std::max(0.0f, std::stof(value));
         }},
        {"uiFillColor", +[](SceneObject& obj, const std::string& value) { ParseVec4(value, obj.ui.fillColor); }},
        {"uiBackgroundColor", +[](SceneObject& obj, const std::string& value) { ParseVec4(value, obj.ui.backgroundColor); }},
        {"uiBorderColor", +[](SceneObject& obj, const std::string& value) { ParseVec4(value, obj.ui.borderColor); }},
        {"uiTextColor", +[](SceneObject& obj, const std::string& value) { ParseVec4(value, obj.ui.textColor); }},
        {"uiFontSize", +[](SceneObject& obj, const std::string& value) { obj.ui.fontSize = std::max(0.0f, std::stof(value)); }},
        {"uiSortingOrder", +[](SceneObject& obj, const std::string& value) { obj.ui.sortingOrder = std::stoi(value); }},
        {"uiBackdropBlur", +[](SceneObject& obj, const std::string& value) {
             obj.ui.backdropBlur = std::min(1.0f, std::max(0.0f, std::stof(value)));
         }},
        {"uiBackdropRounding", +[](SceneObject& obj, const std::string& value) {
             obj.ui.backdropRounding = std::max(0.0f, std::stof(value));
         }},
        {"uiSpriteCustomFrames", +[](SceneObject& obj, const std::string& value) {
             obj.ui.spriteCustomFrames.clear();
             std::stringstream ss(value);
             std::string item;
             while (std::getline(ss, item, ';')) {
                 if (item.empty()) continue;
                 glm::ivec4 rect(0);
                 if (std::sscanf(item.c_str(), "%d,%d,%d,%d", &rect.x, &rect.y, &rect.z, &rect.w) == 4) {
                     rect.z = std::max(1, rect.z);
                     rect.w = std::max(1, rect.w);
                     obj.ui.spriteCustomFrames.push_back(rect);
                 }
             }
         }},
        {"uiSpriteCustomFrameNames", +[](SceneObject& obj, const std::string& value) {
             obj.ui.spriteCustomFrameNames.clear();
             std::stringstream ss(value);
             std::string item;
             while (std::getline(ss, item, ';')) {
                 obj.ui.spriteCustomFrameNames.push_back(item);
             }
         }},
        {"uiSpriteCustomFrameScales", +[](SceneObject& obj, const std::string& value) {
             ParseVec2List(value, obj.ui.spriteCustomFrameScales);
             if (obj.ui.spriteCustomFrameScales.size() < obj.ui.spriteCustomFrames.size()) {
                 obj.ui.spriteCustomFrameScales.resize(obj.ui.spriteCustomFrames.size(), glm::vec2(1.0f));
             } else if (obj.ui.spriteCustomFrameScales.size() > obj.ui.spriteCustomFrames.size()) {
                 obj.ui.spriteCustomFrameScales.resize(obj.ui.spriteCustomFrames.size());
             }
             for (glm::vec2& scale : obj.ui.spriteCustomFrameScales) {
                 scale.x = std::max(0.01f, scale.x);
                 scale.y = std::max(0.01f, scale.y);
             }
         }},
        {"postEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.enabled = (std::stoi(value) != 0); }},
        {"postVolumeGlobal", +[](SceneObject& obj, const std::string& value) { obj.postFx.isGlobal = (std::stoi(value) != 0); }},
        {"postVolumePriority", +[](SceneObject& obj, const std::string& value) { obj.postFx.priority = std::stof(value); }},
        {"postVolumeWeight", +[](SceneObject& obj, const std::string& value) { obj.postFx.distortionPolarization = std::stof(value); }},
        {"postBlendWeight", +[](SceneObject& obj, const std::string& value) { obj.postFx.blendWeight = std::stof(value); }},
        {"postVolumeBlendRadius", +[](SceneObject& obj, const std::string& value) { obj.postFx.blendRadius = std::stof(value); }},
        {"postHDREnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.hdrEnabled = (std::stoi(value) != 0); }},
        {"postToneMapper", +[](SceneObject& obj, const std::string& value) { obj.postFx.toneMapper = static_cast<PostFXToneMapper>(std::stoi(value)); }},
        {"postWhitePoint", +[](SceneObject& obj, const std::string& value) { obj.postFx.whitePoint = std::stof(value); }},
        {"postGamma", +[](SceneObject& obj, const std::string& value) { obj.postFx.gamma = std::stof(value); }},
        {"postBloomEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.bloomEnabled = (std::stoi(value) != 0); }},
        {"postBloomThreshold", +[](SceneObject& obj, const std::string& value) { obj.postFx.bloomThreshold = std::stof(value); }},
        {"postBloomSoftKnee", +[](SceneObject& obj, const std::string& value) { obj.postFx.bloomSoftKnee = std::stof(value); }},
        {"postBloomIntensity", +[](SceneObject& obj, const std::string& value) { obj.postFx.bloomIntensity = std::stof(value); }},
        {"postBloomRadius", +[](SceneObject& obj, const std::string& value) { obj.postFx.bloomRadius = std::stof(value); }},
        {"postColorAdjustEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.colorAdjustEnabled = (std::stoi(value) != 0); }},
        {"postExposure", +[](SceneObject& obj, const std::string& value) { obj.postFx.exposure = std::stof(value); }},
        {"postEyeAdaptationEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.eyeAdaptationEnabled = ParseProjectBool(value); }},
        {"postEyeAdaptationMinEv", +[](SceneObject& obj, const std::string& value) { obj.postFx.eyeAdaptationMinEv = std::clamp(std::stof(value), -16.0f, 16.0f); }},
        {"postEyeAdaptationMaxEv", +[](SceneObject& obj, const std::string& value) { obj.postFx.eyeAdaptationMaxEv = std::clamp(std::stof(value), -16.0f, 16.0f); }},
        {"postEyeAdaptationSpeedUp", +[](SceneObject& obj, const std::string& value) { obj.postFx.eyeAdaptationSpeedUp = std::max(0.01f, std::stof(value)); }},
        {"postEyeAdaptationSpeedDown", +[](SceneObject& obj, const std::string& value) { obj.postFx.eyeAdaptationSpeedDown = std::max(0.01f, std::stof(value)); }},
        {"postEyeAdaptationKeyValue", +[](SceneObject& obj, const std::string& value) { obj.postFx.eyeAdaptationKeyValue = std::clamp(std::stof(value), 0.01f, 1.0f); }},
        {"postPurkinjeEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.purkinjeEnabled = ParseProjectBool(value); }},
        {"postPurkinjeStrength", +[](SceneObject& obj, const std::string& value) { obj.postFx.purkinjeStrength = std::clamp(std::stof(value), 0.0f, 1.0f); }},
        {"postPurkinjeThreshold", +[](SceneObject& obj, const std::string& value) { obj.postFx.purkinjeThreshold = std::max(0.0f, std::stof(value)); }},
        {"postPurkinjeRange", +[](SceneObject& obj, const std::string& value) { obj.postFx.purkinjeRange = std::max(0.0001f, std::stof(value)); }},
        {"postPurkinjeTint", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.postFx.purkinjeTint); }},
        {"postContrast", +[](SceneObject& obj, const std::string& value) { obj.postFx.contrast = std::stof(value); }},
        {"postSaturation", +[](SceneObject& obj, const std::string& value) { obj.postFx.saturation = std::stof(value); }},
        {"postColorFilter", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.postFx.colorFilter); }},
        {"postMotionBlurEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.motionBlurEnabled = (std::stoi(value) != 0); }},
        {"postMotionBlurStrength", +[](SceneObject& obj, const std::string& value) { obj.postFx.motionBlurStrength = std::stof(value); }},
        {"postMotionBlurThreshold", +[](SceneObject& obj, const std::string& value) { obj.postFx.motionBlurThreshold = std::stof(value); }},
        {"postMotionBlurClamp", +[](SceneObject& obj, const std::string& value) { obj.postFx.motionBlurClamp = std::stof(value); }},
        {"postVignetteEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.vignetteEnabled = (std::stoi(value) != 0); }},
        {"postLensFlareEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensFlareEnabled = (std::stoi(value) != 0); }},
        {"postLensFlareIntensity", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensFlareIntensity = std::stof(value); }},
        {"postLensFlareThreshold", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensFlareThreshold = std::stof(value); }},
        {"postLensFlareGhosts", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensFlareGhosts = std::stoi(value); }},
        {"postLensFlareGhostSpacing", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensFlareGhostSpacing = std::stof(value); }},
        {"postLensFlareHaloRadius", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensFlareHaloRadius = std::stof(value); }},
        {"postLensFlareStreak", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensFlareStreak = std::stof(value); }},
        {"postLensFlareTint", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.postFx.lensFlareTint); }},
        {"postLensFlareGhostsEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensFlareGhostsEnabled = (std::stoi(value) != 0); }},
        {"postLensFlareHaloEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensFlareHaloEnabled = (std::stoi(value) != 0); }},
        {"postLensFlareStreakEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensFlareStreakEnabled = (std::stoi(value) != 0); }},
        {"postLensFlareGlowEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensFlareGlowEnabled = (std::stoi(value) != 0); }},
        {"postLensFlareGlowSize", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensFlareGlowSize = std::stof(value); }},
        {"postLensFlareGlowIntensity", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensFlareGlowIntensity = std::stof(value); }},
        {"postVignetteIntensity", +[](SceneObject& obj, const std::string& value) { obj.postFx.vignetteIntensity = std::stof(value); }},
        {"postVignetteSmoothness", +[](SceneObject& obj, const std::string& value) { obj.postFx.vignetteSmoothness = std::stof(value); }},
        {"postChromaticEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.chromaticAberrationEnabled = (std::stoi(value) != 0); }},
        {"postChromaticAmount", +[](SceneObject& obj, const std::string& value) { obj.postFx.chromaticAmount = std::stof(value); }},
        {"postSharpenEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.sharpenEnabled = (std::stoi(value) != 0); }},
        {"postSharpenStrength", +[](SceneObject& obj, const std::string& value) { obj.postFx.sharpenStrength = std::stof(value); }},
        {"postAOEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.ambientOcclusionEnabled = (std::stoi(value) != 0); }},
        {"postAORadius", +[](SceneObject& obj, const std::string& value) { obj.postFx.aoRadius = std::stof(value); }},
        {"postAOStrength", +[](SceneObject& obj, const std::string& value) { obj.postFx.aoStrength = std::stof(value); }},
        {"postDitherEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.ditherEnabled = (std::stoi(value) != 0); }},
        {"postDitherIntensity", +[](SceneObject& obj, const std::string& value) { obj.postFx.ditherIntensity = std::stof(value); }},
        {"postDitherColorBits", +[](SceneObject& obj, const std::string& value) { obj.postFx.ditherColorBits = std::stoi(value); }},
        {"postDitherDarkAdjustment", +[](SceneObject& obj, const std::string& value) { obj.postFx.ditherDarkAdjustment = std::stof(value); }},
        {"postDitherPixelation", +[](SceneObject& obj, const std::string& value) { obj.postFx.ditherPixelation = std::stof(value); }},
        {"postDitherSize", +[](SceneObject& obj, const std::string& value) { obj.postFx.ditherSize = std::stof(value); }},
        {"postDitherContrast", +[](SceneObject& obj, const std::string& value) { obj.postFx.ditherContrast = std::stof(value); }},
        {"postDitherOffset", +[](SceneObject& obj, const std::string& value) { obj.postFx.ditherOffset = std::stof(value); }},
        {"postDitherPalette", +[](SceneObject& obj, const std::string& value) { obj.postFx.ditherPalette = static_cast<PostFXDitherPalette>(std::clamp(std::stoi(value), 0, 4)); }},
        {"postDitherPattern", +[](SceneObject& obj, const std::string& value) { obj.postFx.ditherPattern = static_cast<PostFXDitherPattern>(std::clamp(std::stoi(value), 0, 4)); }},
        {"postStaticEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.staticEnabled = (std::stoi(value) != 0); }},
        {"postStaticIntensity", +[](SceneObject& obj, const std::string& value) { obj.postFx.staticIntensity = std::stof(value); }},
        {"postStaticGrainScale", +[](SceneObject& obj, const std::string& value) { obj.postFx.staticGrainScale = std::stof(value); }},
        {"postStaticDarkAreaInfluence", +[](SceneObject& obj, const std::string& value) { obj.postFx.staticDarkAreaInfluence = std::stof(value); }},
        {"postStaticSpeed", +[](SceneObject& obj, const std::string& value) { obj.postFx.staticSpeed = std::stof(value); }},
        {"postStaticMonochrome", +[](SceneObject& obj, const std::string& value) { obj.postFx.staticMonochrome = (std::stoi(value) != 0); }},
        {"postStaticSparkle", +[](SceneObject& obj, const std::string& value) { obj.postFx.staticSparkle = std::stof(value); }},
        {"postStaticDistortionEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.staticDistortionEnabled = (std::stoi(value) != 0); }},
        {"postStaticDistortionHorizontalJitterAmount", +[](SceneObject& obj, const std::string& value) { obj.postFx.staticDistortionHorizontalJitterAmount = std::stof(value); }},
        {"postStaticDistortionLineDensity", +[](SceneObject& obj, const std::string& value) { obj.postFx.staticDistortionLineDensity = std::stof(value); }},
        {"postStaticDistortionGlitchFrequency", +[](SceneObject& obj, const std::string& value) { obj.postFx.staticDistortionGlitchFrequency = std::stof(value); }},
        {"postStaticDistortionStrength", +[](SceneObject& obj, const std::string& value) { obj.postFx.staticDistortionStrength = std::stof(value); }},
        {"postLensDistortionEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensDistortionEnabled = (std::stoi(value) != 0); }},
        {"postLensDistortionAmount", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensDistortionAmount = std::stof(value); }},
        {"postLensDistortionEdgeFalloff", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensDistortionEdgeFalloff = std::stof(value); }},
        {"postLensDistortionCenterOffset", +[](SceneObject& obj, const std::string& value) { ParseVec2(value, obj.postFx.lensDistortionCenterOffset); }},
        {"postLensDistortionEdgeVignetteEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensDistortionEdgeVignetteEnabled = (std::stoi(value) != 0); }},
        {"postLensDistortionEdgeVignetteIntensity", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensDistortionEdgeVignetteIntensity = std::stof(value); }},
        {"postLensDistortionEdgeVignetteRadius", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensDistortionEdgeVignetteRadius = std::stof(value); }},
        {"postLensDistortionEdgeVignetteSoftness", +[](SceneObject& obj, const std::string& value) { obj.postFx.lensDistortionEdgeVignetteSoftness = std::stof(value); }},
        {"postLensDistortionEdgeVignetteColor", +[](SceneObject& obj, const std::string& value) { ParseVec3(value, obj.postFx.lensDistortionEdgeVignetteColor); }},
        {"postPixelationEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.pixelationEnabled = (std::stoi(value) != 0); }},
        {"postPixelationSize", +[](SceneObject& obj, const std::string& value) { obj.postFx.pixelationSize = std::stof(value); }},
        {"postPosterizeEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.posterizeEnabled = (std::stoi(value) != 0); }},
        {"postPosterizeLevels", +[](SceneObject& obj, const std::string& value) { obj.postFx.posterizeLevels = std::clamp(std::stoi(value), 2, 64); }},
        {"postScanlinesEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.scanlinesEnabled = (std::stoi(value) != 0); }},
        {"postScanlinesIntensity", +[](SceneObject& obj, const std::string& value) { obj.postFx.scanlinesIntensity = std::stof(value); }},
        {"postScanlinesDensity", +[](SceneObject& obj, const std::string& value) { obj.postFx.scanlinesDensity = std::stof(value); }},
        {"postScanlinesSpeed", +[](SceneObject& obj, const std::string& value) { obj.postFx.scanlinesSpeed = std::stof(value); }},
        {"postVHSOverlayEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.vhsOverlayEnabled = (std::stoi(value) != 0); }},
        {"postVHSOverlayOpacity", +[](SceneObject& obj, const std::string& value) { obj.postFx.vhsOverlayOpacity = std::stof(value); }},
        {"postVHSOverlayScanlineStrength", +[](SceneObject& obj, const std::string& value) { obj.postFx.vhsOverlayScanlineStrength = std::stof(value); }},
        {"postVHSOverlayTapeNoise", +[](SceneObject& obj, const std::string& value) { obj.postFx.vhsOverlayTapeNoise = std::stof(value); }},
        {"postVHSOverlayChromaBleed", +[](SceneObject& obj, const std::string& value) { obj.postFx.vhsOverlayChromaBleed = std::stof(value); }},
        {"postVHSOverlayBottomNoiseBandHeight", +[](SceneObject& obj, const std::string& value) { obj.postFx.vhsOverlayBottomNoiseBandHeight = std::stof(value); }},
        {"postVHSOverlayBottomNoiseBandIntensity", +[](SceneObject& obj, const std::string& value) { obj.postFx.vhsOverlayBottomNoiseBandIntensity = std::stof(value); }},
        {"postVHSOverlayDistortionStrength", +[](SceneObject& obj, const std::string& value) { obj.postFx.vhsOverlayDistortionStrength = std::stof(value); }},
        {"postVHSOverlayAnimationSpeed", +[](SceneObject& obj, const std::string& value) { obj.postFx.vhsOverlayAnimationSpeed = std::stof(value); }},
        {"postVHSOverlayColorBleed", +[](SceneObject& obj, const std::string& value) { obj.postFx.vhsOverlayColorBleed = std::stof(value); }},
        {"postVHSOverlayBanding", +[](SceneObject& obj, const std::string& value) { obj.postFx.vhsOverlayBanding = std::stof(value); }},
        {"postVHSOverlaySignalMode", +[](SceneObject& obj, const std::string& value) { obj.postFx.vhsOverlaySignalMode = static_cast<PostFXVhsSignalMode>(std::clamp(std::stoi(value), 0, 5)); }},
        {"postVHSOverlayDropouts", +[](SceneObject& obj, const std::string& value) { obj.postFx.vhsOverlayDropouts = std::stof(value); }},
        {"postWavyEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.wavyEnabled = (std::stoi(value) != 0); }},
        {"postWavyAmplitude", +[](SceneObject& obj, const std::string& value) { obj.postFx.wavyAmplitude = std::stof(value); }},
        {"postWavyFrequency", +[](SceneObject& obj, const std::string& value) { obj.postFx.wavyFrequency = std::stof(value); }},
        {"postWavySpeed", +[](SceneObject& obj, const std::string& value) { obj.postFx.wavySpeed = std::stof(value); }},
        {"postWavyVertical", +[](SceneObject& obj, const std::string& value) { obj.postFx.wavyVertical = (std::stoi(value) != 0); }},
        {"postScope2DEnabled", +[](SceneObject& obj, const std::string& value) { obj.postFx.scope2DEnabled = (std::stoi(value) != 0); }},
        {"postScope2DMode", +[](SceneObject& obj, const std::string& value) { obj.postFx.scope2DMode = static_cast<PostFX2DScopeMode>(std::clamp(std::stoi(value), 0, 1)); }},
        {"postScope2DMinOrder", +[](SceneObject& obj, const std::string& value) { obj.postFx.scope2DMinOrder = std::stoi(value); }},
        {"postScope2DMaxOrder", +[](SceneObject& obj, const std::string& value) { obj.postFx.scope2DMaxOrder = std::stoi(value); }},
        {"meshPath", +[](SceneObject& obj, const std::string& value) {
             obj.meshPath = value;
             if (g_deferSceneAssetLoading) {
                 return;
             }
             if (!value.empty() && obj.hasRenderer && obj.renderType == RenderType::OBJMesh) {
                 std::string err;
                 obj.meshId = g_objLoader.loadOBJ(value, err);
             } else if (!value.empty() && obj.hasRenderer && obj.renderType == RenderType::Model) {
                 ModelSceneData sceneData;
                 std::string err;
                 if (getModelLoader().loadModelScene(value, sceneData, err)) {
                    int sourceIndex = obj.meshSourceIndex;
                     if (sourceIndex < 0 || sourceIndex >= (int)sceneData.meshIndices.size()) {
                         sourceIndex = 0;
                     }
                     if (!sceneData.meshIndices.empty() &&
                         sourceIndex >= 0 && sourceIndex < (int)sceneData.meshIndices.size()) {
                         obj.meshId = sceneData.meshIndices[sourceIndex];
                     }
                     ApplyModelRootTransform(obj, sceneData);
                 } else {
                     std::cerr << "Failed to load model from scene: " << err << std::endl;
                     obj.meshId = -1;
                 }
             }
         }},
        {"meshSourceIndex", +[](SceneObject& obj, const std::string& value) {
             obj.meshSourceIndex = std::stoi(value);
             if (g_deferSceneAssetLoading) {
                 return;
             }
             if (!obj.meshPath.empty() && obj.hasRenderer && obj.renderType == RenderType::Model) {
                 ModelSceneData sceneData;
                 std::string err;
                 if (getModelLoader().loadModelScene(obj.meshPath, sceneData, err)) {
                     int sourceIndex = obj.meshSourceIndex;
                     if (sourceIndex < 0 || sourceIndex >= (int)sceneData.meshIndices.size()) {
                         sourceIndex = 0;
                     }
                     if (!sceneData.meshIndices.empty() &&
                         sourceIndex >= 0 && sourceIndex < (int)sceneData.meshIndices.size()) {
                         obj.meshId = sceneData.meshIndices[sourceIndex];
                     }
                     ApplyModelRootTransform(obj, sceneData);
                 } else {
                     std::cerr << "Failed to load model from scene: " << err << std::endl;
                 }
             }
         }},
        {"children", +[](SceneObject& obj, const std::string& value) {
             if (!value.empty()) {
                 std::stringstream ss(value);
                 std::string item;
                 while (std::getline(ss, item, ',')) {
                     if (!item.empty()) {
                         obj.childIds.push_back(std::stoi(item));
                     }
                 }
             }
         }},
    };
    return handlers;
}
} // namespace

ObjectType GetLegacyTypeFromComponents(const SceneObject& obj) {
    if (obj.type == ObjectType::Sprite25D) {
        return ObjectType::Sprite25D;
    }
    if (obj.type == ObjectType::Light2D && obj.hasLight2D) {
        return ObjectType::Light2D;
    }
    if (obj.type == ObjectType::ShadowCaster2D && obj.hasShadowCaster2D) {
        return ObjectType::ShadowCaster2D;
    }
    if (obj.type == ObjectType::ParticleSystem2D || obj.hasParticleSystem2D) {
        return ObjectType::ParticleSystem2D;
    }
    if (obj.hasRenderer) {
        switch (obj.renderType) {
            case RenderType::Cube: return ObjectType::Cube;
            case RenderType::Sphere: return ObjectType::Sphere;
            case RenderType::Capsule: return ObjectType::Capsule;
            case RenderType::OBJMesh: return ObjectType::OBJMesh;
            case RenderType::Model: return ObjectType::Model;
            case RenderType::Mirror: return ObjectType::Mirror;
            case RenderType::Plane: return ObjectType::Plane;
            case RenderType::Torus: return ObjectType::Torus;
            case RenderType::Sprite: return ObjectType::Sprite;
            case RenderType::None: break;
        }
    }
    if (obj.hasUI) {
        switch (obj.ui.type) {
            case UIElementType::Canvas: return ObjectType::Canvas;
            case UIElementType::Image: return ObjectType::UIImage;
            case UIElementType::Slider: return ObjectType::UISlider;
            case UIElementType::Button: return ObjectType::UIButton;
            case UIElementType::Text: return ObjectType::UIText;
            case UIElementType::Sprite2D:
                return (obj.type == ObjectType::Sprite25D || obj.ui.label == "2.5D Sprite")
                    ? ObjectType::Sprite25D
                    : ObjectType::Sprite2D;
            case UIElementType::None: break;
        }
    }
    if (obj.hasLight) {
        switch (obj.light.type) {
            case LightType::Directional: return ObjectType::DirectionalLight;
            case LightType::Point: return ObjectType::PointLight;
            case LightType::Spot: return ObjectType::SpotLight;
            case LightType::Area: return ObjectType::AreaLight;
        }
    }
    if (obj.hasLight2D) {
        return ObjectType::Light2D;
    }
    if (obj.hasReflectionCast) {
        return ObjectType::ReflectionCast;
    }
    if (obj.hasCamera) {
        return ObjectType::Camera;
    }
    if (obj.hasPostFX) {
        return ObjectType::PostFXNode;
    }
    if (obj.hasShadowCaster2D) {
        return ObjectType::ShadowCaster2D;
    }
    return ObjectType::Empty;
}

bool SceneSerializationInternal::LoadLegacySceneStream(std::istream& file,
                                                       std::vector<SceneObject>& objects,
                                                       int& nextId,
                                                       int& outVersion,
                                                       float* outTimeOfDay,
                                                       SkyboxSettings* outSkyboxSettings,
                                                       bool deferAssetLoading,
                                                       SceneSerializer::Metadata* outMetadata) {
    try {
        objects.clear();
        std::string line;
        SceneObject* currentObj = nullptr;
        int sceneVersion = 20;
        float sceneTimeOfDay = -1.0f;
        SkyboxSettings sceneSkyboxSettings;

        struct DeferGuard {
            bool previous = false;
            explicit DeferGuard(bool enable) {
                previous = g_deferSceneAssetLoading;
                g_deferSceneAssetLoading = enable;
            }
            ~DeferGuard() {
                g_deferSceneAssetLoading = previous;
            }
        } guard(deferAssetLoading);

        while (std::getline(file, line)) {
            size_t first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                continue;
            }
            line.erase(0, first);
            size_t last = line.find_last_not_of(" \t\r\n");
            if (last != std::string::npos) {
                line.erase(last + 1);
            } else {
                continue;
            }

            if (line.empty() || line[0] == '#') continue;

            if (line == "[Object]") {
                objects.push_back(SceneObject("", ObjectType::Empty, 0));
                currentObj = &objects.back();
                continue;
            }

            size_t eqPos = line.find('=');
            if (eqPos == std::string::npos) continue;

            std::string key = line.substr(0, eqPos);
            std::string value = line.substr(eqPos + 1);

            if (SceneValueLooksLikeAbsolutePath(value)) {
                value = Modularity::Platform::ToPortableAssetPath(value);
            }

            if (key == "version") {
                sceneVersion = std::stoi(value);
            } else if (key == "nextId") {
                nextId = std::stoi(value);
            } else if (key == "timeOfDay") {
                sceneTimeOfDay = std::stof(value);
            } else if (key == "skyboxMode") {
                int mode = std::stoi(value);
                sceneSkyboxSettings.mode = (mode == static_cast<int>(SkyboxMode::Scrolling))
                    ? SkyboxMode::Scrolling
                    : SkyboxMode::Procedural;
            } else if (key == "skyboxCachedSky") {
                sceneSkyboxSettings.cachedSky = std::stoi(value) != 0;
            } else if (key == "skyboxSunFollowsLight") {
                sceneSkyboxSettings.sunFollowsDirectionalLight = std::stoi(value) != 0;
            } else if (key == "skyboxVolumetric") {
                sceneSkyboxSettings.volumetricEnabled = std::stoi(value) != 0;
            } else if (key == "skyboxVolumetricIntensity") {
                sceneSkyboxSettings.volumetricIntensity = std::max(0.0f, std::stof(value));
            } else if (key == "skyboxVolumetricSteps") {
                sceneSkyboxSettings.volumetricSteps = std::clamp(std::stoi(value), 4, 256);
            } else if (key == "skyboxVolumetricScattering") {
                sceneSkyboxSettings.volumetricScattering = std::clamp(std::stof(value), -0.95f, 0.95f);
            } else if (key == "skyboxVolumetricDensity") {
                sceneSkyboxSettings.volumetricDensity = std::max(0.0f, std::stof(value));
            } else if (key == "skyboxVolumetricMaxDistance") {
                sceneSkyboxSettings.volumetricMaxDistance = std::max(0.01f, std::stof(value));
            } else if (key == "skyboxWeatherEnabled") {
                sceneSkyboxSettings.weather.enabled = ParseProjectBool(value);
            } else if (key == "skyboxWeatherMode") {
                sceneSkyboxSettings.weather.mode = static_cast<WeatherMode>(std::clamp(std::stoi(value), 0, 4));
            } else if (key == "skyboxWeatherIntensity") {
                sceneSkyboxSettings.weather.intensity = std::clamp(std::stof(value), 0.0f, 1.0f);
            } else if (key == "skyboxWeatherWindSpeed") {
                sceneSkyboxSettings.weather.windSpeed = std::max(0.0f, std::stof(value));
            } else if (key == "skyboxWeatherRainOpacity") {
                sceneSkyboxSettings.weather.rainOpacity = std::clamp(std::stof(value), 0.0f, 2.0f);
            } else if (key == "skyboxWeatherRainSpeed") {
                sceneSkyboxSettings.weather.rainSpeed = std::max(0.0f, std::stof(value));
            } else if (key == "skyboxWeatherRainScale") {
                sceneSkyboxSettings.weather.rainScale = std::max(0.05f, std::stof(value));
            } else if (key == "skyboxWeatherLightningInterval") {
                sceneSkyboxSettings.weather.lightningInterval = std::max(0.5f, std::stof(value));
            } else if (key == "skyboxWeatherLightningIntensity") {
                sceneSkyboxSettings.weather.lightningIntensity = std::max(0.0f, std::stof(value));
            } else if (key == "skyboxWeatherAmbienceVolume") {
                sceneSkyboxSettings.weather.ambienceVolume = std::clamp(std::stof(value), 0.0f, 1.0f);
            } else if (key == "skyboxWeatherHeavyThreshold") {
                sceneSkyboxSettings.weather.heavyRainThreshold = std::clamp(std::stof(value), 0.0f, 1.0f);
            } else if (key == "skyboxWeatherClearSound") {
                sceneSkyboxSettings.weather.clearSoundPath = value;
            } else if (key == "skyboxWeatherOvercastSound") {
                sceneSkyboxSettings.weather.overcastSoundPath = value;
            } else if (key == "skyboxWeatherRainSound") {
                sceneSkyboxSettings.weather.rainSoundPath = value;
            } else if (key == "skyboxWeatherHeavyRainSound") {
                sceneSkyboxSettings.weather.heavyRainSoundPath = value;
            } else if (key == "skyboxWeatherThunderSound") {
                sceneSkyboxSettings.weather.thunderSoundPath = value;
            } else if (key == "skyboxCloudHorizonFade") {
                sceneSkyboxSettings.cloudHorizonFade = std::clamp(std::stof(value), 0.0f, 1.0f);
            } else if (key == "skyboxSkyResolution") {
                sceneSkyboxSettings.skyResolution = std::clamp(std::stoi(value), 64, 2048);
            } else if (key == "skyboxSkyRefreshInterval") {
                sceneSkyboxSettings.skyRefreshInterval = std::clamp(std::stof(value), 0.0f, 5.0f);
            } else if (key == "skyboxCloudMode") {
                sceneSkyboxSettings.cloudMode = std::clamp(std::stoi(value), 0, 1);
            } else if (key == "skyboxCloudCoverage") {
                sceneSkyboxSettings.cloudCoverage = std::clamp(std::stof(value), 0.0f, 1.0f);
            } else if (key == "skyboxCloudDensity") {
                sceneSkyboxSettings.cloudDensity = std::max(0.0f, std::stof(value));
            } else if (key == "skyboxCloudSteps") {
                sceneSkyboxSettings.cloudSteps = std::clamp(std::stoi(value), 4, 64);
            } else if (key == "skyboxCloudLightSteps") {
                sceneSkyboxSettings.cloudLightSteps = std::clamp(std::stoi(value), 1, 8);
            } else if (key == "skyboxCloudBottom") {
                sceneSkyboxSettings.cloudBottom = std::max(1.0f, std::stof(value));
            } else if (key == "skyboxCloudTop") {
                sceneSkyboxSettings.cloudTop = std::max(2.0f, std::stof(value));
            } else if (key == "skyboxCloudWind") {
                sceneSkyboxSettings.cloudWind = std::max(0.0f, std::stof(value));
            } else if (key == "skyboxCloudDarkness") {
                sceneSkyboxSettings.cloudDarkness = std::clamp(std::stof(value), 0.0f, 1.0f);
            } else if (key == "skyboxVolumetricMaxIntensity") {
                sceneSkyboxSettings.volumetricMaxIntensity = std::max(0.0f, std::stof(value));
            } else if (key == "skyboxVolumetricDenoise") {
                sceneSkyboxSettings.volumetricDenoise = ParseProjectBool(value);
            } else if (key == "skyboxVolumetricDenoiseRadius") {
                sceneSkyboxSettings.volumetricDenoiseRadius = std::clamp(std::stoi(value), 0, 8);
            } else if (key == "skyboxVolumetricHalfRes") {
                sceneSkyboxSettings.volumetricHalfResolution = std::stoi(value) != 0;
            } else if (key == "skyboxSsr") {
                sceneSkyboxSettings.ssrEnabled = std::stoi(value) != 0;
            } else if (key == "skyboxSsrIntensity") {
                sceneSkyboxSettings.ssrIntensity = std::max(0.0f, std::stof(value));
            } else if (key == "skyboxSsrSteps") {
                sceneSkyboxSettings.ssrSteps = std::clamp(std::stoi(value), 8, 256);
            } else if (key == "skyboxSsrRefineSteps") {
                sceneSkyboxSettings.ssrRefineSteps = std::clamp(std::stoi(value), 0, 16);
            } else if (key == "skyboxSsrMaxDistance") {
                sceneSkyboxSettings.ssrMaxDistance = std::max(0.01f, std::stof(value));
            } else if (key == "skyboxSsrThickness") {
                sceneSkyboxSettings.ssrThickness = std::max(0.001f, std::stof(value));
            } else if (key == "skyboxSsrMaxRoughness") {
                sceneSkyboxSettings.ssrMaxRoughness = std::clamp(std::stof(value), 0.0f, 1.0f);
            } else if (key == "skyboxSsrHalfRes") {
                sceneSkyboxSettings.ssrHalfResolution = std::stoi(value) != 0;
            } else if (key == "skyboxGi") {
                sceneSkyboxSettings.giEnabled = std::stoi(value) != 0;
            } else if (key == "skyboxGiIntensity") {
                sceneSkyboxSettings.giIntensity = std::max(0.0f, std::stof(value));
            } else if (key == "skyboxGiResolution") {
                sceneSkyboxSettings.giResolution = std::clamp(std::stoi(value), 32, 256);
            } else if (key == "skyboxGiMaxExtent") {
                sceneSkyboxSettings.giMaxExtent = std::max(1.0f, std::stof(value));
            } else if (key == "skyboxGiConeSteps") {
                sceneSkyboxSettings.giConeSteps = std::clamp(std::stoi(value), 4, 64);
            } else if (key == "skyboxGiBounces") {
                sceneSkyboxSettings.giBounces = std::clamp(std::stoi(value), 1, 4);
            } else if (key == "skyboxSunTexture") {
                sceneSkyboxSettings.sunTexturePath = value;
            } else if (key == "skyboxMoonTexture") {
                sceneSkyboxSettings.moonTexturePath = value;
            } else if (key == "skyboxScrollTexture") {
                sceneSkyboxSettings.scrollingTexturePath = value;
            } else if (key == "skyboxScrollRepeat") {
                size_t commaPos = value.find(',');
                if (commaPos != std::string::npos) {
                    sceneSkyboxSettings.scrollingRepeatX = std::max(0.01f, std::stof(value.substr(0, commaPos)));
                    sceneSkyboxSettings.scrollingRepeatY = std::max(0.01f, std::stof(value.substr(commaPos + 1)));
                }
            } else if (key == "skyboxScrollLookSensitivity") {
                sceneSkyboxSettings.scrollingLookSensitivity = std::max(0.0f, std::stof(value));
            } else if (key == "skyboxScrollVerticalInfluence") {
                sceneSkyboxSettings.scrollingVerticalInfluence = std::clamp(std::stof(value), 0.0f, 1.0f);
            } else if (key == "skyboxEnvironmentReflections") {
                sceneSkyboxSettings.environmentReflections = std::stoi(value) != 0;
            } else if (key == "skyboxEnvironmentReflectionIntensity") {
                sceneSkyboxSettings.environmentReflectionIntensity = std::clamp(std::stof(value), 0.0f, 2.0f);
            } else if (key == "skyboxReflectionDistanceFadeStart") {
                sceneSkyboxSettings.reflectionDistanceFadeStart = std::max(0.0f, std::stof(value));
            } else if (key == "skyboxReflectionDistanceFadeEnd") {
                sceneSkyboxSettings.reflectionDistanceFadeEnd = std::max(0.01f, std::stof(value));
            } else if (key == "fogEnabled") {
                sceneSkyboxSettings.fogEnabled = std::stoi(value) != 0;
            } else if (key == "fogMode") {
                sceneSkyboxSettings.fogMode = std::clamp(std::stoi(value), 0, 2);
            } else if (key == "fogColor") {
                ParseVec3(value, sceneSkyboxSettings.fogColor);
            } else if (key == "fogStart") {
                sceneSkyboxSettings.fogStart = std::max(0.0f, std::stof(value));
            } else if (key == "fogEnd") {
                sceneSkyboxSettings.fogEnd = std::max(0.01f, std::stof(value));
            } else if (key == "fogDensity") {
                sceneSkyboxSettings.fogDensity = std::clamp(std::stof(value), 0.0f, 1.0f);
            } else if (key == "fogHeight") {
                sceneSkyboxSettings.fogHeight = std::stof(value);
            } else if (key == "fogHeightFalloff") {
                sceneSkyboxSettings.fogHeightFalloff = std::clamp(std::stof(value), 0.0f, 1.0f);
            } else if (currentObj) {
                const auto& handlers = GetSceneObjectKeyHandlers();
                auto handlerIt = handlers.find(key);
                if (handlerIt != handlers.end()) {
                    handlerIt->second(*currentObj, value);
                } else if (key.rfind("animClip", 0) == 0) {
                    currentObj->hasAnimation = true;
                    size_t underscore = key.find('_');
                    if (underscore != std::string::npos && underscore > 8) {
                        int clipIdx = std::stoi(key.substr(8, underscore - 8));
                        if (clipIdx >= 0 && clipIdx < static_cast<int>(currentObj->animation.clips.size())) {
                            std::string sub = key.substr(underscore + 1);
                            auto& clip = currentObj->animation.clips[clipIdx];
                            if (sub == "name") {
                                clip.name = value;
                            } else if (sub == "asset") {
                                clip.assetPath = value;
                            }
                        }
                    }
                } else if (key.rfind("animKey", 0) == 0) {
                    currentObj->hasAnimation = true;
                    size_t underscore = key.find('_');
                    if (underscore != std::string::npos && underscore > 7) {
                        int idx = std::stoi(key.substr(7, underscore - 7));
                        if (idx >= 0 && idx < static_cast<int>(currentObj->animation.keyframes.size())) {
                            std::string sub = key.substr(underscore + 1);
                            auto& keyframe = currentObj->animation.keyframes[idx];
                            if (sub == "time") {
                                keyframe.time = std::stof(value);
                            } else if (sub == "pos") {
                                sscanf(value.c_str(), "%f,%f,%f",
                                       &keyframe.position.x,
                                       &keyframe.position.y,
                                       &keyframe.position.z);
                            } else if (sub == "rot") {
                                sscanf(value.c_str(), "%f,%f,%f",
                                       &keyframe.rotation.x,
                                       &keyframe.rotation.y,
                                       &keyframe.rotation.z);
                            } else if (sub == "scale") {
                                sscanf(value.c_str(), "%f,%f,%f",
                                       &keyframe.scale.x,
                                       &keyframe.scale.y,
                                       &keyframe.scale.z);
                            } else if (sub == "interp") {
                                keyframe.interpolation = static_cast<AnimationInterpolation>(std::stoi(value));
                            } else if (sub == "curve") {
                                keyframe.curveMode = static_cast<AnimationCurveMode>(std::stoi(value));
                            } else if (sub == "in") {
                                sscanf(value.c_str(), "%f,%f",
                                       &keyframe.bezierIn.x,
                                       &keyframe.bezierIn.y);
                            } else if (sub == "out") {
                                sscanf(value.c_str(), "%f,%f",
                                       &keyframe.bezierOut.x,
                                       &keyframe.bezierOut.y);
                            }
                        }
                    }
                } else if (key.rfind("animEvent", 0) == 0) {
                    currentObj->hasAnimation = true;
                    size_t underscore = key.find('_');
                    if (underscore != std::string::npos && underscore > 9) {
                        int idx = std::stoi(key.substr(9, underscore - 9));
                        if (idx >= 0 && idx < static_cast<int>(currentObj->animation.events.size())) {
                            std::string sub = key.substr(underscore + 1);
                            auto& evt = currentObj->animation.events[idx];
                            if (sub == "time") {
                                evt.time = std::stof(value);
                            } else if (sub == "id") {
                                evt.eventId = value;
                            } else if (sub == "payload") {
                                evt.payload = value;
                            }
                        }
                    }
                } else if (key.rfind("animTrack", 0) == 0) {
                    currentObj->hasAnimation = true;
                    size_t underscore = key.find('_');
                    if (underscore != std::string::npos && underscore > 9) {
                        int trackIdx = std::stoi(key.substr(9, underscore - 9));
                        if (trackIdx >= 0 && trackIdx < static_cast<int>(currentObj->animation.tracks.size())) {
                            std::string sub = key.substr(underscore + 1);
                            auto& track = currentObj->animation.tracks[trackIdx];
                            if (sub == "enabled") {
                                track.enabled = std::stoi(value) != 0;
                            } else if (sub == "path") {
                                track.path = value;
                            } else if (sub == "label") {
                                track.label = value;
                            } else if (sub == "default") {
                                track.defaultValue = std::stof(value);
                            } else if (sub == "keyCount") {
                                int count = std::stoi(value);
                                track.keyframes.resize(std::max(0, count));
                            } else if (sub.rfind("key", 0) == 0) {
                                size_t keyUnderscore = sub.find('_');
                                if (keyUnderscore != std::string::npos && keyUnderscore > 3) {
                                    int keyIdx = std::stoi(sub.substr(3, keyUnderscore - 3));
                                    if (keyIdx >= 0 && keyIdx < static_cast<int>(track.keyframes.size())) {
                                        std::string keySub = sub.substr(keyUnderscore + 1);
                                        auto& kf = track.keyframes[keyIdx];
                                        if (keySub == "time") {
                                            kf.time = std::stof(value);
                                        } else if (keySub == "value") {
                                            kf.value = std::stof(value);
                                        } else if (keySub == "interp") {
                                            kf.interpolation = static_cast<AnimationInterpolation>(std::stoi(value));
                                        } else if (keySub == "curve") {
                                            kf.curveMode = static_cast<AnimationCurveMode>(std::stoi(value));
                                        } else if (keySub == "in") {
                                            sscanf(value.c_str(), "%f,%f",
                                                   &kf.bezierIn.x,
                                                   &kf.bezierIn.y);
                                        } else if (keySub == "out") {
                                            sscanf(value.c_str(), "%f,%f",
                                                   &kf.bezierOut.x,
                                                   &kf.bezierOut.y);
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else if (key.rfind("pcAudioStep", 0) == 0 && key != "pcAudioStepCount") {
                    int idx = std::stoi(key.substr(11));
                    if (idx >= 0 && idx < (int)currentObj->playerMovementAudio.footstepClips.size()) {
                        currentObj->playerMovementAudio.footstepClips[idx] = value;
                    }
                } else if (key.rfind("additionalMaterial", 0) == 0) {
                    int idx = std::stoi(key.substr(18)); // length of "additionalMaterial"
                    if (idx >= 0 && idx < (int)currentObj->additionalMaterialPaths.size()) {
                        currentObj->additionalMaterialPaths[idx] = value;
                    }
                } else if (key.rfind("script", 0) == 0) {
                    size_t underscore = key.find('_');
                    if (underscore != std::string::npos && underscore > 6) {
                        int idx = std::stoi(key.substr(6, underscore - 6));
                        if (idx >= 0 && idx < (int)currentObj->scripts.size()) {
                            std::string sub = key.substr(underscore + 1);
                            ScriptComponent& sc = currentObj->scripts[idx];
                            if (sub == "path") {
                                sc.path = value;
                            } else if (sub == "id") {
                                sc.inspectorId = std::max(0, std::stoi(value));
                            } else if (sub == "lang" || sub == "language") {
                                int langValue = std::stoi(value);
                                if (langValue == static_cast<int>(ScriptLanguage::CSharp)) {
                                    sc.language = ScriptLanguage::CSharp;
                                } else if (langValue == static_cast<int>(ScriptLanguage::C)) {
                                    sc.language = ScriptLanguage::C;
                                } else {
                                    sc.language = ScriptLanguage::Cpp;
                                }
                            } else if (sub == "type") {
                                sc.managedType = value;
                            } else if (sub == "enabled") {
                                sc.enabled = std::stoi(value) != 0;
                            } else if (sub == "settings" || sub == "settingCount") {
                                int cnt = std::stoi(value);
                                sc.settings.resize(std::max(0, cnt));
                            } else if (sub.rfind("setting", 0) == 0) {
                                std::string idxStr = sub.substr(7);
                                if (!idxStr.empty() && std::all_of(idxStr.begin(), idxStr.end(), ::isdigit)) {
                                    int sIdx = std::stoi(idxStr);
                                    if (sIdx >= 0 && sIdx < (int)sc.settings.size()) {
                                        size_t sep = value.find(':');
                                        if (sep != std::string::npos) {
                                            sc.settings[sIdx].key = value.substr(0, sep);
                                            sc.settings[sIdx].value = value.substr(sep + 1);
                                        } else {
                                            sc.settings[sIdx].value = value;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        for (auto& obj : objects) {
            EnsureInspectorComponentMetadata(obj);
            if (!obj.hasAnimation) {
                if (!obj.animation.clips.empty() ||
                    !obj.animation.keyframes.empty() ||
                    !obj.animation.events.empty() ||
                    !obj.animation.tracks.empty() ||
                    !obj.animation.clipAssetPath.empty()) {
                    obj.hasAnimation = true;
                }
            }
            if (obj.hasAnimation) {
                NormalizeAnimationClipSlots(obj.animation);
            }
            obj.type = GetLegacyTypeFromComponents(obj);
        }
        UpgradeLegacyScriptSettings(sceneVersion, objects);
        outVersion = sceneVersion;
        if (outTimeOfDay) {
            *outTimeOfDay = sceneTimeOfDay;
        }
        if (outSkyboxSettings) {
            *outSkyboxSettings = sceneSkyboxSettings;
        }
        if (outMetadata) {
            outMetadata->version = sceneVersion;
            outMetadata->fileFormat = SceneSerializer::FileFormat::LegacyFlat;
            outMetadata->loadedFromLegacyLayout = true;
            outMetadata->upgradedToModularLayout = false;
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to load legacy scene stream: " << e.what() << std::endl;
        return false;
    }
}

bool SceneSerializationInternal::LoadLegacyScene(const fs::path& filePath,
                                                 std::vector<SceneObject>& objects,
                                                 int& nextId,
                                                 int& outVersion,
                                                 float* outTimeOfDay,
                                                 SkyboxSettings* outSkyboxSettings,
                                                 bool deferAssetLoading,
                                                 SceneSerializer::Metadata* outMetadata) {
    try {
        std::ifstream file(filePath);
        if (!file.is_open()) return false;
        const bool loaded = LoadLegacySceneStream(file,
                                                  objects,
                                                  nextId,
                                                  outVersion,
                                                  outTimeOfDay,
                                                  outSkyboxSettings,
                                                  deferAssetLoading,
                                                  outMetadata);
        if (loaded && outMetadata) {
            outMetadata->sourcePath = filePath;
        }
        return loaded;
    } catch (const std::exception& e) {
        std::cerr << "Failed to load legacy scene: " << e.what() << std::endl;
        return false;
    }
}
