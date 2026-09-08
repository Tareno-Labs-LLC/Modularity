#include "Engine.h"
#include "CrashReporter.h"
#include "ScriptHostExports.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if defined(_MSC_VER)
extern int __argc;
extern char** __argv;
#endif
#else
#include <unistd.h>
#endif
static std::filesystem::path getExecutableDir() {
  #if defined(_WIN32)
    char pathBuf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, pathBuf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {return {};}
    return std::filesystem::path(pathBuf).parent_path();
  #else
    std::vector<char> buf(4096, '\0');
    ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len <= 0) {return {};}
    buf[static_cast<size_t>(len)] = '\0';
    return std::filesystem::path(buf.data()).parent_path();
  #endif
}
static int ModularityPlayerMain(int argc, char** argv) {
  if (Modularity::CrashReporter::HandleCrashReporterMode(argc, argv)) return 0;
  if (auto exeDir = getExecutableDir(); !exeDir.empty()) {
    std::error_code ec;
    std::filesystem::current_path(exeDir, ec);
    if (ec) std::cerr << "[WARN] Failed to set working dir to executable: " << ec.message() << std::endl;
  }
  const std::string executablePath = (argc > 0 && argv && argv[0]) ? argv[0] : "";
  Modularity::CrashReporter::Initialize("Modularity Player", executablePath);
  return Modularity::CrashReporter::RunProtected([]() -> int {
    Engine engine;
    if (!engine.init()) {return -1;}
    engine.run();
    engine.shutdown();
    return 0;
  });
}
int main(int argc, char** argv) {return ModularityPlayerMain(argc, argv);}
#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  #if defined(_MSC_VER)
    return ModularityPlayerMain(__argc, __argv);
  #else
    return ModularityPlayerMain(0, nullptr);
  #endif
}
#endif