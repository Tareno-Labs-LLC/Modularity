#pragma once

#include <string>
#include <vector>

namespace Modularity {
namespace Loc {

struct LanguageInfo {
    std::string id;
    std::string folderName;
    std::string displayName;
    std::string endonym;
    bool isCanonical = false;
    bool loadedFromDisk = false;
};

void Initialize();
void ReloadFromDisk();

const std::vector<LanguageInfo>& Languages();
const LanguageInfo* FindLanguage(const std::string& id);
const LanguageInfo& CurrentLanguage();
const std::string& CurrentLanguageId();
std::string NormalizeLanguageId(const std::string& id);
int LanguageIndex(const std::string& id);

bool SetLanguage(const std::string& id);
unsigned Generation();

const char* T(const char* key, const char* englishDefault);

const char* Window(const char* key, const char* englishName);
const char* WindowRef(const char* englishName);

const char* Widget(const char* key, const char* englishLabel);

const char* Field(const char* componentKey, const char* fieldName, const char* englishDefault);

std::string PrettyFieldName(const std::string& serializedName);

std::string DetectOperatingSystemLanguage();

const std::vector<std::string>& MissingKeys();

} // namespace Loc
} // namespace Modularity
