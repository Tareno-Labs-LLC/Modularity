#include "EditorUI.h"
#include "../include/Textures/TextureTranscode.h"
#include "../include/Platform/AssetSource.h"
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#if defined(IMGUI_ENABLE_FREETYPE)
#include "ThirdParty/ModuGUI/misc/freetype/imgui_freetype.h"
#endif
namespace {
    constexpr float kModularityUiFontSizeBase = 18.0f;
    constexpr float kModularityUiFontSizeOffset = -2.5f;
    struct TouchSwipeWindowState {
        ImVec2 targetScroll = ImVec2(0.0f, 0.0f);
        ImVec2 currentScroll = ImVec2(0.0f, 0.0f);
        ImVec2 inputVelocity = ImVec2(0.0f, 0.0f);
        ImVec2 smoothVelocity = ImVec2(0.0f, 0.0f);
        bool initialized = false;
        bool touchedThisFrame = false;
        bool isDragging = false;
    };
    struct TouchSwipeRuntimeState {
        std::unordered_map<ImGuiID, TouchSwipeWindowState> windowStates;
        ImGuiID activeWindowId = 0;
        ImVec2 dragStartPos = ImVec2(0.0f, 0.0f);
        ImVec2 lastPointerPos = ImVec2(0.0f, 0.0f);
        bool dragging = false;
    };
    bool hasScrollableAxis(const ImGuiWindow* window, int axis) {
        if (!window || axis < 0 || axis > 1) return false;
        if ((window->Flags & ImGuiWindowFlags_NoInputs) != 0) return false;
        if ((window->Flags & ImGuiWindowFlags_NoScrollWithMouse) != 0) return false;
        return window->ScrollMax[axis] > 0.0f;
    }
    std::string pathToUtf8(const fs::path& path) { return PathToUtf8(path); }
    std::string buildFileSelectionKey(const fs::path& path) {
        std::error_code ec;
        fs::path canonical = fs::weakly_canonical(path, ec);
        if (!ec) return PathToUtf8(canonical.generic_u8string());
        return PathToUtf8(fs::absolute(path, ec).lexically_normal().generic_u8string());
    }
    FileCategory classifyFileBrowserEntry(const fs::directory_entry& entry) {
        if (entry.is_directory()) return FileCategory::Folder;
        std::string filename = entry.path().filename().string();
        std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (filename == "project.modu" || filename == "build.modu" || filename == "scripts.modu" || filename == "packages.modu" || filename == "autostart.modu" || filename == "launcher_settings.modu") {
            return FileCategory::Text;
        }
        if (ext == ".modu" || ext == ".scene") return FileCategory::Scene;
        if (ext == ".modupak") return FileCategory::Text;
        if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb" ||
            ext == ".dae" || ext == ".blend" || ext == ".3ds" || ext == ".b3d" ||
            ext == ".ply" || ext == ".stl" || ext == ".x" || ext == ".md5mesh" ||
            ext == ".rmesh" || ext == ".mmesh") {
            return FileCategory::Model;
        }
        if (ext == ".mat") return FileCategory::Material;
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
            ext == ".tga" || ext == ".dds" || ext == ".hdr" || ext == ".exr") {
            return FileCategory::Texture;
        }
        if (ext == ".mp4" || ext == ".m4v" || ext == ".mov" || ext == ".avi" ||
            ext == ".mkv" || ext == ".webm" || ext == ".wmv" || ext == ".ogv") {
            return FileCategory::Video;
        }
        if (ext == ".glsl" || ext == ".vert" || ext == ".frag" || ext == ".hlsl" ||
            ext == ".shader" || ext == ".modushader") {
            return FileCategory::Shader;
        }
        if (ext == ".cpp" || ext == ".c" || ext == ".moducpp" || ext == ".modumako" || ext == ".mko" || ext == ".h" || ext == ".hpp" ||
            ext == ".lua" || ext == ".py" || ext == ".cs" || ext == ".js" || ext == ".ts" ||
            ext == ".rb" || ext == ".rs" || ext == ".go" || ext == ".java" || ext == ".swift" ||
            ext == ".kt" || ext == ".kts" || ext == ".sh") {
            return FileCategory::Script;
        }
        if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") {
            return FileCategory::Audio;
        }
        if (ext == ".txt" || ext == ".md" || ext == ".json" || ext == ".xml" ||
            ext == ".yaml" || ext == ".ini" || ext == ".cfg") {
            return FileCategory::Text;
        }
        return FileCategory::Unknown;
    }
    std::string formatFileBrowserByteSize(uintmax_t bytes) {
        static const char* kUnits[] = { "B", "KB", "MB", "GB", "TB" };
        double size = static_cast<double>(bytes);
        int unit = 0;
        while (size >= 1024.0 && unit < 4) {size /= 1024.0; ++unit;}
        std::ostringstream ss;
        if (unit == 0) {ss << bytes << " " << kUnits[unit];}
        else {ss << std::fixed << std::setprecision(size >= 10.0 ? 1 : 2) << size << " " << kUnits[unit];}
        return ss.str();
    }
    const char* fileBrowserCategoryLabel(FileCategory cat) {
        switch (cat) {
            case FileCategory::Folder: return "Folder";
            case FileCategory::Scene: return "Scene";
            case FileCategory::Model: return "Model";
            case FileCategory::Material: return "Material";
            case FileCategory::Texture: return "Texture";
            case FileCategory::Video: return "Video";
            case FileCategory::Shader: return "Shader";
            case FileCategory::Script: return "Script";
            case FileCategory::Audio: return "Audio";
            case FileCategory::Text: return "Text";
            default: return "File";
        }
    }
    bool folderHasVisibleItemsCached(const fs::path& path, bool showHiddenFiles) {
        std::error_code ec;
        for (fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
            !ec && it != fs::directory_iterator();
            ++it) {
            const std::string name = it->path().filename().string();
            if (!showHiddenFiles && !name.empty() && name[0] == '.') {continue;}
            return true;
        }   return false;
    }
    unsigned long long fileBrowserContentSignature(const std::vector<FileBrowser::CachedEntry>& entries) {
        unsigned long long hash = 1469598103934665603ull;
        auto mix = [&hash](unsigned long long value) {
            hash ^= value;
            hash *= 1099511628211ull;
        };
        for (const FileBrowser::CachedEntry& entry : entries) {
            for (unsigned char c : entry.filename) mix(c);
            mix(static_cast<unsigned long long>(entry.sizeBytes));
            mix(static_cast<unsigned long long>(entry.lastWriteTime.time_since_epoch().count()));
            mix(entry.isDirectory ? 1ull : 2ull);
            mix(entry.folderHasItems ? 3ull : 4ull);
        }
        mix(entries.size());
        return hash;
    }
    FileBrowser::RefreshResult BuildFileBrowserRefreshResult(const fs::path& currentPath, const std::string& searchFilter, bool showHiddenFiles) {
        FileBrowser::RefreshResult result;
        result.path = currentPath;
        result.filter = searchFilter;
        result.showHiddenFiles = showHiddenFiles;
        std::string filterLower = searchFilter;
        std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

        std::error_code iterEc;
        fs::directory_iterator it(currentPath, fs::directory_options::skip_permission_denied, iterEc);
        if (iterEc) {
            result.scanError = "Could not read " + pathToUtf8(currentPath) + ": " + iterEc.message();
            return result;
        }

        int skippedEntries = 0;
        for (const fs::directory_iterator end; it != end; it.increment(iterEc)) {
            if (iterEc) {
                result.scanError = "Stopped reading " + pathToUtf8(currentPath) + " after " +
                                   std::to_string(result.entries.size()) + " entries: " + iterEc.message();
                break;
            }
            const fs::directory_entry& entry = *it;
            const std::string filename = pathToUtf8(entry.path().filename());
            if (!showHiddenFiles && !filename.empty() && filename[0] == '.') continue;

            if (!filterLower.empty()) {
                std::string filenameLower = filename;
                std::transform(filenameLower.begin(), filenameLower.end(), filenameLower.begin(), ::tolower);
                if (filenameLower.find(filterLower) == std::string::npos) continue;
            }
            result.entries.push_back(entry);
        }

        std::sort(result.entries.begin(), result.entries.end(), [](const auto& a, const auto& b) {
            std::error_code aEc;
            std::error_code bEc;
            const bool aDir = a.is_directory(aEc);
            const bool bDir = b.is_directory(bEc);
            if (aDir != bDir) {return aDir > bDir;}
            return a.path().filename().native() < b.path().filename().native();
        });

        result.cachedEntries.reserve(result.entries.size());
        for (const fs::directory_entry& entry : result.entries) {
            if (Modularity::IsTranscodedTexturePath(pathToUtf8(entry.path()))) {
                continue;
            }

            FileBrowser::CachedEntry cached;
            cached.path = entry.path();
            cached.filename = pathToUtf8(cached.path.filename());
            cached.selectionKey = buildFileSelectionKey(cached.path);
            std::error_code entryEc;
            cached.isDirectory = entry.is_directory(entryEc);
            if (entryEc) {
                ++skippedEntries;
                continue;
            }
            try {
                cached.category = classifyFileBrowserEntry(entry);
            } catch (...) {
                cached.category = FileCategory::Unknown;
            }
            entryEc.clear();
            cached.lastWriteTime = entry.last_write_time(entryEc);
            cached.metadata = fileBrowserCategoryLabel(cached.category);
            if (cached.isDirectory) {cached.folderHasItems = folderHasVisibleItemsCached(cached.path, showHiddenFiles);}
            else {
                std::error_code sizeEc;
                cached.sizeBytes = entry.file_size(sizeEc);
                cached.hasSizeBytes = !sizeEc;
                if (cached.hasSizeBytes) {
                    cached.metadata += "  ";
                    cached.metadata += formatFileBrowserByteSize(cached.sizeBytes);
                }
            }
            result.cachedEntries.push_back(std::move(cached));
        }

        if (skippedEntries > 0 && result.scanError.empty()) {
            result.scanError = std::to_string(skippedEntries) + " item(s) in " +
                               pathToUtf8(currentPath) + " could not be read and were hidden.";
        }
        return result;
    }
    // Oh yeah, I forgot, not everyone wants to use the side scrollbar lol
    bool isTouchScrollableWindow(const ImGuiWindow* window) {
        if (!window || !window->Active || window->Collapsed || window->SkipItems) return false;
        return hasScrollableAxis(window, 0) || hasScrollableAxis(window, 1);
    }
    bool windowNameContains(const ImGuiWindow* window, const char* token) {
        return window && window->Name && token && std::strstr(window->Name, token) != nullptr;
    }
    bool isConsoleRelatedWindow(const ImGuiWindow* window) {
        if (!window) return false;
        if (windowNameContains(window, "ConsoleOutput") || windowNameContains(window, "Console##MiniLogPanel")) return true;
        const ImGuiWindow* root = window->RootWindow ? window->RootWindow : window;
        return windowNameContains(root, "Console##MiniLogPanel");
    }
    bool isAnimationRelatedWindow(const ImGuiWindow* window) {
        if (!window) return false;
        for (const ImGuiWindow* cursor = window; cursor != nullptr; cursor = cursor->ParentWindow) {
            if (windowNameContains(cursor, "Animation") ||
                windowNameContains(cursor, "AnimationMainArea") ||
                windowNameContains(cursor, "AnimationTimelineArea") ||
                windowNameContains(cursor, "AnimBindingTreePane") ||
                windowNameContains(cursor, "AnimDopesheetPane") ||
                windowNameContains(cursor, "AnimCurvesPane")) {
                return true;
            }
        }
        const ImGuiWindow* root = window->RootWindow ? window->RootWindow : window;
        return windowNameContains(root, "Animation");
    }
    bool shouldBypassGlobalSmoothScroll(const ImGuiWindow* window) {
        return isConsoleRelatedWindow(window) || isAnimationRelatedWindow(window);
    }
    ImGuiWindow* findScrollableWindowFromHover(ImGuiWindow* hovered) {
        for (ImGuiWindow* window = hovered; window != nullptr; window = window->ParentWindow) {
            if (isTouchScrollableWindow(window)) return window;
        }
        return nullptr;
    }
    float applyEdgeResistance(float value, float minValue, float maxValue, float resistance) {
        if (value < minValue) {return minValue + (value - minValue) * resistance; }
        if (value > maxValue) {return maxValue + (value - maxValue) * resistance;}
        return value;
    }
    float computeElasticOverscrollLimit(float axisExtent, float scrollMax) {
        const float byViewport = axisExtent * 0.14f;
        const float byRange = scrollMax * 0.16f + 6.0f;
        return ImClamp(std::min(byViewport, byRange), 6.0f, 28.0f);
    }
    float smoothDampScalar(float current, float target, float& currentVelocity, float smoothTime, float maxSpeed, float deltaTime) {
        smoothTime = ImMax(0.0001f, smoothTime);
        const float omega = 2.0f / smoothTime;
        const float x = omega * deltaTime;
        const float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
        float change = current - target;
        const float originalTarget = target;
        const float maxChange = maxSpeed * smoothTime;
        change = ImClamp(change, -maxChange, maxChange);
        target = current - change;
        const float temp = (currentVelocity + omega * change) * deltaTime;
        currentVelocity = (currentVelocity - omega * temp) * exp;
        float output = target + (change + temp) * exp;
        if (((originalTarget - current) > 0.0f) == (output > originalTarget)) {output = originalTarget; currentVelocity = 0.0f;}
        return output;
    }
}
const EditorChromeMetrics& getEditorChromeMetrics(EditorChromeScale scale) {
    static const EditorChromeMetrics kMetrics[] = {
        {
            0.84f,
            ImVec2(7.0f, 3.0f),
            ImVec2(7.0f, 2.0f),
            15.0f,
            3.0f,
            18.0f,
            ImVec2(88.0f, 27.0f),
            10.0f,
            ImVec2(520.0f, 300.0f)
        },
        {
            0.90f,
            ImVec2(9.0f, 4.0f),
            ImVec2(8.0f, 2.0f),
            16.0f,
            3.0f,
            20.0f,
            ImVec2(96.0f, 30.0f),
            12.0f,
            ImVec2(560.0f, 320.0f)
        },
        {
            0.98f,
            ImVec2(11.0f, 5.0f),
            ImVec2(10.0f, 3.0f),
            18.0f,
            4.0f,
            24.0f,
            ImVec2(108.0f, 34.0f),
            14.0f,
            ImVec2(620.0f, 360.0f)
        }
    };
    const int idx = std::clamp(static_cast<int>(scale), 0, 2);
    return kMetrics[idx];
}
const char* getEditorChromeScaleLabel(EditorChromeScale scale) {
    switch (scale) {
        case EditorChromeScale::Compact: return "Compact";
        case EditorChromeScale::Big: return "Big";
        case EditorChromeScale::Default:
        default:
            return "Default";
    }
}
#pragma region File Browser
FileBrowser::FileBrowser() {
    currentPath = fs::current_path();
    projectRoot = currentPath;
}
void FileBrowser::refresh() {
    if (refreshInFlight && refreshFuture.valid()) {
        const auto status = refreshFuture.wait_for(std::chrono::milliseconds(0));
        if (status == std::future_status::ready) {
            RefreshResult result = refreshFuture.get();
            refreshInFlight = false;
            if (result.path == currentPath &&
                result.filter == searchFilter &&
                result.showHiddenFiles == showHiddenFiles) {
                entries = std::move(result.entries);
                cachedEntries = std::move(result.cachedEntries);
                scanError = std::move(result.scanError);
                contentSignature = fileBrowserContentSignature(cachedEntries);
                hasContentSignature = true;
                needsRefresh = false;
            }
        }
    }
    if (!needsRefresh || refreshInFlight) return;
    const fs::path pathSnapshot = currentPath;
    const std::string filterSnapshot = searchFilter;
    const bool showHiddenSnapshot = showHiddenFiles;
    refreshInFlight = true;
    refreshFuture = std::async(std::launch::async,
        [pathSnapshot, filterSnapshot, showHiddenSnapshot]() {
            return BuildFileBrowserRefreshResult(pathSnapshot, filterSnapshot, showHiddenSnapshot);
        });
}
void FileBrowser::pollExternalChanges(double now) {
    if (externalPollInFlight && externalPollFuture.valid()) {
        if (externalPollFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            return;
        }
        RefreshResult result = externalPollFuture.get();
        externalPollInFlight = false;
        if (result.path != currentPath || result.filter != searchFilter ||
            result.showHiddenFiles != showHiddenFiles) {
            return;
        }
        scanError = result.scanError;
        const unsigned long long signature = fileBrowserContentSignature(result.cachedEntries);
        if (hasContentSignature && signature == contentSignature) {
            return;
        }
        if (needsRefresh || refreshInFlight) {
            return;
        }
        entries = std::move(result.entries);
        cachedEntries = std::move(result.cachedEntries);
        contentSignature = signature;
        hasContentSignature = true;
        return;
    }

    if (needsRefresh || refreshInFlight) return;
    if (currentPath.empty()) return;
    if (lastExternalPollTime >= 0.0 && (now - lastExternalPollTime) < externalPollInterval) return;
    lastExternalPollTime = now;

    const fs::path pathSnapshot = currentPath;
    const std::string filterSnapshot = searchFilter;
    const bool showHiddenSnapshot = showHiddenFiles;
    externalPollInFlight = true;
    externalPollFuture = std::async(std::launch::async,
        [pathSnapshot, filterSnapshot, showHiddenSnapshot]() {
            return BuildFileBrowserRefreshResult(pathSnapshot, filterSnapshot, showHiddenSnapshot);
        });
}
void FileBrowser::navigateUp() {
    if (currentPath.has_parent_path() && currentPath != currentPath.root_path()) {
        if (currentPath != projectRoot) {navigateTo(currentPath.parent_path());}
    }
}
void FileBrowser::navigateTo(const fs::path& path) {
    if (fs::is_directory(path)) {
        // Add to history
        if (historyIndex < 0 || pathHistory.empty() || pathHistory[historyIndex] != currentPath) {
            // Clear forward history
            if (historyIndex >= 0 && historyIndex < (int)pathHistory.size() - 1) {
                pathHistory.erase(pathHistory.begin() + historyIndex + 1, pathHistory.end());
            }
            pathHistory.push_back(currentPath);
            historyIndex = (int)pathHistory.size() - 1;
        }
        currentPath = path;
        selectedFile.clear();
        selectedFiles.clear();
        selectedFileKeys.clear();
        selectionAnchorIndex = -1;
        needsRefresh = true;
    }
}
void FileBrowser::navigateBack() {
    if (historyIndex > 0) {
        historyIndex--;
        currentPath = pathHistory[historyIndex];
        selectedFile.clear();
        selectedFiles.clear();
        selectedFileKeys.clear();
        selectionAnchorIndex = -1;
        needsRefresh = true;
    }
}
void FileBrowser::navigateForward() {
    if (historyIndex < (int)pathHistory.size() - 1) {
        historyIndex++;
        currentPath = pathHistory[historyIndex];
        selectedFile.clear();
        selectedFiles.clear();
        selectedFileKeys.clear();
        selectionAnchorIndex = -1;
        needsRefresh = true;
    }
}
void FileBrowser::setProjectRoot(const fs::path& root) {
    projectRoot = root;
    currentPath = root;
    selectedFile.clear();
    selectedFiles.clear();
    selectedFileKeys.clear();
    selectionAnchorIndex = -1;
    pathHistory.clear();
    historyIndex = -1;
    needsRefresh = true;
}
FileCategory FileBrowser::getFileCategory(const fs::directory_entry& entry) const {return classifyFileBrowserEntry(entry);}
const char* FileBrowser::getFileIcon(const fs::directory_entry& entry) const {
    FileCategory category = getFileCategory(entry);
    switch (category) {
        case FileCategory::Folder:  return "folder";
        case FileCategory::Scene:   return "scene";
        case FileCategory::Model:   return "model";
        case FileCategory::Material: return "material";
        case FileCategory::Texture: return "image";
        case FileCategory::Video:   return "video";
        case FileCategory::Shader:  return "text";
        case FileCategory::Script:  return "code";
        case FileCategory::Audio:   return "audio";
        case FileCategory::Text:    return "text";
        default:                    return "file";
    }
}
bool FileBrowser::isModelFile(const fs::directory_entry& entry) const {return getFileCategory(entry) == FileCategory::Model;}
bool FileBrowser::isSceneFile(const fs::directory_entry& entry) const {return getFileCategory(entry) == FileCategory::Scene;}
bool FileBrowser::isTextureFile(const fs::directory_entry& entry) const {return getFileCategory(entry) == FileCategory::Texture;}
bool FileBrowser::isVideoFile(const fs::directory_entry& entry) const {return getFileCategory(entry) == FileCategory::Video;}
bool FileBrowser::isOBJFile(const fs::directory_entry& entry) const {
    if (entry.is_directory()) return false;
    std::string ext = entry.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".obj";
}
bool FileBrowser::matchesFilter(const fs::directory_entry& entry) const {
    if (searchFilter.empty()) return true;
    std::string filename = entry.path().filename().string();
    std::string filterLower = searchFilter;
    std::string filenameLower = filename;
    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);
    std::transform(filenameLower.begin(), filenameLower.end(), filenameLower.begin(), ::tolower);
    return filenameLower.find(filterLower) != std::string::npos;
}
#pragma endregion
#pragma region ImGui Theme
void appendFontReport(std::string* outReport, const std::string& message) {
    if (!outReport || message.empty()) return;
    if (!outReport->empty()) {*outReport += " ";}
    *outReport += message;
}
static ImFont* AddFontFromAssetSourceTTF(ImGuiIO& io, const std::string& assetPath, float sizePx, const ImFontConfig* cfg = nullptr, const ImWchar* glyphRanges = nullptr);
bool mergeModularityEmojiFont(ImGuiIO& io, float fontSize, std::string* outReport) {
    #if defined(IMGUI_ENABLE_FREETYPE) && defined(IMGUI_USE_WCHAR32)
        struct EmojiFontCandidate {fs::path path; float bitmapStrikePixels;};
        const EmojiFontCandidate emojiFontCandidates[] = {
            { fs::path("Resources") / "Fonts" / "twemoji.ttf", 61.0f },
            { fs::path("Resources") / "Fonts" / "NotoColorEmoji.ttf", 109.0f },
            { fs::path("/usr/share/fonts/twemoji/twemoji.ttf"), 61.0f },
            { fs::path("/usr/share/fonts/noto/NotoColorEmoji.ttf"), 109.0f },
    #if defined(_WIN32)
            { fs::path("C:/Windows/Fonts/seguiemj.ttf"), 0.0f },
    #endif
        };
        static const ImWchar emojiRanges[] = {0x1, 0x1FFFF, 0};
        std::ostringstream report;
        for (const EmojiFontCandidate& candidate : emojiFontCandidates) {
            const fs::path& emojiPath = candidate.path;
            std::error_code ec;
            if (!fs::exists(emojiPath, ec) || ec) {report << "missing '" << emojiPath.string() << "'; "; continue;}
            if (!fs::is_regular_file(emojiPath, ec) || ec) {report << "invalid '" << emojiPath.string() << "'; "; continue;}
            const std::string emojiPathStr = emojiPath.generic_string();
            // i tuned these numbers by hand and i refuse to do it twice. :sob:
            ImFontConfig emojiConfig;
            emojiConfig.MergeMode = true;
            emojiConfig.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_LoadColor;
            emojiConfig.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_Bitmap;
            emojiConfig.GlyphMinAdvanceX = fontSize;
            if (candidate.bitmapStrikePixels > fontSize) {
                emojiConfig.RasterizerDensity = candidate.bitmapStrikePixels / fontSize;
            }
            ImFont* mergedFont = AddFontFromAssetSourceTTF(io, emojiPathStr, fontSize, &emojiConfig, emojiRanges);
            if (mergedFont) {
                if (ImFontBaked* baked = mergedFont->GetFontBaked(fontSize)) {
                    static const ImWchar preloadEmoji[] = {
                        0x1F600,
                        0x1F602,
                        0x1F62D,
                        0x1F642,
                        0x1F680,
                        0
                    };
                    for (const ImWchar* codepoint = preloadEmoji; *codepoint != 0; ++codepoint) {baked->FindGlyphNoFallback(*codepoint);}
                }   return true;
            }
            report << "ImGui rejected '" << emojiPathStr << "'; ";
        }
        appendFontReport(outReport, "Color emoji font load failed. Attempts: " + report.str());
        return false;
    #else
        appendFontReport(outReport, "Color emoji disabled because ImGui was built without FreeType/WCHAR32 support.");
        return false;
    #endif
}
static ImFont* AddFontFromAssetSourceTTF(ImGuiIO& io, const std::string& assetPath, float sizePx, const ImFontConfig* cfg, const ImWchar* glyphRanges) {
    auto& src = Modularity::Platform::GetAssetSource();
    std::vector<uint8_t> bytes = src.ReadAll(assetPath);
    if (bytes.empty()) return nullptr;
    void* owned = IM_ALLOC(bytes.size());
    if (!owned) return nullptr;
    std::memcpy(owned, bytes.data(), bytes.size());
    return io.Fonts->AddFontFromMemoryTTF(owned, static_cast<int>(bytes.size()), sizePx, cfg, glyphRanges);
}
ImFont* loadModularityUiFont(ImGuiIO& io, float fontSize, std::string* outReport) {
    ImFont* loadedFont = nullptr;
    fs::path primaryFontPath;
    std::ostringstream report;
    const fs::path fontCandidates[] = {
        fs::path("Resources") / "Fonts" / "TheSunset.ttf",
        fs::path("Resources") / "Fonts" / "Thesunsethd-Regular (1).ttf",
        fs::path("TheSunset.ttf"),
        fs::path("Thesunsethd-Regular (1).ttf")
    };
    std::error_code cwdEc;
    const fs::path currentWorkingDir = fs::current_path(cwdEc);
    for (const auto& fontPath : fontCandidates) {
        const std::string fontPathStr = fontPath.generic_string();
        loadedFont = AddFontFromAssetSourceTTF(io, fontPathStr, fontSize);
        if (loadedFont) {primaryFontPath = fontPath; break;}
        report << "missing or rejected '" << fontPathStr << "'; ";
    }
    if (!loadedFont) {
        if (outReport) {
            std::ostringstream finalReport;
            finalReport << "UI font load failed. cwd='" << (cwdEc ? std::string("<unavailable>") : currentWorkingDir.string()) << "'. Attempts: " << report.str();
            *outReport = finalReport.str();
        }
        return nullptr;
    }
    const fs::path fallbackCandidates[] = {
        fs::path("Resources") / "Fonts" / "TheSunset.ttf",
        fs::path("TheSunset.ttf")
    };
    if (primaryFontPath.filename() != "TheSunset.ttf") {
        for (const auto& fallbackPath : fallbackCandidates) {
            const std::string fallbackPathStr = fallbackPath.generic_string();
            ImFontConfig mergeConfig;
            mergeConfig.MergeMode = true;
            ImFont* fallbackFont = AddFontFromAssetSourceTTF( io, fallbackPathStr, fontSize, &mergeConfig, io.Fonts->GetGlyphRangesDefault());
            if (!fallbackFont) continue;
            break;
        }
    }
    mergeModularityEmojiFont(io, fontSize, outReport); return loadedFont;
}
void applyModernTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiIO& io = ImGui::GetIO();
    const float fontSize = std::max(1.0f, kModularityUiFontSizeBase + kModularityUiFontSizeOffset);
    std::string fontReport;
    ImFont* editorFont = loadModularityUiFont(io, fontSize, &fontReport);
    if (!editorFont) {
        ImFont* defaultFont = io.Fonts->AddFontDefault();
        if (defaultFont) {
            io.FontDefault = defaultFont;
            mergeModularityEmojiFont(io, fontSize, &fontReport);
        }
        if (!fontReport.empty()) {std::cerr << "[WARN] " << fontReport << std::endl;}
    } else {
        io.FontDefault = editorFont;
        if (!fontReport.empty()) {std::cerr << "[WARN] " << fontReport << std::endl;}
    }
    applyModularityStyle(style);
    ImGuiShadeTheme shade;
    applyModularityShadeTheme(shade);
    ImGui::SetShadeTheme(shade);
}

namespace {
const ImVec4 kModuAccent       = ImVec4(0.580f, 0.451f, 0.902f, 1.00f);
const ImVec4 kModuAccentBright = ImVec4(0.678f, 0.565f, 0.965f, 1.00f);
const ImVec4 kModuAccentDeep   = ImVec4(0.408f, 0.310f, 0.678f, 1.00f);
const ImVec4 kModuSelection    = ImVec4(0.325f, 0.259f, 0.522f, 1.00f);
const ImVec4 kModuOutline      = ImVec4(0.271f, 0.239f, 0.376f, 0.55f);
const ImVec4 kModuSeam         = ImVec4(0.086f, 0.075f, 0.122f, 1.00f);

inline ImVec4 ModuCol(int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}
inline ImVec4 ModuWithAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }
} // namespace

void applyModularityStyle(ImGuiStyle& style) {
    applyEditorLayoutPreset(style);
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_Text]                   = ModuCol(228, 225, 236);
    colors[ImGuiCol_TextDisabled]           = ModuCol(124, 118, 142);

    colors[ImGuiCol_WindowBg]               = ModuCol(38, 33, 52);
    colors[ImGuiCol_ChildBg]                = ModuCol(30, 26, 41);
    colors[ImGuiCol_PopupBg]                = ModuCol(43, 37, 58);
    colors[ImGuiCol_Border]                 = kModuOutline;
    colors[ImGuiCol_BorderShadow]           = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_MenuBarBg]              = ModuCol(45, 39, 60);

    colors[ImGuiCol_FrameBg]                = ModuCol(24, 21, 33);
    colors[ImGuiCol_FrameBgHovered]         = ModuCol(31, 27, 43);
    colors[ImGuiCol_FrameBgActive]          = ModuCol(36, 31, 49);

    colors[ImGuiCol_TitleBg]                = ModuCol(32, 28, 44);
    colors[ImGuiCol_TitleBgActive]          = ModuCol(48, 41, 67);
    colors[ImGuiCol_TitleBgCollapsed]       = ModuCol(28, 24, 38);

    colors[ImGuiCol_Button]                 = ModuCol(52, 45, 70);
    colors[ImGuiCol_ButtonHovered]          = ModuCol(64, 55, 87);
    colors[ImGuiCol_ButtonActive]           = ModuCol(41, 35, 56);

    colors[ImGuiCol_Header]                 = kModuSelection;
    colors[ImGuiCol_HeaderHovered]          = ModuCol(56, 48, 76);
    colors[ImGuiCol_HeaderActive]           = ModuWithAlpha(kModuAccentDeep, 1.0f);

    colors[ImGuiCol_Separator]              = kModuSeam;
    colors[ImGuiCol_SeparatorHovered]       = ModuWithAlpha(kModuAccent, 0.70f);
    colors[ImGuiCol_SeparatorActive]        = kModuAccentBright;

    colors[ImGuiCol_ScrollbarBg]            = ModuCol(23, 20, 32);
    colors[ImGuiCol_ScrollbarGrab]          = ModuCol(58, 51, 78);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ModuCol(72, 63, 96);
    colors[ImGuiCol_ScrollbarGrabActive]    = ModuCol(87, 75, 118);

    colors[ImGuiCol_CheckMark]              = kModuAccentBright;
    colors[ImGuiCol_SliderGrab]             = ModuCol(126, 108, 176);
    colors[ImGuiCol_SliderGrabActive]       = kModuAccent;

    colors[ImGuiCol_InputTextCursor]        = kModuAccentBright;

    colors[ImGuiCol_Tab]                    = ModuCol(31, 27, 42);
    colors[ImGuiCol_TabHovered]             = ModuCol(55, 47, 75);
    colors[ImGuiCol_TabSelected]            = ModuCol(48, 42, 66);
    colors[ImGuiCol_TabSelectedOverline]    = kModuAccent;
    colors[ImGuiCol_TabDimmed]              = ModuCol(26, 22, 35);
    colors[ImGuiCol_TabDimmedSelected]      = ModuCol(37, 32, 50);
    colors[ImGuiCol_TabDimmedSelectedOverline] = ModuWithAlpha(kModuAccent, 0.45f);

    colors[ImGuiCol_ResizeGrip]             = ModuWithAlpha(kModuAccent, 0.22f);
    colors[ImGuiCol_ResizeGripHovered]      = ModuWithAlpha(kModuAccent, 0.55f);
    colors[ImGuiCol_ResizeGripActive]       = kModuAccentBright;

    colors[ImGuiCol_DockingPreview]         = ModuWithAlpha(kModuAccent, 0.45f);
    colors[ImGuiCol_DockingEmptyBg]         = ModuCol(20, 17, 27);

    colors[ImGuiCol_PlotLines]              = ModuCol(150, 133, 197);
    colors[ImGuiCol_PlotLinesHovered]       = kModuAccentBright;
    colors[ImGuiCol_PlotHistogram]          = kModuAccent;
    colors[ImGuiCol_PlotHistogramHovered]   = kModuAccentBright;

    colors[ImGuiCol_TableHeaderBg]          = ModuCol(45, 39, 60);
    colors[ImGuiCol_TableBorderStrong]      = ModuCol(20, 17, 28);
    colors[ImGuiCol_TableBorderLight]       = ModuCol(30, 26, 40);
    colors[ImGuiCol_TableRowBg]             = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.0f, 1.0f, 1.0f, 0.022f);

    colors[ImGuiCol_TextLink]               = kModuAccentBright;
    colors[ImGuiCol_TextSelectedBg]         = ModuWithAlpha(kModuAccent, 0.32f);
    colors[ImGuiCol_TreeLines]              = ModuCol(62, 55, 82);
    colors[ImGuiCol_DragDropTarget]         = kModuAccentBright;
    colors[ImGuiCol_DragDropTargetBg]       = ModuWithAlpha(kModuAccent, 0.20f);
    colors[ImGuiCol_UnsavedMarker]          = ModuCol(224, 200, 120);
    colors[ImGuiCol_NavCursor]              = kModuAccentBright;
    colors[ImGuiCol_NavWindowingHighlight]  = ModuWithAlpha(kModuAccentBright, 0.80f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.04f, 0.03f, 0.06f, 0.45f);
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.04f, 0.03f, 0.06f, 0.55f);

    style.WindowRounding = 9.0f;
    style.ChildRounding = 7.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 9.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 7.0f;
    style.ScrollbarRounding = 7.0f;

    style.TabBorderSize = 0.0f;

    style.GlassBlur = false;
    style.CheckboxSwitch = true;
    style.SliderPill = false;
}

void applyModularityShadeTheme(ImGuiShadeTheme& theme) {
    ImGui::StyleShadeThemeDesktop(&theme);

    const ImU32 seamShadow = IM_COL32(0, 0, 0, 96);
    const ImU32 violetLift = IM_COL32(198, 178, 255, 34);

    ImGuiShadeParams& frameHovered = theme.Params[ImGuiShadeClass_Frame][ImGuiShadeState_Hovered];
    frameHovered.ColTopHighlight = IM_COL32(0, 0, 0, 70);
    frameHovered.ColBottomShadow = IM_COL32(214, 198, 255, 34);

    theme.Params[ImGuiShadeClass_Button][ImGuiShadeState_Normal].ColTopHighlight = violetLift;
    theme.Params[ImGuiShadeClass_Button][ImGuiShadeState_Hovered].ColTopHighlight = IM_COL32(214, 198, 255, 48);
    theme.Params[ImGuiShadeClass_TitleBar][ImGuiShadeState_Focused].ColTopHighlight = IM_COL32(206, 186, 255, 46);
    theme.Params[ImGuiShadeClass_TabActive][ImGuiShadeState_Normal].ColTopHighlight = violetLift;
    theme.Params[ImGuiShadeClass_MenuBar][ImGuiShadeState_Normal].ColTopHighlight = violetLift;

    theme.Params[ImGuiShadeClass_MenuBar][ImGuiShadeState_Normal].ColBottomShadow = seamShadow;
    theme.Params[ImGuiShadeClass_TitleBar][ImGuiShadeState_Normal].ColBottomShadow = seamShadow;
    theme.Params[ImGuiShadeClass_TitleBar][ImGuiShadeState_Focused].ColBottomShadow = seamShadow;

    ImGuiShadeParams& child = theme.Params[ImGuiShadeClass_Child][ImGuiShadeState_Normal];
    child.ColTopHighlight = IM_COL32(0, 0, 0, 58);
    child.ColBottomShadow = IM_COL32(255, 255, 255, 14);

    theme.Params[ImGuiShadeClass_Popup][ImGuiShadeState_Normal].ColTopHighlight = IM_COL32(214, 198, 255, 40);
    theme.Params[ImGuiShadeClass_Popup][ImGuiShadeState_Normal].ColBottomShadow = IM_COL32(0, 0, 0, 100);

    theme.Params[ImGuiShadeClass_Header][ImGuiShadeState_Selected].Flags =
        ImGuiShadeFlags_Set | ImGuiShadeFlags_NoBevel;
    theme.Params[ImGuiShadeClass_Header][ImGuiShadeState_Selected].GradientTop = 0.030f;
    theme.Params[ImGuiShadeClass_Header][ImGuiShadeState_Selected].GradientBottom = -0.022f;

    theme.Params[ImGuiShadeClass_Grab][ImGuiShadeState_Normal].ColTopHighlight = IM_COL32(220, 206, 255, 52);
    theme.Params[ImGuiShadeClass_TableHeader][ImGuiShadeState_Normal].ColTopHighlight = violetLift;
    theme.Params[ImGuiShadeClass_TableHeader][ImGuiShadeState_Normal].ColBottomShadow = seamShadow;
}

void applySlateStyle(ImGuiStyle& style) {
    ImVec4* colors = style.Colors;
    ImVec4 slate = ImVec4(0.11f, 0.12f, 0.19f, 1.00f);
    ImVec4 panel = ImVec4(0.16f, 0.16f, 0.24f, 1.00f);
    ImVec4 overlay = ImVec4(0.10f, 0.11f, 0.17f, 0.98f);
    ImVec4 accent = ImVec4(0.48f, 0.56f, 0.86f, 1.00f);
    ImVec4 accentMuted = ImVec4(0.38f, 0.46f, 0.74f, 1.00f);
    ImVec4 highlight = ImVec4(0.22f, 0.23f, 0.34f, 1.00f);
    colors[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.97f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.62f, 0.70f, 1.00f);
    colors[ImGuiCol_WindowBg] = slate;
    colors[ImGuiCol_ChildBg] = panel;
    colors[ImGuiCol_PopupBg] = overlay;
    colors[ImGuiCol_Border] = ImVec4(0.22f, 0.23f, 0.34f, 0.70f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.09f, 0.10f, 0.16f, 1.00f);
    colors[ImGuiCol_Header] = highlight;
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.28f, 0.38f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.30f, 0.42f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.22f, 0.23f, 0.32f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.30f, 0.42f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.33f, 0.36f, 0.48f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.21f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.28f, 0.40f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.34f, 0.46f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.11f, 0.12f, 0.18f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.17f, 0.24f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.09f, 0.10f, 0.15f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.16f, 0.24f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.34f, 0.48f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.20f, 0.22f, 0.32f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.11f, 0.12f, 0.18f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.16f, 0.18f, 0.26f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.23f, 0.34f, 1.00f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.34f, 0.36f, 0.52f, 1.00f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.44f, 0.50f, 0.70f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.11f, 0.12f, 0.18f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.26f, 0.36f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.32f, 0.35f, 0.48f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.36f, 0.42f, 0.58f, 1.00f);
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = accentMuted;
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.28f, 0.30f, 0.42f, 1.00f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.38f, 0.44f, 0.60f, 0.80f);
    colors[ImGuiCol_ResizeGripActive] = accent;
    colors[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.08f, 0.09f, 0.14f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.24f);
    colors[ImGuiCol_NavHighlight] = accent;
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.20f, 0.22f, 0.32f, 1.00f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.05f, 0.06f, 0.09f, 0.70f);
    applyEditorLayoutPreset(style);
    style.GlassBlur = false;
    style.CheckboxSwitch = false;
    style.SliderPill = false;
}

// Yey! new glass theme!
void applyGlassStyle(ImGuiStyle& style) {
    applyEditorLayoutPreset(style);
    ImVec4* colors = style.Colors;
    const ImVec4 accent = ImVec4(0.42f, 0.62f, 1.00f, 1.00f);   // selection (I really should name stuff better..)
    const ImVec4 switchOn = ImVec4(0.32f, 0.83f, 0.41f, 1.00f);
    colors[ImGuiCol_Text] = ImVec4(0.97f, 0.97f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.66f, 0.66f, 0.70f, 1.00f);

    colors[ImGuiCol_WindowBg] = ImVec4(0.13f, 0.13f, 0.145f, 0.66f);
    colors[ImGuiCol_ChildBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.035f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.14f, 0.14f, 0.155f, 0.70f);
    colors[ImGuiCol_Border] = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_MenuBarBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.04f);
    colors[ImGuiCol_Header] = ImVec4(1.00f, 1.00f, 1.00f, 0.09f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.14f);
    colors[ImGuiCol_HeaderActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.19f);
    colors[ImGuiCol_Button] = ImVec4(1.00f, 1.00f, 1.00f, 0.11f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.18f);
    colors[ImGuiCol_ButtonActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.25f);
    colors[ImGuiCol_FrameBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.13f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.18f);
    colors[ImGuiCol_TitleBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.13f, 0.13f, 0.145f, 0.60f);
    colors[ImGuiCol_Tab] = ImVec4(1.00f, 1.00f, 1.00f, 0.04f);
    colors[ImGuiCol_TabHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.15f);
    colors[ImGuiCol_TabActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.13f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
    colors[ImGuiCol_Separator] = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.20f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(accent.x, accent.y, accent.z, 0.80f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(1.00f, 1.00f, 1.00f, 0.24f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.34f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.45f);

    // Welp, The CheckMark doubles as the switch-on track color, and the 'SliderGrabActive' doubles as the pill slider's fill color
    colors[ImGuiCol_CheckMark] = switchOn;
    colors[ImGuiCol_SliderGrab] = ImVec4(1.00f, 1.00f, 1.00f, 0.95f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(accent.x, accent.y, accent.z, 0.90f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.25f);
    colors[ImGuiCol_ResizeGripActive] = accent;
    colors[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.30f);
    colors[ImGuiCol_NavHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.55f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.03f, 0.03f, 0.04f, 0.45f);

    style.WindowRounding = 16.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 10.0f;
    style.PopupRounding = 14.0f;
    style.GrabRounding = 12.0f;
    style.TabRounding = 10.0f;
    style.ScrollbarRounding = 12.0f;
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(7.0f, 4.0f);
    style.ItemSpacing = ImVec2(10.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.CellPadding = ImVec2(5.0f, 3.0f);
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 12.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.GlassBlur = true;
    style.CheckboxSwitch = true;
    style.SliderPill = true;
}
void applyEditorLayoutPreset(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(3.0f, 3.0f);
    style.FramePadding = ImVec2(4.0f, 4.0f);
    style.ItemSpacing = ImVec2(10.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(2.0f, 2.0f);
    style.CellPadding = ImVec2(4.0f, 2.0f);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing = 11.0f;
    style.GrabMinSize = 8.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowRounding = 12.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 12.0f;
    style.PopupRounding = 12.0f;
    style.GrabRounding = 12.0f;
    style.ScrollbarSize = 11.0f;
    style.ScrollbarRounding = 10.0f;
    style.ScrollbarPadding = 1.0f;
    style.TabBorderSize = 1.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = 1.0f;
    style.TabMinWidthBase = 1.0f;
    style.TabMinWidthShrink = 80.0f;
    style.TabCloseButtonMinWidthSelected = -1.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.TabRounding = 10.0f;
    style.TableAngledHeadersAngle = 35.0f;
    style.TableAngledHeadersTextAlign = ImVec2(0.50f, 0.00f);
    style.TreeLinesFlags = ImGuiTreeNodeFlags_DrawLinesNone;
    style.TreeLinesSize = 1.0f;
    style.TreeLinesRounding = 0.0f;
    style.WindowTitleAlign = ImVec2(0.50f, 0.50f);
    style.WindowBorderHoverPadding = 6.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.50f, 0.50f);
    style.SelectableTextAlign = ImVec2(0.00f, 0.00f);
    style.SeparatorTextBorderSize = 2.0f;
    style.SeparatorTextAlign = ImVec2(0.50f, 0.50f);
    style.SeparatorTextPadding = ImVec2(4.0f, 0.0f);
    style.LogSliderDeadzone = 4.0f;
    style.ImageBorderSize = 0.0f;
    style.DockingNodeHasCloseButton = true;
    style.DockingSeparatorSize = 0.0f;
    style.DisplayWindowPadding = ImVec2(19.0f, 19.0f);
    style.DisplaySafeAreaPadding = ImVec2(0.0f, 0.0f);
}
void applyPixelStyle(ImGuiStyle& style) {
    applyEditorLayoutPreset(style);
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.WindowPadding = ImVec2(8.0f, 6.0f);
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(6.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 14.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;
}
void applySuperRoundStyle(ImGuiStyle& style) {
    applyEditorLayoutPreset(style);
    style.WindowRounding = 18.0f;
    style.ChildRounding = 16.0f;
    style.FrameRounding = 16.0f;
    style.PopupRounding = 16.0f;
    style.ScrollbarRounding = 16.0f;
    style.GrabRounding = 14.0f;
    style.TabRounding = 16.0f;
    style.WindowPadding = ImVec2(14.0f, 10.0f);
    style.FramePadding = ImVec2(12.0f, 8.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.IndentSpacing = 18.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
}
#pragma endregion
#pragma region Card Dialogs
// Welp, now there's a Pop-up window, this basically just shows up stuff for the rename/delete confirmation window.
bool beginCardModal(const char* name, float width, bool* open, const CardModalIcon& icon) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (width <= 0.0f) {
        width = ImGui::GetFontSize() * 21.0f;
    }
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 20.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 10.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::BeginPopupModal(name, open, flags)) {
        ImGui::PopStyleVar(3);
        return false;
    }

    if (icon.id != static_cast<ImTextureID>(0)) {
        const float iconSize = ImGui::GetFontSize() * 3.0f;
        const ImVec2 uvMin = icon.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
        const ImVec2 uvMax = icon.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
        ImGui::SetCursorPosX(ImMax(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - iconSize) * 0.5f));
        ImGui::Image(icon.id, ImVec2(iconSize, iconSize), uvMin, uvMax);
        ImGui::Spacing();
    }

    const char* titleEnd = ImGui::FindRenderedTextEnd(name);
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.15f);
    const float titleWidth = ImGui::CalcTextSize(name, titleEnd).x;
    ImGui::SetCursorPosX(ImMax(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - titleWidth) * 0.5f));
    ImGui::TextUnformatted(name, titleEnd);
    ImGui::PopFont();
    ImGui::Spacing();
    return true;
}
void cardModalText(const char* text) {
    const float wrapWidth = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    if (textSize.x <= wrapWidth) {
        ImGui::SetCursorPosX(ImMax(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - textSize.x) * 0.5f));
        ImGui::TextUnformatted(text);
    } else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
    }
    ImGui::PopStyleColor();
    ImGui::Spacing();
}
bool cardModalButton(const char* label, CardButtonKind kind, int index, int count) {
    const ImGuiStyle& style = ImGui::GetStyle();
    if (index > 0) ImGui::SameLine();
    count = std::max(count, 1);
    const float rowWidth = ImGui::GetWindowWidth() - style.WindowPadding.x * 2.0f;
    const float buttonWidth = (rowWidth - style.ItemSpacing.x * (float)(count - 1)) / (float)count;
    ImVec4 base;
    bool tinted = true;
    switch (kind) {
        case CardButtonKind::Primary: base = ImVec4(0.42f, 0.62f, 1.00f, 0.85f); break;
        case CardButtonKind::Danger:  base = ImVec4(0.92f, 0.32f, 0.34f, 0.85f); break;
        default:                      tinted = false; break;
    }
    if (tinted) {
        ImGui::PushStyleColor(ImGuiCol_Button, base);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(base.x, base.y, base.z, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(base.x * 0.85f, base.y * 0.85f, base.z * 0.85f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f + 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, style.FramePadding.y + 2.0f));
    const bool pressed = ImGui::Button(label, ImVec2(buttonWidth, 0.0f));
    ImGui::PopStyleVar(2);
    if (tinted) ImGui::PopStyleColor(4);
    return pressed;
}
void endCardModal() {
    ImGui::EndPopup();
    ImGui::PopStyleVar(3);
}
#pragma endregion
#pragma region Dockspace
ImGuiID setupDockspace(EditorChromeScale chromeScale) {
    static bool dockspaceOpen = true;
    static ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_None;
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDocking;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    windowFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // Let's just.. Make the toolbar opaque, wouldn't want the engine to waste resources on nothing behind it lol
    ImVec4 dockspaceBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    dockspaceBg.w = 1.0f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, dockspaceBg);
    ImGui::Begin("DockSpace", &dockspaceOpen, windowFlags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    ImGuiID dockspaceId = ImGui::GetID("MainDockspace");
    const float reserveHeight = getEditorBottomStatusReserveHeight(chromeScale);
    ImVec2 dockspaceSize = ImGui::GetContentRegionAvail();
    dockspaceSize.y = ImMax(0.0f, dockspaceSize.y - reserveHeight);
    ImGui::DockSpace(dockspaceId, dockspaceSize, dockspaceFlags);
    ImGui::End();
    return dockspaceId;
}
float getEditorBottomStatusReserveHeight(EditorChromeScale chromeScale) {return getEditorChromeMetrics(chromeScale).bottomReserveHeight;}
#pragma endregion
#pragma region Touch Swipe Scroll
void updateTouchSwipeScrolling() {
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context) return;
    ImGuiContext& g = *context;
    ImGuiIO& io = ImGui::GetIO();
    static TouchSwipeRuntimeState runtime;
    const bool touchScreenMode = (io.ConfigFlags & ImGuiConfigFlags_IsTouchScreen) != 0;
    if (!touchScreenMode) {
        runtime.windowStates.clear();
        runtime.activeWindowId = 0;
        runtime.dragging = false;
        return;
    }
    const bool hasWheelInput = std::abs(io.MouseWheelH) > 0.0001f || std::abs(io.MouseWheel) > 0.0001f;
    ImVec2 wheel(io.MouseWheelH, io.MouseWheel);
    if (io.MouseWheelRequestAxisSwap) {
        wheel = ImVec2(wheel.y, 0.0f);
    }
    ImGuiWindow* wheelWindow = nullptr;
    if ((std::abs(wheel.x) > 0.0001f || std::abs(wheel.y) > 0.0001f) && g.MovingWindow == nullptr) {
        ImGuiWindow* baseWindow = g.WheelingWindow ? g.WheelingWindow : g.HoveredWindow;
        wheelWindow = findScrollableWindowFromHover(baseWindow);
        if (shouldBypassGlobalSmoothScroll(wheelWindow)) {
            wheelWindow = nullptr;
        }
    }
    const bool pointerActive = io.MouseDown[0] || io.MouseClicked[0] || runtime.dragging || runtime.activeWindowId != 0;
    if (!hasWheelInput && !pointerActive) {
        bool hasResidualMotion = false;
        for (const auto& [id, state] : runtime.windowStates) {
            (void)id;
            if (state.isDragging ||
                std::abs(state.inputVelocity.x) > 6.0f ||
                std::abs(state.inputVelocity.y) > 6.0f ||
                std::abs(state.smoothVelocity.x) > 6.0f ||
                std::abs(state.smoothVelocity.y) > 6.0f ||
                std::abs(state.targetScroll.x - state.currentScroll.x) > 0.08f ||
                std::abs(state.targetScroll.y - state.currentScroll.y) > 0.08f) {
                hasResidualMotion = true;
                break;
            }
        }
        if (!hasResidualMotion) {runtime.windowStates.clear(); return;}
    }
    const float dt = ImClamp(io.DeltaTime, 1.0f / 240.0f, 1.0f / 30.0f);
    const float dragThresholdSqr = 16.0f;
    const float edgeResistance = 0.34f;
    const float wheelVelocityBlend = 0.14f;
    const float touchDragVelocityBlend = 0.22f;
    const float wheelImpulseScale = 0.24f;
    const float overscrollEdgeImpulseScale = 0.10f;
    const float freeScrollFriction = 7.2f;
    const float overscrollInputFriction = 11.0f;
    const float overscrollReturnSmoothing = 8.0f;
    const float scrollSmoothTime = 0.13f;
    const float dragSmoothTime = 0.055f;
    const float overscrollSmoothTime = 0.15f;
    const float maxSmoothSpeed = 5000.0f;
    const float settleVelocityEpsilon = 6.0f;
    const float settlePositionEpsilon = 0.04f;
    const float boundaryEpsilon = 0.10f;
    for (auto& [id, state] : runtime.windowStates) {
        state.touchedThisFrame = false;
        if (id != runtime.activeWindowId) state.isDragging = false;
    }
    for (ImGuiWindow* window : g.Windows) {
        if (!isTouchScrollableWindow(window)) continue;
        TouchSwipeWindowState& state = runtime.windowStates[window->ID];
        state.touchedThisFrame = true;
        if (!state.initialized) {
            state.initialized = true;
            state.targetScroll = window->Scroll;
            state.currentScroll = window->Scroll;
            state.inputVelocity = ImVec2(0.0f, 0.0f);
            state.smoothVelocity = ImVec2(0.0f, 0.0f);
            continue;
        }
        const bool stateIsIdle = !state.isDragging && std::abs(state.inputVelocity.x) < settleVelocityEpsilon && std::abs(state.inputVelocity.y) < settleVelocityEpsilon && std::abs(state.smoothVelocity.x) < settleVelocityEpsilon && std::abs(state.smoothVelocity.y) < settleVelocityEpsilon && std::abs(state.targetScroll.x - state.currentScroll.x) < boundaryEpsilon && std::abs(state.targetScroll.y - state.currentScroll.y) < boundaryEpsilon;
        if (stateIsIdle && !(wheelWindow && wheelWindow->ID == window->ID)) {
            state.targetScroll = window->Scroll;
            state.currentScroll = window->Scroll;
            state.inputVelocity = ImVec2(0.0f, 0.0f);
            state.smoothVelocity = ImVec2(0.0f, 0.0f);
        }
    }
    if (runtime.activeWindowId != 0) {
        ImGuiWindow* activeWindow = ImGui::FindWindowByID(runtime.activeWindowId);
        auto it = runtime.windowStates.find(runtime.activeWindowId);
        if (!activeWindow || !isTouchScrollableWindow(activeWindow) ||
            it == runtime.windowStates.end() || !it->second.touchedThisFrame) {
            runtime.activeWindowId = 0;
            runtime.dragging = false;
        }
    }
    if (touchScreenMode) {
        if (io.MouseClicked[0] && runtime.activeWindowId == 0 &&
            g.ActiveId == 0 && g.MovingWindow == nullptr) {
            ImGuiWindow* hovered = findScrollableWindowFromHover(g.HoveredWindow);
            if (hovered && !shouldBypassGlobalSmoothScroll(hovered)) {
                const bool clickInTitleBar = hovered->TitleBarHeight > 0.0f && hovered->TitleBarRect().Contains(io.MouseClickedPos[0]);
                if (!clickInTitleBar) {
                    runtime.activeWindowId = hovered->ID;
                    runtime.dragStartPos = io.MouseClickedPos[0];
                    runtime.lastPointerPos = io.MousePos;
                    runtime.dragging = false;
                    TouchSwipeWindowState& state = runtime.windowStates[hovered->ID];
                    state.isDragging = false;
                    state.inputVelocity = ImVec2(0.0f, 0.0f);
                    state.smoothVelocity = ImVec2(0.0f, 0.0f);
                    state.targetScroll = hovered->Scroll;
                    state.currentScroll = hovered->Scroll;
                    state.initialized = true;
                }
            }
        }
        if (!io.MouseDown[0]) {
            if (runtime.activeWindowId != 0) {
                auto it = runtime.windowStates.find(runtime.activeWindowId);
                if (it != runtime.windowStates.end()) { it->second.isDragging = false;
                }
            }
            runtime.activeWindowId = 0;
            runtime.dragging = false;
        } else if (runtime.activeWindowId != 0) {
            ImGuiWindow* activeWindow = ImGui::FindWindowByID(runtime.activeWindowId);
            auto it = runtime.windowStates.find(runtime.activeWindowId);
            if (activeWindow && it != runtime.windowStates.end()) {
                TouchSwipeWindowState& state = it->second;
                const ImVec2 totalDragDelta(
                    io.MousePos.x - runtime.dragStartPos.x,
                    io.MousePos.y - runtime.dragStartPos.y);
                if (!runtime.dragging && ImLengthSqr(totalDragDelta) >= dragThresholdSqr) {
                    runtime.dragging = true;
                    state.isDragging = true;
                }
                const ImVec2 pointerDelta(
                    io.MousePos.x - runtime.lastPointerPos.x,
                    io.MousePos.y - runtime.lastPointerPos.y);
                runtime.lastPointerPos = io.MousePos;
                if (runtime.dragging && g.ActiveId == 0 && g.MovingWindow == nullptr) {
                    for (int axis = 0; axis < 2; ++axis) {
                        if (!hasScrollableAxis(activeWindow, axis)) continue;
                        const float maxScroll = activeWindow->ScrollMax[axis];
                        const float previousValue = state.targetScroll[axis];
                        const float draggedValue = previousValue - pointerDelta[axis];
                        state.targetScroll[axis] = applyEdgeResistance(draggedValue, 0.0f, maxScroll, edgeResistance);
                        const float frameVelocity = (state.targetScroll[axis] - previousValue) / dt;
                        state.inputVelocity[axis] = ImLerp(state.inputVelocity[axis], frameVelocity, touchDragVelocityBlend);
                    }
                }
            } else {runtime.activeWindowId = 0; runtime.dragging = false;}
        }
    } else {
        if (runtime.activeWindowId != 0) {
            auto it = runtime.windowStates.find(runtime.activeWindowId);
            if (it != runtime.windowStates.end()) it->second.isDragging = false;
        }
        runtime.activeWindowId = 0;
        runtime.dragging = false;
    }
    for (auto& [windowId, state] : runtime.windowStates) {
        if (!state.touchedThisFrame) continue;
        ImGuiWindow* window = ImGui::FindWindowByID(windowId);
        if (!window) continue;
        if (shouldBypassGlobalSmoothScroll(window)) {
            state.targetScroll = window->Scroll;
            state.currentScroll = window->Scroll;
            state.inputVelocity = ImVec2(0.0f, 0.0f);
            state.smoothVelocity = ImVec2(0.0f, 0.0f);
            continue;
        }
        const bool draggingThisWindow = runtime.dragging && runtime.activeWindowId == windowId && io.MouseDown[0] && state.isDragging;
        for (int axis = 0; axis < 2; ++axis) {
            if (!hasScrollableAxis(window, axis)) {
                state.targetScroll[axis] = 0.0f;
                state.currentScroll[axis] = 0.0f;
                state.inputVelocity[axis] = 0.0f;
                state.smoothVelocity[axis] = 0.0f;
                continue;
            }
            const float maxScroll = window->ScrollMax[axis];
            const float wheelDelta = (axis == 0) ? wheel.x : wheel.y;
            const float externalDelta = window->Scroll[axis] - state.currentScroll[axis];
            const bool wheelTargetsThisWindow = !touchScreenMode && wheelWindow && wheelWindow->ID == windowId;
            const bool wheelAxisInput = wheelTargetsThisWindow && (std::abs(wheelDelta) > 0.0001f || std::abs(externalDelta) > 0.001f);
            const bool hasAxisInput = draggingThisWindow || wheelAxisInput;
            if (!draggingThisWindow) {
                const bool outsideBounds = state.targetScroll[axis] < -0.01f || state.targetScroll[axis] > maxScroll + 0.01f;
                if (!touchScreenMode &&
                    !wheelAxisInput &&
                    std::abs(state.inputVelocity[axis]) < settleVelocityEpsilon &&
                    std::abs(state.smoothVelocity[axis]) < settleVelocityEpsilon &&
                    std::abs(state.targetScroll[axis] - state.currentScroll[axis]) < boundaryEpsilon &&
                    !outsideBounds) {
                    state.targetScroll[axis] = window->Scroll[axis];
                    state.currentScroll[axis] = window->Scroll[axis];
                    state.inputVelocity[axis] = 0.0f;
                    state.smoothVelocity[axis] = 0.0f;
                    continue;
                }
                if (wheelAxisInput) {
                    state.targetScroll[axis] += externalDelta;
                    const float impulseVelocity = ImClamp(externalDelta / dt, -2200.0f, 2200.0f) * wheelImpulseScale;
                    state.inputVelocity[axis] = ImLerp(state.inputVelocity[axis], impulseVelocity, wheelVelocityBlend);
                    const bool pushingMin = wheelDelta > 0.0f;
                    const bool pushingMax = wheelDelta < 0.0f;
                    const bool atMin = state.targetScroll[axis] <= 0.5f;
                    const bool atMax = state.targetScroll[axis] >= maxScroll - 0.5f;
                    if ((pushingMin && atMin) || (pushingMax && atMax)) {
                        const float maxStep = (axis == 0) ? (window->InnerRect.GetWidth() * 0.67f) : (window->InnerRect.GetHeight() * 0.67f);
                        const float baseStep = (axis == 0) ? (1.0f * window->FontRefSize) : (2.2f * window->FontRefSize);
                        const float scrollStep = ImTrunc(ImMin(baseStep, maxStep));
                        const float delta = -wheelDelta * scrollStep;
                        const float axisExtent = (axis == 0) ? window->InnerRect.GetWidth() : window->InnerRect.GetHeight();
                        const float overscrollLimit = computeElasticOverscrollLimit(axisExtent, maxScroll);
                        const float clampedValue = ImClamp(state.targetScroll[axis], 0.0f, maxScroll);
                        const float overshoot = std::abs(state.targetScroll[axis] - clampedValue);
                        const float remainingFactor = ImClamp( 1.0f - (overshoot / std::max(overscrollLimit, 0.001f)), 0.10f, 1.0f);
                        const float prev = state.targetScroll[axis];
                        state.targetScroll[axis] = applyEdgeResistance( prev + delta * 0.10f * remainingFactor, 0.0f, maxScroll, 0.45f);
                        const float edgeImpulse = ImClamp((delta * remainingFactor) / dt, -1000.0f, 1000.0f) * overscrollEdgeImpulseScale;
                        state.inputVelocity[axis] = ImLerp(state.inputVelocity[axis], edgeImpulse, wheelVelocityBlend);
                    }
                } else if (!touchScreenMode) {
                    // Basically show the scrollbar stuff if you ain't on android
                    const float syncThreshold = ImClamp(maxScroll * 0.012f, 0.01f, 0.20f);
                    if (maxScroll <= 1.0f || std::abs(externalDelta) > syncThreshold) {
                        state.targetScroll[axis] = window->Scroll[axis];
                        state.inputVelocity[axis] = 0.0f;
                    }
                }
                state.targetScroll[axis] += state.inputVelocity[axis] * dt;
                const float clampedTarget = ImClamp(state.targetScroll[axis], 0.0f, maxScroll);
                const float stretch = state.targetScroll[axis] - clampedTarget;
                if (std::abs(stretch) > 0.0f) {state.inputVelocity[axis] *= std::exp(-overscrollInputFriction * dt);}
                else {state.inputVelocity[axis] *= std::exp(-freeScrollFriction * dt);}
                if (std::abs(state.inputVelocity[axis]) < settleVelocityEpsilon) {state.inputVelocity[axis] = 0.0f;}
                if (!hasAxisInput) {
                    const float returnBlend = 1.0f - std::exp(-overscrollReturnSmoothing * dt);
                    state.targetScroll[axis] = ImLerp(state.targetScroll[axis], clampedTarget, returnBlend);
                    if (std::abs(state.targetScroll[axis] - clampedTarget) < boundaryEpsilon) {state.targetScroll[axis] = clampedTarget;}
                }
            }
            const float axisExtent = (axis == 0) ? window->InnerRect.GetWidth() : window->InnerRect.GetHeight();
            const float overscrollLimit = computeElasticOverscrollLimit(axisExtent, maxScroll);
            state.targetScroll[axis] = ImClamp(state.targetScroll[axis], -overscrollLimit, maxScroll + overscrollLimit);
            const bool targetOutsideBounds = state.targetScroll[axis] < 0.0f || state.targetScroll[axis] > maxScroll;
            const float smoothTime = draggingThisWindow ? dragSmoothTime : (targetOutsideBounds ? overscrollSmoothTime : scrollSmoothTime);
            state.currentScroll[axis] = smoothDampScalar(state.currentScroll[axis], state.targetScroll[axis], state.smoothVelocity[axis], smoothTime, maxSmoothSpeed, dt);
            state.currentScroll[axis] = ImClamp( state.currentScroll[axis], -overscrollLimit, maxScroll + overscrollLimit);
            const float clampedCurrent = ImClamp(state.currentScroll[axis], 0.0f, maxScroll);
            const float clampedTarget = ImClamp(state.targetScroll[axis], 0.0f, maxScroll);
            if (!hasAxisInput &&
                std::abs(state.currentScroll[axis] - clampedCurrent) < boundaryEpsilon &&
                std::abs(state.targetScroll[axis] - clampedTarget) < boundaryEpsilon) {
                state.currentScroll[axis] = clampedCurrent;
                state.targetScroll[axis] = clampedTarget;
                if ((clampedCurrent <= 0.0f && state.smoothVelocity[axis] < 0.0f) ||
                    (clampedCurrent >= maxScroll && state.smoothVelocity[axis] > 0.0f)) {
                    state.smoothVelocity[axis] = 0.0f;
                }
            }
            if (!hasAxisInput &&
                std::abs(state.currentScroll[axis] - state.targetScroll[axis]) < settlePositionEpsilon &&
                std::abs(state.inputVelocity[axis]) < settleVelocityEpsilon &&
                std::abs(state.smoothVelocity[axis]) < settleVelocityEpsilon) {
                state.currentScroll[axis] = state.targetScroll[axis];
                state.inputVelocity[axis] = 0.0f;
                state.smoothVelocity[axis] = 0.0f;
            }
            if (axis == 0) {ImGui::SetScrollX(window, state.currentScroll[axis]);}
            else {ImGui::SetScrollY(window, state.currentScroll[axis]);}
        }
    }
    for (auto it = runtime.windowStates.begin(); it != runtime.windowStates.end();) {
        if (!it->second.touchedThisFrame && it->first != runtime.activeWindowId) it = runtime.windowStates.erase(it);
        else ++it;
    }
}
#pragma endregion
