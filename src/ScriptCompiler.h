#pragma once
#include "Common.h"
#include "EmbeddedScript.h"
inline std::string NativeScriptArtifactStem(const fs::path& sourcePath) {
    std::string stem = sourcePath.stem().string();
    std::string extension = sourcePath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".mko" || extension == ".modumako") {
        stem += ".modumako";
    }
    return stem;
}
inline constexpr const char* NativeScriptArtifactExtension() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

enum class ScriptOptimizationLevel {
    None,
    Speed
};
struct ScriptBuildConfig {
    std::string cppStandard = "c++23";
    ScriptOptimizationLevel optimization = ScriptOptimizationLevel::Speed;
    fs::path scriptsDir = "Scripts";
    fs::path outDir = "Cache/ScriptBin";
    std::vector<fs::path> includeDirs;
    std::vector<std::string> defines;
    std::vector<std::string> linuxLinkLibs;
    std::vector<std::string> windowsLinkLibs;
    std::string crossCompilerDriver;
    std::vector<std::string> extraCompileFlags;
    std::vector<std::string> extraLinkFlags;
    bool embeddedCompiler = false;
    fs::path embeddedToolchain;
};
struct ScriptBuildCommands {
    std::string compile;
    std::string link;
    fs::path objectPath;
    fs::path secondaryObjectPath;
    fs::path dependencyPath;
    fs::path secondaryDependencyPath;
    fs::path binaryPath;
    fs::path linkBinaryPath;
    fs::path wrapperPath;
    fs::path sourcePath;
    fs::path signaturePath;
    std::string buildSignature;
    bool usedWrapper = false;
    bool embeddedCompiler = false;
    EmbeddedScript::Request embeddedRequest;
};
struct ScriptCompileOutput {
    std::string compileLog;
    std::string linkLog;
    fs::path producedBinaryPath;
};
// Held by a background compile worker for the length of one job: everything the thread does,
// and every compiler process it spawns, runs below the editor's UI thread. Without it a 2-core
// machine hands the compilers both cores and the editor drops to a few fps for the whole batch.
// Foreground builds (export) do not take one and keep normal priority.
class ScriptBackgroundBuildScope {
    public:
        ScriptBackgroundBuildScope();
        ~ScriptBackgroundBuildScope();
        ScriptBackgroundBuildScope(const ScriptBackgroundBuildScope&) = delete;
        ScriptBackgroundBuildScope& operator=(const ScriptBackgroundBuildScope&) = delete;
    private:
        int previousPriority = 0;
        bool applied = false;
};
class ScriptCompiler {
    public:
        static void prewarmToolchainEnvironment();
        bool loadConfig(const fs::path& configPath, ScriptBuildConfig& outConfig, std::string& error) const;
        bool makeCommands(const ScriptBuildConfig& config, const fs::path& scriptPath, ScriptBuildCommands& outCommands, std::string& error) const;
        bool compile(const ScriptBuildCommands& commands, ScriptCompileOutput& output, std::string& error) const;
    private:
        static std::string trim(const std::string& value);
        static std::string escapeDefine(const std::string& def);
        static bool runCommand(const std::string& command, std::string& output);
        static std::string formatLinkFlag(const std::string& lib);
};
