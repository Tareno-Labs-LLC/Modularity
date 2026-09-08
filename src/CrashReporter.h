#pragma once
#include <functional>
#include <string>
namespace Modularity::CrashReporter {
    bool HandleCrashReporterMode(int argc, char** argv);
    void Initialize(const std::string& productName, const std::string& executablePath);
    int RunProtected(const std::function<int()>& entryPoint);
    void AppendLogLine(const std::string& line);
}