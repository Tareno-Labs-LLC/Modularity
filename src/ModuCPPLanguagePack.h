#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace ModuCPPLang {

enum class AliasPosition {
    Keyword,
    Member
};

struct KeywordAlias {
    std::string localized;
    std::string canonical;
    AliasPosition position = AliasPosition::Keyword;
    bool identifying = true;
};

struct LanguagePack {
    std::string id;
    std::string displayName;
    std::string endonym;
    std::string folderName;
    bool isCanonical = false;
    bool loadedFromDisk = false;
    std::vector<KeywordAlias> aliases;
};

void ReloadFromDisk();

const std::vector<LanguagePack>& Languages();
const LanguagePack& CanonicalLanguage();
const LanguagePack* FindLanguage(const std::string& id);
const LanguagePack& FindLanguageOrCanonical(const std::string& id);
int LanguageIndex(const std::string& id);
std::string NormalizeLanguageId(const std::string& id);

struct Detection {
    std::string languageId;
    int score = 0;
};

Detection DetectLanguage(const std::string& source);

std::string Canonicalize(const std::string& source, const LanguagePack& pack);
std::string CanonicalizeAuto(const std::string& source);
std::string Localize(const std::string& canonicalSource, const LanguagePack& pack);
std::string Translate(const std::string& source, const LanguagePack& from, const LanguagePack& to);
std::string TranslateAuto(const std::string& source, const LanguagePack& to,
                          std::string* outDetectedLanguageId = nullptr);

const std::unordered_set<std::string>& LocalizedVocabulary();

struct ScriptLanguageSurvey {
    std::vector<std::filesystem::path> files;
    std::vector<std::string> detectedLanguageIds;
};

ScriptLanguageSurvey SurveyScripts(const std::filesystem::path& root, const LanguagePack& target);

struct TranslationReport {
    int translated = 0;
    int unchanged = 0;
    int failed = 0;
    std::string firstError;
};

TranslationReport TranslateScriptFiles(const std::vector<std::filesystem::path>& files,
                                       const LanguagePack& target);

} // namespace ModuCPPLang
