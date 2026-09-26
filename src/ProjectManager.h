#pragma once

#include "Common.h"
#include "SceneObject.h"
#include "HardwareProfile.h"
#include "EditorSoundThemes.h"
#include "XR/XRSettings.h"
#include "../include/Skybox/Skybox.h"
#include <map>
#include <string>

struct RecentProject {
    std::string name;
    std::string path;
    std::string lastOpened;
};

enum class ProjectPipeline {
    Pipeline3D = 0,
    Pipeline2D = 1,
    Pipeline25D = 2
};

inline const char* ProjectPipelineLabel(ProjectPipeline pipeline) {
    switch (pipeline) {
        case ProjectPipeline::Pipeline2D: return "2D Pipeline";
        case ProjectPipeline::Pipeline25D: return "2.5D Pipeline";
        case ProjectPipeline::Pipeline3D:
        default:
            return "3D Pipeline";
    }
}

inline const char* SerializeProjectPipeline(ProjectPipeline pipeline) {
    switch (pipeline) {
        case ProjectPipeline::Pipeline2D: return "2D";
        case ProjectPipeline::Pipeline25D: return "2.5D";
        case ProjectPipeline::Pipeline3D:
        default:
            return "3D";
    }
}

inline ProjectPipeline ParseProjectPipeline(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "2d" || value == "pipeline2d" || value == "1") {
        return ProjectPipeline::Pipeline2D;
    }
    if (value == "2.5d" || value == "25d" || value == "pipeline25d" ||
        value == "pipeline2.5d" || value == "2") {
        return ProjectPipeline::Pipeline25D;
    }
    return ProjectPipeline::Pipeline3D;
}

inline int ProjectPipelineToUiIndex(ProjectPipeline pipeline) {
    switch (pipeline) {
        case ProjectPipeline::Pipeline25D: return 1;
        case ProjectPipeline::Pipeline2D: return 2;
        case ProjectPipeline::Pipeline3D:
        default:
            return 0;
    }
}

inline ProjectPipeline ProjectPipelineFromUiIndex(int index) {
    switch (index) {
        case 1: return ProjectPipeline::Pipeline25D;
        case 2: return ProjectPipeline::Pipeline2D;
        case 0:
        default:
            return ProjectPipeline::Pipeline3D;
    }
}

enum class ProjectMassUnit {
    Kilograms = 0,
    Grams = 1,
    Pounds = 2,
    Ounces = 3
};

inline float ProjectMassUnitToKilograms(ProjectMassUnit unit) {
    switch (unit) {
        case ProjectMassUnit::Grams: return 0.001f;
        case ProjectMassUnit::Pounds: return 0.45359237f;
        case ProjectMassUnit::Ounces: return 0.0283495231f;
        case ProjectMassUnit::Kilograms:
        default:
            return 1.0f;
    }
}

inline const char* ProjectMassUnitLabel(ProjectMassUnit unit) {
    switch (unit) {
        case ProjectMassUnit::Grams: return "Grams";
        case ProjectMassUnit::Pounds: return "Pounds";
        case ProjectMassUnit::Ounces: return "Ounces";
        case ProjectMassUnit::Kilograms:
        default:
            return "Kilograms";
    }
}

inline const char* ProjectMassUnitSuffix(ProjectMassUnit unit) {
    switch (unit) {
        case ProjectMassUnit::Grams: return "g";
        case ProjectMassUnit::Pounds: return "lb";
        case ProjectMassUnit::Ounces: return "oz";
        case ProjectMassUnit::Kilograms:
        default:
            return "kg";
    }
}

enum class PhysicsBackendType {
    Jolt = 0,
    PhysX = 1
};

inline const char* PhysicsBackendLabel(PhysicsBackendType t) {
    switch (t) {
        case PhysicsBackendType::PhysX: return "PhysX";
        case PhysicsBackendType::Jolt:
        default:
            return "Jolt";
    }
}

struct ProjectPhysicsSettings {
    PhysicsBackendType backend = PhysicsBackendType::Jolt;
    ProjectMassUnit massUnit = ProjectMassUnit::Kilograms;
    float globalGravityScale = 1.0f;
    float fixedTimestep = 1.0f / 60.0f;
    int solverIterations = 8;
    bool enable3DPhysics = true;
    bool enable2DPhysics = true;
    float defaultRaycastDistance = 1000.0f;
    bool raycastHitTriggers = false;
    float defaultRigidbodyMass = 1.0f;
    float defaultRigidbodyDrag = 0.0f;
    float defaultMaterialFriction = 0.5f;
    float defaultMaterialBounciness = 0.0f;
    std::vector<std::string> collisionLayers = { "Default" };
};

enum class ProjectTextureFiltering {
    Bilinear = 0,
    Point = 1,
    Trilinear = 2
};

enum class ProjectAntiAliasing {
    Off = 0,
    MSAA2x = 1,
    MSAA4x = 2,
    MSAA8x = 3
};

enum class ProjectRenderingPath {
    Normal = 0,
    NormalPlus = 1,
    Deferred = 2,
    HeavyDeferred = 3
};

inline void ProjectRenderingPathCaps(ProjectRenderingPath path, int& lightBudget, int& directionalAllowance) {
    switch (path) {
        case ProjectRenderingPath::NormalPlus:    lightBudget = 40;  directionalAllowance = 1; break;
        case ProjectRenderingPath::Deferred:      lightBudget = 200; directionalAllowance = 1; break;
        case ProjectRenderingPath::HeavyDeferred: lightBudget = 500; directionalAllowance = 2; break;
        case ProjectRenderingPath::Normal:
        default:                                  lightBudget = 20;  directionalAllowance = 1; break;
    }
}

inline const char* ProjectRenderingPathLabel(ProjectRenderingPath path) {
    switch (path) {
        case ProjectRenderingPath::NormalPlus:    return "Normal+";
        case ProjectRenderingPath::Deferred:      return "Deferred";
        case ProjectRenderingPath::HeavyDeferred: return "Heavy Deferred";
        case ProjectRenderingPath::Normal:
        default:                                  return "Normal";
    }
}

inline const char* ProjectRenderingPathNote(ProjectRenderingPath path) {
    switch (path) {
        case ProjectRenderingPath::NormalPlus:
            return "40 lights max, 1 directional light always allowed at any amount.";
        case ProjectRenderingPath::Deferred:
            return "200 lights max, 1 directional light always allowed at any amount.";
        case ProjectRenderingPath::HeavyDeferred:
            return "500 lights max, 2 directional lights always allowed at any amount.";
        case ProjectRenderingPath::Normal:
        default:
            return "20 lights max, 1 directional light always allowed at any amount.";
    }
}

enum class ProjectColorResolution {
    Auto = 0,
    Color8 = 1,
    Color16F = 2
};

// Project Settings > Graphics Manager > Optimization: Manual runs exactly the checked
// optimizations; Automatic treats them as allowed and picks per scene.
enum class ProjectOptimizationStrategy {
    Manual = 0,
    Automatic = 1
};

enum class ProjectQualityPreset {
    Low = 0,
    Balanced = 1,
    High = 2,
    Custom = 3
};

inline const char* ProjectQualityPresetLabel(ProjectQualityPreset preset) {
    switch (preset) {
        case ProjectQualityPreset::Low:      return "Low";
        case ProjectQualityPreset::Balanced: return "Balanced";
        case ProjectQualityPreset::High:     return "High";
        case ProjectQualityPreset::Custom:
        default:                             return "Custom";
    }
}

inline const char* ProjectQualityPresetNote(ProjectQualityPreset preset) {
    switch (preset) {
        case ProjectQualityPreset::Low:
            return "For integrated graphics and older laptops. No shadows, no HDR, "
                   "no anti-aliasing, and the scene renders at 75% and upscales.";
        case ProjectQualityPreset::Balanced:
            return "One shadowed main light, no soft shadows, 2x MSAA. Sensible on "
                   "a modern iGPU.";
        case ProjectQualityPreset::High:
            return "Everything on: soft shadows on all lights, HDR, 4x MSAA.";
        case ProjectQualityPreset::Custom:
        default:
            return "Settings were tuned by hand and no longer match a preset.";
    }
}

struct ProjectGraphicsSettings {
    bool vsync = true;
    int targetFps = 60;
    int shadowQuality = 2;
    float renderResolutionScale = 1.0f;
    ProjectTextureFiltering textureFiltering = ProjectTextureFiltering::Bilinear;
    ProjectAntiAliasing antiAliasing = ProjectAntiAliasing::MSAA4x;
    bool fullscreenStartup = false;
    bool editorPreviewOverrides = true;
    bool gamePreviewOverrides = false;
    ProjectRenderingPath renderingPath = ProjectRenderingPath::Normal;
    bool hdr = true;
    ProjectColorResolution colorResolution = ProjectColorResolution::Auto;
    std::string defaultTextureFormat = "Auto";
    int maxTextureSize = 2048;
    int shadowFilterQuality = 2;
    float shadowLodDistance = 30.0f;
    bool shadowContactHardening = true;
    bool mainLightEnabled = true;
    bool mainLightCastShadows = true;
    bool additionalLightsEnabled = true;
    bool additionalLightsCastShadows = true;
    bool specularEnabled = true;
    bool softShadows = true;
    float shadowMaxDistance = 0.0f;
    // Render In 3D UI canvases. Auto resolution sizes each canvas's target to
    // what it covers on screen; the max caps the authored Render Target size
    // (0 = no cap). Auto works underneath the cap.
    bool canvas3DAutoResolution = false;
    int canvas3DMaxResolution = 0;
    // Rendering optimizations (Renderer::setOptimizationSettings). All off by default so an
    // existing project renders exactly as before until someone opts in.
    ProjectOptimizationStrategy optimizationStrategy = ProjectOptimizationStrategy::Manual;
    bool optCpuFrustumCulling = false;
    bool optMultiDrawIndirect = false;
    bool optGpuDrivenCulling = false;
    bool optLod = false;
    float optLodMinScreenSize = 1.5f;
    bool optHiZOcclusionCulling = false;
    bool optMaterialSorting = false;
    bool optStaticObjectCaching = false;
    ProjectQualityPreset qualityPreset = ProjectQualityPreset::Custom;
};

inline void ApplyProjectQualityPreset(ProjectGraphicsSettings& g, ProjectQualityPreset preset) {
    switch (preset) {
        case ProjectQualityPreset::Low:
            g.shadowQuality = 0;
            g.mainLightCastShadows = false;
            g.additionalLightsCastShadows = false;
            g.softShadows = false;
            g.specularEnabled = false;
            g.hdr = false;
            g.colorResolution = ProjectColorResolution::Color8;
            g.antiAliasing = ProjectAntiAliasing::Off;
            g.renderResolutionScale = 0.75f;
            g.renderingPath = ProjectRenderingPath::Normal;
            break;
        case ProjectQualityPreset::Balanced:
            g.shadowQuality = 1;
            g.mainLightCastShadows = true;
            g.additionalLightsCastShadows = false;
            g.softShadows = false;
            g.specularEnabled = true;
            g.hdr = false;
            g.colorResolution = ProjectColorResolution::Auto;
            g.antiAliasing = ProjectAntiAliasing::MSAA2x;
            g.renderResolutionScale = 1.0f;
            break;
        case ProjectQualityPreset::High:
            g.shadowQuality = 3;
            g.mainLightCastShadows = true;
            g.additionalLightsCastShadows = true;
            g.softShadows = true;
            g.specularEnabled = true;
            g.hdr = true;
            g.colorResolution = ProjectColorResolution::Auto;
            g.antiAliasing = ProjectAntiAliasing::MSAA4x;
            g.renderResolutionScale = 1.0f;
            break;
        case ProjectQualityPreset::Custom:
        default:
            return;
    }
    g.qualityPreset = preset;
}

enum class ProjectConsoleMode {
    DockedMiniButton = 0,
    FloatingWindow = 1
};

enum class ProjectConsoleTone {
    Fun = 0,
    Concise = 1
};

struct ProjectConsoleSettings {
    ProjectConsoleMode mode = ProjectConsoleMode::DockedMiniButton;
    bool alwaysOpenOnLaunch = false;
    bool openOnlyOnErrors = true;
    ProjectConsoleTone tone = ProjectConsoleTone::Fun;
};

enum class ProjectImportTranslatePolicy {
    Ask = 0,
    Always = 1,
    Never = 2
};

struct ProjectLanguageSettings {
    std::string moduCppSyntaxLanguage;
    ProjectImportTranslatePolicy importTranslatePolicy = ProjectImportTranslatePolicy::Ask;

    bool usesGlobalModuCppDefault() const { return moduCppSyntaxLanguage.empty(); }
};

struct ProjectPlayerSettings {
    std::string productName;
    std::string companyName = "DefaultCompany";
    std::string defaultScene;
    int startupWidth = 1280;
    int startupHeight = 720;
    bool fullscreenStartup = false;
    bool nativeDisplayResolution = false;
    bool cursorLocked = false;
    bool cursorVisible = true;
    std::string buildTarget = "Windows";
    std::string applicationIconPath;
    std::string saveDataPathBehavior = "CompanyAndProduct";
};

class Project {
public:
    std::string name;
    fs::path projectPath;
    fs::path scenesPath;
    fs::path assetsPath;
    fs::path scriptsPath;
    fs::path scriptsConfigPath;
    std::string currentSceneName;
    bool isLoaded = false;
    bool hasUnsavedChanges = false;
    bool usesNewLayout = false;
    ProjectPipeline pipeline = ProjectPipeline::Pipeline3D;
    Modularity::GraphicsBackend rendererBackend = Modularity::GraphicsBackend::OpenGL;
    ProjectPhysicsSettings physicsSettings;
    ProjectGraphicsSettings graphicsSettings;
    std::vector<std::string> tags = { "Untagged" };
    ProjectConsoleSettings consoleSettings;
    ProjectLanguageSettings languageSettings;
    ProjectPlayerSettings playerSettings;
    Modularity::XR::ProjectOpenXRSettings openXRSettings;
    std::map<std::string, std::string> textureFormatOverrides;

    Project() = default;
    Project(const std::string& projectName, const fs::path& basePath);

    bool create();
    bool load(const fs::path& projectFilePath);
    void saveProjectFile() const;
    std::vector<std::string> getSceneList() const;
    fs::path getSceneFilePath(const std::string& sceneName) const;
};

struct EditorGlobalPreferences {
    std::string themePreset = "Default";
    std::string uiFontAsset;
    std::string chromeScale = "Default";
    std::string animationMode = "Fluid";
    float uiScale = 1.0f;

    bool launcherIntroAnimation = true;
    bool launcherProjectThumbnails = true;
    bool launcherRememberLastTab = true;
    int launcherLastTab = 0;
    bool launcherShowFullPaths = true;
    int launcherProjectSort = 0;

    bool feedbackSounds = true;
    bool feedbackClickSounds = true;
    bool feedbackErrorSounds = true;
    bool feedbackOtherSounds = true;
    bool feedbackButtonSounds = true;
    bool feedbackToggleSounds = true;
    bool feedbackSlideSounds = true;
    bool feedbackTransitionSounds = true;
    bool feedbackHoverSounds = true;
    float feedbackSoundVolume = 0.5f;  // UI volume, 0..1
    // Folder name under Resources/Sounds/SoundThemes. Global only: never stored per project.
    std::string feedbackSoundTheme = Modularity::EditorSounds::kDefaultThemeId;

    bool reopenLastProject = false;
    bool hierarchyTexturePreviews = true;
    bool sceneGizmos = true;
    bool gizmoCameraOverlays = true;
    bool consoleWrapText = true;
    bool quickToolsPinned = false;

    bool editorVSync = true;
    bool editorFpsCapEnabled = false;
    float editorFpsCap = 120.0f;
    std::string performanceMode = "Auto";
    std::string hardwareTier;
    float benchFullscreenPassMs = 0.0f;
    float benchBlurPassMs = 0.0f;
    float benchDrawCallBatchMs = 0.0f;
    bool  lowSpecPromptAnswered = false;
    bool scriptAutoCompile = true;
    float scriptAutoCompileInterval = 0.5f;
    float scriptDirectoryScanInterval = 5.0f;

    int defaultPipeline = 0;
    int defaultRenderer = 0;
    std::string defaultCompanyName = "DefaultCompany";
    bool applyDefaultPackages = true;
    std::vector<std::string> defaultPackageIds;

    Modularity::HardwareProfile::Tier effectiveTier() const {
        if (performanceMode != "Auto" && !performanceMode.empty()) {
            return Modularity::HardwareProfile::FromString(performanceMode);
        }
        if (!hardwareTier.empty()) {
            return Modularity::HardwareProfile::FromString(hardwareTier);
        }
        return Modularity::HardwareProfile::Tier::High;
    }

    bool isDefaultPackage(const std::string& id) const {
        return std::find(defaultPackageIds.begin(), defaultPackageIds.end(), id) !=
               defaultPackageIds.end();
    }
    void setDefaultPackage(const std::string& id, bool enabled) {
        const auto it = std::find(defaultPackageIds.begin(), defaultPackageIds.end(), id);
        if (enabled && it == defaultPackageIds.end()) {
            defaultPackageIds.push_back(id);
        } else if (!enabled && it != defaultPackageIds.end()) {
            defaultPackageIds.erase(it);
        }
    }
};

class ProjectManager {
public:
    std::vector<RecentProject> recentProjects;
    fs::path appDataPath;
    char newProjectName[128] = "";
    char newProjectLocation[512] = "";
    char defaultProjectLocation[512] = "";
    char openProjectPath[512] = "";
    bool showNewProjectDialog = false;
    bool showOpenProjectDialog = false;
    bool showCloneProjectDialog = false;
    std::string acceptedTermsVersion;
    bool windowsDisclaimerAcknowledgedV68 = false;
    bool moduCppNvimWarningDismissedV1 = false;
    bool androidStoragePromptDismissedV1 = false;
    std::string editorLanguage = "english";
    std::string moduCppDefaultLanguage = "english";
    bool osLanguagePromptAnswered = false;
    EditorGlobalPreferences preferences;
    int newProjectPipelineMode = 0;
    int newProjectRendererMode = 0;
    bool newProjectImportLastPackages = true;
    std::string newProjectTemplatePath;
    std::string newProjectTemplateName;
    std::string newProjectPresetId = "empty";
    std::string errorMessage;
    Project currentProject;

    ProjectManager();

    void loadRecentProjects();
    void saveRecentProjects();
    void loadLauncherSettings();
    void saveLauncherSettings() const;
    void addToRecentProjects(const std::string& name, const std::string& path);
    bool loadProject(const std::string& path);

    std::string effectiveModuCppLanguage() const;
};

class SceneSerializer {
public:
    enum class FileFormat {
        LegacyFlat = 0,
        ModularNodes = 1
    };

    enum class SavePreference {
        PreferModular = 0,
        ForceLegacyFlat = 1
    };

    struct Metadata {
        int version = 0;
        FileFormat fileFormat = FileFormat::ModularNodes;
        bool loadedFromLegacyLayout = false;
        bool upgradedToModularLayout = false;
        fs::path sourcePath;
    };

    struct SaveOptions {
        SavePreference preference = SavePreference::PreferModular;
        bool moveLegacySourceToCompatibility = false;
        Metadata* metadata = nullptr;
    };

    static bool saveScene(const fs::path& filePath,
                         const std::vector<SceneObject>& objects,
                         int nextId,
                         float timeOfDay,
                         const SkyboxSettings& skyboxSettings = SkyboxSettings{});

    static bool saveScene(const fs::path& filePath,
                         const std::vector<SceneObject>& objects,
                         int nextId,
                         float timeOfDay,
                         const SkyboxSettings& skyboxSettings,
                         const SaveOptions& options);

    static bool loadScene(const fs::path& filePath,
                         std::vector<SceneObject>& objects,
                         int& nextId,
                         int& outVersion,
                         float* outTimeOfDay = nullptr,
                         SkyboxSettings* outSkyboxSettings = nullptr,
                         Metadata* outMetadata = nullptr);

    static bool loadSceneDeferred(const fs::path& filePath,
                         std::vector<SceneObject>& objects,
                         int& nextId,
                         int& outVersion,
                         float* outTimeOfDay = nullptr,
                         SkyboxSettings* outSkyboxSettings = nullptr,
                         Metadata* outMetadata = nullptr);
};
