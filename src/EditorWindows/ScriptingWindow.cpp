#include "../EditorLocalization.h"
#include "Engine.h"
#include "ModuCPPTranspiler.h"
#include "imgui_internal.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <unordered_set>
#include <cstdlib>

#if defined(_WIN32)
#include <shellapi.h>
#endif

namespace Loc = Modularity::Loc;

namespace {
    using ScriptEditorLanguage = ScriptLanguageServiceLanguage;

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
        palette[(int)TextEditor::PaletteIndex::Background] = IM_COL32(13, 13, 14, 255);
        palette[(int)TextEditor::PaletteIndex::Keyword] = IM_COL32(102, 180, 226, 255);
        palette[(int)TextEditor::PaletteIndex::String] = IM_COL32(238, 123, 125, 255);
        palette[(int)TextEditor::PaletteIndex::KnownIdentifier] = IM_COL32(220, 180, 70, 255);
        palette[(int)TextEditor::PaletteIndex::Preprocessor] = IM_COL32(110, 170, 220, 255);
        palette[(int)TextEditor::PaletteIndex::PreprocIdentifier] = IM_COL32(230, 165, 80, 255);
        palette[(int)TextEditor::PaletteIndex::LineNumber] = IM_COL32(69, 155, 162, 255);
        editor.SetPalette(palette);
        editor.SetShowWhitespaces(true);
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
        playEditorSound(EditorSoundCue::Warning, 0.95f);
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

namespace {
    struct ScriptingFileRow {
        size_t index;
        std::string key;
        std::string label;
        size_t icon;
        bool batchCapable;
    };

    struct ScriptingFolder {
        std::map<std::string, ScriptingFolder> folders;
        std::vector<ScriptingFileRow> files;
    };

    static constexpr const char* kScriptIconPaths[] = {
        "Resources/Engine-Root/Scripting IDE/ModuCPP Icon.png",
        "Resources/Engine-Root/Scripting IDE/CPP Icon.png",
        "Resources/Engine-Root/Scripting IDE/C Icon.png",
        "Resources/Engine-Root/File Explorer/File Icon Script.png"
    };

    static bool isRelativeToRoot(const fs::path& relative) {
        return !relative.empty() && *relative.begin() != "..";
    }

    static ScriptLanguageServiceDocumentData analyzeScriptBuffer(
        TextEditor& editor, const fs::path& path, const std::string& buffer) {
        auto language = ScriptLanguageService::analyzeDocument(path, buffer);
        if (language.language == ScriptEditorLanguage::Cpp || language.language == ScriptEditorLanguage::C) {
            language.functionSignatures["Begin"] = "void Begin()";
            language.functionSignatures["TickUpdate"] = "void TickUpdate(float deltaTime)";
            language.functionSignatures["Spec"] = "void Spec()";
            language.functionSignatures["TestEditor"] = "void TestEditor()";
            language.functionSignatures["Update"] = "void Update(float deltaTime)";
        }
        editor.SetLanguageDefinition(buildLanguageDefinition(language.language, language.functions, language.defines));
        return language;
    }
}

void Engine::refreshScriptingFileList() {
    scriptingFileList.clear();
    scriptingCompletions.clear();
    scriptingDirectory.clear();
    if (!projectManager.currentProject.isLoaded) {
        return;
    }

    const Project& project = projectManager.currentProject;
    ScriptBuildConfig config;
    std::string error;
    const bool hasConfig = scriptCompiler.loadConfig(resolveScriptsConfigPath(project), config, error);
    scriptingDirectory = (hasConfig ? config.scriptsDir : project.assetsPath / "Scripts").lexically_normal();
    ScriptLanguageServiceProjectData projectData = ScriptLanguageService::scanProjectFiles(
        project.projectPath, project.assetsPath, hasConfig ? &config : nullptr);
    scriptingFileList = std::move(projectData.files);
    scriptingCompletions = std::move(projectData.projectSymbols);
    for (auto& document : scriptEditorDocuments) {
        document->completionPoolDirty = true;
    }
}

void Engine::syncScriptEditorProject() {
    const Project& project = projectManager.currentProject;
    const fs::path path = project.isLoaded ? project.projectPath : fs::path{};
    if (scriptingProjectPath == path) return;

    scriptingProjectPath = path;
    activeScriptEditor = nullptr;
    scriptEditorPendingClose = nullptr;
    selectScriptEditorTab = false;
    scriptEditorDocuments.clear();
    scriptingFilesDirty = true;
}

bool Engine::loadScriptEditorDocument(ScriptEditorDocument& document) {
    std::ifstream file(document.filePath);
    if (!file.is_open()) {
        addConsoleMessage("Failed to read script: " + document.filePath.string(), ConsoleMessageType::Error);
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad() || buffer.bad()) {
        addConsoleMessage("Failed to read script: " + document.filePath.string(), ConsoleMessageType::Error);
        return false;
    }

    document.buffer = buffer.str();
    document.language = analyzeScriptBuffer(document.editor, document.filePath, document.buffer);
    document.editor.SetText(document.buffer);
    document.completionPoolDirty = true;
    document.transpiledPreviewDirty = true;
    document.dirty = false;
    std::error_code ec;
    document.lastWriteTime = fs::last_write_time(document.filePath, ec);
    document.hasWriteTime = !ec;
    return true;
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

    syncScriptEditorProject();
    const std::string key = normalizeScriptSelectionKey(normalized);
    const auto existing = std::find_if(scriptEditorDocuments.begin(), scriptEditorDocuments.end(),
                                       [&](const auto& document) { return document->key == key; });
    if (existing != scriptEditorDocuments.end()) {
        activeScriptEditor = existing->get();
    } else {
        auto document = std::make_unique<ScriptEditorDocument>();
        document->filePath = std::move(normalized);
        document->key = key;
        document->name = document->filePath.filename().string();
        document->tabLabel = document->name + "###" + key;
        initializeScriptEditor(document->editor);
        if (!loadScriptEditorDocument(*document)) return;
        activeScriptEditor = document.get();
        scriptEditorDocuments.push_back(std::move(document));
    }

    selectScriptEditorTab = true;
    showScriptingWindow = true;
}

void Engine::renderScriptingWindow() {
    if (!showScriptingWindow) return;
    if (!hasScriptingWindowPackage()) {
        showScriptingWindow = false;
        return;
    }

    syncScriptEditorProject();
    if (selectScriptEditorTab) ImGui::SetNextWindowFocus();
    if (!ImGui::Begin(Loc::Window("WINDOW_SCRIPTING", "Scripting"), &showScriptingWindow)) {
        ImGui::End();
        return;
    }
    if (!projectManager.currentProject.isLoaded) {
        ImGui::TextDisabled("Load a project to edit scripts.");
        ImGui::End();
        return;
    }

    static bool showFilePane = true;
    static bool showMinimap = true;
    static bool showDiagnosticsPane = true;
    static bool showIntelliSense = false;
    static float filePaneWidth = 202.0f;
    static float diagnosticsPaneHeight = 148.0f;
    static ScriptingFolder fileTree;
    static std::string cachedFilter;
    static std::unordered_set<std::string> selectedCompileScripts;
    static fs::path batchProjectPath;
    if (batchProjectPath != scriptingProjectPath) {
        batchProjectPath = scriptingProjectPath;
        selectedCompileScripts.clear();
    }

    const bool listRefreshed = scriptingFilesDirty;
    if (listRefreshed) {
        refreshScriptingFileList();
        scriptingFilesDirty = false;
    }

    auto saveDocument = [&](ScriptEditorDocument& document) {
        std::ofstream out(document.filePath);
        out << document.buffer;
        out.close();
        if (!out) {
            addConsoleMessage("Failed to save script: " + document.filePath.string(), ConsoleMessageType::Error);
            return false;
        }
        document.dirty = false;
        std::error_code ec;
        document.lastWriteTime = fs::last_write_time(document.filePath, ec);
        document.hasWriteTime = !ec;
        if (scriptEditorAutoCompileOnSave && ScriptLanguageService::isCompilableScriptPath(document.filePath)) {
            compileScriptFile(document.filePath);
        }
        return true;
    };

    auto closeDocument = [&](ScriptEditorDocument* document) {
        auto it = std::find_if(scriptEditorDocuments.begin(), scriptEditorDocuments.end(),
                               [&](const auto& tab) { return tab.get() == document; });
        if (it == scriptEditorDocuments.end()) return;
        if (activeScriptEditor == document) {
            activeScriptEditor = it != scriptEditorDocuments.begin() ? (it - 1)->get()
                : (it + 1 != scriptEditorDocuments.end() ? (it + 1)->get() : nullptr);
            selectScriptEditorTab = activeScriptEditor != nullptr;
        }
        scriptEditorDocuments.erase(it);
    };

    ImGuiStyle& style = ImGui::GetStyle();
    const bool checkboxSwitch = style.CheckboxSwitch;
    style.CheckboxSwitch = true;
    ImGui::Checkbox("Auto-compile on save", &scriptEditorAutoCompileOnSave);
    style.CheckboxSwitch = checkboxSwitch;
    ImGui::SameLine();
    ImGui::BeginDisabled(!activeScriptEditor);
    if (ImGui::Button("Save")) saveDocument(*activeScriptEditor);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool canCompile = activeScriptEditor && ScriptLanguageService::isCompilableScriptPath(activeScriptEditor->filePath);
    ImGui::BeginDisabled(!canCompile);
    if (ImGui::Button("Compile")) compileScriptFile(activeScriptEditor->filePath);
    ImGui::EndDisabled();

    static std::string reloadCheckKey;
    static double lastReloadCheckTime = 0.0;
    static bool canReload = false;
    if (activeScriptEditor && activeScriptEditor->hasWriteTime) {
        const double now = glfwGetTime();
        if (reloadCheckKey != activeScriptEditor->key || now - lastReloadCheckTime >= 0.25) {
            reloadCheckKey = activeScriptEditor->key;
            lastReloadCheckTime = now;
            std::error_code ec;
            const auto diskTime = fs::last_write_time(activeScriptEditor->filePath, ec);
            canReload = !ec && diskTime != activeScriptEditor->lastWriteTime;
        }
        if (canReload) {
            ImGui::SameLine();
            if (ImGui::Button("Reload")) {
                if (loadScriptEditorDocument(*activeScriptEditor)) canReload = false;
            }
            ImGui::SetItemTooltip("The file changed on disk. Reload it from disk.");
        }
    } else {
        reloadCheckKey.clear();
    }

    const float optionsWidth = ImGui::CalcTextSize("...").x + style.FramePadding.x * 2.0f;
    const float optionsX = ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - optionsWidth;
    if (ImGui::GetWindowPos().x + optionsX >= ImGui::GetItemRectMax().x + style.ItemSpacing.x) {
        ImGui::SameLine(optionsX);
    }
    if (ImGui::Button("...##ScriptingOptions", ImVec2(optionsWidth, 0.0f))) {
        ImGui::OpenPopup("Scripting Options");
    }
    ImGui::SetItemTooltip("Scripting options");
    if (ImGui::BeginPopup("Scripting Options")) {
        if (ImGui::MenuItem("Refresh Files")) scriptingFilesDirty = true;
        ImGui::MenuItem("File Tree", nullptr, &showFilePane);
        ImGui::MenuItem("Minimap", nullptr, &showMinimap);
        ImGui::MenuItem("Diagnostics and Output", nullptr, &showDiagnosticsPane);
        if (ImGui::MenuItem("IntelliSense", nullptr, &showIntelliSense) && showIntelliSense) {
            showDiagnosticsPane = true;
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        ImGui::InputTextWithHint("##ScriptFilter", "Filter files", scriptingFilter, sizeof(scriptingFilter));
        ImGui::Separator();
        const size_t batchCount = selectedCompileScripts.size();
        ImGui::TextDisabled("%zu selected for compile", batchCount);
        if (ImGui::MenuItem("Compile Selected", nullptr, false, batchCount > 0 && !compileInProgress)) {
            std::vector<fs::path> batch;
            batch.reserve(batchCount);
            for (const fs::path& path : scriptingFileList) {
                if (selectedCompileScripts.contains(normalizeScriptSelectionKey(path))) batch.push_back(path);
            }
            if (!batch.empty()) queueScriptCompileBatch(batch);
        }
        if (ImGui::MenuItem("Clear Selection", nullptr, false, batchCount > 0)) selectedCompileScripts.clear();
        ImGui::EndPopup();
    }

    ScriptEditorDocument* closedDocument = nullptr;
    bool confirmClose = false;
    if (!scriptEditorDocuments.empty() &&
        ImGui::BeginTabBar("ScriptingDocuments", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll)) {
        const bool selectRequestedTab = selectScriptEditorTab;
        selectScriptEditorTab = false;
        for (const auto& document : scriptEditorDocuments) {
            bool tabOpen = true;
            ImGuiTabItemFlags flags = document->dirty ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None;
            if (selectRequestedTab && activeScriptEditor == document.get()) flags |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem(document->tabLabel.c_str(), &tabOpen, flags)) {
                if (!selectRequestedTab) activeScriptEditor = document.get();
                ImGui::EndTabItem();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", document->filePath.string().c_str());
            if (!tabOpen) {
                if (document->dirty) {
                    scriptEditorPendingClose = document.get();
                    confirmClose = true;
                } else {
                    closedDocument = document.get();
                }
            }
        }
        ImGui::EndTabBar();
    }

    if (confirmClose) ImGui::OpenPopup("Close Script");
    if (scriptEditorPendingClose && ImGui::BeginPopupModal("Close Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s?", scriptEditorPendingClose->name.c_str());
        if (ImGui::Button("Save") && saveDocument(*scriptEditorPendingClose)) {
            closedDocument = scriptEditorPendingClose;
            scriptEditorPendingClose = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            closedDocument = scriptEditorPendingClose;
            scriptEditorPendingClose = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            scriptEditorPendingClose = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (closedDocument) closeDocument(closedDocument);

    if (listRefreshed || cachedFilter != scriptingFilter) {
        cachedFilter = scriptingFilter;
        const std::string filter = toLowerCopy(cachedFilter);
        fileTree = {};
        fileTree.folders.try_emplace("Scripts");
        fileTree.folders.try_emplace("Assets");
        std::unordered_set<std::string> validBatchKeys;
        for (size_t index = 0; index < scriptingFileList.size(); ++index) {
            const fs::path& path = scriptingFileList[index];
            std::string key = normalizeScriptSelectionKey(path);
            if (selectedCompileScripts.contains(key)) validBatchKeys.insert(key);

            std::string name = path.filename().string();
            if (!filter.empty() && toLowerCopy(name).find(filter) == std::string::npos) continue;

            fs::path relative = scriptingDirectory.empty() ? fs::path{} : path.lexically_relative(scriptingDirectory);
            if (isRelativeToRoot(relative)) {
                relative = "Scripts" / relative;
            } else {
                relative = path.lexically_relative(scriptingProjectPath);
                if (!isRelativeToRoot(relative)) relative = "External" / path.relative_path();
            }
            ScriptingFolder* folder = &fileTree;
            for (const auto& part : relative.parent_path()) {
                folder = &folder->folders[part.string()];
            }

            const std::string ext = extensionLower(path);
            size_t icon = 3;
            if (ext == ".moducpp") icon = 0;
            else if (ext == ".c") icon = 2;
            else if (ext != ".cs" && ScriptLanguageService::detectLanguage(path) == ScriptEditorLanguage::Cpp) icon = 1;
            folder->files.push_back({ index, std::move(key), std::move(name), icon, ext != ".cs" && ext != ".csproj" });
        }
        selectedCompileScripts.swap(validBatchKeys);
        auto sortFiles = [&](auto&& sort, ScriptingFolder& folder) -> void {
            std::sort(folder.files.begin(), folder.files.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.label < rhs.label;
            });
            for (auto& entry : folder.folders) sort(sort, entry.second);
        };
        sortFiles(sortFiles, fileTree);
    }

    const float splitter = 4.0f;
    const float minimumFileWidth = 104.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    const float fileWidthBudget = width - 160.0f - splitter;
    if (showFilePane && fileWidthBudget >= minimumFileWidth) {
        const float renderedWidth = std::clamp(filePaneWidth, minimumFileWidth, fileWidthBudget);
        if (ImGui::BeginChild("ScriptingFiles", ImVec2(renderedWidth, 0.0f), ImGuiChildFlags_Borders)) {
            struct ScriptIcon {
                ImTextureID id = ImTextureID_Invalid;
                bool flipY = false;
            };
            ScriptIcon icons[IM_ARRAYSIZE(kScriptIconPaths)];
            bool requestedIcons[IM_ARRAYSIZE(kScriptIconPaths)] = {};
            auto getIcon = [&](size_t index) {
                if (!requestedIcons[index]) {
                    requestedIcons[index] = true;
                    const char* path = kScriptIconPaths[index];
                    if (usingVulkan() && vulkanRendererInitialized && vulkanRenderer) {
                        icons[index].id = vulkanRenderer->getOrCreateUIImage(path);
                    } else if (rendererInitialized) {
                        if (Texture* texture = renderer.getTexture(path, MaterialProperties::TextureFilter::Point);
                            texture && texture->GetID() != 0) {
                            icons[index] = { static_cast<ImTextureID>(texture->GetID()), true };
                        }
                    }
                }
                return icons[index];
            };

            const float iconWidth = ImGui::GetFontSize();
            const float iconHeight = iconWidth * 14.0f / 15.0f;
            const auto drawFolder = [&](auto&& draw, const std::string& name, const ScriptingFolder& folder, bool defaultOpen = false) -> void {
                ImGui::PushID(name.c_str());
                if (!cachedFilter.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
                if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
                if (ImGui::TreeNodeEx("Folder", flags, "%s", name.c_str())) {
                    for (const auto& [childName, child] : folder.folders) draw(draw, childName, child);
                    for (const auto& row : folder.files) {
                        ImGui::PushID(row.key.c_str());
                        const ImVec2 pos = ImGui::GetCursorScreenPos();
                        const bool batchSelected = selectedCompileScripts.contains(row.key);
                        const bool fileActive = activeScriptEditor && activeScriptEditor->key == row.key;
                        if (ImGui::Selectable("##ScriptFile", fileActive || batchSelected,
                                              ImGuiSelectableFlags_SpanAvailWidth)) {
                            if (ImGui::GetIO().KeyCtrl && row.batchCapable) {
                                if (batchSelected) selectedCompileScripts.erase(row.key);
                                else selectedCompileScripts.insert(row.key);
                            } else {
                                openScriptInEditor(scriptingFileList[row.index]);
                            }
                        }
                        const bool visible = ImGui::IsItemVisible();
                        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
                        const float right = ImGui::GetItemRectMax().x - 3.0f;
                        if (ImGui::BeginPopupContextItem("Script File Actions")) {
                            if (ImGui::MenuItem("Open")) openScriptInEditor(scriptingFileList[row.index]);
                            if (ImGui::MenuItem("Select for Compile", nullptr, batchSelected, row.batchCapable)) {
                                if (batchSelected) selectedCompileScripts.erase(row.key);
                                else selectedCompileScripts.insert(row.key);
                            }
                            ImGui::EndPopup();
                        }
                        if (hovered) ImGui::SetTooltip("%s", scriptingFileList[row.index].string().c_str());
                        if (visible) {
                            const ScriptIcon icon = getIcon(row.icon);
                            ImDrawList* drawList = ImGui::GetWindowDrawList();
                            if (icon.id != ImTextureID_Invalid) {
                                const float y = pos.y + (ImGui::GetTextLineHeight() - iconHeight) * 0.5f;
                                drawList->AddImage(icon.id, ImVec2(pos.x, y), ImVec2(pos.x + iconWidth, y + iconHeight),
                                                   ImVec2(0.0f, icon.flipY ? 1.0f : 0.0f),
                                                   ImVec2(1.0f, icon.flipY ? 0.0f : 1.0f));
                            }
                            const ImVec2 labelPos(pos.x + iconWidth + 4.0f, pos.y);
                            if (labelPos.x < right) {
                                ImGui::RenderTextEllipsis(drawList, labelPos, ImVec2(right, pos.y + ImGui::GetTextLineHeight()),
                                                          right, row.label.c_str(), nullptr, nullptr);
                            }
                        }
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            };

            drawFolder(drawFolder, "Scripts", fileTree.folders.at("Scripts"), true);
            drawFolder(drawFolder, "Assets", fileTree.folders.at("Assets"));
            for (const auto& [name, folder] : fileTree.folders) {
                if (name != "Scripts" && name != "Assets") drawFolder(drawFolder, name, folder);
            }
        }
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::InvisibleButton("ScriptingFilePaneSplitter", ImVec2(splitter, std::max(1.0f, ImGui::GetContentRegionAvail().y)));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive()) {
            filePaneWidth = std::clamp(renderedWidth + ImGui::GetIO().MouseDelta.x, minimumFileWidth, fileWidthBudget);
        }
        ImGui::SameLine(0.0f, 0.0f);
    }

    if (ImGui::BeginChild("ScriptingEditor", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar)) {
        const float height = ImGui::GetContentRegionAvail().y;
        const float verticalSpacing = style.ItemSpacing.y * 2.0f + splitter;
        const float minEditorHeight = 76.0f;
        const float minDiagnosticsHeight = 68.0f;
        const float maxDiagnosticsHeight = height - minEditorHeight - verticalSpacing;
        const bool diagnosticsVisible = showDiagnosticsPane && maxDiagnosticsHeight >= minDiagnosticsHeight;
        const float renderedDiagnosticsHeight = diagnosticsVisible
            ? std::clamp(diagnosticsPaneHeight, minDiagnosticsHeight, maxDiagnosticsHeight) : 0.0f;
        const float editorHeight = diagnosticsVisible ? height - renderedDiagnosticsHeight - verticalSpacing : height;

        if (ImGui::BeginChild("ScriptEditorPane", ImVec2(0.0f, editorHeight), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            if (activeScriptEditor) {
                ScriptEditorDocument& document = *activeScriptEditor;
                ImGui::PushID(document.key.c_str());
                if (ImGui::BeginTabBar("ScriptViews")) {
                    if (ImGui::BeginTabItem("Script")) {
                        document.showTranspiled = false;
                        ImGui::EndTabItem();
                    }
                    if (document.language.language == ScriptEditorLanguage::ModuCPP && ImGui::BeginTabItem("Transpiled")) {
                        document.showTranspiled = true;
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                const float minimapWidth = showMinimap ? ImGui::GetFontSize() * 4.5f : 0.0f;
                if (document.showTranspiled) {
                    if (document.transpiledPreviewDirty) {
                        document.transpiledPreviewDirty = false;
                        document.transpiledError.clear();
                        ModuCPPTranspileResult result;
                        try {
                            ModuCPPTranspiler transpiler;
                            if (transpiler.transpile(document.filePath, document.buffer, result, document.transpiledError)) {
                                if (!document.transpiledEditor) {
                                    document.transpiledEditor = std::make_unique<TextEditor>();
                                    initializeScriptEditor(*document.transpiledEditor);
                                    document.transpiledEditor->SetShowWhitespaces(false);
                                    document.transpiledEditor->SetReadOnly(true);
                                }
                                document.transpiledEditor->SetLanguageDefinition(
                                    result.generatedC ? TextEditor::LanguageDefinition::C() : TextEditor::LanguageDefinition::CPlusPlus());
                                document.transpiledEditor->SetText(result.generatedSource);
                            } else if (document.transpiledError.empty()) {
                                document.transpiledError = "Could not transpile this script.";
                            }
                        } catch (const std::regex_error& error) {
                            document.transpiledError = error.what();
                        }
                    }
                    if (!document.transpiledError.empty()) {
                        ImGui::TextWrapped("%s", document.transpiledError.c_str());
                    } else if (document.transpiledEditor) {
                        document.transpiledEditor->Render("TranspiledSource", ImVec2(0.0f, 0.0f), true, minimapWidth);
                    }
                } else {
                    renderScriptEditorDocument(document, minimapWidth);
                }
                ImGui::PopID();
            } else {
                ImGui::TextDisabled("Select a script or shader file to start editing.");
            }
        }
        ImGui::EndChild();

        if (diagnosticsVisible) {
            ImGui::InvisibleButton("ScriptingDiagnosticsSplitter",
                                   ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), splitter));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsItemActive()) {
                diagnosticsPaneHeight = std::clamp(renderedDiagnosticsHeight - ImGui::GetIO().MouseDelta.y,
                                                   minDiagnosticsHeight, maxDiagnosticsHeight);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 3.0f));
            if (ImGui::BeginChild("ScriptDiagnosticsPane", ImVec2(0.0f, renderedDiagnosticsHeight),
                                  ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar)) {
                if (ImGui::BeginTabBar("ScriptDiagnosticsTabs")) {
                    if (ImGui::BeginTabItem("Diagnostics")) {
                        renderScriptDiagnostics();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Output")) {
                        if (!lastCompileStatus.empty()) ImGui::TextUnformatted(lastCompileStatus.c_str());
                        if (lastCompileLog.empty()) {
                            ImGui::TextDisabled("No build output.");
                        } else {
                            if (ImGui::BeginChild("ScriptOutputLog", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                                                  ImGuiWindowFlags_HorizontalScrollbar)) {
                                ImGui::TextUnformatted(lastCompileLog.c_str());
                            }
                            ImGui::EndChild();
                        }
                        ImGui::EndTabItem();
                    }
                    if (showIntelliSense && ImGui::BeginTabItem("IntelliSense")) {
                        if (ImGui::BeginChild("ScriptIntelliSense", ImVec2(0.0f, 0.0f))) {
                            ScriptBuildConfig config;
                            std::string error;
                            if (scriptCompiler.loadConfig(resolveScriptsConfigPath(projectManager.currentProject), config, error)) {
                                packageManager.applyToBuildConfig(config);
                                ImGui::Text("Standard: %s", config.cppStandard.c_str());
                                ImGui::SeparatorText("Include Directories");
                                for (const auto& path : config.includeDirs) ImGui::BulletText("%s", path.string().c_str());
                                ImGui::SeparatorText("Defines");
                                for (const auto& define : config.defines) ImGui::BulletText("%s", define.c_str());
                            } else {
                                ImGui::TextWrapped("%s", error.c_str());
                            }
                            ImGui::SeparatorText("Outline");
                            if (activeScriptEditor && !activeScriptEditor->language.symbols.empty()) {
                                for (const auto& symbol : activeScriptEditor->language.symbols) ImGui::BulletText("%s", symbol.c_str());
                            } else {
                                ImGui::TextDisabled("No symbols detected.");
                            }
                        }
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void Engine::renderScriptEditorDocument(ScriptEditorDocument& document, float minimapWidth) {
    static std::string completionDocumentKey;
    static std::vector<std::string> completionPool;
    static std::vector<std::string> activeSuggestions;
    static std::string activePrefix;
    static int selectedSuggestionIndex = 0;
    static bool completionManuallyDismissed = false;
    static std::string completionDismissPrefix;
    static bool completionPopupVisibleLastFrame = false;

    if (completionDocumentKey != document.key) {
        completionDocumentKey = document.key;
        document.completionPoolDirty = true;
    }
    if (document.completionPoolDirty) {
        completionManuallyDismissed = false;
        completionDismissPrefix.clear();
        completionPopupVisibleLastFrame = false;
    }
    TextEditor& editor = document.editor;
    const ImGuiIO& io = ImGui::GetIO();

    auto rebuildCompletionPool = [&]() -> bool {
        if (!document.completionPoolDirty) return false;
        completionPool.clear();
        std::unordered_set<std::string> poolSet;
        const auto& langDef = editor.GetLanguageDefinition();
        for (const auto& kw : langDef.mKeywords) poolSet.insert(kw);
        for (const auto& identifier : langDef.mIdentifiers) poolSet.insert(identifier.first);
        for (const auto& identifier : langDef.mPreprocIdentifiers) poolSet.insert(identifier.first);
        for (const auto& entry : scriptingCompletions) poolSet.insert(entry);
        for (const auto& entry : document.language.symbols) poolSet.insert(entry);
        for (const auto& entry : document.language.identifiers) poolSet.insert(entry);
        for (const auto& entry : document.language.functions) poolSet.insert(entry);
        for (const auto& entry : document.language.defines) poolSet.insert(entry);
        for (const auto& entry : document.language.variables) poolSet.insert(entry);
        for (const auto& entry : document.language.moducppImports) poolSet.insert(entry);
        for (const auto& entry : document.language.moducppInspectorFields) poolSet.insert(entry);
        for (const auto& entry : document.language.moducppCompletions) poolSet.insert(entry);
        completionPool.assign(poolSet.begin(), poolSet.end());
        std::sort(completionPool.begin(), completionPool.end());
        document.completionPoolDirty = false;
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
        token.cursor = editor.GetCursorPosition();
        TextEditor::Coordinates probe = token.cursor;
        token.word = editor.GetWordAtPublic(probe);
        if (token.word.empty() && token.cursor.mColumn > 0) {
            probe = TextEditor::Coordinates(token.cursor.mLine, token.cursor.mColumn - 1);
            token.word = editor.GetWordAtPublic(probe);
        }
        if (token.word.empty()) return token;
        token.start = editor.FindWordStartPublic(probe);
        token.end = editor.FindWordEndPublic(probe);
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
        TextEditor::Coordinates cursor = editor.GetCursorPosition();
        TextEditor::Coordinates replaceStart = token.valid
            ? token.start
            : TextEditor::Coordinates(cursor.mLine, std::max(0, cursor.mColumn - static_cast<int>(activePrefix.size())));
        TextEditor::Coordinates replaceEnd = token.valid ? token.end : cursor;
        editor.SetSelection(replaceStart, replaceEnd);
        editor.Delete();
        editor.InsertText(suggestion.c_str());
        document.dirty = true;
        document.buffer = editor.GetText();
        document.language = analyzeScriptBuffer(editor, document.filePath, document.buffer);
        document.completionPoolDirty = true;
        document.transpiledPreviewDirty = true;
        completionManuallyDismissed = true;
        completionDismissPrefix = suggestion;
    };

    bool poolRebuiltBeforeRender = rebuildCompletionPool();
    ActiveTokenInfo tokenBeforeRender = activeToken();
    if (poolRebuiltBeforeRender || tokenBeforeRender.prefix != activePrefix) {
        updateSuggestions(tokenBeforeRender.prefix);
    }
    bool interceptCompletionKeys = !activeSuggestions.empty() && !activePrefix.empty() && !completionManuallyDismissed;
    editor.SetAllowTabInput(!interceptCompletionKeys);
    editor.SetAllowArrowNavigation(!interceptCompletionKeys);
    editor.SetAllowEnterInput(!interceptCompletionKeys);


    const ImVec2 editorRectMin = ImGui::GetCursorScreenPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float minimapSpace = minimapWidth > 0.0f && available.x >= minimapWidth + 120.0f
        ? minimapWidth + 4.0f : 0.0f;
    const ImVec2 editorRectMax(editorRectMin.x + available.x - minimapSpace, editorRectMin.y + available.y);
    editor.Render("ScriptSource", ImVec2(0.0f, 0.0f), true, minimapWidth);
    if (editor.IsTextChanged()) {
        document.dirty = true;
        document.buffer = editor.GetText();
        document.language = analyzeScriptBuffer(editor, document.filePath, document.buffer);
        document.completionPoolDirty = true;
        document.transpiledPreviewDirty = true;
    }

    bool poolRebuiltAfterRender = rebuildCompletionPool();
    ActiveTokenInfo tokenAfterRender = activeToken();
    if (poolRebuiltAfterRender || tokenAfterRender.prefix != activePrefix) {
        updateSuggestions(tokenAfterRender.prefix);
    }

    bool editorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    if (editorFocused && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        completionManuallyDismissed = false;
        completionDismissPrefix.clear();
    }

    bool completionVisible = editorFocused &&
        editor.HasCursorScreenPosition() &&
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
    ImVec2 caretPos = editor.GetCursorScreenPositionPublic();
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

    if (editorFocused && editor.HasCursorScreenPosition()) {
        std::string signatureLabel;
        ScriptLanguageServiceFunctionCallContext callCtx = ScriptLanguageService::detectFunctionCallContext(
            editor.GetCurrentLineText(),
            editor.GetCursorPosition().mColumn);
        if (callCtx.valid) {
            auto tryFindSignature = [&](const std::string& name) -> std::string {
                auto it = document.language.functionSignatures.find(name);
                if (it != document.language.functionSignatures.end()) return it->second;
                size_t scope = name.rfind("::");
                if (scope != std::string::npos && scope + 2 < name.size()) {
                    std::string shortName = name.substr(scope + 2);
                    it = document.language.functionSignatures.find(shortName);
                    if (it != document.language.functionSignatures.end()) return it->second;
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

    if (completionVisible && editor.HasCursorScreenPosition() && !activeSuggestions.empty()) {
        const std::string& suggestion = activeSuggestions[static_cast<size_t>(selectedSuggestionIndex)];
        if (suggestion.size() > activePrefix.size() && suggestion.rfind(activePrefix, 0) == 0) {
            std::string ghost = suggestion.substr(activePrefix.size());
            ImU32 ghostColor = IM_COL32(180, 180, 180, 110);
            ImGui::GetForegroundDrawList()->AddText(editor.GetCursorScreenPositionPublic(),
                ghostColor, ghost.c_str());
        }
    }
}

void Engine::renderScriptDiagnostics() {
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

    ImGui::Text("Errors: %d  Warnings: %d  Info: %d", errorCount, warningCount, infoCount);
    ImGui::Separator();
    if (ImGui::BeginChild("ScriptDiagnosticList", ImVec2(0.0f, 0.0f))) {
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
    ImGui::EndChild();
}
