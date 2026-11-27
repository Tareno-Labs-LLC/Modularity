#pragma once

#if defined(__linux__) && !defined(__ANDROID__) && !MODULARITY_RUNTIME_ONLY
#include <filesystem>

namespace CodexPluginSetup {
    inline constexpr const char* PackageId = "moduengine.codex-plugin";
    void Open(const std::filesystem::path& project, const std::filesystem::path& package, bool embedded);
    void Draw();
}
#endif
