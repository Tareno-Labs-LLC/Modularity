#pragma once
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "DevBuildDir.h"

namespace AndroidToolchain {
namespace fs = std::filesystem;

inline bool ValidAbi(const std::string& abi) {
    return abi == "arm64-v8a" || abi == "armeabi-v7a" || abi == "x86_64" || abi == "x86";
}

inline fs::path EnvironmentPath(const char* name) {
    const char* value = std::getenv(name);
    return value && *value ? fs::path(value) : fs::path{};
}

inline bool Exists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

inline std::vector<fs::path> SdkRoots() {
    std::vector<fs::path> roots;
    for (const char* name : {"ANDROID_SDK_ROOT", "ANDROID_HOME", "ANDROID_SDK"}) {
        if (auto path = EnvironmentPath(name); !path.empty()) roots.push_back(path);
    }
#ifdef MODULARITY_SOURCE_DIR
    roots.emplace_back(ModularityAndroidDir(fs::path(MODULARITY_SOURCE_DIR), "android-sdk"));
#endif
    std::error_code ec;
    const auto cwd = fs::current_path(ec);
    if (!ec) roots.push_back(ModularityAndroidDir(cwd, "android-sdk"));
#ifdef _WIN32
    if (auto local = EnvironmentPath("LOCALAPPDATA"); !local.empty()) roots.push_back(local / "Android" / "Sdk");
#else
    if (auto home = EnvironmentPath("HOME"); !home.empty()) {
        roots.push_back(home / "Android" / "Sdk");
        roots.push_back(home / "Library" / "Android" / "sdk");
    }
#endif
    return roots;
}

inline fs::path FindSdkRoot() {
    for (const auto& root : SdkRoots()) {
        if (Exists(root / "platforms") && Exists(root / "build-tools")) return root;
    }
    return {};
}

// Numeric ordering matters: 30.0 must win over 9.0. Ignore preview packages
// when selecting a default toolchain; an explicit NDK path can still use one.
inline std::vector<int> Version(const fs::path& path) {
    std::vector<int> parts;
    const std::string text = path.filename().string();
    int number = 0;
    bool digit = false;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            if (number > 100000000) return {};
            number = number * 10 + (c - '0');
            digit = true;
        } else if (c == '.' && digit) {
            parts.push_back(number); number = 0; digit = false;
        } else return {};
    }
    if (!digit) return {};
    parts.push_back(number);
    return parts;
}

inline std::vector<fs::path> VersionedDirectories(const fs::path& root) {
    std::vector<fs::path> paths;
    std::error_code ec;
    for (fs::directory_iterator it(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && !Version(it->path()).empty()) paths.push_back(it->path());
    }
    std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) { return Version(a) > Version(b); });
    return paths;
}

inline fs::path FindNdkRoot() {
    for (const char* name : {"ANDROID_NDK_ROOT", "ANDROID_NDK_HOME", "ANDROID_NDK"}) {
        auto root = EnvironmentPath(name);
        if (!root.empty() && Exists(root / "build" / "cmake" / "android.toolchain.cmake")) return root;
    }
    for (const auto& sdk : SdkRoots()) {
        for (const auto& ndk : VersionedDirectories(sdk / "ndk")) {
            if (Exists(ndk / "build" / "cmake" / "android.toolchain.cmake")) return ndk;
        }
        if (Exists(sdk / "ndk-bundle" / "build" / "cmake" / "android.toolchain.cmake")) return sdk / "ndk-bundle";
    }
    return {};
}

inline fs::path FindNinja(const fs::path& sdk) {
#ifdef _WIN32
    const char* name = "ninja.exe";
#else
    const char* name = "ninja";
#endif
    for (const auto& cmake : VersionedDirectories(sdk / "cmake")) {
        if (Exists(cmake / "bin" / name)) return cmake / "bin" / name;
    }
    return {};
}

inline fs::path FindSharedCxxRuntime(const fs::path& ndk, const std::string& abi) {
    std::string triple;
    if (abi == "arm64-v8a") triple = "aarch64-linux-android";
    else if (abi == "armeabi-v7a") triple = "arm-linux-androideabi";
    else if (abi == "x86_64") triple = "x86_64-linux-android";
    else if (abi == "x86") triple = "i686-linux-android";
    else return {};
    for (const char* host : {"windows-x86_64", "linux-x86_64", "darwin-x86_64", "darwin-arm64"}) {
        auto path = ndk / "toolchains/llvm/prebuilt" / host / "sysroot/usr/lib" / triple / "libc++_shared.so";
        if (Exists(path)) return path;
    }
    return {};
}

} // namespace AndroidToolchain
