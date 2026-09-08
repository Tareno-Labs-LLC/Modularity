#include "../EditorLocalization.h"
#include "Engine.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <unordered_map>
#include <cstdlib>

#if defined(_WIN32)
#include <shellapi.h>
#endif

namespace Loc = Modularity::Loc;

namespace {
    using ScriptEditorLanguage = ScriptLanguageServiceLanguage;

    static uint64_t hashBuffer(const std::string& text) {
        uint64_t hash = 1469598103934665603ull;
        for (unsigned char c : text) {
            hash ^= static_cast<uint64_t>(c);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    static std::string toLowerCopy(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    static std::string normalizeScriptSelectionKey(const fs::path& path) {
        std::error_code ec;
        fs::path absPath = fs::absolute(path, ec);
        if (ec) absPath = path;
        std::string key = absPath.lexically_normal().string();
#if defined(_WIN32)
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
#endif
        return key;
    }

    static std::string extensionLower(const fs::path& path) {
        return toLowerCopy(path.extension().string());
    }

    static TextEditor::LanguageDefinition baseLanguageDefinition(ScriptEditorLanguage language) {
        switch (language) {
            case ScriptEditorLanguage::PlainText: {
                TextEditor::LanguageDefinition plainText;
                plainText.mName = "Plain Text";
                plainText.mAutoIndentation = false;
                return plainText;
            }
            case ScriptEditorLanguage::C:
                return TextEditor::LanguageDefinition::C();
            case ScriptEditorLanguage::GLSL:
                return TextEditor::LanguageDefinition::GLSL();
            case ScriptEditorLanguage::HLSL:
                return TextEditor::LanguageDefinition::HLSL();
            case ScriptEditorLanguage::Lua:
                return TextEditor::LanguageDefinition::Lua();
            case ScriptEditorLanguage::Mako: {
                TextEditor::LanguageDefinition mako = TextEditor::LanguageDefinition::CPlusPlus();
                mako.mName = "MAKO";
                mako.mKeywords = ScriptLanguageService::keywordsForLanguage(ScriptEditorLanguage::Mako);
                mako.mPreprocChar = '\0';
                mako.mSingleLineComment = "#";
                return mako;
            }
            case ScriptEditorLanguage::ModuCPP: {
                TextEditor::LanguageDefinition moducpp = TextEditor::LanguageDefinition::CPlusPlus();
                moducpp.mName = "ModuCPP";
                moducpp.mKeywords = ScriptLanguageService::keywordsForLanguage(ScriptEditorLanguage::ModuCPP);
                return moducpp;
            }
            case ScriptEditorLanguage::Cpp:
            default:
                return TextEditor::LanguageDefinition::CPlusPlus();
        }
    }

    static void initializeScriptEditor(TextEditor& editor) {
        auto palette = editor.GetPalette();
        palette[(int)TextEditor::PaletteIndex::KnownIdentifier] = IM_COL32(220, 180, 70, 255);
        palette[(int)TextEditor::PaletteIndex::Preprocessor] = IM_COL32(110, 170, 220, 255);
        palette[(int)TextEditor::PaletteIndex::PreprocIdentifier] = IM_COL32(230, 165, 80, 255);
        editor.SetPalette(palette);
        editor.SetShowWhitespaces(true);
        editor.SetAllowTabInput(false);
        editor.SetSmartTabDelete(true);
    }

    static std::string shellQuote(const fs::path& path) {
        std::string value = path.string();
#ifdef _WIN32
        std::string escaped;
        escaped.reserve(value.size() + 2);
        escaped.push_back('"');
        for (char c : value) {
            if (c == '"') escaped.push_back('\\');
            escaped.push_back(c);
        }
        escaped.push_back('"');
        return escaped;
#else
        std::string escaped = "'";
        for (char c : value) {
            if (c == '\'') escaped += "'\\''";
            else escaped.push_back(c);
        }
        escaped.push_back('\'');
        return escaped;
#endif
    }

    static fs::path findProjectRootForScript(const fs::path& scriptPath, const Project& currentProject) {
        std::error_code ec;
        if (currentProject.isLoaded && !currentProject.projectPath.empty() &&
            fs::exists(currentProject.projectPath / "project.modu", ec)) {
            return currentProject.projectPath.lexically_normal();
        }

        fs::path current = scriptPath;
        if (!fs::is_directory(current, ec)) {
            current = current.parent_path();
        }
        if (current.is_relative()) {
            current = fs::absolute(current, ec);
            if (ec) current = scriptPath.parent_path();
        }

        while (!current.empty()) {
            if (fs::exists(current / "project.modu", ec)) {
                return current.lexically_normal();
            }
            fs::path parent = current.parent_path();
            if (parent == current) break;
            current = parent;
        }
        return {};
    }

    static bool openScriptInVSCodeWorkspace(const fs::path& scriptPath, const fs::path& projectRoot) {
        if (projectRoot.empty()) return false;
#ifdef _WIN32
        const std::string command = "code -r " + shellQuote(projectRoot) +
                                    " -g " + shellQuote(scriptPath);
#elif __linux__
        const std::string command = "if command -v code >/dev/null 2>&1; then code -r " +
                                    shellQuote(projectRoot) + " -g " + shellQuote(scriptPath) +
                                    " >/dev/null 2>&1 & else exit 127; fi";
#else
        (void)scriptPath;
        (void)projectRoot;
        return false;
#endif
        return std::system(command.c_str()) == 0;
    }

    static bool openPathInDefaultEditor(const fs::path& path) {
#ifdef _WIN32
        std::wstring widePath = path.wstring();
        HINSTANCE result = ShellExecuteW(nullptr, L"open", widePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(result) > 32;
#elif __linux__
        std::string cmd = "xdg-open \"" + path.string() + "\"";
        return std::system(cmd.c_str()) == 0;
#else
        (void)path;
        return false;
#endif
    }

    static TextEditor::LanguageDefinition buildLanguageDefinition(ScriptEditorLanguage language,
                                                                  const std::vector<std::string>& functions,
                                                                  const std::vector<std::string>& defines) {
        TextEditor::LanguageDefinition lang = baseLanguageDefinition(language);
        std::unordered_set<std::string> defineSet(defines.begin(), defines.end());

        auto addIdentifier = [&](const std::string& name, const char* declaration) {
            if (name.empty()) return;
            TextEditor::Identifier id;
            id.mDeclaration = declaration;
            lang.mIdentifiers.insert({name, id});
        };

        if (language == ScriptEditorLanguage::Cpp || language == ScriptEditorLanguage::C ||
            language == ScriptEditorLanguage::Mako || language == ScriptEditorLanguage::ModuCPP) {
            addIdentifier("Begin", "Script callback");
            addIdentifier("TickUpdate", "Script callback");
            addIdentifier("Spec", "Script callback");
            addIdentifier("TestEditor", "Script callback");
            addIdentifier("Update", "Script callback");
            addIdentifier("ModuNode", "Preferred ModuCPP script base");
            addIdentifier("ModuBehaviour", "Legacy ModuCPP script base");
            addIdentifier("ModuCPP", "Core ModuCPP script API");
            addIdentifier("ModuEngine", "ModuCPP engine facade");
            addIdentifier("ModuInput", "ModuCPP input helpers");
            addIdentifier("ModuMAKO", "MAKO language integration for Modularity");
            addIdentifier("RMeshBuilder", "ModuCPP mesh-builder helpers");
            addIdentifier("ModuCPP.Experimental", "Advanced ModuCPP script helpers");
            addIdentifier("FPS", "Current frame FPS");
            addIdentifier("obj", "Current object facade");
            addIdentifier("UILabel", "Object UI label shorthand");
            addIdentifier("Start", "Float timer helper");
            addIdentifier("Ready", "Float timer helper");
            addIdentifier("IntRD", "Round down to whole number");
            addIdentifier("IntR", "Round to nearest whole number");
            addIdentifier("IntRU", "Round up to whole number");
        }

        for (const auto& name : functions) {
            if (defineSet.find(name) != defineSet.end()) continue;
            addIdentifier(name, "Function");
        }

        for (const auto& name : defines) {
            if (name.empty()) continue;
            TextEditor::Identifier id;
            id.mDeclaration = "Define";
            lang.mPreprocIdentifiers.insert({name, id});
        }

        return lang;
    }

    static ImVec2 popupPositionWithinBounds(const ImVec2& anchor,
                                            float popupWidth,
                                            float popupHeight,
                                            float lineHeight,
                                            const ImVec2& boundsMin,
                                            const ImVec2& boundsMax,
                                            bool* outFlippedUp = nullptr) {
        bool flipUp = false;
        float yBelow = anchor.y + lineHeight + 2.0f;
        float yAbove = anchor.y - popupHeight - 2.0f;
        if (yBelow + popupHeight > boundsMax.y && yAbove >= boundsMin.y) {
            flipUp = true;
        }
        float y = flipUp ? yAbove : yBelow;
        y = std::clamp(y, boundsMin.y, std::max(boundsMin.y, boundsMax.y - popupHeight));
        float x = std::clamp(anchor.x, boundsMin.x, std::max(boundsMin.x, boundsMax.x - popupWidth));
        if (outFlippedUp) {
            *outFlippedUp = flipUp;
        }
        return ImVec2(x, y);
    }

    static fs::path userNeovimConfigDir() {
#if defined(_WIN32)
        if (const char* localAppData = std::getenv("LOCALAPPDATA")) {
            if (*localAppData) return fs::path(localAppData) / "nvim";
        }
        if (const char* home = std::getenv("USERPROFILE")) {
            if (*home) return fs::path(home) / "AppData" / "Local" / "nvim";
        }
#else
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
            if (*xdg) return fs::path(xdg) / "nvim";
        }
        if (const char* home = std::getenv("HOME")) {
            if (*home) return fs::path(home) / ".config" / "nvim";
        }
#endif
        return {};
    }

    static bool fileContainsModuCppNvimMarker(const fs::path& path) {
        constexpr std::uintmax_t kMaxScanBytes = 1ull << 20; // 1 MiB
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(path, ec);
        if (ec || size == 0 || size > kMaxScanBytes) return false;

        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;
        std::string contents;
        contents.resize(static_cast<size_t>(size));
        in.read(contents.data(), static_cast<std::streamsize>(size));
        if (!in) contents.resize(static_cast<size_t>(in.gcount()));
        return contents.find("Mast3rM0ds/moducpp.nvim") != std::string::npos;
    }

    static bool scanNeovimConfigForModuCpp(const fs::path& configDir) {
        std::error_code ec;
        if (configDir.empty() || !fs::is_directory(configDir, ec)) return false;

        const fs::path initLua = configDir / "init.lua";
        if (fs::is_regular_file(initLua, ec) && fileContainsModuCppNvimMarker(initLua)) {
            return true;
        }

        const fs::path luaRoot = configDir / "lua";
        if (!fs::is_directory(luaRoot, ec)) return false;

        constexpr int kMaxFilesScanned = 512;
        int scanned = 0;
        fs::recursive_directory_iterator it(luaRoot,
            fs::directory_options::skip_permission_denied, ec);
        if (ec) return false;
        const fs::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it->is_symlink(ec)) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(ec)) continue;
            if (it->path().extension() != ".lua") continue;
            if (++scanned > kMaxFilesScanned) return false;
            if (fileContainsModuCppNvimMarker(it->path())) return true;
        }
        return false;
    }

}

void Engine::checkModuCppNvimUsage() {
    if (moduCppNvimWarningChecked) return;
    moduCppNvimWarningChecked = true;
    moduCppNvimWarningDetected = false;

    if (projectManager.moduCppNvimWarningDismissedV1) return;

    const fs::path configDir = userNeovimConfigDir();
    if (configDir.empty()) return;

    moduCppNvimWarningDetected = scanNeovimConfigForModuCpp(configDir);
}

void Engine::renderModuCppNvimWarningPopup() {
#if !MODULARITY_RUNTIME_ONLY && !defined(MODULARITY_PLAYER)
    if (requiresTermsOfServiceAcceptance()) return;
    if (showLauncher && !launcherIntroFinished) return;

    if (!moduCppNvimWarningChecked) {
        checkModuCppNvimUsage();
    }

    if (projectManager.moduCppNvimWarningDismissedV1) {
        moduCppNvimWarningPopupOpened = false;
        moduCppNvimRemovalStepsOpened = false;
        showModuCppNvimRemovalSteps = false;
        return;
    }

    if (!moduCppNvimWarningDetected) {
        moduCppNvimWarningPopupOpened = false;
        return;
    }

    constexpr const char* warningTitle = "Package Safety Warning";
    constexpr const char* removalTitle = "moducpp.nvim Removal Steps";

    if (!moduCppNvimWarningPopupOpened && !showModuCppNvimRemovalSteps) {
        ImGui::OpenPopup(warningTitle);
        moduCppNvimWarningPopupOpened = true;
        playEditorFeedbackOneShot("Resources/Sounds/Package Warning Chime.mp3",
                                  0.95f,
                                  EditorFeedbackSoundCategory::Other);
    }

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    const float popupWidth = ImClamp(displaySize.x * 0.46f, 440.0f, 620.0f);
    ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                            ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(popupWidth, 0.0f), ImGuiCond_Appearing);

    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::BeginPopupModal(warningTitle, nullptr, popupFlags)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + popupWidth - ImGui::GetStyle().WindowPadding.x * 2.0f);
        ImGui::TextWrapped("You're using moducpp.nvim, which has been marked Unsafe by the Modularity Team.");
        ImGui::Spacing();
        ImGui::TextWrapped("This package is potentially not safe or recommended due to maintainer trust concerns. It may still work, but use it at your own risk.");
        ImGui::Spacing();
        ImGui::TextWrapped("Are you sure you want to continue using it?");
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        constexpr float useAnywayWidth = 140.0f;
        constexpr float removalWidth = 200.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float totalWidth = useAnywayWidth + removalWidth + spacing;
        const float available = ImGui::GetContentRegionAvail().x;
        if (available > totalWidth) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - totalWidth) * 0.5f);
        }
        if (ImGui::Button("Use Anyway", ImVec2(useAnywayWidth, 0.0f))) {
            projectManager.moduCppNvimWarningDismissedV1 = true;
            projectManager.saveLauncherSettings();
            moduCppNvimWarningPopupOpened = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Show Removal Steps", ImVec2(removalWidth, 0.0f))) {
            showModuCppNvimRemovalSteps = true;
            moduCppNvimWarningPopupOpened = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (showModuCppNvimRemovalSteps) {
        if (!moduCppNvimRemovalStepsOpened) {
            ImGui::OpenPopup(removalTitle);
            moduCppNvimRemovalStepsOpened = true;
        }

        const float removalWidthPx = ImClamp(displaySize.x * 0.5f, 480.0f, 660.0f);
        ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                                ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(removalWidthPx, 0.0f), ImGuiCond_Appearing);

        if (ImGui::BeginPopupModal(removalTitle, nullptr, popupFlags)) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + removalWidthPx - ImGui::GetStyle().WindowPadding.x * 2.0f);
            ImGui::TextWrapped("To remove this package, open your Neovim plugin configuration and remove:");
            ImGui::Spacing();
            ImGui::TextUnformatted("\"Mast3rM0ds/moducpp.nvim\"");
            ImGui::Spacing();
            ImGui::TextWrapped("Common locations:");
            ImGui::BulletText("~/.config/nvim/init.lua");
            ImGui::BulletText("~/.config/nvim/lua/plugins/moducpp.lua");
            ImGui::BulletText("~/.config/nvim/lua/plugins/*.lua");
            ImGui::Spacing();
            ImGui::TextWrapped("Then restart Neovim.");
            ImGui::PopTextWrapPos();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            constexpr float closeWidth = 120.0f;
            const float availableClose = ImGui::GetContentRegionAvail().x;
            if (availableClose > closeWidth) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availableClose - closeWidth) * 0.5f);
            }
            if (ImGui::Button("Close", ImVec2(closeWidth, 0.0f))) {
                showModuCppNvimRemovalSteps = false;
                moduCppNvimRemovalStepsOpened = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
#endif
}

void Engine::refreshScriptingFileList() {
    scriptingFileList.clear();
    scriptingCompletions.clear();
    if (!projectManager.currentProject.isLoaded) {
        return;
    }

    fs::path configPath = resolveScriptsConfigPath(projectManager.currentProject);
    ScriptBuildConfig config;
    std::string error;
    const bool hasConfig = scriptCompiler.loadConfig(configPath, config, error);
    ScriptLanguageServiceProjectData projectData = ScriptLanguageService::scanProjectFiles(
        projectManager.currentProject.projectPath,
        projectManager.currentProject.assetsPath,
        hasConfig ? &config : nullptr);
    scriptingFileList = std::move(projectData.files);
    scriptingCompletions = std::move(projectData.projectSymbols);
}

void Engine::openScriptInEditor(const fs::path& path) {
    if (path.empty()) return;
    std::error_code ec;
    fs::path absPath = fs::absolute(path, ec);
    fs::path normalized = (ec ? path : absPath).lexically_normal();

    if (!hasScriptingWindowPackage()) {
        const fs::path projectRoot = findProjectRootForScript(normalized, projectManager.currentProject);
        if (!openScriptInVSCodeWorkspace(normalized, projectRoot) && !openPathInDefaultEditor(normalized)) {
            addConsoleMessage("Failed to open script in the system editor: " + normalized.string(),
                              ConsoleMessageType::Error);
        }
        return;
    }

    std::ifstream file(normalized);
    std::stringstream buffer;
    if (file.is_open()) {
        buffer << file.rdbuf();
    }
    scriptEditorState.filePath = normalized;
    scriptEditorState.buffer = buffer.str();
    if (!scriptTextEditorReady) {
        initializeScriptEditor(scriptTextEditor);
        scriptTextEditorReady = true;
    }

    scriptLanguageDocument = ScriptLanguageService::analyzeDocument(normalized, scriptEditorState.buffer);
    scriptTextEditor.SetLanguageDefinition(buildLanguageDefinition(
        scriptLanguageDocument.language,
        scriptLanguageDocument.functions,
        scriptLanguageDocument.defines));

    scriptTextEditor.SetText(scriptEditorState.buffer);
    scriptEditorState.dirty = false;
    scriptEditorState.hasWriteTime = false;
    if (fs::exists(normalized, ec)) {
        scriptEditorState.lastWriteTime = fs::last_write_time(normalized, ec);
        scriptEditorState.hasWriteTime = !ec;
    }
    showScriptingWindow = true;
}

void Engine::renderScriptingWindow() {
    if (!showScriptingWindow) return;
    if (!hasScriptingWindowPackage()) {
        showScriptingWindow = false;
        return;
    }

    bool listRefreshedThisFrame = false;
    if (scriptingFilesDirty) {
        refreshScriptingFileList();
        scriptingFilesDirty = false;
        listRefreshedThisFrame = true;
    }

    ImGui::Begin(Loc::Window("WINDOW_SCRIPTING", "Scripting"), &showScriptingWindow);
    if (!projectManager.currentProject.isLoaded) {
        ImGui::TextDisabled("Load a project to edit scripts.");
        ImGui::End();
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    static std::vector<std::string> symbols;
    static uint64_t symbolsHash = 0;
    static std::vector<std::string> bufferIdentifiers;
    static std::vector<std::string> bufferFunctions;
    static std::vector<std::string> bufferDefines;
    static std::unordered_map<std::string, std::string> functionSignatures;
    static uint64_t identifiersHash = 0;
    static fs::path identifiersFilePath;
    static ScriptLanguageServiceLanguage activeLanguage = ScriptLanguageServiceLanguage::Cpp;
    static std::vector<std::string> completionPool;
    static std::vector<std::string> activeSuggestions;
    static std::string activePrefix;
    static bool completionPoolDirty = true;
    static int selectedSuggestionIndex = 0;
    static bool completionManuallyDismissed = false;
    static std::string completionDismissPrefix;
    static bool completionPopupVisibleLastFrame = false;
    static fs::path reloadCheckPath;
    static bool cachedCanReload = false;
    static double lastReloadCheckTime = 0.0;
    static std::string cachedFilterLower;
    static std::vector<int> filteredScriptIndices;
    static std::unordered_set<std::string> selectedCompileScripts;
    static bool showFilePane = true;
    static bool showDetailsPane = true;
    static bool showDiagnosticsPane = true;
    static float filePaneWidth = 240.0f;
    static float detailsPaneWidth = 250.0f;
    static float diagnosticsPaneHeight = 160.0f;

    if (listRefreshedThisFrame) {
        completionPoolDirty = true;
    }

    ImGui::TextDisabled("Script & Shader Editor");
    ImGui::SameLine();
    if (ImGui::Button("Refresh List")) {
        scriptingFilesDirty = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Files", &showFilePane);
    ImGui::SameLine();
    ImGui::Checkbox("Details", &showDetailsPane);
    ImGui::SameLine();
    ImGui::Checkbox("Diagnostics", &showDiagnosticsPane);

    ImGui::Separator();

    const float topSplitThickness = 4.0f;
    if (showFilePane) {
        float availWidth = ImGui::GetContentRegionAvail().x;
        const float minFilePane = 180.0f;
        const float minMainPane = 380.0f;
        const float maxFilePane = std::max(minFilePane, availWidth - minMainPane - topSplitThickness);
        filePaneWidth = std::clamp(filePaneWidth, minFilePane, maxFilePane);

        ImGui::BeginChild("ScriptingFiles", ImVec2(filePaneWidth, 0.0f), true);
        ImGui::TextDisabled("Scripts / Shaders");
        bool filterChanged = ImGui::InputTextWithHint("##ScriptFilter", "Filter", scriptingFilter, sizeof(scriptingFilter));
        ImGui::Separator();

        std::string filterLower = toLowerCopy(scriptingFilter);
        if (listRefreshedThisFrame || filterChanged || filterLower != cachedFilterLower) {
            cachedFilterLower = filterLower;
            filteredScriptIndices.clear();
            filteredScriptIndices.reserve(scriptingFileList.size());
            for (size_t i = 0; i < scriptingFileList.size(); ++i) {
                if (!cachedFilterLower.empty()) {
                    std::string labelLower = toLowerCopy(scriptingFileList[i].filename().string());
                    if (labelLower.find(cachedFilterLower) == std::string::npos) {
                        continue;
                    }
                }
                filteredScriptIndices.push_back(static_cast<int>(i));
            }
            std::unordered_set<std::string> visibleKeys;
            visibleKeys.reserve(scriptingFileList.size());
            for (const auto& scriptPath : scriptingFileList) {
                visibleKeys.insert(normalizeScriptSelectionKey(scriptPath));
            }
            for (auto it = selectedCompileScripts.begin(); it != selectedCompileScripts.end(); ) {
                if (visibleKeys.find(*it) == visibleKeys.end()) {
                    it = selectedCompileScripts.erase(it);
                } else {
                    ++it;
                }
            }
        }

        int selectedBatchCount = static_cast<int>(selectedCompileScripts.size());
        ImGui::TextDisabled("Batch Selection: %d", selectedBatchCount);
        ImGui::SameLine();
        ImGui::BeginDisabled(selectedBatchCount == 0 || compileInProgress);
        if (ImGui::Button("Compile Selected")) {
            std::vector<fs::path> batch;
            batch.reserve(selectedCompileScripts.size());
            for (const auto& scriptPath : scriptingFileList) {
                const std::string key = normalizeScriptSelectionKey(scriptPath);
                if (selectedCompileScripts.find(key) == selectedCompileScripts.end()) {
                    continue;
                }
                const std::string ext = extensionLower(scriptPath);
                if (ext == ".cs" || ext == ".csproj") {
                    continue;
                }
                batch.push_back(scriptPath);
            }
            if (!batch.empty()) {
                queueScriptCompileBatch(batch);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear Selection")) {
            selectedCompileScripts.clear();
        }

        ImGui::Spacing();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(filteredScriptIndices.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const fs::path& scriptPath = scriptingFileList[filteredScriptIndices[row]];
                std::string label = scriptPath.filename().string();
                const std::string key = normalizeScriptSelectionKey(scriptPath);
                const bool selected = (scriptEditorState.filePath == scriptPath);
                const std::string ext = extensionLower(scriptPath);
                const bool batchCapable = ext != ".cs" && ext != ".csproj";

                ImGui::PushID(filteredScriptIndices[row]);
                bool compileSelected = selectedCompileScripts.find(key) != selectedCompileScripts.end();
                if (!batchCapable) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Checkbox("##BatchSelect", &compileSelected)) {
                    if (compileSelected) {
                        selectedCompileScripts.insert(key);
                    } else {
                        selectedCompileScripts.erase(key);
                    }
                }
                if (!batchCapable) {
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();
                ImGuiSelectableFlags rowFlags = ImGuiSelectableFlags_SpanAvailWidth;
                if (ImGui::Selectable(label.c_str(), selected, rowFlags)) {
                    openScriptInEditor(scriptPath);
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    if (batchCapable) {
                        if (compileSelected) {
                            selectedCompileScripts.erase(key);
                        } else {
                            selectedCompileScripts.insert(key);
                        }
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();
        float splitterHeight = ImGui::GetContentRegionAvail().y;
        if (splitterHeight < 1.0f) splitterHeight = 1.0f;
        ImGui::InvisibleButton("ScriptingFilePaneSplitter", ImVec2(topSplitThickness, splitterHeight));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (ImGui::IsItemActive()) {
            filePaneWidth += io.MouseDelta.x;
            filePaneWidth = std::clamp(filePaneWidth, minFilePane, maxFilePane);
        }
        ImGui::SameLine();
    }

    ImGui::BeginChild("ScriptingEditor", ImVec2(0.0f, 0.0f), false);
    ImGui::TextDisabled("Active File");
    ImGui::SameLine();
    std::string fileLabel = scriptEditorState.filePath.empty()
        ? std::string("None")
        : scriptEditorState.filePath.filename().string();
    ImGui::TextUnformatted(fileLabel.c_str());

    bool hasFile = !scriptEditorState.filePath.empty();
    bool canCompileFile = hasFile && ScriptLanguageService::isCompilableScriptPath(scriptEditorState.filePath);
    ImGui::SameLine();
    if (!hasFile) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Save")) {
        std::ofstream out(scriptEditorState.filePath);
        if (out.is_open()) {
            scriptEditorState.buffer = scriptTextEditor.GetText();
            out << scriptEditorState.buffer;
            scriptEditorState.dirty = false;
            std::error_code ec;
            scriptEditorState.lastWriteTime = fs::last_write_time(scriptEditorState.filePath, ec);
            scriptEditorState.hasWriteTime = !ec;
            cachedCanReload = false;
            lastReloadCheckTime = glfwGetTime();
            if (scriptEditorState.autoCompileOnSave && canCompileFile) {
                compileScriptFile(scriptEditorState.filePath);
            }
        }
    }
    ImGui::SameLine();
    if (!canCompileFile) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Compile")) {
        compileScriptFile(scriptEditorState.filePath);
    }
    if (!canCompileFile) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!canCompileFile) {
        ImGui::BeginDisabled();
    }
    ImGui::Checkbox("Auto-compile on save", &scriptEditorState.autoCompileOnSave);
    if (!canCompileFile) {
        ImGui::EndDisabled();
    }
    if (!hasFile) {
        ImGui::EndDisabled();
    }

    bool canReload = false;
    if (reloadCheckPath != scriptEditorState.filePath) {
        reloadCheckPath = scriptEditorState.filePath;
        cachedCanReload = false;
        lastReloadCheckTime = 0.0;
    }
    if (hasFile && scriptEditorState.hasWriteTime) {
        const double now = glfwGetTime();
        if (now - lastReloadCheckTime >= 0.25) {
            lastReloadCheckTime = now;
            cachedCanReload = false;
            std::error_code ec;
            if (fs::exists(scriptEditorState.filePath, ec)) {
                auto diskTime = fs::last_write_time(scriptEditorState.filePath, ec);
                if (!ec && diskTime > scriptEditorState.lastWriteTime) {
                    cachedCanReload = true;
                }
            }
        }
        canReload = cachedCanReload;
    }
    if (canReload) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "File changed on disk");
        ImGui::SameLine();
        if (ImGui::Button("Reload")) {
            openScriptInEditor(scriptEditorState.filePath);
            cachedCanReload = false;
            lastReloadCheckTime = glfwGetTime();
        }
    }

    ImGui::Separator();

    if (ImGui::BeginTabBar("ScriptingTabs")) {
        if (ImGui::BeginTabItem("Editor")) {
            if (hasFile) {
                if (!scriptTextEditorReady) {
                    initializeScriptEditor(scriptTextEditor);
                    scriptTextEditorReady = true;
                }

                ScriptLanguageServiceLanguage language = ScriptLanguageService::detectLanguage(scriptEditorState.filePath);
                uint64_t bufferHash = hashBuffer(scriptEditorState.buffer);
                bool fileChanged = (identifiersFilePath != scriptEditorState.filePath);
                bool languageChanged = (activeLanguage != language);
                if (bufferHash != identifiersHash || fileChanged || languageChanged) {
                    identifiersHash = bufferHash;
                    identifiersFilePath = scriptEditorState.filePath;
                    activeLanguage = language;

                    scriptLanguageDocument = ScriptLanguageService::analyzeDocument(
                        scriptEditorState.filePath,
                        scriptEditorState.buffer);
                    bufferIdentifiers = scriptLanguageDocument.identifiers;
                    bufferFunctions = scriptLanguageDocument.functions;
                    bufferDefines = scriptLanguageDocument.defines;
                    functionSignatures = scriptLanguageDocument.functionSignatures;
                    if (language == ScriptEditorLanguage::Cpp || language == ScriptEditorLanguage::C) {
                        functionSignatures["Begin"] = "void Begin()";
                        functionSignatures["TickUpdate"] = "void TickUpdate(float deltaTime)";
                        functionSignatures["Spec"] = "void Spec()";
                        functionSignatures["TestEditor"] = "void TestEditor()";
                        functionSignatures["Update"] = "void Update(float deltaTime)";
                    }
                    scriptTextEditor.SetLanguageDefinition(buildLanguageDefinition(
                        scriptLanguageDocument.language,
                        bufferFunctions,
                        bufferDefines));
                    completionPoolDirty = true;
                }

                auto rebuildCompletionPool = [&]() -> bool {
                    if (!completionPoolDirty) return false;
                    completionPool.clear();
                    std::unordered_set<std::string> poolSet;
                    const auto& langDef = scriptTextEditor.GetLanguageDefinition();
                    for (const auto& kw : langDef.mKeywords) poolSet.insert(kw);
                    for (const auto& identifier : langDef.mIdentifiers) poolSet.insert(identifier.first);
                    for (const auto& identifier : langDef.mPreprocIdentifiers) poolSet.insert(identifier.first);
                    for (const auto& entry : scriptingCompletions) poolSet.insert(entry);
                    for (const auto& entry : symbols) poolSet.insert(entry);
                    for (const auto& entry : bufferIdentifiers) poolSet.insert(entry);
                    for (const auto& entry : bufferFunctions) poolSet.insert(entry);
                    for (const auto& entry : bufferDefines) poolSet.insert(entry);
                    for (const auto& entry : scriptLanguageDocument.variables) poolSet.insert(entry);
                    for (const auto& entry : scriptLanguageDocument.moducppImports) poolSet.insert(entry);
                    for (const auto& entry : scriptLanguageDocument.moducppInspectorFields) poolSet.insert(entry);
                    for (const auto& entry : scriptLanguageDocument.moducppCompletions) poolSet.insert(entry);
                    completionPool.assign(poolSet.begin(), poolSet.end());
                    std::sort(completionPool.begin(), completionPool.end());
                    completionPoolDirty = false;
                    return true;
                };

                struct ActiveTokenInfo {
                    bool valid = false;
                    TextEditor::Coordinates cursor;
                    TextEditor::Coordinates start;
                    TextEditor::Coordinates end;
                    std::string word;
                    std::string prefix;
                };

                auto activeToken = [&]() -> ActiveTokenInfo {
                    ActiveTokenInfo token;
                    token.cursor = scriptTextEditor.GetCursorPosition();
                    TextEditor::Coordinates probe = token.cursor;
                    token.word = scriptTextEditor.GetWordAtPublic(probe);
                    if (token.word.empty() && token.cursor.mColumn > 0) {
                        probe = TextEditor::Coordinates(token.cursor.mLine, token.cursor.mColumn - 1);
                        token.word = scriptTextEditor.GetWordAtPublic(probe);
                    }
                    if (token.word.empty()) return token;
                    token.start = scriptTextEditor.FindWordStartPublic(probe);
                    token.end = scriptTextEditor.FindWordEndPublic(probe);
                    int prefixLength = std::clamp(token.cursor.mColumn - token.start.mColumn, 0, static_cast<int>(token.word.size()));
                    token.prefix = token.word.substr(0, static_cast<size_t>(prefixLength));
                    token.valid = true;
                    return token;
                };

                auto updateSuggestions = [&](const std::string& prefix) {
                    bool prefixChanged = (prefix != activePrefix);
                    activePrefix = prefix;
                    if (prefixChanged) {
                        selectedSuggestionIndex = 0;
                    }
                    if (!completionDismissPrefix.empty() && activePrefix != completionDismissPrefix) {
                        completionManuallyDismissed = false;
                    }
                    if (!activePrefix.empty()) {
                        activeSuggestions = ScriptLanguageService::buildCompletionList(completionPool, activePrefix);
                    } else {
                        activeSuggestions.clear();
                    }
                    if (activeSuggestions.empty()) {
                        selectedSuggestionIndex = 0;
                    } else {
                        selectedSuggestionIndex = std::clamp(selectedSuggestionIndex, 0, static_cast<int>(activeSuggestions.size()) - 1);
                    }
                };

                auto applySuggestion = [&](const std::string& suggestion) {
                    ActiveTokenInfo token = activeToken();
                    TextEditor::Coordinates cursor = scriptTextEditor.GetCursorPosition();
                    TextEditor::Coordinates replaceStart = token.valid
                        ? token.start
                        : TextEditor::Coordinates(cursor.mLine, std::max(0, cursor.mColumn - static_cast<int>(activePrefix.size())));
                    TextEditor::Coordinates replaceEnd = token.valid ? token.end : cursor;
                    scriptTextEditor.SetSelection(replaceStart, replaceEnd);
                    scriptTextEditor.Delete();
                    scriptTextEditor.InsertText(suggestion.c_str());
                    scriptEditorState.dirty = true;
                    scriptEditorState.buffer = scriptTextEditor.GetText();
                    completionPoolDirty = true;
                    completionManuallyDismissed = true;
                    completionDismissPrefix = suggestion;
                };

                bool poolRebuiltBeforeRender = rebuildCompletionPool();
                ActiveTokenInfo tokenBeforeRender = activeToken();
                if (poolRebuiltBeforeRender || tokenBeforeRender.prefix != activePrefix) {
                    updateSuggestions(tokenBeforeRender.prefix);
                }
                bool interceptCompletionKeys = !activeSuggestions.empty() && !activePrefix.empty() && !completionManuallyDismissed;
                scriptTextEditor.SetAllowTabInput(!interceptCompletionKeys);
                scriptTextEditor.SetAllowArrowNavigation(!interceptCompletionKeys);
                scriptTextEditor.SetAllowEnterInput(!interceptCompletionKeys);

                const float paneSplitter = 4.0f;
                const float minEditorHeight = 120.0f;
                float availHeight = ImGui::GetContentRegionAvail().y;
                if (showDiagnosticsPane) {
                    diagnosticsPaneHeight = std::clamp(diagnosticsPaneHeight, 100.0f, std::max(100.0f, availHeight - minEditorHeight - paneSplitter));
                }
                float editorRowHeight = showDiagnosticsPane
                    ? std::max(minEditorHeight, availHeight - diagnosticsPaneHeight - paneSplitter)
                    : availHeight;

                ImVec2 editorRectMin(0.0f, 0.0f);
                ImVec2 editorRectMax(0.0f, 0.0f);
                bool hasEditorRect = false;

                ImGui::BeginChild("ScriptEditorRow", ImVec2(0.0f, editorRowHeight), false);
                float rowAvailWidth = ImGui::GetContentRegionAvail().x;
                const float minEditorWidth = 220.0f;
                const float minDetailsWidth = 170.0f;
                if (showDetailsPane) {
                    float maxDetailsWidth = std::max(minDetailsWidth, rowAvailWidth - minEditorWidth - paneSplitter);
                    detailsPaneWidth = std::clamp(detailsPaneWidth, minDetailsWidth, maxDetailsWidth);
                }
                float editorPaneWidth = showDetailsPane
                    ? std::max(minEditorWidth, rowAvailWidth - detailsPaneWidth - paneSplitter)
                    : rowAvailWidth;

                ImGui::BeginChild("ScriptEditorPane", ImVec2(editorPaneWidth, 0.0f), true);
                scriptTextEditor.Render("##ScriptEditor", ImVec2(0.0f, 0.0f), false);
                editorRectMin = ImGui::GetItemRectMin();
                editorRectMax = ImGui::GetItemRectMax();
                hasEditorRect = true;
                if (scriptTextEditor.IsTextChanged()) {
                    scriptEditorState.dirty = true;
                    scriptEditorState.buffer = scriptTextEditor.GetText();
                }
                ImGui::EndChild();

                if (showDetailsPane) {
                    ImGui::SameLine();
                    float detailsSplitterHeight = ImGui::GetContentRegionAvail().y;
                    if (detailsSplitterHeight < 1.0f) detailsSplitterHeight = 1.0f;
                    ImGui::InvisibleButton("ScriptingDetailsSplitter", ImVec2(paneSplitter, detailsSplitterHeight));
                    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    }
                    if (ImGui::IsItemActive()) {
                        detailsPaneWidth -= io.MouseDelta.x;
                        float maxDetailsWidth = std::max(minDetailsWidth, rowAvailWidth - minEditorWidth - paneSplitter);
                        detailsPaneWidth = std::clamp(detailsPaneWidth, minDetailsWidth, maxDetailsWidth);
                    }
                    ImGui::SameLine();
                    ImGui::BeginChild("ScriptDetailsPane", ImVec2(0.0f, 0.0f), true);
                    ImGui::TextDisabled("Symbols");
                    if (!symbols.empty()) {
                        ImGuiListClipper symbolClipper;
                        symbolClipper.Begin(static_cast<int>(symbols.size()));
                        while (symbolClipper.Step()) {
                            for (int i = symbolClipper.DisplayStart; i < symbolClipper.DisplayEnd; ++i) {
                                ImGui::BulletText("%s", symbols[static_cast<size_t>(i)].c_str());
                            }
                        }
                    } else {
                        ImGui::TextDisabled("No symbols detected.");
                    }
                    ImGui::Separator();
                    ImGui::TextDisabled("IntelliSense");
                    ImGui::Text("Pool: %d", static_cast<int>(completionPool.size()));
                    ImGui::Text("Prefix: %s", activePrefix.empty() ? "-" : activePrefix.c_str());
                    ImGui::Text("Suggestions: %d", static_cast<int>(activeSuggestions.size()));
                    ImGui::EndChild();
                }
                ImGui::EndChild();

                if (showDiagnosticsPane) {
                    float splitterWidth = ImGui::GetContentRegionAvail().x;
                    if (splitterWidth < 1.0f) splitterWidth = 1.0f;
                    ImGui::InvisibleButton("ScriptingDiagnosticsSplitter", ImVec2(splitterWidth, paneSplitter));
                    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                    }
                    if (ImGui::IsItemActive()) {
                        diagnosticsPaneHeight -= io.MouseDelta.y;
                        diagnosticsPaneHeight = std::clamp(diagnosticsPaneHeight, 100.0f,
                            std::max(100.0f, availHeight - minEditorHeight - paneSplitter));
                    }

                    ImGui::BeginChild("ScriptDiagnosticsPane", ImVec2(0.0f, diagnosticsPaneHeight), true);
                    if (ImGui::BeginTabBar("ScriptDiagnosticsTabs")) {
                        if (ImGui::BeginTabItem("Diagnostics")) {
                            int errorCount = 0;
                            int warningCount = 0;
                            int infoCount = 0;
                            for (const auto& diagnostic : lastCompileDiagnostics) {
                                if (diagnostic.severity == Modularity::ScriptDiagnosticSeverity::Error) {
                                    ++errorCount;
                                } else if (diagnostic.severity == Modularity::ScriptDiagnosticSeverity::Warning) {
                                    ++warningCount;
                                } else {
                                    ++infoCount;
                                }
                            }

                            ImGui::Text("Status: %s", lastCompileStatus.empty() ? "Idle" : lastCompileStatus.c_str());
                            ImGui::Text("Errors: %d  Warnings: %d  Info: %d", errorCount, warningCount, infoCount);
                            ImGui::Separator();
                            if (lastCompileDiagnostics.empty()) {
                                ImGui::TextDisabled("No diagnostics.");
                            } else {
                                for (size_t index = 0; index < lastCompileDiagnostics.size(); ++index) {
                                    const auto& diagnostic = lastCompileDiagnostics[index];
                                    const ImVec4 color =
                                        diagnostic.severity == Modularity::ScriptDiagnosticSeverity::Error
                                            ? ImVec4(0.94f, 0.45f, 0.45f, 1.0f)
                                            : diagnostic.severity == Modularity::ScriptDiagnosticSeverity::Warning
                                                ? ImVec4(0.95f, 0.79f, 0.40f, 1.0f)
                                                : ImVec4(0.55f, 0.78f, 0.95f, 1.0f);

                                    ImGui::PushID(static_cast<int>(index));
                                    ImGui::TextColored(
                                        color,
                                        "[%s][%s] %s",
                                        Modularity::scriptDiagnosticSeverityToString(diagnostic.severity),
                                        diagnostic.code.c_str(),
                                        diagnostic.message.c_str());
                                    if (!diagnostic.hint.empty()) {
                                        ImGui::TextWrapped("Hint: %s", diagnostic.hint.c_str());
                                    }
                                    if (!diagnostic.source.empty()) {
                                        ImGui::TextDisabled("Source: %s", diagnostic.source.c_str());
                                    }
                                    if (diagnostic.line > 0) {
                                        ImGui::TextDisabled("Line: %d", diagnostic.line);
                                    }
                                    if (!diagnostic.rawDetails.empty() && ImGui::TreeNode("Raw Details")) {
                                        ImGui::TextDisabled("Origin: %s", Modularity::scriptDiagnosticOriginToString(diagnostic.origin));
                                        if (diagnostic.column > 0) {
                                            ImGui::TextDisabled("Column: %d", diagnostic.column);
                                        }
                                        if (!diagnostic.sourceLine.empty()) {
                                            ImGui::TextWrapped("Code: %s", diagnostic.sourceLine.c_str());
                                        }
                                        ImGui::Separator();
                                        ImGui::TextWrapped("%s", diagnostic.rawDetails.c_str());
                                        ImGui::TreePop();
                                    }
                                    if (index + 1 < lastCompileDiagnostics.size()) {
                                        ImGui::Separator();
                                    }
                                    ImGui::PopID();
                                }
                            }
                            ImGui::EndTabItem();
                        }
                        if (ImGui::BeginTabItem("Output")) {
                            if (lastCompileLog.empty()) {
                                ImGui::TextDisabled("No build output.");
                            } else {
                                ImGui::BeginChild("ScriptOutputLog", ImVec2(0.0f, 0.0f), false,
                                    ImGuiWindowFlags_HorizontalScrollbar);
                                ImGui::TextUnformatted(lastCompileLog.c_str());
                                ImGui::EndChild();
                            }
                            ImGui::EndTabItem();
                        }
                        ImGui::EndTabBar();
                    }
                    ImGui::EndChild();
                }

                uint64_t newHash = hashBuffer(scriptEditorState.buffer);
                if (newHash != symbolsHash) {
                    symbolsHash = newHash;
                    scriptLanguageDocument = ScriptLanguageService::analyzeDocument(
                        scriptEditorState.filePath,
                        scriptEditorState.buffer);
                    bufferIdentifiers = scriptLanguageDocument.identifiers;
                    bufferFunctions = scriptLanguageDocument.functions;
                    bufferDefines = scriptLanguageDocument.defines;
                    functionSignatures = scriptLanguageDocument.functionSignatures;
                    if (scriptLanguageDocument.language == ScriptEditorLanguage::Cpp ||
                        scriptLanguageDocument.language == ScriptEditorLanguage::C) {
                        functionSignatures["Begin"] = "void Begin()";
                        functionSignatures["TickUpdate"] = "void TickUpdate(float deltaTime)";
                        functionSignatures["Spec"] = "void Spec()";
                        functionSignatures["TestEditor"] = "void TestEditor()";
                        functionSignatures["Update"] = "void Update(float deltaTime)";
                    }
                    symbols = scriptLanguageDocument.symbols;
                    scriptTextEditor.SetLanguageDefinition(buildLanguageDefinition(
                        scriptLanguageDocument.language,
                        bufferFunctions,
                        bufferDefines));
                    completionPoolDirty = true;
                }
                bool poolRebuiltAfterRender = rebuildCompletionPool();
                ActiveTokenInfo tokenAfterRender = activeToken();
                if (poolRebuiltAfterRender || tokenAfterRender.prefix != activePrefix) {
                    updateSuggestions(tokenAfterRender.prefix);
                }

                bool editorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
                if (editorFocused && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                    completionManuallyDismissed = false;
                    completionDismissPrefix.clear();
                }

                bool completionVisible = editorFocused &&
                    hasEditorRect &&
                    scriptTextEditor.HasCursorScreenPosition() &&
                    !activeSuggestions.empty() &&
                    !activePrefix.empty() &&
                    !completionManuallyDismissed;

                if (completionVisible) {
                    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
                        if (selectedSuggestionIndex <= 0) {
                            selectedSuggestionIndex = static_cast<int>(activeSuggestions.size()) - 1;
                        } else {
                            --selectedSuggestionIndex;
                        }
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) {
                        selectedSuggestionIndex = (selectedSuggestionIndex + 1) % static_cast<int>(activeSuggestions.size());
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                        completionManuallyDismissed = true;
                        completionDismissPrefix = activePrefix;
                        completionVisible = false;
                    } else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
                        const std::string accepted = activeSuggestions[static_cast<size_t>(selectedSuggestionIndex)];
                        applySuggestion(accepted);
                        completionVisible = false;
                    }
                }

                bool completionFlippedUp = false;
                ImVec2 caretPos = scriptTextEditor.GetCursorScreenPositionPublic();
                float lineHeight = ImGui::GetTextLineHeightWithSpacing();
                if (completionVisible) {
                    bool popupJustOpened = !completionPopupVisibleLastFrame;
                    const int visibleRows = std::min(10, static_cast<int>(activeSuggestions.size()));
                    const float popupWidth = 340.0f;
                    const float popupHeight = lineHeight * (visibleRows + 1.9f);
                    ImVec2 popupPos = popupPositionWithinBounds(caretPos, popupWidth, popupHeight, lineHeight,
                        editorRectMin, editorRectMax, &completionFlippedUp);
                    ImGui::SetNextWindowPos(popupPos, ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(popupWidth, popupHeight), ImGuiCond_Always);
                    ImGuiWindowFlags popupFlags =
                        ImGuiWindowFlags_NoDocking |
                        ImGuiWindowFlags_NoTitleBar |
                        ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoFocusOnAppearing;
                    if (ImGui::Begin("##ScriptCompletionPopup", nullptr, popupFlags)) {
                        ImGui::TextDisabled("Autocomplete");
                        ImGui::Separator();
                        ImGui::BeginChild("##ScriptCompletionList", ImVec2(0.0f, 0.0f), false,
                            ImGuiWindowFlags_AlwaysVerticalScrollbar);
                        for (size_t i = 0; i < activeSuggestions.size(); ++i) {
                            bool selected = (static_cast<int>(i) == selectedSuggestionIndex);
                            if (ImGui::Selectable(activeSuggestions[i].c_str(), selected)) {
                                selectedSuggestionIndex = static_cast<int>(i);
                                applySuggestion(activeSuggestions[i]);
                                completionVisible = false;
                                break;
                            }
                            if (selected && popupJustOpened) {
                                ImGui::SetScrollHereY(0.5f);
                            }
                        }
                        ImGui::EndChild();
                    }
                    ImGui::End();
                }
                completionPopupVisibleLastFrame = completionVisible;

                if (editorFocused && hasEditorRect && scriptTextEditor.HasCursorScreenPosition()) {
                    std::string signatureLabel;
                    ScriptLanguageServiceFunctionCallContext callCtx = ScriptLanguageService::detectFunctionCallContext(
                        scriptTextEditor.GetCurrentLineText(),
                        scriptTextEditor.GetCursorPosition().mColumn);
                    if (callCtx.valid) {
                        auto tryFindSignature = [&](const std::string& name) -> std::string {
                            auto it = functionSignatures.find(name);
                            if (it != functionSignatures.end()) return it->second;
                            size_t scope = name.rfind("::");
                            if (scope != std::string::npos && scope + 2 < name.size()) {
                                std::string shortName = name.substr(scope + 2);
                                it = functionSignatures.find(shortName);
                                if (it != functionSignatures.end()) return it->second;
                            }
                            return {};
                        };
                        signatureLabel = tryFindSignature(callCtx.functionName);
                        if (signatureLabel.empty()) {
                            signatureLabel = callCtx.functionName + "(...)";
                        }

                        std::vector<std::string> params = ScriptLanguageService::splitSignatureParameters(signatureLabel);
                        const float sigWidth = 420.0f;
                        const float sigHeight = params.empty()
                            ? lineHeight * 2.2f
                            : lineHeight * std::clamp(static_cast<float>(params.size() + 2), 3.0f, 8.0f);

                        ImVec2 sigPos;
                        if (completionVisible && !completionFlippedUp) {
                            sigPos = ImVec2(caretPos.x, caretPos.y - sigHeight - 6.0f);
                            sigPos.x = std::clamp(sigPos.x, editorRectMin.x, std::max(editorRectMin.x, editorRectMax.x - sigWidth));
                            sigPos.y = std::clamp(sigPos.y, editorRectMin.y, std::max(editorRectMin.y, editorRectMax.y - sigHeight));
                        } else {
                            sigPos = popupPositionWithinBounds(caretPos, sigWidth, sigHeight, lineHeight,
                                editorRectMin, editorRectMax, nullptr);
                        }

                        ImGui::SetNextWindowPos(sigPos, ImGuiCond_Always);
                        ImGui::SetNextWindowSize(ImVec2(sigWidth, sigHeight), ImGuiCond_Always);
                        ImGuiWindowFlags sigFlags =
                            ImGuiWindowFlags_NoDocking |
                            ImGuiWindowFlags_NoTitleBar |
                            ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoMove |
                            ImGuiWindowFlags_NoSavedSettings |
                            ImGuiWindowFlags_NoFocusOnAppearing;
                        if (ImGui::Begin("##ScriptSignaturePopup", nullptr, sigFlags)) {
                            ImGui::TextUnformatted(signatureLabel.c_str());
                            if (!params.empty()) {
                                ImGui::Separator();
                                const int highlighted = std::clamp(callCtx.activeParameter, 0, static_cast<int>(params.size()) - 1);
                                for (size_t i = 0; i < params.size(); ++i) {
                                    bool isActive = static_cast<int>(i) == highlighted;
                                    ImVec4 color = isActive
                                        ? ImVec4(0.96f, 0.84f, 0.40f, 1.0f)
                                        : ImVec4(0.75f, 0.75f, 0.78f, 1.0f);
                                    ImGui::TextColored(color, "%s%s", isActive ? "> " : "  ", params[i].c_str());
                                }
                            }
                        }
                        ImGui::End();
                    }
                }

                if (completionVisible && scriptTextEditor.HasCursorScreenPosition() && !activeSuggestions.empty()) {
                    const std::string& suggestion = activeSuggestions[static_cast<size_t>(selectedSuggestionIndex)];
                    if (suggestion.size() > activePrefix.size() && suggestion.rfind(activePrefix, 0) == 0) {
                        std::string ghost = suggestion.substr(activePrefix.size());
                        ImU32 ghostColor = IM_COL32(180, 180, 180, 110);
                        ImGui::GetForegroundDrawList()->AddText(scriptTextEditor.GetCursorScreenPositionPublic(),
                            ghostColor, ghost.c_str());
                    }
                }
            } else {
                scriptTextEditor.SetAllowTabInput(true);
                scriptTextEditor.SetAllowArrowNavigation(true);
                scriptTextEditor.SetAllowEnterInput(true);
                ImGui::TextDisabled("Select a script or shader file to start editing.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Intellisense")) {
            ScriptBuildConfig config;
            std::string error;
            fs::path configPath = resolveScriptsConfigPath(projectManager.currentProject);
            bool hasConfig = scriptCompiler.loadConfig(configPath, config, error);
            if (hasConfig) {
                packageManager.applyToBuildConfig(config);
                ImGui::TextDisabled("Compiler");
                ImGui::Text("Standard: %s", config.cppStandard.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("Include Dirs");
                for (const auto& includeDir : config.includeDirs) {
                    ImGui::BulletText("%s", includeDir.string().c_str());
                }
                ImGui::Separator();
                ImGui::TextDisabled("Defines");
                for (const auto& def : config.defines) {
                    ImGui::BulletText("%s", def.c_str());
                }
            } else {
                ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.55f, 1.0f), "Scripts.modu not loaded");
                if (!error.empty()) {
                    ImGui::TextWrapped("%s", error.c_str());
                }
            }

            ImGui::Separator();
            ImGui::TextDisabled("Outline");
            if (!symbols.empty()) {
                for (const auto& symbol : symbols) {
                    ImGui::BulletText("%s", symbol.c_str());
                }
            } else {
                ImGui::TextDisabled("No symbols detected.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Build")) {
            ImGui::TextDisabled("Compile Status");
            ImGui::Text("%s", lastCompileStatus.c_str());
            if (!lastCompileLog.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Output");
                ImGui::BeginChild("CompileLog", ImVec2(0.0f, 0.0f), true);
                ImGui::TextUnformatted(lastCompileLog.c_str());
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::EndChild();
    ImGui::End();
}
