#pragma once
#include "PackageManager.h"
#include "ProjectManager.h"
#include "ScriptCompiler.h"
#include "ScriptDiagnostics.h"
#include "ScriptLanguageService.h"
#include "ThirdParty/assimp/contrib/rapidjson/include/rapidjson/document.h"
#include <unordered_map>
fs::path resolveScriptsConfigPath(const Project& project);
class ModularityLspServer {
public: int run(int argc, char** argv);
private:
    struct DocumentState {
        fs::path path;
        std::string uri;
        std::string text;
        int version = 0;
        ScriptLanguageServiceDocumentData analysis;
    };
    struct CompletionPrefix {
        std::string qualifier;
        std::string term;
        bool hasQualifier;
    };
    bool initializeProject(const fs::path& workspacePath);
    bool readMessage(std::string& outJson);
    void writeJson(const std::string& jsonText);
    void sendResponse(const rapidjson::Value& id, rapidjson::Document& payload);
    void sendErrorResponse(const rapidjson::Value& id, int code, const std::string& message);
    void sendNotification(const char* method, rapidjson::Value& params);
    void sendLogMessage(const std::string& message, int type = 3);
    void publishDiagnostics(const DocumentState& doc);
    std::vector<Modularity::ScriptDiagnostic> collectLiveDiagnostics(const DocumentState& doc) const;
    void handleMessage(const rapidjson::Document& request);
    void handleInitialize(const rapidjson::Document& request);
    void handleDidOpen(const rapidjson::Document& request);
    void handleDidChange(const rapidjson::Document& request);
    void handleDidClose(const rapidjson::Document& request);
    void handleCompletion(const rapidjson::Document& request);
    void handleSignatureHelp(const rapidjson::Document& request);
    void handleHover(const rapidjson::Document& request);
    void handleDocumentSymbol(const rapidjson::Document& request);
    static fs::path uriToPath(const std::string& uri);
    static std::string pathToUri(const fs::path& path);
    static std::string readFileText(const fs::path& path);
    static std::string getMethod(const rapidjson::Document& request);
    static const rapidjson::Value* getId(const rapidjson::Document& request);
    static rapidjson::Value makeStringValue(const std::string& value, rapidjson::Document::AllocatorType& alloc);
    static std::string extractLine(const std::string& text, int line);
    static CompletionPrefix extractCompletionPrefix(const std::string& text, int line, int character, ScriptLanguageServiceLanguage language);
    Project project;
    PackageManager packageManager;
    ScriptCompiler scriptCompiler;
    ScriptBuildConfig buildConfig;
    bool hasBuildConfig = false;
    std::vector<fs::path> projectFiles;
    std::vector<std::string> projectSymbols;
    std::vector<std::string> projectCompletions;
    std::unordered_map<std::string, std::string> projectSymbolDetails;
    std::unordered_map<std::string, std::string> projectFunctionSignatures;
    std::vector<std::string> projectScanWarnings;
    std::unordered_map<std::string, DocumentState> openDocuments;
    bool shutdownRequested = false;
    bool exitRequested = false;
};
