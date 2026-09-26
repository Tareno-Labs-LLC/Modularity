#pragma once

// Editor UI sounds are asked for by what happened (a Cue), never by file name. A sound theme is a
// folder under Resources/Sounds/SoundThemes/<Id>/ holding a theme.ini that maps each cue to one or
// more audio files plus a gain. Several files make a variant pool: every play picks one at random,
// never the same one twice in a row, so rapid taps do not sound like a machine gun.
//
// Themes are discovered by scanning that folder, so adding one or retuning a gain needs no rebuild.
// A cue a theme leaves out falls back to the default theme's sound for it, which lets a custom
// theme override only the cues it cares about. "cue = none" opts out instead: that theme plays
// nothing for the cue.

#include <array>
#include <random>
#include <string>
#include <vector>

namespace Modularity::EditorSounds {

enum class Cue {
    Tick = 0,         // small selection ticks: settings tabs, toolbar tools
    SectionSwitch,    // launcher sidebar page change
    Confirm,          // committing a choice: opening a project, picking a theme
    DialogOpen,       // a modal appears: About, Rename, Delete / Reload confirmations
    Warning,          // needs attention but nothing failed: package safety warning
    Error,            // console error, failed compile
    CompileStart,
    CompileSuccess,
    ScriptAssigned,   // script dropped onto an object
    PackageInstalled,

    // Interface feedback, reported by the widgets themselves (ImGuiWidgetFeedback in imgui.h).
    Button,           // any button, menu command or item picked from a popup
    ButtonDisabled,   // click on a disabled button
    Tab,
    ToggleOn,         // checkbox, switch, radio button, toggle menu item
    ToggleOff,
    SlideOpen,        // menus, dropdowns, context menus, dock drawers, expanding sections
    SlideClose,
    PopupOpen,        // modal dialogs
    PopupClose,
    ItemSelect,       // an item picked in a list or grid (File Browser)
    Hover,            // the pointer arriving on a button-like widget
    Count
};

inline constexpr const char* kThemesRoot = "Resources/Sounds/SoundThemes";
inline constexpr const char* kManifestName = "theme.ini";
inline constexpr const char* kDefaultThemeId = "Sleek";

// Manifest key for a cue ("tick", "section_switch", ...).
const char* CueKey(Cue cue);

struct ThemeInfo {
    std::string id;           // folder name; this is what preferences store
    std::string displayName;  // manifest "name", else the id
    std::string description;
};

struct ResolvedSound {
    std::string path;   // relative to the working directory, like every other Resources path
    float gain = 1.0f;  // multiply into the volume the call site asked for
};

class ThemeLibrary {
public:
    // Re-reads every theme folder and manifest. Cheap (a directory listing and a few small text
    // files), so selecting a theme calls it to pick up hand edits to theme.ini.
    void refresh();

    // Unknown ids resolve to the default theme, then to whichever theme was found first.
    void setActive(const std::string& id);
    // The theme actually in use, which differs from the requested id when that one is missing.
    const std::string& activeId();

    // Default theme first, then alphabetical by folder.
    const std::vector<ThemeInfo>& themes();

    // Problems found by the last refresh (missing files, unknown keys), for the console.
    const std::vector<std::string>& warnings() const { return warningList; }

    // Picks the file to play. False when the active theme says "none" for the cue, or when
    // neither it nor the default theme has a sound for it.
    bool resolve(Cue cue, ResolvedSound& out);

private:
    struct CueSounds {
        std::vector<std::string> files;
        float gain = 1.0f;
        int lastPick = -1;
        bool silent = false;  // "cue = none": no sound and no fallback
    };
    struct Theme {
        ThemeInfo info;
        std::array<CueSounds, static_cast<size_t>(Cue::Count)> cues;
    };

    void ensureScanned();
    int findTheme(const std::string& id) const;
    void resolveActiveIndex();

    std::vector<Theme> loadedThemes;
    std::vector<ThemeInfo> infoList;
    std::vector<std::string> warningList;
    std::string requestedId = kDefaultThemeId;
    std::string resolvedId;
    int activeIndex = -1;
    int defaultIndex = -1;
    bool scanned = false;
    std::minstd_rand rng{std::random_device{}()};
};

} // namespace Modularity::EditorSounds
