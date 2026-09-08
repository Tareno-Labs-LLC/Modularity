#pragma once
#include "Common.h"
struct ModuCPPTranspileResult {
    std::string generatedSource;
    std::string className;
    bool generatedC = false;
    std::string cBackendNote;
};
class ModuCPPTranspiler {
public:
    static bool shouldTranspile(const fs::path& sourcePath, const std::string& sourceText);
    bool transpile(const fs::path& sourcePath, const std::string& sourceText, ModuCPPTranspileResult& outResult, std::string& error) const;
};
