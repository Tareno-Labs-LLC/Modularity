#include "../EditorLocalization.h"
#include "Engine.h"

#include <array>
#include <cstdint>
#include <unordered_set>

namespace Loc = Modularity::Loc;

namespace {
enum class ModupakManagerView {
    All = 0,
    Project,
    Global,
    Updates,
    Incompatible,
    Count
};

enum class ModupakManagerSort {
    NameAsc = 0,
    NameDesc,
    VersionDesc,
    Status,
    Count
};

struct ViewTab {
    ModupakManagerView view;
    const char* key;
    const char* label;
    const char* iconPath;
};

constexpr std::array<ViewTab, 5> kViewTabs = {{
    {ModupakManagerView::All,          "MODUPAK_TAB_ALL_PACKAGES", "All Packages", "Resources/Engine-Root/ModuPAK Manager/All Packages.png"},
    {ModupakManagerView::Project,      "MODUPAK_TAB_IN_PROJECT",   "In Project",   "Resources/Engine-Root/ModuPAK Manager/In Project.png"},
    {ModupakManagerView::Global,       "MODUPAK_TAB_GLOBAL",       "Global",       "Resources/Engine-Root/ModuPAK Manager/Global.png"},
    {ModupakManagerView::Updates,      "MODUPAK_TAB_UPDATES",      "Updates",      "Resources/Engine-Root/ModuPAK Manager/Updates.png"},
    {ModupakManagerView::Incompatible, "MODUPAK_TAB_INCOMPATIBLE", "Incompatible", "Resources/Engine-Root/ModuPAK Manager/Incompatible.png"},
}};

struct PackageUiState {
    bool inProject = false;
    bool global = false;
    bool installed = false;
    bool compatible = true;
    bool updateAvailable = false;
    bool availableLocally = false;
    bool availableRemotely = false;
};

struct UnpackPreviewState {
    std::string packageId;
    std::string packageName;
    std::string packageVersion;
    bool reUnpack = false;
    bool isUpdate = false;
    std::string targetVersionLabel;
    fs::path sourcePath;
    fs::path destinationPath;
    std::string downloadUrl;
    std::vector<std::pair<std::string, std::uintmax_t>> entries;
    std::uintmax_t totalBytes = 0;
    bool truncated = false;
    bool sourceAvailable = false;
    std::string note;
};

UnpackPreviewState gUnpackPreview;
bool gOpenUnpackPreview = false;

float gTabIndicatorX = -1.0f;
float gTabIndicatorW = 0.0f;

std::unordered_set<std::string> gExpandedPackageIds;

float smoothApproach(float current, float target, float speed, float dt) {
    const float blend = 1.0f - std::exp(-speed * dt);
    return current + (target - current) * blend;
}

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool containsSearchTerm(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return toLowerCopy(haystack).find(needle) != std::string::npos;
}

bool packageMatchesSearch(const PackageInfo& pkg, const std::string& loweredSearch) {
    if (loweredSearch.empty()) return true;
    return containsSearchTerm(pkg.name, loweredSearch) ||
           containsSearchTerm(pkg.id, loweredSearch) ||
           containsSearchTerm(pkg.subsystem, loweredSearch) ||
           containsSearchTerm(pkg.packageType, loweredSearch) ||
           containsSearchTerm(pkg.author, loweredSearch);
}

std::vector<int> versionComponents(const std::string& version) {
    std::vector<int> parts;
    std::stringstream ss(version);
    std::string token;
    while (std::getline(ss, token, '.')) {
        try {
            parts.push_back(std::stoi(token));
        } catch (...) {
            parts.push_back(0);
        }
    }
    return parts;
}

int compareVersions(const std::string& lhs, const std::string& rhs) {
    const std::vector<int> a = versionComponents(lhs);
    const std::vector<int> b = versionComponents(rhs);
    const size_t count = std::max(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
        const int av = (i < a.size()) ? a[i] : 0;
        const int bv = (i < b.size()) ? b[i] : 0;
        if (av != bv) return (av < bv) ? -1 : 1;
    }
    return 0;
}

const char* sortLabel(ModupakManagerSort sortMode) {
    switch (sortMode) {
        case ModupakManagerSort::NameAsc:     return "Name (A-Z)";
        case ModupakManagerSort::NameDesc:    return "Name (Z-A)";
        case ModupakManagerSort::VersionDesc: return "Version (Newest)";
        case ModupakManagerSort::Status:      return "Status";
        case ModupakManagerSort::Count:       break;
    }
    return "Name (A-Z)";
}

PackageUiState buildPackageUiState(PackageManager& packageManager, const PackageInfo& pkg) {
    PackageUiState state;
    state.inProject = packageManager.isInstalled(pkg.id);
    state.global = packageManager.isGloballyInstalled(pkg.id);
    state.installed = state.inProject || state.global;
    state.compatible = packageManager.isCompatible(pkg);
    state.updateAvailable = packageManager.hasUpdateAvailable(pkg.id);
    state.availableLocally = !pkg.registrySourcePath.empty();
    state.availableRemotely = !pkg.downloadUrl.empty();
    return state;
}

bool matchesView(ModupakManagerView view, const PackageUiState& state) {
    switch (view) {
        case ModupakManagerView::All:          return true;
        case ModupakManagerView::Project:      return state.inProject;
        case ModupakManagerView::Global:       return state.global;
        case ModupakManagerView::Updates:      return state.updateAvailable;
        case ModupakManagerView::Incompatible: return !state.compatible;
        case ModupakManagerView::Count:        break;
    }
    return true;
}

std::string formatBytesShort(std::uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), unit == 0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return buf;
}

float drawBadge(ImDrawList* drawList,
                ImVec2 pos,
                const std::string& label,
                const ImVec4& bgColor,
                const ImVec4& textColor = ImVec4(0.96f, 0.97f, 0.99f, 1.0f)) {
    const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
    const ImVec2 padding(6.0f, 2.0f);
    const ImVec2 size(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
    drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                            ImGui::GetColorU32(bgColor), 4.0f);
    drawList->AddText(ImVec2(pos.x + padding.x, pos.y + padding.y - 1.0f),
                      ImGui::GetColorU32(textColor), label.c_str());
    return size.x;
}

std::string installedStateLabel(const PackageUiState& state) {
    if (state.inProject && state.global) return "Installed in Project and Global";
    if (state.inProject) return "Installed in Project";
    if (state.global) return "Installed Globally";
    return "Not Installed";
}

std::string sourceSummary(const PackageInfo& pkg, const PackageUiState& state) {
    if (state.availableLocally && state.availableRemotely) return "Local registry + online archive";
    if (state.availableLocally) return "Local registry checkout";
    if (state.availableRemotely) return "Online registry download";
    if (!pkg.gitUrl.empty()) return pkg.gitUrl;
    return "Package source unavailable";
}

bool packageHasAnyBadge(const PackageUiState& state) {
    return state.inProject || state.global || state.updateAvailable ||
           !state.compatible || !state.installed;
}

bool drawPackageRow(const PackageInfo& pkg,
                    const PackageUiState& state,
                    bool selected,
                    bool expanded,
                    bool* outChevronClicked) {
    if (outChevronClicked) *outChevronClicked = false;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    const float collapsedHeight = 44.0f;
    const float expandedHeight = 66.0f;
    const bool showBadges = expanded && packageHasAnyBadge(state);
    const float height = showBadges ? expandedHeight : collapsedHeight;

    ImGui::InvisibleButton((pkg.id + "##row").c_str(), ImVec2(width, height));
    ImGui::ReportItemHoverFeedback();
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImVec4 bg = ImVec4(0.13f, 0.15f, 0.20f, 0.0f);
    if (selected) bg = ImVec4(0.20f, 0.31f, 0.46f, 1.0f);
    else if (hovered) bg = ImVec4(0.17f, 0.21f, 0.28f, 0.85f);
    drawList->AddRectFilled(rect.Min, rect.Max, ImGui::GetColorU32(bg), 4.0f);
    if (selected) {
        drawList->AddRect(rect.Min, rect.Max,
                          ImGui::GetColorU32(ImVec4(0.43f, 0.75f, 0.98f, 0.85f)),
                          4.0f, 0, 1.2f);
    }

    const ImVec2 pad(10.0f, 7.0f);
    const std::string title = pkg.name.empty() ? pkg.id : pkg.name;
    const std::string version = pkg.version.empty() ? "" : ("v" + pkg.version);
    drawList->AddText(ImVec2(rect.Min.x + pad.x, rect.Min.y + pad.y),
                      ImGui::GetColorU32(ImVec4(0.96f, 0.97f, 0.99f, 1.0f)),
                      title.c_str());
    if (!version.empty()) {
        const ImVec2 versionSize = ImGui::CalcTextSize(version.c_str());
        drawList->AddText(ImVec2(rect.Max.x - pad.x - versionSize.x, rect.Min.y + pad.y),
                          ImGui::GetColorU32(ImVec4(0.74f, 0.79f, 0.88f, 1.0f)),
                          version.c_str());
    }

    const float chevronSize = 14.0f;
    const float subY = rect.Min.y + pad.y + 18.0f;
    const ImVec2 chevronMin(rect.Min.x + pad.x, subY);
    const ImVec2 chevronMax(chevronMin.x + chevronSize, subY + chevronSize);
    const ImVec2 mousePos = ImGui::GetMousePos();
    const bool mouseInChevron = mousePos.x >= chevronMin.x && mousePos.x < chevronMax.x &&
                                mousePos.y >= chevronMin.y && mousePos.y < chevronMax.y;
    const bool chevronHovered = hovered && mouseInChevron && packageHasAnyBadge(state);

    if (packageHasAnyBadge(state)) {
        const ImU32 chevronCol = ImGui::GetColorU32(
            chevronHovered ? ImVec4(0.96f, 0.97f, 0.99f, 1.0f)
                           : ImVec4(0.70f, 0.75f, 0.83f, 0.95f));
        if (expanded) {
            drawList->AddTriangleFilled(
                ImVec2(chevronMin.x + 2.0f, chevronMin.y + 5.0f),
                ImVec2(chevronMax.x - 2.0f, chevronMin.y + 5.0f),
                ImVec2((chevronMin.x + chevronMax.x) * 0.5f, chevronMin.y + 11.0f),
                chevronCol);
        } else {
            drawList->AddTriangleFilled(
                ImVec2(chevronMin.x + 4.0f, chevronMin.y + 2.0f),
                ImVec2(chevronMin.x + 4.0f, chevronMax.y - 2.0f),
                ImVec2(chevronMax.x - 2.0f, (chevronMin.y + chevronMax.y) * 0.5f),
                chevronCol);
        }
    }

    std::string sub;
    if (!pkg.author.empty()) sub = pkg.author;
    if (!pkg.id.empty()) {
        if (!sub.empty()) sub += "  •  ";
        sub += pkg.id;
    }
    if (!sub.empty()) {
        const float subTextX = packageHasAnyBadge(state) ? (chevronMax.x + 6.0f)
                                                         : (rect.Min.x + pad.x);
        drawList->AddText(ImVec2(subTextX, subY + 1.0f),
                          ImGui::GetColorU32(ImVec4(0.63f, 0.68f, 0.76f, 1.0f)),
                          sub.c_str());
    }

    if (showBadges) {
        float badgeX = rect.Min.x + pad.x;
        const float badgeY = rect.Max.y - 22.0f;
        auto pushBadge = [&](const std::string& text, const ImVec4& color) {
            badgeX += drawBadge(drawList, ImVec2(badgeX, badgeY), text, color) + 4.0f;
        };
        if (state.inProject) pushBadge("Project", ImVec4(0.26f, 0.51f, 0.83f, 1.0f));
        if (state.global) pushBadge("Global", ImVec4(0.79f, 0.55f, 0.18f, 1.0f));
        if (state.updateAvailable) pushBadge("Update", ImVec4(0.24f, 0.62f, 0.42f, 1.0f));
        if (!state.compatible) pushBadge("Incompatible", ImVec4(0.78f, 0.28f, 0.24f, 1.0f));
        else if (!state.installed) pushBadge("Available", ImVec4(0.34f, 0.38f, 0.47f, 1.0f));
    }

    if (clicked && mouseInChevron && packageHasAnyBadge(state)) {
        if (outChevronClicked) *outChevronClicked = true;
        return false;
    }
    return clicked;
}

void buildUnpackPreview(PackageManager& packageManager,
                        const PackageInfo& pkg,
                        bool reUnpack,
                        bool isUpdate,
                        UnpackPreviewState& out) {
    out = {};
    out.packageId = pkg.id;
    out.packageName = pkg.name.empty() ? pkg.id : pkg.name;
    out.packageVersion = pkg.version;
    out.reUnpack = reUnpack;
    out.isUpdate = isUpdate;
    if (isUpdate) out.targetVersionLabel = pkg.version;

    PackageManager::RegistryPackageLocations locations;
    if (!packageManager.resolveRegistryPackageLocations(pkg.id, locations)) {
        out.note = "This package is not part of the registry.";
        return;
    }
    out.destinationPath = locations.destination;
    out.downloadUrl = locations.downloadUrl;
    out.sourcePath = locations.source;
    out.sourceAvailable = !locations.source.empty() && fs::exists(locations.source);

    if (!out.sourceAvailable) {
        if (!locations.downloadUrl.empty()) {
            out.note = "Package files will be downloaded on confirm.";
        } else {
            out.note = "Package files are not currently available.";
        }
        return;
    }

    std::error_code ec;
    const size_t kEntryLimit = 400;
    for (auto it = fs::recursive_directory_iterator(locations.source, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const fs::path rel = fs::relative(it->path(), locations.source, ec);
        if (ec) { ec.clear(); continue; }
        std::uintmax_t size = it->file_size(ec);
        if (ec) { size = 0; ec.clear(); }
        out.totalBytes += size;
        if (out.entries.size() < kEntryLimit) {
            out.entries.emplace_back(rel.generic_string(), size);
        } else {
            out.truncated = true;
        }
    }
    std::sort(out.entries.begin(), out.entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}
} // namespace

void Engine::renderRegistryPackagesWindow() {
    if (!showRegistryPackagesWindow) {
        return;
    }

    const bool wasOpen = showRegistryPackagesWindow;
    if (!ImGui::Begin(Loc::Window("WINDOW_MODUPAK_MANAGER", "Modupak Manager"), &showRegistryPackagesWindow, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (wasOpen != showRegistryPackagesWindow && projectManager.currentProject.isLoaded) {
            saveEditorUserSettings();
        }
        return;
    }

    const auto& registry = packageManager.getRegistry();
    std::vector<const PackageInfo*> allPackages;
    allPackages.reserve(registry.size());

    std::array<int, static_cast<size_t>(ModupakManagerView::Count)> tabCounts = {0, 0, 0, 0, 0};
    for (const auto& pkg : registry) {
        if (!pkg.registryPackage) continue;
        allPackages.push_back(&pkg);
        const PackageUiState state = buildPackageUiState(packageManager, pkg);
        tabCounts[static_cast<int>(ModupakManagerView::All)]++;
        if (state.inProject) tabCounts[static_cast<int>(ModupakManagerView::Project)]++;
        if (state.global) tabCounts[static_cast<int>(ModupakManagerView::Global)]++;
        if (state.updateAvailable) tabCounts[static_cast<int>(ModupakManagerView::Updates)]++;
        if (!state.compatible) tabCounts[static_cast<int>(ModupakManagerView::Incompatible)]++;
    }

    registryPackageView = std::clamp(registryPackageView, 0, static_cast<int>(ModupakManagerView::Count) - 1);
    registryPackageSort = std::clamp(registryPackageSort, 0, static_cast<int>(ModupakManagerSort::Count) - 1);

    // Texture resolver shared by tab icons. OpenGL textures need Y flipped for ImGui.
    struct TabIcon { ImTextureID id = static_cast<ImTextureID>(0); bool flipY = false; };
    const bool hasVulkanUiImages = usingVulkan() && vulkanRendererInitialized && (vulkanRenderer != nullptr);
    auto resolveIcon = [&](const char* iconPath) -> TabIcon {
        if (!iconPath || !*iconPath) return {};
        if (rendererInitialized) {
            if (Texture* tex = renderer.getTexture(iconPath, MaterialProperties::TextureFilter::Bilinear);
                tex && tex->GetID()) {
                return {static_cast<ImTextureID>(tex->GetID()), true};
            }
        }
        if (hasVulkanUiImages) {
            ImTextureID id = vulkanRenderer->getOrCreateUIImage(iconPath);
            if (id != static_cast<ImTextureID>(0)) return {id, false};
        }
        return {};
    };

    const float tabHeight = 40.0f;
    const float tabSpacing = 2.0f;
    const float availTabsWidth = ImGui::GetContentRegionAvail().x;
    const float tabWidth = (availTabsWidth - tabSpacing * (kViewTabs.size() - 1)) / static_cast<float>(kViewTabs.size());

    const ImVec2 tabBarOrigin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(availTabsWidth, tabHeight));

    ImDrawList* tabDrawList = ImGui::GetWindowDrawList();
    std::array<float, kViewTabs.size()> tabXs{};
    for (size_t i = 0; i < kViewTabs.size(); ++i) {
        tabXs[i] = tabBarOrigin.x + (tabWidth + tabSpacing) * static_cast<float>(i);
    }

    for (size_t i = 0; i < kViewTabs.size(); ++i) {
        const ViewTab& tab = kViewTabs[i];
        const bool selected = registryPackageView == static_cast<int>(tab.view);
        const ImVec2 tabMin(tabXs[i], tabBarOrigin.y);
        const ImVec2 tabMax(tabMin.x + tabWidth, tabMin.y + tabHeight);

        ImGui::SetCursorScreenPos(tabMin);
        ImGui::PushID(static_cast<int>(i));
        ImGui::InvisibleButton("##ModupakTab", ImVec2(tabWidth, tabHeight));
        ImGui::ReportItemHoverFeedback();
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        ImGui::PopID();
        if (clicked && !selected) ImGui::ReportWidgetFeedback(ImGuiWidgetFeedback_Tab);
        if (clicked) registryPackageView = static_cast<int>(tab.view);

        if (hovered && !selected) {
            tabDrawList->AddRectFilled(ImVec2(tabMin.x + 6.0f, tabMin.y + 6.0f),
                                       ImVec2(tabMax.x - 6.0f, tabMax.y - 6.0f),
                                       ImGui::GetColorU32(ImVec4(0.13f, 0.16f, 0.22f, 0.65f)),
                                       7.0f);
        }
        if (selected) {
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.0f);
            const float glowAlpha = ImLerp(0.10f, 0.22f, pulse);
            const ImVec2 glowMin(tabMin.x + 4.0f, tabMin.y + 6.0f);
            const ImVec2 glowMax(tabMax.x - 4.0f, tabMax.y - 6.0f);
            const float glowRound = 9.0f;
            tabDrawList->AddRectFilled(glowMin, glowMax,
                                       ImGui::GetColorU32(ImVec4(0.48f, 0.66f, 0.92f, glowAlpha * 0.40f)),
                                       glowRound);
            for (int g = 0; g < 2; ++g) {
                const float gp = static_cast<float>(g + 1) * 3.0f;
                const float ga = glowAlpha * (0.22f - 0.08f * static_cast<float>(g));
                if (ga <= 0.0f) break;
                tabDrawList->AddRect(ImVec2(glowMin.x - gp, glowMin.y - gp),
                                     ImVec2(glowMax.x + gp, glowMax.y + gp),
                                     ImGui::GetColorU32(ImVec4(0.55f, 0.74f, 0.98f, ga)),
                                     glowRound + gp, 0, 1.0f);
            }
        }

        const ImVec4 textColor = selected ? ImVec4(0.96f, 0.98f, 1.0f, 1.0f)
                                : hovered ? ImVec4(0.88f, 0.92f, 0.97f, 1.0f)
                                          : ImVec4(0.66f, 0.72f, 0.80f, 1.0f);
        const std::string countText = " (" + std::to_string(tabCounts[i]) + ")";
        const char* tabLabel = Loc::T(tab.key, tab.label);
        const ImVec2 labelSize = ImGui::CalcTextSize(tabLabel);
        const ImVec2 countSize = ImGui::CalcTextSize(countText.c_str());
        const float iconSize = 18.0f;
        TabIcon icon = resolveIcon(tab.iconPath);
        const bool hasIcon = icon.id != static_cast<ImTextureID>(0);
        const float iconGap = hasIcon ? (iconSize + 6.0f) : 0.0f;
        const float groupW = iconGap + labelSize.x + countSize.x;
        const float groupX = tabMin.x + (tabWidth - groupW) * 0.5f;
        const float groupCenterY = tabMin.y + tabHeight * 0.5f;

        if (hasIcon) {
            const ImVec2 iconMin(groupX, groupCenterY - iconSize * 0.5f);
            const ImVec2 iconMax(iconMin.x + iconSize, iconMin.y + iconSize);
            const ImVec2 uvMin = icon.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
            const ImVec2 uvMax = icon.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
            tabDrawList->AddImage(icon.id, iconMin, iconMax, uvMin, uvMax, ImGui::GetColorU32(textColor));
        }
        const float textY = groupCenterY - labelSize.y * 0.5f;
        tabDrawList->AddText(ImVec2(groupX + iconGap, textY),
                             ImGui::GetColorU32(textColor), tabLabel);
        const ImVec4 countColor = selected ? ImVec4(0.94f, 0.96f, 1.0f, 0.95f)
                                  : hovered ? ImVec4(0.78f, 0.83f, 0.91f, 0.85f)
                                            : ImVec4(0.55f, 0.61f, 0.70f, 0.85f);
        tabDrawList->AddText(ImVec2(groupX + iconGap + labelSize.x, textY),
                             ImGui::GetColorU32(countColor), countText.c_str());
    }

    {
        const int activeIdx = std::clamp(registryPackageView, 0, static_cast<int>(kViewTabs.size()) - 1);
        const float targetX = tabXs[activeIdx] + 14.0f;
        const float targetW = tabWidth - 28.0f;
        if (gTabIndicatorX < 0.0f) { gTabIndicatorX = targetX; gTabIndicatorW = targetW; }
        const float dt = ImGui::GetIO().DeltaTime;
        gTabIndicatorX = smoothApproach(gTabIndicatorX, targetX, 16.0f, dt);
        gTabIndicatorW = smoothApproach(gTabIndicatorW, targetW, 16.0f, dt);

        const float indH = 3.0f;
        const float indY = tabBarOrigin.y + tabHeight - indH - 0.5f;
        tabDrawList->AddRectFilled(ImVec2(gTabIndicatorX, indY),
                                   ImVec2(gTabIndicatorX + gTabIndicatorW, indY + indH),
                                   ImGui::GetColorU32(ImVec4(0.48f, 0.66f, 0.92f, 0.95f)),
                                   indH * 0.5f);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    const float searchWidth = std::min(280.0f, ImGui::GetContentRegionAvail().x * 0.45f);
    ImGui::SetNextItemWidth(searchWidth);
    ImGui::InputTextWithHint("##ModupakSearch", "Search packages",
                             registryPackageSearch, sizeof(registryPackageSearch));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    const ModupakManagerSort sortMode = static_cast<ModupakManagerSort>(registryPackageSort);
    if (ImGui::BeginCombo("##SortPackages", sortLabel(sortMode))) {
        for (int i = 0; i < static_cast<int>(ModupakManagerSort::Count); ++i) {
            const auto candidate = static_cast<ModupakManagerSort>(i);
            const bool selected = (registryPackageSort == i);
            if (ImGui::Selectable(sortLabel(candidate), selected)) registryPackageSort = i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(Loc::T("MODUPAK_REFRESH", "Refresh"))) {
        const std::string previousSelection = registryPackageSelectedId;
        packageManager.refreshRegistry();
        registryPackageFeedback = packageManager.getRegistryStatus();
        registryPackageLastActionSucceeded = packageManager.hasRegistryMetadata();
        registryPackageSelectedId = previousSelection;
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }
    ImGui::PopStyleVar();

    const std::string loweredSearch = toLowerCopy(std::string(registryPackageSearch));
    std::vector<const PackageInfo*> filteredPackages;
    filteredPackages.reserve(allPackages.size());
    for (const PackageInfo* pkg : allPackages) {
        const PackageUiState state = buildPackageUiState(packageManager, *pkg);
        if (!matchesView(static_cast<ModupakManagerView>(registryPackageView), state)) continue;
        if (!packageMatchesSearch(*pkg, loweredSearch)) continue;
        filteredPackages.push_back(pkg);
    }
    std::sort(filteredPackages.begin(), filteredPackages.end(),
        [&](const PackageInfo* lhs, const PackageInfo* rhs) {
            const PackageUiState ls = buildPackageUiState(packageManager, *lhs);
            const PackageUiState rs = buildPackageUiState(packageManager, *rhs);
            switch (static_cast<ModupakManagerSort>(registryPackageSort)) {
                case ModupakManagerSort::NameDesc:
                    if (lhs->name == rhs->name) return lhs->id > rhs->id;
                    return lhs->name > rhs->name;
                case ModupakManagerSort::VersionDesc:
                    if (compareVersions(lhs->version, rhs->version) == 0) return lhs->name < rhs->name;
                    return compareVersions(lhs->version, rhs->version) > 0;
                case ModupakManagerSort::Status: {
                    const int la = (ls.updateAvailable ? 0 : 1) + (ls.installed ? 0 : 3) + (ls.compatible ? 0 : 10);
                    const int ra = (rs.updateAvailable ? 0 : 1) + (rs.installed ? 0 : 3) + (rs.compatible ? 0 : 10);
                    if (la == ra) return lhs->name < rhs->name;
                    return la < ra;
                }
                case ModupakManagerSort::NameAsc:
                case ModupakManagerSort::Count:
                default:
                    if (lhs->name == rhs->name) return lhs->id < rhs->id;
                    return lhs->name < rhs->name;
            }
        });

    auto inFiltered = [&](const std::string& id) {
        return std::any_of(filteredPackages.begin(), filteredPackages.end(),
                           [&](const PackageInfo* pkg) { return pkg->id == id; });
    };
    if (registryPackageSelectedId.empty() || !inFiltered(registryPackageSelectedId)) {
        registryPackageSelectedId = filteredPackages.empty() ? std::string() : filteredPackages.front()->id;
    }

    const PackageInfo* selectedPackage = nullptr;
    for (const PackageInfo* pkg : filteredPackages) {
        if (pkg->id == registryPackageSelectedId) { selectedPackage = pkg; break; }
    }
    if (!selectedPackage) {
        for (const PackageInfo* pkg : allPackages) {
            if (pkg->id == registryPackageSelectedId) { selectedPackage = pkg; break; }
        }
    }

    const float listWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.38f, 260.0f, 420.0f);

    ImGui::BeginChild("ModupakListPane", ImVec2(listWidth, 0.0f), true);
    ImGui::TextDisabled("Packages (%d)", static_cast<int>(filteredPackages.size()));
    ImGui::Separator();
    ImGui::BeginChild("ModupakListScroll", ImVec2(0.0f, 0.0f), false);
    for (const PackageInfo* pkg : filteredPackages) {
        const PackageUiState state = buildPackageUiState(packageManager, *pkg);
        const bool expanded = gExpandedPackageIds.count(pkg->id) > 0;
        bool chevronClicked = false;
        if (drawPackageRow(*pkg, state, registryPackageSelectedId == pkg->id, expanded, &chevronClicked)) {
            registryPackageSelectedId = pkg->id;
            selectedPackage = pkg;
        }
        if (chevronClicked) {
            if (expanded) gExpandedPackageIds.erase(pkg->id);
            else gExpandedPackageIds.insert(pkg->id);
        }
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
    }
    if (filteredPackages.empty()) {
        ImGui::TextDisabled("No packages matched the current view.");
    }
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("ModupakDetailsPane", ImVec2(0.0f, 0.0f), true);
    if (!selectedPackage) {
        ImGui::TextDisabled("Select a package to view details.");
        ImGui::EndChild();
        ImGui::End();
        if (wasOpen != showRegistryPackagesWindow && projectManager.currentProject.isLoaded) {
            saveEditorUserSettings();
        }
        return;
    }

    const PackageUiState selectedState = buildPackageUiState(packageManager, *selectedPackage);

    // Header row: title (left) + actions (right)
    const bool packageAvailable = selectedState.availableLocally || selectedState.availableRemotely || selectedState.global;
    const bool projectLoaded = projectManager.currentProject.isLoaded;
    const bool canUnpack = projectLoaded && selectedState.compatible && packageAvailable && !selectedState.inProject;
    const bool canReUnpack = projectLoaded && selectedState.compatible && packageAvailable && selectedState.inProject;
    const bool canRemove = projectLoaded && selectedState.inProject;
    const bool canUpdate = projectLoaded && selectedState.compatible && selectedState.updateAvailable && selectedState.inProject;

    const float headerStartY = ImGui::GetCursorPosY();
    const float headerRightWidth = 240.0f;
    const float headerLeftWidth = std::max(120.0f, ImGui::GetContentRegionAvail().x - headerRightWidth - 8.0f);

    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + headerLeftWidth);
    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextUnformatted(selectedPackage->name.empty() ? selectedPackage->id.c_str()
                                                         : selectedPackage->name.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopTextWrapPos();

    std::string versionLine;
    if (!selectedPackage->version.empty()) versionLine = "Version " + selectedPackage->version;
    if (selectedState.updateAvailable && !selectedPackage->version.empty()) {
        versionLine += "  •  Update available";
    }
    if (!versionLine.empty()) ImGui::TextDisabled("%s", versionLine.c_str());
    ImGui::EndGroup();
    const float headerLeftBottomY = ImGui::GetItemRectMax().y;

    auto triggerUnpack = [&](bool reUnpack, bool isUpdate) {
        buildUnpackPreview(packageManager, *selectedPackage, reUnpack, isUpdate, gUnpackPreview);
        gOpenUnpackPreview = true;
    };

    auto removeFromProject = [&]() {
        if (packageManager.remove(selectedPackage->id)) {
            clampOptionalPackageState(false);
            registryPackageFeedback = "Removed " + selectedPackage->id + " from the current project.";
            registryPackageLastActionSucceeded = true;
            addConsoleMessage(registryPackageFeedback, ConsoleMessageType::Success);
        } else {
            registryPackageFeedback = packageManager.getLastError();
            registryPackageLastActionSucceeded = false;
            addConsoleMessage("Project package removal failed: " + registryPackageFeedback, ConsoleMessageType::Error);
        }
    };

    auto drawActionButton = [&](const char* label, bool enabled, const ImVec4& tint) -> bool {
        ImGui::PushStyleColor(ImGuiCol_Button, tint);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(tint.x * 1.15f, tint.y * 1.15f, tint.z * 1.15f, tint.w));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(tint.x * 0.9f, tint.y * 0.9f, tint.z * 0.9f, tint.w));
        ImGui::BeginDisabled(!enabled);
        const bool clicked = ImGui::Button(label, ImVec2(headerRightWidth - 8.0f, 0.0f));
        ImGui::EndDisabled();
        ImGui::PopStyleColor(3);
        return clicked;
    };

    const float actionBlockX = ImGui::GetWindowContentRegionMax().x - headerRightWidth + 4.0f;
    ImGui::SetCursorPos(ImVec2(actionBlockX, headerStartY));
    ImGui::BeginGroup();
    if (canUpdate) {
        const std::string label = "Update ModuPAK to V" + selectedPackage->version;
        if (drawActionButton(label.c_str(), true, ImVec4(0.20f, 0.46f, 0.30f, 1.0f))) {
            triggerUnpack(true, true);
        }
    }
    if (selectedState.inProject) {
        if (drawActionButton(Loc::T("MODUPAK_RE_UNPACK", "Re-Unpack ModuPAK"), canReUnpack, ImVec4(0.20f, 0.31f, 0.46f, 1.0f))) {
            triggerUnpack(true, false);
        }
        if (drawActionButton(Loc::T("MODUPAK_REMOVE", "Remove ModuPAK"), canRemove, ImVec4(0.50f, 0.22f, 0.22f, 1.0f))) {
            removeFromProject();
        }
    } else {
        if (drawActionButton(Loc::T("MODUPAK_UNPACK", "Unpack ModuPAK"), canUnpack, ImVec4(0.20f, 0.31f, 0.46f, 1.0f))) {
            triggerUnpack(false, false);
        }
    }
    if (!projectLoaded) {
        ImGui::TextDisabled("Load a project to install.");
    }
    ImGui::EndGroup();
    const float headerRightBottomY = ImGui::GetItemRectMax().y;

    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x,
                                     std::max(headerLeftBottomY, headerRightBottomY) + 6.0f));
    ImGui::Separator();

    // Badge row.
    ImDrawList* detailDrawList = ImGui::GetWindowDrawList();
    ImVec2 badgePos = ImGui::GetCursorScreenPos();
    float badgeX = badgePos.x;
    if (selectedState.inProject) badgeX += drawBadge(detailDrawList, ImVec2(badgeX, badgePos.y), "Project", ImVec4(0.26f, 0.51f, 0.83f, 1.0f)) + 4.0f;
    if (selectedState.global) badgeX += drawBadge(detailDrawList, ImVec2(badgeX, badgePos.y), "Global", ImVec4(0.79f, 0.55f, 0.18f, 1.0f)) + 4.0f;
    if (selectedState.updateAvailable) badgeX += drawBadge(detailDrawList, ImVec2(badgeX, badgePos.y), "Update", ImVec4(0.24f, 0.62f, 0.42f, 1.0f)) + 4.0f;
    if (!selectedState.compatible) badgeX += drawBadge(detailDrawList, ImVec2(badgeX, badgePos.y), "Incompatible", ImVec4(0.78f, 0.28f, 0.24f, 1.0f)) + 4.0f;
    else if (!selectedState.installed) badgeX += drawBadge(detailDrawList, ImVec2(badgeX, badgePos.y), "Available", ImVec4(0.34f, 0.38f, 0.47f, 1.0f)) + 4.0f;
    ImGui::Dummy(ImVec2(0.0f, 22.0f));

    // Description.
    ImGui::BeginChild("ModupakDetailsScroll", ImVec2(0.0f, 0.0f), false);

    if (!selectedPackage->description.empty()) {
        ImGui::TextWrapped("%s", selectedPackage->description.c_str());
    } else {
        ImGui::TextDisabled("No description provided.");
    }
    ImGui::Spacing();

    auto inlineRow = [](const char* label, const std::string& value) {
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(140.0f);
        if (value.empty()) ImGui::TextUnformatted("-");
        else ImGui::TextWrapped("%s", value.c_str());
    };

    inlineRow("Author", selectedPackage->author);
    inlineRow("Package ID", selectedPackage->id);
    inlineRow("Compatibility", selectedPackage->compatibleModuEngineVersion.empty()
                                   ? std::string("Any")
                                   : selectedPackage->compatibleModuEngineVersion);
    if (!selectedState.compatible) {
        ImGui::TextColored(ImVec4(0.92f, 0.47f, 0.38f, 1.0f),
                           "Not compatible with this ModuEngine version.");
    }

    if (selectedState.updateAvailable) {
        ImGui::Spacing();
        if (ImGui::SmallButton(Loc::T("MODUPAK_VIEW_CHANGES", "View Changes"))) {
            registryPackageFeedback = "Changelog: see the registry source for " + selectedPackage->id + ".";
            registryPackageLastActionSucceeded = true;
        }
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Advanced Info")) {
        ImGui::Indent(8.0f);
        inlineRow("Subsystem", selectedPackage->subsystem);
        inlineRow("Package Type", selectedPackage->packageType);
        inlineRow("Registry Source", sourceSummary(*selectedPackage, selectedState));
        inlineRow("Installed State", installedStateLabel(selectedState));
        inlineRow("Project Install", selectedState.inProject ? "Enabled for this project" : "Not enabled for this project");
        inlineRow("Global Install", selectedState.global ? "Present in global package store" : "Not present in global package store");
        inlineRow("Compatible Engine Version", selectedPackage->compatibleModuEngineVersion);
        if (!selectedPackage->downloadUrl.empty()) inlineRow("Download URL", selectedPackage->downloadUrl);
        if (!packageManager.getRegistryLastUpdated().empty()) inlineRow("Registry Updated", packageManager.getRegistryLastUpdated());
        if (!packageManager.getRegistryUpdatedBy().empty()) inlineRow("Updated By", packageManager.getRegistryUpdatedBy());

        ImGui::Spacing();
        const bool canInstallGlobal = selectedState.compatible &&
                                      (selectedState.availableLocally || selectedState.availableRemotely) &&
                                      !selectedState.global;
        ImGui::BeginDisabled(!canInstallGlobal);
        if (ImGui::Button(Loc::T("MODUPAK_INSTALL_GLOBALLY", "Install Globally"))) {
            if (packageManager.installRegistryPackageGlobally(selectedPackage->id)) {
                registryPackageFeedback = "Installed " + selectedPackage->id + " into the global package store.";
                registryPackageLastActionSucceeded = true;
                playEditorSound(EditorSoundCue::PackageInstalled, 0.95f);
                addConsoleMessage(registryPackageFeedback, ConsoleMessageType::Success);
            } else {
                registryPackageFeedback = packageManager.getLastError();
                registryPackageLastActionSucceeded = false;
                addConsoleMessage("Global package install failed: " + registryPackageFeedback, ConsoleMessageType::Error);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!selectedState.global);
        if (ImGui::Button(Loc::T("MODUPAK_REMOVE_GLOBALLY", "Remove Globally"))) {
            if (packageManager.removeRegistryPackageGlobally(selectedPackage->id)) {
                registryPackageFeedback = "Removed " + selectedPackage->id + " from the global package store.";
                registryPackageLastActionSucceeded = true;
                addConsoleMessage(registryPackageFeedback, ConsoleMessageType::Success);
            } else {
                registryPackageFeedback = packageManager.getLastError();
                registryPackageLastActionSucceeded = false;
                addConsoleMessage("Global package removal failed: " + registryPackageFeedback, ConsoleMessageType::Error);
            }
        }
        ImGui::EndDisabled();
        ImGui::Unindent(8.0f);
    }

    if (!registryPackageFeedback.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(registryPackageLastActionSucceeded
                               ? ImVec4(0.43f, 0.85f, 0.60f, 1.0f)
                               : ImVec4(0.95f, 0.45f, 0.45f, 1.0f),
                           "%s", registryPackageFeedback.c_str());
    }

    ImGui::EndChild();
    ImGui::EndChild();

    if (gOpenUnpackPreview) {
        ImGui::OpenPopup("ModuPAK Unpack Preview");
        gOpenUnpackPreview = false;
    }
    ImGui::SetNextWindowSize(ImVec2(620.0f, 480.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("ModuPAK Unpack Preview", nullptr,
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        const char* headerLabel = gUnpackPreview.isUpdate ? "Update package"
                                : gUnpackPreview.reUnpack ? "Re-unpack package"
                                                          : "Unpack package";
        ImGui::Text("%s: %s", headerLabel, gUnpackPreview.packageName.c_str());
        if (!gUnpackPreview.packageVersion.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("v%s", gUnpackPreview.packageVersion.c_str());
        }
        if (!gUnpackPreview.destinationPath.empty()) {
            ImGui::TextDisabled("Destination: %s", gUnpackPreview.destinationPath.generic_string().c_str());
        }
        if (!gUnpackPreview.note.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.30f, 1.0f), "%s", gUnpackPreview.note.c_str());
        }
        ImGui::Separator();

        if (gUnpackPreview.sourceAvailable) {
            ImGui::Text("%zu file(s) — %s", gUnpackPreview.entries.size() + (gUnpackPreview.truncated ? 1u : 0u),
                        formatBytesShort(gUnpackPreview.totalBytes).c_str());
            ImGui::BeginChild("UnpackPreviewList", ImVec2(0.0f, -36.0f), true);
            if (ImGui::BeginTable("UnpackPreviewTable", 2,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                for (const auto& [path, size] : gUnpackPreview.entries) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(path.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextDisabled("%s", formatBytesShort(size).c_str());
                }
                if (gUnpackPreview.truncated) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("... (additional files omitted)");
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
        } else {
            ImGui::BeginChild("UnpackPreviewList", ImVec2(0.0f, -36.0f), true);
            if (!gUnpackPreview.downloadUrl.empty()) {
                ImGui::TextWrapped("Source files are not present locally yet. They will be downloaded from:");
                ImGui::TextUnformatted(gUnpackPreview.downloadUrl.c_str());
            } else {
                ImGui::TextDisabled("No file listing available for this package.");
            }
            ImGui::EndChild();
        }

        const float btnW = 140.0f;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - btnW * 2.0f - 12.0f);
        const char* confirmLabel = gUnpackPreview.isUpdate ? "Confirm Update"
                                  : gUnpackPreview.reUnpack ? "Confirm Re-Unpack"
                                                            : "Confirm Unpack";
        if (ImGui::Button(confirmLabel, ImVec2(btnW, 0.0f))) {
            const std::string id = gUnpackPreview.packageId;
            bool ok = true;
            if (gUnpackPreview.reUnpack || gUnpackPreview.isUpdate) {
                if (!packageManager.remove(id)) {
                    ok = false;
                    registryPackageFeedback = packageManager.getLastError();
                    registryPackageLastActionSucceeded = false;
                    addConsoleMessage("Package re-unpack failed: " + registryPackageFeedback, ConsoleMessageType::Error);
                }
            }
            if (ok) {
                if (packageManager.installRegistryPackageToProject(id)) {
                    clampOptionalPackageState(false);
                    registryPackageFeedback = (gUnpackPreview.isUpdate ? "Updated " : "Unpacked ") + id + " into the current project.";
                    registryPackageLastActionSucceeded = true;
                    playEditorSound(EditorSoundCue::PackageInstalled, 0.95f);
                    addConsoleMessage(registryPackageFeedback, ConsoleMessageType::Success);
                } else {
                    registryPackageFeedback = packageManager.getLastError();
                    registryPackageLastActionSucceeded = false;
                    addConsoleMessage("Package unpack failed: " + registryPackageFeedback, ConsoleMessageType::Error);
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Loc::T("COMMON_CANCEL", "Cancel"), ImVec2(btnW, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
    if (wasOpen != showRegistryPackagesWindow && projectManager.currentProject.isLoaded) {
        saveEditorUserSettings();
    }
}
