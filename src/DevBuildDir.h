#pragma once

#include <filesystem>
#include <system_error>

// Where the engine's CMake tree and dev-only downloads live under a source checkout.
// build.ps1 keeps them in build/Dev-Resources (next to build/Dev-ExportedBuild and the
// release archive); build.sh and checkouts from before that layout use build/ itself.
// The player-cache, the script host import lib and generated assimp headers resolve through
// here; Android toolchains and APK builds use ModularityAndroidDir below.
inline std::filesystem::path ModularityDevBuildDir(const std::filesystem::path& sourceRoot) {
    std::error_code ec;
    const std::filesystem::path devResources = sourceRoot / "build" / "Dev-Resources";
    if (std::filesystem::is_directory(devResources, ec)) return devResources;
    return sourceRoot / "build";
}

// Android SDK/NDK, Cling/TinyCC caches and APK builds live in <source>/Build-Android, outside
// build/, so the multi-GB toolchains are not mixed into the engine build. Checkouts from before
// that (and build.sh trees) keep `legacyItem` in build/Dev-Resources or build/; the first
// existing location wins, and fresh installs go to Build-Android.
inline std::filesystem::path ModularityAndroidDir(const std::filesystem::path& sourceRoot,
                                                  const char* item, const char* legacyItem = nullptr) {
    std::error_code ec;
    const char* oldName = legacyItem ? legacyItem : item;
    const std::filesystem::path preferred = sourceRoot / "Build-Android" / item;
    for (const std::filesystem::path& candidate : {preferred,
                                                   sourceRoot / "build" / "Dev-Resources" / oldName,
                                                   sourceRoot / "build" / oldName}) {
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return preferred;
}

// Where APKs, their build trees and the debug signing key go (legacy name: "android").
inline std::filesystem::path ModularityAndroidOutputDir(const std::filesystem::path& sourceRoot) {
    return ModularityAndroidDir(sourceRoot, "APK-Output", "android");
}
