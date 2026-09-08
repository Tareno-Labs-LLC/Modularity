#include "ModuCPPLanguagePack.h"

#include "ThirdParty/assimp/contrib/rapidjson/include/rapidjson/document.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <unordered_map>

namespace ModuCPPLang {
namespace {

namespace fs = std::filesystem;

constexpr AliasPosition K = AliasPosition::Keyword;
constexpr AliasPosition M = AliasPosition::Member;

struct AliasRow {
    const char* localized;
    const char* canonical;
    AliasPosition position;
    bool identifying;
};

struct LanguageRow {
    const char* id;
    const char* displayName;
    const char* endonym;
    const AliasRow* aliases;
    size_t aliasCount;
};

const AliasRow kGermanAliases[] = {
    { "Nutze",         "add",        K, true  },
    { "nutze",         "add",        K, true  },
    { "markiere",      "mark",       K, true  },
    { "öffentlich",    "public",     K, true  },
    { "Öffentlich",    "public",     K, true  },
    { "privat",        "private",    K, true  },
    { "Privat",        "private",    K, true  },
    { "geschützt",     "protected",  K, true  },
    { "klasse",        "class",      K, true  },
    { "Klasse",        "class",      K, true  },
    { "aufzählung",    "enum",       K, true  },
    { "UnterSkript",   "SubScript",  K, true  },
    { "inspektor",     "inspector",  K, true  },
    { "verweis",       "ref",        K, true  },
    { "zu",            "to",         K, false },
    { "dann",          "then",       K, true  },
    { "Dann",          "then",       K, true  },
    { "für",           "each",       K, true  },
    { "Für",           "each",       K, true  },

    { "Wenn",          "if",         K, true  },
    { "wenn",          "if",         K, true  },
    { "sonst",         "else",       K, true  },
    { "Sonst",         "else",       K, true  },
    { "solange",       "while",      K, true  },
    { "wiederhole",    "for",        K, true  },
    { "Zurück",        "return",     K, true  },
    { "zurück",        "return",     K, true  },
    { "abbrechen",     "break",      K, true  },
    { "fortfahren",    "continue",   K, true  },
    { "wahr",          "true",       K, true  },
    { "falsch",        "false",      K, true  },

    { "Nichts",        "void",       K, true  },
    { "nichts",        "void",       K, true  },
    { "Zeichenkette",  "string",     K, true  },
    { "Ganzzahl",      "int",        K, true  },
    { "Kommazahl",     "float",      K, true  },
    { "Wahrheitswert", "bool",       K, true  },
    { "statisch",      "static",     K, true  },
    { "konstant",      "const",      K, true  },

    { "Beginnen",      "Begin",      K, true  },
    { "Start",         "Begin",      K, false },
    { "Tick",          "TickUpdate", K, false },
    { "Aktualisieren", "Update",     K, true  },

    { "Starten",       "Start",      M, true  },
    { "Bereit",        "Ready",      M, true  },
    { "Zustand",       "state",      M, true  },
    { "Länge",         "Length",     M, true  },
    { "IstLeer",       "IsEmpty",    M, true  },
};

const LanguageRow kLanguageTable[] = {
    { "english", "English",          "English", nullptr,        0 },
    { "german",  "German (Deutsch)", "Deutsch", kGermanAliases, std::size(kGermanAliases) },
};

bool isIdentifierByte(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_' || uc >= 0x80;
}

bool isIdentifierStartByte(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalpha(uc) != 0 || c == '_' || uc >= 0x80;
}

bool precededByMemberOperator(const std::string& text, size_t identStart) {
    size_t pos = identStart;
    while (pos > 0 && std::isspace(static_cast<unsigned char>(text[pos - 1])) != 0) {
        --pos;
    }
    if (pos == 0) return false;
    const char prev = text[pos - 1];
    if (prev == '.') return true;
    if (prev == ':' && pos >= 2 && text[pos - 2] == ':') return true;
    if (prev == '>' && pos >= 2 && text[pos - 2] == '-') return true;
    return false;
}

struct TokenSpan {
    size_t begin = 0;
    size_t end = 0;
    bool member = false;
};

template <typename Visit>
void scanIdentifierTokens(const std::string& source, Visit&& visit) {
    enum class Mode { Normal, LineComment, BlockComment, StringLiteral, CharLiteral, Preproc };
    Mode mode = Mode::Normal;
    bool escaped = false;
    bool atLineStart = true;
    size_t i = 0;

    while (i < source.size()) {
        const char c = source[i];
        const char next = (i + 1 < source.size()) ? source[i + 1] : '\0';

        switch (mode) {
            case Mode::Normal:
                if (c == '/' && next == '/') { mode = Mode::LineComment; i += 2; atLineStart = false; continue; }
                if (c == '/' && next == '*') { mode = Mode::BlockComment; i += 2; atLineStart = false; continue; }
                if (c == '"')  { mode = Mode::StringLiteral; escaped = false; atLineStart = false; ++i; continue; }
                if (c == '\'') { mode = Mode::CharLiteral;   escaped = false; atLineStart = false; ++i; continue; }
                if (c == '#' && atLineStart) { mode = Mode::Preproc; atLineStart = false; ++i; continue; }
                if (isIdentifierStartByte(c)) {
                    size_t end = i + 1;
                    while (end < source.size() && isIdentifierByte(source[end])) ++end;
                    TokenSpan span;
                    span.begin = i;
                    span.end = end;
                    span.member = precededByMemberOperator(source, i);
                    visit(span);
                    i = end;
                    atLineStart = false;
                    continue;
                }
                if (c == '\n') atLineStart = true;
                else if (std::isspace(static_cast<unsigned char>(c)) == 0) atLineStart = false;
                ++i;
                continue;

            case Mode::LineComment:
                if (c == '\n') { mode = Mode::Normal; atLineStart = true; }
                ++i;
                continue;

            case Mode::BlockComment:
                if (c == '*' && next == '/') { mode = Mode::Normal; atLineStart = false; i += 2; continue; }
                ++i;
                continue;

            case Mode::StringLiteral:
                if (escaped)        escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"')  mode = Mode::Normal;
                ++i;
                continue;

            case Mode::CharLiteral:
                if (escaped)         escaped = false;
                else if (c == '\\')  escaped = true;
                else if (c == '\'')  mode = Mode::Normal;
                ++i;
                continue;

            case Mode::Preproc:
                if (c == '\\' && next == '\n') { i += 2; continue; }
                if (c == '\\' && next == '\r') { i += 2; continue; }
                if (c == '\n') { mode = Mode::Normal; atLineStart = true; }
                ++i;
                continue;
        }
    }
}

template <typename Lookup>
std::string rewriteIdentifierTokens(const std::string& source, Lookup&& lookup) {
    std::string out;
    out.reserve(source.size() + 64);
    size_t cursor = 0;
    scanIdentifierTokens(source, [&](const TokenSpan& span) {
        const std::string word = source.substr(span.begin, span.end - span.begin);
        const std::string* replacement = lookup(word, span.member);
        if (!replacement) return;
        out.append(source, cursor, span.begin - cursor);
        out.append(*replacement);
        cursor = span.end;
    });
    out.append(source, cursor, std::string::npos);
    return out;
}

struct AliasMaps {
    std::unordered_map<std::string, std::string> toCanonicalKeyword;
    std::unordered_map<std::string, std::string> toCanonicalMember;
    std::unordered_map<std::string, std::string> toLocalizedKeyword;
    std::unordered_map<std::string, std::string> toLocalizedMember;
};

struct DetectionVote {
    size_t packIndex = 0;
    bool member = false;
};

struct Registry {
    std::vector<LanguagePack> packs;
    std::vector<AliasMaps> maps;
    std::unordered_set<std::string> vocabulary;
    std::unordered_map<std::string, std::vector<DetectionVote>> detectionVotes;
};

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

bool readWholeFile(const fs::path& path, std::string& outText);

bool parseModuCppJson(const std::string& text, LanguagePack& outPack, std::string& outError) {
    rapidjson::Document doc;
    doc.Parse(text.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        outError = "not a JSON object";
        return false;
    }
    if (doc.HasMember("language") && doc["language"].IsObject()) {
        const rapidjson::Value& lang = doc["language"];
        if (lang.HasMember("id") && lang["id"].IsString()) outPack.id = lowerCopy(lang["id"].GetString());
        if (lang.HasMember("displayName") && lang["displayName"].IsString()) {
            outPack.displayName = lang["displayName"].GetString();
        }
        if (lang.HasMember("endonym") && lang["endonym"].IsString()) {
            outPack.endonym = lang["endonym"].GetString();
        }
    }
    if (outPack.id.empty()) {
        outError = "missing language.id";
        return false;
    }
    if (doc.HasMember("aliases") && doc["aliases"].IsArray()) {
        for (const rapidjson::Value& entry : doc["aliases"].GetArray()) {
            if (!entry.IsObject()) continue;
            if (!entry.HasMember("localized") || !entry["localized"].IsString()) continue;
            if (!entry.HasMember("canonical") || !entry["canonical"].IsString()) continue;
            KeywordAlias alias;
            alias.localized = entry["localized"].GetString();
            alias.canonical = entry["canonical"].GetString();
            alias.position = AliasPosition::Keyword;
            if (entry.HasMember("position") && entry["position"].IsString() &&
                lowerCopy(entry["position"].GetString()) == "member") {
                alias.position = AliasPosition::Member;
            }
            alias.identifying = !(entry.HasMember("identifying") && entry["identifying"].IsBool() &&
                                  !entry["identifying"].GetBool());
            if (alias.localized.empty() || alias.canonical.empty()) continue;
            outPack.aliases.push_back(std::move(alias));
        }
    }
    outPack.isCanonical = outPack.aliases.empty();
    return true;
}

void indexPack(Registry& built, size_t packIndex) {
    AliasMaps& maps = built.maps[packIndex];
    for (const KeywordAlias& entry : built.packs[packIndex].aliases) {
        built.vocabulary.insert(entry.localized);
        const bool member = entry.position == AliasPosition::Member;
        if (member) {
            maps.toCanonicalMember.emplace(entry.localized, entry.canonical);
            maps.toLocalizedMember.emplace(entry.canonical, entry.localized);
        } else {
            maps.toCanonicalKeyword.emplace(entry.localized, entry.canonical);
            maps.toLocalizedKeyword.emplace(entry.canonical, entry.localized);
        }
        if (entry.identifying) {
            built.detectionVotes[entry.localized].push_back(DetectionVote{packIndex, member});
        }
    }
}

Registry buildRegistry() {
    Registry built;
    built.packs.reserve(std::size(kLanguageTable) + 8);
    for (size_t packIndex = 0; packIndex < std::size(kLanguageTable); ++packIndex) {
        const LanguageRow& row = kLanguageTable[packIndex];
        LanguagePack pack;
        pack.id = row.id;
        pack.displayName = row.displayName;
        pack.endonym = row.endonym;
        pack.isCanonical = (row.aliasCount == 0);
        pack.aliases.reserve(row.aliasCount);
        for (size_t i = 0; i < row.aliasCount; ++i) {
            const AliasRow& alias = row.aliases[i];
            KeywordAlias entry;
            entry.localized = alias.localized;
            entry.canonical = alias.canonical;
            entry.position = alias.position;
            entry.identifying = alias.identifying;
            pack.aliases.push_back(std::move(entry));
        }
        built.packs.push_back(std::move(pack));
    }

    const fs::path root = findLanguagesRoot();
    if (!root.empty()) {
        std::error_code ec;
        std::vector<fs::path> folders;
        for (const fs::directory_entry& entry : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (entry.is_directory(ec)) folders.push_back(entry.path());
        }
        std::sort(folders.begin(), folders.end());
        for (const fs::path& folder : folders) {
            const fs::path file = folder / "ModuCPP.json";
            if (!fs::is_regular_file(file, ec) || ec) continue;
            std::string text;
            if (!readWholeFile(file, text)) continue;

            LanguagePack pack;
            pack.folderName = folder.filename().string();
            pack.displayName = pack.folderName;
            pack.endonym = pack.folderName;
            pack.id = lowerCopy(pack.folderName);
            std::string error;
            if (!parseModuCppJson(text, pack, error)) {
                std::cerr << "[ModuCPPLang] " << file.string() << ": " << error
                          << "; keeping the built-in dictionary.\n";
                continue;
            }
            pack.loadedFromDisk = true;
            const auto existing = std::find_if(built.packs.begin(), built.packs.end(),
                                               [&](const LanguagePack& p) { return p.id == pack.id; });
            if (existing != built.packs.end()) {
                if (pack.aliases.empty() && !existing->aliases.empty()) {
                    existing->displayName = pack.displayName;
                    existing->endonym = pack.endonym;
                    existing->loadedFromDisk = true;
                } else {
                    *existing = std::move(pack);
                }
            } else {
                built.packs.push_back(std::move(pack));
            }
        }
    }

    built.maps.resize(built.packs.size());
    for (size_t i = 0; i < built.packs.size(); ++i) indexPack(built, i);
    return built;
}

Registry& mutableRegistry() {
    static Registry instance = buildRegistry();
    return instance;
}

const Registry& registry() {
    return mutableRegistry();
}

const AliasMaps& mapsFor(const LanguagePack& pack) {
    const Registry& reg = registry();
    for (size_t i = 0; i < reg.packs.size(); ++i) {
        if (reg.packs[i].id == pack.id) return reg.maps[i];
    }
    return reg.maps.front();
}

bool isModuCppScript(const fs::path& path) {
    return lowerCopy(path.extension().string()) == ".moducpp";
}

bool readWholeFile(const fs::path& path, std::string& outText) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    outText = buffer.str();
    return true;
}

bool writeFile(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    file << text;
    return file.good();
}

constexpr int kDetectionThreshold = 2;

} // namespace

void ReloadFromDisk() {
    mutableRegistry() = buildRegistry();
}

const std::vector<LanguagePack>& Languages() {
    return registry().packs;
}

const LanguagePack& CanonicalLanguage() {
    return registry().packs.front();
}

const LanguagePack* FindLanguage(const std::string& id) {
    const std::string normalized = lowerCopy(id);
    for (const LanguagePack& pack : registry().packs) {
        if (pack.id == normalized) return &pack;
    }
    return nullptr;
}

const LanguagePack& FindLanguageOrCanonical(const std::string& id) {
    const LanguagePack* pack = FindLanguage(id);
    return pack ? *pack : CanonicalLanguage();
}

int LanguageIndex(const std::string& id) {
    const std::string normalized = lowerCopy(id);
    const std::vector<LanguagePack>& packs = registry().packs;
    for (size_t i = 0; i < packs.size(); ++i) {
        if (packs[i].id == normalized) return static_cast<int>(i);
    }
    return 0;
}

std::string NormalizeLanguageId(const std::string& id) {
    const LanguagePack* pack = FindLanguage(id);
    return pack ? pack->id : CanonicalLanguage().id;
}

Detection DetectLanguage(const std::string& source) {
    Detection result;
    result.languageId = CanonicalLanguage().id;
    if (source.empty()) return result;

    const Registry& reg = registry();
    std::vector<int> scores(reg.packs.size(), 0);

    scanIdentifierTokens(source, [&](const TokenSpan& span) {
        const std::string word = source.substr(span.begin, span.end - span.begin);
        const auto it = reg.detectionVotes.find(word);
        if (it == reg.detectionVotes.end()) return;
        for (const DetectionVote& vote : it->second) {
            if (vote.member == span.member) ++scores[vote.packIndex];
        }
    });

    int bestIndex = 0;
    int bestScore = 0;
    for (size_t i = 1; i < scores.size(); ++i) {
        if (scores[i] > bestScore) {
            bestScore = scores[i];
            bestIndex = static_cast<int>(i);
        }
    }
    if (bestIndex != 0 && bestScore >= kDetectionThreshold) {
        result.languageId = reg.packs[static_cast<size_t>(bestIndex)].id;
        result.score = bestScore;
    }
    return result;
}

std::string Canonicalize(const std::string& source, const LanguagePack& pack) {
    if (pack.isCanonical || pack.aliases.empty()) return source;
    const AliasMaps& maps = mapsFor(pack);
    return rewriteIdentifierTokens(source, [&](const std::string& word, bool member) -> const std::string* {
        const auto& table = member ? maps.toCanonicalMember : maps.toCanonicalKeyword;
        const auto it = table.find(word);
        return it == table.end() ? nullptr : &it->second;
    });
}

std::string CanonicalizeAuto(const std::string& source) {
    const Detection detected = DetectLanguage(source);
    const LanguagePack& pack = FindLanguageOrCanonical(detected.languageId);
    return Canonicalize(source, pack);
}

std::string Localize(const std::string& canonicalSource, const LanguagePack& pack) {
    if (pack.isCanonical || pack.aliases.empty()) return canonicalSource;
    const AliasMaps& maps = mapsFor(pack);
    return rewriteIdentifierTokens(canonicalSource,
                                   [&](const std::string& word, bool member) -> const std::string* {
        const auto& table = member ? maps.toLocalizedMember : maps.toLocalizedKeyword;
        const auto it = table.find(word);
        return it == table.end() ? nullptr : &it->second;
    });
}

std::string Translate(const std::string& source, const LanguagePack& from, const LanguagePack& to) {
    if (from.id == to.id) return source;
    return Localize(Canonicalize(source, from), to);
}

std::string TranslateAuto(const std::string& source, const LanguagePack& to,
                          std::string* outDetectedLanguageId) {
    const Detection detected = DetectLanguage(source);
    if (outDetectedLanguageId) *outDetectedLanguageId = detected.languageId;
    return Translate(source, FindLanguageOrCanonical(detected.languageId), to);
}

const std::unordered_set<std::string>& LocalizedVocabulary() {
    return registry().vocabulary;
}

ScriptLanguageSurvey SurveyScripts(const fs::path& root, const LanguagePack& target) {
    ScriptLanguageSurvey survey;
    if (root.empty()) return survey;

    std::error_code ec;
    std::vector<fs::path> candidates;
    if (fs::is_directory(root, ec) && !ec) {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_directory(ec)) continue;
            if (isModuCppScript(it->path())) candidates.push_back(it->path());
        }
    } else if (fs::is_regular_file(root, ec) && !ec && isModuCppScript(root)) {
        candidates.push_back(root);
    }

    for (const fs::path& candidate : candidates) {
        std::string text;
        if (!readWholeFile(candidate, text)) continue;
        const Detection detected = DetectLanguage(text);
        if (detected.languageId == target.id) continue;
        survey.files.push_back(candidate);
        if (std::find(survey.detectedLanguageIds.begin(), survey.detectedLanguageIds.end(),
                      detected.languageId) == survey.detectedLanguageIds.end()) {
            survey.detectedLanguageIds.push_back(detected.languageId);
        }
    }
    return survey;
}

TranslationReport TranslateScriptFiles(const std::vector<fs::path>& files, const LanguagePack& target) {
    TranslationReport report;
    for (const fs::path& file : files) {
        std::string text;
        if (!readWholeFile(file, text)) {
            ++report.failed;
            if (report.firstError.empty()) {
                report.firstError = "Unable to read " + file.string();
            }
            continue;
        }
        const std::string translated = TranslateAuto(text, target);
        if (translated == text) {
            ++report.unchanged;
            continue;
        }
        if (!writeFile(file, translated)) {
            ++report.failed;
            if (report.firstError.empty()) {
                report.firstError = "Unable to write " + file.string();
            }
            continue;
        }
        ++report.translated;
    }
    return report;
}

} // namespace ModuCPPLang
