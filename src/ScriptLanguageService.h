#pragma once

#include "Common.h"
#include "ScriptCompiler.h"

#include <unordered_map>
#include <unordered_set>

enum class ScriptLanguageServiceLanguage {
    PlainText,
    Cpp,
    C,
    GLSL,
    HLSL,
    Lua,
    ModuCPP,
    Mako
};

struct ScriptLanguageServiceProjectData {
    std::vector<fs::path> files;
    std::vector<fs::path> contextFiles;
    std::vector<std::string> projectSymbols;
    std::vector<std::string> projectCompletions;
    std::unordered_map<std::string, std::string> symbolDetails;
    std::unordered_map<std::string, std::string> functionSignatures;
    std::vector<std::string> scanWarnings;
};

struct ScriptLanguageServiceDocumentData {
    ScriptLanguageServiceLanguage language = ScriptLanguageServiceLanguage::PlainText;
    bool isModuMako = false;
    std::vector<std::string> identifiers;
    std::vector<std::string> functions;
    std::vector<std::string> defines;
    std::vector<std::string> symbols;
    std::vector<std::string> variables;
    std::vector<std::string> moducppImports;
    std::vector<std::string> moducppInspectorFields;
    std::vector<std::string> moducppCompletions;
    std::unordered_map<std::string, std::string> functionSignatures;
};

struct ScriptLanguageServiceFunctionCallContext {
    bool valid = false;
    std::string functionName;
    int activeParameter = 0;
};

class ScriptLanguageService {
public:
    static ScriptLanguageServiceLanguage detectLanguage(const fs::path& path);
    static bool isCompilableScriptPath(const fs::path& path);

    static ScriptLanguageServiceProjectData scanProjectFiles(const fs::path& projectRoot,
                                                             const fs::path& assetsPath,
                                                             const ScriptBuildConfig* config);

    static ScriptLanguageServiceDocumentData analyzeDocument(const fs::path& path,
                                                             const std::string& rawText);

    static std::vector<std::string> buildCompletionList(const std::vector<std::string>& pool,
                                                        const std::string& prefix,
                                                        size_t limit = 48);

    static std::vector<std::string> splitSignatureParameters(const std::string& signature);

    static ScriptLanguageServiceFunctionCallContext detectFunctionCallContext(const std::string& currentLine,
                                                                              int cursorColumn);

    static std::vector<std::string> buildQualifiedCompletionPool(const std::vector<std::string>& pool,
                                                                 const std::string& qualifier);

    static const std::unordered_set<std::string>& keywordsForLanguage(ScriptLanguageServiceLanguage language);
};
