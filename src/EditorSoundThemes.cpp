#include "EditorSoundThemes.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace Modularity::EditorSounds {
namespace {

constexpr const char* kCueKeys[] = {
    "tick",
    "section_switch",
    "confirm",
    "dialog_open",
    "warning",
    "error",
    "compile_start",
    "compile_success",
    "script_assigned",
    "package_installed",
    "button",
    "button_disabled",
    "tab",
    "toggle_on",
    "toggle_off",
    "slide_open",
    "slide_close",
    "popup_open",
    "popup_close",
    "item_select",
    "hover",
};
static_assert(sizeof(kCueKeys) / sizeof(kCueKeys[0]) == static_cast<size_t>(Cue::Count),
              "every Cue needs a manifest key");

constexpr float kMaxGain = 4.0f;

std::string Trim(const std::string& s) {
    const size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Manifests and preferences are UTF-8; fs::path's narrow constructor uses the ANSI code page on
// Windows, so round-trip explicitly. u8string() is std::string before C++20, std::u8string after.
std::string Utf8FromPath(const fs::path& path) {
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

fs::path PathFromUtf8(const std::string& utf8) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return fs::u8path(utf8);
#endif
}

int CueIndexForKey(const std::string& key) {
    for (size_t i = 0; i < static_cast<size_t>(Cue::Count); ++i) {
        if (key == kCueKeys[i]) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

const char* CueKey(Cue cue) {
    const size_t index = static_cast<size_t>(cue);
    return index < static_cast<size_t>(Cue::Count) ? kCueKeys[index] : "";
}

void ThemeLibrary::refresh() {
    loadedThemes.clear();
    infoList.clear();
    warningList.clear();
    scanned = true;

    std::error_code ec;
    const fs::path root(kThemesRoot);
    std::vector<fs::path> themeDirs;
    if (fs::is_directory(root, ec)) {
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entryEc;
            if (it->is_directory(entryEc) && fs::is_regular_file(it->path() / kManifestName, entryEc)) {
                themeDirs.push_back(it->path());
            }
        }
    }
    if (themeDirs.empty()) {
        warningList.push_back(std::string("No sound themes found in ") + kThemesRoot +
                              " (each theme folder needs a " + kManifestName + ").");
    }
    std::sort(themeDirs.begin(), themeDirs.end());

    for (const fs::path& dir : themeDirs) {
        Theme theme;
        theme.info.id = Utf8FromPath(dir.filename());
        const std::string where = theme.info.id + "/" + kManifestName;

        std::ifstream manifest(dir / kManifestName);
        std::string line;
        int lineNumber = 0;
        while (std::getline(manifest, line)) {
            ++lineNumber;
            line = Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;

            const size_t eq = line.find('=');
            if (eq == std::string::npos) {
                warningList.push_back(where + ":" + std::to_string(lineNumber) +
                                      ": expected \"key = value\".");
                continue;
            }
            const std::string key = Lower(Trim(line.substr(0, eq)));
            const std::string value = Trim(line.substr(eq + 1));

            if (key == "name") {
                theme.info.displayName = value;
                continue;
            }
            if (key == "description") {
                theme.info.description = value;
                continue;
            }

            static const std::string kGainSuffix = ".gain";
            const bool isGain = key.size() > kGainSuffix.size() &&
                                key.compare(key.size() - kGainSuffix.size(), kGainSuffix.size(),
                                            kGainSuffix) == 0;
            const int cueIndex =
                CueIndexForKey(isGain ? key.substr(0, key.size() - kGainSuffix.size()) : key);
            if (cueIndex < 0) {
                warningList.push_back(where + ":" + std::to_string(lineNumber) +
                                      ": unknown key \"" + key + "\".");
                continue;
            }

            CueSounds& sounds = theme.cues[static_cast<size_t>(cueIndex)];
            if (isGain) {
                try {
                    sounds.gain = std::clamp(std::stof(value), 0.0f, kMaxGain);
                } catch (...) {
                    warningList.push_back(where + ":" + std::to_string(lineNumber) +
                                          ": gain \"" + value + "\" is not a number.");
                }
                continue;
            }

            sounds.files.clear();
            sounds.silent = value.empty() || Lower(value) == "none";
            if (sounds.silent) continue;

            // '|' separates variants: it cannot appear in a Windows file name, commas can.
            size_t start = 0;
            while (start <= value.size()) {
                size_t bar = value.find('|', start);
                if (bar == std::string::npos) bar = value.size();
                const std::string file = Trim(value.substr(start, bar - start));
                start = bar + 1;
                if (file.empty()) continue;

                std::error_code fileEc;
                if (!fs::is_regular_file(dir / PathFromUtf8(file), fileEc)) {
                    warningList.push_back(where + ":" + std::to_string(lineNumber) + ": " +
                                          kCueKeys[cueIndex] + " sound not found: " + file);
                    continue;
                }
                // Same shape as every other editor sound path: relative, forward slashes.
                sounds.files.push_back(std::string(kThemesRoot) + "/" + theme.info.id + "/" + file);
            }
        }

        if (theme.info.displayName.empty()) theme.info.displayName = theme.info.id;
        loadedThemes.push_back(std::move(theme));
    }

    // Default theme first, the rest alphabetical by folder.
    std::stable_partition(loadedThemes.begin(), loadedThemes.end(), [](const Theme& theme) {
        return Lower(theme.info.id) == Lower(kDefaultThemeId);
    });
    for (const Theme& theme : loadedThemes) infoList.push_back(theme.info);

    resolveActiveIndex();
}

void ThemeLibrary::setActive(const std::string& id) {
    requestedId = id.empty() ? std::string(kDefaultThemeId) : id;
    if (scanned) resolveActiveIndex();
}

const std::string& ThemeLibrary::activeId() {
    ensureScanned();
    return resolvedId;
}

const std::vector<ThemeInfo>& ThemeLibrary::themes() {
    ensureScanned();
    return infoList;
}

bool ThemeLibrary::resolve(Cue cue, ResolvedSound& out) {
    ensureScanned();
    const size_t cueIndex = static_cast<size_t>(cue);
    if (cueIndex >= static_cast<size_t>(Cue::Count)) return false;

    for (int themeIndex : {activeIndex, defaultIndex}) {
        if (themeIndex < 0) continue;
        CueSounds& sounds = loadedThemes[static_cast<size_t>(themeIndex)].cues[cueIndex];
        if (sounds.silent) return false;
        if (sounds.files.empty()) continue;

        const int count = static_cast<int>(sounds.files.size());
        int pick = 0;
        if (count > 1) {
            // Draw from the other count-1 files so the same variant never plays twice in a row.
            const bool haveLast = sounds.lastPick >= 0 && sounds.lastPick < count;
            std::uniform_int_distribution<int> dist(0, haveLast ? count - 2 : count - 1);
            pick = dist(rng);
            if (haveLast && pick >= sounds.lastPick) ++pick;
        }
        sounds.lastPick = pick;
        out.path = sounds.files[static_cast<size_t>(pick)];
        out.gain = sounds.gain;
        return true;
    }
    return false;
}

void ThemeLibrary::ensureScanned() {
    if (!scanned) refresh();
}

int ThemeLibrary::findTheme(const std::string& id) const {
    // Folder names are case-insensitive on Windows, so a hand-edited "sleek" should still match.
    const std::string wanted = Lower(id);
    for (size_t i = 0; i < loadedThemes.size(); ++i) {
        if (Lower(loadedThemes[i].info.id) == wanted) return static_cast<int>(i);
    }
    return -1;
}

void ThemeLibrary::resolveActiveIndex() {
    defaultIndex = findTheme(kDefaultThemeId);
    activeIndex = findTheme(requestedId);
    if (activeIndex < 0) activeIndex = defaultIndex;
    if (activeIndex < 0 && !loadedThemes.empty()) activeIndex = 0;
    resolvedId = activeIndex >= 0 ? loadedThemes[static_cast<size_t>(activeIndex)].info.id
                                  : std::string();
}

} // namespace Modularity::EditorSounds
