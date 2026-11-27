#include "Engine.h"
#include "CrashReporter.h"
#include "ModularityLspServer.h"
#include "ScriptHostExports.h"
#include "AsanReportPath.h"
#include <algorithm>
#include <cstring>
#include <sstream>
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
#elif defined(__linux__)
#include <sys/prctl.h>
#endif

#include "Platform/ExecutablePath.h"

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
#if !MODULARITY_RUNTIME_ONLY
// --autoshot <out.png> [--autoshot-delay <s>] [--autoshot-view game|scene]
// [--autoshot-camera px,py,pz,tx,ty,tz] [--autoshot-size WxH] [--autoshot-scene <scene>]
// Parsed before the working directory moves to the executable's folder so a relative output path
// means "relative to where the command was run".
static bool parseFloatList(const std::string& text, float* out, int count) {
  std::string cleaned = text;
  for (char& c : cleaned) if (c == ',' || c == ';') c = ' ';
  std::istringstream in(cleaned);
  for (int i = 0; i < count; ++i) {
    if (!(in >> out[i])) return false;
  }
  return true;
}
static Engine::AutoShotConfig autoShotConfigFromArgs(int argc, char** argv) {
  Engine::AutoShotConfig cfg;
  auto valueOf = [&](int& i, const std::string& arg, const char* name, std::string& out) -> bool {
    const std::string flag = std::string("--") + name;
    if (arg == flag) {
      if (i + 1 >= argc || !argv[i + 1]) return false;
      out = argv[++i];
      return true;
    }
    if (arg.rfind(flag + "=", 0) == 0) {
      out = arg.substr(flag.size() + 1);
      return true;
    }
    return false;
  };
  for (int i = 1; i < argc; ++i) {
    const std::string arg = (argv && argv[i]) ? argv[i] : "";
    std::string v;
    if (valueOf(i, arg, "autoshot-delay", v)) {
      try { cfg.delaySeconds = std::max(0.0, std::stod(v)); } catch (...) {}
    } else if (valueOf(i, arg, "autoshot-view", v)) {
      cfg.sceneView = (v == "scene" || v == "editor");
    } else if (valueOf(i, arg, "autoshot-camera", v)) {
      float f[6];
      if (parseFloatList(v, f, 6)) {
        cfg.hasCamera = true;
        cfg.sceneView = true;
        cfg.cameraPosition = glm::vec3(f[0], f[1], f[2]);
        cfg.cameraTarget = glm::vec3(f[3], f[4], f[5]);
      } else {
        std::cerr << "[AUTOSHOT] ignoring malformed --autoshot-camera (want px,py,pz,tx,ty,tz)" << std::endl;
      }
    } else if (valueOf(i, arg, "autoshot-size", v)) {
      int w = 0, h = 0;
      char sep = 0;
      std::istringstream in(v);
      if ((in >> w >> sep >> h) && (sep == 'x' || sep == 'X') && w > 0 && h > 0) {
        cfg.width = w;
        cfg.height = h;
      } else {
        std::cerr << "[AUTOSHOT] ignoring malformed --autoshot-size (want WxH)" << std::endl;
      }
    } else if (valueOf(i, arg, "autoshot-scene", v)) {
      std::string scene = std::filesystem::path(v).generic_string();
      const std::string marker = "Assets/Scenes/";
      const size_t at = scene.find(marker);
      if (at != std::string::npos) scene = scene.substr(at + marker.size());
      else scene = std::filesystem::path(scene).filename().generic_string();
      if (scene.size() > 6 && scene.compare(scene.size() - 6, 6, ".scene") == 0) scene.resize(scene.size() - 6);
      cfg.sceneName = scene;
    } else if (valueOf(i, arg, "autoshot", v)) {
      std::error_code ec;
      std::filesystem::path out(v);
      if (out.is_relative()) out = std::filesystem::absolute(out, ec);
      cfg.outPath = out.lexically_normal().string();
      cfg.enabled = !cfg.outPath.empty();
    }
  }
  return cfg;
}
#endif
static int ModularityMain(int argc, char** argv) {
  ModularityRouteAsanReports();
  setProcessDisplayName(argc, argv);
  if (isLspMode(argc, argv)) {
    if (auto exeDir = Modularity::CurrentExecutablePath().parent_path(); !exeDir.empty()) {
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
#if !MODULARITY_RUNTIME_ONLY
  const Engine::AutoShotConfig autoShotConfig = autoShotConfigFromArgs(argc, argv);
#endif
  if (auto exeDir = Modularity::CurrentExecutablePath().parent_path(); !exeDir.empty()) {
    std::error_code ec;
    std::filesystem::current_path(exeDir, ec);
    if (ec) std::cerr << "[WARN] Failed to set working dir to executable: " << ec.message() << std::endl;
  }
  const std::string executablePath = (argc > 0 && argv && argv[0]) ? argv[0] : "";
  Modularity::CrashReporter::Initialize("Modularity", executablePath);
  return Modularity::CrashReporter::RunProtected([argc, argv
#if !MODULARITY_RUNTIME_ONLY
                                                   , &autoShotConfig
#endif
                                                   ]() -> int {
    logStartupDebug("[DEBUG] Starting engine initialization...");
    Engine engine;
    engine.setStartupProjectPath(startupProjectPathFromArgs(argc, argv));
#if !MODULARITY_RUNTIME_ONLY
    for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--vr-editor") engine.setVREditorMode(true);
    }
    if (autoShotConfig.enabled) engine.setAutoShotConfig(autoShotConfig);
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
#if !MODULARITY_RUNTIME_ONLY
    return engine.autoShotExitCode();
#else
    return 0;
#endif
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
