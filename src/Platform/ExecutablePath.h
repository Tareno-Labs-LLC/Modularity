#pragma once

#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <cstdint>
#include <mach-o/dyld.h>
#include <vector>
#endif

namespace Modularity {

inline std::filesystem::path CurrentExecutablePath() {
#if defined(_WIN32)
    wchar_t path[MAX_PATH] = {};
    const DWORD size = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return size > 0 && size < MAX_PATH ? std::filesystem::path(path) : std::filesystem::path{};
#elif defined(__APPLE__)
    char path[1024];
    uint32_t size = sizeof(path);
    std::filesystem::path executable;
    if (_NSGetExecutablePath(path, &size) == 0) {
        executable = path;
    } else {
        std::vector<char> buffer(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
        executable = buffer.data();
    }

    std::error_code ec;
    auto canonical = std::filesystem::canonical(executable, ec);
    return ec ? executable.lexically_normal() : canonical;
#elif defined(__linux__)
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec);
#else
    return {};
#endif
}

} // namespace Modularity
