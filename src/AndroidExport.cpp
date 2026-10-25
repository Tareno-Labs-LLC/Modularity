#include "Engine.h"
#include "AndroidScript.h"
#include "AndroidToolchain.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
namespace fs = std::filesystem;
std::string Engine::resolveAndroidNdkPath() {
    return AndroidToolchain::FindNdkRoot().string();
}
namespace {
    fs::path resolveAndroidScriptCompiler(const fs::path& ndkRoot, const std::string& abi) {
        fs::path llvmBin;
        for (const char* host : {"linux-x86_64", "darwin-x86_64", "windows-x86_64"}) {
            const fs::path candidate = ndkRoot / "toolchains" / "llvm" / "prebuilt" / host / "bin";
            std::error_code ec;
            if (fs::is_directory(candidate, ec)) {llvmBin = candidate; break;}
        }
        if (llvmBin.empty()) return {};
        std::string triple;
        if (abi == "arm64-v8a")        triple = "aarch64-linux-android";
        else if (abi == "armeabi-v7a") triple = "armv7a-linux-androideabi";
        else if (abi == "x86_64")      triple = "x86_64-linux-android";
        else if (abi == "x86")         triple = "i686-linux-android";
        else                           return {};
        const std::string base = triple + "26-clang++";
        std::error_code ec;
        fs::path driver = llvmBin / base;
        if (fs::exists(driver, ec)) return driver;
        fs::path winDriver = llvmBin / (base + ".cmd");
        if (fs::exists(winDriver, ec)) return winDriver;
        return driver;
    }
    bool isAndroidScriptSource(const fs::path& path) {
        const std::string name = path.filename().string();
        if (name.find(".gen.cpp") != std::string::npos || name.find(".wrap.cpp") != std::string::npos) return false;
        const std::string ext = path.extension().string();
        return ext == ".moducpp" || ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c";
    }
}
bool Engine::crossCompileAndroidScripts(const ScriptBuildConfig& scriptConfig, const fs::path& projectRoot, const fs::path& ndkRoot, const std::string& androidAbi, const fs::path& libDir, const fs::path& runtimeStageRoot, const fs::path& compiledScriptsRel, const std::function<void(const std::string&)>& appendLog, std::string& error) {
    auto log = [&](const std::string& s) { if (appendLog) appendLog(s); };
    const fs::path compiler = resolveAndroidScriptCompiler(ndkRoot, androidAbi);
    std::error_code ec;
    if (compiler.empty() || !fs::exists(compiler, ec)) {
        error = "Android script cross-compiler not found (expected NDK clang++ at: " + compiler.string() + "). Is the NDK's llvm toolchain installed?"; return false;
    }
    fs::path scriptsDir = scriptConfig.scriptsDir;
    if (scriptsDir.empty()) scriptsDir = projectRoot / "Scripts";
    if (!scriptsDir.is_absolute()) scriptsDir = projectRoot / scriptsDir;
    const fs::path scriptOutDir = libDir.parent_path().parent_path() / "_android_script_build";
    fs::remove_all(scriptOutDir, ec);
    ec.clear();
    fs::create_directories(libDir, ec);
    fs::create_directories(scriptOutDir, ec);
    ScriptBuildConfig androidConfig = scriptConfig;
    androidConfig.outDir = scriptOutDir;
    androidConfig.scriptsDir = scriptsDir;
    androidConfig.crossCompilerDriver = compiler.string();
    androidConfig.linuxLinkLibs.clear();
    androidConfig.extraLinkFlags.push_back("-s");
    androidConfig.defines.push_back("MODULARITY_RUNTIME_ONLY=1");
    androidConfig.defines.push_back("MODULARITY_PLAYER=1");
    androidConfig.defines.push_back("MODULARITY_OPENGL_ES=1");
    androidConfig.defines.push_back("MODULARITY_USE_MONO=0");
    const std::string outDirKey = scriptOutDir.lexically_normal().string();
    const std::string scriptsDirKey = scriptsDir.lexically_normal().string();
    auto underDir = [](const std::string& key, const std::string& dirKey) {
        return !dirKey.empty() && key.size() >= dirKey.size() && key.compare(0, dirKey.size(), dirKey) == 0;
    };
    std::vector<fs::path> sources;
    {   std::error_code walkEc;
        for (auto it = fs::recursive_directory_iterator(projectRoot, walkEc); it != fs::recursive_directory_iterator(); it.increment(walkEc)) {
            if (walkEc) break;
            if (it->is_directory()) {
                const std::string name = it->path().filename().string();
                if (name == "Builds" || name == "build" || name == "Library" || name == "Cache" || name == ".git" || name == ".loaded" || name == ".staging") {
                    it.disable_recursion_pending();
                }   continue;
            }
            const fs::path& p = it->path();
            const std::string key = p.lexically_normal().string();
            if (underDir(key, outDirKey)) continue;
            if (!isAndroidScriptSource(p)) continue;
            const bool isModuCpp = (p.extension() == ".moducpp");
            if (isModuCpp || underDir(key, scriptsDirKey)) sources.push_back(p);
        }
    }
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    if (sources.empty()) {log("No script sources found in project; skipping cross-compilation."); return true;}
    log("Cross-compiling " + std::to_string(sources.size()) + " script(s) for " + androidAbi + " with " + compiler.filename().string() + " ...");
    int compiled = 0;
    for (const fs::path& source : sources) {
        ScriptBuildCommands commands;
        std::string buildError;
        if (!scriptCompiler.makeCommands(androidConfig, source, commands, buildError)) {
            error = "Android script transpile/setup failed for " + source.string() + ": " + buildError; return false;
        }
        ScriptCompileOutput output;
        if (!scriptCompiler.compile(commands, output, buildError)) {
            error = "Android script compile failed for " + source.string() + ": " + buildError; return false;
        }
        const fs::path produced = output.producedBinaryPath.empty() ? commands.binaryPath : output.producedBinaryPath;
        if (produced.empty() || !fs::exists(produced, ec)) {
            error = "Android script produced no binary for " + source.string(); return false;
        }
        fs::path relBinary = moduAndroidScriptRelativeBinary(commands.binaryPath, scriptOutDir);
        if (relBinary.empty()) relBinary = commands.binaryPath.filename();
        const std::string soname = moduAndroidScriptSoname(relBinary);
        const fs::path dest = libDir / soname;
        fs::copy_file(produced, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {error = "Failed to stage script library " + dest.string() + ": " + ec.message(); return false;}
        const fs::path placeholder = runtimeStageRoot / compiledScriptsRel / relBinary;
        std::error_code dirEc;
        fs::create_directories(placeholder.parent_path(), dirEc);
        std::ofstream(placeholder, std::ios::binary | std::ios::trunc).close();
        ++compiled;
    }
    log("Cross-compiled " + std::to_string(compiled) + " script(s) into apk/lib/" + androidAbi + "/.");
    fs::remove_all(scriptOutDir, ec);
    return true;
}
