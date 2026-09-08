#include "ModularityLspServer.h"

#include "ThirdParty/assimp/contrib/rapidjson/include/rapidjson/stringbuffer.h"
#include "ThirdParty/assimp/contrib/rapidjson/include/rapidjson/writer.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <stack>
#include <sstream>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {
    using JsonValue = rapidjson::Value;
    using JsonDocument = rapidjson::Document;
    using JsonAllocator = JsonDocument::AllocatorType;

    static bool isIdentifierBody(unsigned char c) {
        return std::isalnum(c) || c == '_';
    }

    static bool isIdentifierStart(unsigned char c) {
        return std::isalpha(c) || c == '_';
    }

    static std::string jsonToString(const JsonValue& value) {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        value.Accept(writer);
        return buffer.GetString();
    }

    static void addJsonMember(JsonValue& object,
                              const char* key,
                              const std::string& value,
                              JsonAllocator& alloc) {
        JsonValue jsonKey;
        jsonKey.SetString(key, alloc);
        JsonValue jsonValue;
        jsonValue.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), alloc);
        object.AddMember(jsonKey, jsonValue, alloc);
    }

    static void addJsonMember(JsonValue& object,
                              const char* key,
                              const char* value,
                              JsonAllocator& alloc) {
        JsonValue jsonKey;
        jsonKey.SetString(key, alloc);
        JsonValue jsonValue;
        jsonValue.SetString(value ? value : "", alloc);
        object.AddMember(jsonKey, jsonValue, alloc);
    }

    static void addJsonMember(JsonValue& object,
                              const char* key,
                              int value,
                              JsonAllocator& alloc) {
        JsonValue jsonKey;
        jsonKey.SetString(key, alloc);
        object.AddMember(jsonKey, value, alloc);
    }

    static void addJsonMember(JsonValue& object,
                              const char* key,
                              bool value,
                              JsonAllocator& alloc) {
        JsonValue jsonKey;
        jsonKey.SetString(key, alloc);
        object.AddMember(jsonKey, value, alloc);
    }

    static std::string percentDecode(std::string value) {
        std::string decoded;
        decoded.reserve(value.size());
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%' && i + 2 < value.size()) {
                const std::string hex = value.substr(i + 1, 2);
                char* end = nullptr;
                long parsed = std::strtol(hex.c_str(), &end, 16);
                if (end != nullptr && *end == '\0') {
                    decoded.push_back(static_cast<char>(parsed));
                    i += 2;
                    continue;
                }
            }
            decoded.push_back(value[i]);
        }
        return decoded;
    }

    struct DeprecationRule {
        const char* symbolName;
        Modularity::ScriptDiagnosticSeverity severity;
        const char* message;
        const char* replacement;
        const char* reason;
    };

    static const std::vector<DeprecationRule>& deprecationRegistry() {
        static const std::vector<DeprecationRule> kRules = {
            {
                "ModuBehaviour",
                Modularity::ScriptDiagnosticSeverity::Warning,
                "ModuBehaviour is deprecated. Use ModuNode instead.",
                "ModuNode",
                "ModuNode is the current native script base class."
            }
        };
        return kRules;
    }

    static const DeprecationRule* findDeprecationRule(const std::string& symbol) {
        for (const DeprecationRule& rule : deprecationRegistry()) {
            if (symbol == rule.symbolName) return &rule;
        }
        return nullptr;
    }

    static std::string wordAtPosition(const std::string& text, int line, int character) {
        std::istringstream stream(text);
        std::string current;
        for (int i = 0; i <= line; ++i) {
            if (!std::getline(stream, current)) return {};
        }
        int pos = std::clamp(character, 0, static_cast<int>(current.size()));
        if (pos > 0 && (pos == static_cast<int>(current.size()) ||
                        !isIdentifierBody(static_cast<unsigned char>(current[static_cast<size_t>(pos)])))) {
            --pos;
        }
        if (pos < 0 || pos >= static_cast<int>(current.size()) ||
            !isIdentifierBody(static_cast<unsigned char>(current[static_cast<size_t>(pos)]))) {
            return {};
        }
        int start = pos;
        while (start > 0 && isIdentifierBody(static_cast<unsigned char>(current[static_cast<size_t>(start - 1)]))) {
            --start;
        }
        int end = pos + 1;
        while (end < static_cast<int>(current.size()) &&
               isIdentifierBody(static_cast<unsigned char>(current[static_cast<size_t>(end)]))) {
            ++end;
        }
        return current.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
    }

    static bool containsIdentifier(const std::vector<std::string>& values, const std::string& symbol) {
        return std::find(values.begin(), values.end(), symbol) != values.end();
    }

    static fs::path findProjectRoot(fs::path path) {
        std::error_code ec;
        if (!path.empty() && !fs::is_directory(path, ec)) {
            path = path.parent_path();
        }
        if (path.is_relative()) {
            fs::path absolute = fs::absolute(path, ec);
            if (!ec) path = absolute;
        }
        while (!path.empty()) {
            if (fs::exists(path / "project.modu", ec)) {
                return path.lexically_normal();
            }
            fs::path parent = path.parent_path();
            if (parent == path) break;
            path = parent;
        }
        return {};
    }
}

int ModularityLspServer::run(int argc, char** argv) {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);
    fs::path workspacePath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if ((arg == "--project" || arg == "--workspace") && i + 1 < argc) {
            workspacePath = argv[++i];
        }
    }

    if (!workspacePath.empty()) {
        try {
            initializeProject(workspacePath);
        } catch (const std::exception& e) {
            fprintf(stderr, "[ModuCPP LSP] initializeProject threw: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "[ModuCPP LSP] initializeProject threw unknown exception\n");
        }
    }

    while (!exitRequested) {
        std::string payload;
        if (!readMessage(payload)) {
            break;
        }

        JsonDocument request;
        request.Parse(payload.c_str());
        if (request.HasParseError() || !request.IsObject()) {
            continue;
        }
        handleMessage(request);
    }

    return 0;
}

bool ModularityLspServer::initializeProject(const fs::path& workspacePath) {
    std::error_code ec;
    fs::path normalized = findProjectRoot(workspacePath);
    if (normalized.empty()) {
        normalized = workspacePath;
        if (!normalized.empty() && !fs::is_directory(normalized, ec)) {
            normalized = normalized.parent_path();
        }
    }
    if (normalized.empty()) {
        return false;
    }

    fs::path projectFile = normalized / "project.modu";
    if (!fs::exists(projectFile, ec)) {
        return false;
    }
    if (!project.load(projectFile)) {
        return false;
    }

    packageManager.setProjectRoot(project.projectPath);
    fs::path configPath = resolveScriptsConfigPath(project);
    std::string error;
    hasBuildConfig = scriptCompiler.loadConfig(configPath, buildConfig, error);
    if (hasBuildConfig) {
        packageManager.applyToBuildConfig(buildConfig);
    }

    ScriptLanguageServiceProjectData projectData = ScriptLanguageService::scanProjectFiles(
        project.projectPath,
        project.assetsPath,
        hasBuildConfig ? &buildConfig : nullptr);
    projectFiles = std::move(projectData.files);
    projectSymbols = std::move(projectData.projectSymbols);
    projectCompletions = std::move(projectData.projectCompletions);
    projectSymbolDetails = std::move(projectData.symbolDetails);
    projectFunctionSignatures = std::move(projectData.functionSignatures);
    projectScanWarnings = std::move(projectData.scanWarnings);
    if (projectCompletions.empty()) {
        projectScanWarnings.push_back("ModuCPP language service found no workspace completions.");
    }
    return true;
}

bool ModularityLspServer::readMessage(std::string& outJson) {
    outJson.clear();
    std::string line;
    size_t contentLength = 0;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }
        const std::string prefix = "Content-Length:";
        if (line.rfind(prefix, 0) == 0) {
            contentLength = static_cast<size_t>(std::strtoull(line.substr(prefix.size()).c_str(), nullptr, 10));
        }
    }

    if (contentLength == 0) {
        return false;
    }

    outJson.resize(contentLength);
    std::cin.read(&outJson[0], static_cast<std::streamsize>(contentLength));
    return std::cin.good() || static_cast<size_t>(std::cin.gcount()) == contentLength;
}

void ModularityLspServer::writeJson(const std::string& jsonText) {
    std::cout << "Content-Length: " << jsonText.size() << "\r\n\r\n";
    std::cout.write(jsonText.data(), static_cast<std::streamsize>(jsonText.size()));
    std::cout.flush();
}

void ModularityLspServer::sendResponse(const JsonValue& id, JsonDocument& payload) {
    JsonDocument response;
    response.SetObject();
    JsonAllocator& alloc = response.GetAllocator();
    addJsonMember(response, "jsonrpc", "2.0", alloc);
    JsonValue idValue;
    idValue.CopyFrom(id, alloc);
    JsonValue resultValue;
    resultValue.CopyFrom(payload, alloc);
    response.AddMember(JsonValue("id", alloc), idValue, alloc);
    response.AddMember(JsonValue("result", alloc), resultValue, alloc);
    writeJson(jsonToString(response));
}

void ModularityLspServer::sendErrorResponse(const JsonValue& id, int code, const std::string& message) {
    JsonDocument response;
    response.SetObject();
    JsonAllocator& alloc = response.GetAllocator();
    addJsonMember(response, "jsonrpc", "2.0", alloc);
    response.AddMember(JsonValue("id", alloc), JsonValue(id, alloc), alloc);

    JsonValue error(rapidjson::kObjectType);
    addJsonMember(error, "code", code, alloc);
    addJsonMember(error, "message", message, alloc);
    response.AddMember(JsonValue("error", alloc), error, alloc);
    writeJson(jsonToString(response));
}

void ModularityLspServer::sendNotification(const char* method, JsonValue& params) {
    JsonDocument notification;
    notification.SetObject();
    JsonAllocator& alloc = notification.GetAllocator();
    addJsonMember(notification, "jsonrpc", "2.0", alloc);
    addJsonMember(notification, "method", method, alloc);
    JsonValue paramsValue;
    paramsValue.CopyFrom(params, alloc);
    notification.AddMember(JsonValue("params", alloc), paramsValue, alloc);
    writeJson(jsonToString(notification));
}

void ModularityLspServer::sendLogMessage(const std::string& message, int type) {
    JsonDocument params;
    params.SetObject();
    JsonAllocator& alloc = params.GetAllocator();
    addJsonMember(params, "type", type, alloc);
    addJsonMember(params, "message", message, alloc);
    sendNotification("window/logMessage", params);
}

void ModularityLspServer::publishDiagnostics(const DocumentState& doc) {
    JsonDocument params;
    params.SetObject();
    JsonAllocator& alloc = params.GetAllocator();
    addJsonMember(params, "uri", doc.uri, alloc);
    JsonValue diagnostics(rapidjson::kArrayType);
    const std::vector<Modularity::ScriptDiagnostic> liveDiagnostics = collectLiveDiagnostics(doc);
    for (const Modularity::ScriptDiagnostic& diagnostic : liveDiagnostics) {
        JsonValue item(rapidjson::kObjectType);

        JsonValue range(rapidjson::kObjectType);
        JsonValue start(rapidjson::kObjectType);
        JsonValue end(rapidjson::kObjectType);
        const int line = std::max(0, diagnostic.line - 1);
        const int startCharacter = std::max(0, diagnostic.column > 0 ? diagnostic.column - 1 : 0);
        const int endCharacter = std::max(startCharacter + 1, static_cast<int>(diagnostic.sourceLine.size()));
        addJsonMember(start, "line", line, alloc);
        addJsonMember(start, "character", startCharacter, alloc);
        addJsonMember(end, "line", line, alloc);
        addJsonMember(end, "character", endCharacter, alloc);
        range.AddMember(JsonValue("start", alloc), start, alloc);
        range.AddMember(JsonValue("end", alloc), end, alloc);
        item.AddMember(JsonValue("range", alloc), range, alloc);

        int severity = 1;
        if (diagnostic.severity == Modularity::ScriptDiagnosticSeverity::Warning) severity = 2;
        else if (diagnostic.severity == Modularity::ScriptDiagnosticSeverity::Info) severity = 3;
        addJsonMember(item, "severity", severity, alloc);
        addJsonMember(item, "code", diagnostic.code, alloc);
        addJsonMember(item, "source", diagnostic.source.empty() ? "ModuCPP" : diagnostic.source, alloc);
        addJsonMember(item, "message", diagnostic.message, alloc);
        diagnostics.PushBack(item, alloc);
    }
    params.AddMember(JsonValue("diagnostics", alloc), diagnostics, alloc);
    sendNotification("textDocument/publishDiagnostics", params);
}

std::vector<Modularity::ScriptDiagnostic> ModularityLspServer::collectLiveDiagnostics(const DocumentState& doc) const {
    using namespace Modularity;

    std::vector<ScriptDiagnostic> diagnostics;
    std::vector<std::pair<int, int>> braceStack;
    bool inBlockComment = false;
    bool unterminatedString = false;
    int unterminatedStringLine = 0;
    int unterminatedStringColumn = 0;

    const bool isModuCppDocument = doc.analysis.language == ScriptLanguageServiceLanguage::ModuCPP;

    auto isSemicolonRequired = [isModuCppDocument](const std::string& trimmed, const std::string& nextTrimmed) {
        if (trimmed.empty()) return false;
        if (trimmed.rfind("//", 0) == 0) return false;
        if (trimmed[0] == '#') return false;
        if (isModuCppDocument && trimmed[0] == '[') return false;
        if (trimmed == "{" || trimmed == "}") return false;
        const char last = trimmed.back();
        if (last == ';' || last == '{' || last == '}' || last == ':' || last == ',' || last == '(') return false;

        static const char* kContinuationSuffixes[] = {
            "||", "&&", "+=", "-=", "*=", "/=", "!=", "==", "<=", ">=",
            "<<", ">>", "->", "?", "=", "+", "-", "*", "/", "|", "&", "^", "~", "("
        };
        for (const char* suffix : kContinuationSuffixes) {
            const size_t len = std::strlen(suffix);
            if (trimmed.size() >= len && trimmed.substr(trimmed.size() - len) == suffix) {
                return false;
            }
        }

        if (!nextTrimmed.empty()) {
            static const char* kContinuationPrefixes[] = {
                "||", "&&", "+=", "-=", "*=", "/=", "!=", "==", "<=", ">=",
                "<<", ">>", "->", "?", ":", ".", "+", "-", "*", "/", "|", "&", "^"
            };
            for (const char* prefix : kContinuationPrefixes) {
                if (nextTrimmed.rfind(prefix, 0) == 0) {
                    return false;
                }
            }
        }

        static const char* kPrefixes[] = {
            "if", "else", "for", "while", "switch", "case", "default", "class", "struct",
            "enum", "namespace", "public", "private", "protected", "template", "do", "try", "catch", "SubScript",
            "inspector"
        };
        for (const char* prefix : kPrefixes) {
            const size_t len = std::strlen(prefix);
            if (trimmed.rfind(prefix, 0) == 0 &&
                (trimmed.size() == len || std::isspace(static_cast<unsigned char>(trimmed[len])) || trimmed[len] == '(' || trimmed[len] == ':')) {
                return false;
            }
        }

        if (last == ')') {
            if (nextTrimmed == "{" || nextTrimmed.rfind("{", 0) == 0) {
                return false;
            }
            const size_t parenPos = trimmed.find('(');
            if (parenPos != std::string::npos && parenPos > 0) {
                const std::string beforeParen = trimmed.substr(0, parenPos);
                if (beforeParen.find(' ') != std::string::npos) {
                    return false;
                }
            }
        }
        return true;
    };

    auto trim = [](const std::string& value) {
        size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
        size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
        return value.substr(start, end - start);
    };

    std::vector<std::string> lines;
    {
        std::istringstream input(doc.text);
        std::string tmp;
        while (std::getline(input, tmp)) {
            lines.push_back(tmp);
        }
    }

    for (int lineIndex = 0; lineIndex < static_cast<int>(lines.size()); ++lineIndex) {
        const int lineNumber = lineIndex + 1;
        const std::string& line = lines[lineIndex];
        const std::string sourceLine = line;
        const std::string nextTrimmed = (lineIndex + 1 < static_cast<int>(lines.size()))
            ? trim(lines[lineIndex + 1])
            : std::string{};

        bool inString = false;
        char stringDelimiter = '\0';
        bool escaping = false;
        bool sawCode = false;

        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            const char next = (i + 1 < line.size()) ? line[i + 1] : '\0';

            if (inBlockComment) {
                if (c == '*' && next == '/') {
                    inBlockComment = false;
                    ++i;
                }
                continue;
            }

            if (inString) {
                if (escaping) {
                    escaping = false;
                    continue;
                }
                if (c == '\\') {
                    escaping = true;
                    continue;
                }
                if (c == stringDelimiter) {
                    inString = false;
                    stringDelimiter = '\0';
                }
                continue;
            }

            if (c == '/' && next == '/') {
                break;
            }
            if (c == '/' && next == '*') {
                inBlockComment = true;
                ++i;
                continue;
            }
            if (c == '"' || c == '\'') {
                inString = true;
                stringDelimiter = c;
                if (!unterminatedString) {
                    unterminatedString = true;
                    unterminatedStringLine = lineNumber;
                    unterminatedStringColumn = static_cast<int>(i) + 1;
                }
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(c))) {
                continue;
            }

            sawCode = true;
            if (isIdentifierStart(static_cast<unsigned char>(c))) {
                const size_t start = i;
                while (i < line.size() && isIdentifierBody(static_cast<unsigned char>(line[i]))) {
                    ++i;
                }
                const std::string symbol = line.substr(start, i - start);
                if (const DeprecationRule* rule = findDeprecationRule(symbol)) {
                    ScriptDiagnostic diagnostic = makeScriptDiagnostic(
                        ScriptDiagnosticId::BuildWarning,
                        "ModuCPP",
                        lineNumber,
                        rule->message,
                        std::string("Use '") + rule->replacement + "' instead."
                    );
                    diagnostic.column = static_cast<int>(start) + 1;
                    diagnostic.severity = rule->severity;
                    diagnostic.sourceLine = sourceLine;
                    diagnostics.push_back(std::move(diagnostic));
                }
                --i;
                continue;
            }
            if (c == '{') {
                braceStack.emplace_back(lineNumber, static_cast<int>(i) + 1);
            } else if (c == '}') {
                if (!braceStack.empty()) {
                    braceStack.pop_back();
                } else {
                    ScriptDiagnostic diagnostic = makeScriptDiagnostic(
                        ScriptDiagnosticId::UnexpectedToken,
                        "ModuCPP",
                        lineNumber,
                        "Unexpected '}' token.",
                        "Remove the extra closing brace or add the missing opening brace."
                    );
                    diagnostic.column = static_cast<int>(i) + 1;
                    diagnostic.sourceLine = sourceLine;
                    diagnostics.push_back(std::move(diagnostic));
                }
            }
        }

        if (!inString && unterminatedString && unterminatedStringLine == lineNumber) {
            unterminatedString = false;
        }

        const std::string trimmed = [&] {
            std::string t = trim(line);
            const size_t commentPos = t.find("//");
            if (commentPos != std::string::npos) {
                t = trim(t.substr(0, commentPos));
            }
            return t;
        }();
        
        if (sawCode && isSemicolonRequired(trimmed, nextTrimmed)) {
            ScriptDiagnostic diagnostic = makeScriptDiagnostic(
                ScriptDiagnosticId::MissingSemicolon,
                "ModuCPP",
                lineNumber,
                "Missing ';'.",
                "Add ';' at the end of the statement."
            );
            diagnostic.column = static_cast<int>(sourceLine.find_last_not_of(" \t\r\n")) + 2;
            diagnostic.sourceLine = sourceLine;
            diagnostics.push_back(std::move(diagnostic));
        }
    }

    if (unterminatedString) {
        ScriptDiagnostic diagnostic = makeScriptDiagnostic(
            ScriptDiagnosticId::UnclosedString,
            "ModuCPP",
            unterminatedStringLine,
            "A string was opened but not closed.",
            "Add the missing closing quote."
        );
        diagnostic.column = unterminatedStringColumn;
        diagnostics.push_back(std::move(diagnostic));
    }

    if (!braceStack.empty()) {
        const auto [lineNumber, column] = braceStack.back();
        ScriptDiagnostic diagnostic = makeScriptDiagnostic(
            ScriptDiagnosticId::UnclosedBlock,
            "ModuCPP",
            lineNumber,
            "A code block was opened but not closed.",
            "Add the missing '}'."
        );
        diagnostic.column = column;
        diagnostics.push_back(std::move(diagnostic));
    }

    return diagnostics;
}

void ModularityLspServer::handleMessage(const JsonDocument& request) {
    const std::string method = getMethod(request);
    if (method == "initialize") {
        handleInitialize(request);
    } else if (method == "initialized") {
        return;
    } else if (method == "shutdown") {
        shutdownRequested = true;
        JsonDocument result;
        result.SetNull();
        const JsonValue* id = getId(request);
        if (id != nullptr) {
            sendResponse(*id, result);
        }
    } else if (method == "exit") {
        exitRequested = true;
    } else if (method == "textDocument/didOpen") {
        handleDidOpen(request);
    } else if (method == "textDocument/didChange") {
        handleDidChange(request);
    } else if (method == "textDocument/didClose") {
        handleDidClose(request);
    } else if (method == "textDocument/completion") {
        handleCompletion(request);
    } else if (method == "textDocument/signatureHelp") {
        handleSignatureHelp(request);
    } else if (method == "textDocument/hover") {
        handleHover(request);
    } else if (method == "textDocument/documentSymbol") {
        handleDocumentSymbol(request);
    }  else if (method == "workspace/didChangeWatchedFiles") {
        if (!project.projectPath.empty()) {
            initializeProject(project.projectPath);
            sendLogMessage("[ModuCPP LSP] Workspace rescanned.", 3);
        }
    } else {
        const JsonValue* id = getId(request);
        if (id != nullptr) {
            sendErrorResponse(*id, -32601, "Method not found");
        }
    }
}

void ModularityLspServer::handleInitialize(const JsonDocument& request) {
    if (request.HasMember("params") && request["params"].IsObject()) {
        const JsonValue& params = request["params"];
        if (params.HasMember("rootUri") && params["rootUri"].IsString()) {
            initializeProject(uriToPath(params["rootUri"].GetString()));
        } else if (params.HasMember("rootPath") && params["rootPath"].IsString()) {
            initializeProject(params["rootPath"].GetString());
        } else if (params.HasMember("workspaceFolders") && params["workspaceFolders"].IsArray() &&
                   !params["workspaceFolders"].Empty()) {
            const JsonValue& folder = params["workspaceFolders"][0];
            if (folder.IsObject() && folder.HasMember("uri") && folder["uri"].IsString()) {
                initializeProject(uriToPath(folder["uri"].GetString()));
            }
        }
    }

    JsonDocument result;
    result.SetObject();
    JsonAllocator& alloc = result.GetAllocator();
    JsonValue capabilities(rapidjson::kObjectType);
    addJsonMember(capabilities, "textDocumentSync", 1, alloc);

    JsonValue completionProvider(rapidjson::kObjectType);
    addJsonMember(completionProvider, "resolveProvider", false, alloc);
    JsonValue completionTriggers(rapidjson::kArrayType);
    completionTriggers.PushBack(makeStringValue(".", alloc), alloc);
    completionTriggers.PushBack(makeStringValue(":", alloc), alloc);
    completionTriggers.PushBack(makeStringValue("_", alloc), alloc);
    completionTriggers.PushBack(makeStringValue(">", alloc), alloc);
    completionProvider.AddMember(JsonValue("triggerCharacters", alloc), completionTriggers, alloc);
    capabilities.AddMember(JsonValue("completionProvider", alloc), completionProvider, alloc);

    JsonValue signatureProvider(rapidjson::kObjectType);
    JsonValue signatureTriggers(rapidjson::kArrayType);
    signatureTriggers.PushBack(makeStringValue("(", alloc), alloc);
    signatureTriggers.PushBack(makeStringValue(",", alloc), alloc);
    signatureProvider.AddMember(JsonValue("triggerCharacters", alloc), signatureTriggers, alloc);
    capabilities.AddMember(JsonValue("signatureHelpProvider", alloc), signatureProvider, alloc);
    addJsonMember(capabilities, "hoverProvider", true, alloc);
    addJsonMember(capabilities, "documentSymbolProvider", true, alloc);
    result.AddMember(JsonValue("capabilities", alloc), capabilities, alloc);

    const JsonValue* id = getId(request);
    if (id != nullptr) {
        sendResponse(*id, result);
    }
    for (const std::string& warning : projectScanWarnings) {
        sendLogMessage("[ModuCPP LSP] " + warning, 2);
    }
}

void ModularityLspServer::handleDidOpen(const JsonDocument& request) {
    if (!request.HasMember("params") || !request["params"].IsObject()) return;
    const JsonValue& params = request["params"];
    if (!params.HasMember("textDocument") || !params["textDocument"].IsObject()) return;
    const JsonValue& textDocument = params["textDocument"];
    if (!textDocument.HasMember("uri") || !textDocument["uri"].IsString()) return;

    DocumentState doc;
    doc.uri = textDocument["uri"].GetString();
    doc.path = uriToPath(doc.uri);
    doc.version = textDocument.HasMember("version") && textDocument["version"].IsInt() ? textDocument["version"].GetInt() : 0;
    if (textDocument.HasMember("text") && textDocument["text"].IsString()) {
        doc.text = textDocument["text"].GetString();
    } else {
        doc.text = readFileText(doc.path);
    }
    doc.analysis = ScriptLanguageService::analyzeDocument(doc.path, doc.text);
    openDocuments[doc.uri] = std::move(doc);
    publishDiagnostics(openDocuments[doc.uri]);
}

void ModularityLspServer::handleDidChange(const JsonDocument& request) {
    if (!request.HasMember("params") || !request["params"].IsObject()) return;
    const JsonValue& params = request["params"];
    if (!params.HasMember("textDocument") || !params["textDocument"].IsObject()) return;
    if (!params.HasMember("contentChanges") || !params["contentChanges"].IsArray() || params["contentChanges"].Empty()) return;

    const JsonValue& textDocument = params["textDocument"];
    if (!textDocument.HasMember("uri") || !textDocument["uri"].IsString()) return;
    const std::string uri = textDocument["uri"].GetString();
    auto it = openDocuments.find(uri);
    if (it == openDocuments.end()) return;

    const JsonValue& change = params["contentChanges"][params["contentChanges"].Size() - 1];
    if (change.IsObject() && change.HasMember("text") && change["text"].IsString()) {
        it->second.text = change["text"].GetString();
        it->second.version = textDocument.HasMember("version") && textDocument["version"].IsInt()
            ? textDocument["version"].GetInt()
            : (it->second.version + 1);
        it->second.analysis = ScriptLanguageService::analyzeDocument(it->second.path, it->second.text);
        publishDiagnostics(it->second);
    }
}

void ModularityLspServer::handleDidClose(const JsonDocument& request) {
    if (!request.HasMember("params") || !request["params"].IsObject()) return;
    const JsonValue& params = request["params"];
    if (!params.HasMember("textDocument") || !params["textDocument"].IsObject()) return;
    const JsonValue& textDocument = params["textDocument"];
    if (!textDocument.HasMember("uri") || !textDocument["uri"].IsString()) return;
    const std::string uri = textDocument["uri"].GetString();

    auto it = openDocuments.find(uri);
    if (it != openDocuments.end()) {
        DocumentState closedDoc = it->second;
        openDocuments.erase(it);
        JsonDocument params;
        params.SetObject();
        JsonAllocator& alloc = params.GetAllocator();
        addJsonMember(params, "uri", closedDoc.uri, alloc);
        JsonValue diagnostics(rapidjson::kArrayType);
        params.AddMember(JsonValue("diagnostics", alloc), diagnostics, alloc);
        sendNotification("textDocument/publishDiagnostics", params);
    }
}

void ModularityLspServer::handleCompletion(const JsonDocument& request) {
    const JsonValue* id = getId(request);
    if (id == nullptr) return;
    if (!request.HasMember("params") || !request["params"].IsObject()) {
        sendErrorResponse(*id, -32602, "Missing params");
        return;
    }
    const JsonValue& params = request["params"];
    if (!params.HasMember("textDocument") || !params["textDocument"].IsObject() ||
        !params.HasMember("position") || !params["position"].IsObject()) {
        sendErrorResponse(*id, -32602, "Missing completion params");
        return;
    }

    const std::string uri = params["textDocument"]["uri"].GetString();
    auto it = openDocuments.find(uri);
    if (it == openDocuments.end()) {
        sendErrorResponse(*id, -32602, "Document not open");
        return;
    }

    const int line = params["position"]["line"].GetInt();
    const int character = params["position"]["character"].GetInt();
    const CompletionPrefix prefix = extractCompletionPrefix(
    it->second.text, line, character, it->second.analysis.language);

    std::unordered_set<std::string> poolSet;
    const auto& keywords = ScriptLanguageService::keywordsForLanguage(it->second.analysis.language);
    for (const auto& keyword : keywords) poolSet.insert(keyword);
    for (const auto& entry : it->second.analysis.identifiers) poolSet.insert(entry);
    for (const auto& entry : it->second.analysis.functions) poolSet.insert(entry);
    for (const auto& entry : it->second.analysis.defines) poolSet.insert(entry);
    for (const auto& entry : it->second.analysis.symbols) poolSet.insert(entry);
    for (const auto& entry : it->second.analysis.variables) poolSet.insert(entry);
    for (const auto& entry : it->second.analysis.moducppImports) poolSet.insert(entry);
    for (const auto& entry : it->second.analysis.moducppInspectorFields) poolSet.insert(entry);
    for (const auto& entry : it->second.analysis.moducppCompletions) poolSet.insert(entry);
    for (const auto& entry : projectSymbols) poolSet.insert(entry);
    for (const auto& entry : projectCompletions) poolSet.insert(entry);

    std::vector<std::string> pool(poolSet.begin(), poolSet.end());
    std::sort(pool.begin(), pool.end());
    std::vector<std::string> qualifiedPool = prefix.hasQualifier ? ScriptLanguageService::buildQualifiedCompletionPool(pool, prefix.qualifier) : pool;
    
    
    std::vector<std::string> completions = ScriptLanguageService::buildCompletionList(
        qualifiedPool, prefix.term);

    if (completions.empty() && (prefix.hasQualifier || !prefix.term.empty()) &&
        it->second.analysis.language == ScriptLanguageServiceLanguage::ModuCPP) {
        const std::string display = prefix.hasQualifier ? prefix.qualifier + "::" + prefix.term : prefix.term;
        sendLogMessage("[ModuCPP LSP] No completion candidates found for prefix '" + display + "'.", 3);
    }

    JsonDocument result;
    result.SetArray();
    JsonAllocator& alloc = result.GetAllocator();
    for (const auto& entry : completions) {
        JsonValue item(rapidjson::kObjectType);
        addJsonMember(item, "label", entry, alloc);
        addJsonMember(item, "insertText", entry, alloc);
        int kind = 6; // Variable as default
        const bool isFunction =
            it->second.analysis.functionSignatures.count(entry) > 0 ||
            projectFunctionSignatures.count(entry) > 0;
        const bool isVariable =
            std::find(it->second.analysis.variables.begin(),
                      it->second.analysis.variables.end(), entry) != it->second.analysis.variables.end() ||
            std::find(it->second.analysis.moducppInspectorFields.begin(),
                      it->second.analysis.moducppInspectorFields.end(), entry) != it->second.analysis.moducppInspectorFields.end();
        const bool isDefine =
            std::find(it->second.analysis.defines.begin(),
                      it->second.analysis.defines.end(), entry) != it->second.analysis.defines.end();
        const bool isContextSymbol = projectSymbolDetails.find(entry) != projectSymbolDetails.end();

        if (keywords.find(entry) != keywords.end())   kind = 14; // Keyword
        else if (isFunction)                           kind = 3;  // Function
        else if (isContextSymbol)                      kind = 13; // Property
        else if (isDefine)                             kind = 21; // Constant
        else if (isVariable)                           kind = 6;  // Variable
        addJsonMember(item, "kind", kind, alloc);
        
        if (isContextSymbol) {
            addJsonMember(item, "detail", projectSymbolDetails[entry], alloc);
        } else if (it->second.analysis.language == ScriptLanguageServiceLanguage::ModuCPP ||
                   it->second.analysis.isModuMako) {
            addJsonMember(item, "detail", it->second.analysis.isModuMako ? "ModuMAKO" : "ModuCPP", alloc);
        }
        result.PushBack(item, alloc);
    }
    sendResponse(*id, result);
}

void ModularityLspServer::handleSignatureHelp(const JsonDocument& request) {
    const JsonValue* id = getId(request);
    if (id == nullptr) return;
    if (!request.HasMember("params") || !request["params"].IsObject()) {
        sendErrorResponse(*id, -32602, "Missing params");
        return;
    }
    const JsonValue& params = request["params"];
    const std::string uri = params["textDocument"]["uri"].GetString();
    auto it = openDocuments.find(uri);
    if (it == openDocuments.end()) {
        sendErrorResponse(*id, -32602, "Document not open");
        return;
    }

    const int line = params["position"]["line"].GetInt();
    const int character = params["position"]["character"].GetInt();
    const std::string currentLine = extractLine(it->second.text, line);
    ScriptLanguageServiceFunctionCallContext callCtx =
        ScriptLanguageService::detectFunctionCallContext(currentLine, character);

    JsonDocument result;
    if (!callCtx.valid) {
        result.SetNull();
        sendResponse(*id, result);
        return;
    }

    std::string signatureLabel;
    auto findSignature = [&](const std::string& name) -> std::string {
        auto fit = it->second.analysis.functionSignatures.find(name);
        if (fit != it->second.analysis.functionSignatures.end()) return fit->second;
        fit = projectFunctionSignatures.find(name);
        if (fit != projectFunctionSignatures.end()) return fit->second;
        size_t scope = name.rfind("::");
        if (scope != std::string::npos && scope + 2 < name.size()) {
            fit = it->second.analysis.functionSignatures.find(name.substr(scope + 2));
            if (fit != it->second.analysis.functionSignatures.end()) return fit->second;
            fit = projectFunctionSignatures.find(name.substr(scope + 2));
            if (fit != projectFunctionSignatures.end()) return fit->second;
        }
        size_t dot = name.rfind('.');
        if (dot != std::string::npos && dot + 1 < name.size()) {
            fit = it->second.analysis.functionSignatures.find(name.substr(dot + 1));
            if (fit != it->second.analysis.functionSignatures.end()) return fit->second;
            fit = projectFunctionSignatures.find(name.substr(dot + 1));
            if (fit != projectFunctionSignatures.end()) return fit->second;
        }
        return {};
    };

    signatureLabel = findSignature(callCtx.functionName);
    if (signatureLabel.empty() &&
        (it->second.analysis.language == ScriptLanguageServiceLanguage::Cpp ||
         it->second.analysis.language == ScriptLanguageServiceLanguage::ModuCPP ||
         it->second.analysis.isModuMako ||
         it->second.analysis.language == ScriptLanguageServiceLanguage::C)) {
        if (callCtx.functionName == "Begin") signatureLabel = "void Begin()";
        else if (callCtx.functionName == "TickUpdate") signatureLabel = "void TickUpdate(float deltaTime)";
        else if (callCtx.functionName == "Spec") signatureLabel = "void Spec()";
        else if (callCtx.functionName == "TestEditor") signatureLabel = "void TestEditor()";
        else if (callCtx.functionName == "Update") signatureLabel = "void Update(float deltaTime)";
    }
    if (signatureLabel.empty()) {
        signatureLabel = callCtx.functionName + "(...)";
    }

    std::vector<std::string> paramsList = ScriptLanguageService::splitSignatureParameters(signatureLabel);
    result.SetObject();
    JsonAllocator& alloc = result.GetAllocator();
    JsonValue signatures(rapidjson::kArrayType);
    JsonValue signature(rapidjson::kObjectType);
    addJsonMember(signature, "label", signatureLabel, alloc);
    JsonValue parameters(rapidjson::kArrayType);
    for (const auto& param : paramsList) {
        JsonValue parameter(rapidjson::kObjectType);
        addJsonMember(parameter, "label", param, alloc);
        parameters.PushBack(parameter, alloc);
    }
    signature.AddMember(JsonValue("parameters", alloc), parameters, alloc);
    signatures.PushBack(signature, alloc);
    result.AddMember(JsonValue("signatures", alloc), signatures, alloc);
    addJsonMember(result, "activeSignature", 0, alloc);
    addJsonMember(result, "activeParameter", std::max(0, callCtx.activeParameter), alloc);
    sendResponse(*id, result);
}

void ModularityLspServer::handleHover(const JsonDocument& request) {
    const JsonValue* id = getId(request);
    if (id == nullptr) return;
    if (!request.HasMember("params") || !request["params"].IsObject()) {
        sendErrorResponse(*id, -32602, "Missing params");
        return;
    }
    const JsonValue& params = request["params"];
    if (!params.HasMember("textDocument") || !params["textDocument"].IsObject() ||
        !params.HasMember("position") || !params["position"].IsObject()) {
        sendErrorResponse(*id, -32602, "Missing hover params");
        return;
    }

    const std::string uri = params["textDocument"]["uri"].GetString();
    auto it = openDocuments.find(uri);
    if (it == openDocuments.end()) {
        sendErrorResponse(*id, -32602, "Document not open");
        return;
    }

    const int line = params["position"]["line"].GetInt();
    const int character = params["position"]["character"].GetInt();
    const std::string symbol = wordAtPosition(it->second.text, line, character);

    JsonDocument result;
    if (symbol.empty()) {
        result.SetNull();
        sendResponse(*id, result);
        return;
    }

    std::vector<std::string> sections;
    if (const DeprecationRule* rule = findDeprecationRule(symbol)) {
        std::string text = std::string("**Deprecated:** ") + rule->message;
        if (rule->replacement && rule->replacement[0] != '\0') {
            text += "\n\nReplacement: `" + std::string(rule->replacement) + "`";
        }
        if (rule->reason && rule->reason[0] != '\0') {
            text += "\n\n" + std::string(rule->reason);
        }
        sections.push_back(text);
    }

    std::string signature;
    if (const auto localSignature = it->second.analysis.functionSignatures.find(symbol);
        localSignature != it->second.analysis.functionSignatures.end()) {
        signature = localSignature->second;
    } else if (const auto projectSignature = projectFunctionSignatures.find(symbol);
               projectSignature != projectFunctionSignatures.end()) {
        signature = projectSignature->second;
    }
    if (!signature.empty()) {
        sections.push_back("```cpp\n" + signature + "\n```");
    }

    const auto& keywords = ScriptLanguageService::keywordsForLanguage(it->second.analysis.language);
    if (keywords.find(symbol) != keywords.end()) {
        sections.push_back("`" + symbol + "` keyword");
    }

    auto detailIt = projectSymbolDetails.find(symbol);
    if (detailIt != projectSymbolDetails.end()) {
        sections.push_back("`" + symbol + "`\n\n" + detailIt->second);
    } else if (containsIdentifier(projectSymbols, symbol) || containsIdentifier(projectCompletions, symbol)) {
        sections.push_back("`" + symbol + "` project symbol");
    } else if (containsIdentifier(it->second.analysis.symbols, symbol) ||
               containsIdentifier(it->second.analysis.functions, symbol) ||
               containsIdentifier(it->second.analysis.variables, symbol)) {
        sections.push_back("`" + symbol + "` document symbol");
    }

    if (sections.empty()) {
        result.SetNull();
        sendResponse(*id, result);
        return;
    }

    std::string markdown;
    for (const std::string& section : sections) {
        if (!markdown.empty()) markdown += "\n\n---\n\n";
        markdown += section;
    }

    result.SetObject();
    JsonAllocator& alloc = result.GetAllocator();
    JsonValue contents(rapidjson::kObjectType);
    addJsonMember(contents, "kind", "markdown", alloc);
    addJsonMember(contents, "value", markdown, alloc);
    result.AddMember(JsonValue("contents", alloc), contents, alloc);
    sendResponse(*id, result);
}

void ModularityLspServer::handleDocumentSymbol(const JsonDocument& request) {
    const JsonValue* id = getId(request);
    if (id == nullptr) return;
    if (!request.HasMember("params") || !request["params"].IsObject()) {
        sendErrorResponse(*id, -32602, "Missing params");
        return;
    }

    const std::string uri = request["params"]["textDocument"]["uri"].GetString();
    auto it = openDocuments.find(uri);
    if (it == openDocuments.end()) {
        sendErrorResponse(*id, -32602, "Document not open");
        return;
    }

    JsonDocument result;
    result.SetArray();
    JsonAllocator& alloc = result.GetAllocator();
    for (const auto& symbol : it->second.analysis.symbols) {
        JsonValue item(rapidjson::kObjectType);
        addJsonMember(item, "name", symbol, alloc);
        addJsonMember(item, "kind", 12, alloc);

        JsonValue range(rapidjson::kObjectType);
        JsonValue start(rapidjson::kObjectType);
        addJsonMember(start, "line", 0, alloc);
        addJsonMember(start, "character", 0, alloc);
        JsonValue end(rapidjson::kObjectType);
        addJsonMember(end, "line", 0, alloc);
        addJsonMember(end, "character", 0, alloc);
        range.AddMember(JsonValue("start", alloc), start, alloc);
        range.AddMember(JsonValue("end", alloc), end, alloc);
        JsonValue selectionRange(rapidjson::kObjectType);
        JsonValue selectionStart(rapidjson::kObjectType);
        addJsonMember(selectionStart, "line", 0, alloc);
        addJsonMember(selectionStart, "character", 0, alloc);
        JsonValue selectionEnd(rapidjson::kObjectType);
        addJsonMember(selectionEnd, "line", 0, alloc);
        addJsonMember(selectionEnd, "character", 0, alloc);
        selectionRange.AddMember(JsonValue("start", alloc), selectionStart, alloc);
        selectionRange.AddMember(JsonValue("end", alloc), selectionEnd, alloc);
        item.AddMember(JsonValue("range", alloc), range, alloc);
        item.AddMember(JsonValue("selectionRange", alloc), selectionRange, alloc);
        result.PushBack(item, alloc);
    }
    sendResponse(*id, result);
}

fs::path ModularityLspServer::uriToPath(const std::string& uri) {
    const std::string prefix = "file://";
    std::string decoded = uri;
    if (decoded.rfind(prefix, 0) == 0) {
        decoded = decoded.substr(prefix.size());
    }
#if defined(_WIN32)
    if (!decoded.empty() && decoded[0] == '/' && decoded.size() > 2 && decoded[2] == ':') {
        decoded.erase(decoded.begin());
    }
#endif
    return fs::path(percentDecode(decoded)).lexically_normal();
}

std::string ModularityLspServer::pathToUri(const fs::path& path) {
    fs::path absolute = fs::absolute(path).lexically_normal();
    std::string generic = absolute.generic_string();
#if defined(_WIN32)
    return "file:///" + generic;
#else
    return "file://" + generic;
#endif
}

std::string ModularityLspServer::readFileText(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string ModularityLspServer::getMethod(const JsonDocument& request) {
    if (request.HasMember("method") && request["method"].IsString()) {
        return request["method"].GetString();
    }
    return {};
}

const JsonValue* ModularityLspServer::getId(const JsonDocument& request) {
    if (request.HasMember("id")) {
        return &request["id"];
    }
    return nullptr;
}

rapidjson::Value ModularityLspServer::makeStringValue(const std::string& value, JsonAllocator& alloc) {
    JsonValue jsonValue;
    jsonValue.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), alloc);
    return jsonValue;
}

std::string ModularityLspServer::extractLine(const std::string& text, int line) {
    if (line < 0) return {};
    std::istringstream stream(text);
    std::string current;
    for (int i = 0; i <= line; ++i) {
        if (!std::getline(stream, current)) {
            return {};
        }
    }
    return current;
}

ModularityLspServer::CompletionPrefix ModularityLspServer::extractCompletionPrefix(const std::string& text, int line, int character,
                                                         ScriptLanguageServiceLanguage language) {
    const std::string currentLine = extractLine(text, line);
    const int clampedCharacter = std::clamp(character, 0, static_cast<int>(currentLine.size()));

    int start = clampedCharacter;
    while (start > 0) {
        const char c = currentLine[static_cast<size_t>(start - 1)];
        const bool isModuCppAccessor = language == ScriptLanguageServiceLanguage::ModuCPP && c == '.';
        if (!isIdentifierBody(static_cast<unsigned char>(c)) && c != ':' && !isModuCppAccessor) {
            break;
        }
        --start;
    }

    const std::string fullToken = currentLine.substr(
        static_cast<size_t>(start),
        static_cast<size_t>(clampedCharacter - start));

    const size_t colonPos = fullToken.rfind("::");
    if (colonPos != std::string::npos) {
        return { fullToken.substr(0, colonPos), fullToken.substr(colonPos + 2), true };
    }
    const size_t dotPos = fullToken.rfind('.');
    if (dotPos != std::string::npos) {
        return { fullToken.substr(0, dotPos), fullToken.substr(dotPos + 1), true };
    }

    return {"", fullToken, false};
}
