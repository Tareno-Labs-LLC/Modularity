#include "Engine.h"
#include "EmbeddedScript.h"
#if defined(__ANDROID__)
#include "AndroidRuntime/AndroidRuntime.h"
#include "../include/Platform/AssetSource.h"
#include <sys/stat.h>
#include <fstream>
#include <mutex>
#include <sstream>

namespace {
constexpr const char* kAndroidAbi = "arm64-v8a";
std::mutex toolchainMutex;
}

fs::path Engine::ensureOnDeviceToolchain(std::string& error) {
    std::lock_guard<std::mutex> lock(toolchainMutex);
    const char* dataPath = Modularity::AndroidRuntime::GetInternalDataPath();
    if (!dataPath || !*dataPath) { error = "Android internal storage is unavailable."; return {}; }
    auto& assets = Modularity::Platform::GetAssetSource();
    const std::string assetBase = std::string("toolchain/") + kAndroidAbi;
    const auto listBytes = assets.ReadAll(assetBase + "/files.list");
    if (listBytes.empty()) {
        error = "No on-device compiler SDK is bundled. Run tools/setup-embedded-compilers.py, then build --Android --editor.";
        return {};
    }
    const auto versionBytes = assets.ReadAll(assetBase + "/bundle.id");
    const std::string version = "modu-toolchain-abi62:" + std::string(versionBytes.begin(), versionBytes.end());
    const fs::path root = fs::path(dataPath) / "toolchain" / kAndroidAbi;
    const fs::path stamp = root / ".stamp";
    std::ifstream previous(stamp, std::ios::binary);
    std::ostringstream oldVersion; oldVersion << previous.rdbuf();
    if (!versionBytes.empty() && oldVersion.str() == version && fs::exists(root / "headers/src/ScriptRuntime.h"))
        return root;
    previous.close();
    std::error_code ec;
    fs::remove_all(root, ec);
    if (!ec) fs::create_directories(root, ec);
    if (ec) { error = "Cannot prepare the on-device compiler SDK: " + ec.message(); return {}; }
    std::istringstream lines(std::string(listBytes.begin(), listBytes.end()));
    std::string line;
    size_t extracted = 0;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 3 || line[1] != ' ' || (line[0] != 'x' && line[0] != '-')) {
            error = "Invalid bundled compiler file list."; return {};
        }
        const fs::path relative = fs::path(line.substr(2)).lexically_normal();
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
            error = "Invalid bundled compiler file path."; return {};
        }
        const std::string asset = assetBase + "/" + relative.generic_string();
        if (!assets.Exists(asset)) { error = "Missing bundled compiler asset: " + asset; return {}; }
        const auto data = assets.ReadAll(asset);
        const fs::path destination = root / relative;
        fs::create_directories(destination.parent_path(), ec);
        if (ec) { error = "Cannot create compiler SDK directory: " + ec.message(); return {}; }
        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out || (line[0] == 'x' && ::chmod(destination.c_str(), 0755) != 0)) {
            error = "Cannot extract compiler SDK file: " + destination.string(); return {}; }
        ++extracted;
    }
    if (!extracted) { error = "The bundled compiler SDK is empty."; return {}; }
    std::ofstream out(stamp, std::ios::binary | std::ios::trunc);
    out << version;
    out.close();
    if (!out) { error = "Cannot save the compiler SDK extraction stamp."; return {}; }
    return root;
}

bool Engine::configureOnDeviceScriptCompile(ScriptBuildConfig& config, std::string& error) {
    const fs::path tc = ensureOnDeviceToolchain(error);
    if (tc.empty()) return false;
    config.embeddedCompiler = EmbeddedScript::Available();
    config.embeddedToolchain = tc;
    const fs::path clang = tc / "bin/clang";
    if (!config.embeddedCompiler && !fs::exists(clang)) {
        error = "This editor has neither embedded Cling/TinyCC nor a bundled external Clang compiler.";
        return false;
    }
    config.crossCompilerDriver = config.embeddedCompiler ? std::string{} : clang.string();
    config.linuxLinkLibs.clear();
    for (const char* relative : {"headers/src", "headers/include", "headers/src/ThirdParty",
             "headers/src/ThirdParty/glm", "headers/src/ThirdParty/glad", "headers/src/ThirdParty/ModuGUI",
             "headers/src/ThirdParty/ModuGUI/backends", "headers/src/ThirdParty/glfw/include",
             "headers/src/ThirdParty/openxr/include", "cling/include", "headers/glm-root"}) {
        if (fs::exists(tc / relative)) config.includeDirs.push_back(tc / relative);
    }
    config.defines.push_back("MODULARITY_RUNTIME_ONLY=0");
    config.defines.push_back("MODULARITY_PLAYER=0");
    config.defines.push_back("MODULARITY_OPENGL_ES=1");
    config.defines.push_back("MODULARITY_USE_MONO=0");
    config.defines.push_back("MODULARITY_ENABLE_ASSIMP=0");
#ifdef MODULARITY_HAS_OPENXR
    config.defines.push_back("MODULARITY_HAS_OPENXR=" + std::to_string(MODULARITY_HAS_OPENXR));
#endif
#ifdef MODULARITY_HAS_FFMPEG
    config.defines.push_back("MODULARITY_HAS_FFMPEG=" + std::to_string(MODULARITY_HAS_FFMPEG));
#endif
    const std::string target = "aarch64-linux-android26";
    const std::string sysroot = "--sysroot=" + (tc / "sysroot").string();
    auto& flags = config.extraCompileFlags;
    flags.insert(flags.end(), {"-target", target, sysroot, "-nostdinc++", "-isystem",
                               (tc / "sysroot/usr/include/c++/v1").string()});
    if (config.embeddedCompiler) {
        const fs::path resourceRoot = tc / "cling/lib/clang";
        std::error_code ec;
        fs::path resource;
        for (fs::directory_iterator it(resourceRoot, ec), end; !ec && it != end; it.increment(ec)) {
            if (fs::exists(it->path() / "include/stddef.h")) resource = it->path();
        }
        if (resource.empty()) { error = "The bundled Cling resource headers are missing."; return false; }
        flags.insert(flags.end(), {"-resource-dir", resource.string()});
    } else {
        config.extraLinkFlags.insert(config.extraLinkFlags.end(), {"-target", target, sysroot,
            "-fuse-ld=lld", "-s", "-lc++_shared", "-llog", "-lm", "-ldl", "-Wl,-z,max-page-size=16384"});
    }
    return true;
}
#else
fs::path Engine::ensureOnDeviceToolchain(std::string& error) {
    error = "On-device script compilation is only available on Android.";
    return {};
}
bool Engine::configureOnDeviceScriptCompile(ScriptBuildConfig&, std::string& error) {
    error = "On-device script compilation is only available on Android.";
    return false;
}
#endif
