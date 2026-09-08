#include "ModularityLspServer.h"
#include <filesystem>
#include <string>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif
static std::filesystem::path getExecutableDir() {
#if defined(_WIN32)
    char pathBuf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, pathBuf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return {};
    return std::filesystem::path(pathBuf).parent_path();
#else
    std::string buf(4096, '\0');
    ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len <= 0) return {};
    buf[static_cast<size_t>(len)] = '\0';
    return std::filesystem::path(buf.c_str()).parent_path();
#endif
}
int main(int argc, char** argv) {
    if (auto exeDir = getExecutableDir(); !exeDir.empty()) {std::error_code ec; std::filesystem::current_path(exeDir, ec);
    }
    ModularityLspServer lspServer;
    return lspServer.run(argc, argv);
}
