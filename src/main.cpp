#include "Engine.h"
#include "CrashReporter.h"
#include "ModularityLspServer.h"
#include "ScriptHostExports.h"
#include "AsanReportPath.h"
#include <cstring>
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
// Hybrid-graphics laptops run an executable on the integrated GPU unless the executable itself
// exports these (NVIDIA Optimus and AMD PowerXpress). GLFW can emit them, but only when built with
// GLFW_USE_HYBRID_HPG, which this build does not enable.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#else
#include <sys/prctl.h>
#include <unistd.h>
#endif

static void setProcessDisplayName(int argc, char** argv) {
#if defined(__linux__)
  prctl(PR_SET_NAME, "Modularity", 0, 0, 0);
  if (argc > 0 && argv && argv[0]) {
    constexpr char processName[] = "Modularity";
    const size_t capacity = std::strlen(argv[0]);
    if (capacity >= sizeof(processName) - 1) {
      std::memcpy(argv[0], processName, sizeof(processName));
    }
  }
#else
  (void)argc;
  (void)argv;
#endif
}

static std::filesystem::path getExecutableDir() {
  #if defined(_WIN32)
    char pathBuf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, pathBuf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return {};
    return std::filesystem::path(pathBuf).parent_path();
  #else
    std::vector<char> buf(4096, '\0');
    ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len <= 0) return {};
    buf[static_cast<size_t>(len)] = '\0';
    return std::filesystem::path(buf.data()).parent_path();
  #endif
}
static void logStartupDebug(const char* message) {
  #if defined(NDEBUG)
    (void)message;
  #else
    std::cerr << message << std::endl;
  #endif
}
static bool isLspMode(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = (argv && argv[i]) ? argv[i] : "";
    if (arg == "--lsp") return true;
  }
  return false;
}
static bool isAndroidBuildMode(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = (argv && argv[i]) ? argv[i] : "";
    if (arg == "--build-android") return true;
  }
  return false;
}
static Engine::AndroidBuildRequest androidBuildRequestFromArgs(int argc, char** argv) {
  Engine::AndroidBuildRequest req;
  auto valueAfter = [&](int i) -> std::string {
    return (i + 1 < argc && argv && argv[i + 1]) ? std::string(argv[i + 1]) : std::string();
  };
  for (int i = 1; i < argc; ++i) {
    const std::string arg = (argv && argv[i]) ? argv[i] : "";
    if (arg.rfind("--abi=", 0) == 0)            req.abi = arg.substr(6);
    else if (arg == "--abi")                    req.abi = valueAfter(i);
    else if (arg.rfind("--out-dir=", 0) == 0)   req.outputDir = arg.substr(10);
    else if (arg.rfind("--output=", 0) == 0)    req.outputApk = arg.substr(9);
    else if (arg.rfind("-o=", 0) == 0)          req.outputApk = arg.substr(3);
    else if (arg == "-o" || arg == "--output")  req.outputApk = valueAfter(i);
    else if (arg == "--debug")                  req.debug = true;
    else if (arg == "--editor")                 req.editor = true;
  }
  return req;
}
static std::string startupProjectPathFromArgs(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = (argv && argv[i]) ? argv[i] : "";
    if ((arg == "--project" || arg == "--open") && i + 1 < argc) {return (argv && argv[i + 1]) ? argv[i + 1] : "";}
    if (arg.rfind("--project=", 0) == 0) {return arg.substr(10);}
    if (arg.rfind("--open=", 0) == 0) {return arg.substr(7);}
    if (!arg.empty() && arg[0] != '-') {
      std::filesystem::path path(arg);
      if (path.extension() == ".modu") return arg;
    }
  }
  return "";
}
static int ModularityMain(int argc, char** argv) {
  ModularityRouteAsanReports();
  setProcessDisplayName(argc, argv);
  if (isLspMode(argc, argv)) {
    if (auto exeDir = getExecutableDir(); !exeDir.empty()) {
      std::error_code ec;
      std::filesystem::current_path(exeDir, ec);
    }
    ModularityLspServer lspServer;
    return lspServer.run(argc, argv);
  }
  if (Modularity::CrashReporter::HandleCrashReporterMode(argc, argv)) return 0;
  if (isAndroidBuildMode(argc, argv)) {
    Engine::AndroidBuildRequest req = androidBuildRequestFromArgs(argc, argv);
    req.projectPath = startupProjectPathFromArgs(argc, argv);
    Engine engine;
    std::string error;
    int rc = engine.buildAndroidApkHeadless(req, error);
    if (rc != 0 && !error.empty()) {
      std::cerr << "[Android] Build failed: " << error << std::endl;
    }
    return rc;
  }
  for (int i = 1; i < argc; ++i) {
    const std::string arg = (argv && argv[i]) ? argv[i] : "";
    if (arg != "--build-desktop") continue;
    std::string outDir;
    for (int j = 1; j < argc; ++j) {
      const std::string a = (argv && argv[j]) ? argv[j] : "";
      if (a.rfind("--out-dir=", 0) == 0) outDir = a.substr(10);
    }
    Engine engine;
    std::string error;
    int rc = engine.buildDesktopHeadless(startupProjectPathFromArgs(argc, argv), outDir, error);
    if (rc != 0 && !error.empty()) {
      std::cerr << "[Build] Export failed: " << error << std::endl;
    }
    return rc;
  }
  if (auto exeDir = getExecutableDir(); !exeDir.empty()) {
    std::error_code ec;
    std::filesystem::current_path(exeDir, ec);
    if (ec) std::cerr << "[WARN] Failed to set working dir to executable: " << ec.message() << std::endl;
  }
  const std::string executablePath = (argc > 0 && argv && argv[0]) ? argv[0] : "";
  Modularity::CrashReporter::Initialize("Modularity", executablePath);
  return Modularity::CrashReporter::RunProtected([argc, argv]() -> int {
    logStartupDebug("[DEBUG] Starting engine initialization...");
    Engine engine;
    engine.setStartupProjectPath(startupProjectPathFromArgs(argc, argv));
#if !MODULARITY_RUNTIME_ONLY
    for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--vr-editor") engine.setVREditorMode(true);
    }
#endif
    for (int i = 1; i < argc; ++i) {
      const std::string a = (argv && argv[i]) ? argv[i] : "";
      if (a == "--play") { engine.setAutoPlayDevMode(true); break; }
    }
    logStartupDebug("[DEBUG] Calling engine.init()...");
    if (!engine.init()) {
      logStartupDebug("[DEBUG] Engine init failed!"); return -1;
    }
    logStartupDebug("[DEBUG] Engine init succeeded, starting run loop...");
    engine.run();
    logStartupDebug("[DEBUG] Run loop ended, shutting down...");
    engine.shutdown();
    return 0;
  });
}
int main(int argc, char** argv) {return ModularityMain(argc, argv);}
#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  #if defined(_MSC_VER)
    return ModularityMain(__argc, __argv);
  #else
    return ModularityMain(0, nullptr);
  #endif
}
#endif
