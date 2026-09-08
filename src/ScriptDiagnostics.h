#pragma once

#include "Common.h"

namespace Modularity {

enum class ScriptDiagnosticSeverity {
    Error,
    Warning,
    Info
};

enum class ScriptDiagnosticOrigin {
    Unknown,
    Parser,
    Transpiler,
    NativeCompile,
    ManagedBuild,
    Runtime
};

enum class ScriptDiagnosticId {
    MissingSemicolon = 1,
    NullVariable = 2,
    NullObjectReference = 3,
    MissingFunctionCallParentheses = 4,
    MissingAssignmentValue = 5,
    InvalidAssignment = 6,
    UndefinedVariable = 7,
    DuplicateVariable = 8,
    TypeMismatch = 9,
    UnexpectedToken = 10,
    UnclosedString = 11,
    UnclosedBlock = 12,
    InvalidStatement = 13,
    UndefinedFunction = 14,
    InvalidFunctionArguments = 15,
    TooFewArguments = 16,
    TooManyArguments = 17,
    ReturnTypeMismatch = 18,
    MissingImportTarget = 19,
    ImportAssignmentError = 20,
    ModuleNotFound = 21,
    DuplicateImport = 22,
    InvalidImportSyntax = 23,
    CircularImport = 24,
    InvalidIfStatement = 25,
    InvalidLoopSyntax = 26,
    BreakOutsideLoop = 27,
    ContinueOutsideLoop = 28,
    ReturnOutsideFunction = 29,
    EmptyList = 30,
    UnusedVariable = 31,
    EmptyFunction = 32,
    ScriptConfigUnavailable = 101,
    ScriptFileUnavailable = 102,
    ScriptParseFailed = 103,
    NativeCompileFailed = 104,
    NativeLinkFailed = 105,
    ManagedBuildFailed = 106,
    BuildEnvironmentUnavailable = 107,
    BuildWarning = 108,
    BuildNote = 109
};

struct ScriptDiagnosticDefinition {
    ScriptDiagnosticId id = ScriptDiagnosticId::InvalidStatement;
    const char* code = "MD013";
    ScriptDiagnosticSeverity severity = ScriptDiagnosticSeverity::Error;
    const char* name = "Invalid Statement";
    const char* defaultMessage = "This line is not a valid statement.";
    const char* defaultHint = "Check the syntax near the highlighted section.";
};

struct ScriptDiagnostic {
    ScriptDiagnosticId id = ScriptDiagnosticId::InvalidStatement;
    ScriptDiagnosticSeverity severity = ScriptDiagnosticSeverity::Error;
    ScriptDiagnosticOrigin origin = ScriptDiagnosticOrigin::Unknown;
    std::string code;
    std::string name;
    std::string message;
    std::string hint;
    std::string source;
    int line = 0;
    int column = 0;
    std::string sourceLine;
    std::string rawDetails;
};

const ScriptDiagnosticDefinition& getScriptDiagnosticDefinition(ScriptDiagnosticId id);
const char* scriptDiagnosticSeverityToString(ScriptDiagnosticSeverity severity);
const char* scriptDiagnosticOriginToString(ScriptDiagnosticOrigin origin);

ScriptDiagnostic makeScriptDiagnostic(ScriptDiagnosticId id,
                                      const std::string& source = {},
                                      int line = 0,
                                      const std::string& message = {},
                                      const std::string& hint = {},
                                      const std::string& rawDetails = {});

std::string formatScriptDiagnostic(const ScriptDiagnostic& diagnostic);

std::vector<ScriptDiagnostic> collectScriptDiagnostics(const fs::path& sourcePath,
                                                       const std::string& stageError,
                                                       const std::string& compileLog,
                                                       const std::string& linkLog,
                                                       bool managedBuild);

} // namespace Modularity
