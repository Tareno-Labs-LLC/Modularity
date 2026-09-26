#include "../EditorLocalization.h"
#include "Engine.h"
#include "SceneObjectIcons.h"

#if !MODULARITY_RUNTIME_ONLY

#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#include <shellapi.h>
#endif

namespace Loc = Modularity::Loc;

namespace {

#pragma region Formatting

enum class PendingGroup { Conflicts = 0, Changed, Deleted, Moved, Added, Count };

const char* GroupLabel(PendingGroup group) {
    switch (group) {
        case PendingGroup::Conflicts: return "Conflicts";
        case PendingGroup::Changed: return "Changed items";
        case PendingGroup::Deleted: return "Deleted items";
        case PendingGroup::Moved: return "Moved items";
        case PendingGroup::Added: return "Added items";
        default: return "";
    }
}

PendingGroup GroupOf(VcChangeKind kind) {
    switch (kind) {
        case VcChangeKind::Conflicted: return PendingGroup::Conflicts;
        case VcChangeKind::Deleted: return PendingGroup::Deleted;
        case VcChangeKind::Renamed: return PendingGroup::Moved;
        case VcChangeKind::Added:
        case VcChangeKind::Untracked: return PendingGroup::Added;
        default: return PendingGroup::Changed;
    }
}

const char* KindLabel(VcChangeKind kind) {
    switch (kind) {
        case VcChangeKind::Added: return "Added";
        case VcChangeKind::Untracked: return "New";
        case VcChangeKind::Deleted: return "Deleted";
        case VcChangeKind::Renamed: return "Moved";
        case VcChangeKind::Conflicted: return "Conflicted";
        default: return "Changed";
    }
}

ImVec4 KindColor(VcChangeKind kind) {
    switch (kind) {
        case VcChangeKind::Added:
        case VcChangeKind::Untracked: return ImVec4(0.45f, 0.84f, 0.52f, 1.0f);
        case VcChangeKind::Deleted: return ImVec4(0.95f, 0.45f, 0.42f, 1.0f);
        case VcChangeKind::Renamed: return ImVec4(0.56f, 0.72f, 0.98f, 1.0f);
        case VcChangeKind::Conflicted: return ImVec4(1.0f, 0.55f, 0.25f, 1.0f);
        default: return ImVec4(0.93f, 0.78f, 0.38f, 1.0f);
    }
}

std::string FormatAbsolute(std::time_t t) {
    if (t <= 0) return std::string();
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y/%m/%d %H:%M", &local);
    return buffer;
}

std::string FormatRelative(std::time_t t) {
    if (t <= 0) return std::string();
    const double seconds = std::difftime(std::time(nullptr), t);
    const int minutes = static_cast<int>(seconds / 60.0);
    const int hours = minutes / 60;
    const int days = hours / 24;
    if (seconds < 45.0) return "just now";
    if (minutes < 2) return "a minute ago";
    if (minutes < 60) return std::to_string(minutes) + " minutes ago";
    if (hours < 2) return "an hour ago";
    if (hours < 24) return std::to_string(hours) + " hours ago";
    if (days < 2) return "yesterday";
    if (days < 30) return std::to_string(days) + " days ago";
    return FormatAbsolute(t);
}

// "24 minutes ago (2026/09/23 12:40)", or just the date once it is old.
std::string FormatWhen(std::time_t t) {
    const std::string relative = FormatRelative(t);
    const std::string absolute = FormatAbsolute(t);
    if (relative.empty() || relative == absolute) return absolute;
    return relative + " (" + absolute + ")";
}

fs::path FromUtf8(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

// Rows show paths relative to the project, the way the Project panel does.
std::string DisplayPath(const std::string& repoPath, const std::string& prefix) {
    if (!prefix.empty() && repoPath.compare(0, prefix.size(), prefix) == 0) return repoPath.substr(prefix.size());
    return repoPath;
}

void OpenExternal(const fs::path& target, bool selectInFolder) {
#ifdef _WIN32
    if (selectInFolder) {
        const std::wstring args = L"/select,\"" + target.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, L"open", target.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
#else
    const fs::path open = selectInFolder ? target.parent_path() : target;
#if defined(__APPLE__)
    const std::string cmd = "open \"" + open.string() + "\" &";
#else
    const std::string cmd = "xdg-open \"" + open.string() + "\" >/dev/null 2>&1 &";
#endif
    (void)std::system(cmd.c_str());
#endif
}

void OpenUrl(const char* url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    (void)std::system((std::string("open \"") + url + "\" &").c_str());
#else
    (void)std::system((std::string("xdg-open \"") + url + "\" >/dev/null 2>&1 &").c_str());
#endif
}

std::string MegabyteText(std::uintmax_t bytes) {
    const std::uintmax_t mib = 1024ull * 1024ull;
    if (bytes < mib) return std::to_string(bytes) + " bytes";
    return std::to_string((bytes + mib / 2) / mib) + " MB";
}

bool IsGitHubAddress(std::string url) {
    std::transform(url.begin(), url.end(), url.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return url.find("github.com") != std::string::npos;
}

// Split files travel as ".modusplit/<path>.parts/partNNN"; lists show the real file.
std::string FriendlyPath(const std::string& repoPath, const std::string& prefix) {
    static const std::string splitDir = ".modusplit/";
    if (repoPath.compare(0, splitDir.size(), splitDir) == 0) {
        const size_t cut = repoPath.rfind(".parts/");
        if (cut != std::string::npos && cut > splitDir.size()) {
            return DisplayPath(repoPath.substr(splitDir.size(), cut - splitDir.size()), prefix) + "  (split part)";
        }
    }
    return DisplayPath(repoPath, prefix);
}

// "https://github.com/me/My-Game.git" -> "My-Game"
std::string RepoNameFromUrl(std::string url) {
    while (!url.empty() && (url.back() == '/' || url.back() == '\\')) url.pop_back();
    if (url.size() > 4 && url.compare(url.size() - 4, 4, ".git") == 0) url.resize(url.size() - 4);
    const size_t cut = url.find_last_of("/\\:");
    return cut == std::string::npos ? url : url.substr(cut + 1);
}

std::string Trimmed(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// A project-relative path as a .gitignore line that matches only that file:
// glob characters and a leading '#' or '!' would otherwise mean something.
std::string IgnorePatternForFile(const std::string& projectRelative) {
    std::string pattern = "/";
    for (const char c : projectRelative) {
        if (c == '\\' || c == '*' || c == '?' || c == '[') pattern.push_back('\\');
        pattern.push_back(c);
    }
    if (!pattern.empty() && pattern.back() == ' ') pattern.insert(pattern.size() - 1, "\\");
    return pattern;
}

void CopyString(char* dest, size_t size, const std::string& text) {
    std::snprintf(dest, size, "%s", text.c_str());
}

#pragma endregion

struct PendingRow {
    bool header = false;
    PendingGroup group = PendingGroup::Changed;
    int change = -1;
};

struct VcPanelState {
    bool visible = false;
    fs::path boundRoot;  // project the fields below were filled for
    // Repository values last copied into the edit fields; a field only follows
    // the repository when that value changes, so typing in it is never undone.
    std::string seenRemoteUrl;
    std::string seenUserName;
    std::string seenUserEmail;

    std::unordered_set<std::string> unchecked;  // everything starts ticked
    std::string selectedPath;
    bool groupOpen[static_cast<int>(PendingGroup::Count)] = {true, true, true, true, true};
    std::vector<PendingRow> rows;
    std::unordered_map<std::string, FileCategory> categories;
    char comment[4096] = {};

    std::vector<std::string> discardPaths;
    bool requestDiscardModal = false;
    bool showDiscardModal = false;
    bool requestLfsModal = false;
    bool showLfsModal = false;

    std::string selectedCommit;
    char newBranch[128] = {};
    bool newBranchSwitch = true;

    char remoteUrl[512] = {};
    char userName[128] = {};
    char userEmail[256] = {};
    bool identityGlobal = true;

    bool setupGitignore = true;
    bool setupLfs = true;
    bool setupLfsTouched = false;  // until then LFS follows the address: off for GitHub
    char setupRemote[512] = {};

    char cloneUrl[512] = {};
    char cloneParent[1024] = {};
    char cloneFolder[256] = {};
    bool cloneFolderEdited = false;
    std::string cloneError;
    bool cloneRunning = false;
};

VcPanelState& Panel() {
    static VcPanelState state;
    return state;
}

bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    const ImVec4 tint(0.20f, 0.46f, 0.30f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, tint);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(tint.x * 1.15f, tint.y * 1.15f, tint.z * 1.15f, tint.w));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(tint.x * 0.9f, tint.y * 0.9f, tint.z * 0.9f, tint.w));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

void Banner(const ImVec4& accent, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void TooltipIfDisabled(const char* reason) {
    if (reason && *reason && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", reason);
}

// "Uploading: 45% (12/30)" -> 0.45; -1 when the step reports no percentage.
float ProgressFraction(const std::string& step) {
    const size_t percent = step.find('%');
    if (percent == std::string::npos) return -1.0f;
    size_t start = percent;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(step[start - 1]))) --start;
    if (start == percent) return -1.0f;
    return std::clamp(static_cast<float>(std::atoi(step.c_str() + start)) / 100.0f, 0.0f, 1.0f);
}

// The running operation: a bar that fills when git reports a percentage and
// animates when it doesn't (so a long silent step never looks frozen), the
// elapsed time, and git's current step underneath.
void DrawOperationProgress(VersionControl& vc, bool withCancel) {
    const std::string step = vc.progressLine();
    const float fraction = ProgressFraction(step);
    const int elapsed = static_cast<int>(vc.runningSeconds());
    char overlay[128];
    std::snprintf(overlay, sizeof(overlay), "%s   %d:%02d", vc.busyLabel(), elapsed / 60, elapsed % 60);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float cancelWidth = withCancel ? ImGui::CalcTextSize("Cancel").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x : 0.0f;
    ImGui::ProgressBar(fraction >= 0.0f ? fraction : -1.0f * static_cast<float>(ImGui::GetTime()),
                       ImVec2(-cancelWidth, 0.0f), overlay);
    if (withCancel) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel##vc_cancel")) vc.cancel();
    }
    if (!step.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", step.c_str());
        ImGui::PopStyleColor();
    }
}

} // namespace

#pragma region Engine glue

void Engine::updateVersionControl() {
    const fs::path root = projectManager.currentProject.isLoaded ? projectManager.currentProject.projectPath : fs::path();
    VcPanelState& panel = Panel();
    versionControl.update(root, ImGui::GetTime(), showVersionControlWindow && panel.visible);

    VcResult result;
    while (versionControl.takeResult(result)) {
        const bool silentSuccess = result.ok && (result.kind == VcJobKind::Refresh ||
                                                 result.kind == VcJobKind::CommitFiles ||
                                                 result.kind == VcJobKind::Probe);
        if (!result.message.empty() && !silentSuccess) {
            const ConsoleMessageType type = result.ok ? ConsoleMessageType::Success : ConsoleMessageType::Error;
            addConsoleMessage("Version Control: " + result.message, type);
            if (!result.ok && !result.detail.empty()) addConsoleMessage(result.detail, ConsoleMessageType::Info);
            if (result.kind != VcJobKind::Clone) showEditorToast(result.message, type, 3.0);
        }
        if (result.kind == VcJobKind::CheckIn && result.ok) panel.comment[0] = '\0';
        if (result.kind == VcJobKind::CheckIn || result.kind == VcJobKind::Discard) {
            // Forget ticks for files that are no longer pending, so a file that
            // changes again later starts ticked like any other.
            std::unordered_set<std::string> stillPending;
            for (const VcChange& change : versionControl.snapshot().changes) stillPending.insert(change.path);
            for (auto it = panel.unchecked.begin(); it != panel.unchecked.end();) {
                it = stillPending.count(*it) ? std::next(it) : panel.unchecked.erase(it);
            }
        }
        if (result.kind == VcJobKind::Clone) {
            panel.cloneRunning = false;
            if (!result.ok) {
                panel.cloneError = result.message;
            } else if (!result.clonedProjectFile.empty()) {
                panel.cloneError.clear();
                projectManager.showCloneProjectDialog = false;
                OpenProjectPath(PathToUtf8(result.clonedProjectFile));
            } else {
                panel.cloneError = result.message;
            }
        }
        if (!result.changedPaths.empty()) applyVersionControlFileChanges(result);
    }
}

// Git rewrote files under the editor (get changes, switch branch, undo). Pick
// up what the editor has cached from them.
void Engine::applyVersionControlFileChanges(const VcResult& result) {
    fileBrowser.needsRefresh = true;
    Project& project = projectManager.currentProject;
    if (!project.isLoaded) return;

    // Git reports repository-relative paths. Compare in that form instead of
    // resolving each one on disk: a pull can touch thousands of files.
    std::error_code ec;
    const fs::path repoRoot = fs::weakly_canonical(result.repoRoot, ec);
    const fs::path projectRoot = fs::weakly_canonical(project.projectPath, ec);
    const std::string sceneRel = PathToUtf8(fs::weakly_canonical(project.getSceneFilePath(project.currentSceneName), ec)
                                                .lexically_relative(repoRoot).generic_u8string());
    std::string projectPrefix = PathToUtf8(projectRoot.lexically_relative(repoRoot).generic_u8string());
    projectPrefix = (projectPrefix.empty() || projectPrefix == ".") ? std::string() : projectPrefix + "/";
    auto samePath = [](const std::string& a, const std::string& b) {
#ifdef _WIN32
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
#else
        return a == b;
#endif
    };
    static const std::unordered_set<std::string> kTextureExts = {
        ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".tif", ".tiff", ".exr", ".hdr",
        ".dds", ".ktx", ".ktx2", ".webp", ".gif"};

    bool sceneChanged = false;
    bool settingsChanged = false;
    for (const std::string& rel : result.changedPaths) {
        if (samePath(rel, sceneRel)) sceneChanged = true;
        const std::string inProject = DisplayPath(rel, projectPrefix);
        const std::string top = inProject.substr(0, inProject.find('/'));
        if (top == "project.modu" || top == "build.modu" || top == "scripts.modu" || top == "packages.modu" ||
            top == "input_actions.modu" || top == "BuildProfiles" || top == "ProjectSettings") {
            settingsChanged = true;
        }

        // The texture cache is keyed by whatever path string loaded the file, so
        // drop both the absolute and the project-relative spellings.
        std::string ext = FromUtf8(rel).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (kTextureExts.count(ext)) {
            const fs::path absolute = result.repoRoot / FromUtf8(rel);
            const fs::path relative = FromUtf8(inProject);
            renderer.invalidateTexture(absolute.string());
            renderer.invalidateTexture(absolute.generic_string());
            renderer.invalidateTexture(relative.string());
            renderer.invalidateTexture(relative.generic_string());
        }
    }

    if (sceneChanged && !isPlaying) {
        if (fs::exists(project.getSceneFilePath(project.currentSceneName), ec)) {
            performLoadScene(project.currentSceneName);
            addConsoleMessage("Version Control: reloaded " + project.currentSceneName + " from disk.",
                              ConsoleMessageType::Info);
        } else {
            addConsoleMessage("Version Control: the open scene no longer exists on disk. Open another scene "
                              "before saving, or it will be written back.", ConsoleMessageType::Warning);
        }
    }
    if (settingsChanged) {
        addConsoleMessage("Version Control: project settings changed on disk. Reopen the project to apply them.",
                          ConsoleMessageType::Warning);
    }
}

std::string Engine::versionControlStatusText() const {
    const VcSnapshot& s = versionControl.snapshot();
    if (!s.isRepo || !projectManager.currentProject.isLoaded) return std::string();
    std::string text = (s.detached ? "(detached)" : s.branch) + "@" + projectManager.currentProject.name;
    std::string counts;
    if (s.ahead > 0) counts += std::to_string(s.ahead) + " to upload";
    if (s.behind > 0) counts += (counts.empty() ? "" : ", ") + std::to_string(s.behind) + " incoming";
    if (s.unborn) counts = "no check-ins yet";
    return counts.empty() ? text : text + "  (" + counts + ")";
}

#pragma endregion

#pragma region Version Control Window

void Engine::renderVersionControlWindow() {
    VcPanelState& panel = Panel();
    panel.visible = false;
    if (!showVersionControlWindow) return;

    // First time it opens, sit next to the Project panel like the other bottom tabs.
    if (ImGuiWindow* projectWindow = ImGui::FindWindowByName(Loc::WindowRef("Project"))) {
        if (projectWindow->DockId != 0) ImGui::SetNextWindowDockID(projectWindow->DockId, ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_FirstUseEver);
    const bool wasOpen = showVersionControlWindow;
    if (!ImGui::Begin(Loc::Window("WINDOW_VERSION_CONTROL", "Version Control"), &showVersionControlWindow)) {
        ImGui::End();
        if (wasOpen != showVersionControlWindow) saveEditorUserSettings();
        return;
    }
    panel.visible = true;

    const VcSnapshot& s = versionControl.snapshot();
    const bool busy = versionControl.isBusy();
    const Project& project = projectManager.currentProject;

    if (panel.boundRoot != s.repoRoot) {
        panel.boundRoot = s.repoRoot;
        panel.seenRemoteUrl = panel.seenUserName = panel.seenUserEmail = "\x01";  // force a copy below
        panel.unchecked.clear();
        panel.selectedCommit.clear();
        panel.categories.clear();
    }
    if (panel.seenRemoteUrl != s.remoteUrl) {
        panel.seenRemoteUrl = s.remoteUrl;
        CopyString(panel.remoteUrl, sizeof(panel.remoteUrl), s.remoteUrl);
    }
    if (panel.seenUserName != s.userName || panel.seenUserEmail != s.userEmail) {
        panel.seenUserName = s.userName;
        panel.seenUserEmail = s.userEmail;
        CopyString(panel.userName, sizeof(panel.userName), s.userName);
        CopyString(panel.userEmail, sizeof(panel.userEmail), s.userEmail);
    }

    auto drawBusyLine = [&]() {
        if (busy) DrawOperationProgress(versionControl, true);
    };

    auto drawBody = [&]() {
        if (!project.isLoaded) {
            ImGui::TextDisabled("Open a project to use version control.");
            return;
        }
        if (!s.probed || (!s.checked && s.gitAvailable)) {
            ImGui::TextDisabled("Reading the project's version control state...");
            return;
        }
        if (!s.gitAvailable) {
            ImGui::TextUnformatted("Git is not installed");
            ImGui::Spacing();
            ImGui::TextWrapped("Modularity backs projects up with Git. Install it, then press Retry. On Windows the "
                               "installer also brings Git Credential Manager (for signing in to GitHub and similar) "
                               "and Git LFS (for large textures and models).");
            ImGui::Spacing();
            if (PrimaryButton("Download Git")) OpenUrl("https://git-scm.com/downloads");
            ImGui::SameLine();
            if (ImGui::Button("Retry")) versionControl.probe();
            return;
        }
        if (!s.isRepo) {
            renderVersionControlSetup();
            drawBusyLine();
            return;
        }

        const int pendingCount = static_cast<int>(s.changes.size());
        char pendingLabel[64];
        std::snprintf(pendingLabel, sizeof(pendingLabel),
                      pendingCount > 0 ? "Pending Changes (%d)###vc_pending" : "Pending Changes###vc_pending", pendingCount);
        char incomingLabel[64];
        std::snprintf(incomingLabel, sizeof(incomingLabel),
                      s.behind > 0 ? "Incoming Changes (%d)###vc_incoming" : "Incoming Changes###vc_incoming", s.behind);

        if (ImGui::BeginTabBar("##VersionControlTabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
            if (ImGui::TabItemButton("Refresh", ImGuiTabItemFlags_Trailing)) versionControl.refresh(true);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-read the project's changes and history");
            if (ImGui::BeginTabItem(pendingLabel)) {
                drawBusyLine();
                renderVersionControlPending();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(incomingLabel)) {
                drawBusyLine();
                renderVersionControlIncoming();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Changesets")) {
                drawBusyLine();
                renderVersionControlHistory();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Branches")) {
                drawBusyLine();
                renderVersionControlBranches();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings")) {
                drawBusyLine();
                renderVersionControlSettings();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        renderVersionControlDiscardModal();
    };
    drawBody();

    ImGui::End();
    if (wasOpen != showVersionControlWindow) saveEditorUserSettings();
}

void Engine::renderVersionControlSetup() {
    VcPanelState& panel = Panel();
    const VcSnapshot& s = versionControl.snapshot();
    const bool busy = versionControl.isBusy();

    ImGui::TextUnformatted("Back up this project with Git");
    ImGui::Spacing();
    ImGui::TextWrapped("Check-ins save a snapshot of the project you can go back to. Connect a server "
                       "(GitHub, GitLab, or your own) and every check-in is uploaded, so you can open the "
                       "project on another computer with Clone from Git in the launcher.");
    ImGui::Spacing();
    ImGui::Checkbox("Skip files Modularity rebuilds (.gitignore)", &panel.setupGitignore);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Compiled scripts, build output, caches and your personal window layout stay\n"
                          "out of the backup. Installed packages are kept.");
    }
    // GitHub's free LFS allowance runs out fast, and without LFS the editor splits
    // anything over GitHub's 100 MB limit itself, so LFS starts off for GitHub.
    const bool gitHub = IsGitHubAddress(panel.setupRemote);
    if (!panel.setupLfsTouched) panel.setupLfs = !gitHub;
    ImGui::BeginDisabled(!s.lfsAvailable);
    bool useLfs = panel.setupLfs && s.lfsAvailable;
    if (ImGui::Checkbox("Store textures, models and audio with Git LFS", &useLfs)) {
        panel.setupLfs = useLfs;
        panel.setupLfsTouched = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", !s.lfsAvailable
            ? "Git LFS is not installed. Large files are stored directly; ones over GitHub's\n"
              "100 MB limit are split into parts automatically."
            : gitHub ? "Off by default for GitHub: its free LFS allowance (10 GB a month) runs out quickly.\n"
                       "Without LFS, files over 100 MB are split into parts automatically."
                     : "Large binary files are uploaded separately, which keeps the project quick to clone.\n"
                       "Your server needs Git LFS support for this.");
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Server address (optional)");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##vc_setup_remote", "https://github.com/you/your-game.git",
                             panel.setupRemote, sizeof(panel.setupRemote));
    ImGui::TextDisabled("Create an empty repository on the server and paste its address. You can add it later.");
    ImGui::Spacing();
    ImGui::BeginDisabled(busy);
    if (PrimaryButton("Set Up Version Control")) {
        VcInitOptions options;
        options.writeGitignore = panel.setupGitignore;
        options.useLfs = panel.setupLfs;
        options.remoteUrl = Trimmed(panel.setupRemote);
        versionControl.init(options);
    }
    ImGui::EndDisabled();
}

void Engine::renderVersionControlPending() {
    VcPanelState& panel = Panel();
    const VcSnapshot& s = versionControl.snapshot();
    const bool busy = versionControl.isBusy();
    Project& project = projectManager.currentProject;

    // Notices that change what Check In does.
    if (s.mergeInProgress) {
        int conflicts = 0;
        for (const VcChange& change : s.changes) conflicts += change.kind == VcChangeKind::Conflicted ? 1 : 0;
        const std::string text = conflicts > 0
            ? "Merging the server's changes: " + std::to_string(conflicts) + " file(s) changed on both sides. "
              "Right-click each to keep your version or take the server's, then check in."
            : std::string("Merge ready: check in to finish it. Every change is included.");
        Banner(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), text.c_str());
        ImGui::BeginDisabled(busy);
        if (ImGui::SmallButton("Cancel Merge")) versionControl.abortMerge();
        ImGui::EndDisabled();
    }
    if (project.hasUnsavedChanges) {
        Banner(ImVec4(0.95f, 0.80f, 0.40f, 1.0f), "The open scene has unsaved changes. Save it to include them.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Save Scene")) saveCurrentScene();
    }
    const bool identityMissing = s.userName.empty() || s.userEmail.empty();
    if (identityMissing) {
        Banner(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "Check-ins are signed with a name and email. Set them once:");
        const float fieldW = std::max(120.0f, (ImGui::GetContentRegionAvail().x - 260.0f) * 0.5f);
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputTextWithHint("##vc_id_name", "Your name", panel.userName, sizeof(panel.userName));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputTextWithHint("##vc_id_email", "Email", panel.userEmail, sizeof(panel.userEmail));
        ImGui::SameLine();
        ImGui::Checkbox("All projects", &panel.identityGlobal);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || Trimmed(panel.userName).empty() || Trimmed(panel.userEmail).empty());
        if (ImGui::Button("Save##vc_id_save")) {
            versionControl.setIdentity(Trimmed(panel.userName), Trimmed(panel.userEmail), panel.identityGlobal);
        }
        ImGui::EndDisabled();
    }

    // Tick state per group, for the headers.
    int groupTotal[static_cast<int>(PendingGroup::Count)] = {};
    int groupChecked[static_cast<int>(PendingGroup::Count)] = {};
    int totalChecked = 0;
    for (const VcChange& change : s.changes) {
        const int g = static_cast<int>(GroupOf(change.kind));
        ++groupTotal[g];
        if (!panel.unchecked.count(change.path)) {
            ++groupChecked[g];
            ++totalChecked;
        }
    }
    const int total = static_cast<int>(s.changes.size());

    // Flat row list so the clipper can skip what is off screen; a first check-in
    // can easily list thousands of files.
    panel.rows.clear();
    for (int g = 0; g < static_cast<int>(PendingGroup::Count); ++g) {
        if (groupTotal[g] == 0) continue;
        PendingRow header;
        header.header = true;
        header.group = static_cast<PendingGroup>(g);
        panel.rows.push_back(header);
        if (!panel.groupOpen[g]) continue;
        for (int i = 0; i < total; ++i) {
            if (static_cast<int>(GroupOf(s.changes[i].kind)) != g) continue;
            PendingRow row;
            row.group = header.group;
            row.change = i;
            panel.rows.push_back(row);
        }
    }

    auto setChecked = [&](const std::string& path, bool checked) {
        if (checked) panel.unchecked.erase(path);
        else panel.unchecked.insert(path);
    };
    auto checkedPaths = [&]() {
        std::vector<std::string> paths;
        for (const VcChange& change : s.changes) {
            if (!panel.unchecked.count(change.path)) paths.push_back(change.path);
        }
        return paths;
    };
    auto iconFor = [&](const std::string& repoPath) -> std::pair<ImTextureID, bool> {
        auto it = panel.categories.find(repoPath);
        if (it == panel.categories.end()) {
            std::error_code ec;
            const fs::directory_entry entry(s.repoRoot / FromUtf8(repoPath), ec);
            it = panel.categories.emplace(repoPath, fileBrowser.getFileCategory(entry)).first;
        }
        const char* iconPath = AssetCategoryIconPath(it->second);
        if (rendererInitialized) {
            if (Texture* icon = renderer.getTexture(iconPath, MaterialProperties::TextureFilter::Point); icon && icon->GetID()) {
                return {static_cast<ImTextureID>(icon->GetID()), true};
            }
        }
        if (usingVulkan() && vulkanRendererInitialized && vulkanRenderer) {
            return {vulkanRenderer->getOrCreateUIImage(iconPath), false};
        }
        return {static_cast<ImTextureID>(0), false};
    };

    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 3.0f + 16.0f;
    const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (total == 0) {
        ImGui::Spacing();
        ImGui::TextDisabled(s.unborn ? "Nothing to check in yet: the project folder is empty."
                                     : "No pending changes. Everything is checked in.");
    } else if (ImGui::BeginTable("##vc_pending", 3, tableFlags, ImVec2(0.0f, -footerHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Date modified", ImGuiTableColumnFlags_WidthStretch, 1.4f);

        // Header row with the select-all box, like the column header in Unity's list.
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        ImGui::TableSetColumnIndex(0);
        {
            bool all = totalChecked == total;
            const bool mixed = totalChecked > 0 && totalChecked < total;
            if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            if (ImGui::Checkbox("##vc_all", &all)) {
                for (const VcChange& change : s.changes) setChecked(change.path, all);
            }
            if (mixed) ImGui::PopItemFlag();
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            char header[64];
            std::snprintf(header, sizeof(header), "Item (%d/%d selected)", totalChecked, total);
            ImGui::TableHeader(header);
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::TableHeader("Status");
        ImGui::TableSetColumnIndex(2);
        ImGui::TableHeader("Date modified");

        const float rowHeight = ImGui::GetFrameHeight();
        const float iconSize = ImFloor(rowHeight - 4.0f);
        const float arrowWidth = ImGui::GetFontSize();
        // Files sit one step in from their group's checkbox.
        const float itemIndent = arrowWidth + ImGui::GetStyle().ItemSpacing.x + rowHeight * 0.75f;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(panel.rows.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const PendingRow& row = panel.rows[r];
                ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                ImGui::PushID(r);
                ImGui::TableSetColumnIndex(0);
                const int g = static_cast<int>(row.group);
                if (row.header) {
                    const ImVec2 start = ImGui::GetCursorScreenPos();
                    ImGui::SetNextItemAllowOverlap();
                    if (ImGui::Selectable("##group", false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, rowHeight))) {
                        panel.groupOpen[g] = !panel.groupOpen[g];
                    }
                    ImGui::RenderArrow(ImGui::GetWindowDrawList(),
                                       ImVec2(start.x, start.y + (rowHeight - ImGui::GetFontSize()) * 0.5f),
                                       ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                       panel.groupOpen[g] ? ImGuiDir_Down : ImGuiDir_Right, 0.8f);
                    ImGui::SetCursorScreenPos(ImVec2(start.x + arrowWidth, start.y));
                    bool all = groupChecked[g] == groupTotal[g];
                    const bool mixed = groupChecked[g] > 0 && groupChecked[g] < groupTotal[g];
                    if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
                    if (ImGui::Checkbox("##groupcheck", &all)) {
                        for (const VcChange& change : s.changes) {
                            if (static_cast<int>(GroupOf(change.kind)) == g) setChecked(change.path, all);
                        }
                    }
                    if (mixed) ImGui::PopItemFlag();
                    ImGui::SameLine();
                    ImGui::Text("%s (%d/%d selected)", GroupLabel(row.group), groupChecked[g], groupTotal[g]);
                    ImGui::PopID();
                    continue;
                }

                const VcChange& change = s.changes[row.change];
                const std::string shown = DisplayPath(change.path, s.projectPrefix);
                const fs::path absolute = s.repoRoot / FromUtf8(change.path);
                const ImVec2 start = ImGui::GetCursorScreenPos();
                ImGui::SetNextItemAllowOverlap();
                if (ImGui::Selectable("##row", panel.selectedPath == change.path,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                                      ImVec2(0.0f, rowHeight))) {
                    panel.selectedPath = change.path;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        std::error_code ec;
                        if (fs::exists(absolute, ec)) {
                            fileBrowser.navigateTo(absolute.parent_path());
                            fileBrowser.selectedFile = absolute;
                        }
                    }
                }
                if (ImGui::BeginPopupContextItem("##rowmenu")) {
                    panel.selectedPath = change.path;
                    std::error_code ec;
                    const bool exists = fs::exists(absolute, ec);
                    if (change.kind == VcChangeKind::Conflicted) {
                        ImGui::BeginDisabled(busy);
                        if (ImGui::MenuItem("Keep Mine")) versionControl.resolve({change.path}, true);
                        if (ImGui::MenuItem("Take Theirs")) versionControl.resolve({change.path}, false);
                        ImGui::EndDisabled();
                        ImGui::Separator();
                    }
                    ImGui::BeginDisabled(busy || isPlaying);
                    if (ImGui::MenuItem("Undo Changes...")) {
                        panel.discardPaths = {change.path};
                        panel.requestDiscardModal = true;
                    }
                    ImGui::EndDisabled();
                    if (change.kind == VcChangeKind::Untracked) {
                        const std::string rel = DisplayPath(change.path, s.projectPrefix);
                        const std::string ext = FromUtf8(change.path).extension().string();
                        ImGui::BeginDisabled(busy);
                        if (ImGui::MenuItem("Ignore This File")) versionControl.ignore(IgnorePatternForFile(rel));
                        if (!ext.empty() && ImGui::MenuItem(("Ignore All *" + ext + " Files").c_str())) {
                            versionControl.ignore("*" + ext);
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::Separator();
                    ImGui::BeginDisabled(!exists);
                    if (ImGui::MenuItem("Reveal in Project")) {
                        fileBrowser.navigateTo(absolute.parent_path());
                        fileBrowser.selectedFile = absolute;
                    }
                    if (ImGui::MenuItem("Show in File Manager")) OpenExternal(absolute, true);
                    ImGui::EndDisabled();
                    if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(shown.c_str());
                    ImGui::EndPopup();
                }

                ImGui::SetCursorScreenPos(ImVec2(start.x + itemIndent, start.y));
                bool checked = !panel.unchecked.count(change.path);
                if (ImGui::Checkbox("##check", &checked)) setChecked(change.path, checked);
                ImGui::SameLine();
                const auto icon = iconFor(change.path);
                if (icon.first != static_cast<ImTextureID>(0)) {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowHeight - iconSize) * 0.5f);
                    ImGui::Image(icon.first, ImVec2(iconSize, iconSize),
                                 icon.second ? ImVec2(0, 1) : ImVec2(0, 0), icon.second ? ImVec2(1, 0) : ImVec2(1, 1));
                    ImGui::SameLine();
                    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, start.y));
                }
                ImGui::AlignTextToFramePadding();
                if (!change.oldPath.empty()) {
                    ImGui::Text("%s  <-  %s", shown.c_str(), DisplayPath(change.oldPath, s.projectPrefix).c_str());
                } else {
                    ImGui::TextUnformatted(shown.c_str());
                }
                if (!change.path.empty() && change.path.back() == '/' && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("This folder is a Git repository of its own. It will be recorded as a\n"
                                      "link to that repository, not as its files.");
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                if (change.split) {
                    ImGui::TextColored(KindColor(change.kind), "%s (split)", KindLabel(change.kind));
                    if (ImGui::IsItemHovered() && s.splitPartSize > 0) {
                        ImGui::SetTooltip("Over the server's %s limit, so it is checked in as parts of %s.\n"
                                          "Every other computer puts it back together after downloading.",
                                          MegabyteText(s.splitLimit).c_str(), MegabyteText(s.splitPartSize).c_str());
                    }
                } else {
                    ImGui::TextColored(KindColor(change.kind), "%s", KindLabel(change.kind));
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::AlignTextToFramePadding();
                if (change.modified > 0) ImGui::TextUnformatted(FormatWhen(change.modified).c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    // Footer: comment, Check In, Undo.
    ImGui::Separator();
    const float avatar = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 center(pos.x + avatar * 0.5f, pos.y + avatar * 0.5f);
        draw->AddCircleFilled(center, avatar * 0.5f, ImGui::GetColorU32(ImGuiCol_FrameBgActive));
        // First character, not first byte: names are UTF-8.
        std::string initial = "?";
        if (!s.userName.empty()) {
            const unsigned char lead = static_cast<unsigned char>(s.userName[0]);
            const size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
            initial = s.userName.substr(0, length);
        }
        const ImVec2 textSize = ImGui::CalcTextSize(initial.c_str());
        draw->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_Text), initial.c_str());
        ImGui::Dummy(ImVec2(avatar, avatar));
        if (ImGui::IsItemHovered() && !s.userName.empty()) {
            ImGui::SetTooltip("%s <%s>", s.userName.c_str(), s.userEmail.c_str());
        }
    }
    ImGui::SameLine();
    const float commentHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
    // InputTextWithHint is single-line only; InputTextEx takes a hint for multiline too.
    ImGui::InputTextEx("##vc_comment", "Add a comment to your check-in...", panel.comment,
                       static_cast<int>(sizeof(panel.comment)), ImVec2(-1.0f, commentHeight),
                       ImGuiInputTextFlags_Multiline);

    const bool hasComment = !Trimmed(panel.comment).empty();
    const bool canCheckIn = !busy && !identityMissing && hasComment && (s.mergeInProgress || totalChecked > 0);
    const char* blockReason = busy ? "Wait for the current operation to finish."
        : identityMissing ? "Set your name and email first."
        : !(s.mergeInProgress || totalChecked > 0) ? "Tick the files to check in."
        : !hasComment ? "Describe what changed in the comment box."
        : "";
    const bool hasRemote = !s.remoteName.empty();

    ImGui::BeginDisabled(!canCheckIn);
    if (PrimaryButton("Check In Changes")) {
        versionControl.checkIn(checkedPaths(), panel.comment, true);
    }
    ImGui::EndDisabled();
    TooltipIfDisabled(canCheckIn ? (hasRemote ? "Save a snapshot and upload it to the server" : "Save a snapshot on this computer")
                                 : blockReason);
    if (hasRemote) {
        ImGui::SameLine(0.0f, 1.0f);
        ImGui::BeginDisabled(!canCheckIn);
        if (ImGui::ArrowButton("##vc_checkin_more", ImGuiDir_Down)) ImGui::OpenPopup("##vc_checkin_menu");
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("##vc_checkin_menu")) {
            if (ImGui::MenuItem("Check In Without Uploading")) {
                versionControl.checkIn(checkedPaths(), panel.comment, false);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || isPlaying || totalChecked == 0);
    if (ImGui::Button("Undo")) {
        panel.discardPaths = checkedPaths();
        panel.requestDiscardModal = true;
    }
    ImGui::EndDisabled();
    TooltipIfDisabled(isPlaying ? "Leave play mode first." : "Put the ticked files back to their last check-in");
    if (s.ahead > 0 && hasRemote) {
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        char uploadLabel[48];
        std::snprintf(uploadLabel, sizeof(uploadLabel), "Upload %d Changeset%s", s.ahead, s.ahead == 1 ? "" : "s");
        if (ImGui::Button(uploadLabel)) versionControl.push();
        ImGui::EndDisabled();
    }
}

void Engine::renderVersionControlDiscardModal() {
    VcPanelState& panel = Panel();
    const VcSnapshot& s = versionControl.snapshot();
    if (panel.requestDiscardModal) {
        panel.requestDiscardModal = false;
        panel.showDiscardModal = true;
        playEditorSound(EditorSoundCue::DialogOpen, 0.95f);
        ImGui::OpenPopup("Undo changes?");
    }
    if (!beginCardModal("Undo changes?", 0.0f, &panel.showDiscardModal)) return;

    size_t newFiles = 0;
    bool touchesScene = false;
    std::error_code ec;
    const Project& project = projectManager.currentProject;
    const fs::path scenePath = fs::weakly_canonical(project.getSceneFilePath(project.currentSceneName), ec);
    for (const std::string& path : panel.discardPaths) {
        for (const VcChange& change : s.changes) {
            if (change.path != path) continue;
            if (change.kind == VcChangeKind::Untracked || change.kind == VcChangeKind::Added ||
                change.kind == VcChangeKind::Renamed) {
                ++newFiles;
            }
            break;
        }
        if (fs::weakly_canonical(s.repoRoot / FromUtf8(path), ec) == scenePath) touchesScene = true;
    }
    const std::string summary = std::to_string(panel.discardPaths.size()) + " file(s) go back to how they were at the last check-in.";
    cardModalText(summary.c_str());
    if (newFiles > 0) {
#ifdef _WIN32
        const std::string newText = std::to_string(newFiles) + " new file(s) are moved to the Recycle Bin.";
#else
        const std::string newText = std::to_string(newFiles) + " new file(s) are deleted.";
#endif
        cardModalText(newText.c_str());
    }
    if (touchesScene) cardModalText("The open scene reloads, and any unsaved edits to it are lost.");
    if (cardModalButton("Cancel", CardButtonKind::Neutral, 0, 2)) {
        panel.showDiscardModal = false;
        ImGui::CloseCurrentPopup();
    }
    if (cardModalButton("Undo Changes", CardButtonKind::Danger, 1, 2)) {
        versionControl.discard(panel.discardPaths);
        panel.discardPaths.clear();
        panel.showDiscardModal = false;
        ImGui::CloseCurrentPopup();
    }
    endCardModal();
}

void Engine::renderVersionControlIncoming() {
    const VcSnapshot& s = versionControl.snapshot();
    const bool busy = versionControl.isBusy();
    const bool hasRemote = !s.remoteName.empty();
    const bool sceneDirty = projectManager.currentProject.hasUnsavedChanges;

    ImGui::BeginDisabled(busy || !hasRemote);
    if (ImGui::Button("Check for Changes")) versionControl.fetch(false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!versionControl.lastBackgroundError().empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "Last check failed: %s",
                           versionControl.lastBackgroundError().c_str());
    } else if (s.lastFetch > 0) {
        ImGui::TextDisabled("Last checked %s", FormatRelative(s.lastFetch).c_str());
    }
    ImGui::Separator();

    if (!hasRemote) {
        ImGui::TextWrapped("No server is connected, so there is nothing to receive. Add one in the Settings tab "
                           "to back the project up and open it on other computers.");
        return;
    }
    if (s.upstream.empty()) {
        if (s.unborn) {
            ImGui::TextDisabled("Check something in first, then it can be uploaded.");
            return;
        }
        ImGui::TextWrapped("The branch \"%s\" is not on the server yet.", s.branch.c_str());
        ImGui::BeginDisabled(busy);
        if (PrimaryButton("Upload Branch")) versionControl.push();
        ImGui::EndDisabled();
        return;
    }
    if (s.behind == 0) {
        ImGui::TextDisabled("You're up to date with the server.");
        if (s.ahead > 0) {
            ImGui::Text("%d changeset%s on this computer %s not uploaded yet.", s.ahead, s.ahead == 1 ? "" : "s",
                        s.ahead == 1 ? "is" : "are");
            ImGui::BeginDisabled(busy);
            if (PrimaryButton("Upload")) versionControl.push();
            ImGui::EndDisabled();
        }
        return;
    }

    const char* blockReason = isPlaying ? "Leave play mode first."
        : sceneDirty ? "Save the open scene first; the incoming version may replace it."
        : busy ? "Wait for the current operation to finish."
        : "";
    const bool blocked = blockReason[0] != '\0';
    if (s.ahead > 0) {
        Banner(ImVec4(1.0f, 0.72f, 0.35f, 1.0f),
               "You and the server both have new check-ins. Merging combines them; files changed on both "
               "sides will ask which version to keep.");
        ImGui::BeginDisabled(blocked);
        if (PrimaryButton("Merge Changes")) versionControl.merge();
        ImGui::EndDisabled();
        TooltipIfDisabled(blockReason);
    } else {
        ImGui::BeginDisabled(blocked);
        if (PrimaryButton("Get Changes")) versionControl.pull();
        ImGui::EndDisabled();
        TooltipIfDisabled(blocked ? blockReason : "Update the project to the server's latest check-in");
    }
    if (sceneDirty) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Save Scene")) saveCurrentScene();
    }

    const float half = std::max(80.0f, ImGui::GetContentRegionAvail().y * 0.5f);
    if (ImGui::BeginTable("##vc_incoming_commits", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, half))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (const VcCommit& commit : s.incoming) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(commit.subject.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(commit.author.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(FormatWhen(commit.time).c_str());
        }
        ImGui::EndTable();
    }
    if (ImGui::BeginTable("##vc_incoming_files", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Incoming file", ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(s.incomingFiles.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const VcChange& change = s.incomingFiles[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(FriendlyPath(change.path, s.projectPrefix).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(KindColor(change.kind), "%s", KindLabel(change.kind));
            }
        }
        ImGui::EndTable();
    }
}

void Engine::renderVersionControlHistory() {
    VcPanelState& panel = Panel();
    const VcSnapshot& s = versionControl.snapshot();
    if (s.history.empty()) {
        ImGui::TextDisabled(s.unborn ? "No check-ins yet." : "No history to show.");
        return;
    }

    const float listHeight = std::max(80.0f, ImGui::GetContentRegionAvail().y * 0.6f);
    if (ImGui::BeginTable("##vc_history", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, listHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Changeset", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000000000 (local)").x);
        ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(s.history.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const VcCommit& commit = s.history[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableSetColumnIndex(0);
                const std::string label = commit.local ? commit.shortHash + " (local)" : commit.shortHash;
                if (ImGui::Selectable(label.c_str(), panel.selectedCommit == commit.hash, ImGuiSelectableFlags_SpanAllColumns)) {
                    panel.selectedCommit = commit.hash;
                }
                if (commit.local && ImGui::IsItemHovered()) ImGui::SetTooltip("Only on this computer: not uploaded yet");
                if (ImGui::BeginPopupContextItem("##commitmenu")) {
                    if (ImGui::MenuItem("Copy Changeset ID")) ImGui::SetClipboardText(commit.hash.c_str());
                    if (ImGui::MenuItem("Copy Comment")) ImGui::SetClipboardText(commit.subject.c_str());
                    ImGui::EndPopup();
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(commit.subject.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(commit.author.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(FormatWhen(commit.time).c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    if (panel.selectedCommit.empty()) {
        ImGui::TextDisabled("Select a changeset to see the files it changed.");
        return;
    }
    const std::vector<VcChange>* files = versionControl.commitFiles(panel.selectedCommit);
    if (!files) {
        versionControl.loadCommitFiles(panel.selectedCommit);
        ImGui::TextDisabled("Loading files...");
        return;
    }
    if (ImGui::BeginTable("##vc_history_files", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(files->size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const VcChange& change = (*files)[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(FriendlyPath(change.path, s.projectPrefix).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(KindColor(change.kind), "%s", KindLabel(change.kind));
            }
        }
        ImGui::EndTable();
    }
}

void Engine::renderVersionControlBranches() {
    VcPanelState& panel = Panel();
    const VcSnapshot& s = versionControl.snapshot();
    const bool busy = versionControl.isBusy();
    const char* switchBlock = isPlaying ? "Leave play mode first."
        : projectManager.currentProject.hasUnsavedChanges ? "Save the open scene first; switching replaces project files."
        : busy ? "Wait for the current operation to finish."
        : "";

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("New Branch...")) {
        panel.newBranch[0] = '\0';
        ImGui::OpenPopup("##vc_new_branch");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Current: %s", s.detached ? "(detached)" : s.branch.c_str());
    if (ImGui::BeginPopup("##vc_new_branch")) {
        ImGui::TextUnformatted("New branch from the current check-in");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(260.0f);
        const bool submitted = ImGui::InputTextWithHint("##vc_branch_name", "feature/new-level", panel.newBranch,
                                                        sizeof(panel.newBranch), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Checkbox("Switch to it", &panel.newBranchSwitch);
        const std::string name = Trimmed(panel.newBranch);
        ImGui::BeginDisabled(name.empty());
        if (ImGui::Button("Create") || (submitted && !name.empty())) {
            versionControl.createBranch(name, panel.newBranchSwitch);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::Separator();

    if (!ImGui::BeginTable("##vc_branches", 4,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Server", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("Last check-in", ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableHeadersRow();
    for (int pass = 0; pass < 2; ++pass) {
        const bool remotePass = pass == 1;
        bool any = false;
        for (size_t i = 0; i < s.branches.size(); ++i) {
            const VcBranch& branch = s.branches[i];
            if (branch.remote != remotePass) continue;
            if (!any && remotePass) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("On the server");
            }
            any = true;
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableSetColumnIndex(0);
            const std::string label = branch.current ? branch.name + "  (current)" : branch.name;
            if (branch.current) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 0.60f, 1.0f));
            ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
            if (branch.current) ImGui::PopStyleColor();
            if (!branch.current && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                switchBlock[0] == '\0') {
                versionControl.switchBranch(branch.name, branch.remote);
            }
            if (ImGui::BeginPopupContextItem("##branchmenu")) {
                ImGui::BeginDisabled(branch.current || switchBlock[0] != '\0');
                if (ImGui::MenuItem("Switch to This Branch")) versionControl.switchBranch(branch.name, branch.remote);
                ImGui::EndDisabled();
                TooltipIfDisabled(switchBlock);
                if (!branch.remote) {
                    ImGui::BeginDisabled(branch.current || busy);
                    if (ImGui::MenuItem("Delete Branch")) versionControl.deleteBranch(branch.name);
                    ImGui::EndDisabled();
                }
                if (ImGui::MenuItem("Copy Name")) ImGui::SetClipboardText(branch.name.c_str());
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(1);
            if (!branch.remote) {
                if (branch.upstream.empty()) ImGui::TextDisabled("not uploaded");
                else if (branch.track.empty()) ImGui::TextDisabled("in sync");
                else ImGui::TextUnformatted(branch.track.c_str());
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(branch.subject.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(FormatWhen(branch.time).c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void Engine::renderVersionControlSettings() {
    VcPanelState& panel = Panel();
    const VcSnapshot& s = versionControl.snapshot();
    const bool busy = versionControl.isBusy();
    const float labelW = 150.0f;

    ImGui::SeparatorText("Server");
    ImGui::TextWrapped("Where check-ins are uploaded. Create an empty repository on GitHub, GitLab or your own "
                       "server and paste its address; signing in happens the first time you upload.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Address");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputTextWithHint("##vc_remote", "https://github.com/you/your-game.git", panel.remoteUrl, sizeof(panel.remoteUrl));
    ImGui::SameLine();
    const std::string url = Trimmed(panel.remoteUrl);
    ImGui::BeginDisabled(busy || url.empty() || url == s.remoteUrl);
    if (ImGui::Button("Connect", ImVec2(-1.0f, 0.0f))) versionControl.setRemote(url);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Your identity");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##vc_settings_name", panel.userName, sizeof(panel.userName));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Email");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##vc_settings_email", panel.userEmail, sizeof(panel.userEmail));
    ImGui::Dummy(ImVec2(labelW - ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::SameLine(labelW);
    ImGui::Checkbox("Use for all projects", &panel.identityGlobal);
    ImGui::SameLine();
    const std::string name = Trimmed(panel.userName);
    const std::string email = Trimmed(panel.userEmail);
    ImGui::BeginDisabled(busy || name.empty() || email.empty() || (name == s.userName && email == s.userEmail));
    if (ImGui::Button("Save##vc_settings_identity")) versionControl.setIdentity(name, email, panel.identityGlobal);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Repository");
    auto row = [&](const char* label, const std::string& value) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelW);
        ImGui::TextWrapped("%s", value.c_str());
    };
    row("Folder", PathToUtf8(s.repoRoot));
    if (!s.projectPrefix.empty()) row("Project inside", s.projectPrefix);
    row("Git", s.gitVersion);
    std::string largeFiles;
    if (s.lfsTracked) {
        largeFiles = "Stored with Git LFS.";
    } else {
        largeFiles = "Stored directly in Git.";
        if (s.splitLimit > 0) {
            largeFiles += " Files over " + MegabyteText(s.splitLimit) + " are checked in as parts of " +
                          MegabyteText(s.splitPartSize) + " and put back together after every download.";
        }
    }
    row("Large files", largeFiles);
    if (s.lfsTracked) {
        ImGui::Dummy(ImVec2(labelW - ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::SameLine(labelW);
        ImGui::BeginDisabled(busy || !s.lfsAvailable);
        if (ImGui::Button("Stop Using Git LFS...")) {
            panel.requestLfsModal = true;
        }
        ImGui::EndDisabled();
        TooltipIfDisabled(!s.lfsAvailable ? "Git LFS is not installed, so its files can't be converted."
                                          : "Store large files directly in Git, for servers whose LFS allowance is\n"
                                            "used up (GitHub's free one runs out quickly).");
    }
    ImGui::Spacing();
    if (ImGui::Button("Open Repository Folder")) OpenExternal(s.repoRoot, false);
    ImGui::SameLine();
    const fs::path ignoreFile = projectManager.currentProject.projectPath / ".gitignore";
    std::error_code ec;
    ImGui::BeginDisabled(!fs::exists(ignoreFile, ec));
    if (ImGui::Button("Edit .gitignore")) OpenExternal(ignoreFile, false);
    ImGui::EndDisabled();

    if (panel.requestLfsModal) {
        panel.requestLfsModal = false;
        panel.showLfsModal = true;
        playEditorSound(EditorSoundCue::DialogOpen, 0.95f);
        ImGui::OpenPopup("Stop using Git LFS?");
    }
    if (beginCardModal("Stop using Git LFS?", 0.0f, &panel.showLfsModal)) {
        cardModalText("Textures, models and audio will be stored directly in Git instead of Git LFS, so uploads "
                      "no longer count against an LFS allowance.");
        cardModalText("Check-ins that aren't uploaded yet are rewritten to hold the real files. Uploaded ones "
                      "stay as they are.");
        if (s.splitLimit > 0) {
            const std::string splitText = "Files over " + MegabyteText(s.splitLimit) +
                                          " are split into parts automatically from now on.";
            cardModalText(splitText.c_str());
        }
        cardModalText("Pending changes to tracked files have to be checked in first (without uploading is fine).");
        if (cardModalButton("Cancel", CardButtonKind::Neutral, 0, 2)) {
            panel.showLfsModal = false;
            ImGui::CloseCurrentPopup();
        }
        if (cardModalButton("Stop Using LFS", CardButtonKind::Primary, 1, 2)) {
            versionControl.disableLfs();
            panel.showLfsModal = false;
            ImGui::CloseCurrentPopup();
        }
        endCardModal();
    }
}

#pragma endregion

#pragma region Clone Dialog

void Engine::renderCloneProjectDialog() {
    VcPanelState& panel = Panel();
    const VcSnapshot& s = versionControl.snapshot();
    if (panel.cloneParent[0] == '\0') {
        CopyString(panel.cloneParent, sizeof(panel.cloneParent),
                   projectManager.defaultProjectLocation[0] != '\0'
                       ? std::string(projectManager.defaultProjectLocation)
                       : PathToUtf8(fs::current_path() / "Projects"));
    }

    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (mainViewport) {
        ImGui::SetNextWindowViewport(mainViewport->ID);
        ImGui::SetNextWindowPos(mainViewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    }
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Clone from Git", &projectManager.showCloneProjectDialog,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    const bool running = panel.cloneRunning;

    ImGui::TextWrapped("Download a project you backed up from another computer. Sign-in happens through Git "
                       "if the server asks for it.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Repository address");
    ImGui::SetNextItemWidth(520.0f);
    if (ImGui::InputTextWithHint("##clone_url", "https://github.com/you/your-game.git", panel.cloneUrl,
                                 sizeof(panel.cloneUrl)) && !panel.cloneFolderEdited) {
        CopyString(panel.cloneFolder, sizeof(panel.cloneFolder), RepoNameFromUrl(Trimmed(panel.cloneUrl)));
    }
    ImGui::TextUnformatted("Save in folder");
    ImGui::SetNextItemWidth(520.0f);
    ImGui::InputText("##clone_parent", panel.cloneParent, sizeof(panel.cloneParent));
    ImGui::TextUnformatted("Project folder name");
    ImGui::SetNextItemWidth(520.0f);
    if (ImGui::InputText("##clone_folder", panel.cloneFolder, sizeof(panel.cloneFolder))) panel.cloneFolderEdited = true;

    const std::string url = Trimmed(panel.cloneUrl);
    const std::string folder = Trimmed(panel.cloneFolder);
    const fs::path destination = FromUtf8(Trimmed(panel.cloneParent)) / FromUtf8(folder);
    if (!folder.empty()) ImGui::TextDisabled("%s", PathToUtf8(destination).c_str());

    if (s.probed && !s.gitAvailable) {
        ImGui::Spacing();
        Banner(ImVec4(1.0f, 0.5f, 0.45f, 1.0f), "Git is not installed. Install it from git-scm.com, then try again.");
        if (ImGui::SmallButton("Download Git")) OpenUrl("https://git-scm.com/downloads");
        ImGui::SameLine();
        if (ImGui::SmallButton("Retry")) versionControl.probe();
    }
    if (!panel.cloneError.empty()) {
        ImGui::Spacing();
        Banner(ImVec4(1.0f, 0.5f, 0.45f, 1.0f), panel.cloneError.c_str());
    }
    if (running) {
        ImGui::Spacing();
        ImGui::PushItemWidth(520.0f);
        DrawOperationProgress(versionControl, false);
        ImGui::PopItemWidth();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (running) {
        if (ImGui::Button("Cancel", ImVec2(100, 0))) versionControl.cancel();
    } else {
        if (ImGui::Button("Close", ImVec2(100, 0))) projectManager.showCloneProjectDialog = false;
        ImGui::SameLine();
        ImGui::BeginDisabled(url.empty() || folder.empty() || !s.gitAvailable);
        if (PrimaryButton("Clone and Open", ImVec2(140, 0))) {
            playEditorSound(EditorSoundCue::Confirm, 0.95f);
            panel.cloneError.clear();
            panel.cloneRunning = true;
            versionControl.clone(url, destination);
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

#pragma endregion

#endif // !MODULARITY_RUNTIME_ONLY
