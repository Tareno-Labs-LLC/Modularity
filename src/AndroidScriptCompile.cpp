#include "Engine.h"
#if defined(__ANDROID__)
#include "AndroidRuntime/AndroidRuntime.h"
#include "../include/Platform/AssetSource.h"
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
namespace {constexpr const char *kAndroidAbi = "arm64-v8a"; constexpr const char *kToolchainStamp = "modu-toolchain-v2";}
fs::path Engine::ensureOnDeviceToolchain(std::string &error) {
    const char *dataPath = Modularity::AndroidRuntime::GetInternalDataPath();
    if (!dataPath || !*dataPath) {error = "The Android internal data path is unavailable; thus, it cannot stage the compiler."; return {};}
    const fs::path root = fs::path(dataPath) / "toolchain" / kAndroidAbi;
    const fs::path stampPath = root / ".stamp";
    {std::ifstream in(stampPath); std::string have; if (in && std::getline(in, have) && have == kToolchainStamp) {return root;}}
    auto &assets = Modularity::Platform::GetAssetSource();
    const std::string assetBase = std::string("toolchain/") + kAndroidAbi;
    const std::string listAsset = assetBase + "/files.list";
    if (!assets.Exists(listAsset)) {error = "The on-device compiler toolchain is not bundled in this build (You're missing " "asset '" + listAsset + "'). The on-device compiling is an editor-only " "feature - please rebuild with ./build.sh --Android --editor."; return {};}
    const std::vector<uint8_t> listBytes = assets.ReadAll(listAsset);
    if (listBytes.empty()) {error = "The Engine failed to read the bundled toolchain file list."; return {};}
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    std::istringstream lines(std::string(listBytes.begin(), listBytes.end()));
    std::string line;
    size_t extracted = 0;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 3) continue;
        const bool exec = (line[0] == 'x');
        const std::string rel = line.substr(2);
        if (rel.empty()) continue;
        const std::vector<uint8_t> data = assets.ReadAll(assetBase + "/" + rel);
        const fs::path dest = root / rel;
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {error = "The Engine failed to write extracted toolchain file: " + dest.string(); return {};}
        if (!data.empty()) {out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));}
        out.close();
        if (exec) {::chmod(dest.c_str(), 0755);}
        ++extracted;
    }
    if (extracted == 0) {error = "The Bundled toolchain file list was empty."; return {};}
    std::ofstream(stampPath, std::ios::trunc) << kToolchainStamp << "\n"; return root;
}
bool Engine::configureOnDeviceScriptCompile(ScriptBuildConfig &config, std::string &error) {
    const fs::path tc = ensureOnDeviceToolchain(error);
    if (tc.empty()) return false;
    const fs::path clang = tc / "bin" / "clang";
    const fs::path lld = tc / "bin" / "ld.lld";
    const fs::path sysroot = tc / "sysroot";
    const fs::path headersInclude = tc / "headers" / "include";
    const fs::path headersGlm = tc / "headers" / "glm-root";
    std::error_code ec;
    if (!fs::exists(clang, ec)) {error = "The Engine could not find a bundled clang binary at " + clang.string(); return false;}
    const std::string target = "aarch64-linux-android26";
    const std::string sysrootFlag = "--sysroot=" + sysroot.string();
    config.crossCompilerDriver = clang.string();
    config.linuxLinkLibs.clear();
    auto addCompile = [&](const std::string &flag) {config.extraCompileFlags.push_back(flag);};
    addCompile("-target");
    addCompile(target);
    addCompile(sysrootFlag);
    addCompile("-x");
    addCompile("c++");
    addCompile("-fvisibility=hidden");
    addCompile("-isystem");
    addCompile((sysroot / "usr" / "include" / "c++" / "v1").string());
    if (fs::exists(headersInclude, ec)) {config.includeDirs.push_back(headersInclude);}
    if (fs::exists(headersGlm, ec))     {config.includeDirs.push_back(headersGlm);}
    auto addLink = [&](const std::string &flag) {config.extraLinkFlags.push_back(flag);};
    addLink("-target");
    addLink(target);
    addLink(sysrootFlag);
    if (fs::exists(lld, ec)) {addLink("-fuse-ld=lld");}
    addLink("-s");
    addLink("-lc++_shared");
    addLink("-llog");
    addLink("-lm");
    addLink("-ldl");
    return true;
}
#else
fs::path Engine::ensureOnDeviceToolchain(std::string &error)                         {error = "The On-device script compilation is only available on Android."; return {};}
bool Engine::configureOnDeviceScriptCompile(ScriptBuildConfig &, std::string &error) {error = "The On-device script compilation is only available on Android."; return false;}
#endif // __ANDROID__
