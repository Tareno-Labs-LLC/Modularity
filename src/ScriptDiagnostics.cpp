#include "ScriptDiagnostics.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace Modularity {

namespace {

const ScriptDiagnosticDefinition kDefinitions[] = {
    { ScriptDiagnosticId::MissingSemicolon, "MD001", ScriptDiagnosticSeverity::Error,
      "Missing Semicolon", "Missing ';'.", "Add ';' at the end of the line." },
    { ScriptDiagnosticId::NullVariable, "MD002", ScriptDiagnosticSeverity::Error,
      "Null Variable", "Variable is null.", "Assign a value or remove '='." },
    { ScriptDiagnosticId::NullObjectReference, "MD003", ScriptDiagnosticSeverity::Warning,
      "Null Object Reference", "Object reference is empty.", "Assign an object if this reference is meant to be used." },
    { ScriptDiagnosticId::MissingFunctionCallParentheses, "MD004", ScriptDiagnosticSeverity::Error,
      "Missing Function Parentheses", "Function call is missing '()'.", "Add '()' to call the function correctly." },
    { ScriptDiagnosticId::MissingAssignmentValue, "MD005", ScriptDiagnosticSeverity::Error,
      "Missing Assignment Value", "Assignment is missing a value.", "Add a value after '='." },
    { ScriptDiagnosticId::InvalidAssignment, "MD006", ScriptDiagnosticSeverity::Error,
      "Invalid Assignment", "Invalid assignment.", "Use '=' to assign a value correctly." },
    { ScriptDiagnosticId::UndefinedVariable, "MD007", ScriptDiagnosticSeverity::Error,
      "Undefined Variable", "Variable does not exist.", "Declare it before using it." },
    { ScriptDiagnosticId::DuplicateVariable, "MD008", ScriptDiagnosticSeverity::Error,
      "Duplicate Variable", "Variable is already defined in this scope.", "Rename the variable or remove the duplicate declaration." },
    { ScriptDiagnosticId::TypeMismatch, "MD009", ScriptDiagnosticSeverity::Error,
      "Type Mismatch", "Assigned value has the wrong type.", "Use a value that matches the variable type." },
    { ScriptDiagnosticId::UnexpectedToken, "MD010", ScriptDiagnosticSeverity::Error,
      "Unexpected Token", "Unexpected token.", "Check the lines above for missing or invalid syntax." },
    { ScriptDiagnosticId::UnclosedString, "MD011", ScriptDiagnosticSeverity::Error,
      "Unclosed String", "String was opened but not closed.", "Add a closing '\"' character." },
    { ScriptDiagnosticId::UnclosedBlock, "MD012", ScriptDiagnosticSeverity::Error,
      "Unclosed Block", "A code block was opened but not closed.", "Add the missing '}'." },
    { ScriptDiagnosticId::InvalidStatement, "MD013", ScriptDiagnosticSeverity::Error,
      "Invalid Statement", "This line is not a valid statement.", "Check the syntax near the highlighted section." },
    { ScriptDiagnosticId::UndefinedFunction, "MD014", ScriptDiagnosticSeverity::Error,
      "Undefined Function", "Function does not exist.", "Check the spelling or define the function first." },
    { ScriptDiagnosticId::InvalidFunctionArguments, "MD015", ScriptDiagnosticSeverity::Error,
      "Invalid Function Arguments", "Function received invalid arguments.", "Check the argument types and order." },
    { ScriptDiagnosticId::TooFewArguments, "MD016", ScriptDiagnosticSeverity::Error,
      "Too Few Arguments", "Function is missing required arguments.", "Add all required arguments to the function call." },
    { ScriptDiagnosticId::TooManyArguments, "MD017", ScriptDiagnosticSeverity::Error,
      "Too Many Arguments", "Function received too many arguments.", "Remove extra arguments from the function call." },
    { ScriptDiagnosticId::ReturnTypeMismatch, "MD018", ScriptDiagnosticSeverity::Error,
      "Return Type Mismatch", "Returned value does not match the function type.", "Return a value that matches the function type." },
    { ScriptDiagnosticId::MissingImportTarget, "MD019", ScriptDiagnosticSeverity::Error,
      "Missing Import Target", "Import statement is missing a target module.", "Add the module name after 'import'." },
    { ScriptDiagnosticId::ImportAssignmentError, "MD020", ScriptDiagnosticSeverity::Error,
      "Import Assignment Error", "Imported value cannot be assigned here.", "Use an imported value that matches the expected type." },
    { ScriptDiagnosticId::ModuleNotFound, "MD021", ScriptDiagnosticSeverity::Error,
      "Module Not Found", "Module could not be found.", "Make sure the module exists and the name is correct." },
    { ScriptDiagnosticId::DuplicateImport, "MD022", ScriptDiagnosticSeverity::Warning,
      "Duplicate Import", "Module was imported more than once.", "Remove the duplicate import if it is not needed." },
    { ScriptDiagnosticId::InvalidImportSyntax, "MD023", ScriptDiagnosticSeverity::Error,
      "Invalid Import Syntax", "Import statement is not written correctly.", "Check the import format near this line." },
    { ScriptDiagnosticId::CircularImport, "MD024", ScriptDiagnosticSeverity::Error,
      "Circular Import", "Circular import detected.", "Remove the circular dependency." },
    { ScriptDiagnosticId::InvalidIfStatement, "MD025", ScriptDiagnosticSeverity::Error,
      "Invalid If Statement", "If statement is missing a valid condition.", "Add a valid condition inside the statement." },
    { ScriptDiagnosticId::InvalidLoopSyntax, "MD026", ScriptDiagnosticSeverity::Error,
      "Invalid Loop Syntax", "Loop statement is not written correctly.", "Check the loop syntax near this line." },
    { ScriptDiagnosticId::BreakOutsideLoop, "MD027", ScriptDiagnosticSeverity::Error,
      "Break Outside Loop", "'break' can only be used inside a loop.", "Move 'break' into a valid loop block." },
    { ScriptDiagnosticId::ContinueOutsideLoop, "MD028", ScriptDiagnosticSeverity::Error,
      "Continue Outside Loop", "'continue' can only be used inside a loop.", "Move 'continue' into a valid loop block." },
    { ScriptDiagnosticId::ReturnOutsideFunction, "MD029", ScriptDiagnosticSeverity::Error,
      "Return Outside Function", "'return' can only be used inside a function.", "Move 'return' into a valid function block." },
    { ScriptDiagnosticId::EmptyList, "MD030", ScriptDiagnosticSeverity::Warning,
      "Empty List", "List is empty.", "The script can still run, but no objects will be processed." },
    { ScriptDiagnosticId::UnusedVariable, "MD031", ScriptDiagnosticSeverity::Warning,
      "Unused Variable", "Variable was declared but never used.", "Remove it or use it in the script." },
    { ScriptDiagnosticId::EmptyFunction, "MD032", ScriptDiagnosticSeverity::Info,
      "Empty Function", "Function is empty.", "Add logic to the function or remove it if it is not needed." },
    { ScriptDiagnosticId::ScriptConfigUnavailable, "MD101", ScriptDiagnosticSeverity::Error,
      "Script Config Unavailable", "Scripts.modu could not be loaded.", "Make sure the file exists and can be read." },
    { ScriptDiagnosticId::ScriptFileUnavailable, "MD102", ScriptDiagnosticSeverity::Error,
      "Script File Unavailable", "Script file could not be loaded.", "Make sure the script exists and can be read." },
    { ScriptDiagnosticId::ScriptParseFailed, "MD103", ScriptDiagnosticSeverity::Error,
      "Script Parse Failed", "Script could not be parsed.", "Check the syntax near the reported code." },
    { ScriptDiagnosticId::NativeCompileFailed, "MD104", ScriptDiagnosticSeverity::Error,
      "Native Compile Failed", "Script could not be compiled.", "Fix the errors above and try again." },
    { ScriptDiagnosticId::NativeLinkFailed, "MD105", ScriptDiagnosticSeverity::Error,
      "Native Link Failed", "Script could not be linked.", "Check the build output for missing libraries or exported functions." },
    { ScriptDiagnosticId::ManagedBuildFailed, "MD106", ScriptDiagnosticSeverity::Error,
      "Managed Build Failed", "Managed script build failed.", "Fix the C# errors and build again." },
    { ScriptDiagnosticId::BuildEnvironmentUnavailable, "MD107", ScriptDiagnosticSeverity::Error,
      "Build Environment Unavailable", "Script build environment is not ready.", "Install the required build tools or rebuild the host target first." },
    { ScriptDiagnosticId::BuildWarning, "MD108", ScriptDiagnosticSeverity::Warning,
      "Build Warning", "Compiler warning.", "Review the warning and adjust the code if needed." },
    { ScriptDiagnosticId::BuildNote, "MD109", ScriptDiagnosticSeverity::Info,
      "Build Note", "Build note.", "Review the build output for more detail." }
};

std::string trimCopy(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool containsAny(const std::string& value, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (value.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct DiagnosticNormalizationContext {
    ScriptDiagnosticSeverity severity = ScriptDiagnosticSeverity::Error;
    ScriptDiagnosticOrigin origin = ScriptDiagnosticOrigin::Unknown;
    std::string source;
    fs::path sourcePath;
    int line = 0;
    int column = 0;
    std::string sourceLine;
    std::string message;
    std::string rawLine;
};

struct DiagnosticCandidateMatch {
    ScriptDiagnosticId id = ScriptDiagnosticId::InvalidStatement;
    int priority = -1;
    std::string message;
    std::string hint;
    int line = -1;
    int column = -1;
};

std::string ensureFunctionCallName(const std::string& name) {
    if (name.empty()) return "Function";
    if (name.size() >= 2 && name.substr(name.size() - 2) == "()") {
        return name;
    }
    return name + "()";
}

std::string extractQuotedValue(const std::string& text) {
    static const std::regex singleQuotePattern(R"('([^']+)')");
    static const std::regex doubleQuotePattern("\"([^\"]+)\"");
    std::smatch match;
    if (std::regex_search(text, match, singleQuotePattern)) {
        return match[1].str();
    }
    if (std::regex_search(text, match, doubleQuotePattern)) {
        return match[1].str();
    }
    return {};
}

std::string extractFunctionName(const std::string& text) {
    static const std::regex patterns[] = {
        std::regex(R"(call to '([^']+)')"),
        std::regex(R"(function '([^']+)')"),
        std::regex(R"(named '([^']+)')"),
        std::regex("\"([^\"]+)\"\\s*:"),
        std::regex("'([^']+)'\\s*:"),
    };

    std::smatch match;
    for (const auto& pattern : patterns) {
        if (std::regex_search(text, match, pattern)) {
            return trimCopy(match[1].str());
        }
    }
    return {};
}

int extractLineNumberFromText(const std::string& text) {
    static const std::regex linePattern(R"((?:^|[^A-Za-z])line\s+(\d+))", std::regex_constants::icase);
    std::smatch match;
    if (std::regex_search(text, match, linePattern)) {
        return std::max(0, std::atoi(match[1].str().c_str()));
    }
    return 0;
}

int extractColumnNumberFromText(const std::string& text) {
    static const std::regex columnPattern(R"((?:^|[^A-Za-z])column\s+(\d+))", std::regex_constants::icase);
    std::smatch match;
    if (std::regex_search(text, match, columnPattern)) {
        return std::max(0, std::atoi(match[1].str().c_str()));
    }
    return 0;
}

std::string readSourceLine(const fs::path& path, int lineNumber) {
    if (path.empty() || lineNumber <= 0) {
        return {};
    }

    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }

    std::string line;
    for (int currentLine = 1; std::getline(input, line); ++currentLine) {
        if (currentLine == lineNumber) {
            return line;
        }
    }

    return {};
}

int readModuMakoSourceLine(const fs::path& generatedPath, int generatedLine) {
    if (generatedPath.empty() || generatedLine <= 0) return 0;
    std::ifstream input(generatedPath);
    if (!input.is_open()) return 0;

    static const std::regex markerPattern(R"(^\s*//\s*@modumako-source-line\s+(\d+)\s*$)");
    std::string line;
    int mappedLine = 0;
    for (int currentLine = 1; currentLine <= generatedLine && std::getline(input, line); ++currentLine) {
        std::smatch match;
        if (std::regex_match(line, match, markerPattern)) {
            mappedLine = std::max(0, std::atoi(match[1].str().c_str()));
        }
    }
    return mappedLine;
}

void remapModuMakoDiagnostics(const fs::path& sourcePath, std::vector<ScriptDiagnostic>& diagnostics) {
    const std::string ext = toLowerCopy(sourcePath.extension().string());
    if (ext != ".mko" && ext != ".modumako") return;

    static const std::regex locationPattern(R"(^(.+?):(\d+)(?::\d+)?:)");
    for (ScriptDiagnostic& diagnostic : diagnostics) {
        std::smatch match;
        if (!std::regex_search(diagnostic.rawDetails, match, locationPattern)) continue;
        const fs::path generatedPath = trimCopy(match[1].str());
        if (generatedPath.filename().string().find(".modumako.gen.moducpp") == std::string::npos) continue;
        const int generatedLine = std::max(0, std::atoi(match[2].str().c_str()));
        const int mappedLine = readModuMakoSourceLine(generatedPath, generatedLine);
        if (mappedLine <= 0) continue;
        diagnostic.source = sourcePath.filename().string();
        diagnostic.line = mappedLine;
        diagnostic.sourceLine = readSourceLine(sourcePath, mappedLine);
    }
}

int endOfLineColumn(const std::string& sourceLine) {
    if (sourceLine.empty()) {
        return 0;
    }

    size_t end = sourceLine.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) {
        return 1;
    }
    return static_cast<int>(end) + 2;
}

bool isGeneratedScriptPath(const fs::path& path) {
    const std::string name = path.filename().string();
    return containsAny(name, { ".moducpp.gen.cpp", ".wrap.cpp", ".capi.wrap.cpp" });
}

std::string displaySourceFromPath(const fs::path& rawPath, const fs::path& fallbackPath) {
    fs::path path = rawPath;
    if (path.empty()) {
        path = fallbackPath;
    }

    if (path.empty()) {
        return {};
    }

    const std::string filename = path.filename().string();
    if (filename.size() > std::string(".moducpp.gen.cpp").size() &&
        filename.rfind(".moducpp.gen.cpp") == filename.size() - std::string(".moducpp.gen.cpp").size()) {
        return filename.substr(0, filename.size() - std::string(".gen.cpp").size());
    }

    if (isGeneratedScriptPath(path) && !fallbackPath.empty()) {
        return fallbackPath.filename().string();
    }

    return filename;
}

ScriptDiagnostic makeGenericBuildDiagnostic(ScriptDiagnosticSeverity severity,
                                            const std::string& source,
                                            int line,
                                            const std::string& rawLine) {
    if (severity == ScriptDiagnosticSeverity::Warning) {
        return makeScriptDiagnostic(ScriptDiagnosticId::BuildWarning, source, line, {}, {}, rawLine);
    }
    if (severity == ScriptDiagnosticSeverity::Info) {
        return makeScriptDiagnostic(ScriptDiagnosticId::BuildNote, source, line, {}, {}, rawLine);
    }
    return makeScriptDiagnostic(ScriptDiagnosticId::NativeCompileFailed, source, line, {}, {}, rawLine);
}

void addCandidate(std::vector<DiagnosticCandidateMatch>& out,
                  ScriptDiagnosticId id,
                  int priority,
                  const std::string& message = {},
                  const std::string& hint = {},
                  int line = -1,
                  int column = -1) {
    DiagnosticCandidateMatch candidate;
    candidate.id = id;
    candidate.priority = priority;
    candidate.message = message;
    candidate.hint = hint;
    candidate.line = line;
    candidate.column = column;
    out.push_back(std::move(candidate));
}

ScriptDiagnostic finalizeCandidate(const DiagnosticNormalizationContext& context,
                                   const DiagnosticCandidateMatch& candidate) {
    ScriptDiagnostic diagnostic = makeScriptDiagnostic(
        candidate.id,
        context.source,
        candidate.line > 0 ? candidate.line : context.line,
        candidate.message,
        candidate.hint,
        context.rawLine.empty() ? context.message : context.rawLine);
    diagnostic.origin = context.origin;
    diagnostic.column = candidate.column > 0 ? candidate.column : context.column;
    diagnostic.sourceLine = context.sourceLine;
    return diagnostic;
}

ScriptDiagnostic normalizeDiagnostic(const DiagnosticNormalizationContext& context) {
    const std::string lower = toLowerCopy(context.message);
    const std::string quoted = extractQuotedValue(context.message);
    const std::string functionName = extractFunctionName(context.message);
    std::vector<DiagnosticCandidateMatch> candidates;

    const bool hasLoopContext =
        containsAny(lower, { "for", "while", "loop" }) ||
        containsAny(toLowerCopy(context.sourceLine), { "for", "while" });
    const bool hasIfContext =
        lower.find("if") != std::string::npos ||
        toLowerCopy(context.sourceLine).find("if") != std::string::npos;

    if (containsAny(lower, { "; expected", "expected ';'", "missing ';'", "expected `;`", "expected ;" })) {
        addCandidate(
            candidates,
            ScriptDiagnosticId::MissingSemicolon,
            100,
            {},
            {},
            context.line,
            endOfLineColumn(context.sourceLine));
    }

    if (containsAny(lower, { "missing terminating", "unterminated string", "newline in constant" })) {
        addCandidate(candidates, ScriptDiagnosticId::UnclosedString, 95);
    }

    if (containsAny(lower, { "expected '}'", "'}' expected", "missing '}'", "unmatched '{'" })) {
        addCandidate(candidates, ScriptDiagnosticId::UnclosedBlock, 95);
    }

    if (containsAny(lower, { "break statement not within loop", "break not within loop" })) {
        addCandidate(candidates, ScriptDiagnosticId::BreakOutsideLoop, 95);
    }

    if (containsAny(lower, { "continue statement not within loop", "continue not within loop" })) {
        addCandidate(candidates, ScriptDiagnosticId::ContinueOutsideLoop, 95);
    }

    if (containsAny(lower, { "return statement not in function", "return-statement with no function" })) {
        addCandidate(candidates, ScriptDiagnosticId::ReturnOutsideFunction, 95);
    }

    if (containsAny(lower, { "too few arguments", "requires more arguments", "not enough arguments" })) {
        if (!functionName.empty()) {
            addCandidate(
                candidates,
                ScriptDiagnosticId::TooFewArguments,
                90,
                "Function '" + ensureFunctionCallName(functionName) + "' is missing required arguments.");
        } else {
            addCandidate(candidates, ScriptDiagnosticId::TooFewArguments, 90);
        }
    }

    if (containsAny(lower, { "too many arguments", "does not take", "takes 0 arguments", "takes 1 argument" })) {
        if (!functionName.empty()) {
            addCandidate(
                candidates,
                ScriptDiagnosticId::TooManyArguments,
                90,
                "Function '" + ensureFunctionCallName(functionName) + "' received too many arguments.");
        } else {
            addCandidate(candidates, ScriptDiagnosticId::TooManyArguments, 90);
        }
    }

    if (containsAny(lower, { "unused variable", "unreferenced local variable", "declared but never referenced",
                             "declared but never used" })) {
        if (!quoted.empty()) {
            addCandidate(
                candidates,
                ScriptDiagnosticId::UnusedVariable,
                85,
                "Variable '" + quoted + "' was declared but never used.");
        } else {
            addCandidate(candidates, ScriptDiagnosticId::UnusedVariable, 85);
        }
    }

    if (containsAny(lower, { "redefinition", "already defined", "already declared", "duplicate member" })) {
        if (!quoted.empty()) {
            addCandidate(
                candidates,
                ScriptDiagnosticId::DuplicateVariable,
                85,
                "Variable '" + quoted + "' is already defined in this scope.");
        } else {
            addCandidate(candidates, ScriptDiagnosticId::DuplicateVariable, 85);
        }
    }

    if (containsAny(lower, { "undeclared identifier", "was not declared in this scope",
                             "does not exist in the current context", "identifier not found" })) {
        if (!quoted.empty()) {
            addCandidate(
                candidates,
                ScriptDiagnosticId::UndefinedVariable,
                85,
                "Variable '" + quoted + "' does not exist.");
        } else {
            addCandidate(candidates, ScriptDiagnosticId::UndefinedVariable, 85);
        }
    }

    if (containsAny(lower, { "undefined reference to", "no member named", "is not a member of",
                             "could not be found" }) && !functionName.empty()) {
        addCandidate(
            candidates,
            ScriptDiagnosticId::UndefinedFunction,
            85,
            "Function '" + ensureFunctionCallName(functionName) + "' does not exist.");
    }

    if (containsAny(lower, { "no matching function", "no instance of overloaded function matches",
                             "candidate function not viable", "cannot convert argument",
                             "cannot convert parameter" })) {
        if (!functionName.empty()) {
            addCandidate(
                candidates,
                ScriptDiagnosticId::InvalidFunctionArguments,
                85,
                "Function '" + ensureFunctionCallName(functionName) + "' received invalid arguments.");
        } else {
            addCandidate(candidates, ScriptDiagnosticId::InvalidFunctionArguments, 85);
        }
    }

    if (containsAny(lower, { "return-statement", "return statement", "cannot convert return",
                             "cannot return", "must return" })) {
        if (!functionName.empty()) {
            addCandidate(
                candidates,
                ScriptDiagnosticId::ReturnTypeMismatch,
                85,
                "Function '" + ensureFunctionCallName(functionName) + "' returned the wrong type.");
        } else {
            addCandidate(candidates, ScriptDiagnosticId::ReturnTypeMismatch, 85);
        }
    }

    if (containsAny(lower, { "cannot convert", "invalid conversion", "cannot initialize",
                             "no viable conversion", "cannot assign" })) {
        addCandidate(candidates, ScriptDiagnosticId::TypeMismatch, 80);
    }

    if (containsAny(lower, { "unexpected token", "invalid token", "expected expression",
                             "expected unqualified-id", "expected primary-expression" })) {
        if (!quoted.empty()) {
            addCandidate(
                candidates,
                ScriptDiagnosticId::UnexpectedToken,
                80,
                "Unexpected token '" + quoted + "'.");
        } else {
            addCandidate(candidates, ScriptDiagnosticId::UnexpectedToken, 80);
        }
    }

    if (hasIfContext && containsAny(lower, { "expected ')'", "missing condition", "invalid condition" })) {
        addCandidate(candidates, ScriptDiagnosticId::InvalidIfStatement, 20);
    }

    if (hasLoopContext && containsAny(lower, { "expected", "invalid", "syntax" })) {
        addCandidate(candidates, ScriptDiagnosticId::InvalidLoopSyntax, 20);
    }

    if (!candidates.empty()) {
        const auto bestIt = std::max_element(
            candidates.begin(),
            candidates.end(),
            [](const DiagnosticCandidateMatch& a, const DiagnosticCandidateMatch& b) {
                return a.priority < b.priority;
            });
        return finalizeCandidate(context, *bestIt);
    }

    ScriptDiagnostic diagnostic =
        context.severity == ScriptDiagnosticSeverity::Warning
            ? makeGenericBuildDiagnostic(context.severity, context.source, context.line, context.rawLine)
            : makeScriptDiagnostic(
                ScriptDiagnosticId::InvalidStatement,
                context.source,
                context.line,
                {},
                {},
                context.rawLine.empty() ? context.message : context.rawLine);
    diagnostic.origin = context.origin;
    diagnostic.column = context.column;
    diagnostic.sourceLine = context.sourceLine;
    return diagnostic;
}

std::vector<ScriptDiagnostic> parseCompilerLog(const fs::path& fallbackSourcePath,
                                               const std::string& logText,
                                               bool managedBuild) {
    std::vector<ScriptDiagnostic> diagnostics;
    std::istringstream stream(logText);
    std::string lineText;

    static const std::regex gccClangPattern(
        R"(^(.+?):(\d+)(?::(\d+))?:\s*(fatal error|error|warning|note):\s*(.+)$)",
        std::regex_constants::icase);
    static const std::regex msvcPattern(
        R"(^(.+?)\((\d+)(?:,(\d+))?\)\s*:\s*(fatal error|error|warning|note)\s+(?:[A-Z]{1,4}\d+):\s*(.+)$)",
        std::regex_constants::icase);

    while (std::getline(stream, lineText)) {
        const std::string trimmed = trimCopy(lineText);
        if (trimmed.empty()) {
            continue;
        }

        std::smatch match;
        fs::path rawSourcePath;
        int lineNumber = 0;
        int columnNumber = 0;
        ScriptDiagnosticSeverity severity = ScriptDiagnosticSeverity::Error;
        std::string message;

        if (std::regex_match(trimmed, match, gccClangPattern)) {
            rawSourcePath = fs::path(trimCopy(match[1].str()));
            lineNumber = std::max(0, std::atoi(match[2].str().c_str()));
            columnNumber = match[3].matched ? std::max(0, std::atoi(match[3].str().c_str())) : 0;
            const std::string severityText = toLowerCopy(match[4].str());
            if (severityText == "warning") severity = ScriptDiagnosticSeverity::Warning;
            else if (severityText == "note") severity = ScriptDiagnosticSeverity::Info;
            message = trimCopy(match[5].str());
        } else if (std::regex_match(trimmed, match, msvcPattern)) {
            rawSourcePath = fs::path(trimCopy(match[1].str()));
            lineNumber = std::max(0, std::atoi(match[2].str().c_str()));
            columnNumber = match[3].matched ? std::max(0, std::atoi(match[3].str().c_str())) : 0;
            const std::string severityText = toLowerCopy(match[4].str());
            if (severityText == "warning") severity = ScriptDiagnosticSeverity::Warning;
            else if (severityText == "note") severity = ScriptDiagnosticSeverity::Info;
            message = trimCopy(match[5].str());
        } else {
            continue;
        }

        const bool generatedSource = isGeneratedScriptPath(rawSourcePath);
        const std::string displaySource = displaySourceFromPath(rawSourcePath, fallbackSourcePath);
        fs::path sourceLinePath = rawSourcePath;
        if (sourceLinePath.empty() || generatedSource) {
            sourceLinePath = fallbackSourcePath;
        }

        DiagnosticNormalizationContext context;
        context.severity = severity;
        context.origin = managedBuild ? ScriptDiagnosticOrigin::ManagedBuild : ScriptDiagnosticOrigin::NativeCompile;
        context.source = displaySource;
        context.sourcePath = sourceLinePath;
        context.line = generatedSource ? 0 : lineNumber;
        context.column = generatedSource ? 0 : columnNumber;
        context.sourceLine = (generatedSource || lineNumber <= 0) ? std::string() : readSourceLine(sourceLinePath, lineNumber);
        context.message = message;
        context.rawLine = trimmed;
        diagnostics.push_back(normalizeDiagnostic(context));
    }

    return diagnostics;
}

ScriptDiagnostic parseStageError(const fs::path& sourcePath,
                                 const std::string& stageError,
                                 bool managedBuild) {
    const std::string errorText = trimCopy(stageError);
    const std::string lower = toLowerCopy(errorText);
    const std::string source = displaySourceFromPath(sourcePath, sourcePath);
    const int line = extractLineNumberFromText(errorText);
    const int column = extractColumnNumberFromText(errorText);
    const ScriptDiagnosticOrigin origin =
        lower.rfind("moducpp transpile failed:", 0) == 0
            ? ScriptDiagnosticOrigin::Transpiler
            : managedBuild
                ? ScriptDiagnosticOrigin::ManagedBuild
                : ScriptDiagnosticOrigin::Parser;

    DiagnosticNormalizationContext context;
    context.severity = ScriptDiagnosticSeverity::Error;
    context.origin = origin;
    context.source = source;
    context.sourcePath = sourcePath;
    context.line = line;
    context.column = column;
    context.sourceLine = readSourceLine(sourcePath, line);
    context.message = lower.rfind("moducpp transpile failed:", 0) == 0
        ? trimCopy(errorText.substr(std::string("ModuCPP transpile failed:").size()))
        : errorText;
    context.rawLine = errorText;

    if (containsAny(lower, { "config file not found", "unable to open config file" })) {
        ScriptDiagnostic diagnostic = makeScriptDiagnostic(
            ScriptDiagnosticId::ScriptConfigUnavailable, source, line, {}, {}, errorText);
        diagnostic.origin = origin;
        diagnostic.sourceLine = context.sourceLine;
        return diagnostic;
    }

    if (containsAny(lower, { "script file not found", "unable to read script file", "missing managed assembly" })) {
        ScriptDiagnostic diagnostic = makeScriptDiagnostic(
            ScriptDiagnosticId::ScriptFileUnavailable, source, line, {}, {}, errorText);
        diagnostic.origin = origin;
        diagnostic.sourceLine = context.sourceLine;
        return diagnostic;
    }

    if (containsAny(lower, { "failed to spawn process", "failed to launch dotnet build",
                             "unable to locate the host import library" })) {
        ScriptDiagnostic diagnostic = makeScriptDiagnostic(
            ScriptDiagnosticId::BuildEnvironmentUnavailable, source, line, {}, {}, errorText);
        diagnostic.origin = origin;
        diagnostic.sourceLine = context.sourceLine;
        return diagnostic;
    }

    if (managedBuild) {
        ScriptDiagnostic diagnostic = normalizeDiagnostic(context);
        if (diagnostic.id == ScriptDiagnosticId::InvalidStatement) {
            diagnostic = makeScriptDiagnostic(ScriptDiagnosticId::ManagedBuildFailed, source, line, {}, {}, errorText);
            diagnostic.origin = origin;
            diagnostic.sourceLine = context.sourceLine;
        }
        return diagnostic;
    }

    if (lower == "link failed") {
        ScriptDiagnostic diagnostic = makeScriptDiagnostic(
            ScriptDiagnosticId::NativeLinkFailed, source, line, {}, {}, errorText);
        diagnostic.origin = origin;
        diagnostic.sourceLine = context.sourceLine;
        return diagnostic;
    }
    if (lower == "compile failed") {
        ScriptDiagnostic diagnostic = makeScriptDiagnostic(
            ScriptDiagnosticId::NativeCompileFailed, source, line, {}, {}, errorText);
        diagnostic.origin = origin;
        diagnostic.sourceLine = context.sourceLine;
        return diagnostic;
    }

    ScriptDiagnostic diagnostic = normalizeDiagnostic(context);
    if (diagnostic.id == ScriptDiagnosticId::InvalidStatement && origin == ScriptDiagnosticOrigin::Transpiler) {
        diagnostic = makeScriptDiagnostic(ScriptDiagnosticId::ScriptParseFailed, source, line, {}, {}, errorText);
        diagnostic.origin = origin;
        diagnostic.sourceLine = context.sourceLine;
    }
    return diagnostic;
}

void appendUniqueDiagnostics(std::vector<ScriptDiagnostic>& out,
                             std::unordered_set<std::string>& seen,
                             const std::vector<ScriptDiagnostic>& incoming) {
    for (const ScriptDiagnostic& diagnostic : incoming) {
        const std::string key =
            std::to_string(static_cast<int>(diagnostic.id)) + "|" +
            std::to_string(static_cast<int>(diagnostic.severity)) + "|" +
            diagnostic.source + "|" +
            std::to_string(diagnostic.line) + "|" +
            diagnostic.message;
        if (!seen.insert(key).second) {
            continue;
        }
        out.push_back(diagnostic);
    }
}

} // namespace

const ScriptDiagnosticDefinition& getScriptDiagnosticDefinition(ScriptDiagnosticId id) {
    for (const ScriptDiagnosticDefinition& definition : kDefinitions) {
        if (definition.id == id) {
            return definition;
        }
    }
    return kDefinitions[12];
}

const char* scriptDiagnosticSeverityToString(ScriptDiagnosticSeverity severity) {
    switch (severity) {
        case ScriptDiagnosticSeverity::Error:
            return "Error";
        case ScriptDiagnosticSeverity::Warning:
            return "Warning";
        case ScriptDiagnosticSeverity::Info:
            return "Info";
        default:
            return "Info";
    }
}

const char* scriptDiagnosticOriginToString(ScriptDiagnosticOrigin origin) {
    switch (origin) {
        case ScriptDiagnosticOrigin::Parser:
            return "Parser";
        case ScriptDiagnosticOrigin::Transpiler:
            return "Transpiler";
        case ScriptDiagnosticOrigin::NativeCompile:
            return "Native Compile";
        case ScriptDiagnosticOrigin::ManagedBuild:
            return "Managed Build";
        case ScriptDiagnosticOrigin::Runtime:
            return "Runtime";
        case ScriptDiagnosticOrigin::Unknown:
        default:
            return "Unknown";
    }
}

ScriptDiagnostic makeScriptDiagnostic(ScriptDiagnosticId id,
                                      const std::string& source,
                                      int line,
                                      const std::string& message,
                                      const std::string& hint,
                                      const std::string& rawDetails) {
    const ScriptDiagnosticDefinition& definition = getScriptDiagnosticDefinition(id);
    ScriptDiagnostic diagnostic;
    diagnostic.id = id;
    diagnostic.severity = definition.severity;
    diagnostic.code = definition.code;
    diagnostic.name = definition.name;
    diagnostic.message = message.empty() ? definition.defaultMessage : message;
    diagnostic.hint = hint.empty() ? definition.defaultHint : hint;
    diagnostic.source = source;
    diagnostic.line = line;
    diagnostic.column = 0;
    diagnostic.rawDetails = rawDetails;
    return diagnostic;
}

std::string formatScriptDiagnostic(const ScriptDiagnostic& diagnostic) {
    std::ostringstream out;
    out << "[" << scriptDiagnosticSeverityToString(diagnostic.severity) << "]["
        << diagnostic.code << "] " << diagnostic.message;
    if (!diagnostic.hint.empty()) {
        out << "\nHint: " << diagnostic.hint;
    }
    if (!diagnostic.source.empty()) {
        out << "\nSource: " << diagnostic.source;
    }
    if (diagnostic.line > 0) {
        out << "\nLine: " << diagnostic.line;
    }
    return out.str();
}

std::vector<ScriptDiagnostic> collectScriptDiagnostics(const fs::path& sourcePath,
                                                       const std::string& stageError,
                                                       const std::string& compileLog,
                                                       const std::string& linkLog,
                                                       bool managedBuild) {
    std::vector<ScriptDiagnostic> diagnostics;
    std::unordered_set<std::string> seen;

    appendUniqueDiagnostics(diagnostics, seen, parseCompilerLog(sourcePath, compileLog, managedBuild));
    appendUniqueDiagnostics(diagnostics, seen, parseCompilerLog(sourcePath, linkLog, managedBuild));

    if (!trimCopy(stageError).empty() && diagnostics.empty()) {
        appendUniqueDiagnostics(
            diagnostics,
            seen,
            std::vector<ScriptDiagnostic>{ parseStageError(sourcePath, stageError, managedBuild) });
    }

    remapModuMakoDiagnostics(sourcePath, diagnostics);
    return diagnostics;
}

} // namespace Modularity
