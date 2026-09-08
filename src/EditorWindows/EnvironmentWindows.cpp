#include "../EditorLocalization.h"
#include "Engine.h"
#include "ModelLoader.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <cfloat>
#include <cmath>
#include <functional>
#include <sstream>
#include <unordered_set>
#include <optional>
#include <future>
#include <chrono>
#include <future>

#ifdef _WIN32
#include <shlobj.h>
#endif

namespace Loc = Modularity::Loc;

#pragma region Environment Window
void Engine::renderEnvironmentWindow() {
    if (!showEnvironmentWindow) return;
    ImGui::Begin(Loc::Window("WINDOW_ENVIRONMENT", "Environment"), &showEnvironmentWindow);

    const bool vulkanPreviewMode = usingVulkan();

    Skybox* skybox = renderer.getSkybox();
    float tod = getSceneTimeOfDay();
    SkyboxSettings skySettings = getSceneSkyboxSettings();

    static char sunTexBuf[512] = {};
    static char moonTexBuf[512] = {};
    static char scrollTexBuf[512] = {};
    static SkyboxSettings lastUiSkySettings;
    static int uiSkyMode = static_cast<int>(SkyboxMode::Procedural);
    static float uiScrollRepeat[2] = { 2.0f, 1.0f };
    static float uiScrollLookSensitivity = 1.0f;
    static float uiScrollVerticalInfluence = 0.18f;
    static bool uiCachedSky = true;
    static bool uiSunFollowsLight = false;
    static bool uiEnvironmentReflections = false;
    static float uiEnvironmentReflectionIntensity = 0.5f;
    static float uiReflectionDistanceFade[2] = { 4.0f, 24.0f };
    static bool uiFogEnabled = false;
    static int uiFogMode = 0;
    static glm::vec3 uiFogColor = glm::vec3(0.65f, 0.72f, 0.78f);
    static float uiFogStart = 20.0f;
    static float uiFogEnd = 120.0f;
    static float uiFogDensity = 0.015f;
    static float uiFogHeight = 0.0f;
    static float uiFogHeightFalloff = 0.0f;
    static float uiVolumetricMaxIntensity = 2.0f;
    static bool uiVolumetricDenoise = true;
    static int uiVolumetricDenoiseRadius = 3;
    static float uiCloudHorizonFade = 0.16f;
    static int uiSkyResolution = 256;
    static float uiSkyRefreshInterval = 0.1f;
    static int uiCloudMode = 0;
    static float uiCloudCoverage = 0.45f;
    static float uiCloudDensity = 1.0f;
    static int uiCloudSteps = 24;
    static int uiCloudLightSteps = 4;
    static float uiCloudBottom = 900.0f;
    static float uiCloudTop = 2400.0f;
    static float uiCloudWind = 1.0f;
    static bool uiWeatherEnabled = false;
    static int uiWeatherMode = 0;
    static float uiWeatherIntensity = 1.0f;
    static float uiWeatherWindSpeed = 1.0f;
    static float uiWeatherRainOpacity = 0.55f;
    static float uiWeatherRainSpeed = 1.6f;
    static float uiWeatherRainScale = 1.0f;
    static float uiWeatherLightningInterval = 11.0f;
    static float uiWeatherLightningIntensity = 2.5f;
    static float uiWeatherAmbienceVolume = 0.7f;
    static float uiWeatherHeavyThreshold = 0.65f;
    static std::string uiWeatherClearSound;
    static std::string uiWeatherOvercastSound;
    static std::string uiWeatherRainSound;
    static std::string uiWeatherHeavyRainSound;
    static std::string uiWeatherThunderSound;
    static bool uiVolumetricEnabled = false;
    static float uiVolumetricIntensity = 1.0f;
    static int uiVolumetricSteps = 32;
    static float uiVolumetricScattering = 0.6f;
    static float uiVolumetricDensity = 0.05f;
    static float uiVolumetricMaxDistance = 60.0f;
    static bool uiVolumetricHalfResolution = true;
    static bool uiSsrEnabled = false;
    static float uiSsrIntensity = 1.0f;
    static int uiSsrSteps = 48;
    static int uiSsrRefineSteps = 6;
    static float uiSsrMaxDistance = 40.0f;
    static float uiSsrThickness = 0.5f;
    static float uiSsrMaxRoughness = 0.6f;
    static bool uiSsrHalfResolution = true;
    static bool uiGiEnabled = false;
    static float uiGiIntensity = 1.0f;
    static int uiGiResolution = 128;
    static float uiGiMaxExtent = 60.0f;
    static int uiGiConeSteps = 24;
    static int uiGiBounces = 2;
    static bool skyUiInitialized = false;
    if (!skyUiInitialized ||
        lastUiSkySettings.mode != skySettings.mode ||
        lastUiSkySettings.sunTexturePath != skySettings.sunTexturePath ||
        lastUiSkySettings.moonTexturePath != skySettings.moonTexturePath ||
        lastUiSkySettings.scrollingTexturePath != skySettings.scrollingTexturePath ||
        std::abs(lastUiSkySettings.scrollingRepeatX - skySettings.scrollingRepeatX) > 0.0001f ||
        std::abs(lastUiSkySettings.scrollingRepeatY - skySettings.scrollingRepeatY) > 0.0001f ||
        std::abs(lastUiSkySettings.scrollingLookSensitivity - skySettings.scrollingLookSensitivity) > 0.0001f ||
        std::abs(lastUiSkySettings.scrollingVerticalInfluence - skySettings.scrollingVerticalInfluence) > 0.0001f ||
        lastUiSkySettings.cachedSky != skySettings.cachedSky ||
        lastUiSkySettings.sunFollowsDirectionalLight != skySettings.sunFollowsDirectionalLight ||
        lastUiSkySettings.environmentReflections != skySettings.environmentReflections ||
        std::abs(lastUiSkySettings.environmentReflectionIntensity - skySettings.environmentReflectionIntensity) > 0.0001f ||
        std::abs(lastUiSkySettings.reflectionDistanceFadeStart - skySettings.reflectionDistanceFadeStart) > 0.0001f ||
        std::abs(lastUiSkySettings.reflectionDistanceFadeEnd - skySettings.reflectionDistanceFadeEnd) > 0.0001f ||
        lastUiSkySettings.fogEnabled != skySettings.fogEnabled ||
        lastUiSkySettings.fogMode != skySettings.fogMode ||
        glm::length(lastUiSkySettings.fogColor - skySettings.fogColor) > 0.0001f ||
        std::abs(lastUiSkySettings.fogStart - skySettings.fogStart) > 0.0001f ||
        std::abs(lastUiSkySettings.fogEnd - skySettings.fogEnd) > 0.0001f ||
        std::abs(lastUiSkySettings.fogDensity - skySettings.fogDensity) > 0.0001f ||
        std::abs(lastUiSkySettings.fogHeight - skySettings.fogHeight) > 0.0001f ||
        std::abs(lastUiSkySettings.fogHeightFalloff - skySettings.fogHeightFalloff) > 0.0001f ||
        lastUiSkySettings.volumetricEnabled != skySettings.volumetricEnabled ||
        std::abs(lastUiSkySettings.volumetricIntensity - skySettings.volumetricIntensity) > 0.0001f ||
        lastUiSkySettings.volumetricSteps != skySettings.volumetricSteps ||
        std::abs(lastUiSkySettings.volumetricScattering - skySettings.volumetricScattering) > 0.0001f ||
        std::abs(lastUiSkySettings.volumetricDensity - skySettings.volumetricDensity) > 0.0001f ||
        std::abs(lastUiSkySettings.volumetricMaxDistance - skySettings.volumetricMaxDistance) > 0.0001f ||
        lastUiSkySettings.volumetricHalfResolution != skySettings.volumetricHalfResolution ||
        lastUiSkySettings.ssrEnabled != skySettings.ssrEnabled ||
        std::abs(lastUiSkySettings.ssrIntensity - skySettings.ssrIntensity) > 0.0001f ||
        lastUiSkySettings.ssrSteps != skySettings.ssrSteps ||
        lastUiSkySettings.ssrRefineSteps != skySettings.ssrRefineSteps ||
        std::abs(lastUiSkySettings.ssrMaxDistance - skySettings.ssrMaxDistance) > 0.0001f ||
        std::abs(lastUiSkySettings.ssrThickness - skySettings.ssrThickness) > 0.0001f ||
        std::abs(lastUiSkySettings.ssrMaxRoughness - skySettings.ssrMaxRoughness) > 0.0001f ||
        lastUiSkySettings.ssrHalfResolution != skySettings.ssrHalfResolution ||
        lastUiSkySettings.giEnabled != skySettings.giEnabled ||
        std::abs(lastUiSkySettings.giIntensity - skySettings.giIntensity) > 0.0001f ||
        lastUiSkySettings.giResolution != skySettings.giResolution ||
        std::abs(lastUiSkySettings.giMaxExtent - skySettings.giMaxExtent) > 0.0001f ||
        lastUiSkySettings.giConeSteps != skySettings.giConeSteps ||
        lastUiSkySettings.giBounces != skySettings.giBounces) {
        std::snprintf(sunTexBuf, sizeof(sunTexBuf), "%s", skySettings.sunTexturePath.c_str());
        std::snprintf(moonTexBuf, sizeof(moonTexBuf), "%s", skySettings.moonTexturePath.c_str());
        std::snprintf(scrollTexBuf, sizeof(scrollTexBuf), "%s", skySettings.scrollingTexturePath.c_str());
        uiSkyMode = static_cast<int>(skySettings.mode);
        uiScrollRepeat[0] = skySettings.scrollingRepeatX;
        uiScrollRepeat[1] = skySettings.scrollingRepeatY;
        uiScrollLookSensitivity = skySettings.scrollingLookSensitivity;
        uiScrollVerticalInfluence = skySettings.scrollingVerticalInfluence;
        uiCachedSky = skySettings.cachedSky;
        uiSunFollowsLight = skySettings.sunFollowsDirectionalLight;
        uiVolumetricMaxIntensity = skySettings.volumetricMaxIntensity;
        uiVolumetricDenoise = skySettings.volumetricDenoise;
        uiVolumetricDenoiseRadius = skySettings.volumetricDenoiseRadius;
        uiCloudHorizonFade = skySettings.cloudHorizonFade;
        uiSkyResolution = skySettings.skyResolution;
        uiSkyRefreshInterval = skySettings.skyRefreshInterval;
        uiCloudMode = skySettings.cloudMode;
        uiCloudCoverage = skySettings.cloudCoverage;
        uiCloudDensity = skySettings.cloudDensity;
        uiCloudSteps = skySettings.cloudSteps;
        uiCloudLightSteps = skySettings.cloudLightSteps;
        uiCloudBottom = skySettings.cloudBottom;
        uiCloudTop = skySettings.cloudTop;
        uiCloudWind = skySettings.cloudWind;
        uiWeatherEnabled = skySettings.weather.enabled;
        uiWeatherMode = static_cast<int>(skySettings.weather.mode);
        uiWeatherIntensity = skySettings.weather.intensity;
        uiWeatherWindSpeed = skySettings.weather.windSpeed;
        uiWeatherRainOpacity = skySettings.weather.rainOpacity;
        uiWeatherRainSpeed = skySettings.weather.rainSpeed;
        uiWeatherRainScale = skySettings.weather.rainScale;
        uiWeatherLightningInterval = skySettings.weather.lightningInterval;
        uiWeatherLightningIntensity = skySettings.weather.lightningIntensity;
        uiWeatherAmbienceVolume = skySettings.weather.ambienceVolume;
        uiWeatherHeavyThreshold = skySettings.weather.heavyRainThreshold;
        uiWeatherClearSound = skySettings.weather.clearSoundPath;
        uiWeatherOvercastSound = skySettings.weather.overcastSoundPath;
        uiWeatherRainSound = skySettings.weather.rainSoundPath;
        uiWeatherHeavyRainSound = skySettings.weather.heavyRainSoundPath;
        uiWeatherThunderSound = skySettings.weather.thunderSoundPath;
        uiVolumetricEnabled = skySettings.volumetricEnabled;
        uiVolumetricIntensity = skySettings.volumetricIntensity;
        uiVolumetricSteps = skySettings.volumetricSteps;
        uiVolumetricScattering = skySettings.volumetricScattering;
        uiVolumetricDensity = skySettings.volumetricDensity;
        uiVolumetricMaxDistance = skySettings.volumetricMaxDistance;
        uiVolumetricHalfResolution = skySettings.volumetricHalfResolution;
        uiSsrEnabled = skySettings.ssrEnabled;
        uiSsrIntensity = skySettings.ssrIntensity;
        uiSsrSteps = skySettings.ssrSteps;
        uiSsrRefineSteps = skySettings.ssrRefineSteps;
        uiSsrMaxDistance = skySettings.ssrMaxDistance;
        uiSsrThickness = skySettings.ssrThickness;
        uiSsrMaxRoughness = skySettings.ssrMaxRoughness;
        uiSsrHalfResolution = skySettings.ssrHalfResolution;
        uiGiEnabled = skySettings.giEnabled;
        uiGiIntensity = skySettings.giIntensity;
        uiGiResolution = skySettings.giResolution;
        uiGiMaxExtent = skySettings.giMaxExtent;
        uiGiConeSteps = skySettings.giConeSteps;
        uiGiBounces = skySettings.giBounces;
        uiEnvironmentReflections = skySettings.environmentReflections;
        uiEnvironmentReflectionIntensity = skySettings.environmentReflectionIntensity;
        uiReflectionDistanceFade[0] = skySettings.reflectionDistanceFadeStart;
        uiReflectionDistanceFade[1] = skySettings.reflectionDistanceFadeEnd;
        uiFogEnabled = skySettings.fogEnabled;
        uiFogMode = skySettings.fogMode;
        uiFogColor = skySettings.fogColor;
        uiFogStart = skySettings.fogStart;
        uiFogEnd = skySettings.fogEnd;
        uiFogDensity = skySettings.fogDensity;
        uiFogHeight = skySettings.fogHeight;
        uiFogHeightFalloff = skySettings.fogHeightFalloff;
        lastUiSkySettings = skySettings;
        skyUiInitialized = true;
    }

    bool selectionIsTexture = false;
    if (!fileBrowser.selectedFile.empty() && fs::exists(fileBrowser.selectedFile)) {
        selectionIsTexture = fileBrowser.getFileCategory(fs::directory_entry(fileBrowser.selectedFile)) == FileCategory::Texture;
    }

    auto beginSettingsTable = [&] (const char* id) {
        const float width = ImGui::GetContentRegionAvail().x;
        const float labelWidth = std::clamp(width * 0.28f, 78.0f, 118.0f);
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
            return false;
        }
        ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed, labelWidth);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        return true;
    };

    auto drawSettingRow = [&] (const char* label, const std::function<void()>& drawControl) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        drawControl();
    };

    auto drawSection = [] (const char* label) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", label);
    };

    auto applySkySettings = [&] () {
        SkyboxSettings updated = skySettings;
        updated.mode = (uiSkyMode == static_cast<int>(SkyboxMode::Scrolling)) ? SkyboxMode::Scrolling : SkyboxMode::Procedural;
        updated.sunTexturePath = sunTexBuf;
        updated.moonTexturePath = moonTexBuf;
        updated.scrollingTexturePath = scrollTexBuf;
        updated.scrollingRepeatX = uiScrollRepeat[0];
        updated.scrollingRepeatY = uiScrollRepeat[1];
        updated.scrollingLookSensitivity = uiScrollLookSensitivity;
        updated.scrollingVerticalInfluence = uiScrollVerticalInfluence;
        updated.cachedSky = uiCachedSky;
        updated.sunFollowsDirectionalLight = uiSunFollowsLight;
        updated.environmentReflections = uiEnvironmentReflections;
        updated.environmentReflectionIntensity = std::clamp(uiEnvironmentReflectionIntensity, 0.0f, 2.0f);
        updated.reflectionDistanceFadeStart = std::max(0.0f, uiReflectionDistanceFade[0]);
        updated.reflectionDistanceFadeEnd = std::max(updated.reflectionDistanceFadeStart + 0.01f, uiReflectionDistanceFade[1]);
        updated.fogEnabled = uiFogEnabled;
        updated.fogMode = std::clamp(uiFogMode, 0, 2);
        updated.fogColor = uiFogColor;
        updated.fogStart = std::max(0.0f, uiFogStart);
        updated.fogEnd = std::max(updated.fogStart + 0.01f, uiFogEnd);
        updated.fogDensity = std::clamp(uiFogDensity, 0.0f, 1.0f);
        updated.fogHeight = uiFogHeight;
        updated.fogHeightFalloff = std::clamp(uiFogHeightFalloff, 0.0f, 1.0f);
        updated.volumetricMaxIntensity = std::max(0.0f, uiVolumetricMaxIntensity);
        updated.volumetricDenoise = uiVolumetricDenoise;
        updated.volumetricDenoiseRadius = std::clamp(uiVolumetricDenoiseRadius, 0, 8);
        updated.cloudHorizonFade = std::clamp(uiCloudHorizonFade, 0.0f, 1.0f);
        updated.skyResolution = std::clamp(uiSkyResolution, 64, 2048);
        updated.skyRefreshInterval = std::clamp(uiSkyRefreshInterval, 0.0f, 5.0f);
        updated.cloudMode = std::clamp(uiCloudMode, 0, 1);
        updated.cloudCoverage = std::clamp(uiCloudCoverage, 0.0f, 1.0f);
        updated.cloudDensity = std::max(0.0f, uiCloudDensity);
        updated.cloudSteps = std::clamp(uiCloudSteps, 4, 64);
        updated.cloudLightSteps = std::clamp(uiCloudLightSteps, 1, 8);
        updated.cloudBottom = std::max(1.0f, uiCloudBottom);
        updated.cloudTop = std::max(uiCloudBottom + 1.0f, uiCloudTop);
        updated.cloudWind = std::max(0.0f, uiCloudWind);
        updated.weather.enabled = uiWeatherEnabled;
        updated.weather.mode = static_cast<WeatherMode>(std::clamp(uiWeatherMode, 0, 4));
        updated.weather.intensity = std::clamp(uiWeatherIntensity, 0.0f, 1.0f);
        updated.weather.windSpeed = std::max(0.0f, uiWeatherWindSpeed);
        updated.weather.rainOpacity = std::clamp(uiWeatherRainOpacity, 0.0f, 2.0f);
        updated.weather.rainSpeed = std::max(0.0f, uiWeatherRainSpeed);
        updated.weather.rainScale = std::max(0.05f, uiWeatherRainScale);
        updated.weather.lightningInterval = std::max(0.5f, uiWeatherLightningInterval);
        updated.weather.lightningIntensity = std::max(0.0f, uiWeatherLightningIntensity);
        updated.weather.ambienceVolume = std::clamp(uiWeatherAmbienceVolume, 0.0f, 1.0f);
        updated.weather.heavyRainThreshold = std::clamp(uiWeatherHeavyThreshold, 0.0f, 1.0f);
        updated.weather.clearSoundPath = uiWeatherClearSound;
        updated.weather.overcastSoundPath = uiWeatherOvercastSound;
        updated.weather.rainSoundPath = uiWeatherRainSound;
        updated.weather.heavyRainSoundPath = uiWeatherHeavyRainSound;
        updated.weather.thunderSoundPath = uiWeatherThunderSound;
        updated.volumetricEnabled = uiVolumetricEnabled;
        updated.volumetricIntensity = std::max(0.0f, uiVolumetricIntensity);
        updated.volumetricSteps = std::clamp(uiVolumetricSteps, 4, 256);
        updated.volumetricScattering = std::clamp(uiVolumetricScattering, -0.95f, 0.95f);
        updated.volumetricDensity = std::max(0.0f, uiVolumetricDensity);
        updated.volumetricMaxDistance = std::max(0.01f, uiVolumetricMaxDistance);
        updated.volumetricHalfResolution = uiVolumetricHalfResolution;
        updated.ssrEnabled = uiSsrEnabled;
        updated.ssrIntensity = std::max(0.0f, uiSsrIntensity);
        updated.ssrSteps = std::clamp(uiSsrSteps, 8, 256);
        updated.ssrRefineSteps = std::clamp(uiSsrRefineSteps, 0, 16);
        updated.ssrMaxDistance = std::max(0.01f, uiSsrMaxDistance);
        updated.ssrThickness = std::max(0.001f, uiSsrThickness);
        updated.ssrMaxRoughness = std::clamp(uiSsrMaxRoughness, 0.0f, 1.0f);
        updated.ssrHalfResolution = uiSsrHalfResolution;
        updated.giEnabled = uiGiEnabled;
        updated.giIntensity = std::max(0.0f, uiGiIntensity);
        updated.giResolution = std::clamp(uiGiResolution, 32, 256);
        updated.giMaxExtent = std::max(1.0f, uiGiMaxExtent);
        updated.giConeSteps = std::clamp(uiGiConeSteps, 4, 64);
        updated.giBounces = std::clamp(uiGiBounces, 1, 4);
        applySceneSkyboxSettings(updated);
        projectManager.currentProject.hasUnsavedChanges = true;
        lastUiSkySettings = updated;
    };

    auto drawWeatherSoundRow = [&] (const char* label, const char* id, std::string& path) {
        drawSettingRow(label, [&] {
            char buffer[512] = {};
            std::snprintf(buffer, sizeof(buffer), "%s", path.c_str());
            if (ImGui::InputTextWithHint(id, "Drop an audio clip here", buffer, sizeof(buffer))) {
                path = buffer;
                applySkySettings();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATH")) {
                    path = static_cast<const char*>(payload->Data);
                    applySkySettings();
                }
                ImGui::EndDragDropTarget();
            }
            if (!path.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton((std::string("x") + id).c_str())) {
                    path.clear();
                    applySkySettings();
                }
            }
        });
    };

    const char* skyModeLabels[] = { "Procedural Day/Night", "Scrolling 2.5D Background" };
    const char* fogModeLabels[] = { "Linear", "Exponential", "Exponential Squared" };

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f, 2.0f));
    if (ImGui::BeginTabBar("##EnvironmentTabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        if (ImGui::BeginTabItem("Skydome")) {
            if (beginSettingsTable("##EnvironmentSkyTable")) {
                drawSettingRow("Day / Night", [&] {
                    if (ImGui::SliderFloat("##EnvDayNight", &tod, 0.0f, 1.0f, "%.2f")) {
                        applySceneTimeOfDay(tod);
                        projectManager.currentProject.hasUnsavedChanges = true;
                    }
                });
                drawSettingRow("Mode", [&] {
                    ImGui::Combo("##SkyMode", &uiSkyMode, skyModeLabels, IM_ARRAYSIZE(skyModeLabels));
                });
                drawSettingRow("Cached Sky", [&] {
                    if (ImGui::Checkbox("##CachedSky", &uiCachedSky)) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Bake the procedural sky into a cubemap refreshed a few times per second\n"
                                          "instead of evaluating it per pixel every frame. Large speedup; star twinkle\n"
                                          "and cloud motion update in small steps.");
                    }
                });
                drawSettingRow("Sun Follows Light", [&] {
                    if (ImGui::Checkbox("##SunFollowsLight", &uiSunFollowsLight)) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Place the sun and moon where the scene's directional light points from,\n"
                                          "instead of on the Day / Night arc. The first enabled Directional light in\n"
                                          "the hierarchy wins - the same one the renderer treats as the sun - so the\n"
                                          "sky and the shading always agree. Day / Night still drives sky colour.");
                    }
                });
                drawSettingRow("Repeat", [&] {
                    ImGui::DragFloat2("##ScrollRepeat", uiScrollRepeat, 0.05f, 0.01f, 32.0f, "%.2f");
                });
                drawSettingRow("Look", [&] {
                    ImGui::SliderFloat("##LookSensitivity", &uiScrollLookSensitivity, 0.0f, 4.0f, "%.2f");
                });
                drawSettingRow("Vertical", [&] {
                    ImGui::SliderFloat("##VerticalLookSensitivity", &uiScrollVerticalInfluence, 0.0f, 1.0f, "%.2f");
                });
                ImGui::EndTable();
            }

            drawSection("Textures");
            if (beginSettingsTable("##EnvironmentTextureTable")) {
                drawSettingRow("Sun", [&] {
                    ImGui::InputText("##SunTexture", sunTexBuf, sizeof(sunTexBuf));
                });
                drawSettingRow("Moon", [&] {
                    ImGui::InputText("##MoonTexture", moonTexBuf, sizeof(moonTexBuf));
                });
                drawSettingRow("Background", [&] {
                    ImGui::InputText("##ScrollingBackground", scrollTexBuf, sizeof(scrollTexBuf));
                });
                drawSettingRow("Use Selection", [&] {
                    ImGui::BeginDisabled(!selectionIsTexture);
                    if (ImGui::Button("Sun")) {
                        std::snprintf(sunTexBuf, sizeof(sunTexBuf), "%s", fileBrowser.selectedFile.string().c_str());
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Moon")) {
                        std::snprintf(moonTexBuf, sizeof(moonTexBuf), "%s", fileBrowser.selectedFile.string().c_str());
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Background")) {
                        std::snprintf(scrollTexBuf, sizeof(scrollTexBuf), "%s", fileBrowser.selectedFile.string().c_str());
                    }
                    ImGui::EndDisabled();
                });
                drawSettingRow("Actions", [&] {
                    if (ImGui::Button("Built-in Sun/Moon")) {
                        std::snprintf(sunTexBuf, sizeof(sunTexBuf), "%s", "Resources/Engine-Root/Skybox/Sun Skybox.png");
                        std::snprintf(moonTexBuf, sizeof(moonTexBuf), "%s", "Resources/Engine-Root/Skybox/Moon Skybox.png");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Apply")) {
                        applySkySettings();
                    }
                });
                ImGui::EndTable();
            }

            if (skybox && !vulkanPreviewMode) {
                static char skyVertBuf[256] = {};
                static char skyFragBuf[256] = {};
                if (skyVertBuf[0] == '\0') std::snprintf(skyVertBuf, sizeof(skyVertBuf), "%s", skybox->getVertPath().c_str());
                if (skyFragBuf[0] == '\0') std::snprintf(skyFragBuf, sizeof(skyFragBuf), "%s", skybox->getFragPath().c_str());

                bool selectionIsShader = false;
                if (!fileBrowser.selectedFile.empty() && fs::exists(fileBrowser.selectedFile)) {
                    selectionIsShader = fileBrowser.getFileCategory(fs::directory_entry(fileBrowser.selectedFile)) == FileCategory::Shader;
                }

                drawSection("Skydome Shader");
                if (beginSettingsTable("##EnvironmentShaderTable")) {
                    drawSettingRow("Vertex", [&] {
                        ImGui::InputText("##SkyVert", skyVertBuf, sizeof(skyVertBuf));
                    });
                    drawSettingRow("Fragment", [&] {
                        ImGui::InputText("##SkyFrag", skyFragBuf, sizeof(skyFragBuf));
                    });
                    drawSettingRow("Use Selection", [&] {
                        ImGui::BeginDisabled(!selectionIsShader);
                        if (ImGui::Button("Vertex")) {
                            std::snprintf(skyVertBuf, sizeof(skyVertBuf), "%s", fileBrowser.selectedFile.string().c_str());
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Fragment")) {
                            std::snprintf(skyFragBuf, sizeof(skyFragBuf), "%s", fileBrowser.selectedFile.string().c_str());
                        }
                        ImGui::EndDisabled();
                    });
                    drawSettingRow("Actions", [&] {
                        if (ImGui::Button("Reload")) {
                            skybox->setShaderPaths(skyVertBuf, skyFragBuf);
                        }
                    });
                    ImGui::EndTable();
                }
            } else if (vulkanPreviewMode) {
                drawSection("Skydome Shader");
                ImGui::TextWrapped("Built-in Vulkan sky rendering is active. Custom OpenGL Skydome shader paths are edited only in OpenGL mode.");
            } else {
                ImGui::TextDisabled("Skydome not available in this backend");
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Environment")) {
            ImGui::TextDisabled("Reflections");
            if (beginSettingsTable("##EnvironmentReflectionTable")) {
                drawSettingRow("Enabled", [&] {
                    ImGui::Checkbox("##EnvironmentReflections", &uiEnvironmentReflections);
                });
                ImGui::BeginDisabled(!uiEnvironmentReflections);
                drawSettingRow("Intensity", [&] {
                    ImGui::SliderFloat("##ReflectionIntensity", &uiEnvironmentReflectionIntensity, 0.0f, 2.0f, "%.2f");
                });
                drawSettingRow("Distance", [&] {
                    if (ImGui::DragFloat2("##ReflectionDistanceFade", uiReflectionDistanceFade, 0.25f, 0.0f, 10000.0f, "%.1f")) {
                        uiReflectionDistanceFade[0] = std::max(0.0f, uiReflectionDistanceFade[0]);
                        uiReflectionDistanceFade[1] = std::max(uiReflectionDistanceFade[0] + 0.01f, uiReflectionDistanceFade[1]);
                    }
                });
                ImGui::EndDisabled();
                ImGui::EndTable();
            }

            drawSection("Fog");
            if (beginSettingsTable("##EnvironmentFogTable")) {
                drawSettingRow("Enabled", [&] {
                    ImGui::Checkbox("##EnableFog", &uiFogEnabled);
                });
                ImGui::BeginDisabled(!uiFogEnabled);
                drawSettingRow("Mode", [&] {
                    ImGui::Combo("##FogMode", &uiFogMode, fogModeLabels, IM_ARRAYSIZE(fogModeLabels));
                });
                drawSettingRow("Color", [&] {
                    ImGui::ColorEdit3("##FogColor", &uiFogColor.x, ImGuiColorEditFlags_DisplayRGB);
                });
                drawSettingRow("Start", [&] {
                    if (ImGui::DragFloat("##FogStart", &uiFogStart, 0.25f, 0.0f, 10000.0f, "%.1f")) {
                        uiFogStart = std::max(0.0f, uiFogStart);
                        uiFogEnd = std::max(uiFogStart + 0.01f, uiFogEnd);
                    }
                });
                drawSettingRow("End", [&] {
                    if (ImGui::DragFloat("##FogEnd", &uiFogEnd, 0.25f, 0.01f, 10000.0f, "%.1f")) {
                        uiFogEnd = std::max(uiFogStart + 0.01f, uiFogEnd);
                    }
                });
                drawSettingRow("Density", [&] {
                    ImGui::SliderFloat("##FogDensity", &uiFogDensity, 0.0f, 0.2f, "%.3f");
                });
                drawSettingRow("Height", [&] {
                    ImGui::DragFloat("##FogHeight", &uiFogHeight, 0.05f, -10000.0f, 10000.0f, "%.2f");
                });
                drawSettingRow("Falloff", [&] {
                    ImGui::SliderFloat("##FogHeightFalloff", &uiFogHeightFalloff, 0.0f, 1.0f, "%.2f");
                });
                ImGui::EndDisabled();
                ImGui::EndTable();
            }

            drawSection("Realtime Global Illumination");
            {
                const std::string giReason = renderer.giUnsupportedReason();
                const bool giAvailable = giReason.empty();
                if (!giAvailable) {
                    ImGui::TextWrapped("Not available: %s.", giReason.c_str());
                }
                ImGui::BeginDisabled(!giAvailable);
                if (beginSettingsTable("##EnvironmentGiTable")) {
                    drawSettingRow("Enabled", [&] {
                        if (ImGui::Checkbox("##EnableGi", &uiGiEnabled)) {
                            applySkySettings();
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("Voxel cone traced bounce light. The scene is rasterized into a\n"
                                              "radiance volume, and every lit surface gathers indirect light\n"
                                              "from it. The volume is rebuilt only when geometry, materials\n"
                                              "or lights change - moving the camera alone costs nothing.\n"
                                              "\n"
                                              "Works alongside baked lightmaps rather than replacing them:\n"
                                              "bakes are ordinary textures and are not counted as a separate\n"
                                              "lighting term, so turn Intensity down on a lightmapped scene\n"
                                              "and this fills in the bounce the bake could not see.");
                        }
                    });
                    ImGui::BeginDisabled(!uiGiEnabled);
                    drawSettingRow("Intensity", [&] {
                        if (ImGui::SliderFloat("##GiIntensity", &uiGiIntensity, 0.0f, 4.0f, "%.2f")) {
                            applySkySettings();
                        }
                    });
                    drawSettingRow("Resolution", [&] {
                        const char* giResLabels[] = { "32", "64", "128", "256" };
                        const int giResValues[] = { 32, 64, 128, 256 };
                        int index = 2;
                        for (int i = 0; i < 4; ++i) {
                            if (giResValues[i] == uiGiResolution) index = i;
                        }
                        if (ImGui::Combo("##GiResolution", &index, giResLabels, 4)) {
                            uiGiResolution = giResValues[std::clamp(index, 0, 3)];
                            applySkySettings();
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("Voxels per axis. The volume is float, so memory is this cubed\n"
                                              "times eight bytes plus a mip chain: 128 is about 21 MB and\n"
                                              "256 about 150 MB. Raise it for finer contact between\n"
                                              "surfaces, not for brightness.");
                        }
                    });
                    drawSettingRow("Max Extent", [&] {
                        if (ImGui::DragFloat("##GiMaxExtent", &uiGiMaxExtent, 0.5f, 1.0f, 2000.0f, "%.1f")) {
                            uiGiMaxExtent = std::max(1.0f, uiGiMaxExtent);
                            applySkySettings();
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("The volume is centred on the scene and sized to fit it, up to\n"
                                              "this. A scene larger than this is not covered at the edges -\n"
                                              "raise it, or accept GI only through the middle.");
                        }
                    });
                    drawSettingRow("Bounces", [&] {
                        if (ImGui::SliderInt("##GiBounces", &uiGiBounces, 1, 4)) {
                            applySkySettings();
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("How many times light is allowed to bounce. Each pass\n"
                                              "rasterizes the scene again gathering from the previous\n"
                                              "one, so a wall lit on bounce 1 becomes a light source\n"
                                              "on bounce 2 - this is what fills a room lit only\n"
                                              "through a window.\n"
                                              "\n"
                                              "Paid on rebuild, not per frame. Above 1 allocates a\n"
                                              "second volume, doubling the memory above.");
                        }
                    });
                    drawSettingRow("Cone Steps", [&] {
                        if (ImGui::SliderInt("##GiConeSteps", &uiGiConeSteps, 4, 64)) {
                            applySkySettings();
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("How far each of the six cones marches. This is the per-pixel\n"
                                              "cost of GI; the volume bake is separate and far rarer.");
                        }
                    });
                    ImGui::EndDisabled();
                    ImGui::EndTable();
                }
                ImGui::EndDisabled();
            }

            drawSection("Screen Space Reflections");
            if (beginSettingsTable("##EnvironmentSsrTable")) {
                drawSettingRow("Enabled", [&] {
                    if (ImGui::Checkbox("##EnableSsr", &uiSsrEnabled)) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Reflects what is already on screen, tracing the depth buffer.\n"
                                          "Falls back to the reflection cube above wherever the ray\n"
                                          "leaves the frame, so enabling those too is worth it.\n"
                                          "Costs an extra pass over opaque geometry while on.");
                    }
                });
                ImGui::BeginDisabled(!uiSsrEnabled);
                drawSettingRow("Intensity", [&] {
                    if (ImGui::SliderFloat("##SsrIntensity", &uiSsrIntensity, 0.0f, 2.0f, "%.2f")) {
                        applySkySettings();
                    }
                });
                drawSettingRow("Distance", [&] {
                    if (ImGui::DragFloat("##SsrMaxDistance", &uiSsrMaxDistance, 0.25f, 0.5f, 1000.0f, "%.1f")) {
                        uiSsrMaxDistance = std::max(0.01f, uiSsrMaxDistance);
                        applySkySettings();
                    }
                });
                drawSettingRow("Max Roughness", [&] {
                    if (ImGui::SliderFloat("##SsrMaxRoughness", &uiSsrMaxRoughness, 0.0f, 1.0f, "%.2f")) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Rougher surfaces than this are left to the reflection cube.\n"
                                          "Screen-space tracing is a mirror technique - one ray\n"
                                          "blurred is a smear, not a rough reflection.");
                    }
                });
                drawSettingRow("Thickness", [&] {
                    if (ImGui::SliderFloat("##SsrThickness", &uiSsrThickness, 0.01f, 5.0f, "%.2f")) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("How far behind a surface a ray may land and still count as\n"
                                          "hitting it. Too low breaks reflections up on grazing\n"
                                          "geometry; too high lets rays hit the back of thin objects.");
                    }
                });
                drawSettingRow("Steps", [&] {
                    if (ImGui::SliderInt("##SsrSteps", &uiSsrSteps, 8, 256)) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("A ceiling, not a fixed cost. The march takes one step per\n"
                                          "pixel the reflection crosses on screen, so short\n"
                                          "reflections are cheap however high this is - raise it\n"
                                          "until long reflections stop breaking up.");
                    }
                });
                drawSettingRow("Refine", [&] {
                    if (ImGui::SliderInt("##SsrRefineSteps", &uiSsrRefineSteps, 0, 16)) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Binary-search passes that sharpen a crossing once the march\n"
                                          "has stepped through a surface. Far cheaper than raising\n"
                                          "Steps, and this is what removes the staircase.");
                    }
                });
                drawSettingRow("Half Res", [&] {
                    if (ImGui::Checkbox("##SsrHalfRes", &uiSsrHalfResolution)) {
                        applySkySettings();
                    }
                });
                ImGui::EndDisabled();
                ImGui::EndTable();
            }

            drawSection("Clouds");
            if (beginSettingsTable("##EnvironmentCloudTable")) {
                drawSettingRow("Sky Resolution", [&] {
                    const int options[] = { 64, 128, 256, 512, 1024 };
                    const char* labels[] = { "64", "128", "256 (recommended)", "512", "1024" };
                    int idx = 2;
                    for (int i = 0; i < 5; ++i) {
                        if (options[i] == uiSkyResolution) { idx = i; break; }
                    }
                    if (ImGui::Combo("##SkyResolution", &idx, labels, IM_ARRAYSIZE(labels))) {
                        uiSkyResolution = options[std::clamp(idx, 0, 4)];
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Cube face size for the cached sky. The cloud march runs per\n"
                                          "baked pixel across six faces, so this is the biggest cost\n"
                                          "lever there is - 1024 is 16x the work of 256 for a sky that\n"
                                          "is mostly smooth gradient either way.");
                    }
                });
                drawSettingRow("Sky Refresh", [&] {
                    if (ImGui::SliderFloat("##SkyRefresh", &uiSkyRefreshInterval, 0.0f, 1.0f, "%.2f s")) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("How often the whole cube is rebuilt. All six faces are baked\n"
                                          "together - lower is smoother cloud motion and more cost.");
                    }
                });
                drawSettingRow("Mode", [&] {
                    const char* modes[] = { "Flat band (cheap)", "Volumetric (ray-marched)" };
                    if (ImGui::Combo("##CloudMode", &uiCloudMode, modes, IM_ARRAYSIZE(modes))) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Volumetric marches a cloud slab with a light march toward\n"
                                          "the sun, so clouds have interior shading and a silver lining\n"
                                          "instead of being a flat sheet.\n"
                                          "\n"
                                          "Far more expensive per pixel - keep Cached Sky on, which\n"
                                          "bakes the sky a cubemap face at a time instead of paying\n"
                                          "for it per screen pixel every frame.");
                    }
                });
                ImGui::BeginDisabled(uiCloudMode != 1);
                drawSettingRow("Coverage", [&] {
                    if (ImGui::SliderFloat("##CloudCoverage", &uiCloudCoverage, 0.0f, 1.0f, "%.2f")) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Overridden while Weather is enabled - the mode drives it.");
                    }
                });
                drawSettingRow("Density", [&] {
                    if (ImGui::SliderFloat("##CloudDensity", &uiCloudDensity, 0.0f, 3.0f, "%.2f")) {
                        applySkySettings();
                    }
                });
                drawSettingRow("Base Height", [&] {
                    if (ImGui::DragFloat("##CloudBottom", &uiCloudBottom, 10.0f, 1.0f, 20000.0f, "%.0f")) {
                        applySkySettings();
                    }
                });
                drawSettingRow("Top Height", [&] {
                    if (ImGui::DragFloat("##CloudTop", &uiCloudTop, 10.0f, 2.0f, 40000.0f, "%.0f")) {
                        applySkySettings();
                    }
                });
                drawSettingRow("Wind", [&] {
                    if (ImGui::SliderFloat("##CloudWind", &uiCloudWind, 0.0f, 5.0f, "%.2f")) {
                        applySkySettings();
                    }
                });
                drawSettingRow("Horizon Fade", [&] {
                    if (ImGui::SliderFloat("##CloudHorizonFade", &uiCloudHorizonFade, 0.0f, 0.6f, "%.3f")) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Fades clouds out toward the horizon, so the finite edge of\n"
                                          "the layer and the point where the noise repeats dissolve\n"
                                          "into haze instead of visibly ending.");
                    }
                });
                drawSettingRow("Steps", [&] {
                    if (ImGui::SliderInt("##CloudSteps", &uiCloudSteps, 4, 64)) {
                        applySkySettings();
                    }
                });
                drawSettingRow("Light Steps", [&] {
                    if (ImGui::SliderInt("##CloudLightSteps", &uiCloudLightSteps, 1, 8)) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Samples toward the sun per cloud sample. This is what gives\n"
                                          "a cloud an interior; at 1 it reads as a flat cut-out.");
                    }
                });
                ImGui::EndDisabled();
                ImGui::EndTable();
            }

            drawSection("Weather");
            if (beginSettingsTable("##EnvironmentWeatherTable")) {
                drawSettingRow("Enabled", [&] {
                    if (ImGui::Checkbox("##EnableWeather", &uiWeatherEnabled)) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Drives cloud cover, rain, lightning and an ambience track\n"
                                          "from one control. Off by default, and while off it writes\n"
                                          "to none of them - a scene keeps whatever it was authored with.");
                    }
                });
                ImGui::BeginDisabled(!uiWeatherEnabled);
                drawSettingRow("Condition", [&] {
                    const char* modes[] = { "Clear", "Overcast", "Dark Cloudy", "Rain", "Thunderstorm" };
                    if (ImGui::Combo("##WeatherMode", &uiWeatherMode, modes, IM_ARRAYSIZE(modes))) {
                        applySkySettings();
                    }
                });
                drawSettingRow("Intensity", [&] {
                    if (ImGui::SliderFloat("##WeatherIntensity", &uiWeatherIntensity, 0.0f, 1.0f, "%.2f")) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Blends from clear toward the chosen condition, so light rain\n"
                                          "and a downpour are one mode at two strengths.");
                    }
                });
                drawSettingRow("Wind Speed", [&] {
                    if (ImGui::SliderFloat("##WeatherWind", &uiWeatherWindSpeed, 0.0f, 4.0f, "%.2f")) {
                        applySkySettings();
                    }
                });
                drawSettingRow("Rain Opacity", [&] {
                    if (ImGui::SliderFloat("##WeatherRainOpacity", &uiWeatherRainOpacity, 0.0f, 2.0f, "%.2f")) {
                        applySkySettings();
                    }
                });
                drawSettingRow("Rain Speed", [&] {
                    if (ImGui::SliderFloat("##WeatherRainSpeed", &uiWeatherRainSpeed, 0.0f, 6.0f, "%.2f")) {
                        applySkySettings();
                    }
                });
                drawSettingRow("Rain Scale", [&] {
                    if (ImGui::SliderFloat("##WeatherRainScale", &uiWeatherRainScale, 0.05f, 4.0f, "%.2f")) {
                        applySkySettings();
                    }
                });
                drawSettingRow("Lightning Interval", [&] {
                    if (ImGui::SliderFloat("##WeatherLightningInterval", &uiWeatherLightningInterval, 0.5f, 60.0f, "%.1f s")) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Average gap between strikes. The real gap is randomised\n"
                                          "around it so a storm does not tick like a metronome.");
                    }
                });
                drawSettingRow("Lightning Intensity", [&] {
                    if (ImGui::SliderFloat("##WeatherLightningIntensity", &uiWeatherLightningIntensity, 0.0f, 8.0f, "%.2f")) {
                        applySkySettings();
                    }
                });
                ImGui::EndDisabled();
                ImGui::EndTable();
            }

            drawSection("Weather Sounds");
            if (beginSettingsTable("##EnvironmentWeatherSoundTable")) {
                ImGui::BeginDisabled(!uiWeatherEnabled);
                drawSettingRow("Volume", [&] {
                    if (ImGui::SliderFloat("##WeatherVolume", &uiWeatherAmbienceVolume, 0.0f, 1.0f, "%.2f")) {
                        applySkySettings();
                    }
                });
                drawSettingRow("Heavy Rain At", [&] {
                    if (ImGui::SliderFloat("##WeatherHeavyThreshold", &uiWeatherHeavyThreshold, 0.0f, 1.0f, "%.2f")) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Intensity at which Rain swaps to the heavy loop.");
                    }
                });
                drawWeatherSoundRow("Clear", "##WxClear", uiWeatherClearSound);
                drawWeatherSoundRow("Overcast", "##WxOvercast", uiWeatherOvercastSound);
                drawWeatherSoundRow("Rain", "##WxRain", uiWeatherRainSound);
                drawWeatherSoundRow("Heavy Rain", "##WxHeavyRain", uiWeatherHeavyRainSound);
                drawWeatherSoundRow("Thunder", "##WxThunder", uiWeatherThunderSound);
                ImGui::EndDisabled();
                ImGui::EndTable();
            }

            drawSection("Volumetric Lighting");
            if (beginSettingsTable("##EnvironmentVolumetricTable")) {
                drawSettingRow("Enabled", [&] {
                    if (ImGui::Checkbox("##EnableVolumetric", &uiVolumetricEnabled)) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Light scattering in the air between the camera and what it sees:\n"
                                          "god rays, visible spot cones, haze picking up the sun.\n"
                                          "Only shadow-casting lights take part - the effect is their\n"
                                          "shadow map sampled along the view ray, so a light without one\n"
                                          "has nothing to cut its beams.");
                    }
                });
                ImGui::BeginDisabled(!uiVolumetricEnabled);
                drawSettingRow("Intensity", [&] {
                    if (ImGui::SliderFloat("##VolumetricIntensity", &uiVolumetricIntensity, 0.0f, 4.0f, "%.2f")) {
                        applySkySettings();
                    }
                });
                drawSettingRow("Density", [&] {
                    if (ImGui::SliderFloat("##VolumetricDensity", &uiVolumetricDensity, 0.0f, 0.5f, "%.3f")) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("How much medium there is per world unit. Prefer this over\n"
                                          "Intensity for the overall level - it is the physical knob.");
                    }
                });
                drawSettingRow("Scattering", [&] {
                    if (ImGui::SliderFloat("##VolumetricScattering", &uiVolumetricScattering, -0.95f, 0.95f, "%.2f")) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Forward scattering. Positive makes beams blaze when you look\n"
                                          "toward the light and fade when you look away; 0 scatters evenly.");
                    }
                });
                drawSettingRow("Distance", [&] {
                    if (ImGui::DragFloat("##VolumetricMaxDistance", &uiVolumetricMaxDistance, 0.5f, 1.0f, 10000.0f, "%.1f")) {
                        uiVolumetricMaxDistance = std::max(0.01f, uiVolumetricMaxDistance);
                        applySkySettings();
                    }
                });
                drawSettingRow("Max Intensity", [&] {
                    if (ImGui::SliderFloat("##VolumetricMaxIntensity", &uiVolumetricMaxIntensity, 0.0f, 8.0f, "%.2f")) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Ceiling on in-scattered light, rolled off smoothly rather\n"
                                          "than clipped. The pass only ever ADDS light, so a shaft\n"
                                          "crossing an already-bright surface blows out and then blooms.\n"
                                          "0 removes the limit.");
                    }
                });
                drawSettingRow("Denoise", [&] {
                    if (ImGui::Checkbox("##VolumetricDenoise", &uiVolumetricDenoise)) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Depth-aware blur over the volumetric buffer. The march\n"
                                          "dithers each ray so its error lands as grain rather than\n"
                                          "rings; this clears the grain, which is what makes a low\n"
                                          "step count usable.");
                    }
                });
                drawSettingRow("Denoise Radius", [&] {
                    ImGui::BeginDisabled(!uiVolumetricDenoise);
                    if (ImGui::SliderInt("##VolumetricDenoiseRadius", &uiVolumetricDenoiseRadius, 1, 8)) {
                        applySkySettings();
                    }
                    ImGui::EndDisabled();
                });
                drawSettingRow("Steps", [&] {
                    if (ImGui::SliderInt("##VolumetricSteps", &uiVolumetricSteps, 4, 256)) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Samples along each view ray. Cost is this times the number of\n"
                                          "shadowed lights, per pixel - raise it only until the banding\n"
                                          "stops, because past that it buys nothing.");
                    }
                });
                drawSettingRow("Half Res", [&] {
                    if (ImGui::Checkbox("##VolumetricHalfRes", &uiVolumetricHalfResolution)) {
                        applySkySettings();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("March at half width and height and upsample. About a quarter of\n"
                                          "the cost, and rarely visible - the effect is low-frequency.");
                    }
                });
                ImGui::EndDisabled();
                ImGui::EndTable();
            }

            drawSection("Global Ambient");
            glm::vec3 ambient = renderer.getAmbientColor();
            if (beginSettingsTable("##EnvironmentAmbientTable")) {
                drawSettingRow("Color", [&] {
                    if (ImGui::ColorEdit3("##AmbientColor", &ambient.x, ImGuiColorEditFlags_DisplayRGB)) {
                        renderer.setAmbientColor(ambient);
                        projectManager.currentProject.hasUnsavedChanges = true;
                    }
                });
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Lighting")) {
            int active3DLights = 0;
            for (const SceneObject& obj : sceneObjects) {
                if (IsObjectEnabledInHierarchy(obj) && obj.hasLight && obj.light.enabled && obj.light.intensity > 0.0f) {
                    ++active3DLights;
                }
            }

            ImGui::TextDisabled("Realtime Lighting");
            if (beginSettingsTable("##EnvironmentLightingTable")) {
                int maxLights = renderer.getMaxRealtimeLights();
                drawSettingRow("Max 3D Lights", [&] {
                    if (ImGui::SliderInt("##MaxRealtimeLights", &maxLights, 1, kRendererMaxRealtimeLights)) {
                        renderer.setMaxRealtimeLights(maxLights);
                        saveEditorUserSettings();
                    }
                });
                int shadowResolution = renderer.getShadowMapResolution();
                drawSettingRow("Shadow Res", [&] {
                    if (ImGui::DragInt("##DefaultShadowResolution", &shadowResolution, 64.0f, 128, 4096)) {
                        renderer.setShadowMapResolution(shadowResolution);
                    }
                });
                bool shaderAutoReload = renderer.isShaderAutoReloadEnabled();
                drawSettingRow("Shader Reload", [&] {
                    if (ImGui::Checkbox("##ShaderAutoReload", &shaderAutoReload)) {
                        renderer.setShaderAutoReload(shaderAutoReload);
                    }
                });
                drawSettingRow("Lighting Res", [&] {
                    bool resChanged = ImGui::Checkbox("Auto##Light2DResAuto", &buildSettings.light2DLightingResolutionAuto);
                    ImGui::SameLine();
                    if (ImGui::SliderFloat("##Light2DRes", &buildSettings.light2DLightingResolution, 0.25f, 1.0f, "%.2fx")) {
                        buildSettings.light2DLightingResolution = std::clamp(buildSettings.light2DLightingResolution, 0.25f, 1.0f);
                        resChanged = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Internal render scale for the 2D light buffers (ships with the game).\n"
                                          "Auto: adaptive downscale at high resolution / many lights.\n"
                                          "Off: render lighting at exactly this scale.");
                    }
                    if (resChanged) saveBuildSettings();
                });
                drawSettingRow("Lighting Filter", [&] {
                    if (ImGui::Checkbox("Pixel (Nearest)##Light2DPixelFilter", &buildSettings.light2DLightingPixelFilter)) {
                        saveBuildSettings();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("How the 2D lighting output is filtered (ships with the game).\n"
                                          "Off: bilinear (smooth light).\n"
                                          "On: nearest — crisp pixel-art light, and skips the bilinear taps when\n"
                                          "sampling the light buffers (a little cheaper per pixel).");
                    }
                });
                drawSettingRow("Lighting Depth", [&] {
                    if (ImGui::Checkbox("SDR 8-bit##Light2DSDR", &buildSettings.light2DLightingSDR)) {
                        saveBuildSettings();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Internal format of the 2D lighting buffers (ships with the game).\n"
                                          "Off: HDR RGBA16F.\n"
                                          "On: SDR RGBA8 — halves the lighting-buffer bandwidth (runs faster),\n"
                                          "but overbright/additive light is clamped at white (no HDR overshoot).");
                    }
                });
                drawSettingRow("Active 3D", [&] {
                    ImGui::Text("%d / %d", active3DLights, renderer.getMaxRealtimeLights());
                });
                drawSettingRow("Active 2D", [&] {
                    ImGui::Text("%d", light2DActiveCountLastFrame);
                });
                drawSettingRow("Shadow Maps", [&] {
                    ImGui::Text("4 realtime shadow maps");
                });
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar(3);

    ImGui::End();
}
#pragma endregion

#pragma region Camera Window
void Engine::renderCameraWindow() {
    if (!showCameraWindow) return;
    ImGui::Begin(Loc::Window("WINDOW_CAMERA", "Camera"), &showCameraWindow);

    ImGui::TextDisabled("Movement");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat("Base Speed", &camera.moveSpeed, 0.1f, 0.1f, 100.0f, "%.2f")) {
        camera.moveSpeed = std::max(0.01f, camera.moveSpeed);
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat("Sprint Speed", &camera.sprintSpeed, 0.1f, 0.1f, 200.0f, "%.2f")) {
        camera.sprintSpeed = std::max(camera.moveSpeed, camera.sprintSpeed);
    }
    ImGui::Checkbox("Smooth Movement", &camera.smoothMovement);
    ImGui::BeginDisabled(!camera.smoothMovement);
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("Acceleration", &camera.acceleration, 0.1f, 0.1f, 100.0f, "%.2f");
    ImGui::EndDisabled();

    ImGui::End();
}
#pragma endregion
