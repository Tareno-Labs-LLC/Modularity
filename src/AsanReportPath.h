#pragma once

// AddressSanitizer builds (build.ps1 --asan, built into Build-ASAN/) are GUI-subsystem
// programs, so a report printed to stderr would vanish. Route it beside the exe as
// asan-report.<pid> instead. This has to happen at runtime: MSVC's dynamic ASan runtime
// never calls __asan_default_options (checked with toolset 14.51). A log_path given in
// ASAN_OPTIONS wins. Call first thing in main(); a no-op in normal builds.
#if defined(_WIN32) && defined(__SANITIZE_ADDRESS__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>

extern "C" void __sanitizer_set_report_path(const char* path);

inline void ModularityRouteAsanReports() {
    char env[512] = {};
    if (GetEnvironmentVariableA("ASAN_OPTIONS", env, sizeof(env)) && std::strstr(env, "log_path")) return;
    char path[MAX_PATH + 16] = {};
    DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (len > 0 && path[len - 1] != '\\' && path[len - 1] != '/') --len;
    path[len] = '\0';
    lstrcatA(path, "asan-report");
    __sanitizer_set_report_path(path);
}
#else
inline void ModularityRouteAsanReports() {}
#endif
