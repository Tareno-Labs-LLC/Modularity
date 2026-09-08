#include "EditorLocalization.h"

#include "ThirdParty/assimp/contrib/rapidjson/include/rapidjson/document.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace Modularity {
namespace Loc {
namespace {

// Every lookup below arrives as a `const char*` - either a literal at the call site or a key built
// on the fly by Field(). With a plain unordered_map<std::string, ...> each find() has to materialise
// a std::string just to hash it, and the editor makes hundreds of these per frame while drawing
// panels. A transparent hasher lets find() work straight off the pointer with no allocation.
struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view key) const noexcept {
        return std::hash<std::string_view>{}(key);
    }
};

using StringTable = std::unordered_map<std::string, std::string,
                                       TransparentStringHash, std::equal_to<>>;

struct State {
    bool initialized = false;
    unsigned generation = 1;
    std::string currentId = "english";
    std::vector<LanguageInfo> languages;
    std::map<std::string, StringTable> byLanguage;
    StringTable builtinEnglish;
    StringTable active;
    StringTable composedWindows;
    StringTable windowRefs;
    StringTable composedWidgets;
    std::unordered_set<std::string> warnedKeys;
    std::vector<std::string> missingKeys;
    fs::path languagesRoot;
};

State& state() {
    static State s;
    return s;
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

fs::path findLanguagesRoot() {
    std::vector<fs::path> candidates;
    std::error_code ec;
    candidates.push_back(fs::current_path(ec) / "Resources" / "Languages");
#ifdef MODULARITY_SOURCE_DIR
    candidates.push_back(fs::path(MODULARITY_SOURCE_DIR) / "Resources" / "Languages");
#endif
    for (const fs::path& candidate : candidates) {
        if (fs::is_directory(candidate, ec) && !ec) return candidate;
    }
    return {};
}

bool readFile(const fs::path& path, std::string& outText) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    outText = buffer.str();
    return true;
}

bool parseEditorJson(const std::string& text, LanguageInfo& outInfo, StringTable& outStrings,
                     std::string& outError) {
    rapidjson::Document doc;
    doc.Parse(text.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        outError = "not a JSON object";
        return false;
    }

    if (doc.HasMember("language") && doc["language"].IsObject()) {
        const rapidjson::Value& lang = doc["language"];
        if (lang.HasMember("id") && lang["id"].IsString()) {
            outInfo.id = lowerCopy(lang["id"].GetString());
        }
        if (lang.HasMember("displayName") && lang["displayName"].IsString()) {
            outInfo.displayName = lang["displayName"].GetString();
        }
        if (lang.HasMember("endonym") && lang["endonym"].IsString()) {
            outInfo.endonym = lang["endonym"].GetString();
        }
    }

    const rapidjson::Value* strings = &doc;
    if (doc.HasMember("strings") && doc["strings"].IsObject()) {
        strings = &doc["strings"];
    }
    for (auto it = strings->MemberBegin(); it != strings->MemberEnd(); ++it) {
        if (!it->name.IsString() || !it->value.IsString()) continue;
        const std::string key = it->name.GetString();
        if (key == "language" || key == "strings") continue;
        outStrings[key] = it->value.GetString();
    }
    return true;
}

void rebuildActiveTable() {
    State& s = state();
    auto apply = [&](const StringTable& source) {
        for (const auto& [key, value] : source) {
            s.active[key] = value;
        }
    };
    apply(s.builtinEnglish);
    const auto english = s.byLanguage.find("english");
    if (english != s.byLanguage.end()) apply(english->second);
    if (s.currentId != "english") {
        const auto selected = s.byLanguage.find(s.currentId);
        if (selected != s.byLanguage.end()) apply(selected->second);
    }

    if (s.currentId != "english") {
        const auto selected = s.byLanguage.find(s.currentId);
        for (auto& [key, value] : s.active) {
            const bool inSelected = selected != s.byLanguage.end() &&
                                    selected->second.find(key) != selected->second.end();
            if (inSelected) continue;
            if (english != s.byLanguage.end()) {
                const auto it = english->second.find(key);
                if (it != english->second.end()) { value = it->second; continue; }
            }
            const auto builtin = s.builtinEnglish.find(key);
            if (builtin != s.builtinEnglish.end()) value = builtin->second;
        }
    }
}

void rebuildComposedCaches() {
    State& s = state();
    for (auto& [key, composed] : s.composedWindows) {
        const size_t marker = composed.rfind("###");
        if (marker == std::string::npos) continue;
        const std::string englishName = composed.substr(marker + 3);
        const auto it = s.active.find(key);
        const std::string display = it != s.active.end() ? it->second : englishName;
        composed = display + "###" + englishName;
    }
    for (auto& [key, composed] : s.composedWidgets) {
        const auto it = s.active.find(key);
        composed = (it != s.active.end() ? it->second : key) + "###" + key;
    }
}

void loadLanguagesFromDisk() {
    State& s = state();
    s.byLanguage.clear();
    s.languages.clear();
    s.languagesRoot = findLanguagesRoot();

    LanguageInfo english;
    english.id = "english";
    english.folderName = "English";
    english.displayName = "English";
    english.endonym = "English";
    english.isCanonical = true;
    s.languages.push_back(english);

    if (s.languagesRoot.empty()) {
        std::cerr << "[Localization] Resources/Languages not found; editor stays English.\n";
        return;
    }

    std::error_code ec;
    std::vector<fs::path> folders;
    for (const fs::directory_entry& entry : fs::directory_iterator(s.languagesRoot, ec)) {
        if (ec) break;
        if (entry.is_directory(ec)) folders.push_back(entry.path());
    }
    std::sort(folders.begin(), folders.end());

    for (const fs::path& folder : folders) {
        const fs::path editorFile = folder / "Editor.json";
        if (!fs::is_regular_file(editorFile, ec) || ec) continue;

        std::string text;
        if (!readFile(editorFile, text)) {
            std::cerr << "[Localization] Unable to read " << editorFile.string() << "\n";
            continue;
        }

        LanguageInfo info;
        info.folderName = folder.filename().string();
        info.id = lowerCopy(info.folderName);
        info.displayName = info.folderName;
        info.endonym = info.folderName;

        StringTable strings;
        std::string error;
        if (!parseEditorJson(text, info, strings, error)) {
            std::cerr << "[Localization] " << editorFile.string() << ": " << error
                      << "; language skipped.\n";
            continue;
        }
        info.isCanonical = info.id == "english";
        info.loadedFromDisk = true;

        s.byLanguage[info.id] = std::move(strings);
        const auto existing = std::find_if(s.languages.begin(), s.languages.end(),
                                           [&](const LanguageInfo& l) { return l.id == info.id; });
        if (existing != s.languages.end()) {
            *existing = info;
        } else {
            s.languages.push_back(info);
        }
    }
}

} // namespace

std::string KeyFromFieldName(const std::string& serializedName);
void AppendKeyFromFieldName(std::string& out, std::string_view serializedName);

void Initialize() {
    State& s = state();
    if (s.initialized) return;
    s.initialized = true;
    loadLanguagesFromDisk();
    rebuildActiveTable();
    rebuildComposedCaches();
}

void ReloadFromDisk() {
    State& s = state();
    s.initialized = true;
    loadLanguagesFromDisk();
    if (!FindLanguage(s.currentId)) s.currentId = "english";
    rebuildActiveTable();
    rebuildComposedCaches();
    ++s.generation;
}

const std::vector<LanguageInfo>& Languages() {
    Initialize();
    return state().languages;
}

const LanguageInfo* FindLanguage(const std::string& id) {
    Initialize();
    const std::string normalized = lowerCopy(id);
    for (const LanguageInfo& info : state().languages) {
        if (info.id == normalized) return &info;
    }
    return nullptr;
}

const LanguageInfo& CurrentLanguage() {
    Initialize();
    const LanguageInfo* info = FindLanguage(state().currentId);
    return info ? *info : state().languages.front();
}

const std::string& CurrentLanguageId() {
    Initialize();
    return state().currentId;
}

std::string NormalizeLanguageId(const std::string& id) {
    const LanguageInfo* info = FindLanguage(id);
    return info ? info->id : std::string("english");
}

int LanguageIndex(const std::string& id) {
    Initialize();
    const std::string normalized = lowerCopy(id);
    const std::vector<LanguageInfo>& languages = state().languages;
    for (size_t i = 0; i < languages.size(); ++i) {
        if (languages[i].id == normalized) return static_cast<int>(i);
    }
    return 0;
}

bool SetLanguage(const std::string& id) {
    Initialize();
    State& s = state();
    const std::string normalized = NormalizeLanguageId(id);
    if (normalized == s.currentId) return false;
    s.currentId = normalized;
    rebuildActiveTable();
    rebuildComposedCaches();
    ++s.generation;
    return true;
}

unsigned Generation() {
    Initialize();
    return state().generation;
}

const char* T(const char* key, const char* englishDefault) {
    Initialize();
    State& s = state();
    if (!key || !*key) return englishDefault ? englishDefault : "";

    if (englishDefault && *englishDefault) {
        const auto builtin = s.builtinEnglish.find(key);
        if (builtin == s.builtinEnglish.end()) {
            s.builtinEnglish.emplace(key, englishDefault);
            const auto active = s.active.find(key);
            if (active == s.active.end()) {
                std::string resolved = englishDefault;
                if (s.currentId != "english") {
                    const auto selected = s.byLanguage.find(s.currentId);
                    if (selected != s.byLanguage.end()) {
                        const auto it = selected->second.find(key);
                        if (it != selected->second.end()) resolved = it->second;
                    }
                }
                return s.active.emplace(key, std::move(resolved)).first->second.c_str();
            }
        }
    }

    const auto it = s.active.find(key);
    if (it != s.active.end()) return it->second.c_str();

    if (s.warnedKeys.insert(key).second) {
        s.missingKeys.push_back(key);
        std::cerr << "[Localization] Missing key '" << key << "' in '" << s.currentId
                  << "' and in English; showing the key.\n";
    }
    return s.active.emplace(key, key).first->second.c_str();
}

const char* Window(const char* key, const char* englishName) {
    Initialize();
    State& s = state();
    const char* name = (englishName && *englishName) ? englishName : "Window";
    const auto existing = s.composedWindows.find(key ? key : name);
    if (existing != s.composedWindows.end()) return existing->second.c_str();

    const std::string display = T(key, name);
    std::string composed = display + "###" + name;
    return s.composedWindows.emplace(key ? key : name, std::move(composed)).first->second.c_str();
}

const char* Widget(const char* key, const char* englishLabel) {
    Initialize();
    State& s = state();
    if (!key || !*key) return englishLabel ? englishLabel : "";
    const auto existing = s.composedWidgets.find(key);
    if (existing != s.composedWidgets.end()) return existing->second.c_str();
    const std::string display = T(key, englishLabel);
    return s.composedWidgets.emplace(key, display + "###" + key).first->second.c_str();
}

const char* WindowRef(const char* englishName) {
    Initialize();
    State& s = state();
    const char* name = (englishName && *englishName) ? englishName : "Window";
    const auto existing = s.windowRefs.find(name);
    if (existing != s.windowRefs.end()) return existing->second.c_str();
    return s.windowRefs.emplace(name, std::string("###") + name).first->second.c_str();
}

const char* Field(const char* componentKey, const char* fieldName, const char* englishDefault) {
    Initialize();
    if (!fieldName || !*fieldName) return englishDefault ? englishDefault : "";
    if (!componentKey || !*componentKey) {
        return (englishDefault && *englishDefault) ? englishDefault : fieldName;
    }

    // Reused across calls so the concatenation stops allocating once the buffer has grown; this
    // runs for every labelled inspector row on every frame.
    static thread_local std::string fullKey;
    fullKey.assign(componentKey);
    fullKey.push_back('_');
    AppendKeyFromFieldName(fullKey, fieldName);

    if (englishDefault && *englishDefault) {
        return T(fullKey.c_str(), englishDefault);
    }
    const std::string pretty = PrettyFieldName(fieldName);
    return T(fullKey.c_str(), pretty.c_str());
}

std::string KeyFromFieldName(const std::string& serializedName) {
    std::string out;
    out.reserve(serializedName.size() + 8);
    AppendKeyFromFieldName(out, serializedName);
    return out;
}

void AppendKeyFromFieldName(std::string& out, std::string_view serializedName) {
    for (size_t i = 0; i < serializedName.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(serializedName[i]);
        const unsigned char prev = i > 0 ? static_cast<unsigned char>(serializedName[i - 1]) : 0;
        if (c == '_') {
            if (!out.empty() && out.back() != '_') out.push_back('_');
            continue;
        }
        const bool boundary =
            (std::isupper(c) != 0 && (std::islower(prev) != 0 || std::isdigit(prev) != 0)) ||
            (std::isupper(c) != 0 && std::isupper(prev) != 0 && i + 1 < serializedName.size() &&
             std::islower(static_cast<unsigned char>(serializedName[i + 1])) != 0);
        if (boundary && !out.empty() && out.back() != '_') out.push_back('_');
        out.push_back(static_cast<char>(std::toupper(c)));
    }
}

std::string PrettyFieldName(const std::string& serializedName) {
    std::string out;
    out.reserve(serializedName.size() + 8);
    char previous = '\0';
    for (size_t i = 0; i < serializedName.size(); ++i) {
        const char c = serializedName[i];
        if (c == '_') {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
            previous = c;
            continue;
        }
        const bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
        const bool digit = std::isdigit(static_cast<unsigned char>(c)) != 0;
        const bool prevLower = std::islower(static_cast<unsigned char>(previous)) != 0;
        const bool prevDigit = std::isdigit(static_cast<unsigned char>(previous)) != 0;
        if (!out.empty() && out.back() != ' ' && ((upper && (prevLower || prevDigit)) ||
                                                  (digit && !prevDigit && previous != '\0'))) {
            out.push_back(' ');
        }
        if (out.empty() || out.back() == ' ') {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        } else {
            out.push_back(c);
        }
        previous = c;
    }
    return out.empty() ? serializedName : out;
}

std::string DetectOperatingSystemLanguage() {
    Initialize();
    std::string locale;
#ifdef _WIN32
    wchar_t buffer[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(buffer, LOCALE_NAME_MAX_LENGTH) > 0) {
        for (wchar_t* p = buffer; *p; ++p) locale.push_back(static_cast<char>(*p));
    }
#else
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        if (const char* value = std::getenv(name)) {
            if (*value) { locale = value; break; }
        }
    }
#endif
    if (locale.empty()) return {};

    std::string tag = lowerCopy(locale);
    const size_t cut = tag.find_first_of("_-.@");
    if (cut != std::string::npos) tag = tag.substr(0, cut);
    if (tag.empty() || tag == "c" || tag == "posix") return {};

    static const std::unordered_map<std::string, std::string> kIsoToLanguage = {
        { "en", "english" },
        { "de", "german" },
    };
    const auto it = kIsoToLanguage.find(tag);
    if (it == kIsoToLanguage.end()) return {};
    return FindLanguage(it->second) ? it->second : std::string();
}

const std::vector<std::string>& MissingKeys() {
    Initialize();
    return state().missingKeys;
}

} // namespace Loc
} // namespace Modularity
